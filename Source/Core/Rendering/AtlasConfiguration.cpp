// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/AtlasConfiguration.h"

#include <bit>
#include <cmath>
#include <string_view>
#include <utility>

namespace Ubiquitty::Rendering {
namespace {

class ConfigurationEncoder {
public:
    ConfigurationEncoder()
    {
        m_key.reserve(1024);
    }

    void append(std::uint64_t value)
    {
        constexpr char digits[] = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) {
            m_key += digits[(value >> shift) & 15];
        }
    }

    void append(int value) { append(static_cast<std::uint64_t>(value)); }
    void append(float value) { append(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value))); }
    void append(double value) { append(std::bit_cast<std::uint64_t>(value)); }

    void append(std::string_view value)
    {
        append(static_cast<std::uint64_t>(value.size()));
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned char byte : value) {
            m_key += digits[byte >> 4];
            m_key += digits[byte & 15];
        }
    }

    void append(const BoxDrawing::BoxStrokeBand& band)
    {
        append(static_cast<std::uint64_t>(band.isValid));
        append(band.centerInCell);
        append(band.thicknessInCell);
    }

    void append(const BoxDrawing::BoxDoubleBand& band)
    {
        append(static_cast<std::uint64_t>(band.isValid));
        append(band.centerInCell);
        append(band.railThicknessInCell);
        append(band.gapInCell);
    }

    std::string takeKey() { return std::move(m_key); }

private:
    std::string m_key;
};

bool isFiniteNonnegative(float value)
{
    return std::isfinite(value) && value >= 0;
}

bool isUnitValue(float value)
{
    return isFiniteNonnegative(value) && value <= 1;
}

bool isValidBand(const BoxDrawing::BoxStrokeBand& band)
{
    return isUnitValue(band.centerInCell) && isUnitValue(band.thicknessInCell)
        && (!band.isValid || band.thicknessInCell > 0);
}

bool isValidBand(const BoxDrawing::BoxDoubleBand& band)
{
    return isUnitValue(band.centerInCell) && isUnitValue(band.railThicknessInCell)
        && isUnitValue(band.gapInCell) && (!band.isValid || band.railThicknessInCell > 0);
}

bool isValidOptionalExtent(int width, int height)
{
    return (!width && !height) || (width > 0 && height > 0);
}

bool isValidGeometry(const GlyphSlotGeometry& geometry)
{
    const auto& profile = geometry.boxStrokeProfile;
    return geometry.columns > 0 && geometry.slotWidthInPixels > 0 && geometry.slotHeightInPixels > 0
        && geometry.glyphWidthInPixels > 0 && geometry.glyphWidthInPixels <= geometry.slotWidthInPixels
        && geometry.glyphMarginInPixels.width >= 0 && geometry.glyphMarginInPixels.width <= geometry.slotWidthInPixels
        && geometry.glyphMarginInPixels.height >= 0 && geometry.glyphMarginInPixels.height <= geometry.slotHeightInPixels
        && isFiniteNonnegative(geometry.referenceAdvanceInPixels) && isFiniteNonnegative(geometry.referenceLineHeightInPixels)
        && isValidOptionalExtent(geometry.wideSlotWidthInPixels, geometry.wideSlotHeightInPixels)
        && isValidOptionalExtent(geometry.doubleSlotWidthInPixels, geometry.doubleSlotHeightInPixels)
        && isValidOptionalExtent(geometry.ligatureSlotWidthInPixels, geometry.ligatureSlotHeightInPixels)
        && (geometry.ligatureParticipation == LigatureParticipation::OperatorRuns || geometry.ligatureParticipation == LigatureParticipation::AllEligibleRuns)
        && isValidBand(profile.lightHorizontal) && isValidBand(profile.lightVertical)
        && isValidBand(profile.heavyHorizontal) && isValidBand(profile.heavyVertical)
        && isValidBand(profile.doubleHorizontal) && isValidBand(profile.doubleVertical);
}

}

std::string AtlasConfiguration::sharingKey() const
{
    if (!resourceDomain || familyName.empty() || !std::isfinite(pointSize) || pointSize <= 0
        || !std::isfinite(pixelsPerPoint) || pixelsPerPoint <= 0 || slotCapacity <= 0 || unifontScale < 0
        || !std::isfinite(baselinePenInPixels) || !std::isfinite(wideGlyphBoostLimit) || !std::isfinite(fontWidthPercent)
        || !std::isfinite(proportionalPointSize)
        || !isValidGeometry(geometry)) {
        return {};
    }

    ConfigurationEncoder encoder;
    encoder.append(resourceDomain);
    encoder.append(familyName);
    encoder.append(pointSize);
    encoder.append(pixelsPerPoint);
    encoder.append(geometry.columns);
    encoder.append(geometry.slotWidthInPixels);
    encoder.append(geometry.slotHeightInPixels);
    encoder.append(geometry.glyphWidthInPixels);
    encoder.append(geometry.glyphMarginInPixels.width);
    encoder.append(geometry.glyphMarginInPixels.height);
    encoder.append(geometry.referenceAdvanceInPixels);
    encoder.append(geometry.referenceLineHeightInPixels);
    encoder.append(geometry.wideSlotWidthInPixels);
    encoder.append(geometry.wideSlotHeightInPixels);
    encoder.append(geometry.doubleSlotWidthInPixels);
    encoder.append(geometry.doubleSlotHeightInPixels);
    encoder.append(geometry.ligatureSlotWidthInPixels);
    encoder.append(geometry.ligatureSlotHeightInPixels);
    encoder.append(static_cast<int>(geometry.ligatureParticipation));
    const auto& profile = geometry.boxStrokeProfile;
    encoder.append(profile.lightHorizontal);
    encoder.append(profile.lightVertical);
    encoder.append(profile.heavyHorizontal);
    encoder.append(profile.heavyVertical);
    encoder.append(profile.doubleHorizontal);
    encoder.append(profile.doubleVertical);
    encoder.append(slotCapacity);
    encoder.append(baselinePenInPixels);
    encoder.append(wideGlyphBoostLimit);
    encoder.append(unifontScale);
    encoder.append(fontWeight);
    encoder.append(boldFontWeight);
    encoder.append(fontWidthPercent);
    encoder.append(proportionalFamilyName);
    encoder.append(proportionalPointSize);
    encoder.append(cjkFallbackFamily);
    encoder.append(cjkFallbackLanguage);
    encoder.append(symbolFamilyName);
    encoder.append(symbolFontPath);
    encoder.append(ligatureFeatureSpecification);
    encoder.append(userFeatureSpecification);
    encoder.append(userVariationSpecification);
    encoder.append(static_cast<std::uint64_t>(doesFontLigate));
    encoder.append(static_cast<std::uint64_t>(wantsLigatures));
    return encoder.takeKey();
}

}
