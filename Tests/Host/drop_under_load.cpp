// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/drop_under_load.cpp — a client whose link drops while the host
// is busy with it resumes into the state the host holds (Docs/sync.md §6.9).
//
//   1. Mid-flood: frames the host queued and the client never read are lost
//      with the link, the program keeps printing, and the resumed client is
//      brought to the host's picture exactly.
//   2. The same, with the pane resized while the client was away.
//   3. Mid-question: a person's link drops while an agent's tree change waits
//      for their approval. The person resumes within the grace, is asked
//      again on the new connection, and their answer carries the change.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), the QUIC-off host build as
// resume_swap. Exit 0 on success.

#include "Assertions.h"
#include "NotifyRead.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Screen/Addressing.h"
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

/// What a client holds of a pane, as its resume names it.
Sync::ResumeRequest resumeOf(Sync::ReplicaSet& replicas, Sync::ID128 paneID, const Sync::Welcome& welcome)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    if (Sync::ReplicaSet::Held* held = replicas.find(paneID))
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live });
    return request;
}

/// Attaches a person under a token on a new connection, with the authority to
/// approve a change to the workspace's tree.
std::size_t attachPerson(Daemon& d, auto& session, const char* token)
{
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
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

/// What happens to the pane while the client's link is down.
enum class WhileAway : std::uint8_t {
    Unchanged,
    Resized,
};

/// Whether the dead physical client is freed before its replacement arrives.
enum class Reap : std::uint8_t {
    Deferred,
    Immediate,
};

/// Cases 1 and 2: the link drops with the host's frames unread.
void floodThenDrop(WhileAway whileAway, Reap reap)
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    const Sync::ID128 paneID = session.screenSource->paneID();
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    Sync::ReplicaSet replicas;
    const std::size_t heldIndex = attachPerson(d, session, "alice");
    UbiquittyTest::ScreenClient held(24, 80, replicas, std::nullopt);
    Screen::greet(d, heldIndex, held);
    write(d, session, "before the flood\r\n");
    Screen::deliver(d, heldIndex, held);
    REQUIRE(held.connection().welcome().has_value());
    const Sync::Welcome welcome = *held.connection().welcome();
    REQUIRE(held.generation() == session.screenSource->generation());
    held.storeReplicas();

    // The host keeps publishing frames the client never reads, and the link
    // goes with them unread.
    for (int line = 0; line < 400; ++line)
        write(d, session, "flood line " + std::to_string(line) + " with enough text to fill a good part of the row\r\n");
    Screen::drop(d, heldIndex);
    if (reap == Reap::Immediate)
        Screen::reap(d, heldIndex);
    if (whileAway == WhileAway::Resized)
        Ubiquitty::resize(*session.term, 30, 100);
    for (int line = 0; line < 50; ++line)
        write(d, session, "while away " + std::to_string(line) + "\r\n");

    const std::size_t resumedIndex = attachPerson(d, session, "alice");
    UbiquittyTest::ScreenClient resumed(24, 80, replicas, resumeOf(replicas, paneID, welcome));
    Screen::greet(d, resumedIndex, resumed);
    REQUIRE(session.isFrameDue);
    // The real loop publishes an owed frame before serving the connection.
    // Advance admission past the last frame's pacing interval without sleeping.
    const auto frameClass = Screen::mayPublish(d, session, Screen::now() + Ubiquitty::Runtime::refreshIntervalMicroseconds + 1);
    REQUIRE(frameClass.has_value());
    Screen::publish(d, session, *frameClass);
    CHECK(!session.isFrameDue);
    CHECK(session.screenSource->isFrameHeld());
    Screen::deliver(d, resumedIndex, resumed);
    REQUIRE(resumed.connection().welcome().has_value());
    CHECK_MSG(resumed.connection().welcome()->attachmentID == welcome.attachmentID, "the resume did not continue the attachment");
    CHECK_MSG(resumed.generation() == session.screenSource->generation(), "the resumed client does not hold the host's picture");
    const Ubiquitty::Terminal* picture = resumed.pane();
    REQUIRE(picture != nullptr);
    CHECK_MSG(picture->rows == session.term->rows && picture->columns == session.term->columns, "the resumed picture is not the host's size");
    CHECK_MSG(gridText(*picture) == gridText(*session.term), "the resumed picture is not the host's");
    write(d, session, "after\r\n");
    Screen::deliver(d, resumedIndex, resumed);
    CHECK_MSG(resumed.generation() == session.screenSource->generation(), "the resumed connection is not served");
}

/// Case 3: the person who must approve an agent's change loses their link
/// while the question waits.
void questionThenDrop()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    const std::uint32_t workspace = Screen::nameWorkspace(d, session, "before");
    const Sync::ID128 paneID = session.screenSource->paneID();
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    Sync::ReplicaSet replicas;
    const std::size_t personIndex = attachPerson(d, session, "alice");
    UbiquittyTest::ScreenClient person(24, 80, replicas, std::nullopt);
    Screen::greet(d, personIndex, person);
    Screen::deliver(d, personIndex, person);
    REQUIRE(person.connection().welcome().has_value());
    const Sync::Welcome welcome = *person.connection().welcome();

    Ubiquitty::HostTest::FullCarrier carrier;
    const std::size_t agentIndex = Screen::connect(d, carrier.takeDaemonEnd());
    REQUIRE(carrier.isReady());
    UbiquittyTest::ScreenClient agent { 24, 80 };
    Screen::setTokenID(d, agentIndex, "requester");
    Screen::grantScope(d, agentIndex, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::greet(d, agentIndex, agent);
    const std::string rename = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace.rename\",\"params\":{\"workspace\":" + std::to_string(workspace) + ",\"name\":\"after\"}}";
    CHECK(agent.call(rename));
    Screen::deliver(d, agentIndex, agent);
    CHECK(agent.takeResults().empty());
    REQUIRE(Screen::pendingStructureConsents(d) == 1);

    const auto questionOf = [](UbiquittyTest::ScreenClient& client) -> std::optional<Sync::StructureConsent> {
        for (const Sync::Notify& notify : client.takeNotifies()) {
            if (auto question = UbiquittyTest::structureConsentOf(notify))
                return question;
        }
        return std::nullopt;
    };
    Screen::deliver(d, personIndex, person);
    const auto asked = questionOf(person);
    REQUIRE(asked.has_value());

    // The person's link drops before they answer, and comes back within the
    // grace on a new connection.
    person.storeReplicas();
    Screen::drop(d, personIndex);
    Screen::expireStructureConsents(d, Screen::now() / 1000);
    CHECK_MSG(Screen::pendingStructureConsents(d) == 1, "the question was dropped with the person's link");
    const std::size_t resumedIndex = attachPerson(d, session, "alice");
    UbiquittyTest::ScreenClient resumed(24, 80, replicas, resumeOf(replicas, paneID, welcome));
    Screen::greet(d, resumedIndex, resumed);
    Screen::deliver(d, resumedIndex, resumed);
    REQUIRE(resumed.connection().welcome().has_value());
    CHECK_MSG(resumed.connection().welcome()->attachmentID == welcome.attachmentID, "the resume did not continue the person's attachment");
    const auto askedAgain = questionOf(resumed);
    CHECK_MSG(askedAgain && askedAgain->requestID == asked->requestID && askedAgain->method == "workspace.rename",
        "the person was not asked again on the connection they resumed");

    // Their answer on the new connection carries the change.
    const std::string answer = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"consent.respond\",\"params\":{\"request\":" + std::to_string(asked->requestID)
        + ",\"kind\":\"structure\",\"decision\":\"once\"}}";
    CHECK(resumed.call(answer));
    Screen::deliver(d, resumedIndex, resumed);
    Screen::deliver(d, resumedIndex, resumed);
    const auto answered = resumed.takeResults();
    CHECK_MSG(answered.size() == 1 && answered.front().find("\"result\"") != std::string::npos, (answered.empty() ? std::string("no answer") : answered.front()).c_str());
    Screen::deliver(d, agentIndex, agent);
    const auto results = agent.takeResults();
    CHECK_MSG(results.size() == 1 && results.front().find("\"result\"") != std::string::npos, (results.empty() ? std::string("no result") : results.front()).c_str());
    CHECK_MSG(Screen::pendingStructureConsents(d) == 0, "the answered question is still pending");
}

} // namespace

int main()
{
    for (Reap reap : { Reap::Deferred, Reap::Immediate }) {
        floodThenDrop(WhileAway::Unchanged, reap);
        floodThenDrop(WhileAway::Resized, reap);
    }
    questionThenDrop();
    if (failures == 0)
        std::printf("drop_under_load: a client dropped mid-flood resumes into the host's picture, resized or not, and a person dropped "
                    "while an agent's change waits for them is asked again on the connection they resume and their answer carries it\n");
    return failures == 0 ? 0 : 1;
}
