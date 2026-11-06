// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/restore_renumbered_workspace.cpp — a workspace restored on the
// host's current daemon under an id other than the one its window saved, and
// one restored from a daemon that stopped after the current one started.
//
// Every fresh daemon numbers its first workspace 1. The host's current daemon
// holds a workspace 1 of its own, and a daemon on a sibling socket holds
// another; a window saves its reference to the sibling's, and the sibling is
// killed. The current daemon, started again, takes in its own record and the
// sibling's, and lists the sibling's workspace under another id because 1 is
// taken. The window reopened from its reference is answered with that
// workspace under the new id: the reference matches the record by the
// instance and the id it held the workspace under, not by the id it is
// listed by now. A second client holding the same reference, once the
// record is spent, joins the running restoration under the same answer.
//
// Then a daemon started after the current one numbers its first workspace 1
// as well, and is killed with a window open on it. The current daemon has
// never read its record; asked for that workspace by the open window, it
// takes the stopped daemon's record in, restores the workspace under an id of
// its own, and the window's pane is the restored workspace's from then on; a
// second client with that reference joins the same restored session.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/WorkspaceConnection.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/DetachedDaemons.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Ubiquitty;

namespace {

int failureCount = 0;

void check(bool ok, const std::string& what) // bool-ok: an assertion's outcome, as every Host smoke spells it
{
    if (!ok) {
        std::fprintf(stderr, "restore_renumbered_workspace: FAIL: %s\n", what.c_str());
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

/// The factory a frontend's session runs: the local bridge bootstrap, pinned
/// to a daemon socket that Core may withdraw.
class PinnedBootstrapFactory final : public Session::ChannelFactory {
public:
    explicit PinnedBootstrapFactory(const std::string& hostBinary)
        : m_command(Session::localBridgeCommand(Transport::shellSingleQuote(hostBinary)))
    {
    }

    void createChannel(Session::TransportMode, Done done) override
    {
        done(std::make_unique<Transport::SubprocessChannel>(Session::substituteReattachSocket(m_command, m_pin), Transport::Origin { Transport::OriginKind::Daemon, "" }), false);
    }
    bool pinDaemonSocket(std::string_view socketName) override
    {
        if (!m_pin.empty() && socketName != m_pin)
            return false;
        m_pin = socketName;
        return true;
    }
    void clearHandoverReach() override { m_pin.clear(); }

    [[nodiscard]] const std::string& pin() const { return m_pin; }

private:
    std::string m_command;
    std::string m_pin;
};

class RecordingObserver final : public Session::RemoteSessionObserver {
public:
    void onScreenChanged() override { ++m_screens; }
    void onExit() override { ++m_exits; }
    int m_screens = 0;
    int m_exits = 0;
};

std::unique_ptr<Ubiquitty::Terminal> makeTerm()
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, 24, 80);
    return term;
}

/// A window on one workspace: a workspace carrier through the pinned
/// bootstrap, and a pane core on it, as TerminalSession builds one.
struct Window {
    std::unique_ptr<Ubiquitty::Terminal> term = makeTerm();
    TestDispatcher* dispatcher = nullptr;
    PinnedBootstrapFactory* factory = nullptr;
    std::shared_ptr<Session::WorkspaceConnection> workspace;
    RecordingObserver observer;
    std::unique_ptr<Session::RemoteSessionCore> pane;

    Window(const std::string& hostBinary, std::string_view pinnedSocket)
    {
        auto ownedDispatcher = std::make_unique<TestDispatcher>();
        dispatcher = ownedDispatcher.get();
        auto ownedFactory = std::make_unique<PinnedBootstrapFactory>(hostBinary);
        factory = ownedFactory.get();
        check(factory->pinDaemonSocket(pinnedSocket), "the window's factory takes its pin");
        workspace = std::make_shared<Session::WorkspaceConnection>(std::move(ownedDispatcher), std::move(ownedFactory));
    }

    /// Opens the workspace by name, creating it on the pinned daemon.
    void create(const std::string& workspaceName)
    {
        workspace->connection().setWorkspace(workspaceName, false);
        workspace->connection().setReattachPolicy(Session::ReattachPolicy::AttachOrCreate);
        pane = std::make_unique<Session::RemoteSessionCore>(term.get(), *workspace, &observer);
        pane->start(0, 80, 24);
    }

    /// Reopens a saved reference, attach-only.
    void reopen(std::uint64_t daemonID, std::uint32_t workspaceID, std::uint32_t sessionID)
    {
        workspace->connection().setAttachWorkspaceID(workspaceID);
        workspace->connection().setExpectedDaemonInstanceID(daemonID);
        workspace->connection().setReattachPolicy(Session::ReattachPolicy::AttachOnly);
        pane = std::make_unique<Session::RemoteSessionCore>(term.get(), *workspace, &observer);
        pane->start(sessionID, 80, 24);
    }

    [[nodiscard]] Session::RemoteSessionCore& connection() { return workspace->connection(); }

    [[nodiscard]] bool isShowing() { return pane && pane->acknowledged() && observer.m_screens > 0; }

    void stop()
    {
        if (pane)
            pane->stop();
        workspace->connection().stop();
        dispatcher->pump();
    }
};

std::string readFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool waitFor(int timeoutMilliseconds, const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

/// The PID file a daemon on a socket writes: the socket's name with `.pid`
/// in place of `.sock`.
std::string processIDPath(const std::string& home, const std::string& socketName)
{
    return home + "/" + socketName.substr(0, socketName.size() - std::string_view(".sock").size()) + ".pid";
}

/// Runs the host binary, returning its exit status.
int runHost(const std::string& hostBinary, const std::string& arguments)
{
    const std::string line = Transport::shellSingleQuote(hostBinary) + " " + arguments + " 2>/dev/null";
    FILE* pipe = ::popen(line.c_str(), "r");
    if (!pipe)
        return -1;
    // Read to its end, so the host is not left writing to a closed pipe.
    std::string output;
    std::array<char, 512> buffer {};
    while (const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), pipe))
        output.append(buffer.data(), count);
    const int status = ::pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// Starts a daemon of the host binary on a socket of its own name, its log
/// in `logPath`.
pid_t startDaemon(const std::string& hostBinary, const std::string& socketName, const std::string& logPath)
{
    const pid_t child = ::fork();
    if (child == 0) {
        const int log = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0)
            ::dup2(log, STDERR_FILENO);
        ::execl(hostBinary.c_str(), hostBinary.c_str(), "--daemon", "--socket", socketName.c_str(), "--quic-port", "-1", "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return child;
}

/// Kills a daemon outright, as a crash ends one.
bool killDaemon(pid_t daemon)
{
    ::kill(daemon, SIGKILL);
    int status = 0;
    ::waitpid(daemon, &status, 0);
    return WIFSIGNALED(status);
}

/// Stops the host's current daemon as a person's stop does, by the PID it
/// wrote, and waits for it to remove its socket and PID file.
bool stopCurrentDaemon(const std::string& home, const std::string& socketName)
{
    const pid_t current = static_cast<pid_t>(std::atoi(readFile(processIDPath(home, socketName)).c_str()));
    if (current <= 0)
        return false;
    ::kill(current, SIGTERM);
    return waitFor(10000, [&] { return !exists(home + "/" + socketName) && !exists(processIDPath(home, socketName)); });
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBinary = argv[1];
    std::string rootTemplate = "/tmp/ubq-renumbered.XXXXXX";
    if (!::mkdtemp(rootTemplate.data())) {
        std::fprintf(stderr, "restore_renumbered_workspace: no scratch directory\n");
        return 1;
    }
    const std::string root = rootTemplate;
    const std::string home = root + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
    ::setenv("SHELL", "/bin/sh", 1);
    ::setenv("ENV", "/dev/null", 1);
    const std::string currentSocket = Host::socketLeafForBinary(hostBinary.c_str());
    const std::string currentPath = home + "/" + currentSocket;
    const std::string siblingSocket = "host-aaaaaaaaaaaaaaaa.sock";
    const std::string siblingPath = home + "/" + siblingSocket;
    const std::string lateSocket = "host-bbbbbbbbbbbbbbbb.sock";
    const std::string latePath = home + "/" + lateSocket;
    check(currentSocket != siblingSocket && currentSocket != lateSocket, "premise: the siblings' sockets are not the binary's own");

    // ---- The host's current daemon numbers its first workspace 1. ----
    check(runHost(hostBinary, "--ensure-daemon --quic-port -1") == 0, "the host's current daemon started");
    std::uint32_t currentWorkspace = 0;
    {
        Window own(hostBinary, currentSocket);
        own.create("current-first");
        check(pumpUntil(*own.dispatcher, 15000, [&] { return own.isShowing(); }), "a workspace opened on the current daemon");
        currentWorkspace = own.pane->workspaceID();
        check(waitFor(10000, [&] { return readFile(currentPath + ".layouts").find("current-first") != std::string::npos; }), "the current daemon recorded its workspace");
        own.stop();
    }

    // ---- A daemon on a sibling socket numbers its first workspace the same, and is killed. ----
    // It starts while the current daemon runs, so it takes in none of the
    // current daemon's records and its first workspace is its first id.
    const pid_t sibling = startDaemon(hostBinary, siblingSocket, root + "/sibling.log");
    check(sibling > 0 && HostTest::waitForDaemonReady(siblingPath), "a daemon listens on a sibling socket");
    std::uint64_t savedDaemon = 0;
    std::uint32_t savedWorkspace = 0;
    std::uint32_t savedSession = 0;
    {
        Window owner(hostBinary, siblingSocket);
        owner.create("sibling-first");
        check(pumpUntil(*owner.dispatcher, 15000, [&] { return owner.isShowing(); }), "a workspace opened on the sibling daemon");
        savedDaemon = owner.connection().daemonInstanceID();
        savedWorkspace = owner.pane->workspaceID();
        savedSession = owner.pane->sessionID();
        check(savedDaemon && savedWorkspace && savedSession, "the window holds a whole reference to save");
        check(waitFor(10000, [&] { return readFile(siblingPath + ".layouts").find("sibling-first") != std::string::npos; }), "the sibling daemon recorded its workspace");
        owner.stop();
    }
    check(currentWorkspace && savedWorkspace == currentWorkspace, "premise: both daemons gave their first workspace the same id");
    check(killDaemon(sibling), "premise: the sibling daemon was killed");

    // ---- The current daemon starts again and takes both records in, the sibling's under another id. ----
    check(stopCurrentDaemon(home, currentSocket), "the current daemon stopped when told to");
    check(readFile(currentPath + ".layouts").find("current-first") != std::string::npos, "and kept the record of its workspace");
    check(runHost(hostBinary, "--ensure-daemon --quic-port -1") == 0, "the host's current daemon started again");
    check(waitFor(10000, [&] { return !exists(siblingPath + ".layouts"); }), "the current daemon took in the sibling daemon's record");

    // ---- The reopened window gets the sibling's workspace back under the current daemon's id for it. ----
    {
        Window reopened(hostBinary, siblingSocket);
        reopened.reopen(savedDaemon, savedWorkspace, savedSession);
        check(pumpUntil(*reopened.dispatcher, 20000, [&] { return reopened.isShowing(); }),
            "the reopened window reached its workspace on the current daemon, though the id it saved is another workspace's there");
        check(reopened.connection().workspaceName() == "sibling-first", "the workspace restored is the one the window saved");
        check(reopened.pane->workspaceID() != savedWorkspace, "under the id the current daemon gave it");
        check(reopened.connection().daemonInstanceID() != savedDaemon, "answered by another daemon instance");
        check(reopened.factory->pin() == currentSocket, "the window is pinned to the current daemon now");

        // Another app instance still holds the stopped instance's reference.
        // The consumed record must lead to the same live restoration.
        Window second(hostBinary, siblingSocket);
        second.connection().setClientInstance({ .low = 2, .high = 0x726573746f7265 });
        second.reopen(savedDaemon, savedWorkspace, savedSession);
        check(pumpUntil(*second.dispatcher, 20000, [&] {
            reopened.dispatcher->pump();
            return second.isShowing();
        }),
            "a second client holding the stopped reference reaches the workspace after its record was consumed");
        check(second.connection().daemonInstanceID() == reopened.connection().daemonInstanceID(), "both clients reach the same current daemon");
        check(second.pane->workspaceID() == reopened.pane->workspaceID(), "the second client receives the same restored workspace id");
        check(second.pane->sessionID() == reopened.pane->sessionID(), "the second client attaches to the restored session instead of starting another");
        check(second.factory->pin() == currentSocket, "the second stopped reference also resolves to the current socket");
        check(second.observer.m_exits == 0, "the second client's pane was not ended while accepting the restored answer");
        second.stop();
        reopened.stop();
    }

    // ---- A daemon started after the current one numbers its workspace the same, and dies under an open window. ----
    const pid_t late = startDaemon(hostBinary, lateSocket, root + "/late.log");
    check(late > 0 && HostTest::waitForDaemonReady(latePath), "a later daemon listens on another sibling socket");
    Window live(hostBinary, lateSocket);
    live.create("late-first");
    check(pumpUntil(*live.dispatcher, 15000, [&] { return live.isShowing(); }), "a window opened a workspace on the later daemon");
    const std::uint64_t lateDaemon = live.connection().daemonInstanceID();
    const std::uint32_t lateWorkspace = live.pane->workspaceID();
    const std::uint32_t lateSession = live.pane->sessionID();
    check(lateWorkspace == currentWorkspace, "premise: the later daemon gave its first workspace that id as well");
    check(waitFor(10000, [&] { return readFile(latePath + ".layouts").find("late-first") != std::string::npos; }), "the later daemon recorded the open window's workspace");
    check(killDaemon(late), "premise: the later daemon was killed");

    // ---- The current daemon, running since before, takes that record in when the window asks. ----
    const int liveScreens = live.observer.m_screens;
    check(pumpUntil(*live.dispatcher, 20000, [&] {
        return live.pane->acknowledged() && live.pane->state() == Session::State::Connected && live.connection().daemonInstanceID() != lateDaemon
            && live.observer.m_screens > liveScreens;
    }),
        "the open window's pane is attached to its workspace restored on the current daemon, which read the record only when asked");
    check(live.observer.m_exits == 0, "the open window's pane never ended");
    check(live.connection().workspaceName() == "late-first", "the workspace restored is the window's");
    check(live.pane->workspaceID() != lateWorkspace, "under the id the current daemon gave it");
    check(!exists(latePath + ".layouts"), "the current daemon took the later daemon's record in and removed its file");
    Window lateSecond(hostBinary, lateSocket);
    lateSecond.connection().setClientInstance({ .low = 3, .high = 0x726573746f7265 });
    lateSecond.reopen(lateDaemon, lateWorkspace, lateSession);
    check(pumpUntil(*lateSecond.dispatcher, 20000, [&] {
        live.dispatcher->pump();
        return lateSecond.isShowing();
    }),
        "a second client joins the restoration after the on-demand sibling record was consumed");
    check(lateSecond.pane->workspaceID() == live.pane->workspaceID(), "the on-demand restoration also retains its old workspace reference for another client");
    check(lateSecond.pane->sessionID() == live.pane->sessionID(), "the later client joins the same running restored session");
    check(lateSecond.connection().daemonInstanceID() == live.connection().daemonInstanceID(), "both later clients reach the same current daemon");
    lateSecond.stop();
    live.stop();

    check(stopCurrentDaemon(home, currentSocket), "the current daemon stopped when told to, at the end");
    // The windows dial through the local bridge, whose ensure step starts the
    // host's current daemon detached wherever none answers; whichever start
    // it was, and however the stop above went, none is left running.
    check(HostTest::endDetachedDaemons(home), "no daemon started detached is left running");

    if (failureCount != 0)
        return 1;
    std::printf("restore_renumbered_workspace: a window whose saved workspace id is another workspace's on the current daemon got its workspace back under the id that daemon gave it; a daemon that stopped after the current one started had its record taken in when a window asked, and the window's pane stayed open on the restored workspace\n");
    return 0;
}
