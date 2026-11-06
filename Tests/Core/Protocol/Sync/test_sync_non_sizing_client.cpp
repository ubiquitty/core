// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"

// test_sync_non_sizing_client.cpp — what a client that draws no sized text
// holds after the screen plane delivers a sizing host's OSC 66 blocks.
//
// `drawsSizedText` is a frontend's own fact, not something the host is told, so
// a sizing host sends sized cells to every client. A client that cannot band a
// sized run paints the whole glyph on every row it covers — stacked duplicates —
// unless the apply turns each run back into ordinary cells. The invariant every
// case below asserts: such a client holds no cell carrying a size id, on either
// screen or in its history, after a checkpoint and after an update. A sizing
// client keeps the runs, with ids that resolve.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

std::unique_ptr<Terminal> makeSizingHost()
{
    auto host = std::make_unique<Terminal>();
    hardReset(*host);
    resize(*host, 4, 20);
    host->drawsSizedText = true;
    host->drawsProportionalText = true;
    return host;
}

unsigned sizeIDOf(const Cell& cell) { return cellPen(cell).extendedAttributes.textSizeSpecID(); }

// The cells of one screen that carry a size id.
int sizedCellsOn(const Terminal& terminal, ScreenMode screen)
{
    int count = 0;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Cell* cell = terminal.grid->cellAt(screen, Row(row), Column(column));
            count += cell && sizeIDOf(*cell) != 0 ? 1 : 0;
        }
    }
    return count;
}

// The history cells that carry a size id.
int sizedCellsInHistory(const Terminal& terminal)
{
    int count = 0;
    for (int index = 0; index < Screen::scrollbackCount(&terminal); ++index) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&terminal, index);
        if (!line)
            continue;
        for (int column = 0; column < line->length; ++column)
            count += sizeIDOf(line->cells[column]) != 0 ? 1 : 0;
    }
    return count;
}

// A connected host and client, the client's checkpoint applied.
struct Pair {
    std::unique_ptr<Terminal> host = makeSizingHost();
    std::unique_ptr<Terminal> client = std::make_unique<Terminal>();
    ScreenPublisher publisher;
    AttachmentBaseline attachment;
    ScreenReplica replica;

    /// Applies a checkpoint of the host as it now stands.
    ApplyOutcome checkpoint()
    {
        std::vector<std::uint8_t> body;
        if (!publisher.capture(*host) || !publisher.checkpoint(*host, ++generation, body, attachment))
            return ApplyOutcome::Malformed;
        return applyCheckpoint(*client, body, replica);
    }

    /// Applies the update that brings the client to the host as it now stands.
    ApplyOutcome update()
    {
        std::vector<std::uint8_t> body;
        if (!publisher.capture(*host) || publisher.rowUpdate(*host, attachment, body) != UpdateOutcome::Encoded)
            return ApplyOutcome::Malformed;
        return applyRowUpdate(*client, body, replica);
    }

    std::uint64_t generation = 0;
};

} // namespace

// A sized block scrolled into history reaches a non-sizing client on the
// history plane, and is flattened as the client wraps it into its scrollback,
// as on the visible rows. A flatten that read only the visible rows would
// leave history cells naming ids the client never resolves.
TEST(projectedHistoryIsFlattenedForANonSizingClient)
{
    Pair pair;
    processBytes(pair.host.get(), "\033]66;s=3;X\007\r\n");
    for (int line = 0; line < 12; ++line)
        processBytes(pair.host.get(), "filler\r\n");
    REQUIRE(Screen::scrollbackCount(pair.host.get()) > 0);
    CHECK_MSG(sizedCellsInHistory(*pair.host) > 0, "premise: the sized block reached the host's history");

    CHECK(!pair.client->drawsSizedText);
    CHECK(pair.checkpoint() == ApplyOutcome::Applied);
    HistoryPublisher history;
    REQUIRE(history.capture(*pair.host));
    const Screen::HistoryAddress address = history.address();
    std::vector<std::uint8_t> body;
    REQUIRE(history.writeBody(*pair.host, { address.oldest, address.head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) > 0);
    std::vector<DecodedLine> lines;
    REQUIRE(decodeHistoryBody(body, address.oldest, HistoryOrder::Ascending, historyBodyLimits, lines));
    HistoryCache cache({}, {}, 1'000);
    REQUIRE(cache.insert(address.epoch, lines) == CacheInsert::Stored);
    cache.setHead(address.epoch, address.head);
    cache.project(*pair.client, pair.client->columns);
    CHECK_MSG(Screen::scrollbackCount(pair.client.get()) > 0, "the client holds history, else the invariant is vacuous");
    CHECK_MSG(sizedCellsInHistory(*pair.client) == 0, "a non-sizing client holds a sized cell in its history");
    CHECK(sizedCellsOn(*pair.client, ScreenMode::Primary) == 0);
}

// The inactive screen's arm: a host on the alternate screen carries its frozen
// primary, sized cells and all, and the flatten has to read that grid too — it
// is what the user sees the moment the alternate application exits.
TEST(aCheckpointFlattensSizedCellsOnTheInactiveScreen)
{
    Pair pair;
    processBytes(pair.host.get(), "\033]66;s=2;W\007");
    processBytes(pair.host.get(), "\033[?1049h");
    REQUIRE(pair.host->screens.isUsingAlternateScreen);
    CHECK_MSG(sizedCellsOn(*pair.host, ScreenMode::Primary) > 0, "premise: the host's frozen primary holds a sized cell");

    CHECK(pair.checkpoint() == ApplyOutcome::Applied);
    CHECK_MSG(sizedCellsOn(*pair.client, ScreenMode::Primary) == 0, "a non-sizing client holds sized cells on its inactive screen");
}

// The update arm: a block drawn after the attach arrives in an update, which
// carries the size table beside its rows, so the ids would resolve on the
// client and still render as stacked duplicates there.
TEST(anUpdateFlattensSizedCellsForANonSizingClient)
{
    Pair pair;
    CHECK(pair.checkpoint() == ApplyOutcome::Applied);
    processBytes(pair.host.get(), "\033]66;s=3;X\007");
    CHECK_MSG(sizedCellsOn(*pair.host, ScreenMode::Primary) > 0, "premise: a sized block is live on the host grid");

    CHECK(pair.update() == ApplyOutcome::Applied);
    CHECK_MSG(sizedCellsOn(*pair.client, ScreenMode::Primary) == 0, "a non-sizing client holds a sized cell after an update");
    const Cell* anchor = Screen::visibleCell(pair.client.get(), 0, 0, 0);
    CHECK_MSG(anchor && anchor->codePoint == 'X', "and the flattened run keeps its text");
}

// The sizing arm: a client that draws sized text keeps the runs an update
// brings, and their ids resolve in its table.
TEST(aSizingClientKeepsAndResolvesSizedCellsFromAnUpdate)
{
    Pair pair;
    pair.client->drawsSizedText = true;
    pair.client->drawsProportionalText = true;
    CHECK(pair.checkpoint() == ApplyOutcome::Applied);
    processBytes(pair.host.get(), "\033]66;s=3;X\007");
    CHECK(pair.update() == ApplyOutcome::Applied);

    const Cell* anchor = Screen::visibleCell(pair.client.get(), 0, 0, 0);
    REQUIRE(anchor);
    const unsigned id = sizeIDOf(*anchor);
    CHECK_MSG(id != 0, "a sizing client keeps the sized run");
    CHECK_MSG(id == 0 || pair.client->textSizeTable.lookup(id) != nullptr, "and its id resolves in the client's table");
}

// The two facts are separate: a client that crops sized bands but draws no
// proportional run keeps the block and flattens the run, the run's text
// staying on its anchor cell, and the reverse client does the reverse.
TEST(aClientFlattensOnlyTheRunsItsRendererDoesNotDraw)
{
    for (const bool drawsProportional : { false, true }) {
        Pair pair;
        pair.client->drawsSizedText = !drawsProportional;
        pair.client->drawsProportionalText = drawsProportional;
        processBytes(pair.host.get(), "\033[3;1H\033]66;p=1:c=4;Run\007\033[H");
        CHECK(pair.checkpoint() == ApplyOutcome::Applied);
        processBytes(pair.host.get(), "\033]66;s=2;X\007");
        CHECK(pair.update() == ApplyOutcome::Applied);

        const Cell* block = Screen::visibleCell(pair.client.get(), 0, 0, 0);
        const Cell* run = Screen::visibleCell(pair.client.get(), 0, 2, 0);
        REQUIRE(block && run);
        CHECK(block->codePoint == 'X' && run->codePoint == 'R');
        CHECK_MSG((sizeIDOf(*block) != 0) == !drawsProportional, "the sized block survives only where sized text is drawn");
        CHECK_MSG((sizeIDOf(*run) != 0) == drawsProportional, "the run survives only where proportional text is drawn");
    }
}

TEST(aCheckpointFlattensOnlyTheUnsupportedKindOnBothScreens)
{
    for (const bool drawsProportional : { false, true }) {
        Pair pair;
        pair.client->drawsSizedText = !drawsProportional;
        pair.client->drawsProportionalText = drawsProportional;
        for (const auto screen : { "", "\033[?1049h" }) {
            processBytes(pair.host.get(), screen);
            processBytes(pair.host.get(), "\033[H\033]66;s=2;X\007\033[3;1H\033]66;p=1:c=4;Run\007");
        }
        REQUIRE(pair.checkpoint() == ApplyOutcome::Applied);
        for (const auto mode : { ScreenMode::Primary, ScreenMode::Alternate }) {
            const Cell* block = pair.client->grid->cellAt(mode, Row(0), Column(0));
            const Cell* run = pair.client->grid->cellAt(mode, Row(2), Column(0));
            REQUIRE(block && run);
            CHECK(block->codePoint == 'X' && run->codePoint == 'R');
            CHECK((sizeIDOf(*block) != 0) == !drawsProportional);
            CHECK((sizeIDOf(*run) != 0) == drawsProportional);
        }
    }
}

TEST(projectedHistoryFlattensOnlyTheUnsupportedKind)
{
    for (const bool drawsProportional : { false, true }) {
        Pair pair;
        pair.client->drawsSizedText = !drawsProportional;
        pair.client->drawsProportionalText = drawsProportional;
        processBytes(pair.host.get(), "\033]66;s=2;X\007\r\n\r\n\033]66;p=1:c=4;Run\007\r\n");
        for (int line = 0; line < 12; ++line)
            processBytes(pair.host.get(), "filler\r\n");
        REQUIRE(pair.checkpoint() == ApplyOutcome::Applied);
        HistoryPublisher history;
        REQUIRE(history.capture(*pair.host));
        const Screen::HistoryAddress address = history.address();
        std::vector<std::uint8_t> body;
        REQUIRE(history.writeBody(*pair.host, { address.oldest, address.head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) > 0);
        std::vector<DecodedLine> lines;
        REQUIRE(decodeHistoryBody(body, address.oldest, HistoryOrder::Ascending, historyBodyLimits, lines));
        HistoryCache cache({}, {}, 1'000);
        REQUIRE(cache.insert(address.epoch, lines) == CacheInsert::Stored);
        cache.setHead(address.epoch, address.head);
        cache.project(*pair.client, pair.client->columns);
        bool foundBlock = false;
        bool foundRun = false;
        for (int index = 0; index < Screen::scrollbackCount(pair.client.get()); ++index) {
            const Screen::ScrollLine* line = Screen::scrollbackLine(pair.client.get(), index);
            REQUIRE(line);
            for (int column = 0; column < line->length; ++column) {
                const Cell& cell = line->cells[column];
                if (cell.codePoint == 'X') {
                    foundBlock = true;
                    CHECK((sizeIDOf(cell) != 0) == !drawsProportional);
                } else if (cell.codePoint == 'R') {
                    foundRun = true;
                    CHECK((sizeIDOf(cell) != 0) == drawsProportional);
                }
            }
        }
        CHECK(foundBlock && foundRun);
    }
}

UBIQUITTY_TEST_MAIN()
