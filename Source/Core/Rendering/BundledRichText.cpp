// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/BundledRichText.h"

#include "Core/Rendering/ProportionalOutline.h"
#include "Core/Rendering/ProportionalStrike.h"
#include "Core/Rendering/Unifont.h"
#include "Core/RichText/Drawing.h"
#include "Core/Unicode/Width.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace Ubiquitty::Rendering {

namespace {

using RichText::FamilyClass;
using RichText::StyleBit;

constexpr auto boldBit = static_cast<std::uint8_t>(StyleBit::Bold);
constexpr auto italicBit = static_cast<std::uint8_t>(StyleBit::Italic);
constexpr auto underlineBit = static_cast<std::uint8_t>(StyleBit::Underline);
constexpr auto strikethroughBit = static_cast<std::uint8_t>(StyleBit::Strikethrough);

/// Layout units in one point.
constexpr double unitsPerPoint = 4096.0;

/// A face's vertical design, in ems: what its table entry publishes and what
/// its decorations are drawn from.
struct FaceDesign {
    double ascent = 0.0;
    double descent = 0.0;
    /// The underline's centre below the baseline.
    double underlinePosition = 0.0;
    /// The thickness of the underline and the strikethrough.
    double ruleThickness = 0.0;
    /// The strikethrough's centre rides half of it above the baseline.
    double xHeight = 0.0;
};

// Noto Sans's `hhea` ascender and descender, `post` underline and `OS/2`
// x-height, in its 1000-unit em (Resources/fonts/NotoSans-Regular.ttf and
// NotoSans-Bold.ttf); the line gap is zero.
constexpr FaceDesign notoSansRegular { 1.069, 0.293, 0.100, 0.050, 0.536 };
constexpr FaceDesign notoSansBold { 1.069, 0.293, 0.100, 0.050, 0.546 };

// GNU Unifont's sixteen-row box with its baseline 13.5 rows down, as the grid
// seats it (`UnifontRasterizer::cellMetrics`): the underline on the bottom
// row and the strikethrough on row 8, the x-height's middle.
constexpr double unifontRows = 16.0;
constexpr double unifontBaselineRows = 13.5;
constexpr FaceDesign unifontDesign {
    unifontBaselineRows / unifontRows,
    (unifontRows - unifontBaselineRows) / unifontRows,
    2.0 / unifontRows,
    1.0 / unifontRows,
    10.0 / unifontRows,
};

/// The shear of a face drawn oblique: tan 12 degrees, the slant FreeType's
/// synthetic oblique gives a face without an italic design.
constexpr double obliqueSlant = 0.2126;

/// The strike the sans-serif face's advances are read from: the 24 px one,
/// whose advances are HarfBuzz's for the unhinted outline, in 1/64 px.
constexpr int measuredStrikeScale = 2;

/// The most retained raster bytes across a frame's blocks; a block past it is
/// not drawn, as the Metal arm bounds its block textures.
constexpr std::size_t maxRetainedBytes = 64u * 1024u * 1024u;

[[nodiscard]] ProportionalStrike::Face strikeFace(std::uint8_t styles)
{
    return (styles & boldBit) ? ProportionalStrike::Face::Bold : ProportionalStrike::Face::Regular;
}

[[nodiscard]] const FaceDesign& designOf(FamilyClass familyClass, std::uint8_t styles)
{
    if (familyClass == FamilyClass::Monospace)
        return unifontDesign;
    return (styles & boldBit) ? notoSansBold : notoSansRegular;
}

/// Whether the sans-serif face draws a code point from its own outline. One
/// it lacks is GNU Unifont's glyph, in the table and on the raster alike.
[[nodiscard]] bool isSansGlyph(ProportionalStrike::Face face, std::uint32_t codePoint)
{
    return ProportionalOutline::hasGlyph(face, codePoint) && ProportionalStrike::lookup(face, measuredStrikeScale, codePoint, 0).present;
}

/// GNU Unifont's glyph for a code point, or U+FFFD's where it has none.
[[nodiscard]] Unifont::Glyph unifontGlyph(std::uint32_t codePoint)
{
    const Unifont::Glyph glyph = Unifont::lookup(codePoint);
    return glyph.bits ? glyph : Unifont::lookup(0xFFFD);
}

/// A code point's advance in a class face, in ems.
[[nodiscard]] double advanceOf(FamilyClass familyClass, std::uint8_t styles, std::uint32_t codePoint)
{
    const ProportionalStrike::Face face = strikeFace(styles);
    if (familyClass != FamilyClass::Monospace && isSansGlyph(face, codePoint)) {
        const double strikeEm = static_cast<double>(ProportionalStrike::emPixelsPerScale * measuredStrikeScale * ProportionalStrike::unitsPerPixel);
        return static_cast<double>(ProportionalStrike::lookup(face, measuredStrikeScale, codePoint, 0).advanceInUnits) / strikeEm;
    }
    return static_cast<double>(unifontGlyph(codePoint).width) / unifontRows;
}

[[nodiscard]] std::uint16_t inTableUnits(double ems)
{
    return static_cast<std::uint16_t>(std::clamp<long>(std::lround(ems * RichText::unitsPerEm), 0, std::numeric_limits<std::uint16_t>::max()));
}

[[nodiscard]] std::uint16_t inSixtyFourths(double points)
{
    return static_cast<std::uint16_t>(std::clamp<long>(std::lround(points * 64.0), 1, RichText::maxCellSize));
}

void addFace(RichText::TypographyFacts& facts, FamilyClass familyClass, std::uint8_t styles)
{
    const FaceDesign& design = designOf(familyClass, styles);
    RichText::FaceMetrics& face = facts.faces[facts.faceCount++];
    face = {};
    face.familyClass = familyClass;
    face.styles = styles;
    face.ascent = inTableUnits(design.ascent);
    face.descent = inTableUnits(design.descent);
    face.lineHeight = inTableUnits(design.ascent + design.descent);
    for (std::size_t index = 0; index < face.advances.size(); ++index)
        face.advances[index] = inTableUnits(advanceOf(familyClass, styles, RichText::firstTabledCodePoint + static_cast<std::uint32_t>(index)));
    // The printable ASCII advances' mean, which an estimate takes, as every
    // arm's table derives it.
    constexpr std::size_t asciiCount = 0x7F - RichText::firstTabledCodePoint;
    std::uint32_t total = 0;
    for (std::size_t index = 0; index < asciiCount; ++index)
        total += face.advances[index];
    face.averageAdvance = static_cast<std::uint16_t>(total / asciiCount);
}

/// A rectangle of device pixels, `[left, right)` by `[top, bottom)`.
struct PixelBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] bool isEmpty() const { return right <= left || bottom <= top; }
};

[[nodiscard]] PixelBox intersection(PixelBox one, PixelBox other)
{
    return { std::max(one.left, other.left), std::max(one.top, other.top), std::min(one.right, other.right), std::min(one.bottom, other.bottom) };
}

/// A block's premultiplied RGBA8 raster, borrowed while it is drawn.
struct RasterView {
    std::span<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
};

/// The coverage a cluster's code points are drawn into before the cluster is
/// blended: the box its glyphs can reach, clipped to the raster.
struct Plane {
    PixelBox box;
    std::vector<std::uint8_t> coverage;
    std::vector<float> area;

    [[nodiscard]] int width() const { return box.right - box.left; }
    [[nodiscard]] int height() const { return box.bottom - box.top; }
};

/// Draws a GNU Unifont glyph at an em size, keeping the greater of its
/// coverage and what the plane holds.
///
/// Each texel is a square whose coverage of a pixel is the share of the pixel
/// it covers, so the bitmap is drawn at any size as the outline faces are,
/// and at a whole multiple of its rows it is the grid's own glyph. Bold sets
/// each texel's right neighbour too, the one-texel emboldening a bitmap face
/// takes; italic moves each row across by its height above the baseline.
void drawBitmap(Unifont::Glyph glyph, double emPixels, double horizontalScale, double penX, double baselineY, std::uint8_t styles, Plane& plane)
{
    if (!glyph.bits || glyph.width <= 0)
        return;
    const int width = plane.width();
    const int height = plane.height();
    const double texelY = emPixels / unifontRows;
    const double texelX = texelY * horizontalScale;
    const double slant = (styles & italicBit) ? obliqueSlant : 0.0;
    const bool isEmboldened = (styles & boldBit) != 0;
    const int stride = glyph.width / 8;
    const auto isSet = [&](int row, int column) {
        return column >= 0 && column < glyph.width && (glyph.bits[row * stride + column / 8] & (0x80u >> (column % 8))) != 0;
    };
    std::fill(plane.area.begin(), plane.area.end(), 0.0f);
    for (int row = 0; row < static_cast<int>(unifontRows); ++row) {
        const double top = baselineY + (static_cast<double>(row) - unifontBaselineRows) * texelY;
        const double bottom = top + texelY;
        const double shift = slant * (baselineY - 0.5 * (top + bottom));
        const int firstY = std::max(0, static_cast<int>(std::floor(top)));
        const int endY = std::min(height, static_cast<int>(std::ceil(bottom)));
        for (int column = 0; column <= glyph.width; ++column) {
            if (!isSet(row, column) && !(isEmboldened && isSet(row, column - 1)))
                continue;
            const double left = penX + static_cast<double>(column) * texelX + shift;
            const double right = left + texelX;
            const int firstX = std::max(0, static_cast<int>(std::floor(left)));
            const int endX = std::min(width, static_cast<int>(std::ceil(right)));
            for (int y = firstY; y < endY; ++y) {
                const double overlapY = std::min(bottom, y + 1.0) - std::max(top, static_cast<double>(y));
                if (overlapY <= 0.0)
                    continue;
                float* areaRow = plane.area.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
                for (int x = firstX; x < endX; ++x) {
                    const double overlapX = std::min(right, x + 1.0) - std::max(left, static_cast<double>(x));
                    if (overlapX > 0.0)
                        areaRow[x] += static_cast<float>(overlapX * overlapY);
                }
            }
        }
    }
    for (std::size_t index = 0; index < plane.coverage.size(); ++index) {
        const auto value = static_cast<std::uint8_t>(std::lround(std::min(plane.area[index], 1.0f) * 255.0f));
        plane.coverage[index] = std::max(plane.coverage[index], value);
    }
}

/// Draws one code point in the face its style resolved to: the sans-serif
/// face's outline where it has one, GNU Unifont's glyph otherwise.
void drawCodePoint(std::uint32_t codePoint, const RichText::DrawnStyle& style, double emPixels, double horizontalScale, double penX, double baselineY, Plane& plane)
{
    if (style.familyClass != FamilyClass::Monospace) {
        const ProportionalStrike::Face face = strikeFace(style.styles);
        if (isSansGlyph(face, codePoint)) {
            (void)ProportionalOutline::rasterise(face, codePoint, emPixels, horizontalScale, penX, baselineY, plane.width(), plane.height(),
                plane.coverage, (style.styles & italicBit) ? obliqueSlant : 0.0);
            return;
        }
    }
    drawBitmap(unifontGlyph(codePoint), emPixels, horizontalScale, penX, baselineY, style.styles, plane);
}

/// Mixes a colour into a premultiplied pixel at a coverage byte.
void blendPremultiplied(std::uint8_t* pixel, std::array<std::uint8_t, 3> color, unsigned coverage)
{
    const unsigned inverse = 255 - coverage;
    for (std::size_t channel = 0; channel < 3; ++channel)
        pixel[channel] = static_cast<std::uint8_t>((color[channel] * coverage + pixel[channel] * inverse + 127) / 255);
    pixel[3] = static_cast<std::uint8_t>(coverage + (pixel[3] * inverse + 127) / 255);
}

/// Fills a rectangle of a raster at fractional edges, each pixel covered by
/// the share of it the rectangle covers: a decoration's rule.
void fillRule(RasterView raster, double left, double top, double right, double bottom, std::array<std::uint8_t, 3> color)
{
    const int firstY = std::max(0, static_cast<int>(std::floor(top)));
    const int endY = std::min(raster.height, static_cast<int>(std::ceil(bottom)));
    const int firstX = std::max(0, static_cast<int>(std::floor(left)));
    const int endX = std::min(raster.width, static_cast<int>(std::ceil(right)));
    for (int y = firstY; y < endY; ++y) {
        const double overlapY = std::min(bottom, y + 1.0) - std::max(top, static_cast<double>(y));
        for (int x = firstX; x < endX; ++x) {
            const double overlapX = std::min(right, x + 1.0) - std::max(left, static_cast<double>(x));
            const auto coverage = static_cast<unsigned>(std::lround(std::clamp(overlapX * overlapY, 0.0, 1.0) * 255.0));
            if (coverage)
                blendPremultiplied(raster.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.width) + static_cast<std::size_t>(x)) * 4, color, coverage);
        }
    }
}

[[nodiscard]] std::array<std::uint8_t, 3> colorOf(const RichText::DrawnStyle& style, const RichText::BlockQuad& quad)
{
    if (style.isAutomaticColor)
        return { quad.red, quad.green, quad.blue };
    return { style.red, style.green, style.blue };
}

/// Draws a block's graphemes and decorations at Core's positions into its
/// raster. Every grapheme is set on its own, at the pen Core gave it, so
/// the drawn glyph, its highlight and the character a click names agree.
void drawBlock(const RichText::BlockQuad& quad, const RichText::BlockDrawing& drawing, double offsetX, double offsetY, RasterView raster)
{
    const auto styles = drawing.styles();
    const auto text = drawing.text();
    const double unitsX = quad.pixelsPerUnitX;
    const double unitsY = quad.pixelsPerUnitY;
    const double pixelsPerPoint = unitsY * unitsPerPoint;
    // The em is sized down the cell and the pen placed across it, so a cell
    // whose two pixel densities differ keeps glyphs at Core's positions.
    const double horizontalScale = unitsY > 0.0 ? unitsX / unitsY : 1.0;
    Plane plane;
    const std::array<std::uint32_t, 1> ellipsis { 0x2026 };
    for (const RichText::DrawnCluster& cluster : drawing.clusters()) {
        if (cluster.style >= styles.size())
            continue;
        const RichText::DrawnStyle& style = styles[cluster.style];
        const std::span<const std::uint32_t> codePoints = cluster.isEllipsis
            ? std::span<const std::uint32_t>(ellipsis)
            : (cluster.first < cluster.end && cluster.end <= text.size() ? text.subspan(cluster.first, cluster.end - cluster.first) : std::span<const std::uint32_t> {});
        const double emPixels = static_cast<double>(style.halfPoints) / 2.0 * pixelsPerPoint;
        if (codePoints.empty() || emPixels <= 0.0)
            continue;
        const double penX = static_cast<double>(cluster.x) * unitsX + offsetX;
        const double baselineY = static_cast<double>(cluster.baseline) * unitsY + offsetY;
        // Every glyph a face can draw stays inside this box: an em left of
        // the pen and two right of it, one and a half above the baseline and
        // three quarters below, the oblique's lean included.
        const double reach = emPixels * std::max(horizontalScale, 1.0);
        plane.box = intersection({ static_cast<int>(std::floor(penX - reach)), static_cast<int>(std::floor(baselineY - 1.5 * emPixels)),
                                     static_cast<int>(std::ceil(penX + 2.0 * reach)), static_cast<int>(std::ceil(baselineY + 0.75 * emPixels)) },
            { 0, 0, raster.width, raster.height });
        if (plane.box.isEmpty())
            continue;
        const std::size_t planeSize = static_cast<std::size_t>(plane.width()) * static_cast<std::size_t>(plane.height());
        plane.coverage.assign(planeSize, 0);
        plane.area.resize(planeSize);
        bool hasInk = false;
        for (const std::uint32_t codePoint : codePoints) {
            if (codePoint == '\t' || RichText::isLineBreak(codePoint) || Unicode::Width::isInvisibleFormat(codePoint))
                continue;
            drawCodePoint(codePoint, style, emPixels, horizontalScale, penX - plane.box.left, baselineY - plane.box.top, plane);
            hasInk = true;
        }
        if (!hasInk)
            continue;
        const std::array<std::uint8_t, 3> color = colorOf(style, quad);
        for (int y = 0; y < plane.height(); ++y) {
            const std::uint8_t* row = plane.coverage.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.width());
            std::uint8_t* destination = raster.pixels.data() + (static_cast<std::size_t>(plane.box.top + y) * static_cast<std::size_t>(raster.width) + static_cast<std::size_t>(plane.box.left)) * 4;
            for (int x = 0; x < plane.width(); ++x) {
                if (row[x])
                    blendPremultiplied(destination + static_cast<std::size_t>(x) * 4, color, row[x]);
            }
        }
    }
    // One rule per decorated stretch, never per grapheme: two abutting
    // antialiased rules leave a seam where their shared edge falls inside a
    // pixel.
    for (const RichText::DrawnRule& rule : drawing.rules()) {
        if (rule.style >= styles.size())
            continue;
        const RichText::DrawnStyle& style = styles[rule.style];
        const FaceDesign& design = designOf(style.familyClass, style.styles);
        const double emPixels = static_cast<double>(style.halfPoints) / 2.0 * pixelsPerPoint;
        const double thickness = std::max(1.0, design.ruleThickness * emPixels);
        const double left = static_cast<double>(rule.x) * unitsX + offsetX;
        const double right = left + static_cast<double>(rule.width) * unitsX;
        const double baselineY = static_cast<double>(rule.baseline) * unitsY + offsetY;
        const std::array<std::uint8_t, 3> color = colorOf(style, quad);
        if (style.styles & underlineBit) {
            const double centre = baselineY + design.underlinePosition * emPixels;
            fillRule(raster, left, centre - thickness / 2.0, right, centre + thickness / 2.0, color);
        }
        if (style.styles & strikethroughBit) {
            const double centre = baselineY - design.xHeight / 2.0 * emPixels;
            fillRule(raster, left, centre - thickness / 2.0, right, centre + thickness / 2.0, color);
        }
    }
}

/// The frame's pixels, and the chrome no block covers.
struct Target {
    std::span<std::uint8_t> rgba;
    IntSize surface;
    std::span<const IntRect> occlusions;

    /// The pixel at a position, or null where chrome covers it.
    [[nodiscard]] std::uint8_t* pixel(int x, int y) const
    {
        for (const IntRect& rect : occlusions) {
            if (x >= rect.origin.x && x < rect.origin.x + rect.size.width && y >= rect.origin.y && y < rect.origin.y + rect.size.height)
                return nullptr;
        }
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width) + static_cast<std::size_t>(x)) * 4;
    }

    [[nodiscard]] PixelBox bounds() const { return { 0, 0, surface.width, surface.height }; }
};

/// Fills a box of the frame with a colour at an opacity.
void fill(const Target& target, PixelBox box, float red, float green, float blue, float alpha)
{
    box = intersection(box, target.bounds());
    const float opacity = std::clamp(alpha, 0.0f, 1.0f);
    const std::array<float, 3> color { red * 255.0f, green * 255.0f, blue * 255.0f };
    for (int y = box.top; y < box.bottom; ++y) {
        for (int x = box.left; x < box.right; ++x) {
            std::uint8_t* destination = target.pixel(x, y);
            if (!destination)
                continue;
            for (std::size_t channel = 0; channel < 3; ++channel)
                destination[channel] = static_cast<std::uint8_t>(std::lround(std::clamp(color[channel] * opacity + destination[channel] * (1.0f - opacity), 0.0f, 255.0f)));
            destination[3] = static_cast<std::uint8_t>(std::lround(255.0f * opacity + destination[3] * (1.0f - opacity)));
        }
    }
}

} // namespace

RichText::TypographyFacts BundledRichText::typographyFacts(const CellMetrics& cell, int unifontScale, double pixelsPerPoint)
{
    const double perPoint = pixelsPerPoint > 0.0 ? pixelsPerPoint : 1.0;
    const auto scale = static_cast<double>(std::max(unifontScale, 1));
    RichText::TypographyFacts facts;
    facts.cellWidth = inSixtyFourths(cell.cellWidth / perPoint);
    facts.cellHeight = inSixtyFourths(cell.cellHeight / perPoint);
    facts.baseline = std::min(facts.cellHeight, inSixtyFourths(cell.baselineFromTop / perPoint));
    // The sizes a `g=1` run follows: the proportional strike's em and GNU
    // Unifont's sixteen-row em, each at the grid's scale.
    facts.proportionalSize = inSixtyFourths(static_cast<double>(ProportionalStrike::emPixelsPerScale) * scale / perPoint);
    facts.monospaceSize = inSixtyFourths(unifontRows * scale / perPoint);
    facts.shaping = 0;
    facts.rendersBlocks = 1;
    for (const FamilyClass familyClass : { FamilyClass::SansSerif, FamilyClass::Monospace }) {
        for (std::uint8_t styles = 0; styles <= (boldBit | italicBit); ++styles)
            addFace(facts, familyClass, styles);
    }
    return facts;
}

void BundledRichText::composite(const TerminalFrameInputs& frame, std::span<std::uint8_t> rgba, IntSize surface, std::span<const IntRect> occlusions)
{
    for (auto& entry : m_rasters)
        entry.second.isSeen = false;
    const auto quads = frame.richTextQuads();
    const bool hasSurface = surface.width > 0 && surface.height > 0
        && rgba.size() >= static_cast<std::size_t>(surface.width) * static_cast<std::size_t>(surface.height) * 4;
    if (!hasSurface || quads.empty()) {
        m_rasters.clear();
        return;
    }
    const Target target { rgba, surface, occlusions };
    const CellMetrics& cell = frame.metrics();

    // Each block's raster sits on whole device pixels, its rectangle's origin
    // rounded, so it lands one texel per pixel; the fraction rounding moved
    // it is carried into the positions drawn in it.
    struct Placed {
        PixelBox raster;
        std::vector<PixelBox> clips;
        const Raster* pixels = nullptr;
    };
    std::vector<Placed> placed(quads.size());
    std::size_t retainedBytes = 0;
    for (std::size_t index = 0; index < quads.size(); ++index) {
        const RichText::BlockQuad& quad = quads[index];
        const RichText::BlockDrawing& drawing = frame.richTextDrawing(index);
        const auto left = static_cast<int>(std::lround(quad.left));
        const auto top = static_cast<int>(std::lround(quad.top));
        const auto width = static_cast<int>(std::lround(quad.width));
        const auto height = static_cast<int>(std::lround(quad.height));
        if (width <= 0 || height <= 0)
            continue;
        const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (bytes > maxRetainedBytes - retainedBytes)
            continue;
        retainedBytes += bytes;
        Placed& block = placed[index];
        block.raster = { left, top, left + width, top + height };
        // The block's visible rectangles, each edge rounded once, or its
        // whole rectangle when it names none.
        if (drawing.visibleRects().empty()) {
            block.clips.push_back(intersection(block.raster, target.bounds()));
        } else {
            for (const RichText::VisibleRect& visible : drawing.visibleRects()) {
                const PixelBox box {
                    static_cast<int>(std::lround(quad.left + static_cast<float>(visible.x) * cell.cellWidth)),
                    static_cast<int>(std::lround(quad.top + static_cast<float>(visible.y) * cell.cellHeight)),
                    static_cast<int>(std::lround(quad.left + static_cast<float>(visible.x + visible.width) * cell.cellWidth)),
                    static_cast<int>(std::lround(quad.top + static_cast<float>(visible.y + visible.height) * cell.cellHeight)),
                };
                block.clips.push_back(intersection(intersection(box, block.raster), target.bounds()));
            }
        }
        Raster& raster = m_rasters[quad.serial];
        raster.isSeen = true;
        if (raster.pixels.empty() || raster.key != quad.contentKey || raster.width != width || raster.height != height) {
            raster.key = quad.contentKey;
            raster.width = width;
            raster.height = height;
            raster.pixels.assign(bytes, 0);
            drawBlock(quad, drawing, static_cast<double>(quad.left) - left, static_cast<double>(quad.top) - top, { raster.pixels, width, height });
            ++m_rasterCount;
        }
        block.pixels = &raster;
    }

    // The `\highlight` backgrounds first, under the selection and the text,
    // each cluster's edges rounded on their own so two touching runs share a
    // device-pixel boundary.
    for (std::size_t index = 0; index < quads.size(); ++index) {
        const Placed& block = placed[index];
        if (!block.pixels)
            continue;
        const RichText::BlockQuad& quad = quads[index];
        const RichText::BlockDrawing& drawing = frame.richTextDrawing(index);
        const auto styles = drawing.styles();
        for (const RichText::DrawnCluster& cluster : drawing.clusters()) {
            if (cluster.style >= styles.size() || !styles[cluster.style].hasBackground)
                continue;
            const RichText::DrawnStyle& style = styles[cluster.style];
            const PixelBox box {
                static_cast<int>(std::lround(quad.left + static_cast<float>(cluster.x) * quad.pixelsPerUnitX)),
                static_cast<int>(std::lround(quad.top + static_cast<float>(cluster.top) * quad.pixelsPerUnitY)),
                static_cast<int>(std::lround(quad.left + static_cast<float>(cluster.x + cluster.advance) * quad.pixelsPerUnitX)),
                static_cast<int>(std::lround(quad.top + static_cast<float>(cluster.top + cluster.height) * quad.pixelsPerUnitY)),
            };
            for (const PixelBox& clip : block.clips)
                fill(target, intersection(box, clip), style.backgroundRed / 255.0f, style.backgroundGreen / 255.0f, style.backgroundBlue / 255.0f, 1.0f);
        }
    }

    const auto drawHighlights = [&] {
        for (const RichText::HighlightQuad& highlight : frame.richTextHighlights()) {
            const PixelBox box {
                static_cast<int>(std::lround(highlight.left)),
                static_cast<int>(std::lround(highlight.top)),
                static_cast<int>(std::lround(highlight.left + highlight.width)),
                static_cast<int>(std::lround(highlight.top + highlight.height)),
            };
            fill(target, box, highlight.red, highlight.green, highlight.blue, highlight.alpha);
        }
    };
    if (frame.inputs().isSelectionBehindText)
        drawHighlights();
    for (const Placed& block : placed) {
        if (!block.pixels)
            continue;
        const Raster& raster = *block.pixels;
        for (const PixelBox& clip : block.clips) {
            for (int y = clip.top; y < clip.bottom; ++y) {
                const std::uint8_t* row = raster.pixels.data() + static_cast<std::size_t>(y - block.raster.top) * static_cast<std::size_t>(raster.width) * 4;
                for (int x = clip.left; x < clip.right; ++x) {
                    const std::uint8_t* source = row + static_cast<std::size_t>(x - block.raster.left) * 4;
                    if (!source[3])
                        continue;
                    std::uint8_t* destination = target.pixel(x, y);
                    if (!destination)
                        continue;
                    const unsigned inverse = 255u - source[3];
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        destination[channel] = static_cast<std::uint8_t>(source[channel] + (destination[channel] * inverse + 127) / 255);
                }
            }
        }
    }
    if (!frame.inputs().isSelectionBehindText)
        drawHighlights();
    std::erase_if(m_rasters, [](const auto& entry) { return !entry.second.isSeen; });
}

} // namespace Ubiquitty::Rendering
