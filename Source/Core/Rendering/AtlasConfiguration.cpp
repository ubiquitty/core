// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/AtlasConfiguration.h"

#include <algorithm>
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

void appendTypography(ConfigurationEncoder& encoder, const RenderConfiguration& value)
{
    for (const auto* text : { &value.fontFamily, &value.proportionalFontFamily, &value.symbolFallbackFamily, &value.cjkFallbackFamily, &value.emojiFallbackFamily })
        encoder.append(text->string());
    encoder.append(value.fontSizePoints);
    encoder.append(value.proportionalFontSizePoints);
    encoder.append(value.fontFeatures.string());
    encoder.append(value.fontVariations.string());
    encoder.append(static_cast<int>(value.hasLigatures));
    encoder.append(value.fontWeight);
    encoder.append(value.boldFontWeight);
    encoder.append(value.fontWidthPercent);
    encoder.append(value.lineSpacingPoints);
    encoder.append(value.characterSpacingPoints);
    encoder.append(value.baselineOffsetPoints);
    encoder.append(static_cast<int>(value.shouldMatchFallbackSize));
    encoder.append(static_cast<int>(value.shouldBoostWideGlyphs));
    encoder.append(value.wideGlyphScale);
    encoder.append(static_cast<int>(value.shouldDrawBrailleGrid));
    encoder.append(static_cast<int>(value.brailleStyle));
    encoder.append(static_cast<int>(value.shadeStyle));
    const auto appendRule = [&](const FallbackRule& rule) {
        encoder.append(rule.script.string());
        encoder.append(rule.language.string());
        encoder.append(rule.mode.string());
        encoder.append(rule.family.string());
        encoder.append(static_cast<std::uint64_t>(rule.firstCodePoint));
        encoder.append(static_cast<std::uint64_t>(rule.lastCodePoint));
        encoder.append(static_cast<int>(rule.sizing));
    };
    encoder.append(static_cast<int>(value.languageFallbackCount));
    for (std::size_t index = 0; index < std::min<std::size_t>(value.languageFallbackCount, value.languageFallbacks.size()); ++index)
        appendRule(value.languageFallbacks[index]);
    encoder.append(static_cast<int>(value.rangeMapCount));
    for (std::size_t index = 0; index < std::min<std::size_t>(value.rangeMapCount, value.rangeMaps.size()); ++index)
        appendRule(value.rangeMaps[index]);
}

void appendFontFacts(ConfigurationEncoder& encoder, const AtlasConfiguration& value)
{
    encoder.append(value.familyName);
    encoder.append(value.pointSize);
    encoder.append(value.pixelsPerPoint);
    encoder.append(value.unifontScale);
    encoder.append(value.fontWeight);
    encoder.append(value.boldFontWeight);
    encoder.append(value.fontWidthPercent);
    encoder.append(value.proportionalFamilyName);
    encoder.append(value.proportionalPointSize);
    encoder.append(value.cjkFallbackFamily);
    encoder.append(value.cjkFallbackLanguage);
    encoder.append(value.symbolFamilyName);
    encoder.append(value.symbolFontPath);
    encoder.append(value.ligatureFeatureSpecification);
    encoder.append(value.userFeatureSpecification);
    encoder.append(value.userVariationSpecification);
    encoder.append(static_cast<int>(value.doesFontLigate));
    encoder.append(static_cast<int>(value.wantsLigatures));
    const auto& facts = value.providerResourceFacts;
    encoder.append(facts.catalogIdentity);
    encoder.append(facts.catalogGeneration);
    encoder.append(facts.rasterPixelSize);
    encoder.append(facts.deviceScale);
    encoder.append(facts.lineSpacingInPixels);
    encoder.append(facts.trackingInPixels);
    encoder.append(facts.bitmapScale);
    encoder.append(static_cast<std::uint64_t>(facts.faces.size()));
    for (const auto& face : facts.faces) {
        encoder.append(face.role);
        encoder.append(face.path);
        encoder.append(face.faceIndex);
        encoder.append(face.device);
        encoder.append(face.inode);
        encoder.append(face.byteSize);
        encoder.append(face.modifiedSeconds);
        encoder.append(face.modifiedNanoseconds);
    }
    encoder.append(facts.cjkFallbackFamily);
    encoder.append(facts.symbolFallbackFamily);
    encoder.append(facts.emojiFallbackFamily);
    encoder.append(static_cast<int>(facts.shouldMatchFallbackSize));
    for (const auto* rules : { &facts.languageFallbacks, &facts.rangeMaps }) {
        encoder.append(static_cast<std::uint64_t>(rules->size()));
        for (const auto& rule : *rules) {
            encoder.append(rule.script);
            encoder.append(rule.language);
            encoder.append(rule.family);
            encoder.append(static_cast<std::uint64_t>(rule.firstCodePoint));
            encoder.append(static_cast<std::uint64_t>(rule.lastCodePoint));
            encoder.append(rule.sizing);
            encoder.append(static_cast<int>(rule.shouldYieldToEmoji));
        }
    }
    appendTypography(encoder, facts.configuration);
}

}

std::string AtlasConfiguration::fontSharingKey() const
{
    if (familyName.empty() || !std::isfinite(pointSize) || pointSize <= 0 || !std::isfinite(pixelsPerPoint) || pixelsPerPoint <= 0
        || !std::isfinite(fontWidthPercent) || !std::isfinite(proportionalPointSize)
        || !std::isfinite(providerResourceFacts.rasterPixelSize))
        return {};
    ConfigurationEncoder encoder;
    appendFontFacts(encoder, *this);
    return encoder.takeKey();
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
    appendFontFacts(encoder, *this);
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
