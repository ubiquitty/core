// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Poll.cpp — PAL/Poll.h under WebAssembly.
//
// A page is driven by the event loop it already runs on: readiness arrives as a
// callback, and blocking the main thread to wait for it deadlocks the very loop
// that would deliver it. So a wait is a failure here rather than a timeout, which
// keeps a caller that ignored ``localSocketsAvailable`` from spinning.

#include "PAL/Poll.h"

#include "PAL/Stream.h"

// std::to_underlying. Every other build gets <utility> transitively through a
// heavier header; this translation unit includes two and neither carries it.
#include <utility>

namespace Ubiquitty {
namespace PAL {

int waitForReadiness(std::span<Watch> watches, int)
{
    for (Watch& watch : watches)
        watch.result = Ready::None;
    return std::to_underlying(StreamStatus::Fatal);
}

std::unique_ptr<Waker> makeWaker() { return nullptr; }

} // namespace PAL
} // namespace Ubiquitty
