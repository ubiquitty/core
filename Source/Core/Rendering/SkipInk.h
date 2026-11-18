// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

// Skip-ink: where glyph ink cuts an underline, decided once, here.
//
// The decision is Core's for every renderer. A renderer draws the pieces of a
// rule the builder emits and samples no glyph for it, so the rule cannot read
// one way on the Mac, another on Linux and a third in a browser, and a change
// to it is made and tested in one place.
//
// The rule: a pixel of an underline is cut where glyph ink touches the
// underline near it. Ink touches when it lies in the underline's own rows or
// in a row adjacent to them; a letter with any background between it and the
// underline, the bowl of a p or an e above the line, leaves it whole. Beside
// ink that touches, the cut widens across by the padding, so the underline
// stops short of the stroke by exactly the padding, measured as a distance,
// edge to edge, in both directions: the line is cut wherever it lies within
// the padding of a touching stroke, as a circle of the padding's radius
// rolled along the stroke would clear it. So the cut follows a slanted tail
// along its slant, the side it leans toward is cleared as far from the line
// as a straight stem's sides are, and a stroke resting on the line bites into
// it as deep as the padding. Following a stroke never takes in a bowl it joins, so the
// belly of a p does not widen the cut under its stem. The stroke's edge is where
// its anti-aliased coverage puts it, not the texel it rounds to, and the cut
// ends where the padding from it does, inside a pixel as often as not; the
// pixel there is drawn in part, as the glyph's own edge is. So a glyph drawn at
// any subpixel phase is cleared by the same distance on both sides. Each row is
// judged on its own, so a stroke that touches only the top of a thick
// underline notches it rather than severing it. A texel is ink at half
// coverage or more.
//
// The ink comes from the coverage the glyphs were drawn from. A backend whose
// sheets live on the CPU hands those over as they are; one whose sheets live
// on a GPU keeps a ``SkipInkRecord`` of each sheet beside it, written wherever
// the backend uploads coverage, and hands over that.

namespace Ubiquitty::Rendering {

/// Which coverage sheet a skip-ink source reads.
///
/// A cell's ink is in whichever sheet holds the raster the cell draws: a
/// ligature run's strip, a proportional run's strip or its per-glyph sheet, a
/// wide glyph's two-cell sheet, or the mono atlas.
enum class SkipInkSheet : std::uint8_t {
    Mono,
    ProportionalStrip,
    LigatureStrip,
    /// The dedicated proportional sheet: a run drawn glyph by glyph has each
    /// glyph's ink in a slot of its own, so a rule under it reads the slot of
    /// the glyph beneath each cell rather than one strip over the claim.
    Proportional,
    /// The two-cell sheet: each column of a wide glyph reads its own half of
    /// the glyph's slot, so a rule under CJK is cut by the strokes above it as
    /// a rule under Latin is by descenders.
    Wide,
};

/// The texel coverage at which skip-ink counts a texel as ink: half.
inline constexpr std::uint8_t skipInkCoverageThreshold = 128;

/// How far above and below a pixel of a rule ink counts as touching it, in
/// device pixels: the row adjacent to the rule, and no farther. The padding
/// widens a cut across; it never lets ink a row away from the rule reach it.
inline constexpr float skipInkContactInDevicePixels = 1.0f;

/// One coverage sheet as skip-ink reads it, in the sheet's own texels: one
/// byte a texel, the coverage the glyphs were drawn from.
///
/// The bytes themselves rather than a texel's verdict, because the edge of a
/// stroke drawn at a subpixel phase lies inside a texel, and only the coverage
/// says where. A plane without bytes is absent, and nothing it would hold
/// cuts a rule.
struct UBIQUITTY_ESCAPABLE SkipInkPlane {
    const std::uint8_t* coverage = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    /// Bytes between rows of `coverage`.
    std::int32_t stride = 0;
};

/// Returns a plane over a sheet kept on the CPU, one coverage byte a texel,
/// row-major with no padding between rows.
///
/// - Parameters:
///   - coverage: The sheet's bytes.
///   - width: The sheet's width in texels.
///   - height: The sheet's height in texels.
/// - Returns: The plane; absent when the sheet is empty or its bytes are fewer
///   than its size names, so a short buffer is never read past its end.
[[nodiscard]] inline SkipInkPlane skipInkCoveragePlane(std::span<const std::uint8_t> coverage, std::int32_t width, std::int32_t height)
{
    SkipInkPlane plane;
    if (width <= 0 || height <= 0 || coverage.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return plane;
    plane.coverage = coverage.data();
    plane.width = width;
    plane.height = height;
    plane.stride = width;
    return plane;
}

/// Every sheet a frame's glyphs can be drawn from, as skip-ink reads them.
///
/// Returned once a frame by ``GlyphBackend::bindSkipInkPlanes``. A sheet the
/// backend does not have stays absent.
struct UBIQUITTY_ESCAPABLE SkipInkPlanes {
    SkipInkPlane mono;
    SkipInkPlane proportionalStrip;
    SkipInkPlane ligatureStrip;
    SkipInkPlane proportional;
    SkipInkPlane wide;

    /// Returns the plane a sheet's ink is read from.
    ///
    /// - Parameter sheet: The sheet.
    /// - Returns: Its plane, absent when the backend has none.
    [[nodiscard]] const SkipInkPlane& plane(SkipInkSheet sheet) const
    {
        switch (sheet) {
        case SkipInkSheet::ProportionalStrip:
            return proportionalStrip;
        case SkipInkSheet::LigatureStrip:
            return ligatureStrip;
        case SkipInkSheet::Proportional:
            return proportional;
        case SkipInkSheet::Wide:
            return wide;
        case SkipInkSheet::Mono:
            break;
        }
        return mono;
    }
};

class SkipInkRecord;

} // namespace Ubiquitty::Rendering

/// Claims a reference to `record` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else.
///
/// - Parameter record: The record to retain. A null pointer is ignored.
void retainSkipInkRecord(Ubiquitty::Rendering::SkipInkRecord* record);

/// Drops a reference to `record`, destroying it at zero.
///
/// - Parameter record: The record to release. A null pointer is ignored.
void releaseSkipInkRecord(Ubiquitty::Rendering::SkipInkRecord* record);

namespace Ubiquitty::Rendering {

/// The coverage of a sheet a backend uploads to a GPU, one byte a texel, kept
/// where the build can read it.
///
/// The build reads glyph ink on the CPU, and a GPU backend's sheets are where
/// it cannot reach them. Such a backend keeps one record per coverage sheet and
/// writes it with ``record`` from exactly the bytes and the region each upload
/// writes, where it stages them, so the record holds what the sheet holds —
/// including a glyph rasterised during the build that reads it. The bytes
/// rather than a bit a texel: a stroke's subpixel edge is in its coverage (see
/// ``SkipInkPlane``), and every backend then hands the cut the same thing.
///
/// Sized once. A sheet that changes size gets a new record, so a plane handed
/// to a build stays valid for it. Noncopyable and a shared reference for the
/// reasons ``GlyphSlotCache`` is: Swift holds it as a managed reference, and
/// the bytes it hands Core stay put.
class UBIQUITTY_NONCOPYABLE SkipInkRecord {
public:
    /// Makes an empty record: every texel uncovered until one is recorded.
    ///
    /// - Parameters:
    ///   - width: The sheet's width in texels.
    ///   - height: The sheet's height in texels.
    SkipInkRecord(std::int32_t width, std::int32_t height);

    /// Records the coverage of a region an upload writes.
    ///
    /// Every texel of the region is written, so the previous occupant of a
    /// reused slot leaves nothing behind. Texels outside the sheet are dropped.
    ///
    /// - Parameters:
    ///   - x: The region's left texel in the sheet.
    ///   - y: The region's top texel in the sheet.
    ///   - width: The region's width in texels.
    ///   - height: The region's height in texels.
    ///   - bytes: The uploaded texels, row by row.
    ///   - bytesPerRow: Bytes between rows of `bytes`.
    ///   - bytesPerTexel: Bytes per texel of `bytes`.
    ///   - coverageChannel: Which byte of a texel is its coverage.
    void record(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, const void* bytes, std::size_t bytesPerRow,
        std::int32_t bytesPerTexel, std::int32_t coverageChannel);

    /// Clears a region: a sheet region cleared or released covers nothing.
    ///
    /// - Parameters:
    ///   - x: The region's left texel.
    ///   - y: The region's top texel.
    ///   - width: The region's width in texels.
    ///   - height: The region's height in texels.
    void clear(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);

    /// Swift's references to a heap instance from ``makeSkipInkRecord``; a
    /// record a C++ owner embeds never has its count touched.
    std::atomic<int> swiftReferences { 1 };

private:
    friend SkipInkPlane skipInkPlaneOf(const SkipInkRecord*);

    /// Clips a region to the sheet, in place.
    ///
    /// - Parameters:
    ///   - x: The region's left texel, moved onto the sheet.
    ///   - y: The region's top texel, moved onto the sheet.
    ///   - width: The region's width, cut to what is on the sheet.
    ///   - height: The region's height, cut to what is on the sheet.
    ///   - skipColumns: Set to the columns cut from the region's left.
    ///   - skipRows: Set to the rows cut from the region's top.
    /// - Returns: False when nothing of the region is on the sheet.
    bool clip(std::int32_t& x, std::int32_t& y, std::int32_t& width, std::int32_t& height, std::int32_t& skipColumns, std::int32_t& skipRows) const;

    /// Frees the coverage `calloc` made.
    struct CoverageRelease {
        void operator()(std::uint8_t* coverage) const noexcept { std::free(coverage); }
    };

    // From calloc rather than a zero-filled vector: the system hands back zero
    // pages it commits only when they are written, so a sheet sized for the
    // largest screen costs memory only for the glyphs recorded on it, where a
    // vector's zero fill would commit the whole sheet up front.
    std::unique_ptr<std::uint8_t[], CoverageRelease> m_coverage;
    std::int32_t m_width = 0;
    std::int32_t m_height = 0;
} UBIQUITTY_SHARED_REFERENCE(retainSkipInkRecord, releaseSkipInkRecord);

} // namespace Ubiquitty::Rendering

/// Claims a reference to `record` on Swift's behalf.
///
/// - Parameter record: The record to retain. A null pointer is ignored.
inline void retainSkipInkRecord(Ubiquitty::Rendering::SkipInkRecord* record)
{
    if (record)
        record->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops a reference to `record`, destroying it at zero.
///
/// - Parameter record: The record to release. A null pointer is ignored.
inline void releaseSkipInkRecord(Ubiquitty::Rendering::SkipInkRecord* record)
{
    if (record && record->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete record;
}

namespace Ubiquitty::Rendering {

/// Returns a record as skip-ink reads it.
///
/// A function rather than a method because the plane points into the record,
/// and the Swift seam refuses a method that returns a pointer into its own
/// object. The record outlives the plane by the backend's contract: a sheet
/// and its record outlive every build that reads them.
///
/// - Parameter record: The record, or null for a sheet the backend lacks.
/// - Returns: A plane over the record's bytes; absent for a null or empty record.
[[nodiscard]] SkipInkPlane skipInkPlaneOf(const SkipInkRecord* record);

/// Allocates a record a reference-managing language can own.
///
/// - Parameters:
///   - width: The sheet's width in texels.
///   - height: The sheet's height in texels.
/// - Returns: An empty record with one reference held.
[[nodiscard]] inline SkipInkRecord* makeSkipInkRecord(std::int32_t width, std::int32_t height) UBIQUITTY_RETURNS_RETAINED
{
    return new SkipInkRecord(width, height);
}

/// One raster a rule is tested against, mapped onto the rule.
///
/// The rule's pixel at fraction `fx` across it and `fy` down its band reads the
/// sheet at `u = inkLeft + (fx + boxesBefore) * (inkRight - inkLeft)` and
/// `v = bandTop + fy * (bandBottom - bandTop)`. A cell's own glyph maps the
/// rule onto its own box; a neighbour's is the same box a whole box to one
/// side, so a descender leaning in from the next cell cuts this cell's rule
/// too. Only texels whose centres lie inside the slot are read: the sheet packs
/// glyphs side by side, and a texel past the slot's edge belongs to another.
struct UBIQUITTY_ESCAPABLE SkipInkSource {
    SkipInkSheet sheet = SkipInkSheet::Mono;
    /// The glyph's cell box across, in sheet texels.
    float inkLeft = 0.0f;
    float inkRight = 0.0f;
    /// The rule's band down, in sheet texels.
    float bandTop = 0.0f;
    float bandBottom = 0.0f;
    /// How many boxes the rule stands to the right of this one: 0 for the
    /// cell's own glyph, 1 for its left neighbour's, -1 for its right's.
    float boxesBefore = 0.0f;
    /// The glyph's whole slot, in sheet texels.
    float slotLeft = 0.0f;
    float slotTop = 0.0f;
    float slotRight = 0.0f;
    float slotBottom = 0.0f;
    /// The glyph's slot in the mono sheet, when the source reads one: a build
    /// that memoises its cuts learns once which rows of the slot hold ink, and
    /// passes over a glyph whose ink cannot touch the rule. -1 when unknown.
    std::int32_t slot = -1;
    bool isPresent = false; // bool-ok: an absent neighbour, a blank cell or a grid edge
};

/// The marks a patterned rule is drawn as, so a cut ends between marks or
/// leaves a mark long enough to read as one.
///
/// Mark `k` covers [origin + offset + k * pitch, ... + length). A cut ending
/// inside a mark that would leave less than `shortest` of it takes the whole
/// mark: a dot is never half drawn and a dash never left a sliver.
struct UBIQUITTY_ESCAPABLE SkipInkPattern {
    float pitch = 0.0f; // device pixels between marks; none for a solid rule
    float origin = 0.0f;
    float offset = 0.0f;
    float length = 0.0f;
    float shortest = 0.0f;
};

/// What a rule is tested against and how far it looks.
struct UBIQUITTY_ESCAPABLE SkipInkQuery {
    /// The cell's own raster and its left and right neighbours'.
    SkipInkSource own;
    SkipInkSource left;
    SkipInkSource right;
    /// The padding across, in sheet texels: how far beside touching ink the
    /// cut reaches.
    float reachAcross = 0.0f;
    /// How far above and below a pixel ink counts as touching it, in sheet
    /// texels: ``skipInkContactInDevicePixels``.
    float reachUpDown = 0.0f;
    /// Whether ink above the rule's top row may touch it. A grid rule sits
    /// below its letters with room to spare, and a stroke resting on it is
    /// touching it. A rich-text block's rule can hang a single row under the
    /// baseline, where every round letter's overshoot would rest on it.
    bool isTouchedFromAbove = true; // bool-ok: a property of where the rule hangs
    /// The shortest stretch of the rule worth leaving between two cuts, or
    /// between a cut and the line's end, in device pixels. A stretch shorter
    /// reads as a speck beside the letters rather than as the line, and is cut
    /// with them.
    float shortestPiece = 0.0f;
    /// Whether the line goes on past the rule's left and right edges into the
    /// next cell's rule. A stretch running across such an edge is judged by
    /// its whole length, the neighbours' ink included; one ending at an edge
    /// where the line stops ends at the line's end.
    bool continuesLeft = false; // bool-ok: whether the neighbour is underlined too
    bool continuesRight = false; // bool-ok: whether the neighbour is underlined too
    /// The marks of a dotted or dashed rule; a pitch of zero for any other.
    SkipInkPattern pattern;
    /// Whether the rule is cut across its full height: every row takes the
    /// cut of all of them, so a dot or a dash is cut square. A solid rule's cut
    /// follows a slanting stroke row by row, keeping its gap along the stroke;
    /// a dash cut that way beside a comma's tail is left a row tall.
    bool isCutAcrossFullHeight = false;
};

/// A rule's rectangle in device pixels and the pixels it covers.
///
/// A pixel is the rule's when its centre lies in the rectangle, half-open, the
/// GPU's top-left fill rule: so a rule a pixel tall lights one row.
struct UBIQUITTY_ESCAPABLE SkipInkRule {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] std::int32_t firstColumn() const { return static_cast<std::int32_t>(std::ceil(left - 0.5f)); }
    [[nodiscard]] std::int32_t endColumn() const { return static_cast<std::int32_t>(std::ceil(right - 0.5f)); }
    [[nodiscard]] std::int32_t firstRow() const { return static_cast<std::int32_t>(std::ceil(top - 0.5f)); }
    [[nodiscard]] std::int32_t endRow() const { return static_cast<std::int32_t>(std::ceil(bottom - 0.5f)); }
    [[nodiscard]] std::int32_t columns() const { return endColumn() > firstColumn() ? endColumn() - firstColumn() : 0; }
    [[nodiscard]] std::int32_t rows() const { return endRow() > firstRow() ? endRow() - firstRow() : 0; }
};

/// One rule a build tested against ink, for a harness or a diagnostic dump.
///
/// Written only into the optional trace a caller hands the builder: the rule,
/// what it was tested against, and how many pieces of it were drawn.
struct UBIQUITTY_ESCAPABLE SkipInkTrace {
    SkipInkRule rule;
    SkipInkQuery query;
    /// The first decoration of the rule's pieces, and how many there are; one
    /// for a rule the ink left whole, none for one it cut away entirely.
    std::uint32_t firstPiece = 0;
    std::uint32_t pieceCount = 0;
    bool isCut = false; // bool-ok: whether the ink cut the rule anywhere
};

/// Where ink cuts a rule: each of its pixel rows' cut spans, in device pixels.
///
/// Spans rather than pixels: a span ends wherever the padding beside a stroke's
/// edge puts it, inside a pixel as often as not, and the pixel there is drawn
/// in the part the cut leaves.
struct SkipInkCut {
    /// [left, right) pairs, sorted and disjoint within a row and inside the
    /// rule, row after row.
    std::vector<float> spans;
    /// Where each row's pairs end in ``spans``, one entry a rule row.
    std::vector<std::uint32_t> rowEnds;

    /// Returns one row's spans.
    ///
    /// - Parameter row: The row, counted from the rule's first.
    /// - Returns: The row's [left, right) pairs; empty past the last row.
    [[nodiscard]] std::span<const float> row(std::int32_t row) const
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rowEnds.size())
            return {};
        const std::uint32_t begin = row == 0 ? 0u : rowEnds[static_cast<std::size_t>(row) - 1];
        return std::span<const float>(spans).subspan(begin, rowEnds[static_cast<std::size_t>(row)] - begin);
    }
};

/// One span a row of a rule is cut across, before a row's spans are merged.
struct SkipInkRowSpan {
    std::int32_t row = 0;
    float left = 0.0f;
    float right = 0.0f;
};

/// The hash a skip-ink memo files a key under.
[[nodiscard]] inline std::uint64_t skipInkMemoHash(std::span<const std::uint32_t> words)
{
    // Two words a multiply.
    std::uint64_t hash = 0x9E3779B97F4A7C15ull;
    std::size_t index = 0;
    for (; index + 1 < words.size(); index += 2) {
        hash ^= static_cast<std::uint64_t>(words[index]) | (static_cast<std::uint64_t>(words[index + 1]) << 32);
        hash *= 0xBF58476D1CE4E5B9ull;
        hash ^= hash >> 31;
    }
    if (index < words.size()) {
        hash ^= words[index];
        hash *= 0xBF58476D1CE4E5B9ull;
        hash ^= hash >> 31;
    }
    return hash;
}

/// Values a build works out once and reads many times, filed under keys of
/// `KeyWords` words and forgotten together when the next build begins.
///
/// A glyph's cut depends on its raster and on the rule's shape, never on where
/// the rule stands, so a letter repeated across a screen under the same rule
/// is cut once a build rather than once for every cell that reads it; a cell
/// beside the same neighbours under the same rule is cut once too. A build
/// turns a memo on with ``begin()``, which forgets every earlier entry in
/// constant time: a slot one frame read may hold another glyph in the next.
/// Off, everything is worked out afresh.
template<std::size_t KeyWords, typename Element>
class SkipInkMemoTable {
public:
    using Key = std::array<std::uint32_t, KeyWords>;

    /// Starts a build's memo, forgetting every entry.
    void begin()
    {
        if (m_entries.empty())
            m_entries.resize(256);
        // A new generation empties every entry at once; on the rare wrap, the
        // entries are emptied by hand so none of a long-gone build's reads as
        // live.
        if (++m_generation == 0) {
            for (Entry& entry : m_entries)
                entry.generation = 0;
            m_generation = 1;
        }
        m_pool.clear();
        m_used = 0;
        m_isActive = true;
    }

    [[nodiscard]] bool isActive() const { return m_isActive; }

    /// Finds the values stored under a key.
    ///
    /// - Parameters:
    ///   - key: The key.
    ///   - values: Set to the stored values when found.
    /// - Returns: Whether the key was found.
    [[nodiscard]] bool find(const Key& key, std::span<const Element>& values) const
    {
        if (m_entries.empty())
            return false;
        const Entry& entry = m_entries[indexOf(key)];
        if (entry.generation != m_generation)
            return false;
        values = std::span<const Element>(m_pool).subspan(entry.first, entry.count);
        return true;
    }

    /// The values' storage: an entry's values are appended here, then filed
    /// with ``insert(_:first:)``.
    [[nodiscard]] std::vector<Element>& pool() { return m_pool; }

    /// Files the values appended to ``pool()`` from `first` on under a key.
    ///
    /// - Parameters:
    ///   - key: The key the values are found by for the rest of the build.
    ///   - first: The index in ``pool()`` of the first value to file.
    /// - Returns: The values filed.
    std::span<const Element> insert(const Key& key, std::size_t first)
    {
        if ((m_used + 1) * 2 > m_entries.size())
            grow();
        Entry& entry = m_entries[indexOf(key)];
        entry.key = key;
        entry.first = static_cast<std::uint32_t>(first);
        entry.count = static_cast<std::uint32_t>(m_pool.size() - first);
        entry.generation = m_generation;
        ++m_used;
        return std::span<const Element>(m_pool).subspan(entry.first, entry.count);
    }

private:
    /// One slot of the table: a key and where its values lie in the pool.
    struct Entry {
        Key key {};
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        /// The build the entry belongs to; any other build's is empty.
        std::uint32_t generation = 0;
    };

    [[nodiscard]] std::size_t indexOf(const Key& key) const
    {
        const std::size_t mask = m_entries.size() - 1;
        std::size_t index = static_cast<std::size_t>(skipInkMemoHash(key)) & mask;
        while (m_entries[index].generation == m_generation && m_entries[index].key != key)
            index = (index + 1) & mask;
        return index;
    }

    /// Doubles the table, at least to 256 slots, and refiles this build's
    /// entries; another build's are dropped, being empty already.
    void grow()
    {
        std::vector<Entry> previous = std::move(m_entries);
        m_entries.assign(std::max<std::size_t>(previous.size() * 2, 256), Entry {});
        for (const Entry& entry : previous) {
            if (entry.generation == m_generation)
                m_entries[indexOf(entry.key)] = entry;
        }
    }

    std::vector<Entry> m_entries;
    std::vector<Element> m_pool;
    std::size_t m_used = 0;
    std::uint32_t m_generation = 0;
    bool m_isActive = false;
};

/// A source's cut under a rule, by the source's mapping onto its sheet and the
/// rule's shape, relative to the rule's left edge and first pixel row.
using SkipInkSourceMemo = SkipInkMemoTable<19, SkipInkRowSpan>;

/// A rule's whole cut, by everything it depends on: its three sources, its
/// shape, where the line goes on and its pattern's phase, relative to the
/// rule's left edge. Filed as whether it was cut, each row's end, then the
/// spans.
using SkipInkCutMemo = SkipInkMemoTable<48, float>;

/// A glyph's coverage grown by a disc, over the sheet rows a soft cut reads:
/// worked out once a build for each glyph, slot rect and reach. Filed as its
/// first column, first row, columns and rows, then the coverage row by row.
using SkipInkGrownMemo = SkipInkMemoTable<8, float>;

/// Working memory the cut reuses across rules, so a frame allocates once.
struct SkipInkScratch {
    /// Starts a build that memoises its cuts (SkipInkMemoTable).
    void beginBuild()
    {
        memo.begin();
        cutMemo.begin();
        grownMemo.begin();
        if (++buildGeneration == 0) {
            for (SlotInk& ink : slotInk)
                ink.generation = 0;
            buildGeneration = 1;
        }
    }

    /// The rows of a mono slot holding ink at or over the threshold, as a
    /// build learns them: `bottom < top` is a blank slot.
    struct SlotInk {
        std::uint32_t generation = 0;
        std::int32_t top = 0;
        std::int32_t bottom = -1;
    };
    /// Each mono slot's inked rows, by slot, for the build of `buildGeneration`.
    std::vector<SlotInk> slotInk;
    /// The build the memos and `slotInk` belong to; zero outside a build.
    std::uint32_t buildGeneration = 0;

    SkipInkCut cut;
    /// The cuts the build's sources leave, when the build memoises them.
    SkipInkSourceMemo memo;
    /// The build's rules' whole cuts, when the build memoises them.
    SkipInkCutMemo cutMemo;
    /// The build's glyphs' grown coverage, for soft cuts.
    SkipInkGrownMemo grownMemo;
    /// A soft cut's share kept at each pixel, and along its curl.
    std::vector<float> keeps;
    std::vector<float> visibility;
    /// One glyph's grown coverage, when the memo is off.
    std::vector<float> grown;
    /// One source's cut, when the memo is off.
    std::vector<SkipInkRowSpan> sourceCut;
    /// The ink near a rule, as [left, right) edge pairs in texels, a pair a
    /// run, with the sheet row each run is in and the stroke it belongs to.
    std::vector<float> runEdges;
    std::vector<std::int32_t> runRows;
    std::vector<std::int32_t> strokeOf;
    /// One touching stroke's extent in each sheet row it is followed through.
    std::vector<SkipInkRowSpan> strokeRows;
    std::vector<SkipInkRowSpan> rowSpans;
    std::vector<float> kept;
    std::vector<float> previousKept;
};

/// Finds where ink cuts a rule.
///
/// - Parameters:
///   - planes: The sheets the sources read.
///   - query: The rasters and the reach.
///   - rule: The rule.
///   - cut: Receives each row's cut spans.
///   - scratch: Working memory.
/// - Returns: Whether any of the rule is cut.
bool markSkipInkCuts(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, SkipInkCut& cut, SkipInkScratch& scratch);

/// The rows of a mono slot holding ink at or over the coverage threshold, as
/// a build learns them, once a slot. A cell whose glyphs' ink stays clear of
/// the rule's rows cannot be cut, so it is drawn whole without building its
/// query.
///
/// - Parameters:
///   - plane: The mono sheet.
///   - slot: The slot.
///   - slotLeft: The slot's rect, in sheet texels.
///   - slotTop: Its top.
///   - slotRight: Its right.
///   - slotBottom: Its bottom.
///   - scratch: The build's working memory.
///   - top: Set to the first inked row.
///   - bottom: Set to the last; less than `top` for a blank slot.
/// - Returns: Whether the rows are known: false outside a build.
bool skipInkSlotInkRows(const SkipInkPlane& plane, std::int32_t slot, float slotLeft, float slotTop, float slotRight, float slotBottom, SkipInkScratch& scratch,
    std::int32_t& top, std::int32_t& bottom);

/// A stretch of a rule's pixel row a soft cut draws at a share of the rule's
/// opacity.
struct SkipInkKeep {
    /// The first row, counted from the rule's first.
    std::int32_t row = 0;
    /// How many rows the stretch runs down: rows that keep the same are one.
    std::int32_t rows = 1;
    float left = 0.0f;
    float right = 0.0f;
    /// The share of the rule's opacity kept, above zero and up to one.
    float keep = 1.0f;
};

/// The curl a soft cut is judged along.
struct SkipInkCurl {
    /// Where the curl's arcs start: its band's left edge, in device pixels.
    float origin = 0.0f;
    /// Device pixels from one crest to the next.
    float wavelength = 0.0f;
    /// The stroke's width.
    float thickness = 0.0f;
};

/// Works out a curly rule's soft cut: each pixel keeps one less the coverage
/// of the glyphs beside it, their ink grown by a disc `query.reachAcross`
/// texels across in every direction and read smoothly at the pixel's centre.
///
/// A wave cut hard falls apart into ticks; faded where the ink is, it keeps its
/// rhythm and the letter stays clean. Every glyph the grown ink reaches the
/// curl from cuts it, an underscore resting on the line as much as a
/// descender through it. A stretch of the curl's stroke the cut leaves less
/// than one and a half stroke widths of, between two cut places or a cut place
/// and the line's own end, reads as a speck and goes: judged two pixels past
/// each edge the line goes on across, so the cells either side of a speck
/// agree on it.
///
/// - Parameters:
///   - planes: The sheets the sources read.
///   - query: The rasters, the reach, and where the line goes on.
///   - rule: The rule: the curl's band.
///   - curl: The curl.
///   - keeps: Where the stretches drawn are written: whole stretches kept
///     entire, the shares kept beside the ink in steps of a 64th, and rows
///     keeping the same as one. A pixel the curl's stroke does not pass
///     through draws nothing, so it joins whichever stretch is beside it.
///   - scratch: Working memory, and the build's memo when it is on.
/// - Returns: Whether the rule is cut anywhere; when not, `keeps` is empty and
///   the rule is drawn whole.
bool markSkipInkMask(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, const SkipInkCurl& curl, std::vector<SkipInkKeep>& keeps,
    SkipInkScratch& scratch);

/// Returns how much of a stretch of a row the row's cut spans cover.
///
/// - Parameters:
///   - spans: The row's spans, as ``SkipInkCut/row(_:)`` returns them.
///   - left: The stretch's left edge, device pixels.
///   - right: Its right edge.
/// - Returns: The covered length divided by the stretch's, from 0 to 1.
[[nodiscard]] float skipInkCutCoverage(std::span<const float> spans, float left, float right);

/// A piece's ends that ink cut, as ``DecorationInstance/cutEnds`` states them.
inline constexpr std::uint32_t skipInkCutLeftEnd = 1u;
inline constexpr std::uint32_t skipInkCutRightEnd = 2u;

/// Calls `emit` with each rectangle of a rule that stays drawn.
///
/// Rows that keep the same spans share one rectangle, so an underline cut
/// straight through by a descender is two pieces whatever its weight. A
/// piece's left and right are where the rule or the cut ends; a kept stretch
/// narrower than a 256th of a pixel is no piece.
///
/// - Parameters:
///   - rule: The rule.
///   - cut: The rule's cut, as ``markSkipInkCuts`` leaves it.
///   - scratch: Working memory.
///   - emit: Called as `emit(left, top, right, bottom, cutEnds)` in device
///     pixels, `cutEnds` holding ``skipInkCutLeftEnd`` where the piece's left
///     is a cut and ``skipInkCutRightEnd`` where its right is.
template<typename Emit>
void forEachKeptPiece(const SkipInkRule& rule, const SkipInkCut& cut, SkipInkScratch& scratch, Emit&& emit)
{
    const std::int32_t rows = rule.rows();
    if (rows <= 0 || !(rule.right > rule.left))
        return;
    constexpr float narrowest = 1.0f / 256.0f;
    const std::int32_t firstRow = rule.firstRow();
    const auto edgeY = [&](std::int32_t row) {
        if (row <= firstRow)
            return rule.top;
        if (row >= firstRow + rows)
            return rule.bottom;
        return static_cast<float>(row);
    };
    // Each row's kept stretches as [left, right) pairs; a row that keeps the
    // same as the row above extends that row's pieces downward.
    std::vector<float>& kept = scratch.kept;
    std::vector<float>& previous = scratch.previousKept;
    previous.clear();
    std::int32_t pieceTop = firstRow;
    const auto flush = [&](std::int32_t endRow) {
        for (std::size_t index = 0; index + 1 < previous.size(); index += 2) {
            const std::uint32_t cutEnds = (previous[index] > rule.left ? skipInkCutLeftEnd : 0u)
                | (previous[index + 1] < rule.right ? skipInkCutRightEnd : 0u);
            emit(previous[index], edgeY(pieceTop), previous[index + 1], edgeY(endRow), cutEnds);
        }
    };
    for (std::int32_t row = 0; row < rows; ++row) {
        kept.clear();
        float from = rule.left;
        const std::span<const float> spans = cut.row(row);
        for (std::size_t index = 0; index + 1 < spans.size(); index += 2) {
            if (spans[index] - from >= narrowest) {
                kept.push_back(from);
                kept.push_back(spans[index]);
            }
            from = std::max(from, spans[index + 1]);
        }
        if (rule.right - from >= narrowest) {
            kept.push_back(from);
            kept.push_back(rule.right);
        }
        if (row > 0 && kept == previous)
            continue;
        if (row > 0)
            flush(firstRow + row);
        previous.swap(kept);
        pieceTop = firstRow + row;
    }
    flush(firstRow + rows);
}

} // namespace Ubiquitty::Rendering
