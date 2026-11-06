// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/HistoryLines.h"

#include "Core/Cell.h"
#include "Core/Color.h"
#include "Core/LineStore.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Terminal.h"

#include <algorithm>

namespace Ubiquitty::Screen {

HistoryAddress historyAddress(const Ubiquitty::Terminal& terminal)
{
    HistoryAddress address;
    address.epoch = terminal.historyEpoch;
    address.clearedThrough = terminal.historyEpochBase;
    if (terminal.lineStore) {
        address.oldest = terminal.lineStore->frontLineNumber();
        address.head = terminal.lineStore->headLineNumber();
    } else {
        address.oldest = terminal.historyEpochBase;
        address.head = terminal.historyEpochBase;
    }
    return address;
}

std::optional<Ubiquitty::LineStore::LogicalLineRows> logicalLineRows(const Ubiquitty::Terminal& terminal, std::uint64_t number)
{
    if (!terminal.lineStore || number < terminal.historyEpochBase)
        return std::nullopt;
    return terminal.lineStore->logicalLine(number);
}

std::optional<Ubiquitty::LineStore::LogicalLineRows> openLineRows(const Ubiquitty::Terminal& terminal)
{
    return terminal.lineStore ? terminal.lineStore->openLine() : std::nullopt;
}

bool isUnwrittenCell(const Ubiquitty::Cell& cell)
{
    // The reflow drops the same cells at a join.
    return cell.codePoint == 0 && cell.pen.attributes.isEmpty() && cell.pen.foregroundIndex == Ubiquitty::colorDefault
        && cell.pen.backgroundIndex == Ubiquitty::colorDefault;
}

int writtenLength(const ScrollLine& row)
{
    int length = std::max(row.length, 0);
    while (length > 0 && isUnwrittenCell(row.cells[length - 1]))
        --length;
    return length;
}

bool hasNumberedHistoryInEpoch(const Ubiquitty::Terminal& terminal)
{
    return terminal.lineStore && terminal.lineStore->maxHeadLineNumber() > terminal.historyEpochBase;
}

void retireHistory(Ubiquitty::Terminal& terminal)
{
    // An epoch no line was numbered in has nothing a client could hold, so a
    // clear repeated at an empty prompt opens none and costs no frame.
    if (!hasNumberedHistoryInEpoch(terminal))
        return;
    ++terminal.historyEpoch;
    // Every number the old epoch reached is below the new base, including the
    // ones a pull-back withdrew: a client may still hold those lines.
    const std::uint64_t base = std::max(terminal.lineStore->maxHeadLineNumber(), terminal.historyEpochBase);
    // The survivors keep their names through the renumbering, and the lines
    // still to complete take names above every one handed out: a cleared
    // line's number is about to go to one of them.
    notePositionsRenumbered(terminal, base);
    terminal.historyEpochBase = base;
    terminal.lineStore->restartNumbering(base);
    startFreshLineNames(terminal);
}

} // namespace Ubiquitty::Screen
