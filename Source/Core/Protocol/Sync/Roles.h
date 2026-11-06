// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The role an attachment holds on a host (Docs/sync.md §6.10): what a
// handshake asks for and is granted, and what the method registry names when
// it says who may call an operation. A wire declaration: a role crosses as
// its enumerator's value in one byte.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

/// What an attachment may do, most privileged first among people.
enum class Role : std::uint8_t {
    /// The person the sessions belong to: drives, rearranges, administers.
    Owner,
    /// A person who may type and rearrange what they are attached to.
    Driver,
    /// A person who is shown a pane and sends it nothing.
    Viewer,
    /// A program acting for a person, within what that person consented to.
    Agent,
};

/// The last role, for a decoder's bound and a table's size.
inline constexpr Role lastRole = Role::Agent;

/// Reads a role from its wire byte.
///
/// - Parameter byte: The byte a handshake carried.
/// - Returns: The role, or nothing for a byte past the last role.
[[nodiscard]] constexpr std::optional<Role> roleFromWire(std::uint8_t byte)
{
    if (byte > std::to_underlying(lastRole))
        return std::nullopt;
    return static_cast<Role>(byte);
}

/// A role's word, as a description of an operation spells it.
///
/// - Parameter role: The role.
/// - Returns: `owner`, `driver`, `viewer` or `agent`.
[[nodiscard]] constexpr std::string_view roleName(Role role)
{
    switch (role) {
    case Role::Owner:
        return "owner";
    case Role::Driver:
        return "driver";
    case Role::Viewer:
        return "viewer";
    case Role::Agent:
        return "agent";
    }
    return "";
}

/// A set of roles. A role's wire value is an ordinal, so the set keeps one
/// bit per role rather than the role's own value.
class Roles {
public:
    constexpr Roles() = default;
    constexpr Roles(std::initializer_list<Role> roles)
    {
        for (const Role role : roles)
            m_bits |= bitOf(role);
    }

    /// Whether a role is in the set.
    ///
    /// - Parameter role: The role asked about.
    /// - Returns: True when the set holds it.
    [[nodiscard]] constexpr bool contains(Role role) const { return m_bits & bitOf(role); }

    /// Whether the set holds no role.
    ///
    /// - Returns: True for the empty set.
    [[nodiscard]] constexpr bool isEmpty() const { return !m_bits; }

    friend constexpr bool operator==(Roles, Roles) = default;

private:
    [[nodiscard]] static constexpr std::uint8_t bitOf(Role role) { return static_cast<std::uint8_t>(1u << std::to_underlying(role)); }

    std::uint8_t m_bits = 0;
};

/// Every role, in the order a description lists them.
inline constexpr std::array<Role, 4> everyRole { Role::Owner, Role::Driver, Role::Viewer, Role::Agent };

} // namespace Ubiquitty::Protocol::Sync
