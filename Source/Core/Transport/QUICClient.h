// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The portable QUIC client transport (ngtcp2 + AWS-LC): the client counterpart of
// the daemon's QUICListener. Dials the data plane, does a TLS 1.3 handshake, opens
// one bidirectional relay stream, and carries an OPAQUE byte stream — it knows
// nothing of the Sync protocol. QUIC buys connection migration (wifi↔cellular/sleep
// survive without reconnect).
//
// SELF-DRIVING: start() spawns an I/O thread owning the UDP socket + ngtcp2
// connection (and its timers), pumped with poll(); callbacks fire on that thread.
// Built only under -DUBIQUITTY_QUIC=ON.

#include "Core/Transport/CarrierCapacity.h"
#include "Core/Transport/Channel.h"
#include "Core/Transport/RepathPolicy.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Ubiquitty::Transport {

/// A one-time bootstrap token, minted over SSH, that a connection presents to
/// attach anew. Empty, the connection presents nothing.
struct BootstrapToken {
    std::string token;
};

/// The byte a token's preamble leads with, ahead of its length byte: the
/// daemon's preamble kind for a bootstrap token.
inline constexpr std::uint8_t bootstrapTokenPreambleKind = 1;

/// A preamble only this connection can compute: it is built once the
/// handshake completes, from keying material the handshake exports (RFC 8446
/// §7.5), which no other connection derives. A resume proof is one
/// (`Sync::resumePreamble`): the secret it proves never travels, and the
/// bytes replay nowhere. The carrier sends what `build` returns and knows
/// nothing of what it means.
struct ExportedPreamble {
    /// The exporter label; the context is empty.
    std::string exporterLabel;
    /// Builds the preamble from 32 exported bytes. Called once, on the I/O
    /// thread; an empty result fails the connection.
    std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t, 32> exporter)> build;
};

/// How a connection authenticates in its stream's preamble.
using QUICCredential = std::variant<BootstrapToken, ExportedPreamble>;

/// The action a QUIC carrier takes on a datagram the socket cannot send.
enum class DatagramSendErrorAction : std::uint8_t {
    /// Keep the packet and send it first once the socket can take it.
    Hold,
    /// Drop the packet and let QUIC's loss recovery resend what it carried.
    Drop,
    /// End the connection.
    Close,
};

/// Reads the system error code of a failed send as the action to take.
///
/// A route that vanishes for a moment is what a hop between networks looks
/// like from the socket: the old interface goes, a tunnel restarts, a packet
/// filter is reloaded. Those lose the packet and keep the connection, because
/// QUIC resends what it carried and its idle timer still ends a path that never
/// comes back. Ending the connection there would tear down the session that
/// migration exists to keep.
///
/// - Parameter errorCode: The `errno` the send failed with.
/// - Returns: The action.
[[nodiscard]] constexpr DatagramSendErrorAction datagramSendErrorAction(int errorCode)
{
    switch (errorCode) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ENOBUFS:
        return DatagramSendErrorAction::Hold;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EADDRNOTAVAIL:
    case ENETDOWN:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
    case EPERM:
        return DatagramSendErrorAction::Drop;
    default:
        return DatagramSendErrorAction::Close;
    }
}

// A single QUIC connection to one daemon. Not copyable. Thread-safety:
// construct/start/stop/send are called from the owner thread; callbacks fire on
// the internal I/O thread. send() is the one cross-thread method (internally
// synchronized). Debug builds assert this contract.
class QUICClient {
public:
    struct Callbacks {
        using ConnectHandler = std::function<void()>;
        using DataCallback = std::function<void(std::span<const std::uint8_t>)>;
        using DatagramCallback = std::function<void(std::span<const std::uint8_t>)>;
        using CloseHandler = std::function<void(Health health, const std::string& reason)>;

        // The handshake completed and the relay stream is open — the carrier
        // is ready for the first outbound bytes (the owner's HELLO).
        ConnectHandler onConnect;
        // Inbound relay bytes (a slice of the Sync stream). The span is
        // valid only for the call; copy what you keep.
        DataCallback onData;
        // One Sync envelope as an unreliable DATAGRAM (a small Update, a Probe or an Echo),
        // delivered separately from onData; the span is valid only for the call.
        DatagramCallback onDatagram;
        // The connection closed or failed; no further callbacks. `health` is Closed
        // (graceful peer close) or Failed (I/O / TLS / timeout). Not fired after a caller stop().
        CloseHandler onClose;
    };

    // Why start() could not bring the carrier up locally (before the I/O thread
    // spawns). A connection-time failure (bad token, TLS, timeout) is not here —
    // that arrives asynchronously as onClose(Failed).
    enum class StartError : std::uint8_t {
        AlreadyStarted, // a second start() while the I/O thread is already running
        MissingCertificatePin, // a credential was supplied without the required cert pin
        WakePipe, // the self-pipe pipe() failed
        TokenTooLong, // the bootstrap token exceeds the preamble's length byte
    };
    // Resolution, socket, TLS and connection setup are not here: they happen on
    // the I/O thread, after this call has already returned, and report through
    // onClose. What remains is what start() can still decide by itself.

    /// The longest bootstrap token the relay preamble can carry.
    ///
    /// The preamble is one length byte followed by the token, so this is what
    /// that byte can count.
    static constexpr std::size_t maxBootstrapTokenBytes = 255;

    QUICClient();
    ~QUICClient();

    /// Whether the completed handshake failed its exact certificate pin.
    [[nodiscard]] bool peerAuthenticationFailed() const;

    /// The application error code the peer's CONNECTION_CLOSE carried.
    ///
    /// Set before `onClose` fires for a graceful close the peer initiated with
    /// an application close, and never for a transport close or a failure.
    ///
    /// - Returns: The code, or nothing.
    [[nodiscard]] std::optional<std::uint64_t> peerApplicationCloseCode() const;

    QUICClient(const QUICClient&) = delete;
    QUICClient& operator=(const QUICClient&) = delete;

    /// Begins connecting to a host, delivering events through `callbacks`.
    ///
    /// The client leads its relay stream with a preamble the daemon validates
    /// before any frame: `[1][length:u8][token]` for a bootstrap token, or
    /// what an `ExportedPreamble` builds on the I/O thread from 32 bytes the
    /// handshake exports. An empty token sends no preamble. The I/O thread is spawned and the call
    /// returns at once; the connection's success or failure arrives through
    /// `onConnect` or `onClose`.
    ///
    /// - Parameters:
    ///   - host: The host to dial.
    ///   - port: Its UDP port.
    ///   - credential: What the connection presents.
    ///   - pinnedCertSHA256: The SHA-256 of the daemon's certificate, required
    ///     with any credential but an empty token.
    ///   - callbacks: Where events are delivered, on the I/O thread.
    /// - Returns: A `StartError` for a failure decided locally, before the
    ///   I/O thread; a second call is `AlreadyStarted`.
    [[nodiscard]] std::expected<void, StartError> start(const std::string& host, std::uint16_t port, QUICCredential credential, const std::string& pinnedCertSHA256, Callbacks);

    // Queue opaque `bytes` to the relay stream. Thread-safe; buffered until the
    // stream opens; no-op after stop(). DoS guard: a single burst over
    // maxOutboundBytes, or a stalled backlog exceeding it every I/O tick (see
    // ioLoop), drops the bytes and becomes onClose(Failed) — no OOM from a wedged peer.
    void send(std::span<const std::uint8_t> bytes);

    /// The datagrams `sendDatagram` holds queued before it refuses one.
    static constexpr std::size_t maxQueuedDatagrams = 64;

    /// Queues one DATAGRAM frame, sent once and never resent. Thread-safe.
    ///
    /// - Parameter datagram: The payload, copied before the call returns.
    /// - Returns: False, queuing nothing, before the handshake completes, when
    ///   the daemon accepts no datagrams or none this large, after `stop()`,
    ///   or with `maxQueuedDatagrams` already queued.
    [[nodiscard]] bool sendDatagram(std::span<const std::uint8_t> datagram);

    // Outbound high-water limit: max unsent bytes (queued or on the relay stream
    // awaiting ngtcp2/peer) before the connection fails closed. Sized to at least
    // one max Sync frame so a legit large paste isn't fail-closed on a draining conn.
    static constexpr std::size_t maxOutboundBytes = 16u * 1024u * 1024u;
    static_assert(maxOutboundBytes >= guaranteedOutboundFrameBytes,
        "a carrier cap below the cross-carrier floor breaks Protocol/Sync's frame guarantee");

    // True when appending `length` to a queue holding `pending` would exceed
    // maxOutboundBytes. Overflow-safe (INT30-C): never forms pending+length.
    static bool outboundWouldOverflow(std::size_t pending, std::size_t length) { return pending >= maxOutboundBytes || length > maxOutboundBytes - pending; }

    /// Tells the carrier the system reported the interface in use gone, so
    /// it moves to a fresh path now and does not wait out the silence that
    /// would tell it the same (`RepathPolicy`). The I/O thread opens a new
    /// UDP socket and migrates the connection to it; the connection is lost
    /// only if that path fails too. Thread-safe; does nothing before the
    /// handshake completes, after `stop()`, or while a move is being judged.
    void repath();

    // Stop the connection and join the I/O thread. No callbacks fire after
    // this returns. Idempotent; safe from the destructor.
    void stop() noexcept;

    // A short description of the carrier ("quic host:port") for status UI.
    [[nodiscard]] std::string description() const;

    // Copy the latest connection statistics (refreshed on the I/O thread).
    // Returns false until the first post-handshake sample. Thread-safe.
    [[nodiscard]] bool stats(TransportStats& out) const;

    // Opaque connection state, forward-declared so the .cpp's ngtcp2 callbacks can
    // name it; definition lives in the .cpp.
    struct Impl;

private:
#if UBIQUITTY_ENABLE_TEST_HOOKS
    // Test-only seam: reads the .cpp-hidden outbound queue to drive the real send() limit.
    friend struct OutboundCapacityAccess;
    friend struct QUICClockAccess;

    // Test-only seam: drives the real ngtcp2 receive/handshake callbacks
    // directly to reproduce the coalesced-packet MITM race deterministically.
    friend struct PinGateAccess;

    // Test-only seam: the ephemeral port the client bound. A datagram can only
    // be aimed at this socket by something that knows it, and the source filter
    // is only observable by aiming one at it from somewhere else.
    friend struct LocalEndpointAccess;
#endif

    std::unique_ptr<Impl> m_impl;

    // The I/O thread body + its two pump halves (defined in the .cpp; no ngtcp2 in signatures).
    void ioLoop();
    // Returns false on a fatal teardown, setting closeHealth to Closed for a
    // peer-initiated draining close or Failed for a TLS/protocol error.
    /// Resolves, opens the socket, and builds the connection.
    ///
    /// Runs as the I/O thread's first act, because resolution blocks for as
    /// long as the resolver takes and the owner thread must not wait on it.
    ///
    /// - Returns: false when the connection could not be established, having
    ///   already reported why.
    bool establishConnection();

    bool readDatagrams(Health& closeHealth);
    bool writePump();
};

#if UBIQUITTY_ENABLE_TEST_HOOKS
// Test-only accessor into the .cpp-hidden outbound queue (befriended above).
/// Test-only accessor for the client's own UDP endpoint.
struct LocalEndpointAccess {
    /// Returns the local port the client bound, in host order.
    ///
    /// - Parameter client: The started client.
    /// - Returns: The port, or 0 before start().
    static std::uint16_t localPort(const QUICClient& client);

    /// Returns how many datagrams were refused for coming from elsewhere.
    ///
    /// - Parameter client: The started client.
    /// - Returns: The count since start.
    static std::uint64_t foreignDatagramsDropped(const QUICClient& client);

    /// Returns how many times the client moved its connection to a fresh
    /// socket.
    ///
    /// - Parameter client: The started client.
    /// - Returns: The count since start.
    static std::uint64_t migrations(const QUICClient& client);

    /// Closes the client's UDP socket under it, as an interface going away
    /// does: every later send on it fails. The I/O thread closes it on its
    /// next pass.
    ///
    /// - Parameter client: The started client.
    static void closeSocketForTest(QUICClient& client);
};

struct OutboundCapacityAccess {
    static std::size_t queuedBytes(const QUICClient&);
    static bool didOverflow(const QUICClient&);

    // Stage the relay-stream backlog (toSend) without a live connection, then run
    // the same budget check ioLoop runs every tick.
    static void stageBacklogBytes(QUICClient&, std::size_t backlogBytes);
    static bool checkBudget(QUICClient&);

    // Install onClose directly (bypassing start()) so a test can observe reportClose
    // idempotency — exactly one onClose no matter how many overflows.
    static void installOnCloseForTest(QUICClient&, QUICClient::Callbacks::CloseHandler);

    // Drive writePump's real cursor drain without a live connection: seed
    // the backlog, read the pending window, advance the REAL cursor, acknowledge.
    // Exposes the buffer's storage to prove ordered delivery, that releasing
    // acknowledged bytes keeps storage bounded by what is retained, that a byte
    // never moves while ngtcp2 may resend it, and that the budget keys on what the
    // buffer is still obliged to hold (unacknowledged bytes), not on what has
    // merely been written.
    static void seedToSend(QUICClient&, std::span<const std::uint8_t> bytes);
    static void stageConsumedBacklog(QUICClient&, std::size_t size, std::size_t consumed);
    /// Copies up to `maxBytes` of the queued bytes from the write cursor.
    ///
    /// - Parameters:
    ///   - client: The client to read.
    ///   - maxBytes: The most bytes to copy.
    /// - Returns: The bytes the next packet would carry, in stream order.
    static std::vector<std::uint8_t> toSendWindow(const QUICClient&, std::size_t maxBytes);
    /// The address of the first queued byte not yet handed to ngtcp2.
    ///
    /// - Parameter client: The client to read.
    /// - Returns: The address, or null when nothing is queued.
    static const std::uint8_t* unsentFrontAddress(const QUICClient&);
    static std::size_t pendingBacklogBytes(const QUICClient&);
    static void advanceToSend(QUICClient&, std::size_t written);
    // Report [0, ackedThrough) acknowledged. ngtcp2 retransmits from bytes it has
    // written, so the acknowledgement is what releases them and this is how a
    // test drives it.
    static void ackThrough(QUICClient&, std::uint64_t ackedThrough);
    static std::size_t retainedBacklogBytes(const QUICClient&);
    /// The memory the send buffer's blocks occupy.
    ///
    /// - Parameter client: The client to read.
    /// - Returns: The bytes allocated, held or spare.
    static std::size_t storageBytes(const QUICClient&);
    /// The size of one send-buffer block.
    ///
    /// - Returns: The block size in bytes.
    static std::size_t blockBytes();
};

/// Test-only accessor: the clock the connection's timestamps come from.
struct QUICClockAccess {
    /// Makes the connection read `source` instead of the QUIC clock.
    ///
    /// Call before start(): the I/O thread reads it from the first packet on.
    ///
    /// - Parameters:
    ///   - client: The client whose connection reads it.
    ///   - source: Nanosecond readings; an empty one restores the QUIC clock.
    static void setSource(QUICClient&, std::function<std::uint64_t()>);
};

// Test-only accessor: drives the real ngtcp2 handshake/receive callbacks
// without a live connection, reproducing the coalesced-packet MITM race.
struct PinGateAccess {
    static void installDataCallbacksForTest(QUICClient&, QUICClient::Callbacks::DataCallback onData, QUICClient::Callbacks::DatagramCallback onDatagram);
    static void setPinnedCertSHA256ForTest(QUICClient&, const std::string& pin);
    static bool isPinVerified(const QUICClient&);

    // Invokes the same callback ngtcp2 calls on handshake completion (where
    // isPinVerified is set). tls is null; an empty pin short-circuits before touching it.
    static void simulateHandshakeCompleted(QUICClient&);
    // Invokes the same callbacks ngtcp2 calls on a 1-RTT STREAM/DATAGRAM decrypt,
    // gated on isPinVerified as production is.
    static void simulateReceiveStreamData(QUICClient&, std::span<const std::uint8_t> bytes);
    static void simulateReceiveDatagram(QUICClient&, std::span<const std::uint8_t> bytes);
};
#endif // UBIQUITTY_ENABLE_TEST_HOOKS

} // namespace Ubiquitty::Transport
