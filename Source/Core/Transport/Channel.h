// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The portable transport seam under a remote session. A Channel is a bidirectional
// OPAQUE byte pipe to a ubiquitty-host daemon — it knows nothing of the Sync
// protocol (framed above it by RemoteSessionCore). Backends: SubprocessChannel
// (ssh/local stdio), LocalSocketChannel (AF_UNIX), QUICChannel.
//
// SELF-DRIVING: start() owns the I/O (poll thread or ngtcp2's thread), so a
// frontend integrates no socket/timer into its runloop. No AppKit/GCD/Cocoa.

#include "Core/Transport/Origin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Ubiquitty::Transport {

// Per-channel link state. The SESSION-level Reconnecting/backoff is not here:
// RemoteSessionCore drives reconnect by creating a fresh Channel per attempt,
// so a single Channel only ever moves Idle → Connecting → Connected →
// Closed/Failed (Closed = clean stop()/peer EOF; Failed = setup/I-O error).
// Pinned because Session::Health spells three of these names while meaning a
// different machine. The dangerous part is that it agrees on two and disagrees
// on one: Connecting is 1 in both and Failed is 4 in both, but `Connected` is 0
// there and 2 here. A conversion written later and tested with Connecting or
// Failed passes — and Connected is the steady state, so the wrong answer is the
// one that holds almost all of the time.
enum class Health : std::uint8_t {
    Idle = 0, // constructed, start() not called yet
    Connecting = 1, // start() in flight; awaiting onConnect / onClose
    Connected = 2, // carrier up; bytes flow
    Closed = 3, // stopped cleanly, or the peer closed
    Failed = 4, // a setup or I/O error tore it down
};

// Live transport-level statistics for the Sync Debug window. Most carriers leave
// `valid` false; a QUIC carrier fills it from ngtcp2's connection info.
struct TransportStats {
    bool valid = false; // false unless the carrier populated it
    // Round-trip time, microseconds; -1 when not yet sampled.
    int64_t smoothedRttMicros = -1;
    int64_t latestRttMicros = -1;
    int64_t minRttMicros = -1;
    int64_t rttVarMicros = -1;
    // Congestion control.
    uint64_t congestionWindow = 0; // bytes
    uint64_t bytesInFlight = 0; // sent but unacknowledged
    uint64_t slowStartThreshold = 0; // bytes
    // Packet / byte counters since the connection opened.
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsLost = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t bytesLost = 0;
    // How many times the carrier moved its connection to a fresh path.
    uint64_t migrations = 0;
};

// Channel events. All fire on the channel's I/O thread, never re-entering the
// channel; the owner marshals to its UI thread as needed. No callback fires
// after stop() returns.
class ChannelObserver {
public:
    virtual ~ChannelObserver() = default;

    // The carrier is up and ready for the first outbound bytes (the HELLO).
    // For a migration-capable backend (QUIC) this fires once per logical
    // connection, not per network change.
    virtual void onConnect() = 0;

    /// Delivers inbound bytes: one slice of the Sync stream, at no framing
    /// boundary in particular.
    ///
    /// - Parameter data: Valid only for the duration of the call. Copy what
    ///   outlives it.
    virtual void onData(std::span<const std::uint8_t> data) = 0;

    /// Delivers one complete Sync frame that arrived as an unreliable datagram.
    ///
    /// Only a QUIC carrier fires this, and only for a delta small enough to
    /// fit one datagram. Unlike `onData` the boundary is exact: what arrives
    /// is a whole frame or nothing, and nothing is what a lost or reordered
    /// datagram becomes — the sender does not repeat it on the reliable
    /// stream, so the gap is recovered through the sequence-gap keyframe path.
    /// The default no-op therefore costs an observer deltas, not correctness.
    ///
    /// - Parameter data: Valid only for the duration of the call.
    virtual void onDatagram(std::span<const std::uint8_t> /*data*/) { }

    // The carrier closed or failed; no further callbacks follow. `reason` is
    // a short human note for status UI. Not fired after a caller stop().
    virtual void onClose(const std::string& reason) = 0;
};

// An open (or opening) byte carrier. Own it behind a pointer in the adapter.
// Not copyable. The virtual destructor stop()s if still running (RAII).
class Channel {
public:
    virtual ~Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// Begins connecting, delivering events to the observer.
    ///
    /// The observer is borrowed and must outlive the channel's running span.
    /// Idempotent: a second call while already started is a no-op that returns
    /// true. Whether the connect itself succeeds is not this call's answer —
    /// that arrives asynchronously through `onConnect` or `onClose`.
    ///
    /// - Returns: `false` only for an immediate local setup error, in which
    ///   case no observer callback ever fires.
    virtual bool start(ChannelObserver*) = 0;

    /// Identifies the certificate verified by this established carrier.
    ///
    /// - Returns: The verified SHA-256 pin after authentication, or absent for
    ///   an unconnected or non-certificate transport. Cached peer metadata is
    ///   not sufficient to implement this method.
    [[nodiscard]] virtual std::optional<std::array<std::uint8_t, 32>> authenticatedCertificatePin() const { return std::nullopt; }

    /// The peer failed the carrier's trust or credential gate. Read after a
    /// failed start or before disposing a closed carrier; never inferred
    /// from a diagnostic string. This failure must not trigger a downgrade.
    [[nodiscard]] virtual bool peerAuthenticationFailed() const { return false; }

    /// Bounds repeated setup failures until this connection first completes
    /// its protocol handshake. Established-session recovery is unaffected.
    [[nodiscard]] virtual bool boundsInitialSetupFailures() const { return false; }

    /// The application error code the peer closed the carrier with, for a
    /// carrier whose close can carry one (a QUIC CONNECTION_CLOSE). Read after
    /// `onClose`.
    ///
    /// - Returns: The code, or nothing for a carrier or close without one.
    [[nodiscard]] virtual std::optional<std::uint64_t> peerApplicationCloseCode() const { return std::nullopt; }

    /// Queues opaque bytes outbound.
    ///
    /// Thread-safe, and never blocks the caller: an internal non-blocking
    /// writer and a bounded buffer apply backpressure by dropping the channel
    /// if the peer wedges. Bytes queued before `onConnect` are buffered until
    /// the carrier opens; a call after `stop()` is a no-op.
    ///
    /// - Parameter data: Bytes borrowed for the duration of the call. An
    ///   implementation that defers the write copies them first.
    virtual void send(std::span<const std::uint8_t> data) = 0;

    /// Queues one unreliable datagram outbound, beside the stream.
    ///
    /// A datagram may be lost or reordered, and nothing resends it; what it
    /// carries must be worth sending only once. Thread-safe, never blocks.
    ///
    /// - Parameter datagram: One whole payload, borrowed for the call.
    /// - Returns: Whether the carrier took it. A carrier without datagrams —
    ///   any but a QUIC one whose peer accepts them — or one whose queue is
    ///   full answers false, and the caller sends what it says on the stream.
    [[nodiscard]] virtual bool sendDatagram(std::span<const std::uint8_t> /*datagram*/) { return false; }

    // Close cleanly and join the I/O thread. No callback fires after this
    // returns. Idempotent; safe from the destructor.
    virtual void stop() = 0;

    /// Stops taking bytes off the carrier until ``resumeReading``.
    ///
    /// The observer's `onData` returns void and cannot refuse, so a consumer slower than the transport
    /// queues every chunk it is handed and the queue is the only thing that grows. This is how it says
    /// stop: the carrier is left unread, its receive buffer fills, and the peer's own writes block —
    /// backpressure through the kernel rather than through a queue nobody bounds.
    ///
    /// Idempotent, and safe from inside `onData`: an implementation that reads on its own thread
    /// finishes the chunk in flight and takes no more. A channel with nothing to suspend does nothing.
    virtual void suspendReading() { }

    /// Resumes taking bytes off the carrier.
    ///
    /// Idempotent, and safe to call when reading was never suspended.
    virtual void resumeReading() { }

    /// Bytes handed to ``send`` that have not reached the peer yet.
    ///
    /// A caller reporting progress on something it sent reads this: what is
    /// still queued is what has not gone. A channel that cannot say answers
    /// -1, which a caller shows as unmeasured rather than as finished.
    ///
    /// - Returns: The queued byte count, or -1 when the channel cannot say.
    [[nodiscard]] virtual std::int64_t pendingOutboundBytes() const { return -1; }

    /// The most bytes this carrier will hold queued before it refuses the write
    /// or ends the channel.
    ///
    /// Read by a caller about to hand over a large frame, so it can ask whether
    /// the frame fits beside what is already queued. Refusing before the write
    /// is the only graceful answer a framed carrier has: a frame cut short at
    /// the cap desynchronises every frame after it, so PollChannel ends the
    /// channel rather than corrupt the stream. That makes an oversized push a
    /// dropped session instead of a refused paste, and it lands on the fallback
    /// carrier — the one taken when QUIC has already failed.
    ///
    /// A channel that cannot say answers -1, and a caller then writes without
    /// the check rather than refusing work it has no way to size.
    ///
    /// - Returns: The outbound queue capacity in bytes, or -1 when unknown.
    [[nodiscard]] virtual std::int64_t outboundCapacityBytes() const { return -1; }

    /// Returns where this carrier reaches.
    ///
    /// Stamped by whoever created the channel, so a frontend renders a badge
    /// without re-deriving the origin from constructor arguments.
    ///
    /// - Returns: The channel's origin.
    [[nodiscard]] virtual Transport::Origin origin() const = 0;

    // Current link state (lock-free read; advisory for status UI).
    [[nodiscard]] virtual Health health() const = 0;

    // Short carrier description for status + diagnostics ("ssh host", "local
    // socket ~/.ubiquitty/host.sock", "quic host:port").
    [[nodiscard]] virtual std::string description() const = 0;

    // Fill `out` with live transport statistics and return true if the carrier
    // has any to report (only QUIC does today). Default: no stats. Read on the
    // owner's thread; the implementation must be thread-safe vs its I/O thread.
    virtual bool queryStats(TransportStats& /*out*/) const { return false; }

    /// Tells the carrier the system reported its network interface gone. A
    /// carrier that can move between paths moves to a fresh one now and
    /// keeps its connection (QUIC); every other carrier does nothing, and
    /// its owner replaces it if it has gone silent. Safe from the owner's
    /// thread at any time.
    virtual void repath() { }

protected:
    Channel() = default;
};

} // namespace Ubiquitty::Transport
