// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/vanished_instance_reresolve.cpp — a window pinned to a daemon
// that stopped without handing over finds its workspace on the host's current
// daemon, through the real pinned bootstrap.
//
// The first daemon runs on a socket of its own name, as a daemon of another
// build runs beside the host binary's current one. A client creates a named
// workspace there through the bootstrap pinned to that socket, and keeps what
// a window saves: the daemon instance, the workspace id and the pane's
// session. The daemon is told to stop, and stops: its record stays, and its
// socket and PID file go. The host's current daemon starts on the binary's
// own socket and takes in the stopped daemon's records.
//
// A window left open on a second daemon that is killed outright keeps its
// pane: told the daemon is gone, it asks the current daemon, which takes in
// the killed daemon's record too and restores the workspace under a session
// of another number, and the pane is the restored workspace's pane from then
// on, never ended.
//
// A reopened window dials the stopped daemon through the same pinned
// bootstrap. The host says that daemon is gone; the window drops the pin, asks
// the current daemon for the same reference, and is answered with the
// workspace restored from its record. A reference the current daemon holds
// no record of ends plainly, and nothing dials again. The host binary's own
// answers are checked as well: its probe and its pinned QUIC bootstrap say a
// socket nothing listens on is gone.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        std::fprintf(stderr, "vanished_instance_reresolve: FAIL: %s\n", what.c_str());
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
        ++m_creates;
        done(std::make_unique<Transport::SubprocessChannel>(Session::substituteReattachSocket(m_command, m_pin), Transport::Origin { Transport::OriginKind::Daemon, "" }), false);
    }
    bool pinDaemonSocket(std::string_view socketName) override
    {
        if (!m_pin.empty() && socketName != m_pin)
            return false;
        m_pin = socketName;
        return true;
    }
    void clearHandoverReach() override
    {
        m_pin.clear();
        ++m_clears;
    }

    [[nodiscard]] int creates() const { return m_creates; }
    [[nodiscard]] int clears() const { return m_clears; }
    [[nodiscard]] const std::string& pin() const { return m_pin; }

private:
    std::string m_command;
    std::string m_pin;
    int m_creates = 0;
    int m_clears = 0;
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

/// Runs the host binary, returning what it printed on standard output and
/// its exit status.
std::pair<std::string, int> runHost(const std::string& hostBinary, const std::string& arguments)
{
    const std::string line = Transport::shellSingleQuote(hostBinary) + " " + arguments + " 2>/dev/null";
    FILE* pipe = ::popen(line.c_str(), "r");
    if (!pipe)
        return { {}, -1 };
    std::string output;
    std::array<char, 512> buffer {};
    while (const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), pipe))
        output.append(buffer.data(), count);
    const int status = ::pclose(pipe);
    return { output, WIFEXITED(status) ? WEXITSTATUS(status) : -1 };
}

/// Leaves a socket file nothing listens on, as a daemon that was killed does.
bool plantStaleSocket(const std::string& path)
{
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0)
        return false;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(descriptor);
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const bool isBound = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(descriptor);
    return isBound;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBinary = argv[1];
    std::string rootTemplate = "/tmp/ubq-vanished.XXXXXX";
    if (!::mkdtemp(rootTemplate.data())) {
        std::fprintf(stderr, "vanished_instance_reresolve: no scratch directory\n");
        return 1;
    }
    const std::string root = rootTemplate;
    const std::string home = root + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
    ::setenv("SHELL", "/bin/sh", 1);
    ::setenv("ENV", "/dev/null", 1);
    const std::string stoppedSocket = "host-aaaaaaaaaaaaaaaa.sock";
    const std::string stoppedPath = home + "/" + stoppedSocket;
    const std::string currentSocket = Host::socketLeafForBinary(hostBinary.c_str());
    check(currentSocket != stoppedSocket, "premise: the stopped daemon's socket is not the binary's own");

    // ---- The host binary's own answers for a socket nothing listens on. ----
    const std::string staleSocket = "host-bbbbbbbbbbbbbbbb.sock";
    check(plantStaleSocket(home + "/" + staleSocket), "premise: a stale socket file was left");
    const auto staleProbe = runHost(hostBinary, "--probe-daemon --socket " + staleSocket);
    check(staleProbe.second == Session::daemonProbeGoneStatus, "the probe says a stale socket's daemon is gone");
    const auto missingProbe = runHost(hostBinary, "--probe-daemon --socket host-cccccccccccccccc.sock");
    check(missingProbe.second == Session::daemonProbeGoneStatus, "the probe says a missing socket's daemon is gone");
    const auto staleQUIC = runHost(hostBinary, "--quic-bootstrap --socket " + staleSocket);
    check(staleQUIC.second != 0 && staleQUIC.first == std::string(Session::instanceGoneSentinel) + " " + staleSocket + "\n",
        "a QUIC bootstrap pinned to a stale socket prints that its daemon is gone, and nothing else");
    ::unlink((home + "/" + staleSocket).c_str());

    // ---- The first daemon, on a socket of its own, holds a named workspace. ----
    const pid_t first = ::fork();
    if (first == 0) {
        const int log = ::open((root + "/first.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0)
            ::dup2(log, STDERR_FILENO);
        ::execl(hostBinary.c_str(), hostBinary.c_str(), "--daemon", "--socket", stoppedSocket.c_str(), "--quic-port", "-1", "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    check(first > 0 && HostTest::waitForDaemonReady(stoppedPath), "the first daemon listens on its own socket");
    check(exists(processIDPath(home, stoppedSocket)), "premise: the first daemon wrote its PID file");
    check(runHost(hostBinary, "--probe-daemon --socket " + stoppedSocket).second == 0, "the probe says a listening daemon is there");

    std::uint64_t savedDaemon = 0;
    std::uint32_t savedWorkspace = 0;
    std::uint32_t savedSession = 0;
    {
        Window owner(hostBinary, stoppedSocket);
        owner.create("work");
        check(pumpUntil(*owner.dispatcher, 15000, [&] { return owner.pane->acknowledged() && owner.observer.m_screens > 0; }),
            "the pinned bootstrap reached the live daemon and opened the workspace");
        check(owner.factory->clears() == 0, "a live pinned daemon is never reported gone");
        savedDaemon = owner.connection().daemonInstanceID();
        savedWorkspace = owner.pane->workspaceID();
        savedSession = owner.pane->sessionID();
        check(savedDaemon && savedWorkspace && savedSession, "the window holds a whole reference to save");
        check(waitFor(10000, [&] { return readFile(stoppedPath + ".layouts").find("work") != std::string::npos; }), "the first daemon recorded the workspace");
        owner.stop();
    }

    // ---- A window stays open on a daemon whose carrier is lost. ----
    // Killed outright, as a crash or a machine sleeping through it ends a
    // daemon: no Bye reaches the window, which redials the socket it was on.
    const std::string lostSocket = "host-dddddddddddddddd.sock";
    const std::string lostPath = home + "/" + lostSocket;
    const pid_t lost = ::fork();
    if (lost == 0) {
        const int log = ::open((root + "/lost.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log >= 0)
            ::dup2(log, STDERR_FILENO);
        ::execl(hostBinary.c_str(), hostBinary.c_str(), "--daemon", "--socket", lostSocket.c_str(), "--quic-port", "-1", "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    check(lost > 0 && HostTest::waitForDaemonReady(lostPath), "a second daemon listens on a socket of its own");
    // A workspace opened first and ended once the window's is open, so the
    // one the window holds is numbered past the first numbers a fresh host
    // gives, and the daemon keeps the window's session when the first ends.
    Window ended(hostBinary, lostSocket);
    ended.create("ended");
    check(pumpUntil(*ended.dispatcher, 15000, [&] { return ended.pane->acknowledged() && ended.observer.m_screens > 0; }), "a first workspace opened on the second daemon");
    Window live(hostBinary, lostSocket);
    live.create("live");
    check(pumpUntil(*live.dispatcher, 15000, [&] { return live.pane->acknowledged() && live.observer.m_screens > 0; }),
        "a window opened a workspace on the second daemon");
    ended.pane->sendInput("exit\r");
    check(pumpUntil(*ended.dispatcher, 15000, [&] { return ended.observer.m_exits > 0; }), "the first workspace ended with its shell");
    ended.stop();
    const std::uint64_t liveDaemon = live.connection().daemonInstanceID();
    const std::uint32_t liveWorkspace = live.pane->workspaceID();
    const std::uint32_t liveSession = live.pane->sessionID();
    check(waitFor(10000, [&] { return readFile(lostPath + ".layouts").find("live") != std::string::npos; }), "the second daemon recorded the open window's workspace");
    ::kill(lost, SIGKILL);
    int lostStatus = 0;
    ::waitpid(lost, &lostStatus, 0);
    check(WIFSIGNALED(lostStatus), "premise: the second daemon was killed");

    // ---- It is told to stop, and leaves its record but no socket or PID file. ----
    ::kill(first, SIGTERM);
    int status = 0;
    ::waitpid(first, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a stop asked for by SIGTERM is a clean exit");
    check(!exists(stoppedPath), "a daemon that stopped with no successor removed its socket");
    check(!exists(processIDPath(home, stoppedSocket)), "and its PID file");
    check(readFile(stoppedPath + ".layouts").find("work") != std::string::npos, "and kept the record of the workspace it held");

    // ---- The host's current daemon starts on the binary's socket and takes the record in. ----
    check(runHost(hostBinary, "--ensure-daemon --quic-port -1").second == 0, "the host's current daemon started");
    const std::string currentPath = home + "/" + currentSocket;
    check(waitFor(10000, [&] { return !exists(stoppedPath + ".layouts"); }), "the current daemon took in the stopped daemon's record");
    check(waitFor(10000, [&] { return !exists(lostPath + ".layouts"); }), "and the killed daemon's");

    // ---- The open window's pane is the restored workspace's pane now. ----
    const int liveScreens = live.observer.m_screens;
    check(pumpUntil(*live.dispatcher, 20000, [&] {
        return live.pane->acknowledged() && live.pane->state() == Session::State::Connected && live.connection().daemonInstanceID() != liveDaemon
            && live.observer.m_screens > liveScreens;
    }),
        "the open window's pane is attached to its workspace restored on the host's current daemon, and shows it");
    check(live.observer.m_exits == 0, "the open window's pane never ended");
    check(live.pane->workspaceID() == liveWorkspace, "it holds the workspace id it had");
    check(live.pane->sessionID() != liveSession, "premise: the restored workspace's session has another number");
    check(live.factory->clears() == 1 && live.factory->pin() == currentSocket, "the open window was told its daemon is gone once, and is pinned to the current daemon now");
    live.stop();

    // ---- The reopened window is pinned to the stopped daemon. ----
    {
        Window reopened(hostBinary, stoppedSocket);
        reopened.reopen(savedDaemon, savedWorkspace, savedSession);
        check(pumpUntil(*reopened.dispatcher, 20000, [&] { return reopened.pane->acknowledged() && reopened.observer.m_screens > 0; }),
            "the reopened window reached its workspace on the host's current daemon");
        check(reopened.factory->clears() == 1, "the host reported the pinned daemon gone, once");
        check(reopened.factory->pin() == currentSocket, "the window is pinned to the current daemon now");
        check(reopened.factory->creates() == 2, "one dial to the stopped daemon, one to the current");
        check(reopened.pane->workspaceID() == savedWorkspace, "under the workspace id the window saved");
        check(reopened.connection().daemonInstanceID() != savedDaemon, "answered by another daemon instance");
        reopened.stop();
    }

    // ---- A reference the current daemon has no record of ends, and is not dialled again. ----
    {
        Window orphan(hostBinary, stoppedSocket);
        orphan.reopen(savedDaemon, savedWorkspace + 1000, savedSession);
        check(pumpUntil(*orphan.dispatcher, 20000, [&] { return orphan.connection().state() == Session::State::Failed; }),
            "a workspace the current daemon cannot restore fails the window");
        check(orphan.connection().lastRefusal().detail == "Ubiquitty restarted on the host, and the saved workspace is no longer available.",
            "and says so in plain words");
        const int creates = orphan.factory->creates();
        (void)pumpUntil(*orphan.dispatcher, 3000, [] { return false; });
        check(orphan.factory->creates() == creates && creates == 2, "and nothing dials again");
        orphan.stop();
    }

    // ---- The current daemon is stopped the same way. ----
    const std::string pidText = readFile(processIDPath(home, currentSocket));
    const pid_t current = static_cast<pid_t>(std::atoi(pidText.c_str()));
    if (current > 0) {
        ::kill(current, SIGTERM);
        check(waitFor(10000, [&] { return !exists(currentPath) && !exists(processIDPath(home, currentSocket)); }),
            "the current daemon removed its socket and PID file when told to stop");
    } else {
        check(false, "the current daemon wrote its PID file");
    }
    // The windows dial through the local bridge, whose ensure step starts the
    // host's current daemon detached wherever none answers; whichever start
    // it was, and however the stop above went, none is left running.
    check(HostTest::endDetachedDaemons(home), "no daemon started detached is left running");

    if (failureCount != 0)
        return 1;
    std::printf("vanished_instance_reresolve: a window open on a daemon that stopped kept its pane on the workspace the host's current daemon restored; a window pinned to a stopped daemon was told it is gone, dropped the pin and got its workspace back from the host's current daemon; a reference with no record ended plainly with no redial; the probe and the QUIC bootstrap name a stale socket gone, and a stop removes the socket\n");
    return 0;
}
