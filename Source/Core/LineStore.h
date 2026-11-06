// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// LineStore — content-trimmed line storage for terminal scrollback.
//
// Replaces the flat full-width scrollback arena (capacity × maxColumns cells,
// paid in full at the row ceiling regardless of content) with a list of
// fixed-size cell slabs ("blocks", a block-list scrollback layout). Each retained
// line is copied once, TRIMMED to its content length, into the current
// block; a full block starts a new one; FIFO eviction frees a whole block
// once all the lines it backs have aged out. Typical content (most lines far
// short of full width) costs proportionally less; the pathological all-full-
// width case matches a full-width arena, never exceeds it.
//
// A fixed ring of `StoredLine` descriptors (capacity entries) gives O(1)
// index access (`at`); the blocks own the cells. Lines evict oldest-first,
// blocks fill front-to-back, so the oldest live line is always in the front
// block — eviction needs no per-line block bookkeeping. A blank line owns no
// cells and belongs to no block. The blocks partition the row ordinals all the
// same: a block's span runs from the ordinal of its first line to the next
// block's first, so a line's block is found by its ordinal, and the blank
// lines a span covers are never decoded.
//
// A block has two forms. A *raw* block is the slab of cells the append path
// writes, and a line in it is read at the cost of a pointer. A *folded* block
// holds the same lines as row forms (Core/Screen/RowForm.h): UTF-8 text plus
// run-length pens over a pen table of the block's own, so the pens die with
// the block and no id ever rolls. Folding is byte-exact — a line read from a
// folded block is the line that was appended — and it runs off the append
// path: `foldColdBlocks` is called from the ingest loop's quiet path, where
// nothing is being drained. A line in a folded block is read by decoding its
// whole block into a scratch slab that a small cache of recently decoded
// blocks owns, and the line's `cells` then point into it.
//
// Portable Core: STL is allowed here (not the PAL seam); no exceptions/rtti.

#pragma once

#include "Core/Cell.h"
#include "Core/Pen.h"
#include "Core/Screen/ScrollLine.h"

#include "Core/TerminalOperations.h"

#include "Base/FallibleVector.h"
#include "PAL/PAL.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty {

// LineStore retains `Screen::ScrollLine` records (cells/length/wrapped/lineAttribute/mark,
// defined in Cell.h) so the scrollback read API and its callers/tests are
// unchanged — only the cell *backing* moves from the flat full-width arena
// to trimmed block slabs. `cells` points into a block slab, or into the
// decode of a folded block, trimmed to `length`.
using StoredLine = Ubiquitty::Screen::ScrollLine;

class LineStore {
public:
    LineStore() = default;
    ~LineStore() { dispose(); }
    LineStore(const LineStore&) = delete;
    LineStore& operator=(const LineStore&) = delete;

    // Ring of `capacity` line descriptors; capacity clamped to >= 1.
    // Normal live initialization retains its throwing allocation policy.
    // Recoverable restoration uses the detached tryRestore factory instead.
    void init(int capacity);
    void dispose();

    // Append a line, copying min(length, maxColumns) cells trimmed into a block.
    // Evicts the oldest line (and frees its block once fully drained) when at
    // capacity. length == 0 is valid (a blank scrolled-off row).
    // Exhausted counters or refused directory/slab growth do not evict a row.
    void append(const Cell*, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping);
    /// Appends default-pen byte cells directly into the history slab.
    ///
    /// Bytes beyond text.size() up to length are canonical blanks. The input
    /// is borrowed only for this call; allocation refusal leaves history intact.
    /// - Parameters:
    ///   - text: The borrowed byte prefix, including zero bytes for blank gaps.
    ///   - length: The stored width, at least text.size() and at most maxColumns.
    ///   - lineAttribute: The row's line-size attribute.
    ///   - mark: The row's OSC 133 mark.
    ///   - wrapped: Whether the row continues into the following row.
    ///   - needsShaping: The captured row's shaping flag.
    void appendDefaultASCII(std::span<const std::uint8_t> text, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping); // bool-ok: wrap and shaping are stored row metadata.

    // Oldest-first: at(0) is the oldest retained line, at(count()-1) newest.
    // Out-of-range returns nullptr.
    //
    // The line's `cells` stay valid until the next append, eviction or drop,
    // and — for a line in a folded block — until `decodedBlockCacheCapacity`
    // other folded blocks have been read, which is when its decode is dropped.
    // A read of a line whose decode was dropped decodes its block again, so a
    // reader takes a line's cells before it reads another line, never after.
    // Reading a folded line mutates the decode cache, which is why every read
    // of the store, like every write, happens under the model lock.
    [[nodiscard]] const StoredLine* at(int index) const;
    [[nodiscard]] StoredLine* at(int index);

    /// Returns a line whose `cells` may be written in place.
    ///
    /// A folded block is unfolded first: the write then lands in the block's
    /// own slab rather than in a decode the cache may drop. A caller that
    /// only reads uses `at`, which leaves a folded block folded.
    ///
    /// - Parameter index: Oldest-first, as `at` takes it.
    /// - Returns: The line, or null out of range.
    [[nodiscard]] StoredLine* writableAt(int index);

    [[nodiscard]] int count() const { return m_count; }
    [[nodiscard]] int capacity() const { return m_capacity; }

    // Monotonic count of lines EVER appended (never decremented by eviction or
    // clear). The remote-scrollback host reads it to detect how many rows aged
    // into history since the last broadcast even when the ring is full and
    // count() is pinned at capacity — count-delta would miss those.
    [[nodiscard]] long long totalAppended() const { return m_totalAppended; }

    // Drop the oldest lineCount lines (clamped); frees drained front blocks.
    void dropOldest(int lineCount);
    // Drop the newest lineCount lines (clamped). Their cells stay slack in the back
    // block (reused on the next append, freed on clear) — symmetric to the
    // reflow grow that pulls the newest history back onto the live screen.
    void dropNewest(int lineCount);
    // Drop every line; frees every block (keeps the descriptor ring).
    void clear();

    // The newest blocks that stay raw whatever `foldColdBlocks` is asked. The
    // back block is the append cursor, and the one before it holds the lines
    // that aged off the screen most recently: the ones a reflow grow pulls
    // back (`dropNewest`), a scrolled-back viewport reads first, and the
    // history plane sends next. Folding those would decode them again at
    // once, and a fold a `dropNewest` then truncates was work for nothing.
    static constexpr int rawBlockWindow = 2;

    // How many folded blocks are held decoded at once. A viewport spans at
    // most two blocks and a sequential walk (search, reflow) touches each
    // block once, so two would do; four keeps a viewport's blocks decoded
    // across a reader that touches two others between frames. Each entry is
    // one slab.
    static constexpr int decodedBlockCacheCapacity = 4;

    // A block naming more pens than this stays raw. Its form would be mostly
    // the pen table and a run per cell, which saves little of the slab, and
    // the fold's cost is in naming pens: a block of cells each in its own
    // colour folded in five times the time of a text block and kept three
    // quarters of its bytes.
    static constexpr std::size_t maxPensPerFoldedBlock = 1024;

    /// The most rows a block's span may cover and still fold.
    ///
    /// A blank row takes an ordinal and no cell, so only the capacity bounds a
    /// span, and a fold walks every row of it and gives each a slot in the
    /// offset table: one cell line and a million blank rows after it would
    /// cost a million slots. A block without blank rows spans at most one row
    /// per cell, which this allows four times over, and the table it allows is
    /// half a slab. A longer span keeps its block raw.
    static constexpr std::uint64_t maxFoldSpan = 32768;

    /// Folds cold raw blocks into the row form.
    ///
    /// A block is cold when it is older than the newest `rawBlockWindow`. At
    /// most `blockBudget` blocks fold per call, oldest first; one block is
    /// bounded work (`blockCells` cells encoded over at most `maxFoldSpan`
    /// rows, then decoded again to check the round trip), which is what lets
    /// the ingest loop call this between drain passes. A block holding a row
    /// the form cannot say exactly, or spanning more than `maxFoldSpan` rows,
    /// stays raw and is not offered again; one whose check slab is refused
    /// stays raw and is offered again.
    ///
    /// Every line keeps its bytes: a fold is committed only after its decode
    /// reproduced every live cell of the block.
    ///
    /// - Parameter blockBudget: How many blocks this call may fold. Zero
    ///   queries whether work remains without changing the store.
    /// - Returns: Whether a cold raw block remains to fold.
    [[nodiscard]] bool foldColdBlocks(int blockBudget);

    // Bytes held for the retained lines: raw slabs plus folded forms, their
    // offset tables and pen tables. The decode cache is `decodedCacheBytes`.
    [[nodiscard]] std::size_t arenaBytes() const;
    // Bytes the folded blocks take, as `arenaBytes` counts them.
    [[nodiscard]] std::size_t foldedBytes() const;
    // Bytes the decode cache's slabs take.
    [[nodiscard]] std::size_t decodedCacheBytes() const;
    [[nodiscard]] int blockCount() const { return static_cast<int>(m_blocks.size()); }
    [[nodiscard]] int foldedBlockCount() const;
    // Folded blocks currently held decoded.
    [[nodiscard]] int decodedBlockCount() const;

    // Logical line numbering; Core/Screen/HistoryLines.h holds the contract.
    // A row stored with `wrapped == false` completes one logical line, and the
    // completed lines are numbered in order. Eviction, `dropNewest` and `clear`
    // keep the numbering; only `restartNumbering` changes it.

    /// Returns the oldest line whose first row is still stored.
    ///
    /// A front row that continues a line whose first rows were evicted makes
    /// that line unaddressable, so the oldest is the line after it.
    ///
    /// - Returns: The oldest addressable line, or the head when the store
    ///   holds no complete line.
    [[nodiscard]] std::uint64_t frontLineNumber() const;

    /// Returns the number the next completed logical line takes.
    [[nodiscard]] std::uint64_t headLineNumber() const { return m_headLine; }

    /// Returns the highest head this numbering has reached.
    ///
    /// A pull-back retreats the head below it, and the numbers in between stay
    /// spoken for: a reader may still hold the lines they named.
    [[nodiscard]] std::uint64_t maxHeadLineNumber() const { return m_maxHeadLine; }

    /// The stored rows of one logical line.
    struct LogicalLineRows {
        /// The index `at` takes for the line's first row.
        int firstIndex;
        /// The rows, consecutive, every one wrapped but the last.
        int rowCount;
    };

    /// Finds the stored rows of a completed line.
    ///
    /// One binary search over the index, then at most one stride of rows to
    /// the line's start and its own rows after it.
    ///
    /// - Parameter number: The line's number.
    /// - Returns: Its rows, or `nullopt` outside `[frontLineNumber(),
    ///   headLineNumber())`.
    [[nodiscard]] std::optional<LogicalLineRows> logicalLine(std::uint64_t number) const;

    /// Finds the stored rows of the line the head numbers when it completes:
    /// the newest rows, every one wrapped, back to the line's first.
    ///
    /// Walks those rows once.
    ///
    /// - Returns: Its rows, or `nullopt` when the newest row completes a line,
    ///   the store is empty, or the line's first rows were evicted.
    [[nodiscard]] std::optional<LogicalLineRows> openLine() const;

    /// A cell position in a logical line, independent of its current wrapping.
    struct LogicalPosition {
        std::uint64_t line = 0;
        std::uint64_t cell = 0;
    };

    /// Names the first written cell of a retained row.
    /// - Parameter index: Oldest-first row index.
    /// - Returns: No position if the row or its logical line's beginning is gone.
    [[nodiscard]] std::optional<LogicalPosition> positionAt(int index) const;

    /// Finds the row holding a logical position after wrapping or replacement.
    /// - Parameter position: A position captured by positionAt.
    /// - Returns: The containing row, clamped to a shortened line's final row,
    ///   or no row when the line's beginning is no longer retained.
    [[nodiscard]] std::optional<int> rowForPosition(LogicalPosition position) const;

    /// Whether a row is the first of its logical line.
    enum class LineContinuity : std::uint8_t {
        /// The row begins its line.
        StartsLine,
        /// The row continues a line begun in the row before it.
        ContinuesLine,
    };

    /// Renumbers the stored rows from a new front.
    ///
    /// - Parameters:
    ///   - frontLineNumber: The number of the line holding the front row.
    ///   - front: `ContinuesLine` when the front row continues a line whose
    ///     first rows are gone, which leaves that line unaddressable.
    void restartNumbering(std::uint64_t frontLineNumber, LineContinuity front = LineContinuity::StartsLine);

    /// Where the numbering stands at the front row, for a rebuild that clears
    /// the store and appends the same lines again.
    struct FrontLine {
        /// The number of the line holding the front row.
        std::uint64_t number;
        /// Whether the front row is that line's first.
        LineContinuity continuity;
    };

    /// Returns the numbering at the front row.
    [[nodiscard]] FrontLine frontLine() const { return { m_frontLine, m_frontContinuity }; }

    /// Returns the ordinal the next stored row takes.
    ///
    /// A row's ordinal counts the rows stored before it, less those
    /// `dropNewest` took back, so it is the same through eviction and through
    /// a pull-back that moves the newest rows onto the screen and a push that
    /// stores them again. Only `init`, `prepend` and a restored store start the
    /// count afresh. A screen row that will be stored next takes this ordinal,
    /// the one below it the next, and so on, which is what lets a position on
    /// the screen name its row in the same space as one in history.
    [[nodiscard]] std::uint64_t nextOrdinal() const { return m_nextOrdinal; }

    /// Returns the ordinal of the front row.
    [[nodiscard]] std::uint64_t frontOrdinal() const { return m_nextOrdinal - static_cast<std::uint64_t>(m_count); }

    /// Returns whether a stored row begins its logical line.
    ///
    /// Reads the row facts only, so a folded block stays undecoded.
    ///
    /// - Parameter index: Oldest-first, below `count()`.
    /// - Returns: Whether the row before it ends a line, or, for the front row,
    ///   whether the front continues nothing.
    [[nodiscard]] bool startsLine(int index) const
    {
        if (index <= 0)
            return m_frontContinuity == LineContinuity::StartsLine;
        return !descriptor(index - 1).wrapped;
    }

    /// Returns whether a stored row continues into the next, without decoding.
    ///
    /// - Parameter index: Oldest-first, below `count()`.
    [[nodiscard]] bool isWrapped(int index) const { return descriptor(index).wrapped; }

    /// Restores oldest rows without copying the retained history behind them.
    ///
    /// Cells are copied before mutation. Internal row ordinals are rebased;
    /// logical line numbers follow `front`. Append totals remain monotonic.
    /// - Parameters:
    ///   - rows: Oldest-first rows to prepend, with borrowed cells.
    ///   - front: The logical identity and continuity of the new oldest row.
    ///   - newestToDrop: Newest rows removed only after all allocations succeed.
    /// - Returns: False for invalid rows, insufficient descriptor capacity,
    ///   exhausted numbering, or refused allocation, leaving content unchanged.
    [[nodiscard]] bool prepend(std::span<const StoredLine> rows, FrontLine front, int newestToDrop = 0);

    /// The numbering and append counters a rebuilt history must retain.
    struct Continuation {
        FrontLine front;
        std::uint64_t head = 0;
        std::uint64_t maxHead = 0;
        long long totalAppended = 0;
    };

    /// Captures the counters independently of the store's block layout.
    /// - Returns: The exact numbering of the retained oldest-first rows.
    [[nodiscard]] Continuation continuation() const { return { frontLine(), m_headLine, m_maxHeadLine, m_totalAppended }; }

    /// Refuses invalid restoration state, unrepresentable storage or allocation.
    enum class StorageError : std::uint8_t {
        Malformed = 0,
        OverCapacity = 1,
        AllocationRefused = 2
    };

    /// Exact allocation requests for a newly restored, entirely raw store.
    ///
    /// Retained bytes include the fixed LineStore object and all owned backing
    /// capacities; allocator bookkeeping is excluded. No staging cells, old
    /// destination, folded forms or decode cache belong to the new store.
    /// Preparation allocates these owners directly, so its peak is retainedBytes.
    struct RestorationPlan {
        std::size_t rows = 0;
        std::size_t cells = 0;
        std::size_t blocks = 0;
        std::size_t descriptorBytes = 0;
        std::size_t indexBytes = 0;
        std::size_t blockDirectoryBytes = 0;
        std::size_t slabBytes = 0;
        std::size_t retainedBytes = 0;
    };

    /// Measures the existing row-boundary packing without allocating or parsing.
    /// The history wire validator feeds this same owner calculator while it
    /// validates each row; there is no second row or cell grammar.
    class RestoreLayout {
    public:
        RestoreLayout(int capacity, const Continuation& continuation);
        /// Adds one full physical row; blank rows still retain descriptors.
        [[nodiscard]] std::expected<void, StorageError> noteRow(int length, bool wrapped); // bool-ok: wrapped is row metadata, not an operation choice.
        /// Validates the complete continuation and returns exact owned capacity.
        [[nodiscard]] std::expected<RestorationPlan, StorageError> plan() const;

    private:
        int m_capacity;
        Continuation m_continuation;
        std::size_t m_rows = 0;
        std::size_t m_cells = 0;
        std::size_t m_blocks = 0;
        int m_used = 0;
        std::uint64_t m_completed = 0;
        std::optional<StorageError> m_error;
    };

    /// Validates borrowed rows and measures their detached replacement.
    [[nodiscard]] static std::expected<RestorationPlan, StorageError> restorationPlan(int capacity, std::span<const StoredLine> rows, const Continuation& continuation);

    /// Builds a complete private owner before publishing any history.
    ///
    /// The allowance is only this store's retained allocation requests, including
    /// its object header. It is not a terminal, caller-staging or old/new peak
    /// budget. Init, folding and cache allocation follow the store's ordinary
    /// policies.
    /// - Parameters:
    ///   - capacity: Exact configured descriptor capacity, positive.
    ///   - rows: Stable borrowed full physical rows, oldest first.
    ///   - continuation: The exact counters and front/open-line continuity.
    ///   - maxOwnedBytes: Explicit component allowance; no shipping cap implied.
    /// - Returns: A detached complete store or refusal with no caller mutation.
    [[nodiscard]] static std::expected<std::unique_ptr<LineStore>, StorageError> tryRestore(int capacity, std::span<const StoredLine> rows, const Continuation& continuation,
        std::size_t maxOwnedBytes = std::numeric_limits<std::size_t>::max());

    /// Measures all current owned backing capacities, excluding this object.
    /// Includes directory spare slots, folded vector capacities, parked slab and
    /// all cache slabs, including unoccupied reusable cache entries.
    [[nodiscard]] std::expected<std::size_t, StorageError> ownedBackingBytes() const;

    /// Restores retained rows into an initialized empty store.
    ///
    /// Cells are copied before returning. Their external cluster, hyperlink
    /// and run-text owners must be restored with the containing terminal.
    /// Invalid metadata, rows or refused detached allocation leave it unchanged.
    /// - Parameters:
    ///   - rows: Every retained row, oldest first, with stable borrowed cells.
    ///   - continuation: The counters that name those exact rows.
    /// - Returns: False for a nonempty store, capacity overflow, inconsistent
    ///   input or allocation refusal.
    [[nodiscard]] bool restore(std::span<const StoredLine> rows, const Continuation& continuation);

    // ---- Readers on another thread -------------------------------------
    //
    // History that has left the screen does not change: a raw block is only
    // appended to, past what any reader has seen, and a folded block never
    // changes after its fold. So a reader on another thread, the search
    // worker, can read a block without the model lock, provided nothing frees
    // or rewrites the block under it. The gate is that guarantee, one hazard
    // per reader:
    //
    // - The reader, for each block, takes the gate's lock, publishes the
    //   block's sequence as its hazard, then checks that the block is not
    //   released (`oldestLiveSequence`) and that no other mutation happened
    //   since its snapshot (`invalidation`). It reads, clears the hazard, frees
    //   anything eviction deferred to it, and unlocks.
    // - Eviction, the one mutation that runs at full speed beside a reader,
    //   publishes the released block's end as `oldestLiveSequence`, then reads
    //   the hazard. Both are sequentially consistent, so either the reader sees
    //   the block released and skips it, or eviction sees the hazard and hands
    //   the block's memory to the reader to free instead of parking it.
    // - Every other mutation that rewrites or frees history memory (an unfold,
    //   `dropNewest`, `prepend`, `clear`, `restore`, `dispose`) bumps
    //   `invalidation` and takes the gate's lock once, which waits for at most
    //   one block read, with the waiter's priority donated to the reader.
    // - A fold changes a raw block's form and reuses its slab, so it waits
    //   while any reader is registered: `foldColdBlocks` answers that work
    //   remains, and the quiet pass asks again.
    //
    // The gate is shared rather than owned, so a reader holding one survives
    // the store being destroyed or replaced: the destruction quiesces the gate
    // and the reader, finding it invalidated, snapshots the terminal again.

    /// The facts of a stored row a reader needs without the descriptor ring.
    struct RowFacts {
        std::uint16_t length = 0;
        std::uint8_t isWrapped : 1 = 0;
        std::uint8_t needsShaping : 1 = 0;
        /// The row's cells are unwritten; its bytes are at the same offset in
        /// the region's `text` as its cells in `cells`.
        std::uint8_t isCompact : 1 = 0;
        std::uint8_t lineAttribute = 0;
    };
    static_assert(sizeof(RowFacts) == 4, "a raw row's facts copy at four bytes");

    /// One ordinal of a folded block: where its row form starts, and its facts.
    struct FormEntry {
        std::uint32_t offset = 0;
        RowFacts facts;
    };

private:
    struct FoldedForms;

public:
    /// What the store and its readers agree on, shared by both.
    class ReaderGate {
    public:
        ReaderGate() = default;
        /// Frees whatever eviction still had deferred to a reader.
        ~ReaderGate();
        ReaderGate(const ReaderGate&) = delete;
        ReaderGate& operator=(const ReaderGate&) = delete;

        /// Frees whatever eviction deferred to the reader.
        void releaseDeferred();

        std::atomic<std::uint32_t> hazardSequence { 0 };
        std::atomic<std::uint32_t> oldestLiveSequence { 0 };
        std::atomic<std::uint32_t> invalidation { 0 };
        std::atomic<int> readerCount { 0 };
        std::unique_ptr<PAL::PriorityInheritingLock> lock;
        std::atomic<Cell*> deferredSlab { nullptr };
        std::atomic<std::uint8_t*> deferredText { nullptr };
        std::atomic<FoldedForms*> deferredForms { nullptr };
    };

    /// One run of a snapshot's rows that share their memory: a block's live
    /// rows, or a run of blank rows no block backs.
    struct SnapshotRegion {
        std::uint64_t firstOrdinal = 0;
        std::uint64_t endOrdinal = 0;
        /// The block's identity, which the gate's hazard names; zero for blank
        /// rows outside every block, which have no memory to protect.
        std::uint32_t sequence = 0;
        bool isFolded = false;
        /// A raw region's first stored cell; its rows' cells follow in order.
        const Cell* cells = nullptr;
        /// A raw region's text slab, where a compact row's bytes sit at the
        /// offset its cells have from `cells`; null when no row is compact.
        const std::uint8_t* text = nullptr;
        /// A folded region's forms, entries and pens, entries indexed from
        /// `entryOrdinal`.
        std::span<const std::uint8_t> forms;
        std::span<const FormEntry> entries;
        std::span<const Pen> pens;
        std::uint64_t entryOrdinal = 0;
        /// A raw or blank region's facts in `Snapshot::facts`, once copied.
        std::uint32_t factsOffset = 0;
        bool hasFacts = false;
    };

    /// The store as one reader sees it: the block directory and numbering at
    /// one moment, captured in O(blocks) under the model lock.
    ///
    /// Owned by the reader and reused across captures, so a steady reader
    /// allocates nothing.
    struct Snapshot {
        Snapshot() = default;
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        /// Ends the snapshot's registration as a reader.
        ~Snapshot() { releaseSnapshot(*this); }

        std::shared_ptr<ReaderGate> gate;
        /// A lock the reader made, for a gate no reader has used yet: the
        /// store itself never asks the platform for one.
        std::unique_ptr<PAL::PriorityInheritingLock> spareLock;
        std::uint32_t invalidation = 0;
        std::uint64_t frontOrdinal = 0;
        std::uint64_t nextOrdinal = 0;
        std::uint64_t headLine = 0;
        FrontLine front { 0, LineContinuity::StartsLine };
        FallibleVector<SnapshotRegion> regions;
        FallibleVector<RowFacts> facts;
        bool isRegistered = false;

        /// Whether the store has changed in a way that invalidates the regions.
        [[nodiscard]] bool isStale() const { return !gate || gate->invalidation.load(std::memory_order_acquire) != invalidation; }
    };

    /// A half-open range of completed logical line numbers.
    struct SnapshotRange {
        std::uint64_t firstLine = 0;
        std::uint64_t endLine = 0;
    };

    /// Captures the store for a reader. The caller holds the model lock.
    ///
    /// The directory is O(blocks). The facts of raw and blank rows are copied
    /// newest first up to `maxStartFacts`; older raw regions are left for
    /// ``captureRegionFacts``. The snapshot registers as a reader, which holds
    /// folding off until ``releaseSnapshot``. A gate no reader has used takes
    /// the snapshot's `spareLock`; without one the capture fails. A range
    /// captures only its intersecting blocks and excludes the open line.
    ///
    /// - Parameters:
    ///   - snapshot: Receives the capture; its storage is reused.
    ///   - maxStartFacts: The most row facts this call copies.
    ///   - range: Completed lines to capture, clipped to retained history;
    ///     no value captures the whole store, including its open prefix.
    /// - Returns: False when storage for the capture was refused.
    [[nodiscard]] bool captureSnapshot(Snapshot& snapshot, std::size_t maxStartFacts, std::optional<SnapshotRange> range = std::nullopt) const;

    /// Copies the facts of a region a snapshot left uncopied. The caller holds
    /// the model lock.
    ///
    /// - Parameters:
    ///   - snapshot: A snapshot of this store, not stale.
    ///   - regionIndex: The region.
    /// - Returns: False when the snapshot is stale, the region's rows have all
    ///   been evicted, or storage was refused; true when the facts are there.
    [[nodiscard]] bool captureRegionFacts(Snapshot& snapshot, std::size_t regionIndex) const;

    /// Ends a snapshot's registration as a reader, which lets folding resume.
    ///
    /// Any thread; the snapshot's regions go with it.
    ///
    /// - Parameter snapshot: The snapshot; reusable for the next capture.
    static void releaseSnapshot(Snapshot& snapshot);

    /// A reader's hold on one region's memory for the length of a read.
    ///
    /// Never held while taking the model lock, which a rare mutation holds
    /// while it waits for this.
    class RegionRead {
    public:
        /// Takes the gate for one region and checks it may still be read.
        ///
        /// - Parameters:
        ///   - snapshot: The reader's snapshot.
        ///   - region: One of its regions.
        RegionRead(const Snapshot& snapshot, const SnapshotRegion& region);
        /// Ends the read, freeing what eviction deferred to it.
        ~RegionRead();
        RegionRead(const RegionRead&) = delete;
        RegionRead& operator=(const RegionRead&) = delete;

        /// Whether the region's memory may be read: false when eviction
        /// released it or the store changed since the snapshot.
        [[nodiscard]] bool isValid() const { return m_isValid; }

    private:
        ReaderGate* m_gate = nullptr;
        bool m_isValid = false;
    };

    /// Decodes a folded region's rows `[from, to)` into `scratch`, back to
    /// back, calling `visit(ordinal, facts, cells)` for each, a blank row with
    /// no cells. Reads only the region's forms, so it needs no lock beyond a
    /// ``RegionRead``.
    ///
    /// - Returns: False when a form does not decode or `scratch` is too small.
    template<typename Visit>
    [[nodiscard]] static bool decodeFoldedRegion(const SnapshotRegion& region, std::span<Cell> scratch, std::uint64_t from, std::uint64_t to, Visit&& visit);

    /// Decodes one row form into `cells`, which hold its facts' length.
    [[nodiscard]] static bool decodeRowForm(std::span<const std::uint8_t> form, std::span<const Pen> pens, std::span<Cell> cells);

    /// Expands a compact row's bytes into default-pen cells.
    ///
    /// - Parameters:
    ///   - bytes: The row's `length` bytes, one code point each.
    ///   - cells: Where the row's cells go.
    ///   - length: The row's cell count.
    static void expandCompactRow(const std::uint8_t* bytes, Cell* cells, int length);

private:
    /// A folded block's storage: every line's row form back to back, one
    /// entry per ordinal of the block's span at the fold, blank lines
    /// included, so the table is indexed by ordinal, and the pens the forms'
    /// ids name. One heap object, so a reader's hazard can take it whole.
    struct FoldedForms {
        std::vector<std::uint8_t> forms;
        std::vector<FormEntry> entries;
        std::vector<Pen> pens;
    };

    struct Block {
        // The raw slab, or null once folded. Freed by parkSlab() or dispose().
        std::unique_ptr<Cell[]> base;
        // A byte per slab cell, allocated by the first compact row: a row
        // appended as bytes keeps them here at its cells' offset until a read
        // expands them. Null until then, and once folded.
        std::unique_ptr<std::uint8_t[]> text;
        int capacity = 0; // cells the slab can hold; 0 once folded, so the append test fails it
        int used = 0; // cells consumed
        int linesAppended = 0; // lines whose cells live in this block, evicted ones included
        // The ordinal the span starts at: that of the first line it backs, or
        // the front row's once a fold or a prepend trims the evicted lines off
        // the front block's span.
        std::uint64_t firstOrdinal = 0;
        std::uint32_t sequence = 0; // identity, for the decode cache and the reader gate; never 0
        bool isFolded = false;
        bool isFoldRefused = false; // holds a row the form cannot say, or more pens or a longer span than a fold takes
        std::unique_ptr<FoldedForms> folded;
    };

    /// Owner-local circular slots, with no hidden deque map or node growth.
    /// Slots are constructed in one exact raw allocation, avoiding array-cookie
    /// overhead for nontrivial Block objects. Only the allocator's bookkeeping
    /// lies outside the reported request size.
    class BlockDirectory {
    public:
        [[nodiscard]] bool empty() const { return !m_count; }
        [[nodiscard]] std::size_t size() const { return m_count; }
        [[nodiscard]] std::size_t capacity() const { return m_slots.get_deleter().capacity; }
        [[nodiscard]] Block& operator[](std::size_t index);
        [[nodiscard]] const Block& operator[](std::size_t index) const;
        [[nodiscard]] Block& front() { return (*this)[0]; }
        [[nodiscard]] Block& back() { return (*this)[m_count - 1]; }
        /// Holds exactly slots when growth is needed, unchanged on refusal.
        [[nodiscard]] bool reserve(std::size_t slots);
        /// Requires already reserved capacity; cannot allocate.
        void pushBack(Block&& block);
        /// Prepends an owned block without allocating.
        ///
        /// - Parameter block: The block moved into reserved directory capacity.
        void pushFront(Block&& block);
        void popFront();
        void popBack();
        void clear();
        void reset();
        void swap(BlockDirectory& other) noexcept;

    private:
        struct SlotDeleter {
            std::size_t capacity = 0;
            void operator()(Block* slots) const;
        };
        std::unique_ptr<Block, SlotDeleter> m_slots { nullptr, SlotDeleter {} };
        std::size_t m_start = 0;
        std::size_t m_count = 0;
    };

    /// One folded block held decoded.
    ///
    /// The slab outlives the entry's occupant so the next decode reuses it;
    /// `sequence` 0 is a free slot.
    struct DecodedBlock {
        std::uint32_t sequence = 0;
        /* owned */ Cell* cells = nullptr;
        std::uint64_t lastUse = 0;
    };

    // Block slab size in cells. >= maxColumns so any single line fits one block.
    static constexpr int blockCells = (maxColumns > 8192) ? maxColumns : 8192;
    static_assert(maxFoldSpan >= static_cast<std::uint64_t>(blockCells), "a block of one-cell rows with no blank between them folds");

    void evictOldest();
    /// Publishes already-owned rows/counters by swapping detached state.
    void swap(LineStore& other) noexcept;
    /// Takes the parked slab, or allocates one.
    ///
    /// - Returns: A slab of `blockCells` cells the caller owns.
    [[nodiscard]] Cell* takeSlab();
    /// Parks a text slab for the next `takeTextSlab`, or frees it when one
    /// is parked.
    ///
    /// - Parameter slab: The slab, which may be null.
    void parkTextSlab(std::uint8_t* slab);
    /// Writes a compact row's cells from its bytes, in place.
    ///
    /// - Parameters:
    ///   - line: A compact line of a raw block.
    ///   - block: The block whose slabs hold it.
    void expandLine(StoredLine& line, const Block& block) const;
    /// Parks a slab for the next `takeSlab`, or frees it when one is parked.
    ///
    /// - Parameter slab: The slab, which may be null.
    void parkSlab(Cell* slab);
    void releaseRetiredSlab();
    /// Gives up a block's slab and its decode-cache entry; the block's lines
    /// are all evicted or dropped by then.
    ///
    /// - Parameter block: The block, about to leave `m_blocks`.
    void releaseBlock(Block& block);
    /// Gives up the front block as eviction drains it, handing its memory to
    /// a reader that holds a hazard on it rather than reusing it.
    ///
    /// - Parameter block: The front block, about to leave `m_blocks`.
    void retireFrontBlock(Block& block);
    /// Waits for any reader's block read in progress and marks every reader's
    /// snapshot stale, before a mutation that rewrites or frees history.
    void quiesceReaders();
    /// Gives every block a fresh sequence in directory order, for a mutation
    /// that put blocks in front of others.
    void renumberBlockSequences();
    /// Numbers the newest row: it belongs to the open line, and an unwrapped
    /// row closes that line.
    ///
    /// - Parameter isWrapped: The newest row's wrap join, as stored.
    void noteStoredRow(bool isWrapped); // bool-ok: wrap and shaping are stored row metadata, as append takes them.
    /// Appends a row with cells into the back block's slack, when doing so
    /// neither allocates nor releases a block.
    ///
    /// - Returns: False, having changed nothing, when the general path must run.
    template<typename Store>
    [[nodiscard]] bool appendWithinBackBlock(int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping, const Store&, bool storesBytes); // bool-ok: wrap and shaping are stored row metadata, as append takes them.
    /// Commits a row through an infallible writer after securing its owners.
    ///
    /// Allocation refusal precedes eviction or counter changes.
    /// - Parameters:
    ///   - length: The stored cell count, from zero through maxColumns.
    ///   - lineAttribute: The row's line-size attribute.
    ///   - mark: The row's OSC 133 mark.
    ///   - wrapped: Whether the row continues into the following row.
    ///   - needsShaping: The captured row's shaping flag.
    ///   - store: A writer for the reserved destination cells, given the block
    ///     and the cells, returning whether it left them compact: bytes in the
    ///     block's text slab at the cells' offset, the cells unwritten.
    ///   - storesBytes: Whether the writer stores bytes, so the block needs its
    ///     text slab: secured here with the cell slab, before any eviction.
    template<typename Store>
    void appendStoredRow(int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping, const Store& store, bool storesBytes); // bool-ok: wrap and shaping are stored row metadata, as append takes them.

    /// Returns a descriptor with no decode: the numbering and the drops read
    /// only a line's facts, never its cells.
    ///
    /// - Parameter index: Oldest-first, below `m_count`.
    /// - Returns: The ring's descriptor for the line.
    [[nodiscard]] StoredLine& descriptor(int index) const { return m_ring[ringIndex(m_start + index)]; }
    /// Returns the ring index for a start plus an offset that are each below
    /// the capacity.
    ///
    /// The sum overshoots by at most one lap, so a conditional subtraction is
    /// the whole wrap, where `%` on a runtime capacity is a hardware divide
    /// per line a scroll captures.
    ///
    /// - Parameter sum: A ring index plus an offset, each below `m_capacity`.
    /// - Returns: The sum wrapped into the ring.
    [[nodiscard]] int ringIndex(int sum) const { return sum >= m_capacity ? sum - m_capacity : sum; }

    /// Returns the line at an index, decoding its block when it is folded and
    /// not held; both `at` overloads are this.
    ///
    /// - Parameter index: Oldest-first, as `at` takes it.
    /// - Returns: The line, or null out of range.
    [[nodiscard]] StoredLine* resolved(int index) const;
    /// Returns the index in `m_blocks` of the block whose span holds an ordinal.
    ///
    /// - Parameter ordinal: The ordinal of a line with cells.
    /// - Returns: The block's index.
    [[nodiscard]] std::size_t blockIndexHolding(std::uint64_t ordinal) const;
    /// Returns one past the last ordinal of a block's span: the next block's
    /// first, or the next ordinal to be stored for the back block.
    ///
    /// - Parameter index: The block's index in `m_blocks`.
    /// - Returns: The ordinal the span ends before.
    [[nodiscard]] std::uint64_t blockEnd(std::size_t index) const;
    /// Folds a raw block into the row form, or marks it refused.
    ///
    /// - Parameter index: The block's index in `m_blocks`.
    void foldBlock(std::size_t index);
    /// Unfolds a block into a slab of its own, so its cells can be written.
    ///
    /// - Parameter index: The block's index in `m_blocks`.
    void unfoldBlock(std::size_t index);
    /// Decodes a folded block's live lines into `scratch`, back to back in
    /// ordinal order, calling `visit(line, cells)` for each.
    ///
    /// - Parameters:
    ///   - index: The block's index in `m_blocks`.
    ///   - scratch: A slab of `blockCells` cells.
    ///   - visit: Called with each line's descriptor and its decoded cells.
    /// - Returns: False when a form does not decode, which a fold's check
    ///   keeps from ever being stored.
    template<typename Visit>
    [[nodiscard]] bool decodeBlock(std::size_t index, Cell* scratch, Visit&& visit) const;
    /// Points a folded line's descriptors at a cached decode of its block,
    /// decoding it when no entry holds it.
    ///
    /// - Parameter index: Oldest-first, naming a line with cells.
    void decodeLine(int index) const;
    /// Empties a cache entry, nulling the cells of the lines that point into it.
    ///
    /// - Parameter decoded: The entry.
    void forgetDecodedBlock(DecodedBlock& decoded) const;
    /// Empties the cache entry a block holds, if any, leaving the descriptors
    /// to the caller: an evicted block has none, and an unfold re-points them.
    ///
    /// - Parameter sequence: The block's identity.
    void dropDecodedBlock(std::uint32_t sequence) const;

    // One index entry per `lineIndexStride` rows, keyed by the row's ordinal:
    // the number of the line that row belongs to. The ring of entries spans
    // more rows than the store holds, so a live entry is never overwritten.
    static constexpr std::uint64_t lineIndexStride = 64;

    BlockDirectory m_blocks;
    // One drained slab parked for reuse so steady-state flood recycles a
    // 256 KiB allocation per block cycle instead of a new/delete pair.
    // Freed on clear/dispose/dropNewest so an idle store holds no spare slab.
    /* owned */ Cell* m_retiredSlab = nullptr; // parked slab for reuse; freed on clear/dispose/dropNewest
    /* owned */ std::uint8_t* m_retiredText = nullptr; // parked text slab, freed with m_retiredSlab
    std::unique_ptr<StoredLine[]> m_ring;
    int m_capacity = 0;
    int m_start = 0; // ring index of the oldest line
    int m_count = 0;
    int m_frontEvicted = 0; // lines evicted from m_blocks[0]
    long long m_totalAppended = 0; // monotonic; lines ever appended (see totalAppended)
    std::uint32_t m_nextBlockSequence = 1;

    std::unique_ptr<std::uint64_t[]> m_lineIndex; // see lineIndexStride
    std::size_t m_lineIndexCount = 0;
    std::uint64_t m_nextOrdinal = 0; // ordinal of the next stored row; `dropNewest` rewinds it
    std::uint64_t m_frontLine = 0; // the line holding the front row
    std::uint64_t m_headLine = 0;
    std::uint64_t m_maxHeadLine = 0;
    LineContinuity m_frontContinuity = LineContinuity::StartsLine;

    // Last, after every field the append path touches per line, so those
    // fields keep their cache lines to themselves. The cache is a memo
    // over immutable folded forms, so a read may fill it: `mutable` is that
    // fact and nothing else.
    mutable std::array<DecodedBlock, decodedBlockCacheCapacity> m_decoded {};
    mutable std::uint64_t m_decodeClock = 0;
    // Read once per released block and on the rare mutations, never per line.
    std::shared_ptr<ReaderGate> m_gate;
};

template<typename Visit>
bool LineStore::decodeFoldedRegion(const SnapshotRegion& region, std::span<Cell> scratch, std::uint64_t from, std::uint64_t to, Visit&& visit)
{
    if (!region.isFolded || from < region.entryOrdinal || to > region.endOrdinal || from > to)
        return false;
    std::size_t cursor = 0;
    for (std::uint64_t ordinal = from; ordinal < to; ++ordinal) {
        const std::size_t slot = static_cast<std::size_t>(ordinal - region.entryOrdinal);
        if (slot >= region.entries.size())
            return false;
        const FormEntry& entry = region.entries[slot];
        const std::size_t length = entry.facts.length;
        if (!length) {
            visit(ordinal, entry.facts, static_cast<const Cell*>(nullptr));
            continue;
        }
        if (length > scratch.size() - cursor)
            return false;
        const std::size_t formEnd = slot + 1 < region.entries.size() ? region.entries[slot + 1].offset : region.forms.size();
        if (entry.offset > formEnd || formEnd > region.forms.size())
            return false;
        const std::span<Cell> cells = scratch.subspan(cursor, length);
        if (!decodeRowForm(region.forms.subspan(entry.offset, formEnd - entry.offset), region.pens, cells))
            return false;
        visit(ordinal, entry.facts, static_cast<const Cell*>(cells.data()));
        cursor += length;
    }
    return true;
}

} // namespace Ubiquitty
