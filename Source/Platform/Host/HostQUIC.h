// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The daemon's QUIC listener (ngtcp2 + AWS-LC). Built only with
// -DUBIQUITTY_QUIC=ON. QUIC is the remote data-plane: SSH bootstraps the daemon +
// hands off a quic_port + one-time token, then the client dials the QUIC listener
// directly so wifi↔cellular/sleep survive via connection migration, with fallback
// to ssh-stdio.
//
// ngtcp2 is a you-drive-it state machine (no foreign threads): the listener owns a
// non-blocking UDP socket the daemon's poll() loop watches and drives each
// ngtcp2_conn from that one thread — preserving the lock-free single-thread reactor.

#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

// AWS-LC's SSL_CTX, opaque here so this header pulls in no QUIC/TLS deps.
typedef struct ssl_ctx_st SSL_CTX;

namespace Ubiquitty {
namespace Host {

struct QUICConnection; // an accepted ngtcp2 server connection (HostQUIC.cpp)

// Largest Sync envelope the daemon will ship as a single QUIC DATAGRAM: kept
// comfortably under a conservative QUIC path MTU so the datagram always rides
// one packet (a DATAGRAM frame can't be fragmented). A superset update this
// size or smaller goes unreliable (no head-of-line blocking); anything larger,
// and every checkpoint, takes the reliable stream.
constexpr std::size_t quicDatagramMaxBytes = 1100;

// How many bytes of unsent DATAGRAMs one connection may hold before the oldest
// are dropped to make room.
//
// A datagram that cannot go out now stays queued for the next pump, and a peer
// that stops reading pumps nothing: without a ceiling the queue grows by one
// delta per frame for as long as the peer cares to stall, on a path whose whole
// point is that losing a frame is survivable. The reliable stream has its own
// high-water and drops the client past it; this is the unreliable half of the
// same policy, and it drops frames rather than the client.
//
// The oldest go first, and that is the better half to lose: a client behind by
// more than this is one whose screen costs less to re-send as a keyframe than to
// rebuild from hundreds of stale deltas, and the seq gap the drop opens is
// exactly what asks for that keyframe. At this size the queue holds a couple of
// hundred deltas, which is far more slack than a client behind by a moment needs.
constexpr std::size_t quicDatagramQueueHighWaterBytes = 256 * 1024;

// Every received UDP packet spends one slot, including empty and malformed
// packets, so a network flood yields to the daemon's other clients and timers.
constexpr std::size_t quicReceivePacketBudget = 64;

// Version string of the linked QUIC stack (e.g. "ngtcp2 1.23.0"). Proves the
// ngtcp2/AWS-LC link is wired; surfaced via `ubiquitty-host --quic-version`.
std::string quicStackVersion();

/// An application CONNECTION_CLOSE: the error code and reason phrase a QUIC
/// peer reads ahead of anything its stream still holds.
struct QUICApplicationClose {
    std::uint64_t errorCode = 0;
    /// What the peer may show; at most `maxReasonPhraseBytes` of it is sent.
    std::string_view reasonPhrase;

    /// The longest reason phrase sent: the close is one packet.
    static constexpr std::size_t maxReasonPhraseBytes = 256;
};

// The daemon's QUIC listener: a UDP socket + a AWS-LC server TLS context the
// ngtcp2 server connections share. Single-threaded — created, polled, and torn
// down on the daemon's main loop.
class QUICListener {
public:
    QUICListener();
    ~QUICListener();

    QUICListener(const QUICListener&) = delete;
    QUICListener& operator=(const QUICListener&) = delete;

    // Ensure a self-signed cert/key exist (generated once under ~/.ubiquitty),
    // build the AWS-LC server context, and bind a non-blocking UDP socket on
    // `port` (0 = an ephemeral port the OS picks; read it back via boundPort).
    // Returns false (with a reason in lastError) on any failure.
    bool start(std::uint16_t port);

    /// Adopts an already-bound UDP socket without changing its address or port.
    ///
    /// The listener must be stopped. The socket must be unconnected IPv4 or
    /// IPv6 UDP with a nonzero bound port. Admission sets nonblocking and
    /// close-on-exec flags and loads the certificate from the control directory.
    /// - Parameter socket: Exclusive socket ownership, consumed even on refusal.
    /// - Returns: Whether the listener owns a ready socket; otherwise `lastError` explains the refusal.
    [[nodiscard]] bool startOnBoundSocket(PAL::UnixFileDescriptor socket);

    /// Relinquishes the bound socket after every QUIC connection is closed.
    ///
    /// The caller must retire clients and send any planned departure before
    /// release. Refusal preserves the listener. Success tears down TLS without
    /// closing the socket, leaving this listener stopped.
    /// - Returns: Exclusive socket ownership, or an empty owner on refusal.
    [[nodiscard]] PAL::UnixFileDescriptor releaseBoundSocket();

    // Close the socket + free the TLS context. Idempotent.
    void stop();

    // The UDP socket fd to add to the daemon's poll() set, or -1 when stopped.
    int fileDescriptor() const { return m_fileDescriptor.get(); }

    // The actual bound UDP port (resolved when port 0 was requested). The SSH
    // bootstrap returns this to the client.
    std::uint16_t boundPort() const { return m_boundPort; }

    // SHA-256 (32 raw bytes) of the daemon's QUIC cert (DER). Returned by the
    // bootstrap so the client pins it; empty if unavailable.
    std::string certificateSHA256() const;

    // Read at most quicReceivePacketBudget datagrams, then pump writes and
    // expiries even when more input remains. Each valid packet is fed to its
    // ngtcp2 connection, creating one for a fresh client initial. Call when
    // poll() reports the fd readable; queued input keeps it readable next turn.
    void processReadable();

    // Sync relay over the QUIC stream. A QUIC connection carries the same framed
    // Sync byte stream as the AF_UNIX path, on its first client bidi stream:
    // inbound frames arrive via the stream-data callback, outbound via sendStream.

    // Install the inbound handler (the daemon feeds these bytes to the
    // connection's Session). Fires on the caller's thread from processReadable.
    void setStreamDataCallback(std::function<void(QUICConnection*, std::span<const std::uint8_t>)> callback);

    /// Installs the handler for a datagram a peer sends: the client's half of
    /// the Sync screen plane's unreliable carrier.
    ///
    /// - Parameter callback: Fires on the caller's thread from
    ///   `processReadable`, with one datagram's payload.
    void setDatagramCallback(std::function<void(QUICConnection*, std::span<const std::uint8_t>)> callback);

    // Queue the `bytes` to `connection`'s relay stream; flushed on the next
    // processReadable pump (or call flush()).
    void sendStream(QUICConnection*, std::span<const std::uint8_t> bytes);

    /// Frees a connection and its queued output when the daemon reaps the
    /// bound Client, so the unbounded toSend queues do not pin memory until the
    /// 90 s idle timeout. Silent: no closed-callback, since the daemon already
    /// owns the drop.
    ///
    /// - Parameters:
    ///   - connection: The connection to free.
    ///   - close: An application CONNECTION_CLOSE to send first: the peer reads
    ///     its code and phrase ahead of whatever the stream still holds.
    ///     Without one the peer learns of the close only by its idle timeout.
    void closeConnection(QUICConnection* connection, std::optional<QUICApplicationClose> close = std::nullopt);

    // Bytes the connection holds: the relay stream's unsent and unacknowledged
    // bytes plus pending datagrams. The memory a stalled reader pins; the daemon's
    // aggregate cap and its session-exit drain read this.
    [[nodiscard]] std::size_t pendingBytes(QUICConnection*) const;

    /// Allocated stream blocks (including the spare) and datagram capacities.
    /// This is memory ownership, not the pending-byte drain/backpressure signal.
    [[nodiscard]] std::size_t storageBytes(QUICConnection*) const;
    /// Additional stream-block capacity needed before copying a carrier frame.
    [[nodiscard]] std::size_t additionalStreamStorageBytes(QUICConnection*, std::size_t bytes) const;

    /// Bytes queued and not yet handed to ngtcp2: the relay stream's unsent bytes
    /// plus pending datagrams.
    ///
    /// The backpressure signal. Bytes in flight are the congestion controller's
    /// to pace and the peer's flow-control window bounds them, so counting them
    /// against a one-frame low water caps a connection at one frame per round
    /// trip.
    ///
    /// - Parameter connection: The connection to read, or null.
    /// - Returns: The queued byte count, zero for null.
    [[nodiscard]] std::size_t unsentBytes(QUICConnection*) const;
    // The daemon calls this once a peer's token verifies. Until then the
    // connection is on a short leash: see preAuthGraceMilliseconds.
    void markAuthenticated(QUICConnection*);

    /// Exports keying material from a connection's TLS handshake (RFC 8446
    /// §7.5), with no context: bytes only the two ends of this handshake can
    /// derive, which bind what a peer presents to this connection.
    ///
    /// - Parameters:
    ///   - connection: The connection, or null.
    ///   - label: The exporter label.
    /// - Returns: 32 bytes, or `nullopt` for null, a connection with no TLS
    ///   session, or a handshake that exports none.
    [[nodiscard]] std::optional<std::array<std::uint8_t, 32>> exportKeyingMaterial(QUICConnection* connection, std::string_view label) const;

    // True once the peer (client) advertised DATAGRAM support (RFC 9221) in its
    // handshake transport params — i.e. sendDatagram to it will be delivered.
    [[nodiscard]] bool datagramCapable(QUICConnection*) const;

    // Queue one complete Sync frame as an unreliable QUIC DATAGRAM. `bytes` must
    // fit quicDatagramMaxBytes (one packet); larger frames go via sendStream. A
    // lost datagram never head-of-line-blocks the stream; the next Sync
    // superset update covers what it carried.
    void sendDatagram(QUICConnection*, std::span<const std::uint8_t> bytes);

    // Flush all connections' pending outbound stream bytes + protocol packets.
    // Call after queueing with sendStream outside the read loop.
    void flush();

    // Smallest ngtcp2 expiry across all connections as a relative poll timeout
    // in milliseconds (0 = already due, -1 = no timer), capped at 1 s. The
    // daemon folds this into its poll() timeout so loss/PTO/keepalive/idle timers
    // fire on schedule even when no datagram arrives to wake the loop.
    int nextTimeoutMilliseconds() const;

    // Run ngtcp2_conn_handle_expiry on every connection whose timer is due, and
    // reap any that report a fatal expiry (e.g. idle timeout). Call each loop
    // iteration before flush(); flush() then sends any timer-driven packets
    // (retransmits, PING, PATH_CHALLENGE).
    void serviceTimers();

    // Install the connection-closed handler: fires when a connection is
    // removed (peer close / draining / a fatal read error), so the daemon
    // drops the Client bound to it. Fires from processReadable.
    void setConnectionClosedCallback(std::function<void(QUICConnection* connection)> callback);

    // Called by a connection's recv_stream_data callback — forwards to the
    // installed stream-data handler. (Public only so the C callback reaches it.)
    void deliverStreamData(QUICConnection*, std::span<const std::uint8_t> bytes);

    /// Forwards a datagram a connection received to the installed handler.
    ///
    /// Public only so the C callback reaches it.
    ///
    /// - Parameters:
    ///   - connection: The connection it arrived on.
    ///   - bytes: Its payload.
    void deliverDatagram(QUICConnection* connection, std::span<const std::uint8_t> bytes);

    const std::string& lastError() const { return m_lastError; }

    /// The UDP packets the listener has handed its socket, across every
    /// connection, for a test that counts what one publish cost.
    [[nodiscard]] std::uint64_t packetsSent() const { return m_packetsSent; }

private:
    /// Sends a connection's terminal CONNECTION_CLOSE packet, carrying an
    /// application error code and reason phrase.
    ///
    /// - Parameters:
    ///   - connection: A connection whose handshake completed.
    ///   - close: What the packet says.
    void sendApplicationClose(QUICConnection& connection, const QUICApplicationClose& close);
    bool ensureCertificate(std::string& certPath, std::string& keyPath);
    bool buildTLSContext(const std::string& certPath, const std::string& keyPath);

    PAL::UnixFileDescriptor m_fileDescriptor;
    std::uint16_t m_boundPort = 0;
    std::string m_certPath; // the daemon's QUIC cert PEM (for pin fingerprint)
    /* owned via SSL_CTX_free */ SSL_CTX* m_tlsContext = nullptr;
    std::string m_lastError;
    std::uint64_t m_packetsSent = 0;
    std::vector<std::unique_ptr<QUICConnection>> m_connections;
    sockaddr_storage m_localAddr {};
    socklen_t m_localAddrLength = 0;
    // Retry-token signing key: random for this listener's lifetime.
    // No ngtcp2/TLS state is allocated for a fresh Initial until its Retry token
    // verifies against this secret and the packet's claimed source address — a
    // spoofed source never receives our Retry and can't echo a valid token back.
    std::uint8_t m_retryTokenSecret[32] {};
    std::function<void(QUICConnection*, std::span<const std::uint8_t>)> m_streamDataCallback;
    std::function<void(QUICConnection*)> m_connectionClosedCallback;
    std::function<void(QUICConnection*, std::span<const std::uint8_t>)> m_datagramCallback;

    // Test-only: reach m_connections to prove reapClient frees a bound connection
    // immediately. Not on any production path.
    friend QUICConnection* adoptTestQUICConnection(QUICListener&, QUICConnection*);
    friend std::size_t testQUICConnectionCount(const QUICListener&);
    friend void setTestQUICClockSource(QUICListener&, std::function<std::uint64_t()>);

    // Test-only: the clock every connection accepted from here on reads instead
    // of the QUIC clock. Empty on every production path.
    std::function<std::uint64_t()> m_clockSourceForTesting;
};

/// Test-support: makes every connection the listener accepts from here on read
/// `source` instead of the QUIC clock.
///
/// Drives the timestamp clamp with a clock that steps backwards.
///
/// - Parameters:
///   - listener: The listener whose future connections read it.
///   - source: Nanosecond readings; an empty one restores the QUIC clock.
void setTestQUICClockSource(QUICListener&, std::function<std::uint64_t()>);

// Test-support: allocate/free a bare QUICConnection (no live handshake) so a test
// can give a Client a real, non-null quicConnection — the routing key marking it a
// NETWORK peer. Null handles + empty buffers, so sendStream/destroy are safe.
QUICConnection* makeTestQUICConnection();
void destroyTestQUICConnection(QUICConnection*);
// The Sync bytes the daemon queued onto this connection's relay stream — lets a
// test decode the grant a QUIC peer received without a live handshake.
std::vector<std::uint8_t> testQUICConnectionPendingBytes(const QUICConnection*);
// Stage a connection's backlog directly (toSendSize bytes appended, the first
// toSendConsumed of them marked handed to ngtcp2) so a test can stage what
// pendingBytes() reports: a near-cap backlog, or a desync (toSendConsumed >
// toSendSize) the buffer must clamp rather than underflow.
void setTestQUICConnectionPending(QUICConnection*, std::size_t toSendSize, std::size_t toSendConsumed);
/// Test-support: how many DATAGRAMs the connection still holds.
///
/// A byte count alone cannot tell a bounded queue from an emptied one, and the
/// bound is meant to drop frames rather than the connection.
///
/// - Parameter connection: The connection to read, or null.
/// - Returns: The queued datagram count, zero for null.
std::size_t testQUICQueuedDatagramCount(const QUICConnection*);

/// Test-support: the first byte of the DATAGRAM at the head of the queue.
///
/// Which end the bound gave up is the half a count cannot answer, so a test tags
/// its frames by that byte and reads back the one that survived at the head.
///
/// - Parameter connection: The connection to read, or null.
/// - Returns: The head frame's first byte, or -1 when there is no such byte.
int testQUICFrontDatagramTag(const QUICConnection*);
// Drive the acknowledgement path without a peer: report [offset, offset+datalen)
// acked on the connection's relay stream, and read back what the send buffer
// still retains. ngtcp2 retransmits from that buffer, so what it holds after a
// write — and only releases on an ack — is the property these expose.
void testAckStreamData(QUICConnection*, std::uint64_t offset, std::uint64_t datalen);
[[nodiscard]] std::size_t testRetainedSendBytes(const QUICConnection*);
// Drive acceptServerConnection's CID clamp with a synthetic version-cid whose
// scidlen/dcidlen exceed NGTCP2_MAX_CIDLEN. True when the clamp rejects; an
// unclamped build overflows the 20-byte ngtcp2_cid.data in the memcpy.
bool testAcceptServerConnectionRejectsCID(std::size_t scidlen, std::size_t dcidlen);
// Drives the pre-auth deadline without a peer or a wall-clock wait: stages a
// connection as handshake-complete `ageMilliseconds` ago, optionally already
// authenticated, runs the reap the I/O loop runs, and reports whether it
// survived. True means still connected.
[[nodiscard]] bool testPreAuthReapSurvives(std::uint64_t ageMilliseconds, bool authenticated);
// Move a caller-allocated bare connection into the listener's live table (so
// closeConnection can find + erase it) and return the borrowed routing pointer —
// what makes reapClient's immediate free observable.
QUICConnection* adoptTestQUICConnection(QUICListener&, QUICConnection*);
// Live entries in the listener's connection table — the before/after probe
// distinguishing the immediate reap-free from an idle linger.
std::size_t testQUICConnectionCount(const QUICListener&);

} // namespace Host
} // namespace Ubiquitty
