// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Selection/Selection.h"
#include <cstdio>
#include <type_traits>

TEST(defaultConstructedSelectionStateIsIdle)
{
    constexpr Ubiquitty::SelectionState selection;
    static_assert(std::is_trivially_copyable_v<Ubiquitty::SelectionState>);
    static_assert(!selection.active);
    CHECK(!Ubiquitty::Selection::hasSelection(&selection));
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 0);
    CHECK(selection.headColumn == 0);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(!selection.isAnchoredByCommand);
}

TEST(defaultConstructedWordEngineUsesTheDefaultClassifier)
{
    constexpr Ubiquitty::Selection::Engine engine;
    static_assert(std::is_trivially_copyable_v<Ubiquitty::Selection::Engine>);
    static_assert(!engine.wordBitmapInited);
    for (const auto bits : engine.wordBitmap) {
        CHECK(bits == 0);
    }
    for (uint32_t codePoint = 0; codePoint <= 128; ++codePoint) {
        CHECK(Ubiquitty::Selection::engineIsWordByte(&engine, codePoint)
            == Ubiquitty::Selection::engineIsWordByte(Ubiquitty::Selection::defaultEngine(), codePoint));
    }
    CHECK(!engine.wordBitmapInited);
}

using Grid = struct {
    int rows;
    int columns;
    const char* text;
};

static uint32_t gridCellAt(void* context, int row, int column)
{
    Grid* g = (Grid*)context;
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return ' ';
    // '~' marks an unwritten/void cell — the model's blank sentinel
    // (ch == 0), distinct from a typed space (' ', 0x20) which is content.
    char c = g->text[row * g->columns + column];
    return (c == '~') ? 0 : (uint16_t)(uint8_t)c;
}

static void checkBounds(Ubiquitty::SelectionBounds b, int row0, int column0, int row1, int column1)
{
    CHECK(b.row0 == row0);
    CHECK(b.column0 == column0);
    CHECK(b.row1 == row1);
    CHECK(b.column1 == column1);
}

// Synthetic row carrying a leading wide glyph plus continuation marker.
// Layout for the wide-cell tests below:
// column 0: 'A'           (narrow)
// column 1: wide-lead 'W' (sets Attribute::Wide)
// column 2: wide-cont     (Attribute::WideContinuation, char=0)
// column 3: 'B'           (narrow)
// column 4: ' '
// column 5: wide-lead 'X'
// column 6: wide-cont
// column 7: 'C'
// That gives us a simple but realistic mix to exercise snap + word
// bounds against.
enum {
    WC_LEAD = 0x0100u, // matches Attribute::Wide
    WC_CONT = 0x0200u // matches Attribute::WideContinuation
};

using WideGrid = struct {
    int rows;
    int columns;
    const uint16_t* chars;
    const uint16_t* attributes;
};

static uint32_t wideCellAt(void* context, int row, int column)
{
    WideGrid* g = (WideGrid*)context;
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return ' ';
    return g->chars[row * g->columns + column];
}

static uint16_t wideAttrAt(void* context, int row, int column)
{
    WideGrid* g = (WideGrid*)context;
    if (row < 0 || row >= g->rows || column < 0 || column >= g->columns)
        return 0;
    return g->attributes[row * g->columns + column];
}

// Soft-wrap join callback that reports the row in `wrappedRow` as
// joined to the next via soft wrap. -1 means no join. Used by the
// cross-row word tests below.
using WrapContext = struct {
    int wrappedRow;
};

static int wrapCtxAt(void* context, int row)
{
    WrapContext* w = (WrapContext*)context;
    return (w && w->wrappedRow == row) ? 1 : 0;
}

// Two engines, each with its own word-character set, queried via
// expandToWordWith on the same grid. The selections must reflect the
// engine bound to the call, not the process-default.
// setWordCharacters() applied to the default engine, then read back via
// the engine-free expandToWord(): the override takes effect.
// Fake GestureOperations recording which mutation the Core sequencer drives,
// so the click/drag policy can be tested without a real Terminal.
using FakeGesture = struct {
    char lastOp; // 'C' char, 'W' word, 'L' line, 'U' update, 'X' clear, 0 none
    int opCount;
    int lastRow, lastColumn;
    int hasSelection; // simulated: any begin/word/line/update sets it; clear unsets
};

static void fg_beginCharacter(void* ud, int r, int c)
{
    FakeGesture* f = (FakeGesture*)ud;
    f->lastOp = 'C';
    f->opCount++;
    f->lastRow = r;
    f->lastColumn = c;
    f->hasSelection = 1;
}
static void fg_beginWord(void* ud, int r, int c)
{
    FakeGesture* f = (FakeGesture*)ud;
    f->lastOp = 'W';
    f->opCount++;
    f->lastRow = r;
    f->lastColumn = c;
    f->hasSelection = 1;
}
static void fg_beginLine(void* ud, int r, int c)
{
    FakeGesture* f = (FakeGesture*)ud;
    (void)c;
    f->lastOp = 'L';
    f->opCount++;
    f->lastRow = r;
    f->hasSelection = 1;
}
static void foregroundUpdate(void* ud, int r, int c)
{
    FakeGesture* f = (FakeGesture*)ud;
    f->lastOp = 'U';
    f->opCount++;
    f->lastRow = r;
    f->lastColumn = c;
    f->hasSelection = 1;
}
static void foregroundClear(void* ud)
{
    FakeGesture* f = (FakeGesture*)ud;
    f->lastOp = 'X';
    f->opCount++;
    f->hasSelection = 0;
}
static bool fg_hasSelection(void* ud) { return static_cast<FakeGesture*>(ud)->hasSelection != 0; }

static Ubiquitty::Selection::GestureOperations fakeOps(FakeGesture* f)
{
    Ubiquitty::Selection::GestureOperations operations;
    operations.beginCharacter = fg_beginCharacter;
    operations.beginWord = fg_beginWord;
    operations.beginLine = fg_beginLine;
    operations.update = foregroundUpdate;
    operations.clear = foregroundClear;
    operations.hasSelection = fg_hasSelection;
    operations.ud = f;
    return operations;
}

// The selection model is coordinate-space agnostic: a frontend that
// addresses absolute buffer rows (scrollback + screen) passes the total
// row count and absolute rows. These tests pin that a selection spanning
// far more rows than a single screen materialises without gridFloor — the
// invariant that lets a drag run into scrollback.

// A char selection whose anchor sits deep in history and head sits on
// the live screen keeps both endpoints; nothing collapses to the visible
// grid. totalRows here models 1000 scrollback lines + 24 screen rows.
// A drag that starts on the screen (high absolute row) and moves up into
// history (low absolute row) flips the bounds so row0 < row1.
// Cmd+A select-all-buffer: anchor (0,0) through head (totalRows-1,
// columns-1) covers every buffer row edge-to-edge.
// Line-mode drag across the scrollback/screen boundary spans full-width
// rows from the topmost to the bottommost absolute row touched.
// Rectangular (column-block) mode: bounds report the top/bottom rows and a
// single normalized [column0,column1] span that applies to every row — no
// line-wrap fill, and the column span is independent of which endpoint sits
// on the top row.
// A rect block whose anchor is to the RIGHT of the head on a lower row still
// reports column0 = min(columns), column1 = max(columns): the span does not collapse to
// the first/last row's columns the way linear char mode would.
// --- Apple-Ubiquitty::Terminal void clamp ------------------------------------

// --- Full-pipeline gesture simulations -----------------------------
// Drive the same Core sequence the frontends' gesture shims use (begin/
// expand + update head, then getBoundsWide + rowSpan) so a behaviour
// regression in the wiring — not just the leaf rule — is caught.

// Resolve the drawn span on `row` exactly as a frontend would: wide-aware
// bounds, then the per-row void rule.
static Ubiquitty::Selection::RowSpan simRow(const Ubiquitty::SelectionState* selection, int rows, int columns, int row, Ubiquitty::SelectionCellAt cellAt, void* context)
{
    Ubiquitty::SelectionBounds b;
    if (!Ubiquitty::Selection::getBoundsWide(selection, rows, columns, cellAt, context, nullptr, nullptr, &b))
        return Ubiquitty::Selection::RowSpan { 0, 0, false };
    return Ubiquitty::Selection::rowSpan(&b, selection->mode, row, columns, cellAt, context, nullptr, nullptr);
}

// ===== Full mouse-gesture simulation harness ========================
// Two harnesses mirror the two frontend input paths over a Grid so a
// press→move sequence drives the same Core pipeline the UI does.

// (1) Gesture path (double/triple-click + shift): GestureOperations bound to a
// Grid, as a frontend's gesture shims bind it, driven by gestureBegin/
// gestureDrag through one persistent Gesture.
using SimGesture = struct {
    Ubiquitty::SelectionState* selection;
    Grid* g;
};

static void sg_beginCharacter(void* ud, int r, int c)
{
    SimGesture* s = (SimGesture*)ud;
    Ubiquitty::Selection::begin(s->selection, s->g->rows, s->g->columns, r, c, Ubiquitty::SelectionMode::Character);
}
static void sg_beginWord(void* ud, int r, int c)
{
    SimGesture* s = (SimGesture*)ud;
    Ubiquitty::Selection::expandToWord(s->selection, s->g->rows, s->g->columns, r, c, gridCellAt, s->g);
    s->selection->mode = Ubiquitty::SelectionMode::Word; // mirror gopBeginWord
}
static void sg_beginLine(void* ud, int r, int /*c*/)
{
    SimGesture* s = (SimGesture*)ud;
    Ubiquitty::Selection::expandToLine(s->selection, s->g->rows, s->g->columns, r);
}
static void sgUpdate(void* ud, int r, int c)
{
    SimGesture* s = (SimGesture*)ud;
    Ubiquitty::Selection::update(s->selection, s->g->rows, s->g->columns, r, c);
}
static void sgClear(void* ud) { Ubiquitty::Selection::clear(static_cast<SimGesture*>(ud)->selection); }
static bool sgHas(void* ud) { return Ubiquitty::Selection::hasSelection(static_cast<SimGesture*>(ud)->selection); }

static Ubiquitty::Selection::GestureOperations sgOps(SimGesture* s)
{
    Ubiquitty::Selection::GestureOperations operations;
    operations.beginCharacter = sg_beginCharacter;
    operations.beginWord = sg_beginWord;
    operations.beginLine = sg_beginLine;
    operations.update = sgUpdate;
    operations.clear = sgClear;
    operations.hasSelection = sgHas;
    operations.ud = s;
    return operations;
}

// (2) Caret/char path (single click + drag): round a fractional column (in
// cells) to the nearest cell boundary, as selectionCaretAtWindowPoint does — a
// half-cell of travel past a midpoint moves the caret to the next boundary.
static int simCaret(double xCells, int columns)
{
    int caret = (int)(xCells + 0.5);
    if (caret < 0)
        caret = 0;
    if (caret > columns)
        caret = columns;
    return caret;
}
// Plain char drag from anchor pixel-x to head pixel-x (charDragLinear, the
// caret model that owns single-click drags).
static void simCharacter(Ubiquitty::SelectionState* selection, Grid* g, int aRow, double aX, int hRow, double hX) { Ubiquitty::Selection::charDragLinear(selection, g->rows, g->columns, aRow, simCaret(aX, g->columns), hRow, simCaret(hX, g->columns)); }

// ---- char/caret model ----------------------------------------------

// ---- word model (gesture) ------------------------------------------

// ---- line model (gesture) ------------------------------------------

// ---- shift-extend + rect -------------------------------------------

// Multi-row RECT: every row — including the interior ones — carries the same
// [column0,column1] column block. An interior row widened to full width (the linear
// first/interior/last rule leaking into rect) renders a column-block selection as a
// linear one.
// ---- prompt trailing space (typed 0x20 vs void sentinel) -----------

// Counting cell/attribute stubs: prove rowSpan never touches the callbacks on an
// interior row (the perf + off-viewport invariant).
using CountContext = struct {
    Grid grid;
    int cellCalls;
    int attrCalls;
};

static uint32_t countingCellAt(void* context, int row, int column)
{
    CountContext* c = (CountContext*)context;
    c->cellCalls++;
    return gridCellAt(&c->grid, row, column);
}

static uint16_t countingAttrAt(void* context, int row, int column)
{
    CountContext* c = (CountContext*)context;
    c->attrCalls++;
    (void)row;
    (void)column;
    return 0;
}

// --- Smart-token (URL/path/hash) expansion -------------------------

// --- doubleClickExpand: word default + drag preservation, smart opt-in ---

// REGRESSION: a double-click must leave the selection in WORD mode so a
// subsequent drag re-snaps to whole words (getBounds Word). A smart-token
// default that leaves Char here collapses drag-after-double-click to a char
// span — this test fails if that recurs.
// Smart enabled: a double-click in a URL selects the whole token (char mode).
// Smart disabled (default): the same URL click selects a WORD, not the token.
// --- Semantic last-command-output region (OSC 133) -----------------

using MarkGrid = struct {
    int total;
    const uint8_t* marks; // one byte per absolute row
};

static uint8_t markAt(void* context, int absoluteRow)
{
    MarkGrid* m = (MarkGrid*)context;
    if (absoluteRow < 0 || absoluteRow >= m->total)
        return 0;
    return m->marks[absoluteRow];
}

TEST(testLinearCharBounds)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 4, 8, 2, 6, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, 4, 8, 1, 2);

    CHECK(Ubiquitty::Selection::getBounds(&selection, 4, 8, gridCellAt, nullptr, &b));
    checkBounds(b, 1, 2, 2, 6);
}

TEST(testLastPrintedCol)
{
    static const char text[] = "ab~~~~~~" // row0: "ab" + trailing void
                               "xyz12345" // row1: full row
                               "~~~~~~~~"; // row2: all void
    Grid g = { 3, 8, text };
    CHECK(Ubiquitty::Selection::lastPrintedColumn(0, 8, gridCellAt, &g, nullptr, nullptr) == 1);
    CHECK(Ubiquitty::Selection::lastPrintedColumn(1, 8, gridCellAt, &g, nullptr, nullptr) == 7);
    CHECK(Ubiquitty::Selection::lastPrintedColumn(2, 8, gridCellAt, &g, nullptr, nullptr) == -1);
}

TEST(testLastPrintedColWideTail)
{
    // A wide glyph ('W' lead + continuation) is the last thing on the row.
    static const uint16_t chars[] = { 'A', ' ', ' ', ' ', ' ', ' ', 'W', 0 };
    static const uint16_t attributes[] = { 0, 0, 0, 0, 0, 0, WC_LEAD, WC_CONT };
    WideGrid g = { 1, 8, chars, attributes };
    // The continuation half counts as printed → last printed column is 7.
    CHECK(Ubiquitty::Selection::lastPrintedColumn(0, 8, wideCellAt, &g, wideAttrAt, &g) == 7);
    // Without attributes the cont cell (ch==0) reads as void → falls to the lead.
    CHECK(Ubiquitty::Selection::lastPrintedColumn(0, 8, wideCellAt, &g, nullptr, nullptr) == 6);
}

TEST(testSnapBoundsVoid)
{
    static const char text[] = "hello~~~"; // 1 row, 8 columns: "hello" + 3 void
    Grid g = { 1, 8, text };

    // Head 2 cells into the void (column6, lastPrinted4) — within voidSnapCells,
    // so it clamps to the last glyph rather than taking the line.
    Ubiquitty::SelectionBounds b = { 0, 0, 0, 6 };
    Ubiquitty::Selection::snapBoundsToVoid(&b, Ubiquitty::SelectionMode::Character, 8, gridCellAt, &g, nullptr, nullptr);
    checkBounds(b, 0, 0, 0, 4);

    // Rect mode keeps the void literally — the column block is unchanged.
    Ubiquitty::SelectionBounds r = { 0, 2, 0, 6 };
    Ubiquitty::Selection::snapBoundsToVoid(&r, Ubiquitty::SelectionMode::Rectangle, 8, gridCellAt, &g, nullptr, nullptr);
    checkBounds(r, 0, 2, 0, 6);
}

TEST(testSnapBoundsVoidPlus5)
{
    static const char text[] = "abc~~~~~~~~~"; // 1 row, 12 columns: "abc" + 9 void
    Grid g = { 1, 12, text };

    // Head 3 cells past the last glyph (column5, lastPrinted2) — within
    // voidSnapCells → clamp to the last glyph.
    Ubiquitty::SelectionBounds b = { 0, 0, 0, 5 };
    Ubiquitty::Selection::snapBoundsToVoid(&b, Ubiquitty::SelectionMode::Character, 12, gridCellAt, &g, nullptr, nullptr);
    checkBounds(b, 0, 0, 0, 2);

    // Head exactly voidSnapCells past (column8 = lastPrinted2 + 6) → snap to the
    // whole line.
    Ubiquitty::SelectionBounds c = { 0, 0, 0, 8 };
    Ubiquitty::Selection::snapBoundsToVoid(&c, Ubiquitty::SelectionMode::Character, 12, gridCellAt, &g, nullptr, nullptr);
    checkBounds(c, 0, 0, 0, 11);
}

TEST(testSnapBoundsTopVoid)
{
    // EMPTY top line is a pure \n: the anchor sits past it, so the line drops
    // out (column0 == columns) and the selection lands on the LOWER line.
    static const char text[] = "~~~~~~~~"
                               "~~~~~~~~";
    Grid g = { 2, 8, text };
    Ubiquitty::SelectionBounds b = { 0, 3, 1, 5 };
    Ubiquitty::Selection::snapBoundsToVoid(&b, Ubiquitty::SelectionMode::Character, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(b.row0 == 0 && b.column0 == 8); // columns ⇒ empty top line excluded
    CHECK(b.row1 == 1 && b.column1 == 7);

    // Top line with CONTENT, anchor on a glyph: kept from the anchor.
    static const char text2[] = "abc~~~~~"
                                "~~~~~~~~";
    Grid g2 = { 2, 8, text2 };
    Ubiquitty::SelectionBounds c = { 0, 1, 1, 4 };
    Ubiquitty::Selection::snapBoundsToVoid(&c, Ubiquitty::SelectionMode::Character, 8, gridCellAt, &g2, nullptr, nullptr);
    checkBounds(c, 0, 1, 1, 7);

    // Top line with CONTENT, the TOP endpoint in its trailing void: the void
    // is the line's newline at its end, so the selection starts after the
    // content — the top line contributes only its trailing void (column0 snaps
    // to lastPrinted+1), not its content.
    Ubiquitty::SelectionBounds d = { 0, 6, 1, 4 }; // column6 is past "abc" (lastPrinted 2)
    Ubiquitty::Selection::snapBoundsToVoid(&d, Ubiquitty::SelectionMode::Character, 8, gridCellAt, &g2, nullptr, nullptr);
    checkBounds(d, 0, 3, 1, 7);
}

TEST(testSnapBoundsTopVoidCatchment)
{
    static const char text[] = "abc~~~~~~~~~~~~~" // row0: "abc" + void (16 columns)
                               "~~~~~~~~~~~~~~~~"; // row1: empty
    Grid g = { 2, 16, text };

    // Top endpoint within the catchment (column5 = 3 past "abc", lastPrinted 2):
    // the line is caught → trailing void (column0 → lastPrinted+1).
    Ubiquitty::SelectionBounds c = { 0, 5, 1, 4 };
    Ubiquitty::Selection::snapBoundsToVoid(&c, Ubiquitty::SelectionMode::Character, 16, gridCellAt, &g, nullptr, nullptr);
    CHECK(c.row0 == 0 && c.column0 == 3);
    CHECK(c.row1 == 1 && c.column1 == 15);

    // Top endpoint beyond the catchment (column9 = 7 past "abc"): not caught →
    // the top line drops out (column0 == columns); only the row below selects.
    Ubiquitty::SelectionBounds b = { 0, 9, 1, 4 };
    Ubiquitty::Selection::snapBoundsToVoid(&b, Ubiquitty::SelectionMode::Character, 16, gridCellAt, &g, nullptr, nullptr);
    CHECK(b.row0 == 0 && b.column0 == 16);
    CHECK(b.row1 == 1 && b.column1 == 15);
}

TEST(testSnapBoundsMultirow)
{
    static const char text[] = "ab~~~~~~" // row0: content, anchor in void
                               "cccccccc" // row1: interior, kept full
                               "de~~~~~~"; // row2: content, head in void
    Grid g = { 3, 8, text };
    Ubiquitty::SelectionBounds b = { 0, 5, 2, 7 };
    Ubiquitty::Selection::snapBoundsToVoid(&b, Ubiquitty::SelectionMode::Character, 8, gridCellAt, &g, nullptr, nullptr);
    // Both edges sit in the void of CONTENT lines. The top endpoint's void
    // collapses to that line's trailing newline (column0 → lastPrinted+1 = 2);
    // the bottom head is voidSnapCells past row2's last glyph, so it takes
    // the whole line (column1 → line end). Rows 1 and 2 stay selected.
    checkBounds(b, 0, 2, 2, 7);
}

TEST(testSimWordDragIntoVoidSnapsLine)
{
    // Double-click 'e' in "hello", keep WORD mode (as gopBeginWord does),
    // then drag the head 3 cells into the trailing void — within the CHAR
    // +6 catchment. WORD must snap to the whole line immediately, not clamp
    // to the last glyph the way a char drag would. This is the regression
    // guard for word-mode honouring the catchment by mistake.
    static const char text[] = "hello~~~~~~~"; // 1 row, 12 columns
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, 1, 12, 0, 1, gridCellAt, &g);
    selection.mode = Ubiquitty::SelectionMode::Word; // gopBeginWord keeps WORD mode
    Ubiquitty::Selection::update(&selection, 1, 12, 0, 8); // head 3 past 'o' (lastPrinted 4)
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(rs.selected);
    CHECK(rs.column0 == 0);
    CHECK(rs.column1 == 11); // whole line; would be 4 if the +6 char rule leaked in
}

TEST(test_sim_word_drag_FAR_into_void_snaps_line)
{
    // WORD mode, head dragged FAR into the trailing void (well past the +6
    // char catchment). Word must still snap to end of line — the catchment
    // is char-only. Regression guard for word beyond 6 cells.
    static const char text[] = "hello~~~~~~~~~~~~~~~~~~~"; // 1 row, 24 columns
    Grid g = { 1, 24, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, 1, 24, 0, 1, gridCellAt, &g);
    selection.mode = Ubiquitty::SelectionMode::Word;
    Ubiquitty::Selection::update(&selection, 1, 24, 0, 20); // 16 past 'o' — far beyond +6
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 24, 0, gridCellAt, &g);
    CHECK(rs.selected);
    CHECK(rs.column0 == 0);
    CHECK(rs.column1 == 23); // whole line, not clamped
}

TEST(test_sim_word_drag_UP_catches_top_line_beyond_catchment)
{
    // WORD-mode regression: double-click a word, then drag up so a CONTENT
    // line becomes the selection's TOP edge, with the head far (>6 cells) into
    // that line's trailing void. Word must catch the whole top line (to EOL) —
    // the +6 catchment is char-only; in word mode the line is always caught.
    // (Char mode would drop the top line here; word must not.)
    static const char text[] = "hi~~~~~~~~~~" // row 0: "hi" then void  (lastPrinted = 1)
                               "world~~~~~~~"; // row 1: "world" then void
    Grid g = { 2, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, 2, 12, 1, 2, gridCellAt, &g); // dbl-click "world"
    selection.mode = Ubiquitty::SelectionMode::Word;
    Ubiquitty::Selection::update(&selection, 2, 12, 0, 10); // drag up, 9 past "hi" — far beyond +6

    Ubiquitty::Selection::RowSpan top = simRow(&selection, 2, 12, 0, gridCellAt, &g);
    CHECK(top.selected); // top content line caught, not dropped
    CHECK(top.column0 == 2); // from the first void column after "hi"
    CHECK(top.column1 == 11); // to end of line

    Ubiquitty::Selection::RowSpan bot = simRow(&selection, 2, 12, 1, gridCellAt, &g);
    CHECK(bot.selected); // anchor "world" present
    CHECK(bot.column0 == 0);
    CHECK(bot.column1 == 4);
}

TEST(testSimCharDragIntoVoidKeepsCatchment)
{
    // Same geometry but CHAR mode: within +6 the head clamps to the last
    // glyph; only past the catchment does it take the whole line.
    static const char text[] = "hello~~~~~~~"; // 12 columns, lastPrinted 4
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 1, 12, 0, 0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, 1, 12, 0, 8); // 4 past 'o' → within 6 → clamp
    Ubiquitty::Selection::RowSpan a = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(a.selected && a.column0 == 0 && a.column1 == 4);
    Ubiquitty::Selection::update(&selection, 1, 12, 0, 10); // 6 past → whole line
    Ubiquitty::Selection::RowSpan b = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(b.selected && b.column0 == 0 && b.column1 == 11);
}

TEST(testSimCharSingleCell)
{
    // One cell wide: anchor boundary 2 → head boundary 3 selects cell 2 only.
    static const char text[] = "abcdef~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 2.0, 0, 3.0);
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 8, 0, gridCellAt, &g);
    CHECK(rs.selected && rs.column0 == 2 && rs.column1 == 2);
}

TEST(testSimCharHalfCellPickup)
{
    // Crossing a cell midpoint picks up that cell; staying short of it doesn't.
    static const char text[] = "abcdef~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 2.4, 0, 2.6); // boundary 2 → 3: cell 2
    Ubiquitty::Selection::RowSpan a = simRow(&selection, 1, 8, 0, gridCellAt, &g);
    CHECK(a.selected && a.column0 == 2 && a.column1 == 2);
    simCharacter(&selection, &g, 0, 2.4, 0, 2.4); // both round to boundary 2: empty
    CHECK(!Ubiquitty::Selection::hasSelection(&selection));
}

TEST(testSimCharOffMiddleOnlyNext)
{
    // Press on a cell's right half (boundary 3), drag onto the next cell —
    // only the next cell selects, not the one the press sat in.
    static const char text[] = "abcdef~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 2.6, 0, 3.6); // boundary 3 → 4: cell 3 only
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 8, 0, gridCellAt, &g);
    CHECK(rs.selected && rs.column0 == 3 && rs.column1 == 3);
}

TEST(testSimCharVoidClampThenLine)
{
    // Char drag into the trailing void: clamp to last glyph within +6, snap
    // to the whole line at/beyond it. (caret model)
    static const char text[] = "hello~~~~~~~";
    Grid g = { 1, 12, text }; // lp 4
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 0.0, 0, 8.0); // head boundary 8 → cell 7, 3 past 'o'
    Ubiquitty::Selection::RowSpan a = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(a.selected && a.column0 == 0 && a.column1 == 4); // clamp
    simCharacter(&selection, &g, 0, 0.0, 0, 11.0); // head boundary 11 → cell 10, 6 past
    Ubiquitty::Selection::RowSpan b = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(b.selected && b.column0 == 0 && b.column1 == 11); // whole line
}

TEST(testSimCharMultirowInteriorFull)
{
    // Multi-row char drag: the interior row spans full width; edges snap.
    static const char text[] = "ab~~~~~~"
                               "cccccccc"
                               "de~~~~~~";
    Grid g = { 3, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 1.0, 2, 2.0); // row0 column1 → row2 boundary2 (cell1)
    Ubiquitty::Selection::RowSpan mid = simRow(&selection, 3, 8, 1, gridCellAt, &g);
    CHECK(mid.selected && mid.column0 == 0 && mid.column1 == 7);
}

TEST(testSimCharEmptyLineSlotCollapse)
{
    // An empty line's void collapses to a single slot, so horizontal travel
    // on it never opens a drag (every caret maps to the same slot).
    int s0 = Ubiquitty::Selection::caretSlot(0, -1);
    int s5 = Ubiquitty::Selection::caretSlot(5, -1);
    int s9 = Ubiquitty::Selection::caretSlot(9, -1);
    CHECK(s0 == s5 && s5 == s9); // empty line: one slot
    // A content line keeps caret-granular slots (a real drag can open).
    CHECK(Ubiquitty::Selection::caretSlot(2, 4) != Ubiquitty::Selection::caretSlot(3, 4));
}

// A row holding a code point above the basic plane, beside ordinary letters.
// Read sixteen bits wide, U+10041 is the letter 'A' and U+10020 a space, so a
// double-click beside either extends the word across a character that is
// neither.
struct AstralGrid {
    int columns;
    std::uint32_t codePoint;
    int atColumn;
    const char* letters;
};

static std::uint32_t astralCellAt(void* context, int row, int column)
{
    const AstralGrid* g = static_cast<const AstralGrid*>(context);
    if (row != 0 || column < 0 || column >= g->columns)
        return ' ';
    if (column == g->atColumn)
        return g->codePoint;
    const char c = g->letters[column];
    return (c == '~') ? 0 : static_cast<std::uint32_t>(static_cast<unsigned char>(c));
}

TEST(testAWordStopsAtACodePointAboveTheBasicPlane)
{
    // "ab" then the astral cell then "cd": a double-click on 'a' selects "ab"
    // and stops, because the character beside it is not a word character.
    for (const std::uint32_t codePoint : { 0x10041u, 0x10020u }) {
        AstralGrid g { 6, codePoint, 2, "ab~cd~" };
        Ubiquitty::SelectionState selection;
        Ubiquitty::Selection::init(&selection);
        Ubiquitty::Selection::expandToWord(&selection, 1, g.columns, 0, 0, astralCellAt, &g);
        const Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, g.columns, 0, astralCellAt, &g);
        CHECK(rs.selected);
        CHECK_MSG(rs.column0 == 0 && rs.column1 == 1, "a word ends at the astral cell, not through it");
    }

    // The control: with a plain letter in that column the word runs through it,
    // so the case above is not passing because word selection stopped working.
    AstralGrid plain { 6, static_cast<std::uint32_t>('x'), 2, "ab~cd~" };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, 1, plain.columns, 0, 0, astralCellAt, &plain);
    const Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, plain.columns, 0, astralCellAt, &plain);
    CHECK_MSG(rs.selected && rs.column0 == 0 && rs.column1 == 4, "a plain letter joins the word across the same column");
}

TEST(testSimWordDoubleclickSelectsWord)
{
    static const char text[] = "foo bar baz~";
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 5, 2, Ubiquitty::Selection::GestureMode::Fresh); // dbl-click 'a' of "bar"
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Word);
    CHECK(rs.selected && rs.column0 == 4 && rs.column1 == 6); // "bar"
}

TEST(testSimWordDragStaysWordGranular)
{
    // Double-click a word, drag into a later word — selection re-expands to
    // whole words at both ends, never collapsing to a bare char span.
    static const char text[] = "foo bar baz~";
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 1, 2, Ubiquitty::Selection::GestureMode::Fresh); // "foo"
    Ubiquitty::Selection::gestureDrag(&gest, &operations, 0, 5); // into "bar"
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Word);
    CHECK(rs.selected && rs.column0 == 0 && rs.column1 == 6); // "foo bar", whole words
}

TEST(testSimWordDragBottomVoidSnapsLine)
{
    // Double-click last word, drag right into the trailing void — word snaps
    // to end of line immediately, within and beyond the char +6 catchment.
    static const char text[] = "foo bar~~~~~";
    Grid g = { 1, 12, text }; // lp 6
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 5, 2, Ubiquitty::Selection::GestureMode::Fresh); // "bar"
    Ubiquitty::Selection::gestureDrag(&gest, &operations, 0, 8); // 2 past — within 6
    Ubiquitty::Selection::RowSpan a = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(a.selected && a.column1 == 11);
    Ubiquitty::Selection::gestureDrag(&gest, &operations, 0, 11); // far void
    Ubiquitty::Selection::RowSpan b = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(b.selected && b.column1 == 11);
}

TEST(testSimLineTripleclickWholeRow)
{
    static const char text[] = "hi there~~~~";
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 3, 3, Ubiquitty::Selection::GestureMode::Fresh);
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Line);
    CHECK(rs.selected && rs.column0 == 0 && rs.column1 == 11);
}

TEST(testSimLineDragFullRows)
{
    // Triple-click then drag down: every covered row is whole, including the
    // void — line mode ignores the catchment entirely.
    static const char text[] = "ab~~~~~~"
                               "cd~~~~~~"
                               "ef~~~~~~";
    Grid g = { 3, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 0, 3, Ubiquitty::Selection::GestureMode::Fresh);
    Ubiquitty::Selection::gestureDrag(&gest, &operations, 2, 1);
    for (int r = 0; r < 3; ++r) {
        Ubiquitty::Selection::RowSpan rs = simRow(&selection, 3, 8, r, gridCellAt, &g);
        CHECK(rs.selected && rs.column0 == 0 && rs.column1 == 7);
    }
}

TEST(testSimShiftExtendsSelection)
{
    // A char selection, then shift-click further along extends to that cell.
    static const char text[] = "abcdef~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 1.0, 0, 2.0); // cell 1 selected
    SimGesture sg = { &selection, &g };
    Ubiquitty::Selection::Gesture gest;
    Ubiquitty::Selection::gestureReset(&gest);
    Ubiquitty::Selection::GestureOperations operations = sgOps(&sg);
    Ubiquitty::Selection::gestureBegin(&gest, &operations, 0, 5, 1, Ubiquitty::Selection::GestureMode::Extend); // shift-click cell 5
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 8, 0, gridCellAt, &g);
    CHECK(rs.selected && rs.column0 == 1 && rs.column1 == 5);
}

TEST(testSimRectSelectsVoidLiterally)
{
    // Rect mode never snaps: it selects the void columns as-is.
    static const char text[] = "ab~~~~~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 1, 8, 0, 1, Ubiquitty::SelectionMode::Rectangle);
    Ubiquitty::Selection::update(&selection, 1, 8, 0, 4);
    Ubiquitty::Selection::RowSpan rs = simRow(&selection, 1, 8, 0, gridCellAt, &g);
    CHECK(rs.selected && rs.column0 == 1 && rs.column1 == 4); // void columns 2..4 included
}

TEST(testSimRectInteriorRowIsColumnBlock)
{
    static const char text[] = "abcdefghijklmnopqrstuvwxyz012345"; // 4 rows x 8 columns
    Grid g = { 4, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 4, 8, 1, 2, Ubiquitty::SelectionMode::Rectangle); // anchor (1,2)
    Ubiquitty::Selection::update(&selection, 4, 8, 3, 5); // head (3,5)
    Ubiquitty::Selection::RowSpan mid = simRow(&selection, 4, 8, 2, gridCellAt, &g);
    CHECK(mid.selected && mid.column0 == 2 && mid.column1 == 5); // interior: block, not 0..7
    Ubiquitty::Selection::RowSpan top = simRow(&selection, 4, 8, 1, gridCellAt, &g);
    CHECK(top.selected && top.column0 == 2 && top.column1 == 5);
    Ubiquitty::Selection::RowSpan btm = simRow(&selection, 4, 8, 3, gridCellAt, &g);
    CHECK(btm.selected && btm.column0 == 2 && btm.column1 == 5);
}

TEST(testSimPromptTypedSpaceSelectable)
{
    // A typed trailing space (0x20) is content and selectable; the unwritten
    // void past it is not — the char drag clamps back to the typed space.
    static const char text[] = "user % ~~~~~";
    Grid g = { 1, 12, text }; // lp 6 (space)
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    simCharacter(&selection, &g, 0, 0.0, 0, 7.0); // boundary 7 → cell 6: the typed space
    Ubiquitty::Selection::RowSpan a = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(a.selected && a.column0 == 0 && a.column1 == 6); // includes the space
    simCharacter(&selection, &g, 0, 0.0, 0, 9.0); // boundary 9 → cell 8, into the void (1 past)
    Ubiquitty::Selection::RowSpan b = simRow(&selection, 1, 12, 0, gridCellAt, &g);
    CHECK(b.selected && b.column0 == 0 && b.column1 == 6); // clamps back to the space
}

TEST(testWordBoundsExpandBothEnds)
{
    static const char text[] = "alpha 12"
                               "beta_345"
                               "  gap   ";
    Grid grid = { 3, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, grid.rows, grid.columns, 0, 2, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::update(&selection, grid.rows, grid.columns, 1, 6);

    CHECK(Ubiquitty::Selection::getBounds(&selection, grid.rows, grid.columns, gridCellAt, &grid, &b));
    checkBounds(b, 0, 0, 1, 7);
}

TEST(testLineBoundsCoverFullRows)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 5, 10, 4, 8, Ubiquitty::SelectionMode::Line);
    Ubiquitty::Selection::update(&selection, 5, 10, 2, 1);

    CHECK(Ubiquitty::Selection::getBounds(&selection, 5, 10, gridCellAt, nullptr, &b));
    checkBounds(b, 2, 0, 4, 9);
}

TEST(testCharBoundsClampToGrid)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 3, 5, -20, 99, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, 3, 5, 99, -20);

    CHECK(Ubiquitty::Selection::getBounds(&selection, 3, 5, gridCellAt, nullptr, &b));
    checkBounds(b, 0, 4, 2, 0);
}

TEST(testClearRemovesActiveSelection)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, 2, 4, 0, 0, Ubiquitty::SelectionMode::Character);
    CHECK(Ubiquitty::Selection::hasSelection(&selection));
    Ubiquitty::Selection::clear(&selection);
    CHECK(!Ubiquitty::Selection::hasSelection(&selection));
    CHECK(!Ubiquitty::Selection::getBounds(&selection, 2, 4, gridCellAt, nullptr, &b));
}

TEST(testWordBoundsExpandSpaceRuns)
{
    static const char text[] = "ab  cd"
                               "  ef  ";
    Grid grid = { 2, 6, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, grid.rows, grid.columns, 0, 2, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::update(&selection, grid.rows, grid.columns, 1, 5);

    CHECK(Ubiquitty::Selection::getBounds(&selection, grid.rows, grid.columns, gridCellAt, &grid, &b));
    checkBounds(b, 0, 2, 1, 5);
}

TEST(testSnapToCellMovesOffContinuation)
{
    static const uint16_t chars[] = { 'A', 'W', 0, 'B', ' ', 'X', 0, 'C' };
    static const uint16_t attributes[] = { 0, WC_LEAD, WC_CONT, 0, 0, WC_LEAD, WC_CONT, 0 };
    WideGrid g = { 1, 8, chars, attributes };
    int row, column;

    row = 0;
    column = 2; // continuation half of leading 'W'
    Ubiquitty::Selection::snapToCell(g.rows, g.columns, wideAttrAt, &g, &row, &column);
    CHECK(row == 0);
    CHECK(column == 1);

    row = 0;
    column = 6; // continuation half of leading 'X'
    Ubiquitty::Selection::snapToCell(g.rows, g.columns, wideAttrAt, &g, &row, &column);
    CHECK(column == 5);

    // Non-continuation cell: unchanged.
    row = 0;
    column = 3;
    Ubiquitty::Selection::snapToCell(g.rows, g.columns, wideAttrAt, &g, &row, &column);
    CHECK(column == 3);

    // No attributesAt: clamp only, leave column alone otherwise.
    row = 0;
    column = 2;
    Ubiquitty::Selection::snapToCell(g.rows, g.columns, nullptr, nullptr, &row, &column);
    CHECK(column == 2);

    // Out-of-range coords get clamped first.
    row = 99;
    column = -5;
    Ubiquitty::Selection::snapToCell(g.rows, g.columns, wideAttrAt, &g, &row, &column);
    CHECK(row == 0);
    CHECK(column == 0);
}

TEST(testWideBoundsSnapAnchorAndHead)
{
    static const uint16_t chars[] = { 'A', 'W', 0, 'B', ' ', 'X', 0, 'C' };
    static const uint16_t attributes[] = { 0, WC_LEAD, WC_CONT, 0, 0, WC_LEAD, WC_CONT, 0 };
    WideGrid g = { 1, 8, chars, attributes };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    /// Anchor on continuation half of 'W' (column 2), head on continuation
    /// half of 'X' (column 6). Both should snap to their leading column,
    /// and the trailing edge should expand back through the wide tail
    /// to column 6 so the entire 'X' glyph is included.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, g.rows, g.columns, 0, 2, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, g.rows, g.columns, 0, 6);

    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, g.rows, g.columns, wideCellAt, &g, wideAttrAt, &g, &b));
    // anchor 2 → 1 (leading W), head 6 → 5 (leading X), expanded to 6.
    checkBounds(b, 0, 1, 0, 6);
}

TEST(testWideWordBoundsCoverFullGlyph)
{
    /// A word run that ends with a wide character: "AW" where W is the
    /// wide-glyph pair. The word "AW" should include both halves.
    static const uint16_t chars[] = { 'A', 'W', 0, ' ', 'B', ' ', ' ', ' ' };
    static const uint16_t attributes[] = { 0, WC_LEAD, WC_CONT, 0, 0, 0, 0, 0 };
    WideGrid g = { 1, 8, chars, attributes };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, g.rows, g.columns, 0, 0, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::update(&selection, g.rows, g.columns, 0, 0);

    // Wide-aware bounds: trailing edge expands across the WIDE_CONT
    // cell so column1 == 2 (the full glyph). isWordByte sees ch==0 for
    // the continuation cell and won't extend on its own; the explicit
    // wide-tail expansion handles it.
    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, g.rows, g.columns, wideCellAt, &g, wideAttrAt, &g, &b));
    checkBounds(b, 0, 0, 0, 2);
}

TEST(testWideBoundsFallbackWithoutAttrCallback)
{
    /// With attributesAt = nullptr, GetBoundsWide should behave like the plain
    /// GetBounds: no snap, no wide-tail extension. Caller can opt in
    /// only when it has attribute info.
    static const uint16_t chars[] = { 'A', 'W', 0, 'B', ' ', 'X', 0, 'C' };
    static const uint16_t attributes[] = { 0, WC_LEAD, WC_CONT, 0, 0, WC_LEAD, WC_CONT, 0 };
    WideGrid g = { 1, 8, chars, attributes };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, g.rows, g.columns, 0, 2, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, g.rows, g.columns, 0, 6);

    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, g.rows, g.columns, wideCellAt, &g, nullptr, nullptr, &b));
    checkBounds(b, 0, 2, 0, 6);
}

TEST(testExpandToWordMaterialisesEndpoints)
{
    /// Row layout: "  hello_42 world"
    /// click on column 4 ('l' in "hello_42") should anchor at column 2 and
    /// head at column 9 (end of "hello_42").
    static const char text[] = "  hello_42 world";
    Grid grid = { 1, 16, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 0, 4, gridCellAt, &grid);
    CHECK(Ubiquitty::Selection::hasSelection(&selection));
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 2);
    CHECK(selection.headRow == 0);
    CHECK(selection.headColumn == 9);
    // Bounds should reflect the materialised endpoints directly.
    CHECK(Ubiquitty::Selection::getBounds(&selection, grid.rows, grid.columns, gridCellAt, &grid, &b));
    checkBounds(b, 0, 2, 0, 9);
}

TEST(testExpandToWordOnSeparatorIsSingleCell)
{
    /// Click on a space — word expansion only triggers for word-cells.
    /// For a non-word click the selection collapses to that one cell.
    static const char text[] = "ab  cd";
    Grid grid = { 1, 6, text };
    Ubiquitty::SelectionState selection;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 0, 2, gridCellAt, &grid);
    CHECK(selection.anchorColumn == 2);
    CHECK(selection.headColumn == 3);
}

TEST(testExpandToLineSpansFullRow)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToLine(&selection, 3, 8, 1);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Line); // stays line mode so a drag extends by rows
    CHECK(selection.anchorRow == 1);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 1);
    CHECK(selection.headColumn == 7);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 3, 8, gridCellAt, nullptr, &b));
    checkBounds(b, 1, 0, 1, 7);
}

TEST(testExpandHelpersClampOutOfRange)
{
    static const char text[] = "abcd";
    Grid grid = { 1, 4, text };
    Ubiquitty::SelectionState selection;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 99, -3, gridCellAt, &grid);
    // Click clamps into (0, 0), then expands across "abcd".
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headColumn == 3);

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToLine(&selection, 2, 5, 99);
    CHECK(selection.anchorRow == 1);
    CHECK(selection.headColumn == 4);
}

TEST(testWordBoundsCrossSoftWrap)
{
    /// 2-row grid, 8 columns. Row 0 ends with "verylong" (columns 0..7); row
    /// 1 begins with "wordcontinues" — but only 8 fit, so cells 0..7
    /// hold "wordcont". The double-click lands on row 0 column 4, in the
    /// middle of the wrapped word. With row 0 marked soft-wrapped, the
    /// word-mode bounds should span (0,0) → (1,7).
    static const char text[] = "verylong"
                               "wordcont";
    Grid grid = { 2, 8, text };
    WrapContext wrap = { 0 };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 0, 4, gridCellAt, &grid, wrapCtxAt, &wrap);
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 1);
    CHECK(selection.headColumn == 7);

    /// getBoundsWide on a word-mode anchor/head pair lands at the same
    /// cross-row span when the wrap callback is provided.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, grid.rows, grid.columns, 0, 4, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::update(&selection, grid.rows, grid.columns, 0, 4);
    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, grid.rows, grid.columns, gridCellAt, &grid, nullptr, nullptr, &b, wrapCtxAt, &wrap));
    checkBounds(b, 0, 0, 1, 7);
}

TEST(testWordBoundsNoCrossOnHardBreak)
{
    /// Same cell layout, but no wrap join. The double-click should
    /// only expand within row 0.
    static const char text[] = "verylong"
                               "wordcont";
    Grid grid = { 2, 8, text };
    WrapContext wrap = { -1 };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 0, 4, gridCellAt, &grid, wrapCtxAt, &wrap);
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 0);
    CHECK(selection.headColumn == 7);

    /// With a null wrap callback the word stays on the clicked row too.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 0, 4, gridCellAt, &grid);
    CHECK(selection.headRow == 0);

    /// Double-click on the start of the lower row should expand right
    /// inside row 1 only — without a wrap join its leading edge cannot
    /// walk left into row 0.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 1, 3, gridCellAt, &grid, wrapCtxAt, &wrap);
    CHECK(selection.anchorRow == 1);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 1);
    CHECK(selection.headColumn == 7);

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, grid.rows, grid.columns, 0, 4, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::update(&selection, grid.rows, grid.columns, 0, 4);
    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, grid.rows, grid.columns, gridCellAt, &grid, nullptr, nullptr, &b, wrapCtxAt, &wrap));
    checkBounds(b, 0, 0, 0, 7);
}

TEST(testWordBoundsCrossSoftWrapLeftWalk)
{
    /// Click in the right half of the wrapped word. The left walk must
    /// step from (1, 0) back into (0, 7) and continue.
    static const char text[] = "verylong"
                               "wordcont";
    Grid grid = { 2, 8, text };
    WrapContext wrap = { 0 };
    Ubiquitty::SelectionState selection;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, grid.rows, grid.columns, 1, 3, gridCellAt, &grid, wrapCtxAt, &wrap);
    CHECK(selection.anchorRow == 0);
    CHECK(selection.anchorColumn == 0);
    CHECK(selection.headRow == 1);
    CHECK(selection.headColumn == 7);
}

TEST(testTwoEnginesIndependentWordChars)
{
    Grid g = { 1, 12, "foo-bar baz " };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::Engine eDash;
    Ubiquitty::Selection::initEngine(&eDash);
    /// '-' counts as a word character → "foo-bar" is one word.
    Ubiquitty::Selection::engineSetWordCharacters(&eDash, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");

    Ubiquitty::Selection::Engine eStrict;
    Ubiquitty::Selection::initEngine(&eStrict);
    /// Default classifier: '-' is a separator.

    Ubiquitty::Selection::init(&selection);
    /// Click in 'a' of 'bar' (column 5). Strict engine breaks at the
    /// dash → just "bar".
    Ubiquitty::Selection::expandToWordWith(&selection, 1, 12, 0, 5, gridCellAt, &g, &eStrict);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 12, gridCellAt, &g, &b));
    checkBounds(b, 0, 4, 0, 6);

    /// Same click, dash-friendly engine expands across the dash → "foo-bar".
    Ubiquitty::Selection::expandToWordWith(&selection, 1, 12, 0, 5, gridCellAt, &g, &eDash);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 12, gridCellAt, &g, &b));
    checkBounds(b, 0, 0, 0, 6);

    /// The default engine must not have been disturbed by the *With
    /// calls above.
    Ubiquitty::Selection::setWordCharacters(nullptr);
    Ubiquitty::Selection::expandToWord(&selection, 1, 12, 0, 5, gridCellAt, &g, nullptr, nullptr);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 12, gridCellAt, &g, &b));
    checkBounds(b, 0, 4, 0, 6);
}

TEST(testDefaultEngineCompat)
{
    Grid g = { 1, 12, "foo-bar baz " };
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::setWordCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::expandToWord(&selection, 1, 12, 0, 5, gridCellAt, &g, nullptr, nullptr);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 12, gridCellAt, &g, &b));
    checkBounds(b, 0, 0, 0, 6);

    /// Restore the default so subsequent tests get the original ruleset.
    Ubiquitty::Selection::setWordCharacters(nullptr);
}

TEST(testGestureSingleClickClearsNoHighlight)
{
    FakeGesture f = { 0, 0, 0, 0, 0 };
    Ubiquitty::Selection::GestureOperations operations = fakeOps(&f);
    Ubiquitty::Selection::Gesture g;
    Ubiquitty::Selection::gestureReset(&g);
    /// Plain click clears; no begin happens (no highlight).
    Ubiquitty::Selection::gestureBegin(&g, &operations, 3, 5, 1, Ubiquitty::Selection::GestureMode::Fresh);
    CHECK(f.lastOp == 'X');
    CHECK(f.hasSelection == 0);
    // Release without drag leaves nothing selected.
    CHECK(Ubiquitty::Selection::gestureEnd(&g, &operations) == false);
    CHECK(!Ubiquitty::Selection::gestureDrag(&g, &operations, 3, 8));
}

TEST(testGestureSingleClickThenDragOpensCharacter)
{
    FakeGesture f = { 0, 0, 0, 0, 0 };
    Ubiquitty::Selection::GestureOperations operations = fakeOps(&f);
    Ubiquitty::Selection::Gesture g;
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &operations, 2, 4, 1, Ubiquitty::Selection::GestureMode::Fresh); // clears, anchors at (2,4)
    Ubiquitty::Selection::gestureDrag(&g, &operations, 2, 9);
    // First drag opened a char selection at the anchor, then moved head.
    CHECK(f.hasSelection == 1);
    CHECK(f.lastOp == 'U'); // last call was the head update
    CHECK(f.lastColumn == 9);
    /// A second drag only updates (no second beginCharacter).
    int before = f.opCount;
    Ubiquitty::Selection::gestureDrag(&g, &operations, 2, 12);
    CHECK(f.opCount == before + 1);
    CHECK(f.lastOp == 'U');
}

TEST(testGestureDoubleClickWordTripleLine)
{
    FakeGesture f = { 0, 0, 0, 0, 0 };
    Ubiquitty::Selection::GestureOperations operations = fakeOps(&f);
    Ubiquitty::Selection::Gesture g;
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &operations, 1, 1, 2, Ubiquitty::Selection::GestureMode::Fresh);
    CHECK(f.lastOp == 'W');
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &operations, 1, 1, 3, Ubiquitty::Selection::GestureMode::Fresh);
    CHECK(f.lastOp == 'L');
    /// clickCount > 3 folds back to single-click (clear).
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &operations, 1, 1, 4, Ubiquitty::Selection::GestureMode::Fresh);
    CHECK(f.lastOp == 'X');
}

TEST(testGestureShiftClickExtends)
{
    FakeGesture f = { 0, 0, 0, 0, /*hasSelection*/ 1 }; // an existing selection
    Ubiquitty::Selection::GestureOperations operations = fakeOps(&f);
    Ubiquitty::Selection::Gesture g;
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &operations, 5, 7, 1, Ubiquitty::Selection::GestureMode::Extend);
    CHECK(f.lastOp == 'U'); // extended via update, not a fresh begin
    CHECK(f.lastRow == 5);
    CHECK(f.lastColumn == 7);
    // Shift with no existing selection falls through to plain click.
    FakeGesture f2 = { 0, 0, 0, 0, 0 };
    Ubiquitty::Selection::GestureOperations ops2 = fakeOps(&f2);
    Ubiquitty::Selection::gestureReset(&g);
    Ubiquitty::Selection::gestureBegin(&g, &ops2, 5, 7, 1, Ubiquitty::Selection::GestureMode::Extend);
    CHECK(f2.lastOp == 'X');
}

TEST(testCharBoundsSpanScrollback)
{
    const int totalRows = 1024; // 1000 history + 24 screen
    const int columns = 80;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, totalRows, columns, 5, 3, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, totalRows, columns, 1010, 40);

    CHECK(Ubiquitty::Selection::getBounds(&selection, totalRows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 5, 3, 1010, 40);
}

TEST(testCharBoundsDragUpIntoScrollback)
{
    const int totalRows = 600;
    const int columns = 40;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, totalRows, columns, 590, 10, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, totalRows, columns, 12, 4);

    CHECK(Ubiquitty::Selection::getBounds(&selection, totalRows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 12, 4, 590, 10);
}

TEST(testSelectAllBufferBounds)
{
    const int totalRows = 777;
    const int columns = 100;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, totalRows, columns, 0, 0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, totalRows, columns, totalRows - 1, columns - 1);

    CHECK(Ubiquitty::Selection::getBounds(&selection, totalRows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 0, 0, totalRows - 1, columns - 1);
}

TEST(testLineBoundsSpanScrollback)
{
    const int totalRows = 300;
    const int columns = 50;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, totalRows, columns, 280, 5, Ubiquitty::SelectionMode::Line);
    Ubiquitty::Selection::update(&selection, totalRows, columns, 20, 9);

    CHECK(Ubiquitty::Selection::getBounds(&selection, totalRows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 20, 0, 280, columns - 1);
}

TEST(testRectBoundsSameColumnSpanEveryRow)
{
    const int rows = 6, columns = 12;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    // Anchor lower-right, head upper-left: rows + columns both flip so the
    // block normalizes to its top-left / bottom-right corners.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, rows, columns, 4, 9, Ubiquitty::SelectionMode::Rectangle);
    Ubiquitty::Selection::update(&selection, rows, columns, 1, 3);

    CHECK(Ubiquitty::Selection::isRectangular(&selection));
    CHECK(Ubiquitty::Selection::getBounds(&selection, rows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 1, 3, 4, 9);
}

TEST(testRectBoundsColumnNormalizedNotLinear)
{
    const int rows = 5, columns = 10;
    Ubiquitty::SelectionState selection;
    Ubiquitty::SelectionBounds b;

    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::begin(&selection, rows, columns, 0, 7, Ubiquitty::SelectionMode::Rectangle);
    Ubiquitty::Selection::update(&selection, rows, columns, 3, 2);

    CHECK(Ubiquitty::Selection::getBounds(&selection, rows, columns, gridCellAt, nullptr, &b));
    checkBounds(b, 0, 2, 3, 7);

    // getBoundsWide (no attribute callback) must agree with getBounds.
    Ubiquitty::SelectionBounds w;
    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, rows, columns, gridCellAt, nullptr, nullptr, nullptr, &w));
    checkBounds(w, 0, 2, 3, 7);
}

TEST(testRowSpanInteriorIsO1)
{
    static const char text[] = "ab~~~~~~"
                               "cccccccc"
                               "de~~~~~~";
    CountContext c = { { 3, 8, text }, 0, 0 };
    Ubiquitty::SelectionBounds b = { 0, 5, 2, 7 };

    // Interior row 1 resolves full-width without consulting either callback.
    Ubiquitty::Selection::RowSpan rs = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Character, 1, 8, countingCellAt, &c, countingAttrAt, &c);
    CHECK(rs.column0 == 0);
    CHECK(rs.column1 == 7);
    CHECK(rs.selected);
    CHECK(c.cellCalls == 0);
    CHECK(c.attrCalls == 0);

    // Out-of-range row returns {0,0,false} and also touches nothing.
    Ubiquitty::Selection::RowSpan off = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Character, 9, 8, countingCellAt, &c, countingAttrAt, &c);
    CHECK(off.column0 == 0 && off.column1 == 0 && !off.selected);
    CHECK(c.cellCalls == 0);
    CHECK(c.attrCalls == 0);
}

TEST(testRowSpanEmptyLineDrop)
{
    // Single empty line, head deep in the void: lastPrinted < 0 → whole line
    // on the bottom edge; on a single-row selection the top edge collapses to
    // lp+1 == 0.
    static const char text[] = "~~~~~~~~";
    Grid g = { 1, 8, text };
    Ubiquitty::SelectionBounds b = { 0, 3, 0, 6 };
    Ubiquitty::Selection::RowSpan rs = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    // bottom edge: lp<0 → c1 = columns-1 = 7; top edge: lp<0, single row → lp+1 = 0.
    CHECK(rs.column0 == 0);
    CHECK(rs.column1 == 7);
    CHECK(rs.selected);
}

TEST(testRowSpanVoidSnapBoundary)
{
    static const char text[] = "abc~~~~~~~~~"; // 1 row, 12 columns, lastPrinted 2
    Grid g = { 1, 12, text };

    // Distance 5 (column 7 = lp2 + 5): within catchment → clamp to lastPrinted.
    Ubiquitty::SelectionBounds b5 = { 0, 0, 0, 7 };
    Ubiquitty::Selection::RowSpan d5 = Ubiquitty::Selection::rowSpan(&b5, Ubiquitty::SelectionMode::Character, 0, 12, gridCellAt, &g, nullptr, nullptr);
    CHECK(d5.column1 == 2);

    // Distance 6 (column 8 = lp2 + 6): at voidSnapCells → whole line.
    Ubiquitty::SelectionBounds b6 = { 0, 0, 0, 8 };
    Ubiquitty::Selection::RowSpan d6 = Ubiquitty::Selection::rowSpan(&b6, Ubiquitty::SelectionMode::Character, 0, 12, gridCellAt, &g, nullptr, nullptr);
    CHECK(d6.column1 == 11);
}

TEST(testRowSpanWordBottomVoidWholeLine)
{
    // Word mode: a single cell past the text snaps to the whole line, no
    // catchment wait.
    static const char text[] = "abc~~~~~~~~~"; // lastPrinted 2
    Grid g = { 1, 12, text };
    Ubiquitty::SelectionBounds b = { 0, 0, 0, 3 }; // just one past "abc"
    Ubiquitty::Selection::RowSpan rs = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Word, 0, 12, gridCellAt, &g, nullptr, nullptr);
    CHECK(rs.column1 == 11);
}

TEST(testRowSpanTopEdgeSingleVsMulti)
{
    // Empty top line, top endpoint in the void.
    static const char text[] = "~~~~~~~~"
                               "~~~~~~~~";
    Grid g = { 2, 8, text };

    // Multi-row: top line drops out (column0 == columns).
    Ubiquitty::SelectionBounds multi = { 0, 3, 1, 5 };
    Ubiquitty::Selection::RowSpan tMulti = Ubiquitty::Selection::rowSpan(&multi, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(tMulti.column0 == 8);

    // Single-row empty line: collapses to lp+1 == 0 instead of columns.
    Ubiquitty::SelectionBounds single = { 0, 3, 0, 5 };
    Ubiquitty::Selection::RowSpan tSingle = Ubiquitty::Selection::rowSpan(&single, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(tSingle.column0 == 0);

    // Content top line beyond the catchment (multi-row) → drops out (columns).
    static const char text2[] = "abc~~~~~~~~~~~~~"
                                "~~~~~~~~~~~~~~~~";
    Grid g2 = { 2, 16, text2 };
    Ubiquitty::SelectionBounds beyond = { 0, 9, 1, 4 }; // 7 past lp2
    Ubiquitty::Selection::RowSpan tBeyond = Ubiquitty::Selection::rowSpan(&beyond, Ubiquitty::SelectionMode::Character, 0, 16, gridCellAt, &g2, nullptr, nullptr);
    CHECK(tBeyond.column0 == 16);
}

TEST(testABlankTopRowStaysWhenTheBoundsAreAnchoredByCommand)
{
    // Blank top row, multi-row bounds from its first cell. A pointer's drag
    // drops the row: (0, 0) is the row's void. A command's selection keeps it
    // whole, and the bottom row is the pointer's business either way.
    static const char text[] = "~~~~~~~~"
                               "ab~~~~~~";
    Grid g = { 2, 8, text };
    Ubiquitty::SelectionBounds pointer = { 0, 0, 1, 1 };
    Ubiquitty::Selection::RowSpan dragged = Ubiquitty::Selection::rowSpan(&pointer, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(!dragged.selected);
    CHECK(dragged.column0 == 8);

    Ubiquitty::SelectionBounds command = { 0, 0, 1, 1 };
    command.isAnchoredByCommand = true;
    Ubiquitty::Selection::RowSpan kept = Ubiquitty::Selection::rowSpan(&command, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(kept.selected);
    CHECK(kept.column0 == 0 && kept.column1 == 7);
    Ubiquitty::Selection::RowSpan below = Ubiquitty::Selection::rowSpan(&command, Ubiquitty::SelectionMode::Character, 1, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(below.selected);
    CHECK(below.column0 == 0 && below.column1 == 1);

    // A single blank row collapses to its one slot with or without the fact.
    Ubiquitty::SelectionBounds one = { 0, 3, 0, 5 };
    one.isAnchoredByCommand = true;
    Ubiquitty::Selection::RowSpan alone = Ubiquitty::Selection::rowSpan(&one, Ubiquitty::SelectionMode::Character, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(alone.column0 == 0 && alone.column1 == 7);
}

TEST(testSelectAllIsAnchoredByCommandUntilTheAnchorMoves)
{
    static const char text[] = "~~~~~~~~"
                               "ab cd~~~"
                               "~~~~~~~~";
    Grid g = { 3, 8, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    CHECK(!selection.isAnchoredByCommand);

    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.isAnchoredByCommand);
    CHECK(selection.anchorRow == 0 && selection.anchorColumn == 0);
    CHECK(selection.headRow == 2 && selection.headColumn == 7);

    // The bounds carry the fact to the void rule, wide or not.
    Ubiquitty::SelectionBounds b = { 9, 9, 9, 9 };
    CHECK(Ubiquitty::Selection::getBounds(&selection, 3, 8, gridCellAt, &g, &b));
    CHECK(b.isAnchoredByCommand);
    Ubiquitty::SelectionBounds wide = { 9, 9, 9, 9 };
    CHECK(Ubiquitty::Selection::getBoundsWide(&selection, 3, 8, gridCellAt, &g, nullptr, nullptr, &wide));
    CHECK(wide.isAnchoredByCommand);

    // Moving the head is a trim from the buffer start; the fact stays.
    Ubiquitty::Selection::update(&selection, 3, 8, 1, 3);
    CHECK(selection.isAnchoredByCommand);

    // Every placement of the anchor is a pointer's or a click's, and clears it.
    Ubiquitty::Selection::begin(&selection, 3, 8, 0, 0, Ubiquitty::SelectionMode::Character);
    CHECK(!selection.isAnchoredByCommand);
    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    Ubiquitty::Selection::charDragLinear(&selection, 3, 8, 0, 0, 1, 2);
    CHECK(!selection.isAnchoredByCommand);
    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    Ubiquitty::Selection::expandToWord(&selection, 3, 8, 1, 0, gridCellAt, &g);
    CHECK(!selection.isAnchoredByCommand);
    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    Ubiquitty::Selection::expandToLine(&selection, 3, 8, 1);
    CHECK(!selection.isAnchoredByCommand);
    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    Ubiquitty::Selection::setCharacterSpan(&selection, 3, 8, 1, 3, 4);
    CHECK(!selection.isAnchoredByCommand);
    Ubiquitty::Selection::selectAll(&selection, 3, 8);
    Ubiquitty::Selection::init(&selection);
    CHECK(!selection.isAnchoredByCommand);
}

TEST(testASmartTokenPlacesTheAnchorAndClearsTheCommandFact)
{
    static const char text[] = "see https://a.io/x ok   ";
    Grid grid = { 1, 24, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::selectAll(&selection, 1, 24);
    CHECK(selection.isAnchoredByCommand);
    CHECK(Ubiquitty::Selection::expandToSmartToken(&selection, 1, 24, 0, 10, gridCellAt, &grid));
    CHECK(!selection.isAnchoredByCommand);
}

TEST(testSelectRegionIsACommandsPlacement)
{
    // A region a command names: character mode, anchored by command, the
    // cells as given and clamped to the grid. Select All is the region over
    // the whole buffer.
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::selectRegion(&selection, 6, 8, { 2, 0, 4, 7 });
    CHECK(selection.active);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.isAnchoredByCommand);
    CHECK(selection.anchorRow == 2 && selection.anchorColumn == 0);
    CHECK(selection.headRow == 4 && selection.headColumn == 7);

    Ubiquitty::SelectionState all;
    Ubiquitty::Selection::init(&all);
    Ubiquitty::Selection::selectAll(&all, 6, 8);
    Ubiquitty::SelectionState whole;
    Ubiquitty::Selection::init(&whole);
    Ubiquitty::Selection::selectRegion(&whole, 6, 8, { 0, 0, 5, 7 });
    CHECK(all.anchorRow == whole.anchorRow && all.anchorColumn == whole.anchorColumn);
    CHECK(all.headRow == whole.headRow && all.headColumn == whole.headColumn);
    CHECK(all.mode == whole.mode && all.isAnchoredByCommand == whole.isAnchoredByCommand);

    Ubiquitty::Selection::selectRegion(&selection, 6, 8, { -1, -3, 9, 40 });
    CHECK(selection.anchorRow == 0 && selection.anchorColumn == 0);
    CHECK(selection.headRow == 5 && selection.headColumn == 7);
}

TEST(testACommandsOutputKeepsItsBlankFirstRow)
{
    // The output region starts on a blank row: a program that opens its
    // output with a newline. A pointer's drag from that row starts below;
    // the command's selection paints the row whole.
    static const char text[] = "$ cmd~~~"
                               "~~~~~~~~"
                               "out~~~~~"
                               "~~~~~~~~";
    Grid g = { 4, 8, text };
    uint8_t marks[4] = {
        Ubiquitty::Selection::selectionMarkPromptStart | Ubiquitty::Selection::selectionMarkCmdStart,
        Ubiquitty::Selection::selectionMarkOutputStart,
        0,
        0,
    };
    MarkGrid mg = { 4, marks };
    Ubiquitty::SelectionBounds region;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 4, 8, 3, &region));
    CHECK(region.row0 == 1 && region.column0 == 0);

    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::selectRegion(&selection, 4, 8, region);
    Ubiquitty::SelectionBounds b = { 9, 9, 9, 9 };
    CHECK(Ubiquitty::Selection::getBounds(&selection, 4, 8, gridCellAt, &g, &b));
    Ubiquitty::Selection::RowSpan first = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Character, 1, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(first.selected);
    CHECK(first.column0 == 0 && first.column1 == 7);

    Ubiquitty::Selection::begin(&selection, 4, 8, region.row0, region.column0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::update(&selection, 4, 8, region.row1, region.column1);
    CHECK(Ubiquitty::Selection::getBounds(&selection, 4, 8, gridCellAt, &g, &b));
    Ubiquitty::Selection::RowSpan dragged = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Character, 1, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(!dragged.selected);
}

TEST(testRowSpanRectLinePassthrough)
{
    static const char text[] = "ab~~~~~~"
                               "cccccccc"
                               "de~~~~~~";
    Grid g = { 3, 8, text };
    Ubiquitty::SelectionBounds b = { 0, 2, 2, 6 };

    // Rect: rowSpan clips every row to the bounds' [column0,column1] column block (the
    // void rule is skipped — the void columns inside the block are selected as-is).
    // Centralised here so each frontend renders the block without re-clipping.
    // b = {row0=0,column0=2,row1=2,column1=6} → columns 2..6 on rows 0, 1 and 2.
    Ubiquitty::Selection::RowSpan rTop = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Rectangle, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(rTop.column0 == 2 && rTop.column1 == 6 && rTop.selected);
    Ubiquitty::Selection::RowSpan rBot = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Rectangle, 2, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(rBot.column0 == 2 && rBot.column1 == 6 && rBot.selected);

    Ubiquitty::Selection::RowSpan lTop = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Line, 0, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(lTop.column0 == 2 && lTop.column1 == 7 && lTop.selected);
    Ubiquitty::Selection::RowSpan lBot = Ubiquitty::Selection::rowSpan(&b, Ubiquitty::SelectionMode::Line, 2, 8, gridCellAt, &g, nullptr, nullptr);
    CHECK(lBot.column0 == 0 && lBot.column1 == 6 && lBot.selected);
}

TEST(testCaretSlot)
{
    // Empty line (lastPrinted < 0): caret pins to slot 0.
    CHECK(Ubiquitty::Selection::caretSlot(5, -1) == 0);
    CHECK(Ubiquitty::Selection::caretSlot(0, -1) == 0);
    // Content line: caret unchanged.
    CHECK(Ubiquitty::Selection::caretSlot(5, 3) == 5);
    CHECK(Ubiquitty::Selection::caretSlot(0, 0) == 0);
}

TEST(testCharDragLinear)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);

    // Linear order across rows: anchor (1,2), head (3,5) on an 8-column grid.
    // lo = 1*8+2 = 10, hi = 3*8+5 = 29, e = 28 → (3,4).
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 1, 2, 3, 5);
    CHECK(selection.active);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.anchorRow == 1 && selection.anchorColumn == 2);
    CHECK(selection.headRow == 3 && selection.headColumn == 4);

    // Reversed endpoints normalize to the same span.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 3, 5, 1, 2);
    CHECK(selection.anchorRow == 1 && selection.anchorColumn == 2);
    CHECK(selection.headRow == 3 && selection.headColumn == 4);

    // Equal carets clear the selection.
    Ubiquitty::Selection::begin(&selection, 5, 8, 0, 0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 2, 4, 2, 4);
    CHECK(!selection.active);

    // Half-open inclusive mapping: caret 0..3 on row 0 → cells [0,2].
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 0, 0, 3);
    CHECK(selection.anchorRow == 0 && selection.anchorColumn == 0);
    CHECK(selection.headRow == 0 && selection.headColumn == 2);

    // Caret at end of a row rolls onto the next row's last cell.
    // anchor (0,6), head (1,0): lo=6, hi=8, e=7 → (0,7).
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 6, 1, 0);
    CHECK(selection.headRow == 0 && selection.headColumn == 7);
}

// A caret inside a packed proportional cell is a boundary plus letters into
// the cell right of it. The pair orders lexicographically, and the range it
// cuts is the same half-open rule one boundary further in.
TEST(testTheCaretDragCutsInsideAPackedCell)
{
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);

    // Both carets inside one cell: the cell is the whole span, and the offsets
    // say which of its letters the copy keeps.
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 1, 0, 2, 3);
    CHECK(selection.active);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.anchorRow == 0 && selection.anchorColumn == 2);
    CHECK(selection.headRow == 0 && selection.headColumn == 2);
    CHECK(selection.startSubColumn == 1 && selection.endSubColumn == 3);

    // Reversed endpoints normalize to the same cut.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 3, 0, 2, 1);
    CHECK(selection.anchorColumn == 2 && selection.headColumn == 2);
    CHECK(selection.startSubColumn == 1 && selection.endSubColumn == 3);

    // Two carets one letter apart order by the offset, since the boundary they
    // share cannot separate them.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 2, 0, 2, 1);
    CHECK(selection.startSubColumn == 1 && selection.endSubColumn == 2);

    // An end at a boundary keeps none of the cell to its right, as the
    // boundary form does: cells [2,2], with the start's first letter dropped.
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 1, 0, 3, 0);
    CHECK(selection.anchorColumn == 2 && selection.headColumn == 2);
    CHECK(selection.startSubColumn == 1 && selection.endSubColumn == 0);

    // Two carets at the same letter of the same cell select nothing.
    Ubiquitty::Selection::begin(&selection, 5, 8, 0, 0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 2, 0, 2, 2);
    CHECK(!selection.active);

    // With both offsets zero this is the boundary form exactly.
    Ubiquitty::SelectionState boundaries;
    Ubiquitty::Selection::init(&boundaries);
    Ubiquitty::Selection::charDragLinear(&boundaries, 5, 8, 1, 2, 3, 5);
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 1, 2, 0, 3, 5, 0);
    CHECK(selection.anchorRow == boundaries.anchorRow && selection.anchorColumn == boundaries.anchorColumn);
    CHECK(selection.headRow == boundaries.headRow && selection.headColumn == boundaries.headColumn);
    CHECK(selection.startSubColumn == 0 && selection.endSubColumn == 0);
}

// Every way of placing an endpoint but the caret drag drops the offsets. The
// whole invariant rests on this: a cut counts into the cell its edge stands
// on, and an edge placed afresh no longer stands there.
TEST(testEveryOtherEndpointPlacementDropsTheCut)
{
    Ubiquitty::SelectionState selection;
    const auto cutting = [&selection] {
        Ubiquitty::Selection::init(&selection);
        Ubiquitty::Selection::charDragLinear(&selection, 5, 8, 0, 2, 1, 0, 2, 3);
        CHECK(selection.startSubColumn == 1 && selection.endSubColumn == 3);
    };
    const auto isWholeCells = [&selection] {
        return selection.startSubColumn == 0 && selection.endSubColumn == 0;
    };

    // A cell drag, which moves the head through `update`.
    cutting();
    Ubiquitty::Selection::update(&selection, 5, 8, 0, 5);
    CHECK_MSG(isWholeCells(), "a head placed by cells takes its cells whole");

    // A Shift-click extension, which re-places the anchor through `begin`.
    cutting();
    Ubiquitty::Selection::begin(&selection, 5, 8, 0, 1, Ubiquitty::SelectionMode::Character);
    CHECK_MSG(isWholeCells(), "an anchor placed afresh takes its cells whole");

    // A word expansion.
    static const char row[] = "one two ";
    Grid grid { 1, 8, row };
    cutting();
    Ubiquitty::Selection::expandToWord(&selection, 1, 8, 0, 1, gridCellAt, &grid);
    CHECK_MSG(isWholeCells(), "a word expansion takes whole cells");

    // A smart-token expansion.
    cutting();
    (void)Ubiquitty::Selection::expandToSmartToken(&selection, 1, 8, 0, 1, gridCellAt, &grid);
    CHECK_MSG(isWholeCells(), "a smart-token expansion takes whole cells");

    // A line expansion.
    cutting();
    Ubiquitty::Selection::expandToLine(&selection, 5, 8, 0);
    CHECK_MSG(isWholeCells(), "a line expansion takes whole cells");

    // A character span placed by a caller.
    cutting();
    Ubiquitty::Selection::setCharacterSpan(&selection, 5, 8, 0, 1, 4);
    CHECK_MSG(isWholeCells(), "a character span takes whole cells");

    // A region, and Select All, which is a region over the whole buffer.
    cutting();
    Ubiquitty::Selection::selectRegion(&selection, 5, 8, { 0, 0, 1, 7 });
    CHECK_MSG(isWholeCells(), "a region takes whole cells");
    cutting();
    Ubiquitty::Selection::selectAll(&selection, 5, 8);
    CHECK_MSG(isWholeCells(), "Select All takes whole cells");

    // And a cleared selection carries none.
    cutting();
    Ubiquitty::Selection::clear(&selection);
    Ubiquitty::Selection::init(&selection);
    CHECK(isWholeCells());
}

TEST(testSmartTokenExpandsURL)
{
    // "see https://a.io/x ok" laid out on one 24-column row.
    static const char text[] = "see https://a.io/x ok   ";
    Grid grid = { 1, 24, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);

    // Click inside the URL (column 10, the 'a').
    CHECK(Ubiquitty::Selection::expandToSmartToken(&selection, 1, 24, 0, 10, gridCellAt, &grid));
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    // Token spans columns 4.."https://a.io/x" inclusive (ends column 17).
    CHECK(selection.anchorRow == 0 && selection.anchorColumn == 4);
    CHECK(selection.headRow == 0 && selection.headColumn == 17);
}

TEST(testSmartTokenIncludesPathAndHash)
{
    static const char text[] = "/usr/bin/a deadBEEF1234 z";
    Grid grid = { 1, 25, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);

    // Path token: columns 0.."/usr/bin/a" (ends column 9).
    CHECK(Ubiquitty::Selection::expandToSmartToken(&selection, 1, 25, 0, 3, gridCellAt, &grid));
    CHECK(selection.anchorColumn == 0 && selection.headColumn == 9);

    // Hash token: columns 11.."deadBEEF1234" (ends column 22).
    CHECK(Ubiquitty::Selection::expandToSmartToken(&selection, 1, 25, 0, 15, gridCellAt, &grid));
    CHECK(selection.anchorColumn == 11 && selection.headColumn == 22);
}

TEST(testSmartTokenOnSeparatorReturnsFalse)
{
    static const char text[] = "ab  cd";
    Grid grid = { 1, 6, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    selection.active = false;

    // Column 2 is a space — not a token char.
    CHECK(!Ubiquitty::Selection::expandToSmartToken(&selection, 1, 6, 0, 2, gridCellAt, &grid));
    // No-op: selection untouched.
    CHECK(!selection.active);

    CHECK(!Ubiquitty::Selection::isSmartTokenByte(' '));
    CHECK(!Ubiquitty::Selection::isSmartTokenByte('"'));
    CHECK(!Ubiquitty::Selection::isSmartTokenByte('<'));
    CHECK(Ubiquitty::Selection::isSmartTokenByte('/'));
    CHECK(Ubiquitty::Selection::isSmartTokenByte('.'));
    CHECK(Ubiquitty::Selection::isSmartTokenByte('A'));
    // Non-ASCII is never a token char.
    CHECK(!Ubiquitty::Selection::isSmartTokenByte(0x00E9));
}

TEST(testDoubleClickWordThenDragStaysWord)
{
    static const char text[] = "alpha bravo charlie    ";
    Grid grid = { 1, 23, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::Engine e;
    Ubiquitty::Selection::initEngine(&e);

    // Double-click inside "bravo" (column 8), smart disabled (the default).
    Ubiquitty::Selection::doubleClickExpand(&selection, 1, 23, 0, 8, gridCellAt, &grid, &e, Ubiquitty::Selection::SmartTokenMode::Disabled);
    CHECK(selection.active);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Word);

    // Drag the head into the middle of "charlie" (column 14).
    Ubiquitty::Selection::update(&selection, 1, 23, 0, 14);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Word);

    // Re-snaps to whole words: start of "bravo" (6) .. end of "charlie" (18),
    // not a char span ending at column 14.
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 23, gridCellAt, &grid, &b));
    CHECK(b.column0 == 6);
    CHECK(b.column1 == 18);
}

TEST(testDoubleClickSmartEnabledSelectsToken)
{
    static const char text[] = "see https://a.io/x ok   ";
    Grid grid = { 1, 24, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::Engine e;
    Ubiquitty::Selection::initEngine(&e);

    Ubiquitty::Selection::doubleClickExpand(&selection, 1, 24, 0, 10, gridCellAt, &grid, &e, Ubiquitty::Selection::SmartTokenMode::Enabled);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Character);
    CHECK(selection.anchorColumn == 4 && selection.headColumn == 17);
}

TEST(testDoubleClickSmartDisabledWordOnURL)
{
    static const char text[] = "see https://a.io/x ok   ";
    Grid grid = { 1, 24, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::Engine e;
    Ubiquitty::Selection::initEngine(&e);

    // Click the 't' in "https" (column 6): word is "https" (columns 4..8), not the
    // whole "https://a.io/x" token (4..17).
    Ubiquitty::Selection::doubleClickExpand(&selection, 1, 24, 0, 6, gridCellAt, &grid, &e, Ubiquitty::Selection::SmartTokenMode::Disabled);
    CHECK(selection.mode == Ubiquitty::SelectionMode::Word);
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::getBounds(&selection, 1, 24, gridCellAt, &grid, &b));
    CHECK(b.column0 == 4);
    CHECK(b.column1 == 8);
}

TEST(testLastOutputBasicSpan)
{
    // 6 rows. Row 0: prompt+cmd. Row 1: ; C. Rows 1-3: output. Row 4: ; D.
    uint8_t marks[6] = {
        Ubiquitty::Selection::selectionMarkPromptStart | Ubiquitty::Selection::selectionMarkCmdStart,
        Ubiquitty::Selection::selectionMarkOutputStart,
        0,
        0,
        Ubiquitty::Selection::selectionMarkCmdEnd,
        0,
    };
    MarkGrid mg = { 6, marks };
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 6, 10, 5, &b));
    // Output rows 1..3 (; D on row 4 is the next prompt, excluded).
    checkBounds(b, 1, 0, 3, 9);
}

TEST(testLastOutputRunningCommandToEnd)
{
    // ; C on row 2, no ; D yet → output runs to the last buffer row.
    uint8_t marks[5] = {
        0,
        0,
        Ubiquitty::Selection::selectionMarkOutputStart,
        0,
        0,
    };
    MarkGrid mg = { 5, marks };
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 5, 8, 4, &b));
    checkBounds(b, 2, 0, 4, 7);
}

TEST(testLastOutputPicksMostRecent)
{
    // Two commands. Ceiling sits below the second ; C → second wins.
    uint8_t marks[7] = {
        Ubiquitty::Selection::selectionMarkOutputStart, // cmd1 output
        Ubiquitty::Selection::selectionMarkCmdEnd, // cmd1 end
        0,
        Ubiquitty::Selection::selectionMarkOutputStart, // cmd2 output
        0,
        Ubiquitty::Selection::selectionMarkCmdEnd, // cmd2 end
        0,
    };
    MarkGrid mg = { 7, marks };
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 7, 4, 6, &b));
    checkBounds(b, 3, 0, 4, 3);
}

TEST(testLastOutputZeroOutputSingleRow)
{
    // ; C and ; D on the same row (command produced no output).
    uint8_t marks[3] = {
        0,
        Ubiquitty::Selection::selectionMarkOutputStart | Ubiquitty::Selection::selectionMarkCmdEnd,
        0,
    };
    MarkGrid mg = { 3, marks };
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 3, 5, 2, &b));
    checkBounds(b, 1, 0, 1, 4);
}

TEST(testLastOutputNoMarksReturnsFalse)
{
    uint8_t marks[3] = { 0, 0, 0 };
    MarkGrid mg = { 3, marks };
    Ubiquitty::SelectionBounds b;
    b.row0 = 99;
    CHECK(!Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 3, 5, 2, &b));
    // Out untouched on failure.
    CHECK(b.row0 == 99);
    // Null provider / degenerate geometry also fail safely.
    CHECK(!Ubiquitty::Selection::lastCommandOutputRegion(nullptr, &mg, 3, 5, 2, &b));
    CHECK(!Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 0, 5, 2, &b));
}

TEST(testLastOutputMaxClampsIntoRunning)
{
    // Ceiling above a still-running ; C: a ceiling past totalRows-1 is
    // clamped, and the search still finds the latest ; C.
    uint8_t marks[4] = {
        Ubiquitty::Selection::selectionMarkOutputStart,
        Ubiquitty::Selection::selectionMarkCmdEnd,
        Ubiquitty::Selection::selectionMarkOutputStart,
        0,
    };
    MarkGrid mg = { 4, marks };
    Ubiquitty::SelectionBounds b;
    CHECK(Ubiquitty::Selection::lastCommandOutputRegion(markAt, &mg, 4, 6, 999, &b));
    checkBounds(b, 2, 0, 3, 5);
}

// An engine nobody initialised still classifies word bytes.
//
// Every other case here calls initEngine first, so this is the only case on
// the path a caller takes when it holds an Engine by value and never
// initialises it. That is the shape WebTerminal has: `Selection::Session
// m_selection {}` and no init call, where Linux calls Selection::init and macOS
// calls initEngine.
//
// With an all-zero bitmap no byte is a word byte, so double-click expansion
// finds no word to grow into and takes the single cell under the click. The
// failure then reads as broken selection rather than as an uninitialised
// engine, which is the reading it cost: "hello world" clicked at column 7
// selected "o".
TEST(anUninitialisedEngineStillKnowsAWordByte)
{
    // Not initEngine'd: value-initialised, as a caller embedding
    // it in a struct gets.
    Ubiquitty::Selection::Engine bare {};
    CHECK(Ubiquitty::Selection::engineIsWordByte(&bare, 'w'));
    CHECK(Ubiquitty::Selection::engineIsWordByte(&bare, '_'));
    CHECK(Ubiquitty::Selection::engineIsWordByte(&bare, '7'));
    CHECK(!Ubiquitty::Selection::engineIsWordByte(&bare, ' '));

    // And the expansion it feeds takes the word rather than the cell: this is
    // what the web arm was failing, at Core.
    //
    // The span is read off the state rather than through getBounds. That
    // overload takes no engine and re-derives the word from whichever engine is
    // bound at the time, which is the initialised default -- so it answers 6..10
    // whether or not the engine passed here classifies anything, and asserting
    // through it would pass against the defect it is here to catch.
    // doubleClickExpand binds this engine for the expansion, so the columns it
    // leaves behind are the ones that carry the answer.
    static const char text[] = "hello world         ";
    Grid grid = { 1, 20, text };
    Ubiquitty::SelectionState selection;
    Ubiquitty::Selection::init(&selection);
    Ubiquitty::Selection::doubleClickExpand(&selection, 1, 20, 0, 7, gridCellAt, &grid, &bare,
        Ubiquitty::Selection::SmartTokenMode::Disabled);
    CHECK(selection.active);
    CHECK_EQ(selection.anchorColumn, 6);
    CHECK_EQ(selection.headColumn, 10);
}

UBIQUITTY_TEST_MAIN()
