// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/CellInstanceBuilder.cpp — see CellInstanceBuilder.h. The
// portable per-frame Cell -> GPU-instance build, using neutral Vec2/Vec4 PODs
// and the glyph atlas behind GlyphBackend. Two passes: cell-background
// (background + glyph atlas UV + focused-cursor bake + wide cells) and
// decoration (underline styles + skip-ink, strike, overline, framed/encircled).
// No GPU / platform / simd types.

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/CellPen.h"
#include "Core/CellScan.h"
#include "Core/Rendering/CellInstanceScratch.h"

#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxFillEmit.h"
#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/DRCSRaster.h"
#include "Core/Rendering/LetterLayout.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/ProportionalRun.h"
#include "Core/Rendering/RuleGeometry.h"
#include "Core/Rendering/SubpixelPhase.h"

#include "Core/RenderCell.h"
#include "Core/RowMeta.h"
#include "Core/Screen/MarkValues.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "PAL/Log.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Ubiquitty::Rendering {

namespace {

/// Which frame facts the cell walk is compiled to hold as constants.
enum class WalkClass : std::uint8_t {
    /// No selection or search in the cells, no native vibrancy, no occluding
    /// letterbox, no live text-size spec.
    Quiet,
    /// Quiet but for a selection drawn in the cells.
    Selected,
    /// Every fact read at run time.
    General,
};

template<WalkClass value>
using WalkClassConstant = std::integral_constant<WalkClass, value>;

float atlasToDeviceScale(const CellMetrics& metrics)
{
    return metrics.atlasToDevicePixels > 0.0f ? metrics.atlasToDevicePixels : 1.0f;
}

void emitSizedTextRun(const BuildInputs& in, const CellMetrics& metrics, const GlyphBackend& atlas,
    const Cell& cell, const TextSizeSpec& spec, float left, float top, float width,
    FontFace face, std::uint32_t weight, std::uint32_t variant, Vec4 foreground, Vec4 background,
    const InstanceBuffers& out, InstanceCounts& counts)
{
    if (!in.clusterResolver)
        return;
    std::vector<std::uint32_t> text(runTextMaximumCodePoints);
    const int count = in.clusterResolver(&cell, text, in.clusterResolverContext);
    if (count <= 0 || count > runTextMaximumCodePoints)
        return;
    const float blockHeight = metrics.cellHeight * spec.scale;
    const float blockTop = top - penTextBand(&cell.pen) * metrics.cellHeight;
    const auto area = fractionalRenderArea(spec, { width, blockHeight });
    const float step = width / spec.widthCells * area.scale;
    const float height = blockHeight * area.scale;
    float x = left + area.offset.x;
    const float y = blockTop + area.offset.y;
    const auto emit = [&](std::span<const std::uint32_t> cluster) {
        int cells = std::max(1, Unicode::Width::forCodePoint(cluster.front(), in.ambiguousWidth));
        bool isEmoji = Unicode::Width::isEmojiPresentation(cluster.front());
        for (const auto codePoint : cluster) {
            if (Unicode::isExtendedPictographic(cluster.front()) && codePoint == Unicode::Width::variationSelector15)
                isEmoji = false;
            if ((Unicode::isExtendedPictographic(cluster.front()) && codePoint == Unicode::Width::variationSelector16)
                || (Unicode::isKeycapBase(cluster.front()) && codePoint == 0x20E3)) {
                isEmoji = true;
                cells = 2;
            }
        }
        const float right = x + cells * step;
        const float clippedLeft = std::max(x, left);
        const float clippedRight = std::min(right, left + width);
        const float clippedTop = std::max(y, top);
        const float clippedBottom = std::min(y + height, top + metrics.cellHeight);
        if (clippedLeft < clippedRight && clippedTop < clippedBottom) {
            AtlasGlyph glyph;
            GlyphInstance* instances = out.glyphs;
            std::size_t capacity = out.glyphCapacity;
            std::size_t* total = &counts.glyphs;
            if (isEmoji && atlas.colorGlyph && out.colorGlyphs) {
                glyph = atlas.colorGlyph(atlas.context, cluster, 0);
                instances = out.colorGlyphs;
                capacity = out.colorGlyphCapacity;
                total = &counts.colorGlyphs;
            } else if (cells == 2 && out.wideGlyphs) {
                if (cluster.size() > 1 && atlas.wideClusterGlyph)
                    glyph = atlas.wideClusterGlyph(atlas.context, cluster, face, weight, variant);
                else if (atlas.wideGlyph)
                    glyph = atlas.wideGlyph(atlas.context, cluster.front(), face);
                instances = out.wideGlyphs;
                capacity = out.wideGlyphCapacity;
                total = &counts.wideGlyphs;
            } else if (cluster.size() > 1 && atlas.clusterGlyph) {
                glyph = atlas.clusterGlyph(atlas.context, cluster, face, weight, variant);
            } else if (atlas.monoGlyph) {
                glyph = atlas.monoGlyph(atlas.context, cluster.front(), face, weight, variant);
            }
            if (glyph.present && instances) {
                const auto index = (*total)++;
                if (index < capacity) {
                    auto& instance = instances[index];
                    instance.originInPixels = { clippedLeft, clippedTop };
                    instance.sizeInPixels = { clippedRight - clippedLeft, clippedBottom - clippedTop };
                    instance.foreground = foreground;
                    instance.background = background;
                    const float unitX = (glyph.maxUV.x - glyph.minUV.x) / (right - x);
                    const float unitY = (glyph.maxUV.y - glyph.minUV.y) / height;
                    instance.atlasTopLeft = { glyph.minUV.x + (clippedLeft - x) * unitX,
                        glyph.minUV.y + (clippedTop - y) * unitY };
                    instance.atlasBottomRight = { glyph.maxUV.x - (right - clippedRight) * unitX,
                        glyph.maxUV.y - (y + height - clippedBottom) * unitY };
                }
            }
        }
        x = right;
    };
    Unicode::GraphemeState state {};
    Unicode::graphemeStateReset(&state, text.front());
    std::size_t first = 0;
    for (std::size_t at = 1; at < static_cast<std::size_t>(count); ++at) {
        if (Unicode::graphemeExtends(&state, text[at - 1], text[at]))
            continue;
        emit(std::span<const std::uint32_t>(text).subspan(first, at - first));
        first = at;
        Unicode::graphemeStateReset(&state, text[at]);
    }
    emit(std::span<const std::uint32_t>(text).subspan(first, static_cast<std::size_t>(count) - first));
}

/// Cuts everything one cell emitted at a displaced band's edge.
///
/// A cell that a band standing away from its storage covers in part keeps the
/// side the band leaves. The cut is applied once, when the cell's walk step
/// ends, to every instance the step appended in every stream, so no stream —
/// the cell quads, the glyph sheets, the rules — can draw under the band, and
/// none needs a clip of its own. A quad is narrowed with its atlas rect cut in
/// proportion, so the part kept samples exactly the texels it showed before.
/// An overlay's shape is defined over its whole rect and cannot be narrowed,
/// so one the edge crosses is dropped.
class BandEdgeCut {
public:
    BandEdgeCut() = default;
    BandEdgeCut(const BandEdgeCut&) = delete;
    BandEdgeCut& operator=(const BandEdgeCut&) = delete;

    /// Keeps what lies left of `edge`, device pixels.
    void keepLeftOf(float edge)
    {
        m_keptLeft = -std::numeric_limits<float>::infinity();
        m_keptRight = edge;
    }

    /// Keeps what lies right of `edge`, device pixels.
    void keepRightOf(float edge)
    {
        m_keptLeft = edge;
        m_keptRight = std::numeric_limits<float>::infinity();
    }

    /// Records where each stream stands before the cell emits anything.
    ///
    /// - Parameters:
    ///   - out: The streams the cell writes.
    ///   - counts: The counts the cell advances.
    ///   - gridLeft: The grid's left edge, device pixels.
    ///   - cellWidth: One column, device pixels.
    void begin(const InstanceBuffers& out, InstanceCounts& counts, float gridLeft, float cellWidth)
    {
        m_out = &out;
        m_counts = &counts;
        m_start = counts;
        m_gridLeft = gridLeft;
        m_cellWidth = cellWidth;
    }

    ~BandEdgeCut()
    {
        if (!m_out)
            return;
        const InstanceBuffers& out = *m_out;
        const InstanceCounts& counts = *m_counts;
        cutCells(out.cells, m_start.cells, std::min(counts.cells, out.cellCapacity));
        cutGlyphs(out.glyphs, m_start.glyphs, std::min(counts.glyphs, out.glyphCapacity));
        cutGlyphs(out.wideGlyphs, m_start.wideGlyphs, std::min(counts.wideGlyphs, out.wideGlyphCapacity));
        cutGlyphs(out.proportionalGlyphs, m_start.proportionalGlyphs, std::min(counts.proportionalGlyphs, out.proportionalGlyphCapacity));
        cutGlyphs(out.proportionalStrips, m_start.proportionalStrips, std::min(counts.proportionalStrips, out.proportionalStripCapacity));
        cutGlyphs(out.colorGlyphs, m_start.colorGlyphs, std::min(counts.colorGlyphs, out.colorGlyphCapacity));
        cutGlyphs(out.ligatureGlyphs, m_start.ligatureGlyphs, std::min(counts.ligatureGlyphs, out.ligatureGlyphCapacity));
        cutGlyphs(out.doubleGlyphs, m_start.doubleGlyphs, std::min(counts.doubleGlyphs, out.doubleGlyphCapacity));
        cutDecorations(out.decorations, m_start.decorations, std::min(counts.decorations, out.decorationCapacity));
        dropCrossedOverlays(out.overlays, m_start.overlays, std::min(counts.overlays, out.overlayCapacity));
        dropCrossedOverlays(out.boxFills, m_start.boxFills, std::min(counts.boxFills, out.boxFillCapacity));
    }

private:
    struct Span {
        float left;
        float right;
    };

    /// - Returns: The part of `[left, right)` kept, empty when none is.
    [[nodiscard]] Span kept(float left, float right) const { return { std::max(left, m_keptLeft), std::min(right, m_keptRight) }; }

    void cutCells(CellInstance* cells, std::size_t begin, std::size_t end) const
    {
        for (std::size_t index = begin; cells && index < end; ++index) {
            CellInstance& cell = cells[index];
            const float left = m_gridLeft + cell.gridPosition.x * m_cellWidth;
            const float right = left + cell.widthCells * m_cellWidth;
            const Span part = kept(left, right);
            if (part.right <= part.left) {
                cell.widthCells = 0.0f;
                continue;
            }
            const float width = right - left;
            const float span = cell.atlasBottomRight.x - cell.atlasTopLeft.x;
            const float uLeft = cell.atlasTopLeft.x + span * (part.left - left) / width;
            const float uRight = cell.atlasTopLeft.x + span * (part.right - left) / width;
            cell.gridPosition.x = (part.left - m_gridLeft) / m_cellWidth;
            cell.widthCells = (part.right - part.left) / m_cellWidth;
            cell.atlasTopLeft.x = uLeft;
            cell.atlasBottomRight.x = uRight;
        }
    }

    void cutGlyphs(GlyphInstance* glyphs, std::size_t begin, std::size_t end) const
    {
        for (std::size_t index = begin; glyphs && index < end; ++index) {
            GlyphInstance& glyph = glyphs[index];
            const float left = glyph.originInPixels.x;
            const float right = left + glyph.sizeInPixels.x;
            const Span part = kept(left, right);
            if (part.right <= part.left) {
                glyph.sizeInPixels = { 0.0f, 0.0f };
                glyph.atlasBottomRight = glyph.atlasTopLeft;
                continue;
            }
            const float width = right - left;
            const float span = glyph.atlasBottomRight.x - glyph.atlasTopLeft.x;
            const float uLeft = glyph.atlasTopLeft.x + span * (part.left - left) / width;
            const float uRight = glyph.atlasTopLeft.x + span * (part.right - left) / width;
            glyph.originInPixels.x = part.left;
            glyph.sizeInPixels.x = part.right - part.left;
            glyph.atlasTopLeft.x = uLeft;
            glyph.atlasBottomRight.x = uRight;
        }
    }

    void cutDecorations(DecorationInstance* decorations, std::size_t begin, std::size_t end) const
    {
        for (std::size_t index = begin; decorations && index < end; ++index) {
            DecorationInstance& decoration = decorations[index];
            const Span part = kept(decoration.topLeftInPixels.x, decoration.bottomRightInPixels.x);
            decoration.topLeftInPixels.x = part.left;
            decoration.bottomRightInPixels.x = std::max(part.left, part.right);
        }
    }

    void dropCrossedOverlays(OverlayInstance* overlays, std::size_t begin, std::size_t end) const
    {
        for (std::size_t index = begin; overlays && index < end; ++index) {
            OverlayInstance& overlay = overlays[index];
            const Span part = kept(overlay.topLeftInPixels.x, overlay.bottomRightInPixels.x);
            if (part.left > overlay.topLeftInPixels.x || part.right < overlay.bottomRightInPixels.x)
                overlay.bottomRightInPixels = overlay.topLeftInPixels;
        }
    }

    const InstanceBuffers* m_out = nullptr;
    InstanceCounts* m_counts = nullptr;
    InstanceCounts m_start {};
    float m_gridLeft = 0.0f;
    float m_cellWidth = 1.0f;
    float m_keptLeft = -std::numeric_limits<float>::infinity();
    float m_keptRight = std::numeric_limits<float>::infinity();
};

float resolvedBackgroundOpacity(Vec4 background, const BuildInputs& inputs)
{
    float opacity = inputs.backgroundOpacity * inputs.uniformBackgroundOpacity;
    const auto& defaultBackground = inputs.uniformDefaultBackground;
    const float red = background.x - defaultBackground.x;
    const float green = background.y - defaultBackground.y;
    const float blue = background.z - defaultBackground.z;
    if (red * red + green * green + blue * blue < 1e-6f) {
        opacity *= defaultBackground.w;
    } else if (inputs.preservesBackgroundTints || inputs.usesNativeBackgroundVibrancy) {
        // Explicit SGR backgrounds carry interface state even when the theme
        // tint is clear. Their distance from that tint preserves continuous
        // ramps without imposing an opacity floor on nearly equal colors.
        const float distance = std::max({ std::abs(red), std::abs(green), std::abs(blue) });
        opacity = std::lerp(opacity, 1.0f, distance);
    }
    return opacity;
}

// The frame's flag is the walk's copy of `usesNativeBackgroundVibrancy`, held
// where the cell loop keeps it rather than reloaded through `inputs` per cell.
Vec4 nativeBackgroundInk(bool usesNativeBackgroundVibrancy, Vec4 background, const BuildInputs& inputs, bool isAllowed = true) // bool-ok: the frame's flag and the cell's eligibility compose
{
    if (usesNativeBackgroundVibrancy && isAllowed && background.w > 0) {
        const auto& base = inputs.uniformDefaultBackground;
        const float difference = std::abs(background.x - base.x) + std::abs(background.y - base.y) + std::abs(background.z - base.z);
        if (difference > 0.015f) {
            background.w = -background.w;
        }
    }
    return background;
}

/// Which sizing the frame's face offers.
///
/// One reading of ``BuildInputs::isBitmapFace`` rather than the same test at
/// each scripted site, so the three cannot come to disagree about a frame.
///
/// - Parameter in: The frame's inputs.
/// - Returns: ``GlyphSizing::FixedRaster`` for the tier Core rasterises itself.
constexpr GlyphSizing glyphSizingFor(const BuildInputs& in)
{
    return in.isBitmapFace ? GlyphSizing::FixedRaster : GlyphSizing::AnySize;
}

// The 256 answers to `byte / 255.0f`, which is six divides per cell where it is
// spelled out. Generated by the division it replaces, so every entry is the
// value that expression produces rather than one close to it.
constexpr std::array<float, 256> unitFromByte = [] {
    std::array<float, 256> table {};
    for (int i = 0; i < 256; ++i)
        table[static_cast<std::size_t>(i)] = static_cast<float>(i) / 255.0f;
    return table;
}();

Vec4 toVec4(RGB8 c) { return { unitFromByte[c.red], unitFromByte[c.green], unitFromByte[c.blue], 1.0f }; }

// A negative ink alpha routes a glyph exclusively to the native secondary plane.
// Its magnitude is that plane's coverage; ordinary frontends never request it.
// The vibrant half, out of line: only a frame drawing native text vibrancy
// reaches it.
[[gnu::noinline]] Vec4 nativeSecondaryInkVibrant(Vec4 foreground, Vec4 background, const BuildInputs& inputs, RGB8 defaultForeground)
{
    const float chroma = std::max({ foreground.x, foreground.y, foreground.z }) - std::min({ foreground.x, foreground.y, foreground.z });
    const float backgroundDifference = std::abs(background.x - inputs.uniformDefaultBackground.x)
        + std::abs(background.y - inputs.uniformDefaultBackground.y) + std::abs(background.z - inputs.uniformDefaultBackground.z);
    if (chroma > 0.04f && backgroundDifference <= 0.015f && background.w < 1 && foreground.w > 0) {
        // Colored ink contributes its RGB to native additive/darken blending;
        // applying a hierarchical mask would discard the requested hue.
        return { foreground.x, foreground.y, foreground.z, -foreground.w };
    }
    const float coverage = secondaryTextCoverage(foreground, background, toVec4(defaultForeground), inputs.uniformDefaultBackground);
    if (coverage > 0) {
        const float position = coverage / foreground.w;
        const float identity = defaultForeground.red > inputs.uniformDefaultBackground.x * 255 ? 0.0f : 1.0f;
        const Vec4 invisible { identity, identity, identity, 1 };
        const Vec4 primary { 1 - identity, 1 - identity, 1 - identity, 1 };
        const Vec4 low = position < 0.3f ? invisible : position < 0.6f ? inputs.nativeTertiaryInk
                                                                       : inputs.nativeSecondaryInk;
        const Vec4 high = position < 0.3f ? inputs.nativeTertiaryInk : position < 0.6f ? inputs.nativeSecondaryInk
                                                                                       : primary;
        const float fraction = position < 0.3f ? position / 0.3f : position < 0.6f ? (position - 0.3f) / 0.3f
                                                                                   : (position - 0.6f) / 0.4f;
        // Semantic colors anchor a continuous ramp; quantizing to label roles
        // destroys brightness animation in indexed and true-color text.
        const Vec4 semantic { std::lerp(low.x, high.x, fraction), std::lerp(low.y, high.y, fraction),
            std::lerp(low.z, high.z, fraction), std::lerp(low.w, high.w, fraction) };
        return { semantic.x, semantic.y, semantic.z, -foreground.w * semantic.w };
    }
    return foreground;
}

// Every instance write asks this, so the frame flag is tested inline and an
// ordinary frame makes no call. The flag is the walk's own, which the quiet
// walk holds as a constant false, so there the test folds away.
inline Vec4 nativeSecondaryInk(bool usesNativeTextVibrancy, Vec4 foreground, Vec4 background, const BuildInputs& inputs, RGB8 defaultForeground) // bool-ok: the frame's flag as the walk holds it
{
    if (!usesNativeTextVibrancy) [[likely]]
        return foreground;
    return nativeSecondaryInkVibrant(foreground, background, inputs, defaultForeground);
}

// Rec.709 linear luminance of one 8-bit sRGB channel value, the table the
// per-pen tier lookup reads. Built from SRGBGamma::linearize so the threshold
// a colour lands on is the one the composite's own curve places it at.
const std::array<float, 256>& linearFromByte()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t {};
        for (int i = 0; i < 256; ++i)
            t[static_cast<std::size_t>(i)] = static_cast<float>(SRGBGamma::linearize(i / 255.0));
        return t;
    }();
    return table;
}

std::uint32_t rasterTierForByte(int red, int green, int blue)
{
    const std::array<float, 256>& linear = linearFromByte();
    const double luma = 0.2126 * linear[static_cast<std::size_t>(red)] + 0.7152 * linear[static_cast<std::size_t>(green)] + 0.0722 * linear[static_cast<std::size_t>(blue)];
    return rasterTierForLinearLuma(luma);
}

std::uint32_t rasterTierFor(RGB8 c) { return rasterTierForByte(c.red, c.green, c.blue); }

// For a foreground an override already turned into floats (link tint,
// selection, search, the baked cursor): back to the byte the palette held,
// which is where every tint here came from.
std::uint32_t rasterTierFor(const Vec4& c)
{
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return rasterTierForByte(byte(c.x), byte(c.y), byte(c.z));
}

/// The contracted slot of a mono slot, whichever convention the backend keeps its table in.
///
/// The subpixel quad expands the rect it receives by the margins, so the rect
/// has to be the slot less them; every other convention is converted here, once,
/// rather than in each arm that crops or places a quad.
///
/// - Parameters:
///   - mono: The frame's mono binding.
///   - slot: The slot the glyph occupies.
/// - Returns: The slot contracted by its margins, headroom column included.
GlyphUV contractedSlotRect(const MonoAtlasBinding& mono, int slot)
{
    if (mono.slotUV == nullptr)
        return mono.geometry.atlasRectForBuilder(slot, mono.slotCapacity);
    const GlyphUV entry = mono.slotUV[slot];
    switch (mono.slotUVConvention) {
    case SlotRectConvention::ContractedSlot:
        return entry;
    case SlotRectConvention::CellBox: {
        const GlyphUV contracted = mono.geometry.atlasRectForBuilder(slot, mono.slotCapacity);
        return { entry.minU, entry.minV, entry.minU + (contracted.maxU - contracted.minU), entry.maxV };
    }
    case SlotRectConvention::PaddedSlot: {
        const float marginX = static_cast<float>(mono.geometry.glyphMarginInPixels.width);
        const float marginY = static_cast<float>(mono.geometry.glyphMarginInPixels.height);
        return { entry.minU + marginX, entry.minV + marginY, entry.maxU - marginX, entry.maxV - marginY };
    }
    case SlotRectConvention::MeasuredInk:
        return mono.geometry.atlasRectForBuilder(slot, mono.slotCapacity);
    }
    return mono.geometry.atlasRectForBuilder(slot, mono.slotCapacity);
}

/// One band's rows of a sized glyph's slot rect, and the overhang they keep.
struct SizedBandRows {
    /// The band's top edge in atlas pixels, margin included on the block's first band.
    float minV = 0.0f;
    /// The band's bottom edge in atlas pixels, margin included on the block's last band.
    float maxV = 0.0f;
    /// How far `minV` reaches above the band's cell box: the margin on the first band, else zero.
    float reachTop = 0.0f;
};

/// Cuts one band's rows out of a sized glyph's slot rect.
///
/// Bands partition the cell box, not the padded canvas: the margin belongs to
/// no band's share, and only the block's outer bands retain it, where the
/// glyph's overhang lives. The caller draws these rows into the band's own cell
/// row, backed off by `reachTop` at the magnification of its rows, so every
/// arm that crops a sized glyph places the result by the same numbers.
///
/// The bands partition the font's line, which is the first `lineShare` of the
/// cell box: the box is the line rounded up, and a band drawn at the magnified
/// box would reach past its row into the next band's.
///
/// - Parameters:
///   - slotMinV: The rect's top edge in atlas pixels.
///   - slotMaxV: The rect's bottom edge in atlas pixels.
///   - marginV: The margin the rect carries on each side, zero for a cell box.
///   - lineShare: The share of the cell box the font's line fills, one for a box with no rounding.
///   - scale: The block's rows.
///   - band: The band's row, already clamped below `scale`.
/// - Returns: The band's rows.
SizedBandRows sizedBandRows(float slotMinV, float slotMaxV, float marginV, float lineShare, unsigned scale, unsigned band)
{
    const float slice = (slotMaxV - slotMinV - 2.0f * marginV) * lineShare / static_cast<float>(scale);
    const float sliceTop = slotMinV + marginV + slice * static_cast<float>(band);
    const bool isFirst = band == 0;
    const bool isLast = band + 1 == scale;
    return { sliceTop - (isFirst ? marginV : 0.0f), sliceTop + slice + (isLast ? marginV : 0.0f), isFirst ? marginV : 0.0f };
}

/// The bottom edge of the rows one band shows of a window's rect.
///
/// The rect names the window's cell box; the band shows the share of it that
/// is the font's line, and the rows past that are the next band's.
///
/// - Parameters:
///   - minV: The cell box's top edge in atlas pixels.
///   - maxV: The cell box's bottom edge in atlas pixels.
///   - bandHeight: The rows the band shows, as ``sizedBandHeightInPixels`` gives them.
///   - cellBoxHeight: The rows the cell box holds.
/// - Returns: The band's bottom edge in atlas pixels.
float sizedBandBottom(float minV, float maxV, float bandHeight, float cellBoxHeight)
{
    const float share = cellBoxHeight > 0.0f ? bandHeight / cellBoxHeight : 1.0f;
    return minV + (maxV - minV) * share;
}

/// The run of a sized block's window rows one admission probes: rows
/// `[first, pastLast)` of `windowsPerRow` windows each.
///
/// A band-addressed arm's rows are the block's bands on the grid; a fractional
/// or colour arm's are its raster's rows, which do not map onto bands.
struct SizedWindowRows {
    int first = 0;
    int pastLast = 0;
    int windowsPerRow = 0;
};

/// The bands of a sized block that lie on the grid.
///
/// - Parameters:
///   - row: The grid row of the band being drawn.
///   - band: That band's index in its block.
///   - scale: The block's band count.
///   - rows: The grid's rows.
///   - windowsPerBand: The windows one band is cut into.
/// - Returns: The bands from the first on the grid to the last, top-down.
constexpr SizedWindowRows visibleSizedBands(int row, int band, int scale, int rows, int windowsPerBand)
{
    const int top = row - band;
    return { std::max(0, -top), std::min(scale, rows - top), windowsPerBand };
}

/// Which arm of the walk is asking ``SizedBlockWindows::admit`` about a block.
///
/// Part of a block's identity: the arms ask different sheets, or the same
/// sheet for different rects, so one arm's windows say nothing about another's.
enum class SizedBlockArm : std::uint8_t {
    DoubledLetter,
    DoubledCell,
    CellBaked,
    CellBakedDoubledRow,
    Subpixel,
    SubpixelDoubledRow,
    Wide,
    Fractional,
    FractionalWide,
    Colour,
};

/// The windows of the sized blocks one build draws sharp, each looked up once.
///
/// A block draws sharp whole or not at all. Its bands sit on different rows and
/// the walk reaches them a row at a time, so a sheet that fills between two of a
/// block's rows would otherwise leave its upper bands sharp and its lower bands
/// on the magnified crop. The first band the walk reaches therefore acquires
/// every window of every row the block shows, and the later bands draw from
/// what it acquired: the windows are slots touched this frame, which a
/// ``GlyphSlotCache`` never evicts within it, so the rects stay good for the
/// build. Each window is asked of the backend once a frame, as a band-by-band
/// walk asks it, and the verdict is what acquiring them found.
///
/// A block refused by the first band is refused at every band: its entry says
/// so. An entry another block displaced is acquired afresh and reaches the same
/// answer, because the slots answered before the refusal are hits the second
/// time and the slots the frame has not touched only ever grow fewer. A walk
/// over only the dirty rows acquires from whichever band it meets first.
///
/// Acquisition stops at the first absent window, and the slots it assigned
/// before it go back to the sheet (``GlyphSlotCache::forgetSlotsAssignedSince``):
/// a block that cannot fit holds no slot it will not draw from, so the cells
/// after it, its own 1x glyph among them, find the room it gave up. A null
/// sheet, or one the backend does not keep the windows on, gives nothing back
/// and changes no verdict.
///
/// Held per thread across builds, as the walk's other scratch is, so a build
/// in the steady state allocates nothing: ``beginBuild`` invalidates the
/// entries by a serial and keeps the storage.
class SizedBlockWindows {
public:
    /// Starts a build: every block is undecided again.
    void beginBuild()
    {
        ++m_build;
        m_windows.clear();
    }

    /// Acquires every window of a block, or finds the ones a band before this
    /// one acquired, and returns one row of them.
    ///
    /// - Parameters:
    ///   - sheet: The cache the backend keeps these windows in, if it binds one.
    ///   - topRow: The grid row of the block's band 0, negative when it has
    ///     scrolled off the top.
    ///   - column: The column the block's anchors stand in.
    ///   - arm: The arm asking.
    ///   - rows: The window rows the block shows.
    ///   - row: The row whose windows the caller draws; later rows follow it,
    ///     `rows.windowsPerRow` apart.
    ///   - fetch: Asks the backend for window `(row, window)`.
    /// - Returns: Row `row`'s windows, valid until the next ``admit``; null when
    ///   the block is refused or `row` is not one it shows.
    template<typename FetchWindow>
    [[nodiscard]] const AtlasGlyph* admit(GlyphSlotCache* sheet, int topRow, int column, SizedBlockArm arm, SizedWindowRows rows, int row, FetchWindow&& fetch)
    {
        if (column < 0 || row < rows.first || row >= rows.pastLast || rows.windowsPerRow <= 0 || rows.windowsPerRow > maxSizedWindowsPerBand)
            return nullptr;
        const std::size_t rowOffset = static_cast<std::size_t>(row - rows.first) * static_cast<std::size_t>(rows.windowsPerRow);
        if (static_cast<std::size_t>(column) >= m_entries.size())
            m_entries.resize(static_cast<std::size_t>(column) + 1);
        Entry& entry = m_entries[static_cast<std::size_t>(column)];
        if (entry.build == m_build && entry.topRow == topRow && entry.arm == arm && entry.first == rows.first && entry.pastLast == rows.pastLast
            && entry.windowsPerRow == rows.windowsPerRow)
            return entry.isSharp ? m_windows.data() + entry.offset + rowOffset : nullptr;
        const std::size_t offset = m_windows.size();
        const int mark = sheet ? sheet->slotsAssignedThisFrame() : 0;
        bool isEveryWindowPresent = true;
        for (int windowRow = rows.first; isEveryWindowPresent && windowRow < rows.pastLast; ++windowRow) {
            for (int window = 0; isEveryWindowPresent && window < rows.windowsPerRow; ++window) {
                const AtlasGlyph glyph = fetch(windowRow, window);
                isEveryWindowPresent = glyph.present;
                m_windows.push_back(glyph);
            }
        }
        if (!isEveryWindowPresent) {
            m_windows.resize(offset);
            if (sheet)
                sheet->forgetSlotsAssignedSince(mark);
        }
        entry = { m_build, topRow, arm, isEveryWindowPresent, rows.first, rows.pastLast, rows.windowsPerRow, offset };
        return isEveryWindowPresent ? m_windows.data() + offset + rowOffset : nullptr;
    }

private:
    struct Entry {
        std::uint32_t build = 0;
        int topRow = 0;
        SizedBlockArm arm = SizedBlockArm::CellBaked;
        bool isSharp = false;
        int first = 0;
        int pastLast = 0;
        int windowsPerRow = 0;
        std::size_t offset = 0;
    };

    // One entry a column: blocks never overlap, so the column's entry is the
    // block whose bands the walk is in until the next block down replaces it.
    std::vector<Entry> m_entries;
    std::vector<AtlasGlyph> m_windows;
    std::uint32_t m_build = 0;
};

/// Attaches the ink box the backend measured for a mono slot to its glyph.
///
/// Every glyph drawn from a mono slot reads its ink from the one table the
/// binding carries, so a quad that shrinks to the ink and a rule that follows it
/// past the cell box see the same box whichever entry filled the slot.
///
/// - Parameters:
///   - glyph: The slot's glyph, whose ink bounds this fills.
///   - mono: The frame's mono binding, which owns the table.
///   - slot: The slot the glyph occupies.
void attachMeasuredInk(AtlasGlyph& glyph, const MonoAtlasBinding& mono, int slot)
{
    // min > max is the unmeasured marker.
    if (mono.inkBoxForSlot == nullptr || slot < 0 || slot >= mono.slotCapacity)
        return;
    const GlyphInkBox& ink = mono.inkBoxForSlot[slot];
    if (ink.minInPixels.x > ink.maxInPixels.x)
        return;
    const AtlasSlotRegion slotBox = atlasSlotRegion(slot, mono.geometry.columns,
        mono.geometry.slotWidthInPixels, mono.geometry.slotHeightInPixels);
    glyph.hasInkBounds = true;
    glyph.inkMinUV = { static_cast<float>(slotBox.x) + ink.minInPixels.x,
        static_cast<float>(slotBox.y) + ink.minInPixels.y };
    glyph.inkMaxUV = { static_cast<float>(slotBox.x) + ink.maxInPixels.x,
        static_cast<float>(slotBox.y) + ink.maxInPixels.y };
}

/// Resolves the glyph a shaper chose to a mono slot, rasterising on a miss.
///
/// The hit path is Core's as it is for a code point: the key is derived here,
/// the cache probed here, and the backend asked only to draw into a slot it was
/// handed. That is what lets the slot's measured ink travel with the glyph, so
/// a healed letterform's quad and its underline see the ink a code point's
/// would.
///
/// A glyph id is already an identity rather than a hash of one, so the probe
/// needs no verification behind it.
///
/// - Parameters:
///   - glyphID: The glyph the shaper chose. Zero is `.notdef`, which keeps
///     the caller on its strip rather than drawing a box.
///   - variant: The raster variant, carrying the tier and the column's phase.
/// - Returns: The glyph with its ink bounds, or an absent glyph, which keeps
///   the caller on the strip it already has.
AtlasGlyph shapedGlyphForCell(const GlyphBackend& atlas, const MonoAtlasBinding& mono, std::uint32_t glyphID, FontFace face, std::uint32_t variant)
{
    if (glyphID == 0 || mono.cache == nullptr || atlas.rasteriseShapedGlyph == nullptr)
        return {};
    const std::uint64_t key = glyphSlotKey(shapedGlyphKey(glyphID, mono.weight, face, variantPhase(variant)), variant);
    bool isFresh = false;
    const int slot = mono.cache->slotForKey(key, &isFresh);
    if (slot < 0)
        return {};
    if (isFresh && !atlas.rasteriseShapedGlyph(atlas.context, glyphID, face, variant, slot)) {
        mono.cache->forgetKey(key);
        return {};
    }
    // The contracted-slot rect, which the subpixel quad expands by the margin.
    const GlyphUV uv = contractedSlotRect(mono, slot);
    AtlasGlyph glyph = { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
    attachMeasuredInk(glyph, mono, slot);
    return glyph;
}

/// Resolves a DECDLD soft-font cell to its atlas rect, rasterising on a miss.
///
/// The whole of the work is Core's except the upload: the matrix is on the wire,
/// ``rasterizeDRCSGlyph`` scales it onto the slot's cell box, and every number
/// that raster needs is in the geometry the backend already handed over. So the
/// backend is asked one thing — put these bytes in that slot — and a backend that
/// cannot returns false, which lands the cell on the background-only path an
/// undefined DRCS position already takes.
///
/// Keyed on the font's `serial` so a re-download, and a second pane's own soft
/// font, can never be served each other's pixels out of a shared atlas.
///
/// - Returns: The glyph box rect, or an absent glyph for a position this font
///   does not carry, a full atlas, or a backend that declined to stage.
AtlasGlyph softFontGlyph(const GlyphBackend& atlas, const MonoAtlasBinding& mono, const VT::SoftFont& font, int index)
{
    if (mono.cache == nullptr || !softFontHasGlyph(font, index))
        return {};

    // The bitmap decides identity on a hit. `drcsGlyphKey` carries 22 bits of the
    // font's serial, a byte stream can walk that all the way round, and the atlas
    // is shared across panes — so an unverified alias paints one pane's glyph
    // under another's name. Dimensions lead: two fonts can carry identical rows
    // under different geometry.
    //
    // Clamped rather than trusted: the parser's cap on the matrix is enforced a
    // long way from here, and its input is the wire.
    const auto matrixRows = std::min<std::size_t>(font.matrixHeight, VT::drcsMaxMatrixHeight);
    std::array<std::uint32_t, 1 + VT::drcsMaxMatrixHeight> identity {};
    identity.front() = (std::uint32_t { font.matrixWidth } << 8) | static_cast<std::uint32_t>(matrixRows);
    std::ranges::copy(std::span { font.glyph[index] }.first(matrixRows), identity.begin() + 1);

    const std::uint32_t key = drcsGlyphKey(font.serial, static_cast<std::uint32_t>(index));
    bool isFresh = false;
    const int slot = mono.cache->slotForKeyVerified(key, identity.data(), static_cast<int>(1 + matrixRows), &isFresh);
    if (slot < 0)
        return {};
    if (isFresh) {
        const GlyphSlotGeometry& geometry = mono.geometry;
        const int planeBytes = geometry.slotWidthInPixels * geometry.slotHeightInPixels;
        // Reused across frames and grown once: a DRCS raster runs per downloaded
        // glyph rather than per cell, and sizing a plane off the geometry is the
        // one thing here that cannot be done on the stack — a slot is as large as
        // the font and the backing scale make it.
        static thread_local std::vector<std::uint8_t> plane;
        if (planeBytes <= 0) {
            mono.cache->forgetKey(key);
            return {};
        }
        if (plane.size() < static_cast<std::size_t>(planeBytes))
            plane.resize(static_cast<std::size_t>(planeBytes));

        const IntRect cellBox { { geometry.glyphMarginInPixels.width, geometry.glyphMarginInPixels.height },
            { geometry.glyphWidthInPixels, geometry.slotHeightInPixels - 2 * geometry.glyphMarginInPixels.height } };
        const bool isDrawn = rasterizeDRCSGlyph(font, index, { geometry.slotWidthInPixels, geometry.slotHeightInPixels },
                                 cellBox, std::span<std::uint8_t>(plane.data(), static_cast<std::size_t>(planeBytes)))
            && atlas.stageMonoCoverage(atlas.context, slot, std::span<const std::uint8_t>(plane.data(), static_cast<std::size_t>(planeBytes)));
        if (!isDrawn) {
            // The slot was taken and never written. Left resident, every later
            // frame would be told this key is cached and would blit whatever the
            // evicted glyph left there — this cell's colours over another glyph's
            // ink, with every count in the frame correct.
            mono.cache->forgetKey(key);
            return {};
        }
    }
    const GlyphUV uv = mono.geometry.atlasRectForSlot(slot, mono.slotCapacity);
    return { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
}

/// One cell's glyph coverage, as the skip-ink cut samples it.
///
/// `inkMinUV`/`inkMaxUV` are the glyph BOX; `boxMinUV`/`boxMaxUV` the full slot
/// the dilation taps clamp to. Absent for a cell carrying no maskable ink.
enum class SkipInkSheet : std::uint8_t {
    Mono,
    ProportionalStrip,
    LigatureStrip,
    /// The dedicated proportional sheet: a run drawn glyph by glyph has each
    /// glyph's ink in a slot of its own, so a rule under it taps the slot of
    /// the glyph beneath each cell rather than one strip over the claim.
    Proportional,
    /// The two-cell sheet: each column of a wide glyph taps its own half of
    /// the glyph's slot, so a rule under CJK is cut by the strokes above it
    /// as a rule under Latin is by descenders.
    Wide,
};

struct SkipInkTap {
    Vec2 inkMinUV { 0.0f, 0.0f };
    Vec2 inkMaxUV { 0.0f, 0.0f };
    Vec2 boxMinUV { 0.0f, 0.0f };
    Vec2 boxMaxUV { 0.0f, 0.0f };
    bool present = false;
    /// The coverage sheet this tap reads. A shaped run draws a strip slice,
    /// not the glyph the mono sheet holds for its code point, and sampling the
    /// wrong sheet cuts a rule where no visible ink crosses it.
    SkipInkSheet sheet = SkipInkSheet::Mono;
};

/// The coverage tap for a column a ligature run covers: the cell's slice of
/// the strip, in the strip sheet.
///
/// The slice is the cell box at the grid pitch from the run's pen — the
/// seam's anchoring contract — between the slot's vertical margins, which is
/// the same box the mono tap names inside its own slot, so the band fractions
/// the builder derives map onto it unchanged. The clamp is the strip's inked
/// extent rather than the slice: a dilation reaching into the neighbouring
/// cell's slice reads the ink that is really there, which is what the
/// neighbour taps read on the mono path.
///
/// - Parameters:
///   - in: The frame's inputs.
///   - run: The run covering the column, with a present strip.
///   - column: The column.
///   - metrics: The frame's cell metrics.
/// - Returns: The tap, on the strip sheet.
SkipInkTap stripSkipInkTap(const BuildInputs& in, const LigatureRun& run, int column, const CellMetrics& metrics)
{
    SkipInkTap tap;
    const float scale = metrics.presentationScale > 0 && std::isfinite(metrics.presentationScale) ? metrics.presentationScale : 1;
    const float runStartX = in.originInPixels.x + run.startColumn * metrics.cellWidth;
    const float penX = in.subpixelGlyphs
        ? static_cast<float>(phaseForPen(runStartX, in.subpixelPhaseCount, in.subpixelPhaseRounding).snappedPenX)
        : runStartX;
    const float fullOriginX = penX - in.glyphMarginInPixels.width * scale;
    const float segmentLeft = (in.originInPixels.x + column * metrics.cellWidth - fullOriginX) / scale;
    const float segmentRight = segmentLeft + metrics.cellWidth / scale;
    const float slotHeight = in.glyphSlotHeightInPixels > 0.0f ? in.glyphSlotHeightInPixels : (run.strip.maxUV.y - run.strip.minUV.y);
    tap.inkMinUV = { run.strip.minUV.x + segmentLeft, run.strip.minUV.y + in.glyphMarginInPixels.height };
    tap.inkMaxUV = { run.strip.minUV.x + segmentRight, run.strip.minUV.y + slotHeight - in.glyphMarginInPixels.height };
    tap.boxMinUV = { run.strip.minUV.x, run.strip.minUV.y };
    tap.boxMaxUV = { run.strip.minUV.x + run.usedWidthInAtlasPixels, run.strip.maxUV.y };
    tap.present = true;
    tap.sheet = SkipInkSheet::LigatureStrip;
    return tap;
}

/// The phase count a two-cell glyph is keyed and blitted at.
///
/// - Parameter in: The frame's inputs.
/// - Returns: The wide page's own count where the backend names one, else the frame's.
int widePhaseCount(const BuildInputs& in)
{
    return in.widePhaseCount > 0 ? in.widePhaseCount : in.subpixelPhaseCount;
}

/// The phase count one two-cell glyph is keyed and blitted at.
///
/// - Parameters:
///   - in: The frame's inputs.
///   - atlas: The backend, which may name the glyph's own pen grid.
///   - codePoint: The glyph.
///   - face: The face it is drawn in.
/// - Returns: The glyph's own count where the backend names one, else the
///   wide page's.
int widePhaseCount(const BuildInputs& in, const GlyphBackend& atlas, std::uint32_t codePoint, FontFace face)
{
    if (atlas.wideGlyphPhaseCount) {
        const int own = atlas.wideGlyphPhaseCount(atlas.context, codePoint, face);
        if (own > 0)
            return std::min(own, 8);
    }
    return widePhaseCount(in);
}

/// The coverage tap for one column of a two-cell glyph: that column's slice of
/// the glyph's slot on the wide sheet.
///
/// Only on a phased frame, where the glyph is blitted texel for pixel from its
/// snapped pen, so a screen column maps onto the slot at the cell pitch as a
/// strip's does. A frame that stretches the slot onto the cell has no such
/// mapping and reports absent. The slot is keyed and, when fresh, faulted
/// exactly as the glyph fetch keys it — the anchor's own phase, the asking
/// cell's tier — for the reason the mono tap faults: a tap that reaches a glyph
/// ahead of its fetch takes the slot the fetch will find resident.
///
/// - Parameters:
///   - in: The frame being built, for the cells and the grid origin.
///   - atlas: The backend, which rasterises a slot this faults.
///   - wide: The bound two-cell sheet.
///   - weight: The frame's variable weight, as the glyph fetch keys it.
///   - rowBase: Index of the row's first cell.
///   - column: The column to resolve, the glyph's first or second.
///   - metrics: The cell box the pen position is measured in.
///   - askingVariant: The raster variant of the cell asking.
/// - Returns: The tap, or an absent one.
SkipInkTap wideSkipInkTap(const BuildInputs& in, const GlyphBackend& atlas, const WideAtlasBinding& wide, std::uint32_t weight,
    int rowBase, int column, const CellMetrics& metrics, std::uint32_t askingVariant, const LetterExtent* rowLetters)
{
    SkipInkTap tap;
    if (!in.subpixelGlyphs || !wide.cache || !atlas.rasteriseWideGlyph || column < 0 || column >= in.columns)
        return tap;
    int anchorColumn = column;
    if (Ubiquitty::cellPen(in.cells[static_cast<std::size_t>(rowBase + column)]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        anchorColumn = column - 1;
    if (anchorColumn < 0 || column - anchorColumn > 1)
        return tap;
    const Cell& anchor = in.cells[static_cast<std::size_t>(rowBase + anchorColumn)];
    const Pen& pen = Ubiquitty::cellPen(anchor);
    if (anchor.codePoint == 0 || anchor.codePoint == ' ' || pen.extendedAttributes.textSizeSpecID() != 0
        || !pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)
        || pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Hidden, Ubiquitty::VT::Attribute::Emoji }))
        return tap;
    const FontFace face = fontFace(pen.attributes.contains(Ubiquitty::VT::Attribute::Bold), pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    // On a row laid out as letters the glyph's pen and the column's box are
    // the layout's; on a grid row they are the column at the pitch.
    const float anchorLeft = rowLetters ? in.originInPixels.x + rowLetters[anchorColumn].leftInPixels : in.originInPixels.x + anchorColumn * metrics.cellWidth;
    const float columnLeft = rowLetters ? in.originInPixels.x + rowLetters[column].leftInPixels : in.originInPixels.x + column * metrics.cellWidth;
    const float columnWidth = rowLetters ? rowLetters[column].advanceInPixels : metrics.cellWidth;
    const SubpixelPhase snap = phaseForPen(anchorLeft, widePhaseCount(in, atlas, anchor.codePoint, face), in.subpixelPhaseRounding);
    const std::uint32_t phase = static_cast<std::uint32_t>(snap.phase);
    const std::uint32_t variant = makeRasterVariant(phase, variantTier(askingVariant));
    const std::uint64_t key = glyphSlotKey(phasedWideGlyphKey(anchor.codePoint, weight, face, phase), variant);
    bool isFresh = false;
    const int slot = wide.cache->slotForKey(key, &isFresh);
    if (slot < 0)
        return tap;
    if (isFresh && !atlas.rasteriseWideGlyph(atlas.context, anchor.codePoint, face, variant, slot)) {
        wide.cache->forgetKey(key);
        return tap;
    }
    const GlyphUV full = wide.atlasRectForSlot(slot);
    // Tracking moves the pen without stretching the raster. Convert the cell's
    // screen span at the same atlas scale as the glyph quad.
    const float phasedScaleX = atlasToDeviceScale(metrics);
    const float segmentLeft = (columnLeft - static_cast<float>(snap.snappedPenX)) / phasedScaleX
        + in.glyphMarginInPixels.width;
    const float slotHeight = full.maxV - full.minV;
    tap.inkMinUV = { full.minU + segmentLeft, full.minV + in.glyphMarginInPixels.height };
    tap.inkMaxUV = { full.minU + segmentLeft + columnWidth / phasedScaleX, full.minV + slotHeight - in.glyphMarginInPixels.height };
    tap.boxMinUV = { full.minU, full.minV };
    tap.boxMaxUV = { full.maxU, full.maxV };
    tap.present = true;
    tap.sheet = SkipInkSheet::Wide;
    return tap;
}

/// Resolves the coverage tap for one column of a row.
///
/// Off-grid columns and cells with no maskable ink — blank, hidden, wide, emoji
/// — report absent, which is what leaves a neighbour's halo unclipped rather
/// than clipped against the wrong glyph.
///
/// The slot is keyed at the column's OWN subpixel phase, which is the raster
/// the cell actually draws. Keying at phase 0 instead would sample a glyph
/// shifted by up to a device pixel from the ink on screen, and would fault a
/// phase-0 slot that nothing else on the subpixel path ever uses.
///
/// On the phased path the tap is the cell's pitch laid over the slot from
/// where the cell's left edge falls in it — the glyph is blitted texel for
/// pixel from the snapped pen, so that edge lands a phase's fraction into the
/// slot's cell — as the strip and two-cell taps lay theirs. The glyph box
/// stretched over the cell is the stretched path's mapping; read on the
/// phased path it sits the fraction to one side of the ink, and the cut
/// lands beside the descender rather than around it.
///
/// - Parameters:
///   - in: The frame being built, for the cells and the grid origin.
///   - atlas: The backend, so a slot this faults is rasterised rather than left
///     empty for a cell that will never fault it again.
///   - mono: The bound atlas: cache, slot layout, capacity and frame weight.
///   - rowBase: Index of the row's first cell.
///   - column: The column to resolve; out-of-range reports absent.
///   - metrics: The cell box the pen position is measured in.
///   - askingVariant: The raster variant of the cell asking, whose tier the
///     tap is keyed under.
///   - rowLetters: The row's letters where the primary face laid it out as
///     runs (LetterLayout.h), so the tap is the letter's own box; null on a
///     grid row.
/// - Returns: The tap, or an absent one.
SkipInkTap resolveSkipInkTap(const BuildInputs& in, const GlyphBackend& atlas, const MonoAtlasBinding& mono,
    int rowBase, int column, const CellMetrics& metrics, std::uint32_t askingVariant, const LetterExtent* rowLetters)
{
    SkipInkTap tap;
    if (column < 0 || column >= in.columns)
        return tap;
    const Cell& cell = in.cells[static_cast<std::size_t>(rowBase + column)];
    const Pen& pen = cell.pen;
    if (cell.codePoint == 0 || cell.codePoint == ' ' || pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)
        || pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Emoji }))
        return tap;
    const FontFace face = fontFace(pen.attributes.contains(Ubiquitty::VT::Attribute::Bold), pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    // Tracking changes the screen span, not the texel scale of the glyph; a
    // row laid out as letters puts the glyph at its letter's pen and spans
    // its advance.
    const float cellLeft = rowLetters ? in.originInPixels.x + rowLetters[column].leftInPixels : in.originInPixels.x + column * metrics.cellWidth;
    const float cellWidth = rowLetters ? rowLetters[column].advanceInPixels : metrics.cellWidth;
    const std::uint32_t phase = in.subpixelGlyphs
        ? static_cast<std::uint32_t>(phaseForPen(cellLeft, in.subpixelPhaseCount, in.subpixelPhaseRounding).phase)
        : 0u;
    // The tier is the ASKING cell's, not this neighbour's: the tap wants the
    // glyph's shape for skip-ink, which every tier shares, and a word is one
    // pen. A neighbour in another tier faults one more slot than it needed; it
    // never draws the wrong shape.
    const std::uint32_t variant = makeRasterVariant(phase, variantTier(askingVariant));
    bool isFresh = false;
    const int slot = mono.cache->slotForKey(glyphSlotKey(monoGlyphKey(cell.codePoint, mono.weight, face, phase), variant), &isFresh);
    if (slot < 0)
        return tap;
    // Faulting a slot obliges the caller to fill it. This tap reaches a cell the
    // column walk has not arrived at yet, so leaving it unfilled costs that cell
    // its glyph entirely: its own fetch finds the key already resident, is told
    // the slot is not fresh, and never rasterises — and the slot it inherits
    // holds whatever the atlas last cleared it to. The cell then draws blank,
    // with no diagnostic anywhere, because a blank glyph is what a space looks
    // like.
    if (isFresh)
        atlas.rasteriseMonoGlyph(atlas.context, cell.codePoint, face, variant, slot);
    const GlyphUV box = mono.geometry.atlasRectForSlotFull(slot, mono.slotCapacity);
    tap.boxMinUV = { box.minU, box.minV };
    tap.boxMaxUV = { box.maxU, box.maxV };
    tap.present = true;
    if (!in.subpixelGlyphs) {
        const GlyphUV ink = mono.geometry.atlasRectForSlot(slot, mono.slotCapacity);
        tap.inkMinUV = { ink.minU, ink.minV };
        tap.inkMaxUV = { ink.maxU, ink.maxV };
        return tap;
    }
    const SubpixelPhase snap = phaseForPen(cellLeft, in.subpixelPhaseCount, in.subpixelPhaseRounding);
    const float phasedScaleX = atlasToDeviceScale(metrics);
    const float segmentLeft = (cellLeft - static_cast<float>(snap.snappedPenX)) / phasedScaleX + in.glyphMarginInPixels.width;
    const float slotHeight = box.maxV - box.minV;
    tap.inkMinUV = { box.minU + segmentLeft, box.minV + in.glyphMarginInPixels.height };
    tap.inkMaxUV = { box.minU + segmentLeft + cellWidth / phasedScaleX, box.minV + slotHeight - in.glyphMarginInPixels.height };
    return tap;
}

/// Substitutes the cell a proportional run's column draws in the grid's face.
///
/// A proportional run (Docs/proportional-text.md) stores its text once, on
/// the anchor, and its continuations carry nothing; a backend without a
/// proportional strip sets the text across the run's cells in the grid's own
/// face, one code point per cell, clipped to the run. Each cell of the run
/// therefore draws as a plain narrow cell holding the run's k-th code point,
/// k being its distance from the anchor, with the run's chain attributes and
/// spec id cleared so the plain path claims one column and fetches one glyph.
/// A column past the text draws blank.
///
/// - Parameters:
///   - in: The frame, for the cells, the spec table and the cluster resolver.
///   - rowBase: Index of the row's first cell.
///   - column: The column to substitute.
///   - stored: The cell as the model holds it.
///   - scratch: Receives the cell to draw when the column belongs to a run.
/// - Returns: Whether `scratch` was filled.
bool gridFaceCellOfProportionalRun(const BuildInputs& in, int rowBase, int column, const Cell& stored, Cell& scratch)
{
    const Pen& pen = Ubiquitty::cellPen(stored);
    const unsigned specID = pen.extendedAttributes.textSizeSpecID();
    if (specID == 0 || !in.textSizeTable)
        return false;
    const Ubiquitty::TextSizeSpec* spec = in.textSizeTable->lookup(specID);
    if (!spec || !spec->isProportional)
        return false;
    int anchorColumn = column;
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
        // The anchor is the nearest cell to the left that is not itself a
        // continuation; a chain the repair has already cut leaves no anchor
        // with this spec, and the cell then draws as the model holds it.
        while (anchorColumn > 0) {
            --anchorColumn;
            if (!Ubiquitty::cellPen(in.cells[static_cast<std::size_t>(rowBase + anchorColumn)]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                break;
        }
    }
    const Cell& anchorCell = in.cells[static_cast<std::size_t>(rowBase + anchorColumn)];
    if (Ubiquitty::cellPen(anchorCell).extendedAttributes.textSizeSpecID() != specID)
        return false;
    // The run's text, which its anchor names; cell k of the run draws code
    // point k. Scratch kept across cells and grown once to the most a run
    // holds, which is more than a stack frame should carry.
    thread_local std::vector<std::uint32_t> codePoints;
    if (codePoints.size() < static_cast<std::size_t>(runTextMaximumCodePoints))
        codePoints.resize(static_cast<std::size_t>(runTextMaximumCodePoints));
    int count = in.clusterResolver ? in.clusterResolver(&anchorCell, std::span(codePoints), in.clusterResolverContext) : 0;
    if (count <= 0) {
        codePoints[0] = anchorCell.codePoint;
        count = 1;
    }
    const int k = column - anchorColumn;
    scratch = stored;
    scratch.codePoint = k < count ? codePoints[k] : 0;
    scratch.setCombiningLength(0);
    scratch.setClusterID(0);
    Pen drawn = pen;
    drawn.attributes.remove(Ubiquitty::VT::Attribute::Wide);
    drawn.attributes.remove(Ubiquitty::VT::Attribute::WideContinuation);
    drawn.extendedAttributes.setTextSizeSpecID(0);
    Ubiquitty::cellSetPen(scratch, drawn);
    return true;
}

// A row ink crossing's sources, one bit each: the strips mark theirs as they
// are emitted, and the scan after the walk marks the mono glyphs and the other
// streams without disturbing them. Any bit set is a crossing.
constexpr std::uint8_t otherStreamCrossing = 1;
constexpr std::uint8_t monoGlyphCrossing = 2;

// Marks `bit` on every row boundary the ink spanning [top, bottom) crosses.
void markInkSpan(std::span<std::uint8_t> crossings, float gridTop, float cellHeight, int rows, float top, float bottom, std::uint8_t bit)
{
    if (crossings.empty() || !(bottom > top) || !(cellHeight > 0.0f))
        return;
    // The first boundary below the span's top, and every one after it the
    // span still reaches past.
    int boundary = std::max(static_cast<int>(std::floor((top - gridTop) / cellHeight)) + 1, 1);
    for (; boundary < rows; ++boundary) {
        const float edge = gridTop + static_cast<float>(boundary) * cellHeight;
        if (edge >= bottom)
            break;
        if (edge > top)
            crossings[static_cast<std::size_t>(boundary)] |= bit;
    }
}

// Marks a ligature strip's crossings as it is emitted, from the rows its
// raster inked: the quad itself carries a guard row a side that holds nothing.
void markStripInkCrossings(std::span<std::uint8_t> crossings, float gridTop, float cellHeight, int rows, float inkTop, float inkBottom)
{
    markInkSpan(crossings, gridTop, cellHeight, rows, inkTop, inkBottom, otherStreamCrossing);
}

std::size_t streamHeads(std::size_t count, std::size_t tail, std::size_t capacity)
{
    return std::min(count > tail ? count - tail : 0, capacity);
}

// Marks the mono glyph stream's crossings afresh. A glyph drawn from a mono
// slot the backend has measured marks by the slot's ink, the guard taken back
// off; one from a slot not yet measured — rasterised by this very build, on a
// backend whose rasters land later — marks by its quad, which is the slot.
void markMonoInkCrossings(float gridTop, float cellHeight, int rows, const MonoAtlasBinding* mono,
    const InstanceBuffers& out, const InstanceCounts& counts, std::span<std::uint8_t> crossings)
{
    for (std::uint8_t& entry : crossings)
        entry &= static_cast<std::uint8_t>(~monoGlyphCrossing);
    if (!out.glyphs || !(cellHeight > 0.0f))
        return;
    const std::size_t head = streamHeads(counts.glyphs, counts.cursorTier.glyphs, out.glyphCapacity);
    // A soft-font glyph rides the mono stream from a sheet of its own, whose
    // coordinates name no mono slot; a frame holding one keeps every quad.
    const bool canReadMonoInk = mono && mono->inkBoxForSlot && mono->geometry.columns > 0
        && mono->geometry.slotWidthInPixels > 0 && mono->geometry.slotHeightInPixels > 0 && counts.softFontGlyphs == 0;
    for (std::size_t index = 0; index < head; ++index) {
        const GlyphInstance& glyph = out.glyphs[index];
        float top = glyph.originInPixels.y;
        float bottom = top + glyph.sizeInPixels.y;
        if (canReadMonoInk) {
            const float slotWidth = static_cast<float>(mono->geometry.slotWidthInPixels);
            const float slotHeight = static_cast<float>(mono->geometry.slotHeightInPixels);
            const float uvTop = glyph.atlasTopLeft.y;
            const float uvBottom = glyph.atlasBottomRight.y;
            const int column = static_cast<int>(std::floor((glyph.atlasTopLeft.x + glyph.atlasBottomRight.x) * 0.5f / slotWidth));
            const int slotRow = static_cast<int>(std::floor((uvTop + uvBottom) * 0.5f / slotHeight));
            const int slot = slotRow * mono->geometry.columns + column;
            const bool isInsideOneSlot = column >= 0 && slotRow >= 0 && column < mono->geometry.columns
                && slot < mono->slotCapacity && uvBottom > uvTop
                && uvTop >= static_cast<float>(slotRow) * slotHeight && uvBottom <= static_cast<float>(slotRow + 1) * slotHeight;
            if (isInsideOneSlot) {
                const GlyphInkBox& ink = mono->inkBoxForSlot[slot];
                if (ink.minInPixels.x <= ink.maxInPixels.x && ink.minInPixels.y <= ink.maxInPixels.y) {
                    if (ink.maxInPixels.y <= ink.minInPixels.y)
                        continue; // measured and blank: no ink to cross anything
                    // The guard row ``measureInkBox`` adds sits outside the
                    // ink unless the ink reaches the slot's edge, where the
                    // clamp took it back already.
                    const float guardedTop = ink.minInPixels.y;
                    const float guardedBottom = ink.maxInPixels.y;
                    const float inkTop = guardedTop > 0.0f ? guardedTop + 1.0f : guardedTop;
                    const float inkBottom = guardedBottom < slotHeight ? guardedBottom - 1.0f : guardedBottom;
                    const float scale = glyph.sizeInPixels.y / (uvBottom - uvTop);
                    const float slotY = static_cast<float>(slotRow) * slotHeight;
                    top = std::max(top, glyph.originInPixels.y + (slotY + inkTop - uvTop) * scale);
                    bottom = std::min(bottom, glyph.originInPixels.y + (slotY + inkBottom - uvTop) * scale);
                }
            }
        }
        markInkSpan(crossings, gridTop, cellHeight, rows, top, bottom, monoGlyphCrossing);
    }
}

// Marks every row boundary a quad drawn into the grid crosses, for the
// streams the strips and the mono glyphs leave: the heads only, because the
// cursor tier is each stream's tail and draws over the grid, never into it.
// Cell quads are their own cell, a glyph baked into one is cropped to it, and
// the margin strips sit outside the grid, so none of them can carry ink across.
void markRowInkCrossingsOver(float gridTop, float cellHeight, int rows, const MonoAtlasBinding* mono,
    const InstanceBuffers& out, const InstanceCounts& counts, std::span<std::uint8_t> crossings)
{
    if (!(cellHeight > 0.0f))
        return;
    const InstanceCounts::CursorTier& tier = counts.cursorTier;
    const auto markGlyphs = [&](const GlyphInstance* stream, std::size_t count, std::size_t tail, std::size_t capacity) {
        if (!stream)
            return;
        const std::size_t head = streamHeads(count, tail, capacity);
        for (std::size_t index = 0; index < head; ++index)
            markInkSpan(crossings, gridTop, cellHeight, rows, stream[index].originInPixels.y,
                stream[index].originInPixels.y + stream[index].sizeInPixels.y, otherStreamCrossing);
    };
    markMonoInkCrossings(gridTop, cellHeight, rows, mono, out, counts, crossings);
    markGlyphs(out.wideGlyphs, counts.wideGlyphs, tier.wideGlyphs, out.wideGlyphCapacity);
    markGlyphs(out.proportionalGlyphs, counts.proportionalGlyphs, tier.proportionalGlyphs, out.proportionalGlyphCapacity);
    markGlyphs(out.proportionalStrips, counts.proportionalStrips, tier.proportionalStrips, out.proportionalStripCapacity);
    markGlyphs(out.colorGlyphs, counts.colorGlyphs, tier.colorGlyphs, out.colorGlyphCapacity);
    markGlyphs(out.doubleGlyphs, counts.doubleGlyphs, tier.doubleGlyphs, out.doubleGlyphCapacity);
    if (out.decorations) {
        const std::size_t head = streamHeads(counts.decorations, tier.decorations, out.decorationCapacity);
        for (std::size_t index = 0; index < head; ++index)
            markInkSpan(crossings, gridTop, cellHeight, rows, out.decorations[index].topLeftInPixels.y,
                out.decorations[index].bottomRightInPixels.y, otherStreamCrossing);
    }
    if (out.boxFills) {
        const std::size_t head = streamHeads(counts.boxFills, tier.boxFills, out.boxFillCapacity);
        for (std::size_t index = 0; index < head; ++index)
            markInkSpan(crossings, gridTop, cellHeight, rows, out.boxFills[index].topLeftInPixels.y,
                out.boxFills[index].bottomRightInPixels.y, otherStreamCrossing);
    }
}

} // namespace

InstanceCounts buildCellInstances(const BuildInputs& in, const GlyphBackend& atlas, const InstanceBuffers& out)
{
    InstanceCounts counts;
    if (in.isScreenBlanked) {
        counts.overlays = 1;
        if (out.overlays && out.overlayCapacity) {
            OverlayInstance cover {};
            cover.topLeftInPixels = { 0.0f, 0.0f };
            cover.bottomRightInPixels = {
                in.viewportSizeInPixels.x > 0 ? in.viewportSizeInPixels.x : in.originInPixels.x + in.columns * in.metrics.cellWidth,
                in.viewportSizeInPixels.y > 0 ? in.viewportSizeInPixels.y : in.originInPixels.y + in.rows * in.metrics.cellHeight
            };
            cover.color = { 0.0f, 0.0f, 0.0f, 1.0f };
            out.overlays[0] = cover;
        }
        return counts;
    }
    // Before beginFrame, so a backend missing a required hook is not asked to
    // start a frame it cannot supply.
    if (!isDrawable(atlas))
        return counts;
    atlas.beginFrame(atlas.context);
    if (!in.cells || !in.palette || in.rows <= 0 || in.columns <= 0)
        return counts;
    // Per build, as the slots its windows name are touched per frame.
    static thread_local SizedBlockWindows sizedBlocks;
    sizedBlocks.beginBuild();
    // Cleared before the walk: the strips mark their crossings as they are
    // emitted, and the scan after the walk marks the rest.
    const std::span<std::uint8_t> rowInkCrossings = out.rowInkCrossings && out.rowInkCrossingCapacity >= static_cast<std::size_t>(in.rows) + 1
        ? std::span<std::uint8_t>(out.rowInkCrossings, static_cast<std::size_t>(in.rows) + 1)
        : std::span<std::uint8_t> {};
    std::ranges::fill(rowInkCrossings, std::uint8_t { 0 });

    // Attribution harness. Each bit skips one phase of the per-cell body so its
    // cost shows up as the delta against the same binary with the mask at zero,
    // which is the only comparison that holds: a separate build moves code
    // layout and the difference stops being the phase.
    //
    // Read once per process rather than per frame — a getenv inside the timed
    // region would be measuring the harness. Zero by default and never set in
    // production, so the branches are loop-invariant and predicted.
    //
    // Output is WRONG under any non-zero mask. This answers "what does this
    // phase cost", never "is the frame right".
    enum AblateBit : unsigned {
        AblateResolve = 1u << 0,
        // Bit 1 is unassigned and stays that way: these values are typed into
        // UBQ_BUILDER_ABLATE by hand and quoted in measurements, so closing the
        // gap re-points every mask above it at a different phase. A new bit
        // goes on the end.
        // The variable is UBIQUITTY_BUILDER_ABLATE, which is what the lambda
        // below reads. A mask set under any other name is not read and every
        // phase reports its full cost, which looks like a phase that costs
        // nothing rather than like a switch that was never thrown.
        AblateGlyphFetch = 1u << 2,
        AblateCellWrite = 1u << 3,
        AblateDecorations = 1u << 4,
        AblateClusterRead = 1u << 5,
        // The fetch splits four ways and a scene rarely pays for all of them.
        // `AblateGlyphFetch` skips the lot, which answers "what does fetching
        // cost" and not "which fetch" — and the two have had different answers
        // every time they were asked separately.
        //
        // These RANK the four; they do not decompose the total. Skipping a fetch
        // leaves the cell with an absent glyph, which also skips the UV maths,
        // the DHL crop and the glyph-instance write behind it, so the four
        // never sum to `AblateGlyphFetch`. `AblateBoxFetch` is the clearest
        // case: it measures NEGATIVE, because a box cell denied its font glyph
        // falls to procedural geometry, which is more work rather than less.
        AblateMonoFetch = 1u << 6,
        AblateWideFetch = 1u << 7,
        AblateColorFetch = 1u << 8,
        AblateBoxFetch = 1u << 9,
        // Per ROW, and the only per-row call in the walk. Left out of
        // AblateGlyphFetch because it is not a fetch: it asks the backend to
        // SHAPE a row, which on a CoreText backend is the most expensive thing
        // buildCellInstances does and the one a mask over the per-cell phases
        // cannot reach.
        AblateLigatureShape = 1u << 10,
        // Restores the unconditional per-row shaping call, so the row gate
        // below can be A/B'd inside one process. A wall-clock A/B of two
        // binaries cannot resolve a microsecond on a shared machine.
        AblateLigaturePrescan = 1u << 11,
        // Skips distilling the row the shaper reads, so the cost of answering
        // eligibility and shape identity in Core is separable from the shaping
        // it feeds. The seam names no model type, which means this pass exists;
        // what it costs is a question about a row that can ligate, since a row
        // that cannot never reaches it. Ablated, the shaper reads the previous
        // row's answers and draws the wrong runs — every ablation changes
        // output, and this one is measured against the same binary rather than
        // against a second build, for the reason above.
        AblateLigatureDistil = 1u << 12,
    };
    static const unsigned ablate = [] {
        const char* raw = std::getenv("UBIQUITTY_BUILDER_ABLATE");
        return raw ? static_cast<unsigned>(std::strtoul(raw, nullptr, 0)) : 0u;
    }();

    const CellMetrics& metrics = in.metrics;
    const float presentationScale = metrics.presentationScale > 0 && std::isfinite(metrics.presentationScale) ? metrics.presentationScale : 1;
    // Optional atlas facts fall back to the original cell, not the smaller
    // destination pitch. Every raster route applies the presentation once.
    const float naturalCellWidth = metrics.cellWidth / presentationScale;
    const float naturalCellHeight = metrics.cellHeight / presentationScale;
    const float glyphScaleY = in.glyphReferenceLineHeightInPixels > 0.0f
        ? metrics.cellHeight / in.glyphReferenceLineHeightInPixels
        : presentationScale;
    // One phase for the frame: the shade dither is anchored to the grid's
    // top-left pixel, so every shaded cell continues one field.
    const ShadeField shadeField {
        in.shadeStyle,
        shadePhase(static_cast<int>(std::round(in.originInPixels.x)), static_cast<int>(std::round(in.originInPixels.y))),
    };
    const BrailleLattice brailleLattice = textAlignedBrailleLattice(
        { in.originInPixels.x, in.originInPixels.y, metrics.cellWidth, metrics.cellHeight, in.brailleStyle,
            in.subpixelPhaseCount, in.subpixelPhaseRounding },
        metrics.baselineFromTop, metrics.xHeight);
    // The atlas texel size is not read here. Every use of it was a
    // multiplication carrying an atlas-pixel quantity into UV space — the
    // dilation pad, the subpixel quad's margins — and the rects this builder
    // emits are atlas pixels, so those quantities are already in the space
    // they are written in. The hook remains for the consumers that normalize.
    // Same shape for the ligature sheet, and the row scratch it fills. Sized
    // once for the widest row rather than per row: a shaper is asked about a
    // whole row at a time, so the answer has to live somewhere for the column
    // walk to read, and a backend offering no sheet allocates nothing.
    const bool hasLigatureSheet = out.ligatureGlyphs != nullptr;
    const bool hasProportionalStripSheet = out.proportionalStrips != nullptr;
    const bool hasBoxSheet = out.boxFills != nullptr;
    // A session that ever committed a DECDLD download. Tested per cell ahead of
    // the code-point range, so a session that never did — nearly all of them —
    // reaches the range test only through a branch that always falls the same way.
    const bool hasSoftFont = in.softFont != nullptr && in.softFont->valid;
    const MonoAtlasBinding mono = atlas.bindMonoAtlas(atlas.context);
    // Release a superseded soft font's slots before anything competes for them.
    // Bounded by `drcsMaxCells` and reached only on the frame a download lands,
    // and a key that was never resident costs one hash probe.
    if (in.retiredSoftFontSerial != 0 && mono.cache != nullptr) {
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(VT::drcsMaxCells); ++index)
            mono.cache->forgetKey(drcsGlyphKey(in.retiredSoftFontSerial, index));
    }
    // The colour sheet, on the same terms: bound once so a resident emoji is a
    // probe in Core rather than a crossing per cell.
    const ColorAtlasBinding color = atlas.bindColorAtlas ? atlas.bindColorAtlas(atlas.context) : ColorAtlasBinding {};
    // An output stream cannot carry colour without a backing atlas. A
    // missing sheet falls through to the ordinary bitmap cluster route.
    const bool hasColorSheet = out.colorGlyphs != nullptr
        && ((color.cache && atlas.rasteriseColorGlyph) || atlas.colorGlyph);
    const WideAtlasBinding wide = atlas.bindWideAtlas ? atlas.bindWideAtlas(atlas.context) : WideAtlasBinding {};
    const ProportionalAtlasBinding proportional = atlas.bindProportionalAtlas
        ? atlas.bindProportionalAtlas(atlas.context)
        : ProportionalAtlasBinding {};
    // A proportional primary face lays every row out as proportional runs
    // (LetterLayout.h): Core's verdict on the face, from the advances the
    // backend sampled, and a backend that can shape a run in it. A fixed
    // face, or a backend with no shaper, keeps the grid: each column at the
    // pitch, which is what every site below reads through `cellGridX`.
    // Only where glyphs take the subpixel stream: a letter is placed by its
    // own pen, and the cell-baked path has none, its glyph being stretched
    // over the cell quad it is baked into.
    const bool isLaidOutAsLetters = atlas.primaryFacePitch == FontMetrics::FacePitch::Proportional
        && atlas.shapePrimaryRun != nullptr && in.cells != nullptr && metrics.cellWidth > 0.0f && in.subpixelGlyphs;
    static thread_local std::vector<LetterCell> letterCells;
    static thread_local std::vector<std::uint32_t> letterPool;
    static thread_local LetterLayoutScratch letterScratch;
    static thread_local std::vector<LetterExtent> callLetters;
    static thread_local std::vector<std::uint8_t> callLetterRows;
    // Where each walked row's letters go: the caller's storage, which the
    // capture answers hit tests from and which keeps a clean row's layout
    // across a damage-first build, else storage for this call alone. The
    // per-row bytes are written for every walked row, letters or not, so a
    // face change from proportional to fixed clears the rows it repaints.
    const std::size_t letterCount = static_cast<std::size_t>(std::max(in.rows, 0)) * static_cast<std::size_t>(std::max(in.columns, 0));
    LetterExtent* frameLetters = nullptr;
    std::uint8_t* frameLetterRows = nullptr;
    if (out.letters != nullptr && out.letterCapacity >= letterCount && out.letterRows != nullptr && out.letterRowCapacity >= static_cast<std::size_t>(std::max(in.rows, 0))) {
        frameLetters = out.letters;
        frameLetterRows = out.letterRows;
    } else if (isLaidOutAsLetters) {
        callLetters.assign(letterCount, LetterExtent {});
        callLetterRows.assign(static_cast<std::size_t>(std::max(in.rows, 0)), 0u);
        frameLetters = callLetters.data();
        frameLetterRows = callLetterRows.data();
    }
    // Stale entries have no reader: shouldShapeLigatures gates every read,
    // and a row that shapes rewrites both spans whole.
    static thread_local std::vector<LigatureRun> ligatureRuns;
    static thread_local std::vector<LigatureCell> ligatureCells;
    static thread_local std::vector<SkipInkTap> proportionalSkipInkTaps;
    static thread_local std::vector<std::uint8_t> proportionalSkipInkClaims;
    // The row's glyph inks, appended run by run, and per column whether the
    // claim covering it was drawn glyph by glyph: such an anchor emits its
    // rules cell by cell over the claim, each cell tapping its own glyph. The
    // caller's scratch when it keeps one, else one for this call; a row with
    // no such claim leaves both untouched.
    CellInstanceScratch callScratch;
    CellInstanceScratch& scratch = out.scratch ? *out.scratch : callScratch;
    std::vector<ProportionalGlyphInk>& proportionalGlyphInks = scratch.proportionalGlyphInks;
    std::vector<std::uint8_t>& proportionalPerGlyphClaims = scratch.proportionalPerGlyphClaims;
    // Per visual column of the row being built: whether a sized band standing
    // away from its own storage stands there.
    static thread_local std::vector<std::uint8_t> displacedBandCover;
    if (hasLigatureSheet) {
        ligatureRuns.resize(static_cast<std::size_t>(in.columns));
        ligatureCells.resize(static_cast<std::size_t>(in.columns));
    }

    if (hasLigatureSheet && atlas.prepareLigatureRow && atlas.finishLigaturePreparation && !isLaidOutAsLetters
        && !(ablate & (AblateLigatureShape | AblateLigatureDistil))) {
        for (int row = 0; row < in.rows; ++row) {
            if (in.dirtyRows && !in.dirtyRows[row]) {
                continue;
            }
            const auto lineAttribute = in.lineAttributes ? in.lineAttributes[row] : lineAttributeNormal;
            if (textScaleForLineAttribute(lineAttribute).wide()) {
                continue;
            }
            const auto cells = std::span(&in.cells[static_cast<std::size_t>(row) * in.columns], static_cast<std::size_t>(in.columns));
            distilLigatureRow(cells, std::span<LigatureCell>(ligatureCells));
            atlas.prepareLigatureRow(atlas.context, std::span<const LigatureCell>(ligatureCells), metrics.cellWidth / presentationScale);
        }
        atlas.finishLigaturePreparation(atlas.context);
    }

    // Narrowed once. Resolving a cell against the 16-bit palette runs an integer
    // divide per channel — six per cell, nine when bold remaps to a bright index
    // — and none of it depends on the pen. On a screen of varied pens the resolve
    // memo misses nearly every cell, so those divides are per cell rather than
    // per distinct pen, and the frame pays a few thousand of them to compute 259
    // constants over and over.
    //
    // Which rect of a mono slot this cell's glyph occupies, and the two
    // answers are not interchangeable.
    //
    // A SUBPIXEL quad is the slot: it expands what it receives by the
    // margin, so it takes the contracted-slot rect ``slotUV`` holds.
    // A BAKED glyph's quad is the CELL, and the rect that maps onto it is
    // the glyph box — one texel narrower, the difference being the column
    // of subpixel phase headroom every slot carries. Sampling that column
    // into a cell draws every glyph a hair too wide with a sliver of the
    // next phase down its right edge, which reads as a font choice rather
    // than as a wrong rect.
    const auto monoSlotUV = [&mono](int slot, bool isSubpixelSlot) { // bool-ok: which of the two rects, decided per cell
        if (!isSubpixelSlot)
            return mono.geometry.atlasRectForSlot(slot, mono.slotCapacity);
        return contractedSlotRect(mono, slot);
    };

    // DECSCNM (whole-screen reverse video) swaps the DEFAULT foreground/
    // background only, so explicit-color cells do not invert; per-cell
    // Attribute::Reverse still swaps inside resolveCell, so reverseAlso is false
    // below. Each slot narrows on its own, so the swap is taken on the narrowed
    // pair rather than on a copy of the 16-bit palette.
    const Palette8 framePalette8 = [&] {
        Palette8 narrowed = narrowPalette(*in.palette);
        if (in.screenReverse)
            std::swap(narrowed.defaultForeground, narrowed.defaultBackground);
        return narrowed;
    }();

    // The selected cells, indexed by visible row so the cell loop can test "is
    // (row, column) selected?" in O(1). Built only when a selected cell draws
    // differently from its neighbours in the cell pass — an OSC 19 ink, or a
    // fill behind the text; a selection drawn as an overlay quad needs no
    // lookup, the lookup stays empty and the gate short-circuits.
    // The reader tests the gate before it indexes, so stale entries are
    // unreachable.
    static thread_local std::vector<int> selectionColumn0; // per visible row, 0 default
    static thread_local std::vector<int> selectionColumn1; // per visible row, -1 = not selected
    // The cuts inside the two end cells, per visible row; zero is the whole
    // cell. Read only by the fill under a proportional run, whose cells hold
    // several letters each.
    static thread_local std::vector<int> selectionSubColumn0;
    static thread_local std::vector<int> selectionSubColumn1;
    // A row's pieces, left to right, when it holds more than one: rows 0 to
    // rows-1 index `selectionPieces` from selectionPieceStart[row] to
    // selectionPieceStart[row + 1]. The two columns above are the hull of a
    // row's pieces; a cell between two pieces is inside the hull and outside
    // the selection, and only the pieces say so.
    static thread_local std::vector<SelectionRowSpan> selectionPieces;
    static thread_local std::vector<int> selectionPieceStart;
    const bool hasSelection = in.selectionSpans && in.selectionSpanCount > 0;
    const bool isFrameSelectionForegroundActive = in.selectionForegroundOverride && hasSelection;
    const bool isFrameSelectionBehindText = in.isSelectionBehindText && hasSelection;
    const bool isFrameSelectionInCells = isFrameSelectionForegroundActive || isFrameSelectionBehindText;
    if (isFrameSelectionInCells) {
        selectionColumn0.assign(static_cast<std::size_t>(in.rows), 0);
        selectionColumn1.assign(static_cast<std::size_t>(in.rows), -1);
        selectionSubColumn0.assign(static_cast<std::size_t>(in.rows), 0);
        selectionSubColumn1.assign(static_cast<std::size_t>(in.rows), 0);
        selectionPieceStart.assign(static_cast<std::size_t>(in.rows) + 1, 0);
        // A row a rich text block crosses is selected in a piece each side of
        // it, and a row holding part of a sized block the selection touches
        // in a piece for the block's cells; the row's entry spans them all.
        int pieceCount = 0;
        for (int spanIndex = 0; spanIndex < in.selectionSpanCount; ++spanIndex) {
            const SelectionRowSpan& span = in.selectionSpans[spanIndex];
            if (span.visibleRow < 0 || span.visibleRow >= in.rows || span.column1 < span.column0)
                continue;
            const auto row = static_cast<std::size_t>(span.visibleRow);
            ++selectionPieceStart[row + 1];
            ++pieceCount;
            const bool isFirstPiece = selectionColumn1[row] < selectionColumn0[row];
            if (isFirstPiece || span.column0 < selectionColumn0[row]) {
                selectionColumn0[row] = span.column0;
                selectionSubColumn0[row] = span.subColumn0;
            }
            if (isFirstPiece || span.column1 > selectionColumn1[row]) {
                selectionColumn1[row] = span.column1;
                selectionSubColumn1[row] = span.subColumn1;
            }
        }
        for (int row = 0; row < in.rows; ++row)
            selectionPieceStart[static_cast<std::size_t>(row) + 1] += selectionPieceStart[static_cast<std::size_t>(row)];
        selectionPieces.assign(static_cast<std::size_t>(pieceCount), SelectionRowSpan {});
        static thread_local std::vector<int> pieceCursor;
        pieceCursor.assign(selectionPieceStart.begin(), selectionPieceStart.end() - 1);
        for (int spanIndex = 0; spanIndex < in.selectionSpanCount; ++spanIndex) {
            const SelectionRowSpan& span = in.selectionSpans[spanIndex];
            if (span.visibleRow < 0 || span.visibleRow >= in.rows || span.column1 < span.column0)
                continue;
            selectionPieces[static_cast<std::size_t>(pieceCursor[static_cast<std::size_t>(span.visibleRow)]++)] = span;
        }
        for (int row = 0; row < in.rows; ++row) {
            const auto first = selectionPieces.begin() + selectionPieceStart[static_cast<std::size_t>(row)];
            const auto last = selectionPieces.begin() + selectionPieceStart[static_cast<std::size_t>(row) + 1];
            if (last - first > 1)
                std::sort(first, last, [](const SelectionRowSpan& left, const SelectionRowSpan& right) { return left.column0 < right.column0; });
        }
    }
    // Whether the cell at (row, column) is inside the selection: inside its
    // row's hull and, for a row of several pieces, inside one of them.
    const auto isCellSelected = [&](int row, int column) {
        if (row < 0 || row >= static_cast<int>(selectionColumn1.size()))
            return false;
        const auto index = static_cast<std::size_t>(row);
        if (column < selectionColumn0[index] || column > selectionColumn1[index])
            return false;
        const int first = selectionPieceStart[index];
        const int last = selectionPieceStart[index + 1];
        if (last - first <= 1)
            return true;
        for (int piece = first; piece < last; ++piece) {
            const SelectionRowSpan& span = selectionPieces[static_cast<std::size_t>(piece)];
            if (span.column0 > column)
                break;
            if (column <= span.column1)
                return true;
        }
        return false;
    };

    // The focused search match's rows, in the same shape and for the same reason
    // as the selection lookup above: an O(1) test per cell, built only when
    // there is a match on screen.
    static thread_local std::vector<int> searchMatchColumn0;
    static thread_local std::vector<int> searchMatchColumn1;
    const bool hasFrameFocusedSearchMatch = in.searchMatchSpans && in.searchMatchSpanCount > 0;
    if (hasFrameFocusedSearchMatch) {
        searchMatchColumn0.assign(static_cast<std::size_t>(in.rows), 0);
        searchMatchColumn1.assign(static_cast<std::size_t>(in.rows), -1);
        for (int spanIndex = 0; spanIndex < in.searchMatchSpanCount; ++spanIndex) {
            const SelectionRowSpan& span = in.searchMatchSpans[spanIndex];
            if (span.visibleRow < 0 || span.visibleRow >= in.rows || span.column1 < span.column0)
                continue;
            searchMatchColumn0[static_cast<std::size_t>(span.visibleRow)] = span.column0;
            searchMatchColumn1[static_cast<std::size_t>(span.visibleRow)] = span.column1;
        }
    }
    // Black, not the inverse of the highlight's lightness. The highlight is not
    // a colour the user chooses — it is the fixed yellow every frontend passes
    // from `focusedSearchMatchColor` — so an adaptive ink only ever resolved
    // that one input, to a 0.19 grey, and spent the contrast it was computing on
    // text that reads as dimmed. The match is its background; the glyph on it is
    // black.
    constexpr float searchMatchInk = 0.0f;

    // Push one decoration rule. `top`/`bottom` are device-px y; the rule spans
    // the cell's advance box (cellLeft..cellRight) so a run reads as one
    // continuous line. `shouldMaskInk` attaches the skip-ink coverage band so
    // glyph ink crossing it cuts a clean gap (underline rules only).
    // The cell's own coverage tap and its two neighbours', refreshed per cell.
    // Held out here so the rule emitter reads them without seventeen call sites
    // threading four rects each.
    SkipInkTap selfTap, leftTap, rightTap;
    // The halo radius in atlas UV, and how many taps the shader spends walking
    // it. One tap per device pixel of radius keeps the dilation continuous as
    // the pad grows; a fixed count leaves gaps and stripes the cut.
    const SkipInkReach skipInkReachInDevicePixels = skipInkReach(metrics.skipInkPadding);
    const float atlasToDevice = atlasToDeviceScale(metrics);
    const Vec2 skipInkPadInAtlasPixels { skipInkReachInDevicePixels.horizontal / atlasToDevice, skipInkReachInDevicePixels.vertical / atlasToDevice };
    const float skipInkTaps = static_cast<float>(skipInkReachInDevicePixels.steps);
    auto emitRule = [&](DecorationSite site, float cellLeft, float cellRight, float cellTop, float cellHeight, float top, float bottom, Vec4 color, DecorationKind kind, float thickness, bool shouldMaskInk, bool haveInkMask, Vec2 inkTopLeft, Vec2 inkBottomRight) {
        // Recorded before the capacity check: the site was REACHED whether or
        // not the instance fitted, and a coverage answer that changed with the
        // caller's buffer size would report a scene as uncovered for running
        // out of room.
        counts.decorationSites |= static_cast<std::uint32_t>(site);
        const std::size_t index = counts.decorations++;
        if (index >= out.decorationCapacity || !out.decorations)
            return;
        DecorationInstance& d = out.decorations[index];
        d.topLeftInPixels = { cellLeft, top };
        d.bottomRightInPixels = { cellRight, bottom };
        d.color = color;
        d.kind = static_cast<float>(kind);
        d.dashPeriodInPixels = metrics.cellWidth;
        d.thicknessInPixels = thickness;
        d.skipInk = (shouldMaskInk && haveInkMask) ? 1.0f : 0.0f;
        if (shouldMaskInk && haveInkMask) {
            // The band is the rule's own rect, so every pixel of the rule
            // samples the ink at its own height and the halo is measured from
            // where the rule is drawn. A curl's crest or a dot's top sits above
            // the line; a band squeezed to the stroke at the line's middle
            // mapped those pixels onto ink rows below them, leaving a crest
            // standing against a letter it touched. A rule under a pixel tall
            // still samples one whole row of coverage.
            const float midY = (top + bottom) * 0.5f;
            const float halfBand = (bottom - top) * 0.5f > 0.5f ? (bottom - top) * 0.5f : 0.5f;
            const float bandTop = midY - halfBand;
            const float bandBottom = midY + halfBand;
            const float fractionTop = cellHeight > 0.0f ? (bandTop - cellTop) / cellHeight : 0.0f;
            const float fractionBottom = cellHeight > 0.0f ? (bandBottom - cellTop) / cellHeight : 0.0f;
            // The band's v range, mapped onto whichever cell's slot it samples.
            // A neighbour's glyph occupies its own slot, so the same fractions
            // are re-derived against that slot rather than reused.
            auto bandInSlot = [&](const SkipInkTap& tap) {
                const float span = tap.inkMaxUV.y - tap.inkMinUV.y;
                return Vec4 { tap.inkMinUV.x, tap.inkMaxUV.x,
                    tap.inkMinUV.y + fractionTop * span, tap.inkMinUV.y + fractionBottom * span };
            };
            d.inkTopLeft = { selfTap.inkMinUV.x, selfTap.inkMinUV.y + fractionTop * (selfTap.inkMaxUV.y - selfTap.inkMinUV.y) };
            d.inkBottomRight = { selfTap.inkMaxUV.x, selfTap.inkMinUV.y + fractionBottom * (selfTap.inkMaxUV.y - selfTap.inkMinUV.y) };
            // Already the atlas pixels the name says; the reach's shape is
            // skipInkReach's.
            d.padInAtlas = skipInkPadInAtlasPixels;
            // Three base-8 digits identify the self, left, and right sheets:
            // mono 0, proportional strip 1, ligature strip 2, the proportional
            // glyph sheet 3, the two-cell sheet 4. The complete value is at
            // most 511, which a float represents exactly.
            const float sheetSelectors = static_cast<float>(selfTap.sheet)
                + 8.0f * static_cast<float>(leftTap.sheet)
                + 64.0f * static_cast<float>(rightTap.sheet);
            d.pad1 = { skipInkTaps, sheetSelectors };
            // The own-cell clamp, so a dilation tap reaching past the slot
            // cannot sample the glyph stored beside it.
            d.glyphSlotTopLeftUV = selfTap.boxMinUV;
            d.glyphSlotBottomRightUV = selfTap.boxMaxUV;
            // A zeroed *Box marks an absent neighbour, which is what a cell at
            // the grid edge and a blank neighbour both report.
            d.leftInk = leftTap.present ? bandInSlot(leftTap) : Vec4 { 0.0f, 0.0f, 0.0f, 0.0f };
            d.leftBox = leftTap.present ? Vec4 { leftTap.boxMinUV.x, leftTap.boxMinUV.y, leftTap.boxMaxUV.x, leftTap.boxMaxUV.y } : Vec4 { 0.0f, 0.0f, 0.0f, 0.0f };
            d.rightInk = rightTap.present ? bandInSlot(rightTap) : Vec4 { 0.0f, 0.0f, 0.0f, 0.0f };
            d.rightBox = rightTap.present ? Vec4 { rightTap.boxMinUV.x, rightTap.boxMinUV.y, rightTap.boxMaxUV.x, rightTap.boxMaxUV.y } : Vec4 { 0.0f, 0.0f, 0.0f, 0.0f };
        } else {
            d.inkTopLeft = { 0.0f, 0.0f };
            d.inkBottomRight = { 0.0f, 0.0f };
            d.padInAtlas = { 0.0f, 0.0f };
            d.pad1 = { 0.0f, 0.0f };
            d.glyphSlotTopLeftUV = { 0.0f, 0.0f };
            d.glyphSlotBottomRightUV = { 0.0f, 0.0f };
            d.leftInk = { 0.0f, 0.0f, 0.0f, 0.0f };
            d.leftBox = { 0.0f, 0.0f, 0.0f, 0.0f };
            d.rightInk = { 0.0f, 0.0f, 0.0f, 0.0f };
            d.rightBox = { 0.0f, 0.0f, 0.0f, 0.0f };
        }
        (void)inkTopLeft;
        (void)inkBottomRight;
    };

    // Resolved-colour memo, frame-wide rather than per row: a run of one pen
    // crosses a row boundary as often as not, and resetting at each row would
    // throw the hit away exactly where a blank region is longest.
    //
    // Seeded outside the Pen value range so the first cell always misses: a
    // zeroed Pen is a REAL pen (the default one), and seeding with it would
    // serve default colours to a first cell that carries them without ever
    // resolving.
    Pen memoPen {};
    memoPen.foregroundIndex = static_cast<std::uint16_t>(0xFFFFu);
    ResolvedCell memoResolved {};
    // The smoothing tier of memoResolved.foreground, kept beside it because the
    // two change together and a tier costs three table reads per distinct pen
    // rather than per cell.
    std::uint32_t memoTier = 0;

    // A proportional route exists only when both halves of its seam are
    // present: the backend can produce the resource and the caller has a
    // stream in which Core can submit it. Treating the hook alone as the
    // route drops the stored continuation cells without emitting anything.
    // Frame facts, every term of them, so the walk below reads them rather
    // than re-deciding per row — and the cursor's claim, resolved before the
    // walk, decides the same way the cell it stands on will draw.
    const bool hasProportionalStripRoute = hasProportionalStripSheet
        && atlas.proportionalStrip != nullptr;
    const bool hasProportionalGlyphRoute = atlas.shapeProportionalRun != nullptr
        && atlas.rasteriseProportionalGlyph != nullptr
        && proportional.cache != nullptr
        && proportional.slotCapacity > 0
        && proportional.slotWidthInPixels > 0
        && proportional.slotHeightInPixels > 0
        && out.proportionalGlyphs != nullptr
        && in.glyphBoxWidthInPixels > 0.0f;
    const bool hasProportionalRoute = hasProportionalStripRoute || hasProportionalGlyphRoute;

    // Where the cursor's column lands inside a multi-column claim — a wide
    // pair, a sized block, a proportional run — this is the anchor and the
    // claim's width, so the overlay cursor below draws the box the glyph
    // occupies rather than one grid column inside it. -1 where the cursor
    // stands on a glyph that claims its own column alone.
    //
    // Read from the grid here rather than recorded as the walk passes the
    // anchor: the walk skips a clean row, and no cursor is owed a row, since
    // the retained pixels carry none. An unfocused
    // pane repaints no row at all, so a claim discovered by walking is a claim
    // an inactive window never has — its hollow outline standing one column
    // wide on a two-column glyph. The focused block cannot show it: it is
    // drawn as the cell itself, by the tier pass, which no dirty mask gates.
    int cursorClaimAnchorColumn = -1;
    int cursorClaimWidthCells = 1;
    if (in.cells && in.cursor.row >= 0 && in.cursor.row < in.rows
        && in.cursor.column >= 0 && in.cursor.column < in.columns) {
        const int cursorRowBase = in.cursor.row * in.columns;
        // The anchor is the nearest cell at or left of the cursor that is not
        // itself a continuation, which is the chain shape `Screen::WidePair`
        // maintains and the walk relies on when it leaves a continuation to
        // its lead.
        int anchorColumn = in.cursor.column;
        while (anchorColumn > 0
            && Ubiquitty::cellPen(in.cells[static_cast<std::size_t>(cursorRowBase + anchorColumn)]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
            --anchorColumn;
        const Cell& anchorCell = in.cells[static_cast<std::size_t>(cursorRowBase + anchorColumn)];
        const bool mayCursorRowHoldRun = !in.rowFlags
            || (in.rowFlags[in.cursor.row] & static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed)) != 0;
        // A proportional run with no route draws one code point per cell in
        // the grid's face, so each of its columns is its own glyph and the
        // cursor claims one — the same answer the walk's substituted cell
        // gives, asked of the same facts.
        Cell gridFaceScratch;
        const bool isRunDrawnPerCell = mayCursorRowHoldRun && !hasProportionalRoute
            && Ubiquitty::cellPen(anchorCell).extendedAttributes.textSizeSpecID() != 0
            && gridFaceCellOfProportionalRun(in, cursorRowBase, anchorColumn, anchorCell, gridFaceScratch);
        const int claimedColumns = isRunDrawnPerCell
            ? 1
            : Ubiquitty::claimedRunColumns(anchorCell, in.textSizeTable, in.ambiguousWidth);
        // A chain shorter than its anchor's claim leaves the cursor outside
        // it, and the cursor then stands on the cell it addresses.
        if (claimedColumns > 1 && anchorColumn + claimedColumns > in.cursor.column) {
            cursorClaimAnchorColumn = anchorColumn;
            cursorClaimWidthCells = claimedColumns;
        }
    }

    // The cursor tier. A focused, settled block is drawn as the cell it
    // stands on, re-emitted with the cursor's colours — the fill lerped toward
    // the cursor colour by the blink weight, the glyph inverted under it — as
    // the TAIL of every stream, after the grid, the images and the overlays:
    // a backend keeping its grid across frames draws the heads into that grid
    // and the tails after the blit, and a backend that draws every stream
    // whole draws the tails after every head. The grid never holds a cursor,
    // so a blink or a move owes no row a repaint, and what a program draws
    // around the cursor cannot change how the cursor is drawn: ink reaching
    // the cell from a neighbour, a picture under it and a run spanning it all
    // sit under the tier the same way. The walk visits the cursor's row once
    // more after the last row, so the tier's instances land last; on that
    // pass only the cell whose claim spans the cursor emits, and only with
    // the tier's colours. The bar, the underline, the unfocused outline and a
    // sliding block stay overlay quads, emitted after the walk.
    const bool wantsCursorTier = in.cursor.row >= 0 && in.cursor.row < in.rows && in.cursor.focused
        && !in.cursor.isSliding && in.cursor.shape == Ubiquitty::Cursor::Shape::Block;
    InstanceCounts countsBeforeTier {};
    bool didWalkTier = false;
    // A frame that keeps no background tint and draws no native background
    // over an opaque theme ground gives every cell the same background
    // opacity: resolvedBackgroundOpacity scales the frame's by the ground's
    // alpha, which is one, and takes no distance. Asked once here instead of
    // per cell.
    const bool isBackgroundOpacityUniform = !in.preservesBackgroundTints && !in.usesNativeBackgroundVibrancy
        && in.uniformDefaultBackground.w == 1.0f;
    const float uniformCellBackgroundOpacity = in.backgroundOpacity * in.uniformBackgroundOpacity;
    // The sharp windows of a sized cell on the cell-baked path. Frame scope
    // rather than the cell's: AtlasGlyph initialises its members, so an array
    // declared in the cell body is constructed on every cell, sized or not,
    // and only a sized cell writes it — each entry it reads was written by
    // the same cell's probe.
    AtlasGlyph bakedTiles[maxSizedWindowsPerBand];
    // A frame with no selection or search drawn in its cells, no native
    // vibrancy, no occluding letterbox and no live text-size spec is the
    // common one, and the walk below is compiled for it with
    // those facts as constants: their tests and arms fold out of the cell
    // loop, and the values they would hold stay off its frame. With no live
    // spec every lookup answers null, so the quiet walk reads no table and
    // draws exactly what the table would have told it. Whether glyphs take
    // the subpixel stream is a second constant, since it routes every glyph;
    // whether a quiet frame carries link flags is a third, so a page of links
    // keeps the quiet walk and an ordinary one does not test for them.
    //
    // A frame whose only noise is a selection — the frame of every drag and
    // every copy — has a walk of its own, with the selection read at run time
    // and the rest of the quiet facts still constants.
    const bool hasLiveTextSizeSpec = in.textSizeTable != nullptr && in.textSizeTable->liveMask() != 0;
    const bool isQuietBesideSelection = !hasFrameFocusedSearchMatch && !in.usesNativeTextVibrancy && !in.isGridOccluded
        && !hasLiveTextSizeSpec;
    const WalkClass walkClass = !isQuietBesideSelection ? WalkClass::General
        : isFrameSelectionInCells                       ? WalkClass::Selected
                                                        : WalkClass::Quiet;
    const auto walkPasses = [&](auto frameClass, auto subpixel, auto linked) {
        constexpr bool isQuiet = decltype(frameClass)::value == WalkClass::Quiet;
        constexpr bool isGeneral = decltype(frameClass)::value == WalkClass::General;
        constexpr bool drawsSubpixelGlyphs = decltype(subpixel)::value;
        constexpr bool isLinked = decltype(linked)::value;
        const Ubiquitty::TextSizeTable* const textSizeTable = isGeneral ? in.textSizeTable : nullptr;
        const bool selectionForegroundActive = !isQuiet && isFrameSelectionForegroundActive;
        const bool isSelectionBehindText = !isQuiet && isFrameSelectionBehindText;
        const bool isSelectionInCells = !isQuiet && isFrameSelectionInCells;
        const bool hasFocusedSearchMatch = isGeneral && hasFrameFocusedSearchMatch;
        const bool hasLinkCells = isQuiet ? isLinked : in.linkCells != nullptr;
        const bool usesNativeTextVibrancy = isGeneral && in.usesNativeTextVibrancy;
        const bool usesNativeBackgroundVibrancy = in.usesNativeBackgroundVibrancy;
        const bool isGridOccluded = isGeneral && in.isGridOccluded;
        // The frame facts the cell loop tests, copied into the walk. Captured,
        // each is a load through the closure that every call the loop makes
        // forces again; held here they are values the loop keeps.
        const bool hasSoftFontInWalk = hasSoftFont;
        const bool hasBoxSheetInWalk = hasBoxSheet;
        const bool hasColorSheetInWalk = hasColorSheet;
        const bool hasLigatureSheetInWalk = hasLigatureSheet;
        const bool isBackgroundOpacityUniformInWalk = isBackgroundOpacityUniform;
        const float uniformCellBackgroundOpacityInWalk = uniformCellBackgroundOpacity;
        const bool hasProportionalStripRouteInWalk = hasProportionalStripRoute;
        const bool hasProportionalGlyphRouteInWalk = hasProportionalGlyphRoute;
        const bool hasProportionalRouteInWalk = hasProportionalRoute;
        const bool wantsCursorTierInWalk = wantsCursorTier;
        for (int pass = 0; pass <= in.rows; ++pass) {
            const bool isTierPass = pass == in.rows;
            if (isTierPass) {
                if (!wantsCursorTierInWalk)
                    break;
                countsBeforeTier = counts;
                didWalkTier = true;
            }
            const int row = isTierPass ? in.cursor.row : pass;
            // A clean row's instances are still held by the caller, so rebuilding it
            // would cost the whole per-row walk — shaping, resolve, emit — to
            // reproduce bytes that already exist. Skipping before any of that is the
            // point; a guard further in would still pay for the row. The tier pass
            // is the exception: its row may well be clean, and it emits into no
            // row's instances but the tier's.
            if (!isTierPass && in.dirtyRows && !in.dirtyRows[row])
                continue;

            // A caller building from a dirty-row mask draws into a target it keeps,
            // and confines the grid passes to the content rectangle plus the glyph
            // overhang (FrameDamage.h retainedContentRect). The overhang strips
            // beside this row are then this row's to repaint: a glyph that spilled
            // into the margin last frame — a texture-healed `w` at column zero, a
            // `j` with a left bearing — is gone from the row's cells, and nothing
            // else paints there. Two quads of the clear colour, in their own
            // stream drawn without blending before the cells: a restore must
            // write the clear value, and a blended restore under a translucent
            // window adds its alpha to the margin it resets. The first and last
            // rows repaint the strip above and below as well. A caller building
            // every row clears the whole target first and needs none of this, and
            // the tier pass repaints no row at all.
            if (!isTierPass && in.dirtyRows && (in.glyphMarginInPixels.width > 0.0f || in.glyphMarginInPixels.height > 0.0f)) {
                const float reachX = in.glyphMarginInPixels.width;
                const float reachY = in.glyphMarginInPixels.height;
                const float gridLeft = in.originInPixels.x;
                const float gridRight = in.originInPixels.x + in.columns * metrics.cellWidth;
                const float rowTop = in.originInPixels.y + row * metrics.cellHeight;
                const float rowBottom = rowTop + metrics.cellHeight;
                const float stripTop = row == 0 ? rowTop - reachY : rowTop;
                const float stripBottom = row == in.rows - 1 ? rowBottom + reachY : rowBottom;
                // Premultiplied on the encoded components, the way the pass's
                // clear stores the same value: the restore pass writes without
                // blending, so the byte here is the byte the compositor reads, and
                // an unmultiplied colour at a sub-unity alpha un-premultiplies past
                // the theme into white.
                Vec4 clearColor = in.uniformDefaultBackground;
                clearColor.x *= in.uniformBackgroundOpacity;
                clearColor.y *= in.uniformBackgroundOpacity;
                clearColor.z *= in.uniformBackgroundOpacity;
                clearColor.w = in.uniformBackgroundOpacity;
                const auto emitStrip = [&](float left, float top, float right, float bottom) {
                    if (right <= left || bottom <= top)
                        return;
                    const std::size_t index = counts.marginStrips++;
                    if (index >= out.marginStripCapacity)
                        return;
                    OverlayInstance& strip = out.marginStrips[index];
                    strip = OverlayInstance {};
                    strip.topLeftInPixels = { left, top };
                    strip.bottomRightInPixels = { right, bottom };
                    strip.color = clearColor;
                };
                emitStrip(gridLeft - reachX, stripTop, gridLeft, stripBottom);
                emitStrip(gridRight, stripTop, gridRight + reachX, stripBottom);
                if (row == 0)
                    emitStrip(gridLeft, rowTop - reachY, gridRight, rowTop);
                if (row == in.rows - 1)
                    emitStrip(gridLeft, rowBottom, gridRight, rowBottom + reachY);
            }

            // DECDWL / DECDHL row geometry. A double-width/height row paints each
            // logical column at twice the advance; double-height additionally shows
            // the top/bottom glyph half. NORMAL keeps xStep 1 (the single-width path).
            const uint8_t lineAttribute = in.lineAttributes ? in.lineAttributes[row] : lineAttributeNormal;
            // A row the model marked as holding no proportional run is asked
            // nothing about one: the run reads below run only on a flowed row,
            // and a caller that read no flags leaves every row to ask its cells.
            // A frame with no spec to read holds no run on any row, and the
            // quiet walk reads none.
            const bool mayHoldRun = textSizeTable != nullptr && (!in.rowFlags || (in.rowFlags[row] & static_cast<std::uint8_t>(Ubiquitty::RowFlag::Flowed)) != 0);
            const TextScale rowScale = Ubiquitty::textScaleForLineAttribute(lineAttribute);
            const bool isDoubleHeightTop = rowScale.half == TextScale::Half::Top;
            const bool isDoubleHeightBottom = rowScale.half == TextScale::Half::Bottom;
            const bool isDoubleWide = rowScale.wide();
            const float xStep = rowScale.xStep();

            // The row's letters, where the primary face is proportional: each
            // cell classified by what it is to the layout, its pen's face and
            // weight, and the run key that breaks a run wherever the pen
            // changes; a cluster's marks travel with their base. Laid out
            // again on the tier pass, whose row the grid pass already walked,
            // rather than read back: one shaping of one row, and nothing here
            // depends on whether the row was dirty.
            // A doubled row is laid out as letters where its letters have a pen
            // to stand at: on the double sheet of a backend that keeps one, or
            // as windows of the doubled master on the mono stream of a backend
            // that cuts sized windows. A backend with neither draws a doubled
            // glyph as a cell-shaped quad, which has no pen of its own, and
            // stretched over a letter's advance it would resample the master
            // and overwrite the neighbour it overlaps; such a row keeps the
            // pitch, one letter a doubled cell.
            const bool isLetterRow = isLaidOutAsLetters && (!isDoubleWide || out.doubleGlyphs != nullptr || atlas.sizedGlyph != nullptr);
            std::span<LetterExtent> rowLetters;
            if (frameLetterRows != nullptr && row >= 0 && row < in.rows)
                frameLetterRows[static_cast<std::size_t>(row)] = 0u;
            if (isLetterRow) {
                rowLetters = std::span<LetterExtent>(frameLetters + static_cast<std::size_t>(row) * static_cast<std::size_t>(in.columns), static_cast<std::size_t>(in.columns));
                letterCells.assign(static_cast<std::size_t>(in.columns), LetterCell {});
                letterPool.clear();
                const Cell* const rowCells = &in.cells[static_cast<std::size_t>(row) * in.columns];
                Pen runPen {};
                std::uint32_t runKey = 0;
                bool hasRunPen = false;
                for (int column = 0; column < in.columns; ++column) {
                    const Cell& letterSource = rowCells[column];
                    const Pen& letterPen = Ubiquitty::cellPen(letterSource);
                    LetterCell& letter = letterCells[static_cast<std::size_t>(column)];
                    if (letterPen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation) || letterSource.codePoint == wideContinuationSentinelCodePoint) {
                        letter.kind = LetterCellKind::Continuation;
                        continue;
                    }
                    if (letterSource.codePoint == 0) {
                        letter.kind = LetterCellKind::Empty;
                        continue;
                    }
                    const bool isSizedClaim = letterPen.extendedAttributes.textSizeSpecID() != 0;
                    const bool isCellBound = isSizedClaim
                        || letterPen.attributes.containsAny({ Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::Emoji })
                        || (letterSource.codePoint >= BoxDrawing::lowestCoveredCodePoint && BoxDrawing::covers(letterSource.codePoint))
                        || isBraille(letterSource.codePoint)
                        || (hasSoftFontInWalk && (letterSource.codePoint - Ubiquitty::VT::drcsCodePointBase) < static_cast<std::uint32_t>(Ubiquitty::VT::drcsMaxCells));
                    if (isCellBound) {
                        // The claim at the row's pitch, or at its anchor row's for a
                        // sized band standing on a row doubled differently.
                        float claimStep = xStep;
                        if (isSizedClaim && in.lineAttributes) {
                            const int anchorRow = row - static_cast<int>(Ubiquitty::penTextBand(&letterPen));
                            if (anchorRow >= 0 && anchorRow < row)
                                claimStep = Ubiquitty::textScaleForLineAttribute(in.lineAttributes[anchorRow]).wide() ? 2.0f : 1.0f;
                        }
                        letter.kind = LetterCellKind::CellBound;
                        letter.widthCells = static_cast<float>(Ubiquitty::claimedRunColumns(letterSource, textSizeTable, in.ambiguousWidth)) * claimStep;
                        continue;
                    }
                    letter.kind = LetterCellKind::Letter;
                    letter.face = fontFace(letterPen.attributes.contains(Ubiquitty::VT::Attribute::Bold),
                        letterPen.attributes.contains(Ubiquitty::VT::Attribute::Italic) || penFontStyle(&letterPen) == Ubiquitty::VT::fontFraktur);
                    letter.weight = static_cast<std::uint32_t>(letterPen.extendedAttributes.weight());
                    if (!hasRunPen || std::memcmp(&letterPen, &runPen, sizeof(Pen)) != 0) {
                        runPen = letterPen;
                        hasRunPen = true;
                        ++runKey;
                    }
                    letter.runKey = runKey;
                    letter.clusterStart = static_cast<std::uint32_t>(letterPool.size());
                    if (letterSource.combiningLength() > 0 || letterSource.clusterID() != 0) {
                        std::array<std::uint32_t, clusterPoolMaximumCodePoints> clusterScratch;
                        int clusterLength = 0;
                        if (letterSource.clusterID() == 0)
                            clusterLength = cellInlineClusterCodePoints(letterSource, clusterScratch.data(), clusterPoolMaximumCodePoints);
                        else if (in.clusterResolver)
                            clusterLength = in.clusterResolver(&letterSource, std::span(clusterScratch), in.clusterResolverContext);
                        if (clusterLength > 0)
                            letterPool.insert(letterPool.end(), clusterScratch.begin(), clusterScratch.begin() + clusterLength);
                    }
                    if (letterPool.size() == letter.clusterStart)
                        letterPool.push_back(letterSource.codePoint);
                    letter.clusterCount = static_cast<std::uint16_t>(std::min<std::size_t>(letterPool.size() - letter.clusterStart, 0xFFFFu));
                }
                layoutRowLetters(letterCells, letterPool, static_cast<double>(metrics.cellWidth), static_cast<double>(xStep),
                    atlas.shapePrimaryRun, atlas.context, letterScratch, rowLetters);
                if (frameLetterRows != nullptr)
                    frameLetterRows[static_cast<std::size_t>(row)] = 1u;
            }

            // Claim routed proportional columns before the decorated-cell walk.
            // The preceding cell's right-neighbour skip-ink tap can ask about an
            // anchor that has not emitted its strip yet; an absent claimed tap
            // keeps that rule solid until the anchor fills the exact strip slice.
            if (metrics.skipInk && mayHoldRun && hasProportionalRouteInWalk) {
                proportionalSkipInkTaps.assign(static_cast<std::size_t>(in.columns), SkipInkTap {});
                proportionalSkipInkClaims.assign(static_cast<std::size_t>(in.columns), 0u);
                proportionalPerGlyphClaims.assign(static_cast<std::size_t>(in.columns), 0u);
                proportionalGlyphInks.clear();
                for (int anchorColumn = 0; anchorColumn < in.columns; ++anchorColumn) {
                    const Cell& candidate = in.cells[static_cast<std::size_t>(row) * in.columns + anchorColumn];
                    const Pen& candidatePen = Ubiquitty::cellPen(candidate);
                    const unsigned specID = candidatePen.extendedAttributes.textSizeSpecID();
                    const Ubiquitty::TextSizeSpec* spec = specID != 0 && textSizeTable
                        ? textSizeTable->lookup(specID)
                        : nullptr;
                    if (!spec || !spec->isProportional || candidate.codePoint == wideContinuationSentinelCodePoint)
                        continue;
                    const int widthCells = std::min(std::max<int>(spec->scale, 1) * (spec->runWidthCells > 0 ? spec->runWidthCells : 1),
                        in.columns - anchorColumn);
                    std::fill_n(proportionalSkipInkClaims.begin() + anchorColumn,
                        static_cast<std::size_t>(widthCells), static_cast<std::uint8_t>(1u));
                    anchorColumn += widthCells - 1;
                }
            } else {
                proportionalSkipInkTaps.clear();
                proportionalSkipInkClaims.clear();
                // Inks are recorded only beside claims, so empty claims mean
                // nothing to reset.
                if (!proportionalPerGlyphClaims.empty()) [[unlikely]] {
                    proportionalPerGlyphClaims.clear();
                    proportionalGlyphInks.clear();
                }
            }

            // The row's slice of the link flag array, or null. Hoisted so the column
            // loop indexes a row pointer instead of recomputing row * columns per
            // cell, and so the absent-array test is loop-invariant rather than a
            // branch on every cell of every frame — the array is absent on most
            // frames and constant for the whole row when it is not.
            const std::uint8_t* const linkRow = hasLinkCells ? in.linkCells + static_cast<std::size_t>(row) * static_cast<std::size_t>(in.columns) : nullptr;

            // A double-width row draws each column at twice the advance, so a run
            // shaped against the single-width pen would land on the wrong cells.
            // Such a row keeps every cell on the mono path.
            // A row laid out as letters draws every letter from the mono sheet at
            // its own pen; the strip is a grid construct and shapes nothing there.
            bool shouldShapeLigatures = hasLigatureSheetInWalk && atlas.shapeLigatureRuns && !isDoubleWide && !isLaidOutAsLetters && !(ablate & AblateLigatureShape);
            // A row holding no operator character cannot produce a run —
            // `ligatureOperatorCharacter` states that rule and the shaper applies
            // it per RUN, after it has already reset four per-column arrays and
            // built its context buffers. Asking it here, per row, is a scan of the
            // cells this walk is about to read anyway, and it takes a row of prose
            // off the shaping path entirely.
            //
            // This asks the character test alone, not `ligatureCellEligible`:
            // over-approximating shapes a row that yields nothing, which costs
            // time, while under-approximating drops a ligature, which is a
            // rendering fault. The cheap test is the safe direction.
            //
            // Every reader of `ligatureRuns` is gated on this same flag, so a row
            // turned off here needs no reset — there is nothing that can read the
            // previous row's entries.
            if (shouldShapeLigatures && !(ablate & AblateLigaturePrescan)) {
                const Cell* const scan = &in.cells[static_cast<std::size_t>(row) * in.columns];
                bool mayRowLigate = false;
                if (in.ligatureParticipation == LigatureParticipation::AllEligibleRuns) {
                    // A healing font shapes plain text, so the cheap early-out is
                    // only "is there any printable at all": a blank row still
                    // skips the distil, and everything else is the walk's to
                    // judge.
                    for (int column = 0; column < in.columns && !mayRowLigate; ++column) {
                        const std::uint32_t codePoint = scan[column].codePoint;
                        mayRowLigate = codePoint > 0x20 && codePoint <= 0x7E;
                    }
                } else {
                    for (int column = 0; column < in.columns && !mayRowLigate; ++column)
                        mayRowLigate = ligatureOperatorCharacter(scan[column].codePoint);
                }
                shouldShapeLigatures = mayRowLigate;
            }
            if (shouldShapeLigatures) {
                // Distil the row for the shaper. The contract carries no model type,
                // so eligibility and shape identity — six rules over five pen axes,
                // all of them Core policy — are answered here rather than by every
                // backend re-deriving them from a struct layout.
                //
                // The second pass over the row is paid only where a run can exist:
                // the prescan above stops at the first operator character and turns
                // the whole row off when it finds none, so a row of prose is
                // distilled not at all. Folding this into the prescan would trade
                // that early exit for a full row every time.
                if (!(ablate & AblateLigatureDistil))
                    distilLigatureRow(std::span(&in.cells[static_cast<std::size_t>(row) * in.columns], static_cast<std::size_t>(in.columns)), std::span<LigatureCell>(ligatureCells));
                if (in.hasRasterTiers) {
                    // The variant each shaped cell's paint selects, resolved once per
                    // distinct pen through the same memo the walk below keeps, so a
                    // row of one colour costs one resolve. The overrides the walk
                    // applies afterwards (link tint, selection, search, the baked
                    // cursor) are not folded in: a strip under a selection keeps the
                    // weight of its text, which is what the selection overlay expects
                    // to tint.
                    const Cell* const rowCells = &in.cells[static_cast<std::size_t>(row) * in.columns];
                    for (int column = 0; column < in.columns; ++column) {
                        LigatureCell& shaped = ligatureCells[static_cast<std::size_t>(column)];
                        if (shaped.codePoint == 0u) {
                            shaped.variant = 0u;
                            continue;
                        }
                        const Pen& pen = rowCells[column].pen;
                        if (std::memcmp(&pen, &memoPen, sizeof(Pen)) != 0) {
                            memoPen = pen;
                            memoResolved = resolveCell(pen, framePalette8, /*reverseAlso=*/false, in.brightenBold);
                            if (in.shouldEnhanceTextContrast && !usesNativeTextVibrancy) {
                                memoResolved = enhanceTextContrast(memoResolved, pen);
                            }
                            memoTier = in.hasRasterTiers ? rasterTierFor(memoResolved.foreground) : 0u;
                        }
                        shaped.variant = makeRasterVariant(0u, memoTier);
                    }
                }
                atlas.shapeLigatureRuns(atlas.context, std::span<const LigatureCell>(ligatureCells),
                    in.originInPixels.x / presentationScale, metrics.cellWidth / presentationScale,
                    drawsSubpixelGlyphs ? GlyphPass::Subpixel : GlyphPass::CellBaked, std::span<LigatureRun>(ligatureRuns));
                // A shaper answering "no runs" leaves the row where a row that never asked would be: every
                // reader is gated on this flag and each would find a length of zero. Held past that answer
                // the flag still costs the row the blank-run batching below, and the prescan admits any row
                // holding one operator character — which in code is every row, most of them ligature-free.
                // A backend without a shaper answers this way for every row (declineLigatureShaping zeroes
                // the lengths), so the whole path folds away where shaping is not implemented.
                bool hasLigatureRun = false;
                for (int column = 0; column < in.columns; ++column) {
                    if (ligatureRuns[static_cast<std::size_t>(column)].length > 0) {
                        hasLigatureRun = true;
                        break;
                    }
                }
                shouldShapeLigatures = hasLigatureRun;
            }

            // The spans are sized only where a ligature sheet exists, and every read
            // below indexes them by column without testing a size, so the flag is
            // what keeps the subscript in range. It can only ever turn off: it is
            // seeded from `hasLigatureSheetInWalk`, and both assignments above sit inside
            // blocks this same flag guards.
            //
            // Checked rather than described because the failure is silent where the
            // standard library does not test the subscript — an arm whose vector
            // checks nothing reads whatever follows the allocation and agrees with
            // the arms that do. A read hoisted out of its guard aborts under a
            // checking library and stays green on every other arm.
            assert(!shouldShapeLigatures
                || (ligatureRuns.size() == static_cast<std::size_t>(in.columns)
                    && ligatureCells.size() == static_cast<std::size_t>(in.columns)));

            // The column whose skip-ink taps sit in selfTap/leftTap/rightTap, so a
            // decorated cell adjacent to its predecessor shifts the window instead
            // of resolving three taps. -2 so the first column never matches.
            int tapWindowColumn = -2;
            // A sized band takes its anchor row's width, so on a row whose own
            // doubling differs from the anchor's it stands somewhere other than
            // the storage it reserves, over the row's own cells there. Those are
            // marked before the walk, because a band can stand over cells the
            // walk reaches before the band's anchor. A band's anchor row is at
            // most six rows up, so a row with no differently doubled row that
            // near above it holds no such band and is not scanned.
            bool hasDisplacedBand = false;
            if (textSizeTable && in.lineAttributes) {
                bool mayHoldDisplacedBand = false;
                for (int above = std::max(0, row - 6); above < row && !mayHoldDisplacedBand; ++above)
                    mayHoldDisplacedBand = Ubiquitty::textScaleForLineAttribute(in.lineAttributes[above]).wide() != isDoubleWide;
                for (int column = 0; mayHoldDisplacedBand && column < in.columns; ++column) {
                    const Cell& bandCell = in.cells[static_cast<std::size_t>(row) * in.columns + column];
                    const Pen& bandPen = Ubiquitty::cellPen(bandCell);
                    const int band = static_cast<int>(Ubiquitty::penTextBand(&bandPen));
                    if (band == 0 || band > row || bandPen.extendedAttributes.textSizeSpecID() == 0
                        || bandPen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                        continue;
                    const bool isAnchorRowWide = Ubiquitty::textScaleForLineAttribute(in.lineAttributes[row - band]).wide();
                    if (isAnchorRowWide == isDoubleWide)
                        continue;
                    if (!hasDisplacedBand) {
                        displacedBandCover.assign(static_cast<std::size_t>(in.columns), 0);
                        hasDisplacedBand = true;
                    }
                    const int step = isAnchorRowWide ? 2 : 1;
                    const int left = column * step;
                    const int right = std::min(in.columns, left + Ubiquitty::claimedRunColumns(bandCell, textSizeTable, in.ambiguousWidth) * step);
                    for (int covered = left; covered < right; ++covered)
                        displacedBandCover[static_cast<std::size_t>(covered)] = 1;
                }
            }
            // Paints a band's reserved storage, or the cells a band stands over,
            // with the cell's background wherever no displaced band stands. Bound
            // once a row: only a row holding a displaced band calls it.
            const auto paintBackgroundWhereNoBandStands = [&](int left, int right, Vec4 foreground, Vec4 background, bool mayUseNativeBackground) { // bool-ok: the cell's own native-background eligibility
                for (int runLeft = left; runLeft < right;) {
                    if (hasDisplacedBand && displacedBandCover[static_cast<std::size_t>(runLeft)]) {
                        ++runLeft;
                        continue;
                    }
                    int runRight = runLeft + 1;
                    while (runRight < right && !(hasDisplacedBand && displacedBandCover[static_cast<std::size_t>(runRight)]))
                        ++runRight;
                    const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                    if (index < out.cellCapacity && out.cells) {
                        CellInstance& inst = out.cells[index];
                        inst = CellInstance {};
                        inst.gridPosition = { static_cast<float>(runLeft), static_cast<float>(row) };
                        inst.widthCells = static_cast<float>(runRight - runLeft);
                        if (isGridOccluded) [[unlikely]] {
                            inst.background = in.occludingColor;
                            inst.background.w = 1.0f;
                        } else {
                            inst.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                            inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                        }
                    }
                    runLeft = runRight;
                }
            };

            // The rest of a row laid out as letters, past its last cell. The
            // letters' advances sum to less than the row's width when the face
            // sets them narrower than the digit cell, and no cell stands there
            // to paint the pane: one background quad from the last cell's right
            // edge to the grid's, in the pane's own background, so every arm
            // paints such a row to its edge as it paints a grid row, and a
            // cell's own quad stays the size of its letter. A row whose letters
            // reach past the edge has nothing left to paint.
            if (isLetterRow && !rowLetters.empty() && metrics.cellWidth > 0.0f) {
                const LetterExtent& last = rowLetters[static_cast<std::size_t>(in.columns - 1)];
                const float rowEndCells = (last.leftInPixels + last.advanceInPixels) / metrics.cellWidth;
                if (rowEndCells < static_cast<float>(in.columns)) {
                    const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                    if (index < out.cellCapacity && out.cells) {
                        CellInstance& inst = out.cells[index];
                        inst = CellInstance {};
                        inst.gridPosition = { rowEndCells, static_cast<float>(row) };
                        inst.widthCells = static_cast<float>(in.columns) - rowEndCells;
                        if (isGridOccluded) [[unlikely]] {
                            inst.background = in.occludingColor;
                            inst.background.w = 1.0f;
                        } else {
                            const Vec4 paneBackground = toVec4(framePalette8.defaultBackground);
                            inst.foreground = nativeSecondaryInk(usesNativeTextVibrancy, toVec4(framePalette8.defaultForeground), paneBackground, in, framePalette8.defaultForeground);
                            inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, paneBackground, in, !isTierPass);
                        }
                    }
                }
            }

            // Whether a run of identical blank cells in the row being built may take the fill
            // below. Every term is a row or frame fact, so the column walk never
            // re-asks: what disqualifies a row is that some column of it draws
            // something the blank cell beside it does not.
            //
            // The cursor is the row's own; selection, search and links are tested
            // as whole-frame gates rather than per span, which gives up the blank
            // tail of a row holding a selection somewhere and keeps the condition
            // one AND of loop-invariant booleans.
            //
            // A shaping row is excluded whole. `ligatureCellEligible` admits the
            // space, so a run crosses one — `x => y` shapes as a single strip —
            // and the covered space draws that strip's slice rather than its own
            // background. The row prescan has already turned shaping off for
            // every row holding no operator character, which is what keeps this
            // from costing the common row anything.
            const bool mayFillBlankRuns = !isDoubleWide && !isGridOccluded
                && !isSelectionInCells && !hasFocusedSearchMatch
                && linkRow == nullptr && !metrics.skipInk
                && !shouldShapeLigatures
                && in.cursor.row != row
                && !(ablate & (AblateCellWrite | AblateResolve));
            // The cell body keeps the text path inline — mono and wide glyphs,
            // combining marks, the decoration pass, the link, selection, search
            // and cursor recolours — and reaches each rarer arm through one branch
            // into a noinline lambda invoked in place: sized, fractional and
            // doubled geometry, proportional runs and ligature slices, colour and
            // box glyphs, soft-font and scripted glyphs. Those arms' locals and
            // code then stay out of the loop's frame and register allocation, and
            // each copies the per-cell scalars it only reads, so none of them has
            // its address taken on the text path.
            for (int column = 0; column < in.columns; ++column) {
                const Cell& storedCell = in.cells[static_cast<std::size_t>(row) * in.columns + column];
                // A proportional run's column draws the run's text in the grid's
                // face, one code point per cell, on a backend with no strip to
                // shape it; the test is a spec-id read on the cell, and every
                // unsized cell answers it in one compare. A backend with the
                // strip keeps the stored chain: the anchor claims the run and
                // draws the strip, the continuations draw nothing of their own.
                Cell proportionalScratch;
                const Cell& cell = (mayHoldRun && !hasProportionalStripRouteInWalk && !hasProportionalGlyphRouteInWalk
                                       && Ubiquitty::cellPen(storedCell).extendedAttributes.textSizeSpecID() != 0
                                       && gridFaceCellOfProportionalRun(in, row * in.columns, column, storedCell, proportionalScratch))
                    ? proportionalScratch
                    : storedCell;
                const Pen& pen = Ubiquitty::cellPen(cell);
                // The tier pass emits one cell: the one whose claim spans the
                // cursor column. Everything before it is walked past — its
                // instances are already in the grid streams — and everything
                // after it is not walked at all. A continuation cell never
                // claims; its lead does, and the lead's claim spans it.
                if (isTierPass) {
                    if (column > in.cursor.column)
                        break;
                    if (pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)
                        || column + Ubiquitty::claimedRunColumns(cell, textSizeTable, in.ambiguousWidth) <= in.cursor.column)
                        continue;
                }
                // A run of cells identical to this blank one draws one background
                // colour and nothing else, so the whole run's instances differ
                // only in their grid position. Emitting them here skips the
                // column body — the glyph routing, the decoration resolve, the
                // per-cell branch tree — for every cell but the first, and the
                // scan that measures the run is the vector compare CellGrid
                // already uses to find an idempotent repaint.
                //
                // Blank is the cell's own business: `defaultBlankCell` is not the
                // pattern, because a run under a non-default background is just as
                // uniform and just as common (a themed pane, a selection-free
                // region). What the pattern must be is a cell whose ink is
                // nothing: no code point, no marks, no attribute that draws.
                //
                // Not on the tier pass: the tier is one cell, and a run filled
                // from it would tint the cursor's neighbours with its colours.
                if (!isTierPass && mayFillBlankRuns && isBlankFillable(cell)) {
                    const int runEnd = firstDifferingCell(&in.cells[static_cast<std::size_t>(row) * in.columns],
                        column, in.columns - 1, cell);
                    if (runEnd - column >= blankRunFillFloor) {
                        if (std::memcmp(&pen, &memoPen, sizeof(Pen)) != 0) {
                            memoPen = pen;
                            memoResolved = resolveCell(pen, framePalette8, /*reverseAlso=*/false, in.brightenBold);
                            if (in.shouldEnhanceTextContrast && !usesNativeTextVibrancy) {
                                memoResolved = enhanceTextContrast(memoResolved, pen);
                            }
                            memoTier = in.hasRasterTiers ? rasterTierFor(memoResolved.foreground) : 0u;
                        }
                        Vec4 runBackground = toVec4(memoResolved.background);
                        runBackground.w = resolvedBackgroundOpacity(runBackground, in);
                        Vec4 runForeground = toVec4(memoResolved.foreground);
                        runForeground.w = pen.foregroundIndex == colorDefault
                            ? in.defaultForegroundAlpha / 255.0f
                            : (pen.foregroundIndex < 256 && in.paletteForegroundAlpha
                                      ? in.paletteForegroundAlpha[pen.foregroundIndex] / 255.0f
                                      : 1.0f);
                        for (int fill = column; fill < runEnd; ++fill) {
                            // Under a displaced band, as the cell walk below skips.
                            if (hasDisplacedBand && displacedBandCover[static_cast<std::size_t>(fill)])
                                continue;
                            const std::size_t index = counts.cells++;
                            if (index >= out.cellCapacity || !out.cells)
                                continue;
                            CellInstance& inst = out.cells[index];
                            if (isLetterRow) {
                                const LetterExtent& letter = rowLetters[static_cast<std::size_t>(fill)];
                                inst.gridPosition = { letter.leftInPixels / metrics.cellWidth, static_cast<float>(row) };
                                inst.widthCells = letter.advanceInPixels / metrics.cellWidth;
                            } else {
                                inst.gridPosition = { static_cast<float>(fill), static_cast<float>(row) };
                                inst.widthCells = 1.0f;
                            }
                            inst.foreground = runForeground;
                            inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, runBackground, in);
                            inst.atlasTopLeft = { 0.0f, 0.0f };
                            inst.atlasBottomRight = { 0.0f, 0.0f };
                        }
                        column = runEnd - 1; // the loop's ++ lands on runEnd
                        continue;
                    }
                }
                if (pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                    continue; // the lead cell draws the whole wide span

                // `writeSizedBlock` had no free attribute bit, so it marks a sized
                // block's anchors and continuations with the wide pair — every band
                // anchor carries `Wide` whatever its glyph is. The attribute
                // therefore answers "does this cell lead a run", not "does this
                // glyph belong on the wide page", and only the code point settles
                // the second. Reading it as the wide page would fetch a real,
                // cached wide glyph for a narrow sized letter: the wrong glyph,
                // drawn confidently.
                const unsigned textSizeSpecID = pen.extendedAttributes.textSizeSpecID();
                const Ubiquitty::TextSizeSpec* sizedSpec = (textSizeSpecID != 0 && textSizeTable)
                    ? textSizeTable->lookup(textSizeSpecID)
                    : nullptr;
                // Kitty's `n`/`d`: the glyph is drawn smaller inside the block it
                // claims. A proportional run carries no sizing keys.
                const bool hasFraction = sizedSpec && !sizedSpec->isProportional
                    && !fractionalRenderArea(*sizedSpec, { 1.0f, 1.0f }).isWholeBlock();
                // A sized block is one glyph drawn across its bands, so every band
                // takes its anchor row's width, doubled or not, whatever its own
                // row's attribute is. Drawn at its own row's width, a band under a
                // DECDWL anchor put the stem of a doubled T under the left half
                // of its crossbar, and a band on a DECDWL row under a plain anchor
                // drew the lower half of the glyph twice as wide as the upper. An
                // anchor scrolled out of view leaves the band at its own row's
                // width, which is all the frame can know.
                bool isCellDoubleWide = isDoubleWide;
                float cellXStep = xStep;
                if (sizedSpec && in.lineAttributes) {
                    const int anchorRow = row - static_cast<int>(Ubiquitty::penTextBand(&pen));
                    if (anchorRow >= 0 && anchorRow < row) {
                        isCellDoubleWide = Ubiquitty::textScaleForLineAttribute(in.lineAttributes[anchorRow]).wide();
                        cellXStep = isCellDoubleWide ? 2.0f : 1.0f;
                    }
                }
                const bool isWide = sizedSpec
                    ? (Ubiquitty::Unicode::Width::forCodePoint(cell.codePoint, in.ambiguousWidth) > 1 || pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji))
                    : pen.attributes.contains(Ubiquitty::VT::Attribute::Wide);
                const bool isBold = pen.attributes.contains(Ubiquitty::VT::Attribute::Bold);
                // Fraktur (SGR 20) has no dedicated face, so it borrows the italic
                // slant; unconfigured alternate-font slots (SGR 11-19) fall back to
                // the primary face.
                const bool isItalic = pen.attributes.contains(Ubiquitty::VT::Attribute::Italic) || penFontStyle(&pen) == Ubiquitty::VT::fontFraktur;
                // SGR 1:n variable-font weight (0 = none, 1..7 → wght axis). A
                // rasterizer without variable fonts ignores it (orthogonal to bold).
                const std::uint32_t weight = static_cast<std::uint32_t>(pen.extendedAttributes.weight());

                // `resolveCell` is a pure function of (pen, framePalette8, brightenBold)
                // and the last two are fixed for the frame, so a cell's resolved
                // colours depend on its Pen alone. Terminal text runs in long spans of
                // one pen — a word, a line of output, a whole blank region — so
                // resolving once per DISTINCT pen and reusing is the difference
                // between per-cell and per-run cost.
                //
                // `Pen` carries explicit tail padding and is memset-zeroed at reset,
                // so it holds no implicit padding bytes and a raw memcmp is an exact
                // equality test rather than an approximation of one.
                if (!(ablate & AblateResolve) && std::memcmp(&pen, &memoPen, sizeof(Pen)) != 0) {
                    memoPen = pen;
                    memoResolved = resolveCell(pen, framePalette8, /*reverseAlso=*/false, in.brightenBold);
                    if (in.shouldEnhanceTextContrast && !usesNativeTextVibrancy) {
                        memoResolved = enhanceTextContrast(memoResolved, pen);
                    }
                    memoTier = in.hasRasterTiers ? rasterTierFor(memoResolved.foreground) : 0u;
                }
                ResolvedCell resolved = memoResolved;
                Vec4 foreground = toVec4(resolved.foreground);
                Vec4 background = toVec4(resolved.background);
                if (usesNativeTextVibrancy && pen.foregroundIndex == 0 && pen.backgroundIndex == colorDefault
                    && !pen.attributes.contains(VT::Attribute::Reverse) && !pen.attributes.contains(VT::Attribute::Hidden)
                    && framePalette8.defaultForeground.red > framePalette8.defaultBackground.red) {
                    // ANSI black is a semantic palette choice; true-color and
                    // extended grayscale entries retain their animation ramp.
                    foreground.x = framePalette8.defaultForeground.red / 255.0f * 0.3f;
                    foreground.y = framePalette8.defaultForeground.green / 255.0f * 0.3f;
                    foreground.z = framePalette8.defaultForeground.blue / 255.0f * 0.3f;
                }
                std::uint32_t foregroundTier = memoTier;
                // Set by any override below that replaces the foreground, so the
                // tier is recomputed from what the cell draws with rather than
                // from the pen it was memoised under.
                bool didOverrideForeground = false;

                // Foreground alpha = palette opacity (indexed / default) × the SGR
                // 5/6 blink curve, folded into foreground.w. RGB-direct foregrounds
                // carry no protocol alpha, so the palette factor stays 1.0; both
                // inputs default to 1.0.
                float foregroundAlpha = 1.0f;
                const uint16_t foregroundIndex = pen.foregroundIndex;
                if (foregroundIndex == colorDefault)
                    foregroundAlpha = in.defaultForegroundAlpha / 255.0f;
                else if (foregroundIndex < 256 && in.paletteForegroundAlpha)
                    foregroundAlpha = in.paletteForegroundAlpha[foregroundIndex] / 255.0f;
                // Kept apart from the product, for the decoration pass: a rule whose
                // colour comes from a palette slot other than the foreground's takes
                // that slot's opacity times this, and cannot get at it once the two
                // are multiplied together.
                float blinkCurve = 1.0f;
                if (in.resolveBlink && pen.attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
                    const bool isFastBlink = pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
                    const uint32_t period = isFastBlink ? (Cursor::Blink::caretPeriodTicks / 2) : Cursor::Blink::caretPeriodTicks;
                    blinkCurve = blinkAlpha(in.blinkTicks, period);
                }
                foregroundAlpha *= blinkCurve;
                if (pen.attributes.contains(VT::Attribute::Hidden)) {
                    foregroundAlpha = 0;
                }
                foreground.w = foregroundAlpha;

                // OSC 8 / regex link cell: a flagged cell carries an always-on
                // underline and takes the link colour for its text and that
                // underline.
                const LinkState linkState = linkRow != nullptr ? static_cast<LinkState>(linkRow[column]) : LinkState::None;
                // An action cell is not drawn as a link: resting it is plain text
                // on its plate, hovered its plate lightens below.
                const bool isActionCell = linkState == LinkState::RestingAction || linkState == LinkState::HoveredAction;
                const bool isLinkCell = linkState != LinkState::None && !isActionCell;
                const bool isHoveredLink = linkState == LinkState::Hovered;
                if (linkState == LinkState::HoveredAction) [[unlikely]] {
                    // The hover affordance of a control: its plate a step towards
                    // the text's colour, which reads as lit on a dark plate and as
                    // pressed on a light one.
                    constexpr float hoverLift = 0.18f;
                    background.x += (foreground.x - background.x) * hoverLift;
                    background.y += (foreground.y - background.y) * hoverLift;
                    background.z += (foreground.z - background.z) * hoverLift;
                }
                // The hovered run reads in the accent colour rather than the theme's
                // resting link colour, which is what makes the run under the pointer
                // separable from every other link on screen.
                const Vec4& linkTint = isHoveredLink ? in.linkHoverColor : in.linkColor;
                // The text colour before the link recolour, kept for the underline
                // resolve. An SGR underline with `colorDefault` follows the text, and
                // "the text" there means the colour the cell's own pen asked for — not
                // the link tint painted over it. Resolving against the tinted colour
                // instead turns an explicitly styled underline inside a link the
                // link's colour, which is the cell losing the styling it requested.
                //
                // Written only on the link branch, so an ordinary cell pays nothing:
                // the reference below binds straight to `foreground`.
                Vec4 foregroundBeforeLink {};
                if (isLinkCell) [[unlikely]] {
                    foregroundBeforeLink = foreground;
                    foreground.x = linkTint.x;
                    foreground.y = linkTint.y;
                    foreground.z = linkTint.z;
                    // Keep the resolved alpha (palette opacity × blink).
                    didOverrideForeground = true;
                }

                // A selected cell: under OSC 19 its glyph takes the host colour,
                // overriding the SGR/link colour above, with the resolved alpha kept;
                // under a fill behind the text the fill is its background, opaque,
                // the way a text view draws a selection. Gated so the default path
                // is byte-identical.
                // A proportional run's anchor claims every cell of the run, and a
                // selection ending inside the run covers only some of them, so its
                // fill is not the anchor's: the run route draws it over the
                // selected letters' own extent on the pixel-rect stream, and an
                // arm without that stream fills the selected cells of the claim
                // where its tiles are emitted. Baked on the anchor, the run lit
                // whole or not at all by whether its first cell was selected.
                const bool isSelectionFillPerClaimedCell = isSelectionBehindText && sizedSpec && sizedSpec->isProportional;
                if (isSelectionInCells && isCellSelected(row, column) && !penRichTextCovered(&pen)) {
                    if (selectionForegroundActive) {
                        foreground.x = in.selectionForeground.x;
                        foreground.y = in.selectionForeground.y;
                        foreground.z = in.selectionForeground.z;
                        didOverrideForeground = true;
                    }
                    if (isSelectionBehindText && !isSelectionFillPerClaimedCell) {
                        // The selection's wash over the cell's own background,
                        // by the fill's opacity: a coloured background shows
                        // through it as itself.
                        const float wash = in.selectionColor.w;
                        background.x += (in.selectionColor.x - background.x) * wash;
                        background.y += (in.selectionColor.y - background.y) * wash;
                        background.z += (in.selectionColor.z - background.z) * wash;
                        background.w = 1.0f;
                    }
                }

                // The focused search match: its highlight is bright and nearly
                // opaque, so the glyph over it takes a contrasting ink instead of
                // its own colour. Gated so the default path is byte-identical.
                if (hasFocusedSearchMatch && row < static_cast<int>(searchMatchColumn1.size()) && column >= searchMatchColumn0[static_cast<std::size_t>(row)] && column <= searchMatchColumn1[static_cast<std::size_t>(row)]) {
                    foreground.x = searchMatchInk;
                    foreground.y = searchMatchInk;
                    foreground.z = searchMatchInk;
                    background.x = in.searchMatchHighlight.x;
                    background.y = in.searchMatchHighlight.y;
                    background.z = in.searchMatchHighlight.z;
                    background.w = 1.0f;
                    didOverrideForeground = true;
                }

                // `claimedRunColumns` is Core's single answer for how many columns
                // an anchor takes, and it already subsumes the wide case, so a
                // sized block's scale*width claim needs no second rule beside it.
                // A cell carrying no size id resolves through the Wide attribute
                // alone, so an unsized grid claims exactly what that attribute
                // spells and no table has to be reachable for it to be right.
                const int claimedColumns = Ubiquitty::claimedRunColumns(cell, textSizeTable, in.ambiguousWidth);
                // The cursor's colours: the lerp toward the cursor colour by the
                // blink weight, and the glyph inverted under it. They ride the same
                // instances the cell's own glyph does — a wide pair, a strip slice,
                // a sized run, its rules — drawn last on the tier pass, over
                // whatever the grid holds there.
                background.w = isBackgroundOpacityUniformInWalk ? uniformCellBackgroundOpacityInWalk : resolvedBackgroundOpacity(background, in);
                if (isTierPass) {
                    const CursorTextInk ink = in.cursor.hasTextColor ? CursorTextInk { CursorTextInk::Source::Color, in.cursor.textRGB }
                        : in.cursor.shouldInvertText                 ? CursorTextInk { CursorTextInk::Source::Inverse }
                                                                     : CursorTextInk {};
                    const CursorBake tier = bakeBlockCursor(foreground, background, in.cursor.rgb, in.cursor.mixA, ink);
                    foreground = tier.foreground;
                    background = tier.background;
                    didOverrideForeground = true;
                }
                const bool mayUseNativeBackground = !isTierPass;
                if (didOverrideForeground && in.hasRasterTiers) [[unlikely]]
                    foregroundTier = rasterTierFor(foreground);
                // The raster variant every glyph this cell fetches is keyed and
                // rasterised under; the phase joins it once the pen is snapped.
                const std::uint32_t cellVariantStyle = makeRasterVariant(0u, foregroundTier);

                // DECDWL / DECDHL placement. The logical column lands at
                // column*cellXStep; a cell whose 2x lead starts at or past the last
                // column paints nothing (it would spill into the right padding) —
                // drop it. The straddling cell is clipped to the grid content width;
                // cellClipFrac trims the glyph UV.
                const float gridX = column * cellXStep;
                // Where the cell stands, in cells from the grid's left edge: the
                // letter the row's layout put there when the primary face is
                // proportional, else the grid's column. Every pixel the cell
                // emits is placed from this; `gridX` keeps the grid's own count
                // for what is still counted in columns — the edge a doubled row
                // is cut at, the cover a displaced band leaves.
                const float cellGridX = isLetterRow ? rowLetters[static_cast<std::size_t>(column)].leftInPixels / metrics.cellWidth : gridX;
                // A band standing away from the storage it reserves on its row
                // still owns that storage, whose continuations draw nothing: it is
                // painted with the band's background, or no instance covers it
                // and the surface shows whatever it was cleared to. Painted here,
                // ahead of the edge test below, because a band standing past the
                // grid's edge has its storage on the grid all the same, and only
                // where no band stands: a neighbouring block's band can stand on
                // this one's storage, and the walk has drawn it already.
                const bool isBandDisplaced = sizedSpec && cellXStep != xStep;
                if (isBandDisplaced) {
                    const int reservedLeft = column * static_cast<int>(xStep);
                    paintBackgroundWhereNoBandStands(reservedLeft,
                        std::min(in.columns, reservedLeft + claimedColumns * static_cast<int>(xStep)), foreground, background, mayUseNativeBackground);
                }
                if (isCellDoubleWide && gridX >= static_cast<float>(in.columns))
                    continue;
                const float fullWidthCells = static_cast<float>(claimedColumns) * cellXStep;
                // What the glyph itself spans at its scale. A `w` wider than the
                // text reserves cells the glyph does not reach: the glyph keeps its
                // size at the block's left and the rest of the claim is background,
                // which is what the sharp windows draw, since their master is only
                // the glyph's own size. A `w` narrower than the text squeezes it
                // into the claim.
                const float glyphWidthCells = sizedSpec
                    ? std::min(fullWidthCells, static_cast<float>(sizedSpec->scale) * (isWide ? 2.0f : 1.0f) * cellXStep)
                    : fullWidthCells;
                float cellVisibleWidth = fullWidthCells;
                if (isCellDoubleWide && gridX + fullWidthCells > static_cast<float>(in.columns)) {
                    cellVisibleWidth = static_cast<float>(in.columns) - gridX;
                    if (cellVisibleWidth < 0.0f)
                        cellVisibleWidth = 0.0f;
                }
                const float cellClipFrac = (fullWidthCells > 0.0f) ? cellVisibleWidth / fullWidthCells : 1.0f;
                // What the cell spans on screen, in cells: a letter's advance from
                // the row's layout, else its visible claim at the pitch.
                const float cellSpanCells = isLetterRow ? rowLetters[static_cast<std::size_t>(column)].advanceInPixels / metrics.cellWidth : cellVisibleWidth;
                FractionalRenderArea fractionArea;
                Vec2 fractionBlockOrigin {};
                float fractionBlockRows = 1.0f;
                int fractionBlockTopRow = row;
                if (hasFraction) [[unlikely]] {
                    // Sized blocks own their height. A one-row block on DECDHL
                    // instead shares the doubled master with the other half-row.
                    const bool isDoubledPair = sizedSpec->scale == 1 && (isDoubleHeightTop || isDoubleHeightBottom);
                    fractionBlockRows = isDoubledPair ? 2.0f : static_cast<float>(sizedSpec->scale);
                    const unsigned band = isDoubledPair ? (isDoubleHeightBottom ? 1u : 0u)
                                                        : std::min<unsigned>(Ubiquitty::penTextBand(&pen), sizedSpec->scale - 1u);
                    fractionBlockTopRow = row - static_cast<int>(band);
                    fractionBlockOrigin = { in.originInPixels.x + cellGridX * metrics.cellWidth,
                        in.originInPixels.y + (static_cast<float>(row) - static_cast<float>(band)) * metrics.cellHeight };
                    fractionArea = fractionalRenderArea(*sizedSpec, { fullWidthCells * metrics.cellWidth, fractionBlockRows * metrics.cellHeight });
                }
                // A displaced band stands in front of the row's own cells. One it
                // covers whole draws nothing, as the cells past a doubled row's
                // edge draw nothing: painted after the band, its background would
                // cover the glyph wherever the ink is baked into the cell quads.
                // One it covers in part draws as any cell does and is then cut at
                // the band's edge, every instance it emitted in every stream, so
                // the passes that draw glyphs apart from their cells cannot draw
                // over the band either.
                BandEdgeCut bandEdgeCut;
                if (hasDisplacedBand && !isBandDisplaced) {
                    const int spanLeft = static_cast<int>(gridX);
                    const int spanRight = std::min(in.columns, static_cast<int>(std::ceil(gridX + cellVisibleWidth)));
                    int coveredLeft = spanRight;
                    int coveredRight = spanLeft;
                    for (int covered = spanLeft; covered < spanRight; ++covered) {
                        if (displacedBandCover[static_cast<std::size_t>(covered)] == 0)
                            continue;
                        coveredLeft = std::min(coveredLeft, covered);
                        coveredRight = std::max(coveredRight, covered + 1);
                    }
                    if (coveredLeft < coveredRight) {
                        if (coveredLeft <= spanLeft && coveredRight >= spanRight) {
                            paintBackgroundWhereNoBandStands(spanLeft, spanRight, foreground, background, mayUseNativeBackground);
                            continue;
                        }
                        // The part kept is the side the band leaves. A band inside
                        // the cell's span leaves two, and the cell keeps the one
                        // where its glyph begins.
                        const float columnLeft = in.originInPixels.x;
                        if (coveredLeft > spanLeft)
                            bandEdgeCut.keepLeftOf(columnLeft + static_cast<float>(coveredLeft) * metrics.cellWidth);
                        else
                            bandEdgeCut.keepRightOf(columnLeft + static_cast<float>(coveredRight) * metrics.cellWidth);
                        bandEdgeCut.begin(out, counts, in.originInPixels.x, metrics.cellWidth);
                    }
                }

                // An occluded grid emits a background-only cell and nothing else. The
                // early-out is placed here rather than at the emit below because
                // everything between the two — the glyph fault, the shaping classify,
                // the decoration resolve — produces output that is not merely
                // invisible but WRONG: the glyph pass draws over the letterbox.
                if (isGridOccluded) [[unlikely]] {
                    const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                    if (index < out.cellCapacity && out.cells) {
                        CellInstance& inst = out.cells[index];
                        inst = CellInstance {};
                        inst.gridPosition = { cellGridX, static_cast<float>(row) };
                        inst.widthCells = cellSpanCells;
                        inst.background = in.occludingColor;
                        // Opaque regardless of window translucency: the letterbox hides
                        // the grid, and a desktop showing through it would defeat the
                        // mode rather than soften it.
                        inst.background.w = 1.0f;
                    }
                    continue;
                }

                // Subpixel mono glyph pass (opt-in): a plain mono inked glyph is
                // emitted as a separate pixel-snapped GlyphInstance at its subpixel
                // phase, its CellInstance carrying background only. Everything else —
                // wide / emoji / box-drawing / double row / combining / blank —
                // keeps the cell-baked phase-0 path below.
                // A cell the shaper drew as part of a run takes its slice of that
                // run's strip and no other path: its glyph does not exist on its
                // own anywhere, so a mono or colour fetch for it would draw a
                // character the shaped image already contains.
                // The glyph the shaper chose, when the producer named one and a
                // backend can draw it by that name. It is the same letterform the
                // strip holds, keyed by the face's own identity instead of by the
                // text that produced it — which is what bounds the slots: fragments
                // do not repeat, a repertoire does.
                //
                // Only a substitution takes this route. A cell that carries an
                // N-cell cluster's whole image needs a slot N cells wide, and a
                // mono slot is one cell and its margins, so a wider answer is never
                // fetched and the run keeps the strip it already drew — which
                // costs nothing, because the population that outgrows the sheet is
                // the substitutions.
                // Indexed under the flag, never before it. `ligatureRuns` is sized
                // only when the frame carries a ligature sheet, so on a frame
                // without one it is EMPTY and every subscript is out of bounds —
                // which is the invariant this file states where the span is
                // declared: shouldShapeLigatures gates every read.
                AtlasGlyph shapedImage {};
                if (shouldShapeLigatures && ligatureRuns[static_cast<std::size_t>(column)].length > 0
                    && ligatureRuns[static_cast<std::size_t>(column)].hasShapedGlyph
                    && ligatureRuns[static_cast<std::size_t>(column)].shaped.coveredCells == 1
                    && atlas.rasteriseShapedGlyph != nullptr && !(ablate & AblateMonoFetch)) {
                    const LigatureRun& columnRun = ligatureRuns[static_cast<std::size_t>(column)];
                    // The phase is derived here rather than read from the mono
                    // path's, because that path's own gate depends on whether this
                    // cell ends up strip-covered, and this answer is what decides
                    // that. A shapeable cell is printable, single-width and
                    // unadorned by construction, so the gate reduces to whether
                    // the frame positions glyphs subpixel at all.
                    //
                    // The phase bucket, and not the strip's own fractional pen, is
                    // what keeps the key bounded — which is the point of keying by
                    // identity rather than a matter of matching the neighbours. A
                    // slot baked at a strip's residual belongs to one column phase
                    // of one run, so the key would have to carry that residual and
                    // the space would be unbounded again for the same reason a
                    // fragment key is. A face needing a finer residual than the
                    // buckets carry is a question about how many buckets there
                    // are, not about this pen.
                    const std::uint32_t shapedPhase = drawsSubpixelGlyphs
                        ? static_cast<std::uint32_t>(phaseForPen(in.originInPixels.x + cellGridX * metrics.cellWidth,
                              in.subpixelPhaseCount, in.subpixelPhaseRounding)
                                  .phase)
                        : 0u;
                    shapedImage = shapedGlyphForCell(atlas, mono, columnRun.shaped.glyphID,
                        fontFace(isBold, isItalic), cellVariantStyle | shapedPhase);
                }
                const bool isLigatureCovered = shouldShapeLigatures
                    && ligatureRuns[static_cast<std::size_t>(column)].length > 0
                    && ligatureRuns[static_cast<std::size_t>(column)].strip.present && !shapedImage.present;
                // Where this cell's ink lands on screen, for an underline that
                // follows the ink past the cell box: filled by whichever quad
                // path measured the ink, left unset by the paths that did not.
                bool hasInkExtent = false;
                float inkScreenLeft = 0.0f;
                float inkScreenRight = 0.0f;
                // Where a proportional run's text lies inside its claim, for the
                // rules beneath it: from the aligned pen to the pen plus the
                // laid-out width, in screen pixels, cut at the claim. Set only
                // where the anchor laid the run out; a rule with no layout to
                // read spans the claim.
                bool hasProportionalTextExtent = false;
                float proportionalTextLeft = 0.0f;
                float proportionalTextRight = 0.0f;
                // A proportional run's anchor on a backend with the strip: the
                // strip is the run's whole image, so the anchor fetches no glyph
                // of its own — a fetch would draw its first letter across the
                // width it claims. Whether the strip arrives decides only
                // between the image and a bare background.
                // A per-glyph route needs the wide sheet to draw into and a
                // shaper to ask; a strip route needs only the strip. Either makes
                // the anchor the run's one emitter.
                const bool isProportionalAnchor = mayHoldRun && sizedSpec && sizedSpec->isProportional
                    && (hasProportionalGlyphRouteInWalk || hasProportionalStripRouteInWalk)
                    && cell.codePoint != wideContinuationSentinelCodePoint;
                const bool isSizedTextRunAnchor = sizedSpec && sizedSpec->isTextRun
                    && cell.codePoint != wideContinuationSentinelCodePoint;
                // A box-drawing cell is geometry, not a glyph: neighbours must meet
                // with no seam, and a rasterised glyph carries its own bearings and
                // antialiasing and will not tile. The font wins only where the
                // backend says its glyph reaches the cell edges.
                // Asked once. `covers` walks five disjoint code-point ranges and
                // probes `decompose` inside two of them, and both flags below need
                // the answer — the box path because it draws geometry, the plain
                // mono path because it must not.
                const bool isBoxCodePoint = cell.codePoint >= BoxDrawing::lowestCoveredCodePoint
                    && BoxDrawing::covers(cell.codePoint);
                bool isBoxDrawing = hasBoxSheetInWalk && (!isCellDoubleWide || hasFraction) && !isWide
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)
                    && isBoxCodePoint;
                // Braille, on the same terms and by the same test, because the
                // two ranges are disjoint and both draw into the fill stream.
                // Not folded into the box family: a box stroke is placed inside
                // the cell's own snapped rect and a braille dot is placed on the
                // grid, which is the whole of the difference and the reason for
                // the separate emitter.
                // One column of an unscaled row, which is the case the even
                // pitch is a claim about: a DECDWL row and a sized block each
                // paint their cells at a different advance from the rows around
                // them, so a lattice drawn across one would meet its neighbours
                // nowhere. Those keep the font's braille, as the box family's
                // sized cells keep their own geometry.
                const bool isBrailleLattice = hasBoxSheetInWalk && in.drawsBrailleGrid
                    && !isCellDoubleWide && !isWide && !hasFraction && claimedColumns == 1
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)
                    && isBraille(cell.codePoint);
                // A dash is in the box family only where a box FONT can draw it.
                // The fill stream cannot express a dash's gaps — the decomposition
                // carries the count, the instances do not — so a backend with no
                // box-font hook, or a face without the glyph, keeps the dash on
                // the text lane, where the font tiles it as designed. Probed here,
                // ahead of every flag that keys on the routing, so a demoted cell
                // is a text cell to the whole of the loop below.
                AtlasGlyph dashGlyph;
                bool isDashOnTextLane = false;
                if (isBoxDrawing && BoxDrawing::isDash(cell.codePoint)) {
                    if (atlas.boxGlyph && !(ablate & AblateBoxFetch))
                        dashGlyph = atlas.boxGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic));
                    if (!dashGlyph.present) {
                        isBoxDrawing = false;
                        isDashOnTextLane = true;
                    }
                }
                // A DECDLD soft-font cell (Core/VT/SoftFont.h). One subtract and one
                // compare on a code point already in a register: `drcsCodePointBase`
                // sits in plane 15, far above anything real text carries, so the
                // unsigned wrap puts every ordinary glyph out of range without a
                // second test. `hasSoftFontInWalk` is loop-invariant and leads, so a session
                // that never downloaded one pays a perfectly predicted branch.
                const bool isSoftFontCell = hasSoftFontInWalk && cell.codePoint != 0
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)
                    && (cell.codePoint - Ubiquitty::VT::drcsCodePointBase) < static_cast<std::uint32_t>(Ubiquitty::VT::drcsMaxCells);
                // A cell carrying combining marks belongs here with the rest. Its
                // glyph is monochrome and foreground-tinted like any other, so it
                // takes the row's phase — a cluster held back to phase 0 sits up to
                // a phase-width left of where the unaccented letters beside it in
                // the same word were drawn. It also needs the native-size blit
                // rather than the cell quad: a stacked diacritic overruns the cell
                // box, and the cell-baked path crops at the box edge where the
                // glyph pass lets the overhang spill into the neighbour.
                //
                // Only the atlas call differs, and the cluster branch below is
                // tested before the mono cache, so a cluster never reaches a key
                // built from its base code point alone.
                const bool isSubpixelGlyph = !isLigatureCovered && !isProportionalAnchor && drawsSubpixelGlyphs && !isWide && !pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji) && !isCellDoubleWide && cell.codePoint != 0 && cell.codePoint != wideContinuationSentinelCodePoint && cell.codePoint != ' '
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden) && (!isBoxCodePoint || isDashOnTextLane) && !isSoftFontCell
                    && !hasFraction;
                // A fractionally scaled sized glyph. Its picture is smaller than
                // the block and stands where `v` and `h` put it, which neither a
                // cell quad nor a subpixel slot can express: the glyph takes the
                // cell-box raster and draws through the glyph stream as a box of
                // its own, the way a scripted glyph does, and its cell is
                // background only. Downloaded bitmaps and box-sheet glyphs use
                // the same mapping; neither is a request for a font raster.
                const bool isFractionalGlyph = hasFraction && out.glyphs != nullptr
                    && !isLigatureCovered && !isWide
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)
                    && cell.codePoint != 0 && cell.codePoint != wideContinuationSentinelCodePoint && cell.codePoint != ' '
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden);
                // SGR 73/74 where glyphs are placed at whole pixels.
                //
                // The shrink is a box, and a cell quad has none to give: it draws
                // its glyph across the whole cell, so a superscript baked into one
                // comes out the size of the letter beside it. Only the subpixel
                // stream below carried the box, which left the arms that rasterise
                // at whole pixels drawing scripted runs full size — the software
                // reference among them, and that reference is the picture the GPU
                // arms are compared against.
                //
                // The same exclusions the subpixel line names, for the same
                // reasons, minus its own: a cell reaches this only where that one
                // did not. `!isSubpixelGlyph` leads so the pen is not read at all
                // on a backend whose cells nearly all take the other route.
                const bool isScriptedOwnSheetGlyph = !isSubpixelGlyph && !isFractionalGlyph && out.glyphs != nullptr
                    && penScript(&pen) != Ubiquitty::VT::scriptNone
                    && !isLigatureCovered && !isProportionalAnchor && !isWide
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji) && !isCellDoubleWide
                    && cell.codePoint != 0 && cell.codePoint != wideContinuationSentinelCodePoint && cell.codePoint != ' '
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden)
                    && (!isBoxCodePoint || isDashOnTextLane) && !isSoftFontCell;
                // A two-cell glyph goes to its own sheet only when the caller
                // offers one and the atlas reports a wide slot; otherwise it stays
                // on the cell, which is correct for a single-sheet backend.
                // Emoji use coverage when the backend has no colour atlas.
                const bool isWideOnOwnSheet = isWide && !(hasColorSheetInWalk && pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji))
                    && out.wideGlyphs != nullptr
                    && in.glyphWideSlotWidthInPixels > 0.0f && in.glyphBoxWidthInPixels > 0.0f;
                // A fractional two-cell glyph leaves its cell for the wide stream on
                // either kind of wide page: one whose slots carry margins, placed as
                // the stretched quad below is, or one whose rect is the cell box,
                // which the cell quad would have stretched over the whole claim.
                const bool isFractionalWide = hasFraction && isWide && out.wideGlyphs != nullptr
                    && !(hasColorSheetInWalk && pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji))
                    && !isLigatureCovered
                    && cell.codePoint != 0 && cell.codePoint != wideContinuationSentinelCodePoint && cell.codePoint != ' '
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden);
                // An emoji draws from the RGBA colour sheet, whose glyphs carry their
                // own colours and are never tinted by the cell foreground. A cell
                // quad samples one texture and cannot reach a second, so the glyph
                // leaves the cell only when the caller offers the stream to put it
                // in. `hasColorSheetInWalk` is loop-invariant and tested first, so a
                // single-sheet backend pays one predicted branch per cell and never
                // reads the attributes again.
                const bool isColorOnOwnSheet = hasColorSheetInWalk && pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)
                    && cell.codePoint != 0 && cell.codePoint != ' '
                    && cell.codePoint != wideContinuationSentinelCodePoint
                    && !pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden);
                // A two-cell glyph on a phased frame is placed the way a mono one
                // is: rasterised at its pen's phase and blitted texel for pixel
                // from the snapped pen. A cell advance is fractional, so two-cell
                // pens land between pixels as often as one-cell pens do, and a
                // slot drawn at phase zero and scaled onto the fractional cell is
                // resampled — the glyph CoreText would draw at that pen, blurred
                // by the sampler. Sized and doubled cells keep their own routes.
                // The buckets are the wide page's own, which a backend that grows
                // its two-cell glyphs draws on a coarser grid than its mono text.
                const bool isWidePhased = isWideOnOwnSheet && drawsSubpixelGlyphs && !isCellDoubleWide
                    && !(sizedSpec && sizedSpec->scale >= 2) && !hasFraction;
                SubpixelPhase phaseSnap;
                if (isSubpixelGlyph || isWidePhased) {
                    phaseSnap = phaseForPen(in.originInPixels.x + cellGridX * metrics.cellWidth,
                        isWidePhased ? widePhaseCount(in, atlas, cell.codePoint, fontFace(isBold, isItalic))
                                     : in.subpixelPhaseCount,
                        in.subpixelPhaseRounding);
                }
                const std::uint32_t widePhase = isWidePhased ? static_cast<std::uint32_t>(phaseSnap.phase) : 0u;

                // Glyph atlas UV (minUV == maxUV ⇒ background only). The subpixel
                // path faults the slot at its phase; otherwise phase 0.
                AtlasGlyph glyph;
                // The glyph's right edge in the atlas before the straddle trim.
                float glyphUnclippedMaxU = 0.0f;
                // At cell scope rather than the lookup's, because the colour
                // emission arm re-probes the cluster for its sharp sized windows —
                // resolving it twice would cross the resolver's indirect call a
                // second time for data already in hand. Left uninitialised: every
                // reader is bounded by `clusterCount`, and clearing the array would
                // store 64 bytes on every cell for entries nothing reads.
                std::array<std::uint32_t, clusterPoolMaximumCodePoints> clusterCodePoints;
                int clusterCount = 0;
                // True only when the backend returned a real 2x master with its region
                // already applied; the V-halving below is the 1x fallback and must not
                // also run on one.
                bool hasDoubleMaster = false;
                // Set once the master is in hand: a doubled glyph rides the double
                // stream only when the caller offered one, and the master is what
                // there is to put in it. Without a master the 1x fallback stays on
                // the cell, so a backend with a sheet and a font that lacks the
                // codepoint still draws.
                bool isDoubleOnOwnSheet = false;
                const bool shouldFetchGlyph = !(ablate & AblateGlyphFetch) && !isSizedTextRunAnchor;
                // Set when the font declined the cell and its geometry is drawn
                // instead: the cell then carries background only and the quads
                // below are the whole of its ink.
                bool usesProceduralBox = false;
                if (!shouldFetchGlyph) {
                    // no fetch; the cell keeps an absent glyph
                } else if (isSoftFontCell) {
                    [&] [[gnu::noinline]] {
                        // A downloaded bitmap, not a shaped code point, so it never
                        // reaches a font: Core scales the matrix into a mono slot and the
                        // cell blits it through the ordinary quad, taking the same
                        // foreground, inverse, dim, selection recolour and cursor as any
                        // inked cell. Ahead of the box arm because a DRCS code point sits
                        // in plane 15 where no box range reaches, so the order is a fact
                        // about which test is cheaper rather than a precedence rule.
                        //
                        // Absent covers all three of DEC's undefined cases — a position
                        // never downloaded, a font erased by RIS or a DECDLD wipe, and a
                        // cell outliving the font it was drawn from — and the cell paints
                        // background only, which is what DEC leaves them as and the only
                        // safe answer for the third.
                        glyph = softFontGlyph(atlas, mono, *in.softFont, static_cast<int>(cell.codePoint - Ubiquitty::VT::drcsCodePointBase));
                        // Counted where the bitmap arrives rather than at the cell quad
                        // it blits through: a DRCS cell is one of those quads and nothing
                        // downstream can tell it from a cell of text.
                        counts.softFontGlyphs += glyph.present ? 1u : 0u;
                    }();
                } else if (isBoxDrawing) {
                    [&, isBold, isItalic] [[gnu::noinline]] {
                        if (!(ablate & AblateBoxFetch))
                            // The procedural set never had a font glyph to fetch, and
                            // asking is a crossing per cell for an answer Core already
                            // holds: a block, shade or sextant is inset by the same
                            // bearings that make text readable, so it does not tile and
                            // the decomposition wins whatever the font offers. Half the
                            // box scene is this set. A dash arriving here already
                            // fetched its glyph in the routing probe above; fetching
                            // again would be a second hook crossing for the same slot.
                            glyph = dashGlyph.present ? dashGlyph
                                : (atlas.boxGlyph && !BoxDrawing::prefersProcedural(cell.codePoint))
                                ? atlas.boxGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic))
                                : AtlasGlyph {};
                        usesProceduralBox = !glyph.present;
                    }();
                } else if (isBrailleLattice) {
                    // No fetch and no fallback. The lattice is not an opinion
                    // about a glyph that came back badly — it is a different
                    // drawing of the character, and asking a face first would
                    // spend a hook crossing on an answer nothing reads.
                } else if (cell.codePoint != wideContinuationSentinelCodePoint && !isLigatureCovered && !isProportionalAnchor) {
                    if (!(ablate & AblateClusterRead) && (cell.combiningLength() > 0 || cell.clusterID() != 0)) {
                        // The inline case first, because it is the common one and it
                        // needs nothing this builder does not already hold. The
                        // resolver is a function pointer into the frontend, so every
                        // call through it is an indirect call the compiler cannot see
                        // past — and for a cell whose marks fit in `combining` it
                        // would fetch data already in the cell being read.
                        if (cell.clusterID() == 0)
                            clusterCount = cellInlineClusterCodePoints(cell, clusterCodePoints.data(), clusterPoolMaximumCodePoints);
                        else if (in.clusterResolver)
                            clusterCount = in.clusterResolver(&cell, std::span(clusterCodePoints), in.clusterResolverContext);
                    }
                    // DWL/DHL rows fetch the 2x master first, so the DHL halving
                    // below crops a doubled raster; a backend without a master
                    // (present == false) falls back to the 1x glyph, degrading to
                    // stretched rather than blank.
                    if (isColorOnOwnSheet) {
                        [&, clusterCount] [[gnu::noinline]] {
                            // The whole cluster, so a flag / ZWJ family / skin tone /
                            // VS16 sequence shapes into one colour glyph. A cell
                            // carrying no cluster passes its bare codePoint, which keys
                            // the same slot a one-element sequence would — the colour
                            // sheet has no other master to fall back to, so this is the
                            // only fetch rather than the first of a chain.
                            // A null hook drops the colour: absent leaves the emoji
                            // on the mono path, which draws its coverage in the
                            // foreground colour. Reported by `optionalDefaultsAreLossy`.
                            if (!(ablate & AblateColorFetch)) {
                                const std::uint32_t* const points = clusterCount > 0 ? clusterCodePoints.data() : &cell.codePoint;
                                const int pointCount = clusterCount > 0 ? clusterCount : 1;
                                if (color.cache && atlas.rasteriseColorGlyph) {
                                    // A lone code point probes UNVERIFIED and a cluster
                                    // probes VERIFIED, which is the split the inline walk
                                    // has and not an arbitrary one. One cache holds both
                                    // because the keyspaces are disjoint —
                                    // `colorClusterKey` sets bit 31 for a run of more than
                                    // one and a raw code point never reaches it — so a
                                    // verified probe can never land on a slot stored
                                    // without contents. It if did, the memcmp would
                                    // compare against nothing, report fresh every time,
                                    // and re-rasterise every emoji on every frame.
                                    //
                                    // That argument is about which KEY a probe matches,
                                    // and it says nothing about which SLOT one takes. A
                                    // verified miss allocates like any other, by recency,
                                    // so it can reuse the slot a lone code point was
                                    // handed earlier in the same row — and that earlier
                                    // cell is already holding the UV that named it. Read
                                    // the disjointness above as covering keys only: a
                                    // case where a lone colour glyph goes missing once a
                                    // second one follows it in the same row is open
                                    // against this branch.
                                    bool isFresh = false;
                                    const int slot = pointCount > 1
                                        ? color.cache->slotForKeyVerified(colorClusterKey(points, pointCount), points, pointCount, &isFresh)
                                        : color.cache->slotForKey(colorGlyphKey(points[0]), &isFresh);
                                    if (slot >= 0) {
                                        // A refused raster unclaims its key. Left resident
                                        // the cache would report it cached to every later
                                        // frame, so the raster is never retried and the
                                        // slot is blitted unwritten — or holding whatever
                                        // glyph occupied it before, with every count right.
                                        const bool didRasterise = !isFresh
                                            || atlas.rasteriseColorGlyph(atlas.context, std::span(points, static_cast<std::size_t>(pointCount)), slot);
                                        if (!didRasterise) {
                                            color.cache->forgetKey(pointCount > 1 ? colorClusterKey(points, pointCount) : colorGlyphKey(points[0]));
                                        } else {
                                            const GlyphUV uv = color.atlasRectForSlot(slot);
                                            glyph = { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
                                        }
                                    }
                                } else if (atlas.colorGlyph) {
                                    glyph = atlas.colorGlyph(atlas.context, std::span(points, static_cast<std::size_t>(pointCount)), clusterCount > 0 ? cell.clusterID() : 0);
                                }
                            }
                        }();
                    } else {
                        if (isCellDoubleWide && !isWide && atlas.doubleGlyph
                            && (!hasFraction || (out.doubleGlyphs && in.glyphDoubleSlotWidthInPixels > 0.0f))) {
                            [&, cellVariantStyle, isBold, isItalic] [[gnu::noinline]] {
                                // The backend applies the region and returns the finished
                                // rect, so the V-halving below is the FALLBACK path only.
                                // Halving a master that already carries its half crops it
                                // twice, and the result still draws — a doubled row with
                                // a quarter of the glyph in it.
                                const DoubleRegion region = hasFraction || (sizedSpec && sizedSpec->scale >= 2) ? DoubleRegion::Full
                                    : isDoubleHeightTop                                                         ? DoubleRegion::Top
                                    : isDoubleHeightBottom                                                      ? DoubleRegion::Bottom
                                                                                                                : DoubleRegion::Full;
                                glyph = atlas.doubleGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), cellVariantStyle, region);
                                hasDoubleMaster = glyph.present;
                                isDoubleOnOwnSheet = hasDoubleMaster && out.doubleGlyphs && in.glyphDoubleSlotWidthInPixels > 0.0f;
                            }();
                        }
                        // A cell-baked wide glyph's rect is a wide-page coordinate,
                        // and a wide slot is two cells across, so slot N covers
                        // different texels on each page. A cluster rasterised into
                        // the mono page and handed to a wide cell therefore names a
                        // slot nothing wrote, and a consumer keyed on the cell's width
                        // samples it and draws nothing. Without a wide slot to fill,
                        // the cluster arm below is skipped entirely and the wide base
                        // glyph is drawn with its marks dropped, which keeps the
                        // coordinate on the page the width names.
                        const bool shouldTakeWideCluster = isWide && atlas.wideClusterGlyph != nullptr;
                        if (!glyph.present) {
                            if (clusterCount > 1 && (!isWide || shouldTakeWideCluster)) {
                                [&, cellVariantStyle, clusterCount, isBold, isItalic, isSubpixelGlyph, phaseSnap, weight, widePhase] [[gnu::noinline]] {
                                    const std::uint32_t clusterPhase = isSubpixelGlyph ? static_cast<std::uint32_t>(phaseSnap.phase) : 0u;
                                    const std::uint32_t clusterVariant = cellVariantStyle | clusterPhase;
                                    const FontFace clusterFace = fontFace(isBold, isItalic);
                                    // The hit path, in Core, for the reason the mono one
                                    // is: the key is a Core derivation, the slots are in
                                    // the cache the frame already bound, and the UV comes
                                    // from its geometry. A resident cluster crosses the
                                    // seam zero times; only a miss does, to rasterise.
                                    //
                                    // Verified rather than plain: coverageClusterKey is a
                                    // 23-bit fold of a variable-length run, so the cache
                                    // memcmps the stored run on a hit and reports fresh
                                    // on a collision instead of serving another cluster's
                                    // pixels.
                                    if (shouldTakeWideCluster) {
                                        glyph = atlas.wideClusterGlyph(atlas.context, std::span(clusterCodePoints.data(), static_cast<std::size_t>(clusterCount)), clusterFace, weight, cellVariantStyle | widePhase);
                                    } else if (mono.cache && atlas.rasteriseClusterGlyph) {
                                        bool isFresh = false;
                                        const std::uint64_t clusterKey = glyphSlotKey(
                                            coverageClusterKey(clusterCodePoints.data(), clusterCount, static_cast<int>(clusterPhase), clusterFace), clusterVariant);
                                        const int slot = mono.cache->slotForKeyVerified(clusterKey, clusterCodePoints.data(), clusterCount, &isFresh);
                                        if (slot >= 0) {
                                            const bool didRasterise = !isFresh
                                                || atlas.rasteriseClusterGlyph(atlas.context, std::span(clusterCodePoints.data(), static_cast<std::size_t>(clusterCount)), clusterFace, clusterVariant, slot);
                                            if (!didRasterise) {
                                                mono.cache->forgetKey(clusterKey);
                                            } else {
                                                const GlyphUV uv = monoSlotUV(slot, isSubpixelGlyph);
                                                glyph = { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
                                            }
                                        }
                                    } else if (atlas.clusterGlyph) {
                                        glyph = atlas.clusterGlyph(atlas.context, std::span(clusterCodePoints.data(), static_cast<std::size_t>(clusterCount)), clusterFace, weight, clusterVariant);
                                    } else {
                                        // drops the marks — the base code point alone, an
                                        // `e` where the text said `é`, with the cell inked
                                        // and every count right. The documented
                                        // substitution, reported by
                                        // `optionalDefaultsAreLossy()`. The base is the
                                        // first code point that is not a Prepend.
                                        const std::size_t base = Ubiquitty::Unicode::clusterBaseIndex(
                                            std::span(clusterCodePoints.data(), static_cast<std::size_t>(clusterCount)));
                                        glyph = atlas.monoGlyph(atlas.context, clusterCodePoints[base], clusterFace, weight, clusterVariant);
                                    }
                                }();
                            } else if (isWide) {
                                if (!(ablate & AblateWideFetch)) {
                                    const FontFace wideFace = fontFace(isBold, isItalic);
                                    if (wide.cache && atlas.rasteriseWideGlyph && cell.codePoint != 0 && cell.codePoint != ' ') {
                                        // Keyed by the cell's tier as the mono page is: the
                                        // raster dilates by the foreground's luminance, so
                                        // one slot per glyph would draw dark text at the
                                        // weight of whichever colour faulted it first.
                                        const std::uint64_t wideKey = glyphSlotKey(phasedWideGlyphKey(cell.codePoint, mono.weight, wideFace, widePhase), cellVariantStyle);
                                        bool isFresh = false;
                                        const int slot = wide.cache->slotForKey(wideKey, &isFresh);
                                        if (slot >= 0) {
                                            const bool didRasterise = !isFresh
                                                || atlas.rasteriseWideGlyph(atlas.context, cell.codePoint, wideFace, cellVariantStyle | widePhase, slot);
                                            if (!didRasterise) {
                                                wide.cache->forgetKey(wideKey);
                                            } else {
                                                const GlyphUV uv = wide.atlasRectForSlot(slot);
                                                glyph = { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
                                            }
                                        }
                                    } else if (atlas.wideGlyph) {
                                        glyph = atlas.wideGlyph(atlas.context, cell.codePoint, wideFace);
                                    }
                                }
                            } else if (shapedImage.present) {
                                // Already fetched above, because the answer decides
                                // whether this cell is strip-covered at all.
                                glyph = shapedImage;
                            } else if (mono.cache && !(ablate & AblateMonoFetch) && cell.codePoint != 0 && cell.codePoint != ' ') {
                                // The whole hit path, in Core: derive the key, probe
                                // the cache, divide the slot into a UV. Nothing here
                                // enters the backend unless the probe misses.
                                const FontFace face = fontFace(isBold, isItalic);
                                const std::uint32_t glyphPhase = isSubpixelGlyph ? static_cast<std::uint32_t>(phaseSnap.phase) : 0u;
                                const std::uint32_t glyphVariant = cellVariantStyle | glyphPhase;
                                bool isFresh = false;
                                const std::uint64_t monoKey = glyphSlotKey(monoGlyphKey(cell.codePoint, mono.weight, face, glyphPhase), glyphVariant);
                                const int slot = mono.cache->slotForKey(monoKey, &isFresh);
                                const bool didRasterise = !isFresh
                                    || atlas.rasteriseMonoGlyph(atlas.context, cell.codePoint, face, glyphVariant, slot);
                                if (!didRasterise) {
                                    mono.cache->forgetKey(monoKey);
                                } else if (slot >= 0) {
                                    const GlyphUV uv = monoSlotUV(slot, isSubpixelGlyph);
                                    glyph = { { uv.minU, uv.minV }, { uv.maxU, uv.maxV }, true };
                                    // The backend's measured ink rides the binding
                                    // like slotUV does, and the same slot indexes
                                    // both.
                                    attachMeasuredInk(glyph, mono, slot);
                                }
                            }
                        }
                    }
                }
                // DECDHL: halve the glyph atlas V range so the cell shows the top /
                // bottom half of the master stretched to full cell height. DWL
                // straddle: trim the glyph U range by the visible fraction.
                //
                // A colour glyph is exempt: its quad is a centred square derived
                // from the span box rather than the box itself, so halving the
                // master's V would crop an emoji that is already sized to fit.
                //
                // A fractional glyph maps the whole master into its smaller area
                // and cuts its own rows there, so it keeps the uncropped rect.
                std::optional<AtlasGlyph> wholeMaster;
                if (hasFraction) [[unlikely]]
                    wholeMaster = glyph;
                // The rows the crop below cut for a sized band, which the quad that
                // draws them places by: the crop and the quad read one description.
                SizedBandRows sizedRows;
                if (!isColorOnOwnSheet && (glyph.present || glyph.maxUV.x > glyph.minUV.x)) {
                    // A sized cell owns its vertical scale independently of its
                    // row's DECDHL attribute. Its band crop therefore takes the
                    // complete master, without a second top/bottom halving.
                    if (sizedSpec && sizedSpec->scale >= 2) {
                        // The rect arrives in the convention of the arm that will
                        // draw it, and the band is cut from the slot that arm's quad
                        // covers. A cell-baked quad is the cell box, so its rect has
                        // no margin. A subpixel quad expands its rect by the margins,
                        // so the margin is put back before the cut and the cut keeps
                        // it on the outer bands only: cropping the box and letting the
                        // quad expand every band would stretch each band's slice over
                        // a slot, a magnification of neither the block's scale nor its
                        // neighbours'. The wide and doubled sheets hand over the whole
                        // slot, margins included.
                        const float marginY = in.glyphMarginInPixels.height;
                        const SlotRectConvention convention = isDoubleOnOwnSheet || isWideOnOwnSheet ? SlotRectConvention::PaddedSlot
                            : isSubpixelGlyph                                                        ? SlotRectConvention::ContractedSlot
                                                                                                     : SlotRectConvention::CellBox;
                        const float margin = isDoubleOnOwnSheet         ? 2.0f * marginY
                            : convention == SlotRectConvention::CellBox ? 0.0f
                                                                        : marginY;
                        const float slotMinV = glyph.minUV.y - (convention == SlotRectConvention::ContractedSlot ? marginY : 0.0f);
                        const float slotMaxV = glyph.maxUV.y + (convention == SlotRectConvention::ContractedSlot ? marginY : 0.0f);
                        // Core places whole blocks and erases torn ones, so a band
                        // past its own scale means the grid and the table disagree.
                        // Clamping keeps the sample inside this glyph's slot; the
                        // alternative reads a neighbour's pixels and draws them.
                        const unsigned band = Ubiquitty::penTextBand(&pen) < sizedSpec->scale
                            ? Ubiquitty::penTextBand(&pen)
                            : static_cast<unsigned>(sizedSpec->scale - 1);
                        // The cell-baked quad is the cell box itself, squeezed onto
                        // the row as an unsized glyph is, so its bands keep the box;
                        // the arms that place a quad by the font's own scale cut the
                        // line the grid's rows are made of.
                        const float monoBox = in.glyphSlotHeightInPixels - 2.0f * marginY;
                        const float lineShare = convention == SlotRectConvention::CellBox || monoBox <= 0.0f
                            ? 1.0f
                            : sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, monoBox) / monoBox;
                        sizedRows = sizedBandRows(slotMinV, slotMaxV, margin, lineShare, sizedSpec->scale, band);
                        glyph.minUV.y = sizedRows.minV;
                        glyph.maxUV.y = sizedRows.maxV;
                    } else if (!hasDoubleMaster) {
                        // The V halving is the 1x FALLBACK: a real master already
                        // carries its region, and halving one again crops it twice —
                        // which still draws, as a doubled row holding a quarter of
                        // the glyph.
                        if (isDoubleHeightTop) {
                            const float halfV = (glyph.maxUV.y - glyph.minUV.y) * 0.5f;
                            glyph.maxUV.y = glyph.minUV.y + halfV;
                        } else if (isDoubleHeightBottom) {
                            const float halfV = (glyph.maxUV.y - glyph.minUV.y) * 0.5f;
                            glyph.minUV.y = glyph.minUV.y + halfV;
                        }
                    }
                    // The straddle trim is not a region: it clips a row running past
                    // the right edge, so it applies to a master exactly as it does to
                    // a fallback.
                    glyphUnclippedMaxU = glyph.maxUV.x;
                    if (cellClipFrac < 1.0f)
                        glyph.maxUV.x = glyph.minUV.x + (glyph.maxUV.x - glyph.minUV.x) * cellClipFrac;
                }

                {
                    // A letter on a doubled row, on a backend with no double sheet:
                    // the windows of its doubled master, the master the sharp
                    // doubled path below cuts for a monospace cell, as quads on
                    // the mono stream standing at the letter's pen. The cell path
                    // has no pen to stand a letter at, and a master stretched over
                    // the letter's advance would resample it and paint the quad's
                    // background over the neighbour its ink overlaps; the mono
                    // stream mixes ink over the cell and leaves what has none. The
                    // rasterizer draws such a master at its pen rather than centred
                    // (GlyphRasterizer.h, rasterizeSizedMaster), so the windows step
                    // from the pen by the cell box as every sized window does, the
                    // first and last reaching the margin where the letter's
                    // overhang lives, and enough of them to reach the master's far
                    // edge: a letter wider than the digit cell runs past two. The
                    // master is cut on whole pixels, so the pen is rounded to one.
                    // Every window is probed before any quad is emitted, so a full
                    // sheet leaves the letter on the cell path rather than a part
                    // of it there.
                    bool isDoubledLetter = false;
                    if (isLetterRow && isCellDoubleWide && !isWide && !sizedSpec && !hasFraction && !isDoubleOnOwnSheet
                        && out.glyphs != nullptr && atlas.sizedGlyph != nullptr && cellClipFrac >= 1.0f && clusterCount <= 1
                        && !isSoftFontCell && !isBoxCodePoint && penScript(&pen) == 0
                        && letterCells[static_cast<std::size_t>(column)].kind == LetterCellKind::Letter) [[unlikely]] {
                        isDoubledLetter = [&, cellGridX, cellVariantStyle, isBold, isItalic, mayUseNativeBackground] [[gnu::noinline]] {
                            const float slotWidth = in.glyphSlotWidthInPixels > 0.0f ? in.glyphSlotWidthInPixels : naturalCellWidth;
                            const float slotHeight = in.glyphSlotHeightInPixels > 0.0f ? in.glyphSlotHeightInPixels : naturalCellHeight;
                            const float windowStep = slotWidth - 2.0f * in.glyphMarginInPixels.width;
                            if (windowStep <= 0.0f)
                                return false;
                            constexpr int maxWindows = 4;
                            const int windows = std::min(maxWindows, 2 + static_cast<int>(std::ceil(in.glyphMarginInPixels.width / windowStep)));
                            const bool isDoubleHeight = isDoubleHeightTop || isDoubleHeightBottom;
                            const std::uint8_t scale = isDoubleHeight ? 2 : 1;
                            const std::uint8_t band = isDoubleHeightBottom ? 1 : 0;
                            AtlasGlyph tiles[maxWindows];
                            const auto windowAt = [&](int half, int w) {
                                SizedTile tileRef { scale, static_cast<std::uint8_t>(half), static_cast<std::uint8_t>(w) };
                                tileRef.widthScale = isDoubleHeight ? 1 : 2;
                                return atlas.sizedGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), mono.weight, cellVariantStyle, GlyphPass::Subpixel, tileRef);
                            };
                            // A DECDHL pair is a block of two bands, and is drawn
                            // sharp whole for the reason a sized block is.
                            const SizedWindowRows blockRows = visibleSizedBands(row, band, scale, in.rows, windows);
                            const AtlasGlyph* const blockWindows = sizedBlocks.admit(mono.cache, row - band, column, SizedBlockArm::DoubledLetter, blockRows, band, windowAt);
                            if (blockWindows == nullptr)
                                return false;
                            for (int w = 0; w < windows; ++w) {
                                tiles[w] = blockWindows[w];
                                if (!tiles[w].present)
                                    return false;
                            }
                            const float scaleX = atlasToDeviceScale(metrics);
                            const float scaleY = glyphScaleY;
                            const SubpixelPhase letterPen = phaseForPen(in.originInPixels.x + cellGridX * metrics.cellWidth, 1, in.subpixelPhaseRounding);
                            const float rowTop = in.originInPixels.y + row * metrics.cellHeight;
                            const float boxHeight = slotHeight - 2.0f * in.glyphMarginInPixels.height;
                            const float bandHeight = isDoubleHeight ? sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, boxHeight) : boxHeight;
                            for (int w = 0; w < windows; ++w) {
                                const std::size_t index = counts.glyphs++;
                                if (index >= out.glyphCapacity)
                                    continue;
                                const float reachLeft = w == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachRight = w == windows - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachTop = band == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                const float reachBottom = band + 1 == scale ? in.glyphMarginInPixels.height : 0.0f;
                                GlyphInstance& gi = out.glyphs[index];
                                gi.originInPixels = { static_cast<float>(letterPen.snappedPenX) + (static_cast<float>(w) * windowStep - reachLeft) * scaleX,
                                    rowTop - reachTop * scaleY };
                                gi.sizeInPixels = { (reachLeft + windowStep + reachRight) * scaleX,
                                    (bandHeight + reachTop + reachBottom) * scaleY };
                                gi.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                gi.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                gi.atlasTopLeft = { tiles[w].minUV.x - reachLeft, tiles[w].minUV.y - reachTop };
                                gi.atlasBottomRight = { tiles[w].minUV.x + windowStep + reachRight, sizedBandBottom(tiles[w].minUV.y, tiles[w].maxUV.y, bandHeight, boxHeight) + reachBottom };
                            }
                            return true;
                        }();
                    }
                    const bool isOwnSheetCell = isSubpixelGlyph || isWideOnOwnSheet || isColorOnOwnSheet || isLigatureCovered || isDoubleOnOwnSheet || isDoubledLetter || usesProceduralBox || isProportionalAnchor || isScriptedOwnSheetGlyph || isBrailleLattice || isFractionalGlyph || isFractionalWide;
                    // A narrow cell on a DECDWL or DECDHL row, on the cell-baked path
                    // of a backend with no double sheet, is two windows of its
                    // doubled master, one grid cell each and each mapped 1:1: the
                    // glyph drawn twice as wide, or its half of the glyph drawn
                    // twice as tall, rather than the 1x raster stretched over two
                    // cells. Both windows are probed before either is used, so a
                    // full sheet leaves the cell on the stretched raster rather
                    // than half a glyph.
                    bool isSharpDoubled = false;
                    if (!sizedSpec && isCellDoubleWide && !isWide && !isOwnSheetCell && !hasDoubleMaster && glyph.present && atlas.sizedGlyph != nullptr
                        && cellClipFrac >= 1.0f && clusterCount <= 1 && !isSoftFontCell && !isBoxCodePoint && penScript(&pen) == 0) [[unlikely]] {
                        const bool isDoubleHeight = isDoubleHeightTop || isDoubleHeightBottom;
                        const int scale = isDoubleHeight ? 2 : 1;
                        const int half = isDoubleHeightBottom ? 1 : 0;
                        const auto windowAt = [&](int bandRow, int tile) {
                            SizedTile tileRef { static_cast<std::uint8_t>(scale), static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(tile) };
                            tileRef.widthScale = isDoubleHeight ? 1 : 2;
                            tileRef.step = SizedWindowStep::CellBox;
                            return atlas.sizedGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), mono.weight, cellVariantStyle, GlyphPass::CellBaked, tileRef);
                        };
                        const SizedWindowRows blockRows = visibleSizedBands(row, half, scale, in.rows, 2);
                        const AtlasGlyph* const blockWindows = sizedBlocks.admit(mono.cache, row - half, column, SizedBlockArm::DoubledCell, blockRows, half, windowAt);
                        isSharpDoubled = blockWindows != nullptr;
                        for (int tile = 0; isSharpDoubled && tile < 2; ++tile) {
                            bakedTiles[tile] = blockWindows[tile];
                            isSharpDoubled = bakedTiles[tile].present;
                        }
                    }
                    if (isSharpDoubled) [[unlikely]] {
                        const bool isDoubleHeight = isDoubleHeightTop || isDoubleHeightBottom;
                        for (int tile = 0; tile < 2; ++tile) {
                            const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                            if (index >= out.cellCapacity || !out.cells)
                                continue;
                            CellInstance& inst = out.cells[index];
                            inst.gridPosition = { cellGridX + static_cast<float>(tile), static_cast<float>(row) };
                            inst.widthCells = 1.0f;
                            inst.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                            inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                            inst.atlasTopLeft = bakedTiles[tile].minUV;
                            // Two halves meet once, at the row boundary. A lone
                            // doubled-wide row has no neighbour band to share rows with.
                            inst.atlasBottomRight = { bakedTiles[tile].maxUV.x,
                                isDoubleHeight ? bakedTiles[tile].minUV.y + sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, bakedTiles[tile].maxUV.y - bakedTiles[tile].minUV.y)
                                               : bakedTiles[tile].maxUV.y };
                        }
                    } else if (!sizedSpec) [[likely]] {
                        // An unsized cell is one tile over its own span: the loop
                        // below reduces to this instance when no spec tiles it.
                        const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                        if (index < out.cellCapacity && out.cells) {
                            CellInstance& inst = out.cells[index];
                            inst.gridPosition = { cellGridX, static_cast<float>(row) };
                            inst.widthCells = cellSpanCells;
                            inst.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                            inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                            if (isOwnSheetCell) {
                                inst.atlasTopLeft = { 0.0f, 0.0f };
                                inst.atlasBottomRight = { 0.0f, 0.0f };
                            } else {
                                inst.atlasTopLeft = glyph.minUV;
                                inst.atlasBottomRight = glyph.maxUV;
                            }
                        }
                    } else {
                        [&, cellGridX, cellVariantStyle, cellVisibleWidth, cellXStep, foreground, fullWidthCells, glyphUnclippedMaxU, glyphWidthCells, gridX, isBold, isCellDoubleWide, isItalic, isWide, mayUseNativeBackground] [[gnu::noinline]] {
                            // A sized run is TILED rather than claimed whole, and the reason
                            // is the consumer rather than the geometry: the software
                            // compositor reads which SHEET a rect indexes off `widthCells`
                            // (a span past 1.5 row-scales names the wide page). A cell
                            // claiming more columns than its glyph is wide therefore names
                            // the wide sheet while holding a mono rect, and samples a slot
                            // nothing wrote — the block places, the cursor advances, and no
                            // ink appears. Emitting one instance per glyph-width group keeps
                            // `widthCells` equal to the glyph's own width, so that inference
                            // stays true, and each group takes its own horizontal slice of
                            // the master: the band crop above turned ninety degrees.
                            //
                            // The continuations cannot do this themselves. `writeSizedBlock`
                            // builds them with no code point, so a continuation cannot fetch
                            // the glyph it would slice; the anchor emits for the whole run.
                            //
                            // The span is the narrower of the row's pitch and the cell's. A
                            // band doubled by its anchor row sits on a row the compositor
                            // measures at one cell a column, and a two-cell tile there names
                            // the wide page, so it is tiled a row cell at a time. A band on
                            // a doubled row under a plain anchor is tiled as its anchor is,
                            // a cell a tile: at the row's pitch an odd scale rounds to
                            // tiles that overhang the block, and a sharp band counts half
                            // the windows its anchor draws.
                            float glyphSpanCells = (isWide ? 2.0f : 1.0f) * std::min(xStep, cellXStep);
                            int tileCount = (sizedSpec && glyphSpanCells > 0.0f)
                                ? std::max(1, static_cast<int>(std::lround(fullWidthCells / glyphSpanCells)))
                                : 1;
                            int glyphTileCount = (sizedSpec && glyphSpanCells > 0.0f)
                                ? std::clamp(static_cast<int>(std::lround(glyphWidthCells / glyphSpanCells)), 1, tileCount)
                                : tileCount;
                            // Tiles take their shares of the whole glyph and are cut at the
                            // grid's edge one by one, so the share is of the span before
                            // the straddle trim: sliced from the trimmed span, the part of
                            // the glyph left of the edge would be squeezed into fewer
                            // tiles than it covers.
                            const float uSpan = (tileCount > 1 ? std::max(glyphUnclippedMaxU, glyph.maxUV.x) : glyph.maxUV.x) - glyph.minUV.x;
                            // Sharp sized on the CELL-BAKED path: the same consult the
                            // subpixel arm makes, asking for the glyph-box rect this path
                            // maps onto its cell quads, so a backend whose subpixel pass
                            // is off draws sized text sharp too. A two-cell glyph asks the
                            // wide entry, whose windows step by the two cells its tiles
                            // span, and maps the wide-page rect it answers with 1:1.
                            // Probed whole before the loop for the reason the other arms
                            // probe whole: a full sheet degrades the block to soft, never
                            // to a glyph with holes in it.
                            bool isSharpBaked = false;
                            // A block anchored on a doubled row is twice as wide as it is
                            // tall: its master is the doubled one, and its windows are
                            // one grid cell each, so every window maps its rect 1:1
                            // whatever the row's pitch. Tiles of the row's pitch would
                            // stretch a cell-wide window across two.
                            if (sizedSpec && sizedSpec->scale >= 2 && isCellDoubleWide && !isWide && atlas.sizedGlyph != nullptr && glyph.present
                                && !isOwnSheetCell && penScript(&pen) == 0 && cellClipFrac >= 1.0f) {
                                const int windows = static_cast<int>(std::lround(glyphWidthCells));
                                const auto windowAt = [&](int bandRow, int tile) {
                                    SizedTile tileRef { sizedSpec->scale, static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(tile) };
                                    tileRef.widthScale = 2;
                                    tileRef.step = SizedWindowStep::CellBox;
                                    return atlas.sizedGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), mono.weight, cellVariantStyle, GlyphPass::CellBaked, tileRef);
                                };
                                const int band = static_cast<int>(Ubiquitty::penTextBand(&pen));
                                const SizedWindowRows blockRows = visibleSizedBands(row, band, sizedSpec->scale, in.rows, windows);
                                const AtlasGlyph* const blockWindows = sizedBlocks.admit(mono.cache, row - band, column, SizedBlockArm::CellBakedDoubledRow, blockRows, band, windowAt);
                                bool isEveryWindowPresent = blockWindows != nullptr;
                                for (int tile = 0; isEveryWindowPresent && tile < windows; ++tile) {
                                    bakedTiles[tile] = blockWindows[tile];
                                    isEveryWindowPresent = bakedTiles[tile].present;
                                }
                                if (isEveryWindowPresent) {
                                    isSharpBaked = true;
                                    glyphSpanCells = 1.0f;
                                    tileCount = std::max(windows, static_cast<int>(std::lround(fullWidthCells)));
                                    glyphTileCount = windows;
                                }
                            }
                            if (!isSharpBaked && sizedSpec && sizedSpec->scale >= 2 && (isWide ? atlas.sizedWideGlyph != nullptr : atlas.sizedGlyph != nullptr) && glyph.present
                                && !isOwnSheetCell && !isCellDoubleWide && penScript(&pen) == 0
                                && glyphTileCount > 0 && glyphTileCount <= maxSizedWindowsPerBand) {
                                const auto windowAt = [&](int bandRow, int tile) {
                                    SizedTile tileRef { sizedSpec->scale, static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(tile) };
                                    tileRef.step = SizedWindowStep::CellBox;
                                    return isWide
                                        ? atlas.sizedWideGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), cellVariantStyle, tileRef)
                                        : atlas.sizedGlyph(atlas.context, cell.codePoint,
                                              fontFace(isBold, isItalic), mono.weight, cellVariantStyle, GlyphPass::CellBaked, tileRef);
                                };
                                const int band = static_cast<int>(Ubiquitty::penTextBand(&pen));
                                const SizedWindowRows blockRows = visibleSizedBands(row, band, sizedSpec->scale, in.rows, glyphTileCount);
                                const AtlasGlyph* const blockWindows = sizedBlocks.admit(isWide ? (wide.cache ? wide.cache : mono.cache) : mono.cache, row - band, column, SizedBlockArm::CellBaked, blockRows, band, windowAt);
                                bool isEveryWindowPresent = blockWindows != nullptr;
                                for (int tile = 0; isEveryWindowPresent && tile < glyphTileCount; ++tile) {
                                    bakedTiles[tile] = blockWindows[tile];
                                    isEveryWindowPresent = bakedTiles[tile].present;
                                }
                                isSharpBaked = isEveryWindowPresent;
                            }
                            for (int tile = 0; tile < tileCount; ++tile) {
                                // A block standing across the grid's right edge is cut
                                // there: a tile past the edge is not drawn, and the one
                                // astride it keeps the part of its share left of the edge.
                                const float tileLeft = gridX + static_cast<float>(tile) * glyphSpanCells;
                                float tileWidth = tileCount > 1 ? glyphSpanCells : cellVisibleWidth;
                                float tileShare = 1.0f;
                                if (tileCount > 1) {
                                    const float room = static_cast<float>(in.columns) - tileLeft;
                                    if (room <= 0.0f)
                                        break;
                                    if (room < tileWidth) {
                                        tileShare = room / tileWidth;
                                        tileWidth = room;
                                    }
                                }
                                const std::size_t index = (ablate & AblateCellWrite) ? out.cellCapacity : counts.cells++;
                                if (index >= out.cellCapacity || !out.cells)
                                    continue;
                                CellInstance& inst = out.cells[index];
                                // The tile's place on screen: its share of the claim from
                                // where the cell stands, which on a row laid out as letters
                                // is not the grid column the edge test above counted from.
                                inst.gridPosition = { cellGridX + (tileLeft - gridX), static_cast<float>(row) };
                                inst.widthCells = tileWidth;
                                Vec4 tileBackground = background;
                                if (isSelectionFillPerClaimedCell && !out.boxFills && !isTierPass && row < static_cast<int>(selectionColumn1.size())) {
                                    // The tile's logical column: tiles step one claimed
                                    // cell, at the row's pitch.
                                    const int tileColumn = column + static_cast<int>(std::floor((static_cast<float>(tile) + 0.5f) * glyphSpanCells / cellXStep));
                                    if (isCellSelected(row, tileColumn)) {
                                        const float wash = in.selectionColor.w;
                                        tileBackground.x += (in.selectionColor.x - tileBackground.x) * wash;
                                        tileBackground.y += (in.selectionColor.y - tileBackground.y) * wash;
                                        tileBackground.z += (in.selectionColor.z - tileBackground.z) * wash;
                                        tileBackground.w = resolvedBackgroundOpacity(tileBackground, in);
                                    }
                                }
                                inst.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, tileBackground, in, framePalette8.defaultForeground);
                                inst.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, tileBackground, in, mayUseNativeBackground);
                                if (isOwnSheetCell) {
                                    // Glyph rides a pass of its own below; cell is bg-only.
                                    inst.atlasTopLeft = { 0.0f, 0.0f };
                                    inst.atlasBottomRight = { 0.0f, 0.0f };
                                } else if (tile >= glyphTileCount) {
                                    // Past the glyph: an explicit width's spare columns.
                                    inst.atlasTopLeft = { 0.0f, 0.0f };
                                    inst.atlasBottomRight = { 0.0f, 0.0f };
                                } else if (isSharpBaked) {
                                    // The window's whole rect at 1:1, in place of a slice
                                    // of the magnified crop.
                                    inst.atlasTopLeft = bakedTiles[tile].minUV;
                                    inst.atlasBottomRight = { bakedTiles[tile].minUV.x + (bakedTiles[tile].maxUV.x - bakedTiles[tile].minUV.x) * tileShare,
                                        bakedTiles[tile].minUV.y + sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, bakedTiles[tile].maxUV.y - bakedTiles[tile].minUV.y) };
                                } else if (tileCount > 1) {
                                    const float sliceU = uSpan / static_cast<float>(glyphTileCount);
                                    const float left = glyph.minUV.x + sliceU * static_cast<float>(tile);
                                    inst.atlasTopLeft = { left, glyph.minUV.y };
                                    inst.atlasBottomRight = { left + sliceU * tileShare, glyph.maxUV.y };
                                } else {
                                    inst.atlasTopLeft = glyph.minUV;
                                    inst.atlasBottomRight = glyph.maxUV;
                                }
                            }
                        }();
                    }
                }

                // Box geometry. The cell rect is snapped to whole device pixels
                // FIRST: the decomposition places strokes against those edges, and a
                // neighbour snapping to a different rounding of the same edge is
                // exactly the seam a shared boundary must not have.
                if (usesProceduralBox || isBrailleLattice) [[unlikely]] {
                    [&, cellGridX, cellVisibleWidth, didOverrideForeground, isBrailleLattice, mayUseNativeBackground, usesProceduralBox] [[gnu::noinline]] {
                        if (usesProceduralBox) {
                            // The rect spans what the cell CLAIMS rather than one column.
                            // They are the same number for ordinary text and for every box
                            // character that is not sized, since claimedRunColumns answers 1
                            // there — but a sized anchor claims scale*width, and
                            // writeSizedBlock gives its continuations no code point to draw
                            // with, so a one-column rect leaves the rest of the block empty
                            // rather than merely narrow.
                            float boxLeft = std::round(in.originInPixels.x + cellGridX * metrics.cellWidth);
                            float boxRight = std::round(in.originInPixels.x + (cellGridX + cellVisibleWidth) * metrics.cellWidth);
                            float boxTop = std::round(in.originInPixels.y + row * metrics.cellHeight);
                            float boxBottom = std::round(in.originInPixels.y + (row + 1) * metrics.cellHeight);
                            float referenceHeight = in.glyphReferenceLineHeightInPixels * presentationScale;
                            std::optional<FloatRect> boxClip;
                            if (hasFraction) {
                                boxClip = FloatRect { { boxLeft, boxTop }, { boxRight - boxLeft, boxBottom - boxTop } };
                                const Vec2 areaOrigin = fractionArea.map(fractionBlockOrigin, fractionBlockOrigin);
                                boxLeft = std::round(areaOrigin.x);
                                boxTop = std::round(areaOrigin.y);
                                boxRight = std::round(areaOrigin.x + glyphWidthCells * metrics.cellWidth * fractionArea.scale);
                                boxBottom = std::round(areaOrigin.y + fractionBlockRows * metrics.cellHeight * fractionArea.scale);
                                referenceHeight *= fractionBlockRows * fractionArea.scale;
                            }
                            Rendering::BoxDrawing::PowerlineShape powerline;
                            const Rendering::BoxDrawing::PowerlineShape* powerlinePtr = Rendering::BoxDrawing::analyticCellShape(cell.codePoint, &powerline) ? &powerline : nullptr;
                            const bool isBackgroundFill = (cell.codePoint >= 0x2580 && cell.codePoint <= 0x259F)
                                || (powerlinePtr && (powerline.kind == BoxDrawing::PowerlineShapeKind::SolidTriangle || powerline.kind == BoxDrawing::PowerlineShapeKind::SolidHalfDisc));
                            const bool usesBackgroundColors = isBackgroundFill && mayUseNativeBackground
                                && (in.preservesBackgroundTints || in.usesNativeBackgroundVibrancy || usesNativeTextVibrancy || in.shouldEnhanceTextContrast);
                            Vec4 shapeInk = foreground;
                            if (usesBackgroundColors) {
                                // Blocks and filled separators join background regions. Their
                                // opacity and native contribution must match those regions,
                                // and the shape replaces rather than covers its own cell.
                                if (!didOverrideForeground) {
                                    shapeInk = toVec4(resolveCell(pen, framePalette8, false, in.brightenBold).foreground);
                                }
                                shapeInk.w = resolvedBackgroundOpacity(shapeInk, in);
                                shapeInk = nativeBackgroundInk(usesNativeBackgroundVibrancy, shapeInk, in);
                            } else if (mayUseNativeBackground) {
                                shapeInk = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                            }
                            // The count runs past the capacity — it is what the caller's
                            // regrow is sized from — but the write cursor must not: once
                            // the count is over, the next slot expression would form a
                            // pointer beyond one-past-the-end, which is invalid to even
                            // compute.
                            const std::size_t boxFillWriteIndex = counts.boxFills < out.boxFillCapacity
                                ? counts.boxFills
                                : out.boxFillCapacity;
                            counts.boxFills += static_cast<std::size_t>(emitBoxFill(cell.codePoint, boxLeft, boxTop, boxRight, boxBottom,
                                shapeInk, out.boxFills + boxFillWriteIndex,
                                static_cast<int>(out.boxFillCapacity - boxFillWriteIndex), powerlinePtr, nullptr,
                                referenceHeight, &in.boxStrokeProfile, boxClip, shadeField));
                            if (usesBackgroundColors) {
                                for (std::size_t index = boxFillWriteIndex; index < std::min(counts.boxFills, out.boxFillCapacity); ++index) {
                                    out.boxFills[index].usesBackgroundColors = 1;
                                    out.boxFills[index].foregroundCoverage = foreground.w;
                                    out.boxFills[index].background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in);
                                }
                            }
                        }

                        // The braille lattice. Same stream, same write-cursor rule as
                        // the box fills above — the count may run past the capacity so
                        // the caller can size a regrow from it, the cursor may not — and
                        // the grid rather than the cell rect, which is what keeps the
                        // dot pitch even across a block of them.
                        if (isBrailleLattice) {
                            const std::size_t brailleWriteIndex = counts.boxFills < out.boxFillCapacity
                                ? counts.boxFills
                                : out.boxFillCapacity;
                            counts.boxFills += static_cast<std::size_t>(emitBrailleDots(cell.codePoint, brailleLattice,
                                column, row, mayUseNativeBackground ? nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground) : foreground,
                                out.boxFills + brailleWriteIndex,
                                static_cast<int>(out.boxFillCapacity - brailleWriteIndex)));
                        }
                    }();
                }

                if (isFractionalWide || isFractionalGlyph || (isWideOnOwnSheet && glyph.present)) [[unlikely]] {
                    [&, cellGridX, cellVariantStyle, cellXStep, glyphWidthCells, isBold, isFractionalGlyph, isFractionalWide, isItalic, isWideOnOwnSheet, isWidePhased, mayUseNativeBackground, phaseSnap] [[gnu::noinline]] {
                        const float fractionRowTop = in.originInPixels.y + static_cast<float>(row) * metrics.cellHeight;
                        // Emits one quad's part in this band's row into `stream`. The part
                        // past the grid's right edge is cut, as a straddling tile is.
                        const auto emitRowSlice = [&](GlyphInstance* stream, std::size_t capacity, std::size_t& count, FractionalBandSlice slice) {
                            if (!slice.isVisible)
                                return;
                            const float gridRight = in.originInPixels.x + static_cast<float>(in.columns) * metrics.cellWidth;
                            if (slice.origin.x >= gridRight)
                                return;
                            if (slice.origin.x + slice.size.x > gridRight) {
                                const float share = (gridRight - slice.origin.x) / slice.size.x;
                                slice.atlasBottomRight.x = slice.atlasTopLeft.x + (slice.atlasBottomRight.x - slice.atlasTopLeft.x) * share;
                                slice.size.x = gridRight - slice.origin.x;
                            }
                            const std::size_t index = count++;
                            if (index >= capacity || stream == nullptr)
                                return;
                            GlyphInstance& instance = stream[index];
                            instance.originInPixels = slice.origin;
                            instance.sizeInPixels = slice.size;
                            instance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                            instance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                            instance.atlasTopLeft = slice.atlasTopLeft;
                            instance.atlasBottomRight = slice.atlasBottomRight;
                        };
                        // One band's slice of a full-size glyph quad mapped into the area.
                        const auto emitFractionalSlice = [&](GlyphInstance* stream, std::size_t capacity, std::size_t& count,
                                                             Vec2 glyphOrigin, Vec2 glyphSize, const AtlasGlyph& master) {
                            emitRowSlice(stream, capacity, count,
                                fractionalBandSlice(fractionArea, fractionBlockOrigin, glyphOrigin, glyphSize,
                                    master.minUV, master.maxUV, fractionRowTop, fractionRowTop + metrics.cellHeight));
                        };

                        // Draws the windows of a raster the backend drew at the glyph's
                        // fractional size, standing at the area's origin, each cut at this
                        // band's row. Every window the row needs is probed before any is
                        // emitted, so a full sheet degrades the whole band to the mapped
                        // cell raster rather than to a glyph with holes in it.
                        const auto emitFractionalWindows = [&](SizedTile raster) {
                            const double rasterScale = raster.rasterScale();
                            const int windowRows = static_cast<int>(std::ceil(rasterScale - 1e-6));
                            const int windowColumns = static_cast<int>(std::ceil(rasterScale * raster.widthScale - 1e-6));
                            const bool isSubpixelPass = drawsSubpixelGlyphs;
                            const Vec2 areaOrigin = fractionArea.map(fractionBlockOrigin, fractionBlockOrigin);
                            const SubpixelPhase pen = isSubpixelPass
                                ? phaseForPen(areaOrigin.x, in.subpixelPhaseCount, in.subpixelPhaseRounding)
                                : SubpixelPhase {};
                            const float left = isSubpixelPass ? static_cast<float>(pen.snappedPenX) : std::round(areaOrigin.x);
                            const float top = std::round(areaOrigin.y);
                            const float rowBottom = fractionRowTop + metrics.cellHeight;
                            // A row meets at most two of the raster's rows, so this holds
                            // every window any row needs and no band of a block is
                            // refused where another is drawn.
                            constexpr int maxWindows = 2 * maxSizedWindowsPerBand;
                            struct Window {
                                AtlasGlyph rect;
                                int row;
                                int column;
                            };
                            std::array<Window, maxWindows> windows {};
                            int windowCount = 0;
                            const auto windowAt = [&](int windowRow, int windowColumn) {
                                SizedTile tile = raster;
                                tile.band = static_cast<std::uint8_t>(windowRow);
                                tile.tile = static_cast<std::uint8_t>(windowColumn);
                                tile.step = isSubpixelPass ? SizedWindowStep::FaceAdvance : SizedWindowStep::CellBox;
                                return atlas.sizedGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), mono.weight,
                                    cellVariantStyle | static_cast<std::uint32_t>(pen.phase),
                                    isSubpixelPass ? GlyphPass::Subpixel : GlyphPass::CellBaked, tile);
                            };
                            // The raster's windows do not follow the block's bands, so
                            // the whole raster is the block's to fit.
                            const AtlasGlyph* const rasterWindows = sizedBlocks.admit(mono.cache, fractionBlockTopRow, column, SizedBlockArm::Fractional,
                                { 0, windowRows, windowColumns }, 0, windowAt);
                            if (rasterWindows == nullptr)
                                return false;
                            for (int windowRow = 0; windowRow < windowRows; ++windowRow) {
                                const float windowTop = top + static_cast<float>(windowRow) * metrics.cellHeight;
                                if (windowTop >= rowBottom || windowTop + metrics.cellHeight <= fractionRowTop)
                                    continue;
                                for (int column = 0; column < windowColumns; ++column) {
                                    if (windowCount >= maxWindows)
                                        return false;
                                    const AtlasGlyph& rect = rasterWindows[windowRow * windowColumns + column];
                                    windows[static_cast<std::size_t>(windowCount++)] = { rect, windowRow, column };
                                }
                            }
                            // The subpixel flavour names the window's cell box and leaves
                            // its margins around it, which the outer windows reach into for
                            // the glyph's overhang; the cell-baked flavour is the cell.
                            const float windowStep = sizedWindowStepInPixels(in.glyphReferenceAdvanceInPixels,
                                in.glyphBoxWidthInPixels > 0.0f ? in.glyphBoxWidthInPixels : naturalCellWidth);
                            for (int index = 0; index < windowCount; ++index) {
                                const Window& window = windows[static_cast<std::size_t>(index)];
                                const float windowLeft = isSubpixelPass ? sizedWindowLeftInPixels(window.column, windowStep) : 0.0f;
                                const float atlasWidth = isSubpixelPass ? sizedWindowLeftInPixels(window.column + 1, windowStep) - windowLeft : window.rect.maxUV.x - window.rect.minUV.x;
                                const float atlasHeight = sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, window.rect.maxUV.y - window.rect.minUV.y);
                                if (atlasWidth <= 0.0f || atlasHeight <= 0.0f)
                                    continue;
                                const float pixelsPerTexelX = isSubpixelPass ? atlasToDeviceScale(metrics) : metrics.cellWidth / atlasWidth;
                                const float pixelsPerTexelY = metrics.cellHeight / atlasHeight;
                                const float reachLeft = isSubpixelPass && window.column == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachRight = isSubpixelPass && window.column == windowColumns - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachTop = isSubpixelPass && window.row == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                const float reachBottom = isSubpixelPass && window.row == windowRows - 1 ? in.glyphMarginInPixels.height : 0.0f;
                                const float columnLeft = isSubpixelPass ? windowLeft : static_cast<float>(window.column) * atlasWidth;
                                const Vec2 origin { left + (columnLeft - reachLeft) * pixelsPerTexelX,
                                    top + static_cast<float>(window.row) * metrics.cellHeight - reachTop * pixelsPerTexelY };
                                const Vec2 size { (reachLeft + atlasWidth + reachRight) * pixelsPerTexelX,
                                    (reachTop + atlasHeight + reachBottom) * pixelsPerTexelY };
                                emitRowSlice(out.glyphs, out.glyphCapacity, counts.glyphs,
                                    quadRowSlice(origin, size, { window.rect.minUV.x - reachLeft, window.rect.minUV.y - reachTop },
                                        { window.rect.minUV.x + atlasWidth + reachRight, window.rect.minUV.y + atlasHeight + reachBottom },
                                        fractionRowTop, rowBottom));
                            }
                            return true;
                        };

                        // The two-cell counterpart: the wide raster at the drawn size,
                        // cut into windows two cells wide and one row tall, phase zero
                        // like every sized wide window, standing at the area's origin.
                        // Probed whole before any quad is emitted, for the reason the
                        // narrow windows are.
                        const auto emitFractionalWideWindows = [&](SizedTile raster) {
                            const double rasterScale = raster.rasterScale();
                            const int windowRows = static_cast<int>(std::ceil(rasterScale - 1e-6));
                            const int windowColumns = static_cast<int>(std::ceil(rasterScale - 1e-6));
                            const Vec2 areaOrigin = fractionArea.map(fractionBlockOrigin, fractionBlockOrigin);
                            const float left = std::round(areaOrigin.x);
                            const float top = std::round(areaOrigin.y);
                            const float rowBottom = fractionRowTop + metrics.cellHeight;
                            // A row meets at most two of the raster's rows, so this holds
                            // every window any row needs and no band of a block is
                            // refused where another is drawn.
                            constexpr int maxWindows = 2 * maxSizedWindowsPerBand;
                            struct Window {
                                AtlasGlyph rect;
                                int row;
                                int column;
                            };
                            std::array<Window, maxWindows> windows {};
                            int windowCount = 0;
                            const auto windowAt = [&](int windowRow, int windowColumn) {
                                SizedTile tile = raster;
                                tile.band = static_cast<std::uint8_t>(windowRow);
                                tile.tile = static_cast<std::uint8_t>(windowColumn);
                                return atlas.sizedWideGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), cellVariantStyle, tile);
                            };
                            const AtlasGlyph* const rasterWindows = sizedBlocks.admit(wide.cache ? wide.cache : mono.cache, fractionBlockTopRow, column, SizedBlockArm::FractionalWide,
                                { 0, windowRows, windowColumns }, 0, windowAt);
                            if (rasterWindows == nullptr)
                                return false;
                            for (int windowRow = 0; windowRow < windowRows; ++windowRow) {
                                const float windowTop = top + static_cast<float>(windowRow) * metrics.cellHeight;
                                if (windowTop >= rowBottom || windowTop + metrics.cellHeight <= fractionRowTop)
                                    continue;
                                for (int column = 0; column < windowColumns; ++column) {
                                    if (windowCount >= maxWindows)
                                        return false;
                                    const AtlasGlyph& rect = rasterWindows[windowRow * windowColumns + column];
                                    windows[static_cast<std::size_t>(windowCount++)] = { rect, windowRow, column };
                                }
                            }
                            const float windowStep = 2.0f * sizedWindowStepInPixels(in.glyphReferenceAdvanceInPixels, in.glyphBoxWidthInPixels > 0.0f ? in.glyphBoxWidthInPixels : naturalCellWidth);
                            const float cellsHeight = sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, in.glyphWideSlotHeightInPixels - 2.0f * in.glyphMarginInPixels.height);
                            const float scaleX = atlasToDeviceScale(metrics);
                            const float scaleY = glyphScaleY;
                            for (int index = 0; index < windowCount; ++index) {
                                const Window& window = windows[static_cast<std::size_t>(index)];
                                const float reachLeft = window.column == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachRight = window.column == windowColumns - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachTop = window.row == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                const float reachBottom = window.row == windowRows - 1 ? in.glyphMarginInPixels.height : 0.0f;
                                // The entry returns the full slot; the window's cells
                                // begin a margin in on each axis.
                                const Vec2 cells { window.rect.minUV.x + in.glyphMarginInPixels.width, window.rect.minUV.y + in.glyphMarginInPixels.height };
                                const float windowLeft = sizedWindowLeftInPixels(window.column, windowStep);
                                const float windowWidth = sizedWindowLeftInPixels(window.column + 1, windowStep) - windowLeft;
                                const Vec2 origin { left + (windowLeft - reachLeft) * scaleX,
                                    top + static_cast<float>(window.row) * metrics.cellHeight - reachTop * scaleY };
                                const Vec2 size { (reachLeft + windowWidth + reachRight) * scaleX, (reachTop + cellsHeight + reachBottom) * scaleY };
                                emitRowSlice(out.wideGlyphs, out.wideGlyphCapacity, counts.wideGlyphs,
                                    quadRowSlice(origin, size, { cells.x - reachLeft, cells.y - reachTop },
                                        { cells.x + windowWidth + reachRight, cells.y + cellsHeight + reachBottom }, fractionRowTop, rowBottom));
                            }
                            return true;
                        };

                        if (isFractionalWide) {
                            // The wide raster at the drawn size where the backend draws
                            // one. A product of one is the wide slot itself, which the
                            // mapped quad below draws 1:1; a `w` narrower than the glyph
                            // squeezes it, and a doubled row widens it, neither of which
                            // a window describes.
                            const SizedTile wideRaster = fractionalSizedTile(static_cast<std::uint8_t>(fractionBlockRows),
                                sizedSpec->fractionNumerator, sizedSpec->fractionDenominator);
                            const bool keepsItsWidth = std::fabs(glyphWidthCells - static_cast<float>(sizedSpec->scale) * 2.0f) < 0.001f;
                            const bool didDrawWideWindows = wideRaster.scale != 0 && atlas.sizedWideGlyph && isWideOnOwnSheet
                                && keepsItsWidth && cellXStep == 1.0f && penScript(&pen) == 0 && emitFractionalWideWindows(wideRaster);
                            if (!didDrawWideWindows && wholeMaster->present && isWideOnOwnSheet) {
                                // The wide slot magnified over the whole block is the
                                // picture the stretched bands below cut; the fraction maps
                                // it into its area. The slot carries its margins, so the
                                // quad backs off them at the block's magnification.
                                const float wideScaleX = atlasToDeviceScale(metrics);
                                const float wideScaleY = glyphScaleY;
                                const float slotsClaimed = glyphWidthCells * 0.5f;
                                const Vec2 glyphOrigin { fractionBlockOrigin.x - in.glyphMarginInPixels.width * wideScaleX * slotsClaimed,
                                    fractionBlockOrigin.y - in.glyphMarginInPixels.height * wideScaleY * fractionBlockRows };
                                const Vec2 glyphSize { in.glyphWideSlotWidthInPixels * wideScaleX * slotsClaimed,
                                    in.glyphWideSlotHeightInPixels * wideScaleY * fractionBlockRows };
                                emitFractionalSlice(out.wideGlyphs, out.wideGlyphCapacity, counts.wideGlyphs, glyphOrigin, glyphSize, *wholeMaster);
                            } else if (!didDrawWideWindows && wholeMaster->present) {
                                emitFractionalSlice(out.wideGlyphs, out.wideGlyphCapacity, counts.wideGlyphs, fractionBlockOrigin,
                                    { glyphWidthCells * metrics.cellWidth, fractionBlockRows * metrics.cellHeight }, *wholeMaster);
                            }
                        } else if (isWideOnOwnSheet && glyph.present) {
                            const float wideScaleX = atlasToDeviceScale(metrics);
                            const float wideScaleY = glyphScaleY;
                            // Sharp sized: one ordinary two-cell quad per claimed slot,
                            // windows stepping by two cells, in place of the one quad
                            // stretched `slotsClaimed` times below. Same destination, same
                            // single-margin backoff an unsized wide quad takes — the
                            // stretched path's margin arithmetic exists only because its
                            // quad is magnified, and these are not. Gated and ordered like
                            // the subpixel arm: no super/subscript, and every window
                            // probed before any quad is emitted.
                            bool didDrawSharpWide = false;
                            if (sizedSpec && sizedSpec->scale >= 2 && atlas.sizedWideGlyph && penScript(&pen) == 0) {
                                const int windows = static_cast<int>(std::lround(glyphWidthCells / (2.0f * cellXStep)));
                                AtlasGlyph tiles[maxSizedWindowsPerBand];
                                const auto windowAt = [&](int bandRow, int w) {
                                    const SizedTile tileRef { sizedSpec->scale, static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(w) };
                                    return atlas.sizedWideGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), cellVariantStyle, tileRef);
                                };
                                const int band = static_cast<int>(Ubiquitty::penTextBand(&pen));
                                const SizedWindowRows blockRows = visibleSizedBands(row, band, sizedSpec->scale, in.rows, windows);
                                const AtlasGlyph* const blockWindows = sizedBlocks.admit(wide.cache ? wide.cache : mono.cache, row - band, column, SizedBlockArm::Wide, blockRows, band, windowAt);
                                bool isEveryWindowPresent = blockWindows != nullptr;
                                for (int w = 0; isEveryWindowPresent && w < windows; ++w) {
                                    tiles[w] = blockWindows[w];
                                    isEveryWindowPresent = tiles[w].present;
                                }
                                if (isEveryWindowPresent) {
                                    // Each window takes the two cells it steps by, with
                                    // the margin before the first and after the last, for
                                    // the reason the mono arm's windows do: the slot is
                                    // wider than a window and the raster fills it, so a
                                    // slot-wide quad repaints the neighbouring windows'
                                    // ink and samples the next slot's edge column.
                                    const float windowStep = 2.0f * sizedWindowStepInPixels(in.glyphReferenceAdvanceInPixels, in.glyphBoxWidthInPixels > 0.0f ? in.glyphBoxWidthInPixels : naturalCellWidth);
                                    // Windows partition one raster. Tracking belongs
                                    // between glyphs, never between its windows.
                                    const float sharpScaleX = wideScaleX;
                                    const float wideBoxHeight = in.glyphWideSlotHeightInPixels - 2.0f * in.glyphMarginInPixels.height;
                                    const float wideBandHeight = sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, wideBoxHeight);
                                    for (int w = 0; w < windows; ++w) {
                                        const std::size_t sharpIndex = counts.wideGlyphs++;
                                        if (sharpIndex >= out.wideGlyphCapacity)
                                            continue;
                                        const float reachLeft = w == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                        const float reachRight = w == windows - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                        // Adjacent bands meet once; overlapping padding
                                        // would blend their shared ink twice.
                                        const float reachTop = Ubiquitty::penTextBand(&pen) == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                        const float reachBottom = Ubiquitty::penTextBand(&pen) + 1 == sizedSpec->scale ? in.glyphMarginInPixels.height : 0.0f;
                                        const float windowLeft = sizedWindowLeftInPixels(w, windowStep);
                                        const float windowWidth = sizedWindowLeftInPixels(w + 1, windowStep) - windowLeft;
                                        GlyphInstance& wideInstance = out.wideGlyphs[sharpIndex];
                                        wideInstance.originInPixels = { in.originInPixels.x + cellGridX * metrics.cellWidth
                                                + (windowLeft - reachLeft) * sharpScaleX,
                                            in.originInPixels.y + row * metrics.cellHeight - reachTop * wideScaleY };
                                        wideInstance.sizeInPixels = { (reachLeft + windowWidth + reachRight) * sharpScaleX,
                                            (wideBandHeight + reachTop + reachBottom) * wideScaleY };
                                        wideInstance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                        wideInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                        // The entry returns the full slot; the window's
                                        // cells begin a margin in.
                                        const float cellsLeft = tiles[w].minUV.x + in.glyphMarginInPixels.width;
                                        wideInstance.atlasTopLeft = { cellsLeft - reachLeft, tiles[w].minUV.y + in.glyphMarginInPixels.height - reachTop };
                                        wideInstance.atlasBottomRight = { cellsLeft + windowWidth + reachRight, sizedBandBottom(tiles[w].minUV.y + in.glyphMarginInPixels.height, tiles[w].maxUV.y - in.glyphMarginInPixels.height, wideBandHeight, wideBoxHeight) + reachBottom };
                                    }
                                    didDrawSharpWide = true;
                                }
                            }
                            const std::size_t wideIndex = didDrawSharpWide ? out.wideGlyphCapacity : counts.wideGlyphs++;
                            if (wideIndex < out.wideGlyphCapacity) {
                                GlyphInstance& wideInstance = out.wideGlyphs[wideIndex];
                                // The slot lays out two columns, which is the whole glyph for
                                // ordinary wide text and a third of it for a sized one — a
                                // wide glyph at scale 3 spans 3*2. Stretching by the ratio
                                // magnifies it to the scale, and ordinary wide text spans
                                // exactly the two the slot already holds, so the factor is
                                // 1 there.
                                const float slotsClaimed = glyphWidthCells / (2.0f * cellXStep);
                                // The horizontal backoff carries the same factor. The slot's
                                // left margin is part of what the quad stretches, so a quad
                                // drawn at three times its slot puts its glyph's left edge
                                // three margins out — offsetting by one would start the ink
                                // two margins into the columns it claims. The vertical margin
                                // takes no factor: every band draws into its own row, so the
                                // quad's height is one row whatever the scale.
                                wideInstance.originInPixels = { in.originInPixels.x + cellGridX * metrics.cellWidth - in.glyphMarginInPixels.width * wideScaleX * slotsClaimed,
                                    in.originInPixels.y + row * metrics.cellHeight - in.glyphMarginInPixels.height * wideScaleY };
                                wideInstance.sizeInPixels = { in.glyphWideSlotWidthInPixels * wideScaleX * slotsClaimed,
                                    in.glyphWideSlotHeightInPixels * wideScaleY };
                                if (sizedSpec && sizedSpec->scale >= 2) {
                                    // The cropped band rows, magnified by the block's scale
                                    // into the band's own cell row: the slice the crop cut
                                    // and the quad that shows it are the same rows.
                                    const float bandScale = wideScaleY * static_cast<float>(sizedSpec->scale);
                                    wideInstance.originInPixels.y = in.originInPixels.y + row * metrics.cellHeight - sizedRows.reachTop * bandScale;
                                    wideInstance.sizeInPixels.y = (glyph.maxUV.y - glyph.minUV.y) * bandScale;
                                }
                                if (isWidePhased) {
                                    // The raster is already drawn at the face's advance.
                                    // Extra cell spacing stays empty after the glyph.
                                    const float phasedScaleX = atlasToDeviceScale(metrics);
                                    wideInstance.originInPixels.x = static_cast<float>(phaseSnap.snappedPenX) - in.glyphMarginInPixels.width * phasedScaleX;
                                    wideInstance.sizeInPixels.x = in.glyphWideSlotWidthInPixels * phasedScaleX;
                                }
                                wideInstance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                wideInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                wideInstance.atlasTopLeft = glyph.minUV;
                                wideInstance.atlasBottomRight = glyph.maxUV;
                                const ScriptGlyphBox scripted = scriptGlyphBox(wideInstance.originInPixels, wideInstance.sizeInPixels, penScript(&pen), metrics.cellHeight, glyphSizingFor(in));
                                wideInstance.originInPixels = scripted.origin;
                                wideInstance.sizeInPixels = scripted.size;
                            }
                        }

                        // The fractional glyph: the cell-box raster magnified over the
                        // block's glyph span is the full-size picture, mapped into the
                        // area and cut at this band's row.
                        //
                        // The picture is a raster the backend drew at the glyph's drawn
                        // size — `s * n / d` of it — cut into cell-sized windows, as a
                        // whole-scale block is: the cell's own raster resampled to that
                        // size is a different letter, softer or coarser, than the face
                        // draws there. A product of exactly one is the cell's own raster,
                        // drawn 1:1. Only a backend that declines the windows — a bitmap
                        // face, whose scaled raster would be its own pixels repeated, or a
                        // full sheet — draws from the cell's raster mapped into the area.
                        if (isFractionalGlyph && wholeMaster->present) {
                            SizedTile fractionRaster = fractionalSizedTile(static_cast<std::uint8_t>(fractionBlockRows),
                                sizedSpec->fractionNumerator, sizedSpec->fractionDenominator);
                            fractionRaster.widthScale = static_cast<std::uint8_t>(static_cast<float>(sizedSpec->scale) * cellXStep / fractionBlockRows);
                            if (fractionRaster.scale == 0 && fractionRaster.widthScale == 2)
                                fractionRaster.scale = 1;
                            // The windows hold the glyph at its own width; a `w` narrower
                            // than the text squeezes it, which no window describes.
                            const bool keepsItsWidth = std::fabs(glyphWidthCells - static_cast<float>(sizedSpec->scale) * cellXStep) < 0.001f;
                            bool didDrawWindows = false;
                            if (fractionRaster.scale != 0 && atlas.sizedGlyph && keepsItsWidth && !isSoftFontCell && !isBoxCodePoint) {
                                didDrawWindows = emitFractionalWindows(fractionRaster);
                            }
                            if (!didDrawWindows && isDoubleOnOwnSheet) {
                                const float scaleX = atlasToDeviceScale(metrics);
                                const float scaleY = glyphScaleY;
                                const float slotsClaimed = glyphWidthCells * 0.5f;
                                const Vec2 glyphOrigin { fractionBlockOrigin.x - 2.0f * in.glyphMarginInPixels.width * scaleX * slotsClaimed,
                                    fractionBlockOrigin.y - in.glyphMarginInPixels.height * scaleY * fractionBlockRows };
                                const Vec2 glyphSize { in.glyphDoubleSlotWidthInPixels * scaleX * slotsClaimed,
                                    in.glyphDoubleSlotHeightInPixels * scaleY * fractionBlockRows * 0.5f };
                                emitFractionalSlice(out.doubleGlyphs, out.doubleGlyphCapacity, counts.doubleGlyphs, glyphOrigin, glyphSize, *wholeMaster);
                            } else if (!didDrawWindows) {
                                const float glyphAdvance = drawsSubpixelGlyphs && !isSoftFontCell && !isBoxCodePoint && in.glyphReferenceAdvanceInPixels > 0.0f
                                    ? in.glyphReferenceAdvanceInPixels * atlasToDeviceScale(metrics)
                                    : metrics.cellWidth;
                                emitFractionalSlice(out.glyphs, out.glyphCapacity, counts.glyphs, fractionBlockOrigin,
                                    { glyphWidthCells * glyphAdvance, fractionBlockRows * metrics.cellHeight }, *wholeMaster);
                            }
                        }
                    }();
                }

                // Doubled glyph (DECDWL/DECDHL). The quad spans two cells and half a
                // master slot, with its vertical midline on the DHL pair's shared row
                // boundary: Top backs off by the doubled top margin, Bottom starts at
                // its own row top, and the two halves therefore meet across that
                // boundary. DWL condenses the whole master into one row, centred on
                // it by half a margin.
                //
                // The doubled margins are not a scale factor applied twice — the 2x
                // master is rasterised into a slot whose margins are themselves 2x,
                // so backing off by one margin would leave the glyph a margin low.
                // A sized block on a doubled row, drawn sharp: each visual cell of
                // the claim takes a window of the glyph rasterised at the block's
                // scale and twice that across, the way an undoubled block takes
                // windows at its scale. The double master is the 2x glyph, and
                // cropping it into bands and stretching the crops over the claim
                // draws the block soft and wider than it is tall by resampling.
                // Every window is probed before any is emitted, so a full sheet
                // leaves the whole cell on the double master rather than a column
                // of the glyph blank.
                if (isDoubleOnOwnSheet && !isFractionalGlyph) [[unlikely]] {
                    [&, cellClipFrac, cellGridX, cellVariantStyle, cellVisibleWidth, glyphWidthCells, isBold, isBoxCodePoint, isDoubleOnOwnSheet, isItalic, isWide, mayUseNativeBackground] [[gnu::noinline]] {
                        bool didDrawSharpDouble = false;
                        if (isDoubleOnOwnSheet && sizedSpec && sizedSpec->scale >= 2 && !isWide && atlas.sizedGlyph
                            && drawsSubpixelGlyphs && out.glyphs && penScript(&pen) == 0) {
                            const int windows = static_cast<int>(std::lround(glyphWidthCells));
                            const SubpixelPhase blockPen = phaseForPen(in.originInPixels.x + cellGridX * metrics.cellWidth,
                                in.subpixelPhaseCount, in.subpixelPhaseRounding);
                            AtlasGlyph tiles[maxSizedWindowsPerBand];
                            const auto windowAt = [&](int bandRow, int w) {
                                SizedTile tileRef { sizedSpec->scale, static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(w) };
                                tileRef.widthScale = 2;
                                return atlas.sizedGlyph(atlas.context, cell.codePoint, fontFace(isBold, isItalic), mono.weight,
                                    cellVariantStyle | static_cast<std::uint32_t>(blockPen.phase), GlyphPass::Subpixel, tileRef);
                            };
                            const int band = static_cast<int>(Ubiquitty::penTextBand(&pen));
                            const SizedWindowRows blockRows = visibleSizedBands(row, band, sizedSpec->scale, in.rows, windows);
                            const AtlasGlyph* const blockWindows = cellClipFrac >= 1.0f
                                ? sizedBlocks.admit(mono.cache, row - band, column, SizedBlockArm::SubpixelDoubledRow, blockRows, band, windowAt)
                                : nullptr;
                            bool isEveryWindowPresent = blockWindows != nullptr;
                            for (int w = 0; isEveryWindowPresent && w < windows; ++w) {
                                tiles[w] = blockWindows[w];
                                isEveryWindowPresent = tiles[w].present;
                            }
                            if (isEveryWindowPresent) {
                                const float scaleX = atlasToDeviceScale(metrics);
                                const float scaleY = glyphScaleY;
                                const float slotHeight = in.glyphSlotHeightInPixels > 0.0f ? in.glyphSlotHeightInPixels : naturalCellHeight;
                                const float windowStep = sizedWindowStepInPixels(in.glyphReferenceAdvanceInPixels,
                                    in.glyphBoxWidthInPixels > 0.0f ? in.glyphBoxWidthInPixels : naturalCellWidth);
                                const unsigned band = Ubiquitty::penTextBand(&pen);
                                const float boxHeight = slotHeight - 2.0f * in.glyphMarginInPixels.height;
                                const float bandHeight = sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, boxHeight);
                                for (int w = 0; w < windows; ++w) {
                                    const std::size_t sharpIndex = counts.glyphs++;
                                    if (sharpIndex >= out.glyphCapacity)
                                        continue;
                                    const float reachLeft = w == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                    const float reachRight = w == windows - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                    const float reachTop = band == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                    const float reachBottom = band + 1 == sizedSpec->scale ? in.glyphMarginInPixels.height : 0.0f;
                                    const float windowLeft = sizedWindowLeftInPixels(w, windowStep);
                                    const float windowWidth = sizedWindowLeftInPixels(w + 1, windowStep) - windowLeft;
                                    GlyphInstance& gi = out.glyphs[sharpIndex];
                                    gi.originInPixels = { static_cast<float>(blockPen.snappedPenX)
                                            + (windowLeft - reachLeft) * scaleX,
                                        in.originInPixels.y + row * metrics.cellHeight - reachTop * scaleY };
                                    gi.sizeInPixels = { (reachLeft + windowWidth + reachRight) * scaleX,
                                        (bandHeight + reachTop + reachBottom) * scaleY };
                                    gi.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                    gi.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                    gi.atlasTopLeft = { tiles[w].minUV.x - reachLeft, tiles[w].minUV.y - reachTop };
                                    gi.atlasBottomRight = { tiles[w].minUV.x + windowWidth + reachRight, sizedBandBottom(tiles[w].minUV.y, tiles[w].maxUV.y, bandHeight, boxHeight) + reachBottom };
                                }
                                didDrawSharpDouble = true;
                            }
                        }
                        if (isDoubleOnOwnSheet && !didDrawSharpDouble) {
                            const float scaleX = atlasToDeviceScale(metrics);
                            const float scaleY = glyphScaleY;
                            const float cellLeft = in.originInPixels.x + cellGridX * metrics.cellWidth;
                            const float rowTop = in.originInPixels.y + row * metrics.cellHeight;
                            const std::size_t doubleIndex = counts.doubleGlyphs++;
                            if (doubleIndex < out.doubleGlyphCapacity) {
                                GlyphInstance& doubleInstance = out.doubleGlyphs[doubleIndex];
                                // The slot spans two cells, which is one doubled column and
                                // the whole claim for ordinary doubled text. A sized cell on
                                // a doubled row claims more of them, and the horizontal
                                // backoff carries the same factor as the width — a quad
                                // stretched to three slots whose origin backs off one slot's
                                // margins starts its ink inside the columns it claims.
                                const float doubleSlotsClaimed = glyphWidthCells / 2.0f;
                                doubleInstance.originInPixels = { cellLeft - 2.0f * in.glyphMarginInPixels.width * scaleX * doubleSlotsClaimed,
                                    isDoubleHeightTop          ? rowTop - 2.0f * in.glyphMarginInPixels.height * scaleY
                                        : isDoubleHeightBottom ? rowTop
                                                               : rowTop - in.glyphMarginInPixels.height * scaleY };
                                doubleInstance.sizeInPixels = { in.glyphDoubleSlotWidthInPixels * scaleX * doubleSlotsClaimed,
                                    in.glyphDoubleSlotHeightInPixels * scaleY * 0.5f };
                                // The rows the region shows, in atlas pixels. A DECDHL
                                // half is cut at the master's midline, which is the cell
                                // box's rounding; the pair's two rows are the font's line
                                // each, so the top half ends at the line and the bottom
                                // one starts there. The bottom half keeps the master's
                                // rows below its line for the overhang they carry.
                                float shownTopV = glyph.minUV.y;
                                float shownBottomV = glyph.maxUV.y;
                                if (sizedSpec && sizedSpec->scale >= 2) {
                                    const float bandScale = scaleY * static_cast<float>(sizedSpec->scale) * 0.5f;
                                    doubleInstance.originInPixels.y = rowTop - sizedRows.reachTop * bandScale;
                                    doubleInstance.sizeInPixels.y = (glyph.maxUV.y - glyph.minUV.y) * bandScale;
                                } else if (isDoubleHeightTop || isDoubleHeightBottom) {
                                    const float halfHeight = 0.5f * in.glyphDoubleSlotHeightInPixels;
                                    const float boxHeight = halfHeight - 2.0f * in.glyphMarginInPixels.height;
                                    const float spareRows = boxHeight - sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, boxHeight);
                                    // The top half loses the rows below the line; the bottom
                                    // one gains the rows of the line it takes from the top.
                                    const float shownRows = isDoubleHeightTop ? halfHeight - spareRows : halfHeight + spareRows;
                                    if (isDoubleHeightTop)
                                        shownBottomV -= spareRows;
                                    else
                                        shownTopV -= spareRows;
                                    doubleInstance.sizeInPixels.y = shownRows * scaleY;
                                }
                                // The straddle trim already cut the U range above, which is
                                // the half that keeps the texel-to-pixel mapping; the quad
                                // has to lose the same fraction or the visible part stretches
                                // to fill a width the grid no longer has.
                                if (cellClipFrac < 1.0f)
                                    doubleInstance.sizeInPixels.x *= cellClipFrac;
                                doubleInstance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                doubleInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                doubleInstance.atlasTopLeft = { glyph.minUV.x, shownTopV };
                                doubleInstance.atlasBottomRight = { glyph.maxUV.x, shownBottomV };
                                if (isBoxCodePoint) {
                                    // Edge-stretched strokes stop at the cell boundary;
                                    // atlas padding must not extend them into another row.
                                    const float texelsPerPixelX = (glyph.maxUV.x - glyph.minUV.x) / doubleInstance.sizeInPixels.x;
                                    const float texelsPerPixelY = (glyph.maxUV.y - glyph.minUV.y) / doubleInstance.sizeInPixels.y;
                                    doubleInstance.atlasTopLeft.x += (cellLeft - doubleInstance.originInPixels.x) * texelsPerPixelX;
                                    doubleInstance.atlasTopLeft.y += (rowTop - doubleInstance.originInPixels.y) * texelsPerPixelY;
                                    doubleInstance.originInPixels = { cellLeft, rowTop };
                                    doubleInstance.sizeInPixels = { cellVisibleWidth * metrics.cellWidth, metrics.cellHeight };
                                    doubleInstance.atlasBottomRight = {
                                        doubleInstance.atlasTopLeft.x + doubleInstance.sizeInPixels.x * texelsPerPixelX,
                                        doubleInstance.atlasTopLeft.y + doubleInstance.sizeInPixels.y * texelsPerPixelY
                                    };
                                }
                            }
                        }
                    }();
                }

                // Colour glyph. The atlas slot is SQUARE, so the destination quad
                // must be square too: drawing into the full span box compresses the
                // emoji horizontally for a one-cell span, where cellHeight exceeds
                // spanWidth, and stretches it for a two-cell one. Fit the largest
                // square that sits inside the span box and centre it on both axes,
                // so the master keeps the 1:1 aspect it was rasterised at.
                //
                // No script geometry and no glyph scale: the square is derived from
                // the cell box the frame resolved, so it already tracks DECCOLM and
                // resize without a reference advance to divide by.
                // A sized block covers `scale` rows and every band's anchor carries
                // the same code point, so an emission that ignores the band draws the
                // whole emoji once per row — the failure that makes a replica which cannot
                // band-crop flatten its sized cells (Sync/HistoryCache.cpp).
                //
                // The crop is the one the mono streams take at :1141, which the colour
                // path is exempt from there because its quad is a centred square
                // rather than the cell box: cropping the master's V against a box the
                // quad does not fill would slice the wrong region. So the square is
                // built first, over the whole block, and each band then takes the part
                // of it that falls in that band's own row.
                //
                // Per band rather than one tall quad, because an instance draws into
                // one row and nothing here may span them — the same reason the wide
                // and double arms take no vertical factor. A quad three rows tall
                // would also be the only one in the builder, so every consumer would
                // have to be shown to accept it.
                //
                // Sized raster tiles preserve source resolution. A backend without
                // sized colour support scales the ordinary master over the block.
                if (isColorOnOwnSheet && glyph.present) [[unlikely]] {
                    [&, cellGridX, cellVisibleWidth, fullWidthCells, hasFraction, isColorOnOwnSheet, mayUseNativeBackground] [[gnu::noinline]] {
                        if (isColorOnOwnSheet && glyph.present) {
                            const unsigned colourScale = (sizedSpec && sizedSpec->scale >= 1) ? sizedSpec->scale : 1u;
                            // Clamped for the reason the mono crop clamps: a band past its own
                            // scale means the grid and the table disagree, and the alternative
                            // samples outside this glyph's slot.
                            const unsigned colourBand = Ubiquitty::penTextBand(&pen) < colourScale
                                ? Ubiquitty::penTextBand(&pen)
                                : colourScale - 1;
                            const float blockRows = static_cast<float>(colourScale);
                            const float spanWidth = cellVisibleWidth * metrics.cellWidth;
                            const float spanHeight = blockRows * metrics.cellHeight;
                            // Whole pixels before the multiply, so the scaled square is whole
                            // too: an atlas slot is a whole number of texels, and a fractional
                            // side resamples the master by a few percent for nothing.
                            const float square = colourGlyphSquareInPixels(metrics.cellWidth, metrics.cellHeight) * blockRows;
                            const float fullSide = spanWidth < square ? spanWidth : square;
                            const float spanLeft = in.originInPixels.x + cellGridX * metrics.cellWidth;
                            const float rowTop = in.originInPixels.y + row * metrics.cellHeight;
                            const float blockTop = rowTop - static_cast<float>(colourBand) * metrics.cellHeight;
                            // A fraction maps the full-size square into its area: smaller,
                            // and moved to where `v` and `h` put the area.
                            const FractionalRenderArea colourArea = hasFraction
                                ? fractionalRenderArea(*sizedSpec, { fullWidthCells * metrics.cellWidth, spanHeight })
                                : FractionalRenderArea {};
                            const float side = fullSide * colourArea.scale;
                            // Every edge is snapped before it is intersected, not after. The
                            // sheet has one texel per destination pixel; and two neighbouring bands must round
                            // their shared row boundary to the same number, or the block draws
                            // with a seam or an overlap along it.
                            const float squareTop = std::round(blockTop + colourArea.offset.y + colourArea.scale * (spanHeight - fullSide) * 0.5f);
                            const float squareBottom = squareTop + side;
                            const float bandTop = std::round(rowTop);
                            const float bandBottom = std::round(rowTop + metrics.cellHeight);
                            const float visibleTop = squareTop > bandTop ? squareTop : bandTop;
                            const float visibleBottom = squareBottom < bandBottom ? squareBottom : bandBottom;
                            // A band the centred square does not reach draws nothing. It can
                            // only happen where the block is wider than it is tall, so the
                            // square is bounded by the width and leaves the outer rows empty.
                            if (visibleBottom > visibleTop) {
                                const float squareLeft = std::round(spanLeft + colourArea.offset.x + colourArea.scale * (spanWidth - fullSide) * 0.5f);
                                // Sharp sized: the backend rasterised the cluster at the
                                // block's size and holds it as square-sized windows, so each
                                // window draws one ordinary colour quad at 1:1 instead of
                                // this band's slice of the magnified 1x master. The window
                                // grid is the SOURCE square's, so a window is clipped
                                // against this band's row exactly as the soft square is —
                                // and every window this band needs is probed before any quad
                                // is emitted, so a full sheet degrades the whole band to
                                // soft rather than to a square with holes in it.
                                bool didDrawSharp = false;
                                // The raster the windows cut: the block's scale, or under a
                                // fraction the size the square is drawn at. A fraction whose
                                // product is one is the master itself, which the soft quad
                                // below draws texel for pixel.
                                SizedTile colourRaster;
                                colourRaster.scale = static_cast<std::uint8_t>(colourScale);
                                if (hasFraction)
                                    colourRaster = fractionalSizedTile(static_cast<std::uint8_t>(colourScale), sizedSpec->fractionNumerator, sizedSpec->fractionDenominator);
                                const bool hasSizedRaster = colourRaster.scale >= 2 || colourRaster.isFractional();
                                if (hasSizedRaster && atlas.sizedColorGlyph) {
                                    const std::uint32_t* const points = clusterCount > 0 ? clusterCodePoints.data() : &cell.codePoint;
                                    const int pointCount = clusterCount > 0 ? clusterCount : 1;
                                    // One window is one source square; a raster drawn at a
                                    // fraction of a whole number of them ends in a partial
                                    // row and column.
                                    const double rasterScale = colourRaster.rasterScale();
                                    const float windowSide = side / static_cast<float>(rasterScale);
                                    const int windowsPerSide = static_cast<int>(std::ceil(rasterScale - 1e-6));
                                    struct SharpWindow {
                                        AtlasGlyph rect;
                                        float left, width, top, bottom, windowTop;
                                    };
                                    // Per band: every column of the grid, over at most the
                                    // two source rows a cell row can straddle when the
                                    // square and the cell height differ.
                                    SharpWindow windows[16];
                                    int windowCount = 0;
                                    // The source square's windows do not follow the
                                    // block's bands, so the whole square is the block's
                                    // to fit.
                                    const auto windowAt = [&](int windowRow, int windowColumn) {
                                        SizedTile tileRef = colourRaster;
                                        tileRef.band = static_cast<std::uint8_t>(windowRow);
                                        tileRef.tile = static_cast<std::uint8_t>(windowColumn);
                                        return atlas.sizedColorGlyph(atlas.context, std::span(points, static_cast<std::size_t>(pointCount)), tileRef);
                                    };
                                    const AtlasGlyph* const squareWindows = windowSide > 0.0f
                                        ? sizedBlocks.admit(color.cache, row - static_cast<int>(colourBand), column, SizedBlockArm::Colour,
                                              { 0, windowsPerSide, windowsPerSide }, 0, windowAt)
                                        : nullptr;
                                    bool isEveryWindowPresent = squareWindows != nullptr;
                                    for (int r = 0; isEveryWindowPresent && r < windowsPerSide; ++r) {
                                        const float windowTop = squareTop + windowSide * static_cast<float>(r);
                                        const float windowBottom = std::min(windowTop + windowSide, squareBottom);
                                        const float clipTop = windowTop > visibleTop ? windowTop : visibleTop;
                                        const float clipBottom = windowBottom < visibleBottom ? windowBottom : visibleBottom;
                                        if (clipBottom <= clipTop)
                                            continue;
                                        for (int t = 0; t < windowsPerSide; ++t) {
                                            if (windowCount >= 16) {
                                                isEveryWindowPresent = false;
                                                break;
                                            }
                                            const AtlasGlyph& rect = squareWindows[r * windowsPerSide + t];
                                            const float windowLeft = squareLeft + windowSide * static_cast<float>(t);
                                            const float windowWidth = std::min(windowSide, squareLeft + side - windowLeft);
                                            windows[windowCount++] = SharpWindow { rect, windowLeft, windowWidth, clipTop, clipBottom, windowTop };
                                        }
                                    }
                                    if (isEveryWindowPresent && windowCount > 0) {
                                        for (int w = 0; w < windowCount; ++w) {
                                            const std::size_t colorIndex = counts.colorGlyphs++;
                                            if (colorIndex >= out.colorGlyphCapacity)
                                                continue;
                                            GlyphInstance& colorInstance = out.colorGlyphs[colorIndex];
                                            const SharpWindow& win = windows[w];
                                            colorInstance.originInPixels = { win.left, win.top };
                                            colorInstance.sizeInPixels = { win.width, win.bottom - win.top };
                                            colorInstance.foreground = foreground;
                                            colorInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                            // A partial window samples only the part of its slot the
                                            // raster reaches, at the slot's own texel pitch.
                                            const float uSpan = win.rect.maxUV.x - win.rect.minUV.x;
                                            const float vSpan = win.rect.maxUV.y - win.rect.minUV.y;
                                            colorInstance.atlasTopLeft = { win.rect.minUV.x,
                                                win.rect.minUV.y + vSpan * (win.top - win.windowTop) / windowSide };
                                            colorInstance.atlasBottomRight = { win.rect.minUV.x + uSpan * win.width / windowSide,
                                                win.rect.minUV.y + vSpan * (win.bottom - win.windowTop) / windowSide };
                                        }
                                        didDrawSharp = true;
                                    }
                                }
                                if (!didDrawSharp) {
                                    const std::size_t colorIndex = counts.colorGlyphs++;
                                    if (colorIndex < out.colorGlyphCapacity) {
                                        GlyphInstance& colorInstance = out.colorGlyphs[colorIndex];
                                        colorInstance.originInPixels = { squareLeft, visibleTop };
                                        colorInstance.sizeInPixels = { side, visibleBottom - visibleTop };
                                        // A colour master carries its own pixels, so the fragment
                                        // stage samples those and reads neither of these. They hold
                                        // the cell's resolved colours so the two never disagree.
                                        colorInstance.foreground = foreground;
                                        colorInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                        // This band's slice of the master, measured against the
                                        // square rather than the row: the rows tile the square only
                                        // when it is the block's height that bounds it.
                                        const float vSpan = glyph.maxUV.y - glyph.minUV.y;
                                        colorInstance.atlasTopLeft = { glyph.minUV.x,
                                            glyph.minUV.y + vSpan * (visibleTop - squareTop) / side };
                                        colorInstance.atlasBottomRight = { glyph.maxUV.x,
                                            glyph.minUV.y + vSpan * (visibleBottom - squareTop) / side };
                                    }
                                }
                            }
                        }
                    }();
                }

                // Ligature slice. The run was rasterised as one strip at the run's
                // pen; each covered cell draws its share of that strip, so a colour
                // change mid-run recolours without reshaping and without moving a
                // glyph. The share is the cell's own cluster's ink where the strip's
                // producer measured it — `ligatureOwnedInkSlice`, which keeps a
                // healed alternate's overhanging tips under their own cell's colours
                // — and the geometric band over the column where it did not.
                //
                // On the geometric cut, the run's END cells take the strip's edges
                // rather than their own segment bounds — `ligatureStripCellSlice`
                // decides that — because a shaped run overhangs its advance box and
                // clipping to the box would shave the outer strokes.
                if (isSizedTextRunAnchor && !(ablate & AblateGlyphFetch)) [[unlikely]] {
                    emitSizedTextRun(in, metrics, atlas, cell, *sizedSpec,
                        in.originInPixels.x + cellGridX * metrics.cellWidth,
                        in.originInPixels.y + row * metrics.cellHeight, fullWidthCells * metrics.cellWidth,
                        fontFace(isBold, isItalic), weight, cellVariantStyle,
                        nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground),
                        nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground), out, counts);
                }
                if (isProportionalAnchor || isLigatureCovered) [[unlikely]] {
                    [&, cellGridX, cellVariantStyle, gridX, isBold, isItalic, isLigatureCovered, isProportionalAnchor, mayUseNativeBackground, weight] [[gnu::noinline]] {
                        if (isProportionalAnchor) {
                            // The run's text, which its anchor names, shaped by the backend
                            // into one strip over the run's cells and drawn as one quad at the
                            // run's pen: the strip rect spans margin, cells and margin, so
                            // the quad starts a margin before the pen the way a subpixel
                            // glyph's does, and the continuations — skipped by the loop as
                            // every continuation is — contribute no quad of their own.
                            const int widthCells = sizedSpec->runWidthCells > 0 ? sizedSpec->runWidthCells : 1;
                            // A run at an OSC 66 scale is a block: laid out at its own
                            // width, then every position and every quad drawn `scale`
                            // times the size, as one picture standing on the block's top
                            // row, of which each band row draws the part inside its own
                            // row. It takes its anchor row's doubling across, as a sized
                            // block's bands do, and its own height whatever its row's
                            // DECDHL attribute says.
                            const int runScale = std::max<int>(sizedSpec->scale, 1);
                            const bool isScaledRun = runScale > 1;
                            // A run on a DECDWL row doubles with the row: laid out at its
                            // own width, then every position and every quad drawn twice as
                            // wide from its anchor's doubled column.
                            const int runWidthScale = (isScaledRun ? isCellDoubleWide : isDoubleWide) ? 2 : 1;
                            const float runXScale = static_cast<float>(runWidthScale * runScale);
                            const float runPixelScale = runXScale * presentationScale;
                            // A run on a DECDHL pair doubles both ways, as the pair's other
                            // glyphs do: the whole run is one picture twice the size,
                            // standing on the pair's top row, and each row draws the part
                            // of it on its own side of the midline the two rows share — so
                            // the halves meet where ordinary doubled glyphs' halves meet.
                            // A scaled run is the same picture over `scale` rows.
                            const bool isDoubleHeightRun = !isScaledRun && (isDoubleHeightTop || isDoubleHeightBottom);
                            const int runRows = std::max(runScale, isDoubleHeightRun ? 2 : 1);
                            const bool isBandedRun = runRows > 1;
                            const int runBand = isScaledRun ? std::min<int>(Ubiquitty::penTextBand(&pen), runScale - 1)
                                : isDoubleHeightBottom      ? 1
                                                            : 0;
                            const float rowTopInPixels = in.originInPixels.y + row * metrics.cellHeight;
                            const float blockTopInPixels = std::round(rowTopInPixels - static_cast<float>(runBand) * metrics.cellHeight);
                            const float bandTopInPixels = runBand > 0 ? std::round(rowTopInPixels) : -std::numeric_limits<float>::infinity();
                            const float bandBottomInPixels = runBand + 1 < runRows ? std::round(rowTopInPixels + metrics.cellHeight) : std::numeric_limits<float>::infinity();
                            // Scratch kept across runs and grown once to the most a run
                            // holds: more than a stack frame should carry, and the walk is
                            // one thread.
                            thread_local std::vector<std::uint32_t> runCodePoints;
                            thread_local std::vector<ProportionalShapedGlyph> shaped;
                            thread_local std::vector<ProportionalGlyphPlacement> placements;
                            thread_local std::vector<SkipInkTap> runSkipInkTaps;
                            if (runCodePoints.size() < static_cast<std::size_t>(runTextMaximumCodePoints))
                                runCodePoints.resize(static_cast<std::size_t>(runTextMaximumCodePoints));
                            const int claimedWidthCells = std::min(widthCells, static_cast<int>((static_cast<float>(in.columns) - gridX) / runXScale));
                            if (metrics.skipInk)
                                runSkipInkTaps.assign(static_cast<std::size_t>(claimedWidthCells), SkipInkTap {});
                            int runCount = in.clusterResolver ? in.clusterResolver(&cell, std::span(runCodePoints), in.clusterResolverContext) : 0;
                            if (runCount <= 0) {
                                runCodePoints[0] = cell.codePoint;
                                runCount = 1;
                            }
                            // The true pen, split into the whole pixel the quad is placed
                            // from and the residual the backend draws at: a strip drawn
                            // from a phase bucket puts every later glyph a step from where
                            // the same text sits at another column.
                            const float runStartX = in.originInPixels.x + cellGridX * metrics.cellWidth;
                            // The claim's right edge on screen: where the next cell, and
                            // so a neighbouring run, begins, or the grid's own edge where
                            // a doubled claim runs past it.
                            const float claimCells = static_cast<float>(widthCells) * runXScale;
                            const float visibleClaimCells = std::min(claimCells, static_cast<float>(in.columns) - gridX);
                            const float claimRightX = runStartX + visibleClaimCells * metrics.cellWidth;
                            const float penWholeX = std::floor(runStartX);
                            const float penResidual = drawsSubpixelGlyphs ? runStartX - penWholeX : 0.0f;
                            // The per-glyph route: the backend shapes, Core places, and
                            // each glyph draws as its own coverage quad at the phase of its
                            // own origin. A run wider than any strip draws whole, and the
                            // sheet holds a repertoire rather than every text on screen.
                            bool didDrawGlyphs = false;
                            const std::size_t firstGlyphInkOfRun = proportionalGlyphInks.size();
                            if (hasProportionalGlyphRouteInWalk) {
                                shaped.assign(static_cast<std::size_t>(2 * runCount + 1), ProportionalShapedGlyph {});
                                const int shapedCount = atlas.shapeProportionalRun(atlas.context,
                                    std::span<const std::uint32_t>(runCodePoints.data(), static_cast<std::size_t>(runCount)),
                                    fontFace(isBold, isItalic), weight, shaped.data(), static_cast<int>(shaped.size()));
                                if (shapedCount > 0 && shapedCount <= static_cast<int>(shaped.size())) {
                                    placements.assign(static_cast<std::size_t>(shapedCount), ProportionalGlyphPlacement {});
                                    const double cellAdvance = in.glyphReferenceAdvanceInPixels > 0.0f
                                        ? static_cast<double>(in.glyphReferenceAdvanceInPixels)
                                        : static_cast<double>(naturalCellWidth);
                                    const ProportionalRunLayout layout = layoutProportionalRun(
                                        std::span<const ProportionalShapedGlyph>(shaped.data(), static_cast<std::size_t>(shapedCount)),
                                        std::span<const std::uint32_t>(runCodePoints.data(), static_cast<std::size_t>(runCount)),
                                        widthCells, sizedSpec->horizontalAlign, cellAdvance, placements);
                                    if (layout.glyphCount > 0 && layout.widthInPixels > 0.0) {
                                        proportionalTextLeft = std::min(claimRightX, runStartX + static_cast<float>(layout.penOffsetInPixels) * runPixelScale);
                                        proportionalTextRight = std::min(claimRightX, proportionalTextLeft + static_cast<float>(layout.widthInPixels) * runPixelScale);
                                        hasProportionalTextExtent = proportionalTextRight > proportionalTextLeft;
                                    }
                                    // Drawn one atlas pixel to one screen pixel before
                                    // the explicit local presentation transform. A wide
                                    // cell's quad stretches its slot to the cell pitch,
                                    // because a two-cell glyph has to span its two cells
                                    // whatever the pitch is; a proportional glyph spans
                                    // nothing but itself, and a quad scaled by the pitch
                                    // resamples every glyph a few percent narrower — a
                                    // lighter, blurred stroke beside what CoreText draws.
                                    // The selection's fill under the run: the letters the
                                    // selected cells own, by the cell each letter's ink
                                    // begins in — the ownership the copy takes a letter by —
                                    // from the first such letter's origin to the origin of
                                    // the letter after the last, or the text's end. Cells
                                    // and letters do not coincide in a run, so a fill at
                                    // the cell pitch stands beside the letters it names.
                                    if (isSelectionFillPerClaimedCell && out.boxFills && !isTierPass && row < static_cast<int>(selectionColumn1.size())
                                        && layout.glyphCount > 0) {
                                        thread_local std::vector<std::uint16_t> runOwners;
                                        runOwners.assign(static_cast<std::size_t>(runCount), std::uint16_t { 0 });
                                        proportionalRunCellOwners(
                                            std::span<const ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(layout.glyphCount)),
                                            layout, runCount, widthCells, cellAdvance, runOwners);
                                        // The selected cells of the claim, in the run's own
                                        // cells (a scaled run's are `scale` columns each): a
                                        // row of one piece reads its hull, which may start
                                        // before the claim and end after it, and a row of
                                        // several pieces reads each one overlapping the
                                        // claim on its own, so a piece elsewhere on the row,
                                        // the cells between, and the letters between two
                                        // pieces in the claim light nothing here.
                                        struct ClaimSelection {
                                            int firstSelected;
                                            int lastSelected;
                                            int startCut;
                                            int endCut;
                                            int firstNamedLetter;
                                            int endNamedLetter;
                                        };
                                        thread_local std::vector<ClaimSelection> claimSelections;
                                        claimSelections.clear();
                                        const int claimColumns = std::max(1, widthCells) * runScale;
                                        const auto inRunCells = [&](int offset) {
                                            return offset >= 0 ? offset / runScale : -((runScale - 1 - offset) / runScale);
                                        };
                                        const int piecesFirst = selectionPieceStart[static_cast<std::size_t>(row)];
                                        const int piecesLast = selectionPieceStart[static_cast<std::size_t>(row) + 1];
                                        for (int piece = piecesFirst; piece < piecesLast; ++piece) {
                                            const SelectionRowSpan& span = selectionPieces[static_cast<std::size_t>(piece)];
                                            if (span.column1 < column || span.column0 >= column + claimColumns)
                                                continue;
                                            claimSelections.push_back({ inRunCells(span.column0 - column), inRunCells(span.column1 - column), span.subColumn0,
                                                span.subColumn1, span.firstLetter, span.endLetter });
                                        }
                                        for (const ClaimSelection& claimSelection : claimSelections) {
                                            const int firstSelected = claimSelection.firstSelected;
                                            const int lastSelected = claimSelection.lastSelected;
                                            const int startCut = claimSelection.startCut;
                                            const int endCut = claimSelection.endCut;
                                            int firstLetter = -1;
                                            int lastLetter = -1;
                                            for (int index = 0; index < runCount; ++index) {
                                                const int owner = runOwners[static_cast<std::size_t>(index)];
                                                if (owner < firstSelected || owner > lastSelected)
                                                    continue;
                                                if (firstLetter < 0)
                                                    firstLetter = index;
                                                lastLetter = index;
                                            }
                                            // An end standing inside a run cell, between two of
                                            // its letters, cuts that cell's stretch where the
                                            // copy cuts it: the first cut leaves that many of
                                            // the start cell's letters out, the second keeps
                                            // that many of the end cell's.
                                            const auto stretchStart = [&](int cell) {
                                                return static_cast<int>(std::ranges::lower_bound(runOwners, static_cast<std::uint16_t>(cell)) - runOwners.begin());
                                            };
                                            const bool isStartCut = startCut > 0 && firstSelected >= 0 && firstSelected < widthCells;
                                            const bool isEndCut = endCut > 0 && lastSelected >= 0 && lastSelected < widthCells;
                                            if (isStartCut && firstLetter >= 0)
                                                firstLetter = std::max(firstLetter, std::min(stretchStart(firstSelected) + startCut, runCount));
                                            if (isEndCut && lastLetter >= 0)
                                                lastLetter = std::min(lastLetter, stretchStart(lastSelected) + endCut - 1);
                                            if (firstLetter > lastLetter)
                                                firstLetter = lastLetter = -1;
                                            // Letters the selection names are the letters lit,
                                            // whichever cells this layout puts them in: the
                                            // measurer's may put one a cell from it.
                                            const bool areLettersNamed = claimSelection.firstNamedLetter >= 0;
                                            if (areLettersNamed) {
                                                firstLetter = std::clamp(claimSelection.firstNamedLetter, 0, runCount);
                                                lastLetter = std::clamp(claimSelection.endNamedLetter, 0, runCount) - 1;
                                                if (firstLetter > lastLetter)
                                                    firstLetter = lastLetter = -1;
                                            }
                                            // The claim's selected cells, at the row's pitch: what
                                            // the fill covers where no letter decides — the empty
                                            // cells before a right-aligned run's text and after
                                            // any run's, which a selection runs through
                                            // continuously.
                                            const int claimCells = std::max(1, widthCells);
                                            const int firstClaimed = std::clamp(firstSelected, 0, claimCells);
                                            const int lastClaimed = std::clamp(lastSelected, -1, claimCells - 1);
                                            const float cellPitch = metrics.cellWidth * runXScale;
                                            const float cellsLeft = runStartX + static_cast<float>(firstClaimed) * cellPitch;
                                            const float cellsRight = runStartX + static_cast<float>(lastClaimed + 1) * cellPitch;
                                            if (lastClaimed >= firstClaimed) {
                                                // A letter's origin: the glyph that begins at it, or
                                                // the last glyph begun before it where a ligature
                                                // absorbed it.
                                                const auto originOfLetter = [&](int letter) {
                                                    float origin = 0.0f;
                                                    for (int g = 0; g < layout.glyphCount; ++g) {
                                                        if (placements[static_cast<std::size_t>(g)].cluster > letter)
                                                            break;
                                                        origin = placements[static_cast<std::size_t>(g)].xInPixels;
                                                    }
                                                    return origin;
                                                };
                                                const float textEnd = static_cast<float>(layout.penOffsetInPixels + layout.widthInPixels);
                                                float fillLeft = cellsLeft;
                                                float fillRight = cellsRight;
                                                if (firstLetter >= 0) {
                                                    const float letterLeft = runStartX + originOfLetter(firstLetter) * runPixelScale;
                                                    // Selected from a cell before the one the first
                                                    // letter's ink begins in: the slack there fills
                                                    // too. By the ink's own cell rather than the owner
                                                    // map's, which keeps a run's first letter on the
                                                    // anchor however far in its alignment put it. A
                                                    // cut end is the letter's own edge and nothing
                                                    // beside it.
                                                    const int inkCell = static_cast<int>(std::floor((letterLeft - runStartX) / std::max(cellPitch, 1.0f)));
                                                    // A selection begun before the claim — a whole row, or
                                                    // a drag from the cells before it — covers the claim
                                                    // from its first cell whatever slack the first letter
                                                    // leaves there.
                                                    const bool isSelectedFromBeforeTheClaim = firstSelected < 0 && !isStartCut;
                                                    // Named letters begin at the claim's first cell
                                                    // only where their piece does: slack held before
                                                    // them is a piece of its own.
                                                    const bool isSlackHeld = areLettersNamed ? firstSelected <= 0 && !isStartCut : !isStartCut && firstClaimed < inkCell;
                                                    fillLeft = isSelectedFromBeforeTheClaim || isSlackHeld ? std::min(cellsLeft, letterLeft) : letterLeft;
                                                    // After the last letter the fill ends at the text,
                                                    // where the selection does, unless the selection
                                                    // runs on past the claim — the row's void taken
                                                    // whole — when the claim's tail is lit with it.
                                                    const bool isTailTaken = lastSelected >= claimCells - 1 && !isEndCut;
                                                    fillRight = lastLetter + 1 < runCount
                                                        ? runStartX + originOfLetter(lastLetter + 1) * runPixelScale
                                                        : (isTailTaken ? std::max(runStartX + textEnd * runPixelScale, cellsRight) : runStartX + textEnd * runPixelScale);
                                                } else if (!areLettersNamed && (isStartCut || isEndCut)) {
                                                    // Every letter the cells own was cut away:
                                                    // nothing of the run is selected.
                                                    fillLeft = fillRight = cellsLeft;
                                                }
                                                fillLeft = std::max(runStartX, fillLeft);
                                                fillRight = std::min(claimRightX, fillRight);
                                                // Counted past the capacity, as every fill is: the
                                                // count is what the frame needs, which is what the
                                                // caller grows the stream to. Written only within it.
                                                if (fillRight > fillLeft) {
                                                    const std::size_t fillIndex = counts.boxFills++;
                                                    if (fillIndex < out.boxFillCapacity) {
                                                        OverlayInstance& fill = out.boxFills[fillIndex];
                                                        fill = OverlayInstance {};
                                                        fill.topLeftInPixels = { std::round(fillLeft), std::round(in.originInPixels.y + row * metrics.cellHeight) };
                                                        fill.bottomRightInPixels = { std::round(fillRight), std::round(in.originInPixels.y + (row + 1) * metrics.cellHeight) };
                                                        // The wash a baked cell takes, mixed here from the
                                                        // run's own background and drawn whole: an overlay
                                                        // blended on the target would mix in the target's
                                                        // space, and on an sRGB target that is a lighter
                                                        // plate than the cells beside it.
                                                        const float wash = in.selectionColor.w;
                                                        Vec4 fillColor {
                                                            background.x + (in.selectionColor.x - background.x) * wash,
                                                            background.y + (in.selectionColor.y - background.y) * wash,
                                                            background.z + (in.selectionColor.z - background.z) * wash,
                                                            1.0f,
                                                        };
                                                        fillColor.w = resolvedBackgroundOpacity(fillColor, in);
                                                        fill.color = fillColor;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    const std::size_t firstProportionalGlyph = counts.proportionalGlyphs;
                                    bool didRejectGlyph = false;
                                    float rightmostInk = -std::numeric_limits<float>::infinity();
                                    for (int g = 0; g < layout.glyphCount; ++g) {
                                        const ProportionalGlyphPlacement& placement = placements[g];
                                        // Glyph zero is the face's missing-glyph sentinel,
                                        // not an identity the dedicated sheet can safely
                                        // cache. The strip route can draw the shaper's
                                        // fallback as one image; caching zero here would
                                        // make an unwritten slot look resident forever.
                                        if (placement.glyph == 0) {
                                            didRejectGlyph = true;
                                            break;
                                        }
                                        // The glyph's true origin, split into the whole
                                        // pixel its quad is placed at and the phase it was
                                        // drawn at, so the letter sits where the shaper
                                        // put it to an eighth of a pixel.
                                        const float originX = runStartX + placement.xInPixels * runPixelScale;
                                        const SubpixelPhase snap = phaseForPen(originX,
                                            std::clamp(proportional.phaseCount, 1, subpixelPhaseCount),
                                            in.subpixelPhaseRounding);
                                        const FontFace face = fontFace(isBold, isItalic);
                                        const std::uint32_t variant = cellVariantStyle | static_cast<std::uint32_t>(snap.phase);
                                        const std::uint64_t key = proportionalGlyphKey(
                                            placement.glyph, weight, face,
                                            static_cast<std::uint32_t>(snap.phase), variantTier(variant));
                                        bool isFresh = false;
                                        const int slot = proportional.cache->slotForKey(key, &isFresh);
                                        if (slot < 0) {
                                            didRejectGlyph = true;
                                            break;
                                        }
                                        if (isFresh && !atlas.rasteriseProportionalGlyph(atlas.context, placement.glyph, face, weight, variant, slot)) {
                                            proportional.cache->forgetKey(key);
                                            didRejectGlyph = true;
                                            break;
                                        }
                                        const GlyphUV slotUV = proportional.atlasRectForSlot(slot);
                                        // Cut at the last cell the run claims, the rule
                                        // Docs/proportional-text.md states: a run whose
                                        // text needs more cells than it asked for stops at
                                        // its claim rather than drawing over whatever its
                                        // neighbour put in the next cell. The strip route
                                        // had this for nothing, its raster being exactly
                                        // the claim wide; a per-glyph quad reaches as far
                                        // as its glyph does and has to be trimmed. Trimmed
                                        // rather than dropped, because the cut falls where
                                        // the cells end and not where a letter happens to
                                        // begin — the last glyph is cut through, and the
                                        // strip cut through it too.
                                        const float quadLeft = static_cast<float>(snap.snappedPenX) - static_cast<float>(proportional.marginInPixels.width) * runPixelScale;
                                        if (quadLeft >= claimRightX)
                                            continue;
                                        const float slotWidth = static_cast<float>(proportional.slotWidthInPixels);
                                        const float drawnWidth = std::min(slotWidth * runPixelScale, claimRightX - quadLeft);
                                        if (drawnWidth <= 0.0f)
                                            continue;
                                        if (!proportional.cache->slotHasOwnedInk(slot)) {
                                            didRejectGlyph = true;
                                            break;
                                        }
                                        const auto ink = proportional.cache->ownedInkForSlot(slot, 0);
                                        if (ink.rightInPixels > ink.leftInPixels) {
                                            const float inkLeft = quadLeft + ink.leftInPixels * runPixelScale;
                                            const float inkRight = std::min(claimRightX, quadLeft + ink.rightInPixels * runPixelScale);
                                            // Separate coverage quads composite against
                                            // the run background, not one another. Keep
                                            // intersecting ink in a strip so a weaker edge
                                            // cannot erase the preceding glyph's stroke.
                                            if (inkLeft < inkRight && inkLeft < rightmostInk) {
                                                didRejectGlyph = true;
                                                break;
                                            }
                                            rightmostInk = std::max(rightmostInk, inkRight);
                                        }
                                        const float slotHeight = static_cast<float>(proportional.slotHeightInPixels);
                                        // The glyph's slot top: a margin above its row, or
                                        // on a DECDHL pair or a scaled block that many
                                        // margins above its top row, and the shaper's
                                        // vertical offset scaled with it.
                                        const float quadTop = isBandedRun
                                            ? blockTopInPixels - static_cast<float>(runRows) * presentationScale * (static_cast<float>(proportional.marginInPixels.height) + placement.yInPixels)
                                            : std::round(in.originInPixels.y + row * metrics.cellHeight) - presentationScale * (static_cast<float>(proportional.marginInPixels.height) + placement.yInPixels);
                                        // Emits one quad of the glyph, on a DECDHL row or a
                                        // scaled block's band only the part of it inside
                                        // this row's band.
                                        const auto emitProportionalQuad = [&](Vec2 origin, Vec2 size, Vec2 atlasTopLeft, Vec2 atlasBottomRight) -> GlyphInstance* {
                                            FractionalBandSlice slice { true, origin, size, atlasTopLeft, atlasBottomRight };
                                            if (isBandedRun) {
                                                slice = quadRowSlice(origin, size, atlasTopLeft, atlasBottomRight, bandTopInPixels, bandBottomInPixels);
                                                if (!slice.isVisible)
                                                    return nullptr;
                                            }
                                            const std::size_t index = counts.proportionalGlyphs++;
                                            if (index >= out.proportionalGlyphCapacity)
                                                return nullptr;
                                            GlyphInstance& instance = out.proportionalGlyphs[index];
                                            instance.originInPixels = slice.origin;
                                            instance.sizeInPixels = slice.size;
                                            instance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                            instance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                            instance.atlasTopLeft = slice.atlasTopLeft;
                                            instance.atlasBottomRight = slice.atlasBottomRight;
                                            return &instance;
                                        };
                                        // Doubled or scaled, the glyph is drawn large rather
                                        // than stretched where the backend can: slot-sized
                                        // windows of its sized raster, laid edge to edge
                                        // from the sized quad's left, as many across as its
                                        // sized ink needs. On a DECDHL pair or a scaled
                                        // block the raster is as many slots tall as the
                                        // picture is rows, and a row faults only the slot
                                        // heights that reach into its band. Every window is
                                        // faulted before any is drawn, so a refusal leaves
                                        // the whole glyph on the stretched quad below.
                                        if (runXScale != 1.0f && atlas.rasteriseSizedProportionalGlyph && ink.rightInPixels > ink.leftInPixels) {
                                            // The key's window lane, and the protocol's largest scale.
                                            constexpr int maxSizedWindows = 16;
                                            constexpr int maxSizedRegions = 7;
                                            // Counted with a margin's room past the sized
                                            // ink: a face widened on its width axis has
                                            // tighter side bearings, so its glyph scaled
                                            // to the doubled advance reaches a little past
                                            // twice the ordinary ink. A window with no ink
                                            // in it draws nothing.
                                            const float sizedInkRight = runXScale * (ink.rightInPixels + static_cast<float>(proportional.marginInPixels.width));
                                            const int windows = std::clamp(static_cast<int>(std::ceil(sizedInkRight / slotWidth)), 1, maxSizedWindows);
                                            // The raster's rows of slots, of which each region
                                            // below is one, and what it is drawn at across past
                                            // them: a DECDWL row's raster is one slot tall and
                                            // twice across, a DECDHL pair's two slots and
                                            // twice, and a scaled run's `scale` slots and
                                            // `scale` times, twice that again on a DECDWL row.
                                            const std::uint8_t tileScale = static_cast<std::uint8_t>(runRows);
                                            const std::uint8_t tileWidthScale = static_cast<std::uint8_t>(static_cast<int>(runXScale) / runRows);
                                            std::array<std::uint8_t, maxSizedRegions> regions {};
                                            int regionCount = 0;
                                            for (int region = 0; region < std::min<int>(tileScale, maxSizedRegions); ++region) {
                                                const float regionTop = quadTop + static_cast<float>(region) * slotHeight * presentationScale;
                                                if (!isBandedRun || (regionTop < bandBottomInPixels && regionTop + slotHeight * presentationScale > bandTopInPixels))
                                                    regions[regionCount++] = static_cast<std::uint8_t>(region);
                                            }
                                            GlyphUV windowUV[maxSizedRegions][maxSizedWindows];
                                            bool isEveryWindowPresent = true;
                                            for (int regionIndex = 0; isEveryWindowPresent && regionIndex < regionCount; ++regionIndex) {
                                                for (int window = 0; isEveryWindowPresent && window < windows; ++window) {
                                                    SizedTile windowTile { tileScale, regions[regionIndex], static_cast<std::uint8_t>(window) };
                                                    windowTile.widthScale = tileWidthScale;
                                                    const std::uint64_t windowKey = sizedProportionalGlyphKey(placement.glyph, weight, face,
                                                        static_cast<std::uint32_t>(snap.phase), variantTier(variant), static_cast<std::uint32_t>(window),
                                                        windowTile.scale, windowTile.band, windowTile.widthScale);
                                                    bool isWindowFresh = false;
                                                    const int windowSlot = proportional.cache->slotForKey(windowKey, &isWindowFresh);
                                                    if (windowSlot < 0) {
                                                        isEveryWindowPresent = false;
                                                        break;
                                                    }
                                                    if (isWindowFresh && !atlas.rasteriseSizedProportionalGlyph(atlas.context, placement.glyph, face, weight, variant, windowTile, windowSlot)) {
                                                        proportional.cache->forgetKey(windowKey);
                                                        isEveryWindowPresent = false;
                                                        break;
                                                    }
                                                    windowUV[regionIndex][window] = proportional.atlasRectForSlot(windowSlot);
                                                }
                                            }
                                            if (isEveryWindowPresent) {
                                                for (int regionIndex = 0; regionIndex < regionCount; ++regionIndex) {
                                                    const float windowTop = quadTop + static_cast<float>(regions[regionIndex]) * slotHeight * presentationScale;
                                                    for (int window = 0; window < windows; ++window) {
                                                        const float windowLeft = quadLeft + static_cast<float>(window) * slotWidth * presentationScale;
                                                        const float windowWidth = std::min(slotWidth * presentationScale, claimRightX - windowLeft);
                                                        if (windowWidth <= 0.0f)
                                                            break;
                                                        const GlyphUV& uv = windowUV[regionIndex][window];
                                                        emitProportionalQuad({ windowLeft, windowTop }, { windowWidth, slotHeight * presentationScale },
                                                            { uv.minU, uv.minV }, { uv.minU + (uv.maxU - uv.minU) * windowWidth / (slotWidth * presentationScale), uv.maxV });
                                                    }
                                                }
                                                continue;
                                            }
                                        }
                                        // The atlas is read in its own pixels, so the
                                        // quad's trim takes the same fraction of the
                                        // entry: a cut quad showing the whole entry would
                                        // squeeze the letter instead of cutting it.
                                        GlyphInstance* const glyphInstance = emitProportionalQuad({ quadLeft, quadTop },
                                            { drawnWidth, slotHeight * presentationScale * static_cast<float>(runRows) },
                                            { slotUV.minU, slotUV.minV },
                                            { slotUV.minU + (slotUV.maxU - slotUV.minU) * (slotWidth > 0.0f ? drawnWidth / (slotWidth * runPixelScale) : 1.0f), slotUV.maxV });
                                        if (!glyphInstance)
                                            continue;
                                        const GlyphInstance& gi = *glyphInstance;
                                        // Where this glyph's ink lies and which slot holds
                                        // it, for the rule beneath: the quad is drawn one
                                        // atlas pixel to one screen pixel, so a screen x
                                        // maps onto the slot by the quad's left edge alone.
                                        // A doubled quad samples two screen pixels per
                                        // texel, which the rule's tap cannot say; its rule
                                        // stays solid.
                                        if (metrics.skipInk && runXScale == 1.0f && !proportionalSkipInkClaims.empty() && ink.rightInPixels > ink.leftInPixels) {
                                            ProportionalGlyphInk record;
                                            record.inkLeftInPixels = quadLeft + ink.leftInPixels * presentationScale;
                                            record.inkRightInPixels = std::min(claimRightX, quadLeft + ink.rightInPixels * presentationScale);
                                            record.quadLeftInPixels = quadLeft;
                                            record.cellTopInSlot = ((in.originInPixels.y + row * metrics.cellHeight) - gi.originInPixels.y) / presentationScale;
                                            record.slot = slotUV;
                                            proportionalGlyphInks.push_back(record);
                                        }
                                    }
                                    if (didRejectGlyph) {
                                        // A run is atomic. Keeping the prefix would draw it
                                        // again under the strip fallback and make the seam
                                        // visible as darkened letters. Slots faulted before
                                        // the rejection may stay warm, but no instance from
                                        // this failed route survives.
                                        counts.proportionalGlyphs = firstProportionalGlyph;
                                        proportionalGlyphInks.resize(firstGlyphInkOfRun);
                                    } else {
                                        didDrawGlyphs = layout.glyphCount > 0;
                                    }
                                }
                            }
                            const std::size_t firstStrip = counts.proportionalStrips;
                            int completedStripTiles = 0;
                            int tileCount = !didDrawGlyphs && hasProportionalStripRouteInWalk ? 1 : 0;
                            // The strip is asked for at the run's size: a DECDWL row's
                            // twice across, a DECDHL pair's and a scaled run's that
                            // many rows tall, of which each request names one slot
                            // height, the band. A backend that draws it so says it in
                            // the placement, and the band rows draw the slot heights
                            // that reach into them one texel a pixel; one that cannot
                            // answers the run at its own size, which is stretched.
                            constexpr int maxStripRegions = 7;
                            SizedTile stripSize { static_cast<std::uint8_t>(runRows), 0, 0 };
                            stripSize.widthScale = static_cast<std::uint8_t>(static_cast<int>(runXScale) / runRows);
                            std::array<std::uint8_t, maxStripRegions> stripRegions {};
                            int stripRegionCount = 1;
                            bool isStripAtSize = false;
                            const auto askForStrip = [&](int tileIndex, std::uint8_t band) {
                                stripSize.band = band;
                                return atlas.proportionalStrip(atlas.context,
                                    std::span<const std::uint32_t>(runCodePoints.data(), static_cast<std::size_t>(runCount)),
                                    widthCells, sizedSpec->horizontalAlign, penResidual, fontFace(isBold, isItalic), weight, cellVariantStyle, tileIndex,
                                    stripSize);
                            };
                            if (tileCount > 0 && isBandedRun) {
                                const ProportionalStripPlacement probe = askForStrip(0, 0);
                                isStripAtSize = probe.strip.present && probe.drawnScale >= static_cast<float>(runRows);
                                if (isStripAtSize) {
                                    // The slot heights of the strip at its size that reach
                                    // into this row.
                                    const float probeHeight = (probe.strip.maxUV.y - probe.strip.minUV.y) * glyphScaleY;
                                    const float probeTop = blockTopInPixels + probe.topInPixels * glyphScaleY;
                                    stripRegionCount = 0;
                                    for (int region = 0; region < std::min(runRows, maxStripRegions); ++region) {
                                        const float regionTop = probeTop + static_cast<float>(region) * probeHeight;
                                        if (regionTop < bandBottomInPixels && regionTop + probeHeight > bandTopInPixels)
                                            stripRegions[stripRegionCount++] = static_cast<std::uint8_t>(region);
                                    }
                                }
                            }
                            for (int regionIndex = 0; regionIndex < stripRegionCount; ++regionIndex) {
                                for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
                                    const ProportionalStripPlacement placement = askForStrip(tileIndex, stripRegions[regionIndex]);
                                    // Whatever of the doubling the backend did not draw, the
                                    // quad stretches. A strip a backend drew at its own size
                                    // is one row tall, so on a DECDHL pair or a scaled block
                                    // it is stretched the same both ways, keeping its
                                    // letterforms' proportions.
                                    const float stripXScale = runPixelScale / std::max(1.0f, placement.drawnWidthScale);
                                    if (!placement.strip.present || placement.tileCount <= 0 || placement.tileCount > maxColumns) {
                                        counts.proportionalStrips = firstStrip;
                                        regionIndex = stripRegionCount;
                                        break;
                                    }
                                    tileCount = placement.tileCount;
                                    if (regionIndex == 0)
                                        ++completedStripTiles;
                                    const std::size_t stripIndex = placement.strip.present ? counts.proportionalStrips++ : out.proportionalStripCapacity;
                                    const float scaleY = glyphScaleY;
                                    const float stripRectHeight = placement.strip.maxUV.y - placement.strip.minUV.y;
                                    const float slotHeight = stripRectHeight > 0.0f ? stripRectHeight
                                        : in.glyphSlotHeightInPixels > 0.0f         ? in.glyphSlotHeightInPixels
                                                                                    : naturalCellHeight;
                                    if (metrics.skipInk && runXScale == 1.0f && scaleY > 0.0f && placement.widthInPixels > 0.0f) {
                                        const float quadLeft = penWholeX + placement.leftInPixels * presentationScale;
                                        const float quadRight = quadLeft + placement.widthInPixels * presentationScale;
                                        const float rowTop = in.originInPixels.y + row * metrics.cellHeight;
                                        const float quadTop = std::round(rowTop) + placement.topInPixels * scaleY;
                                        const float sourceTop = std::clamp(placement.strip.minUV.y + (rowTop - quadTop) / scaleY,
                                            placement.strip.minUV.y, placement.strip.maxUV.y);
                                        const float sourceBottom = std::clamp(sourceTop + metrics.cellHeight / scaleY,
                                            placement.strip.minUV.y, placement.strip.maxUV.y);
                                        if (placement.tileCount == 1 && claimedWidthCells > 0 && sourceBottom > sourceTop) {
                                            // The proportional claim emits one decoration rule
                                            // spanning all its cells. Map that complete rule to
                                            // the one strip; samples in alignment slack fall
                                            // outside the slot clamp and leave the rule solid.
                                            SkipInkTap& anchorTap = runSkipInkTaps.front();
                                            anchorTap.inkMinUV = { placement.strip.minUV.x + (runStartX - quadLeft) / presentationScale, sourceTop };
                                            anchorTap.inkMaxUV = { anchorTap.inkMinUV.x + claimedWidthCells * metrics.cellWidth / presentationScale, sourceBottom };
                                            anchorTap.boxMinUV = placement.strip.minUV;
                                            anchorTap.boxMaxUV = placement.strip.maxUV;
                                            anchorTap.present = true;
                                            anchorTap.sheet = SkipInkSheet::ProportionalStrip;
                                        }
                                        const int firstSliceColumn = placement.tileCount == 1 ? 1 : 0;
                                        for (int runColumn = firstSliceColumn; runColumn < claimedWidthCells; ++runColumn) {
                                            const float segmentLeft = runStartX + runColumn * metrics.cellWidth;
                                            const float segmentRight = segmentLeft + metrics.cellWidth;
                                            // A cell split across two bounded tiles has no
                                            // single texture rect. Leave its tap absent so the
                                            // rule stays solid rather than sampling one half as
                                            // though it described the complete cell.
                                            if (segmentLeft < quadLeft || segmentRight > quadRight || sourceBottom <= sourceTop)
                                                continue;
                                            SkipInkTap& tap = runSkipInkTaps[static_cast<std::size_t>(runColumn)];
                                            tap.inkMinUV = { placement.strip.minUV.x + (segmentLeft - quadLeft) / presentationScale, sourceTop };
                                            tap.inkMaxUV = { placement.strip.minUV.x + (segmentRight - quadLeft) / presentationScale, sourceBottom };
                                            tap.boxMinUV = placement.strip.minUV;
                                            tap.boxMaxUV = placement.strip.maxUV;
                                            tap.present = true;
                                            tap.sheet = SkipInkSheet::ProportionalStrip;
                                        }
                                    }
                                    if (stripIndex < out.proportionalStripCapacity && out.proportionalStrips) {
                                        // The strip's own slot, not the mono slot: a face whose
                                        // proportional glyphs overhang the cell cuts its strips
                                        // taller and says where their top sits, so the quad
                                        // takes the rect the placement names and the top it gives.
                                        GlyphInstance& stripInstance = out.proportionalStrips[stripIndex];
                                        // The row's top rounded, as the per-glyph route rounds it: a
                                        // frame whose origin sits on a half pixel would otherwise put
                                        // the strip's top edge on a pixel centre, and a GPU's edge
                                        // rule then draws it a row above the glyphs beside it.
                                        stripInstance.originInPixels = { penWholeX + placement.leftInPixels * stripXScale,
                                            std::round(in.originInPixels.y + row * metrics.cellHeight) + placement.topInPixels * scaleY };
                                        // A claim running past the grid's edge is cut there,
                                        // and the quad keeps the part of the strip left of
                                        // the cut rather than the whole squeezed into it. A
                                        // claim on the grid keeps its strip whole, margins
                                        // and all.
                                        float stripWidth = placement.widthInPixels * stripXScale;
                                        float stripShare = 1.0f;
                                        if (visibleClaimCells < claimCells && stripWidth > 0.0f) {
                                            const float room = std::clamp(claimRightX - stripInstance.originInPixels.x, 0.0f, stripWidth);
                                            stripShare = room / stripWidth;
                                            stripWidth = room;
                                        }
                                        stripInstance.sizeInPixels = { stripWidth, slotHeight * scaleY };
                                        stripInstance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                        stripInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                        stripInstance.atlasTopLeft = placement.strip.minUV;
                                        stripInstance.atlasBottomRight = { placement.strip.minUV.x + (placement.strip.maxUV.x - placement.strip.minUV.x) * stripShare,
                                            placement.strip.maxUV.y };
                                        if (isStripAtSize) {
                                            // The slot height the backend cut from the strip
                                            // at its size, standing where the placement puts it
                                            // from the block's top, and the part of it inside
                                            // this row's band.
                                            const FractionalBandSlice slice = quadRowSlice(
                                                { stripInstance.originInPixels.x, blockTopInPixels + placement.topInPixels * scaleY },
                                                stripInstance.sizeInPixels, stripInstance.atlasTopLeft, stripInstance.atlasBottomRight, bandTopInPixels, bandBottomInPixels);
                                            stripInstance.originInPixels = slice.origin;
                                            stripInstance.sizeInPixels = slice.isVisible ? slice.size : Vec2 {};
                                            stripInstance.atlasTopLeft = slice.atlasTopLeft;
                                            stripInstance.atlasBottomRight = slice.atlasBottomRight;
                                        } else if (isBandedRun) {
                                            // The strip as many times as tall as the picture
                                            // is rows, from the block's top, and the part of
                                            // it inside this row's band.
                                            const float rowsTall = static_cast<float>(runRows);
                                            const FractionalBandSlice slice = quadRowSlice(
                                                { stripInstance.originInPixels.x, blockTopInPixels + rowsTall * placement.topInPixels * scaleY },
                                                { stripInstance.sizeInPixels.x, rowsTall * stripInstance.sizeInPixels.y },
                                                stripInstance.atlasTopLeft, stripInstance.atlasBottomRight, bandTopInPixels, bandBottomInPixels);
                                            stripInstance.originInPixels = slice.origin;
                                            stripInstance.sizeInPixels = slice.isVisible ? slice.size : Vec2 {};
                                            stripInstance.atlasTopLeft = slice.atlasTopLeft;
                                            stripInstance.atlasBottomRight = slice.atlasBottomRight;
                                        }
                                    }
                                }
                            }
                            if (metrics.skipInk && runXScale == 1.0f && !proportionalSkipInkClaims.empty()) {
                                if (!didDrawGlyphs && tileCount > 0 && completedStripTiles == tileCount) {
                                    std::ranges::copy(runSkipInkTaps, proportionalSkipInkTaps.begin() + column);
                                }
                                if (didDrawGlyphs) {
                                    // Each cell of the claim taps the glyph whose ink covers
                                    // most of it, mapped onto that glyph's slot at the
                                    // cell's own pitch — the mono tap's shape, so the rule
                                    // emitter's band fractions and neighbour mapping read
                                    // it unchanged. A cell no ink reaches keeps no tap and
                                    // its rule stays solid, as a blank mono cell's does.
                                    const std::size_t firstInk = firstGlyphInkOfRun;
                                    for (int runColumn = 0; runColumn < claimedWidthCells; ++runColumn) {
                                        const float columnLeft = runStartX + runColumn * metrics.cellWidth;
                                        const float columnRight = columnLeft + metrics.cellWidth;
                                        const ProportionalGlyphInk* best = nullptr;
                                        float bestOverlap = 0.0f;
                                        for (std::size_t index = firstInk; index < proportionalGlyphInks.size(); ++index) {
                                            const ProportionalGlyphInk& candidate = proportionalGlyphInks[index];
                                            const float overlap = std::min(columnRight, candidate.inkRightInPixels) - std::max(columnLeft, candidate.inkLeftInPixels);
                                            if (overlap > bestOverlap) {
                                                bestOverlap = overlap;
                                                best = &candidate;
                                            }
                                        }
                                        SkipInkTap& tap = proportionalSkipInkTaps[static_cast<std::size_t>(column + runColumn)];
                                        tap = SkipInkTap {};
                                        if (!best)
                                            continue;
                                        tap.inkMinUV = { best->slot.minU + (columnLeft - best->quadLeftInPixels) / presentationScale, best->slot.minV + best->cellTopInSlot };
                                        tap.inkMaxUV = { best->slot.minU + (columnRight - best->quadLeftInPixels) / presentationScale, best->slot.minV + best->cellTopInSlot + metrics.cellHeight / presentationScale };
                                        tap.boxMinUV = { best->slot.minU, best->slot.minV };
                                        tap.boxMaxUV = { best->slot.maxU, best->slot.maxV };
                                        tap.present = true;
                                        tap.sheet = SkipInkSheet::Proportional;
                                    }
                                    std::fill_n(proportionalPerGlyphClaims.begin() + column, static_cast<std::size_t>(claimedWidthCells), static_cast<std::uint8_t>(1u));
                                }
                                // A look-ahead may have cached this anchor as a mono tap
                                // before its strip was submitted. Force the current cell
                                // and its neighbours to resolve against the routed result.
                                tapWindowColumn = -2;
                            }
                        } else if (isLigatureCovered) {
                            const LigatureRun& run = ligatureRuns[static_cast<std::size_t>(column)];
                            // The strip's pixels are on the grid pitch by the seam's
                            // contract — every backend anchors each cluster at its own
                            // cell — so a slice maps to its cell 1:1 and the reference
                            // advance stays what it is: the MONO canvas's basis. Scaling
                            // by it here stretches tracked text on one arm and packs it on
                            // another: the scale assumes a pen-run raster the portable
                            // sheet never produces. Only the uniform local
                            // presentation transform scales that original pixel domain.
                            const float scaleX = presentationScale;
                            const float scaleY = glyphScaleY;
                            // The whole run shares one pen, snapped once at its start. A
                            // per-cell snap would quantise each slice separately and open
                            // seams inside an image that was rasterised as one piece.
                            const float runStartX = in.originInPixels.x + run.startColumn * metrics.cellWidth;
                            const float penX = drawsSubpixelGlyphs
                                ? static_cast<float>(phaseForPen(runStartX, in.subpixelPhaseCount, in.subpixelPhaseRounding).snappedPenX)
                                : runStartX;
                            const float fullOriginX = penX - in.glyphMarginInPixels.width * scaleX;
                            const float segmentLeft = in.originInPixels.x + column * metrics.cellWidth;
                            const float segmentRight = in.originInPixels.x + (column + 1) * metrics.cellWidth;
                            const LigatureStripSlice slice = run.hasOwnedInk
                                ? ligatureOwnedInkSlice(run.ownInkLeftInAtlasPixels,
                                      run.ownInkRightInAtlasPixels, run.slotWidthInAtlasPixels)
                                : ligatureStripCellSlice(column, run.startColumn,
                                      run.startColumn + run.length, (segmentLeft - fullOriginX) / scaleX,
                                      (segmentRight - fullOriginX) / scaleX, run.usedWidthInAtlasPixels,
                                      run.contextInsetLeftInAtlasPixels, run.contextInsetRightInAtlasPixels);
                            // A measured column owning no ink — the tail of a multi-cell
                            // ligature, whose whole image the lead column carries — emits
                            // nothing rather than a zero-width quad.
                            const bool ownsInk = !run.hasOwnedInk || slice.atlasRight > slice.atlasLeft;
                            const float slotHeight = in.glyphSlotHeightInPixels > 0.0f ? in.glyphSlotHeightInPixels : naturalCellHeight;
                            const std::size_t ligatureIndex = ownsInk ? counts.ligatureGlyphs++ : 0;
                            if (ownsInk && ligatureIndex < out.ligatureGlyphCapacity) {
                                GlyphInstance& ligatureInstance = out.ligatureGlyphs[ligatureIndex];
                                // The rows the strip's raster inked, or the whole slot
                                // where nothing measured them. Both map onto the screen
                                // by the same affine as the slot, so the quad samples
                                // the texels it always sampled and only stops covering
                                // rows that hold nothing.
                                // One guard row a side around the ink, for the reason
                                // ``measureInkBox`` gives: a linear tap at the quad's edge
                                // reads half a texel outward.
                                const float stripHeight = run.strip.maxUV.y - run.strip.minUV.y;
                                const float sliceTop = run.hasInkRows ? std::clamp(run.inkTopInAtlasPixels - 1.0f, 0.0f, stripHeight) : 0.0f;
                                const float sliceBottom = run.hasInkRows ? std::clamp(run.inkBottomInAtlasPixels + 1.0f, sliceTop, stripHeight) : slotHeight;
                                const float slotTop = in.originInPixels.y + row * metrics.cellHeight - in.glyphMarginInPixels.height * scaleY;
                                markStripInkCrossings(rowInkCrossings, in.originInPixels.y, metrics.cellHeight, in.rows,
                                    run.hasInkRows ? slotTop + run.inkTopInAtlasPixels * scaleY : slotTop,
                                    run.hasInkRows ? slotTop + run.inkBottomInAtlasPixels * scaleY : slotTop + slotHeight * scaleY);
                                ligatureInstance.originInPixels = { fullOriginX + slice.atlasLeft * scaleX, slotTop + sliceTop * scaleY };
                                ligatureInstance.sizeInPixels = { (slice.atlasRight - slice.atlasLeft) * scaleX, (sliceBottom - sliceTop) * scaleY };
                                ligatureInstance.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                ligatureInstance.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                // The strip rect spans the whole slot, so a slice's UV is
                                // its atlas-pixel range as a fraction of that slot. Doing it
                                // here rather than asking the atlas keeps the seam at one
                                // call per row instead of one per covered cell.
                                // The slice is measured in the strip's own atlas pixels and
                                // the strip rect is in those pixels too, so the
                                // fraction-of-the-slot conversion is an addition.
                                ligatureInstance.atlasTopLeft = { run.strip.minUV.x + slice.atlasLeft, run.strip.minUV.y + sliceTop };
                                ligatureInstance.atlasBottomRight = { run.strip.minUV.x + slice.atlasRight,
                                    run.hasInkRows ? run.strip.minUV.y + sliceBottom : run.strip.maxUV.y };
                                // The slice is what the column draws, which may reach past
                                // the ink by the fringe its producer reported; the ink is
                                // the record's own span, on the same atlas-to-screen map.
                                if (run.hasOwnedInk && run.inkRightInAtlasPixels > run.inkLeftInAtlasPixels) {
                                    hasInkExtent = true;
                                    inkScreenLeft = fullOriginX + run.inkLeftInAtlasPixels * scaleX;
                                    inkScreenRight = fullOriginX + run.inkRightInAtlasPixels * scaleX;
                                }
                            }
                        }
                    }();
                }

                // Subpixel GlyphInstance: pixel-snapped origin at the phase from the
                // fractional pen X, slot size in device px (margin-free default ⇒ the
                // cell box). foreground/background are the resolved colors before the
                // window-opacity scale. Skipped when the slot didn't fault in (atlas
                // full / no coverage) — the cell then shows background only.
                if (isSubpixelGlyph && glyph.present) {
                    const float slotWidth = in.glyphSlotWidthInPixels > 0.0f ? in.glyphSlotWidthInPixels : naturalCellWidth;
                    const float slotHeight = in.glyphSlotHeightInPixels > 0.0f ? in.glyphSlotHeightInPixels : naturalCellHeight;
                    // Cell width includes tracking; the raster keeps the face's own
                    // advance. Only the atlas's backing scale changes its pixel size.
                    const float scaleX = atlasToDeviceScale(metrics);
                    const float scaleY = glyphScaleY;
                    // Sharp sized: one ordinary quad per claimed column, each
                    // sampling a cell-shaped window of a master the backend
                    // rasterised at the block's scale, instead of one quad
                    // stretching the band-cropped 1x slot over the whole claim.
                    // The windows and the soft quad cover the same destination;
                    // only the texels differ, which is the contract the crop site
                    // states for an own-sheet master.
                    //
                    // Gated off super/subscript: the soft path shrinks its one
                    // quad about the block's centre, and shrinking each window
                    // about its own would pull the tiles apart at every boundary.
                    // Every window is probed before any quad is emitted, so a full
                    // sheet degrades the whole cell to soft rather than leaving a
                    // column of the glyph blank.
                    bool didDrawSharp = false;
                    if (sizedSpec && sizedSpec->scale >= 2 && atlas.sizedGlyph && penScript(&pen) == 0) {
                        const int windows = static_cast<int>(std::lround(glyphWidthCells / cellXStep));
                        AtlasGlyph tiles[maxSizedWindowsPerBand];
                        const auto windowAt = [&](int bandRow, int w) {
                            const SizedTile tileRef { sizedSpec->scale, static_cast<std::uint8_t>(bandRow), static_cast<std::uint8_t>(w) };
                            return atlas.sizedGlyph(atlas.context, cell.codePoint,
                                fontFace(isBold, isItalic), mono.weight,
                                cellVariantStyle | static_cast<std::uint32_t>(phaseSnap.phase), GlyphPass::Subpixel, tileRef);
                        };
                        const int band = static_cast<int>(Ubiquitty::penTextBand(&pen));
                        const SizedWindowRows blockRows = visibleSizedBands(row, band, sizedSpec->scale, in.rows, windows);
                        const AtlasGlyph* const blockWindows = sizedBlocks.admit(mono.cache, row - band, column, SizedBlockArm::Subpixel, blockRows, band, windowAt);
                        bool isEveryWindowPresent = blockWindows != nullptr;
                        for (int w = 0; isEveryWindowPresent && w < windows; ++w) {
                            tiles[w] = blockWindows[w];
                            isEveryWindowPresent = tiles[w].present;
                        }
                        if (isEveryWindowPresent) {
                            // A window's quad is its own cell, not the slot. The
                            // slot is cut for the widest glyph the face overhangs
                            // to — three cells under a symbol fallback — and a
                            // window's raster fills it: the magnified glyph runs
                            // on past the window's cell into the slot's tail. A
                            // slot-wide quad then paints that tail over the next
                            // two cells, every window repainting the ink its
                            // neighbours paint, and its right edge lands on the
                            // slot boundary, where the sampler blends in the next
                            // slot's first column — the next window, holding the
                            // same glyph a cell along, so the column is dashed by
                            // the glyph's own strokes. That was a hairline through
                            // every scaled letter and one standing two cells past
                            // the block. Each window therefore takes the cell it
                            // steps by; only the first keeps the margin before it
                            // and the last the margin after, where the glyph's
                            // bearings overhang and no other window paints.
                            const float windowStep = sizedWindowStepInPixels(in.glyphReferenceAdvanceInPixels,
                                in.glyphBoxWidthInPixels > 0.0f ? in.glyphBoxWidthInPixels : naturalCellWidth);
                            const float boxHeight = slotHeight - 2.0f * in.glyphMarginInPixels.height;
                            const float bandHeight = sizedBandHeightInPixels(in.glyphReferenceLineHeightInPixels, boxHeight);
                            for (int w = 0; w < windows; ++w) {
                                const std::size_t sharpIndex = counts.glyphs++;
                                if (sharpIndex >= out.glyphCapacity || !out.glyphs)
                                    continue;
                                const float reachLeft = w == 0 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachRight = w == windows - 1 ? in.glyphMarginInPixels.width : 0.0f;
                                const float reachTop = Ubiquitty::penTextBand(&pen) == 0 ? in.glyphMarginInPixels.height : 0.0f;
                                const float reachBottom = Ubiquitty::penTextBand(&pen) + 1 == sizedSpec->scale ? in.glyphMarginInPixels.height : 0.0f;
                                GlyphInstance& gi = out.glyphs[sharpIndex];
                                const float windowLeft = sizedWindowLeftInPixels(w, windowStep);
                                const float windowWidth = sizedWindowLeftInPixels(w + 1, windowStep) - windowLeft;
                                gi.originInPixels = { static_cast<float>(phaseSnap.snappedPenX)
                                        + (windowLeft - reachLeft) * scaleX,
                                    in.originInPixels.y + row * metrics.cellHeight - reachTop * scaleY };
                                gi.sizeInPixels = { (reachLeft + windowWidth + reachRight) * scaleX,
                                    (bandHeight + reachTop + reachBottom) * scaleY };
                                gi.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                gi.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                // The entry returns the cell-box rect, whose left
                                // edge is where the window's cell begins.
                                gi.atlasTopLeft = { tiles[w].minUV.x - reachLeft,
                                    tiles[w].minUV.y - reachTop };
                                gi.atlasBottomRight = { tiles[w].minUV.x + windowWidth + reachRight,
                                    sizedBandBottom(tiles[w].minUV.y, tiles[w].maxUV.y, bandHeight, boxHeight) + reachBottom };
                            }
                            didDrawSharp = true;
                        }
                    }
                    const std::size_t glyphIndex = didDrawSharp ? out.glyphCapacity : counts.glyphs++;
                    if (glyphIndex < out.glyphCapacity && out.glyphs) {
                        GlyphInstance& gi = out.glyphs[glyphIndex];
                        // The origin backs off by the SCALED margins so the glyph's
                        // pen origin lands on the snapped penX rather than beside it.
                        const float claimedColumns = glyphWidthCells / cellXStep;
                        // The slot's left margin is part of what a magnified quad
                        // stretches, so the origin backs off by the margin as magnified:
                        // a quad drawn at `claimedColumns` times its slot starts its
                        // glyph's left edge that many margins out, and backing off by
                        // one would start the ink inside the columns it claims.
                        gi.originInPixels = { static_cast<float>(phaseSnap.snappedPenX) - in.glyphMarginInPixels.width * scaleX * claimedColumns,
                            in.originInPixels.y + row * metrics.cellHeight - in.glyphMarginInPixels.height * scaleY };
                        // A sized glyph spans scale columns of its block. The cell
                        // path spends `glyphTileCount` instances partitioning U
                        // across that span; one quad carrying the whole U over the
                        // whole span is the same picture, and it is what this stream
                        // can express. The span is 1 for an ordinary cell, so this
                        // is inert off the sized path — and
                        // a wide character never arrives here, because isWide
                        // excludes the subpixel stream.
                        gi.sizeInPixels = { slotWidth * scaleX * claimedColumns, slotHeight * scaleY };
                        // A sized band is a stretched slice of the slot: its rows are
                        // the ones the crop cut, margin included on the outer bands,
                        // magnified by the block's scale into the band's own cell row.
                        // Every band takes the whole slot's rows otherwise, and the
                        // slices of neighbouring bands overlap and gap.
                        const bool isSizedBand = sizedSpec && sizedSpec->scale >= 2;
                        if (isSizedBand) {
                            const float bandScale = scaleY * static_cast<float>(sizedSpec->scale);
                            gi.originInPixels.y = in.originInPixels.y + row * metrics.cellHeight - sizedRows.reachTop * bandScale;
                            gi.sizeInPixels.y = (glyph.maxUV.y - glyph.minUV.y) * bandScale;
                        }
                        gi.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                        gi.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                        // A backend that measured its raster reports the box the ink
                        // covers, and the quad shrinks to it: the slot carries a
                        // margin and a phase-headroom column on every side, so
                        // slot-sized quads overlap their neighbours and the pass
                        // shades far more screen than the glyphs' ink. The dropped
                        // fragments sampled zero coverage and blended nothing, so
                        // the pixels are the ones the slot quad produced.
                        //
                        // Two shapes keep the slot quad. A sized glyph spans
                        // claimed columns and partitions its U across them, which
                        // the ink box does not describe. A scripted glyph is
                        // scaled about its quad's centre, and the tight quad's
                        // centre is not the slot's, so the transform would move
                        // the letter.
                        const bool wantsTightQuad = glyph.hasInkBounds && claimedColumns == 1.0f
                            && penScript(&pen) == Ubiquitty::VT::scriptNone;
                        const float inkWidth = glyph.inkMaxUV.x - glyph.inkMinUV.x;
                        const float inkHeight = glyph.inkMaxUV.y - glyph.inkMinUV.y;
                        if (wantsTightQuad && (inkWidth <= 0.0f || inkHeight <= 0.0f)) {
                            // A raster with no ink — a space routed here, a blank
                            // fallback — needs no quad at all. The decoration pass
                            // below still runs: an underlined blank keeps its rule.
                            counts.glyphs--;
                        } else if (wantsTightQuad) {
                            // The slot's atlas origin anchors the ink offsets: the
                            // quad's slot origin is the pen backed off by the
                            // scaled margins, already in gi.originInPixels.
                            const float slotMinU = glyph.minUV.x - in.glyphMarginInPixels.width;
                            const float slotMinV = glyph.minUV.y - in.glyphMarginInPixels.height;
                            gi.originInPixels.x += (glyph.inkMinUV.x - slotMinU) * scaleX;
                            gi.originInPixels.y += (glyph.inkMinUV.y - slotMinV) * scaleY;
                            gi.sizeInPixels = { inkWidth * scaleX, inkHeight * scaleY };
                            gi.atlasTopLeft = { glyph.inkMinUV.x, glyph.inkMinUV.y };
                            gi.atlasBottomRight = { glyph.inkMaxUV.x, glyph.inkMaxUV.y };
                            // The box carries one guard texel a side; the ink is
                            // what lies inside it.
                            hasInkExtent = true;
                            inkScreenLeft = gi.originInPixels.x + scaleX;
                            inkScreenRight = gi.originInPixels.x + gi.sizeInPixels.x - scaleX;
                        } else {
                            // monoGlyph returns the cell-box UV. The quad is the FULL
                            // slot, so expand the UV by the margin (UV units) — a glyph
                            // wider than the cell box samples its overflow from the slot
                            // margins instead of clipping. margin-free ⇒ no expansion.
                            //
                            // The texel size hoisted at the top of the pass is used here
                            // rather than asked for again: it is the reciprocal of the
                            // atlas dimensions, which both backends fix at construction
                            // and never reassign, so it cannot change mid-pass. Exhaustion
                            // reports failure instead of growing — see doubleGlyph. An
                            // atlas that reallocated on exhaustion would break that, and
                            // would invalidate the hoist, not just this line.
                            //
                            // A sized band's rows already carry the margin, on the outer
                            // bands, that this would add to every one.
                            const float marginY = isSizedBand ? 0.0f : in.glyphMarginInPixels.height;
                            gi.atlasTopLeft = { glyph.minUV.x - in.glyphMarginInPixels.width, glyph.minUV.y - marginY };
                            gi.atlasBottomRight = { glyph.maxUV.x + in.glyphMarginInPixels.width, glyph.maxUV.y + marginY };
                            // SGR 73/74 super/subscript: shrink the glyph ~0.7 about its
                            // center and shift it off the baseline within its cell
                            // (background + decorations stay full-cell).
                            const ScriptGlyphBox scripted = scriptGlyphBox(gi.originInPixels, gi.sizeInPixels, penScript(&pen), metrics.cellHeight, glyphSizingFor(in));
                            gi.originInPixels = scripted.origin;
                            gi.sizeInPixels = scripted.size;
                        }
                    }
                }

                // The same shrink for a cell whose glyph would have been baked into
                // its quad. The box is the cell's own, so the picture is what the
                // cell would have drawn, moved and scaled by the helper the
                // subpixel path just used — one description of super/subscript
                // rather than two that can drift apart.
                //
                // The atlas rect is the one the baked route resolved, which is the
                // glyph box rather than the padded slot: this quad is the cell,
                // not the slot, so the two agree.
                if (isScriptedOwnSheetGlyph && glyph.present) [[unlikely]] {
                    [&, cellGridX, fullWidthCells, isScriptedOwnSheetGlyph, mayUseNativeBackground] [[gnu::noinline]] {
                        if (isScriptedOwnSheetGlyph && glyph.present) {
                            const std::size_t scriptedIndex = counts.glyphs++;
                            if (scriptedIndex < out.glyphCapacity && out.glyphs) {
                                GlyphInstance& gi = out.glyphs[scriptedIndex];
                                const ScriptGlyphBox scripted = scriptGlyphBox(
                                    { in.originInPixels.x + cellGridX * metrics.cellWidth,
                                        in.originInPixels.y + row * metrics.cellHeight },
                                    { metrics.cellWidth * fullWidthCells, metrics.cellHeight },
                                    penScript(&pen), metrics.cellHeight, glyphSizingFor(in));
                                gi.originInPixels = scripted.origin;
                                gi.sizeInPixels = scripted.size;
                                gi.foreground = nativeSecondaryInk(usesNativeTextVibrancy, foreground, background, in, framePalette8.defaultForeground);
                                gi.background = nativeBackgroundInk(usesNativeBackgroundVibrancy, background, in, mayUseNativeBackground);
                                gi.atlasTopLeft = glyph.minUV;
                                gi.atlasBottomRight = glyph.maxUV;
                            }
                        }
                    }();
                }

                // --- Decoration pass ---
                const Ubiquitty::VT::AttributeSet attributes = pen.attributes;
                // Underline shape only: penUnderlineShape masks off the super/sub
                // script bits that share Pen::underlineStyle, so a scripted cell
                // with no underline doesn't draw a spurious rule.
                const uint8_t underlineStyle = penUnderlineShape(&pen);
                const bool hasSGRUnderline = attributes.contains(Ubiquitty::VT::Attribute::Underline) || underlineStyle != Ubiquitty::VT::underlineNone;
                // A link cell underlines even without an SGR underline (the link affordance).
                const bool hasUnderline = hasSGRUnderline || isLinkCell;
                // Ideogram attribute (SGR 60-64): a mark on a cell edge. ideogramNone
                // keeps wantDecoration false so plain rows cost nothing.
                const uint8_t ideogram = penIdeogram(&pen);
                const bool wantDecoration = !attributes.contains(Ubiquitty::VT::Attribute::Hidden) && (hasUnderline || attributes.contains(Ubiquitty::VT::Attribute::Strike) || attributes.contains(Ubiquitty::VT::Attribute::Overline) || attributes.containsAny({ Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled }) || ideogram != Ubiquitty::VT::ideogramNone);
                if (!wantDecoration || (ablate & AblateDecorations))
                    continue;

                // Cell pixel rect (device px) tracks the DECDWL placement: the
                // logical column lands at gridX, and the straddling cell's rule
                // stops at the grid edge (cellVisibleWidth). NORMAL rows reduce to
                // column*cellWidth + (wide?2:1).
                // Grid origin included, as every pixel-space stream carries it. A
                // decoration rect is viewport pixels, the same space the glyph and
                // box-fill origins are in; leaving it out puts the rules a grid
                // inset from the glyphs they underline, which only shows where the
                // inset is non-zero.
                const float cellLeft = in.originInPixels.x + cellGridX * metrics.cellWidth;
                const float cellTop = in.originInPixels.y + row * metrics.cellHeight;
                const float cellRight = cellLeft + (hasFraction ? glyphWidthCells : cellSpanCells) * metrics.cellWidth;
                const float cellBottom = cellTop + metrics.cellHeight;
                const float baselineY = cellTop + metrics.baselineFromTop;

                const auto emitCellRule = [&](DecorationSite site, float left, float right, float topOfCell, float cellHeight,
                                              float top, float bottom, Vec4 color, DecorationKind kind, float thickness,
                                              bool shouldMaskInk, bool hasMask, Vec2 minInk, Vec2 maxInk) { // bool-ok: masking policy and available coverage are independent predicates.
                    if (!hasFraction) [[likely]] {
                        emitRule(site, left, right, topOfCell, cellHeight, top, bottom, color, kind, thickness, shouldMaskInk, hasMask, minInk, maxInk);
                        return;
                    }
                    const Vec2 areaOrigin = fractionArea.map(fractionBlockOrigin, fractionBlockOrigin);
                    const float verticalScale = fractionBlockRows * fractionArea.scale;
                    const float horizontalScale = static_cast<float>(sizedSpec->scale) * cellXStep * fractionArea.scale;
                    const float ruleWidth = right - left;
                    left = std::round(areaOrigin.x) + (left - cellLeft) * fractionArea.scale;
                    right = std::round(areaOrigin.x) + (right - cellLeft) * fractionArea.scale;
                    if (site == DecorationSite::FramedLeft)
                        right = left + ruleWidth * horizontalScale;
                    else if (site == DecorationSite::FramedRight)
                        left = right - ruleWidth * horizontalScale;
                    else if (site == DecorationSite::IdeogramStress) {
                        const float center = (left + right) * 0.5f;
                        left = center - ruleWidth * horizontalScale * 0.5f;
                        right = center + ruleWidth * horizontalScale * 0.5f;
                    }
                    top = std::round(areaOrigin.y) + (top - cellTop) * verticalScale;
                    bottom = std::round(areaOrigin.y) + (bottom - cellTop) * verticalScale;
                    right = std::min(right, in.originInPixels.x + in.columns * metrics.cellWidth);
                    if (kind == DecorationKind::Dotted || kind == DecorationKind::Curly) {
                        // Their band height defines the shape, so cutting it would
                        // shrink the dots or wave. The row containing its centre
                        // owns the complete rule, exactly once across the block.
                        const int firstRow = static_cast<int>(std::floor((fractionBlockOrigin.y - in.originInPixels.y) / metrics.cellHeight));
                        const int lastRow = firstRow + static_cast<int>(fractionBlockRows) - 1;
                        const int ownerRow = std::clamp(static_cast<int>(std::floor(((top + bottom) * 0.5f - in.originInPixels.y) / metrics.cellHeight)),
                            std::max(firstRow, 0), std::min(lastRow, in.rows - 1));
                        if (row != ownerRow)
                            return;
                    } else {
                        top = std::max(top, cellTop);
                        bottom = std::min(bottom, cellBottom);
                    }
                    if (right <= left || bottom <= top)
                        return;
                    const std::size_t index = counts.decorations;
                    emitRule(site, left, right, std::round(areaOrigin.y), cellHeight * verticalScale,
                        top, bottom, color, kind, thickness * verticalScale, shouldMaskInk, hasMask, minInk, maxInk);
                    if (index < out.decorationCapacity && out.decorations)
                        out.decorations[index].dashPeriodInPixels *= horizontalScale;
                };

                // A bold cell takes the face's bold rule where it has one, so the
                // underline under heavy text carries the same weight the glyphs do.
                const bool hasBoldRule = isBold && metrics.underlineThicknessBold > 0.0f;
                float thick = hasBoldRule ? metrics.underlineThicknessBold : metrics.underlineThickness;
                if (thick < 1.0f)
                    thick = 1.0f;
                const float underlineDrop = hasBoldRule ? metrics.underlineDropBold : metrics.underlineDrop;
                float thickCurly = metrics.underlineCurlyThickness > 0.0f ? metrics.underlineCurlyThickness : metrics.underlineThickness;
                if (thickCurly < 0.75f)
                    thickCurly = 0.75f;
                float strikeThick = (isBold && metrics.strikeThicknessBold > 0.0f) ? metrics.strikeThicknessBold : metrics.strikeThickness;
                if (strikeThick < 1.0f)
                    strikeThick = 1.0f;

                // Underline color: colorDefault follows the (baked) text foreground;
                // colorRGB uses the pen's ul channels; else the live palette.
                //
                // Resolved against the pen's own text colour, which on a link cell is
                // the colour before the link tint — see `foregroundBeforeLink`.
                const Vec4& underlineBase = isLinkCell ? foregroundBeforeLink : foreground;
                const RGB8 foregroundByte { channelToByte(underlineBase.x), channelToByte(underlineBase.y), channelToByte(underlineBase.z) };
                const RGB8 underlineByte = resolveUnderlineColor(pen, *in.palette, foregroundByte);
                const float underlineAlpha = resolveUnderlineAlpha(pen, in.paletteForegroundAlpha, foreground.w, blinkCurve);
                Vec4 underlineColor { underlineByte.red / 255.0f, underlineByte.green / 255.0f, underlineByte.blue / 255.0f, underlineAlpha };
                // The link affordance's own rule takes the link colour, which comes
                // from the frontend's theme rather than the palette — so it has no
                // slot to take an opacity from and rides the text's.
                //
                // Only when the cell has no SGR underline of its own: a rule the
                // content asked for keeps its colour as well as its style, or an
                // SGR 58 underline would be repainted for the single reason that a
                // link happens to cover the same cell.
                if (isLinkCell && !hasSGRUnderline) [[unlikely]]
                    underlineColor = { linkTint.x, linkTint.y, linkTint.z, foreground.w };

                // The mask is resolved for this cell and both neighbours. A rule is
                // per-cell, so a descender leaning in from the cell next door is cut
                // by its rule and not by this one — without the neighbours a halo
                // stops dead at the cell edge, and the gap under a `y` followed by a
                // space ends halfway through the letter.
                //
                // The slot rects come from the cache directly rather than from the
                // cell's own `glyph`: `atlasRectForBuilder` contracts the slot by the
                // margin, and the coverage tap wants the glyph box (`atlasRectForSlot`) for
                // the sample and the FULL slot (`atlasRectForSlotFull`) as the clamp, so a
                // dilation tap cannot wander into the neighbouring glyph's pixels.
                bool haveInkMask = false;
                // A column a strip covers taps the strip, which is what it
                // draws; a column of a run drawn glyph by glyph taps the glyph
                // that inks it; the mono tap is for the glyph the cell would
                // have drawn.
                const int rowBase = row * in.columns;
                auto tapFor = [&](int tapColumn) {
                    if (tapColumn >= 0 && tapColumn < in.columns
                        && !proportionalSkipInkClaims.empty()
                        && proportionalSkipInkClaims[static_cast<std::size_t>(tapColumn)] != 0u)
                        return proportionalSkipInkTaps[static_cast<std::size_t>(tapColumn)];
                    if (shouldShapeLigatures && tapColumn >= 0 && tapColumn < in.columns) {
                        const LigatureRun& covering = ligatureRuns[static_cast<std::size_t>(tapColumn)];
                        if (covering.length > 0 && covering.strip.present)
                            return stripSkipInkTap(in, covering, tapColumn, metrics);
                    }
                    if (!isDoubleWide && tapColumn >= 0 && tapColumn < in.columns
                        && Ubiquitty::cellPen(in.cells[static_cast<std::size_t>(rowBase + tapColumn)]).attributes.containsAny({ Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation }))
                        return wideSkipInkTap(in, atlas, wide, mono.weight, rowBase, tapColumn, metrics, cellVariantStyle, rowLetters.empty() ? nullptr : rowLetters.data());
                    return resolveSkipInkTap(in, atlas, mono, rowBase, tapColumn, metrics, cellVariantStyle, rowLetters.empty() ? nullptr : rowLetters.data());
                };
                if (metrics.skipInk && mono.cache) {
                    // The window rolls along a run of decorated cells: the
                    // predecessor's self tap is this cell's left, its right this
                    // cell's self, and only the look-ahead resolves. Carrying a
                    // tap across the glyph fetches in between is sound because a
                    // slot touched this pass is never evicted within it
                    // (GlyphSlotCache::beginFrame), so a carried UV cannot be
                    // recycled under another glyph mid-frame.
                    if (column == tapWindowColumn + 1) {
                        leftTap = selfTap;
                        selfTap = rightTap;
                    } else {
                        selfTap = tapFor(column);
                        leftTap = tapFor(column - 1);
                    }
                    rightTap = tapFor(column + 1);
                    tapWindowColumn = column;
                    haveInkMask = selfTap.present || leftTap.present || rightTap.present;
                } else {
                    selfTap = SkipInkTap {};
                    leftTap = SkipInkTap {};
                    rightTap = SkipInkTap {};
                }
                if (hasFraction) {
                    // The block maps one glyph across the rule's entire advance;
                    // neighbouring stored cells are not neighbouring glyph taps.
                    leftTap = SkipInkTap {};
                    rightTap = SkipInkTap {};
                    haveInkMask = selfTap.present;
                    tapWindowColumn = -2;
                }
                // An underline that follows the ink: at a run's outer edge the rule
                // reaches past the cell box to where the letter's ink ends — a
                // healed `w` at a word's end, an italic `f` — rather than stopping
                // at the advance while the letter carries on. Only where no
                // underlined neighbour stands on that side: between two
                // underlined cells the rules already meet. Patterned rules keep
                // the cell box, because their phase starts at the rule's left
                // edge and a widened first cell would put its dots out of step
                // with the next.
                float underlineLeft = cellLeft;
                float underlineRight = cellRight;
                if (metrics.shouldUnderlineFollowInk && hasInkExtent && hasUnderline && !hasFraction) {
                    auto isUnderlinedNeighbour = [&](int neighbourColumn) {
                        if (neighbourColumn < 0 || neighbourColumn >= in.columns)
                            return false;
                        const Pen& neighbourPen = in.cells[static_cast<std::size_t>(row * in.columns + neighbourColumn)].pen;
                        return neighbourPen.attributes.contains(Ubiquitty::VT::Attribute::Underline)
                            || penUnderlineShape(&neighbourPen) != Ubiquitty::VT::underlineNone;
                    };
                    const float leftOverhang = cellLeft - inkScreenLeft;
                    const float rightOverhang = inkScreenRight - cellRight;
                    if (leftOverhang >= 0.5f && !isUnderlinedNeighbour(column - 1))
                        underlineLeft = std::floor(inkScreenLeft);
                    if (rightOverhang >= 0.5f && !isUnderlinedNeighbour(column + 1))
                        underlineRight = std::ceil(inkScreenRight);
                }
                // A proportional run's rules lie under its text, not across the
                // alignment slack its claim leaves beside it: a right-aligned or
                // centred run's rule starts at its pen and ends where its text
                // does. A patterned rule's phase starts at the rule's left edge,
                // so it begins at the left edge of the cell the pen stands in,
                // keeping its period on the cells' pitch.
                float ruleLeft = cellLeft;
                float ruleRight = cellRight;
                float patternedRuleLeft = cellLeft;
                if (hasProportionalTextExtent) {
                    ruleLeft = proportionalTextLeft;
                    ruleRight = proportionalTextRight;
                    underlineLeft = proportionalTextLeft;
                    underlineRight = proportionalTextRight;
                    patternedRuleLeft = cellLeft + std::floor((proportionalTextLeft - cellLeft) / metrics.cellWidth) * metrics.cellWidth;
                }
                // The skip-ink tap spans the rule, so a widened rule widens its
                // window by the same pixels: an atlas pixel is a device pixel on
                // the mono path.
                const Vec2 inkTopLeft = { selfTap.inkMinUV.x - (cellLeft - underlineLeft), selfTap.inkMinUV.y };
                const Vec2 inkBottomRight = { selfTap.inkMaxUV.x + (underlineRight - cellRight), selfTap.inkMaxUV.y };
                // The shader maps a rule's width onto its tap's box, and a point a
                // fraction across it onto the neighbours' boxes a whole box to
                // either side. A rule covering only part of its box, as a run's
                // text beside its alignment slack does, narrows all three to that
                // part, so each point still samples the ink above it.
                auto narrowTapsTo = [&](float left, float right, float boxLeft, float boxRight) {
                    const float boxWidth = boxRight - boxLeft;
                    if (boxWidth <= 0.0f || (left <= boxLeft && right >= boxRight))
                        return;
                    const float offset = (left - boxLeft) / boxWidth;
                    const float share = (right - left) / boxWidth;
                    auto narrow = [&](SkipInkTap& tap, float boxesToTheRight) {
                        if (!tap.present)
                            return;
                        const float span = tap.inkMaxUV.x - tap.inkMinUV.x;
                        tap.inkMinUV.x += (offset + boxesToTheRight * (1.0f - share)) * span;
                        tap.inkMaxUV.x = tap.inkMinUV.x + share * span;
                    };
                    narrow(selfTap, 0.0f);
                    narrow(leftTap, 1.0f);
                    narrow(rightTap, -1.0f);
                };

                // A rule that is cut by ink. Over a claim drawn glyph by glyph it
                // is emitted one cell at a time, each cell tapping the glyph that
                // inks it and its neighbours theirs: one rule over the claim could
                // name one slot, and the glyphs are in as many. Cell by cell rather
                // than glyph by glyph because a patterned rule's phase is the
                // cell's, and a segment starting mid-cell would restart it. A strip's
                // claim and a mono cell keep their one rule and its taps.
                //
                // A phased two-cell glyph is ruled the same way, a column at a
                // time, each column tapping its half of the glyph: a tap is one cell
                // wide wherever the shader maps a neighbour onto it.
                const bool isPerGlyphClaim = metrics.skipInk
                    && ((!proportionalPerGlyphClaims.empty() && proportionalPerGlyphClaims[static_cast<std::size_t>(column)] != 0u)
                        || isWidePhased);
                // The per-glyph arm, apart from the rule every other cell takes, so
                // the call sites inline the common rule and only a claim drawn
                // glyph by glyph reaches the loop.
                auto emitPerGlyphRule = [&](DecorationSite site, float left, float right, float top, float bottom, Vec4 color, DecorationKind kind, float thickness) {
                    const SkipInkTap savedSelf = selfTap, savedLeft = leftTap, savedRight = rightTap;
                    // The cells the rule crosses. A rule that starts or ends
                    // inside its claim, as a proportional run's text beside its
                    // alignment slack does, crosses only the cells its text does;
                    // one reaching the claim's edges keeps the claim's count.
                    const int firstRuleCell = left > cellLeft ? static_cast<int>(std::floor((left - cellLeft) / metrics.cellWidth)) : 0;
                    const int endRuleCell = right < cellRight
                        ? static_cast<int>(std::ceil((right - cellLeft) / metrics.cellWidth))
                        : std::max(1, static_cast<int>(std::lround((right - std::min(left, cellLeft)) / metrics.cellWidth)));
                    for (int ruleCell = firstRuleCell; ruleCell < endRuleCell; ++ruleCell) {
                        const int tapColumn = column + ruleCell;
                        const float tapLeft = cellLeft + ruleCell * metrics.cellWidth;
                        const float tapRight = tapLeft + metrics.cellWidth;
                        const float segmentLeft = std::max(left, tapLeft);
                        const float segmentRight = std::min(right, tapRight);
                        if (segmentRight <= segmentLeft)
                            continue;
                        selfTap = tapFor(tapColumn);
                        leftTap = tapFor(tapColumn - 1);
                        rightTap = tapFor(tapColumn + 1);
                        const bool hasMask = selfTap.present || leftTap.present || rightTap.present;
                        narrowTapsTo(segmentLeft, segmentRight, tapLeft, tapRight);
                        emitCellRule(site, segmentLeft, segmentRight, cellTop, metrics.cellHeight, top, bottom, color, kind, thickness, true, hasMask, selfTap.inkMinUV, selfTap.inkMaxUV);
                    }
                    selfTap = savedSelf;
                    leftTap = savedLeft;
                    rightTap = savedRight;
                };
                auto emitMaskedRule = [&] [[gnu::always_inline]] (DecorationSite site, float left, float right, float top, float bottom, Vec4 color, DecorationKind kind, float thickness) {
                    if (!isPerGlyphClaim) [[likely]] {
                        if (hasProportionalTextExtent) [[unlikely]] {
                            const SkipInkTap savedSelf = selfTap, savedLeft = leftTap, savedRight = rightTap;
                            narrowTapsTo(left, right, cellLeft, cellRight);
                            emitCellRule(site, left, right, cellTop, metrics.cellHeight, top, bottom, color, kind, thickness, true, haveInkMask, inkTopLeft, inkBottomRight);
                            selfTap = savedSelf;
                            leftTap = savedLeft;
                            rightTap = savedRight;
                            return;
                        }
                        emitCellRule(site, left, right, cellTop, metrics.cellHeight, top, bottom, color, kind, thickness, true, haveInkMask, inkTopLeft, inkBottomRight);
                        return;
                    }
                    emitPerGlyphRule(site, left, right, top, bottom, color, kind, thickness);
                };

                // Every straight rule snaps its edges and its weight to whole device
                // pixels. A fractional edge antialiases the stroke across two rows,
                // which reads as a lighter line than its neighbours and makes the
                // four sides of a framed box come out at visibly different weights.
                //
                // The snapped value is the GEOMETRY only. `thicknessInPixels` keeps
                // the unrounded weight, which is what the shader scales its dot and
                // dash periods and its skip-ink band by — rounding that too would
                // step the pattern a whole pixel between adjacent point sizes.
                // Whole device pixels, and above 1x the rule keeps the fraction it
                // asked for rather than losing it.
                //
                // Nothing downstream draws a partial row: a rule of 1.5 device
                // pixels paints one fully inked row and nothing else, so the
                // fraction is not rendered, it is discarded. Rounding to nearest
                // therefore costs weight — an 11pt rule wants 1.375 device pixels at
                // 2x and `std::round` hands it 1, the same single pixel a 6pt rule
                // gets, so the weight stops tracking the type size.
                //
                // That was addressed once by raising ruleGeometry's floor to a
                // point, which fixed 11pt by making every size from 6pt to 14pt come
                // out at exactly two device pixels — one weight for the whole range
                // a terminal is read at, and twice what 6pt had. The floor cannot
                // tell those cases apart, because it is a constant lower bound and
                // 6pt's weight is below 11pt's by construction.
                //
                // Rounding up can. It moves only the values a round-to-nearest sends
                // DOWN, which is exactly the population that loses weight: 1.375
                // becomes 2 and 0.75 stays at the floor's 1. At every scale: AppKit
                // paints a face's rule as ceil(thickness × scale) rows on a 1x
                // display too (Monaco 14pt asks 1.06 pixels and gets two), and a
                // 1x rule rounded down is the one place this renderer would draw a
                // lighter underline than TextEdit beside it.
                auto snapped = [](float value) { return snappedRuleWeight(value); };
                // A rule belongs to its cell. The snapping biases downward by half a
                // point so a fractional centre cannot smear the stroke across two
                // rows, and on a cell whose baseline already sits at the last row
                // that bias rounds the whole rule past the bottom edge — where it is
                // clipped and nothing is drawn at all. A rule pushed out is a rule
                // at the bottom, never a missing one.
                //
                // Cells are tall enough on macOS that this never binds, which is why
                // a parity run against the inline loop cannot see it: both paths
                // agree, and the case that disagrees is a short cell neither scene
                // has. No test covers it either: on the 16px Unifont cell the
                // stroke lift already carries the rule to row 15, so the pixel
                // assertion there holds with or without this clamp. Its effect on
                // pixels is unasserted anywhere in the tree.
                // The rule must BEGIN inside its cell; its weight may hang below,
                // exactly as the double pair's does. Only the first is needed for
                // the guarantee above, and the difference is not cosmetic: a floor
                // of cellBottom - thickness fits the whole stroke inside, which for
                // a thick rule means lifting it to the x-height, and skip-ink then
                // erases every part of it that crosses a stem. The setting's one job
                // is to make the rule heavier, so a floor that derives from the
                // thickness fights the only thing the setting controls.
                //
                // Stated as a floor rather than as a tolerance on the overshoot: a
                // tolerance reads the same and behaves backwards, because a larger
                // allowance then makes more displacements small enough to clamp.
                // The floor still moves down by the configured underline_offset, so a
                // rule the operator asked to sit lower keeps its offset while a rule
                // pushed past the cell by ROUNDING still comes back. Thickness is the
                // term that had to go: an offset is asked for, a thickness is not.
                const float offsetFloor = metrics.underlineOffset > 0.0f ? metrics.underlineOffset : 0.0f;
                // The lowest top that keeps a rule `band` pixels tall inside the
                // cell, moved down by whatever the configured underline offset
                // asked for.
                //
                // A band rather than a stroke, because a pair is clamped as a
                // pair: bounding the first rule of a double and letting the second
                // follow puts that one below the cell floor, where it reads as the
                // next row's overline and, on the last row, is drawn outside the
                // grid. The overline pair states the same rule for the direction
                // it grows in.
                auto withinCellBand = [&](float top, float band) {
                    const float lowest = cellBottom - band + offsetFloor;
                    return top > lowest ? lowest : top;
                };
                auto withinCell = [&](float top) { return withinCellBand(top, 1.0f); };

                if (hasUnderline) {
                    uint8_t style = underlineStyle != Ubiquitty::VT::underlineNone ? underlineStyle : Ubiquitty::VT::underlineSingle;
                    // A link cell with no SGR underline carries the link affordance,
                    // in the style the frontend resolved from the user's setting. An
                    // SGR underline outranks it: the cell asked for that rule itself.
                    if (isLinkCell && !hasSGRUnderline) [[unlikely]] {
                        style = linkState == LinkState::RestingFile ? in.linkUnderlineStyleFile
                            : linkState == LinkState::RestingMailto ? in.linkUnderlineStyleMailto
                                                                    : in.linkUnderlineStyle;
                    }
                    // Hover outranks both the resting link style and an SGR underline
                    // the cell asked for: while the pointer is on a run, its rule is
                    // the affordance saying so, and a run whose cells each kept their
                    // own style would not read as one run.
                    if (isHoveredLink) [[unlikely]] {
                        style = Ubiquitty::VT::underlineSingle;
                        underlineColor = { in.linkHoverColor.x, in.linkHoverColor.y, in.linkHoverColor.z, foreground.w };
                    }
                    const float strokeLift = (style == Ubiquitty::VT::underlineDotted || style == Ubiquitty::VT::underlineCurly) ? 0.0f : metrics.underlineStrokeLift;
                    const float uy = baselineY + underlineDrop - strokeLift;
                    switch (style) {
                    case Ubiquitty::VT::underlineDouble: {
                        // Two rules of EQUAL weight a gap apart, fitting below the
                        // baseline without meeting the next row's overline. The gap
                        // floors at a pixel so the pair never merges into one rule.
                        const float t = snapped(thick);
                        const float gap = doubledRuleGap(t, metrics.deviceScale);
                        // Not passed through withinCell. The pair is three strokes
                        // tall, so fitting it inside the cell lifts both rules a
                        // row into the descender band, where skip-ink
                        // cuts each one at every stem and the survivors read as a
                        // single thick line. A double underline that does not read
                        // as two has lost the only thing that distinguishes it.
                        // The pair hangs from the underline position downward. A
                        // thicker stroke may not carry the upper rule up past it:
                        // above uy is the descender band, where skip-ink erases
                        // whatever crosses a stem, and an upper rule eaten to
                        // fragments over a solid lower one is the merged look this
                        // geometry exists to avoid. The floor binds only where the
                        // stroke is thick enough to climb.
                        const float top = std::max(std::round(uy - t + metrics.deviceScale), std::round(uy));
                        emitMaskedRule(DecorationSite::UnderlineDoubleUpper, underlineLeft, underlineRight, top, top + t, underlineColor, DecorationKind::Solid, thick);
                        emitMaskedRule(DecorationSite::UnderlineDoubleLower, underlineLeft, underlineRight, top + t + gap, top + t + gap + t, underlineColor, DecorationKind::Solid, thick);
                        break;
                    }
                    case Ubiquitty::VT::underlineCurly: {
                        // band = stroke + amplitude room, kept shallow: the
                        // amplitude is band/2 - halfThick, so a tall band swings the
                        // wave down into the descenders it is meant to sit under.
                        const float band = thickCurly * 2.5f + 2.0f;
                        const float curlDrop = 0.75f * metrics.deviceScale;
                        // Diagnostic, off unless asked: the numbers the curl is
                        // built from, so two arms that draw it differently can be
                        // compared on their inputs rather than their pixels.
                        static const bool shouldLogMetrics = std::getenv("UBIQUITTY_METRICS_LOG") != nullptr;
                        if (shouldLogMetrics)
                            PAL::logLine(stderr, "metrics-log curly thickCurly={} curlyThickness={} underlineThickness={} deviceScale={} uy={} band={} cellHeight={}",
                                thickCurly, metrics.underlineCurlyThickness, metrics.underlineThickness, metrics.deviceScale, uy, band, metrics.cellHeight);
                        emitMaskedRule(DecorationSite::UnderlineCurly, patternedRuleLeft, ruleRight, uy - band * 0.5f + curlDrop, uy + band * 0.5f + curlDrop, underlineColor, DecorationKind::Curly, thickCurly);
                        break;
                    }
                    case Ubiquitty::VT::underlineDotted: {
                        // The fragment fills the band with a ball of radius band/2,
                        // so the band is the dot diameter. It tracks the cell width
                        // so dots grow with the font, with a stroke-derived floor
                        // that keeps them visible at small sizes.
                        const float diameter = std::max(metrics.cellWidth * 0.225f, thick * 2.0f);
                        const float dotDrop = diameter * 0.2f;
                        emitMaskedRule(DecorationSite::UnderlineDotted, patternedRuleLeft, ruleRight, uy - diameter * 0.5f + dotDrop, uy + diameter * 0.5f + dotDrop, underlineColor, DecorationKind::Dotted, thick);
                        break;
                    }
                    case Ubiquitty::VT::underlineDashed: {
                        // The dash shader fills the band in the "on" phase, so the
                        // band is each dash's weight — a touch heavier than the
                        // single rule, or the dashes read as a hairline beside the
                        // round dots.
                        // The top sits on the pixel grid and the height is the
                        // weight itself. A band centred on a fractional position
                        // splits one dash across three rows and none of them
                        // reads solid, which is the hairline the dashes showed
                        // beside the 2px rules; a height rounded to whole rows
                        // instead answers the thickness setting in whole-pixel
                        // steps only, and the setting offers finer ones than
                        // that -- `everyUnderlineShapeAnswersASubPixelThicknessStep`
                        // is the claim, and a quarter-step is inside one row.
                        const float dash = std::max(thick * 1.3f, 1.0f);
                        const float top = withinCell(std::max(std::round(uy - dash * 0.5f + 0.5f * metrics.deviceScale), std::round(uy)));
                        emitMaskedRule(DecorationSite::UnderlineDashed, patternedRuleLeft, ruleRight, top, top + dash, underlineColor, DecorationKind::Dashed, thick);
                        break;
                    }
                    case Ubiquitty::VT::underlineSingle:
                    default: {
                        const float t = snapped(thick);
                        // Centred on the underline position, with the same floor the
                        // double pair carries and for the same reason: above uy is
                        // the type, and skip-ink erases whatever crosses a stem. A
                        // centred rule puts half its weight up there, so it thins as
                        // it is asked to thicken — at the maximum setting the top
                        // half is eaten across the whole run and what survives reads
                        // as a THINNER rule sitting lower than the double's, which is
                        // the opposite of the setting's one job.
                        //
                        // The floor binds only where the stroke is thick enough to
                        // climb; a hairline straddles uy untouched.
                        //
                        const float top = withinCell(singleUnderlineTop(uy, t, metrics.deviceScale));
                        emitMaskedRule(DecorationSite::UnderlineSingle, underlineLeft, underlineRight, top, top + t, underlineColor, DecorationKind::Solid, thick);
                        break;
                    }
                    }
                }

                if (attributes.contains(Ubiquitty::VT::Attribute::Strike)) {
                    // Rides the x-height midpoint, in the text foreground rather
                    // than the underline colour.
                    const float strikeSnap = snapped(strikeThick);
                    const float strikeRow = strikeTop(baselineY, metrics.xHeight, strikeSnap);
                    emitCellRule(DecorationSite::Strike, ruleLeft, ruleRight, cellTop, metrics.cellHeight, strikeRow, strikeRow + strikeSnap, foreground, DecorationKind::Solid, thick, false, false, inkTopLeft, inkBottomRight);
                }

                if (attributes.contains(Ubiquitty::VT::Attribute::Overline)) {
                    const float overlineWeight = snapped(thick);
                    const float overlineTop = std::round(cellTop);
                    emitCellRule(DecorationSite::Overline, ruleLeft, ruleRight, cellTop, metrics.cellHeight, overlineTop, overlineTop + overlineWeight, underlineColor, DecorationKind::Solid, thick, false, false, inkTopLeft, inkBottomRight);
                }

                if (attributes.containsAny({ Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled })) {
                    // A one-stroke box around the cell; encircled reuses the
                    // rectangle rather than drawing an ellipse.
                    const float boxWeight = snapped(thick);
                    const float boxTop = std::round(cellTop);
                    const float boxBottom = std::round(cellBottom);
                    const float boxLeft = std::round(cellLeft);
                    const float boxRight = std::round(cellRight);
                    emitCellRule(DecorationSite::FramedTop, cellLeft, cellRight, cellTop, metrics.cellHeight, boxTop, boxTop + boxWeight, underlineColor, DecorationKind::Solid, thick, false, false, inkTopLeft, inkBottomRight);
                    emitCellRule(DecorationSite::FramedBottom, cellLeft, cellRight, cellTop, metrics.cellHeight, boxBottom - boxWeight, boxBottom, underlineColor, DecorationKind::Solid, thick, false, false, inkTopLeft, inkBottomRight);
                    // The two vertical edges carry the snapped weight, because their
                    // narrow x span is the stroke rather than a band the shader
                    // fills.
                    emitCellRule(DecorationSite::FramedLeft, boxLeft, boxLeft + boxWeight, cellTop, metrics.cellHeight, boxTop, boxBottom, underlineColor, DecorationKind::Solid, boxWeight, false, false, inkTopLeft, inkBottomRight);
                    emitCellRule(DecorationSite::FramedRight, boxRight - boxWeight, boxRight, cellTop, metrics.cellHeight, boxTop, boxBottom, underlineColor, DecorationKind::Solid, boxWeight, false, false, inkTopLeft, inkBottomRight);
                }

                if (ideogram != Ubiquitty::VT::ideogramNone) {
                    // SGR 60-64 are named for two writing directions: 60 is
                    // "ideogram underline or right side line", 62 "overline or left
                    // side line". A mark sits at the foot of the text, and in
                    // vertical writing the foot of a column is its right edge, which
                    // is the side-line half of each name. A terminal sets
                    // horizontally, so the first half is the one that applies, and
                    // the constants say as much: ideogramUnderline is an underline.
                    //
                    // Drawn from the same rule line and the same weight the SGR 4
                    // underline uses, because a reader seeing both in one line must
                    // not see two heights of underline: the ideogram marks are the
                    // same typographic furniture reached by a different escape.
                    const float t = snapped(thick);
                    const float gap = doubledRuleGap(t, metrics.deviceScale);
                    const float uy = baselineY + underlineDrop - metrics.underlineStrokeLift;

                    if (ideogram == Ubiquitty::VT::ideogramUnderline || ideogram == Ubiquitty::VT::ideogramDoubleUnderline) {
                        // On the rule line the SGR 4 underline sits on, and held
                        // inside the cell by the stroke alone — the single rule,
                        // and the pair's upper rule with its lower a gap below,
                        // the way the SGR 4:2 pair hangs. One line of text then
                        // shows one height of underline whichever escape drew it,
                        // on every arm: the line is derived from the face's own
                        // drop, which every arm carries alike, where a seat in the
                        // leading below the glyph reads a line spacing the arms
                        // report differently for one cell — the macOS arm's
                        // snapped cell names the face's leading as spacing, the
                        // portable arm folds it into the cell — and drew the same
                        // rule two pixels apart on the two. Holding the pair's
                        // whole band inside the cell lifted it off the line
                        // wherever the cell could not seat three strokes below,
                        // and put the upper rule into the descender band where
                        // skip-ink cuts it at every stem; the lower hangs past
                        // the floor by at most a gap and a stroke, into the row
                        // below's leading, as the SGR 4:2 lower does.
                        const float top = withinCellBand(std::max(std::round(uy - t * 0.5f + 0.5f * metrics.deviceScale), std::round(uy)), t);
                        // Cut like the SGR 4 rule, a column at a time under a
                        // glyph drawn glyph by glyph: one tap stretched over a
                        // two-cell rule would place each cut half a glyph away
                        // from the stroke that made it.
                        emitMaskedRule(DecorationSite::IdeogramUnderLeft, cellLeft, cellRight, top, top + t, underlineColor, DecorationKind::Solid, t);
                        if (ideogram == Ubiquitty::VT::ideogramDoubleUnderline)
                            emitMaskedRule(DecorationSite::IdeogramUnderRight, cellLeft, cellRight, top + t + gap, top + t + gap + t, underlineColor, DecorationKind::Solid, t);
                    } else if (ideogram == Ubiquitty::VT::ideogramOverline || ideogram == Ubiquitty::VT::ideogramDoubleOverline) {
                        // The pair hangs INWARD from the top edge, as the double
                        // underline hangs down from the rule line: above the cell top
                        // is the previous row, and a rule that grows into it reads as
                        // that row's underline.
                        const float top = std::round(cellTop);
                        emitCellRule(DecorationSite::IdeogramOverLeft, cellLeft, cellRight, cellTop, metrics.cellHeight, top, top + t, underlineColor, DecorationKind::Solid, t, false, false, inkTopLeft, inkBottomRight);
                        if (ideogram == Ubiquitty::VT::ideogramDoubleOverline)
                            emitCellRule(DecorationSite::IdeogramOverRight, cellLeft, cellRight, cellTop, metrics.cellHeight, top + t + gap, top + t + gap + t, underlineColor, DecorationKind::Solid, t, false, false, inkTopLeft, inkBottomRight);
                    } else { // ideogramStress
                        // Emphasis marking: the CJK convention is a dot ABOVE each
                        // character in horizontal setting — kenten, the same mark CSS
                        // spells text-emphasis. A dot and not a dash: a short
                        // horizontal stroke at this size is read as a piece of an
                        // overline, which is the neighbouring attribute.
                        //
                        // A single mark per character, centred on it. Emphasis belongs to
                        // the character; a mark repeated across the cell is a dotted
                        // overline, which is the neighbouring attribute again. Centred
                        // on the character's full width, so a double-width ideograph
                        // is marked once over its middle rather than once per cell.
                        //
                        // Square, so it reads as a point: the width comes from the
                        // same measure as the height instead of from the cell, which
                        // is what separates a dot from a dash at every size.
                        //
                        // In the text colour rather than the underline colour. Emphasis
                        // marking is a property of the character the way italics are,
                        // so it takes the character's colour — which is what CSS
                        // means by defaulting text-emphasis-color to currentColor.
                        // SGR 58 sets the colour of an UNDERLINE, and a reader who
                        // recolours their underlines has said nothing about how their
                        // emphasis should look. The strike above draws in the
                        // foreground for the same reason.
                        //
                        // Seated at the very top of the cell, which is as much
                        // clearance as a cell can offer. An ideographic glyph fills
                        // its em, so at zero leading there is no empty band above it
                        // and the mark touches. A Japanese compositor answers that by
                        // opening the line height and setting the mark in the leading
                        // rather than by squeezing it into the em. The line spacing
                        // is split above and below the glyph, so the band this sits
                        // in is that leading, and the mark lifts clear on its own as
                        // the line height grows.
                        const float side = std::max(2.0f * metrics.deviceScale, std::round(metrics.cellWidth * 0.25f));
                        const float centreX = std::round((cellLeft + cellRight) * 0.5f);
                        const float top = std::round(cellTop);
                        emitCellRule(DecorationSite::IdeogramStress, centreX - side * 0.5f, centreX + side * 0.5f, cellTop, metrics.cellHeight, top, top + side, foreground, DecorationKind::Solid, side, false, false, inkTopLeft, inkBottomRight);
                    }
                }
            }
        }
    };
    const bool hasLinkCellsInFrame = in.linkCells != nullptr;
    // The link constant is the quiet walk's alone; the other two read the flag.
    switch (walkClass) {
    case WalkClass::Quiet:
        if (in.subpixelGlyphs && hasLinkCellsInFrame)
            walkPasses(WalkClassConstant<WalkClass::Quiet> {}, std::true_type {}, std::true_type {});
        else if (in.subpixelGlyphs)
            walkPasses(WalkClassConstant<WalkClass::Quiet> {}, std::true_type {}, std::false_type {});
        else if (hasLinkCellsInFrame)
            walkPasses(WalkClassConstant<WalkClass::Quiet> {}, std::false_type {}, std::true_type {});
        else
            walkPasses(WalkClassConstant<WalkClass::Quiet> {}, std::false_type {}, std::false_type {});
        break;
    case WalkClass::Selected:
        if (in.subpixelGlyphs)
            walkPasses(WalkClassConstant<WalkClass::Selected> {}, std::true_type {}, std::false_type {});
        else
            walkPasses(WalkClassConstant<WalkClass::Selected> {}, std::false_type {}, std::false_type {});
        break;
    case WalkClass::General:
        if (in.subpixelGlyphs)
            walkPasses(WalkClassConstant<WalkClass::General> {}, std::true_type {}, std::false_type {});
        else
            walkPasses(WalkClassConstant<WalkClass::General> {}, std::false_type {}, std::false_type {});
        break;
    }

    // The tier's tails, per stream, for the backends that draw the heads into
    // a retained grid and the tails after it. Zero when no tier was walked.
    if (didWalkTier) {
        counts.cursorTier.cells = counts.cells - countsBeforeTier.cells;
        counts.cursorTier.glyphs = counts.glyphs - countsBeforeTier.glyphs;
        counts.cursorTier.wideGlyphs = counts.wideGlyphs - countsBeforeTier.wideGlyphs;
        counts.cursorTier.proportionalGlyphs = counts.proportionalGlyphs - countsBeforeTier.proportionalGlyphs;
        counts.cursorTier.proportionalStrips = counts.proportionalStrips - countsBeforeTier.proportionalStrips;
        counts.cursorTier.colorGlyphs = counts.colorGlyphs - countsBeforeTier.colorGlyphs;
        counts.cursorTier.ligatureGlyphs = counts.ligatureGlyphs - countsBeforeTier.ligatureGlyphs;
        counts.cursorTier.doubleGlyphs = counts.doubleGlyphs - countsBeforeTier.doubleGlyphs;
        counts.cursorTier.decorations = counts.decorations - countsBeforeTier.decorations;
        counts.cursorTier.boxFills = counts.boxFills - countsBeforeTier.boxFills;
    }

    if (!rowInkCrossings.empty())
        markRowInkCrossingsOver(in.originInPixels.y, metrics.cellHeight, in.rows, &mono, out, counts, rowInkCrossings);

    // The overlay stream: membership and order are the assembly's, stated once
    // in Rendering/OverlayAssembly.h as the reason-chain. Restating either here
    // lets this arm and another disagree about whether the trigger tint draws
    // over the hilite band.
    {
        OverlayAssemblyInputs overlayInputs;
        overlayInputs.grid = { in.columns, in.rows };
        overlayInputs.cellSizeInPixels = { metrics.cellWidth, metrics.cellHeight };
        overlayInputs.originInPixels = { in.originInPixels.x, in.originInPixels.y };
        overlayInputs.viewportSizeInPixels = { in.viewportSizeInPixels.x, in.viewportSizeInPixels.y };
        overlayInputs.lineAttributes = in.lineAttributes;
        overlayInputs.deviceScale = metrics.deviceScale;
        overlayInputs.isBitmapFace = in.isBitmapFace;
        overlayInputs.bellFlashAlpha = in.bellFlashAlpha;
        overlayInputs.isColumns132 = in.isColumns132;
        overlayInputs.defaultForeground = in.screenReverse ? in.palette->defaultBackground : in.palette->defaultForeground;
        overlayInputs.otherSearchMatchSpans = { in.otherSearchMatchSpans, static_cast<std::size_t>(in.otherSearchMatchSpans ? in.otherSearchMatchSpanCount : 0) };
        overlayInputs.searchMatchSpans = { in.searchMatchSpans, static_cast<std::size_t>(in.searchMatchSpans ? in.searchMatchSpanCount : 0) };
        overlayInputs.searchMatchHighlight = in.searchMatchHighlight;
        overlayInputs.focusedMatchAgeSeconds = in.focusedMatchAgeSeconds;
        overlayInputs.hiliteRegionSpans = { in.hiliteRegionSpans, static_cast<std::size_t>(in.hiliteRegionSpans ? in.hiliteRegionSpanCount : 0) };
        overlayInputs.triggerHighlightSpans = { in.triggerHighlightSpans, static_cast<std::size_t>(in.triggerHighlightSpans ? in.triggerHighlightSpanCount : 0) };
        // A selection behind the text is already in the cells; a quad over it
        // would cover the glyphs with an opaque fill.
        overlayInputs.selectionSpans = { in.selectionSpans,
            static_cast<std::size_t>(in.selectionSpans && !in.isSelectionBehindText ? in.selectionSpanCount : 0) };
        overlayInputs.selectionColor = in.selectionColor;
        overlayInputs.cursor = in.cursor;
        // The rows laid out as letters, so the cursor, the selection and every
        // highlight stand where the letters do rather than at the pitch.
        overlayInputs.letters = frameLetters;
        overlayInputs.letterRows = frameLetterRows;
        // A cursor inside a claim is handed over as the claim: the anchor's
        // column and the claimed width, so the bar seats at the glyph's left
        // edge and the underline and hollow block span the glyph.
        if (cursorClaimAnchorColumn >= 0) {
            overlayInputs.cursor.column = cursorClaimAnchorColumn;
            overlayInputs.cursor.widthCells = cursorClaimWidthCells;
        }
        overlayInputs.hasStatusLineOnBottomRow = in.hasStatusLineOnBottomRow;
        overlayInputs.cellHeightPoints = in.cellHeightPoints;
        overlayInputs.lineSpacingPoints = in.lineSpacingPoints;
        overlayInputs.toolkitCaret = in.toolkitCaret;
        overlayInputs.promptMarks = in.promptMarks;
        overlayInputs.shouldDrawPasswordLock = in.shouldDrawPasswordLock;
        overlayInputs.isPasswordPromptActive = in.isPasswordPromptActive;
        overlayInputs.palette = in.palette;
        const std::size_t already = counts.overlays;
        const std::size_t room = out.overlayCapacity > already ? out.overlayCapacity - already : 0;
        counts.overlays += static_cast<std::size_t>(assembleOverlays(overlayInputs,
            { out.overlays ? out.overlays + already : nullptr, out.overlays ? room : 0 }));
    }

    return counts;
}

} // namespace Ubiquitty::Rendering
