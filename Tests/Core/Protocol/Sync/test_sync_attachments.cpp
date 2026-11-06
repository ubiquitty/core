// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_attachments — the attachments notify (Docs/sync.md §6.10 "Attachments
// are visible", §7.3): every client is told which attachments share its
// panes, what each is, whether it is live, and which owns the size and is the
// primary; only an Owner is told principals; an attachment of another pane is not
// listed; and a list is sent only when it changed.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;

namespace {

/// One client's end and the host connection that serves it, on a table and a
/// pane the test owns.
struct Seat {
    std::unique_ptr<HostConnection> connection;
    ClientAttachment client;
    /// Every notify the host sent this client, as text.
    std::vector<std::string> heard;
    /// The host's entire stream, including the handshake and transfers.
    std::vector<std::uint8_t> received;

    Seat(AttachmentTable& table, UbiquittyTest::OwnedPane& pane, ID128 instance, std::uint64_t number, const ConnectionAuthority& authority = owner, ClientKind kind = ClientKind::Human, Role role = Role::Owner)
        : client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), identityOf(instance, kind, role))
    {
        connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
        connection->serveFromTable(table, authority, number, [&pane](ID128 id) { return id == pane.source->paneID() ? pane.source.get() : nullptr; });
    }

    static ClientIdentity identityOf(ID128 instance, ClientKind kind, Role role)
    {
        ClientIdentity identity = clientIdentity();
        identity.clientInstance = instance;
        identity.kind = kind;
        identity.requestedRole = role;
        return identity;
    }

    /// Runs the host's pass and the client's read once, as their owners do:
    /// the host serves its notifies before it services the connection, and
    /// the client reads them after its read.
    void pump(std::uint64_t now)
    {
        serveNotifies(*connection);
        connection->service(now);
        std::vector<std::uint8_t> down;
        while (connection->popStreamFrame(down, now)) {
        }
        received.insert(received.end(), down.begin(), down.end());
        FrameDecoder decoder;
        decoder.append(down);
        while (const std::optional<Frame> frame = decoder.next()) {
            if (frame->kind == FrameKind::Notify) {
                if (const std::optional<NotifyHead> notify = decodeNotifyHead(frame->body))
                    heard.emplace_back(notify->record.begin(), notify->record.end());
            }
        }
        client.receiveStream(down, now);
        client.tick(now);
        (void)readNotifies(client);
        connection->receiveStream(client.takeStreamOutput(), now);
    }

    /// Greets and binds the pane.
    void attach(UbiquittyTest::OwnedPane& pane, std::uint64_t now)
    {
        (void)client.connect(now);
        pump(now);
        pump(now);
        (void)connection->bindPane(*pane.source);
    }

    [[nodiscard]] const AttachmentEntry* entry(ID128 attachmentID) const
    {
        const std::span<const AttachmentEntry> entries = client.attachments();
        const auto found = std::ranges::find(entries, attachmentID, &AttachmentEntry::attachmentID);
        return found == entries.end() ? nullptr : &*found;
    }
};

/// Pumps every seat each millisecond for a span.
void pumpAll(std::span<Seat* const> seats, std::uint64_t& now, std::uint64_t span)
{
    for (const std::uint64_t until = now + span; now < until; now += 1'000) {
        for (Seat* seat : seats)
            seat->pump(now);
    }
}

} // namespace

// A second client that shows the same pane is listed to the first, and its
// loss and its going are told as they happen.
TEST(everyClientLearnsOfASecondAttachmentAndOfItsLoss)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    a.attach(pane, now);
    Seat* const alone[] { &a };
    pumpAll(alone, now, 50'000);
    REQUIRE(a.client.held().has_value());
    const ID128 aID = a.client.held()->attachmentID;
    REQUIRE(a.client.attachments().size() == 1);
    CHECK(a.client.attachments()[0].attachmentID == aID);
    // The first client types, so the size is its own whatever the second
    // does: what follows moves nothing but the second's state.
    REQUIRE(a.client.send(paneID, raw("a"), now).has_value());
    pumpAll(alone, now, 20'000);

    Seat b(table, pane, { 70, 80 }, 2);
    b.attach(pane, now);
    Seat* const both[] { &a, &b };
    pumpAll(both, now, 50'000);
    CHECK_EQ(a.client.attachments().size(), 2u);
    REQUIRE(b.client.held().has_value());
    const ID128 bID = b.client.held()->attachmentID;
    REQUIRE(a.entry(bID) != nullptr);
    CHECK(a.entry(bID)->state == AttachmentState::Live);
    CHECK(a.entry(bID)->role == AttachmentRole::Driver);
    CHECK(a.entry(bID)->clientKind == ClientKind::Human);
    CHECK_EQ(b.client.attachments().size(), 2u);

    // The second client's transport goes: Lost, and still listed.
    table.noteTransportLost(bID, 2, now);
    b.connection.reset();
    pumpAll(alone, now, 20'000);
    REQUIRE(a.entry(bID) != nullptr);
    CHECK(a.entry(bID)->state == AttachmentState::Lost);

    // It resumes on another connection: Live again.
    b.client.transportLost(now);
    b.connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
    b.connection->serveFromTable(table, owner, 5, [&pane](ID128 id) { return id == pane.source->paneID() ? pane.source.get() : nullptr; });
    (void)b.client.connect(now);
    pumpAll(both, now, 100'000);
    REQUIRE(b.client.state() == ClientAttachmentState::Live);
    REQUIRE(b.client.held()->attachmentID == bID);
    REQUIRE(a.entry(bID) != nullptr);
    CHECK(a.entry(bID)->state == AttachmentState::Live);
    table.noteTransportLost(bID, 5, now);
    b.connection.reset();
    pumpAll(alone, now, 20'000);
    REQUIRE(a.entry(bID) != nullptr);
    CHECK(a.entry(bID)->state == AttachmentState::Lost);

    // Past its grace it is freed, and no longer listed.
    now += measuredResumePolicy().lostGraceMicroseconds + 1;
    table.expire(now);
    pumpAll(alone, now, 20'000);
    CHECK_EQ(a.client.attachments().size(), 1u);
    CHECK(a.entry(bID) == nullptr);
}

TEST(onlyAnOwnerReceivesEveryAttachmentsPrincipal)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    const ConnectionAuthority other { .principal = "mallory-the-principal", .mayDrive = false, .verifiedKind = ClientKind::Agent };
    Seat b(table, pane, { 70, 80 }, 2, other, ClientKind::Agent);
    const ConnectionAuthority observer { .principal = "observer", .mayDrive = false, .verifiedKind = ClientKind::Human };
    Seat c(table, pane, { 71, 81 }, 3, observer, ClientKind::Human, Role::Viewer);
    const ConnectionAuthority driver { .principal = "driver", .mayDrive = true, .verifiedKind = ClientKind::Human };
    Seat d(table, pane, { 72, 82 }, 4, driver, ClientKind::Human, Role::Driver);
    a.attach(pane, now);
    b.attach(pane, now);
    c.attach(pane, now);
    d.attach(pane, now);
    Seat* const seats[] { &a, &b, &c, &d };
    pumpAll(seats, now, 50'000);
    REQUIRE(!a.heard.empty());
    REQUIRE(!b.heard.empty());
    for (const Seat* seat : { &b, &c, &d }) {
        REQUIRE(!seat->heard.empty());
        for (const std::string& text : seat->heard) {
            CHECK(!text.contains("principal"));
            CHECK(!text.contains("alice"));
            CHECK(!text.contains("mallory"));
        }
        const std::string stream(seat->received.begin(), seat->received.end());
        CHECK(!stream.contains("alice"));
        CHECK(std::ranges::none_of(seat->client.attachments(), [](const AttachmentEntry& entry) { return entry.principal.has_value(); }));
    }
    CHECK_EQ(a.client.attachments().size(), 4u);
    CHECK(std::ranges::all_of(a.client.attachments(), [](const AttachmentEntry& entry) { return entry.principal.has_value(); }));
    CHECK(a.heard.back().contains("alice"));
    CHECK(a.heard.back().contains("mallory-the-principal"));
    CHECK(a.heard.back().contains("observer"));
    CHECK(a.heard.back().contains("driver"));
    // What it does say of the other: an agent that may not drive.
    REQUIRE(b.client.held().has_value());
    const AttachmentEntry* listed = a.entry(b.client.held()->attachmentID);
    REQUIRE(listed != nullptr);
    CHECK(listed->role == AttachmentRole::Observer);
    CHECK(listed->clientKind == ClientKind::Agent);
}

// The list names who owns the pane's size and who is its primary, and says
// so again when typing moves them.
TEST(theNotifyNamesTheGeometryOwnerAndThePrimary)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    Seat b(table, pane, { 70, 80 }, 2);
    a.attach(pane, now);
    b.attach(pane, now);
    Seat* const both[] { &a, &b };
    pumpAll(both, now, 50'000);
    const ID128 aID = a.client.held()->attachmentID;
    const ID128 bID = b.client.held()->attachmentID;

    REQUIRE(b.client.send(paneID, raw("b"), now).has_value());
    pumpAll(both, now, 50'000);
    for (const Seat* seat : both) {
        REQUIRE(seat->entry(aID) != nullptr);
        REQUIRE(seat->entry(bID) != nullptr);
        CHECK(seat->entry(bID)->isGeometryOwner);
        CHECK(seat->entry(bID)->isPrimary);
        CHECK(!seat->entry(aID)->isGeometryOwner);
        CHECK(!seat->entry(aID)->isPrimary);
    }

    REQUIRE(a.client.send(paneID, raw("a"), now).has_value());
    pumpAll(both, now, 50'000);
    for (const Seat* seat : both) {
        CHECK(seat->entry(aID)->isGeometryOwner);
        CHECK(seat->entry(aID)->isPrimary);
        CHECK(!seat->entry(bID)->isGeometryOwner);
        CHECK(!seat->entry(bID)->isPrimary);
    }

    // The owner's transport goes: it still owns the size, and the primary
    // falls to the other.
    table.noteTransportLost(aID, 1, now);
    a.connection.reset();
    Seat* const left[] { &b };
    pumpAll(left, now, 20'000);
    REQUIRE(b.entry(aID) != nullptr);
    CHECK(b.entry(aID)->isGeometryOwner);
    CHECK(!b.entry(aID)->isPrimary);
    CHECK(b.entry(bID)->isPrimary);
}

// A client is told of the attachments that share a pane with it and of no
// other: who else is on the host is not its to know.
TEST(anAttachmentOfAnotherPaneIsNotListed)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    Ubiquitty::Terminal elsewhereTerminal;
    Ubiquitty::resize(elsewhereTerminal, 24, 80);
    PaneSource elsewhere(elsewhereTerminal, ID128 { 90, 91 });
    elsewhere.publish(FrameClass::Structural, 1);
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    a.attach(pane, now);
    Seat* const alone[] { &a };
    pumpAll(alone, now, 50'000);
    const std::uint64_t sent = a.connection->counters().attachmentsNotifiesSent;
    REQUIRE(sent >= 1);

    // A third client, bound to another pane alone.
    Seat c(table, pane, { 71, 81 }, 3);
    (void)c.client.connect(now);
    c.pump(now);
    c.pump(now);
    (void)c.connection->bindPane(elsewhere);
    Seat* const both[] { &a, &c };
    pumpAll(both, now, 50'000);
    REQUIRE(c.client.held().has_value());
    CHECK_EQ(a.client.attachments().size(), 1u);
    CHECK(a.entry(c.client.held()->attachmentID) == nullptr);
    CHECK_EQ(c.client.attachments().size(), 1u);
    CHECK(c.entry(a.client.held()->attachmentID) == nullptr);
    // What changed was not the first client's to hear of: it was sent nothing.
    CHECK_EQ(a.connection->counters().attachmentsNotifiesSent, sent);
}

// A pane a client binds later, with no attachment made or lost meanwhile,
// lists it the attachments that hold that pane, and unbinding it takes them
// off the list again. Two agents: neither owns a size, so nothing else moves.
TEST(bindingAndUnbindingAPaneChangesTheList)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    Ubiquitty::Terminal elsewhereTerminal;
    Ubiquitty::resize(elsewhereTerminal, 24, 80);
    PaneSource elsewhere(elsewhereTerminal, ID128 { 90, 91 });
    elsewhere.publish(FrameClass::Structural, 1);
    const ConnectionAuthority automation { .principal = "alice", .mayDrive = false, .verifiedKind = ClientKind::Agent };
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1, automation, ClientKind::Agent);
    a.attach(pane, now);
    Seat c(table, pane, { 71, 81 }, 3, automation, ClientKind::Agent);
    (void)c.client.connect(now);
    c.pump(now);
    c.pump(now);
    (void)c.connection->bindPane(elsewhere);
    Seat* const both[] { &a, &c };
    pumpAll(both, now, 50'000);
    REQUIRE(a.client.held().has_value());
    REQUIRE(c.client.held().has_value());
    const ID128 cID = c.client.held()->attachmentID;
    REQUIRE(a.entry(cID) == nullptr);
    REQUIRE(!elsewhere.geometry().status().owner.has_value());

    const ID128 aID = a.client.held()->attachmentID;
    const std::uint32_t handle = a.connection->bindPane(elsewhere);
    REQUIRE(handle != 0);
    pumpAll(both, now, 50'000);
    CHECK(a.entry(cID) != nullptr);
    CHECK_EQ(a.client.attachments().size(), 2u);
    // The client that already showed the pane is told of the newcomer.
    CHECK(c.entry(aID) != nullptr);

    a.connection->unbindPane(handle, 0);
    pumpAll(both, now, 50'000);
    CHECK(a.entry(cID) == nullptr);
    CHECK(c.entry(aID) == nullptr);
    CHECK_EQ(c.client.attachments().size(), 1u);
}

// A client is listed to itself as soon as it is welcomed, before it shows
// any pane.
TEST(aClientIsListedItselfBeforeItShowsAPane)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    (void)a.client.connect(now);
    Seat* const alone[] { &a };
    pumpAll(alone, now, 20'000);
    REQUIRE(a.client.state() == ClientAttachmentState::Live);
    REQUIRE(a.client.held().has_value());
    REQUIRE(a.client.attachments().size() == 1);
    CHECK(a.client.attachments()[0].attachmentID == a.client.held()->attachmentID);
    CHECK(!a.client.attachments()[0].isGeometryOwner);
}

// A list is sent when it changed and not otherwise: an owner typing on, a
// screen changing and idle passes cost no notify, and each one sent carries a
// higher revision.
TEST(aNotifyIsSentOnlyWhenTheListChanges)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    a.attach(pane, now);
    Seat* const alone[] { &a };
    REQUIRE(a.client.send(paneID, raw("a"), now).has_value());
    pumpAll(alone, now, 50'000);
    const std::uint64_t sent = a.connection->counters().attachmentsNotifiesSent;
    const std::uint64_t revision = a.client.attachmentsRevision();
    REQUIRE(sent >= 1);
    REQUIRE(revision >= 1);

    for (int key = 0; key < 20; ++key) {
        REQUIRE(a.client.send(paneID, raw("k"), now).has_value());
        UbiquittyTest::feed(pane.terminal, "k");
        pane.source->publish(FrameClass::Settled, now);
        pumpAll(alone, now, 5'000);
    }
    pumpAll(alone, now, 100'000);
    CHECK_EQ(a.connection->counters().attachmentsNotifiesSent, sent);
    CHECK_EQ(a.client.attachmentsRevision(), revision);

    Seat b(table, pane, { 70, 80 }, 2);
    b.attach(pane, now);
    Seat* const both[] { &a, &b };
    pumpAll(both, now, 50'000);
    CHECK(a.connection->counters().attachmentsNotifiesSent > sent);
    CHECK(a.client.attachmentsRevision() > revision);
}

// The list a lost connection left stands until the next connection's first,
// which replaces it whatever its revision: another host instance numbers its
// lists from 1.
TEST(aNewConnectionsFirstListReplacesTheOneHeld)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    std::uint64_t now = 1;
    Seat a(table, pane, { 7, 8 }, 1);
    a.attach(pane, now);
    Seat* const alone[] { &a };
    pumpAll(alone, now, 50'000);
    REQUIRE(a.client.attachments().size() == 1);
    a.client.noteAttachments({ .revision = 900, .attachments = {} });
    REQUIRE(a.client.attachments().empty());
    // A list older than the one held is dropped.
    a.client.noteAttachments({ .revision = 5, .attachments = { AttachmentEntry {} } });
    CHECK(a.client.attachments().empty());

    // The same attachment on another connection: its first list is taken.
    a.client.transportLost(now);
    table.noteTransportLost(a.client.held()->attachmentID, 1, now);
    a.connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = UbiquittyTest::syncDefaultSelection() });
    a.connection->serveFromTable(table, owner, 4, [&pane](ID128 id) { return id == pane.source->paneID() ? pane.source.get() : nullptr; });
    (void)a.client.connect(now);
    CHECK(a.client.attachments().empty());
    pumpAll(alone, now, 100'000);
    REQUIRE(a.client.state() == ClientAttachmentState::Live);
    CHECK_EQ(a.client.attachments().size(), 1u);
    CHECK(a.client.attachmentsRevision() < 900);
}

// The table's revision moves with everything a list shows of an attachment:
// its state, and the panes it holds. Making one moves nothing: it shows nobody
// anything until it takes a pane.
TEST(theTablesRevisionMovesWithWhatAListShows)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    const std::uint64_t empty = table.revision();
    const AttachmentRecord& made = table.create({ 1, 0 }, ClientKind::Human, owner, 1, 1);
    const ID128 id = made.attachmentID;
    const ID128 secret = made.resumeSecret;
    CHECK_EQ(table.revision(), empty);

    table.noteHoldingChanged();
    const std::uint64_t held = table.revision();
    CHECK(held > empty);

    table.noteTransportLost(id, 1, 10);
    const std::uint64_t lost = table.revision();
    CHECK(lost > held);
    // Losing what is already lost changes nothing.
    table.noteTransportLost(id, 1, 11);
    CHECK_EQ(table.revision(), lost);

    REQUIRE(table.resume({ .attachmentID = id, .resumeSecret = secret, .panes = {} }, owner, 2, 12).has_value());
    const std::uint64_t resumed = table.revision();
    CHECK(resumed > lost);
    // A refused resume changes nothing.
    CHECK(!table.resume({ .attachmentID = id, .resumeSecret = secret, .panes = {} }, owner, 3, 13).has_value());
    CHECK_EQ(table.revision(), resumed);

    table.noteDetached(id);
    const std::uint64_t detached = table.revision();
    CHECK(detached > resumed);

    const ID128 second = table.create({ 2, 0 }, ClientKind::Human, owner, 4, 14).attachmentID;
    table.noteTransportLost(second, 4, 15);
    const std::uint64_t secondLost = table.revision();
    table.expire(15 + measuredResumePolicy().lostGraceMicroseconds);
    CHECK(table.revision() > secondLost);
    CHECK(table.find(second) == nullptr);
}

// A Lost attachment is kept for a client that may come back. When the same
// principal's same client instance holds a newer attachment it has come
// back, by another door: a proof that only ran out of time, then a login.
// The older one is freed, whichever of the two happens first, so it holds no
// geometry, tag or grant beside the one in use. Another principal's
// attachment, another client's, and one still served are left alone.
TEST(aLostAttachmentItsClientReplacedIsFreed)
{
    SequenceSecrets secrets;
    const ConnectionAuthority other { .principal = "bob", .mayDrive = true, .verifiedKind = ClientKind::Human };
    const ID128 instance { 7, 8 };

    // Lost first, then replaced.
    {
        AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
        const ID128 old = table.create(instance, ClientKind::Human, owner, 1, 10).attachmentID;
        const ID128 bystander = table.create({ 9, 9 }, ClientKind::Human, owner, 2, 10).attachmentID;
        const ID128 foreign = table.create(instance, ClientKind::Human, other, 3, 10).attachmentID;
        table.noteTransportLost(old, 1, 20);
        table.noteTransportLost(bystander, 2, 20);
        table.noteTransportLost(foreign, 3, 20);
        (void)table.takeFreed();
        const ID128 fresh = table.create(instance, ClientKind::Human, owner, 4, 30).attachmentID;
        CHECK(table.find(old) == nullptr);
        CHECK(table.find(fresh) != nullptr);
        CHECK(table.find(bystander) != nullptr);
        CHECK(table.find(foreign) != nullptr);
        const std::vector<AttachmentTable::Freed> freed = table.takeFreed();
        REQUIRE(freed.size() == 1);
        CHECK(freed[0].attachmentID == old);
        CHECK(freed[0].clientInstance == instance);
    }

    // Replaced first, while the host still serves the old transport, then
    // found Lost.
    {
        AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
        const ID128 old = table.create(instance, ClientKind::Human, owner, 1, 10).attachmentID;
        const ID128 fresh = table.create(instance, ClientKind::Human, owner, 2, 20).attachmentID;
        CHECK(table.find(old) != nullptr);
        CHECK(table.takeFreed().empty());
        const std::uint64_t revision = table.revision();
        table.noteTransportLost(old, 1, 30);
        CHECK(table.find(old) == nullptr);
        CHECK(table.find(fresh) != nullptr);
        CHECK(table.revision() > revision);
        REQUIRE(table.takeFreed().size() == 1);
        // The newer one, lost in its turn, is kept: nothing replaced it.
        table.noteTransportLost(fresh, 2, 40);
        REQUIRE(table.find(fresh) != nullptr);
        CHECK(table.find(fresh)->state == AttachmentState::Lost);
        CHECK(table.takeFreed().empty());
    }

    // Two served at once, as two windows restored from one state are: the
    // newer, lost first, is kept, since an older attachment replaced nothing.
    {
        AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
        const ID128 old = table.create(instance, ClientKind::Human, owner, 1, 10).attachmentID;
        const ID128 fresh = table.create(instance, ClientKind::Human, owner, 2, 20).attachmentID;
        table.noteTransportLost(fresh, 2, 30);
        REQUIRE(table.find(fresh) != nullptr);
        CHECK(table.find(fresh)->state == AttachmentState::Lost);
        CHECK(table.find(old) != nullptr);
        CHECK(table.takeFreed().empty());
    }
}

// Current Attachments Notify codec and strict refusal cases live in test_sync_notify.cpp.

TEST(oversizedPresenceOmitsAllEnrichmentWithoutHidingAnyBaseAttachment)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    Seat a(table, pane, { 7, 8 }, 1);
    Seat b(table, pane, { 70, 80 }, 2);
    a.attach(pane, 1);
    b.attach(pane, 1);
    a.connection->setAttachmentPresenceProvider([](ID128, std::span<const ID128> panes, bool isSelf) -> std::optional<AttachmentPresence> {
        AttachmentPresence value { .clientID = 7, .processID = 23, .process = isSelf ? std::string(40'000, 'p') : "/verified/tool", .claimedLabel = "claim", .grantIdentity = isSelf ? std::string(40'000, 'g') : "token:digest", .isQUIC = false, .sharedPanes = {} };
        for (const auto shared : panes)
            value.sharedPanes.push_back({ shared, PresenceGrant::Session });
        return value;
    });
    const auto list = a.connection->owedAttachments();
    REQUIRE(list);
    REQUIRE(list->attachments.size() == 2);
    CHECK(std::ranges::none_of(list->attachments, [](const AttachmentEntry& entry) { return entry.presence.has_value(); }));
    CHECK(std::ranges::all_of(list->attachments, [](const AttachmentEntry& entry) { return entry.principal.has_value(); }));
    for (const auto& entry : list->attachments)
        CHECK(entry.panes == std::vector { pane.source->paneID() });
    REQUIRE(attachmentsNotify(*list));
    a.connection->noteAttachmentsSent(*list);
    CHECK(!a.connection->owedAttachments());
    CHECK(a.connection->state() == ConnectionState::Attached);
}

TEST(presentationChangesReconsiderTheSingleAttachmentsCacheWithoutNewTableAuthority)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    Seat seat(table, pane, { 7, 8 }, 1);
    seat.attach(pane, 1);
    const auto base = seat.connection->owedAttachments();
    REQUIRE(base);
    seat.connection->noteAttachmentsSent(*base);
    const auto tableRevision = table.revision();
    seat.connection->setAttachmentPresenceProvider([](ID128, std::span<const ID128> panes, bool) -> std::optional<AttachmentPresence> {
        AttachmentPresence value { .clientID = 7, .processID = 0, .process = {}, .claimedLabel = "claim", .grantIdentity = "token:digest", .isQUIC = true, .sharedPanes = {} };
        for (const auto shared : panes)
            value.sharedPanes.push_back({ shared, PresenceGrant::Always });
        return value;
    });
    const auto enriched = seat.connection->owedAttachments();
    REQUIRE(enriched && enriched->attachments.size() == 1);
    CHECK(enriched->attachments[0].presence.has_value());
    CHECK(enriched->revision > base->revision);
    CHECK(table.revision() == tableRevision);
    CHECK(enriched->attachments[0].attachmentID == base->attachments[0].attachmentID);
    CHECK(enriched->attachments[0].panes == base->attachments[0].panes);
    seat.connection->noteAttachmentsSent(*enriched);
    seat.connection->noteAttachmentsChanged();
    CHECK(!seat.connection->owedAttachments());
}

TEST(anOversizedMandatoryRosterClosesWithoutCachingItAsSent)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    UbiquittyTest::OwnedPane pane;
    Seat seat(table, pane, { 7, 8 }, 1);
    seat.attach(pane, 1);
    const auto sentBefore = seat.connection->counters().attachmentsNotifiesSent;
    ConnectionAuthority enormous { .principal = std::string(maxFrameBodyBytes, 'p'), .mayDrive = false, .verifiedKind = ClientKind::Agent };
    auto& other = table.create({ 70, 80 }, ClientKind::Agent, std::move(enormous), 2, 1);
    (void)other.paneFor(pane.source->paneID());
    CHECK(!seat.connection->owedAttachments());
    CHECK(seat.connection->state() == ConnectionState::Closed);
    CHECK(seat.connection->byeReason() == ByeReason::Oversize);
    CHECK(seat.connection->counters().attachmentsNotifiesSent == sentBefore);
    CHECK(!seat.connection->owedAttachments());
}

UBIQUITTY_TEST_MAIN()

TEST(eachRetirementNotifiesItsOwnerOnce)
{
    SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, { .lostGraceMicroseconds = 100 }, secrets);
    std::vector<ID128> retired;
    table.setRetireRecord([&](ID128 identity) { retired.push_back(identity); });
    const ID128 detached = table.create({ 1, 1 }, ClientKind::Human, owner, 1, 1).attachmentID;
    table.noteDetached(detached);
    table.noteDetached(detached);
    CHECK_EQ(std::ranges::count(retired, detached), 1);

    const ID128 expired = table.create({ 2, 1 }, ClientKind::Human, owner, 2, 1).attachmentID;
    table.noteTransportLost(expired, 2, 1);
    table.expire(100);
    CHECK_EQ(std::ranges::count(retired, expired), 0);
    table.expire(101);
    table.expire(102);
    CHECK_EQ(std::ranges::count(retired, expired), 1);

    const ID128 superseded = table.create({ 3, 1 }, ClientKind::Human, owner, 3, 103).attachmentID;
    table.noteTransportLost(superseded, 3, 103);
    (void)table.create({ 3, 1 }, ClientKind::Human, owner, 4, 104);
    table.noteDetached(superseded);
    CHECK_EQ(std::ranges::count(retired, superseded), 1);
    CHECK_EQ(retired.size(), 3u);
}

TEST(handoverRetiresAnOldIssuerThroughTheOwnerOnce)
{
    SequenceSecrets secrets;
    AttachmentTable predecessor({ 41, 42 }, measuredResumePolicy(), secrets);
    const ID128 attachment = predecessor.create({ 1, 1 }, ClientKind::Human, owner, 1, 1).attachmentID;
    auto carried = predecessor.exportForHandover(2);
    carried.inheritedInstances = { { 100, 200 }, { 101, 201 }, { 102, 202 }, { 103, 203 },
        { 104, 204 }, { 105, 205 }, { 106, 206 }, { 107, 207 } };
    carried.records.front()->issuer = carried.inheritedInstances.front();
    AttachmentTable successor({ 51, 52 }, measuredResumePolicy(), secrets);
    std::vector<ID128> retired;
    successor.setRetireRecord([&](ID128 identity) { retired.push_back(identity); });
    REQUIRE(successor.inherit(std::move(carried), { 41, 42 }, 3));
    CHECK(!successor.find(attachment));
    successor.noteDetached(attachment);
    REQUIRE(retired.size() == 1);
    CHECK(retired.front() == attachment);
}
