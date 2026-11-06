// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// CursorMotion — renderer-neutral cursor motion-smoothing state.
//
// Frontends that opt into a gliding caret feed each frame's target cell + the
// elapsed wall-clock time; this interpolates the rendered position toward the
// target over a short duration. Plain floats (no AppKit / Metal / simd), so
// every backend animates identically.
//
// Glide policy is DISTANCE-based, not input-timing based: the euclidean gap from
// the rendered cursor to its new target (old cell → new cell) decides glide vs.
// snap against motionThresholdCells. Distance is reliable everywhere, including
// remote/high-latency sessions where echo timing lies. shouldGlide() owns that
// decision so every frontend agrees.
//
// When the mode is off or the cursor is settled, update() is a couple of
// comparisons and animating() returns false, so the frontend takes its baked
// cursor path with zero added work.
//
// Reduce Motion: a true `reduceMotion` snaps instantly, without the caller branching.
//
// Pure C++, no exceptions / RTTI, host-testable.

namespace Ubiquitty::Rendering {

// Distance-based glide policy keyed on the move distance against
// motionThresholdCells (the short/long boundary). Off never glides. Short glides
// a short move and snaps a long jump (so a clear / history jump cuts, and typing
// far-right glides the step). Long glides a long jump and snaps a short move
// (typing stays instant, only big moves draw the eye). Always glides every move.
// Reduce Motion snaps in every mode (resolved in update(), not here).
//
// Preferences::CursorMotionMode aliases this; frontends consume the Core enum.
enum class CursorMotionMode : std::uint8_t {
    Off = 0,
    Short = 1,
    Long = 2,
    Always = 3,
};

// The tuning every backend feeds Frame unchanged, named here so a glide feels
// identical on each. A frontend restating either as its own literal is a
// second copy of this policy, and the two drift the first time one is tuned.
// Namespace-scope rather than members: Swift's importer drops a C++ class's
// static constexpr data, and these exist to be read from Swift.
inline constexpr float cursorMotionDefaultThresholdCells = 10.0f;
inline constexpr double cursorMotionDefaultDurationSeconds = 0.08;

class CursorMotion {
public:
    // Per-frame inputs. column/vrow are the target cell in visible-grid coords;
    // seconds is a monotonic wall-clock timestamp; durationSeconds is how long a
    // full slide takes (clamped to a tiny floor so 0 still snaps). mode +
    // gap-distance decide glide vs. snap (see shouldGlide); reduceMotion forces
    // an instant snap regardless of mode.
    struct Frame {
        float targetColumn = 0.0f;
        float targetVRow = 0.0f;
        // The short/long boundary in cells (euclidean), ~10. Short glides at or
        // under it and snaps beyond; Long is the inverse. 0 disables the
        // distance gate (any move that the mode glides, glides unbounded).
        float motionThresholdCells = 0.0f;
        double seconds = 0.0;
        double durationSeconds = 0.08;
        CursorMotionMode mode = CursorMotionMode::Off;
        bool reduceMotion = false;
        bool visible = false; // hidden cursor resets so it never slides on reappear
    };

    // The mode's glide decision (see the enum) over SQUARED euclidean cell
    // distances: the move's gap² and the boundary². A non-positive threshold
    // drops the distance gate (Short glides any move, Long none). Pure, so both
    // frontends and the tests share one rule.
    static bool shouldGlide(CursorMotionMode mode, float gapSquaredCells, float thresholdSquaredCells)
    {
        switch (mode) {
        case CursorMotionMode::Off:
            return false;
        case CursorMotionMode::Always:
            return true;
        case CursorMotionMode::Short:
            return thresholdSquaredCells <= 0.0f || gapSquaredCells <= thresholdSquaredCells;
        case CursorMotionMode::Long:
            return thresholdSquaredCells > 0.0f && gapSquaredCells > thresholdSquaredCells;
        }
        return false;
    }

    // Advance the animation to this frame. Returns true while a slide is in
    // flight (the rendered position differs from the target) — the only time the
    // frontend should draw the moving overlay instead of the settled cursor.
    bool update(const Frame& f)
    {
        // Off / hidden / Reduce-Motion: pin to the target and forget any motion.
        // An off or Reduce-Motion frame must leave the caller's existing path
        // untouched, so we report not-animating and keep the position exact.
        if (f.mode == CursorMotionMode::Off || f.reduceMotion || !f.visible) {
            m_column = f.targetColumn;
            m_vrow = f.targetVRow;
            m_gliding = false;
            m_animating = false;
            m_haveTarget = f.visible;
            m_targetColumn = f.targetColumn;
            m_targetVRow = f.targetVRow;
            m_lastSeconds = f.seconds;
            m_haveLastSeconds = true;
            return false;
        }

        double dt = m_haveLastSeconds ? (f.seconds - m_lastSeconds) : 0.0;
        m_lastSeconds = f.seconds;
        m_haveLastSeconds = true;

        // First-ever visible frame: appear settled on the target (don't slide in
        // from the origin).
        if (!m_haveTarget) {
            m_haveTarget = true;
            m_column = f.targetColumn;
            m_vrow = f.targetVRow;
            m_targetColumn = f.targetColumn;
            m_targetVRow = f.targetVRow;
            m_gliding = false;
            m_animating = false;
            return false;
        }

        // Decide glide vs. snap once per move, at the instant the target changes,
        // from the MOVE distance (previous target → new target). The decision is
        // latched for the move's duration so re-checking the SHRINKING gap each
        // frame can't truncate an in-flight glide (a Long glide must ease all the
        // way in, not snap its last cells).
        const bool targetMoved = (f.targetColumn != m_targetColumn) || (f.targetVRow != m_targetVRow);
        if (targetMoved) {
            const float moveC = f.targetColumn - m_targetColumn;
            const float moveR = f.targetVRow - m_targetVRow;
            const float moveSquared = moveC * moveC + moveR * moveR;
            const float thresholdSquared = f.motionThresholdCells * f.motionThresholdCells;
            m_gliding = shouldGlide(f.mode, moveSquared, thresholdSquared);
        }
        m_targetColumn = f.targetColumn;
        m_targetVRow = f.targetVRow;

        // Snap (this move doesn't glide, or the latch is clear): pin to the
        // target and stop.
        if (!m_gliding) {
            m_column = m_targetColumn;
            m_vrow = m_targetVRow;
            m_gliding = false;
            m_animating = false;
            return false;
        }

        // A glide beginning after the cursor sat idle (renderer paused, no frames
        // ticked) must not inherit the whole idle gap as its first ease step — a
        // huge dt eases ~fully in one frame (an instant snap). Hold at rest the
        // frame a fresh glide starts, then ease from real frames on; clamp any
        // other gap (a hitch) the same.
        if (targetMoved && !m_animating)
            dt = 0.0;
        else if (dt > maxStepSeconds)
            dt = maxStepSeconds;

        // Exponential ease toward the target: smooth, frame-rate independent, and
        // self-terminating (it never overshoots). The time constant derives from
        // the configured slide duration; a non-positive dt (first tick / clock
        // hiccup) holds position rather than jumping.
        const double duration = (f.durationSeconds > 0.001) ? f.durationSeconds : 0.001;
        // Reach ~98% of the gap over `duration`: alpha = 1 - e^(-k*dt), k chosen
        // so that at dt == duration the response is ~0.98.
        const double k = 4.0 / duration;
        double alpha = 1.0;
        if (dt > 0.0) {
            alpha = 1.0 - fastExpNeg(k * dt);
            if (alpha < 0.0)
                alpha = 0.0;
            if (alpha > 1.0)
                alpha = 1.0;
        } else {
            alpha = 0.0; // no time passed — hold
        }

        m_column += (m_targetColumn - m_column) * static_cast<float>(alpha);
        m_vrow += (m_targetVRow - m_vrow) * static_cast<float>(alpha);

        // Settle: once within a small fraction of a cell, snap exactly, clear the
        // glide latch, and stop so the renderer returns to its settled cursor path
        // (and idles).
        const float dc = m_targetColumn - m_column;
        const float dr = m_targetVRow - m_vrow;
        if (dc * dc + dr * dr < settleEpsilon) {
            m_column = m_targetColumn;
            m_vrow = m_targetVRow;
            m_gliding = false;
            m_animating = false;
            return false;
        }
        m_animating = true;
        return true;
    }

    [[nodiscard]] bool animating() const { return m_animating; }
    [[nodiscard]] float column() const { return m_column; }
    [[nodiscard]] float vrow() const { return m_vrow; }

    void reset()
    {
        m_haveTarget = false;
        m_gliding = false;
        m_animating = false;
        m_lastSeconds = 0.0;
        m_haveLastSeconds = false;
    }

private:
    // Settle when the squared cell-distance to the target drops below this
    // (≈0.02 cell), small enough to be sub-pixel at any sane cell size.
    static constexpr float settleEpsilon = 0.0004f;

    // Largest ease step honored in one frame. A paused renderer (idle) or a hitch
    // can hand update() a dt of seconds; without this cap one frame would ease the
    // whole way and snap. ~3 frames at 60 Hz — above any real interval, so normal
    // animation is untouched.
    static constexpr double maxStepSeconds = 0.05;

    // exp(-x) without <cmath> (keeps this header dependency-free). The result
    // only paces an animation — monotonic decay matters, exactness does not.
    // Range-reduce by halving x, evaluate the series on the small remainder, then
    // square back the same number of times (exp(-x) = (exp(-x/2^n))^(2^n)).
    static double fastExpNeg(double x)
    {
        if (x <= 0.0)
            return 1.0;
        if (x > 30.0)
            return 0.0;
        int halvings = 0;
        while (x > 0.5) {
            x *= 0.5;
            ++halvings;
        }
        double r = expNegSeries(x); // 0 <= x <= 0.5
        for (int i = 0; i < halvings; ++i)
            r *= r;
        return r;
    }

    // exp(-x) for 0 <= x <= 0.5 via a 6-term Maclaurin series (error < 1e-6).
    static double expNegSeries(double x)
    {
        const double x2 = x * x;
        const double x3 = x2 * x;
        const double x4 = x2 * x2;
        const double x5 = x4 * x;
        return 1.0 - x + x2 / 2.0 - x3 / 6.0 + x4 / 24.0 - x5 / 120.0;
    }

    float m_column = 0.0f;
    float m_vrow = 0.0f;
    float m_targetColumn = 0.0f;
    float m_targetVRow = 0.0f;
    double m_lastSeconds = 0.0;
    bool m_haveLastSeconds = false;
    bool m_haveTarget = false;
    bool m_gliding = false; // latched at move onset: this move glides to its target rather than snapping
    bool m_animating = false;
};

} // namespace Ubiquitty::Rendering
