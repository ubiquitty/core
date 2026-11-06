// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>

namespace Ubiquitty {

// Apple-style cursor blink (macOS text caret, matched to TextEdit): a STEP
// function through five discrete opacity levels — 1, ¾, ½, ¼, 0 — as only those
// values ever appear in a capture. A long solid hold, symmetric 2-frame
// quarter-steps down to off, a brief off hold, then symmetric steps back up.
// Segment frame counts are measured from a 60 fps capture and sum to exactly
// 60 = 1.00 s. Stepping (vs. a smooth ramp) means the frontend only repaints on
// a level change (8/cycle), not every frame. Phased to start at the solid hold
// so the blink resumes cleanly after the post-activity hold.

namespace Cursor::Blink {
static constexpr uint32_t solidFrames = 32; // frames at full in one cycle
static constexpr uint32_t caretPeriodTicks = 60; // 1.00 s at 60 Hz

/// How a blink transitions between on and off.
///
/// `Fade` is the five-level step ramp described above, the macOS caret look.
/// `OnOff` is a hard blink with no intermediate opacities — the classic terminal
/// look — in the same rhythm and phase as the fade, so switching changes the
/// shape of the curve and not its timing.
///
/// Driven by `[appearance] blink_animation`.
enum class Animation : uint8_t {
    Fade = 0,
    OnOff = 1
};

/// The process-wide blink curve.
///
/// A render policy rather than per-call data: every blink path — the cursor
/// caret and SGR 5/6 text — reads it through `alphaAt`, on every frontend, so a
/// frontend sets it once from configuration instead of threading it through each
/// call. Same shape as the OSC clipboard policy globals. `Fade` is the default
/// because it keeps every existing caller, and the golden render tests,
/// byte-identical.
inline Animation s_animation = Animation::Fade;

/// Sets the process-wide blink curve.
///
/// - Parameter animation: The curve every blink path will read.
static inline void setAnimation(Animation animation) { s_animation = animation; }

/// Returns the process-wide blink curve.
[[nodiscard]] static inline Animation animation() { return s_animation; }

// Opacity at a monotonic tick. Pure function of the phase and the global style;
// Fade yields one of five levels, OnOff a binary 0/1 at the same >=0.5 threshold
// the `visible` gate uses (so on/off timing and phase match the fade exactly).
static inline float alphaAt(uint32_t monotonicTicks)
{
    struct Segment {
        float alpha;
        uint32_t frames;
    };
    static const Segment cycle[] = {
        { 1.00f, solidFrames }, // solid
        { 0.75f, 2 },
        { 0.50f, 2 },
        { 0.25f, 2 }, // step-down
        { 0.00f, 16 }, // off
        { 0.25f, 2 },
        { 0.50f, 2 },
        { 0.75f, 2 }, // step-up
    };
    uint32_t p = monotonicTicks % caretPeriodTicks;
    float alpha = 1.00f; // frames sum to caretPeriodTicks, so the loop always sets this
    for (const Segment& segment : cycle) {
        if (p < segment.frames) {
            alpha = segment.alpha;
            break;
        }
        p -= segment.frames;
    }
    if (s_animation == Animation::OnOff)
        return alpha >= 0.5f ? 1.0f : 0.0f;
    return alpha;
}

/// A desktop toolkit's caret blink, as a frontend reads it from its settings.
///
/// GTK's GtkText is the definition (`blink_cb`, `blink_alpha`): a cycle of
/// `periodMilliseconds` split evenly into visible, fading out, invisible and
/// fading in, the fades linear; input restarts the cycle half a period late so
/// a typing caret stays lit, focus restarts it on time; and the caret holds
/// solid once `timeoutSeconds` pass with neither. GTK's settings are
/// `gtk-cursor-blink`, `gtk-cursor-blink-time` and `gtk-cursor-blink-timeout`.
struct ToolkitBlink {
    bool isEnabled = true;
    std::uint32_t periodMilliseconds = 1200;
    std::uint32_t timeoutSeconds = 10;
};

/// Where a toolkit blink stands: the caret's opacity, and how long it keeps it.
struct ToolkitBlinkPhase {
    float alpha = 1.0f;
    /// Microseconds until the opacity next changes: zero during the raw
    /// continuous fade, a positive step deadline for the painted phase, and
    /// negative when it never changes again.
    std::int64_t microsecondsUntilChange = -1;
};

/// The fade's bounded presentation steps. GtkText's phase remains continuous;
/// a terminal repaints at these crossings instead of at every display refresh.
inline constexpr std::int64_t toolkitBlinkPaintStepsPerFade = 8;

/// How long a toolkit restart holds the cycle back.
///
/// GtkText starts the cycle half a period late after input (`pend`), so a
/// typing caret stays lit, and on time when focus arrives.
///
/// - Parameters:
///   - blink: The toolkit's settings.
///   - isInput: Whether input restarted the cycle rather than focus.
/// - Returns: Microseconds from the restart to the cycle's start.
[[nodiscard]] static inline std::int64_t toolkitBlinkDelayMicroseconds(const ToolkitBlink& blink, bool isInput) // bool-ok: GtkText's own add_blink_timeout(delay) distinction.
{
    return isInput ? static_cast<std::int64_t>(blink.periodMilliseconds) * 1000 / 2 : 0;
}

/// The toolkit blink at a moment, in GtkText's arithmetic.
///
/// - Parameters:
///   - blink: The toolkit's settings.
///   - microsecondsSinceCycleStart: Time since the cycle started, negative
///     while a restart still holds it back (``toolkitBlinkDelayMicroseconds``).
///   - microsecondsSinceInteraction: Time since input or focus last reached
///     the text, which the timeout counts from.
/// - Returns: The opacity and the time until it changes.
[[nodiscard]] static inline ToolkitBlinkPhase toolkitBlinkPhase(const ToolkitBlink& blink, std::int64_t microsecondsSinceCycleStart, std::int64_t microsecondsSinceInteraction)
{
    if (!blink.isEnabled || blink.periodMilliseconds == 0)
        return {};
    const std::int64_t untilTimeout = static_cast<std::int64_t>(blink.timeoutSeconds) * 1000000 - microsecondsSinceInteraction;
    if (untilTimeout < 0)
        return {};
    const std::int64_t period = static_cast<std::int64_t>(blink.periodMilliseconds) * 1000;
    // Clamped to the next moment past the timeout, after which the caret is
    // lit for good: a wake there sees the caret come back on.
    const auto until = [&](std::int64_t change) { return std::min(change, untilTimeout + 1); };
    // Until the cycle starts, and through its first quarter, the caret is lit.
    if (microsecondsSinceCycleStart < 0)
        return { 1.0f, until(period / 4 - microsecondsSinceCycleStart) };
    const std::int64_t intoCycle = microsecondsSinceCycleStart % period;
    const float phase = static_cast<float>(intoCycle) / static_cast<float>(period);
    if (phase < 0.25f)
        return { 1.0f, until(period / 4 - intoCycle) };
    if (phase < 0.5f)
        return { 1.0f - 4.0f * (phase - 0.25f), 0 };
    if (phase < 0.75f)
        return { 0.0f, until(period * 3 / 4 - intoCycle) };
    return { 4.0f * (phase - 0.75f), 0 };
}

/// The toolkit phase sampled for presentation, with a wake deadline per step.
/// The timeout still cuts a fade short and leaves the caret solid afterward.
[[nodiscard]] static inline ToolkitBlinkPhase toolkitBlinkPaintPhase(const ToolkitBlink& blink, std::int64_t microsecondsSinceCycleStart, std::int64_t microsecondsSinceInteraction)
{
    ToolkitBlinkPhase phase = toolkitBlinkPhase(blink, microsecondsSinceCycleStart, microsecondsSinceInteraction);
    if (phase.microsecondsUntilChange != 0)
        return phase;

    const std::int64_t period = static_cast<std::int64_t>(blink.periodMilliseconds) * 1000;
    const std::int64_t quarter = period / 4;
    const std::int64_t intoCycle = microsecondsSinceCycleStart % period;
    const bool isFadingOut = intoCycle < period / 2;
    const std::int64_t elapsed = intoCycle - (isFadingOut ? quarter : period * 3 / 4);
    const std::int64_t step = elapsed * toolkitBlinkPaintStepsPerFade / quarter;
    phase.alpha = static_cast<float>(isFadingOut ? toolkitBlinkPaintStepsPerFade - step : step)
        / static_cast<float>(toolkitBlinkPaintStepsPerFade);
    const std::int64_t nextStep = ((step + 1) * quarter + toolkitBlinkPaintStepsPerFade - 1) / toolkitBlinkPaintStepsPerFade;
    const std::int64_t untilTimeout = static_cast<std::int64_t>(blink.timeoutSeconds) * 1000000 - microsecondsSinceInteraction + 1;
    phase.microsecondsUntilChange = std::min(std::max<std::int64_t>(nextStep - elapsed, 1), untilTimeout);
    return phase;
}

} // namespace Cursor::Blink

// Shared blink-alpha curve for SGR 5 / SGR 6 text and the cursor caret. Same
// step function (Cursor::Blink::alphaAt) so blink-attributed text fades in phase
// with the caret; periodTicks selects the cycle length. periodTicks == 60
// (Cursor::Blink::caretPeriodTicks) reproduces the caret exactly; periodTicks == 30
// halves it (SGR 6 fast blink). Other periods scale the same shape
// proportionally. Pure function of nowTicks — deterministic across frontends.
static inline float blinkAlpha(uint32_t nowTicks, uint32_t periodTicks)
{
    if (periodTicks == 0)
        return 1.0f;
    const uint64_t scaled = static_cast<uint64_t>(nowTicks) * static_cast<uint64_t>(Cursor::Blink::caretPeriodTicks) / periodTicks;
    return Cursor::Blink::alphaAt(static_cast<uint32_t>(scaled));
}

static inline uint32_t ticksUntilBlinkAlphaChange(uint32_t nowTicks, uint32_t periodTicks)
{
    if (!periodTicks) {
        return 0;
    }
    const float currentAlpha = blinkAlpha(nowTicks, periodTicks);
    for (uint32_t delta = 1; delta <= periodTicks; ++delta) {
        if (blinkAlpha(nowTicks + delta, periodTicks) != currentAlpha) {
            return delta;
        }
    }
    return 0;
}

} // namespace Ubiquitty
