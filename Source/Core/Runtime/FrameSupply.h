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
    /// rows. ``Selection::viewportSpanCapacity`` spans for the frame always
    /// have room; an empty buffer receives none.
    Selection::ViewportSpans selectionSpans {};
    /// Whether the view is in a live resize. A resize does not wait for quiet
    /// output: only an open synchronized update and the redraw behind a
    /// whole-screen erase hold its frame, and both are judged under the live
    /// read's lock.
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
    /// The drain is saturated and published its newest capture inside the
    /// last publish window, so its next capture is due before a live read
    /// could do better.
    AwaitingCapture,
    /// Under the live read's lock: a synchronized update opened, or the
    /// screen was erased, after the published check.
    BlockOpenUnderLock,
    /// Under a live resize's lock: the redraw behind the newest whole-screen
    /// erase has not been parsed whole.
    RedrawArriving,
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
};

} // namespace Ubiquitty::Runtime
