// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/CellColor.h"
#include "Core/Input/PointerInput.h"
#include "Core/LineStore.h"
#include "Core/RichText/Drawing.h"
#include "Core/Screen/HistoryFacts.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/SelectionSpans.h"
#include "Core/Selection/StyledClipboard.h"
#include "Core/Terminal.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

// The selection machinery bound to a Terminal, so a frontend holds one Session
// and calls in viewport coordinates. Everything the primitive layer delegates
// through callbacks — which grid a row reads (alternate versus absolute), the
// visible-to-absolute row conversion, the word engine, the characterClass
// override, the copy walk — is answered here, once. Before this facade every
// frontend re-implemented that plumbing as its own shim set, and the macOS
// view alone carried three copies in three files; a wrapper written against
// this header carries none.
//
// Layering: Selection/ is a behaviour module and Terminal is a model type, and
// a behaviour module may depend on a model one — the primitive layer's
// callback style exists so that POLICY stays testable without a Terminal, not
// to keep this binding out of Core. The primitives stay as they are; this file
// adds the binding beside them. References at this boundary, because every
// argument is required; the callback internals keep the pointers their C
// shapes demand.

namespace Ubiquitty::Selection {

/// One view's selection over one terminal.
///
/// Owned by the frontend view (a selection is per view, not per terminal — two
/// views of one session select independently) and handed to every call here
/// with the Terminal it binds to. Plain data so a frontend can hold it by
/// value or behind a stable pointer as its language requires.
/// How a gesture interprets its press, beyond what the click count decides.
struct GestureStyle {
    /// Column-block selection rather than the flowing kind.
    bool isRectangular = false;
    /// Whether a double click grows to a URL/path/hash token instead of a word.
    SmartTokenMode smartToken = SmartTokenMode::Disabled;
    /// Whether the press extends the existing selection rather than opening a
    /// fresh one — the shift-click behaviour.
    GestureMode mode = GestureMode::Fresh;
    /// Whether the press is the one that brought an inactive window forward.
    /// A frontend fact -- AppKit asks `acceptsFirstMouse:` for exactly that
    /// press -- which the gesture declines so the standing selection survives.
    /// Only the click after it opens a fresh one.
    bool isWindowActivating = false;
};

/// Where a selection's ends and an open gesture's rows stood when they were
/// last named or resolved.
///
/// An operation that moves an end leaves the session's rows apart from these,
/// which is how the next synchronization knows to name the new ends rather
/// than resolve the old ones.
struct NamedEnds {
    bool isNamed = false;
    bool hasSelection = false;
    bool hasGesture = false;
    SelectionMode mode = SelectionMode::Character;
    int anchorRow = 0;
    int anchorColumn = 0;
    int headRow = 0;
    int headColumn = 0;
    int gestureAnchorRow = 0;
    int gestureTargetRow = 0;
    /// The terminal's `absoluteRowEpoch` at the naming: a rectangle's columns
    /// mean nothing across the resize that bumps it.
    std::uint32_t absoluteRowEpoch = 0;
};

/// One view's selection over one terminal.
///
/// Owned by the frontend view (a selection is per view, not per terminal — two
/// views of one session select independently) and handed to every call here
/// with the Terminal it binds to. Plain data so a frontend can hold it by
/// value or behind a stable pointer as its language requires.
struct Session {
    SelectionState state {};
    Gesture gesture {};
    Engine engine {};
    /// The open gesture's style, kept here because the sequencer defers a
    /// single click's open to the first drag motion: the drag is where a
    /// rectangular or smart-token press takes effect, and a style read from
    /// the drag call alone would always be the default.
    GestureStyle gestureStyle;
    /// The press row in the terminal's absolute row space. A deferred
    /// character drag must keep naming that character while output moves the
    /// viewport beneath the pointer.
    int gestureAnchorAbsoluteRow = 0;
    /// The buffer row under the latest pointer fact. Output refreshes the
    /// gesture only when the same viewport cell comes to name another row.
    int gestureTargetAbsoluteRow = 0;
    /// The unrounded pointer location for resolving the word beneath a
    /// stationary pointer after output moves its viewport row.
    double gesturePointerXInCells = std::numeric_limits<double>::quiet_NaN();
    double gesturePointerYInRows = std::numeric_limits<double>::quiet_NaN();
    /// Half-open caret coordinates for character drags and words inside
    /// packed text. Cell gestures use `GestureOperations` instead.
    int gestureAnchorCaret = 0;
    int gestureAnchorSlot = 0;
    int gestureTargetCaret = 0;
    /// How many code points of the cell right of each caret stand to its
    /// left. Nonzero only inside a proportional run, where one cell holds
    /// several letters' ink and a boundary alone cannot name a place between
    /// two of them.
    int gestureAnchorSubCaret = 0;
    int gestureTargetSubCaret = 0;
    /// The boundary after the pressed character or word. A drag that runs
    /// back past the press selects from this edge of the pressed text.
    int gestureAnchorCaretAfter = 0;
    int gestureAnchorSubCaretAfter = 0;
    bool usesCharacterCarets = false;
    /// The retained history facts at the last Core selection operation. Their
    /// deltas distinguish appended rows from front eviction.
    std::int64_t observedScrollbackAppended = 0;
    int observedScrollbackRows = 0;
    std::uint32_t observedBufferEpoch = 0;
    bool wasUsingAlternateScreen = false;
    bool hasBufferObservation = false;
    /// The screen's rows as a digest at the last observation, taken only while
    /// a selection or an open gesture stands. Output that appends rows and
    /// leaves this digest unchanged repainted the screen it scrolled, and the
    /// selection follows its text down the buffer rather than up the screen.
    std::uint64_t observedScreenDigest = 0;
    bool hasScreenObservation = false;
    /// The selection's ends and the open gesture's rows as logical positions,
    /// which keep naming their characters while output, eviction, in-place
    /// moves and rewraps renumber the rows; the rows above are these resolved.
    /// A gesture's rows are named at their first cell. On the alternate screen
    /// they name screen rows instead (``Screen::captureAlternateRow``), which
    /// the program's erases and rewrites leave in place.
    Screen::LogicalPosition anchorPosition {};
    Screen::LogicalPosition headPosition {};
    Screen::LogicalPosition gestureAnchorPosition {};
    Screen::LogicalPosition gestureTargetPosition {};
    NamedEnds namedEnds {};
    /// The history facts the last output transition was judged by. The rows
    /// appended since are what a held viewport moves by. Kept apart from the
    /// gesture's observation above, which a drag between two drains refreshes
    /// and would then report the drain's growth as zero.
    Screen::HistoryFacts viewportObservedHistory;
    bool hasViewportObservation = false;
    /// The top visible logical cell, retained through local resizing and
    /// remote history reconstruction. Remote views also resolve it on output;
    /// local output follows the ordinary viewport observation.
    std::optional<Screen::LogicalPosition> viewportAnchor;
    std::uint32_t viewportHistoryEpoch = 0;
    bool usesLogicalViewport = false;
};

/// The selection and viewport result of one output transition.
struct OutputUpdate {
    /// The viewport decision shared by every frontend.
    Input::ViewportAfterOutput viewport;
    /// Whether an open gesture moved because content scrolled beneath its
    /// retained pointer target.
    bool didChangeSelection = false;
};

/// Prepares a session for its first gesture.
///
/// - Parameter session: The session to initialise.
void init(Session& session) noexcept;

/// Captures a remote view's explicit scroll position under the model lock.
/// A zero offset follows the tail. Subsequent output resolves this logical
/// position even if cached rows are replaced, backfilled or rewrapped.
/// - Parameters:
///   - session: The view-owned selection session.
///   - terminal: The model whose history is currently presented.
///   - viewOffset: Rows above the tail after the user's viewport movement.
void observeViewport(Session& session, const Terminal& terminal, int viewOffset) noexcept;

/// Captures the top visible logical position before a local resize.
/// An unchanged view retains its original cell across successive rewraps.
/// - Parameters:
///   - session: The view-owned anchor and history observation.
///   - terminal: The model before its grid moves; the caller holds its lock.
///   - viewOffset: Rows above the tail; zero follows the tail.
void noteViewportBeforeResize(Session& session, const Terminal& terminal, int viewOffset) noexcept;

/// Resolves the captured viewport in the resized model.
/// A missing or evicted anchor clamps to the oldest retained row.
/// - Parameters:
///   - session: The view-owned anchor and history observation.
///   - terminal: The model after resizing; the caller holds its lock.
///   - viewOffset: The offset before resizing; zero stays at the tail.
/// - Returns: The offset showing the anchored cell, clamped to visible history.
[[nodiscard]] int viewportAfterResize(Session& session, const Terminal& terminal, int viewOffset) noexcept;

/// Opens a mouse gesture at a viewport cell.
///
/// A plain single click clears any selection and opens one only on the first
/// drag motion; a double click selects the word (through the characterClass
/// tables when the terminal configured them); a triple click the line.
/// Coordinates are viewport rows — 0 at the top of what the view shows — and
/// the conversion into the buffer's absolute rows happens here, including the
/// alternate screen's no-history rule.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - visibleRow: The pressed cell's viewport row.
///   - column: The pressed cell's column.
///   - clickCount: The press's click count, which picks character, word or
///     line granularity.
///   - viewOffset: How many rows the view is scrolled back.
///   - style: The press's modifiers, resolved by the frontend.
///   - xInCells: Where the press is along the row, in cells from the grid's
///     left edge, unrounded; a word press inside a proportional run grows
///     from the letter under it. Non-finite keeps the press on its cell.
///   - yInRows: Where the press is down the viewport, in rows, unrounded; a
///     word press inside a rich text block finds its line by it. Non-finite
///     takes the middle of `visibleRow`.
/// - Returns: Whether the selection changed, which is the cue to repaint.
bool beginGesture(Session& session, Terminal& terminal, int visibleRow, int column, int clickCount, int viewOffset, const GestureStyle& style,
    double xInCells = std::numeric_limits<double>::quiet_NaN(), double yInRows = std::numeric_limits<double>::quiet_NaN()) noexcept;

/// Extends the open gesture to a viewport cell.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - visibleRow: The dragged-to viewport row.
///   - column: The dragged-to column.
///   - viewOffset: How many rows the view is scrolled back.
///   - xInCells: The unrounded horizontal pointer position for a word drag.
///   - yInRows: The unrounded vertical pointer position inside a rich text block.
/// - Returns: Whether the selection changed.
bool dragGesture(Session& session, Terminal& terminal, int visibleRow, int column, int viewOffset,
    double xInCells = std::numeric_limits<double>::quiet_NaN(), double yInRows = std::numeric_limits<double>::quiet_NaN()) noexcept;

struct CaretResolution;

/// Gives an open gesture its anchor's character boundaries.
///
/// The frontend measures where the pointer is; Core owns what that point means
/// on this terminal, when the press becomes a selection, and how its half-open
/// bounds move. A caret here is a cell boundary and, inside a proportional
/// run, how far past that boundary it stands in the cell's letters — which
/// ``resolveSelectionCaret`` answers and a caller passes through.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows.
///   - press: The character under the press, or the selected word's bounds.
void configureCharacterCaretGesture(Session& session, const Terminal& terminal, const CaretResolution& press) noexcept;

/// Extends a text-boundary character gesture to a caret.
///
/// Between boundaries nothing opens until the pointer leaves the boundary it
/// went down on: the same row and the same threshold is still the press
/// rather than a drag. On a letter of a proportional run the gesture is by
/// letters: the first motion selects the pressed letter, and the selection
/// runs from the far edge of the pressed letter to the far edge of the letter
/// under the pointer, whichever way the pointer went.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows.
///   - head: The caret under the pointer, from ``resolveSelectionCaret``.
///   - viewOffset: Rows the viewport is scrolled back; the pointer's visible
///     row is retained across output.
/// - Returns: Whether the selection changed.
bool dragCharacterCaretGesture(Session& session, Terminal& terminal, const CaretResolution& head, int viewOffset) noexcept;

/// Closes the open mouse gesture without dropping its selection.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether a selection survives, for copy-on-select policy.
bool endGesture(Session& session) noexcept;

/// Selects as a plain drag between two points would, through the caret
/// gesture the pointer ladder runs: for a snapshot or a test that has no
/// pointer to move.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows.
///   - fromX: Where the press is across the viewport, in cells, unrounded.
///   - fromY: Where it is down the viewport, in rows, unrounded.
///   - toX: Where the release is across, in cells.
///   - toY: Where it is down, in rows.
void selectBetweenPoints(Session& session, Terminal& terminal, double fromX, double fromY, double toX, double toY) noexcept;

/// Whether the session has a mouse gesture in progress.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether a press is awaiting motion or release.
[[nodiscard]] bool isGestureOpen(const Session& session) noexcept;

/// Whether motion has opened a selection for the current gesture.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether the gesture has crossed its drag threshold.
[[nodiscard]] bool hasGestureDragBegun(const Session& session) noexcept;

/// Whether an output transition is worth `updateAfterOutput`'s full
/// evaluation, or answers from the viewport policy alone.
///
/// `updateAfterOutput` starts by rebasing the session against the terminal's
/// retained-history facts even when nothing is selected and no gesture is
/// open, which is real work a caller with neither can skip outright — every
/// frontend's per-drain output path pays this question, so it is answered
/// once, here, rather than each arm writing its own copy of `hasSelection ||
/// isGestureOpen` (or its negation) inline.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether the session has a selection to keep or a gesture whose
///   retained target could move.
[[nodiscard]] bool needsOutputEvaluation(const Session& session) noexcept;

/// The viewport after output, judged from published history facts.
///
/// Takes no terminal, so a drain with nothing selected runs it off the
/// snapshot the drain published without the model lock. The session keeps
/// the facts it last judged: the rows appended since are what a held view
/// moves by, and a changed epoch (a resize, a clear) starts the count over
/// rather than reading a rebuilt history as growth. The first transition
/// establishes the baseline and moves nothing.
///
/// - Parameters:
///   - session: The view's selection session.
///   - viewOffset: Rows above the tail before the output.
///   - snapPolicy: The scroll-on-output policy, if available.
///   - history: The retained history as the output left it.
/// - Returns: The offset to present, and the verdict behind it.
[[nodiscard]] Input::ViewportAfterOutput viewportAfterOutput(Session& session, int viewOffset, Input::OutputSnapPolicy snapPolicy, const Screen::HistoryFacts& history) noexcept;

/// Applies one output transition to a view's selection and viewport.
///
/// A tail view follows output. While a gesture remains open, its press stays
/// attached to the original buffer character and its head is re-evaluated at
/// the latest pointer cell using the gesture's character, word, wrapped-line,
/// or rectangular rules. A scrolled-back view continues to hold its rows,
/// moving by the rows the output appended as `viewportAfterOutput` judges
/// them from the terminal's own facts. With nothing selected and no gesture
/// open this costs what `viewportAfterOutput` costs.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal after output was applied. Read under the model
///     lock.
///   - viewOffset: Rows above the tail before the output.
///   - snapPolicy: The scroll-on-output policy, if available.
/// - Returns: The viewport decision and whether the selection changed.
[[nodiscard]] OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, int viewOffset, Input::OutputSnapPolicy snapPolicy) noexcept;

/// Applies one output transition, reading the scroll-on-output mode itself.
///
/// For an owner whose terminal no other thread writes -- the browser page,
/// a test -- so the mode needs no lock to read. The mode is consulted only
/// on the drains where it decides; a view at the tail or a standing
/// selection answers without it.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal after output was applied.
///   - viewOffset: Rows above the tail before the output.
/// - Returns: The viewport decision and whether the selection changed.
[[nodiscard]] OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, int viewOffset) noexcept;

/// Applies one output transition, taking the model lock only as far as the
/// transition needs it.
///
/// The per-drain path of every threaded frontend: with nothing selected and
/// no gesture open the verdict comes from the published `history` without
/// the lock, the mode read under a try-lock so a drain that finds the lock
/// busy holds rather than stalls (Indeterminate holds, the non-disruptive
/// answer for a reader part-way up their history). A selection or an open
/// gesture takes the lock and judges the terminal itself.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal after output was applied, dereferenced only
///     under `modelLock`.
///   - modelLock: The lock the terminal's writer takes.
///   - viewOffset: Rows above the tail before the output.
///   - history: The history facts the drain published.
/// - Returns: The viewport decision and whether the selection changed.
[[nodiscard]] OutputUpdate updateAfterOutput(Session& session, Terminal& terminal, std::mutex& modelLock, int viewOffset, const Screen::HistoryFacts& history) noexcept;

/// Whether the session holds a selection.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether anything is selected.
[[nodiscard]] bool hasSelection(const Session& session) noexcept;

/// Whether a selection is live in `terminal`, dropping one the buffer has
/// renumbered.
///
/// The overload above answers about the state alone, which is what a caller
/// with no terminal in hand can ask. This one is the answer a frontend wants
/// before offering a copy.
[[nodiscard]] bool hasSelection(Session& session, const Terminal& terminal) noexcept;

/// Drops the selection and closes any open gesture.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether a selection was dropped.
bool clearSelection(Session& session) noexcept;

/// Drops the selection because a press went to the process as a report.
///
/// A reported press is input like a keystroke and drops the selection the
/// same way -- except the press that brought the window forward, which the
/// report still describes to the process while a standing selection survives
/// it, as through `beginGesture`.
///
/// - Parameters:
///   - session: The view's selection session.
///   - style: The press's facts, read for `isWindowActivating`.
/// - Returns: Whether a selection was dropped.
bool clearSelectionForReportedPress(Session& session, const GestureStyle& style) noexcept;

/// Drops the selection because the user is sending input to the process.
///
/// A highlight left standing over content the user has begun editing is
/// stale, so the keystroke, paste and keypad paths all pass through here.
/// Also resets the open gesture, so a drag interrupted by typing does not
/// resume from a dropped anchor.
///
/// - Parameter session: The view's selection session.
/// - Returns: Whether anything changed — the cue to repaint.
bool clearSelectionForUserInput(Session& session) noexcept;

/// Selects the whole buffer — the oldest scrollback row through the live
/// screen's last cell, or the screen alone on the alternate screen.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows.
void selectAllText(Session& session, const Terminal& terminal) noexcept;

/// The selected text, resolved the way the highlight is.
///
/// Wide-aware bounds resolution, the cluster walk and the soft-wrap newline
/// rule are the shared policies; an empty string means no selection. The
/// session is mutable because the word engine populates its table on first
/// use.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
/// - Returns: The selection as UTF-8 text, rows joined by `\n` except across a
///   soft wrap.
[[nodiscard]] std::string selectedText(Session& session, Terminal& terminal);

/// Copies selected text in the direction selected by DECRLCM.
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The model, read under its lock.
/// - Returns: UTF-8 clipboard text with complete grapheme clusters.
[[nodiscard]] std::string copiedText(Session& session, Terminal& terminal);

/// Resolves the selection in absolute buffer rows, including wide characters.
/// - Parameters:
///   - session: The view’s selection.
///   - terminal: The model, read under its lock.
/// - Returns: Inclusive cell bounds, or no value when the selection is empty.
[[nodiscard]] std::optional<SelectionBounds> selectedBounds(Session& session, Terminal& terminal) noexcept;

/// The selection as the styled clipboard flavours, resolved the way the
/// highlight is.
///
/// The plain flavour is byte-for-byte what ``copiedText`` answers; the RTF
/// and HTML carry each run's resolved colours and attributes. Bound here
/// rather than in a frontend because resolving a cell to its final colours is
/// one rule: the macOS window controller kept a second styled-cell callback
/// beside this file's, and a shell that has no such callback, the Swift one,
/// could not copy with styling at all.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - colorMode: Which colours survive into the rich flavours.
///   - palette: The palette each cell resolves through.
///   - font: The face the rich flavours name.
///   - flavors: Which rich formats to produce alongside plain text.
/// - Returns: The three flavours; all three empty when nothing is selected.
[[nodiscard]] StyledClipboard styledClipboard(Session& session, Terminal& terminal, StyleColorMode colorMode, const CellPalette& palette, const StyledFont& font, StyledFlavors flavors = StyledFlavors::All);

/// Drops a selection of screen cells when a resize renumbered the buffer under
/// it: a rectangle, or any selection on the alternate screen.
///
/// A flowing selection's ends on the primary screen are logical positions,
/// which a rewrap carries, so the next facade call puts it back on its
/// characters. A rectangle names a column range on every row it spans, and a
/// rewrap gives those columns no meaning. A selection on the alternate screen
/// names screen rows, and the program lays its frame out again for the new
/// size. Both are dropped; the facade does the same at its next call, and this
/// lets a frontend drop them with the resize, before its next frame.
///
/// - Parameters:
///   - state: The selection to judge, cleared when it names screen cells and
///     the buffer moved.
///   - epochBefore: ``Terminal::absoluteRowEpoch`` read before the resize. A
///     resize that changes nothing does not bump it, so a repeated resize to
///     the same dimensions keeps even a rectangle.
///   - terminal: The terminal, now resized.
/// - Returns: Whether the selection was dropped.
bool dropSelectionAcrossResize(SelectionState& state, std::uint32_t epochBefore, const Terminal& terminal);

/// Where a selection begins and ends on the viewport.
///
/// The ends of the selection itself: the start is the first selected cell of
/// the topmost selected visible row, the end the last selected cell of the
/// bottom one. They are not the extent of the highlight, which a sized block
/// or a rich text block widens; a handle on the selection belongs here.
struct ViewportEnds {
    /// Whether any row of the selection is on the viewport; the fields below
    /// mean nothing otherwise.
    bool isVisible = false;
    /// The topmost selected visible row.
    int startRow = 0;
    /// The first selected column on that row.
    int startColumn = 0;
    /// The bottom selected visible row.
    int endRow = 0;
    /// The last selected column on that row, inclusive.
    int endColumn = 0;
};

/// Collects the highlight's per-row spans for one frame.
///
/// Bounds are resolved wide-aware against the live grid, then intersected with
/// the viewport; the output feeds `TerminalSnapshotOptions::selectionSpans`
/// unchanged. A caller that wants the selection's own ends, for a handle,
/// asks ``viewportSpansAndEnds``: the spans are a highlight and are not
/// the ends.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - viewOffset: How many rows the view is scrolled back.
///   - out: Receives the spans, sorted by row where sized blocks add to them:
///     the selection's own, at most one per selected visible row; a second for
///     a row a rich text block cuts in two, so its covered cells are never
///     highlighted as cells; and a piece for each run of a sized block's cells
///     that the selection touches the block through and does not cover, so a
///     block the selection reaches is lit on all its band rows and claimed
///     columns. Contiguous pieces on a row are one span. Pieces go in while
///     `out` has room, and never overlap the spans they join; a buffer of
///     ``viewportSpanCapacity`` spans always has room.
/// - Returns: How many spans were written.
[[nodiscard]] int viewportSpans(Session& session, Terminal& terminal, int viewOffset, ViewportSpans out) noexcept;

/// ``viewportSpans`` and the selection's own ends on the viewport.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - viewOffset: How many rows the view is scrolled back.
///   - out: Receives the spans, as ``viewportSpans`` writes them.
///   - ends: Receives where the selection begins and ends, from the selection's
///     own row spans: not a block piece, and not the cell a rich text block cut
///     out of one.
/// - Returns: How many spans were written.
[[nodiscard]] int viewportSpansAndEnds(Session& session, Terminal& terminal, int viewOffset, ViewportSpans out, ViewportEnds& ends) noexcept;

/// How many spans a frontend must make room for so ``viewportSpans`` drops none.
///
/// A row's spans and pieces cover disjoint cells, pieces that touch are one
/// span, and a sized block is at least two columns wide, so a row holds about
/// half as many spans as it has columns; the allowance is two over that. Zero
/// for a grid with no rows or no columns.
///
/// - Parameters:
///   - rows: The viewport's rows.
///   - columns: The viewport's columns.
/// - Returns: The span count to allocate.
[[nodiscard]] int viewportSpanCapacity(int rows, int columns) noexcept;

/// Where the selection's characters fall inside rich text blocks.
using BlockSelections = RichText::BlockSelections;

/// Collects the characters the selection covers in each block of the active
/// screen, for the per-line highlights the capture draws under the glyphs
/// (Docs/rich-text.md, "Selection and copy").
///
/// The cut is the one a copy takes, read at the block's top-left cell, so
/// what is highlighted and what is copied cannot differ.
///
/// - Parameters:
///   - session: The view's selection session.
///   - terminal: The terminal the view shows. Read under the model lock.
///   - out: Receives one entry per block the selection reaches.
/// - Returns: How many entries were written.
[[nodiscard]] int blockSelections(Session& session, Terminal& terminal, BlockSelections out) noexcept;

/// A character-mode selection over a rectangle given in visible rows.
///
/// The two row spaces are the whole of this function. A selection is anchored
/// in absolute buffer rows so that it survives scrolling, and a caller holding
/// a rectangle it read off the screen holds visible rows; the difference is the
/// scrollback ahead of the viewport. A caller that skips the conversion selects
/// the right rows only while the buffer has no history, which is the state a
/// freshly fed terminal is in and no long-lived one stays in.
///
/// - Parameters:
///   - terminal: The terminal whose scrollback places the viewport.
///   - visible: The rectangle, in visible rows and columns.
/// - Returns: An active character-mode selection over the absolute rows.
[[nodiscard]] SelectionState characterSelectionOverVisibleRows(const Terminal& terminal, const SelectionBounds& visible) noexcept;

/// Whether the reader confined selection to printed text.
enum class TextRestriction : std::uint8_t {
    /// `[selection] restrict_to_text` is off: every cell is selectable.
    Unrestricted,
    /// It is on: a row's trailing void holds nothing to select.
    ToText,
};

/// Whether a visible cell sits past the end of its row's text.
///
/// The document-style pointer's question. Three exemptions, each a case
/// where "nothing to select here" would be the wrong thing to say: the
/// setting off, an application tracking the pointer — it owns the cell,
/// printed or not — and the alternate screen, where a full-screen program
/// fills the grid and a blank cell is its blank cell.
///
/// The tracking exemption is also ranked by `Input::pointerShape`, above this
/// answer, so a shape asked through the ladder reaches the same conclusion
/// twice. It is stated here as well because the question this answers —
/// whether there is anything to select at a cell — has to be right on its own
/// terms for a caller that is not asking about a pointer image.
///
/// Pointer SHAPE only: what a drag from here selects is unchanged, because a
/// reader who drags across a short line still means the rows they crossed.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal to read.
///   - viewOffset: Rows the viewport is scrolled back.
///   - visibleRow: The pointer's row, zero-based in the viewport.
///   - column: The pointer's column.
///   - restriction: What the reader configured.
/// - Returns: Whether the cell is in the row's void.
[[nodiscard]] bool isCellInTextVoid(
    const Terminal& terminal, int viewOffset, int visibleRow, int column, TextRestriction restriction) noexcept;

/// A caret resolved against a terminal: where it is, and its open threshold.
struct CaretResolution {
    /// The absolute buffer row, scrollback included. A caret drag orders its
    /// two ends linearly, and a drag that walks into history crosses rows
    /// whose visible index has moved under it.
    int absoluteRow = 0;
    /// The cell boundary the caret stands on.
    int caret = 0;
    /// The threshold position the gesture opens against, from ``caretSlot``.
    int slot = 0;
    /// How many code points of the cell right of `caret` stand to the caret's
    /// left.
    ///
    /// The one field that says more than a cell. A proportional run lays
    /// several letters' ink inside one cell, so a boundary alone cannot name
    /// the place between two of them; this counts into that cell's share of
    /// the run's text, in the order a copy reads it. Zero everywhere else,
    /// which is every caret outside a run and every caret standing on a
    /// boundary — so a caller that ignores the field is exact everywhere but
    /// between two letters of a run. It reaches the selection model and stops
    /// there: a mouse report names the cell under the pointer, because an
    /// application's mouse protocol is in cells and a reader's caret is not
    /// the program's cursor.
    int subCaret = 0;
    /// The boundary after the letter the point is on, where the point is on a
    /// letter of a proportional run: `caret` and `subCaret` are the boundary
    /// before it. A drag through a run selects by the letters the pointer is
    /// on rather than by the boundaries it is nearest — the pressed letter is
    /// selected as soon as the drag moves, and a letter joins when the pointer
    /// reaches it — and the gesture needs both edges of a letter for that.
    /// Equal to `caret` and `subCaret` everywhere else, where a point is a
    /// boundary and the gesture is the half-open one between two of them.
    int caretAfter = 0;
    int subCaretAfter = 0;
};

/// Resolves a caret's row and open threshold against a terminal.
///
/// The geometry — which boundary a point is nearest — is the frontend's, since
/// only it knows where its cells are drawn; what a caret MEANS on this
/// terminal is here. Two things come out of the model: the absolute row, and
/// the threshold, which collapses an empty line's void to a single slot so
/// that sweeping left and right along a blank line opens no selection at all,
/// while a content line's trailing void stays selectable.
///
/// The form for a caller with no measured point: the boundary stands, and
/// `subCaret` is zero, which is the answer on every row outside a proportional
/// run. It owns the non-finite value the form below reads as "no point", so no
/// caller spells that convention itself.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal to read.
///   - viewOffset: Rows the viewport is scrolled back.
///   - visibleRow: The caret's row, zero-based in the viewport.
///   - caret: The cell boundary, 0..columns.
/// - Returns: The resolution.
[[nodiscard]] CaretResolution resolveSelectionCaret(
    const Terminal& terminal, int viewOffset, int visibleRow, int caret) noexcept;

/// Resolves a caret against a terminal from where the pointer is along the row.
///
/// The form above answers a boundary, which is all a monospace row has. Inside
/// a proportional run a cell holds several letters' ink, and the boundary a
/// point is nearest cannot name the letter under it — so this form takes the
/// point as well and asks the run's own layout, the same layout the letters
/// were stored by, which letter the pointer stands before. The answer descends
/// into the caret a selection already speaks: a cell, and code points into
/// that cell's text.
///
/// One run is laid out, the one under the point; nothing else on the screen is
/// touched, and a point on an ordinary cell shapes nothing at all. The point
/// is in cells rather than pixels because the grid's pitch is the one length
/// both sides know; the run's pixels are the shaper's. Off every run, and for
/// a frontend that could not measure the point — pass a non-finite value —
/// `caret` stands exactly as handed in and `subCaret` is zero.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal to read, whose measurer lays the run out.
///   - viewOffset: Rows the viewport is scrolled back.
///   - visibleRow: The caret's row, zero-based in the viewport.
///   - caret: The cell boundary the frontend's geometry rounded to, 0..columns.
///   - xInCells: Where the pointer is along the row, in cells from the grid's
///     left edge, unrounded.
/// - Returns: The resolution, whose `subCaret` is nonzero only inside a run.
[[nodiscard]] CaretResolution resolveSelectionCaret(
    const Terminal& terminal, int viewOffset, int visibleRow, int caret, double xInCells) noexcept;

/// Resolves a caret against a terminal from where the pointer is along the
/// row and down it.
///
/// The form above, which a rich text block needs the vertical half of: its
/// lines are not rows, so which line a point is on is the block's layout's
/// answer. Inside a block the caret is the block's top-left cell and the index
/// of the character the point is on, the first of the block's lines whose top
/// is at or above the point and the first character on it whose right edge is
/// past the point, at its grapheme's start; `caretAfter` is past the
/// grapheme. Everywhere else this is the form above.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal to read.
///   - viewOffset: Rows the viewport is scrolled back.
///   - visibleRow: The caret's row, zero-based in the viewport.
///   - caret: The cell boundary the frontend's geometry rounded to.
///   - xInCells: Where the pointer is along the row, in cells, unrounded.
///   - yInRows: Where the pointer is down the viewport, in rows, unrounded;
///     non-finite takes the middle of `visibleRow`.
/// - Returns: The resolution.
[[nodiscard]] CaretResolution resolveSelectionCaret(
    const Terminal& terminal, int viewOffset, int visibleRow, int caret, double xInCells, double yInRows) noexcept;

/// Whether an executed command sits at or above one row.
///
/// Asked without selecting anything, because a menu decides whether its
/// item is enabled before the user picks it.
///
/// - Parameters:
///   - terminal: The terminal to read.
///   - absoluteRow: The row, in absolute buffer coordinates.
/// - Returns: Whether a command was found.
[[nodiscard]] bool hasCommandUnderRow(const Terminal& terminal, int absoluteRow) noexcept;

/// Selects the command line of the command under one row.
///
/// The prompt row through the row before output begins, which is more than
/// one row whenever the prompt printed an information line above the command
/// or the command wrapped. Whole rows, because a command line is what was
/// typed and the columns around it are the prompt's.
///
/// - Parameters:
///   - state: The selection to write, untouched when nothing is found.
///   - terminal: The terminal to read.
///   - absoluteRow: The row, in absolute buffer coordinates.
/// - Returns: Whether a command was found.
bool selectCommandLineUnderRow(SelectionState& state, const Terminal& terminal, int absoluteRow) noexcept;

/// Selects the output of the command under one row.
///
/// The search is anchored on this command's own executed-prompt row rather
/// than on the click, so a click on the prompt line selects the output of
/// the command it introduces and not the previous command's.
///
/// - Parameters:
///   - state: The selection to write, untouched when nothing is found.
///   - terminal: The terminal to read.
///   - absoluteRow: The row, in absolute buffer coordinates.
/// - Returns: Whether a command with output was found.
bool selectCommandOutputUnderRow(SelectionState& state, const Terminal& terminal, int absoluteRow) noexcept;

/// Selects the search's active match whole, as closing a find bar hands it
/// over.
///
/// A match across soft-wrapped rows is one character selection from its first
/// row's first cell to its last row's last cell, so a copy takes the text the
/// search matched. The search counts a match's rows after the primary
/// screen's history on either screen, and a selection after the history its
/// screen shows, none on the alternate screen; a match in the history behind
/// the alternate screen is on no row a selection there can name, and selects
/// nothing.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - state: The selection to write, untouched when nothing is selected.
///   - terminal: The terminal searched.
/// - Returns: Whether the active match was selected.
bool selectActiveSearchMatch(SelectionState& state, const Terminal& terminal) noexcept;

} // namespace Ubiquitty::Selection
