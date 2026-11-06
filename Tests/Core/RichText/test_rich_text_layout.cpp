// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_rich_text_layout.cpp — a block laid out from a typography table and
// the fit query answered from it (Docs/rich-text.md, "Layout", "The
// measurement model" and "Query"): the pointer-to-character rule over a
// synthetic advance table, line breaking under each overflow policy,
// alignment, the reply, and the reply refused where a terminal must not
// answer.

#include "TerminalFixture.h"

#include "Core/OSC/RichTextBlocks.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Drawing.h"
#include "Core/RichText/Layout.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TerminalShared.h"
#include "Core/VT/Constants.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::RichText;

namespace {

class MeasurementClock final : public Ubiquitty::TerminalClock {
public:
    Ubiquitty::Ticks now() override { return instant; }
    Ubiquitty::Ticks instant;
};

constexpr std::uint16_t narrowAdvance = 400; // `i` and `l`
constexpr std::uint16_t letterAdvance = 1024;
constexpr std::uint16_t averageAdvance = 1000;
constexpr std::uint16_t lineHeight = 2400;
constexpr std::uint16_t ascent = 1900;

/// One sans-serif face: half an em per letter, `i` and `l` narrow, an 8 by
/// 16 point cell.
TypographyFacts syntheticFacts()
{
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.faceCount = 2;
    for (std::size_t index = 0; index < 2; ++index) {
        FaceMetrics& face = facts.faces[index];
        face.familyClass = FamilyClass::SansSerif;
        face.styles = index ? static_cast<std::uint8_t>(StyleBit::Bold) : 0;
        face.ascent = ascent;
        face.descent = lineHeight - ascent;
        face.lineHeight = lineHeight;
        face.averageAdvance = averageAdvance;
        face.advances.fill(letterAdvance);
        face.advances['i' - firstTabledCodePoint] = narrowAdvance;
        face.advances['l' - firstTabledCodePoint] = narrowAdvance;
        face.advances[' ' - firstTabledCodePoint] = letterAdvance / 2;
        if (index)
            face.advances['W' - firstTabledCodePoint] = 2 * letterAdvance;
    }
    return facts;
}

Document read(std::string_view body)
{
    auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    CHECK(document.has_value());
    return document ? std::move(*document) : Document {};
}

/// An advance at 10 points, in layout units.
constexpr std::uint32_t at10(std::uint32_t advance) { return advance * 20; }

void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
}

std::string drained(Ubiquitty::Terminal& t)
{
    std::array<char, 2048> buffer {};
    const int length = UbiquittyTest::drain(t, buffer);
    return std::string(buffer.data(), static_cast<std::size_t>(std::max(length, 0)));
}

// The value of one key in a fit reply, or empty when the reply lacks it.
std::string replyField(const std::string& reply, std::string_view key)
{
    const std::string needle = ":" + std::string(key) + "=";
    const auto at = reply.find(needle);
    if (at == std::string::npos)
        return {};
    const auto start = at + needle.size();
    return reply.substr(start, reply.find_first_of(":\x1b", start) - start);
}

class DenyingMeasurePolicy final : public Ubiquitty::CapabilityPolicy {
public:
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        return capability == Ubiquitty::Capability::TextMeasureRead ? Ubiquitty::CapabilityDecision::Deny : Ubiquitty::CapabilityDecision::Allow;
    }
};

} // namespace

TEST(aClickBetweenTwoNarrowLettersSelectsTheOneItIsOn)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20 il}"), 20, 2, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(layout.rightEdge(0), at10(narrowAdvance));
    CHECK_EQ(layout.rightEdge(1), at10(2 * narrowAdvance));
    // A point on `i`'s last unit is on `i`; the next unit is `l`'s first.
    CHECK_EQ(layout.characterAt(at10(narrowAdvance) - 1, 0), 0u);
    CHECK_EQ(layout.characterAt(at10(narrowAdvance), 0), 1u);
    CHECK_EQ(layout.characterAt(at10(narrowAdvance) + at10(narrowAdvance) / 2, 0), 1u);
    // Past the text is past it; left of it is on the first letter.
    CHECK_EQ(layout.characterAt(at10(5 * narrowAdvance), 0), 2u);
    CHECK_EQ(layout.characterAt(-100, 0), 0u);
}

TEST(aWideGlyphAndAGraphemeSelectAsOne)
{
    const TypographyFacts facts = syntheticFacts();
    // `a`, a wide ideograph (one em, estimated), `e` with a combining acute,
    // and an astral emoji, all encoded directly as UTF-8.
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20 a漢é😀b}"), 40, 2, Overflow::Wrap, Alignment::Left, &facts);
    const std::uint32_t wideLeft = at10(letterAdvance);
    const std::uint32_t wideRight = wideLeft + at10(unitsPerEm);
    CHECK_EQ(layout.rightEdge(1), wideRight);
    CHECK_EQ(layout.characterAt(wideLeft + 1, 0), 1u);
    CHECK_EQ(layout.characterAt(wideRight - 1, 0), 1u);
    // The acute adds no advance and continues `e`'s grapheme: a point on it
    // names `e`, and `e`'s grapheme ends after the mark.
    CHECK(layout.continuesGrapheme(3));
    CHECK_EQ(layout.rightEdge(3), layout.rightEdge(2));
    CHECK_EQ(layout.characterAt(wideRight + 1, 0), 2u);
    CHECK_EQ(layout.graphemeEnd(2), 4u);
    CHECK(!layout.isExact());
}

TEST(theLineIsTheLastShownLineWhoseTopIsAboveThePoint)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20 one\\par two\\par three}"), 20, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(layout.lineCount(), std::size_t { 3 });
    const std::uint32_t height = at10(lineHeight);
    CHECK_EQ(layout.line(1).top, height);
    CHECK_EQ(layout.characterAt(0, height - 1), 0u);
    CHECK_EQ(layout.characterAt(0, height), 4u);
    // Past a line that ends at a line feed stands before the feed.
    CHECK_EQ(layout.characterAt(at10(40 * letterAdvance), height + 1), 7u);
    // Below the last line is on the last line.
    CHECK_EQ(layout.characterAt(0, 100 * height), 8u);
}

TEST(wrappingBreaksAfterTheLastSpaceThatFitsAndSplitsAWordWiderThanTheBlock)
{
    const TypographyFacts facts = syntheticFacts();
    // Ten cells of eight points is eighty points; a letter at 10 points is
    // five, so sixteen letters fit a line.
    const Layout words = Layout::lay(read("{\\rtf1\\fs20 aaaaaaaaaa aaaaaaaaaa}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(words.lineCount(), std::size_t { 2 });
    CHECK_EQ(words.line(0).end, 11u);
    CHECK_EQ(words.line(0).width, at10(10 * letterAdvance));
    const Layout word = Layout::lay(read("{\\rtf1\\fs20 aaaaaaaaaaaaaaaaaaaa}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(word.lineCount(), std::size_t { 2 });
    CHECK_EQ(word.line(0).end, 16u);
    CHECK(word.isFitting());
}

TEST(mixedFacesKeepTheirFullAscentAndDescentInsideTheLine)
{
    TypographyFacts facts = syntheticFacts();
    facts.faces[0].ascent = 1900;
    facts.faces[0].descent = 148;
    facts.faces[0].lineHeight = 2048;
    facts.faces[1].ascent = 1024;
    facts.faces[1].descent = 1024;
    facts.faces[1].lineHeight = 2048;
    const Document document = read("{\\rtf1\\fs40 A\\b B\\b0\\par C}");
    const Layout layout = Layout::lay(document, 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(layout.lineCount() == 2);
    CHECK_EQ(layout.line(0).baseline, 1900u * 40);
    CHECK_EQ(layout.line(0).height, (1900u + 1024) * 40);
    CHECK_EQ(layout.line(1).top, layout.line(0).height);
    CHECK_EQ(layout.line(1).height, 2048u * 40);

    // Positive leading stays below the baseline; a short advertised line
    // height must not cut off the face's actual descent.
    facts.faces[1].lineHeight = 2300;
    const Layout leading = Layout::lay(document, 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(leading.line(0).height, (1900u + 2300 - 1024) * 40);
    facts.faces[1].lineHeight = 1800;
    const Layout shortMetrics = Layout::lay(document, 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(shortMetrics.line(0).height, (1900u + 1024) * 40);
    const Layout capacity = Layout::lay(read("{\\rtf1\\fs40\\b AB}"), 10, 10, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(capacity.line(0).height, 2048u * 40);
    CHECK_EQ(capacity.linesThatFit(), 8u);
}

// The paragraph's line spacing sets each line's box from its natural height:
// exactly, at least, or a multiple of it, the baseline keeping its share.
TEST(lineSpacingSetsTheLineBox)
{
    const TypographyFacts facts = syntheticFacts();
    // A natural ten-point line: 2400/2048 em, 48000 units, its baseline 38000.
    constexpr std::uint32_t natural = lineHeight * 20;
    constexpr std::uint32_t naturalBaseline = ascent * 20;
    constexpr std::uint32_t twentyFourPoints = 24 * 4096;
    const Layout exact = Layout::lay(read("{\\rtf1\\fs20\\sl-480 a\\line b}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(exact.lineCount() == 2);
    CHECK_EQ(exact.line(0).height, twentyFourPoints);
    CHECK_EQ(exact.line(0).baseline, naturalBaseline * twentyFourPoints / natural);
    CHECK_EQ(exact.line(1).top, twentyFourPoints);
    const Layout least = Layout::lay(read("{\\rtf1\\fs20\\sl480 a}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(least.line(0).height, twentyFourPoints);
    const Layout tall = Layout::lay(read("{\\rtf1\\fs100\\sl480 a}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(tall.line(0).height, lineHeight * 100u);
    const Layout multiple = Layout::lay(read("{\\rtf1\\fs20\\sl360\\slmult1 a}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(multiple.line(0).height, natural * 360 / 240);
}

// The space before a paragraph sits inside its first line's box and the space
// after inside its last, so the boxes tile; a line break keeps the paragraph.
TEST(paragraphSpacingStacksInsideTheFirstAndLastLines)
{
    const TypographyFacts facts = syntheticFacts();
    constexpr std::uint32_t natural = lineHeight * 20;
    constexpr std::uint32_t twelvePoints = 12 * 4096;
    constexpr std::uint32_t sixPoints = 6 * 4096;
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20\\sb240\\sa120 a\\line b\\par\\pard c}"), 10, 8, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(layout.lineCount() == 3);
    CHECK_EQ(layout.line(0).height, natural + twelvePoints);
    CHECK_EQ(layout.line(0).baseline, ascent * 20u + twelvePoints);
    CHECK_EQ(layout.line(1).top, natural + twelvePoints);
    CHECK_EQ(layout.line(1).height, natural + sixPoints);
    CHECK_EQ(layout.line(2).top, 2 * natural + twelvePoints + sixPoints);
    CHECK_EQ(layout.line(2).height, natural);
}

// Every line starts at the left indent and the first at its own indent from
// it, which a negative one hangs; a line wraps at the width its indents leave.
TEST(indentsPlaceAndWrapEachLine)
{
    const TypographyFacts facts = syntheticFacts();
    // Eighty points wide; a twenty-point left indent, a first line hanging ten
    // points left of it, and a ten-point right indent. The first line wraps
    // at sixty points, the second at fifty.
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20\\li400\\fi-200\\ri200 aaaa aaaa aaaa}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(layout.lineCount() == 2);
    CHECK_EQ(layout.line(0).end, 10u);
    CHECK_EQ(layout.line(0).origin, 10u * 4096);
    CHECK_EQ(layout.line(1).origin, 20u * 4096);
    // Right alignment takes the free width the indents leave.
    const Layout right = Layout::lay(read("{\\rtf1\\fs20\\qr\\ri200 aaaa}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(right.line(0).origin, 50u * 4096);
}

// A hanging first line's first tab stops at the left indent, where the
// paragraph's other lines start, as a bullet's text is set.
TEST(aHangingFirstLinesTabStopsAtTheLeftIndent)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20\\li400\\fi-400 \\bullet\\tab x}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(layout.lineCount() == 1);
    CHECK_EQ(layout.line(0).origin, 0u);
    CHECK_EQ(layout.rightEdge(1), 20u * 4096);
}

// The fit reply counts the spacing in h; c is read in the width the first
// line's indents leave. Terminal lines keep one row each.
TEST(theFitReplyCountsSpacingAndIndents)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=8;{\\rtf1\\fs20\\sb240 a\\par b}\x1b\\");
    // Both paragraphs take the twelve points before them: twice 48000 + 49152
    // units stand 47.4 points, three sixteen-point rows.
    CHECK_STR_EQ(replyField(drained(t), "h"), "3");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=8:g=1;{\\rtf1\\fs20\\sb240 a\\par b}\x1b\\");
    CHECK_STR_EQ(replyField(drained(t), "h"), "2");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=1;{\\rtf1\\fs20\\li800 a}\x1b\\");
    // Forty points left over 4.88-point average advances.
    CHECK_STR_EQ(replyField(drained(t), "c"), "8");
}

// Padding insets the content box: lines wrap at its width and sit inside it.
TEST(paddingInsetsTheContentBox)
{
    const TypographyFacts facts = syntheticFacts();
    constexpr std::uint32_t point = 4096;
    // Eighty points less ten on each side: sixty, so one ten-letter word of
    // fifty points a line.
    const Layout padded = Layout::lay(read("{\\rtf1\\fs20 aaaaaaaaaa aaaaaaaaaa}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts,
        LineMode::Natural, nullptr, Insets { .top = 10, .right = 10, .bottom = 10, .left = 10 });
    REQUIRE(padded.lineCount() == 2);
    CHECK_EQ(padded.line(0).end, 11u);
    CHECK_EQ(padded.line(0).origin, 10 * point);
    CHECK_EQ(padded.line(0).top, 10 * point);
    CHECK_EQ(padded.blockWidth(), 70 * point);
    // Four sides, in the order top, right, bottom, left.
    const Layout sides = Layout::lay(read("{\\rtf1\\fs20 a}"), 10, 4, Overflow::Wrap, Alignment::Right, &facts,
        LineMode::Natural, nullptr, Insets { .top = 2, .right = 4, .bottom = 6, .left = 8 });
    CHECK_EQ(sides.line(0).top, 2 * point);
    // Right-aligned in the 68 points left between the side insets.
    CHECK_EQ(sides.line(0).origin, 8 * point + 68 * point - at10(letterAdvance));
    // Without a table the layout counts in cells, and points do not apply.
    const Layout cells = Layout::lay(read("{\\rtf1 a}"), 10, 4, Overflow::Wrap, Alignment::Left, nullptr,
        LineMode::Natural, nullptr, Insets { .top = 10, .right = 10, .bottom = 10, .left = 10 });
    CHECK_EQ(cells.line(0).origin, 0u);
}

// Content that needs fewer rows than the block covers sits at the top, the
// centre or the bottom of its content box.
TEST(verticalPlacementSetsTheContentInTheSlack)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    constexpr std::uint32_t natural = lineHeight * 20;
    constexpr std::uint32_t rows = 4 * 16 * 4096;
    const auto topOf = [&](std::string_view keys) {
        UbiquittyTest::feed(t, "\x1b[H\x1b]7404;i=1:w=10:h=4:" + std::string(keys) + ";{\\rtf1\\fs20 a}\x1b\\");
        const auto& blocks = t.richTextBlocks.blocks(activeScreen(t));
        CHECK(blocks.size() == 1);
        return blocks.empty() ? 0u : layoutOf(t, blocks.front()).line(0).top;
    };
    CHECK_EQ(topOf("y=t"), 0u);
    CHECK_EQ(topOf("y=c"), (rows - natural) / 2);
    CHECK_EQ(topOf("y=b"), rows - natural);
    // Inside four points of padding on every side.
    CHECK_EQ(topOf("y=b:p=4"), 4u * 4096 + (rows - 8u * 4096 - natural));
}

// A fit reply names the rectangle, its padding included.
TEST(theFitReplyCountsThePadding)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=4:p=8;{\\rtf1\\fs20 aaaa}\x1b\\");
    const std::string reply = drained(t);
    // Twenty points of text and sixteen of padding: five cells of eight. A
    // 11.72-point line and sixteen points stand two sixteen-point rows.
    CHECK_STR_EQ(replyField(reply, "w"), "5");
    CHECK_STR_EQ(replyField(reply, "h"), "2");
    CHECK_STR_EQ(replyField(reply, "m"), "5");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=4:y=b;{\\rtf1\\fs20 aaaa}\x1b\\");
    const std::string placed = drained(t);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=4;{\\rtf1\\fs20 aaaa}\x1b\\");
    CHECK_STR_EQ(placed, drained(t));
}

// Padding names one inset or four, each at most 255 points; vertical
// placement one of three letters; a delete takes neither.
TEST(paddingAndPlacementValuesAreChecked)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    for (const std::string_view keys : { "p=256", "p=1,2,3", "p=1,2,3,4,5", "p=1,,3,4", "y=x", "y=" }) {
        UbiquittyTest::feed(t, "\x1b[H\x1b]7404;i=7:e=1:w=10:h=2:" + std::string(keys) + ";{\\rtf1 a}\x1b\\");
        CHECK_MSG(drained(t) == "\x1b]7404;a=e:i=7:v=3:s=invalid\x1b\\", std::string(keys).c_str());
        CHECK(t.richTextBlocks.isEmpty());
    }
    UbiquittyTest::feed(t, "\x1b[H\x1b]7404;i=7:w=10:h=2:p=1,2,3,4:y=c;{\\rtf1 a}\x1b\\");
    const auto& blocks = t.richTextBlocks.blocks(activeScreen(t));
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front().padding == (Insets { .top = 1, .right = 2, .bottom = 3, .left = 4 }));
    CHECK(blocks.front().verticalPlacement == VerticalPlacement::Center);
    UbiquittyTest::feed(t, "\x1b]7404;a=d:i=7:p=1\x1b\\");
    CHECK(!t.richTextBlocks.isEmpty());
}

TEST(clipKeepsALineWholeAndDoesNotFit)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20 aaaaaaaaaaaaaaaaaaaa}"), 10, 4, Overflow::Clip, Alignment::Left, &facts);
    CHECK_EQ(layout.lineCount(), std::size_t { 1 });
    CHECK(!layout.isFitting());
    CHECK_EQ(layout.shownCount(), 16u);
    CHECK_EQ(layout.copyableEnd(), 20u);
}

TEST(anEllipsisCutsTheLastShownLine)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read("{\\rtf1\\fs20 aaaa aaaa aaaa aaaa aaaa}"), 10, 1, Overflow::Ellipsis, Alignment::Left, &facts);
    CHECK(layout.lineCount() > 1);
    CHECK_EQ(layout.visibleLineCount(), std::size_t { 1 });
    const LineLayout line = layout.line(0);
    CHECK_EQ(line.ellipsisAdvance, at10(averageAdvance));
    CHECK(line.width <= layout.blockWidth());
    CHECK_EQ(layout.copyableEnd(), line.end);
    CHECK(!layout.isFitting());
}

TEST(nativeEllipsisNeverExtendsAMaximumSourcePastItsGeometryEnvelope)
{
    struct FontModel {
        std::size_t largestSource = 0;
        std::size_t calls = 0;
        bool shouldJoinFinalPair = false;
    } model;
    const Shaper shaper { [](void* context, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                             auto& model = *static_cast<FontModel*>(context);
                             model.largestSource = std::max(model.largestSource, document->text.size());
                             ++model.calls;
                             const auto count = static_cast<std::uint32_t>(document->text.size());
                             const auto height = static_cast<std::uint32_t>(facts->cellHeight) * 64;
                             for (std::uint32_t first = 0, row = 0; first < count; ++row) {
                                 const auto end = model.shouldJoinFinalPair && first + 2 == count ? count : first + 1;
                                 const auto width = (end - first) * 4096;
                                 output->appendLine({ first, end, row * height, height, static_cast<std::uint32_t>(facts->baseline) * 64, 0, width });
                                 output->appendSpan({ first, end, row, 0, static_cast<std::int32_t>(width) });
                                 first = end;
                             }
                             return true;
                         },
        &model };
    const auto document = read("{\\rtf1 " + std::string(maxCodePoints, 'a') + "}");
    auto facts = syntheticFacts();
    facts.baseline = 12 * 64;
    const Insets exhausted { .top = 16 };

    // Padding consumes the final viewport row. EOF leaves no source suffix
    // to replace, so native shaping must stop after the original document.
    const auto atEnd = Layout::lay(document, 1, 1, Overflow::Ellipsis, Alignment::Left, &facts, LineMode::Terminal, &shaper, exhausted, maxCodePoints);
    CHECK(!atEnd.isShaped());
    CHECK_EQ(model.calls, std::size_t { 1 });
    CHECK_EQ(model.largestSource, std::size_t { maxCodePoints });

    model = {};
    const auto beforeEnd = Layout::lay(document, 1, 1, Overflow::Ellipsis, Alignment::Left, &facts, LineMode::Terminal, &shaper, exhausted, maxCodePoints - 1);
    CHECK(beforeEnd.isShaped());
    CHECK(beforeEnd.isElided());
    CHECK(model.calls > 1);
    CHECK_EQ(model.largestSource, std::size_t { maxCodePoints });

    model = { .shouldJoinFinalPair = true };
    const auto combiningDocument = read("{\\rtf1 " + std::string(maxCodePoints - 1, 'a') + "́}");
    const auto insideFinalGrapheme = Layout::lay(combiningDocument, 1, 1, Overflow::Ellipsis, Alignment::Left, &facts, LineMode::Terminal, &shaper, exhausted, maxCodePoints - 1);
    CHECK(!insideFinalGrapheme.isShaped());
    CHECK_EQ(model.calls, std::size_t { 1 });
    CHECK_EQ(model.largestSource, std::size_t { maxCodePoints });

    model = {};
    const auto fittingToken = Layout::lay(document, 1, 1, Overflow::Ellipsis, Alignment::Left, &facts, LineMode::Terminal, &shaper, {}, maxCodePoints - 2);
    CHECK(fittingToken.isShaped());
    CHECK(fittingToken.isElided());
    CHECK_EQ(fittingToken.copyableEnd(), std::size_t { maxCodePoints - 2 });
    CHECK_EQ(model.largestSource, std::size_t { maxCodePoints });
}

TEST(anUnderlineIsOneRuleAcrossItsLettersAndBreaksOnlyAtItsStyleAndLine)
{
    const TypographyFacts facts = syntheticFacts();
    Block block;
    block.document = read("{\\rtf1\\fs20 a\\ul Underlined\\ulnone , \\strike struck\\strike0  and \\ul\\strike both\\ulnone\\strike0 .}");
    const Layout layout = Layout::lay(block.document, 40, 2, Overflow::Clip, Alignment::Left, &facts);
    const BlockDrawing drawing = BlockDrawing::draw(block, layout, &facts);
    REQUIRE(drawing.rules().size() == 3);

    // Each rule spans exactly its decorated graphemes, edge to edge.
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> spans { { { 1, 11 }, { 13, 19 }, { 24, 28 } } };
    for (std::size_t index = 0; index < spans.size(); ++index) {
        const DrawnRule& rule = drawing.rules()[index];
        const auto [first, end] = spans[index];
        CHECK_EQ(rule.x, layout.leftEdge(layout.line(0), first));
        CHECK_EQ(rule.x + rule.width, layout.rightEdge(end - 1));
        CHECK_EQ(rule.baseline, layout.line(0).top + layout.line(0).baseline);
        const auto decoration = index == 1 ? StyleBit::Strikethrough : StyleBit::Underline;
        CHECK((drawing.styles()[rule.style].styles & std::to_underlying(decoration)) != 0);
    }

    // Composited antialiased at a scale that puts letter boundaries inside
    // device pixels, no pixel between the first underlined letter and the
    // last is left partly uncovered: the seam a rule per letter leaves.
    constexpr double pixelsPerUnit = 1.37 / 4096;
    const DrawnRule& last = drawing.rules().back();
    std::vector<double> coverage(static_cast<std::size_t>(std::ceil((last.x + last.width) * pixelsPerUnit)) + 1, 0);
    for (const DrawnRule& drawn : drawing.rules()) {
        const double left = drawn.x * pixelsPerUnit;
        const double right = (drawn.x + drawn.width) * pixelsPerUnit;
        for (std::size_t pixel = 0; pixel < coverage.size(); ++pixel) {
            const double alpha = std::clamp(std::min(right, pixel + 1.0) - std::max(left, static_cast<double>(pixel)), 0.0, 1.0);
            coverage[pixel] = 1 - (1 - coverage[pixel]) * (1 - alpha);
        }
    }
    const auto firstLetterCentre = static_cast<std::size_t>((layout.leftEdge(layout.line(0), 1) + layout.rightEdge(1)) / 2 * pixelsPerUnit);
    const auto lastLetterCentre = static_cast<std::size_t>((layout.leftEdge(layout.line(0), 10) + layout.rightEdge(10)) / 2 * pixelsPerUnit);
    for (std::size_t pixel = firstLetterCentre; pixel <= lastLetterCentre; ++pixel)
        CHECK_EQ(coverage[pixel], 1.0);

    // A wrapped stretch is one rule per line.
    block.document = read("{\\rtf1\\fs20\\ul aaaa aaaa aaaa\\ulnone}");
    const Layout wrapped = Layout::lay(block.document, 8, 4, Overflow::Wrap, Alignment::Left, &facts);
    REQUIRE(wrapped.lineCount() > 1);
    const BlockDrawing wrappedDrawing = BlockDrawing::draw(block, wrapped, &facts);
    CHECK_EQ(wrappedDrawing.rules().size(), wrapped.lineCount());
    for (std::size_t index = 0; index < wrappedDrawing.rules().size(); ++index)
        CHECK_EQ(wrappedDrawing.rules()[index].baseline, wrapped.line(index).top + wrapped.line(index).baseline);
}

TEST(alignmentOffsetsTheLineByItsFreeWidth)
{
    const TypographyFacts facts = syntheticFacts();
    const Layout centred = Layout::lay(read("{\\rtf1\\fs20\\qc aaaa\\par\\pard\\qr bb}"), 10, 4, Overflow::Wrap, Alignment::Left, &facts);
    const std::uint32_t free = centred.blockWidth() - at10(4 * letterAdvance);
    CHECK_EQ(centred.line(0).origin, free / 2);
    CHECK_EQ(centred.line(1).origin, centred.blockWidth() - at10(2 * letterAdvance));
    // A paragraph naming no alignment takes the block's.
    const Layout right = Layout::lay(read("{\\rtf1\\fs20 aa}"), 10, 4, Overflow::Wrap, Alignment::Right, &facts);
    CHECK_EQ(right.line(0).origin, right.blockWidth() - at10(2 * letterAdvance));
    // A point left of an aligned line is on its first character.
    CHECK_EQ(right.characterAt(0, 0), 0u);
}

TEST(aNamedFamilyResolvesToItsClassFaceAndBoldToTheBoldFace)
{
    const TypographyFacts facts = syntheticFacts();
    const Document document = read("{\\rtf1{\\fonttbl{\\f0\\fswiss Helvetica;}}\\f0\\fs20\\b W}");
    CHECK_EQ(resolveFace(facts, document, document.runs[0]), std::size_t { 1 });
    const Layout layout = Layout::lay(document, 10, 2, Overflow::Wrap, Alignment::Left, &facts);
    CHECK_EQ(layout.rightEdge(0), at10(2 * letterAdvance));
}

TEST(theQueryAnswersFromThePublishedTable)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=9:w=10:h=2;{\\rtf1\\fs20 aaaa}\x1b\\");
    // Four letters of five points: twenty points, three cells of eight. One
    // line of twelve points in a sixteen-point row. Sixteen average advances
    // and two lines fit 80 by 32 points.
    CHECK(drained(t) == "\x1b]7404;a=r:i=9:f=1:w=3:h=1:n=4:c=16:l=2:m=3:x=1:v=3:W=512:H=1024:B=0:S=0:E=1:L=750:A=594:Q=0:K=0:F=0:V=1:N=0:T=2000:Z=body=16384,scalars=4096,runs=512,blocks=256,pending=32,rectangles=32,carets=128,fonts=16,colors=16,depth=32,family=255,width=1024,height=1024,open-width=1024,open-height=5120,retained-bytes=1048576,retained-used=0,catalog-count=255,catalog-bytes=16212:R=4\x1b\\");
    CHECK(t.richTextBlocks.isEmpty());
}

TEST(aQueryThatDoesNotFitSaysSoAndWhatItNeeds)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=1;{\\rtf1\\fs20 aaaaaaaaaa aaaaaaaaaa aaaa}\x1b\\");
    // The second line is the widest: fourteen letters and a space, 72.5 points.
    CHECK(drained(t) == "\x1b]7404;a=r:i=0:f=0:w=10:h=2:n=11:c=16:l=1:m=7:x=1:v=3:W=512:H=1024:B=0:S=0:E=1:L=750:A=594:Q=0:K=0:F=0:V=1:N=0:T=2000:Z=body=16384,scalars=4096,runs=512,blocks=256,pending=32,rectangles=32,carets=128,fonts=16,colors=16,depth=32,family=255,width=1024,height=1024,open-width=1024,open-height=5120,retained-bytes=1048576,retained-used=0,catalog-count=255,catalog-bytes=16212:R=11,26\x1b\\");
}

TEST(aClippedLineLongerThanTheLayoutsRangeShowsNothingPastTheEdge)
{
    // 4096 letters at 256 points end exactly 2^31 layout units along the
    // line, past what a character's edge word holds. Not one of them fits a
    // ten-cell block, and no edge may wrap back inside it or read as a
    // grapheme's continuation.
    std::string body = "{\\rtf1\\fs512 ";
    body.append(4096, 'a');
    body += "}";
    const TypographyFacts facts = syntheticFacts();
    const Layout layout = Layout::lay(read(body), 10, 1, Overflow::Clip, Alignment::Left, &facts);
    CHECK_EQ(layout.shownCount(), 0u);
    CHECK(!layout.isFitting());
    CHECK(!layout.continuesGrapheme(4095));
    CHECK(layout.rightEdge(4095) >= layout.rightEdge(4094));
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, facts));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:o=c:w=10:h=1;" + body + "\x1b\\");
    const std::string reply = drained(t);
    CHECK_STR_EQ(replyField(reply, "f"), "0");
    CHECK_STR_EQ(replyField(reply, "n"), "0");
}

// An open width measures the body unwrapped: the reply's width is the widest
// paragraph, and the counts describe the rectangle that width resolves to.
TEST(anOpenWidthAnswersTheIdealWidth)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=0:h=1;{\\rtf1\\fs20 aaaaaaaaaa aaaaaaaaaa aaaa}\x1b\\");
    const std::string reply = drained(t);
    // Twenty-four letters of five points and two spaces of 2.5: 125 points,
    // sixteen cells of eight, on one line. Sixteen cells are 128 points, 26
    // average advances of 4.88 points.
    CHECK_STR_EQ(replyField(reply, "w"), "16");
    CHECK_STR_EQ(replyField(reply, "h"), "1");
    CHECK_STR_EQ(replyField(reply, "f"), "1");
    CHECK_STR_EQ(replyField(reply, "n"), "26");
    CHECK_STR_EQ(replyField(reply, "c"), "26");
    CHECK_STR_EQ(replyField(reply, "R"), "26");
}

// An open height wraps at the width and shows every line: the reply's height
// is the rows they need, and nothing is cut.
TEST(anOpenHeightAnswersTheRowsTheWrappedTextNeeds)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=0:o=e;{\\rtf1\\fs20 aaaaaaaaaa aaaaaaaaaa aaaa aaaaaaaaaa}\x1b\\");
    const std::string reply = drained(t);
    // Three twelve-point lines stand 36 points: three rows of sixteen.
    CHECK_STR_EQ(replyField(reply, "h"), "3");
    CHECK_STR_EQ(replyField(reply, "f"), "1");
    CHECK_STR_EQ(replyField(reply, "n"), "37");
    CHECK_STR_EQ(replyField(reply, "l"), "4");
    CHECK_STR_EQ(replyField(reply, "R"), "11,27,37");
}

TEST(bothDimensionsOpenAnswerOneLinePerParagraph)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=0:h=0;{\\rtf1\\fs20 aaaa\\par aaaaaaaa}\x1b\\");
    const std::string reply = drained(t);
    CHECK_STR_EQ(replyField(reply, "w"), "5");
    CHECK_STR_EQ(replyField(reply, "h"), "2");
    CHECK_STR_EQ(replyField(reply, "f"), "1");
    CHECK_STR_EQ(replyField(reply, "R"), "5,13");
}

// Every reply names the narrowest width that breaks no word: the widest run
// between two break opportunities, whatever the query's own width.
TEST(aReplyNamesTheWidestWord)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=4:h=1;{\\rtf1\\fs20 aaa aaaaaaaaaa aa}\x1b\\");
    // Ten letters of five points: fifty points, seven cells of eight.
    CHECK_STR_EQ(replyField(drained(t), "m"), "7");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=0:h=0;{\\rtf1\\fs20 aaa aaaaaaaaaa aa}\x1b\\");
    CHECK_STR_EQ(replyField(drained(t), "m"), "7");
}

// A wide character is a word of its own: a line may break on either side.
TEST(aWideCharacterIsAWordOfItsOwn)
{
    Ubiquitty::Terminal t;
    reset(t);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=2;{\\rtf1 日本語}\x1b\\");
    const std::string reply = drained(t);
    // Without a table a wide character takes two cells.
    CHECK_STR_EQ(replyField(reply, "w"), "6");
    CHECK_STR_EQ(replyField(reply, "m"), "2");
}

// Only a query may leave a dimension open: a placement names its rectangle.
TEST(aPlacementOfNoWidthOrHeightIsRefused)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;a=p:i=1:w=0:h=1;{\\rtf1 a}\x1b\\");
    UbiquittyTest::feed(t, "\x1b]7404;a=p:i=2:w=4:h=0;{\\rtf1 a}\x1b\\");
    CHECK(t.richTextBlocks.isEmpty());
    UbiquittyTest::feed(t, "\x1b]7404;a=p:i=3:w=4:h=1:e=1;{\\rtf1 a}\x1b\\");
    CHECK(!t.richTextBlocks.isEmpty());
}

TEST(aQueryWithNoTableAnswersInCellsAndSaysItIsAnEstimate)
{
    Ubiquitty::Terminal t;
    reset(t);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=2;{\\rtf1\\fs40 aaaa}\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=r:i=0:f=1:w=4:h=1:n=4:c=10:l=2:m=4:x=0:v=3:W=0:H=0:B=0:S=0:E=0:L=64:A=51:Q=0:K=0:F=0:V=1:N=0:T=2000:Z=body=16384,scalars=4096,runs=512,blocks=256,pending=32,rectangles=32,carets=128,fonts=16,colors=16,depth=32,family=255,width=1024,height=1024,open-width=1024,open-height=5120,retained-bytes=1048576,retained-used=0,catalog-count=255,catalog-bytes=16212:R=4\x1b\\");
}

TEST(anEmptyQueryIsTheFeatureTest)
{
    Ubiquitty::Terminal t;
    reset(t);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=4:h=1\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=r:i=0:f=1:w=0:h=0:n=0:c=4:l=1:m=0:x=0:v=3:W=0:H=0:B=0:S=0:E=0:L=0:A=0:Q=0:K=0:F=0:V=1:N=0:T=2000:Z=body=16384,scalars=4096,runs=512,blocks=256,pending=32,rectangles=32,carets=128,fonts=16,colors=16,depth=32,family=255,width=1024,height=1024,open-width=1024,open-height=5120,retained-bytes=1048576,retained-used=0,catalog-count=255,catalog-bytes=16212:R=\x1b\\");
}

TEST(aDeniedOriginGetsNoReply)
{
    Ubiquitty::Terminal t;
    reset(t);
    DenyingMeasurePolicy policy;
    t.capabilityPolicy = &policy;
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=4:h=1;{\\rtf1 x}\x1b\\");
    t.capabilityPolicy = nullptr;
    CHECK(drained(t).empty());
}

TEST(aReplicaAnswersNothingBecauseItsHostDoes)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.replyAuthority = Ubiquitty::ReplyAuthority::Elsewhere;
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=4:h=1;{\\rtf1 x}\x1b\\");
    CHECK(drained(t).empty());
}

TEST(aLayoutIsBuiltOnceAndAgainOnlyWhenTheTableMoves)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;w=20:h=3;{\\rtf1\\fs20 Title\\par body text}\x1b\\");
    const Block& block = t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary)[0];
    const Layout& first = layoutOf(t, block);
    for (int pass = 0; pass < 100; ++pass)
        (void)layoutOf(t, block);
    CHECK_EQ(t.richTextLayouts.buildCount(), std::uint64_t { 1 });
    // One allocation, sized once: every character's edge and nine words a line.
    CHECK_EQ(first.storageCapacity(), block.document.text.size() + first.lineCount() * 9);
    // An equal table moves nothing; another one rebuilds.
    CHECK(adoptTypography(t, syntheticFacts()));
    (void)layoutOf(t, block);
    CHECK_EQ(t.richTextLayouts.buildCount(), std::uint64_t { 1 });
    TypographyFacts wider = syntheticFacts();
    wider.cellWidth = 9 * 64;
    CHECK(adoptTypography(t, wider));
    (void)layoutOf(t, block);
    CHECK_EQ(t.richTextLayouts.buildCount(), std::uint64_t { 2 });
}

TEST(aTableThatIsNotWellFormedIsRefused)
{
    Ubiquitty::Terminal t;
    reset(t);
    TypographyFacts facts = syntheticFacts();
    facts.faces[0].lineHeight = 0;
    CHECK(!adoptTypography(t, facts));
    CHECK(!t.typography.facts);
    facts = syntheticFacts();
    facts.cellWidth = maxCellSize + 1;
    CHECK(!adoptTypography(t, facts));
}

TEST(terminalLinesFollowZoomAndUseThePublishedBaseline)
{
    TypographyFacts facts = syntheticFacts();
    facts.baseline = 13 * 64;
    facts.proportionalSize = 12 * 64;
    const Document document = read("{\\rtf1\\fs24 iii\\line www}");
    const Layout layout = Layout::lay(document, 20, 2, Overflow::Clip, Alignment::Left, &facts, LineMode::Terminal);
    CHECK_EQ(layout.line(0).height, 16u * 4096);
    CHECK_EQ(layout.line(1).top, 16u * 4096);
    CHECK_EQ(layout.line(0).baseline, 13u * 4096);
    CHECK_EQ(layout.linesThatFit(), 2u);
    facts.proportionalSize = 18 * 64;
    facts.cellWidth = 12 * 64;
    facts.cellHeight = 24 * 64;
    facts.baseline = 19 * 64;
    const Layout zoomed = Layout::lay(document, 20, 2, Overflow::Clip, Alignment::Left, &facts, LineMode::Terminal);
    CHECK_EQ(zoomed.rightEdge(2), layout.rightEdge(2) * 3 / 2);
    CHECK_EQ(zoomed.line(0).baseline, 19u * 4096);
    CHECK_EQ(zoomed.line(1).top, 24u * 4096);
}

TEST(terminalMonospaceUsesTheGridSizeIndependentlyOfProse)
{
    TypographyFacts facts = syntheticFacts();
    facts.proportionalSize = 20 * 64;
    facts.monospaceSize = 16 * 64;
    facts.faces[2] = facts.faces[0];
    facts.faces[2].familyClass = FamilyClass::Monospace;
    facts.faceCount = 3;
    Block block;
    block.lineMode = LineMode::Terminal;
    block.document = read("{\\rtf1{\\fonttbl{\\f0\\fswiss ;}{\\f1\\fmodern ;}}\\fs24\\f0 a{\\f1 b}c}");
    const auto layout = Layout::lay(block.document, 20, 2, Overflow::Clip, Alignment::Left, &facts, block.lineMode);
    CHECK_EQ(layout.rightEdge(0), 10u * 4096);
    CHECK_EQ(layout.rightEdge(1) - layout.rightEdge(0), facts.cellWidth * 64u);
    CHECK_EQ(layout.rightEdge(2) - layout.rightEdge(1), 10u * 4096);
    const auto drawing = BlockDrawing::draw(block, layout, &facts);
    REQUIRE(drawing.styles().size() == 3);
    CHECK_EQ(drawing.styles()[0].halfPoints, 40.0f);
    CHECK_EQ(drawing.styles()[1].halfPoints, 32.0f);
    CHECK_EQ(drawing.styles()[2].halfPoints, 40.0f);

    facts.proportionalSize = 24 * 64;
    const auto largerProse = Layout::lay(block.document, 20, 2, Overflow::Clip, Alignment::Left, &facts, block.lineMode);
    CHECK_EQ(largerProse.rightEdge(0), 12u * 4096);
    CHECK_EQ(largerProse.rightEdge(1) - largerProse.rightEdge(0), 8u * 4096);
    facts.monospaceSize = 20 * 64;
    const auto largerGrid = Layout::lay(block.document, 20, 2, Overflow::Clip, Alignment::Left, &facts, block.lineMode);
    CHECK_EQ(largerGrid.rightEdge(1) - largerGrid.rightEdge(0), 10u * 4096);

    block.lineMode = LineMode::Natural;
    const auto natural = Layout::lay(block.document, 20, 2, Overflow::Clip, Alignment::Left, &facts, block.lineMode);
    const auto naturalDrawing = BlockDrawing::draw(block, natural, &facts);
    CHECK_EQ(natural.rightEdge(1) - natural.rightEdge(0), 6u * 4096);
    CHECK_EQ(naturalDrawing.styles()[1].halfPoints, 24.0f);
    facts.monospaceSize = maxCellSize + 1;
    CHECK(!isAdoptable(facts));
}

TEST(typographyEpochGuardsQueryAndPlacementWithoutMutatingOnStale)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(adoptTypography(t, syntheticFacts()));
    UbiquittyTest::feed(t, "\x1b]7404;w=10:h=1:i=17:C=1;{\\rtf1 original}\x1b\\");
    const auto serial = t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).front().serial;
    const auto previousEpoch = t.typography.epoch;
    adoptTypography(t, t.typography.facts, TypographyAuthority::Changed);
    CHECK_EQ(t.typography.epoch, previousEpoch + 1);
    const auto old = std::to_string(previousEpoch);
    UbiquittyTest::feed(t, "\x1b]7404;w=10:h=1:i=17:e=1:E=" + old + ";{\\rtf1 replacement}\x1b\\");
    CHECK(drained(t).find(":s=stale") != std::string::npos);
    CHECK_EQ(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).front().serial, serial);
    CHECK_EQ(t.screens.active->cursorY, 0);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=1:i=18:e=1:E=" + old + ";{\\rtf1 query}\x1b\\");
    CHECK(drained(t).find(":s=stale") != std::string::npos);
    CHECK(t.richTextMeasurements.empty());
    const auto current = std::to_string(t.typography.epoch);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:w=10:h=1:i=19:e=1:E=" + current + ";{\\rtf1 query}\x1b\\");
    CHECK(drained(t).find("a=r:i=19:") != std::string::npos);
    UbiquittyTest::feed(t, "\x1b]7404;w=10:h=1:i=17:e=1:E=" + current + ";{\\rtf1 replacement}\x1b\\");
    CHECK(drained(t).find(":s=ok") != std::string::npos);
    CHECK(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).front().serial != serial);
}

TEST(epochKeysHaveAnUnambiguousUnsigned32BitDomain)
{
    using Ubiquitty::OSC::RichTextBlocks::parseKeys;
    const auto maximum = parseKeys("a=q:w=1:h=1:E=4294967295");
    CHECK(maximum && maximum->expectedTypographyEpoch == 4294967295U);
    CHECK(parseKeys("a=p:w=1:h=1:E=0"));
    for (const auto bad : { "", "-1", "4294967296", "00000000000", "1:E=1" })
        CHECK(!parseKeys(std::string("a=q:w=1:h=1:E=") + bad));
    CHECK(!parseKeys("a=d:i=1:E=0"));
    CHECK(!parseKeys("a=s:i=1:N=1:E=0"));
}

UBIQUITTY_TEST_MAIN()

TEST(typographySubscriptionsReportChangesWithoutAResize)
{
    Ubiquitty::Terminal t;
    reset(t);
    auto facts = syntheticFacts();
    CHECK(adoptTypography(t, facts));
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b]7404;a=s:i=7:N=1\x1b\\");
    const auto initial = drained(t);
    CHECK(initial.find("a=n:i=7:v=3:E=1:W=512:H=1024") != std::string::npos);
    CHECK(replyField(initial, "T") == std::to_string(maxMeasurementWaitMilliseconds));
    facts.proportionalSize = 14 * 64;
    CHECK(adoptTypography(t, facts));
    CHECK(drained(t).find(":E=2:W=512:H=1024:B=0:S=896:") != std::string::npos);
    CHECK(adoptTypography(t, facts));
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b]7404;a=s:i=7:N=0\x1b\\");
    facts.proportionalSize = 16 * 64;
    CHECK(adoptTypography(t, facts));
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b[?1049h\x1b]7404;a=s:i=8:N=1\x1b\\");
    CHECK(!drained(t).empty());
    UbiquittyTest::feed(t, "\x1b[?1049l");
    CHECK_EQ(t.typography.subscriptionID, 0);
    UbiquittyTest::feed(t, "\x1b]7404;a=s:i=9:N=1\x1b\\");
    CHECK(!drained(t).empty());
    Ubiquitty::hardReset(t);
    CHECK_EQ(t.typography.subscriptionID, 0);
}

TEST(richTextErrorsAreCorrelatedAndOnlyOptedInto)
{
    Ubiquitty::Terminal t;
    reset(t);
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:w=1025:h=1\x1b\\");
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:e=1:w=1025:h=1\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=invalid\x1b\\");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:e=1:w=1:h=1:z=1\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=unsupported\x1b\\");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:i=5:e=1:w=1025:h=1\x1b\\");
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b]7404;a=p:i=4:e=1:w=81:h=1;{\\rtf1 x}\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=geometry\x1b\\");
    UbiquittyTest::feed(t, "\x1b]7404;a=p:i=4:e=1:w=1:h=1;{\\rtf1 x}\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=ok\x1b\\");
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:e=1:w=1:h=1;{\\rtf1 " + std::string(4097, 'a') + "}\x1b\\");
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=limit\x1b\\");
}

TEST(deniedMeasurementCannotSubscribeOrReadErrors)
{
    Ubiquitty::Terminal t;
    reset(t);
    DenyingMeasurePolicy policy;
    t.capabilityPolicy = &policy;
    UbiquittyTest::feed(t, "\x1b]7404;a=s:i=7:N=1:e=1\x1b\\\x1b]7404;a=q:i=8:e=1:w=0:h=1\x1b\\");
    t.capabilityPolicy = nullptr;
    CHECK_EQ(t.typography.subscriptionID, 0);
    CHECK(drained(t).empty());
    t.replyAuthority = Ubiquitty::ReplyAuthority::Elsewhere;
    UbiquittyTest::feed(t, "\x1b]7404;a=s:i=7:N=1:e=1\x1b\\");
    CHECK_EQ(t.typography.subscriptionID, 0);
    CHECK(drained(t).empty());
}

TEST(caretGeometryUsesNativeGraphemesAndBoundedPackets)
{
    Ubiquitty::Terminal t;
    reset(t);
    auto facts = syntheticFacts();
    facts.shaping = 1;
    CHECK(adoptTypography(t, facts));
    t.typography.shaper = { [](void*, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                               output->lines.push_back({ 0, static_cast<std::uint32_t>(document->text.size()), 0, static_cast<std::uint32_t>(facts->cellHeight) * 64, static_cast<std::uint32_t>(facts->baseline) * 64, 0, 16000 });
                               for (std::uint32_t index = 0; index < document->text.size(); ++index)
                                   output->spans.push_back({ index, index + 1, 0, static_cast<std::int32_t>((index + 1) * 64), static_cast<std::int32_t>(index * 64) });
                               return true;
                           },
        nullptr };
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=9:w=80:h=1:g=1:G=1;e=1\x1b\\");
    CHECK(drained(t).empty());
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=9:w=80:h=1:g=1:G=1;{\\rtf1 " + std::string(256, 'a') + "}\x1b\\");
    std::array<char, 16384> buffer {};
    const auto length = UbiquittyTest::drain(t, buffer);
    const std::string reply(buffer.data(), static_cast<std::size_t>(length));
    CHECK(reply.starts_with("\x1b]7404;a=g:i=9:v=3:E=1:s=0:G=0,1,0,1,0,"));
    CHECK(reply.find("\x1b]7404;a=g:i=9:v=3:E=1:s=128:G=128,129,0,129,128,") != std::string::npos);
    CHECK(reply.find(":Q=1:K=0:F=0:V=1:N=256:T=2000:Z=") != std::string::npos);
    CHECK(reply.ends_with(":R=256\x1b\\"));
}

TEST(delegatedQueriesRequestGeometryAndCancelWhenFontsChange)
{
    Ubiquitty::Terminal t;
    reset(t);
    auto facts = syntheticFacts();
    facts.shaping = 1;
    CHECK(adoptTypography(t, facts));
    UbiquittyTest::feed(t, "\x1b]7404;a=q:i=4:e=1:w=10:h=2:G=1;{\\rtf1 hello}\x1b\\");
    CHECK_EQ(t.richTextMeasurements.size(), std::size_t { 1 });
    CHECK(t.richTextMeasurements.front().request.needsGeometry);
    CHECK(drained(t).empty());
    facts.proportionalSize = 14 * 64;
    CHECK(adoptTypography(t, facts));
    CHECK(t.richTextMeasurements.empty());
    CHECK(drained(t) == "\x1b]7404;a=e:i=4:v=3:s=stale\x1b\\");
}

TEST(delegatedMeasurementsExpireOnceFromAdmissionAcrossClockWrap)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    MeasurementClock clock;
    clock.instant = Ubiquitty::Ticks(0xFFFFFFC0);
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=7:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    CHECK_EQ(measurementWaitMicroseconds(terminal.richTextMeasurements.front(), clock.instant), std::uint64_t { 2'000'000 });
    expireMeasurements(terminal, clock.instant + (maxMeasurementWaitTicks - 1));
    CHECK_EQ(terminal.richTextMeasurements.size(), std::size_t { 1 });
    CHECK(drained(terminal).empty());
    expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=7:v=3:s=timeout\x1b\\");
    expireMeasurements(terminal, clock.instant + (2 * maxMeasurementWaitTicks));
    CHECK(drained(terminal).empty());
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=7:w=10:h=1;{\\rtf1 silent}\x1b\\");
    expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal).empty());
}

TEST(nativeCompletionChecksTheCoreDeadlineAtTheReceiveBoundary)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    for (const auto admission : { 100u, std::numeric_limits<std::uint32_t>::max() - 64 }) {
        for (const auto elapsed : { maxMeasurementWaitTicks - 1, maxMeasurementWaitTicks, maxMeasurementWaitTicks + 1 }) {
            MeasurementClock clock;
            clock.instant = Ubiquitty::Ticks { admission };
            Ubiquitty::Terminal terminal;
            reset(terminal);
            terminal.tickClock = &clock;
            auto facts = syntheticFacts();
            facts.shaping = 1;
            REQUIRE(adoptTypography(terminal, facts));
            UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=7:e=1:w=10:h=1;{\\rtf1 answer}\x1b\\");
            REQUIRE(terminal.richTextMeasurements.size() == 1);
            const auto& pending = terminal.richTextMeasurements.front();
            const auto token = pending.request.token;
            auto measured = measure(pending.request.document, pending.request.shape, terminal.typography);
            clock.instant = clock.instant + elapsed;
            const auto beforeDeadline = elapsed < maxMeasurementWaitTicks;
            CHECK_EQ(completeMeasurement(terminal, token, std::move(measured)), beforeDeadline);
            publishMeasurements(terminal);
            CHECK(terminal.richTextMeasurements.empty());
            const auto reply = drained(terminal);
            if (beforeDeadline)
                CHECK(reply.starts_with("\x1b]7404;a=r:i=7:"));
            else
                CHECK(reply == "\x1b]7404;a=e:i=7:v=3:s=timeout\x1b\\");
            CHECK(!completeMeasurement(terminal, token, {}));
            CHECK(drained(terminal).empty());
            UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=7:e=1:w=10:h=1;{\\rtf1 fresh}\x1b\\");
            REQUIRE(terminal.richTextMeasurements.size() == 1);
            CHECK(terminal.richTextMeasurements.front().request.token != token);
            CHECK(!completeMeasurement(terminal, token, {}));
            CHECK(terminal.richTextMeasurements.size() == 1);
            CHECK(drained(terminal).empty());
        }
    }
}

TEST(aDuplicateQueryRetiresBothMeaningsOfItsIdentifier)
{
    for (const auto id : { 0, 7 }) {
        MeasurementClock clock;
        Ubiquitty::Terminal terminal;
        reset(terminal);
        terminal.tickClock = &clock;
        auto facts = syntheticFacts();
        facts.shaping = 1;
        REQUIRE(adoptTypography(terminal, facts));
        const auto prefix = "\x1b]7404;a=q:i=" + std::to_string(id) + ":e=1:w=10:h=1;";
        UbiquittyTest::feed(terminal, prefix + "{\\rtf1 first}\x1b\\");
        REQUIRE(terminal.richTextMeasurements.size() == 1);
        const auto token = terminal.richTextMeasurements.front().request.token;
        UbiquittyTest::feed(terminal, prefix + "{\\rtf1 second}\x1b\\");
        CHECK(terminal.richTextMeasurements.empty());
        CHECK(drained(terminal) == "\x1b]7404;a=e:i=" + std::to_string(id) + ":v=3:s=duplicate\x1b\\");
        CHECK(!Ubiquitty::OSC::RichTextBlocks::completeMeasurement(terminal, token, {}));
        UbiquittyTest::feed(terminal, prefix + "{\\rtf1 fresh}\x1b\\");
        REQUIRE(terminal.richTextMeasurements.size() == 1);
        CHECK(terminal.richTextMeasurements.front().request.token != token);
    }
}

TEST(aStaleEpochCannotGiveTwoMeaningsToAPendingQueryIdentifier)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 original}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    const auto token = terminal.richTextMeasurements.front().request.token;
    const auto stale = "\x1b]7404;a=q:i=4:e=1:E=0:w=10:h=1;{\\rtf1 replacement}\x1b\\";
    UbiquittyTest::feed(terminal, stale);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=4:v=3:s=duplicate\x1b\\");
    CHECK(!completeMeasurement(terminal, token, {}));
    UbiquittyTest::feed(terminal, stale);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=4:v=3:s=stale\x1b\\");
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:E=1:w=10:h=1;{\\rtf1 fresh}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    CHECK(terminal.richTextMeasurements.front().request.token != token);
    CHECK(!completeMeasurement(terminal, token, {}));
    CHECK(drained(terminal).empty());
}

TEST(cancellationIsAnIdentifierBarrierAndNeverDeletesABlock)
{
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    UbiquittyTest::feed(terminal, "\x1b]7404;a=p:i=4:w=10:h=1;{\\rtf1 retained}\x1b\\");
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    const auto token = terminal.richTextMeasurements.front().request.token;
    UbiquittyTest::feed(terminal, "\x1b]7404;a=c:i=4:e=1\x1b\\");
    CHECK(terminal.richTextMeasurements.empty());
    CHECK_EQ(terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 1 });
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=4:v=3:s=cancelled\x1b\\");
    CHECK(!Ubiquitty::OSC::RichTextBlocks::completeMeasurement(terminal, token, {}));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=c:i=4:e=1\x1b\\");
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=4:v=3:s=absent\x1b\\");
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:w=10:h=1;{\\rtf1 silent}\x1b\\\x1b]7404;a=c:i=4\x1b\\");
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal).empty());
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:w=10:h=1\x1b\\\x1b]7404;a=c:i=4:e=1\x1b\\");
    const auto completed = drained(terminal);
    CHECK(completed.starts_with("\x1b]7404;a=r:i=4:"));
    CHECK(completed.ends_with("\x1b]7404;a=e:i=4:v=3:s=absent\x1b\\"));
    for (const auto malformed : { "a=c", "a=c:i=4:w=1", "a=c:i=4:E=1", "a=c:i=4:N=0", "a=c:i=4:G=1" })
        CHECK(!Ubiquitty::OSC::RichTextBlocks::parseKeys(malformed));
}

TEST(deniedCancellationCannotReadOrReplaceAnOutstandingOutcome)
{
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
    DenyingMeasurePolicy policy;
    terminal.capabilityPolicy = &policy;
    UbiquittyTest::feed(terminal, "\x1b]7404;a=c:i=4:e=1\x1b\\");
    terminal.capabilityPolicy = nullptr;
    CHECK_EQ(terminal.richTextMeasurements.size(), std::size_t { 1 });
    CHECK(drained(terminal).empty());
}

TEST(completedMeasurementSurvivesOutputPressureWithoutBeingRemeasured)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 completed}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    const auto token = terminal.richTextMeasurements.front().request.token;
    auto measured = measure(terminal.richTextMeasurements.front().request.document, terminal.richTextMeasurements.front().request.shape, terminal.typography);
    REQUIRE(completeMeasurement(terminal, token, std::move(measured)));
    const std::string pressure(Ubiquitty::VT::maxBulkOutputBytes, 'x');
    REQUIRE(terminal.bulkOutput.append(pressure, Ubiquitty::VT::maxBulkOutputBytes));
    expireMeasurements(terminal, clock.instant + (2 * maxMeasurementWaitTicks));
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    CHECK(terminal.richTextMeasurements.front().completion.has_value());
    CHECK_EQ(terminal.bulkOutput.length, Ubiquitty::VT::maxBulkOutputBytes);
    terminal.bulkOutput.reset();
    publishMeasurements(terminal);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal).starts_with("\x1b]7404;a=r:i=4:"));
    publishMeasurements(terminal);
    CHECK(drained(terminal).empty());
}

TEST(cancelledAndTimedOutMeasurementsKeepTheirOutcomeUnderOutputPressure)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    for (const auto cancel : { false, true }) {
        MeasurementClock clock;
        Ubiquitty::Terminal terminal;
        reset(terminal);
        terminal.tickClock = &clock;
        auto facts = syntheticFacts();
        facts.shaping = 1;
        REQUIRE(adoptTypography(terminal, facts));
        UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
        const std::string pressure(Ubiquitty::VT::maxBulkOutputBytes, 'x');
        REQUIRE(terminal.bulkOutput.append(pressure, Ubiquitty::VT::maxBulkOutputBytes));
        if (cancel)
            UbiquittyTest::feed(terminal, "\x1b]7404;a=c:i=4:e=1\x1b\\");
        else
            expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
        REQUIRE(terminal.richTextMeasurements.size() == 1);
        CHECK(terminal.richTextMeasurements.front().request.document.text.empty());
        terminal.bulkOutput.reset();
        publishMeasurements(terminal);
        CHECK(terminal.richTextMeasurements.empty());
        CHECK(drained(terminal) == std::string("\x1b]7404;a=e:i=4:v=3:s=") + (cancel ? "cancelled" : "timeout") + "\x1b\\");
    }
}

TEST(typographyReportsCoalesceWhileTheProgramIsNotReading)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    auto facts = syntheticFacts();
    REQUIRE(adoptTypography(terminal, facts));
    const std::string pressure(Ubiquitty::VT::maxBulkOutputBytes, 'x');
    REQUIRE(terminal.bulkOutput.append(pressure, Ubiquitty::VT::maxBulkOutputBytes));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=s:i=3:N=1\x1b\\");
    CHECK(terminal.typography.needsReport);
    facts.cellWidth += 64;
    REQUIRE(adoptTypography(terminal, facts));
    terminal.bulkOutput.reset();
    publishMeasurements(terminal);
    CHECK(!terminal.typography.needsReport);
    const auto reply = drained(terminal);
    CHECK(reply.starts_with("\x1b]7404;a=n:i=3:v=3:E=2:"));
    CHECK(reply.find(":W=" + std::to_string(facts.cellWidth) + ":") != std::string::npos);
    publishMeasurements(terminal);
    CHECK(drained(terminal).empty());
}

TEST(pointerDocumentRevisionsHaveAnUnambiguousUnsigned32BitDomain)
{
    using Ubiquitty::OSC::RichTextBlocks::parseKeys;
    const auto maximum = parseKeys("w=10:h=1:i=7:M=1:D=4294967295");
    CHECK(maximum && maximum->placement.documentRevision == 4294967295U);
    CHECK(!parseKeys("w=10:h=1:i=7:M=1"));
    CHECK(!parseKeys("w=10:h=1:i=7:M=1:D=0"));
    CHECK(!parseKeys("w=10:h=1:i=0:M=1:D=1"));
    CHECK(!parseKeys("a=q:w=10:h=1:D=1"));
    CHECK(!parseKeys("a=d:i=1:D=1"));
    CHECK(!parseKeys("a=c:i=1:D=1"));
    CHECK(!parseKeys("a=s:i=1:N=1:D=1"));
    for (const auto bad : { "", "-1", "4294967296", "1:D=1" })
        CHECK(!parseKeys(std::string("w=10:h=1:D=") + bad));
}

TEST(aFullMeasurementQueueRecoversWithoutAnAnswerFromTheRenderer)
{
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    for (std::size_t index = 0; index < maxPendingMeasurements; ++index)
        UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=" + std::to_string(index) + ":e=1:w=10:h=1;{\\rtf1 queued}\x1b\\");
    REQUIRE(terminal.richTextMeasurements.size() == maxPendingMeasurements);
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=100:e=1:w=10:h=1;{\\rtf1 busy}\x1b\\");
    CHECK(drained(terminal) == "\x1b]7404;a=e:i=100:v=3:s=busy\x1b\\");
    CHECK_EQ(terminal.richTextMeasurements.size(), maxPendingMeasurements);
    clock.instant = clock.instant + maxMeasurementWaitTicks;
    Ubiquitty::OSC::RichTextBlocks::expireMeasurements(terminal, clock.instant);
    CHECK(terminal.richTextMeasurements.empty());
    const auto timeouts = drained(terminal);
    CHECK(timeouts.find("a=e:i=0:v=3:s=timeout") != std::string::npos);
    CHECK(timeouts.find("a=e:i=31:v=3:s=timeout") != std::string::npos);
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=100:e=1:w=10:h=1;{\\rtf1 recovered}\x1b\\");
    CHECK_EQ(terminal.richTextMeasurements.size(), std::size_t { 1 });
}

TEST(blockGeometryHasTheSameFiniteAdmissionDeadlineWithoutPTYStatus)
{
    MeasurementClock clock;
    Ubiquitty::Terminal terminal;
    reset(terminal);
    terminal.tickClock = &clock;
    auto facts = syntheticFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=p:i=4:w=10:h=1;{\\rtf1 retained}\x1b\\");
    requestBlockMeasurements(terminal);
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    CHECK(terminal.richTextMeasurements.front().blockSerial != 0);
    Ubiquitty::OSC::RichTextBlocks::expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drained(terminal).empty());
    CHECK_EQ(terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 1 });
}

TEST(resetAndScreenExitRetireQueriesWithoutPermittingLateCompletion)
{
    for (const auto resetSequence : { "\x1b"
                                      "c",
             "\x1b[?1049l" }) {
        MeasurementClock clock;
        Ubiquitty::Terminal terminal;
        reset(terminal);
        terminal.tickClock = &clock;
        UbiquittyTest::feed(terminal, "\x1b[?1049h");
        auto facts = syntheticFacts();
        facts.shaping = 1;
        REQUIRE(adoptTypography(terminal, facts));
        UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
        REQUIRE(terminal.richTextMeasurements.size() == 1);
        const auto token = terminal.richTextMeasurements.front().request.token;
        UbiquittyTest::feed(terminal, resetSequence);
        CHECK(terminal.richTextMeasurements.empty());
        CHECK(!Ubiquitty::OSC::RichTextBlocks::completeMeasurement(terminal, token, {}));
        CHECK(drained(terminal).empty());
    }
}

TEST(screenChangesRetireRequestsResultsAndPendingSubscriptions)
{
    using namespace Ubiquitty::OSC::RichTextBlocks;
    const std::array transitions {
        std::pair { std::string_view {}, std::string_view { "\x1b[?1049h" } },
        std::pair { std::string_view { "\x1b[?1049h" }, std::string_view { "\x1b[?1049l" } },
        std::pair { std::string_view {}, std::string_view { "\x1b"
                                                            "c" } }
    };
    for (const auto& [initial, transition] : transitions) {
        for (const auto isCompleted : { false, true }) {
            MeasurementClock clock;
            Ubiquitty::Terminal terminal;
            reset(terminal);
            terminal.tickClock = &clock;
            UbiquittyTest::feed(terminal, initial);
            auto facts = syntheticFacts();
            facts.shaping = 1;
            REQUIRE(adoptTypography(terminal, facts));
            UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
            REQUIRE(terminal.richTextMeasurements.size() == 1);
            const auto token = terminal.richTextMeasurements.front().request.token;
            if (isCompleted) {
                const auto& pending = terminal.richTextMeasurements.front();
                auto measured = measure(pending.request.document, pending.request.shape, terminal.typography);
                REQUIRE(completeMeasurement(terminal, token, std::move(measured)));
            }
            const std::string pressure(Ubiquitty::VT::maxBulkOutputBytes, 'x');
            REQUIRE(terminal.bulkOutput.append(pressure, Ubiquitty::VT::maxBulkOutputBytes));
            UbiquittyTest::feed(terminal, "\x1b]7404;a=s:i=7:N=1\x1b\\");
            REQUIRE(terminal.typography.needsReport);
            UbiquittyTest::feed(terminal, transition);
            CHECK(terminal.richTextMeasurements.empty());
            CHECK_EQ(terminal.typography.subscriptionID, 0);
            CHECK(!terminal.typography.needsReport);
            CHECK(!completeMeasurement(terminal, token, {}));
            terminal.bulkOutput.reset();
            publishMeasurements(terminal);
            CHECK(drained(terminal).empty());
            UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:e=1:w=10:h=1;{\\rtf1 fresh}\x1b\\");
            REQUIRE(terminal.richTextMeasurements.size() == 1);
            CHECK(terminal.richTextMeasurements.front().request.token != token);
            CHECK(!completeMeasurement(terminal, token, {}));
            CHECK(terminal.richTextMeasurements.size() == 1);
            CHECK(drained(terminal).empty());
        }
    }
}

TEST(discoveryNamesItsCurrentBudgetsAndDistinguishesRenderingFromParsing)
{
    Ubiquitty::Terminal terminal;
    reset(terminal);
    REQUIRE(terminal.richTextBlocks.setStorageLimit(4096));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:w=0:h=0\x1b\\");
    auto reply = drained(terminal);
    CHECK(replyField(reply, "K") == "0");
    CHECK(replyField(reply, "T") == "2000");
    const auto limits = replyField(reply, "Z");
    CHECK(limits.starts_with("body=16384,scalars=4096,runs=512,blocks=256,pending=32,"));
    CHECK(limits.find("fonts=16,colors=16,depth=32,family=255") != std::string::npos);
    CHECK(limits.find("width=1024,height=1024,open-width=1024,open-height=5120") != std::string::npos);
    CHECK(limits.ends_with("retained-bytes=4096,retained-used=0,catalog-count=255,catalog-bytes=16212"));
    auto facts = syntheticFacts();
    facts.rendersBlocks = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;a=p:i=1:w=10:h=1;{\\rtf1 budgeted}\x1b\\");
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:i=4:w=0:h=0\x1b\\");
    reply = drained(terminal);
    CHECK(replyField(reply, "K") == "1");
    CHECK(replyField(reply, "Z").ends_with("retained-bytes=4096,retained-used=" + std::to_string(terminal.richTextBlocks.storageBytes()) + ",catalog-count=255,catalog-bytes=16212"));
}

TEST(measurementHeaderCapacityCoversMaximumDecimalFields)
{
    Ubiquitty::Terminal terminal;
    reset(terminal);
    Measurement maximum;
    maximum.width = maximum.height = maximum.shown = maximum.capacity = maximum.lineCapacity = maximum.epoch
        = maximum.lineHeight = maximum.lineBaseline = maximum.minWidth = std::numeric_limits<std::uint32_t>::max();
    maximum.cellWidth = maximum.cellHeight = maximum.baseline = maximum.fontSize = std::numeric_limits<std::uint16_t>::max();
    CHECK(Ubiquitty::OSC::RichTextBlocks::answerMeasurement(terminal, std::numeric_limits<std::uint16_t>::max(), maximum));
    const auto reply = drained(terminal);
    CHECK(reply.starts_with("\x1b]7404;a=r:i=65535:"));
    CHECK(replyField(reply, "w") == "4294967295");
    CHECK(replyField(reply, "W") == "65535");
    CHECK(replyField(reply, "Z").ends_with("retained-bytes=1048576,retained-used=0,catalog-count=255,catalog-bytes=16212"));
    CHECK(reply.ends_with(":R=\x1b\\"));
}

TEST(caretReplyRoundingCannotOverflowItsSignedNativeCoordinate)
{
    Ubiquitty::Terminal terminal;
    reset(terminal);
    Measurement measured;
    measured.isShaped = true;
    measured.geometry.spans = { { 0, 1, 0, 0, std::numeric_limits<std::int32_t>::max() - 1 } };
    CHECK(Ubiquitty::OSC::RichTextBlocks::answerMeasurement(terminal, 1, measured, Ubiquitty::OSC::RichTextBlocks::GeometryReport::Include));
    const auto reply = drained(terminal);
    CHECK(reply.find(":G=0,1,0,0,33554432\x1b\\") != std::string::npos);
}
