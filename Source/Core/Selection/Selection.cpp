// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Selection/Selection.h"

#include "Core/CellContent.h"

#include <mutex>

// Internal alias: refer to Ubiquitty::SelectionMode as `Mode` inside this TU.
using Mode = Ubiquitty::SelectionMode;
// Mirror of Attribute::WideContinuation from CellContent.h; if the bit moves there,
// update this too (a static_assert catches mismatches where CellContent.h shares the TU).
constexpr unsigned selectionAttributeWideContinuation = 0x0200u;

static void clampCell(int rows, int columns, int* row, int* column)
{
    if (rows <= 0 || columns <= 0) {
        *row = 0;
        *column = 0;
        return;
    }
    if (*row < 0)
        *row = 0;
    if (*row >= rows)
        *row = rows - 1;
    if (*column < 0)
        *column = 0;
    if (*column >= columns)
        *column = columns - 1;
}

static int cellIndex(int columns, int row, int column) { return row * columns + column; }

// Walk left while the cell carries Attribute::WideContinuation; the leading half
// lacks the bit, so the loop lands on the owning cell. Bounded by 0.
static int snapToLeadingCell(int row, int column, Ubiquitty::SelectionAttributesAt attributesAt, void* context)
{
    if (!attributesAt)
        return column;
    while (column > 0 && (attributesAt(context, row, column) & selectionAttributeWideContinuation))
        column--;
    return column;
}

// Walk right while the next cell carries Attribute::WideContinuation, the
// mirror of snapToLeadingCell's leftward loop, so both ends of a selection
// span a run of any width.
static int extendThroughWideTail(int row, int column, int columns, Ubiquitty::SelectionAttributesAt attributesAt, void* context)
{
    if (!attributesAt)
        return column;
    while (column + 1 < columns
        && (attributesAt(context, row, column + 1) & selectionAttributeWideContinuation))
        ++column;
    return column;
}

// Process-wide default Engine backing the engine-less forms and setWordCharacters().

namespace Ubiquitty::Selection {

// Mutex guarding writes to g_defaultEngine (config reload). Reads are
// lock-free: the bitmap is 16 bytes and config reloads are rare, so a torn
// read at worst temporarily misclassifies one character.
static std::mutex gWordCharMutex;

// Compile-time default engine: letters, digits, underscore. The constexpr
// factory runs at translation-unit init with zero runtime cost.
static constexpr Engine defaultEngineInit()
{
    Engine e = { { 0 }, false };
    for (int c = 'A'; c <= 'Z'; c++)
        e.wordBitmap[(c >> 3) & 0x0F] |= static_cast<uint8_t>(1u << (c & 7));
    for (int c = 'a'; c <= 'z'; c++)
        e.wordBitmap[(c >> 3) & 0x0F] |= static_cast<uint8_t>(1u << (c & 7));
    for (int c = '0'; c <= '9'; c++)
        e.wordBitmap[(c >> 3) & 0x0F] |= static_cast<uint8_t>(1u << (c & 7));
    e.wordBitmap[('_' >> 3) & 0x0F] |= static_cast<uint8_t>(1u << ('_' & 7));
    e.wordBitmapInited = true;
    return e;
}

// Mutable copy: setWordCharacters() mutates this at runtime.
static Engine g_defaultEngine = defaultEngineInit();

static void engineInitDefault(Engine* e)
{
    *e = defaultEngineInit();
}

void initEngine(Engine* e)
{
    if (!e)
        return;
    engineInitDefault(e);
}

void engineSetWordCharacters(Engine* e, const char* chars)
{
    if (!e)
        return;
    std::lock_guard lock(gWordCharMutex);
    if (!chars || chars[0] == 0) {
        engineInitDefault(e);
        return;
    }
    for (unsigned char& bits : e->wordBitmap)
        bits = 0;
    for (const char* p = chars; *p; p++) {
        uint8_t c = static_cast<uint8_t>(*p);
        if (c > 0x7F)
            continue; // ASCII-only bitmap.
        e->wordBitmap[(c >> 3) & 0x0F] |= static_cast<uint8_t>(1u << (c & 7));
    }
    e->wordBitmapInited = true;
}

bool engineIsWordByte(const Engine* e, uint32_t ch)
{
    if (!e || ch > 0x7F)
        return false;
    // An engine nobody populated carries the default set, which is what the
    // field beside it in the header promises. A caller that holds a
    // Session by value and never calls init has an all-zero bitmap, and the
    // reading that gives is that no byte is a word byte: a double click then
    // selects the single cell under it and the failure names selection rather
    // than initialisation. The default is read rather than written in, so this
    // stays callable on a const engine from any thread.
    const Engine* source = e->wordBitmapInited ? e : &g_defaultEngine;
    return (source->wordBitmap[(ch >> 3) & 0x0F] & (1u << (ch & 7))) != 0;
}

Engine* defaultEngine() { return &g_defaultEngine; }

} // namespace Ubiquitty::Selection

// Engine the active call carries, defaulting to the process-wide one.
// thread_local: ScopedEngine's save/restore is a per-call stack discipline, so
// each thread must see its own binding rather than racing another's.
static thread_local const Ubiquitty::Selection::Engine* currentSelectionEngine = nullptr;

static bool isWordByte(uint32_t ch)
{
    const Ubiquitty::Selection::Engine* e = currentSelectionEngine ? currentSelectionEngine : Ubiquitty::Selection::defaultEngine();
    return Ubiquitty::Selection::engineIsWordByte(e, ch) != 0;
}

static uint32_t safeCellAt(Ubiquitty::SelectionCellAt cellAt, void* context, int row, int column)
{
    if (!cellAt)
        return 0;
    return cellAt(context, row, column);
}

static void wordBounds(int rows, int columns, Ubiquitty::SelectionCellAt cellAt, void* context, int row, int column, int* outLo, int* outHi)
{
    uint32_t ch;
    int lo, hi;
    clampCell(rows, columns, &row, &column);
    lo = hi = column;
    ch = safeCellAt(cellAt, context, row, column);
    if (isWordByte(ch)) {
        while (lo > 0 && isWordByte(safeCellAt(cellAt, context, row, lo - 1)))
            lo--;
        while (hi + 1 < columns && isWordByte(safeCellAt(cellAt, context, row, hi + 1)))
            hi++;
    } else if (ch == ' ') {
        while (lo > 0 && safeCellAt(cellAt, context, row, lo - 1) == ' ')
            lo--;
        while (hi + 1 < columns && safeCellAt(cellAt, context, row, hi + 1) == ' ')
            hi++;
    }
    *outLo = lo;
    *outHi = hi;
}

// Multi-row word walk: extends wordBounds across soft-wrap joins (rowWrapped(r)
// makes rows r and r+1 contiguous). With rowWrapped null the result stays on a
// single row. Space runs are not extended across a join — only word-byte runs —
// because a wrapped line's trailing/leading spaces are usually padding.
static void wordBoundsMultiRow(int rows, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* rowWrappedContext, int clickRow, int clickColumn, int* outLoRow, int* outLoColumn, int* outHiRow, int* outHiColumn)
{
    int loRow, loColumn, hiRow, hiColumn;
    uint32_t ch;
    clampCell(rows, columns, &clickRow, &clickColumn);
    loRow = hiRow = clickRow;
    loColumn = hiColumn = clickColumn;
    ch = safeCellAt(cellAt, cellContext, clickRow, clickColumn);
    if (isWordByte(ch)) {
        for (;;) {
            int prevRow = loRow;
            int prevColumn = loColumn;
            if (prevColumn > 0) {
                prevColumn--;
            } else if (rowWrapped && prevRow > 0 && rowWrapped(rowWrappedContext, prevRow - 1)) {
                prevRow--;
                prevColumn = columns - 1;
            } else {
                break;
            }
            if (!isWordByte(safeCellAt(cellAt, cellContext, prevRow, prevColumn)))
                break;
            loRow = prevRow;
            loColumn = prevColumn;
        }
        for (;;) {
            int nextRow = hiRow;
            int nextColumn = hiColumn;
            if (nextColumn + 1 < columns) {
                nextColumn++;
            } else if (rowWrapped && nextRow + 1 < rows && rowWrapped(rowWrappedContext, nextRow)) {
                nextRow++;
                nextColumn = 0;
            } else {
                break;
            }
            if (!isWordByte(safeCellAt(cellAt, cellContext, nextRow, nextColumn)))
                break;
            hiRow = nextRow;
            hiColumn = nextColumn;
        }
    } else if (ch == ' ') {
        while (loColumn > 0 && safeCellAt(cellAt, cellContext, loRow, loColumn - 1) == ' ')
            loColumn--;
        while (hiColumn + 1 < columns && safeCellAt(cellAt, cellContext, hiRow, hiColumn + 1) == ' ')
            hiColumn++;
    }
    *outLoRow = loRow;
    *outLoColumn = loColumn;
    *outHiRow = hiRow;
    *outHiColumn = hiColumn;
}

namespace Ubiquitty::Selection {

bool isRectangular(const SelectionState* selection) { return selection && selection->mode == Mode::Rectangle; }

void init(SelectionState* selection)
{
    if (!selection)
        return;
    selection->active = false;
    selection->anchorRow = 0;
    selection->anchorColumn = 0;
    selection->headRow = 0;
    selection->headColumn = 0;
    selection->mode = Mode::Character;
    selection->isAnchoredByCommand = false;
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

void begin(SelectionState* selection, int rows, int columns, int row, int column, SelectionMode mode)
{
    if (!selection)
        return;
    clampCell(rows, columns, &row, &column);
    selection->active = true;
    selection->anchorRow = row;
    selection->anchorColumn = column;
    selection->headRow = row;
    selection->headColumn = column;
    selection->mode = mode;
    selection->isAnchoredByCommand = false;
    // A fresh anchor stands on a cell: the text-boundary drag is the one
    // gesture that cuts inside one, and it writes the offsets after this.
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

void update(SelectionState* selection, int rows, int columns, int row, int column)
{
    if (!selection || !selection->active)
        return;
    clampCell(rows, columns, &row, &column);
    selection->headRow = row;
    selection->headColumn = column;
    // A head placed on a cell selects that cell whole; the caret drag writes
    // its offsets back after moving the head through here.
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

void clear(SelectionState* selection)
{
    if (!selection)
        return;
    selection->active = false;
}

bool hasSelection(const SelectionState* selection) { return selection && selection->active; }

bool getBounds(const SelectionState* selection, int rows, int columns, SelectionCellAt cellAt, void* context, SelectionBounds* out)
{
    int ar, ac, hr, hc;
    int a0, a1, h0, h1;
    bool anchorBeforeHead;

    if (!selection || !selection->active || !out || rows <= 0 || columns <= 0)
        return false;

    out->isAnchoredByCommand = selection->isAnchoredByCommand;
    // The sub-column offsets belong to a character selection whose anchor is
    // its start; every other shape, and a head that ran back past its anchor,
    // covers whole cells.
    out->subColumn0 = 0;
    out->subColumn1 = 0;
    ar = selection->anchorRow;
    ac = selection->anchorColumn;
    hr = selection->headRow;
    hc = selection->headColumn;
    clampCell(rows, columns, &ar, &ac);
    clampCell(rows, columns, &hr, &hc);

    if (selection->mode == Mode::Line) {
        if (ar <= hr) {
            out->row0 = ar;
            out->row1 = hr;
        } else {
            out->row0 = hr;
            out->row1 = ar;
        }
        out->column0 = 0;
        out->column1 = columns - 1;
        return true;
    }

    if (selection->mode == Mode::Rectangle) {
        // Column block: top/bottom rows from anchor/head, the same
        // normalized column span on every row in between.
        out->row0 = (ar <= hr) ? ar : hr;
        out->row1 = (ar <= hr) ? hr : ar;
        out->column0 = (ac <= hc) ? ac : hc;
        out->column1 = (ac <= hc) ? hc : ac;
        return true;
    }

    if (selection->mode == Mode::Word) {
        wordBounds(rows, columns, cellAt, context, ar, ac, &a0, &a1);
        wordBounds(rows, columns, cellAt, context, hr, hc, &h0, &h1);
        anchorBeforeHead = (cellIndex(columns, ar, ac) <= cellIndex(columns, hr, hc));
        if (anchorBeforeHead) {
            out->row0 = ar;
            out->column0 = a0;
            out->row1 = hr;
            out->column1 = h1;
        } else {
            out->row0 = hr;
            out->column0 = h0;
            out->row1 = ar;
            out->column1 = a1;
        }
        return true;
    }

    if (cellIndex(columns, ar, ac) <= cellIndex(columns, hr, hc)) {
        out->row0 = ar;
        out->column0 = ac;
        out->row1 = hr;
        out->column1 = hc;
        // The same letters the wide-aware bounds cut, so a caller reading
        // either form of the bounds copies the same text.
        out->subColumn0 = selection->startSubColumn;
        out->subColumn1 = selection->endSubColumn;
    } else {
        out->row0 = hr;
        out->column0 = hc;
        out->row1 = ar;
        out->column1 = ac;
    }
    return true;
}

void expandToWord(SelectionState* selection, int rows, int columns, int row, int column, SelectionCellAt cellAt, void* context, SelectionRowWrappedCallback rowWrapped, void* rowWrappedContext)
{
    int loRow, loColumn, hiRow, hiColumn;
    if (!selection || rows <= 0 || columns <= 0)
        return;
    clampCell(rows, columns, &row, &column);
    wordBoundsMultiRow(rows, columns, cellAt, context, rowWrapped, rowWrappedContext, row, column, &loRow, &loColumn, &hiRow, &hiColumn);
    selection->active = true;
    selection->mode = Mode::Character;
    selection->anchorRow = loRow;
    selection->anchorColumn = loColumn;
    selection->headRow = hiRow;
    selection->headColumn = hiColumn;
    selection->isAnchoredByCommand = false;
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

bool isSmartTokenByte(uint16_t ch)
{
    if (ch > 0x7F)
        return false;
    char c = static_cast<char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
    case ':':
    case '/':
    case '?':
    case '#':
    case '[':
    case ']':
    case '@':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
    case '.':
    case '_':
    case '~':
    case '-':
    case '%':
    case '\\':
        return true;
    default:
        return false;
    }
}

bool expandToSmartToken(SelectionState* selection, int rows, int columns, int row, int column, SelectionCellAt cellAt, void* context)
{
    if (!selection || rows <= 0 || columns <= 0)
        return false;
    clampCell(rows, columns, &row, &column);
    if (!isSmartTokenByte(safeCellAt(cellAt, context, row, column)))
        return false;
    int lo = column, hi = column;
    while (lo > 0 && isSmartTokenByte(safeCellAt(cellAt, context, row, lo - 1)))
        lo--;
    while (hi + 1 < columns && isSmartTokenByte(safeCellAt(cellAt, context, row, hi + 1)))
        hi++;
    selection->active = true;
    selection->mode = Mode::Character;
    selection->anchorRow = row;
    selection->anchorColumn = lo;
    selection->headRow = row;
    selection->headColumn = hi;
    selection->isAnchoredByCommand = false;
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
    return true;
}

void setCharacterSpan(SelectionState* selection, int rows, int columns, int row, int column0, int column1)
{
    if (!selection || rows <= 0 || columns <= 0)
        return;
    if (row < 0)
        row = 0;
    if (row >= rows)
        row = rows - 1;
    if (column0 < 0)
        column0 = 0;
    if (column0 >= columns)
        column0 = columns - 1;
    if (column1 < column0)
        column1 = column0; // degenerate span → single cell
    if (column1 >= columns)
        column1 = columns - 1;
    selection->active = true;
    selection->mode = Mode::Character;
    selection->anchorRow = row;
    selection->anchorColumn = column0;
    selection->headRow = row;
    selection->headColumn = column1;
    selection->isAnchoredByCommand = false;
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

void selectRegion(SelectionState* selection, int rows, int columns, const SelectionBounds& region)
{
    if (!selection || rows <= 0 || columns <= 0)
        return;
    begin(selection, rows, columns, region.row0, region.column0, Mode::Character);
    update(selection, rows, columns, region.row1, region.column1);
    // Not a pointer's press: column 0 of a blank first row is that row's void,
    // where rowSpan's top-edge rule starts a drag on the row after. A command
    // names the row's start, and the blank row stays in the highlight. Line
    // mode would keep the row too, and hand a Shift-click afterwards whole
    // rows where the user clicked a cell.
    selection->isAnchoredByCommand = true;
}

void selectAll(SelectionState* selection, int totalRows, int columns)
{
    if (!selection || totalRows <= 0 || columns <= 0)
        return;
    selectRegion(selection, totalRows, columns, { 0, 0, totalRows - 1, columns - 1 });
}

bool clearForUserInput(SelectionState* selection)
{
    if (!selection || !hasSelection(selection))
        return false;
    clear(selection);
    return true;
}

void doubleClickExpand(SelectionState* selection, int rows, int columns, int row, int column, SelectionCellAt cellAt, void* context, Engine* engine, SmartTokenMode smartTokens)
{
    if (smartTokens == SmartTokenMode::Enabled && expandToSmartToken(selection, rows, columns, row, column, cellAt, context))
        return;
    expandToWordWith(selection, rows, columns, row, column, cellAt, context, engine);
    // WORD mode (not the char span expandToWordWith leaves) so a drag re-snaps
    // both endpoints to whole words.
    if (selection)
        selection->mode = Mode::Word;
}

void expandToLine(SelectionState* selection, int rows, int columns, int row)
{
    if (!selection || rows <= 0 || columns <= 0)
        return;
    if (row < 0)
        row = 0;
    if (row >= rows)
        row = rows - 1;
    selection->active = true;
    // Stay in LINE mode so a drag extends by whole rows and snapBoundsToVoid
    // leaves the full-width rows alone.
    selection->mode = Mode::Line;
    selection->anchorRow = row;
    selection->anchorColumn = 0;
    selection->headRow = row;
    selection->headColumn = columns - 1;
    selection->isAnchoredByCommand = false;
    selection->startSubColumn = 0;
    selection->endSubColumn = 0;
}

void snapToCell(int rows, int columns, SelectionAttributesAt attributesAt, void* context, int* row, int* column)
{
    if (!row || !column)
        return;
    clampCell(rows, columns, row, column);
    *column = snapToLeadingCell(*row, *column, attributesAt, context);
}

bool getBoundsWide(const SelectionState* selection, int rows, int columns, SelectionCellAt cellAt, void* cellContext, SelectionAttributesAt attributesAt, void* attributeContext, SelectionBounds* out, SelectionRowWrappedCallback rowWrapped, void* rowWrappedContext)
{
    int ar, ac, hr, hc;
    int a0Row, a0Column, a1Row, a1Column;
    int h0Row, h0Column, h1Row, h1Column;
    bool anchorBeforeHead;

    if (!selection || !selection->active || !out || rows <= 0 || columns <= 0)
        return false;

    out->isAnchoredByCommand = selection->isAnchoredByCommand;
    // The sub-column offsets belong to a character selection whose anchor is
    // its start; every other shape, and a head that ran back past its anchor,
    // covers whole cells.
    out->subColumn0 = 0;
    out->subColumn1 = 0;
    ar = selection->anchorRow;
    ac = selection->anchorColumn;
    hr = selection->headRow;
    hc = selection->headColumn;
    clampCell(rows, columns, &ar, &ac);
    clampCell(rows, columns, &hr, &hc);

    // Snap anchor and head past any continuation cells.
    ac = snapToLeadingCell(ar, ac, attributesAt, attributeContext);
    hc = snapToLeadingCell(hr, hc, attributesAt, attributeContext);

    if (selection->mode == Mode::Line) {
        if (ar <= hr) {
            out->row0 = ar;
            out->row1 = hr;
        } else {
            out->row0 = hr;
            out->row1 = ar;
        }
        out->column0 = 0;
        out->column1 = columns - 1;
        return true;
    }

    if (selection->mode == Mode::Rectangle) {
        // Column block: same snapped column span on every row; extend the right
        // edge through a wide glyph's continuation half.
        out->row0 = (ar <= hr) ? ar : hr;
        out->row1 = (ar <= hr) ? hr : ar;
        const int lc = (ac <= hc) ? ac : hc;
        const int rc = (ac <= hc) ? hc : ac;
        out->column0 = lc;
        out->column1 = extendThroughWideTail(out->row1, rc, columns, attributesAt, attributeContext);
        return true;
    }

    anchorBeforeHead = (cellIndex(columns, ar, ac) <= cellIndex(columns, hr, hc));

    if (selection->mode == Mode::Word) {
        wordBoundsMultiRow(rows, columns, cellAt, cellContext, rowWrapped, rowWrappedContext, ar, ac, &a0Row, &a0Column, &a1Row, &a1Column);
        wordBoundsMultiRow(rows, columns, cellAt, cellContext, rowWrapped, rowWrappedContext, hr, hc, &h0Row, &h0Column, &h1Row, &h1Column);
        // Extend the trailing edges across a wide glyph's continuation half so
        // the whole glyph is covered (only matters at the word's far-right column).
        a1Column = extendThroughWideTail(a1Row, a1Column, columns, attributesAt, attributeContext);
        h1Column = extendThroughWideTail(h1Row, h1Column, columns, attributesAt, attributeContext);
        if (anchorBeforeHead) {
            out->row0 = a0Row;
            out->column0 = a0Column;
            out->row1 = h1Row;
            out->column1 = h1Column;
        } else {
            out->row0 = h0Row;
            out->column0 = h0Column;
            out->row1 = a1Row;
            out->column1 = a1Column;
        }
        return true;
    }

    // Mode::Character — keep the trailing edge inclusive of the wide glyph the
    // head landed on, so a click on the leading half selects both halves.
    if (anchorBeforeHead) {
        out->row0 = ar;
        out->column0 = ac;
        out->row1 = hr;
        // An end standing inside a packed cell ends on that cell: the run
        // cells after it hold no letter of their own, and extending through
        // them would move the edge off the cell whose letters the cut names.
        out->column1 = selection->endSubColumn > 0
            ? hc
            : extendThroughWideTail(hr, hc, columns, attributesAt, attributeContext);
        out->subColumn0 = selection->startSubColumn;
        out->subColumn1 = selection->endSubColumn;
    } else {
        out->row0 = hr;
        out->column0 = hc;
        out->row1 = ar;
        out->column1 = extendThroughWideTail(ar, ac, columns, attributesAt, attributeContext);
    }
    return true;
}

int lastPrintedColumn(int row, int columns, SelectionCellAt cellAt, void* cellContext, SelectionAttributesAt attributesAt, void* attributeContext)
{
    for (int c = columns - 1; c >= 0; --c) {
        if (attributesAt && (attributesAt(attributeContext, row, c) & selectionAttributeWideContinuation))
            return c; // continuation half of a wide glyph
                      // ch <= 0x20: blank (0), space (0x20) or C0 control — all void.
        if (safeCellAt(cellAt, cellContext, row, c) != 0)
            return c;
    }
    return -1;
}

RowSpan rowSpan(const SelectionBounds* b, SelectionMode mode, int row, int columns, SelectionCellAt cellAt, void* cellContext, SelectionAttributesAt attributesAt, void* attributeContext)
{
    RowSpan rs = { 0, 0, false };
    if (!b || columns <= 0 || row < b->row0 || row > b->row1)
        return rs;

    int c0 = (row == b->row0) ? b->column0 : 0;
    int c1 = (row == b->row1) ? b->column1 : (columns - 1);

    const Mode m = mode;
    if (m == Mode::Rectangle) {
        // Column block: the same [column0,column1] span on every row. The c0/c1
        // above carry linear first/interior/last widening, which would render a
        // multi-row rectangle full width — so use the bounds directly.
        rs.column0 = b->column0;
        rs.column1 = b->column1;
        rs.selected = true;
        return rs;
    }
    if (m == Mode::Line) {
        // Whole rows already (getBounds set column0=0, column1=columns-1).
        rs.column0 = c0;
        rs.column1 = c1;
        rs.selected = true;
        return rs;
    }
    if (row != b->row0 && row != b->row1) {
        // Interior row: full width in O(1), no callback (off-viewport safe).
        rs.column0 = c0;
        rs.column1 = c1;
        rs.selected = true;
        return rs;
    }

    // Bottom edge in the void. An empty line is a bare \n → whole line.
    // In WORD mode the next cell past the text is already "off the word", so
    // it snaps to the whole line immediately. In CHAR mode the void clamps to
    // the last glyph until voidSnapCells past it, then snaps to the line.
    if (row == b->row1) {
        const int lp = lastPrintedColumn(row, columns, cellAt, cellContext, attributesAt, attributeContext);
        if (c1 > lp) {
            if (lp < 0 || m == Mode::Word || c1 - lp >= voidSnapCells)
                c1 = columns - 1;
            else
                c1 = lp;
        }
    }
    // Top edge in the void (the line's trailing newline), so a top endpoint there
    // starts the selection after the content:
    //   * Empty top line → drops out (column0 == columns); run begins next row.
    //     Unless the bounds are isAnchoredByCommand: a command named the row's
    //     start, no pointer sat in its void, and the row stays (column0 == 0).
    //   * Content line, endpoint within voidSnapCells → caught, column0 collapses
    //     to the first void column.
    //   * Content line, endpoint beyond → not caught, line drops out.
    // Catchment is char-only: WORD mode always catches the content line.
    if (row == b->row0) {
        const int lp = lastPrintedColumn(row, columns, cellAt, cellContext, attributesAt, attributeContext);
        if (c0 > lp) {
            if (lp < 0) {
                // `&`, not `&&`: a short-circuit adds a branch, and this
                // function keeps the block structure its PGO profile describes.
                const bool shouldDropRow = (b->row0 != b->row1) & !b->isAnchoredByCommand;
                c0 = shouldDropRow ? columns : (lp + 1);
            } else if (m != Mode::Word && c0 - lp >= voidSnapCells)
                c0 = columns;
            else
                c0 = lp + 1;
        }
    }

    rs.column0 = c0;
    rs.column1 = c1;
    rs.selected = (c1 >= c0);
    return rs;
}

void snapBoundsToVoid(SelectionBounds* b, SelectionMode mode, int columns, SelectionCellAt cellAt, void* cellContext, SelectionAttributesAt attributesAt, void* attributeContext)
{
    if (!b || columns <= 0)
        return;
    // rowSpan is the single source of the per-row void rule (it early-outs
    // rect / line). The top edge owns column0, the bottom edge owns column1.
    const RowSpan top = rowSpan(b, mode, b->row0, columns, cellAt, cellContext, attributesAt, attributeContext);
    const RowSpan btm = rowSpan(b, mode, b->row1, columns, cellAt, cellContext, attributesAt, attributeContext);
    b->column0 = top.column0;
    b->column1 = btm.column1;
}

int caretSlot(int caret, int lastPrinted) { return (lastPrinted < 0) ? 0 : caret; }

void charDragLinear(SelectionState* selection, int rows, int columns, int anchorRow, int anchorCaret, int headRow, int headCaret)
{
    if (!selection || columns <= 0)
        return;
    const long pA = static_cast<long>(anchorRow) * columns + anchorCaret;
    const long pHeight = static_cast<long>(headRow) * columns + headCaret;
    const long lo = (pA < pHeight) ? pA : pHeight;
    const long hi = (pA < pHeight) ? pHeight : pA;
    if (lo == hi) {
        clear(selection);
        return;
    }
    const long e = hi - 1; // half-open [lo,hi) → inclusive cells [lo,hi-1]
    begin(selection, rows, columns, static_cast<int>(lo / columns), static_cast<int>(lo % columns), Mode::Character);
    update(selection, rows, columns, static_cast<int>(e / columns), static_cast<int>(e % columns));
}

void charDragLinear(SelectionState* selection, int rows, int columns, int anchorRow, int anchorCaret, int anchorSubColumn,
    int headRow, int headCaret, int headSubColumn)
{
    if (!selection || columns <= 0)
        return;
    struct Caret {
        long cell;
        int subColumn;
    };
    const Caret anchor { static_cast<long>(anchorRow) * columns + anchorCaret, anchorSubColumn > 0 ? anchorSubColumn : 0 };
    const Caret head { static_cast<long>(headRow) * columns + headCaret, headSubColumn > 0 ? headSubColumn : 0 };
    const bool isAnchorFirst = anchor.cell < head.cell || (anchor.cell == head.cell && anchor.subColumn <= head.subColumn);
    const Caret lo = isAnchorFirst ? anchor : head;
    const Caret hi = isAnchorFirst ? head : anchor;
    if (lo.cell == hi.cell && lo.subColumn == hi.subColumn) {
        clear(selection);
        return;
    }
    // An end caret standing inside a cell keeps that cell's letters before it;
    // one on a boundary keeps none of the cell to its right.
    const long last = hi.subColumn > 0 ? hi.cell : hi.cell - 1;
    begin(selection, rows, columns, static_cast<int>(lo.cell / columns), static_cast<int>(lo.cell % columns), Mode::Character);
    update(selection, rows, columns, static_cast<int>(last / columns), static_cast<int>(last % columns));
    selection->startSubColumn = lo.subColumn;
    selection->endSubColumn = hi.subColumn;
}

void setWordCharacters(const char* chars) { engineSetWordCharacters(defaultEngine(), chars); }

// Scope guard for the per-call engine pointer; restores the previous binding so
// nested / re-entrant Selection calls keep working.
namespace {
struct ScopedEngine {
    const Engine* prev;
    ScopedEngine(const Engine* e)
        : prev(currentSelectionEngine)
    {
        currentSelectionEngine = e ? e : defaultEngine();
    }
    ~ScopedEngine() { currentSelectionEngine = prev; }
};
} // unnamed namespace

void expandToWordWith(SelectionState* selection, int rows, int columns, int row, int column, SelectionCellAt cellAt, void* context, Engine* engine, SelectionRowWrappedCallback rowWrapped, void* rowWrappedContext)
{
    ScopedEngine bind(engine);
    expandToWord(selection, rows, columns, row, column, cellAt, context, rowWrapped, rowWrappedContext);
}

bool getBoundsWideWith(
    const SelectionState* selection, int rows, int columns, SelectionCellAt cellAt, void* cellContext, SelectionAttributesAt attributesAt, void* attributeContext, SelectionBounds* out, Engine* engine, SelectionRowWrappedCallback rowWrapped, void* rowWrappedContext)
{
    ScopedEngine bind(engine);
    return getBoundsWide(selection, rows, columns, cellAt, cellContext, attributesAt, attributeContext, out, rowWrapped, rowWrappedContext);
}

// --- Mouse-gesture policy ------------------------------------------

void gestureReset(Gesture* g)
{
    if (!g)
        return;
    g->anchorRow = 0;
    g->anchorColumn = 0;
    g->targetRow = 0;
    g->targetColumn = 0;
    g->clickCount = 1;
    g->dragBegan = false;
    g->isOpen = false;
}

bool gestureBegin(Gesture* g, const GestureOperations* operations, int row, int column, int clickCount, GestureMode gesture)
{
    if (!g || !operations)
        return false;
    g->anchorRow = row;
    g->anchorColumn = column;
    g->targetRow = row;
    g->targetColumn = column;
    g->dragBegan = false;
    g->isOpen = true;

    if (clickCount > 3)
        clickCount = 1;
    g->clickCount = clickCount;

    if (gesture == GestureMode::Extend && operations->hasSelection && operations->hasSelection(operations->ud)) {
        // Extend the existing selection to the clicked cell.
        if (operations->update)
            operations->update(operations->ud, row, column);
        g->dragBegan = true;
        return true;
    }
    if (clickCount >= 3) {
        if (operations->beginLine)
            operations->beginLine(operations->ud, row, column);
        g->dragBegan = true;
        return true;
    }
    if (clickCount == 2) {
        if (operations->beginWord)
            operations->beginWord(operations->ud, row, column);
        g->dragBegan = true;
        return true;
    }
    // Single click: clear now, leave no highlight; the selection only
    // opens once the pointer drags (gestureDrag).
    bool had = (operations->hasSelection && operations->hasSelection(operations->ud));
    if (operations->clear)
        operations->clear(operations->ud);
    return had;
}

bool gestureDrag(Gesture* g, const GestureOperations* operations, int row, int column)
{
    if (!g || !operations || !g->isOpen) {
        return false;
    }
    g->targetRow = row;
    g->targetColumn = column;
    if (!g->dragBegan) {
        if (operations->beginCharacter)
            operations->beginCharacter(operations->ud, g->anchorRow, g->anchorColumn);
        g->dragBegan = true;
    }
    if (operations->update)
        operations->update(operations->ud, row, column);
    return true;
}

bool gestureEnd(Gesture* g, const GestureOperations* operations)
{
    if (!g || !operations)
        return false;
    g->isOpen = false;
    return (operations->hasSelection && operations->hasSelection(operations->ud));
}

bool lastCommandOutputRegion(SelectionRowMarkAt markAt, void* context, int totalRows, int columns, int maxAbsoluteRow, SelectionBounds* out)
{
    if (!markAt || !out || totalRows <= 0 || columns <= 0)
        return false;

    int hi = maxAbsoluteRow;
    if (hi >= totalRows)
        hi = totalRows - 1;
    if (hi < 0)
        return false;

    // Walk up from the ceiling to the most recent OUTPUT_START (; C).
    int outputStart = -1;
    for (int r = hi; r >= 0; --r) {
        if (markAt(context, r) & selectionMarkOutputStart) {
            outputStart = r;
            break;
        }
    }
    if (outputStart < 0)
        return false;

    // Output ends just above the next CMD_END (; D) at or below the start, or at
    // the last buffer row if still running. A ; D on the ; C row yields one row.
    int endRow = totalRows - 1;
    for (int r = outputStart; r < totalRows; ++r) {
        if (markAt(context, r) & selectionMarkCmdEnd) {
            endRow = (r > outputStart) ? (r - 1) : outputStart;
            break;
        }
    }

    out->row0 = outputStart;
    out->column0 = 0;
    out->row1 = endRow;
    out->column1 = columns - 1;
    return true;
}

Ubiquitty::Accessibility::ProjectionRange selectionInProjection(const Ubiquitty::Accessibility::TextProjection& projection, const SelectionState& selection, std::ptrdiff_t historyRows, std::ptrdiff_t viewOffset)
{
    if (!selection.active)
        return {};
    int startRow = selection.anchorRow;
    int startColumn = selection.anchorColumn;
    int endRow = selection.headRow;
    int endColumn = selection.headColumn;
    int startSubColumn = selection.startSubColumn;
    int endSubColumn = selection.endSubColumn;
    if (endRow < startRow || (endRow == startRow && endColumn < startColumn)) {
        std::swap(startRow, endRow);
        std::swap(startColumn, endColumn);
        startSubColumn = 0;
        endSubColumn = 0;
    }
    const std::ptrdiff_t visibleStart = startRow - historyRows + viewOffset;
    const std::ptrdiff_t visibleEnd = endRow - historyRows + viewOffset;
    const std::ptrdiff_t lineCount = static_cast<std::ptrdiff_t>(projection.lineRanges.size());
    if (visibleEnd < 0 || visibleStart >= lineCount)
        return {};
    const std::ptrdiff_t clampedStart = std::max<std::ptrdiff_t>(0, visibleStart);
    const std::ptrdiff_t clampedEnd = std::min<std::ptrdiff_t>(lineCount - 1, visibleEnd);
    if (clampedStart > clampedEnd)
        return {};
    const auto from = Ubiquitty::Accessibility::positionInProjection(projection, clampedStart,
        visibleStart == clampedStart ? startColumn : 0, visibleStart == clampedStart ? startSubColumn : 0);
    const int endBoundary = endSubColumn > 0 ? endColumn : std::min(endColumn, projection.columns - 1) + 1;
    const auto to = Ubiquitty::Accessibility::positionInProjection(projection, clampedEnd,
        visibleEnd == clampedEnd ? endBoundary : projection.columns, visibleEnd == clampedEnd ? endSubColumn : 0);
    if (!from.isValid || !to.isValid || to.location < from.location)
        return {};
    return { from.location, to.location - from.location, true };
}

} // namespace Ubiquitty::Selection
