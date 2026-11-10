// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/ThreadSafety.h"
#include "Core/Selection/SelectionSpans.h"
#include "Core/Selection/TerminalSelection.h"

#include <cstdint>
#include <vector>

// A view's selection resolved for each frame the drain captures, by the drain,
// under the lock it captures under. The frontend hands its selection over
// without the lock (``PublishedSelection``); the drain runs on it the
// evaluation the live read runs on the view's own session
// (``viewportSpansAndEnds``), against the terminal the frame freezes, and
// stamps the frame with the result (``CapturedSelection``). The frontend draws
// the stamped spans over the frame and takes the resolved session back, so
// what output did to the selection reaches the view without the view reading
// the terminal.

namespace Ubiquitty::Selection {

/// What resolving a selection for a captured frame did to it.
enum class CaptureOutcome : std::uint8_t {
    /// The ends stand where they stood.
    Kept,
    /// Output moved the ends with the characters, or on the alternate screen
    /// the rows, they name.
    Moved,
    /// Output took what an end named, or moved one end and not the other: the
    /// selection is gone, as the live read drops it.
    Lost,
};

/// A view's selection as its frontend handed it to the frames the drain
/// captures.
struct PublishedSelection {
    /// Which hand-off this is, counted from one; zero names none.
    std::uint64_t generation = 0;
    /// The newest capture that resolved this hand-off, as far as its holder
    /// knows: the drain's count of them, or the one the view last took back.
    std::uint64_t resolution = 0;
    /// The view's session at the hand-off, or as the holder last resolved or
    /// took it back.
    Session session {};
};

/// A view's selection resolved for one captured frame, against the terminal
/// the frame froze.
struct CapturedSelection {
    /// The hand-off resolved, zero when there was none.
    std::uint64_t generation = 0;
    /// Which capture of that hand-off this is, counted from one.
    std::uint64_t resolution = 0;
    /// What output since the previous capture did to the selection.
    CaptureOutcome outcome = CaptureOutcome::Kept;
    /// The hand-off's session after resolving: synchronized with the frame's
    /// terminal as the live read leaves the session it draws.
    Session session {};
    /// Where the selection begins and ends on the frame.
    ViewportEnds ends {};
    /// The highlight's spans in the frame's visible rows, as
    /// ``viewportSpansAndEnds`` writes them; the first `spanCount` are the
    /// frame's.
    std::vector<Rendering::SelectionRowSpan> spans;
    int spanCount = 0;
};

/// Resolves a view's selection for a frame being captured.
///
/// The drain's half, under the model lock, at the moment the frame is
/// captured: `selection` is the drain's own copy of the latest hand-off,
/// synchronized here as the live read synchronizes the view's session, so
/// each capture continues from the one before. A selection lost here stays
/// lost until the frontend hands over another one.
///
/// - Parameters:
///   - selection: The drain's copy of the hand-off, synchronized in place.
///   - terminal: The terminal the frame is captured from.
///   - viewOffset: The rows the frame is scrolled back, as it resolves them.
///   - out: Receives the resolved selection; its spans grow to the frame's
///     ``viewportSpanCapacity``.
///   - blocks: Receives where the selection falls inside rich text blocks,
///     for the capture to draw under the glyphs.
/// - Returns: How many block entries were written.
[[nodiscard]] int resolveForCapture(PublishedSelection& selection, Terminal& terminal, int viewOffset, CapturedSelection& out, BlockSelections blocks) noexcept;

/// Records the view's selection as the next hand-off, unless nothing the drain
/// resolves changed since the last one.
///
/// The frontend's half, on its own thread. The view's position over the
/// terminal (``Session/viewportObservedHistory``, the viewport anchor and
/// whether the view keeps one) is the view's alone, and resolving a selection
/// neither reads nor writes it, so a move of it never forces a hand-off. A
/// delivery's output transition over a standing selection moves only that
/// position (``updateAfterOutput``) and hands nothing over, except on a view
/// that keeps a logical anchor or holds a gesture open: that transition
/// synchronizes the session against the terminal itself.
///
/// - Parameters:
///   - selection: The view's session.
///   - handedOff: The frontend's record of its last hand-off, advanced to the
///     next one.
/// - Returns: Whether `handedOff` now holds a hand-off to publish.
[[nodiscard]] bool prepareHandOff(const Session& selection, PublishedSelection& handedOff) noexcept UBIQUITTY_NONBLOCKING;

/// The answer of ``takeCapturedSelection``.
struct CaptureSpans {
    /// How many spans were written.
    int count = 0;
    /// How many spans the frame's selection has. More than `count` when the
    /// buffer had no room for them all, as one sized for a grid other than
    /// the frame's can: a second take of the same frame into a buffer this
    /// size writes every one.
    int needed = 0;
    /// What output did to the selection the frame shows.
    CaptureOutcome outcome = CaptureOutcome::Kept;
    /// Whether the view's session took the frame's resolved one back.
    bool didAdopt = false;
};

/// The selection a captured frame shows, and the view's session brought up to
/// it.
///
/// The frontend's half, without the model lock. The frame shows the selection
/// as it stood when the frame was captured, resolved against the terminal the
/// frame froze, so the spans are the frame's whatever the view did since.
/// When nothing the drain resolves changed in the view's session since the
/// hand-off the frame resolved, and the view holds no later capture's, the
/// view takes the resolved session back, keeping its own position: a
/// selection output moved or lost is moved or lost in the view as the live
/// read leaves it. A view that moved its selection since keeps it, and the
/// next capture resolves that one.
///
/// - Parameters:
///   - selection: The view's session.
///   - handedOff: The frontend's record of its last hand-off.
///   - captured: The frame's resolved selection.
///   - out: Receives the spans, as many as it has room for; a buffer of
///     ``viewportSpanCapacity`` spans for the frame's grid always has room.
///   - ends: Receives where the selection begins and ends on the frame.
/// - Returns: How many spans were written and how many the frame has, the
///   outcome, and whether the view took the resolved session.
[[nodiscard]] CaptureSpans takeCapturedSelection(Session& selection, PublishedSelection& handedOff, const CapturedSelection& captured,
    ViewportSpans out, ViewportEnds& ends) noexcept UBIQUITTY_NONBLOCKING;

} // namespace Ubiquitty::Selection
