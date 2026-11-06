// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_corpus_oracle.cpp — the Sync wire, over the whole corpus.
//
// Every entry of Tests/Corpus/sync is replayed through LoopbackPaneLink: the
// coherence rule, the host end of a Sync connection and a client end
// applying what it sends to a passive client. The client must hold the
// host's screen and history after every frame.
//
// The same replay is the measurement. With UBQ_SYNC_CORPUS_REPORT set, it
// prints, per entry, the frames by census class, the frames that landed inside
// a cursor-hidden redraw or between two reads of one marked redraw (the tears a
// coherence gate would hold), the bytes per frame, and the hold times:
//
//     UBQ_SYNC_CORPUS_REPORT=1 <build>/Tests/test_sync_corpus_oracle
//
// Docs/sync-measurements.md is that output.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncCorpus.h"

#include "Core/Protocol/Sync/PublishCensus.h"
#include "Core/Runtime/FrameCoherence.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Ubiquitty::Runtime::FrameClass;
using UbiquittyTest::CorpusEntry;
using UbiquittyTest::LoopbackOptions;
using UbiquittyTest::LoopbackPaneLink;
using UbiquittyTest::PublishedFrame;

namespace {

void checkFaithful(const CorpusEntry& entry, const LoopbackPaneLink& link)
{
    CHECK_MSG(link.failure().empty(), (entry.name + ": " + link.failure()).c_str());
}

std::vector<CorpusEntry> loadOrFail()
{
    std::string error;
    auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    CHECK_MSG(corpus.has_value(), error.c_str());
    return corpus ? std::move(*corpus) : std::vector<CorpusEntry> {};
}

std::uint64_t percentile(std::vector<std::uint64_t> values, int percent)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const std::size_t index = (values.size() * static_cast<std::size_t>(percent) + 99) / 100;
    return values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)];
}

std::vector<std::uint64_t> wireBytes(const LoopbackPaneLink& link)
{
    std::vector<std::uint64_t> sizes;
    for (const PublishedFrame& frame : link.frames())
        sizes.push_back(frame.wireBytes);
    return sizes;
}

std::uint64_t mean(const std::vector<std::uint64_t>& values)
{
    std::uint64_t total = 0;
    for (const std::uint64_t value : values)
        total += value;
    return values.empty() ? 0 : total / values.size();
}

void report(const CorpusEntry& entry, const LoopbackPaneLink& link)
{
    const std::vector<PublishedFrame>& frames = link.frames();
    std::size_t splitRedraw = 0;
    std::size_t updates = 0;
    std::vector<std::uint64_t> holds;
    for (const PublishedFrame& frame : frames) {
        splitRedraw += entry.isInsideRedraw(frame.streamOffset) ? 1 : 0;
        updates += frame.isCheckpoint ? 0 : 1;
        holds.push_back(frame.census.holdMicroseconds);
    }
    const auto& census = link.census();
    const std::vector<std::uint64_t> sizes = wireBytes(link);
    std::printf("| %s | %zu | %llu | %llu | %llu | %llu | %llu | %llu | %zu | %zu/%zu | %zu | %llu (%llu) | %.2f | %.2f | %.2f |\n",
        entry.name.c_str(), entry.bytes.size(),
        static_cast<unsigned long long>(frames.size()),
        static_cast<unsigned long long>(census.frameCount(FrameClass::Boundary)),
        static_cast<unsigned long long>(census.frameCount(FrameClass::Settled)),
        static_cast<unsigned long long>(census.frameCount(FrameClass::Flood)),
        static_cast<unsigned long long>(census.frameCount(FrameClass::Forced)),
        static_cast<unsigned long long>(census.frameCount(FrameClass::Structural)),
        static_cast<std::size_t>(census.redrawBracketFrameCount()),
        splitRedraw, entry.redraws.size(),
        updates,
        static_cast<unsigned long long>(mean(sizes)), static_cast<unsigned long long>(percentile(sizes, 50)),
        static_cast<double>(percentile(holds, 50)) / 1000.0,
        static_cast<double>(percentile(holds, 99)) / 1000.0,
        static_cast<double>(census.longestHoldMicroseconds()) / 1000.0);
}

// One frame of the refresh the coherence gates measure holds against.
constexpr std::uint64_t frameIntervalMicroseconds = Ubiquitty::Runtime::refreshIntervalMicroseconds;

/// Returns how long each key the entry records waited for the frame that
/// first showed its echo.
///
/// The echo is the first read at or after the key; the frame that shows it is
/// the first one published with the host past that read's last byte.
std::vector<std::uint64_t> inputHolds(const CorpusEntry& entry, const LoopbackPaneLink& link)
{
    std::vector<std::uint64_t> holds;
    const std::vector<PublishedFrame>& frames = link.frames();
    for (const std::uint64_t key : entry.inputs) {
        const auto echo = std::ranges::find_if(entry.reads, [key](const UbiquittyTest::CorpusRead& read) { return read.atMicroseconds >= key; });
        if (echo == entry.reads.end())
            continue;
        const std::size_t echoEnd = echo->offset + echo->length;
        const auto shown = std::ranges::find_if(frames, [echoEnd](const PublishedFrame& frame) { return frame.streamOffset >= echoEnd; });
        if (shown != frames.end())
            holds.push_back(shown->atMicroseconds - key);
    }
    return holds;
}

/// Returns how many frames a second the link published while the entry's
/// writer was writing, from its first read to its last.
double framesPerSecondWhileWriting(const CorpusEntry& entry, const LoopbackPaneLink& link)
{
    if (entry.reads.size() < 2)
        return 0.0;
    const std::uint64_t first = entry.reads.front().atMicroseconds;
    const std::uint64_t last = entry.reads.back().atMicroseconds;
    const auto during = static_cast<std::size_t>(std::ranges::count_if(link.frames(), [first, last](const PublishedFrame& frame) {
        return frame.atMicroseconds > first && frame.atMicroseconds <= last;
    }));
    return static_cast<double>(during) * 1'000'000.0 / static_cast<double>(last - first);
}

/// Counts the frames published part way through a marked redraw.
std::size_t framesInsideSplitRedraws(const CorpusEntry& entry, const LoopbackPaneLink& link)
{
    return static_cast<std::size_t>(std::ranges::count_if(link.frames(), [&entry](const PublishedFrame& frame) { return entry.isInsideRedraw(frame.streamOffset); }));
}

/// Counts the frames taken inside a cursor-hidden redraw that the gate called
/// coherent; a forced frame is the deadline's, counted against the forced share.
std::size_t coherentFramesInsideBrackets(const LoopbackPaneLink& link)
{
    return static_cast<std::size_t>(std::ranges::count_if(link.frames(), [](const PublishedFrame& frame) {
        return frame.census.isInsideRedrawBracket && frame.census.frameClass != FrameClass::Forced;
    }));
}

/// Counts the frames the census saw taken mid-sequence or inside an open 2026
/// block that the rule did not class as forced. The forced share counts the
/// rule's own classes, so a frame the rule misclassed would leave the share
/// low while the screen tore; this count sees the terminal instead.
std::size_t incoherentFramesNotForced(const LoopbackPaneLink& link)
{
    return static_cast<std::size_t>(std::ranges::count_if(link.frames(), [](const PublishedFrame& frame) {
        return frame.census.isIncoherent && frame.census.frameClass != FrameClass::Forced && frame.census.frameClass != FrameClass::Structural;
    }));
}

/// Returns the visible cells of a terminal, row-major.
std::vector<Ubiquitty::Cell> visibleGrid(const Ubiquitty::Terminal& terminal)
{
    std::vector<Ubiquitty::Cell> cells;
    cells.reserve(static_cast<std::size_t>(terminal.rows) * static_cast<std::size_t>(terminal.columns));
    for (int r = 0; r < terminal.rows; ++r) {
        for (int c = 0; c < terminal.columns; ++c) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, r, c);
            cells.push_back(cell ? *cell : Ubiquitty::Cell {});
        }
    }
    return cells;
}

bool isSameGrid(const std::vector<Ubiquitty::Cell>& a, const std::vector<Ubiquitty::Cell>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Ubiquitty::Cell)) == 0;
}

const CorpusEntry* findEntry(const std::vector<CorpusEntry>& corpus, std::string_view name)
{
    const auto found = std::ranges::find_if(corpus, [name](const CorpusEntry& entry) { return entry.name == name; });
    CHECK_MSG(found != corpus.end(), (std::string(name) + " is not in the corpus").c_str());
    return found == corpus.end() ? nullptr : &*found;
}

} // namespace

// A non-2026 program that hides the cursor, repaints across several reads with
// would-blocks between and shows the cursor again has drawn one frame, not
// three. Every frame the host publishes while the repaint is under way must
// show the screen from before it or the screen after it; anything else is a
// screen the program never drew.
TEST(aCursorHiddenRedrawSplitAcrossReadsPublishesNoHalfDrawnScreen)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    const CorpusEntry* entry = findEntry(corpus, "cursor-hidden-repaint");
    if (!entry)
        return;
    CHECK_MSG(!entry->redraws.empty(), "the entry marks no redraw");

    // The screen at each redraw's first and last byte, from a host fed the
    // stream whole: a parser resumes at any byte, so the grid at an offset does
    // not depend on how the reads cut it.
    std::vector<std::size_t> boundaries;
    for (const UbiquittyTest::RedrawSpan& span : entry->redraws) {
        boundaries.push_back(span.begin);
        boundaries.push_back(span.end);
    }
    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::vector<std::pair<std::size_t, std::vector<Ubiquitty::Cell>>> gridAt;
    {
        LoopbackPaneLink reference(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
        std::size_t fed = 0;
        for (const std::size_t boundary : boundaries) {
            reference.feedHost(std::string_view(entry->bytes).substr(fed, boundary - fed));
            fed = boundary;
            gridAt.emplace_back(boundary, visibleGrid(reference.host()));
        }
    }
    const auto gridAtOffset = [&gridAt](std::size_t offset) -> const std::vector<Ubiquitty::Cell>& {
        return std::ranges::find_if(gridAt, [offset](const auto& pair) { return pair.first == offset; })->second;
    };

    LoopbackPaneLink link(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
    std::size_t halfDrawn = 0;
    std::string firstHalfDrawn;
    link.onFrame = [&](const PublishedFrame& frame, const LoopbackPaneLink& published) {
        for (const UbiquittyTest::RedrawSpan& span : entry->redraws) {
            if (frame.streamOffset <= span.begin || frame.streamOffset >= span.end)
                continue;
            const std::vector<Ubiquitty::Cell> shown = visibleGrid(published.client());
            if (isSameGrid(shown, gridAtOffset(span.begin)) || isSameGrid(shown, gridAtOffset(span.end)))
                continue;
            if (halfDrawn++ == 0)
                firstHalfDrawn = "frame generation " + std::to_string(frame.generation) + " at stream offset " + std::to_string(frame.streamOffset)
                    + " shows neither the screen before the redraw at " + std::to_string(span.begin) + " nor the one after it at " + std::to_string(span.end);
        }
    };
    link.replay(entry->ptyReads());
    checkFaithful(*entry, link);
    CHECK_MSG(halfDrawn == 0, (std::to_string(halfDrawn) + " published frames show a half-drawn repaint; the first: " + firstHalfDrawn).c_str());
}

// An Ink-style agent UI keeps the cursor hidden throughout, so no bracket ever
// holds it; its repaint split across two reads is held by read quiescence alone.
TEST(anInkSpinnerRepaintSplitAcrossTwoReadsIsNeverPublishedHalfDrawn)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    const CorpusEntry* entry = findEntry(corpus, "ink-spinner");
    if (!entry)
        return;
    LoopbackPaneLink link(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
    link.replay(entry->ptyReads());
    checkFaithful(*entry, link);
    const std::size_t inside = framesInsideSplitRedraws(*entry, link);
    CHECK_MSG(inside == 0, (std::to_string(inside) + " frames were published inside a split redraw").c_str());
}

// The gates Sync ships the coherence rule behind, over the whole corpus: a
// forced share at or below 1%, a p99 hold after input at or below one frame,
// no coherent frame inside a cursor-hidden redraw or a split repaint, and no
// frame the census sees taken mid-sequence or inside a 2026 block that the
// rule left out of the forced share.
TEST(theCoherenceGatesHoldOverTheWholeCorpus)
{
    std::uint64_t frames = 0;
    std::uint64_t forced = 0;
    std::size_t bracketed = 0;
    std::size_t split = 0;
    std::size_t unreported = 0;
    std::vector<std::uint64_t> holds;
    std::string where;
    for (const CorpusEntry& entry : loadOrFail()) {
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        checkFaithful(entry, link);
        frames += link.frames().size();
        forced += link.census().frameCount(FrameClass::Forced);
        const std::size_t entryBracketed = coherentFramesInsideBrackets(link);
        const std::size_t entrySplit = framesInsideSplitRedraws(entry, link);
        const std::size_t entryUnreported = incoherentFramesNotForced(link);
        if (entryBracketed + entrySplit + entryUnreported > 0)
            where += " " + entry.name + " (" + std::to_string(entryBracketed) + " in a bracket, " + std::to_string(entrySplit) + " in a split redraw, "
                + std::to_string(entryUnreported) + " incoherent and not forced)";
        unreported += entryUnreported;
        bracketed += entryBracketed;
        split += entrySplit;
        const std::vector<std::uint64_t> entryHolds = inputHolds(entry, link);
        holds.insert(holds.end(), entryHolds.begin(), entryHolds.end());
    }
    CHECK_MSG(!holds.empty(), "the corpus records no input");
    CHECK_MSG(forced * 100 <= frames, ("forced " + std::to_string(forced) + " of " + std::to_string(frames) + " frames").c_str());
    const std::uint64_t inputHoldP99 = percentile(holds, 99);
    CHECK_MSG(inputHoldP99 <= frameIntervalMicroseconds, ("p99 hold after input is " + std::to_string(inputHoldP99) + " us").c_str());
    CHECK_MSG(bracketed == 0 && split == 0 && unreported == 0, ("frames torn:" + where).c_str());
}

// A program that hides the cursor for a whole input burst opens one bracket.
// Once it has outlived the deadline it is persistent and holds nothing until
// the cursor is shown, so the burst pays the deadline once, not on every key.
TEST(aCursorHiddenForAnInputBurstDoesNotHoldEveryEcho)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    const CorpusEntry* entry = findEntry(corpus, "echo-burst");
    if (!entry)
        return;
    LoopbackPaneLink link(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
    link.replay(entry->ptyReads());
    checkFaithful(*entry, link);
    const std::vector<std::uint64_t> holds = inputHolds(*entry, link);
    CHECK_MSG(holds.size() == entry->inputs.size(), "an echo was never published");
    const auto slowEchoes = static_cast<std::size_t>(std::ranges::count_if(holds, [](std::uint64_t hold) { return hold > frameIntervalMicroseconds; }));
    // One per burst: the key that opened the bracket waits it out.
    CHECK_MSG(slowEchoes <= 2, (std::to_string(slowEchoes) + " echoes waited longer than a frame").c_str());
    const std::uint64_t p99 = percentile(holds, 99);
    CHECK_MSG(p99 <= frameIntervalMicroseconds, ("p99 hold after input is " + std::to_string(p99) + " us").c_str());
}

// A program that hides the cursor for its whole life — a process monitor, an
// Ink-style agent UI — opens no bracket after its first frame, so nothing it
// draws waits on one: the first frame flows within the deadline (its hide opened
// a bracket against the shown cursor before it), and every later one within a
// frame.
TEST(aCursorHiddenForAProgramsWholeLifeIsNeverHeld)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    for (const std::string_view name : { std::string_view("top"), std::string_view("ink-spinner") }) {
        const CorpusEntry* entry = findEntry(corpus, name);
        if (!entry)
            continue;
        LoopbackPaneLink link(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
        link.replay(entry->ptyReads());
        checkFaithful(*entry, link);
        const std::vector<PublishedFrame>& frames = link.frames();
        CHECK_MSG(!frames.empty(), (std::string(name) + " published nothing").c_str());
        // The daemon's poll wakes on whole milliseconds, rounded up.
        constexpr std::uint64_t wakeRounding = 1000;
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const std::uint64_t bound = index == 0 ? Ubiquitty::Runtime::coherenceDeadlineMicroseconds + wakeRounding : frameIntervalMicroseconds;
            const std::uint64_t hold = frames[index].census.holdMicroseconds;
            CHECK_MSG(hold <= bound, (std::string(name) + " frame " + std::to_string(index) + " waited " + std::to_string(hold) + " us").c_str());
        }
    }
}

// A writer that never fills a pass is shown at the refresh, whatever its gaps:
// below the quiet window the read never goes quiet, and only the deadline would
// take a frame, every one of them forced, at half the refresh; just above it the
// read goes quiet after every line, and nothing but the pacing stops a frame
// per line.
TEST(aLineTrickleIsShownAtTheRefreshRate)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    for (const std::string_view name : { std::string_view("line-trickle-fast"), std::string_view("line-trickle-slow") }) {
        const CorpusEntry* entry = findEntry(corpus, name);
        if (!entry)
            continue;
        LoopbackPaneLink link(LoopbackOptions { .rows = entry->rows, .columns = entry->columns });
        link.replay(entry->ptyReads());
        checkFaithful(*entry, link);
        const double perSecond = framesPerSecondWhileWriting(*entry, link);
        CHECK_MSG(perSecond >= 50.0 && perSecond <= 70.0, (std::string(name) + " shows " + std::to_string(perSecond) + " frames a second").c_str());
        const std::uint64_t forced = link.census().frameCount(FrameClass::Forced);
        CHECK_MSG(forced * 100 <= link.frames().size(), (std::string(name) + " forced " + std::to_string(forced) + " of " + std::to_string(link.frames().size())).c_str());
    }
}

TEST(theWireKeepsEveryCorpusClientOnItsHost)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    const bool isReporting = std::getenv("UBQ_SYNC_CORPUS_REPORT") != nullptr;
    if (isReporting) {
        std::printf("| entry | bytes | frames | boundary | settled | flood | forced | structural | in ?25l bracket | inside a split redraw / redraws | updates | wire B/frame mean (p50) | hold p50 ms | hold p99 ms | hold max ms |\n");
        std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    }
    for (const CorpusEntry& entry : corpus) {
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        checkFaithful(entry, link);
        CHECK_MSG(!link.frames().empty(), (entry.name + " published nothing").c_str());
        if (isReporting)
            report(entry, link);
    }
    if (!isReporting)
        return;
    // The census's own hold-time histogram.
    std::printf("\n| entry | <1 ms | 1-2 ms | 2-4 ms | 4-8 ms | 8-16 ms | 16-33 ms | >=33 ms |\n|---|---|---|---|---|---|---|---|\n");
    for (const CorpusEntry& entry : corpus) {
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        const auto& histogram = link.census().holdHistogram();
        std::uint64_t beyond = 0;
        for (std::size_t bucket = 6; bucket < histogram.size(); ++bucket)
            beyond += histogram[bucket];
        std::printf("| %s | %llu | %llu | %llu | %llu | %llu | %llu | %llu |\n", entry.name.c_str(),
            static_cast<unsigned long long>(histogram[0]), static_cast<unsigned long long>(histogram[1]),
            static_cast<unsigned long long>(histogram[2]), static_cast<unsigned long long>(histogram[3]),
            static_cast<unsigned long long>(histogram[4]), static_cast<unsigned long long>(histogram[5]),
            static_cast<unsigned long long>(beyond));
    }

    // From each key the user sent to the frame that first showed its echo.
    std::printf("\n| entry | keys | hold p50 ms | hold p99 ms | hold max ms |\n|---|---|---|---|---|\n");
    for (const CorpusEntry& entry : corpus) {
        if (entry.inputs.empty())
            continue;
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        const std::vector<std::uint64_t> holds = inputHolds(entry, link);
        std::printf("| %s | %zu | %.2f | %.2f | %.2f |\n", entry.name.c_str(), holds.size(),
            static_cast<double>(percentile(holds, 50)) / 1000.0, static_cast<double>(percentile(holds, 99)) / 1000.0,
            static_cast<double>(percentile(holds, 100)) / 1000.0);
    }

    // What another value of each constant would have measured on the same
    // corpus: the evidence the shipped values are chosen from.
    struct Variant {
        const char* name;
        Ubiquitty::Runtime::CoherenceTuning tuning;
    };
    const Ubiquitty::Runtime::CoherenceTuning shipped;
    const Variant variants[] = {
        { "shipped", shipped },
        { "deadline = one refresh (16.7 ms)", { .deadlineMicroseconds = Ubiquitty::Runtime::refreshIntervalMicroseconds } },
        { "deadline = 50 ms", { .deadlineMicroseconds = 50'000 } },
        { "quiescence = 0 (would-block alone)", { .quiescenceMicroseconds = 0 } },
        { "quiescence = 1 ms", { .quiescenceMicroseconds = 1'000 } },
        { "quiescence = 1.5 ms", { .quiescenceMicroseconds = 1'500 } },
        { "no persistent bracket", { .isPersistentBracketEnabled = false } },
        { "unpaced", { .isPaced = false } },
    };
    std::printf("\n| variant | frames | forced | forced share | coherent frames in a bracket | frames in a split redraw | input hold p99 ms | longest coherent hold ms | line trickle frames a second (fast / slow) |\n|---|---|---|---|---|---|---|---|---|\n");
    for (const Variant& variant : variants) {
        std::uint64_t frames = 0;
        std::uint64_t forced = 0;
        std::size_t bracketed = 0;
        std::size_t split = 0;
        std::uint64_t longest = 0;
        double fastTrickleRate = 0.0;
        double slowTrickleRate = 0.0;
        std::vector<std::uint64_t> holds;
        for (const CorpusEntry& entry : corpus) {
            LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns, .coherence = variant.tuning });
            link.replay(entry.ptyReads());
            frames += link.frames().size();
            forced += link.census().frameCount(FrameClass::Forced);
            bracketed += coherentFramesInsideBrackets(link);
            split += framesInsideSplitRedraws(entry, link);
            for (const PublishedFrame& frame : link.frames()) {
                if (frame.census.frameClass != FrameClass::Forced && frame.census.frameClass != FrameClass::Structural)
                    longest = std::max(longest, frame.census.holdMicroseconds);
            }
            const std::vector<std::uint64_t> entryHolds = inputHolds(entry, link);
            holds.insert(holds.end(), entryHolds.begin(), entryHolds.end());
            if (entry.name == "line-trickle-fast")
                fastTrickleRate = framesPerSecondWhileWriting(entry, link);
            if (entry.name == "line-trickle-slow")
                slowTrickleRate = framesPerSecondWhileWriting(entry, link);
        }
        std::printf("| %s | %llu | %llu | %.2f%% | %zu | %zu | %.2f | %.2f | %.1f / %.1f |\n", variant.name,
            static_cast<unsigned long long>(frames), static_cast<unsigned long long>(forced),
            frames ? 100.0 * static_cast<double>(forced) / static_cast<double>(frames) : 0.0, bracketed, split,
            static_cast<double>(percentile(holds, 99)) / 1000.0, static_cast<double>(longest) / 1000.0, fastTrickleRate, slowTrickleRate);
    }
}

// The census over the corpus has to see what the corpus was built to show, or
// the measurement it feeds is of nothing: a frame after every pass of the
// cursor-hidden repaint must land inside its bracket (the rule holds exactly
// those), the 2026 user must close blocks, and the flood must saturate passes.
TEST(theCensusSeesTheShapesTheCorpusCarries)
{
    for (const CorpusEntry& entry : loadOrFail()) {
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
        link.replay(entry.ptyReads());
        const auto& census = link.census();
        if (entry.name == "cursor-hidden-repaint") {
            LoopbackPaneLink ungated(LoopbackOptions { .rows = entry.rows, .columns = entry.columns, .isCoherenceGated = false });
            ungated.replay(entry.ptyReads());
            CHECK_MSG(ungated.census().redrawBracketFrameCount() > 0, "no frame landed inside a cursor-hidden repaint");
        }
        if (entry.name == "synchronized-output")
            CHECK_MSG(census.frameCount(FrameClass::Boundary) > 0, "no frame followed a closed 2026 block");
        if (entry.name == "cat-flood")
            CHECK_MSG(census.frameCount(FrameClass::Flood) > 0, "no drain pass saturated");
        if (entry.name == "ink-spinner")
            CHECK_MSG(census.redrawBracketFrameCount() == 0, "a cursor hidden throughout opened a bracket");
    }
}

UBIQUITTY_TEST_MAIN()
