// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Multicell.h"

#include "Core/CellPen.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Screen {

namespace {

bool isContinuation(const Ubiquitty::Cell* c)
{
    return c && Ubiquitty::cellPen(*c).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
}

bool isAnchorCell(const Ubiquitty::Cell* c)
{
    return c && Ubiquitty::cellPen(*c).attributes.contains(Ubiquitty::VT::Attribute::Wide);
}

} // namespace

MulticellRun multicellRunAt(const Ubiquitty::Terminal* t, int row, int column)
{
    MulticellRun run;
    if (!t || row < 0 || row >= t->rows || column < 0 || column >= t->columns)
        return run;

    // Walk left to the anchor. A continuation cell names no anchor of its own —
    // the grid stores membership by adjacency, so the leading cell is found by
    // stepping back over continuations rather than by following a link.
    int anchorColumn = column;
    while (anchorColumn > 0 && isContinuation(visibleCell(t, 0, row, anchorColumn)))
        --anchorColumn;

    if (!isAnchorCell(visibleCell(t, 0, row, anchorColumn)))
        return run;

    // A continuation whose leftward walk ended somewhere that is not an anchor
    // is an orphan, and the caller sees an invalid run rather than a run that
    // covers it — which is the property an orphan check wants to assert.
    int columns = 1;
    while (anchorColumn + columns < t->columns
        && isContinuation(visibleCell(t, 0, row, anchorColumn + columns)))
        ++columns;

    run.anchorRow = row;
    run.anchorColumn = anchorColumn;
    run.span.columns = columns;
    run.span.rows = 1;
    return run;
}

} // namespace Ubiquitty::Screen
