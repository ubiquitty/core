// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/sync_mirror_clear_and_reflow.cpp — the mirror after Cmd-K and
// after a narrowing resize.
//
// Two things a person does at a remote prompt, and what each leaves on their
// screen: Cmd-K, which clears the buffer while keeping the line being typed,
// and dragging the window narrower, which rewraps everything above the
// prompt. Either can go wrong over Sync — the prompt gone until the next
// keystroke, the rewrapped text scrambled — and both are invisible to a test
// that counts frames, because every frame in both cases is well-formed.
//
// So this drives the real consumer. The daemon's bytes go into the client end
// of a Sync connection holding a replica Terminal, the way a window holds its
// mirror, and clear/resize requests go to the host, which alone changes the
// screen and the history. The oracle is the host's own Terminal: after the dust
// settles, every row the mirror shows, history included, must be the row the
// host holds, and the cursor must sit where the host's does.
//
// Exit 0 on success.
//
#include "Assertions.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"
#include "TrustedLocalPeer.h"

#include "Core/Crypto/SHA256.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/TokenStore.h"

#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct SyncMirrorAccess {
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, int rows, int columns, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, rows, columns);
        session->term->scrollbackLimit = 2000;
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        session->isFrameDue = true;
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    // The window's attach bridge: a human client subscribed to the session.
    // Its output is the daemon's own buffer, which the test reads as the
    // transport would; nothing writes to the socket, whose peer stays open so
    // a flush, should one run, writes rather than faults.
    static std::size_t addWindow(Daemon& d, std::uint32_t sessionID)
    {
        int ends[2] = { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
            std::abort();
        peerEnds().push_back(ends[1]);
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.fileDescriptor = PAL::UnixFileDescriptor(ends[0]);
        c.sessionID = sessionID;
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (!session)
            return static_cast<std::size_t>(-1);
        c.workspaceID = d.hostedWorkspaceFor(*session).workspaceID;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    // The frames the session's screen source has captured.
    static std::uint64_t generation(const Daemon::Session& s) { return s.screenSource->generation(); }

    static std::vector<int>& peerEnds()
    {
        static std::vector<int> ends;
        return ends;
    }

    // An ordinary agent whose known credential is verified by its real Greet.
    // The exact principal has prior session consent but no Drive scope.
    static std::size_t addAgent(Daemon& d, std::uint32_t sessionID, const std::string& tokenPath, const std::string& token)
    {
        d.m_tokenPath = tokenPath;
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.peerProcess = "/usr/bin/agent";
        c.agentLabel = "agent";
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (session) {
            (void)session->addSessionGrant("token:" + Crypto::sha256Hex(token));
        }
        return d.m_clients.size() - 1;
    }

    static bool isFrameDue(const Daemon::Session& s) { return s.isFrameDue; }

    static bool hasObserveOnlyAuthority(const Daemon& d, std::size_t index, const std::string& token)
    {
        const auto& client = d.m_clients[index];
        return d.hasLiveStructureAttachment(client) && client.hasToken
            && digestsEqual(client.tokenID, Crypto::sha256Hex(token))
            && client.tokenScope == Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe };
    }

    // One read pass of a client's frames, ended as the run loop ends it: the
    // newest size of the pass is applied once.
    static void feed(Daemon& d, std::size_t index, const std::vector<std::uint8_t>& wire)
    {
        ScreenPlaneAccess::receive(d, index, wire);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        for (auto& session : d.m_sessions)
            d.applyPendingGeometry(*session);
    }

    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }
    // A frame, published when the terminal owes one.
    static void publishIfDue(Daemon& d, Daemon::Session& s)
    {
        if (s.isFrameDue)
            d.publishFrame(s, Runtime::FrameClass::Settled);
    }

    static std::vector<std::uint8_t> takeQueued(Daemon& d, std::size_t index)
    {
        Daemon::Client& c = d.m_clients[index];
        std::vector<std::uint8_t> bytes(c.outputBuffer.begin() + static_cast<std::ptrdiff_t>(c.outputConsumed), c.outputBuffer.end());
        c.clearPendingOutput();
        return bytes;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::SyncMirrorAccess;
using Ubiquitty::TestDoubles::StreamDouble;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = SyncMirrorAccess;

using Screen = Ubiquitty::Host::ScreenPlaneAccess;

// A mirror the way a window holds one: the client end of the window's screen
// connection, so the bytes are consumed by the code that consumes them.
struct Mirror {
    UbiquittyTest::ScreenClient client;
    std::optional<std::vector<std::uint8_t>> credential;

    Mirror(int rows, int columns, std::uint32_t historyCacheLines = 2000)
        : client(rows, columns, historyCacheLines)
    {
    }

    [[nodiscard]] const Ubiquitty::Terminal& term() const
    {
        static const Ubiquitty::Terminal none;
        return client.pane() ? *client.pane() : none;
    }
};

void clearWindow(Daemon& daemon, std::size_t window, Mirror& mirror, std::string_view command = "buffer")
{
    CHECK(mirror.client.call("{\"jsonrpc\":\"2.0\",\"id\":\"clear\",\"method\":\"pane.clear\",\"params\":{\"pane\":1,\"command\":\"" + std::string(command) + "\"}}"));
    A::feed(daemon, window, mirror.client.takeUpstream(Screen::now()));
}

// Every row the terminal holds, history first, then the screen: what a person
// scrolling up and down would read.
std::vector<std::string> allRows(const Ubiquitty::Terminal& t)
{
    std::vector<std::string> rows;
    const int total = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;
    rows.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i)
        rows.push_back(Ubiquitty::Screen::absoluteRowText(t, i, t.columns));
    return rows;
}

std::string visibleRow(const Ubiquitty::Terminal& t, int row)
{
    return Ubiquitty::Screen::absoluteRowText(t, Ubiquitty::Screen::scrollbackCount(&t) + row, t.columns);
}

// One service tick the way the run loop takes it: the program's output into
// the host Terminal, the frame the rule would take, and the window's screen
// plane carried both ways.
template<typename Session>
void settle(Daemon& d, Session& s, std::size_t window, Mirror& mirror)
{
    A::service(d, s);
    A::publishIfDue(d, s);
    Screen::deliver(d, window, mirror.client);
}

// The first attach: the window greets, and the checkpoint carries the screen
// and the history behind it.
template<typename Session>
void attach(Daemon& d, Session& s, std::size_t window, Mirror& mirror)
{
    if (mirror.credential) {
        const auto request = Screen::selectedRequest(d, window);
        CHECK(request);
        if (!request)
            return;
        CHECK(mirror.client.start(*request, mirror.credential));
        // The credentialed agent is attached without a transport; a standing
        // grant welcomes it inside the admission, whose flush needs one.
        Screen::useTransport(d, window, mirror.client.takeCarrier());
        Screen::receive(d, window, mirror.client.takeUpstream(Screen::now()));
        Screen::deliver(d, window, mirror.client);
    } else
        Screen::greet(d, window, mirror.client);
    CHECK(mirror.client.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
    settle(d, s, window, mirror);
}

// The whole comparison, printed row by row on the first divergence so a
// failure names the row rather than a count.
//
// History travels as logical lines (Docs/sync.md §6.6), and the first rows
// of a line that straddles the screen's top edge as the open line, so every
// history row is compared. A window that attaches is backfilled to a depth in
// lines; history older than that is left out.
bool mirrorsHost(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& mirror, const char* what, int depthLines = std::numeric_limits<int>::max())
{
    std::vector<std::string> hostRows = allRows(host);
    const int history = Ubiquitty::Screen::scrollbackCount(&host);
    // The open line's rows are below the depth's first line.
    int first = history;
    while (first > 0 && Ubiquitty::Screen::scrollbackLine(&host, first - 1)->wrapped)
        --first;
    int lines = 0;
    while (first > 0 && lines < depthLines) {
        --first;
        lines += Ubiquitty::Screen::scrollbackLine(&host, first)->wrapped ? 0 : 1;
        if (lines == depthLines) {
            // The line's first row is where the depth begins.
            while (first > 0 && Ubiquitty::Screen::scrollbackLine(&host, first - 1)->wrapped)
                --first;
        }
    }
    hostRows.erase(hostRows.begin(), hostRows.begin() + first);
    const std::vector<std::string> mirrorRows = allRows(mirror);
    bool same = hostRows.size() == mirrorRows.size();
    for (std::size_t i = 0; same && i < hostRows.size(); ++i)
        same = hostRows[i] == mirrorRows[i];
    if (!same) {
        std::fprintf(stderr, "%s: host holds %zu rows, the mirror %zu\n", what, hostRows.size(), mirrorRows.size());
        const std::size_t shown = hostRows.size() < mirrorRows.size() ? hostRows.size() : mirrorRows.size();
        for (std::size_t i = 0; i < shown; ++i)
            if (hostRows[i] != mirrorRows[i])
                std::fprintf(stderr, "  row %zu\n    host   |%s|\n    mirror |%s|\n", i, hostRows[i].c_str(), mirrorRows[i].c_str());
    }
    const bool isCursorSame = host.screens.active->cursorX == mirror.screens.active->cursorX && host.screens.active->cursorY == mirror.screens.active->cursorY;
    if (!isCursorSame)
        std::fprintf(stderr, "%s: host cursor (%d,%d), mirror cursor (%d,%d)\n", what, host.screens.active->cursorY, host.screens.active->cursorX, mirror.screens.active->cursorY, mirror.screens.active->cursorX);
    return same && isCursorSame;
}

// A shell session with history above the prompt: lines long enough to wrap
// differently at every width, each carrying its ordinal so a scrambled reflow
// names the line it scrambled.
void writeSession(Ubiquitty::Terminal& t, int lines)
{
    for (int i = 0; i < lines; ++i) {
        char row[200];
        std::snprintf(row, sizeof row, "line %03d ", i);
        std::string text = row;
        while (text.size() < 130)
            text += "abcdefghij";
        text += "\r\n";
        UbiquittyTest::feed(t, text);
    }
    UbiquittyTest::feed(t, "prompt$ typed");
}

/// Resizes the window: its viewport of the pane, read by the daemon in one
/// pass that ends as the run loop ends it, the pass's newest size applied
/// once.
///
/// - Parameters:
///   - d: The daemon.
///   - window: The window's client.
///   - mirror: The window's end of its screen connection.
///   - columns: The columns the window now shows.
///   - rows: The rows.
void resizeWindow(Daemon& d, std::size_t window, Mirror& mirror, int columns, int rows)
{
    CHECK(mirror.client.sendViewport(columns, rows));
    Screen::receive(d, window, mirror.client.takeStream(Screen::now()));
    A::feed(d, window, {});
}

} // namespace

int main()
{
    constexpr int rows = 24;
    constexpr int wide = 80;
    constexpr int narrow = 40;
    constexpr auto checkpoint = Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead;

    // A sized run scrolled into history keeps its size on the mirror.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& session = A::addSession(d, 1, rows, wide, &stream);
        session.term->drawsSizedText = true;
        session.term->drawsProportionalText = true;
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, session, window, mirror);
        UbiquittyTest::feed(*session.term, "\033]66;n=1:d=2;X\007");
        for (int row = 0; row < rows; ++row)
            UbiquittyTest::feed(*session.term, "\r\n");
        session.isFrameDue = true;
        settle(d, session, window, mirror);
        const auto* history = Ubiquitty::Screen::scrollbackLine(&mirror.term(), 0);
        CHECK(history && history->length > 0);
        if (history && history->length > 0) {
            CHECK(history->cells[0].codePoint == 'X');
            const auto* spec = mirror.term().textSizeTable.lookup(Ubiquitty::cellPen(history->cells[0]).extendedAttributes.textSizeSpecID());
            CHECK(spec && spec->fractionNumerator == 1 && spec->fractionDenominator == 2);
        }
    }

    // ---- 1. Attach: the mirror is the host, before anything happens.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 60);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);
        CHECK(mirrorsHost(*s.term, mirror.term(), "attach"));
    }

    // A flood between two frames: the ring turns over (10,000 full rows, about
    // 25 MB of cells), then 7,000 rows more are appended onto it (about 18 MB).
    // Neither fits one frame, and the window still ends holding the host's
    // history without being dropped.
    {
        constexpr int ring = 10000;
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        s.term->scrollbackLimit = ring;
        writeSession(*s.term, 40);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide, ring);
        attach(d, s, window, mirror);

        const auto flood = [&](int first, int count) {
            std::string bytes;
            for (int i = first; i < first + count; ++i) {
                char number[16];
                std::snprintf(number, sizeof number, "%07d", i);
                bytes += number;
                bytes.append(static_cast<std::size_t>(wide) - 7, 'x');
                bytes += "\r\n";
            }
            UbiquittyTest::feed(*s.term, bytes);
            s.isFrameDue = true;
        };
        flood(0, 12000);
        settle(d, s, window, mirror);
        CHECK(!A::didOverflow(d, window));
        CHECK(mirrorsHost(*s.term, mirror.term(), "a window after a ring turnover"));

        flood(12000, 7000);
        settle(d, s, window, mirror);
        CHECK(!A::didOverflow(d, window));
        CHECK(mirrorsHost(*s.term, mirror.term(), "a window after a burst past one frame"));
    }

    // A session nobody is attached to captures no frame for nobody: a detached
    // flood encodes none of its rows. A window that attaches afterwards still
    // mirrors the host exactly.
    {
        constexpr int ring = 10000;
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        s.term->scrollbackLimit = ring;
        writeSession(*s.term, 40);
        A::publishIfDue(d, s);
        std::string bytes;
        for (int i = 0; i < 3000; ++i) {
            char number[16];
            std::snprintf(number, sizeof number, "%07d", i);
            bytes += number;
            bytes.append(static_cast<std::size_t>(wide) - 7, 'x');
            bytes += "\r\n";
        }
        UbiquittyTest::feed(*s.term, bytes);
        s.isFrameDue = true;
        A::publishIfDue(d, s);
        CHECK_MSG(A::generation(s) == 0, "a detached publish captured a frame for nobody");

        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide, ring);
        attach(d, s, window, mirror);
        UbiquittyTest::feed(*s.term, "after the attach\r\n");
        s.isFrameDue = true;
        settle(d, s, window, mirror);
        // The backfill reaches the depth an attach is given, and the line
        // written since is an append; the window pulls older history when it
        // scrolls to it.
        CHECK(mirrorsHost(*s.term, mirror.term(), "a window attached after a detached flood", static_cast<int>(Ubiquitty::Protocol::Sync::defaultBackfillDepthLines) + 1));
    }

    // Repeated geometry, including an over-limit request clamped to the
    // current grid, must not signal the process or owe anyone a frame.
    for (const int columns : { wide, Ubiquitty::maxColumns }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, columns, &stream);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, columns);
        attach(d, s, window, mirror);
        // An attach ends with the update that tells the client the pane's
        // echo context; what the viewport costs is counted from there.
        for (int rounds = 0; rounds < 100 && !mirror.client.connection().echoContext(1); ++rounds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            settle(d, s, window, mirror);
        }
        const std::uint64_t updatesBefore = mirror.client.connection().counters().updatesApplied;
        const std::size_t checkpointsBefore = mirror.client.count(checkpoint);
        resizeWindow(d, window, mirror, columns == wide ? wide : 65535, rows);
        CHECK(stream->columns == -1);
        CHECK(!A::isFrameDue(s));
        settle(d, s, window, mirror);
        CHECK(mirror.client.count(checkpoint) == checkpointsBefore);
        CHECK(mirror.client.connection().counters().updatesApplied == updatesBefore);
    }

    // A clear, or a clear and a new history, retracts what the window holds:
    // the history it had is purged, not finished with stale archived rows.
    for (const int replacementLines : { 0, 40 }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 600);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);
        CHECK(Ubiquitty::Screen::scrollbackCount(&mirror.term()) > 0);
        clearWindow(d, window, mirror);
        if (replacementLines)
            writeSession(*s.term, replacementLines);
        settle(d, s, window, mirror);
        CHECK(mirrorsHost(*s.term, mirror.term(), "history replacement after a clear"));
    }

    // A primary-screen application redraws its own rows on SIGWINCH.
    // Its geometry changes must never archive fragments on either side, and
    // each new geometry reaches the window as one checkpoint.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        stream->facts.isShell = false;
        for (int row = 0; row < 20; ++row)
            UbiquittyTest::feed(*s.term, "paragraph rendered incrementally with right-edge text 1234\r\n");
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) == 0);
        for (int step = 0; step < 12; ++step) {
            const int columns = step % 2 ? wide : narrow;
            const std::size_t checkpointsBefore = mirror.client.count(checkpoint);
            // A pane's size changes at most once per coalesce window
            // (Docs/sync.md §6.10); each step is a geometry of its own.
            std::this_thread::sleep_for(std::chrono::microseconds(Ubiquitty::Protocol::Sync::measuredGeometryPolicy().coalesceMicroseconds));
            resizeWindow(d, window, mirror, columns, rows);
            stream->pushInbound("\r\033[2Kupdated incremental paragraph");
            settle(d, s, window, mirror);
            CHECK(mirror.client.count(checkpoint) == checkpointsBefore + 1);
            CHECK(Ubiquitty::Screen::scrollbackCount(&mirror.term()) == 0);
            CHECK(mirrorsHost(*s.term, mirror.term(), "primary application resize without history"));
        }
    }

    // The host alone resizes the screen and history; the passive mirror
    // replaces both with the authoritative frames, including in-flight output.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 60);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);

        resizeWindow(d, window, mirror, narrow, rows);
        settle(d, s, window, mirror);
        CHECK(s.term->columns == narrow);
        CHECK(mirrorsHost(*s.term, mirror.term(), "after narrowing to 40"));

        // And back out again.
        resizeWindow(d, window, mirror, wide, rows);
        settle(d, s, window, mirror);
        CHECK(mirrorsHost(*s.term, mirror.term(), "after widening to 80"));

        // Output arriving in the same tick as a narrowing: the frame after it
        // must not append onto a ring the reflow rewrote.
        resizeWindow(d, window, mirror, narrow, rows);
        stream->pushInbound("\r\nafter the resize\r\nprompt$ ");
        settle(d, s, window, mirror);
        CHECK(mirrorsHost(*s.term, mirror.term(), "narrowing with output in flight"));
    }

    // ---- 3. Cmd-K at the prompt. The host is asked to clear keeping the
    // typed line; the shell then redraws its prompt on Ctrl-L the way zsh and
    // bash do. Afterwards the prompt is on screen — not blank until the next
    // keystroke — and the mirror is the host.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 60);
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);

        // The mirror stays untouched until the host publishes its clear.
        const auto beforeClear = allRows(mirror.term());
        clearWindow(d, window, mirror);
        CHECK(allRows(mirror.term()) == beforeClear);
        // The shell answers the redraw request the way zsh's clear-screen does:
        // clear, home, prompt and buffer again.
        if (stream->written.find('\x0c') != std::string::npos)
            stream->pushInbound("\033[H\033[2Jprompt$ typed");
        settle(d, s, window, mirror);
        settle(d, s, window, mirror);
        CHECK(visibleRow(mirror.term(), 0).rfind("prompt$ typed", 0) == 0);
        CHECK(Ubiquitty::Screen::scrollbackCount(&mirror.term()) == 0);
        CHECK(mirrorsHost(*s.term, mirror.term(), "after Cmd-K"));
    }

    // ---- 4. Cmd-K while a full-screen program owns the host's layout. The
    // command clears nothing on either side and only asks the program to
    // repaint, so the host's screen is untouched, its history intact, and the
    // redraw request is what reached the PTY.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 30);
        UbiquittyTest::feed(*s.term, "\033[?1049h\033[Hfull-screen program");
        const std::size_t window = A::addWindow(d, 1);
        Mirror mirror(rows, wide);
        attach(d, s, window, mirror);
        const std::vector<std::string> before = allRows(*s.term);
        const int historyBefore = Ubiquitty::Screen::scrollbackCount(s.term.get());

        clearWindow(d, window, mirror);
        CHECK(stream->written.find('\x0c') != std::string::npos);
        CHECK(allRows(*s.term) == before);
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) == historyBefore);
        CHECK(visibleRow(*s.term, 0).rfind("full-screen program", 0) == 0);
        settle(d, s, window, mirror);
        CHECK(mirrorsHost(*s.term, mirror.term(), "Cmd-K under a full-screen program"));
    }

    // ---- 5. The frame is refused with a body that names no command, and
    // refused without the Drive scope. Neither reaches the Terminal.
    {
        char directoryTemplate[] = "/tmp/ubq-mirror-scope-XXXXXX";
        const char* made = ::mkdtemp(directoryTemplate);
        CHECK(made);
        if (!made)
            return UBIQUITTY_TEST_SUMMARY();
        const std::string directory(made);
        const std::string tokenPath = directory + "/tokens";
        struct TokenCleanup {
            const std::string& directory;
            const std::string& path;
            ~TokenCleanup()
            {
                ::unlink(path.c_str());
                ::unlink((path + ".lock").c_str());
                ::unlink((path + ".metadata").c_str());
                ::rmdir(directory.c_str());
            }
        } cleanup { directory, tokenPath };
        const std::string token = "mirror-owned-observe-token";
        Sync::TokenRecord record;
        record.hashHex = Ubiquitty::Crypto::sha256Hex(token);
        record.scope = { Sync::Scope::Observe };
        record.label = "mirror-observe";
        CHECK(Ubiquitty::Host::writeTokens(tokenPath, { record }) == Ubiquitty::Host::TokenWriteOutcome::Durable);
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 30);
        const std::size_t window = A::addWindow(d, 1);
        const std::size_t watcher = A::addAgent(d, 1, tokenPath, token);
        Mirror mirror(rows, wide);
        Mirror watchingMirror(rows, wide);
        watchingMirror.credential.emplace(token.begin(), token.end());
        attach(d, s, window, mirror);
        attach(d, s, watcher, watchingMirror);
        CHECK(A::hasObserveOnlyAuthority(d, watcher, token));
        s.isFrameDue = false;
        const std::vector<std::string> before = allRows(*s.term);

        for (const std::string_view malformed : { "", "invalid", "Buffer" }) {
            clearWindow(d, window, mirror, malformed);
            Screen::deliver(d, window, mirror.client);
            const auto results = mirror.client.takeResults();
            CHECK(results.size() == 1 && results.front().find("-32602") != std::string::npos);
        }
        CHECK(allRows(*s.term) == before);
        CHECK(stream->written.empty());
        CHECK(!A::isFrameDue(s));

        clearWindow(d, watcher, watchingMirror);
        Screen::deliver(d, watcher, watchingMirror.client);
        const auto results = watchingMirror.client.takeResults();
        CHECK(results.size() == 1 && results.front().find("-32002") != std::string::npos);
        CHECK(allRows(*s.term) == before);
        CHECK(stream->written.empty());

        // The window itself is allowed, and the same frame then clears.
        clearWindow(d, window, mirror);
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) == 0);
        CHECK(visibleRow(*s.term, 0).rfind("prompt$ typed", 0) == 0);
        CHECK(A::isFrameDue(s));
    }

    // ---- 6. Two windows on one session, one of them clearing. The clear is
    // the host's alone, and a cleared history is purged on every window: each
    // is sent one HistoryReset, and the screen the clear left as an update,
    // never a checkpoint, and both end up showing the host — the watching
    // window included, though it asked for nothing.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, rows, wide, &stream);
        writeSession(*s.term, 60);
        const std::size_t clearing = A::addWindow(d, 1);
        const std::size_t watching = A::addWindow(d, 1);
        Mirror clearingMirror(rows, wide);
        Mirror watchingMirror(rows, wide);
        attach(d, s, clearing, clearingMirror);
        attach(d, s, watching, watchingMirror);
        constexpr auto reset = Ubiquitty::Protocol::Sync::FrameKind::HistoryReset;
        const std::size_t clearingBefore = clearingMirror.client.count(checkpoint);
        const std::size_t watchingBefore = watchingMirror.client.count(checkpoint);
        const std::size_t clearingResetsBefore = clearingMirror.client.count(reset);
        const std::size_t watchingResetsBefore = watchingMirror.client.count(reset);

        clearWindow(d, clearing, clearingMirror);
        if (stream->written.find('\x0c') != std::string::npos)
            stream->pushInbound("\033[H\033[2Jprompt$ typed");
        settle(d, s, clearing, clearingMirror);
        settle(d, s, watching, watchingMirror);
        CHECK(clearingMirror.client.count(reset) == clearingResetsBefore + 1);
        CHECK(watchingMirror.client.count(reset) == watchingResetsBefore + 1);
        CHECK(clearingMirror.client.count(checkpoint) == clearingBefore);
        CHECK(watchingMirror.client.count(checkpoint) == watchingBefore);
        CHECK(mirrorsHost(*s.term, clearingMirror.term(), "the clearing window after Cmd-K"));
        CHECK(mirrorsHost(*s.term, watchingMirror.term(), "the watching window after Cmd-K"));
    }

    if (UBIQUITTY_TEST_SUMMARY() == 0)
        std::printf("sync_mirror_clear_and_reflow: the mirror is the host after a narrowing resize and after Cmd-K\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
