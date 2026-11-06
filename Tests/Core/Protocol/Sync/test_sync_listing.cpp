// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_listing.cpp — what a host sends a client that lists it
// (Docs/sync.md §7.3, §7.7): a workspace's structure, and the records of a
// pane the client has not bound.
//
// A listed pane is sent its facts and its status set and nothing else: no
// screen, no history, and no frame the client addresses to it is taken. Its
// records leave at the listing's cadence, however fast they move. A
// workspace's structure crosses in one frame when it fits and as a bulk
// transfer when it does not; the host owes a client the newest revision and
// never queues an older one behind it. A client keeps the newest of each
// workspace and forgets one the host withdrew.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "SyncLink.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
namespace OSC = Ubiquitty::OSC;

namespace {

/// A pane a host runs, with facts and a status set already read for it.
struct HostedPane {
    Ubiquitty::Terminal terminal;
    std::unique_ptr<PaneSource> source;

    explicit HostedPane(std::uint64_t number, std::size_t statusRecords = 1)
    {
        Ubiquitty::resize(terminal, 24, 80);
        source = std::make_unique<PaneSource>(terminal, ID128 { .low = number, .high = 77 });
        UbiquittyTest::feed(terminal, "$ secret on the screen");
        source->publish(FrameClass::Structural, 1);
        setTitle("pane " + std::to_string(number));
        StatusRecords records;
        for (std::size_t index = 0; index < statusRecords; ++index) {
            OSC::ProgramStatus record;
            record.id = "job/" + std::to_string(index);
            record.app = "agent";
            record.state = OSC::ProgramState::Working;
            record.message.resize(statusRecords > 1 ? maxStatusMessageBytes : 8, 'm');
            records.store.records.push_back(std::move(record));
        }
        source->setStatus(std::move(records));
    }

    /// Sets the pane's title, as a host reads it into the facts.
    void setTitle(const std::string& title)
    {
        PaneFacts facts = source->facts();
        facts.programTitle = title;
        facts.rows = 24;
        facts.columns = 80;
        source->setFacts(std::move(facts));
    }
};

/// What the client's owner was told, in order.
struct Recorder final : ClientPaneDelegate {
    std::vector<ID128> listed;
    std::vector<ID128> changed;
    std::vector<ID128> unlisted;
    std::vector<std::uint32_t> structures;
    std::size_t bound = 0;

    void paneListed(ID128 paneID) override { listed.push_back(paneID); }
    void listedPaneChanged(ID128 paneID) override { changed.push_back(paneID); }
    void paneUnlisted(ID128 paneID) override { unlisted.push_back(paneID); }
    void workspaceStructureChanged(std::uint32_t workspaceID) override { structures.push_back(workspaceID); }
    void paneBound(std::uint32_t, ID128) override { ++bound; }
};

/// A host and a client that has greeted it, carried a millisecond a step.
struct Listing {
    Recorder told;
    HostConnection host;
    ClientConnection client;
    std::uint64_t now = 1;
    /// The frames the host sent, by kind and in order.
    std::map<FrameKind, std::size_t> sent;
    std::vector<FrameKind> order;

    explicit Listing(HostPolicy policy = UbiquittyTest::syncHostPolicy(0))
        : host(policy, HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = Role::Owner, .principal = "test", .mayDrive = true, .verifiedKind = ClientKind::Human, .selection = { .target = AdmissionTarget::Control, .sessionID = 0, .workspaceID = 0, .daemonInstanceID = 1, .socketName = "listing-test.sock", .buildFlags = 0, .hostPlatform = "fixture" } })
        , client(UbiquittyTest::syncClientPolicy, identity(), &told)
    {
        client.start();
        for (int steps = 0; steps < 10 && !client.welcome(); ++steps)
            step();
        sent.clear();
        order.clear();
    }

    static ClientIdentity identity()
    {
        ClientIdentity identity { .clientInstance = { 7, 8 }, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts(), .resume = std::nullopt };
        identity.request.target = AdmissionTarget::Control;
        return identity;
    }

    /// One pass: the client's bytes reach the host, the host services, and
    /// what it sends reaches the client.
    ///
    /// - Parameter advance: How far the clock moves first, in microseconds.
    void step(std::uint64_t advance = 1'000)
    {
        now += advance;
        host.receiveStream(client.takeStreamOutput(), now);
        host.service(now);
        std::vector<std::uint8_t> down;
        while (host.popStreamFrame(down, now)) {
        }
        FrameDecoder decoder;
        decoder.append(down);
        while (const std::optional<Frame> frame = decoder.next()) {
            ++sent[frame->kind];
            order.push_back(frame->kind);
        }
        client.receiveStream(down, now);
        client.tick(now);
    }

    void settle(int steps = 40)
    {
        for (int index = 0; index < steps; ++index)
            step();
    }

    [[nodiscard]] std::size_t count(FrameKind kind) const
    {
        const auto found = sent.find(kind);
        return found == sent.end() ? 0 : found->second;
    }

    /// Hands the client one frame as if the host had sent it.
    void deliver(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
    {
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, channel, body));
        client.receiveStream(frame, now);
    }

    /// Hands the host one frame as if the client had sent it.
    void send(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
    {
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, channel, body));
        host.receiveStream(frame, now);
    }
};

std::shared_ptr<const std::vector<std::uint8_t>> recordOf(std::size_t bytes, std::uint8_t fill)
{
    auto record = std::make_shared<std::vector<std::uint8_t>>(bytes);
    for (std::size_t index = 0; index < bytes; ++index)
        (*record)[index] = static_cast<std::uint8_t>(fill + index * 31);
    return record;
}

} // namespace

TEST(aListedPaneIsSentItsRecordsAndNothingElse)
{
    Listing listing;
    HostedPane first(1);
    HostedPane second(2);
    const std::uint32_t firstHandle = listing.host.listPane(*first.source, ListedDetail::Whole);
    const std::uint32_t secondHandle = listing.host.listPane(*second.source, ListedDetail::Whole);
    CHECK(firstHandle >= firstListedHandle && secondHandle >= firstListedHandle && firstHandle != secondHandle);
    // Listed once: asked again, the pane has the handle it had.
    CHECK_EQ(listing.host.listPane(*first.source, ListedDetail::Whole), firstHandle);
    listing.settle();

    CHECK((listing.client.listedPanes() == std::vector<ID128> { first.source->paneID(), second.source->paneID() }));
    CHECK((listing.told.listed == listing.client.listedPanes()));
    for (const HostedPane* pane : { &first, &second }) {
        const PaneFacts* facts = listing.client.listedPaneFacts(pane->source->paneID());
        const StatusRecords* status = listing.client.listedPaneStatus(pane->source->paneID());
        REQUIRE(facts != nullptr && status != nullptr);
        CHECK(*facts == pane->source->facts());
        CHECK(*status == pane->source->status());
    }
    CHECK(listing.client.listedPaneFacts(first.source->paneID())->programTitle == "pane 1");
    CHECK_EQ(listing.count(FrameKind::PaneListed), std::size_t { 2 });
    CHECK_EQ(listing.count(FrameKind::PaneFacts), std::size_t { 2 });
    CHECK_EQ(listing.count(FrameKind::StatusRecords), std::size_t { 2 });
    // Nothing of the screen, and no binding: the pane is nobody's attachment.
    for (const FrameKind kind : { FrameKind::PaneBind, FrameKind::CheckpointHead, FrameKind::Update, FrameKind::BulkChunk, FrameKind::PaneDirty, FrameKind::HistoryInline, FrameKind::HistoryHead, FrameKind::Receipt })
        CHECK_MSG(listing.count(kind) == 0, ("a listed pane was sent a frame of kind " + std::to_string(std::to_underlying(kind))).c_str());
    CHECK(listing.client.boundPanes().empty() && listing.host.boundPanes().empty());
    CHECK_EQ(listing.host.handleSlots(), std::size_t { 0 });
    CHECK_EQ(listing.told.bound, std::size_t { 0 });
    CHECK(listing.client.paneTerminal(firstHandle) == nullptr);
    CHECK((listing.host.listedPanes() == std::vector<std::pair<std::uint32_t, ID128>> { { firstHandle, first.source->paneID() }, { secondHandle, second.source->paneID() } }));
    // The pane's size has no say from a client that only lists it.
    CHECK(!first.source->geometry().status().owner.has_value());
    CHECK(listing.host.state() == ConnectionState::Attached && listing.client.state() == ConnectionState::Attached);
}

TEST(aListedPanesRecordsLeaveAtTheListingsCadence)
{
    Listing listing;
    HostedPane pane(1);
    (void)listing.host.listPane(*pane.source, ListedDetail::Whole);
    listing.settle();
    REQUIRE(listing.count(FrameKind::PaneFacts) == 1);
    const std::uint64_t first = listing.now;

    // The title moves every millisecond for half a second: nothing leaves.
    for (int change = 0; change < 500; ++change) {
        pane.setTitle("title " + std::to_string(change));
        listing.step();
    }
    CHECK_EQ(listing.count(FrameKind::PaneFacts), std::size_t { 1 });
    CHECK(listing.client.listedPaneFacts(pane.source->paneID())->programTitle == "pane 1");
    // The host is due when the cadence allows the next, not at once: a
    // listing does not keep its loop awake.
    const std::optional<std::uint64_t> due = listing.host.nextServiceMicroseconds(listing.now);
    REQUIRE(due.has_value());
    CHECK(*due > listing.now && *due <= first + listedRecordsIntervalMicroseconds);

    // A second after the first, the newest goes, once.
    for (int steps = 0; steps < 700; ++steps)
        listing.step();
    CHECK_EQ(listing.count(FrameKind::PaneFacts), std::size_t { 2 });
    CHECK(listing.client.listedPaneFacts(pane.source->paneID())->programTitle == "title 499");
    CHECK_EQ(listing.client.listedPaneFacts(pane.source->paneID())->revision, pane.source->facts().revision);
    // Nothing moved since: nothing is owed, and nothing is due for it.
    for (int steps = 0; steps < 1'500; ++steps)
        listing.step();
    CHECK_EQ(listing.count(FrameKind::PaneFacts), std::size_t { 2 });
    CHECK((listing.told.changed.size() == 3));
}

TEST(aStatusSetOverAFrameReachesAListedPaneWhole)
{
    Listing listing;
    HostedPane pane(1, OSC::programStatusLimit);
    (void)listing.host.listPane(*pane.source, ListedDetail::Whole);
    for (int steps = 0; steps < 400 && !listing.client.listedPaneStatus(pane.source->paneID()); ++steps)
        listing.step();
    const StatusRecords* status = listing.client.listedPaneStatus(pane.source->paneID());
    REQUIRE(status != nullptr);
    CHECK(*status == pane.source->status());
    CHECK(listing.count(FrameKind::StatusRecords) > 1);
    CHECK_EQ(listing.host.counters().statusSetsSent, 1u);
    // A set begun finishes a frame a pass: it does not wait a second a frame.
    CHECK(listing.now < listedRecordsIntervalMicroseconds);
}

TEST(aFrameAddressedToAListedPaneIsOneForNoPane)
{
    // A listing reads facts. It cannot ask for the screen, type, or be taken
    // for an attachment by naming the handle it was given.
    const auto closes = [](FrameKind kind, const std::vector<std::uint8_t>& body) {
        Listing listing;
        HostedPane pane(1);
        const std::uint32_t handle = listing.host.listPane(*pane.source, ListedDetail::Whole);
        listing.settle();
        listing.send(kind, handle, body);
        listing.step();
        const bool isClosed = listing.host.state() == ConnectionState::Closed && listing.host.byeReason() == ByeReason::UnboundHandle; // bool-ok: the verdict
        // And nothing of the screen left on the way.
        return isClosed && listing.count(FrameKind::CheckpointHead) == 0 && listing.count(FrameKind::Update) == 0 && pane.source->inputLedger().lastWritten({ 7, 8 }).value_or(0) == 0;
    };
    std::vector<std::uint8_t> view;
    encode(view, View { .state = ViewState::Live });
    CHECK(closes(FrameKind::View, view));
    std::vector<std::uint8_t> raw;
    const std::array<std::uint8_t, 1> typed { 'x' };
    encode(raw, Raw { .inputSeq = 1, .bytes = typed });
    CHECK(closes(FrameKind::Raw, raw));
    std::vector<std::uint8_t> resync;
    encode(resync, Resync { .reason = ResyncReason::DigestMismatch, .generation = 0, .sectionHashes = {} });
    CHECK(closes(FrameKind::Resync, resync));
}

TEST(anUnlistedPaneIsForgottenAndItsHandleIsNotUsedAgain)
{
    Listing listing;
    HostedPane first(1);
    HostedPane second(2);
    const std::uint32_t firstHandle = listing.host.listPane(*first.source, ListedDetail::Whole);
    const std::uint32_t secondHandle = listing.host.listPane(*second.source, ListedDetail::Whole);
    listing.settle();
    listing.host.unlistPane(firstHandle);
    // A handle that lists nothing is ignored.
    listing.host.unlistPane(firstHandle);
    listing.host.unlistPane(12);
    listing.settle();
    CHECK((listing.client.listedPanes() == std::vector<ID128> { second.source->paneID() }));
    CHECK((listing.told.unlisted == std::vector<ID128> { first.source->paneID() }));
    CHECK(listing.client.listedPaneFacts(first.source->paneID()) == nullptr);
    CHECK_EQ(listing.count(FrameKind::PaneUnbind), std::size_t { 1 });
    // Its records move and none is sent.
    first.setTitle("gone");
    for (int steps = 0; steps < 1'200; ++steps)
        listing.step();
    CHECK_EQ(listing.count(FrameKind::PaneFacts), std::size_t { 2 });
    // Listed again, it is a new listing under a handle nothing has had.
    const std::uint32_t again = listing.host.listPane(*first.source, ListedDetail::Whole);
    CHECK(again != firstHandle && again != secondHandle && again >= firstListedHandle);
    listing.settle();
    REQUIRE(listing.client.listedPaneFacts(first.source->paneID()) != nullptr);
    CHECK(listing.client.listedPaneFacts(first.source->paneID())->programTitle == "gone");
    CHECK(listing.client.state() == ConnectionState::Attached);
}

TEST(aWorkspacesStructureCrossesInOneFrameOrAsATransfer)
{
    Listing listing;
    const auto small = recordOf(300, 1);
    listing.host.offerWorkspaceStructure(4, 1, small);
    listing.step();
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 1 });
    CHECK_EQ(listing.count(FrameKind::BulkChunk), std::size_t { 0 });
    REQUIRE(listing.client.workspaceStructures().contains(4));
    CHECK(listing.client.workspaceStructures().at(4).record == *small && listing.client.workspaceStructures().at(4).revision == 1);
    CHECK((listing.told.structures == std::vector<std::uint32_t> { 4 }));

    // A record at the frame's edge is still one frame; one byte more is not.
    const std::size_t inlineMost = maxFrameBodyBytes - (4 + 8 + 1 + 1);
    listing.host.offerWorkspaceStructure(5, 1, recordOf(inlineMost, 2));
    listing.settle();
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 2 });
    CHECK_EQ(listing.count(FrameKind::BulkChunk), std::size_t { 0 });
    CHECK_EQ(listing.client.workspaceStructures().at(5).record.size(), inlineMost);
    listing.host.offerWorkspaceStructure(6, 1, recordOf(inlineMost + 1, 3));
    const auto large = recordOf(600'000, 4);
    listing.host.offerWorkspaceStructure(7, 9, large);
    for (int steps = 0; steps < 4'000 && !listing.client.workspaceStructures().contains(7); ++steps)
        listing.step();
    CHECK(listing.count(FrameKind::BulkChunk) > 2);
    REQUIRE(listing.client.workspaceStructures().contains(6) && listing.client.workspaceStructures().contains(7));
    CHECK_EQ(listing.client.workspaceStructures().at(6).record.size(), inlineMost + 1);
    CHECK(listing.client.workspaceStructures().at(7).record == *large && listing.client.workspaceStructures().at(7).revision == 9);
    CHECK_EQ(listing.host.counters().structuresSent, 4u);
    CHECK_EQ(listing.host.counters().structureTransfersSent, 2u);
    CHECK_EQ(listing.client.counters().structuresApplied, 4u);
    // A record past what a connection carries is not offered at all.
    listing.host.offerWorkspaceStructure(8, 1, recordOf(maxWorkspaceStructureRecordBytes + 1, 5));
    listing.host.offerWorkspaceStructure(8, 0, small);
    listing.host.offerWorkspaceStructure(8, 1, std::make_shared<std::vector<std::uint8_t>>());
    listing.settle();
    CHECK(!listing.client.workspaceStructures().contains(8));
    CHECK_EQ(listing.host.counters().structuresSent, 4u);
    CHECK_EQ(listing.host.counters().structureTransfersSent, 2u);
    CHECK(listing.host.state() == ConnectionState::Attached && listing.client.state() == ConnectionState::Attached);
}

TEST(theHostOwesTheNewestRevisionAndQueuesNoOlderOne)
{
    Listing listing;
    // Three revisions before a pass: one frame, the newest.
    listing.host.offerWorkspaceStructure(4, 1, recordOf(100, 1));
    listing.host.offerWorkspaceStructure(4, 2, recordOf(100, 2));
    const auto third = recordOf(100, 3);
    listing.host.offerWorkspaceStructure(4, 3, third);
    listing.settle();
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 1 });
    CHECK(listing.client.workspaceStructures().at(4).record == *third && listing.client.workspaceStructures().at(4).revision == 3);
    // The same revision again, or an older one, is nothing new.
    listing.host.offerWorkspaceStructure(4, 3, third);
    listing.host.offerWorkspaceStructure(4, 2, recordOf(100, 2));
    listing.settle();
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 1 });
    CHECK(listing.host.nextServiceMicroseconds(listing.now) != std::optional { listing.now });

    // While a transfer of a workspace is leaving, its later revisions wait
    // and replace one another: two transfers leave, not four.
    listing.host.offerWorkspaceStructure(9, 1, recordOf(500'000, 1));
    listing.step();
    listing.step();
    REQUIRE(!listing.client.workspaceStructures().contains(9));
    listing.host.offerWorkspaceStructure(9, 2, recordOf(500'000, 2));
    listing.step();
    listing.host.offerWorkspaceStructure(9, 3, recordOf(500'000, 3));
    listing.step();
    const auto newest = recordOf(500'000, 4);
    listing.host.offerWorkspaceStructure(9, 4, newest);
    for (int steps = 0; steps < 8'000 && (!listing.client.workspaceStructures().contains(9) || listing.client.workspaceStructures().at(9).revision != 4); ++steps)
        listing.step();
    REQUIRE(listing.client.workspaceStructures().contains(9));
    CHECK(listing.client.workspaceStructures().at(9).record == *newest);
    CHECK_EQ(listing.host.counters().structureTransfersSent, 2u);
    CHECK(listing.client.state() == ConnectionState::Attached);
}

TEST(aWithdrawnWorkspaceIsForgottenByAClientThatWasToldOfIt)
{
    Listing listing;
    listing.host.offerWorkspaceStructure(4, 1, recordOf(100, 1));
    listing.host.offerWorkspaceStructure(5, 1, recordOf(100, 2));
    listing.settle();
    listing.host.withdrawWorkspaceStructure(4);
    listing.host.withdrawWorkspaceStructure(4);
    listing.settle();
    CHECK(!listing.client.workspaceStructures().contains(4) && listing.client.workspaceStructures().contains(5));
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 3 });
    CHECK((listing.told.structures == std::vector<std::uint32_t> { 4, 5, 4 }));
    // One the client was never told of is forgotten in silence.
    listing.host.offerWorkspaceStructure(6, 1, recordOf(100, 3));
    listing.host.withdrawWorkspaceStructure(6);
    listing.host.withdrawWorkspaceStructure(77);
    listing.settle();
    CHECK_EQ(listing.count(FrameKind::WorkspaceStructure), std::size_t { 3 });
    CHECK(!listing.client.workspaceStructures().contains(6));
    // A workspace made again under the id is a structure once more.
    const auto again = recordOf(100, 4);
    listing.host.offerWorkspaceStructure(4, 1, again);
    listing.settle();
    REQUIRE(listing.client.workspaceStructures().contains(4));
    CHECK(listing.client.workspaceStructures().at(4).record == *again);
    // The host kept nothing of the one that went: the new one's revision is
    // the one it was offered under.
    CHECK_EQ(listing.client.workspaceStructures().at(4).revision, 1u);
}

TEST(theStructureHeadRoundTripsAndABodyNoHostSendsIsRefused)
{
    const auto encoded = [](const WorkspaceStructureHead& head) {
        std::vector<std::uint8_t> body;
        encode(body, head);
        return body;
    };
    const WorkspaceStructureHead inlined { .workspaceID = 0xAABBCCDD, .revision = 0x1122334455667788, .transfer = 0, .isGone = false, .record = { 1, 2, 3 } };
    std::optional<WorkspaceStructureHead> read = decodeWorkspaceStructureHead(encoded(inlined));
    REQUIRE(read.has_value());
    CHECK(read->workspaceID == inlined.workspaceID && read->revision == inlined.revision && read->transfer == 0 && !read->isGone && read->record == inlined.record);
    read = decodeWorkspaceStructureHead(encoded({ .workspaceID = 4, .revision = 2, .transfer = 300, .isGone = false, .record = {} }));
    REQUIRE(read.has_value());
    CHECK(read->transfer == 300 && read->record.empty());
    read = decodeWorkspaceStructureHead(encoded({ .workspaceID = 4, .revision = 2, .transfer = 0, .isGone = true, .record = {} }));
    REQUIRE(read.has_value());
    CHECK(read->isGone);

    const std::vector<std::uint8_t> body = encoded(inlined);
    for (std::size_t length = 0; length < body.size() - inlined.record.size(); ++length)
        CHECK(!decodeWorkspaceStructureHead(std::span { body }.first(length)).has_value());
    // Neither a record nor a transfer; a transfer with a record beside it; a
    // flag no flag names; a workspace that is gone and carries something.
    std::vector<std::uint8_t> bare = encoded({ .workspaceID = 4, .revision = 2, .transfer = 0, .isGone = true, .record = {} });
    bare[12] = 0;
    CHECK(!decodeWorkspaceStructureHead(bare).has_value());
    std::vector<std::uint8_t> both = encoded({ .workspaceID = 4, .revision = 2, .transfer = 3, .isGone = false, .record = {} });
    both.push_back(9);
    CHECK(!decodeWorkspaceStructureHead(both).has_value());
    std::vector<std::uint8_t> flagged = body;
    flagged[12] = 2;
    CHECK(!decodeWorkspaceStructureHead(flagged).has_value());
    std::vector<std::uint8_t> goneWithRecord = body;
    goneWithRecord[12] = 1;
    CHECK(!decodeWorkspaceStructureHead(goneWithRecord).has_value());
    std::vector<std::uint8_t> goneWithTransfer = encoded({ .workspaceID = 4, .revision = 2, .transfer = 3, .isGone = false, .record = {} });
    goneWithTransfer[12] = 1;
    CHECK(!decodeWorkspaceStructureHead(goneWithTransfer).has_value());
}

TEST(aListingFrameNoHostSendsClosesTheClient)
{
    const auto listedBody = [](std::uint32_t handle, std::uint64_t pane) {
        std::vector<std::uint8_t> body;
        encode(body, PaneBind { .handle = handle, .paneID = { .low = pane, .high = 0 } });
        return body;
    };
    {
        // A listed handle from the range bound panes take.
        Listing listing;
        listing.deliver(FrameKind::PaneListed, 0, listedBody(1, 1));
        CHECK(listing.client.state() == ConnectionState::Closed && listing.client.byeReason() == ByeReason::Malformed);
    }
    {
        // A handle listed twice.
        Listing listing;
        listing.deliver(FrameKind::PaneListed, 0, listedBody(firstListedHandle, 1));
        CHECK(listing.client.state() == ConnectionState::Attached);
        listing.deliver(FrameKind::PaneListed, 0, listedBody(firstListedHandle, 2));
        CHECK(listing.client.state() == ConnectionState::Closed);
    }
    {
        // A pane bound under a handle a listing takes.
        Listing listing;
        listing.deliver(FrameKind::PaneBind, 0, listedBody(firstListedHandle + 5, 1));
        CHECK(listing.client.state() == ConnectionState::Closed);
    }
    {
        // More listed than a connection holds.
        Listing listing;
        for (std::uint32_t index = 0; index < maxPanesPerConnection; ++index)
            listing.deliver(FrameKind::PaneListed, 0, listedBody(firstListedHandle + index, index + 1));
        CHECK(listing.client.state() == ConnectionState::Attached);
        CHECK_EQ(listing.client.listedPanes().size(), maxPanesPerConnection);
        listing.deliver(FrameKind::PaneListed, 0, listedBody(firstListedHandle + maxPanesPerConnection, 999));
        CHECK(listing.client.state() == ConnectionState::Closed);
    }
    {
        // Facts for a handle nothing was listed or bound under.
        Listing listing;
        std::vector<std::uint8_t> facts;
        encodePaneFacts(facts, PaneFacts {});
        listing.deliver(FrameKind::PaneFacts, firstListedHandle, facts);
        CHECK(listing.client.state() == ConnectionState::Closed);
    }
    {
        // A structure with neither a record nor a transfer.
        Listing listing;
        std::vector<std::uint8_t> body;
        encode(body, WorkspaceStructureHead { .workspaceID = 1, .revision = 1, .transfer = 0, .isGone = false, .record = {} });
        listing.deliver(FrameKind::WorkspaceStructure, 0, body);
        CHECK(listing.client.state() == ConnectionState::Closed);
    }
    {
        // An older revision arriving late is dropped, not refused; so is a
        // withdrawal of a workspace the client does not hold.
        Listing listing;
        const auto send = [&](std::uint64_t revision, std::uint8_t fill, bool isGone = false) { // bool-ok: the head's one flag
            std::vector<std::uint8_t> body;
            encode(body, WorkspaceStructureHead { .workspaceID = 1, .revision = revision, .transfer = 0, .isGone = isGone, .record = isGone ? std::vector<std::uint8_t> {} : std::vector<std::uint8_t> { fill } });
            listing.deliver(FrameKind::WorkspaceStructure, 0, body);
        };
        send(5, 50);
        send(4, 40);
        send(5, 51);
        CHECK(listing.client.state() == ConnectionState::Attached);
        CHECK((listing.client.workspaceStructures().at(1).record == std::vector<std::uint8_t> { 50 }));
        send(4, 0, true);
        CHECK(listing.client.workspaceStructures().contains(1));
        send(6, 0, true);
        CHECK(!listing.client.workspaceStructures().contains(1));
        send(7, 0, true);
        CHECK(listing.client.state() == ConnectionState::Attached);
        CHECK_EQ(listing.client.counters().structuresApplied, 2u);
    }
    {
        // More workspaces than a client holds.
        Listing listing;
        for (std::uint32_t index = 0; index <= maxHeldStructures && listing.client.state() == ConnectionState::Attached; ++index) {
            std::vector<std::uint8_t> body;
            encode(body, WorkspaceStructureHead { .workspaceID = index + 1, .revision = 1, .transfer = 0, .isGone = false, .record = { 1 } });
            listing.deliver(FrameKind::WorkspaceStructure, 0, body);
        }
        CHECK(listing.client.state() == ConnectionState::Closed);
        CHECK_EQ(listing.client.workspaceStructures().size(), maxHeldStructures);
    }
    {
        // A client says neither kind: the host closes on one.
        CHECK(directionOf(FrameKind::WorkspaceStructure) == Direction::HostToClient && directionOf(FrameKind::PaneListed) == Direction::HostToClient);
        CHECK(isConnectionKind(FrameKind::WorkspaceStructure) && isConnectionKind(FrameKind::PaneListed));
        Listing listing;
        listing.send(FrameKind::PaneListed, 0, listedBody(firstListedHandle, 1));
        listing.step();
        CHECK(listing.host.state() == ConnectionState::Closed);
    }
}

TEST(aListingThatMayNotReadAPanesScreenIsSentNoCommandLine)
{
    // A command line carries what a screen carries: a token, a password, a
    // path. A connection listed a pane it could not read is sent the names
    // of what runs and not the arguments.
    HostedPane pane(1);
    PaneFacts facts = pane.source->facts();
    facts.processChain = "sh > make";
    facts.processChainWithArguments = "sh > make -j8 TOKEN=secret";
    facts.currentDirectory = "/home/user/project";
    facts.terminalName = "ttys004";
    pane.source->setFacts(facts);
    Listing withheld;
    const std::uint32_t handle = withheld.host.listPane(*pane.source, ListedDetail::WithoutArguments);
    REQUIRE(handle != 0);
    withheld.settle();
    const PaneFacts* sent = withheld.client.listedPaneFacts(pane.source->paneID());
    REQUIRE(sent != nullptr);
    CHECK_STR_EQ(sent->processChain, "sh > make");
    CHECK_STR_EQ(sent->processChainWithArguments, "sh > make");
    // Everything else a listing shows is as the pane has it.
    PaneFacts expected = pane.source->facts();
    expected.processChainWithArguments = expected.processChain;
    CHECK(*sent == expected);
    CHECK_STR_EQ(sent->currentDirectory, "/home/user/project");
    CHECK_STR_EQ(sent->terminalName, "ttys004");
    // Listing it again cannot widen what the connection is sent.
    CHECK_EQ(withheld.host.listPane(*pane.source, ListedDetail::Whole), handle);
    facts.processChainWithArguments = "sh > make -j8 TOKEN=other";
    facts.programTitle = "moved";
    pane.source->setFacts(facts);
    withheld.step(2 * listedRecordsIntervalMicroseconds);
    withheld.settle();
    sent = withheld.client.listedPaneFacts(pane.source->paneID());
    REQUIRE(sent != nullptr);
    CHECK_STR_EQ(sent->programTitle, "moved");
    CHECK_STR_EQ(sent->processChainWithArguments, "sh > make");

    // A connection that could read the pane's screen is sent the facts whole.
    Listing whole;
    REQUIRE(whole.host.listPane(*pane.source, ListedDetail::Whole) != 0);
    whole.settle();
    sent = whole.client.listedPaneFacts(pane.source->paneID());
    REQUIRE(sent != nullptr);
    CHECK_STR_EQ(sent->processChainWithArguments, "sh > make -j8 TOKEN=other");
    CHECK(*sent == pane.source->facts());
}

UBIQUITTY_TEST_MAIN()
