// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_lag.cpp — how far behind the picture an update delivers is
// (Docs/sync.md §7.4, `Update.lagMicros`).
//
// `lagMicros` is the host time an update leaves less the time the oldest
// change it delivers was made: `hostTime − dirtySince(baseGeneration + 1)`. A
// client that holds acknowledgements back, or a slow link, makes it grow; a
// program that is merely thinking does not. The properties held:
//
// - An update from a base two generations back reports the age of the first
//   of them, not of the newest: a Dormant pane shown again reports how long
//   its picture waited.
// - A base older than the host's ring reads the ring's oldest stamp.
// - On a shaped link the lag of every update stays within the injected delay
//   and one frame.
// - The header's fields round-trip, the lag as a varint of any width, and a
//   lag in an overlong varint is refused.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Wire.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

// A pane the client has Dormant changes at T0 and again 20 ms later, each
// published as a generation of its own. Shown 300 ms after T0, it is sent one
// update that delivers both, and its lag is the age of the first change, not
// of the newest.
TEST(lagIsTheAgeOfTheOldestChangeTheUpdateDelivers)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane();
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    const std::uint64_t heldBefore = link.held(pane);
    const std::uint64_t firstChange = link.now();
    link.write(pane, "\x1b[1;1Hfirst change");
    link.advance(20'000);
    link.write(pane, "\x1b[2;1Hsecond change");
    link.advance(280'000);
    REQUIRE(link.newest(pane) == heldBefore + 2);
    link.client().setView(pane, ViewState::Live);
    link.pump();
    const std::uint64_t cutAt = link.now() + UbiquittyTest::syncFrameInterval;
    link.step();
    REQUIRE(link.held(pane) == link.newest(pane));
    CHECK_EQ(link.client().paneLagMicroseconds(pane), cutAt - firstChange);
}

// A base the ring no longer holds reads the ring's oldest stamp: the lag is
// still an age the update delivers, never a guess past it.
TEST(aBaseOlderThanTheRingReadsTheOldestStamp)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 20);
    PaneSource source(terminal, ID128 { 1, 2 });
    for (std::uint64_t generation = 1; generation <= 200; ++generation)
        source.publish(FrameClass::Settled, 1'000 * generation);
    CHECK_EQ(source.dirtySince(200), std::uint64_t { 200'000 });
    CHECK_EQ(source.dirtySince(150), std::uint64_t { 150'000 });
    CHECK_EQ(source.dirtySince(1), source.dirtySince(200 - paneDirtyRingCapacity + 1));
    CHECK_EQ(source.dirtySince(200 - paneDirtyRingCapacity + 1), std::uint64_t { (200 - paneDirtyRingCapacity + 1) * 1'000 });
}

// Over a link of 1 Mbit/s and 60 ms each way, the lag says how long the host
// held a picture back, not how long the wire took. A pane that changes a line
// a frame is never held: its lag stays within one frame. A pane rewriting its
// whole screen every frame asks for more than the link carries, so the window
// holds it, and its lag grows past the frame while staying within the
// window's drain time and a round trip.
TEST(lagStaysWithinTheInjectedDelayPlusOneFrame)
{
    constexpr std::uint64_t oneWayDelay = 60'000;
    constexpr std::uint64_t bytesPerSecond = 125'000;
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        const std::uint32_t pane = link.addPane();
        link.shape({ .bytesPerSecond = bytesPerSecond, .oneWayDelayMicroseconds = oneWayDelay });
        std::uint64_t worst = 0;
        for (int frame = 0; frame < 300; ++frame) {
            link.write(pane, "\x1b[5;1Hframe " + std::to_string(frame));
            link.step();
            worst = std::max(worst, link.client().paneLagMicroseconds(pane));
        }
        CHECK_MSG(worst <= UbiquittyTest::syncFrameInterval, ("a line a frame lagged " + std::to_string(worst) + " us").c_str());
    }
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        const std::uint32_t pane = link.addPane();
        link.shape({ .bytesPerSecond = bytesPerSecond, .oneWayDelayMicroseconds = oneWayDelay });
        std::uint64_t worst = 0;
        std::uint64_t widestWindow = 0;
        for (int frame = 0; frame < 300; ++frame) {
            std::string screen = "\x1b[H";
            // Every row changes, and each names itself, so no row is ever
            // another's earlier frame and no scroll can say the frame in less
            // than its rows.
            for (int row = 0; row < 24; ++row) {
                const std::string label = std::to_string(100 + row);
                screen += label + std::string(80 - label.size(), static_cast<char>('A' + (frame + row) % 26));
            }
            link.write(pane, screen);
            link.step();
            worst = std::max(worst, link.client().paneLagMicroseconds(pane));
            widestWindow = std::max(widestWindow, link.host().pacer().windowBytes(link.now()));
        }
        const std::uint64_t bound = widestWindow * 1'000'000 / bytesPerSecond + 2 * oneWayDelay + 2 * UbiquittyTest::syncFrameInterval;
        CHECK_MSG(worst > 2 * UbiquittyTest::syncFrameInterval && worst <= bound, ("a flood lagged " + std::to_string(worst) + " us against " + std::to_string(bound)).c_str());
    }
}

// The header's fields round-trip, the lag as a varint at each width boundary.
TEST(theUpdateHeaderRoundTripsAndRefusesAnOverlongLag)
{
    for (const std::uint64_t lag : { std::uint64_t { 0 }, std::uint64_t { 127 }, std::uint64_t { 128 }, std::uint64_t { 1 } << 63 }) {
        UpdateHeader header { .mode = UpdateMode::Chain, .baseGeneration = 4, .generation = 5, .frameClass = FrameClass::Settled, .hostTime = 99, .lagMicroseconds = lag, .inputWritten = 7, .inputSettled = 6, .historyHead = 300, .effectsThrough = 5 };
        std::vector<std::uint8_t> body;
        encode(body, header);
        body.push_back(0xAB);
        const std::optional<DecodedUpdateHeader> decoded = decodeUpdateHeader(body);
        REQUIRE(decoded.has_value());
        CHECK_EQ(decoded->header.lagMicroseconds, lag);
        CHECK_EQ(decoded->header.historyHead, std::uint64_t { 300 });
        CHECK_EQ(decoded->header.inputWritten, std::uint64_t { 7 });
        CHECK_EQ(decoded->header.effectsThrough, std::uint64_t { 5 });
        CHECK_EQ(decoded->bytes, body.size() - 1);
    }
    // The lag in eleven bytes, where ten hold any u64.
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.u8(0);
    writer.u64(4);
    writer.u64(5);
    writer.u8(1);
    writer.u64(99);
    for (int byte = 0; byte < 10; ++byte)
        writer.u8(0x80);
    writer.u8(0);
    for (int field = 0; field < 4; ++field)
        writer.u64(0);
    CHECK(!decodeUpdateHeader(body).has_value());
}

UBIQUITTY_TEST_MAIN()
