// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_abort.cpp — the host abandons a checkpoint the client no longer
// wants, and says so (Docs/sync.md §6.5, `BulkAbort`).
//
// A checkpoint is a transfer, and the latest want for a
// pane wins: a `Resync` that asks for a newer one, a `View` that stops showing
// the pane and an unbind each make the body in transfer worthless. The host
// drops its unsent chunks and sends `BulkAbort{transfer}` on the interactive
// lane, behind the head that announced it, so the client frees the partial
// body rather than holding it until the connection ends. The properties held:
//
// - A pane leaving `Live` mid-transfer aborts the body; shown again, it is
//   sent a fresh checkpoint and converges.
// - A `Resync` mid-transfer aborts the old body for a new one.
// - An unbind mid-transfer aborts it, and the client holds no transfer after.
// - A `BulkAbort` naming a transfer never announced closes the connection
//   `Malformed`, and one from the client is a frame the host does not accept.
// - An unannounced, unfenced abort closes `Malformed`: the client takes an
//   abort while a head announces its transfer or its own fence holds it, and
//   refuses the same transfer's abort once neither does.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// Draws a screen large enough that the pane's checkpoint takes several chunks.
///
/// - Parameter link: The link.
/// - Returns: The pane, bound and its checkpoint not yet sent.
std::uint32_t bindWithLargeScreen(SyncLink& link)
{
    const std::uint32_t pane = link.bindPane(200, 300, "");
    link.write(pane, UbiquittyTest::colourfulScreen(200, 300));
    return pane;
}

/// Steps until the pane's checkpoint has begun to leave and not finished.
///
/// - Parameters:
///   - link: The link.
///   - pane: The pane.
void stepIntoTheTransfer(SyncLink& link, std::uint32_t pane)
{
    for (int step = 0; step < 50 && link.host().counters().checkpointsSent == 0; ++step)
        link.step();
    REQUIRE(link.host().counters().checkpointsSent > 0);
    REQUIRE(link.client().paneGeneration(pane) == 0);
}

/// Runs the link until the client holds the pane's newest generation.
///
/// - Parameters:
///   - link: The link.
///   - pane: The pane.
void converge(SyncLink& link, std::uint32_t pane)
{
    for (int step = 0; step < 2'000 && link.held(pane) != link.newest(pane); ++step)
        link.step();
}

} // namespace

// The pane goes Dormant while its checkpoint crosses: the host aborts the
// body, and the client frees it. Shown again, the pane is sent a fresh
// checkpoint and matches the host.
TEST(aPaneLeavingLiveAbortsItsCheckpoint)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = bindWithLargeScreen(link);
    stepIntoTheTransfer(link, pane);
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    link.step();
    CHECK_EQ(link.host().counters().bulkAborts, std::uint64_t { 1 });
    CHECK_EQ(link.client().counters().bulkAbortsReceived, std::uint64_t { 1 });
    CHECK_EQ(link.client().transfersHeld(), std::size_t { 0 });
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });

    link.client().setView(pane, ViewState::Live);
    converge(link, pane);
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 2 });
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// A Resync while the checkpoint crosses: the latest want wins, the old body
// is aborted and a new one sent at once.
TEST(aResyncSupersedesTheCheckpointInTransfer)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = bindWithLargeScreen(link);
    stepIntoTheTransfer(link, pane);
    std::vector<std::uint8_t> body;
    encode(body, Resync { .reason = ResyncReason::DecodeFailed, .generation = 0, .sectionHashes = {} });
    link.sendToHost(FrameKind::Resync, body, pane);
    converge(link, pane);
    CHECK_EQ(link.host().counters().bulkAborts, std::uint64_t { 1 });
    CHECK_EQ(link.client().counters().bulkAbortsReceived, std::uint64_t { 1 });
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 2 });
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// An unbind while the checkpoint crosses: the client is told the transfer is
// abandoned before the pane goes, and holds nothing of it after.
TEST(anUnboundPaneAbortsItsTransferAndTheClientFreesIt)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = bindWithLargeScreen(link);
    stepIntoTheTransfer(link, pane);
    link.unbind(pane);
    link.step();
    link.step();
    CHECK_EQ(link.host().counters().bulkAborts, std::uint64_t { 1 });
    CHECK_EQ(link.client().transfersHeld(), std::size_t { 0 });
    CHECK(link.client().state() == ConnectionState::Attached);
}

// A BulkAbort naming a transfer the host never announced is a host speaking
// out of turn.
TEST(aBulkAbortForATransferNeverAnnouncedCloses)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    std::vector<std::uint8_t> body;
    encode(body, BulkAbort { .transfer = 999 });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::BulkAbort, 0, body));
    link.deliverToClient(frame);
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);
}

// The rule the client holds a host's BulkAbort to: one naming a transfer that
// no head announced and no fence of the client's holds closes the connection
// Malformed. One transfer's abort is taken while its head has announced it,
// which abandons and fences it; taken again while that fence holds, which
// releases it; and refused the third time, when neither holds.
TEST(anUnannouncedUnfencedAbortClosesMalformed)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::optional<std::uint32_t> transfer;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        FrameDecoder decoder;
        decoder.append(stream);
        while (const std::optional<Frame> frame = decoder.next()) {
            if (frame->kind != FrameKind::CheckpointHead || transfer)
                continue;
            if (const std::optional<CheckpointHead> head = decodeCheckpointHead(frame->body))
                transfer = head->transfer;
        }
    });
    const std::uint32_t pane = bindWithLargeScreen(link);
    stepIntoTheTransfer(link, pane);
    REQUIRE(transfer.has_value());
    const std::size_t heldBefore = link.client().transfersHeld();
    REQUIRE(heldBefore > 0);
    // The host says nothing more; every abort from here is the test's.
    link.holdDownstream();
    const auto deliverAbort = [&link](std::uint32_t named) {
        std::vector<std::uint8_t> body;
        encode(body, BulkAbort { .transfer = named });
        std::vector<std::uint8_t> frame;
        CHECK(appendFrame(frame, FrameKind::BulkAbort, 0, body));
        link.deliverToClient(frame);
    };

    deliverAbort(*transfer);
    CHECK_MSG(link.client().state() == ConnectionState::Attached, "an abort of a transfer a head announced closed the connection");
    CHECK_EQ(link.client().transfersHeld(), heldBefore - 1);

    deliverAbort(*transfer);
    CHECK_MSG(link.client().state() == ConnectionState::Attached, "an abort of a transfer the client's fence holds closed the connection");

    deliverAbort(*transfer);
    CHECK_MSG(link.client().state() == ConnectionState::Closed, "an abort of a transfer neither announced nor fenced was taken");
    CHECK(link.client().byeReason() == ByeReason::Malformed);
}

// A BulkAbort is the host's to send.
TEST(aBulkAbortFromTheClientCloses)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    std::vector<std::uint8_t> body;
    encode(body, BulkAbort { .transfer = 1 });
    link.sendToHost(FrameKind::BulkAbort, body);
    CHECK(link.host().state() == ConnectionState::Closed);
}

UBIQUITTY_TEST_MAIN()
