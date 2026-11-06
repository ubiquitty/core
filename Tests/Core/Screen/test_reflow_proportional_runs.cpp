// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_reflow_proportional_runs.cpp — OSC 66 proportional runs and OSC 7404
// rich text blocks through a width-changing resize (Docs/proportional-text.md,
// "What the model stores"; Docs/rich-text.md, "The box model").
//
// A proportional run is an anchor holding the run's text and a chain of
// continuation cells; a chain never straddles a wrap point, and its text rides
// the anchor. A run that does not fit what is left of its row breaks at its
// own spaces into pieces, each a chain of the cells its words measure, and the
// pieces join again when a wider row holds the run. Narrowing and widening
// back to the original geometry gives the original grid, the history included. A
// block rides its rows while its cells stay together and is removed when the
// rewrap separates them. Over Sync the client applies what the host
// published after the resize and agrees with it cell for cell.

#include "LoopbackPaneLink.h"
#include "TerminalFixture.h"

#include "Core/Cell.h"
#include "Core/CellPen.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/RichText/Blocks.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;
using UbiquittyTest::cell;

namespace {

/// A terminal that draws sized text, so OSC 66 places proportional runs.
std::unique_ptr<Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Terminal>();
    hardReset(*terminal);
    resize(*terminal, rows, columns);
    terminal->drawsSizedText = true;
    terminal->drawsProportionalText = true;
    return terminal;
}

std::string run(std::string_view text, int cells)
{
    return "\x1b]66;p=1:c=" + std::to_string(cells) + ";" + std::string(text) + "\x1b\\";
}

std::string utf8(std::span<const std::uint32_t> codePoints)
{
    std::string text;
    for (const std::uint32_t codePoint : codePoints) {
        if (codePoint < 0x80) {
            text.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    return text;
}

/// One cell, as a signature reads it: a run's anchor by its text and the
/// cells it claims, a continuation as `~`, a wide pair's halves by their
/// role, anything else by its code point.
void describe(const Terminal& t, const Cell& here, std::string& out)
{
    const Pen pen = cellPen(here);
    if (isProportionalRunAnchor(&t, here)) {
        const TextSizeSpec* spec = t.textSizeTable.lookup(pen.extendedAttributes.textSizeSpecID());
        out += "[" + utf8(runTextCodePoints(&t, here)) + "/" + std::to_string(spec ? spec->runWidthCells : 0) + "]";
        return;
    }
    if (pen.attributes.contains(VT::Attribute::WideContinuation)) {
        out.push_back('~');
        return;
    }
    const std::uint32_t codePoint = here.codePoint ? here.codePoint : ' ';
    out += utf8(std::span(&codePoint, 1));
    if (pen.attributes.contains(VT::Attribute::Wide))
        out.push_back('<');
}

/// Everything a rewrap may move: every history line and every screen row,
/// its cells, its wrap flag and its line attribute, with each row's trailing
/// blanks dropped.
std::string signature(const Terminal& t)
{
    std::string out;
    auto endRow = [&out](bool isWrapped, int lineAttribute) {
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        out += isWrapped ? "\\\n" : "\n";
        if (lineAttribute)
            out += "#" + std::to_string(lineAttribute) + " ";
    };
    const int history = Screen::scrollbackCount(&t);
    for (int index = 0; index < history; ++index) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&t, index);
        if (!line)
            continue;
        for (int column = 0; column < line->length; ++column)
            describe(t, line->cells[column], out);
        endRow(line->wrapped, line->lineAttribute);
    }
    out += "--\n";
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            if (const Cell* here = cell(t, row, column))
                describe(t, *here, out);
        }
        endRow(Screen::visibleRowWrapped(&t, 0, row), t.screens.active->lineAttribute[row]);
    }
    return out;
}

/// Two signatures agree; the message is the first that differs, so a
/// failure names what the rewrap moved.
void checkSame(const std::string& actual, const std::string& expected)
{
    const std::string message = actual == expected ? std::string {} : "got\n" + actual + "\nwant\n" + expected;
    CHECK_MSG(actual == expected, message.c_str());
}

/// The first continuation cell on screen whose chain reaches no anchor to
/// its left, as `row,column`; empty when every chain is whole.
std::string firstTornCell(const Terminal& t)
{
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Cell* here = cell(t, row, column);
            if (!here || !cellPen(*here).attributes.contains(VT::Attribute::WideContinuation))
                continue;
            int lead = column - 1;
            while (lead >= 0 && cell(t, row, lead) && cellPen(*cell(t, row, lead)).attributes.contains(VT::Attribute::WideContinuation))
                --lead;
            if (lead < 0 || !cell(t, row, lead) || !cellPen(*cell(t, row, lead)).attributes.contains(VT::Attribute::Wide))
                return std::to_string(row) + "," + std::to_string(column);
        }
    }
    return {};
}

/// The runs on screen, in reading order, as `text/cells`.
std::vector<std::string> runs(const Terminal& t)
{
    std::vector<std::string> found;
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Cell* here = cell(t, row, column);
            if (!here || !isProportionalRunAnchor(&t, *here))
                continue;
            int cells = 1;
            while (column + cells < t.columns) {
                const Cell* next = cell(t, row, column + cells);
                if (!next || !isProportionalRunContinuation(&t, *next))
                    break;
                ++cells;
            }
            found.push_back(utf8(runTextCodePoints(&t, *here)) + "/" + std::to_string(cells));
        }
    }
    return found;
}

std::string place(std::string_view keys, std::string_view body)
{
    return "\x1b]7404;" + std::string(keys) + ";" + std::string(body) + "\x1b\\";
}

std::size_t blockCount(const Terminal& t) { return t.richTextBlocks.blocks(ScreenMode::Primary).size(); }

/// A face that sets every letter, space included, three quarters of a cell
/// wide, so a run measures fewer cells than the grid face would take.
struct ThreeQuarterCellMeasurer final : TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, ProportionalRunStyle, std::span<ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = ProportionalShapedGlyph {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = 6.0f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

/// What a copy of everything carries.
std::string copyAll(Terminal& t)
{
    Selection::Session session;
    Selection::init(session);
    Selection::selectAllText(session, t);
    return Selection::selectedText(session, t);
}

/// Publishes a host's screen to a client the way an attachment brings itself
/// up after a resize: a geometry change is a checkpoint.
struct Mirror {
    Protocol::Sync::ScreenPublisher publisher;
    Protocol::Sync::AttachmentBaseline baseline;
    Protocol::Sync::ScreenReplica replica;
    std::unique_ptr<Terminal> client;
    std::uint64_t generation = 0;

    explicit Mirror(const Terminal& host)
        : client(makeTerminal(host.rows, host.columns))
    {
        follow(host);
    }

    /// Brings the client up to the host; an update when one can say what
    /// moved, a checkpoint when only one can, and the history beside it.
    void follow(const Terminal& host)
    {
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(host));
        if (generation && publisher.rowUpdate(host, baseline, body) == Protocol::Sync::UpdateOutcome::Encoded) {
            CHECK(Protocol::Sync::applyRowUpdate(*client, body, replica) == Protocol::Sync::ApplyOutcome::Applied);
        } else {
            body.clear();
            REQUIRE(publisher.checkpoint(host, ++generation, body, baseline));
            CHECK(Protocol::Sync::applyCheckpoint(*client, body, replica) == Protocol::Sync::ApplyOutcome::Applied);
        }
        CHECK(followHistory(host));
    }

    /// The history the screen plane names and does not carry: its lines on
    /// the history plane, which the client wraps into its scrollback at its
    /// own width (Docs/sync-protocol.md).
    bool followHistory(const Terminal& host)
    {
        Protocol::Sync::HistoryPublisher history;
        (void)history.capture(host);
        const Screen::HistoryAddress address = history.address();
        Protocol::Sync::HistoryCache cache({}, {}, 1'000'000);
        if (address.head > address.oldest) {
            std::vector<std::uint8_t> body;
            if (history.writeBody(host, { address.oldest, address.head }, Protocol::Sync::HistoryOrder::Ascending, std::size_t { 64 } << 20, body) == 0)
                return false;
            const Protocol::Sync::HistoryBodyLimits limits { .maxLines = Protocol::Sync::historyBodyLimits.maxLines, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() };
            std::vector<Protocol::Sync::DecodedLine> lines;
            if (!Protocol::Sync::decodeHistoryBody(body, address.oldest, Protocol::Sync::HistoryOrder::Ascending, limits, lines)
                || cache.insert(cache.key().epoch, lines) != Protocol::Sync::CacheInsert::Stored)
                return false;
        }
        cache.setHead(cache.key().epoch, address.head);
        if (history.openLine()) {
            std::vector<std::uint8_t> body;
            std::vector<Protocol::Sync::DecodedLine> lines;
            if (!history.writeOpenLine(host, std::size_t { 64 } << 20, body)
                || !Protocol::Sync::decodeHistoryBody(body, address.head, Protocol::Sync::HistoryOrder::Ascending, { .maxLines = 1, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() }, lines)
                || cache.setOpenLine(cache.key().epoch, std::move(lines.front())) != Protocol::Sync::CacheInsert::Stored)
                return false;
        }
        cache.project(*client, client->columns);
        return true;
    }
};

/// The client agrees with the host cell for cell, and on every run's whole
/// text, which the oracle reads only as far as a cluster holds.
void checkAgrees(const Terminal& host, const Terminal& client)
{
    const std::string difference = UbiquittyTest::firstDisagreement(host, client);
    CHECK_MSG(difference.empty(), difference.c_str());
    checkSame(signature(client), signature(host));
}

} // namespace

TEST(aRunThatNoLongerFitsItsRowMovesWholeWithItsText)
{
    auto t = makeTerminal(6, 40);
    UbiquittyTest::feed(*t, "ab " + run("Proportional", 10) + " cd");
    const std::string original = signature(*t);
    CHECK(runs(*t) == std::vector<std::string> { "Proportional/10" });
    // Twelve columns hold `ab ` and not the ten cells after it: the run moves
    // to the next row whole, anchor first.
    resize(*t, t->rows, 12);
    CHECK(runs(*t) == std::vector<std::string> { "Proportional/10" });
    CHECK(firstTornCell(*t).empty());
    const Cell* anchor = cell(*t, 1, 0);
    REQUIRE(anchor);
    CHECK(isProportionalRunAnchor(t.get(), *anchor));
    CHECK(Screen::visibleRowWrapped(t.get(), 0, 0));
    resize(*t, t->rows, 40);
    checkSame(signature(*t), original);
}

TEST(aRunStraddlingTheWrapPointMovesWholeAndRejoinsWhenWidened)
{
    auto t = makeTerminal(6, 40);
    UbiquittyTest::feed(*t, "0123456789" + run("straddle", 6) + "tail");
    const std::string original = signature(*t);
    // Thirteen columns cut through columns 10 to 15, where the run lies: the
    // row ends short at ten and the run opens the next.
    resize(*t, t->rows, 13);
    const Cell* anchor = cell(*t, 1, 0);
    REQUIRE(anchor);
    CHECK(isProportionalRunAnchor(t.get(), *anchor));
    CHECK(runs(*t) == std::vector<std::string> { "straddle/6" });
    resize(*t, t->rows, 40);
    checkSame(signature(*t), original);
}

TEST(aWrappedLineHoldingARunUnwrapsWhenWidenedAndWrapsBackWhenNarrowed)
{
    auto t = makeTerminal(8, 20);
    UbiquittyTest::feed(*t, "one two three four five " + run("in the middle", 9) + " six seven eight");
    const std::string original = signature(*t);
    CHECK(Screen::visibleRowWrapped(t.get(), 0, 0));
    resize(*t, t->rows, 80);
    CHECK(!Screen::visibleRowWrapped(t.get(), 0, 0));
    CHECK(runs(*t) == std::vector<std::string> { "in the middle/9" });
    const Cell* anchor = cell(*t, 0, 24);
    REQUIRE(anchor);
    CHECK(isProportionalRunAnchor(t.get(), *anchor));
    resize(*t, t->rows, 20);
    checkSame(signature(*t), original);
}

TEST(aRunOnADoubleWidthRowKeepsItsRowAttributeAndItsText)
{
    auto t = makeTerminal(6, 40);
    UbiquittyTest::feed(*t, "\x1b#6" + run("doubled", 8) + "\r\nnext");
    const std::string original = signature(*t);
    // Thirty columns are fifteen doubled cells: the run's eight still fit.
    resize(*t, t->rows, 30);
    CHECK(runs(*t) == std::vector<std::string> { "doubled/8" });
    CHECK_EQ(static_cast<int>(t->screens.active->lineAttribute[0]), static_cast<int>(lineAttributeDoubleWidth));
    resize(*t, t->rows, 40);
    checkSame(signature(*t), original);
}

TEST(aRunBesideWideCellsTearsNoPairAndKeepsItsPlace)
{
    auto t = makeTerminal(6, 30);
    UbiquittyTest::feed(*t, "東京" + run("between", 7) + "😀漢字");
    const std::string original = signature(*t);
    for (const int columns : { 13, 9, 7 }) {
        resize(*t, t->rows, columns);
        CHECK_MSG(firstTornCell(*t).empty(), firstTornCell(*t).c_str());
        CHECK(runs(*t).size() <= 1);
    }
    // Seven columns and wider hold the run whole, on a row of its own at
    // seven.
    resize(*t, t->rows, 13);
    CHECK(runs(*t) == std::vector<std::string> { "between/7" });
    resize(*t, t->rows, 30);
    checkSame(signature(*t), original);
}

TEST(aRunInTheScrollbackReflowsWithTheHistory)
{
    auto t = makeTerminal(4, 40);
    UbiquittyTest::feed(*t, "history " + run("kept in the scrollback", 14) + " end\r\n");
    for (int line = 0; line < 8; ++line)
        UbiquittyTest::feed(*t, "line " + std::to_string(line) + "\r\n");
    REQUIRE(Screen::scrollbackCount(t.get()) >= 1);
    const std::string original = signature(*t);
    CHECK(original.find("[kept in the scrollback/14]") != std::string::npos);
    // Sixteen columns leave eight cells beside `history `: the run breaks
    // after `kept in` and the rest opens the next row.
    resize(*t, t->rows, 16);
    const std::string narrowed = signature(*t);
    CHECK(narrowed.find("[kept in /7]") != std::string::npos);
    CHECK(narrowed.find("[the scrollback/14]") != std::string::npos);
    resize(*t, t->rows, 40);
    checkSame(signature(*t), original);
}

TEST(aRunWiderThanTheNarrowedRowGetsItsWidthBackWhenWidened)
{
    auto t = makeTerminal(6, 60);
    UbiquittyTest::feed(*t, run("a_heading_wider_than_the_narrow_window", 30));
    const std::string original = signature(*t);
    // Twenty columns cannot hold thirty, and the run has no space to break
    // at: it is clamped to the row and keeps every letter of its text.
    resize(*t, t->rows, 20);
    CHECK(runs(*t) == std::vector<std::string> { "a_heading_wider_than_the_narrow_window/20" });
    resize(*t, t->rows, 60);
    checkSame(signature(*t), original);
}

TEST(aRunWiderThanTheRowBreaksAtItsSpacesAndJoinsWhenWidened)
{
    auto t = makeTerminal(6, 60);
    UbiquittyTest::feed(*t, run("a heading wider than the narrow window", 30));
    const std::string original = signature(*t);
    // Without a shaper a piece measures in the grid face: twenty columns
    // take `a heading wider than` and the rest goes on the next row. The
    // space a piece breaks after stays in its text, outside its cells.
    resize(*t, t->rows, 20);
    CHECK(runs(*t) == (std::vector<std::string> { "a heading wider than /20", "the narrow window/17" }));
    CHECK(firstTornCell(*t).empty());
    CHECK(Screen::visibleRowWrapped(t.get(), 0, 0));
    resize(*t, t->rows, 60);
    checkSame(signature(*t), original);
}

TEST(aRunReflowsAtItsOwnWordsAndJoinsWhenWidenedBack)
{
    ThreeQuarterCellMeasurer measurer;
    auto t = makeTerminal(6, 40);
    t->textMeasureClient = CheckedPtr<TextMeasureClient>(&measurer);
    const std::string sentence = "Grid text, then a proportional run in a sentence that wraps when the window narrows.";
    // Eighteen letters at three quarters of a cell measure fourteen cells.
    UbiquittyTest::feed(*t, "Grid text, then " + run("a proportional run", 14) + " in a sentence that wraps when the window narrows.");
    const std::string original = signature(*t);
    CHECK(runs(*t) == std::vector<std::string> { "a proportional run/14" });
    CHECK(copyAll(*t).starts_with(sentence));
    struct Width {
        int columns;
        std::vector<std::string> runs;
    };
    // Sixteen grid cells precede the run. Thirty columns hold all fourteen
    // after them; twenty-eight hold `a proportional` and not `run`;
    // twenty-four hold `a` and not `proportional`, which opens the next row
    // with the rest; twenty do the same with four cells left.
    const Width widths[] = {
        { 30, { "a proportional run/14" } },
        { 28, { "a proportional /11", "run/3" } },
        { 24, { "a /1", "proportional run/12" } },
        { 20, { "a /1", "proportional run/12" } },
        { 28, { "a proportional /11", "run/3" } },
    };
    Mirror mirror(*t);
    for (const Width& width : widths) {
        resize(*t, t->rows, width.columns);
        const std::vector<std::string> found = runs(*t);
        std::string message = std::to_string(width.columns) + " columns:";
        for (const std::string& piece : found)
            message += " [" + piece + "]";
        CHECK_MSG(found == width.runs, message.c_str());
        CHECK_MSG(firstTornCell(*t).empty(), message.c_str());
        // The pieces carry the text between them: the copy is the sentence.
        CHECK_MSG(copyAll(*t).starts_with(sentence), (message + " copied: " + copyAll(*t)).c_str());
        mirror.follow(*t);
        checkAgrees(*t, *mirror.client);
    }
    // Widening back joins the pieces into the run the program wrote.
    resize(*t, t->rows, 40);
    checkSame(signature(*t), original);
    mirror.follow(*t);
    checkAgrees(*t, *mirror.client);
    t->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

TEST(aDragResizeBreaksAndJoinsARunAsASettledOneDoes)
{
    auto t = makeTerminal(6, 40);
    UbiquittyTest::feed(*t, "Grid text, then " + run("a proportional run here", 23) + " in a sentence.");
    const std::string original = signature(*t);
    // A drag rewraps the screen at every step and keeps the history for the
    // settle; the pieces a step leaves still join at the next one.
    for (const int columns : { 32, 26, 20, 33, 40 }) {
        resize(*t, t->rows, columns, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        CHECK_MSG(firstTornCell(*t).empty(), std::to_string(columns).c_str());
    }
    settleResize(t.get());
    checkSame(signature(*t), original);
}

TEST(theCursorAfterARunKeepsItsPlaceInTheTextAcrossARewrap)
{
    auto t = makeTerminal(6, 60);
    UbiquittyTest::feed(*t, run("a heading wider than the narrow window", 30) + "!");
    // The cursor follows the `!`, whatever the run's pieces take.
    resize(*t, t->rows, 20);
    const Cell* mark = cell(*t, t->screens.active->cursorY, t->screens.active->cursorX - 1);
    REQUIRE(mark);
    CHECK_EQ(mark->codePoint, std::uint32_t { '!' });
    resize(*t, t->rows, 60);
    CHECK_EQ(t->screens.active->cursorY, 0);
    CHECK_EQ(t->screens.active->cursorX, 31);
}

// A scaled run is a block of bands on rows of their own, so a rewrap moves
// each band whole with its row, as it moves a sized glyph's, and the block
// rides a resize that keeps its rows together.
TEST(aScaledRunWhoseBandsStayTogetherRidesTheResize)
{
    auto t = makeTerminal(10, 40);
    UbiquittyTest::feed(*t, "\x1b[3;3H\x1b]66;p=1:c=3:s=2;Hello\x1b\\");
    const std::string original = signature(*t);
    CHECK(runs(*t) == (std::vector<std::string> { "Hello/6", "Hello/6" }));
    for (const int columns : { 30, 8, 50, 40 }) {
        resize(*t, t->rows, columns);
        CHECK(runs(*t) == (std::vector<std::string> { "Hello/6", "Hello/6" }));
        for (int band = 0; band < 2; ++band) {
            const Cell* anchor = cell(*t, 2 + band, 2);
            REQUIRE(anchor);
            CHECK(isProportionalRunAnchor(t.get(), *anchor));
            CHECK_EQ(static_cast<int>(penTextBand(&cellPen(*anchor))), band);
        }
        CHECK(firstTornCell(*t).empty());
    }
    checkSame(signature(*t), original);
}

// A rewrap that sets the bands of a scaled run apart leaves a block no row
// can draw, and it is erased as a sized block is; the text around it stays.
// A band is never broken at its spaces or clamped to a narrower row, which
// would leave it a different chain from the bands it stacks with.
TEST(aScaledRunWhoseBandsTheRewrapSeparatesIsErased)
{
    auto t = makeTerminal(10, 40);
    UbiquittyTest::feed(*t, "\x1b[3;1Hab\x1b]66;p=1:c=3:s=2;Hello there\x1b\\");
    REQUIRE(runs(*t) == (std::vector<std::string> { "Hello there/6", "Hello there/6" }));
    // Six columns hold the band and not the two letters before it: the top
    // band's row wraps before it, and the blank cells before the lower band
    // wrap its row the same way, with the letters' row between them.
    resize(*t, t->rows, 6);
    CHECK(runs(*t).empty());
    CHECK(firstTornCell(*t).empty());
    resize(*t, t->rows, 40);
    CHECK(runs(*t).empty());
    const Cell* first = cell(*t, 2, 0);
    CHECK(first && first->codePoint == 'a');
}

TEST(aBlockWhoseCellsStayTogetherRidesTheResize)
{
    auto t = makeTerminal(10, 40);
    UbiquittyTest::feed(*t, "\x1b[3;1H" + place("w=10:h=2:C=1", "{\\rtf1 kept}"));
    REQUIRE(blockCount(*t) == 1);
    UbiquittyTest::feed(*t, "\x1b[6;1H" + place("w=10:h=2:p=4:y=c:C=1", "{\\rtf1 padded}"));
    REQUIRE(blockCount(*t) == 2);
    // The blocks' rows hold nothing past their ten cells, so no rewrap moves
    // them, narrower or wider, padded or not.
    resize(*t, t->rows, 30);
    CHECK_EQ(blockCount(*t), std::size_t { 2 });
    resize(*t, t->rows, 50);
    CHECK_EQ(blockCount(*t), std::size_t { 2 });
    CHECK(t->richTextBlocks.blocks(ScreenMode::Primary)[1].padding == (RichText::Insets { .top = 4, .right = 4, .bottom = 4, .left = 4 }));
    CHECK(t->richTextBlocks.blocks(ScreenMode::Primary)[1].verticalPlacement == RichText::VerticalPlacement::Center);
    const RichText::Block& block = t->richTextBlocks.blocks(ScreenMode::Primary)[0];
    CHECK_EQ(block.row, 2);
    CHECK_EQ(block.column, 0);
}

TEST(aBlockWhoseCellsTheRewrapSeparatesIsRemoved)
{
    auto t = makeTerminal(10, 40);
    // Twenty letters, then the block from column 20: a 25-column grid wraps
    // the row inside the block's rectangle.
    UbiquittyTest::feed(*t, "\x1b[3;1Habcdefghijklmnopqrst" + place("w=10:h=2:C=1", "{\\rtf1 apart}"));
    REQUIRE(blockCount(*t) == 1);
    resize(*t, t->rows, 25);
    CHECK_EQ(blockCount(*t), std::size_t { 0 });
    // The letters survive; the cells the block covered are plain spaces.
    resize(*t, t->rows, 40);
    CHECK_EQ(blockCount(*t), std::size_t { 0 });
    CHECK_EQ(cell(*t, 2, 0)->codePoint, std::uint32_t { 'a' });
}

TEST(theClientAgreesWithTheHostAfterEachResize)
{
    auto host = makeTerminal(8, 40);
    UbiquittyTest::feed(*host, "one two " + run("a run longer than a cluster holds", 18) + " three four five six seven\r\n");
    UbiquittyTest::feed(*host, "東京 " + run("beside wide cells", 9) + " 😀\r\n");
    UbiquittyTest::feed(*host, "\x1b#6" + run("doubled", 8) + "\r\n");
    UbiquittyTest::feed(*host, place("w=10:h=2:C=1", "{\\rtf1\\b block}") + "\r\n");
    UbiquittyTest::feed(*host, place("w=12:h=2:p=2,6,2,6:y=b:C=1", "{\\rtf1 padded}") + "\r\n");
    for (int line = 0; line < 6; ++line)
        UbiquittyTest::feed(*host, "scrolled " + std::to_string(line) + " " + run("away", 4) + "\r\n");
    Mirror mirror(*host);
    checkAgrees(*host, *mirror.client);
    for (const int columns : { 24, 13, 60, 40 }) {
        resize(*host, host->rows, columns);
        mirror.follow(*host);
        checkAgrees(*host, *mirror.client);
        CHECK_EQ(blockCount(*mirror.client), blockCount(*host));
    }
}

UBIQUITTY_TEST_MAIN()
