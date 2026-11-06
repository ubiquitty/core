// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Selection/Selection.h"
#include <span>

// Under Selection/ because it reads the selection model, and the layer rule runs
// one way: a behaviour module may depend on a model one and not the reverse. The
// spans it produces are a render input, so the output type stays Rendering's.

namespace Ubiquitty::Selection {

/// The viewport a selection is being drawn into.
///
/// Selection rows live in absolute buffer space — scrollback plus screen — and
/// the renderer draws viewport rows, so every span needs both. One struct
/// because the three travel together and a caller that passes the row count of
/// one grid with the origin of another produces spans that are individually
/// well-formed and collectively wrong.
struct Viewport {
    /// Visible rows.
    int rows = 0;
    /// Grid columns.
    int columns = 0;
    /// The absolute buffer row currently at the top of the viewport.
    ///
    /// `history - viewOffset` for a caller holding those two separately. On the
    /// alternate screen `history` is zero rather than the primary's surviving
    /// scrollback count — see ``selectionHistoryRows``, which answers it — and a
    /// caller that passes the raw count shifts every highlight off the pointer
    /// by that many rows.
    int firstAbsoluteRow = 0;
};

/// Where the collected spans land: the caller's storage, never grown.
///
/// Named rather than spelled `std::span<SelectionRowSpan>` because the Swift
/// importer has no spelling for the extent template parameter and drops every
/// function that names one, with no diagnostic.
using ViewportSpans = std::span<Rendering::SelectionRowSpan>;

/// Collects one span per selected visible row.
///
/// The mapping between the two row spaces is the whole of this function, and it
/// is the part a frontend gets wrong quietly: a selection is anchored in
/// absolute rows that survive scrolling, and a renderer paints viewport rows
/// that do not. Written per frontend, it drifts on the cases that are hard to
/// reach by hand — scrolled-back selections and the alternate screen.
///
/// The walk is over the intersection of the selection and the viewport rather
/// than over either whole, so a selection spanning a large scrollback stays
/// proportional to what is on screen.
///
/// Resolving `bounds` stays with the caller. A frontend that understands wide
/// cells resolves them with ``Selection::getBoundsWideWith`` and one that does
/// not uses ``Selection::getBounds``; that is a real difference in what a
/// frontend supports rather than a decision this should be making for it.
///
/// - Parameters:
///   - bounds: The selection's absolute-row extent, already resolved.
///   - mode: Character, word, line or rectangular.
///   - viewport: The grid being drawn, and where it sits in the buffer.
///   - cellAt: Reads a code point at an absolute row and column. Off-screen
///     reads answer zero, which stops a word expansion at the viewport edge.
///   - cellContext: Handed to `cellAt`.
///   - attributesAt: Reads a cell's raw attributes, for the per-row rules that
///     depend on them.
///   - attributeContext: Handed to `attributesAt`.
///   - out: Receives the spans. At most one per visible row, so a buffer of
///     `viewport.rows` cannot overflow; a shorter one stops early and the
///     return value says how many fit.
///   - lineAttributeAt: Reads a row's DEC line attribute at an absolute row,
///     or null to read every row as single-height. Both halves of a
///     double-height pair show the same glyphs, so the top half's span is
///     the pair's: the bottom half shows the same columns — its own span,
///     where the bounds run through it as a middle row, says nothing about
///     the glyphs and is replaced — and a bottom half selected with its top
///     half off the viewport lights the top half where it can. A mirror
///     lands only on a row without a span, so the one-per-row bound holds.
///   - lineAttributeContext: Handed to `lineAttributeAt`.
/// - Returns: How many spans were written, in row order.
[[nodiscard]] int collectViewportSpans(
    const SelectionBounds& bounds,
    SelectionMode mode,
    const Viewport& viewport,
    SelectionCellAt cellAt,
    void* cellContext,
    SelectionAttributesAt attributesAt,
    void* attributeContext,
    ViewportSpans out,
    SelectionRowLineAttributeAt lineAttributeAt = nullptr,
    void* lineAttributeContext = nullptr);

} // namespace Ubiquitty::Selection
