// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel whose I/O belongs to the embedder rather than to a poll loop over a
// file descriptor. The carrier is a C-ABI Driver the frontend supplies, and the
// frontend pushes the inbound direction back through notifyConnect() / deliver()
// / notifyClose(). This is the carrier seam for a frontend that cannot spawn a
// subprocess — iOS has neither fork/exec of arbitrary binaries nor an ssh on
// disk, so SubprocessChannel has no meaning there.
//
// Self-driving like every Channel: the Driver owns whatever thread or event loop
// carries the bytes, so the frontend integrates no socket into its runloop.
//
// Plain function pointers rather than a virtual interface because the Driver is
// implemented across a language seam (Swift), which cannot subclass a C++ class.

#include "Core/Transport/Channel.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace Ubiquitty::Transport {

/// A carrier whose I/O the embedder owns.
///
/// Inverts the usual direction: rather than opening a file descriptor and
/// polling it, this channel calls out to an embedder-supplied Driver and waits
/// to be handed bytes. Everything above it — the Sync client, the Kit facade —
/// sees an ordinary Channel.
class DelegatedChannel final : public Channel {
public:
    /// The embedder's carrier, as plain function pointers.
    ///
    /// Every entry point receives `context`. A null entry point is a no-op,
    /// except `start`, whose absence is a setup failure — a Driver that cannot
    /// be started is not a carrier.
    struct Driver {
        void* context = nullptr;

        // Begin connecting. Answers asynchronously through notifyConnect() or
        // notifyClose(). Returning false is an immediate local setup error,
        // after which no callback may fire.
        bool (*start)(void* context) = nullptr;

        // Queue bytes outbound. Must not block the caller, must buffer whatever
        // arrives before the carrier opens, and must be inert once `stop` has
        // run — a send racing a concurrent stop reaches the Driver either side
        // of it, and the Channel contract makes a post-stop send a no-op.
        void (*send)(void* context, const std::uint8_t* bytes, std::size_t count) = nullptr;

        // Close and quiesce. The embedder must guarantee that no deliver() or
        // notify* call is in flight, or begins, after this returns; that is what
        // makes the Channel contract's "no callback after stop()" hold at the
        // carrier. The stopped gate below is a floor under a Driver that fails
        // to, not a substitute for it.
        void (*stop)(void* context) = nullptr;

        // Propagate a client resize where the carrier has a remote PTY.
        void (*setWindowSize)(void* context, int columns, int rows) = nullptr;

        // Release `context`. Called exactly once, from the destructor.
        void (*release)(void* context) = nullptr;

        // Stop taking bytes off the carrier until resumeReading. This is
        // inbound backpressure's reach into the embedder's transport: a
        // consumer holding too much staged input asks the carrier to pause,
        // and a windowed carrier pauses by not reopening its read window.
        // Left null, the channel inherits the base contract's no-op — and a
        // session over this Driver then buffers a slow consumer without
        // bound, which is the failure the entry point exists to prevent.
        //
        // These trail `release` and new entries trail these: the fixture
        // Drivers are aggregate-initialized, and every neighbour here shares
        // one signature, so a mid-struct insertion rebinds positions with no
        // diagnostic.
        void (*suspendReading)(void* context) = nullptr;

        // Resume taking bytes off the carrier. Null under the same terms.
        void (*resumeReading)(void* context) = nullptr;

        // Bytes queued behind send() that have not reached the peer, or a
        // negative count where the carrier cannot say. Paste progress reads
        // this; null reads as unmeasured, never as finished.
        std::int64_t (*pendingOutboundBytes)(void* context) = nullptr;

        // The hard idle-queue frame capacity, or negative when unmeasured.
        std::int64_t (*outboundCapacityBytes)(void* context) = nullptr;

        // The native credential or pinned-peer gate's actual refusal fact.
        bool (*peerAuthenticationFailed)(void* context) = nullptr;

        // A passive exec that cannot complete its first protocol handshake
        // stops after its initial attempt budget instead of looping forever.
        bool (*boundsInitialSetupFailures)(void* context) = nullptr;
    };

    /// Creates a channel over an embedder-supplied carrier.
    ///
    /// - Parameters:
    ///   - driver: The carrier. Its `context` is owned by the channel and
    ///     released through `driver.release` when the channel is destroyed.
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    ///   - description: Short carrier note for status and diagnostics.
    DelegatedChannel(Driver driver, Transport::Origin origin, std::string description);

    /// Stops the carrier if it is still running, then releases the Driver's
    /// context.
    ~DelegatedChannel() override;

    /// Begins connecting, delivering events to the observer.
    ///
    /// Hands the call straight to the Driver, whose answer arrives through
    /// `notifyConnect` or `notifyClose`. Idempotent: a second call while
    /// already started is a no-op that returns true, and the first observer
    /// keeps the channel.
    ///
    /// - Parameter observer: Borrowed; must outlive the channel's running span.
    /// - Returns: False for an immediate local setup error — a Driver with no
    ///   `start`, a null observer, or a `start` that refused — in which case no
    ///   observer callback ever fires.
    bool start(ChannelObserver* observer) override;

    /// Queues opaque bytes outbound through the Driver.
    ///
    /// A no-op before `start` and after `stop`.
    ///
    /// - Parameter data: Borrowed for the duration of the call; a Driver that
    ///   defers the write copies them first.
    void send(std::span<const std::uint8_t> data) override;

    /// Closes the carrier and quiesces it. Idempotent; safe from the destructor.
    ///
    /// Drops the observer before asking the Driver to stop, so no callback
    /// reaches an observer the caller is free to destroy once this returns.
    void stop() override;

    /// Returns where this carrier reaches, as stamped at construction.
    ///
    /// - Returns: The channel's origin.
    Transport::Origin origin() const override { return m_origin; }

    /// Returns the carrier's current link state.
    ///
    /// - Returns: The channel's health.
    Health health() const override;

    /// Returns the short carrier note for status and diagnostics.
    ///
    /// - Returns: The description given at construction.
    std::string description() const override { return m_description; }

    /// Stops taking bytes off the carrier, through the Driver.
    ///
    /// A no-op for a Driver that left `suspendReading` null — such a carrier
    /// cannot pause, and the consumer's queue is then the only bound.
    void suspendReading() override;

    /// Resumes taking bytes off the carrier, through the Driver.
    void resumeReading() override;

    /// Returns what the Driver still holds behind ``send``.
    ///
    /// - Returns: The queued byte count, or -1 for a Driver that left
    ///   `pendingOutboundBytes` null or answered negative — unmeasured,
    ///   never finished.
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override;

    [[nodiscard]] std::int64_t outboundCapacityBytes() const override;
    [[nodiscard]] bool peerAuthenticationFailed() const override;
    [[nodiscard]] bool boundsInitialSetupFailures() const override;

    /// Propagates a client resize to the carrier's own geometry.
    ///
    /// A no-op for a Driver that left `setWindowSize` null — an exec channel
    /// carrying Sync has no PTY to resize.
    ///
    /// - Parameters:
    ///   - columns: The new width in cells.
    ///   - rows: The new height in cells.
    void setWindowSize(int columns, int rows);

    // ---- Embedder -> channel ----------------------------------------------
    // Safe to call from the Driver's own I/O thread. Each is dropped once the
    // channel has stopped, so a late call from a quiescing carrier is inert
    // rather than a use-after-free of the observer.

    /// Reports that the carrier is up and ready for the first outbound bytes.
    ///
    /// The observer may send from inside this call — the Sync client answers
    /// with its HELLO — so no lock is held across it.
    void notifyConnect();

    /// Delivers inbound bytes.
    ///
    /// - Parameter data: Borrowed for the duration of the call; the observer
    ///   copies whatever outlives it.
    void deliver(std::span<const std::uint8_t> data);

    /// Reports that the carrier finished. No further callback is delivered.
    ///
    /// - Parameters:
    ///   - outcome: `Health::Closed` for a clean stop or peer EOF,
    ///     `Health::Failed` when a setup or I/O error tore the carrier down.
    ///     The carrier is the only party that knows which, and neither can be
    ///     inferred from the prior state: a Connected carrier reaches both.
    ///     Any other value is treated as `Health::Failed`.
    ///   - reason: Short human note for status UI.
    void notifyClose(Health outcome, const std::string& reason);

private:
    // Guards m_observer, m_health and m_hasStopped against the Driver's I/O thread.
    // Released before every observer callback and before every call out to the
    // Driver — the Sync client sends its HELLO from onConnect, so a lock held
    // across a callback deadlocks against send(); and a Driver's stop() joins
    // the very thread that would otherwise be blocked on this mutex.
    mutable std::mutex m_mutex;

    Driver m_driver;
    Transport::Origin m_origin;
    std::string m_description;

    ChannelObserver* m_observer = nullptr;
    Health m_health = Health::Idle;
    bool m_hasStarted = false;
    bool m_hasStopped = false;

    // What makes "no callback after stop() returns" a fact rather than a hope:
    // each embedder->observer callback is counted in flight under the mutex,
    // and stop() waits for the count to drain before returning. The stopped
    // gate alone is a window, not a wall — a deliver that passed the gate and
    // released the mutex is still calling the observer while a concurrent
    // stop() runs to completion, which the Linux scheduler reproduces where
    // the macOS one happened not to.
    //
    // m_callbackThread is the drain's re-entrancy exemption: an observer that
    // calls stop() from inside its own callback must not wait for itself. It
    // is meaningful only while m_callbacksInFlight > 0, and relies on the
    // Driver's own contract that callbacks arrive from one I/O thread.
    mutable std::condition_variable m_callbackDone;
    int m_callbacksInFlight = 0;
    std::thread::id m_callbackThread {};
};

} // namespace Ubiquitty::Transport
