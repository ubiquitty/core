// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A session's LIVE LINK STATE as a frontend renders it.
//
// Distinct from Transport::Health (Core/Transport/Channel.h), which is
// per-Channel: RemoteSessionCore drives reconnect by creating a fresh Channel
// per attempt, so backoff is a property of the session and has no per-channel
// counterpart. Core names neither; both exist for a frontend to render.

#include <cstdint>

namespace Ubiquitty::Session {

/// A session's live link state — the dynamic axis, orthogonal to `Origin`.
///
/// Every transport reports the same enum, so a frontend renders one status
/// indicator rather than one per backend. A local session is always
/// `Connected`; a remote one moves Connecting to Connected to Reconnecting to
/// Failed. Additive, for the same reason `OriginKind` is.
// Pinned because Transport::Channel declares a `Health` spelling three of these
// names. They are different state machines — a session survives the channels
// under it — and both are std::uint8_t, so a cast between them is well-formed
// at every value. Connecting and Failed happen to agree; `Connected` is 0 here
// and 2 there, which is the one that matters because it is the steady state.
enum class Health : std::uint8_t {
    Connected = 0, // bytes flowing (the steady state; a local session is always this)
    Connecting = 1, // a remote transport is being established; awaiting the first frames
    Deploying = 2, // the host has no usable daemon binary; the client is pushing one
    Reconnecting = 3, // a remote transport dropped; backing off before the next attempt
    Failed = 4, // gave up / the session ended
};

} // namespace Ubiquitty::Session
