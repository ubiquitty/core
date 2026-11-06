// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/Drawing.h"

#include "Core/Rendering/CellLayout.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/RuleGeometry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace Ubiquitty::RichText {

BlockDrawing BlockDrawing::draw(const Block& block, const Layout& layout, const TypographyFacts* facts)
{
    BlockDrawing drawing;
    const Document& document = block.document;
    drawing.m_text.assign(document.text.begin(), document.text.end());
    if (layout.isElided()) {
        drawing.m_text.resize(layout.copyableEnd());
        drawing.m_text.push_back(0x2026);
    }
    drawing.m_visibleRects.assign(block.visibleRects.begin(), block.visibleRects.end());
    drawing.m_overflow = block.overflow;
    drawing.m_padding = layout.padding();
    drawing.m_cellWidth = layout.cellWidth();
    drawing.m_cellHeight = layout.cellHeight();
    const bool hasFaces = facts && facts->faceCount;
    drawing.m_styles.reserve(document.runs.size());
    for (const Run& run : document.runs) {
        if (run.start >= drawing.m_text.size())
            break;
        DrawnStyle style;
        style.alignment = run.alignment;
        style.paragraph = run.paragraph;
        style.first = run.start;
        style.end = std::min<std::uint32_t>(run.start + run.length, static_cast<std::uint32_t>(drawing.m_text.size()));
        if (layout.isShaped() && run.font != noFontEntry) {
            style.setFont(document.fonts[run.font]);
        } else if (hasFaces) {
            const FaceMetrics& face = facts->faces[resolveFace(*facts, document, run)];
            style.familyClass = face.familyClass;
            style.name = face.name;
            style.nameLength = face.nameLength;
        } else if (run.font != noFontEntry) {
            style.familyClass = document.fonts[run.font].familyClass;
        }
        style.styles = run.styles;
        style.halfPoints = block.lineMode == LineMode::Terminal && facts && facts->terminalSize(style.familyClass)
            ? static_cast<float>(run.halfPoints) * facts->terminalSize(style.familyClass) / 768.0f
            : run.halfPoints;
        if (run.color) {
            const ColorEntry& color = document.colors[run.color];
            style.red = color.red;
            style.green = color.green;
            style.blue = color.blue;
            style.isAutomaticColor = 0;
        }
        if (run.background) {
            const ColorEntry& color = document.colors[run.background];
            style.backgroundRed = color.red;
            style.backgroundGreen = color.green;
            style.backgroundBlue = color.blue;
            style.hasBackground = 1;
        }
        drawing.m_styles.push_back(style);
    }
    if (layout.isElided()) {
        // The synthetic ellipsis inherits the last visible run, including
        // when the cut falls exactly at a style boundary.
        while (drawing.m_styles.size() > 1 && drawing.m_styles.back().first >= layout.copyableEnd())
            drawing.m_styles.pop_back();
        drawing.m_styles.back().end = static_cast<std::uint32_t>(drawing.m_text.size());
    }

    std::size_t run = 0;
    auto styleOf = [&](std::uint32_t character) -> std::uint16_t {
        while (run + 1 < document.runs.size() && document.runs[run + 1].start <= character)
            ++run;
        return static_cast<std::uint16_t>(run);
    };
    for (std::size_t index = layout.firstVisibleLine(); index < layout.visibleLineCount(); ++index) {
        const LineLayout line = layout.line(index);
        if (layout.isShaped()) {
            drawing.m_shapedLines.push_back({ line.first, line.end, line.top - layout.viewportTop(), line.height, line.baseline, line.origin, line.width });
            if (layout.isElided() && index + 1 == layout.visibleLineCount())
                ++drawing.m_shapedLines.back().end;
            for (const ShapedSpan& span : layout.shapedSpans()) {
                if (span.line != index)
                    continue;
                drawing.m_clusters.push_back({
                    .first = span.first,
                    .end = span.end,
                    .x = static_cast<std::uint32_t>(std::min(span.leading, span.trailing)),
                    .baseline = line.top + line.baseline - layout.viewportTop(),
                    .advance = static_cast<std::uint32_t>(std::abs(span.trailing - span.leading)),
                    .top = line.top - layout.viewportTop(),
                    .height = line.height,
                    .style = styleOf(span.first),
                });
            }
            continue;
        }
        const std::uint32_t baseline = line.top + line.baseline - layout.viewportTop();
        std::uint32_t character = line.first;
        while (character < line.end) {
            const std::uint32_t end = layout.graphemeEnd(character);
            const std::uint32_t codePoint = document.text[character];
            const std::uint32_t left = layout.leftEdge(line, character);
            // A grapheme starting past the block's right edge is clipped
            // whole; one crossing it is drawn and clipped by the raster.
            if (line.origin + left >= layout.blockWidth())
                break;
            if (!isLineBreak(codePoint)) {
                drawing.m_clusters.push_back({
                    .first = character,
                    .end = std::min(end, line.end),
                    .x = line.origin + left,
                    .baseline = baseline,
                    .advance = layout.rightEdge(std::min(end, line.end) - 1) - left,
                    .top = line.top - layout.viewportTop(),
                    .height = line.height,
                    .style = styleOf(character),
                    .isEllipsis = 0,
                });
            }
            character = end;
        }
        if (line.ellipsisAdvance) {
            const std::uint32_t before = line.end > line.first ? line.end - 1 : line.first;
            const std::uint32_t x = line.end > line.first ? layout.rightEdge(line.end - 1) : 0;
            drawing.m_clusters.push_back({
                .first = line.end,
                .end = line.end,
                .x = line.origin + x,
                .baseline = baseline,
                .advance = line.ellipsisAdvance,
                .top = line.top - layout.viewportTop(),
                .height = line.height,
                .style = document.runs.empty() ? std::uint16_t { 0 } : styleOf(before),
                .isEllipsis = 1,
            });
        }
    }

    constexpr auto decorations = static_cast<std::uint8_t>(std::to_underlying(StyleBit::Underline) | std::to_underlying(StyleBit::Strikethrough));
    for (const DrawnCluster& cluster : drawing.m_clusters) {
        if (cluster.style >= drawing.m_styles.size() || !(drawing.m_styles[cluster.style].styles & decorations) || !cluster.advance)
            continue;
        if (!drawing.m_rules.empty()) {
            // A right-to-left stretch grows leftwards, so either edge may be
            // the one the next grapheme touches.
            DrawnRule& rule = drawing.m_rules.back();
            const bool isSameStretch = rule.style == cluster.style && rule.baseline == cluster.baseline;
            if (isSameStretch && cluster.x == rule.x + rule.width) {
                rule.width += cluster.advance;
                continue;
            }
            if (isSameStretch && cluster.x + cluster.advance == rule.x) {
                rule.x = cluster.x;
                rule.width += cluster.advance;
                continue;
            }
        }
        drawing.m_rules.push_back({ .x = cluster.x, .width = cluster.advance, .baseline = cluster.baseline, .style = cluster.style });
    }
    return drawing;
}

void appendHighlights(const Layout& layout, const BlockSelection& selection, const BlockQuad& quad, std::vector<HighlightQuad>& out)
{
    const std::uint32_t first = selection.first;
    const std::uint32_t end = selection.end;
    if (first >= end)
        return;
    const std::size_t lines = layout.visibleLineCount();
    const std::size_t firstLine = layout.firstVisibleLine();
    const auto blockWidth = static_cast<std::int32_t>(layout.blockWidth());
    const std::uint32_t blockHeight = layout.blockHeight();
    const std::uint32_t shownTop = std::min(layout.viewportTop(), blockHeight);
    for (std::size_t index = firstLine; index < lines; ++index) {
        const LineLayout line = layout.line(index);
        const bool isLastLine = index + 1 == lines;
        const std::uint32_t lineEnd = line.endsWithLineFeed ? line.end - 1 : line.end;
        // Reached from before: from an earlier line, or from the grid ahead
        // of the block. Gone on past: into a later line through the line's
        // own break, or out of the block after its last shown line.
        const bool isReachedFromBefore = first < line.first || (line.first == 0 && first == 0 && selection.isContinuedBefore);
        const bool isGoneOnPast = isLastLine ? end >= layout.copyableEnd() && selection.isContinuedAfter : end >= line.end && end > lineEnd;
        const std::uint32_t from = std::max(first, line.first);
        const std::uint32_t to = std::min(end, lineEnd);
        if (from >= to && !(isReachedFromBefore && isGoneOnPast))
            continue;
        const std::uint32_t top = std::clamp(index == firstLine && isReachedFromBefore ? shownTop : line.top, shownTop, blockHeight);
        const std::uint32_t bottom = std::clamp(isLastLine ? (isGoneOnPast ? blockHeight : line.top + line.height) : layout.line(index + 1).top, shownTop, blockHeight);
        if (bottom <= top)
            continue;
        const auto band = [&](std::int32_t left, std::int32_t right) {
            left = std::max(left, 0);
            right = std::min(right, blockWidth);
            if (right <= left)
                return;
            out.push_back({
                .left = quad.left + static_cast<float>(left) * quad.pixelsPerUnitX,
                .top = quad.top + static_cast<float>(top - shownTop) * quad.pixelsPerUnitY,
                .width = static_cast<float>(right - left) * quad.pixelsPerUnitX,
                .height = static_cast<float>(bottom - top) * quad.pixelsPerUnitY,
                .red = quad.highlightRed,
                .green = quad.highlightGreen,
                .blue = quad.highlightBlue,
                .alpha = quad.highlightAlpha,
            });
        };
        if (isReachedFromBefore && isGoneOnPast) {
            band(0, blockWidth);
            continue;
        }
        if (!layout.isShaped()) {
            const auto left = static_cast<std::int32_t>(line.origin + layout.leftEdge(line, from));
            const auto right = static_cast<std::int32_t>(line.origin + layout.rightEdge(to - 1));
            band(isReachedFromBefore ? 0 : left, isGoneOnPast ? blockWidth : right);
            continue;
        }
        // The selected spans' extent, or each span where the band is the
        // selected glyphs alone.
        std::int32_t extentLeft = blockWidth;
        std::int32_t extentRight = 0;
        for (const ShapedSpan& span : layout.shapedSpans()) {
            if (span.line != index || span.end <= from || span.first >= to)
                continue;
            const std::int32_t left = std::min(span.leading, span.trailing);
            const std::int32_t right = std::max(span.leading, span.trailing);
            if (isReachedFromBefore || isGoneOnPast) {
                extentLeft = std::min(extentLeft, left);
                extentRight = std::max(extentRight, right);
            } else {
                band(left, right);
            }
        }
        if (isReachedFromBefore && extentRight > 0)
            band(0, extentRight);
        else if (isGoneOnPast && extentLeft < blockWidth)
            band(extentLeft, blockWidth);
    }
}

DecorationRows decorationRows(const DecorationFace& face, float baseline, const BlockQuad& quad)
{
    const float scale = quad.deviceScale > 0 ? quad.deviceScale : 1.0f;
    const Rendering::RuleFaceMetrics metrics {
        .underlineThickness = face.underlineThickness,
        .descent = face.descent,
        .leading = face.leading,
    };
    const Rendering::RuleGeometry geometry = Rendering::ruleGeometry(face.em, scale, metrics);
    const float multiplier = quad.underlineThicknessScale > 0 ? quad.underlineThicknessScale : 0.0f;
    const float underlineWeight = Rendering::snappedRuleWeight(geometry.underlineThickness * multiplier);
    const float strikeWeight = Rendering::snappedRuleWeight(geometry.strikeThickness * multiplier);
    const float ruleLine = baseline + geometry.underlineDrop + quad.underlineOffset - Rendering::underlineStrokeLiftForEm(face.em);
    const float underlineTop = Rendering::singleUnderlineTop(ruleLine, underlineWeight, scale);
    const float xHeight = Rendering::xHeightForStrike(face.xHeight >= 1.0f ? face.xHeight : 0.0f, face.ascent);
    const float strikeTop = Rendering::strikeTop(baseline, xHeight, strikeWeight);
    return { .underlineTop = underlineTop, .underlineBottom = underlineTop + underlineWeight, .strikeTop = strikeTop, .strikeBottom = strikeTop + strikeWeight };
}

PixelRect clippedRuleRect(double left, double top, double right, double bottom, int width, int height)
{
    if (width <= 0 || height <= 0 || !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)
        || right <= left || bottom <= top)
        return {};
    left = std::max(0.0, left);
    top = std::max(0.0, top);
    right = std::min(static_cast<double>(width), right);
    bottom = std::min(static_cast<double>(height), bottom);
    if (right <= left || bottom <= top)
        return {};
    const int firstColumn = static_cast<int>(std::floor(left));
    const int firstRow = static_cast<int>(std::floor(top));
    const int endColumn = static_cast<int>(std::ceil(right));
    const int endRow = static_cast<int>(std::ceil(bottom));
    return { .left = firstColumn, .top = firstRow, .width = endColumn - firstColumn, .height = endRow - firstRow };
}

void skipInkRuleAlpha(InkCoverage ink, int inkWidth, int inkHeight, PixelRect rule, double ruleTop, float padding, RuleAlpha alpha)
{
    if (rule.width <= 0 || rule.height <= 0 || static_cast<std::size_t>(rule.width) > alpha.size() / static_cast<std::size_t>(rule.height))
        return;
    const auto ruleAlpha = alpha.first(static_cast<std::size_t>(rule.width) * static_cast<std::size_t>(rule.height));
    if (inkWidth <= 0 || inkHeight <= 0 || !std::isfinite(ruleTop) || static_cast<std::size_t>(inkWidth) > ink.size() / static_cast<std::size_t>(inkHeight)) {
        std::ranges::fill(ruleAlpha, std::uint8_t { 255 });
        return;
    }
    // A coverage read between pixel centres, as a sampler reads a sheet;
    // outside the raster is no ink.
    const auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= inkWidth || y >= inkHeight)
            return 0.0f;
        return static_cast<float>(ink[static_cast<std::size_t>(y) * static_cast<std::size_t>(inkWidth) + static_cast<std::size_t>(x)]);
    };
    const auto sample = [&](float x, float y) {
        const float left = std::floor(x - 0.5f);
        const float top = std::floor(y - 0.5f);
        // A very large configured reach can put a tap beyond any integer
        // coordinate. Such a tap sees no ink; reject it before narrowing.
        if (!std::isfinite(left) || !std::isfinite(top) || left < -1 || top < -1 || static_cast<double>(left) >= inkWidth || static_cast<double>(top) >= inkHeight)
            return 0.0f;
        const float across = x - 0.5f - left;
        const float down = y - 0.5f - top;
        const int column = static_cast<int>(left);
        const int row = static_cast<int>(top);
        const float upper = at(column, row) + (at(column + 1, row) - at(column, row)) * across;
        const float lower = at(column, row + 1) + (at(column + 1, row + 1) - at(column, row + 1)) * across;
        return upper + (lower - upper) * down;
    };
    const Rendering::SkipInkReach reach = Rendering::skipInkReach(padding);
    const float stepX = reach.horizontal / static_cast<float>(reach.steps);
    const float stepY = reach.vertical / static_cast<float>(reach.steps);
    for (int row = 0; row < rule.height; ++row) {
        for (int column = 0; column < rule.width; ++column) {
            const float centreX = static_cast<float>(rule.left) + static_cast<float>(column) + 0.5f;
            const float centreY = static_cast<float>(rule.top) + static_cast<float>(row) + 0.5f;
            float strongest = 0.0f;
            for (int deltaY = -reach.steps; deltaY <= reach.steps; ++deltaY) {
                for (int deltaX = -reach.steps; deltaX <= reach.steps; ++deltaX) {
                    if (deltaX * deltaX + deltaY * deltaY > reach.steps * reach.steps)
                        continue;
                    // Only ink in the rule's rows or below them crosses it. A
                    // block's rule can hang a single row under its baseline,
                    // where the grid's never does, and the reach upward would
                    // let every round letter's overshoot cut it.
                    const double tapY = std::max(static_cast<double>(centreY + static_cast<float>(deltaY) * stepY), ruleTop + 0.5);
                    if (!std::isfinite(tapY) || tapY < -0.5 || tapY >= static_cast<double>(inkHeight) + 0.5)
                        continue;
                    strongest = std::max(strongest, sample(centreX + static_cast<float>(deltaX) * stepX, static_cast<float>(tapY)));
                }
            }
            ruleAlpha[static_cast<std::size_t>(row) * static_cast<std::size_t>(rule.width) + static_cast<std::size_t>(column)]
                = static_cast<std::uint8_t>(std::lround(255.0f - std::min(strongest, 255.0f)));
        }
    }
}

std::shared_ptr<const BlockDrawing> LayoutCache::drawingOf(const Block& block, const PublishedTypography& typography)
{
    Entry& entry = entryOf(block, typography);
    if (!entry.drawing) {
        entry.drawing = std::make_shared<const BlockDrawing>(BlockDrawing::draw(block, entry.layout, typography.facts.get()));
        ++m_drawingCount;
    }
    return entry.drawing;
}

} // namespace Ubiquitty::RichText
