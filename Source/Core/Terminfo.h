// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Terminfo.h — static terminfo capability lookup for XTGETTCAP / XTQUERYTCAP.
//
// XTGETTCAP (xterm extension) is `DCS + q <hex-cap-name> ST`. Clients
// (neovim, tmux, ncurses) probe the terminal for terminfo strings at
// runtime instead of reading $TERMINFO from disk. The response is
// hit:  DCS 1 + r <hex-cap>=<hex-value> ST
// miss: DCS 0 + r ST
//
// The lookup table here is restricted to the caps neovim/tmux/ncurses
// actually probe at startup — about 40.
// Both short (2-letter) termcap names and long terminfo names are
// supported; queries hit on either.
//

#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::Terminfo {

/// The terminfo source text for the `xterm-ubiquitty` entry, aliases
/// `ubiquitty` and `ubiquitty-256color`: the `tic`-format description of what
/// the VT engine implements, NUL-terminated, several capability lines. The
/// installer compiles it and ``lookup`` answers XTGETTCAP from it.
///
/// - Returns: The source text, static for the life of the process.
const char* sourceText();

/// Looks up a terminfo capability by its short-cap or long-cap name.
///
/// The result views static storage and stays valid for the lifetime of the
/// process, so a caller may hold it across queries. A boolean capability hits
/// with an empty value, which the optional distinguishes from a miss — as a
/// length and a separate flag, both read as a length of 0, and a caller that
/// reads the length without the flag cannot tell them apart.
///
/// - Parameter capabilityName: The short (2-letter termcap) or long (terminfo) name.
/// - Returns: The capability value on a hit, or no value on a miss.
[[nodiscard]] std::optional<std::string_view> lookup(std::string_view capabilityName);

/// Whether a name is one the entry goes by: `xterm-ubiquitty` or an alias.
///
/// - Parameter terminalName: A terminal name, as `$TERM` would hold it.
/// - Returns: True when the entry's name list holds it.
[[nodiscard]] bool isEntryName(std::string_view terminalName);

/// The terminfo name a capability goes by.
///
/// - Parameter capabilityName: A short (2-letter termcap) or long (terminfo) name.
/// - Returns: The terminfo name the termcap name stands for, or the name itself
///   when it is not a termcap name the entry aliases. Views static storage or
///   the argument.
[[nodiscard]] std::string_view canonicalName(std::string_view capabilityName);

// The hex helpers are stream-safe: exactly two ASCII hex chars per byte, either
// case accepted on input, so a caller may split a payload on any byte boundary.

/// Decodes ASCII hex into bytes.
///
/// An odd number of characters is malformed rather than truncated — half a byte
/// has no value to write, and accepting it would let a split payload decode as
/// though it were whole.
///
/// - Parameters:
///   - hex: The ASCII hex characters, either case; an even count.
///   - output: Receives the decoded bytes; needs room for half of `hex.size()`.
/// - Returns: The number of bytes written, or -1 on malformed input.
[[nodiscard]] int hexDecode(std::string_view hex, std::span<char> output);

/// Encodes bytes as lower-case ASCII hex.
///
/// - Parameters:
///   - input: The bytes to encode. A view rather than a string: an embedded zero
///     is a byte like any other here, and the count is what bounds the read.
///   - output: Receives twice as many characters as `input` holds bytes.
/// - Returns: The number of characters written, or -1 when `output` is too small.
[[nodiscard]] int hexEncode(std::string_view input, std::span<char> output);

} // namespace Ubiquitty::Terminfo
