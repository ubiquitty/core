// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The largest 2D texture edge a Metal GPU family allows.
//
// Metal states this per family rather than exposing a query, so the device has
// to be asked which family it is in, and that question is asked in Swift, by
// the renderer that holds the device. The NUMBERS live here,
// beside Core's atlas clamp that takes them as a parameter: a backend carrying
// its own 16384 keeps working until it runs somewhere the answer is 8192, and
// the failure is a texture that silently fails to create rather than anything
// naming a limit.
//
// The family mapping imports no framework and is exposed through the module
// map both Apple arms read.

namespace Ubiquitty::Apple {

/// The edge, in pixels, for a device's family class.
///
/// - Parameter isLargeTextureFamily: Whether the device is Apple 3 or above,
///   or any Mac family. Every Mac is; the iOS simulator reports Apple 2.
/// - Returns: 16384 for the large-texture families, 8192 below them.
[[nodiscard]] constexpr int maximumTexture2DDimension(bool isLargeTextureFamily)
{
    return isLargeTextureFamily ? 16384 : 8192;
}

} // namespace Ubiquitty::Apple
