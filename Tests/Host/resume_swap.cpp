// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/resume_swap.cpp — the daemon keeps a client's attachment across
// its connections (Docs/sync.md §6.9).
//
//   1. A client attaches under a token and reads the pane. The same client on
//      a second connection resumes A's attachment while A is still open: the
//      pane continues with a diff, A is told Superseded and is closing.
//   2. A client under another token presenting the attachment and its secret
//      is refused, and the serving connection still gets updates.
//   3. A connection that is dropped leaves its attachment Lost; past the
//      grace the loop's expiry forgets it and a resume is refused.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), the QUIC-off host build as
// screen_lost_silence. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdint>
#include <cstdio>
#include <optional>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// What a client holds of the session's pane, as its resume names it.
Sync::ResumeRequest resumeOf(Sync::ReplicaSet& replicas, Sync::ID128 paneID, const Sync::Welcome& welcome)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    if (Sync::ReplicaSet::Held* held = replicas.find(paneID))
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live });
    return request;
}

/// Attaches a person under a token on a new connection.
///
/// The bearer is marked an interactive user, as an interactive-user bootstrap
/// token marks a person dialling in: a plain token holder is an agent, and an
/// agent with nobody attached to consent is refused before any resume is read.
std::size_t attachUnder(Daemon& d, std::uint32_t sessionID, const char* token)
{
    const std::size_t index = Screen::attach(d, sessionID);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::setTokenID(d, index, token);
    Screen::markInteractiveUser(d, index);
    return index;
}

void write(Daemon& d, auto& session, const char* text)
{
    Ubiquitty::processBytes(session.term.get(), text);
    session.isFrameDue = true;
    Screen::publish(d, session);
}

void run()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    const Sync::ID128 paneID = session.screenSource->paneID();
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    Sync::ReplicaSet replicas;
    const std::size_t aIndex = attachUnder(d, session.id, "alice");
    UbiquittyTest::ScreenClient a(24, 80, replicas, std::nullopt);
    Screen::greet(d, aIndex, a);
    write(d, session, "first\r\n");
    Screen::deliver(d, aIndex, a);
    REQUIRE(a.connection().welcome().has_value());
    const Sync::Welcome welcomeA = *a.connection().welcome();
    CHECK_MSG(a.generation() == session.screenSource->generation(), "premise: A does not hold the pane");
    a.storeReplicas();

    // 2. Another principal with A's attachment and secret is refused.
    const std::size_t mIndex = attachUnder(d, session.id, "mallory");
    Sync::ReplicaSet malloryReplicas;
    UbiquittyTest::ScreenClient m(24, 80, malloryReplicas, resumeOf(replicas, paneID, welcomeA));
    Screen::greet(d, mIndex, m);
    CHECK_MSG(!Screen::isLeaving(d, aIndex), "a resume under another token closed the serving connection");
    CHECK_MSG(Screen::connection(d, mIndex)->resumeRefusal() == std::optional { Sync::ResumeRefusal::OtherPrincipal }, "a resume under another token was not refused as another principal's");
    // A refused resume attaches fresh, as its own attachment.
    CHECK_MSG(m.connection().welcome() && m.connection().welcome()->attachmentID != welcomeA.attachmentID, "a refused resume was given A's attachment");
    write(d, session, "still served\r\n");
    Screen::deliver(d, aIndex, a);
    CHECK_MSG(a.generation() == session.screenSource->generation(), "the serving connection stopped getting updates after a refused resume");
    a.storeReplicas();

    // 1. The same token resumes on a second connection while A is open.
    write(d, session, "while resuming\r\n");
    const std::size_t bIndex = attachUnder(d, session.id, "alice");
    UbiquittyTest::ScreenClient b(24, 80, replicas, resumeOf(replicas, paneID, welcomeA));
    Screen::greet(d, bIndex, b);
    REQUIRE(b.connection().welcome().has_value());
    const Sync::Welcome& welcomeB = *b.connection().welcome();
    CHECK_MSG(welcomeB.attachmentID == welcomeA.attachmentID, "the resume did not continue A's attachment");
    REQUIRE(welcomeB.resumed.size() == 1);
    CHECK_MSG(welcomeB.resumed[0].outcome == Sync::ResumeOutcome::Diffed, "the resumed pane was not continued with a diff");
    CHECK_EQ(Screen::connection(d, bIndex)->counters().checkpointsSent, 0u);
    CHECK_MSG(b.generation() == session.screenSource->generation(), "the resumed client does not hold the pane");
    CHECK_MSG(Screen::isLeaving(d, aIndex), "the superseded connection is not closing");
    Screen::deliver(d, aIndex, a);
    CHECK_MSG(a.connection().byeReason() == std::optional { Sync::ByeReason::Superseded }, "the superseded client was not told Superseded");
    write(d, session, "after\r\n");
    Screen::deliver(d, bIndex, b);
    CHECK_MSG(b.generation() == session.screenSource->generation(), "the resumed connection is not served");

    // 3. A dropped connection leaves its attachment for the grace, no longer.
    b.storeReplicas();
    const Sync::Welcome welcomeAfter = *b.connection().welcome();
    const std::uint64_t droppedAt = Screen::now();
    Screen::drop(d, bIndex);
    const Sync::AttachmentRecord* lost = Screen::attachments(d).find(welcomeA.attachmentID);
    REQUIRE(lost != nullptr);
    CHECK_MSG(lost->state == Sync::AttachmentState::Lost, "a dropped connection's attachment is not Lost");
    const std::uint64_t grace = Sync::measuredResumePolicy().lostGraceMicroseconds;
    Screen::attachments(d).expire(droppedAt + grace / 2);
    CHECK_MSG(Screen::attachments(d).find(welcomeA.attachmentID) != nullptr, "an attachment was forgotten inside its grace");
    Screen::attachments(d).expire(Screen::now() + grace);
    CHECK_MSG(Screen::attachments(d).find(welcomeA.attachmentID) == nullptr, "an attachment outlived its grace");
    const std::size_t lateIndex = attachUnder(d, session.id, "alice");
    UbiquittyTest::ScreenClient late(24, 80, replicas, resumeOf(replicas, paneID, welcomeAfter));
    Screen::greet(d, lateIndex, late);
    CHECK_MSG(Screen::connection(d, lateIndex)->resumeRefusal() == std::optional { Sync::ResumeRefusal::UnknownAttachment }, "a resume past the grace was not refused");
    CHECK_MSG(Screen::hasSession(d, session.id), "losing the attachment ended the session");
}

} // namespace

int main()
{
    run();
    if (failures == 0)
        std::printf("resume_swap: a resume on a second connection continues the attachment with a diff and closes the first with Superseded, "
                    "another token's resume is refused without disturbing the served one, and a lost attachment is forgotten past its grace\n");
    return failures == 0 ? 0 : 1;
}
