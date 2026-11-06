// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// DetachedDaemons — ends the daemons a test started detached.
//
// `--ensure-daemon`, an unpinned `--quic-bootstrap`, the local bridge's ensure
// step and an upgrade's successor each start a daemon through a double fork, in
// a session of its own. It is no process's child: no process-group kill reaches
// it and nothing waits for it, so a test that starts one ends it on every path
// out, or the run finds it left behind.
//
// Such a daemon is found as a person's stop finds it, by the PID file beside its
// socket. A daemon holds its socket's startup lock for its whole life and writes
// the PID file after taking it, so a PID file is read only while that lock is
// held: one a killed daemon left names a number the system may since have given
// another process.

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/param.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Ubiquitty::HostTest {

/// Whether a process runs a binary named `ubiquitty-host`, by the name the
/// kernel keeps for it.
///
/// - Parameter process: The process to ask about.
/// - Returns: `false` for a process that is gone or runs anything else.
inline bool runsHostBinary(pid_t process)
{
    constexpr std::string_view hostName = "ubiquitty-host";
#if defined(__APPLE__)
    std::array<char, 2 * MAXCOMLEN + 1> name {};
    const int length = ::proc_name(process, name.data(), static_cast<std::uint32_t>(name.size()));
    return length > 0 && std::string_view(name.data(), static_cast<std::size_t>(length)) == hostName;
#else
    std::ifstream file("/proc/" + std::to_string(process) + "/comm");
    std::string name;
    return std::getline(file, name) && name == hostName;
#endif
}

/// Whether some process holds the startup lock at a path.
///
/// - Parameter path: A daemon's `<socket>.lock`.
/// - Returns: `true` while a daemon holds it.
inline bool isStartupLockHeld(const std::string& path)
{
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    // A shared lock is refused only while the daemon's exclusive one stands,
    // and closing the descriptor lets go of one this call was granted.
    const bool isHeld = ::flock(descriptor, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(descriptor);
    return isHeld;
}

/// The process ID a PID file names, when it names one other than this process.
///
/// - Parameter path: A daemon's `<stem>.pid`.
/// - Returns: The process ID, or nothing for a file that is missing or holds no number.
inline std::optional<pid_t> processIDNamedIn(const std::string& path)
{
    std::ifstream file(path);
    const std::string text { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end == text.data() || value <= 1 || value == ::getpid())
        return std::nullopt;
    return static_cast<pid_t>(value);
}

/// Whether a process is this process's child, alive or not yet reaped.
///
/// The test that started a child waits for it, so it is left to that test:
/// asking does not reap it.
///
/// - Parameter process: The process to ask about.
/// - Returns: `true` for a child of this process, running or exited.
inline bool isChildOfThisProcess(pid_t process)
{
    siginfo_t state {};
    return ::waitid(P_PID, static_cast<id_t>(process), &state, WEXITED | WNOHANG | WNOWAIT) == 0;
}

/// The daemons serving a socket in a control directory that this process
/// cannot wait for.
///
/// Each holds its socket's startup lock, is named by the PID file beside that
/// socket, runs a `ubiquitty-host`, and is not this process's child.
///
/// - Parameter controlDirectory: The `UBIQUITTY_HOME` the daemons run under.
/// - Returns: Their process IDs, each once.
inline std::vector<pid_t> detachedDaemonsIn(const std::string& controlDirectory)
{
    std::vector<pid_t> daemons;
    DIR* directory = ::opendir(controlDirectory.c_str());
    if (!directory)
        return daemons;
    constexpr std::string_view lockSuffix = ".sock.lock";
    while (const struct dirent* entry = ::readdir(directory)) {
        const std::string_view name = entry->d_name;
        if (name.size() <= lockSuffix.size() || !name.ends_with(lockSuffix) || !isStartupLockHeld(controlDirectory + "/" + std::string(name)))
            continue;
        const std::string stem(name.substr(0, name.size() - lockSuffix.size()));
        const std::optional<pid_t> daemon = processIDNamedIn(controlDirectory + "/" + stem + ".pid");
        if (daemon && !isChildOfThisProcess(*daemon) && runsHostBinary(*daemon) && std::ranges::find(daemons, *daemon) == daemons.end())
            daemons.push_back(*daemon);
    }
    ::closedir(directory);
    return daemons;
}

/// Ends every daemon serving a socket in a control directory that this
/// process cannot wait for, and returns once each has gone.
///
/// SIGTERM first, so each stops as a person's stop ends it, writing its records
/// and removing its files; SIGKILL for one still there after `patience`. A
/// daemon that started while the others stopped is found by the next pass. A
/// direct child is left to the test that started it (``isChildOfThisProcess``).
///
/// - Parameters:
///   - controlDirectory: The `UBIQUITTY_HOME` the daemons run under.
///   - patience: How long a daemon has to stop when asked.
/// - Returns: Whether every daemon found has gone.
[[nodiscard]] inline bool endDetachedDaemons(const std::string& controlDirectory, std::chrono::milliseconds patience = std::chrono::seconds(5))
{
    // Gone, or its number names something other than a host now: the system
    // reaps a daemon that is no one's child as soon as it exits, and may give
    // its number to another process.
    const auto isEnded = [](pid_t daemon) { return (::kill(daemon, 0) != 0 && errno == ESRCH) || !runsHostBinary(daemon); };
    const auto waitUntilEnded = [&](const std::vector<pid_t>& daemons, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!std::ranges::all_of(daemons, isEnded) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return std::ranges::all_of(daemons, isEnded);
    };
    constexpr int maxPasses = 3;
    for (int pass = 0; pass < maxPasses; ++pass) {
        const std::vector<pid_t> daemons = detachedDaemonsIn(controlDirectory);
        if (daemons.empty())
            return true;
        for (const pid_t daemon : daemons)
            ::kill(daemon, SIGTERM);
        if (waitUntilEnded(daemons, patience))
            continue;
        for (const pid_t daemon : daemons) {
            if (!isEnded(daemon))
                ::kill(daemon, SIGKILL);
        }
        if (!waitUntilEnded(daemons, std::chrono::seconds(2)))
            return false;
    }
    return detachedDaemonsIn(controlDirectory).empty();
}

} // namespace Ubiquitty::HostTest
