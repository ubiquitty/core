// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_accessibility_text_projection — the visible-grid → text projection.
//
// The contract every screen-read answers to: VoiceOver, AT-SPI, agent/MCP, and
// the control socket's get-text all read through it.

#include "Harness.h"

#include "Core/Accessibility/TextProjection.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <string>

namespace {

using Ubiquitty::Accessibility::caretInProjection;
using Ubiquitty::Accessibility::projectVisibleText;
using Ubiquitty::Accessibility::TextProjection;
using Ubiquitty::Accessibility::visibleScreenText;

void feed(Ubiquitty::Terminal& t, const char* s)
{
    Ubiquitty::processBytes(&t, s);
}

// Rows join with '\n', trailing spaces trim, ranges are cumulative UTF-16
// offsets.
TEST(testRowJoinAndRanges)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "Hi\r\nWorld\r\n"); // row0 "Hi", row1 "World", row2 "" (cursor)

    const TextProjection p = projectVisibleText(t, 0);
    CHECK_MSG(p.text == "Hi\nWorld\n", "rows join with a newline and trailing blanks trim");
    CHECK_MSG(p.lineRanges.size() == 3, "one range per visible row");
    CHECK_MSG(p.lineRanges[0].location == 0 && p.lineRanges[0].length == 2, "row 0 is \"Hi\"");
    CHECK_MSG(p.lineRanges[1].location == 3 && p.lineRanges[1].length == 5, "row 1 starts after \"Hi\\n\"");
    CHECK_MSG(p.lineRanges[2].location == 9 && p.lineRanges[2].length == 0, "row 2 is empty");
}

// An astral codepoint is one scalar but two UTF-16 units. Ranges are not byte
// offsets.
TEST(testAstralCodePointCountsAsTwoUTF16Units)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t,
        "\xF0\x9F\x98\x80"
        "X"); // U+1F600 (wide) then 'X'

    const TextProjection p = projectVisibleText(t, 0);
    CHECK_MSG(!p.lineRanges.empty(), "the row projected");
    CHECK_MSG(p.lineRanges[0].location == 0 && p.lineRanges[0].length == 3,
        "2 UTF-16 units for the emoji plus 1 for 'X'");
}

// A wide glyph occupies two cells and must appear once. Asserted on bytes:
// that is what the agent and the control socket return.
// The UTF-16 count and the UTF-8 bytes agree on a value that is not a scalar.
//
// utf16Units indexes the string appendUTF8 built. appendUTF8 emits ZERO bytes
// above U+10FFFF and for the surrogate range; utf16Units returned 2 for the
// former. So one cell holding such a value made every later row's lineRange
// offset wrong by the difference, and the ranges are what a screen reader uses to
// map a selection back to text.
//
// Cells now arrive validated from both producers, so this is defence in depth
// rather than a live path — and it is asserted here rather than assumed, because
// the reason the drift existed was that each side was correct on its own and
// nobody owned the pair.
//
// Fails-without-it: the row's length counts units for bytes that were never
// written, so it exceeds the text the row actually contributed.
TEST(testNonScalarCellCountsNoUTF16Units)
{
    for (uint32_t bad : { 0x110000u, 0xFFFFFFFFu, 0xD800u, 0xDFFFu }) {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 2, 4, Ubiquitty::Modes::ReflowMode::Fixed);
        feed(t, "AB");

        // Plant the value directly: the point is that the projection holds it
        // whatever produced the cell, including a producer that forgot to check.
        Ubiquitty::Cell planted {};
        planted.codePoint = bad;
        t.grid->fillCellRun(Ubiquitty::Row(0), Ubiquitty::Column(2), planted, 1);

        const TextProjection p = projectVisibleText(t, 0);
        CHECK(!p.lineRanges.empty());
        if (p.lineRanges.empty())
            continue;

        // The invariant, stated as the relation rather than as a number: a row's
        // declared UTF-16 length is exactly the units its own bytes encode.
        // Equality in both directions, because the two ways to break it are
        // symmetric — a count for bytes never written, and bytes written that the
        // count skipped. The surrogate case is the second one: those are below
        // U+10000, so a length-only check encodes them as three bytes of WTF-8.
        uint32_t unitsInText = 0;
        for (size_t i = 0; i < p.text.size();) {
            const unsigned char c = static_cast<unsigned char>(p.text[i]);
            if (c == '\n') {
                ++i;
                continue;
            }
            const size_t sequenceLength = (c < 0x80) ? 1 : (c < 0xE0) ? 2
                : (c < 0xF0)                                          ? 3
                                                                      : 4;
            unitsInText += (sequenceLength == 4) ? 2 : 1;
            i += sequenceLength;
        }
        uint32_t declared = 0;
        for (const auto& range : p.lineRanges)
            declared += range.length;
        // Rows join with a newline, which the ranges do not count.
        CHECK_MSG(declared == unitsInText, "the declared UTF-16 length is exactly what the bytes encode");
    }
}

TEST(testWideGlyphIsNotDoubled)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t,
        "\xF0\x9F\x98\x80"
        "X");

    const std::string text = visibleScreenText(t, 0);
    CHECK_MSG(text == "\xF0\x9F\x98\x80X\n", "the wide glyph's continuation cell emits nothing");
    // 4 bytes of emoji + 1 of 'X' + the row join. A doubled glyph would be 10.
    CHECK_MSG(text.size() == 6, "a doubled wide glyph would show up as extra bytes");
}

// A combining mark belongs to the cell its base occupies and follows it in the
// output — it is not a cell of its own and must not become a space.
TEST(testCombiningMarkFollowsItsBase)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "e\xCC\x81z"); // 'e' + U+0301 COMBINING ACUTE, then 'z'

    const std::string text = visibleScreenText(t, 0);
    CHECK_MSG(text == "e\xCC\x81z\n", "the mark follows its base and consumes no column");
}

TEST(testTrailingBlanksAreTrimmed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 8, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "ab    ");
    CHECK_MSG(visibleScreenText(t, 0) == "ab\n", "trailing blanks do not reach the caller");
}

// viewOffset scrolls the window back through history.
TEST(testViewOffsetReadsScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 8, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "one\r\ntwo\r\nthree\r\n");

    CHECK_MSG(visibleScreenText(t, 0) == "three\n", "offset 0 is the live screen");
    CHECK_MSG(visibleScreenText(t, 1) == "two\nthree", "offset 1 scrolls back one row");
    CHECK_MSG(visibleScreenText(t, 2) == "one\ntwo", "offset 2 scrolls back two rows");
}

// One entry point is defined as the other's text; they must never disagree.
TEST(testBothEntryPointsAgree)
{
    const char* inputs[] = {
        "",
        "plain",
        "Hi\r\nWorld\r\n",
        "trailing    ",
        "e\xCC\x81 combining",
        "\xF0\x9F\x98\x80 wide",
        "\033[31mcoloured\033[0m",
        "tab\there",
    };

    for (const char* input : inputs) {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 4, 20, Ubiquitty::Modes::ReflowMode::Fixed);
        feed(t, input);
        for (int viewOffset = 0; viewOffset < 3; ++viewOffset) {
            CHECK_MSG(visibleScreenText(t, viewOffset) == projectVisibleText(t, viewOffset).text,
                "visibleScreenText is exactly projectVisibleText(...).text");
        }
    }
}

// Every row gets a range even when blank, so a caller can index rows without
// checking whether one produced output. Measured against t.rows: resize clamps
// to a 2x2 minimum, so asking for 1 row yields 2.
TEST(testEveryVisibleRowGetsARange)
{
    for (int requested = 1; requested <= 8; ++requested) {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, requested, 10, Ubiquitty::Modes::ReflowMode::Fixed);
        feed(t, "only one line");
        const TextProjection p = projectVisibleText(t, 0);
        CHECK_MSG(p.lineRanges.size() == static_cast<std::size_t>(t.rows),
            "one range per visible row, blank rows included");
    }
}

// A row's location plus length never runs past the projection: an off-by-one
// here is a crash in a screen reader.
TEST(testRangesStayInsideTheText)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 5, 12, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "alpha\r\n\xF0\x9F\x98\x80 wide\r\n\r\ntrailing   \r\n");

    const TextProjection p = projectVisibleText(t, 0);
    // UTF-16 length of the whole projection, counted independently of the code
    // under test so a shared mistake cannot cancel out.
    uint32_t totalUTF16 = 0;
    for (std::size_t i = 0; i < p.text.size();) {
        const unsigned char lead = static_cast<unsigned char>(p.text[i]);
        std::size_t width = 1;
        if ((lead & 0xE0) == 0xC0)
            width = 2;
        else if ((lead & 0xF0) == 0xE0)
            width = 3;
        else if ((lead & 0xF8) == 0xF0)
            width = 4;
        totalUTF16 += width == 4 ? 2 : 1; // 4-byte UTF-8 is one surrogate pair
        i += width;
    }

    for (const auto& range : p.lineRanges) {
        CHECK_MSG(range.location + range.length <= totalUTF16,
            "a row's range never runs past the end of the text");
    }
}

// caretInProjection maps the active screen's cursor to a UTF-16 index into the
// projection, on the live view only. This pins the value it returns; the guards
// that answer absent are the next case.
TEST(testCaretMapsCursorToProjectionIndex)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "Hi\r\nWorld"); // cursor ends on row 1, column 5

    const TextProjection p = projectVisibleText(t, 0);
    const auto caret = caretInProjection(p, t, 0);
    CHECK_MSG(caret.isValid, "a live-view cursor projects to a valid caret");
    CHECK_MSG(caret.length == 0, "a caret is a zero-length range");
    // Row 1 starts after "Hi\n" (UTF-16 offset 3) and the cursor is at column 5.
    // location adds the cursor's OWN row offset, not row 0's — a bug that read
    // row 0 would answer 5.
    CHECK_MSG(caret.location == 8, "caret is the row's offset plus the column");
}

// The cursor can sit past the trimmed line end (typing at the right margin) or,
// through a producer, below zero; the index floors at 0 and clamps to the line's
// reported length. Planted directly so the arithmetic, not how a byte advances
// the cursor, is what is under test.
TEST(testCaretFloorsAndClampsTheColumn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "Hi"); // row 0 is "Hi", length 2

    const TextProjection p = projectVisibleText(t, 0);

    t.screens.active->cursorX = 7; // past the trimmed end
    const auto clamped = caretInProjection(p, t, 0);
    CHECK_MSG(clamped.isValid && clamped.location == 2,
        "a column past the trimmed length clamps to it");

    t.screens.active->cursorX = -1; // std::max(0, ...) floors it
    const auto floored = caretInProjection(p, t, 0);
    CHECK_MSG(floored.isValid && floored.location == 0,
        "a negative column floors to the row start");
}

// The four ways the caret is off the live view, each answered as absent
// (isValid false) rather than as a zero range: an assistive reader reads the
// latter as a caret at the document start.
TEST(testCaretIsAbsentWhenOffTheLiveView)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "Hi\r\nWorld");
    const TextProjection p = projectVisibleText(t, 0);

    // Scrolled back: the rows on display are not the cursor's.
    CHECK_MSG(!caretInProjection(p, t, 1).isValid, "a nonzero view offset is absent");

    // Cursor row below the projection's first row.
    t.screens.active->cursorY = -1;
    CHECK_MSG(!caretInProjection(p, t, 0).isValid, "a negative cursor row is absent");

    // Cursor row at or past the projection's row count. An empty projection has
    // no rows, so row 0 is already past it.
    t.screens.active->cursorY = 0;
    const TextProjection empty;
    CHECK_MSG(!caretInProjection(empty, t, 0).isValid, "a cursor row past the ranges is absent");

    // No active screen is absent, even at offset 0. The constructor always seeds
    // one (initializeTerminal sets active = &primary), so this guards a
    // not-yet-initialized Terminal and is reached by clearing the pointer
    // directly rather than by any construction sequence.
    t.screens.active = nullptr;
    CHECK_MSG(!caretInProjection(p, t, 0).isValid, "no active screen is absent");
}

} // namespace

TEST(testAGridWithNoAreaProjectsEmptyRatherThanOneEmptyRow)
{
    // `rows` and `columns` are public and any holder of a Terminal may assign
    // them; `resize` clamps to a two-by-two floor, so it is not the producer this
    // guard answers. Without it a zero-column grid still pushes one range per
    // row, and a reader offered a selection into a grid with no cells.
    Ubiquitty::Terminal noRows;
    Ubiquitty::resize(noRows, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    noRows.rows = 0;
    const TextProjection none = projectVisibleText(noRows, 0);
    CHECK_MSG(none.text.empty() && none.lineRanges.empty(), "no rows means no ranges");

    // Rows without columns is the other half of the same guard, and it is a
    // separate arm: the first stops the loop, the second would let it run.
    Ubiquitty::Terminal noColumns;
    Ubiquitty::resize(noColumns, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    noColumns.columns = 0;
    const TextProjection empty = projectVisibleText(noColumns, 0);
    CHECK_MSG(empty.text.empty() && empty.lineRanges.empty(), "no columns means no ranges either");

    // The control: the same call on the geometry both fixtures started from does
    // produce ranges, so the two above are about the area rather than about this
    // entry point being inert.
    Ubiquitty::Terminal sized;
    Ubiquitty::resize(sized, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    CHECK_MSG(projectVisibleText(sized, 0).lineRanges.size() == 3, "a sized grid still projects");
}

TEST(testEveryAddressResolvesToACellRatherThanToNull)
{
    // The projection reads through `visibleCell`'s answer without a null check,
    // because the function has no path that returns one — out of range, past a
    // scrollback line's length, and a Terminal with no grid all answer a blank
    // cell. That is the contract this asserts, and a change to it lands as a
    // failure here rather than as a crash inside the projection.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 6, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(t, "Hi\r\n");

    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, -1, 0) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, -1) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 99, 0) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 99) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 999, 0, 0) != nullptr); // past the scrollback
    CHECK(Ubiquitty::Screen::visibleCell(&t, -1, 0, 0) != nullptr);
    CHECK(Ubiquitty::Screen::visibleCell(nullptr, 0, 0, 0) != nullptr);

    // A Terminal that never got a grid takes the last of the four, which no
    // resized fixture can reach.
    Ubiquitty::Terminal unsized;
    unsized.rows = 1;
    unsized.columns = 1;
    CHECK(Ubiquitty::Screen::visibleCell(&unsized, 0, 0, 0) != nullptr);
}

namespace {

struct ProjectionMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const uint32_t> text, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphs) override
    {
        if (glyphs.size() < text.size())
            return 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
            glyphs[index] = {};
            glyphs[index].glyph = text[index];
            glyphs[index].cluster = static_cast<uint16_t>(index);
            glyphs[index].advanceInPixels = 4;
        }
        return static_cast<int>(text.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8; }
};

void checkRunProjection(Ubiquitty::Terminal& terminal)
{
    const auto projection = projectVisibleText(terminal, 0);
    CHECK(projection.text == "A\xF0\x9F\x98\x80"
                             "e\xCC\x81Z\n\n\n");
    REQUIRE(projection.lineRanges.size() == 4);
    CHECK(projection.lineRanges[0].length == 6);
    CHECK(projection.lineRanges[1].location == 7);
    const auto caret = caretInProjection(projection, terminal, 0);
    CHECK(caret.isValid && caret.location == 5);

    Ubiquitty::SelectionState selection;
    selection.active = true;
    selection.anchorColumn = 0;
    selection.headColumn = 1;
    selection.startSubColumn = 1;
    selection.endSubColumn = 1;
    const auto range = Ubiquitty::Selection::selectionInProjection(projection, selection, 0, 0);
    CHECK(range.isValid && range.location == 1 && range.length == 3);
    selection.startSubColumn = 0;
    selection.endSubColumn = 0;
    selection.anchorColumn = 2;
    selection.headColumn = 0;
    const auto reversed = Ubiquitty::Selection::selectionInProjection(projection, selection, 0, 0);
    CHECK(reversed.isValid && reversed.location == 0 && reversed.length == 6);
}

} // namespace

TEST(proportionalTextAndRangesSurviveLocalAndCheckpointScreens)
{
    using namespace Ubiquitty::Protocol::Sync;
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        ProjectionMeasurer measurer;
        Ubiquitty::Terminal host;
        Ubiquitty::resize(host, 4, 12);
        host.drawsSizedText = true;
        host.drawsProportionalText = true;
        host.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
        if (screen == Ubiquitty::ScreenMode::Alternate)
            feed(host, "\x1b[?1049h");
        feed(host, "\x1b]66;p=1:c=6;A\xF0\x9F\x98\x80"
                   "e\xCC\x81Z\x07\x1b[1;3H");
        checkRunProjection(host);

        ScreenPublisher publisher;
        AttachmentBaseline baseline;
        std::vector<uint8_t> body;
        REQUIRE(publisher.capture(host));
        REQUIRE(publisher.checkpoint(host, 1, body, baseline));
        Ubiquitty::Terminal replica;
        replica.drawsSizedText = true;
        replica.drawsProportionalText = true;
        replica.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
        ScreenReplica state;
        REQUIRE(applyCheckpoint(replica, body, state) == ApplyOutcome::Applied);
        checkRunProjection(replica);
    }
}

TEST(proportionalTextWithoutAMeasurerIsCompleteOnceAndTrimsOnlyTrailingSpaces)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 70);
    terminal.drawsSizedText = true;
    terminal.drawsProportionalText = true;
    const std::string sentence = "Review queue and check the whole proportional sentence";
    Ubiquitty::processBytes(&terminal, "\x1b]66;p=1:c=58;" + sentence + "   \x07!");
    const auto projection = projectVisibleText(terminal, 0);
    CHECK(projection.text == sentence + "   !\n\n");
    CHECK(projection.lineRanges[0].length == sentence.size() + 4);
    CHECK(caretInProjection(projection, terminal, 0).location == sentence.size() + 4);
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::selectAllText(selection, terminal);
    const auto copied = Ubiquitty::Selection::selectedText(selection, terminal);
    CHECK(copied == sentence + "   !\n\n");
}

TEST(clusterOffsetsUseCompleteUTF16TextAndInclusiveSelectionCells)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 12);
    feed(terminal, "e\xCC\x81\xCC\x82\xF0\x9F\x98\x80Z\r\nlast");
    const auto projection = projectVisibleText(terminal, 0);
    CHECK(projection.text == "e\xCC\x81\xCC\x82\xF0\x9F\x98\x80Z\nlast\n");
    CHECK(projection.lineRanges[0].length == 6);
    CHECK(caretInProjection(projection, terminal, 0).location == 11);
    Ubiquitty::SelectionState selection;
    selection.active = true;
    selection.anchorColumn = 1;
    selection.headColumn = 2;
    const auto emoji = Ubiquitty::Selection::selectionInProjection(projection, selection, 0, 0);
    CHECK(emoji.isValid && emoji.location == 3 && emoji.length == 2);
    selection.anchorRow = -1;
    selection.headRow = 9;
    const auto clipped = Ubiquitty::Selection::selectionInProjection(projection, selection, 0, 0);
    CHECK(clipped.isValid && clipped.location == 0 && clipped.length == 12);
}

// A scaled run reads once, on its top band's row, and its letters stand at
// the columns their cells cover, on every band: two letters to each of the
// run's cells, and each of those cells two columns of the row.
TEST(aScaledRunReadsOnceWithItsLettersAtTheColumnsTheyStandIn)
{
    ProjectionMeasurer measurer;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 12);
    terminal.drawsSizedText = true;
    terminal.drawsProportionalText = true;
    terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(terminal, "\x1b]66;p=1:c=3:s=2;abcdef\x07z");
    const auto projection = projectVisibleText(terminal, 0);
    CHECK_MSG(projection.text == "abcdefz\n\n\n", projection.text.c_str());
    const uint32_t expected[] = { 0, 2, 2, 4, 4, 6, 6 };
    for (int column = 0; column <= 6; ++column) {
        const auto boundary = Ubiquitty::Accessibility::positionInProjection(projection, 0, column);
        CHECK_MSG(boundary.isValid && boundary.location == expected[column], std::to_string(column).c_str());
        // The lower band shows the same letters: a position on it is the
        // boundary above it, so a selection made there names those letters.
        if (column < 6) {
            const auto lower = Ubiquitty::Accessibility::positionInProjection(projection, 1, column);
            CHECK_MSG(lower.isValid && lower.location == expected[column], ("lower band " + std::to_string(column)).c_str());
        }
    }
    Ubiquitty::SelectionState selection;
    selection.active = true;
    selection.anchorRow = 1;
    selection.headRow = 1;
    selection.anchorColumn = 2;
    selection.headColumn = 3;
    const auto range = Ubiquitty::Selection::selectionInProjection(projection, selection, 0, 0);
    CHECK_MSG(range.isValid && range.location == 2 && range.length == 2, "a selection on the lower band names the letters above it");
}

TEST(runTrimmingAndLowerBandsKeepEveryBoundaryInsideItsRow)
{
    ProjectionMeasurer measurer;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 12);
    terminal.drawsSizedText = true;
    terminal.drawsProportionalText = true;
    terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(terminal, "\x1b]66;p=1:c=6;AB  \x07\x1b[2;1H\x1b]66;s=2;Q\x07");
    const auto projection = projectVisibleText(terminal, 0);
    CHECK(projection.text == "AB\nQ\n\n");
    CHECK(projection.lineRanges[0].length == 2);
    CHECK(projection.lineRanges[1].length == 1);
    CHECK(projection.lineRanges[2].length == 0);
    for (int row = 0; row < terminal.rows; ++row) {
        const auto& line = projection.lineRanges[static_cast<size_t>(row)];
        uint32_t previous = line.location;
        for (int column = 0; column <= terminal.columns; ++column) {
            const auto boundary = Ubiquitty::Accessibility::positionInProjection(projection, row, column);
            const auto beyondCell = Ubiquitty::Accessibility::positionInProjection(projection, row, column, 1000);
            CHECK(boundary.isValid && beyondCell.isValid);
            CHECK(boundary.location >= previous);
            CHECK(boundary.location <= beyondCell.location);
            CHECK(beyondCell.location <= line.location + line.length);
            previous = boundary.location;
        }
    }
    CHECK(!Ubiquitty::Accessibility::positionInProjection(projection, -1, 0).isValid);
    CHECK(!Ubiquitty::Accessibility::positionInProjection(projection, terminal.rows, 0).isValid);
}

TEST(scrolledRunProjectionRetainsTextAndSelectionCoordinates)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 12);
    terminal.drawsSizedText = true;
    terminal.drawsProportionalText = true;
    feed(terminal, "\x1b]66;p=1:c=6;History sentence\x07\r\nsecond\r\nthird");
    const auto projection = projectVisibleText(terminal, 1);
    CHECK(projection.text == "History sentence\nsecond");
    CHECK(!caretInProjection(projection, terminal, 1).isValid);
    Ubiquitty::SelectionState selection;
    selection.active = true;
    selection.anchorRow = 0;
    selection.headRow = 0;
    selection.headColumn = 5;
    const auto range = Ubiquitty::Selection::selectionInProjection(projection, selection, terminal.scrollbackCount, 1);
    CHECK(range.isValid && range.location == 0 && range.length == 16);
}

TEST(clippedRichTextReadsOnlyVisibleRangesAlongsideTheMenu)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 10);
    feed(terminal, "000MENU000\r\x1b]7404;w=10:h=1:C=1:P=1:r=0,0,3,1,7,0,3,1;{\\rtf1 0123456789}\x1b\\");
    const auto projection = projectVisibleText(terminal, 0);
    CHECK_MSG(projection.text == "012MENU789\n", projection.text.c_str());
    CHECK_EQ(projection.richText.size(), std::size_t { 1 });
    CHECK_EQ(projection.richText.front().visibleRanges.size(), std::size_t { 2 });
    CHECK_EQ(projection.richText.front().visibleRanges[1].first, std::uint32_t { 7 });
    CHECK_EQ(projection.richText.front().visibleRanges[1].column, 7);
}

UBIQUITTY_TEST_MAIN()
