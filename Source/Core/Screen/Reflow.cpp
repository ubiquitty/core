// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Reflow.cpp — what a resize does to the rows.
//
// Split from Addressing.cpp because it is the opposite operation: addressing
// answers where a row is, this decides where rows GO when the width changes.
// Runs of soft-wrapped rows form one logical line, are concatenated and re-split
// at the new width; hard breaks stay breaks, wide pairs never straddle the new
// right edge. The alternate screen does not reflow.

// The scrollback/visible-row queries the frontend reads at paint (visibleCell,
// getDisplayRow, rowMark, findPromptMark*) plus the width-change reflow +
// resize machinery. Off the per-byte parse loop; the reflow helpers are
// file-local statics that only run on resize.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Color.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/GridBounds.h"
#include "Core/LineStore.h"
#include "Core/OSC/TextSizing.h"
#include "Core/RichText/Blocks.h"
#include "Core/RowMeta.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/Resize.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Unicode/Joining.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "Core/Unicode/Width.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

#if UBIQUITTY_ENABLE_TEST_HOOKS
namespace Ubiquitty::Screen {
std::uint64_t reflowCellsCopiedForTest = 0;
} // namespace Ubiquitty::Screen
#endif

using ParserState = Ubiquitty::VT::Parser::State;

// Width-change reflow. On a columns change the PRIMARY screen + scrollback
// rewrap: runs of soft-wrapped rows form one logical line, concatenated and
// re-split at the new width. Hard breaks stay breaks; wide pairs never straddle
// the new right edge; trailing blanks are trimmed before the re-split. The
// alternate screen does not reflow. Input rows stream (scrollback then visible)
// into a fixed-capacity output ring, so a narrowing resize can't overrun memory
// — only the most recent (scrollbackCapacity + newRows) rows are retained.

// True when a cell carries no glyph and no painted background/attributes —
// safe to drop as trailing padding. A space with a non-default background or
// any attribute is content (an app drew it) and survives.
static bool reflowCellIsBlank(const Ubiquitty::Cell& c)
{
    const Ubiquitty::Pen& pen = Ubiquitty::cellPen(c);
    return c.codePoint == 0 && pen.attributes.isEmpty() && pen.foregroundIndex == Ubiquitty::colorDefault && pen.backgroundIndex == Ubiquitty::colorDefault;
}

// One reflow output row plus its soft-wrap join flag. cells points into
// the reflow ring's flat cell store (outputCapacity × newColumns), sized to the new
// column count so a wide reflow doesn't allocate a fixed-max ring.
struct ReflowRow {
    Ubiquitty::Cell* cells;
    int length;
    bool wrapped;
    uint8_t mark; // OSC 133 marker carried from the logical line's head
    uint8_t lineAttribute; // DECDWL/DECDHL of the logical line, on its first row only
    bool needsShaping;
    long startOrdinal;
};

struct ReflowAnchor {
    long targetOrdinal { -1 };
    int row { -1 }; // Output ring index, retained across pushes until eviction.
    int column {};
};

// Streaming reflow accumulator: a fixed-capacity ring of output rows.
// appendCells feeds a logical line's cells; flushLogical closes the
// line (marks the final row non-wrapped). The cursor's logical
// position is tracked as a running cell ordinal so it can be mapped
// back to (row, column) after the re-split.
struct ReflowState {
    ReflowRow* ring; // [cap]
    int capacity;
    int head; // index of oldest retained row
    int count; // retained rows
    int newColumns;
    // The terminal being reflowed, for a proportional run wider than the new
    // row: its spec table interns the clamped claim. Null leaves such a run to
    // drop as a sized glyph does.
    Ubiquitty::Terminal* terminal = nullptr;
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> current; // row under construction
    int currentLength;
    bool currentNeedsShaping; // OR of mayNeedShaping over the open row's cells
    uint8_t currentMark; // marker for the current logical line's rows
    uint8_t currentLineAttribute; // DECDWL/DECDHL for the current logical line
    bool currentFirstRow; // the open row is the logical line's first
    bool haveCurrentRow; // a row is open (even if empty)

    // cursor mapping. The cursor is anchored to a content offset within
    // its logical line; placement happens when that line closes so the
    // empty-line and end-of-line cases (offset == line length) map
    // correctly to a concrete (row, column).
    long cursorTargetOrd; // absolute content ordinal of cursor, -1 none
    long emittedOrdinal; // content cells emitted into closed rows
    long lineStartOrd; // emittedOrdinal at the current line's start
    int lineFirstSeq; // sequence index of the current line's first row
    bool cursorPlaced;
    int cursorRow; // output ring index where cursor landed
    // Leading ring rows that are, cell for cell and field for field, the history
    // lines they were built from — so the store already holds them and rebuilding
    // them would write back what is there. Counted only while it is an unbroken
    // run from the first row; the first row that is not one closes it for good.
    int unchangedPrefixRows;
    bool prefixStillUnbroken = true; // bool-ok: the run is open or it is not
    int cursorColumn;
    ReflowAnchor visibleTop;
    ReflowAnchor savedCursor;
    // The lines the ring's oldest rows closed as it dropped them, and whether
    // the row now oldest begins a line, once any was dropped: the rebuild
    // resumes the store's line numbering past them, so no line is renumbered.
    std::uint64_t droppedLineEnds = 0;
    std::optional<Ubiquitty::LineStore::LineContinuity> rowAfterDropped;
    // Proportional run reflow, after the row buffer so the per-cell copy
    // keeps its place in the state.
    // Whether a live spec names a proportional run clamped below its claim.
    // A history line holding one is fed rather than pushed whole, so the
    // rewrap can give the run its claim back.
    bool hasClampedRuns = false; // bool-ok: a fact about the spec table, read per history line
    // Whether any live spec is a proportional run. Without one no cell can
    // be a run's anchor and no broken run can be held, so the feed takes the
    // loop that tests neither.
    bool hasProportionalRuns = false; // bool-ok: a fact about the spec table, read per fed row
    // The pieces of a broken run fed so far, chain after chain, while the
    // last one fed says the next run continues it; empty otherwise.
    std::vector<Ubiquitty::Cell> pendingRunCells;
    // The width of each pending piece, in feed order.
    std::vector<int> pendingRunWidths;
    // Output cells minus input cells over everything fed so far: a run set
    // at other widths than it came in moves every ordinal after it.
    long ordinalShift = 0;
};

// Two-case enums in place of bool parameters so call sites read as intent.
enum class RowWrap : bool {
    Final = false,
    Wrapped = true
};
enum class RowShaping : bool {
    Plain = false,
    MayNeedShaping = true
};

// The one place a closed row's cells are copied into the ring, so what the
// test-hook count reads is copies made and not one call site's share of them.
static void reflowCopyRow(Ubiquitty::Cell* destination, const Ubiquitty::Cell* source, int length)
{
    std::memcpy(destination, source, static_cast<size_t>(length) * sizeof(Ubiquitty::Cell));
#if UBIQUITTY_ENABLE_TEST_HOOKS
    Ubiquitty::Screen::reflowCellsCopiedForTest += static_cast<std::uint64_t>(length);
#endif
}

static void reflowPushRow(ReflowState* st, const Ubiquitty::Cell* cells, int length, RowWrap wrap, uint8_t mark, uint8_t lineAttribute, RowShaping shaping)
{
    int index;
    if (st->count < st->capacity) {
        index = (st->head + st->count) % st->capacity;
        st->count++;
    } else {
        index = st->head;
        st->head = (st->head + 1) % st->capacity;
        st->rowAfterDropped = st->ring[index].wrapped ? Ubiquitty::LineStore::LineContinuity::ContinuesLine : Ubiquitty::LineStore::LineContinuity::StartsLine;
        if (!st->ring[index].wrapped)
            ++st->droppedLineEnds;
        // The ring has started dropping its oldest rows, so ring position i is no
        // longer history line i and the store's leading lines are no longer the
        // ones that survive. Counting a row into the run is only safe while the
        // run can still be identified afterwards, and eviction is what ends that.
        st->unchangedPrefixRows = 0;
        st->prefixStillUnbroken = false;
        // The cursor row just spilled out of the ring; it will be
        // clamped to the top of the surviving region by the caller.
        if (st->cursorPlaced && st->cursorRow == index)
            st->cursorPlaced = false;
        for (ReflowAnchor* anchor : { &st->visibleTop, &st->savedCursor }) {
            if (anchor->row == index) {
                *anchor = {};
            }
        }
    }
    ReflowRow* dst = &st->ring[index];
    // An empty row carries no cell storage, and memcpy's source must be non-null
    // even for zero bytes.
    if (length > 0)
        reflowCopyRow(dst->cells, cells, length);
    dst->length = length;
    dst->wrapped = (wrap == RowWrap::Wrapped);
    dst->mark = mark;
    dst->lineAttribute = lineAttribute;
    // needsShaping was OR-accumulated over these cells by reflowAppendSpan as it
    // copied them (a safe superset, per Screen/ScrollLine.h).
    dst->needsShaping = (shaping == RowShaping::MayNeedShaping);
    dst->startOrdinal = st->emittedOrdinal;
    st->emittedOrdinal += length;
}

static void reflowPlaceAnchorInLine(const ReflowState& state, ReflowAnchor& anchor)
{
    if (anchor.row >= 0 || anchor.targetOrdinal < state.lineStartOrd || anchor.targetOrdinal > state.emittedOrdinal) {
        return;
    }
    // Ring eviction can remove the logical line's beginning. Each surviving
    // row carries its own ordinal so the anchor never depends on that prefix.
    for (int sequence = state.count - 1; sequence >= 0; --sequence) {
        const int index = (state.head + sequence) % state.capacity;
        const ReflowRow& row = state.ring[index];
        const long end = row.startOrdinal + row.length;
        if (anchor.targetOrdinal >= row.startOrdinal
            && (anchor.targetOrdinal < end || (sequence == state.count - 1 && anchor.targetOrdinal <= end))) {
            anchor.row = index;
            anchor.column = std::clamp(static_cast<int>(anchor.targetOrdinal - row.startOrdinal), 0, state.newColumns - 1);
            return;
        }
    }
}

static void reflowCaptureVisibleAnchors(ReflowState& state, const Ubiquitty::Screen::State& screen,
    Ubiquitty::Modes::AnchorMode anchor, int row, long fedOrdinal, int oldColumns, int& feedLength)
{
    if (anchor != Ubiquitty::Modes::AnchorMode::VisibleTop) {
        return;
    }
    if (row == 0) {
        state.visibleTop.targetOrdinal = fedOrdinal;
    }
    if (screen.hasSavedCursor && row == screen.savedY) {
        const int column = std::clamp(screen.savedX, 0, oldColumns);
        feedLength = std::max(feedLength, column);
        state.savedCursor.targetOrdinal = fedOrdinal + column;
    }
}

static int reflowVisibleStart(const ReflowState& state, int newRows)
{
    if (state.visibleTop.row >= 0) {
        const int firstRow = (state.visibleTop.row - state.head + state.capacity) % state.capacity;
        // Every retained row below the anchor must remain visible, including the
        // cursor's row. A narrower layout may need more room than the grow gives.
        if (firstRow < state.count && state.count - firstRow <= newRows) {
            return firstRow;
        }
    }
    return std::max(0, state.count - newRows);
}

static void reflowRestoreSavedCursor(const ReflowState& state, Ubiquitty::Screen::State& screen, int firstVisibleRow, int newRows)
{
    if (state.savedCursor.row < 0) {
        return;
    }
    const int sequence = (state.savedCursor.row - state.head + state.capacity) % state.capacity;
    screen.savedY = std::clamp(sequence - firstVisibleRow, 0, newRows - 1);
    screen.savedX = sequence < firstVisibleRow ? 0 : state.savedCursor.column;
}

// Place the cursor onto a concrete (row, column) within the logical line
// that just closed, spanning ring sequence positions
// [lineFirstSeq .. count-1] and content ordinals
// [lineStartOrd .. emittedOrdinal]. Called from the line-close paths.
// Offset == line length (cursor at/after the last glyph) maps to the
// tail column of the line's final row.
static void reflowPlaceCursorInLine(ReflowState* st)
{
    reflowPlaceAnchorInLine(*st, st->visibleTop);
    reflowPlaceAnchorInLine(*st, st->savedCursor);
    if (st->cursorTargetOrd < 0 || st->cursorPlaced)
        return;
    if (st->visibleTop.targetOrdinal >= 0) {
        ReflowAnchor cursor { .targetOrdinal = st->cursorTargetOrd };
        reflowPlaceAnchorInLine(*st, cursor);
        if (cursor.row >= 0) {
            st->cursorPlaced = true;
            st->cursorRow = cursor.row;
            st->cursorColumn = cursor.column;
        }
        return;
    }
    if (st->cursorTargetOrd < st->lineStartOrd || st->cursorTargetOrd > st->emittedOrdinal)
        return;
    long offset = st->cursorTargetOrd - st->lineStartOrd;
    long acc = 0;
    for (int seq = st->lineFirstSeq; seq < st->count; seq++) {
        int index = (st->head + seq) % st->capacity;
        int length = st->ring[index].length;
        bool last = (seq == st->count - 1);
        long hi = acc + length;
        if (offset < hi || (last && offset <= hi)) {
            st->cursorPlaced = true;
            st->cursorRow = index;
            int column = static_cast<int>(offset - acc);
            if (column >= st->newColumns)
                column = st->newColumns - 1;
            if (column < 0)
                column = 0;
            st->cursorColumn = column;
            return;
        }
        acc = hi;
    }
}

// Emit the in-progress row as a non-final (soft-wrapped) row of the
// current logical line and start a fresh empty row. DECDWL/DECDHL rides only
// the logical line's first physical row; continuation rows are NORMAL.
static void reflowEmitWrapped(ReflowState* st)
{
    uint8_t attribute = st->currentFirstRow ? st->currentLineAttribute : static_cast<uint8_t>(Ubiquitty::lineAttributeNormal);
    reflowPushRow(st, st->current.data(), st->currentLength, RowWrap::Wrapped, st->currentMark, attribute, st->currentNeedsShaping ? RowShaping::MayNeedShaping : RowShaping::Plain);
    st->currentFirstRow = false;
    st->currentLength = 0;
    st->currentNeedsShaping = false;
}

// Append one run to the current row, wrapping to a new row when the row
// fills. The caller hands a whole run — anchor plus its continuation chain —
// so a run never straddles the new right edge, and guarantees width fits
// newColumns, so the wrap always opens enough room.
// One cell, which is what an ordinary row is made of.
//
// `reflowFeedRow` takes this inline form for width-1 cells, which is everything
// that is not a wide chain, rather than a call to the span form per cell. The
// wide chains stay out of line, where their loop belongs.
static inline void reflowAppendCell(ReflowState* st, const Ubiquitty::Cell& cell)
{
    if (st->currentLength + 1 > st->newColumns)
        reflowEmitWrapped(st);
    if (Ubiquitty::Unicode::mayNeedShaping(cell.codePoint))
        st->currentNeedsShaping = true;
    st->current.constructAt(static_cast<std::size_t>(st->currentLength++), cell);
#if UBIQUITTY_ENABLE_TEST_HOOKS
    ++Ubiquitty::Screen::reflowCellsCopiedForTest;
#endif
    st->haveCurrentRow = true;
}

static void reflowAppendSpan(ReflowState* st, const Ubiquitty::Cell* span, int width)
{
    if (st->currentLength + width > st->newColumns)
        reflowEmitWrapped(st);
    for (int i = 0; i < width; i++) {
        if (Ubiquitty::Unicode::mayNeedShaping(span[i].codePoint))
            st->currentNeedsShaping = true;
        st->current.constructAt(static_cast<std::size_t>(st->currentLength++), span[i]);
    }
#if UBIQUITTY_ENABLE_TEST_HOOKS
    Ubiquitty::Screen::reflowCellsCopiedForTest += static_cast<std::uint64_t>(width);
#endif
    st->haveCurrentRow = true;
}

// Frees the spec ids nothing names, before the feed, when a clamp could need
// one.
//
// A clamped run interns a spec for the new width, and a table every id of
// which is still named refuses it — the same refusal an OSC 66 mint meets,
// which takes a census first (Screen::internTextSizeSpec). The census walks the
// grid and the line store, and during the feed those are still the OLD
// population: taken mid-feed it would read cells naming the original ids and
// free the ones only the ring's earlier clamped rows carry. So it runs here,
// once, over the cells that exist, and a table still full afterwards is
// exhausted in fact.
static void reflowPrepareTextSizeTable(Ubiquitty::Terminal& t)
{
    if (t.textSizeTable.liveCount() >= Ubiquitty::textSizeTableCapacity)
        Ubiquitty::Screen::reclaimTextSizeSpecs(t);
}

// Whether any live spec is a proportional run.
static bool hasProportionalRuns(const Ubiquitty::Terminal& t)
{
    for (int slot = 0; slot < Ubiquitty::textSizeTableCapacity; ++slot) {
        if ((t.textSizeTable.liveMask() >> slot & 1u) && t.textSizeTable.specAtSlot(slot).isProportional)
            return true;
    }
    return false;
}

// Whether any live spec is a proportional run clamped below its claim.
static bool hasClampedRuns(const Ubiquitty::Terminal& t)
{
    for (int slot = 0; slot < Ubiquitty::textSizeTableCapacity; ++slot) {
        if ((t.textSizeTable.liveMask() >> slot & 1u) && t.textSizeTable.specAtSlot(slot).claimedRunWidthCells)
            return true;
    }
    return false;
}

// The cells a proportional run's claim reaches on a row of `rowCapacity`
// cells: the `c` its program asked for, as far as the row holds it. Any other
// chain keeps the width it has.
static int reflowRunTargetWidth(const ReflowState* st, const Ubiquitty::Cell* run, int runWidth, int rowCapacity)
{
    const Ubiquitty::Terminal* t = st->terminal;
    if (!t || !Ubiquitty::isProportionalRunAnchor(t, run[0]))
        return runWidth;
    const Ubiquitty::TextSizeSpec* spec = t->textSizeTable.lookup(Ubiquitty::cellPen(run[0]).extendedAttributes.textSizeSpecID());
    if (!spec)
        return runWidth;
    const int claimed = spec->claimedRunWidthCells ? spec->claimedRunWidthCells : spec->runWidthCells;
    return std::min(std::max(claimed, 1), rowCapacity);
}

// Appends a proportional run at `width` cells: clamped to a row narrower than
// its claim, or given its claim back on a row that holds more of it.
//
// The chain claims `width` cells under a spec interned for that width, which
// remembers the claim the program made while the two differ, so the orphan
// scrub, which judges a chain against its claim, keeps it, and the anchor's
// run-text id rides the copy, so every character survives. Cells a widened
// run gains are continuations like its others. A sized glyph the width cannot
// hold, a terminal the state does not name, and a spec table with no id left
// after the census that preceded the feed all append nothing: the chain drops
// whole, and the exhausted table says so.
static void reflowAppendRunAtWidth(ReflowState* st, const Ubiquitty::Cell* run, int runWidth, int width)
{
    Ubiquitty::Terminal* t = st->terminal;
    if (!t || !Ubiquitty::isProportionalRunAnchor(t, run[0]) || width < 1 || runWidth < 2)
        return;
    const Ubiquitty::TextSizeSpec* spec = t->textSizeTable.lookup(Ubiquitty::cellPen(run[0]).extendedAttributes.textSizeSpecID());
    if (!spec)
        return;
    Ubiquitty::TextSizeSpec resized = *spec;
    const int claimed = spec->claimedRunWidthCells ? spec->claimedRunWidthCells : spec->runWidthCells;
    resized.runWidthCells = static_cast<uint16_t>(width);
    resized.claimedRunWidthCells = width == claimed ? 0 : static_cast<uint16_t>(claimed);
    const unsigned resizedID = t->textSizeTable.intern(resized);
    if (resizedID == 0) {
        Ubiquitty::Diagnostics::diagLog(*t, "text-size-specs-exhausted", Ubiquitty::textSizeTableCapacity);
        return;
    }
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> storage;
    for (int i = 0; i < width; ++i) {
        storage.constructAt(static_cast<std::size_t>(i), run[std::min(i, runWidth - 1)]);
        Ubiquitty::Cell& cell = storage.data()[i];
        Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
        pen.extendedAttributes.setTextSizeSpecID(resizedID);
        Ubiquitty::cellSetPen(cell, pen);
    }
    reflowAppendSpan(st, storage.data(), width);
}

// Where a run may break: after a space, before the letter that follows it.
static bool isRunBreak(std::span<const std::uint32_t> text, std::size_t at)
{
    return at > 0 && at < text.size() && text[at - 1] == ' ' && text[at] != ' ';
}

// The cells a piece of a run measures, without the spaces it ends in: a
// piece's trailing space is the break and sits past its cells.
static int reflowPieceCells(Ubiquitty::Terminal& t, std::span<const std::uint32_t> text, Ubiquitty::ProportionalRunStyle style)
{
    std::size_t end = text.size();
    while (end > 1 && text[end - 1] == ' ')
        --end;
    return Ubiquitty::OSC::TextSizing::proportionalRunCells(t, text.first(end), style);
}

// One piece of a broken run: code points [first, end) of its text over
// `width` cells.
struct RunPiece {
    std::size_t first = 0;
    std::size_t end = 0;
    int width = 0;

    friend bool operator==(const RunPiece&, const RunPiece&) = default;
};

// Breaks a run's text into the pieces a row holding `used` of `columns`
// cells, and the rows after it, take, as grid text wraps: each piece ends at
// the last space whose words fit what is left of its row, a piece that fits
// nothing opens the next row, and a word wider than a row is a piece of its
// own clamped to the row. The run fits whole when its claim does.
//
// - Returns: The pieces; one means the run does not break.
static std::vector<RunPiece> reflowPlanRun(Ubiquitty::Terminal& t, std::span<const std::uint32_t> text,
    Ubiquitty::ProportionalRunStyle style, int claimed, int used, int columns)
{
    std::vector<RunPiece> pieces;
    std::vector<std::size_t> breaks;
    for (std::size_t at = 1; at < text.size(); ++at) {
        if (isRunBreak(text, at))
            breaks.push_back(at);
    }
    std::size_t first = 0;
    auto nextBreak = breaks.begin();
    while (first < text.size()) {
        const int room = columns - used;
        const int restCells = first == 0 ? claimed : Ubiquitty::OSC::TextSizing::proportionalRunCells(t, text.subspan(first), style);
        if (restCells <= room) {
            pieces.push_back({ .first = first, .end = text.size(), .width = restCells });
            break;
        }
        // The breaks after `first`, searched for the last whose words fit:
        // a longer prefix never measures fewer cells.
        while (nextBreak != breaks.end() && *nextBreak <= first)
            ++nextBreak;
        auto low = nextBreak;
        auto high = breaks.end();
        int fitWidth = 0;
        std::size_t fitEnd = 0;
        while (low != high) {
            const auto middle = low + (high - low) / 2;
            const int width = reflowPieceCells(t, text.subspan(first, *middle - first), style);
            if (width <= room) {
                fitWidth = width;
                fitEnd = *middle;
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (fitEnd) {
            pieces.push_back({ .first = first, .end = fitEnd, .width = fitWidth });
            used += fitWidth;
            first = fitEnd;
        } else if (used > 0) {
            used = 0;
        } else {
            const std::size_t end = nextBreak == breaks.end() ? text.size() : *nextBreak;
            const int width = std::min(reflowPieceCells(t, text.subspan(first, end - first), style), columns);
            pieces.push_back({ .first = first, .end = end, .width = width });
            used = width;
            first = end;
        }
    }
    return pieces;
}

// Writes one run's chain into `out`: the anchor naming `textID` and the
// continuations after it, under `specID`, in the pen and link of `model`.
static void reflowBuildRunCells(const Ubiquitty::Cell& model, std::uint32_t firstCodePoint, std::uint16_t textID, unsigned specID, int width, Ubiquitty::Cell* out)
{
    Ubiquitty::Pen pen = Ubiquitty::cellPen(model);
    pen.attributes.remove(Ubiquitty::VT::Attribute::WideContinuation);
    if (width >= 2)
        pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    else
        pen.attributes.remove(Ubiquitty::VT::Attribute::Wide);
    pen.extendedAttributes.setTextSizeSpecID(specID);
    Ubiquitty::penSetTextBand(&pen, 0);
    Ubiquitty::Cell anchor {};
    anchor.codePoint = firstCodePoint;
    Ubiquitty::cellSetPen(anchor, pen);
    anchor.setLinkID(model.linkID());
    anchor.setClusterID(textID);
    out[0] = anchor;
    Ubiquitty::Pen continuationPen = pen;
    continuationPen.attributes.remove(Ubiquitty::VT::Attribute::Wide);
    continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    Ubiquitty::Cell continuation {};
    Ubiquitty::cellSetPen(continuation, continuationPen);
    continuation.setLinkID(model.linkID());
    for (int i = 1; i < width; ++i)
        out[i] = continuation;
}

// Appends a run's pieces, each a chain of its own under a spec for its
// width that keeps the run's claim, every piece but the last marked as
// continued. Nothing is appended unless every piece's text and spec intern.
static bool reflowAppendPieces(ReflowState* st, const Ubiquitty::Cell& model, const Ubiquitty::TextSizeSpec& runSpec,
    std::span<const std::uint32_t> text, int claimed, std::span<const RunPiece> pieces)
{
    Ubiquitty::Terminal& t = *st->terminal;
    struct Interned {
        std::uint16_t textID;
        unsigned specID;
    };
    std::vector<Interned> interned;
    interned.reserve(pieces.size());
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const RunPiece& piece = pieces[index];
        Ubiquitty::TextSizeSpec spec = runSpec;
        spec.runWidthCells = static_cast<std::uint16_t>(piece.width);
        spec.claimedRunWidthCells = static_cast<std::uint16_t>(claimed);
        spec.isContinued = index + 1 < pieces.size() ? 1 : 0;
        const unsigned specID = t.textSizeTable.intern(spec);
        if (specID == 0) {
            Ubiquitty::Diagnostics::diagLog(t, "text-size-specs-exhausted", Ubiquitty::textSizeTableCapacity);
            return false;
        }
        // The census before the feed cannot see the ring's ids, so the store
        // is not swept here; a full store keeps the run whole.
        const std::uint16_t textID = t.runTextStore.intern(text.subspan(piece.first, piece.end - piece.first));
        if (textID == 0)
            return false;
        interned.push_back({ textID, specID });
    }
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> storage;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const RunPiece& piece = pieces[index];
        reflowBuildRunCells(model, text[piece.first], interned[index].textID, interned[index].specID, piece.width, storage.data());
        reflowAppendSpan(st, storage.data(), piece.width);
    }
    return true;
}

// Whether a run at the anchor `model` may break at its spaces: a left-set
// run on a row of ordinary width. A run set right, centred or justified is
// placed by its whole width and moves whole.
static bool isBreakableRun(const ReflowState* st, const Ubiquitty::TextSizeSpec& spec, uint8_t lineAttribute)
{
    return st->terminal && spec.horizontalAlign == 0 && lineAttribute == Ubiquitty::lineAttributeNormal;
}

// Moves the cursor's target past a run set at `outputWidth` cells for the
// `inputWidth` it came in: a cursor after the run keeps its place in the
// text, and one inside it stays inside it.
static void reflowShiftOrdinals(ReflowState* st, long outputStart, int inputWidth, int outputWidth)
{
    if (st->cursorTargetOrd >= outputStart + inputWidth)
        st->cursorTargetOrd += outputWidth - inputWidth;
    else if (st->cursorTargetOrd > outputStart)
        st->cursorTargetOrd = std::min(st->cursorTargetOrd, outputStart + outputWidth);
    st->ordinalShift += outputWidth - inputWidth;
}

// Appends one whole run as it came in, at the width its row gives it.
static void reflowAppendWholeRun(ReflowState* st, const Ubiquitty::Cell* run, int runWidth, uint8_t lineAttribute)
{
    // A run wider than the row has no row it fits on. A sized glyph drops
    // whole rather than tearing, because a torn chain renders a glyph over
    // columns the grid no longer gives it; a proportional run is clamped to
    // the row instead, since its text rides the anchor and a narrower claim
    // keeps every character. The row's capacity is in its own cells: a
    // doubled line draws each cell two columns wide, so it holds half the
    // grid's. The capacity estimate assumes a wrap wastes at most one column,
    // which holds while the widest run is the two-column pair; a clamped run
    // fills one output row, no more than the input row it came from held.
    const int rowCapacity = std::max(1, st->newColumns / Ubiquitty::textScaleForLineAttribute(lineAttribute).widthScale);
    const int width = runWidth <= rowCapacity ? reflowRunTargetWidth(st, run, runWidth, rowCapacity) : rowCapacity;
    if (width == runWidth)
        reflowAppendSpan(st, run, runWidth);
    else
        reflowAppendRunAtWidth(st, run, runWidth, width);
}

// Appends a piece of a broken run at its own width, clamped to a row
// narrower than it: a piece's claim is the whole run's, which it does not get
// back on its own.
static void reflowAppendPiece(ReflowState* st, const Ubiquitty::Cell* run, int runWidth, uint8_t lineAttribute)
{
    const int rowCapacity = std::max(1, st->newColumns / Ubiquitty::textScaleForLineAttribute(lineAttribute).widthScale);
    if (runWidth <= rowCapacity)
        reflowAppendSpan(st, run, runWidth);
    else
        reflowAppendRunAtWidth(st, run, runWidth, rowCapacity);
}

// Sets a proportional run — one chain, or the joined pieces of a broken
// one — into the open row: whole where its claim fits what is left of the
// row, broken at its spaces where it does not, and whole on the rows after
// where it has no space to break at.
//
// `chains` is the run's cells as they came in, `widths` the width of each
// chain; their texts joined are the run's text.
static void reflowSetRun(ReflowState* st, std::span<const Ubiquitty::Cell> chains, std::span<const int> widths, uint8_t lineAttribute)
{
    Ubiquitty::Terminal& t = *st->terminal;
    const Ubiquitty::Cell& model = chains[0];
    const Ubiquitty::TextSizeSpec* found = t.textSizeTable.lookup(Ubiquitty::cellPen(model).extendedAttributes.textSizeSpecID());
    const long outputStart = st->emittedOrdinal + st->currentLength;
    const int inputWidth = static_cast<int>(chains.size());
    auto appendAsCame = [&] {
        if (widths.size() == 1)
            return reflowAppendWholeRun(st, chains.data(), widths[0], lineAttribute);
        std::size_t at = 0;
        for (const int width : widths) {
            reflowAppendPiece(st, chains.data() + at, width, lineAttribute);
            at += static_cast<std::size_t>(width);
        }
    };
    auto finish = [&] {
        reflowShiftOrdinals(st, outputStart, inputWidth, static_cast<int>(st->emittedOrdinal + st->currentLength - outputStart));
    };
    if (!found) {
        appendAsCame();
        return finish();
    }
    const Ubiquitty::TextSizeSpec runSpec = *found;
    const int claimed = runSpec.claimedRunWidthCells ? runSpec.claimedRunWidthCells : runSpec.runWidthCells;
    const bool isJoined = widths.size() > 1;
    // A whole run that fits keeps the chain it came in.
    if (!isJoined && st->currentLength + claimed <= st->newColumns) {
        appendAsCame();
        return finish();
    }
    std::vector<std::uint32_t> text;
    std::vector<RunPiece> cameAs;
    {
        std::size_t at = 0;
        for (const int width : widths) {
            const std::span<const std::uint32_t> piece = Ubiquitty::runTextCodePoints(&t, chains[at]);
            cameAs.push_back({ .first = text.size(), .end = text.size() + piece.size(), .width = width });
            text.insert(text.end(), piece.begin(), piece.end());
            at += static_cast<std::size_t>(width);
        }
    }
    if (text.empty()) {
        appendAsCame();
        return finish();
    }
    if (st->currentLength + claimed > st->newColumns && isBreakableRun(st, runSpec, lineAttribute)) {
        const std::vector<RunPiece> pieces = reflowPlanRun(t, text, Ubiquitty::OSC::TextSizing::runStyleOf(Ubiquitty::cellPen(model)),
            claimed, st->currentLength, st->newColumns);
        if (pieces.size() > 1) {
            // Pieces that break where the run came in broken keep their
            // chains, so a rewrap that moves nothing interns nothing.
            if (pieces == cameAs) {
                appendAsCame();
                return finish();
            }
            if (reflowAppendPieces(st, model, runSpec, text, claimed, pieces))
                return finish();
        }
    }
    if (!isJoined) {
        appendAsCame();
        return finish();
    }
    // The pieces join into the run whole, on the next row when this one
    // cannot hold it.
    const int rowCapacity = std::max(1, st->newColumns / Ubiquitty::textScaleForLineAttribute(lineAttribute).widthScale);
    const int whole = std::min(claimed, rowCapacity);
    const RunPiece wholePiece { .first = 0, .end = text.size(), .width = whole };
    if (whole == claimed) {
        // The run the program wrote: its own spec, so the grid it had comes
        // back field for field.
        Ubiquitty::TextSizeSpec wholeSpec = runSpec;
        wholeSpec.runWidthCells = static_cast<std::uint16_t>(whole);
        wholeSpec.claimedRunWidthCells = 0;
        wholeSpec.isContinued = 0;
        const unsigned specID = t.textSizeTable.intern(wholeSpec);
        const std::uint16_t textID = specID ? t.runTextStore.intern(text) : 0;
        if (textID) {
            Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> storage;
            reflowBuildRunCells(model, text[0], textID, specID, whole, storage.data());
            reflowAppendSpan(st, storage.data(), whole);
            return finish();
        }
    } else if (reflowAppendPieces(st, model, runSpec, text, claimed, std::span(&wholePiece, 1))) {
        return finish();
    }
    appendAsCame();
    finish();
}

// Feeds the pieces of a run held while the next run could continue it, as
// they came in: what followed them was not their continuation.
static void reflowFlushPendingRun(ReflowState* st, uint8_t lineAttribute)
{
    if (st->pendingRunCells.empty())
        return;
    const std::vector<Ubiquitty::Cell> cells = std::move(st->pendingRunCells);
    const std::vector<int> widths = std::move(st->pendingRunWidths);
    st->pendingRunCells.clear();
    st->pendingRunWidths.clear();
    const long outputStart = st->emittedOrdinal + st->currentLength;
    std::size_t at = 0;
    for (const int width : widths) {
        reflowAppendPiece(st, cells.data() + at, width, lineAttribute);
        at += static_cast<std::size_t>(width);
    }
    reflowShiftOrdinals(st, outputStart, static_cast<int>(cells.size()), static_cast<int>(st->emittedOrdinal + st->currentLength - outputStart));
}

// Feeds one proportional run's chain: held while its spec says the next run
// continues it, joined with the pieces before it when one is held, and set
// once the run is whole.
static void reflowFeedProportionalRun(ReflowState* st, const Ubiquitty::Cell* run, int runWidth, uint8_t lineAttribute)
{
    const Ubiquitty::TextSizeSpec* spec = st->terminal->textSizeTable.lookup(Ubiquitty::cellPen(run[0]).extendedAttributes.textSizeSpecID());
    // A scaled run is a block: each band is a chain on a row of its own, and
    // the rows are separate logical lines, so a band broken or clamped by its
    // own line would no longer stack on the others. It moves whole and drops
    // whole as a sized glyph does, and the scrub after the rewrap erases a
    // block whose bands the rewrap set apart.
    if (spec && spec->scale > 1) {
        reflowFlushPendingRun(st, lineAttribute);
        const int rowCapacity = std::max(1, st->newColumns / Ubiquitty::textScaleForLineAttribute(lineAttribute).widthScale);
        if (runWidth <= rowCapacity)
            reflowAppendSpan(st, run, runWidth);
        return;
    }
    const bool isContinued = spec && spec->isContinued;
    if (st->pendingRunCells.empty() && !isContinued) {
        const int widths[] = { runWidth };
        return reflowSetRun(st, std::span(run, static_cast<std::size_t>(runWidth)), widths, lineAttribute);
    }
    st->pendingRunCells.insert(st->pendingRunCells.end(), run, run + runWidth);
    st->pendingRunWidths.push_back(runWidth);
    if (isContinued)
        return;
    const std::vector<Ubiquitty::Cell> cells = std::move(st->pendingRunCells);
    const std::vector<int> widths = std::move(st->pendingRunWidths);
    st->pendingRunCells.clear();
    st->pendingRunWidths.clear();
    reflowSetRun(st, cells, widths, lineAttribute);
}

// A whole logical line, pushed from where it already sits.
//
// A history line that is not wrapped and fits the new width entire is its own
// logical line and its own output row: no wrap point falls inside it, so no wide
// chain can be split, and the staging buffer would take these cells and hand the
// same ones back. Pushing from the source skips that copy — one of the three a
// line otherwise makes on its way to the rebuilt store.
//
// The bookkeeping is what reflowFeedRow's header and reflowFlushLogical's tail
// do between them for a single-row line, done here in their place: the line's
// ordinal and ring positions are recorded before the push so the cursor search
// can span them, and the row state is left closed.
//
// Shaping comes from the line's own recorded answer rather than from a walk of
// its cells. ScrollLine documents it as a superset — stale-true costs a shaping
// pass and never misses a shape — which is the guarantee the walk also gives.
static bool reflowPushWholeLine(ReflowState* st, const Ubiquitty::Cell* cells, int contentLength,
    uint8_t mark, uint8_t lineAttribute, bool needsShaping, bool isVerbatim) // bool-ok: mirrors ScrollLine::needsShaping, the value it carries
{
    if (st->haveCurrentRow || contentLength > st->newColumns || st->hasClampedRuns)
        return false;
    // The run closes at the first row the store does not already hold, and at the
    // point the ring starts evicting, because an evicted row is no longer the
    // history line at its own index.
    if (st->prefixStillUnbroken && isVerbatim && st->count == st->unchangedPrefixRows && st->count < st->capacity)
        ++st->unchangedPrefixRows;
    else
        st->prefixStillUnbroken = false;
    st->currentMark = mark;
    st->currentLineAttribute = lineAttribute;
    st->lineStartOrd = st->emittedOrdinal;
    st->lineFirstSeq = st->count;
    reflowPushRow(st, cells, contentLength, RowWrap::Final, mark, lineAttribute,
        needsShaping ? RowShaping::MayNeedShaping : RowShaping::Plain);
    st->currentFirstRow = false;
    st->currentLength = 0;
    st->currentNeedsShaping = false;
    st->haveCurrentRow = false;
    reflowPlaceCursorInLine(st);
    return true;
}

// Feed a physical input row's content cells into the open logical line.
// contentLength is the row's significant length (trailing blanks already
// excluded at the logical-line tail).
// Feeds a row's cells when no proportional run is live: every chain is a
// sized glyph or a wide pair, moved whole, and every other cell is copied.
static void reflowFeedCellsWithoutRuns(ReflowState* st, const Ubiquitty::Cell* cells, int contentLength, uint8_t lineAttribute)
{
    for (int c = 0; c < contentLength;) {
        const Ubiquitty::VT::AttributeSet a = Ubiquitty::cellPen(cells[c]).attributes;
        if (a.contains(Ubiquitty::VT::Attribute::Wide) && c + 1 < contentLength && Ubiquitty::cellPen(cells[c + 1]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            int runEnd = c + 1;
            while (runEnd < contentLength
                && Ubiquitty::cellPen(cells[runEnd]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++runEnd;
            reflowAppendWholeRun(st, &cells[c], runEnd - c, lineAttribute);
            c = runEnd;
        } else if (a.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            // Orphan continuation half (lead trimmed away) — skip.
            c += 1;
        } else {
            reflowAppendCell(st, cells[c]);
            c += 1;
        }
    }
}

static void reflowFeedRow(ReflowState* st, const Ubiquitty::Cell* cells, int contentLength, uint8_t mark, bool firstRowOfLogical, uint8_t lineAttribute) // bool-ok: pre-existing parameter
{
    if (firstRowOfLogical && !st->haveCurrentRow) {
        st->currentMark = mark;
        st->currentLineAttribute = lineAttribute;
        st->currentFirstRow = true;
        st->lineStartOrd = st->emittedOrdinal;
        st->lineFirstSeq = st->count;
    }
    if (!st->hasProportionalRuns) {
        reflowFeedCellsWithoutRuns(st, cells, contentLength, lineAttribute);
        return;
    }
    // Held in a local, not read through `st`, so the plain-cell test below
    // stays in registers across the appends that write through it; only the
    // run feeds can change it.
    bool hasPendingRun = !st->pendingRunCells.empty(); // bool-ok: a cached fact about st, refreshed after each run feed
    constexpr Ubiquitty::VT::AttributeSet chainRoles { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation };
    for (int c = 0; c < contentLength;) {
        const Ubiquitty::Pen& pen = Ubiquitty::cellPen(cells[c]);
        const Ubiquitty::VT::AttributeSet a = pen.attributes;
        const bool isSized = pen.extendedAttributes.textSizeSpecID() != 0;
        // The cell nearly every row is made of: unsized, one column, and no
        // broken run held open before it.
        if (!isSized && !a.containsAny(chainRoles) && !hasPendingRun) [[likely]] {
            reflowAppendCell(st, cells[c]);
            c += 1;
            continue;
        }
        if (isSized && Ubiquitty::isProportionalRunAnchor(st->terminal, cells[c])) {
            // The anchor moves with its whole chain — a wrap that split it
            // would leave fragments no scrub runs here to catch — and the
            // run breaks, if it must, between chains of its own.
            int runEnd = c + 1;
            while (runEnd < contentLength
                && Ubiquitty::cellPen(cells[runEnd]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++runEnd;
            reflowFeedProportionalRun(st, &cells[c], runEnd - c, lineAttribute);
            hasPendingRun = !st->pendingRunCells.empty();
            c = runEnd;
            continue;
        }
        if (hasPendingRun && reflowCellIsBlank(cells[c])) {
            // Erased cells closing a wrapped row stand between a broken run's
            // pieces where the piece after them did not fit beside them; they
            // are not the line's text, so the pieces still join.
            int blankEnd = c;
            while (blankEnd < contentLength && reflowCellIsBlank(cells[blankEnd]))
                ++blankEnd;
            if (blankEnd == contentLength) {
                const long position = st->emittedOrdinal + st->currentLength + static_cast<long>(st->pendingRunCells.size());
                reflowShiftOrdinals(st, position, blankEnd - c, 0);
                break;
            }
        }
        if (hasPendingRun) {
            reflowFlushPendingRun(st, lineAttribute);
            hasPendingRun = false;
        }
        if (a.contains(Ubiquitty::VT::Attribute::Wide) && c + 1 < contentLength && Ubiquitty::cellPen(cells[c + 1]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            // A sized glyph moves with its whole chain, and drops whole on a
            // row it does not fit.
            int runEnd = c + 1;
            while (runEnd < contentLength
                && Ubiquitty::cellPen(cells[runEnd]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++runEnd;
            reflowAppendWholeRun(st, &cells[c], runEnd - c, lineAttribute);
            c = runEnd;
        } else if (a.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            // Orphan continuation half (lead trimmed away) — skip.
            c += 1;
        } else {
            reflowAppendCell(st, cells[c]);
            c += 1;
        }
    }
}

// Close the current logical line: emit the open row as its final
// (non-wrapped) row. An empty logical line still produces one blank row
// so hard breaks between blank lines survive.
static void reflowFlushLogical(ReflowState* st)
{
    reflowFlushPendingRun(st, st->currentLineAttribute);
    // The final row carries the line attribute only when it is the first row (a
    // single-row logical line, e.g. a DECDWL/DECDHL banner that never wraps);
    // if the line already wrapped, its attribute left on the first row above.
    uint8_t attribute = st->currentFirstRow ? st->currentLineAttribute : static_cast<uint8_t>(Ubiquitty::lineAttributeNormal);
    reflowPushRow(st, st->current.data(), st->currentLength, RowWrap::Final, st->currentMark, attribute, st->currentNeedsShaping ? RowShaping::MayNeedShaping : RowShaping::Plain);
    st->currentFirstRow = false;
    st->currentLength = 0;
    st->currentNeedsShaping = false;
    st->haveCurrentRow = false;
    reflowPlaceCursorInLine(st);
}

namespace Ubiquitty {

// Height-only resize (columns unchanged): no rewrap needed, so skip the full
// reflowPrimary ring rebuild and move only the lines crossing the visible/
// scrollback boundary — a grow pulls history into the new rows, a shrink scrolls
// the top rows into history. Mirrors reflowPrimary's distribution (trailing-blank
// trim, bottom-anchor of a grow that doesn't fill the taller screen).
// Re-derives a destination row's proportional-run flag from the cells it holds.
//
// The flag says a row may hold a proportional run, and the frame builder reads
// it alone before it looks for one: a row whose flag is false draws its anchor
// through the mono path, where the run's multi-cell claim stretches one glyph's
// atlas slot across every cell of the claim. Cells move between rows here while
// row metadata stays with the index, so each row written states the flag for
// the cells it holds afterwards.
static void relatchFlowedRow(Ubiquitty::Terminal& t, int screen, int row)
{
    if (!t.grid || row < 0 || row >= t.rows)
        return;
    t.grid->rowMetaAt(screen, row).set(Ubiquitty::RowFlag::Flowed, false);
    Screen::latchFlowedRow(t, screen, row);
}

static void reflowHeightOnly(Ubiquitty::Terminal& t, int newRows, Modes::AnchorMode anchor, int* outCursorRow, int* outCursorColumn, int* outRowShift)
{
    if (outRowShift)
        *outRowShift = 0;
    if (!t.grid)
        return;
    Screen::State* screen = &t.screens.primary;
    const int columns = t.columns; // unchanged on this path
    const int oldRows = t.rows;
    const int historyRows = t.scrollbackCount;
    auto clamp = [](int value, int minimum, int maximum) { return value < minimum ? minimum : (value > maximum ? maximum : value); };

    // Reads/writes target the primary; flip the alternate off for the duration
    // (restored at the end) so readRow/setRowSpan hit the right store.
    const bool wasAlternate = t.grid->isAlternateActive();
    if (wasAlternate)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);

    const int cursorRow = clamp(screen->cursorY, 0, oldRows - 1);

    // True content length by scanning the ACTUAL cells, not rowContentWidth
    // (CellGrid::resize stamps every row's bound to full width on each geometry
    // change, so it reports every row full mid-drag and the trailing-blank trim
    // would never fire). A soft-wrap continuation keeps full width; a normal row
    // trims to its last non-default-blank cell.
    auto liveContentWidth = [&](int row) -> int {
        if (screen->lineWrapped[row])
            return columns;
        const RowView v = t.grid->rowCells(Ubiquitty::Row(row));
        int width = v.count;
        while (width > 0 && reflowCellIsBlank(v.data[width - 1]))
            width--;
        return width;
    };
    int contentRows = oldRows;
    while (contentRows > cursorRow + 1) {
        int row = contentRows - 1;
        if (screen->lineWrapped[row])
            break;
        if (liveContentWidth(row) != 0)
            break;
        contentRows--;
    }

    // Shrink/grow accounting from visible content only (never folding history into
    // the repack). Shrink spills only the rows trimmed content overruns the new
    // height by; the trailing blank band absorbs the rest (contentRows <= newRows
    // → nothing spills). Grow pulls history back to fill the rows freed above the
    // content. Grow/shrink are mutually exclusive on newRows vs oldRows.
    const int pushedRows = (newRows < oldRows && contentRows > newRows) ? contentRows - newRows : 0;
    const int growRoom = (anchor != Modes::AnchorMode::VisibleTop && newRows > oldRows && newRows > contentRows) ? newRows - contentRows : 0;
    const int pulledRows = (growRoom < historyRows) ? growRoom : historyRows;
    const int visibleRows = (pushedRows > 0) ? newRows : contentRows + pulledRows;

    Cell blank = defaultBlankCell;
    CellScratchBuffer<maxColumns> rowStorage;
    Cell* rowBuffer = rowStorage.data();

    auto putRow = [&](int dest, const Cell* cells, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped) {
        t.grid->eraseRow(Ubiquitty::Row(dest), blank);
        if (length > 0)
            t.grid->setRowSpan(Ubiquitty::Row(dest), Ubiquitty::Column(0), Ubiquitty::RowView { const_cast<Cell*>(cells), length });
        screen->lineAttribute[dest] = lineAttribute;
        screen->lineWrapped[dest] = wrapped;
        screen->mark[dest] = mark;
        relatchFlowedRow(t, 0, dest);
    };

    int rowShift;
    if (pushedRows > 0) {
        // Shrink: the top pushedRows scroll into history, survivors shift up.
        for (int row = 0; row < pushedRows; row++) {
            t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
            const bool needsShaping = t.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping);
            Screen::scrollbackAppend(&t, rowBuffer, liveContentWidth(row), screen->lineAttribute[row], screen->mark[row], screen->lineWrapped[row], needsShaping);
        }
        for (int row = pushedRows; row < contentRows; row++) { // low->high: dest < src
            t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
            putRow(row - pushedRows, rowBuffer, liveContentWidth(row), screen->lineAttribute[row], screen->mark[row], screen->lineWrapped[row]);
        }
        for (int row = visibleRows; row < newRows; row++)
            putRow(row, nullptr, 0, Ubiquitty::lineAttributeNormal, 0, false);
        t.grid->resize(newRows, columns);
        rowShift = -pushedRows;
    } else if (pulledRows > 0) {
        // Grow: expose the rows, shift content down by the pulled count, then
        // fill the freed top rows from the newest history. Content shorter than
        // the screen stays top-anchored (blank pads the bottom).
        t.grid->resize(newRows, columns);
        for (int row = contentRows - 1; row >= 0; row--) { // high->low: dest > src
            t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
            putRow(row + pulledRows, rowBuffer, liveContentWidth(row), screen->lineAttribute[row], screen->mark[row], screen->lineWrapped[row]);
        }
        for (int row = 0; row < pulledRows; row++) {
            const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, historyRows - pulledRows + row);
            if (line)
                putRow(row, line->cells, line->length, line->lineAttribute, line->mark, line->wrapped);
            else
                putRow(row, nullptr, 0, Ubiquitty::lineAttributeNormal, 0, false);
        }
        for (int row = pulledRows + contentRows; row < newRows; row++)
            putRow(row, nullptr, 0, Ubiquitty::lineAttributeNormal, 0, false);
        // The newest pulledRows lines moved back onto the live screen; drop
        // them from the store's newest end so history no longer holds them.
        const std::uint64_t nextOrdinalBefore = t.lineStore ? t.lineStore->nextOrdinal() : 0;
        if (t.lineStore)
            t.lineStore->dropNewest(pulledRows);
        t.scrollbackCount = t.lineStore ? t.lineStore->count() : 0;
        Screen::notePositionsPulledFromHistory(t, nextOrdinalBefore);
        rowShift = pulledRows;
    } else {
        // Boundary unchanged — commit the new geometry and pad the bottom.
        t.grid->resize(newRows, columns);
        for (int row = visibleRows; row < newRows; row++)
            putRow(row, nullptr, 0, Ubiquitty::lineAttributeNormal, 0, false);
        rowShift = 0;
    }
    // A content row at the screen bottom never joins a soft-wrap below it.
    if (visibleRows > 0 && visibleRows == newRows)
        screen->lineWrapped[newRows - 1] = false;

    if (wasAlternate)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);

    // The DECSET 1048/1049 saved cursor rode the same shift; move it too, or
    // restoring it on app exit drops the prompt at its old row.
    if (screen->hasSavedCursor)
        screen->savedY = clamp(screen->savedY + rowShift, 0, newRows - 1);

    *outCursorRow = clamp(cursorRow + rowShift, 0, newRows - 1);
    *outCursorColumn = clamp(screen->cursorX, 0, columns - 1);
    if (outRowShift)
        *outRowShift = rowShift;
}

enum class ReflowError : uint8_t {
    AllocationFailed
};

// Rewrap the primary screen + scrollback to newColumns. Called from resize
// only when the column count actually changes. The pre-resize cursor is
// passed as the primary visible row/column it occupied; its absolute cell
// ordinal in the concatenated logical stream is computed during the feed
// so it restores onto the same logical character. The new visible cursor
// (row, column) is written back through outCursorRow / outCursorColumn (clamped
// to the visible screen).
[[nodiscard]] static std::expected<void, ReflowError> reflowPrimary(Ubiquitty::Terminal& t, int oldColumns, int newRows, int newColumns, Modes::AnchorMode anchor, int cursorRowIn, int cursorColIn, int* outCursorRow, int* outCursorColumn)
{
    assert(t.grid);

    Screen::State* s = &t.screens.primary;
    const bool isPrimaryActive = (t.screens.active == &t.screens.primary);

    // Output capacity sized to the CONTENT's worst-case post-rewrap row count,
    // not the scrollback LIMIT (which would allocate a limit-sized ring every
    // drag step). Per-row bound (width + divisor) / divisor is wide-cell-safe (a
    // pair never splits a wrap, so a row carries >= newColumns-1 content cells)
    // and over-estimates the merged-line cost (ceil is superadditive), so it
    // never under-sizes. Clamped to scrollbackLimit+newRows so the ring still
    // spills the oldest beyond the limit exactly as scrollback would.
    const int maxReflowRows = t.scrollbackLimit + newRows;
    const int reflowRowDivisor = newColumns > 1 ? newColumns - 1 : 1;
    long contentRows = 0;
    for (int i = 0; i < t.scrollbackCount && contentRows < maxReflowRows; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        // Each line sizes (and below, feeds) at its OWN stored length: history
        // is width-uniform except while an Interactive-resize deferral is
        // pending, when it carries mixed widths — the settle rewrap through
        // this path must ingest each line whole, not clamped to oldColumns.
        int rowLength = line->length;
        if (rowLength > maxColumns)
            rowLength = maxColumns;
        contentRows += (rowLength + reflowRowDivisor) / reflowRowDivisor;
    }
    // The primary screen feeds t.rows more input rows, worst case oldColumns each.
    contentRows += static_cast<long>(t.rows) * ((oldColumns + reflowRowDivisor) / reflowRowDivisor);
    int outputCapacity = contentRows < maxReflowRows ? static_cast<int>(contentRows) : maxReflowRows;
    if (outputCapacity < newRows)
        outputCapacity = newRows; // never below the visible screen the ring must hold
    std::unique_ptr<ReflowState> stateOwner(new (std::nothrow) ReflowState {});
    std::unique_ptr<ReflowRow[]> ringOwner(new (std::nothrow) ReflowRow[static_cast<std::size_t>(outputCapacity)] {});
    const std::size_t cellCount = static_cast<std::size_t>(outputCapacity) * static_cast<std::size_t>(newColumns);
    std::unique_ptr<Cell[]> cellOwner(new (std::nothrow) Cell[cellCount] {});
    if (!stateOwner || !ringOwner || !cellOwner)
        return std::unexpected(ReflowError::AllocationFailed);
    ReflowState* st = stateOwner.get();
    ReflowRow* ring = ringOwner.get();
    Cell* ringCells = cellOwner.get();
    for (int i = 0; i < outputCapacity; i++)
        ring[i].cells = ringCells + static_cast<size_t>(i) * static_cast<size_t>(newColumns);
    st->ring = ring;
    st->capacity = outputCapacity;
    st->newColumns = newColumns;
    st->terminal = &t;
    reflowPrepareTextSizeTable(t);
    st->hasClampedRuns = hasClampedRuns(t);
    st->hasProportionalRuns = hasProportionalRuns(t);
    st->cursorTargetOrd = -1;
    st->cursorPlaced = false;

    CellScratchBuffer<maxColumns> rowStorage;
    Cell* rowBuffer = rowStorage.data();

    // Walk every input row in display order (scrollback oldest→newest, then
    // visible). A row joins the next (same logical line) when its wrap flag is set.
    int history = t.scrollbackCount;
    bool midLogical = false;
    // Absolute ordinal of the next cell to append; anchors the cursor.
    long fedOrdinal = 0;

    auto contentLengthOf = [&](const Cell* cells, int rowLength, bool /*wrapped*/) -> int {
        // Trim trailing ERASED blanks (codePoint 0) even on a soft-wrapped row: a
        // wide glyph that can't fit the last column wraps WHOLE to the next row and
        // leaves that column an erased blank; feeding it as content injects a stray
        // space at the wrap join, one more per resize.
        // Typed spaces are 0x20, not erased blanks, so real trailing spaces survive.
        int length = rowLength;
        while (length > 0 && reflowCellIsBlank(cells[length - 1]))
            length--;
        return length;
    };

    for (int i = 0; i < history; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        int rowLength = line->length;
        if (rowLength > maxColumns)
            rowLength = maxColumns;
        bool wrapped = line->wrapped;
        int contentLength = contentLengthOf(line->cells, rowLength, wrapped);
        // The rebuild below stores a trimmed line at its trimmed length, and an
        // empty one as a single blank; a stored line that differs from either is
        // not one the store already holds, whatever its cells say.
        const bool isVerbatim = contentLength > 0 && contentLength == line->length;
        if (!midLogical && !wrapped
            && reflowPushWholeLine(st, line->cells, contentLength, line->mark, line->lineAttribute, line->needsShaping, isVerbatim)) {
            fedOrdinal += contentLength;
            continue;
        }
        // The captured row carries its own DECDWL/DECDHL + mark; feed them so
        // a double-width/height history line keeps its attribute through the rewrap.
        reflowFeedRow(st, line->cells, contentLength, line->mark, !midLogical, line->lineAttribute);
        fedOrdinal += contentLength;
        if (wrapped) {
            midLogical = true;
        } else {
            reflowFlushLogical(st);
            midLogical = false;
        }
    }

    // Reflow rewraps the PRIMARY screen. With the alternate active, readRow
    // returns ALTERNATE cells, so flip to primary for the read — else the app's
    // screen gets rewrapped into primary + scrollback and leaks out as garbage on
    // exit. The scrollback read above is unaffected (standalone store).
    const bool shouldFlipForRead = !isPrimaryActive;
    if (shouldFlipForRead)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
    for (int r = 0; r < t.rows; r++) {
        t.grid->readRow(Ubiquitty::Row(r), rowBuffer);
        bool wrapped = s->lineWrapped[r];
        int contentLength = contentLengthOf(rowBuffer, oldColumns, wrapped);
        int feedLength = contentLength;
        // Anchor the cursor ordinal (cells fed + cursor column). Feed up to the
        // cursor column even when it sits in trailing blanks so the reflow places
        // it at its real column, not pulled onto the last non-blank cell (the
        // "cursor jumps one left on resize" flicker).
        if (r == cursorRowIn && st->cursorTargetOrd < 0) {
            int column = cursorColIn;
            if (column < 0)
                column = 0;
            if (column > oldColumns)
                column = oldColumns;
            if (column > feedLength)
                feedLength = column;
            st->cursorTargetOrd = fedOrdinal + column + st->ordinalShift;
        }
        reflowCaptureVisibleAnchors(*st, *s, anchor, r, fedOrdinal, oldColumns, feedLength);
        reflowFeedRow(st, rowBuffer, feedLength, s->mark[r], !midLogical, s->lineAttribute[r]);
        fedOrdinal += feedLength;
        if (wrapped) {
            midLogical = true;
        } else {
            reflowFlushLogical(st);
            midLogical = false;
        }
    }
    if (shouldFlipForRead)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
    // A trailing soft-wrap with no following row (shouldn't persist, but
    // guard anyway) still needs its open row closed.
    if (st->haveCurrentRow || midLogical)
        reflowFlushLogical(st);

    // Cursor never matched a logical line (e.g. the input row index lay
    // past the fed content): pin to the tail of the last emitted row.
    if (st->cursorTargetOrd >= 0 && !st->cursorPlaced && st->count > 0) {
        int index = (st->head + st->count - 1) % st->capacity;
        st->cursorPlaced = true;
        st->cursorRow = index;
        int tail = st->ring[index].length;
        if (tail >= newColumns)
            tail = newColumns - 1;
        st->cursorColumn = tail;
    }

    // Trailing empty logical lines below all content are screen padding, not
    // history — drop them so a mostly-blank reflow doesn't dump blank rows into
    // scrollback. A blank line between content stays; never
    // trim past the cursor's row.
    int cursorSeq = -1;
    if (st->cursorPlaced)
        cursorSeq = (st->cursorRow - st->head + st->capacity) % st->capacity;
    if (st->savedCursor.row >= 0) {
        cursorSeq = std::max(cursorSeq, (st->savedCursor.row - st->head + st->capacity) % st->capacity);
    }
    while (st->count > 1) {
        int lastSeq = st->count - 1;
        if (lastSeq <= cursorSeq)
            break;
        int index = (st->head + lastSeq) % st->capacity;
        ReflowRow* row = &st->ring[index];
        if (row->length != 0 || row->wrapped)
            break;
        st->count--;
    }

    // Old primary cells are now fully captured in the ring; commit the
    // new geometry to the display before writing the reflowed rows back.
    t.grid->resize(newRows, newColumns);

    // The visible-top split retains the mapped row when its suffix fits;
    // otherwise the last newRows stay visible. Unfilled rows are blank below.
    const int scrollbackRows = reflowVisibleStart(*st, newRows);
    const int visibleRows = st->count - scrollbackRows;

    // Rebuild scrollback, less the prefix it already holds. Those rows were
    // pushed from the store's own lines without passing through the staging
    // buffer, so re-appending them would write back the bytes that are there —
    // and this is the whole of a resize where nothing needed rewrapping.
    // History marks ride their rebuilt StoredLine records via scrollbackAppend
    // below, which self-inits the store; trimmed slabs carry no fixed width, so
    // no pre-size is needed.
    // All of it or none of it. Preserving a leading part means dropping the rest,
    // and the store drops a line at a time where clear frees whole slabs — on a
    // history where anything wrapped, that trade costs more than the rebuild it
    // avoids. Whole-history is the case worth having and the one that pays
    // nothing: no line moves, so no line is dropped and none is written.
    const int held = t.lineStore ? t.lineStore->count() : 0;
    const bool isWholeHistoryPreserved = t.lineStore
        && scrollbackRows == held && st->unchangedPrefixRows >= scrollbackRows;
    const int preservedRows = isWholeHistoryPreserved ? scrollbackRows : 0;
    // The rebuild re-appends the same logical lines from the same front, so it
    // resumes the numbering there: a reflow renumbers no line (HistoryLines.h).
    if (t.lineStore && !isWholeHistoryPreserved) {
        const Ubiquitty::LineStore::FrontLine front = t.lineStore->frontLine();
        t.lineStore->clear();
        t.lineStore->restartNumbering(front.number + st->droppedLineEnds, st->rowAfterDropped.value_or(front.continuity));
    }
    t.scrollbackCount = t.lineStore ? t.lineStore->count() : 0;

    Cell blank = defaultBlankCell;

    int cursorVisRow = -1;
    int cursorVisColumn = 0;

    for (int seq = 0; seq < st->count; seq++) {
        int index = (st->head + seq) % st->capacity;
        ReflowRow* row = &st->ring[index];
        bool isCursorRow = (st->cursorPlaced && index == st->cursorRow);
        if (seq < preservedRows) {
            // Already in the store, unrebuilt. A cursor cannot be here: these
            // rows are history that reflow did not touch.
            continue;
        }
        if (seq < scrollbackRows) {
            // A blank logical line stores one default-pen blank so the line
            // survives — not the zeroed ring cell (backgroundIndex 0 = ANSI black,
            // which would paint a black column-0 bar on empty scrollback rows).
            if (row->length > 0)
                Screen::scrollbackAppend(&t, row->cells, row->length, row->lineAttribute, row->mark, row->wrapped, row->needsShaping);
            else
                Screen::scrollbackAppend(&t, &blank, 1, row->lineAttribute, row->mark, row->wrapped, false);
            // A cursor that reflowed into scrollback gets pinned to the
            // top-left of the visible screen (history is above the
            // viewport).
            if (isCursorRow) {
                cursorVisRow = 0;
                cursorVisColumn = 0;
            }
        } else {
            int vr = seq - scrollbackRows;
            // Write the row into the visible primary screen. The active
            // screen pointer may be alternate; setRowSpan targets the
            // active screen, so flip to primary for the writes when the
            // alternate is active, then flip back.
            bool flipped = false;
            if (!isPrimaryActive) {
                t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
                flipped = true;
            }
            t.grid->eraseRow(Ubiquitty::Row(vr), blank);
            if (row->length > 0) {
                Ubiquitty::RowView span { row->cells, row->length };
                t.grid->setRowSpan(Ubiquitty::Row(vr), Ubiquitty::Column(0), span);
            }
            // The flag follows the cells written, as every row this rewrap
            // writes states it: a row index keeps the flag of whatever it held
            // before, and a run landing on an unflagged index draws as one
            // glyph across its claim.
            relatchFlowedRow(t, 0, vr);
            if (flipped)
                t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
            // Carry the logical line's DECDWL/DECDHL through the rewrap (set only
            // on the first row; continuation rows stay Ubiquitty::lineAttributeNormal).
            s->lineAttribute[vr] = row->lineAttribute;
            s->lineWrapped[vr] = row->wrapped;
            s->mark[vr] = row->mark;
            if (isCursorRow) {
                cursorVisRow = vr;
                cursorVisColumn = st->cursorColumn;
            }
        }
    }

    // Blank the visible rows the content didn't fill (shorter than the screen)
    // and clear their per-row flags.
    for (int vr = visibleRows; vr < newRows; vr++) {
        bool flipped = false;
        if (!isPrimaryActive) {
            t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
            flipped = true;
        }
        t.grid->eraseRow(Ubiquitty::Row(vr), blank);
        relatchFlowedRow(t, 0, vr);
        if (flipped)
            t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
        s->lineAttribute[vr] = Ubiquitty::lineAttributeNormal;
        s->lineWrapped[vr] = false;
        s->mark[vr] = 0;
    }
    // The final visible row of a logical line that ends exactly at the screen
    // bottom must not claim a soft-wrap join into a nonexistent row below.
    if (visibleRows > 0 && visibleRows == newRows)
        s->lineWrapped[newRows - 1] = false;

    if (cursorVisRow < 0) {
        // Cursor ordinal landed past the retained content (or none was supplied):
        // clamp to the last written visible row.
        cursorVisRow = (visibleRows > 0) ? (visibleRows - 1) : 0;
        cursorVisColumn = 0;
    }
    if (cursorVisRow >= newRows)
        cursorVisRow = newRows - 1;
    if (cursorVisColumn >= newColumns)
        cursorVisColumn = newColumns - 1;
    if (cursorVisRow < 0)
        cursorVisRow = 0;
    if (cursorVisColumn < 0)
        cursorVisColumn = 0;
    *outCursorRow = cursorVisRow;
    *outCursorColumn = cursorVisColumn;

    // Visible-top reflow maps the saved cursor independently. Other anchors
    // carry it when it coincides with the live cursor, as on DECSET 1049 entry.
    if (st->savedCursor.row >= 0) {
        reflowRestoreSavedCursor(*st, *s, scrollbackRows, newRows);
    } else if (s->hasSavedCursor && s->savedX == cursorColIn && s->savedY == cursorRowIn) {
        s->savedX = cursorVisColumn;
        s->savedY = cursorVisRow;
    }
    return {};
}

} // namespace Ubiquitty

// The margin a deferred-wrap latch arms at: the region's right edge under
// DECSLRM, else the row's last column. Read after the margin reclamp, so it
// always sees new-geometry margins.
enum class WrapMargin : std::uint8_t {
    RowEdge,
    RegionEdge
};

static int wrapLatchMargin(const Ubiquitty::Screen::State* s, WrapMargin margin, int newColumns)
{
    return margin == WrapMargin::RegionEdge ? s->scrollRight : newColumns - 1;
}

// Resize either screen's scroll region and clamp cursor for the new dims.
// The caller blanks newly revealed geometry because cell storage lives in
// CellGrid, not Screen.
static void screenResize(Ubiquitty::Screen::State* s, int oldRows, int oldColumns, int newRows, int newColumns,
    WrapMargin margin)
{
    int r, c;

    // The deferred-wrap latch arms when a print fills the last column; it is
    // carried below, not cleared, because a resize is not a print.
    const bool isWrapPending = s->wrapPending;

    // Blanking newly-revealed cells on grow now happens in resize
    // (caller), which has the Ubiquitty::Terminal* needed to reach CellGrid.
    (void)oldRows;
    (void)oldColumns;
    (void)newRows;
    (void)newColumns;
    (void)r;
    (void)c;

    if (s->scrollTop == 0 && s->scrollBottom == oldRows - 1) {
        s->scrollBottom = newRows - 1;
    } else {
        // Clamp both edges into the new range. A region whose top now
        // sits past the new bottom (small newRows) is invalid; the
        // degenerate-region recovery below resets it to full screen.
        if (s->scrollTop < 0)
            s->scrollTop = 0;
        if (s->scrollTop >= newRows)
            s->scrollTop = newRows - 1;
        if (s->scrollBottom < 0)
            s->scrollBottom = 0;
        if (s->scrollBottom >= newRows)
            s->scrollBottom = newRows - 1;
    }
    // DECSTBM requires strictly scrollTop < scrollBottom (a one-line
    // region is meaningless). If shrinking flattened the region, fall
    // back to the full screen so subsequent linefeeds keep working.
    if (s->scrollTop >= s->scrollBottom) {
        s->scrollTop = 0;
        s->scrollBottom = newRows - 1;
    }
    // savedScrollTop/Bottom are not part of the DECSC/DECRC record
    // (restoreCursor never applies them); they ride this reclamp only so the Sync
    // snapshot wire format doesn't serialize bounds stale to the new geometry.
    if (s->hasSavedCursor) {
        if (s->savedScrollTop == 0 && s->savedScrollBottom == oldRows - 1) {
            s->savedScrollBottom = newRows - 1;
        } else {
            if (s->savedScrollTop < 0)
                s->savedScrollTop = 0;
            if (s->savedScrollTop >= newRows)
                s->savedScrollTop = newRows - 1;
            if (s->savedScrollBottom < 0)
                s->savedScrollBottom = 0;
            if (s->savedScrollBottom >= newRows)
                s->savedScrollBottom = newRows - 1;
        }
        if (s->savedScrollTop >= s->savedScrollBottom) {
            s->savedScrollTop = 0;
            s->savedScrollBottom = newRows - 1;
        }
    }
    // The remembered pairs describe a layout the new width has redrawn.
    if (newColumns != oldColumns)
        Ubiquitty::Screen::forgetMargins(*s);
    // DECSLRM margins: preserve a non-default region if it still fits, else reset
    // to full width [0, newColumns-1].
    if (s->scrollLeft == 0 && s->scrollRight == oldColumns - 1) {
        s->scrollRight = newColumns - 1;
    } else {
        if (s->scrollLeft < 0)
            s->scrollLeft = 0;
        if (s->scrollLeft >= newColumns)
            s->scrollLeft = newColumns - 1;
        if (s->scrollRight < 0)
            s->scrollRight = 0;
        if (s->scrollRight >= newColumns)
            s->scrollRight = newColumns - 1;
    }
    if (s->scrollLeft >= s->scrollRight) {
        s->scrollLeft = 0;
        s->scrollRight = newColumns - 1;
    }
    if (s->cursorX >= newColumns)
        s->cursorX = newColumns - 1;
    if (s->cursorY >= newRows)
        s->cursorY = newRows - 1;
    if (s->cursorX < 0)
        s->cursorX = 0;
    if (s->cursorY < 0)
        s->cursorY = 0;
    // The latch survives exactly while the cursor still sits at the margin it
    // armed at — and that margin is the ACTIVE right margin, not the edge:
    // under DECSLRM the parser arms the latch at scrollRight, and a
    // height-only resize preserves scrollRight above. A narrowing clamps the
    // cursor to the margin it still faces; a widening (or a margin reset to
    // full width, which forgetMargins above arranged) moves the margin past
    // the cursor — room now, so the next print lands instead of wrapping.
    s->wrapPending = isWrapPending && s->cursorX == wrapLatchMargin(s, margin, newColumns);
}

// Inline-image reflow remap. Images overlay mostly-blank cells, so a
// content-ordinal anchor fails; anchor instead to the logical-line index (which
// a width rewrap preserves), captured before the rewrap and located after.

// Wrap flag of feed row `f`: [0, scrollbackCount) is scrollback, then the
// primary screen rows. A non-wrapped row ends its logical line.
static bool imageReflowRowWrapped(Ubiquitty::Terminal& t, int f)
{
    int history = t.scrollbackCount;
    if (f < history) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, f);
        return line ? line->wrapped : false;
    }
    int r = f - history;
    return (r >= 0 && r < t.rows) ? t.screens.primary.lineWrapped[r] : false;
}

namespace Ubiquitty {

bool shouldReflowOnResize(const Ubiquitty::Terminal* t, bool foregroundIsShell)
{
    // mintty ?7723 reset: the program asked for geometry-only resizes.
    if (t && !t->modes.shouldRewrapOnResize)
        return false;
    // Reflow unless a non-shell foreground app owns the primary (non-alternate)
    // screen — it repaints its own rows on SIGWINCH, so our rewrap only fights
    // it (the less -X dup/jitter). The shell, and any alternate-screen app, reflow
    // normally. No display / alternate screen active → reflow.
    const bool primaryAppOwnsScreen = !foregroundIsShell && t && t->grid && !t->grid->isAlternateActive();
    return !primaryAppOwnsScreen;
}

// Strict bottom-anchor for the gate-suppressed (allowReflow=false) ACTIVE primary
// screen across a height-only resize. Unlike reflowHeightOnly's content-aware
// distribution, every shrunk row is pushed into scrollback from the TOP and every
// grown row pulls history back, so the BOTTOM rows stay glued to the fixed edge —
// matching a bottom-anchored inline TUI that repaints only its bottom band. Caller
// sequences the two directions around CellGrid::resize: shrink while the display
// still has oldRows (source readable), grow after it grew (destination writable).
static void anchorPrimaryRowsBottom(Ubiquitty::Terminal& t, int oldRows, int newRows)
{
    Screen::State* screen = &t.screens.primary;
    const int columns = t.columns;
    const int delta = newRows - oldRows;
    Cell blank = defaultBlankCell;
    CellScratchBuffer<maxColumns> rowStorage;
    Cell* rowBuffer = rowStorage.data();
    auto moveRow = [&](int dst, int src) {
        t.grid->readRow(Ubiquitty::Row(src), rowBuffer);
        t.grid->eraseRow(Ubiquitty::Row(dst), blank);
        t.grid->setRowSpan(Ubiquitty::Row(dst), Ubiquitty::Column(0), Ubiquitty::RowView { rowBuffer, columns });
        screen->lineAttribute[dst] = screen->lineAttribute[src];
        screen->lineWrapped[dst] = screen->lineWrapped[src];
        screen->mark[dst] = screen->mark[src];
        relatchFlowedRow(t, 0, dst);
    };
    if (delta < 0) {
        const int push = -delta;
        for (int row = 0; row < push; row++) {
            t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
            // Trim a hard-ended row to its content (a soft-wrap keeps the
            // full width — its trailing cells belong to the logical line),
            // mirroring reflowHeightOnly's storage shape.
            int width = columns;
            if (!screen->lineWrapped[row]) {
                while (width > 0 && reflowCellIsBlank(rowBuffer[width - 1]))
                    width--;
            }
            Screen::scrollbackAppend(&t, rowBuffer, width, screen->lineAttribute[row], screen->mark[row], screen->lineWrapped[row], t.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping));
        }
        for (int dst = 0; dst < newRows; dst++) // dst < src: low->high
            moveRow(dst, dst + push);
    } else {
        const int historyRows = t.scrollbackCount;
        const int pulled = (delta < historyRows) ? delta : historyRows;
        // Blank rows land between the pulled history and the content: the
        // lines below them are not the lines their numbers name any more.
        Screen::notePositionsBlankRowsInsertedAtSeam(t, pulled, delta - pulled);
        const std::uint64_t nextOrdinalBefore = t.lineStore ? t.lineStore->nextOrdinal() : 0;
        for (int dst = newRows - 1; dst >= delta; dst--) // dst > src: high->low
            moveRow(dst, dst - delta);
        // Blanks on top, then the pulled history directly above the
        // content (oldest pulled line first), exactly the scrollback
        // adjacency the lines had before they were pushed.
        for (int dst = 0; dst < delta - pulled; dst++) {
            t.grid->eraseRow(Ubiquitty::Row(dst), blank);
            screen->lineAttribute[dst] = Ubiquitty::lineAttributeNormal;
            screen->lineWrapped[dst] = false;
            screen->mark[dst] = 0;
            relatchFlowedRow(t, 0, dst);
        }
        for (int i = 0; i < pulled; i++) {
            const int dst = delta - pulled + i;
            const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, historyRows - pulled + i);
            t.grid->eraseRow(Ubiquitty::Row(dst), blank);
            if (line && line->length > 0)
                t.grid->setRowSpan(Ubiquitty::Row(dst), Ubiquitty::Column(0), Ubiquitty::RowView { const_cast<Cell*>(line->cells), line->length });
            screen->lineAttribute[dst] = line ? line->lineAttribute : static_cast<uint8_t>(Ubiquitty::lineAttributeNormal);
            screen->lineWrapped[dst] = line ? line->wrapped : false;
            screen->mark[dst] = line ? line->mark : 0;
            relatchFlowedRow(t, 0, dst);
        }
        if (t.lineStore)
            t.lineStore->dropNewest(pulled);
        t.scrollbackCount = t.lineStore ? t.lineStore->count() : 0;
        if (delta > pulled)
            Screen::startFreshLineNames(t);
        else
            Screen::notePositionsPulledFromHistory(t, nextOrdinalBefore);
    }
    // A content row at the screen bottom never joins a soft-wrap below it.
    if (newRows > 0)
        screen->lineWrapped[newRows - 1] = false;
}

// Interactive (live-drag) width step: rewrap only the visible screen plus the
// newest history logical lines needed to keep the viewport full, leaving the rest
// of scrollback at its stored width. Bounded work per step (O(rows)); the caller
// marks the remaining history wrap-stale (pendingHistoryRewrapColumns) and the one
// full rewrap runs on settle. Stored-width history reads correctly meanwhile (each
// read path uses the line's own stored length).
[[nodiscard]] static std::expected<void, ReflowError> reflowInteractive(Ubiquitty::Terminal& t, int oldColumns, int newRows, int newColumns, Modes::AnchorMode anchor, int cursorRowIn, int cursorColIn, int* outCursorRow, int* outCursorColumn)
{
    assert(t.grid);

    Screen::State* s = &t.screens.primary;
    const bool isPrimaryActive = (t.screens.active == &t.screens.primary);
    const int history = t.scrollbackCount;

    // Pull set: the trailing wrapped run joining the screen (the logical line
    // across the history/screen seam must rewrap whole or the seam tears),
    // extended backwards by whole logical lines until the pulled rows refill the
    // viewport. Capped so one pathological line can't drag all history into the
    // per-step cost (the settle rewrap reconciles).
    int pullStart = history;
    while (pullStart > 0) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, pullStart - 1);
        if (!line || !line->wrapped)
            break;
        pullStart--;
    }
    const int pullCapacity = (history < newRows * 4) ? history : newRows * 4;
    while (pullStart > 0 && (history - pullStart) < newRows) {
        int lineStart = pullStart - 1;
        while (lineStart > 0) {
            const Ubiquitty::Screen::ScrollLine* previous = Ubiquitty::Screen::scrollbackLine(&t, lineStart - 1);
            if (!previous || !previous->wrapped)
                break;
            lineStart--;
        }
        if ((history - lineStart) > pullCapacity)
            break;
        pullStart = lineStart;
    }
    const int pulledRows = history - pullStart;

    // Ring capacity: reflowPrimary's per-row ceiling, summed over only the
    // pulled + screen rows.
    const int reflowRowDivisor = newColumns > 1 ? newColumns - 1 : 1;
    long contentRows = 0;
    for (int i = pullStart; i < history; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        int rowLength = line->length;
        if (rowLength > maxColumns)
            rowLength = maxColumns;
        contentRows += (rowLength + reflowRowDivisor) / reflowRowDivisor;
    }
    contentRows += static_cast<long>(t.rows) * ((oldColumns + reflowRowDivisor) / reflowRowDivisor);
    int outputCapacity = static_cast<int>(contentRows);
    if (outputCapacity < newRows)
        outputCapacity = newRows;
    std::unique_ptr<ReflowState> stateOwner(new (std::nothrow) ReflowState {});
    std::unique_ptr<ReflowRow[]> ringOwner(new (std::nothrow) ReflowRow[static_cast<std::size_t>(outputCapacity)] {});
    const std::size_t cellCount = static_cast<std::size_t>(outputCapacity) * static_cast<std::size_t>(newColumns);
    std::unique_ptr<Cell[]> cellOwner(new (std::nothrow) Cell[cellCount] {});
    if (!stateOwner || !ringOwner || !cellOwner)
        return std::unexpected(ReflowError::AllocationFailed);
    ReflowState* st = stateOwner.get();
    ReflowRow* ring = ringOwner.get();
    Cell* ringCells = cellOwner.get();
    for (int i = 0; i < outputCapacity; i++)
        ring[i].cells = ringCells + static_cast<size_t>(i) * static_cast<size_t>(newColumns);
    st->ring = ring;
    st->capacity = outputCapacity;
    st->newColumns = newColumns;
    st->terminal = &t;
    reflowPrepareTextSizeTable(t);
    st->hasClampedRuns = hasClampedRuns(t);
    st->hasProportionalRuns = hasProportionalRuns(t);
    st->cursorTargetOrd = -1;
    st->cursorPlaced = false;

    CellScratchBuffer<maxColumns> rowStorage;
    Cell* rowBuffer = rowStorage.data();
    bool isMidLogical = false;
    long fedOrdinal = 0;

    auto contentLengthOf = [&](const Cell* cells, int rowLength, bool isWrapped) -> int {
        if (isWrapped)
            return rowLength;
        int length = rowLength;
        while (length > 0 && reflowCellIsBlank(cells[length - 1]))
            length--;
        return length;
    };

    // Feed the pulled tail at each line's own stored length (mixed widths are
    // fine — the wrap flag, not the width, carries the logical-line structure).
    for (int i = pullStart; i < history; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        int rowLength = line->length;
        if (rowLength > maxColumns)
            rowLength = maxColumns;
        const bool isWrapped = line->wrapped;
        const int contentLength = contentLengthOf(line->cells, rowLength, isWrapped);
        reflowFeedRow(st, line->cells, contentLength, line->mark, !isMidLogical, line->lineAttribute);
        fedOrdinal += contentLength;
        if (isWrapped) {
            isMidLogical = true;
        } else {
            reflowFlushLogical(st);
            isMidLogical = false;
        }
    }
    // The ring copied the pulled cells; release them from the store's newest
    // end so the overflow re-append below lands where they came from.
    if (pulledRows > 0 && t.lineStore) {
        t.lineStore->dropNewest(pulledRows);
        t.scrollbackCount = t.lineStore->count();
    }

    // Feed the visible primary rows — identical to reflowPrimary's screen walk,
    // including the alternate-active flip and the cursor's ordinal anchor.
    const bool shouldFlipForRead = !isPrimaryActive;
    if (shouldFlipForRead)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
    for (int r = 0; r < t.rows; r++) {
        t.grid->readRow(Ubiquitty::Row(r), rowBuffer);
        const bool isWrapped = s->lineWrapped[r];
        const int contentLength = contentLengthOf(rowBuffer, oldColumns, isWrapped);
        int feedLength = contentLength;
        if (r == cursorRowIn && st->cursorTargetOrd < 0) {
            int column = cursorColIn;
            if (column < 0)
                column = 0;
            if (column > oldColumns)
                column = oldColumns;
            if (column > feedLength)
                feedLength = column;
            st->cursorTargetOrd = fedOrdinal + column + st->ordinalShift;
        }
        reflowCaptureVisibleAnchors(*st, *s, anchor, r, fedOrdinal, oldColumns, feedLength);
        reflowFeedRow(st, rowBuffer, feedLength, s->mark[r], !isMidLogical, s->lineAttribute[r]);
        fedOrdinal += feedLength;
        if (isWrapped) {
            isMidLogical = true;
        } else {
            reflowFlushLogical(st);
            isMidLogical = false;
        }
    }
    if (shouldFlipForRead)
        t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
    if (st->haveCurrentRow || isMidLogical)
        reflowFlushLogical(st);

    if (st->cursorTargetOrd >= 0 && !st->cursorPlaced && st->count > 0) {
        const int index = (st->head + st->count - 1) % st->capacity;
        st->cursorPlaced = true;
        st->cursorRow = index;
        int tail = st->ring[index].length;
        if (tail >= newColumns)
            tail = newColumns - 1;
        st->cursorColumn = tail;
    }

    // Trailing blank rows below all content are screen padding, not history
    // (same trim as reflowPrimary; never past the cursor's row).
    int cursorSeq = -1;
    if (st->cursorPlaced)
        cursorSeq = (st->cursorRow - st->head + st->capacity) % st->capacity;
    if (st->savedCursor.row >= 0) {
        cursorSeq = std::max(cursorSeq, (st->savedCursor.row - st->head + st->capacity) % st->capacity);
    }
    while (st->count > 1) {
        const int lastSeq = st->count - 1;
        if (lastSeq <= cursorSeq)
            break;
        const int index = (st->head + lastSeq) % st->capacity;
        ReflowRow* row = &st->ring[index];
        if (row->length != 0 || row->wrapped)
            break;
        st->count--;
    }

    t.grid->resize(newRows, newColumns);

    // Rows above the chosen visible start re-append to the scrollback tail at
    // the new width, preserving history order and the older stored-width rows.
    const int totalRows = st->count;
    const int overflowRows = reflowVisibleStart(*st, newRows);
    const int visibleRows = totalRows - overflowRows;

    Cell blank = defaultBlankCell;
    int cursorVisRow = -1;
    int cursorVisColumn = 0;

    for (int seq = 0; seq < totalRows; seq++) {
        const int index = (st->head + seq) % st->capacity;
        ReflowRow* row = &st->ring[index];
        const bool isCursorRow = (st->cursorPlaced && index == st->cursorRow);
        if (seq < overflowRows) {
            if (row->length > 0)
                Screen::scrollbackAppend(&t, row->cells, row->length, row->lineAttribute, row->mark, row->wrapped, row->needsShaping);
            else
                Screen::scrollbackAppend(&t, &blank, 1, row->lineAttribute, row->mark, row->wrapped, false);
            if (isCursorRow) {
                cursorVisRow = 0;
                cursorVisColumn = 0;
            }
        } else {
            const int vr = seq - overflowRows;
            bool didFlip = false;
            if (!isPrimaryActive) {
                t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
                didFlip = true;
            }
            t.grid->eraseRow(Ubiquitty::Row(vr), blank);
            if (row->length > 0) {
                Ubiquitty::RowView span { row->cells, row->length };
                t.grid->setRowSpan(Ubiquitty::Row(vr), Ubiquitty::Column(0), span);
            }
            relatchFlowedRow(t, 0, vr);
            if (didFlip)
                t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
            s->lineAttribute[vr] = row->lineAttribute;
            s->lineWrapped[vr] = row->wrapped;
            s->mark[vr] = row->mark;
            if (isCursorRow) {
                cursorVisRow = vr;
                cursorVisColumn = st->cursorColumn;
            }
        }
    }

    for (int vr = visibleRows; vr < newRows; vr++) {
        bool didFlip = false;
        if (!isPrimaryActive) {
            t.grid->setAlternateActive(Ubiquitty::ScreenMode::Primary);
            didFlip = true;
        }
        t.grid->eraseRow(Ubiquitty::Row(vr), blank);
        relatchFlowedRow(t, 0, vr);
        if (didFlip)
            t.grid->setAlternateActive(Ubiquitty::ScreenMode::Alternate);
        s->lineAttribute[vr] = Ubiquitty::lineAttributeNormal;
        s->lineWrapped[vr] = false;
        s->mark[vr] = 0;
    }
    if (visibleRows > 0 && visibleRows == newRows)
        s->lineWrapped[newRows - 1] = false;

    if (cursorVisRow < 0) {
        cursorVisRow = (visibleRows > 0) ? (visibleRows - 1) : 0;
        cursorVisColumn = 0;
    }
    if (cursorVisRow >= newRows)
        cursorVisRow = newRows - 1;
    if (cursorVisColumn >= newColumns)
        cursorVisColumn = newColumns - 1;
    *outCursorRow = cursorVisRow;
    *outCursorColumn = cursorVisColumn;
    reflowRestoreSavedCursor(*st, *s, overflowRows, newRows);
    return {};
}

Screen::ResizeOutcome Screen::resizeGrid(Ubiquitty::Terminal* t, int newRows, int newColumns, Modes::ReflowMode reflow, Modes::AnchorMode anchor, Modes::ResizeUrgency urgency)
{
    if (!t)
        return {};
    const bool allowReflowFlag = (reflow == Modes::ReflowMode::Reflow);
    const bool isAnchoredToBottom = (anchor == Modes::AnchorMode::Bottom);
    int oldRows, oldColumns;
    newRows = clampGridRows(newRows);
    newColumns = clampGridColumns(newColumns);
    // Reserve the destination before changing state. The reflow helpers also
    // refuse before mutation when their scratch storage cannot be allocated.
    if (t->grid && !t->grid->reserveCapacity(newRows, newColumns))
        return {};

    // A dimension no-op still settles a deferred history rewrap (the drag
    // ended back at its starting width, but the re-appended overflow rows and
    // the wrap-stale marker are real).
    if (newRows == t->rows && newColumns == t->columns && !(t->pendingHistoryRewrapColumns != 0 && urgency == Modes::ResizeUrgency::Settled)) {
        // The cell COUNT did not change; the cells' pixels may have. A font
        // size or family change re-lays every glyph inside a window whose
        // dimensions divide into the same grid, and arrives here as a resize to
        // the size already held. The shadow mirrors the bitmap rather than the
        // cells, so it no longer describes what is on screen, and a paint that
        // trusts it offers no runs and leaves the old glyphs standing.
        if (t->grid)
            t->grid->requestFullRedraw();
        return {};
    }

    oldRows = t->rows;
    oldColumns = t->columns;
    // A new page geometry moves the positions a DAQ marked; the areas' starts
    // are not followed through reflow, so they are forgotten.
    Screen::forgetQualifiedAreas(*t);
    // The pending prompt is an absolute row, which the resize renumbers: it
    // rides a position across, so a clear to the prompt still finds the
    // prompt's line. The shell redraws its prompt at will, so an erase of the
    // line does not lose it. A prompt recorded on the alternate screen has no
    // line on the primary to ride.
    std::optional<Screen::LogicalPosition> pendingPrompt;
    if (t->pendingPromptRow >= 0 && !t->screens.isUsingAlternateScreen) {
        if (const auto captured = Screen::capturePosition(*t, t->pendingPromptRow, 0))
            pendingPrompt = *captured;
    }

    int reflowCursorRow = -1, reflowCursorColumn = 0;
    // Reflow when content must move between the visible rows and scrollback:
    // width change (also rewraps), height shrink (top → history), or a grow
    // with scrollback to pull into the new top rows. A grow with no scrollback
    // stays top-anchored so DECDWL/DECDHL survive.
    const bool hasColumnChange = (newColumns != oldColumns);
    // A Settled resize consumes any deferred history rewrap, even when the drag
    // ended back at its starting width — the wrap-stale marker is real.
    const bool hasPendingSettle = (t->pendingHistoryRewrapColumns != 0) && (urgency == Modes::ResizeUrgency::Settled);
    const Modes::AnchorMode reflowAnchor = hasPendingSettle && newRows == oldRows && !hasColumnChange
        ? t->pendingHistoryRewrapAnchor
        : anchor;
    // Any used inline-image placement forces the full path: the placement remap
    // sweeps the whole feed, which would defeat the deferral (correctness over
    // deferral, and image apps under a reflowing width-drag are rare).
    const bool hasPlacements = t->images.livePlacementCount != 0 || !t->richTextBlocks.isEmpty();
    const bool shouldDeferHistory = (urgency == Modes::ResizeUrgency::Interactive) && hasColumnChange && allowReflowFlag && (t->grid != nullptr) && !hasPlacements;
    const bool needsWidthRewrap = hasColumnChange || hasPendingSettle;
    const bool shouldReflow = allowReflowFlag && (t->grid != nullptr) && (needsWidthRewrap || newRows < oldRows || (newRows > oldRows && t->scrollbackCount > 0));
    // Bottom-anchor the active primary across a height-only change: the
    // gate-suppressed primary exchanges rows with scrollback (a reflowing primary
    // already bottom-anchors). The alternate screen never takes the anchor: a
    // full-screen program lays out from row 0 and repaints the rest on SIGWINCH,
    // so a shrink drops its bottom rows and keeps the top bar it drew. Cursor
    // positions captured before any shift so the post-screenResize adjustment is
    // direction-independent.
    const bool isRowsOnlyChange = !needsWidthRewrap && (newRows != oldRows) && (t->grid != nullptr);
    const bool shouldAnchorPrimary = isAnchoredToBottom && isRowsOnlyChange && (t->screens.active == &t->screens.primary) && !allowReflowFlag;
    const int primCursorY0 = t->screens.primary.cursorY;
    const int primSavedY0 = t->screens.primary.savedY;
    // The latch the rewrap must carry: screenResize below decides on the
    // pre-rewrap cursor, which the tracked position then replaces.
    const bool isWrapPending = t->screens.primary.wrapPending;
    // Shrink shifts while the display still spans oldRows (the doomed top
    // rows must be readable); the grow shift waits until the display grew.
    if (shouldAnchorPrimary && newRows < oldRows)
        anchorPrimaryRowsBottom(*t, oldRows, newRows);

    // A width rewrap moves an image placement's physical row, so anchor each to
    // its rewrap-stable logical-line index. All placements share one feed sweep
    // (line captured before, resolved to a first row after). A height-only change
    // never rewraps, so a placement rides the same plain rowShift as the cursor.
    const bool isHeightOnlyReflow = shouldReflow && !needsWidthRewrap;
    // An entry is an image placement's slot, or a primary-screen rich text
    // block's position offset by the image slot count: both ride the same
    // logical-line anchor.
    struct {
        int slot;
        int feedRow;
        int line;
    } placementOrder[Image::maxTerminalImagePlacementCount + RichText::maxBlocksPerScreen];
    int placementCount = 0;
    const std::span<RichText::Block> primaryBlocks = t->richTextBlocks.mutableBlocks(ScreenMode::Primary);
    // shouldDeferHistory never coexists with a used placement (gated above), so the
    // O(history) placement sweep never runs on an Interactive step.
    if (shouldReflow && !isHeightOnlyReflow && !shouldDeferHistory) {
        // Used placements by ascending pre-reflow feed row — which also orders
        // their logical-line index (a monotone function of feed row), so both
        // sweeps advance one running cursor. Insertion sort over <= 32 slots.
        for (int i = 0; i < Image::maxTerminalImagePlacementCount; i++) {
            const Image::TerminalImagePlacement* p = Ubiquitty::Image::placement(t, i);
            if (!p || !p->used)
                continue;
            int feedRow = t->scrollbackCount + p->cursorY;
            if (feedRow < 0)
                feedRow = 0;
            int j = placementCount++;
            while (j > 0 && placementOrder[j - 1].feedRow > feedRow) {
                placementOrder[j] = placementOrder[j - 1];
                j--;
            }
            placementOrder[j] = { i, feedRow, -1 };
        }
        for (std::size_t position = 0; position < primaryBlocks.size(); ++position) {
            const int feedRow = std::max(0, t->scrollbackCount + primaryBlocks[position].row);
            int j = placementCount++;
            while (j > 0 && placementOrder[j - 1].feedRow > feedRow) {
                placementOrder[j] = placementOrder[j - 1];
                j--;
            }
            placementOrder[j] = { Image::maxTerminalImagePlacementCount + static_cast<int>(position), feedRow, -1 };
        }
        // Sweep 1: each placement's logical-line index = count of non-wrapped
        // (logical-line-ending) rows strictly before its feed row.
        int lines = 0, f = 0;
        for (int k = 0; k < placementCount; k++) {
            while (f < placementOrder[k].feedRow) {
                if (!imageReflowRowWrapped(*t, f))
                    lines++;
                f++;
            }
            placementOrder[k].line = lines;
        }
    }

    int reflowRowShift = 0;
    if (shouldReflow) {
        // Pure height change → cheap boundary-shift (O(rows)). Interactive width
        // step → rewrap only the screen band, defer history to settle. Every other
        // width change (or a pending settle) → full rewrap.
        if (isHeightOnlyReflow) {
            reflowHeightOnly(*t, newRows, reflowAnchor, &reflowCursorRow, &reflowCursorColumn, &reflowRowShift);
        } else {
            // A rewrap gives the primary's rows new ordinals; the pins ride
            // their lines' numbers across it, which a rewrap keeps.
            Screen::PositionReflow positions;
            Screen::beginPositionReflow(*t, positions);
            const auto result = shouldDeferHistory
                ? reflowInteractive(*t, oldColumns, newRows, newColumns, reflowAnchor, t->screens.primary.cursorY, t->screens.primary.cursorX, &reflowCursorRow, &reflowCursorColumn)
                : reflowPrimary(*t, oldColumns, newRows, newColumns, reflowAnchor, t->screens.primary.cursorY, t->screens.primary.cursorX, &reflowCursorRow, &reflowCursorColumn);
            if (!result)
                return {};
            Screen::finishPositionReflow(*t, positions);
        }
    }
    // The wrap-stale marker tracks the deferral: set to the target width while any
    // stored line awaits the settle rewrap, cleared by the full rewrap. A
    // height-only step leaves an existing marker in place.
    if (shouldReflow && !isHeightOnlyReflow)
        t->pendingHistoryRewrapColumns = (shouldDeferHistory && t->scrollbackCount > 0) ? newColumns : 0;
    t->pendingHistoryRewrapAnchor = t->pendingHistoryRewrapColumns != 0 ? reflowAnchor : Modes::AnchorMode::Top;

    t->rows = newRows;
    t->columns = newColumns;
    // The rows a caller named before this call are not the rows those numbers
    // reach now: the rewrap repacked them and the aging shifted them.
    t->absoluteRowEpoch++;

    // Sized blocks last, after the new dimensions are in place, because what a
    // block's chain may claim is measured against them. Every resize path can
    // separate a block's rows — the height shrink ages its top rows into
    // history, the rewrap repacks each row on its own, the alternate is
    // shifted and resized untouched by any control — and what a scrub cannot
    // put back together it erases.
    Screen::scrubRearrangedScreens(*t);

    // Re-anchor images: ride the plain rowShift on a height-only change, or map
    // every captured logical line back to its first row in one post-rewrap sweep.
    if (isHeightOnlyReflow && reflowRowShift != 0) {
        for (int i = 0; i < Image::maxTerminalImagePlacementCount; i++) {
            if (!t->images.placements[i].used)
                continue;
            int row = t->images.placements[i].cursorY + reflowRowShift;
            if (row < -t->scrollbackLimit)
                row = -t->scrollbackLimit;
            if (row > maxRows)
                row = maxRows;
            t->images.placements[i].cursorY = static_cast<int16_t>(std::max(row, Image::minPlacementRow));
        }
        RichText::shiftBlocks(*t, ScreenMode::Primary, reflowRowShift);
    } else if (shouldReflow && !isHeightOnlyReflow && !shouldDeferHistory) {
        // Sweep 2: first physical feed row of each captured logical line; one
        // running cursor continues across placements in line order (a line
        // beyond the feed's last resolves to its end, matching a lost anchor).
        const int total = t->scrollbackCount + t->rows;
        int completed = 0, f = 0;
        for (int k = 0; k < placementCount; k++) {
            const int target = placementOrder[k].line;
            int feedRow;
            if (target <= 0) {
                feedRow = 0;
            } else {
                while (f < total && completed < target) {
                    const bool isUnwrapped = !imageReflowRowWrapped(*t, f);
                    f++;
                    if (isUnwrapped)
                        completed++;
                }
                feedRow = (completed == target) ? f : total;
            }
            int row = feedRow - t->scrollbackCount;
            if (row < -t->scrollbackLimit)
                row = -t->scrollbackLimit;
            if (row > maxRows)
                row = maxRows;
            const int slot = placementOrder[k].slot;
            if (slot >= Image::maxTerminalImagePlacementCount)
                primaryBlocks[static_cast<std::size_t>(slot - Image::maxTerminalImagePlacementCount)].row = row;
            else
                t->images.placements[slot].cursorY = static_cast<int16_t>(std::max(row, Image::minPlacementRow));
        }
    }
    // Keep the margin-bell threshold at nMarginBell (10) columns from the right.
    t->marginBellColumn = (newColumns > 10) ? (newColumns - 10) : 0;
    const WrapMargin marginRule = t->modes.leftRightMarginMode ? WrapMargin::RegionEdge : WrapMargin::RowEdge;
    screenResize(&t->screens.primary, oldRows, oldColumns, newRows, newColumns, marginRule);
    screenResize(&t->screens.alternate, oldRows, oldColumns, newRows, newColumns, marginRule);
    // An anchored cursor rides the row shift (screenResize only clamped it),
    // computed from the pre-shift rows so the result is direction-independent.
    if (shouldAnchorPrimary) {
        const int shiftDelta = newRows - oldRows;
        auto clampRow = [newRows](int v) { return v < 0 ? 0 : (v >= newRows ? newRows - 1 : v); };
        t->screens.primary.cursorY = clampRow(primCursorY0 + shiftDelta);
        if (t->screens.primary.hasSavedCursor)
            t->screens.primary.savedY = clampRow(primSavedY0 + shiftDelta);
        // Landed image placements ride too (cursorY is screen-relative
        // and may extend into scrollback as negative rows; same clamp
        // band as the reflow remap above).
        for (int i = 0; i < Image::maxTerminalImagePlacementCount; i++) {
            if (!t->images.placements[i].used)
                continue;
            int row = t->images.placements[i].cursorY + shiftDelta;
            if (row < -t->scrollbackLimit)
                row = -t->scrollbackLimit;
            if (row > maxRows)
                row = maxRows;
            t->images.placements[i].cursorY = static_cast<int16_t>(std::max(row, Image::minPlacementRow));
        }
        RichText::shiftBlocks(*t, ScreenMode::Primary, shiftDelta);
    }
    if (shouldReflow && reflowCursorRow >= 0) {
        t->screens.primary.cursorX = reflowCursorColumn;
        t->screens.primary.cursorY = reflowCursorRow;
        // The rewrap tracked the cursor by absolute ordinal; the latch rides
        // the tracked position under the same margin rule — decided here,
        // because screenResize above could only read the pre-rewrap cursor.
        // Margins are already reclamped, so the margin is new-geometry.
        t->screens.primary.wrapPending = isWrapPending
            && reflowCursorColumn == wrapLatchMargin(&t->screens.primary, marginRule, newColumns);
    }
    // Rows revealed by a grow get NORMAL metadata from CellGrid::resize (which owns
    // m_rowMeta for both screens).
    if (t->grid) {
        // The reflow path commits CellGrid::resize itself; when it didn't run
        // (height grow / no width change) this is the sole resize call. Pass the
        // caller's reflow mode through — a Reflow-mode caller can still land here
        // (a plain height grow with no scrollback), and its full-width stamp is
        // correct since newColumns == oldColumns there.
        if (!shouldReflow)
            t->grid->resize(newRows, newColumns, reflow);
        // The anchored GROW shifts run here — the display now spans
        // newRows, so the shifted-down destination rows are writable.
        if (shouldAnchorPrimary && newRows > oldRows)
            anchorPrimaryRowsBottom(*t, oldRows, newRows);
        // Blank cells that become visible on grow so old content from a
        // previous larger size doesn't reappear. Geometry-created cells
        // are not application erases: seed them with the default blank so
        // a fullscreen app's last SGR pen doesn't tint the exposed band
        // before its resize repaint arrives.
        //
        // FIXED mode skips this entirely: the peer owns the content and never
        // rewrapped, so "newly revealed" columns aren't never-before-seen —
        // growing back after a shrink reveals real content the backing store still
        // holds (unconditional blanking lost earlier columns of a shrunk-then-grown
        // tmux pane). When the primary was reflowed its cells are already correct,
        // so route active-screen blanking only to the screen that did not reflow,
        // and always blank the inactive screen's newly-revealed band.
        if (allowReflowFlag) {
            Cell resizeBlank = defaultBlankCell;
            const bool isPrimaryActive = (t->screens.active == &t->screens.primary);
            // Skip active-screen blanking when the active screen is the reflowed
            // primary, or an anchored shift just placed content in the grown bottom
            // band (which blanked the freed TOP rows itself).
            const bool shouldBlankActive = !(shouldReflow && isPrimaryActive) && !shouldAnchorPrimary;
            const bool shouldBlankInactive = !(shouldReflow && !isPrimaryActive);
            if (newColumns > oldColumns) {
                int rEnd = (oldRows < newRows) ? oldRows : newRows;
                for (int r = 0; r < rEnd; r++) {
                    if (shouldBlankActive)
                        t->grid->eraseRowRange(Ubiquitty::Row(r), Ubiquitty::Column(oldColumns), Ubiquitty::Column(newColumns - 1), resizeBlank);
                    if (shouldBlankInactive)
                        t->grid->eraseRowRangeInactive(Ubiquitty::Row(r), Ubiquitty::Column(oldColumns), Ubiquitty::Column(newColumns - 1), resizeBlank);
                }
            }
            if (newRows > oldRows) {
                for (int r = oldRows; r < newRows; r++) {
                    if (shouldBlankActive)
                        t->grid->eraseRowRange(Ubiquitty::Row(r), Ubiquitty::Column(0), Ubiquitty::Column(newColumns - 1), resizeBlank);
                    if (shouldBlankInactive)
                        t->grid->eraseRowRangeInactive(Ubiquitty::Row(r), Ubiquitty::Column(0), Ubiquitty::Column(newColumns - 1), resizeBlank);
                }
            }
        }
    }
    // Preserve HTS tab stops in surviving columns; only newly-revealed columns
    // get the i%8==0 default (a blanket resetTabStops would erase explicit stops).
    if (newColumns > oldColumns) {
        int c;
        for (c = oldColumns; c < newColumns; c++)
            t->tabStops[c] = (c != 0 && (c % 8) == 0);
    }
    for (int row = newRows; row < oldRows; ++row)
        t->lineTabStops.reset(static_cast<std::size_t>(row));
    // The blocks rode their rows; one whose cells the rewrap separated, or
    // that the new geometry cuts, is gone.
    RichText::reconcileBlocks(*t);
    // A pin on a row the new geometry no longer has names nothing; a grow
    // that brings the row index back brings a blank row, not the line.
    Screen::notePositionsResized(*t);
    if (pendingPrompt) {
        const auto place = Screen::resolvePosition(*t, *pendingPrompt, Screen::RevisionCheck::IgnoreErases);
        t->pendingPromptRow = place ? place->absoluteRow : -1;
    }
    Ubiquitty::Screen::markAllDirty(*t);
    return { .geometryChanged = true };
}

void settleResize(Ubiquitty::Terminal* t)
{
    if (!t || t->pendingHistoryRewrapColumns == 0)
        return;
    // Re-enter resize at the CURRENT dimensions with Settled urgency: the pending
    // marker routes it through the full history rewrap, reusing the whole
    // orchestration rather than duplicating it here.
    resize(*t, t->rows, t->columns, Modes::ReflowMode::Reflow, t->pendingHistoryRewrapAnchor, Modes::ResizeUrgency::Settled);
}

} // namespace Ubiquitty
