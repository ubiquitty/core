// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

//
// Source/Platform/POSIX/ProcStatFields.h — the fields the process-tree walk
// and the foreground-owner rule need out of /proc/<pid>/stat and status.
//
// Pure and header-only so the parse can be tested against a crafted line
// rather than against whatever processes happen to exist. Reading /proc is the
// caller's job; deciding where the fields are is this.
//

namespace Ubiquitty::PAL {

/// The fields a `/proc/<pid>/stat` line carries that the process walk reads.
struct ProcStatFields {
    std::uint32_t parentProcessID;
    std::uint32_t processGroupID;
    /// The kernel's one-character run state: `R`, `S`, `D`, `Z`, `T`, `I`, …
    /// Zero when the line held none, which no well-formed line does.
    char state;
};

/// Reads the parent and process-group ids out of a `/proc/<pid>/stat` line.
///
/// The line is `pid (comm) state ppid pgrp ...`, and `comm` is the hazard: it
/// is the executable's own name, so it may hold spaces and parentheses. A
/// process named `foo) 1 2 3` puts both inside the field every whitespace split
/// reads as the boundary, and such a parser answers plausible numbers from the
/// name rather than from the kernel — which is worse than failing, because the
/// walk would then treat a hostile name as a claim about ancestry.
///
/// The kernel guarantees only that `comm` is parenthesised, so the fields are
/// found from the LAST `)` in the line.
///
/// - Parameters:
///   - line: One line of `/proc/<pid>/stat`, NUL termination not required.
///   - out: Receives the two ids; untouched unless this returns true.
/// - Returns: Whether both ids were read.
[[nodiscard]] inline bool parseProcStatFields(std::string_view line, ProcStatFields& out)
{
    const std::size_t commEnd = line.rfind(')');
    if (commEnd == std::string_view::npos)
        return false;
    std::size_t index = commEnd + 1;

    const auto skipSpaces = [&] {
        while (index < line.size() && line[index] == ' ')
            ++index;
    };
    // " <state> " — one character. Read rather than skipped because it is the
    // zombie flag the inspector shows, and because it is the field a parse that
    // split on the first ')' would take out of the name.
    skipSpaces();
    if (index >= line.size())
        return false;
    const char state = line[index];
    ++index;

    // from_chars rather than a hand-rolled accumulate: it refuses on overflow
    // and on a non-digit rather than answering something, which is the whole
    // distinction this parse is built on — and it does not read past `last`, so
    // a line that is not NUL-terminated is safe input.
    const auto readNumber = [&](std::uint32_t& value) {
        skipSpaces();
        const auto* const first = line.data() + index;
        const auto* const last = line.data() + line.size();
        const auto [next, error] = std::from_chars(first, last, value);
        if (error != std::errc {})
            return false;
        index = static_cast<std::size_t>(next - line.data());
        return true;
    };

    ProcStatFields fields {};
    if (!readNumber(fields.parentProcessID) || !readNumber(fields.processGroupID))
        return false;
    fields.state = state;
    out = fields;
    return true;
}

/// The kernel's `PF_FORKNOEXEC`: set by fork, cleared by exec.
inline constexpr std::uint32_t processFlagForkedWithoutExec = 0x00000040;

/// The kernel's `PF_EXITING`: set as a process begins to exit, before it lets
/// go of its memory.
inline constexpr std::uint32_t processFlagExiting = 0x00000004;

/// Reads the kernel birth ticks (field 22) without trusting the process name.
///
/// - Parameter line: One complete `/proc/<pid>/stat` line.
/// - Returns: The exact unsigned field, or empty for truncation or overflow.
[[nodiscard]] inline std::optional<std::uint64_t> parseProcStatBirthTicks(std::string_view line)
{
    const std::size_t commEnd = line.rfind(')');
    if (commEnd == std::string_view::npos)
        return std::nullopt;
    line.remove_prefix(commEnd + 1);
    constexpr int fieldsBeforeBirth = 19;
    for (int field = 0; field <= fieldsBeforeBirth; ++field) {
        const std::size_t start = line.find_first_not_of(' ');
        if (start == std::string_view::npos)
            return std::nullopt;
        line.remove_prefix(start);
        const std::size_t end = std::min(line.find_first_of(" \n"), line.size());
        if (field == fieldsBeforeBirth) {
            std::uint64_t ticks = 0;
            const auto [next, error] = std::from_chars(line.data(), line.data() + end, ticks);
            if (error != std::errc {} || next != line.data() + end)
                return std::nullopt;
            return ticks;
        }
        line.remove_prefix(end);
    }
    return std::nullopt;
}

/// Reads the per-process kernel flags out of a `/proc/<pid>/stat` line.
///
/// The ninth field: after `comm` come state, ppid, pgrp, session, tty_nr,
/// tpgid, then flags. Found from the last `)` for the reason
/// ``parseProcStatFields`` gives.
///
/// - Parameter line: One line of `/proc/<pid>/stat`.
/// - Returns: The flags, or empty when the line does not reach them.
[[nodiscard]] inline std::optional<std::uint32_t> parseProcStatFlags(std::string_view line)
{
    const std::size_t commEnd = line.rfind(')');
    if (commEnd == std::string_view::npos)
        return std::nullopt;
    line.remove_prefix(commEnd + 1);
    constexpr int fieldsBeforeFlags = 6;
    for (int field = 0; field <= fieldsBeforeFlags; ++field) {
        const std::size_t start = line.find_first_not_of(' ');
        if (start == std::string_view::npos)
            return std::nullopt;
        line.remove_prefix(start);
        const std::size_t end = std::min(line.find(' '), line.size());
        if (field == fieldsBeforeFlags) {
            std::uint32_t flags = 0;
            const auto [next, error] = std::from_chars(line.data(), line.data() + end, flags);
            if (error != std::errc {} || next != line.data() + end)
                return std::nullopt;
            return flags;
        }
        line.remove_prefix(end);
    }
    return std::nullopt;
}

/// Whether SIGINT is ignored, from the text of `/proc/<pid>/status`.
///
/// The `SigIgn:` line is the ignored mask in hexadecimal, signal n at bit n-1.
///
/// - Parameter status: The file's text, or a prefix holding the line.
/// - Returns: Whether bit 1 is set, or empty when the line is absent or malformed.
[[nodiscard]] inline std::optional<bool> parseStatusIgnoresInterrupt(std::string_view status)
{
    constexpr std::string_view key = "\nSigIgn:";
    const std::size_t found = status.find(key);
    if (found == std::string_view::npos)
        return std::nullopt;
    std::string_view value = status.substr(found + key.size());
    const std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return std::nullopt;
    value.remove_prefix(start);
    std::uint64_t mask = 0;
    const auto [next, error] = std::from_chars(value.data(), value.data() + value.size(), mask, 16);
    if (error != std::errc {} || next == value.data())
        return std::nullopt;
    constexpr int interruptSignal = 2;
    return (mask & (std::uint64_t { 1 } << (interruptSignal - 1))) != 0;
}

/// How many threads a process has, from the text of `/proc/<pid>/status`.
///
/// A process whose first thread has ended reads `Z` in its state for as long
/// as another thread runs the program on; one that has ended keeps the one
/// thread until it is reaped.
///
/// - Parameter status: The file's text, or a prefix holding the line.
/// - Returns: The count, or empty when the line is absent or malformed.
[[nodiscard]] inline std::optional<std::uint32_t> parseStatusThreadCount(std::string_view status)
{
    constexpr std::string_view key = "\nThreads:";
    const std::size_t found = status.find(key);
    if (found == std::string_view::npos)
        return std::nullopt;
    std::string_view value = status.substr(found + key.size());
    const std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return std::nullopt;
    value.remove_prefix(start);
    std::uint32_t count = 0;
    const auto [next, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (error != std::errc {} || next == value.data())
        return std::nullopt;
    return count;
}

} // namespace Ubiquitty::PAL
