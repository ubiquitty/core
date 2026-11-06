// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Performance/transport_bench.cpp — what each remote carrier costs and
// what a person on the other end of it feels, measured on loopback with the
// shipping classes.
//
// Built and driven by Scripts/bench-transport.py, which owns the build, the
// private sshd, the flood corpus and the report. Every mode prints one JSON
// object per measurement on stdout and nothing else there; diagnostics go to
// stderr.
//
//   raw       A synthetic ceiling, not product behaviour: one-way bulk bytes
//             over a bare carrier (QUICClient against QUICListener, an AF_UNIX
//             LocalSocketChannel, a SubprocessChannel pipe, the same over ssh).
//             The product never streams pane output; Sync sends coalesced
//             screen state, so these numbers bound the carrier and nothing more.
//   datagram  Unreliable DATAGRAM frames from QUICListener to QUICClient.
//   e2e       A real daemon, a pane flooding a file, and RemoteSessionCore
//             attached over the bridge, ssh or QUIC: how long until the pane is
//             drained and until the client's mirror matches the host.
//   echo      Keystroke to echo through the same stack, idle and while the
//             same pane floods.
//   memory    The daemon's footprint with 1..N clients on one pane, idle, under
//             a flood, and with one client stopped so it reads nothing.
//
// Internal modes the driver and the pane run: raw-source, pane-echo,
// attach-client, shape-stdio, shape-udp, gen-flood.
//
// Impairment is applied in user space because the kernel shapers need root.
// shape-udp sits between QUICClient and the daemon's UDP port and imposes delay,
// loss and a rate on real packets, so QUIC's own loss recovery and congestion
// control respond. shape-stdio sits between a stream channel and its peer
// process; it imposes delay and a rate with a bounded queue, so backpressure
// reaches the host as it would through a congested TCP path, and it models a
// lost segment as one round trip of head-of-line delay — what a fast
// retransmit costs a TCP stream. That stream loss is a model, not a lost
// packet.
//

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Sync/SyncTrace.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/LocalSocketChannel.h"
#include "Core/Transport/QUICChannel.h"
#include "Core/Transport/QUICClient.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Platform/Host/HostQUIC.h"
#include "Platform/Host/HostSocketName.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

using namespace Ubiquitty;

namespace {

// ---------------------------------------------------------------------------
// Clocks, process accounting, output

double steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double wallSeconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void sleepMilliseconds(int milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

double selfCPUSeconds()
{
    rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

/// One process's CPU and memory, read through proc_pid_rusage.
struct ProcessSample {
    bool valid = false;
    double cpuSeconds = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t footprintBytes = 0;
};

ProcessSample sampleProcess(pid_t pid)
{
    rusage_info_v4 info {};
    if (::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return {};
    // The times are in Mach absolute units, which are not nanoseconds on
    // Apple silicon.
    mach_timebase_info_data_t timebase {};
    ::mach_timebase_info(&timebase);
    const double ticks = static_cast<double>(info.ri_user_time + info.ri_system_time);
    return { .valid = true, .cpuSeconds = ticks * timebase.numer / timebase.denom / 1e9, .residentBytes = info.ri_resident_size, .footprintBytes = info.ri_phys_footprint };
}

std::string processName(pid_t pid)
{
    std::array<char, 256> name {};
    if (::proc_name(pid, name.data(), static_cast<std::uint32_t>(name.size())) <= 0)
        return {};
    return name.data();
}

std::vector<pid_t> childProcesses(pid_t parent)
{
    std::vector<pid_t> children(512);
    const int count = ::proc_listchildpids(parent, children.data(), static_cast<int>(children.size() * sizeof(pid_t)));
    if (count <= 0)
        return {};
    children.resize(std::min<std::size_t>(static_cast<std::size_t>(count), children.size()));
    return children;
}

void collectDescendants(pid_t root, std::vector<pid_t>& out)
{
    for (const pid_t child : childProcesses(root)) {
        if (child <= 0)
            continue;
        out.push_back(child);
        collectDescendants(child, out);
    }
}

/// CPU of a set of processes classified by name, summed.
struct TreeCPU {
    double host = 0; // sshd and the bridge it runs: the host side of an ssh hop
    double client = 0; // ssh, or the local bridge: the client side of the hop
    double shaper = 0; // the impairment relay, which neither end pays in production
};

/// Reads every descendant of this process and of `sshdProcess`.
///
/// The local bridge is a client-side cost: it is the relay the app runs. Under
/// ssh the same binary runs on the host behind sshd, so its cost moves there.
TreeCPU sampleTree(pid_t sshdProcess, const std::string& benchName, pid_t daemonProcess = -1)
{
    TreeCPU result;
    std::vector<pid_t> local;
    collectDescendants(::getpid(), local);
    // The daemon and its panes are measured on their own.
    std::vector<pid_t> excluded { daemonProcess };
    if (daemonProcess > 0)
        collectDescendants(daemonProcess, excluded);
    for (const pid_t pid : local) {
        if (std::ranges::find(excluded, pid) != excluded.end())
            continue;
        const std::string name = processName(pid);
        const ProcessSample sample = sampleProcess(pid);
        if (!sample.valid)
            continue;
        if (name.starts_with(benchName))
            result.shaper += sample.cpuSeconds;
        else
            result.client += sample.cpuSeconds;
    }
    if (sshdProcess > 0) {
        std::vector<pid_t> remote;
        collectDescendants(sshdProcess, remote);
        for (const pid_t pid : remote) {
            const ProcessSample sample = sampleProcess(pid);
            if (sample.valid)
                result.host += sample.cpuSeconds;
        }
    }
    return result;
}

/// A flat JSON object, printed on one line.
class Record {
public:
    explicit Record(std::string_view kind) { field("kind", kind); }

    Record& field(std::string_view key, std::string_view value)
    {
        separator();
        m_text += '"';
        m_text += key;
        m_text += "\":\"";
        for (const char character : value) {
            if (character == '"' || character == '\\')
                m_text += '\\';
            if (static_cast<unsigned char>(character) >= 0x20)
                m_text += character;
        }
        m_text += '"';
        return *this;
    }

    Record& field(std::string_view key, const char* value) { return field(key, std::string_view(value)); }
    Record& field(std::string_view key, const std::string& value) { return field(key, std::string_view(value)); }

    Record& field(std::string_view key, double value)
    {
        separator();
        std::array<char, 64> number {};
        std::snprintf(number.data(), number.size(), "%.6g", value);
        m_text += '"';
        m_text += key;
        m_text += "\":";
        m_text += number.data();
        return *this;
    }

    Record& field(std::string_view key, std::uint64_t value) { return field(key, static_cast<double>(value)); }
    Record& field(std::string_view key, int value) { return field(key, static_cast<double>(value)); }

    Record& flag(std::string_view key, bool value) // bool-ok: a JSON boolean field
    {
        separator();
        m_text += '"';
        m_text += key;
        m_text += "\":";
        m_text += value ? "true" : "false";
        return *this;
    }

    void emit()
    {
        m_text += '}';
        std::printf("%s\n", m_text.c_str());
        std::fflush(stdout);
    }

private:
    void separator() { m_text += m_text.empty() ? "{" : ","; }

    std::string m_text;
};

// ---------------------------------------------------------------------------
// Arguments

class Arguments {
public:
    Arguments(int count, char** values)
    {
        for (int index = 0; index < count; ++index)
            m_values.emplace_back(values[index]);
    }

    [[nodiscard]] std::string text(std::string_view name, std::string fallback = {}) const
    {
        for (std::size_t index = 0; index + 1 < m_values.size(); ++index) {
            if (m_values[index] == name)
                return m_values[index + 1];
        }
        return fallback;
    }

    [[nodiscard]] double number(std::string_view name, double fallback) const
    {
        const std::string value = text(name);
        return value.empty() ? fallback : std::strtod(value.c_str(), nullptr);
    }

    [[nodiscard]] bool has(std::string_view name) const { return std::ranges::find(m_values, name) != m_values.end(); }

    /// Everything after a bare `--`.
    [[nodiscard]] std::vector<std::string> tail() const
    {
        const auto separator = std::ranges::find(m_values, std::string("--"));
        if (separator == m_values.end())
            return {};
        return { separator + 1, m_values.end() };
    }

private:
    std::vector<std::string> m_values;
};

std::string shellQuote(std::string_view value)
{
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    quoted += '\'';
    return quoted;
}

std::string selfPath()
{
    return Host::resolveSelfExecutablePath();
}

/// Spawns `argv` with the given extra environment, stdio inherited unless
/// redirected by the caller's file actions.
pid_t spawnProcess(const std::vector<std::string>& arguments, const std::vector<std::string>& extraEnvironment, posix_spawn_file_actions_t* actions = nullptr)
{
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
        const std::string_view text(*entry);
        const auto equals = text.find('=');
        const std::string_view key = text.substr(0, equals == std::string_view::npos ? text.size() : equals + 1);
        const bool isOverridden = std::ranges::any_of(extraEnvironment, [&](const std::string& extra) { return extra.starts_with(key); });
        if (!isOverridden)
            environment.emplace_back(text);
    }
    environment.insert(environment.end(), extraEnvironment.begin(), extraEnvironment.end());
    std::vector<char*> argv;
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : environment)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    pid_t pid = -1;
    if (::posix_spawn(&pid, argv[0], actions, nullptr, argv.data(), envp.data()) != 0)
        return -1;
    return pid;
}

void stopProcess(pid_t pid)
{
    if (pid <= 0)
        return;
    ::kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (::waitpid(pid, nullptr, WNOHANG) == pid)
            return;
        sleepMilliseconds(10);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

// ---------------------------------------------------------------------------
// The session thread: a dispatcher that wakes the moment work is posted, so a
// latency sample is not quantised by a polling interval.

class BenchDispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override
    {
        {
            std::lock_guard lock(m_mutex);
            m_ready.push_back(std::move(work));
        }
        m_wake.notify_one();
    }

    void postDelayed(double seconds, Session::PostedWork work) override
    {
        {
            std::lock_guard lock(m_mutex);
            m_delayed.emplace(steadySeconds() + seconds, std::move(work));
        }
        m_wake.notify_one();
    }

    /// Runs posted work until `isDone` holds or `timeoutSeconds` pass.
    ///
    /// - Returns: Whether `isDone` held.
    bool runUntil(const std::function<bool()>& isDone, double timeoutSeconds)
    {
        const double deadline = steadySeconds() + timeoutSeconds;
        for (;;) {
            if (isDone())
                return true;
            const double now = steadySeconds();
            if (now >= deadline)
                return false;
            std::vector<Session::PostedWork> batch;
            {
                std::unique_lock lock(m_mutex);
                double wakeAt = deadline;
                if (!m_delayed.empty())
                    wakeAt = std::min(wakeAt, m_delayed.begin()->first);
                if (m_ready.empty() && wakeAt > now)
                    m_wake.wait_for(lock, std::chrono::duration<double>(std::min(wakeAt - now, 0.05)));
                const double after = steadySeconds();
                while (!m_delayed.empty() && m_delayed.begin()->first <= after) {
                    batch.push_back(std::move(m_delayed.begin()->second));
                    m_delayed.erase(m_delayed.begin());
                }
                while (!m_ready.empty()) {
                    batch.push_back(std::move(m_ready.front()));
                    m_ready.pop_front();
                }
            }
            for (auto& work : batch)
                std::move(work)();
        }
    }

    void runFor(double seconds)
    {
        (void)runUntil([] { return false; }, seconds);
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Session::PostedWork> m_ready;
    std::multimap<double, Session::PostedWork> m_delayed;
};

// ---------------------------------------------------------------------------
// Carriers into a daemon

enum class Carrier : std::uint8_t {
    Bridge, // `ubiquitty-host --attach` over a pipe: the app's local path
    SSH, // the same bridge behind a real ssh hop
    QUIC, // QUICChannel, token minted by --quic-bootstrap
};

std::optional<Carrier> parseCarrier(std::string_view name)
{
    if (name == "bridge")
        return Carrier::Bridge;
    if (name == "ssh")
        return Carrier::SSH;
    if (name == "quic")
        return Carrier::QUIC;
    return std::nullopt;
}

const char* carrierName(Carrier carrier)
{
    switch (carrier) {
    case Carrier::Bridge:
        return "bridge";
    case Carrier::SSH:
        return "ssh";
    case Carrier::QUIC:
        return "quic";
    }
    return "?";
}

/// A link impairment: one-way delay, loss, and a rate.
struct Shape {
    double oneWayMilliseconds = 0;
    double lossPercent = 0;
    double bitsPerSecond = 0;

    [[nodiscard]] bool isClear() const { return oneWayMilliseconds == 0 && lossPercent == 0 && bitsPerSecond == 0; }

    [[nodiscard]] std::vector<std::string> arguments() const
    {
        return { "--delay-ms", std::to_string(oneWayMilliseconds), "--loss", std::to_string(lossPercent), "--rate", std::to_string(bitsPerSecond) };
    }

    [[nodiscard]] std::string label() const
    {
        if (isClear())
            return "loopback";
        std::array<char, 96> text {};
        std::snprintf(text.data(), text.size(), "rtt%.0fms-loss%.1f%%-rate%.0fMbit", oneWayMilliseconds * 2, lossPercent, bitsPerSecond / 1e6);
        return text.data();
    }
};

Shape parseShape(const Arguments& arguments)
{
    return { .oneWayMilliseconds = arguments.number("--delay-ms", 0), .lossPercent = arguments.number("--loss", 0), .bitsPerSecond = arguments.number("--rate", 0) };
}

/// Mints a one-time QUIC token the way the product does.
///
/// Pinned to the socket the binary's own daemon binds: an unpinned bootstrap
/// ensures a daemon first, and where none answers it starts one detached that
/// the bench never ends.
bool mintToken(const std::string& hostBinary, const std::string& home, Protocol::Sync::QUICBootstrapInfo& info)
{
    const std::string socketLeaf = Host::socketLeafForBinary(hostBinary.c_str());
    if (socketLeaf.empty())
        return false;
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return false;
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipeDescriptors[1], 1);
    ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[0]);
    const pid_t pid = spawnProcess({ hostBinary, "--quic-bootstrap", "--socket", socketLeaf }, { "UBIQUITTY_HOME=" + home }, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipeDescriptors[1]);
    std::string line;
    std::array<char, 512> buffer {};
    for (;;) {
        const ssize_t count = ::read(pipeDescriptors[0], buffer.data(), buffer.size());
        if (count <= 0)
            break;
        line.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipeDescriptors[0]);
    int status = 0;
    if (pid > 0)
        ::waitpid(pid, &status, 0);
    return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && Protocol::Sync::parseQUICBootstrapLine(line, info);
}

/// Where a client reaches the daemon, and through what.
struct Route {
    Carrier carrier = Carrier::Bridge;
    std::string hostBinary;
    std::string home;
    std::string sshPrefix; // `ssh -F <config> <host>`
    Shape shape;
    std::uint16_t quicProxyPort = 0; // the shape-udp port standing in for the daemon's
};

/// What the host sent a stream client, by Sync frame kind.
struct KindTally {
    std::array<std::atomic<std::uint64_t>, 256> frames {};
    std::array<std::atomic<std::uint64_t>, 256> bytes {};
    std::atomic<bool> isValid { true };

    [[nodiscard]] std::uint64_t framesOf(Protocol::Sync::FrameKind kind) const { return frames[std::to_underlying(kind)].load(); }
    [[nodiscard]] std::uint64_t bytesOf(Protocol::Sync::FrameKind kind) const { return bytes[std::to_underlying(kind)].load(); }
};

/// A stream carrier with a tap on what it delivers, decoded into Sync kinds.
///
/// Stream carriers only: a datagram path would bypass the tap, so a QUIC
/// carrier is counted from its own transport statistics instead.
class TappedChannel final : public Transport::Channel, private Transport::ChannelObserver {
public:
    TappedChannel(std::unique_ptr<Transport::Channel> inner, std::shared_ptr<KindTally> tally)
        : m_inner(std::move(inner))
        , m_tally(std::move(tally))
    {
    }

    ~TappedChannel() override { m_inner->stop(); }

    bool start(Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return m_inner->start(this);
    }
    void send(std::span<const std::uint8_t> data) override { m_inner->send(data); }
    void stop() override { m_inner->stop(); }
    void suspendReading() override { m_inner->suspendReading(); }
    void resumeReading() override { m_inner->resumeReading(); }
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override { return m_inner->pendingOutboundBytes(); }
    [[nodiscard]] std::int64_t outboundCapacityBytes() const override { return m_inner->outboundCapacityBytes(); }
    [[nodiscard]] Transport::Origin origin() const override { return m_inner->origin(); }
    [[nodiscard]] Transport::Health health() const override { return m_inner->health(); }
    [[nodiscard]] std::string description() const override { return m_inner->description(); }
    bool queryStats(Transport::TransportStats& out) const override { return m_inner->queryStats(out); }

private:
    void onConnect() override { m_observer->onConnect(); }
    void onData(std::span<const std::uint8_t> data) override
    {
        using namespace Protocol::Sync;
        const auto consumed = m_ingress.consume(data, [](std::span<const std::uint8_t> bytes) {
                FrameHeader header;
                EnvelopeError error = EnvelopeError::None;
                switch (readFrameHeader(bytes, header, error)) {
                case FrameHeaderState::NeedsMore:
                    return FrameHeaderDecision {};
                case FrameHeaderState::Complete:
                    return FrameHeaderDecision { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
                case FrameHeaderState::Failed:
                    return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
                }
                return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused }; }, [&](std::span<const std::uint8_t> header, std::span<const std::uint8_t> body) {
                // Delivery happens once, only after this complete envelope.
                // Fragmented headers contribute no repeated bytes or frames.
                const auto kind = header[0];
                m_tally->frames[kind].fetch_add(1, std::memory_order_relaxed);
                m_tally->bytes[kind].fetch_add(header.size() + body.size(), std::memory_order_relaxed);
                return FrameDelivery::Continue; });
        if ((consumed.state != FrameIngress::State::Accepting || consumed.consumedBytes != data.size())
            && m_tally->isValid.exchange(false))
            std::fprintf(stderr, "transport_bench: canonical frame accounting refused the stream\n");
        m_observer->onData(data);
    }
    void onDatagram(std::span<const std::uint8_t> data) override { m_observer->onDatagram(data); }
    void onClose(const std::string& reason) override
    {
        if (m_ingress.pendingBytes())
            m_tally->isValid = false;
        m_observer->onClose(reason);
    }

    std::unique_ptr<Transport::Channel> m_inner;
    std::shared_ptr<KindTally> m_tally;
    Transport::ChannelObserver* m_observer = nullptr;
    Protocol::Sync::FrameIngress m_ingress { Protocol::Sync::maxFrameBodyBytes };
};

class BenchChannelFactory final : public Session::ChannelFactory {
public:
    explicit BenchChannelFactory(Route route)
        : m_route(std::move(route))
    {
    }

    /// What the stream carriers this factory built have delivered.
    std::shared_ptr<KindTally> tally = std::make_shared<KindTally>();

    void createChannel(Session::TransportMode, Done done) override
    {
        const Transport::Origin origin { Transport::OriginKind::Daemon, "127.0.0.1" };
        if (m_route.carrier == Carrier::QUIC) {
            Protocol::Sync::QUICBootstrapInfo info;
            if (!mintToken(m_route.hostBinary, m_route.home, info)) {
                std::fprintf(stderr, "transport_bench: the QUIC token mint failed\n");
                done(nullptr, true);
                return;
            }
            const std::uint16_t port = m_route.quicProxyPort ? m_route.quicProxyPort : info.port;
            done(std::make_unique<Transport::QUICChannel>("127.0.0.1", port, info.token, info.certificateSHA256, origin), true);
            return;
        }
        const std::string attach = "UBIQUITTY_HOME=" + shellQuote(m_route.home) + " exec " + shellQuote(m_route.hostBinary) + " --attach";
        std::string command = m_route.carrier == Carrier::SSH ? m_route.sshPrefix + " " + shellQuote(attach) : attach;
        if (!m_route.shape.isClear()) {
            std::string shaper = "exec " + shellQuote(selfPath()) + " shape-stdio";
            for (const std::string& argument : m_route.shape.arguments())
                shaper += " " + shellQuote(argument);
            command = shaper + " -- /bin/sh -c " + shellQuote(command);
        } else if (m_route.carrier == Carrier::SSH) {
            command = "exec " + command;
        }
        done(std::make_unique<TappedChannel>(std::make_unique<Transport::SubprocessChannel>(command, origin), tally), false);
    }

private:
    Route m_route;
};

/// Learns the daemon's QUIC port by minting a token nobody presents.
std::uint16_t daemonQUICPort(const std::string& hostBinary, const std::string& home)
{
    Protocol::Sync::QUICBootstrapInfo info;
    return mintToken(hostBinary, home, info) ? info.port : 0;
}

class BenchObserver final : public Session::RemoteSessionObserver {
public:
    void onStateChanged(Session::State state, const std::string& detail) override
    {
        m_state = state;
        if (state == Session::State::Reconnecting) {
            ++m_reconnects;
            m_lastDrop = detail;
        }
        if (state == Session::State::Failed || state == Session::State::Reconnecting)
            std::fprintf(stderr, "transport_bench: session state %d: %s\n", static_cast<int>(state), detail.c_str());
    }
    void onScreenChanged() override
    {
        ++m_screenChanges;
        if (m_onScreen)
            m_onScreen();
    }
    void onExit() override { m_didExit = true; }

    Session::State m_state = Session::State::Idle;
    std::uint64_t m_screenChanges = 0;
    // A reconnect starts a new carrier, whose transport counters start again
    // from zero, so a delta across one is meaningless and is reported as such.
    std::uint64_t m_reconnects = 0;
    std::string m_lastDrop;
    bool m_didExit = false;
    std::function<void()> m_onScreen;
};

/// One attached client: its mirror, its observer and its core.
struct AttachedClient {
    AttachedClient(BenchDispatcher& dispatcher, Route route, int rows, int columns)
        : terminal(std::make_unique<Terminal>())
        , factory(std::move(route))
    {
        Ubiquitty::resize(*terminal, rows, columns);
        core = std::make_unique<Session::RemoteSessionCore>(terminal.get(), &dispatcher, &factory, &observer);
    }

    ~AttachedClient()
    {
        if (core)
            core->stop();
    }

    std::unique_ptr<Terminal> terminal;
    BenchObserver observer;
    BenchChannelFactory factory;
    std::unique_ptr<Session::RemoteSessionCore> core;
};

std::string rowText(const Terminal& terminal, int row)
{
    std::string text;
    for (int column = 0; column < terminal.columns; ++column) {
        const Cell* cell = Screen::visibleCell(&terminal, 0, row, column);
        std::uint32_t codePoint = cell ? cell->codePoint : 0;
        if (codePoint == 0)
            codePoint = ' ';
        text.push_back(codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
    }
    return text;
}

/// The visible grid with every cell's code point and pen, for comparing two
/// mirrors exactly.
std::vector<std::uint8_t> gridImage(const Terminal& terminal)
{
    std::vector<std::uint8_t> image;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Cell* cell = Screen::visibleCell(&terminal, 0, row, column);
            Cell copy {};
            if (cell) {
                copy.codePoint = cell->codePoint;
                copy.pen = cell->pen;
                copy.combining = cell->combining;
                copy.clusterPacked = cell->clusterPacked;
            }
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&copy);
            image.insert(image.end(), bytes, bytes + sizeof(copy));
        }
    }
    return image;
}

bool gridContains(const Terminal& terminal, std::string_view needle)
{
    for (int row = 0; row < terminal.rows; ++row) {
        if (rowText(terminal, row).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

/// A daemon this bench started, on its own socket, stopped by its own pid.
struct DaemonProcess {
    pid_t pid = -1;
    std::string home;
    std::string hostBinary;

    ~DaemonProcess() { stopProcess(pid); }
};

std::unique_ptr<DaemonProcess> startDaemon(const std::string& hostBinary, const std::string& scratch, int rows, int columns)
{
    auto daemon = std::make_unique<DaemonProcess>();
    daemon->hostBinary = hostBinary;
    std::string directory = scratch + "/home.XXXXXX";
    if (!::mkdtemp(directory.data()))
        return nullptr;
    daemon->home = directory + "/.ubiquitty";
    ::mkdir(daemon->home.c_str(), 0700);
    // A plain shell with no startup files, so the pane's own work is the
    // flood and nothing a profile happens to run. Its output goes to a log in
    // its home: a daemon that outlives a bench that died must not hold the
    // bench's stdout open for whoever is reading it.
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    const std::string log = daemon->home + "/daemon.log";
    ::posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    ::posix_spawn_file_actions_adddup2(&actions, 1, 2);
    daemon->pid = spawnProcess({ hostBinary, "--daemon", "--columns", std::to_string(columns), "--rows", std::to_string(rows), "--quic-port", "0" },
        { "UBIQUITTY_HOME=" + daemon->home, "SHELL=/bin/sh", "ENV=/dev/null", "PS1=$ ", "HISTFILE=/dev/null" }, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    if (daemon->pid <= 0)
        return nullptr;
    const std::string socketPath = daemon->home + "/" + Host::socketLeafForBinary(hostBinary.c_str());
    for (int attempt = 0; attempt < 3000; ++attempt) {
        const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
        const bool connected = ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        ::close(descriptor);
        if (connected)
            return daemon;
        sleepMilliseconds(10);
    }
    std::fprintf(stderr, "transport_bench: the daemon never listened on %s\n", socketPath.c_str());
    return nullptr;
}

/// The session's traffic since the trace was last reset.
struct TrafficSample {
    std::uint64_t syncFrames = 0;
    std::uint64_t syncBytes = 0;
    Transport::TransportStats quic;
};

TrafficSample sampleTraffic(const Session::RemoteSessionCore& core)
{
    TrafficSample sample;
    const auto totals = core.trace().totals();
    sample.syncFrames = totals.inboundFrames;
    sample.syncBytes = totals.inboundWireBytes;
    // Trace rows and totals count canonical frames directly; there is no
    // outer wrapper count distinct from the envelope count.
    (void)core.queryTransportStats(sample.quic);
    return sample;
}

struct CommonOptions {
    std::string hostBinary;
    std::string scratch;
    std::string sshPrefix;
    pid_t sshdProcess = -1;
    Carrier carrier = Carrier::Bridge;
    Shape shape;
    int rows = 48;
    int columns = 160;
};

CommonOptions parseCommon(const Arguments& arguments)
{
    CommonOptions options;
    options.hostBinary = arguments.text("--host");
    options.scratch = arguments.text("--scratch", "/tmp");
    options.sshPrefix = arguments.text("--ssh-prefix");
    options.sshdProcess = static_cast<pid_t>(arguments.number("--sshd-pid", -1));
    options.carrier = parseCarrier(arguments.text("--carrier", "bridge")).value_or(Carrier::Bridge);
    options.shape = parseShape(arguments);
    options.rows = static_cast<int>(arguments.number("--rows", 48));
    options.columns = static_cast<int>(arguments.number("--columns", 160));
    return options;
}

/// Starts a UDP shaper in front of the daemon's QUIC port when the link is impaired.
struct QUICProxy {
    pid_t pid = -1;
    std::uint16_t port = 0;

    ~QUICProxy() { stopProcess(pid); }
};

std::unique_ptr<QUICProxy> startQUICProxy(const Shape& shape, std::uint16_t upstreamPort)
{
    auto proxy = std::make_unique<QUICProxy>();
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return nullptr;
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipeDescriptors[1], 1);
    ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[0]);
    std::vector<std::string> command { selfPath(), "shape-udp", "--upstream-port", std::to_string(upstreamPort) };
    for (const std::string& argument : shape.arguments())
        command.push_back(argument);
    proxy->pid = spawnProcess(command, {}, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipeDescriptors[1]);
    std::array<char, 32> buffer {};
    const ssize_t count = ::read(pipeDescriptors[0], buffer.data(), buffer.size() - 1);
    ::close(pipeDescriptors[0]);
    if (count <= 0)
        return nullptr;
    proxy->port = static_cast<std::uint16_t>(std::atoi(buffer.data()));
    return proxy;
}

/// Everything a run against one daemon on one carrier needs.
struct Bench {
    CommonOptions options;
    BenchDispatcher dispatcher;
    std::unique_ptr<DaemonProcess> daemon;
    std::unique_ptr<QUICProxy> proxy;
    std::string benchName;

    bool start()
    {
        benchName = processName(::getpid());
        daemon = startDaemon(options.hostBinary, options.scratch, options.rows, options.columns);
        if (!daemon)
            return false;
        ::setenv("UBIQUITTY_HOME", daemon->home.c_str(), 1);
        if (options.carrier == Carrier::QUIC && !options.shape.isClear()) {
            const std::uint16_t port = daemonQUICPort(options.hostBinary, daemon->home);
            proxy = startQUICProxy(options.shape, port);
            if (!proxy)
                return false;
        }
        return true;
    }

    [[nodiscard]] Route route() const
    {
        return { .carrier = options.carrier, .hostBinary = options.hostBinary, .home = daemon->home, .sshPrefix = options.sshPrefix, .shape = options.shape, .quicProxyPort = proxy ? proxy->port : std::uint16_t { 0 } };
    }

    std::unique_ptr<AttachedClient> attach(std::uint32_t sessionID, double timeoutSeconds = 30)
    {
        auto client = std::make_unique<AttachedClient>(dispatcher, route(), options.rows, options.columns);
        client->core->start(sessionID, options.columns, options.rows);
        const bool ready = dispatcher.runUntil([&] { return client->core->acknowledged() && client->observer.m_screenChanges > 0; }, timeoutSeconds);
        if (!ready) {
            std::fprintf(stderr, "transport_bench: a %s client did not attach\n", carrierName(options.carrier));
            return nullptr;
        }
        return client;
    }

    /// Waits until the screen has not changed for `quietSeconds`.
    void settle(AttachedClient& client, double quietSeconds, double timeoutSeconds = 30)
    {
        const double deadline = steadySeconds() + timeoutSeconds;
        while (steadySeconds() < deadline) {
            const std::uint64_t before = client.observer.m_screenChanges;
            dispatcher.runFor(quietSeconds);
            if (client.observer.m_screenChanges == before)
                return;
        }
    }
};

Record& addShape(Record& record, const CommonOptions& options)
{
    return record.field("carrier", carrierName(options.carrier)).field("link", options.shape.label());
}

// ---------------------------------------------------------------------------
// e2e: a pane drains a file; how long until the host consumed it and until the
// client's mirror matches the host.

int runEndToEnd(const Arguments& arguments)
{
    Bench bench { .options = parseCommon(arguments) };
    const std::string floodCommand = arguments.text("--flood-command");
    const std::string label = arguments.text("--label", "flood");
    const double paneBytes = arguments.number("--pane-bytes", 0);
    const std::string sampleDirectory = arguments.text("--sample-dir");
    if (bench.options.hostBinary.empty() || floodCommand.empty()) {
        std::fprintf(stderr, "usage: e2e --host BIN --carrier bridge|ssh|quic --flood-command CMD --pane-bytes N\n");
        return 2;
    }
    if (!bench.start())
        return 1;
    auto client = bench.attach(0);
    if (!client)
        return 1;
    bench.settle(*client, 0.3);
    const std::uint32_t sessionID = client->core->sessionID();

    const std::string stamp = bench.daemon->home + "/consumed.stamp";
    const std::string marker = "BENCH_DONE_" + std::to_string(::getpid());
    // The marker is printed through an octal escape, so the echoed command line
    // never contains it and only the command's own output can match.
    const std::string command = floodCommand + "; perl -MTime::HiRes=time -e 'printf \"%.6f\\n\", time' > " + shellQuote(stamp) + "; printf 'BENCH_\\104ONE_%s\\n' " + std::to_string(::getpid()) + "\r";

    client->core->resetTrace();
    const TrafficSample trafficBefore = sampleTraffic(*client->core);
    const ProcessSample daemonBefore = sampleProcess(bench.daemon->pid);
    const double clientBefore = selfCPUSeconds();
    const TreeCPU treeBefore = sampleTree(bench.options.sshdProcess, bench.benchName, bench.daemon->pid);
    const KindTally& tally = *client->factory.tally;
    using Kind = Protocol::Sync::FrameKind;
    const std::array<std::uint64_t, 6> tallyBefore { tally.framesOf(Kind::Update), tally.bytesOf(Kind::Update), tally.framesOf(Kind::BulkChunk), tally.bytesOf(Kind::BulkChunk), tally.framesOf(Kind::CheckpointHead), client->core->trace().totals().resyncs };
    std::uint64_t peakFootprint = daemonBefore.footprintBytes;
    std::uint64_t peakResident = daemonBefore.residentBytes;

    const std::uint64_t reconnectsBefore = client->observer.m_reconnects;
    const double startWall = wallSeconds();
    const double start = steadySeconds();
    client->core->sendInput(command);

    std::vector<pid_t> samplers;
    bool hasSampled = sampleDirectory.empty();
    double nextMemorySample = start;
    bool didConverge = false;
    const double timeout = arguments.number("--timeout", 600);
    // The grid is read only after the screen changed: a scan per wake costs
    // the session thread enough to delay its acknowledgements, and the host
    // paces by those.
    std::uint64_t scannedAt = client->observer.m_screenChanges;
    const auto markerArrived = [&] {
        if (client->observer.m_screenChanges == scannedAt)
            return false;
        scannedAt = client->observer.m_screenChanges;
        return gridContains(*client->terminal, marker);
    };
    while (!didConverge && steadySeconds() - start < timeout) {
        didConverge = bench.dispatcher.runUntil(markerArrived, 0.25);
        if (steadySeconds() >= nextMemorySample) {
            const ProcessSample now = sampleProcess(bench.daemon->pid);
            peakFootprint = std::max(peakFootprint, now.footprintBytes);
            peakResident = std::max(peakResident, now.residentBytes);
            nextMemorySample = steadySeconds() + 0.5;
        }
        if (!hasSampled && steadySeconds() - start > arguments.number("--sample-after", 0.3)) {
            // A profile of both ends while the flood is in its steady state.
            hasSampled = true;
            const std::string prefix = sampleDirectory + "/" + std::string(carrierName(bench.options.carrier)) + "-" + label + "-" + bench.options.shape.label();
            samplers.push_back(spawnProcess({ "/usr/bin/sample", std::to_string(bench.daemon->pid), "2", "-mayDie", "-file", prefix + "-daemon.txt" }, {}));
            samplers.push_back(spawnProcess({ "/usr/bin/sample", std::to_string(::getpid()), "2", "-mayDie", "-file", prefix + "-client.txt" }, {}));
        }
    }
    const double convergedAt = steadySeconds();
    const ProcessSample daemonAfter = sampleProcess(bench.daemon->pid);
    const double clientAfter = selfCPUSeconds();
    const TreeCPU treeAfter = sampleTree(bench.options.sshdProcess, bench.benchName, bench.daemon->pid);
    const TrafficSample trafficAfter = sampleTraffic(*client->core);
    const std::array<std::uint64_t, 6> tallyAfter { tally.framesOf(Kind::Update), tally.bytesOf(Kind::Update), tally.framesOf(Kind::BulkChunk), tally.bytesOf(Kind::BulkChunk), tally.framesOf(Kind::CheckpointHead), client->core->trace().totals().resyncs };
    const std::uint64_t screenChanges = client->observer.m_screenChanges;

    double consumedWall = 0;
    if (FILE* file = std::fopen(stamp.c_str(), "r")) {
        if (std::fscanf(file, "%lf", &consumedWall) != 1)
            consumedWall = 0;
        std::fclose(file);
    }

    // The oracle: a client attaching now is handed the host's state as one
    // checkpoint. The streamed mirror converged if it holds the same cells.
    bench.settle(*client, 0.5);
    bool didOracleMatch = false;
    {
        Route oracleRoute = bench.route();
        oracleRoute.carrier = Carrier::Bridge;
        oracleRoute.shape = {};
        auto oracle = std::make_unique<AttachedClient>(bench.dispatcher, oracleRoute, bench.options.rows, bench.options.columns);
        oracle->core->start(sessionID, bench.options.columns, bench.options.rows);
        if (bench.dispatcher.runUntil([&] { return oracle->core->acknowledged() && oracle->observer.m_screenChanges > 0; }, 30)) {
            bench.settle(*oracle, 0.3);
            didOracleMatch = gridImage(*oracle->terminal) == gridImage(*client->terminal);
            if (!didOracleMatch) {
                for (int row = 0; row < bench.options.rows; ++row) {
                    const std::string streamed = rowText(*client->terminal, row);
                    const std::string fresh = rowText(*oracle->terminal, row);
                    if (streamed != fresh)
                        std::fprintf(stderr, "row %d differs:\n  mirror %s\n  oracle %s\n", row, streamed.c_str(), fresh.c_str());
                }
            }
        }
    }
    for (const pid_t sampler : samplers)
        ::waitpid(sampler, nullptr, 0);

    const double seconds = convergedAt - start;
    const double consumedSeconds = consumedWall > 0 ? consumedWall - startWall : -1;
    const std::uint64_t syncBytes = trafficAfter.syncBytes - trafficBefore.syncBytes;
    const std::uint64_t reconnects = client->observer.m_reconnects - reconnectsBefore;
    const bool hasValidQUICCounters = trafficAfter.quic.valid && reconnects == 0 && trafficAfter.quic.bytesReceived >= trafficBefore.quic.bytesReceived;
    const std::uint64_t quicBytes = hasValidQUICCounters ? trafficAfter.quic.bytesReceived - trafficBefore.quic.bytesReceived : 0;
    const bool hasValidKindCounters = bench.options.carrier != Carrier::QUIC && tally.isValid.load();
    // A QUIC run that reconnected has no valid wire count; its Sync count stands.
    const std::uint64_t wireBytes = bench.options.carrier == Carrier::QUIC && hasValidQUICCounters ? quicBytes : syncBytes;
    Record record("e2e");
    addShape(record, bench.options)
        .field("flood", label)
        .field("wireVersion", static_cast<int>(Protocol::Sync::wireVersion))
        .field("wireFormat", "canonical-envelope")
        .flag("frameKindCountersValid", hasValidKindCounters)
        .flag("converged", didConverge)
        .field("reconnects", reconnects)
        .field("lastDrop", client->observer.m_lastDrop)
        .flag("oracleMatched", didOracleMatch)
        .field("paneBytes", paneBytes)
        .field("consumedSeconds", consumedSeconds)
        .field("convergedSeconds", seconds)
        .field("lagSeconds", consumedSeconds > 0 ? seconds - consumedSeconds : -1)
        .field("syncBytes", syncBytes)
        .field("quicWireBytes", quicBytes)
        .field("wireBytes", wireBytes)
        .field("wirePerPaneByte", paneBytes > 0 ? static_cast<double>(wireBytes) / paneBytes : 0)
        .field("wireBytesPerSecond", static_cast<double>(wireBytes) / seconds)
        .field("canonicalFrames", trafficAfter.syncFrames - trafficBefore.syncFrames)
        .field("updateFrames", tallyAfter[0] - tallyBefore[0])
        .field("updateBytes", tallyAfter[1] - tallyBefore[1])
        .field("bulkChunkFrames", tallyAfter[2] - tallyBefore[2])
        .field("bulkChunkBytes", tallyAfter[3] - tallyBefore[3])
        .field("checkpointHeads", tallyAfter[4] - tallyBefore[4])
        .field("clientResyncs", tallyAfter[5] - tallyBefore[5])
        .field("screenApplies", screenChanges)
        .field("appliesPerSecond", static_cast<double>(screenChanges) / seconds)
        .field("quicPacketsReceived", hasValidQUICCounters ? trafficAfter.quic.packetsReceived - trafficBefore.quic.packetsReceived : 0)
        .field("quicPacketsLost", hasValidQUICCounters ? trafficAfter.quic.packetsLost - trafficBefore.quic.packetsLost : 0)
        .field("daemonCPUSeconds", daemonAfter.cpuSeconds - daemonBefore.cpuSeconds)
        .field("hostRelayCPUSeconds", treeAfter.host - treeBefore.host)
        .field("clientCPUSeconds", clientAfter - clientBefore)
        .field("clientRelayCPUSeconds", treeAfter.client - treeBefore.client)
        .field("shaperCPUSeconds", treeAfter.shaper - treeBefore.shaper)
        .field("daemonPeakFootprintBytes", peakFootprint)
        .field("daemonPeakResidentBytes", peakResident)
        .emit();
    client.reset();
    return didConverge && didOracleMatch && (bench.options.carrier == Carrier::QUIC || hasValidKindCounters) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// echo: keystroke to echo, through a program in the pane that echoes each key
// on the bottom row while it floods the rows above.

std::vector<double> percentiles(std::vector<double> samples, std::initializer_list<double> points)
{
    std::vector<double> result;
    if (samples.empty())
        return std::vector<double>(points.size(), -1);
    std::ranges::sort(samples);
    for (const double point : points) {
        const std::size_t index = std::min(samples.size() - 1, static_cast<std::size_t>(point * static_cast<double>(samples.size() - 1) + 0.5));
        result.push_back(samples[index]);
    }
    return result;
}

// The pane program's own keys are control bytes, so no echoed letter can be
// mistaken for one.
constexpr char floodKey = '\x01';
constexpr char quitKey = '\x04';

char echoCharacter(int index)
{
    static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return alphabet[static_cast<std::size_t>(index * 7) % alphabet.size()];
}

int runEcho(const Arguments& arguments)
{
    Bench bench { .options = parseCommon(arguments) };
    const int keys = static_cast<int>(arguments.number("--keys", 1000));
    const std::string floodFile = arguments.text("--flood-file");
    const bool isFlooding = arguments.has("--flood");
    const double gapSeconds = arguments.number("--gap-ms", 20) / 1000;
    if (bench.options.hostBinary.empty()) {
        std::fprintf(stderr, "usage: echo --host BIN --carrier C [--flood --flood-file F] [--keys N]\n");
        return 2;
    }
    if (!bench.start())
        return 1;
    auto client = bench.attach(0);
    if (!client)
        return 1;
    bench.settle(*client, 0.3);

    std::string command = shellQuote(selfPath()) + " pane-echo";
    if (isFlooding)
        command += " --flood " + shellQuote(floodFile);
    client->core->sendInput(command + "\r");
    if (!bench.dispatcher.runUntil([&] { return gridContains(*client->terminal, "ECHO_READY"); }, 30)) {
        std::fprintf(stderr, "transport_bench: the pane's echo program never started\n");
        return 1;
    }
    if (isFlooding) {
        client->core->sendInput(std::string(1, floodKey));
        bench.dispatcher.runFor(2.0);
    }

    const int lastRow = bench.options.rows - 1;
    std::vector<double> latencies;
    int timeouts = 0;
    Transport::TransportStats before {};
    (void)client->core->queryTransportStats(before);
    const std::uint64_t reconnectsBefore = client->observer.m_reconnects;
    const double cpuBefore = selfCPUSeconds();
    const ProcessSample daemonBefore = sampleProcess(bench.daemon->pid);
    const double started = steadySeconds();
    for (int index = 0; index < keys; ++index) {
        const char key = echoCharacter(index);
        const int column = index % 60;
        const double sent = steadySeconds();
        client->core->sendInput(std::string(1, key));
        const bool didEcho = bench.dispatcher.runUntil([&] {
            const Cell* cell = Screen::visibleCell(client->terminal.get(), 0, lastRow, column);
            return cell && cell->codePoint == static_cast<std::uint32_t>(key);
        },
            5.0);
        if (didEcho) {
            latencies.push_back((steadySeconds() - sent) * 1000);
        } else {
            if (!timeouts) {
                std::fprintf(stderr, "transport_bench: key %d ('%c' at column %d) never didEcho; the screen reads:\n", index, key, column);
                for (int row = 0; row < bench.options.rows; ++row)
                    std::fprintf(stderr, "  |%s|\n", rowText(*client->terminal, row).c_str());
            }
            if (++timeouts >= 5)
                break;
        }
        bench.dispatcher.runFor(gapSeconds);
    }
    const double elapsed = steadySeconds() - started;
    Transport::TransportStats after {};
    (void)client->core->queryTransportStats(after);
    const std::uint64_t reconnects = client->observer.m_reconnects - reconnectsBefore;
    if (reconnects > 0 || after.packetsReceived < before.packetsReceived)
        after.valid = false;
    const ProcessSample daemonAfter = sampleProcess(bench.daemon->pid);
    const double cpuAfter = selfCPUSeconds();
    client->core->sendInput(std::string(1, quitKey));
    bench.dispatcher.runFor(0.3);

    const std::vector<double> points = percentiles(latencies, { 0.5, 0.99, 1.0 });
    double mean = 0;
    for (const double latency : latencies)
        mean += latency;
    mean = latencies.empty() ? -1 : mean / static_cast<double>(latencies.size());
    Record record("echo");
    addShape(record, bench.options)
        .field("load", isFlooding ? "flood" : "idle")
        .field("keys", keys)
        .field("echoed", static_cast<int>(latencies.size()))
        .field("timeouts", timeouts)
        .field("reconnects", reconnects)
        .field("lastDrop", client->observer.m_lastDrop)
        .field("p50Milliseconds", points[0])
        .field("p99Milliseconds", points[1])
        .field("maxMilliseconds", points[2])
        .field("meanMilliseconds", mean)
        .field("quicPacketsReceivedPerKey", after.valid ? static_cast<double>(after.packetsReceived - before.packetsReceived) / keys : -1)
        .field("quicPacketsSentPerKey", after.valid ? static_cast<double>(after.packetsSent - before.packetsSent) / keys : -1)
        .field("quicBytesReceivedPerKey", after.valid ? static_cast<double>(after.bytesReceived - before.bytesReceived) / keys : -1)
        .field("quicSmoothedRTTMicroseconds", after.valid ? static_cast<double>(after.smoothedRttMicros) : -1)
        .field("daemonCPUSeconds", daemonAfter.cpuSeconds - daemonBefore.cpuSeconds)
        .field("clientCPUSeconds", cpuAfter - cpuBefore)
        .field("elapsedSeconds", elapsed)
        .emit();
    return timeouts == 0 ? 0 : 1;
}

/// The pane-side half of `echo`: raw input, each key drawn on the bottom row,
/// and optionally a flood of a corpus through the rows above.
int runPaneEcho(const Arguments& arguments)
{
    const std::string floodFile = arguments.text("--flood");
    winsize size {};
    ::ioctl(0, TIOCGWINSZ, &size);
    const int rows = size.ws_row > 2 ? size.ws_row : 24;
    termios original {};
    ::tcgetattr(0, &original);
    termios raw = original;
    ::cfmakeraw(&raw);
    raw.c_oflag |= OPOST | ONLCR;
    ::tcsetattr(0, TCSANOW, &raw);

    std::span<const char> corpus;
    void* mapping = MAP_FAILED;
    std::size_t mappingLength = 0;
    if (!floodFile.empty()) {
        const int descriptor = ::open(floodFile.c_str(), O_RDONLY);
        struct stat status {};
        if (descriptor >= 0 && ::fstat(descriptor, &status) == 0 && status.st_size > 0) {
            mappingLength = static_cast<std::size_t>(status.st_size);
            mapping = ::mmap(nullptr, mappingLength, PROT_READ, MAP_PRIVATE, descriptor, 0);
            if (mapping != MAP_FAILED)
                corpus = { static_cast<const char*>(mapping), mappingLength };
        }
        if (descriptor >= 0)
            ::close(descriptor);
    }

    const auto writeAll = [](std::string_view bytes) {
        while (!bytes.empty()) {
            const ssize_t written = ::write(1, bytes.data(), bytes.size());
            if (written <= 0) {
                if (written < 0 && errno == EINTR)
                    continue;
                return;
            }
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
    };
    // The rows above the last scroll; the last is the echo line.
    writeAll("\x1b[2J\x1b[1;" + std::to_string(rows - 1) + "r\x1b[1;1HECHO_READY\r\n");

    bool isFlooding = false;
    std::size_t offset = 0;
    int column = 0;
    std::array<char, 256> input {};
    for (;;) {
        pollfd descriptor { 0, POLLIN, 0 };
        const int ready = ::poll(&descriptor, 1, isFlooding ? 0 : -1);
        if (ready > 0) {
            const ssize_t count = ::read(0, input.data(), input.size());
            if (count <= 0)
                break;
            std::string echo;
            bool shouldQuit = false;
            for (ssize_t index = 0; index < count; ++index) {
                const char key = input[static_cast<std::size_t>(index)];
                if (key == quitKey) {
                    shouldQuit = true;
                    break;
                }
                if (key == floodKey && !corpus.empty()) {
                    isFlooding = true;
                    continue;
                }
                // DECSC, the cursor to the echo cell, the key, DECRC. Octal,
                // because a hex escape would swallow the digit after it.
                echo += "\0337\033[" + std::to_string(rows) + ";" + std::to_string(column % 60 + 1) + "H";
                echo += key;
                echo += "\0338";
                ++column;
            }
            writeAll(echo);
            if (shouldQuit)
                break;
        }
        if (isFlooding) {
            // Small writes, so a key waits behind at most one of them.
            constexpr std::size_t chunk = 4096;
            if (offset + chunk > corpus.size())
                offset = 0;
            std::size_t end = offset + chunk;
            while (end < corpus.size() && corpus[end] != '\n')
                ++end;
            writeAll({ corpus.data() + offset, end - offset });
            offset = end;
        }
    }
    writeAll("\x1b[r\x1b[0m\x1b[2J\x1b[H");
    ::tcsetattr(0, TCSANOW, &original);
    if (mapping != MAP_FAILED)
        ::munmap(mapping, mappingLength);
    return 0;
}

// ---------------------------------------------------------------------------
// memory: the daemon's footprint with many clients on one pane.

struct MemoryCurve {
    std::vector<std::pair<double, ProcessSample>> samples;

    void add(double time, const ProcessSample& sample) { samples.emplace_back(time, sample); }

    [[nodiscard]] std::uint64_t peakFootprint() const
    {
        std::uint64_t peak = 0;
        for (const auto& [time, sample] : samples)
            peak = std::max(peak, sample.footprintBytes);
        return peak;
    }

    [[nodiscard]] std::uint64_t peakResident() const
    {
        std::uint64_t peak = 0;
        for (const auto& [time, sample] : samples)
            peak = std::max(peak, sample.residentBytes);
        return peak;
    }

    /// Least-squares slope of the footprint over the later half, bytes per second.
    [[nodiscard]] double lateSlope() const
    {
        if (samples.size() < 4)
            return 0;
        const std::size_t first = samples.size() / 2;
        double sumTime = 0, sumBytes = 0, sumTimeBytes = 0, sumTimeTime = 0;
        const double count = static_cast<double>(samples.size() - first);
        for (std::size_t index = first; index < samples.size(); ++index) {
            const double time = samples[index].first;
            const double bytes = static_cast<double>(samples[index].second.footprintBytes);
            sumTime += time;
            sumBytes += bytes;
            sumTimeBytes += time * bytes;
            sumTimeTime += time * time;
        }
        const double denominator = count * sumTimeTime - sumTime * sumTime;
        return denominator == 0 ? 0 : (count * sumTimeBytes - sumTime * sumBytes) / denominator;
    }
};

/// Runs a command and returns what it printed.
std::string captureOutput(const std::vector<std::string>& command)
{
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return {};
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipeDescriptors[1], 1);
    ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[0]);
    const pid_t pid = spawnProcess(command, {}, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipeDescriptors[1]);
    std::string output;
    std::array<char, 4096> buffer {};
    for (;;) {
        const ssize_t count = ::read(pipeDescriptors[0], buffer.data(), buffer.size());
        if (count <= 0)
            break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipeDescriptors[0]);
    if (pid > 0)
        ::waitpid(pid, nullptr, 0);
    return output;
}

/// The bytes a process's malloc zones hold allocated, from `vmmap --summary`.
///
/// The footprint counts pages malloc has not given back, so a client that
/// detached can still read as resident for a long while after; this counts what
/// is live, which is what a per-client increment is a difference of.
///
/// - Parameter pid: The process.
/// - Returns: The bytes, or a negative value when vmmap could not say.
double mallocAllocatedBytes(pid_t pid)
{
    const std::string output = captureOutput({ "/usr/bin/vmmap", "--summary", std::to_string(pid) });
    const std::size_t table = output.find("\nMALLOC ZONE");
    const std::size_t total = table == std::string::npos ? std::string::npos : output.find("\nTOTAL", table);
    if (total == std::string::npos)
        return -1;
    const std::size_t end = output.find('\n', total + 1);
    std::vector<std::string> fields;
    std::string field;
    for (const char character : std::string_view(output).substr(total + 1, end - total - 1)) {
        if (character == ' ') {
            if (!field.empty())
                fields.push_back(std::exchange(field, {}));
        } else {
            field += character;
        }
    }
    if (!field.empty())
        fields.push_back(field);
    // TOTAL, virtual, resident, dirty, swapped, allocation count, bytes allocated, ...
    if (fields.size() < 7)
        return -1;
    const std::string& allocated = fields[6];
    double value = std::strtod(allocated.c_str(), nullptr);
    switch (allocated.back()) {
    case 'K':
        value *= 1024;
        break;
    case 'M':
        value *= 1024 * 1024;
        break;
    case 'G':
        value *= 1024.0 * 1024 * 1024;
        break;
    default:
        break;
    }
    return value;
}

/// A client that attaches, says so, and is then stopped by its parent so it
/// reads nothing: a laptop with its lid shut.
int runAttachClient(const Arguments& arguments)
{
    Bench bench { .options = parseCommon(arguments) };
    const auto sessionID = static_cast<std::uint32_t>(arguments.number("--session", 0));
    bench.daemon = std::make_unique<DaemonProcess>();
    bench.daemon->home = arguments.text("--home");
    bench.daemon->hostBinary = bench.options.hostBinary;
    auto client = bench.attach(sessionID);
    // Not this process's daemon: never stop it.
    bench.daemon->pid = -1;
    if (!client)
        return 1;
    std::printf("ATTACHED\n");
    std::fflush(stdout);
    for (;;)
        bench.dispatcher.runFor(1.0);
}

pid_t spawnStalledClient(const Bench& bench, std::uint32_t sessionID)
{
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipeDescriptors[1], 1);
    ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[0]);
    std::vector<std::string> command { selfPath(), "attach-client", "--host", bench.options.hostBinary, "--home", bench.daemon->home, "--carrier", carrierName(bench.options.carrier), "--session", std::to_string(sessionID), "--rows", std::to_string(bench.options.rows), "--columns", std::to_string(bench.options.columns) };
    if (!bench.options.sshPrefix.empty()) {
        command.push_back("--ssh-prefix");
        command.push_back(bench.options.sshPrefix);
    }
    const pid_t pid = spawnProcess(command, {}, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipeDescriptors[1]);
    std::array<char, 32> buffer {};
    const ssize_t count = ::read(pipeDescriptors[0], buffer.data(), buffer.size() - 1);
    ::close(pipeDescriptors[0]);
    if (count <= 0 || std::string_view(buffer.data()).find("ATTACHED") == std::string_view::npos) {
        stopProcess(pid);
        return -1;
    }
    return pid;
}

int runMemory(const Arguments& arguments)
{
    Bench bench { .options = parseCommon(arguments) };
    const std::string floodFile = arguments.text("--flood-file");
    const double floodSeconds = arguments.number("--flood-seconds", 20);
    const double stallSeconds = arguments.number("--stall-seconds", 120);
    std::vector<int> counts;
    const std::string countList = arguments.text("--clients", "1,2,5,10");
    for (const char* cursor = countList.c_str(); *cursor;) {
        char* end = nullptr;
        counts.push_back(static_cast<int>(std::strtol(cursor, &end, 10)));
        cursor = *end == ',' ? end + 1 : end;
    }
    if (bench.options.hostBinary.empty() || floodFile.empty()) {
        std::fprintf(stderr, "usage: memory --host BIN --carrier C --flood-file F\n");
        return 2;
    }
    if (!bench.start())
        return 1;
    std::vector<std::unique_ptr<AttachedClient>> clients;
    clients.push_back(bench.attach(0));
    if (!clients.back())
        return 1;
    bench.settle(*clients.front(), 0.3);
    const std::uint32_t sessionID = clients.front()->core->sessionID();
    // One `cat` of the file named many times over, in place of the subshell
    // that builds its arguments: a single process with the default answer to
    // SIGINT, so a flood that survives a stop means the signal never reached
    // the pane's foreground group. A shell loop of `cat`s cannot say that. An
    // interrupt that lands between two of them, or on one that has not yet
    // replaced the shell that forked it, kills nothing, and whether the loop
    // then ends is the shell's decision; a trap in the subshell does not
    // settle it either.
    struct stat floodStat {};
    const std::uint64_t floodFileBytes = ::stat(floodFile.c_str(), &floodStat) == 0 && floodStat.st_size > 0 ? static_cast<std::uint64_t>(floodStat.st_size) : 1;
    constexpr std::uint64_t floodBytes = std::uint64_t { 64 } << 30;
    const std::uint64_t floodRepeats = std::clamp<std::uint64_t>(floodBytes / floodFileBytes + 1, 1, 2048);
    const std::string floodLoop = "(n=0; set --; while [ $n -lt " + std::to_string(floodRepeats) + " ]; do set -- \"$@\" " + shellQuote(floodFile)
        + "; n=$((n+1)); done; exec cat \"$@\")\r";

    const auto sampleFor = [&](double seconds, MemoryCurve& curve, double interval) {
        const double start = steadySeconds();
        while (steadySeconds() - start < seconds) {
            bench.dispatcher.runFor(interval);
            curve.add(steadySeconds() - start, sampleProcess(bench.daemon->pid));
        }
    };
    const auto stopFlood = [&] {
        clients.front()->core->sendInput("\x03");
        bench.dispatcher.runFor(0.5);
        clients.front()->core->sendInput("printf 'MEM_\\104ONE\\n'\r");
        std::uint64_t scannedAt = 0;
        const bool didAllCatchUp = bench.dispatcher.runUntil([&] {
            std::uint64_t changes = 0;
            for (const auto& client : clients)
                changes += client->observer.m_screenChanges;
            if (changes == scannedAt)
                return false;
            scannedAt = changes;
            return std::ranges::all_of(clients, [](const auto& client) { return gridContains(*client->terminal, "MEM_DONE"); });
        },
            120);
        if (!didAllCatchUp) {
            // Whether the pane is still producing: the daemon spends a core on a
            // flood and next to nothing on an idle prompt.
            // Whether the daemon is alive at all, what the pane's processes
            // are doing, what the daemon logged and where its threads sit: a
            // daemon that has gone reads as 0.00 s of CPU exactly as an idle
            // one does, and an idle one with a shell at its prompt is a pane
            // whose screen stopped following its program.
            {
                const std::string daemonID = std::to_string(bench.daemon->pid);
                const std::string sampleFile = bench.options.scratch + "/daemon-sample-" + daemonID + ".txt";
                const std::string command = "{ echo \"transport_bench: daemon " + daemonID + " alive: $(kill -0 " + daemonID + " 2>/dev/null && echo yes || echo no)\";"
                                                                                                                               " for child in $(pgrep -P "
                    + daemonID + "); do tty=$(ps -o tty= -p $child | tr -d ' ');"
                                 " echo \"transport_bench: pane tty $tty\"; ps -o pid,ppid,pgid,tpgid,stat,%cpu,command -t \"$tty\"; done;"
                                 " kill -USR1 "
                    + daemonID + " 2>/dev/null; sleep 0.3;"
                                 " echo 'transport_bench: daemon log:'; tail -n 20 "
                    + shellQuote(bench.daemon->home + "/daemon.log") + ";"
                                                                       " sample "
                    + daemonID + " 1 -mayDie -file " + shellQuote(sampleFile) + " >/dev/null 2>&1; echo 'transport_bench: stack sample in '" + shellQuote(sampleFile) + "; } 1>&2";
                (void)std::system(command.c_str());
                // Whether input still reaches the pane and its echo comes back.
                const std::uint64_t changesBefore = clients.front()->observer.m_screenChanges;
                clients.front()->core->sendInput("echo PROBE_AFTER_MISS\r");
                bench.dispatcher.runFor(3.0);
                std::fprintf(stderr, "transport_bench: a probe line typed after the miss: %s on the client, %llu screen changes since\n",
                    gridContains(*clients.front()->terminal, "PROBE_AFTER_MISS") ? "shown" : "not shown",
                    static_cast<unsigned long long>(clients.front()->observer.m_screenChanges - changesBefore));
                (void)std::system(("{ for child in $(pgrep -P " + daemonID + "); do ps -o pid,stat,command -t \"$(ps -o tty= -p $child | tr -d ' ')\"; done; } 1>&2").c_str());
                // Whether the path to the pane still carries an interrupt: a
                // second one that ends the flood says the first was delivered
                // and the shell let it go, not that it was held on the way.
                clients.front()->core->sendInput("\x03");
                bench.dispatcher.runFor(2.0);
                std::fprintf(stderr, "transport_bench: the pane's processes after a second interrupt:\n");
                (void)std::system(("{ for child in $(pgrep -P " + daemonID + "); do ps -o pid,stat,command -t \"$(ps -o tty= -p $child | tr -d ' ')\"; done; } 1>&2").c_str());
            }
            const double cpuBefore = sampleProcess(bench.daemon->pid).cpuSeconds;
            bench.dispatcher.runFor(2.0);
            std::fprintf(stderr, "transport_bench: daemon CPU over 2 s at the miss: %.2f s\n", sampleProcess(bench.daemon->pid).cpuSeconds - cpuBefore);
            // A fresh attach tells a stale mirror from a pane that never printed
            // the marker: it gets the daemon's state as it is now.
            const auto witness = bench.attach(sessionID);
            if (witness)
                bench.settle(*witness, 1.0);
            const bool doesWitnessShowMarker = witness && gridContains(*witness->terminal, "MEM_DONE");
            for (std::size_t index = 0; index < clients.size(); ++index) {
                if (gridContains(*clients[index]->terminal, "MEM_DONE"))
                    continue;
                std::fprintf(stderr, "transport_bench: client %zu never showed MEM_DONE (state %d, %llu screen changes); a fresh attach %s it\n", index,
                    static_cast<int>(clients[index]->observer.m_state), static_cast<unsigned long long>(clients[index]->observer.m_screenChanges),
                    doesWitnessShowMarker ? "shows" : "does not show");
                for (int row = std::max(0, clients[index]->terminal->rows - 4); row < clients[index]->terminal->rows; ++row)
                    std::fprintf(stderr, "transport_bench:   client row %d: %s\n", row, rowText(*clients[index]->terminal, row).c_str());
            }
            if (witness) {
                for (int row = 0; row < witness->terminal->rows; ++row) {
                    const std::string text = rowText(*witness->terminal, row);
                    if (text.find_first_not_of(' ') != std::string::npos)
                        std::fprintf(stderr, "transport_bench:   fresh row %d: %s\n", row, text.c_str());
                }
                std::fprintf(stderr, "transport_bench:   fresh state %d, %llu screen changes\n", static_cast<int>(witness->observer.m_state),
                    static_cast<unsigned long long>(witness->observer.m_screenChanges));
            }
        }
        bench.settle(*clients.front(), 0.5);
        clients.front()->core->sendInput("clear\r");
        bench.settle(*clients.front(), 0.5);
        return didAllCatchUp;
    };

    // Fill the history ring first, so every client count below is measured
    // against the same scrollback rather than against one the flood grows.
    clients.front()->core->sendInput(floodLoop);
    bench.dispatcher.runFor(std::min(floodSeconds, 10.0));
    (void)stopFlood();

    for (const int count : counts) {
        while (static_cast<int>(clients.size()) < count) {
            clients.push_back(bench.attach(sessionID));
            if (!clients.back())
                return 1;
        }
        MemoryCurve idle;
        sampleFor(3.0, idle, 0.5);
        const double idleMalloc = mallocAllocatedBytes(bench.daemon->pid);
        clients.front()->core->sendInput(floodLoop);
        MemoryCurve flood;
        sampleFor(floodSeconds, flood, 0.5);
        const double floodMalloc = mallocAllocatedBytes(bench.daemon->pid);
        const bool didCatchUp = stopFlood();
        MemoryCurve after;
        sampleFor(2.0, after, 0.5);
        Record("memory")
            .field("idleMallocBytes", idleMalloc)
            .field("floodEndMallocBytes", floodMalloc)
            .field("carrier", carrierName(bench.options.carrier))
            .field("clients", count)
            .field("phase", "attached")
            .field("idleFootprintBytes", idle.samples.back().second.footprintBytes)
            .field("idleResidentBytes", idle.samples.back().second.residentBytes)
            .field("floodPeakFootprintBytes", flood.peakFootprint())
            .field("floodPeakResidentBytes", flood.peakResident())
            .field("floodFinalFootprintBytes", flood.samples.back().second.footprintBytes)
            .field("floodLateSlopeBytesPerSecond", flood.lateSlope())
            .field("afterFootprintBytes", after.samples.back().second.footprintBytes)
            .field("floodSeconds", floodSeconds)
            .flag("allClientsCaughtUp", didCatchUp)
            .emit();
    }

    // One of the clients stops reading for the whole flood.
    if (stallSeconds > 0) {
        while (clients.size() > 9)
            clients.pop_back();
        while (clients.size() < 9) {
            clients.push_back(bench.attach(sessionID));
            if (!clients.back())
                return 1;
        }
        const pid_t stalled = spawnStalledClient(bench, sessionID);
        if (stalled <= 0) {
            std::fprintf(stderr, "transport_bench: the stalled client did not attach\n");
            return 1;
        }
        bench.dispatcher.runFor(2.0);
        MemoryCurve idle;
        sampleFor(2.0, idle, 0.5);
        const double idleMalloc = mallocAllocatedBytes(bench.daemon->pid);
        ::kill(stalled, SIGSTOP);
        clients.front()->core->sendInput(floodLoop);
        MemoryCurve flood;
        std::string mallocCurve;
        for (double elapsed = 0; elapsed < stallSeconds; elapsed += 30) {
            MemoryCurve part;
            sampleFor(std::min(30.0, stallSeconds - elapsed), part, 1.0);
            for (const auto& [time, sample] : part.samples)
                flood.add(elapsed + time, sample);
            mallocCurve += std::to_string(static_cast<int>(elapsed + 30)) + ":" + std::to_string(static_cast<long long>(mallocAllocatedBytes(bench.daemon->pid) / 1024)) + " ";
        }
        const double floodMalloc = mallocAllocatedBytes(bench.daemon->pid);
        const bool didCatchUp = stopFlood();
        MemoryCurve after;
        sampleFor(3.0, after, 0.5);
        const bool isStalledClientAlive = ::waitpid(stalled, nullptr, WNOHANG) == 0;
        ::kill(stalled, SIGKILL);
        ::waitpid(stalled, nullptr, 0);
        MemoryCurve released;
        sampleFor(3.0, released, 0.5);
        const double releasedMalloc = mallocAllocatedBytes(bench.daemon->pid);
        Record("memory")
            .field("idleMallocBytes", idleMalloc)
            .field("floodEndMallocBytes", floodMalloc)
            .field("releasedMallocBytes", releasedMalloc)
            .field("mallocKiBByTime", mallocCurve)
            .field("carrier", carrierName(bench.options.carrier))
            .field("clients", 10)
            .field("phase", "stalled")
            .field("idleFootprintBytes", idle.samples.back().second.footprintBytes)
            .field("idleResidentBytes", idle.samples.back().second.residentBytes)
            .field("floodPeakFootprintBytes", flood.peakFootprint())
            .field("floodPeakResidentBytes", flood.peakResident())
            .field("floodFinalFootprintBytes", flood.samples.back().second.footprintBytes)
            .field("floodLateSlopeBytesPerSecond", flood.lateSlope())
            .field("afterFootprintBytes", after.samples.back().second.footprintBytes)
            .field("releasedFootprintBytes", released.samples.back().second.footprintBytes)
            .field("floodSeconds", stallSeconds)
            .flag("allClientsCaughtUp", didCatchUp)
            .flag("stalledProcessAlive", isStalledClientAlive)
            .emit();
        std::string curve;
        for (const auto& [time, sample] : flood.samples)
            curve += std::to_string(static_cast<int>(time)) + ":" + std::to_string(sample.footprintBytes / 1024) + " ";
        Record("memory-curve").field("carrier", carrierName(bench.options.carrier)).field("footprintKiBByTime", curve).emit();
    }
    clients.clear();
    return 0;
}

// ---------------------------------------------------------------------------
// raw: a synthetic ceiling — bulk bytes over a bare carrier.

/// The byte a raw QUIC sender puts at a stream offset.
constexpr std::uint8_t rawPatternByte(std::uint64_t offset)
{
    return static_cast<std::uint8_t>((offset % (256 * 1024)) * 131 + 7);
}

/// What a raw server reports back to the client process.
struct ServerReport {
    double cpuSeconds = 0;
    double relayCPUSeconds = 0;
    std::uint64_t count = 0;
    std::uint64_t corrupt = 0;
};

class RawObserver final : public Transport::ChannelObserver {
public:
    void onConnect() override { m_connected = true; }
    void onData(std::span<const std::uint8_t> data) override
    {
        if (m_received.load(std::memory_order_relaxed) == 0)
            m_firstAt = steadySeconds();
        const std::uint64_t total = m_received.fetch_add(data.size(), std::memory_order_relaxed) + data.size();
        if (total >= m_expected && m_doneAt == 0)
            m_doneAt = steadySeconds();
    }
    void onClose(const std::string& reason) override
    {
        m_closed = true;
        m_reason = reason;
    }

    std::uint64_t m_expected = 0;
    std::atomic<std::uint64_t> m_received { 0 };
    std::atomic<bool> m_connected { false };
    std::atomic<bool> m_closed { false };
    double m_firstAt = 0;
    std::atomic<double> m_doneAt { 0 };
    std::string m_reason;
};

void writeReport(int descriptor, const ServerReport& report)
{
    (void)::write(descriptor, &report, sizeof(report));
}

ServerReport readReport(int descriptor, double timeoutSeconds)
{
    ServerReport report;
    pollfd ready { descriptor, POLLIN, 0 };
    if (::poll(&ready, 1, static_cast<int>(timeoutSeconds * 1000)) > 0)
        (void)::read(descriptor, &report, sizeof(report));
    return report;
}

/// The listener half, in its own process so its CPU is its own.
///
/// The first bytes on the stream say what to do: 'D' then a count sends that
/// many bytes down; 'U' then a count reads that many and reports progress
/// every MiB; 'G' then a size and a duration sends datagrams of that size.
[[noreturn]] void runQUICServer(int portPipe, int reportPipe, const std::string& home, std::size_t backlogBytes)
{
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
    Host::QUICListener listener;
    if (!listener.start(0)) {
        std::fprintf(stderr, "listener: %s\n", listener.lastError().c_str());
        ::_exit(1);
    }
    const std::uint16_t port = listener.boundPort();
    (void)::write(portPipe, &port, sizeof(port));
    ::close(portPipe);

    Host::QUICConnection* connection = nullptr;
    std::vector<std::uint8_t> command;
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t nextProgress = 1 << 20;
    bool isFinished = false;
    listener.setStreamDataCallback([&](Host::QUICConnection* from, std::span<const std::uint8_t> bytes) {
        if (!connection) {
            connection = from;
            listener.markAuthenticated(from);
        }
        if (command.size() < 17) {
            const std::size_t take = std::min(bytes.size(), 17 - command.size());
            command.insert(command.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));
            bytes = bytes.subspan(take);
        }
        if (!bytes.empty() && bytes[0] == 'E' && command.size() == 17 && command[0] != 'U') {
            isFinished = true;
            return;
        }
        if (command.size() == 17 && command[0] == 'U') {
            for (std::size_t index = 0; index < bytes.size(); ++index)
                corrupt += bytes[index] != rawPatternByte(received + index) ? 1 : 0;
        }
        received += bytes.size();
    });
    listener.setConnectionClosedCallback([&](Host::QUICConnection*) { isFinished = true; });

    std::vector<std::uint8_t> chunk(256 * 1024);
    for (std::size_t index = 0; index < chunk.size(); ++index)
        chunk[index] = rawPatternByte(index);
    std::uint64_t sent = 0;
    std::uint64_t datagramsOffered = 0;
    double datagramDeadline = 0;
    bool didSendDatagramReport = false;
    while (!isFinished) {
        pollfd descriptor { listener.fileDescriptor(), POLLIN, 0 };
        int timeout = listener.nextTimeoutMilliseconds();
        if (connection && command.size() == 17 && command[0] == 'G' && !didSendDatagramReport)
            timeout = 0;
        ::poll(&descriptor, 1, timeout < 0 ? 1000 : timeout);
        listener.processReadable();
        listener.serviceTimers();
        if (connection && command.size() == 17) {
            std::uint64_t argument = 0;
            std::memcpy(&argument, command.data() + 1, sizeof(argument));
            std::uint64_t second = 0;
            std::memcpy(&second, command.data() + 9, sizeof(second));
            if (command[0] == 'D') {
                while (sent < argument && listener.pendingBytes(connection) < backlogBytes) {
                    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), argument - sent));
                    listener.sendStream(connection, { chunk.data(), take });
                    sent += take;
                }
            } else if (command[0] == 'U') {
                while (received >= nextProgress || (received >= argument && nextProgress <= argument + (1 << 20))) {
                    const std::uint64_t progress = std::min(received, argument);
                    listener.sendStream(connection, { reinterpret_cast<const std::uint8_t*>(&progress), sizeof(progress) });
                    nextProgress += 1 << 20;
                    if (progress >= argument) {
                        // Every byte arrived: the report is this side's cost.
                        nextProgress = ~std::uint64_t { 0 };
                        listener.flush();
                        isFinished = true;
                        break;
                    }
                }
            } else if (command[0] == 'G') {
                if (datagramDeadline == 0)
                    datagramDeadline = steadySeconds() + static_cast<double>(second) / 1000;
                if (steadySeconds() < datagramDeadline) {
                    std::vector<std::uint8_t> datagram(static_cast<std::size_t>(argument), 0x5A);
                    for (int burst = 0; burst < 64 && listener.pendingBytes(connection) < 64 * 1024; ++burst) {
                        std::memcpy(datagram.data(), &datagramsOffered, sizeof(datagramsOffered));
                        listener.sendDatagram(connection, datagram);
                        ++datagramsOffered;
                    }
                } else if (!didSendDatagramReport) {
                    didSendDatagramReport = true;
                    listener.sendStream(connection, { reinterpret_cast<const std::uint8_t*>(&datagramsOffered), sizeof(datagramsOffered) });
                }
            }
        }
        listener.flush();
    }
    writeReport(reportPipe, { .cpuSeconds = selfCPUSeconds(), .relayCPUSeconds = 0, .count = command.size() == 17 && command[0] == 'G' ? datagramsOffered : sent, .corrupt = corrupt });
    ::_exit(0);
}

/// Starts the QUIC server process and a client connected to it.
struct QUICPair {
    pid_t server = -1;
    int reportPipe = -1;
    std::uint16_t port = 0;
};

QUICPair forkQUICServer(const std::string& scratch, std::size_t backlogBytes)
{
    std::string directory = scratch + "/quic.XXXXXX";
    if (!::mkdtemp(directory.data()))
        return {};
    const std::string home = directory + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    int portPipe[2], reportPipe[2];
    if (::pipe(portPipe) != 0 || ::pipe(reportPipe) != 0)
        return {};
    // Forked before this process has started a thread.
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(portPipe[0]);
        ::close(reportPipe[0]);
        runQUICServer(portPipe[1], reportPipe[1], home, backlogBytes);
    }
    ::close(portPipe[1]);
    ::close(reportPipe[1]);
    QUICPair pair { .server = pid, .reportPipe = reportPipe[0] };
    if (::read(portPipe[0], &pair.port, sizeof(pair.port)) != sizeof(pair.port))
        pair.port = 0;
    ::close(portPipe[0]);
    return pair;
}

std::array<std::uint8_t, 17> rawCommand(char kind, std::uint64_t first, std::uint64_t second)
{
    std::array<std::uint8_t, 17> command {};
    command[0] = static_cast<std::uint8_t>(kind);
    std::memcpy(command.data() + 1, &first, sizeof(first));
    std::memcpy(command.data() + 9, &second, sizeof(second));
    return command;
}

/// Which way a raw bulk transfer runs.
enum class RawDirection : std::uint8_t {
    /// Listener to client: the way the host's screen state travels.
    Down,
    /// Client to listener: the way a paste or a file push travels.
    Up,
};

int runRawQUIC(const Arguments& arguments, std::uint64_t bytes, RawDirection direction)
{
    const bool isUpward = direction == RawDirection::Up;
    // How much the sender keeps queued on the listener: the daemon's own bound
    // is its per-client high-water, which this defaults to.
    const auto backlog = static_cast<std::size_t>(arguments.number("--server-backlog", 8u << 20));
    QUICPair pair = forkQUICServer(arguments.text("--scratch", "/tmp"), backlog);
    if (pair.port == 0)
        return 1;
    // Through a shaped link when one is asked for, so loss reaches QUIC's own
    // recovery and every resent byte is checked below.
    const Shape shape = parseShape(arguments);
    std::unique_ptr<QUICProxy> proxy;
    if (!shape.isClear()) {
        proxy = startQUICProxy(shape, pair.port);
        if (!proxy)
            return 1;
    }
    const std::uint16_t dialPort = proxy ? proxy->port : pair.port;
    std::atomic<std::uint64_t> received { 0 };
    std::atomic<std::uint64_t> corrupted { 0 };
    std::atomic<std::uint64_t> firstCorruptOffset { ~std::uint64_t { 0 } };
    std::atomic<std::uint64_t> acknowledged { 0 };
    std::atomic<bool> connected { false };
    std::atomic<bool> closed { false };
    std::atomic<double> doneAt { 0 };
    std::vector<std::uint8_t> progressBytes;
    std::mutex progressMutex;
    Transport::QUICClient client;
    Transport::QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&] { connected = true; };
    callbacks.onData = [&](std::span<const std::uint8_t> data) {
        if (isUpward) {
            std::lock_guard lock(progressMutex);
            progressBytes.insert(progressBytes.end(), data.begin(), data.end());
            while (progressBytes.size() >= 8) {
                std::uint64_t value = 0;
                std::memcpy(&value, progressBytes.data(), sizeof(value));
                progressBytes.erase(progressBytes.begin(), progressBytes.begin() + 8);
                acknowledged = value;
                if (value >= bytes && doneAt == 0)
                    doneAt = steadySeconds();
            }
            return;
        }
        // The listener sends a fixed pattern, so every byte's value is known
        // from its stream offset alone.
        const std::uint64_t offset = received.load();
        for (std::size_t index = 0; index < data.size(); ++index) {
            const std::uint64_t position = offset + index;
            if (data[index] != rawPatternByte(position)) {
                if (corrupted.fetch_add(1) == 0)
                    firstCorruptOffset = position;
            }
        }
        const std::uint64_t total = received.fetch_add(data.size()) + data.size();
        if (total >= bytes && doneAt == 0)
            doneAt = steadySeconds();
    };
    std::string closeReason;
    callbacks.onClose = [&](Transport::Health, const std::string& reason) {
        closeReason = reason;
        closed = true;
    };
    if (!client.start("127.0.0.1", dialPort, Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value())
        return 1;
    for (int attempt = 0; attempt < 500 && !connected; ++attempt)
        sleepMilliseconds(10);
    if (!connected)
        return 1;
    const double cpuBefore = selfCPUSeconds();
    const double start = steadySeconds();
    const auto command = rawCommand(isUpward ? 'U' : 'D', bytes, 0);
    client.send(command);
    // How far the sender runs ahead of what the listener has confirmed. The
    // listener's stream window is 4 MiB; the default stays under it, so the
    // lane measures the carrier rather than flow-control round trips.
    const auto upWindow = static_cast<std::uint64_t>(arguments.number("--up-window", 1u << 20));
    if (isUpward) {
        // A whole number of the pattern's periods, so every chunk starts in phase.
        std::vector<std::uint8_t> chunk(1 << 20);
        for (std::size_t index = 0; index < chunk.size(); ++index)
            chunk[index] = rawPatternByte(index);
        std::uint64_t sent = 0;
        while (sent < bytes && !closed) {
            if (sent > acknowledged.load() + upWindow) {
                sleepMilliseconds(1);
                continue;
            }
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - sent));
            client.send({ chunk.data(), take });
            sent += take;
        }
    }
    while (doneAt == 0 && !closed && steadySeconds() - start < 600)
        sleepMilliseconds(1);
    const double seconds = doneAt - start;
    const double cpu = selfCPUSeconds() - cpuBefore;
    Transport::TransportStats stats {};
    (void)client.stats(stats);
    const std::uint8_t end = 'E';
    client.send({ &end, 1 });
    sleepMilliseconds(50);
    client.stop();
    const ServerReport report = readReport(pair.reportPipe, 10);
    stopProcess(pair.server);
    const double gibibytes = static_cast<double>(bytes) / (1u << 30);
    Record("raw")
        .field("carrier", "quic")
        .field("direction", isUpward ? "up" : "down")
        .field("serverBacklogBytes", static_cast<std::uint64_t>(isUpward ? upWindow : backlog))
        .field("bytes", bytes)
        .flag("completed", doneAt > 0)
        .field("closeReason", closeReason)
        .field("link", shape.label())
        .field("corruptBytes", isUpward ? report.corrupt : corrupted.load())
        .field("firstCorruptOffset", corrupted.load() ? static_cast<double>(firstCorruptOffset.load()) : -1.0)
        .field("seconds", seconds)
        .field("megabytesPerSecond", static_cast<double>(bytes) / seconds / 1e6)
        .field("receiverCPUSecondsPerGiB", (isUpward ? report.cpuSeconds : cpu) / gibibytes)
        .field("senderCPUSecondsPerGiB", (isUpward ? cpu : report.cpuSeconds) / gibibytes)
        .field("clientCPUSecondsPerGiB", cpu / gibibytes)
        .field("serverCPUSecondsPerGiB", report.cpuSeconds / gibibytes)
        .field("packetsSent", stats.packetsSent)
        .field("packetsReceived", stats.packetsReceived)
        .field("packetsLost", stats.packetsLost)
        .field("congestionWindow", stats.congestionWindow)
        .field("smoothedRTTMicroseconds", static_cast<double>(stats.smoothedRttMicros))
        .emit();
    return doneAt > 0 ? 0 : 1;
}

int runRawDatagram(const Arguments& arguments)
{
    const auto size = static_cast<std::uint64_t>(arguments.number("--size", 1100));
    const double seconds = arguments.number("--seconds", 3);
    QUICPair pair = forkQUICServer(arguments.text("--scratch", "/tmp"), 0);
    if (pair.port == 0)
        return 1;
    std::atomic<std::uint64_t> delivered { 0 };
    std::atomic<std::uint64_t> offered { 0 };
    std::atomic<bool> connected { false };
    std::atomic<double> lastAt { 0 };
    Transport::QUICClient client;
    Transport::QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&] { connected = true; };
    callbacks.onDatagram = [&](std::span<const std::uint8_t>) {
        delivered.fetch_add(1, std::memory_order_relaxed);
        lastAt = steadySeconds();
    };
    callbacks.onData = [&](std::span<const std::uint8_t> data) {
        if (data.size() >= 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, data.data(), sizeof(value));
            offered = value;
        }
    };
    callbacks.onClose = [](Transport::Health, const std::string&) { };
    if (!client.start("127.0.0.1", pair.port, Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value())
        return 1;
    for (int attempt = 0; attempt < 500 && !connected; ++attempt)
        sleepMilliseconds(10);
    const double cpuBefore = selfCPUSeconds();
    const double start = steadySeconds();
    client.send(rawCommand('G', size, static_cast<std::uint64_t>(seconds * 1000)));
    while (offered == 0 && steadySeconds() - start < seconds + 10)
        sleepMilliseconds(5);
    sleepMilliseconds(200);
    const double cpu = selfCPUSeconds() - cpuBefore;
    const std::uint8_t end = 'E';
    client.send({ &end, 1 });
    sleepMilliseconds(50);
    client.stop();
    const ServerReport report = readReport(pair.reportPipe, 10);
    stopProcess(pair.server);
    Record("datagram")
        .field("carrier", "quic")
        .field("payloadBytes", size)
        .field("seconds", seconds)
        .field("offered", offered.load())
        .field("delivered", delivered.load())
        .field("deliveredPerSecond", static_cast<double>(delivered.load()) / seconds)
        .field("deliveredMegabytesPerSecond", static_cast<double>(delivered.load() * size) / seconds / 1e6)
        .field("deliveredFraction", offered ? static_cast<double>(delivered.load()) / static_cast<double>(offered.load()) : 0)
        .field("clientCPUSeconds", cpu)
        .field("serverCPUSeconds", report.cpuSeconds)
        .emit();
    return 0;
}

/// The host's own UDP loopback cost, with no QUIC in it: one process sends
/// 1,400-byte datagrams as fast as `sendto` returns, another reads them.
///
/// The control every QUIC number is read against. On a machine running other
/// work — or a network extension in the packet path — a loopback `sendto` can
/// cost many times what it costs on a quiet one, which moves every QUIC lane by
/// the same factor and no stream lane at all.
int runUDPFloor(const Arguments& arguments)
{
    const int count = static_cast<int>(arguments.number("--packets", 50000));
    const int receiver = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const int bufferBytes = 4 << 20;
    ::setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    if (::bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return 1;
    const pid_t reader = ::fork();
    if (reader == 0) {
        std::array<std::uint8_t, 2048> buffer {};
        for (;;) {
            if (::recv(receiver, buffer.data(), buffer.size(), 0) < 0)
                ::_exit(0);
        }
    }
    ::close(receiver);
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    ::setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
    ::fcntl(sender, F_SETFL, ::fcntl(sender, F_GETFL) | O_NONBLOCK);
    std::array<std::uint8_t, 1400> packet {};
    int refused = 0;
    const double cpuBefore = selfCPUSeconds();
    const double start = steadySeconds();
    for (int index = 0; index < count; ++index) {
        if (::sendto(sender, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            ++refused;
    }
    const double seconds = steadySeconds() - start;
    const double cpu = selfCPUSeconds() - cpuBefore;
    ::close(sender);
    ::kill(reader, SIGKILL);
    ::waitpid(reader, nullptr, 0);
    Record("udp-floor")
        .field("packets", count)
        .field("microsecondsPerSend", seconds / count * 1e6)
        .field("cpuMicrosecondsPerSend", cpu / count * 1e6)
        .field("packetsPerSecond", count / seconds)
        .field("refused", refused)
        .emit();
    return 0;
}

/// The stream carriers: bytes from a peer into a Channel the product uses.
int runRawStream(const Arguments& arguments, std::string_view carrier, std::uint64_t bytes)
{
    const std::string scratch = arguments.text("--scratch", "/tmp");
    RawObserver observer;
    observer.m_expected = bytes;
    std::unique_ptr<Transport::Channel> channel;
    pid_t server = -1;
    int reportPipe = -1;
    const std::string stats = scratch + "/raw-source-" + std::to_string(::getpid()) + ".stats";
    ::unlink(stats.c_str());
    const std::string sourceCommand = "exec " + shellQuote(selfPath()) + " raw-source --bytes " + std::to_string(bytes) + " --stats " + shellQuote(stats);
    const Transport::Origin origin { Transport::OriginKind::Daemon, "127.0.0.1" };
    if (carrier == "unix") {
        const std::string path = scratch + "/raw-" + std::to_string(::getpid()) + ".sock";
        ::unlink(path.c_str());
        const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 1) != 0)
            return 1;
        int pipes[2];
        if (::pipe(pipes) != 0)
            return 1;
        server = ::fork();
        if (server == 0) {
            ::close(pipes[0]);
            const int peer = ::accept(listener, nullptr, nullptr);
            std::vector<std::uint8_t> chunk(256 * 1024, 0x42);
            std::uint64_t sent = 0;
            while (sent < bytes) {
                const ssize_t written = ::write(peer, chunk.data(), static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - sent)));
                if (written <= 0)
                    break;
                sent += static_cast<std::uint64_t>(written);
            }
            std::array<char, 1> wait {};
            (void)::read(peer, wait.data(), 1);
            writeReport(pipes[1], { .cpuSeconds = selfCPUSeconds(), .relayCPUSeconds = 0, .count = sent });
            ::_exit(0);
        }
        ::close(pipes[1]);
        ::close(listener);
        reportPipe = pipes[0];
        channel = std::make_unique<Transport::LocalSocketChannel>(path, origin);
    } else if (carrier == "pipe") {
        channel = std::make_unique<Transport::SubprocessChannel>(sourceCommand, origin);
    } else if (carrier == "ssh") {
        channel = std::make_unique<Transport::SubprocessChannel>("exec " + arguments.text("--ssh-prefix") + " " + shellQuote(sourceCommand), origin);
    } else {
        return 2;
    }
    const double cpuBefore = selfCPUSeconds();
    const double start = steadySeconds();
    if (!channel->start(&observer))
        return 1;
    while (observer.m_doneAt == 0 && !observer.m_closed && steadySeconds() - start < 600)
        sleepMilliseconds(1);
    const double seconds = observer.m_doneAt - start;
    const double cpu = selfCPUSeconds() - cpuBefore;
    // The peer's relay (ssh) runs beside this process; read it before it exits.
    const TreeCPU tree = sampleTree(-1, processName(::getpid()));
    channel->stop();
    ServerReport report;
    if (reportPipe >= 0) {
        report = readReport(reportPipe, 10);
        stopProcess(server);
    } else {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (FILE* file = std::fopen(stats.c_str(), "r")) {
                const int fields = std::fscanf(file, "%lf %lf", &report.cpuSeconds, &report.relayCPUSeconds);
                std::fclose(file);
                if (fields == 2)
                    break;
            }
            sleepMilliseconds(10);
        }
    }
    ::unlink(stats.c_str());
    const double gibibytes = static_cast<double>(bytes) / (1u << 30);
    const double clientRelay = carrier == "ssh" ? tree.client : 0;
    Record("raw")
        .field("carrier", carrier)
        .field("direction", "down")
        .field("bytes", bytes)
        .flag("completed", observer.m_doneAt > 0)
        .field("seconds", seconds)
        .field("megabytesPerSecond", static_cast<double>(bytes) / seconds / 1e6)
        .field("receiverCPUSecondsPerGiB", (cpu + clientRelay) / gibibytes)
        .field("senderCPUSecondsPerGiB", (report.cpuSeconds + report.relayCPUSeconds) / gibibytes)
        .field("clientCPUSecondsPerGiB", cpu / gibibytes)
        .field("clientRelayCPUSecondsPerGiB", clientRelay / gibibytes)
        .field("serverCPUSecondsPerGiB", report.cpuSeconds / gibibytes)
        .field("serverRelayCPUSecondsPerGiB", report.relayCPUSeconds / gibibytes)
        .emit();
    return observer.m_doneAt > 0 ? 0 : 1;
}

/// Writes N bytes to stdout, then its own CPU and its parent's (sshd's, when
/// it runs behind one) to a file.
int runRawSource(const Arguments& arguments)
{
    const auto bytes = static_cast<std::uint64_t>(arguments.number("--bytes", 0));
    const std::string stats = arguments.text("--stats");
    std::vector<std::uint8_t> chunk(256 * 1024, 0x42);
    std::uint64_t sent = 0;
    while (sent < bytes) {
        const ssize_t written = ::write(1, chunk.data(), static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - sent)));
        if (written <= 0)
            break;
        sent += static_cast<std::uint64_t>(written);
    }
    const pid_t parent = ::getppid();
    const std::string parentName = processName(parent);
    const double relay = parentName.starts_with("sshd") ? sampleProcess(parent).cpuSeconds : 0;
    if (FILE* file = std::fopen(stats.c_str(), "w")) {
        std::fprintf(file, "%.6f %.6f\n", selfCPUSeconds(), relay);
        std::fclose(file);
    }
    // Hold the pipe open until the reader has everything.
    std::array<char, 1> wait {};
    (void)::read(0, wait.data(), 1);
    return 0;
}

int runRaw(const Arguments& arguments)
{
    const std::string carrier = arguments.text("--carrier", "quic");
    const auto bytes = static_cast<std::uint64_t>(arguments.number("--bytes", 1u << 30));
    if (carrier == "quic")
        return runRawQUIC(arguments, bytes, arguments.text("--direction", "down") == "up" ? RawDirection::Up : RawDirection::Down);
    return runRawStream(arguments, carrier, bytes);
}

// ---------------------------------------------------------------------------
// Link shapers

/// What a shaped link does with a unit it loses.
enum class LossBehaviour : std::uint8_t {
    /// Drops it: a datagram, which the transport above recovers or not.
    Drop,
    /// Delays it and everything behind it by a round trip: a stream segment,
    /// which TCP's fast retransmit resends in about that.
    DelayOneRoundTrip,
};

/// Departure and arrival times for one direction of a shaped link.
class LinkModel {
public:
    explicit LinkModel(const Shape& shape, std::uint64_t seed)
        : m_shape(shape)
        , m_random(seed)
    {
    }

    /// When a unit of `bytes` sent now arrives, or a negative time when the
    /// link drops it.
    double admit(std::size_t bytes, double now, double queueLimitSeconds, LossBehaviour loss)
    {
        const bool shouldDropOnLoss = loss == LossBehaviour::Drop;
        double departure = now;
        if (m_shape.bitsPerSecond > 0) {
            departure = std::max(now, m_lastDeparture) + static_cast<double>(bytes) * 8 / m_shape.bitsPerSecond;
            if (queueLimitSeconds > 0 && departure - now > queueLimitSeconds)
                return -1;
            m_lastDeparture = departure;
        }
        double arrival = departure + m_shape.oneWayMilliseconds / 1000;
        if (m_shape.lossPercent > 0 && std::uniform_real_distribution<double>(0, 100)(m_random) < m_shape.lossPercent) {
            if (shouldDropOnLoss)
                return -1;
            arrival += 2 * m_shape.oneWayMilliseconds / 1000 + 0.001;
        }
        if (!shouldDropOnLoss)
            arrival = std::max(arrival, m_lastArrival);
        m_lastArrival = arrival;
        return arrival;
    }

private:
    Shape m_shape;
    std::mt19937_64 m_random;
    double m_lastDeparture = 0;
    double m_lastArrival = 0;
};

/// Relays stdin to a child's stdin and the child's stdout to stdout through a
/// shaped link, reading from a side only while its queue has room.
int runShapeStdio(const Arguments& arguments)
{
    const Shape shape = parseShape(arguments);
    const std::vector<std::string> command = arguments.tail();
    if (command.empty())
        return 2;
    int toChild[2], fromChild[2];
    if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0)
        return 1;
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, toChild[0], 0);
    ::posix_spawn_file_actions_adddup2(&actions, fromChild[1], 1);
    ::posix_spawn_file_actions_addclose(&actions, toChild[1]);
    ::posix_spawn_file_actions_addclose(&actions, fromChild[0]);
    const pid_t child = spawnProcess(command, {}, &actions);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(toChild[0]);
    ::close(fromChild[1]);
    if (child <= 0)
        return 1;

    struct Segment {
        double arrival;
        std::vector<std::uint8_t> bytes;
        std::size_t written = 0;
    };
    struct Direction {
        int from;
        int to;
        LinkModel model;
        std::deque<Segment> queue;
        std::size_t queued = 0;
        bool open = true;
    };
    // A bottleneck holds what the link carries in a round trip plus a router
    // buffer; without a rate the bound is a TCP window.
    const double roundTrip = 2 * shape.oneWayMilliseconds / 1000;
    const std::size_t limit = shape.bitsPerSecond > 0 ? static_cast<std::size_t>(shape.bitsPerSecond / 8 * (roundTrip + 0.05)) + 16384 : (4u << 20);
    std::array<Direction, 2> directions { Direction { .from = 0, .to = toChild[1], .model = LinkModel(shape, 1), .queue = {} }, Direction { .from = fromChild[0], .to = 1, .model = LinkModel(shape, 2), .queue = {} } };
    for (Direction& direction : directions)
        ::fcntl(direction.to, F_SETFL, ::fcntl(direction.to, F_GETFL) | O_NONBLOCK);
    std::vector<std::uint8_t> buffer(65536);
    for (;;) {
        const double now = steadySeconds();
        std::array<pollfd, 4> descriptors {};
        int count = 0;
        double wake = now + 1;
        for (Direction& direction : directions) {
            if (direction.open && direction.queued < limit)
                descriptors[count++] = { direction.from, POLLIN, 0 };
            if (!direction.queue.empty()) {
                if (direction.queue.front().arrival <= now)
                    descriptors[count++] = { direction.to, POLLOUT, 0 };
                else
                    wake = std::min(wake, direction.queue.front().arrival);
            }
        }
        if (!directions[1].open && directions[1].queue.empty())
            break;
        // Rounded up: a wait under a millisecond rounded down is a spin.
        const int timeout = std::max(0, static_cast<int>(std::ceil((wake - now) * 1000)));
        ::poll(descriptors.data(), static_cast<nfds_t>(count), timeout);
        const double after = steadySeconds();
        for (Direction& direction : directions) {
            if (direction.open && direction.queued < limit) {
                pollfd probe { direction.from, POLLIN, 0 };
                if (::poll(&probe, 1, 0) > 0) {
                    const ssize_t read = ::read(direction.from, buffer.data(), buffer.size());
                    if (read <= 0) {
                        direction.open = false;
                    } else {
                        for (std::size_t offset = 0; offset < static_cast<std::size_t>(read); offset += 1448) {
                            const std::size_t length = std::min<std::size_t>(1448, static_cast<std::size_t>(read) - offset);
                            const double arrival = direction.model.admit(length, after, 0, LossBehaviour::DelayOneRoundTrip);
                            direction.queue.push_back({ arrival, { buffer.begin() + static_cast<std::ptrdiff_t>(offset), buffer.begin() + static_cast<std::ptrdiff_t>(offset + length) } });
                            direction.queued += length;
                        }
                    }
                }
            }
            while (!direction.queue.empty() && direction.queue.front().arrival <= after) {
                Segment& segment = direction.queue.front();
                const ssize_t written = ::write(direction.to, segment.bytes.data() + segment.written, segment.bytes.size() - segment.written);
                if (written <= 0)
                    break;
                segment.written += static_cast<std::size_t>(written);
                if (segment.written < segment.bytes.size())
                    break;
                direction.queued -= segment.bytes.size();
                direction.queue.pop_front();
            }
            if (!direction.open && direction.queue.empty() && direction.to != 1) {
                ::close(direction.to);
                direction.to = -1;
            }
        }
        if (directions[0].to < 0)
            directions[0].to = ::open("/dev/null", O_WRONLY);
    }
    stopProcess(child);
    return 0;
}

/// Forwards UDP between clients and one upstream port through a shaped link,
/// one upstream socket per client so the upstream sees distinct peers.
int runShapeUDP(const Arguments& arguments)
{
    const Shape shape = parseShape(arguments);
    const auto upstreamPort = static_cast<std::uint16_t>(arguments.number("--upstream-port", 0));
    const auto bindLoopback = [](std::uint16_t port) {
        const int descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        const int bufferBytes = 4 << 20;
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
        ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK);
        return descriptor;
    };
    const int front = bindLoopback(0);
    sockaddr_in bound {};
    socklen_t boundLength = sizeof(bound);
    ::getsockname(front, reinterpret_cast<sockaddr*>(&bound), &boundLength);
    std::printf("%u\n", ntohs(bound.sin_port));
    std::fflush(stdout);
    sockaddr_in upstream {};
    upstream.sin_family = AF_INET;
    upstream.sin_port = htons(upstreamPort);
    upstream.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct Packet {
        double arrival;
        int socket;
        sockaddr_in to;
        std::vector<std::uint8_t> bytes;
        bool operator>(const Packet& other) const { return arrival > other.arrival; }
    };
    struct Flow {
        sockaddr_in client;
        int socket;
    };
    std::vector<Flow> flows;
    LinkModel uplink(shape, 3), downlink(shape, 4);
    // A router buffer of 50 ms, past which the bottleneck drops.
    const double queueLimit = shape.bitsPerSecond > 0 ? 0.05 + 2 * shape.oneWayMilliseconds / 1000 : 0;
    std::vector<Packet> heap;
    std::array<std::uint8_t, 65536> buffer {};
    for (;;) {
        std::vector<pollfd> descriptors { { front, POLLIN, 0 } };
        for (const Flow& flow : flows)
            descriptors.push_back({ flow.socket, POLLIN, 0 });
        const double now = steadySeconds();
        int timeout = 1000;
        if (!heap.empty())
            timeout = std::max(0, static_cast<int>(std::ceil((heap.front().arrival - now) * 1000)));
        ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout);
        const double after = steadySeconds();
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (!(descriptors[index].revents & POLLIN))
                continue;
            for (;;) {
                sockaddr_in from {};
                socklen_t fromLength = sizeof(from);
                const ssize_t length = ::recvfrom(descriptors[index].fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
                if (length <= 0)
                    break;
                Packet packet;
                if (index == 0) {
                    auto flow = std::ranges::find_if(flows, [&](const Flow& candidate) { return candidate.client.sin_port == from.sin_port; });
                    if (flow == flows.end()) {
                        flows.push_back({ from, bindLoopback(0) });
                        flow = flows.end() - 1;
                    }
                    packet = { uplink.admit(static_cast<std::size_t>(length), after, queueLimit, LossBehaviour::Drop), flow->socket, upstream, {} };
                } else {
                    const Flow& flow = flows[index - 1];
                    packet = { downlink.admit(static_cast<std::size_t>(length), after, queueLimit, LossBehaviour::Drop), front, flow.client, {} };
                }
                if (packet.arrival < 0)
                    continue;
                packet.bytes.assign(buffer.begin(), buffer.begin() + length);
                heap.push_back(std::move(packet));
                std::ranges::push_heap(heap, std::greater<> {});
            }
        }
        while (!heap.empty() && heap.front().arrival <= steadySeconds()) {
            std::ranges::pop_heap(heap, std::greater<> {});
            const Packet& packet = heap.back();
            (void)::sendto(packet.socket, packet.bytes.data(), packet.bytes.size(), 0, reinterpret_cast<const sockaddr*>(&packet.to), sizeof(packet.to));
            heap.pop_back();
        }
    }
}

// ---------------------------------------------------------------------------
// The flood corpus: ASCII prose, SGR colour runs, CJK, emoji with variation
// selectors and ZWJ sequences, combining marks and box drawing, one kind per
// line in a fixed rotation so every run of it is the same bytes.

// ---------------------------------------------------------------------------
// interrupt: a pane floods; how long from the ^C a client sends until the
// flooding program has ended.

/// The flooding program, written beside the daemon's home: it writes its
/// process ID into `mark` and prints until a signal ends it. It installs no
/// handler, so it ends on SIGINT only if it started with the default a user's
/// program starts with. `queries` puts a primary device attributes request on
/// every line, so the terminal's replies fill the input of a program that
/// never reads them.
std::string writeInterruptFlooder(const std::string& scratch)
{
    const std::string path = scratch + "/interrupt-flood.pl";
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file)
        return {};
    std::fputs("my ($mark, $kind) = @ARGV;\n"
               "open(my $f, '>', \"$mark.tmp\"); print $f \"$$\\n\"; close $f; rename(\"$mark.tmp\", $mark);\n"
               "$| = 1;\n"
               "my $line = ($kind eq 'queries' ? \"\\e[c\" : '') . ('x' x 100) . \"\\n\";\n"
               "print $line while 1;\n",
        file);
    std::fclose(file);
    return path;
}

/// The process ID a mark file holds, or nothing until it is written.
std::optional<pid_t> readMark(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return std::nullopt;
    int processID = 0;
    const int read = std::fscanf(file, "%d", &processID);
    std::fclose(file);
    return read == 1 && processID > 0 ? std::optional<pid_t>(processID) : std::nullopt;
}

int runInterrupt(const Arguments& arguments)
{
    Bench bench { .options = parseCommon(arguments) };
    const int trials = static_cast<int>(arguments.number("--trials", 5));
    const double floodSeconds = arguments.number("--flood-seconds", 3);
    const double timeoutSeconds = arguments.number("--timeout-seconds", 10);
    const std::string kind = arguments.text("--kind", "text");
    // The client's main thread busy for this long after it sends the ^C, as a
    // frontend whose run loop is drawing a flood would be.
    const double busySeconds = arguments.number("--busy-seconds", 0);
    if (bench.options.hostBinary.empty()) {
        std::fprintf(stderr, "usage: interrupt --host BIN --carrier C [--kind text|queries] [--trials N]\n");
        return 2;
    }
    if (!bench.start())
        return 1;
    auto client = bench.attach(0);
    if (!client)
        return 1;
    bench.settle(*client, 0.3);
    const std::string flooder = writeInterruptFlooder(bench.options.scratch);
    std::vector<double> latencies;
    int timeouts = 0;
    for (int trial = 0; trial < trials; ++trial) {
        const std::string mark = bench.options.scratch + "/interrupt-mark-" + std::to_string(::getpid()) + "-" + std::to_string(trial);
        std::remove(mark.c_str());
        client->core->sendInput("perl " + shellQuote(flooder) + " " + shellQuote(mark) + " " + kind + "\r");
        std::optional<pid_t> flooding;
        (void)bench.dispatcher.runUntil([&] { return (flooding = readMark(mark)).has_value(); }, 10);
        bench.dispatcher.runFor(floodSeconds);
        const double sent = steadySeconds();
        client->core->sendInput("\x03");
        // Watched from a thread of its own: the dispatcher applies the flood's
        // screen updates on this thread and can be busy for half a second,
        // which a poll here would read as the program's own latency. Ended is
        // gone or a zombie — reaping is its shell's business.
        std::atomic<bool> didEnd { false };
        std::atomic<double> endedAt { -1 };
        std::thread watcher([&] {
            while (flooding && steadySeconds() - sent < timeoutSeconds) {
                struct proc_bsdinfo info {};
                const bool isGone = ::kill(*flooding, 0) != 0;
                const bool isZombie = !isGone && ::proc_pidinfo(*flooding, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_status == SZOMB;
                if (isGone || isZombie) {
                    endedAt.store(steadySeconds());
                    didEnd.store(true);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        if (busySeconds > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(busySeconds));
        while (!didEnd.load() && steadySeconds() - sent < timeoutSeconds)
            bench.dispatcher.runFor(0.01);
        watcher.join();
        if (didEnd.load()) {
            latencies.push_back((endedAt.load() - sent) * 1000);
        } else {
            ++timeouts;
            std::fprintf(stderr, "transport_bench: trial %d: no SIGINT within %.0f s of the ^C\n", trial, timeoutSeconds);
            // Ends the flood another way, so the next trial starts at a prompt.
            client->core->sendInput("\x03");
            bench.dispatcher.runFor(1.0);
            if (flooding)
                ::kill(*flooding, SIGKILL);
        }
        bench.settle(*client, 0.5);
        client->core->sendInput("clear\r");
        bench.settle(*client, 0.5);
    }
    const std::vector<double> points = percentiles(latencies, { 0.5, 1.0 });
    Record record("interrupt");
    addShape(record, bench.options)
        .field("flood", kind)
        .field("busySeconds", busySeconds)
        .field("trials", trials)
        .field("timeouts", timeouts)
        .field("p50Milliseconds", points[0])
        .field("maxMilliseconds", points[1])
        .emit();
    return timeouts == 0 ? 0 : 1;
}

int runGenerateFlood(const Arguments& arguments)
{
    const auto bytes = static_cast<std::uint64_t>(arguments.number("--bytes", 100e6));
    FILE* file = std::fopen(arguments.text("--out").c_str(), "wb");
    if (!file)
        return 1;
    static constexpr std::array<std::string_view, 8> words { "transport", "latency", "terminal", "checkpoint", "datagram", "coalesce", "mirror", "pane" };
    static constexpr std::array<std::string_view, 6> unicode { "日本語のテキスト", "中文字符测试", "한국어 문장", "😀👍🏽👩‍💻❤️", "éàöñ", "┌──┬──┐│ab│cd│└──┴──┘" };
    std::mt19937_64 random(7);
    std::uint64_t written = 0;
    std::uint64_t line = 0;
    std::string text;
    while (written < bytes) {
        text.clear();
        switch (line % 4) {
        case 0:
            text += std::to_string(line) + " ";
            for (int index = 0; index < 12; ++index) {
                text += words[random() % words.size()];
                text += ' ';
            }
            break;
        case 1:
            for (int index = 0; index < 8; ++index) {
                text += "\x1b[38;5;" + std::to_string(random() % 256) + "m";
                text += words[random() % words.size()];
                text += index % 3 == 0 ? "\x1b[1m " : " ";
            }
            text += "\x1b[0m";
            break;
        case 2:
            for (int index = 0; index < 4; ++index) {
                text += unicode[random() % unicode.size()];
                text += ' ';
            }
            break;
        default:
            text += "\x1b[48;2;" + std::to_string(random() % 256) + ";40;60m" + std::string(words[random() % words.size()]) + "\x1b[49m ";
            text += unicode[random() % unicode.size()];
            text += " \x1b[4m";
            text += words[random() % words.size()];
            text += "\x1b[24m";
            break;
        }
        text += '\n';
        std::fwrite(text.data(), 1, text.size(), file);
        written += text.size();
        ++line;
    }
    std::fclose(file);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    ::signal(SIGPIPE, SIG_IGN);
    const Arguments arguments(argc, argv);
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "raw")
        return runRaw(arguments);
    if (mode == "datagram")
        return runRawDatagram(arguments);
    if (mode == "raw-source")
        return runRawSource(arguments);
    if (mode == "e2e")
        return runEndToEnd(arguments);
    if (mode == "echo")
        return runEcho(arguments);
    if (mode == "pane-echo")
        return runPaneEcho(arguments);
    if (mode == "memory")
        return runMemory(arguments);
    if (mode == "attach-client")
        return runAttachClient(arguments);
    if (mode == "shape-stdio")
        return runShapeStdio(arguments);
    if (mode == "shape-udp")
        return runShapeUDP(arguments);
    if (mode == "udp-floor")
        return runUDPFloor(arguments);
    if (mode == "gen-flood")
        return runGenerateFlood(arguments);
    if (mode == "interrupt")
        return runInterrupt(arguments);
    std::fprintf(stderr, "usage: %s raw|datagram|e2e|echo|memory|gen-flood ...\n", argv[0]);
    return 2;
}
