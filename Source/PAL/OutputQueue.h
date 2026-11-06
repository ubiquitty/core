// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

// OutputQueue — ordered, lossless FIFO in front of a sink that may accept only
// part of a write now (a non-blocking fd at its high-water mark). Never drops
// bytes and preserves order across calls: pushes as much as the sink takes and
// queues the rest, flushed when the sink reports room (owner watches POLLOUT).
// A dropped tail would, e.g., lose a bracketed paste's closing ESC[201~ and hang
// the shell. The sink is a plain callback, so the queue has no fd/platform dep.

namespace Ubiquitty::PAL {

// Write up to bytes.size() bytes to the sink. Return bytes accepted
// (0..size); 0 means "full right now, try later". Return <0 for a
// fatal/closed sink — the queue stops and surfaces that value to its caller.
using OutputWriteCallback = int (*)(void* context, std::span<const char> bytes);

class OutputQueue {
public:
    OutputQueue() = default;
    ~OutputQueue();
    OutputQueue(const OutputQueue&) = delete;
    OutputQueue& operator=(const OutputQueue&) = delete;

    // Returned by submit()/drain() when the bounded pending queue is exhausted
    // (sink wedged + sustained input) or a growth allocation fails. Distinct
    // negative sentinel so the owner can tell backpressure-overflow apart from
    // a sink-reported fatal; either way the lossless guarantee is broken and
    // the caller should stop/close rather than continue.
    static constexpr int errQueueFull = -1000;
    /// An owned submission must name a nonzero owner; zero names ordinary writes.
    static constexpr int errInvalidOwner = -1001;

    // Hard cap on undelivered (queued) bytes. A wedged sink plus a giant paste
    // degrades to errQueueFull instead of unbounded growth / OOM-abort.
    static constexpr size_t maxPending = 32u * 1024u * 1024u; // 32 MiB

    /// The maximum adjacent ownership runs retained beside the byte buffer.
    /// Ordinary writes coalesce as owner zero. This fixed metadata needs no
    /// allocator and bounds alternating owners independently of byte size.
    static constexpr std::size_t maxPendingRuns = 128;

    /// One contiguous unwritten range; zero owns ordinary terminal replies.
    struct Run {
        std::uint64_t owner = 0;
        std::size_t bytes = 0;
    };

    /// Exact pending bytes and their ordered ownership, independent of a sink.
    /// Copying charges another bounded byte owner until either copy is retired.
    class Unwritten {
    public:
        Unwritten() = default;
        /// Transfers payload and metadata, leaving an empty source.
        /// - Parameter other: The owner to consume.
        Unwritten(Unwritten&& other) noexcept;
        /// Replaces this owner and leaves the source empty.
        /// - Parameter other: The owner to consume, or this owner unchanged.
        /// - Returns: This owner.
        Unwritten& operator=(Unwritten&&) noexcept;
        Unwritten(const Unwritten&) = delete;
        Unwritten& operator=(const Unwritten&) = delete;

        /// Validates the complete partition before copying any payload.
        /// - Parameters:
        ///   - bytes: At most maxPending bytes, none already delivered.
        ///   - runs: Nonempty lengths summing exactly to bytes.size(), with
        ///     distinct adjacent owners and at most maxPendingRuns entries.
        /// - Returns: An exact owner, or nothing on invalid input or allocation refusal.
        [[nodiscard]] static std::optional<Unwritten> copyOf(std::span<const char> bytes, std::span<const Run> runs);

        /// - Returns: The undelivered payload, borrowed until this owner moves.
        [[nodiscard]] std::span<const char> bytes() const { return { m_data.get(), m_size }; }
        /// - Returns: The exact ordered partition of bytes(), including owner zero.
        [[nodiscard]] std::span<const Run> runs() const { return std::span(m_runs).first(m_runCount); }

    private:
        friend class OutputQueue;
        std::unique_ptr<char[]> m_data;
        std::size_t m_size = 0;
        std::array<Run, maxPendingRuns> m_runs {};
        std::size_t m_runCount = 0;
    };

    /// Copies only the unwritten suffix without draining or changing this queue.
    /// The caller freezes writes and drains through the handover decision and
    /// retires the predecessor only after the successor commits the copy.
    /// - Returns: A bounded independent snapshot, or nothing on allocation refusal.
    [[nodiscard]] std::optional<Unwritten> copyUnwritten() const;

    /// Installs accepted bytes without sending or numbering them again.
    /// The caller preserves the input ledger and reserves carried owner tokens
    /// before minting new ones; queue tokens are not authentication identities.
    /// - Parameter unwritten: Consumed only on success; retained on refusal.
    /// - Returns: False when this queue is not empty; never calls the sink.
    [[nodiscard]] bool restoreUnwritten(Unwritten& unwritten);

    // Append the bytes and push as much as the sink accepts now. Queued bytes
    // (if any) go out first, so order is preserved. Returns <0 (the sink's
    // fatal code, or errQueueFull) on failure, else 0.
    int submit(std::span<const char> bytes, OutputWriteCallback, void* context);

    /// Reserves space for an owned submission before any bytes reach the sink.
    /// The caller serializes reservation and submission with all other writes.
    ///
    /// - Parameters:
    ///   - owner: A nonzero token issued by the owner of this queue, never a
    ///     peer-selected identity. It is not an authorization check.
    ///   - bytes: The largest additional payload to be submitted.
    /// - Returns: Zero, or an error with no pending bytes changed.
    int reserveOwned(std::uint64_t owner, std::size_t bytes);

    /// Submits bytes whose still-queued suffix may be withdrawn by owner.
    /// Capacity and fixed metadata are admitted before calling the sink.
    /// Successful submission means accepted by this queue, not necessarily
    /// delivered. Sink failure can follow a partial delivery, as with submit.
    ///
    /// - Parameters:
    ///   - owner: A nonzero queue-owner-issued token.
    ///   - bytes: The payload.
    ///   - callback: The non-reentrant sink.
    ///   - context: Passed to the sink.
    /// - Returns: Zero, a sink failure, errInvalidOwner, or errQueueFull.
    int submitOwned(std::uint64_t owner, std::span<const char> bytes, OutputWriteCallback callback, void* context);

    /// Removes only this owner's bytes still in user space, preserving every
    /// other run's order. It neither recalls delivered bytes nor revokes the
    /// token: the owner must prevent further submissions itself.
    ///
    /// - Parameter owner: The token; zero cannot cancel ordinary writes.
    /// - Returns: Bytes removed, not the size of any original input event.
    std::size_t discardPending(std::uint64_t owner);

    /// Replaces a withdrawn owner's suffix with trusted delimiter repair.
    /// The repair occupies the first removed run's place, ahead of later
    /// ordinary replies; with no queued suffix it follows existing bytes.
    /// Admission is transactional and does not call the sink.
    /// - Parameters:
    ///   - owner: Nonzero server-issued owner.
    ///   - repair: At most twelve host-generated paste delimiter bytes.
    /// - Returns: False without mutation if capacity or allocation refuses.
    [[nodiscard]] bool repairPending(std::uint64_t owner, std::span<const char> repair);

    /// Counts one owner's currently queued bytes, not its delivery progress.
    ///
    /// - Parameter owner: The token, or zero for ordinary writes.
    /// - Returns: Bytes still queued; absence proves no delivery history.
    [[nodiscard]] std::size_t pendingBytes(std::uint64_t owner) const;

    // Flush queued bytes to the sink (call when it can take more). Returns <0 on
    // sink fatal, else 0. No-op when nothing is queued.
    int drain(OutputWriteCallback, void* context);

    /// Drops what is queued without delivering it.
    ///
    /// The one case where bytes are lost on purpose: input queued ahead of an
    /// interrupt, which the terminal would discard unread on taking it.
    void discardPending()
    {
        m_head = 0;
        m_size = 0;
        m_runCount = 0;
    }

    [[nodiscard]] bool hasPending() const { return m_head < m_size; }
    [[nodiscard]] size_t pendingBytes() const { return m_size - m_head; }

private:
    // Returns false if the bounded queue is exhausted or growth allocation
    // fails (no-throw); the caller turns that into errQueueFull.
    bool append(std::span<const char> bytes, std::uint64_t owner = 0);
    /// Admits byte storage and ownership metadata before a write reaches the sink.
    ///
    /// - Parameters:
    ///   - bytes: The additional payload size.
    ///   - owner: The identity for the appended run, including ordinary owner zero.
    /// - Returns: False if either bound or allocation prevents admission.
    bool reserve(std::size_t bytes, std::uint64_t owner);
    int flush(OutputWriteCallback, void* context);
    /// Retires ownership metadata for a prefix already accepted by the sink.
    ///
    /// - Parameter bytes: The delivered prefix, bounded by the pending byte count.
    void consumeRuns(std::size_t bytes);

    char* m_data = nullptr;
    size_t m_size = 0; // logical end of queued bytes
    size_t m_capacity = 0;
    size_t m_head = 0; // bytes [0,m_head) already delivered (lazily reclaimed)
    std::array<Run, maxPendingRuns> m_runs {};
    std::size_t m_runCount = 0;
};

}
// namespace Ubiquitty::PAL
