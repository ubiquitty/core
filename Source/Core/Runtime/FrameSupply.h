// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Selection/SelectionOverCapture.h"
#include "Core/Selection/SelectionSpans.h"
#include "Core/Selection/TerminalSelection.h"

#include <cstdint>

// Source/Core/Runtime/FrameSupply.h — the question a view asks of its pane at
// each display tick, and Core's answer: which frame reaches the glass and by
// which route, or why none does. ``TerminalSession::takeFrame`` answers it.

namespace Ubiquitty::Runtime {

/// What a view asks of its pane at one display tick.
///
/// The view's own facts, which the pane cannot read: where its clock stands,
/// its selection, whether it is being resized, and what its last frame drew.
struct FrameDemand {
    /// Now, on the 60 Hz monotonic clock synchronized updates are timed on.
    Ticks nowTicks {};
    /// The view's selection: handed to the frames the drain captures from
    /// here on, and brought up to the frame taken. Null for a view that
    /// selects nothing.
    Selection::Session* selection = nullptr;
    /// Receives the selection's spans over a captured frame, in its visible
    /// rows, as many as it has room for. ``Selection::viewportSpanCapacity``
    /// spans for the frame's grid always have room. A frame captured at a
    /// grid larger than the one the buffer was sized for says how many it
    /// needed (``Selection::CaptureSpans::needed``), and
    /// ``TerminalSession::selectionOverCapture`` on the frame refills a
    /// buffer that size. An empty buffer receives none.
    Selection::ViewportSpans selectionSpans {};
    /// Whether the view is in a live resize. A resize does not wait for quiet
    /// output: only an open synchronized update and the redraw behind a
    /// whole-screen erase still arriving hold its frame, and the owner serves
    /// the view's demand by that rule alone.
    bool isResizing = false;
    /// Whether the view's last presented frame drew search matches. A
    /// captured frame is built without the search tiers, so such a view reads
    /// the model, saturated or not, until a frame it reads draws none.
    bool keepsSearchLive = false;
};

/// How the frame a view asked for reaches the glass.
enum class FrameRoute : std::uint8_t {
    /// The newest frame the drain captured, with the view's selection
    /// resolved for it. The grid may have moved on since the capture, so a
    /// view that presents it asks for another frame; and it draws no search
    /// matches, so the view stops keeping search live.
    Captured,
    /// No capture waits and nothing published holds the frame: the view reads
    /// the model under the model lock, and asks
    /// ``TerminalSession::liveReadDecline`` under that lock before it reads.
    LiveRead,
    /// Nothing is presented this tick; ``FrameSupply::decline`` says why.
    Declined,
};

/// Why a view presents nothing this tick.
enum class FrameDecline : std::uint8_t {
    /// A frame is presented.
    None,
    /// The coherence rule holds the frame: the grid may show a screen the
    /// program has not finished drawing. A frame the rule releases wakes the
    /// view, through the delivery that released it or the loop's release
    /// hook.
    HeldByCoherence,
    /// A synchronized update is open, or the redraw behind a whole-screen
    /// erase is still arriving. The view keeps the frame owed and asks again
    /// when the hold's budget is spent.
    HeldByPresentation,
    /// No capture waits, and the owner has been asked for one, or a saturated
    /// drain's next is due inside the publish window. Its publication wakes
    /// the view.
    AwaitingCapture,
    /// Under the live read's lock: a synchronized update opened, or the
    /// screen was erased, after the published check.
    BlockOpenUnderLock,
    /// The view is being resized, and the redraw behind the newest
    /// whole-screen erase has not been parsed whole. The view asks again when
    /// the hold's budget is spent, unless a capture's publication wakes it
    /// first.
    RedrawArriving,
    /// The view's demand for a frame has gone unserved for
    /// ``FrameLane::ownerStallMicros`` and the owner has taken no turn since
    /// it was raised: the owner is stuck in long work, not about to publish.
    /// The view keeps its frame and asks again.
    OwnerStalled,
};

/// What a view presents at one display tick.
struct FrameSupply {
    /// How the frame reaches the glass.
    FrameRoute route = FrameRoute::Declined;
    /// Why nothing is presented; ``FrameDecline::None`` on every other route.
    FrameDecline decline = FrameDecline::None;
    /// A captured frame's selection: how many spans were written to the
    /// demand's buffer, what output did to the selection there, and whether
    /// the view's session took the frame's resolved one back.
    Selection::CaptureSpans selection {};
    /// Where the selection begins and ends on a captured frame.
    Selection::ViewportEnds selectionEnds {};
    /// Ticks until the view asks again on its own, from now: on
    /// ``FrameDecline::HeldByPresentation`` and
    /// ``FrameDecline::RedrawArriving``, the hold's budget, at least 1; on
    /// ``FrameDecline::AwaitingCapture`` for a pane no ingest loop drains,
    /// whose model another thread held at the ask, 1, since nothing else will
    /// serve it. Zero on every other answer, where what ends the hold wakes
    /// the view: the delivery or release hook that admits a held frame, and
    /// the publication of an awaited capture.
    std::uint32_t askAgainTicks = 0;
};

/// The live read's own answer, under the model lock, after
/// ``FrameRoute::LiveRead``.
struct LiveReadAnswer {
    /// Why the read presents nothing, or ``FrameDecline::None``.
    FrameDecline decline = FrameDecline::None;
    /// Ticks until the view asks again on its own, from the demand's tick:
    /// the budget of the hold that declined the read, at least 1. Zero when
    /// the read presents.
    std::uint32_t askAgainTicks = 0;
};

} // namespace Ubiquitty::Runtime
