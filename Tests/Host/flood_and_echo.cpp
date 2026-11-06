// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/flood_and_echo.cpp — a keystroke's echo leaves ahead of a flood.
//
// When the window opens, the host serves first the panes holding input whose
// effect the client has not been sent, then the rest by deficit round-robin
// over the bytes each was sent (Docs/sync.md §6.4). A flood fills a client's
// window; the client types; the program echoes; the client reads what it was
// sent and acknowledges it. Two cases, each driving the daemon's own screen
// plane:
//
//   1. Beside the flood: the client types into a quiet pane next to the
//      flooding one, which is bound first.
//   2. Inside the flood: the client types into a flooding pane, bound after
//      another flooding pane, so bind order would serve the other first.
//
// Each asserts an order and a count, not a time: of the updates the host
// sends from the echo's frame on, the first is the typed pane's, and the
// client holds the echo once it has read what the first service after its
// acknowledgement sent.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// A redraw of the top `rows` rows, every cell changed from the frame
/// before.
///
/// - Parameters:
///   - frame: The frame's number.
///   - rows: How many rows to redraw from the top.
///   - columns: How many columns each row fills.
/// - Returns: The bytes a program writes.
std::string redraw(int frame, int rows, int columns)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row) {
        bytes += "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int column = 0; column < columns; ++column)
            bytes += static_cast<char>('a' + (frame * 7 + row * 3 + column) % 26);
    }
    return bytes;
}

/// The handles of the updates in a run of stream bytes, in the order sent.
class UpdateOrder {
public:
    /// Reads the next bytes the daemon queued.
    ///
    /// - Parameter bytes: Whole frames.
    void read(std::span<const std::uint8_t> bytes)
    {
        m_decoder.append(bytes);
        while (const std::optional<Sync::Frame> frame = m_decoder.next()) {
            if (frame->kind == Sync::FrameKind::Update)
                m_handles.push_back(frame->channel);
        }
    }

    /// The handles, one per update, in the order the updates were sent.
    [[nodiscard]] const std::vector<std::uint32_t>& handles() const { return m_handles; }

private:
    Sync::FrameDecoder m_decoder;
    std::vector<std::uint32_t> m_handles;
};

/// The rows and columns of every pane and of the client's window. One size
/// for all, so typing applies no pending geometry and every frame of a pane
/// travels as an update rather than a checkpoint.
constexpr int paneRows = 50;
constexpr int paneColumns = 160;

/// One daemon whose workspace a typing client is attached to.
class TypedWorkspace {
public:
    /// Makes the workspace's sessions, the first its root, and attaches a
    /// client that may type into them.
    ///
    /// - Parameter paneCount: How many panes, in bind order.
    explicit TypedWorkspace(std::size_t paneCount)
        : m_client(paneRows, paneColumns)
    {
        while (m_sessions.size() < paneCount) {
            auto& session = Screen::makeSession(m_daemon, paneRows, paneColumns, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
            Ubiquitty::processBytes(session.term.get(), "$ ");
            Screen::publish(m_daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
            m_sessions.push_back(&session);
        }
        (void)Screen::nameWorkspace(m_daemon, *m_sessions.front(), "flood");
        for (std::size_t place = 1; place < m_sessions.size(); ++place)
            (void)Screen::tabInto(m_daemon, *m_sessions.front(), *m_sessions[place]);
        m_index = Screen::attach(m_daemon, m_sessions.front()->id);
        Screen::grantScope(m_daemon, m_index, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(m_daemon, m_index, "typist");
        Screen::markInteractiveUser(m_daemon, m_index);
        Screen::bindWorkspace(m_daemon, m_index, *m_sessions.front());
        Screen::greet(m_daemon, m_index, m_client);
        for (std::size_t place = 0; place < m_sessions.size(); ++place) {
            m_handles.push_back(0);
            for (std::uint32_t handle = 1; handle <= m_sessions.size(); ++handle) {
                if (const auto pane = m_client.boundPane(handle); pane && pane->low == m_sessions[place]->id)
                    m_handles.back() = handle;
            }
        }
        // The panes are captured once a client shows them; their first
        // frames go now, so the client starts holding every pane.
        for (auto* session : m_sessions) {
            session->isFrameDue = true;
            Screen::publish(m_daemon, *session);
        }
        Screen::deliver(m_daemon, m_index, m_client);
    }

    /// Whether the client attached and holds every pane's newest frame.
    [[nodiscard]] bool isReady() const
    {
        const auto* connection = Screen::connection(m_daemon, m_index);
        if (!m_client.hasStarted() || !connection || connection->state() != Sync::ConnectionState::Attached)
            return false;
        for (std::size_t place = 0; place < m_sessions.size(); ++place) {
            if (!m_handles[place] || m_client.connection().paneGeneration(m_handles[place]) != m_sessions[place]->screenSource->generation())
                return false;
        }
        return true;
    }

    /// Writes to a pane's program output and publishes the frame it leaves.
    ///
    /// - Parameters:
    ///   - place: The pane's place in bind order.
    ///   - bytes: What the program writes.
    void write(std::size_t place, std::string_view bytes)
    {
        Ubiquitty::processBytes(m_sessions[place]->term.get(), bytes);
        m_sessions[place]->isFrameDue = true;
        Screen::publish(m_daemon, *m_sessions[place]);
    }

    /// Runs the loop's screen step and takes what it queued for the client,
    /// as a carrier that accepts everything does.
    ///
    /// - Returns: The bytes, whole frames.
    std::vector<std::uint8_t> serviceAndTake()
    {
        Screen::service(m_daemon);
        return Screen::takeOutput(m_daemon, m_index);
    }

    /// Types into a pane and hands the daemon the client's frames.
    ///
    /// - Parameters:
    ///   - place: The pane's place in bind order.
    ///   - text: What the client types.
    /// - Returns: Whether the client took the input.
    bool type(std::size_t place, std::string_view text)
    {
        const std::span<const std::uint8_t> bytes { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
        if (!m_client.sendRaw(m_handles[place], bytes))
            return false;
        Screen::receive(m_daemon, m_index, m_client.takeStream(Screen::now()));
        return true;
    }

    /// The client reads bytes it was sent and acknowledges them.
    ///
    /// - Parameter bytes: What the carrier held for it.
    void read(std::span<const std::uint8_t> bytes)
    {
        m_client.read(bytes, Screen::now());
        Screen::receive(m_daemon, m_index, m_client.takeStream(Screen::now()));
    }

    /// The input a pane's program was written.
    ///
    /// - Parameter place: The pane's place in bind order.
    /// - Returns: The bytes, in order.
    [[nodiscard]] const std::string& programInput(std::size_t place) const
    {
        return static_cast<const Ubiquitty::TestDoubles::StreamDouble&>(*m_sessions[place]->stream).written;
    }

    /// Whether the client holds a pane's newest frame and shows what its
    /// program's terminal shows.
    ///
    /// - Parameter place: The pane's place in bind order.
    /// - Returns: The first difference, or empty when the client holds it.
    [[nodiscard]] std::string differenceAt(std::size_t place) const
    {
        if (m_client.connection().paneGeneration(m_handles[place]) != m_sessions[place]->screenSource->generation())
            return "the client does not hold the pane's newest frame";
        return UbiquittyTest::firstDisagreement(*m_sessions[place]->term, m_client.connection().paneTerminal(m_handles[place]));
    }

    /// The handle the client knows a pane by.
    [[nodiscard]] std::uint32_t handle(std::size_t place) const { return m_handles[place]; }
    /// The times the window refused a frame that was due.
    [[nodiscard]] std::uint64_t windowRefusals() const { return Screen::connection(m_daemon, m_index)->counters().windowRefusals; }
    /// Whether the daemon dropped the client.
    [[nodiscard]] bool isDropped() const { return Screen::didOverflow(m_daemon, m_index); }
    /// The bytes the connection encoded and has not handed the daemon's
    /// queue; frames encoded before the echo would leave ahead of it.
    [[nodiscard]] std::size_t encodedUnsentBytes() const { return Screen::connection(m_daemon, m_index)->queuedBytes(); }

private:
    Daemon m_daemon;
    using Session = std::remove_reference_t<decltype(Screen::makeSession(std::declval<Daemon&>(), 0, 0))>;
    std::vector<Session*> m_sessions;
    std::vector<std::uint32_t> m_handles;
    std::size_t m_index = 0;
    UbiquittyTest::ScreenClient m_client;
};

/// Floods the panes until the window refuses one, holding everything the
/// client was sent unread.
///
/// - Parameters:
///   - workspace: The workspace.
///   - flooding: The places of the panes that flood, and the rows each redraws.
///   - relayed: Receives what the carrier took.
/// - Returns: The number of the next frame, or nothing if the window never refused.
std::optional<int> floodUntilTheWindowRefuses(TypedWorkspace& workspace, std::span<const std::pair<std::size_t, int>> flooding, std::vector<std::uint8_t>& relayed)
{
    const std::uint64_t refusalsBefore = workspace.windowRefusals();
    for (int frame = 0; frame < 400; ++frame) {
        for (const auto& [place, rows] : flooding)
            workspace.write(place, redraw(frame, rows, paneColumns));
        const std::vector<std::uint8_t> taken = workspace.serviceAndTake();
        relayed.insert(relayed.end(), taken.begin(), taken.end());
        if (workspace.windowRefusals() > refusalsBefore)
            return frame + 1;
    }
    return std::nullopt;
}

/// Case 1: the client types into a quiet pane beside a flooding one.
void anEchoBesideAFloodLeavesFirst()
{
    constexpr std::size_t flooding = 0;
    constexpr std::size_t typing = 1;
    TypedWorkspace workspace(2);
    REQUIRE_MSG(workspace.isReady(), "beside: premise: the client did not attach to both panes");

    std::vector<std::uint8_t> relayed;
    const std::array<std::pair<std::size_t, int>, 1> flood { { { flooding, paneRows } } };
    const std::optional<int> nextFrame = floodUntilTheWindowRefuses(workspace, flood, relayed);
    REQUIRE_MSG(nextFrame.has_value(), "beside: premise: the flood never filled the window");

    REQUIRE_MSG(workspace.type(typing, "x"), "beside: premise: the client did not take the keystroke");
    const std::vector<std::uint8_t> typed = workspace.serviceAndTake();
    relayed.insert(relayed.end(), typed.begin(), typed.end());
    REQUIRE_MSG(workspace.programInput(typing).ends_with("x"), "beside: premise: the keystroke did not reach the program");
    REQUIRE_MSG(workspace.encodedUnsentBytes() == 0, "beside: premise: frames encoded before the echo were still waiting to leave");

    // The program echoes while the flood goes on.
    UpdateOrder order;
    workspace.write(typing, "x");
    workspace.write(flooding, redraw(*nextFrame, paneRows, paneColumns));
    const std::vector<std::uint8_t> echoed = workspace.serviceAndTake();
    order.read(echoed);
    relayed.insert(relayed.end(), echoed.begin(), echoed.end());

    // The client reads everything it was sent, and its acknowledgement opens
    // the window.
    workspace.read(relayed);
    const std::vector<std::uint8_t> opened = workspace.serviceAndTake();
    order.read(opened);
    workspace.read(opened);

    CHECK_MSG(!workspace.isDropped(), "beside: the client was dropped");
    REQUIRE_MSG(!order.handles().empty(), "beside: no update was sent once the window opened");
    CHECK_MSG(order.handles().front() == workspace.handle(typing),
        ("beside: the first update after the echo was the flooding pane's (handle " + std::to_string(order.handles().front()) + "), not the echo's").c_str());
    const std::string difference = workspace.differenceAt(typing);
    CHECK_MSG(difference.empty(), ("beside: the echo had not reached the client by the first service after its acknowledgement: " + difference).c_str());
    CHECK_MSG(std::ranges::count(order.handles(), workspace.handle(flooding)) > 0, "beside: the flooding pane was not served once the echo had gone");
}

/// Case 2: the client types into a flooding pane, bound after another
/// flooding pane.
void anEchoInsideAFloodLeavesFirst()
{
    constexpr std::size_t smaller = 0;
    constexpr std::size_t typing = 1;
    TypedWorkspace workspace(2);
    REQUIRE_MSG(workspace.isReady(), "inside: premise: the client did not attach to both panes");

    // Both panes flood; the typing pane's bottom row is left for the echo.
    std::vector<std::uint8_t> relayed;
    const std::array<std::pair<std::size_t, int>, 2> flood { { { smaller, paneRows / 2 }, { typing, paneRows - 1 } } };
    const std::optional<int> nextFrame = floodUntilTheWindowRefuses(workspace, flood, relayed);
    REQUIRE_MSG(nextFrame.has_value(), "inside: premise: the flood never filled the window");

    REQUIRE_MSG(workspace.type(typing, "y"), "inside: premise: the client did not take the keystroke");
    const std::vector<std::uint8_t> typed = workspace.serviceAndTake();
    relayed.insert(relayed.end(), typed.begin(), typed.end());
    REQUIRE_MSG(workspace.programInput(typing).ends_with("y"), "inside: premise: the keystroke did not reach the program");
    REQUIRE_MSG(workspace.encodedUnsentBytes() == 0, "inside: premise: frames encoded before the echo were still waiting to leave");

    // The program echoes on its bottom row and both panes go on flooding.
    UpdateOrder order;
    workspace.write(typing, redraw(*nextFrame, paneRows - 1, paneColumns) + "\x1b[" + std::to_string(paneRows) + ";1Hy");
    workspace.write(smaller, redraw(*nextFrame, paneRows / 2, paneColumns));
    const std::vector<std::uint8_t> echoed = workspace.serviceAndTake();
    order.read(echoed);
    relayed.insert(relayed.end(), echoed.begin(), echoed.end());

    workspace.read(relayed);
    const std::vector<std::uint8_t> opened = workspace.serviceAndTake();
    order.read(opened);
    workspace.read(opened);

    CHECK_MSG(!workspace.isDropped(), "inside: the client was dropped");
    REQUIRE_MSG(!order.handles().empty(), "inside: no update was sent once the window opened");
    CHECK_MSG(order.handles().front() == workspace.handle(typing),
        ("inside: the first update after the echo was the other pane's (handle " + std::to_string(order.handles().front()) + "), not the typed pane's").c_str());
    const std::string difference = workspace.differenceAt(typing);
    CHECK_MSG(difference.empty(), ("inside: the echo had not reached the client by the first service after its acknowledgement: " + difference).c_str());
    CHECK_MSG(std::ranges::count(order.handles(), workspace.handle(smaller)) > 0, "inside: the other flooding pane was not served once the echo had gone");
}

} // namespace

int main()
{
    anEchoBesideAFloodLeavesFirst();
    anEchoInsideAFloodLeavesFirst();
    if (failures == 0)
        std::printf("flood_and_echo: beside a flood and inside one, the typed pane's update is the first the host sends from "
                    "the echo's frame on, and the client holds the echo once it reads the first service after its acknowledgement\n");
    return failures == 0 ? 0 : 1;
}
