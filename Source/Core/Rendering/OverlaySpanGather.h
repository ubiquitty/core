// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Terminal.h"

#include <span>
#include <vector>

// Terminal state → visible-row highlight spans, one gather per tier. These sit
// beside the geometry rather than inside it: `OverlayGeometry` is pure
// arithmetic with no Terminal in sight, and these are the reads that feed it —
// the search segments, the trigger ring and the hilite scalars, each mapped
// through the one absolute-to-visible formula `visibleCell` defines. Written
// per frontend, this mapping drifts on the cases that are hard to reach by
// hand.
//
// The out-vector shape: these run per frame, and a caller-owned vector keeps
// its capacity across frames where a returned value would allocate on each —
// the same measurement that keeps heap-carrying out-parameters elsewhere in
// Core.

namespace Ubiquitty::Rendering {

struct FrameDamage;

/// The selection a frame drew behind its text, kept for the next frame's
/// comparison.
///
/// A selection behind the text is each selected cell's background, so it is
/// in the retained pixels while no cell carries it: a drag, a recolour or a
/// cleared selection changes those pixels without a write, and only the spans
/// can say which rows.
struct BakedSelection {
    std::vector<SelectionRowSpan> spans;
    Vec4 color {};
};

/// Marks the rows a selection drawn behind the text owes a retained frame,
/// and records the selection the frame draws.
///
/// Every row of the selection the target holds and of the one this frame
/// draws, once anything differs between them — a span, a sub-cell cut, the
/// colour — or a scroll moved the held one with its rows; the held rows are
/// marked where they stood and where the scroll carried them. Nothing when
/// the two are the same selection, which is the steady frame under a held
/// selection.
///
/// - Parameters:
///   - held: The selection the target holds; replaced by this frame's.
///   - spans: The spans this frame draws behind the text, in visible rows;
///     empty for a selection drawn as an overlay, which is no row's.
///   - color: The fill they are drawn in.
///   - damage: This frame's drained scroll.
///   - rowDirty: The frame's dirty rows; entries past its end are not marked.
void markBakedSelectionRows(BakedSelection& held, std::span<const SelectionRowSpan> spans, Vec4 color,
    const FrameDamage& damage, std::span<std::uint8_t> rowDirty);

/// Collects the focused search match's per-row spans in visible-grid
/// coordinates.
///
/// One span per row the active match covers, selected by its match ID so a
/// match wrapping across rows carries its emphasis on every row rather than
/// the first. The builder bakes these into the cell background and takes the
/// glyph's ink from the same rule, so the emphasis is the cell rather than a
/// quad over it. Empty when no search is active.
///
/// - Parameters:
///   - terminal: The terminal whose search state is read.
///   - viewOffset: Scrollback rows above the viewport, which maps the match's
///     absolute row onto a visible one.
///   - outSpans: Cleared, then filled with the visible spans.
void collectSearchMatchRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans);

/// Collects the non-focused search matches' per-row spans in visible-grid
/// coordinates.
///
/// Every match segment whose match ID differs from the active one, so the
/// hits the user is not standing on locate without competing with the one
/// they are. The complement of ``collectSearchMatchRowSpans``, keyed on the same
/// match ID, so a segment cannot land in both lists and be drawn twice.
/// Empty when no search is active.
///
/// - Parameters:
///   - terminal: The terminal whose search state is read.
///   - viewOffset: Scrollback rows above the viewport.
///   - outSpans: Cleared, then filled with the visible spans.
void collectOtherSearchMatchRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans);

/// A caller-owned destination for one collector's spans.
///
/// The `std::vector` overloads own their storage and grow it, which a Swift
/// caller cannot express: an imported `std::vector&` is not a type Swift can
/// form, so an arm reaching Core through the interop seam has no way to call
/// them at all. A span it can — `Selection::ViewportSpans` is the same shape,
/// for the same reason, and the selection tier already crosses on it.
using OverlayRowSpans = std::span<SelectionRowSpan>;

/// Collects the non-focused search matches into a caller-owned span.
///
/// The same derivation as the vector overload, writing into storage the caller
/// sized. A row can contain multiple matches; `Screen::ScrollSearch::segmentCount`
/// bounds the required capacity. A shorter destination truncates the answer
/// rather than growing, because the caller owns the memory.
///
/// - Parameters:
///   - terminal: The terminal whose search state is read.
///   - viewOffset: Scrollback rows above the viewport.
///   - out: Caller-owned destination; its size bounds the answer.
/// - Returns: How many entries were written.
[[nodiscard]] int collectOtherSearchMatchRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out);

/// Collects the focused search match into a caller-owned span.
///
/// - Parameters:
///   - terminal: The terminal whose search state is read.
///   - viewOffset: Scrollback rows above the viewport.
///   - out: Caller-owned destination; its size bounds the answer.
/// - Returns: How many entries were written.
[[nodiscard]] int collectSearchMatchRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out);

/// Collects the trigger-action highlights into a caller-owned span.
///
/// - Parameters:
///   - terminal: The terminal whose trigger state is read.
///   - viewOffset: Scrollback rows above the viewport.
///   - out: Caller-owned destination; its size bounds the answer.
/// - Returns: How many entries were written.
[[nodiscard]] int collectTriggerHighlightRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out);

/// Resolves the DEC mode 1001 hilite band into a caller-owned span.
///
/// - Parameters:
///   - terminal: The terminal holding the band.
///   - viewOffset: Scrollback rows above the viewport.
///   - out: Caller-owned destination; its size bounds the answer.
/// - Returns: How many entries were written.
[[nodiscard]] int collectHiliteRegionRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out);

/// Collects the trigger-action highlights' per-row spans in visible-grid
/// coordinates.
///
/// Read from `Triggers::highlightAt`, the published accessor over Core's
/// fixed oldest-evicting ring, so no caller needs a view of the ring's shape.
/// Empty when no `highlight` trigger has fired.
///
/// - Parameters:
///   - terminal: The terminal whose trigger state is read.
///   - viewOffset: Scrollback rows above the viewport.
///   - outSpans: Cleared, then filled with the visible spans.
void collectTriggerHighlightRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans);

/// Resolves the DEC mode 1001 hilite band into visible-row spans.
///
/// The band's shape belongs to the scalar `hiliteRegionRowSpans` overload in
/// `OverlayGeometry`; this supplies the terminal's four scalars and nothing
/// else, so the region a host names is the same set of rows on every
/// frontend.
///
/// - Parameters:
///   - terminal: The terminal holding the band.
///   - viewOffset: Scrollback rows above the viewport.
///   - outSpans: Receives one span per covered row, cleared first and left
///     empty when the host has set no band.
void collectHiliteRegionRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans);

} // namespace Ubiquitty::Rendering
