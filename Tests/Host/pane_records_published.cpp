// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/pane_records_published.cpp — the daemon reads every pane's
// `PaneFacts` and `StatusRecords` from its terminal and what it knows beside
// it (Docs/sync.md §7.7).
//
//   1. Nothing is computed for a pane no connection reads: no records, no
//      process walk, no owed probe, and nothing owed for the loop to wake
//      for. A pane with a reader has current facts: its title, its status
//      set, the bell nobody saw, when it was created and where it came from.
//   2. A person who shows a pane is sent its facts ahead of the frame they
//      describe, and no status set: the screen carries the status.
//   3. Attention is seen by a person whose driver holds the pane's focus in
//      a focused window, and by nobody else: not an agent, not a pane that
//      gave the focus up when it was hidden, not an unfocused window. The
//      window taking the focus again clears what was latched.
//   4. An agent, which is sent no screen, is sent the status set.
//   5. The records are read on a budget: a second ask inside it is owed and
//      served when the window closes, and an owed foreground probe reads
//      them.
//   6. The activity stamps ride along with whatever else moves, and move the
//      revision by themselves at most once a second.
//   7. A pin names who pinned in the facts, and the session's end is in its
//      last facts.
//
// Friend seam, the QUIC-off host build, as geometry_follows_hands. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace OSC = Ubiquitty::OSC;

namespace {

std::uint64_t wallNow = 1'700'000'000'000;
std::uint64_t testWallClock() { return wallNow; }

/// The loop's clock the test asks for records at, each ask well past the
/// budget of the last unless a case says otherwise.
std::uint64_t loopNow = 10'000;
std::uint64_t later(std::uint64_t milliseconds = 5'000)
{
    loopNow += milliseconds;
    return loopNow;
}

} // namespace

int main()
{
    Daemon d;
    Screen::setWallClock(d, &testWallClock);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };
    const auto hostFacts = [](const auto& session) -> const Sync::PaneFacts& { return session.screenSource->facts(); };

    // 1. Nobody reads the pane's facts, so nothing is computed for it: no
    // records, no process walk, and nothing owed for the loop to wake for.
    auto& detached = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    detached.createdUnix = 1'699'999'000;
    detached.adopted = true;
    Ubiquitty::processBytes(detached.term.get(), "\x1b]2;backup\x07\x1b]7501;id=job:state=working\x1b\\\x07");
    Screen::publish(d, detached);
    Screen::noteRecordsDue(d, detached, later());
    CHECK_MSG(hostFacts(detached).revision == 0 && detached.screenSource->status().revision == 0, "records were read for a pane nobody reads");
    CHECK_MSG(detached.paneFacts.processChain.empty() && detached.lastPaneFactsWalkMilliseconds == 0, "the process chain was walked for a pane nobody reads");
    CHECK_MSG(detached.lastRecordsMilliseconds == 0 && detached.recordsOwedAtMilliseconds == 0, "a pane nobody reads owes the loop a wake");
    detached.foregroundProbeOwedAtMilliseconds = 7;
    Screen::serveOwedForegroundProbe(d, detached, 8);
    CHECK_MSG(hostFacts(detached).revision == 0 && detached.paneFacts.processChain.empty() && detached.lastPaneFactsWalkMilliseconds == 0 && detached.foregroundProbeOwedAtMilliseconds == 0, "an owed probe did work for a pane nobody reads");

    // 2. A person shows a pane: facts ahead of the frame, and no status set.
    auto& shown = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    Screen::publish(d, shown, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t personIndex = Screen::attach(d, shown.id);
    Screen::grantScope(d, personIndex, driving);
    Screen::setTokenID(d, personIndex, "person");
    Screen::markInteractiveUser(d, personIndex);
    UbiquittyTest::ScreenClient person { 24, 80 };
    Screen::greet(d, personIndex, person);
    // The pane is the one the person's window has in front: it holds the focus.
    CHECK(person.setPaneFocus(Ubiquitty::Modes::FocusState::Focused));
    Screen::deliver(d, personIndex, person);
    Ubiquitty::processBytes(shown.term.get(), "\x1b]2;editing\x07\x1b]7501;id=job:state=blocked:kind=question\x1b\\\x07");
    Screen::publish(d, shown);
    Screen::publishRecords(d, shown, later());
    Screen::deliver(d, personIndex, person);
    const Sync::PaneFacts* seen = person.connection().paneFacts(1);
    CHECK_MSG(seen && seen->programTitle == "editing" && seen->revision == hostFacts(shown).revision, "a title set between two publishes did not arrive as PaneFacts");
    CHECK_MSG(person.count(Sync::FrameKind::StatusRecords) == 0, "a pane whose screen the client receives was sent a status set as well");
    const std::optional<Sync::StatusRecords> carried = person.connection().paneStatus(1);
    CHECK_MSG(carried && carried->store.records.size() == 1 && carried->store.records[0].state == OSC::ProgramState::Blocked, "the status report did not reach the client in the pane's screen");

    // 3. The person was looking: the bell latched nothing.
    CHECK_MSG(!hostFacts(shown).attention.hasUnseenBell && !Ubiquitty::unseenAttention(shown.term.get()).hasBell, "a bell rung in front of a person stayed unseen");
    CHECK_MSG(seen && !seen->attention.hasUnseenBell, "the person was told of a bell they saw ring");
    // Hidden, the pane gives the focus up; a bell is latched and the person
    // is told.
    CHECK(person.setPaneFocus(Ubiquitty::Modes::FocusState::Unfocused));
    person.setView(Sync::ViewState::Dormant);
    Screen::deliver(d, personIndex, person);
    Ubiquitty::processBytes(shown.term.get(), "\x07");
    Screen::publish(d, shown);
    Screen::publishRecords(d, shown, later());
    Screen::deliver(d, personIndex, person);
    CHECK_MSG(hostFacts(shown).attention.hasUnseenBell, "a bell behind a hidden pane was marked seen");
    CHECK_MSG(person.connection().paneFacts(1) && person.connection().paneFacts(1)->attention.hasUnseenBell, "a person was not told of the bell behind a pane they hid");
    // Hidden, the pane's status has no screen to ride: it comes as a set.
    CHECK_MSG(person.count(Sync::FrameKind::StatusRecords) == 1, "a pane hidden from its client was not sent its status set");
    // Shown again in an unfocused window, it is still unseen.
    person.setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    person.setView(Sync::ViewState::Live);
    CHECK(person.setPaneFocus(Ubiquitty::Modes::FocusState::Focused));
    Screen::deliver(d, personIndex, person);
    Screen::publishRecords(d, shown, later());
    CHECK_MSG(hostFacts(shown).attention.hasUnseenBell, "a bell in an unfocused window was marked seen");
    // The window takes the focus: what the client shows changed, the facts
    // are read again without any output, and the bell is seen.
    loopNow = 0;
    shown.lastRecordsMilliseconds = 0;
    person.setFocus(Ubiquitty::Modes::FocusState::Focused);
    Screen::deliver(d, personIndex, person);
    CHECK_MSG(!hostFacts(shown).attention.hasUnseenBell, "a person focusing the window did not see the bell");
    Screen::deliver(d, personIndex, person);
    CHECK_MSG(person.connection().paneFacts(1) && !person.connection().paneFacts(1)->attention.hasUnseenBell, "the person was not told the bell was seen");
    loopNow = 100'000;

    // 4. An agent is sent no screen, so it is sent the set; and it sees nothing.
    const std::size_t agentIndex = Screen::attach(d, detached.id);
    Screen::grantScope(d, agentIndex, driving);
    Screen::setTokenID(d, agentIndex, "agent");
    // No person is attached to this session, so the agent holds the standing
    // grant a person's allow-for-this-session answer gives.
    Screen::grantAgentSession(d, detached, agentIndex);
    UbiquittyTest::ScreenClient agent { 24, 80 };
    Screen::greet(d, agentIndex, agent);
    agent.setView(Sync::ViewState::RecordsOnly);
    Screen::deliver(d, agentIndex, agent);
    // The pane has a reader now: an ask is a read, and the facts are current
    // though nobody read them while it wrote.
    Screen::noteRecordsDue(d, detached, later());
    CHECK_MSG(hostFacts(detached).revision == 1 && hostFacts(detached).programTitle == "backup", "a pane with a reader has no current facts");
    CHECK_MSG(hostFacts(detached).createdUnixSeconds == 1'699'999'000 && hostFacts(detached).isAdopted, "the facts do not say when the session was created or that it was adopted");
    CHECK_MSG(hostFacts(detached).rows == 24 && hostFacts(detached).columns == 80, "the facts do not carry the pane's grid");
    CHECK_MSG(hostFacts(detached).hostName == Ubiquitty::PAL::systemHostName(), "the facts do not name the host");
    CHECK_MSG(detached.screenSource->status().revision == 1 && detached.screenSource->status().store.records.size() == 1, "a pane with a reader has no status set");
    Screen::deliver(d, agentIndex, agent);
    CHECK_MSG(Screen::connection(d, agentIndex)->grantedRole() == Sync::Role::Agent, "the second client is not an agent");
    CHECK_MSG(agent.count(Sync::FrameKind::StatusRecords) == 1, "an agent was not sent the pane's status set");
    const std::optional<Sync::StatusRecords> agentStatus = agent.connection().paneStatus(1);
    CHECK_MSG(agentStatus && agentStatus->revision == detached.screenSource->status().revision && agentStatus->store == detached.screenSource->status().store, "the agent's status set is not the host's");
    CHECK_MSG(hostFacts(detached).attention.hasUnseenBell, "an agent's attachment marked a bell seen");
    CHECK_MSG(agent.connection().paneFacts(1) && agent.connection().paneFacts(1)->attention.hasUnseenBell, "the agent was not told of the unseen bell");
    // A status that stands is not sent again.
    Screen::publishRecords(d, detached, later());
    Screen::deliver(d, agentIndex, agent);
    CHECK_MSG(agent.count(Sync::FrameKind::StatusRecords) == 1, "a status set that did not move was sent again");
    // Shown by automation in a focused window, the bell is still unseen.
    agent.setView(Sync::ViewState::Live);
    CHECK(agent.setPaneFocus(Ubiquitty::Modes::FocusState::Focused));
    Screen::deliver(d, agentIndex, agent);
    Screen::publishRecords(d, detached, later());
    CHECK_MSG(Screen::connection(d, agentIndex)->paneView(1) == std::optional { Sync::ViewState::Live } && Screen::connection(d, agentIndex)->facts().isFocused, "the agent does not show the pane in a focused window");
    CHECK_MSG(detached.screenSource->isAnyDriverFocused() && !detached.screenSource->isAnyPersonFocused(), "the agent's focus was not taken as a driver's that is no person's");
    CHECK_MSG(hostFacts(detached).attention.hasUnseenBell, "an agent showing the pane marked its bell seen");

    // 5. The budget: an ask inside it is owed, and served when it closes.
    const std::uint64_t read = later();
    Screen::publishRecords(d, detached, read);
    const std::uint32_t revision = hostFacts(detached).revision;
    Ubiquitty::processBytes(detached.term.get(), "\x1b]2;restore\x07");
    Screen::noteRecordsDue(d, detached, read + 10);
    CHECK_MSG(hostFacts(detached).revision == revision && detached.recordsOwedAtMilliseconds == read + 50, "records asked for inside the budget were read at once, or not owed");
    Screen::serveOwedRecords(d, detached, read + 49);
    CHECK_MSG(hostFacts(detached).revision == revision, "owed records were read before the budget's window closed");
    Screen::serveOwedRecords(d, detached, read + 50);
    CHECK_MSG(hostFacts(detached).revision == revision + 1 && hostFacts(detached).programTitle == "restore" && detached.recordsOwedAtMilliseconds == 0, "owed records were not read when the window closed");
    // Past the budget an ask is a read.
    Ubiquitty::processBytes(detached.term.get(), "\x1b]2;verify\x07");
    Screen::noteRecordsDue(d, detached, read + 100);
    CHECK_MSG(hostFacts(detached).programTitle == "verify", "records asked for past the budget were not read");
    // An owed probe, once its window has closed, reads the records too.
    const std::uint64_t probed = later();
    Ubiquitty::processBytes(detached.term.get(), "\x1b]2;probed\x07");
    detached.foregroundProbeOwedAtMilliseconds = probed;
    Screen::serveOwedForegroundProbe(d, detached, probed - 1);
    CHECK_MSG(hostFacts(detached).programTitle == "verify", "a probe was served before it was owed");
    Screen::serveOwedForegroundProbe(d, detached, probed);
    CHECK_MSG(hostFacts(detached).programTitle == "probed" && detached.lastRecordsMilliseconds == probed, "an owed probe did not read the pane's records");

    // A reader that attaches is read for at once, before it shows anything
    // and with no frame to ask: the attach itself asks.
    auto& quiet = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    std::array<int, 2> ends {};
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0, "premise: no socket pair");
    const std::size_t readerIndex = Screen::attach(d, quiet.id, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::grantScope(d, readerIndex, Sync::ScopeSet { Sync::Scope::Observe });
    Screen::markInteractiveUser(d, readerIndex);
    CHECK_MSG(hostFacts(quiet).revision == 0, "a pane was read before its reader was seeded");
    UbiquittyTest::ScreenClient reader { 24, 80 };
    Screen::greet(d, readerIndex, reader);
    CHECK_MSG(hostFacts(quiet).revision == 1 && quiet.lastRecordsMilliseconds != 0, "a reader that attached was not read for");

    // 6. The stamps ride along, and move the revision once a second at most.
    const std::uint64_t stamped = later();
    wallNow += 60'000;
    detached.activity.noteOutput(wallNow);
    Screen::publishRecords(d, detached, stamped);
    const std::uint32_t stampedRevision = hostFacts(detached).revision;
    CHECK_MSG(hostFacts(detached).activity.lastOutputUnixMilliseconds == wallNow, "the facts do not carry the output stamp");
    wallNow += 100;
    detached.activity.noteOutput(wallNow);
    Screen::publishRecords(d, detached, stamped + 999);
    CHECK_MSG(hostFacts(detached).revision == stampedRevision, "a stamp alone moved the facts inside a second");
    CHECK_MSG(hostFacts(detached).activity.lastOutputUnixMilliseconds == wallNow, "a stamp that rides along was not kept");
    Screen::publishRecords(d, detached, stamped + 1000);
    CHECK_MSG(hostFacts(detached).revision == stampedRevision + 1, "a stamp a second old did not move the facts");
    loopNow = stamped + 1000;

    // 7. A pin names who pinned; the session's end is in its last facts.
    // The pin asks for the records itself: no frame follows a pin.
    shown.lastRecordsMilliseconds = 0;
    CHECK(person.call(R"({"jsonrpc":"2.0","id":1,"method":"pane.pinGeometry","params":{"pane":1,"rows":30,"columns":100}})"));
    Screen::deliver(d, personIndex, person);
    Screen::deliver(d, personIndex, person);
    const Sync::AttachmentRecord* attachment = Screen::connection(d, personIndex)->attachment();
    CHECK_MSG(attachment && hostFacts(shown).geometry.isPinned && hostFacts(shown).geometry.pinnedBy == std::optional { attachment->attachmentID }, "a pin is not in the pane's facts with who pinned");
    CHECK_MSG(person.connection().paneFacts(1) && person.connection().paneFacts(1)->geometry.isPinned, "the client was not told its pane is pinned");
    shown.end = { .didEnd = true, .hasStatus = true, .wasSignalled = false, .code = 3 };
    Screen::publishRecords(d, shown, later());
    CHECK_MSG((hostFacts(shown).end == Sync::SessionEndFacts { .didEnd = true, .hasStatus = true, .wasSignalled = false, .code = 3 }), "how the session ended is not in its facts");

    if (failures == 0)
        std::printf("pane_records_published: a pane with a reader has current facts and a status set, and one without costs nothing; a shown pane's status rides its screen; attention is seen by a person looking and nobody else; records are read on a budget\n");
    return failures == 0 ? 0 : 1;
}
