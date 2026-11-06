// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/LigatureSheet.h"
#include "Core/Rendering/ProportionalStripRaster.h"

#include <array>
#include <cmath>
#include <vector>

using namespace Ubiquitty::Rendering;
using Ubiquitty::ProportionalShapedGlyph;

namespace {

class RecordingRasterizer final : public GlyphRasterizer {
public:
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics;
        metrics.cellWidth = 8.0f;
        metrics.cellHeight = 1.0f;
        return metrics;
    }

    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return false; }

    int shapeProportionalRun(ClusterSpan codePoints, FontFace, std::span<ProportionalShapedGlyph> glyphs) override
    {
        if (glyphs.size() < codePoints.size()) {
            return 0;
        }
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphs[index] = {};
            glyphs[index].glyph = codePoints[index];
            glyphs[index].cluster = static_cast<std::uint16_t>(index);
            glyphs[index].advanceInPixels = 4.0f;
            glyphs[index].offsetXInPixels = m_glyphOffset;
        }
        return static_cast<int>(codePoints.size());
    }

    bool rasteriseProportionalRun(std::span<const ProportionalShapedGlyph> glyphs, FontFace,
        int width, int, std::span<std::uint8_t> coverage) override
    {
        m_origins.clear();
        double pen = 0.0;
        for (const ProportionalShapedGlyph& glyph : glyphs) {
            const double origin = pen + glyph.offsetXInPixels;
            m_origins.push_back(origin);
            pen += glyph.advanceInPixels;
            const int pixel = static_cast<int>(std::floor(origin));
            if (glyph.glyph != ' ' && pixel >= 0 && pixel < width) {
                coverage[static_cast<std::size_t>(pixel)] = 255;
            }
        }
        return true;
    }

    // A master marks each glyph's column with its canvas row plus one, so a
    // slot shows which slot height of the master it was cut from.
    bool rasteriseSizedProportionalRun(std::span<const ProportionalShapedGlyph> glyphs, FontFace, const SizedMaster& master,
        int width, int height, std::span<std::uint8_t> coverage) override
    {
        m_masterScale = master.scale.whole();
        m_masterWidthScale = master.widthScale;
        m_masterHeight = height;
        m_origins.clear();
        for (const ProportionalShapedGlyph& glyph : glyphs) {
            m_origins.push_back(glyph.offsetXInPixels);
            const int pixel = static_cast<int>(std::floor(glyph.offsetXInPixels));
            for (int y = 0; y < height && pixel >= 0 && pixel < width; ++y)
                coverage[static_cast<std::size_t>(y) * width + pixel] = static_cast<std::uint8_t>(y + 1);
        }
        return true;
    }

    void setGlyphOffset(float offset) { m_glyphOffset = offset; }
    [[nodiscard]] const std::vector<double>& origins() const { return m_origins; }
    [[nodiscard]] int masterScale() const { return m_masterScale; }
    [[nodiscard]] int masterWidthScale() const { return m_masterWidthScale; }
    [[nodiscard]] int masterHeight() const { return m_masterHeight; }

private:
    float m_glyphOffset = 0.0f;
    std::vector<double> m_origins;
    int m_masterScale = 0;
    int m_masterWidthScale = 0;
    int m_masterHeight = 0;
};

ProportionalStripRequest requestForAlignment(std::uint8_t alignment)
{
    ProportionalStripRequest request;
    request.widthCells = 4;
    request.horizontalAlign = alignment;
    request.cellAdvanceInPixels = 8.0f;
    request.slotWidthInPixels = 40;
    request.slotHeightInPixels = 1;
    return request;
}

} // namespace

TEST(justifiedStripOpensEachGapOnceAndKeepsTheFinalWord)
{
    RecordingRasterizer rasterizer;
    const std::array<std::uint32_t, 5> text { 'a', ' ', 'b', ' ', 'c' };
    const ProportionalStripRequest request = requestForAlignment(3);
    std::vector<ProportionalShapedGlyph> shaped;
    const ProportionalStripShape shape = shapeProportionalStrip(rasterizer, text, request, shaped);
    CHECK(shape.glyphCount == 5);
    std::vector<std::uint8_t> coverage(40, 17);
    CHECK(rasteriseProportionalStrip(rasterizer, shaped, request, shape.layout, coverage));
    const std::array<double, 5> expected { 0.0, 4.0, 14.0, 18.0, 28.0 };
    CHECK(rasterizer.origins().size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(rasterizer.origins()[index] - shape.layout.rasterInsetInPixels == expected[index]);
    }
    const int inset = static_cast<int>(shape.layout.rasterInsetInPixels);
    CHECK(coverage[inset] == 255);
    CHECK(coverage[inset + 14] == 255);
    CHECK(coverage[inset + 28] == 255);
    CHECK(coverage[32] == 0);
    CHECK(coverage[39] == 0);
}

// A strip at a size is the run's layout taken that many times across, drawn
// from a master that many times the size on a canvas as many slots tall, of
// which the slot is the band's slot height: the strip one texel a pixel on
// the screen at the run's size, where a stretch would resample the run drawn
// one row tall.
TEST(aStripAtAScaleIsTheRunAtThatSizeCutAtTheBandsSlotHeight)
{
    RecordingRasterizer rasterizer;
    const std::array<std::uint32_t, 2> text { 'a', 'b' };
    ProportionalStripRequest request = requestForAlignment(0);
    request.slotHeightInPixels = 3;
    request.scale = 2;
    request.band = 1;
    std::vector<ProportionalShapedGlyph> shaped;
    const ProportionalStripShape shape = shapeProportionalStrip(rasterizer, text, request, shaped);
    REQUIRE(shape.glyphCount == 2);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(40 * 3), 0);
    CHECK(rasteriseProportionalStrip(rasterizer, shaped, request, shape.layout, coverage));
    CHECK_EQ(rasterizer.masterScale(), 2);
    CHECK_EQ(rasterizer.masterWidthScale(), 1);
    CHECK_MSG(rasterizer.masterHeight() == 6, "a canvas two slots tall");
    REQUIRE(rasterizer.origins().size() == 2);
    const double inset = shape.layout.rasterInsetInPixels;
    CHECK_MSG(rasterizer.origins()[0] - inset == 0.0 && rasterizer.origins()[1] - inset == 8.0, "the layout's four-pixel advance taken twice");
    const int column = static_cast<int>(std::floor(rasterizer.origins()[1]));
    for (int y = 0; y < 3; ++y)
        CHECK_MSG(coverage[static_cast<std::size_t>(y) * 40 + static_cast<std::size_t>(column)] == 4 + y, "the slot is the canvas's second slot height");
    // The shape at its own size is the same text at its own width, and keys apart.
    ProportionalStripRequest unscaled = requestForAlignment(0);
    unscaled.slotHeightInPixels = 3;
    CHECK(shape.layout.usedWidthInSlotPixels > shapeProportionalStrip(rasterizer, text, unscaled, shaped).layout.usedWidthInSlotPixels);
}

TEST(stripOriginsMatchCoreLayoutForEveryAlignmentAndFractionalPen)
{
    RecordingRasterizer rasterizer;
    rasterizer.setGlyphOffset(0.25f);
    const std::array<std::uint32_t, 7> text { ' ', 'a', ' ', 'b', ' ', 'c', ' ' };
    for (std::uint8_t alignment = 0; alignment <= 3; ++alignment) {
        ProportionalStripRequest request = requestForAlignment(alignment);
        request.marginXInPixels = 2.0f;
        request.penResidualInPixels = 0.75f;
        std::vector<ProportionalShapedGlyph> original(text.size());
        CHECK(rasterizer.shapeProportionalRun(text, FontFace::Regular, original) == 7);
        std::vector<ProportionalGlyphPlacement> placements(text.size());
        const ProportionalRunLayout layout = layoutProportionalRun(original, text, request.widthCells,
            alignment, request.cellAdvanceInPixels, placements);
        CHECK(layout.glyphCount == 7);
        std::vector<ProportionalShapedGlyph> shaped;
        const ProportionalStripShape shape = shapeProportionalStrip(rasterizer, text, request, shaped);
        std::vector<std::uint8_t> coverage(40);
        CHECK(rasteriseProportionalStrip(rasterizer, shaped, request, shape.layout, coverage));
        for (std::size_t index = 0; index < placements.size(); ++index) {
            const double stripOrigin = shape.layout.quadLeftInPixels + rasterizer.origins()[index] - shape.layout.rasterInsetInPixels;
            CHECK(std::abs(stripOrigin - request.penResidualInPixels - placements[index].xInPixels) < 0.0001);
        }
    }
}

TEST(overfullStripClipsAtTheClaimedCells)
{
    RecordingRasterizer rasterizer;
    const std::array<std::uint32_t, 5> text { 'a', ' ', 'b', ' ', 'c' };
    ProportionalStripRequest request = requestForAlignment(3);
    request.widthCells = 2;
    request.marginXInPixels = 2.0f;
    std::vector<ProportionalShapedGlyph> shaped;
    const ProportionalStripShape shape = shapeProportionalStrip(rasterizer, text, request, shaped);
    std::vector<std::uint8_t> coverage(40, 17);
    CHECK(rasteriseProportionalStrip(rasterizer, shaped, request, shape.layout, coverage));
    const int inset = static_cast<int>(shape.layout.rasterInsetInPixels);
    CHECK(rasterizer.origins().back() - inset == 18.0);
    CHECK(coverage[inset + 2] == 255);
    CHECK(coverage[inset + 10] == 255);
    for (std::size_t index = static_cast<std::size_t>(inset + 18); index < coverage.size(); ++index) {
        CHECK(coverage[index] == 0);
    }
}

namespace {
class CountedUnifontRasterizer final : public GlyphRasterizer {
public:
    [[nodiscard]] CellMetrics cellMetrics() const override { return m_rasterizer.cellMetrics(); }
    [[nodiscard]] bool hasProportionalFace() const override { return true; }
    [[nodiscard]] int proportionalStripOverhangInPixels() const override { return m_rasterizer.proportionalStripOverhangInPixels(); }
    bool rasterizeGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, int width, int height, std::uint8_t* coverage) override
    {
        return m_rasterizer.rasterizeGlyph(codePoint, face, weight, width, height, coverage);
    }
    int shapeProportionalRun(ClusterSpan text, FontFace face, std::span<ProportionalShapedGlyph> shaped) override
    {
        ++m_shapes;
        return m_rasterizer.shapeProportionalRun(text, face, shaped);
    }
    bool rasteriseProportionalRun(std::span<const ProportionalShapedGlyph> glyphs, FontFace face,
        int width, int height, std::span<std::uint8_t> coverage) override
    {
        return m_rasterizer.rasteriseProportionalRun(glyphs, face, width, height, coverage);
    }
    [[nodiscard]] int shapes() const { return m_shapes; }

private:
    UnifontRasterizer m_rasterizer { 2, 3 };
    int m_shapes = 0;
};
}

TEST(longOverlappingRunsTileExactlyLikeAnUnlimitedStripAndShapeOnce)
{
    for (int widthCells : { 48, 1024 }) {
        for (std::uint8_t alignment : { 0, 3 }) {
            CountedUnifontRasterizer rasterizer;
            LigatureSheet sheet(16, 35, 0, 0, &rasterizer);
            CHECK(sheet.isUsable());
            const std::array<std::uint32_t, 6> pattern { 'W', 'W', 'i', ' ', 'j', ' ' };
            std::vector<std::uint32_t> text(static_cast<std::size_t>(widthCells));
            for (std::size_t index = 0; index < text.size(); ++index) {
                text[index] = pattern[index % pattern.size()];
            }
            text.back() = 'W';
            ProportionalStripRequest referenceRequest;
            referenceRequest.widthCells = widthCells;
            referenceRequest.horizontalAlign = alignment;
            referenceRequest.penResidualInPixels = 0.375f;
            referenceRequest.cellAdvanceInPixels = 16.0f;
            referenceRequest.slotWidthInPixels = widthCells * 16 + 64;
            referenceRequest.slotHeightInPixels = sheet.slotHeightInPixels();
            std::vector<ProportionalShapedGlyph> shaped;
            const auto referenceShape = shapeProportionalStrip(rasterizer, text, referenceRequest, shaped);
            CHECK(referenceShape.layout.tileCount == 1);
            const int expectedWidth = static_cast<int>(referenceShape.layout.usedWidthInSlotPixels);
            const int height = referenceRequest.slotHeightInPixels;
            std::vector<std::uint8_t> reference(static_cast<std::size_t>(referenceRequest.slotWidthInPixels) * height);
            CHECK(rasteriseProportionalStrip(rasterizer, shaped, referenceRequest, referenceShape.layout, reference));
            std::vector<std::uint8_t> tiled(static_cast<std::size_t>(expectedWidth) * height);
            const int shapesBefore = rasterizer.shapes();
            int tiles = 1;
            int coveredWidth = 0;
            for (int tile = 0; tile < tiles; ++tile) {
                const auto placement = sheet.proportionalStrip(text, widthCells, alignment, 0.375f, 16.0f,
                    FontFace::Regular, 0, tile);
                CHECK(placement.strip.present);
                tiles = placement.tileCount;
                const int offset = static_cast<int>(placement.leftInPixels - referenceShape.layout.quadLeftInPixels);
                CHECK(offset == coveredWidth);
                const int width = static_cast<int>(placement.widthInPixels);
                CHECK(width > 0 && width <= sheet.slotWidthInPixels() - 2);
                const int sourceX = static_cast<int>(placement.strip.minUV.x);
                const int sourceY = static_cast<int>(placement.strip.minUV.y);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        tiled[static_cast<std::size_t>(y) * expectedWidth + offset + x]
                            = sheet.coverage()[static_cast<std::size_t>(sourceY + y) * sheet.widthInPixels() + sourceX + x];
                    }
                }
                coveredWidth += width;
            }
            CHECK(tiles > 1);
            CHECK(coveredWidth == expectedWidth);
            CHECK(rasterizer.shapes() == shapesBefore + 1);
            const int referenceInset = static_cast<int>(referenceShape.layout.rasterInsetInPixels);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < expectedWidth; ++x) {
                    CHECK(tiled[static_cast<std::size_t>(y) * expectedWidth + x]
                        == reference[static_cast<std::size_t>(y) * referenceRequest.slotWidthInPixels + referenceInset + x]);
                }
            }
            bool hasTailInk = false;
            for (int y = 0; y < height; ++y) {
                for (int x = std::max(0, expectedWidth - 20); x < expectedWidth; ++x) {
                    hasTailInk |= tiled[static_cast<std::size_t>(y) * expectedWidth + x] != 0;
                }
            }
            CHECK(hasTailInk);
        }
    }
}

TEST(stripSlotsVerifyTheCompleteSourceAndEveryRasterIdentityAxis)
{
    GlyphSlotCache cache(32, 64, 8);
    cache.beginFrame();
    ProportionalStripVerification verification;
    std::vector<std::uint32_t> source(600, 'i');
    ProportionalStripRequest request;
    request.widthCells = 1024;
    ProportionalStripLayout layout {};
    layout.penResidualInPixels = 0.125;
    layout.tileCount = 4;
    const auto first = resolveProportionalStripSlot(cache, source, request, layout, 0, verification);
    CHECK(first.slot >= 0 && first.isFresh);
    CHECK(verification.size() == source.size() + 9);
    CHECK(verification[599] == 'i');
    const auto* storage = verification.data();
    const auto hit = resolveProportionalStripSlot(cache, source, request, layout, 0, verification);
    CHECK(hit.slot == first.slot && !hit.isFresh);
    CHECK(verification.data() == storage);

    ProportionalStripVerification collision = verification;
    collision[0] = 'W';
    bool isFreshCollision = false;
    const int collisionSlot = cache.slotForKeyVerified(first.key, collision.data(),
        static_cast<int>(collision.size()), &isFreshCollision);
    CHECK(collisionSlot >= 0 && collisionSlot != first.slot && isFreshCollision);
    const auto verifiedHit = resolveProportionalStripSlot(cache, source, request, layout, 0, verification);
    CHECK(verifiedHit.slot == first.slot && !verifiedHit.isFresh);

    for (int axis = 0; axis < 10; ++axis) {
        auto changedSource = source;
        auto changedRequest = request;
        auto changedLayout = layout;
        std::uint32_t variant = 0;
        switch (axis) {
        case 0:
            changedSource[599] = 'W';
            break;
        case 1:
            ++changedRequest.widthCells;
            break;
        case 2:
            changedRequest.horizontalAlign = 2;
            break;
        case 3:
            changedRequest.face = FontFace::Bold;
            break;
        case 4:
            changedLayout.penResidualInPixels = 0.375;
            break;
        case 5:
            changedRequest.tileIndex = 1;
            break;
        case 6:
            variant = makeRasterVariant(0, 1);
            break;
        case 7:
            changedRequest.widthScale = 2;
            break;
        case 8:
            changedRequest.scale = 2;
            break;
        case 9:
            changedRequest.scale = 2;
            changedRequest.band = 1;
            break;
        }
        const auto changed = resolveProportionalStripSlot(cache, changedSource, changedRequest, changedLayout, variant, verification);
        CHECK(changed.slot >= 0 && changed.slot != first.slot && changed.isFresh);
        const auto repeated = resolveProportionalStripSlot(cache, changedSource, changedRequest, changedLayout, variant, verification);
        CHECK(repeated.slot == changed.slot && !repeated.isFresh);
    }
    auto italicRequest = request;
    italicRequest.face = FontFace::Italic;
    const auto italic = resolveProportionalStripSlot(cache, source, italicRequest, layout, 0, verification);
    CHECK(italic.slot >= 0 && italic.slot != first.slot && italic.isFresh);

    request.tileIndex = layout.tileCount;
    CHECK(resolveProportionalStripSlot(cache, source, request, layout, 0, verification).slot < 0);
    request.tileIndex = 0;
    CHECK(resolveProportionalStripSlot(cache, {}, request, layout, 0, verification).slot < 0);
}

UBIQUITTY_TEST_MAIN()
