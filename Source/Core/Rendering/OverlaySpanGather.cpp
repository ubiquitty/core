// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OverlaySpanGather.h"

#include "Core/Rendering/FrameDamage.h"

#include <algorithm>

#include "Core/Screen/Addressing.h"

#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Triggers/Triggers.h"

namespace Ubiquitty::Rendering {

void markBakedSelectionRows(BakedSelection& held, std::span<const SelectionRowSpan> spans, Vec4 color,
    const FrameDamage& damage, std::span<std::uint8_t> rowDirty)
{
    const std::span<const SelectionRowSpan> drawn = spans;
    const auto isSameSpan = [](const SelectionRowSpan& a, const SelectionRowSpan& b) { return a == b; };
    const bool isSameColor = held.color.x == color.x && held.color.y == color.y && held.color.z == color.z
        && held.color.w == color.w;
    const bool isUnchanged = std::ranges::equal(held.spans, drawn, isSameSpan)
        && (drawn.empty() || isSameColor) && (damage.scrollDelta == 0 || held.spans.empty());
    if (!isUnchanged) {
        const int rows = static_cast<int>(rowDirty.size());
        const auto mark = [&](int row) {
            if (row >= 0 && row < rows)
                rowDirty[static_cast<std::size_t>(row)] = 1;
        };
        for (const SelectionRowSpan& span : drawn)
            mark(span.visibleRow);
        for (const SelectionRowSpan& span : held.spans) {
            mark(span.visibleRow);
            if (damage.scrollDelta != 0 && span.visibleRow >= damage.scrollTop && span.visibleRow <= damage.scrollBottom)
                mark(span.visibleRow - damage.scrollDelta);
        }
    }
    held.spans.assign(drawn.begin(), drawn.end());
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

} // namespace Ubiquitty::Rendering
