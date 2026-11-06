// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_row_ops.cpp — the chain update's two row ops: `Spans`, the cells
// of a row that changed, and `ScrollRect`, rows that moved (Docs/sync.md §6.3,
// §7.4).
//
// The properties held:
//
// - A keystroke at a prompt is one `Spans` op, an update far smaller than its
//   row.
// - A margined scroll several generations deep is one `ScrollRect` and the rows
//   it revealed, and the client converges.
// - A superset update, which must apply onto any generation the client may
//   hold, carries neither, and neither goes to a client that accepts neither.
// - A `ScrollRect` outside the grid, or one after a row of its screen, and a
//   `Spans` against a row the client does not hold, are refused before the
//   terminal changes.
// - A scaled proportional run's band rows arrive whole, flowed and naming
//   their text, and a replica that draws no sized bands flattens them.

#include "Harness.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// Every optional op.
constexpr ChainOps bothOps { ChainOp::Spans, ChainOp::ScrollRect };

/// A reset terminal of a size.
///
/// - Parameters:
///   - rows: Its rows.
///   - columns: Its columns.
/// - Returns: The terminal.
std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

/// A host and a client one checkpoint apart, and the publisher between them.
struct Pair {
    std::unique_ptr<Ubiquitty::Terminal> host;
    std::unique_ptr<Ubiquitty::Terminal> client;
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;

    Pair(int rows, int columns, std::string_view initial)
        : host(makeTerminal(rows, columns))
        , client(makeTerminal(rows, columns))
    {
        UbiquittyTest::feed(*host, initial);
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
        REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    }

    /// Captures the host and encodes the update that brings the client to it.
    ///
    /// - Parameter chainOps: The optional ops the update may use.
    /// - Returns: The body.
    std::vector<std::uint8_t> update(ChainOps chainOps)
    {
        std::vector<std::uint8_t> body;
        CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body, chainOps) == UpdateOutcome::Encoded);
        return body;
    }
};

/// Lines that name themselves, one a row.
///
/// - Parameters:
///   - first: The first line's number.
///   - count: How many.
/// - Returns: The bytes, each line ending in CR LF but the last.
std::string numberedLines(int first, int count)
{
    std::string bytes;
    for (int line = first; line < first + count; ++line) {
        if (line > first)
            bytes += "\r\n";
        bytes += "line " + std::to_string(line) + " of a file an editor scrolls";
    }
    return bytes;
}

/// The code points of a row of a terminal's active screen.
///
/// - Parameters:
///   - terminal: The terminal.
///   - row: The row.
/// - Returns: One `char` per cell, its code point's low byte.
std::string rowText(const Ubiquitty::Terminal& terminal, int row)
{
    std::string text;
    for (int column = 0; column < terminal.columns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
        text.push_back(cell ? static_cast<char>(cell->codePoint & 0x7F) : '?');
    }
    return text;
}

} // namespace

// A typed character is one Spans op, the update a few dozen
// bytes against the row's whole record.
TEST(aKeystrokeEchoIsOneSpansOp)
{
    Pair pair(24, 80, "\x1b[1;32muser@host\x1b[0m:\x1b[1;34m~/src/ubiquitty\x1b[0m$ git status");
    UbiquittyTest::feed(*pair.host, "x");
    const std::vector<std::uint8_t> body = pair.update(bothOps);
    CHECK_EQ(pair.publisher.lastUpdateSpans(), std::size_t { 1 });
    // About fifty bytes of op and digest and the cursor block, and the sixteen
    // of the base and new row hashes a client checks the spans against.
    CHECK_MSG(body.size() <= 80, ("a keystroke's update took " + std::to_string(body.size()) + " bytes").c_str());
    CHECK(applyRowUpdate(*pair.client, body, pair.replica) == ApplyOutcome::Applied);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(*pair.host, *pair.client), "");

    // The same keystroke to a client that accepts no optional op is its row.
    Pair whole(24, 80, "\x1b[1;32muser@host\x1b[0m:\x1b[1;34m~/src/ubiquitty\x1b[0m$ git status");
    UbiquittyTest::feed(*whole.host, "x");
    const std::vector<std::uint8_t> row = whole.update({});
    CHECK_EQ(whole.publisher.lastUpdateSpans(), std::size_t { 0 });
    CHECK_MSG(row.size() > body.size(), ("its row took " + std::to_string(row.size()) + " bytes against " + std::to_string(body.size())).c_str());
    std::printf("aKeystrokeEchoIsOneSpansOp: %zu bytes as spans, %zu as its row\n", body.size(), row.size());
}

TEST(activeRowAndSpansRestoreTheFlowedFlagForAlreadyHeldRunText)
{
    for (const bool isAlternate : { false, true }) {
        for (const UpdateOp operation : { UpdateOp::Row, UpdateOp::Spans }) {
            Pair pair(32, 77, isAlternate ? "\x1b[?1049h" : "");
            pair.host->drawsSizedText = true;
            pair.host->drawsProportionalText = true;
            pair.client->drawsSizedText = true;
            pair.client->drawsProportionalText = true;
            UbiquittyTest::feed(*pair.host, "\x1b[1;10H\x1b]66;p=1:c=58;Review queue\x1b\\");
            std::vector<std::uint8_t> initial;
            REQUIRE(pair.publisher.capture(*pair.host));
            REQUIRE(pair.publisher.checkpoint(*pair.host, 2, initial, pair.baseline));
            REQUIRE(applyCheckpoint(*pair.client, initial, pair.replica) == ApplyOutcome::Applied);
            constexpr int targetRow = 1;
            const int screen = isAlternate ? 1 : 0;
            const auto mode = isAlternate ? Ubiquitty::ScreenMode::Alternate : Ubiquitty::ScreenMode::Primary;
            REQUIRE(!pair.client->grid->rowMetaAt(screen, targetRow).has(Ubiquitty::RowFlag::Flowed));
            const auto source = pair.host->grid->rowCells(mode, Ubiquitty::Row(0));
            REQUIRE(Ubiquitty::isProportionalRunAnchor(pair.client.get(), source.data[9]));
            REQUIRE(!Ubiquitty::runTextCodePoints(pair.client.get(), source.data[9]).empty());
            pair.host->grid->setRowSpan(mode, Ubiquitty::Row(targetRow), Ubiquitty::Column(0), source);
            Ubiquitty::Screen::latchFlowedRow(*pair.host, screen, targetRow);
            REQUIRE(pair.publisher.capture(*pair.host));

            // The producer checkpoints changed run-text rows. The decoder also
            // admits Row and Spans that reference text already installed by a
            // checkpoint; construct those valid operations without a new ID.
            AttachmentBaseline expectedBaseline;
            std::vector<std::uint8_t> expected;
            REQUIRE(pair.publisher.checkpoint(*pair.host, 3, expected, expectedBaseline));
            constexpr std::size_t digestBytes = 16;
            REQUIRE(expected.size() >= digestBytes);
            std::vector<std::uint8_t> body;
            WireWriter writer(body);
            writer.u8(isAlternate ? 1 : 0);
            writer.u16(32);
            writer.u16(77);
            writer.u32(pair.replica.dictionaryEpoch);
            writer.varint(1);
            writer.u8(std::to_underlying(operation));
            if (operation == UpdateOp::Spans)
                writer.u8(0);
            writer.varint(targetRow);
            if (operation == UpdateOp::Spans) {
                writer.u64(pair.replica.rowHashes()[screen][targetRow]);
                writer.u64(pair.publisher.rows(screen).hash(targetRow));
                writer.varint(1);
                writer.u16(0);
                writer.u16(77);
            }
            writer.bytes(pair.publisher.rows(screen).record(targetRow));
            writer.raw(std::span(expected).last(digestBytes));
            REQUIRE(applyRowUpdate(*pair.client, body, pair.replica) == ApplyOutcome::Applied);
            const auto target = pair.client->grid->rowCells(mode, Ubiquitty::Row(targetRow));
            CHECK(Ubiquitty::isProportionalRunAnchor(pair.client.get(), target.data[9]));
            CHECK(std::ranges::equal(Ubiquitty::runTextCodePoints(pair.host.get(), source.data[9]),
                Ubiquitty::runTextCodePoints(pair.client.get(), target.data[9])));
            CHECK(pair.client->grid->rowMetaAt(screen, targetRow).has(Ubiquitty::RowFlag::Flowed));
            CHECK(std::ranges::equal(pair.replica.rowHashes()[0], expectedBaseline.rowHashes[0]));
            CHECK(std::ranges::equal(pair.replica.rowHashes()[1], expectedBaseline.rowHashes[1]));
        }
    }
}

// A scaled proportional run travels as the block it is: its spec keeps its
// scale on the wire, every band row arrives with its anchor naming the run's
// text and is flowed, a later run arrives on the next checkpoint (new run
// text is a checkpoint's to carry), and the client agrees with the host cell
// for cell. A replica whose renderer draws runs but
// no sized bands reduces the block to its text's first letter, as it reduces
// a sized glyph.
TEST(aScaledProportionalRunTravelsAsABlockOfBands)
{
    const auto drawBoth = [](Ubiquitty::Terminal& terminal) {
        terminal.drawsSizedText = true;
        terminal.drawsProportionalText = true;
    };
    Pair pair(8, 40, "");
    drawBoth(*pair.host);
    drawBoth(*pair.client);
    UbiquittyTest::feed(*pair.host, "\x1b[2;3H\x1b]66;p=1:c=3:s=2;Hello\x1b\\");
    std::vector<std::uint8_t> body;
    REQUIRE(pair.publisher.capture(*pair.host) && pair.publisher.checkpoint(*pair.host, 2, body, pair.baseline));
    REQUIRE(applyCheckpoint(*pair.client, body, pair.replica) == ApplyOutcome::Applied);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(*pair.host, *pair.client), "");
    for (int band = 0; band < 2; ++band) {
        const Ubiquitty::Cell* anchor = Ubiquitty::Screen::visibleCell(pair.client.get(), 0, 1 + band, 2);
        REQUIRE(anchor && Ubiquitty::isProportionalRunAnchor(pair.client.get(), *anchor));
        CHECK_EQ(static_cast<int>(Ubiquitty::penTextBand(&Ubiquitty::cellPen(*anchor))), band);
        const Ubiquitty::TextSizeSpec* spec = pair.client->textSizeTable.lookup(Ubiquitty::cellPen(*anchor).extendedAttributes.textSizeSpecID());
        CHECK(spec && spec->scale == 2 && spec->runWidthCells == 3);
        const auto text = Ubiquitty::runTextCodePoints(pair.client.get(), *anchor);
        CHECK(text.size() == 5 && text[0] == 'H' && text[4] == 'o');
        CHECK(pair.client->grid->rowMetaAt(0, 1 + band).has(Ubiquitty::RowFlag::Flowed));
    }

    UbiquittyTest::feed(*pair.host, "\x1b[5;1H\x1b]66;p=1:c=2:s=3;Hi\x1b\\");
    std::vector<std::uint8_t> next;
    REQUIRE(pair.publisher.capture(*pair.host) && pair.publisher.checkpoint(*pair.host, 3, next, pair.baseline));
    REQUIRE(applyCheckpoint(*pair.client, next, pair.replica) == ApplyOutcome::Applied);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(*pair.host, *pair.client), "");
    const Ubiquitty::Cell* lowest = Ubiquitty::Screen::visibleCell(pair.client.get(), 0, 6, 0);
    CHECK(lowest && Ubiquitty::isProportionalRunAnchor(pair.client.get(), *lowest) && Ubiquitty::penTextBand(&Ubiquitty::cellPen(*lowest)) == 2);

    auto gridOnly = makeTerminal(8, 40);
    gridOnly->drawsProportionalText = true;
    ScreenReplica gridOnlyReplica;
    std::vector<std::uint8_t> latest;
    AttachmentBaseline latestBaseline;
    REQUIRE(pair.publisher.capture(*pair.host) && pair.publisher.checkpoint(*pair.host, 4, latest, latestBaseline));
    REQUIRE(applyCheckpoint(*gridOnly, latest, gridOnlyReplica) == ApplyOutcome::Applied);
    const Ubiquitty::Cell* flattened = Ubiquitty::Screen::visibleCell(gridOnly.get(), 0, 1, 2);
    CHECK(flattened && flattened->codePoint == 'H' && Ubiquitty::cellPen(*flattened).extendedAttributes.textSizeSpecID() == 0);
    const Ubiquitty::Cell* lowerBand = Ubiquitty::Screen::visibleCell(gridOnly.get(), 0, 2, 2);
    CHECK(lowerBand && lowerBand->codePoint == 0);
}

// An editor scrolls three lines inside the margins (rows 3 to 23 of 24) while
// the client skipped two generations: one ScrollRect, the three rows it
// revealed, and no more.
TEST(aMarginedScrollTravelsAsOneScrollRect)
{
    Pair pair(24, 80, "\x1b[1;1Hheader line\x1b[3;23r\x1b[3;1H" + numberedLines(0, 21) + "\x1b[24;1Hstatus line");
    std::size_t rowBytes = 0;
    {
        Pair whole(24, 80, "\x1b[1;1Hheader line\x1b[3;23r\x1b[3;1H" + numberedLines(0, 21) + "\x1b[24;1Hstatus line");
        for (int line = 21; line < 24; ++line)
            UbiquittyTest::feed(*whole.host, "\x1b[23;1H\r\n" + numberedLines(line, 1));
        rowBytes = whole.update({}).size();
    }
    for (int line = 21; line < 24; ++line) {
        UbiquittyTest::feed(*pair.host, "\x1b[23;1H\r\n" + numberedLines(line, 1));
        // A generation the client is never sent.
        if (line < 23)
            REQUIRE(pair.publisher.capture(*pair.host));
    }
    const std::vector<std::uint8_t> body = pair.update(bothOps);
    CHECK_EQ(pair.publisher.lastUpdateScrollRects(), std::size_t { 1 });
    CHECK_EQ(pair.publisher.lastUpdateRows(), std::size_t { 3 });
    CHECK_MSG(body.size() * 4 < rowBytes, ("the scroll took " + std::to_string(body.size()) + " bytes against " + std::to_string(rowBytes) + " in rows").c_str());
    CHECK(applyRowUpdate(*pair.client, body, pair.replica) == ApplyOutcome::Applied);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(*pair.host, *pair.client), "");
    std::printf("aMarginedScrollTravelsAsOneScrollRect: %zu bytes as a scroll and three rows, %zu as rows\n", body.size(), rowBytes);
}

// Over a link with datagrams and 10% loss, the superset updates carry neither
// op however the screen scrolls and changes, and the client converges.
TEST(aSupersetUpdateCarriesNoScrollOp)
{
    SyncLink link(1'200, UbiquittyTest::syncDefaultFacts(24, 80));
    const std::uint32_t pane = link.addPane(24, 80, numberedLines(0, 23));
    link.setLossPermille(100);
    for (int frame = 0; frame < 60 && link.host().counters().supersetUpdatesSent == 0; ++frame) {
        link.write(pane, "\r\n" + numberedLines(100 + frame, 1));
        link.step();
    }
    REQUIRE(link.host().counters().supersetUpdatesSent > 0);
    const HostCounters before = link.host().counters();
    for (int frame = 0; frame < 300; ++frame) {
        link.write(pane, frame % 3 == 0 ? "\r\n" + numberedLines(1'000 + frame, 1) : std::string("x"));
        link.step();
    }
    const HostCounters& after = link.host().counters();
    CHECK(after.supersetUpdatesSent > before.supersetUpdatesSent);
    CHECK_EQ(after.chainUpdatesSent, before.chainUpdatesSent);
    CHECK_EQ(after.spansOps, before.spansOps);
    CHECK_EQ(after.scrollRectOps, before.scrollRectOps);
    link.setLossPermille(0);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// A ScrollRect that reaches past the grid, or comes after a row of its screen,
// and a Spans against a row the client does not hold, are refused; the
// terminal and the replica stay as they were.
TEST(aScrollRectOutsideTheGridIsRefusedBeforeMutation)
{
    Pair pair(24, 80, numberedLines(0, 24));
    const std::string before = rowText(*pair.client, 5);
    const auto body = [&](const std::function<void(WireWriter&)>& ops, std::size_t count) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.u8(0);
        writer.u16(24);
        writer.u16(80);
        writer.u32(pair.replica.dictionaryEpoch);
        writer.varint(count);
        ops(writer);
        writer.raw(std::array<std::uint8_t, 16> {});
        return bytes;
    };
    const ScreenReplica held = pair.replica;
    const std::vector<std::uint8_t> pastTheGrid = body([](WireWriter& writer) {
        writer.u8(std::to_underlying(UpdateOp::ScrollRect));
        writer.u8(0);
        writer.u16(5);
        writer.u16(30);
        writer.svarint(1);
    },
        1);
    CHECK(applyRowUpdate(*pair.client, pastTheGrid, pair.replica) == ApplyOutcome::Malformed);
    const std::vector<std::uint8_t> pastItsRows = body([](WireWriter& writer) {
        writer.u8(std::to_underlying(UpdateOp::ScrollRect));
        writer.u8(0);
        writer.u16(5);
        writer.u16(10);
        writer.svarint(6);
    },
        1);
    CHECK(applyRowUpdate(*pair.client, pastItsRows, pair.replica) == ApplyOutcome::Malformed);
    const std::vector<std::uint8_t> afterARow = body([](WireWriter& writer) {
        writer.u8(std::to_underlying(UpdateOp::Row));
        writer.varint(2);
        writer.bytes(std::vector<std::uint8_t> { 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        writer.u8(std::to_underlying(UpdateOp::ScrollRect));
        writer.u8(0);
        writer.u16(5);
        writer.u16(10);
        writer.svarint(1);
    },
        2);
    CHECK(applyRowUpdate(*pair.client, afterARow, pair.replica) == ApplyOutcome::Malformed);
    const std::vector<std::uint8_t> wrongBase = body([](WireWriter& writer) {
        writer.u8(std::to_underlying(UpdateOp::Spans));
        writer.u8(0);
        writer.varint(5);
        writer.u64(0x1234);
        writer.u64(0x5678);
        writer.varint(0);
    },
        1);
    CHECK(applyRowUpdate(*pair.client, wrongBase, pair.replica) == ApplyOutcome::DigestMismatch);
    CHECK(pair.replica.storage == held.storage);
    CHECK_STR_EQ(rowText(*pair.client, 5), before);
}

UBIQUITTY_TEST_MAIN()
