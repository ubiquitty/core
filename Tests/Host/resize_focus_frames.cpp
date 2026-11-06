// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/resize_focus_frames.cpp — the two frames a viewer sends about its
// own window.
//
// `Viewport` and `Focus` are the messages where a client reports something the
// host cannot see for itself — the size and the focus of a window it does not
// own — and the host acts on the report by driving the session: SIGWINCH and a
// reflow for one, a mode change and a CSI I/O report for the other. That is
// why both are a driver's alone: what reaches the program is bytes it asked
// for, and a client watching a session must not be able to inject them by
// claiming its window moved.
//
// Two behaviours here are easy to get wrong in a way no client would report:
//
//   * The focus report has to reach the PTY within the pass that carried it.
//     CSI I / CSI O lands in Core's reply buffer, which the daemon otherwise
//     drains only after the PTY produces output — so on an idle session the
//     report would sit there until the program happened to print, by which
//     time the focus it announces is stale.
//
//   * Whether or not the host reflows (a non-shell app owning the primary
//     screen suppresses it), every client of the session must end up holding
//     the host's history at the host's width. The new geometry reaches each as
//     a checkpoint carrying the host-authored history, so a client never keeps
//     a history it rewrapped itself — a mixed-width desync that no error path
//     reports. Clients of other sessions are sent nothing.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "LoopbackPaneLink.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"

#include "Core/GridBounds.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ResizeFocusAccess {
    // A session with a real Terminal and a stream double, so the geometry the
    // daemon pushes at the PTY is observable and the foreground-process answer
    // that decides the reflow can be scripted.
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, int rows, int columns,
        TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, rows, columns);
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        // A driver's focus reaches the program through the pane's input
        // writer, as its typing does.
        (void)d.wireRemoteInput(*session);
        session->isFrameDue = true;
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    /// Adds a person's client attached to a session: one holding a token
    /// minted for the person at the window, with a scope.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The session it shows.
    ///   - scope: What its token allows.
    /// - Returns: The client's index.
    static std::size_t addClient(Daemon& d, std::uint32_t sessionID, Protocol::Sync::ScopeSet scope)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.isInteractiveUser = true;
        c.tokenID = "person-" + std::to_string(c.clientID);
        c.tokenScope = scope;
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
    static bool isFrameDue(const Daemon::Session& s) { return s.isFrameDue; }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::ResizeFocusAccess;
using Ubiquitty::Modes::FocusState;
using Ubiquitty::TestDoubles::StreamDouble;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = ResizeFocusAccess;

const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
const Sync::ScopeSet watchingOnly { Sync::Scope::Observe };

/// Hands the daemon what a client has to send, as one read pass.
///
/// - Parameters:
///   - d: The daemon.
///   - index: The client.
///   - client: Its end of its screen connection.
void carry(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& client)
{
    Screen::receive(d, index, client.takeStream(Screen::now()));
}

/// Hands the daemon one `Viewport` frame with a body of the caller's
/// choosing, as a client that does not keep its own refusals would send it.
///
/// - Parameters:
///   - d: The daemon.
///   - index: The client.
///   - body: The frame's body.
void carryViewportBody(Daemon& d, std::size_t index, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    CHECK(Sync::appendFrame(frame, Sync::FrameKind::Viewport, 1, body));
    Screen::receive(d, index, frame);
}

/// A viewport's body.
///
/// - Parameters:
///   - columns: Its columns.
///   - rows: Its rows.
/// - Returns: The encoding, with no pixels measured.
std::vector<std::uint8_t> viewportBody(std::uint16_t columns, std::uint16_t rows)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Viewport { .rows = rows, .columns = columns, .cellPixelWidth = 0, .cellPixelHeight = 0, .textAreaPixelWidth = 0, .textAreaPixelHeight = 0, .drawablePixelWidth = 0, .drawablePixelHeight = 0 });
    return body;
}

/// Whether the daemon closed a client's screen connection.
///
/// - Parameters:
///   - d: The daemon.
///   - index: The client.
/// - Returns: Whether its connection is closed, or gone with the client.
bool isClosed(Daemon& d, std::size_t index)
{
    return index >= A::clientCount(d) || !Screen::connection(d, index) || Screen::connection(d, index)->state() == Sync::ConnectionState::Closed;
}

// Fill a session's scrollback so a resize has history to carry.
void fillScrollback(Ubiquitty::Terminal& t, int lines)
{
    for (int i = 0; i < lines; ++i)
        UbiquittyTest::feed(t, "history line\r\n");
}

} // namespace

int main()
{
    // ---- 1. Focus reporting is driving. What a client holding Observe alone
    // says of its focus moves nothing, and the terminal's idea of focus is
    // untouched — a watcher that could report focus would be telling the
    // program the user arrived.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        UbiquittyTest::feed(*s.term, "\033[?1004h"); // the program asked to hear about focus
        s.term->windowFocused = false;
        const std::size_t watcher = A::addClient(d, 1, watchingOnly);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, watcher, client);

        CHECK(client.setPaneFocus(FocusState::Focused));
        carry(d, watcher, client);

        CHECK(!s.term->windowFocused);
        CHECK(stream->written.empty()); // and no CSI I reached the program
    }

    // ---- 2. A driver's focus reaches the program IMMEDIATELY. The reply is
    // drained in the pass that carried the report rather than left for the
    // next PTY read, which on an idle session may never come. A driver that
    // has said nothing of its focus has told the program nothing.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        UbiquittyTest::feed(*s.term, "\033[?1004h");
        const std::size_t viewer = A::addClient(d, 1, driving);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, viewer, client);
        CHECK_MSG(stream->written.empty(), "a driver that said nothing of its focus was reported to the program");

        CHECK(client.setPaneFocus(FocusState::Focused));
        carry(d, viewer, client);
        CHECK(s.term->windowFocused);
        CHECK(stream->written == "\033[I");

        CHECK(client.setPaneFocus(FocusState::Unfocused));
        carry(d, viewer, client);
        CHECK(!s.term->windowFocused);
        CHECK(stream->written == "\033[I\033[O");
    }

    // ---- 3. Resizing is driving too, for the same reason: a watcher that
    // could resize would move the program's window under the person using
    // it. Its connection is closed, and nothing reaches the PTY.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        const std::size_t watcher = A::addClient(d, 1, watchingOnly);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, watcher, client);

        carryViewportBody(d, watcher, viewportBody(100, 40));
        Screen::endReadPass(d);

        CHECK(isClosed(d, watcher));
        CHECK(stream->columns == -1); // nothing reached the PTY
        CHECK(s.term->columns == 80);
        CHECK(s.term->rows == 24);
    }

    // ---- 4. An over-limit wire value never reaches the child raw. The PTY is
    // the half only the daemon can get right: Core clamps its own arguments to
    // the same limits in resizeGrid, so handing Core the raw value is
    // indistinguishable from clamping first, but TIOCSWINSZ takes whatever it
    // is given. The assertion that both ends agree is what makes the clamp
    // worth having — a child told 5000 columns wraps against a width the host
    // never rendered.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        const std::size_t viewer = A::addClient(d, 1, driving);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, viewer, client);
        Screen::endReadPass(d);
        const int sizesBefore = stream->windowSizeCalls;

        CHECK(client.sendViewport(5000, 4000)); // over Core's limit on both axes
        carry(d, viewer, client);
        // A read pass's viewports take effect once, at its end; nothing of
        // this one has reached the PTY before then.
        CHECK(stream->windowSizeCalls == sizesBefore);
        Screen::endReadPass(d);

        CHECK(stream->columns == Ubiquitty::maxColumns);
        CHECK(stream->rows == Ubiquitty::maxRows);
        CHECK(s.term->columns == stream->columns);
        CHECK(s.term->rows == stream->rows);
        CHECK(A::isFrameDue(s)); // the reflowed grid has to be resent
    }

    // ---- 5. A degenerate geometry is ignored rather than applied, and its
    // sender keeps its connection. Zero columns would make every subsequent
    // row walk empty.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        const std::size_t viewer = A::addClient(d, 1, driving);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, viewer, client);
        Screen::endReadPass(d);
        const int sizesBefore = stream->windowSizeCalls;

        carryViewportBody(d, viewer, viewportBody(0, 40));
        carryViewportBody(d, viewer, viewportBody(100, 0));
        Screen::endReadPass(d);

        CHECK(!isClosed(d, viewer));
        CHECK(stream->windowSizeCalls == sizesBefore);
        CHECK(s.term->columns == 80 && s.term->rows == 24);
    }

    // ---- 6. A truncated Viewport body closes its sender and sizes nothing.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        const std::size_t viewer = A::addClient(d, 1, driving);
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(d, viewer, client);
        Screen::endReadPass(d);
        const int sizesBefore = stream->windowSizeCalls;

        const std::array<std::uint8_t, 1> truncated { 0x01 };
        carryViewportBody(d, viewer, truncated);
        Screen::endReadPass(d);

        CHECK(isClosed(d, viewer));
        CHECK(stream->windowSizeCalls == sizesBefore);
        CHECK(s.term->columns == 80);
    }

    // ---- 7. Every client of the resized session ends up holding the host's
    // history at the host's width, whether the host reflowed it or — a
    // non-shell app owning the primary screen — kept it at its previous width. The
    // new geometry reaches each as a checkpoint; a client of another session
    // is sent nothing.
    for (const bool isShell : { false, true }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream);
        auto& other = A::addSession(d, 2, 24, 80, nullptr);
        stream->facts.isShell = isShell;
        fillScrollback(*s.term, 40);
        for (auto* session : { &s, &other })
            Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);

        const std::size_t viewer = A::addClient(d, 1, driving);
        const std::size_t mirror = A::addClient(d, 1, driving);
        const std::size_t elsewhere = A::addClient(d, 2, driving);
        UbiquittyTest::ScreenClient viewerScreen(24, 80);
        UbiquittyTest::ScreenClient mirrorScreen(24, 80);
        UbiquittyTest::ScreenClient elsewhereScreen(24, 80);
        const auto deliverAll = [&] {
            Screen::deliver(d, viewer, viewerScreen);
            Screen::deliver(d, mirror, mirrorScreen);
            Screen::deliver(d, elsewhere, elsewhereScreen);
        };
        Screen::greet(d, viewer, viewerScreen);
        Screen::greet(d, mirror, mirrorScreen);
        Screen::greet(d, elsewhere, elsewhereScreen);
        for (auto* session : { &s, &other })
            Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
        deliverAll();
        // An attach ends with the update that tells each client its pane's
        // echo context; what the resize costs is counted from there.
        for (int rounds = 0; rounds < 100 && !(viewerScreen.connection().echoContext(1) && mirrorScreen.connection().echoContext(1) && elsewhereScreen.connection().echoContext(1)); ++rounds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            deliverAll();
        }
        const std::uint64_t elsewhereUpdatesBefore = elsewhereScreen.connection().counters().updatesApplied;
        const std::size_t checkpointsBefore = viewerScreen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead);
        CHECK(checkpointsBefore == 1 && mirrorScreen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 1);

        CHECK(viewerScreen.sendViewport(100, 30));
        carry(d, viewer, viewerScreen);
        Screen::endReadPass(d);
        CHECK(stream->columns == 100); // the resize did happen
        CHECK(s.term->columns == 100);
        CHECK(A::isFrameDue(s));
        Screen::publish(d, s, Ubiquitty::Runtime::FrameClass::Structural);
        deliverAll();

        for (const UbiquittyTest::ScreenClient* client : { &viewerScreen, &mirrorScreen }) {
            CHECK(client->count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == checkpointsBefore + 1);
            const std::string difference = client->pane() ? UbiquittyTest::firstDisagreement(*s.term, *client->pane()) : std::string("no pane");
            CHECK_MSG(difference.empty(), difference.c_str());
        }
        CHECK(elsewhereScreen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 1); // another session is untouched
        CHECK(elsewhereScreen.connection().counters().updatesApplied == elsewhereUpdatesBefore);
    }

    if (failures == 0)
        std::printf("resize_focus_frames: Viewport and Focus drive the session, so both are a driver's "
                    "and a malformed viewport sizes nothing; the focus report reaches the program immediately "
                    "rather than waiting for it to print; one clamp feeds the PTY and Core alike; and a "
                    "resize, reflowed or not, leaves every client of that session holding the host's history and no other "
                    "client sent anything\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
