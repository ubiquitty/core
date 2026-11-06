// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Session/Presence.h"

using namespace Ubiquitty;
namespace Sync = Protocol::Sync;

namespace {

constexpr Sync::ID128 paneA { .low = 31, .high = 77 };
constexpr Sync::ID128 paneB { .low = 32, .high = 77 };

Sync::AttachmentEntry row(std::uint64_t identity)
{
    Sync::AttachmentEntry value;
    value.attachmentID = { .low = identity, .high = 91 };
    value.clientKind = Sync::ClientKind::Agent;
    value.panes = { paneA, paneB };
    value.presence = Sync::AttachmentPresence {
        .clientID = 23,
        .processID = 41,
        .process = "/verified/process",
        .claimedLabel = "claimed helper",
        .grantIdentity = "token:verified-digest",
        .isQUIC = true,
        .sharedPanes = { { paneB, Sync::PresenceGrant::Always }, { paneA, Sync::PresenceGrant::Session } },
    };
    return value;
}

} // namespace

TEST(canonicalAttachmentsProjectOnlyLiveRowsSharingTheExactPane)
{
    Sync::AttachmentsList list { .revision = 1, .attachments = {} };
    auto self = row(1);
    self.clientKind = Sync::ClientKind::Human;
    self.isSelf = true;
    auto unrelated = row(2);
    unrelated.panes = { paneB };
    auto lost = row(3);
    lost.state = Sync::AttachmentState::Lost;
    lost.presence.reset();
    auto observer = row(4);
    observer.clientKind = Sync::ClientKind::Viewer;
    observer.role = Sync::AttachmentRole::Observer;
    list.attachments = { self, unrelated, lost, observer };
    const auto snapshot = Session::presenceForPane(list.attachments, paneA, 31);
    CHECK(snapshot.sessionID == 31);
    REQUIRE(snapshot.attendees.size() == 2);
    CHECK(snapshot.attendees[0].attachmentID == self.attachmentID);
    CHECK((snapshot.attendees[0].flags & Session::presenceSelf) != 0);
    CHECK(snapshot.attendees[1].attachmentID == observer.attachmentID);
    CHECK((snapshot.attendees[1].flags & Session::presenceHuman) != 0);
    CHECK((snapshot.attendees[1].flags & Session::presenceSelf) == 0);
    CHECK(Session::presenceForPane(list.attachments, { .low = 31, .high = 78 }, 31).attendees.empty());
}

TEST(oneAttachmentCanHaveDifferentGrantsOnTwoSharedPanes)
{
    const Sync::AttachmentsList list { .revision = 2, .attachments = { row(2) } };
    const auto first = Session::presenceForPane(list.attachments, paneA, 31);
    const auto second = Session::presenceForPane(list.attachments, paneB, 32);
    REQUIRE(first.attendees.size() == 1 && second.attendees.size() == 1);
    CHECK(first.attendees[0].attachmentID == second.attendees[0].attachmentID);
    CHECK(first.attendees[0].grantScope == Session::grantSession);
    CHECK(second.attendees[0].grantScope == Session::grantAlways);
}

TEST(presenceProjectionKeepsGrantSelectorsSeparateFromDisplayFacts)
{
    auto value = row(3);
    value.principal = "owner-only-principal";
    value.presence->claimedLabel = "token:spoofed-claim";
    const Sync::AttachmentsList list { .revision = 3, .attachments = { value } };
    const auto snapshot = Session::presenceForPane(list.attachments, paneA, 31);
    REQUIRE(snapshot.attendees.size() == 1);
    const auto& actual = snapshot.attendees[0];
    CHECK(actual.attachmentID == value.attachmentID);
    CHECK(actual.clientID == 23 && actual.peerProcessID == 41);
    CHECK(actual.process == "/verified/process");
    CHECK(actual.label == "token:spoofed-claim");
    CHECK(actual.grantIdentity == "token:verified-digest");
    CHECK((actual.flags & Session::presenceQUIC) != 0);
    CHECK((actual.flags & Session::presenceHuman) == 0);
}

TEST(unavailablePresenceMetadataNeverInventsAnAuthoritySelectorOrHidesPeers)
{
    auto first = row(4);
    auto second = row(5);
    first.presence.reset();
    second.presence.reset();
    const Sync::AttachmentsList list { .revision = 4, .attachments = { first, second } };
    const auto snapshot = Session::presenceForPane(list.attachments, paneA, 31);
    REQUIRE(snapshot.attendees.size() == 2);
    for (std::size_t index = 0; index < snapshot.attendees.size(); ++index) {
        const auto& actual = snapshot.attendees[index];
        CHECK(actual.attachmentID == (index ? second.attachmentID : first.attachmentID));
        CHECK(actual.clientID == 0 && actual.peerProcessID == 0);
        CHECK(actual.process.empty() && actual.label.empty() && actual.grantIdentity.empty());
        CHECK(actual.grantScope == UINT8_MAX);
    }
}

UBIQUITTY_TEST_MAIN()
