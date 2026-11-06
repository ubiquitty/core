// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_daemon_core_rules.cpp — the Core rules a daemon session's
// owner installs (Platform/Host/Daemon.cpp, installTerminalClients and
// servicePTY).
//
// Core defines a terminal's clock, who answers its program's queries and when
// a frame is coherent; the daemon is the owner that has to install them. Each
// case drives the daemon's own pass over a scripted PTY, so a rule Core keeps
// that the daemon never installs fails here rather than passing in Core's unit
// tests alone. The coherence cases replay the Sync PTY corpus through
// servicePTY and publishFrameIfAdmitted, which the corpus oracle otherwise
// measures only through the loopback link's copy of the daemon's loop.
//

#include "Harness.h"
#include "Platform/StreamDouble.h"
#include "SyncCorpus.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/PublishCensus.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// Whether a replay asks the coherence rule before taking a frame.
enum class Gate : bool {
    /// A frame waits for the rule, as the loop takes one.
    Asked,
    /// A frame is taken after every pass, as a daemon that never asked would.
    Skipped,
};

/// One frame the daemon took during a replay.
struct DaemonFrame {
    std::uint64_t atMicroseconds = 0;
    /// The bytes of the stream the session had parsed when it was taken.
    std::size_t streamOffset = 0;
    Protocol::Sync::CensusVerdict census;
    /// The visible cells it showed, row-major.
    std::vector<Cell> grid;
};

struct DaemonCoreRulesAccess {
    // A session holding a scripted PTY, given its Terminal the way every
    // daemon path gives one.
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, int rows, int columns, TestDoubles::StreamDouble*& stream, std::unique_ptr<Terminal> terminal = {})
    {
        auto session = std::make_unique<Daemon::Session>(d.m_capabilityDecisions);
        session->id = id;
        session->term = terminal ? std::move(terminal) : std::make_unique<Terminal>();
        resize(*session->term, rows, columns);
        Daemon::installTerminalClients(*session);
        auto owned = std::make_unique<TestDoubles::StreamDouble>();
        stream = owned.get();
        session->stream = std::move(owned);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 7 });
        Daemon::Session& added = *session;
        d.m_sessions.push_back(std::move(session));
        return added;
    }

    static void setMonotonicClock(Daemon& d, std::uint64_t (*clock)()) { d.m_monotonicMicroseconds = clock; }
    static void servicePTY(Daemon& d, Daemon::Session& s) { (void)d.servicePTY(s); }
    static Terminal& terminalOf(Daemon::Session& s) { return *s.term; }

    /// Replays a corpus entry through the daemon's own pass and publish, on
    /// the daemon clock `clock` points at.
    ///
    /// - Parameters:
    ///   - entry: The recorded reads.
    ///   - clock: The scripted instant the daemon reads.
    ///   - readClock: Reads `clock`, for the daemon.
    ///   - gate: Whether frames wait for the coherence rule.
    /// - Returns: Every frame taken, in order.
    static std::vector<DaemonFrame> replay(const UbiquittyTest::CorpusEntry& entry, std::uint64_t& clock, std::uint64_t (*readClock)(), Gate gate)
    {
        Daemon d;
        d.m_monotonicMicroseconds = readClock;
        TestDoubles::StreamDouble* stream = nullptr;
        Daemon::Session& s = addSession(d, 1, entry.rows, entry.columns, stream);
        // A pane a connection reads, so its frames wait for the rule.
        Daemon::Client reader {};
        reader.clientID = 1;
        reader.paneSubscriptions.push_back({ .sessionID = s.id });
        d.m_clients.push_back(std::move(reader));

        std::vector<DaemonFrame> frames;
        std::size_t parsed = 0;
        const auto take = [&] {
            std::optional<Protocol::Sync::CensusVerdict> verdict;
            if (gate == Gate::Asked)
                verdict = d.publishFrameIfAdmitted(s);
            else if (s.isFrameDue) {
                verdict = s.census.notePublish(*s.term, clock, Runtime::FrameClass::Settled);
                d.publishFrame(s, Runtime::FrameClass::Settled);
            }
            if (verdict)
                frames.push_back({ clock, parsed, *verdict, visibleGrid(*s.term) });
        };
        // The loop's wakes until `until`: the rule says when to ask next.
        const auto askUntil = [&](std::uint64_t until) {
            for (int wake = 0; wake < 100000 && s.isFrameDue && s.coherence.isDirty(); ++wake) {
                const std::optional<std::uint64_t> wait = s.coherence.microsecondsUntilAsk(*s.term, clock, Ticks::fromMilliseconds(clock / 1000));
                if (!wait || clock + std::max<std::uint64_t>(*wait, 1) >= until)
                    return;
                clock += std::max<std::uint64_t>(*wait, 1);
                take();
            }
        };

        clock = entry.reads.empty() ? 0 : entry.reads.front().atMicroseconds;
        std::size_t index = 0;
        while (index < entry.reads.size()) {
            askUntil(entry.reads[index].atMicroseconds);
            // One pass: the reads the PTY held before its next would-block.
            std::uint64_t passEnd = clock;
            for (;;) {
                const UbiquittyTest::CorpusRead& read = entry.reads[index++];
                passEnd = std::max(passEnd, read.atMicroseconds);
                stream->pushInbound(std::string_view(entry.bytes).substr(read.offset, read.length));
                if (read.isFollowedByWouldBlock || index >= entry.reads.size())
                    break;
            }
            clock = passEnd;
            // The descriptor stays readable past a pass's byte budget, so the
            // loop services it again before it asks for a frame.
            for (int pass = 0; pass < 1024; ++pass) {
                const Runtime::DrainPassResult result = d.servicePTY(s);
                parsed += static_cast<std::size_t>(result.bytesDrained);
                if (result.didCatchUp || !result.bytesDrained)
                    break;
            }
            // A scripted descriptor never polls readable, so an ask with the
            // next read already due is the loop's next turn, after that read.
            if (index < entry.reads.size() && entry.reads[index].atMicroseconds <= clock)
                continue;
            take();
        }
        askUntil(UINT64_MAX);
        return frames;
    }

    /// Returns a terminal's visible cells, row-major.
    static std::vector<Cell> visibleGrid(const Terminal& terminal)
    {
        std::vector<Cell> cells;
        cells.reserve(static_cast<std::size_t>(terminal.rows) * static_cast<std::size_t>(terminal.columns));
        for (int row = 0; row < terminal.rows; ++row) {
            for (int column = 0; column < terminal.columns; ++column) {
                const Cell* cell = Screen::visibleCell(&terminal, 0, row, column);
                cells.push_back(cell ? *cell : Cell {});
            }
        }
        return cells;
    }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::DaemonFrame;
using Ubiquitty::Runtime::FrameClass;
using Access = Ubiquitty::Host::DaemonCoreRulesAccess;
using Ubiquitty::TestDoubles::StreamDouble;
using UbiquittyTest::CorpusEntry;

namespace {

// The daemon clock a case sets; read through a plain function, as the daemon's
// own clock is.
std::uint64_t scriptedMicroseconds = 0;
std::uint64_t scriptedClock() { return scriptedMicroseconds; }

std::uint32_t lastCommandTicks(const Ubiquitty::Terminal& terminal)
{
    return terminal.commandDurationTicksRing[(terminal.commandCompletedGeneration + 7) % 8];
}

std::vector<CorpusEntry> loadCorpus()
{
    std::string error;
    auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    CHECK_MSG(corpus.has_value(), error.c_str());
    return corpus ? std::move(*corpus) : std::vector<CorpusEntry> {};
}

std::vector<DaemonFrame> replayThroughDaemon(const CorpusEntry& entry, Ubiquitty::Host::Gate gate = Ubiquitty::Host::Gate::Asked)
{
    return Access::replay(entry, scriptedMicroseconds, &scriptedClock, gate);
}

bool isSameGrid(const std::vector<Ubiquitty::Cell>& a, const std::vector<Ubiquitty::Cell>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Ubiquitty::Cell)) == 0;
}

// Counts the frames that show neither the screen before a marked redraw nor
// the one after it, against a terminal fed the stream whole: a parser resumes
// at any byte, so the grid at an offset does not depend on how reads cut it.
std::size_t halfDrawnFrames(const CorpusEntry& entry, const std::vector<DaemonFrame>& frames)
{
    std::vector<std::size_t> boundaries;
    for (const UbiquittyTest::RedrawSpan& span : entry.redraws) {
        boundaries.push_back(span.begin);
        boundaries.push_back(span.end);
    }
    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::vector<std::pair<std::size_t, std::vector<Ubiquitty::Cell>>> gridAt;
    Ubiquitty::Terminal whole;
    Ubiquitty::resize(whole, entry.rows, entry.columns);
    std::size_t fed = 0;
    for (const std::size_t boundary : boundaries) {
        Ubiquitty::processBytes(&whole, std::string_view(entry.bytes).substr(fed, boundary - fed));
        fed = boundary;
        gridAt.emplace_back(boundary, Access::visibleGrid(whole));
    }
    const auto at = [&gridAt](std::size_t offset) -> const std::vector<Ubiquitty::Cell>& {
        return std::ranges::find_if(gridAt, [offset](const auto& pair) { return pair.first == offset; })->second;
    };
    std::size_t halfDrawn = 0;
    for (const DaemonFrame& frame : frames) {
        for (const UbiquittyTest::RedrawSpan& span : entry.redraws) {
            if (frame.streamOffset > span.begin && frame.streamOffset < span.end && !isSameGrid(frame.grid, at(span.begin)) && !isSameGrid(frame.grid, at(span.end)))
                ++halfDrawn;
        }
    }
    return halfDrawn;
}

std::uint64_t percentile(std::vector<std::uint64_t> values, int percent)
{
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    const std::size_t index = (values.size() * static_cast<std::size_t>(percent) + 99) / 100;
    return values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)];
}

// How long each key the entry records waited for the first frame showing its
// echo: the first read at or after the key.
std::vector<std::uint64_t> inputHolds(const CorpusEntry& entry, const std::vector<DaemonFrame>& frames)
{
    std::vector<std::uint64_t> holds;
    for (const std::uint64_t key : entry.inputs) {
        const auto echo = std::ranges::find_if(entry.reads, [key](const UbiquittyTest::CorpusRead& read) { return read.atMicroseconds >= key; });
        if (echo == entry.reads.end())
            continue;
        const std::size_t echoEnd = echo->offset + echo->length;
        const auto shown = std::ranges::find_if(frames, [echoEnd](const DaemonFrame& frame) { return frame.streamOffset >= echoEnd; });
        if (shown != frames.end())
            holds.push_back(shown->atMicroseconds - key);
    }
    return holds;
}

} // namespace

// A command is timed by when its marks were read, on the daemon's clock, and
// two sessions in one daemon each by their own reads.
TEST(twoSessionsTimeTheirCommandsByWhenTheDaemonReadTheirMarks)
{
    Daemon daemon;
    Access::setMonotonicClock(daemon, &scriptedClock);
    StreamDouble* leftStream = nullptr;
    StreamDouble* rightStream = nullptr;
    auto& left = Access::addSession(daemon, 1, 10, 40, leftStream);
    auto& right = Access::addSession(daemon, 2, 10, 40, rightStream);

    scriptedMicroseconds = 1'000'000;
    leftStream->pushInbound("\033]133;C\007");
    Access::servicePTY(daemon, left);
    scriptedMicroseconds = 1'500'000;
    rightStream->pushInbound("\033]133;C\007");
    Access::servicePTY(daemon, right);
    scriptedMicroseconds = 2'000'000;
    leftStream->pushInbound("\033]133;D;0\007");
    Access::servicePTY(daemon, left);
    scriptedMicroseconds = 4'500'000;
    rightStream->pushInbound("\033]133;D;0\007");
    Access::servicePTY(daemon, right);

    CHECK_EQ(lastCommandTicks(Access::terminalOf(left)), 60u);
    CHECK_EQ(lastCommandTicks(Access::terminalOf(right)), 180u);
}

// The host answers its sessions' queries even for a terminal that arrived
// as a replica's, and the answer reaches the program through the pass that
// read the query.
TEST(theHostAnswersAQueryForATerminalThatHeldAReplicasAuthority)
{
    Daemon daemon;
    auto deferring = std::make_unique<Ubiquitty::Terminal>();
    deferring->replyAuthority = Ubiquitty::ReplyAuthority::Elsewhere;
    StreamDouble* stream = nullptr;
    auto& session = Access::addSession(daemon, 1, 10, 40, stream, std::move(deferring));
    CHECK(Access::terminalOf(session).replyAuthority == Ubiquitty::ReplyAuthority::ThisTerminal);
    stream->pushInbound("\033[c");
    Access::servicePTY(daemon, session);
    CHECK_MSG(stream->written.starts_with("\033[?"), ("the program read: " + stream->written).c_str());
}

// A cursor-hidden repaint split across reads is one frame through the
// daemon's own pass: every frame it takes shows the screen before the repaint
// or after it. The same replay taking a frame after every pass shows a
// half-drawn one, so the case sees what the rule holds.
TEST(theDaemonNeverPublishesAHalfDrawnCursorHiddenRedraw)
{
    const std::vector<CorpusEntry> corpus = loadCorpus();
    const auto entry = std::ranges::find_if(corpus, [](const CorpusEntry& candidate) { return candidate.name == "cursor-hidden-repaint"; });
    CHECK_MSG(entry != corpus.end(), "cursor-hidden-repaint is not in the corpus");
    if (entry == corpus.end())
        return;
    CHECK_MSG(!entry->redraws.empty(), "the entry marks no redraw");
    const std::size_t gated = halfDrawnFrames(*entry, replayThroughDaemon(*entry));
    CHECK_MSG(gated == 0, (std::to_string(gated) + " frames the daemon took show a half-drawn repaint").c_str());
    CHECK_MSG(halfDrawnFrames(*entry, replayThroughDaemon(*entry, Ubiquitty::Host::Gate::Skipped)) > 0, "without the rule no frame is half-drawn: the case cannot see the gate");
}

// The gates Sync ships the coherence rule behind, measured through the
// daemon: a forced share at or below 1%, a p99 hold after input at or below
// one frame, no coherent frame inside a cursor-hidden bracket or a split
// repaint, and no frame the census saw incoherent that the rule left out of
// the forced share.
TEST(theCoherenceGatesHoldThroughTheDaemonsOwnPass)
{
    std::uint64_t frames = 0;
    std::uint64_t forced = 0;
    std::vector<std::uint64_t> holds;
    std::string torn;
    for (const CorpusEntry& entry : loadCorpus()) {
        const std::vector<DaemonFrame> taken = replayThroughDaemon(entry);
        frames += taken.size();
        std::size_t bracketed = 0;
        std::size_t split = 0;
        std::size_t unreported = 0;
        for (const DaemonFrame& frame : taken) {
            const bool isForced = frame.census.frameClass == FrameClass::Forced;
            forced += isForced ? 1 : 0;
            bracketed += frame.census.isInsideRedrawBracket && !isForced ? 1 : 0;
            split += entry.isInsideRedraw(frame.streamOffset) ? 1 : 0;
            unreported += frame.census.isIncoherent && !isForced && frame.census.frameClass != FrameClass::Structural ? 1 : 0;
        }
        if (bracketed + split + unreported > 0)
            torn += " " + entry.name + " (" + std::to_string(bracketed) + " in a bracket, " + std::to_string(split) + " in a split redraw, " + std::to_string(unreported) + " incoherent and not forced)";
        const std::vector<std::uint64_t> entryHolds = inputHolds(entry, taken);
        holds.insert(holds.end(), entryHolds.begin(), entryHolds.end());
    }
    CHECK_MSG(frames > 0 && !holds.empty(), "the corpus replayed no frames or records no input");
    CHECK_MSG(forced * 100 <= frames, ("forced " + std::to_string(forced) + " of " + std::to_string(frames) + " frames").c_str());
    const std::uint64_t p99 = percentile(holds, 99);
    CHECK_MSG(p99 <= Ubiquitty::Runtime::refreshIntervalMicroseconds, ("p99 hold after input is " + std::to_string(p99) + " us").c_str());
    CHECK_MSG(torn.empty(), ("frames torn:" + torn).c_str());
}

UBIQUITTY_TEST_MAIN()
