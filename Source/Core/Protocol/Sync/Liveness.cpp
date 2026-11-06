// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Liveness.h"

#include <algorithm>

namespace Ubiquitty::Protocol::Sync {

LinkState judgeLink(const LinkFacts& facts, std::uint64_t now)
{
    LinkState state;
    state.lastHeardMicroseconds = facts.lastHeardMicroseconds;
    if (facts.unreceiptedSinceMicroseconds) {
        const std::uint64_t age = now > *facts.unreceiptedSinceMicroseconds ? now - *facts.unreceiptedSinceMicroseconds : 0;
        state.oldestUnreceiptedAgeMicroseconds = age;
        state.isInputUndelivered = age >= facts.stallBoundMicroseconds;
    }
    // Silence is a fact only where the host should have spoken.
    if (facts.silenceBoundMicroseconds && facts.lastHeardMicroseconds)
        state.isSilent = now > *facts.lastHeardMicroseconds && now - *facts.lastHeardMicroseconds > *facts.silenceBoundMicroseconds;
    return state;
}

std::optional<std::uint64_t> nextStallMicroseconds(const LinkFacts& facts, std::uint64_t now)
{
    const LinkState state = judgeLink(facts, now);
    std::optional<std::uint64_t> next;
    if (facts.unreceiptedSinceMicroseconds && !state.isInputUndelivered)
        next = *facts.unreceiptedSinceMicroseconds + facts.stallBoundMicroseconds;
    // Silence shows strictly past its bound.
    if (facts.silenceBoundMicroseconds && facts.lastHeardMicroseconds && !state.isSilent) {
        const std::uint64_t silent = *facts.lastHeardMicroseconds + *facts.silenceBoundMicroseconds + 1;
        next = next ? std::min(*next, silent) : silent;
    }
    return next;
}

} // namespace Ubiquitty::Protocol::Sync
