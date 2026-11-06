// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_view.cpp — the view state each attachment holds on each pane,
// which the client sets and the host enforces (Docs/sync.md §6.8).
//
// The properties held:
//
// - A `Dormant` pane is sent no update however much it changes: one
//   `PaneDirty` the first time it changes, then nothing; its Sync frames — its
//   typed title and status records — still arrive.
// - Setting it `Live` again sends one update of the rows that changed, diffed
//   from what the client was last sent, never a checkpoint.
// - An agent's pane starts `RecordsOnly`: no rows until it asks for them.
// - Eight panes bound with one `Live` cost one screen; the other seven nothing.
// - A `View` naming a pane never bound closes the connection, and so does one
//   from the host.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// Counts the frames of each kind the client is handed on one channel.
class Tally {
public:
    /// Taps a link.
    ///
    /// - Parameters:
    ///   - link: The link.
    ///   - channel: The pane counted.
    Tally(SyncLink& link, std::uint32_t channel)
    {
        link.onHostFrame([this, channel](FrameKind kind, std::uint32_t on, Carrier, std::size_t) {
            if (on == channel)
                ++m_counts[kind];
        });
    }

    /// How many frames of a kind arrived.
    ///
    /// - Parameter kind: The kind.
    /// - Returns: The count.
    [[nodiscard]] std::size_t operator[](FrameKind kind) const
    {
        const auto found = m_counts.find(kind);
        return found == m_counts.end() ? 0 : found->second;
    }

private:
    std::map<FrameKind, std::size_t> m_counts;
};

} // namespace

// A Dormant pane changes a thousand rows and is sent one PaneDirty and not
// one update, while its title still arrives; set Live, it is sent one update
// carrying only the rows that changed since what it was last sent.
TEST(aDormantPaneSendsOnePaneDirtyThenOneDiffWhenItWakes)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(24, 80));
    const std::uint32_t pane = link.addPane(24, 80, "$ ");
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    Tally tally(link, pane);
    const std::uint64_t checkpointsBefore = link.host().counters().checkpointsSent;
    for (int change = 0; change < 1'000; ++change) {
        // Rows 3 to 7 and nothing else.
        link.write(pane, "\x1b[" + std::to_string(3 + change % 5) + ";1Hrow " + std::to_string(change));
        if (change == 500) {
            PaneFacts facts;
            facts.programTitle = "dormant title";
            link.source(pane).setFacts(facts);
        }
        link.step();
    }
    CHECK_EQ(tally[FrameKind::Update], std::size_t { 0 });
    CHECK_EQ(tally[FrameKind::PaneDirty], std::size_t { 1 });
    CHECK_EQ(link.client().counters().paneDirtiesReceived, std::uint64_t { 1 });
    REQUIRE(link.client().paneFacts(pane));
    CHECK(link.client().paneFacts(pane)->programTitle == "dormant title");

    const std::uint64_t rowsBefore = link.host().counters().rowsSent;
    link.client().setView(pane, ViewState::Live);
    link.step();
    link.step();
    CHECK_EQ(tally[FrameKind::Update], std::size_t { 1 });
    CHECK_EQ(link.host().counters().rowsSent - rowsBefore, std::uint64_t { 5 });
    CHECK_EQ(link.host().counters().checkpointsSent, checkpointsBefore);
    CHECK_EQ(link.held(pane), link.newest(pane));
    const std::string difference = link.disagreement(pane);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(hidingOnePaneWithdrawsOnlyItsQueuedDatagram)
{
    SyncLink link(1200);
    const std::uint32_t hidden = link.addPane();
    const std::uint32_t other = link.addPane();
    link.stepUntilQuiet();
    const std::uint64_t before = link.host().counters().datagramsSent;
    link.write(hidden, "hidden change");
    link.write(other, "visible change");
    link.serviceWithoutCarrying();
    REQUIRE(link.host().counters().datagramsSent >= before + 2);
    std::map<std::uint32_t, std::size_t> updates;
    link.onHostFrame([&](FrameKind kind, std::uint32_t handle, SyncLink::Carrier, std::size_t) {
        if (kind == FrameKind::Update)
            ++updates[handle];
    });
    link.client().setView(hidden, ViewState::RecordsOnly);
    link.host().receiveStream(link.client().takeStreamOutput(), link.now());
    // Carry the survivor now. Delaying its delivery by a whole fixture step
    // and its coalesced Ack by another would cross the loss timeout and owe
    // a legitimate second update; this case isolates queue withdrawal.
    link.pump();
    CHECK_EQ(updates[hidden], 0u);
    CHECK_EQ(updates[other], 1u);
    for (int step = 0; step < 20; ++step)
        link.step();
    CHECK_EQ(updates[hidden], 0u);
    CHECK_EQ(updates[other], 1u);
    CHECK_STR_EQ(link.disagreement(other), "");
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.client().setView(hidden, ViewState::Live);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(hidden), "");
}

TEST(abortingAQueuedCheckpointHeadLeavesNoPacingDebt)
{
    SyncLink link(0, 24);
    link.stepUntilQuiet();
    link.resize(30, 80);
    link.serviceWithoutCarrying();
    REQUIRE(link.host().queuedBulkBytes() > 0);
    // No frame left: the queued head reserves space but is not stream debt.
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.client().setView(link.handle(), ViewState::Dormant);
    link.host().receiveStream(link.client().takeStreamOutput(), link.now());
    CHECK_EQ(link.host().queuedBulkBytes(), 0u);
    for (int step = 0; step < 20; ++step)
        link.step();
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.client().setView(link.handle(), ViewState::Live);
    link.stepUntilQuiet();
    CHECK_STR_EQ(link.disagreement(link.handle()), "");
}

TEST(cancelingAnUnsentHeadWakesAStructureWithoutWaitingForAnImpossibleAck)
{
    SyncLink link(0, 24);
    link.stepUntilQuiet();
    link.resize(30, 80);
    link.serviceWithoutCarrying();
    REQUIRE(link.host().queuedBulkBytes() > 0);
    const std::size_t reservation = link.host().queuedInteractiveBytes();
    const std::uint64_t window = link.host().pacer().windowBytes(link.now());
    std::vector<std::uint8_t> checkpointBody;
    encode(checkpointBody, CheckpointHead {});
    REQUIRE(reservation == frameBytes(link.handle(), checkpointBody.size()));
    REQUIRE(window > reservation && window < maxFrameBodyBytes);
    std::vector<std::uint8_t> unbindBody;
    encode(unbindBody, PaneUnbind { .handle = link.handle(), .reason = 0 });
    const std::size_t unbindBytes = frameBytes(0, unbindBody.size());
    REQUIRE(reservation > unbindBytes);
    // The host treats this payload as opaque; this test stops at the transport
    // boundary rather than asking a tree decoder to interpret its contents.
    // Derive its size from the codecs: it fits beside the small PaneUnbind,
    // but not beside the checkpoint head whose reservation is being freed.
    std::vector<std::uint8_t> structureHead;
    encode(structureHead, WorkspaceStructureHead { .workspaceID = 7, .revision = 1, .transfer = 0, .isGone = false, .record = {} });
    const std::size_t targetBytes = static_cast<std::size_t>(window) - reservation / 2;
    REQUIRE(targetBytes > structureHead.size());
    std::size_t recordBytes = targetBytes - structureHead.size();
    while (frameBytes(0, structureHead.size() + recordBytes) > targetBytes)
        --recordBytes;
    const std::size_t structureBytes = frameBytes(0, structureHead.size() + recordBytes);
    REQUIRE(structureBytes + reservation > window);
    REQUIRE(structureBytes + unbindBytes <= window);
    auto record = std::make_shared<const std::vector<std::uint8_t>>(recordBytes, 0);
    link.host().offerWorkspaceStructure(7, 1, record);
    const auto refused = link.host().counters().windowRefusals;
    link.host().service(link.now());
    REQUIRE(link.host().counters().windowRefusals > refused);
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    // Unlike setPaneView, unbind goes through the common checkpoint/history
    // cancellation path. Its tiny PaneUnbind leaves room for the structure.
    link.unbind();
    REQUIRE(link.host().queuedInteractiveBytes() == unbindBytes);
    const auto due = link.host().nextServiceMicroseconds(link.now());
    REQUIRE(due);
    CHECK(*due <= link.now());
    const auto structures = link.host().counters().structuresSent;
    link.host().service(link.now());
    CHECK_EQ(link.host().counters().structuresSent, structures + 1);
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
}

// A Dormant pane is told it changed once, not once per change: a second
// PaneDirty waits until it has been Live and gone Dormant again.
TEST(aPaneDirtyIsOncePerDormantSpan)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane();
    Tally tally(link, pane);
    for (int span = 0; span < 3; ++span) {
        link.client().setView(pane, ViewState::Dormant);
        link.step();
        for (int change = 0; change < 10; ++change) {
            link.write(pane, "x");
            link.step();
        }
        link.client().setView(pane, ViewState::Live);
        link.step();
        link.step();
    }
    CHECK_EQ(tally[FrameKind::PaneDirty], std::size_t { 3 });
    CHECK_EQ(link.held(pane), link.newest(pane));
}

// An agent's pane starts RecordsOnly: its records arrive and no rows do, not
// even a checkpoint, until the agent asks for the screen by setting it Live.
TEST(anAgentsPaneSendsNoRowsUntilItAsks)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Agent);
    const std::uint32_t pane = link.bindPane(24, 80, "$ agent");
    Tally tally(link, pane);
    for (int step = 0; step < 32; ++step) {
        link.write(pane, "more output\r\n");
        link.step();
    }
    PaneFacts facts;
    facts.programTitle = "agent title";
    link.source(pane).setFacts(facts);
    link.step();
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 0 });
    CHECK_EQ(tally[FrameKind::Update] + tally[FrameKind::CheckpointHead] + tally[FrameKind::PaneDirty], std::size_t { 0 });
    REQUIRE(link.client().paneFacts(pane));
    CHECK(link.client().paneFacts(pane)->programTitle == "agent title");

    link.client().setView(pane, ViewState::Live);
    for (int step = 0; step < 16 && link.client().counters().checkpointsApplied == 0; ++step)
        link.step();
    CHECK_EQ(link.client().counters().checkpointsApplied, std::uint64_t { 1 });
    // Its history follows the checkpoint as backfill.
    link.stepUntilQuiet();
    const std::string difference = link.disagreement(pane);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// Eight panes bound at once with one Live, as a client restoring eight tabs
// with one visible: one pane's screen crosses, and the other seven send at
// most a PaneDirty each.
TEST(eightPanesWithOneLiveSendOneScreen)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint32_t> handles;
    link.setViewOnBind([](std::uint32_t handle) { return handle == 1 ? ViewState::Live : ViewState::Dormant; });
    for (int pane = 0; pane < 8; ++pane)
        handles.push_back(link.bindPane(24, 80, "$ pane " + std::to_string(pane)));
    for (int step = 0; step < 16; ++step) {
        for (const std::uint32_t handle : handles)
            link.write(handle, "tick\r\n");
        link.step();
    }
    CHECK_EQ(link.host().counters().checkpointsSent, std::uint64_t { 1 });
    CHECK_EQ(link.client().counters().checkpointsApplied, std::uint64_t { 1 });
    CHECK_EQ(link.held(handles.front()), link.newest(handles.front()));
    for (std::size_t index = 1; index < handles.size(); ++index)
        CHECK_EQ(link.held(handles[index]), std::uint64_t { 0 });
    // Each pane may be told once that it changed: the seven kept Dormant, and
    // the Live one if it changed before the host read the View that shows it.
    CHECK(link.client().counters().paneDirtiesReceived <= 8);
}

// A theme changes under a pane in a background tab: the client's terminal
// wears the new colours without the pane being shown, from one block and no
// update, so the tab drawn from them restyles with the rest of the window. A
// frame that moves no colour sends no second block, and showing the pane
// sends no checkpoint and no palette: the update that wakes it builds on the
// block the client already holds.
TEST(aDormantPanesTerminalTakesAPaletteChangeWithoutBeingShown)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(24, 80));
    const std::uint32_t pane = link.addPane(24, 80, "$ ");
    link.stepUntilQuiet();
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    Tally tally(link, pane);
    const std::uint64_t checkpointsBefore = link.host().counters().checkpointsSent;
    // The foreground and background a light theme sets, in one frame.
    link.write(pane, "\x1b]11;rgb:fa/fa/f0\x1b\\\x1b]10;rgb:20/20/28\x1b\\");
    for (int step = 0; step < 8; ++step)
        link.step();
    const Ubiquitty::Terminal& host = link.hostTerminal(pane);
    const Ubiquitty::Terminal& client = link.clientTerminal(pane);
    CHECK_EQ(tally[FrameKind::Update], std::size_t { 0 });
    CHECK(client.palette.defaultBackground == host.palette.defaultBackground);
    CHECK(client.palette.defaultForeground == host.palette.defaultForeground);
    CHECK(client.palette == host.palette);
    CHECK_EQ(tally[FrameKind::PanePalette], std::size_t { 1 });
    CHECK_EQ(link.client().counters().palettesApplied, std::uint64_t { 1 });

    link.write(pane, "no colour moves\r\n");
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK_EQ(tally[FrameKind::PanePalette], std::size_t { 1 });

    link.client().setView(pane, ViewState::Live);
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().checkpointsSent, checkpointsBefore);
    CHECK_EQ(link.client().counters().digestMismatches, std::uint64_t { 0 });
    CHECK_EQ(link.client().counters().resyncsSent, std::uint64_t { 0 });
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK(client.palette == host.palette);
    const std::string difference = link.disagreement(pane);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A pane bound hidden and never shown keeps the client's own colours: binding
// it costs no palette, and its first block is the one a theme change sends.
TEST(aPaneNeverShownIsSentItsPaletteOnlyWhenItMoves)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    link.setViewOnBind([](std::uint32_t handle) { return handle == 1 ? ViewState::Live : ViewState::Dormant; });
    const std::uint32_t shown = link.bindPane(24, 80, "$ shown");
    const std::uint32_t hidden = link.bindPane(24, 80, "$ hidden");
    Tally tally(link, hidden);
    for (int step = 0; step < 16; ++step)
        link.step();
    CHECK_EQ(tally[FrameKind::PanePalette], std::size_t { 0 });
    link.write(hidden, "\x1b]11;rgb:fa/fa/f0\x1b\\");
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK_EQ(tally[FrameKind::PanePalette], std::size_t { 1 });
    CHECK(link.clientTerminal(hidden).palette.defaultBackground == link.hostTerminal(hidden).palette.defaultBackground);
    CHECK_EQ(link.held(hidden), std::uint64_t { 0 });
    CHECK_EQ(link.held(shown), link.newest(shown));
}

// A View is the client's to send and names a pane the host bound: one for a
// handle never bound closes the connection, and so does one the host sends.
TEST(aViewForAPaneNeverBoundOrFromTheHostCloses)
{
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        std::vector<std::uint8_t> body;
        encode(body, View { .state = ViewState::Live });
        link.sendToHost(FrameKind::View, body, 9);
        CHECK(link.host().state() == ConnectionState::Closed);
        CHECK(link.host().byeReason() == ByeReason::UnboundHandle);
    }
    {
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        const std::uint32_t pane = link.addPane();
        std::vector<std::uint8_t> body;
        encode(body, View { .state = ViewState::Dormant });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::View, pane, body));
        link.deliverToClient(frame);
        CHECK(link.client().state() == ConnectionState::Closed);
        CHECK(link.client().byeReason() == ByeReason::Malformed);
    }
    {
        // A state byte past the last state is malformed.
        SyncLink link(0, UbiquittyTest::syncDefaultFacts());
        const std::uint32_t pane = link.addPane();
        const std::vector<std::uint8_t> body { 7 };
        link.sendToHost(FrameKind::View, body, pane);
        CHECK(link.host().state() == ConnectionState::Closed);
        CHECK(link.host().byeReason() == ByeReason::Malformed);
    }
}

UBIQUITTY_TEST_MAIN()
