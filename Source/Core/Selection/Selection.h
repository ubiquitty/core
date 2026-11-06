// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Accessibility/TextProjection.h"

#include "Base/Types.h"

namespace Ubiquitty {

enum class SelectionMode : int {
    Character = 0,
    Word = 1,
    Line = 2,
    // Rectangular (column-block) selection: every row in the span clips to the
    // same [column0,column1] column range — getBounds reports the normalized span and
    // callers apply it to each row (no line-wrap fill).
    Rectangle = 3,
};

struct SelectionState {
    bool active = false;
    int anchorRow = 0;
    int anchorColumn = 0;
    int headRow = 0;
    int headColumn = 0;
    SelectionMode mode = SelectionMode::Character;
    /// True when a command put the anchor at a row's first cell, the way Select
    /// All and Select Last Command Output do.
    ///
    /// A pointer pressed on a blank row sits in that row's void, and the void
    /// rule starts the selection on the row after. A command names the row's
    /// start, so the row stays in. Moving the head keeps the fact; every
    /// function that places the anchor afresh clears it.
    bool isAnchoredByCommand = false;
    /// Code points of the first selected cell's text the selection leaves out,
    /// and of the last selected cell's text it keeps.
    ///
    /// A proportional run packs several letters' ink into one cell, so a
    /// character drag between two of them names letters rather than cells.
    /// Zero is the whole cell: every selection outside a run, and every one a
    /// cell gesture makes, carries zero at both ends and copies as a cell
    /// selection. Only the text-boundary caret drag writes them,
    /// and every function that places an endpoint afresh clears them.
    int startSubColumn = 0;
    int endSubColumn = 0;
};

struct SelectionBounds {
    int row0;
    int column0;
    int row1;
    int column1;
    /// The state's ``SelectionState/isAnchoredByCommand``, carried by the bounds
    /// functions so the per-row void rule can read it: the top edge is a row's
    /// first cell by command, and a blank first row stays in where a
    /// pointer's press on it would drop the row. False for a bounds built by hand.
    bool isAnchoredByCommand = false;
    /// Code points of `column0`'s text on `row0` the selection leaves out, and
    /// of `column1`'s text on `row1` it keeps; zero is the whole cell. See
    /// ``SelectionState/startSubColumn``.
    int subColumn0 = 0;
    int subColumn1 = 0;
};

namespace Selection {
// One row's selected column range after the per-row void rule. column1 < column0
// (selected false) means the row contributes nothing.
struct RowSpan {
    int column0;
    int column1;
    bool selected;
};

// Mirrors the Core/VT/Constants.h osc133Mark* values (redeclared to keep this
// module standalone; a static_assert pins them where both headers share a TU).
// Only the four selection-relevant boundary marks are mirrored:
//   prompt start (OSC 133 ; A), command start (; B), output start (; C),
//   command end (; D). Core's exit-status marks (0x10, 0x20) are not consumed here.
constexpr uint8_t selectionMarkPromptStart = 0x01;
constexpr uint8_t selectionMarkCmdStart = 0x02;
constexpr uint8_t selectionMarkOutputStart = 0x04;
constexpr uint8_t selectionMarkCmdEnd = 0x08;
} // namespace Selection

// OSC 133 prompt-mark bits for an absolute buffer row. The semantic
// routines take this provider so the Selection module stays free of any
// Terminal/Core dependency: the frontend wires it to Terminal::rowMark.
using SelectionRowMarkAt = uint8_t (*)(void* context, int absoluteRow);

using SelectionCellAt = uint32_t (*)(void* context, int row, int column);

// Returns the cell attribute bitfield (ATTR_* from CellContent.h) at (row,column),
// or 0 if no attribute info is available. Used by the wide-cell snapper
// to walk left past Attribute::WideContinuation continuation cells.
using SelectionAttributesAt = uint16_t (*)(void* context, int row, int column);

// Returns a column count for `row` such that every column at or beyond it is
// blank — the model's stored length, where a row ending before the screen edge
// stores nothing past its last cell. A trailing-blank scan may start there
// rather than at the screen's edge.
//
// The contract is one-sided: a count LARGER than the row's content is always
// correct and merely scans further, so returning the column count is the safe
// answer for a caller that cannot do better, and is what a null callback means.
// A count smaller than the last cell holding content would trim that content
// out of the copy.
using SelectionRowExtentAt = int (*)(void* context, int row);

// Returns 1 when row `row` is soft-wrapped to row+1 (right margin, no CR/LF), so
// word-mode bounds extend across the join. Returning 0 (or a NULL callback)
// keeps word walks bounded to the clicked row.
using SelectionRowWrappedCallback = int (*)(void* context, int row);

// Returns row `row`'s DEC line attribute (Core/RowMeta.h), so a span walk can
// read a double-height pair as the one line it shows. A NULL callback reads
// every row as a single-height line.
using SelectionRowLineAttributeAt = uint8_t (*)(void* context, int row);

namespace Selection {

/// How aggressively a double-click expands to a token.
enum class SmartTokenMode : uint8_t {
    Disabled,
    Enabled
};
/// Which selection shape a drag builds: stream, word, line or block.
enum class GestureMode : uint8_t {
    Fresh,
    Extend
};

// True when the selection is in rectangular (column-block) mode, so the
// frontend clips every row to the bounds' [column0,column1] span and copies it
// as a block (newline per row, no reflow).
[[nodiscard]] bool isRectangular(const SelectionState*);

void init(SelectionState*);
void begin(SelectionState*, int rows, int columns, int row, int column, SelectionMode);
void update(SelectionState*, int rows, int columns, int row, int column);
void clear(SelectionState*);
[[nodiscard]] bool hasSelection(const SelectionState*);
[[nodiscard]] bool getBounds(const SelectionState*, int rows, int columns, SelectionCellAt, void* context, SelectionBounds* out);

// Set anchor + head to the word surrounding (row, column): a maximal run of
// word-cells (alphanumeric ASCII or '_'). Result is char-mode. If the click cell
// isn't a word-cell, leaves a single-cell char selection at (row, column).
// When `rowWrapped` is non-null, the walk extends across a soft-wrap join.
void expandToWord(SelectionState*, int rows, int columns, int row, int column, SelectionCellAt, void* context, SelectionRowWrappedCallback rowWrapped = nullptr, void* rowWrappedContext = nullptr);

// Set the selection anchor + head to span the full width of `row`.
// Char-mode result: anchor at column 0, head at column (columns - 1).
void expandToLine(SelectionState*, int rows, int columns, int row);

// Snap (row,column) to a wide cell's leading half: if the cell carries
// Attribute::WideContinuation, walk *column left to the leading Attribute::Wide
// cell (bounded by 0). Coordinates are clamped against (rows,columns) first; a
// NULL attributesAt performs only the clamp. Apply at both anchor and head
// before begin / update so a continuation-cell click selects the wide char.
void snapToCell(int rows, int columns, SelectionAttributesAt, void* context, int* row, int* column);

// Like getBounds, but uses attributesAt to skip Attribute::WideContinuation
// cells. Char-mode: anchor/head snap to the leading half of a wide cell.
// Word-mode: also expands the trailing edge across a wide glyph's continuation.
// NULL attributesAt behaves identically to getBounds. When `rowWrapped` is
// non-null, word-mode bounds extend across a soft-wrap join.
[[nodiscard]] bool getBoundsWide(const SelectionState*, int rows, int columns, SelectionCellAt, void* cellContext, SelectionAttributesAt, void* attributeContext, SelectionBounds* out, SelectionRowWrappedCallback rowWrapped = nullptr, void* rowWrappedContext = nullptr);

// Last column on `row` holding a printed glyph, or -1 if the row is all
// void (blank / space / control). A wide glyph's continuation half counts
// as printed. attributesAt may be null.
[[nodiscard]] int lastPrintedColumn(int row, int columns, SelectionCellAt, void* cellContext, SelectionAttributesAt, void* attributeContext);

// Cells the pointer must travel past a line's last glyph before the trailing
// void snaps to the whole line ("last printed column + 6").
inline constexpr int voidSnapCells = 6;

// CHAR-mode void rule for a bounds rect, in place: a bottom edge in the trailing
// void clamps to the last glyph until voidSnapCells past it, then extends to line
// end; a top edge in the void collapses to the first void column. No-op for rect
// and line modes.
void snapBoundsToVoid(SelectionBounds* b, SelectionMode, int columns, SelectionCellAt, void* cellContext, SelectionAttributesAt, void* attributeContext);

// Selected column range on `row` for bounds `b`, applying the per-row void rule
// (the single source of that rule). Rows outside [row0,row1] yield {0,0,false}
// without touching the callbacks; interior rows resolve in O(1). Rect/line spans
// pass through whole; word/char edge rows snap the void per voidSnapCells. A
// blank top row drops out of a multi-row span unless the bounds are
// isAnchoredByCommand: a command named the row's start.
[[nodiscard]] RowSpan rowSpan(const SelectionBounds* b, SelectionMode, int row, int columns, SelectionCellAt, void* cellContext, SelectionAttributesAt, void* attributeContext);

// Caret column the empty-line drag collapses to: an empty line (lastPrinted
// < 0) has a single selectable slot at column 0, so its caret pins there;
// any content line keeps the caret as given.
[[nodiscard]] int caretSlot(int caret, int lastPrinted);

// Linear (reflowed) char drag from an anchor caret to a head caret. Carets
// are cell BOUNDARIES (0..columns): the drag selects the half-open slot range
// [lo,hi) as inclusive cells [lo,hi-1]. An empty range clears the
// selection; otherwise it (re)opens a char-mode selection spanning it.
void charDragLinear(SelectionState*, int rows, int columns, int anchorRow, int anchorCaret, int headRow, int headCaret);

/// A character drag between carets that may stand inside packed proportional
/// cells.
///
/// A caret here is a cell boundary plus how many of the next cell's code
/// points stand to its left. Carets order by row, then boundary, then that
/// offset; the range between them selects from the start caret's cell through
/// the end caret's cell when the end stands inside one, or through the cell
/// before its boundary when it does not, and the state's sub-columns leave out
/// the start cell's letters before the start and the end cell's letters after
/// the end. With both offsets zero this is the boundary form exactly. An empty
/// range clears the selection.
///
/// - Parameters:
///   - selection: The selection to set.
///   - rows: The rows the selection may name.
///   - columns: The grid width.
///   - anchorRow: The anchor caret's row.
///   - anchorCaret: Its boundary, 0 through `columns`.
///   - anchorSubColumn: Code points into the cell right of that boundary.
///   - headRow: The head caret's row.
///   - headCaret: Its boundary.
///   - headSubColumn: Code points into the cell right of it.
void charDragLinear(SelectionState* selection, int rows, int columns, int anchorRow, int anchorCaret, int anchorSubColumn,
    int headRow, int headCaret, int headSubColumn);

// Per-engine word-character classifier. The bitmap is plain bytes so the struct
// is trivially copyable and safe to embed by value. Callers without their own
// use defaultEngine().
struct Engine {
    uint8_t wordBitmap[16] {}; // 128 bits, one per 7-bit ASCII code
    bool wordBitmapInited = false; // false → the default set answers, without writing here
};

void initEngine(Engine*);
/// Sets the extra characters double-click treats as part of a word.
///
/// - Parameters:
///   - engine: The selection engine to configure.
///   - characters: A NUL-terminated set, beyond letters and digits.
void engineSetWordCharacters(Engine*, const char*);
[[nodiscard]] bool engineIsWordByte(const Engine*, uint32_t ch);

// Process-wide default engine backing setWordCharacters() and the engine-less forms.
// Never null.
[[nodiscard]] Engine* defaultEngine();

// Override the default engine's word-character set: a NUL-terminated string of
// literal chars, each set in the bitmap. Null/empty resets to "[A-Za-z0-9_]".
// Non-ASCII (>= 128) is always non-word.
/// Sets the extra characters double-click treats as part of a word.
///
/// - Parameter characters: A NUL-terminated set, beyond letters and digits.
void setWordCharacters(const char*);

// Engine-aware variants of expandToWord / getBoundsWide. `engine` is nullable
// (null → defaultEngine()); otherwise identical to the engine-less forms.
void expandToWordWith(SelectionState*, int rows, int columns, int row, int column, SelectionCellAt, void* context, Engine*, SelectionRowWrappedCallback rowWrapped = nullptr, void* rowWrappedContext = nullptr);

[[nodiscard]] bool getBoundsWideWith(
    const SelectionState*, int rows, int columns, SelectionCellAt, void* cellContext, SelectionAttributesAt, void* attributeContext, SelectionBounds* out, Engine*, SelectionRowWrappedCallback rowWrapped = nullptr, void* rowWrappedContext = nullptr);

// Semantic (OSC 133) selection: the absolute-row span of the LAST command's
// output — from the most recent ; C (output start) at or before
// `maxAbsoluteRow` down to the matching ; D (command end), or the last buffer
// row when still running. `markAt` returns mark bits for an absolute row (0 out
// of range / alt-screen); `maxAbsoluteRow` is the inclusive search top (pass
// totalRows-1 for the whole buffer). On success fills `out` with a char-mode
// whole-row region and returns true; false (out untouched) when no output region
// is found. The ; C row is the first output row; the ; D row is EXCLUDED.
[[nodiscard]] bool lastCommandOutputRegion(SelectionRowMarkAt, void* context, int totalRows, int columns, int maxAbsoluteRow, SelectionBounds* out);

// Expand a click to the surrounding "smart token": a maximal run of URL / path /
// hash characters — alphanumerics plus the URL/path punctuation set
// ( : / ? # [ ] @ ! $ & ' ( ) * + , ; = . _ ~ - % \ ). Sets a char-mode span
// [lo, hi] on `row` and returns true when the clicked cell is a smart-token
// char; a no-op returning false otherwise (caller falls back to expandToWord).
// Stays on a single row.
bool expandToSmartToken(SelectionState*, int rows, int columns, int row, int column, SelectionCellAt, void* context);

// True when `ch` belongs to the smart-token character class above.
[[nodiscard]] bool isSmartTokenByte(uint16_t ch);

// Double-click expansion. With `smartTokens` and a smart-token byte under the
// click, selects the whole token (char mode); otherwise expands to a word and
// sets WORD mode so a subsequent drag re-snaps endpoints to whole words.
void doubleClickExpand(SelectionState*, int rows, int columns, int row, int column, SelectionCellAt, void* context, Engine*, SmartTokenMode smartTokens);

// Set a CHAR-mode selection to the inclusive span [column0..column1] on `row`
// (used to materialise a span a smart-selection regex resolved). Clamps to the
// grid; a degenerate (column1 < column0) span collapses to a single cell at column0.
void setCharacterSpan(SelectionState*, int rows, int columns, int row, int column0, int column1);

/// Selects the cells a command names: `region` as given, in character mode.
///
/// The endpoints are placed, not pressed, so the selection is marked
/// ``SelectionState/isAnchoredByCommand`` and a blank first row stays in the
/// highlight where a pointer's press on it would start the drag below. Select
/// All and Select Last Command Output are regions, and every arm routes
/// through here so none carries a begin-and-update pair of its own.
///
/// - Parameters:
///   - selection: The selection to set.
///   - rows: History plus screen rows.
///   - columns: The grid width.
///   - region: The rows and columns to select, clamped to the grid.
void selectRegion(SelectionState* selection, int rows, int columns, const SelectionBounds& region);

/// Selects the whole buffer: the oldest scrollback row through the last cell of
/// the live screen.
///
/// Here rather than in each frontend because "does Select All reach into the
/// scrollback" is one answer, and a copy per frontend is a chance each to give a
/// different one.
///
/// The ``selectRegion`` over every row: a character selection, so a
/// Shift-click afterwards trims it to the cell clicked, with a blank first
/// row kept in the highlight.
///
/// - Parameters:
///   - selection: The selection to set.
///   - totalRows: History plus screen rows; the screen alone on the alternate
///     screen, which keeps no history.
///   - columns: The grid width.
void selectAll(SelectionState* selection, int totalRows, int columns);

/// Drops a selection because the user is sending input to the process.
///
/// A highlight left standing over content the user has begun editing is stale,
/// so the keystroke, paste and keypad paths all pass through here.
///
/// - Parameter selection: The selection to drop; null is accepted and does
///   nothing.
/// - Returns: Whether anything changed, which is the frontend's cue to repaint.
///   The no-selection case is the common one and costs nothing.
bool clearForUserInput(SelectionState* selection);

// Mouse-gesture policy. A plain single click clears any selection and only opens
// one on the first drag motion (a bare click leaves no highlight); double click
// selects the word; triple click the line; shift-click extends. The mutation is
// delegated through GestureOperations. A frontend keeps one Gesture across a
// press→drag→release sequence and calls gestureBegin / gestureDrag / gestureEnd;
// each returns true when the selection changed.
struct GestureOperations {
    void (*beginCharacter)(void* ud, int row, int column);
    void (*beginWord)(void* ud, int row, int column);
    void (*beginLine)(void* ud, int row, int column);
    void (*update)(void* ud, int row, int column);
    void (*clear)(void* ud);
    bool (*hasSelection)(void* ud);
    void* ud;
};

struct Gesture {
    int anchorRow {}; // press cell — where a deferred drag begins
    int anchorColumn {};
    int targetRow {}; // latest pointer cell, retained while the gesture is open
    int targetColumn {};
    /// The press's normalized click count, retained while the selection moves.
    int clickCount = 1;
    bool dragBegan {}; // a drag selection has opened this gesture
    bool isOpen {};
};

// Reset a Gesture to the idle state.
void gestureReset(Gesture* g);

// Mouse-down at (row,column). clickCount is 1/2/3 (values >3 treated as 1).
// gesture==Extend with an existing selection extends rather than starting fresh.
bool gestureBegin(Gesture* g, const GestureOperations*, int row, int column, int clickCount, GestureMode gesture);

// Drag to (row,column). Opens the deferred single-click selection at the
// anchor on first motion, then moves the head.
bool gestureDrag(Gesture* g, const GestureOperations*, int row, int column);

// Mouse-up. Closes the gesture without changing its selection; returns whether
// a selection survives (for copy-on-select).
bool gestureEnd(Gesture* g, const GestureOperations*);

/// A selection's span in the projection.
///
/// Selection rows are absolute buffer rows; they map to viewport rows by the
/// visible-cell arithmetic and then to character indices through the row
/// ranges, endpoints clamping to their trimmed lines. A selection wholly
/// outside the viewport answers absent; one crossing an edge clamps to the
/// visible part.
///
/// - Parameters:
///   - projection: The projection to index into.
///   - selection: The selection, anchor and head in either order.
///   - historyRows: Scrollback rows retained, 0 on the alternate screen.
///   - viewOffset: Rows scrolled back from the live tail.
/// - Returns: The visible span, or absent.
[[nodiscard]] Ubiquitty::Accessibility::ProjectionRange selectionInProjection(const Ubiquitty::Accessibility::TextProjection& projection, const SelectionState& selection, std::ptrdiff_t historyRows, std::ptrdiff_t viewOffset);

} // namespace Selection

} // namespace Ubiquitty
