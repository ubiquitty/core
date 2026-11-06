// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/UnifontRasterizer.cpp — see UnifontRasterizer.h.

#include "Core/Rendering/UnifontRasterizer.h"

#include <algorithm>
#include <array>

#include "Core/Rendering/CellLayout.h"
#include "Core/Rendering/GlyphTiling.h"

#include "Core/Rendering/ProportionalOutline.h"
#include "Core/Rendering/ProportionalStrike.h"
#include "Core/Rendering/Unifont.h"

#include <cmath>
#include <cstring>

namespace Ubiquitty::Rendering {

UnifontRasterizer::UnifontRasterizer(int scale, int lineSpacingInPixels, int characterSpacingInPixels)
    : m_scale(scale > 0 ? scale : 1)
    , m_lineSpacingInPixels(lineSpacingInPixels > 0 ? lineSpacingInPixels : 0)
    , m_characterSpacingInPixels(characterSpacingInPixels)
{
}

int unifontScaleForRequestedSize(double requestedPointSize, double backingScale)
{
    if (requestedPointSize <= 0.0 || backingScale <= 0.0) {
        return 1;
    }
    const double requestedPixels = requestedPointSize * backingScale;
    const int scale = static_cast<int>(std::ceil(requestedPixels / 16.0));
    return scale > 0 ? scale : 1;
}

// The Unifont cell: a fixed 8×16 bitmap. These are the EXACT values
// SoftwareRenderer::render (and the Vulkan grid path) use for the decoration
// pass, so the Unifont path is byte-for-byte identical across the rasterizer
// seam. baseline 13.5 + drop 2 lands the single underline on row 15 (the cell's
// bottom row) under the half-open coverage rule, clear of the baseline + most
// descenders; strike rides the x-height midpoint → row 9. The one-row gap below
// the ink keeps the rule reading cleanly even at the integer-upscaled macOS
// Unifont size, where the gap scales up with the cell.
CellMetrics UnifontRasterizer::cellMetrics() const
{
    CellMetrics m;
    const float scale = static_cast<float>(m_scale);
    // 13.5 up and 2.5 down is Unifont's own 16-row cell.
    const CellLayout layout = cellLayout(13.5f * scale, 2.5f * scale, m_lineSpacingInPixels);
    // Character spacing widens the cell, as line spacing heightens it: the
    // glyph keeps its 8-column box and the gap opens around it. A cell has to
    // stay drawable, so a tightening past the box floors at one column.
    m.cellWidth = std::max(1.0f, 8.0f * scale + static_cast<float>(m_characterSpacingInPixels));
    m.cellHeight = layout.cellHeight;
    m.baselineFromTop = layout.baselineFromTop;
    m.underlineThickness = scale;
    m.underlineDrop = 2.0f * scale;
    m.underlineCurlyThickness = scale;
    m.strikeThickness = scale;
    // The upscale and the device scale are one number here: this raster draws `scale` device
    // pixels per Unifont pixel, and every rule the builder sizes in device
    // pixels (the curl's drop, the doubled rule's gap) must know it. Left at
    // the default of one, the curl sat three quarters of a pixel higher than
    // on a GPU arm that reported its real scale.
    m.deviceScale = static_cast<float>(scale);
    // Unifont's lowercase spans rows 6 to 13 and stands on the bottom of row
    // 13, so its x-height is 8. The strike rides `baseline - xHeight / 2` and
    // the braille lattice centres on the same band; a larger value lifts both
    // above the middle of the letters.
    m.xHeight = 8.0f * scale;
    // The same lift the Metal renderer applies (its font geometry sets
    // 0.5 * scale * fontSize/12). A straight rule sits ON the baseline's drop,
    // and half a pixel of lift is what keeps its top edge off the glyph's
    // descender row; without it the rule lands one device row lower and reads
    // as a heavier underline than the same text on the other backend.
    //
    // The fontSize factor is 1 here: Unifont is a fixed 8x16 bitmap face whose
    // scale is the size, so there is no separate point size to divide by.
    //
    // Dotted and curly are exempt in the builder rather than here — they are
    // placed by their own amplitude, not by the rule's top edge.
    m.underlineStrokeLift = 0.5f * scale;
    m.skipInk = false;
    return m;
}

// The bitmap's own box, before the line spacing the cell adds, and the
// bracket's ink within it: one derivation, in UnifontFaceMetrics.cpp, which
// the frame capture reaches without this bitmap. The measurement it was
// taken from is held by test_unifont_sizing against the glyph itself.
FaceMetrics UnifontRasterizer::faceMetrics() const
{
    return unifontFaceMetricsForScale(m_scale, m_lineSpacingInPixels);
}

// Mirrors SoftwareRenderer::rasterizeInto / VulkanGlyphAtlas::rasterizeInto,
// writing into a slot-local R8 buffer (row stride == slotWidth) rather than an
// atlas sub-rect. Coverage is byte-identical across those paths: native-size
// centered when the glyph fits, otherwise downscale-averaging the covered
// source columns so a too-wide glyph antialiases into the slot (gray edges)
// instead of clipping.
namespace {

// One packed bitmap byte expands to eight coverage samples. The native-size
// path shares this immutable table across fonts and avoids per-pixel division.
constexpr auto bitmapByteCoverage = [] {
    std::array<std::array<std::uint8_t, 8>, 256> table {};
    for (std::size_t byte = 0; byte < table.size(); ++byte)
        for (std::size_t bit = 0; bit < table[byte].size(); ++bit)
            table[byte][bit] = byte & (0x80u >> bit) ? 255 : 0;
    return table;
}();

// The strike a scale draws from and the integer upscale it applies: the 12 px
// strike at scale one and the 24 px strike at scale two are exact, scale three
// triples the 12 px strike and every scale past that doubles or triples one of
// them, as Unifont's own glyphs upscale.
struct StrikeChoice {
    int strikeScale;
    int factor;
};

StrikeChoice strikeChoiceForScale(int scale)
{
    if (scale <= 1)
        return { 1, 1 };
    if (scale == 2)
        return { 2, 1 };
    if (scale % 2 == 0)
        return { 2, scale / 2 };
    return { 1, scale };
}

ProportionalStrike::Face strikeFace(FontFace face)
{
    return face == FontFace::Bold || face == FontFace::BoldItalic ? ProportionalStrike::Face::Bold : ProportionalStrike::Face::Regular;
}

} // namespace

int UnifontRasterizer::proportionalStripOverhangInPixels() const
{
    const StrikeChoice choice = strikeChoiceForScale(m_scale);
    return ProportionalStrike::overhangInPixels(choice.strikeScale) * choice.factor;
}

int UnifontRasterizer::shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut)
{
    if (codePoints.empty() || glyphsOut.size() < codePoints.size())
        return 0;
    const StrikeChoice choice = strikeChoiceForScale(m_scale);
    const ProportionalStrike::Face strike = strikeFace(face);
    const float unitsToPixels = static_cast<float>(choice.factor) / static_cast<float>(ProportionalStrike::unitsPerPixel);
    for (std::size_t i = 0; i < codePoints.size(); ++i) {
        const ProportionalStrike::Glyph glyph = ProportionalStrike::lookup(strike, choice.strikeScale, codePoints[i], 0);
        int advance = glyph.advanceInUnits;
        if (glyph.present && i + 1 < codePoints.size())
            advance += ProportionalStrike::kern(strike, choice.strikeScale, codePoints[i], codePoints[i + 1]);
        glyphsOut[i].glyph = codePoints[i];
        // The advance keeps its fraction: the pen walks in 1/64 px as the
        // shaper measured, and the raster below picks the phase the pen lands at.
        glyphsOut[i].advanceInPixels = static_cast<float>(advance) * unitsToPixels;
        glyphsOut[i].offsetXInPixels = 0.0f;
        glyphsOut[i].offsetYInPixels = 0.0f;
        glyphsOut[i].cluster = static_cast<std::uint16_t>(std::min<std::size_t>(i, 0xFFFF));
        if (!glyph.present && !Unicode::Width::isInvisibleFormat(codePoints[i])) {
            Unifont::Glyph fallback = Unifont::lookup(codePoints[i]);
            if (!fallback.bits) {
                glyphsOut[i].glyph = 0xFFFD;
                fallback = Unifont::lookup(glyphsOut[i].glyph);
            }
            // A missing strike glyph occupies the grid face's real width; its
            // code point stays drawable through both the sheet and strip paths.
            glyphsOut[i].advanceInPixels = static_cast<float>(fallback.width * m_scale);
        }
    }
    return static_cast<int>(codePoints.size());
}

bool UnifontRasterizer::rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
{
    if (stripWidthInPixels <= 0 || stripHeightInPixels <= 0)
        return false;
    const std::size_t expected = static_cast<std::size_t>(stripWidthInPixels) * static_cast<std::size_t>(stripHeightInPixels);
    if (coverageOut.size() != expected)
        return false;
    // The strip's baseline is the cell's, below the overhang rows the slot
    // holds above the cell; a glyph's rows are placed by their distance from
    // the baseline, times the upscale.
    const int baselineY = proportionalStripOverhangInPixels() + static_cast<int>(cellMetrics().baselineFromTop);
    // Past the strikes' two sizes the face is drawn from its outlines at the
    // grid's size, not a strike magnified.
    if (m_scale > 2)
        return drawOutlines(glyphs, face, static_cast<double>(ProportionalStrike::emPixelsPerScale * m_scale), 1, 1.0, baselineY, 1,
            stripWidthInPixels, stripHeightInPixels, coverageOut);
    std::fill(coverageOut.begin(), coverageOut.end(), static_cast<std::uint8_t>(0));
    const StrikeChoice choice = strikeChoiceForScale(m_scale);
    const ProportionalStrike::Face strike = strikeFace(face);
    const int phases = ProportionalStrike::phaseCount(choice.strikeScale);
    if (phases <= 0)
        return false;
    std::vector<std::uint8_t> fallbackCoverage;
    double penX = 0.0;
    for (const Ubiquitty::ProportionalShapedGlyph& shaped : glyphs) {
        const double x = penX + static_cast<double>(shaped.offsetXInPixels);
        penX += static_cast<double>(shaped.advanceInPixels);
        if (shaped.glyph == 0 || Unicode::Width::isInvisibleFormat(shaped.glyph))
            continue;
        // The pen in the strike's own pixels, split into the whole pixel and
        // the phase: the raster drawn is the one FreeType rendered with that
        // fraction in the outline, so the ink sits where the shaper put it.
        const double strikeX = x / static_cast<double>(choice.factor);
        int wholeX = static_cast<int>(std::floor(strikeX));
        int phase = static_cast<int>(std::lround((strikeX - static_cast<double>(wholeX)) * static_cast<double>(phases)));
        if (phase >= phases) {
            wholeX += 1;
            phase = 0;
        }
        const ProportionalStrike::Glyph glyph = ProportionalStrike::lookup(strike, choice.strikeScale, shaped.glyph, phase);
        if (!glyph.present) {
            const Unifont::Glyph fallback = Unifont::lookup(shaped.glyph);
            if (!fallback.bits) {
                return false;
            }
            const int width = fallback.width * m_scale;
            const int height = 16 * m_scale;
            fallbackCoverage.resize(static_cast<std::size_t>(width) * height);
            if (!rasterizeGlyph(shaped.glyph, face, 0, width, height, fallbackCoverage.data())) {
                return false;
            }
            const int startX = static_cast<int>(std::floor(x));
            const int startY = proportionalStripOverhangInPixels() + m_lineSpacingInPixels / 2
                - static_cast<int>(std::round(shaped.offsetYInPixels));
            for (int sourceY = 0; sourceY < height; ++sourceY) {
                const int destinationY = startY + sourceY;
                if (destinationY < 0 || destinationY >= stripHeightInPixels) {
                    continue;
                }
                const std::uint8_t* source = fallbackCoverage.data() + static_cast<std::size_t>(sourceY) * width;
                std::uint8_t* destination = coverageOut.data() + static_cast<std::size_t>(destinationY) * stripWidthInPixels;
                for (int sourceX = 0; sourceX < width; ++sourceX) {
                    const int destinationX = startX + sourceX;
                    if (destinationX >= 0 && destinationX < stripWidthInPixels) {
                        destination[destinationX] = std::max(destination[destinationX], source[sourceX]);
                    }
                }
            }
            continue;
        }
        if (!glyph.coverage)
            continue;
        const int startX = (wholeX + glyph.left) * choice.factor;
        const int startY = baselineY - glyph.top * choice.factor;
        for (int sourceY = 0; sourceY < glyph.height; ++sourceY) {
            const std::uint8_t* sourceRow = glyph.coverage + static_cast<std::size_t>(sourceY) * glyph.width;
            for (int repeatY = 0; repeatY < choice.factor; ++repeatY) {
                const int destinationY = startY + sourceY * choice.factor + repeatY;
                if (destinationY < 0 || destinationY >= stripHeightInPixels)
                    continue;
                std::uint8_t* destinationRow = coverageOut.data() + static_cast<std::size_t>(destinationY) * stripWidthInPixels;
                for (int sourceX = 0; sourceX < glyph.width; ++sourceX) {
                    const std::uint8_t coverage = sourceRow[sourceX];
                    if (coverage == 0)
                        continue;
                    for (int repeatX = 0; repeatX < choice.factor; ++repeatX) {
                        const int destinationX = startX + sourceX * choice.factor + repeatX;
                        // The greater of the two, not a write: glyphs drawn
                        // at a fraction share edge pixels with their
                        // neighbours, and the later one overwriting the
                        // earlier would notch it.
                        if (destinationX >= 0 && destinationX < stripWidthInPixels)
                            destinationRow[destinationX] = std::max(destinationRow[destinationX], coverage);
                    }
                }
            }
        }
    }
    return true;
}

bool UnifontRasterizer::drawOutlines(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, double pixelsPerEm, int across,
    double advanceScale, int baselineY, int fallbackScale, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
{
    const std::size_t expected = static_cast<std::size_t>(stripWidthInPixels) * static_cast<std::size_t>(stripHeightInPixels);
    if (stripWidthInPixels <= 0 || stripHeightInPixels <= 0 || coverageOut.size() != expected)
        return false;
    std::fill(coverageOut.begin(), coverageOut.end(), static_cast<std::uint8_t>(0));
    const ProportionalStrike::Face strike = strikeFace(face);
    std::vector<std::uint8_t> fallbackCoverage;
    double penX = 0.0;
    for (const Ubiquitty::ProportionalShapedGlyph& shaped : glyphs) {
        const double x = penX + static_cast<double>(shaped.offsetXInPixels);
        penX += static_cast<double>(shaped.advanceInPixels) * advanceScale;
        if (shaped.glyph == 0 || Unicode::Width::isInvisibleFormat(shaped.glyph))
            continue;
        if (ProportionalOutline::rasterise(strike, shaped.glyph, pixelsPerEm, static_cast<double>(across), x,
                static_cast<double>(baselineY) - static_cast<double>(shaped.offsetYInPixels), stripWidthInPixels, stripHeightInPixels, coverageOut))
            continue;
        // A code point the face lacks is the grid face's glyph, a bitmap drawn
        // at the size by whole texels, the grid's own rule for it.
        const Unifont::Glyph fallback = Unifont::lookup(shaped.glyph);
        if (!fallback.bits)
            return false;
        const int width = fallback.width * m_scale;
        const int height = 16 * m_scale;
        fallbackCoverage.resize(static_cast<std::size_t>(width) * height);
        if (!rasterizeGlyph(shaped.glyph, face, 0, width, height, fallbackCoverage.data()))
            return false;
        const int startX = static_cast<int>(std::floor(x));
        const int startY = fallbackScale * (proportionalStripOverhangInPixels() + m_lineSpacingInPixels / 2) - static_cast<int>(std::round(shaped.offsetYInPixels));
        const int repeatX = fallbackScale * across;
        for (int y = 0; y < height * fallbackScale; ++y) {
            const int destinationY = startY + y;
            if (destinationY < 0 || destinationY >= stripHeightInPixels)
                continue;
            const std::uint8_t* source = fallbackCoverage.data() + static_cast<std::size_t>(y / fallbackScale) * width;
            std::uint8_t* destination = coverageOut.data() + static_cast<std::size_t>(destinationY) * stripWidthInPixels;
            for (int sourceX = 0; sourceX < width * repeatX; ++sourceX) {
                const int destinationX = startX + sourceX;
                if (destinationX >= 0 && destinationX < stripWidthInPixels)
                    destination[destinationX] = std::max(destination[destinationX], source[sourceX / repeatX]);
            }
        }
    }
    return true;
}

bool UnifontRasterizer::rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, const SizedMaster& master,
    int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
{
    if (master.isIdentity())
        return rasteriseProportionalRun(glyphs, face, stripWidthInPixels, stripHeightInPixels, coverageOut);
    if (!master.scale.isWhole() || stripWidthInPixels <= 0 || stripHeightInPixels <= 0)
        return false;
    const std::size_t expected = static_cast<std::size_t>(stripWidthInPixels) * static_cast<std::size_t>(stripHeightInPixels);
    if (coverageOut.size() != expected)
        return false;
    std::fill(coverageOut.begin(), coverageOut.end(), static_cast<std::uint8_t>(0));
    const int scale = std::max(1, master.scale.whole());
    const int across = master.widthScale == 2 ? 2 : 1;
    const int wholeScale = std::max(1, m_scale) * scale;
    // A size the strikes hold, at its own width, is the strike's FreeType
    // text; any other is the face's outlines drawn at that size.
    if (wholeScale > 2 || across != 1) {
        const int baselineY = scale * (proportionalStripOverhangInPixels() + static_cast<int>(cellMetrics().baselineFromTop));
        return drawOutlines(glyphs, face, static_cast<double>(ProportionalStrike::emPixelsPerScale * wholeScale), across, static_cast<double>(scale * across),
            baselineY, scale, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }
    // The strike the whole size draws from.
    const StrikeChoice choice = strikeChoiceForScale(wholeScale);
    const ProportionalStrike::Face strike = strikeFace(face);
    const int phases = ProportionalStrike::phaseCount(choice.strikeScale);
    if (phases <= 0)
        return false;
    const int baselineY = scale * (proportionalStripOverhangInPixels() + static_cast<int>(cellMetrics().baselineFromTop));
    const int stepX = choice.factor * across;
    std::vector<std::uint8_t> fallbackCoverage;
    double penX = 0.0;
    for (const Ubiquitty::ProportionalShapedGlyph& shaped : glyphs) {
        const double x = penX + static_cast<double>(shaped.offsetXInPixels);
        penX += static_cast<double>(shaped.advanceInPixels) * scale * across;
        if (shaped.glyph == 0 || Unicode::Width::isInvisibleFormat(shaped.glyph))
            continue;
        const double strikeX = x / static_cast<double>(stepX);
        int wholeX = static_cast<int>(std::floor(strikeX));
        int phase = static_cast<int>(std::lround((strikeX - static_cast<double>(wholeX)) * static_cast<double>(phases)));
        if (phase >= phases) {
            wholeX += 1;
            phase = 0;
        }
        const ProportionalStrike::Glyph glyph = ProportionalStrike::lookup(strike, choice.strikeScale, shaped.glyph, phase);
        if (!glyph.present) {
            // A code point the strike lacks is the grid face's glyph, drawn at
            // the master's size by whole texels as the grid draws it.
            const Unifont::Glyph fallback = Unifont::lookup(shaped.glyph);
            if (!fallback.bits)
                return false;
            const int width = fallback.width * m_scale;
            const int height = 16 * m_scale;
            fallbackCoverage.resize(static_cast<std::size_t>(width) * height);
            if (!rasterizeGlyph(shaped.glyph, face, 0, width, height, fallbackCoverage.data()))
                return false;
            const int startX = static_cast<int>(std::floor(x));
            const int startY = scale * (proportionalStripOverhangInPixels() + m_lineSpacingInPixels / 2) - static_cast<int>(std::round(shaped.offsetYInPixels));
            for (int y = 0; y < height * scale; ++y) {
                const int destinationY = startY + y;
                if (destinationY < 0 || destinationY >= stripHeightInPixels)
                    continue;
                const std::uint8_t* source = fallbackCoverage.data() + static_cast<std::size_t>(y / scale) * width;
                std::uint8_t* destination = coverageOut.data() + static_cast<std::size_t>(destinationY) * stripWidthInPixels;
                for (int sourceX = 0; sourceX < width * scale * across; ++sourceX) {
                    const int destinationX = startX + sourceX;
                    if (destinationX >= 0 && destinationX < stripWidthInPixels)
                        destination[destinationX] = std::max(destination[destinationX], source[sourceX / (scale * across)]);
                }
            }
            continue;
        }
        if (!glyph.coverage)
            continue;
        const int startX = (wholeX + glyph.left) * stepX;
        const int startY = baselineY - glyph.top * choice.factor;
        for (int sourceY = 0; sourceY < glyph.height; ++sourceY) {
            const std::uint8_t* sourceRow = glyph.coverage + static_cast<std::size_t>(sourceY) * glyph.width;
            for (int repeatY = 0; repeatY < choice.factor; ++repeatY) {
                const int destinationY = startY + sourceY * choice.factor + repeatY;
                if (destinationY < 0 || destinationY >= stripHeightInPixels)
                    continue;
                std::uint8_t* destinationRow = coverageOut.data() + static_cast<std::size_t>(destinationY) * stripWidthInPixels;
                for (int sourceX = 0; sourceX < glyph.width; ++sourceX) {
                    const std::uint8_t coverage = sourceRow[sourceX];
                    if (coverage == 0)
                        continue;
                    for (int repeatX = 0; repeatX < stepX; ++repeatX) {
                        const int destinationX = startX + sourceX * stepX + repeatX;
                        if (destinationX >= 0 && destinationX < stripWidthInPixels)
                            destinationRow[destinationX] = std::max(destinationRow[destinationX], coverage);
                    }
                }
            }
        }
    }
    return true;
}

bool UnifontRasterizer::rasterizeGlyph(std::uint32_t codePoint, FontFace /*face*/, std::uint32_t /*weight*/, int slotWidth, int slotHeight, std::uint8_t* coverageOut)
{
    if (slotWidth <= 0 || slotHeight <= 0 || !coverageOut) {
        return false;
    }
    return rasterizeGlyphInCell(codePoint, slotWidth, slotHeight, slotWidth, slotHeight,
        { coverageOut, static_cast<std::size_t>(slotWidth) * slotHeight });
}

bool UnifontRasterizer::rasterizeGlyphInCell(std::uint32_t codePoint, int cellWidth, int cellHeight,
    int slotWidth, int slotHeight, CoverageSlot coverageOut)
{
    if (slotWidth <= 0 || slotHeight <= 0 || cellWidth <= 0 || cellHeight <= 0
        || cellWidth > slotWidth || cellHeight > slotHeight
        || coverageOut.size() < static_cast<std::size_t>(slotWidth) * slotHeight) {
        return false;
    }
    for (int i = 0; i < slotWidth * slotHeight; ++i)
        coverageOut[i] = 0;

    const unsigned donor = stemDonor(codePoint);
    const unsigned drawnCodePoint = donor ? donor : codePoint;
    const Unifont::Glyph glyph = Unifont::lookup(drawnCodePoint);
    if (!glyph.bits)
        return false;
    const int sourceWidth = glyph.width; // 8 or 16
    const int shift = sourceWidth - 1;
    const int requestedWidth = sourceWidth * m_scale;
    const int requestedHeight = 16 * m_scale;
    const int drawnWidth = std::min(requestedWidth, cellWidth);
    const int drawnHeight = std::min(requestedHeight, cellHeight);
    const int xOffset = (slotWidth - drawnWidth) / 2;
    const int yOffset = (slotHeight - drawnHeight) / 2;

    if (tilesOnTextPen(codePoint)) {
        const auto rowBits = [&](int row) -> unsigned {
            return sourceWidth == 8 ? glyph.bits[row]
                                    : (static_cast<unsigned>(glyph.bits[row * 2]) << 8) | glyph.bits[row * 2 + 1];
        };
        int first = 0;
        int last = 15;
        while (first <= last && !rowBits(first)) {
            ++first;
        }
        while (last >= first && !rowBits(last)) {
            --last;
        }
        const int marginY = (slotHeight - cellHeight) / 2;
        const int marginX = (slotWidth - cellWidth) / 2;
        const auto fit = fitTilingGlyph(drawnCodePoint, (15 - last) * m_scale,
            (last - first + 1) * m_scale, slotHeight - marginY - cellHeight, cellHeight);
        if (fit.isApplicable) {
            for (int row = marginY; row < marginY + cellHeight; ++row) {
                const double fontY = (slotHeight - row - 0.5) / fit.verticalScale - fit.baseline;
                const int sourceY = static_cast<int>(std::floor(16.0 - fontY / m_scale));
                if (sourceY < 0 || sourceY >= 16) {
                    continue;
                }
                const unsigned bits = rowBits(sourceY);
                for (int column = 0; column < drawnWidth; ++column) {
                    const int sourceX = column * sourceWidth / drawnWidth;
                    if ((bits >> (shift - sourceX)) & 1u) {
                        coverageOut[static_cast<std::size_t>(row) * slotWidth + xOffset + column] = 255;
                    }
                }
            }
            TilingCoverage(coverageOut, slotWidth, slotHeight, marginX, marginX + cellWidth,
                marginY, marginY + cellHeight)
                .finish(codePoint, drawnCodePoint);
            return true;
        }
    }

    if (drawnWidth == sourceWidth && drawnHeight == 16) {
        const int bytesPerRow = sourceWidth / 8;
        for (int row = 0; row < 16; ++row) {
            auto* destination = &coverageOut[static_cast<std::size_t>(yOffset + row) * slotWidth + xOffset];
            for (int byte = 0; byte < bytesPerRow; ++byte)
                std::memcpy(destination + byte * 8, bitmapByteCoverage[glyph.bits[row * bytesPerRow + byte]].data(), 8);
        }
    } else {
        for (int destinationY = 0; destinationY < drawnHeight; ++destinationY) {
            const int sourceY = destinationY * 16 / drawnHeight;
            const unsigned bitsRow = (sourceWidth == 8) ? glyph.bits[sourceY] : ((static_cast<unsigned>(glyph.bits[sourceY * 2]) << 8) | glyph.bits[sourceY * 2 + 1]);
            std::uint8_t* destination = &coverageOut[static_cast<std::size_t>(yOffset + destinationY) * slotWidth];
            for (int destinationX = 0; destinationX < drawnWidth; ++destinationX) {
                const int sourceX = destinationX * sourceWidth / drawnWidth;
                if ((bitsRow >> (shift - sourceX)) & 1u)
                    destination[xOffset + destinationX] = 255;
            }
        }
    }
    if (tilesOnTextPen(codePoint)) {
        const int marginY = (slotHeight - cellHeight) / 2;
        const int marginX = (slotWidth - cellWidth) / 2;
        TilingCoverage(coverageOut, slotWidth, slotHeight, marginX, marginX + cellWidth,
            marginY, marginY + cellHeight)
            .finish(codePoint, drawnCodePoint);
    }
    return true;
}

bool rasteriseUnifontIntoSlot(std::uint32_t codePoint, int scale, int nativeWidth,
    int nativeHeight, CoverageSlot coverageOut, int slotWidthInPixels,
    int slotHeightInPixels)
{
    // The source cell is bounded by the face's one-cell and two-cell bitmaps.
    static constexpr int maxNativeCell = 16 * 16;
    if (nativeWidth <= 0 || nativeHeight <= 0 || nativeWidth * nativeHeight > maxNativeCell || slotWidthInPixels <= 0
        || slotHeightInPixels <= 0
        || coverageOut.size() < static_cast<std::size_t>(slotWidthInPixels) * slotHeightInPixels)
        return false;
    if (tilesOnTextPen(codePoint)) {
        UnifontRasterizer scaled(scale);
        return scaled.rasterizeGlyph(codePoint, FontFace::Regular, 0, slotWidthInPixels, slotHeightInPixels, coverageOut.data());
    }
    // Sample the source bitmap at the final cell width. Narrowing a wide
    // glyph before upscaling discards columns that the destination can hold.
    UnifontRasterizer scaled(scale);
    return scaled.rasterizeGlyphInCell(codePoint, std::min(nativeWidth * std::max(scale, 1), slotWidthInPixels),
        std::min(nativeHeight * std::max(scale, 1), slotHeightInPixels), slotWidthInPixels, slotHeightInPixels, coverageOut);
}

} // namespace Ubiquitty::Rendering
