// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The scopes a credential carries: what its bearer may ask a host to do. A
// wire declaration, and the type the method registry names a call's
// requirement in, so it stands apart from the frame catalog that uses it.

#include "Base/Bits/OptionSet.h"

#include <bit>
#include <cstdint>
#include <string>
#include <type_traits>

namespace Ubiquitty::Protocol::Sync {

/// What a credential lets its bearer ask of a host: the ceiling on a
/// token-authenticated client, within which the consent gate and the agent
/// policy still apply.
///
/// An operation outside the set is refused for insufficient scope. A client
/// without a token is admitted only as a local peer: the host's own binaries
/// with no ceiling, and, on a host whose token requirement is off, a same-user
/// agent held to the scopes an `all` token carries. The values are single
/// bits, kept in the token store as their decimal mask, and are never
/// renumbered.
enum class Scope : std::uint8_t {
    /// Attaches and receives the screen plane.
    Observe = 0x01,
    /// Sends input: keys, paste, and what else drives a pane.
    Drive = 0x02,
    /// Lists and ends sessions.
    Control = 0x04,
    /// Reads a region of a pane's text and the last command's output.
    RegionRead = 0x08,
    /// Creates a session.
    NewSession = 0x10,
    /// Reads a file on the host; off unless a token is granted it by name.
    FileRead = 0x20,
    /// Administers this host, including credentials and other clients.
    Manage = 0x40,
};

/// Reports whether a scope's value is one bit, as a set of them requires.
///
/// - Parameter flag: The scope.
/// - Returns: True when exactly one bit is set.
constexpr bool scopeIsSingleBit(Scope flag) { return std::popcount(static_cast<std::underlying_type_t<Scope>>(flag)) == 1; }
static_assert(scopeIsSingleBit(Scope::Observe) && scopeIsSingleBit(Scope::Drive) && scopeIsSingleBit(Scope::Control) && scopeIsSingleBit(Scope::RegionRead) && scopeIsSingleBit(Scope::NewSession) && scopeIsSingleBit(Scope::FileRead) && scopeIsSingleBit(Scope::Manage), "every Scope enumerator must be a single bit (power of two)");

/// The scopes one credential carries.
using ScopeSet = Bits::OptionSet<Scope>;

/// Joins two scopes into a set, so a pair reads without braces.
///
/// - Parameters:
///   - a: One scope.
///   - b: Another.
/// - Returns: The set holding both.
[[nodiscard]] constexpr ScopeSet operator|(Scope a, Scope b) { return ScopeSet { a, b }; }

/// Spells a scope set as its comma-separated words, lowest bit first.
///
/// - Parameter scope: The set; a bit no scope names is left out.
/// - Returns: The words, such as `observe,drive`, or `none` for the empty set.
inline std::string scopeMaskToString(ScopeSet scope)
{
    std::string text;
    const auto append = [&text](const char* name) {
        if (!text.empty())
            text.push_back(',');
        text += name;
    };
    if (scope.contains(Scope::Observe))
        append("observe");
    if (scope.contains(Scope::Drive))
        append("drive");
    if (scope.contains(Scope::Control))
        append("control");
    if (scope.contains(Scope::RegionRead))
        append("region");
    if (scope.contains(Scope::NewSession))
        append("new");
    if (scope.contains(Scope::FileRead))
        append("file");
    if (scope.contains(Scope::Manage))
        append("manage");
    return text.empty() ? std::string("none") : text;
}

} // namespace Ubiquitty::Protocol::Sync
