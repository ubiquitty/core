// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_rtf_reader.cpp — the RTF subset a rich text block accepts
// (Docs/rich-text.md, "The RTF subset"): the text and runs a body reads as,
// strict UTF-8, optional destinations, and whole-body refusal on malformed input.

#include "Core/RichText/RTFReader.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::RichText;

namespace {

std::expected<Document, ReadError> read(std::string_view body)
{
    return readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
}

std::u32string textOf(const Document& document)
{
    std::u32string text;
    for (const std::uint32_t codePoint : document.text)
        text.push_back(static_cast<char32_t>(codePoint));
    return text;
}

bool refusedWith(std::string_view body, ReadError error)
{
    const auto result = read(body);
    return !result && result.error() == error;
}

} // namespace

TEST(aPlainBodyReadsAsOneRunInTheDefaultStyle)
{
    const auto document = read("{\\rtf1 Hello}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(textOf(*document) == U"Hello");
    CHECK_EQ(document->runs.size(), std::size_t { 1 });
    const Run& run = document->runs[0];
    CHECK_EQ(run.start, 0u);
    CHECK_EQ(run.length, 5u);
    CHECK_EQ(run.halfPoints, defaultHalfPoints);
    CHECK_EQ(run.styles, 0);
    CHECK_EQ(run.font, noFontEntry);
    CHECK_EQ(run.color, 0);
}

TEST(anEmptyBodyHasNoTextAndNoRuns)
{
    const auto document = read("{\\rtf1}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(document->text.empty());
    CHECK(document->runs.empty());
}

TEST(aStyleChangeStartsARunAndAParagraphIsALineFeed)
{
    const auto document = read("{\\rtf1\\b Title\\b0\\par\\fs20 body}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(textOf(*document) == U"Title\nbody");
    CHECK_EQ(document->runs.size(), std::size_t { 3 });
    CHECK(document->runs[0].has(StyleBit::Bold));
    CHECK_EQ(document->runs[0].length, 5u);
    CHECK(!document->runs[1].has(StyleBit::Bold));
    CHECK_EQ(document->runs[1].length, 1u);
    CHECK_EQ(document->runs[2].halfPoints, std::uint16_t { 20 });
    CHECK_EQ(document->runs[2].start, 6u);
}

TEST(aGroupRestoresTheFormattingItOpenedWith)
{
    const auto document = read("{\\rtf1 a{\\b\\i\\ul\\strike b}c}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(textOf(*document) == U"abc");
    CHECK_EQ(document->runs.size(), std::size_t { 3 });
    CHECK_EQ(document->runs[1].styles, allStyleBits);
    CHECK_EQ(document->runs[2].styles, 0);
}

TEST(theFontTableNamesFamiliesAndClasses)
{
    const auto document = read("{\\rtf1\\deff1{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier New;}"
                               "{\\f2\\froman{\\*\\panose 0}Times;}}default\\f0 sans\\f2 serif\\plain default}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK_EQ(int { document->fontCount() }, 3);
    CHECK(document->fonts[0].familyName() == "Helvetica");
    CHECK(document->fonts[0].familyClass == FamilyClass::SansSerif);
    CHECK(document->fonts[1].familyName() == "Courier New");
    CHECK(document->fonts[1].familyClass == FamilyClass::Monospace);
    CHECK(document->fonts[2].familyName() == "Times");
    CHECK(document->fonts[2].familyClass == FamilyClass::Serif);
    CHECK(textOf(*document) == U"defaultsansserifdefault");
    CHECK_EQ(document->runs.size(), std::size_t { 4 });
    // \deff1 selects the second entry before any \f and after \plain.
    CHECK_EQ(int { document->runs[0].font }, 1);
    CHECK_EQ(int { document->runs[1].font }, 0);
    CHECK_EQ(int { document->runs[2].font }, 2);
    CHECK_EQ(int { document->runs[3].font }, 1);
}

TEST(theColourTableKeepsAnEmptyEntryAutomatic)
{
    const auto document = read(R"({\rtf1{\colortbl;\red255\green0\blue0;\red0\green102\blue204;;}a\cf2 b\cf0 c\cf3 d})");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK_EQ(int { document->colorCount }, 4);
    CHECK(document->colors[0].isAutomatic);
    CHECK(!document->colors[1].isAutomatic);
    CHECK_EQ(int { document->colors[1].red }, 255);
    CHECK_EQ(int { document->colors[2].green }, 102);
    CHECK_EQ(int { document->colors[2].blue }, 204);
    CHECK_EQ(document->runs.size(), std::size_t { 3 });
    CHECK_EQ(int { document->runs[1].color }, 2);
    // A declared empty entry is automatic, so c and d share a run.
    CHECK_EQ(int { document->runs[2].color }, 0);
    CHECK_EQ(document->runs[2].length, 2u);
}

TEST(rawUnicodeRetainsScalarsWithoutEncodingDeclarationsOrFallback)
{
    const auto document = read("{\\rtf1 a—béc…d😀}");
    REQUIRE(document.has_value());
    CHECK(textOf(*document) == U"a\u2014b\u00E9c\u2026d\U0001F600\uF0B7");
}

TEST(legacyEncodingControlsAreRefusedInEveryDestination)
{
    constexpr std::array controls {
        "\\ansi",
        "\\ansicpg1252",
        "\\ansicpg65001",
        "\\mac",
        "\\pc",
        "\\pca",
        "\\cpg65001",
        "\\fcharset0",
        "\\u233",
        "\\uc0",
        "\\'e9",
        "\\'",
        "\\'zz",
    };
    for (const auto control : controls) {
        CHECK(refusedWith(std::string("{\\rtf1") + control + " x}", ReadError::UnsupportedControl));
        CHECK(refusedWith(std::string("{\\rtf1{\\fonttbl{\\f0") + control + " Family;}}x}", ReadError::UnsupportedControl));
        CHECK(refusedWith(std::string("{\\rtf1{\\*\\optional") + control + " x}body}", ReadError::UnsupportedControl));
    }
}

TEST(controlSymbolsAndNamedCharactersReadAsTheirCharacters)
{
    const auto document = read("{\\rtf1 \\\\\\{\\}a\\~b\\_c\\-d\\tab e\\line f\\emdash\\endash\\bullet\\lquote\\rquote\\ldblquote\\rdblquote}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(textOf(*document) == U"\\{}a\u00A0b\u2011cd\te\u2028f\u2014\u2013\u2022\u2018\u2019\u201C\u201D");
}

TEST(lineEndsAreIgnoredAndAnEscapedOneIsAParagraph)
{
    const auto document = read("{\\rtf1 a\r\nb\\\nc}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(textOf(*document) == U"ab\nc");
}

TEST(onlyExplicitlyOptionalDestinationsIgnoreUnknownControls)
{
    const auto document = read("{\\rtf1{\\*\\generator Writer;}{\\*\\info{\\title Secret}}{\\*\\stylesheet{\\s0 Normal;}}"
                               "\\pard\\sa200\\sl276\\slmult1 Hi{\\*\\unknown 名字\\future17 {more}}!}");
    REQUIRE(document.has_value());
    CHECK(textOf(*document) == U"Hi!");
    for (const auto control : { "\\viewkind4", "\\info", "\\stylesheet", "\\pict", "\\object", "\\field", "\\fldinst", "\\fldrslt", "\\future", "\\?" })
        CHECK(refusedWith(std::string("{\\rtf1") + control + " x}", ReadError::UnsupportedControl));
    // Control names are case-sensitive. These spellings have no encoding
    // meaning, and can only occur as opaque optional-destination content.
    for (const auto control : { "\\ANSI", "\\AnsiCpG65001", "\\FCHARSET0", "\\U233", "\\UC0" }) {
        CHECK(refusedWith(std::string("{\\rtf1") + control + " x}", ReadError::UnsupportedControl));
        CHECK(read(std::string("{\\rtf1{\\*\\optional") + control + " x}body}"));
    }
}

TEST(optionalMarkersMustStartAGroupAndNameADestination)
{
    for (const auto body : { R"({\rtf1\*\hidden text})", R"({\rtf1{a\*\hidden text}})", "{\\rtf1{\\*}}", "{\\rtf1{\\* text}}", R"({\rtf1{\*{\hidden text}}})" })
        CHECK(refusedWith(body, ReadError::Malformed));
}

TEST(paragraphAlignmentRidesOnTheRuns)
{
    const auto document = read("{\\rtf1\\qc Title\\par\\pard Body\\par\\qr End}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK(document->runs.front().alignment == Alignment::Center);
    CHECK(document->runs[1].alignment == Alignment::Unspecified);
    CHECK(document->runs.back().alignment == Alignment::Right);
}

// A line break keeps its paragraph whole: \line writes U+2028 and only \par
// ends a paragraph with U+000A.
TEST(aLineBreakIsALineSeparatorAndAParagraphBreakALineFeed)
{
    const auto document = read("{\\rtf1 a\\line b\\par c}");
    REQUIRE(document.has_value());
    CHECK(textOf(*document) == U"a b\nc");
}

// The paragraph properties stick until \pard, as alignment does, and a value
// outside its range refuses the body.
TEST(paragraphSpacingAndIndentsRideOnTheRuns)
{
    const auto document = read("{\\rtf1\\sl-360\\sb120\\sa60\\li720\\ri240\\fi-360 Hang\\par\\pard\\sl480\\slmult1 Double\\par\\sl300 Least}");
    REQUIRE(document.has_value());
    REQUIRE(document->runs.size() == 3);
    const Paragraph& hanging = document->runs[0].paragraph;
    CHECK(hanging.lineSpacing == LineSpacing::Exactly);
    CHECK_EQ(hanging.lineSpacingValue, 360);
    CHECK_EQ(hanging.spaceBefore, 120);
    CHECK_EQ(hanging.spaceAfter, 60);
    CHECK_EQ(hanging.leftIndent, 720);
    CHECK_EQ(hanging.rightIndent, 240);
    CHECK_EQ(hanging.firstIndent, -360);
    const Paragraph& doubled = document->runs[1].paragraph;
    CHECK(doubled.lineSpacing == LineSpacing::Multiple);
    CHECK_EQ(doubled.lineSpacingValue, 480);
    CHECK_EQ(doubled.leftIndent, 0);
    const Paragraph& least = document->runs[2].paragraph;
    CHECK(least.lineSpacing == LineSpacing::Multiple);
    CHECK_EQ(least.lineSpacingValue, 300);
    CHECK(isWellFormed(*document));
}

TEST(aParagraphPropertyOutOfRangeRefusesTheBody)
{
    CHECK(refusedWith("{\\rtf1\\sb32768 a}", ReadError::ParagraphOutOfRange));
    CHECK(refusedWith("{\\rtf1\\li-1 a}", ReadError::ParagraphOutOfRange));
    CHECK(refusedWith("{\\rtf1\\fi-32768 a}", ReadError::ParagraphOutOfRange));
    CHECK(refusedWith("{\\rtf1\\slmult2 a}", ReadError::ParagraphOutOfRange));
    CHECK(refusedWith("{\\rtf1\\sl2401\\slmult1 a}", ReadError::ParagraphOutOfRange));
    CHECK(read("{\\rtf1\\sl2400\\slmult1\\fi-32767\\ri32767 a}").has_value());
}

// A multiple is read from `\sl` and `\slmult` together, in either order, and
// refuses the body only when text takes it: `\pard` and a closing group put
// back a paragraph the text can take.
TEST(aSpacingMultipleOutOfRangeRefusesOnlyTheTextThatTakesIt)
{
    CHECK(refusedWith("{\\rtf1\\slmult1\\sl2401 a}", ReadError::ParagraphOutOfRange));
    CHECK(refusedWith("{\\rtf1 a{\\sl2401\\slmult1 b}c}", ReadError::ParagraphOutOfRange));
    CHECK(read("{\\rtf1\\sl2401\\slmult1\\pard a}").has_value());
    CHECK(read("{\\rtf1\\sl2401\\slmult1\\sl2400 a}").has_value());
    const auto document = read("{\\rtf1 a{\\sl2401\\slmult1}b{\\sl480\\slmult1 c}d}");
    REQUIRE(document.has_value());
    REQUIRE(document->runs.size() == 3);
    CHECK(document->runs[0].paragraph == Paragraph {});
    CHECK_EQ(document->runs[0].length, 2u);
    CHECK(document->runs[1].paragraph.lineSpacing == LineSpacing::Multiple);
    CHECK_EQ(document->runs[1].paragraph.lineSpacingValue, 480);
    CHECK(document->runs[2].paragraph == Paragraph {});
}

TEST(plainResetsCharacterFormatting)
{
    const auto document = read(R"({\rtf1{\fonttbl{\f0\fswiss A;}{\f1\froman B;}}{\colortbl;\red1;}\f1\fs40\b\cf1 x\plain y})");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK_EQ(document->runs.size(), std::size_t { 2 });
    CHECK_EQ(int { document->runs[1].font }, 0);
    CHECK_EQ(document->runs[1].halfPoints, defaultHalfPoints);
    CHECK_EQ(document->runs[1].styles, 0);
    CHECK_EQ(int { document->runs[1].color }, 0);
}

TEST(aSizeIsClampedToTheSupportedRange)
{
    const auto document = read("{\\rtf1\\fs0 a\\fs99999 b}");
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK_EQ(document->runs[0].halfPoints, minHalfPoints);
    CHECK_EQ(document->runs[1].halfPoints, maxHalfPoints);
}

TEST(malformedUTF8IsRefusedInTextFontNamesAndOptionalGroups)
{
    for (const auto bytes : { "\x80", "\xC0\xAF", "\xC1\xBF", "\xC2", "\xE2\x80", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF0\x80\x80\xAF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF" }) {
        CHECK(refusedWith(std::string("{\\rtf1 ") + bytes + "}", ReadError::InvalidUTF8));
        CHECK(refusedWith(std::string("{\\rtf1{\\fonttbl{\\f0 ") + bytes + ";}}x}", ReadError::InvalidUTF8));
        CHECK(refusedWith(std::string("{\\rtf1{\\*\\optional ") + bytes + "}x}", ReadError::InvalidUTF8));
    }
    CHECK(refusedWith("{\\rtf1 \xC3{\\b\xA9}}", ReadError::InvalidUTF8));
    CHECK(refusedWith("{\\rtf1 \xC3\\b\xA9}", ReadError::InvalidUTF8));
}

TEST(theRootAndEveryGroupAndControlEscapeMustBeComplete)
{
    for (const auto body : { "{\\rtf1", "{\\rtf1 a{\\b b", "{\\rtf1\\", "{\\rtf1 x\\}", "{\\rtf1 x}}", "{\\rtf1 x}junk", "{\\rtf1 x}{\\rtf1 y}", R"({\rtf1{\*\optional x})" })
        CHECK(refusedWith(body, ReadError::Malformed));
    CHECK(read("{\\rtf1 x} \t\r\n").has_value());
}

TEST(aBodyThatIsNotRTFIsRefused)
{
    CHECK(refusedWith("", ReadError::NotRTF));
    CHECK(refusedWith("hello", ReadError::NotRTF));
    CHECK(refusedWith("{\\rtf2 x}", ReadError::NotRTF));
    CHECK(refusedWith("{\\rtf10 x}", ReadError::NotRTF));
    CHECK(refusedWith("{\\rtfx}", ReadError::NotRTF));
    CHECK(read("{\\rtf1}").has_value());
}

TEST(everyMaximumRefusesTheWholeBody)
{
    std::string longBody = "{\\rtf1 ";
    longBody.append(maxBodyBytes, ' ');
    longBody += "}";
    CHECK(refusedWith(longBody, ReadError::BodyTooLong));

    std::string deep = "{\\rtf1 ";
    for (int depth = 0; depth < maxGroupDepth; ++depth)
        deep += "{";
    CHECK(refusedWith(deep, ReadError::TooDeep));
    std::string deepest = "{\\rtf1 ";
    for (int depth = 1; depth < maxGroupDepth; ++depth)
        deepest += "{";
    deepest.append(maxGroupDepth, '}');
    CHECK(read(deepest).has_value());

    std::string text = "{\\rtf1 ";
    text.append(static_cast<std::size_t>(maxCodePoints), 'x');
    CHECK(read(text + "}").has_value());
    CHECK(refusedWith(text + "y}", ReadError::TooMuchText));

    std::string runs = "{\\rtf1 ";
    for (int run = 0; run < maxRuns; ++run)
        runs += (run % 2) ? "\\b0 x" : "\\b x";
    CHECK(read(runs + "}").has_value());
    CHECK(refusedWith(runs + "\\b x}", ReadError::TooManyRuns));

    std::string fonts = "{\\rtf1{\\fonttbl";
    for (int font = 0; font < maxFonts; ++font)
        fonts += "{\\f" + std::to_string(font) + " F;}";
    CHECK(read(fonts + "}x}").has_value());
    CHECK(refusedWith(fonts + "{\\f99 G;}}x}", ReadError::TooManyFonts));

    std::string colors = "{\\rtf1{\\colortbl";
    for (int color = 0; color < maxColors; ++color)
        colors += "\\red1;";
    CHECK(read(colors + "}x}").has_value());
    CHECK(refusedWith(colors + "\\red2;}x}", ReadError::TooManyColors));
}

TEST(aFamilyNameAtItsByteLimitIsPreserved)
{
    std::string body = "{\\rtf1{\\fonttbl{\\f0 ";
    body.append(maxFamilyNameBytes, 'N');
    body += ";}}\\fs2147483647 x}";
    const auto document = read(body);
    CHECK(document.has_value());
    if (!document)
        return;
    CHECK_EQ(int { document->fonts[0].nameLength }, maxFamilyNameBytes);
    CHECK_EQ(document->runs[0].halfPoints, maxHalfPoints);
}

TEST(anOverlongFamilyNameIsRefusedInsteadOfSelectingItsPrefix)
{
    std::string body = R"({\rtf1{\fonttbl{\f0 )";
    body.append(maxFamilyNameBytes, 'N');
    CHECK(refusedWith(body + "x;}}x}", ReadError::FamilyNameTooLong));
}

TEST(unicodeFamilyNamesAndEscapedPunctuationRetainTheirExactBytes)
{
    const auto document = read("{\\rtf1{\\fonttbl{\\f0\\fswiss  家族  Café\\;\\{Display\\}\\\\  ;}}\\f0 x}");
    REQUIRE(document);
    CHECK(document->fonts[0].familyName() == "家族  Café;{Display}\\");
    std::string family;
    while (family.size() + 3 <= static_cast<std::size_t>(maxFamilyNameBytes))
        family += "界";
    family.append(static_cast<std::size_t>(maxFamilyNameBytes) - family.size(), 'x');
    const auto atLimit = read(R"({\rtf1{\fonttbl{\f0 )" + family + ";}}x}");
    REQUIRE(atLimit);
    CHECK(atLimit->fonts[0].familyName() == family);
    const auto paddedLimit = read(R"({\rtf1{\fonttbl{\f0 )" + family + "   ;}}x}");
    REQUIRE(paddedLimit);
    CHECK(paddedLimit->fonts[0].familyName() == family);
    CHECK(refusedWith("{\\rtf1{\\fonttbl{\\f0 " + family + "é;}}x}", ReadError::FamilyNameTooLong));
    CHECK(refusedWith("{\\rtf1\\;}", ReadError::UnsupportedControl));
}

TEST(rawControlCharactersCannotHideInTextNamesOrOptionalGroups)
{
    for (const unsigned char byte : { 0x00, 0x01, 0x07, 0x09, 0x0B, 0x0C, 0x1B, 0x1F, 0x7F }) {
        const std::string control(1, static_cast<char>(byte));
        CHECK(refusedWith("{\\rtf1 a" + control + "b}", ReadError::Malformed));
        CHECK(refusedWith("{\\rtf1{\\fonttbl{\\f0 A" + control + "B;}}x}", ReadError::Malformed));
        CHECK(refusedWith("{\\rtf1{\\*\\optional " + control + "}x}", ReadError::Malformed));
    }
    for (const auto control : { "\xC2\x80", "\xC2\x85", "\xC2\x9F" })
        CHECK(refusedWith(std::string("{\\rtf1 ") + control + "}", ReadError::Malformed));
}

TEST(controlParametersCannotOverflowOrOmitTheirRequiredNumber)
{
    for (const auto control : { "\\fs2147483648", "\\fs-2147483649", "\\fs999999999999999999999", "\\fs-", "\\fs", "\\f", "\\deff", "\\cf", "\\sl", "\\slmult", "\\tab1", "\\plain0" })
        CHECK(refusedWith(std::string("{\\rtf1") + control + " x}", ReadError::Malformed));
    CHECK(refusedWith("{\\rtf1{\\*\\optional\\number2147483648}x}", ReadError::Malformed));
    CHECK(read("{\\rtf1\\fs-2147483648 a\\fs2147483647 b}").has_value());
    CHECK(refusedWith("{\\rtf1{\\fonttbl{\\f0 Name}}x}", ReadError::Malformed));
    CHECK(refusedWith("{\\rtf1{\\colortbl\\red255}x}", ReadError::Malformed));
}

TEST(fontAndColorDeclarationsHaveUniqueDefinedReferences)
{
    for (const auto body : {
             R"({\rtf1{\fonttbl{\f0 A;}{\f0 B;}}x})",
             R"({\rtf1{\fonttbl{\f0 A;}}{\fonttbl{\f1 B;}}x})",
             R"({\rtf1{\colortbl;}{\colortbl;}})",
             R"({\rtf1 x{\fonttbl{\f0 A;}}})",
             "{\\rtf1 x{\\colortbl;}}",
             "{\\rtf1\\f0 x}",
             R"({\rtf1{\fonttbl{\f0 A;}}\f1 x})",
             R"({\rtf1\deff1{\fonttbl{\f0 A;}}x})",
             "{\\rtf1\\deff0}",
             "{\\rtf1\\cf1 x}",
             R"({\rtf1{\colortbl;}\cf1 x})",
             R"({\rtf1{\colortbl;}\highlight1 x})",
             "{\\rtf1\\cf-1 x}",
             "{\\rtf1\\highlight-1 x}" })
        CHECK(refusedWith(body, ReadError::Malformed));
    const auto defaults = read(R"({\rtf1\cf0\highlight0 default\plain text})");
    REQUIRE(defaults);
    CHECK_EQ(defaults->runs.front().font, noFontEntry);
    CHECK_EQ(defaults->runs.front().color, 0);
    CHECK_EQ(defaults->runs.front().background, 0);
    CHECK(read("{\\rtf1\\deff1{\\fonttbl{\\f1 Family;}}text}").has_value());
}

TEST(everyTruncatedPrefixOfACompleteUnicodeBodyIsRefused)
{
    const std::string body = "{\\rtf1{\\fonttbl{\\f0 家族;}}\\f0 café {\\b é😀}\\par 終}";
    REQUIRE(read(body));
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!read(std::string_view(body).substr(0, length)));
}

TEST(canonicalDocumentsEnforceTheSameUnicodeTextAndFamilyContract)
{
    auto document = read("{\\rtf1{\\fonttbl{\\f0 家族;}}\\f0 x}");
    REQUIRE(document);
    for (const auto codePoint : { 0u, 1u, 7u, 11u, 12u, 13u, 27u, 31u, 127u, 128u, 133u, 159u, 0xD800u, 0x110000u }) {
        document->text[0] = codePoint;
        CHECK(!isWellFormed(*document));
    }
    for (const auto codePoint : { 9u, 10u, 32u, 0xA0u, 0x301u, 0x2028u, 0x1F600u }) {
        document->text[0] = codePoint;
        CHECK(isWellFormed(*document));
    }
    for (const std::string_view family : { "\xC3", "\xED\xA0\x80", "A\nB", "A\tB", "A\x7F", "\xC2\x85", " leading", "trailing " }) {
        auto& font = document->fonts[0];
        font.nameLength = static_cast<std::uint8_t>(family.size());
        std::ranges::copy(family, font.name.begin());
        CHECK(!isWellFormed(*document));
    }
    document->fonts[0].nameLength = 0;
    CHECK(isWellFormed(*document));
}

TEST(aUnicodeOSCBodySurvivesEveryTransportSplit)
{
    const std::string wire = "\x1b]7404;w=20:h=2:C=1:i=1;{\\rtf1{\\fonttbl{\\f0 家族;}}\\f0 café Ü {\\b é😀}\\par 終}\x1b\\";
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 6, 30);
        UbiquittyTest::feed(terminal, std::string_view(wire).substr(0, split));
        UbiquittyTest::feed(terminal, std::string_view(wire).substr(split));
        const auto blocks = terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
        REQUIRE(blocks.size() == 1);
        CHECK(textOf(blocks.front().document) == U"café Ü é😀\n終");
        CHECK(blocks.front().document.fonts[0].familyName() == "家族");
    }
}

TEST(refusedUnicodeOSCBodiesPreserveTheExistingPlacementAndCursor)
{
    for (const auto body : { "{\\rtf1\\ansi changed}", "{\\rtf1{\\b changed}", "{\\rtf1 \xC3}", "{\\rtf1 changed}trailing" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 6, 30);
        UbiquittyTest::feed(terminal, "\x1b]7404;w=20:h=1:C=1:i=1;{\\rtf1 original}\x1b\\");
        REQUIRE(terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size() == 1);
        const auto held = terminal.richTextBlocks;
        UbiquittyTest::feed(terminal, std::string("\x1b]7404;w=20:h=1:i=1:e=1;") + body + "\x1b\\");
        CHECK(terminal.richTextBlocks == held);
        CHECK_EQ(terminal.screens.active->cursorX, 0);
        CHECK_EQ(terminal.screens.active->cursorY, 0);
        std::array<char, 256> reply {};
        const auto length = UbiquittyTest::drain(terminal, reply);
        REQUIRE(length > 0);
        const std::string_view status(reply.data(), static_cast<std::size_t>(length));
        CHECK(status.find(std::string_view(body).find("\\ansi") != std::string_view::npos ? ":s=unsupported" : ":s=invalid") != std::string_view::npos);
    }
}

UBIQUITTY_TEST_MAIN()
