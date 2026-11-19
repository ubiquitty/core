// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Unifont's combining marks stacked clear of one another: every mark of a
// tall stack drawn at its full size, a pixel of the face's grid apart, inside
// the windows Core cuts the stack into, and every cluster whose marks already
// sit clear drawn exactly as the overprint drew it.

#include "Core/Rendering/ClusterWindows.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

/// A slot the bitmap face draws a cluster into.
struct BitmapSlot {
    /// The face's integer upscale.
    int scale = 1;
    /// The slot's width: one cell.
    int width = 8;
    /// The slot's height: the cell, its line spacing included.
    int height = 16;
};

/// The slots every case draws in: the native cell, the portable tier's
/// upscaled cell with four rows of line spacing, and a cell three times the
/// native one.
constexpr std::array<BitmapSlot, 3> bitmapSlots { BitmapSlot { 1, 8, 16 }, BitmapSlot { 2, 16, 36 }, BitmapSlot { 3, 24, 48 } };

/// A Z under six marks above: acute, circumflex, tilde, macron, breve and hook.
constexpr std::array<std::uint32_t, 7> stackAbove { 'Z', 0x301, 0x302, 0x303, 0x304, 0x306, 0x309 };

/// A g over six marks below.
constexpr std::array<std::uint32_t, 7> stackBelow { 'g', 0x316, 0x317, 0x318, 0x319, 0x31C, 0x31D };

/// A run of inked rows.
struct InkBand {
    /// The first inked row.
    int top = 0;
    /// One past the last inked row.
    int bottom = 0;
};

/// The runs of inked rows in a plane, top to bottom.
///
/// - Parameters:
///   - plane: The coverage, row-major.
///   - width: The plane's width.
/// - Returns: Every maximal run of rows holding ink.
std::vector<InkBand> inkBands(const std::vector<std::uint8_t>& plane, int width)
{
    std::vector<InkBand> bands;
    const int height = static_cast<int>(plane.size()) / width;
    for (int row = 0; row < height; ++row) {
        const auto start = plane.begin() + static_cast<std::ptrdiff_t>(row) * width;
        if (!std::any_of(start, start + width, [](std::uint8_t texel) { return texel != 0; }))
            continue;
        if (!bands.empty() && bands.back().bottom == row)
            bands.back().bottom = row + 1;
        else
            bands.push_back({ row, row + 1 });
    }
    return bands;
}

/// Draws a cluster uncut, through one window `reach` rows past each edge of its slot.
///
/// - Parameters:
///   - rasterizer: The face.
///   - cluster: The cluster's code points.
///   - slot: The slot the cluster is drawn into.
///   - reach: How many rows past each edge the window shows.
/// - Returns: The window, `slot.width` wide and `slot.height + 2 * reach` tall.
std::vector<std::uint8_t> drawUncut(UnifontRasterizer& rasterizer, std::span<const std::uint32_t> cluster, BitmapSlot slot, int reach)
{
    std::vector<std::uint8_t> window(static_cast<std::size_t>(slot.width) * static_cast<std::size_t>(slot.height + 2 * reach), 0xAB);
    CHECK(rasterizer.rasteriseClusterWindow(cluster, slot.width, slot.height, -reach, slot.height + 2 * reach, window));
    return window;
}

/// The rows one glyph inks when drawn alone into a slot.
///
/// - Parameters:
///   - rasterizer: The face.
///   - codePoint: The glyph's code point.
///   - slot: The slot.
/// - Returns: How many rows hold its ink.
int inkedRowCount(UnifontRasterizer& rasterizer, std::uint32_t codePoint, BitmapSlot slot)
{
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(slot.width) * static_cast<std::size_t>(slot.height));
    CHECK(rasterizer.rasterizeGlyph(codePoint, FontFace::Regular, 0, slot.width, slot.height, plane.data()));
    const std::vector<InkBand> bands = inkBands(plane, slot.width);
    CHECK(!bands.empty());
    return bands.empty() ? 0 : bands.back().bottom - bands.front().top;
}

} // namespace

// Six marks over a Z draw as six marks, each at the height Unifont draws it
// alone, stacked outward in the order they were typed, the nearest at its own
// place and every one past it a single pixel of the face's grid clear of the
// one beneath it. The Z stays where it draws alone.
TEST(sixMarksAboveALetterDrawAsSixSeparatedMarks)
{
    for (const BitmapSlot slot : bitmapSlots) {
        UnifontRasterizer rasterizer(slot.scale);
        const int reach = clusterWindowReach(slot.height);
        const std::vector<InkBand> bands = inkBands(drawUncut(rasterizer, stackAbove, slot, reach), slot.width);
        REQUIRE_MSG(bands.size() == stackAbove.size(), "the letter and six separate marks");
        const InkBand letter = bands.back();
        const std::vector<InkBand> alone = inkBands(drawUncut(rasterizer, std::span(stackAbove).first(1), slot, reach), slot.width);
        REQUIRE(alone.size() == 1);
        CHECK_MSG(letter.top == alone.front().top && letter.bottom == alone.front().bottom, "the letter keeps its place");
        for (std::size_t mark = 1; mark < stackAbove.size(); ++mark) {
            // The marks run outward from the letter: the first typed is the
            // band just above it.
            const InkBand band = bands[bands.size() - 1 - mark];
            const InkBand beneath = bands[bands.size() - mark];
            CHECK_MSG(band.bottom - band.top == inkedRowCount(rasterizer, stackAbove[mark], slot), "each mark keeps its full height");
            if (mark > 1)
                CHECK_MSG(beneath.top - band.bottom == slot.scale, "a moved mark stands one pixel of the face's grid clear");
        }
        CHECK_MSG(bands.front().top < reach, "the stack rises past the slot's top");
    }
}

// Six marks under a g draw as six, each at its own height and a pixel of the
// grid below the last, starting a pixel under the g's descender.
TEST(sixMarksBelowALetterDrawAsSixSeparatedMarks)
{
    for (const BitmapSlot slot : bitmapSlots) {
        UnifontRasterizer rasterizer(slot.scale);
        const int reach = clusterWindowReach(slot.height);
        const std::vector<InkBand> bands = inkBands(drawUncut(rasterizer, stackBelow, slot, reach), slot.width);
        REQUIRE_MSG(bands.size() == stackBelow.size(), "the letter and six separate marks");
        for (std::size_t mark = 1; mark < stackBelow.size(); ++mark) {
            CHECK_MSG(bands[mark].bottom - bands[mark].top == inkedRowCount(rasterizer, stackBelow[mark], slot), "each mark keeps its full height");
            CHECK_MSG(bands[mark].top - bands[mark - 1].bottom == slot.scale, "a pixel of the face's grid clear of the ink above it");
        }
        CHECK_MSG(bands.back().bottom > reach + slot.height, "the stack reaches past the slot's bottom");
    }
}

// A stack's ink lies whole inside the windows Core cuts for it, and the
// windows tile one drawing: the slot holds that drawing's rows, and every
// window the rows its offset names, byte for byte. Nothing of the stack is
// cut and nothing is resampled.
TEST(aStackStaysInsideTheWindowsItTakes)
{
    for (const BitmapSlot slot : bitmapSlots) {
        UnifontRasterizer rasterizer(slot.scale);
        const int reach = clusterWindowReach(slot.height);
        const int pitch = clusterWindowPitch(slot.height);
        const std::size_t rowBytes = static_cast<std::size_t>(slot.width);
        for (const auto& cluster : { std::span<const std::uint32_t>(stackAbove), std::span<const std::uint32_t>(stackBelow) }) {
            const std::vector<std::uint8_t> uncut = drawUncut(rasterizer, cluster, slot, reach);
            const ClusterInkRows ink = rasterizer.measureClusterInk(cluster, slot.width, slot.height, reach);
            REQUIRE(ink.isInked);
            const ClusterWindowCounts counts = clusterWindowCounts(ink, ClusterOwnQuad::CellBox, slot.height, 0);
            const ClusterSlotRows beside = clusterOwnRowsBesideWindows(ClusterOwnQuad::CellBox, slot.height, 0);
            CHECK(counts.above <= clusterWindowLimitPerSide && counts.below <= clusterWindowLimitPerSide);
            CHECK_MSG(ink.topInPixels >= beside.top - counts.above * pitch, "the windows above hold the stack's top");
            CHECK_MSG(ink.bottomInPixels <= beside.bottom + counts.below * pitch, "the windows below hold its bottom");
            // The rows the slot and its windows draw, laid back into one plane.
            std::vector<std::uint8_t> tiled(uncut.size(), 0);
            std::vector<std::uint8_t> own(rowBytes * static_cast<std::size_t>(slot.height));
            REQUIRE(rasterizer.rasterizeCluster(cluster.data(), static_cast<int>(cluster.size()), FontFace::Regular, 0, slot.width, slot.height, own.data()));
            std::ranges::copy(own, tiled.begin() + static_cast<std::ptrdiff_t>(reach * rowBytes));
            const ClusterSlotRows drawn = clusterWindowRows(slot.height);
            const auto tile = [&](ClusterWindowSide side, int index) {
                const int offset = clusterWindowRowOffset(beside, slot.height, side, index);
                std::vector<std::uint8_t> window(rowBytes * static_cast<std::size_t>(slot.height));
                REQUIRE(rasterizer.rasterizeClusterWindow(cluster.data(), static_cast<int>(cluster.size()), FontFace::Regular, 0, 0.0, {}, slot.width,
                    slot.height, offset, slot.height, window.data()));
                for (int row = drawn.top; row < drawn.bottom; ++row) {
                    const int tallRow = offset + row + reach;
                    if (tallRow < 0 || tallRow >= slot.height + 2 * reach)
                        continue;
                    std::copy_n(window.begin() + static_cast<std::ptrdiff_t>(row * rowBytes), rowBytes,
                        tiled.begin() + static_cast<std::ptrdiff_t>(tallRow * rowBytes));
                }
            };
            for (int index = 1; index <= counts.above; ++index)
                tile(ClusterWindowSide::Above, index);
            for (int index = 1; index <= counts.below; ++index)
                tile(ClusterWindowSide::Below, index);
            CHECK_MSG(tiled == uncut, "the slot and its windows draw the whole stack, as one drawing");
        }
    }
}

// A mark that would land on another stacked mark moves clear of it even when
// the two touch without overlapping: an e with a circumflex takes its macron
// above the circumflex, a pixel apart, where Unifont alone draws the macron
// on the circumflex's row.
TEST(aMarkOnAStackedMarkMovesAPixelClearOfIt)
{
    const BitmapSlot slot = bitmapSlots[1];
    UnifontRasterizer rasterizer(slot.scale);
    constexpr std::array<std::uint32_t, 2> marked { 0xEA, 0x304 };
    const std::vector<InkBand> bands = inkBands(drawUncut(rasterizer, marked, slot, 0), slot.width);
    REQUIRE(bands.size() == 3);
    CHECK_MSG(bands[1].top - bands[0].bottom == slot.scale, "the macron stands a pixel above the circumflex");
}

// A cluster whose marks already sit clear of their base draws byte for byte
// what the overprint drew, at every scale and line spacing: the accented
// letters of running text never move. So does an attached mark, which Unifont
// draws touching its letter, an overlay, which crosses it, and every mark on a
// two-cell base, which the wide sheet gives no windows. A precomposed letter is
// one glyph, drawn as it always was.
TEST(aClusterWhoseMarksSitClearDrawsExactlyTheOverprint)
{
    const std::vector<std::vector<std::uint32_t>> clusters {
        { 'e', 0x301 }, // é
        { 'a', 0x308 }, // ä
        { 'n', 0x303 }, // ñ
        { 'o', 0x302, 0x323 }, // ộ
        { 'E', 0x301 }, // É
        { 0xE0, 0x301 }, // à́: the acute touches the grave Unifont draws on the à
        { 'o', 0x332, 0x305 }, // o̲̅
        { 'c', 0x327 }, // ç: attached below
        { 'o', 0x338 }, // o̸: an overlay
        { 0x3042, 0x301 }, // あ́: a two-cell base
        { 0xE9 }, // é, precomposed
    };
    for (const BitmapSlot slot : bitmapSlots) {
        UnifontRasterizer rasterizer(slot.scale);
        for (const std::vector<std::uint32_t>& cluster : clusters) {
            // A two-cell base draws into the wide sheet's two-cell slot.
            const int width = cluster.front() == 0x3042 ? 2 * slot.width : slot.width;
            const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(slot.height);
            std::vector<std::uint8_t> stacked(bytes, 0xAB);
            std::vector<std::uint8_t> overprinted(bytes, 0xCD);
            const int count = static_cast<int>(cluster.size());
            CHECK(rasterizer.rasterizeCluster(cluster.data(), count, FontFace::Regular, 0, width, slot.height, stacked.data()));
            CHECK(rasterizer.GlyphRasterizer::rasterizeCluster(cluster.data(), count, FontFace::Regular, 0, width, slot.height, overprinted.data()));
            CHECK_MSG(stacked == overprinted, "a cluster with nothing to stack draws exactly the overprint");
        }
    }
}

UBIQUITTY_TEST_MAIN()
