// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/hostile_clients.cpp — clients that break the protocol's manners
// on purpose are held to its bounds (Docs/sync.md §6.7, §7.3, §9).
//
//   1. A client types past its share of a pane into a program that reads
//      nothing, in large frames and in a flood of one-byte frames: the pane
//      holds no more than the share in bytes or in events, the sender is
//      closed with Bye{Oversize}, and what it queued unwritten is dropped.
//      Another person at the pane is not refused for it: a key and a burst
//      they send after the flood are taken, and they stay attached. The same
//      holds when the sender fills its share without passing it and so
//      stays, its input waiting.
//   2. Two people each paste a megabyte into a program that reads nothing:
//      each keeps within its own window, both stay attached, and the pane
//      holds both windows.
//   3. A connection types into three busy panes: it is closed once what it
//      holds unwritten across them passes its connection's bound, though no
//      one pane's share was passed, and another connection typing into one
//      of those panes is unaffected.
//   4. A client whose link is lost keeps the input accepted from it: a
//      resume continues it. Connections that come and go this way fill the
//      host toward its own bound, and the first one to find no room is
//      closed, every earlier one's input still held.
//   5. A client turns its focus over and over at a program that reads
//      nothing: the pane holds one waiting report, not one for each turn.
//   6. A client writes calls and reads none of the answers: the host stops
//      reading it once its unsent interactive frames pass their bound, holds
//      no more than that however many calls it writes, keeps it attached, and
//      answers the calls it read once it reads.
//   7. A resume names the most panes a greeting carries, none of which the
//      attachment held: the attachment is resumed and every pane named gone.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), the QUIC-off host build as
// resume_swap. The host is handed a client's bytes only while it would read
// that client, as its poll does. Exit 0 on success.

#include "Assertions.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };

std::size_t attachUnder(Daemon& d, std::uint32_t sessionID, const char* token)
{
    const std::size_t index = Screen::attach(d, sessionID);
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, token);
    Screen::markInteractiveUser(d, index);
    return index;
}

bool isAttached(Daemon& d, std::size_t index)
{
    const auto* connection = Screen::connection(d, index);
    return !Screen::didOverflow(d, index) && connection && connection->state() == Sync::ConnectionState::Attached;
}

bool wasClosedFor(Daemon& d, std::size_t index, Sync::ByeReason reason)
{
    const auto* connection = Screen::connection(d, index);
    return connection && connection->state() == Sync::ConnectionState::Closed && connection->byeReason() == reason;
}

/// A session whose program reads nothing: a small queue fills, and every
/// write past it is refused.
auto& busySession(Daemon& d)
{
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    stream->queueWriteAll();
    stream->capWriteQueue(4'096);
    stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    return session;
}

/// The instance every `ScreenClient` greets as unless it is given another.
constexpr Sync::ID128 defaultInstance { .low = 1, .high = 2 };

/// How much of one byte a busy session's program was written.
std::size_t writtenOf(auto& session, std::uint8_t byte)
{
    const auto& pty = static_cast<const Ubiquitty::TestDoubles::StreamDouble&>(*session.stream);
    return static_cast<std::size_t>(std::ranges::count(pty.written, byte));
}

/// Case 1: a client types past its share, in frames of a given size, and
/// another person types after it.
void typesPastTheBound(std::size_t frameBytes, int framesPerPass)
{
    Daemon d;
    auto& session = busySession(d);
    const Sync::ID128 honestInstance { .low = 7, .high = 7 };
    const std::size_t honestIndex = attachUnder(d, session.id, "honest");
    UbiquittyTest::ScreenClient honest(24, 80, honestInstance);
    Screen::greet(d, honestIndex, honest);
    const std::size_t index = attachUnder(d, session.id, "flooder");
    UbiquittyTest::ScreenClient flooder(24, 80);
    Screen::greet(d, index, flooder);
    REQUIRE(isAttached(d, index) && isAttached(d, honestIndex));

    const std::vector<std::uint8_t> frame(frameBytes, 'f');
    const std::size_t mostSent = 4 * Sync::maxHeldInputBytes;
    std::size_t sent = 0;
    std::size_t mostHeld = 0;
    while (isAttached(d, index) && sent < mostSent) {
        for (int count = 0; count < framesPerPass; ++count) {
            REQUIRE(flooder.sendRaw(frame).has_value());
            sent += frame.size();
        }
        // Input is read whatever the host owes the client.
        CHECK_MSG(!Screen::isInputHeld(d, index), "the host stopped reading a client that sends input");
        Screen::receive(d, index, flooder.takeStream(Screen::now()));
        Screen::service(d);
        mostHeld = std::max(mostHeld, session.screenSource->heldInputBytes());
    }
    const std::string sizes = " (" + std::to_string(frameBytes) + "-byte frames)";
    CHECK_MSG(wasClosedFor(d, index, Sync::ByeReason::Oversize), ("a client that typed past its share was not closed for it" + sizes).c_str());
    CHECK_MSG(mostHeld <= Sync::maxHeldInputBytes, ("the pane held more of one sender's input than its share: " + std::to_string(mostHeld) + sizes).c_str());
    CHECK_MSG(frameBytes > 1 || mostHeld <= Sync::maxHeldInputEvents, ("the pane held more of one sender's events than its share: " + std::to_string(mostHeld) + sizes).c_str());
    CHECK_MSG(session.screenSource->heldInputBytes(defaultInstance) == 0, ("the closed sender's unwritten input was kept" + sizes).c_str());

    // Another person at the pane types a key, then a burst.
    const std::uint8_t key[] = { 'x' };
    REQUIRE(honest.sendRaw(key).has_value());
    Screen::deliver(d, honestIndex, honest);
    const std::vector<std::uint8_t> burst(50'000, 'h');
    REQUIRE(honest.sendRaw(burst).has_value());
    Screen::deliver(d, honestIndex, honest);
    CHECK_MSG(isAttached(d, honestIndex), ("another person was closed for a flood that was not theirs" + sizes).c_str());
    // An event refused closes its sender, so staying attached is being
    // taken; what the program's queue had room for may be gone from the pane.
    CHECK_MSG(session.screenSource->heldInputBytes(honestInstance) > 0, ("another person's input does not wait in the pane" + sizes).c_str());
}

/// Case 1, the sender that stays: it fills its share of bytes or of events
/// without passing it, and another person types after it.
void aFullShareRefusesNobodyElse(std::size_t frameBytes)
{
    Daemon d;
    auto& session = busySession(d);
    const Sync::ID128 honestInstance { .low = 7, .high = 7 };
    const std::size_t honestIndex = attachUnder(d, session.id, "honest");
    UbiquittyTest::ScreenClient honest(24, 80, honestInstance);
    Screen::greet(d, honestIndex, honest);
    const std::size_t index = attachUnder(d, session.id, "filler");
    UbiquittyTest::ScreenClient filler(24, 80);
    Screen::greet(d, index, filler);
    REQUIRE(isAttached(d, index) && isAttached(d, honestIndex));

    // Bytes: frames while one more still fits. Events: one-byte frames up to
    // one short of the event share.
    const std::vector<std::uint8_t> frame(frameBytes, 'f');
    const auto held = [&] { return session.screenSource->heldInputBytes(defaultInstance); };
    const auto fits = [&] { return frameBytes > 1 ? held() + frameBytes <= Sync::maxHeldInputBytes : held() + 1 < Sync::maxHeldInputEvents; };
    for (int count = 0; count < 20'000 && fits(); ++count) {
        REQUIRE(filler.sendRaw(frame).has_value());
        Screen::receive(d, index, filler.takeStream(Screen::now()));
        Screen::service(d);
    }
    const std::string sizes = " (" + std::to_string(frameBytes) + "-byte frames)";
    REQUIRE_MSG(isAttached(d, index) && !fits(), ("premise: the sender did not fill its share and stay" + sizes).c_str());

    const std::uint8_t key[] = { 'x' };
    REQUIRE(honest.sendRaw(key).has_value());
    Screen::deliver(d, honestIndex, honest);
    const std::vector<std::uint8_t> burst(frameBytes > 1 ? frameBytes : 1, 'h');
    REQUIRE(honest.sendRaw(burst).has_value());
    Screen::deliver(d, honestIndex, honest);
    CHECK_MSG(isAttached(d, honestIndex) && session.screenSource->heldInputBytes(honestInstance) == 1 + burst.size(),
        ("another person was refused because one sender filled its share" + sizes).c_str());
    CHECK_MSG(isAttached(d, index), ("a sender within its share was closed" + sizes).c_str());
}

/// Case 2: two people paste into a program that reads nothing.
void twoPeoplePaste()
{
    Daemon d;
    auto& session = busySession(d);
    const std::size_t aliceIndex = attachUnder(d, session.id, "alice");
    UbiquittyTest::ScreenClient alice(24, 80, Sync::ID128 { .low = 11, .high = 1 });
    Screen::greet(d, aliceIndex, alice);
    const std::size_t bobIndex = attachUnder(d, session.id, "bob");
    UbiquittyTest::ScreenClient bob(24, 80, Sync::ID128 { .low = 12, .high = 1 });
    Screen::greet(d, bobIndex, bob);
    REQUIRE(alice.sendPaste(std::string(std::size_t { 1 } << 20, 'a')).has_value());
    REQUIRE(bob.sendPaste(std::string(std::size_t { 1 } << 20, 'b')).has_value());
    std::size_t mostHeld = 0;
    for (int round = 0; round < 400; ++round) {
        Screen::deliver(d, aliceIndex, alice);
        Screen::deliver(d, bobIndex, bob);
        mostHeld = std::max(mostHeld, session.screenSource->heldInputBytes());
    }
    CHECK_MSG(isAttached(d, aliceIndex) && isAttached(d, bobIndex), "a person pasting within their window was closed for another's paste");
    CHECK_MSG(mostHeld > Sync::maxPendingInputBytes, ("premise: the pane never held more than one window: " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(mostHeld <= 2 * Sync::maxHeldInputBytes, ("the pane held more than two people's shares: " + std::to_string(mostHeld)).c_str());
    CHECK_MSG(alice.connection().isPasting(1) && bob.connection().isPasting(1), "premise: a paste left whole into a program that reads nothing");
}

/// Case 3: a connection types into three busy panes.
void oneConnectionAcrossPanes()
{
    Daemon d;
    auto& first = busySession(d);
    auto& second = busySession(d);
    auto& third = busySession(d);
    (void)Screen::nameWorkspace(d, first, "busy");
    Screen::splitInto(d, first, second, Ubiquitty::Panes::Orientation::Vertical);
    Screen::splitInto(d, first, third, Ubiquitty::Panes::Orientation::Vertical);
    const std::size_t index = attachUnder(d, first.id, "typist");
    Screen::bindWorkspace(d, index, first);
    UbiquittyTest::ScreenClient typist(24, 80);
    Screen::greet(d, index, typist);
    REQUIRE(isAttached(d, index));
    REQUIRE(typist.connection().paneTerminal(1) && typist.connection().paneTerminal(2) && typist.connection().paneTerminal(3));
    const Sync::ID128 otherInstance { .low = 21, .high = 1 };
    const std::size_t otherIndex = attachUnder(d, first.id, "other");
    UbiquittyTest::ScreenClient other(24, 80, otherInstance);
    Screen::greet(d, otherIndex, other);
    REQUIRE(isAttached(d, otherIndex));

    const std::vector<std::uint8_t> frame(50'000, 't');
    std::size_t sent = 0;
    std::size_t mostInOnePane = 0;
    for (int round = 0; round < 40 && isAttached(d, index); ++round) {
        for (std::uint32_t handle = 1; handle <= 3; ++handle) {
            REQUIRE(typist.sendRaw(handle, frame).has_value());
            sent += frame.size();
        }
        Screen::receive(d, index, typist.takeStream(Screen::now()));
        Screen::service(d);
        for (auto* session : { &first, &second, &third })
            mostInOnePane = std::max(mostInOnePane, session->screenSource->heldInputBytes(defaultInstance));
    }
    CHECK_MSG(wasClosedFor(d, index, Sync::ByeReason::Oversize), "a connection past its bound across its panes was not closed for it");
    CHECK_MSG(sent > Sync::maxConnectionHeldInputBytes && sent <= Sync::maxConnectionHeldInputBytes + 3 * frame.size(),
        ("the connection was not closed once it passed its bound: " + std::to_string(sent)).c_str());
    CHECK_MSG(mostInOnePane < Sync::maxHeldInputBytes, "premise: one pane's share was passed, not the connection's bound");
    const std::vector<std::uint8_t> burst(60'000, 'o');
    REQUIRE(other.sendRaw(burst).has_value());
    Screen::deliver(d, otherIndex, other);
    CHECK_MSG(isAttached(d, otherIndex) && first.screenSource->heldInputBytes(otherInstance) + writtenOf(first, 'o') == burst.size(),
        "another connection typing into a busy pane was refused");
}

/// Case 4: a client whose link is lost keeps the input accepted from it.
void aLostLinkKeepsItsInput()
{
    Daemon d;
    auto& session = busySession(d);
    const std::size_t index = attachUnder(d, session.id, "typist");
    UbiquittyTest::ScreenClient typist(24, 80);
    Screen::greet(d, index, typist);
    const std::vector<std::uint8_t> typed(60'000, 'k');
    REQUIRE(typist.sendRaw(typed).has_value());
    Screen::deliver(d, index, typist);
    const std::size_t held = session.screenSource->heldInputBytes(defaultInstance);
    REQUIRE(held > 0);
    Screen::drop(d, index);
    CHECK_MSG(session.screenSource->heldInputBytes(defaultInstance) == held, "input accepted from a client was dropped with its link");
}

/// Case 4, many times over: connections that type and lose their links fill
/// the host toward its bound.
void connectionsThatComeAndGo()
{
    // Four workspaces of two busy panes, so no pane runs out of the readers it
    // keeps for each attachment that may resume.
    Daemon d;
    using Session = std::remove_reference_t<decltype(busySession(d))>;
    std::vector<std::pair<Session*, Session*>> workspaces;
    for (int workspace = 0; workspace < 4; ++workspace) {
        auto& first = busySession(d);
        auto& second = busySession(d);
        (void)Screen::nameWorkspace(d, first, "busy " + std::to_string(workspace));
        Screen::splitInto(d, first, second, Ubiquitty::Panes::Orientation::Vertical);
        workspaces.emplace_back(&first, &second);
    }
    const std::vector<std::uint8_t> frame(60'000, 'c');
    std::size_t held = 0;
    bool wasClosed = false;
    for (std::uint64_t connection = 0; connection < 100 && !wasClosed; ++connection) {
        auto& first = *workspaces[connection % workspaces.size()].first;
        const std::size_t index = attachUnder(d, first.id, "churn");
        Screen::bindWorkspace(d, index, first);
        UbiquittyTest::ScreenClient client(24, 80, Sync::ID128 { .low = 100 + connection, .high = 1 });
        Screen::greet(d, index, client);
        REQUIRE(isAttached(d, index));
        // Eight frames into each of its two panes: below a share and below
        // the connection's bound.
        std::size_t typed = 0;
        for (std::uint32_t handle : { 1u, 2u }) {
            for (int count = 0; count < 8 && isAttached(d, index); ++count) {
                const bool hasRoom = Screen::heldInputRoom(d) >= frame.size();
                REQUIRE(client.sendRaw(handle, frame).has_value());
                Screen::receive(d, index, client.takeStream(Screen::now()));
                Screen::service(d);
                if (!isAttached(d, index)) {
                    CHECK_MSG(!hasRoom && wasClosedFor(d, index, Sync::ByeReason::Oversize), "a connection was closed while the host had room for it");
                    wasClosed = true;
                } else
                    typed += frame.size();
            }
        }
        // Its link is lost and the host reaps the connection, as its loop
        // does; the attachment, and the input accepted from it, stay. What a
        // connection closed for passing the bound had queued goes with it.
        if (!wasClosed) {
            held += typed;
            Screen::reap(d, index);
        }
    }
    CHECK_MSG(wasClosed, "connections that came and went were never held to the host's bound");
    std::size_t total = 0;
    for (const auto& [first, second] : workspaces)
        total += first->screenSource->heldInputBytes() + second->screenSource->heldInputBytes();
    // Each busy program's queue took its first 4 KiB.
    CHECK_MSG(total + 8 * 4'096 >= held && total <= 64 * Sync::maxConnectionHeldInputBytes,
        ("the host did not keep what it took from links since lost, within its bound: " + std::to_string(total) + " of " + std::to_string(held)).c_str());
}

/// Case 5: a client turns its focus over and over.
void focusTurnedOverAndOver()
{
    Daemon d;
    auto& session = busySession(d);
    // The program asks to be told of focus.
    Ubiquitty::processBytes(session.term.get(), "\x1b[?1004h");
    const std::size_t index = attachUnder(d, session.id, "fidget");
    UbiquittyTest::ScreenClient fidget(24, 80);
    Screen::greet(d, index, fidget);
    for (int turn = 0; turn < 4'000 && isAttached(d, index); ++turn) {
        REQUIRE(fidget.sendFocus(turn % 2 ? Ubiquitty::Modes::FocusState::Unfocused : Ubiquitty::Modes::FocusState::Focused));
        if (turn % 64 == 63) {
            Screen::receive(d, index, fidget.takeStream(Screen::now()));
            Screen::service(d);
        }
    }
    Screen::receive(d, index, fidget.takeStream(Screen::now()));
    Screen::service(d);
    // Two reports at most: one that may be partly written at the head, and
    // the newest behind it.
    CHECK_MSG(session.screenSource->heldInputBytes() <= Sync::maxInputShareSlack, ("the pane held a focus report for every turn: " + std::to_string(session.screenSource->heldInputBytes())).c_str());
}

/// Case 6: a client writes calls and reads none of the answers.
void callsWhoseAnswersAreNeverRead()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = attachUnder(d, session.id, "caller");
    UbiquittyTest::ScreenClient caller(24, 80);
    Screen::greet(d, index, caller);
    REQUIRE(isAttached(d, index));
    const std::size_t bound = Sync::HostPolicy {}.maxQueuedInteractiveBytes;

    // What the host does not read waits in the socket, here in `unread`.
    std::vector<std::uint8_t> unread;
    std::size_t mostQueued = 0;
    bool wasHeld = false;
    int calls = 0;
    for (int pass = 0; pass < 2'000; ++pass) {
        for (int count = 0; count < 64; ++count, ++calls)
            REQUIRE(caller.call("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(calls + 1) + ",\"method\":\"host.describe\",\"params\":{}}"));
        const std::vector<std::uint8_t> more = caller.takeStream(Screen::now());
        unread.insert(unread.end(), more.begin(), more.end());
        if (Screen::isInputHeld(d, index)) {
            wasHeld = true;
        } else {
            Screen::receive(d, index, unread);
            unread.clear();
        }
        Screen::service(d);
        mostQueued = std::max(mostQueued, Screen::connection(d, index)->queuedBytes());
    }
    CHECK_MSG(wasHeld, "the host never stopped reading a client that reads none of its answers");
    // One pass's answers may land past the bound before the host stops.
    CHECK_MSG(mostQueued <= 2 * bound, ("the host queued answers far past its bound: " + std::to_string(mostQueued) + " of " + std::to_string(bound)).c_str());
    CHECK_MSG(isAttached(d, index), "a client held for not reading was closed");

    std::size_t answered = 0;
    for (int round = 0; round < 4'000; ++round) {
        Screen::deliver(d, index, caller);
        const std::size_t taken = caller.takeResults().size();
        answered += taken;
        if (!taken && !Screen::isInputHeld(d, index))
            break;
    }
    CHECK_MSG(answered > 0 && isAttached(d, index), "the client was not answered once it read");
}

/// Case 7: a resume names the most panes a greeting carries.
void resumeNamingTheMostPanes()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t heldIndex = attachUnder(d, session.id, "alice");
    Sync::ReplicaSet replicas;
    UbiquittyTest::ScreenClient held(24, 80, replicas, std::nullopt);
    Screen::greet(d, heldIndex, held);
    REQUIRE(held.connection().welcome().has_value());
    const Sync::Welcome welcome = *held.connection().welcome();
    Screen::drop(d, heldIndex);

    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    for (std::uint64_t pane = 0; pane < Sync::maxPanesPerConnection; ++pane)
        request.panes.push_back({ .paneID = { .low = 0x5EED, .high = pane + 1 }, .ackedGeneration = 1, .ackedRowHashesDigest = {}, .historyEpoch = 0, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live });
    const std::size_t resumedIndex = attachUnder(d, session.id, "alice");
    UbiquittyTest::ScreenClient resumed(24, 80, replicas, request);
    Screen::greet(d, resumedIndex, resumed);
    const auto again = resumed.connection().welcome();
    REQUIRE(again.has_value());
    CHECK_MSG(again->attachmentID == welcome.attachmentID, "a resume naming the most panes did not continue its attachment");
    CHECK_MSG(again->resumed.size() == Sync::maxPanesPerConnection
            && std::ranges::all_of(again->resumed, [](const Sync::PaneResumed& pane) { return pane.outcome == Sync::ResumeOutcome::Gone; }),
        "the panes the attachment never held were not each named gone");
    CHECK_MSG(isAttached(d, resumedIndex), "a resume naming the most panes was not attached");
}

} // namespace

int main()
{
    typesPastTheBound(60'000, 1);
    typesPastTheBound(1, 64);
    aFullShareRefusesNobodyElse(60'000);
    aFullShareRefusesNobodyElse(1);
    twoPeoplePaste();
    oneConnectionAcrossPanes();
    aLostLinkKeepsItsInput();
    connectionsThatComeAndGo();
    focusTurnedOverAndOver();
    callsWhoseAnswersAreNeverRead();
    resumeNamingTheMostPanes();
    if (failures == 0)
        std::printf("hostile_clients: a client typing past its share is closed and its input dropped while another person's typing is taken; "
                    "two people pasting into a busy program both stay; a connection past its bound across panes is closed and nobody else; "
                    "a lost link keeps its input; focus turned over and over holds one report; a client that reads no answers is no longer read past its bound; "
                    "a resume naming the most panes is answered\n");
    return failures == 0 ? 0 : 1;
}
