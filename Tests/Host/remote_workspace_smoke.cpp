// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/remote_workspace_smoke.cpp — a workspace over Sync, end to end,
// with the real client stack: Session::RemoteSessionCore over a real
// Transport::SubprocessChannel bridged by `ubiquitty-host --attach` to a real
// forked ubiquitty-host daemon. The stack the macOS app uses, minus ssh.
//
// Two clients share one hosted workspace. The first attaches under a name and
// is told the one-pane projection; a verb it sends mints a tab whose pane the
// host backs with a session of its own; the second client attaches to that
// pane's session and is told the same tree; a verb from the second reaches the
// first; a shadow Layout::Workspace given the same verbs describes what both
// mirrors hold, which is the local outcome of the same verbs; closing the tab
// ends the second client's session, and it is told so.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"
#include "WorkspaceMirrorFixture.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using Ubiquitty::Session::WorkspaceIntent;
using Ubiquitty::Session::WorkspaceIntentKind;
using Ubiquitty::Session::WorkspaceIntentResult;

namespace {

using WorkspaceVerbMessage = Ubiquitty::Session::WorkspaceIntent;
using WorkspaceVerbResultMessage = Ubiquitty::Session::WorkspaceIntentResult;

int g_failures = 0;

void check(bool ok, const char* what) // bool-ok: an assertion's outcome, as every Host smoke spells it
{
    if (!ok) {
        std::fprintf(stderr, "remote_workspace_smoke: FAIL: %s\n", what);
        ++g_failures;
    }
}

std::unique_ptr<Ubiquitty::Terminal> makeTerm()
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    return t;
}

/// Main-thread stand-in: the channel's I/O thread posts, the test pumps.
class TestDispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }
    // A delay is honoured rather than fired at once: the core arms timers for
    // moments that have not come, and one run early is armed again at once.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000.0)), std::move(work));
    }
    // Runs every piece of work whose time has come, in the order it was posted.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto due = std::ranges::find_if(m_queue, [&](const Posted& posted) { return posted.due <= now; });
                if (due == m_queue.end())
                    return;
                work = std::move(due->work);
                m_queue.erase(due);
            }
            std::move(work)();
        }
    }

private:
    struct Posted {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
    };

    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({ .due = due, .work = std::move(work) });
    }

    std::mutex m_mutex;
    std::vector<Posted> m_queue;
};

/// The channel the macOS factory builds: `ubiquitty-host --attach`, which finds
/// the daemon socket through the inherited UBIQUITTY_HOME.
class AttachChannelFactory final : public Session::ChannelFactory {
public:
    explicit AttachChannelFactory(std::string command)
        : m_command(std::move(command))
    {
    }
    void createChannel(Session::TransportMode, Done done) override
    {
        done(std::make_unique<Transport::SubprocessChannel>(m_command, Transport::Origin { Transport::OriginKind::Daemon, "" }), false);
    }

private:
    std::string m_command;
};

class RecordingObserver final : public Session::RemoteSessionObserver {
public:
    void onScreenChanged() override { ++m_screens; }
    void onWorkspaceChanged() override { ++m_workspaceChanges; }
    void onWorkspaceVerbResult(const WorkspaceVerbResultMessage& result) override
    {
        ++m_verbResults;
        m_lastResult = result;
    }
    void onExit() override { m_didExit = true; }
    int m_screens = 0;
    int m_workspaceChanges = 0;
    int m_verbResults = 0;
    WorkspaceVerbResultMessage m_lastResult {};
    bool m_didExit = false;
};

template<class Pred> bool pumpUntil(TestDispatcher& dispatcher, int timeoutMs, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    dispatcher.pump();
    return pred();
}

/// One client of the daemon: a terminal, a dispatcher, the real core.
struct Client {
    std::unique_ptr<Ubiquitty::Terminal> term = makeTerm();
    TestDispatcher dispatcher;
    AttachChannelFactory factory;
    RecordingObserver observer;
    Session::RemoteSessionCore core;

    explicit Client(const std::string& attachCommand)
        : factory(attachCommand)
        , core(term.get(), &dispatcher, &factory, &observer)
    {
    }

    /// Pumps until the core has a mirror newer than `changesSeen`.
    bool awaitWorkspaceChange(int changesSeen)
    {
        return pumpUntil(dispatcher, 5000, [&] { return observer.m_workspaceChanges > changesSeen; });
    }

    /// The mirror as text, every leaf at the host's 80x24, so two mirrors
    /// and a shadow compare as strings.
    std::string describe() const
    {
        return UbiquittyTest::mirrorText(*core.workspace(), [this](Session::Layout::PaneID pane) { return UbiquittyTest::MirrorPane { .sessionID = core.sessionForPane(pane), .columns = 80, .rows = 24 }; });
    }
};

/// Two descriptions of the same topology, sessions included.
bool sameLayout(const std::string& a, const std::string& b) { return a == b; }

/// Everything the lane asserts; returns early on a fatal miss so main still
/// tears the daemon down, whose inherited pipes would otherwise hold the runner.
void exercise(const std::string& attachCommand)
{
    // The shadow: what the same verbs do to a local workspace.
    auto shadow = std::make_unique<Session::Layout::Workspace>();
    std::vector<std::pair<uint32_t, uint32_t>> shadowSessions;
    const auto shadowSessionForPane = [](void* context, Session::Layout::PaneID pane) -> uint32_t {
        for (const auto& [bound, session] : *static_cast<const std::vector<std::pair<uint32_t, uint32_t>>*>(context))
            if (bound == std::to_underlying(pane))
                return session;
        return 0;
    };
    const auto describeShadow = [&](uint32_t) {
        return UbiquittyTest::mirrorText(*shadow, [&](Session::Layout::PaneID pane) { return UbiquittyTest::MirrorPane { .sessionID = shadowSessionForPane(&shadowSessions, pane), .columns = 80, .rows = 24 }; });
    };

    // ---- A creates the named workspace and is told the one-pane projection. ----
    Client a(attachCommand);
    a.core.setWorkspace("shared", false);
    a.core.start(0, 80, 24);
    check(pumpUntil(a.dispatcher, 5000, [&] { return a.core.acknowledged() && a.observer.m_screens > 0; }), "A: connected + first checkpoint applied");
    check(a.awaitWorkspaceChange(0), "A: told the workspace on attach");
    check(a.core.workspace() != nullptr, "A: holds a mirror");
    if (!a.core.workspace())
        return;
    const Session::Layout::SurfaceID surface = a.core.workspace()->surfaceIDAt(0);
    const Session::Layout::PaneID rootPane = a.core.workspace()->focusedPane(surface);
    check(a.core.workspace()->surfaceCount() == 1 && a.core.workspace()->tabCount(surface) == 1, "A: the projection is one surface, one tab");
    check(a.core.sessionForPane(rootPane) == a.core.sessionID(), "A: the one pane is bound to A's own session");
    shadowSessions.emplace_back(std::to_underlying(shadow->focusedPane(shadow->surfaceIDAt(0))), a.core.sessionID());
    check(sameLayout(a.describe(), describeShadow(a.core.sessionID())), "A: the mirror is the fresh local workspace");

    // ---- A adds a tab; the host mints a pane and a session behind it. ----
    WorkspaceVerbMessage addTab {};
    addTab.verb = WorkspaceIntentKind::AddTab;
    addTab.surfaceID = std::to_underlying(surface);
    addTab.paneID = std::to_underlying(rootPane);
    const int changesBeforeTab = a.observer.m_workspaceChanges;
    const uint32_t addTabRequest = a.core.sendWorkspaceVerb(addTab);
    check(addTabRequest != 0, "A: the verb was sent");
    if (!addTabRequest)
        return;
    check(pumpUntil(a.dispatcher, 5000, [&] { return a.observer.m_verbResults > 0; }), "A: the verb was answered");
    const WorkspaceVerbResultMessage& minted = a.observer.m_lastResult;
    check(minted.requestID == addTabRequest && minted.wasPerformed, "A: AddTab was performed and answers the request sent");
    check(minted.tabID != 0 && minted.paneID != 0 && minted.sessionID != 0 && minted.sessionID != a.core.sessionID(), "A: the new tab's pane has a session of its own");
    check(a.awaitWorkspaceChange(changesBeforeTab), "A: the new topology followed");
    const Session::Layout::TabID shadowTab = shadow->addTab(shadow->surfaceIDAt(0));
    shadowSessions.emplace_back(std::to_underlying(shadow->focusedPane(shadow->surfaceIDAt(0))), minted.sessionID);
    check(std::to_underlying(shadowTab) == minted.tabID, "A: the host minted the tab id the local model mints");
    check(sameLayout(a.describe(), describeShadow(a.core.sessionID())), "A: after AddTab the mirror is what the same verb does locally");
    check(a.core.sessionForPane(static_cast<Session::Layout::PaneID>(minted.paneID)) == minted.sessionID, "A: the mirror binds the new pane to the minted session");

    // ---- B attaches to the new pane's session and is told the same tree. ----
    Client b(attachCommand);
    b.core.start(minted.sessionID, 80, 24);
    check(pumpUntil(b.dispatcher, 5000, [&] { return b.core.acknowledged() && b.observer.m_screens > 0; }), "B: attached to the pane's session");
    check(b.core.sessionID() == minted.sessionID, "B: is the minted session");
    check(b.awaitWorkspaceChange(0), "B: told the workspace on attach");
    check(b.core.workspace() != nullptr && sameLayout(b.describe(), a.describe()), "B: holds the tree A built, the workspace the pane is in");

    // ---- B splits its pane; both mirrors and the shadow agree. ----
    WorkspaceVerbMessage split {};
    split.verb = WorkspaceIntentKind::SplitFocusedPane;
    split.surfaceID = std::to_underlying(surface);
    split.paneID = std::to_underlying(b.core.paneForSession(b.core.sessionID()));
    split.orientation = static_cast<uint8_t>(Ubiquitty::Panes::Orientation::Vertical);
    split.ratio = 0.5f;
    const int aChangesBeforeSplit = a.observer.m_workspaceChanges;
    const int bChangesBeforeSplit = b.observer.m_workspaceChanges;
    const uint32_t splitRequest = b.core.sendWorkspaceVerb(split);
    check(splitRequest != 0, "B: the split was sent");
    if (!splitRequest)
        return;
    check(pumpUntil(b.dispatcher, 5000, [&] { return b.observer.m_verbResults > 0; }), "B: the split was answered");
    const WorkspaceVerbResultMessage& splitResult = b.observer.m_lastResult;
    check(splitResult.requestID == splitRequest && splitResult.wasPerformed && splitResult.paneID != 0 && splitResult.sessionID != 0, "B: the split minted a backed pane");
    check(b.awaitWorkspaceChange(bChangesBeforeSplit), "B: told the split topology");
    check(a.awaitWorkspaceChange(aChangesBeforeSplit), "A: told the topology B changed");
    const Session::Layout::PaneID shadowSplit = shadow->splitFocusedPane(shadow->surfaceIDAt(0), Ubiquitty::Panes::Orientation::Vertical, 0.5f);
    shadowSessions.emplace_back(std::to_underlying(shadowSplit), splitResult.sessionID);
    check(std::to_underlying(shadowSplit) == splitResult.paneID, "B: the host minted the pane id the local model mints");
    check(sameLayout(a.describe(), describeShadow(a.core.sessionID())) && sameLayout(b.describe(), a.describe()), "both mirrors hold what the same verbs do locally");

    // ---- A closes the tab; the sessions behind it end and B is told. ----
    WorkspaceVerbMessage closeTab {};
    closeTab.verb = WorkspaceIntentKind::CloseTab;
    closeTab.surfaceID = std::to_underlying(surface);
    closeTab.tabID = minted.tabID;
    const int aChangesBeforeClose = a.observer.m_workspaceChanges;
    const int aResultsBeforeClose = a.observer.m_verbResults;
    check(a.core.sendWorkspaceVerb(closeTab) != 0, "A: the close was sent");
    check(pumpUntil(a.dispatcher, 5000, [&] { return a.observer.m_verbResults > aResultsBeforeClose; }), "A: the close was answered");
    check(a.observer.m_lastResult.wasPerformed, "A: CloseTab was performed");
    check(a.awaitWorkspaceChange(aChangesBeforeClose), "A: told the topology after the close");
    check(shadow->closeTab(shadow->surfaceIDAt(0), shadowTab), "the shadow closes the tab");
    check(sameLayout(a.describe(), describeShadow(a.core.sessionID())), "A: after CloseTab the mirror is what the same verb does locally");
    check(pumpUntil(b.dispatcher, 5000, [&] { return b.observer.m_didExit; }), "B: its session ended with the tab it drew into");

    a.core.stop();
    a.dispatcher.pump();
    b.core.stop();
    b.dispatcher.pump();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBin = argv[1];

    // An isolated UBIQUITTY_HOME, inherited by the daemon and every attach child.
    char homeTemplate[] = "/tmp/mt-remote-workspace.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    ::mkdir(macHome.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
    const std::string socketPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBin.c_str());

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        char* args[] = { const_cast<char*>(hostBin.c_str()), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBin.c_str(), args);
        ::_exit(127);
    }
    check(Ubiquitty::HostTest::waitForDaemonReady(socketPath), "daemon accepted a connection");
    const std::string attachCommand = hostBin + " --attach";

    exercise(attachCommand);

    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 200; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, 0);

    if (g_failures == 0)
        std::printf("remote_workspace_smoke: two RemoteSessionCores over the real daemon — projection, AddTab, attach to a pane, split, close — all passed\n");
    return g_failures == 0 ? 0 : 1;
}
