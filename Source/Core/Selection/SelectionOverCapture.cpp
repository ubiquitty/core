// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Selection/SelectionOverCapture.h"

#include "Core/Screen/Addressing.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace Ubiquitty::Selection {

namespace {

// The hand-off crosses threads as bytes, and is compared as bytes: a
// field-wise comparison reads an unset pointer coordinate, a NaN, as
// differing from itself.
static_assert(std::is_trivially_copyable_v<Session>);

template<typename Value>
void copyBytes(Value& into, const Value& from) noexcept
{
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memcpy(static_cast<void*>(&into), static_cast<const void*>(&from), sizeof(Value));
}

// The view's position over the terminal: what output transitions and
// resizes move, and what resolving a selection never reads or writes.
void keepViewPosition(Session& into, const Session& from) noexcept
{
    copyBytes(into.viewportObservedHistory, from.viewportObservedHistory);
    copyBytes(into.hasViewportObservation, from.hasViewportObservation);
    copyBytes(into.viewportAnchor, from.viewportAnchor);
    copyBytes(into.viewportHistoryEpoch, from.viewportHistoryEpoch);
    copyBytes(into.usesLogicalViewport, from.usesLogicalViewport);
}

// Whether `view` holds what `handedOff` held, the view's position aside.
// Padding a write of the view's changed reads as a difference, which costs
// one hand-off and nothing else.
[[nodiscard]] bool isSameSelection(const Session& view, const Session& handedOff) noexcept
{
    Session comparable;
    copyBytes(comparable, view);
    keepViewPosition(comparable, handedOff);
    return std::memcmp(static_cast<const void*>(&comparable), static_cast<const void*>(&handedOff), sizeof(Session)) == 0;
}

[[nodiscard]] bool haveEndsMoved(const SelectionState& before, const SelectionState& after) noexcept
{
    return before.anchorRow != after.anchorRow || before.anchorColumn != after.anchorColumn || before.headRow != after.headRow
        || before.headColumn != after.headColumn;
}

} // namespace

int resolveForCapture(PublishedSelection& selection, Terminal& terminal, int viewOffset, CapturedSelection& out, BlockSelections blocks) noexcept
{
    Session& session = selection.session;
    if (selection.generation != 0)
        ++selection.resolution;
    out.generation = selection.generation;
    out.resolution = selection.resolution;
    out.outcome = CaptureOutcome::Kept;
    out.ends = {};
    out.spanCount = 0;
    int blockCount = 0;
    const bool hadSelection = Selection::hasSelection(&session.state);
    // Nothing selected and no gesture open leaves the live read nothing to
    // draw and nothing to move, so a frame of a pane without a selection
    // pays nothing here.
    if (selection.generation != 0 && (hadSelection || session.gesture.isOpen)) {
        const SelectionState before = session.state;
        const auto capacity = static_cast<std::size_t>(viewportSpanCapacity(terminal.rows, terminal.columns));
        if (out.spans.size() < capacity)
            out.spans.resize(capacity);
        out.spanCount = viewportSpansAndEnds(session, terminal, Screen::resolvedViewOffset(&terminal, viewOffset),
            ViewportSpans(out.spans).first(capacity), out.ends);
        if (!terminal.richTextBlocks.isEmpty())
            blockCount = blockSelections(session, terminal, blocks);
        if (hadSelection && !Selection::hasSelection(&session.state))
            out.outcome = CaptureOutcome::Lost;
        else if (hadSelection && haveEndsMoved(before, session.state))
            out.outcome = CaptureOutcome::Moved;
    }
    copyBytes(out.session, session);
    return blockCount;
}

bool prepareHandOff(const Session& selection, PublishedSelection& handedOff) noexcept
{
    if (handedOff.generation != 0 && isSameSelection(selection, handedOff.session))
        return false;
    ++handedOff.generation;
    handedOff.resolution = 0;
    copyBytes(handedOff.session, selection);
    return true;
}

CaptureSpans takeCapturedSelection(Session& selection, PublishedSelection& handedOff, const CapturedSelection& captured,
    ViewportSpans out, ViewportEnds& ends) noexcept
{
    const std::size_t count = std::min({ static_cast<std::size_t>(std::max(captured.spanCount, 0)), captured.spans.size(), out.size() });
    std::copy_n(captured.spans.begin(), count, out.begin());
    ends = captured.ends;
    const bool canAdopt = captured.generation != 0 && captured.generation == handedOff.generation
        && captured.resolution > handedOff.resolution && isSameSelection(selection, handedOff.session);
    if (canAdopt) {
        Session resolved;
        copyBytes(resolved, captured.session);
        keepViewPosition(resolved, selection);
        copyBytes(selection, resolved);
        copyBytes(handedOff.session, resolved);
        handedOff.resolution = captured.resolution;
    }
    return { static_cast<int>(count), captured.outcome, canAdopt };
}

} // namespace Ubiquitty::Selection
