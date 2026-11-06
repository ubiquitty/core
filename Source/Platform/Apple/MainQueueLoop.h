// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The Apple frontends' session thread, the main queue, as the loop Core's
// posted work runs on.
//
// A token rides in the context word libdispatch hands back to the function it
// calls, so a post takes no allocation of the frontend's and no block. The
// word is never dereferenced.

#include "Core/Runtime/PostedWorkTable.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <dispatch/dispatch.h>

namespace Ubiquitty::Apple {

namespace Detail {

static_assert(sizeof(Runtime::PostedWorkToken) == sizeof(void*), "a token rides in libdispatch's context word");

/// The longest delay handed to libdispatch, so that the conversion to
/// nanoseconds is defined; no Core delay comes near it.
inline constexpr double maxMainQueueDelaySeconds = 365.0 * 24 * 60 * 60;

/// Redeems the token a context word carries, on the main queue.
///
/// - Parameter context: The token, as the context word.
inline void runMainQueueToken(void* context)
{
    // Puns the context word back into the token it was made from. libdispatch
    // hands each token back exactly once, so a refusal can only mean that this
    // image redeems from a table other than the one that admitted it, and the
    // work would never run.
    [[maybe_unused]] const auto outcome = Runtime::runPostedSessionWork(std::bit_cast<Runtime::PostedWorkToken>(context));
    assert(outcome == Runtime::PostedWorkOutcome::Ran);
}

/// Queues a token behind the work the main queue already holds.
///
/// - Parameter token: The token to redeem there.
inline void runOnMainQueueSoon(Runtime::PostedWorkToken token)
{
    // Puns the token into the context word, which libdispatch only carries.
    dispatch_async_f(dispatch_get_main_queue(), std::bit_cast<void*>(token), &runMainQueueToken);
}

/// Queues a token on the main queue once a delay has passed.
///
/// - Parameters:
///   - seconds: The delay; none, a negative one or NaN queues it at once.
///   - token: The token to redeem there.
inline void runOnMainQueueAfter(double seconds, Runtime::PostedWorkToken token)
{
    if (!(seconds > 0)) {
        runOnMainQueueSoon(token);
        return;
    }
    const auto nanoseconds = static_cast<std::int64_t>(std::min(seconds, maxMainQueueDelaySeconds) * static_cast<double>(NSEC_PER_SEC));
    // Puns the token into the context word, which libdispatch only carries.
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), dispatch_get_main_queue(), std::bit_cast<void*>(token), &runMainQueueToken);
}

} // namespace Detail

/// The main queue as the loop a session's posted work runs on.
///
/// - Returns: Entry points that queue each token on the main queue and redeem
///   it there.
[[nodiscard]] inline Runtime::PostedWorkLoop mainQueueLoop()
{
    return { .runSoon = &Detail::runOnMainQueueSoon, .runAfter = &Detail::runOnMainQueueAfter };
}

} // namespace Ubiquitty::Apple
