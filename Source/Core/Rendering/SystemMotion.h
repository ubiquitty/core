// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// SystemMotion — the one duration/curve table every animated chrome surface reads
// from, plus the single Reduce-Motion translation layer.
//
// A role enum names each surface, one canonical MotionToken (duration + curve +
// whether it is spatial) lives per role, and resolveMotion() is the only place
// the accessibility setting is applied. Frontends map MotionCurve to their
// toolkit's timing function and animate for `durationSeconds`; they never restate
// a literal or branch on the system flag themselves.
//
// The spatial/opacity distinction exists because Reduce-Motion targets
// vestibular-triggering MOTION (position / scale / rotation), not opacity. A fade
// or tint change is preserved under Reduce-Motion while a slide or zoom snaps to
// instant.
//
// Pure C++, no platform types, header-only, host-testable.

namespace Ubiquitty::Rendering {

// The easing family a motion uses; a frontend maps it to its toolkit's timing
// function (SwiftUI `.easeOut`, a CAMediaTimingFunction, …). Linear doubles as
// the "no easing" curve an instant (Reduce-Motion-snapped) token carries.
enum class MotionCurve : std::uint8_t {
    Linear = 0,
    EaseOut, // decelerate — appears and short moves
    EaseInOut, // symmetric — larger position / scale changes
    Spring, // overshoot-and-settle — playful emphasis
};

// A named animated surface. Every animated chrome element resolves its timing
// from one of these roles instead of hardcoding a duration/curve.
enum class Motion : std::uint8_t {
    CursorGlide = 0, // the caret smear toward its new cell
    BellFlash, // the visual-bell flash
    ToastFade, // a notification / toast appearing or dismissing
    TabSwitch, // the fast tab-switch transition
    PresenceRing, // an agent-presence ring pulse
    PaletteAppear, // the command palette (Beam) summon + results settling
    FindPop, // the active find-match pop-zoom
};

// The canonical timing for a role: how long, on what curve, and whether it is
// SPATIAL (position / scale / rotation — snapped under Reduce-Motion) or a pure
// opacity / tint change (kept, because a fade is not motion).
struct MotionToken {
    double durationSeconds;
    MotionCurve curve;
    bool isSpatial;
};

// The single source of truth. Adopters read timing here rather than restating a
// literal at each call site; changing the house motion language is one edit.
[[nodiscard]] constexpr MotionToken motionToken(Motion role)
{
    switch (role) {
    case Motion::CursorGlide:
        return { 0.09, MotionCurve::EaseOut, true };
    case Motion::BellFlash:
        return { 0.15, MotionCurve::EaseOut, false }; // a flash is opacity, not motion
    case Motion::ToastFade:
        return { 0.20, MotionCurve::EaseInOut, false }; // fade in/out is opacity
    case Motion::TabSwitch:
        return { 0.18, MotionCurve::EaseInOut, true };
    case Motion::PresenceRing:
        return { 0.90, MotionCurve::EaseInOut, true }; // one pulse cycle
    case Motion::PaletteAppear:
        return { 0.10, MotionCurve::EaseOut, true }; // matches the shipped Beam 0.10 easeOut
    case Motion::FindPop:
        return { 0.18, MotionCurve::EaseOut, true };
    }
    return { 0.0, MotionCurve::Linear, true }; // unreachable — every case returns above
}

// the Reduce-Motion translation layer: the one place the accessibility setting is
// applied. A spatial motion snaps to instant (zero duration, linear curve); a
// pure opacity / tint change is preserved. Every animated surface calls this
// instead of branching on the system flag itself.
[[nodiscard]] constexpr MotionToken resolveMotion(Motion role, bool shouldReduceMotion)
{
    const MotionToken token = motionToken(role);
    if (shouldReduceMotion && token.isSpatial)
        return { 0.0, MotionCurve::Linear, true };
    return token;
}

// A resolved token with no duration has nothing to animate — the frontend applies
// the end state directly.
[[nodiscard]] constexpr bool isInstant(const MotionToken& token)
{
    return token.durationSeconds <= 0.0;
}

} // namespace Ubiquitty::Rendering
