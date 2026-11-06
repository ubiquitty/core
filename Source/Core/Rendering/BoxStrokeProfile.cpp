// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/BoxStrokeProfile.h"

#include "Core/Rendering/GlyphRasterizer.h"

#include <vector>

namespace Ubiquitty::Rendering::BoxDrawing {

namespace {

// Half coverage: the antialiased skirt of a stroke says where ink fades, not
// where the rule is, and the body of a box rule is fully opaque in any face.
constexpr std::uint8_t inkThreshold = 128;

// Which way a reference bitmap is scanned: a horizontal rule's ink crosses
// the rows, a vertical rule's the columns.
enum class BandOrientation : std::uint8_t {
    Rows,
    Columns,
};

struct InkRun {
    int first = 0;
    int last = 0; // inclusive
    [[nodiscard]] float thickness() const { return static_cast<float>(last - first + 1); }
    [[nodiscard]] float center() const { return (static_cast<float>(first) + static_cast<float>(last) + 1.0f) * 0.5f; }
};

void scanBand(std::span<const std::uint8_t> coverage, int width, int height,
    BandOrientation orientation, BoxStrokeBand* single, BoxDoubleBand* doubled)
{
    if (width <= 0 || height <= 0 || coverage.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return;

    // The scanned axis runs across the stroke; the middle third of the other
    // axis is what gets sampled, clear of end caps and corner antialiasing.
    const int across = orientation == BandOrientation::Rows ? height : width;
    const int along = orientation == BandOrientation::Rows ? width : height;
    const int alongFirst = along / 3;
    const int alongLast = along - along / 3;
    if (alongLast <= alongFirst)
        return;
    const float acrossExtent = static_cast<float>(across);

    InkRun runs[3];
    int runCount = 0;
    bool isInRun = false;
    // The optical weight and where it sits, alongside the threshold extents:
    // the coverage integral over the scan and its centroid. The extents say
    // whether this is a single rule or a rail pair; the mass says how heavy
    // the rule LOOKS, which the extents overstate for an antialiased stroke —
    // both edge rows of a 1.5px rule can clear the threshold, and a fallback
    // that paints two full rows beside the font's soft rule reads thicker
    // than the table it completes.
    float massAcross = 0.0f;
    float weightedPositionSum = 0.0f;
    const float alongSamples = static_cast<float>(alongLast - alongFirst);
    for (int position = 0; position < across; ++position) {
        bool isInked = false;
        float coverageSum = 0.0f;
        for (int sample = alongFirst; sample < alongLast; ++sample) {
            const int x = orientation == BandOrientation::Rows ? sample : position;
            const int y = orientation == BandOrientation::Rows ? position : sample;
            const std::uint8_t pixel = coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            coverageSum += static_cast<float>(pixel);
            isInked = isInked || pixel >= inkThreshold;
        }
        const float meanCoverage = coverageSum / (alongSamples * 255.0f);
        massAcross += meanCoverage;
        weightedPositionSum += meanCoverage * (static_cast<float>(position) + 0.5f);
        if (isInked) {
            if (!isInRun) {
                if (runCount >= 3)
                    return; // more runs than any reference draws: not a rule
                runs[runCount].first = position;
                isInRun = true;
            }
            runs[runCount].last = position;
        } else if (isInRun) {
            ++runCount;
            isInRun = false;
        }
    }
    if (isInRun)
        ++runCount;

    if (runCount == 1 && single) {
        // The mass and centroid, not the run's extents: a hard-edged
        // reference reports the same numbers either way, and an antialiased
        // one reports the weight the eye sees. The whole scan feeds the
        // integral because a single rule's ink is the only ink there is —
        // the sub-threshold skirt rows the run test excludes still weigh.
        single->isValid = true;
        single->centerInCell = massAcross > 0.0f
            ? weightedPositionSum / massAcross / acrossExtent
            : runs[0].center() / acrossExtent;
        single->thicknessInCell = massAcross > 0.0f
            ? massAcross / acrossExtent
            : runs[0].thickness() / acrossExtent;
        return;
    }
    if (runCount == 2 && doubled) {
        // Rails of one double rule agree in thickness to within a pixel; two
        // runs further apart than that are some other glyph, and measuring
        // them as rails would mis-draw every healed junction.
        const float a = runs[0].thickness();
        const float b = runs[1].thickness();
        if (a > b + 1.0f || b > a + 1.0f)
            return;
        doubled->isValid = true;
        doubled->centerInCell = (runs[0].center() + runs[1].center()) * 0.5f / acrossExtent;
        doubled->railThicknessInCell = (a + b) * 0.5f / acrossExtent;
        doubled->gapInCell = static_cast<float>(runs[1].first - runs[0].last - 1) / acrossExtent;
    }
}

constexpr std::uint32_t referenceCodePoints[] = { 0x2500, 0x2502, 0x2501, 0x2503, 0x2550, 0x2551 };

} // namespace

std::span<const std::uint32_t> boxStrokeReferenceCodePoints()
{
    return referenceCodePoints;
}

void foldBoxReferenceScan(BoxStrokeProfile* profile, std::uint32_t codePoint,
    std::span<const std::uint8_t> coverage, int width, int height)
{
    if (!profile)
        return;
    switch (codePoint) {
    case 0x2500:
        scanBand(coverage, width, height, BandOrientation::Rows, &profile->lightHorizontal, nullptr);
        break;
    case 0x2502:
        scanBand(coverage, width, height, BandOrientation::Columns, &profile->lightVertical, nullptr);
        break;
    case 0x2501:
        scanBand(coverage, width, height, BandOrientation::Rows, &profile->heavyHorizontal, nullptr);
        break;
    case 0x2503:
        scanBand(coverage, width, height, BandOrientation::Columns, &profile->heavyVertical, nullptr);
        break;
    case 0x2550:
        scanBand(coverage, width, height, BandOrientation::Rows, nullptr, &profile->doubleHorizontal);
        break;
    case 0x2551:
        scanBand(coverage, width, height, BandOrientation::Columns, nullptr, &profile->doubleVertical);
        break;
    default:
        break;
    }
}

BoxStrokeProfile measureBoxStrokeProfile(GlyphRasterizer& rasterizer, int width, int height)
{
    BoxStrokeProfile profile;
    if (width <= 0 || height <= 0)
        return profile;
    std::vector<std::uint8_t> scratch(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (const std::uint32_t codePoint : boxStrokeReferenceCodePoints()) {
        if (!rasterizer.rasterizeGlyph(codePoint, FontFace::Regular, 0u, width, height, scratch.data()))
            continue;
        foldBoxReferenceScan(&profile, codePoint, scratch, width, height);
    }
    return profile;
}

} // namespace Ubiquitty::Rendering::BoxDrawing
