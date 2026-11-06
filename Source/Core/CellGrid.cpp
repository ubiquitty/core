// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// CellGrid.cpp — implementation of Ubiquitty::CellGrid + C bridge.
//
// The PaintSession iterator walks rows; for each row with a cell != m_bbShadow (or selectionDirty), it
// yields one same-pen contiguous run. AckRun copies cells_ → m_bbShadow for that run, then advances.
// Per-run (not per-row): the bbFlush rect must exactly equal the painted area, so sparse dirty rows yield
// one rect per run and never blit stale gap pixels. Same-pen runs: the PAL sets up color/font once per run.

#include "Core/CellGrid.h"

#include <cstdlib>

#include "Core/CellContent.h"
#include "Core/CellPen.h"
#include "Core/CellScan.h"
#include "Core/Unicode/Joining.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>

#endif

namespace Ubiquitty {

// --- Pen / Cell comparison ----------------------------------------

// Cells are POD with no padding holes (every spare byte is a real field —
// see Cell.h), so a raw memcmp over the whole struct is byte-exact.
// The two halves of a multi-column run. A constant rather than a braced list at
// each use: the list form constructs an OptionSet per evaluation, and both uses
// below sit in per-cell loops.
static constexpr Ubiquitty::VT::AttributeSet multicellHalves { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation };

static inline bool cellEqual(const Cell& a, const Cell& b) { return std::memcmp(&a, &b, sizeof(Cell)) == 0; }

// Broadcast-fill count cells with blank. The pattern is loaded once, so `blank` must not alias dst (no
// caller does). Cached stores, not non-temporal ones: erased rows are live-screen rows read again at once
// (equality probe, paint, scrollback).
static inline void fillBlank(Cell* dst, const Cell& blank, int count)
{
    if (count <= 0)
        return;
#if defined(__aarch64__)
    static_assert(sizeof(Cell) == 32, "the fill kernel walks 32-byte cells");
    const uint8_t* patternBytes = reinterpret_cast<const uint8_t*>(&blank);
    const uint8x16_t patternLow = vld1q_u8(patternBytes);
    const uint8x16_t patternHigh = vld1q_u8(patternBytes + 16);
    uint8_t* destinationBytes = reinterpret_cast<uint8_t*>(dst);
    int index = 0;
    for (; index + 2 <= count; index += 2) {
        asm volatile("stp %q0, %q1, [%2]\n\t"
                     "stp %q0, %q1, [%2, #32]"
            :
            : "w"(patternLow), "w"(patternHigh), "r"(destinationBytes)
            : "memory");
        destinationBytes += 64;
    }
    if (index < count) {
        asm volatile("stp %q0, %q1, [%2]" : : "w"(patternLow), "w"(patternHigh), "r"(destinationBytes) : "memory");
    }
#else
    constexpr int fillBlankDoublingThreshold = 128;
    if (count <= fillBlankDoublingThreshold) {
        for (int i = 0; i < count; ++i)
            dst[i] = blank;
        return;
    }
    dst[0] = blank;
    int filled = 1;
    while (filled < count) {
        int chunk = filled < count - filled ? filled : count - filled;
        std::memcpy(dst + filled, dst, std::size_t(chunk) * sizeof(Cell));
        filled += chunk;
    }
#endif
}

// Canonical blank (defaultBlankCell in Cell.h) so CellGrid, the parser, and Sync apply fill identically.
static inline Cell blankCell() { return defaultBlankCell; }

// Run boundaries: two adjacent dirty cells coalesce iff pen + attributes match, masking out Wide/WideContinuation.
// Attribute::Emoji is not masked so an emoji breaks the run, isolating its glyph-atlas
// Pause/Resume to the emoji boundary instead of contaminating a whole mixed run.
static constexpr uint16_t attrRunBreakMask = static_cast<uint16_t>(~(std::to_underlying(Ubiquitty::VT::Attribute::Wide) | std::to_underlying(Ubiquitty::VT::Attribute::WideContinuation)));

static inline bool sameRunPen(const Cell& a, const Cell& b)
{
    const Pen& penA = cellPen(a);
    const Pen& penB = cellPen(b);
    if ((penA.attributes.toRaw() & attrRunBreakMask) != (penB.attributes.toRaw() & attrRunBreakMask))
        return false;
    if (penA.foregroundIndex != penB.foregroundIndex || penA.backgroundIndex != penB.backgroundIndex)
        return false;
    if (penA.foregroundIndex == colorRGB) {
        if (penA.foregroundRed != penB.foregroundRed || penA.foregroundGreen != penB.foregroundGreen || penA.foregroundBlue != penB.foregroundBlue)
            return false;
    }
    if (penA.backgroundIndex == colorRGB) {
        if (penA.backgroundRed != penB.backgroundRed || penA.backgroundGreen != penB.backgroundGreen || penA.backgroundBlue != penB.backgroundBlue)
            return false;
    }
    return true;
}

// --- CellGrid -------------------------------------------------------

CellGrid::CellGrid()
    : m_rows(0)
    , m_columns(0)
    , m_rowCapacity(0)
    , m_columnCapacity(0)
    , m_activeIndex(0)
    , m_bbShadow(nullptr)
    , m_bbShadowValid(false)
    , m_fullRedrawRequested(false)
    , m_anyMutationsSinceLastPaint(false)
    , m_blinkHint(false)
    , m_blinkHintGeneration(0)
    , m_hasSizedCellHint(false)
#ifndef NDEBUG
    , m_paintInvariantBroken(false)
#endif
    , m_selectionDirty(nullptr)
    , m_selectionStride(0)
    , m_anySelectionDirty(false)
    , m_anyDirtyInRow(nullptr)
    , m_rowGeneration(nullptr)
    , m_dirtyRangeValid(false)
    , m_dirtyRangeTop(0)
    , m_dirtyRangeBottom(0)
{
    m_screenStore[0] = m_screenStore[1] = nullptr;
    m_rowPtrs[0] = m_rowPtrs[1] = nullptr;
    m_rowState[0] = m_rowState[1] = nullptr;

    m_rowOrigin[0] = m_rowOrigin[1] = 0;
    m_defaultBlank = blankCell();
    m_haveDefaultBlank = true;
    m_pendingScroll.valid = false;
    m_pendingScroll.top = 0;
    m_pendingScroll.bot = 0;
    m_pendingScroll.delta = 0;
    m_pendingScroll.activeIndex = 0;
    // No allocation here — buffers are sized in init(). Entry points bounds-check against m_rows/m_columns
    // (both 0), so a fresh CellGrid is safe to query before init().
}

CellGrid::~CellGrid() { freeBuffers(); }

namespace {

// The caller refuses a request beyond the ceiling, which keeps the granule
// addition in the ceiling's domain.
int roundUpCapacity(int needed, int ceiling)
{
    constexpr int capacityGranule = 16;
    if (needed <= 0)
        return 1;
    const int rounded = (needed + capacityGranule - 1) / capacityGranule * capacityGranule;
    return std::max(needed, std::min(rounded, ceiling));
}

std::expected<std::size_t, CellGrid::BackingStorageError> backingArrayBytes(int rows, int columns)
{
    const auto rowCapacity = static_cast<std::size_t>(rows);
    const auto columnCapacity = static_cast<std::size_t>(columns);
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (!rowCapacity || !columnCapacity || columnCapacity > maximum / rowCapacity)
        return std::unexpected(CellGrid::BackingStorageError::OverCapacity);
    const auto cells = rowCapacity * columnCapacity;
    const auto selectionStride = columnCapacity / 8 + (columnCapacity % 8 != 0);
    std::size_t bytes = 0;
    const auto addArray = [&](std::size_t count, std::size_t elementBytes) {
        if (count > maximum / elementBytes)
            return false;
        const auto arrayBytes = count * elementBytes;
        if (arrayBytes > maximum - bytes)
            return false;
        bytes += arrayBytes;
        return true;
    };
    // Two screens and their row rings/state, then one renderer shadow and
    // selection, dirtiness, and generation arrays. These are all the owners
    // allocBuffers constructs; lazy rendering caches have separate lifetimes.
    for (int screen = 0; screen < 2; ++screen) {
        if (!addArray(cells, sizeof(Cell)) || !addArray(rowCapacity, sizeof(Cell*)) || !addArray(rowCapacity, sizeof(RowState)))
            return std::unexpected(CellGrid::BackingStorageError::OverCapacity);
    }
    if (!addArray(cells, sizeof(Cell)) || !addArray(rowCapacity, selectionStride)
        || !addArray(rowCapacity, sizeof(bool)) || !addArray(rowCapacity, sizeof(std::uint32_t)))
        return std::unexpected(CellGrid::BackingStorageError::OverCapacity);
    return bytes;
}

} // namespace

std::expected<CellGrid::BackingStoragePlan, CellGrid::BackingStorageError> CellGrid::backingStoragePlan(int rows, int columns) const
{
    // No geometry holds more than maxRows by maxColumns, so a larger request
    // asks for storage nothing can use.
    if (rows > maxRows || columns > maxColumns)
        return std::unexpected(BackingStorageError::OverCapacity);
    const bool hasBacking = m_screenStore[0] != nullptr;
    const bool shouldGrow = !hasBacking || rows > m_rowCapacity || columns > m_columnCapacity;
    const int rowCapacity = shouldGrow ? roundUpCapacity(std::max(rows, m_rowCapacity), maxRows) : m_rowCapacity;
    const int columnCapacity = shouldGrow ? roundUpCapacity(std::max(columns, m_columnCapacity), maxColumns) : m_columnCapacity;
    const auto retained = backingArrayBytes(rowCapacity, columnCapacity);
    if (!retained)
        return std::unexpected(retained.error());
    std::size_t peak = *retained;
    if (hasBacking && shouldGrow) {
        const auto previous = backingArrayBytes(m_rowCapacity, m_columnCapacity);
        if (!previous || *previous > std::numeric_limits<std::size_t>::max() - peak)
            return std::unexpected(BackingStorageError::OverCapacity);
        peak += *previous;
    }
    return BackingStoragePlan { rowCapacity, columnCapacity, *retained, peak };
}

// Allocate flat stores + row-pointer rings at the given capacity. Assumes members are null (caller frees).
// Leaves cell content uninitialized; init()/ensureCapacity() seed it.
bool CellGrid::allocBuffers(const BackingStoragePlan& plan)
{
    const int rowCapacity = plan.rowCapacity;
    const int columnCapacity = plan.columnCapacity;
    // backingStoragePlan produces capacities in this range; checking it where
    // the arrays are sized keeps a negative int from converting to a huge count.
    if (rowCapacity < 1 || rowCapacity > maxRows || columnCapacity < 1 || columnCapacity > maxColumns)
        return false;
    const int selectionStride = columnCapacity / 8 + (columnCapacity % 8 != 0);
    const std::size_t cells = std::size_t(rowCapacity) * std::size_t(columnCapacity);

    // Every buffer into an owner first, and nothing installed until all of them
    // are held. The Core builds without exceptions, so a throwing allocation
    // that fails is std::terminate — a window drag under memory pressure taking
    // the process down. A grid that cannot grow keeps the geometry it has,
    // which is the degrade contract the reflow path beside it already states.
    std::unique_ptr<Cell[]> screenStore[2];
    std::unique_ptr<Cell*[]> rowPtrs[2];
    std::unique_ptr<RowState[]> rowState[2];
    for (int s = 0; s < 2; ++s) {
        screenStore[s].reset(new (std::nothrow) Cell[cells]);
        rowPtrs[s].reset(new (std::nothrow) Cell*[std::size_t(rowCapacity)]);
        rowState[s].reset(new (std::nothrow) RowState[std::size_t(rowCapacity)]);
        if (!screenStore[s] || !rowPtrs[s] || !rowState[s])
            return false;
    }
    std::unique_ptr<Cell[]> shadow(new (std::nothrow) Cell[cells]);
    std::unique_ptr<unsigned char[]> selectionDirty(new (std::nothrow) unsigned char[std::size_t(rowCapacity) * std::size_t(selectionStride)]);
    std::unique_ptr<bool[]> anyDirtyInRow(new (std::nothrow) bool[std::size_t(rowCapacity)]);
    std::unique_ptr<uint32_t[]> rowGeneration(new (std::nothrow) uint32_t[std::size_t(rowCapacity)]()); // () zero-inits
    if (!shadow || !selectionDirty || !anyDirtyInRow || !rowGeneration)
        return false;

    m_rowCapacity = rowCapacity;
    m_columnCapacity = columnCapacity;
    m_selectionStride = selectionStride;
    for (int s = 0; s < 2; ++s) {
        m_screenStore[s] = screenStore[s].release();
        m_rowPtrs[s] = rowPtrs[s].release();
        m_rowState[s] = rowState[s].release();
        // Conservatively claim the full row until cells are seeded, so no premature trim.
        for (int r = 0; r < rowCapacity; ++r)
            m_rowState[s][r].filled = columnCapacity;
        for (int r = 0; r < rowCapacity; ++r)
            m_rowState[s][r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
    }
    m_bbShadow = shadow.release();
    m_selectionDirty = selectionDirty.release();
    m_anyDirtyInRow = anyDirtyInRow.release();
    m_rowGeneration = rowGeneration.release();
    resetRowPointers();
    return true;
}

void CellGrid::freeBuffers()
{
    for (int s = 0; s < 2; ++s) {
        delete[] m_screenStore[s];
        m_screenStore[s] = nullptr;
        delete[] m_rowPtrs[s];
        m_rowPtrs[s] = nullptr;
        delete[] m_rowState[s];
        m_rowState[s] = nullptr;
    }
    delete[] m_bbShadow;
    m_bbShadow = nullptr;
    delete[] m_selectionDirty;
    m_selectionDirty = nullptr;
    delete[] m_anyDirtyInRow;
    m_anyDirtyInRow = nullptr;
    delete[] m_rowGeneration;
    m_rowGeneration = nullptr;
    delete[] m_shapedRowSlots; // lazy cache — nullptr when never populated
    m_shapedRowSlots = nullptr;
    delete[] m_shapedRowCells;
    m_shapedRowCells = nullptr;
    m_shapedRowCapacity = 0;
    m_rowOrigin[0] = m_rowOrigin[1] = 0;
    m_deferredBlankSlot[0] = m_deferredBlankSlot[1] = -1;
    m_deferredBlankCount[0] = m_deferredBlankCount[1] = 0;
    m_deferredBlankStart[0] = m_deferredBlankStart[1] = 0;
    clearDirtyRange();
    m_rowCapacity = m_columnCapacity = m_selectionStride = 0;
}

CellGrid::CapacityOutcome CellGrid::ensureCapacity(int rows, int columns, std::size_t maxPeakBackingBytes)
{
    if (m_screenStore[0] && rows <= m_rowCapacity && columns <= m_columnCapacity
        && maxPeakBackingBytes == std::numeric_limits<std::size_t>::max())
        return CapacityOutcome::AlreadySufficient;
    const auto plan = backingStoragePlan(rows, columns);
    if (!plan || plan->peakBytes > maxPeakBackingBytes)
        return CapacityOutcome::Refused;
    if (m_screenStore[0] && rows <= m_rowCapacity && columns <= m_columnCapacity)
        return CapacityOutcome::AlreadySufficient;
    // The copy below reads old rows as live content; materialize any deferred blank first.
    materializeCompactRows(0);
    materializeCompactRows(1);
    resolveDeferredBlankSlot(0);
    resolveDeferredBlankSlot(1);
    // Stash old buffers; reading old logical rows through the old ring preserves content across the realloc.
    Cell* oldS[2] = { m_screenStore[0], m_screenStore[1] };
    Cell** oldRP[2] = { m_rowPtrs[0], m_rowPtrs[1] };
    RowState* oldState[2] = { m_rowState[0], m_rowState[1] };
    int oldOrigin[2] = { m_rowOrigin[0], m_rowOrigin[1] };
    Cell* oldShadow = m_bbShadow;
    unsigned char* oldSelection = m_selectionDirty;
    bool* oldDirty = m_anyDirtyInRow;
    const int oldRows = m_rows;
    // Captured before allocBuffers overwrites m_columnCapacity — see the cpColumns comment below.
    const int oldColumnCapacity = m_columnCapacity;
    uint32_t* const oldGeneration = m_rowGeneration;
    m_screenStore[0] = m_screenStore[1] = nullptr;
    m_rowPtrs[0] = m_rowPtrs[1] = nullptr;

    m_rowState[0] = m_rowState[1] = nullptr;
    m_bbShadow = nullptr;
    m_selectionDirty = nullptr;
    m_anyDirtyInRow = nullptr;
    m_rowGeneration = nullptr;
    if (!allocBuffers(*plan)) {
        // Nothing was freed, so putting the stash back leaves the grid exactly
        // as it was found — the geometry it already had, still drawable.
        for (int s = 0; s < 2; ++s) {
            m_screenStore[s] = oldS[s];
            m_rowPtrs[s] = oldRP[s];
            m_rowState[s] = oldState[s];
        }
        m_bbShadow = oldShadow;
        m_selectionDirty = oldSelection;
        m_anyDirtyInRow = oldDirty;
        m_rowGeneration = oldGeneration;
        m_columnCapacity = oldColumnCapacity;
        return CapacityOutcome::Refused;
    }
    // Generations are not carried across a resize (the shaped cache is
    // epoch-invalidated below), so the old array is dropped rather than copied.
    delete[] oldGeneration;
    // Blank-fill the shadow, clear tracking arrays. Shadow/selectionDirty/anyDirty are not carried across
    // the realloc — resize() invalidates the shadow and clears selection anyway.
    const Cell blank = blankCell();
    for (int r = 0; r < m_rowCapacity; ++r)
        fillBlank(m_bbShadow + std::size_t(r) * m_columnCapacity, blank, m_columnCapacity);
    std::memset(m_selectionDirty, 0, std::size_t(m_rowCapacity) * std::size_t(m_selectionStride));
    std::memset(m_anyDirtyInRow, 0, std::size_t(m_rowCapacity) * sizeof(bool));
    clearDirtyRange();
    m_anySelectionDirty = false;
    // Preserve logical cell content (both screens) into the new identity ring; cells outside the copied
    // region read as the canonical default blank (the [filled, columns) tail invariant is memcmp-exact).
    if (oldS[0]) {
        const int cpRows = oldRows < m_rowCapacity ? oldRows : m_rowCapacity;
        // Copy up to the OLD COLUMN CAPACITY, not the old logical columns: a Modes::ReflowMode::Fixed shrink
        // narrows m_columns without touching cells beyond it, so real content can sit up to the previous
        // capacity while the logical width is temporarily narrower. Bounding to the logical width would
        // silently drop it on a capacity-exceeding realloc.
        const int cpColumns = oldColumnCapacity < m_columnCapacity ? oldColumnCapacity : m_columnCapacity;
        for (int s = 0; s < 2; ++s) {
            // Per surviving row: cell prefix + blank tail + metadata + tracked width. Metadata and the
            // tracked width must survive a grow (a FIXED-reflow resize relies on it, else a
            // shrink-then-grow-past-old-capacity cycle forgets a row had content past the new width).
            for (int r = 0; r < cpRows; ++r) {
                int slot = r;
                if (oldRows > 0) {
                    slot = oldOrigin[s] + r;
                    if (slot >= oldRows)
                        slot -= oldRows;
                }
                Cell* dst = m_screenStore[s] + std::size_t(r) * m_columnCapacity;
                std::memcpy(dst, oldRP[s][slot], std::size_t(cpColumns) * sizeof(Cell));
                if (cpColumns < m_columnCapacity)
                    fillBlank(dst + cpColumns, blank, m_columnCapacity - cpColumns);
                m_rowState[s][r].meta = oldState[s][slot].meta;
                int filled = oldState[s][slot].filled;
                if (filled > cpColumns)
                    filled = cpColumns;
                m_rowState[s][r].filled = filled;
            }
            // Rows past the surviving screen are all-blank (metadata/width keep the conservative seed).
            for (int r = cpRows; r < m_rowCapacity; ++r)
                fillBlank(m_screenStore[s] + std::size_t(r) * m_columnCapacity, blank, m_columnCapacity);
        }
        delete[] oldS[0];
        delete[] oldS[1];
        delete[] oldRP[0];
        delete[] oldRP[1];
        delete[] oldState[0];
        delete[] oldState[1];
        delete[] oldShadow;
        delete[] oldSelection;
        delete[] oldDirty;
    } else {
        // First allocation through this path: no content to carry, blank both stores whole.
        for (auto& screen : m_screenStore)
            for (int r = 0; r < m_rowCapacity; ++r)
                fillBlank(screen + std::size_t(r) * m_columnCapacity, blank, m_columnCapacity);
    }
    return CapacityOutcome::Grown;
}

void CellGrid::resetRowPointers()
{
    // Repointing remaps slot indexes; a deferred blank is slot-addressed.
    resolveDeferredBlankSlot(0);
    resolveDeferredBlankSlot(1);
    m_rowOrigin[0] = m_rowOrigin[1] = 0;
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < m_rowCapacity; ++r)
            m_rowPtrs[s][r] = m_screenStore[s] + std::size_t(r) * m_columnCapacity;
}

// Materialize a deferred recycle blank (see CellGrid.h): physically fill the
// recorded row's stale prefix with the default blank. Idempotent; the common
// caller is rowPtrAt's [[unlikely]] branch.
void CellGrid::resolveDeferredBlankSlot(int screen)
{
    const int slot = m_deferredBlankSlot[screen];
    if (slot < 0)
        return;
    int count = m_deferredBlankCount[screen];
    if (count > m_columnCapacity)
        count = m_columnCapacity;
    const int start = m_deferredBlankStart[screen];
    if (count > start && m_rowPtrs[screen] && m_rowPtrs[screen][slot])
        fillBlank(m_rowPtrs[screen][slot] + start, m_defaultBlank, count - start);
    m_deferredBlankSlot[screen] = -1;
    m_deferredBlankCount[screen] = 0;
    m_deferredBlankStart[screen] = 0;
}

void CellGrid::materializeCompactRow(int screen, int slot)
{
    if (!m_rowState[screen])
        return;
    assert(slot >= 0 && slot < m_rowCapacity);
    if (m_rowState[screen][slot].storage != RowStorage::DefaultASCII)
        return;
    assert(slot != m_deferredBlankSlot[screen]);
    auto& state = m_rowState[screen][slot];
    Cell* row = m_rowPtrs[screen][slot];
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(row);
    assert(state.filled >= 0 && state.filled <= m_columns);
    // Expand backwards: each destination begins at 32 * column, so it can
    // overwrite only compact bytes that have already been consumed.
    for (int column = state.filled - 1; column >= 0; --column) {
        Cell cell = m_defaultBlank;
        cell.codePoint = bytes[column];
        row[column] = cell;
    }
    fillBlank(row + state.filled, m_defaultBlank, m_columns - state.filled);
    state.storage = RowStorage::Cells;
}

void CellGrid::materializeCompactRows(int screen)
{
    if (!m_rowState[screen])
        return;
    for (int slot = 0; slot < m_rowCapacity; ++slot)
        materializeCompactRow(screen, slot);
}

bool CellGrid::writeCompactASCIIRun(int slot, int row, int firstColumn, int lastColumn, const Cell& cellTemplate, const char* source, CompactWriteComparison comparison)
{
    auto& state = m_rowState[m_activeIndex][slot];
    const bool isRecycled = slot == m_deferredBlankSlot[m_activeIndex]
        && m_deferredBlankStart[m_activeIndex] == 0 && m_deferredBlankCount[m_activeIndex] >= m_columns;
    if (state.storage != RowStorage::DefaultASCII && !isRecycled)
        return false;
    Cell* backing = m_rowPtrs[m_activeIndex][slot];
    const std::less<const void*> precedes;
    const auto overlapsBacking = [&](const void* first, const void* end) {
        return precedes(first, backing + m_columnCapacity) && precedes(backing, end);
    };
    // The ordinary kernel observes borrowed input between complete-cell
    // stores, and deferred flanks are filled before loading its template.
    if (overlapsBacking(&cellTemplate, &cellTemplate + 1)
        || overlapsBacking(source, source + (lastColumn - firstColumn + 1)))
        return false;
    Cell pattern = cellTemplate;
    pattern.codePoint = 0;
    if (!isDefaultBlank(pattern))
        return false;
    if (isRecycled) {
        const int count = std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        // Hidden columns remain full Cells. Finish the old blank obligation
        // beyond the visible row before replacing its representation.
        if (count > m_columns)
            fillBlank(backing + m_columns, m_defaultBlank, count - m_columns);
        m_deferredBlankSlot[m_activeIndex] = -1;
        m_deferredBlankStart[m_activeIndex] = 0;
        m_deferredBlankCount[m_activeIndex] = 0;
        state.storage = RowStorage::DefaultASCII;
    }
    auto* bytes = reinterpret_cast<std::uint8_t*>(backing);
    bool hasDifference = isRecycled || comparison == CompactWriteComparison::KnownDifferent;
    for (int column = firstColumn; !hasDifference && column <= lastColumn; ++column) {
        const auto old = column < state.filled ? bytes[column] : 0;
        hasDifference = old != static_cast<std::uint8_t>(source[column - firstColumn]);
    }
    if (!hasDifference)
        return true;
    if (firstColumn > state.filled)
        std::memset(bytes + state.filled, 0, static_cast<std::size_t>(firstColumn - state.filled));
    std::memcpy(bytes + firstColumn, source, static_cast<std::size_t>(lastColumn - firstColumn + 1));
    state.filled = std::max(state.filled, lastColumn + 1);
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    return true;
}

int CellGrid::physicalRowSlot(int screen, int r) const
{
    if (screen < 0 || screen > 1 || r < 0 || r >= m_rows)
        return r;
    int slot = m_rowOrigin[screen] + r;
    if (slot >= m_rows)
        slot -= m_rows;
    return slot;
}

void CellGrid::rotateRowRingToOrigin(int screen)
{
    if (screen < 0 || screen > 1 || m_rows <= 1)
        return;
    // Before the origin test, not after: a deferred blank is slot-addressed, and
    // every caller here is about to move rows between slots by some means. An
    // origin already at 0 means this function has no rotation to do, not that the
    // caller has none — the banded scrollRegion branch reorders m_rowPtrs with
    // memmove immediately after calling this, and origin 0 with a deferral live is
    // ordinary: every m_rows-th full-viewport scroll wraps the origin back to 0 and
    // records a fresh deferral in the same step.
    resolveDeferredBlankSlot(screen);
    if (m_rowOrigin[screen] == 0)
        return;
    const int origin = m_rowOrigin[screen];
    std::rotate(m_rowPtrs[screen], m_rowPtrs[screen] + origin, m_rowPtrs[screen] + m_rows);
    std::rotate(m_rowState[screen], m_rowState[screen] + origin, m_rowState[screen] + m_rows);
    m_rowOrigin[screen] = 0;
}

void CellGrid::init(int rows, int columns)
{
    materializeCompactRows(0);
    materializeCompactRows(1);
    if (rows < 1)
        rows = 1;
    if (columns < 1)
        columns = 1;
    if (rows > maxRows)
        rows = maxRows;
    if (columns > maxColumns)
        columns = maxColumns;
    if (!m_screenStore[0] || rows > m_rowCapacity || columns > m_columnCapacity) {
        // Through ensureCapacity so the allocation happens before anything is
        // freed. A refusal with buffers already in hand leaves them, and the
        // geometry clamps to what they cover; a refusal with none leaves a grid
        // that cannot be initialised, which the caller reads back as zero rows.
        if (ensureCapacity(rows, columns) == CapacityOutcome::Refused) {
            if (!m_screenStore[0])
                return;
            if (rows > m_rowCapacity)
                rows = m_rowCapacity;
            if (columns > m_columnCapacity)
                columns = m_columnCapacity;
        }
    }
    m_rows = rows;
    m_columns = columns;
    // Seed both screens + shadow with the canonical blank: build one blank row, memcpy across every row.
    const Cell blank = blankCell();
    Cell* tmpl = m_screenStore[0]; // row 0 = scratch template
    fillBlank(tmpl, blank, m_columnCapacity);
    const std::size_t rowBytes = std::size_t(m_columnCapacity) * sizeof(Cell);
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < m_rowCapacity; ++r)
            if (!(s == 0 && r == 0))
                std::memcpy(m_screenStore[s] + std::size_t(r) * m_columnCapacity, tmpl, rowBytes);
    for (int r = 0; r < m_rowCapacity; ++r)
        std::memcpy(m_bbShadow + std::size_t(r) * m_columnCapacity, tmpl, rowBytes);
    resetRowPointers();
    // Whole screen is default blank → every row's live width is 0, and a blank screen carries NORMAL
    // metadata on every row (init may reuse a large allocation, so both are reset).
    for (auto& state : m_rowState)
        for (int r = 0; r < m_rowCapacity; ++r)
            state[r] = RowState { 0, RowMeta { lineAttributeNormal, 0, {}, 0 }, RowStorage::DefaultASCII };
    m_activeIndex = 0;
    m_bbShadowValid = true;
    m_fullRedrawRequested = false;
    std::memset(m_selectionDirty, 0, std::size_t(m_rowCapacity) * std::size_t(m_selectionStride));
    m_anySelectionDirty = false;
    m_pendingScroll.valid = false;
    m_anyMutationsSinceLastPaint = false;
    std::memset(m_anyDirtyInRow, 0, std::size_t(m_rowCapacity) * sizeof(bool));
    invalidateShapedRowCache(); // bulk content reset without per-row bumps
    clearDirtyRange();
}

void CellGrid::resize(int newRows, int newColumns, Ubiquitty::Modes::ReflowMode reflowMode)
{
    materializeCompactRows(0);
    materializeCompactRows(1);
    if (newRows < 1)
        newRows = 1;
    if (newColumns < 1)
        newColumns = 1;
    if (newRows > maxRows)
        newRows = maxRows;
    if (newColumns > maxColumns)
        newColumns = maxColumns;
    const int oldRows = m_rows;
    // Grow the backing store if the new geometry exceeds capacity, preserving cell content.
    const CapacityOutcome outcome = ensureCapacity(newRows, newColumns);
    // A refusal here cannot be absorbed: the caller derived its own geometry
    // before the call and uses it after, so a grid that silently took a smaller
    // one would leave those two disagreeing. Callers that can be refused ask
    // `reserveCapacity` first, which is where the decision is made once and
    // before anything depends on it; a refusal reaching this point leaves the
    // buffers as they are and the geometry clamped to what they cover, which is
    // the last thing that keeps a row index inside its own storage.
    if (outcome == CapacityOutcome::Refused) {
        if (newRows > m_rowCapacity)
            newRows = m_rowCapacity;
        if (newColumns > m_columnCapacity)
            newColumns = m_columnCapacity;
    }
    const bool didReallocate = outcome == CapacityOutcome::Grown;
    if (!didReallocate) {
        normalizeRowRing(0);
        normalizeRowRing(1);
    }
    m_rows = newRows;
    m_columns = newColumns;
    // Conservative content widths after a geometry change: claim the full new width per row so a scroll
    // before the next write copies/blanks the whole row. Skipped for FIXED reflow: nothing rewraps that
    // row's content, so stamping would misrecord a shrink as erasing content the cells still hold.
    if (reflowMode == Ubiquitty::Modes::ReflowMode::Reflow) {
        for (auto& state : m_rowState)
            for (int r = 0; r < m_rowCapacity; ++r)
                state[r].filled = newColumns;
    }
    // Rows revealed by a grow start with normal metadata: a within-capacity grow reuses m_rowState slots
    // that may hold a prior taller screen's DECDWL/DECDHL + wrap flags (which would render blanks at 2x).
    // Surviving rows keep their metadata. (A grow past capacity already reset revealed rows.)
    if (newRows > oldRows)
        for (auto& state : m_rowState)
            for (int r = oldRows; r < newRows; ++r)
                state[r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
    // Geometry change: BB pixels re-laid, so the shadow can't mirror the new BB. Force full dirty next paint.
    m_bbShadowValid = false;
    m_fullRedrawRequested = true;
    clearAllSelectionDirty();
    m_pendingScroll.valid = false;
    clearDirtyRange();
    invalidateShapedRowCache(); // row identities remap; drop shaped entries
    // Shrink: zero the out-of-range m_anyDirtyInRow tail so a later grow-back doesn't surface stale true
    // bits (harmless, but assertInvariants() would flag them).
    if (newRows < oldRows) {
        std::memset(m_anyDirtyInRow + newRows, 0, std::size_t(oldRows - newRows) * sizeof(bool));
    }
}

// True iff `c` byte-equals the canonical default blank (the value the [filled, columns) tail invariant is
// stated against).
bool CellGrid::isDefaultBlank(const Cell& c) const { return m_haveDefaultBlank && std::memcmp(&c, &m_defaultBlank, sizeof(Cell)) == 0; }

// Raise row ri's content-width bound to at least `width` (active screen).
void CellGrid::noteFilled(int ri, int width)
{
    if (width > m_columns)
        width = m_columns;
    if (width > rowFilledAt(m_activeIndex, ri))
        rowFilledAt(m_activeIndex, ri) = width;
}

int CellGrid::rowContentWidth(Row r) const
{
    return rowContentWidth(static_cast<ScreenMode>(m_activeIndex), r);
}

int CellGrid::rowContentWidth(ScreenMode screen, Row r) const
{
    const int ri = r.value();
    if (screen > ScreenMode::Alternate || ri < 0 || ri >= m_rows)
        return m_columns;
    int w = rowFilledAt(static_cast<int>(screen), ri);
    if (w < 0)
        w = 0;
    if (w > m_columns)
        w = m_columns;
    return w;
}

bool CellGrid::restoreRowContentWidth(ScreenMode screen, Row row, int width)
{
    if (screen > ScreenMode::Alternate || row.value() < 0 || row.value() >= m_rows || width < 0 || width > m_columns)
        return false;
    const auto* cells = rowPtrAt(static_cast<int>(screen), row.value());
    for (int column = width; column < m_columns; ++column) {
        if (!isDefaultBlank(cells[column]))
            return false;
    }
    rowFilledAt(static_cast<int>(screen), row.value()) = width;
    return true;
}

// --- Per-row metadata -------------------------------------------------
// rowMeta(r) is the active-screen shorthand over rowMetaAt (inline, in
// CellGrid.h). The setters write the active screen's record.
RowMeta& CellGrid::rowMeta(int r) { return rowMetaAt(m_activeIndex, r); }
const RowMeta& CellGrid::rowMeta(int r) const { return rowMetaAt(m_activeIndex, r); }

void CellGrid::setLineAttribute(int r, uint8_t a)
{
    if (r < 0 || r >= m_rowCapacity || !m_rowState[m_activeIndex])
        return;
    RowMeta& m = rowMetaAt(m_activeIndex, r);
    if (m.lineAttribute == a)
        return;
    m.lineAttribute = a;
    // DECDWL/DECDHL changes the row's geometry; force a repaint of it.
    if (r < m_rows)
        bumpRow(r);
}

void CellGrid::setRowWrapped(int r, bool w)
{
    if (r < 0 || r >= m_rowCapacity || !m_rowState[m_activeIndex])
        return;
    rowMetaAt(m_activeIndex, r).set(Ubiquitty::RowFlag::Wrapped, w);
}

void CellGrid::setRowMark(int r, uint8_t mark)
{
    if (r < 0 || r >= m_rowCapacity || !m_rowState[m_activeIndex])
        return;
    rowMetaAt(m_activeIndex, r).oscMark = mark;
}

void CellGrid::resetRowMeta(int r)
{
    if (r < 0 || r >= m_rowCapacity || !m_rowState[m_activeIndex])
        return;
    rowMetaAt(m_activeIndex, r) = RowMeta { lineAttributeNormal, 0, {}, 0 };
}

void CellGrid::setCell(Row r, Column c, const Cell& cell)
{
    const int ri = r.value();
    const int ci = c.value();
    assert(ri >= 0 && ri < m_rows);
    assert(ci >= 0 && ci < m_columns);
    if (ri < 0 || ri >= m_rows || ci < 0 || ci >= m_columns)
        return;
    // Resolve the row slot once and reuse it for the idempotency read, the write,
    // and the fill bound (rowPtrAt/noteFilled each re-resolve it otherwise).
    const int slot = physicalRowSlot(m_activeIndex, ri);
    if (cell.codePoint != 0 && cell.codePoint <= 0x7F && isCompactASCIIWriteCandidate(slot)) {
        const char byte = static_cast<char>(cell.codePoint);
        if (writeCompactASCIIRun(slot, ri, ci, ci, cell, &byte))
            return;
    }
    Cell* rowPtr = rowPtrAtSlot(m_activeIndex, slot);
    // Idempotent: identical write is a no-op, doesn't touch shadow
    // tracking.
    if (cellEqual(rowPtr[ci], cell))
        return;
    rowPtr[ci] = cell;
    m_anyMutationsSinceLastPaint = true;
    bumpRow(ri);
    if (cellPen(cell).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(cell).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    // A cursive/RTL base latches the row's render-time shaping gate (never
    // cleared per-cell; resetRowMeta clears it when the row is recycled).
    if (Unicode::mayNeedShaping(cell.codePoint))
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, true);
    if (cellPen(cell).attributes.containsAny(multicellHalves))
        rowMeta(ri).set(Ubiquitty::RowFlag::HasMulticell, true);
    // Non-blank content at ci extends the row's live width (erases own shrinks). Width-first: a write
    // inside the filled width exits on one compare, and a non-zero codePoint can never byte-equal the cp==0
    // canonical blank, so the memcmp runs only for the cp==0 styled-blank corner.
    int& filled = m_rowState[m_activeIndex][slot].filled;
    if (ci >= filled && (cell.codePoint != 0 || !isDefaultBlank(cell)))
        filled = ci + 1;
}

void CellGrid::setCellKnownDiff(Row r, Column c, const Cell& cell)
{
    const int ri = r.value();
    const int ci = c.value();
    assert(ri >= 0 && ri < m_rows);
    assert(ci >= 0 && ci < m_columns);
    if (ri < 0 || ri >= m_rows || ci < 0 || ci >= m_columns)
        return;
    const int slot = physicalRowSlot(m_activeIndex, ri);
    if (isCompactASCIIWriteCandidate(slot)) [[unlikely]] {
        setCellKnownDiffCompact(r, c, cell);
        return;
    }
    setCellKnownDiffOrdinary(r, c, cell);
}

[[gnu::always_inline]]
inline void CellGrid::setCellKnownDiffCompact(Row r, Column c, const Cell& cell)
{
    const int ri = r.value();
    const int ci = c.value();
    const int slot = physicalRowSlot(m_activeIndex, ri);
    if (cell.codePoint >= 0x20 && cell.codePoint <= 0x7E) {
        const char byte = static_cast<char>(cell.codePoint);
        if (writeCompactASCIIRun(slot, ri, ci, ci, cell, &byte, CompactWriteComparison::KnownDifferent))
            return;
    }
    materializeCompactRow(m_activeIndex, slot);
    setCellKnownDiffOrdinary(r, c, cell);
}

[[gnu::noinline]]
void CellGrid::setCellKnownDiffOrdinary(Row r, Column c, const Cell& cell)
{
    // Caller guarantees cell != current, so skip setCell's idempotency memcmp; rest is identical bookkeeping.
    const int ri = r.value();
    const int ci = c.value();
    const int slot = physicalRowSlot(m_activeIndex, ri);
    assert(m_rowState[m_activeIndex][slot].storage != RowStorage::DefaultASCII);
    Cell* row;
    if (slot == m_deferredBlankSlot[m_activeIndex] && ci < m_deferredBlankStart[m_activeIndex]) [[unlikely]] {
        // Below the stale window: these columns already hold cells this run
        // wrote, so the write lands on live storage and the deferral stands.
        // Resolving here would blank the rest of a row for a column that was
        // never stale — the case a cluster's lead-cell rewrite takes.
        row = m_rowPtrs[m_activeIndex][slot];
    } else if (slot == m_deferredBlankSlot[m_activeIndex] && ci == m_deferredBlankStart[m_activeIndex]) [[unlikely]] {
        // The cell lands exactly on the stale window's start edge, so it
        // consumes one column of the deferral in place: [0, start) is cells
        // this same lane wrote, and the window shrinks by one instead of the
        // row being filled to hand out a pointer. A glyph-at-a-time write
        // across a recycled row is the shape a plain text line takes, and
        // resolving per glyph filled the row on the first one.
        const int staleCount = m_deferredBlankCount[m_activeIndex] <= m_columnCapacity ? m_deferredBlankCount[m_activeIndex] : m_columnCapacity;
        if (ci + 1 >= staleCount) {
            m_deferredBlankSlot[m_activeIndex] = -1;
            m_deferredBlankCount[m_activeIndex] = 0;
            m_deferredBlankStart[m_activeIndex] = 0;
        } else {
            m_deferredBlankStart[m_activeIndex] = ci + 1;
        }
        row = m_rowPtrs[m_activeIndex][slot];
    } else {
        resolveDeferredBlankIfSlot(m_activeIndex, slot);
        row = m_rowPtrs[m_activeIndex][slot];
    }
    row[ci] = cell;
    m_anyMutationsSinceLastPaint = true;
    bumpRow(ri);
    if (cellPen(cell).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(cell).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    if (Unicode::mayNeedShaping(cell.codePoint))
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, true);
    if (cellPen(cell).attributes.containsAny(multicellHalves))
        rowMeta(ri).set(Ubiquitty::RowFlag::HasMulticell, true);
    // Width-first bookkeeping, same shape as setCell.
    int& filled = m_rowState[m_activeIndex][slot].filled;
    if (ci >= filled && (cell.codePoint != 0 || !isDefaultBlank(cell)))
        filled = ci + 1;
}

void CellGrid::setRowSpan(Row r, Column firstColumn, RowView span)
{
    const int ri = r.value();
    const int fc = firstColumn.value();
    assert(ri >= 0 && ri < m_rows);
    assert(span.count >= 0);
    // firstColumn is not asserted >= 0: the compensate-and-clamp path below
    // trims a negative fc against the run, so callers may pass one.
    assert(span.data != nullptr || span.count == 0);
    if (ri < 0 || ri >= m_rows || span.count <= 0 || !span.data)
        return;
    int lo = fc;
    int hi = fc + span.count - 1;
    const Cell* src = span.data;
    // Soft-fail for negative firstColumn: compensate src by -lo cells so a stale caller can't walk past its
    // buffer; bail if the span has fewer than that many cells.
    if (lo < 0) {
        const int shift = -lo;
        if (shift >= span.count)
            return;
        src += shift;
        lo = 0;
    }
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo) {
        return;
    }
    const int slot = physicalRowSlot(m_activeIndex, ri);
    materializeCompactRow(m_activeIndex, slot);
    // A span inside the deferred window replaces logically blank cells regardless of stale storage.
    // Its memcmp lane is skipped: matching stale bytes must not select the no-op path. The one
    // logically idempotent span — every cell the default blank — is detected in the copy loop instead.
    bool isDestinationLogicallyBlank = slot == m_deferredBlankSlot[m_activeIndex];
    if (isDestinationLogicallyBlank) [[unlikely]] {
        const int staleStart = m_deferredBlankStart[m_activeIndex];
        const int staleCount = std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        isDestinationLogicallyBlank = lo >= staleStart && hi < staleCount;
        if (!isDestinationLogicallyBlank) {
            // Both sides of the recorded window may hold live cells. Materialize the whole window
            // before comparing a mixed span; leaving its covered stale bytes in place could make a
            // real write appear identical. Fixed resize can retain content beyond staleCount too.
            resolveDeferredBlankSlot(m_activeIndex);
        } else {
            materializeDeferredBlankAroundWindow(slot, lo, hi);
        }
    }
    Cell* dst = m_rowPtrs[m_activeIndex][slot];
    // One memcmp over the zero-padded Cells decides the whole span; an unchanged span (idempotent repaint)
    // writes nothing and leaves the dirty/blink hints alone.
    const std::size_t spanBytes = static_cast<std::size_t>(hi - lo + 1) * sizeof(Cell);
    if (!isDestinationLogicallyBlank && std::memcmp(&dst[lo], src, spanBytes) == 0)
        return;
    // Changed: copy and detect blink/shape in one pass, with an unconditional store and no per-cell branch.
    Ubiquitty::VT::AttributeSet spanAttributes {};
    bool hasShape = false;
    std::uint8_t extendedRaw = 0;
    bool doesSpanDifferFromBlank = false;
    for (int c = lo; c <= hi; ++c) {
        const Cell& srcCell = src[c - lo];
        // Loop-invariant guard (unswitched at -O3): only the deferred lane
        // needs the against-blank probe standing in for the skipped memcmp.
        if (isDestinationLogicallyBlank)
            doesSpanDifferFromBlank |= !cellEqual(srcCell, m_defaultBlank);
        dst[c] = srcCell;
        // The union, not three tests: every question asked after this loop is
        // about whether any cell carried a bit, so one or per cell answers all of
        // them and the masking happens once.
        spanAttributes.add(cellPen(srcCell).attributes);
        hasShape |= Unicode::mayNeedShaping(srcCell.codePoint);
        // Ored raw like the rest and not folded into spanAttributes: the
        // specification id is a subfield of the EXTENDED attributes, so the
        // union above does not carry it, and the field is read once below.
        extendedRaw |= cellPen(srcCell).extendedAttributes.flags.toRaw();
    }
    const bool hasSizedCell = Ubiquitty::VT::ExtendedAttributes::TextSizeSpecField::get(extendedRaw) != 0;
    // Blanks stored over a logically blank row: the physical bytes now match
    // what materializing would have written, and the hints stay untouched
    // exactly as the memcmp no-op lane leaves them.
    if (isDestinationLogicallyBlank && !doesSpanDifferFromBlank)
        return;
    m_anyMutationsSinceLastPaint = true;
    bumpRow(ri);
    if (spanAttributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (hasSizedCell)
        noteSizedWrite();
    // A span covering the whole row is the row, so `hasShape` is the row's
    // answer and not merely an addition to it. A narrower span can only add:
    // the columns it did not write may still carry a joiner, and clearing on
    // one would leave the rest of the row unshaped on screen.
    if (lo <= 0 && hi + 1 >= m_columns)
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, hasShape);
    else if (hasShape)
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, true);
    if (spanAttributes.containsAny(multicellHalves))
        rowMeta(ri).set(Ubiquitty::RowFlag::HasMulticell, true);
    // A span write is content (erases take the eraseRowRange path), so the
    // row's live width reaches at least past the span.
    noteFilled(ri, hi + 1);
}

void CellGrid::setRowSpan(ScreenMode screen, Row r, Column firstColumn, RowView span)
{
    const int screenIndex = (screen == ScreenMode::Alternate) ? 1 : 0;
    if (screenIndex == m_activeIndex) {
        // Active screen: the full paint-tracked write, same path the parser uses.
        setRowSpan(r, firstColumn, span);
        return;
    }
    // Inactive screen (snapshot apply): write cells + content-width but stay off the paint-dirty path.
    // A later setAlternateActive() supplies the repaint. Mirrors eraseRowRangeInactive.
    const int ri = r.value();
    const int fc = firstColumn.value();
    assert(ri >= 0 && ri < m_rows);
    assert(span.count >= 0);
    assert(span.data != nullptr || span.count == 0);
    if (ri < 0 || ri >= m_rows || span.count <= 0 || !span.data)
        return;
    int lo = fc;
    int hi = fc + span.count - 1;
    const Cell* src = span.data;
    if (lo < 0) {
        const int shift = -lo;
        if (shift >= span.count)
            return;
        src += shift;
        lo = 0;
    }
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    const int slot = physicalRowSlot(screenIndex, ri);
    Cell* dst = rowPtrAtSlot(screenIndex, slot);
    Ubiquitty::VT::AttributeSet spanAttributes {};
    bool hasShape = false;
    std::uint8_t extendedRaw = 0;
    for (int c = lo; c <= hi; ++c) {
        dst[c] = src[c - lo];
        hasShape |= Unicode::mayNeedShaping(src[c - lo].codePoint);
        spanAttributes.add(cellPen(src[c - lo]).attributes);
        extendedRaw |= cellPen(src[c - lo]).extendedAttributes.flags.toRaw();
    }
    const bool hasSizedCell = Ubiquitty::VT::ExtendedAttributes::TextSizeSpecField::get(extendedRaw) != 0;
    noteFilledSlot(screenIndex, slot, hi + 1);
    // The inactive screen arms the sized hint though it does not arm the blink
    // one: a blink cell off screen animates nothing, while a sized cell off
    // screen is read by a flatten that scans both grids. This is the path a
    // snapshot apply rebuilds the alternate screen through.
    if (hasSizedCell)
        noteSizedWrite();
    if (hasShape)
        rowMetaAt(screenIndex, ri).set(Ubiquitty::RowFlag::NeedsShaping, true);
    if (spanAttributes.containsAny(multicellHalves))
        rowMetaAt(screenIndex, ri).set(Ubiquitty::RowFlag::HasMulticell, true);
}

#if defined(__clang__)
[[gnu::always_inline]]
#else
[[gnu::noinline]]
#endif
void CellGrid::materializeDeferredBlankAroundWindow(int slot, int firstColumn, int lastColumn)
{
    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    // Only the stale window's uncovered parts are filled. The window is
    // [start, count): below start the columns already hold cells an edge lane
    // wrote, and inside [firstColumn, lastColumn] the caller's own store lands
    // on every byte — filling either is writing cells that are already right
    // or are about to be overwritten. A full-width run over a recycled row
    // leaves both gaps empty and fills nothing at all.
    const int staleCount = m_deferredBlankCount[m_activeIndex] <= m_columnCapacity ? m_deferredBlankCount[m_activeIndex] : m_columnCapacity;
    const int staleStart = m_deferredBlankStart[m_activeIndex];
    if (firstColumn > staleStart) {
        const int gapEnd = firstColumn < staleCount ? firstColumn : staleCount;
        if (gapEnd > staleStart)
            fillBlank(destination + staleStart, m_defaultBlank, gapEnd - staleStart);
    }
    const int tailStart = (lastColumn + 1) > staleStart ? (lastColumn + 1) : staleStart;
    if (staleCount > tailStart) {
        fillBlank(&destination[tailStart], m_defaultBlank, staleCount - tailStart);
    }
    m_deferredBlankSlot[m_activeIndex] = -1;
    m_deferredBlankCount[m_activeIndex] = 0;
    m_deferredBlankStart[m_activeIndex] = 0;
}

// The run writer's share of the parse attribution harness (see VT.cpp's
// ParseAblateBit). Bit 2 skips the idempotency probe, whose cost is a second
// resolution of the row this writer has already resolved.
//
// Compile-time for the reason the VT side gives: a runtime read inside a
// profiled function detaches it from the profile. A default build compiles this
// away entirely, and the output is wrong when it is set — an identical repaint
// stops being idempotent and dirties the row.
#if !defined(UBIQUITTY_PARSE_ABLATE)
#define UBIQUITTY_PARSE_ABLATE 0
#endif
inline constexpr bool skipsRunIdempotencyProbe = (UBIQUITTY_PARSE_ABLATE & (1u << 2)) != 0;

// One run byte's code point. The untranslated form is the identity the batched
// writer was built on, so its instantiation carries no table and no load; the
// translated form indexes a 95-entry table the caller has already established
// maps every byte to a single-cell, non-shaping code point.
template<bool Translated>
static inline uint32_t runCodePoint(const char* source, int index, const uint16_t* translation)
{
    const unsigned char byte = static_cast<unsigned char>(source[index]);
    if constexpr (Translated)
        return translation[byte - Ubiquitty::runTranslationFirstByte];
    else
        return static_cast<uint32_t>(byte);
}

// Preload the template before storing: it may be borrowed from the destination.
// Each cell is written once, with no destination reads or intermediate fill.
template<bool Translated>
static inline void storeRunCells(std::span<Cell> destination, const Cell& cellTemplate, const char* source, const std::uint16_t* translation)
{
    const int count = static_cast<int>(destination.size());
#if defined(__aarch64__)
    static_assert(sizeof(Cell) == 32 && offsetof(Cell, codePoint) == 0, "the run kernel builds 32-byte cells led by the code point");
    const uint8_t* templateBytes = reinterpret_cast<const uint8_t*>(&cellTemplate);
    const uint32x4_t templateLow = vreinterpretq_u32_u8(vld1q_u8(templateBytes));
    const uint8x16_t templateHigh = vld1q_u8(templateBytes + 16);
    uint8_t* destinationBytes = reinterpret_cast<uint8_t*>(destination.data());
    for (int index = 0; index < count; ++index) {
        const uint32x4_t low = vsetq_lane_u32(runCodePoint<Translated>(source, index, translation), templateLow, 0);
        vst1q_u8(destinationBytes, vreinterpretq_u8_u32(low));
        vst1q_u8(destinationBytes + 16, templateHigh);
        destinationBytes += sizeof(Cell);
    }
#else
    Cell cell = cellTemplate;
    for (int index = 0; index < count; ++index) {
        cell.codePoint = runCodePoint<Translated>(source, index, translation);
        destination[index] = cell;
    }
#endif
}

// A deferred row is logically blank: store the desired cells and materialize
// only the stale flanks the run leaves. Keep the bookkeeping out of line so
// the ordinary writer does not carry the recycled-row machinery in its body.
template<bool Translated>
[[gnu::noinline]]
void CellGrid::writeRunToDeferredBlank(int slot, int row, int firstColumn, int lastColumn, const Cell& cellTemplate, const char* source, const std::uint16_t* translation)
{
    materializeDeferredBlankAroundWindow(slot, firstColumn, lastColumn);
    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    storeRunCells<Translated>({ &destination[firstColumn], static_cast<std::size_t>(lastColumn - firstColumn + 1) }, cellTemplate, source, translation);
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    if (cellPen(cellTemplate).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(cellTemplate).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    noteFilled(row, lastColumn + 1);
}

template<bool Translated>
[[gnu::noinline]]
void CellGrid::writeRunFromCompactBytes(Row row, Column column, const Cell& cellTemplate, std::string_view source, const std::uint16_t* translation)
{
    assert(!source.empty() && source.size() <= static_cast<std::size_t>(maxColumns));
    // A compact byte view can be passed back as input. Expansion would
    // overwrite it, and an overlapping compact write must preserve its input.
    // Keep this bounded staging frame off the ordinary write path.
    std::array<char, maxColumns> bytes;
    std::memcpy(bytes.data(), source.data(), source.size());
    writeRun<Translated>(row, column, cellTemplate, std::string_view(bytes.data(), source.size()), translation);
}

template<bool Translated>
void CellGrid::writeRun(Row r, Column firstColumnValue, const Cell& tmpl, std::string_view text, const std::uint16_t* translation)
{
    const int ri = r.value();
    const int count = static_cast<int>(text.size());
    int lo = firstColumnValue.value();
    int hi = lo + count - 1;
    if (ri < 0 || ri >= m_rows || text.empty()) {
        return;
    }
    const char* src = text.data();
    if (lo < 0) {
        const int shift = -lo;
        if (shift >= count) {
            return;
        }
        src += shift;
        lo = 0;
    }
    if (hi >= m_columns) {
        hi = m_columns - 1;
    }
    if (hi < lo) {
        return;
    }
    const int slot = physicalRowSlot(m_activeIndex, ri);
    if (m_rowState[m_activeIndex][slot].storage == RowStorage::DefaultASCII) {
        const Cell* row = m_rowPtrs[m_activeIndex][slot];
        const std::less<const void*> precedes;
        if (precedes(src, row + m_columnCapacity) && precedes(row, src + (hi - lo + 1))) [[unlikely]] {
            writeRunFromCompactBytes<Translated>(r, Column(lo), tmpl, std::string_view(src, static_cast<std::size_t>(hi - lo + 1)), translation);
            return;
        }
    }
    if constexpr (!Translated) {
        if (isCompactASCIIWriteCandidate(slot) && writeCompactASCIIRun(slot, ri, lo, hi, tmpl, src))
            return;
    }
    materializeCompactRow(m_activeIndex, slot);
    if (slot == m_deferredBlankSlot[m_activeIndex]) [[unlikely]] {
        const int staleStart = m_deferredBlankStart[m_activeIndex];
        const int staleCount = std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        if (staleStart == 0 || (lo >= staleStart && hi < staleCount)) {
            writeRunToDeferredBlank<Translated>(slot, ri, lo, hi, tmpl, src, translation);
            return;
        }
        // A run reaching outside the remaining stale window can overlap live
        // prefix cells or retained fixed-width content. Compare those normally.
        resolveDeferredBlankSlot(m_activeIndex);
    }
    const int row = ri;
    const int firstColumn = lo;
    const int lastColumn = hi;
    const Cell& cellTemplate = tmpl;
    const char* source = src;
    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    Cell cell = cellTemplate;
    // Fresh-run fast path: probe the first cell. New text landing on blank/old cells differs, so stream the
    // run with stores only, skipping the idempotency read. A fully-identical repaint starts with an
    // identical first cell, so it falls to the careful per-cell path.
    cell.codePoint = runCodePoint<Translated>(source, 0, translation);
    if (skipsRunIdempotencyProbe || !cellEqual(destination[firstColumn], cell)) {
        // The first difference already requires mutation bookkeeping. Store the
        // remaining cells directly without inspecting their old pens or clusters.
        storeRunCells<Translated>({ &destination[firstColumn], static_cast<std::size_t>(lastColumn - firstColumn + 1) }, cell, source, translation);
        m_anyMutationsSinceLastPaint = true;
        bumpRow(row);
        if (cellPen(cellTemplate).extendedAttributes.textSizeSpecID() != 0)
            noteSizedWrite();
        if (cellPen(cellTemplate).attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
            noteBlinkWrite();
        }
        noteFilled(row, lastColumn + 1);
        return;
    }
    // Careful path (may be an identical repaint): per-cell compare keeps dirty/blink/filled idempotent.
    bool changed = false;
    for (int column = firstColumn; column <= lastColumn; ++column) {
        cell.codePoint = runCodePoint<Translated>(source, column - firstColumn, translation);
        if (!cellEqual(destination[column], cell)) {
            destination[column] = cell;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    if (cellPen(cellTemplate).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    if (cellPen(cellTemplate).attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
        noteBlinkWrite();
    }
    noteFilled(row, lastColumn + 1);
}

void CellGrid::writeASCIIRun(Row r, Column firstColumn, const Cell& tmpl, std::string_view text)
{
    writeRun<false>(r, firstColumn, tmpl, text, nullptr);
}

void CellGrid::writeWideCodePointRun(Row rowValue, Column firstColumnValue, const Cell& cellTemplate, std::span<const uint32_t> codePoints)
{
    const int row = rowValue.value();
    const int firstColumn = firstColumnValue.value();
    if (row < 0 || row >= m_rows || firstColumn < 0 || firstColumn >= m_columns || codePoints.empty()
        || codePoints.size() > static_cast<std::size_t>((m_columns - firstColumn) / 2))
        return;
    assert(row >= 0 && row < m_rows);
    assert(firstColumn >= 0 && firstColumn < m_columns);
    assert(cellTemplate.combiningLength() == 0 && cellTemplate.clusterID() == 0);
    const auto attributes = cellPen(cellTemplate).attributes - VT::Attribute::Emoji;
    Cell lead = cellTemplate;
    cellSetPenAttributes(lead, attributes | VT::Attribute::Wide);
    Cell continuation = cellTemplate;
    continuation.codePoint = wideContinuationSentinelCodePoint;
    cellSetPenAttributes(continuation, attributes | VT::Attribute::WideContinuation);
    const int lastColumn = firstColumn + static_cast<int>(codePoints.size()) * 2 - 1;
    const int slot = physicalRowSlot(m_activeIndex, row);
    materializeCompactRow(m_activeIndex, slot);
    bool isDestinationLogicallyBlank = slot == m_deferredBlankSlot[m_activeIndex];
    if (isDestinationLogicallyBlank) {
        isDestinationLogicallyBlank = firstColumn >= m_deferredBlankStart[m_activeIndex]
            && lastColumn < std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        if (isDestinationLogicallyBlank)
            materializeDeferredBlankAroundWindow(slot, firstColumn, lastColumn);
        else
            resolveDeferredBlankSlot(m_activeIndex);
    }

    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    if (!isDestinationLogicallyBlank) {
        bool hasDifference = false;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            const int column = firstColumn + static_cast<int>(index) * 2;
            assert(column >= 0 && column + 1 < m_columns);
            lead.codePoint = codePoints[index];
            if (!cellEqual(destination[column], lead) || !cellEqual(destination[column + 1], continuation)) {
                hasDifference = true;
                break;
            }
        }
        if (!hasDifference)
            return;
    }
    for (std::size_t index = 0; index < codePoints.size(); ++index) {
        const int column = firstColumn + static_cast<int>(index) * 2;
        assert(column >= 0 && column + 1 < m_columns);
        assert(codePoints[index] && !Unicode::mayNeedShaping(codePoints[index]));
        lead.codePoint = codePoints[index];
        destination[column] = lead;
        destination[column + 1] = continuation;
    }
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    if (attributes.contains(VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(lead).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    if (firstColumn == 0 && lastColumn + 1 == m_columns)
        rowMeta(row).set(RowFlag::NeedsShaping, false);
    rowMeta(row).set(RowFlag::HasMulticell, true);
    noteFilled(row, lastColumn + 1);
}

void CellGrid::writeNarrowCodePointRun(Row rowValue, Column firstColumnValue, const Cell& cellTemplate, std::span<const uint32_t> codePoints)
{
    const int row = rowValue.value();
    const int firstColumn = firstColumnValue.value();
    if (row < 0 || row >= m_rows || firstColumn < 0 || firstColumn >= m_columns || codePoints.empty()
        || codePoints.size() > static_cast<std::size_t>(m_columns - firstColumn))
        return;
    assert(row >= 0 && row < m_rows);
    assert(firstColumn >= 0 && firstColumn < m_columns);
    assert(cellTemplate.combiningLength() == 0 && cellTemplate.clusterID() == 0);
    assert(!cellPen(cellTemplate).attributes.containsAny(multicellHalves));
    Cell cell = cellTemplate;
    const int lastColumn = firstColumn + static_cast<int>(codePoints.size()) - 1;
    const int slot = physicalRowSlot(m_activeIndex, row);
    materializeCompactRow(m_activeIndex, slot);
    bool isDestinationLogicallyBlank = slot == m_deferredBlankSlot[m_activeIndex];
    if (isDestinationLogicallyBlank) {
        isDestinationLogicallyBlank = firstColumn >= m_deferredBlankStart[m_activeIndex]
            && lastColumn < std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        if (isDestinationLogicallyBlank)
            materializeDeferredBlankAroundWindow(slot, firstColumn, lastColumn);
        else
            resolveDeferredBlankSlot(m_activeIndex);
    }

    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    if (!isDestinationLogicallyBlank) {
        bool hasDifference = false;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            const int column = firstColumn + static_cast<int>(index);
            assert(column >= 0 && column < m_columns);
            cell.codePoint = codePoints[index];
            if (!cellEqual(destination[column], cell)) {
                hasDifference = true;
                break;
            }
        }
        if (!hasDifference)
            return;
    }
    for (std::size_t index = 0; index < codePoints.size(); ++index) {
        const int column = firstColumn + static_cast<int>(index);
        assert(column >= 0 && column < m_columns);
        assert(codePoints[index] && !Unicode::mayNeedShaping(codePoints[index]));
        Cell outputCell = cell;
        outputCell.codePoint = codePoints[index];
        destination[column] = outputCell;
    }
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    if (cellPen(cell).attributes.contains(VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(cell).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    if (firstColumn == 0 && lastColumn + 1 == m_columns)
        rowMeta(row).set(RowFlag::NeedsShaping, false);
    noteFilled(row, lastColumn + 1);
}

void CellGrid::writeTranslatedRun(Row r, Column firstColumn, const Cell& tmpl, std::string_view text, const std::uint16_t* translation)
{
    writeRun<true>(r, firstColumn, tmpl, text, translation);
}

void CellGrid::fillCellRun(Row r, Column firstColumnValue, const Cell& cell, int count)
{
    const int ri = r.value();
    int lo = firstColumnValue.value();
    int hi = lo + count - 1;
    if (ri < 0 || ri >= m_rows || count <= 0)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    const int slot = physicalRowSlot(m_activeIndex, ri);
    materializeCompactRow(m_activeIndex, slot);
    // A deferred-blank destination is logically blank regardless of stale storage; skip the equality scan
    // (byte-matching stale bytes must not select the no-op path).
    bool logicallyBlankDestination = false;
    if (slot == m_deferredBlankSlot[m_activeIndex]) [[unlikely]] {
        if (m_deferredBlankStart[m_activeIndex] == 0) {
            materializeDeferredBlankAroundWindow(slot, lo, hi);
            logicallyBlankDestination = true;
        } else {
            // Partially consumed: live pairs stand in [0, start); resolve whole.
            resolveDeferredBlankSlot(m_activeIndex);
        }
    }
    Cell* destination = m_rowPtrs[m_activeIndex][slot];
    // Local pattern copy: `cell` arrives by reference and could alias dst, pinning the loads per cell.
    const Cell pattern = cell;
    int first = lo;
    if (!logicallyBlankDestination) {
        first = firstDifferingCell(destination, first, hi, pattern);
        // Identical repaint: no stores, dirty/blink/filled untouched.
        if (first > hi)
            return;
    }
    fillBlank(&destination[first], pattern, hi - first + 1);
    m_anyMutationsSinceLastPaint = true;
    bumpRow(ri);
    if (cellPen(pattern).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(pattern).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    // Every cell of the run is the same cell, so a run covering the row makes
    // the pattern the row's whole answer to whether anything here shapes.
    const bool doesPatternShape = Unicode::mayNeedShaping(pattern.codePoint);
    if (firstColumnValue.value() <= 0 && hi + 1 >= m_columns)
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, doesPatternShape);
    else if (doesPatternShape)
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, true);
    noteFilled(ri, hi + 1);
}

void CellGrid::eraseRowRange(Row r, Column c0, Column c1, const Cell& blank)
{
    int ri = r.value();
    int lo = c0.value(), hi = c1.value();
    assert(ri >= 0 && ri < m_rows);
    assert(lo <= hi);
    if (ri < 0 || ri >= m_rows)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    // A default-blank erase can stop at the content-width bound: [filled, columns) already equals
    // m_defaultBlank. A range entirely inside that tail returns before rowPtrAt (no deferred-blank
    // materialization to verify blanks).
    int scanHi = hi;
    if (isDefaultBlank(blank)) {
        const int filled = rowFilledAt(m_activeIndex, ri);
        if (lo >= filled)
            return;
        if (scanHi >= filled)
            scanHi = filled - 1;
    }
    Cell* dst = rowPtrAt(m_activeIndex, ri);
    // Scan first for idempotency: an already-blank range writes nothing. On any difference, broadcast-fill
    // the whole range in one fillBlank run.
    const bool hasDifference = firstDifferingCell(dst, lo, scanHi, blank) <= scanHi;
    if (hasDifference) {
        fillBlank(&dst[lo], blank, scanHi - lo + 1);
        m_anyMutationsSinceLastPaint = true;
        bumpRow(ri);
    }
    // Maintain the content-width bound (applied even when idempotent): a colored blank is content → extend;
    // a default-blank erase to the right edge shrinks to lo; a default-blank mid-row erase leaves the bound.
    if (!isDefaultBlank(blank)) {
        noteFilled(ri, hi + 1);
    } else if (hi >= m_columns - 1) {
        if (lo < rowFilledAt(m_activeIndex, ri))
            rowFilledAt(m_activeIndex, ri) = lo;
    }
    // An erase spanning the row leaves it holding the erase cell and nothing
    // else, so the shaping gate answers for that cell. Erase-line is how a row
    // that once carried a joiner — every ZWJ emoji sequence carries one — stops
    // carrying it, and without this the row keeps paying the shaping path for
    // the life of the screen.
    if (lo <= 0 && hi + 1 >= m_columns)
        rowMeta(ri).set(Ubiquitty::RowFlag::NeedsShaping, Unicode::mayNeedShaping(blank.codePoint));
}

void CellGrid::eraseRow(Row r, const Cell& blank) { eraseRowRange(r, Column(0), Column(m_columns - 1), blank); }

void CellGrid::eraseRowRangeInactive(Row r, Column c0, Column c1, const Cell& blank)
{
    // Blank a range on the non-active screen without touching dirty tracking. Used by resize to clear
    // newly-revealed alt-screen cells so a future alt-flip doesn't surface pre-resize garbage.
    const int inactive = 1 - m_activeIndex;
    int ri = r.value();
    int lo = c0.value(), hi = c1.value();
    assert(ri >= 0 && ri < m_rows);
    assert(lo <= hi);
    if (ri < 0 || ri >= m_rows)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    fillBlank(&rowPtrAt(inactive, ri)[lo], blank, hi - lo + 1);
    // A colored blank writes content on the inactive screen; keep its width
    // bound covering it (default blanks leave the [filled, columns) invariant).
    if (!isDefaultBlank(blank)) {
        int w = hi + 1;
        if (w > m_columns)
            w = m_columns;
        if (w > rowFilledAt(inactive, ri))
            rowFilledAt(inactive, ri) = w;
    }
}

void CellGrid::clearWideOverlap(Row r, Column from, Column to, const Cell& blank)
{
    const int ri = r.value();
    int lo = from.value(), hi = to.value();
    if (ri < 0 || ri >= m_rows)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    Cell* row = rowPtrAt(m_activeIndex, ri);
    // The same operation the span writers do: whatever run overlaps this
    // window, the part standing outside orphans when the window is
    // overwritten. Reached by the per-glyph write, so it steps a pair's one
    // partner in the common case and the whole chain where a run is wider.
    // Width is claimed only per STORED break-blank: claiming for a write that
    // never happened made filled depend on byte-stream chunking, storing a
    // spurious trailing blank into scrollback.
    blankRunSpillOutsideWindow(ri, row, lo, hi, blank,
        isDefaultBlank(blank) ? FilledWidthClaim::None : FilledWidthClaim::PerStoredBreakBlank);
}

void CellGrid::clearWideOverlapEdges(Row r, Column from, Column to, const Cell& blank)
{
    const int ri = r.value();
    int lo = from.value(), hi = to.value();
    if (ri < 0 || ri >= m_rows)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    if (hi < lo)
        return;
    // A full-width replacement has no cells outside its window to orphan, even when an edge holds
    // a fragmented wide run. Leave deferred blanks untouched for the writer that owns the window.
    if (lo == 0 && hi + 1 == m_columns)
        return;
    const int slot = physicalRowSlot(m_activeIndex, ri);
    if (m_rowState[m_activeIndex][slot].storage == RowStorage::DefaultASCII)
        return;
    if (slot == m_deferredBlankSlot[m_activeIndex]) [[unlikely]] {
        // Neither edge inside the remaining blank window can carry a wide half. Outside it, check
        // the live edge cells before giving up the deferral: a longer ASCII run usually reaches
        // already-blank storage past staleCount and still benefits from the deferred run writer.
        // A colored break-blank takes the materialized path for its filled-width claim.
        const int staleCount = std::min(m_deferredBlankCount[m_activeIndex], m_columnCapacity);
        if (isDefaultBlank(blank)) {
            const int staleStart = m_deferredBlankStart[m_activeIndex];
            if (lo >= staleStart && hi < staleCount)
                return;
            const Cell* row = m_rowPtrs[m_activeIndex][slot];
            const auto firstAttributes = lo >= staleStart && lo < staleCount
                ? VT::AttributeSet {}
                : cellPen(row[lo]).attributes;
            const auto lastAttributes = hi >= staleStart && hi < staleCount
                ? VT::AttributeSet {}
                : cellPen(row[hi]).attributes;
            if (!firstAttributes.contains(VT::Attribute::WideContinuation)
                && !lastAttributes.containsAny(multicellHalves))
                return;
        }
        resolveDeferredBlankSlot(m_activeIndex);
    }
    Cell* row = m_rowPtrs[m_activeIndex][slot];
    // Width claimed per STORED break-blank (see clearWideOverlap).
    blankRunSpillOutsideWindow(ri, row, lo, hi, blank,
        isDefaultBlank(blank) ? FilledWidthClaim::None : FilledWidthClaim::PerStoredBreakBlank);
}

void CellGrid::blankRunSpillOutsideWindow(int ri, Cell* row, int lo, int hi, const Cell& blank, FilledWidthClaim claim)
{
    // Left edge: the window's first cell mid-chain means the run's head stands
    // outside and orphans when the window is overwritten. The walk blanks the
    // chain back to and including its anchor; a chain that ends on no anchor is
    // a fragment, and nothing left of a fragment belongs to it.
    if (cellPen(row[lo]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
        bool didBlank = false;
        int j = lo - 1;
        while (j >= 0 && cellPen(row[j]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            if (!cellEqual(row[j], blank)) {
                row[j] = blank;
                didBlank = true;
            }
            --j;
        }
        if (j >= 0 && cellPen(row[j]).attributes.contains(Ubiquitty::VT::Attribute::Wide) && !cellEqual(row[j], blank)) {
            row[j] = blank;
            didBlank = true;
        }
        if (didBlank) {
            m_anyMutationsSinceLastPaint = true;
            bumpRow(ri);
            if (claim == FilledWidthClaim::PerStoredBreakBlank)
                noteFilled(ri, lo);
        }
    }
    // Right edge: a continuation just past the window chains from a cell inside
    // it, so the tail orphans. The window's last cell gates the probe twice
    // over: a chain can only continue past `hi` through `hi`, so a window
    // ending on a plain cell proves there is no tail without touching the
    // neighbour — and `row[hi]` is a cell every caller has already read.
    const Ubiquitty::VT::AttributeSet endAttributes = cellPen(row[hi]).attributes;
    const bool isWindowEndInChain = endAttributes.contains(Ubiquitty::VT::Attribute::Wide)
        || endAttributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
    if (isWindowEndInChain && hi + 1 < m_columns
        && cellPen(row[hi + 1]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
        bool didBlank = false;
        int j = hi + 1;
        while (j < m_columns && cellPen(row[j]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
            if (!cellEqual(row[j], blank)) {
                row[j] = blank;
                didBlank = true;
            }
            ++j;
        }
        if (didBlank) {
            m_anyMutationsSinceLastPaint = true;
            bumpRow(ri);
            if (claim == FilledWidthClaim::PerStoredBreakBlank)
                noteFilled(ri, j);
        }
    }
}

void CellGrid::writeRunRow(Row r, Column c, RowView run, const Cell& blank)
{
    const int ri = r.value();
    const int ci = c.value();
    const int width = run.count;
    if (ri < 0 || ri >= m_rows || ci < 0 || width < 2 || ci + width > m_columns)
        return;
    assert(cellPen(run.data[0]).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    Cell* row = rowPtrAt(m_activeIndex, ri);
    blankRunSpillOutsideWindow(ri, row, ci, ci + width - 1, blank, FilledWidthClaim::None);
    setRowSpan(r, c, run);
}

void CellGrid::writeWideCell(Row r, Column c, const Cell& lead, const Cell& continuation, const Cell& blank)
{
    const int ri = r.value();
    const int ci = c.value();
    assert(ri >= 0 && ri < m_rows);
    assert(ci >= 0 && ci < m_columns);
    assert(cellPen(lead).attributes.contains(Ubiquitty::VT::Attribute::Wide) && !cellPen(lead).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    assert(cellPen(continuation).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation) && !cellPen(continuation).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    // A wide pair needs two columns; at the right margin reject cleanly (neither half written). The VT
    // layer wraps/space-fills before here; this guards the contract without aborting.
    if (ri < 0 || ri >= m_rows || ci < 0 || ci + 1 >= m_columns)
        return;
    // Pen-equality invariant: the continuation half must carry the lead's fg/bg so a half-cell repaint never drifts
    // color across the glyph. Force the continuation's color fields to the lead's rather than trust the caller
    // (a mismatch silently corrupts under NDEBUG). AttributeSet stay as supplied.
    Cell continuationCell = continuation;
    {
        const Pen& leadPen = cellPen(lead);
        Pen mergedPen = cellPen(continuationCell);
        mergedPen.foregroundIndex = leadPen.foregroundIndex;
        mergedPen.backgroundIndex = leadPen.backgroundIndex;
        mergedPen.isForegroundAssignedDefault = leadPen.isForegroundAssignedDefault;
        mergedPen.isBackgroundAssignedDefault = leadPen.isBackgroundAssignedDefault;
        mergedPen.foregroundRed = leadPen.foregroundRed;
        mergedPen.foregroundGreen = leadPen.foregroundGreen;
        mergedPen.foregroundBlue = leadPen.foregroundBlue;
        mergedPen.backgroundRed = leadPen.backgroundRed;
        mergedPen.backgroundGreen = leadPen.backgroundGreen;
        mergedPen.backgroundBlue = leadPen.backgroundBlue;
        cellSetPen(continuationCell, mergedPen);
    }
    // Break any run overlapping the pair's window: the spill outside the
    // window orphans when the window is overwritten, at whatever width the
    // overlapped chain reaches. The window's own cells are not wiped — the
    // pair being written replaces them. A deferred-blank destination is
    // logically blank, so no chain can overlap the window: blank only the
    // stale flanks the pair does not overwrite and skip the spill walk.
    const int slot = physicalRowSlot(m_activeIndex, ri);
    materializeCompactRow(m_activeIndex, slot);
    Cell* row = m_rowPtrs[m_activeIndex][slot];
    bool isWindowLogicallyBlank = false;
    if (slot == m_deferredBlankSlot[m_activeIndex]) [[unlikely]] {
        // Both fast branches state their no-chain/no-fill reasoning only for a
        // window inside the recorded stale bound; a pair reaching past it
        // resolves whole and takes the ordinary walk, with no appeal to the
        // width invariant Fixed-mode resize can break.
        const int staleCount = m_deferredBlankCount[m_activeIndex] <= m_columnCapacity ? m_deferredBlankCount[m_activeIndex] : m_columnCapacity;
        if (ci + Ubiquitty::wideContinuationOffset >= staleCount) {
            resolveDeferredBlankSlot(m_activeIndex);
        } else if (ci == m_deferredBlankStart[m_activeIndex]) {
            // The pair lands exactly at the stale edge and consumes it in
            // place: [0, ci) is complete pairs from this same lane, so no
            // chain straddles the window, and [ci + 2, count) stays deferred.
            // No fill at all.
            const int consumedEnd = ci + Ubiquitty::wideContinuationOffset + 1;
            if (consumedEnd >= staleCount) {
                m_deferredBlankSlot[m_activeIndex] = -1;
                m_deferredBlankCount[m_activeIndex] = 0;
                m_deferredBlankStart[m_activeIndex] = 0;
            } else {
                m_deferredBlankStart[m_activeIndex] = consumedEnd;
            }
            isWindowLogicallyBlank = true;
        } else if (m_deferredBlankStart[m_activeIndex] == 0) {
            materializeDeferredBlankAroundWindow(slot, ci, ci + Ubiquitty::wideContinuationOffset);
            isWindowLogicallyBlank = true;
        } else {
            // Partially consumed and off the edge: live pairs stand in
            // [0, start) and the walk must see them through real bytes.
            resolveDeferredBlankSlot(m_activeIndex);
        }
    }
    if (!isWindowLogicallyBlank)
        blankRunSpillOutsideWindow(ri, row, ci, ci + Ubiquitty::wideContinuationOffset, blank, FilledWidthClaim::None);
    // Atomic pair write, unconditional. The only caller probes both halves and skips the call when the
    // pair is identical, so at least one half is changing; dirty is row-granular, so a per-half idempotency
    // compare couldn't avoid the bumpRow. Worst case for a future unguarded caller: one spurious row-dirty.
    row[ci] = lead;
    row[ci + Ubiquitty::wideContinuationOffset] = continuationCell;
    bumpRow(ri);
    m_anyMutationsSinceLastPaint = true;
    // Wide pair shares pen attributes; check the lead half.
    if (cellPen(lead).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    // The continuation is checked too, unlike the blink above. A sized block's
    // halves are written from pens the caller built separately, and a lead that
    // lost its id while the continuation kept one is exactly the shape the
    // flatten exists to reduce.
    if (cellPen(lead).extendedAttributes.textSizeSpecID() != 0
        || cellPen(continuation).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    // This path writes the row buffer directly, so it latches the hint itself
    // rather than inheriting setCell's.
    rowMeta(ri).set(Ubiquitty::RowFlag::HasMulticell, true);
    // The wide pair is content occupying ci..ci+1.
    noteFilled(ri, ci + 2);
}

void CellGrid::scrollRegion(Row topR, Row botR, int n, const Cell& blank)
{
    const int top = topR.value(), bot = botR.value();
    assert(top >= 0 && top <= bot && bot < m_rows);
    if (top < 0 || bot >= m_rows || top > bot || n == 0)
        return;
    const int region = bot - top + 1;
    int shift = n > 0 ? n : -n;
    if (top == 0 && bot == m_rows - 1 && n == 1 && shift < region) {
        scrollFullViewportUpOne(blank);
        return;
    }
    materializeCompactRows(m_activeIndex);
    bool fullRegionClear = false;
    if (shift >= region) {
        // Full-region erase: wipe row by row and drop the pending scroll before coalescing — a prior shift
        // no longer applies to the post-clear shadow, and coalescing it into the synthetic ±region one
        // would over-shift m_bbShadow on AckScroll.
        for (int r = top; r <= bot; ++r)
            eraseRow(Row(r), blank);
        shift = region;
        n = (n > 0) ? region : -region;
        m_pendingScroll.valid = false;
        fullRegionClear = true;
    }
    const bool blankDefault = isDefaultBlank(blank);
    bool narrowedDirtyHints = false;
    if (!fullRegionClear && top == 0 && bot == m_rows - 1 && shift == 1) {
        if (n > 0) {
            ++m_rowOrigin[m_activeIndex];
            if (m_rowOrigin[m_activeIndex] == m_rows)
                m_rowOrigin[m_activeIndex] = 0;

            const int slot = physicalRowSlot(m_activeIndex, bot);
            Cell* row = m_rowPtrs[m_activeIndex][slot];
            int& filled = m_rowState[m_activeIndex][slot].filled;
            if (blankDefault) {
                // Defer the recycled row's blank, as in scrollFullViewportUpOneWithBlank (same scroll,
                // different entry point).
                if (m_deferredBlankSlot[m_activeIndex] >= 0 && m_deferredBlankSlot[m_activeIndex] != slot) {
                    resolveDeferredBlankSlot(m_activeIndex);
                }
                if (filled > 0) {
                    m_deferredBlankSlot[m_activeIndex] = slot;
                    if (filled > m_deferredBlankCount[m_activeIndex])
                        m_deferredBlankCount[m_activeIndex] = filled;
                    m_deferredBlankStart[m_activeIndex] = 0;
                    filled = 0;
                }
            } else {
                resolveDeferredBlankIfSlot(m_activeIndex, slot);
                fillBlank(row, blank, m_columns);
                filled = m_columns;
            }
            m_rowState[m_activeIndex][slot].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
        } else {
            if (m_rowOrigin[m_activeIndex] == 0)
                m_rowOrigin[m_activeIndex] = m_rows - 1;
            else
                --m_rowOrigin[m_activeIndex];

            const int slot = physicalRowSlot(m_activeIndex, top);
            Cell* row = m_rowPtrs[m_activeIndex][slot];
            int& filled = m_rowState[m_activeIndex][slot].filled;
            if (blankDefault) {
                // Scroll-down recycles at the top; same deferral (the next
                // write usually lands on the revealed top row).
                if (m_deferredBlankSlot[m_activeIndex] >= 0 && m_deferredBlankSlot[m_activeIndex] != slot) {
                    resolveDeferredBlankSlot(m_activeIndex);
                }
                if (filled > 0) {
                    m_deferredBlankSlot[m_activeIndex] = slot;
                    if (filled > m_deferredBlankCount[m_activeIndex])
                        m_deferredBlankCount[m_activeIndex] = filled;
                    m_deferredBlankStart[m_activeIndex] = 0;
                    filled = 0;
                }
            } else {
                resolveDeferredBlankIfSlot(m_activeIndex, slot);
                fillBlank(row, blank, m_columns);
                filled = m_columns;
            }
            m_rowState[m_activeIndex][slot].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
        }
        markDirtyRange(top, bot);
        narrowedDirtyHints = true;
    } else if (!fullRegionClear && top == 0 && bot == m_rows - 1 && shift < region) {
        if (n > 0) {
            m_rowOrigin[m_activeIndex] += shift;
            if (m_rowOrigin[m_activeIndex] >= m_rows)
                m_rowOrigin[m_activeIndex] -= m_rows;
            for (int r = bot - shift + 1; r <= bot; ++r) {
                Cell* row = rowPtrAt(m_activeIndex, r);
                int& filled = rowFilledAt(m_activeIndex, r);
                if (blankDefault) {
                    fillBlank(row, blank, filled);
                    filled = 0;
                } else {
                    fillBlank(row, blank, m_columns);
                    filled = m_columns;
                }
                rowMetaAt(m_activeIndex, r) = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
            markDirtyRange(top, bot);
            narrowedDirtyHints = true;
        } else {
            m_rowOrigin[m_activeIndex] -= shift;
            if (m_rowOrigin[m_activeIndex] < 0)
                m_rowOrigin[m_activeIndex] += m_rows;
            for (int r = top; r < top + shift; ++r) {
                Cell* row = rowPtrAt(m_activeIndex, r);
                int& filled = rowFilledAt(m_activeIndex, r);
                if (blankDefault) {
                    fillBlank(row, blank, filled);
                    filled = 0;
                } else {
                    fillBlank(row, blank, m_columns);
                    filled = m_columns;
                }
                rowMetaAt(m_activeIndex, r) = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
            markDirtyRange(top, bot);
            narrowedDirtyHints = true;
        }
    } else if (!fullRegionClear && n > 0 && shift == 1
        && (m_rows - (bot - top + 1)) < (bot - top)) {
        // Bumping the origin moves every logical row up by one in constant time,
        // which is what the band wants and what the rows outside it do not. So
        // those are put back: `m_rows - band` of them, against the `bot - top`
        // a slide of the band would have moved. The guard above is that trade.
        const int rows = m_rows;
        Cell** rowPointers = m_rowPtrs[m_activeIndex];
        RowState* rowState = m_rowState[m_activeIndex];
        // A deferral names a slot, and the rows outside the band change slots.
        resolveDeferredBlankSlot(m_activeIndex);
        const int origin = m_rowOrigin[m_activeIndex];
        const auto slotOf = [rows, origin](int logical) {
            int slot = origin + logical;
            if (slot >= rows)
                slot -= rows;
            return slot;
        };
        // The band's first row leaves it, and its buffer becomes the blank one
        // revealed at the far edge — saved before an outside row takes its slot.
        Cell* recycledRow = rowPointers[slotOf(top)];
        const RowState recycledState = rowState[slotOf(top)];
        // An outside row r must end up where logical r now maps, which is the slot
        // logical r + 1 occupied before the shift. Descending writes that slot
        // before reading its own, so neither loop overwrites what it has yet to
        // move.
        const auto restoreOutsideRow = [&](int logical) {
            const int from = slotOf(logical);
            const int to = slotOf(logical + 1);
            rowPointers[to] = rowPointers[from];
            rowState[to] = rowState[from];
        };
        for (int logical = top - 1; logical >= 0; --logical)
            restoreOutsideRow(logical);
        for (int logical = rows - 1; logical > bot; --logical)
            restoreOutsideRow(logical);
        // slot(bot + 1) is where logical bot maps after the bump, and whatever it
        // held has already been moved. For a band reaching the last row that is
        // slot(origin), which is why the anchored case needs no separate arm.
        const int blankSlot = slotOf(bot + 1 == rows ? 0 : bot + 1);
        rowPointers[blankSlot] = recycledRow;
        m_rowOrigin[m_activeIndex] = (origin + 1 == rows) ? 0 : origin + 1;
        if (blankDefault) {
            fillBlank(recycledRow, blank, recycledState.filled);
            rowState[blankSlot].filled = 0;
        } else {
            fillBlank(recycledRow, blank, m_columns);
            rowState[blankSlot].filled = m_columns;
        }
        rowState[blankSlot].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
        markDirtyRange(top, bot);
        narrowedDirtyHints = true;
    } else {
        normalizeRowRing(m_activeIndex);
        // Shift the cell model by rotating row pointers, not cell bytes. n > 0 = content moves up: the
        // `shift` rows leaving the band top recycle to its bottom (cleared); the rest slide up.
        Cell** rows = m_rowPtrs[m_activeIndex];
        RowState* state = m_rowState[m_activeIndex];
        if (n > 0 && shift == 1) {
            Cell* recycledRow = rows[top];
            const RowState recycledState = state[top];
            const int movingRows = bot - top;
            if (movingRows > 0) {
                // A band is rows, not bytes: a libc call moves a hundred-odd bytes
                // and the count is a runtime value, so it is a real call rather than
                // inlined stores. One pass over the two arrays together pays neither
                // the calls nor two walks of the band.
                for (int offset = 0; offset < movingRows; ++offset) {
                    const int to = top + offset;
                    rows[to] = rows[to + 1];
                    state[to] = state[to + 1];
                }
            }
            rows[bot] = recycledRow;
            state[bot] = recycledState;
            for (int r = bot - shift + 1; r <= bot; ++r) {
                if (blankDefault) {
                    fillBlank(rows[r], blank, state[r].filled);
                    state[r].filled = 0;
                } else {
                    fillBlank(rows[r], blank, m_columns);
                    state[r].filled = m_columns;
                }
                state[r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
        } else if (n > 0) {
            std::rotate(rows + top, rows + top + shift, rows + bot + 1);
            std::rotate(state + top, state + top + shift, state + bot + 1);
            for (int r = bot - shift + 1; r <= bot; ++r) {
                if (blankDefault) {
                    fillBlank(rows[r], blank, state[r].filled);
                    state[r].filled = 0;
                } else {
                    fillBlank(rows[r], blank, m_columns);
                    state[r].filled = m_columns;
                }
                state[r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
        } else if (shift == 1) {
            Cell* recycledRow = rows[bot];
            const RowState recycledState = state[bot];
            const int movingRows = bot - top;
            if (movingRows > 0) {
                // As the upward slide: a libc call for a hundred-odd bytes and a
                // runtime count, so it is a call rather than inlined stores. This
                // direction moves toward higher indices, so the pass runs backward
                // to leave what it has yet to move.
                for (int offset = movingRows - 1; offset >= 0; --offset) {
                    const int from = top + offset;
                    rows[from + 1] = rows[from];
                    state[from + 1] = state[from];
                }
            }
            rows[top] = recycledRow;
            state[top] = recycledState;
            for (int r = top; r < top + shift; ++r) {
                if (blankDefault) {
                    fillBlank(rows[r], blank, state[r].filled);
                    state[r].filled = 0;
                } else {
                    fillBlank(rows[r], blank, m_columns);
                    state[r].filled = m_columns;
                }
                state[r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
        } else {
            const int region = bot - top + 1;
            std::rotate(rows + top, rows + top + (region - shift), rows + bot + 1);
            std::rotate(state + top, state + top + (region - shift), state + bot + 1);
            for (int r = top; r < top + shift; ++r) {
                if (blankDefault) {
                    fillBlank(rows[r], blank, state[r].filled);
                    state[r].filled = 0;
                } else {
                    fillBlank(rows[r], blank, m_columns);
                    state[r].filled = m_columns;
                }
                state[r].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
            }
        }
    }
    // Queue the BB-pixel scroll: nextPendingScroll returns it, the PAL blits BB, ackScroll shifts
    // m_bbShadow in lockstep. Coalescing requires identical top/bot and the same active screen (an alt-flip
    // drops the valid bit, but the activeIndex check guards a future path that mutates it without clearing).
    if (m_pendingScroll.valid && m_pendingScroll.activeIndex == m_activeIndex && m_pendingScroll.top == top && m_pendingScroll.bot == bot && ((m_pendingScroll.delta > 0 && n > 0) || (m_pendingScroll.delta < 0 && n < 0))) {
        m_pendingScroll.delta += n;
        if (m_pendingScroll.delta > region)
            m_pendingScroll.delta = region;
        if (m_pendingScroll.delta < -region)
            m_pendingScroll.delta = -region;
    } else {
        m_pendingScroll.top = top;
        m_pendingScroll.bot = bot;
        m_pendingScroll.delta = n;
        m_pendingScroll.activeIndex = m_activeIndex;
        m_pendingScroll.valid = true;
    }
    (void)fullRegionClear;
    m_anyMutationsSinceLastPaint = true;
    if (!narrowedDirtyHints) {
        // All rows in the scrolled band may now diverge from shadow.
        //
        bumpRowRange(top, bot);
    }
}

void CellGrid::scrollRectangleCells(int top, int bot, int left, int right, int n, const Cell& blank,
    RectangleScrollDirection direction)
{
    if (top < 0 || bot >= m_rows || top > bot || left < 0 || right >= m_columns || left > right || n <= 0)
        return;
    const int height = bot - top + 1;
    if (n > height)
        n = height;
    const bool isDefaultBlankFill = isDefaultBlank(blank);

    // No claim about a destination's band extent; the row's live width stands in.
    static constexpr int extentUnknown = -2;

    // The ring answers where a logical row lives, and that row's cells, live width and metadata all hang
    // off the one answer. Asking again per field walks the ring afresh for a row already in hand, and a
    // band scroll asks for every row it moves on every line it scrolls.
    RowState* const rowStates = m_rowState[m_activeIndex];

    // One row of the rectangle: the span arrives whole, so the bookkeeping a per-cell write repeats for
    // every column is settled once from a single sequential read of the source.
    //
    // `knownDestinationExtent` is the destination's last occupied column inside the band when the caller
    // already knows it, or `extentUnknown`. Returns the SOURCE's, which is what the caller then knows
    // about the row it is about to write over next.
    const auto moveRow = [&](int destinationRow, int sourceRow, int knownDestinationExtent) -> int {
        const int destinationSlot = physicalRowSlot(m_activeIndex, destinationRow);
        const int sourceSlot = physicalRowSlot(m_activeIndex, sourceRow);
        Cell* destination = rowPtrAtSlot(m_activeIndex, destinationSlot);
        const Cell* source = rowPtrAtSlot(m_activeIndex, sourceSlot);
        if (!destination || !source)
            return extentUnknown;
        // A source span beyond the row's live width is blank throughout, which is the common case under a
        // scrolling pane and settles every question below without reading a cell.
        const int sourceFilled = rowStates[sourceSlot].filled;
        int lastOccupiedColumn = -1;
        bool needsShaping = false;
        bool hasMulticell = false;
        bool hasBlinkAttribute = false;
        bool hasSizedText = false;
        if (sourceFilled > left) {
            const int scanEnd = sourceFilled - 1 < right ? sourceFilled - 1 : right;
            // The source row's own flags answer for every cell in it: both are latched by the writes that
            // would make them true and are never stale-false, so without them no cell of the span can
            // need shaping or be a multicell half, and the per-cell lookup and test have one answer.
            const RowMeta& sourceMeta = rowStates[sourceSlot].meta;
            const bool mayShape = sourceMeta.has(Ubiquitty::RowFlag::NeedsShaping);
            const bool mayBeMulticell = sourceMeta.has(Ubiquitty::RowFlag::HasMulticell);
            // The blink and sized hints answer the same way one rank up: each covers the whole grid and is
            // never stale-false, so a clear one states that no cell anywhere carries the attribute, the
            // span among them. With all four answers settled ahead of it the attribute walk has nothing
            // left to find, and the span is read once to be copied rather than once to be searched and
            // again to be copied.
            const bool mayBlink = m_blinkHint;
            const bool maySized = m_hasSizedCellHint;
            // Walked from the far end, where the answer is the first cell met unless the span's tail is
            // blank. The width cannot stand in for the answer: a blank written over content through
            // setCell leaves the width where it was, so the column it names need not be occupied — and
            // under horizontal margins the width belongs to the whole row, so a neighbouring pane starts
            // the walk at the band's far edge whatever this band holds. That distance is the scan, so it
            // is a vector one.
            //
            // A cell counts as occupied when it carries a code point or differs from the blank. Those two
            // are the same question only while the blank's own code point is zero; when it is not, every
            // cell in the span carries one and the far end is the answer.
            if (!m_haveDefaultBlank || m_defaultBlank.codePoint != 0)
                lastOccupiedColumn = scanEnd;
            else
                lastOccupiedColumn = lastDifferingCell(source, left, scanEnd, m_defaultBlank);
            if (mayShape || mayBeMulticell || mayBlink || maySized) {
                for (int column = left; column <= scanEnd; ++column) {
                    const Cell& cell = source[column];
                    if (mayShape && Unicode::mayNeedShaping(cell.codePoint))
                        needsShaping = true;
                    const auto& pen = cellPen(cell);
                    if (mayBeMulticell && pen.attributes.containsAny(multicellHalves))
                        hasMulticell = true;
                    if (mayBlink && pen.attributes.contains(Ubiquitty::VT::Attribute::Blink))
                        hasBlinkAttribute = true;
                    if (maySized && pen.extendedAttributes.textSizeSpecID() != 0)
                        hasSizedText = true;
                }
            }
        }
        // The copy stops where the BAND's content stops, which is what the backward scan above just
        // found. A row's live width covers the whole row, so a neighbouring pane's text — or the single
        // divider column tmux draws between panes — holds it at the full width however little of this
        // band is occupied, and bounding the copy by it moves the band's blank tail on every line.
        // Past the band's own last occupied column the source is default blank, by that scan for the
        // span it walked and by the width invariant beyond it.
        int& filled = rowStates[destinationSlot].filled;
        const int liveEnd = lastOccupiedColumn >= 0 ? lastOccupiedColumn + 1 : left;
        if (liveEnd > left)
            std::memmove(destination + left, source + left, static_cast<std::size_t>(liveEnd - left) * sizeof(Cell));
        // What the destination held past what arrived has to go — but only as far as the destination's
        // own content reached inside the band. Its live width is no guide: it spans the whole row, so a
        // neighbouring pane holds it at the full width and blanking to it writes the band's blank tail
        // every line. When the caller knows the extent the bound is exact; otherwise the width stands in.
        const int destinationBandEnd = knownDestinationExtent != extentUnknown
            ? knownDestinationExtent + 1
            : filled;
        const int staleEnd = destinationBandEnd < right + 1 ? destinationBandEnd : right + 1;
        for (int column = liveEnd > left ? liveEnd : left; column < staleEnd; ++column)
            destination[column] = m_defaultBlank;
        bumpRow(destinationRow);
        if (needsShaping)
            rowMeta(destinationRow).set(Ubiquitty::RowFlag::NeedsShaping, true);
        if (hasMulticell)
            rowMeta(destinationRow).set(Ubiquitty::RowFlag::HasMulticell, true);
        if (hasBlinkAttribute)
            noteBlinkWrite();
        if (hasSizedText)
            noteSizedWrite();
        // Content extends the destination's live width and never shrinks it, as a per-cell write would.
        if (lastOccupiedColumn >= filled)
            filled = lastOccupiedColumn + 1;
        return lastOccupiedColumn;
    };

    // The revealed rows. A default blank leaves the live width alone; a styled one is content.
    const auto blankRow = [&](int row) {
        const int slot = physicalRowSlot(m_activeIndex, row);
        Cell* cells = rowPtrAtSlot(m_activeIndex, slot);
        if (!cells)
            return;
        for (int column = left; column <= right; ++column)
            cells[column] = blank;
        bumpRow(row);
        if (!isDefaultBlankFill) {
            int& filled = rowStates[slot].filled;
            if (right >= filled)
                filled = right + 1;
        }
    };

    m_anyMutationsSinceLastPaint = true;
    if (direction == RectangleScrollDirection::Up) {
        // Moving up by one, each destination is the row the previous step read as its source, so the
        // extent that step measured is exactly what this one needs. Any other distance breaks the chain
        // and each row answers from its own width.
        int knownExtent = extentUnknown;
        for (int row = top; row + n <= bot; ++row) {
            const int sourceExtent = moveRow(row, row + n, knownExtent);
            knownExtent = n == 1 ? sourceExtent : extentUnknown;
        }
        for (int row = bot - n + 1; row <= bot; ++row)
            blankRow(row);
    } else {
        // Downward the destinations run ahead of the rows already read, so nothing is known in advance.
        for (int row = bot; row - n >= top; --row)
            moveRow(row, row - n, extentUnknown);
        for (int row = top; row < top + n; ++row)
            blankRow(row);
    }
}

namespace {

// Fills `count` cells with `blank`, two cells per iteration from a pattern
// loaded once outside the loop. Written by hand: a plain loop and std::fill_n
// both read the pattern back from memory for every cell.
void blankCellRun(Cell* out, int count, const Cell& blank)
{
    if (count <= 0)
        return;
    static_assert(sizeof(Cell) == 32, "the pair-per-cache-line fill below assumes a 32-byte Cell");
    static_assert(std::is_trivially_copyable_v<Cell>, "a Cell is filled as bytes here");
#if defined(__ARM_NEON)
    const auto* pattern = reinterpret_cast<const std::uint8_t*>(&blank);
    const uint8x16_t low = vld1q_u8(pattern);
    const uint8x16_t high = vld1q_u8(pattern + 16);
    auto* bytes = reinterpret_cast<std::uint8_t*>(out);
    int remaining = count;
    for (; remaining >= 2; remaining -= 2, bytes += 64) {
        vst1q_u8(bytes, low);
        vst1q_u8(bytes + 16, high);
        vst1q_u8(bytes + 32, low);
        vst1q_u8(bytes + 48, high);
    }
    if (remaining > 0) {
        vst1q_u8(bytes, low);
        vst1q_u8(bytes + 16, high);
    }
#else
    for (int index = 0; index < count; ++index)
        out[index] = blank;
#endif
}

} // namespace

void CellGrid::bandLaneDefer(int bandBottomRow, int left, int right, const Cell& blank)
{
    if (bandBottomRow < 0 || bandBottomRow >= m_rows || left < 0 || right >= m_columns || left > right)
        return;
    const int slot = physicalRowSlot(m_activeIndex, bandBottomRow);
    materializeCompactRow(m_activeIndex, slot);
    resolveDeferredBlankIfSlot(m_activeIndex, slot);
    Cell** ptrs = m_rowPtrs[m_activeIndex];
    RowState* states = m_rowState[m_activeIndex];
    if (m_bandStashCount == 0) {
        if (static_cast<int>(m_bandStash.size()) < bandLanePoolCapacity() + 1)
            m_bandStash.resize(static_cast<std::size_t>(bandLanePoolCapacity()) + 1);
        if (m_bandPoolColumnStride != m_columnCapacity
            || static_cast<int>(m_bandPool.size()) < bandLanePoolCapacity() * m_columnCapacity) {
            m_bandPool.assign(static_cast<std::size_t>(bandLanePoolCapacity()) * static_cast<std::size_t>(m_columnCapacity), Cell {});
            m_bandPoolColumnStride = m_columnCapacity;
        }
        m_bandSavedFilled = states[slot].filled;
        m_bandSavedMeta = states[slot].meta;
    }
    // The outgoing storage keeps the line it holds; the pool row takes the
    // cursor. Stash order is arrival order.
    m_bandStash[static_cast<std::size_t>(m_bandStashCount)] = ptrs[slot];
    ++m_bandStashCount;
    Cell* pool = m_bandPool.data()
        + static_cast<std::size_t>(m_bandStashCount - 1) * static_cast<std::size_t>(m_columnCapacity);
    ptrs[slot] = pool;
    // The revealed row a classic scroll would blank; the pool row's band
    // columns start as exactly that blank. Non-band columns stay junk: nothing
    // reads them -- every model read flushes first -- and the commit copies
    // band columns alone.
    blankCellRun(pool + left, right - left + 1, blank);
    states[slot].filled = right + 1;
    bumpRow(bandBottomRow);
}

void CellGrid::bandLaneCommit(int top, int bandBottomRow, int left, int right, int count, const Cell& blank)
{
    if (count <= 0 || count != m_bandStashCount) {
        m_bandStashCount = 0;
        return;
    }
    const int slot = physicalRowSlot(m_activeIndex, bandBottomRow);
    Cell** ptrs = m_rowPtrs[m_activeIndex];
    RowState* states = m_rowState[m_activeIndex];
    // The deferred lines live in stash[1..count-1] and the still-installed pool
    // row; the pre-burst bottom storage returns first so the rectangle move
    // sees the band exactly as the burst found it.
    Cell* const lastLine = ptrs[slot];
    ptrs[slot] = m_bandStash[0];
    states[slot].filled = m_bandSavedFilled;
    states[slot].meta = m_bandSavedMeta;
    scrollRectangleCells(top, bandBottomRow, left, right, count, blank, RectangleScrollDirection::Up);
    const bool mayBlink = m_blinkHint;
    const bool maySized = m_hasSizedCellHint;
    for (int index = 0; index < count; ++index) {
        const Cell* line = (index + 1 < count) ? m_bandStash[static_cast<std::size_t>(index + 1)] : lastLine;
        const int destinationRow = bandBottomRow - count + 1 + index;
        if (destinationRow < top)
            continue; // a run longer than the band: the overflow scrolled away
        const int destinationSlot = physicalRowSlot(m_activeIndex, destinationRow);
        Cell* destination = ptrs[destinationSlot];
        if (!destination || !line)
            continue;
        const int extent = lastDifferingCell(line, left, right, blank);
        const int liveEnd = extent >= 0 ? extent + 1 : left;
        if (liveEnd > left)
            std::memmove(destination + left, line + left, static_cast<std::size_t>(liveEnd - left) * sizeof(Cell));
        // The rectangle move's blankRow already blanked [liveEnd, right].
        bool needsShaping = false;
        bool hasMulticell = false;
        bool hasBlinkAttribute = false;
        bool hasSizedText = false;
        for (int column = left; column < liveEnd; ++column) {
            const Cell& cell = line[column];
            if (Unicode::mayNeedShaping(cell.codePoint))
                needsShaping = true;
            const auto& pen = cellPen(cell);
            if (pen.attributes.containsAny(multicellHalves))
                hasMulticell = true;
            if (mayBlink && pen.attributes.contains(Ubiquitty::VT::Attribute::Blink))
                hasBlinkAttribute = true;
            if (maySized && pen.extendedAttributes.textSizeSpecID() != 0)
                hasSizedText = true;
        }
        if (needsShaping)
            rowMeta(destinationRow).set(Ubiquitty::RowFlag::NeedsShaping, true);
        if (hasMulticell)
            rowMeta(destinationRow).set(Ubiquitty::RowFlag::HasMulticell, true);
        if (hasBlinkAttribute)
            noteBlinkWrite();
        if (hasSizedText)
            noteSizedWrite();
        int& filled = states[destinationSlot].filled;
        if (extent >= filled)
            filled = extent + 1;
        bumpRow(destinationRow);
    }
    m_bandStashCount = 0;
    m_anyMutationsSinceLastPaint = true;
}

void CellGrid::shiftRowCells(int row, int from, int rightBound, int n, const Cell& blank, RowShiftDirection direction)
{
    if (row < 0 || row >= m_rows || from < 0 || rightBound > m_columns || from >= rightBound)
        return;
    if (n > rightBound - from)
        n = rightBound - from;
    if (n <= 0)
        return;
    Cell* cells = rowPtrAt(m_activeIndex, row);
    if (!cells)
        return;
    const std::size_t movedBytes = static_cast<std::size_t>(rightBound - from - n) * sizeof(Cell);
    int blankFrom = from;
    if (direction == RowShiftDirection::InsertRight) {
        std::memmove(cells + from + n, cells + from, movedBytes);
    } else {
        std::memmove(cells + from, cells + from + n, movedBytes);
        blankFrom = rightBound - n;
    }
    for (int column = blankFrom; column < blankFrom + n; ++column)
        cells[column] = blank;

    // The cells that moved kept the row flags their own writes set, and a shift adds no code point the
    // row did not already hold. The blank does arrive, so its hints are the ones this has to raise.
    m_anyMutationsSinceLastPaint = true;
    bumpRow(row);
    if (cellPen(blank).attributes.contains(Ubiquitty::VT::Attribute::Blink))
        noteBlinkWrite();
    if (cellPen(blank).extendedAttributes.textSizeSpecID() != 0)
        noteSizedWrite();
    if (Unicode::mayNeedShaping(blank.codePoint))
        rowMeta(row).set(Ubiquitty::RowFlag::NeedsShaping, true);
    // A shift is content, as the span store it replaces was: the live width reaches past the window.
    noteFilled(row, rightBound);
}

// Rotation core of scrollFullViewportUpOneWithBlank without the per-scroll bookkeeping (see
// scrollLaneCommit). Keep the two bodies textually adjacent: a change to one is a change to the other.
void CellGrid::scrollLaneRotateOnly(const Cell& blank)
{
    if (m_rows <= 0)
        return;
    const bool isBlankDefault = isDefaultBlank(blank);

    ++m_rowOrigin[m_activeIndex];
    if (m_rowOrigin[m_activeIndex] == m_rows)
        m_rowOrigin[m_activeIndex] = 0;

    const int bottomSlot = physicalRowSlot(m_activeIndex, m_rows - 1);
    if (!isBlankDefault)
        materializeCompactRow(m_activeIndex, bottomSlot);
    Cell* row = m_rowPtrs[m_activeIndex][bottomSlot];
    int& filled = m_rowState[m_activeIndex][bottomSlot].filled;
    if (isBlankDefault && m_rowState[m_activeIndex][bottomSlot].storage == RowStorage::DefaultASCII) {
        resolveDeferredBlankSlot(m_activeIndex);
        filled = 0;
    } else if (isBlankDefault) {
        if (m_deferredBlankSlot[m_activeIndex] >= 0 && m_deferredBlankSlot[m_activeIndex] != bottomSlot) {
            resolveDeferredBlankSlot(m_activeIndex);
        }
        if (filled > 0) {
            m_deferredBlankSlot[m_activeIndex] = bottomSlot;
            if (filled > m_deferredBlankCount[m_activeIndex])
                m_deferredBlankCount[m_activeIndex] = filled;
            m_deferredBlankStart[m_activeIndex] = 0;
            filled = 0;
        }
    } else {
        resolveDeferredBlankIfSlot(m_activeIndex, bottomSlot);
        fillBlank(row, blank, m_columns);
        filled = m_columns;
    }
    m_rowState[m_activeIndex][bottomSlot].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
}

void CellGrid::scrollLaneCommit(int scrolledLines)
{
    if (scrolledLines <= 0)
        return;
    markDirtyRange(0, m_rows - 1);
    if (m_pendingScroll.valid && m_pendingScroll.activeIndex == m_activeIndex && m_pendingScroll.top == 0 && m_pendingScroll.bot == m_rows - 1 && m_pendingScroll.delta > 0) {
        m_pendingScroll.delta += scrolledLines;
        if (m_pendingScroll.delta > m_rows)
            m_pendingScroll.delta = m_rows;
    } else {
        m_pendingScroll.top = 0;
        m_pendingScroll.bot = m_rows - 1;
        m_pendingScroll.delta = scrolledLines <= m_rows ? scrolledLines : m_rows;
        m_pendingScroll.activeIndex = m_activeIndex;
        m_pendingScroll.valid = true;
    }
    m_anyMutationsSinceLastPaint = true;
}

void CellGrid::scrollFullViewportUpOne(const Cell& blank) { scrollFullViewportUpOneWithBlank(blank, isDefaultBlank(blank)); }

void CellGrid::scrollFullViewportUpOneDefaultBlank() { scrollFullViewportUpOneWithBlank(m_defaultBlank, true); }

void CellGrid::scrollFullViewportUpOneWithBlank(const Cell& blank, bool blankDefault)
{
    if (m_rows <= 0)
        return;

    ++m_rowOrigin[m_activeIndex];
    if (m_rowOrigin[m_activeIndex] == m_rows)
        m_rowOrigin[m_activeIndex] = 0;

    const int bottomSlot = physicalRowSlot(m_activeIndex, m_rows - 1);
    if (!blankDefault)
        materializeCompactRow(m_activeIndex, bottomSlot);
    Cell* row = m_rowPtrs[m_activeIndex][bottomSlot];
    int& filled = m_rowState[m_activeIndex][bottomSlot].filled;
    if (blankDefault && m_rowState[m_activeIndex][bottomSlot].storage == RowStorage::DefaultASCII) {
        resolveDeferredBlankSlot(m_activeIndex);
        filled = 0;
    } else if (blankDefault) {
        // Defer the physical blank (see m_deferredBlankSlot): the next line's write usually covers this
        // row, eliding the fill. At most one row per screen rides deferred; a second scroll materializes it.
        if (m_deferredBlankSlot[m_activeIndex] >= 0 && m_deferredBlankSlot[m_activeIndex] != bottomSlot) {
            resolveDeferredBlankSlot(m_activeIndex);
        }
        if (filled > 0) {
            m_deferredBlankSlot[m_activeIndex] = bottomSlot;
            if (filled > m_deferredBlankCount[m_activeIndex])
                m_deferredBlankCount[m_activeIndex] = filled;
            m_deferredBlankStart[m_activeIndex] = 0;
            filled = 0;
        }
    } else {
        resolveDeferredBlankIfSlot(m_activeIndex, bottomSlot);
        fillBlank(row, blank, m_columns);
        filled = m_columns;
    }
    m_rowState[m_activeIndex][bottomSlot].meta = RowMeta { lineAttributeNormal, 0, {}, 0 };
    markDirtyRange(0, m_rows - 1);

    if (m_pendingScroll.valid && m_pendingScroll.activeIndex == m_activeIndex && m_pendingScroll.top == 0 && m_pendingScroll.bot == m_rows - 1 && m_pendingScroll.delta > 0) {
        ++m_pendingScroll.delta;
        if (m_pendingScroll.delta > m_rows)
            m_pendingScroll.delta = m_rows;
    } else {
        m_pendingScroll.top = 0;
        m_pendingScroll.bot = m_rows - 1;
        m_pendingScroll.delta = 1;
        m_pendingScroll.activeIndex = m_activeIndex;
        m_pendingScroll.valid = true;
    }
    m_anyMutationsSinceLastPaint = true;
}

const Cell* CellGrid::cellAt(Row r, Column c) const
{
    const int ri = r.value(), ci = c.value();
    if (ri < 0 || ri >= m_rows || ci < 0 || ci >= m_columns)
        return nullptr;
    return &rowPtrAt(m_activeIndex, ri)[ci];
}

const Cell* CellGrid::logicalCellAtSlot(int screen, int slot, int column) const
{
    assert(screen >= 0 && screen < static_cast<int>(std::size(m_rowPtrs)));
    assert(slot >= 0 && slot < m_rowCapacity);
    assert(column >= 0 && column < m_columns);
    // A deferred row is the default blank exactly on the recorded stale window
    // [start, count): that is precisely what materialization fills, so the
    // answer here equals cellAt's after it by construction — no reliance on
    // the [filled, columns) width invariant, which Fixed-mode resize can
    // break. Below the start are live cells written while consuming the window;
    // past the count the storage byte is the answer on either path.
    if (slot == m_deferredBlankSlot[screen]) [[unlikely]] {
        const int staleCount = m_deferredBlankCount[screen] <= m_columnCapacity ? m_deferredBlankCount[screen] : m_columnCapacity;
        if (column >= m_deferredBlankStart[screen] && column < staleCount)
            return &m_defaultBlank;
    }
    return &m_rowPtrs[screen][slot][column];
}

[[gnu::noinline]] const Cell* CellGrid::materializedLogicalCellAt(int screen, int slot, int column) const
{
    const_cast<CellGrid*>(this)->materializeCompactRow(screen, slot);
    return logicalCellAtSlot(screen, slot, column);
}

const Cell* CellGrid::logicalCellAt(Row r, Column c) const
{
    const int row = r.value(), column = c.value();
    if (row < 0 || row >= m_rows || column < 0 || column >= m_columns)
        return nullptr;
    assert(m_rowState[m_activeIndex]);
    const int slot = physicalRowSlot(m_activeIndex, row);
    if (m_rowState[m_activeIndex][slot].storage == RowStorage::DefaultASCII)
        return materializedLogicalCellAt(m_activeIndex, slot, column);
    return logicalCellAtSlot(m_activeIndex, slot, column);
}

const Cell* CellGrid::cellAt(ScreenMode screen, Row r, Column c) const
{
    const int ri = r.value(), ci = c.value();
    if (ri < 0 || ri >= m_rows || ci < 0 || ci >= m_columns)
        return nullptr;
    const int screenIndex = (screen == ScreenMode::Alternate) ? 1 : 0;
    return &rowPtrAt(screenIndex, ri)[ci];
}

CellRead CellGrid::logicalCellReadAt(Row row, Column column) const
{
    if (row.value() < 0 || row.value() >= m_rows || column.value() < 0 || column.value() >= m_columns)
        return {};
    assert(m_rowState[m_activeIndex]);
    const int slot = physicalRowSlot(m_activeIndex, row.value());
    const auto& state = m_rowState[m_activeIndex][slot];
    if (state.storage != RowStorage::DefaultASCII)
        return CellRead(logicalCellAtSlot(m_activeIndex, slot, column.value()));
    assert(slot != m_deferredBlankSlot[m_activeIndex]);
    Cell cell = m_defaultBlank;
    if (column.value() < state.filled)
        cell.codePoint = reinterpret_cast<const std::uint8_t*>(m_rowPtrs[m_activeIndex][slot])[column.value()];
    return CellRead(cell);
}

std::optional<Cell> CellGrid::logicalCellValueAt(Row row, Column column) const
{
    const auto read = logicalCellReadAt(row, column);
    if (const auto* cell = read.get())
        return *cell;
    return std::nullopt;
}

std::optional<std::span<const std::uint8_t>> CellGrid::compactASCIIRow(Row row) const
{
    if (row.value() < 0 || row.value() >= m_rows)
        return std::nullopt;
    const int slot = physicalRowSlot(m_activeIndex, row.value());
    const auto& state = m_rowState[m_activeIndex][slot];
    if (state.storage != RowStorage::DefaultASCII)
        return std::nullopt;
    assert(state.filled >= 0 && state.filled <= m_columns);
    return std::span { reinterpret_cast<const std::uint8_t*>(m_rowPtrs[m_activeIndex][slot]), static_cast<std::size_t>(state.filled) };
}

RowView CellGrid::rowCells(Row r) const
{
    const int ri = r.value();
    if (ri < 0 || ri >= m_rows)
        return RowView { nullptr, 0 };
    return RowView { rowPtrAt(m_activeIndex, ri), m_columns };
}

RowView CellGrid::rowCells(ScreenMode screen, Row r) const
{
    const int ri = r.value();
    if (ri < 0 || ri >= m_rows)
        return RowView { nullptr, 0 };
    const int screenIndex = (screen == ScreenMode::Alternate) ? 1 : 0;
    return RowView { rowPtrAt(screenIndex, ri), m_columns };
}

void CellGrid::readRow(Row r, Cell* out) const
{
    const int ri = r.value();
    if (ri < 0 || ri >= m_rows || !out)
        return;
    std::memcpy(out, rowPtrAt(m_activeIndex, ri), std::size_t(m_columns) * sizeof(Cell));
}

void CellGrid::readRow(ScreenMode screen, Row r, Cell* out) const
{
    const int ri = r.value();
    if (ri < 0 || ri >= m_rows || !out)
        return;
    const int screenIndex = (screen == ScreenMode::Alternate) ? 1 : 0;
    std::memcpy(out, rowPtrAt(screenIndex, ri), std::size_t(m_columns) * sizeof(Cell));
}

void CellGrid::setAlternateActive(ScreenMode screen)
{
    const int newIndex = (screen == ScreenMode::Alternate) ? 1 : 0;
    if (newIndex == m_activeIndex)
        return;
    m_activeIndex = newIndex;
    // bb shadow stays put (it tracks BB pixels, unchanged yet); the (new-active != shadow) diff surfaces
    // what needs repainting. Cross-screen selection state is treated as ephemeral.
    clearAllSelectionDirty();
    // Cancel any pending scroll from the previous active screen — its rows reference the previous content.
    m_pendingScroll.valid = false;
    // The same absolute-row index now maps to the other screen, so its shaped-row cache entries are stale.
    invalidateShapedRowCache();
    clearDirtyRange();
    m_anyMutationsSinceLastPaint = true;
    // Alt-screen swap — every row's cells may differ from shadow.
    bumpRowRange(0, m_rows - 1);
}

void CellGrid::clearSelectionDirty() { clearAllSelectionDirty(); }

void CellGrid::markSelectionFlipped(Row r, Column c0, Column c1)
{
    const int ri = r.value();
    int lo = c0.value(), hi = c1.value();
    if (ri < 0 || ri >= m_rows)
        return;
    if (lo < 0)
        lo = 0;
    if (hi >= m_columns)
        hi = m_columns - 1;
    for (int c = lo; c <= hi; ++c)
        setSelectionDirty(ri, c);
    m_anyMutationsSinceLastPaint = true;
    bumpRow(ri);
}

void CellGrid::invalidateShadow()
{
    // Marks the shadow not-matching-BB. nextDirtyRun's full flag is `m_fullRedraw || !m_bbShadowValid`, so
    // an invalid shadow already yields every cell and rebuilds the shadow — do not also flip
    // m_fullRedrawRequested (that would loop when the renderer's full-redraw bail calls this on the way out).
    // Pending scroll dropped: its queued BB shift is no longer valid against an unknown shadow.
    m_bbShadowValid = false;
    m_pendingScroll.valid = false;
    clearDirtyRange();
    m_anyMutationsSinceLastPaint = true;
}

void CellGrid::requestFullRedraw()
{
    m_fullRedrawRequested = true;
    m_anyMutationsSinceLastPaint = true;
    // A full repaint rewrites every cell, subsuming any queued (not-yet-blitted) pending scroll — drop it
    // rather than blit pixels the next paint overwrites. It's only queued, so cancelling touches no pixels.
    m_pendingScroll.valid = false;
}

const Cell* CellGrid::readShadowCell(int row, int column) const
{
    if (!m_bbShadowValid)
        return nullptr;
    if (row < 0 || row >= m_rows || column < 0 || column >= m_columns)
        return nullptr;
    return &shadowRow(row)[column];
}

void CellGrid::writeShadowCell(int row, int column, const Cell& cell)
{
    if (row < 0 || row >= m_rows || column < 0 || column >= m_columns)
        return;
    shadowRow(row)[column] = cell;
}

void CellGrid::markShadowValid() { m_bbShadowValid = true; }

void CellGrid::shiftShadowForViewOffset(int topR, int botR, int n)
{
    // Same lockstep shift ackScroll uses for output-driven scrolls. The PAL already blit-shifted BB and
    // window pixels; the shadow follows so the next diff doesn't flag every row dirty (a full repaint per
    // scrollbar tick). No-op if shadow already invalid.
    shadowShiftRows(Row(topR), Row(botR), n);
}

void CellGrid::invalidateRowShadow(int row)
{
    // Zero the shadow row so the next paint repaints every non-blank cell on it — O(columns) vs
    // requestFullRedraw's O(rows × columns). The blink tick handler walks rows-with-blink and invalidates each.
    if (row < 0 || row >= m_rows)
        return;
    for (int c = 0; c < m_columns; c++) {
        shadowRow(row)[c] = Cell();
    }
    bumpRow(row);
    m_anyMutationsSinceLastPaint = true;
}

bool CellGrid::hasUnpaintedWork() const { return m_anyMutationsSinceLastPaint || m_pendingScroll.valid || m_fullRedrawRequested || !m_bbShadowValid; }

void CellGrid::shadowShiftRows(Row topR, Row botR, int n)
{
    const int top = topR.value(), bot = botR.value();
    if (!m_bbShadowValid)
        return;
    if (top < 0 || bot >= m_rows || top > bot)
        return;
    const int shift = n > 0 ? n : -n;
    if (shift == 0 || shift > bot - top)
        return;
    const std::size_t rowBytes = std::size_t(m_columns) * sizeof(Cell);
    if (n > 0) {
        for (int r = top; r <= bot - shift; ++r)
            std::memcpy(shadowRow(r), shadowRow(r + shift), rowBytes);
    } else {
        for (int r = bot; r >= top + shift; --r)
            std::memcpy(shadowRow(r), shadowRow(r - shift), rowBytes);
    }
}

void CellGrid::clearAllSelectionDirty()
{
    if (!m_anySelectionDirty)
        return;
    std::memset(m_selectionDirty, 0, std::size_t(m_rowCapacity) * std::size_t(m_selectionStride));
    m_anySelectionDirty = false;
}

void CellGrid::markDirtyRange(int top, int bot)
{
    if (top < 0) {
        top = 0;
    }
    if (bot >= m_rows) {
        bot = m_rows - 1;
    }
    if (top > bot) {
        return;
    }
    // No cache exists until the first shaped row, so generation work before then invalidates nothing.
    // A full-grid mutation drops the cache via its epoch; partial bands retain per-row keys.
    if (m_shapedRowSlots) {
        if (top == 0 && bot == m_rows - 1) {
            invalidateShapedRowCache();
        } else {
            for (int r = top; r <= bot; ++r) {
                ++m_rowGeneration[r];
            }
        }
    }
    if (!m_dirtyRangeValid) {
        m_dirtyRangeTop = top;
        m_dirtyRangeBottom = bot;
        m_dirtyRangeValid = true;
        return;
    }
    if (top < m_dirtyRangeTop) {
        m_dirtyRangeTop = top;
    }
    if (bot > m_dirtyRangeBottom) {
        m_dirtyRangeBottom = bot;
    }
}

// --- shaped-row cache ------------------------------------------------------

uint32_t CellGrid::rowGeneration(int liveRow) const
{
    if (liveRow < 0 || liveRow >= m_rowCapacity)
        return 0;
    return m_rowGeneration[liveRow];
}

bool CellGrid::lookupShapedRow(int absoluteRow, uint32_t generation, Cell* out, int columns) const
{
    if (!m_shapedRowSlots || m_shapedRowCapacity <= 0 || !out || absoluteRow < 0 || columns <= 0 || columns > maxColumns)
        return false;
    const int slot = absoluteRow % m_shapedRowCapacity;
    const ShapedRowSlot& s = m_shapedRowSlots[slot];
    // A hit requires matching row identity, generation, epoch, and width; any mismatch re-shapes.
    if (s.epoch != m_shapedRowEpoch || s.absoluteRow != absoluteRow || s.generation != generation || s.length != columns)
        return false;
    std::memcpy(out, m_shapedRowCells + std::size_t(slot) * maxColumns, std::size_t(columns) * sizeof(Cell));
    return true;
}

void CellGrid::storeShapedRow(int absoluteRow, uint32_t generation, const Cell* shaped, int columns) const
{
    if (!shaped || absoluteRow < 0 || columns <= 0 || columns > maxColumns)
        return;
    // Lazy: a pane with no RTL/combining content never shapes, so never allocates
    // the cache. Sized to the live rows, and re-sized when a taller screen would
    // otherwise fold two of its rows onto one slot. Entries do not survive the
    // re-size: the slot a row maps to moves with the size.
    const int wantedCapacity = m_rows > 0 ? m_rows : 1;
    if (!m_shapedRowSlots || m_shapedRowCapacity < wantedCapacity) {
        delete[] m_shapedRowSlots;
        delete[] m_shapedRowCells;
        m_shapedRowSlots = nullptr;
        m_shapedRowCells = nullptr;
        m_shapedRowCapacity = 0;
        // A cache that cannot be sized is no cache: the row is shaped again on
        // its next fetch, which is what an absent cache already means. The
        // Core builds without exceptions, so a throwing allocation that fails
        // here would end the process over a memo.
        std::unique_ptr<ShapedRowSlot[]> slots(new (std::nothrow) ShapedRowSlot[std::size_t(wantedCapacity)]);
        std::unique_ptr<Cell[]> cells(new (std::nothrow) Cell[std::size_t(wantedCapacity) * maxColumns]);
        if (!slots || !cells)
            return;
        m_shapedRowCapacity = wantedCapacity;
        m_shapedRowSlots = slots.release();
        m_shapedRowCells = cells.release();
        ++m_shapedRowEpoch;
    }
    const int slot = absoluteRow % m_shapedRowCapacity;
    ShapedRowSlot& s = m_shapedRowSlots[slot];
    s.absoluteRow = absoluteRow;
    s.generation = generation;
    s.epoch = m_shapedRowEpoch;
    s.length = columns;
    std::memcpy(m_shapedRowCells + std::size_t(slot) * maxColumns, shaped, std::size_t(columns) * sizeof(Cell));
}

void CellGrid::dropShapedRowCache() const
{
    delete[] m_shapedRowSlots;
    m_shapedRowSlots = nullptr;
    delete[] m_shapedRowCells;
    m_shapedRowCells = nullptr;
    m_shapedRowCapacity = 0;
    ++m_shapedRowEpoch;
}

void CellGrid::clearDirtyRange()
{
    m_dirtyRangeValid = false;
    m_dirtyRangeTop = 0;
    m_dirtyRangeBottom = 0;
}

void CellGrid::materializeDirtyRange()
{
    if (!m_dirtyRangeValid) {
        return;
    }
    for (int r = m_dirtyRangeTop; r <= m_dirtyRangeBottom; ++r) {
        m_anyDirtyInRow[r] = true;
    }
    clearDirtyRange();
}

bool CellGrid::isSelectionDirty(int r, int c) const { return (selectionRow(r)[c >> 3] & static_cast<unsigned char>(1 << (c & 7))) != 0; }

void CellGrid::setSelectionDirty(int r, int c)
{
    selectionRow(r)[c >> 3] |= static_cast<unsigned char>(1 << (c & 7));
    m_anySelectionDirty = true;
}

#if !defined(NDEBUG)
bool CellGrid::assertInvariants() const
{
    // The iterator's O(1) row skip trusts m_anyDirtyInRow: if false, active must equal shadow for every
    // column and no selection-dirty bit set. A false negative would skip real paint work. Verify every clean row.
    for (int r = 0; r < m_rows; ++r) {
        if (m_anyDirtyInRow[r])
            continue;
        const bool pendingScrollRow = m_pendingScroll.valid && m_pendingScroll.activeIndex == m_activeIndex && r >= m_pendingScroll.top && r <= m_pendingScroll.bot;
        for (int c = 0; c < m_columns; ++c) {
            if (!pendingScrollRow && !cellEqual(rowPtrAt(m_activeIndex, r)[c], shadowRow(r)[c]))
                return false;
            if (isSelectionDirty(r, c))
                return false;
        }
    }
    // Pending-scroll geometry must fit its band and delta stay within [-region, +region], else ackScroll
    // reads past the band edges (on-screen smear after a scroll).
    if (m_pendingScroll.valid) {
        const int region = m_pendingScroll.bot - m_pendingScroll.top + 1;
        if (region <= 0 || m_pendingScroll.top < 0 || m_pendingScroll.bot >= m_rows)
            return false;
        if (m_pendingScroll.delta > region || m_pendingScroll.delta < -region)
            return false;
        if (m_pendingScroll.activeIndex != 0 && m_pendingScroll.activeIndex != 1)
            return false;
    }
    // Per-row metadata rides the same ring as cells: both screens' m_rowState must be
    // allocated alongside m_rowPtrs so a row's metadata moves in the same slot as its content.
    for (int s = 0; s < 2; ++s) {
        if (!m_rowPtrs[s] || !m_rowState[s])
            return false;
    }
    // Content-width-bound honesty: cells [filled, m_columns) are the default blank, so a scroll/capture
    // trusting the bound can't drop real content. Only checkable once a default blank is known.
    if (m_haveDefaultBlank) {
        for (int r = 0; r < m_rows; ++r) {
            const Cell* row = rowPtrAt(m_activeIndex, r);
            int filled = rowFilledAt(m_activeIndex, r);
            if (filled < 0)
                filled = 0;
            for (int c = filled; c < m_columns; ++c) {
                if (!isDefaultBlank(row[c]))
                    return false;
            }
        }
    }
    return true;
}
#endif

PaintSession CellGrid::beginPaint() { return PaintSession(this); }

// --- PaintSession --------------------------------------------------

PaintSession::PaintSession(CellGrid* d)
    : m_display(d)
    , m_cursorRow(0)
    , m_cursorColumn(0)
    , m_fullRedraw(d->m_fullRedrawRequested)
    , m_moved(false)
    , m_consumedDamage(false)
    , m_hasWalkedToEnd(false)
#ifndef NDEBUG
    , m_yieldedRuns(0)
    , m_ackedRuns(0)
#endif
{
    // Clear the request as the session starts; the first iteration captured it, later cycles aren't full.
    m_display->m_fullRedrawRequested = false;
}

PaintSession::PaintSession(PaintSession&& o) noexcept
    : m_display(o.m_display)
    , m_cursorRow(o.m_cursorRow)
    , m_cursorColumn(o.m_cursorColumn)
    , m_fullRedraw(o.m_fullRedraw)
    , m_moved(o.m_moved)
    , m_consumedDamage(o.m_consumedDamage)
#ifndef NDEBUG
    , m_yieldedRuns(o.m_yieldedRuns)
    , m_ackedRuns(o.m_ackedRuns)
#endif
{
    o.m_moved = true;
}

PaintSession& PaintSession::operator=(PaintSession&& o) noexcept
{
    if (this != &o) {
        m_display = o.m_display;
        m_cursorRow = o.m_cursorRow;
        m_cursorColumn = o.m_cursorColumn;
        m_fullRedraw = o.m_fullRedraw;
        m_moved = o.m_moved;
        m_consumedDamage = o.m_consumedDamage;
#ifndef NDEBUG
        m_yieldedRuns = o.m_yieldedRuns;
        m_ackedRuns = o.m_ackedRuns;
#endif
        o.m_moved = true;
    }
    return *this;
}

PaintSession::~PaintSession()
{
    if (m_moved)
        return;
    // A session that acked any run/scroll advanced the shadow baseline — bump the generation so a peer
    // snapshot buffer sees the baseline moved (captureCanReuse). A no-op paint leaves it untouched.
    if (m_consumedDamage)
        ++m_display->m_damageGeneration;
    // The shadow describes the whole screen only when this walk offered the whole screen. A caller that
    // stopped early left divergent cells unoffered, and calling the shadow valid tells the next walk those
    // rows match: it skips them by their clean dirty hints and the pixels stay stale.
    if (m_hasWalkedToEnd && !m_display->m_pendingScroll.valid) {
        m_display->m_bbShadowValid = true;
    }
    // The constructor consumed any full-redraw request. A walk that did not reach the end has not honoured
    // it, so the request stands; re-arming is idempotent against one raised during this paint.
    if (!m_hasWalkedToEnd && m_fullRedraw) {
        m_display->m_fullRedrawRequested = true;
    }
    m_display->clearSelectionDirty();
#ifndef NDEBUG
    bool fullySynced = (m_yieldedRuns == m_ackedRuns);
    // Every yielded run must be acked; a mismatch means the PAL forgot AckRun and the shadow will drift.
    m_display->m_paintInvariantBroken = m_display->m_paintInvariantBroken || !fullySynced;
#else
    bool fullySynced = true;
#endif
    // Clean completion means the renderer is in sync with cells_ — drop the unpainted-work flag so the next
    // cursor-only-skip check can fire. Any later mutation resets it.
    if (fullySynced && m_hasWalkedToEnd && !m_display->m_pendingScroll.valid) {
        m_display->m_anyMutationsSinceLastPaint = false;
    }
}

bool PaintSession::nextPendingScroll(Screen::PendingScroll* out)
{
    if (m_moved || !m_display->m_pendingScroll.valid)
        return false;
    out->topRow = Row(m_display->m_pendingScroll.top);
    out->botRow = Row(m_display->m_pendingScroll.bot);
    out->shiftRows = m_display->m_pendingScroll.delta;
    return true;
}

void PaintSession::ackScroll(const Screen::PendingScroll& s)
{
    if (m_moved)
        return;
    // Shift m_bbShadow to match the BB pixels the PAL just blitted; the exposed band then surfaces through
    // the dirty-run iterator (cells_ != m_bbShadow there).
    m_display->shadowShiftRows(s.topRow, s.botRow, s.shiftRows);
    m_display->m_pendingScroll.valid = false;
    m_display->materializeDirtyRange();
    m_consumedDamage = true;
}

bool PaintSession::nextDirtyRun(PaintRun* out)
{
    if (m_moved)
        return false;
#ifndef NDEBUG
    assert(m_walkKind != WalkKind::Rows && "a session walks rows or runs, not both — they share one cursor");
    m_walkKind = WalkKind::Runs;
#endif
    m_display->materializeDirtyRange();
    const bool full = m_fullRedraw || !m_display->m_bbShadowValid;
    while (m_cursorRow < m_display->m_rows) {
        const int r = m_cursorRow;
        // O(1) row skip: no mutator touched this row and no full-redraw ⇒ active matches m_bbShadow for the
        // whole row, skip without the per-column scan. Only valid at column 0 (partial-row state means an
        // earlier scan already passed cells we'd want to yield).
        if (!full && m_cursorColumn == 0 && !m_display->m_anyDirtyInRow[r]) {
            ++m_cursorRow;
            m_cursorColumn = 0;
            continue;
        }
        // Find first dirty cell from m_cursorColumn on (dirty iff cells_ != m_bbShadow or selection flipped).
        // Hoist the row pointers so the inner loop doesn't recompute the row stride each iteration.
        const Cell* const rowActive = m_display->rowPtrAt(m_display->m_activeIndex, r);
        const Cell* const rowShadow = m_display->shadowRow(r);
        int c = m_cursorColumn;
        int runStart = -1;
        bool selectionOnly = false;
        // The ordinary frame carries no selection and no full redraw, and there
        // the question is only where the row stops matching its shadow — which
        // is a two-array scan, not a per-cell walk with two questions in it.
        if (!full && !m_display->m_anySelectionDirty) {
            const int firstDifferent = firstDifferingCellBetween(rowActive, rowShadow, c, m_display->m_columns - 1);
            if (firstDifferent < m_display->m_columns) {
                runStart = firstDifferent;
                selectionOnly = false;
            }
            c = firstDifferent;
        } else {
            while (c < m_display->m_columns) {
                const bool isContentDirty = full || !cellEqual(rowActive[c], rowShadow[c]);
                const bool isSelectionBitSet = m_display->isSelectionDirty(r, c);
                if (isContentDirty || isSelectionBitSet) {
                    runStart = c;
                    selectionOnly = !isContentDirty && isSelectionBitSet;
                    break;
                }
                ++c;
            }
        }
        if (runStart < 0) {
            // Nothing dirty from m_cursorColumn on. If we scanned from column 0 the hint over-claimed —
            // clear it so future frames skip this row in O(1). (Starting partway, an earlier yield already
            // consumed the dirty cells; the hint stays valid.)
            if (m_cursorColumn == 0)
                m_display->m_anyDirtyInRow[r] = false;
            ++m_cursorRow;
            m_cursorColumn = 0;
            continue;
        }
        // Extend the run while cells share the same pen + same
        // dirty/selection-only state.
        int runEnd = runStart;
        while (runEnd + 1 < m_display->m_columns) {
            const int next = runEnd + 1;
            const bool contentDirty = full || !cellEqual(rowActive[next], rowShadow[next]);
            const bool selectionBit = m_display->isSelectionDirty(r, next);
            const bool thisSelectionOnly = !contentDirty && selectionBit;
            if (!contentDirty && !selectionBit)
                break;
            if (thisSelectionOnly != selectionOnly)
                break;
            if (!sameRunPen(rowActive[runStart], rowActive[next]))
                break;
            runEnd = next;
        }
        out->row = Row(r);
        out->firstColumn = Column(runStart);
        out->lastColumn = Column(runEnd);
        out->selectionOnly = selectionOnly;
        m_cursorColumn = runEnd + 1;
#ifndef NDEBUG
        ++m_yieldedRuns;
#endif
        return true;
    }
    // Every row was examined, so every divergent cell has been offered.
    m_hasWalkedToEnd = true;
    return false;
}

bool PaintSession::nextDirtyRow(Row* out)
{
    if (m_moved)
        return false;
#ifndef NDEBUG
    assert(m_walkKind != WalkKind::Runs && "a session walks rows or runs, not both — they share one cursor");
    m_walkKind = WalkKind::Rows;
#endif
    m_display->materializeDirtyRange();
    const bool isFullFrame = m_fullRedraw || !m_display->m_bbShadowValid;
    while (m_cursorRow < m_display->m_rows) {
        const int r = m_cursorRow;
        ++m_cursorRow;
        m_cursorColumn = 0;
        if (isFullFrame) {
            *out = Row(r);
            return true;
        }
        // The same O(1) skip the run walk takes: no mutator touched this row, so
        // it matches its shadow whole.
        if (!m_display->m_anyDirtyInRow[r])
            continue;
        const Cell* const rowActive = m_display->rowPtrAt(m_display->m_activeIndex, r);
        const Cell* const rowShadow = m_display->shadowRow(r);
        // One two-array scan for the whole row, and the selection bits only
        // where a selection changed — which is almost never, and reading them
        // pulls their cache lines into a scan that has no use for them.
        bool isDirty = firstDifferingCellBetween(rowActive, rowShadow, 0, m_display->m_columns - 1) < m_display->m_columns;
        if (!isDirty && m_display->m_anySelectionDirty) {
            for (int c = 0; c < m_display->m_columns && !isDirty; ++c)
                isDirty = m_display->isSelectionDirty(r, c);
        }
        if (!isDirty) {
            // The hint over-claimed, so future frames skip this row in O(1).
            m_display->m_anyDirtyInRow[r] = false;
            continue;
        }
        *out = Row(r);
#ifndef NDEBUG
        ++m_yieldedRuns;
#endif
        return true;
    }
    // Every row was examined, so every divergent cell has been offered.
    m_hasWalkedToEnd = true;
    return false;
}

void PaintSession::ackRow(Row row)
{
    if (m_moved)
        return;
    const int r = row.value();
    if (r < 0 || r >= m_display->m_rows)
        return;
    std::memcpy(m_display->shadowRow(r), m_display->rowPtrAt(m_display->m_activeIndex, r),
        static_cast<std::size_t>(m_display->m_columns) * sizeof(Cell));
    if (m_display->m_anySelectionDirty) {
        unsigned char* selection = m_display->selectionRow(r);
        std::memset(selection, 0, static_cast<std::size_t>((m_display->m_columns + 7) / 8));
    }
    m_consumedDamage = true;
#ifndef NDEBUG
    ++m_ackedRuns;
#endif
}

void PaintSession::ackRun(const PaintRun& run)
{
    if (m_moved)
        return;
    const int r = run.row.value();
    const int c0 = run.firstColumn.value();
    const int c1 = run.lastColumn.value();
    if (r < 0 || r >= m_display->m_rows)
        return;
    // nextDirtyRun yields runs wholly within [0, m_columns) over a contiguous Cell span, so the
    // active→shadow copy is one memcpy. Selection-dirty bits clear separately.
    const int count = c1 - c0 + 1;
    std::memcpy(&m_display->shadowRow(r)[c0], &m_display->rowPtrAt(m_display->m_activeIndex, r)[c0], static_cast<std::size_t>(count) * sizeof(Cell));
    unsigned char* selection = m_display->selectionRow(r);
    for (int c = c0; c <= c1; ++c)
        selection[c >> 3] &= static_cast<unsigned char>(~(1 << (c & 7)));
    m_consumedDamage = true;
#ifndef NDEBUG
    ++m_ackedRuns;
#endif
}

} // namespace Ubiquitty
