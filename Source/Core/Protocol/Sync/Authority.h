// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// What a host lets an attachment be (Docs/sync.md §6.10). A client asks for
// a role in its Greet; the host answers with the role it grants, which is
// never more than the client's credential allows and never more than it
// asked for.

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Roles.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

/// The less privileged of two person roles.
///
/// The person roles are declared most privileged first, so the lesser is the
/// later enumerator. Neither argument may be the agent role, which is not a
/// rung on that ladder: it passes input gates a viewer does not and is
/// granted by what a client is, never by comparison.
///
/// - Parameters:
///   - first: One person role.
///   - second: The other.
/// - Returns: Whichever may do less.
[[nodiscard]] constexpr Role lesserPersonRole(Role first, Role second)
{
    return static_cast<Role>(std::max(std::to_underlying(first), std::to_underlying(second)));
}

/// Decides the role a host grants.
///
/// A program acting for a person is an agent whatever it asks to be, because
/// what it may do is what that person consented to and no more; so is a
/// client whose credential allows nothing else. A person is granted the less
/// privileged of what they asked for and what their credential allows. A
/// client that says it only watches, or a person who asks to be an agent, is
/// granted nothing above a viewer: the agent role is not a person's to ask
/// for, and asking for it earns the role that sends nothing.
///
/// - Parameters:
///   - requested: The role the client's Greet asked for.
///   - kind: What the client says it is.
///   - ceiling: The most the client's credential allows.
/// - Returns: The role the Welcome grants.
[[nodiscard]] constexpr Role grantRole(Role requested, ClientKind kind, Role ceiling)
{
    if (kind == ClientKind::Agent || ceiling == Role::Agent)
        return Role::Agent;
    Role asked = requested == Role::Agent ? Role::Viewer : requested;
    if (kind == ClientKind::Viewer)
        asked = lesserPersonRole(asked, Role::Viewer);
    return lesserPersonRole(asked, ceiling);
}

} // namespace Ubiquitty::Protocol::Sync
