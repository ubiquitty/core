// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_topology — topology never overtakes content (Docs/sync.md §6.10,
// §7.3, §7.8): a leaf names its pane's size and the generation that gave it
// that size; the host sends a workspace's structure behind the pane frames
// taken before the change; and a client that receives the structure first,
// as separate streams allow, never lays a pane out at a size its picture does
// not have.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;

namespace {

/// A frame that owns its body, to be delivered after the stream it was read
/// from is gone.
struct OwnedFrame {
    FrameKind kind = FrameKind::Bye;
    std::uint32_t channel = 0;
    std::vector<std::uint8_t> body;
};

/// A client attached to a pane, its frames carried by hand so a test decides
/// the order they arrive in.
struct Seat {
    SequenceSecrets secrets;
    AttachmentTable table { { 41, 42 }, measuredResumePolicy(), secrets };
    UbiquittyTest::OwnedPane pane;
    std::unique_ptr<HostConnection> connection;
    ClientAttachment client { UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity() };
    std::uint64_t now = 1;

    explicit Seat(HostPolicy policy = UbiquittyTest::syncHostPolicy(0))
    {
        accept(policy, 1);
        (void)client.connect(now);
        pump();
        pump();
        (void)connection->bindPane(*pane.source);
        settle();
    }

    void accept(HostPolicy policy, std::uint64_t number)
    {
        connection = std::make_unique<HostConnection>(policy, HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
        connection->serveFromTable(table, owner, number, [this](ID128 id) { return id == paneID ? pane.source.get() : nullptr; });
    }

    /// Everything the host has to send after one service pass, as frames.
    std::vector<OwnedFrame> hostFrames()
    {
        connection->service(now);
        std::vector<std::uint8_t> down;
        while (connection->popStreamFrame(down, now)) {
        }
        std::vector<OwnedFrame> frames;
        FrameDecoder decoder;
        decoder.append(down);
        while (const std::optional<Frame> frame = decoder.next())
            frames.push_back({ .kind = frame->kind, .channel = frame->channel, .body = { frame->body.begin(), frame->body.end() } });
        return frames;
    }

    /// Hands the client one frame and carries what it answers.
    void deliver(const OwnedFrame& frame)
    {
        std::vector<std::uint8_t> one;
        (void)appendFrame(one, frame.kind, frame.channel, frame.body);
        client.receiveStream(one, now);
        client.tick(now);
        (void)readNotifies(client);
        noteStructure();
        connection->receiveStream(client.takeStreamOutput(), now);
    }

    /// Offers the client the structure of a workspace of this one pane, as a
    /// host does when the structure changes.
    ///
    /// - Parameter revision: The structure's revision.
    void offer(std::uint64_t revision)
    {
        Ubiquitty::Session::TreeWorkspace workspace;
        workspace.id = workspaceID;
        workspace.revision = revision;
        Ubiquitty::Session::TreeWindow window;
        Ubiquitty::Session::TreeTab tab;
        tab.split = tree(revision).root;
        tab.panes.emplace_back();
        window.tabs.push_back(std::move(tab));
        workspace.windows.push_back(std::move(window));
        auto record = std::make_shared<std::vector<std::uint8_t>>();
        REQUIRE(Ubiquitty::Session::encodeWorkspaceStructure(*record, workspace));
        connection->offerWorkspaceStructure(workspaceID, revision, std::move(record));
    }

    /// Reads the structure the client holds into the topology of the tab it
    /// draws, as a client does when one arrives.
    void noteStructure()
    {
        const ClientConnection* held = client.connection();
        if (!held)
            return;
        const auto structure = held->workspaceStructures().find(workspaceID);
        if (structure == held->workspaceStructures().end())
            return;
        const std::optional<Ubiquitty::Session::TreeWorkspace> workspace = Ubiquitty::Session::decodeWorkspaceStructure(structure->second.record);
        REQUIRE(workspace.has_value());
        client.noteTopology({ .revision = structure->second.revision, .root = workspace->windows[0].tabs[0].split, .zoomedPane = std::nullopt });
    }

    static constexpr std::uint32_t workspaceID = 1;

    void pump()
    {
        for (const OwnedFrame& frame : hostFrames())
            deliver(frame);
        client.tick(now);
        connection->receiveStream(client.takeStreamOutput(), now);
    }

    /// Pumps each millisecond for a tenth of a second.
    void settle()
    {
        for (const std::uint64_t until = now + 100'000; now < until; now += 1'000)
            pump();
    }

    /// The tree a host with this one pane sends: the pane's size now and the
    /// generation of its latest Structural frame.
    [[nodiscard]] TopologyTree tree(std::uint64_t revision) const
    {
        TopologyNode leaf;
        leaf.paneID = paneID;
        leaf.rows = static_cast<std::uint16_t>(pane.terminal.rows);
        leaf.columns = static_cast<std::uint16_t>(pane.terminal.columns);
        leaf.structuralGeneration = pane.source->structuralGeneration();
        return { .revision = revision, .root = std::move(leaf), .zoomedPane = std::nullopt };
    }

    /// The program's window changes size: a Structural frame.
    void resizePane(int rows, int columns)
    {
        Ubiquitty::resize(pane.terminal, rows, columns);
        pane.source->publish(FrameClass::Structural, now);
    }

    /// Whether the rectangle the client would lay the pane out at fits the
    /// picture it holds: the rule no frame may break.
    [[nodiscard]] bool isRectangleThePictures() const
    {
        const std::optional<PaneRectangle> rectangle = client.paneRectangle(paneID);
        const Ubiquitty::Terminal* picture = client.paneTerminal(paneID);
        if (!rectangle || !picture)
            return true;
        return rectangle->isLetterboxed || (rectangle->rows == picture->rows && rectangle->columns == picture->columns);
    }
};

TopologyNode leafOf(ID128 pane, std::uint16_t rows, std::uint16_t columns, std::uint64_t generation)
{
    TopologyNode leaf;
    leaf.paneID = pane;
    leaf.rows = rows;
    leaf.columns = columns;
    leaf.structuralGeneration = generation;
    return leaf;
}

} // namespace

// The structure that names a pane's new size arrives ahead of the checkpoint
// that carries it, as a separate stream may deliver it. At every frame the
// client's rectangle is either letterboxed or the size of the grid it draws.
TEST(aStructureAheadOfItsCheckpointNeverResizesTheOldPicture)
{
    Seat seat;
    seat.offer(1);
    seat.settle();
    REQUIRE(seat.client.paneRectangle(paneID).has_value());
    const PaneRectangle before { .rows = 24, .columns = 80, .isLetterboxed = false };
    CHECK(*seat.client.paneRectangle(paneID) == before);

    seat.resizePane(30, 100);
    seat.offer(2);
    std::vector<OwnedFrame> frames = seat.hostFrames();
    const auto notify = std::ranges::find(frames, FrameKind::WorkspaceStructure, &OwnedFrame::kind);
    REQUIRE(notify != frames.end());
    REQUIRE(std::ranges::any_of(frames, [](const OwnedFrame& frame) { return frame.kind == FrameKind::CheckpointHead; }));
    // Channel 0 overtakes the pane's channel.
    std::ranges::rotate(frames.begin(), notify, notify + 1);
    REQUIRE(frames.front().kind == FrameKind::WorkspaceStructure);

    seat.deliver(frames.front());
    REQUIRE(seat.client.topology().has_value());
    REQUIRE(seat.client.topology()->revision == 2);
    // The tree is the new one and the picture the old: the pane keeps the
    // picture's size, letterboxed.
    const PaneRectangle waiting { .rows = 24, .columns = 80, .isLetterboxed = true };
    REQUIRE(seat.client.paneRectangle(paneID).has_value());
    CHECK(*seat.client.paneRectangle(paneID) == waiting);
    for (std::size_t index = 1; index < frames.size(); ++index) {
        seat.deliver(frames[index]);
        CHECK(seat.isRectangleThePictures());
    }
    for (int rounds = 0; rounds < 200 && seat.client.connection()->paneGeneration(1) != seat.pane.source->generation(); ++rounds) {
        seat.now += 1'000;
        for (const OwnedFrame& frame : seat.hostFrames()) {
            seat.deliver(frame);
            CHECK(seat.isRectangleThePictures());
        }
    }
    const PaneRectangle after { .rows = 30, .columns = 100, .isLetterboxed = false };
    REQUIRE(seat.client.paneRectangle(paneID).has_value());
    CHECK(*seat.client.paneRectangle(paneID) == after);
}

// The picture is newer than the tree: the pane changed size again and the
// tree that says so has not come. The stale leaf's size is not applied.
TEST(aPictureNewerThanTheTreeKeepsItsOwnSize)
{
    Seat seat;
    seat.offer(1);
    seat.settle();
    seat.resizePane(30, 100);
    seat.settle();
    REQUIRE(seat.client.paneTerminal(paneID)->rows == 30);
    const PaneRectangle stale { .rows = 30, .columns = 100, .isLetterboxed = true };
    REQUIRE(seat.client.paneRectangle(paneID).has_value());
    CHECK(*seat.client.paneRectangle(paneID) == stale);
    CHECK(seat.isRectangleThePictures());
    seat.offer(2);
    seat.settle();
    const PaneRectangle caughtUp { .rows = 30, .columns = 100, .isLetterboxed = false };
    CHECK(*seat.client.paneRectangle(paneID) == caughtUp);
}

// A structure offered when a pane's frame is owed leaves behind that frame,
// in the same pass; one offered twice before the pass is sent once, the
// newer.
TEST(theHostSendsAStructureBehindEarlierPaneFrames)
{
    Seat seat;
    seat.resizePane(30, 100);
    seat.offer(1);
    seat.offer(2);
    const std::vector<OwnedFrame> frames = seat.hostFrames();
    const auto head = std::ranges::find(frames, FrameKind::CheckpointHead, &OwnedFrame::kind);
    const auto notify = std::ranges::find(frames, FrameKind::WorkspaceStructure, &OwnedFrame::kind);
    REQUIRE(head != frames.end());
    REQUIRE(notify != frames.end());
    CHECK(head < notify);
    CHECK_EQ(std::ranges::count(frames, FrameKind::WorkspaceStructure, &OwnedFrame::kind), 1);
    const std::optional<WorkspaceStructureHead> said = decodeWorkspaceStructureHead(notify->body);
    REQUIRE(said.has_value());
    CHECK_EQ(said->revision, 2u);
    const std::optional<Ubiquitty::Session::TreeWorkspace> workspace = Ubiquitty::Session::decodeWorkspaceStructure(said->record);
    REQUIRE(workspace.has_value());
    const TopologyNode& root = workspace->windows[0].tabs[0].split;
    REQUIRE(root.isLeaf());
    CHECK_EQ(root.structuralGeneration, seat.pane.source->generation());
    for (const OwnedFrame& frame : frames)
        seat.deliver(frame);
    seat.settle();

    // An update, too, goes ahead of the notify queued after its frame.
    UbiquittyTest::feed(seat.pane.terminal, "output");
    seat.pane.source->publish(FrameClass::Settled, seat.now);
    seat.offer(3);
    seat.now += 50'000;
    const std::vector<OwnedFrame> later = seat.hostFrames();
    const auto update = std::ranges::find(later, FrameKind::Update, &OwnedFrame::kind);
    const auto second = std::ranges::find(later, FrameKind::WorkspaceStructure, &OwnedFrame::kind);
    REQUIRE(update != later.end());
    REQUIRE(second != later.end());
    CHECK(update < second);
}

// An owed structure makes the connection due at once while it can leave, and
// not while the interactive lane is past its bound: a record that cannot go
// must not keep the host's loop awake.
TEST(anOwedStructureIsDueOnlyWhileItCanLeave)
{
    Seat seat;
    seat.settle();
    seat.offer(1);
    CHECK(seat.connection->nextServiceMicroseconds(seat.now) == std::optional { seat.now });
    seat.settle();
    REQUIRE(seat.client.topology().has_value());

    HostPolicy narrow = UbiquittyTest::syncHostPolicy(0);
    narrow.maxQueuedInteractiveBytes = 0;
    Seat held(narrow);
    held.settle();
    // An update waits in the lane, so the lane is past its bound of nothing.
    UbiquittyTest::feed(held.pane.terminal, "output");
    held.pane.source->publish(FrameClass::Settled, held.now);
    held.now += 50'000;
    held.connection->service(held.now);
    REQUIRE(held.connection->queuedInteractiveBytes() > 0);
    const std::optional<std::uint64_t> without = held.connection->nextServiceMicroseconds(held.now);
    held.offer(1);
    CHECK(held.connection->nextServiceMicroseconds(held.now) == without);
}

// The rule itself: a leaf applies to a picture of at least its generation and
// of its size, and to no other.
TEST(aLeafAppliesOnlyToAPictureOfItsGenerationAndSize)
{
    const TopologyNode leaf = leafOf(paneID, 30, 100, 7);
    const PaneRectangle applied { .rows = 30, .columns = 100, .isLetterboxed = false };
    CHECK(paneRectangleFor(leaf, { .generation = 7, .rows = 30, .columns = 100 }) == applied);
    CHECK(paneRectangleFor(leaf, { .generation = 9, .rows = 30, .columns = 100 }) == applied);
    // The picture from before the change.
    const PaneRectangle older { .rows = 24, .columns = 80, .isLetterboxed = true };
    CHECK(paneRectangleFor(leaf, { .generation = 6, .rows = 24, .columns = 80 }) == older);
    // A picture of the leaf's size from before its generation is still the
    // old one: a pane that went back to a size it had before.
    const PaneRectangle sameSize { .rows = 30, .columns = 100, .isLetterboxed = true };
    CHECK(paneRectangleFor(leaf, { .generation = 6, .rows = 30, .columns = 100 }) == sameSize);
    // A picture past the leaf's generation at another size: the pane changed
    // again.
    const PaneRectangle newer { .rows = 40, .columns = 120, .isLetterboxed = true };
    CHECK(paneRectangleFor(leaf, { .generation = 9, .rows = 40, .columns = 120 }) == newer);
}

// A pane's structural generation is that of its newest Structural frame;
// settled frames, and the frame taken again for an echo context, leave it.
TEST(theStructuralGenerationIsTheNewestStructuralFrames)
{
    UbiquittyTest::OwnedPane pane;
    const std::uint64_t first = pane.source->structuralGeneration();
    CHECK_EQ(first, pane.source->generation());
    UbiquittyTest::feed(pane.terminal, "output");
    pane.source->publish(FrameClass::Settled, 2);
    pane.source->republish(3);
    CHECK_EQ(pane.source->structuralGeneration(), first);
    CHECK(pane.source->generation() > first);
    Ubiquitty::resize(pane.terminal, 30, 100);
    pane.source->publish(FrameClass::Structural, 4);
    CHECK_EQ(pane.source->structuralGeneration(), pane.source->generation());
}

// A new connection's first tree replaces the one a lost connection left,
// whatever its revision; on one connection an older tree is dropped.
TEST(aNewConnectionsFirstTreeReplacesTheOneHeld)
{
    Seat seat;
    seat.offer(40);
    seat.settle();
    REQUIRE(seat.client.topology().has_value());
    seat.client.noteTopology({ .revision = 12, .root = leafOf(paneID, 1, 1, 0), .zoomedPane = std::nullopt });
    CHECK_EQ(seat.client.topology()->revision, 40u);

    seat.client.transportLost(seat.now);
    seat.table.noteTransportLost(seat.client.held()->attachmentID, 1, seat.now);
    seat.accept(UbiquittyTest::syncHostPolicy(0), 2);
    (void)seat.client.connect(seat.now);
    // The tree the lost connection left still stands.
    REQUIRE(seat.client.topology().has_value());
    CHECK_EQ(seat.client.topology()->revision, 40u);
    seat.settle();
    REQUIRE(seat.client.state() == ClientAttachmentState::Live);
    seat.offer(3);
    seat.settle();
    CHECK_EQ(seat.client.topology()->revision, 3u);
}

UBIQUITTY_TEST_MAIN()
