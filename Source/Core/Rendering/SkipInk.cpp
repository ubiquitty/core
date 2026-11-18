// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/SkipInk.h"

#include <algorithm>
#include <limits>

namespace Ubiquitty::Rendering {

namespace {

/// How far a distance must fall inside a reach to count as within it, in
/// texels. A reach that meets a pixel centre exactly is decided by the float
/// arithmetic that maps the pixel onto the sheet, which lands a hair to either
/// side, so without it a stroke's two edges could be cleared differently by
/// rounding alone. Far below anything a raster can resolve.
constexpr float reachTolerance = 1.0f / 1024.0f;

/// A texel's coverage, as a fraction. Outside the plane is uncovered.
float coverageAt(const SkipInkPlane& plane, std::int32_t x, std::int32_t y)
{
    if (x < 0 || y < 0 || x >= plane.width || y >= plane.height)
        return 0.0f;
    return static_cast<float>(plane.coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.stride) + static_cast<std::size_t>(x)]) / 255.0f;
}

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
    m_coverage.assign(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height), 0);
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
        std::uint8_t* line = m_coverage.data() + static_cast<std::size_t>(y + row) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
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
        std::uint8_t* line = m_coverage.data() + static_cast<std::size_t>(y + row) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        std::fill(line, line + width, std::uint8_t { 0 });
    }
}

SkipInkPlane skipInkPlaneOf(const SkipInkRecord* record)
{
    if (!record)
        return {};
    return skipInkCoveragePlane(record->m_coverage, record->m_width, record->m_height);
}

bool markSkipInkCuts(const SkipInkPlanes& planes, const SkipInkQuery& query, const SkipInkRule& rule, SkipInkCut& cut, SkipInkScratch& scratch)
{
    cut.spans.clear();
    cut.rowEnds.clear();
    const std::int32_t rows = rule.rows();
    const float width = rule.right - rule.left;
    const float height = rule.bottom - rule.top;
    if (rows <= 0 || !(width > 0.0f))
        return false;
    cut.rowEnds.assign(static_cast<std::size_t>(rows), 0u);
    const std::int32_t firstRow = rule.firstRow();
    const float reachAcross = std::max(query.reachAcross, 0.0f);
    const float reachUpDown = std::max(query.reachUpDown, 0.0f);
    // The stretch of line the cut is worked out over: the rule, and as far as
    // the shortest piece past each edge the line continues across, so a
    // stretch running into the next cell is judged by its whole length.
    const float shortestPiece = std::isfinite(query.shortestPiece) ? std::max(query.shortestPiece, 0.0f) : 0.0f;
    const float workLeft = rule.left - (query.continuesLeft ? shortestPiece : 0.0f);
    const float workRight = rule.right + (query.continuesRight ? shortestPiece : 0.0f);
    const float threshold = static_cast<float>(skipInkCoverageThreshold) / 255.0f;
    std::vector<SkipInkRowSpan>& rowSpans = scratch.rowSpans;
    rowSpans.clear();
    for (const SkipInkSource* sourceOf : { &query.own, &query.left, &query.right }) {
        const SkipInkSource& source = *sourceOf;
        if (!source.isPresent)
            continue;
        const SkipInkPlane& plane = planes.plane(source.sheet);
        if (!isReadable(plane))
            continue;
        const float across = source.inkRight - source.inkLeft;
        if (across == 0.0f || !std::isfinite(across))
            continue;
        // Only texels whose centres lie inside the slot, and inside the plane.
        const std::int32_t slotFirstColumn = std::max(texelOf(source.slotLeft + 0.5f), 0);
        const std::int32_t slotLastColumn = std::min(texelOf(source.slotRight - 0.5f), plane.width - 1);
        const std::int32_t slotFirstRow = std::max(texelOf(source.slotTop + 0.5f), 0);
        const std::int32_t slotLastRow = std::min(texelOf(source.slotBottom - 0.5f), plane.height - 1);
        if (slotLastColumn < slotFirstColumn || slotLastRow < slotFirstRow)
            continue;
        // A point of the rule across and the sheet's texel coordinate under
        // it, both ways.
        const auto uAt = [&](float x) { return source.inkLeft + ((x - rule.left) / width + source.boxesBefore) * across; };
        const auto xAt = [&](float u) { return rule.left + ((u - source.inkLeft) / across - source.boxesBefore) * width; };
        // The texels this rule can reach in the source, across, with two to
        // spare each side: an edge is placed from the texel it lies in and the
        // one beyond, and a stroke whose edge is in reach may start past it.
        const float uLeft = uAt(workLeft);
        const float uRight = uAt(workRight);
        const std::int32_t scanFirst = std::max(texelOf(std::min(uLeft, uRight) - reachAcross) - 2, slotFirstColumn);
        const std::int32_t scanLast = std::min(texelOf(std::max(uLeft, uRight) + reachAcross) + 2, slotLastColumn);
        if (scanLast < scanFirst)
            continue;
        const std::size_t span = static_cast<std::size_t>(scanLast - scanFirst + 1);
        // The rule's top row in the source: ink above it touches a rule only
        // when the query allows it.
        const float down = source.bandBottom - source.bandTop;
        const std::int32_t ruleTopTexel = texelOf(source.bandTop + (height > 0.0f ? (static_cast<float>(firstRow) + 0.5f - rule.top) / height : 0.0f) * down);
        std::vector<std::uint8_t>& strongest = scratch.strongest;
        std::vector<float>& runEdges = scratch.runEdges;
        std::vector<std::int32_t>& runRows = scratch.runRows;
        std::vector<std::int32_t>& strokeOf = scratch.strokeOf;
        strongest.resize(span);
        // Appends one row of the sheet's ink as runs of texels at half
        // coverage or more. A run and the partial texel either side of it are
        // one stroke's coverage, and a stroke box-filtered into texels keeps
        // its width as the coverage's sum and its centre as the coverage's
        // centre, at whatever phase it was drawn: so its edges are those,
        // inside a texel as often as not.
        const auto appendRow = [&](std::int32_t sheetRow) {
            for (std::size_t index = 0; index < span; ++index)
                strongest[index] = static_cast<std::uint8_t>(std::lround(coverageAt(plane, scanFirst + static_cast<std::int32_t>(index), sheetRow) * 255.0f));
            const auto fractionOf = [&](std::size_t index) { return static_cast<float>(strongest[index]) / 255.0f; };
            for (std::size_t index = 0; index < span;) {
                if (fractionOf(index) < threshold) {
                    ++index;
                    continue;
                }
                const std::size_t begin = index;
                while (index < span && fractionOf(index) >= threshold)
                    ++index;
                const std::size_t from = begin > 0 ? begin - 1 : begin;
                const std::size_t through = index < span ? index : index - 1;
                float coverage = 0.0f;
                float moment = 0.0f;
                for (std::size_t texel = from; texel <= through; ++texel) {
                    coverage += fractionOf(texel);
                    moment += fractionOf(texel) * (static_cast<float>(scanFirst) + static_cast<float>(texel) + 0.5f);
                }
                const float centre = moment / coverage;
                runEdges.push_back(centre - coverage * 0.5f);
                runEdges.push_back(centre + coverage * 0.5f);
                runRows.push_back(sheetRow);
            }
        };
        // The sheet row under each of the rule's pixel rows.
        const auto vOf = [&](std::int32_t row) {
            return source.bandTop + (height > 0.0f ? (static_cast<float>(firstRow + row) + 0.5f - rule.top) / height : 0.0f) * down;
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
        const std::int32_t contactTop = std::max(query.isTouchedFromAbove ? firstTexelWithin(vTop, reachUpDown) : ruleTopTexel, slotFirstRow);
        const std::int32_t contactBottom = std::min(lastTexelWithin(vBottom, reachUpDown), slotLastRow);
        const std::int32_t windowTop = std::max(query.isTouchedFromAbove ? firstTexelWithin(vTop, approach) : ruleTopTexel, slotFirstRow);
        const std::int32_t windowBottom = std::min(lastTexelWithin(vBottom, approach), slotLastRow);
        if (contactBottom < contactTop)
            continue;
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
                cutLeft = std::max(cutLeft, workLeft);
                cutRight = std::min(cutRight, workRight);
                if (cutRight > cutLeft)
                    rowSpans.push_back({ row, cutLeft, cutRight });
            }
        }
    }
    if (rowSpans.empty())
        return false;
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
