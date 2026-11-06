// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Bits/BitField.h"
#include "Core/Attributes/Hyperlinks.h"
#include "Core/Pen.h"
#include "Core/VT/Constants.h"
#include "Forward.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>

// Inline cluster-continuation slots beyond the base codePoint. Kept at 1 so
// Cell stays 32 B and memcmp-comparable; longer clusters spill whole into the
// cluster pool (clusterID).
namespace Ubiquitty {

/// First byte a batched run translation covers; the entry for `byte` sits at
/// `byte - runTranslationFirstByte`.
///
/// A run translation lets a batched writer put a designated character set's
/// glyphs in cells without giving up the batch: the table stands in for a
/// per-byte resolve, so one byte still becomes exactly one cell.
inline constexpr std::uint8_t runTranslationFirstByte = 0x20;

/// Entry count of a batched run translation: the printable GL range, `0x20..0x7E`.
inline constexpr int runTranslationByteCount = 0x7F - runTranslationFirstByte;

/// How many designated sets a run translation is held for, one per 94-set id.
///
/// A set numbered past this range is never batched: the soft DRCS set resolves
/// through mutable per-session font state, which no prepared table describes.
inline constexpr int runTranslationSetCount = 16;

constexpr int cellMaxCombining = 1;

// Width of the `clusterPacked` field holding the inline combining count. The
// count is read out of a bit field rather than a whole byte, so its largest
// value is a property of that width — and a consumer iterating `combining` is
// in bounds from the count alone only while the two agree. Widening the array
// without widening the field, or the reverse, stops here.
constexpr int cellCombiningLengthBits = 1;
static_assert((1 << cellCombiningLengthBits) - 1 == cellMaxCombining,
    "the combining count's bit field must express exactly the array's bound");

struct Cell {
    // Cluster base, full Unicode range. 0 == blank / wide-continuation.
    uint32_t codePoint;
    Pen pen;
    // Cluster continuation codePoints in source order; painter reconstructs
    // [codePoint, combining[0..combiningLength)]. Zero when combiningLength == 0.
    std::array<uint32_t, cellMaxCombining> combining;
    // OSC 8 hyperlink id (0 == none), indexing Terminal's link table via
    // Ubiquitty::Attributes::Hyperlinks::uri. Split into two bytes, not a uint16_t, so Cell stays
    // zero-padded for the raw-memcmp probe.
    uint8_t linkIDLo, linkIDHi;
    // Packed inline combining length + cluster overflow id (0 == inline, else
    // indexes the Terminal cluster pool via Ubiquitty::cellClusterCodePoints);
    // layout in the accessors below, never read the raw word. uint16 keeps Cell
    // 32 B and zero-padded.
    uint16_t clusterPacked;

    [[nodiscard]] uint8_t combiningLength() const { return static_cast<uint8_t>(Ubiquitty::Bits::BitField<uint16_t, 0, cellCombiningLengthBits>::get(clusterPacked)); }
    void setCombiningLength(uint8_t length) { clusterPacked = Ubiquitty::Bits::BitField<uint16_t, 0, cellCombiningLengthBits>::set(clusterPacked, length); }

    // Writes preserve the combining-length bit sharing the word.
    [[nodiscard]] uint16_t clusterID() const { return Ubiquitty::Bits::BitField<uint16_t, cellCombiningLengthBits, 16 - cellCombiningLengthBits>::get(clusterPacked); }
    void setClusterID(uint16_t id) { clusterPacked = Ubiquitty::Bits::BitField<uint16_t, cellCombiningLengthBits, 16 - cellCombiningLengthBits>::set(clusterPacked, id); }

    [[nodiscard]] uint16_t linkID() const { return static_cast<uint16_t>(static_cast<uint16_t>(linkIDLo) | (static_cast<uint16_t>(linkIDHi) << 8)); }
    void setLinkID(uint16_t id)
    {
        linkIDLo = static_cast<uint8_t>(id & 0xFF);
        linkIDHi = static_cast<uint8_t>((id >> 8) & 0xFF);
    }
};

// Inline-pen Cell embeds Pen's non-trivial OptionSet fields, so it must never
// be memset/calloc'd (UB); it stays trivially copyable + zero-padded, which the
// raw-memcmp equality and whole-cell memcpy (reflow/scroll/snapshot) rely on.
static_assert(std::is_trivially_copyable_v<Cell>, "Cell must stay trivially copyable (memcmp equality + whole-cell memcpy)");
static_assert(std::is_trivially_destructible_v<Cell>, "Cell scratch storage relies on trivial destruction");
// is_trivially_copyable does not imply zero padding; memcmp equality does.
static_assert(std::has_unique_object_representations_v<Cell>, "Cell must stay padding-free (raw-memcmp equality)");
static_assert(sizeof(Cell) == 32, "the inline-pen Cell is exactly 32 bytes");

// Scratch storage that starts Cell lifetimes only for entries a caller writes:
// Cell's non-trivial default construction would otherwise clear every slot of a
// plain Cell[capacity]. memcpy into data() starts implicit-lifetime Cells;
// constructAt() constructs a single entry explicitly.
template<std::size_t capacity> class CellScratchBuffer {
public:
    Cell* data() { return reinterpret_cast<Cell*>(m_storage); }
    [[nodiscard]] const Cell* data() const { return reinterpret_cast<const Cell*>(m_storage); }

    Cell& constructAt(std::size_t index, const Cell& source)
    {
        assert(index < capacity);
        void* address = static_cast<void*>(m_storage + index * sizeof(Cell));
        return *::new (address) Cell(source);
    }

private:
    alignas(Cell) std::byte m_storage[sizeof(Cell) * capacity];
};

static_assert(std::is_trivially_default_constructible_v<CellScratchBuffer<maxColumns>>,
    "Cell scratch buffers must not initialize unused slots");

// Canonical blank: the value every empty cell must equal byte-for-byte so the
// memcmp probe treats independently-produced blanks as identical. codePoint 0
// is the unwritten sentinel, distinct from a typed space (0x20) so the written
// extent (selection/reflow/scrollback) can tell "app wrote a space" from "never
// touched". Pen colors sit at colorDefault (== penReset); all else zero.
inline constexpr Cell defaultBlankCell = { 0, { {}, colorDefault, colorDefault, colorDefault, SemanticRole::Output, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}, 0 }, { 0 }, 0, 0, 0 };

// cellClusterCodePoints returns the cluster's full codePoint sequence (base +
// combining marks / VS / ZWJ / flag halves / skin-tone) in source order.
// Writes up to `max` codePoints into `out`, returns the count written (pass
// max >= clusterPoolMaximumCodePoints to never clip). Reads the cell's inline
// buffer when Cell::clusterID is 0, else the cluster pool; an id the pool
// doesn't hold falls back to inline. Returns 0 for null args, max <= 0, or a blank cell.
[[nodiscard]] inline int cellClusterCodePoints(const Ubiquitty::Terminal* t, const Cell*, uint32_t* outputBuffer, int max);

/// Copies a cell's cluster out of the terminal's stores, for a cell whose
/// cluster id is non-zero.
///
/// The out-of-line half of ``cellClusterCodePoints``: a text-run anchor
/// reads RunTextStore; other cells read ClusterPool.
///
/// - Parameters:
///   - t: The terminal holding both stores.
///   - cell: A cell whose `clusterID()` is non-zero.
///   - outputBuffer: Where to write, base first.
///   - max: How many code points `outputBuffer` holds, at least one.
/// - Returns: The count written, or -1 when the store holds no entry for the
///   id and the cell's inline storage answers instead.
[[nodiscard]] int cellStoredClusterCodePoints(const Ubiquitty::Terminal& t, const Cell& cell, uint32_t* outputBuffer, int max);

/// Whether a text-size spec id names a proportional spec in `t`'s table.
///
/// The out-of-line half of the run predicates below; callers reach it only
/// for a cell whose pen carries a non-zero spec id.
[[nodiscard]] bool isProportionalTextSizeSpec(const Ubiquitty::Terminal& t, unsigned specID);

/// Whether a cell's cluster ID names RunTextStore rather than ClusterPool.
[[nodiscard]] bool isRunTextAnchor(const Ubiquitty::Terminal* t, const Cell& cell);

/// Whether a cell anchors an OSC 66 proportional run.
///
/// An anchor's cluster field then names the run's text in the terminal's
/// run-text store rather than a cluster-pool entry. The pen says which: a
/// text-size spec id whose spec is proportional, on a cell that is not a
/// continuation.
///
/// - Parameters:
///   - t: The terminal whose spec table resolves the pen's id.
///   - cell: The cell to ask about.
/// - Returns: Whether the cell anchors a proportional run; false for a null
///   terminal.
[[nodiscard]] inline bool isProportionalRunAnchor(const Ubiquitty::Terminal* t, const Cell& cell)
{
    const unsigned specID = cell.pen.extendedAttributes.textSizeSpecID();
    if (!t || specID == 0 || cell.pen.attributes.contains(VT::Attribute::WideContinuation))
        return false;
    return isProportionalTextSizeSpec(*t, specID);
}

/// Whether a cell continues an OSC 66 proportional run.
///
/// Such a cell carries only its run's claim. A reader that takes the run's
/// text whole at the anchor adds nothing for it, where a sized glyph's
/// continuation still reads as the blank half of a wide cell.
///
/// - Parameters:
///   - t: The terminal whose spec table resolves the pen's id.
///   - cell: The cell to ask about.
/// - Returns: Whether the cell continues a proportional run; false for a null
///   terminal.
[[nodiscard]] inline bool isProportionalRunContinuation(const Ubiquitty::Terminal* t, const Cell& cell)
{
    const unsigned specID = cell.pen.extendedAttributes.textSizeSpecID();
    if (!t || specID == 0 || !cell.pen.attributes.contains(VT::Attribute::WideContinuation))
        return false;
    return isProportionalTextSizeSpec(*t, specID);
}

/// The whole text of the OSC 66 run a cell anchors.
///
/// For a reader that needs the run rather than a cluster: copy, search,
/// reflow, the snapshot and the render builder. ``cellClusterCodePoints``
/// answers the same text clipped to its caller's buffer.
///
/// - Parameters:
///   - t: The terminal holding the run-text store.
///   - cell: The run's anchor.
/// - Returns: The run's code points; empty when the cell anchors no run or
///   its entry has been freed.
[[nodiscard]] std::span<const uint32_t> runTextCodePoints(const Ubiquitty::Terminal* t, const Cell& cell);

/// Writes a cell's cluster from its own INLINE storage, reaching no pool.
///
/// A cell whose marks fit in `combining` carries its whole cluster, so reading
/// it needs the cell and nothing else — no Terminal, no frontend snapshot, no
/// callback. That matters because the render builder reaches the cluster pool
/// through a function pointer into the frontend: an indirect call per cluster
/// cell that the compiler cannot inline through, paid on cells whose data was
/// already in hand.
///
/// Header-inline on purpose. It runs once per cluster cell in the render walk,
/// where a call would cost more than the work it wraps.
///
/// - Parameters:
///   - cell: The cell to read.
///   - outputBuffer: Where to write, base first.
///   - max: How many code points `outputBuffer` holds.
/// It does not test `clusterID`, because two callers need it on opposite sides
/// of that question: the builder reads inline storage only when the id is zero,
/// and a pool reader falls back to it when the id names no entry the pool
/// holds. Testing here would serve the first and silently return nothing to
/// the second.
///
/// - Returns: The count written, or 0 for a blank cell or no room.
[[nodiscard]] inline int cellInlineClusterCodePoints(const Cell& cell, uint32_t* outputBuffer, int max)
{
    if (cell.codePoint == 0 || !outputBuffer || max <= 0)
        return 0;
    int count = 0;
    outputBuffer[count++] = cell.codePoint;
    for (int i = 0; i < cell.combiningLength() && count < max; ++i)
        outputBuffer[count++] = cell.combining[i];
    return count;
}

// Header-inline so a plain cell, whose cluster lives in the cell itself, is
// read without a call; only a cell naming a store entry leaves the caller.
inline int cellClusterCodePoints(const Ubiquitty::Terminal* t, const Cell* cell, uint32_t* outputBuffer, int max)
{
    if (!t || !cell || !outputBuffer || max <= 0 || cell->codePoint == 0)
        return 0;
    if (cell->clusterID() != 0) [[unlikely]] {
        const int stored = cellStoredClusterCodePoints(*t, *cell, outputBuffer, max);
        if (stored >= 0)
            return stored;
    }
    return cellInlineClusterCodePoints(*cell, outputBuffer, max);
}

} // namespace Ubiquitty
