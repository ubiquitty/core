// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Shared poll-loop base for fd-backed Channels (SubprocessChannel,
// LocalSocketChannel). Owns the I/O thread, poll() loop, outbound queue,
// self-pipe wakeup, and Health bookkeeping. Subclasses only open/close the carrier.

#include "Core/Transport/Channel.h"
#include "PAL/Poll.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Ubiquitty::Transport {

class PollChannel : public Channel {
public:
    ~PollChannel() override;

    bool start(ChannelObserver*) override;
    /// What the I/O loop has yet to write, under the queue's own lock.
    ///
    /// - Returns: The queued byte count; never -1, this channel always knows.
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override;
    [[nodiscard]] std::int64_t outboundCapacityBytes() const override;

    /// Appends the bytes to the pending-out buffer and wakes the I/O loop.
    ///
    /// The buffer is capped: bytes that would carry it past the cap are not
    /// truncated but refused whole, since a partial write corrupts a framed
    /// stream. The I/O loop turns that refusal into `onClose(Failed)`, because
    /// an observer callback may only fire on the I/O thread.
    ///
    /// - Parameter data: Bytes copied into the buffer before the call returns.
    void send(std::span<const std::uint8_t> data) override;
    /// Drops the readable interest from the poll set, so the carrier goes unread.
    ///
    /// The interest and not merely the read: a socket with data waiting is readable forever, so a loop
    /// that keeps asking to be told about it and declines to read is told again on every iteration and
    /// burns a core. Wakes the I/O loop, so the change takes effect before the current wait ends.
    void suspendReading() override;

    /// Restores the readable interest and wakes the I/O loop.
    void resumeReading() override;
    void stop() override;
    /// Returns the origin stamped at construction.
    ///
    /// - Returns: The channel's origin.
    Transport::Origin origin() const override { return m_origin; }
    Health health() const override { return m_health.load(std::memory_order_acquire); }

    /// What a channel does when its unsent queue reaches the cap.
    enum class OverflowPolicy : std::uint8_t {
        /// End the channel. The stream carries frames, and a frame missing bytes is not a
        /// shorter frame — it desynchronises everything after it, so nothing the peer sends
        /// afterwards can be read and the channel has no way to continue.
        CloseChannel,
        /// Refuse the write and keep the channel. The stream carries bytes with no framing
        /// over them, so bytes that never arrive are bytes the peer never sees — a paste
        /// into a program that stopped reading, which a real terminal answers by making the
        /// writer wait. Ending the session instead loses everything the session holds.
        RefuseWrite,
    };

    /// How many bytes ``send`` has refused for want of room.
    ///
    /// Zero on a channel that closes on overflow, since such a channel refuses nothing — it ends.
    /// On one that refuses, this is what the peer will never see, and it is a count rather than a
    /// flag because the interesting question is whether it is growing.
    ///
    /// - Returns: The running total.
    [[nodiscard]] std::uint64_t refusedBytes() const { return m_refusedBytes.load(std::memory_order_acquire); }

protected:
    /// Creates a channel stamped with where it reaches, ending on outbound overflow.
    ///
    /// - Parameter origin: Where this carrier reaches; returned by `origin()`.
    explicit PollChannel(Transport::Origin);

    /// Creates a channel that answers an outbound overflow the way its stream needs.
    ///
    /// - Parameters:
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    ///   - overflowPolicy: What happens when the unsent queue reaches its cap.
    PollChannel(Transport::Origin, OverflowPolicy overflowPolicy);

    /// Opens the carrier on the caller's thread, during `start()` and before the
    /// I/O thread spawns.
    ///
    /// Two streams rather than one because the directions are not always the same
    /// kernel object: a subprocess reads one pipe and writes another, while a PTY
    /// or a socket hands out two owners of a single endpoint. A transport with one
    /// endpoint gives both parameters a stream over it, so the base can close each
    /// exactly once without knowing which case it has.
    ///
    /// - Parameters:
    ///   - readStream: Receives the readable direction on success, untouched on
    ///     failure.
    ///   - writeStream: Receives the writable direction.
    ///   - error: Filled on failure.
    /// - Returns: False with `error` set and both parameters left empty.
    virtual bool openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
        std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error)
        = 0;

    // Release any carrier resources the base does not own (reap a child,
    // close a listener). Called once after the I/O thread has joined. The
    // base has already released both streams.
    virtual void closeTransport() { }

    /// Transfers a diagnostic pipe before the I/O thread starts.
    ///
    /// - Parameter stream: The child's nonblocking stderr pipe.
    void setDiagnosticStream(std::unique_ptr<PAL::PollableByteStream> stream) { m_diagnosticStream = std::move(stream); }

    /// The strong reference a sender holds across its wake.
    ///
    /// Exposed to subclasses so a test can occupy the send-versus-teardown window
    /// directly: hold this reference, run a full stop(), and the waker must still
    /// be live with its descriptor unrecycled.
    [[nodiscard]] std::shared_ptr<PAL::Waker> loadWaker() const;

private:
    void ioLoop();
    /// Drains a bounded batch of stderr and retains the most recent diagnostic bytes.
    void drainDiagnostics();
    /// Writes what is queued, holding `m_outMutex` only around the queue.
    ///
    /// The write loop runs unlocked. `send` is the per-keystroke path for every
    /// SSH and local-socket session, and it contends on the same mutex: holding
    /// it across a multi-megabyte drain makes a keypress wait for the whole
    /// syscall loop, when the lock is only needed for the buffer's indices.
    void flushPending();
    void emitCloseOnce(Health terminal, const std::string& reason);
    void joinAndCleanup();

    // Wake the poll() loop. A sender copies a strong reference to the self-pipe
    // before writing, so no descriptor can be closed — and no number reused by an
    // unrelated open() — between the validity test and the ::write. The reference
    // is copied under m_wakeMutex, which is never held across the ::write nor
    // across the I/O-thread join, so a callback-driven send() cannot deadlock
    // against a concurrent stop().
    void wakeIOLoop();

    Transport::Origin m_origin;
    ChannelObserver* m_observer = nullptr;
    std::thread m_thread;
    std::atomic<Health> m_health { Health::Idle };
    std::atomic<bool> m_stopRequested { false };
    std::atomic<bool> m_closeEmitted { false };
    std::atomic<bool> m_sendOverflow { false }; // set by send() when the queue would exceed the cap
    std::atomic<bool> m_isReadingSuspended { false }; // set by suspendReading(); the poll set drops Readable while it holds
    std::atomic<std::uint64_t> m_refusedBytes { 0 }; // bytes send() declined for want of room
    OverflowPolicy m_overflowPolicy { OverflowPolicy::CloseChannel };
    std::mutex m_lifecycleMutex; // serializes joinAndCleanup across owner + callback stop()
    bool m_cleanedUp = false; // guarded by m_lifecycleMutex

    std::unique_ptr<PAL::PollableByteStream> m_diagnosticStream;
    std::string m_diagnostics;
    std::unique_ptr<PAL::PollableByteStream> m_readStream;
    std::unique_ptr<PAL::PollableByteStream> m_writeStream;
    // The waker, held as one shared resource because senders on any thread
    // race stop()'s teardown for it. Both ends travel together: keeping only the
    // write end alive would leave a sender writing into a pipe whose read end
    // teardown had closed, which raises SIGPIPE and takes the process down. The
    // shared_ptr is the single owner — teardown drops this reference and the waker
    // is destroyed once the last in-flight sender drops its own, so repeated
    // concurrent stop() calls cannot race a reset.
    mutable std::mutex m_wakeMutex;
    std::shared_ptr<PAL::Waker> m_waker;

#if UBIQUITTY_ENABLE_TEST_HOOKS
    // Test-only seam: what the outbound buffer still holds. Retention is not
    // visible from outside — the bytes are queued either way — so the only
    // place the difference between "the backlog" and "every byte ever sent"
    // shows is here.
    friend struct OutboundRetentionAccess;
#endif

    mutable std::mutex m_outMutex;
    // Queued as discrete chunks rather than one buffer. A deque never moves the
    // elements it already holds, so the chunk a flush is writing keeps its
    // address while send() appends behind it — which is what lets the write
    // loop run without the lock. One buffer cannot do that: appending can
    // reallocate it under the in-flight span, and carrying the remainder
    // forward means moving megabytes while holding the mutex a keystroke wants.
    std::deque<std::vector<std::uint8_t>> m_pendingChunks;
    // How far into the front chunk the writes have got.
    std::size_t m_pendingSent = 0;
    // Unwritten bytes across every chunk, which is what the cap bounds. Kept as
    // a running total because the alternative is walking the deque on the
    // per-keystroke path.
    std::size_t m_pendingBytes = 0;
};

#if UBIQUITTY_ENABLE_TEST_HOOKS
/// Test-only accessor for what the outbound buffer retains.
struct OutboundRetentionAccess {
    /// Returns the bytes the outbound buffer is holding.
    ///
    /// - Parameter channel: The channel to inspect.
    /// - Returns: The buffer's size, delivered prefix included.
    static std::size_t retainedBytes(PollChannel& channel);
};
#endif

} // namespace Ubiquitty::Transport
