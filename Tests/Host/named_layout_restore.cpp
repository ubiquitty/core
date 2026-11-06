// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/named_layout_restore.cpp — a named workspace outlives the daemon
// that held it, end to end, with the real client stack over real daemons.
//
// The first daemon holds a workspace named "work": one window, a tab of one
// pane and a renamed tab split in two, each pane's shell in a directory of its
// own and the split's panes at grids of their own. It is killed outright, as a
// crash kills it. A second daemon on the same socket lists the workspace as
// restorable and nothing else, and attaching to it by name hands back the same
// topology with a fresh shell in each recorded directory at each recorded grid,
// marked as restored after the host restarted.
//
// Then the lifetimes around it. A named session whose shell exits on its own
// leaves no record behind, so a third daemon lists only "work"; terminating the
// restorable entry forgets it and removes the file. A record file that does
// not decode is read as no record at all, logged, and a HELLO under the name
// creates a fresh workspace.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/DetachedDaemons.h"
#include "Host/HostConnection.h"
#include "Host/TerminationClient.h"
#include "Platform/Host/HostSocketName.h"

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
#include <sstream>
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

using WorkspaceVerbMessage = Ubiquitty::Session::WorkspaceIntent;
using WorkspaceVerbResultMessage = Ubiquitty::Session::WorkspaceIntentResult;

int failureCount = 0;

void check(bool ok, const char* what) // bool-ok: an assertion's outcome, as every Host smoke spells it
{
    if (!ok) {
        std::fprintf(stderr, "named_layout_restore: FAIL: %s\n", what);
        ++failureCount;
    }
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

/// The channel the macOS factory builds: `ubiquitty-host --attach`.
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

std::unique_ptr<Ubiquitty::Terminal> makeTerm(int columns, int rows)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    return term;
}

/// One client of the daemon: a terminal at its own grid, a dispatcher, the real core.
struct Client {
    std::unique_ptr<Ubiquitty::Terminal> term;
    TestDispatcher dispatcher;
    AttachChannelFactory factory;
    RecordingObserver observer;
    std::string expectedSocketName;
    Session::RemoteSessionCore core;

    Client(const std::string& attachCommand, std::string socketName, int columns, int rows)
        : term(makeTerm(columns, rows))
        , factory(attachCommand)
        , expectedSocketName(std::move(socketName))
        , core(term.get(), &dispatcher, &factory, &observer)
    {
    }

    bool connect(std::uint32_t sessionID, int columns, int rows, std::uint64_t daemonID = 0)
    {
        if (sessionID && !daemonID)
            return false;
        core.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
        core.setExpectedDaemonInstanceID(daemonID);
        core.start(sessionID, columns, rows);
        return pumpUntil(dispatcher, 10000, [&] { return core.acknowledged() && observer.m_screens > 0; })
            && core.daemonInstanceID() && core.socketName() == expectedSocketName
            && (!daemonID || core.daemonInstanceID() == daemonID) && (!sessionID || core.sessionID() == sessionID);
    }

    /// Sends one verb and waits for its answer.
    bool perform(const WorkspaceVerbMessage& verb)
    {
        const int before = observer.m_verbResults;
        const std::uint32_t request = core.sendWorkspaceVerb(verb);
        return request != 0 && pumpUntil(dispatcher, 5000, [&] { return observer.m_verbResults > before; }) && observer.m_lastResult.requestID == request && observer.m_lastResult.wasPerformed;
    }

    void stop()
    {
        core.stop();
        dispatcher.pump();
    }
};

/// A daemon this test started, and the only kind it ever signals.
struct Daemon {
    pid_t processID = -1;
    std::string socketPath;
};

Daemon startDaemon(const std::string& hostBinary, const std::string& socketPath, const std::string& logPath)
{
    const pid_t child = ::fork();
    if (child == 0) {
        const int log = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0)
            ::dup2(log, STDERR_FILENO);
        // A shell that reads no rc file of the person running the test and
        // reports no directory by OSC 7, so the directory the host records is
        // the one it asks the process for.
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        char* arguments[] = { const_cast<char*>(hostBinary.c_str()), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBinary.c_str(), arguments);
        ::_exit(127);
    }
    Daemon daemon { .processID = child, .socketPath = socketPath };
    check(child > 0 && Ubiquitty::HostTest::waitForDaemonReady(socketPath), "a daemon accepted a connection");
    return daemon;
}

/// Kills a daemon this test started the way a crash ends one: no teardown runs.
///
/// Nothing the daemon started outlives it. A process it forked holds its
/// socket's startup lock until that process execs, and a daemon a client's
/// bridge started in its place would hold the lock as its own, so once the
/// daemon is reaped the lock is free, or something the run never ends is
/// still there.
void crash(Daemon& daemon)
{
    if (daemon.processID <= 0)
        return;
    ::kill(daemon.processID, SIGKILL);
    ::waitpid(daemon.processID, nullptr, 0);
    daemon.processID = -1;
    const std::string lockPath = daemon.socketPath + ".lock";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Ubiquitty::HostTest::isStartupLockHeld(lockPath) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(!Ubiquitty::HostTest::isStartupLockHeld(lockPath), "nothing a killed daemon started still holds its socket");
}

std::string readFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool fileExists(const std::string& path)
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

/// Waits for a file's contents to satisfy a predicate.
template<class Predicate> bool waitForFile(const std::string& path, int timeoutMilliseconds, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    for (;;) {
        if (predicate(fileExists(path) ? std::optional<std::string>(readFile(path)) : std::nullopt))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/// Admits a trusted local Control reader without creating or attaching a pane.
/// Its authenticated receipt may pin a later operation on listed entries.
std::optional<Protocol::Sync::ServingSelection> admitControl(HostTest::Connection& connection,
    const std::string& hostBinary, std::uint64_t daemonID = 0)
{
    namespace Sync = Protocol::Sync;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x6e616d65646c };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.request.expectedDaemonInstanceID = daemonID;
    if (!HostTest::sendGreeting(connection, std::move(greeting)))
        return std::nullopt;
    std::string body;
    if (!HostTest::waitFor(connection, Sync::FrameKind::Welcome, body, 5000))
        return std::nullopt;
    const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted || welcome->selection.target != Sync::AdmissionTarget::Control
        || welcome->selection.sessionID || welcome->selection.workspaceID || !welcome->selection.daemonInstanceID
        || welcome->selection.socketName != Host::socketLeafForBinary(hostBinary.c_str())
        || (daemonID && welcome->selection.daemonInstanceID != daemonID))
        return std::nullopt;
    return welcome->selection;
}

/// Asks the daemon for its session listing over the trusted local bridge.
std::optional<std::vector<SessionRow>> listSessions(const std::string& hostBinary, const std::string& home, std::uint64_t* daemonID = nullptr)
{
    using namespace Ubiquitty::HostTest;
    LocalBridgeConnection bridge = connectDaemonAsLocalBridge(hostBinary, home);
    if (bridge.fileDescriptor < 0)
        return std::nullopt;
    Connection connection;
    connection.fileDescriptor = bridge.fileDescriptor;
    std::optional<std::vector<SessionRow>> listed;
    if (const auto selection = admitControl(connection, hostBinary)) {
        if (const auto rows = readSessionDirectory(connection, 7, 5000)) {
            listed = *rows;
            if (daemonID)
                *daemonID = selection->daemonInstanceID;
        }
    }
    ::close(bridge.fileDescriptor);
    ::kill(bridge.bridgeProcessID, SIGTERM);
    ::waitpid(bridge.bridgeProcessID, nullptr, 0);
    return listed;
}

/// Ends a listed entry over the trusted local bridge.
void terminateSession(const std::string& hostBinary, const std::string& home, std::uint32_t sessionID, std::uint64_t daemonID)
{
    using namespace Ubiquitty::HostTest;
    LocalBridgeConnection bridge = connectDaemonAsLocalBridge(hostBinary, home);
    if (bridge.fileDescriptor < 0)
        return;
    Connection connection;
    connection.fileDescriptor = bridge.fileDescriptor;
    if (!daemonID || !admitControl(connection, hostBinary, daemonID))
        check(false, "terminate: the listing's exact daemon did not admit its Control request");
    else {
        TerminationClient termination(connection);
        // A remembered workspace holds no live session: removing it is
        // answered `forgotten` (Docs/sync.md), not `terminated`.
        check(termination.send(sessionID, "restore.terminate") && termination.wait() == Protocol::Control::TerminationOutcome::Forgotten,
            "terminate: the exact listed entry returned its correlated typed completion");
        const auto remaining = readSessionDirectory(connection, 8, 5000);
        check(remaining && std::ranges::none_of(*remaining, [&](const auto& row) { return row.id == sessionID; }),
            "terminate: the same admitted connection no longer lists the exact entry");
    }
    ::close(bridge.fileDescriptor);
    ::kill(bridge.bridgeProcessID, SIGTERM);
    ::waitpid(bridge.bridgeProcessID, nullptr, 0);
}

const SessionRow* listedSession(const std::vector<SessionRow>& listed, std::uint32_t sessionID)
{
    for (const auto& entry : listed)
        if (entry.id == sessionID)
            return &entry;
    return nullptr;
}

std::size_t countRestorable(const std::vector<SessionRow>& listed)
{
    std::size_t count = 0;
    for (const auto& entry : listed)
        count += entry.isRestorable() ? 1 : 0;
    return count;
}

std::string makeDirectory(const std::string& path)
{
    ::mkdir(path.c_str(), 0700);
    char resolved[PATH_MAX];
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

std::string changeDirectoryCommand(const std::string& directory) { return "cd '" + directory + "'\r"; }

/// What one pane of the arrangement is expected to come back as.
struct ExpectedPane {
    std::string directory;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBinary = argv[1];

    char homeTemplate[] = "/tmp/ubq-named-layout.XXXXXX";
    char* temporary = ::mkdtemp(homeTemplate);
    if (!temporary)
        return 1;
    const std::string root = temporary;
    const std::string home = root + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(hostBinary.c_str());
    const std::string socketPath = home + "/" + socketName;
    const std::string recordPath = socketPath + ".layouts";
    const std::string logPath = root + "/daemon.log";
    const std::string attachCommand = hostBinary + " --attach";

    const std::string firstDirectory = makeDirectory(root + "/first");
    const std::string secondDirectory = makeDirectory(root + "/second");
    const std::string thirdDirectory = makeDirectory(root + "/third pane");

    // ---- The first daemon holds "work": a tab of one pane, and a renamed tab split in two. ----
    Daemon first = startDaemon(hostBinary, socketPath, logPath);
    std::uint32_t secondSession = 0;
    std::uint32_t thirdSession = 0;
    std::string recordedBytes;
    {
        Client owner(attachCommand, socketName, 80, 24);
        owner.core.setWorkspace("work", false);
        check(owner.connect(0, 80, 24), "work: the owner created the named workspace");
        check(pumpUntil(owner.dispatcher, 5000, [&] { return owner.core.workspace() != nullptr; }), "work: the owner was told the workspace");
        const auto* mirror = owner.core.workspace();
        const auto surface = mirror ? mirror->surfaceIDAt(0) : Session::Layout::invalidSurface;

        WorkspaceVerbMessage addTab {};
        addTab.verb = WorkspaceIntentKind::AddTab;
        addTab.surfaceID = std::to_underlying(surface);
        addTab.paneID = std::to_underlying(owner.core.paneForSession(owner.core.sessionID()));
        check(owner.perform(addTab), "work: AddTab minted a backed tab");
        const std::uint32_t tab = owner.observer.m_lastResult.tabID;
        secondSession = owner.observer.m_lastResult.sessionID;

        WorkspaceVerbMessage split {};
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.surfaceID = std::to_underlying(surface);
        split.paneID = owner.observer.m_lastResult.paneID;
        check(pumpUntil(owner.dispatcher, 5000, [&] {
            const auto* current = owner.core.workspace();
            return split.paneID && current && current->surfaceHolding(Session::Layout::PaneID { split.paneID }) == surface;
        }),
            "work: the typed split source arrived in the authoritative structure");
        split.orientation = static_cast<uint8_t>(Ubiquitty::Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        check(owner.perform(split), "work: the new tab's pane was split");
        thirdSession = owner.observer.m_lastResult.sessionID;

        WorkspaceVerbMessage rename {};
        rename.verb = WorkspaceIntentKind::RenameTab;
        rename.surfaceID = std::to_underlying(surface);
        rename.tabID = tab;
        rename.title = "logs";
        check(owner.perform(rename), "work: the split tab was renamed");

        // Each split pane is driven by a client of its own, which states the
        // grid it lays the pane out at. A pane beside another takes that
        // Viewport, never the size of the window a client attached with.
        Client second(attachCommand, socketName, 100, 30);
        check(second.connect(secondSession, 100, 30, owner.core.daemonInstanceID()), "work: a client attached to the second pane");
        second.core.setSize(100, 30);
        Client third(attachCommand, socketName, 120, 40);
        check(third.connect(thirdSession, 120, 40, owner.core.daemonInstanceID()), "work: a client attached to the third pane");
        third.core.setSize(120, 40);

        owner.core.sendInput(changeDirectoryCommand(firstDirectory));
        second.core.sendInput(changeDirectoryCommand(secondDirectory));
        third.core.sendInput(changeDirectoryCommand(thirdDirectory));
        // A client keeps a line typed at a pane whose bind it has not read
        // yet and sends it when it has, which is its dispatcher's work: a
        // screen can change before the pane is bound, so having connected
        // does not say the line left.
        const std::pair<const char*, Client*> typists[] = { { "owner", &owner }, { "second", &second }, { "third", &third } };
        for (const auto& [name, client] : typists)
            check(pumpUntil(client->dispatcher, 10000, [&] { return client->core.inputStatus().waiting == 0; }), (std::string("work: the line typed at ") + name + "'s pane left its client").c_str());
        const bool didRecord = waitForFile(recordPath, 10000, [&](const std::optional<std::string>& contents) {
            return contents && contents->find(firstDirectory) != std::string::npos && contents->find(secondDirectory) != std::string::npos
                && contents->find(thirdDirectory) != std::string::npos && contents->find("logs") != std::string::npos;
        });
        check(didRecord, "work: the record holds every pane's directory and the tab's title");
        if (!didRecord) {
            // What stood instead: which directories the record holds, what
            // each client still holds unsent, and where the host says each
            // session's shell is.
            const std::string held = fileExists(recordPath) ? readFile(recordPath) : std::string();
            std::fprintf(stderr, "named_layout_restore: the record holds first=%d second=%d third=%d logs=%d (%zu bytes)\n",
                held.find(firstDirectory) != std::string::npos, held.find(secondDirectory) != std::string::npos,
                held.find(thirdDirectory) != std::string::npos, held.find("logs") != std::string::npos, held.size());
            for (const auto& [name, client] : typists) {
                const auto status = client->core.inputStatus();
                std::fprintf(stderr, "named_layout_restore: %s holds %zu input(s) waiting, %zu in doubt, %zu discarded\n", name, status.waiting, status.inDoubt, status.discarded);
            }
            if (const auto listed = listSessions(hostBinary, home)) {
                for (const SessionRow& entry : *listed)
                    std::fprintf(stderr, "named_layout_restore: the host lists session %u at %dx%d in \"%s\"\n", entry.id, static_cast<int>(entry.columns), static_cast<int>(entry.rows), entry.currentDirectory.c_str());
            }
        }
        recordedBytes = readFile(recordPath);
        struct stat status {};
        check(::stat(recordPath.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "work: the record is readable by its owner alone");

        crash(first);
        owner.stop();
        second.stop();
        third.stop();
    }

    // ---- The second daemon lists "work" as restorable and restores it on attach. ----
    Daemon second = startDaemon(hostBinary, socketPath, logPath);
    {
        const auto beforeAttach = listSessions(hostBinary, home);
        check(beforeAttach.has_value(), "restart: the new daemon answered a listing");
        if (beforeAttach) {
            check(beforeAttach->size() == 1 && countRestorable(*beforeAttach) == 1, "restart: the listing holds one entry, restorable");
            if (!beforeAttach->empty()) {
                const SessionRow& entry = beforeAttach->front();
                check(entry.workspaceName == "work" && entry.id != 0 && entry.workspaceID == entry.id, "restart: the restorable entry is \"work\", by an id of its own");
                check(entry.attendeeCount == 0 && !entry.wasRestoredAfterHostRestart(), "restart: nothing runs in the restorable entry yet");
            }
        }

        // The window is none of the recorded grids, and the pane it opens on
        // is one of a split: every pane keeps its own.
        Client owner(attachCommand, socketName, 90, 28);
        owner.core.setWorkspace("work", false);
        check(owner.connect(0, 90, 28), "restart: attaching by name answered");
        check(pumpUntil(owner.dispatcher, 5000, [&] { return owner.core.workspace() != nullptr; }), "restart: the client was told the workspace");
        check(owner.core.isWorkspaceRestoredAfterHostRestart(), "restart: the workspace says it was restored after the host restarted");
        check(owner.core.workspaceName() == "work", "restart: under its name");
        const auto* mirror = owner.core.workspace();
        std::vector<std::pair<std::uint32_t, ExpectedPane>> expected;
        if (mirror) {
            const auto surface = mirror->surfaceIDAt(0);
            check(mirror->surfaceCount() == 1 && mirror->tabCount(surface) == 2, "restart: one window of two tabs");
            const auto firstTab = mirror->tabAt(surface, 0);
            const auto secondTab = mirror->tabAt(surface, 1);
            const auto* firstTree = mirror->paneTree(surface, firstTab);
            const auto* secondTree = mirror->paneTree(surface, secondTab);
            check(firstTree && firstTree->paneCount() == 1, "restart: the first tab is one pane");
            check(secondTree && secondTree->paneCount() == 2, "restart: the second tab is split in two");
            check(mirror->tabTitle(surface, secondTab) == "logs", "restart: the second tab keeps its title");
            check(mirror->activeTab(surface) == secondTab, "restart: the tab that was active is active");
            if (firstTree && secondTree) {
                Panes::forEachLeaf(*firstTree, [&](int pane) {
                    expected.emplace_back(owner.core.sessionForPane(static_cast<Session::Layout::PaneID>(pane)), ExpectedPane { firstDirectory, 80, 24 });
                });
                std::vector<ExpectedPane> splitPanes { { secondDirectory, 100, 30 }, { thirdDirectory, 120, 40 } };
                std::size_t leaf = 0;
                Panes::forEachLeaf(*secondTree, [&](int pane) {
                    if (leaf < splitPanes.size())
                        expected.emplace_back(owner.core.sessionForPane(static_cast<Session::Layout::PaneID>(pane)), splitPanes[leaf]);
                    ++leaf;
                });
            }
        }
        check(expected.size() == 3, "restart: three panes came back");
        // The shells start in their directories before the program runs, and the
        // listing asks the process, so the last answer is the one compared.
        std::optional<std::vector<SessionRow>> listed;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            listed = listSessions(hostBinary, home);
            bool isSettled = listed.has_value();
            for (const auto& [sessionID, pane] : expected) {
                const SessionRow* entry = listed ? listedSession(*listed, sessionID) : nullptr;
                isSettled = isSettled && entry && entry->currentDirectory == pane.directory && entry->columns == pane.columns && entry->rows == pane.rows;
            }
            if (isSettled || std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        for (const auto& [sessionID, pane] : expected) {
            const SessionRow* entry = listed ? listedSession(*listed, sessionID) : nullptr;
            check(entry != nullptr, "restart: every restored pane is backed by a live session");
            if (!entry)
                continue;
            if (entry->currentDirectory != pane.directory)
                std::fprintf(stderr, "named_layout_restore: session %u runs in \"%s\", expected \"%s\"\n", sessionID, entry->currentDirectory.c_str(), pane.directory.c_str());
            check(entry->currentDirectory == pane.directory, "restart: a restored pane's shell runs in its recorded directory");
            if (entry->columns != pane.columns || entry->rows != pane.rows)
                std::fprintf(stderr, "named_layout_restore: session %u is %dx%d, expected %dx%d\n", sessionID, static_cast<int>(entry->columns), static_cast<int>(entry->rows), static_cast<int>(pane.columns), static_cast<int>(pane.rows));
            check(entry->columns == pane.columns && entry->rows == pane.rows, "restart: a restored pane has its recorded grid");
            check(entry->wasRestoredAfterHostRestart() && !entry->isRestorable(), "restart: a restored pane is live and says it was restored");
            check(entry->workspaceName == "work", "restart: a restored pane is in \"work\"");
        }
        check(listed && countRestorable(*listed) == 0, "restart: nothing is left restorable once it is restored");

        // ---- A workspace of one pane is its window: a second window opened on
        // it by name, at a size of its own, sizes the pane, and the split's
        // panes in "work" keep theirs. ----
        Client narrow(attachCommand, socketName, 80, 24);
        narrow.core.setWorkspace("alone", false);
        check(narrow.connect(0, 80, 24), "alone: created");
        Client wide(attachCommand, socketName, 90, 28);
        wide.core.setWorkspace("alone", false);
        check(wide.connect(0, 90, 28), "alone: a second window attached by name");
        const auto sizedBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const SessionRow* alone = nullptr;
        for (;;) {
            narrow.dispatcher.pump();
            wide.dispatcher.pump();
            listed = listSessions(hostBinary, home);
            alone = nullptr;
            if (listed) {
                for (const SessionRow& entry : *listed)
                    if (entry.workspaceName == "alone")
                        alone = &entry;
            }
            if ((alone && alone->columns == 90 && alone->rows == 28) || std::chrono::steady_clock::now() >= sizedBy)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(alone && alone->columns == 90 && alone->rows == 28, "alone: a pane that is its window takes the newest window's size");
        for (const auto& [sessionID, pane] : expected) {
            const SessionRow* entry = listed ? listedSession(*listed, sessionID) : nullptr;
            check(entry && entry->columns == pane.columns && entry->rows == pane.rows, "alone: the split's panes in \"work\" keep their grids");
        }
        wide.core.sendInput(std::string_view("exit\r"));
        check(pumpUntil(wide.dispatcher, 10000, [&] { return wide.observer.m_didExit; }), "alone: its shell exited");
        narrow.stop();
        wide.stop();

        // ---- A named session whose shell exits on its own leaves no record. ----
        Client brief(attachCommand, socketName, 80, 24);
        brief.core.setWorkspace("brief", false);
        check(brief.connect(0, 80, 24), "brief: created");
        brief.core.sendInput(std::string_view("true\r"));
        check(waitForFile(recordPath, 10000, [](const std::optional<std::string>& contents) { return contents && contents->find("brief") != std::string::npos; }), "brief: recorded while it runs");
        brief.core.sendInput(std::string_view("exit\r"));
        check(pumpUntil(brief.dispatcher, 10000, [&] { return brief.observer.m_didExit; }), "brief: its shell exited");
        check(waitForFile(recordPath, 10000, [](const std::optional<std::string>& contents) { return contents && contents->find("brief") == std::string::npos && contents->find("work") != std::string::npos; }), "brief: an ended session's record is gone, and the live one's stays");
        brief.stop();

        // ---- A shell killed by a signal is lost, not ended: as a machine
        // shutting down kills it, so its workspace stays restorable. ----
        Client lost(attachCommand, socketName, 80, 24);
        lost.core.setWorkspace("lost", false);
        check(lost.connect(0, 80, 24), "lost: created");
        lost.core.sendInput(std::string_view("true\r"));
        check(waitForFile(recordPath, 10000, [](const std::optional<std::string>& contents) { return contents && contents->find("lost") != std::string::npos; }), "lost: recorded while it runs");
        lost.core.sendInput(std::string_view("kill -9 $$\r"));
        check(pumpUntil(lost.dispatcher, 10000, [&] { return lost.observer.m_didExit; }), "lost: its shell was killed");
        const bool isStillRestorable = pumpUntil(lost.dispatcher, 3000, [&] {
            const auto listed = listSessions(hostBinary, home);
            return listed && std::ranges::any_of(*listed, [](const SessionRow& entry) { return entry.workspaceName == "lost" && entry.isRestorable(); });
        });
        check(isStillRestorable, "lost: a workspace whose last shell was killed is listed as restorable");
        lost.stop();

        crash(second);
        owner.stop();
    }

    // ---- The third daemon lists "work" and "lost" and not "brief";
    // terminating them forgets them. ----
    Daemon third = startDaemon(hostBinary, socketPath, logPath);
    {
        std::uint64_t listedDaemonID = 0;
        const auto listed = listSessions(hostBinary, home, &listedDaemonID);
        const auto isListed = [&](std::string_view name) {
            return listed && std::ranges::any_of(*listed, [&](const SessionRow& entry) { return entry.workspaceName == name && entry.isRestorable(); });
        };
        check(listed && listed->size() == 2 && countRestorable(*listed) == 2 && isListed("work") && isListed("lost"), "ended: \"work\" and \"lost\" are restorable after the next restart, and \"brief\" is not");
        if (listed && !listed->empty()) {
            for (const SessionRow& entry : *listed)
                terminateSession(hostBinary, home, entry.id, listedDaemonID);
            const auto after = listSessions(hostBinary, home);
            check(after && after->empty(), "terminate: a terminated restorable entry is gone from the listing");
            check(waitForFile(recordPath, 5000, [](const std::optional<std::string>& contents) { return !contents; }), "terminate: with nothing left to remember the record file is removed");
        }
    }
    crash(third);

    // ---- A host replaced by another binary leaves its record under another
    // socket's name; the next host takes it in once that host is not running. ----
    const std::string siblingRecordPath = home + "/host-0123456789abcdef.sock.layouts";
    {
        std::ofstream sibling(siblingRecordPath, std::ios::binary | std::ios::trunc);
        sibling << recordedBytes;
    }
    Daemon replacement = startDaemon(hostBinary, socketPath, logPath);
    {
        const auto listed = listSessions(hostBinary, home);
        check(listed && listed->size() == 1 && countRestorable(*listed) == 1 && listed->front().workspaceName == "work", "replaced: the record a stopped host left under its own socket is restorable here");
        check(!fileExists(siblingRecordPath), "replaced: the stopped host's record file is taken in and removed");
        check(readFile(recordPath).find("work") != std::string::npos, "replaced: the record now lives under this host's socket");
    }
    crash(replacement);

    // ---- A record that does not decode is no record. ----
    {
        std::ofstream garbage(recordPath, std::ios::binary | std::ios::trunc);
        garbage << "UBQL\x01\xff\xff this is not a layout record";
    }
    ::chmod(recordPath.c_str(), 0600);
    const std::size_t logBefore = readFile(logPath).size();
    Daemon fourth = startDaemon(hostBinary, socketPath, logPath);
    {
        const auto listed = listSessions(hostBinary, home);
        check(listed && listed->empty(), "malformed: a record that does not decode lists nothing");
        const std::string log = readFile(logPath).substr(logBefore);
        check(log.find(recordPath) != std::string::npos, "malformed: the daemon logged the record it could not read");
        // It is set aside whole, where the next write cannot replace it.
        check(readFile(recordPath + ".unread") == "UBQL\x01\xff\xff this is not a layout record", "malformed: the file the daemon could not read keeps its bytes under another name");
        check(log.find(recordPath + ".unread") != std::string::npos, "malformed: the log says where the file is kept");

        Client owner(attachCommand, socketName, 80, 24);
        owner.core.setWorkspace("work", false);
        check(owner.connect(0, 80, 24), "malformed: attaching by name still answers");
        check(pumpUntil(owner.dispatcher, 5000, [&] { return owner.core.workspace() != nullptr; }), "malformed: the client was told a workspace");
        const auto* mirror = owner.core.workspace();
        check(mirror && mirror->surfaceCount() == 1 && mirror->tabCount(mirror->surfaceIDAt(0)) == 1, "malformed: the workspace under the name is a fresh one");
        check(!owner.core.isWorkspaceRestoredAfterHostRestart(), "malformed: a fresh workspace does not say it was restored");
        owner.stop();
    }
    crash(fourth);

    // ---- A record written as a version this build does not know is refused
    // by name, with both versions, and is not replaced by this host's own. ----
    const std::string newerBytes = std::string("UBQN\xff\x7f", 6) + "whatever a later build lays out here";
    {
        std::ofstream newer(recordPath, std::ios::binary | std::ios::trunc);
        newer << newerBytes;
    }
    ::chmod(recordPath.c_str(), 0600);
    const std::size_t logBeforeNewer = readFile(logPath).size();
    Daemon fifth = startDaemon(hostBinary, socketPath, logPath);
    {
        const auto listed = listSessions(hostBinary, home);
        check(listed && listed->empty(), "newer: a record of a version this build does not know lists nothing");
        const std::string log = readFile(logPath).substr(logBeforeNewer);
        check(log.find(recordPath) != std::string::npos && log.find("version 32767") != std::string::npos
                && log.find("up to version " + std::to_string(Ubiquitty::Session::hostLayoutFileVersion)) != std::string::npos,
            "newer: the daemon logged the file, the version it was written as and the newest it reads");
        check(readFile(recordPath + ".unread") == newerBytes, "newer: the file keeps its bytes under another name");
        check(readFile(recordPath + ".unread.1") == "UBQL\x01\xff\xff this is not a layout record", "newer: the file set aside before it is kept too, a place older");

        Client owner(attachCommand, socketName, 80, 24);
        owner.core.setWorkspace("work", false);
        check(owner.connect(0, 80, 24), "newer: attaching by name still answers");
        check(waitForFile(recordPath, 5000, [](const std::optional<std::string>& contents) { return contents && contents->find("work") != std::string::npos; }), "newer: this host records its own workspace under the name");
        check(readFile(recordPath + ".unread") == newerBytes, "newer: and what the later build wrote is still there");
        owner.stop();
    }
    crash(fifth);

    // ---- A record in a form written before the file carried a version is a
    // file this host cannot read, and goes the way of any other: named in the
    // log, kept whole under another name, and not read as an empty one. ----
    const std::string olderBytes = std::string("UBQM\x01\x04\x00work\x00\x00\x00\x01", 15) + "the rest of what an earlier build wrote";
    {
        std::ofstream older(recordPath, std::ios::binary | std::ios::trunc);
        older << olderBytes;
    }
    ::chmod(recordPath.c_str(), 0600);
    // Two more were set aside before, so four are kept already: the oldest
    // makes room.
    for (const char* place : { ".unread.2", ".unread.3" }) {
        std::ofstream earlier(recordPath + place, std::ios::binary | std::ios::trunc);
        earlier << "set aside as " << place;
    }
    const std::size_t logBeforeOlder = readFile(logPath).size();
    Daemon sixth = startDaemon(hostBinary, socketPath, logPath);
    {
        const auto listed = listSessions(hostBinary, home);
        check(listed && listed->empty(), "older: a record in an unversioned form lists nothing");
        const std::string log = readFile(logPath).substr(logBeforeOlder);
        check(log.find(recordPath + " is not read because") != std::string::npos && log.find(recordPath + ".unread") != std::string::npos, "older: the daemon logged the file it does not read and where it is kept");
        check(readFile(recordPath + ".unread") == olderBytes, "older: the file keeps its bytes under another name");
        check(readFile(recordPath + ".unread.1") == newerBytes && readFile(recordPath + ".unread.2") == "UBQL\x01\xff\xff this is not a layout record" && readFile(recordPath + ".unread.3") == "set aside as .unread.2",
            "older: each file set aside before moved a place older, whole");
        check(!fileExists(recordPath + ".unread.4"), "older: no more than four are kept");
        check(log.find(recordPath + ".unread.3 is removed") != std::string::npos, "older: the log names the oldest, which made room");
        check(!fileExists(recordPath), "older: nothing stands in the file's place until this host records a workspace of its own");
    }
    crash(sixth);

    if (failureCount != 0) {
        std::fprintf(stderr, "named_layout_restore: the daemons' log and the record are left in %s\n", root.c_str());
        return 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::printf("named_layout_restore: a named workspace survived a crash with its topology, directories and grids; an ended one left no record; a malformed record read as none\n");
    return 0;
}
