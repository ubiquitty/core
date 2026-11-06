// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Host/ScreenClient.h — the client end of a daemon client's Sync
// screen plane, for the Host tests that drive the daemon through a friend seam.
//
// The daemon and client exchange the canonical envelope stream directly.
// A fixture selects its actual admission target before starting this client.
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/RichText/TypographyFacts.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Host/FullCarrier.h"
#include "PAL/UnixFileDescriptor.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace UbiquittyTest {

/// The client end of one daemon client's screen plane.
///
/// Each pane the host binds replicates into a terminal this client owns, one
/// that draws sized text: a mirror of the host keeps the host's OSC 66 runs.
class ScreenClient final : private Ubiquitty::Protocol::Sync::ClientPaneDelegate {
public:
    /// Creates a client that greets a pane of the given geometry and takes
    /// every update the host sends, acknowledging each at once.
    ///
    /// - Parameters:
    ///   - rows: The viewport's rows.
    ///   - columns: The viewport's columns.
    ///   - historyCacheLines: The most history lines it keeps, which its
    ///     `Greet` announces; 0 keeps none.
    ///   - link: What the client's link costs.
    ///   - role: The role requested before the host applies its authority cap.
    ScreenClient(int rows, int columns, std::uint32_t historyCacheLines = Ubiquitty::VT::fallbackScrollbackLines,
        Ubiquitty::Protocol::Sync::LinkCost link = Ubiquitty::Protocol::Sync::LinkCost::Unmetered,
        Ubiquitty::Protocol::Sync::Role role = Ubiquitty::Protocol::Sync::Role::Owner)
        : m_historyCacheLines(historyCacheLines)
        , m_identity(identity(rows, columns, link, role))
    {
    }

    /// Creates a client that is another instance than the rest: a second
    /// person at the pane, whose input is numbered apart from the first's.
    ///
    /// - Parameters:
    ///   - rows: The viewport's rows.
    ///   - columns: The viewport's columns.
    ///   - clientInstance: The instance it greets as.
    ScreenClient(int rows, int columns, Ubiquitty::Protocol::Sync::ID128 clientInstance,
        Ubiquitty::Protocol::Sync::Role role = Ubiquitty::Protocol::Sync::Role::Owner)
        : m_historyCacheLines(0)
        , m_identity(asInstance(identity(rows, columns, Ubiquitty::Protocol::Sync::LinkCost::Unmetered, role), clientInstance))
    {
    }

    /// Creates a client whose panes live in a replica set that outlives it,
    /// and that greets with a resume when one is given: the same client on a
    /// new transport.
    ///
    /// - Parameters:
    ///   - rows: The viewport's rows.
    ///   - columns: The viewport's columns.
    ///   - replicas: The client's replicas; they must outlive this client.
    ///   - resume: The attachment to continue, or `nullopt` for a fresh one.
    ///   - clientInstance: A distinct client's identity, or the fixture default.
    ScreenClient(int rows, int columns, Ubiquitty::Protocol::Sync::ReplicaSet& replicas, std::optional<Ubiquitty::Protocol::Sync::ResumeRequest> resume,
        std::optional<Ubiquitty::Protocol::Sync::ID128> clientInstance = std::nullopt)
        : m_historyCacheLines(0)
        , m_replicas(&replicas)
        , m_identity(withResume(identity(rows, columns, Ubiquitty::Protocol::Sync::LinkCost::Unmetered), std::move(resume)))
    {
        if (clientInstance)
            m_identity.clientInstance = *clientInstance;
    }

    ScreenClient(const ScreenClient&) = delete;
    ScreenClient& operator=(const ScreenClient&) = delete;
    ScreenClient(ScreenClient&&) = delete;
    ScreenClient& operator=(ScreenClient&&) = delete;

    /// Starts once, using the fixture's explicit target and any credential.
    /// No received pane or Welcome is used to invent the request.
    /// - Parameters:
    ///   - request: The exact target chosen by fixture setup, including its pin.
    ///   - credential: The optional real test token, never a claimed authority.
    /// - Returns: Whether the production client encoded its one valid Greet.
    bool start(Ubiquitty::Protocol::Sync::AdmissionRequest request,
        std::optional<std::vector<std::uint8_t>> credential = std::nullopt)
    {
        if (m_connection)
            return false;
        m_identity.request = std::move(request);
        m_identity.credential = std::move(credential);
        auto chosen = policy(m_historyCacheLines);
        chosen.capabilities = Ubiquitty::Protocol::Sync::ClientCapabilities::fromRaw(chosen.capabilities.toRaw() | m_extraCapabilities.toRaw());
        m_connection = std::make_unique<Ubiquitty::Protocol::Sync::ClientConnection>(chosen, m_identity,
            static_cast<Ubiquitty::Protocol::Sync::ClientPaneDelegate*>(this), nullptr, m_replicas);
        m_connection->start();
        return m_connection->state() != Ubiquitty::Protocol::Sync::ConnectionState::Closed;
    }

    /// Names ``ClientCapability/ShowsNoQuestions`` in the Greet, as the
    /// editor clients do: the person is attached and shown no question.
    /// Called before ``start``.
    void showsNoQuestions() { m_extraCapabilities.add(Ubiquitty::Protocol::Sync::ClientCapability::ShowsNoQuestions); }

    /// Offers the face publication a renderer of proportional runs owes.
    /// Called before ``start`` by fixtures whose client draws those runs.
    void publishesFaces() { m_extraCapabilities.add(Ubiquitty::Protocol::Sync::ClientCapability::PublishesFaces); }

    /// The daemon's end of a transport for this client, for a fixture that
    /// attached it with none. A real client always has one, and the daemon
    /// drops a client whose write fails, which a missing descriptor does at
    /// the first flush outside a service pass: the greet-time seed among
    /// them. The carrier's reader never reads, so what the daemon writes
    /// stays in the client's output for `ScreenPlaneAccess::deliver`.
    /// - Returns: The descriptor, or an empty one when it was already taken
    ///   or the carrier could not be made.
    [[nodiscard]] Ubiquitty::PAL::UnixFileDescriptor takeCarrier()
    {
        if (!m_carrier)
            m_carrier.emplace();
        return m_carrier->takeDaemonEnd();
    }

    /// Whether an explicit target has been selected and its client constructed.
    /// - Returns: Whether start has constructed the physical client owner.
    [[nodiscard]] bool hasStarted() const { return m_connection != nullptr; }

    /// The authenticated Welcome, absent before admission or on a silent dial.
    /// - Returns: A copy of the production reader's Welcome, or nothing.
    [[nodiscard]] std::optional<Ubiquitty::Protocol::Sync::Welcome> welcome() const { return m_connection ? m_connection->welcome() : std::nullopt; }

    /// Takes the host's `Notify`s received since the last call.
    [[nodiscard]] std::vector<Ubiquitty::Protocol::Sync::Notify> takeNotifies() { return m_connection ? m_connection->takeNotifies() : std::vector<Ubiquitty::Protocol::Sync::Notify> {}; }

    /// Tells the host the client is about to be suspended and closes, as a
    /// client does at its background deadline.
    void suspend()
    {
        if (m_connection)
            m_connection->leave(Ubiquitty::Protocol::Sync::ByeReason::Suspending);
    }

    /// Tells the host the client is leaving on purpose and closes, as a client
    /// does when its window closes, so the host frees its attachment rather
    /// than holding it `Lost` for a resume.
    void detach()
    {
        if (m_connection)
            m_connection->leave(Ubiquitty::Protocol::Sync::ByeReason::Normal);
    }

    /// Writes the panes' replicas back to the set, as the client does before
    /// its transport goes.
    void storeReplicas()
    {
        if (m_connection)
            m_connection->storeReplicas();
    }

    /// Reads the bytes the daemon queued for this client, in order.
    ///
    /// - Parameters:
    ///   - bytes: Whole or partial canonical envelopes.
    ///   - now: The client's clock in microseconds.
    void read(std::span<const std::uint8_t> bytes, std::uint64_t now)
    {
        if (m_connection) {
            countHistory(bytes);
            m_connection->receiveStream(bytes, now);
        }
    }

    /// Reads one canonical datagram without touching stream reassembly.
    ///
    /// - Parameters:
    ///   - bytes: One whole canonical envelope.
    ///   - now: The client's clock in microseconds.
    void readDatagram(std::span<const std::uint8_t> bytes, std::uint64_t now)
    {
        if (m_connection)
            m_connection->receiveDatagram(bytes, now);
    }

    /// Takes the client's pending datagrams, each one canonical envelope.
    ///
    /// - Returns: The pending envelopes, in order.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> takeDatagrams()
    {
        return m_connection ? m_connection->takeDatagrams() : std::vector<std::vector<std::uint8_t>> {};
    }

    /// Takes the client's direct canonical stream output.
    ///
    /// - Parameter now: The client's clock in microseconds.
    /// - Returns: Canonical bytes to hand the daemon.
    [[nodiscard]] std::vector<std::uint8_t> takeUpstream(std::uint64_t now)
    {
        return takeStream(now);
    }

    /// The client's raw Sync stream output, unwrapped, for a test that
    /// feeds the daemon's screen connection directly.
    ///
    /// - Parameter now: The client's clock in microseconds.
    /// - Returns: The bytes.
    [[nodiscard]] std::vector<std::uint8_t> takeStream(std::uint64_t now)
    {
        if (!m_connection)
            return {};
        m_connection->tick(now);
        return m_connection->takeStreamOutput();
    }

    /// Sends a `Call`; its `Result` arrives through `takeResults`.
    ///
    /// - Parameter request: A JSON-RPC 2.0 request with an id.
    /// - Returns: Whether it was sent.
    [[nodiscard]] bool call(std::string_view request) { return m_connection && m_connection->call(request); }

    /// Sends keys to the first pane, encoded by the host when it writes them.
    ///
    /// - Parameter events: The keys.
    /// - Returns: Their number, or `nullopt` when nothing was sent.
    [[nodiscard]] std::optional<std::uint64_t> sendKeys(std::span<const Ubiquitty::Protocol::Sync::KeyEvent> events) { return m_connection ? m_connection->sendKeys(1, events) : std::nullopt; }

    /// Pastes text into the first pane, its pieces leaving one per
    /// `takeStream`.
    ///
    /// - Parameter text: The text.
    /// - Returns: The paste's number, or `nullopt` when nothing was sent.
    [[nodiscard]] std::optional<std::uint64_t> sendPaste(std::string_view text) { return m_connection ? m_connection->sendPaste(1, text) : std::nullopt; }

    /// Sends bytes to the first pane, written verbatim.
    ///
    /// - Parameter bytes: The bytes.
    /// - Returns: Their number, or `nullopt` when nothing was sent.
    [[nodiscard]] std::optional<std::uint64_t> sendRaw(std::span<const std::uint8_t> bytes) { return m_connection ? m_connection->sendRaw(1, bytes) : std::nullopt; }

    /// Sends bytes to the pane selected by its actual bound handle.
    ///
    /// - Parameters:
    ///   - handle: The handle reported by the admitted client's pane binding.
    ///   - bytes: The bytes to write verbatim.
    /// - Returns: The production input sequence, or nothing if not sent.
    [[nodiscard]] std::optional<std::uint64_t> sendRaw(std::uint32_t handle, std::span<const std::uint8_t> bytes) { return m_connection ? m_connection->sendRaw(handle, bytes) : std::nullopt; }

    /// Sends bytes under a number the test chooses, as the production client
    /// numbers what it types after a resume: past what the host reported
    /// written, which a new connection's own count does not know.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - inputSeq: The number.
    ///   - bytes: The bytes.
    /// - Returns: Whether the connection took it.
    [[nodiscard]] bool sendRawNumbered(std::uint32_t handle, std::uint64_t inputSeq, std::span<const std::uint8_t> bytes)
    {
        return m_connection && m_connection->sendNumbered(handle, inputSeq, Ubiquitty::Protocol::Sync::RawInput { .bytes = { bytes.begin(), bytes.end() } });
    }

    /// Sends pointer events to the first pane, encoded by the host when it
    /// writes them.
    ///
    /// - Parameter events: The events.
    /// - Returns: Their number, or `nullopt` when nothing was sent.
    [[nodiscard]] std::optional<std::uint64_t> sendPointer(std::span<const Ubiquitty::Protocol::Sync::PointerEvent> events) { return m_connection ? m_connection->sendPointer(1, events) : std::nullopt; }

    /// Tells the host whether this client's window holds the focus.
    ///
    /// - Parameter focus: The focus state.
    /// - Returns: Whether the report was queued.
    bool sendFocus(Ubiquitty::Modes::FocusState focus) { return m_connection && m_connection->sendFocus(1, focus); }

    /// Tells the host the view the client holds of its pane.
    ///
    /// - Parameter state: The view.
    void setView(Ubiquitty::Protocol::Sync::ViewState state)
    {
        if (m_connection)
            m_connection->setView(1, state);
    }

    /// Takes the `Result` texts the client has read since the last call.
    [[nodiscard]] std::vector<std::string> takeResults() { return m_connection ? m_connection->takeResults() : std::vector<std::string> {}; }

    /// Takes command lines from validated typed command-completion records.
    ///
    /// - Returns: Owned strings delivered once by the connection's effect reader.
    [[nodiscard]] std::vector<std::string> takeCompletedCommands() { return std::exchange(m_completedCommands, {}); }

    /// Sets whether the client is focused, which its next stream output
    /// tells the host.
    ///
    /// - Parameter focus: The focus.
    void setFocus(Ubiquitty::Modes::FocusState focus)
    {
        m_identity.facts.isFocused = focus == Ubiquitty::Modes::FocusState::Focused;
        if (m_connection)
            m_connection->setFocus(focus);
    }

    /// Tells the host whether the client's view of its pane holds the
    /// keyboard, which its next stream output carries.
    ///
    /// - Parameter focus: The focus.
    /// - Returns: Whether it was sent; not before the pane is bound.
    [[nodiscard]] bool setPaneFocus(Ubiquitty::Modes::FocusState focus) { return m_connection && m_connection->sendFocus(1, focus); }

    /// Tells the host the size this client shows its pane at.
    ///
    /// - Parameter viewport: The grid, and the pixels it is drawn in.
    /// - Returns: Whether it was sent.
    [[nodiscard]] bool sendViewport(const Ubiquitty::Protocol::Sync::Viewport& viewport) { return m_connection && m_connection->sendViewport(1, viewport); }

    /// Tells the host the size this client shows its pane at, with no pixels
    /// measured.
    ///
    /// - Parameters:
    ///   - columns: The grid's columns.
    ///   - rows: Its rows.
    /// - Returns: Whether it was sent.
    [[nodiscard]] bool sendViewport(int columns, int rows)
    {
        return sendViewport({ .rows = static_cast<std::uint16_t>(rows), .columns = static_cast<std::uint16_t>(columns), .cellPixelWidth = 0, .cellPixelHeight = 0, .textAreaPixelWidth = 0, .textAreaPixelHeight = 0, .drawablePixelWidth = 0, .drawablePixelHeight = 0 });
    }

    /// Publishes the client's typography table, as a frontend does when it
    /// cuts a renderer; the connection sends it once attached.
    /// - Parameters:
    ///   - facts: The table.
    ///   - handle: The pane's binding, initially one.
    void publishTypography(const Ubiquitty::RichText::TypographyFacts& facts, std::uint32_t handle = 1)
    {
        if (m_connection)
            m_connection->publishTypography(handle, facts);
    }

    /// Publishes the faces the client draws proportional runs in, as a
    /// frontend does when it cuts a renderer; the connection sends them once
    /// attached.
    /// - Parameter faces: The faces.
    void publishProportionalFaces(const Ubiquitty::OSC::TextSizing::ProportionalFaceFacts& faces)
    {
        if (m_connection)
            m_connection->publishProportionalFaces(faces);
    }

    /// Sets the theme the client draws in, which its `Facts` carries to a
    /// host that reads it.
    ///
    /// - Parameter palette: The theme, or `nullopt`.
    void setThemePalette(std::optional<Ubiquitty::Protocol::Sync::ThemePalette> palette)
    {
        m_identity.facts.themePalette = palette;
        if (m_connection)
            m_connection->setThemePalette(std::move(palette));
    }

    /// The replica of the first pane the host bound, or null.
    [[nodiscard]] const Ubiquitty::Terminal* pane() const { return m_connection ? m_connection->paneTerminal(1) : nullptr; }

    /// The replica of the first pane the host bound, for a test standing in
    /// for the frontend that measures in it; null before the bind.
    [[nodiscard]] Ubiquitty::Terminal* replica() { return m_connection ? m_connection->paneTerminal(1) : nullptr; }

    /// The generation the first pane holds, or 0.
    [[nodiscard]] std::uint64_t generation() const { return m_connection ? m_connection->paneGeneration(1) : 0; }

    /// The client end of the connection after an explicit start.
    /// - Returns: The production client, borrowed for this owner's lifetime.
    [[nodiscard]] const Ubiquitty::Protocol::Sync::ClientConnection& connection() const
    {
        assert(m_connection);
        return *m_connection;
    }

    /// Every canonical kind decoded by the actual client, in stream order.
    [[nodiscard]] const std::vector<Ubiquitty::Protocol::Sync::FrameKind>& tags() const { return m_kinds; }

    /// The bytes of the history plane read: `HistoryInline` and `HistoryHead`
    /// frames, and the chunks of the transfers the heads announced, each
    /// counted whole with its envelope.
    [[nodiscard]] std::size_t historyBytes() const { return m_historyBytes; }

    /// Asks the host for history lines of the first pane.
    ///
    /// - Parameters:
    ///   - priority: The want's priority.
    ///   - ranges: The lines.
    /// - Returns: False when the client sent nothing.
    [[nodiscard]] bool wantHistory(Ubiquitty::Protocol::Sync::HistoryPriority priority, std::span<const Ubiquitty::Protocol::Sync::LineRange> ranges)
    {
        return m_connection && m_connection->wantHistory(1, priority, ranges);
    }

    /// How many Sync frames of a kind were read.
    ///
    /// - Parameter kind: The kind.
    /// - Returns: The count.
    [[nodiscard]] std::size_t count(Ubiquitty::Protocol::Sync::FrameKind kind) const
    {
        std::size_t total = 0;
        for (const auto seen : m_kinds)
            total += seen == kind ? 1 : 0;
        return total;
    }

    /// Calls `observer` after each frame the connection applies, while what
    /// that frame set is still held: one read can carry a pane's last facts
    /// and the unbind that discards them.
    ///
    /// - Parameter observer: What to call, or an empty function to stop.
    void observeAppliedFrames(std::function<void(const Ubiquitty::Protocol::Sync::ClientConnection&)> observer)
    {
        m_appliedFrameObserver = std::move(observer);
    }

    /// The pane the host last bound to a handle on this client.
    ///
    /// - Parameter handle: The handle.
    /// - Returns: The pane, or `nullopt` when the host bound none to it.
    [[nodiscard]] std::optional<Ubiquitty::Protocol::Sync::ID128> boundPane(std::uint32_t handle) const
    {
        const auto found = std::ranges::find(m_boundPanes, handle, &std::pair<std::uint32_t, Ubiquitty::Protocol::Sync::ID128>::first);
        return found == m_boundPanes.end() ? std::nullopt : std::optional { found->second };
    }

private:
    /// Records the pane a handle names, for a test that resumes it.
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    void paneBound(std::uint32_t handle, Ubiquitty::Protocol::Sync::ID128 paneID) override
    {
        std::erase_if(m_boundPanes, [&](const auto& bound) { return bound.first == handle; });
        m_boundPanes.emplace_back(handle, paneID);
    }

    /// Observes the same complete frames the actual client dispatches.
    /// - Parameter kind: The actual decoded canonical frame kind.
    void connectionFrameDecoded(Ubiquitty::Protocol::Sync::FrameKind kind, std::size_t) override
    {
        m_kinds.push_back(kind);
    }

    /// Hands each applied frame's connection to the test's observer.
    /// - Parameter connection: The connection that applied it.
    void connectionFrameApplied(const Ubiquitty::Protocol::Sync::ClientConnection& connection) override
    {
        if (m_appliedFrameObserver)
            m_appliedFrameObserver(connection);
    }

    /// Copies command context while its validated effect body remains alive.
    ///
    /// - Parameter body: The typed effect whose borrowed fields last for this call.
    void paneEffect(std::uint32_t, const Ubiquitty::Protocol::Sync::EffectView&,
        const Ubiquitty::Protocol::Sync::EffectBody& body, Ubiquitty::Protocol::Sync::EffectDisposition) override
    {
        if (const auto* command = std::get_if<Ubiquitty::Protocol::Sync::CommandFinishedEffect>(&body))
            m_completedCommands.emplace_back(command->commandLine);
    }

    /// Counts the history plane's bytes in one run of Sync frames.
    ///
    /// - Parameter carried: The run.
    void countHistory(std::span<const std::uint8_t> carried)
    {
        using namespace Ubiquitty::Protocol::Sync;
        (void)m_historyIngress.consume(carried, [](std::span<const std::uint8_t> prefix) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            const auto state = readFrameHeader(prefix, header, error);
            if (state == FrameHeaderState::NeedsMore)
                return FrameHeaderDecision {};
            return FrameHeaderDecision { .state = state == FrameHeaderState::Complete ? FrameHeaderDecision::State::Admitted : FrameHeaderDecision::State::Refused,
                .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            if (readFrameHeader(prefix, header, error) != FrameHeaderState::Complete)
                return FrameDelivery::Stop;
            const auto bytes = prefix.size() + body.size();
            if (header.kind == FrameKind::HistoryInline) {
                m_historyBytes += bytes;
            } else if (header.kind == FrameKind::HistoryHead) {
                m_historyBytes += bytes;
                if (const auto head = decodeHistoryHead(body))
                    m_historyTransfers.push_back(head->transfer);
            } else if (header.kind == FrameKind::BulkChunk) {
                const auto chunk = decodeBulkChunk(body);
                if (chunk && std::ranges::find(m_historyTransfers, chunk->transfer) != m_historyTransfers.end()) {
                    m_historyBytes += bytes;
                    if (chunk->isLast)
                        std::erase(m_historyTransfers, chunk->transfer);
                }
            } else if (header.kind == FrameKind::BulkAbort) {
                if (const auto abort = decodeBulkAbort(body))
                    std::erase(m_historyTransfers, abort->transfer);
            }
            return FrameDelivery::Continue; });
    }

    /// Makes the sizing terminal a newly bound pane replicates into.
    ///
    /// - Returns: The terminal, owned by this client.
    Ubiquitty::Terminal* terminalForPane(Ubiquitty::Protocol::Sync::ID128) override
    {
        // A client over a replica set draws into the set's terminals.
        if (m_replicas)
            return nullptr;
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        terminal->drawsSizedText = true;
        terminal->drawsProportionalText = true;
        if (m_historyCacheLines > 0)
            Ubiquitty::setScrollbackLimit(terminal.get(), static_cast<int>(m_historyCacheLines));
        m_terminals.push_back(std::move(terminal));
        return m_terminals.back().get();
    }

    /// The client's constants.
    ///
    /// - Parameter historyCacheLines: The most history lines the replica keeps.
    /// - Returns: The policy.
    static Ubiquitty::Protocol::Sync::ClientPolicy policy(std::uint32_t historyCacheLines)
    {
        // A test client takes whatever the host sends and acknowledges at once:
        // what is under test is the host.
        return { .maxCheckpointBytes = std::size_t { 256 } << 20, .maxBufferedUpdateBytes = std::size_t { 64 } << 20, .minAckIntervalMicroseconds = 0, .historyCacheLines = historyCacheLines };
    }

    /// What the client tells the host: a focused human viewport.
    ///
    /// - Parameters:
    ///   - rows: The viewport's rows.
    ///   - columns: The viewport's columns.
    ///   - link: What the client's link costs.
    /// - Returns: The identity.
    static Ubiquitty::Protocol::Sync::ClientIdentity identity(int rows, int columns, Ubiquitty::Protocol::Sync::LinkCost link,
        Ubiquitty::Protocol::Sync::Role role = Ubiquitty::Protocol::Sync::Role::Owner)
    {
        Ubiquitty::Protocol::Sync::ClientIdentity identity;
        identity.clientInstance = { .low = 1, .high = 2 };
        identity.requestedRole = role;
        identity.facts.viewportRows = static_cast<std::uint16_t>(rows);
        identity.facts.viewportColumns = static_cast<std::uint16_t>(columns);
        identity.facts.isFocused = true;
        identity.facts.link = link;
        return identity;
    }

    /// An identity under another client instance.
    ///
    /// - Parameters:
    ///   - identity: The identity.
    ///   - clientInstance: The instance.
    /// - Returns: The identity as that instance.
    static Ubiquitty::Protocol::Sync::ClientIdentity asInstance(Ubiquitty::Protocol::Sync::ClientIdentity identity, Ubiquitty::Protocol::Sync::ID128 clientInstance)
    {
        identity.clientInstance = clientInstance;
        return identity;
    }

    /// An identity that greets with a resume.
    ///
    /// - Parameters:
    ///   - identity: The identity.
    ///   - resume: The attachment to continue, or `nullopt`.
    /// - Returns: The identity with the resume.
    static Ubiquitty::Protocol::Sync::ClientIdentity withResume(Ubiquitty::Protocol::Sync::ClientIdentity identity, std::optional<Ubiquitty::Protocol::Sync::ResumeRequest> resume)
    {
        identity.resume = std::move(resume);
        return identity;
    }

    std::uint32_t m_historyCacheLines = 0;
    /// What the Greet names beyond the policy's own capabilities.
    Ubiquitty::Protocol::Sync::ClientCapabilities m_extraCapabilities;
    Ubiquitty::Protocol::Sync::ReplicaSet* m_replicas = nullptr;
    Ubiquitty::Protocol::Sync::ClientIdentity m_identity;
    std::optional<Ubiquitty::HostTest::FullCarrier> m_carrier;
    std::function<void(const Ubiquitty::Protocol::Sync::ClientConnection&)> m_appliedFrameObserver;
    // Declared before the connection, which holds pointers into it.
    std::vector<std::unique_ptr<Ubiquitty::Terminal>> m_terminals;
    std::vector<Ubiquitty::Protocol::Sync::FrameKind> m_kinds;
    std::vector<std::pair<std::uint32_t, Ubiquitty::Protocol::Sync::ID128>> m_boundPanes;
    std::vector<std::string> m_completedCommands;
    std::size_t m_historyBytes = 0;
    std::vector<std::uint32_t> m_historyTransfers;
    Ubiquitty::Protocol::Sync::FrameIngress m_historyIngress { Ubiquitty::Protocol::Sync::maxFrameBodyBytes };
    // Destroy first: all borrowed terminals and callback storage remain live.
    std::unique_ptr<Ubiquitty::Protocol::Sync::ClientConnection> m_connection;
};

/// Reads the client's clock.
///
/// - Returns: Microseconds since an arbitrary epoch, never 0.
inline std::uint64_t screenClientNow()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) + 1;
}

/// Carries a client's screen plane over a socket to a live daemon for a span
/// of time: what the socket holds is read into the client, and what the
/// client sends is written back.
///
/// - Parameters:
///   - fileDescriptor: The client's end of its connection to the daemon.
///   - client: The client.
///   - milliseconds: How long to carry.
///   - isDone: Asked after every read; carrying stops early once it answers true.
/// - Returns: False when the daemon closed the connection.
inline bool carryScreen(int fileDescriptor, ScreenClient& client, int milliseconds, const std::function<bool()>& isDone = {})
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::vector<std::uint8_t> up = client.takeUpstream(screenClientNow());
        for (std::size_t written = 0; written < up.size();) {
            const ssize_t n = ::write(fileDescriptor, up.data() + written, up.size() - written);
            if (n <= 0)
                return false;
            written += static_cast<std::size_t>(n);
        }
        pollfd p { fileDescriptor, POLLIN, 0 };
        if (::poll(&p, 1, 10) <= 0)
            continue;
        std::uint8_t buffer[65536];
        const ssize_t n = ::read(fileDescriptor, buffer, sizeof(buffer));
        if (n <= 0)
            return false;
        client.read({ buffer, static_cast<std::size_t>(n) }, screenClientNow());
        if (isDone && isDone())
            return true;
    }
    return true;
}

/// Types at a client's pane over its socket to a live daemon: `Raw` input
/// events, then everything the client has to send. The pane must be bound,
/// which it is once the client holds a checkpoint.
///
/// - Parameters:
///   - fileDescriptor: The client's end of its connection to the daemon.
///   - client: The client.
///   - text: What to type.
/// - Returns: Whether the client took every event and the socket every byte.
inline bool typeOverSocket(int fileDescriptor, ScreenClient& client, std::string_view text)
{
    constexpr std::size_t maxRawEventBytes = 16 * 1024;
    for (; !text.empty(); text.remove_prefix(std::min(text.size(), maxRawEventBytes))) {
        const std::string_view piece = text.substr(0, maxRawEventBytes);
        if (!client.sendRaw({ reinterpret_cast<const std::uint8_t*>(piece.data()), piece.size() }).has_value())
            return false;
    }
    const std::vector<std::uint8_t> up = client.takeUpstream(screenClientNow());
    for (std::size_t written = 0; written < up.size();) {
        const ssize_t n = ::write(fileDescriptor, up.data() + written, up.size() - written);
        if (n <= 0)
            return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace UbiquittyTest
