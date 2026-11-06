// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_framedriver — pins the idle<->active presentation-pacing policy the
// frame clock runs. Single-threaded transitions:
// the activity stamp, the idle->active edge return, the idle-timeout pause
// decision, and the pause/re-check race-catch the platform orchestrates.

#include "Core/Runtime/FrameDriver.h"

#include <cstdio>

using Ubiquitty::Runtime::AdaptiveDrive;
using Ubiquitty::Runtime::FrameDriver;

TEST(framedriver)
{

    const double timeout = 0.1; // uBRenderIdleTimeoutSecs

    // Fresh driver: paused, configured timeout.
    {
        FrameDriver d(timeout);
        CHECK(!d.active());
        CHECK(d.idleTimeoutSeconds() == timeout);
    }

    // Default ctor: paused, the terminal idle default.
    {
        FrameDriver d;
        CHECK(!d.active());
        CHECK(d.idleTimeoutSeconds() == FrameDriver::defaultIdleTimeoutSeconds);
        CHECK(d.idleTimeoutSeconds() == 0.1);
    }

    // A non-positive timeout clamps to 0 (any elapsed time is "expired").
    {
        FrameDriver d(-1.0);
        CHECK(d.idleTimeoutSeconds() == 0.0);
        d.noteActivity(10.0);
        CHECK(d.idleExpired(10.0) == true); // 0 >= 0
    }

    // noteActivity returns true on the idle->active edge (was paused), false when
    // already active — so the platform resumes the link exactly once.
    {
        FrameDriver d(timeout);
        CHECK(d.noteActivity(10.0) == true); // was idle -> resume edge
        d.setMode(Ubiquitty::Runtime::FrameMode::Continuous); // platform resumes the link
        CHECK(d.noteActivity(10.05) == false); // already active, no second resume
        CHECK(d.active());
    }

    // idleExpired: false strictly within the timeout, true at/after it.
    {
        FrameDriver d(timeout);
        d.noteActivity(10.0);
        d.setMode(Ubiquitty::Runtime::FrameMode::Continuous);
        CHECK(d.idleExpired(10.05) == false); // 0.05 < 0.1 -> still active
        // Avoid probing the exact 0.1 boundary: 10.10-10.0 is 0.0999... in IEEE
        // double, so use values far enough inside/outside the timeout (the macOS
        // code uses the same raw-double < comparison, never hitting it exactly).
        CHECK(d.idleExpired(10.11) == true); // 0.11 >= 0.1 -> pause
        CHECK(d.idleExpired(10.20) == true);
    }

    // Pause orchestration (maybePauseRenderTick): after the timeout the platform
    // flips paused + setMode(OnDemand); with no fresh activity the re-check stays
    // paused.
    {
        FrameDriver d(timeout);
        d.noteActivity(10.0);
        d.setMode(Ubiquitty::Runtime::FrameMode::Continuous);
        CHECK(d.idleExpired(10.2)); // end-of-frame: idle expired -> pause
        d.setMode(Ubiquitty::Runtime::FrameMode::OnDemand); // platform: paused = yes; setMode(OnDemand)
        CHECK(d.idleExpired(10.21) == true); // re-check: still no activity -> stays paused
        CHECK(!d.active());
    }

    // Race-catch: a drain stamps activity after the timeout test; the platform's
    // re-check sees the fresh stamp and resumes (no lost wake).
    {
        FrameDriver d(timeout);
        d.noteActivity(10.0);
        d.setMode(Ubiquitty::Runtime::FrameMode::Continuous);
        CHECK(d.idleExpired(10.2)); // would pause
        d.setMode(Ubiquitty::Runtime::FrameMode::OnDemand);
        d.noteActivity(10.2001); // a drain landed during the flip
        CHECK(d.idleExpired(10.2002) == false); // re-check sees fresh activity -> resume
    }

    // keepRenderingForReflow analogue: stamp + resume-if-idle, idempotent while
    // active.
    {
        FrameDriver d(timeout);
        CHECK(d.noteActivity(5.0) == true); // idle -> resume edge
        d.setMode(Ubiquitty::Runtime::FrameMode::Continuous);
        CHECK(d.noteActivity(5.01) == false); // active -> no second resume
    }
}

// test_framedriver_adaptive — pins the opt-in adaptive drive policy
// (setAdaptiveDrive / noteContentDrain / noteNetChange / recommendedMode).
// Deterministic timestamps, no threading. Interval 0.01: fast window 0.015,
// drain-stale window 0.03, net-change window 0.30.
TEST(framedriver_adaptive)
{
    using Ubiquitty::Runtime::FrameMode;

    // Off by default: Continuous whatever the stamps.
    {
        FrameDriver d;
        CHECK(!d.isAdaptiveDrive());
        CHECK(d.recommendedMode(100.0) == FrameMode::Continuous);
        d.setFrameIntervalSeconds(0.01);
        for (double t = 1.000; t < 1.030; t += 0.008)
            d.noteContentDrain(t);
        d.noteNetChange(1.024);
        CHECK(d.recommendedMode(1.024) == FrameMode::Continuous);
    }

    // The interval is the configured one; a non-positive set is ignored.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        CHECK(d.frameIntervalSeconds() == 0.01);
        d.setFrameIntervalSeconds(-1.0);
        CHECK(d.frameIntervalSeconds() == 0.01);
        d.setFrameIntervalSeconds(0.0);
        CHECK(d.frameIntervalSeconds() == 0.01);
    }

    // Interval not sourced (0): Continuous even under a flood cadence.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        CHECK(d.frameIntervalSeconds() == 0.0);
        for (double t = 1.000; t < 1.030; t += 0.008)
            d.noteContentDrain(t);
        d.noteNetChange(1.024);
        CHECK(d.recommendedMode(1.024) == FrameMode::Continuous);
    }

    // Fresh, or one lone drain: OnDemand.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        CHECK(d.recommendedMode(5.0) == FrameMode::OnDemand);
        d.noteContentDrain(5.0);
        d.noteNetChange(5.0);
        CHECK(d.recommendedMode(5.0) == FrameMode::OnDemand);
    }

    // Paced: drains 30 ms apart never build the streak.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        for (double t = 2.00; t < 2.10; t += 0.03) {
            d.noteContentDrain(t);
            d.noteNetChange(t);
        }
        CHECK(d.recommendedMode(2.09) == FrameMode::OnDemand);
    }

    // Flood: fast drains build the streak; Continuous after the third keeping-
    // pace drain, given a net change rendered within the window.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        d.noteNetChange(2.990);
        d.noteContentDrain(3.000);
        CHECK(d.recommendedMode(3.000) == FrameMode::OnDemand); // streak 0
        d.noteContentDrain(3.008);
        d.noteContentDrain(3.016);
        CHECK(d.recommendedMode(3.016) == FrameMode::OnDemand); // streak 2
        d.noteContentDrain(3.024);
        CHECK(d.recommendedMode(3.024) == FrameMode::Continuous); // streak 3
    }

    // Fast drains with no rendered net change (a mouse-tracking app redrawing
    // identically on motion): the streak fills, Continuous is still refused.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        for (double t = 3.500; t < 3.540; t += 0.008)
            d.noteContentDrain(t);
        CHECK(d.fastDrainStreak() == FrameDriver::adaptiveEscalateStreak);
        CHECK(d.recommendedMode(3.532) == FrameMode::OnDemand);
    }

    // A net change rendered tens of milliseconds before the escalating drain
    // still confirms it: the on-demand path renders once per delivery.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        d.noteNetChange(3.800); // 80 ms before, inside the 300 ms window
        for (double t = 3.856; t < 3.890; t += 0.008)
            d.noteContentDrain(t);
        CHECK(d.recommendedMode(3.880) == FrameMode::Continuous);
    }

    // Net change gone stale: past the window, Continuous lapses to OnDemand
    // however fast the drains keep coming.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        d.noteNetChange(4.000);
        double t = 4.000;
        for (; t < 4.400; t += 0.008)
            d.noteContentDrain(t);
        CHECK(d.recommendedMode(4.100) == FrameMode::Continuous); // 100 ms since: within 300
        CHECK(d.recommendedMode(4.392) == FrameMode::OnDemand); // 392 ms since: stale
    }

    // De-escalation on one slow drain: the streak resets at once.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        d.noteNetChange(4.500);
        for (double t = 4.500; t < 4.530; t += 0.008)
            d.noteContentDrain(t);
        CHECK(d.recommendedMode(4.524) == FrameMode::Continuous);
        d.noteContentDrain(4.574); // 50 ms gap
        CHECK(d.recommendedMode(4.574) == FrameMode::OnDemand);
    }

    // Drain staleness: with no drain for adaptiveStaleIntervalFactor frame-
    // intervals, OnDemand without a slower drain to reset the streak.
    {
        FrameDriver d;
        d.setAdaptiveDrive(AdaptiveDrive::On);
        d.setFrameIntervalSeconds(0.01);
        d.noteNetChange(5.000);
        for (double t = 5.000; t < 5.030; t += 0.008)
            d.noteContentDrain(t);
        CHECK(d.recommendedMode(5.024) == FrameMode::Continuous);
        CHECK(d.recommendedMode(5.044) == FrameMode::Continuous); // 20 ms since the last drain
        CHECK(d.recommendedMode(5.064) == FrameMode::OnDemand); // 40 ms since: stale
    }

    // Panel-rate awareness: one 60 Hz drain cadence reads paced against a 120 Hz
    // interval, flood against a 60 Hz interval.
    {
        FrameDriver onFast;
        onFast.setAdaptiveDrive(AdaptiveDrive::On);
        onFast.setFrameIntervalSeconds(1.0 / 120.0);
        onFast.noteNetChange(6.000);
        for (double t = 6.000; t < 6.060; t += 0.017)
            onFast.noteContentDrain(t);
        CHECK(onFast.recommendedMode(6.051) == FrameMode::OnDemand);

        FrameDriver onSlow;
        onSlow.setAdaptiveDrive(AdaptiveDrive::On);
        onSlow.setFrameIntervalSeconds(1.0 / 60.0);
        onSlow.noteNetChange(7.000);
        for (double t = 7.000; t < 7.060; t += 0.017)
            onSlow.noteContentDrain(t);
        CHECK(onSlow.recommendedMode(7.051) == FrameMode::Continuous);
    }
}

UBIQUITTY_TEST_MAIN()

TEST(framedriver_capacity_limited_adaptive)
{
    using Ubiquitty::Runtime::FrameMode;
    FrameDriver driver;
    driver.setAdaptiveDrive(AdaptiveDrive::On);
    driver.setFrameIntervalSeconds(1.0 / 120.0);
    driver.noteContentDrain(1.0, true);
    CHECK(driver.recommendedMode(1.0) == FrameMode::OnDemand);
    driver.noteNetChange(1.0);
    CHECK(driver.recommendedMode(1.04) == FrameMode::Continuous);
    driver.noteContentDrain(1.04, true);
    driver.noteNetChange(1.04);
    CHECK(driver.recommendedMode(1.08) == FrameMode::Continuous);
    CHECK(driver.recommendedMode(1.15) == FrameMode::OnDemand);
    driver.noteContentDrain(1.16);
    driver.noteNetChange(1.16);
    CHECK(driver.recommendedMode(1.16) == FrameMode::OnDemand);
}
