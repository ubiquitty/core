// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/Protocol/Sync/test_synctrace.cpp — the Sync client's live
// instrument (Core/Protocol/Sync/SyncTrace): per-(type,direction) histogram,
// running totals with the largest-frame watermarks, the echo-latency stopwatch
// and its percentiles, the frame + event rings (retention, oldest-first order,
// ordinal gaps as the overflow tell), and reset().
//
// Time-independence: every assertion is over counts, bytes, orderings and
// monotonic RELATIONS (a >= b, a latency within a slept-for band), never an
// absolute clock reading — so the test is deterministic on a loaded machine.

#include "Core/Protocol/Sync/SyncTrace.h"

#include <chrono>
#include <string>
#include <thread>

using namespace Ubiquitty::Protocol::Sync;

// Arbitrary historical tags for the histogram, which records refused frames too.
// 0x83 remains observable in diagnostics but is no longer a legal pane record.
static constexpr uint8_t pastePushType = 0x12;
static constexpr uint8_t syncCarrierType = 0x0E;
static constexpr uint8_t titleType = 0x83;

TEST(testEmptyState)
{
    SyncTrace trace;
    const SyncTrace::Totals totals = trace.totals();
    CHECK(totals.inboundFrames == 0 && totals.outboundFrames == 0);
    CHECK(totals.inboundWireBytes == 0 && totals.outboundWireBytes == 0);
    CHECK(totals.resyncs == 0);
    CHECK(totals.lastScreenAgeMicros == -1); // no screen yet
    CHECK(totals.uptimeMicros >= 0);

    const SyncTrace::LatencyStats latency = trace.latency();
    CHECK(latency.count == 0);
    CHECK(latency.lastMicros == -1 && latency.minMicros == -1);
    CHECK(latency.p50Micros == -1 && latency.p95Micros == -1 && latency.maxMicros == -1);

    CHECK(trace.perType().empty());
    CHECK(trace.recent(10).empty());
    CHECK(trace.events(10).empty());
}

TEST(testTotalsAndWatermarks)
{
    SyncTrace trace;
    trace.recordInbound(syncCarrierType, /*wireBytes*/ 100, /*logicalBytes*/ 400); // compressed 4:1
    trace.recordInbound(titleType, 30, 30);
    trace.recordScreenApplied();
    trace.recordOutbound(pastePushType, 12);
    trace.recordOutbound(pastePushType, 7);
    trace.recordResync();
    trace.recordResync();

    const SyncTrace::Totals totals = trace.totals();
    CHECK_EQ(totals.inboundFrames, 2u);
    CHECK_EQ(totals.outboundFrames, 2u);
    CHECK_EQ(totals.inboundWireBytes, 130u);
    CHECK_EQ(totals.inboundLogicalBytes, 430u); // the decompression win is visible
    CHECK_EQ(totals.outboundWireBytes, 19u);
    CHECK_EQ(totals.resyncs, 2u);
    // Watermarks are maxima, not last-writes: the smaller later frame doesn't lower them.
    CHECK_EQ(totals.largestInboundWireBytes, 100u);
    CHECK_EQ(totals.largestOutboundWireBytes, 12u);
    // The screen has changed, so its age is now a real (non-negative) reading.
    CHECK(totals.lastScreenAgeMicros >= 0);
}

TEST(testPerTypeRows)
{
    SyncTrace trace;
    trace.recordInbound(titleType, 10, 10);
    trace.recordInbound(titleType, 20, 25);
    trace.recordInbound(syncCarrierType, 50, 50);
    trace.recordOutbound(pastePushType, 5);

    const std::vector<SyncTrace::TypeRow> rows = trace.perType();
    // Only types seen at least once appear — not the whole 256-wide table.
    CHECK_EQ(rows.size(), 3u);
    // Stable order: inbound first, then outbound; ascending type within each.
    CHECK(!rows[0].outbound && rows[0].type == syncCarrierType); // 0x0E < 0x83
    CHECK(!rows[1].outbound && rows[1].type == titleType);
    CHECK(rows[2].outbound && rows[2].type == pastePushType);
    // The histogram aggregates repeats of one type.
    CHECK_EQ(rows[1].stat.count, 2u);
    CHECK_EQ(rows[1].stat.wireBytes, 30u);
    CHECK_EQ(rows[1].stat.logicalBytes, 35u);
    // An outbound row's logical bytes mirror its wire bytes (nothing to decompress).
    CHECK(rows[2].stat.wireBytes == 5 && rows[2].stat.logicalBytes == 5);
}

TEST(testEchoLatencyPairing)
{
    SyncTrace trace;
    // A screen change with no keystroke pending yields no sample.
    trace.recordScreenApplied();
    CHECK_EQ(trace.latency().count, 0u);

    // Input sent, then a screen change → one sample, at least the slept-for gap.
    trace.recordInputSent();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    trace.recordScreenApplied();
    SyncTrace::LatencyStats latency = trace.latency();
    CHECK_EQ(latency.count, 1u);
    CHECK(latency.lastMicros >= 2000); // the 2ms sleep is a floor, never an exact value
    // A single sample is simultaneously the min, both percentiles, and the max.
    CHECK_EQ(latency.minMicros, latency.lastMicros);
    CHECK_EQ(latency.p50Micros, latency.lastMicros);
    CHECK_EQ(latency.p95Micros, latency.lastMicros);
    CHECK_EQ(latency.maxMicros, latency.lastMicros);

    // The pending stopwatch is consumed: a second screen change adds no sample.
    trace.recordScreenApplied();
    CHECK_EQ(trace.latency().count, 1u);

    // Typing ahead measures from the oldest unanswered input, so a burst of
    // keystrokes on a slow link reports the full first-key->paint lag.
    trace.recordInputSent();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    trace.recordInputSent(); // must not restart the stopwatch
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    trace.recordScreenApplied();
    latency = trace.latency();
    CHECK_EQ(latency.count, 2u);
    CHECK(latency.lastMicros >= 4000); // both sleeps, i.e. measured from the FIRST key

    // An inbound frame does not close it, a Sync frame included: only a
    // screen the plane applied is the echo (the sample stays pending).
    const uint64_t beforeNonScreen = trace.latency().count;
    trace.recordInputSent();
    trace.recordInbound(syncCarrierType, 10, 10);
    trace.recordInbound(titleType, 10, 10);
    CHECK_EQ(trace.latency().count, beforeNonScreen);
    trace.recordScreenApplied(); // the still-pending stopwatch closes here
    CHECK_EQ(trace.latency().count, beforeNonScreen + 1);
}

TEST(testLatencyPercentiles)
{
    SyncTrace trace;
    // Drive 100 samples whose ordering is scrambled relative to insertion, so
    // the percentiles prove the sort rather than positional luck.
    for (int i = 0; i < 100; ++i) {
        trace.recordInputSent();
        std::this_thread::sleep_for(std::chrono::microseconds(((i * 37) % 100) * 10));
        trace.recordScreenApplied();
    }
    const SyncTrace::LatencyStats latency = trace.latency();
    CHECK_EQ(latency.count, 100u);
    // The ordering contract is what's deterministic; the values are wall-clock.
    CHECK(latency.minMicros <= latency.p50Micros);
    CHECK(latency.p50Micros <= latency.p95Micros);
    CHECK(latency.p95Micros <= latency.maxMicros);
    CHECK(latency.minMicros >= 0);
    CHECK(latency.lastMicros >= 0);
}

TEST(testLatencyRingSaturates)
{
    SyncTrace trace;
    // The sample window holds 120; drive well past it.
    for (int i = 0; i < 200; ++i) {
        trace.recordInputSent();
        trace.recordScreenApplied();
    }
    const SyncTrace::LatencyStats latency = trace.latency();
    CHECK_EQ(latency.count, 120u); // capped at the window, not 200
    CHECK(latency.minMicros <= latency.p50Micros && latency.p50Micros <= latency.maxMicros);
}

TEST(testFrameRingRetention)
{
    SyncTrace trace;
    for (int i = 0; i < 5; ++i)
        trace.recordOutbound(pastePushType, static_cast<std::size_t>(i + 1));

    // recent() clamps to what's retained and returns OLDEST first.
    std::vector<TracedFrame> recent = trace.recent(10);
    CHECK_EQ(recent.size(), 5u);
    CHECK_EQ(recent.front().ordinal, 1u);
    CHECK_EQ(recent.back().ordinal, 5u);
    CHECK(recent.front().wireBytes == 1 && recent.back().wireBytes == 5);
    CHECK(recent.back().outbound);
    CHECK(recent.back().monotonicMicros >= recent.front().monotonicMicros);
    CHECK(recent.back().unixMicros >= recent.front().unixMicros);

    // A smaller max returns the NEWEST tail, still oldest-first within it.
    recent = trace.recent(2);
    CHECK_EQ(recent.size(), 2u);
    CHECK(recent[0].ordinal == 4 && recent[1].ordinal == 5);
    CHECK(trace.recent(0).empty());

    // Past capacity (600) the ring drops the oldest; ordinals keep counting, so
    // a gap between 1 and the first retained ordinal is the overflow tell.
    for (int i = 0; i < 700; ++i)
        trace.recordInbound(syncCarrierType, 1, 1);
    recent = trace.recent(1000);
    CHECK_EQ(recent.size(), 600u); // capped at the ring capacity
    CHECK_EQ(recent.back().ordinal, 705u); // 5 outbound + 700 inbound
    CHECK_EQ(recent.front().ordinal, 706u - 600u);
    // Still contiguous and ascending across the wrap.
    for (std::size_t i = 1; i < recent.size(); ++i)
        CHECK(recent[i].ordinal == recent[i - 1].ordinal + 1);
}

TEST(testEventRing)
{
    SyncTrace trace;
    trace.recordEvent("connecting");
    trace.recordEvent("handshake ok");

    std::vector<TraceEvent> events = trace.events(10);
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].text == "connecting"); // oldest first
    CHECK(events[1].text == "handshake ok");
    CHECK(events[1].monotonicMicros >= events[0].monotonicMicros);
    CHECK(events[1].unixMicros > 0);

    events = trace.events(1);
    CHECK(events.size() == 1 && events[0].text == "handshake ok"); // newest tail
    CHECK(trace.events(0).empty());

    // The timeline is 64 deep; past that the oldest events fall off.
    for (int i = 0; i < 100; ++i)
        trace.recordEvent("event-" + std::to_string(i));
    events = trace.events(1000);
    CHECK_EQ(events.size(), 64u);
    CHECK(events.front().text == "event-36"); // 100 - 64
    CHECK(events.back().text == "event-99");
}

TEST(testReset)
{
    SyncTrace trace;
    trace.recordInbound(syncCarrierType, 100, 400);
    trace.recordOutbound(pastePushType, 12);
    trace.recordInputSent();
    trace.recordInbound(syncCarrierType, 10, 10);
    trace.recordScreenApplied(); // closes a latency sample
    trace.recordResync();
    trace.recordEvent("before reset");
    CHECK_EQ(trace.totals().inboundFrames, 2u);
    CHECK_EQ(trace.latency().count, 1u);

    trace.reset();

    const SyncTrace::Totals totals = trace.totals();
    CHECK(totals.inboundFrames == 0 && totals.outboundFrames == 0);
    CHECK(totals.inboundWireBytes == 0 && totals.inboundLogicalBytes == 0);
    CHECK(totals.outboundWireBytes == 0 && totals.resyncs == 0);
    CHECK(totals.largestInboundWireBytes == 0 && totals.largestOutboundWireBytes == 0);
    CHECK(totals.lastScreenAgeMicros == -1); // the screen clock is forgotten
    CHECK(trace.latency().count == 0 && trace.latency().lastMicros == -1);
    CHECK(trace.perType().empty());
    CHECK(trace.recent(10).empty());
    CHECK(trace.events(10).empty());

    // Ordinals restart from 1 — a reset trace is indistinguishable from a fresh one.
    trace.recordOutbound(pastePushType, 3);
    CHECK_EQ(trace.recent(1).front().ordinal, 1u);

    // A pending stopwatch does not survive the reset: the next screen change
    // must not report a latency spanning the reset boundary.
    trace.reset();
    trace.recordOutbound(pastePushType, 3);
    trace.reset();
    trace.recordScreenApplied();
    CHECK_EQ(trace.latency().count, 0u);
}

UBIQUITTY_TEST_MAIN()
