// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/RichText/Drawing.h"
#include "Core/RichText/RTFReader.h"

#include <algorithm>
#include <string_view>

using namespace Ubiquitty::RichText;

namespace {

constexpr std::string_view requestedFamily = "日本語 Family";

Block sourceBlock()
{
    constexpr std::string_view source = "{\\rtf1{\\fonttbl{\\f0\\fnil 日本語 Family;}}\\f0 a\\b b\\b0\\i c}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    CHECK(document.has_value());
    if (!document)
        return {};
    Block block;
    block.document = std::move(*document);
    block.lineMode = LineMode::Terminal;
    return block;
}

TypographyFacts fontFacts()
{
    TypographyFacts facts;
    facts.cellWidth = 512;
    facts.cellHeight = 1024;
    facts.baseline = 768;
    facts.proportionalSize = 768;
    facts.monospaceSize = 1536;
    facts.faceCount = 4;
    for (std::size_t index = 0; index < facts.faceCount; ++index) {
        auto& face = facts.faces[index];
        face.familyClass = FamilyClass::SansSerif;
        face.styles = static_cast<std::uint8_t>(index);
        face.ascent = 1536;
        face.descent = 512;
        face.lineHeight = 2048;
        face.averageAdvance = 1024;
        face.advances.fill(1024);
    }
    // A regular-only metrics alias must not change a native run's fallback
    // class or terminal-size choice when bold or italic is selected.
    auto& alias = facts.faces[0];
    alias.familyClass = FamilyClass::Monospace;
    alias.nameLength = static_cast<std::uint8_t>(requestedFamily.size());
    std::ranges::copy(requestedFamily, alias.name.begin());
    return facts;
}

} // namespace

UBIQUITTY_TEST_MAIN()

TEST(nativeDrawingOwnsNamedFamiliesAndPreservesTheSourceClassAcrossStyles)
{
    auto block = sourceBlock();
    const auto facts = fontFacts();
    const Shaper shaper { [](void*, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText* result) {
        result->appendLine({ 0, 3, 0, 65536, 49152, 0, 24576 });
        for (std::uint32_t index = 0; index < 3; ++index)
            result->appendSpan({ index, index + 1, 0, static_cast<std::int32_t>(index * 8192), static_cast<std::int32_t>((index + 1) * 8192) });
        return true;
    } };
    const auto layout = Layout::lay(block.document, 10, 2, Overflow::Wrap, Alignment::Left, &facts, LineMode::Terminal, &shaper);
    REQUIRE(layout.isShaped());
    const auto drawing = BlockDrawing::draw(block, layout, &facts);
    block = {};
    REQUIRE(drawing.styles().size() == 3);
    for (const auto& style : drawing.styles()) {
        CHECK(style.familyName() == requestedFamily);
        CHECK_EQ(style.familyClass, FamilyClass::SansSerif);
        CHECK_EQ(style.halfPoints, 24.0f);
    }
    CHECK_EQ(drawing.styles()[1].styles, std::to_underlying(StyleBit::Bold));
    CHECK_EQ(drawing.styles()[2].styles, std::to_underlying(StyleBit::Italic));
}

TEST(tableDrawingUsesOnlyTheFamilyWhoseMetricsWereMeasured)
{
    const auto block = sourceBlock();
    const auto facts = fontFacts();
    const auto layout = Layout::lay(block.document, 10, 2, Overflow::Wrap, Alignment::Left, &facts, LineMode::Terminal);
    CHECK(!layout.isShaped());
    const auto drawing = BlockDrawing::draw(block, layout, &facts);
    REQUIRE(drawing.styles().size() == 3);
    CHECK(drawing.styles()[0].familyName() == requestedFamily);
    CHECK_EQ(drawing.styles()[0].familyClass, FamilyClass::Monospace);
    CHECK_EQ(drawing.styles()[0].halfPoints, 48.0f);
    for (const auto& style : drawing.styles().subspan(1)) {
        CHECK(style.nameView().empty());
        CHECK_EQ(style.familyClass, FamilyClass::SansSerif);
        CHECK_EQ(style.halfPoints, 24.0f);
    }
}
