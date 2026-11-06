// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_rich_text_blocks.cpp — OSC 7404 blocks in the grid (Docs/rich-text.md,
// "The sequence" and "The box model"): what a placement covers and where the
// cursor goes, what removes a block, what moves one, the caps refusing with
// nothing mutated, and the text a copy yields.

#include "TerminalFixture.h"

#include "Core/CellPen.h"
#include "Core/OSC/RichTextBlocks.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <string>
#include <vector>

using namespace Ubiquitty::RichText;
using UbiquittyTest::cell;
using UbiquittyTest::feed;

namespace {

void reset(Ubiquitty::Terminal& t, int rows = 24, int columns = 80)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);
}

std::string place(std::string_view keys, std::string_view body)
{
    return "\x1b]7404;" + std::string(keys) + ";" + std::string(body) + "\x1b\\";
}

std::span<const Block> primaryBlocks(const Ubiquitty::Terminal& t) { return t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary); }

bool isCoveredAt(const Ubiquitty::Terminal& t, int row, int column)
{
    const Ubiquitty::Cell* at = cell(t, row, column);
    return at && Ubiquitty::penRichTextCovered(&Ubiquitty::cellPen(*at));
}

std::string copyRows(Ubiquitty::Terminal& t, int row0, int column0, int row1, int column1, int viewOffset = 0)
{
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, t, row0, column0, 1, viewOffset, {});
    Ubiquitty::Selection::dragGesture(selection, t, row1, column1, viewOffset);
    return Ubiquitty::Selection::selectedText(selection, t);
}

class DenyingPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        return capability == Ubiquitty::Capability::RichTextRender ? Ubiquitty::CapabilityDecision::Deny : Ubiquitty::CapabilityDecision::Allow;
    }
};

} // namespace

TEST(aPlacementCoversItsRectangleAndMovesTheCursorBelowIt)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[3;5H");
    feed(t, place("w=10:h=3:i=7", "{\\rtf1 Hello}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    if (primaryBlocks(t).empty())
        return;
    const Block& block = primaryBlocks(t)[0];
    CHECK_EQ(block.row, 2);
    CHECK_EQ(int { block.column }, 4);
    CHECK_EQ(int { block.width }, 10);
    CHECK_EQ(int { block.height }, 3);
    CHECK_EQ(int { block.id }, 7);
    CHECK_EQ(block.document.text.size(), std::size_t { 5 });
    for (int row = 2; row < 5; ++row) {
        for (int column = 4; column < 14; ++column) {
            CHECK(isCoveredAt(t, row, column));
            CHECK_EQ(cell(t, row, column)->codePoint, std::uint32_t { ' ' });
        }
    }
    CHECK(!isCoveredAt(t, 2, 3));
    CHECK(!isCoveredAt(t, 2, 14));
    CHECK(!isCoveredAt(t, 5, 4));
    CHECK_EQ(t.screens.active->cursorY, 5);
    CHECK_EQ(t.screens.active->cursorX, 4);
}

TEST(aPlacementThatStaysLeavesTheCursor)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[2;3H");
    feed(t, place("w=4:h=2:C=1", "{\\rtf1 x}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    CHECK_EQ(t.screens.active->cursorY, 1);
    CHECK_EQ(t.screens.active->cursorX, 2);
}

TEST(aMalformedCodeIsRefusedWhole)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "abc\r");
    const std::vector<std::string> refused = {
        place("w=4", "{\\rtf1 x}"), // no height
        place("h=4", "{\\rtf1 x}"), // no width
        place("w=4:h=2:w=4", "{\\rtf1 x}"), // repeated
        place("w=4:h=2:z=1", "{\\rtf1 x}"), // unknown
        place("w=0:h=2", "{\\rtf1 x}"), // out of range
        place("w=4:h=2:o=x", "{\\rtf1 x}"), // bad value
        place("w=4:h=2:i", "{\\rtf1 x}"), // no `=`
        place("w=4:h=2", "plain text"), // not RTF
        place("w=81:h=2", "{\\rtf1 x}"), // wider than the screen
        place("w=4:h=25", "{\\rtf1 x}"), // taller than the screen
        place("a=d:w=4", ""), // a key the action does not take
    };
    for (const std::string& code : refused)
        feed(t, code);
    CHECK(primaryBlocks(t).empty());
    CHECK_EQ(cell(t, 0, 0)->codePoint, std::uint32_t { 'a' });
    CHECK(!isCoveredAt(t, 0, 0));
    CHECK_EQ(t.screens.active->cursorY, 0);
    CHECK_EQ(t.screens.active->cursorX, 0);
}

TEST(aBlockThatDoesNotFitTheRowFromTheCursorIsRefused)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[1;75H");
    feed(t, place("w=7:h=1", "{\\rtf1 x}"));
    CHECK(primaryBlocks(t).empty());
    feed(t, place("w=6:h=1", "{\\rtf1 x}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
}

TEST(aBlockPastTheBottomScrollsTheScreenFirst)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "top\x1b[24;1H");
    feed(t, place("w=5:h=3", "{\\rtf1 x}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    if (primaryBlocks(t).empty())
        return;
    // Two rows for the excess and one for the cursor's line below.
    CHECK_EQ(primaryBlocks(t)[0].row, 20);
    CHECK_EQ(t.scrollbackCount, 3);
    CHECK_EQ(t.screens.active->cursorY, 23);
}

TEST(printingIntoACoveredCellRemovesTheBlock)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 x}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    feed(t, "\x1b[2;3HZ");
    CHECK(primaryBlocks(t).empty());
    // The cells the block leaves are plain spaces in its pen.
    CHECK(!isCoveredAt(t, 0, 0));
    CHECK(!isCoveredAt(t, 1, 5));
    CHECK_EQ(cell(t, 0, 0)->codePoint, std::uint32_t { ' ' });
    CHECK_EQ(cell(t, 1, 2)->codePoint, std::uint32_t { 'Z' });
}

TEST(erasingACoveredCellRemovesTheBlock)
{
    const std::vector<std::string> erasures = {
        "\x1b[2;3H\x1b[K", // EL to the end
        "\x1b[2;3H\x1b[1X", // ECH
        "\x1b[2J", // ED 2
        "\x1b[2;3H\x1b[P", // DCH
        "\x1b[2;3H\x1b[@", // ICH
        "\x1b[3;1H\x1b[L", // IL inside it
        "\x1b[2;1H\x1b[M", // DL through it
        "\x1b[1;2r\x1b[2;1H\n\x1b[r", // a region scroll across its top edge
    };
    for (const std::string& erase : erasures) {
        Ubiquitty::Terminal t;
        reset(t);
        feed(t, "\x1b[2;1H");
        feed(t, place("w=6:h=2:C=1", "{\\rtf1 x}"));
        CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
        feed(t, erase);
        CHECK_MSG(primaryBlocks(t).empty(), ("the block survived " + erase.substr(1)).c_str());
    }
}

TEST(anEditOutsideTheBlockKeepsIt)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[3;1H");
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 x}"));
    feed(t, "\x1b[1;1Habc\x1b[K\x1b[3;7Hright\x1b[10;1H\x1b[J");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
}

TEST(aLineInsertedAboveMovesTheBlockDown)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[5;1H");
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 x}"));
    feed(t, "\x1b[2;1H\x1b[2L");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    if (!primaryBlocks(t).empty())
        CHECK_EQ(primaryBlocks(t)[0].row, 6);
}

TEST(aFullScreenScrollCarriesTheBlockIntoTheScrollback)
{
    Ubiquitty::Terminal t;
    reset(t, 10, 40);
    feed(t, place("w=8:h=2", "{\\rtf1 history}"));
    feed(t, "\x1b[10;1H");
    for (int line = 0; line < 5; ++line)
        feed(t, "\n");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    if (primaryBlocks(t).empty())
        return;
    CHECK_EQ(primaryBlocks(t)[0].row, -5);
    // The copy reads it from the scrollback.
    CHECK(copyRows(t, 0, 0, 1, 39, 5) == "history\n");
}

TEST(aBlockRidesABatchedScrollIntoTheScrollbackOnEveryParseEntry)
{
    // A session parses through the entry that stops at a synchronized-update
    // clear, which leaves a batched scroll lane run open across its return; a
    // run of line feeds at the bottom takes the lane through either entry, and
    // the block moves with it into the scrollback.
    std::string output = place("w=28:h=2", "{\\rtf1 history}");
    for (int line = 1; line <= 9; ++line)
        output += "line " + std::to_string(line) + (line < 9 ? "\r\n" : "");
    for (const bool isSessionEntry : { false, true }) {
        Ubiquitty::Terminal t;
        reset(t, 6, 30);
        t.isScrollLaneEnabled = true;
        if (isSessionEntry) {
            std::string_view rest = output;
            while (!rest.empty()) {
                const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, rest, nullptr);
                REQUIRE(consumed > 0);
                rest.remove_prefix(static_cast<std::size_t>(consumed));
            }
        } else {
            feed(t, output);
        }
        CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
        if (!primaryBlocks(t).empty())
            CHECK_EQ(primaryBlocks(t)[0].row, -5);
    }
}

TEST(aScrollInsideARegionMovesTheBlocksWhollyInsideIt)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[3;1H");
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 x}"));
    feed(t, "\x1b[20;1H");
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 y}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 2 });
    feed(t, "\x1b[2;10r\x1b[10;1H\n\x1b[r");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 2 });
    if (primaryBlocks(t).size() == 2) {
        CHECK_EQ(primaryBlocks(t)[0].row, 1);
        CHECK_EQ(primaryBlocks(t)[1].row, 19);
    }
    // A second scroll carries the first block past the region's top edge.
    feed(t, "\x1b[2;10r\x1b[10;1H\n\x1b[r");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
}

TEST(theAlternateScreenKeepsItsOwnBlocks)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=4:h=1", "{\\rtf1 main}"));
    feed(t, "\x1b[?1049h");
    feed(t, place("w=4:h=1", "{\\rtf1 alt}"));
    CHECK_EQ(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).size(), std::size_t { 1 });
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    feed(t, "\x1b[?1049l");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
}

TEST(aPlacementWithALiveIDReplacesThatBlock)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=4:h=1:i=3:C=1", "{\\rtf1 one}"));
    feed(t, "\x1b[5;20H");
    feed(t, place("w=4:h=1:i=3", "{\\rtf1 two}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    if (primaryBlocks(t).empty())
        return;
    CHECK_EQ(primaryBlocks(t)[0].row, 4);
    CHECK_EQ(primaryBlocks(t)[0].document.text.size(), std::size_t { 3 });
    CHECK(!isCoveredAt(t, 0, 0));
}

TEST(aPlacementOverAnotherBlockDisplacesIt)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=10:h=3:C=1", "{\\rtf1 under}"));
    feed(t, "\x1b[2;5H");
    feed(t, place("w=4:h=4", "{\\rtf1 over}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    // The displaced block's cells outside the new one are no longer covered.
    CHECK(!isCoveredAt(t, 0, 0));
    CHECK(isCoveredAt(t, 1, 4));
}

TEST(deleteRemovesByIDOrEveryBlock)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=4:h=1:i=1", "{\\rtf1 a}"));
    feed(t, place("w=4:h=1:i=2", "{\\rtf1 b}"));
    feed(t, place("w=4:h=1", "{\\rtf1 c}"));
    feed(t, "\x1b]7404;a=d:i=2\x1b\\");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 2 });
    CHECK(!isCoveredAt(t, 1, 0));
    feed(t, "\x1b]7404;a=d\x1b\\");
    CHECK(primaryBlocks(t).empty());
    CHECK(!isCoveredAt(t, 0, 0));
}

TEST(theSixtyFifthBlockIsRefusedWithNothingMutated)
{
    Ubiquitty::Terminal t;
    reset(t, 24, 80);
    for (int index = 0; index < maxBlocksPerScreen; ++index) {
        feed(t, "\x1b[" + std::to_string(index / 16 + 1) + ";" + std::to_string((index % 16) * 2 + 1) + "H");
        feed(t, place("w=1:h=1:C=1", "{\\rtf1 x}"));
    }
    CHECK_EQ(primaryBlocks(t).size(), static_cast<std::size_t>(maxBlocksPerScreen));
    feed(t, "\x1b[20;1Hkeep\x1b[20;1H");
    std::array<char, 64> drained {};
    (void)UbiquittyTest::drain(t, drained);
    feed(t, place("w=4:h=1", "{\\rtf1 one too many}"));
    CHECK_EQ(primaryBlocks(t).size(), static_cast<std::size_t>(maxBlocksPerScreen));
    CHECK_EQ(cell(t, 19, 0)->codePoint, std::uint32_t { 'k' });
    CHECK(!isCoveredAt(t, 19, 0));
    CHECK_EQ(t.screens.active->cursorY, 19);
    CHECK_EQ(t.screens.active->cursorX, 0);
    CHECK_EQ(UbiquittyTest::drain(t, drained), 0);
}

TEST(theTerminalTextMaximumRefusesAPlacement)
{
    Ubiquitty::Terminal t;
    reset(t);
    const std::string body = "{\\rtf1 " + std::string(static_cast<std::size_t>(maxCodePoints), 'x') + "}";
    const auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    REQUIRE(document);
    const auto allowance = 2 * (sizeof(Block) + document->storageBytes());
    REQUIRE(t.richTextBlocks.setStorageLimit(allowance));
    for (int index = 0; index < 2; ++index) {
        feed(t, "\x1b[" + std::to_string(index + 1) + ";1H");
        feed(t, place("w=2:h=1:C=1", body));
    }
    CHECK_EQ(t.richTextBlocks.storageBytes(), allowance);
    feed(t, "\x1b[10;1H");
    feed(t, place("w=2:h=1", "{\\rtf1 y}"));
    CHECK_EQ(t.richTextBlocks.storageBytes(), allowance);
    CHECK_EQ(t.richTextBlocks.codePointCount(), 2 * maxCodePoints);
    CHECK(!isCoveredAt(t, 9, 0));
}

TEST(retainedAdmissionChargesStyleStorageInsteadOfOnlyTextLength)
{
    Ubiquitty::Terminal t;
    reset(t);
    const std::string plain = "{\\rtf1 " + std::string(32, 'x') + "}";
    std::string styled = "{\\rtf1 ";
    for (int index = 0; index < 32; ++index)
        styled += index % 2 ? "\\b0 x" : "\\b x";
    styled += '}';
    const auto plainDocument = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()));
    const auto styledDocument = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(styled.data()), styled.size()));
    REQUIRE(plainDocument && styledDocument);
    CHECK_EQ(plainDocument->text.size(), styledDocument->text.size());
    CHECK(styledDocument->storageBytes() > plainDocument->storageBytes());
    REQUIRE(t.richTextBlocks.setStorageLimit(2 * (sizeof(Block) + plainDocument->storageBytes())));
    feed(t, place("w=10:h=1:C=1:i=1", plain));
    feed(t, "\x1b[2;1Hpreserved\r");
    feed(t, place("w=10:h=1:i=2", styled));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    CHECK_EQ(cell(t, 1, 0)->codePoint, std::uint32_t { 'p' });
    CHECK_EQ(t.screens.active->cursorY, 1);
    feed(t, place("w=10:h=1:i=2", plain));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 2 });
}

TEST(retainedAdmissionChargesFontStorageAndRefusesAReplacementAtomically)
{
    Ubiquitty::Terminal terminal;
    reset(terminal);
    const std::string body = R"({\rtf1{\fonttbl{\f0 )" + std::string(maxFamilyNameBytes, 'F') + ";}}x}";
    const auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    REQUIRE(document);
    REQUIRE(document->fonts.capacity() == 1);
    const auto fullCost = sizeof(Block) + document->storageBytes();
    REQUIRE(terminal.richTextBlocks.setStorageLimit(fullCost - sizeof(FontEntry)));
    feed(terminal, place("w=10:h=1:C=1:i=1", "{\\rtf1 original}"));
    REQUIRE(primaryBlocks(terminal).size() == 1);
    const auto held = terminal.richTextBlocks;
    feed(terminal, place("w=10:h=1:i=1", body));
    CHECK(terminal.richTextBlocks == held);
    CHECK_EQ(terminal.screens.active->cursorX, 0);
    CHECK_EQ(terminal.screens.active->cursorY, 0);
    REQUIRE(terminal.richTextBlocks.setStorageLimit(fullCost));
    feed(terminal, place("w=10:h=1:i=1", body));
    REQUIRE(primaryBlocks(terminal).size() == 1);
    CHECK(primaryBlocks(terminal).front().document == *document);
    CHECK_EQ(terminal.richTextBlocks.storageBytes(), fullCost);
}

TEST(retainedPressureRetiresOldestWholeHistoryAndKeepsTheRest)
{
    Ubiquitty::Terminal t;
    reset(t, 6, 40);
    const std::string body = "{\\rtf1 " + std::string(1000, 'x') + "}";
    const auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    REQUIRE(document);
    REQUIRE(t.richTextBlocks.setStorageLimit(2 * (sizeof(Block) + document->storageBytes())));
    feed(t, place("w=8:h=1:C=1:i=1", body));
    feed(t, "\x1b[2;1Hkeep\x1b[6;1H\n\n\x1b[1;1H");
    feed(t, place("w=8:h=1:C=1:i=2", body));
    feed(t, "\x1b[6;1H\n\n\x1b[4;20HLIVE\x1b[1;1H");
    REQUIRE(primaryBlocks(t).size() == 2);
    const auto retainedSerial = primaryBlocks(t)[1].serial;
    CHECK_EQ(t.scrollbackCount, 4);
    feed(t, place("w=8:h=1:C=1:i=3", body));
    REQUIRE(primaryBlocks(t).size() == 2);
    CHECK_EQ(primaryBlocks(t)[0].serial, retainedSerial);
    CHECK_EQ(primaryBlocks(t)[0].id, 2);
    CHECK_EQ(primaryBlocks(t)[1].id, 3);
    CHECK_EQ(t.scrollbackCount, 3);
    const auto* first = Ubiquitty::Screen::scrollbackLine(&t, 0);
    REQUIRE(first && first->cells && first->length >= 4);
    CHECK_EQ(first->cells[0].codePoint, std::uint32_t { 'k' });
    CHECK_EQ(cell(t, 3, 19)->codePoint, std::uint32_t { 'L' });
    CHECK(t.richTextBlocks.storageBytes() <= t.richTextBlocks.storageLimit());
}

TEST(historyPressureCannotSeverABlockThatStillTouchesTheLiveScreen)
{
    Ubiquitty::Terminal t;
    reset(t, 6, 40);
    feed(t, place("w=8:h=2:C=1:i=1", "{\\rtf1 oldest}"));
    feed(t, "\x1b[2;12H");
    feed(t, place("w=8:h=3:C=1:i=2", "{\\rtf1 crossing}"));
    feed(t, "\x1b[6;1H\n\n\x1b[5;25Hkeep\r\x1b[5;25H");
    REQUIRE(primaryBlocks(t).size() == 2);
    REQUIRE(t.richTextBlocks.setStorageLimit(t.richTextBlocks.storageBytes()));
    const auto held = t.richTextBlocks;
    const auto history = t.scrollbackCount;
    feed(t, place("w=8:h=1:i=3", "{\\rtf1 cannot fit}"));
    CHECK(t.richTextBlocks == held);
    CHECK_EQ(t.scrollbackCount, history);
    CHECK_EQ(t.screens.active->cursorX, 24);
    CHECK_EQ(t.screens.active->cursorY, 4);
    CHECK_EQ(cell(t, 4, 24)->codePoint, std::uint32_t { 'k' });
}

TEST(documentRevisionAllowsGeometryUpdatesButRejectsDifferentSource)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=10:h=1:C=1:i=5:D=1:M=1", "{\\rtf1 source}"));
    REQUIRE(primaryBlocks(t).size() == 1);
    const auto serial = primaryBlocks(t).front().serial;
    feed(t, "\x1b[3;1H");
    feed(t, place("w=12:h=1:C=1:i=5:D=1:M=1", "{\\rtf1 changed}"));
    CHECK_EQ(primaryBlocks(t).front().serial, serial);
    CHECK(!isCoveredAt(t, 2, 0));
    feed(t, place("w=12:h=1:C=1:i=5:D=1:M=1", "{\\rtf1 source}"));
    CHECK(primaryBlocks(t).front().serial != serial);
    CHECK_EQ(primaryBlocks(t).front().row, 2);
    CHECK_EQ(primaryBlocks(t).front().documentRevision, std::uint32_t { 1 });
}

TEST(aDeniedOriginPlacesNothing)
{
    Ubiquitty::Terminal t;
    reset(t);
    DenyingPolicy policy;
    t.capabilityPolicy = &policy;
    feed(t, place("w=4:h=1", "{\\rtf1 x}"));
    t.capabilityPolicy = nullptr;
    CHECK(primaryBlocks(t).empty());
    CHECK(!isCoveredAt(t, 0, 0));
}

TEST(aHardResetRemovesBlocksAndASoftResetKeepsThem)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, place("w=4:h=1", "{\\rtf1 x}"));
    feed(t, "\x1b[!p");
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    feed(t, "\x1b"
            "c");
    CHECK(primaryBlocks(t).empty());
}

TEST(aHardResetKeepsTheHostRichTextQuota)
{
    Ubiquitty::Terminal t;
    reset(t);
    constexpr std::size_t quota = 8192;
    REQUIRE(t.richTextBlocks.setStorageLimit(quota));
    feed(t, place("w=4:h=1:C=1", "{\\rtf1 x}"));
    REQUIRE(primaryBlocks(t).size() == 1);
    feed(t, "\x1b"
            "c");
    CHECK(primaryBlocks(t).empty());
    CHECK_EQ(t.richTextBlocks.storageLimit(), quota);
    // A program cannot raise the host's quota by resetting the terminal.
    const std::string body = "{\\rtf1 " + std::string(static_cast<std::size_t>(maxCodePoints), 'x') + "}";
    feed(t, place("w=8:h=1", body));
    CHECK(primaryBlocks(t).empty());
}

TEST(aCopyReadsTheBlockWholeAtItsTopLeftInReadingOrder)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "before\r\n");
    feed(t, "L ");
    feed(t, place("w=10:h=2:C=1", "{\\rtf1\\b Title\\b0\\par body}"));
    feed(t, "\x1b[2;13HR\x1b[3;13Hnext\r\nafter");
    // From the row above to the row below: the block's text sits where its
    // top-left cell is, the cells beside it follow on their rows, and its
    // other cells add nothing.
    const std::string copied = copyRows(t, 0, 0, 3, 4);
    CHECK_MSG(copied == "before\nL Title\nbodyR\n  next\nafter", copied.c_str());
}

TEST(aCopyThatEndsInsideABlockStillTakesItWhole)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "x ");
    feed(t, place("w=6:h=2:C=1", "{\\rtf1 inside}"));
    // A cell selection inside the block's rectangle takes the block whole,
    // and its second row adds only the row break; the character carets are
    // the pointer ladder's.
    const std::string copied = copyRows(t, 0, 0, 1, 4);
    CHECK_MSG(copied == "x inside\n", copied.c_str());
}

TEST(anApplicationOverlaySurvivesRepaintUntilExplicitlyDeleted)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[?1049h");
    feed(t, place("w=20:h=1:i=7:D=1:C=1:g=1:M=1:P=1", "{\\rtf1 Hello}"));
    REQUIRE(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).size() == 1);
    const auto serial = t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).front().serial;
    feed(t, "\x1b[1;1H\x1b[2K");
    const auto blocks = t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate);
    CHECK_EQ(blocks.size(), std::size_t { 1 });
    CHECK_EQ(blocks.front().serial, serial);
    CHECK(blocks.front().lineMode == LineMode::Terminal);
    CHECK(blocks.front().pointerMode == PointerMode::Characters);
    feed(t, "\x1b]7404;a=d:i=7\x1b\\");
    CHECK(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).empty());
}

TEST(applicationOverlaysDoNotSurviveAFullClearOrAnAlternateScreenExit)
{
    Ubiquitty::Terminal t;
    reset(t);
    feed(t, "\x1b[?1049h");
    feed(t, place("w=20:h=1:i=7:C=1:P=1", "{\\rtf1 Old prompt}"));
    feed(t, "\x1b[2J");
    CHECK(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).empty());
    feed(t, place("w=20:h=1:i=7:C=1:P=1", "{\\rtf1 Old prompt}"));
    feed(t, "\x1b[?1049l");
    CHECK(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).empty());
    feed(t, "\x1b[?1049h");
    CHECK(t.richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).empty());
}

TEST(visibleRectanglesPreserveGridContentAndParagraphGeometry)
{
    Ubiquitty::Terminal t;
    reset(t, 4, 20);
    feed(t, "000MENU000\r");
    feed(t, place("w=10:h=1:i=1:C=1:P=1:r=0,0,3,1,7,0,3,1", "{\\rtf1 0123456789}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 1 });
    const auto& block = primaryBlocks(t).front();
    CHECK_EQ(block.visibleRects.size(), std::size_t { 2 });
    CHECK(block.covers(0, 2));
    CHECK(!block.covers(0, 3));
    CHECK(block.covers(0, 7));
    CHECK_EQ(cell(t, 0, 3)->codePoint, std::uint32_t { 'M' });
    CHECK_EQ(cell(t, 0, 6)->codePoint, std::uint32_t { 'U' });
    CHECK_EQ(layoutOf(t, block).lineCount(), std::size_t { 1 });
    CHECK_EQ(layoutOf(t, block).line(0).end, std::uint32_t { 10 });
    CHECK(blockCovering(t, Ubiquitty::ScreenMode::Primary, 0, 3) == nullptr);
    // Disjoint visible regions may share a paragraph's bounding rectangle.
    feed(t, place("w=10:h=1:i=2:C=1:P=1:r=3,0,4,1", "{\\rtf1 abcdefghij}"));
    CHECK_EQ(primaryBlocks(t).size(), std::size_t { 2 });
}

TEST(invalidVisibleRectanglesRefuseTheWholePlacement)
{
    for (const auto keys : { "r=", "r=0,0,1", "r=0,0,1,1,", "r=0,0,0,1", "r=9,0,2,1", "r=0,1,1,1", "r=0,0,4,1,3,0,4,1", "r=0,0,1,1:r=2,0,1,1" }) {
        Ubiquitty::Terminal t;
        reset(t);
        feed(t, "preserved\r");
        feed(t, place(std::string("w=10:h=1:") + keys, "{\\rtf1 rejected}"));
        CHECK(primaryBlocks(t).empty());
        CHECK_EQ(cell(t, 0, 0)->codePoint, std::uint32_t { 'p' });
    }
    CHECK(!Ubiquitty::OSC::RichTextBlocks::parseKeys("a=q:w=10:h=1:r=0,0,1,1"));
    CHECK(!Ubiquitty::OSC::RichTextBlocks::parseKeys("a=d:r=0,0,1,1"));
    CHECK(!Ubiquitty::OSC::RichTextBlocks::parseKeys("w=10:h=1:"));
    std::string rectangles;
    for (int column = 0; column <= 32; ++column) {
        if (column)
            rectangles += ',';
        rectangles += std::to_string(column) + ",0,1,1";
    }
    CHECK(!Ubiquitty::OSC::RichTextBlocks::parseKeys("w=40:h=1:r=" + rectangles));
}

UBIQUITTY_TEST_MAIN()
