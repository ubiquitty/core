// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// SizedBlock.h — writing and maintaining the multi-row blocks the text-size
// protocol draws: one glyph over scale rows by scale*width columns.
//
// Storage is per row, on the horizontal run encoding the wide pair set: every
// covered row carries its own anchor-plus-continuations chain, every cell
// carries the block's spec id, and the font byte's band bits say which row of
// the block a chain draws — the DECDHL discipline, where rows share their
// bytes and a stored discriminator picks the crop. A row is self-describing,
// which is what lets an editing control's damage be judged locally: a chain
// claiming band k is backed only by a chain claiming band k-1 with the same
// id and span directly above, and a chain nothing backs is erased rather than
// drawn — a torn block renders as absence, never as the wrong slice.

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Pen.h"
#include "Core/RunTextStore.h"
#include "Core/TextSizeTable.h"
#include "Forward.h"

#include <optional>
#include <span>

namespace Ubiquitty::Screen {

/// Writes one sized block: `spec.scale` rows of identical chains from
/// `(row, column)`, each stamped with `specID` and its band.
///
/// Every covered row carries the glyph's code point in its anchor and the
/// same chain shape, so a consumer can crop any row's slice without walking
/// to the block's top. The caller has already sized the block against the
/// grid and the scroll region — a block that does not fit whole is the
/// caller's refusal, not a partial write here.
///
/// - Parameters:
///   - grid: The grid to write.
///   - row: The block's top row.
///   - column: The block's left column.
///   - columns: The block's width in columns, at least 2.
///   - rows: The block's height in rows, at least 1.
///   - codePoint: The glyph, stored in every covered row's anchor.
///   - pen: The pen the block draws with; the id and band stamps are made
///     here, over whatever styling it carries.
///   - specID: The block's 1-based spec table id.
///   - blank: What a broken neighbouring run's spill becomes.
///   - linkID: The `OSC 8` link every covered cell carries, as the plain emit
///     stamps it; zero for none. A link is per cell, not in the pen, and a
///     block written without it is text a reader cannot click.
void writeSizedBlock(Ubiquitty::CellGrid& grid, int row, int column, int columns, int rows,
    std::uint32_t codePoint, const Ubiquitty::Pen& pen, unsigned specID, const Ubiquitty::Cell& blank, std::uint16_t linkID = 0);

/// Blanks every chain of `row` whose band claims a backing the row above
/// does not give it, and widens `[firstTouched, lastTouched]` over the
/// blanked cells.
///
/// The judgment is local because the rows are self-describing: a chain
/// claiming band k needs a chain claiming band k-1, the same spec id and the
/// same span directly above; band-zero chains — single-row runs and block
/// top rows — need nothing. A top-down sweep over edited rows is the
/// cascade: blanking row k's fragment is what unbacked row k+1's chain, and
/// the sweep reaches it next.
///
/// - Parameters:
///   - rowAbove: The cells one row up, or null at the grid's top edge.
///   - row: The row to judge.
///   - columns: Both rows' width.
///   - blank: What an unbacked chain's cells become.
///   - firstTouched: Widened left to the first cell blanked.
///   - lastTouched: Widened right to the last cell blanked.
void scrubUnbackedBands(const Ubiquitty::Cell* rowAbove, Ubiquitty::Cell* row, int columns,
    const Ubiquitty::Cell& blank, int& firstTouched, int& lastTouched);

/// Blanks every chain of `row` whose block the rows below cut short, and
/// widens `[firstTouched, lastTouched]` over the blanked cells.
///
/// The upward scrub cannot see this half of the split rule: a block whose
/// BOTTOM rows are gone leaves bands 0..k each backed from above — a locally
/// valid fragment that would draw the glyph's top slice. Wholeness is the
/// spec's to state: a chain of band k whose spec claims `scale` rows needs a
/// chain of band k+1 below it until the last band, and a chain whose band
/// reaches past its spec's scale is malformed outright. A bottom-up sweep is
/// this direction's cascade — blanking row k's chain is what cuts row k-1's
/// block short, and the sweep reaches it next.
///
/// A chain whose id resolves to no spec is kept: a stale id renders unsized
/// by the lookup contract, and erasing it would turn a soft degradation into
/// data loss.
///
/// - Parameters:
///   - row: The row to judge.
///   - rowBelow: The cells one row down, or null at the grid's bottom edge.
///   - columns: Both rows' width.
///   - table: The terminal's spec table; null keeps every sized chain, the
///     same soft answer an unresolvable id gets.
///   - blank: What a cut-short chain's cells become.
///   - firstTouched: Widened left to the first cell blanked.
///   - lastTouched: Widened right to the last cell blanked.
void scrubShortenedBlocks(Ubiquitty::Cell* row, const Ubiquitty::Cell* rowBelow, int columns,
    const Ubiquitty::TextSizeTable* table, const Ubiquitty::Cell& blank, int& firstTouched, int& lastTouched);

/// Re-judges every block the edit of rows `[firstRow, lastRow]` can have
/// damaged, in both directions, and blanks what the split rule condemns.
///
/// The one call an editing control makes after touching rows: the top-down
/// sweep erases chains whose backing is gone, the bottom-up sweep erases
/// chains whose block the rows below cut short, and each sweep runs past the
/// edited span while it still blanks — the cascades. The whole pass costs
/// one table read while no spec is live: bands come only from sized blocks,
/// so an empty table proves there is nothing to judge.
///
/// - Parameters:
///   - t: The terminal whose active grid was edited.
///   - firstRow: The first touched row.
///   - lastRow: The last touched row, inclusive.
void scrubBlockDamage(Ubiquitty::Terminal& t, int firstRow, int lastRow);

/// Re-judges every block on both screens after a resize rearranged them.
///
/// A resize is not an edit of a row range: rows cross between the screen and
/// history, a rewrap repacks them at new columns, and the alternate is
/// shifted and resized while nothing is showing it. Each of those separates a
/// block's rows from each other, so the population to judge is both grids
/// whole rather than a span, and the alternate is judged now because the
/// switch back does not pass through any control that would judge it.
///
/// - Parameters:
///   - t: The terminal, already carrying its new dimensions — the sweep reads
///     `rows` and `columns` to decide what a chain's claim can reach.
void scrubRearrangedScreens(Ubiquitty::Terminal& t);

/// Frees every spec id no cell on either screen, in the status row or in the
/// scrollback still names.
///
/// The mask has to come from the whole population of cells that can carry an
/// id, because a slot freed while something still points at it is handed to
/// the next distinct spec and that cell's block silently changes geometry.
/// That population is both grids whole — the inactive screen returns — the
/// DECSSDT status row, which the renderer draws through the same table, and
/// every retained scrollback line, which the view scrolls back to.
///
/// - Parameters:
///   - t: The terminal whose ids to census.
void reclaimTextSizeSpecs(Ubiquitty::Terminal& t);

/// Interns `spec`, taking a census of the cells when the table is full.
///
/// A full table usually holds specs nothing draws any more: an overwrite, an
/// erase or a scroll out of history retires cells without telling the table.
/// The census frees those ids and the intern retries. A census that frees
/// nothing defers the next one by a fixed number of refused interns, so a
/// stream of distinct specs over a table whose every id is still named costs
/// one walk per batch rather than one per run.
///
/// - Parameters:
///   - t: The terminal whose table to intern into.
///   - spec: The specification to store.
/// - Returns: The spec's 1-based id, or zero when every id is still named by a
///   cell — the caller emits its run unsized and logs the refusal.
[[nodiscard]] unsigned internTextSizeSpec(Ubiquitty::Terminal& t, const Ubiquitty::TextSizeSpec& spec);

/// The frame a peer's spec table arrived on, which decides the cells here the
/// frame did not refresh.
enum class TextSizeTableFrame : std::uint8_t {
    /// A keyframe: both grids and the status row arrive with the table, so only
    /// the scrollback can hold cells the peer no longer has.
    Snapshot,
    /// A row delta: the status row travels only on a keyframe, so it can hold
    /// such cells too.
    RowDelta,
    /// History arrives before its frame, so both grids and the status row can be stale.
    Scrollback,
};

/// Adopts a peer's spec table, first flattening the cells it would otherwise
/// misread.
///
/// A peer mints an id only into a slot none of its own cells names, but some
/// of this terminal's cells are not the peer's current ones: the scrollback is
/// its own ring, which at a larger limit keeps rows the peer has evicted, and
/// on a row delta the status row is the one the last keyframe carried. A slot
/// whose incoming spec differs from the one held here would hand those cells a
/// different geometry, so each of them naming a renamed slot is reduced to
/// plain text — the degradation an unresolvable id already gets, never a wrong
/// size. Ongoing history arrives before the tick's screen frame. Each apply
/// adopts the carried table before writing its cells, judging retained cells
/// against the table they were written under.
///
/// - Parameters:
///   - t: The receiving terminal.
///   - incoming: The peer's table, slot for slot.
///   - frame: The frame the table arrived on.
void adoptTextSizeTable(Ubiquitty::Terminal& t, const Ubiquitty::TextSizeTable& incoming, TextSizeTableFrame frame);

/// Adopts snapshot run text after flattening retained history whose ids change meaning.
///
/// - Parameters:
///   - t: The terminal whose visible cells already belong to the snapshot.
///   - incoming: The run-text store those snapshot cells name.
void adoptRunTextStore(Ubiquitty::Terminal& t, Ubiquitty::RunTextStore&& incoming);

/// Latches ``RowFlag/Flowed`` on a row that holds a proportional run.
///
/// For a row written whole by something other than the run's own writer —
/// the wire's snapshot and row-delta decoders — which writes cells without
/// knowing what they are. The host's writer latches the flag itself; this
/// scans the row once for an anchor whose spec is proportional, the same
/// question the renderer's flowed arm asks, so a mirror answers it the way
/// its host does.
///
/// - Parameters:
///   - t: The terminal.
///   - screen: The screen index the row belongs to.
///   - row: The row.
void latchFlowedRow(Ubiquitty::Terminal& t, int screen, int row);

/// Places one sized glyph at the cursor and advances past it.
///
/// The block is `spec.scale` rows tall and as many columns wide as the spec's
/// width states, or the glyph's own width where the spec leaves the terminal
/// to compute it — times the scale. A block that does not fit the columns
/// remaining wraps to the next row first, which is what the protocol's own
/// wrapping says a run does when the margin arrives mid-text.
///
/// Two refusals, and both leave the grid untouched so the caller can emit the
/// glyph unsized — the degradation a terminal without the feature already
/// gives: a block wider than the writable columns has no row it fits on, and
/// a block taller than the rows below it inside the scroll region would
/// straddle the region's edge, where a later scroll of that region moves part
/// of the block and leaves the rest. Straddling is the one damage the scrubs
/// cannot repair into anything but erasure, so it is refused at the source.
///
/// - Parameters:
///   - t: The terminal to write.
///   - codePoint: The glyph.
///   - spec: The size the block draws at.
///   - specID: The spec's table id, stamped into every covered cell.
/// - Returns: Whether the block was placed. False leaves the cursor and the
///   grid exactly as they were.
/// Places a proportional run (Docs/proportional-text.md): `spec.scale` rows
/// of `spec.runWidthCells` times `spec.scale` columns, every band's anchor
/// naming the run's text in the terminal's run-text store and every cell
/// carrying `specID` and its band, the shape ``writeSizedBlock`` gives a block.
///
/// Fits and refuses the way ``placeSizedGlyph`` does for a block — wraps
/// whole when the margin arrives mid-run, refuses a run wider than the
/// writable columns or taller than the rows left in the scroll region — and
/// advances the cursor by the run's width, which is what every terminal
/// knowing the protocol advances by whatever its face measures; that
/// invariance is the protocol's reason for carrying the width.
///
/// - Parameters:
///   - t: The terminal to write.
///   - codePoints: The run's text, bounded by the OSC payload
///     (``OSC::TextSizing::maxTextBytes``) rather than by the cells it
///     claims. It is stored once and the anchor names it.
///   - spec: The proportional specification, with its width.
///   - specID: The spec's table id, stamped into every covered cell.
/// - Returns: Whether the run was placed. False leaves the cursor and the
///   grid exactly as they were, which includes a run-text store with no id
///   free after a sweep.
[[nodiscard]] bool placeProportionalRun(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID);

/// Where a proportional run's claim stands: its top row and first column.
struct ProportionalRunPlacement {
    int row = 0;
    int column = 0;
    /// The claim wraps past the scroll region's last row, which scrolls the
    /// region a line before the claim is written on it.
    bool shouldScrollFirst = false;
};

/// Fits a proportional run's claim at the cursor, as ``placeSizedGlyph``
/// fits a block: wrapped whole to the next row when the margin arrives
/// mid-claim or the wrap is pending, or with autowrap reset set against the
/// row's right edge. The right edge is the margin's, or the last visible
/// column of a row DECDWL or DECDHL doubles; a one-row claim wrapping past
/// the scroll region's last row scrolls the region, as text does.
///
/// - Parameters:
///   - t: The terminal the run is printed in.
///   - spec: The proportional specification, with its width and scale.
/// - Returns: The claim's place, or nothing when no row of the writable
///   columns holds the claim or the scroll region has too few rows left
///   below the cursor for its bands; such a run prints as ordinary text.
[[nodiscard]] std::optional<ProportionalRunPlacement> fitProportionalRun(const Ubiquitty::Terminal& t, const Ubiquitty::TextSizeSpec& spec);

/// Sets a proportional run the terminal cannot store as a run in the grid's
/// face inside the cells it claims (Docs/proportional-text.md).
///
/// The run's place is the program's layout, which it measured and printed
/// against, so it survives a table or store with no room for the run. Each
/// grapheme takes the cells the grid's print path gives it, from the claim's
/// first cell or right or centred by `h` when the text is narrower, cut at
/// the last whole grapheme the claim holds; the claim's other cells are
/// erased. The cells are written directly, as the run's own are, so the text
/// joins no cluster before the claim, follows no right-to-left writing and
/// stays inside the claim whatever a selector widens. The cursor stands after
/// the claim, as it stands after the run. That is the run as a renderer with
/// no proportional face draws it, written into the cells.
///
/// - Parameters:
///   - t: The terminal to write.
///   - codePoints: The run's text.
///   - spec: The proportional specification, with its width, alignment and
///     scale; a scaled claim is set on its top row.
///   - placement: The claim's place, from ``fitProportionalRun``.
void setProportionalRunInGridFace(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, ProportionalRunPlacement placement);

/// Frees the run text no cell can still show.
///
/// Marks the cluster id of every sized cell that is not a continuation, on
/// both screens and every scrollback line, and hands the set to
/// ``RunTextStore::retainOnly``. Placement calls it when the store asks for a
/// sweep; the scrollback drops call it once the lines they drop are gone.
///
/// - Parameter t: The terminal whose run text to sweep.
void sweepRunText(Ubiquitty::Terminal& t);

/// Places all of an explicit-width OSC 66 payload in one indivisible block.
[[nodiscard]] bool placeSizedTextRun(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID);

/// Interns a run with appended combining characters; zero leaves the existing run intact.
[[nodiscard]] std::uint16_t appendRunText(Ubiquitty::Terminal& t, std::uint16_t textID, std::span<const std::uint32_t> suffix);

[[nodiscard]] bool placeSizedGlyph(Ubiquitty::Terminal& t, std::uint32_t codePoint,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID);

/// Places a grapheme cluster in a block with a resolved cell width.
///
/// Each band retains the cluster's text and presentation. The parser keeps
/// the anchor open for combining characters that follow the OSC string.
///
/// - Parameters:
///   - t: The terminal to write.
///   - codePoints: One nonempty grapheme cluster.
///   - spec: The size, with a nonzero resolved width.
///   - specID: The specification's table ID.
/// - Returns: Whether the block was placed.
[[nodiscard]] bool placeSizedCluster(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID);

/// Reduces every sized block on both grids to ordinary text.
///
/// For a terminal whose renderer draws no bands and which received sized
/// cells anyway — a snapshot from a host that does draw them. Each block
/// keeps one plain glyph where its anchor sat and loses the rest: the cells
/// carry an anchor and continuations, so a renderer with no band crop paints
/// the whole glyph on every covered row, and the block arrives as a column of
/// stacked duplicates. Ordinary text is what that terminal would have shown
/// had the run been typed into it, and it is the reading the protocol's own
/// degradation names.
///
/// The spec table empties with the cells, since the ids nothing carries are
/// free by the table's own rule.
///
/// - Parameter t: The terminal whose grid to flatten.
void flattenSizedCells(Ubiquitty::Terminal& t);

/// The specification ids `t`'s renderer does not draw: every ordinary sized
/// specification without ``Terminal/drawsSizedText`` and every proportional
/// one without ``Terminal/drawsProportionalText``, bit `id - 1` per id.
///
/// - Parameters:
///   - t: The terminal whose renderer's facts decide.
///   - table: The specification table the cells were written under.
/// - Returns: The ids to flatten; zero when the renderer draws every id.
[[nodiscard]] std::uint16_t undrawnTextSizeSpecIDs(const Ubiquitty::Terminal& t, const Ubiquitty::TextSizeTable& table);

/// Flattens every cell on both screens whose specification the renderer does
/// not draw (``undrawnTextSizeSpecIDs``), keeping the specification table.
///
/// For a passive terminal that takes its rows from the screen plane: a later
/// row may name the same specification ids, and only the table the host sent
/// says which of them are run-text ids, so the table stays as it arrived.
///
/// - Parameter t: The terminal whose grid to flatten.
void flattenSizedGridCells(Ubiquitty::Terminal& t);

/// Flattens one run of cells in place, the way ``flattenSizedCells`` flattens a
/// grid row.
///
/// Every frame carrying sized cells also carries the table they were written
/// under. That table distinguishes run-text ids from grapheme-cluster ids.
///
/// - Parameter cells: The run to flatten, modified in place.
/// - Parameter count: How many cells the run holds.
/// - Parameter blank: The cell a non-anchor band becomes.
/// - Parameter firstTouched: Set to the first column changed, when any was.
/// - Parameter lastTouched: Set to the last column changed, when any was.
/// - Parameter specIDMask: The ids to flatten, bit `id - 1` per id; a cell
///   naming any other id is left as it is.
/// - Parameter table: The specification table the cells were written under.
/// - Returns: True when the run held a cell the mask selects, so a caller can
///   skip its write when nothing changed.
bool flattenSizedRun(Ubiquitty::Cell* cells, int count, const Ubiquitty::Cell& blank,
    int* firstTouched, int* lastTouched, std::uint16_t specIDMask,
    const Ubiquitty::TextSizeTable& table);

} // namespace Ubiquitty::Screen
