// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphBackendTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// ShapedCellCache — what the shaper answered for a row segment, held between
// frames so an unchanged segment is not shaped twice.
//
// Named for CELLS because "shaped row" already means something else here: the
// grid memoizes its Arabic-shaping and bidi pass per row under that name
// (`CellGrid::lookupShapedRow`), and that answers what a row's CELLS are. This
// answers which GLYPHS those cells draw, once the face's contextual rules have
// chosen among letterforms. Two caches, two questions, one word between them.
//
// The cache holds no pixels, which is what licenses its key to be
// combinatorial. A cache of run pixels keyed by the run's text inverts both
// properties: the text fragments of one screen outnumber an atlas sheet's
// slots and grow with the corpus, while the shaped answers saturate at the
// face's repertoire. Pixels are keyed by the answer —
// `shapedGlyphKey` — and the answer is keyed by the question here.
//
// Retention is the frame first, then a bounded past. Two frames ping-pong: a
// lookup that misses the current frame is migrated forward out of the previous
// one, and `endFrame` swaps them. What falls out of the swap is held a while
// longer in a kept set ordered by recency and capped in entries: a prompt
// line, a status bar or a token that scrolls back into view is answered rather
// than shaped again, and a segment that never returns is the least recent and
// the first evicted. The working set cannot thrash — the frames hold exactly
// what the last frame used — and the kept set's cost is its capacity:
// `keptSegmentCapacity` entries of at most one run's words and cells each.
//
// An answer can be empty: a run the shaper resolves to no per-cell identities
// draws from a strip alone, and that verdict is kept like any other, so the
// run is not shaped again on every frame its strip is already rasterised
// for. `holdsNoAnswer` tells that verdict apart from a segment never seen.

namespace Ubiquitty::Rendering {

/// The code points a segment is keyed by, context included.
///
/// Named because a span's element type is a template argument with no Swift
/// spelling, so an unnamed one drops the declaration from the generated
/// interface rather than failing to compile.
using ShapedSegmentSpan = std::span<const std::uint32_t>;

/// One segment's shaped answers, one entry per drawn cell.
using ShapedCellSpan = std::span<const ShapedCell>;

/// The shaped answers for a row segment, keyed by the segment and its style.
class ShapedCellCache {
public:
    // NOESCAPE on codePoints/cells: append() copies both into the frame's own
    // Text/cells vectors (ShapedCellCache.cpp), so the returned span views the
    // cache's storage, never the caller's -- Swift's importer cannot see that
    // C++-level fact on its own, and without the annotation it conservatively
    // ties the RETURNED span's lifetime to these arguments', which refuses a
    // caller whose span is a temporary (a Span<T>.extracting(...) result, in
    // particular) that dies at the end of the call expression.

    /// Returns the answers for a segment shaped before, or an empty span.
    ///
    /// A hit in the previous frame is migrated into the current one, so a
    /// segment that keeps being drawn keeps being held and one that stops is
    /// dropped a frame later without anything deciding to evict it.
    ///
    /// - Parameters:
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: ``ligatureShapeIdentity`` for the segment's cells.
    /// - Returns: The answers, or an empty span when the segment is unseen.
    [[nodiscard]] ShapedCellSpan lookup(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity);

    /// Records the answers for a segment.
    ///
    /// - Parameters:
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: ``ligatureShapeIdentity`` for the segment's cells.
    ///   - cells: The shaper's answer, one entry per drawn cell.
    /// - Returns: The stored span, which stays valid until this segment's
    ///   frame is cleared.
    ShapedCellSpan publish(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity, ShapedCellSpan cells UBIQUITTY_NOESCAPE);

    /// Ends the frame: what this frame used is kept, what it did not is
    /// dropped.
    void endFrame();

    /// How many lookups this frame were answered from a previous shaping.
    [[nodiscard]] std::uint32_t hitsThisFrame() const { return m_hits; }

    /// How many lookups this frame had to be shaped.
    [[nodiscard]] std::uint32_t missesThisFrame() const { return m_misses; }

    /// How many segments the two live frames hold.
    [[nodiscard]] std::size_t retainedSegments() const { return m_current.entries.size() + m_previous.entries.size(); }

    /// How many segments the kept set holds beyond the two frames.
    [[nodiscard]] std::size_t keptSegments() const { return m_keptCount; }

    /// Whether the current frame holds the segment as one the shaper answered
    /// with no identities.
    ///
    /// Asked after a ``lookup`` that returned nothing, which has migrated any
    /// such record into the current frame: an empty answer and an unseen
    /// segment look alike to the caller otherwise.
    ///
    /// Not const, because const costs a copy of the whole cache here. A Swift
    /// caller holding it as a stored property reaches a const member through
    /// the property's READ accessor,
    /// which for a type Swift keeps indirectly yields a whole copy — every
    /// vector here, once per call. A mutating member is reached through
    /// `modify`, which borrows in place. Measured under a flood, that copy is
    /// three quarters of the modern arm's main-thread CPU, and the window stops
    /// answering its run loop while it runs.
    ///
    /// - Parameters:
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: ``ligatureShapeIdentity`` for the segment's cells.
    /// - Returns: Whether the segment is known to draw from a strip alone.
    [[nodiscard]] bool holdsNoAnswer(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity);

private:
    /// One segment's record. The text and the answers live in the frame's own
    /// arrays rather than in the record, so a frame's storage is reused rather
    /// than reallocated per segment: the steady state allocates nothing.
    struct Entry {
        std::uint64_t hash = 0;
        std::uint32_t shapeIdentity = 0;
        std::uint32_t textOffset = 0;
        std::uint32_t textCount = 0;
        std::uint32_t cellOffset = 0;
        std::uint32_t cellCount = 0;
    };

    /// One frame's records and the arrays they point into.
    ///
    /// The index maps a fold to entry positions and the code points decide
    /// among them, so a hash that is not injective over variable-length text
    /// narrows the search rather than answering it.
    ///
    /// Open-addressed over the entry positions rather than a node table. The
    /// index is rebuilt every frame, so a node per segment is a malloc and a free
    /// per segment on the frame path; these slots are one allocation that
    /// outlives every frame. Entries sharing a fold sit in consecutive slots and
    /// the probe stops at the first empty one, so the walk that decides among
    /// them reads the same records it did before.
    struct Frame {
        std::vector<Entry> entries;
        std::vector<std::uint32_t> text;
        std::vector<ShapedCell> cells;
        /// `hash & (index.size() - 1)` → entry position, `noSlot` where empty.
        /// Empty before the first segment; a power of two once filled.
        std::vector<std::uint32_t> index;

        /// Empties the frame, keeping the storage for the next one to fill.
        void clear()
        {
            entries.clear();
            text.clear();
            cells.clear();
            std::ranges::fill(index, noSlot);
        }
    };

    /// The entry index for a segment, or -1 when the frame does not hold it.
    ///
    /// The hash narrows and the code points decide: a hash is not injective
    /// over variable-length text, and a segment served under another's answer
    /// draws the wrong letterforms with every count correct.
    ///
    /// - Parameters:
    ///   - frame: The frame to search.
    ///   - hash: The segment's fold.
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: The style the segment is drawn in.
    /// - Returns: The entry's position, or -1.
    [[nodiscard]] static int find(const Frame& frame, std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity);

    /// Copies a record and its arrays into a frame.
    ///
    /// - Parameters:
    ///   - frame: The frame to grow.
    ///   - hash: The segment's fold, already computed by the caller.
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: The style the segment was shaped in.
    ///   - cells: The answers to copy.
    /// - Returns: The stored answers, valid until that frame is cleared.
    static ShapedCellSpan append(Frame& frame, std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity, ShapedCellSpan cells);

    /// Writes an entry position into the frame's index at its fold's home.
    ///
    /// - Parameters:
    ///   - frame: The frame whose index is written; it has a free slot.
    ///   - hash: The entry's fold.
    ///   - position: The entry's position in `frame.entries`.
    static void indexEntry(Frame& frame, std::uint64_t hash, std::uint32_t position);

    /// Grows the frame's index so one more entry leaves it under its load bound,
    /// re-indexing what it already holds.
    ///
    /// A probe walks to the first empty slot, so a table allowed to fill turns
    /// every miss into a walk of the whole table.
    ///
    /// - Parameter frame: The frame about to gain an entry.
    static void reserveIndex(Frame& frame);

    /// No slot: the end of a recency chain, an empty index cell, an empty free list.
    static constexpr std::uint32_t noSlot = 0xFFFF'FFFFu;
    /// The frame index's first size, grown by doubling. A screen of text holds a
    /// few hundred distinct segments, so this is the order a steady frame wants
    /// and the growth is paid once rather than per frame.
    static constexpr std::size_t frameIndexInitialCapacity = 512;
    /// Words a kept segment holds inline: `ligatureMaxCells` drawn cells,
    /// `ligatureContextCells` each side, two flank spaces and the key's own
    /// words, rounded up. The walk produces nothing wider; a wider segment is
    /// not kept.
    static constexpr std::size_t keptSegmentWords = 40;
    static_assert(keptSegmentWords >= static_cast<std::size_t>(ligatureMaxCells + 2 * ligatureContextCells) + 5);
    /// Answers a kept segment holds inline: one per drawn cell.
    static constexpr std::size_t keptSegmentCells = static_cast<std::size_t>(ligatureMaxCells);
    /// The most segments the kept set holds. A slot is a few hundred bytes, so
    /// the slab is under two megabytes, allocated once on the first segment kept.
    static constexpr std::size_t keptSegmentCapacity = 4096;
    /// The open-addressed index over the slab: a power of two, twice the slab,
    /// so a probe chain stays a few cells long at full occupancy.
    static constexpr std::size_t keptIndexCapacity = 8192;
    static_assert((keptIndexCapacity & (keptIndexCapacity - 1)) == 0 && keptIndexCapacity >= 2 * keptSegmentCapacity);

    /// A segment that fell out of the frames, held by recency in a slab.
    ///
    /// The words and the answers live inline, so keeping one copies into a
    /// slot that already exists and frees nothing; recency is a pair of slot
    /// indices, so promoting or evicting one moves no payload. A free slot
    /// chains to the next free through `newer`.
    struct Kept {
        std::uint64_t hash = 0;
        std::uint32_t shapeIdentity = 0;
        std::uint16_t textCount = 0;
        std::uint16_t cellCount = 0;
        std::uint32_t newer = noSlot;
        std::uint32_t older = noSlot;
        std::array<std::uint32_t, keptSegmentWords> text {};
        std::array<ShapedCell, keptSegmentCells> cells {};
    };

    /// The kept slot holding a segment, or ``noSlot``.
    ///
    /// Linear probing from the hash's home cell: the hash narrows, the
    /// identity and the code points decide.
    ///
    /// - Parameters:
    ///   - hash: The segment's fold.
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: The style the segment was shaped in.
    /// - Returns: The slot, or ``noSlot`` when the set does not hold it.
    [[nodiscard]] std::uint32_t findKept(std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity) const;

    /// Keeps a segment as the most recent, evicting the least recent when the
    /// slab is full. A segment already kept, or wider than a slot, is left as
    /// it is.
    ///
    /// - Parameters:
    ///   - hash: The segment's fold.
    ///   - codePoints: The segment's code points, context included.
    ///   - shapeIdentity: The style the segment was shaped in.
    ///   - cells: The answers to keep; empty for a run the shaper cannot answer.
    void keepSegment(std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity, ShapedCellSpan cells);

    /// Releases a kept slot: out of the recency chain, out of the index by
    /// backward shift so no tombstone lengthens a later probe, onto the free
    /// list.
    ///
    /// - Parameter slot: The slot to release.
    void forgetKept(std::uint32_t slot);

    /// Moves what the previous frame holds and the current does not into the
    /// kept set, most recent first.
    void keepDroppedSegments();

    Frame m_current;
    Frame m_previous;
    std::vector<Kept> m_kept; // the slab: empty until the first segment is kept, then keptSegmentCapacity
    std::vector<std::uint32_t> m_keptIndex; // hash & (keptIndexCapacity - 1) → slot, noSlot where empty
    std::uint32_t m_keptNewest = noSlot;
    std::uint32_t m_keptOldest = noSlot;
    std::uint32_t m_keptFree = noSlot;
    std::uint32_t m_keptTaken = 0; // slots ever taken: the next fresh one while the free list is empty
    std::uint32_t m_keptCount = 0;
    std::uint32_t m_hits = 0;
    std::uint32_t m_misses = 0;
};

} // namespace Ubiquitty::Rendering
