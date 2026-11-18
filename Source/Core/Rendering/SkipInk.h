// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

    std::vector<std::uint8_t> m_coverage;
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

/// Working memory the cut reuses across rules, so a frame allocates once.
struct SkipInkScratch {
    SkipInkCut cut;
    /// The strongest coverage each texel column has in the rows touching a
    /// rule row.
    std::vector<std::uint8_t> strongest;
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
