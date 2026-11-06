// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Host/RemoteLinkHarness.h — a real daemon and the client stack the
// native apps attach through, for the stanzas that drive a remote pane end to
// end: a forked `ubiquitty-host --daemon` in a private UBIQUITTY_HOME, a
// RemoteSessionCore over `ubiquitty-host --attach`, a dispatcher whose timers
// a test can hold, a clock it can stop, and a carrier that keeps each write the
// client makes.
//

#include "Core/CellGrid.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/Transport/Channel.h"
#include "Core/Transport/SSHHost.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Ubiquitty::HostTest {

/// Reads the steady clock.
///
/// - Returns: Microseconds since an arbitrary epoch.
inline std::uint64_t steadyMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// A clock a test can stop. It reads the steady clock until it is frozen,
/// then the moment it froze at, and the steady clock again once thawed, so it
/// never runs backwards.
class StoppableClock {
public:
    /// Stops the clock where it is.
    void freeze() { m_frozenAt.store(steadyMicroseconds()); }

    /// Lets the clock run again.
    void thaw() { m_frozenAt.store(0); }

    /// Reads the clock.
    ///
    /// - Returns: Microseconds on the steady clock's scale.
    [[nodiscard]] std::uint64_t now() const
    {
        const std::uint64_t frozenAt = m_frozenAt.load();
        return frozenAt ? frozenAt : steadyMicroseconds();
    }

private:
    std::atomic<std::uint64_t> m_frozenAt { 0 };
};

/// Whether a dispatcher runs the work it was given for later.
enum class Timers : std::uint8_t {
    Running,
    Held,
};

/// The session thread's dispatcher, on the test's thread: posted work runs
/// when pumped, in order, and delayed work once its time has come, unless the
/// test holds the timers. The carrier's I/O thread posts here.
class PumpedDispatcher final : public Session::Dispatcher {
public:
    /// Queues work for the next pump.
    ///
    /// - Parameter work: The work.
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work), Kind::Posted); }

    /// Queues work for the first pump once a delay has passed.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: The work.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000.0)), std::move(work), Kind::Delayed);
    }

    /// Holds delayed work, or lets it run again.
    ///
    /// - Parameter timers: Whether delayed work runs.
    void setTimers(Timers timers)
    {
        const std::lock_guard lock(m_mutex);
        m_timers = timers;
    }

    /// Runs every piece of work whose time has come, in the order it was
    /// posted, and what that work posts.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::lock_guard lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto due = std::ranges::find_if(m_queue, [&](const Queued& queued) {
                    return queued.due <= now && (queued.kind == Kind::Posted || m_timers == Timers::Running);
                });
                if (due == m_queue.end())
                    return;
                work = std::move(due->work);
                m_queue.erase(due);
            }
            std::move(work)();
        }
    }

private:
    /// Whether work runs at the next pump or once a delay has passed; delayed
    /// work is what a test may hold.
    enum class Kind : std::uint8_t {
        Posted,
        Delayed,
    };

    /// Work and the time it may run from.
    struct Queued {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
        Kind kind;
    };

    /// Queues work.
    ///
    /// - Parameters:
    ///   - due: When it may run.
    ///   - work: The work.
    ///   - kind: Whether it runs at the next pump or after a delay.
    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work, Kind kind)
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push_back({ .due = due, .work = std::move(work), .kind = kind });
    }

    std::mutex m_mutex;
    std::vector<Queued> m_queue;
    Timers m_timers = Timers::Running;
};

/// Every write a client made to its carrier, each kept whole.
class WriteLog {
public:
    /// Keeps one write.
    ///
    /// - Parameter bytes: The write.
    void record(std::span<const std::uint8_t> bytes)
    {
        const std::lock_guard lock(m_mutex);
        m_writes.emplace_back(bytes.begin(), bytes.end());
    }

    /// How many writes were made.
    [[nodiscard]] std::size_t size() const
    {
        const std::lock_guard lock(m_mutex);
        return m_writes.size();
    }

    /// One write.
    ///
    /// - Parameter index: Its place, from the first.
    /// - Returns: A copy of its bytes.
    [[nodiscard]] std::vector<std::uint8_t> at(std::size_t index) const
    {
        const std::lock_guard lock(m_mutex);
        return index < m_writes.size() ? m_writes[index] : std::vector<std::uint8_t> {};
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::vector<std::uint8_t>> m_writes;
};

/// A carrier that writes through another and keeps each write in a log, so a
/// test sees the bytes exactly as the client handed them over.
class RecordingChannel final : public Transport::Channel {
public:
    /// - Parameters:
    ///   - inner: The carrier that moves the bytes.
    ///   - log: Where each write is kept.
    RecordingChannel(std::unique_ptr<Transport::Channel> inner, std::shared_ptr<WriteLog> log)
        : m_inner(std::move(inner))
        , m_log(std::move(log))
    {
    }

    /// Starts the carrier underneath, which reports to the observer.
    ///
    /// - Parameter observer: The client.
    /// - Returns: Whether the carrier started.
    bool start(Transport::ChannelObserver* observer) override { return m_inner->start(observer); }

    /// Keeps the write, then hands it to the carrier underneath.
    ///
    /// - Parameter data: The bytes the client wrote.
    void send(std::span<const std::uint8_t> data) override
    {
        m_log->record(data);
        m_inner->send(data);
    }

    /// Hands a datagram to the carrier underneath.
    ///
    /// - Parameter datagram: One whole payload.
    /// - Returns: Whether the carrier took it.
    [[nodiscard]] bool sendDatagram(std::span<const std::uint8_t> datagram) override { return m_inner->sendDatagram(datagram); }
    /// Stops the carrier underneath.
    void stop() override { m_inner->stop(); }
    /// Stops the carrier underneath reading.
    void suspendReading() override { m_inner->suspendReading(); }
    /// Lets the carrier underneath read again.
    void resumeReading() override { m_inner->resumeReading(); }
    /// The bytes the carrier underneath has not sent.
    ///
    /// - Returns: The count, or -1 when it cannot say.
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override { return m_inner->pendingOutboundBytes(); }
    /// The most the carrier underneath queues.
    ///
    /// - Returns: The capacity, or -1 when it cannot say.
    [[nodiscard]] std::int64_t outboundCapacityBytes() const override { return m_inner->outboundCapacityBytes(); }
    /// Where the carrier underneath reaches.
    ///
    /// - Returns: Its origin.
    [[nodiscard]] Transport::Origin origin() const override { return m_inner->origin(); }
    /// The carrier underneath's link state.
    ///
    /// - Returns: Its health.
    [[nodiscard]] Transport::Health health() const override { return m_inner->health(); }
    /// The carrier underneath, described.
    ///
    /// - Returns: Its description.
    [[nodiscard]] std::string description() const override { return m_inner->description(); }

private:
    const std::unique_ptr<Transport::Channel> m_inner;
    const std::shared_ptr<WriteLog> m_log;
};

/// Builds the attach bridge's carrier, pinned to the fixture's daemon, and
/// keeps what the client writes on it when given a log.
class AttachChannelFactory final : public Session::ChannelFactory {
public:
    /// - Parameters:
    ///   - command: The shell command that runs `ubiquitty-host --attach`.
    ///   - socketName: The daemon socket the command names.
    ///   - log: Where each write is kept, or null for none.
    AttachChannelFactory(std::string command, std::string socketName, std::shared_ptr<WriteLog> log = nullptr)
        : m_command(std::move(command))
        , m_socketName(std::move(socketName))
        , m_log(std::move(log))
    {
    }

    /// Builds a carrier that runs the attach command.
    ///
    /// - Parameter done: Receives the carrier.
    void createChannel(Session::TransportMode, Done done) override
    {
        std::unique_ptr<Transport::Channel> channel = std::make_unique<Transport::SubprocessChannel>(m_command, Transport::Origin { Transport::OriginKind::Daemon, "" });
        if (m_log)
            channel = std::make_unique<RecordingChannel>(std::move(channel), m_log);
        done(std::move(channel), false);
    }

    /// Accepts only the fixture's own daemon socket.
    ///
    /// - Parameter socketName: The socket a receipt names.
    /// - Returns: Whether it is the fixture's.
    bool pinDaemonSocket(std::string_view socketName) override { return socketName == m_socketName; }

private:
    std::string m_command;
    std::string m_socketName;
    std::shared_ptr<WriteLog> m_log;
};

/// A daemon forked for one stanza in a home of its own, which is set as
/// this process's UBIQUITTY_HOME so the attach bridges it spawns find it.
/// The daemon is stopped and the home removed when this goes.
class ForkedDaemon {
public:
    /// Starts the daemon and waits, within a deadline, until it accepts.
    ///
    /// - Parameter hostBinary: The built `ubiquitty-host`.
    explicit ForkedDaemon(std::string hostBinary)
        : m_hostBinary(std::move(hostBinary))
    {
        std::string pattern = "/tmp/ubq-remote-link.XXXXXX";
        if (!::mkdtemp(pattern.data()))
            return;
        m_root = pattern;
        const std::string home = m_root + "/.ubiquitty";
        if (::mkdir(home.c_str(), 0700) != 0)
            return;
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        m_socketName = Host::socketLeafForBinary(m_hostBinary.c_str());
        m_pid = ::fork();
        if (m_pid < 0)
            return;
        if (m_pid == 0) {
            // A shell that reads no rc file of the person running the test, so
            // its prompt is the short default and its history stays out of
            // their home; a typed line then fits the 80-column row.
            ::setenv("HOME", m_root.c_str(), 1);
            ::setenv("HISTFILE", "/dev/null", 1);
            ::setenv("SHELL", "/bin/sh", 1);
            ::setenv("ENV", "/dev/null", 1);
            char* arguments[] = { m_hostBinary.data(), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"),
                const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
            ::execv(m_hostBinary.c_str(), arguments);
            ::_exit(127);
        }
        m_isReady = waitForDaemonReady(home + "/" + m_socketName);
    }

    ForkedDaemon(const ForkedDaemon&) = delete;
    ForkedDaemon& operator=(const ForkedDaemon&) = delete;

    /// Stops the daemon, waiting a bounded time before killing it, and
    /// removes its home.
    ~ForkedDaemon()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool isReaped = false;
            while (!isReaped && std::chrono::steady_clock::now() < deadline) {
                isReaped = ::waitpid(m_pid, nullptr, WNOHANG) == m_pid;
                if (!isReaped)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!isReaped) {
                ::kill(m_pid, SIGKILL);
                ::waitpid(m_pid, nullptr, 0);
            }
        }
        if (!m_root.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }
    }

    /// Whether the daemon started and accepts connections.
    [[nodiscard]] bool isReady() const { return m_isReady; }

    /// The daemon's socket, by the name a client pins.
    [[nodiscard]] const std::string& socketName() const { return m_socketName; }

    /// The shell command a client's carrier runs to reach this daemon.
    [[nodiscard]] std::string attachCommand() const { return Transport::shellSingleQuote(m_hostBinary) + " --attach --socket " + Transport::shellSingleQuote(m_socketName); }

private:
    std::string m_hostBinary;
    std::string m_root;
    std::string m_socketName;
    pid_t m_pid = -1;
    bool m_isReady = false;
};

/// Pumps the dispatcher until a condition holds or a deadline passes.
///
/// - Parameters:
///   - dispatcher: The session thread's dispatcher.
///   - timeout: How long to keep pumping.
///   - isDone: Whether the wait is over, asked after each pump.
/// - Returns: Whether the condition held before the deadline.
inline bool pumpUntil(PumpedDispatcher& dispatcher, std::chrono::milliseconds timeout, const std::function<bool()>& isDone)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (isDone())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    dispatcher.pump();
    return isDone();
}

/// The visible grid as one string, a soft-wrapped row running on into the
/// next, so text typed past the margin is found whole.
///
/// - Parameter terminal: The terminal.
/// - Returns: The text.
inline std::string gridText(const Terminal& terminal)
{
    std::string text;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        if (!terminal.screens.active->lineWrapped[row])
            text.push_back('\n');
    }
    return text;
}

} // namespace Ubiquitty::HostTest
