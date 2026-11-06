// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/AtlasConfiguration.h"

#include <cmath>
#include <limits>

using namespace Ubiquitty::Rendering;

namespace {

AtlasConfiguration configuration()
{
    AtlasConfiguration result;
    result.resourceDomain = 1;
    result.familyName = "Fixture";
    result.pointSize = 16;
    result.pixelsPerPoint = 1;
    result.geometry.columns = 16;
    result.geometry.slotWidthInPixels = 24;
    result.geometry.slotHeightInPixels = 32;
    result.geometry.glyphWidthInPixels = 12;
    result.geometry.glyphMarginInPixels = { 2, 3 };
    result.geometry.referenceAdvanceInPixels = 11.5f;
    result.geometry.referenceLineHeightInPixels = 20;
    result.geometry.wideSlotWidthInPixels = 48;
    result.geometry.wideSlotHeightInPixels = 32;
    result.geometry.doubleSlotWidthInPixels = 48;
    result.geometry.doubleSlotHeightInPixels = 64;
    result.geometry.ligatureSlotWidthInPixels = 512;
    result.geometry.ligatureSlotHeightInPixels = 32;
    result.slotCapacity = 256;
    result.baselinePenInPixels = 7;
    result.fontWeight = 400;
    result.boldFontWeight = 700;
    result.fontWidthPercent = 100;
    return result;
}

void checkDistinct(const AtlasConfiguration& first, const AtlasConfiguration& second)
{
    CHECK(!first.sharingKey().empty());
    CHECK(!second.sharingKey().empty());
    CHECK(first.sharingKey() != second.sharingKey());
}

}

TEST(equivalentConfigurationFactsShareAnIdentity)
{
    const auto first = configuration();
    const auto second = configuration();
    CHECK(!first.sharingKey().empty());
    CHECK(first.sharingKey() == second.sharingKey());
}

TEST(everyConfigurationScalarContributesToIdentity)
{
    const auto original = configuration();
    for (auto member : { &AtlasConfiguration::pointSize, &AtlasConfiguration::pixelsPerPoint,
             &AtlasConfiguration::baselinePenInPixels, &AtlasConfiguration::wideGlyphBoostLimit, &AtlasConfiguration::fontWidthPercent }) {
        auto changed = original;
        changed.*member = std::nextafter(changed.*member, std::numeric_limits<double>::infinity());
        checkDistinct(original, changed);
    }
    for (auto member : { &AtlasConfiguration::slotCapacity, &AtlasConfiguration::unifontScale,
             &AtlasConfiguration::fontWeight, &AtlasConfiguration::boldFontWeight }) {
        auto changed = original;
        ++(changed.*member);
        checkDistinct(original, changed);
    }
    for (auto member : { &AtlasConfiguration::doesFontLigate, &AtlasConfiguration::wantsLigatures }) {
        auto changed = original;
        changed.*member = true;
        checkDistinct(original, changed);
    }
    auto changed = original;
    ++changed.resourceDomain;
    checkDistinct(original, changed);
}

TEST(everySheetGeometryAxisContributesToIdentity)
{
    const auto original = configuration();
    for (auto member : { &GlyphSlotGeometry::columns, &GlyphSlotGeometry::slotWidthInPixels,
             &GlyphSlotGeometry::slotHeightInPixels, &GlyphSlotGeometry::glyphWidthInPixels,
             &GlyphSlotGeometry::wideSlotWidthInPixels, &GlyphSlotGeometry::wideSlotHeightInPixels,
             &GlyphSlotGeometry::doubleSlotWidthInPixels, &GlyphSlotGeometry::doubleSlotHeightInPixels,
             &GlyphSlotGeometry::ligatureSlotWidthInPixels, &GlyphSlotGeometry::ligatureSlotHeightInPixels }) {
        auto changed = original;
        ++(changed.geometry.*member);
        checkDistinct(original, changed);
    }
    for (auto member : { &GlyphSlotGeometry::referenceAdvanceInPixels, &GlyphSlotGeometry::referenceLineHeightInPixels }) {
        auto changed = original;
        changed.geometry.*member = std::nextafter(changed.geometry.*member, std::numeric_limits<float>::infinity());
        checkDistinct(original, changed);
    }
    auto horizontal = original;
    ++horizontal.geometry.glyphMarginInPixels.width;
    checkDistinct(original, horizontal);
    auto vertical = original;
    ++vertical.geometry.glyphMarginInPixels.height;
    checkDistinct(original, vertical);
    auto participation = original;
    participation.geometry.ligatureParticipation = LigatureParticipation::AllEligibleRuns;
    checkDistinct(original, participation);
}

TEST(everyBoxStrokeFactContributesToIdentity)
{
    for (auto member : { &BoxDrawing::BoxStrokeProfile::lightHorizontal, &BoxDrawing::BoxStrokeProfile::lightVertical,
             &BoxDrawing::BoxStrokeProfile::heavyHorizontal, &BoxDrawing::BoxStrokeProfile::heavyVertical }) {
        auto original = configuration();
        original.geometry.boxStrokeProfile.*member = { true, 0.5f, 0.1f };
        auto validity = original;
        (validity.geometry.boxStrokeProfile.*member).isValid = false;
        checkDistinct(original, validity);
        auto center = original;
        (center.geometry.boxStrokeProfile.*member).centerInCell = 0.6f;
        checkDistinct(original, center);
        auto thickness = original;
        (thickness.geometry.boxStrokeProfile.*member).thicknessInCell = 0.2f;
        checkDistinct(original, thickness);
    }
    for (auto member : { &BoxDrawing::BoxStrokeProfile::doubleHorizontal, &BoxDrawing::BoxStrokeProfile::doubleVertical }) {
        auto original = configuration();
        original.geometry.boxStrokeProfile.*member = { true, 0.5f, 0.1f, 0.1f };
        auto validity = original;
        (validity.geometry.boxStrokeProfile.*member).isValid = false;
        checkDistinct(original, validity);
        for (auto bandMember : { &BoxDrawing::BoxDoubleBand::centerInCell, &BoxDrawing::BoxDoubleBand::railThicknessInCell, &BoxDrawing::BoxDoubleBand::gapInCell }) {
            auto changed = original;
            ((changed.geometry.boxStrokeProfile.*member).*bandMember) += 0.1f;
            checkDistinct(original, changed);
        }
    }
}

TEST(everyFontStringContributesExactUnboundedBytes)
{
    const auto original = configuration();
    for (auto member : { &AtlasConfiguration::familyName, &AtlasConfiguration::cjkFallbackFamily,
             &AtlasConfiguration::cjkFallbackLanguage, &AtlasConfiguration::symbolFamilyName,
             &AtlasConfiguration::symbolFontPath, &AtlasConfiguration::ligatureFeatureSpecification,
             &AtlasConfiguration::userFeatureSpecification, &AtlasConfiguration::userVariationSpecification }) {
        auto first = original;
        first.*member = std::string(600, 'a') + 'x';
        auto second = first;
        (second.*member).back() = 'y';
        checkDistinct(first, second);
    }
}

TEST(stringBoundariesAndUnicodeSpellingCannotAlias)
{
    auto first = configuration();
    auto second = first;
    first.symbolFamilyName = "family|suffix";
    first.symbolFontPath = "font.ttf";
    second.symbolFamilyName = "family";
    second.symbolFontPath = "suffix|font.ttf";
    checkDistinct(first, second);
    first.familyName = "caf\xc3\xa9";
    second = first;
    second.familyName = "cafe\xcc\x81";
    checkDistinct(first, second);
    second = first;
    second.familyName += '\0';
    checkDistinct(first, second);
}

TEST(nonfiniteConfigurationAndGeometryFactsAreRejected)
{
    for (double value : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() }) {
        for (auto member : { &AtlasConfiguration::pointSize, &AtlasConfiguration::pixelsPerPoint,
                 &AtlasConfiguration::baselinePenInPixels, &AtlasConfiguration::wideGlyphBoostLimit, &AtlasConfiguration::fontWidthPercent }) {
            auto invalid = configuration();
            invalid.*member = value;
            CHECK(invalid.sharingKey().empty());
        }
        for (auto member : { &GlyphSlotGeometry::referenceAdvanceInPixels, &GlyphSlotGeometry::referenceLineHeightInPixels }) {
            auto invalid = configuration();
            invalid.geometry.*member = static_cast<float>(value);
            CHECK(invalid.sharingKey().empty());
        }
    }
}

TEST(invalidRequiredAndOptionalGeometryIsRejected)
{
    for (auto member : { &GlyphSlotGeometry::columns, &GlyphSlotGeometry::slotWidthInPixels,
             &GlyphSlotGeometry::slotHeightInPixels, &GlyphSlotGeometry::glyphWidthInPixels }) {
        auto invalid = configuration();
        invalid.geometry.*member = 0;
        CHECK(invalid.sharingKey().empty());
    }
    for (auto member : { &GlyphSlotGeometry::wideSlotWidthInPixels, &GlyphSlotGeometry::wideSlotHeightInPixels,
             &GlyphSlotGeometry::doubleSlotWidthInPixels, &GlyphSlotGeometry::doubleSlotHeightInPixels,
             &GlyphSlotGeometry::ligatureSlotWidthInPixels, &GlyphSlotGeometry::ligatureSlotHeightInPixels }) {
        auto invalid = configuration();
        invalid.geometry.*member = 0;
        CHECK(invalid.sharingKey().empty());
        invalid.geometry.*member = -1;
        CHECK(invalid.sharingKey().empty());
    }
    auto invalid = configuration();
    invalid.geometry.glyphMarginInPixels.width = -1;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.geometry.ligatureParticipation = static_cast<LigatureParticipation>(255);
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.geometry.boxStrokeProfile.lightVertical.isValid = true;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.geometry.boxStrokeProfile.doubleVertical.gapInCell = std::numeric_limits<float>::quiet_NaN();
    CHECK(invalid.sharingKey().empty());
}

TEST(invalidConfigurationCannotBecomeASharedEmptyIdentity)
{
    CHECK(AtlasConfiguration().sharingKey().empty());
    auto invalid = configuration();
    invalid.resourceDomain = 0;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.familyName.clear();
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.pointSize = 0;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.pixelsPerPoint = -1;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.slotCapacity = 0;
    CHECK(invalid.sharingKey().empty());
    invalid = configuration();
    invalid.unifontScale = -1;
    CHECK(invalid.sharingKey().empty());
}

UBIQUITTY_TEST_MAIN()
