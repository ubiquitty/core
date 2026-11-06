// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/remote_pane_link_e2e.cpp — the remote pane link keeps the
// `PaneLink` contract against a real daemon, end to end.
//
// The client stack the native apps draw a remote pane through, a
// RemoteSessionCore over `ubiquitty-host --attach` to a real forked daemon,
// driven only through its `Session::PaneLink` face. The shell the daemon
// starts writes back what is typed at its prompt, which is the program the
// shared checks expect (Tests/Core/Session/PaneLinkConformance.h); the
// in-process and loopback links pass the same checks in
// test_pane_link_conformance. Beyond them, through the link:
//
//   1. the viewport the link reports sizes the host's pane: a program asked
//      for its size answers with the new one, and the view's terminal takes
//      the size from the host's picture;
//   2. a stopped link reads as Ended.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//

#include "Assertions.h"

#include "Core/Session/PaneLink.h"
#include "Core/Session/PaneLinkConformance.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/RemoteLinkHarness.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace Harness = Ubiquitty::HostTest;
namespace Session = Ubiquitty::Session;

namespace {

/// Counts the pictures the core applies.
struct ScreenCount final : Session::RemoteSessionObserver {
    /// Counts one.
    void onScreenChanged() override { ++screens; }
    int screens = 0;
};

/// A remote pane link attached to a session of a real forked daemon.
class DaemonLinkFixture {
public:
    /// Starts the daemon and attaches a fresh session through the link.
    ///
    /// - Parameter hostBinary: The built `ubiquitty-host`.
    explicit DaemonLinkFixture(const std::string& hostBinary)
        : m_daemon(hostBinary)
        , m_factory(m_daemon.attachCommand(), m_daemon.socketName())
    {
        Ubiquitty::resize(*m_terminal, 24, 80);
        if (!m_daemon.isReady())
            return;
        m_core.emplace(m_terminal.get(), &m_dispatcher, &m_factory, &m_observer);
        m_core->start(0, 80, 24);
        m_isReady = Harness::pumpUntil(m_dispatcher, 10s, [this] { return m_core->acknowledged() && m_observer.screens > 0; });
        // The shell's prompt arrives after the first picture.
        settle();
    }

    /// Whether the daemon started and the link attached.
    [[nodiscard]] bool isReady() const { return m_isReady; }

    /// The link.
    [[nodiscard]] Session::PaneLink& link() { return *m_core; }

    /// Pumps the session thread until a condition holds or ten seconds pass.
    ///
    /// - Parameter isDone: Whether the wait is over.
    /// - Returns: Whether it held.
    bool runUntil(const std::function<bool()>& isDone) { return Harness::pumpUntil(m_dispatcher, 10s, isDone); }

    /// Pumps the session thread for half a second.
    void settle()
    {
        (void)Harness::pumpUntil(m_dispatcher, 500ms, [] { return false; });
    }

    /// Reads the view; the core's terminal is written only on this thread.
    ///
    /// - Parameter reader: Reads the view.
    void readView(const std::function<void(const Session::PaneView&)>& reader) { reader(m_core->view()); }

    /// Detaches cleanly, as a closed window does.
    void stop()
    {
        if (!m_core)
            return;
        m_core->stop();
        m_dispatcher.pump();
    }

private:
    // Declared first, so the daemon outlives everything that reaches it.
    Harness::ForkedDaemon m_daemon;
    Harness::PumpedDispatcher m_dispatcher;
    Harness::AttachChannelFactory m_factory;
    ScreenCount m_observer;
    std::unique_ptr<Ubiquitty::Terminal> m_terminal = std::make_unique<Ubiquitty::Terminal>();
    bool m_isReady = false;
    // Destroyed first: it holds the dispatcher, the factory and the terminal.
    std::optional<Session::RemoteSessionCore> m_core;
};

/// Drives the remote link through the shared checks and the ones only a real
/// daemon answers.
///
/// - Parameter hostBinary: The built `ubiquitty-host`.
void theRemoteLinkKeepsThePaneLinkContractAgainstADaemon(const std::string& hostBinary)
{
    DaemonLinkFixture fixture(hostBinary);
    REQUIRE_MSG(fixture.isReady(), "premise: the link did not attach to the daemon");
    UbiquittyTest::checkPaneLinkConformance(fixture, "remote link over a daemon");

    // 1. The viewport the link reports sizes the host's pane. The line the
    // checks typed is killed first, so the shell runs only the question.
    Session::PaneLink& link = fixture.link();
    Session::PaneViewport viewport;
    viewport.rows = 30;
    viewport.columns = 100;
    link.setViewport(viewport);
    constexpr std::string_view question = "\x15stty size\r";
    CHECK_MSG(link.send(Session::PaneInput { .intent = Session::PaneInput::Raw {}, .localEncoding = std::span<const char>(question.data(), question.size()) }),
        "the link refused the size question");
    const auto isResized = [&fixture] {
        bool isAnswered = false;
        fixture.readView([&](const Session::PaneView& view) {
            isAnswered = view.terminal && view.terminal->rows == 30 && view.terminal->columns == 100
                && UbiquittyTest::paneViewText(view).find("30 100") != std::string::npos;
        });
        return isAnswered;
    };
    CHECK_MSG(fixture.runUntil(isResized), "the host's pane did not take the size the link reported");

    // 2. A stopped link is Ended.
    fixture.stop();
    CHECK_MSG(link.linkState(link.clockMicroseconds()).phase == Session::LinkPhase::Ended, "a stopped link does not read as Ended");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    // A carrier whose bridge has gone is an error to read, not a signal.
    (void)::signal(SIGPIPE, SIG_IGN);
    theRemoteLinkKeepsThePaneLinkContractAgainstADaemon(argv[1]);
    if (failures == 0)
        std::printf("remote_pane_link_e2e: the remote link over a real daemon kept the PaneLink contract, sized the host's pane "
                    "to the viewport it reported, and read as Ended once stopped\n");
    return failures == 0 ? 0 : 1;
}
