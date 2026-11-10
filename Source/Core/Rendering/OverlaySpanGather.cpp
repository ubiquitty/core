// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OverlaySpanGather.h"

#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/FrameDamage.h"

#include <algorithm>

#include "Core/Screen/Addressing.h"

#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Triggers/Triggers.h"

namespace Ubiquitty::Rendering {

void markSelectionRepaintRows(DrawnSelectionSpans heldSpans, Vec4 heldColor, DrawnSelectionSpans spans, Vec4 color,
    const FrameDamage& damage, RowBytesToWrite rowDirty)
{
    const auto isSameSpan = [](const SelectionRowSpan& a, const SelectionRowSpan& b) { return a == b; };
    const bool isSameColor = heldColor.x == color.x && heldColor.y == color.y && heldColor.z == color.z
        && heldColor.w == color.w;
    const bool isUnchanged = std::ranges::equal(heldSpans, spans, isSameSpan)
        && (spans.empty() || isSameColor) && (damage.scrollDelta == 0 || heldSpans.empty());
    if (isUnchanged)
        return;
    const int rows = static_cast<int>(rowDirty.size());
    const auto mark = [&](int row) {
        if (row >= 0 && row < rows)
            rowDirty[static_cast<std::size_t>(row)] = 1;
    };
    for (const SelectionRowSpan& span : spans)
        mark(span.visibleRow);
    for (const SelectionRowSpan& span : heldSpans) {
        mark(span.visibleRow);
        if (damage.scrollDelta != 0 && span.visibleRow >= damage.scrollTop && span.visibleRow <= damage.scrollBottom)
            mark(span.visibleRow - damage.scrollDelta);
    }
}

void markBakedSelectionRows(BakedSelection& held, std::span<const SelectionRowSpan> spans, Vec4 color,
    const FrameDamage& damage, std::span<std::uint8_t> rowDirty)
{
    markSelectionRepaintRows(held.spans, held.color, spans, color, damage, rowDirty);
    held.spans.assign(spans.begin(), spans.end());
    held.color = color;
}

void collectSearchMatchRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans)
{
    outSpans.clear();
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(terminal);
    if (!active)
        return;
    const int rows = terminal.rows;
    if (rows <= 0)
        return;
    // absoluteRow lives in the scrollback + live address space, so it converts
    // through the inversion visibleCell uses: visible = absolute - history + viewOffset.
    const int history = Screen::scrollbackCount(&terminal);
    viewOffset = Screen::resolvedViewOffset(&terminal, viewOffset);
    const int count = Screen::ScrollSearch::segmentCount(terminal);
    for (int i = 0; i < count; ++i) {
        const Screen::ScrollSearch::Match* m = Screen::ScrollSearch::segmentAt(terminal, i);
        // Only the active match bakes; the others carry no emphasis on this
        // path. Selecting on matchID rather than on the segment index is what
        // keeps a match that wraps across rows emphasised on every row it
        // covers rather than on the first.
        if (!m || m->matchID != active->matchID || m->length <= 0)
            continue;
        const int visibleRow = Screen::visibleRowForAbsolute(history, viewOffset, m->absoluteRow);
        if (visibleRow < 0 || visibleRow >= rows)
            continue;
        outSpans.push_back({ visibleRow, m->column, m->column + m->length - 1 });
    }
}

void collectOtherSearchMatchRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans)
{
    outSpans.clear();
    const int rows = terminal.rows;
    if (rows <= 0)
        return;
    // No active match means no search is running, so there is no "other" either:
    // the segment list is stale rather than a set of unfocused hits.
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(terminal);
    if (!active)
        return;
    const int history = Screen::scrollbackCount(&terminal);
    viewOffset = Screen::resolvedViewOffset(&terminal, viewOffset);
    const int count = Screen::ScrollSearch::segmentCount(terminal);
    for (int i = 0; i < count; ++i) {
        const Screen::ScrollSearch::Match* m = Screen::ScrollSearch::segmentAt(terminal, i);
        // The complement of searchMatchRowSpans, keyed on the same match ID, so
        // a segment cannot land in both lists and be drawn twice.
        if (!m || m->matchID == active->matchID || m->length <= 0)
            continue;
        const int visibleRow = Screen::visibleRowForAbsolute(history, viewOffset, m->absoluteRow);
        if (visibleRow < 0 || visibleRow >= rows)
            continue;
        outSpans.push_back({ visibleRow, m->column, m->column + m->length - 1 });
    }
}

void collectTriggerHighlightRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans)
{
    outSpans.clear();
    const int rows = terminal.rows;
    if (rows <= 0)
        return;
    // Through the published accessor rather than the ring itself: the ring
    // evicts oldest and carries a head index, and a caller indexing it
    // directly would read the raw array in commit order rather than in age
    // order the first time it wrapped.
    const int history = Screen::scrollbackCount(&terminal);
    viewOffset = Screen::resolvedViewOffset(&terminal, viewOffset);
    const int count = Triggers::highlightCount(&terminal);
    for (int i = 0; i < count; ++i) {
        int absoluteRow = 0;
        int column0 = 0;
        int column1 = 0;
        if (!Triggers::highlightAt(&terminal, i, &absoluteRow, &column0, &column1))
            continue;
        const int visibleRow = Screen::visibleRowForAbsolute(history, viewOffset, absoluteRow);
        if (visibleRow < 0 || visibleRow >= rows)
            continue;
        outSpans.push_back({ visibleRow, column0, column1 });
    }
}

void collectHiliteRegionRowSpans(const Terminal& terminal, int viewOffset, std::vector<SelectionRowSpan>& outSpans)
{
    outSpans.clear();
    if (!terminal.modes.hiliteRegionValid)
        return;
    const int rows = terminal.rows;
    if (rows <= 0)
        return;
    // The band's shape is the scalar overload's: the host sends four 1-based
    // scalars and one function turns them into rows for every renderer.
    outSpans.resize(static_cast<std::size_t>(rows));
    const int count = hiliteRegionRowSpans(terminal.modes.hiliteStartX, terminal.modes.hiliteStartY,
        terminal.modes.hiliteFirstRow, terminal.modes.hiliteLastRow, rows, terminal.columns, viewOffset,
        outSpans.data(), rows);
    outSpans.resize(static_cast<std::size_t>(count));
}

namespace {

/// Copies a collector's vector answer into a caller-owned span.
///
/// The four span overloads differ only in which collector fills the scratch, so
/// the copy is written once. The scratch is `thread_local` because these run on
/// the frame path and a per-call vector would allocate every frame; each arm
/// drives its own thread, and the value never outlives the call.
///
/// - Parameters:
///   - scratch: The vector a collector just filled.
///   - out: The caller's destination.
/// - Returns: How many entries were copied, bounded by `out.size()`.
int copyIntoSpan(const std::vector<SelectionRowSpan>& scratch, OverlayRowSpans out)
{
    const std::size_t written = std::min(scratch.size(), out.size());
    std::copy_n(scratch.begin(), written, out.begin());
    return static_cast<int>(written);
}

} // namespace

int collectOtherSearchMatchRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out)
{
    static thread_local std::vector<SelectionRowSpan> scratch;
    collectOtherSearchMatchRowSpans(terminal, viewOffset, scratch);
    return copyIntoSpan(scratch, out);
}

int collectSearchMatchRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out)
{
    static thread_local std::vector<SelectionRowSpan> scratch;
    collectSearchMatchRowSpans(terminal, viewOffset, scratch);
    return copyIntoSpan(scratch, out);
}

int collectTriggerHighlightRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out)
{
    static thread_local std::vector<SelectionRowSpan> scratch;
    collectTriggerHighlightRowSpans(terminal, viewOffset, scratch);
    return copyIntoSpan(scratch, out);
}

int collectHiliteRegionRowSpans(const Terminal& terminal, int viewOffset, OverlayRowSpans out)
{
    static thread_local std::vector<SelectionRowSpan> scratch;
    collectHiliteRegionRowSpans(terminal, viewOffset, scratch);
    return copyIntoSpan(scratch, out);
}

CapturedRows capturedRows(const FrameCapture& capture) noexcept UBIQUITTY_NONBLOCKING
{
    return { .rows = capture.rows(), .columns = capture.columns(), .viewOffset = capture.viewOffset(), .numbering = capture.rowNumbering() };
}

namespace {

// The absolute row of the capture's top visible row, as `visibleCell` maps it.
[[nodiscard]] int firstVisibleAbsoluteRow(const CapturedRows& captured) noexcept
{
    return captured.numbering.historyRows - captured.viewOffset;
}

enum class SearchTier : std::uint8_t {
    Focused,
    Other,
};

int searchSpansOverCapture(const SearchOverlayFacts& facts, const CapturedRows& captured, SearchTier tier, OverlayRowSpans out) noexcept UBIQUITTY_NONBLOCKING
{
    if (captured.rows <= 0 || out.empty())
        return 0;
    const int focusedCount = std::clamp(facts.focusedCount, 0, static_cast<int>(facts.segments.size()));
    const int otherCount = std::clamp(facts.otherCount, 0, static_cast<int>(facts.segments.size()) - focusedCount);
    const int first = tier == SearchTier::Focused ? 0 : focusedCount;
    const int end = tier == SearchTier::Focused ? focusedCount : focusedCount + otherCount;
    if (first == end)
        return 0;
    const int top = firstVisibleAbsoluteRow(captured);
    int written = 0;
    for (int index = first; index < end && written < static_cast<int>(out.size()); ++index) {
        const OverlaySegment& segment = facts.segments[static_cast<std::size_t>(index)];
        int row = 0;
        if (!Screen::translateAbsoluteRow(segment.absoluteRow, facts.numbering, captured.numbering, row))
            return 0;
        const int visibleRow = row - top;
        if (visibleRow < 0 || visibleRow >= captured.rows)
            continue;
        out[static_cast<std::size_t>(written++)] = { visibleRow, segment.column0, segment.column1 };
    }
    return written;
}

} // namespace

int focusedSearchSpansOverCapture(const SearchOverlayFacts& facts, const CapturedRows& captured, OverlayRowSpans out) noexcept UBIQUITTY_NONBLOCKING
{
    return searchSpansOverCapture(facts, captured, SearchTier::Focused, out);
}

int otherSearchSpansOverCapture(const SearchOverlayFacts& facts, const CapturedRows& captured, OverlayRowSpans out) noexcept UBIQUITTY_NONBLOCKING
{
    return searchSpansOverCapture(facts, captured, SearchTier::Other, out);
}

int triggerSpansOverCapture(const MarkOverlayFacts& facts, const CapturedRows& captured, OverlayRowSpans out) noexcept UBIQUITTY_NONBLOCKING
{
    if (captured.rows <= 0)
        return 0;
    const int top = firstVisibleAbsoluteRow(captured);
    const int count = std::clamp(facts.triggerCount, 0, static_cast<int>(facts.triggers.size()));
    int written = 0;
    for (int index = 0; index < count && written < static_cast<int>(out.size()); ++index) {
        const OverlaySegment& segment = facts.triggers[static_cast<std::size_t>(index)];
        const int visibleRow = segment.absoluteRow - top;
        if (visibleRow < 0 || visibleRow >= captured.rows)
            continue;
        out[static_cast<std::size_t>(written++)] = { visibleRow, segment.column0, segment.column1 };
    }
    return written;
}

int hiliteSpansOverCapture(const MarkOverlayFacts& facts, const CapturedRows& captured, OverlayRowSpans out) noexcept UBIQUITTY_NONBLOCKING
{
    if (!facts.isHiliteValid || captured.rows <= 0 || out.empty())
        return 0;
    return hiliteRegionRowSpans(facts.hiliteStartColumn, facts.hiliteStartRow, facts.hiliteFirstRow, facts.hiliteLastRow,
        captured.rows, captured.columns, captured.viewOffset, out.data(), static_cast<int>(out.size()));
}

OverlaySpanCounts overlaySpansOverCapture(const SearchOverlayFacts& search, const MarkOverlayFacts& marks,
    const FrameCapture& capture, OverlayRowSpans focusedSearch, OverlayRowSpans otherSearch, OverlayRowSpans triggers,
    OverlayRowSpans hilite) noexcept UBIQUITTY_NONBLOCKING
{
    const CapturedRows captured = capturedRows(capture);
    return {
        .focusedSearch = focusedSearchSpansOverCapture(search, captured, focusedSearch),
        .otherSearch = otherSearchSpansOverCapture(search, captured, otherSearch),
        .triggers = triggerSpansOverCapture(marks, captured, triggers),
        .hilite = hiliteSpansOverCapture(marks, captured, hilite),
        .searchGeneration = search.generation,
        .markGeneration = marks.generation,
        .activeMatchEpoch = search.activeMatchEpoch,
    };
}

} // namespace Ubiquitty::Rendering
