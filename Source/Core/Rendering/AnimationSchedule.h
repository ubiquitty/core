// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/Cursor/Style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Ubiquitty::Rendering {

/// Whether a pane's pixels are on display, for the frame-rate ladder alone —
/// the runtime's drain gate keeps its own presentation state.
enum class FramePresentation : std::uint8_t {
    /// Nothing of the pane is on screen — a tab behind the front one, a
    /// miniaturized window, a window covered whole or on another space.
    Background UBIQUITTY_SWIFT_NAME(background),
    /// The pane's pixels are visible, wholly or in part.
    Presented UBIQUITTY_SWIFT_NAME(presented)
};

/// The cadence a hidden pane's session still publishes snapshots at, so the
/// state a foregrounded pane repaints from is never older than this.
inline constexpr std::int32_t backgroundSyncPublishFramesPerSecond = 10;

/// The presentation frame rate a clock runs at: the screen's own maximum,
/// clamped by the user's cap when one is set — and zero, meaning no continuous
/// drive at all, when the pane is not presented. Partly visible pixels deserve
/// current state; a pane nobody can see deserves no link, because a frame per
/// refresh into a drawable nothing composites is the whole cost of a covered
/// terminal, and the pane repaints on demand from the edge that brings it
/// back.
///
/// - Parameters:
///   - screenMaximumFPS: The display's maximum frames per second.
///   - userLimitFPS: The configured limit; zero or less means none.
///   - presentation: Whether the pane's pixels are on display.
/// - Returns: The frames per second the clock should request; zero for none.
[[nodiscard]] constexpr std::int32_t presentationFramesPerSecond(std::int32_t screenMaximumFPS, std::int32_t userLimitFPS, FramePresentation presentation)
{
    if (presentation == FramePresentation::Background)
        return 0;
    std::int32_t rate = screenMaximumFPS;
    if (userLimitFPS > 0 && userLimitFPS < rate)
        rate = userLimitFPS;
    return rate;
}

/// Whether blink content is visible enough to keep an animation clock armed.
///
/// The live-screen reading decides on the tail; scrolled back, the rows on
/// display are outside that reading, so the sticky hint keeps the clock
/// conservatively armed — a blink cell in viewed history must not freeze.
/// The hint alone never arms a clock at the tail: sticky-true would hold it
/// for the life of the grid once anything ever blinked.
///
/// One rule for the session's schedule and both frontends' clocks — three
/// call sites reading it differently is a cursor that blinks in one arm and
/// freezes in another.
///
/// - Parameters:
///   - hasBlinkCellOnScreen: The live-screen reading.
///   - hasBlinkHint: The grid's sticky hint.
///   - presentedViewOffset: Rows of history presented above the tail.
/// - Returns: Whether blink content can be on display.
[[nodiscard]] constexpr bool isBlinkContentVisible(bool hasBlinkCellOnScreen, bool hasBlinkHint, int presentedViewOffset)
{
    return hasBlinkCellOnScreen || (hasBlinkHint && presentedViewOffset > 0);
}

/// Everything that can keep a pane's animation clock armed for another tick.
///
/// Gathered from the published snapshot and from the renderer's own live state,
/// which is why it is a struct of plain scalars rather than a snapshot
/// reference: the two halves are read under different rules — one lock-free at
/// capture, one main-thread-owned — and a frontend is the only thing that holds
/// both.
///
/// The decision itself is not a frontend's to make. Each arm asking "is anything
/// still moving?" separately is a cursor that blinks in one and freezes in
/// another, and the case that goes wrong is never the ordinary one: it is a
/// cursor style nobody enumerated, or a blink cell scrolled out of the live
/// reading.
// No continuous-link tick is gated on `needsDisplay || isAnimating ||
// isInLiveResize`. The ~60 sites raising `needsDisplay` are not the complete
// list of reasons pixels move: in continuous mode those flags are inert. The
// on-demand path carries the repaint only while the link is paused, that is,
// while the session is idle. While output is flowing the link alone is the
// driver, and nothing maintains `needsDisplay` for it.
//
// A tick gated on those flags freezes ubench on one frame, advancing only when
// the application is switched away and back, and blanks the row of a link
// hovered under load, because the frame that would repaint a mis-marked row a
// few milliseconds later is skipped instead of merely late.
//
// A gate that skips ticks has to ask what changed — Core computes that as
// `Rendering::FrameKind::Unchanged`, which only the sync daemon consumes — and
// not what happened to ask for a repaint.

struct AnimationDemand {
    /// Whether the focused search match's pill is mid-bounce: a smooth
    /// animation for as long as the curve runs.
    bool isFocusedMatchBouncing = false;
    /// Whether the cursor is mid-glide: a smooth animation for as long as the
    /// slide runs. An animation only advances when a frame asks for the time,
    /// so a drive that stops presenting on quiet freezes the caret wherever
    /// the last frame left it, until an unrelated redraw finishes the glide.
    bool isCursorSliding = false;
    /// Alpha of a decaying visual-bell flash; anything above zero is still
    /// drawing.
    float bellFlashAlpha = 0.0f;
    /// The live-screen blink reading, taken under the model lock at capture.
    bool hasBlinkCellOnScreen = false;
    /// The grid's sticky blink hint: armed by any blink-attributed write and
    /// never cleared.
    bool hasBlinkHint = false;
    /// Rows of history presented above the tail.
    int presentedViewOffset = 0;
    /// A multi-frame image is still advancing.
    bool isImageAnimating = false;
    /// A hilite (mode 1001) wait is outstanding, whose deadline must fire even
    /// with no output at all.
    bool isHiliteAwaiting = false;
    /// A synchronised-output (DECSET 2026) block is held. Its watchdog advances
    /// only when a frame is drawn, so a frontend whose repaints follow arriving
    /// bytes never expires a block an application opened and then went quiet on
    /// -- the display stays stale for as long as the application stays silent.
    bool isCursorVisible = false;
    /// The user's blink preference, which decides the styles that carry no
    /// blink of their own.
    bool isCursorBlinkEnabled = false;
    Cursor::Style cursorStyle = Cursor::Style::FollowUserPreference;
    /// A cursor in an unfocused pane does not blink, so it schedules nothing.
    bool isCursorFocused = true;
    /// The pane's frames are withheld (`Runtime::paneFrameDelivery`): nothing
    /// it animates reaches the glass, so only a deadline still owes a fire.
    bool isFrameDeliveryWithheld = false;

    /// Whether the cursor is both on screen and mid-blink.
    ///
    /// The style is asked rather than enumerated. Listing the blinking styles
    /// reads as equivalent while DECSCUSR spans 0 through 6, and stops being so
    /// the moment a style is added — leaving a cursor that blinks on screen and
    /// schedules no repaint to show it.
    ///
    /// - Returns: Whether a repaint is owed to the cursor.
    [[nodiscard]] constexpr bool isCursorBlinkingOnScreen() const
    {
        if (!isCursorVisible || !isCursorFocused || !isCursorBlinkEnabled)
            return false;
        // FollowUserPreference carries no blink of its own and answers false to
        // `isBlinking`, so it falls through to the preference alone.
        return cursorStyle == Cursor::Style::FollowUserPreference || Cursor::isBlinking(cursorStyle);
    }

    /// Whether pixels on this pane are changing.
    ///
    /// Separate from ``isAnythingAnimating`` because a hilite wait is not
    /// content: it owes a deadline, not a frame. A schedule that treated the
    /// two alike would run the smooth per-tick cadence for a pane whose only
    /// business is a timeout, spending sixty frames a second to watch a clock.
    ///
    /// - Returns: Whether something is drawing.
    [[nodiscard]] constexpr bool isContentAnimating() const
    {
        return bellFlashAlpha > 0.0f
            || isCursorSliding
            || isFocusedMatchBouncing
            || isBlinkContentVisible(hasBlinkCellOnScreen, hasBlinkHint, presentedViewOffset)
            || isImageAnimating
            || isCursorBlinkingOnScreen();
    }

    /// Whether anything on this pane still owes a frame.
    ///
    /// Content animates for a reader, and a pane whose frames are withheld has
    /// none: the blink, the flash and the image advance would be drawn into a
    /// drawable nobody composites, and the clock that asked for them would
    /// spend a frame per tick to do it. The hilite deadline stays — it owes a
    /// timeout rather than a frame, and the application waiting for the reply
    /// cannot see the window either way.
    ///
    /// - Returns: Whether the animation clock should stay armed.
    [[nodiscard]] constexpr bool isAnythingAnimating() const
    {
        return isHiliteAwaiting || (!isFrameDeliveryWithheld && isContentAnimating());
    }
};

struct AnimationScheduleInputs {
    uint32_t blinkDelayTicks = 0;
    uint32_t cursorDelayTicks = 0;
    uint32_t imageDelayMilliseconds = 0;
    /// A blink delay the session already resolved, for a caller holding its
    /// verdict rather than the ticks (`TerminalSession::nextAnimationDelay`);
    /// zero when nothing blinks.
    double resolvedBlinkDelaySeconds = 0;
    bool smoothAnimationActive = false;
    bool hiliteAwaiting = false;

    /// The next animation delay these inputs schedule, in seconds.
    ///
    /// A member spelling of ``nextAnimationDelaySeconds`` because Swift's
    /// C++ importer carries member functions where it drops namespace-level
    /// free functions — both frontends' clocks call one implementation.
    ///
    /// - Returns: The delay, at least one tick; see ``nextAnimationDelaySeconds``
    ///   for when it may pass half a second.
    [[nodiscard]] double nextDelaySeconds() const;
};

/// The delay to the next frame these inputs owe, in seconds.
///
/// The sooner of every pending term, and never under one tick. Half a second
/// is both the idle answer and a ceiling on the terms whose crossing is not
/// exact: the hilite wait, which polls its timeout at it, and an image frame.
///
/// A blink's crossing is exact — the tick its opacity next changes — so when
/// blinks are all that is pending the delay is that crossing, however far
/// away. The caret holds solid for 32 ticks, past the ceiling, and a capped
/// delay woke the pane before the fade-out began to draw a frame identical to
/// the last, once per cycle.
///
/// - Parameter inputs: What is pending on the pane.
/// - Returns: The delay in seconds.
inline double nextAnimationDelaySeconds(const AnimationScheduleInputs& inputs)
{
    constexpr double tickSeconds = 1.0 / 60.0;
    constexpr double maxDelaySeconds = 0.5;
    double blinkDelay = std::numeric_limits<double>::infinity();
    if (inputs.blinkDelayTicks)
        blinkDelay = std::min(blinkDelay, static_cast<double>(inputs.blinkDelayTicks) * tickSeconds);
    if (inputs.cursorDelayTicks)
        blinkDelay = std::min(blinkDelay, static_cast<double>(inputs.cursorDelayTicks) * tickSeconds);
    if (inputs.resolvedBlinkDelaySeconds > 0)
        blinkDelay = std::min(blinkDelay, inputs.resolvedBlinkDelaySeconds);
    const bool isBlinkTheOnlyTerm = std::isfinite(blinkDelay) && !inputs.smoothAnimationActive
        && !inputs.imageDelayMilliseconds && !inputs.hiliteAwaiting;
    if (isBlinkTheOnlyTerm)
        return std::max(blinkDelay, tickSeconds);

    double delay = std::min(maxDelaySeconds, blinkDelay);
    if (inputs.smoothAnimationActive)
        delay = tickSeconds;
    if (inputs.imageDelayMilliseconds)
        delay = std::min(delay, static_cast<double>(inputs.imageDelayMilliseconds) / 1000.0);
    return std::clamp(delay, tickSeconds, maxDelaySeconds);
}

inline double AnimationScheduleInputs::nextDelaySeconds() const
{
    return nextAnimationDelaySeconds(*this);
}

} // namespace Ubiquitty::Rendering
