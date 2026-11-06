// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_trace_report.cpp — the report a Sync Debug window draws
// (Session/SyncTraceReport.h), built in Core so every frontend shows the same
// numbers under the same names.
//
// The claims are the ones a frontend would otherwise make for itself and could
// get wrong: every counter the trace holds reaches the report, a wire tag
// arrives with its protocol name, the carrier's statistics appear only when
// the carrier supplied them, and the rings are copied oldest first and capped.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/SyncTrace.h"
#include "Core/Session/SyncTraceReport.h"

#include <cstdint>
#include <string>

using Ubiquitty::Protocol::Sync::frameKindName;
using Ubiquitty::Protocol::Sync::SyncTrace;
using Ubiquitty::Session::makeSyncTraceReport;
using Ubiquitty::Session::State;
using Ubiquitty::Session::syncTraceReportFrameLimit;
using Ubiquitty::Session::TransportMode;
using Ubiquitty::Transport::TransportStats;

static constexpr std::uint8_t inputType = std::to_underlying(Ubiquitty::Protocol::Sync::FrameKind::Raw);
static constexpr std::uint8_t updateType = std::to_underlying(Ubiquitty::Protocol::Sync::FrameKind::Update);
// A refused historical tag is still recorded, named numerically, not as TITLE.
static constexpr std::uint8_t titleType = 0x83;

// Every counter the trace keeps reaches the report unchanged, and the
// session facts handed in are the ones it carries.
TEST(aReportCarriesTheTracesCountersAndTheSessionsFacts)
{
    SyncTrace trace;
    trace.recordInbound(updateType, 100, 400);
    trace.recordInbound(titleType, 30, 30);
    trace.recordOutbound(inputType, 12);
    trace.recordResync();

    const auto report = makeSyncTraceReport(trace, nullptr, State::Connected, TransportMode::TCP, 7, 1'700'000'000'000'000);
    CHECK(report.state == State::Connected);
    CHECK_EQ(report.sessionID, 7u);
    CHECK_EQ(report.sampleUnixMicros, 1'700'000'000'000'000);
    CHECK_EQ(report.totals.inboundFrames, 2u);
    CHECK_EQ(report.totals.outboundFrames, 1u);
    CHECK_EQ(report.totals.inboundWireBytes, 130u);
    CHECK_EQ(report.totals.inboundLogicalBytes, 430u);
    CHECK_EQ(report.totals.outboundWireBytes, 12u);
    CHECK_EQ(report.totals.resyncs, 1u);
    CHECK_EQ(report.totals.largestInboundWireBytes, 100u);
}

// A wire tag arrives with the protocol's own name for it, on the per-type rows
// and on each frame, so no frontend keeps a table of names that can drift.
TEST(eachTypeRowAndFrameIsNamedByTheProtocol)
{
    SyncTrace trace;
    trace.recordInbound(titleType, 10, 10);
    trace.recordInbound(updateType, 50, 50);
    trace.recordOutbound(inputType, 5);

    const auto report = makeSyncTraceReport(trace, nullptr, State::Connected, TransportMode::TCP, 0, 0);
    CHECK_EQ(report.typeRows.size(), 3u);
    for (const auto& row : report.typeRows)
        CHECK_MSG(row.typeName == frameKindName(row.type), "a type row is named by the canonical kind");
    CHECK_MSG(!report.typeRows.front().isOutbound && report.typeRows.back().isOutbound,
        "inbound rows come before outbound ones");

    CHECK_EQ(report.recentFrames.size(), 3u);
    CHECK_EQ(report.recentFrames.front().type, titleType);
    CHECK_EQ(report.recentFrames.back().type, inputType);
    for (const auto& frame : report.recentFrames)
        CHECK_MSG(frame.typeName == frameKindName(frame.type), "a frame is named by the canonical kind");
}

TEST(canonicalTraceNamesDoNotReuseLegacyTagsOrMutableScratch)
{
    CHECK(std::string(frameKindName(1)) == "Greet");
    CHECK(std::string(frameKindName(2)) == "Welcome");
    CHECK(std::string(frameKindName(33)) == "Update");
    CHECK(std::string(frameKindName(69)) == "Raw");
    const auto* retiredCatalog = frameKindName(35);
    const auto* retiredTitle = frameKindName(0x83);
    CHECK(std::string(retiredCatalog) == "0x23");
    CHECK(std::string(retiredTitle) == "0x83");
    for (unsigned value = 0; value < 256; ++value)
        CHECK(frameKindName(static_cast<std::uint8_t>(value))[0]);
    CHECK(std::string(retiredCatalog) == "0x23");
    CHECK(std::string(retiredTitle) == "0x83");
}

// The carrier's statistics appear only when it supplied valid ones; ssh-stdio
// supplies none, and a report must not show a table of zeros as if QUIC had
// measured them.
TEST(transportStatisticsAppearOnlyWhenTheCarrierSuppliedThem)
{
    SyncTrace trace;
    const auto overSSH = makeSyncTraceReport(trace, nullptr, State::Connected, TransportMode::TCP, 0, 0);
    CHECK(!overSSH.hasTransportStats);
    CHECK(overSSH.transportName == "ssh-stdio");

    TransportStats invalid;
    const auto unpopulated = makeSyncTraceReport(trace, &invalid, State::Connected, TransportMode::QUIC, 0, 0);
    CHECK_MSG(!unpopulated.hasTransportStats, "stats the carrier did not populate are not shown");

    TransportStats measured;
    measured.valid = true;
    measured.smoothedRttMicros = 12'000;
    measured.packetsSent = 40;
    measured.packetsLost = 2;
    const auto overQUIC = makeSyncTraceReport(trace, &measured, State::Connected, TransportMode::QUIC, 0, 0);
    CHECK(overQUIC.hasTransportStats);
    CHECK(overQUIC.transportName == "QUIC");
    CHECK_EQ(overQUIC.transport.smoothedRttMicros, 12'000);
    CHECK_EQ(overQUIC.transport.packetsLost, 2u);
}

// The frame log is capped at what the window shows, keeping the newest, and
// stays oldest first; events keep their text.
TEST(theFrameLogIsCappedToTheNewestAndEventsKeepTheirText)
{
    SyncTrace trace;
    for (std::size_t index = 0; index < syncTraceReportFrameLimit + 40; ++index)
        trace.recordOutbound(inputType, 1);
    trace.recordEvent("connected over ssh-stdio");

    const auto report = makeSyncTraceReport(trace, nullptr, State::Connected, TransportMode::TCP, 0, 0);
    CHECK_MSG(report.recentFrames.size() <= syncTraceReportFrameLimit, "the log is capped at the report limit");
    CHECK_MSG(!report.recentFrames.empty(), "the log keeps frames");
    CHECK_MSG(report.recentFrames.front().ordinal < report.recentFrames.back().ordinal, "the log is oldest first");
    CHECK_EQ(report.recentFrames.back().ordinal, static_cast<std::uint64_t>(syncTraceReportFrameLimit + 40));
    CHECK_EQ(report.events.size(), 1u);
    CHECK(report.events.front().text == "connected over ssh-stdio");
}

UBIQUITTY_TEST_MAIN()
