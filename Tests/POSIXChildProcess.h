// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cerrno>
#include <csignal>
#include <optional>
#include <sys/types.h>
#include <sys/wait.h>
#include <utility>

namespace UbiquittyTest {

/// Supplies the system calls used by an exclusively owned direct child.
struct POSIXChildCalls {
    /// Reads the current child-exit signal disposition.
    ///
    /// - Parameter disposition: Receives the disposition on success.
    /// - Returns: Zero on success, or minus one with `errno` set.
    int childDisposition(struct sigaction& disposition) const { return ::sigaction(SIGCHLD, nullptr, &disposition); }

    /// Waits only for the named child.
    ///
    /// - Parameters:
    ///   - process: The positive direct-child identity.
    ///   - status: Receives a status when the child is reaped.
    ///   - flags: The `waitpid` options.
    /// - Returns: The `waitpid` result, preserving its `errno` on failure.
    pid_t wait(pid_t process, int* status, int flags) const
    {
        if (process <= 1) {
            errno = ECHILD;
            return -1;
        }
        return ::waitpid(process, status, flags);
    }

    /// Sends a signal to one positive process identity, never a process group.
    ///
    /// - Parameters:
    ///   - process: The positive unreaped child identity.
    ///   - signal: The signal number.
    /// - Returns: Zero on success, or minus one with `errno` set.
    int sendSignal(pid_t process, int signal) const
    {
        if (process <= 1) {
            errno = EINVAL;
            return -1;
        }
        return ::kill(process, signal);
    }
};

/// Retains exclusive reaping authority for one direct fork child.
///
/// The caller keeps `SIGCHLD` at its default disposition without
/// `SA_NOCLDWAIT`, and permits no other reaper for this child. An unreaped
/// child pins its PID. Every signal first verifies that this exact child is
/// still unreaped; a discovery result or reported PID never supplies authority.
/// An observed loss of the required disposition permanently retires ownership.
/// The injected calls allow failure-path tests without creating a process.
template<typename Calls = POSIXChildCalls>
class OwnedChildProcess {
public:
    /// Creates an empty owner with the default call implementation.
    OwnedChildProcess() = default;

    /// Creates an empty owner using an injected call implementation.
    ///
    /// - Parameter calls: The calls retained for this owner's lifetime.
    explicit OwnedChildProcess(Calls calls)
        : m_calls(std::move(calls))
    {
    }

    /// Stops and reaps the child when ownership remains verifiable.
    ~OwnedChildProcess() { (void)stop(); }

    OwnedChildProcess(const OwnedChildProcess&) = delete;
    OwnedChildProcess& operator=(const OwnedChildProcess&) = delete;
    OwnedChildProcess(OwnedChildProcess&&) = delete;
    OwnedChildProcess& operator=(OwnedChildProcess&&) = delete;

    /// Checks whether the child disposition permits exclusive reaping.
    ///
    /// - Returns: Whether child exits retain a waitable identity.
    [[nodiscard]] bool canOwnChildren() const
    {
        struct sigaction disposition {};
        return m_calls.childDisposition(disposition) == 0 && disposition.sa_handler == SIG_DFL
            && !(disposition.sa_flags & SA_NOCLDWAIT);
    }

    /// Takes one successful direct fork result into an empty owner.
    ///
    /// The argument must come directly from this process's successful child
    /// creation, not from a descriptor query, a report, or process discovery.
    ///
    /// - Parameter process: The returned direct-child PID.
    /// - Returns: Whether the owner accepted that child.
    [[nodiscard]] bool adoptForkResult(pid_t process)
    {
        if (m_process > 1 || process <= 1 || !canOwnChildren())
            return false;
        m_process = process;
        return true;
    }

    /// Returns the retained child identity, or minus one without ownership.
    [[nodiscard]] pid_t processID() const { return m_process; }

    /// Sends a signal only after an exact-child nonblocking wait confirms ownership.
    ///
    /// - Parameter signal: The signal number.
    /// - Returns: Whether the signal was sent to the still-owned child.
    [[nodiscard]] bool signal(int signal)
    {
        if (m_process <= 1)
            return false;
        if (!canOwnChildren()) {
            m_process = -1;
            return false;
        }
        if (reap(WNOHANG) != 0)
            return false;
        return m_calls.sendSignal(m_process, signal) == 0;
    }

    /// Reaps only the retained child and retires its identity on exit or `ECHILD`.
    ///
    /// Interrupted waits retry. Other wait errors preserve the identity but
    /// confer no signaling authority. An unverifiable child disposition retires
    /// ownership permanently. A status is published only for a matching reap.
    ///
    /// - Parameters:
    ///   - flags: The `waitpid` options.
    ///   - status: Receives a matching child's exit status when non-null.
    /// - Returns: The exact `waitpid` result, or minus one with `ECHILD` when empty.
    [[nodiscard]] pid_t reap(int flags, int* status = nullptr)
    {
        if (m_process <= 1 || !canOwnChildren()) {
            m_process = -1;
            errno = ECHILD;
            return -1;
        }
        int childStatus = 0;
        pid_t result;
        do {
            result = m_calls.wait(m_process, &childStatus, flags);
        } while (result < 0 && errno == EINTR);
        if (result == m_process) {
            m_process = -1;
            if (status)
                *status = childStatus;
        } else if (result < 0 && errno == ECHILD)
            m_process = -1;
        return result;
    }

    /// Stops and reaps the exclusively owned child, leaving empty owners unchanged.
    ///
    /// A failed ownership check or signal is never escalated to a broader target.
    ///
    /// - Returns: Whether no child ownership remains after the attempt.
    [[nodiscard]] bool stop()
    {
        if (m_process <= 1)
            return true;
        if (!signal(SIGKILL))
            return m_process <= 1;
        (void)reap(0);
        return m_process <= 1;
    }

private:
    Calls m_calls;
    pid_t m_process = -1;
};

/// Reads one exact positive process report without granting cleanup ownership.
///
/// - Parameter read: A callback receiving the initialized PID address and its size.
/// - Returns: The observed PID for a complete report, or nothing for invalid or short input.
template<typename Read>
[[nodiscard]] std::optional<pid_t> readReportedProcessID(Read&& read)
{
    pid_t process = -1;
    if (read(&process, sizeof(process)) != static_cast<ssize_t>(sizeof(process)) || process <= 1)
        return std::nullopt;
    return process;
}

} // namespace UbiquittyTest
