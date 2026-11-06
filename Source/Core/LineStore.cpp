// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// LineStore — see LineStore.h. Block-slab cell storage for scrollback, and the
// fold of a cold block into the row form.

#include "Core/LineStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/RowForm.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace Ubiquitty {

// Copy one trimmed row into the append slab. On the flood path this runs
// once per scrolled-off line (~12M times per GB of pty traffic), so it is a
// full memory-bandwidth pass: the source row is cache-resident live screen,
// the destination slab is a streaming append cursor that the CPU has no
// other reason to cache — scrollback cells are written once and read back
// only on scroll-back/reflow. On ARM64 that makes non-temporal store pairs
// (stnp) the right tool: they hint the cache hierarchy not to keep the
// freshly written lines, which both avoids evicting the live screen and
// measured ~15-20% faster than libc memcpy on this exact access pattern
// (L2-resident 90-cell source rows, streaming 256 KB slabs; see
// Tests/Performance/bench_cellops.cpp, Apple M5 Pro). stnp is only a cache
// allocation hint — unlike x86 MOVNT it keeps normal store ordering and
// coherency, so the cross-thread handoff under the Ubiquitty::Terminal lock is unaffected.
#if defined(__aarch64__)
static inline void copyCellsIntoSlab(Cell* destination, const Cell* source, int cellCount)
{
    static_assert(sizeof(Cell) == 32, "the copy kernel walks 32-byte cells");
    constexpr int cellsPerStep = 4; // 128 B, one Apple Silicon cache line
    const uint8_t* sourceBytes = reinterpret_cast<const uint8_t*>(source);
    uint8_t* destinationBytes = reinterpret_cast<uint8_t*>(destination);
    int index = 0;
    // One cache line (128 B) per step — the same ldp/stnp body on both
    // layouts; only the cell count it covers changes.
    for (; index + cellsPerStep <= cellCount; index += cellsPerStep) {
        uint8x16_t chunk0, chunk1, chunk2, chunk3, chunk4, chunk5, chunk6, chunk7;
        asm volatile("ldp %q0, %q1, [%8]\n\t"
                     "ldp %q2, %q3, [%8, #32]\n\t"
                     "ldp %q4, %q5, [%8, #64]\n\t"
                     "ldp %q6, %q7, [%8, #96]\n\t"
                     "stnp %q0, %q1, [%9]\n\t"
                     "stnp %q2, %q3, [%9, #32]\n\t"
                     "stnp %q4, %q5, [%9, #64]\n\t"
                     "stnp %q6, %q7, [%9, #96]"
            : "=&w"(chunk0), "=&w"(chunk1), "=&w"(chunk2), "=&w"(chunk3), "=&w"(chunk4), "=&w"(chunk5), "=&w"(chunk6), "=&w"(chunk7)
            : "r"(sourceBytes), "r"(destinationBytes)
            : "memory");
        sourceBytes += 128;
        destinationBytes += 128;
    }
    for (; index < cellCount; ++index) {
        uint8x16_t chunk0, chunk1;
        asm volatile("ldp %q0, %q1, [%2]\n\t"
                     "stnp %q0, %q1, [%3]"
            : "=&w"(chunk0), "=&w"(chunk1)
            : "r"(sourceBytes), "r"(destinationBytes)
            : "memory");
        sourceBytes += 32;
        destinationBytes += 32;
    }
}
#else
static inline void copyCellsIntoSlab(Cell* destination, const Cell* source, int cellCount) { std::memcpy(destination, source, static_cast<std::size_t>(cellCount) * sizeof(Cell)); }
#endif

namespace {

using Ubiquitty::Protocol::Sync::WireReader;
using Ubiquitty::Protocol::Sync::WireWriter;

// A pen's bytes, which identify it independently of its layout. Pen has no
// padding — Cell's unique-object-representation assert covers it — so the
// bytes are the whole value.
struct PenKey {
    std::array<std::uint8_t, sizeof(Pen)> bytes {};
    friend bool operator==(const PenKey&, const PenKey&) = default;
};

struct PenKeyHash {
    std::size_t operator()(const PenKey& key) const
    {
        std::uint64_t hash = 0x9E3779B97F4A7C15ull;
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= key.bytes.size(); offset += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            std::memcpy(&word, key.bytes.data() + offset, sizeof(word));
            hash = (hash ^ word) * 0xBF58476D1CE4E5B9ull;
            hash ^= hash >> 29;
        }
        return static_cast<std::size_t>(hash);
    }
};

// How a folded block names what its cells refer to: a pen by its index in
// the block's own table, links and clusters by the host's ids, which a cell
// carries and so pass through unchanged.
class BlockNames {
public:
    explicit BlockNames(std::vector<Pen>& pens)
        : m_pens(pens)
    {
        m_ids.reserve(64);
    }

    std::uint64_t pen(const Pen& pen)
    {
        const PenKey key { std::bit_cast<std::array<std::uint8_t, sizeof(Pen)>>(pen) };
        const auto [found, isNew] = m_ids.try_emplace(key, static_cast<std::uint32_t>(m_pens.size()));
        if (isNew)
            m_pens.push_back(pen);
        return found->second;
    }

    std::uint64_t link(std::uint16_t id) { return id; }

    std::optional<std::uint64_t> cluster(const Cell& lead) { return lead.clusterID(); }

private:
    std::vector<Pen>& m_pens;
    std::unordered_map<PenKey, std::uint32_t, PenKeyHash> m_ids;
};

// Every cluster id a folded form carries came from a cell the store held.
constexpr auto everyClusterResolves = [](std::uint64_t) { return true; };

bool addBytes(std::size_t& total, std::size_t bytes)
{
    if (bytes > std::numeric_limits<std::size_t>::max() - total)
        return false;
    total += bytes;
    return true;
}

bool addArrayBytes(std::size_t& total, std::size_t count, std::size_t elementBytes)
{
    return count <= std::numeric_limits<std::size_t>::max() / elementBytes && addBytes(total, count * elementBytes);
}

} // namespace

void LineStore::BlockDirectory::SlotDeleter::operator()(Block* slots) const
{
    std::destroy_n(slots, capacity);
    ::operator delete(slots);
}

LineStore::Block& LineStore::BlockDirectory::operator[](std::size_t index)
{
    assert(index < m_count);
    const auto untilEnd = capacity() - m_start;
    return m_slots.get()[index < untilEnd ? m_start + index : index - untilEnd];
}

const LineStore::Block& LineStore::BlockDirectory::operator[](std::size_t index) const
{
    assert(index < m_count);
    const auto untilEnd = capacity() - m_start;
    return m_slots.get()[index < untilEnd ? m_start + index : index - untilEnd];
}

bool LineStore::BlockDirectory::reserve(std::size_t slots)
{
    if (slots <= capacity())
        return true;
    if (slots > std::numeric_limits<std::size_t>::max() / sizeof(Block))
        return false;
    static_assert(alignof(Block) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    static_assert(std::is_nothrow_default_constructible_v<Block> && std::is_nothrow_move_assignable_v<Block>);
    auto* storage = static_cast<Block*>(::operator new(slots * sizeof(Block), std::nothrow));
    if (!storage)
        return false;
    for (std::size_t index = 0; index < slots; ++index)
        std::construct_at(storage + index);
    std::unique_ptr<Block, SlotDeleter> replacement(storage, SlotDeleter { slots });
    for (std::size_t index = 0; index < m_count; ++index)
        storage[index] = std::move((*this)[index]);
    m_slots = std::move(replacement);
    m_start = 0;
    return true;
}

void LineStore::BlockDirectory::pushBack(Block&& block)
{
    assert(m_count < capacity());
    ++m_count;
    back() = std::move(block);
}

void LineStore::BlockDirectory::pushFront(Block&& block)
{
    assert(m_count < capacity());
    m_start = m_start ? m_start - 1 : capacity() - 1;
    ++m_count;
    front() = std::move(block);
}

void LineStore::BlockDirectory::popFront()
{
    assert(m_count);
    Block* slot = &front();
    std::destroy_at(slot);
    std::construct_at(slot);
    --m_count;
    m_start = m_start + 1 == capacity() ? 0 : m_start + 1;
    if (!m_count)
        m_start = 0;
}

void LineStore::BlockDirectory::popBack()
{
    assert(m_count);
    Block* slot = &back();
    std::destroy_at(slot);
    std::construct_at(slot);
    --m_count;
    if (!m_count)
        m_start = 0;
}

void LineStore::BlockDirectory::clear()
{
    while (m_count)
        popBack();
}

void LineStore::BlockDirectory::reset()
{
    m_slots = std::unique_ptr<Block, SlotDeleter>(nullptr, SlotDeleter {});
    m_start = 0;
    m_count = 0;
}

void LineStore::BlockDirectory::swap(BlockDirectory& other) noexcept
{
    m_slots.swap(other.m_slots);
    std::swap(m_start, other.m_start);
    std::swap(m_count, other.m_count);
}

LineStore::RestoreLayout::RestoreLayout(int capacity, const Continuation& continuation)
    : m_capacity(capacity)
    , m_continuation(continuation)
{
    if (capacity < 1 || continuation.totalAppended < 0 || continuation.head < continuation.front.number
        || continuation.maxHead < continuation.head || (continuation.front.continuity != LineContinuity::StartsLine && continuation.front.continuity != LineContinuity::ContinuesLine))
        m_error = StorageError::Malformed;
}

std::expected<void, LineStore::StorageError> LineStore::RestoreLayout::noteRow(int length, bool wrapped)
{
    if (m_error)
        return std::unexpected(*m_error);
    if (length < 0 || length > maxColumns)
        m_error = StorageError::Malformed;
    else if (m_rows == static_cast<std::size_t>(m_capacity) || !addBytes(m_cells, static_cast<std::size_t>(length)))
        m_error = StorageError::OverCapacity;
    if (m_error)
        return std::unexpected(*m_error);
    if (length) {
        if (!m_blocks || length > blockCells - m_used) {
            ++m_blocks;
            m_used = 0;
        }
        m_used += length;
    }
    ++m_rows;
    m_completed += !wrapped;
    return {};
}

std::expected<LineStore::RestorationPlan, LineStore::StorageError> LineStore::RestoreLayout::plan() const
{
    if (m_error)
        return std::unexpected(*m_error);
    if (static_cast<std::uint64_t>(m_continuation.totalAppended) < m_rows || m_completed != m_continuation.head - m_continuation.front.number)
        return std::unexpected(StorageError::Malformed);
    RestorationPlan result { .rows = m_rows, .cells = m_cells, .blocks = m_blocks, .retainedBytes = sizeof(LineStore) };
    if (!addArrayBytes(result.descriptorBytes, static_cast<std::size_t>(m_capacity), sizeof(StoredLine))
        || !addArrayBytes(result.indexBytes, static_cast<std::size_t>(m_capacity) / lineIndexStride + 2, sizeof(std::uint64_t))
        || !addArrayBytes(result.blockDirectoryBytes, m_blocks, sizeof(Block))
        || !addArrayBytes(result.slabBytes, m_blocks, static_cast<std::size_t>(blockCells) * sizeof(Cell))
        || !addBytes(result.retainedBytes, result.descriptorBytes) || !addBytes(result.retainedBytes, result.indexBytes)
        || !addBytes(result.retainedBytes, result.blockDirectoryBytes) || !addBytes(result.retainedBytes, result.slabBytes))
        return std::unexpected(StorageError::OverCapacity);
    return result;
}

std::expected<LineStore::RestorationPlan, LineStore::StorageError> LineStore::restorationPlan(int capacity, std::span<const StoredLine> rows, const Continuation& continuation)
{
    RestoreLayout layout(capacity, continuation);
    for (const auto& row : rows) {
        if (row.length && !row.cells)
            return std::unexpected(StorageError::Malformed);
        if (auto admitted = layout.noteRow(row.length, row.wrapped); !admitted)
            return std::unexpected(admitted.error());
    }
    return layout.plan();
}

void LineStore::init(int capacity)
{
    dispose();
    if (!m_gate)
        m_gate = std::make_shared<ReaderGate>();
    if (capacity < 1)
        capacity = 1;
    m_capacity = capacity;
    m_ring.reset(new StoredLine[static_cast<std::size_t>(capacity)]);
    for (int i = 0; i < capacity; ++i)
        m_ring[i] = StoredLine { nullptr, 0, false, 0, 0, false, false };
    m_start = 0;
    m_count = 0;
    m_frontEvicted = 0;
    m_lineIndexCount = static_cast<std::size_t>(capacity) / lineIndexStride + 2;
    m_lineIndex.reset(new std::uint64_t[m_lineIndexCount] {});
    m_nextOrdinal = 0;
    m_frontLine = m_headLine;
}

Cell* LineStore::takeSlab()
{
    // Every slab is exactly blockCells, so a parked retired slab is a
    // drop-in replacement for a fresh allocation.
    if (m_retiredSlab) {
        Cell* slab = m_retiredSlab;
        m_retiredSlab = nullptr;
        return slab;
    }
    return new Cell[static_cast<std::size_t>(blockCells)];
}

void LineStore::parkSlab(Cell* slab)
{
    // Park one drained slab for the next append instead of paying a
    // delete/new pair per block cycle at steady state.
    if (!slab)
        return;
    if (!m_retiredSlab)
        m_retiredSlab = slab;
    else
        delete[] slab;
}

void LineStore::releaseRetiredSlab()
{
    delete[] m_retiredSlab;
    m_retiredSlab = nullptr;
    delete[] m_retiredText;
    m_retiredText = nullptr;
}

void LineStore::parkTextSlab(std::uint8_t* slab)
{
    if (!slab)
        return;
    if (!m_retiredText)
        m_retiredText = slab;
    else
        delete[] slab;
}

void LineStore::expandCompactRow(const std::uint8_t* bytes, Cell* cells, int length)
{
    Cell cell = defaultBlankCell;
    for (int column = 0; column < length; ++column) {
        cell.codePoint = bytes[column];
        cells[column] = cell;
    }
}

void LineStore::expandLine(StoredLine& line, const Block& block) const
{
    // Under the model lock, as every read is. A reader on another thread
    // whose snapshot saw the row compact reads its bytes, which stay as they
    // are; it never reads the cells written here.
    assert(block.base && block.text && line.cells >= block.base.get());
    expandCompactRow(block.text.get() + (line.cells - block.base.get()), line.cells, line.length);
    line.isCompact = 0;
}

void LineStore::releaseBlock(Block& b)
{
    parkSlab(b.base.release());
    parkTextSlab(b.text.release());
    b.folded.reset();
    b.capacity = 0;
    dropDecodedBlock(b.sequence);
}

void LineStore::retireFrontBlock(Block& b)
{
    // Store, then load, both sequentially consistent; a reader publishes its
    // hazard and then loads this. One of the two sees the other.
    if (m_gate) {
        m_gate->oldestLiveSequence.store(b.sequence + 1, std::memory_order_seq_cst);
        if (m_gate->hazardSequence.load(std::memory_order_seq_cst) == b.sequence) [[unlikely]] {
            // The reader reading this block frees it when its read ends; the
            // next block takes a fresh slab rather than this one.
            delete[] m_gate->deferredSlab.exchange(b.base.release(), std::memory_order_acq_rel);
            delete[] m_gate->deferredText.exchange(b.text.release(), std::memory_order_acq_rel);
            delete m_gate->deferredForms.exchange(b.folded.release(), std::memory_order_acq_rel);
            b.capacity = 0;
            dropDecodedBlock(b.sequence);
            return;
        }
    }
    releaseBlock(b);
}

void LineStore::quiesceReaders()
{
    if (!m_gate)
        return;
    m_gate->invalidation.fetch_add(1, std::memory_order_seq_cst);
    // Readers register under the model lock, which the caller holds, so none
    // can register between this load and the mutation.
    if (m_gate->readerCount.load(std::memory_order_seq_cst) == 0 || !m_gate->lock)
        return;
    m_gate->lock->lock();
    m_gate->lock->unlock();
}

void LineStore::renumberBlockSequences()
{
    for (DecodedBlock& decoded : m_decoded)
        forgetDecodedBlock(decoded);
    for (std::size_t index = 0; index < m_blocks.size(); ++index)
        m_blocks[index].sequence = m_nextBlockSequence++;
    if (m_gate)
        m_gate->oldestLiveSequence.store(m_blocks.empty() ? m_nextBlockSequence : m_blocks.front().sequence, std::memory_order_seq_cst);
}

void LineStore::dispose()
{
    quiesceReaders();
    m_blocks.reset();
    releaseRetiredSlab();
    for (DecodedBlock& decoded : m_decoded) {
        delete[] decoded.cells;
        decoded = {};
    }
    m_ring.reset();
    m_lineIndex.reset();
    m_lineIndexCount = 0;
    m_capacity = 0;
    m_start = 0;
    m_count = 0;
    m_frontEvicted = 0;
}

void LineStore::evictOldest()
{
    // Drop the oldest descriptor; account it against the front block. When
    // every line a front block backs has been evicted, free the whole block.
    // The front row sits at m_start, which is already below the capacity.
    const bool hadCells = m_ring && m_ring[m_start].length > 0;
    // The front row that leaves either completes its line, so the next row
    // starts one, or continues into the next row, which then starts nothing.
    if (m_ring && !m_ring[m_start].wrapped) {
        ++m_frontLine;
        m_frontContinuity = LineContinuity::StartsLine;
    } else {
        m_frontContinuity = LineContinuity::ContinuesLine;
    }
    m_start = ringIndex(m_start + 1);
    --m_count;
    if (hadCells) {
        ++m_frontEvicted;
        while (!m_blocks.empty() && m_frontEvicted >= m_blocks.front().linesAppended) {
            m_frontEvicted -= m_blocks.front().linesAppended;
            retireFrontBlock(m_blocks.front());
            m_blocks.popFront();
        }
    }
}

// The steady state of a flood: a row with cells that fits the back block's
// slack, at a capacity where the eviction it pays leaves the front block still
// backing live lines. Nothing here can allocate, release a block or refuse, so
// the general path's preparation has nothing to secure. Every outcome is the
// general path's: the same eviction, slot and numbering.
template<typename Store>
inline bool LineStore::appendWithinBackBlock(int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping, const Store& store, bool storesBytes) // bool-ok: wrap and shaping are stored row metadata, as append takes them.
{
    if (length <= 0 || length > maxColumns || !m_ring || m_blocks.empty() || m_totalAppended == std::numeric_limits<long long>::max()
        || (!wrapped && m_headLine == std::numeric_limits<std::uint64_t>::max()))
        return false;
    Block& back = m_blocks.back();
    if (length > back.capacity - back.used)
        return false;
    // A block's first compact row brings its text slab, an allocation the
    // general path secures.
    if (storesBytes && !back.text)
        return false;
    if (m_count == m_capacity) {
        const StoredLine& front = m_ring[m_start];
        const bool hadCells = front.length > 0;
        if (hadCells && m_frontEvicted + 1 >= m_blocks.front().linesAppended)
            return false;
        // evictOldest without the block release this case excludes.
        if (!front.wrapped) {
            ++m_frontLine;
            m_frontContinuity = LineContinuity::StartsLine;
        } else {
            m_frontContinuity = LineContinuity::ContinuesLine;
        }
        m_frontEvicted += hadCells ? 1 : 0;
        m_start = ringIndex(m_start + 1);
        --m_count;
    }
    Cell* slot = back.base.get() + back.used;
    const bool isCompact = store(back, slot);
    back.used += length;
    ++back.linesAppended;
    m_ring[ringIndex(m_start + m_count)] = StoredLine { slot, length, wrapped, lineAttribute, mark, needsShaping, isCompact };
    ++m_count;
    ++m_totalAppended;
    noteStoredRow(wrapped);
    return true;
}

template<typename Store>
void LineStore::appendStoredRow(int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping, const Store& store, bool storesBytes) // bool-ok: wrap and shaping are stored row metadata.
{
    if (appendWithinBackBlock(length, lineAttribute, mark, wrapped, needsShaping, store, storesBytes)) [[likely]]
        return;
    if (!m_ring || length < 0 || m_totalAppended == std::numeric_limits<long long>::max()
        || (!wrapped && m_headLine == std::numeric_limits<std::uint64_t>::max()))
        return;

    // Secure every fallible owner before eviction. A front raw slab that this
    // exact eviction drains will become the parked slab, so steady-state append
    // still recycles it without an unnecessary allocation.
    const bool releasesFront = m_count == m_capacity && m_ring[m_start].length > 0
        && m_frontEvicted + 1 >= m_blocks.front().linesAppended;
    const bool keepsBack = !m_blocks.empty() && !(releasesFront && m_blocks.size() == 1);
    const bool needsBlock = length > 0 && (!keepsBack || length > m_blocks.back().capacity - m_blocks.back().used);
    // A compact row needs the back block's text slab, which a block gets on
    // its first compact row: secured here like the cell slab.
    const bool needsText = storesBytes && length > 0 && (needsBlock || !m_blocks.back().text);
    std::unique_ptr<Cell[]> readySlab;
    std::unique_ptr<std::uint8_t[]> readyText;
    // A reader may hold a hazard on the front block, and then its slabs are
    // the reader's to free rather than this append's to reuse: they are
    // secured here, before eviction, while a reader is registered.
    const bool hasReader = m_gate && m_gate->readerCount.load(std::memory_order_acquire) > 0;
    if (needsText) {
        const bool isReleasedTextReusable = releasesFront && m_blocks.front().text && !hasReader;
        if (!m_retiredText && !isReleasedTextReusable) {
            readyText.reset(new (std::nothrow) std::uint8_t[static_cast<std::size_t>(blockCells)]);
            if (!readyText)
                return;
        }
    }
    if (needsBlock) {
        const bool reusesReleasedSlab = releasesFront && m_blocks.front().base && !hasReader;
        if (!m_retiredSlab && !reusesReleasedSlab) {
            readySlab.reset(new (std::nothrow) Cell[static_cast<std::size_t>(blockCells)]);
            if (!readySlab)
                return;
        }
        const auto required = m_blocks.size() - static_cast<std::size_t>(releasesFront) + 1;
        if (required > m_blocks.capacity()) {
            const auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(Block);
            if (required > maximum)
                return;
            const auto grown = m_blocks.capacity() <= maximum / 2 ? 2 * m_blocks.capacity() : maximum;
            if (!m_blocks.reserve(std::max(required, grown)))
                return;
        }
    }
    if (m_count == m_capacity)
        evictOldest();

    Cell* slot = nullptr;
    bool isCompact = false;
    if (length > 0) {
        if (needsBlock) {
            Block block;
            if (!readySlab) {
                assert(m_retiredSlab);
                readySlab.reset(std::exchange(m_retiredSlab, nullptr));
            }
            block.base = std::move(readySlab);
            block.capacity = blockCells;
            block.firstOrdinal = m_nextOrdinal;
            block.sequence = m_nextBlockSequence++;
            m_blocks.pushBack(std::move(block));
        }
        Block& b = m_blocks.back();
        if (needsText && !b.text) {
            if (!readyText) {
                assert(m_retiredText);
                readyText.reset(std::exchange(m_retiredText, nullptr));
            }
            b.text = std::move(readyText);
        }
        slot = b.base.get() + b.used;
        isCompact = store(b, slot);
        b.used += length;
        ++b.linesAppended;
    }

    const int index = ringIndex(m_start + m_count);
    m_ring[index] = StoredLine { slot, length, wrapped, lineAttribute, mark, needsShaping, isCompact };
    ++m_count;
    ++m_totalAppended; // monotonic; survives eviction + clear (see totalAppended)
    noteStoredRow(wrapped);
}

void LineStore::append(const Cell* cells, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping)
{
    if (!cells || length < 0)
        return;
    length = std::min(length, maxColumns);
    appendStoredRow(length, lineAttribute, mark, wrapped, needsShaping, [cells, length](Block&, Cell* destination) {
        copyCellsIntoSlab(destination, cells, length);
        return false; }, false);
}

void LineStore::appendDefaultASCII(std::span<const std::uint8_t> text, int length, uint8_t lineAttribute, uint8_t mark, bool wrapped, bool needsShaping) // bool-ok: wrap and shaping are stored row metadata.
{
    if (length < 0 || length > maxColumns || text.size() > static_cast<std::size_t>(length))
        return;
    appendStoredRow(length, lineAttribute, mark, wrapped, needsShaping, [text, length](Block& block, Cell* destination) {
        // The bytes land in the block's text slab at the cells' offset and
        // the cells stay unwritten: a flood evicts most rows unread, and a
        // 32-byte cell per byte was most of the append. A read expands the
        // row in place (expandLine). The slab was secured before this ran;
        // a block without one takes the row expanded, as every row was.
        if (block.text) [[likely]] {
            std::uint8_t* bytes = block.text.get() + (destination - block.base.get());
            std::memcpy(bytes, text.data(), text.size());
            std::memset(bytes + text.size(), 0, static_cast<std::size_t>(length) - text.size());
            return true;
        }
        Cell cell = defaultBlankCell;
        for (int column = 0; column < length; ++column) {
            cell.codePoint = static_cast<std::size_t>(column) < text.size() ? text[static_cast<std::size_t>(column)] : 0;
            destination[column] = cell;
        }
        return false; }, true);
}

// The newest row takes ordinal m_nextOrdinal and belongs to the open line,
// whose number is the head; an unwrapped row closes it.
void LineStore::noteStoredRow(bool isWrapped) // bool-ok: wrap and shaping are stored row metadata, as append takes them.
{
    const std::uint64_t ordinal = m_nextOrdinal++;
    if (ordinal % lineIndexStride == 0 && m_lineIndexCount)
        m_lineIndex[(ordinal / lineIndexStride) % m_lineIndexCount] = m_headLine;
    if (!isWrapped) {
        ++m_headLine;
        if (m_headLine > m_maxHeadLine)
            m_maxHeadLine = m_headLine;
    }
}

std::uint64_t LineStore::frontLineNumber() const
{
    if (m_count == 0 || m_frontLine >= m_headLine)
        return m_headLine;
    // A front row continuing a line whose start was evicted makes that line
    // unaddressable; a store holding only that line's tail holds no line.
    const std::uint64_t oldest = m_frontContinuity == LineContinuity::StartsLine ? m_frontLine : m_frontLine + 1;
    return oldest < m_headLine ? oldest : m_headLine;
}

std::optional<LineStore::LogicalLineRows> LineStore::openLine() const
{
    if (m_count == 0 || !descriptor(m_count - 1).wrapped)
        return std::nullopt;
    int first = m_count - 1;
    while (first > 0 && descriptor(first - 1).wrapped)
        --first;
    // Rows that reach the front continue a line only the front row can say
    // began there.
    if (first == 0 && m_frontContinuity == LineContinuity::ContinuesLine)
        return std::nullopt;
    return LogicalLineRows { first, m_count - first };
}

std::optional<LineStore::LogicalLineRows> LineStore::logicalLine(std::uint64_t number) const
{
    if (m_count == 0 || !m_lineIndexCount || number < frontLineNumber() || number >= m_headLine)
        return std::nullopt;

    // The last index entry naming a line before `number`: entries are in row
    // order and their numbers never decrease, so a binary search finds it.
    const std::uint64_t front = frontOrdinal();
    const std::uint64_t firstEntry = (front + lineIndexStride - 1) / lineIndexStride;
    const std::uint64_t endEntry = (m_nextOrdinal + lineIndexStride - 1) / lineIndexStride;
    std::uint64_t low = firstEntry;
    std::uint64_t high = endEntry;
    while (low < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        if (m_lineIndex[middle % m_lineIndexCount] < number)
            low = middle + 1;
        else
            high = middle;
    }
    int row = 0;
    std::uint64_t line = m_frontLine;
    if (low > firstEntry) {
        const std::uint64_t entry = low - 1;
        row = static_cast<int>(entry * lineIndexStride - front);
        line = m_lineIndex[entry % m_lineIndexCount];
    }
    // At most one stride of rows separates that entry from the line's start.
    while (line < number && row < m_count) {
        if (!descriptor(row).wrapped)
            ++line;
        ++row;
    }
    if (line != number || row >= m_count)
        return std::nullopt;
    int rowCount = 1;
    while (row + rowCount - 1 < m_count && descriptor(row + rowCount - 1).wrapped)
        ++rowCount;
    if (row + rowCount > m_count)
        return std::nullopt;
    return LogicalLineRows { row, rowCount };
}

std::optional<LineStore::LogicalPosition> LineStore::positionAt(int index) const
{
    if (index < 0 || index >= m_count || !m_lineIndexCount)
        return std::nullopt;
    const std::uint64_t front = frontOrdinal();
    const std::uint64_t entryOrdinal = (front + static_cast<std::uint64_t>(index)) / lineIndexStride * lineIndexStride;
    int row = 0;
    std::uint64_t number = m_frontLine;
    if (entryOrdinal >= front) {
        row = static_cast<int>(entryOrdinal - front);
        number = m_lineIndex[(entryOrdinal / lineIndexStride) % m_lineIndexCount];
    }
    for (; row < index; ++row) {
        if (!descriptor(row).wrapped)
            ++number;
    }
    int first = index;
    while (first > 0 && descriptor(first - 1).wrapped)
        --first;
    if (!first && m_frontContinuity == LineContinuity::ContinuesLine)
        return std::nullopt;
    std::uint64_t cell = 0;
    for (row = first; row < index; ++row)
        cell += static_cast<std::uint64_t>(Screen::writtenLength(*at(row)));
    return LogicalPosition { number, cell };
}

std::optional<int> LineStore::rowForPosition(LogicalPosition position) const
{
    const auto rows = position.line == m_headLine ? openLine() : logicalLine(position.line);
    if (!rows)
        return std::nullopt;
    std::uint64_t cell = 0;
    for (int row = rows->firstIndex; row < rows->firstIndex + rows->rowCount - 1; ++row) {
        cell += static_cast<std::uint64_t>(Screen::writtenLength(*at(row)));
        if (position.cell < cell)
            return row;
    }
    return rows->firstIndex + rows->rowCount - 1;
}

bool LineStore::prepend(std::span<const StoredLine> rows, FrontLine front, int newestToDrop)
{
    if (newestToDrop < 0 || newestToDrop > m_count)
        return false;
    if (rows.empty()) {
        dropNewest(newestToDrop);
        return true;
    }
    if (!m_ring || std::cmp_greater(rows.size(), m_capacity - (m_count - newestToDrop)))
        return false;
    std::size_t blockCount = 0;
    int used = blockCells;
    std::uint64_t closedLines = m_headLine - m_frontLine;
    for (int index = m_count - newestToDrop; index < m_count; ++index)
        closedLines -= !descriptor(index).wrapped;
    for (const auto& row : rows) {
        if (row.length < 0 || row.length > maxColumns || (row.length && !row.cells))
            return false;
        if (!row.wrapped) {
            if (closedLines == std::numeric_limits<std::uint64_t>::max())
                return false;
            ++closedLines;
        }
        if (!row.length)
            continue;
        if (row.length > blockCells - used) {
            ++blockCount;
            used = 0;
        }
        used += row.length;
    }
    if (front.number > std::numeric_limits<std::uint64_t>::max() - closedLines
        || blockCount > std::numeric_limits<std::uint32_t>::max() - m_nextBlockSequence)
        return false;
    std::unique_ptr<Block[]> blocks(blockCount ? new (std::nothrow) Block[blockCount] : nullptr);
    std::unique_ptr<StoredLine[]> prepared(new (std::nothrow) StoredLine[rows.size()]);
    if ((blockCount && !blocks) || !prepared)
        return false;
    std::size_t nextBlock = 0;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        prepared[index] = row;
        prepared[index].cells = nullptr;
        prepared[index].isCompact = 0;
        if (!row.length)
            continue;
        if (!nextBlock || row.length > blocks[nextBlock - 1].capacity - blocks[nextBlock - 1].used) {
            auto& block = blocks[nextBlock];
            block.base.reset(new (std::nothrow) Cell[static_cast<std::size_t>(blockCells)]);
            if (!block.base)
                return false;
            block.capacity = blockCells;
            block.firstOrdinal = index;
            block.sequence = m_nextBlockSequence + static_cast<std::uint32_t>(nextBlock);
            ++nextBlock;
        }
        auto& block = blocks[nextBlock - 1];
        Cell* destination = block.base.get() + block.used;
        copyCellsIntoSlab(destination, row.cells, row.length);
        prepared[index].cells = destination;
        block.used += row.length;
        ++block.linesAppended;
    }
    if (!m_blocks.reserve(m_blocks.size() + blockCount))
        return false;

    quiesceReaders();
    dropNewest(newestToDrop);
    const std::uint64_t oldFront = frontOrdinal();
    if (!m_blocks.empty()) {
        auto& first = m_blocks.front();
        if (first.firstOrdinal < oldFront) {
            if (first.isFolded) {
                FoldedForms& folded = *first.folded;
                const auto skipped = static_cast<std::size_t>(oldFront - first.firstOrdinal);
                assert(skipped <= folded.entries.size());
                const auto bytes = skipped < folded.entries.size() ? folded.entries[skipped].offset : folded.forms.size();
                folded.forms.erase(folded.forms.begin(), folded.forms.begin() + static_cast<std::ptrdiff_t>(bytes));
                folded.entries.erase(folded.entries.begin(), folded.entries.begin() + static_cast<std::ptrdiff_t>(skipped));
                for (auto& entry : folded.entries)
                    entry.offset -= static_cast<std::uint32_t>(bytes);
                dropDecodedBlock(first.sequence);
            }
            first.firstOrdinal = oldFront;
        }
        first.linesAppended -= m_frontEvicted;
        m_frontEvicted = 0;
    }
    for (std::size_t index = 0; index < m_blocks.size(); ++index)
        m_blocks[index].firstOrdinal = m_blocks[index].firstOrdinal - oldFront + rows.size();
    const int added = static_cast<int>(rows.size());
    m_start -= added;
    if (m_start < 0)
        m_start += m_capacity;
    for (int index = 0; index < added; ++index)
        m_ring[ringIndex(m_start + index)] = prepared[static_cast<std::size_t>(index)];
    m_count += added;
    m_nextOrdinal = static_cast<std::uint64_t>(m_count);
    m_nextBlockSequence += static_cast<std::uint32_t>(blockCount);
    while (blockCount)
        m_blocks.pushFront(std::move(blocks[--blockCount]));
    // The prepended blocks stand in front of older sequences; the gate reads
    // release order from sequences, so they are renumbered in place.
    renumberBlockSequences();
    restartNumbering(front.number, front.continuity);
    return true;
}

void LineStore::restartNumbering(std::uint64_t frontLineNumber, LineContinuity front)
{
    m_frontLine = frontLineNumber;
    m_frontContinuity = front;
    std::uint64_t line = frontLineNumber;
    const std::uint64_t firstOrdinal = frontOrdinal();
    for (int row = 0; row < m_count; ++row) {
        const std::uint64_t ordinal = firstOrdinal + static_cast<std::uint64_t>(row);
        if (ordinal % lineIndexStride == 0 && m_lineIndexCount)
            m_lineIndex[(ordinal / lineIndexStride) % m_lineIndexCount] = line;
        if (!descriptor(row).wrapped)
            ++line;
    }
    m_headLine = line;
    if (m_headLine > m_maxHeadLine)
        m_maxHeadLine = m_headLine;
}

bool LineStore::restore(std::span<const StoredLine> rows, const Continuation& continuation)
{
    if (!m_ring || m_count)
        return false;
    auto replacement = tryRestore(m_capacity, rows, continuation);
    if (!replacement)
        return false;
    quiesceReaders();
    swap(**replacement);
    return true;
}

std::expected<std::unique_ptr<LineStore>, LineStore::StorageError> LineStore::tryRestore(int capacity, std::span<const StoredLine> rows,
    const Continuation& continuation, std::size_t maxOwnedBytes)
{
    const auto plan = restorationPlan(capacity, rows, continuation);
    if (!plan)
        return std::unexpected(plan.error());
    if (plan->retainedBytes > maxOwnedBytes)
        return std::unexpected(StorageError::AllocationRefused);
    std::unique_ptr<LineStore> store(new (std::nothrow) LineStore);
    if (!store)
        return std::unexpected(StorageError::AllocationRefused);
    store->m_ring.reset(new (std::nothrow) StoredLine[static_cast<std::size_t>(capacity)] {});
    if (!store->m_ring)
        return std::unexpected(StorageError::AllocationRefused);
    store->m_lineIndexCount = static_cast<std::size_t>(capacity) / lineIndexStride + 2;
    store->m_lineIndex.reset(new (std::nothrow) std::uint64_t[store->m_lineIndexCount] {});
    if (!store->m_lineIndex || !store->m_blocks.reserve(plan->blocks))
        return std::unexpected(StorageError::AllocationRefused);
    store->m_gate = std::make_shared<ReaderGate>();
    store->m_capacity = capacity;
    store->m_frontLine = continuation.front.number;
    store->m_headLine = continuation.front.number;
    store->m_maxHeadLine = continuation.maxHead;
    store->m_frontContinuity = continuation.front.continuity;
    store->m_totalAppended = continuation.totalAppended - static_cast<long long>(rows.size());
    const Cell blank {};
    for (const auto& row : rows) {
        const auto count = store->m_count;
        store->append(row.cells ? row.cells : &blank, row.length, row.lineAttribute, row.mark, row.wrapped, row.needsShaping);
        if (store->m_count != count + 1)
            return std::unexpected(StorageError::AllocationRefused);
    }
    return store;
}

void LineStore::swap(LineStore& other) noexcept
{
    m_blocks.swap(other.m_blocks);
    std::swap(m_retiredSlab, other.m_retiredSlab);
    std::swap(m_retiredText, other.m_retiredText);
    m_ring.swap(other.m_ring);
    std::swap(m_capacity, other.m_capacity);
    std::swap(m_start, other.m_start);
    std::swap(m_count, other.m_count);
    std::swap(m_frontEvicted, other.m_frontEvicted);
    std::swap(m_totalAppended, other.m_totalAppended);
    std::swap(m_nextBlockSequence, other.m_nextBlockSequence);
    m_lineIndex.swap(other.m_lineIndex);
    std::swap(m_lineIndexCount, other.m_lineIndexCount);
    std::swap(m_nextOrdinal, other.m_nextOrdinal);
    std::swap(m_frontLine, other.m_frontLine);
    std::swap(m_headLine, other.m_headLine);
    std::swap(m_maxHeadLine, other.m_maxHeadLine);
    std::swap(m_frontContinuity, other.m_frontContinuity);
    std::swap(m_decoded, other.m_decoded);
    std::swap(m_decodeClock, other.m_decodeClock);
    m_gate.swap(other.m_gate);
}

StoredLine* LineStore::resolved(int index) const
{
    if (!m_ring || index < 0 || index >= m_count)
        return nullptr;
    StoredLine& line = descriptor(index);
    // A line with cells and no pointer to them is in a folded block whose
    // decode is not held; a raw line always points into its slab.
    if (!line.cells && line.length > 0) [[unlikely]]
        decodeLine(index);
    else if (line.isCompact) [[unlikely]]
        expandLine(line, m_blocks[blockIndexHolding(frontOrdinal() + static_cast<std::uint64_t>(index))]);
    return &line;
}

const StoredLine* LineStore::at(int index) const { return resolved(index); }

StoredLine* LineStore::at(int index) { return resolved(index); }

StoredLine* LineStore::writableAt(int index)
{
    if (!m_ring || index < 0 || index >= m_count)
        return nullptr;
    StoredLine& line = descriptor(index);
    if (line.length > 0) {
        // The caller writes the cells after this returns, so a reader of the
        // block must be done with it and must not start on its snapshot again.
        quiesceReaders();
        const std::size_t block = blockIndexHolding(frontOrdinal() + static_cast<std::uint64_t>(index));
        if (m_blocks[block].isFolded)
            unfoldBlock(block);
        else if (line.isCompact)
            expandLine(line, m_blocks[block]);
    }
    return &line;
}

std::uint64_t LineStore::blockEnd(std::size_t index) const
{
    return index + 1 < m_blocks.size() ? m_blocks[index + 1].firstOrdinal : m_nextOrdinal;
}

std::size_t LineStore::blockIndexHolding(std::uint64_t ordinal) const
{
    // Spans are contiguous and ascending, so the first block whose span ends
    // past the ordinal is the one holding it.
    std::size_t low = 0;
    std::size_t high = m_blocks.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (blockEnd(middle) <= ordinal)
            low = middle + 1;
        else
            high = middle;
    }
    assert(low < m_blocks.size());
    return low;
}

template<typename Visit>
bool LineStore::decodeBlock(std::size_t index, Cell* scratch, Visit&& visit) const
{
    const Block& b = m_blocks[index];
    const std::uint64_t front = frontOrdinal();
    const std::uint64_t begin = std::max(b.firstOrdinal, front);
    const std::uint64_t end = blockEnd(index);
    int cursor = 0;
    for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal) {
        StoredLine& line = descriptor(static_cast<int>(ordinal - front));
        if (line.length == 0)
            continue;
        if (line.length > blockCells - cursor)
            return false;
        // Every line with cells was in the span at the fold, so it has an entry.
        const FoldedForms& folded = *b.folded;
        const std::size_t slot = static_cast<std::size_t>(ordinal - b.firstOrdinal);
        const std::size_t formBegin = folded.entries[slot].offset;
        const std::size_t formEnd = slot + 1 < folded.entries.size() ? folded.entries[slot + 1].offset : folded.forms.size();
        Cell* cells = scratch + cursor;
        if (!decodeRowForm(std::span<const std::uint8_t>(folded.forms).subspan(formBegin, formEnd - formBegin), folded.pens, std::span<Cell>(cells, static_cast<std::size_t>(line.length))))
            return false;
        visit(line, cells);
        cursor += line.length;
    }
    return true;
}

void LineStore::foldBlock(std::size_t index)
{
    Block& b = m_blocks[index];
    const std::uint64_t front = frontOrdinal();
    const std::uint64_t end = blockEnd(index);
    const auto refuse = [&b] {
        b.isFoldRefused = true;
        b.folded.reset();
    };
    // Only the front block spans evicted lines, and no reader reaches them
    // again: its span starts at the front row from here on, as a prepend
    // starts it, so the entry table names no line that is gone.
    if (b.firstOrdinal < front)
        b.firstOrdinal = front;
    // Only a long run of blank rows the store keeps stretches a span this far.
    // Marked unlikely, the exit leaves the encoder inlined below laid out for
    // the fold that goes ahead; unmarked, a fold ran about 1% more instructions.
    if (end - b.firstOrdinal > maxFoldSpan) [[unlikely]] {
        refuse();
        return;
    }
    // A compact row's cells are written now: the form is made from cells,
    // and the check below compares them.
    for (std::uint64_t ordinal = b.firstOrdinal; ordinal < end; ++ordinal) {
        StoredLine& line = descriptor(static_cast<int>(ordinal - front));
        if (line.isCompact)
            expandLine(line, b);
    }
    b.folded = std::make_unique<FoldedForms>();
    FoldedForms& folded = *b.folded;
    // Text is about a byte a cell and the lists a few bytes a line.
    folded.forms.reserve(static_cast<std::size_t>(b.used) + static_cast<std::size_t>(end - b.firstOrdinal) * 8);
    folded.entries.reserve(static_cast<std::size_t>(end - b.firstOrdinal));
    BlockNames names(folded.pens);
    WireWriter writer(folded.forms);
    for (std::uint64_t ordinal = b.firstOrdinal; ordinal < end; ++ordinal) {
        FormEntry entry;
        entry.offset = static_cast<std::uint32_t>(folded.forms.size());
        // A blank line has no cells: it writes no form, and the entry table
        // still names a slot for it.
        const StoredLine& line = descriptor(static_cast<int>(ordinal - front));
        entry.facts = RowFacts { .length = static_cast<std::uint16_t>(line.length), .isWrapped = line.wrapped, .needsShaping = line.needsShaping, .lineAttribute = line.lineAttribute };
        if (line.length > 0
            && (!Screen::writeRowForm(std::span<const Cell>(line.cells, static_cast<std::size_t>(line.length)), names, writer) || folded.pens.size() > maxPensPerFoldedBlock)) {
            refuse();
            return;
        }
        folded.entries.push_back(entry);
    }

    // The slab goes only once its decode reproduced every live cell. A check
    // slab the heap refuses leaves the block raw, to be offered again.
    Cell* scratch = m_retiredSlab ? std::exchange(m_retiredSlab, nullptr) : new (std::nothrow) Cell[static_cast<std::size_t>(blockCells)];
    if (!scratch) {
        b.folded.reset();
        return;
    }
    bool isExact = true;
    const bool isDecoded = decodeBlock(index, scratch, [&isExact](const StoredLine& line, const Cell* cells) {
        isExact = isExact && std::memcmp(line.cells, cells, static_cast<std::size_t>(line.length) * sizeof(Cell)) == 0;
    });
    if (!isDecoded || !isExact) {
        parkSlab(scratch);
        refuse();
        return;
    }
    for (std::uint64_t ordinal = std::max(b.firstOrdinal, front); ordinal < end; ++ordinal)
        descriptor(static_cast<int>(ordinal - front)).cells = nullptr;
    parkSlab(b.base.release());
    parkTextSlab(b.text.release());
    b.capacity = 0;
    parkSlab(scratch);
    b.isFolded = true;
    folded.forms.shrink_to_fit();
    folded.entries.shrink_to_fit();
    folded.pens.shrink_to_fit();
}

void LineStore::unfoldBlock(std::size_t index)
{
    Block& b = m_blocks[index];
    Cell* slab = takeSlab();
    int used = 0;
    const bool isDecoded = decodeBlock(index, slab, [&used](StoredLine& line, Cell* cells) {
        line.cells = cells;
        used += line.length;
    });
    // The forms decoded once already, at the fold; a drop only shortens them
    // by whole forms.
    assert(isDecoded);
    (void)isDecoded;
    dropDecodedBlock(b.sequence);
    b.base.reset(slab);
    b.capacity = blockCells;
    b.used = used;
    b.isFolded = false;
    b.folded.reset();
}

bool LineStore::decodeRowForm(std::span<const std::uint8_t> form, std::span<const Pen> pens, std::span<Cell> cells)
{
    WireReader reader(form);
    return Screen::readRowForm(reader, pens, everyClusterResolves, cells) && reader.isAtEnd();
}

void LineStore::decodeLine(int index) const
{
    const std::uint64_t ordinal = frontOrdinal() + static_cast<std::uint64_t>(index);
    const std::size_t block = blockIndexHolding(ordinal);
    const Block& b = m_blocks[block];
    if (!b.isFolded)
        return;
    DecodedBlock* entry = nullptr;
    for (DecodedBlock& decoded : m_decoded) {
        if (decoded.sequence == b.sequence)
            entry = &decoded;
    }
    if (!entry) {
        // A free slot with a slab, else any free slot, else the least recently used.
        for (DecodedBlock& decoded : m_decoded) {
            if (decoded.sequence == 0 && (!entry || (!entry->cells && decoded.cells)))
                entry = &decoded;
        }
        if (!entry) {
            entry = &m_decoded.front();
            for (DecodedBlock& decoded : m_decoded) {
                if (decoded.lastUse < entry->lastUse)
                    entry = &decoded;
            }
            forgetDecodedBlock(*entry);
        }
        if (!entry->cells)
            entry->cells = new Cell[static_cast<std::size_t>(blockCells)];
    }
    const bool isDecoded = decodeBlock(block, entry->cells, [](StoredLine& line, Cell* cells) { line.cells = cells; });
    assert(isDecoded);
    (void)isDecoded;
    entry->sequence = b.sequence;
    entry->lastUse = ++m_decodeClock;
}

void LineStore::forgetDecodedBlock(DecodedBlock& decoded) const
{
    if (decoded.sequence == 0)
        return;
    // Blocks are in sequence order in the directory. The block may be gone: an
    // evicted block drops its entry as it goes, but a drop that races nothing
    // still costs nothing to tolerate.
    std::size_t low = 0;
    std::size_t high = m_blocks.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (m_blocks[middle].sequence < decoded.sequence)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < m_blocks.size() && m_blocks[low].sequence == decoded.sequence && m_blocks[low].isFolded) {
        const std::uint64_t front = frontOrdinal();
        const std::uint64_t begin = std::max(m_blocks[low].firstOrdinal, front);
        const std::uint64_t end = blockEnd(low);
        for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal)
            descriptor(static_cast<int>(ordinal - front)).cells = nullptr;
    }
    decoded.sequence = 0;
}

void LineStore::dropDecodedBlock(std::uint32_t sequence) const
{
    for (DecodedBlock& decoded : m_decoded) {
        if (decoded.sequence == sequence)
            decoded.sequence = 0;
    }
}

bool LineStore::foldColdBlocks(int blockBudget)
{
    const std::size_t coldEnd = m_blocks.size() > static_cast<std::size_t>(rawBlockWindow) ? m_blocks.size() - static_cast<std::size_t>(rawBlockWindow) : 0;
    // A reader may be reading a raw block, and a fold rewrites its form and
    // reuses its slab: the work waits for the reader, and is still owed.
    const bool hasReader = m_gate && m_gate->readerCount.load(std::memory_order_acquire) > 0;
    for (std::size_t index = 0; index < coldEnd; ++index) {
        const Block& b = m_blocks[index];
        if (b.isFolded || b.isFoldRefused)
            continue;
        if (blockBudget <= 0 || hasReader)
            return true;
        foldBlock(index);
        --blockBudget;
    }
    return false;
}

void LineStore::dropOldest(int lineCount)
{
    if (lineCount <= 0)
        return;
    if (lineCount > m_count)
        lineCount = m_count;
    for (int i = 0; i < lineCount; ++i)
        evictOldest();
}

void LineStore::dropNewest(int lineCount)
{
    if (lineCount <= 0 || m_count <= 0)
        return;
    // The next append overwrites the cells this releases.
    quiesceReaders();
    if (lineCount > m_count)
        lineCount = m_count;
    // Lines fill blocks front-to-back, so the newest lineCount lines occupy a
    // contiguous suffix of the back block(s). Roll back each dropped line's
    // cells and line tally; pop a back block once it holds no lines (freeing
    // its slab). The descriptor ring just shrinks (m_count).
    for (int i = 0; i < lineCount; ++i) {
        const StoredLine& line = descriptor(m_count - 1);
        // A row that closed its line takes the line with it: the head retreats
        // by whole lines, and the ordinal is reused by the next stored row.
        if (!line.wrapped)
            --m_headLine;
        --m_nextOrdinal;
        if (line.length > 0 && !m_blocks.empty()) {
            Block& b = m_blocks.back();
            if (b.isFolded) {
                // The line's form is the tail of the block's forms; the slots
                // past it named blank lines that are gone too.
                const std::size_t slot = static_cast<std::size_t>(m_nextOrdinal - b.firstOrdinal);
                b.folded->forms.resize(b.folded->entries[slot].offset);
                b.folded->entries.resize(slot);
            } else {
                b.used -= line.length;
                if (b.used < 0)
                    b.used = 0;
            }
            --b.linesAppended;
            // linesAppended counts every line the block ever backed, and when
            // this block is also the FRONT block that includes lines already
            // evicted from the front — lines with no descriptor left, whose
            // cells no reader can reach. The block is free once only those
            // remain; against zero it held the evicted tally forever, and the
            // tally reset below then orphaned the slab for the store's life.
            const bool isAlsoFrontBlock = m_blocks.size() == 1;
            const int evictedRemainder = isAlsoFrontBlock ? m_frontEvicted : 0;
            if (b.linesAppended <= evictedRemainder) {
                releaseBlock(b);
                m_blocks.popBack();
                if (isAlsoFrontBlock)
                    m_frontEvicted = 0;
            }
        }
        --m_count;
    }
    // Shrinking is the idle direction; hold no spare slab past it.
    releaseRetiredSlab();
    if (m_count == 0) {
        // No descriptor references any slab now, so a block still standing
        // holds only front-evicted lines' cells; freeing it here is what keeps
        // the front-eviction tally's reset from orphaning it.
        for (std::size_t index = 0; index < m_blocks.size(); ++index)
            releaseBlock(m_blocks[index]);
        m_blocks.clear();
        releaseRetiredSlab();
        for (DecodedBlock& decoded : m_decoded) {
            delete[] decoded.cells;
            decoded = {};
        }
        m_start = 0;
        m_frontEvicted = 0;
        m_frontLine = m_headLine;
    }
}

void LineStore::clear()
{
    quiesceReaders();
    // The next stored row continues the open line when the newest row wrapped
    // into it, and that line's start is gone with the rows.
    if (m_count > 0)
        m_frontContinuity = descriptor(m_count - 1).wrapped ? LineContinuity::ContinuesLine : LineContinuity::StartsLine;
    m_frontLine = m_headLine;
    for (std::size_t index = 0; index < m_blocks.size(); ++index)
        releaseBlock(m_blocks[index]);
    m_blocks.clear();
    releaseRetiredSlab();
    for (DecodedBlock& decoded : m_decoded) {
        delete[] decoded.cells;
        decoded = {};
    }
    m_start = 0;
    m_count = 0;
    m_frontEvicted = 0;
}

std::size_t LineStore::arenaBytes() const
{
    std::size_t total = 0;
    for (std::size_t index = 0; index < m_blocks.size(); ++index) {
        const Block& b = m_blocks[index];
        if (b.base)
            total += static_cast<std::size_t>(b.capacity) * sizeof(Cell);
        else if (b.folded)
            total += b.folded->forms.size() + b.folded->entries.size() * sizeof(FormEntry) + b.folded->pens.size() * sizeof(Pen);
        if (b.text)
            total += static_cast<std::size_t>(blockCells);
    }
    return total;
}

std::size_t LineStore::foldedBytes() const
{
    std::size_t total = 0;
    for (std::size_t index = 0; index < m_blocks.size(); ++index) {
        const Block& b = m_blocks[index];
        if (b.isFolded && b.folded)
            total += b.folded->forms.size() + b.folded->entries.size() * sizeof(FormEntry) + b.folded->pens.size() * sizeof(Pen);
    }
    return total;
}

std::size_t LineStore::decodedCacheBytes() const
{
    std::size_t total = 0;
    for (const DecodedBlock& decoded : m_decoded) {
        if (decoded.cells)
            total += static_cast<std::size_t>(blockCells) * sizeof(Cell);
    }
    return total;
}

int LineStore::foldedBlockCount() const
{
    int count = 0;
    for (std::size_t index = 0; index < m_blocks.size(); ++index)
        count += m_blocks[index].isFolded ? 1 : 0;
    return count;
}

std::expected<std::size_t, LineStore::StorageError> LineStore::ownedBackingBytes() const
{
    std::size_t total = 0;
    if (!addArrayBytes(total, m_ring ? static_cast<std::size_t>(m_capacity) : 0, sizeof(StoredLine))
        || !addArrayBytes(total, m_lineIndexCount, sizeof(std::uint64_t))
        || !addArrayBytes(total, m_blocks.capacity(), sizeof(Block)))
        return std::unexpected(StorageError::OverCapacity);
    for (std::size_t index = 0; index < m_blocks.size(); ++index) {
        const Block& block = m_blocks[index];
        if (!addArrayBytes(total, block.base ? static_cast<std::size_t>(block.capacity) : 0, sizeof(Cell))
            || !addBytes(total, block.text ? static_cast<std::size_t>(blockCells) : 0))
            return std::unexpected(StorageError::OverCapacity);
        if (block.folded
            && (!addBytes(total, sizeof(FoldedForms))
                || !addArrayBytes(total, block.folded->forms.capacity(), sizeof(std::uint8_t))
                || !addArrayBytes(total, block.folded->entries.capacity(), sizeof(FormEntry))
                || !addArrayBytes(total, block.folded->pens.capacity(), sizeof(Pen))))
            return std::unexpected(StorageError::OverCapacity);
    }
    if (m_retiredSlab && !addArrayBytes(total, blockCells, sizeof(Cell)))
        return std::unexpected(StorageError::OverCapacity);
    for (const auto& decoded : m_decoded) {
        if (decoded.cells && !addArrayBytes(total, blockCells, sizeof(Cell)))
            return std::unexpected(StorageError::OverCapacity);
    }
    return total;
}

int LineStore::decodedBlockCount() const
{
    int count = 0;
    for (const DecodedBlock& decoded : m_decoded)
        count += decoded.sequence != 0 ? 1 : 0;
    return count;
}

// ---- Readers on another thread -------------------------------------------

LineStore::ReaderGate::~ReaderGate() { releaseDeferred(); }

void LineStore::ReaderGate::releaseDeferred()
{
    delete[] deferredSlab.exchange(nullptr, std::memory_order_acq_rel);
    delete[] deferredText.exchange(nullptr, std::memory_order_acq_rel);
    delete deferredForms.exchange(nullptr, std::memory_order_acq_rel);
}

void LineStore::releaseSnapshot(Snapshot& snapshot)
{
    if (snapshot.isRegistered && snapshot.gate)
        snapshot.gate->readerCount.fetch_sub(1, std::memory_order_acq_rel);
    snapshot.isRegistered = false;
    snapshot.gate.reset();
    snapshot.regions.clear();
    snapshot.facts.clear();
}

namespace {

LineStore::RowFacts factsOf(const StoredLine& line)
{
    return { .length = static_cast<std::uint16_t>(line.length), .isWrapped = line.wrapped, .needsShaping = line.needsShaping, .isCompact = line.isCompact, .lineAttribute = line.lineAttribute };
}

} // namespace

bool LineStore::captureSnapshot(Snapshot& snapshot, std::size_t maxStartFacts, std::optional<SnapshotRange> range) const
{
    if (snapshot.gate != m_gate)
        releaseSnapshot(snapshot);
    snapshot.regions.clear();
    snapshot.facts.clear();
    if (!m_gate)
        return false;
    // The first reader brings the lock, so a store no thread reads never
    // needs a platform lock to exist.
    if (!m_gate->lock)
        m_gate->lock = std::move(snapshot.spareLock);
    if (!m_gate->lock)
        return false;
    if (!snapshot.isRegistered) {
        // Under the model lock, which every mutation that checks the count
        // also holds, so the count is exact for them.
        m_gate->readerCount.fetch_add(1, std::memory_order_acq_rel);
        snapshot.gate = m_gate;
        snapshot.isRegistered = true;
    }
    snapshot.invalidation = m_gate->invalidation.load(std::memory_order_acquire);
    snapshot.frontOrdinal = frontOrdinal();
    snapshot.nextOrdinal = m_nextOrdinal;
    snapshot.headLine = m_headLine;
    snapshot.front = frontLine();
    if (range) {
        const std::uint64_t firstLine = std::clamp(range->firstLine, frontLineNumber(), m_headLine);
        const std::uint64_t endLine = std::clamp(range->endLine, firstLine, m_headLine);
        snapshot.headLine = endLine;
        snapshot.front = { firstLine, LineContinuity::StartsLine };
        if (firstLine == endLine) {
            snapshot.frontOrdinal = m_nextOrdinal;
            snapshot.nextOrdinal = m_nextOrdinal;
        } else {
            const auto first = logicalLine(firstLine);
            const auto last = logicalLine(endLine - 1);
            if (!first || !last)
                return false;
            snapshot.frontOrdinal = frontOrdinal() + static_cast<std::uint64_t>(first->firstIndex);
            snapshot.nextOrdinal = frontOrdinal() + static_cast<std::uint64_t>(last->firstIndex + last->rowCount);
        }
    }
    if (!m_ring || snapshot.frontOrdinal == snapshot.nextOrdinal)
        return true;
    const std::size_t firstBlock = m_blocks.empty() ? 0 : blockIndexHolding(snapshot.frontOrdinal);
    const std::size_t endBlock = m_blocks.empty() ? 0 : blockIndexHolding(snapshot.nextOrdinal - 1) + 1;
    if (!snapshot.regions.tryReserve(endBlock - firstBlock + 1))
        return false;

    const std::uint64_t front = snapshot.frontOrdinal;
    // Blank rows ahead of every block that backs cells: the rest of a released
    // block's span, or the whole history when no row has cells.
    const std::uint64_t firstBlocked = m_blocks.empty() ? snapshot.nextOrdinal : std::min(snapshot.nextOrdinal, std::max(front, m_blocks[0].firstOrdinal));
    if (front < firstBlocked) {
        SnapshotRegion blank;
        blank.firstOrdinal = front;
        blank.endOrdinal = firstBlocked;
        (void)snapshot.regions.tryPushBack(blank);
    }
    for (std::size_t index = firstBlock; index < endBlock; ++index) {
        const Block& block = m_blocks[index];
        const std::uint64_t begin = std::max(block.firstOrdinal, front);
        const std::uint64_t end = std::min(blockEnd(index), snapshot.nextOrdinal);
        if (begin >= end)
            continue;
        SnapshotRegion region;
        region.firstOrdinal = begin;
        region.endOrdinal = end;
        region.sequence = block.sequence;
        region.isFolded = block.isFolded;
        if (block.isFolded && block.folded) {
            region.forms = block.folded->forms;
            region.entries = block.folded->entries;
            region.pens = block.folded->pens;
            region.entryOrdinal = block.firstOrdinal;
        }
        (void)snapshot.regions.tryPushBack(region);
    }

    // The newest rows' facts now, so a search of the recent history needs no
    // second lock; the rest as the scan reaches them.
    std::size_t budget = maxStartFacts;
    for (std::size_t index = snapshot.regions.size(); index-- > 0;) {
        SnapshotRegion& region = snapshot.regions[index];
        if (region.isFolded)
            continue;
        const auto rows = static_cast<std::size_t>(region.endOrdinal - region.firstOrdinal);
        if (rows > budget)
            break;
        if (!captureRegionFacts(snapshot, index))
            return false;
        budget -= rows;
    }
    return true;
}

bool LineStore::captureRegionFacts(Snapshot& snapshot, std::size_t regionIndex) const
{
    if (snapshot.gate != m_gate || snapshot.isStale() || regionIndex >= snapshot.regions.size() || !m_ring)
        return false;
    SnapshotRegion& region = snapshot.regions[regionIndex];
    if (region.isFolded || region.hasFacts)
        return true;
    // Eviction since the snapshot took the region's oldest rows with it.
    const std::uint64_t front = frontOrdinal();
    const std::uint64_t begin = std::max(region.firstOrdinal, front);
    if (begin >= region.endOrdinal)
        return false;
    const auto rows = static_cast<std::size_t>(region.endOrdinal - begin);
    if (snapshot.facts.size() + rows > std::numeric_limits<std::uint32_t>::max() || !snapshot.facts.tryReserve(snapshot.facts.size() + rows))
        return false;
    region.firstOrdinal = begin;
    if (front > snapshot.frontOrdinal) {
        snapshot.frontOrdinal = front;
        snapshot.front = frontLine();
    }
    region.factsOffset = static_cast<std::uint32_t>(snapshot.facts.size());
    region.cells = nullptr;
    region.text = nullptr;
    for (std::uint64_t ordinal = begin; ordinal < region.endOrdinal; ++ordinal) {
        const StoredLine& line = descriptor(static_cast<int>(ordinal - front));
        if (!region.cells && line.length > 0)
            region.cells = line.cells;
        (void)snapshot.facts.tryPushBack(factsOf(line));
    }
    // The block still stands: a region with a live row has not been retired,
    // and a rewrite since the snapshot was refused above as stale. The text
    // starts where the region's first cells do, so a row's bytes sit at the
    // offset its cells have from `cells`, evicted rows ahead of it included.
    if (region.sequence && region.cells) {
        const Block& block = m_blocks[blockIndexHolding(begin)];
        region.text = block.text ? block.text.get() + (region.cells - block.base.get()) : nullptr;
    }
    region.hasFacts = true;
    return true;
}

LineStore::RegionRead::RegionRead(const Snapshot& snapshot, const SnapshotRegion& region)
{
    if (!snapshot.gate || !snapshot.gate->lock)
        return;
    if (!region.sequence) {
        // Blank rows: the facts are the snapshot's own, and there is no
        // memory of the store's to hold.
        m_isValid = region.hasFacts;
        return;
    }
    m_gate = snapshot.gate.get();
    m_gate->lock->lock();
    m_gate->hazardSequence.store(region.sequence, std::memory_order_seq_cst);
    m_isValid = m_gate->oldestLiveSequence.load(std::memory_order_seq_cst) <= region.sequence
        && m_gate->invalidation.load(std::memory_order_seq_cst) == snapshot.invalidation;
}

LineStore::RegionRead::~RegionRead()
{
    if (!m_gate)
        return;
    m_gate->hazardSequence.store(0, std::memory_order_seq_cst);
    m_gate->releaseDeferred();
    m_gate->lock->unlock();
}

} // namespace Ubiquitty
