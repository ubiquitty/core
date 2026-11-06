// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_owed.cpp — one window across every frame a connection sends, and
// the order owed panes are served in when it opens (Docs/sync.md §6.4).
//
// The properties held:
//
// - A pane holding input is served first when the window opens, so a pane
//   flooding beside it never delays its echo.
// - Panes that all flood share the window by the bytes each is sent.
// - Two panes changed in one pass both leave in that service.
// - A Result larger than one frame is paced as
//   bulk, so a keystroke echo behind it waits about one capped frame's time on
//   the wire, not the whole result's.
// - A suspended reader with ten panes updating their records holds at most
//   one window of unacknowledged bytes, and owes each pane only its newest
//   record of each kind; once it reads again it is sent the newest.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// A full-screen redraw of `rows` rows of `columns` columns, every cell
/// changed from the frame before.
///
/// - Parameters:
///   - index: The frame's number.
///   - rows: How many rows to redraw from the top.
///   - columns: How many columns each row fills.
/// - Returns: The bytes a program writes.
std::string redraw(int index, int rows, int columns)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row) {
        bytes += "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int column = 0; column < columns; ++column)
            bytes += static_cast<char>('a' + (index * 7 + row * 3 + column) % 26);
    }
    return bytes;
}

/// The bytes a frame takes once the window refuses nothing smaller.
constexpr std::uint64_t cappedFrameBytes = maxEnvelopeHeaderBytes + maxFrameBodyBytes;

} // namespace

// The window is full of a flooding pane's updates when the client types into
// the pane beside it. When the next acknowledgement opens the window, the
// echo is what leaves first, and the client holds it after that one opening.
TEST(aFloodingPaneNeverDelaysTheEchoBesideIt)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(50, 160));
    const std::uint32_t flooding = link.addPane(50, 160, "");
    const std::uint32_t typing = link.addPane(24, 80, "$ ");
    link.holdUpstream(FrameKind::Ack);
    int frame = 0;
    for (; frame < 200 && link.host().counters().windowRefusals == 0; ++frame) {
        link.write(flooding, redraw(frame, 50, 160));
        link.step();
    }
    REQUIRE(link.host().counters().windowRefusals > 0);

    // The program in the typing pane echoes a key, while the flood goes on.
    link.source(typing).noteInputWritten();
    link.write(typing, "x");
    link.write(flooding, redraw(++frame, 50, 160));
    std::vector<std::uint32_t> updates;
    link.onHostFrame([&](FrameKind kind, std::uint32_t channel, Carrier, std::size_t) {
        if (kind == FrameKind::Update)
            updates.push_back(channel);
    });
    link.releaseUpstream();
    link.step();
    REQUIRE(!updates.empty());
    CHECK_EQ(updates.front(), typing);
    CHECK_EQ(link.held(typing), link.newest(typing));
    const std::string difference = UbiquittyTest::firstDisagreement(link.hostTerminal(typing), link.clientTerminal(typing));
    CHECK_MSG(difference.empty(), difference.c_str());
}

// Three panes flood at different sizes over a link that cannot carry them
// all. Each is sent about the same bytes: the deficit round-robin keeps any
// two within two quanta of each other, where serving them in bind order, or
// whichever fits the room an acknowledgement makes, starves one.
TEST(owedPanesShareTheWindowByEncodedBytes)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(50, 160));
    const std::uint32_t whole = link.addPane(50, 160, "");
    const std::uint32_t half = link.addPane(50, 160, "");
    const std::uint32_t quarter = link.addPane(50, 160, "");
    link.shape({ .bytesPerSecond = 125'000, .oneWayDelayMicroseconds = 20'000 });
    std::map<std::uint32_t, std::uint64_t> sent;
    link.onHostFrame([&](FrameKind kind, std::uint32_t channel, Carrier, std::size_t bytes) {
        if (kind == FrameKind::Update)
            sent[channel] += bytes;
    });
    for (int frame = 0; frame < 600; ++frame) {
        link.write(whole, redraw(frame, 50, 160));
        link.write(half, redraw(frame, 25, 160));
        link.write(quarter, redraw(frame, 12, 160));
        link.step();
    }
    const std::uint64_t most = std::max({ sent[whole], sent[half], sent[quarter] });
    const std::uint64_t least = std::min({ sent[whole], sent[half], sent[quarter] });
    CHECK_MSG(most - least <= 2 * cappedFrameBytes,
        ("sent " + std::to_string(sent[whole]) + ", " + std::to_string(sent[half]) + " and " + std::to_string(sent[quarter]) + " bytes").c_str());
    CHECK(least > 0);
}

// The stale-update guard is per pane: an update of one pane waiting for the
// transport holds back only that pane's next one, so two panes changed in one
// pass both leave in that pass's service.
TEST(twoPanesChangedInOnePassLeaveInOneService)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t first = link.addPane();
    const std::uint32_t second = link.addPane();
    link.write(first, "one");
    link.write(second, "two");
    link.serviceWithoutCarrying();
    std::vector<std::uint8_t> stream;
    while (link.host().popStreamFrame(stream, link.now())) {
    }
    FrameDecoder decoder;
    decoder.append(stream);
    std::vector<std::uint32_t> updated;
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind == FrameKind::Update)
            updated.push_back(frame->channel);
    }
    CHECK_EQ(updated.size(), std::size_t { 2 });
    link.deliverToClient(stream);
    CHECK_EQ(link.held(first), link.newest(first));
    CHECK_EQ(link.held(second), link.newest(second));
}

// A 5 MiB Result crosses the same bounded bulk lane as other typed transfers.
// A keystroke in another pane waits behind one capped chunk, not the result.
TEST(aLargeResultDoesNotHoldAKeystrokeEchoPastOneFrameTime)
{
    constexpr std::uint64_t bytesPerSecond = 125'000;
    constexpr std::uint64_t oneWayDelay = 30'000;
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), measuredHostPolicy(0));
    const std::uint32_t showing = link.addPane();
    const std::uint32_t typing = link.addPane();
    REQUIRE(link.client().call(R"({"jsonrpc":"2.0","id":17,"method":"pane.capture","params":{"pane":)" + std::to_string(showing) + "}}"));
    link.step();
    const auto calls = link.host().takeCalls();
    REQUIRE(calls.size() == 1 && calls.front().admission == CallAdmission::Admitted);
    const std::string result = R"({"jsonrpc":"2.0","id":17,"result":")" + std::string(5u << 20, 'r') + R"("})";
    REQUIRE(link.host().answerCall(calls.front().callIndex, result) == CallAnswer::Sent);
    link.shape({ .bytesPerSecond = bytesPerSecond, .oneWayDelayMicroseconds = oneWayDelay });
    link.advance(3'000'000);
    REQUIRE(link.client().takeResults().empty());
    const std::uint64_t typedAt = link.now();
    link.source(typing).noteInputWritten();
    link.write(typing, "x");
    std::uint64_t echoedAt = 0;
    for (int tick = 0; tick < 5'000 && echoedAt == 0; ++tick) {
        link.advance(1'000);
        if (link.held(typing) == link.newest(typing))
            echoedAt = link.now();
    }
    REQUIRE(echoedAt != 0);
    const std::uint64_t frameTime = cappedFrameBytes * 1'000'000 / bytesPerSecond;
    const std::uint64_t bound = frameTime + 2 * 2 * oneWayDelay + oneWayDelay + 2 * UbiquittyTest::syncFrameInterval;
    CHECK_MSG(echoedAt - typedAt <= bound,
        ("the echo took " + std::to_string(echoedAt - typedAt) + " us against " + std::to_string(bound)).c_str());
    std::vector<std::string> results;
    for (int tick = 0; tick < 120 && results.empty(); ++tick) {
        link.advance(1'000'000);
        results = link.client().takeResults();
    }
    REQUIRE(results.size() == 1);
    CHECK(results.front() == result);
}

// Ten panes continually replace both canonical record families while the
// client reads nothing. Retained output stays bounded independently of the
// number of revisions, then the newest facts and status reach every pane.
TEST(aSuspendedReaderWithTenPanesHoldsAtMostOneWindow)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint32_t> handles;
    for (int pane = 0; pane < 10; ++pane)
        handles.push_back(link.addPane());
    link.settleAttach();
    link.holdDownstream();
    std::size_t largestRecord = 0;
    for (int frame = 0; frame < 600; ++frame) {
        for (const std::uint32_t handle : handles) {
            const auto title = "pane " + std::to_string(handle) + " frame " + std::to_string(frame);
            auto facts = link.source(handle).facts();
            facts.programTitle = title;
            facts.currentDirectory = "/parts/" + std::to_string(frame);
            link.source(handle).setFacts(std::move(facts));
            link.write(handle, "\r" + std::to_string(frame) + "\033]7501;id=job:state=working:progress=" + std::to_string(frame % 100) + "\033\\");
            link.source(handle).setStatus(statusRecordsOf(link.hostTerminal(handle)));
            std::vector<std::uint8_t> factsBody;
            encodePaneFacts(factsBody, link.source(handle).facts());
            std::vector<std::uint8_t> statusBody;
            CHECK_EQ(encodeStatusFragment(statusBody, link.source(handle).status(), 0), 1u);
            largestRecord = std::max({ largestRecord, factsBody.size(), statusBody.size() });
        }
        link.step();
        const auto window = link.host().pacer().windowBytes(link.now());
        CHECK_MSG(link.host().pacer().outstandingBytes() <= window,
            ("frame " + std::to_string(frame) + ": " + std::to_string(link.host().pacer().outstandingBytes()) + " bytes outstanding").c_str());
        CHECK_MSG(link.host().retainedOutboundPayloadBytes() <= window + handles.size() * 2 * (largestRecord + maxEnvelopeHeaderBytes),
            "queued typed records accumulated old revisions");
    }
    link.releaseDownstream();
    for (int step = 0; step < 400; ++step)
        link.step();
    for (const std::uint32_t handle : handles) {
        const auto* facts = link.client().paneFacts(handle);
        REQUIRE(facts);
        CHECK(facts->programTitle == "pane " + std::to_string(handle) + " frame 599");
        CHECK(facts->currentDirectory == "/parts/599");
        const auto status = link.client().paneStatus(handle);
        REQUIRE(status);
        CHECK(status->store == link.source(handle).status().store);
        CHECK_EQ(link.held(handle), link.newest(handle));
    }
}

UBIQUITTY_TEST_MAIN()
