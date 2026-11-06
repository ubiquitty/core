// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SyncLink.h — a host and a client connection over a simulated link, on a
// fake clock, with any number of panes bound.
//
// The fixture owns each pane's host terminal and source, both ends of one
// connection and the link between them; a test drives the terminals with
// escape sequences, publishes, and moves time. Two links are modelled:
//
// - **Instant.** `step` advances one frame interval, services the host and
//   `pump`s: the host's stream is drained whole into the client, datagrams are
//   delivered or dropped, the client ticks, and its frames go upstream.
// - **Shaped.** After `shape`, the link is a relay with a rate and a delay, as
//   an SSH channel is: it takes every frame the host's pacing lets go and
//   queues it, and each arrives once the frames ahead of it and its own bytes
//   have crossed at the rate, plus the delay. What the window lets queue in
//   the relay is therefore what an interactive frame waits behind. The
//   client's frames are charged the delay alone. `advance` runs every event
//   up to a time, in order.
//
// Either link can withhold the client's frames (all of them, or one kind) and
// suspend the client's reading, so a test can model a client that stopped
// acknowledging or stopped reading.
//

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace UbiquittyTest {

/// The provisional constants Docs/sync.md §6.4 and §6.5 name, as test inputs.
inline constexpr Ubiquitty::Protocol::Sync::PacingPolicy syncTestPacing {
    .windowFloorBytes = 16 * 1024,
    .probeIdleMicroseconds = 2'000'000,
    .roundTripStaleMicroseconds = 10'000'000,
    .rateFilterMicroseconds = 10'000'000,
    .datagramLossFloorMicroseconds = 20'000,
};

/// `quicDatagramMaxBytes`, the ceiling Source/Platform/Host/HostQUIC.h sends under.
inline constexpr std::size_t syncMaxDatagramBytes = 1100;

/// One refresh interval at 60 Hz, the step of the instant link.
inline constexpr std::uint64_t syncFrameInterval = 16'667;

/// The host policy the fixture runs on.
///
/// - Parameter datagramMaxBytes: 0 for a link without datagrams.
/// - Returns: The policy.
inline Ubiquitty::Protocol::Sync::HostPolicy syncHostPolicy(std::size_t datagramMaxBytes)
{
    return { .pacing = syncTestPacing, .lanes = { .bulkSharePermille = 250 }, .datagramMaxBytes = datagramMaxBytes, .maxCheckpointBytes = 8 << 20 };
}

/// The client policy the fixture runs on. It accepts no compression, so a
/// checkpoint's chunks are the body's own; a case about deflate says so.
inline constexpr Ubiquitty::Protocol::Sync::ClientPolicy syncClientPolicy { .maxCheckpointBytes = 8 << 20, .maxBufferedUpdateBytes = 1 << 20, .minAckIntervalMicroseconds = 4'000, .capabilities = { Ubiquitty::Protocol::Sync::ClientCapability::SpanOps, Ubiquitty::Protocol::Sync::ClientCapability::ScrollRectOps, Ubiquitty::Protocol::Sync::ClientCapability::EchoContext } };

/// The serving selection a test daemon answers with: one session on one
/// daemon instance, over a socket name the grammar accepts. A Welcome without
/// a valid selection does not encode, so every test host states this one
/// unless the selection is what it tests.
///
/// - Returns: The selection.
inline Ubiquitty::Protocol::Sync::ServingSelection syncDefaultSelection()
{
    return { .target = Ubiquitty::Protocol::Sync::AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "test.sock", .buildFlags = 0, .hostPlatform = "test" };
}

/// The facts a focused, unmetered 60 Hz client reports.
///
/// - Parameters:
///   - rows: The viewport's rows.
///   - columns: The viewport's columns.
/// - Returns: The facts.
inline Ubiquitty::Protocol::Sync::Facts syncDefaultFacts(int rows = 24, int columns = 80)
{
    return {
        .viewportRows = static_cast<std::uint16_t>(rows),
        .viewportColumns = static_cast<std::uint16_t>(columns),
        .cellPixelWidth = 9,
        .cellPixelHeight = 18,
        .textAreaPixelWidth = 0,
        .textAreaPixelHeight = 0,
        .refreshIntervalMicroseconds = static_cast<std::uint32_t>(syncFrameInterval),
        .link = Ubiquitty::Protocol::Sync::LinkCost::Unmetered,
        .power = Ubiquitty::Protocol::Sync::PowerState::Normal,
        .isFocused = true,
    };
}

/// A screen whose checkpoint takes several chunks: every cell of every row a
/// colour of its own, so each row record is long.
///
/// - Parameters:
///   - rows: The rows to fill.
///   - columns: The columns to fill.
/// - Returns: What a program writes to draw it.
inline std::string colourfulScreen(int rows, int columns)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row) {
        bytes += "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int column = 0; column < columns; ++column)
            bytes += "\x1b[38;5;" + std::to_string(16 + (row * 7 + column) % 216) + "m" + static_cast<char>('a' + (row + column) % 26);
    }
    return bytes + "\x1b[0m";
}

/// A pane's program as input sees it: every write in order, with its time,
/// and a way to refuse the next few as a program not reading would.
class RecordingWriter final : public Ubiquitty::Protocol::Sync::InputWriter {
public:
    [[nodiscard]] Ubiquitty::Protocol::Sync::WriteStatus write(Ubiquitty::Protocol::Sync::ID128 clientInstance, const Ubiquitty::Protocol::Sync::InputIntent& intent) override
    {
        if (m_isClosed)
            return Ubiquitty::Protocol::Sync::WriteStatus::Closed;
        if (m_refusals > 0) {
            --m_refusals;
            return Ubiquitty::Protocol::Sync::WriteStatus::Refused;
        }
        // Raw input is recorded as its bytes; an intent the host encodes is
        // recorded as a write with none, for a test that counts writes.
        std::string bytes;
        if (const auto* raw = std::get_if<Ubiquitty::Protocol::Sync::RawInput>(&intent))
            bytes.assign(raw->bytes.begin(), raw->bytes.end());
        m_bytes += bytes;
        m_writes.push_back({ .clientInstance = clientInstance, .bytes = std::move(bytes) });
        return Ubiquitty::Protocol::Sync::WriteStatus::Written;
    }

    /// Refuses the next writes, as a program not reading its input would.
    ///
    /// - Parameter count: How many; 0 takes every write again.
    void refuseNext(int count) { m_refusals = count; }

    /// Makes every later write find the program gone.
    void close() { m_isClosed = true; }

    /// Every byte written, in order.
    [[nodiscard]] const std::string& bytes() const { return m_bytes; }

    /// One write the program took.
    struct Write {
        Ubiquitty::Protocol::Sync::ID128 clientInstance;
        std::string bytes;
    };

    /// Every write, in order.
    [[nodiscard]] const std::vector<Write>& writes() const { return m_writes; }

private:
    std::string m_bytes;
    std::vector<Write> m_writes;
    int m_refusals = 0;
    bool m_isClosed = false;
};

/// Native gesture support advertised by the test client.
enum class NativeDragSupport {
    Unavailable,
    Available
};

/// Whether a test connection's authority carries the Drive scope.
enum class DriveScope : std::uint8_t {
    Granted,
    Withheld,
};

/// A pane a test owns outside any link, its program's input recorded, so
/// several connections can attach to it in turn or at once.
struct OwnedPane {
    Ubiquitty::Terminal terminal;
    RecordingWriter writer;
    std::unique_ptr<Ubiquitty::Protocol::Sync::PaneSource> source;

    /// Draws a prompt on an 80x24 pane and publishes it, its input going to
    /// `writer`.
    OwnedPane()
    {
        Ubiquitty::resize(terminal, 24, 80);
        source = std::make_unique<Ubiquitty::Protocol::Sync::PaneSource>(terminal, Ubiquitty::Protocol::Sync::ID128 { 9, 10 });
        UbiquittyTest::feed(terminal, "$ ");
        // The prompt is a change, as a daemon notes every read before it
        // publishes; a source whose changes trail its generation is no pane
        // a handover can carry.
        source->noteTerminalChanged();
        source->publish(Ubiquitty::Protocol::Sync::FrameClass::Structural, 1);
        source->setInputWriter(writer);
    }
};

/// One connection to a pane another object owns: greeted, the pane bound and
/// its checkpoint applied, on an instant link the test pumps.
struct PaneConnection {
    Ubiquitty::Protocol::Sync::HostConnection host;
    Ubiquitty::Protocol::Sync::ClientConnection client;
    std::uint32_t handle = 0;

    /// Attaches a client to the pane.
    ///
    /// - Parameters:
    ///   - source: The pane.
    ///   - clientInstance: The client; the attachment is named after it, so
    ///     two connections of different instances are two attachments.
    ///   - kind: What kind of client it declares itself.
    ///   - drive: Whether its authority may drive.
    ///   - verified: The most the host verified it to be; a person unless a
    ///     test says otherwise.
    PaneConnection(Ubiquitty::Protocol::Sync::PaneSource& source, Ubiquitty::Protocol::Sync::ID128 clientInstance, Ubiquitty::Protocol::Sync::ClientKind kind, DriveScope drive,
        Ubiquitty::Protocol::Sync::ClientKind verified = Ubiquitty::Protocol::Sync::ClientKind::Human, NativeDragSupport supportsNativeDrag = NativeDragSupport::Unavailable)
        : host(syncHostPolicy(0), Ubiquitty::Protocol::Sync::HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { clientInstance.low, clientInstance.high + 100 }, .resumeSecret = { 5, 6 }, .principal = "test", .mayDrive = drive == DriveScope::Granted, .verifiedKind = verified, .selection = syncDefaultSelection() })
        , client([&] {
            auto policy = syncClientPolicy;
            policy.capabilities.set(Ubiquitty::Protocol::Sync::ClientCapability::NativeDrag, supportsNativeDrag == NativeDragSupport::Available);
            return policy;
        }(),
              Ubiquitty::Protocol::Sync::ClientIdentity { .clientInstance = clientInstance, .kind = kind, .facts = syncDefaultFacts() })
    {
        client.start();
        pump(1);
        pump(1);
        handle = host.bindPane(source);
        for (std::uint64_t now = 2; now < 200 && !client.paneTerminal(handle); ++now)
            pump(now);
        client.setView(handle, Ubiquitty::Protocol::Sync::ViewState::Live);
        pump(200);
    }

    /// Runs the host, then moves everything each end has to send once.
    ///
    /// - Parameter now: The time.
    void pump(std::uint64_t now)
    {
        host.service(now);
        std::vector<std::uint8_t> stream;
        while (host.popStreamFrame(stream, now)) {
        }
        client.receiveStream(stream, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    }
};

/// Panes, both ends of one connection, and the link between them.
class SyncLink {
public:
    using Carrier = Ubiquitty::Protocol::Sync::Carrier;
    using FrameKind = Ubiquitty::Protocol::Sync::FrameKind;

    /// Greets: both ends are attached and no pane is bound.
    ///
    /// - Parameters:
    ///   - datagramMaxBytes: 0 for a link without datagrams.
    ///   - facts: What the client reports.
    ///   - policy: The host's constants; the fixture's own when absent.
    ///   - kind: What kind of client attaches.
    ///   - linkShape: A shaped link from the start, so the handshake and the
    ///     client's first probe cross it; the instant link when absent.
    ///   - clientPolicy: The client's constants; the fixture's own when absent.
    explicit SyncLink(std::size_t datagramMaxBytes, Ubiquitty::Protocol::Sync::Facts facts = syncDefaultFacts(),
        std::optional<Ubiquitty::Protocol::Sync::HostPolicy> policy = std::nullopt, Ubiquitty::Protocol::Sync::ClientKind kind = Ubiquitty::Protocol::Sync::ClientKind::Human,
        std::optional<LinkShape> linkShape = std::nullopt, std::optional<Ubiquitty::Protocol::Sync::ClientPolicy> clientPolicy = std::nullopt)
        : m_host(policy.value_or(syncHostPolicy(datagramMaxBytes)), Ubiquitty::Protocol::Sync::HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .principal = "test", .mayDrive = true, .verifiedKind = kind, .selection = syncDefaultSelection() })
        , m_client(clientPolicy.value_or(syncClientPolicy), Ubiquitty::Protocol::Sync::ClientIdentity { .clientInstance = { 7, 8 }, .kind = kind, .facts = facts }, &m_recorder, &m_purges)
    {
        m_client.start();
        if (linkShape) {
            // The Greet crosses, the Welcome comes back, and the client's
            // probe at the Welcome is answered: two round trips and a frame.
            shape(*linkShape);
            sendUpstream();
            advance(4 * linkShape->oneWayDelayMicroseconds + 2 * syncFrameInterval);
        } else {
            // The Greet reaches the host, then the Welcome the client.
            pump();
            pump();
        }
        CHECK(m_host.state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
        CHECK(m_client.state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
    }

    /// Greets and binds one pane, delivering its first checkpoint.
    ///
    /// - Parameters:
    ///   - datagramMaxBytes: 0 for a link without datagrams.
    ///   - rows: The pane's rows.
    ///   - columns: The pane's columns.
    ///   - initial: What the pane shows before it is bound.
    SyncLink(std::size_t datagramMaxBytes, int rows, int columns = 80, std::string_view initial = "$ ")
        : SyncLink(datagramMaxBytes, syncDefaultFacts(rows, columns))
    {
        (void)addPane(rows, columns, initial);
    }

    /// Binds a fresh pane and runs the link until its checkpoint applies.
    ///
    /// - Parameters:
    ///   - rows: The pane's rows.
    ///   - columns: The pane's columns.
    ///   - initial: What the pane shows before it is bound.
    ///   - scrollbackLines: The rows its history holds; a terminal's own
    ///     default when 0.
    /// - Returns: The pane's handle.
    std::uint32_t addPane(int rows = 24, int columns = 80, std::string_view initial = "$ ", int scrollbackLines = 0)
    {
        const std::uint64_t appliedBefore = m_client.counters().checkpointsApplied;
        const std::uint32_t handle = bindPane(rows, columns, initial, scrollbackLines);
        for (int steps = 0; steps < 4096 && m_client.counters().checkpointsApplied == appliedBefore; ++steps)
            step();
        CHECK_EQ(m_client.counters().checkpointsApplied, appliedBefore + 1);
        return handle;
    }

    /// Steps until the attach is over: the update that follows a checkpoint
    /// with the echo context has arrived for every pane and its `Ack` has
    /// left, so a test that counts what its own act costs starts from a link
    /// that owes nothing.
    void settleAttach()
    {
        const auto isTold = [&](const Pane& pane) { return m_client.echoContext(pane.handle).has_value(); };
        for (int steps = 0; steps < 16 && !std::ranges::all_of(m_panes, isTold); ++steps)
            step();
        step();
        step();
    }

    /// Binds a fresh pane and leaves its checkpoint owed.
    ///
    /// - Parameters:
    ///   - rows: The pane's rows.
    ///   - columns: The pane's columns.
    ///   - initial: What the pane shows before it is bound.
    ///   - scrollbackLines: The rows its history holds; a terminal's own
    ///     default when 0.
    /// - Returns: The pane's handle.
    std::uint32_t bindPane(int rows = 24, int columns = 80, std::string_view initial = "$ ", int scrollbackLines = 0)
    {
        Pane pane;
        pane.terminal = std::make_unique<Ubiquitty::Terminal>();
        if (scrollbackLines > 0)
            Ubiquitty::setScrollbackLimit(pane.terminal.get(), scrollbackLines);
        Ubiquitty::resize(*pane.terminal, rows, columns);
        pane.source = std::make_unique<Ubiquitty::Protocol::Sync::PaneSource>(*pane.terminal, Ubiquitty::Protocol::Sync::ID128 { 9 + m_panes.size(), 10 });
        UbiquittyTest::feed(*pane.terminal, initial);
        pane.source->publish(Ubiquitty::Protocol::Sync::FrameClass::Structural, m_now);
        pane.handle = m_host.bindPane(*pane.source);
        CHECK(pane.handle != 0);
        const std::uint32_t handle = pane.handle;
        m_panes.push_back(std::move(pane));
        return handle;
    }

    /// The pane handles the fixture bound, in bind order.
    [[nodiscard]] std::vector<std::uint32_t> handles() const
    {
        std::vector<std::uint32_t> bound;
        for (const Pane& pane : m_panes)
            bound.push_back(pane.handle);
        return bound;
    }

    /// Writes to the first pane's terminal and publishes the frame it leaves.
    ///
    /// - Parameter bytes: What the program writes.
    void write(std::string_view bytes) { write(handle(), bytes); }

    /// Writes to a pane's terminal and publishes the frame it leaves.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - bytes: What the program writes.
    void write(std::uint32_t handle, std::string_view bytes)
    {
        Pane& pane = paneOf(handle);
        UbiquittyTest::feed(*pane.terminal, bytes);
        pane.source->publish(Ubiquitty::Protocol::Sync::FrameClass::Settled, m_now);
    }

    /// Changes the first pane's terminal through a verb a frontend or the
    /// daemon calls, and publishes the frame it leaves.
    ///
    /// - Parameter change: The change.
    void edit(const std::function<void(Ubiquitty::Terminal&)>& change)
    {
        Pane& pane = paneOf(handle());
        change(*pane.terminal);
        pane.source->publish(Ubiquitty::Protocol::Sync::FrameClass::Settled, m_now);
    }

    /// Writes to the first pane's terminal without publishing, as a program's
    /// output lands between two frames.
    ///
    /// - Parameter bytes: What the program writes.
    void writeUnpublished(std::string_view bytes)
    {
        Pane& pane = paneOf(handle());
        UbiquittyTest::feed(*pane.terminal, bytes);
        pane.source->noteTerminalChanged();
    }

    /// Lets the first pane's terminal and its client's draw sized text, as a
    /// daemon's and a drawing frontend's do, so an OSC 66 run stays a run.
    void drawSizedText()
    {
        paneOf(handle()).terminal->drawsSizedText = true;
        paneOf(handle()).terminal->drawsProportionalText = true;
        if (Ubiquitty::Terminal* client = m_client.paneTerminal(handle())) {
            client->drawsSizedText = true;
            client->drawsProportionalText = true;
        }
    }

    /// Publishes the frame the first pane's terminal holds.
    void publish() { paneOf(handle()).source->publish(Ubiquitty::Protocol::Sync::FrameClass::Settled, m_now); }

    /// Resizes the first pane's terminal and publishes the frame, which only
    /// a checkpoint carries.
    ///
    /// - Parameters:
    ///   - rows: The new rows.
    ///   - columns: The new columns.
    void resize(int rows, int columns)
    {
        Pane& pane = paneOf(handle());
        Ubiquitty::resize(*pane.terminal, rows, columns);
        pane.source->publish(Ubiquitty::Protocol::Sync::FrameClass::Structural, m_now);
    }

    /// Unbinds the first pane on the host.
    void unbind() { m_host.unbindPane(handle(), 0); }

    /// Unbinds a pane on the host.
    ///
    /// - Parameter handle: The pane's handle.
    void unbind(std::uint32_t handle) { m_host.unbindPane(handle, 0); }

    /// Advances one frame interval: the host services, the link carries, the
    /// client may ack. On a shaped link, every event in the interval runs.
    void step()
    {
        if (m_shape) {
            advance(syncFrameInterval);
            return;
        }
        m_now += syncFrameInterval;
        m_host.service(m_now);
        pump();
    }

    /// Moves everything either end has to send, once, on the instant link.
    void pump()
    {
        std::vector<std::uint8_t> stream;
        while (m_host.popStreamFrame(stream, m_now)) {
        }
        if (m_streamFilter)
            m_streamFilter(stream);
        deliverDownstream(std::move(stream));
        for (const std::vector<std::uint8_t>& datagram : m_host.takeDatagrams()) {
            if (m_lossPermille > 0 && m_random() % 1000 < m_lossPermille) {
                ++m_datagramsDropped;
                ++m_lossRun;
                m_longestLossRun = std::max(m_longestLossRun, m_lossRun);
                continue;
            }
            m_lossRun = 0;
            observe(datagram, Carrier::Datagram);
            m_client.receiveDatagram(datagram, m_now);
        }
        m_client.tick(m_now);
        sendUpstream();
    }

    /// Makes the link shaped from now on: a rate host to client and a delay
    /// each way.
    ///
    /// - Parameter shape: The link.
    void shape(LinkShape shape)
    {
        m_shape = shape;
        m_wireFreeAt = m_now;
    }

    /// Moves the clock. On a shaped link every event up to the new time runs
    /// in order; on the instant link nothing is serviced.
    ///
    /// - Parameter microseconds: How far.
    void advance(std::uint64_t microseconds)
    {
        const std::uint64_t until = m_now + microseconds;
        if (!m_shape) {
            m_now = until;
            return;
        }
        for (int events = 0; events < 10'000'000; ++events) {
            const std::uint64_t next = nextShapedEvent();
            if (next > until)
                break;
            m_now = std::max(m_now, next);
            runShapedEvent();
        }
        m_now = until;
    }

    /// Stops the client's frames reaching the host.
    void holdUpstream() { m_isUpstreamHeld = true; }

    /// Stops one kind of the client's frames reaching the host; the others
    /// still flow.
    ///
    /// - Parameter kind: The kind withheld.
    void holdUpstream(FrameKind kind) { m_heldUpstreamKind = kind; }

    /// Delivers what was held and lets the client's frames through again.
    void releaseUpstream()
    {
        m_isUpstreamHeld = false;
        m_heldUpstreamKind.reset();
        receiveUpstream(std::exchange(m_heldUpstream, {}));
    }

    /// Stops the client reading: what the host sends waits in the link.
    void holdDownstream() { m_isDownstreamHeld = true; }

    /// Lets the client read the frames that waited up to and including the
    /// first one `isLast` picks, and keeps the rest waiting: a stream cut
    /// between two frames.
    ///
    /// - Parameter isLast: Takes a frame's kind and body.
    /// - Returns: Whether such a frame was delivered.
    bool releaseDownstreamThrough(const std::function<bool(FrameKind, std::span<const std::uint8_t>)>& isLast)
    {
        std::vector<std::uint8_t> held = std::exchange(m_heldDownstream, {});
        Ubiquitty::Protocol::Sync::FrameDecoder decoder;
        decoder.append(held);
        std::vector<std::uint8_t> delivered;
        bool isFound = false;
        while (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = decoder.next()) {
            std::vector<std::uint8_t>& into = isFound ? m_heldDownstream : delivered;
            CHECK(Ubiquitty::Protocol::Sync::appendFrame(into, frame->kind, frame->channel, frame->body));
            isFound = isFound || isLast(frame->kind, frame->body);
        }
        observe(delivered, Carrier::Stream);
        m_client.receiveStream(delivered, m_now);
        return isFound;
    }

    /// Lets the client read what waited, with the frames `isDeferred` picks
    /// moved behind the others: a carrier whose lanes reorder.
    ///
    /// - Parameter isDeferred: Takes a frame's kind and body.
    void releaseDownstreamReordered(const std::function<bool(FrameKind, std::span<const std::uint8_t>)>& isDeferred)
    {
        m_isDownstreamHeld = false;
        std::vector<std::uint8_t> held = std::exchange(m_heldDownstream, {});
        Ubiquitty::Protocol::Sync::FrameDecoder decoder;
        decoder.append(held);
        std::vector<std::uint8_t> first;
        std::vector<std::uint8_t> last;
        while (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = decoder.next())
            CHECK(Ubiquitty::Protocol::Sync::appendFrame(isDeferred(frame->kind, frame->body) ? last : first, frame->kind, frame->channel, frame->body));
        first.insert(first.end(), last.begin(), last.end());
        deliverDownstream(std::move(first));
    }

    /// Calls back when the client purges a history epoch, as the resume record
    /// and the disk cache are told to.
    ///
    /// - Parameter purge: Takes the retired key and where the clear ended.
    void onPurge(std::function<void(const Ubiquitty::Protocol::Sync::HistoryKey&, std::uint64_t)> purge) { m_purges.onPurge = std::move(purge); }

    /// Lets the client read what waited, and what follows.
    void releaseDownstream()
    {
        m_isDownstreamHeld = false;
        std::vector<std::uint8_t> held = std::exchange(m_heldDownstream, {});
        deliverDownstream(std::move(held));
    }

    /// Calls back for every frame the client is handed, in order.
    ///
    /// - Parameter observer: Takes the frame's kind, channel, carrier and
    ///   bytes on the wire.
    void onHostFrame(std::function<void(FrameKind, std::uint32_t, Carrier, std::size_t)> observer) { m_observer = std::move(observer); }

    /// Chooses the view the client takes of each pane the host binds from now
    /// on, in place of the connection's own.
    ///
    /// - Parameter choose: Takes the pane's handle and returns its view.
    void setViewOnBind(std::function<Ubiquitty::Protocol::Sync::ViewState(std::uint32_t)> choose) { m_recorder.viewOnBindChoice = std::move(choose); }

    /// The times the host received an `Ack`.
    [[nodiscard]] const std::vector<std::uint64_t>& ackTimes() const { return m_ackTimes; }

    /// Hands the host an `Ack` for the first pane, as a client that has not
    /// yet read its `PaneUnbind` sends one.
    void sendStaleAck()
    {
        Ubiquitty::Protocol::Sync::Ack ack;
        ack.panes.push_back({ .handle = handle(), .generation = newest() });
        std::vector<std::uint8_t> body;
        Ubiquitty::Protocol::Sync::encode(body, ack);
        sendToHost(FrameKind::Ack, body);
    }

    /// Hands the client stream bytes as though the host wrote them.
    ///
    /// - Parameter bytes: Whole frames, in stream order.
    void deliverToClient(std::span<const std::uint8_t> bytes) { m_client.receiveStream(bytes, m_now); }

    /// Hands the host a frame of the given kind and body, as a peer that
    /// writes what it likes.
    ///
    /// - Parameters:
    ///   - kind: The frame's kind.
    ///   - body: Its body.
    ///   - channel: Its channel.
    void sendToHost(FrameKind kind, std::span<const std::uint8_t> body, std::uint32_t channel = 0)
    {
        std::vector<std::uint8_t> frame;
        CHECK(Ubiquitty::Protocol::Sync::appendFrame(frame, kind, channel, body));
        m_host.receiveStream(frame, m_now);
    }

    /// Services the host without carrying anything, as a transport that has
    /// taken nothing since the last service.
    void serviceWithoutCarrying()
    {
        m_now += syncFrameInterval;
        m_host.service(m_now);
    }

    /// Services the host at the current time and carries what it sends.
    void serviceNow()
    {
        m_host.service(m_now);
        pump();
    }

    /// The first pane's handle.
    [[nodiscard]] std::uint32_t handle() const { return m_panes.empty() ? 0 : m_panes.front().handle; }

    /// Compares a pane's client with its host: the screen, and the history the
    /// client's cache holds down to `depth` lines below the head.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - depth: How far below the head every line must be held.
    /// - Returns: The first disagreement, or empty.
    [[nodiscard]] std::string disagreement(std::uint32_t handle, std::uint64_t depth = 2'000) const
    {
        if (std::string screen = UbiquittyTest::firstDisagreement(hostTerminal(handle), clientTerminal(handle)); !screen.empty())
            return screen;
        const Ubiquitty::Protocol::Sync::HistoryCache* cache = m_client.historyCache(handle);
        if (!cache)
            return "the client holds no history for the pane";
        return UbiquittyTest::historyDisagreement(hostTerminal(handle), *cache, depth);
    }

    /// Steps until the host has nothing queued, the client holds the newest
    /// generation of every pane and owes no `Ack`, or the bound runs out.
    ///
    /// - Parameter maxSteps: The most frame intervals to run.
    void stepUntilQuiet(int maxSteps = 4'096)
    {
        for (int steps = 0; steps < maxSteps; ++steps) {
            step();
            const bool isCurrent = std::ranges::all_of(m_panes, [&](const Pane& pane) { return m_client.paneGeneration(pane.handle) == pane.source->generation(); });
            const bool isHostDue = m_host.nextServiceMicroseconds(m_now).transform([&](std::uint64_t due) { return due <= m_now; }).value_or(false);
            if (isCurrent && !m_host.hasQueuedStreamFrames() && !isHostDue && !m_client.nextTickMicroseconds().has_value())
                return;
        }
    }

    /// Drops datagrams at random from now on.
    ///
    /// - Parameter permille: How many in a thousand are lost.
    void setLossPermille(std::uint32_t permille) { m_lossPermille = permille; }

    /// Rewrites the host's stream bytes before the client reads them.
    ///
    /// - Parameter filter: Takes the bytes of one carry, in place.
    void setStreamFilter(std::function<void(std::vector<std::uint8_t>&)> filter) { m_streamFilter = std::move(filter); }

    [[nodiscard]] const Ubiquitty::Terminal& hostTerminal() const { return hostTerminal(handle()); }
    [[nodiscard]] const Ubiquitty::Terminal& hostTerminal(std::uint32_t handle) const { return *paneOf(handle).terminal; }
    [[nodiscard]] const Ubiquitty::Terminal& clientTerminal() const { return clientTerminal(handle()); }
    [[nodiscard]] const Ubiquitty::Terminal& clientTerminal(std::uint32_t handle) const { return *m_client.paneTerminal(handle); }
    [[nodiscard]] Ubiquitty::Protocol::Sync::PaneSource& source(std::uint32_t handle) { return *paneOf(handle).source; }
    [[nodiscard]] std::uint64_t newest() const { return newest(handle()); }
    [[nodiscard]] std::uint64_t newest(std::uint32_t handle) const { return paneOf(handle).source->generation(); }
    [[nodiscard]] std::uint64_t held() const { return held(handle()); }
    [[nodiscard]] std::uint64_t held(std::uint32_t handle) const { return m_client.paneGeneration(handle); }
    [[nodiscard]] Ubiquitty::Protocol::Sync::HostConnection& host() { return m_host; }
    [[nodiscard]] const Ubiquitty::Protocol::Sync::HostConnection& host() const { return m_host; }
    [[nodiscard]] Ubiquitty::Protocol::Sync::ClientConnection& client() { return m_client; }
    [[nodiscard]] const Ubiquitty::Protocol::Sync::ClientConnection& client() const { return m_client; }
    [[nodiscard]] std::uint64_t now() const { return m_now; }
    [[nodiscard]] std::uint64_t datagramsDropped() const { return m_datagramsDropped; }
    [[nodiscard]] std::uint64_t longestLossRun() const { return m_longestLossRun; }

private:
    /// One pane the fixture made.
    struct Pane {
        std::unique_ptr<Ubiquitty::Terminal> terminal;
        std::unique_ptr<Ubiquitty::Protocol::Sync::PaneSource> source;
        std::uint32_t handle = 0;
    };

    /// Bytes in flight on the shaped link.
    struct InFlight {
        std::uint64_t arrivesAt = 0;
        std::vector<std::uint8_t> bytes;
    };

    [[nodiscard]] Pane& paneOf(std::uint32_t handle)
    {
        const auto found = std::ranges::find_if(m_panes, [&](const Pane& pane) { return pane.handle == handle; });
        if (found == m_panes.end())
            std::abort();
        return *found;
    }

    [[nodiscard]] const Pane& paneOf(std::uint32_t handle) const
    {
        const auto found = std::ranges::find_if(m_panes, [&](const Pane& pane) { return pane.handle == handle; });
        if (found == m_panes.end())
            std::abort();
        return *found;
    }

    /// Hands the client stream bytes, unless it has stopped reading.
    ///
    /// - Parameter stream: Whole frames, in stream order.
    void deliverDownstream(std::vector<std::uint8_t> stream)
    {
        if (m_isDownstreamHeld) {
            m_heldDownstream.insert(m_heldDownstream.end(), stream.begin(), stream.end());
            return;
        }
        observe(stream, Carrier::Stream);
        m_client.receiveStream(stream, m_now);
    }

    /// Tells the observer about each frame in bytes the client is handed.
    ///
    /// - Parameters:
    ///   - bytes: Stream bytes, or one datagram.
    ///   - carrier: Which of the two.
    void observe(std::span<const std::uint8_t> bytes, Carrier carrier)
    {
        if (!m_observer || bytes.empty())
            return;
        if (carrier == Carrier::Datagram) {
            Ubiquitty::Protocol::Sync::EnvelopeError error = Ubiquitty::Protocol::Sync::EnvelopeError::None;
            if (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = Ubiquitty::Protocol::Sync::decodeDatagram(bytes, error))
                m_observer(frame->kind, frame->channel, carrier, bytes.size());
            return;
        }
        m_observed.append(bytes);
        while (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = m_observed.next())
            m_observer(frame->kind, frame->channel, carrier, Ubiquitty::Protocol::Sync::frameBytes(frame->channel, frame->body.size()));
    }

    /// Sends the client's frames to the host: at once on the instant link, a
    /// one-way delay after on a shaped one, and not at all while held.
    void sendUpstream()
    {
        for (const std::vector<std::uint8_t>& datagram : m_client.takeDatagrams())
            m_host.receiveDatagram(datagram, m_now);
        std::vector<std::uint8_t> upstream = m_client.takeStreamOutput();
        if (upstream.empty())
            return;
        if (m_heldUpstreamKind) {
            // Withholds one kind: the rest is re-framed and sent.
            Ubiquitty::Protocol::Sync::FrameDecoder decoder;
            decoder.append(upstream);
            std::vector<std::uint8_t> passed;
            while (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = decoder.next()) {
                std::vector<std::uint8_t>& into = frame->kind == *m_heldUpstreamKind ? m_heldUpstream : passed;
                CHECK(Ubiquitty::Protocol::Sync::appendFrame(into, frame->kind, frame->channel, frame->body));
            }
            upstream = std::move(passed);
        }
        if (m_isUpstreamHeld) {
            m_heldUpstream.insert(m_heldUpstream.end(), upstream.begin(), upstream.end());
            return;
        }
        if (m_shape) {
            m_upstream.push_back({ .arrivesAt = m_now + m_shape->oneWayDelayMicroseconds, .bytes = std::move(upstream) });
            return;
        }
        receiveUpstream(upstream);
    }

    /// Hands the host the client's stream bytes, counting its acks.
    ///
    /// - Parameter upstream: Whole frames, in stream order.
    void receiveUpstream(std::span<const std::uint8_t> upstream)
    {
        Ubiquitty::Protocol::Sync::FrameDecoder decoder;
        decoder.append(upstream);
        while (const std::optional<Ubiquitty::Protocol::Sync::Frame> frame = decoder.next()) {
            if (frame->kind == FrameKind::Ack)
                m_ackTimes.push_back(m_now);
        }
        m_host.receiveStream(upstream, m_now);
    }

    /// The soonest event on the shaped link: an arrival either way, the
    /// client's next ack, or the host's next service.
    [[nodiscard]] std::uint64_t nextShapedEvent() const
    {
        std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
        if (!m_downstream.empty())
            next = std::min(next, m_downstream.front().arrivesAt);
        if (!m_datagrams.empty())
            next = std::min(next, m_datagrams.front().arrivesAt);
        if (!m_upstream.empty())
            next = std::min(next, m_upstream.front().arrivesAt);
        if (const std::optional<std::uint64_t> tick = m_client.nextTickMicroseconds())
            next = std::min(next, std::max(*tick, m_now));
        if (const std::optional<std::uint64_t> due = m_host.nextServiceMicroseconds(m_now))
            next = std::min(next, std::max(*due, m_now + 1));
        return next;
    }

    /// Runs everything due at the current time on the shaped link.
    void runShapedEvent()
    {
        while (!m_downstream.empty() && m_downstream.front().arrivesAt <= m_now) {
            deliverDownstream(std::move(m_downstream.front().bytes));
            m_downstream.pop_front();
        }
        while (!m_datagrams.empty() && m_datagrams.front().arrivesAt <= m_now) {
            observe(m_datagrams.front().bytes, Carrier::Datagram);
            m_client.receiveDatagram(m_datagrams.front().bytes, m_now);
            m_datagrams.pop_front();
        }
        m_client.tick(m_now);
        sendUpstream();
        while (!m_upstream.empty() && m_upstream.front().arrivesAt <= m_now) {
            receiveUpstream(m_upstream.front().bytes);
            m_upstream.pop_front();
        }
        m_host.service(m_now);
        // A datagram crosses beside the relay, charged the delay alone.
        for (std::vector<std::uint8_t>& datagram : m_host.takeDatagrams())
            m_datagrams.push_back({ .arrivesAt = m_now + m_shape->oneWayDelayMicroseconds, .bytes = std::move(datagram) });
        // The relay takes every frame the host lets go and queues it behind
        // the ones still crossing.
        for (;;) {
            std::vector<std::uint8_t> frame;
            if (!m_host.popStreamFrame(frame, m_now))
                break;
            if (m_streamFilter)
                m_streamFilter(frame);
            m_wireFreeAt = std::max(m_wireFreeAt, m_now) + frame.size() * 1'000'000 / m_shape->bytesPerSecond;
            m_downstream.push_back({ .arrivesAt = m_wireFreeAt + m_shape->oneWayDelayMicroseconds, .bytes = std::move(frame) });
        }
    }

    /// Records the Sync frames the host sends the client's panes.
    class Recorder final : public Ubiquitty::Protocol::Sync::ClientPaneDelegate {
    public:
        /// The view a test chose for a newly bound pane, or the connection's.
        ///
        /// - Parameter handle: The pane's handle.
        /// - Returns: The view.
        std::optional<Ubiquitty::Protocol::Sync::ViewState> viewOnBind(std::uint32_t handle, Ubiquitty::Protocol::Sync::ID128) override
        {
            if (!viewOnBindChoice)
                return std::nullopt;
            return viewOnBindChoice(handle);
        }

        std::function<Ubiquitty::Protocol::Sync::ViewState(std::uint32_t)> viewOnBindChoice;
    };

    /// Forwards the client's purges to a test.
    class PurgeRecorder final : public Ubiquitty::Protocol::Sync::HistoryPurgeSink {
    public:
        /// Hands the purge to the test's callback.
        ///
        /// - Parameters:
        ///   - retired: The key the cache held before the reset.
        ///   - clearedThrough: Every line below it is gone.
        /// - Returns: Confirmed retirement after recording the call.
        Ubiquitty::Protocol::Sync::HistoryPurgeOutcome purge(const Ubiquitty::Protocol::Sync::HistoryKey& retired, std::uint64_t clearedThrough) override
        {
            if (onPurge)
                onPurge(retired, clearedThrough);
            return Ubiquitty::Protocol::Sync::HistoryPurgeOutcome::Purged;
        }

        std::function<void(const Ubiquitty::Protocol::Sync::HistoryKey&, std::uint64_t)> onPurge;
    };

    // Sources and delegates outlive both connection ends during teardown.
    std::vector<Pane> m_panes;
    Recorder m_recorder;
    PurgeRecorder m_purges;
    Ubiquitty::Protocol::Sync::HostConnection m_host;
    Ubiquitty::Protocol::Sync::ClientConnection m_client;
    std::uint64_t m_now = 1'000'000;
    std::mt19937 m_random { 20'261'009 };
    std::uint32_t m_lossPermille = 0;
    std::uint64_t m_datagramsDropped = 0;
    std::uint64_t m_lossRun = 0;
    std::uint64_t m_longestLossRun = 0;
    std::function<void(std::vector<std::uint8_t>&)> m_streamFilter;
    std::function<void(FrameKind, std::uint32_t, Carrier, std::size_t)> m_observer;
    Ubiquitty::Protocol::Sync::FrameDecoder m_observed;
    bool m_isUpstreamHeld = false;
    std::optional<FrameKind> m_heldUpstreamKind;
    std::vector<std::uint8_t> m_heldUpstream;
    bool m_isDownstreamHeld = false;
    std::vector<std::uint8_t> m_heldDownstream;
    std::vector<std::uint64_t> m_ackTimes;
    std::optional<LinkShape> m_shape;
    std::uint64_t m_wireFreeAt = 0;
    std::deque<InFlight> m_downstream;
    std::deque<InFlight> m_datagrams;
    std::deque<InFlight> m_upstream;
};

} // namespace UbiquittyTest
