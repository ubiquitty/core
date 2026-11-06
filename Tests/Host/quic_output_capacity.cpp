// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_output_capacity.cpp — a stalled/desynced QUIC reader's queued-but-
// unsent bytes are bounded by the per-client output high-water cap.
//
// Daemon::queueToClient caps each client's pending output at clientBufferHighWater
// so a reader that can't keep up can't grow daemon memory without bound.
// A QUIC client clears outputBuffer on every flush (the bytes move into the
// connection's own toSend/toSendDatagrams queues), so the outputBuffer cap alone
// never bites for QUIC. The cap counts the connection's unsent bytes as well.
// Retained bytes include in-flight data, but that data must not be mistaken
// for an unsent backlog when pacing screen frames. An excessive consumed
// count must never wrap the pending sum below the cap.
//
// This befriends the Daemon to push a Client with a real (handshake-less)
// quicConnection, stamps the connection's stream-backlog accounting via the
// HostQUIC test-support seam, drives the PRIVATE queueToClient, and reads back
// c.didOverflow / c.outputBuffer:
//   (a) STALLED-READER BOUND: a near-cap stalled backlog + one more frame →
//       c.didOverflow becomes true without retaining the new frame.
//   (b) DESYNC-no-wrap: a toSendConsumed > toSend.size() desync + enough output to
//       exceed the cap → c.didOverflow becomes true (the corrupted accounting
//       does not read as under-cap).
//
// Built only under -DUBIQUITTY_QUIC=ON (the QUIC accounting is #ifdef-gated).
// Exit 0 on success.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"
#include "Platform/Host/OutboundCapacity.h"
#include "Platform/Host/TokenStore.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

// The daemon's own outbound caps (Platform/Host/OutboundCapacity.h).

// Friend seam: reach the private client list, the QUIC listener, and queueToClient.
struct QUICOutputCapacityAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); } // ephemeral listener → m_quic non-null
    static bool haveQUIC(const Daemon& d) { return d.m_quic != nullptr; }

    // Add a QUIC client (non-null connection, no socket fd) and return its index.
    static std::size_t addQUICClient(Daemon& d, QUICConnection* connection)
    {
        Daemon::Client c;
        // fileDescriptor left default-constructed (empty/-1): this is a QUIC-only
        // client with no Unix socket.
        c.quicConnection = connection;
        c.clientID = d.m_nextClientID++;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static void queueToClient(Daemon& d, std::size_t index, const std::vector<std::uint8_t>& bytes) { d.queueToClient(index, bytes); }
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static std::size_t outputBufferSize(const Daemon& d, std::size_t index) { return d.m_clients[index].outputBuffer.size(); }
    /// Everything the daemon queued for a QUIC client and has not sent: what
    /// it handed the relay stream past a staged prefix, then what its output
    /// buffer still holds. A first attach flushes its answer to the stream
    /// at once, so the output buffer alone misses it.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - stagedBytes: Unsent stream bytes the test staged before the client spoke.
    /// - Returns: The queued bytes, in stream order.
    static std::vector<std::uint8_t> queued(const Daemon& d, std::size_t index, std::size_t stagedBytes)
    {
        const auto& client = d.m_clients[index];
        std::vector<std::uint8_t> bytes = testQUICConnectionPendingBytes(client.quicConnection);
        bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(stagedBytes, bytes.size())));
        bytes.insert(bytes.end(), client.outputBuffer.begin() + static_cast<std::ptrdiff_t>(client.outputConsumed), client.outputBuffer.end());
        return bytes;
    }
    // Stage a client whose own outputBuffer already holds `bytes` (the bytes a QUIC
    // client accumulates between flushes) — lets a test exercise the cap's
    // `outputBuffer.size() + pendingBytes()` SUM, where an unguarded pendingBytes
    // underflow can wrap that sum below the cap.
    static void setOutputBufferSize(Daemon& d, std::size_t index, std::size_t bytes) { d.m_clients[index].outputBuffer.assign(bytes, 0u); }

    static std::size_t lowWater() { return screenOutputLowWaterBytes; }

    // The datagram path does not go through queueToClient, so its bound has to be
    // asked of the listener directly, the way the daemon's delta fast path asks it.
    static void sendDatagram(Daemon& d, Ubiquitty::Host::QUICConnection* connection, const std::vector<std::uint8_t>& bytes)
    {
        d.m_quic->sendDatagram(connection, bytes);
    }
    static std::size_t pendingBytes(Daemon& d, Ubiquitty::Host::QUICConnection* connection)
    {
        return d.m_quic->pendingBytes(connection);
    }

    // A session to publish, and the screen step: whether a client's output
    // has room for a screen frame is read there, so that is where a QUIC
    // backlog has to be seen.
    static Daemon::Session& createSession(Daemon& d)
    {
        return ScreenPlaneAccess::makeSession(d, 24, 80);
    }
    // The bare QUIC connection is only the output queue's routing key, not a
    // TLS or transport-preamble proof. Production Greet verifies this stored
    // credential and the exact principal's prior session grant before binding.
    static bool subscribe(Daemon& d, std::size_t index, Daemon::Session& session,
        const std::string& tokenPath, const std::string& token, std::size_t stagedBytes)
    {
        namespace Sync = Protocol::Sync;
        d.m_tokenPath = tokenPath;
        (void)session.addSessionGrant("token:" + Crypto::sha256Hex(token));
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { .low = d.m_clients[index].clientID, .high = 0x636170 };
        greeting.clientKind = Sync::ClientKind::Agent;
        greeting.requestedRole = Sync::Role::Agent;
        greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = session.id, .expectedDaemonInstanceID = d.m_daemonInstanceID, .agentLabel = "output-capacity", .isAttachOnly = true };
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        greeting.credential.emplace(token.begin(), token.end());
        std::vector<std::uint8_t> body, wire;
        Sync::encode(body, greeting);
        if (body.empty() || !Sync::decodeGreet(body) || !Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body))
            return false;
        ScreenPlaneAccess::receive(d, index, wire);
        const auto& client = d.m_clients[index];
        if (client.didOverflow || !client.syncConnection || client.syncConnection->state() != Sync::ConnectionState::Attached
            || !client.hasToken || !digestsEqual(client.tokenID, Crypto::sha256Hex(token))
            || client.tokenScope != Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe }
            || !d.hasLiveStructureAttachment(client))
            return false;
        Sync::FrameDecoder decoder;
        decoder.append(queued(d, index, stagedBytes));
        unsigned welcomes = 0;
        std::uint32_t handle = 0;
        while (const auto frame = decoder.next()) {
            if (frame->kind == Sync::FrameKind::Welcome) {
                const auto welcome = Sync::decodeWelcome(frame->body);
                if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted
                    || welcome->selection.target != Sync::AdmissionTarget::Session
                    || welcome->selection.sessionID != session.id || welcome->selection.daemonInstanceID != d.m_daemonInstanceID
                    || welcome->grantedRole != Sync::Role::Agent)
                    return false;
                ++welcomes;
            } else if (frame->kind == Sync::FrameKind::PaneBind) {
                const auto bind = Sync::decodePaneBind(frame->body);
                if (!bind || handle || bind->paneID != session.screenSource->paneID())
                    return false;
                handle = bind->handle;
            }
        }
        if (welcomes != 1 || !handle || decoder.error() != Sync::EnvelopeError::None || decoder.bufferedBytes())
            return false;
        body.clear();
        wire.clear();
        Sync::encode(body, Sync::View { .state = Sync::ViewState::Live });
        if (!Sync::appendFrame(wire, Sync::FrameKind::View, handle, body))
            return false;
        ScreenPlaneAccess::receive(d, index, wire);
        const auto& selected = d.m_clients[index];
        return !selected.didOverflow && selected.syncConnection->paneView(handle) == Sync::ViewState::Live;
    }
    static void publishAndService(Daemon& d, std::size_t index, Daemon::Session& session)
    {
        session.isFrameDue = true;
        d.publishFrame(session, Runtime::FrameClass::Settled);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::clientBufferHighWater;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICOutputCapacityAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "quic_output_capacity: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    // Isolated control dir so the listener's self-signed cert lands in a temp
    // tree, never a real ~/.ubiquitty.
    char homeTemplate[] = "/tmp/ubiquitty-quic-outputcap.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_output_capacity: mkdtemp failed\n");
        return 1;
    }
    std::string ubiquittyHome = std::string(home) + "/.ubiquitty";
    ::setenv("UBIQUITTY_HOME", ubiquittyHome.c_str(), 1);

    // The daemon's clients borrow these routing keys, including after a case
    // ends: aggregate capacity walks every client. Destroy the daemon first.
    using ConnectionOwner = std::unique_ptr<Ubiquitty::Host::QUICConnection, decltype(&Ubiquitty::Host::destroyTestQUICConnection)>;
    std::vector<ConnectionOwner> connections;
    const auto makeConnection = [&] {
        connections.emplace_back(Ubiquitty::Host::makeTestQUICConnection(), &Ubiquitty::Host::destroyTestQUICConnection);
        return connections.back().get();
    };
    Daemon d;
    // A real listener so m_quic is non-null (queueToClient's QUIC count is gated on
    // c.quicConnection && m_quic). No peer handshake — pendingBytes reads the passed
    // connection's buffers directly, it doesn't look it up in the listener.
    if (!QUICOutputCapacityAccess::startQUIC(d)) {
        std::fprintf(stderr, "quic_output_capacity: startQUIC failed (QUIC build/cert env?)\n");
        return 1;
    }
    check(QUICOutputCapacityAccess::haveQUIC(d), "the QUIC listener is live (m_quic non-null)");
    const std::string token = "quic-output-capacity-owned-token";
    const std::string tokenPath = ubiquittyHome + "/tokens";
    Ubiquitty::Host::TokenRecord tokenRecord;
    tokenRecord.hashHex = Ubiquitty::Crypto::sha256Hex(token);
    tokenRecord.scope = { Ubiquitty::Protocol::Sync::Scope::Observe };
    tokenRecord.label = "output-capacity";
    if (Ubiquitty::Host::writeTokens(tokenPath, { tokenRecord }) != Ubiquitty::Host::TokenWriteOutcome::Durable) {
        check(false, "the fixture's own credential was durably stored");
        return 1;
    }
    auto& session = QUICOutputCapacityAccess::createSession(d);

    // --- (a) STALLED-READER BOUND -------------------------------------------------
    // A QUIC connection whose stream backlog is parked near the cap (a stalled /
    // flow-controlled reader): toSend holds (cap - 1 KiB) un-consumed bytes. The
    // client's own outputBuffer is empty (QUIC clears it on every flush), so without
    // counting the connection backlog the cap never bites. One more 4 KiB frame must
    // tip pending past clientBufferHighWater → overflowed.
    {
        Ubiquitty::Host::QUICConnection* connection = makeConnection();
        check(connection != nullptr, "(a) test QUIC connection allocates");
        const std::size_t stalled = clientBufferHighWater - 1024; // just under the cap
        Ubiquitty::Host::setTestQUICConnectionPending(connection, /*toSendSize*/ stalled, /*toSendConsumed*/ 0);

        std::size_t idx = QUICOutputCapacityAccess::addQUICClient(d, connection);
        std::vector<std::uint8_t> frame(4096, 0xAB); // pending + 4 KiB > cap
        QUICOutputCapacityAccess::queueToClient(d, idx, frame);

        check(QUICOutputCapacityAccess::didOverflow(d, idx),
            "(a) a near-cap stalled QUIC backlog + one frame OVERFLOWS the client");
        check(QUICOutputCapacityAccess::outputBufferSize(d, idx) == 0, "(a) the overflowed client's outputBuffer is reclaimed (cleared)");
    }

    // --- (c) DATAGRAM QUEUE BOUND -------------------------------------------------
    // The unreliable half of the same policy. A delta small enough to ride one
    // DATAGRAM bypasses queueToClient entirely and lands in the connection's
    // datagram queue, which the write pump drains — and a peer that stops reading
    // pumps nothing, so the queue grew by one delta per frame with no ceiling,
    // while the reliable stream beside it had one and dropped the client past it.
    //
    // Frames are tagged by their first byte so the case can say which end the
    // bound gave up. The newest are the ones worth keeping: they describe the
    // screen as it is, and the seq gap a drop opens is what asks for the keyframe
    // that replaces the rest.
    {
        Ubiquitty::Host::QUICConnection* connection = makeConnection();
        check(connection != nullptr, "(c) test QUIC connection allocates");

        const std::size_t datagramBytes = Ubiquitty::Host::quicDatagramMaxBytes;
        const std::size_t fits = Ubiquitty::Host::quicDatagramQueueHighWaterBytes / datagramBytes;
        // Twice what the bound holds, so every one of the first half has to be
        // given up for the second half to be there.
        for (std::size_t i = 0; i < fits * 2; ++i) {
            std::vector<std::uint8_t> datagram(datagramBytes, 0u);
            datagram[0] = static_cast<std::uint8_t>(i < fits ? 1u : 2u); // first half, second half
            QUICOutputCapacityAccess::sendDatagram(d, connection, datagram);
        }

        check(QUICOutputCapacityAccess::pendingBytes(d, connection) <= Ubiquitty::Host::quicDatagramQueueHighWaterBytes,
            "(c) a peer that never drains its datagrams cannot grow the queue past its bound");
        check(Ubiquitty::Host::testQUICQueuedDatagramCount(connection) > 0,
            "(c) the bound drops frames rather than emptying the queue");
        check(Ubiquitty::Host::testQUICFrontDatagramTag(connection) == 2,
            "(c) the oldest frames are the ones given up, so what is left describes the screen as it is");
    }

    // --- (b) DESYNC-no-wrap -------------------------------------------------------
    // A corrupted connection: toSendConsumed > toSend.size(). The cap tests
    //   pending = outputBuffer.size() + pendingBytes(connection)
    // We park the client's own outputBuffer near the cap (B = cap - 2 KiB) and set a
    // desync deficit D == B, then require the near-cap client to still overflow.
    //
    // pendingBytes reports the whole retained buffer and does not subtract,
    // because the bytes written-but-unacknowledged are held for retransmission
    // and are memory too. A pendingBytes returning (toSend.size() -
    // toSendConsumed) unguarded would wrap to SIZE_MAX - D + 1, the sum would
    // wrap back under the cap, and the frame would slip past; this case guards
    // against that subtraction being reintroduced.
    {
        Ubiquitty::Host::QUICConnection* connection = makeConnection();
        check(connection != nullptr, "(b) test QUIC connection allocates");
        const std::size_t B = clientBufferHighWater - 2048; // client's own buffered bytes, near cap
        const std::size_t D = B; // desync deficit chosen so an unguarded underflow wraps the sum to 0
        // toSend.size()=0, toSendConsumed=D → unguarded pendingBytes underflows by D.
        Ubiquitty::Host::setTestQUICConnectionPending(connection, /*toSendSize*/ 0, /*toSendConsumed*/ D);

        std::size_t idx = QUICOutputCapacityAccess::addQUICClient(d, connection);
        QUICOutputCapacityAccess::setOutputBufferSize(d, idx, B); // pending = B + pendingBytes()

        std::vector<std::uint8_t> frame(4096, 0xCD); // B + 4KiB > cap, but B - D + 4KiB << cap
        QUICOutputCapacityAccess::queueToClient(d, idx, frame);

        check(QUICOutputCapacityAccess::didOverflow(d, idx),
            "(b) a desync (toSendConsumed > toSend.size()) does NOT underflow-wrap "
            "outputBuffer.size()+pendingBytes below clientBufferHighWater — the "
            "near-cap client still OVERFLOWS");
    }

    // --- (c) a backlogged QUIC client is queued no screen frame, not run at the cap
    // A client behind by more than one frame is owed its screen, not fed it:
    // the frames wait in its screen connection and it keeps its connection. A
    // QUIC client's outputBuffer is emptied into the connection on every flush,
    // so a measure that reads only that buffer sees this client as keeping up
    // and queues to it — and it sails past one frame to the hard cap. The low
    // water and the cap read one measure, so the client lands softly here.
    {
        Ubiquitty::Host::QUICConnection* connection = makeConnection();
        check(connection != nullptr, "(c) test QUIC connection allocates");
        const std::size_t backlogged = QUICOutputCapacityAccess::lowWater() + 64u * 1024u;
        check(backlogged < clientBufferHighWater, "(c) the staged backlog sits under the drop cap");
        Ubiquitty::Host::setTestQUICConnectionPending(connection, /*toSendSize*/ backlogged, /*toSendConsumed*/ 0);

        const std::size_t idx = QUICOutputCapacityAccess::addQUICClient(d, connection);
        if (!QUICOutputCapacityAccess::subscribe(d, idx, session, tokenPath, token, backlogged)) {
            check(false, "(c) the exact credential and prior grant admitted the pane");
            return 1;
        }
        // The handshake's answer and the pane's binding are interactive frames
        // the connection admitted when it queued them, and they leave at once
        // whatever the backlog; a screen frame is what the low water holds.
        const std::size_t handshakeBytes = QUICOutputCapacityAccess::queued(d, idx, backlogged).size();
        check(handshakeBytes > 0 && handshakeBytes < QUICOutputCapacityAccess::lowWater(),
            "(c) the subscription stages its Welcome and PaneBind, and nothing the size of a frame");

        QUICOutputCapacityAccess::publishAndService(d, idx, session);
        const std::vector<std::uint8_t> staged = QUICOutputCapacityAccess::queued(d, idx, backlogged);
        bool isScreenQueued = false;
        Sync::FrameDecoder decoder;
        if (staged.size() < handshakeBytes) {
            check(false, "(c) the admitted handshake remained queued");
            return 1;
        }
        decoder.append(std::span(staged).subspan(handshakeBytes));
        while (const auto frame = decoder.next()) {
            isScreenQueued = isScreenQueued || frame->kind == Sync::FrameKind::CheckpointHead
                || frame->kind == Sync::FrameKind::Update || frame->kind == Sync::FrameKind::BulkChunk;
        }
        check(decoder.error() == Sync::EnvelopeError::None && !decoder.bufferedBytes(), "(c) all staged bytes are complete canonical frames");
        check(!isScreenQueued,
            "(c) a QUIC client backlogged past one frame was queued its screen instead of owed it");
        check(!QUICOutputCapacityAccess::didOverflow(d, idx),
            "(c) a client between the low water and the cap was dropped rather than owed");
    }

    // --- (d) bytes in flight are not a backlog -----------------------------------
    // A QUIC client whose stream bytes have all been handed to ngtcp2 and await
    // acknowledgement is keeping up: the congestion controller paces those bytes
    // and the peer's flow-control window bounds them. Counted against the
    // one-frame low water, they held a QUIC client to one frame per round trip
    // and a 50 ms, 20 Mbit/s link to half its rate. The same client is owed no
    // drop either: the cap is about what waits, and nothing waits.
    {
        Ubiquitty::Host::QUICConnection* connection = makeConnection();
        check(connection != nullptr, "(d) test QUIC connection allocates");
        const std::size_t inFlight = QUICOutputCapacityAccess::lowWater() + 64u * 1024u;
        Ubiquitty::Host::setTestQUICConnectionPending(connection, /*toSendSize*/ inFlight, /*toSendConsumed*/ inFlight);
        check(QUICOutputCapacityAccess::pendingBytes(d, connection) == inFlight, "(d) the bytes in flight are still held");

        const std::size_t idx = QUICOutputCapacityAccess::addQUICClient(d, connection);
        if (!QUICOutputCapacityAccess::subscribe(d, idx, session, tokenPath, token, 0)) {
            check(false, "(d) the exact credential and prior grant admitted the pane");
            return 1;
        }
        const std::size_t handshakeBytes = QUICOutputCapacityAccess::queued(d, idx, 0).size();
        QUICOutputCapacityAccess::publishAndService(d, idx, session);
        const auto staged = QUICOutputCapacityAccess::queued(d, idx, 0);
        check(staged.size() > handshakeBytes,
            "(d) a QUIC client whose only bytes are in flight is queued its screen");
        if (staged.size() < handshakeBytes) {
            check(false, "(d) the admitted handshake remained queued");
            return 1;
        }
        Sync::FrameDecoder decoder;
        decoder.append(std::span(staged).subspan(handshakeBytes));
        bool isScreenQueued = false;
        while (const auto frame = decoder.next()) {
            isScreenQueued = isScreenQueued || frame->kind == Sync::FrameKind::CheckpointHead
                || frame->kind == Sync::FrameKind::Update || frame->kind == Sync::FrameKind::BulkChunk;
        }
        check(decoder.error() == Sync::EnvelopeError::None && !decoder.bufferedBytes() && isScreenQueued,
            "(d) the queued canonical suffix contains screen state, not only metadata");
        check(!QUICOutputCapacityAccess::didOverflow(d, idx), "(d) a client with bytes in flight and none waiting is not dropped");
    }

    if (failures == 0)
        std::printf("quic_output_capacity: a stalled QUIC reader's queued backlog is counted toward the "
                    "per-client high-water cap; excessive consumption does not wrap that cap, "
                    "datagrams stay bounded, and screen pacing distinguishes unsent from in-flight bytes\n");
    return failures == 0 ? 0 : 1;
}
