// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A foreground host and deterministic PTY peer for real SSH/QUIC acceptance.
// The runner owns process lifetimes; each PTY peer exits when its private
// control connection closes, including when a client fails or times out.

#include "Assertions.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/QUICChannel.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/SyncReconnectSoak.h"
#include "PAL/PAL.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"
#include "Platform/Host/HostSocketName.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

int ubiquittyHostMain(int, char**);

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Session = Ubiquitty::Session;
namespace Transport = Ubiquitty::Transport;

namespace Ubiquitty::Host {

/// Supplies only the listener's binding; the production reactor owns its I/O.
struct SyncTransportAcceptanceAccess {
    static bool startQUIC(Daemon& daemon, PAL::UnixFileDescriptor socket)
    {
        auto listener = std::make_unique<QUICListener>();
        if (!listener->startOnBoundSocket(std::move(socket)))
            return false;
        listener->setStreamDataCallback([&daemon](QUICConnection* connection, std::span<const std::uint8_t> bytes) { daemon.onQUICStreamData(connection, bytes); });
        listener->setConnectionClosedCallback([&daemon](QUICConnection* connection) { daemon.onQUICConnectionClosed(connection); });
        listener->setDatagramCallback([&daemon](QUICConnection* connection, std::span<const std::uint8_t> bytes) { daemon.onQUICDatagram(connection, bytes); });
        daemon.m_quic = std::move(listener);
        return true;
    }
};

} // namespace Ubiquitty::Host

namespace Ubiquitty::Session {

struct RemoteSessionDatagramIngressAccess {
    static const Protocol::Sync::ClientConnection* screen(const RemoteSessionCore& core) { return core.m_core->screen(); }
};

} // namespace Ubiquitty::Session

namespace {

constexpr std::string_view socketLeaf = "host-0000000000000001.sock";
constexpr int waitMilliseconds = 10'000;

bool hasExclusiveChildOwnership()
{
    struct sigaction disposition {};
    return ::sigaction(SIGCHLD, nullptr, &disposition) == 0
        && disposition.sa_handler == SIG_DFL && !(disposition.sa_flags & SA_NOCLDWAIT);
}

bool writeAll(int descriptor, std::string_view bytes)
{
    while (!bytes.empty()) {
        const auto count = ::write(descriptor, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

int runPeer()
{
    const char* controlPath = std::getenv("UBQ_TRANSPORT_CONTROL");
    if (!controlPath || std::strlen(controlPath) >= sizeof(sockaddr_un::sun_path))
        return 2;
    Ubiquitty::PAL::UnixFileDescriptor control(::socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, controlPath, std::strlen(controlPath) + 1);
    if (!control || ::connect(control.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
        return 2;

    struct sigaction disposition {};
    disposition.sa_handler = SIG_DFL;
    sigemptyset(&disposition.sa_mask);
    if (::sigaction(SIGHUP, &disposition, nullptr))
        return 2;
    termios mode {};
    if (::tcgetattr(STDIN_FILENO, &mode))
        return 2;
    ::cfmakeraw(&mode);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &mode))
        return 2;
    if (!writeAll(STDOUT_FILENO, "PEER_READY\r\n"))
        return 2;

    std::string pending;
    for (;;) {
        std::array<pollfd, 2> waiting { pollfd { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 },
            pollfd { .fd = control.get(), .events = POLLIN, .revents = 0 } };
        const auto ready = ::poll(waiting.data(), waiting.size(), 30'000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || waiting[1].revents || waiting[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return 0;
        if (!(waiting[0].revents & POLLIN))
            continue;
        std::array<char, 1024> bytes {};
        const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count <= 0)
            return 0;
        pending.append(bytes.data(), static_cast<std::size_t>(count));
        if (pending.size() > 4096)
            return 2;
        for (auto end = pending.find_first_of("\r\n"); end != std::string::npos; end = pending.find_first_of("\r\n")) {
            const std::string command = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (command.starts_with("TYPE ")) {
                if (!writeAll(STDOUT_FILENO, "TYPED_" + command.substr(5) + "\r\n"))
                    return 2;
            } else if (command.starts_with("HISTORY ")) {
                for (unsigned line = 0; line < 64; ++line) {
                    if (!writeAll(STDOUT_FILENO, "HISTORY_" + command.substr(8) + "_" + std::to_string(line) + "\r\n"))
                        return 2;
                }
                if (!writeAll(STDOUT_FILENO, "HISTORY_END\r\n"))
                    return 2;
            } else if (command.starts_with("FLOOD ") || command.starts_with("REPLACE ")) {
                const bool isReplacement = command.starts_with("REPLACE ");
                const auto seed = UbiquittyTest::soakNumber(std::string_view(command).substr(isReplacement ? 8 : 6));
                if (!seed || !writeAll(STDOUT_FILENO, UbiquittyTest::soakCommandStart(*seed)))
                    return 2;
                for (unsigned frame = 0; frame < (isReplacement ? 256u : UbiquittyTest::soakFrameCount); ++frame) {
                    if (!writeAll(STDOUT_FILENO, UbiquittyTest::soakFrame(*seed, frame)))
                        return 2;
                    ::usleep(isReplacement ? 10'000 : 2'000);
                }
                if (!writeAll(STDOUT_FILENO, "\033]133;D;0\a"))
                    return 2;
            } else if (command == "IMAGE") {
                if (!writeAll(STDOUT_FILENO, "\033_Gf=24,s=1,v=1,i=42,a=T,q=2;/wAA\033\\"))
                    return 2;
            } else if (command == "DELETE_IMAGE") {
                if (!writeAll(STDOUT_FILENO, "\033_Ga=d,d=I,i=42,q=2;\033\\"))
                    return 2;
            } else if (command == "SIZE") {
                winsize geometry {};
                if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &geometry)
                    || !writeAll(STDOUT_FILENO, "SIZE_" + std::to_string(geometry.ws_col) + "_" + std::to_string(geometry.ws_row) + "\r\n"))
                    return 2;
            } else if (command == "EXIT")
                return 0;
        }
    }
}

int runServer(const char* directory)
{
    const auto executable = Ubiquitty::Host::resolveSelfExecutablePath();
    if (executable.empty())
        return 2;
    Ubiquitty::PAL::UnixFileDescriptor socket(::socket(AF_INET, SOCK_DGRAM, 0));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!socket || ::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
        return 2;
    Ubiquitty::Host::Daemon daemon;
    daemon.setConfiguration(Ubiquitty::Preferences::values());
    const std::string socketPath = std::string(directory) + "/" + std::string(socketLeaf);
    const std::string processPath = std::string(directory) + "/host-0000000000000001.pid";
    if (!daemon.start(socketPath.c_str(), processPath.c_str(), executable.c_str(), 80, 12)
        || !Ubiquitty::Host::SyncTransportAcceptanceAccess::startQUIC(daemon, std::move(socket)))
        return 2;
    std::puts("SYNC_TRANSPORT_SERVER_READY");
    std::fflush(stdout);
    return daemon.run();
}

class Dispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000)), std::move(work));
    }

    bool until(const std::function<bool()>& condition, int milliseconds = waitMilliseconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        do {
            pump();
            if (condition())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        pump();
        return condition();
    }

    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::scoped_lock lock(m_mutex);
                const auto ready = std::ranges::find_if(m_work, [](const auto& pending) { return pending.first <= std::chrono::steady_clock::now(); });
                if (ready == m_work.end())
                    return;
                work = std::move(ready->second);
                m_work.erase(ready);
            }
            std::move(work)();
        }
    }

private:
    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work)
    {
        const std::scoped_lock lock(m_mutex);
        m_work.emplace_back(due, std::move(work));
    }

    std::mutex m_mutex;
    std::vector<std::pair<std::chrono::steady_clock::time_point, Session::PostedWork>> m_work;
};

enum class Carrier : std::uint8_t {
    SSH,
    QUIC
};

class Factory final : public Session::ChannelFactory {
public:
    Factory(Carrier carrier, std::string command, Sync::QUICBootstrapInfo bootstrap)
        : m_carrier(carrier)
        , m_command(std::move(command))
        , m_bootstrap(std::move(bootstrap))
    {
    }

    void createChannel(Session::TransportMode, Done done) override
    {
        ++m_creations;
        if (m_carrier == Carrier::SSH && !hasExclusiveChildOwnership())
            done(nullptr, false);
        else if (m_carrier == Carrier::SSH)
            done(std::make_unique<Transport::SubprocessChannel>(m_command, origin()), false);
        else if (m_creations == 1)
            done(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_bootstrap.port, std::move(m_bootstrap.token), m_bootstrap.certificateSHA256, origin()), true);
        else
            done(nullptr, true);
    }

    bool canResumeChannel() const override { return m_carrier == Carrier::QUIC; }
    bool pinDaemonSocket(std::string_view leaf) override { return leaf == socketLeaf; }
    void resumeChannel(Session::ResumeKey key, Done done) override
    {
        ++m_resumes;
        Transport::ExportedPreamble preamble { .exporterLabel = std::move(key.exporterLabel), .build = std::move(key.buildPreamble) };
        done(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_bootstrap.port, std::move(preamble), m_bootstrap.certificateSHA256, origin()), true);
    }

    unsigned creations() const { return m_creations; }
    unsigned resumes() const { return m_resumes; }

private:
    static Transport::Origin origin() { return { Transport::OriginKind::Daemon, "" }; }
    Carrier m_carrier;
    std::string m_command;
    Sync::QUICBootstrapInfo m_bootstrap;
    unsigned m_creations = 0;
    unsigned m_resumes = 0;
};

class Observer final : public Session::RemoteSessionObserver {
public:
    void onStateChanged(Session::State state, const std::string& detail) override
    {
        if (state != Session::State::Failed)
            return;
        if (m_isStopping && detail == "detached")
            return;
        if (m_exits == 1 && detail == "The session ended.") {
            ++m_normalEnds;
        } else {
            ++m_unexpectedFailures;
            std::fprintf(stderr, "sync-transport: %s\n", detail.c_str());
        }
    }
    void onExit() override { ++m_exits; }
    void willStop() { m_isStopping = true; }
    unsigned exits() const { return m_exits; }
    unsigned normalEnds() const { return m_normalEnds; }
    unsigned unexpectedFailures() const { return m_unexpectedFailures; }

private:
    unsigned m_exits = 0;
    unsigned m_normalEnds = 0;
    unsigned m_unexpectedFailures = 0;
    bool m_isStopping = false;
};

std::string screenText(Ubiquitty::Terminal& terminal, int offset = 0)
{
    std::string output;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const auto* cell = Ubiquitty::Screen::visibleCell(&terminal, offset, row, column);
            const auto character = cell ? cell->codePoint : 0;
            output.push_back(character >= 32 && character < 127 ? static_cast<char>(character) : ' ');
        }
        output.push_back('\n');
    }
    return output;
}

bool hasImage(const Ubiquitty::Terminal& terminal)
{
    for (const auto& image : terminal.images.stored) {
        if (!image.used || image.imageID != 42 || image.width != 1 || image.height != 1 || !image.pixels
            || image.pixelLength < 3 || image.pixels[0] != 255 || image.pixels[1] || image.pixels[2])
            continue;
        return std::ranges::any_of(terminal.images.placements, [](const auto& placement) { return placement.used && placement.imageID == 42; });
    }
    return false;
}

void exercise(Carrier carrier, const std::string& command, Sync::QUICBootstrapInfo bootstrap)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 12, 80);
    Dispatcher dispatcher;
    Factory factory(carrier, command, std::move(bootstrap));
    Observer observer;
    Session::RemoteSessionCore core(&terminal, &dispatcher, &factory, &observer);
    const auto connected = [&] { return core.state() == Session::State::Connected && core.acknowledged(); };
    const auto showing = [&](std::string_view marker) { return screenText(terminal).contains(marker); };
    core.start(0, 80, 12);
    REQUIRE_MSG(dispatcher.until([&] { return connected() && showing("PEER_READY"); }), "The real carrier did not attach and deliver its PTY's screen.");
    CHECK(core.isUsingQUIC() == (carrier == Carrier::QUIC));
    const auto sessionID = core.sessionID();
    REQUIRE(sessionID);
    const auto* firstConnection = Session::RemoteSessionDatagramIngressAccess::screen(core);
    REQUIRE(firstConnection && firstConnection->welcome());
    CHECK(firstConnection->welcome()->admission == Sync::AttachmentAdmission::Granted);
    CHECK_EQ(firstConnection->welcome()->selection.sessionID, sessionID);
    const auto bindings = firstConnection->boundPanes();
    REQUIRE(bindings.size() == 1);
    const auto paneID = bindings.front().second;
    CHECK(firstConnection->counters().checkpointsApplied > 0);

    core.sendInput("TYPE acceptance-before\n");
    REQUIRE_MSG(dispatcher.until([&] { return showing("TYPED_acceptance-before"); }), "Typed input was not executed by the PTY peer.");
    core.sendInput("HISTORY acceptance\n");
    REQUIRE(dispatcher.until([&] { return showing("HISTORY_END") && core.view().history && core.view().history->head() > 40; }));
    const Sync::LineRange wanted { .from = 1, .to = 8 };
    REQUIRE(core.wantHistory(Sync::HistoryPriority::Viewport, { &wanted, 1 }));
    REQUIRE_MSG(dispatcher.until([&] {
        const auto* history = core.view().history;
        return history && history->line(3) && terminal.scrollbackCount > 12;
    }),
        "Requested scrollback was not received and projected into the passive terminal.");
    CHECK_MSG(screenText(terminal, terminal.scrollbackCount).contains("HISTORY_acceptance_0"), "Scrolling to the oldest retained rows did not show offscreen output.");

    core.sendInput("IMAGE\n");
    REQUIRE_MSG(dispatcher.until([&] { return hasImage(terminal); }), "Kitty pixels and placement did not cross the real carrier.");
    core.setSize(97, 13);
    REQUIRE_MSG(dispatcher.until([&] { return terminal.rows == 13 && terminal.columns == 97; }), "The host did not publish the requested resize.");
    core.sendInput("SIZE\n");
    REQUIRE_MSG(dispatcher.until([&] { return showing("SIZE_97_13"); }), "The resize did not reach the real PTY.");

    core.suspend();
    REQUIRE(core.state() == Session::State::Reconnecting);
    core.resumeAfterSuspension(120);
    REQUIRE_MSG(dispatcher.until(connected), "The carrier did not reconnect after suspension.");
    CHECK_EQ(core.sessionID(), sessionID);
    const auto* resumed = Session::RemoteSessionDatagramIngressAccess::screen(core);
    REQUIRE(resumed && resumed->welcome());
    const auto& outcomes = resumed->welcome()->resumed;
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes.front().paneID == paneID);
    CHECK(outcomes.front().outcome != Sync::ResumeOutcome::Gone);
    CHECK(hasImage(terminal));
    CHECK_EQ(factory.creations(), carrier == Carrier::SSH ? 2u : 1u);
    CHECK_EQ(factory.resumes(), carrier == Carrier::QUIC ? 1u : 0u);
    core.sendInput("TYPE acceptance-after\n");
    REQUIRE_MSG(dispatcher.until([&] { return showing("TYPED_acceptance-after"); }), "Input after reconnect did not reach the same PTY.");
    core.sendInput("DELETE_IMAGE\n");
    REQUIRE_MSG(dispatcher.until([&] { return !hasImage(terminal); }), "Image deletion did not reach the resumed replica.");
    CHECK_EQ(observer.exits(), 0u);
    CHECK_EQ(observer.unexpectedFailures(), 0u);
    core.sendInput("EXIT\n");
    REQUIRE_MSG(dispatcher.until([&] { return observer.exits() == 1 && core.state() == Session::State::Failed; }), "The peer did not deliver a normal session-end notification.");
    CHECK_EQ(observer.normalEnds(), 1u);
    CHECK_EQ(observer.unexpectedFailures(), 0u);
    CHECK_EQ(factory.creations(), carrier == Carrier::SSH ? 2u : 1u);
    CHECK_EQ(factory.resumes(), carrier == Carrier::QUIC ? 1u : 0u);
    observer.willStop();
    core.stop();
    dispatcher.pump();
    CHECK_EQ(observer.exits(), 1u);
    CHECK_EQ(observer.unexpectedFailures(), 0u);
}

/// Reads one bounded bootstrap record without exposing its credential.
/// - Parameters:
///   - path: The fixture-owned credential file.
///   - bootstrap: Receives the decoded record.
/// - Returns: Whether the complete bounded record parsed.
bool loadAcceptanceBootstrap(const char* path, Sync::QUICBootstrapInfo& bootstrap)
{
    std::ifstream source(path);
    std::array<char, 4097> buffer {};
    source.read(buffer.data(), buffer.size());
    const std::string_view line(buffer.data(), static_cast<std::size_t>(source.gcount()));
    return !source.bad() && line.size() <= 4096 && Sync::parseQUICBootstrapLine(line, bootstrap);
}

} // namespace

#include "Host/DaemonReplacementSoak.h"

namespace {

/// A detached fixture daemon exits when its private supervising socket closes.
/// The runner never signals a PID learnt from a file or a process listing.
bool watchReplacementDaemon()
{
    const char* path = std::getenv("UBQ_REPLACEMENT_CONTROL");
    if (!path || std::strlen(path) >= sizeof(sockaddr_un::sun_path))
        return false;
    Ubiquitty::PAL::UnixFileDescriptor control(::socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, std::strlen(path) + 1);
    if (!control || ::fcntl(control.get(), F_SETFD, FD_CLOEXEC)
        || ::connect(control.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
        return false;
    const auto executable = Ubiquitty::Host::resolveSelfExecutablePath();
    const auto leaf = Ubiquitty::Host::socketLeafForBinary(executable.c_str());
    if (leaf.empty() || !writeAll(control.get(), std::to_string(::getpid()) + " " + leaf + "\n"))
        return false;
    std::thread([control = std::move(control)] {
        char command = 0;
        ssize_t count = 0;
        do {
            count = ::read(control.get(), &command, 1);
        } while (count < 0 && errno == EINTR);
        if (count == 1 && command == 'K')
            ::_exit(86);
        ::raise(SIGTERM);
    }).detach();
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // The client creates only PAL-owned carrier children: no PTY, process
    // tracker, or independent waiter can reap those identities. The server's
    // separate process reaps only the PTY children it creates itself.
    if (!hasExclusiveChildOwnership()) {
        std::fprintf(stderr, "sync-transport: child exits must remain waitable under default SIGCHLD\n");
        return 2;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--daemon") {
        if (!watchReplacementDaemon())
            return 2;
        return ubiquittyHostMain(argc, argv);
    }
    if (argc >= 2 && (std::string_view(argv[1]) == "--attach" || std::string_view(argv[1]) == "--quic-bootstrap" || std::string_view(argv[1]) == "--ensure-daemon"))
        return ubiquittyHostMain(argc, argv);
    // Every carrier here is a local one: a bridge that inherited an SSH
    // session's variables would mark its client as relayed from SSH.
    ::unsetenv("SSH_CONNECTION");
    ::unsetenv("SSH_CLIENT");
    Ubiquitty::PAL::Context context;
    context.monotonicClock = &Ubiquitty::PAL::monotonicTicks;
    Ubiquitty::PAL::installContext(context);
    if (argv[0][0] == '-')
        return runPeer();
    if (argc == 3 && std::string_view(argv[1]) == "--fixture-server")
        return runServer(argv[2]);
    if (argc == 6 && std::string_view(argv[1]) == "--replacement-soak") {
        const auto iterations = UbiquittyTest::soakNumber(argv[4]);
        const auto seed = UbiquittyTest::soakNumber(argv[5]);
        std::array<Sync::QUICBootstrapInfo, 2> bootstraps;
        if (!iterations || !*iterations || *iterations > 1000 || !seed
            || !loadAcceptanceBootstrap(argv[2], bootstraps[0]) || !loadAcceptanceBootstrap(argv[3], bootstraps[1]))
            return 2;
        runDaemonReplacementSoak(bootstraps, *iterations, *seed);
        return failures ? 1 : 0;
    }
    if ((argc != 5 && argc != 8 && argc != 10) || std::string_view(argv[1]) != "--client") {
        std::fprintf(stderr, "usage: sync_transport_acceptance --client ssh|quic command bootstrap-file [--soak iterations seed [--observer-bootstrap file]]\n");
        return 2;
    }
    const std::string_view mode(argv[2]);
    if (mode != "ssh" && mode != "quic")
        return 2;
    std::optional<std::uint32_t> iterations;
    std::optional<std::uint32_t> seed;
    if (argc >= 8) {
        iterations = UbiquittyTest::soakNumber(argv[6]);
        seed = UbiquittyTest::soakNumber(argv[7]);
        if (mode != "quic" || std::string_view(argv[5]) != "--soak" || !iterations || !*iterations || *iterations > 10'000 || !seed)
            return 2;
    }
    Sync::QUICBootstrapInfo bootstrap;
    if (mode == "quic" && !loadAcceptanceBootstrap(argv[4], bootstrap))
        return 2;
    std::optional<Sync::QUICBootstrapInfo> observerBootstrap;
    if (argc == 10) {
        observerBootstrap.emplace();
        if (std::string_view(argv[8]) != "--observer-bootstrap" || !loadAcceptanceBootstrap(argv[9], *observerBootstrap))
            return 2;
    }
    if (iterations) {
        UbiquittyTest::runSyncReconnectSoak(bootstrap, *iterations, *seed, observerBootstrap);
        return failures ? 1 : 0;
    }
    exercise(mode == "ssh" ? Carrier::SSH : Carrier::QUIC, argv[3], std::move(bootstrap));
    if (!failures)
        std::printf("sync-transport: ok (%s; attach, typing, scrollback, images, resize, reconnect)\n", argv[2]);
    return failures ? 1 : 0;
}
