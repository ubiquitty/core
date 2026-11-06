// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// AttachmentLink — a host's attachment table and pane, a client's
// ClientAttachment, and an instant link between them whose two directions a
// test can cut: the fixture for the tests of attachments that outlive their
// connections (test_sync_client_attachment, test_sync_liveness).

#include "SyncLink.h"

#include "Core/ClusterPool.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/RunTextStore.h"
#include "Core/Tek/Tek4014.h"
#include "PAL/OutputQueue.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace UbiquittyTest::AttachmentLink {

using namespace Ubiquitty::Protocol::Sync;

/// Secrets drawn in sequence, so a test knows what was issued.
class SequenceSecrets final : public SecretSource {
public:
    /// - Parameter first: The count the sequence starts after, so two hosts
    ///   draw different secrets.
    explicit SequenceSecrets(std::uint64_t first = 0)
        : m_next(first)
    {
    }

    [[nodiscard]] ID128 nextSecret() override { return { ++m_next, 0xFEED }; }

private:
    std::uint64_t m_next = 0;
};

/// The pane `OwnedPane` makes.
inline constexpr ID128 paneID { 9, 10 };
/// The client every test attaches as.
inline constexpr ID128 clientInstance { 7, 8 };
/// The authority every connection is served under.
inline const ConnectionAuthority owner { .principal = "alice", .mayDrive = true, .verifiedKind = ClientKind::Human };

/// The client's identity: a human, focused, at 80x24.
///
/// - Returns: The identity, with no resume.
inline ClientIdentity clientIdentity()
{
    return { .clientInstance = clientInstance, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts(), .resume = std::nullopt };
}

/// Bytes as an input event.
///
/// - Parameter text: The bytes.
/// - Returns: The event.
inline RawInput raw(const std::string& text)
{
    return { .bytes = { text.begin(), text.end() } };
}

/// One host instance: its attachments, its pane, and the connection that
/// serves the client now.
struct Host {
    SequenceSecrets secrets;
    AttachmentTable table;
    OwnedPane pane;
    std::unique_ptr<HostConnection> connection;
    /// A connection the host still holds after another was accepted: one
    /// whose transport the client found dead and the host has not.
    std::unique_ptr<HostConnection> previous;
    std::uint64_t connectionNumber = 0;
    /// Whether the host still has the pane.
    bool hasPane = true;

    explicit Host(ID128 instance)
        : table(instance, measuredResumePolicy(), secrets)
    {
        table.setRetireRecord([this](ID128 attachmentID) {
            (void)pane.source->effects().noteState(attachmentID, AttachmentState::Detached, 0);
        });
    }

    /// Accepts a connection, still holding the one before it.
    void acceptKeepingTheOld()
    {
        previous = std::move(connection);
        accept();
    }

    /// Accepts a connection.
    void accept()
    {
        connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = table.hostInstanceID().low, .socketName = "attachment-test.sock", .buildFlags = 0, .hostPlatform = "fixture" } });
        connection->serveFromTable(table, owner, ++connectionNumber, [this](ID128 id) { return id == paneID && hasPane ? pane.source.get() : nullptr; });
    }

    /// Loses the connection's transport.
    ///
    /// - Parameter now: The host clock.
    void lose(std::uint64_t now)
    {
        if (connection && connection->attachment())
            table.noteTransportLost(connection->attachment()->attachmentID, connectionNumber, now);
        connection.reset();
    }
};

/// A receiver's bounds for one carried pane, ample for these fixtures.
inline constexpr HandoverPaneLimits handoverPaneLimits {
    { 64 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 5 * 1024 * 1024, 256 * 1024, 4096,
        { 8192, Ubiquitty::Image::terminalImageBytesBudgetDefault }, { 4 * 1024 * 1024, 20000, 1024, 100000 },
        { 4 * 1024 * 1024, Ubiquitty::ClusterPool::maxEntries, Ubiquitty::RunTextStore::maxEntries, 1024 * 1024 },
        { 256 * 1024, Ubiquitty::Tek::tekMaxPrimitives }, { 32 * 1024 * 1024, 28 * 1024 * 1024 } },
    { 1024 * 1024, 65536, 4096, 4096, 4096 },
    72 * 1024 * 1024, 5 * 1024 * 1024, 1024 * 1024, 4 * 1024 * 1024, 32 * 1024 * 1024, 64
};

/// The host instance a handover starts: the predecessor's attachments, and
/// its pane rebuilt from the carried record into a terminal and source of
/// its own.
struct SuccessorHost {
    SequenceSecrets secrets { 1000 };
    AttachmentTable table;
    std::unique_ptr<Ubiquitty::Terminal> terminal;
    std::unique_ptr<PaneSource> source;
    RecordingWriter writer;
    std::unique_ptr<HostConnection> connection;
    std::uint64_t connectionNumber = 0;

    explicit SuccessorHost(ID128 instance)
        : table(instance, measuredResumePolicy(), secrets)
    {
        table.setRetireRecord([this](ID128 attachmentID) {
            if (source)
                (void)source->effects().noteState(attachmentID, AttachmentState::Detached, 0);
        });
    }

    /// Accepts a connection.
    void accept()
    {
        connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = table.hostInstanceID().low, .socketName = "attachment-successor.sock", .buildFlags = 0, .hostPlatform = "fixture" } });
        connection->serveFromTable(table, owner, ++connectionNumber, [this](ID128 id) { return id == paneID ? source.get() : nullptr; });
    }

    /// Loses the connection's transport.
    ///
    /// - Parameter now: The host clock.
    void lose(std::uint64_t now)
    {
        if (connection && connection->attachment())
            table.noteTransportLost(connection->attachment()->attachmentID, connectionNumber, now);
        connection.reset();
    }
};

/// Hands the predecessor's pane and attachments to a new host instance, as
/// the whole-host transaction carries them: the pane through its encoded
/// record, the table through its continuation. The predecessor no longer
/// serves the pane.
///
/// - Parameters:
///   - predecessor: The host handing over, its pane at a published frame.
///   - successor: The new instance.
///   - now: The shared monotonic time.
/// - Returns: The successor, or null when any part was refused.
inline std::unique_ptr<SuccessorHost> handOver(Host& predecessor, ID128 successor, std::uint64_t now)
{
    const auto refuse = [](const char* why) {
        std::fprintf(stderr, "handOver: %s\n", why);
        return std::unique_ptr<SuccessorHost> {};
    };
    auto result = std::make_unique<SuccessorHost>(successor);
    if (!result->table.inherit(predecessor.table.exportForHandover(now), predecessor.table.hostInstanceID(), now))
        return refuse("the attachment table was not inherited");
    SessionFacts session;
    session.sessionID = 9;
    session.createdUnix = 50;
    session.describedStructuralGeneration = predecessor.pane.source->structuralGeneration();
    const auto unwritten = Ubiquitty::PAL::OutputQueue::Unwritten::copyOf({}, {});
    if (!unwritten)
        return refuse("no unwritten queue");
    const auto record = encodePaneHandover(*predecessor.pane.source, session, *unwritten, handoverPaneLimits);
    if (!record) {
        std::fprintf(stderr, "handOver: the pane did not encode (%d)\n", static_cast<int>(record.error()));
        return nullptr;
    }
    auto image = decodePaneHandover(record->view(), handoverPaneLimits);
    if (!image) {
        std::fprintf(stderr, "handOver: the pane did not decode (%d)\n", static_cast<int>(image.error()));
        return nullptr;
    }
    result->terminal = std::move(image->terminal);
    result->source = std::move(image->source);
    result->source->setInputWriter(result->writer);
    predecessor.hasPane = false;
    return result;
}

/// Ends the client's connection as a host handing over does: `Bye{HandingOver}`
/// on the wire, then the transport gone at both ends.
///
/// - Parameters:
///   - host: The predecessor.
///   - client: The client.
///   - now: The time.
inline void sayHandingOver(Host& host, ClientAttachment& client, std::uint64_t now)
{
    host.connection->closeDiscardingQueue(ByeReason::HandingOver, "handing over");
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    client.receiveStream(down, now);
    client.tick(now);
    host.lose(now);
}

/// Which way bytes may cross the link.
struct Carry {
    bool shouldCarryDown = true;
    bool shouldCarryUp = true;
};

/// Runs the host, then moves what each end has to send once.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time.
///   - carry: Which ways bytes cross; what does not is lost.
inline void pump(auto& host, ClientAttachment& client, std::uint64_t now, Carry carry = {})
{
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    const ClientConnection* receiver = client.connection();
    if (carry.shouldCarryDown)
        client.receiveStream(down, now);
    // A notification can synchronously replace the link and advance its
    // clock. This obsolete pump must not tick the replacement at the old time
    // or carry its output over the previous read's transport turn.
    if (client.connection() != receiver)
        return;
    client.tick(now);
    const std::vector<std::uint8_t> up = client.takeStreamOutput();
    if (carry.shouldCarryUp)
        host.connection->receiveStream(up, now);
}

/// Pumps the link every millisecond for a span.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time, advanced past the span.
///   - span: How long.
///   - carry: Which ways bytes cross.
inline void pumpFor(auto& host, ClientAttachment& client, std::uint64_t& now, std::uint64_t span, Carry carry = {})
{
    for (const std::uint64_t until = now + span; now < until; now += 1'000)
        pump(host, client, now, carry);
}

/// Attaches a client fresh: greeted, the pane bound and shown.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time, advanced past the attach.
inline void attachFresh(Host& host, ClientAttachment& client, std::uint64_t& now)
{
    host.accept();
    (void)client.connect(now);
    pump(host, client, now);
    pump(host, client, now);
    (void)host.connection->bindPane(*host.pane.source);
    pumpFor(host, client, now, 100'000);
}

/// Runs the host, then hands the client what it sent one frame at a time,
/// carrying the client's answer to each before the next: a client that acts
/// on a frame before the rest arrive shows it.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time.
inline void pumpFrameByFrame(Host& host, ClientAttachment& client, std::uint64_t now)
{
    client.tick(now);
    host.connection->receiveStream(client.takeStreamOutput(), now);
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    FrameDecoder decoder;
    decoder.append(down);
    while (const std::optional<Frame> frame = decoder.next()) {
        std::vector<std::uint8_t> one;
        (void)appendFrame(one, frame->kind, frame->channel, frame->body);
        client.receiveStream(one, now);
        client.tick(now);
        host.connection->receiveStream(client.takeStreamOutput(), now);
    }
}

/// Drops the transport at both ends.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time.
inline void drop(auto& host, ClientAttachment& client, std::uint64_t now)
{
    client.transportLost(now);
    host.lose(now);
}

/// Connects again, resuming what the client holds.
///
/// - Parameters:
///   - host: The host.
///   - client: The client.
///   - now: The time, advanced past the resume.
inline void reconnect(auto& host, ClientAttachment& client, std::uint64_t& now)
{
    host.accept();
    (void)client.connect(now);
    pumpFor(host, client, now, 200'000);
}

} // namespace UbiquittyTest::AttachmentLink
