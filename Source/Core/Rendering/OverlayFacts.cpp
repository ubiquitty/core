// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OverlayFacts.h"

#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/Triggers/Triggers.h"

#include <algorithm>
#include <span>

namespace Ubiquitty::Rendering {

static_assert(maxTriggerOverlaySegments == Terminal::triggerHighlightCapacity);

void gatherSearchOverlayFacts(const Terminal& terminal, SearchOverlayFacts& out)
{
    out.numbering = {};
    out.activeMatchEpoch = 0;
    out.focusedCount = 0;
    out.otherCount = 0;
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(terminal);
    if (!active || terminal.rows <= 0)
        return;
    out.numbering = Screen::rowNumbering(&terminal);
    out.activeMatchEpoch = Screen::ScrollSearch::activeMatchEpoch(terminal);
    const int activeID = active->matchID;
    const int count = Screen::ScrollSearch::segmentCount(terminal);
    const auto capacity = static_cast<int>(out.segments.size());
    // Two walks, so the focused segments lead without a second buffer; the
    // projection bounds `count` by the array's own capacity.
    int written = 0;
    for (const bool wantsFocused : { true, false }) {
        for (int index = 0; index < count && written < capacity; ++index) {
            const Screen::ScrollSearch::Match* match = Screen::ScrollSearch::segmentAt(terminal, index);
            if (!match || match->length <= 0 || (match->matchID == activeID) != wantsFocused)
                continue;
            out.segments[static_cast<std::size_t>(written++)] = { match->absoluteRow, match->column, match->column + match->length - 1 };
        }
        if (wantsFocused)
            out.focusedCount = written;
    }
    out.otherCount = written - out.focusedCount;
}

void gatherMarkOverlayFacts(const Terminal& terminal, MarkOverlayFacts& out)
{
    out.triggerCount = 0;
    const int count = std::min(Triggers::highlightCount(&terminal), maxTriggerOverlaySegments);
    for (int index = 0; index < count; ++index) {
        OverlaySegment segment;
        if (Triggers::highlightAt(&terminal, index, &segment.absoluteRow, &segment.column0, &segment.column1))
            out.triggers[static_cast<std::size_t>(out.triggerCount++)] = segment;
    }
    out.isHiliteValid = terminal.modes.hiliteRegionValid;
    out.hiliteStartColumn = out.isHiliteValid ? terminal.modes.hiliteStartX : 0;
    out.hiliteStartRow = out.isHiliteValid ? terminal.modes.hiliteStartY : 0;
    out.hiliteFirstRow = out.isHiliteValid ? terminal.modes.hiliteFirstRow : 0;
    out.hiliteLastRow = out.isHiliteValid ? terminal.modes.hiliteLastRow : 0;
}

bool hasSameContent(const SearchOverlayFacts& left, const SearchOverlayFacts& right)
{
    const auto used = [](const SearchOverlayFacts& facts) {
        return std::span(facts.segments).first(static_cast<std::size_t>(facts.focusedCount + facts.otherCount));
    };
    return left.numbering == right.numbering && left.activeMatchEpoch == right.activeMatchEpoch
        && left.focusedCount == right.focusedCount && left.otherCount == right.otherCount
        && std::ranges::equal(used(left), used(right));
}

bool hasSameContent(const MarkOverlayFacts& left, const MarkOverlayFacts& right)
{
    const auto used = [](const MarkOverlayFacts& facts) {
        return std::span(facts.triggers).first(static_cast<std::size_t>(facts.triggerCount));
    };
    return left.triggerCount == right.triggerCount && std::ranges::equal(used(left), used(right))
        && left.isHiliteValid == right.isHiliteValid && left.hiliteStartColumn == right.hiliteStartColumn
        && left.hiliteStartRow == right.hiliteStartRow && left.hiliteFirstRow == right.hiliteFirstRow
        && left.hiliteLastRow == right.hiliteLastRow;
}

} // namespace Ubiquitty::Rendering
