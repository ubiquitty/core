// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_loopback_pane_link.cpp — does the oracle see a client that drifted?
//
// LoopbackPaneLink replays PTY reads through the coherence rule and both ends
// of a Sync connection into a passive client, and compares the client with the host
// after every frame. An oracle that has never been seen to fail has not been
// shown to test anything, so the cases here come in pairs: a stream the wire
// carries faithfully passes, and the same stream with one decoded cell flipped
// is reported, at the frame and the cell where it was flipped.

#include "Harness.h"
#include "LoopbackPaneLink.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using UbiquittyTest::LoopbackOptions;
using UbiquittyTest::LoopbackPaneLink;
using UbiquittyTest::PTYRead;

namespace {

// A short interactive session: a prompt, an echoed command, its output scrolling
// rows into history, and a full-screen repaint on the alternate screen.
std::vector<std::string> sessionChunks()
{
    std::vector<std::string> chunks;
    chunks.emplace_back("\x1b]133;A\x07$ \x1b]133;B\x07");
    for (const char key : std::string("ls -l"))
        chunks.emplace_back(1, key);
    chunks.emplace_back("\r\n\x1b]133;C\x07");
    std::string listing;
    for (int line = 0; line < 40; ++line)
        listing += "-rw-r--r--  1 user  staff  " + std::to_string(1000 + line * 37) + " file" + std::to_string(line) + ".txt\r\n";
    chunks.push_back(listing);
    chunks.emplace_back("\x1b]133;D;0\x07\x1b]133;A\x07$ \x1b]133;B\x07");
    chunks.emplace_back("\x1b[?1049h\x1b[H\x1b[2J\x1b[1;31mred\x1b[0m \x1b[4munderlined\x1b[0m");
    chunks.emplace_back("\x1b[5;10Hmoved\x1b[?1049l");
    return chunks;
}

std::vector<PTYRead> readsEvery(const std::vector<std::string>& chunks, std::uint64_t intervalMicroseconds)
{
    std::vector<PTYRead> reads;
    std::uint64_t at = 0;
    for (const std::string& chunk : chunks) {
        reads.push_back({ at, chunk, true });
        at += intervalMicroseconds;
    }
    return reads;
}

// A ZWJ sequence and an OSC 8 link, which ride the cluster pool and the link
// table beside the cells that name them.
std::vector<std::string> clusterAndLinkChunks()
{
    return {
        "family \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 here",
        "\r\n\x1b]8;;https://example.com/right\x1b\\a link\x1b]8;;\x1b\\",
        "\r\nafter",
    };
}

} // namespace

TEST(aFaithfulStreamKeepsTheClientOnTheHost)
{
    const std::vector<std::string> chunks = sessionChunks();
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 12, .columns = 60 });
    link.replay(reads);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
    // A replay that published one frame would pass by never comparing an
    // update; and after the attach checkpoint every frame here is one.
    CHECK(link.frames().size() >= 8);
    for (const UbiquittyTest::PublishedFrame& frame : link.frames())
        CHECK_MSG(!frame.isCheckpoint, "a frame of an ordinary session went as a checkpoint");
    CHECK(Ubiquitty::Screen::scrollbackCount(&link.host()) > 0);
}

// The mutation that proves the oracle: one cell of the decoded client grid is
// flipped after a chosen frame applies. The oracle must report that frame and
// that cell, not a later one and not a generic mismatch.
TEST(aFlippedDecodedCellIsReportedAtItsFrameAndCell)
{
    const std::vector<std::string> chunks = sessionChunks();
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 12, .columns = 60 });
    int applied = 0;
    link.afterClientApply = [&applied](Ubiquitty::Terminal& client) {
        if (++applied != 4)
            return;
        Ubiquitty::Cell flipped = *client.grid->cellAt(Ubiquitty::Row(2), Ubiquitty::Column(5));
        flipped.codePoint = flipped.codePoint == 'Z' ? 'Y' : 'Z';
        client.grid->setCell(Ubiquitty::Row(2), Ubiquitty::Column(5), flipped);
    };
    link.replay(reads);
    const std::string& failure = link.failure();
    CHECK_MSG(!failure.empty(), "a flipped client cell went unreported");
    CHECK_MSG(failure.find("frame 3 ") == 0, failure.c_str());
    CHECK_MSG(failure.find("grid row 2 column 5") != std::string::npos, failure.c_str());
}

// History is compared too: a client whose mirror lost a line is a client whose
// scrollback disagrees, even when the visible screen matches.
TEST(aDroppedHistoryLineIsReported)
{
    const std::vector<std::string> chunks = sessionChunks();
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 12, .columns = 60 });
    bool didDrop = false;
    link.afterClientApply = [&didDrop](Ubiquitty::Terminal& client) {
        if (didDrop || Ubiquitty::Screen::scrollbackCount(&client) == 0)
            return;
        Ubiquitty::Screen::resetScrollbackStore(&client);
        didDrop = true;
    };
    link.replay(reads);
    CHECK(didDrop);
    CHECK_MSG(link.failure().find("history") != std::string::npos, link.failure().c_str());
}

// A synchronized-output block that spans a would-block is the daemon's to hold:
// no frame is published while it is open, and the composed frame goes out once
// it closes.
TEST(aSynchronizedBlockAcrossReadsPublishesOnlyTheComposedFrame)
{
    const std::vector<std::string> chunks = {
        "before",
        "\x1b[?2026h\x1b[H\x1b[2Jhalf",
        " drawn\x1b[?2026l",
    };
    const std::vector<PTYRead> reads = readsEvery(chunks, 30'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 4, .columns = 20 });
    std::vector<std::string> published;
    link.onFrame = [&published](const UbiquittyTest::PublishedFrame&, const LoopbackPaneLink& current) {
        std::string row;
        for (int c = 0; c < current.host().columns; ++c) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&current.host(), 0, 0, c);
            row.push_back(cell && cell->codePoint ? static_cast<char>(cell->codePoint) : ' ');
        }
        published.push_back(row);
    };
    link.replay(reads);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
    for (const std::string& row : published)
        CHECK_MSG(row.rfind("half ", 0) != 0 || row.rfind("half drawn", 0) == 0, row.c_str());
    CHECK(!published.empty() && published.back().rfind("half drawn", 0) == 0);
}

// Equal cells can still draw different things: a cluster id resolves through
// the client's pool, a link id through its link table. Each is corrupted on the
// client after the frame that carried it, and the oracle must name it.
TEST(aClientClusterPoolThatLostAnEntryIsReported)
{
    const std::vector<std::string> chunks = clusterAndLinkChunks();
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink faithful(LoopbackOptions { .rows = 6, .columns = 40 });
    faithful.replay(reads);
    CHECK_MSG(faithful.failure().empty(), faithful.failure().c_str());

    LoopbackPaneLink link(LoopbackOptions { .rows = 6, .columns = 40 });
    link.afterClientApply = [](Ubiquitty::Terminal& client) { client.clusterPool.clear(); };
    link.replay(reads);
    CHECK_MSG(link.failure().find("frame 0 ") == 0 && link.failure().find("cluster") != std::string::npos, link.failure().c_str());
}

TEST(aClientLinkTableWithTheWrongURIIsReported)
{
    const std::vector<std::string> chunks = clusterAndLinkChunks();
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 6, .columns = 40 });
    link.afterClientApply = [](Ubiquitty::Terminal& client) {
        Ubiquitty::Attributes::Hyperlinks::LinkTable* table = Ubiquitty::Attributes::Hyperlinks::table(&client);
        if (!table)
            return;
        for (auto& uri : table->uris) {
            if (std::string_view(uri.data()) == "https://example.com/right")
                std::snprintf(uri.data(), uri.size(), "%s", "https://example.com/wrong");
        }
    };
    link.replay(reads);
    CHECK_MSG(link.failure().find("frame 1 ") == 0 && link.failure().find("https://example.com/wrong") != std::string::npos, link.failure().c_str());
}

// What a program sets beside its picture rides the pane's records: a user
// variable, progress, an indicator, a badge, a bell nobody has seen. The
// faithful replay proves the records carry each into the replica; a client
// that drops one is named by the member, at the frame that carried it.
TEST(theOracleComparesUserVariables)
{
    const std::vector<std::string> chunks {
        "plain output\r\n",
        "\033]1337;SetUserVar=k=dg==\007set\r\n",
        "\033]9;4;1;40\007\033]21337;indicator=#102030;status=busy\007\033]1337;SetBadgeFormat=cHJvZA==\007more\r\n",
        "\007\033]777;notify;Deploy;went out\007rung\r\n",
        "\033]1337;SetUserVar=k=\007\033]9;4;0\007cleared\r\n",
    };
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink faithful(LoopbackOptions { .rows = 6, .columns = 40 });
    std::vector<std::string> values;
    std::vector<int> bells;
    faithful.onFrame = [&](const UbiquittyTest::PublishedFrame&, const LoopbackPaneLink& link) {
        const Ubiquitty::Protocol::Sync::PaneFacts* facts = link.clientEnd().paneFacts(link.handle());
        values.push_back(facts && !facts->userVariables.empty() ? facts->userVariables[0].name + "=" + facts->userVariables[0].value : std::string());
        bells.push_back(Ubiquitty::unseenAttention(&link.client()).hasBell ? 1 : 0);
    };
    faithful.replay(reads);
    CHECK_MSG(faithful.failure().empty(), faithful.failure().c_str());
    // Set at the frame that carried it, held after, and gone when cleared.
    const auto firstSet = std::ranges::find(values, std::string("k=v"));
    const auto firstRung = std::ranges::find(bells, 1);
    REQUIRE(firstSet != values.end() && firstRung != bells.end());
    CHECK(values.front().empty() && values.back().empty());
    CHECK(firstSet < values.end() - 1 && *(firstSet + 1) == "k=v");
    CHECK(bells.back() == 1);
    CHECK(Ubiquitty::badgeText(&faithful.client()) == "prod");
    CHECK_EQ(static_cast<int>(faithful.client().progressState), 0);

    const std::string setFrame = "frame " + std::to_string(firstSet - values.begin()) + " ";
    LoopbackPaneLink link(LoopbackOptions { .rows = 6, .columns = 40 });
    link.afterClientApply = [](Ubiquitty::Terminal& client) { client.userVariables = {}; };
    link.replay(reads);
    CHECK_MSG(link.failure().find(setFrame) == 0 && link.failure().find("userVariables") != std::string::npos, link.failure().c_str());

    const std::string rungFrame = "frame " + std::to_string(firstRung - bells.begin()) + " ";
    LoopbackPaneLink deaf(LoopbackOptions { .rows = 6, .columns = 40 });
    deaf.afterClientApply = [](Ubiquitty::Terminal& client) { client.hasUnseenBell = false; };
    deaf.replay(reads);
    CHECK_MSG(deaf.failure().find(rungFrame) == 0 && deaf.failure().find("hasUnseenBell") != std::string::npos, deaf.failure().c_str());
}

// Screen-wide state rides beside the cells: DECSCNM, DECSCUSR and bracketed
// paste ship in an update. The faithful replay proves the wire
// carries them; each mutation proves the oracle would see a client that
// dropped one.
TEST(screenWideStateTheClientDroppedIsReported)
{
    const std::vector<std::string> chunks = {
        "prompt$ ",
        "\x1b[?5h\x1b[6 q\x1b[?2004h",
        "typed",
    };
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink faithful(LoopbackOptions { .rows = 4, .columns = 20 });
    faithful.replay(reads);
    CHECK_MSG(faithful.failure().empty(), faithful.failure().c_str());
    CHECK(faithful.frames().size() == chunks.size() && !faithful.frames()[1].isCheckpoint);

    const auto failureWith = [&reads](const std::function<void(Ubiquitty::Terminal&)>& drop) {
        LoopbackPaneLink link(LoopbackOptions { .rows = 4, .columns = 20 });
        int applied = 0;
        link.afterClientApply = [&applied, &drop](Ubiquitty::Terminal& client) {
            if (++applied == 2)
                drop(client);
        };
        link.replay(reads);
        return link.failure();
    };
    const std::string reverseVideo = failureWith([](Ubiquitty::Terminal& client) { client.modes.reverseVideo = false; });
    CHECK_MSG(reverseVideo.find("frame 1 ") == 0 && reverseVideo.find("reverse video") != std::string::npos, reverseVideo.c_str());
    const std::string cursorStyle = failureWith([](Ubiquitty::Terminal& client) { client.cursorStyle = Ubiquitty::Cursor::Style::FollowUserPreference; });
    CHECK_MSG(cursorStyle.find("frame 1 ") == 0 && cursorStyle.find("cursor style") != std::string::npos, cursorStyle.c_str());
    const std::string bracketedPaste = failureWith([](Ubiquitty::Terminal& client) { client.modes.bracketedPaste = false; });
    CHECK_MSG(bracketedPaste.find("frame 1 ") == 0 && bracketedPaste.find("bracketed paste") != std::string::npos, bracketedPaste.c_str());
}

// A row carries its line attribute, so a row switched back to single width
// in place is an update like any other edit: the client must not keep the
// double width it held.
TEST(aRowBackToSingleWidthInPlaceShipsAsAnUpdate)
{
    const std::vector<std::string> chunks = {
        "\x1b[3;1Hwide row",
        "\x1b[3;1H\x1b#6",
        "\x1b[1;1Htyped",
        "\x1b[3;1H\x1b#5",
        "\x1b[1;1Hmore",
    };
    const std::vector<PTYRead> reads = readsEvery(chunks, 40'000);
    LoopbackPaneLink link(LoopbackOptions { .rows = 6, .columns = 30 });
    link.replay(reads);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
    CHECK(link.frames().size() == chunks.size());
    for (const UbiquittyTest::PublishedFrame& frame : link.frames())
        CHECK(!frame.isCheckpoint);
}

UBIQUITTY_TEST_MAIN()
