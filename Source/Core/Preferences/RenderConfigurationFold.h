// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The one derivation from preferences to what a renderer is configured with.
//
// Lives in Preferences rather than beside the struct: Rendering is a model
// layer and does not depend on the behaviour layer that parses a document, so
// the struct is Rendering's and the fold that fills it is this layer's.

#include "Core/Preferences/Preferences.h"
#include "Core/Rendering/RenderConfiguration.h"

#include <cstdint>

namespace Ubiquitty::Preferences {
// Declared, not included: Key.h belongs to the macOS shell's settings module,
// and a header in two Swift-facing modules defines its types twice.
enum class Key;
} // namespace Ubiquitty::Preferences

namespace Ubiquitty::Preferences::Effective {

/// Folds a configuration into everything a renderer takes.
///
/// Every field resolves through the same `Effective::` accessor the rest of
/// Core reads, so an unset key and a key set to its default fold to the same
/// value, byte for byte.
///
/// - Parameter values: The configuration to fold.
/// - Returns: The resolved configuration, lengths in points.
[[nodiscard]] Rendering::RenderConfiguration renderConfiguration(const Values& values);

/// The fold with one profile laid over the configuration first
/// (``overlaidWithProfile``), for a window opened under a profile.
///
/// - Parameters:
///   - values: The configuration to fold.
///   - profile: The resolved profile to lay over it.
/// - Returns: The resolved configuration, lengths in points.
[[nodiscard]] Rendering::RenderConfiguration renderConfiguration(const Values& values, const Profile& profile);

/// The fold over the live configuration, for a caller holding no `Values` —
/// the spelling a Swift renderer reads through interop.
///
/// - Returns: ``renderConfiguration`` of ``values()``.
[[nodiscard]] Rendering::RenderConfiguration liveRenderConfiguration();

/// Where a preference takes effect, which is what decides who must re-read it
/// when it moves.
enum class KeyReach : std::uint8_t {
    /// A field of ``Rendering::RenderConfiguration``.
    Renderer,
    /// State seeded onto each terminal by `Runtime::seedTerminalConfiguration`.
    Terminal,
    /// Read by the frontend at its own seam — window chrome, input, the
    /// system's appearance — or by a Core subsystem at use time.
    Frontend,
};

/// Where `key` takes effect.
///
/// Exhaustive with no default arm, so a key added to ``Key`` does not compile
/// until it is placed — and a key placed in `Renderer` without a field in the
/// fold fails the derivation test, which drives every such key from a
/// document.
///
/// - Parameter key: The preference.
/// - Returns: Its reach.
[[nodiscard]] KeyReach reachOf(Key key);

} // namespace Ubiquitty::Preferences::Effective
