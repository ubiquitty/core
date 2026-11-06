// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ScrollSearchResults.cpp — the readers of a published search result.
//
// A unit of its own because the readers need nothing but the state the
// header defines, while the search that publishes it needs RE2 and Unicode
// normalisation. A frame capture reads the active match through these to
// damage its highlight, so an arm that captures frames and runs no search
// (the headless agent) links them without the matcher.

#include "Core/Screen/ScrollSearch.h"

#include "Core/CellPen.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <algorithm>

namespace Ubiquitty::Screen::ScrollSearch {

namespace {

// Resolves one hit into row segments: from its first character over its
// written cells, across soft wraps, and over the trailing half of a wide
// character it ends on.
//
// - Returns: Whether the hit resolved and every segment fit.
bool projectHit(const Ubiquitty::Terminal& t, const Ubiquitty::Search::Hit& hit, ResultProjection& state, int matchID)
{
    const auto place = resolvePosition(t, hit.start);
    if (!place)
        return false;
    std::uint32_t remaining = hit.endCell > hit.start.cell ? hit.endCell - hit.start.cell : 1;
    const int total = scrollbackCount(&t) + t.rows;
    // A position counts rows on its own buffer, and the alternate buffer has
    // no history; a segment counts them as `Match` does, after the primary's
    // history on either screen, which is also how the rows read below count.
    const int historyBefore = hit.start.buffer == Buffer::Alternate ? scrollbackCount(&t) : 0;
    int row = place->absoluteRow + historyBefore;
    int column = place->column;
    const int firstSegment = state.segmentCount;
    while (remaining > 0 && row < total) {
        int length = 0;
        const Cell* cells = absoluteRowCells(&t, row, &length);
        const int start = column;
        while (remaining > 0 && cells && column < length) {
            if (!isUnwrittenCell(cells[column]))
                --remaining;
            ++column;
        }
        while (cells && column < length && Ubiquitty::isTextContinuation(cells[column]))
            ++column;
        if (column > start) {
            if (state.segmentCount >= scrollSearchMaxSegments) {
                state.segmentCount = firstSegment;
                return false;
            }
            state.segments[state.segmentCount++] = { .absoluteRow = row, .column = start, .length = column - start, .matchID = matchID };
        }
        if (remaining == 0 || !absoluteRowWrapped(&t, row))
            break;
        ++row;
        column = 0;
    }
    return state.segmentCount > firstSegment;
}

// Resolves the logical hits' segments in the numbering the terminal has now.
void project(const Ubiquitty::Terminal& t, ResultProjection& state)
{
    LogicalProjection& logical = *state.logical;
    state.segmentCount = 0;
    state.logicalCount = 0;
    state.activeIndex = -1;
    // Segments are stored oldest first; the logical hit window runs newest first.
    for (std::size_t index = logical.hits.size(); index-- > 0;) {
        if (state.logicalCount >= scrollSearchMaxMatches)
            break;
        const int first = state.segmentCount;
        if (!projectHit(t, logical.hits[index], state, state.logicalCount))
            continue;
        if (static_cast<int>(index) == logical.activeIndex)
            state.activeIndex = state.logicalCount;
        state.firstSegment[state.logicalCount++] = first;
    }
    // The readers count the active match newest first.
    if (state.activeIndex >= 0)
        state.activeIndex = state.logicalCount - 1 - state.activeIndex;
    state.valid = true;
    logical.absoluteRowEpoch = t.absoluteRowEpoch;
    logical.modelEpoch = t.modelEpoch;
    logical.historyRows = scrollbackCount(&t);
    logical.isAlternate = t.screens.isUsingAlternateScreen;
}

} // namespace

// Every reader holds the model lock, so the one that finds the rows renumbered
// since the segments were resolved resolves them again in place.
static const ResultProjection* stateOf(const Ubiquitty::Terminal* t)
{
    ResultProjection* state = t ? t->scrollSearchState.get() : nullptr;
    if (!state || !state->logical)
        return state;
    const LogicalProjection& logical = *state->logical;
    if (logical.absoluteRowEpoch != t->absoluteRowEpoch || logical.modelEpoch != t->modelEpoch || logical.historyRows != scrollbackCount(t)
        || logical.isAlternate != t->screens.isUsingAlternateScreen)
        project(*t, *state);
    return state;
}

void clear(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    t->scrollSearchState.reset();
}

void publishLogicalHits(Ubiquitty::Terminal& t, std::span<const Ubiquitty::Search::Hit> hits, int activeIndex, std::uint32_t activeEpoch)
{
    if (hits.empty()) {
        clear(&t);
        return;
    }
    if (!t.scrollSearchState)
        t.scrollSearchState = std::make_unique<ResultProjection>();
    ResultProjection& state = *t.scrollSearchState;
    // The window of hits around the active one that the state can hold.
    const auto capacity = static_cast<std::size_t>(scrollSearchMaxMatches);
    std::size_t first = 0;
    if (hits.size() > capacity && activeIndex >= 0)
        first = std::min(static_cast<std::size_t>(std::max(activeIndex - static_cast<int>(capacity / 2), 0)), hits.size() - capacity);
    const std::size_t count = std::min(hits.size() - first, capacity);
    if (!state.logical)
        state.logical = std::make_unique<LogicalProjection>();
    state.logical->hits.assign(hits.begin() + static_cast<std::ptrdiff_t>(first), hits.begin() + static_cast<std::ptrdiff_t>(first + count));
    state.logical->activeIndex = activeIndex >= 0 ? activeIndex - static_cast<int>(first) : -1;
    state.activeEpoch = activeEpoch;
    project(t, state);
}

// Segments are stored oldest first; public indices count newest first.
static int storedIndexOf(int searchOrderIndex, int logicalCount)
{
    return logicalCount - 1 - searchOrderIndex;
}

const Match* activeMatch(const Ubiquitty::Terminal& t)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid || st->activeIndex < 0)
        return nullptr;
    return &st->segments[st->firstSegment[storedIndexOf(st->activeIndex, st->logicalCount)]];
}

int matchCount(const Ubiquitty::Terminal& t)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return 0;
    return st->logicalCount;
}

const Match* matchAt(const Ubiquitty::Terminal& t, int index)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return nullptr;
    if (index < 0 || index >= st->logicalCount)
        return nullptr;
    return &st->segments[st->firstSegment[storedIndexOf(index, st->logicalCount)]];
}

MatchList matches(const Ubiquitty::Terminal& t)
{
    MatchList out;
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return out;
    out.reserve(static_cast<std::size_t>(st->logicalCount));
    for (int index = 0; index < st->logicalCount; ++index)
        out.push_back(st->segments[st->firstSegment[storedIndexOf(index, st->logicalCount)]]);
    return out;
}

int segmentCount(const Ubiquitty::Terminal& t)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return 0;
    return st->segmentCount;
}

const Match* segmentAt(const Ubiquitty::Terminal& t, int index)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return nullptr;
    if (index < 0 || index >= st->segmentCount)
        return nullptr;
    return &st->segments[index];
}

int activeMatchIndex(const Ubiquitty::Terminal& t)
{
    const ResultProjection* st = stateOf(&t);
    if (!st || !st->valid)
        return -1;
    if (st->activeIndex < 0 || st->activeIndex >= st->logicalCount)
        return -1;
    return st->activeIndex;
}

} // namespace Ubiquitty::Screen::ScrollSearch
