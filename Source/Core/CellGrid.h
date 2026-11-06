// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// CellGrid — single owner of the cell model + back-buffer shadow.
//   - one model  : cells_
//   - one shadow : m_bbShadow (mirrors BB pixels exactly)
//   - "dirty"    : computed on demand from (cells_ ≠ m_bbShadow)
//   - selection  : separate bitset (visual overlay, not in cells_)
// Cell state is mutated only through the setCell/eraseRange/scrollRegion mutators; the parser never
// touches cells_ directly. The PAL paints through a PaintSession that yields one same-pen run at a time
// and syncs the shadow per acknowledged run.
// Stores are heap-allocated to actual rows × columns (grow-only; see ensureCapacity); maxRows/maxColumns
// are only a ceiling. Single-threaded; the frontend drives it cooperatively.

#include "Core/Cursor/Blink.h"
#include "Core/Cursor/RenderState.h"
#include "Core/PaintRun.h"
#include "Core/PaintSession.h"
#include "Core/RenderCell.h"
#include "Core/RowMeta.h"
#include "Core/RowView.h"
#include "Core/Screen/PendingScroll.h"
#include "Core/StrongIndex.h"
#include "Core/TerminalOperations.h"
#include "PAL/BellMode.h"
#include <algorithm>
#include <vector>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty {

/// Which of the two screen buffers a grid operation addresses.
enum class ScreenMode : uint8_t {
    Primary,
    Alternate
}; // first enumerator == old `false`

/// Which way a rectangle scroll carries its content, and so which edge of the
/// band it vacates: `Up` the last rows, `Down` the first.
enum class RectangleScrollDirection : uint8_t {
    Up,
    Down
};

/// The authoritative representation in a physical row's backing storage.
enum class RowStorage : std::uint8_t {
    Cells,
    DefaultASCII,
};

/// A logical cell borrowed from its row or reconstructed from compact bytes.
///
/// The cell is read-only. Its pointer expires at the next grid mutation or
/// when this read is destroyed, whichever comes first.
class CellRead {
public:
    /// Creates an absent read.
    ///
    /// Its pointer is null until another read is assigned.
    CellRead() = default;

    /// Borrows the cell held by this read.
    ///
    /// Keep this read alive while using the returned pointer.
    /// - Returns: The cell, or nullptr outside the grid.
    [[nodiscard]] const Cell* get() const&
    {
        if (m_cell)
            return m_cell;
        return m_value ? &*m_value : nullptr;
    }

    /// Prevents borrowing a pointer from a temporary read.
    ///
    /// Store the read in a local variable before calling get().
    [[nodiscard]] const Cell* get() const&& = delete;

private:
    friend class CellGrid;

    explicit CellRead(const Cell* cell)
        : m_cell(cell)
    {
    }

    explicit CellRead(const Cell& cell)
        : m_value(cell)
    {
    }

    const Cell* m_cell = nullptr;
    std::optional<Cell> m_value;
};

/// A row's width, storage representation and metadata, which travel together.
///
/// `filled` is the content-width upper bound: cells [filled, m_columns) all equal the default blank.
/// Writers extend it; the scroll path copies and blanks only the live prefix. Conservative — any unsure
/// path sets it to the column count, which is correct and unoptimised.
///
/// `meta` is the single store of record for the line attribute, soft-wrap join and OSC 133 mark; the
/// Screen views forward here.
struct alignas(8) RowState {
    int filled;
    RowMeta meta;
    RowStorage storage = RowStorage::Cells;
};

/// Which way a row shift carries the cells in its window: `InsertRight` opens
/// blanks at the window's start and pushes the tail right (ICH / DECIC),
/// `DeleteLeft` removes at the start and pulls the tail left (DCH / DECDC).
enum class RowShiftDirection : uint8_t {
    InsertRight,
    DeleteLeft
};

/// What a whole-visible-screen scan for SGR blink saw.
enum class BlinkReading : std::uint8_t {
    /// No scan stands for the current content.
    Unread = 0,
    /// The scan read every visible row and found no blink cell.
    NoCellBlinks = 1,
    /// The scan found a blink cell.
    ACellBlinks = 2,
};

class CellGrid {
public:
    CellGrid();
    ~CellGrid();

    /// Why a backing-store capacity cannot be represented.
    enum class BackingStorageError : std::uint8_t {
        OverCapacity,
    };

    /// Capacities of the grid's two screens, shadow, and parallel backing arrays.
    ///
    /// Bytes count owned array capacities, excluding the CellGrid object,
    /// allocator bookkeeping, and independently allocated lazy rendering caches.
    /// Peak includes the old backing arrays while their replacements are held.
    struct BackingStoragePlan {
        int rowCapacity = 0;
        int columnCapacity = 0;
        std::size_t retainedBytes = 0;
        std::size_t peakBytes = 0;

        bool operator==(const BackingStoragePlan&) const = default;
    };

    /// Measures a growth without allocating or materializing deferred cells.
    ///
    /// The existing grow-only capacity and rounding policy select the arrays.
    /// A request below one requires one entry, as the backing allocator does.
    /// - Parameters:
    ///   - rows: Rows the caller needs.
    ///   - columns: Columns the caller needs.
    /// - Returns: Checked backing capacities and overlap, or a refusal for a
    ///   geometry beyond `maxRows` by `maxColumns`.
    [[nodiscard]] std::expected<BackingStoragePlan, BackingStorageError> backingStoragePlan(int rows, int columns) const;

    void init(int rows, int columns);
    // reflowMode distinguishes two callers. REFLOW: rows already rewrapped to newColumns, so re-stamping
    // each row's content width is accurate. FIXED (tmux/agent/sync/daemon panes): row content is untouched,
    // so re-stamping must not happen — it would claim a shrink erased wider content forever.
    /// Resizes the grid, rewrapping the scrollback or not.
    ///
    /// - Parameters:
    ///   - newRows: The new row count.
    ///   - newColumns: The new column count.
    ///   - reflowMode: Whether existing lines rewrap to the new width.
    void resize(int newRows, int newColumns, Ubiquitty::Modes::ReflowMode reflowMode = Ubiquitty::Modes::ReflowMode::Reflow);

    /// Makes room for a geometry ahead of the work that will fill it.
    ///
    /// The resize path derives a great deal from the geometry it was asked for — old and new widths,
    /// which rows rewrap, where the cursor lands — and uses all of it after the grid has been resized.
    /// A grid that could not grow and quietly took a smaller geometry would leave every one of those
    /// disagreeing with it. So a caller that can handle a refusal asks here first, before any of that
    /// is computed, and abandons the resize whole rather than half.
    ///
    /// Grow-only: a geometry the buffers already cover succeeds without allocating.
    ///
    /// - Parameters:
    ///   - rows: Rows the resize will need.
    ///   - columns: Columns the resize will need.
    /// - Returns: Whether the room is there.
    [[nodiscard]] bool reserveCapacity(int rows, int columns) { return ensureCapacity(rows, columns) != CapacityOutcome::Refused; }

    /// Admits the complete backing allocation batch before allocating any array.
    ///
    /// An unrepresentable plan or insufficient allowance leaves backing storage,
    /// geometry, and deferred cells untouched. If an admitted allocation fails,
    /// geometry and logical cells are preserved, but deferred blanks may have
    /// been materialized. The allowance excludes lazy rendering caches.
    /// - Parameters:
    ///   - rows: Rows the caller needs.
    ///   - columns: Columns the caller needs.
    ///   - maxPeakBackingBytes: The most old and replacement backing bytes admitted together.
    /// - Returns: Whether the admitted capacity is held.
    [[nodiscard]] bool reserveCapacity(int rows, int columns, std::size_t maxPeakBackingBytes) { return ensureCapacity(rows, columns, maxPeakBackingBytes) != CapacityOutcome::Refused; }

    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] int columns() const { return m_columns; }

    // Primary/alternate screen ownership; each has its own cells store. setAlternateActive switches the
    // active pointer; the bb shadow does not move (it mirrors BB), so the next paint's (active != shadow)
    // diff yields exactly the cells needing repaint.
    void setAlternateActive(ScreenMode screen);
    [[nodiscard]] bool isAlternateActive() const { return m_activeIndex == 1; }

    // SOLE mutators on cell state. All writes target the ACTIVE screen's cells store.
    void setCell(Row r, Column c, const Cell&);
    // For callers that already know the value differs (parser's per-glyph path). Skips setCell's memcmp,
    // then applies identical dirty/blink/content-width bookkeeping. A mistaken call only over-marks dirty.
    void setCellKnownDiff(Row r, Column c, const Cell&);
    /// Writes a wide-cell pair atomically.
    ///
    /// Pre-clears any adjacent half of a different pair the new cells overlap
    /// (using `blank`), then writes both halves together. Asserts the lead has
    /// Wide set + WideContinuation clear and the continuation the reverse.
    ///
    /// The two halves carry the same colour and style bytes, so a partial
    /// overwrite of either cannot silently change colours. Their `attributes`
    /// words differ and must: the assert above requires it, and the caller
    /// strips the presentation flags from the continuation. A reader wanting
    /// "same region" therefore compares every Pen field EXCEPT attributes — a
    /// whole-pen comparison fails on every wide cell in the tree.
    ///
    /// - Parameters:
    ///   - r: The row to write.
    ///   - c: The lead's column; the continuation lands at c+1.
    ///   - lead: The pair's lead half.
    ///   - continuation: The pair's continuation half.
    ///   - blank: What a broken neighbouring pair's orphaned half becomes.
    void writeWideCell(Row r, Column c, const Cell& lead, const Cell& continuation, const Cell& blank);

    /// Writes one whole run — an anchor and its continuations — as a span.
    ///
    /// The chain-sized sibling of ``writeWideCell``: any run overlapping the
    /// span's window has its spill outside the window blanked first, so the
    /// write cannot leave a neighbouring chain half standing. The span's
    /// first cell must carry `Attribute::Wide` and every later cell
    /// `Attribute::WideContinuation`; a span that does not fit the row is
    /// rejected whole, neither blanking nor writing.
    ///
    /// - Parameters:
    ///   - r: The row to write.
    ///   - c: The anchor's column.
    ///   - run: The run's cells, anchor first, at least two.
    ///   - blank: What a broken neighbouring run's spill becomes.
    void writeRunRow(Row r, Column c, RowView run, const Cell& blank);
    // Bulk row write from `span` at firstColumn; amortises per-call bounds + idempotency overhead.
    // Idempotent: a byte-matching cell is skipped without bumping dirty. Used by Screen::insertCharacters/Screen::deleteCharacters.
    void setRowSpan(Row r, Column firstColumn, RowView span);
    // setRowSpan to a SPECIFIC screen (write mirror of cellAt(ScreenMode)); snapshot apply reconstructs the
    // frozen primary while alternate is active. Active screen → paint-tracked path; inactive → off the
    // paint-dirty path (later setAlternateActive supplies the repaint).
    void setRowSpan(ScreenMode screen, Row r, Column firstColumn, RowView span);
    /// Writes a run of printable ASCII straight into the row.
    ///
    /// One byte becomes one cell's code point, all sharing the pen template, so
    /// the run's byte length is also its column width — which is why the text
    /// carries its own count here and no staging buffer stands between. Same
    /// dirty, blink and filled bookkeeping as `setRowSpan`.
    ///
    /// - Parameters:
    ///   - r: Destination row; out of range writes nothing.
    ///   - firstColumn: Leftmost column. A negative value clips the run's head
    ///     rather than refusing it, matching how a wide grid scrolls sideways.
    ///   - tmpl: The pen every cell in the run takes.
    ///   - text: Printable ASCII, one byte per column. Copied before returning.
    void writeASCIIRun(Row r, Column firstColumn, const Cell& tmpl, std::string_view text);
    /// Writes unclustered, non-shaping, non-emoji code points as wide cell pairs.
    ///
    /// The caller establishes width and clears partners overlapping the run's
    /// outer edges. The entire run must fit; invalid geometry writes nothing.
    /// Pen and hyperlink values come from the template, with width attributes
    /// derived for each half. The template carries no cluster data; code-point
    /// storage does not overlap the destination cells.
    ///
    /// - Parameters:
    ///   - row: Destination row.
    ///   - firstColumn: First lead cell's column.
    ///   - cellTemplate: Shared pen and hyperlink values.
    ///   - codePoints: Nonzero, non-shaping code points, each two columns wide.
    void writeWideCodePointRun(Row row, Column firstColumn, const Cell& cellTemplate, std::span<const uint32_t> codePoints);
    /// Writes unclustered, non-shaping code points as single-width cells.
    ///
    /// The caller establishes width and clears partners overlapping the run's
    /// outer edges. The entire run must fit; invalid geometry writes nothing.
    /// The template supplies the pen and hyperlink and carries no cluster data
    /// or wide-cell attributes. Code-point storage does not overlap the
    /// destination cells.
    ///
    /// - Parameters:
    ///   - row: Destination row.
    ///   - firstColumn: First cell's column.
    ///   - cellTemplate: Shared pen and hyperlink values.
    ///   - codePoints: Nonzero, non-shaping code points, each one column wide.
    void writeNarrowCodePointRun(Row row, Column firstColumn, const Cell& cellTemplate, std::span<const uint32_t> codePoints);
    /// Writes a run of printable ASCII translated through a character set.
    ///
    /// The companion to `writeASCIIRun` for a designated GL set: one byte still
    /// becomes exactly one cell, so the run's byte length remains its column
    /// width. That holds only while every entry in `translation` is a
    /// single-cell code point that needs no cursive shaping, which the caller
    /// establishes before designating the set — this writer marks no row for
    /// shaping and splits no wide pair.
    ///
    /// - Parameters:
    ///   - r: Destination row; out of range writes nothing.
    ///   - firstColumn: Leftmost column, clipped at the head like `writeASCIIRun`.
    ///   - tmpl: The pen every cell in the run takes.
    ///   - text: Printable ASCII, one byte per column. Copied before returning.
    ///   - translation: Code points for bytes `0x20..0x7E`, indexed by
    ///     `byte - runTranslationFirstByte`. Read for the call's duration.
    void writeTranslatedRun(Row r, Column firstColumn, const Cell& tmpl, std::string_view text, const std::uint16_t* translation);
    // Uniform-run write: `count` copies of one cell (REP expansion). Same bookkeeping as writeASCIIRun.
    void fillCellRun(Row r, Column firstColumn, const Cell& cell, int count);
    void eraseRowRange(Row r, Column c0, Column c1, const Cell& blank);
    void eraseRow(Row r, const Cell& blank);
    // Blank a range on the non-active screen without dirty-tracking side effects. Used during resize so
    // newly-revealed inactive-screen cells don't retain pre-resize content.
    void eraseRowRangeInactive(Row r, Column c0, Column c1, const Cell& blank);
    void scrollRegion(Row top, Row bot, int n, const Cell& blank);
    /// Scrolls a column-bounded rectangle, the shape DECSLRM left/right margins force: the ring cannot
    /// rotate, because the columns outside the margins must stay where they are, so the cells move.
    /// Each row resolves its slot once and moves its span in bulk, against the two slot resolutions,
    /// idempotency compare and write-lane bookkeeping a per-cell write pays for every cell it covers.
    /// Per-row metadata stays put: a mark, line attribute or wrap join cannot be split at a column.
    void scrollRectangleCells(int top, int bot, int left, int right, int n, const Cell& blank,
        RectangleScrollDirection direction);
    /// How many deferred band lines the pool can hold.
    ///
    /// The band height is the useful ceiling: at k = height the commit's
    /// rectangle move degenerates to the revealed-row blanks and the copies
    /// carry everything. The pool itself allocates lazily on first deferral --
    /// a terminal that never splits pays nothing -- and an allocation failure
    /// reads as capacity zero, which disables deferral rather than the
    /// terminal.
    /// - Returns: The pool depth; zero disables deferral.
    [[nodiscard]] int bandLanePoolCapacity() const { return m_rows; }
    /// Defers one band scroll: swaps the band-bottom row's storage for a pool
    /// row whose band columns are blanked, so the next line writes into the
    /// pool through the untouched write path while every already-written row
    /// stays where it lies.
    ///
    /// - Parameters:
    ///   - bandBottomRow: The band's bottom row, where the cursor writes.
    ///   - left: The band's first column, inclusive.
    ///   - right: The band's last column, inclusive.
    ///   - blank: The run's BCE blank, pinned for the run's lifetime.
    void bandLaneDefer(int bandBottomRow, int left, int right, const Cell& blank);
    /// Commits a deferred band run: restores the pre-burst bottom storage,
    /// moves the pre-burst band up by `count` in one rectangle move, and
    /// copies each deferred line's band columns into its final row.
    ///
    /// - Parameters:
    ///   - top: The band's top row.
    ///   - bandBottomRow: The band's bottom row.
    ///   - left: The band's first column, inclusive.
    ///   - right: The band's last column, inclusive.
    ///   - count: How many lines the run deferred.
    ///   - blank: The run's BCE blank.
    void bandLaneCommit(int top, int bandBottomRow, int left, int right, int count, const Cell& blank);
    /// Shifts one row's cells inside a column window, blanking the end the shift frees. The row moves
    /// where it lies, against reading it whole into a buffer, shifting there and storing the span back —
    /// two row-length copies for a window that is usually shorter than the row.
    ///
    /// Writes no cell outside the window, so a multicell half the window's edge splits is left split:
    /// callers owning one keep the buffer path, whose scrub can reach the half sitting outside.
    /// - Parameters:
    ///   - row: The row to shift, in logical rows.
    ///   - from: The window's first column.
    ///   - rightBound: One past the window's last column.
    ///   - n: How many columns to shift by, clamped to the window's width.
    ///   - blank: The cell written into the freed columns. Its hints are raised here; a shift carries no
    ///     other cell the row did not already hold, so the rest are already the row's.
    ///   - direction: Which end the blanks open at.
    void shiftRowCells(int row, int from, int rightBound, int n, const Cell& blank, RowShiftDirection direction);
    void scrollFullViewportUpOne(const Cell& blank);
    void scrollFullViewportUpOneDefaultBlank();
    // Pre-emptive break of any wide pair with lead or continuation in [from..to]; both halves overwritten with
    // `blank`. Used by narrow writes that would land mid-pair. Idempotent.
    void clearWideOverlap(Row r, Column from, Column to, const Cell& blank);

    // Like clearWideOverlap but for callers that immediately OVERWRITE all of [from..to]: only a pair
    // straddling an edge can orphan a half outside the span, so this checks the two edge cells — O(1).
    // Caller must overwrite [from..to] right after.
    void clearWideOverlapEdges(Row r, Column from, Column to, const Cell& blank);

private:
    /// Whether a colored break-blank claims filled width where it lands.
    ///
    /// The stored break-blank is the only one that widens a row's filled
    /// extent; a default blank landing past it must not, or the row reports
    /// content it does not hold.
    enum class FilledWidthClaim {
        None,
        PerStoredBreakBlank,
    };

    /// Blanks the parts of any run overlapping `[lo, hi]` that stand outside it.
    ///
    /// The window's own cells are the caller's to overwrite; the spill is what
    /// orphans, at whatever width the overlapped chain reaches. Bumps the row
    /// and the mutation flag only when a cell changed.
    ///
    /// - Parameters:
    ///   - ri: The physical row index, already validated.
    ///   - row: The row's cells.
    ///   - lo: The window's first column.
    ///   - hi: The window's last column, inclusive.
    ///   - blank: What a blanked cell becomes.
    ///   - claim: Whether the blank claims filled width where it lands.
    void blankRunSpillOutsideWindow(int ri, Cell* row, int lo, int hi, const Cell& blank, FilledWidthClaim claim);

public:
    // Per-row metadata (RowMeta). CellGrid is the single store of record for lineAttribute/wrapped/oscMark.
    // m_rowState[screen] is addressed through the same logical-row mapping as m_rowPtrs, so metadata travels
    // with its cells on scroll. rowMeta(r) etc. act on the active screen; rowMetaAt addresses a specific
    // screen (0 primary, 1 alternate) so each Screen's read-through views see its own record.
    [[nodiscard]] RowMeta& rowMeta(int r);
    [[nodiscard]] const RowMeta& rowMeta(int r) const;
    // Inline (for header-only view consumers that don't link CellGrid.cpp). Valid callers stay in [0, rows);
    // an out-of-range row is a caller bug (asserted). Release resolves to a scratch slot so a stray index
    // can't dereference past the allocation.
    [[nodiscard]] RowMeta& rowMetaAt(int screen, int r)
    {
        if (screen < 0 || screen > 1 || r < 0 || r >= m_rowCapacity || !m_rowState[screen]) {
            assert(false && "rowMetaAt: out-of-range row/screen drops the write");
            m_rowMetaScratch = RowMeta { lineAttributeNormal, 0, {}, 0 };
            return m_rowMetaScratch;
        }
        return m_rowState[screen][physicalRowSlot(screen, r)].meta;
    }
    [[nodiscard]] const RowMeta& rowMetaAt(int screen, int r) const
    {
        static const RowMeta normal { lineAttributeNormal, 0, {}, 0 };
        if (screen < 0 || screen > 1 || r < 0 || r >= m_rowCapacity || !m_rowState[screen])
            return normal;
        return m_rowState[screen][physicalRowSlot(screen, r)].meta;
    }
    void setLineAttribute(int r, uint8_t a); // geometry-affecting → marks row dirty
    void setRowWrapped(int r, bool w);
    void setRowMark(int r, uint8_t m);
    /// Returns a row's metadata to the default a recycled row starts from,
    /// clearing every latched flag with it.
    ///
    /// - Parameters:
    ///   - r: The row to reset, in [0, rows).
    void resetRowMeta(int r);

    // Bulk read of the active row into `out` (caller buffer ≥ columns() Cells). Used by scrollback capture
    // which needs the row before Screen::scrollUp shifts it out.
    void readRow(Row r, Cell* out) const;

    /// Reads a named screen's row into `out`.
    ///
    /// The reading half of the `setRowSpan(ScreenMode, …)` pair: a pass that judges the screen the
    /// terminal is not showing names that screen on both sides, where the plain `readRow` would hand it
    /// the active one. Out-of-range rows and a null buffer read nothing.
    ///
    /// - Parameters:
    ///   - screen: The screen to read, active or not.
    ///   - r: The row to read, in [0, rows).
    ///   - out: Caller buffer of at least `columns()` cells.
    void readRow(ScreenMode screen, Row r, Cell* out) const;

    // Upper bound on the active row's live content width: [rowContentWidth(r), columns()) are all default
    // blank. Scroll path copies only this prefix. Returns columns() when unknown (conservative).
    [[nodiscard]] int rowContentWidth(Row r) const;

    /// Reads the exact history-capture width of either screen.
    /// - Parameters:
    ///   - screen: The primary or alternate buffer.
    ///   - row: A live row in that buffer.
    /// - Returns: Its bounded content width, or the column count outside the grid.
    [[nodiscard]] int rowContentWidth(ScreenMode screen, Row row) const;

    /// Restores a captured width without dropping any nonblank cell.
    /// - Parameters:
    ///   - screen: The buffer whose cells have already been restored.
    ///   - row: A live row in that buffer.
    ///   - width: The predecessor's history-capture bound.
    /// - Returns: False without mutation if the range or blank-tail invariant is invalid.
    [[nodiscard]] bool restoreRowContentWidth(ScreenMode screen, Row row, int width);

    // nullptr if (r,c) is out of bounds; callers within rows()×columns() never see it.
    [[nodiscard]] const Cell* cellAt(Row r, Column c) const;
    // cellAt without materializing a deferred recycle blank: a deferred row is
    // logically the default blank in every column, so the probe answers from
    // m_defaultBlank instead of paying the row fill. Same nullptr-on-OOB
    // contract. For idempotency/attribute probes that precede a write — the
    // write lanes materialize (or blank the stale flanks) themselves.
    [[nodiscard]] const Cell* logicalCellAt(Row r, Column c) const;
    /// Reads one logical cell while preserving its row representation.
    ///
    /// Ordinary cells are borrowed; compact cells are reconstructed into the
    /// returned read. Neither arm materializes a deferred recycle blank.
    /// - Parameters:
    ///   - r: The active screen's logical row.
    ///   - c: The visible column in that row.
    /// - Returns: A read whose pointer is null outside the grid.
    [[nodiscard]] CellRead logicalCellReadAt(Row r, Column c) const;
    /// Reads one logical cell without requiring a contiguous Cell row.
    ///
    /// The returned value owns its bytes; out-of-range coordinates return nullopt.
    /// - Parameters:
    ///   - r: The active screen's logical row.
    ///   - c: The visible column in that row.
    /// - Returns: An owned cell, or nullopt outside the grid.
    [[nodiscard]] std::optional<Cell> logicalCellValueAt(Row r, Column c) const;
    /// Borrows the default-pen byte prefix when this row is stored compactly.
    ///
    /// The span expires on mutation or a read requesting contiguous Cells
    /// (cellAt, logicalCellAt or rowCells). The unwritten tail is blank.
    /// - Parameters:
    ///   - r: The active screen's logical row.
    /// - Returns: The borrowed prefix, or nullopt for materialized or invalid rows.
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> compactASCIIRow(Row r) const;
    // cellAt for a SPECIFIC screen (mirrors rowMetaAt); snapshot path reads Primary while alternate is
    // active. Same nullptr-on-OOB contract; resolves that screen's deferred-blank slot.
    [[nodiscard]] const Cell* cellAt(ScreenMode screen, Row r, Column c) const;
    // Direct row-pointer access for tight renderer read loops. `count == columns()` for valid rows,
    // {nullptr, 0} for OOB — so callers can't pair a stale length with the pointer.
    [[nodiscard]] RowView rowCells(Row r) const;
    // rowCells for a SPECIFIC screen, the read mirror of setRowSpan(ScreenMode, ...) and the
    // row-wide sibling of cellAt(ScreenMode, ...). The snapshot serializer reads Primary while
    // the alternate is active, and a whole row at a time is what lets it hand the run over
    // instead of walking it. Same {nullptr, 0} OOB contract; resolves that screen's
    // deferred-blank slot.
    [[nodiscard]] RowView rowCells(ScreenMode screen, Row r) const;

    // Selection overlay — orthogonal to cell content.
    void clearSelectionDirty();
    void markSelectionFlipped(Row r, Column firstColumn, Column lastColumn);

    // Paint.
    PaintSession beginPaint();

    // Forced shadow reset; next paint dirties every cell. Use on resize, theme change, alt-screen toggle.
    void invalidateShadow();

    /// Whether a queued scroll blit is still waiting for the PAL to perform it.
    ///
    /// A caller that moves cells under a queued blit has invalidated its
    /// premise: the shift it describes no longer produces the screen the cells
    /// now hold.
    [[nodiscard]] bool hasPendingScroll() const { return m_pendingScroll.valid; }

    // Mark this paint a full redraw — every cell painted regardless of shadow. Auto-clears at first run.
    void requestFullRedraw();

    // Invalidate the shadow for one row only. Used by the DECBLINK (SGR 5) animator to flip
    // blink-attributed cells without touching the rest of the screen.
    void invalidateRowShadow(int row);

    // Shift m_bbShadow rows [topR..botR] by `n` (positive = content up, matching ackScroll). PAL wrapper
    // for when it scrolls BB + window pixels in lockstep for view-offset changes, so the shadow tracks the
    // new pixel positions instead of forcing a full repaint via invalidateShadow. No-op if shadow invalid.
    void shiftShadowForViewOffset(int topR, int botR, int n);

    // Per-cell shadow read/write for PAL paths doing their own dirty diff (e.g. scrollback's exposed-band
    // painter). readShadowCell returns nullptr for OOB or when m_bbShadowValid is false — treat as needing
    // repaint then. writeShadowCell is a no-op for OOB.
    [[nodiscard]] const Cell* readShadowCell(int row, int column) const;
    void writeShadowCell(int row, int column, const Cell&);

    // After painting every cell and writeShadowCell'ing each, the PAL may call this to declare the shadow
    // trustworthy for the next paint's diff.
    void markShadowValid();

    // Read the pending full-redraw flag without consuming it. Renderer fast-paths check this to decide
    // whether to skip PaintSession entirely and repaint the whole screen.
    [[nodiscard]] bool isFullRedrawRequested() const { return m_fullRedrawRequested; }

    // True if the next paint would have work: any cell write since the last fully-synced paint, a queued
    // pending scroll, a full-redraw request, or a selection-dirty bit. Lets the cursor-only flush fast
    // path skip beginPaint() so cursor-blink frames don't pay iteration cost.
    [[nodiscard]] bool hasUnpaintedWork() const;

    // Conservative DECBLINK hint: true MAY mean a Blink cell exists; false means none does. Set on every
    // Blink-attributed write, sticky until clearBlinkHintIfClean(). Sticky-true is safe.
    [[nodiscard]] bool hasBlinkHint() const { return m_blinkHint; }

    /// The damage generation at which the hint was last set.
    ///
    /// A viewer that scanned the whole grid and saw no Blink cell may retract the hint for itself, but
    /// only when its scan is not older than this — otherwise a cell that started blinking after the scan disarms a
    /// clock with blink on screen, which is the stale-FALSE ``hasBlinkHint`` forbids.
    ///
    /// - Returns: The generation the hint was armed at, or zero while it has never been set.
    [[nodiscard]] std::uint64_t blinkHintGeneration() const { return m_blinkHintGeneration; }

    // Monotonic damage generation: count of paint sessions that acked damage (advanced the shadow
    // baseline). A double-buffering snapshot renderer reads it before/after its drain to detect a peer
    // buffer's intervening drain (see Rendering::captureCanReuse).
    [[nodiscard]] std::uint64_t currentDamageGeneration() const { return m_damageGeneration; }

    /// Advances whenever any row's cells may have changed.
    ///
    /// Every write funnels through the two row-bump helpers, and so does the alternate-screen swap,
    /// which bumps every row — so a reading that depends only on cell contents may be cached against
    /// this and recomputed only when it moves. Not the damage generation: that counts paint sessions,
    /// and a write between two paints does not advance it.
    ///
    /// - Returns: The count of row bumps this grid has taken.
    [[nodiscard]] std::uint64_t contentGeneration() const { return m_contentGeneration; }

    /// A viewer's whole-visible-screen blink reading, if one was taken since the last write.
    ///
    /// ``hasBlinkHint`` is monotone, so a viewer that wants the sharper answer scans the visible rows
    /// — and the capture, the snapshot and the animation clock each want it, every frame, on content
    /// none of them changed. The answer depends only on cells, so it is kept here until a write moves
    /// ``contentGeneration``.
    ///
    /// - Returns: The reading, or ``BlinkReading::Unread`` when none stands for this generation.
    [[nodiscard]] BlinkReading blinkReading() const
    {
        if (m_blinkReading == BlinkReading::Unread || m_blinkReadingGeneration != m_contentGeneration)
            return BlinkReading::Unread;
        return m_blinkReading;
    }

    /// Records a whole-visible-screen blink reading taken at the current content generation.
    ///
    /// The caller owes the reading's scope: the visible rows of the live screen, scanned under the
    /// same lock that writes them. A reading taken across a write lands stamped with a generation it
    /// does not describe.
    ///
    /// - Parameters:
    ///   - reading: What the scan saw. ``Unread`` records nothing.
    void recordBlinkReading(BlinkReading reading) const
    {
        if (reading == BlinkReading::Unread)
            return;
        m_blinkReading = reading;
        m_blinkReadingGeneration = m_contentGeneration;
    }

    // Per-live-row content generation for the shaped-row cache. Once the cache exists, every row mutator
    // bumps it, so a matching generation means the row is unchanged since last shaped.
    [[nodiscard]] uint32_t rowGeneration(int liveRow) const;
    // Shaped-row cache. lookupShapedRow fills `out` and returns true on a hit for (absoluteRow, generation)
    // at the current epoch; storeShapedRow caches an already-shaped row. History rows pass generation 0.
    [[nodiscard]] bool lookupShapedRow(int absoluteRow, uint32_t generation, Cell* out, int columns) const;
    void storeShapedRow(int absoluteRow, uint32_t generation, const Cell* shaped, int columns) const;
    // Invalidate the whole shaped-row cache in O(1) (epoch bump). Called on resize/clear/alt-screen switch.
    void invalidateShapedRowCache() const { ++m_shapedRowEpoch; }
    // Drop the shaped-row cache sizing entirely (frees its slots). Called when
    // a grid is carried across a wipe: the memo is render-side state sized to
    // the old screen, and keeping its allocation would both pin a screenful of
    // Cells past the reset and pre-size a cache a refused rebuild must not have.
    void dropShapedRowCache() const;

    /// Conservative sized-text hint: true MAY mean a cell carries a text-size specification id, false means
    /// none does. Set on every write of a cell whose pen carries one, on either screen; retracted only by
    /// ``clearSizedCellHintAfterWholeGridScan``.
    ///
    /// A client that cannot draw sized text flattens every such cell out of both grids on each applied
    /// frame, and the flatten is a whole-grid read whether or not there is anything to find. On a terminal
    /// that never saw sized text — which is nearly all of them — this answers false and the read is skipped
    /// entirely.
    ///
    /// Stale-true costs one whole-grid scan that finds nothing. Stale-false costs the flatten, which is a
    /// visible defect: the ids survive into a renderer that paints the whole glyph on every row a block
    /// covers. So the arming side is the conservative one — a write arms on the id alone, without asking
    /// whether the cell was already sized.
    [[nodiscard]] bool hasSizedCellHint() const { return m_hasSizedCellHint; }

    /// Drops the sized-text hint.
    ///
    /// The caller owes the same precondition ``clearBlinkHintIfClean`` states, with one difference that
    /// makes it easier to keep: the only caller — ``Screen::flattenSizedCells`` — scans both whole grids
    /// and removes every id it finds in the same call, holding the model lock throughout. Scan and
    /// retraction are one critical section, so no write can slip between them and no generation stamp is
    /// needed to detect one. A caller that scanned under a different lock, or scanned a subset, would need
    /// both.
    void clearSizedCellHintAfterWholeGridScan() { m_hasSizedCellHint = false; }

    /// Drops the hint and its generation.
    ///
    /// The caller owes a precondition: it must have just scanned the WHOLE grid and found no Blink cell.
    /// ``hasBlinkHint`` may be stale-true and must never be stale-false, and this is the one call that can
    /// break that. A scan scoped to anything narrower — one viewer's capture, one screen, the visible rows
    /// — clears a flag the rest of the grid still earns, and a surface with blink on it then disarms its
    /// clock.
    ///
    /// No shipping path calls this, so the hint stays true from the first blink-attributed write until the
    /// grid goes away. Tests call it to isolate which write re-arms it, which is a use the precondition
    /// above does not cover and does not need to: a fresh grid has nothing to scan.
    void clearBlinkHintIfClean()
    {
        m_blinkHint = false;
        m_blinkHintGeneration = 0;
    }

#ifndef NDEBUG
    // Tests: confirm the "row clean" hint matches the grid — any row with m_anyDirtyInRow[r] false must match
    // m_bbShadow byte-for-byte and have no selection-dirty bits. False if drift detected.
    [[nodiscard]] bool assertInvariants() const;
    // True if the most recently destroyed PaintSession yielded runs the PAL never AckRun'd (paint leak).
    [[nodiscard]] bool paintInvariantBroken() const { return m_paintInvariantBroken; }
#endif

private:
    void shadowShiftRows(Row top, Row bot, int n);
    void clearAllSelectionDirty();
    [[nodiscard]] bool isSelectionDirty(int r, int c) const;
    void setSelectionDirty(int r, int c);
    void markDirtyRange(int top, int bot);
    // Single write path for "row r changed": sets the paint dirty hint and bumps the shaped-row cache
    // generation together, so no mutator can move a row's cells without invalidating its cached shaping.
    /// Marks every row of a range as possibly diverged from the shadow.
    ///
    /// ``bumpRow`` asks whether the shaped-row cache exists once per row, and the answer cannot change
    /// inside a range. Hoisted, the hint bytes become a run to be written rather than a branch to be
    /// taken per row. A scrolled band pays this for its whole height on every line it scrolls, which is
    /// where the difference shows.
    ///
    /// - Parameters:
    ///   - top: First row of the range; clamped to the grid.
    ///   - bot: Last row of the range, inclusive; clamped to the grid. Below `top`, nothing is marked.
    void bumpRowRange(int top, int bot)
    {
        if (top < 0)
            top = 0;
        if (bot >= m_rows)
            bot = m_rows - 1;
        if (top > bot)
            return;
        // A band is a handful of rows, and at that width a runtime-length libc call costs more than the
        // stores it stands in for. Eight rows to a write, with a last one overlapping what the loop left,
        // settles any band in three stores and no call.
        static_assert(sizeof(bool) == 1, "the pattern below marks one row per byte");
        bool* const dirty = m_anyDirtyInRow + top;
        const int count = bot - top + 1;
        constexpr uint64_t everyRowMarked = 0x0101010101010101ull;
        if (count >= 8) {
            int index = 0;
            for (; index + 8 <= count; index += 8)
                std::memcpy(dirty + index, &everyRowMarked, sizeof(everyRowMarked));
            std::memcpy(dirty + count - 8, &everyRowMarked, sizeof(everyRowMarked));
        } else {
            for (int index = 0; index < count; ++index)
                dirty[index] = true;
        }
        ++m_contentGeneration;
        if (m_shapedRowSlots) {
            for (int r = top; r <= bot; ++r)
                ++m_rowGeneration[r];
        }
    }
    void bumpRow(int r)
    {
        m_anyDirtyInRow[r] = true;
        ++m_contentGeneration;
        if (m_shapedRowSlots)
            ++m_rowGeneration[r];
    }
    void clearDirtyRange();
    void materializeDirtyRange();
    /// Materializes a deferred recycle blank everywhere but a write window.
    ///
    /// Blanks the stale flanks the caller's own store does not cover, then
    /// clears the deferral. A caller comparing the write window must first
    /// establish that its cells are logically blank, or resolve it whole.
    ///
    /// - Parameters:
    ///   - slot: The deferred physical row slot.
    ///   - firstColumn: The window's first column, owned by the caller's store.
    ///   - lastColumn: The window's last column, likewise.
    void materializeDeferredBlankAroundWindow(int slot, int firstColumn, int lastColumn);
    /// Writes a run onto a row still holding an unmaterialized recycle blank.
    ///
    /// The destination is logically blank but physically stale, so the lanes in
    /// `writeRun` would compare against a recycled row's bytes. Broadcasting the
    /// template and patching code points needs no read at all, which is why this
    /// destination gets its own lane rather than a branch inside the others.
    ///
    /// - Parameters:
    ///   - slot: The deferred physical row slot.
    ///   - row: Destination row, for the dirty and filled bookkeeping.
    ///   - firstColumn: The run's first column.
    ///   - lastColumn: The run's last column.
    ///   - cellTemplate: The pen every cell in the run takes.
    ///   - source: The run's bytes, `lastColumn - firstColumn + 1` of them.
    ///   - translation: Code points for `0x20..0x7E` when `Translated`, else unread.
    template<bool Translated>
    void writeRunToDeferredBlank(int slot, int row, int firstColumn, int lastColumn, const Cell& cellTemplate, const char* source, const std::uint16_t* translation);

    /// Writes a run of bytes into a row, one cell per byte.
    ///
    /// Carries both public run writers: `Translated` decides whether a byte is
    /// its own code point or an index into `translation`, and nothing else about
    /// the write differs, so the two share their bounds, their destination lanes
    /// and their bookkeeping rather than being kept in step by hand.
    ///
    /// - Parameters:
    ///   - r: Destination row; out of range writes nothing.
    ///   - firstColumnValue: Leftmost column; a negative value clips the head.
    ///   - tmpl: The pen every cell in the run takes.
    ///   - text: One byte per column. Copied before returning.
    ///   - translation: Code points for `0x20..0x7E` when `Translated`, else unread.
    template<bool Translated>
    void writeRun(Row r, Column firstColumnValue, const Cell& tmpl, std::string_view text, const std::uint16_t* translation);
    void scrollFullViewportUpOneWithBlank(const Cell& blank, bool blankDefault);

    int m_rows;
    int m_columns;
    // Allocated capacity. All heap buffers below are m_rowCapacity × m_columnCapacity; the logical screen
    // is always ≤ capacity; row stride is m_columnCapacity. Grow-only: resize past it reallocates
    // (preserving content); shrinking just lowers m_rows/m_columns.
    int m_rowCapacity;
    int m_columnCapacity;
    // Two screens (0 primary, 1 alternate), each a flat m_rowCapacity × m_columnCapacity block.
    /* owned */ Cell* m_screenStore[2];
    // Row-pointer ring: m_rowPtrs[s][physicalRowSlot(s,r)] is the storage row logical row r maps to (a
    // permutation of m_screenStore[s]'s rows). Partial scrolls rotate these; full-viewport scrolls move
    // m_rowOrigin. All cell access goes through these — never index m_screenStore by logical row.
    /* owned */ Cell** m_rowPtrs[2];
    // Per-row width and metadata, parallel to m_rowPtrs and rotated with it. Grow-only, m_rowCapacity.
    // One record rather than two arrays: a band scroll moves every row of its band on every line it
    // scrolls, and the two halves always move together, so separate arrays cost that slide a second
    // stream and a second load/store pair per row for no reader's benefit.
    /* owned */ RowState* m_rowState[2];
    // Owned OOB sink for non-const rowMetaAt: an asserted OOB row lands here, not a shared static, so a
    // stray write can't race another CellGrid or thread. No initializer (cheap construction).
    RowMeta m_rowMetaScratch;
    int m_rowOrigin[2];
    // Band-lane pool bookkeeping (bandLaneDefer): stashed storage-row pointers
    // in arrival order -- [0] the pre-burst bottom row, the rest pool rows
    // holding deferred lines -- plus the bottom slot's pre-burst row state,
    // restored at commit. Sized lazily to the pool capacity.
    std::vector<Cell*> m_bandStash;
    // The pool's own storage, m_rows x m_columnCapacity, lazily allocated on
    // the first deferral and dropped whenever the column capacity it was
    // strided for changes (ensureStorage frees it; a live run never spans a
    // resize -- the runtime flushes before the grid reallocates).
    std::vector<Cell> m_bandPool;
    int m_bandPoolColumnStride {};
    int m_bandStashCount {};
    int m_bandSavedFilled {};
    RowMeta m_bandSavedMeta {};
    // Canonical default blank for the [filled, columns) invariant. Set once via setDefaultBlank. Until set,
    // m_haveDefaultBlank is false and every width stays pinned at m_columns (optimization inert).
    Cell m_defaultBlank;
    bool m_haveDefaultBlank;
    int m_activeIndex;
    // Set row r's content-width bound (active screen), clamped to columns.
    void noteFilled(int ri, int width);
    // True iff `c` byte-equals m_defaultBlank.
    bool isDefaultBlank(const Cell& c) const;
    void resetRowPointers();
    // Deferred recycle blank (one row per screen). A full-viewport scroll recycles the departing row as
    // the new edge; the slot is recorded here and the row's filled width drops to 0 (logically blank), and
    // the physical fill happens when next materialized (rowPtrAt resolves it before handing out the
    // pointer; the ASCII-run/span/wide write lanes store their window and blank only the stale flanks).
    // Paths touching storage without rowPtrAt (ring rotation, realloc, reset) must resolve first — the
    // slot index is physical, so a pointer rotation would re-aim it at the wrong row.
    int m_deferredBlankSlot[2] { -1, -1 };
    int m_deferredBlankCount[2] { 0, 0 };
    // [m_deferredBlankStart, m_deferredBlankCount) is the stale window. Single-cell
    // and wide-pair writes at its start consume it in place, leaving live cells
    // below the new edge. A write spanning live and stale storage must materialize
    // the window before comparing: blank-row reasoning applies only inside it.
    int m_deferredBlankStart[2] { 0, 0 };
    void resolveDeferredBlankSlot(int screen);
    /// Expands a compact physical row in place before Cell access.
    ///
    /// Backwards expansion preserves unread compact bytes and hidden columns.
    /// - Parameters:
    ///   - screen: The owning screen buffer index.
    ///   - slot: The physical row slot, already resolved through the ring.
    void materializeCompactRow(int screen, int slot);
    /// Expands a screen's compact rows before geometry or backing changes.
    ///
    /// - Parameters:
    ///   - screen: The owning screen buffer index.
    void materializeCompactRows(int screen);
    /// Selects whether a compact write must establish a content difference.
    enum class CompactWriteComparison : std::uint8_t {
        Compare,
        KnownDifferent,
    };
    /// Whether the row representation permits attempting a compact write.
    ///
    /// Keep ordinary rows out of the compact writer's alias and pen checks.
    /// The writer still validates those inputs before changing representation.
    /// - Parameter slot: The active screen's valid physical row slot.
    /// - Returns: Whether the row is compact or has a whole-width blank deferral.
    [[nodiscard]] bool isCompactASCIIWriteCandidate(int slot) const
    {
        assert(slot >= 0 && slot < m_rowCapacity);
        return m_rowState[m_activeIndex][slot].storage == RowStorage::DefaultASCII
            || (slot == m_deferredBlankSlot[m_activeIndex]
                && m_deferredBlankStart[m_activeIndex] == 0 && m_deferredBlankCount[m_activeIndex] >= m_columns);
    }
    /// Writes canonical-default bytes into an eligible compact row.
    ///
    /// A refusal leaves the existing representation and content intact.
    /// - Parameters:
    ///   - slot: The active screen's physical row slot.
    ///   - row: The corresponding logical row for damage bookkeeping.
    ///   - firstColumn: The clipped inclusive destination start.
    ///   - lastColumn: The clipped inclusive destination end.
    ///   - cellTemplate: The pen and non-codepoint fields shared by the run.
    ///   - source: One borrowed byte per destination column.
    ///   - comparison: Whether the caller already established a difference.
    /// - Returns: Whether the compact path handled the write.
    [[nodiscard]] bool writeCompactASCIIRun(int slot, int row, int firstColumn, int lastColumn, const Cell& cellTemplate, const char* source, CompactWriteComparison comparison = CompactWriteComparison::Compare);
    /// Writes a known-different cell after compact storage has been excluded.
    ///
    /// The caller validates bounds and expands compact storage before entering.
    ///
    /// - Parameters:
    ///   - row: The active screen's logical row.
    ///   - column: The destination column.
    ///   - cell: The cell known to differ from the current value.
    void setCellKnownDiffOrdinary(Row row, Column column, const Cell& cell);
    /// Handles a known-different cell on a compact or recyclable blank row.
    ///
    /// A refused compact write expands storage before ordinary bookkeeping.
    ///
    /// - Parameters:
    ///   - row: The active screen's logical row.
    ///   - column: The destination column.
    ///   - cell: The cell known to differ from the current value.
    void setCellKnownDiffCompact(Row row, Column column, const Cell& cell);
    /// Stages an aliased compact byte input before writing its destination.
    ///
    /// The bounded stack owner prevents expansion from overwriting unread input.
    /// - Parameters:
    ///   - row: The active screen's logical destination row.
    ///   - column: The clipped first destination column.
    ///   - cellTemplate: The pen and non-codepoint fields shared by the run.
    ///   - source: The consumed input bytes overlapping the compact backing,
    ///     from one through maxColumns of them.
    ///   - translation: The run translation table, or null for untranslated input.
    template<bool Translated>
    void writeRunFromCompactBytes(Row row, Column column, const Cell& cellTemplate, std::string_view source, const std::uint16_t* translation);
    void resolveDeferredBlankIfSlot(int screen, int slot)
    {
        if (slot == m_deferredBlankSlot[screen]) [[unlikely]]
            resolveDeferredBlankSlot(screen);
    }
    [[nodiscard]] int physicalRowSlot(int screen, int r) const;
    /// Borrows a cell from a materialized row using an already resolved slot.
    ///
    /// A deferred blank is answered without changing the row's backing.
    /// - Parameters:
    ///   - screen: A valid screen buffer index.
    ///   - slot: A valid physical row slot containing Cells.
    ///   - column: A visible column.
    /// - Returns: The cell, or the default blank in a deferred stale window.
    [[nodiscard]] const Cell* logicalCellAtSlot(int screen, int slot, int column) const;
    /// Materializes one compact row before borrowing its logical cell.
    ///
    /// The pointer expires on the next grid mutation.
    /// - Parameters:
    ///   - screen: A valid screen buffer index.
    ///   - slot: A valid physical row slot.
    ///   - column: A visible column.
    /// - Returns: The materialized cell.
    [[nodiscard]] const Cell* materializedLogicalCellAt(int screen, int slot, int column) const;
    Cell* rowPtrAt(int screen, int r)
    {
        const int slot = physicalRowSlot(screen, r);
        materializeCompactRow(screen, slot);
        resolveDeferredBlankIfSlot(screen, slot);
        return m_rowPtrs[screen][slot];
    }
    const Cell* rowPtrAt(int screen, int r) const
    {
        const int slot = physicalRowSlot(screen, r);
        const_cast<CellGrid*>(this)->materializeCompactRow(screen, slot);
        const_cast<CellGrid*>(this)->resolveDeferredBlankIfSlot(screen, slot);
        return m_rowPtrs[screen][slot];
    }
    /// A row's live content width, to read or to write.
    ///
    /// - Parameters:
    ///   - screen: Which of the two screen buffers to address.
    ///   - r: The logical row, resolved through the ring like every other row access.
    /// - Returns: A reference to the row's width bound; cells past it equal the default blank.
    int& rowFilledAt(int screen, int r) { return m_rowState[screen][physicalRowSlot(screen, r)].filled; }

public:
    // Everything Screen::scrollUp's history capture needs from a departing row, read with one slot resolution
    // rather than one per accessor. Byte-identical to the accessors.
    struct ScrollCaptureMetadata {
        int contentWidth;
        RowMeta meta;
    };
    // Deferred-state scroll lane halves of scrollFullViewportUpOneWithBlank: the rotation core runs per
    // scrolled line; bookkeeping accumulates in the Terminal's lane run and scrollLaneCommit applies it once.
    void scrollLaneRotateOnly(const Cell& blank);
    void scrollLaneCommit(int scrolledLines);

    ScrollCaptureMetadata scrollCaptureMetadata(int r) const
    {
        const int slot = physicalRowSlot(m_activeIndex, r);
        const RowMeta meta = m_rowState[m_activeIndex][slot].meta;
        int width = m_rowState[m_activeIndex][slot].filled;
        if (width < 0)
            width = 0;
        if (width > m_columns)
            width = m_columns;
        return { meta.has(Ubiquitty::RowFlag::Wrapped) ? m_columns : width, meta };
    }

private:
    /// A row's live content width.
    ///
    /// - Parameters:
    ///   - screen: Which of the two screen buffers to address.
    ///   - r: The logical row, resolved through the ring like every other row access.
    /// - Returns: The row's width bound; cells past it equal the default blank.
    const int& rowFilledAt(int screen, int r) const { return m_rowState[screen][physicalRowSlot(screen, r)].filled; }
    // Slot-resolved variants for the per-cell write path: resolve physicalRowSlot once and reuse it for
    // the pointer, fill bound, and idempotency read.
    Cell* rowPtrAtSlot(int screen, int slot)
    {
        materializeCompactRow(screen, slot);
        resolveDeferredBlankIfSlot(screen, slot);
        return m_rowPtrs[screen][slot];
    }
    void noteFilledSlot(int screen, int slot, int width)
    {
        if (width > m_columns)
            width = m_columns;
        int& filled = m_rowState[screen][slot].filled;
        if (width > filled)
            filled = width;
    }
    /// Returns the row ring to origin zero, resolving a live deferred blank first.
    ///
    /// A caller about to move rows between slots by any other means needs the ring flat before it
    /// starts: a deferral names a slot, and a rotation renames every slot it covers.
    ///
    /// A ring already at zero with nothing deferred is the state a screen sits in between full-viewport
    /// scrolls, and there this has no work to do. Two loads settle that, where reaching the rotation is
    /// a call into a frame whose answer is always the same.
    ///
    /// - Parameters:
    ///   - screen: Which of the two screen buffers to normalise.
    void normalizeRowRing(int screen)
    {
        if (screen < 0 || screen > 1)
            return;
        if (m_deferredBlankSlot[screen] < 0 && m_rowOrigin[screen] == 0)
            return;
        rotateRowRingToOrigin(screen);
    }
    /// Rotates the row ring so logical row zero sits in slot zero, resolving a deferred blank first.
    ///
    /// The work ``normalizeRowRing`` delegates once it has established there is any. A ring of one row
    /// or fewer has no rotation to express and returns having done nothing.
    ///
    /// - Parameters:
    ///   - screen: Which of the two screen buffers to rotate.
    void rotateRowRingToOrigin(int screen);
    /// Allocates every buffer for a geometry and installs them, or installs none.
    ///
    /// Nothing is assigned until all of it is held, so a failure leaves the grid exactly as it was:
    /// the caller keeps the geometry it had rather than a half-built one. The Core builds without
    /// exceptions, which is what makes the distinction matter — a throwing allocation that fails
    /// terminates the process, taking every session in a daemon with it.
    ///
    /// - Parameters:
    ///   - plan: Checked capacities already admitted as one allocation batch.
    /// - Returns: Whether the buffers were installed.
    [[nodiscard]] bool allocBuffers(const BackingStoragePlan& plan);
    void freeBuffers();
    // Ensure capacity ≥ (rows, columns), reallocating (preserving content) on growth. True if realloc'd.
    /// What ``ensureCapacity`` did.
    enum class CapacityOutcome : std::uint8_t {
        /// The buffers already covered the geometry; nothing moved.
        AlreadySufficient,
        /// New buffers were installed and the old content carried across.
        Grown,
        /// The buffers could not be allocated; the grid keeps the geometry it had.
        Refused,
    };

    /// Grows the backing store to cover a geometry, carrying the content across.
    ///
    /// Grow-only, with headroom, so consecutive drag steps land inside existing capacity. Nothing is
    /// freed until the replacement is held, so ``Refused`` leaves a grid that still works at the size
    /// it was — which is what the caller must honour rather than proceeding to a geometry the buffers
    /// do not cover.
    ///
    /// - Parameters:
    ///   - rows: Rows the caller needs.
    ///   - columns: Columns the caller needs.
    ///   - maxPeakBackingBytes: The admitted old and replacement backing capacity.
    /// - Returns: Which of the three happened.
    [[nodiscard]] CapacityOutcome ensureCapacity(int rows, int columns, std::size_t maxPeakBackingBytes = std::numeric_limits<std::size_t>::max());
    Cell* activeRow(int r) { return rowPtrAt(m_activeIndex, r); }
    const Cell* activeRow(int r) const { return rowPtrAt(m_activeIndex, r); }
    // Single BB shadow, tracking BB pixels regardless of active screen. After an alt-screen toggle it
    // still holds pre-toggle content, so the (active != shadow) diff surfaces the new screen's cells.
    // Flat m_rowCapacity × m_columnCapacity.
    /* owned */ Cell* m_bbShadow;
    Cell* shadowRow(int r) { return m_bbShadow + std::size_t(r) * m_columnCapacity; }
    const Cell* shadowRow(int r) const { return m_bbShadow + std::size_t(r) * m_columnCapacity; }
    bool m_bbShadowValid;
    bool m_fullRedrawRequested;
    // Sticky bit for hasUnpaintedWork(): set on any state-changing mutation; cleared at successful
    // end-of-paint.
    bool m_anyMutationsSinceLastPaint;
    // Sticky DECBLINK hint (see hasBlinkHint). Set by every Blink-attributed write; cleared only by
    // clearBlinkHintIfClean. May be stale-true, never stale-false.
    bool m_blinkHint;
    // The damage generation the hint was last set at (see blinkHintGeneration).
    std::uint64_t m_blinkHintGeneration;
    // Row bumps taken (see contentGeneration), and the blink reading cached against one.
    std::uint64_t m_contentGeneration = 0;
    mutable std::uint64_t m_blinkReadingGeneration = 0;
    mutable BlinkReading m_blinkReading = BlinkReading::Unread;
    /// Arms the hint and stamps it with the current damage generation.
    ///
    /// The one place the hint is armed. Every Blink-attributed write routes through it so the stamp cannot
    /// fall out of step with the flag: the writes are spread over the per-cell, known-diff, row-span,
    /// ASCII-run, deferred-blank, fill and wide-pair paths, and a site that set one without the other
    /// would publish a stamp older than the blink it describes — which is the staleness the stamp exists
    /// to rule out.
    void noteBlinkWrite()
    {
        m_blinkHint = true;
        m_blinkHintGeneration = m_damageGeneration;
    }
    // Sticky sized-text hint (see hasSizedCellHint). Set by every write of a cell carrying a text-size
    // specification id; cleared only by clearSizedCellHintAfterWholeGridScan. May be stale-true, never
    // stale-false.
    bool m_hasSizedCellHint;
    /// Arms the sized-text hint.
    ///
    /// The one place it is armed, for the reason ``noteBlinkWrite`` gives: the writes are spread over the
    /// per-cell, known-diff, row-span, inactive-span, ASCII-run, deferred-blank, fill and wide-pair paths,
    /// and a site that wrote a sized cell without arming would let the flatten skip a grid that needs it.
    ///
    /// The inactive-screen span write is on this list and not on the blink one. That is not an oversight
    /// on either side: a blink cell off screen animates nothing, while a sized cell off screen is flattened
    /// by a pass that reads both grids.
    void noteSizedWrite() { m_hasSizedCellHint = true; }
#ifndef NDEBUG
    bool m_paintInvariantBroken; // last destroyed PaintSession had unacked runs
#endif

    // Per-cell selection-dirty bitset, flat m_rowCapacity × m_selectionStride ((m_columnCapacity+7)/8).
    /* owned */ unsigned char* m_selectionDirty;
    int m_selectionStride;
    unsigned char* selectionRow(int r) { return m_selectionDirty + std::size_t(r) * m_selectionStride; }
    const unsigned char* selectionRow(int r) const { return m_selectionDirty + std::size_t(r) * m_selectionStride; }
    // True iff any selection-dirty bit is set. Lets PaintSession's dtor skip the memset when no selection
    // is active.
    bool m_anySelectionDirty;
    // Per-row any-dirty hint, set by any mutator touching row r, cleared in nextDirtyRun when a per-cell
    // scan finds the row clean. Lets nextDirtyRun skip rows in O(1). False positives harmless; false
    // negatives would skip real paint work, so every row-touching mutator must set its bit.
    /* owned */ bool* m_anyDirtyInRow;
    // Per-live-row content generation. Dormant until the shaped-row cache is allocated, then bumped via
    // bumpRow. Frozen history rows key on 0.
    /* owned */ uint32_t* m_rowGeneration;
    bool m_dirtyRangeValid;
    int m_dirtyRangeTop;
    int m_dirtyRangeBottom;

    // Shaped-row cache: memoizes getDisplayRow's Arabic-shape + bidi-reorder, keyed on (absolute row,
    // generation, epoch), so a static RTL screen shapes once. mutable — pure render-side memoization.
    // Lazily allocated, one slot per live row, so a direct map never puts two
    // rows of one screen in the same slot. A fixed size does both halves wrong:
    // short windows carry the whole allocation, and every row past the size
    // shares a slot with the row that many rows above it, where the two evict
    // each other every frame.
    mutable int m_shapedRowCapacity = 0;
    struct ShapedRowSlot {
        int absoluteRow = -1;
        uint32_t generation = 0;
        uint32_t epoch = 0;
        int length = 0;
    };
    /* owned */ mutable ShapedRowSlot* m_shapedRowSlots = nullptr; // [m_shapedRowCapacity]
    /* owned */ mutable Cell* m_shapedRowCells = nullptr; // [m_shapedRowCapacity * maxColumns]
    mutable uint32_t m_shapedRowEpoch = 1; // 0 reserved as "never stored"

    struct PendingScrollSlot {
        int top;
        int bot;
        int delta;
        // Screen the scroll belongs to. Coalescing is rejected when a new scroll arrives on a different
        // active screen — the geometry survived but its cell-model context no longer applies.
        int activeIndex;
        bool valid;
    } m_pendingScroll;

    // Monotonic count of paint sessions that acked damage (advanced the shadow baseline). A snapshot
    // renderer double-buffers over this one shadow; a value larger than it stored last capture means a
    // drain it didn't perform moved the baseline, so its retained rows are stale. See captureCanReuse.
    std::uint64_t m_damageGeneration = 0;

    friend class PaintSession;
};

// Animation pump: called once per frame with current + last-painted ticks. True iff a time-dependent bit
// of CellGrid state changed across the interval and a repaint is required. Hides blink phase math.
static inline bool needsAnimationRedraw(const CellGrid* d, uint32_t monotonicTicks, uint32_t lastPaintedTicks)
{
    if (!d)
        return false;
    // Sample both blink periods (SGR 5 = 8 steps/cycle, SGR 6 = 16); any step change is a frame to draw.
    if (blinkAlpha(monotonicTicks, Cursor::Blink::caretPeriodTicks) != blinkAlpha(lastPaintedTicks, Cursor::Blink::caretPeriodTicks))
        return true;
    if (blinkAlpha(monotonicTicks, Cursor::Blink::caretPeriodTicks / 2) != blinkAlpha(lastPaintedTicks, Cursor::Blink::caretPeriodTicks / 2))
        return true;
    return false;
}

static inline uint32_t ticksUntilAnimationRedraw(const CellGrid* display, uint32_t monotonicTicks)
{
    if (!display) {
        return 0;
    }
    const uint32_t slowTicks = ticksUntilBlinkAlphaChange(monotonicTicks, Cursor::Blink::caretPeriodTicks);
    const uint32_t fastTicks = ticksUntilBlinkAlphaChange(monotonicTicks, Cursor::Blink::caretPeriodTicks / 2);
    if (!slowTicks) {
        return fastTicks;
    }
    if (!fastTicks) {
        return slowTicks;
    }
    return std::min(slowTicks, fastTicks);
}

// Bell policy. The parser sets bellPending on BEL; the frontend pushes its preference into bellMode.
// resolveBellEvent folds the pending flag, mode, and a coalescer into one answer; the platform executes
// the cue. No bell decision lives in platform code.

// Rapid BEL bursts collapse to one cue within this window. 6 ticks ≈ 0.1 s at 60 Hz.
static constexpr uint32_t bellCoalesceTicks = 6;

// Visible-bell flash duration. 12 ticks ≈ 0.2 s.
static constexpr uint32_t bellFlashTicks = 12;

// Resolve a pending bell into an effective BellMode. Off when nothing pending, the pref disables it, or a
// prior bell fired within the coalesce window. Clears bellPending and advances the coalescer. nullptr-safe.
static inline BellMode resolveBellEvent(Ubiquitty::Terminal* t, Ubiquitty::Ticks monotonicTicks)
{
    if (!t || !t->bellPending)
        return BellMode::Off;
    t->bellPending = false;
    const BellMode mode = t->bellMode;
    if (mode == BellMode::Off)
        return BellMode::Off;
    if (t->lastBellTick != 0 && monotonicTicks.since(Ubiquitty::Ticks(t->lastBellTick)) < bellCoalesceTicks)
        return BellMode::Off;
    t->lastBellTick = monotonicTicks.count;
    return mode;
}

// Default visible-bell peak overlay opacity; the configurable intensity pref scales this.
static constexpr float bellFlashPeakDefault = 0.35f;

// Visible-bell flash envelope: overlay alpha in [0, peak] for a flash from startTick, decaying linearly to
// 0 over bellFlashTicks. Returns 0 when startTick is 0 or the flash elapsed. peak is the configurable
// intensity; the zero-crossing is independent of peak.
static inline float bellFlashAlpha(uint32_t startTick, uint32_t nowTicks, float peak = bellFlashPeakDefault)
{
    if (startTick == 0)
        return 0.0f;
    const uint32_t elapsed = nowTicks - startTick;
    if (elapsed >= bellFlashTicks)
        return 0.0f;
    return peak * (1.0f - static_cast<float>(elapsed) / static_cast<float>(bellFlashTicks));
}

// Scroll-position indicator fade. The frontend stamps lastScrollTick on each scrollback move; the overlay
// holds full opacity then fades to 0 (auto-hide). Returns 0 when never scrolled or the fade elapsed.
static constexpr uint32_t scrollIndicatorHoldTicks = 60; // ~1.0 s solid
static constexpr uint32_t scrollIndicatorFadeTicks = 36; // ~0.6 s fade-out
static inline float scrollIndicatorAlpha(uint32_t lastScrollTick, uint32_t nowTicks)
{
    if (lastScrollTick == 0)
        return 0.0f;
    const uint32_t elapsed = nowTicks - lastScrollTick;
    if (elapsed < scrollIndicatorHoldTicks)
        return 1.0f;
    const uint32_t faded = elapsed - scrollIndicatorHoldTicks;
    if (faded >= scrollIndicatorFadeTicks)
        return 0.0f;
    return 1.0f - static_cast<float>(faded) / static_cast<float>(scrollIndicatorFadeTicks);
}

} // namespace Ubiquitty

// Screen::lineAttribute / lineWrapped / mark read-through views. Defined here,
// inline, because they index CellGrid::m_rowState (complete only now) and header-only consumers must resolve
// them without linking CellGrid.cpp. A Screen with no bound CellGrid (owner == nullptr, e.g. a math-only
// unit test) falls back to a process-wide RowMeta array indexed by row.

namespace Ubiquitty::detail {
// Namespace-scope inline variable, not function-local: a local static's init guard would make
// rowMetaOrFallback un-inlinable, leaving every view operator[] a real two-deep call even under ThinLTO.
//
// `constinit` because namespace scope alone does not buy what the line above
// wants. RowMeta holds an OptionSet whose storage carries a default member
// initializer, so the implicit default constructor is not trivial and
// default-initialising the array is DYNAMIC initialisation — which emits the
// guard variable the line above avoids. Value-initialising is a constant
// expression instead, and constinit is what keeps it one.
//
// constinit is not for speed: the whole program checks that guard once, and
// buildCellInstances' instruction stream is byte-identical either way. It keeps
// a dynamic initializer out and the intent above enforced.
inline constinit Ubiquitty::RowMeta rowMetaFallback[maxRows] {};
inline Ubiquitty::RowMeta& rowMetaOrFallback(Ubiquitty::CellGrid* owner, int screen, int r)
{
    if (!owner) [[unlikely]] {
        int i = (r < 0) ? 0 : (r >= maxRows ? maxRows - 1 : r);
        return rowMetaFallback[i];
    }
    return owner->rowMetaAt(screen, r);
}
}
// namespace Ubiquitty::detail

namespace Ubiquitty {

inline uint8_t& LineAttributeView::operator[](int r) { return Ubiquitty::detail::rowMetaOrFallback(owner, screen, r).lineAttribute; }
inline const uint8_t& LineAttributeView::operator[](int r) const { return Ubiquitty::detail::rowMetaOrFallback(owner, screen, r).lineAttribute; }
/// - Returns: The row's current soft-wrap state.
inline LineWrappedView::Reference::operator bool() const
{
    return Ubiquitty::detail::rowMetaOrFallback(m_owner, m_screen, m_row).has(Ubiquitty::RowFlag::Wrapped);
}
/// Writes `isWrapped` through to the row's flag.
///
/// - Returns: This reference, so an assignment chains.
inline LineWrappedView::Reference& LineWrappedView::Reference::operator=(bool isWrapped) // bool-ok: a proxy for a bool assigns the value it stands in for
{
    Ubiquitty::detail::rowMetaOrFallback(m_owner, m_screen, m_row).set(Ubiquitty::RowFlag::Wrapped, isWrapped);
    return *this;
}
inline bool LineWrappedView::operator[](int r) const
{
    return Ubiquitty::detail::rowMetaOrFallback(owner, screen, r).has(Ubiquitty::RowFlag::Wrapped);
}
inline uint8_t& RowMarkView::operator[](int r) { return Ubiquitty::detail::rowMetaOrFallback(owner, screen, r).oscMark; }
inline const uint8_t& RowMarkView::operator[](int r) const { return Ubiquitty::detail::rowMetaOrFallback(owner, screen, r).oscMark; }

} // namespace Ubiquitty
