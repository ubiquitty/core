// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/RowMeta.h"

#include "TerminalFixture.h"

#include "Core/OSC/TextSizing.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Ubiquitty::TextSizeSpec;
using Ubiquitty::OSC::TextSizing::parseMetadata;
using UbiquittyTest::cell;
using UbiquittyTest::feed;

namespace {

/// A terminal with a grid, ready to take sequences.
void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
}

} // namespace

TEST(emptyMetadataIsAllDefaults)
{
    const auto spec = parseMetadata("");
    CHECK(spec.has_value());
    CHECK(spec->scale == 1);
    CHECK(spec->widthCells == 0);
    CHECK(spec->fractionNumerator == 0);
    CHECK(spec->fractionDenominator == 0);
    CHECK(spec->verticalAlign == 0);
    CHECK(spec->horizontalAlign == 0);
}

TEST(everyKeyParsesFieldExact)
{
    const auto spec = parseMetadata("s=3:w=5:n=2:d=4:v=1:h=2");
    CHECK(spec.has_value());
    CHECK(spec->scale == 3);
    CHECK(spec->widthCells == 5);
    CHECK(spec->fractionNumerator == 2);
    CHECK(spec->fractionDenominator == 4);
    CHECK(spec->verticalAlign == 1);
    CHECK(spec->horizontalAlign == 2);
}

TEST(rangeBoundariesHoldOnBothSides)
{
    // Each key's extremes parse; one past each refuses. The boundary is the
    // subject, so both sides are asserted rather than one value with slack.
    CHECK(parseMetadata("s=1").has_value());
    CHECK(parseMetadata("s=7").has_value());
    CHECK(!parseMetadata("s=0").has_value());
    CHECK(!parseMetadata("s=8").has_value());
    CHECK(parseMetadata("w=0").has_value());
    CHECK(parseMetadata("w=7").has_value());
    CHECK(!parseMetadata("w=8").has_value());
    CHECK(parseMetadata("n=0:d=15").has_value());
    CHECK(!parseMetadata("n=16:d=15").has_value());
    CHECK(!parseMetadata("d=16").has_value());
    CHECK(parseMetadata("v=2").has_value());
    CHECK(!parseMetadata("v=3").has_value());
    CHECK(parseMetadata("h=2").has_value());
    CHECK(!parseMetadata("h=3").has_value());
}

TEST(theFractionConstraintIsDenominatorExceedsNumerator)
{
    // A numerator alone is a fraction with no denominator: refused rather than
    // read as no fraction at all, which is what the terminal that wrote the
    // second parser for this field had right and this one had wrong.
    CHECK(parseMetadata("n=3:d=4").has_value());
    CHECK(!parseMetadata("n=3:d=3").has_value());
    CHECK(!parseMetadata("n=3:d=2").has_value());
    // A numerator with no denominator is a fraction with no denominator, and
    // it is refused rather than read as no fraction at all: accepted, it
    // interns a specification whose fraction says nothing and sends it to
    // every renderer.
    CHECK(!parseMetadata("n=3").has_value());
    CHECK(!parseMetadata("n=1").has_value());
    // d alone scales nothing over an unset numerator of 0, and says so.
    CHECK(parseMetadata("d=4").has_value());
}

TEST(malformedMetadataRefusesWhole)
{
    CHECK(!parseMetadata("x=1").has_value()); // unknown key
    CHECK(!parseMetadata("s2").has_value()); // no separator
    CHECK(!parseMetadata("s=").has_value()); // empty value
    CHECK(!parseMetadata("=2").has_value()); // empty key
    CHECK(!parseMetadata("s=2:").has_value()); // trailing colon = empty pair
    CHECK(!parseMetadata(":s=2").has_value()); // leading colon
    CHECK(!parseMetadata("s=2::w=1").has_value()); // doubled colon
    CHECK(!parseMetadata("s=two").has_value()); // non-decimal
    CHECK(!parseMetadata("s=2a").has_value()); // trailing junk in value
    CHECK(parseMetadata("s=002").has_value()); // leading zeros are decimal
    CHECK(!parseMetadata("s=0002").has_value()); // but a fourth digit refuses
    CHECK(!parseMetadata("s=2:s=3").has_value()); // repeated key
    CHECK(!parseMetadata(std::string_view("s=\0002", 4)).has_value()); // NUL is payload, not terminator
}

TEST(aSizedRunPrintsAsBlocksAndAdvancesTheCursor)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;s=2;AB\007");

    // Two scale-2 blocks, each two columns by two rows, side by side from the
    // origin — the payload's text on the screen at the size it asked for.
    for (int block = 0; block < 2; ++block) {
        const int column = block * 2;
        for (int band = 0; band < 2; ++band) {
            const Ubiquitty::Cell* anchor = cell(t, band, column);
            CHECK(anchor && Ubiquitty::cellPen(*anchor).attributes.contains(Ubiquitty::VT::Attribute::Wide));
            CHECK(anchor && anchor->codePoint == static_cast<std::uint32_t>('A' + block));
            CHECK(anchor && Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() != 0);
            CHECK_EQ(static_cast<int>(Ubiquitty::penTextBand(&Ubiquitty::cellPen(*anchor))), band);
        }
    }
    CHECK_EQ(t.screens.active->cursorX, 4);
    CHECK_EQ(t.screens.active->cursorY, 0);
}

TEST(explicitWidthContainsTheWholePayloadAndCopyKeepsIt)
{
    for (const auto& [metadata, text, columns] : std::array {
             std::tuple { "w=3", std::string("ABC"), 3 },
             std::tuple { "w=1:n=1:d=2", std::string("ab"), 1 },
             std::tuple { "w=7:n=1:d=3", std::string("abcdefghijklmnopqrstu"), 7 },
             std::tuple { "w=7", std::string(4096, 'x'), 7 } }) {
        Ubiquitty::Terminal t;
        reset(t);
        t.drawsSizedText = true;
        feed(t, std::string("\033]66;") + metadata + ";" + text + "\007");
        CHECK_EQ(t.screens.active->cursorX, columns);
        CHECK_EQ(t.screens.active->cursorY, 0);
        const auto* anchor = cell(t, 0, 0);
        REQUIRE(anchor);
        const auto stored = Ubiquitty::runTextCodePoints(&t, *anchor);
        CHECK(std::ranges::equal(stored, text));
        CHECK(!Ubiquitty::isProportionalRunAnchor(&t, *anchor));
        Ubiquitty::Selection::Session selection;
        Ubiquitty::Selection::init(selection);
        Ubiquitty::Selection::beginGesture(selection, t, 0, 0, 1, 0, {});
        Ubiquitty::Selection::dragGesture(selection, t, 0, columns - 1, 0);
        CHECK(Ubiquitty::Selection::selectedText(selection, t) == text);
    }
}

TEST(explicitWidthWrapsAndErasesAsOneBlock)
{
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 4, 8);
    t.drawsSizedText = true;
    feed(t, "\033[1;7H\033]66;s=2:w=2;ABCD\007");
    CHECK_EQ(t.screens.active->cursorY, 1);
    CHECK_EQ(t.screens.active->cursorX, 4);
    for (int row : { 1, 2 }) {
        const auto stored = Ubiquitty::runTextCodePoints(&t, *cell(t, row, 0));
        CHECK(std::ranges::equal(stored, std::string_view("ABCD")));
    }
    feed(t, "\033[2;2HZ");
    CHECK_EQ(cell(t, 1, 1)->codePoint, static_cast<std::uint32_t>('Z'));
    for (int row : { 1, 2 })
        for (int column = 0; column < 4; ++column)
            CHECK_EQ(cell(t, row, column)->pen.extendedAttributes.textSizeSpecID(), 0u);
    feed(t, "\033T");
    for (int row : { 1, 2 })
        CHECK(std::ranges::equal(Ubiquitty::runTextCodePoints(&t, *cell(t, row, 0)), std::string_view("ABCD")));
}

TEST(combiningAfterExplicitWidthTextPreservesThePayloadAndAllBands)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    feed(t, "\033]66;s=2:w=2;ab\007\xcc\x81\xcc\x88");
    const std::array<std::uint32_t, 4> expected { 'a', 'b', 0x301, 0x308 };
    for (int row : { 0, 1 })
        CHECK(std::ranges::equal(Ubiquitty::runTextCodePoints(&t, *cell(t, row, 0)), expected));
    CHECK_EQ(t.screens.active->cursorX, 4);
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 0, 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 1, 3, 0);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == "ab\xcc\x81\xcc\x88");
}

TEST(aRefusedSpecificationDropsTheWholeCodeAndAPlainSizeJustPrints)
{
    Ubiquitty::Terminal t;
    reset(t);
    // Out-of-range scale: the parser refuses, so nothing of the code lands —
    // neither the size nor its text.
    feed(t, "\033]66;s=9;rejected\007");
    CHECK_EQ(t.screens.active->cursorX, 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);

    // A specification asking for the ordinary size spends no table slot and
    // prints through the plain path.
    feed(t, "\033]66;;plain\007");
    CHECK_EQ(t.screens.active->cursorX, 5);
    CHECK(cell(t, 0, 0)->codePoint == 'p');
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*cell(t, 0, 0)).extendedAttributes.textSizeSpecID()), 0);
    CHECK_EQ(t.textSizeTable.liveCount(), 0);
}

TEST(aGlyphTheTerminalCannotSizePrintsAtItsOrdinarySize)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    Ubiquitty::resize(t, 10, 6);
    // Scale 7 is seven columns per glyph on a six-column screen: no row it
    // fits on: the protocol discards that character.
    feed(t, "\033]66;s=7;X\007");
    const Ubiquitty::Cell* printed = cell(t, 0, 0);
    CHECK(printed && printed->codePoint != 'X');
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*printed).extendedAttributes.textSizeSpecID()), 0);
    CHECK_EQ(t.screens.active->cursorX, 0);

    // The height refusal, with room across but not down: the cursor near the
    // bottom leaves fewer rows than the block needs.
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    Ubiquitty::resize(t, 10, 20);
    feed(t, "\033[8;1H\033]66;s=7;Y\007");
    const Ubiquitty::Cell* low = cell(t, 7, 0);
    CHECK(low && low->codePoint == 'Y');
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*low).extendedAttributes.textSizeSpecID()), 0);
}

TEST(aFullTableLogsAndTheRunPrintsUnsized)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    Ubiquitty::resize(t, 40, 60);
    // Fifteen distinct specifications, each block side by side and still on
    // screen, fill every slot with an id a cell names. The sixteenth finds
    // nothing to reclaim and is refused, and its text still prints — at the
    // ordinary size.
    for (int i = 0; i < 15; ++i) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[1;%dH\033]66;s=2:n=%d:d=15;A\007", 1 + 2 * i, i);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);
    feed(t, "\033[20;1H\033]66;s=3:n=1:d=15;Z\007");
    const Ubiquitty::Cell* z = cell(t, 19, 0);
    CHECK(z && z->codePoint == 'Z');
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*z).extendedAttributes.textSizeSpecID()), 0);
}

TEST(aRendererWithoutTheBandCropPrintsTheRunPlainAndAdvancesByIt)
{
    // The default: no frontend has claimed it draws bands, so a sized run
    // prints at the ordinary size and the cursor advances by the text. The
    // protocol detects support by watching that advance, so a terminal
    // advancing by a block it will not draw tells the application a layout
    // its screen contradicts.
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(!t.drawsSizedText);
    feed(t, "\033]66;s=2;AB\007");
    CHECK_EQ(t.screens.active->cursorX, 2);
    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK(first && first->codePoint == 'A');
    CHECK(!Ubiquitty::cellPen(*first).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*first).extendedAttributes.textSizeSpecID()), 0);
    // No slot spent: an id minted here would be a table entry nothing draws.
    CHECK_EQ(t.textSizeTable.liveCount(), 0);
}

TEST(repeatAfterASizedGlyphRepeatsThatGlyph)
{
    // ECMA-48 gives REP the preceding GRAPHIC character, and a sized block is
    // one the terminal presented. Without the block recording itself, REP
    // reaches past the whole run to whatever was printed before the code —
    // here an X the application had finished with.
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "X");
    feed(t, "\033]66;s=2;A\007");
    feed(t, "\033[2b"); // REP 2

    // The repeats print at ordinary size: the size travelled with the code
    // that carried the run, and no sizing state outlives it.
    const Ubiquitty::Cell* firstRepeat = cell(t, 0, 3);
    const Ubiquitty::Cell* secondRepeat = cell(t, 0, 4);
    CHECK(firstRepeat && firstRepeat->codePoint == 'A');
    CHECK(secondRepeat && secondRepeat->codePoint == 'A');
    CHECK_EQ(static_cast<int>(Ubiquitty::cellPen(*firstRepeat).extendedAttributes.textSizeSpecID()), 0);
}

TEST(repeatAfterARefusedSizedGlyphStillRepeatsIt)
{
    // A discarded oversized character cannot become REP's predecessor.
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    Ubiquitty::resize(t, 10, 6);
    feed(t, "\033]66;s=7;A\007"); // seven columns on a six-column screen: refused
    feed(t, "\033[2b");
    CHECK(cell(t, 0, 1)->codePoint != 'A');
    CHECK(cell(t, 0, 2)->codePoint != 'A');
}

TEST(aFractionSmallerThanOneCellStillNamesItsSpecification)
{
    // A fraction asks for a glyph drawn smaller inside the cells it already
    // occupies — the parser refuses any fraction that is not below one, so
    // that is the only thing n and d can mean. It writes no block, and the
    // gate that decides whether a specification is the ordinary size read
    // only the scale and the width, so the fraction reached the table and
    // never reached a cell.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    Ubiquitty::processBytes(&t, "\x1b[H\x1b]66;n=1:d=2;A\x07");

    const Ubiquitty::Cell* landed = cell(t, 0, 0);
    CHECK(landed && landed->codePoint == static_cast<std::uint32_t>('A'));
    const unsigned id = Ubiquitty::cellPen(*landed).extendedAttributes.textSizeSpecID();
    CHECK_MSG(id != 0, "the cell carries no specification, so the fraction cannot reach a renderer");
    const TextSizeSpec* spec = t.textSizeTable.lookup(id);
    CHECK(spec != nullptr);
    if (spec) {
        CHECK_EQ(static_cast<int>(spec->fractionNumerator), 1);
        CHECK_EQ(static_cast<int>(spec->fractionDenominator), 2);
    }
    // One cell, drawn where the cursor was, and the cursor moved by one.
    CHECK(!Ubiquitty::cellPen(*landed).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK_EQ(t.screens.active->cursorX, 1);
}

TEST(flatteningKeepsGraphemesWhoseIDAlsoNamesRunText)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;n=1:d=2;e\xcc\x81\xcc\x82\007");
    const auto original = *cell(t, 0, 0);
    CHECK(original.clusterID() != 0);
    feed(t, "\033[2;1H\033]66;p=1:c=4;abcd\007");
    CHECK(!t.runTextStore.text(original.clusterID()).empty());
    auto flattened = original;
    CHECK(Ubiquitty::Screen::flattenSizedRun(&flattened, 1, Ubiquitty::defaultBlankCell,
        nullptr, nullptr, Ubiquitty::allTextSizeSpecIDs, t.textSizeTable));
    CHECK(flattened.clusterID() == original.clusterID());
    CHECK(flattened.codePoint == original.codePoint);

    Ubiquitty::Screen::scrollbackAppend(&t, &original, 1, 0, 0, false, true);
    Ubiquitty::TextSizeTable replacement;
    TextSizeSpec proportional;
    proportional.isProportional = true;
    proportional.runWidthCells = 4;
    CHECK(replacement.intern(proportional) == Ubiquitty::cellPen(original).extendedAttributes.textSizeSpecID());
    Ubiquitty::Screen::adoptTextSizeTable(t, replacement, Ubiquitty::Screen::TextSizeTableFrame::Snapshot);
    const auto* history = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(history && history->cells[0].clusterID() == original.clusterID());
    CHECK(history && Ubiquitty::cellPen(history->cells[0]).extendedAttributes.textSizeSpecID() == 0);
}

// A link is per cell, and the plain emit stamps the open `OSC 8` link on each
// cell it writes; a sized block and a proportional run are written by their
// own writer, so every cell they cover takes the link too — a button set as a
// run is clickable along its whole claim, and a sized glyph inside a link is
// the link. Closing the link leaves the next run's cells bare.
TEST(aSizedBlockAndAProportionalRunCarryTheOpenLink)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]8;;https://example.test/\033\\");
    feed(t, "\033]66;p=1:c=4;abcd\007");
    feed(t, "\033]66;s=2;W\007");
    feed(t, "\033]8;;\033\\");
    feed(t, "\033]66;p=1:c=3;xyz\007");
    const uint16_t linkID = cell(t, 0, 0)->linkID();
    CHECK_MSG(linkID != 0, "the run's anchor carries the link");
    for (int column = 1; column < 4; ++column)
        CHECK_MSG(cell(t, 0, column)->linkID() == linkID, "and so does each cell of its claim");
    CHECK_MSG(cell(t, 0, 4)->linkID() == linkID && cell(t, 0, 5)->linkID() == linkID, "the sized glyph's cells carry it");
    CHECK_MSG(cell(t, 1, 4)->linkID() == linkID, "on every band of the block");
    CHECK_MSG(cell(t, 0, 6)->linkID() == 0 && cell(t, 0, 8)->linkID() == 0, "a run after the link closed carries none");
}

// A sized emoji has to reach the colour sheet, and the routing that sends it
// there is Attribute::Emoji on the cell. The ordinary emit sets that while
// building the cluster's own pen; a sized block is written from the SCREEN pen
// instead (SizedBlock.cpp hands writeSizedBlock `s->pen`), so the question is
// whether the attribute survives the different path. Colour is not a nicety
// here: without it CellInstanceBuilder's isColorOnOwnSheet is false and the
// emoji is drawn from the mono sheet, which has no colour and may have no glyph.
TEST(aSizedEmojiCarriesTheAttributeThatRoutesItToTheColourSheet)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;s=3;\xF0\x9F\x98\x80\007"); // U+1F600 GRINNING FACE

    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor != nullptr);
    CHECK_MSG(anchor && anchor->codePoint == 0x1F600u, "the block anchors on the emoji itself");
    CHECK_MSG(anchor && Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() != 0,
        "and it is a sized block rather than a plain emit");
    // The control: the same emoji through the ordinary emit. Whether a sized
    // block LOSES the routing is only a question if a plain one carries it.
    Ubiquitty::Terminal plain;
    reset(plain);
    feed(plain, "\xF0\x9F\x98\x80");
    const Ubiquitty::Cell* plainCell = cell(plain, 0, 0);
    CHECK(plainCell != nullptr);
    const bool isPlainRouted = plainCell
        && Ubiquitty::cellPen(*plainCell).attributes.contains(Ubiquitty::VT::Attribute::Emoji);
    const bool isSizedRouted = anchor
        && Ubiquitty::cellPen(*anchor).attributes.contains(Ubiquitty::VT::Attribute::Emoji);
    const std::string routing = std::string("plain emit routes to the colour sheet: ")
        + (isPlainRouted ? "yes" : "no") + ", sized block: " + (isSizedRouted ? "yes" : "no");
    CHECK_MSG(isSizedRouted == isPlainRouted, routing.c_str());
}

// Can a real feed put a sized block on a doubled row? The builder reaches its
// double-sheet branch when a cell carries both, and a builder test can construct
// that state directly — which says nothing about whether the screen ever
// produces it. DECDWL is a property of the row and OSC 66 one of the cell, so
// the question is whether either refuses the other.
TEST(aSizedBlockOnADoubleWidthRowKeepsBothProperties)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033#6"); // DECDWL — this row is double-width
    feed(t, "\033]66;s=3;A\007");

    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor != nullptr);
    const bool isSizedHere = anchor
        && Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() != 0;
    const std::string both = std::string("a DECDWL row accepts a sized block: ")
        + (isSizedHere ? "yes, so the double sheet is feed-reachable"
                       : "no, the row refuses it and the branch guards a state the screen cannot make");
    CHECK_MSG(anchor && anchor->codePoint == static_cast<std::uint32_t>('A'), both.c_str());
    CHECK_MSG(isSizedHere, both.c_str());
}

// ── Proportional runs (Docs/proportional-text.md) ───────────────────────────

// A shaping client that sets every code point at a fixed advance, and
// remembers what it was asked, so the test can see the text reached it whole.
// The width it answers is then `answer` cells for eight code points: Core
// lays the glyphs out and divides by the cell advance it reports.
struct FixedMeasurer final : Ubiquitty::TextMeasureClient {
    int answer = 0;
    std::vector<std::uint32_t> asked;
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        asked.assign(codePoints.begin(), codePoints.end());
        if (answer <= 0 || glyphsOut.size() < codePoints.size())
            return 0;
        // `answer` cells of 8 px across the text: each code point advances
        // by its even share.
        const float advance = 8.0f * static_cast<float>(answer) / static_cast<float>(codePoints.size());
        for (std::size_t i = 0; i < codePoints.size(); ++i) {
            glyphsOut[i] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[i].glyph = static_cast<std::uint32_t>(i + 1);
            glyphsOut[i].advanceInPixels = advance;
            glyphsOut[i].cluster = static_cast<std::uint16_t>(i);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

TEST(theProportionalKeysParseAndRefuseAsTheDocumentSays)
{
    using Ubiquitty::OSC::TextSizing::parseRequest;
    // A print with its width; `h` rides along.
    const auto print = parseRequest("p=1:c=5:h=2");
    CHECK(print.has_value());
    CHECK(print && print->spec.isProportional == 1 && print->spec.runWidthCells == 5 && print->spec.horizontalAlign == 2);
    CHECK(print && !print->isQuery);
    // A query needs no width, carries its id, and stores no width.
    const auto query = parseRequest("p=1:q=1:i=7:c=3");
    CHECK(query.has_value());
    CHECK(query && query->isQuery && query->requestID == 7 && query->spec.runWidthCells == 0);
    // Refusals: a print without a width, a sizing key other than `s` beside
    // `p`, a proportional key without `p`, a width past the strip, `p` at zero.
    CHECK(!parseRequest("p=1").has_value());
    CHECK(!parseRequest("p=1:c=4:w=2").has_value());
    CHECK(!parseRequest("p=1:c=4:s=2:w=1").has_value());
    CHECK(!parseRequest("p=1:c=4:s=2:n=1:d=2").has_value());
    CHECK(!parseRequest("p=1:c=4:v=1").has_value());
    CHECK(!parseRequest("p=1:c=4:s=8").has_value());
    CHECK(!parseRequest("c=4").has_value());
    CHECK(!parseRequest("q=1").has_value());
    CHECK(parseRequest("p=1:c=33").has_value());
    CHECK(parseRequest("p=1:c=1024").has_value());
    CHECK(!parseRequest("p=1:c=1025").has_value());
    CHECK(!parseRequest("p=0:c=4").has_value());
    // The sizing protocol's own forms still parse through the same walk.
    CHECK(parseRequest("s=2:w=1").has_value());
    CHECK(Ubiquitty::OSC::TextSizing::parseMetadata("s=2").has_value());
}

// `s` beside `p` scales the run as it scales a block: the spec keeps the
// scale and the claim in the run's own cells, and a query at a scale parses
// as the query it is.
TEST(aScaleBesideTheProportionalKeyParsesWithTheRunsClaim)
{
    using Ubiquitty::OSC::TextSizing::parseRequest;
    const auto print = parseRequest("p=1:c=6:s=2:h=2");
    REQUIRE(print.has_value());
    CHECK(print->spec.isProportional == 1 && print->spec.scale == 2 && print->spec.runWidthCells == 6 && print->spec.horizontalAlign == 2);
    const auto largest = parseRequest("s=7:p=1:c=3");
    CHECK(largest && largest->spec.scale == 7 && largest->spec.runWidthCells == 3);
    const auto query = parseRequest("p=1:q=1:s=3:i=4");
    CHECK(query && query->isQuery && query->spec.scale == 3 && query->requestID == 4 && query->spec.runWidthCells == 0);
}

// A scaled run is a block of `s` rows, each band an anchor and continuations
// over `c * s` columns stamped with the spec and the band, every band's anchor
// naming the run's text; the cursor advances by the block's width on its top
// row, and every band row is flowed.
TEST(aScaledProportionalRunIsPlacedAsABlockOfBands)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[3;5H\033]66;p=1:c=3:s=2;Hello\033\\");
    const Ubiquitty::Cell* top = cell(t, 2, 4);
    REQUIRE(top != nullptr);
    const unsigned id = Ubiquitty::cellPen(*top).extendedAttributes.textSizeSpecID();
    const Ubiquitty::TextSizeSpec* spec = t.textSizeTable.lookup(id);
    REQUIRE(spec != nullptr);
    CHECK(spec->isProportional == 1 && spec->scale == 2 && spec->runWidthCells == 3);
    for (int band = 0; band < 2; ++band) {
        const Ubiquitty::Cell* anchor = cell(t, 2 + band, 4);
        REQUIRE(anchor != nullptr);
        const Ubiquitty::Pen& anchorPen = Ubiquitty::cellPen(*anchor);
        CHECK(anchorPen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK_EQ(anchorPen.extendedAttributes.textSizeSpecID(), id);
        CHECK_EQ(static_cast<int>(Ubiquitty::penTextBand(&anchorPen)), band);
        CHECK(Ubiquitty::isProportionalRunAnchor(&t, *anchor));
        const auto text = Ubiquitty::runTextCodePoints(&t, *anchor);
        CHECK(text.size() == 5 && text[0] == 'H' && text[4] == 'o');
        for (int column = 5; column < 10; ++column) {
            const Ubiquitty::Cell* continuation = cell(t, 2 + band, column);
            REQUIRE(continuation != nullptr);
            const Ubiquitty::Pen& pen = Ubiquitty::cellPen(*continuation);
            CHECK(pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
            CHECK_EQ(pen.extendedAttributes.textSizeSpecID(), id);
            CHECK_EQ(static_cast<int>(Ubiquitty::penTextBand(&pen)), band);
        }
        const Ubiquitty::Cell* after = cell(t, 2 + band, 10);
        CHECK(after && after->codePoint == 0 && Ubiquitty::cellPen(*after).extendedAttributes.textSizeSpecID() == 0);
        CHECK(t.grid->rowMeta(2 + band).has(Ubiquitty::RowFlag::Flowed));
        CHECK_EQ(Ubiquitty::claimedRunColumns(*anchor, &t.textSizeTable, t.ambiguousWidth), 6);
    }
    CHECK_EQ(t.screens.active->cursorY, 2);
    CHECK_EQ(t.screens.active->cursorX, 10);
}

// A scaled run fits and refuses as a block does: one taller than the rows
// left in the scroll region prints as text, and one the margin cuts moves
// whole to the next row. A terminal whose renderer draws no sized bands
// places the run at its own size, one row of its `c` cells.
TEST(aScaledProportionalRunFitsAndRefusesAsABlockDoes)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[24;1H\033]66;p=1:c=3:s=2;Hi\033\\");
    const Ubiquitty::Cell* plain = cell(t, 23, 0);
    CHECK(plain && plain->codePoint == 'H' && Ubiquitty::cellPen(*plain).extendedAttributes.textSizeSpecID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 2);

    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[1;76H\033]66;p=1:c=3:s=2;Hi\033\\");
    const Ubiquitty::Cell* moved = cell(t, 1, 0);
    CHECK(moved && Ubiquitty::isProportionalRunAnchor(&t, *moved) && Ubiquitty::penTextBand(&Ubiquitty::cellPen(*moved)) == 0);
    const Ubiquitty::Cell* lower = cell(t, 2, 0);
    CHECK(lower && Ubiquitty::isProportionalRunAnchor(&t, *lower) && Ubiquitty::penTextBand(&Ubiquitty::cellPen(*lower)) == 1);
    CHECK_EQ(t.screens.active->cursorX, 6);

    reset(t);
    t.drawsSizedText = false;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=3:s=2;Hi\033\\");
    const Ubiquitty::Cell* unscaled = cell(t, 0, 0);
    REQUIRE(unscaled != nullptr);
    const Ubiquitty::TextSizeSpec* spec = t.textSizeTable.lookup(Ubiquitty::cellPen(*unscaled).extendedAttributes.textSizeSpecID());
    CHECK(spec && spec->isProportional == 1 && spec->scale == 1 && spec->runWidthCells == 3);
    const Ubiquitty::Cell* below = cell(t, 1, 0);
    CHECK(below && below->codePoint == 0);
    CHECK_EQ(t.screens.active->cursorX, 3);
}

// The measure at a scale is the run's own cells, each `s` columns: the text
// scales with them, so a run printed at the answer with the same `s` fits.
TEST(aQueryAtAScaleAnswersInTheRunsOwnCells)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    FixedMeasurer measurer;
    measurer.answer = 5;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\033]66;p=1:q=1:s=3:i=9;Settings\007"); // preferences-vocabulary-ok: measured bytes, not a name
    char out[128];
    UbiquittyTest::drain(t, out);
    CHECK(std::string(out) == std::string("\033]66;i=9:c=5\007"));
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aQueryRepliesWithTheMeasuredCellsAndPrintsNothing)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    FixedMeasurer measurer;
    measurer.answer = 5;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // The payload is eight characters of arbitrary text, which the cases below count.
    feed(t, "\033]66;p=1:q=1:i=42;Settings\007"); // preferences-vocabulary-ok: measured bytes, not a name
    char out[128];
    UbiquittyTest::drain(t, out);
    CHECK(std::string(out) == std::string("\033]66;i=42:c=5\007"));
    CHECK_EQ(static_cast<int>(measurer.asked.size()), 8);
    CHECK(measurer.asked.size() == 8 && measurer.asked[0] == 'S' && measurer.asked[7] == 's');
    // Nothing printed, cursor where it was.
    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK(first && first->codePoint == 0);
    CHECK_EQ(t.screens.active->cursorX, 0);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aQueryWithNoMeasurerAnswersTheGridFaceWidth)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Eight narrow code points and one wide: ten cells in the grid's face.
    feed(t, "\033]66;p=1:q=1;Settings\xe6\x97\xa5\007"); // preferences-vocabulary-ok: measured bytes
    char out[128];
    UbiquittyTest::drain(t, out);
    CHECK(std::string(out) == std::string("\033]66;i=0:c=10\007"));
}

TEST(aProportionalRunOccupiesItsWidthAndCarriesItsText)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=5:h=1;Settings\007"); // preferences-vocabulary-ok: measured bytes
    // Five cells: an anchor and four continuations, every one stamped.
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'S');
    CHECK(anchor && Ubiquitty::cellPen(*anchor).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    const unsigned id = anchor ? Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() : 0u;
    CHECK(id != 0);
    for (int column = 1; column < 5; ++column) {
        const Ubiquitty::Cell* continuation = cell(t, 0, column);
        CHECK(continuation && Ubiquitty::cellPen(*continuation).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(continuation && Ubiquitty::cellPen(*continuation).extendedAttributes.textSizeSpecID() == id);
    }
    const Ubiquitty::Cell* after = cell(t, 0, 5);
    CHECK(after && after->codePoint == 0);
    CHECK_EQ(t.screens.active->cursorX, 5);
    // The spec the id names is the one the code asked for.
    const Ubiquitty::TextSizeSpec* spec = t.textSizeTable.lookup(id);
    CHECK(spec && spec->isProportional == 1 && spec->runWidthCells == 5 && spec->horizontalAlign == 1);
    // The whole text rides the anchor's cluster, through the pool.
    std::uint32_t text[32] = {};
    const int count = anchor ? Ubiquitty::cellClusterCodePoints(&t, anchor, text, 32) : 0;
    CHECK_EQ(count, 8);
    CHECK(count == 8 && text[0] == 'S' && text[1] == 'e' && text[7] == 's');
}

TEST(aShortProportionalRunStillRidesItsAnchor)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=2;OK\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    // Two code points are the run's text like any other length: the anchor
    // names the store's entry, and its inline buffer stays empty.
    CHECK(anchor && anchor->codePoint == 'O' && anchor->clusterID() != 0 && anchor->combiningLength() == 0);
    std::uint32_t text[8] = {};
    const int count = anchor ? Ubiquitty::cellClusterCodePoints(&t, anchor, text, 8) : 0;
    CHECK(count == 2 && text[0] == 'O' && text[1] == 'K');
    const Ubiquitty::Cell* continuation = cell(t, 0, 1);
    CHECK(continuation && continuation->codePoint == Ubiquitty::wideContinuationSentinelCodePoint
        && continuation->clusterID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 2);
}

TEST(aProportionalRunIsBoundedByItsPayloadAndNotByItsCells)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Seventeen characters into one cell: the text rides the anchor, so the
    // cells a run claims no longer bound it. The cursor still advances by the
    // claim, which is what the protocol makes every receiver agree on.
    feed(t, "\033]66;p=1:c=1;ABCDEFGHIJKLMNOPQ\007");
    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK(first && first->codePoint == 'A' && Ubiquitty::cellPen(*first).extendedAttributes.textSizeSpecID() != 0);
    std::uint32_t text[32] = {};
    const int count = first ? Ubiquitty::cellClusterCodePoints(&t, first, text, 32) : 0;
    CHECK_EQ(count, 17);
    CHECK(count == 17 && text[16] == 'Q');
    CHECK_EQ(t.screens.active->cursorX, 1);
    // A payload-sized run is one entry on its anchor.
    reset(t);
    Ubiquitty::resize(t, 24, 32);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=32;" + std::string(Ubiquitty::OSC::TextSizing::maxTextBytes, 'a') + "\007");
    const Ubiquitty::Cell* payloadAnchor = cell(t, 0, 0);
    CHECK(payloadAnchor && Ubiquitty::isProportionalRunAnchor(&t, *payloadAnchor));
    CHECK_EQ(payloadAnchor ? static_cast<int>(Ubiquitty::runTextCodePoints(&t, *payloadAnchor).size()) : 0,
        static_cast<int>(Ubiquitty::OSC::TextSizing::maxTextBytes));
    // Wider than the screen: the text prints plainly and the cursor follows it.
    reset(t);
    Ubiquitty::resize(t, 24, 20);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=32;Hi\007");
    const Ubiquitty::Cell* h = cell(t, 0, 0);
    CHECK(h && h->codePoint == 'H' && Ubiquitty::cellPen(*h).extendedAttributes.textSizeSpecID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 2);
}

TEST(aBareTerminalNeitherAnswersAQueryNorPlacesARun)
{
    Ubiquitty::Terminal t;
    reset(t);
    CHECK(!t.drawsSizedText);
    CHECK(!t.drawsProportionalText);
    feed(t, "\033]66;p=1:q=1:i=7;Cancel\007");
    char out[128];
    CHECK_EQ(UbiquittyTest::drain(t, out), 0);
    CHECK_EQ(t.screens.active->cursorX, 0);
    feed(t, "\033]66;p=1:c=3;Cancel\007");
    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK(first && first->codePoint == 'C' && Ubiquitty::cellPen(*first).extendedAttributes.textSizeSpecID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 6);
}

TEST(aRendererWithoutProportionalTextIgnoresTheRunButStillSizes)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    FixedMeasurer measurer;
    measurer.answer = 5;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\033]66;p=1:q=1:i=42;Cancel\007");
    char out[128];
    CHECK_EQ(UbiquittyTest::drain(t, out), 0);
    CHECK(measurer.asked.empty());
    feed(t, "\033]66;p=1:c=3;Cancel\007");
    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK(first && Ubiquitty::cellPen(*first).extendedAttributes.textSizeSpecID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 6);
    feed(t, "\033]66;s=2;X\007");
    const Ubiquitty::Cell* sized = cell(t, 0, 6);
    CHECK(sized && sized->codePoint == 'X' && Ubiquitty::cellPen(*sized).extendedAttributes.textSizeSpecID() != 0);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aRendererWithProportionalTextAloneAnswersAndPlacesButDoesNotSize)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:q=1:i=3;OK\007");
    char out[128];
    UbiquittyTest::drain(t, out);
    CHECK(std::string(out) == std::string("\033]66;i=3:c=2\007"));
    feed(t, "\033]66;p=1:c=4;OK\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    CHECK_EQ(t.screens.active->cursorX, 4);
    feed(t, "\033]66;s=2;X\007");
    const Ubiquitty::Cell* plain = cell(t, 0, 4);
    CHECK(plain && plain->codePoint == 'X' && Ubiquitty::cellPen(*plain).extendedAttributes.textSizeSpecID() == 0);
    CHECK_EQ(t.screens.active->cursorX, 5);
}

TEST(aProportionalRunClaimsItsCellsAgainstTheWidePairRepair)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=6;Cancel\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && Ubiquitty::claimedRunColumns(*anchor, &t.textSizeTable, t.ambiguousWidth) == 6);
    // An erase to the right of the run leaves the run whole: the repair that
    // follows an edit keeps a chain as long as its claim, and a claim of one
    // would blank the five continuations.
    feed(t, "\033[1;8H\033[K");
    for (int column = 1; column < 6; ++column) {
        const Ubiquitty::Cell* continuation = cell(t, 0, column);
        CHECK(continuation && Ubiquitty::cellPen(*continuation).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    }
    // Overwriting the run's third cell breaks the chain, and the repair
    // erases the torn run rather than drawing a wrong slice of it.
    feed(t, "\033[1;3HX");
    const Ubiquitty::Cell* third = cell(t, 0, 2);
    CHECK(third && third->codePoint == 'X');
    const Ubiquitty::Cell* tornAnchor = cell(t, 0, 0);
    CHECK(tornAnchor && !Ubiquitty::cellPen(*tornAnchor).attributes.contains(Ubiquitty::VT::Attribute::Wide));
}

// A run the run-text store has no room for still takes the cell it claims:
// its text in the grid face, cut at the claim, and the cursor after it, so
// the program's layout holds whatever the store could keep.
TEST(aRunPastTheRunTextBudgetTakesItsClaimInTheGridFace)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    const std::string run = "\033]66;p=1:c=1;" + std::string(4000, 'x') + "\007";
    for (int index = 0; index < 65; ++index) {
        feed(t, run);
    }
    CHECK_EQ(t.runTextStore.liveCodePoints(), 260000u);
    feed(t, "\033[2;1H");
    feed(t, run);
    feed(t, "|");
    CHECK_EQ(t.runTextStore.liveCodePoints(), 260000u);
    CHECK_EQ(t.screens.active->cursorY, 1);
    CHECK_EQ(t.screens.active->cursorX, 2);
    const auto* letter = cell(t, 1, 0);
    CHECK(letter && letter->codePoint == 'x' && !Ubiquitty::isProportionalRunAnchor(&t, *letter));
    const auto* bar = cell(t, 1, 1);
    CHECK_MSG(bar && bar->codePoint == '|', "the text after the run starts after its one claimed cell");
}

namespace {

/// Fills all fifteen spec slots with runs still on screen, one claim width a
/// row from the top, so no later spec can be interned or reclaimed.
void fillTheSpecTable(Ubiquitty::Terminal& t)
{
    for (int cells = 1; cells <= Ubiquitty::textSizeTableCapacity; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[%d;1H\033]66;p=1:c=%d;x\007", cells, cells);
        feed(t, sequence);
    }
}

/// The text of `row` from `first` up to `end`, a blank cell as a space.
std::string rowText(const Ubiquitty::Terminal& t, int row, int first, int end)
{
    std::string text;
    for (int column = first; column < end; ++column) {
        const Ubiquitty::Cell* at = cell(t, row, column);
        text.push_back(at && at->codePoint >= 0x20 && at->codePoint < 0x7F ? static_cast<char>(at->codePoint) : ' ');
    }
    return text;
}

} // namespace

// A print writes the run into exactly `c` cells and advances by `c`, whatever
// the face (Docs/proportional-text.md), and that holds when the spec table is
// full: a run whose spec no slot can take is set in the grid face inside its
// claim, aligned by `h` and cut at the claim, under its pen, and the text
// after it starts where it would after the run. A dashboard naming sixteen
// claim widths sets its sixteenth this way, and the border after its
// right-aligned column stands where the program put it.
TEST(aRunTheSpecTableHasNoRoomForTakesItsClaimInTheGridFace)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    fillTheSpecTable(t);
    CHECK_EQ(t.textSizeTable.liveCount(), Ubiquitty::textSizeTableCapacity);

    feed(t, "\033[17;1H|\033[1m\033]66;p=1:c=17:h=1;Age\007\033[0m|");
    CHECK_EQ(t.textSizeTable.liveCount(), Ubiquitty::textSizeTableCapacity);
    CHECK_EQ(t.screens.active->cursorX, 19);
    CHECK_MSG(rowText(t, 16, 0, 19) == "|              Age|", rowText(t, 16, 0, 19).c_str());
    const Ubiquitty::Cell* letter = cell(t, 16, 15);
    REQUIRE(letter);
    CHECK_MSG(!Ubiquitty::isProportionalRunAnchor(&t, *letter), "the run is grid text");
    CHECK_MSG(Ubiquitty::cellPen(*letter).attributes.contains(Ubiquitty::VT::Attribute::Bold), "under the run's pen");

    feed(t, "\033[18;1H\033]66;p=1:c=18;Age\007|");
    CHECK_MSG(rowText(t, 17, 0, 19) == "Age               |", rowText(t, 17, 0, 19).c_str());
    feed(t, "\033[19;1H\033]66;p=1:c=19:h=2;Age\007|");
    CHECK_MSG(rowText(t, 18, 0, 20) == "        Age        |", rowText(t, 18, 0, 20).c_str());
    feed(t, "\033[20;1H\033]66;p=1:c=20:h=1;abcdefghijklmnopqrstuvwxyz\007|");
    CHECK_MSG(rowText(t, 19, 0, 21) == "abcdefghijklmnopqrst|", rowText(t, 19, 0, 21).c_str());
    // A wide letter the claim cannot hold whole is cut with the rest.
    feed(t, "\033[21;1H\033]66;p=1:c=21;abcdefghijklmnopqrst\xe6\x97\xa5\007|");
    CHECK_MSG(rowText(t, 20, 0, 22) == "abcdefghijklmnopqrst |", rowText(t, 20, 0, 22).c_str());
    // A claim the margin arrives in moves whole to the next row, as a run's does.
    feed(t, "\033[22;70H\033]66;p=1:c=22;Age\007|");
    CHECK_EQ(t.screens.active->cursorY, 22);
    CHECK_EQ(t.screens.active->cursorX, 23);
    CHECK_MSG(rowText(t, 22, 0, 23) == "Age                   |", rowText(t, 22, 0, 23).c_str());
}

// A run set in the grid face stays inside the cells it claims, wherever the
// grid's own print path would have taken its letters: past the visible half
// of a DECDWL row, wider than its code points where VS16 makes an emoji, onto
// the cell before it for a leading mark, or leftwards under DECRLM. A claim
// ending at the right edge leaves the wrap pending, so the text after it
// starts on the next row, and one wrapping past the scroll region's last row
// scrolls the region as text does.
TEST(aRunSetInTheGridFaceStaysInsideItsClaim)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Fifteen claim widths from 16 hold every spec slot, so the narrower
    // claims below have none and are set in the grid face.
    for (int cells = 16; cells < 16 + Ubiquitty::textSizeTableCapacity; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[%d;1H\033]66;p=1:c=%d;x\007", cells - 15, cells);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), Ubiquitty::textSizeTableCapacity);

    // A DECDWL row shows 40 columns: a claim from column 35 cannot end
    // inside them and moves whole to the next row.
    feed(t, "\033[16;1H\033#6\033[16;36H\033]66;p=1:c=9;Age\007|");
    CHECK_MSG(rowText(t, 15, 35, 40) == "     ", rowText(t, 15, 35, 40).c_str());
    CHECK_MSG(rowText(t, 16, 0, 10) == "Age      |", rowText(t, 16, 0, 10).c_str());

    // VS16 makes the heart an emoji two cells wide, and the claim holds it.
    feed(t, "\033[18;11H\033]66;p=1:c=7;\xe2\x9d\xa4\xef\xb8\x8f"
            "a\007|");
    const Ubiquitty::Cell* heart = cell(t, 17, 10);
    REQUIRE(heart);
    CHECK_EQ(heart->codePoint, 0x2764u);
    CHECK(Ubiquitty::cellPen(*heart).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(Ubiquitty::cellPen(*heart).attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK_MSG(rowText(t, 17, 12, 18) == "a    |", rowText(t, 17, 12, 18).c_str());

    // A leading mark is a grapheme of its own inside the claim and joins
    // nothing printed before it.
    feed(t, "\033[19;1Hx\033]66;p=1:c=3;\xcc\x81"
            "bc\007");
    const Ubiquitty::Cell* before = cell(t, 18, 0);
    const Ubiquitty::Cell* mark = cell(t, 18, 1);
    REQUIRE(before && mark);
    CHECK(before->codePoint == 'x' && before->combiningLength() == 0);
    CHECK_EQ(mark->codePoint, 0x0301u);
    CHECK_MSG(rowText(t, 18, 2, 4) == "bc", rowText(t, 18, 2, 4).c_str());

    // Right-to-left writing moves the grid's letters leftwards; the claim's
    // stay where the claim is.
    feed(t, "\033[?34h\033[20;10H\033]66;p=1:c=4;Age\007\033[?34l");
    CHECK_MSG(rowText(t, 19, 8, 13) == " Age ", rowText(t, 19, 8, 13).c_str());

    // A claim ending at the right edge: the text after it opens the next row.
    feed(t, "\033[21;71H\033]66;p=1:c=10;Age\007|");
    CHECK_MSG(rowText(t, 20, 70, 80) == "Age       ", rowText(t, 20, 70, 80).c_str());
    CHECK_MSG(rowText(t, 21, 0, 1) == "|", rowText(t, 21, 0, 1).c_str());

    // At the last column of the last row the heart's claim holds only the
    // heart, and nothing scrolls.
    feed(t, "\033[24;79H\033]66;p=1:c=2;\xe2\x9d\xa4\xef\xb8\x8f"
            "a\007");
    const Ubiquitty::Cell* lastHeart = cell(t, 23, 78);
    REQUIRE(lastHeart);
    CHECK_EQ(lastHeart->codePoint, 0x2764u);
    const Ubiquitty::Cell* top = cell(t, 0, 0);
    CHECK_MSG(top && Ubiquitty::isProportionalRunAnchor(&t, *top), "the screen did not scroll");

    // A claim wrapping past the region's last row scrolls the region a line.
    feed(t, "\033[24;75H\033]66;p=1:c=9;Age\007");
    const Ubiquitty::Cell* raised = cell(t, 22, 78);
    REQUIRE(raised);
    CHECK_MSG(raised->codePoint == 0x2764u, "the last row moved up a line");
    CHECK_MSG(rowText(t, 23, 0, 9) == "Age      ", rowText(t, 23, 0, 9).c_str());
    CHECK_EQ(t.screens.active->cursorY, 23);
    CHECK_EQ(t.screens.active->cursorX, 9);
}

// A run's claim follows the wrap a printed letter leaves pending: a one-cell
// run after a full row opens the next row rather than overwriting the last
// letter, and the text after a run that ends at the right edge starts on the
// next row instead of being lost.
TEST(aRunFollowsThePendingWrapOnBothSidesOfItsClaim)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, std::string(80, 'x'));
    CHECK(t.screens.active->wrapPending);
    feed(t, "\033]66;p=1:c=1;Q\007");
    const Ubiquitty::Cell* last = cell(t, 0, 79);
    const Ubiquitty::Cell* run = cell(t, 1, 0);
    REQUIRE(last && run);
    CHECK_MSG(last->codePoint == 'x', "the row's last letter stays");
    CHECK_MSG(run->codePoint == 'Q', "the run opens the next row");

    feed(t, "\033[3;71H\033]66;p=1:c=10;Age\007|");
    const Ubiquitty::Cell* anchor = cell(t, 2, 70);
    REQUIRE(anchor);
    CHECK(Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    const Ubiquitty::Cell* bar = cell(t, 3, 0);
    CHECK_MSG(bar && bar->codePoint == '|', "the text after the run opens the next row");
}

TEST(aRunIsPlacedAgainWithinTheBackoffAfterAnEraseFreesItsText)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[?1049h");
    const std::string run = "\033]66;p=1:c=1;" + std::string(4000, 'x') + "\007";
    for (int index = 0; index < 66; ++index) {
        feed(t, run);
    }
    feed(t, "\033[2J\033[H");
    bool didPlace = false;
    for (int index = 0; index < 64; ++index) {
        feed(t, "\033[H");
        feed(t, run);
        const auto* anchor = cell(t, 0, 0);
        if (anchor && Ubiquitty::isProportionalRunAnchor(&t, *anchor)) {
            didPlace = true;
            break;
        }
    }
    CHECK(didPlace);
}

TEST(aRunWhoseCharactersOutnumberItsCellsRidesItsAnchor)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Forty-three characters over four cells: the anchor names them all and the
    // continuations carry only the claim, so no reader rebuilds a run by
    // walking its cells.
    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    feed(t, "\033]66;p=1:c=4;" + sentence + "\007");
    CHECK_EQ(t.screens.active->cursorX, 4);
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'T' && Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    const std::span<const std::uint32_t> stored = anchor
        ? Ubiquitty::runTextCodePoints(&t, *anchor)
        : std::span<const std::uint32_t> {};
    CHECK_EQ(static_cast<int>(stored.size()), static_cast<int>(sentence.size()));
    bool isSame = stored.size() == sentence.size();
    for (std::size_t i = 0; isSame && i < stored.size(); ++i)
        isSame = stored[i] == static_cast<std::uint32_t>(sentence[i]);
    CHECK_MSG(isSame, "the anchor holds the sentence the code carried");
    for (int column = 1; column < 4; ++column) {
        const Ubiquitty::Cell* continuation = cell(t, 0, column);
        CHECK(continuation && continuation->codePoint == Ubiquitty::wideContinuationSentinelCodePoint
            && continuation->clusterID() == 0
            && Ubiquitty::isProportionalRunContinuation(&t, *continuation));
    }
    // A reader sized for a cluster gets the run's first sixteen, and no pool miss.
    std::uint32_t clipped[16] = {};
    CHECK_EQ(anchor ? Ubiquitty::cellClusterCodePoints(&t, anchor, clipped, 16) : 0, 16);
    CHECK(clipped[0] == 'T');
    // Copy takes the whole run: with no measurer the anchor owns every letter.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 0, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 0, 3, 0);
    CHECK(Ubiquitty::Selection::selectedText(selection, t) == sentence);
}

// A shaping client whose letters are 12 px wide in 8 px cells: the layout puts
// three letters in each two cells, so which cell a letter is copied from is the
// layout's answer and not a share of the anchor's text.
struct WideLetterMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t i = 0; i < codePoints.size(); ++i) {
            glyphsOut[i] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[i].glyph = static_cast<std::uint32_t>(i + 1);
            glyphsOut[i].advanceInPixels = 12.0f;
            glyphsOut[i].cluster = static_cast<std::uint16_t>(i);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

// A row's proportional-run flag is what the frame builder reads before it
// looks for a run, and a rows-only resize moves cells between rows. A flag
// that stays with the index instead of the cells sends a run's anchor down the
// mono path, where its multi-cell claim stretches one glyph across every cell
// of the claim.
TEST(aRowsOnlyResizeCarriesTheRunFlagWithItsRow)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    WideLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\033[3;1H");
    feed(t, "\033]66;p=1:c=9;abcdef\007");

    const auto rowOfTheRun = [&]() {
        for (int row = 0; row < t.rows; ++row) {
            const Ubiquitty::Cell* c = cell(t, row, 0);
            if (c && Ubiquitty::isProportionalRunAnchor(&t, *c))
                return row;
        }
        return -1;
    };
    const auto flaggedRows = [&]() {
        int count = 0;
        for (int row = 0; row < t.rows; ++row) {
            if (t.grid && t.grid->rowMeta(row).has(Ubiquitty::RowFlag::Flowed))
                ++count;
        }
        return count;
    };
    const int before = rowOfTheRun();
    REQUIRE(before == 2);
    CHECK(t.grid && t.grid->rowMeta(before).has(Ubiquitty::RowFlag::Flowed));
    CHECK_EQ(flaggedRows(), 1);

    // Rows-only, bottom-anchored, with the reflow gate suppressed: the top row
    // goes to history and every survivor shifts up one.
    Ubiquitty::resize(t, t.rows - 1, t.columns, Ubiquitty::Modes::ReflowMode::Fixed,
        Ubiquitty::Modes::AnchorMode::Bottom);

    const int after = rowOfTheRun();
    CHECK_MSG(after == before - 1, "the run moved up with the shift");
    CHECK_MSG(after >= 0 && t.grid && t.grid->rowMeta(after).has(Ubiquitty::RowFlag::Flowed),
        "and the row it landed on says it may hold a run");
    CHECK_MSG(flaggedRows() == 1, "and no row it left behind still says so");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aShapingMeasurerDecidesWhichCellALetterIsCopiedFrom)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    WideLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Six letters at 12 px: origins 0,12,24,36,48,60 in 8 px cells begin in
    // cells 0,1,3,4,6,7 — nine cells claimed, and no letter begins in cells 2,
    // 5 or 8. The cells carry only the claim now, so the layout is what says
    // which cell a letter is copied from.
    feed(t, "\033]66;p=1:c=9;abcdef\007");
    CHECK_EQ(t.screens.active->cursorX, 9);
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'a' && Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    CHECK_EQ(anchor ? static_cast<int>(Ubiquitty::runTextCodePoints(&t, *anchor).size()) : 0, 6);
    for (int column = 1; column < 9; ++column) {
        const Ubiquitty::Cell* c = cell(t, 0, column);
        CHECK(c && c->codePoint == Ubiquitty::wideContinuationSentinelCodePoint && c->clusterID() == 0);
    }
    // A drag over the cell 'c' begins in copies 'c' and nothing else.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 3, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 0, 3, 0);
    CHECK_MSG(Ubiquitty::Selection::selectedText(selection, t) == "c",
        "the cell a letter's ink begins in is the cell it copies from");
    // And the row is marked as flowed for every reader that maps pixels.
    CHECK(t.grid && t.grid->rowMeta(0).has(Ubiquitty::RowFlag::Flowed));
    const Ubiquitty::Cell* plainRow = cell(t, 1, 0);
    CHECK(plainRow && t.grid && !t.grid->rowMeta(1).has(Ubiquitty::RowFlag::Flowed));
    // A query answers the laid-out width: 72 px in 8 px cells is nine.
    feed(t, "\033]66;p=1:q=1:i=3;abcdef\007");
    char out[128];
    UbiquittyTest::drain(t, out);
    CHECK(std::string(out) == std::string("\033]66;i=3:c=9\007"));
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A shaper setting letters 4 px wide in 8 px cells: two letters a cell, and a
// right-aligned run's letters sit in its last cells.
struct NarrowLetterMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t i = 0; i < codePoints.size(); ++i) {
            glyphsOut[i] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[i].glyph = static_cast<std::uint32_t>(i + 1);
            glyphsOut[i].advanceInPixels = 4.0f;
            glyphsOut[i].cluster = static_cast<std::uint16_t>(i);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

TEST(aRightAlignedRunKeepsItsTextWholeAndOnceWhenTheAnchorOwnsNoLetter)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    NarrowLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // "abcd" is 16 px; in four 8 px cells right-aligned it begins at 16 px,
    // cell 2, so the letters are copied from cells 2, 2, 3 and 3 — and the
    // text reads back whole, once, from the anchor, which stores it.
    feed(t, "\033]66;p=1:c=4:h=1;abcd\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && Ubiquitty::cellPen(*anchor).attributes.contains(Ubiquitty::VT::Attribute::Wide));
    const std::span<const std::uint32_t> stored = anchor
        ? Ubiquitty::runTextCodePoints(&t, *anchor)
        : std::span<const std::uint32_t> {};
    CHECK_EQ(static_cast<int>(stored.size()), 4);
    CHECK_MSG(stored.size() == 4 && stored[0] == 'a' && stored[1] == 'b' && stored[2] == 'c' && stored[3] == 'd',
        "the text reads back whole and once");
    const Ubiquitty::Cell* second = cell(t, 0, 1);
    CHECK(second && second->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
    // A drag between cells that own letters takes those letters: cell 2 owns
    // `ab` and cell 3 owns `cd`.
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 2, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 0, 3, 0);
    const std::string owned = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(owned == "abcd", ("the cells' own letters copy: " + owned).c_str());
    // An endpoint on the slack before the text — a cell no letter begins in
    // — is on a blank cell of its own, and the drag from it takes the
    // letters it reaches.
    Ubiquitty::Selection::Session fromSlack;
    Ubiquitty::Selection::init(fromSlack);
    Ubiquitty::Selection::beginGesture(fromSlack, t, 0, 1, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(fromSlack, t, 0, 2, 0);
    const std::string reached = Ubiquitty::Selection::selectedText(fromSlack, t);
    CHECK_MSG(reached == "ab", ("a drag from the slack takes the letters it reaches: " + reached).c_str());
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

namespace {

/// The caret a pointer lands on, resolved the way a frontend resolves one:
/// the geometry rounds to the nearest cell boundary, and the model answers
/// what that point means on this terminal.
///
/// - Parameters:
///   - t: The terminal to read.
///   - visibleRow: The pointer's row in the viewport.
///   - xInCells: The point along the row, in cells from the grid's left edge.
/// - Returns: The resolution.
Ubiquitty::Selection::CaretResolution caretAtPoint(Ubiquitty::Terminal& t, int visibleRow, double xInCells)
{
    int boundary = static_cast<int>(xInCells + 0.5);
    boundary = boundary < 0 ? 0 : (boundary > t.columns ? t.columns : boundary);
    return Ubiquitty::Selection::resolveSelectionCaret(t, 0, visibleRow, boundary, xInCells);
}

} // namespace

// A proportional run packs several letters' ink into one cell, so a cell
// boundary names a place up to a whole cell away from where the reader
// pointed. The run's layout is the only thing that knows where its letters
// sit, and it is what answers here.
TEST(aPointerInsideAPackedCellResolvesToTheLetterItsPixelsFallOn)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    NarrowLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Eight 4 px letters in 8 px cells: `ab` in cell 0, `cd` in cell 1, `ef`
    // in cell 2, `gh` in cell 3.
    feed(t, "\033]66;p=1:c=4;abcdefgh\007");
    feed(t, "\r\nplain text");

    // On the first letter: the run's start, nothing of the cell to the
    // caret's left, and the letter's far edge one letter in.
    CHECK_EQ(caretAtPoint(t, 0, 0.4 / 8.0).caret, 0);
    CHECK_EQ(caretAtPoint(t, 0, 0.4 / 8.0).subCaret, 0);
    CHECK_EQ(caretAtPoint(t, 0, 0.4 / 8.0).subCaretAfter, 1);
    // On the second letter: the same cell, one letter in, and the boundary
    // after it is the next cell's. The nearest cell boundary would have said
    // cell 0 with both letters to its right.
    CHECK_EQ(caretAtPoint(t, 0, 4.1 / 8.0).caret, 0);
    CHECK_MSG(caretAtPoint(t, 0, 4.1 / 8.0).subCaret == 1,
        "the caret stands between the two letters of one cell");
    CHECK_EQ(caretAtPoint(t, 0, 4.1 / 8.0).caretAfter, 1);
    CHECK_EQ(caretAtPoint(t, 0, 4.1 / 8.0).subCaretAfter, 0);
    // On the third letter: the next cell's boundary.
    CHECK_EQ(caretAtPoint(t, 0, 8.1 / 8.0).caret, 1);
    CHECK_EQ(caretAtPoint(t, 0, 8.1 / 8.0).subCaret, 0);
    // On the last letter: its cell, one letter in, and after it the boundary
    // after the last cell owning a letter.
    CHECK_EQ(caretAtPoint(t, 0, 31.2 / 8.0).caret, 3);
    CHECK_EQ(caretAtPoint(t, 0, 31.2 / 8.0).subCaret, 1);
    CHECK_EQ(caretAtPoint(t, 0, 31.2 / 8.0).caretAfter, 4);
    CHECK_EQ(caretAtPoint(t, 0, 31.2 / 8.0).subCaretAfter, 0);
    // Past the run's ink: that boundary on both sides, so a gesture there is
    // between boundaries.
    CHECK_EQ(caretAtPoint(t, 0, 33.0 / 8.0).caret, 4);
    CHECK_EQ(caretAtPoint(t, 0, 33.0 / 8.0).subCaret, 0);
    CHECK_EQ(caretAtPoint(t, 0, 33.0 / 8.0).caretAfter, 4);
    // A row of ordinary cells keeps the boundary the geometry rounded to, and
    // carries nothing inside a cell.
    CHECK_EQ(caretAtPoint(t, 1, 2.6).caret, 3);
    CHECK_EQ(caretAtPoint(t, 1, 2.6).subCaret, 0);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// The gesture the pointer ladder drives for a plain single click, over a cell
// holding two letters' ink: by letters, the pressed one and every one the
// pointer reaches, so a drag that stays on one letter takes that letter alone
// and one onto the next takes both.
TEST(aDragOverTheLettersOfOneCellCopiesThoseItWasOn)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    NarrowLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\033]66;p=1:c=4;abcdefgh\007");

    const auto dragBetween = [&](double fromInCells, double toInCells) {
        Ubiquitty::Selection::Session selection;
        Ubiquitty::Selection::init(selection);
        Ubiquitty::Selection::beginGesture(selection, t, 0, static_cast<int>(fromInCells), /*clickCount*/ 1, 0, {});
        Ubiquitty::Selection::configureCharacterCaretGesture(selection, t, caretAtPoint(t, 0, fromInCells));
        Ubiquitty::Selection::dragCharacterCaretGesture(selection, t, caretAtPoint(t, 0, toInCells), 0);
        return Ubiquitty::Selection::selectedText(selection, t);
    };

    const std::string first = dragBetween(0.4 / 8.0, 2.1 / 8.0);
    CHECK_MSG(first == "a", ("a drag that stays on the first letter of a packed cell takes it alone: " + first).c_str());
    const std::string second = dragBetween(4.1 / 8.0, 6.0 / 8.0);
    CHECK_MSG(second == "b", ("the second letter of that cell, alone: " + second).c_str());
    const std::string both = dragBetween(0.4 / 8.0, 4.1 / 8.0);
    CHECK_MSG(both == "ab", ("a drag from the first letter onto the second takes both: " + both).c_str());
    const std::string across = dragBetween(4.1 / 8.0, 8.1 / 8.0);
    CHECK_MSG(across == "bc", ("a drag from inside one cell onto the next cell's first letter: " + across).c_str());
    const std::string back = dragBetween(8.1 / 8.0, 4.1 / 8.0);
    CHECK_MSG(back == "bc", ("and the same two letters the other way round: " + back).c_str());

    // The first motion on the pressed letter is the drag: that letter is
    // selected without the pointer having to reach the next.
    Ubiquitty::Selection::Session held;
    Ubiquitty::Selection::init(held);
    Ubiquitty::Selection::beginGesture(held, t, 0, 0, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::configureCharacterCaretGesture(held, t, caretAtPoint(t, 0, 4.1 / 8.0));
    CHECK_MSG(Ubiquitty::Selection::dragCharacterCaretGesture(held, t, caretAtPoint(t, 0, 5.0 / 8.0), 0),
        "a drag that stays on one letter of a packed cell selects that letter");
    CHECK(Ubiquitty::Selection::hasGestureDragBegun(held));
    CHECK_STR_EQ(Ubiquitty::Selection::selectedText(held, t), "b");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A bold face is wider than its regular, and the measurer is asked for the
// face the run is set in: a letter of a bold run is owned by the cell the
// bold layout begins it in, and a point on the row names the letter the bold
// layout puts there. Measured in the regular face, the owners and the pointer
// would both be a letter off from the ink the frame draws.
struct FaceWidthMeasurer final : Ubiquitty::TextMeasureClient {
    std::uint8_t lastFaceBits = 0xFF;
    std::uint32_t lastWeight = 0xFFFF;
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        lastFaceBits = style.faceBits;
        lastWeight = style.weight;
        // Four pixels a letter in the regular face, six in bold.
        const float advance = (style.faceBits & 1u) ? 6.0f : 4.0f;
        for (std::size_t i = 0; i < codePoints.size(); ++i) {
            glyphsOut[i] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[i].glyph = static_cast<std::uint32_t>(i + 1);
            glyphsOut[i].advanceInPixels = advance;
            glyphsOut[i].cluster = static_cast<std::uint16_t>(i);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

TEST(aBoldRunIsMeasuredInTheBoldFace)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    FaceWidthMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Bold, then the run; the pen's face rides the run's anchor.
    feed(t, "\033[1m\033]66;p=1:c=6;abcdefgh\033\\\033[0m");
    // Six pixels a letter in 8 px cells: `a` in cell 0, `b` in 0, `c` in 1,
    // `d` in 2, `e` in 3, `f` in 3, `g` in 4, `h` in 5. In the regular face
    // the same letters would be two to a cell.
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    REQUIRE(anchor);
    std::vector<std::uint16_t> owners(8, 0);
    const Ubiquitty::ProportionalRunStyle style = Ubiquitty::OSC::TextSizing::runStyleOf(Ubiquitty::cellPen(*anchor));
    CHECK_MSG((style.faceBits & 1u) != 0, "the anchor's pen is bold");
    CHECK(Ubiquitty::OSC::TextSizing::proportionalRunCellOwners(t, Ubiquitty::runTextCodePoints(&t, *anchor), 6, 0, style, owners));
    CHECK_MSG(measurer.lastFaceBits == 1 && measurer.lastWeight == 0, "the measurer is asked for the bold face");
    const std::vector<std::uint16_t> expected { 0, 0, 1, 2, 3, 3, 4, 5 };
    CHECK_MSG(owners == expected, "the letters are owned by the cells the bold layout begins them in");
    // A point 13 px in is on `c` (12..18) in bold; in the regular face it
    // would be on `d` (12..16).
    const Ubiquitty::Selection::CaretResolution onC = caretAtPoint(t, 0, 13.0 / 8.0);
    CHECK_EQ(onC.caret, 1);
    CHECK_EQ(onC.subCaret, 0);
    CHECK_EQ(onC.caretAfter, 2);
    CHECK_EQ(onC.subCaretAfter, 0);

    // The SGR 1:n weight step rides the same way, as the renderer's variable
    // weight does, without the bold bit.
    feed(t, "\r\n\033[1:3m\033]66;p=1:c=6;abcdefgh\033\\\033[0m");
    const Ubiquitty::Cell* weighted = cell(t, 1, 0);
    REQUIRE(weighted);
    const Ubiquitty::ProportionalRunStyle weightedStyle = Ubiquitty::OSC::TextSizing::runStyleOf(Ubiquitty::cellPen(*weighted));
    CHECK_EQ(static_cast<int>(weightedStyle.faceBits), 0);
    CHECK_EQ(static_cast<int>(weightedStyle.weight), 3);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A caret stands between graphemes, not between code points: a combining mark
// is drawn on the letter it follows, and no pointer position names the gap
// between the two.
TEST(aCaretInsideARunNeverStandsBetweenALetterAndItsMark)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    NarrowLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // `a`, a combining acute, `b`, `c`: four code points, three graphemes,
    // 16 px of ink in two 8 px cells.
    feed(t, std::string("\033]66;p=1:c=2;a\xcc\x81") + "bc\007");

    // The mark's own glyph box holds pixels, and a point on it is on the
    // letter it belongs to: the cluster's start before, its far edge after.
    CHECK_EQ(caretAtPoint(t, 0, 5.0 / 8.0).caret, 0);
    CHECK_MSG(caretAtPoint(t, 0, 5.0 / 8.0).subCaret == 0,
        "a point on a mark is on the letter it belongs to");
    CHECK_EQ(caretAtPoint(t, 0, 7.0 / 8.0).caret, 0);
    CHECK_EQ(caretAtPoint(t, 0, 7.0 / 8.0).subCaret, 0);
    CHECK_MSG(caretAtPoint(t, 0, 7.0 / 8.0).caretAfter == 1 && caretAtPoint(t, 0, 7.0 / 8.0).subCaretAfter == 0,
        "the letter's far edge is the cluster's, not the mark's start");

    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 0, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::configureCharacterCaretGesture(selection, t, caretAtPoint(t, 0, 0.4 / 8.0));
    Ubiquitty::Selection::dragCharacterCaretGesture(selection, t, caretAtPoint(t, 0, 7.0 / 8.0), 0);
    const std::string cluster = Ubiquitty::Selection::selectedText(selection, t);
    CHECK_MSG(cluster == std::string("a\xcc\x81"), ("the whole cluster copies, mark included: " + cluster).c_str());
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aRightAlignedRunCanPlaceTextPastTheFirstTwoHundredFiftySixCells)
{
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 24, 300);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    NarrowLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Four 4 px letters occupy the last two cells of a 300-cell claim: `a`
    // and `b` begin in cell 298, `c` and `d` in cell 299.
    feed(t, "\033]66;p=1:c=300:h=1;abcd\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'a');
    CHECK_EQ(anchor ? static_cast<int>(Ubiquitty::runTextCodePoints(&t, *anchor).size()) : 0, 4);
    const Ubiquitty::Cell* penultimate = cell(t, 0, 298);
    CHECK(penultimate && penultimate->codePoint == Ubiquitty::wideContinuationSentinelCodePoint
        && penultimate->clusterID() == 0);
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, 0, 298, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, t, 0, 299, 0);
    CHECK_MSG(Ubiquitty::Selection::selectedText(selection, t) == "abcd",
        "selection and copy read the letters by the cells their ink begins in");
    // The claim ends at the right edge: the cursor stands on its last cell
    // with the wrap pending, as after a sized block.
    CHECK_EQ(t.screens.active->cursorX, 299);
    CHECK(t.screens.active->wrapPending);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aThousandColumnRunKeepsEveryLetterOnOneAnchor)
{
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 24, 1024);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Two thousand characters over a 1024-cell claim: more characters than
    // cells, and one entry holds them.
    const std::string text(2000, 'x');
    feed(t, "\033]66;p=1:c=1024;" + text + "\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    CHECK_EQ(anchor ? static_cast<int>(Ubiquitty::runTextCodePoints(&t, *anchor).size()) : 0, 2000);
    // The claim fills the row: the cursor stands on its last cell with the
    // wrap pending.
    CHECK_EQ(t.screens.active->cursorX, 1023);
    CHECK(t.screens.active->wrapPending);
    const Ubiquitty::Cell* last = cell(t, 0, 1023);
    CHECK(last && Ubiquitty::isProportionalRunContinuation(&t, *last));
}

TEST(aRunsTextIsFreedWhenTheScrollbackDropsItsAnchor)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033]66;p=1:c=4;The quick brown fox\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    const std::uint16_t id = anchor ? anchor->clusterID() : 0;
    CHECK(id != 0);
    CHECK(!t.runTextStore.text(id).empty());
    // Scroll the run into history, then drop the history: the entry no cell can
    // show is freed with the lines that named it.
    for (int row = 0; row < t.rows + 2; ++row)
        feed(t, "\r\n");
    CHECK(!t.runTextStore.text(id).empty());
    Ubiquitty::Screen::dropScrollback(&t);
    CHECK(t.runTextStore.text(id).empty());
    CHECK_EQ(static_cast<int>(t.runTextStore.liveCodePoints()), 0);
}

TEST(aRunWiderThanTheNewRowIsClampedByReflowRatherThanDropped)
{
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 24, 20);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // One word: a run with a space to break at breaks there instead.
    const std::string sentence = "clamped_not_dropped";
    feed(t, "\033]66;p=1:c=8;" + sentence + "\007");
    const Ubiquitty::Cell* placed = cell(t, 0, 0);
    CHECK(placed && Ubiquitty::isProportionalRunAnchor(&t, *placed));
    // Narrowing past the run's claim: its text rides the anchor, so the run is
    // clamped to the row under a spec claiming the new width instead of being
    // dropped for having no row it fits on.
    Ubiquitty::resize(t, 24, 4);
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && Ubiquitty::isProportionalRunAnchor(&t, *anchor));
    const std::span<const std::uint32_t> stored = anchor
        ? Ubiquitty::runTextCodePoints(&t, *anchor)
        : std::span<const std::uint32_t> {};
    CHECK_EQ(static_cast<int>(stored.size()), static_cast<int>(sentence.size()));
    CHECK_EQ(anchor ? Ubiquitty::claimedRunColumns(*anchor, &t.textSizeTable, t.ambiguousWidth) : 0, 4);
    for (int column = 1; column < 4; ++column) {
        const Ubiquitty::Cell* continuation = cell(t, 0, column);
        CHECK(continuation && Ubiquitty::isProportionalRunContinuation(&t, *continuation));
    }
    // The chain is as long as its claim, so the repair that follows an edit
    // keeps it rather than blanking a run it reads as torn.
    feed(t, "\033[2;1H\033[K");
    const Ubiquitty::Cell* afterEdit = cell(t, 0, 0);
    CHECK(afterEdit && Ubiquitty::isProportionalRunAnchor(&t, *afterEdit));
}

/// A reflow that must clamp a run past a full spec table takes the census
/// first. Fifteen widening overwrites of one field leave the table full of
/// specs no cell names; a resize narrower than the surviving run's claim
/// interns a spec for the new width, which a full table refuses — unless the
/// ids nothing names are freed before the feed, over the cells that still
/// exist, as an OSC 66 mint on the same table frees them. Widths 5 to 19, so
/// the clamped width is a spec the table does not already hold.
TEST(aReflowClampPastAFullSpecTableTakesTheCensusFirst)
{
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 24, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    for (int cells = 5; cells <= 19; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[H\033]66;p=1:c=%d;field\007", cells);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);
    const Ubiquitty::Cell* placed = cell(t, 0, 0);
    CHECK(placed && Ubiquitty::isProportionalRunAnchor(&t, *placed));

    Ubiquitty::resize(t, 24, 4);
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK_MSG(anchor && Ubiquitty::isProportionalRunAnchor(&t, *anchor), "the run survives the narrowing, clamped rather than dropped");
    CHECK_EQ(anchor ? Ubiquitty::claimedRunColumns(*anchor, &t.textSizeTable, t.ambiguousWidth) : 0, 4);
    const std::span<const std::uint32_t> stored = anchor
        ? Ubiquitty::runTextCodePoints(&t, *anchor)
        : std::span<const std::uint32_t> {};
    CHECK_EQ(static_cast<int>(stored.size()), 5);
    CHECK_MSG(t.textSizeTable.liveCount() <= 2, "the census freed the ids nothing named before the clamp asked for one");
}

TEST(aSixteenthProportionalRunStoresOnceTheOldSpecsAreOverwritten)
{
    // A dashboard redrawing one field through fifteen distinct widths leaves
    // the table full of specs no cell names. The sixteenth distinct run takes
    // one of those ids back rather than printing unsized for the rest of the
    // session.
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Widening claims over the same origin: each run covers every cell the one
    // before it held, so only the latest spec stays reachable.
    for (int cells = 2; cells <= 16; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[H\033]66;p=1:c=%d;field\007", cells);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);

    feed(t, "\033[H\033]66;p=1:c=16:h=1;field\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'f');
    const unsigned id = anchor ? Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() : 0;
    CHECK_MSG(id != 0, "the sixteenth distinct run printed unsized over a table of unreachable specs");
    const TextSizeSpec* spec = t.textSizeTable.lookup(id);
    CHECK(spec && spec->isProportional && spec->runWidthCells == 16 && spec->horizontalAlign == 1);
    // The renderer's reading of the run: sixteen claimed cells on a flowed row.
    CHECK(anchor && Ubiquitty::claimedRunColumns(*anchor, &t.textSizeTable, t.ambiguousWidth) == 16);
    CHECK(t.grid && t.grid->rowMeta(0).has(Ubiquitty::RowFlag::Flowed));
    CHECK_EQ(t.screens.active->cursorX, 16);
}

/// A flattened proportional anchor drops the run-text id its cluster field
/// carried. The id names a run-text entry only while the spec says the cell
/// is a run; flattened, the cell is plain and the same field reads as a
/// cluster-pool id — another run's letters, or an emoji's tail, under the
/// first letter the flatten meant to keep.
TEST(aFlattenedProportionalAnchorDropsItsRunTextID)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[H\033]66;p=1:c=4;abcd\007");
    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    CHECK(anchor && anchor->codePoint == 'a' && anchor->clusterID() != 0);
    CHECK(anchor && Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() != 0);

    std::array<Ubiquitty::Cell, 8> row {};
    for (int column = 0; column < 8; ++column) {
        const Ubiquitty::Cell* source = cell(t, 0, column);
        row[static_cast<std::size_t>(column)] = source ? *source : Ubiquitty::defaultBlankCell;
    }
    CHECK(Ubiquitty::Screen::flattenSizedRun(row.data(), 8, Ubiquitty::defaultBlankCell, nullptr, nullptr,
        Ubiquitty::allTextSizeSpecIDs, t.textSizeTable));
    CHECK_MSG(row[0].codePoint == 'a', "the anchor keeps the first letter, as the plain cell it becomes");
    CHECK_MSG(Ubiquitty::cellPen(row[0]).extendedAttributes.textSizeSpecID() == 0, "and no spec");
    CHECK_MSG(row[0].clusterID() == 0, "and no run-text id for a cluster-pool read to misresolve");
}

TEST(aReclaimNeverHandsAnIDTheScrollbackNamesToAnotherSpec)
{
    // A history row names its spec by slot and is drawn again whenever the view
    // scrolls back to it, so an id the scrollback carries is live with nothing
    // of it on screen. A census that freed it would hand the slot to the next
    // distinct spec and redraw that row at the new geometry.
    Ubiquitty::Terminal t;
    reset(t);
    Ubiquitty::resize(t, 4, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Fifteen distinct runs, one per line: twelve age into history and three
    // stay on screen.
    for (int cells = 1; cells <= 15; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033]66;p=1:c=%d;r\007\r\n", cells);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);
    std::vector<std::pair<unsigned, TextSizeSpec>> history;
    for (int line = 0; line < Ubiquitty::Screen::scrollbackCount(&t); ++line) {
        const Ubiquitty::Screen::ScrollLine* stored = Ubiquitty::Screen::scrollbackLine(&t, line);
        for (int column = 0; stored && column < stored->length; ++column) {
            const unsigned id = Ubiquitty::cellPen(stored->cells[column]).extendedAttributes.textSizeSpecID();
            if (const TextSizeSpec* spec = id != 0 ? t.textSizeTable.lookup(id) : nullptr)
                history.emplace_back(id, *spec);
        }
    }
    CHECK_MSG(!history.empty(), "premise: sized runs reached the scrollback with live ids");

    // Erasing the three screen rows retires their specs; the history keeps its.
    feed(t, "\033[1;1H\033[2K\033[2;1H\033[2K\033[3;1H\033[2K\033[H");
    feed(t, "\033]66;p=1:c=20;new\007");

    const Ubiquitty::Cell* anchor = cell(t, 0, 0);
    const unsigned newID = anchor ? Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID() : 0;
    CHECK_MSG(newID != 0, "the census found the erased specs and still refused");
    for (const auto& [id, spec] : history) {
        CHECK_MSG(id != newID, "the new spec minted into a slot a history row names");
        const TextSizeSpec* now = t.textSizeTable.lookup(id);
        CHECK_MSG(now && *now == spec, "a history cell resolves to a different specification after the reclaim");
    }
}

TEST(aReclaimKeepsTheIDTheStatusRowNames)
{
    // A one-cell sized run written while DECSASD directs output to the status
    // line stamps its id into the status row, and the renderer draws that row
    // through the same table. A census that skipped the row would free the id
    // with the status line still showing it.
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\033[2$~\033[1$}\033]66;n=1:d=2;S\007\033[0$}");
    const std::span<const Ubiquitty::Cell> status = Ubiquitty::Rendering::statusRow(t);
    const unsigned statusID = status.empty() ? 0 : Ubiquitty::cellPen(status[0]).extendedAttributes.textSizeSpecID();
    CHECK_MSG(statusID != 0, "premise: the status row names a spec");
    const TextSizeSpec* statusSpec = t.textSizeTable.lookup(statusID);
    CHECK(statusSpec != nullptr);
    const TextSizeSpec recorded = statusSpec ? *statusSpec : TextSizeSpec {};

    // Fourteen distinct fractions over one data-screen cell fill the table, and
    // only the last of them stays named there.
    for (int numerator = 1; numerator <= 14; ++numerator) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\033[H\033]66;n=%d:d=15;A\007", numerator);
        feed(t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);

    feed(t, "\033[2;1H\033]66;n=1:d=3;B\007");
    const Ubiquitty::Cell* landed = cell(t, 1, 0);
    const unsigned newID = landed ? Ubiquitty::cellPen(*landed).extendedAttributes.textSizeSpecID() : 0;
    CHECK_MSG(newID != 0, "the census found the overwritten fractions and still refused");
    CHECK_MSG(newID != statusID, "the new spec minted into the slot the status row names");
    const TextSizeSpec* now = t.textSizeTable.lookup(statusID);
    CHECK_MSG(now && *now == recorded, "the status row's id resolves to a different specification after the reclaim");
}

TEST(sizedTextConsumesPendingWrapAndDiscardsExcessHeight)
{
    Ubiquitty::Terminal t;
    reset(t);
    t.drawsSizedText = true;
    Ubiquitty::resize(t, 2, 6);
    feed(t, "ABCDEF\033]66;w=2;x\007G");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 0)->codePoint == 'x');
    CHECK(cell(t, 1, 2)->codePoint == 'G');
    CHECK(t.screens.active->cursorX == 3);
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "\033[1;1H\033]66;s=3;BIG\007");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033[?7l\033[1;6H\033]66;w=2;z\007");
    CHECK(cell(t, 0, 4)->codePoint == 'z');
    CHECK(t.screens.active->cursorY == 0);
}

TEST(rendererDeclarationsSurviveRIS)
{
    for (const bool drawsSized : { false, true }) {
        for (const bool drawsProportional : { false, true }) {
            Ubiquitty::Terminal terminal;
            terminal.drawsSizedText = drawsSized;
            terminal.drawsProportionalText = drawsProportional;
            feed(terminal, "\033c\033]66;p=1:q=1;i\007");
            CHECK(terminal.drawsSizedText == drawsSized);
            CHECK(terminal.drawsProportionalText == drawsProportional);
            char out[128];
            CHECK((UbiquittyTest::drain(terminal, out) != 0) == drawsProportional);
            feed(terminal, "\033]66;p=1:c=4;AB\007");
            CHECK_EQ(terminal.screens.active->cursorX, drawsProportional ? 4 : 2);
            feed(terminal, "\r\n\033]66;s=2;X\007");
            CHECK_EQ(terminal.screens.active->cursorX, drawsSized ? 2 : 1);
        }
    }
}

TEST(ordinaryTextSkipsEveryLowerBandWithEitherAutowrapMode)
{
    for (int scale = 2; scale <= 7; ++scale) {
        for (const bool isAutowrapEnabled : { false, true }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::resize(terminal, scale + 1, scale + 4);
            terminal.drawsSizedText = true;
            if (!isAutowrapEnabled)
                feed(terminal, "\033[?7l");
            feed(terminal, "\033]66;s=" + std::to_string(scale) + ";X\033\\");
            for (int band = 1; band < scale; ++band) {
                feed(terminal, "\033[" + std::to_string(band + 1) + ";1Habc");
                CHECK_EQ(cell(terminal, band, 0)->codePoint, 'X');
                CHECK_EQ(Ubiquitty::penTextBand(&cell(terminal, band, 0)->pen), band);
                CHECK_EQ(cell(terminal, band, scale)->codePoint, 'a');
                CHECK_EQ(cell(terminal, band, scale + 2)->codePoint, 'c');
            }
        }
    }
}

TEST(writingPastABlockAtTheRightEdgeWrapsEvenWithAutowrapDisabled)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 4);
    terminal.drawsSizedText = true;
    feed(terminal, "\033]66;s=2:w=2;X\033\\\033[?7l\033[2;1HZ");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 'X');
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'X');
    CHECK_EQ(cell(terminal, 2, 0)->codePoint, 'Z');
    CHECK_EQ(terminal.screens.active->cursorY, 2);
    CHECK_EQ(terminal.screens.active->cursorX, 1);
}

TEST(batchedTextAndRepeatsStopBeforeLowerBands)
{
    for (const std::string& text : { std::string("abcdef"), std::string("a\xcc\x81"
                                                                        "b\xcc\x81"
                                                                        "c\xcc\x81"
                                                                        "d\xcc\x81"),
             std::string("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"), std::string("A\033[5b") }) {
        for (const bool isFragmented : { false, true }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::resize(terminal, 3, 12);
            terminal.drawsSizedText = true;
            feed(terminal, "\033[1;4H\033]66;s=2;X\033\\\033[2;1H");
            if (isFragmented) {
                for (const char byte : text)
                    feed(terminal, std::string_view(&byte, 1));
            } else
                feed(terminal, text);
            CHECK_EQ(cell(terminal, 1, 3)->codePoint, 'X');
            CHECK_EQ(Ubiquitty::penTextBand(&cell(terminal, 1, 3)->pen), 1);
            CHECK(cell(terminal, 1, 5)->codePoint != 0);
        }
    }
    for (const std::string& text : { std::string("\xe4\xb8\xad\xe6\x96\x87"), std::string("\xf0\x9f\x98\x80\xf0\x9f\x98\x80") }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 3, 12);
        terminal.drawsSizedText = true;
        feed(terminal, "\033[1;4H\033]66;s=2;X\033\\\033[2;1H" + text);
        CHECK_EQ(cell(terminal, 1, 3)->codePoint, 'X');
        CHECK_EQ(Ubiquitty::penTextBand(&cell(terminal, 1, 3)->pen), 1);
        CHECK(cell(terminal, 1, 5)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK_EQ(terminal.screens.active->cursorX, 7);
    }
}

TEST(lowerBandsWrapAtMarginsAndScrollAtTheBottom)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 8);
    terminal.drawsSizedText = true;
    feed(terminal, "\033[?69h\033[3;6s\033[2;4r\033[3;3H\033]66;s=2:w=2;X\033\\\033[?7l\033[4;3HZ");
    CHECK_EQ(cell(terminal, 1, 2)->codePoint, 'X');
    CHECK_EQ(cell(terminal, 2, 2)->codePoint, 'X');
    CHECK_EQ(Ubiquitty::penTextBand(&cell(terminal, 2, 2)->pen), 1);
    CHECK_EQ(cell(terminal, 3, 2)->codePoint, 'Z');
    CHECK_EQ(terminal.screens.active->cursorY, 3);
    CHECK_EQ(terminal.screens.active->cursorX, 3);
}

TEST(lowerBandSkippingStopsAtThePhysicalBottomOutsideTheScrollRegion)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 4);
    terminal.drawsSizedText = true;
    feed(terminal, "\033[3;1H\033]66;s=2:w=2;X\033\\\033[1;2r\033[4;1HZ");
    CHECK_EQ(cell(terminal, 2, 0)->codePoint, 'X');
    CHECK_EQ(cell(terminal, 3, 0)->codePoint, 'X');
    CHECK_EQ(terminal.screens.active->cursorY, 3);
    CHECK_EQ(terminal.screens.active->cursorX, 0);
}

TEST(explicitWidthsKeepWholeUnicodeClustersAcrossEveryBand)
{
    const std::array<std::vector<std::uint32_t>, 8> clusters { { { 0x754C }, { 0x1F680 }, { 0x2665, 0xFE0F }, { 0x1F469, 0x200D, 0x1F4BB },
        { 0x1F1EC, 0x1F1E7 }, { 0x915, 0x93E }, { 0xE01, 0xE33 }, { '1', 0xFE0F, 0x20E3 } } };
    const std::array<std::string_view, 8> texts { "界", "🚀", "♥️", "👩‍💻", "🇬🇧", "का", "กำ", "1️⃣" };
    for (std::size_t index = 0; index < texts.size(); ++index) {
        for (int scale : { 1, 2, 7 }) {
            for (int width = 1; width <= 7; ++width) {
                Ubiquitty::Terminal terminal;
                Ubiquitty::resize(terminal, 8, 52);
                terminal.drawsSizedText = true;
                feed(terminal, "\033]66;s=" + std::to_string(scale) + ":w=" + std::to_string(width) + ";" + std::string(texts[index]) + "\033\\");
                CHECK_EQ(terminal.screens.active->cursorX, scale * width);
                CHECK_EQ(terminal.screens.active->cursorY, 0);
                for (int band = 0; band < scale; ++band) {
                    std::array<std::uint32_t, Ubiquitty::clusterPoolMaximumCodePoints> points {};
                    const auto* anchor = cell(terminal, band, 0);
                    REQUIRE(anchor);
                    const int count = Ubiquitty::cellClusterCodePoints(&terminal, anchor, points.data(), static_cast<int>(points.size()));
                    CHECK_EQ(count, static_cast<int>(clusters[index].size()));
                    for (int at = 0; at < count; ++at)
                        CHECK_EQ(points[at], clusters[index][at]);
                    CHECK_EQ(Ubiquitty::penTextBand(&anchor->pen), band);
                }
            }
        }
    }
}

TEST(scaledTextSegmentsClustersBeforePlacingBlocks)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 20);
    terminal.drawsSizedText = true;
    feed(terminal, "\033]66;s=2;👩‍💻🇬🇧é\033\\");
    CHECK_EQ(terminal.screens.active->cursorX, 10);
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0x1F469u);
    CHECK_EQ(cell(terminal, 0, 4)->codePoint, 0x1F1ECu);
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, static_cast<std::uint32_t>('e'));
    CHECK_EQ(cell(terminal, 1, 8)->combining[0], 0x301u);
}

TEST(combiningTextAfterSizedBlocksKeepsTheirWidthAndEveryBand)
{
    for (int scale : { 1, 3 }) {
        for (const bool isFragmented : { false, true }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::resize(terminal, 4, 20);
            terminal.drawsSizedText = true;
            feed(terminal, "\033]66;s=" + std::to_string(scale) + ":w=1;♥\033\\");
            if (isFragmented) {
                for (const char byte : std::string("️́̈"))
                    feed(terminal, std::string_view(&byte, 1));
            } else
                feed(terminal, "️́̈");
            CHECK_EQ(terminal.screens.active->cursorX, scale);
            for (int band = 0; band < scale; ++band) {
                const auto* anchor = cell(terminal, band, 0);
                std::array<std::uint32_t, 16> points {};
                CHECK_EQ(Ubiquitty::cellClusterCodePoints(&terminal, anchor, points.data(), static_cast<int>(points.size())), 4);
                CHECK_EQ(points[1], 0xFE0Fu);
                CHECK_EQ(points[3], 0x308u);
                CHECK(anchor->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
