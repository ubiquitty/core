// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Screen/PromptNavigation.h — the Cmd-Shift-Up / Cmd-Shift-Down prompt
// navigation viewport state machine.
//
// View > Jump to Previous/Next Prompt steps the scrollback viewport between the
// executed-command prompt rows (the bracketed OSC 133 CMD_EXECUTED marks).
// `direction` < 0 walks up to the previous prompt, > 0 down to the next,
// snapping to the live tail when there is none further down. Stepping is
// anchored on the prompt the last step landed on (`anchorRow`) so it walks all
// prompts including ones already on screen; when that anchor isn't visible
// anymore (manual scroll / new output) it re-derives from the viewport edges —
// up starts at the bottom-most prompt, down at the top-most. The landed prompt
// is placed one row below the viewport top for a little preceding context.
//
// This logic was duplicated, byte-for-byte, between the macOS
// UBWindowController scrollToPromptInDirection: and the Linux jumpToPrompt
// ("Mirrors macOS UBWindowController scrollToPromptInDirection"), so the policy
// lives here once and both frontends call it. The function is pure: it owns no
// platform types and reads the OSC 133 marks through a caller-supplied provider
// (the frontend wires it to Terminal::rowMark), so it is host-testable over a
// scripted mark layout with no real Terminal.
//
// The frontend remains responsible for the platform glue the result describes:
// writing the new viewOffset into its renderer/pane, storing the new anchor,
// repainting, and ringing the error bell on the Bell action. Nothing here
// touches a renderer, a lock, or a bell.

#include <cstdint>

namespace Ubiquitty::Screen {

// OSC 133 mark bits for an absolute buffer row. The provider returns the mark
// bitfield (the osc133Mark* bits from VT/Constants.h) for the given absolute
// row, or 0 for rows out of range / on the alternate screen (where marks aren't
// recorded). Keeping the model behind a provider keeps this header free of any
// Terminal/Core-model dependency; the frontend wires it to Terminal::rowMark.
using RowMarkReader = uint8_t (*)(void* context, int absoluteRow);

/// What a frontend should do with the result of a prompt-navigation step.
enum class PromptStepAction : int {
    // Nothing to do — already at the live tail going down, or no prompt to step
    // to. The frontend rings its error bell and leaves the viewport put.
    Bell = 0,
    // Move the viewport to `newViewOffset` and record `newAnchor` as the prompt
    // the step landed on, then repaint.
    Scroll = 1,
    // Going down with nothing further below: snap to the live tail
    // (`newViewOffset` is 0) and forget the anchor (`newAnchor` is -1), then
    // repaint. Distinguished from Scroll only so the frontend can clear its
    // stored anchor; both Scroll and SnapToTail mean "apply newViewOffset +
    // newAnchor and repaint".
    SnapToTail = 2,
};

/// The outcome of one prompt-navigation step.
///
/// `newViewOffset` and `newAnchor` are meaningful for `Scroll` and
/// `SnapToTail`; `Bell` leaves the viewport where it is, so both are ignored.
struct PromptStepResult {
    int newViewOffset; // viewport offset to apply (valid for Scroll / SnapToTail)
    int newAnchor; // prompt row the step landed on, to store as the next anchor
    PromptStepAction action;
};

// Step the prompt-navigation viewport.
//
//   history     scrollback line count (Terminal::scrollbackCount; 0 on the alt
//               screen). Absolute rows [0, history) are scrollback, [history,
//               history + rows) is the live screen.
//   rows        visible grid height.
//   viewOffset  current scrollback viewport offset (0 == live tail; larger ==
//               further back into history).
//   anchorRow   the prompt absolute row the last step landed on, or any value
//               not currently marked/visible to force a fresh re-derivation
//               (the frontends seed it to -1).
//   direction   < 0 to step up to the previous prompt, > 0 to step down.
//   markAt      OSC 133 mark provider keyed by absolute row.
//   markContext opaque context handed back to markAt.
//   markBits    which mark bits identify a navigable prompt (the frontends pass
//               osc133MarkCmdExecuted).
//
// Returns the viewport move + new anchor + the action the frontend applies.
// Pure: no platform types, no side effects, no allocation.
[[nodiscard]] inline PromptStepResult stepToPrompt(int history, int rows, int viewOffset, int anchorRow, int direction, RowMarkReader markAt, void* markContext, uint8_t markBits)
{
    PromptStepResult result { viewOffset, anchorRow, PromptStepAction::Bell };

    // Going down while already at the live tail — nothing below to step to.
    if (direction > 0 && viewOffset == 0) {
        result.action = PromptStepAction::Bell;
        return result;
    }

    const int total = history + rows; // absolute row count (scrollback + screen)
    const int topAbsoluteRow = history - viewOffset; // absolute row at the viewport top
    const int bottomAbsoluteRow = topAbsoluteRow + rows - 1; // ... and at the bottom

    // The anchor is valid only if its prompt is still on screen and still marked.
    const bool anchorVisible = anchorRow >= topAbsoluteRow && anchorRow <= bottomAbsoluteRow && (markAt(markContext, anchorRow) & markBits);

    int target = -1;
    if (direction < 0) {
        // Search above the anchor; from a fresh view, from the bottom-most
        // visible prompt (bottomAbsoluteRow + 1 exclusive) so an on-screen command
        // counts. Mirror of Terminal::findPromptMarkBefore: most recent marked row
        // at or before (ceiling - 1), capped to the last live row.
        const int ceiling = anchorVisible ? anchorRow : bottomAbsoluteRow + 1;
        int hi = ceiling - 1;
        if (hi >= total)
            hi = total - 1;
        for (int r = hi; r >= 0; --r) {
            if (markAt(markContext, r) & markBits) {
                target = r;
                break;
            }
        }
    } else {
        // Search below the anchor; from a fresh view, at/below the top row.
        // Mirror of Terminal::findPromptMarkAfter: first marked row at or after lo.
        const int floor = anchorVisible ? anchorRow + 1 : topAbsoluteRow;
        int lo = floor;
        if (lo < 0)
            lo = 0;
        for (int r = lo; r < total; ++r) {
            if (markAt(markContext, r) & markBits) {
                target = r;
                break;
            }
        }
    }

    if (target < 0) {
        // Nothing in this direction. Going down with history left to unwind:
        // snap to the live tail and forget the anchor. Otherwise ring the bell.
        if (direction > 0 && viewOffset > 0) {
            result.newViewOffset = 0;
            result.newAnchor = -1;
            result.action = PromptStepAction::SnapToTail;
        } else {
            result.action = PromptStepAction::Bell;
        }
        return result;
    }

    // Land the prompt one row below the top for a little preceding context,
    // clamped into the scrollback range [0, history].
    int offset = history - target + 1;
    if (offset < 0)
        offset = 0;
    if (offset > history)
        offset = history;
    result.newViewOffset = offset;
    result.newAnchor = target;
    result.action = PromptStepAction::Scroll;
    return result;
}

} // namespace Ubiquitty::Screen
