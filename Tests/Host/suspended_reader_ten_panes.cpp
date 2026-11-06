// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/suspended_reader_ten_panes.cpp — a client that stops reading
// holds at most one window, however many panes write to it.
//
// The window counts every frame the host sends a connection, records among
// them, and once it is full the host neither encodes nor queues: each pane is
// owed (Docs/sync.md §6.4). One client is attached to a workspace of ten
// panes. Every pane redraws its screen and retitles itself every round, and
// the client stops reading. It stops two ways in turn:
//
//   1. Its socket takes nothing more: the daemon's queue for it holds less
//      than two capped frames, and it is not dropped.
//   2. Its carrier takes everything again, as a relay or an SSH server's
//      buffer would, and the client still acknowledges nothing: the bytes
//      handed to the carrier stop at one window, the window refuses, and what
//      the connection retains stays within one window and each pane's newest
//      records, however many revisions pass.
//
// Once it reads again, every pane reaches its newest picture and title.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

constexpr std::size_t paneCount = 10;

/// A title of one width whatever its numbers, so every revision of a pane's
/// facts is the same size.
///
/// - Parameters:
///   - pane: The pane's place in the workspace.
///   - round: The round.
/// - Returns: The title.
std::string titleOf(std::size_t pane, int round)
{
    std::string title = "pane " + std::to_string(pane) + " round " + std::to_string(round) + " ";
    title.resize(120, '.');
    return title;
}

/// A whole screen redrawn, every cell changed from the round before, and the
/// pane retitled.
///
/// - Parameters:
///   - pane: The pane's place in the workspace.
///   - round: The round.
/// - Returns: The bytes a program writes.
std::string roundOf(std::size_t pane, int round)
{
    std::string bytes = "\x1b]0;" + titleOf(pane, round) + "\x07";
    for (int row = 0; row < 24; ++row) {
        bytes += "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int column = 0; column < 80; ++column)
            bytes += static_cast<char>('a' + (round * 7 + row * 3 + column + static_cast<int>(pane)) % 26);
    }
    return bytes;
}

/// Writes ten panes to a client that stops reading, then reads again.
void tenPanesToASuspendedReader()
{
    Daemon d;
    auto& first = Screen::makeSession(d, 24, 80);
    using Session = std::remove_reference_t<decltype(first)>;
    std::vector<Session*> sessions { &first };
    while (sessions.size() < paneCount)
        sessions.push_back(&Screen::makeSession(d, 24, 80));
    (void)Screen::nameWorkspace(d, first, "ten");
    for (std::size_t pane = 1; pane < sessions.size(); ++pane)
        (void)Screen::tabInto(d, first, *sessions[pane]);
    // Titled at their full width from the start, so the structure the
    // connection holds is the size it keeps.
    for (std::size_t pane = 0; pane < sessions.size(); ++pane) {
        Ubiquitty::processBytes(sessions[pane]->term.get(), roundOf(pane, 0));
        Screen::publish(d, *sessions[pane], Ubiquitty::Runtime::FrameClass::Structural);
    }

    const std::size_t index = Screen::attach(d, first.id);
    Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(d, index, "reader");
    Screen::markInteractiveUser(d, index);
    Screen::bindWorkspace(d, index, first);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(d, index, client);
    const Sync::HostConnection* connection = Screen::connection(d, index);
    REQUIRE_MSG(client.hasStarted() && connection && connection->state() == Sync::ConnectionState::Attached, "premise: the client attached");

    // Which handle shows which session: a session's pane is named by its id.
    std::vector<std::uint32_t> handles(sessions.size(), 0);
    for (std::uint32_t handle = 1; handle <= paneCount; ++handle) {
        const auto pane = client.boundPane(handle);
        for (std::size_t place = 0; pane && place < sessions.size(); ++place) {
            if (pane->low == sessions[place]->id)
                handles[place] = handle;
        }
    }
    REQUIRE_MSG(std::ranges::count(handles, 0u) == 0, "premise: the workspace's ten panes were not all bound on the one connection");
    // The panes are captured once a client shows them; their first frames go
    // now, so the client starts holding every pane.
    for (Session* session : sessions) {
        session->isFrameDue = true;
        Screen::publish(d, *session);
    }
    Screen::deliver(d, index, client);
    for (std::size_t pane = 0; pane < sessions.size(); ++pane)
        REQUIRE_MSG(client.connection().paneGeneration(handles[pane]) == sessions[pane]->screenSource->generation(), "premise: a pane did not reach the client before it stopped reading");
    const std::size_t quietRetained = connection->retainedOutboundPayloadBytes();

    int round = 1;
    std::size_t largestRecord = 0;
    const auto writeRound = [&] {
        for (std::size_t pane = 0; pane < sessions.size(); ++pane) {
            Ubiquitty::processBytes(sessions[pane]->term.get(), roundOf(pane, round));
            sessions[pane]->isFrameDue = true;
            Screen::publish(d, *sessions[pane]);
            std::vector<std::uint8_t> body;
            Sync::encodePaneFacts(body, sessions[pane]->screenSource->facts());
            largestRecord = std::max(largestRecord, body.size());
        }
        ++round;
    };

    // 1. The socket takes nothing: the daemon queues less than two frames.
    std::size_t mostQueued = 0;
    for (int pass = 0; pass < 50; ++pass) {
        writeRound();
        Screen::service(d);
        mostQueued = std::max(mostQueued, Screen::pending(d, index));
        CHECK_MSG(!Screen::didOverflow(d, index), ("a client that stopped reading was dropped at round " + std::to_string(round)).c_str());
    }
    CHECK_MSG(mostQueued > 0, "premise: the daemon queued nothing for the client that stopped reading");
    CHECK_MSG(mostQueued < 2 * Screen::lowWaterBytes(),
        ("the daemon queued " + std::to_string(mostQueued) + " bytes for a client whose socket took nothing; two frames are " + std::to_string(2 * Screen::lowWaterBytes())).c_str());

    // 2. The carrier takes everything, and the client acknowledges nothing.
    const std::uint64_t refusalsBefore = connection->counters().windowRefusals;
    std::vector<std::uint8_t> relayed;
    std::uint64_t window = 0;
    std::size_t mostRetained = 0;
    for (int pass = 0; pass < 300; ++pass) {
        writeRound();
        const std::uint64_t at = Screen::now();
        Screen::service(d, at);
        const std::vector<std::uint8_t> taken = Screen::takeOutput(d, index);
        relayed.insert(relayed.end(), taken.begin(), taken.end());
        window = connection->pacer().windowBytes(at);
        const std::uint64_t outstanding = connection->pacer().outstandingBytes();
        mostRetained = std::max(mostRetained, connection->retainedOutboundPayloadBytes());
        CHECK_MSG(outstanding <= window,
            ("round " + std::to_string(round) + ": " + std::to_string(outstanding) + " bytes outstanding against a window of " + std::to_string(window)).c_str());
        CHECK_MSG(relayed.size() <= outstanding, "premise: bytes left the daemon that its window did not count");
        CHECK_MSG(!Screen::didOverflow(d, index), ("a client that acknowledged nothing was dropped at round " + std::to_string(round)).c_str());
    }
    CHECK_MSG(connection->counters().windowRefusals > refusalsBefore, "premise: the window never refused a pane, so it bounded nothing");
    CHECK_MSG(relayed.size() <= window,
        ("the carrier was handed " + std::to_string(relayed.size()) + " bytes the client never acknowledged; the window is " + std::to_string(window)).c_str());
    const std::size_t retainedBound = quietRetained + window + paneCount * 2 * (largestRecord + Sync::maxEnvelopeHeaderBytes);
    CHECK_MSG(mostRetained <= retainedBound,
        ("the connection retained " + std::to_string(mostRetained) + " bytes over " + std::to_string(round) + " rounds; one window and each pane's newest records are " + std::to_string(retainedBound)).c_str());

    // It reads again: what the carrier held, then whatever it is owed.
    client.read(relayed, Screen::now());
    Screen::receive(d, index, client.takeStream(Screen::now()));
    Screen::deliver(d, index, client);
    for (std::size_t pane = 0; pane < sessions.size(); ++pane) {
        const std::uint32_t handle = handles[pane];
        CHECK_MSG(client.connection().paneGeneration(handle) == sessions[pane]->screenSource->generation(),
            ("pane " + std::to_string(pane) + " did not reach its newest frame once the client read").c_str());
        const std::string difference = UbiquittyTest::firstDisagreement(*sessions[pane]->term, client.connection().paneTerminal(handle));
        CHECK_MSG(difference.empty(), ("pane " + std::to_string(pane) + ": " + difference).c_str());
        const Sync::PaneFacts* facts = client.connection().paneFacts(handle);
        CHECK_MSG(facts && facts->programTitle == titleOf(pane, round - 1), ("pane " + std::to_string(pane) + " does not show its newest title").c_str());
    }
    CHECK_MSG(client.connection().counters().resyncsSent == 0, "the client asked for a resync");

    if (failures == 0)
        std::printf("suspended_reader_ten_panes: ten panes wrote %d rounds to a client that stopped reading; its socket queue "
                    "peaked at %zu bytes, the carrier was handed %zu unacknowledged bytes against a window of %llu, and every "
                    "pane reached its newest picture and title once it read\n",
            round - 1, mostQueued, relayed.size(), static_cast<unsigned long long>(window));
}

} // namespace

int main()
{
    tenPanesToASuspendedReader();
    return failures == 0 ? 0 : 1;
}
