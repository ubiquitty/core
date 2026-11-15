// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>

namespace Ubiquitty::Host {

/// Work requested by the daemon's process control signals.
enum class DaemonSignalEvent : unsigned {
    ReloadCredentials = 1u << 0,
    LogHoldState = 1u << 1,
    Stop = 1u << 2,
    StopIfIdle = 1u << 3,
};

/// Process control flags with a latched, nonblocking reactor wake.
///
/// A signal delivered before poll must remain observable by that poll. Flags
/// alone cannot provide that: the handler has already returned, so there is
/// no signal left to interrupt the wait. The pipe records the wake while the
/// atomic flags record the requested work.
class DaemonSignals {
public:
    /// Opens a nonblocking, close-on-exec wake pipe; isReady reports failure.
    DaemonSignals()
        : m_pipe(PAL::makeUnixPipe())
    {
        if (!m_pipe || !prepareDescriptor(m_pipe.read) || !prepareDescriptor(m_pipe.write))
            m_pipe = {};
    }

    /// The process's signal owner, initialized before its handlers are installed.
    /// - Returns: The owner whose pipe remains live while the daemon runs.
    [[nodiscard]] static DaemonSignals& singleton()
    {
        static DaemonSignals signals;
        return signals;
    }

    /// Reports whether both wake descriptors were prepared.
    /// - Returns: Whether the reactor can receive control wakes.
    [[nodiscard]] bool isReady() const { return m_pipe.isValid(); }

    /// The borrowed read descriptor to register in the reactor's poll set.
    /// - Returns: The descriptor, or -1 when initialization failed.
    [[nodiscard]] int fileDescriptor() const { return m_pipe.read.get(); }

    /// Records a signal without allocation, locks, blocking or changing errno.
    ///
    /// Call from a signal handler only after singleton initialization. A
    /// nonempty flag set already has a wake owed, so further signals coalesce.
    /// - Parameter signal: One of the daemon's five control signals.
    void post(int signal) noexcept
    {
        unsigned event = 0;
        switch (signal) {
        case SIGHUP:
            event = static_cast<unsigned>(DaemonSignalEvent::ReloadCredentials);
            break;
        case SIGUSR1:
            event = static_cast<unsigned>(DaemonSignalEvent::LogHoldState);
            break;
        case SIGTERM:
        case SIGINT:
            event = static_cast<unsigned>(DaemonSignalEvent::Stop);
            break;
        case SIGUSR2:
            event = static_cast<unsigned>(DaemonSignalEvent::StopIfIdle);
            break;
        default:
            return;
        }
        if (m_pending.fetch_or(event, std::memory_order_release) != 0)
            return;
        const int savedError = errno;
        const char byte = 1;
        ssize_t written;
        do {
            written = ::write(m_pipe.write.get(), &byte, sizeof(byte));
        } while (written < 0 && errno == EINTR);
        // EAGAIN leaves an existing wake readable; flags remain authoritative.
        errno = savedError;
    }

    /// Takes all work recorded before this exchange.
    /// - Returns: The pending DaemonSignalEvent bits, cleared atomically.
    [[nodiscard]] unsigned takePending() { return m_pending.exchange(0, std::memory_order_acq_rel); }

    /// Clears readable wake bytes on the reactor thread, preserving work flags.
    void drain()
    {
        std::array<char, 64> bytes;
        for (;;) {
            const auto count = ::read(m_pipe.read.get(), bytes.data(), bytes.size());
            if (count > 0 || (count < 0 && errno == EINTR))
                continue;
            break;
        }
    }

    /// Tests a returned flag set for one requested operation.
    /// - Parameters:
    ///   - pending: The result of takePending.
    ///   - event: The operation to inspect.
    /// - Returns: Whether the operation was requested.
    [[nodiscard]] static bool hasEvent(unsigned pending, DaemonSignalEvent event)
    {
        return (pending & static_cast<unsigned>(event)) != 0;
    }

private:
    /// Prepares one owned end of the pipe for the signal handler and reactor.
    /// - Parameter descriptor: The borrowed pipe end.
    /// - Returns: Whether both descriptor flags were installed.
    static bool prepareDescriptor(const PAL::UnixFileDescriptor& descriptor)
    {
        const int statusFlags = ::fcntl(descriptor.get(), F_GETFL, 0);
        const int descriptorFlags = ::fcntl(descriptor.get(), F_GETFD, 0);
        return statusFlags != -1 && descriptorFlags != -1
            && ::fcntl(descriptor.get(), F_SETFL, statusFlags | O_NONBLOCK) != -1
            && ::fcntl(descriptor.get(), F_SETFD, descriptorFlags | FD_CLOEXEC) != -1;
    }

    static_assert(std::atomic<unsigned>::is_always_lock_free);
    PAL::UnixPipe m_pipe;
    std::atomic<unsigned> m_pending { 0 };
};

} // namespace Ubiquitty::Host
