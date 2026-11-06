// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/several_clients.cpp — several clients on one session see one
// picture, and share one question about its tree (Docs/sync.md §6.9, §7.3).
//
//   1. Four clients hold one session: two people who drive, one who may only
//      observe, and one that reads nothing until the end. The program floods
//      while each reads at its own pace and one person's link drops and
//      resumes; every client ends holding the host's picture.
//   2. Two people answer an agent's question to swap two panes in one pass of
//      the host, in each mix of Allow and Deny and in either order. One answer
//      is served and decides; the other is told the question is gone; the
//      panes are swapped once or not at all, as that answer said (a swap made
//      twice would put them back); and each person is told once that the
//      question is settled.
//   3. A client that may only observe is never asked: not when the question is
//      sent, not when it attaches while the question waits, and not when it
//      resumes. Its answer is refused and leaves the question waiting for a
//      person who may give it.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), the QUIC-off host build as
// resume_swap. Exit 0 on success.

#include "Assertions.h"
#include "NotifyRead.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };
const Sync::ScopeSet observing { Sync::Scope::Observe };

/// What a client holds of a pane, as its resume names it.
Sync::ResumeRequest resumeOf(Sync::ReplicaSet& replicas, Sync::ID128 paneID, const Sync::Welcome& welcome)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    if (Sync::ReplicaSet::Held* held = replicas.find(paneID))
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live });
    return request;
}

/// Attaches a client under a token on a new connection, with the scopes its
/// token grants, to the session's workspace.
std::size_t attachUnder(Daemon& d, auto& session, const char* token, const Sync::ScopeSet& scopes)
{
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, scopes);
    Screen::setTokenID(d, index, token);
    Screen::markInteractiveUser(d, index);
    Screen::bindWorkspace(d, index, session);
    return index;
}

void write(Daemon& d, auto& session, const std::string& text)
{
    Ubiquitty::processBytes(session.term.get(), text.c_str());
    session.isFrameDue = true;
    Screen::publish(d, session);
}

/// The visible grid, row by row, as text.
std::string gridText(const Ubiquitty::Terminal& terminal)
{
    std::string text;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

bool isServed(const std::string& answer)
{
    return answer.find("\"result\"") != std::string::npos && answer.find("\"error\"") == std::string::npos;
}

/// The tree questions a client was sent since it was last read.
std::vector<Sync::StructureConsent> questionsOf(UbiquittyTest::ScreenClient& client)
{
    std::vector<Sync::StructureConsent> questions;
    for (const Sync::Notify& notify : client.takeNotifies()) {
        if (auto question = UbiquittyTest::structureConsentOf(notify))
            questions.push_back(std::move(*question));
    }
    return questions;
}

/// How many times a client was told a tree question is settled.
std::size_t retirementsOf(UbiquittyTest::ScreenClient& client, std::uint32_t requestID)
{
    std::size_t count = 0;
    for (const Sync::Notify& notify : client.takeNotifies()) {
        const auto resolution = UbiquittyTest::consentResolvedOf(notify);
        count += resolution && resolution->kind == Sync::ConsentKind::Structure && resolution->requestID == requestID;
    }
    return count;
}

enum class Answer : std::uint8_t {
    Allow,
    Deny,
};

std::string respond(std::uint32_t requestID, Answer answer)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"consent.respond\",\"params\":{\"request\":" + std::to_string(requestID)
        + ",\"kind\":\"structure\",\"decision\":\"" + (answer == Answer::Allow ? "once" : "deny") + "\"}}";
}

/// The one answer a client was given, or a line saying how many it was.
std::string onlyResult(UbiquittyTest::ScreenClient& client)
{
    const std::vector<std::string> results = client.takeResults();
    return results.size() == 1 ? results.front() : std::to_string(results.size()) + " results";
}

/// Case 1: several clients reading at their own pace through a flood, one of
/// them dropped and resumed in the middle of it.
void floodAcrossClients()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    const Sync::ID128 paneID = session.screenSource->paneID();
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t aliceIndex = attachUnder(d, session, "alice", driving);
    UbiquittyTest::ScreenClient alice { 24, 80 };
    Screen::greet(d, aliceIndex, alice);
    Sync::ReplicaSet bobReplicas;
    std::size_t bobIndex = attachUnder(d, session, "bob", driving);
    std::optional<UbiquittyTest::ScreenClient> bob;
    bob.emplace(24, 80, bobReplicas, std::nullopt);
    Screen::greet(d, bobIndex, *bob);
    const std::size_t watcherIndex = attachUnder(d, session, "carol", observing);
    UbiquittyTest::ScreenClient watcher { 24, 80 };
    Screen::greet(d, watcherIndex, watcher);
    const std::size_t sleeperIndex = attachUnder(d, session, "dave", observing);
    UbiquittyTest::ScreenClient sleeper { 24, 80 };
    Screen::greet(d, sleeperIndex, sleeper);
    REQUIRE(alice.connection().welcome() && bob->connection().welcome() && watcher.connection().welcome() && sleeper.connection().welcome());
    const Sync::Welcome bobWelcome = *bob->connection().welcome();

    for (int line = 0; line < 400; ++line) {
        write(d, session, "line " + std::to_string(line) + " for several readers, long enough to fill most of a row\r\n");
        Screen::deliver(d, aliceIndex, alice);
        CHECK_MSG(alice.generation() == session.screenSource->generation(), ("the client that reads at once fell behind at line " + std::to_string(line)).c_str());
        if (bob && line % 7 == 0)
            Screen::deliver(d, bobIndex, *bob);
        if (line % 53 == 0)
            Screen::deliver(d, watcherIndex, watcher);
        if (line == 150) {
            bob->storeReplicas();
            Screen::drop(d, bobIndex);
            bob.reset();
        }
        if (line == 220) {
            bobIndex = attachUnder(d, session, "bob", driving);
            bob.emplace(24, 80, bobReplicas, resumeOf(bobReplicas, paneID, bobWelcome));
            Screen::greet(d, bobIndex, *bob);
            REQUIRE(bob->connection().welcome().has_value());
            CHECK_MSG(bob->connection().welcome()->attachmentID == bobWelcome.attachmentID, "the dropped person's resume did not continue their attachment");
        }
    }

    const std::string host = gridText(*session.term);
    const auto holdsHost = [&](std::size_t index, UbiquittyTest::ScreenClient& client, const char* who) {
        Screen::deliver(d, index, client);
        CHECK_MSG(Screen::connection(d, index)->state() == Sync::ConnectionState::Attached, (std::string(who) + " is no longer attached").c_str());
        CHECK_MSG(client.generation() == session.screenSource->generation(), (std::string(who) + " does not hold the host's newest frame").c_str());
        const Ubiquitty::Terminal* picture = client.pane();
        CHECK_MSG(picture && gridText(*picture) == host, (std::string(who) + " does not hold the host's picture").c_str());
    };
    holdsHost(aliceIndex, alice, "the client that reads at once");
    holdsHost(bobIndex, *bob, "the person who dropped and resumed");
    holdsHost(watcherIndex, watcher, "the client that observes now and then");
    holdsHost(sleeperIndex, sleeper, "the client that read nothing");
}

enum class Order : std::uint8_t {
    AliceFirst,
    BobFirst,
};

/// Case 2: two people answer the same question before the host serves either.
void twoAnswersAtOnce(Answer aliceAnswer, Answer bobAnswer, Order order)
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 156);
    auto& neighbour = Screen::makeSession(d, 24, 156);
    (void)Screen::nameWorkspace(d, session, "shared");
    Screen::splitInto(d, session, neighbour, Ubiquitty::Panes::Orientation::Vertical);
    auto& model = Screen::layout(d, session);
    const auto window = model.activeSurface();
    const auto tab = model.activeTab(window);
    const auto paneOrder = [&] {
        std::vector<int> panes;
        Ubiquitty::Panes::forEachLeaf(*model.paneTree(window, tab), [&](int pane) { panes.push_back(pane); });
        return panes;
    };
    const std::vector<int> before = paneOrder();
    REQUIRE(before.size() == 2);
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t aliceIndex = attachUnder(d, session, "alice", driving);
    UbiquittyTest::ScreenClient alice { 24, 80 };
    Screen::greet(d, aliceIndex, alice);
    const std::size_t bobIndex = attachUnder(d, session, "bob", driving);
    UbiquittyTest::ScreenClient bob { 24, 80 };
    Screen::greet(d, bobIndex, bob);

    Ubiquitty::HostTest::FullCarrier carrier;
    const std::size_t agentIndex = Screen::connect(d, carrier.takeDaemonEnd());
    REQUIRE(carrier.isReady());
    UbiquittyTest::ScreenClient agent { 24, 80 };
    Screen::setTokenID(d, agentIndex, "requester");
    Screen::grantScope(d, agentIndex, driving);
    Screen::greet(d, agentIndex, agent);
    const auto revision = Screen::windowStructureRevision(d, std::to_underlying(window));
    REQUIRE(revision);
    CHECK(agent.call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"pane.swap\",\"params\":{\"window\":" + std::to_string(std::to_underlying(window))
        + ",\"pane\":" + std::to_string(Screen::layoutPane(d, neighbour)) + ",\"target\":" + std::to_string(Screen::layoutPane(d, session))
        + ",\"expectedRevision\":\"" + std::to_string(revision) + "\"}}"));
    Screen::deliver(d, agentIndex, agent);
    CHECK(agent.takeResults().empty());
    REQUIRE(Screen::pendingStructureConsents(d) == 1);

    Screen::deliver(d, aliceIndex, alice);
    Screen::deliver(d, bobIndex, bob);
    const auto askedAlice = questionsOf(alice);
    const auto askedBob = questionsOf(bob);
    REQUIRE(askedAlice.size() == 1 && askedBob.size() == 1);
    const std::uint32_t requestID = askedAlice.front().requestID;
    CHECK_MSG(askedBob.front().requestID == requestID, "the two people were asked different questions");

    // Both answers are with the host before it serves either.
    CHECK(alice.call(respond(requestID, aliceAnswer)));
    CHECK(bob.call(respond(requestID, bobAnswer)));
    const std::vector<std::uint8_t> fromAlice = alice.takeStream(Screen::now());
    const std::vector<std::uint8_t> fromBob = bob.takeStream(Screen::now());
    if (order == Order::AliceFirst) {
        Screen::receive(d, aliceIndex, fromAlice);
        Screen::receive(d, bobIndex, fromBob);
    } else {
        Screen::receive(d, bobIndex, fromBob);
        Screen::receive(d, aliceIndex, fromAlice);
    }
    Screen::service(d);
    Screen::deliver(d, aliceIndex, alice);
    Screen::deliver(d, bobIndex, bob);
    const std::string toAlice = onlyResult(alice);
    const std::string toBob = onlyResult(bob);
    CHECK_MSG(isServed(toAlice) != isServed(toBob), ("not exactly one answer was served: " + toAlice + " / " + toBob).c_str());
    const std::string& refused = isServed(toAlice) ? toBob : toAlice;
    CHECK_MSG(refused.find("no longer pending") != std::string::npos, refused.c_str());
    const Answer decided = isServed(toAlice) ? aliceAnswer : bobAnswer;

    Screen::deliver(d, agentIndex, agent);
    const std::string toAgent = onlyResult(agent);
    CHECK_MSG(isServed(toAgent) == (decided == Answer::Allow), toAgent.c_str());
    const std::vector<int> swapped { before[1], before[0] };
    CHECK_MSG(paneOrder() == (decided == Answer::Allow ? swapped : before), "the panes were not swapped exactly as the served answer decided");
    CHECK(Screen::pendingStructureConsents(d) == 0);
    CHECK_MSG(retirementsOf(alice, requestID) == 1, "the first person was not told once that the question is settled");
    CHECK_MSG(retirementsOf(bob, requestID) == 1, "the second person was not told once that the question is settled");
}

/// Case 3: a client that may only observe is never asked.
void observerNeverAsked()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    const std::uint32_t workspace = Screen::nameWorkspace(d, session, "before");
    const Sync::ID128 paneID = session.screenSource->paneID();
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    const std::size_t personIndex = attachUnder(d, session, "alice", driving);
    UbiquittyTest::ScreenClient person { 24, 80 };
    Screen::greet(d, personIndex, person);
    Sync::ReplicaSet watcherReplicas;
    const std::size_t watcherIndex = attachUnder(d, session, "carol", observing);
    UbiquittyTest::ScreenClient watcher(24, 80, watcherReplicas, std::nullopt);
    Screen::greet(d, watcherIndex, watcher);
    REQUIRE(watcher.connection().welcome().has_value());
    const Sync::Welcome watcherWelcome = *watcher.connection().welcome();

    Ubiquitty::HostTest::FullCarrier carrier;
    const std::size_t agentIndex = Screen::connect(d, carrier.takeDaemonEnd());
    REQUIRE(carrier.isReady());
    UbiquittyTest::ScreenClient agent { 24, 80 };
    Screen::setTokenID(d, agentIndex, "requester");
    Screen::grantScope(d, agentIndex, driving);
    Screen::greet(d, agentIndex, agent);
    CHECK(agent.call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace.rename\",\"params\":{\"workspace\":" + std::to_string(workspace) + ",\"name\":\"after\"}}"));
    Screen::deliver(d, agentIndex, agent);
    REQUIRE(Screen::pendingStructureConsents(d) == 1);

    Screen::deliver(d, personIndex, person);
    const auto asked = questionsOf(person);
    REQUIRE(asked.size() == 1);
    const std::uint32_t requestID = asked.front().requestID;
    Screen::deliver(d, watcherIndex, watcher);
    CHECK_MSG(questionsOf(watcher).empty(), "a client that may only observe was asked when the question was sent");

    // Another observer attaches while the question waits.
    const std::size_t lateIndex = attachUnder(d, session, "dave", observing);
    UbiquittyTest::ScreenClient late { 24, 80 };
    Screen::greet(d, lateIndex, late);
    Screen::deliver(d, lateIndex, late);
    REQUIRE(late.connection().welcome().has_value());
    CHECK_MSG(questionsOf(late).empty(), "a client that may only observe was asked when it attached");

    // The first observer's link drops and comes back.
    watcher.storeReplicas();
    Screen::drop(d, watcherIndex);
    const std::size_t resumedIndex = attachUnder(d, session, "carol", observing);
    UbiquittyTest::ScreenClient resumed(24, 80, watcherReplicas, resumeOf(watcherReplicas, paneID, watcherWelcome));
    Screen::greet(d, resumedIndex, resumed);
    Screen::deliver(d, resumedIndex, resumed);
    REQUIRE(resumed.connection().welcome().has_value());
    CHECK_MSG(resumed.connection().welcome()->attachmentID == watcherWelcome.attachmentID, "the observer's resume did not continue its attachment");
    CHECK_MSG(questionsOf(resumed).empty(), "a client that may only observe was asked when it resumed");

    // Its answer is refused and the question still waits for the person.
    CHECK(resumed.call(respond(requestID, Answer::Allow)));
    Screen::deliver(d, resumedIndex, resumed);
    const std::string toObserver = onlyResult(resumed);
    CHECK_MSG(!isServed(toObserver), ("an observer's answer was served: " + toObserver).c_str());
    CHECK_MSG(Screen::pendingStructureConsents(d) == 1, "an observer's answer settled the question");
    Screen::deliver(d, agentIndex, agent);
    CHECK_MSG(agent.takeResults().empty(), "the agent was answered on an observer's word");

    CHECK(person.call(respond(requestID, Answer::Allow)));
    Screen::deliver(d, personIndex, person);
    CHECK(isServed(onlyResult(person)));
    Screen::deliver(d, agentIndex, agent);
    const std::string toAgent = onlyResult(agent);
    CHECK_MSG(isServed(toAgent), toAgent.c_str());
    CHECK(Screen::pendingStructureConsents(d) == 0);
    CHECK_MSG(retirementsOf(person, requestID) == 1, "the person was not told the question is settled");
    Screen::deliver(d, lateIndex, late);
    Screen::deliver(d, resumedIndex, resumed);
    CHECK_MSG(retirementsOf(late, requestID) == 0 && retirementsOf(resumed, requestID) == 0, "an observer was told about a question it was never asked");
}

} // namespace

int main()
{
    floodAcrossClients();
    for (const Order order : { Order::AliceFirst, Order::BobFirst }) {
        for (const Answer alice : { Answer::Allow, Answer::Deny }) {
            for (const Answer bob : { Answer::Allow, Answer::Deny })
                twoAnswersAtOnce(alice, bob, order);
        }
    }
    observerNeverAsked();
    if (failures == 0)
        std::printf("several_clients: four clients reading at their own pace through a flood, one dropped and resumed, all hold the host's picture; "
                    "of two answers to one question one is served and decides, and the change is made at most once; "
                    "a client that may only observe is never asked, attached early, late or resumed, and its answer settles nothing\n");
    return failures == 0 ? 0 : 1;
}
