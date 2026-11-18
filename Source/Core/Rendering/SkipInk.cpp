// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/SkipInk.h"

#include "Core/Rendering/RuleGeometry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>

namespace Ubiquitty::Rendering {

namespace {

/// How far a distance must fall inside a reach to count as within it, in
/// texels. A reach that meets a pixel centre exactly is decided by the float
/// arithmetic that maps the pixel onto the sheet, which lands a hair to either
/// side, so without it a stroke's two edges could be cleared differently by
/// rounding alone. Far below anything a raster can resolve.
constexpr float reachTolerance = 1.0f / 1024.0f;

/// Whether a plane holds anything a rule can be read against.
bool isReadable(const SkipInkPlane& plane)
{
    return plane.coverage && plane.width > 0 && plane.height > 0 && plane.stride >= plane.width;
}

/// The texel a coordinate falls in, saturated so that no finite or infinite
/// coordinate overflows the narrowing.
std::int32_t texelOf(float coordinate)
{
    if (!(coordinate > -1.0e9f))
        return std::numeric_limits<std::int32_t>::min() / 2;
    if (!(coordinate < 1.0e9f))
        return std::numeric_limits<std::int32_t>::max() / 2;
    return static_cast<std::int32_t>(std::floor(coordinate));
}

/// The first texel whose centre lies within `reach` of a coordinate.
std::int32_t firstTexelWithin(float coordinate, float reach)
{
    return texelOf(std::ceil(coordinate - reach - 0.5f - reachTolerance));
}

/// The last texel whose centre lies within `reach` of a coordinate.
std::int32_t lastTexelWithin(float coordinate, float reach)
{
    return texelOf(std::floor(coordinate + reach - 0.5f + reachTolerance));
}

} // namespace

SkipInkRecord::SkipInkRecord(std::int32_t width, std::int32_t height)
    : m_width(std::max(width, 0))
    , m_height(std::max(height, 0))
{
    const std::size_t size = static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);
    if (size > 0)
        m_coverage.reset(static_cast<std::uint8_t*>(std::calloc(size, 1)));
    // A sheet whose coverage could not be had records nothing and reads as no
    // ink, rather than writing through a null pointer.
    if (!m_coverage) {
        m_width = 0;
        m_height = 0;
    }
}

bool SkipInkRecord::clip(std::int32_t& x, std::int32_t& y, std::int32_t& width, std::int32_t& height, std::int32_t& skipColumns, std::int32_t& skipRows) const
{
    skipColumns = x < 0 ? -x : 0;
    skipRows = y < 0 ? -y : 0;
    x += skipColumns;
    y += skipRows;
    width -= skipColumns;
    height -= skipRows;
    width = std::min(width, m_width - x);
    height = std::min(height, m_height - y);
    return width > 0 && height > 0;
}

void SkipInkRecord::record(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, const void* bytes, std::size_t bytesPerRow,
    std::int32_t bytesPerTexel, std::int32_t coverageChannel)
{
    if (!bytes || bytesPerTexel <= 0 || coverageChannel < 0 || coverageChannel >= bytesPerTexel)
        return;
    std::int32_t skipColumns = 0;
    std::int32_t skipRows = 0;
    if (!clip(x, y, width, height, skipColumns, skipRows))
        return;
    const auto* texels = static_cast<const std::uint8_t*>(bytes);
    for (std::int32_t row = 0; row < height; ++row) {
        const std::uint8_t* source = texels + static_cast<std::size_t>(row + skipRows) * bytesPerRow
            + static_cast<std::size_t>(skipColumns) * static_cast<std::size_t>(bytesPerTexel) + static_cast<std::size_t>(coverageChannel);
        std::uint8_t* line = m_coverage.get() + static_cast<std::size_t>(y + row) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        if (bytesPerTexel == 1) {
            std::copy(source, source + width, line);
            continue;
        }
        for (std::int32_t column = 0; column < width; ++column)
            line[column] = source[static_cast<std::size_t>(column) * static_cast<std::size_t>(bytesPerTexel)];
    }
}

void SkipInkRecord::clear(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    std::int32_t skipColumns = 0;
    std::int32_t skipRows = 0;
    if (!clip(x, y, width, height, skipColumns, skipRows))
        return;
    for (std::int32_t row = 0; row < height; ++row) {
        std::uint8_t* line = m_coverage.get() + static_cast<std::size_t>(y + row) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        std::fill(line, line + width, std::uint8_t { 0 });
    }
}

SkipInkPlane skipInkPlaneOf(const SkipInkRecord* record)
{
    if (!record)
        return {};
    const std::size_t size = static_cast<std::size_t>(record->m_width) * static_cast<std::size_t>(record->m_height);
    return skipInkCoveragePlane(std::span<const std::uint8_t>(record->m_coverage.get(), record->m_coverage ? size : 0), record->m_width, record->m_height);
}

namespace {

/// Gives every row of a rule the cut of all of them: the union of the rows'
/// spans, in each row.
///
/// - Parameters:
///   - spans: Every row's cut spans, replaced here by the union in each row.
///   - rows: How many rows the rule has.
///   - merged: Scratch for the union.
void cutEveryRowAsOne(std::vector<SkipInkRowSpan>& spans, std::int32_t rows, std::vector<float>& merged)
{
    std::sort(spans.begin(), spans.end(), [](const SkipInkRowSpan& a, const SkipInkRowSpan& b) { return a.left < b.left; });
    merged.clear();
    for (const SkipInkRowSpan& span : spans) {
        if (!merged.empty() && span.left <= merged.back())
            merged.back() = std::max(merged.back(), span.right);
        else {
            merged.push_back(span.left);
            merged.push_back(span.right);
        }
    }
    spans.clear();
    for (std::int32_t row = 0; row < rows; ++row) {
        for (std::size_t index = 0; index + 1 < merged.size(); index += 2)
            spans.push_back({ row, merged[index], merged[index + 1] });
    }
}

} // namespace

namespace {

/// A rule's shape as a source's cut depends on it: its size, its top against
/// the pixel rows, the stretch the cut is worked out over and the reach, all
/// relative to the rule's left edge and first pixel row. The same glyph under
/// a rule of the same shape is cut the same wherever the rule stands.
struct RuleShape {
    float width = 0.0f;
    float height = 0.0f;
    /// The rule's top, less its first pixel row.
    float top = 0.0f;
    std::int32_t rows = 0;
    /// The stretch the cut is worked out over, from the rule's left edge.
    float workLeft = 0.0f;
    float workRight = 0.0f;
    float reachAcross = 0.0f;
    float reachUpDown = 0.0f;
    bool isTouchedFromAbove = true;
};

/// The memo key of a source's cut under a rule of a shape.
SkipInkSourceMemo::Key sourceKeyOf(const SkipInkSource& source, const RuleShape& shape)
{
    SkipInkSourceMemo::Key key {};
    std::size_t at = 0;
    const auto put = [&](float value) { key[at++] = std::bit_cast<std::uint32_t>(value); };
    key[at++] = static_cast<std::uint32_t>(source.sheet);
    put(source.inkLeft);
    put(source.inkRight);
    put(source.bandTop);
    put(source.bandBottom);
    put(source.boxesBefore);
    put(source.slotLeft);
    put(source.slotTop);
    put(source.slotRight);
    put(source.slotBottom);
    put(shape.width);
    put(shape.height);
    put(shape.top);
    key[at++] = static_cast<std::uint32_t>(shape.rows);
    put(shape.workLeft);
    put(shape.workRight);
    put(shape.reachAcross);
    put(shape.reachUpDown);
    key[at++] = shape.isTouchedFromAbove ? 1u : 0u;
    return key;
}

/// Appends the cut one source's ink makes in each row of a rule of a shape,
/// relative to the rule's left edge and not held to the stretch worked over.
///
/// - Parameters:
///   - plane: The source's sheet.
///   - source: The source.
///   - shape: The rule's shape.
///   - scratch: Working memory.
///   - out: Where the row spans are appended.
void cutBesideSource(const SkipInkPlane& plane, const SkipInkSource& source, const RuleShape& shape, SkipInkScratch& scratch, std::vector<SkipInkRowSpan>& out)
{
    const float width = shape.width;
    const float height = shape.height;
    const std::int32_t rows = shape.rows;
    const float reachAcross = shape.reachAcross;
    const float reachUpDown = shape.reachUpDown;
    const float across = source.inkRight - source.inkLeft;
    if (across == 0.0f || !std::isfinite(across))
        return;
    // Only texels whose centres lie inside the slot, and inside the plane.
    const std::int32_t slotFirstColumn = std::max(texelOf(source.slotLeft + 0.5f), 0);
    const std::int32_t slotLastColumn = std::min(texelOf(source.slotRight - 0.5f), plane.width - 1);
    const std::int32_t slotFirstRow = std::max(texelOf(source.slotTop + 0.5f), 0);
    const std::int32_t slotLastRow = std::min(texelOf(source.slotBottom - 0.5f), plane.height - 1);
    if (slotLastColumn < slotFirstColumn || slotLastRow < slotFirstRow)
        return;
    // A point of the rule across and the sheet's texel coordinate under
    // it, both ways.
    const auto uAt = [&](float x) { return source.inkLeft + (x / width + source.boxesBefore) * across; };
    const auto xAt = [&](float u) { return ((u - source.inkLeft) / across - source.boxesBefore) * width; };
    // The texels this rule can reach in the source, across, with two to
    // spare each side: an edge is placed from the texel it lies in and the
    // one beyond, and a stroke whose edge is in reach may start past it.
    const float uLeft = uAt(shape.workLeft);
    const float uRight = uAt(shape.workRight);
    const std::int32_t scanFirst = std::max(texelOf(std::min(uLeft, uRight) - reachAcross) - 2, slotFirstColumn);
    const std::int32_t scanLast = std::min(texelOf(std::max(uLeft, uRight) + reachAcross) + 2, slotLastColumn);
    if (scanLast < scanFirst)
        return;
    const std::size_t span = static_cast<std::size_t>(scanLast - scanFirst + 1);
    // The rule's top row in the source: ink above it touches a rule only
    // when the query allows it.
    const float down = source.bandBottom - source.bandTop;
    const std::int32_t ruleTopTexel = texelOf(source.bandTop + (height > 0.0f ? (0.5f - shape.top) / height : 0.0f) * down);
    std::vector<float>& runEdges = scratch.runEdges;
    std::vector<std::int32_t>& runRows = scratch.runRows;
    std::vector<std::int32_t>& strokeOf = scratch.strokeOf;
    // Appends one row of the sheet's ink as runs of texels at half coverage
    // or more. A run and the partial texel either side of it are one stroke's
    // coverage, and a stroke box-filtered into texels keeps its width as the
    // coverage's sum and its centre as the coverage's centre, at whatever
    // phase it was drawn: so its edges are those, inside a texel as often as
    // not. The scan stays inside the slot and the plane, so the row is read
    // straight from the sheet.
    const auto appendRow = [&](std::int32_t sheetRow) {
        const std::uint8_t* texels = plane.coverage + static_cast<std::size_t>(sheetRow) * static_cast<std::size_t>(plane.stride) + static_cast<std::size_t>(scanFirst);
        for (std::size_t index = 0; index < span;) {
            if (texels[index] < skipInkCoverageThreshold) {
                ++index;
                continue;
            }
            const std::size_t begin = index;
            while (index < span && texels[index] >= skipInkCoverageThreshold)
                ++index;
            const std::size_t from = begin > 0 ? begin - 1 : begin;
            const std::size_t through = index < span ? index : index - 1;
            float coverage = 0.0f;
            float moment = 0.0f;
            for (std::size_t texel = from; texel <= through; ++texel) {
                const float fraction = static_cast<float>(texels[texel]) / 255.0f;
                coverage += fraction;
                moment += fraction * (static_cast<float>(scanFirst) + static_cast<float>(texel) + 0.5f);
            }
            const float centre = moment / coverage;
            runEdges.push_back(centre - coverage * 0.5f);
            runEdges.push_back(centre + coverage * 0.5f);
            runRows.push_back(sheetRow);
        }
    };
    // The sheet row under each of the rule's pixel rows.
    const auto vOf = [&](std::int32_t row) {
        return source.bandTop + (height > 0.0f ? (static_cast<float>(row) + 0.5f - shape.top) / height : 0.0f) * down;
    };
    // The rows ink must reach to touch the rule, and the rows a touching
    // stroke is followed through: the padding up and down, never less than
    // touching. Ink above the rule's top touches it only when the query
    // allows it.
    const float approach = std::max(reachAcross, reachUpDown);
    // A device pixel row's height in the sheet's texels.
    const float rowHeight = height > 0.0f ? std::fabs(down) / height : 1.0f;
    const float vTop = vOf(0);
    const float vBottom = vOf(rows - 1);
    const std::int32_t contactTop = std::max(shape.isTouchedFromAbove ? firstTexelWithin(vTop, reachUpDown) : ruleTopTexel, slotFirstRow);
    const std::int32_t contactBottom = std::min(lastTexelWithin(vBottom, reachUpDown), slotLastRow);
    const std::int32_t windowTop = std::max(shape.isTouchedFromAbove ? firstTexelWithin(vTop, approach) : ruleTopTexel, slotFirstRow);
    const std::int32_t windowBottom = std::min(lastTexelWithin(vBottom, approach), slotLastRow);
    if (contactBottom < contactTop)
        return;
    runEdges.clear();
    runRows.clear();
    for (std::int32_t sheetRow = windowTop; sheetRow <= windowBottom; ++sheetRow)
        appendRow(sheetRow);
    const std::size_t runCount = runRows.size();
    const auto leftOf = [&](std::size_t run) { return runEdges[run * 2]; };
    const auto rightOf = [&](std::size_t run) { return runEdges[run * 2 + 1]; };
    const auto isTouching = [&](std::size_t run) { return runRows[run] >= contactTop && runRows[run] <= contactBottom; };
    // The strokes touching the rule: runs in the touching rows, joined
    // where a run in one row meets a run in the next, diagonally included.
    strokeOf.assign(runCount, -1);
    const auto rootOf = [&](std::size_t run) {
        std::size_t root = run;
        while (strokeOf[root] != static_cast<std::int32_t>(root))
            root = static_cast<std::size_t>(strokeOf[root]);
        return root;
    };
    for (std::size_t run = 0; run < runCount; ++run) {
        if (isTouching(run))
            strokeOf[run] = static_cast<std::int32_t>(run);
    }
    for (std::size_t run = 0; run < runCount; ++run) {
        if (!isTouching(run))
            continue;
        for (std::size_t other = run + 1; other < runCount; ++other) {
            if (!isTouching(other) || runRows[other] != runRows[run] + 1)
                continue;
            if (leftOf(other) < rightOf(run) + 0.5f && leftOf(run) < rightOf(other) + 0.5f)
                strokeOf[rootOf(other)] = static_cast<std::int32_t>(rootOf(run));
        }
    }
    // Follows a stroke away from the rule, a row at a time, through the
    // ink that continues it: a run in the next row meeting the stroke,
    // taken no more than a texel wider each side than the row before, so a
    // slant or a curl is followed and a bowl the stroke joins is not.
    std::vector<SkipInkRowSpan>& strokeRows = scratch.strokeRows;
    const auto follow = [&](float left, float right, std::int32_t fromRow, std::int32_t step, std::int32_t lastRow) {
        for (std::int32_t sheetRow = fromRow + step; step < 0 ? sheetRow >= lastRow : sheetRow <= lastRow; sheetRow += step) {
            const float reachLeft = left - 1.0f;
            const float reachRight = right + 1.0f;
            float nextLeft = std::numeric_limits<float>::max();
            float nextRight = std::numeric_limits<float>::lowest();
            for (std::size_t run = 0; run < runCount; ++run) {
                if (runRows[run] != sheetRow || !(leftOf(run) < reachRight && rightOf(run) > reachLeft))
                    continue;
                nextLeft = std::min(nextLeft, std::max(leftOf(run), reachLeft));
                nextRight = std::max(nextRight, std::min(rightOf(run), reachRight));
            }
            if (!(nextRight > nextLeft))
                return;
            left = nextLeft;
            right = nextRight;
            strokeRows.push_back({ sheetRow, left, right });
        }
    };
    for (std::size_t root = 0; root < runCount; ++root) {
        if (!isTouching(root) || rootOf(root) != root)
            continue;
        // The stroke where it touches: its extent in each of its rows
        // there, then followed away from the rule for as far as the
        // padding, up and down.
        strokeRows.clear();
        std::int32_t topRow = std::numeric_limits<std::int32_t>::max();
        std::int32_t bottomRow = std::numeric_limits<std::int32_t>::min();
        for (std::size_t run = 0; run < runCount; ++run) {
            if (!isTouching(run) || rootOf(run) != root)
                continue;
            topRow = std::min(topRow, runRows[run]);
            bottomRow = std::max(bottomRow, runRows[run]);
            strokeRows.push_back({ runRows[run], leftOf(run), rightOf(run) });
        }
        const auto extentIn = [&](std::int32_t sheetRow, float& left, float& right) {
            left = std::numeric_limits<float>::max();
            right = std::numeric_limits<float>::lowest();
            for (const SkipInkRowSpan& part : strokeRows) {
                if (part.row == sheetRow) {
                    left = std::min(left, part.left);
                    right = std::max(right, part.right);
                }
            }
        };
        float left = 0.0f;
        float right = 0.0f;
        extentIn(topRow, left, right);
        follow(left, right, topRow, -1, windowTop);
        extentIn(bottomRow, left, right);
        follow(left, right, bottomRow, 1, windowBottom);
        // The cut, a row of the rule at a time: everything within the
        // padding of the stroke, measured as a distance, edge to edge, in
        // both directions. A row the stroke crosses or rests against is
        // cleared the whole padding either side of it; a row of the stroke
        // farther off, above or below, clears less, as a circle of the
        // padding's radius around its ink does. So a slant or a curl that
        // leans over the cut is cleared as far from the line as a straight
        // stem is, the cut's ends follow the stroke, and a stroke resting
        // on the line bites into it as deep as the padding, its corners
        // rounded by the same circle.
        for (std::int32_t row = 0; row < rows; ++row) {
            const float v = vOf(row);
            float cutFrom = std::numeric_limits<float>::max();
            float cutTo = std::numeric_limits<float>::lowest();
            for (const SkipInkRowSpan& part : strokeRows) {
                // The white between this pixel row of the rule and that
                // texel row of the stroke: their centres apart, less half
                // of each. Ink in the row or resting against it is none
                // apart and is always cut beneath; ink the whole padding
                // away or more is clear of the row.
                const float apart = std::max(std::fabs(v - (static_cast<float>(part.row) + 0.5f)) - 0.5f - 0.5f * rowHeight, 0.0f);
                if (apart > 0.0f && apart >= reachAcross - reachTolerance)
                    continue;
                const float beside = std::sqrt(std::max(reachAcross * reachAcross - apart * apart, 0.0f));
                cutFrom = std::min(cutFrom, part.left - beside);
                cutTo = std::max(cutTo, part.right + beside);
            }
            if (!(cutTo > cutFrom))
                continue;
            float cutLeft = xAt(cutFrom);
            float cutRight = xAt(cutTo);
            if (cutLeft > cutRight)
                std::swap(cutLeft, cutRight);
            out.push_back({ row, cutLeft, cutRight });
        }
    }
}

} // namespace

namespace {

/// The shortest piece a query keeps, as a usable length.
float shortestPieceOf(const SkipInkQuery& query) { return std::isfinite(query.shortestPiece) ? std::max(query.shortestPiece, 0.0f) : 0.0f; }

/// A rule's shape under a query (RuleShape).
RuleShape shapeOf(const SkipInkQuery& query, const SkipInkRule& rule)
{
    const float width = rule.right - rule.left;
    const float shortestPiece = shortestPieceOf(query);
    return {
        .width = width,
        .height = rule.bottom - rule.top,
        .top = rule.top - static_cast<float>(rule.firstRow()),
        .rows = rule.rows(),
        .workLeft = query.continuesLeft ? -shortestPiece : 0.0f,
        .workRight = width + (query.continuesRight ? shortestPiece : 0.0f),
        .reachAcross = std::max(query.reachAcross, 0.0f),
        .reachUpDown = std::max(query.reachUpDown, 0.0f),
        .isTouchedFromAbove = query.isTouchedFromAbove,
    };
}

/// The memo key of a rule's whole cut: its three sources, its shape, where
/// the line goes on and its pattern, the pattern's phase taken from the rule's
/// left edge.
SkipInkCutMemo::Key cutKeyOf(const SkipInkQuery& query, const SkipInkRule& rule, const RuleShape& shape, std::uint32_t skipped)
{
    SkipInkCutMemo::Key key {};
    std::size_t at = 0;
    const auto put = [&](float value) { key[at++] = std::bit_cast<std::uint32_t>(value); };
    std::uint32_t sourceBit = 1u;
    for (const SkipInkSource* source : { &query.own, &query.left, &query.right }) {
        // A source passed over cuts nothing, as an absent one does.
        const bool isReading = source->isPresent && (skipped & sourceBit) == 0u;
        sourceBit <<= 1u;
        key[at++] = isReading ? 1u + static_cast<std::uint32_t>(source->sheet) : 0u;
        put(isReading ? source->inkLeft : 0.0f);
        put(isReading ? source->inkRight : 0.0f);
        put(isReading ? source->bandTop : 0.0f);
        put(isReading ? source->bandBottom : 0.0f);
        put(isReading ? source->boxesBefore : 0.0f);
        put(isReading ? source->slotLeft : 0.0f);
        put(isReading ? source->slotTop : 0.0f);
        put(isReading ? source->slotRight : 0.0f);
        put(isReading ? source->slotBottom : 0.0f);
    }
    put(shape.width);
    put(shape.height);
    put(shape.top);
    key[at++] = static_cast<std::uint32_t>(shape.rows);
    put(shape.workLeft);
    put(shape.workRight);
    put(shape.reachAcross);
    put(shape.reachUpDown);
    key[at++] = (shape.isTouchedFromAbove ? 1u : 0u) | (query.continuesLeft ? 2u : 0u) | (query.continuesRight ? 4u : 0u) | (query.isCutAcrossFullHeight ? 8u : 0u);
    const SkipInkPattern& pattern = query.pattern;
    const bool isPatterned = pattern.pitch > 0.0f && pattern.length > 0.0f;
    const float along = pattern.origin + pattern.offset - rule.left;
    const float phase = isPatterned ? along - std::floor(along / pattern.pitch) * pattern.pitch : 0.0f;
    put(isPatterned ? pattern.pitch : 0.0f);
    put(phase);
    put(isPatterned ? pattern.length : 0.0f);
    put(isPatterned ? pattern.shortest : 0.0f);
    put(shortestPieceOf(query));
    return key;
}

/// The rows of a source's mono slot holding ink at or over the threshold,
/// learned once a build.
///
/// - Returns: Whether they are known: false for a source with no mono slot,
///   or outside a build.
} // namespace

bool skipInkSlotInkRows(const SkipInkPlane& plane, std::int32_t slotIndex, float slotLeft, float slotTop, float slotRight, float slotBottom, SkipInkScratch& scratch,
    std::int32_t& top, std::int32_t& bottom)
{
    if (slotIndex < 0 || scratch.buildGeneration == 0 || !isReadable(plane))
        return false;
    const std::size_t slot = static_cast<std::size_t>(slotIndex);
    if (slot >= scratch.slotInk.size())
        scratch.slotInk.resize(slot + 1);
    SkipInkScratch::SlotInk& ink = scratch.slotInk[slot];
    if (ink.generation != scratch.buildGeneration) {
        const std::int32_t firstColumn = std::max(texelOf(slotLeft + 0.5f), 0);
        const std::int32_t lastColumn = std::min(texelOf(slotRight - 0.5f), plane.width - 1);
        const std::int32_t firstRow = std::max(texelOf(slotTop + 0.5f), 0);
        const std::int32_t lastRow = std::min(texelOf(slotBottom - 0.5f), plane.height - 1);
        const auto isInked = [&](std::int32_t row) {
            const std::uint8_t* texels = plane.coverage + static_cast<std::size_t>(row) * static_cast<std::size_t>(plane.stride);
            for (std::int32_t column = firstColumn; column <= lastColumn; ++column) {
                if (texels[column] >= skipInkCoverageThreshold)
                    return true;
            }
            return false;
        };
        ink.top = 0;
        ink.bottom = -1;
        if (lastColumn >= firstColumn) {
            std::int32_t row = lastRow;
            while (row >= firstRow && !isInked(row))
                --row;
            if (row >= firstRow) {
                ink.bottom = row;
                ink.top = firstRow;
                while (!isInked(ink.top))
                    ++ink.top;
            }
        }
        ink.generation = scratch.buildGeneration;
    }
    top = ink.top;
    bottom = ink.bottom;
    return true;
}

namespace {

/// The rows of a source's mono slot holding ink (skipInkSlotInkRows).
bool inkRowsOf(const SkipInkPlane& plane, const SkipInkSource& source, SkipInkScratch& scratch, std::int32_t& top, std::int32_t& bottom)
{
    if (source.sheet != SkipInkSheet::Mono)
        return false;
    return skipInkSlotInkRows(plane, source.slot, source.slotLeft, source.slotTop, source.slotRight, source.slotBottom, scratch, top, bottom);
}

/// Whether a source's ink reaches the rows where ink touches a rule of a
/// shape: a stroke the cut follows starts there, so a glyph whose ink stays
/// clear of them cannot cut the rule. True when its ink's rows are not known.
bool canTouch(const SkipInkPlane& plane, const SkipInkSource& source, const RuleShape& shape, SkipInkScratch& scratch)
{
    std::int32_t inkTop = 0;
    std::int32_t inkBottom = -1;
    if (!inkRowsOf(plane, source, scratch, inkTop, inkBottom))
        return true;
    if (inkBottom < inkTop || shape.rows <= 0)
        return false;
    const float down = source.bandBottom - source.bandTop;
    const float height = shape.height;
    const auto vOf = [&](std::int32_t row) {
        return source.bandTop + (height > 0.0f ? (static_cast<float>(row) + 0.5f - shape.top) / height : 0.0f) * down;
    };
    const std::int32_t ruleTopTexel = texelOf(source.bandTop + (height > 0.0f ? (0.5f - shape.top) / height : 0.0f) * down);
    const std::int32_t contactTop = shape.isTouchedFromAbove ? firstTexelWithin(vOf(0), shape.reachUpDown) : ruleTopTexel;
    const std::int32_t contactBottom = lastTexelWithin(vOf(shape.rows - 1), shape.reachUpDown);
    return inkBottom >= contactTop && inkTop <= contactBottom;
}

/// Works a rule's cut out afresh (markSkipInkCuts), passing over the sources
/// `skipped` names: bit 0 the rule's own glyph, 1 its left neighbour, 2 its
/// right.
bool cutRuleAfresh(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, SkipInkCut& cut, SkipInkScratch& scratch, std::uint32_t skipped)
{
    cut.spans.clear();
    cut.rowEnds.clear();
    const std::int32_t rows = rule.rows();
    const float width = rule.right - rule.left;
    if (rows <= 0 || !(width > 0.0f))
        return false;
    cut.rowEnds.assign(static_cast<std::size_t>(rows), 0u);
    // The stretch of line the cut is worked out over: the rule, and as far as
    // the shortest piece past each edge the line continues across, so a
    // stretch running into the next cell is judged by its whole length.
    const float shortestPiece = shortestPieceOf(query);
    const float workLeft = rule.left - (query.continuesLeft ? shortestPiece : 0.0f);
    const float workRight = rule.right + (query.continuesRight ? shortestPiece : 0.0f);
    const RuleShape shape = shapeOf(query, rule);
    std::vector<SkipInkRowSpan>& rowSpans = scratch.rowSpans;
    rowSpans.clear();
    std::uint32_t sourceBit = 1u;
    for (const SkipInkSource* sourceOf : { &query.own, &query.left, &query.right }) {
        const SkipInkSource& source = *sourceOf;
        const bool isSkipped = (skipped & sourceBit) != 0u;
        sourceBit <<= 1u;
        if (!source.isPresent || isSkipped)
            continue;
        const SkipInkPlane& plane = planes.plane(source.sheet);
        if (!isReadable(plane))
            continue;
        // The source's cut beside the rule, relative to the rule's left edge:
        // from the memo when the build has cut this glyph under a rule of
        // this shape already.
        std::span<const SkipInkRowSpan> relative;
        if (scratch.memo.isActive()) {
            const SkipInkSourceMemo::Key key = sourceKeyOf(source, shape);
            if (!scratch.memo.find(key, relative)) {
                std::vector<SkipInkRowSpan>& pool = scratch.memo.pool();
                const std::size_t first = pool.size();
                cutBesideSource(plane, source, shape, scratch, pool);
                relative = scratch.memo.insert(key, first);
            }
        } else {
            scratch.sourceCut.clear();
            cutBesideSource(plane, source, shape, scratch, scratch.sourceCut);
            relative = scratch.sourceCut;
        }
        for (const SkipInkRowSpan& span : relative) {
            const float cutLeft = std::max(rule.left + span.left, workLeft);
            const float cutRight = std::min(rule.left + span.right, workRight);
            if (cutRight > cutLeft)
                rowSpans.push_back({ span.row, cutLeft, cutRight });
        }
    }
    if (rowSpans.empty())
        return false;
    if (query.isCutAcrossFullHeight)
        cutEveryRowAsOne(rowSpans, rows, scratch.kept);
    // Each row's spans, sorted and merged where they meet.
    std::sort(rowSpans.begin(), rowSpans.end(), [](const SkipInkRowSpan& a, const SkipInkRowSpan& b) {
        return a.row != b.row ? a.row < b.row : a.left < b.left;
    });
    // Each row's spans, merged where they meet or leave less than the shortest
    // piece between them, then held to the rule.
    std::vector<float>& merged = scratch.kept;
    std::vector<SkipInkRowSpan>& pieces = scratch.strokeRows;
    pieces.clear();
    std::size_t next = 0;
    for (std::int32_t row = 0; row < rows; ++row) {
        merged.clear();
        while (next < rowSpans.size() && rowSpans[next].row == row) {
            const SkipInkRowSpan& span = rowSpans[next++];
            if (!merged.empty() && span.left - merged.back() < shortestPiece)
                merged.back() = std::max(merged.back(), span.right);
            else {
                merged.push_back(span.left);
                merged.push_back(span.right);
            }
        }
        // A patterned rule's cut ends between its marks, or leaves a mark it
        // ends in long enough to read: never half a dot or a sliver of dash.
        const SkipInkPattern& pattern = query.pattern;
        if (pattern.pitch > 0.0f && pattern.length > 0.0f && !merged.empty()) {
            const float first = pattern.origin + pattern.offset;
            for (std::size_t index = 0; index + 1 < merged.size(); index += 2) {
                const float startMark = first + std::floor((merged[index] - first) / pattern.pitch) * pattern.pitch;
                if (merged[index] > startMark && merged[index] < startMark + pattern.length && merged[index] - startMark < pattern.shortest)
                    merged[index] = startMark;
                const float endMark = first + std::floor((merged[index + 1] - first) / pattern.pitch) * pattern.pitch;
                if (merged[index + 1] > endMark && merged[index + 1] < endMark + pattern.length && endMark + pattern.length - merged[index + 1] < pattern.shortest)
                    merged[index + 1] = endMark + pattern.length;
            }
            // Spans a mark grew into one another are one span.
            std::size_t kept = 0;
            for (std::size_t index = 0; index + 1 < merged.size(); index += 2) {
                if (kept > 0 && merged[index] <= merged[kept - 1])
                    merged[kept - 1] = std::max(merged[kept - 1], merged[index + 1]);
                else {
                    merged[kept++] = merged[index];
                    merged[kept++] = merged[index + 1];
                }
            }
            merged.resize(kept);
        }
        // A stub between a cut and the line's own end is a speck too; where
        // the line goes on, the stretch past the edge is not seen whole.
        if (!merged.empty()) {
            if (!query.continuesLeft && merged.front() - rule.left < shortestPiece)
                merged.front() = rule.left;
            if (!query.continuesRight && rule.right - merged.back() < shortestPiece)
                merged.back() = rule.right;
        }
        for (std::size_t index = 0; index + 1 < merged.size(); index += 2)
            pieces.push_back({ row, merged[index], merged[index + 1] });
    }
    // What the cut leaves, judged as pieces of line rather than a row at a
    // time: stretches kept in rows beside one another and overlapping are one
    // piece, and a piece reads as line only where it is kept the line's whole
    // height for at least the shortest piece. A piece with no such stretch is
    // a sliver a row or two tall, a fragment of the line's edge, and goes in
    // every row; a notch, the line's top row cut where a stroke rests on it,
    // keeps its full-height line either side. A piece running past an edge the
    // line continues across is judged by the next cell, which sees it whole.
    std::vector<SkipInkRowSpan>& stretches = scratch.rowSpans;
    stretches.clear();
    {
        std::size_t cutIndex = 0;
        for (std::int32_t row = 0; row < rows; ++row) {
            float from = workLeft;
            while (cutIndex < pieces.size() && pieces[cutIndex].row == row) {
                if (pieces[cutIndex].left > from)
                    stretches.push_back({ row, from, pieces[cutIndex].left });
                from = std::max(from, pieces[cutIndex].right);
                ++cutIndex;
            }
            if (workRight > from)
                stretches.push_back({ row, from, workRight });
        }
    }
    std::vector<std::int32_t>& pieceOf = scratch.strokeOf;
    pieceOf.resize(stretches.size());
    for (std::size_t index = 0; index < stretches.size(); ++index)
        pieceOf[index] = static_cast<std::int32_t>(index);
    const auto pieceRoot = [&](std::size_t index) {
        while (pieceOf[index] != static_cast<std::int32_t>(index))
            index = static_cast<std::size_t>(pieceOf[index]);
        return index;
    };
    for (std::size_t index = 0; index < stretches.size(); ++index) {
        for (std::size_t other = index + 1; other < stretches.size() && stretches[other].row <= stretches[index].row + 1; ++other) {
            if (stretches[other].row == stretches[index].row + 1 && stretches[other].left < stretches[index].right && stretches[index].left < stretches[other].right)
                pieceOf[pieceRoot(other)] = static_cast<std::int32_t>(pieceRoot(index));
        }
    }
    std::vector<float>& whole = scratch.previousKept;
    for (std::size_t root = 0; root < stretches.size(); ++root) {
        if (pieceRoot(root) != root)
            continue;
        bool doesRunOn = false;
        for (std::size_t index = 0; index < stretches.size() && !doesRunOn; ++index) {
            if (pieceRoot(index) != root)
                continue;
            doesRunOn = (query.continuesLeft && stretches[index].left <= workLeft) || (query.continuesRight && stretches[index].right >= workRight);
        }
        // Where every row of the line is kept: the piece's stretches in the
        // first row, narrowed by each row after it.
        float longest = 0.0f;
        if (!doesRunOn) {
            whole.clear();
            for (std::size_t index = 0; index < stretches.size(); ++index) {
                if (stretches[index].row == 0 && pieceRoot(index) == root) {
                    whole.push_back(stretches[index].left);
                    whole.push_back(stretches[index].right);
                }
            }
            for (std::int32_t row = 1; row < rows && !whole.empty(); ++row) {
                std::vector<float>& narrowed = merged;
                narrowed.clear();
                for (std::size_t index = 0; index < stretches.size(); ++index) {
                    if (stretches[index].row != row || pieceRoot(index) != root)
                        continue;
                    for (std::size_t part = 0; part + 1 < whole.size(); part += 2) {
                        const float left = std::max(whole[part], stretches[index].left);
                        const float right = std::min(whole[part + 1], stretches[index].right);
                        if (right > left) {
                            narrowed.push_back(left);
                            narrowed.push_back(right);
                        }
                    }
                }
                whole.swap(narrowed);
            }
            for (std::size_t part = 0; part + 1 < whole.size(); part += 2)
                longest = std::max(longest, whole[part + 1] - whole[part]);
        }
        if (doesRunOn || longest >= shortestPiece)
            continue;
        for (std::size_t index = 0; index < stretches.size(); ++index) {
            if (pieceRoot(index) == root)
                pieces.push_back(stretches[index]);
        }
    }
    // The cut, each row's spans merged and held to the rule.
    std::sort(pieces.begin(), pieces.end(), [](const SkipInkRowSpan& a, const SkipInkRowSpan& b) {
        return a.row != b.row ? a.row < b.row : a.left < b.left;
    });
    bool isAnyCut = false;
    std::size_t at = 0;
    for (std::int32_t row = 0; row < rows; ++row) {
        const std::size_t rowBegin = cut.spans.size();
        for (; at < pieces.size() && pieces[at].row == row; ++at) {
            const float left = std::max(pieces[at].left, rule.left);
            const float right = std::min(pieces[at].right, rule.right);
            if (!(right > left))
                continue;
            if (cut.spans.size() > rowBegin && left <= cut.spans.back())
                cut.spans.back() = std::max(cut.spans.back(), right);
            else {
                cut.spans.push_back(left);
                cut.spans.push_back(right);
            }
            isAnyCut = true;
        }
        cut.rowEnds[static_cast<std::size_t>(row)] = static_cast<std::uint32_t>(cut.spans.size());
    }
    return isAnyCut;
}

} // namespace

bool markSkipInkCuts(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, SkipInkCut& cut, SkipInkScratch& scratch)
{
    const std::int32_t rows = rule.rows();
    if (!scratch.cutMemo.isActive() || rows <= 0 || !(rule.right > rule.left))
        return cutRuleAfresh(planes, query, rule, cut, scratch, 0u);
    // A glyph whose ink stays clear of the rows where ink touches the rule
    // cannot cut it, and is passed over; with none left, nothing is cut.
    const RuleShape shape = shapeOf(query, rule);
    std::uint32_t skipped = 0u;
    std::uint32_t sourceBit = 1u;
    for (const SkipInkSource* source : { &query.own, &query.left, &query.right }) {
        if (!source->isPresent)
            skipped |= sourceBit;
        else {
            const SkipInkPlane& plane = planes.plane(source->sheet);
            if (!isReadable(plane) || !canTouch(plane, *source, shape, scratch))
                skipped |= sourceBit;
        }
        sourceBit <<= 1u;
    }
    if (skipped == 7u) {
        cut.spans.clear();
        cut.rowEnds.assign(static_cast<std::size_t>(rows), 0u);
        return false;
    }
    // A rule beside the same glyphs under the same shape as one the build has
    // cut already is cut the same, relative to its left edge.
    const SkipInkCutMemo::Key key = cutKeyOf(query, rule, shape, skipped);
    std::span<const float> filed;
    if (scratch.cutMemo.find(key, filed)) {
        cut.rowEnds.resize(static_cast<std::size_t>(rows));
        for (std::int32_t row = 0; row < rows; ++row)
            cut.rowEnds[static_cast<std::size_t>(row)] = static_cast<std::uint32_t>(filed[1 + static_cast<std::size_t>(row)]);
        cut.spans.resize(filed.size() - 1 - static_cast<std::size_t>(rows));
        for (std::size_t index = 0; index < cut.spans.size(); ++index)
            cut.spans[index] = rule.left + filed[1 + static_cast<std::size_t>(rows) + index];
        return filed[0] != 0.0f;
    }
    const bool isCut = cutRuleAfresh(planes, query, rule, cut, scratch, skipped);
    std::vector<float>& pool = scratch.cutMemo.pool();
    const std::size_t first = pool.size();
    pool.push_back(isCut ? 1.0f : 0.0f);
    for (std::int32_t row = 0; row < rows; ++row)
        pool.push_back(static_cast<float>(row < static_cast<std::int32_t>(cut.rowEnds.size()) ? cut.rowEnds[static_cast<std::size_t>(row)] : 0u));
    for (const float edge : cut.spans)
        pool.push_back(edge - rule.left);
    scratch.cutMemo.insert(key, first);
    return isCut;
}

namespace {

/// A glyph's coverage grown by a disc, over a band of its slot's rows, as the
/// grown memo files it.
struct GrownCoverage {
    std::int32_t firstColumn = 0;
    std::int32_t firstRow = 0;
    std::int32_t columns = 0;
    std::int32_t rows = 0;
    const float* values = nullptr;

    [[nodiscard]] float at(std::int32_t column, std::int32_t row) const
    {
        column -= firstColumn;
        row -= firstRow;
        if (column < 0 || row < 0 || column >= columns || row >= rows)
            return 0.0f;
        return values[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
    }

    /// The coverage read smoothly at a point of the sheet, as a GPU's linear
    /// sampler reads it: texel centres at the half-integers.
    [[nodiscard]] float sample(float u, float v) const
    {
        const float x = std::floor(u - 0.5f);
        const float y = std::floor(v - 0.5f);
        const float acrossShare = u - 0.5f - x;
        const float downShare = v - 0.5f - y;
        const auto column = static_cast<std::int32_t>(x);
        const auto row = static_cast<std::int32_t>(y);
        const float upper = at(column, row) + (at(column + 1, row) - at(column, row)) * acrossShare;
        const float lower = at(column, row + 1) + (at(column + 1, row + 1) - at(column, row + 1)) * acrossShare;
        return upper + (lower - upper) * downShare;
    }
};

/// Grows a glyph's coverage by a disc `radius` texels across over the sheet
/// rows `firstRow` to `lastRow`, appending the grown rows to `out`.
///
/// The disc is taken a row of it at a time: each sheet row's widest coverage
/// within each half-width the disc has is worked out once, one half-width from
/// the last, and a grown texel takes, from each row of the disc, the widest
/// within that row's half-width. So a texel costs the disc's height and its
/// half-widths, not its area. Ink outside the slot is another glyph's and is
/// not read.
void growCoverage(const SkipInkPlane& plane, std::int32_t slotFirstColumn, std::int32_t slotLastColumn, std::int32_t slotFirstRow, std::int32_t slotLastRow,
    std::int32_t firstRow, std::int32_t lastRow, float radius, std::vector<float>& out, std::vector<float>& work)
{
    const std::int32_t columns = slotLastColumn - slotFirstColumn + 1;
    const std::int32_t reach = std::max(static_cast<std::int32_t>(std::lround(radius)), 0);
    const std::int32_t inFirst = std::max(firstRow - reach, slotFirstRow);
    const std::int32_t inLast = std::min(lastRow + reach, slotLastRow);
    const std::int32_t inRows = inLast - inFirst + 1;
    const std::size_t plaque = static_cast<std::size_t>(inRows) * static_cast<std::size_t>(columns);
    work.assign(static_cast<std::size_t>(reach + 1) * plaque, 0.0f);
    for (std::int32_t row = 0; row < inRows; ++row) {
        const std::uint8_t* texels = plane.coverage + static_cast<std::size_t>(inFirst + row) * static_cast<std::size_t>(plane.stride) + static_cast<std::size_t>(slotFirstColumn);
        float* widest = work.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(columns);
        for (std::int32_t column = 0; column < columns; ++column)
            widest[column] = static_cast<float>(texels[column]) / 255.0f;
    }
    for (std::int32_t halfWidth = 1; halfWidth <= reach; ++halfWidth) {
        const float* narrower = work.data() + static_cast<std::size_t>(halfWidth - 1) * plaque;
        const float* base = work.data();
        float* wider = work.data() + static_cast<std::size_t>(halfWidth) * plaque;
        for (std::int32_t row = 0; row < inRows; ++row) {
            const std::size_t at = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns);
            for (std::int32_t column = 0; column < columns; ++column) {
                float value = narrower[at + static_cast<std::size_t>(column)];
                if (column - halfWidth >= 0)
                    value = std::max(value, base[at + static_cast<std::size_t>(column - halfWidth)]);
                if (column + halfWidth < columns)
                    value = std::max(value, base[at + static_cast<std::size_t>(column + halfWidth)]);
                wider[at + static_cast<std::size_t>(column)] = value;
            }
        }
    }
    for (std::int32_t row = firstRow; row <= lastRow; ++row) {
        const std::size_t outAt = out.size();
        out.resize(outAt + static_cast<std::size_t>(columns), 0.0f);
        for (std::int32_t offset = -reach; offset <= reach; ++offset) {
            const std::int32_t inRow = row + offset - inFirst;
            const float across = radius * radius - static_cast<float>(offset * offset);
            if (inRow < 0 || inRow >= inRows || across < 0.0f)
                continue;
            const auto halfWidth = std::min(static_cast<std::int32_t>(std::lround(std::sqrt(across))), reach);
            const float* widest = work.data() + static_cast<std::size_t>(halfWidth) * plaque + static_cast<std::size_t>(inRow) * static_cast<std::size_t>(columns);
            for (std::int32_t column = 0; column < columns; ++column)
                out[outAt + static_cast<std::size_t>(column)] = std::max(out[outAt + static_cast<std::size_t>(column)], widest[column]);
        }
    }
}

} // namespace

bool markSkipInkMask(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, const SkipInkCurl& curl, std::vector<SkipInkKeep>& keeps,
    SkipInkScratch& scratch)
{
    keeps.clear();
    const std::int32_t rows = rule.rows();
    const float width = rule.right - rule.left;
    const float height = rule.bottom - rule.top;
    if (rows <= 0 || !(width > 0.0f) || !(height > 0.0f))
        return false;
    const std::int32_t firstRow = rule.firstRow();
    const float radius = std::max(query.reachAcross, 0.0f);
    // The pixels judged: the rule's, and two past each edge the line goes on
    // across, where a speck crossing the edge is seen whole.
    constexpr std::int32_t speckReach = 2;
    const auto firstColumn = static_cast<std::int32_t>(std::floor(rule.left));
    const auto lastColumn = static_cast<std::int32_t>(std::ceil(rule.right)) - 1;
    const std::int32_t scanFirst = firstColumn - (query.continuesLeft ? speckReach : 0);
    const std::int32_t scanLast = lastColumn + (query.continuesRight ? speckReach : 0);
    const std::int32_t scanColumns = scanLast - scanFirst + 1;
    if (scanColumns <= 0)
        return false;
    // Each source's grown coverage, from the memo when the build has grown
    // the glyph for these rows already.
    struct Reader {
        GrownCoverage grown;
        const SkipInkSource* source = nullptr;
    };
    std::array<Reader, 3> readers {};
    std::size_t readerCount = 0;
    // Where each reader's grown coverage starts in the memo's pool or the
    // local buffer: resolved once every source has grown, since growing the
    // next can move the storage.
    std::array<std::size_t, 3> grownFirsts {};
    const bool isMemoised = scratch.grownMemo.isActive();
    if (!isMemoised)
        scratch.grown.clear();
    for (const SkipInkSource* source : { &query.own, &query.left, &query.right }) {
        if (!source->isPresent)
            continue;
        const SkipInkPlane& plane = planes.plane(source->sheet);
        const float across = source->inkRight - source->inkLeft;
        if (!isReadable(plane) || across == 0.0f || !std::isfinite(across))
            continue;
        const std::int32_t slotFirstColumn = std::max(texelOf(source->slotLeft + 0.5f), 0);
        const std::int32_t slotLastColumn = std::min(texelOf(source->slotRight - 0.5f), plane.width - 1);
        const std::int32_t slotFirstRow = std::max(texelOf(source->slotTop + 0.5f), 0);
        const std::int32_t slotLastRow = std::min(texelOf(source->slotBottom - 0.5f), plane.height - 1);
        if (slotLastColumn < slotFirstColumn || slotLastRow < slotFirstRow)
            continue;
        // The sheet rows the rule's pixel centres read, and one beyond for
        // the smooth read.
        const float down = source->bandBottom - source->bandTop;
        const auto vOf = [&](float y) { return source->bandTop + (y - rule.top) / height * down; };
        const float vA = vOf(static_cast<float>(firstRow) + 0.5f);
        const float vB = vOf(static_cast<float>(firstRow + rows) - 0.5f);
        const std::int32_t readFirst = std::max(static_cast<std::int32_t>(std::floor(std::min(vA, vB) - 0.5f)), slotFirstRow);
        const std::int32_t readLast = std::min(static_cast<std::int32_t>(std::floor(std::max(vA, vB) - 0.5f)) + 1, slotLastRow);
        if (readLast < readFirst)
            continue;
        // A glyph whose ink stays farther than the reach from those rows
        // grows short of the rule.
        std::int32_t inkTop = 0;
        std::int32_t inkBottom = -1;
        if (inkRowsOf(plane, *source, scratch, inkTop, inkBottom)) {
            const auto reach = static_cast<std::int32_t>(std::ceil(radius));
            if (inkBottom < inkTop || inkBottom < readFirst - reach || inkTop > readLast + reach)
                continue;
        }
        if (isMemoised) {
            std::span<const float> filed;
            SkipInkGrownMemo::Key key {};
            key[0] = static_cast<std::uint32_t>(source->sheet);
            key[1] = static_cast<std::uint32_t>(slotFirstColumn);
            key[2] = static_cast<std::uint32_t>(slotLastColumn);
            key[3] = static_cast<std::uint32_t>(slotFirstRow);
            key[4] = static_cast<std::uint32_t>(slotLastRow);
            key[5] = static_cast<std::uint32_t>(readFirst);
            key[6] = static_cast<std::uint32_t>(readLast);
            key[7] = std::bit_cast<std::uint32_t>(radius);
            if (!scratch.grownMemo.find(key, filed)) {
                std::vector<float>& pool = scratch.grownMemo.pool();
                const std::size_t first = pool.size();
                pool.insert(pool.end(), { static_cast<float>(slotFirstColumn), static_cast<float>(readFirst), static_cast<float>(slotLastColumn - slotFirstColumn + 1), static_cast<float>(readLast - readFirst + 1) });
                growCoverage(plane, slotFirstColumn, slotLastColumn, slotFirstRow, slotLastRow, readFirst, readLast, radius, pool, scratch.kept);
                filed = scratch.grownMemo.insert(key, first);
            }
            grownFirsts[readerCount] = static_cast<std::size_t>(filed.data() - scratch.grownMemo.pool().data());
        } else {
            // Off the memo, each source grows into its own stretch of one
            // buffer.
            std::vector<float>& grown = scratch.grown;
            grownFirsts[readerCount] = grown.size();
            grown.insert(grown.end(), { static_cast<float>(slotFirstColumn), static_cast<float>(readFirst), static_cast<float>(slotLastColumn - slotFirstColumn + 1), static_cast<float>(readLast - readFirst + 1) });
            growCoverage(plane, slotFirstColumn, slotLastColumn, slotFirstRow, slotLastRow, readFirst, readLast, radius, grown, scratch.kept);
        }
        readers[readerCount++].source = source;
    }
    if (readerCount == 0)
        return false;
    {
        const float* storage = isMemoised ? scratch.grownMemo.pool().data() : scratch.grown.data();
        for (std::size_t index = 0; index < readerCount; ++index) {
            const float* header = storage + grownFirsts[index];
            readers[index].grown = { .firstColumn = static_cast<std::int32_t>(header[0]),
                .firstRow = static_cast<std::int32_t>(header[1]),
                .columns = static_cast<std::int32_t>(header[2]),
                .rows = static_cast<std::int32_t>(header[3]),
                .values = header + 4 };
        }
    }
    // The share each pixel keeps: one less the strongest grown coverage under
    // its centre.
    std::vector<float>& kept = scratch.keeps;
    kept.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(scanColumns), 1.0f);
    const auto keepAt = [&](std::int32_t row, std::int32_t column) -> float& {
        return kept[static_cast<std::size_t>(row) * static_cast<std::size_t>(scanColumns) + static_cast<std::size_t>(column - scanFirst)];
    };
    for (std::size_t index = 0; index < readerCount; ++index) {
        const SkipInkSource& source = *readers[index].source;
        const GrownCoverage& grown = readers[index].grown;
        const float across = source.inkRight - source.inkLeft;
        const float down = source.bandBottom - source.bandTop;
        for (std::int32_t row = 0; row < rows; ++row) {
            const float v = source.bandTop + (static_cast<float>(firstRow + row) + 0.5f - rule.top) / height * down;
            for (std::int32_t column = scanFirst; column <= scanLast; ++column) {
                const float u = source.inkLeft + ((static_cast<float>(column) + 0.5f - rule.left) / width + source.boxesBefore) * across;
                float& keep = keepAt(row, column);
                keep = std::min(keep, std::clamp(1.0f - grown.sample(u, v), 0.0f, 1.0f));
            }
        }
    }
    // A speck: a stretch of the curl's stroke the cut leaves less than one and
    // a half stroke widths of, between two cut places or a cut place and the
    // line's own end, judged along where the stroke is drawn.
    const CurlArcs arcs = curlArcs(curl.wavelength, height, curl.thickness);
    const float halfWeight = curl.thickness * 0.5f;
    std::vector<float>& visibility = scratch.visibility;
    visibility.assign(static_cast<std::size_t>(scanColumns), 0.0f);
    for (std::int32_t column = scanFirst; column <= scanLast; ++column) {
        const float strokeY = rule.top + curlCentreY(arcs, static_cast<float>(column) + 0.5f - curl.origin);
        float visible = 0.0f;
        for (std::int32_t row = 0; row < rows; ++row) {
            if (std::fabs(static_cast<float>(firstRow + row) + 0.5f - strokeY) <= halfWeight + 0.5f)
                visible = std::max(visible, keepAt(row, column));
        }
        visibility[static_cast<std::size_t>(column - scanFirst)] = visible;
    }
    // A stretch ends where less than half the stroke is left.
    constexpr float halfLeft = 0.5f;
    const float shortestVisible = 1.5f * curl.thickness;
    for (std::size_t index = 0; index < visibility.size();) {
        if (visibility[index] < halfLeft) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        float drawn = 0.0f;
        while (index < visibility.size() && visibility[index] >= halfLeft) {
            // Only the share of the pixel the line covers is drawn.
            const float column = static_cast<float>(scanFirst + static_cast<std::int32_t>(index));
            const float lineLeft = query.continuesLeft ? column : std::max(column, rule.left);
            const float lineRight = query.continuesRight ? column + 1.0f : std::min(column + 1.0f, rule.right);
            drawn += visibility[index++] * std::max(lineRight - lineLeft, 0.0f);
        }
        const bool isLeftBounded = begin > 0 || !query.continuesLeft;
        const bool isRightBounded = index < visibility.size() || !query.continuesRight;
        const bool isCutBeside = begin > 0 || index < visibility.size();
        if (drawn < shortestVisible && isLeftBounded && isRightBounded && isCutBeside) {
            for (std::int32_t row = 0; row < rows; ++row) {
                for (std::size_t column = begin; column < index; ++column)
                    keepAt(row, scanFirst + static_cast<std::int32_t>(column)) = 0.0f;
            }
        }
    }
    constexpr float clear = 0.004f;
    // The stretches drawn. A pixel the curl's stroke does not pass through,
    // its antialiasing included, draws nothing whatever share it keeps, so it
    // joins the stretch beside it; the others keep their share in steps of a
    // 64th, finer than the eye or an 8-bit blend tells apart. Rows keeping the
    // same are one stretch down.
    constexpr float steps = 64.0f;
    const float reach = halfWeight + 1.0f;
    const auto isDrawn = [&](std::int32_t row, std::int32_t column) {
        const float centre = static_cast<float>(firstRow + row) + 0.5f;
        float highest = std::numeric_limits<float>::max();
        float lowest = std::numeric_limits<float>::lowest();
        for (const float x : { 0.0f, 0.5f, 1.0f }) {
            const float strokeY = rule.top + curlCentreY(arcs, static_cast<float>(column) + x - curl.origin);
            highest = std::min(highest, strokeY);
            lowest = std::max(lowest, strokeY);
        }
        return centre >= highest - reach && centre <= lowest + reach;
    };
    std::vector<SkipInkRowSpan>& runs = scratch.strokeRows;
    std::vector<float>& runKeeps = scratch.previousKept;
    std::size_t previousBegin = 0;
    std::size_t previousEnd = 0;
    bool isAnyCut = false;
    for (std::int32_t row = 0; row < rows; ++row) {
        // This row's runs, as [left, right) and the share kept.
        const std::size_t rowBegin = keeps.size();
        runs.clear();
        runKeeps.clear();
        // Whether each run so far holds only pass-through pixels.
        std::vector<std::int32_t>& isPassThrough = scratch.strokeOf;
        isPassThrough.clear();
        for (std::int32_t column = firstColumn; column <= lastColumn; ++column) {
            const float left = std::max(static_cast<float>(column), rule.left);
            const float right = std::min(static_cast<float>(column + 1), rule.right);
            if (!(right > left))
                continue;
            const float raw = keepAt(row, column);
            if (raw < 0.999f)
                isAnyCut = true;
            const bool isWildcard = !isDrawn(row, column);
            const float keep = raw >= 0.999f ? 1.0f : std::round(raw * steps) / steps;
            if (!runs.empty() && (isWildcard || keep == runKeeps.back() || isPassThrough.back() != 0)) {
                runs.back().right = right;
                // A run of pass-through pixels takes the first drawn one's share.
                if (!isWildcard && isPassThrough.back() != 0) {
                    runKeeps.back() = keep;
                    isPassThrough.back() = 0;
                }
                continue;
            }
            runs.push_back({ row, left, right });
            runKeeps.push_back(isWildcard ? 1.0f : keep);
            isPassThrough.push_back(isWildcard ? 1 : 0);
        }
        // Runs a drawn pixel's share joined to the run before are one.
        std::size_t kept = 0;
        for (std::size_t index = 0; index < runs.size(); ++index) {
            if (kept > 0 && runKeeps[kept - 1] == runKeeps[index]) {
                runs[kept - 1].right = runs[index].right;
                continue;
            }
            runs[kept] = runs[index];
            runKeeps[kept] = runKeeps[index];
            ++kept;
        }
        runs.resize(kept);
        runKeeps.resize(kept);
        // The same runs as the row above extend its stretches down.
        bool isSameAsAbove = row > 0;
        if (isSameAsAbove) {
            std::size_t at = previousBegin;
            for (std::size_t index = 0; index < runs.size(); ++index) {
                if (runKeeps[index] <= clear)
                    continue;
                if (at >= previousEnd || keeps[at].left != runs[index].left || keeps[at].right != runs[index].right || keeps[at].keep != runKeeps[index]) {
                    isSameAsAbove = false;
                    break;
                }
                ++at;
            }
            isSameAsAbove = isSameAsAbove && at == previousEnd;
        }
        if (isSameAsAbove) {
            for (std::size_t at = previousBegin; at < previousEnd; ++at)
                ++keeps[at].rows;
            continue;
        }
        for (std::size_t index = 0; index < runs.size(); ++index) {
            if (runKeeps[index] > clear)
                keeps.push_back({ .row = row, .rows = 1, .left = runs[index].left, .right = runs[index].right, .keep = runKeeps[index] });
        }
        previousBegin = rowBegin;
        previousEnd = keeps.size();
    }
    if (!isAnyCut)
        keeps.clear();
    return isAnyCut;
}

float skipInkCutCoverage(std::span<const float> spans, float left, float right)
{
    if (!(right > left))
        return 0.0f;
    float covered = 0.0f;
    for (std::size_t index = 0; index + 1 < spans.size(); index += 2)
        covered += std::max(0.0f, std::min(right, spans[index + 1]) - std::max(left, spans[index]));
    return std::clamp(covered / (right - left), 0.0f, 1.0f);
}

} // namespace Ubiquitty::Rendering
