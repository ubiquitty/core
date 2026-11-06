// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Session/PaneLink.h"

#include <optional>
#include <string>

namespace Ubiquitty::Session {

enum class LinkIndicatorKind : std::uint8_t {
    None,
    Reconnecting,
    NoContact,
    InputNotDelivered,
    Behind,
    Ended,
};

struct LinkIndicator {
    LinkIndicatorKind kind = LinkIndicatorKind::None;
    std::uint64_t seconds = 0;
};

/// A lag warning is enabled only when its owner supplies a measured floor.
struct LinkIndicatorPolicy {
    std::optional<std::uint64_t> behindFloorMicroseconds;
};

/// Chooses one presentation without conflating loss and the two stall reasons.
[[nodiscard]] inline LinkIndicator linkIndicatorFor(const LinkState& state, std::uint64_t lagMicroseconds,
    std::uint64_t now, const LinkIndicatorPolicy& policy)
{
    if (state.phase == LinkPhase::Ended)
        return { .kind = LinkIndicatorKind::Ended };
    if (state.phase == LinkPhase::Connecting || state.phase == LinkPhase::Lost)
        return { .kind = LinkIndicatorKind::Reconnecting };
    if (state.transport.isSilent) {
        const auto heard = state.transport.lastHeardMicroseconds;
        return { .kind = LinkIndicatorKind::NoContact,
            .seconds = heard && now >= *heard ? (now - *heard) / 1'000'000 : 0 };
    }
    if (state.transport.isInputUndelivered)
        return { .kind = LinkIndicatorKind::InputNotDelivered,
            .seconds = state.transport.oldestUnreceiptedAgeMicroseconds.value_or(0) / 1'000'000 };
    if (policy.behindFloorMicroseconds && lagMicroseconds > *policy.behindFloorMicroseconds)
        return { .kind = LinkIndicatorKind::Behind, .seconds = lagMicroseconds / 1'000'000 };
    return {};
}

/// Formats Core's status; transport names remain the native shell's fact.
[[nodiscard]] inline std::string linkIndicatorText(const LinkIndicator& indicator)
{
    switch (indicator.kind) {
    case LinkIndicatorKind::None:
        return {};
    case LinkIndicatorKind::Reconnecting:
        return "Reconnecting\xe2\x80\xa6";
    case LinkIndicatorKind::NoContact:
        return indicator.seconds ? "No contact for " + std::to_string(indicator.seconds) + " s" : "No contact";
    case LinkIndicatorKind::InputNotDelivered:
        return "Input not delivered";
    case LinkIndicatorKind::Behind:
        return indicator.seconds ? "Picture behind by " + std::to_string(indicator.seconds) + " s" : "Picture behind";
    case LinkIndicatorKind::Ended:
        return "Session ended";
    }
    return {};
}

} // namespace Ubiquitty::Session
