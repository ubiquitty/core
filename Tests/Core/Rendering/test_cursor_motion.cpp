// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_cursor_motion.cpp — portable cursor motion-smoothing (Rendering/CursorMotion).
//
// Verifies the DISTANCE-based glide policy: Off never glides; Short glides a
// short move and snaps a long jump; Long snaps a short move and glides a long
// jump; Always glides both; Reduce-Motion / hidden snap in every mode. Plus the
// animation invariants the design hinges on: the first visible frame appears
// settled (no slide-in from origin); a glide ease converges monotonically to the
// target and then reports settled; the position never overshoots; a zero/negative
// dt holds; reset re-arms a clean appear. Pure C++, header-only, links standalone.

#include "Core/Rendering/CursorMotion.h"

#include <cstdio>

using Ubiquitty::Rendering::CursorMotion;
using Mode = Ubiquitty::Rendering::CursorMotionMode;

static bool approx(float a, float b, float tol = 1e-3f)
{
    float d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

// The short/long boundary the renderer feeds (cells). Moves at/under it are
// "short"; beyond it are "long" jumps.
static constexpr float threshold = 10.0f;

static CursorMotion::Frame frameAt(float col, float row, double t, Mode mode = Mode::Always)
{
    CursorMotion::Frame f;
    f.targetColumn = col;
    f.targetVRow = row;
    f.motionThresholdCells = threshold;
    f.seconds = t;
    f.durationSeconds = 0.08;
    f.mode = mode;
    f.reduceMotion = false;
    f.visible = true;
    return f;
}

// Drive a settled motion at origin, then a single move of `dist` cells (along
// the column) in the given mode, and report whether that move glided.
static bool movedAndGlided(Mode mode, float dist)
{
    CursorMotion m;
    m.update(frameAt(0, 0, 0.0, mode)); // settle at origin
    return m.update(frameAt(dist, 0, 0.016, mode));
}

TEST(cursorMotion)
{

    // ---- shouldGlide: the pure distance decision (no animation state) ----
    {
        const float th2 = threshold * threshold; // squared threshold
        // Off never glides, at any distance.
        CHECK(!CursorMotion::shouldGlide(Mode::Off, 1.0f, th2));
        CHECK(!CursorMotion::shouldGlide(Mode::Off, 1000.0f, th2));
        // Always glides, at any distance.
        CHECK(CursorMotion::shouldGlide(Mode::Always, 1.0f, th2));
        CHECK(CursorMotion::shouldGlide(Mode::Always, 1000.0f, th2));
        // Short: glide at/under the boundary, snap beyond.
        CHECK(CursorMotion::shouldGlide(Mode::Short, 4.0f * 4.0f, th2)); // 4 < 10
        CHECK(!CursorMotion::shouldGlide(Mode::Short, 40.0f * 40.0f, th2)); // 40 > 10
        // Long: snap at/under the boundary, glide beyond — the inverse of Short.
        CHECK(!CursorMotion::shouldGlide(Mode::Long, 4.0f * 4.0f, th2));
        CHECK(CursorMotion::shouldGlide(Mode::Long, 40.0f * 40.0f, th2));
        // A non-positive threshold drops the gate: Short → always, Long → never.
        CHECK(CursorMotion::shouldGlide(Mode::Short, 1000.0f, 0.0f));
        CHECK(!CursorMotion::shouldGlide(Mode::Long, 1000.0f, 0.0f));
    }

    // ---- The four modes through update(): short move vs. long jump ----
    // Short = 4 cells (< 10), Long = 40 cells (> 10).
    {
        // Off: never glides, either distance.
        CHECK(!movedAndGlided(Mode::Off, 4));
        CHECK(!movedAndGlided(Mode::Off, 40));
        // Short: glides the short move, snaps the long jump.
        CHECK(movedAndGlided(Mode::Short, 4));
        CHECK(!movedAndGlided(Mode::Short, 40));
        // Long: snaps the short move, glides the long jump.
        CHECK(!movedAndGlided(Mode::Long, 4));
        CHECK(movedAndGlided(Mode::Long, 40));
        // Always: glides both.
        CHECK(movedAndGlided(Mode::Always, 4));
        CHECK(movedAndGlided(Mode::Always, 40));
    }

    // ---- A snap pins exactly to the target and does not animate ----
    {
        // Short snapping a long jump lands at the target at once.
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Short));
        CHECK(!m.update(frameAt(40, 0, 0.016, Mode::Short)));
        CHECK(approx(m.column(), 40.0f) && approx(m.vrow(), 0.0f));
    }

    // ---- A Long glide eases all the way in, not just past the boundary ----
    // The glide/snap decision is latched at move onset from the MOVE distance,
    // so the shrinking remaining gap can't truncate an in-flight Long glide once
    // it falls back under the short/long boundary. Without the latch the final
    // ~boundary cells would snap.
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Long)); // settle at origin
        // 40 cells (> 10) -> Long glides. Drive it to convergence.
        CHECK(m.update(frameAt(40, 0, 0.016, Mode::Long)));
        double t = 0.016;
        int guard = 0;
        bool crossedBoundary = false; // saw the remaining gap drop under the boundary mid-glide
        while (m.animating() && guard < 1000) {
            t += 0.016;
            m.update(frameAt(40, 0, t, Mode::Long));
            const float remaining = 40.0f - m.column();
            if (remaining > 0.0f && remaining < threshold)
                crossedBoundary = true; // still animating with gap < boundary -> no truncation
            ++guard;
        }
        CHECK(guard < 1000);
        CHECK(!m.animating());
        CHECK(crossedBoundary); // proves it kept gliding past the boundary instead of snapping
        CHECK(approx(m.column(), 40.0f)); // eased all the way to the target
    }

    // ---- Re-deciding on a fresh mid-glide move uses the new move distance ----
    // A new long jump arriving while a Long glide is in flight re-latches from the
    // prior target to the new one, keeping the glide alive.
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Long));
        CHECK(m.update(frameAt(40, 0, 0.016, Mode::Long))); // gliding toward 40
        // New jump to 80 (another 40-cell move) mid-glide -> still glides.
        CHECK(m.update(frameAt(80, 0, 0.032, Mode::Long)));
        CHECK(m.column() < 80.0f); // not snapped to the new target
    }

    // ---- A diagonal jump just past the radius (euclidean) snaps in Short ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Short));
        // |(8,8)| = 11.3 > 10 -> a long jump -> Short snaps.
        CHECK(!m.update(frameAt(8, 8, 0.016, Mode::Short)));
        CHECK(approx(m.column(), 8.0f) && approx(m.vrow(), 8.0f));
    }

    // ---- Off: always pinned to the target, never animating ----
    {
        CursorMotion m;
        CursorMotion::Frame f = frameAt(5, 2, 0.0, Mode::Off);
        CHECK(!m.update(f));
        CHECK(approx(m.column(), 5.0f) && approx(m.vrow(), 2.0f));
        f = frameAt(20, 9, 0.016, Mode::Off);
        CHECK(!m.update(f)); // jump cells with the mode off -> still no slide
        CHECK(approx(m.column(), 20.0f) && approx(m.vrow(), 9.0f));
    }

    // ---- Reduce Motion: instant snap even when the mode would glide ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Always));
        CursorMotion::Frame f = frameAt(4, 1, 0.016, Mode::Always); // short -> would glide
        f.reduceMotion = true;
        CHECK(!m.update(f));
        CHECK(approx(m.column(), 4.0f) && approx(m.vrow(), 1.0f));
    }

    // ---- Hidden: snaps and does not animate (no slide on reappear) ----
    {
        CursorMotion m;
        CursorMotion::Frame f = frameAt(3, 3, 0.0, Mode::Always);
        f.visible = false;
        CHECK(!m.update(f));
        // Now becomes visible at a new cell -> should APPEAR settled, not slide.
        f = frameAt(10, 5, 0.016, Mode::Always);
        CHECK(!m.update(f));
        CHECK(approx(m.column(), 10.0f) && approx(m.vrow(), 5.0f));
    }

    // ---- First visible frame is settled on its target ----
    {
        CursorMotion m;
        CursorMotion::Frame f = frameAt(7, 4, 0.0, Mode::Always);
        CHECK(!m.update(f)); // appears settled
        CHECK(approx(m.column(), 7.0f) && approx(m.vrow(), 4.0f));
    }

    // ---- A short move glides and converges to the target ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Short)); // settle at origin
        CursorMotion::Frame f = frameAt(4, 0, 0.016, Mode::Short); // 4 < 10 -> a move
        CHECK(m.update(f)); // animating; a fresh glide holds at rest this frame
        CHECK(approx(m.column(), 0.0f)); // held at the rest cell (no idle-gap snap)
        m.update(frameAt(4, 0, 0.032, Mode::Short)); // ease begins on the next real frame
        CHECK(m.column() > 0.0f && m.column() < 4.0f); // mid-glide, strictly between
        double t = 0.032;
        int guard = 0;
        while (m.animating() && guard < 1000) {
            t += 0.016;
            m.update(frameAt(4, 0, t, Mode::Short));
            ++guard;
        }
        CHECK(guard < 1000);
        CHECK(!m.animating());
        CHECK(approx(m.column(), 4.0f)); // converged on the target
    }

    // ---- A glide ease converges monotonically and then settles ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Always)); // settle at origin

        // Move far away in Always (any distance glides); the next tick must START
        // animating with the rendered position strictly between origin and target.
        bool animating = m.update(frameAt(30, 0, 0.016, Mode::Always));
        CHECK(animating); // a fresh glide holds at rest this frame
        CHECK(approx(m.column(), 0.0f)); // held, not snapped
        m.update(frameAt(30, 0, 0.032, Mode::Always)); // ease begins on the next real frame
        CHECK(m.column() > 0.0f && m.column() < 30.0f);
        CHECK(approx(m.vrow(), 0.0f)); // same row -> no vertical motion

        // Step the clock in 16 ms ticks; the position increases monotonically
        // toward the target and never overshoots it.
        float prev = m.column();
        double t = 0.032;
        int guard = 0;
        while (m.animating() && guard < 1000) {
            t += 0.016;
            m.update(frameAt(30, 0, t, Mode::Always));
            CHECK(m.column() >= prev - 1e-3f); // monotonic non-decreasing
            CHECK(m.column() <= 30.0f + 1e-3f); // no overshoot
            prev = m.column();
            ++guard;
        }
        CHECK(guard < 1000); // it actually settled
        CHECK(!m.animating());
        CHECK(approx(m.column(), 30.0f));
        CHECK(t < 1.0); // an 80ms slide settles well under a second
    }

    // ---- A zero/negative dt holds position (no jump on a clock hiccup) ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 1.0, Mode::Always));
        m.update(frameAt(50, 0, 1.016, Mode::Always)); // start a slide
        float held = m.column();
        m.update(frameAt(50, 0, 1.016, Mode::Always)); // same timestamp -> dt == 0
        CHECK(approx(m.column(), held));
    }

    // ---- A glide that begins after a long idle gap still animates (no snap) ----
    // The renderer pauses while idle (no frames), so the first frame after a pause
    // carries a dt of seconds. Without the fresh-glide hold + step cap that huge dt
    // would ease ~fully in one frame and SNAP. A fresh glide holds at rest that
    // frame and animates from the next, however long the pause.
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Short)); // settle at origin
        // 5 cells (< 10) is a short move that should glide, but 9 s elapsed (the
        // user paused, then typed). It must not snap straight to the target.
        bool animating = m.update(frameAt(5, 0, 9.0, Mode::Short));
        CHECK(animating); // a glide, not a snap
        CHECK(m.column() < 5.0f - 1e-3f); // did not jump to the target in one frame
        double t = 9.0;
        int guard = 0;
        while (m.animating() && guard < 1000) {
            t += 0.016;
            m.update(frameAt(5, 0, t, Mode::Short));
            ++guard;
        }
        CHECK(guard < 1000);
        CHECK(!m.animating());
        CHECK(approx(m.column(), 5.0f)); // converges over real frames
    }

    // ---- reset re-arms a clean appear (no slide from the stale position) ----
    {
        CursorMotion m;
        m.update(frameAt(0, 0, 0.0, Mode::Always));
        m.update(frameAt(40, 0, 0.016, Mode::Always)); // mid-slide
        m.reset();
        CHECK(!m.update(frameAt(2, 2, 0.5, Mode::Always))); // first frame after reset = settled
        CHECK(approx(m.column(), 2.0f) && approx(m.vrow(), 2.0f));
    }
}

UBIQUITTY_TEST_MAIN()
