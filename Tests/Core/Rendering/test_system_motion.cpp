// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_system_motion.cpp — pins the portable SystemMotion table
// (Source/Core/Rendering/SystemMotion.h): the per-role duration/curve tokens and the
// single Reduce-Motion translation.
//
// The table exists so that Reduce-Motion is honored in exactly one
// place (resolveMotion) and applies the vestibular rule correctly: spatial
// motion (slide / zoom / rotate) snaps to instant, but a pure opacity/tint change
// (a flash, a fade) is preserved. These checks pin that contract plus the shipped
// timing values so a silent drift shows here, not as an inconsistent or
// accessibility-violating animation on hardware.

#include "Core/Rendering/SystemMotion.h"

#include <cmath>
#include <cstdio>

using Ubiquitty::Rendering::isInstant;
using Ubiquitty::Rendering::Motion;
using Ubiquitty::Rendering::MotionCurve;
using Ubiquitty::Rendering::motionToken;
using Ubiquitty::Rendering::resolveMotion;

// Every role in the enum (the table must be total).
static constexpr Motion allRoles[] = {
    Motion::CursorGlide,
    Motion::BellFlash,
    Motion::ToastFade,
    Motion::TabSwitch,
    Motion::PresenceRing,
    Motion::PaletteAppear,
    Motion::FindPop,
};

// Every role has a real, positive duration — no accidental zero (which would read
// as "instant" and defeat the animation) outside of Reduce-Motion.
TEST(testEveryRoleHasPositiveDuration)
{
    for (Motion role : allRoles) {
        const auto token = motionToken(role);
        CHECK(token.durationSeconds > 0.0);
        CHECK(!isInstant(token));
    }
}

// With Reduce-Motion OFF, resolveMotion returns the table token verbatim.
TEST(testResolveIsIdentityWithoutReduceMotion)
{
    for (Motion role : allRoles) {
        const auto table = motionToken(role);
        const auto resolved = resolveMotion(role, /*shouldReduceMotion=*/false);
        CHECK(resolved.durationSeconds == table.durationSeconds);
        CHECK(resolved.curve == table.curve);
        CHECK(resolved.isSpatial == table.isSpatial);
    }
}

// The vestibular rule: under Reduce-Motion, SPATIAL roles snap to instant
// (zero duration, linear), while OPACITY roles (a flash / a fade) are preserved.
TEST(testReduceMotionSnapsSpatialKeepsOpacity)
{
    for (Motion role : allRoles) {
        const auto table = motionToken(role);
        const auto resolved = resolveMotion(role, /*shouldReduceMotion=*/true);
        if (table.isSpatial) {
            CHECK(isInstant(resolved));
            CHECK(resolved.durationSeconds == 0.0);
            CHECK(resolved.curve == MotionCurve::Linear);
        } else {
            // A fade is not motion — Reduce-Motion leaves it exactly as authored.
            CHECK(resolved.durationSeconds == table.durationSeconds);
            CHECK(resolved.curve == table.curve);
            CHECK(!isInstant(resolved));
        }
    }
}

// The two opacity roles are exactly the flash and the fade; everything else is
// spatial. Pins the classification a reviewer would eyeball.
TEST(testOpacityRolesAreFlashAndFade)
{
    CHECK(!motionToken(Motion::BellFlash).isSpatial);
    CHECK(!motionToken(Motion::ToastFade).isSpatial);
    CHECK(motionToken(Motion::CursorGlide).isSpatial);
    CHECK(motionToken(Motion::TabSwitch).isSpatial);
    CHECK(motionToken(Motion::PresenceRing).isSpatial);
    CHECK(motionToken(Motion::PaletteAppear).isSpatial);
    CHECK(motionToken(Motion::FindPop).isSpatial);
}

// PaletteAppear must match the value the shipped Beam animation already uses
// (0.10s easeOut) — the table reflects reality, so adoption is a no-op swap.
TEST(testPaletteAppearMatchesShippedValue)
{
    const auto token = motionToken(Motion::PaletteAppear);
    CHECK(std::fabs(token.durationSeconds - 0.10) < 1e-9);
    CHECK(token.curve == MotionCurve::EaseOut);
}

// resolveMotion is usable in a constant expression — the table is compile-time
// data, so a frontend can seed static timing without runtime cost.
TEST(testConstexprUsable)
{
    constexpr auto snapped = resolveMotion(Motion::TabSwitch, true);
    static_assert(snapped.durationSeconds == 0.0, "spatial role must snap under reduce-motion at compile time");
    static_assert(snapped.curve == MotionCurve::Linear, "snapped token is linear");
    constexpr auto kept = resolveMotion(Motion::BellFlash, true);
    static_assert(kept.durationSeconds > 0.0, "opacity role is preserved under reduce-motion");
    CHECK(true);
}

UBIQUITTY_TEST_MAIN()
