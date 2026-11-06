// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#elif defined(__linux__)
#include <cstdio>
#endif

namespace Ubiquitty {
namespace Host {

/// Returns the start time out of the contents of a Linux `/proc/<pid>/stat`.
///
/// The field is the twenty-second, and the first twenty-one cannot be found by
/// counting spaces from the left: the second is the executable's name in
/// parentheses, the kernel does not escape it, and a process may name itself
/// `foo bar) 1 2 3`. Every reader that counts from the left can be handed an
/// arbitrary field this way. The kernel guarantees the name's closing
/// parenthesis is the LAST one in the line, so the scan starts there.
///
/// - Parameters:
///   - contents: The whole line as the kernel wrote it.
///   - out: Set to the start time in clock ticks when this returns true.
/// - Returns: Whether a start time was read.
inline bool parseLinuxProcessStartTime(std::string_view contents, std::uint64_t& out)
{
    const auto close = contents.rfind(')');
    if (close == std::string_view::npos)
        return false;
    // Field 3 (state) begins after the name; start time is field 22, so it is
    // the twentieth value from there.
    constexpr int fieldsAfterTheName = 20;
    std::string_view rest = contents.substr(close + 1);
    int seen = 0;
    std::size_t index = 0;
    while (index < rest.size()) {
        while (index < rest.size() && rest[index] == ' ')
            ++index;
        if (index >= rest.size())
            return false;
        const std::size_t start = index;
        while (index < rest.size() && rest[index] != ' ')
            ++index;
        if (++seen == fieldsAfterTheName) {
            std::uint64_t value = 0;
            for (std::size_t digit = start; digit < index; ++digit) {
                if (rest[digit] < '0' || rest[digit] > '9')
                    return false;
                value = value * 10 + static_cast<std::uint64_t>(rest[digit] - '0');
            }
            out = value;
            return true;
        }
    }
    return false;
}

/// Returns a value that distinguishes one use of a process id from the next.
///
/// A process id is a number the kernel reuses. Anything derived from one is
/// about whoever holds it at the moment of the call, so two calls a syscall
/// apart can be about two different programs. This is what makes the pair
/// comparable: read it either side of the work, and a change means the id was
/// reused underneath.
///
/// It does not make a process id trustworthy. The kernel stamps the id on a
/// socket at connect and the daemon first reads it at accept, so a reuse that
/// happened while the connection waited in the backlog is already complete by
/// the time anything here runs — both reads see the same substituted process
/// and agree.
///
/// - Parameters:
///   - processID: The process to ask about.
///   - out: Set to the instance value when this returns true.
/// - Returns: Whether the platform supplied one. False means no worse than not
///   asking: the caller keeps what it had rather than refusing it.
inline bool peerProcessInstanceOf(std::uint32_t processID, std::uint64_t& out)
{
    if (processID == 0)
        return false;
#if defined(__APPLE__)
    // The start time, as on Linux, rather than the kernel's non-recycled serial:
    // p_uniqueid is reachable only through a flavor the public SDK does not
    // declare. Two processes sharing an id and a microsecond would defeat this,
    // and they cannot — the first has to exit before the second is given the id.
    struct proc_bsdinfo info {};
    const int read = ::proc_pidinfo(static_cast<int>(processID), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (read != static_cast<int>(sizeof(info)))
        return false;
    out = info.pbi_start_tvsec * 1000000ULL + info.pbi_start_tvusec;
    return true;
#elif defined(__linux__)
    std::array<char, 64> path {};
    std::snprintf(path.data(), path.size(), "/proc/%u/stat", processID);
    std::FILE* file = std::fopen(path.data(), "r");
    if (!file)
        return false;
    std::array<char, 4096> line {};
    const std::size_t n = std::fread(line.data(), 1, line.size() - 1, file);
    std::fclose(file);
    if (n == 0)
        return false;
    return parseLinuxProcessStartTime(std::string_view(line.data(), n), out);
#else
    (void)processID;
    (void)out;
    return false;
#endif
}

} // namespace Host
} // namespace Ubiquitty
