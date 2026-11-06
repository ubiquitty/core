// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <utility>

// Diagnostic output that cannot take the process down with it.
//
// std::print and std::println throw std::system_error when the write fails,
// and every binary here is built -fno-exceptions: the throw comes out of
// libc++'s own dylib, finds no handler anywhere, and the process aborts. A
// diagnostic line is the one thing in a program that must never do that. The
// daemon runs on other people's machines with its output wherever the
// supervisor pointed it, and the failures are ordinary ones — a full disk, a
// log pipe whose reader went away, a file-size limit — so the shape of the bug
// is that a host dies of writing about something else.
//
// Formatting is separated from writing for that reason. std::format builds a
// string and never touches a stream; a single fwrite then puts it out, and a
// short count is the answer to a failed write rather than an exception.
//
// One write per line, so a line from another thread cannot land inside this
// one. Nothing here locks: the underlying FILE* is what serializes, and a
// diagnostic path is not worth a lock of its own.

namespace Ubiquitty::PAL {

/// Writes one formatted line, newline included, and cannot throw.
///
/// A write that fails is dropped. There is nowhere to report the failure to —
/// the place a report would go is the stream that just failed — and the caller
/// is in the middle of saying something about a different problem.
///
/// - Parameters:
///   - destination: The stream to write to, typically `stderr`.
///   - format: The format string, checked at compile time.
///   - arguments: The values it names.
template<typename... Arguments>
void logLine(std::FILE* destination, std::format_string<Arguments...> format, Arguments&&... arguments)
{
    std::string line = std::format(format, std::forward<Arguments>(arguments)...);
    line.push_back('\n');
    (void)std::fwrite(line.data(), 1, line.size(), destination);
}

/// Writes formatted text with no trailing newline, and cannot throw.
///
/// For a line assembled from several calls — a hex dump, a progress line that
/// ends elsewhere. Each call is its own write, so a line built this way can
/// interleave with another thread's; ``logLine`` is the one that cannot.
///
/// - Parameters:
///   - destination: The stream to write to.
///   - format: The format string, checked at compile time.
///   - arguments: The values it names.
template<typename... Arguments>
void logText(std::FILE* destination, std::format_string<Arguments...> format, Arguments&&... arguments)
{
    const std::string text = std::format(format, std::forward<Arguments>(arguments)...);
    (void)std::fwrite(text.data(), 1, text.size(), destination);
}

} // namespace Ubiquitty::PAL
