// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>

#if !__has_feature(objc_arc)
#error OwnedCallback requires ARC for closure capture ownership.
#endif

namespace Ubiquitty::Apple {

/// Retains a Swift closure in an independently owned C++ callback.
///
/// ARC copies and releases the captured block with the lambda. The callback
/// keeps its captures alive until the final C++ function owner releases it.
inline std::function<void()> ownedCallback(void (^callback)())
{
    return [callback] { callback(); };
}

} // namespace Ubiquitty::Apple
