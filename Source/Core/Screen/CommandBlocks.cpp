// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// CommandBlocks.cpp — walking the OSC 133 marks into a list of commands.
//
// Every primitive the walk reads is elsewhere: the marks in Marks.h, the region
// rule in Addressing.h, the row text in Addressing.cpp. What is here is the
// order they are read in and what is done with the answers, which is the part
// two frontends cannot agree on by reading the same header.
//
// Off every hot path: this runs when a user opens a palette, not per byte and
// not per frame, so it allocates a string per block and scans the whole buffer
// once rather than caching anything.

#include "Core/Screen/CommandBlocks.h"

#include "Core/Screen/Addressing.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <cstdint>
#include <string>
#include <utility>

namespace Ubiquitty::Screen {

namespace {

// The absolute row count a block walk addresses.
//
// History counts as zero while the alternate screen is up, because the
// alternate screen keeps none: a full-screen program's rows are its own and
// nothing above them belongs to the same document.
//
// This bounds the walk without re-basing it. rowMark and absoluteRowText
// resolve against the buffer's real history either way, so the rows left inside
// the bound are the oldest retained lines, not the alternate screen's.
int addressableRowCount(const Ubiquitty::Terminal& t)
{
    const int history = scrollableRowCount(&t);
    return history + t.rows;
}

// Whether a code point separates words inside a command label.
//
// The Unicode White_Space property, not the ASCII subset of it. A cell holds a
// no-break space or an ideographic space as readily as an ASCII one — a command
// pasted out of a document routinely carries one — and collapsing only the
// ASCII runs leaves the label ragged in a way that depends on where the text
// came from.
bool isLabelSeparator(std::uint32_t codePoint)
{
    switch (codePoint) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x0085:
    case 0x00A0:
    case 0x1680:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

// Every run of whitespace in `text` reduced to one space, both ends trimmed.
//
// The rows of a command are joined before this runs, so the collapse is what
// turns a wrapped or multi-row command into one label: the row boundary and the
// blank tail each row was padded to become the single space between two words.
//
// Decoded rather than scanned bytewise because the separator set is a Unicode
// property. `text` comes from absoluteRowText, which encodes code points, so a
// byte the decoder rejects cannot arrive; a status short of Emit means the
// sequence is still being assembled.
std::string collapseWhitespace(const std::string& text)
{
    std::string collapsed;
    Ubiquitty::Unicode::UTF8Decoder decoder;
    Ubiquitty::Unicode::utf8Reset(&decoder);
    bool isSeparatorPending = false;
    for (const char byte : text) {
        std::uint32_t codePoint = 0;
        if (Ubiquitty::Unicode::utf8Feed(&decoder, static_cast<std::uint8_t>(byte), &codePoint) != Ubiquitty::Unicode::UTF8Status::Emit)
            continue;
        if (isLabelSeparator(codePoint)) {
            // Nothing emitted yet means this run is the leading whitespace,
            // which is trimmed rather than collapsed. A run left pending at the
            // end is the trailing whitespace and is never flushed.
            isSeparatorPending = !collapsed.empty();
            continue;
        }
        if (isSeparatorPending)
            collapsed.push_back(' ');
        isSeparatorPending = false;
        Ubiquitty::Unicode::appendUTF8(collapsed, codePoint);
    }
    return collapsed;
}

// The rows of one command region joined into a single label.
//
// `resolveCommandRegion` bounds its scan by the same `totalRows` this walk uses,
// so `commandLastRow` is always a row that exists. That answers whether the walk
// can read out of range; it does not answer how much it reads, and the two are
// not the same question. The distance from the prompt row to the output mark is
// chosen by whatever printed them, so a program that emits `OSC 133;A`, a
// scrollback's worth of text, and `OSC 133;C` names a region the size of the
// scrollback — and every row of it would land in one palette title that the
// fuzzy ranking re-reads on each keystroke.
//
// So the walk stops at `maxCommandTitleRows`, as the output half already stops at
// `maxCommandOutputRows`.
std::string commandLabel(const Ubiquitty::Terminal& t, const CommandRegionRows& region)
{
    int lastRow = region.commandLastRow;
    if (lastRow - region.promptRow >= maxCommandTitleRows)
        lastRow = region.promptRow + maxCommandTitleRows - 1;
    std::string joined;
    for (int row = region.promptRow; row <= lastRow; ++row) {
        if (!joined.empty())
            joined.push_back(' ');
        joined += absoluteRowText(t, row, t.columns);
    }
    return collapseWhitespace(joined);
}

} // namespace

CommandBlockList commandBlocks(const Ubiquitty::Terminal* t, int maxBlocks)
{
    CommandBlockList blocks;
    if (!t || maxBlocks <= 0)
        return blocks;

    const int totalRows = addressableRowCount(*t);
    for (int absoluteRow = totalRows - 1; absoluteRow >= 0 && static_cast<int>(blocks.size()) < maxBlocks; --absoluteRow) {
        if ((rowMark(t, absoluteRow) & osc133MarkCmdExecuted) == 0)
            continue;
        const CommandRegionRows region = resolveCommandRegion(t, totalRows, absoluteRow);
        if (!region.found)
            continue;
        std::string label = commandLabel(*t, region);
        if (label.empty())
            continue; // an executed prompt whose rows carry no text
        CommandBlock block;
        block.command = std::move(label);
        block.promptRow = region.promptRow;
        block.didFail = (rowMark(t, region.promptRow) & osc133MarkCmdFailed) != 0;
        blocks.push_back(std::move(block));
    }
    return blocks;
}

CommandOutputRows lastCommandOutputRows(const Ubiquitty::Terminal* t)
{
    CommandOutputRows span;
    if (!t)
        return span;

    const int totalRows = addressableRowCount(*t);
    for (int absoluteRow = totalRows - 1; absoluteRow >= 0; --absoluteRow) {
        if ((rowMark(t, absoluteRow) & osc133MarkCmdExecuted) == 0)
            continue;
        const CommandRegionRows region = resolveCommandRegion(t, totalRows, absoluteRow);
        // A command whose region ends on the buffer's last row has produced no
        // output rows yet, and `isFound` reports that rather than handing back a
        // pair describing them backwards. A caller building a range out of an
        // inverted pair does not get an empty one — Swift's ClosedRange traps on
        // it — so an absent span and an empty span answer the same way here.
        const int firstOutputRow = region.commandLastRow + 1;
        if (region.found && firstOutputRow <= totalRows - 1) {
            span.isFound = true;
            span.from = firstOutputRow;
            span.to = totalRows - 1;
        }
        // The newest executed prompt decides the answer and the scan stops on it.
        // An older block's output is not the newest one's, so continuing past
        // this row would answer a different question.
        break;
    }
    return span;
}

std::string commandBlockOutput(const Ubiquitty::Terminal* t, int absoluteRow, int maxRows)
{
    if (!t || absoluteRow < 0 || maxRows <= 0)
        return {};
    const int totalRows = addressableRowCount(*t);
    if (absoluteRow >= totalRows)
        return {};
    const CommandRegionRows region = resolveCommandRegion(t, totalRows, absoluteRow);
    if (!region.found)
        return {};

    // The output ends where the NEXT executed prompt begins. Without that scan a
    // block would claim every row to the end of the buffer, so the newest block's
    // text would be correct and every older one would carry its successors'.
    int outputEnd = totalRows - 1;
    for (int row = region.commandLastRow + 1; row < totalRows; ++row) {
        if ((rowMark(t, row) & osc133MarkCmdExecuted) != 0) {
            outputEnd = row - 1;
            break;
        }
    }

    std::vector<std::string> lines;
    int emitted = 0;
    for (int row = region.commandLastRow + 1; row <= outputEnd && emitted < maxRows; ++row, ++emitted)
        lines.push_back(absoluteRowText(*t, row, t->columns));
    // Trailing blanks are the unwritten remainder of the buffer, not output the
    // command produced. A block ending mid-screen would otherwise carry the rest
    // of the screen with it.
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();

    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0)
            joined.push_back('\n');
        joined += lines[i];
    }
    return joined;
}

} // namespace Ubiquitty::Screen
