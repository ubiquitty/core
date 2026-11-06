// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/ScreenPlaneFixture.h — the Sync screen plane for a test that moves a
// terminal's state across the wire, or stands in for the host a client core
// talks to.
//
// `copyThroughCheckpoint` is the round trip a frontend's state takes when a
// window is shared or a client attaches. `ScreenPlaneHost` is the daemon's end
// of one connection's screen plane, without a daemon: it reads the direct
// envelopes a `SyncClientCore` writes, welcomes its `Greet`, binds the session's
// pane under the identity the daemon mints, and publishes the terminal it was
// given as the frames a real host sends.
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace UbiquittyTest {

/// Encodes a checkpoint of a terminal.
///
/// - Parameters:
///   - host: The terminal to encode.
///   - generation: The generation the checkpoint names.
/// - Returns: The body, empty when the terminal cannot be said exactly.
inline std::vector<std::uint8_t> checkpointBody(const Ubiquitty::Terminal& host, std::uint64_t generation = 1)
{
    Ubiquitty::Protocol::Sync::ScreenPublisher publisher;
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::AttachmentBaseline baseline;
    if (!publisher.capture(host) || !publisher.checkpoint(host, generation, body, baseline))
        body.clear();
    return body;
}

/// Applies a checkpoint of one terminal to another.
///
/// - Parameters:
///   - host: The terminal whose state travels.
///   - client: The terminal that takes it.
/// - Returns: What the apply answered.
inline Ubiquitty::Protocol::Sync::ApplyOutcome copyThroughCheckpoint(const Ubiquitty::Terminal& host, Ubiquitty::Terminal& client)
{
    const std::vector<std::uint8_t> body = checkpointBody(host);
    Ubiquitty::Protocol::Sync::ScreenReplica replica;
    return Ubiquitty::Protocol::Sync::applyCheckpoint(client, body, replica);
}

/// Shows a bound pane on a host connection, as the `View{Live}` a client
/// sends on its `PaneBind` does, so a test reads the screen that follows the
/// bind without the round trip a real client's view costs.
///
/// - Parameters:
///   - connection: The host's end.
///   - handle: The pane's handle.
///   - now: The host clock.
inline void showPane(Ubiquitty::Protocol::Sync::HostConnection& connection, std::uint32_t handle, std::uint64_t now)
{
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, Ubiquitty::Protocol::Sync::View { .state = Ubiquitty::Protocol::Sync::ViewState::Live });
    std::vector<std::uint8_t> frame;
    if (Ubiquitty::Protocol::Sync::appendFrame(frame, Ubiquitty::Protocol::Sync::FrameKind::View, handle, body))
        connection.receiveStream(frame, now);
}

/// A pane's program as a client's input reaches it: every event in order,
/// unencoded, since the fixture has no modes to encode one under, and the
/// bytes of the `Raw` ones beside them.
class TypedInput final : public Ubiquitty::Protocol::Sync::InputWriter {
public:
    /// Takes one event.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose event it is.
    ///   - intent: The event.
    /// - Returns: `Written`.
    [[nodiscard]] Ubiquitty::Protocol::Sync::WriteStatus write(Ubiquitty::Protocol::Sync::ID128, const Ubiquitty::Protocol::Sync::InputIntent& intent) override
    {
        if (const auto* raw = std::get_if<Ubiquitty::Protocol::Sync::RawInput>(&intent))
            m_bytes.append(raw->bytes.begin(), raw->bytes.end());
        m_events.push_back(intent);
        return Ubiquitty::Protocol::Sync::WriteStatus::Written;
    }

    /// The bytes written as they were, in order.
    [[nodiscard]] const std::string& bytes() const { return m_bytes; }

    /// Every event written, in order: bytes, keys, pointer events, pastes.
    [[nodiscard]] const std::vector<Ubiquitty::Protocol::Sync::InputIntent>& events() const { return m_events; }

private:
    std::string m_bytes;
    std::vector<Ubiquitty::Protocol::Sync::InputIntent> m_events;
};

/// The host's end of one connection's screen plane.
class ScreenPlaneHost {
public:
    /// Creates the host end for a session.
    ///
    /// - Parameters:
    ///   - terminal: The session's terminal; it must outlive this.
    ///   - sessionID: The session, which the pane identity carries.
    ///   - daemonInstanceID: The daemon instance, which the pane identity
    ///     carries beside the session.
    ///   - datagramMaxBytes: The largest datagram the carrier sends, 0 for a
    ///     carrier without datagrams.
    ///   - role: The authority actually granted by the host.
    ///   - hostInstanceID: The independent full Sync identity, when specified.
    ScreenPlaneHost(const Ubiquitty::Terminal& terminal, std::uint32_t sessionID, std::uint64_t daemonInstanceID, std::size_t datagramMaxBytes = 0, Ubiquitty::Protocol::Sync::Role role = Ubiquitty::Protocol::Sync::Role::Owner, std::optional<Ubiquitty::Protocol::Sync::ID128> hostInstanceID = std::nullopt)
        : m_source(terminal, { .low = sessionID, .high = daemonInstanceID })
        , m_connection(Ubiquitty::Protocol::Sync::measuredHostPolicy(datagramMaxBytes), identity(hostInstanceID.value_or(Ubiquitty::Protocol::Sync::ID128 { daemonInstanceID, 0 }), role, sessionID, daemonInstanceID))
    {
        m_source.setInputWriter(m_typed);
    }

    /// What the client's input came to at the pane's program.
    [[nodiscard]] const TypedInput& typed() const { return m_typed; }

    /// Reads what the client wrote; the canonical envelopes reach the
    /// screen plane, and the pane is bound once the client is welcomed.
    ///
    /// - Parameter clientFrames: Canonical stream bytes, possibly fragmented.
    void receive(std::span<const std::uint8_t> clientFrames)
    {
        m_connection.receiveStream(clientFrames, advance());
        if (m_handle == 0 && m_connection.state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached) {
            m_handle = m_connection.bindPane(m_source);
            if (m_handle != 0)
                showPane(m_connection, m_handle, advance());
        }
    }

    /// Publishes the terminal as the next generation and appends everything
    /// the host then sends, as canonical envelopes.
    ///
    /// - Parameters:
    ///   - out: Receives the frames, appended.
    ///   - frameClass: Why the frame was taken.
    void publish(std::vector<std::uint8_t>& out, Ubiquitty::Protocol::Sync::FrameClass frameClass = Ubiquitty::Protocol::Sync::FrameClass::Settled)
    {
        m_source.publish(frameClass, m_now);
        flush(out);
    }

    /// Appends what the host has queued, as canonical envelopes, without publishing.
    ///
    /// - Parameter out: Receives the frames, appended.
    void flush(std::vector<std::uint8_t>& out)
    {
        const std::uint64_t now = advance();
        m_connection.service(now);
        std::vector<std::uint8_t> envelope;
        for (;;) {
            envelope.clear();
            if (!m_connection.popStreamFrame(envelope, now))
                break;
            out.insert(out.end(), envelope.begin(), envelope.end());
        }
    }

    /// Replaces the pane's records without changing its screen generation.
    ///
    /// - Parameter facts: The current record.
    void setFacts(const Ubiquitty::Protocol::Sync::PaneFacts& facts) { m_source.setFacts(facts); }

    /// Takes the datagrams the host has queued, each one envelope.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> takeDatagrams() { return m_connection.takeDatagrams(); }

    /// Reads one datagram the client sent.
    ///
    /// - Parameter datagram: Exactly one envelope.
    void receiveDatagram(std::span<const std::uint8_t> datagram) { m_connection.receiveDatagram(datagram, m_now); }

    /// Moves the host clock on, for a case that waits out a timer.
    ///
    /// - Parameter microseconds: How far.
    void advanceClock(std::uint64_t microseconds) { m_now += microseconds; }

    /// The host clock.
    [[nodiscard]] std::uint64_t now() const { return m_now; }

    /// The handle the pane is bound to, 0 before the client is welcomed.
    [[nodiscard]] std::uint32_t handle() const { return m_handle; }

    /// The connection, for what a test reads off its counters.
    [[nodiscard]] Ubiquitty::Protocol::Sync::HostConnection& connection() { return m_connection; }

    /// The source that publishes this fixture's pane.
    ///
    /// - Returns: The source owned for the lifetime of this fixture.
    [[nodiscard]] Ubiquitty::Protocol::Sync::PaneSource& source() { return m_source; }

private:
    /// What the host's `Welcome` says, under the full Sync identity given.
    ///
    /// - Parameters:
    ///   - hostInstanceID: The full Sync identity, independent of the carrier.
    ///   - role: The granted role, independent of the client's claim.
    /// - Returns: The identity.
    static Ubiquitty::Protocol::Sync::HostIdentity identity(Ubiquitty::Protocol::Sync::ID128 hostInstanceID, Ubiquitty::Protocol::Sync::Role role,
        std::uint32_t sessionID, std::uint64_t daemonInstanceID)
    {
        Ubiquitty::Protocol::Sync::HostIdentity identity;
        identity.hostInstanceID = hostInstanceID;
        identity.attachmentID = { .low = 1, .high = hostInstanceID.low };
        // A person who may type: the client a frontend's session is.
        identity.mayDrive = role != Ubiquitty::Protocol::Sync::Role::Viewer;
        identity.maxRole = role;
        identity.verifiedKind = role == Ubiquitty::Protocol::Sync::Role::Agent ? Ubiquitty::Protocol::Sync::ClientKind::Agent : Ubiquitty::Protocol::Sync::ClientKind::Human;
        identity.selection = { .target = Ubiquitty::Protocol::Sync::AdmissionTarget::Session,
            .sessionID = sessionID,
            .workspaceID = 0,
            .daemonInstanceID = daemonInstanceID,
            .socketName = "fixture.sock",
            .buildFlags = 0,
            .hostPlatform = "fixture" };
        return identity;
    }

    /// Moves the host clock on by a frame interval, so no rate cap holds a
    /// frame the test publishes.
    ///
    /// - Returns: The host clock after the move.
    std::uint64_t advance()
    {
        m_now += 20'000;
        return m_now;
    }

    TypedInput m_typed;
    Ubiquitty::Protocol::Sync::PaneSource m_source;
    Ubiquitty::Protocol::Sync::HostConnection m_connection;
    std::uint64_t m_now = 1'000'000;
    std::uint32_t m_handle = 0;
};

/// A host terminal and the screen plane that publishes it, for a test that
/// stands in for the daemon a client attached to.
class HostScreen {
public:
    /// Creates a host terminal for a session.
    ///
    /// - Parameters:
    ///   - sessionID: The session the client attached to.
    ///   - daemonInstanceID: The serving incarnation its Welcome names.
    ///   - rows: The terminal's rows.
    ///   - columns: Its columns.
    ///   - datagramMaxBytes: The largest datagram the carrier sends, 0 for a
    ///     carrier without datagrams.
    ///   - role: The authority actually granted by the host.
    ///   - hostInstanceID: The independent full Sync identity, when specified.
    HostScreen(std::uint32_t sessionID, std::uint64_t daemonInstanceID, int rows = 24, int columns = 80, std::size_t datagramMaxBytes = 0, Ubiquitty::Protocol::Sync::Role role = Ubiquitty::Protocol::Sync::Role::Owner, std::optional<Ubiquitty::Protocol::Sync::ID128> hostInstanceID = std::nullopt)
        : m_terminal(makeTerminal(rows, columns))
        , m_plane(*m_terminal, sessionID, daemonInstanceID, datagramMaxBytes, role, hostInstanceID)
    {
    }

    /// The host's terminal, which a test writes to between publishes.
    [[nodiscard]] Ubiquitty::Terminal& terminal() { return *m_terminal; }

    /// Reads the frames the client has written since the last call.
    ///
    /// - Parameter sent: Every frame the client wrote, in order, each whole.
    void receive(const std::vector<std::vector<std::uint8_t>>& sent)
    {
        for (; m_consumed < sent.size(); ++m_consumed)
            m_plane.receive(sent[m_consumed]);
    }

    /// Treats the first `count` frames of what the client wrote as another
    /// connection's, never to be read by this one.
    ///
    /// - Parameter count: The frames to pass over.
    void passOver(std::size_t count) { m_consumed = std::max(m_consumed, count); }

    /// Publishes the terminal and returns what the host sends for it.
    [[nodiscard]] std::vector<std::uint8_t> publish()
    {
        std::vector<std::uint8_t> out;
        m_plane.publish(out);
        return out;
    }

    /// Returns what the host has queued without publishing.
    [[nodiscard]] std::vector<std::uint8_t> flush()
    {
        std::vector<std::uint8_t> out;
        m_plane.flush(out);
        return out;
    }

    /// The host's end of the screen plane.
    [[nodiscard]] ScreenPlaneHost& plane() { return m_plane; }

private:
    /// Makes a reset terminal of a size.
    ///
    /// - Parameters:
    ///   - rows: Its rows.
    ///   - columns: Its columns.
    /// - Returns: The terminal.
    static std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows, int columns)
    {
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::hardReset(*terminal);
        Ubiquitty::resize(*terminal, rows, columns);
        return terminal;
    }

    std::unique_ptr<Ubiquitty::Terminal> m_terminal;
    ScreenPlaneHost m_plane;
    std::size_t m_consumed = 0;
};

} // namespace UbiquittyTest
