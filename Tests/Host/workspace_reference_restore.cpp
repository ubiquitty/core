// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/workspace_reference_restore.cpp — a client that saved a
// reference to a workspace, named or not, gets that workspace back after the
// host restarts, over the stdio bridge an ssh connection runs and over QUIC.
//
// A frontend that reopens a window after a relaunch holds what it saved: the
// daemon instance, the workspace id and the pane's session id. It does not
// hold the workspace's name, and the workspace may have none. The first
// daemon holds "work", or an unnamed workspace: a tab of one pane
// and a renamed tab split in two, each shell in a directory of its own. It is
// killed outright. The second daemon starts on the same socket and lists the
// workspace as restorable under the id it was recorded with.
//
// The client then reconnects the way macOS and iOS do through TerminalSession:
// a WorkspaceConnection pinned to the saved instance and workspace, with a pane
// core asking for the saved session, attach-only. The host recreates the
// workspace from its record under the same workspace id, says which instance
// it restored it from, and the client accepts that as its workspace coming
// back: the same tabs, split, tab title and directories, the pane bound to the
// restored workspace's focused session. A reference naming an instance that
// never held the workspace is refused and starts nothing. A second window
// saved with the same reference, reopened once the workspace runs again, is
// answered with the running workspace.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host, with
// QUIC.
//
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/WorkspaceConnection.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/QUICChannel.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using Ubiquitty::Session::SessionRow;
using Ubiquitty::Session::WorkspaceIntent;
using Ubiquitty::Session::WorkspaceIntentKind;
using Ubiquitty::Session::WorkspaceIntentResult;

namespace {

int failureCount = 0;

void check(bool ok, const std::string& what) // bool-ok: an assertion's outcome, as every Host smoke spells it
{
    if (!ok) {
        std::fprintf(stderr, "workspace_reference_restore: FAIL: %s\n", what.c_str());
        ++failureCount;
    }
}

/// Main-thread stand-in: the channel's I/O thread posts, the test pumps.
class TestDispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000.0)), std::move(work));
    }
    /// Runs every piece of work whose time has come, in the order it was posted.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::scoped_lock lock(m_mutex);
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
        const std::scoped_lock lock(m_mutex);
        m_queue.push_back({ .due = due, .work = std::move(work) });
    }

    std::mutex m_mutex;
    std::vector<Posted> m_queue;
};

/// Pumps until a predicate holds or the time runs out.
template<class Predicate> bool pumpUntil(TestDispatcher& dispatcher, int timeoutMilliseconds, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    dispatcher.pump();
    return predicate();
}

/// How a client reaches the daemon.
enum class Carrier : std::uint8_t {
    /// `ubiquitty-host --attach` over standard input and output: what an ssh
    /// connection runs on the host.
    Bridge,
    /// QUIC, through the bootstrap the ssh run performs, minting a token.
    QUIC,
};

const char* nameOf(Carrier carrier) { return carrier == Carrier::Bridge ? "bridge" : "quic"; }

/// Builds the carrier a frontend's factory builds for `carrier`.
class CarrierFactory final : public Session::ChannelFactory {
public:
    CarrierFactory(Carrier carrier, std::string hostBinary, std::string controlDirectory, std::uint16_t port)
        : m_carrier(carrier)
        , m_hostBinary(std::move(hostBinary))
        , m_controlDirectory(std::move(controlDirectory))
        , m_port(port)
    {
    }

    void createChannel(Session::TransportMode, Done done) override
    {
        const Transport::Origin origin { Transport::OriginKind::Daemon, "" };
        if (m_carrier == Carrier::Bridge) {
            done(std::make_unique<Transport::SubprocessChannel>(m_hostBinary + " --attach", origin), false);
            return;
        }
        std::string token;
        std::string pin;
        std::uint16_t mintedPort = 0;
        if (!mintQUICTokenViaBootstrap(m_hostBinary, m_controlDirectory, token, mintedPort, pin) || pin.size() != 32) {
            done(nullptr, false);
            return;
        }
        done(std::make_unique<Transport::QUICChannel>("127.0.0.1", mintedPort ? mintedPort : m_port, token, pin, origin), true);
    }

private:
    Carrier m_carrier;
    std::string m_hostBinary;
    std::string m_controlDirectory;
    std::uint16_t m_port;
};

class RecordingObserver final : public Session::RemoteSessionObserver {
public:
    void onScreenChanged() override { ++m_screens; }
    void onWorkspaceVerbResult(const WorkspaceIntentResult& result) override
    {
        ++m_verbResults;
        m_lastResult = result;
    }
    void onReattachRefused(uint32_t, Protocol::Sync::ByeReason reason) override
    {
        m_wasRefused = true;
        m_refusal = reason;
    }
    int m_screens = 0;
    int m_verbResults = 0;
    WorkspaceIntentResult m_lastResult {};
    bool m_wasRefused = false;
    Protocol::Sync::ByeReason m_refusal {};
};

std::unique_ptr<Ubiquitty::Terminal> makeTerm(int columns, int rows)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    return term;
}

/// A client that creates or drives sessions directly, as the first daemon's
/// owner does.
struct DirectClient {
    std::unique_ptr<Ubiquitty::Terminal> term;
    TestDispatcher dispatcher;
    CarrierFactory factory;
    RecordingObserver observer;
    Session::RemoteSessionCore core;

    DirectClient(Carrier carrier, const std::string& hostBinary, const std::string& home, std::uint16_t port, int columns, int rows)
        : term(makeTerm(columns, rows))
        , factory(carrier, hostBinary, home, port)
        , core(term.get(), &dispatcher, &factory, &observer)
    {
    }

    bool connect(std::uint32_t sessionID, int columns, int rows, std::uint64_t daemonID = 0)
    {
        core.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
        core.setExpectedDaemonInstanceID(daemonID);
        core.start(sessionID, columns, rows);
        return pumpUntil(dispatcher, 15000, [&] { return core.acknowledged() && observer.m_screens > 0; })
            && (!sessionID || core.sessionID() == sessionID);
    }

    bool perform(const WorkspaceIntent& verb)
    {
        const int before = observer.m_verbResults;
        const std::uint32_t request = core.sendWorkspaceVerb(verb);
        return request != 0 && pumpUntil(dispatcher, 5000, [&] { return observer.m_verbResults > before; })
            && observer.m_lastResult.requestID == request && observer.m_lastResult.wasPerformed;
    }

    void stop()
    {
        core.stop();
        dispatcher.pump();
    }
};

/// A client that reopens a saved window the way TerminalSession does: a
/// workspace carrier pinned to the saved instance and workspace, and a pane
/// core asking for the saved session.
struct ReopenedWindow {
    std::unique_ptr<Ubiquitty::Terminal> term;
    TestDispatcher* dispatcher = nullptr;
    std::shared_ptr<Session::WorkspaceConnection> workspace;
    RecordingObserver observer;
    std::unique_ptr<Session::RemoteSessionCore> pane;

    ReopenedWindow(Carrier carrier, const std::string& hostBinary, const std::string& home, std::uint16_t port, int columns, int rows)
        : term(makeTerm(columns, rows))
    {
        auto ownedDispatcher = std::make_unique<TestDispatcher>();
        dispatcher = ownedDispatcher.get();
        workspace = std::make_shared<Session::WorkspaceConnection>(std::move(ownedDispatcher), std::make_unique<CarrierFactory>(carrier, hostBinary, home, port));
    }

    void reopen(std::uint64_t daemonID, std::uint32_t workspaceID, std::uint32_t sessionID, int columns, int rows)
    {
        workspace->connection().setAttachWorkspaceID(workspaceID);
        workspace->connection().setExpectedDaemonInstanceID(daemonID);
        workspace->connection().setReattachPolicy(Session::ReattachPolicy::AttachOnly);
        pane = std::make_unique<Session::RemoteSessionCore>(term.get(), *workspace, &observer);
        pane->start(sessionID, columns, rows);
    }

    void stop()
    {
        if (pane)
            pane->stop();
        workspace->connection().stop();
        dispatcher->pump();
    }
};

/// A daemon this test started, and the only kind it ever signals.
struct Daemon {
    pid_t processID = -1;
};

Daemon startDaemon(const std::string& hostBinary, const std::string& socketPath, const std::string& logPath, std::uint16_t port)
{
    const std::string portArgument = std::to_string(port);
    const pid_t child = ::fork();
    if (child == 0) {
        const int log = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0)
            ::dup2(log, STDERR_FILENO);
        // A shell that reads no rc file of the person running the test and
        // reports no directory by OSC 7, so the recorded directory is the one
        // the host asks the process for.
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        ::execl(hostBinary.c_str(), hostBinary.c_str(), "--daemon", "--quic-port", portArgument.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    Daemon daemon { child };
    check(child > 0 && HostTest::waitForDaemonReady(socketPath), "a daemon accepted a connection");
    return daemon;
}

/// Kills a daemon this test started the way a crash ends one: no teardown runs.
void crash(Daemon& daemon)
{
    if (daemon.processID <= 0)
        return;
    ::kill(daemon.processID, SIGKILL);
    ::waitpid(daemon.processID, nullptr, 0);
    daemon.processID = -1;
}

std::string readFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// Waits for the record file to hold every needle.
bool waitForRecord(const std::string& path, const std::vector<std::string>& needles, int timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    for (;;) {
        const std::string contents = readFile(path);
        if (std::ranges::all_of(needles, [&](const std::string& needle) { return contents.find(needle) != std::string::npos; }))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/// Admits a trusted local Control reader without creating or attaching a pane.
bool admitControl(HostTest::Connection& connection)
{
    namespace Sync = Protocol::Sync;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x726566657265 };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request.target = Sync::AdmissionTarget::Control;
    if (!HostTest::sendGreeting(connection, std::move(greeting)))
        return false;
    std::string body;
    if (!HostTest::waitFor(connection, Sync::FrameKind::Welcome, body, 5000))
        return false;
    const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    return welcome && welcome->admission == Sync::AttachmentAdmission::Granted && welcome->selection.target == Sync::AdmissionTarget::Control;
}

/// Asks the daemon for its session listing over the trusted local bridge.
std::optional<std::vector<SessionRow>> listSessions(const std::string& hostBinary, const std::string& home)
{
    HostTest::LocalBridgeConnection bridge = HostTest::connectDaemonAsLocalBridge(hostBinary, home);
    if (bridge.fileDescriptor < 0)
        return std::nullopt;
    HostTest::Connection connection;
    connection.fileDescriptor = bridge.fileDescriptor;
    std::optional<std::vector<SessionRow>> listed;
    if (admitControl(connection)) {
        if (auto rows = HostTest::readSessionDirectory(connection, 7, 5000))
            listed = std::move(*rows);
    }
    ::close(bridge.fileDescriptor);
    ::kill(bridge.bridgeProcessID, SIGTERM);
    ::waitpid(bridge.bridgeProcessID, nullptr, 0);
    return listed;
}

std::string makeDirectory(const std::string& path)
{
    ::mkdir(path.c_str(), 0700);
    char resolved[PATH_MAX];
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

std::string changeDirectoryCommand(const std::string& directory) { return "cd '" + directory + "'\r"; }

/// What the first daemon's client saved of the workspace.
struct SavedReference {
    std::uint64_t daemonInstanceID = 0;
    std::uint32_t workspaceID = 0;
    std::uint32_t sessionID = 0;
};

/// One run: build the workspace, crash its daemon, start another, and reopen.
///
/// - Parameters:
///   - carrier: How every client reaches the daemon.
///   - hostBinary: The host.
///   - workspaceName: The workspace's name, empty for an unnamed one.
void run(Carrier carrier, const std::string& hostBinary, const std::string& workspaceName)
{
    const std::string label = std::string(nameOf(carrier)) + (workspaceName.empty() ? " unnamed" : " named");
    const auto expect = [&](bool ok, const std::string& what) { check(ok, label + ": " + what); };

    std::string rootTemplate = "/tmp/ubq-reference-restore.XXXXXX";
    if (!::mkdtemp(rootTemplate.data())) {
        check(false, label + ": a scratch directory was made");
        return;
    }
    const std::string root = rootTemplate;
    const std::string home = root + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
    const std::string socketName = Host::socketLeafForBinary(hostBinary.c_str());
    const std::string socketPath = home + "/" + socketName;
    const std::string recordPath = socketPath + ".layouts";
    const std::string logPath = root + "/daemon.log";
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    const std::string firstDirectory = makeDirectory(root + "/first");
    const std::string secondDirectory = makeDirectory(root + "/second");
    const std::string thirdDirectory = makeDirectory(root + "/third pane");

    // ---- The first daemon holds the workspace: a tab of one pane, and a renamed tab split in two. ----
    SavedReference saved;
    Daemon first = startDaemon(hostBinary, socketPath, logPath, port);
    {
        DirectClient owner(carrier, hostBinary, home, port, 80, 24);
        if (!workspaceName.empty())
            owner.core.setWorkspace(workspaceName, false);
        expect(owner.connect(0, 80, 24), "the owner created the workspace");
        expect(pumpUntil(owner.dispatcher, 5000, [&] { return owner.core.workspace() != nullptr; }), "the owner was told the workspace");
        const auto* mirror = owner.core.workspace();
        const auto surface = mirror ? mirror->surfaceIDAt(0) : Session::Layout::invalidSurface;

        WorkspaceIntent addTab {};
        addTab.verb = WorkspaceIntentKind::AddTab;
        addTab.surfaceID = std::to_underlying(surface);
        addTab.paneID = std::to_underlying(owner.core.paneForSession(owner.core.sessionID()));
        expect(owner.perform(addTab), "AddTab minted a backed tab");
        const std::uint32_t tab = owner.observer.m_lastResult.tabID;
        const std::uint32_t secondSession = owner.observer.m_lastResult.sessionID;

        WorkspaceIntent split {};
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.surfaceID = std::to_underlying(surface);
        split.paneID = owner.observer.m_lastResult.paneID;
        expect(pumpUntil(owner.dispatcher, 5000, [&] {
            const auto* current = owner.core.workspace();
            return split.paneID && current && current->surfaceHolding(Session::Layout::PaneID { split.paneID }) == surface;
        }),
            "the split source arrived in the authoritative structure");
        split.orientation = static_cast<uint8_t>(Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        expect(owner.perform(split), "the new tab's pane was split");
        const std::uint32_t thirdSession = owner.observer.m_lastResult.sessionID;

        WorkspaceIntent rename {};
        rename.verb = WorkspaceIntentKind::RenameTab;
        rename.surfaceID = std::to_underlying(surface);
        rename.tabID = tab;
        rename.title = "logs";
        expect(owner.perform(rename), "the split tab was renamed");

        DirectClient second(carrier, hostBinary, home, port, 100, 30);
        expect(second.connect(secondSession, 100, 30, owner.core.daemonInstanceID()), "a client attached to the second pane");
        DirectClient third(carrier, hostBinary, home, port, 120, 40);
        expect(third.connect(thirdSession, 120, 40, owner.core.daemonInstanceID()), "a client attached to the third pane");
        owner.core.sendInput(changeDirectoryCommand(firstDirectory));
        second.core.sendInput(changeDirectoryCommand(secondDirectory));
        third.core.sendInput(changeDirectoryCommand(thirdDirectory));
        for (DirectClient* client : { &owner, &second, &third })
            expect(pumpUntil(client->dispatcher, 10000, [&] { return client->core.inputStatus().waiting == 0; }), "a typed line left its client");
        expect(waitForRecord(recordPath, { firstDirectory, secondDirectory, thirdDirectory, "logs" }, 10000),
            "the record holds every pane's directory and the tab's title");

        saved = { .daemonInstanceID = owner.core.daemonInstanceID(), .workspaceID = owner.core.workspaceID(), .sessionID = owner.core.sessionID() };
        expect(saved.daemonInstanceID && saved.workspaceID && saved.sessionID, "the owner holds a whole reference to save");

        crash(first);
        owner.stop();
        second.stop();
        third.stop();
    }

    // ---- The second daemon restores the workspace for the saved reference. ----
    Daemon second = startDaemon(hostBinary, socketPath, logPath, port);
    {
        const auto listed = listSessions(hostBinary, home);
        expect(listed && listed->size() == 1 && listed->front().isRestorable() && listed->front().workspaceName == workspaceName,
            "the new daemon lists the workspace as restorable, and nothing else");
        expect(listed && !listed->empty() && listed->front().id == saved.workspaceID,
            "the restorable entry keeps the workspace id it was recorded with");

        // A reference naming an instance that never held the workspace is refused
        // and starts nothing: not a blank session, not the workspace.
        {
            ReopenedWindow stranger(carrier, hostBinary, home, port, 90, 28);
            stranger.reopen(saved.daemonInstanceID ^ 0x5a5a5a5a5a5a5a5aull, saved.workspaceID, saved.sessionID, 90, 28);
            expect(pumpUntil(*stranger.dispatcher, 15000, [&] { return stranger.pane->state() == Session::State::Failed || stranger.observer.m_wasRefused || stranger.workspace->connection().state() == Session::State::Failed; }),
                "a reference to another instance was refused");
            expect(!stranger.pane->acknowledged(), "the refused reference attached nothing");
            stranger.stop();
            const auto after = listSessions(hostBinary, home);
            expect(after && after->size() == 1 && after->front().isRestorable(), "the refusal left the workspace restorable and started no session");
        }

        ReopenedWindow window(carrier, hostBinary, home, port, 90, 28);
        window.reopen(saved.daemonInstanceID, saved.workspaceID, saved.sessionID, 90, 28);
        auto& physical = window.workspace->connection();
        const bool didAttach = pumpUntil(*window.dispatcher, 20000, [&] { return window.pane->acknowledged() && physical.workspace() != nullptr && window.observer.m_screens > 0; });
        expect(didAttach, "the saved reference reattached");
        expect(!window.observer.m_wasRefused, "the saved reference was not refused");
        expect(physical.workspaceID() == saved.workspaceID, "the workspace came back under the id the client saved");
        expect(physical.daemonInstanceID() && physical.daemonInstanceID() != saved.daemonInstanceID, "the workspace is held by the instance that runs now");
        expect(physical.isWorkspaceRestoredAfterHostRestart(), "the workspace says it was restored after the host restarted");
        expect(physical.workspaceName() == workspaceName, "under its name, or none");
        expect(window.pane->sessionID() == physical.sessionID() && window.pane->sessionID() != 0, "the window's pane is bound to the restored workspace's session");
        if (carrier == Carrier::QUIC)
            expect(physical.isUsingQUIC(), "the reattach rode QUIC");

        std::vector<std::pair<std::uint32_t, std::string>> expected;
        if (const auto* mirror = physical.workspace()) {
            const auto surface = mirror->surfaceIDAt(0);
            expect(mirror->surfaceCount() == 1 && mirror->tabCount(surface) == 2, "one window of two tabs");
            const auto firstTab = mirror->tabAt(surface, 0);
            const auto secondTab = mirror->tabAt(surface, 1);
            const auto* firstTree = mirror->paneTree(surface, firstTab);
            const auto* secondTree = mirror->paneTree(surface, secondTab);
            expect(firstTree && firstTree->paneCount() == 1, "the first tab is one pane");
            expect(secondTree && secondTree->paneCount() == 2, "the second tab is split in two");
            expect(mirror->tabTitle(surface, secondTab) == "logs", "the second tab keeps its title");
            if (firstTree && secondTree) {
                Panes::forEachLeaf(*firstTree, [&](int pane) { expected.emplace_back(physical.sessionForPane(static_cast<Session::Layout::PaneID>(pane)), firstDirectory); });
                const std::string splitDirectories[] = { secondDirectory, thirdDirectory };
                std::size_t leaf = 0;
                Panes::forEachLeaf(*secondTree, [&](int pane) {
                    if (leaf < std::size(splitDirectories))
                        expected.emplace_back(physical.sessionForPane(static_cast<Session::Layout::PaneID>(pane)), splitDirectories[leaf]);
                    ++leaf;
                });
            }
        }
        expect(expected.size() == 3, "three panes came back");
        std::optional<std::vector<SessionRow>> listedAfter;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto isSettled = [&] {
            return listedAfter && std::ranges::all_of(expected, [&](const auto& pane) {
                return std::ranges::any_of(*listedAfter, [&](const SessionRow& entry) { return entry.id == pane.first && entry.currentDirectory == pane.second; });
            });
        };
        do {
            window.dispatcher->pump();
            listedAfter = listSessions(hostBinary, home);
            if (isSettled())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        expect(isSettled(), "every restored pane's shell runs in its recorded directory");
        expect(listedAfter && std::ranges::none_of(*listedAfter, [](const SessionRow& entry) { return entry.isRestorable(); }), "nothing is left restorable once it is restored");

        // A second window saved with the same reference, reopened once the
        // workspace runs again, is answered with that workspace as it runs:
        // the same id, its running sessions, nothing recreated.
        {
            ReopenedWindow other(carrier, hostBinary, home, port, 90, 28);
            other.reopen(saved.daemonInstanceID, saved.workspaceID, saved.sessionID, 90, 28);
            auto& again = other.workspace->connection();
            expect(pumpUntil(*other.dispatcher, 20000, [&] { return other.pane->acknowledged() && again.workspace() != nullptr; }),
                "a second window with the same reference reattached");
            expect(!other.observer.m_wasRefused, "the second reference was not refused");
            expect(again.workspaceID() == saved.workspaceID && again.daemonInstanceID() == physical.daemonInstanceID(),
                "the second reference reached the running workspace on the instance that runs now");
            const auto listedSecond = listSessions(hostBinary, home);
            expect(listedSecond && listedAfter && listedSecond->size() == listedAfter->size(), "the second reference recreated nothing");
            other.stop();
        }

        crash(second);
        window.stop();
    }

    if (failureCount == 0) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    } else {
        std::fprintf(stderr, "workspace_reference_restore: %s: the daemons' log and the record are left in %s\n", label.c_str(), root.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBinary = argv[1];
    for (const Carrier carrier : { Carrier::Bridge, Carrier::QUIC }) {
        for (const char* workspaceName : { "work", "" })
            run(carrier, hostBinary, workspaceName);
    }
    if (failureCount != 0)
        return 1;
    std::printf("workspace_reference_restore: a saved reference to a workspace, named or unnamed, brought it back after the host restarted, under the same id with its tabs, split, title and directories, over the bridge and over QUIC; a reference to another instance was refused and started nothing\n");
    return 0;
}
