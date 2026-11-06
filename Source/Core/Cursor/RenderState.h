// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cursor/Blink.h"
#include "Core/Terminal.h"

#include <cstdint>

namespace Ubiquitty::Cursor {

/// The shape a frontend paints the cursor as.
///
/// Distinct from `Cursor::Style`, which is the DECSCUSR code and folds blink
/// into the same value. This is the resolved geometry alone, so a frontend never
/// has to know that 3 and 4 are both underlines.
enum class Shape : uint8_t {
    Block = 0,
    Underline = 1,
    Bar = 2
};

/// The resolved per-frame description of the cursor.
///
/// A frontend paints `shape` at (`row`, `column`) if and only if `visible`, and
/// does not inspect DECSCUSR, the blink phase or the activity timing — every one
/// of those decisions is folded in here, so two frontends cannot disagree about
/// when the cursor is on.
struct RenderState {
    int row;
    int column;
    Shape shape;
    bool visible; // blinkAlpha >= 0.5 — discrete on/off gate
    float blinkAlpha; // 0..1 smooth blink opacity (macOS fade); 1 when
                      // not blinking or activity-frozen
};

/// Frames the cursor stays solid after user activity before the blink resumes.
///
/// Regardless of the DECSCUSR blink mode, so the cursor does not vanish
/// mid-keystroke. 40 frames is about 0.67 s at 60 Hz, matching macOS TextEdit.
/// The hold stands in for the cycle's own solid phase, which is why the blink
/// resumes at the fade-out rather than at the start of a cycle.
static constexpr uint32_t activityHoldTicks = 40;

/// Converts a configured blink period in milliseconds to ticks.
///
/// Ticks run at 60 Hz — `PAL::monotonicTicks`, the same clock the activity hold
/// counts in — so the scaling is linear: `Blink::caretPeriodTicks` ticks is
/// 1000 ms.
///
/// - Parameter periodMilliseconds: The configured period. 0 means "use the
///   default", so an unset preference keeps the built-in cadence.
/// - Returns: The period in ticks, never less than 1 — a tiny configured value
///   would otherwise divide the phase scaling by zero.
static inline uint32_t blinkPeriodTicks(uint32_t periodMilliseconds)
{
    if (periodMilliseconds == 0)
        return Cursor::Blink::caretPeriodTicks;
    const uint64_t ticks = static_cast<uint64_t>(periodMilliseconds) * static_cast<uint64_t>(Cursor::Blink::caretPeriodTicks) / 1000ull;
    return ticks < 1 ? 1u : static_cast<uint32_t>(ticks);
}

/// Records a user-initiated input event for the activity-freeze policy.
///
/// A frontend calls this from its keyboard and mouse handlers.
///
/// - Parameters:
///   - t: The terminal the input reached. Null is a no-op.
///   - monotonicTicks: The current tick.
static inline void noteUserActivity(Ubiquitty::Terminal* t, uint32_t monotonicTicks)
{
    if (t)
        t->lastActivityTick = monotonicTicks;
}

/// Returns the concrete style the `FollowUserPreference` sentinel resolves to.
///
/// The user's shape preference crossed with the configured blink. This is the
/// one source of truth shared by `resolve`, which paints, and
/// `effectiveStyleCode`, which answers DECRQSS — so a report cannot drift
/// from what is on screen.
///
/// The blink here is the CONFIGURED one, not the momentary post-activity freeze:
/// a report describes the configured style, not this frame. Mode 12 does not
/// enter it — the reader's `[cursor] blink`, whose unset value is its default,
/// is the answer whether or not a program sent cnorm.
///
/// - Parameter t: The terminal to read the preferences from.
/// - Returns: A concrete style, never `FollowUserPreference`.
static inline Cursor::Style effectiveStyleForUserPreference(const Ubiquitty::Terminal& t)
{
    const bool isBlinkingPreferred = t.cursorBlinkPref;
    switch (t.cursorShapePref) {
    case 1: // underline
        return isBlinkingPreferred ? Cursor::Style::BlinkingUnderline : Cursor::Style::SteadyUnderline;
    case 2: // bar
        return isBlinkingPreferred ? Cursor::Style::BlinkingBar : Cursor::Style::SteadyBar;
    default: // block
        return isBlinkingPreferred ? Cursor::Style::BlinkingBlock : Cursor::Style::SteadyBlock;
    }
}

/// Whether the terminal's cursor style blinks.
///
/// An explicit DECSCUSR style wins and `FollowUserPreference` resolves to the
/// user's preference; the master blink preference can still force steady.
/// For a preference-resolved style `Cursor::isBlinking` already folds in the
/// mode-12 / att610 precedence (a `cnorm` can't silence a user who wants
/// blink; with no explicit pref, mode 12 governs), and `&& cursorBlinkPref` is
/// then a no-op — so this one expression is exact for both origins.
///
/// - Parameter t: The terminal to read.
/// - Returns: True when the resolved style blinks.
[[nodiscard]] static inline bool isStyleBlinking(const Ubiquitty::Terminal& t)
{
    const Cursor::Style style = (t.cursorStyle == Cursor::Style::FollowUserPreference) ? effectiveStyleForUserPreference(t) : t.cursorStyle;
    return Cursor::isBlinking(style) && t.cursorBlinkPref;
}

/// Resolves cursor appearance independently of program-requested visibility.
///
/// Presentation holds need the continuing blink clock while DECTCEM is hidden.
/// The program's visibility remains a separate fact; ``resolve`` applies it.
///
/// - Parameters:
///   - t: The terminal to resolve. Null or no active screen yields an invisible state.
///   - monotonicTicks: The current tick, for the blink phase and activity hold.
/// - Returns: The resolved row, column, shape, visibility and blink opacity.
static inline RenderState resolveAppearance(const Ubiquitty::Terminal* t, uint32_t monotonicTicks)
{
    RenderState out;
    out.row = 0;
    out.column = 0;
    out.shape = Shape::Block;
    out.visible = false;
    out.blinkAlpha = 0.0f;
    if (!t || !t->screens.active)
        return out;
    out.row = t->screens.active->cursorY;
    out.column = t->screens.active->cursorX;
    // Shape + whether-it-blinks. An explicit DECSCUSR style (app-set) wins;
    // FollowUserPreference resolves to the user's shape/blink preferences via
    // the shared helper (single source of truth with the DECRQSS report). The
    // renderer never interprets this — it paints the resolved state.
    const Cursor::Style style = (t->cursorStyle == Cursor::Style::FollowUserPreference) ? effectiveStyleForUserPreference(*t) : t->cursorStyle;
    switch (style) {
    case Cursor::Style::BlinkingUnderline:
    case Cursor::Style::SteadyUnderline:
        out.shape = Shape::Underline;
        break;
    case Cursor::Style::BlinkingBar:
    case Cursor::Style::SteadyBar:
        out.shape = Shape::Bar;
        break;
    default:
        out.shape = Shape::Block;
        break;
    }
    const bool isBlinkingStyle = isStyleBlinking(*t);
    const uint32_t hold = (t->cursorHoldFrames != 0) ? t->cursorHoldFrames : activityHoldTicks;
    // A terminal that has seen no activity phases from tick 0 the way one that
    // just saw activity does: the hold first, then the fade. A fresh terminal
    // opens on a solid cursor, and a capture pinned at tick 0 draws the same
    // solid block the clockless capture path pins by construction.
    const uint32_t sinceActivity = monotonicTicks - t->lastActivityTick;
    const bool freezeOn = sinceActivity < hold;
    // Phase the blink from the END of the post-activity hold, not the absolute
    // clock — so it always resumes deterministically. The hold already supplied
    // the solid phase, so resume at the fade-out (offset past the cycle's solid):
    // hold solid, then step down → off → step up → 32-frame solid → …
    const uint32_t sinceHold = (sinceActivity >= hold) ? (sinceActivity - hold) : 0;
    // Scale the post-hold phase into the caret's native 60-tick cycle so the
    // step curve (Cursor::Blink::alphaAt) keeps its shape while the period follows
    // the user's configured blink rate. periodTicks == caretPeriodTicks (the
    // default, when cursorBlinkPeriodMilliseconds is 0) makes this the identity
    // and reproduces the previous blink exactly. The solidFrames phase offset
    // lives in the caret domain, so it is added after scaling — keeping the
    // "resume at the fade-out" alignment regardless of period.
    const uint32_t periodTicks = blinkPeriodTicks(t->cursorBlinkPeriodMilliseconds);
    const uint32_t scaledPhase = static_cast<uint32_t>(static_cast<uint64_t>(sinceHold) * Cursor::Blink::caretPeriodTicks / periodTicks);
    // Unfocused: the frontend paints a static hollow outline, so freeze the
    // blink — a background cursor that blinks is wrong, and on a busy stream the
    // per-frame re-resolve would otherwise flicker the outline on the blink
    // phase.
    out.blinkAlpha = (!isBlinkingStyle || freezeOn || !t->windowFocused) ? 1.0f : Cursor::Blink::alphaAt(scaledPhase + Cursor::Blink::solidFrames);
    out.visible = out.blinkAlpha >= 0.5f; // discrete on/off gate
    return out;
}

/// Returns the cursor state to paint directly from the terminal model.
///
/// - Parameters:
///   - t: The terminal to resolve. Null, DECTCEM hidden, or no active screen
///     yields an invisible state.
///   - monotonicTicks: The current tick, for the blink phase and activity hold.
/// - Returns: The resolved cursor, including program-requested visibility.
static inline RenderState resolve(const Ubiquitty::Terminal* t, uint32_t monotonicTicks)
{
    if (!t || !t->modes.isCursorVisible) {
        return { 0, 0, Shape::Block, false, 0.0f };
    }
    return resolveAppearance(t, monotonicTicks);
}

/// Returns how many ticks until the cursor's opacity next changes.
///
/// Lets a frontend sleep to the next visible transition instead of repainting
/// every frame — the step curve changes level 8 times a cycle, not 60.
///
/// - Parameters:
///   - t: The terminal to inspect.
///   - monotonicTicks: The current tick.
/// - Returns: The tick delta, or 0 when the opacity does not change within one
///   blink period — which includes a steady cursor and a frozen one.
static inline uint32_t ticksUntilAlphaChange(const Ubiquitty::Terminal* t, uint32_t monotonicTicks)
{
    if (!t) {
        return 0;
    }

    const float currentAlpha = resolve(t, monotonicTicks).blinkAlpha;
    const uint32_t hold = (t->cursorHoldFrames != 0) ? t->cursorHoldFrames : activityHoldTicks;
    const uint32_t sinceActivity = monotonicTicks - t->lastActivityTick;
    if (sinceActivity < hold) {
        const uint32_t untilHoldEnds = hold - sinceActivity;
        if (resolve(t, monotonicTicks + untilHoldEnds).blinkAlpha != currentAlpha) {
            return untilHoldEnds;
        }
    }

    const uint32_t periodTicks = blinkPeriodTicks(t->cursorBlinkPeriodMilliseconds);
    for (uint32_t delta = 1; delta <= periodTicks; ++delta) {
        if (resolve(t, monotonicTicks + delta).blinkAlpha != currentAlpha) {
            return delta;
        }
    }
    return 0;
}

/// Returns the DECSCUSR style code the terminal is showing, for DECRQSS.
///
/// An explicit application-set style reports verbatim. `FollowUserPreference` is
/// an internal sentinel that must never reach a program-observable reply —
/// reporting its raw 0 tells the application "blinking block" while a steady bar
/// may be on screen — so it resolves to the concrete code matching the user's
/// shape and effective blink, and the query round-trips what is displayed.
///
/// The blink decision mirrors `resolve`'s: the configured blink, not the
/// momentary activity freeze. A report describes the configured style, not this
/// frame.
///
/// - Parameter t: The terminal to report on.
/// - Returns: A code in 1...6, never 0.
static inline uint8_t effectiveStyleCode(const Ubiquitty::Terminal& t)
{
    const Cursor::Style style = (t.cursorStyle == Cursor::Style::FollowUserPreference) ? effectiveStyleForUserPreference(t) : t.cursorStyle;
    return static_cast<uint8_t>(style);
}

} // namespace Ubiquitty::Cursor
