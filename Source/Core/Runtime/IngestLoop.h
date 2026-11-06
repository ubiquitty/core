// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Title/WalkPacing.h"
#include "PAL/PAL.h"
#include "PAL/Poll.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>

// Source/Core/Runtime/IngestLoop.h — the sequential poll-and-drain loop, lifted
// out of the Metal arm's bridge so the tuning is written down once rather than
// re-derived by the next frontend. Every decision here was measured there.
//
// The Metal arm is the only consumer today. The GTK arm drives Core straight
// from the PTY on its main loop and has no ingest thread to give this, so the
// flood pipeline does not reach it.
//
// The OS lives behind PAL: the wait is ``PAL::waitForReadiness`` and the
// cross-thread wake is a ``PAL::Waker``, so this header names no descriptor
// and no syscall. What stays with the platform: the thread and its scheduling
// class (the Metal arm pins its ingest queue to user-initiated so the parser
// is not scheduled onto efficiency cores), and the hop from the deliver hook
// to the main thread.

namespace Ubiquitty::Runtime {

// Coalesced-delivery floor. During a flood, drains complete back-to-back and
// a main hop per drain queues observer bookkeeping faster than the display
// consumes it. Microseconds, because the pacing clock is the wrapping
// monotonic counter and an unsigned delta is wrap-safe.
inline constexpr std::uint32_t drainDeliverMinIntervalMicros = 1'000'000 / 240;

// The bounded park used only when the platform has no waker: the loop then
// ticks to notice a stop or a queued write, and this tick is the floor of its
// stop latency.
inline constexpr int ingestFallbackTickMilliseconds = 25;

// --- flood pipeline ----------------------------------------------------------
// A drain pass that ends with the writer still ahead, at its byte budget or at
// a presented pane's frame interval, flips ingest into pipelined mode: a
// dedicated reader thread drains the transport into this ring while the ingest
// thread parses from it, so reads go on while the parser runs; the sequential
// loop stalls the WRITER whenever it parses. Power of two for index masking.
inline constexpr std::size_t floodRingCapacity = 4 * 1024 * 1024;

// Refill/handoff discipline is ASYMMETRIC. The consumer POLLS on a timed nap
// when the ring runs empty (rationale at floodConsumerNapMicros). The reader
// KEEPS a bounded probe band in front of its readiness wait, because the wait
// is a syscall and on a saturated flood the writer rearms almost at once.
// Neither side spins unbounded. A sustained-quiet wait budget still ends the
// reader.
inline constexpr int floodSpinBudget = 4;
inline constexpr std::uint32_t floodProbeMicros = 1;
// A read that returns less than this mid-burst caught the line discipline
// mid-copy, and a reader that re-reads at once takes the next few hundred
// bytes of the same slab; a short wait lets the slab land first. The wait is
// taken only while the parser has at least a bite queued ahead of it: with an
// empty ring the parser is waiting on this very read. The wait pays only on a
// transport whose next read grows once the slab has landed, so a wait whose
// following read comes back no larger than the short read before it refutes
// the premise, and the burst takes no further wait.
inline constexpr int floodShortReadBytes = 4096;
inline constexpr std::uint32_t floodShortReadAccumulateMicros = 50;
inline constexpr std::size_t floodAccumulateBacklogBytes = 64 * 1024;
inline constexpr int floodReaderQuietPollMilliseconds = 10;
inline constexpr int floodReaderQuietPollBudget = 3; // ~30 ms silent: the burst is over

// Bit 63 of each flood ring counter is that side's done flag. On the consumer
// counter, folding it in makes a stop CHANGE the word the reader's ring-full
// atomic wait is parked on, so that wake cannot be lost (a separate exited
// flag checked before the wait races the notify — checked false, flag set +
// notified with no waiter, then parked forever). On the producer counter the
// consumer polls it (timed nap, no futex), but it stays in the counter so one
// acquire load carries the final position and the exit together. Position
// arithmetic masks it; a ring position is a byte count and cannot reach 2^63.
inline constexpr std::uint64_t floodRingDoneBit = 1ull << 63;
inline constexpr std::uint64_t floodRingPositionMask = floodRingDoneBit - 1;

/// The flood ring's two SPSC counters, each on its own 128-byte Apple Silicon
/// cache line, in their own heap block.
///
/// As adjacent members of a larger object they share one line with the ring
/// pointer and neighbours: every reader publish invalidates the line the
/// consumer's loads sit on and every consumer store does the reverse —
/// coherence transfers at multiples of the bite rate on a saturated flood.
/// A member block cannot guarantee this isolation: alignas inside it is
/// relative to an object base that is itself only 16-byte aligned. Heap new
/// honours the over-alignment.
struct FloodRingControl {
    /// The reader's publish position, plus its done bit.
    alignas(128) std::atomic<std::uint64_t> producer { 0 };
    /// The parser's consume position, plus its done bit.
    alignas(128) std::atomic<std::uint64_t> consumer { 0 };
};

// Consumer bite cap: one model-lock hold parses at most this much, so the
// render thread never waits behind a multi-millisecond parse. The pass still
// closes at drainPassCapacityBytes.
inline constexpr std::size_t floodBiteCapacityBytes = 64 * 1024;

// Starved-consumer nap. Mid-flood the parser outruns the pty transport, so the
// ring empties after nearly every bite while the reader republishes each
// kernel read; parking on the counter would spin re-catching each republish.
// The nap batches several reads per wake (fewer bites and model-lock cycles)
// and leaves the reader's publish a bare store — a sleeping consumer holds no
// futex registration. Wake latency is off the critical path (the transport
// bounds the flood and the ring holds ample slack), and stop/burst-end are
// re-read every wake, so worst-case exit lag is one nap. A much shorter nap
// costs more in wake/sleep transitions than the batching returns.
inline constexpr std::uint32_t floodConsumerNapMicros = 200;

/// The sequential ingest loop over one session's transport.
///
/// `run` parks in ``PAL::waitForReadiness`` with the waker joined into the
/// watch set, so the idle loop waits indefinitely — zero wakes per second —
/// and a stop, a posted command or a queued write ends the park immediately
/// through ``raiseWake()`` rather than waiting out a timeout tick. Without a
/// waker the loop falls back to the bounded tick.
///
/// Outcomes are coalesced before delivery: `bytesDrained` accumulates, the
/// event bits survive later passes, and every snapshot field takes the latest
/// pass's value; a bell, an end-of-stream or a pass that drained nothing
/// flushes immediately, so nothing user-visible waits. The deliver hook fires
/// on the loop's thread — a function pointer and a context, because the hop
/// to a main thread is the platform's to make.
///
/// A pass that ends with the writer still ahead — at its byte budget or its
/// frame interval, the flood signature — hands the burst to the pipelined
/// reader and parser when the owner adopted a flood endpoint
/// (``adoptFloodEndpoint``), and re-enters the drain directly otherwise,
/// which parses the same bytes at the sequential rate. The pipeline is the
/// Metal arm's, ported with its measured constants; the reader thread is this
/// loop's own and lives only for the burst.
class IngestLoop {
public:
    /// The delivery hook's shape: one coalesced outcome, on the loop's thread.
    using DeliverHook = void (*)(void* context, const DrainOutcome& outcome, std::uint32_t ingestMicros);

    /// Prepares a loop over a session's runtime and host.
    ///
    /// - Parameters:
    ///   - runtime: The runtime whose stream the loop drains.
    ///   - host: The drain host whose lock the passes bite.
    IngestLoop(TerminalRuntime& runtime, DrainHost& host)
        : m_runtime(runtime)
        , m_host(host)
    {
    }

    IngestLoop(const IngestLoop&) = delete;
    IngestLoop& operator=(const IngestLoop&) = delete;

    /// Installs the delivery hook.
    ///
    /// - Parameters:
    ///   - hook: The hook, or null to deliver nothing.
    ///   - context: Handed back to the hook verbatim.
    void setDeliverHook(DeliverHook hook, void* context)
    {
        m_deliverHook = hook;
        m_deliverContext = context;
    }

    /// The release hook's shape: a held frame the loop let go, on the loop's
    /// thread, outside the model lock.
    using ReleaseHook = void (*)(void* context);

    /// Installs the hook told when the loop releases a frame the coherence
    /// rule held.
    ///
    /// A frame a pass releases reaches the frontend through the delivery that
    /// follows the pass. One released on the rule's clock, or on the way out,
    /// has no pass behind it, and a frontend whose draw found the frame held
    /// hears nothing else: a program that wrote once and waits sends no byte
    /// that would ask again.
    ///
    /// - Parameters:
    ///   - hook: The hook, or null to tell nobody.
    ///   - context: Handed back to the hook verbatim.
    void setReleaseHook(ReleaseHook hook, void* context)
    {
        m_releaseHook = hook;
        m_releaseContext = context;
    }

    /// Sets the minimum interval between coalesced deliveries.
    ///
    /// The floor constant assumed every display consumes at 240 Hz; a 60 or
    /// 120 Hz panel consumes half or a quarter of that, and each delivery is
    /// a main-thread hop. Written by the frontend on display-rate edges (the
    /// same edge that paces the sync-frame publish), read per pass on the
    /// writer thread.
    ///
    /// - Parameter seconds: The interval; clamped to the floor.
    void setDeliverMinInterval(double seconds)
    {
        const double micros = seconds * 1'000'000.0;
        m_deliverMinIntervalMicros.store(
            micros > drainDeliverMinIntervalMicros ? static_cast<std::uint32_t>(micros) : drainDeliverMinIntervalMicros,
            std::memory_order_relaxed);
    }

    /// Creates the waker the watch set parks on.
    ///
    /// Refusal is not fatal: the loop runs on the bounded tick instead, and
    /// says so by returning false rather than by ticking quietly.
    ///
    /// - Returns: Whether a waker exists.
    bool prepare()
    {
        if (!m_waker)
            m_waker = PAL::makeWaker();
        // Handed to the runtime so a kernel process-tree edge can cut the park
        // short and run the walk before the next bounded tick. Shared rather than borrowed: the
        // edge arrives on the watcher's own thread, and a raw pointer to a
        // waker this loop owns would be a lifetime rule that thread has no way
        // to keep. Waking a waker nobody is parked on is harmless.
        m_runtime.setIngestWaker(m_waker);
        return m_waker != nullptr;
    }

    /// Adopts the endpoint the flood reader drains, arming the pipeline.
    ///
    /// An independently owned duplicate of the transport the loop runs over:
    /// the reader reads it raw on its own thread, so it must not share stream
    /// state with the endpoint ``run`` polls, and the bytes still come off
    /// the one kernel queue both descriptors name. Without one, a flood is
    /// covered by repeated sequential passes.
    ///
    /// - Important: Call only while the loop is not running; the reader
    ///   borrows the endpoint unlocked.
    ///
    /// - Parameter endpoint: The reader's endpoint, or null to disarm.
    void adoptFloodEndpoint(std::unique_ptr<PAL::PollableByteStream> endpoint)
    {
        m_floodEndpoint = std::move(endpoint);
    }

    /// Asks a running loop to exit at its next top-of-loop check.
    ///
    /// Pair with ``raiseWake()`` to end an idle park immediately; the caller
    /// joins its own thread.
    void requestStop() { m_stop.store(true, std::memory_order_relaxed); }

    /// Rearms a stopped loop for another ``run``.
    void resetStop() { m_stop.store(false, std::memory_order_relaxed); }

    /// Wakes a parked loop, from any thread. Coalescing: repeated raises
    /// before the loop runs cost one wake.
    void raiseWake()
    {
        if (m_waker)
            m_waker->wake();
    }

    /// How many times the flood consumer went hungry, cumulatively.
    ///
    /// Ring-empty naps mean the flood was bounded by the producer and the
    /// transport rather than by parse; near zero means parse-bound. The
    /// contention scenarios weigh this against the main-thread side, and one
    /// relaxed add per 200 µs nap is what keeping it unconditional costs.
    ///
    /// - Returns: The nap count since construction or the last reset.
    [[nodiscard]] std::uint64_t floodNapCount() const { return m_floodNapCount.load(std::memory_order_relaxed); }

    /// Clears the nap count, for a scenario that brackets its own window.
    void resetFloodNapCount() { m_floodNapCount.store(0, std::memory_order_relaxed); }

    /// How many flood bursts the pipeline has carried, cumulatively.
    ///
    /// The escalation's own record: a scenario that must see the pipeline
    /// engage (a presented flood) or must see it stay out (a throttled pane)
    /// asserts this rather than inferring from nap counts, which are zero for
    /// a parse-bound burst too. One relaxed add per burst is the whole cost.
    ///
    /// - Returns: The burst count since construction.
    [[nodiscard]] std::uint64_t floodBurstCount() const { return m_floodBurstCount.load(std::memory_order_relaxed); }

    /// How many short-read accumulate waits the flood reader has taken,
    /// cumulatively.
    ///
    /// A transport whose reads do not grow after a wait (a fixed read
    /// quantum) takes at most one per burst; a scenario pins that against
    /// ``floodBurstCount()``.
    ///
    /// - Returns: The wait count since construction.
    [[nodiscard]] std::uint64_t floodAccumulateWaitCount() const { return m_floodAccumulateWaitCount.load(std::memory_order_relaxed); }

    /// Runs the loop over the session's transport until stop or end-of-stream.
    ///
    /// Call on the thread that owns ingest; every drain pass runs here, so
    /// the model is written from one thread for the loop's whole life.
    ///
    /// - Parameter transportHandle: The transport's registration handle, owned
    ///   by a pollable endpoint the caller keeps alive for the run.
    void run(PAL::FileDescriptor& transportHandle)
    {
        runPasses(transportHandle);
        // Nothing asks the rule again once the loop is gone, so a frame it
        // still holds would never reach the screen.
        const bool wasFrameHeld = m_runtime.isFrameHeld();
        m_runtime.releaseHeldFrame(m_host);
        if (wasFrameHeld)
            announceRelease();
    }

private:
    /// Drains and delivers until stop or end-of-stream; the body of ``run``.
    ///
    /// - Parameter transportHandle: The transport's registration handle.
    void runPasses(PAL::FileDescriptor& transportHandle)
    {
        bool shouldDrain = true;
        DrainOutcome pendingOutcome;
        bool havePendingOutcome = false;
        std::uint32_t pendingIngestMicros = 0;
        std::uint32_t lastDeliverMicros = PAL::monotonicMicros() - m_deliverMinIntervalMicros.load(std::memory_order_relaxed);
        // Seeded at the loop's start, so the first walk waits out a full
        // interval rather than firing the moment the loop begins.
        std::uint32_t lastTitleWalkMicros = PAL::monotonicMicros();
        std::uint32_t lastDirectorySampleMicros = lastTitleWalkMicros;
        bool hasPendingDirectorySample = false;
        // What the edge-driven walk waits on: the last time the tree MOVED, and
        // the first time an unanswered edge appeared. The settle and defer-cap
        // rules they serve are Core/Title/WalkPacing.h's.
        std::uint32_t lastTreeEdgeCount = 0;
        std::uint32_t lastTreeMoveMicros = PAL::monotonicMicros();
        std::uint32_t firstPendingEdgeMicros = 0;
        // The tree moved when a changed edge count is first read, before the
        // wait or after it: the edge that ends a wait starts the settle it is
        // answered after.
        const auto noteTreeMove = [&](std::uint32_t nowMicros) {
            const std::uint32_t treeEdgeCount = m_runtime.processTreeEdgeCount();
            if (treeEdgeCount == lastTreeEdgeCount)
                return;
            lastTreeEdgeCount = treeEdgeCount;
            lastTreeMoveMicros = nowMicros;
        };
        for (;;) {
            if (m_stop.load(std::memory_order_relaxed))
                return;
            // Apply any commands main posted under the model lock, taken here
            // outside drainAvailable's own acquire (the mutex is
            // non-recursive) so main never blocks on the lock to drive the
            // model. No-op and no lock when the queue is empty.
            const bool didApplyCommand = m_runtime.applyPendingCommands(m_host);
            m_runtime.drainPendingWrite(); // flush queued outbound (paste backpressure)
            const bool didDrain = shouldDrain || didApplyCommand;
            if (didDrain) {
                shouldDrain = false;
                const std::uint32_t passStart = PAL::monotonicMicros();
                const DrainOutcome out = m_runtime.drainAvailable(m_host);
                const std::uint32_t now = PAL::monotonicMicros();
                if (out.bytesDrained > 0) {
                    // A shell builtin has no process-tree edge, but its next
                    // prompt re-arms one coalesced quiet directory sample.
                    lastDirectorySampleMicros = now;
                    hasPendingDirectorySample = true;
                }
                // A queued command can discover new process facts without reading bytes.
                if (out.bytesDrained > 0 || out.bell != Ubiquitty::BellMode::Off || out.reachedEnd
                    || out.titleChanged || out.currentDirectoryChanged || out.processStateChanged || out.fontRequestChanged)
                    coalesce(out, pendingOutcome, havePendingOutcome, pendingIngestMicros, now - passStart, now, lastDeliverMicros);
                if (out.reachedEnd)
                    return;
                if ((out.end == DrainPassEnd::ByteBudget || out.end == DrainPassEnd::TimeBudget) && m_floodEndpoint
                    && m_runtime.drainRidesBurst()) {
                    // A pass that ended with the writer still ahead: a flood.
                    // Hand the burst to the pipelined reader and parser, then
                    // resume this loop — which classifies any end-of-stream or
                    // residue through the Stream the reader never touches.
                    // A presented pane's pass also ends at its frame
                    // interval, and a flood parsed slower than a byte budget
                    // per interval never fills one: electing on the byte
                    // budget alone would keep it on the sequential reads for
                    // the whole burst. Gated on the same presentation/throttle
                    // fold that collapses the sequential spin budgets: the
                    // pipeline's reader keeps a probe band per refill gap,
                    // which is exactly the busy-wait a hidden pane or a
                    // throttled machine must not pay — those floods stay on
                    // the sequential passes below.
                    runFloodPipeline(pendingOutcome, havePendingOutcome, pendingIngestMicros, lastDeliverMicros);
                    shouldDrain = true;
                    continue;
                }
                if (out.bytesDrained > 0) {
                    // More may be waiting — re-drain with no wait. Without a
                    // flood endpoint the capacity-limited pass takes this
                    // branch too, at the sequential rate.
                    shouldDrain = true;
                    continue;
                }
            }
            // Burst over: flush a held outcome once the delivery interval has
            // elapsed. An unconditional flush here defeated the coalescing
            // for any writer that pauses between bursts — every micro-gap
            // became a main hop. A still-held outcome is delivered when the
            // wait times out, so nothing waits longer than the interval plus
            // the shortened timeout.
            // A window size held by the coalescing is applied once its
            // interval has passed, and the wait is cut to land on that moment.
            m_runtime.applyPendingWindowSizeIfDue(PAL::monotonicMicros());
            // A program that took the terminal hears its size again on a
            // schedule of its own, and a quiet one produces no drain to carry
            // it, so the wait is cut to land on the next re-send.
            m_runtime.reassertWindowSizeIfDue(PAL::monotonicMicros());
            int timeoutMilliseconds = m_waker ? -1 : ingestFallbackTickMilliseconds;
            // A frame the coherence rule held for quiet, a bracket or a parser
            // mid-sequence is asked again on the rule's schedule; a program
            // that went quiet sends no byte that would ask for it. The rule's
            // quiet is a zero-timeout poll finding nothing, so bytes that
            // landed since the last pass are drained before it is asked; a
            // drain this turn that reached here read nothing, which is that
            // poll's answer already. A stream held for a transfer agent stays
            // readable with bytes that are not this loop's, so it is not asked.
            if (!didDrain && m_runtime.isFrameHeld() && !m_runtime.isIngestHeld() && isTransportReadableNow(transportHandle)) {
                shouldDrain = true;
                continue;
            }
            // Nothing was drained this turn, so the scrollback store is not
            // being appended to: fold one cold block of it, and wake for the
            // next while any remain. A pass that drained left above.
            if (m_runtime.foldColdScrollback(m_host, scrollbackFoldBlockBudget)
                && (timeoutMilliseconds < 0 || timeoutMilliseconds > scrollbackFoldTickMilliseconds))
                timeoutMilliseconds = scrollbackFoldTickMilliseconds;
            const bool wasFrameHeld = m_runtime.isFrameHeld();
            if (const std::optional<std::uint64_t> untilHeldFrame = m_runtime.publishHeldFrameIfDue(m_host)) {
                // At least a millisecond: a frame due now was asked for above,
                // and a wait of zero would spin on a refusal.
                const int untilHeldFrameMilliseconds = static_cast<int>(std::clamp<std::uint64_t>((*untilHeldFrame + 999) / 1000, 1, ingestFallbackTickMilliseconds));
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilHeldFrameMilliseconds)
                    timeoutMilliseconds = untilHeldFrameMilliseconds;
            }
            // Released by the clock: the frontend refused this frame while it
            // was held, and no delivery follows to tell it otherwise.
            if (wasFrameHeld && !m_runtime.isFrameHeld())
                announceRelease();
            const auto displayIdle = m_runtime.serviceDisplayIdle(m_host);
            if (displayIdle.didChange)
                announceRelease();
            if (displayIdle.remainingTicks) {
                const int remaining = static_cast<int>((*displayIdle.remainingTicks * 1000ull + 59) / 60);
                if (timeoutMilliseconds < 0 || remaining < timeoutMilliseconds)
                    timeoutMilliseconds = remaining;
            }
            if (m_runtime.hasPendingWindowSize()) {
                const int untilWindowSize = static_cast<int>(m_runtime.pendingWindowSizeDueInMillis(PAL::monotonicMicros()));
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilWindowSize)
                    timeoutMilliseconds = untilWindowSize;
            }
            if (const std::optional<std::uint32_t> untilReassert = m_runtime.windowSizeReassertDueInMillis(PAL::monotonicMicros())) {
                const int untilReassertMilliseconds = static_cast<int>(*untilReassert);
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilReassertMilliseconds)
                    timeoutMilliseconds = untilReassertMilliseconds;
            }
            if (havePendingOutcome) {
                const std::uint32_t now = PAL::monotonicMicros();
                const std::uint32_t deliverInterval = m_deliverMinIntervalMicros.load(std::memory_order_relaxed);
                if (now - lastDeliverMicros >= deliverInterval) {
                    deliver(pendingOutcome, pendingIngestMicros);
                    havePendingOutcome = false;
                    pendingIngestMicros = 0;
                    lastDeliverMicros = now;
                } else {
                    // Wake in time to deliver the held outcome if the burst
                    // is over, or sooner for anything else that is due.
                    const std::uint32_t remaining = deliverInterval - (now - lastDeliverMicros);
                    const int untilDeliver = 1 + static_cast<int>(remaining / 1000);
                    if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilDeliver)
                        timeoutMilliseconds = untilDeliver;
                }
            }
            // A held stream is the transfer agent's: its readability is frame
            // bytes waiting for the agent, and a wait that watches it spins —
            // the descriptor stays readable for as long as the user considers
            // the dialog. While held, the transport earns a slot only for a
            // pending write, and the tick bounds how long a release can go
            // unnoticed, since releasing is an atomic store with no waker.
            // An opening hold is the same shape: bytes that wait their turn.
            const bool isTransferHeld = m_runtime.isIngestHeld();
            if (isTransferHeld && (timeoutMilliseconds < 0 || timeoutMilliseconds > ingestFallbackTickMilliseconds))
                timeoutMilliseconds = ingestFallbackTickMilliseconds;
            // Zero wherever the kernel raises edges, which is every platform that
            // has them: there the edge below is the whole mechanism. Non-zero
            // only where nothing announces a tree change, and then this is.
            const std::uint32_t fallbackInterval = m_runtime.walkFallbackMilliseconds();
            const std::uint32_t sinceWalkMillis = (PAL::monotonicMicros() - lastTitleWalkMicros) / 1000;
            // A kernel fork/exec/exit edge is the signal, and the only one. An edge
            // that arrives inside the floor is DEFERRED, never dropped: the wait
            // is cut to what remains of the floor so it fires the moment it may.
            // Dropping it is what left a tab named after the `login` that had
            // already exec'd the shell — those execs land in one burst, so the
            // decisive last one is exactly the edge a drop would discard.
            const std::uint32_t nowEdgeMicros = PAL::monotonicMicros();
            noteTreeMove(nowEdgeMicros);
            const bool hasTreeEdge = m_runtime.hasPendingProcessTreeEdge();
            if (hasTreeEdge && firstPendingEdgeMicros == 0)
                firstPendingEdgeMicros = nowEdgeMicros;
            if (!hasTreeEdge)
                firstPendingEdgeMicros = 0;
            if (hasTreeEdge) {
                // Wake when the tree has been still long enough, or when the
                // cap says a churning tree has waited long enough anyway.
                const std::uint32_t stillMillis = (nowEdgeMicros - lastTreeMoveMicros) / 1000;
                const std::uint32_t pendingMillis = (nowEdgeMicros - firstPendingEdgeMicros) / 1000;
                const int untilSettled = stillMillis >= Title::edgeSettleMilliseconds
                    ? 0
                    : static_cast<int>(Title::edgeSettleMilliseconds - stillMillis);
                const int untilMaximumDefer = pendingMillis >= Title::edgeMaximumDeferMilliseconds
                    ? 0
                    : static_cast<int>(Title::edgeMaximumDeferMilliseconds - pendingMillis);
                const int untilEdge = untilSettled < untilMaximumDefer ? untilSettled : untilMaximumDefer;
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilEdge)
                    timeoutMilliseconds = untilEdge;
            }
            if (fallbackInterval != 0) {
                const int untilWalk = sinceWalkMillis >= fallbackInterval
                    ? 0
                    : static_cast<int>(fallbackInterval - sinceWalkMillis);
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilWalk)
                    timeoutMilliseconds = untilWalk;
            }
            const std::uint32_t directoryInterval = hasPendingDirectorySample ? m_runtime.directorySampleMilliseconds() : 0;
            const std::uint32_t sinceDirectoryMillis = (nowEdgeMicros - lastDirectorySampleMicros) / 1000;
            if (directoryInterval != 0) {
                const int untilDirectory = sinceDirectoryMillis >= directoryInterval
                    ? 0
                    : static_cast<int>(directoryInterval - sinceDirectoryMillis);
                if (timeoutMilliseconds < 0 || timeoutMilliseconds > untilDirectory)
                    timeoutMilliseconds = untilDirectory;
            }
            PAL::Watch watches[2];
            std::size_t watchCount = 0;
            const bool shouldWatchTransport = !isTransferHeld || m_runtime.hasPendingWrite();
            if (shouldWatchTransport) {
                watches[watchCount].handle = &transportHandle;
                watches[watchCount].interest = isTransferHeld
                    ? PAL::Ready::Writable
                    : (m_runtime.hasPendingWrite()
                              ? (PAL::Ready::Readable | PAL::Ready::Writable)
                              : PAL::Ready::Readable);
                ++watchCount;
            }
            const std::size_t wakerSlot = watchCount;
            if (m_waker) {
                watches[watchCount].handle = &m_waker->handle();
                watches[watchCount].interest = PAL::Ready::Readable;
                ++watchCount;
            }
            if (watchCount == 0) {
                // Held, nothing to write, no waker: there is no descriptor
                // whose readiness means anything, and an empty wait returns
                // at once. Sleep the tick out instead, then re-check the hold.
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMilliseconds));
                continue;
            }
            const int readyCount = PAL::waitForReadiness({ watches, watchCount }, timeoutMilliseconds);
            if (m_waker && PAL::contains(watches[wakerSlot].result, PAL::Ready::Readable)) {
                // Clear delivered wakes; the loop top re-checks stop, commands
                // and pending writes right after.
                m_waker->drain();
            }
            if (readyCount < 0) {
                // A failed wait classifies through the drain: end-of-stream
                // reports as such rather than as a status nobody maps.
                const DrainOutcome again = m_runtime.drainAvailable(m_host);
                if (again.reachedEnd) {
                    deliver(again, 0);
                    return;
                }
            }
            if (shouldWatchTransport
                && PAL::contains(watches[0].result,
                    PAL::Ready::Readable | PAL::Ready::Writable | PAL::Ready::HangUp | PAL::Ready::Error | PAL::Ready::Invalid))
                shouldDrain = true;
            // Due, and on this thread: the sample touches the state
            // drainAvailable's own sample touches, with no lock between
            // them. A pass that will drain skips it — the drain samples on
            // its own probe and would walk the table twice for one tick.
            if (!shouldDrain) {
                const std::uint32_t nowMicros = PAL::monotonicMicros();
                const std::uint32_t elapsed = (nowMicros - lastTitleWalkMicros) / 1000;
                // Only a read foreground schedules an edge or backstop. An
                // unread one retains its invalidation until it has a reader.
                // An edge is answered once the tree has held still, or once a
                // tree that will not hold still has made it wait the cap out.
                // Answering each edge instead walked the table ten times a
                // second for a shell loop forking twenty, and named the `sleep`
                // it caught mid-flight in the title. The edge that ended the
                // wait is a move like any other: answered at once, it reads the
                // group of a program that has just exited before its shell
                // takes the terminal back, and no edge follows to read the
                // shell.
                noteTreeMove(nowMicros);
                const std::uint32_t stillMillis = (nowMicros - lastTreeMoveMicros) / 1000;
                const std::uint32_t pendingMillis
                    = firstPendingEdgeMicros == 0 ? 0 : (nowMicros - firstPendingEdgeMicros) / 1000;
                const bool isEdgeDue = m_runtime.hasPendingProcessTreeEdge()
                    && elapsed >= Title::edgeFloorMilliseconds
                    && (stillMillis >= Title::edgeSettleMilliseconds
                        || pendingMillis >= Title::edgeMaximumDeferMilliseconds);
                const bool isDue = isEdgeDue || (fallbackInterval != 0 && elapsed >= fallbackInterval);
                if (isDue) {
                    lastTitleWalkMicros = nowMicros;
                    lastDirectorySampleMicros = nowMicros;
                    hasPendingDirectorySample = false;
                    const DrainOutcome tick = m_runtime.sampleProcessStateOffDrain(m_host);
                    if (tick.processStateChanged || tick.titleChanged || tick.currentDirectoryChanged) {
                        deliver(tick, 0);
                    }
                } else if (directoryInterval != 0
                    && (nowMicros - lastDirectorySampleMicros) / 1000 >= directoryInterval) {
                    lastDirectorySampleMicros = nowMicros;
                    const DrainOutcome tick = m_runtime.sampleCurrentDirectoryOffDrain(m_host);
                    hasPendingDirectorySample = tick.currentDirectoryChanged;
                    if (tick.currentDirectoryChanged)
                        deliver(tick, 0);
                }
            }
        }
    }

private:
    /// Merges one pass outcome into the held pending outcome and delivers
    /// when the throttle allows.
    ///
    /// Merge rule: `bytesDrained` accumulates; `titleChanged`,
    /// `currentDirectoryChanged`, `processStateChanged` and
    /// `fontRequestChanged` are event bits
    /// whose held value must survive later passes that saw no corresponding
    /// write; every snapshot field takes the latest pass's value. A bell or
    /// an end-of-stream flushes immediately, and so does a pass that left
    /// the transfer hold active — the firing pass itself (a preamble that
    /// began in an earlier chunk yields a byteless pass) and the parked
    /// passes after it, so the detection event reaches the frontend's
    /// main-thread answer.
    void coalesce(const DrainOutcome& out, DrainOutcome& pending, bool& havePending,
        std::uint32_t& pendingIngestMicros, std::uint32_t passIngestMicros,
        std::uint32_t now, std::uint32_t& lastDeliverMicros)
    {
        const int accumulatedBytes = havePending ? pending.bytesDrained : 0;
        const bool didTitleChange = havePending && pending.titleChanged;
        const bool didDirectoryChange = havePending && pending.currentDirectoryChanged;
        const bool didProcessStateChange = havePending && pending.processStateChanged;
        const bool didFontRequestChange = havePending && pending.fontRequestChanged;
        pending = out;
        pending.bytesDrained += accumulatedBytes;
        pending.titleChanged |= didTitleChange;
        pending.currentDirectoryChanged |= didDirectoryChange;
        pending.processStateChanged |= didProcessStateChange;
        pending.fontRequestChanged |= didFontRequestChange;
        havePending = true;
        pendingIngestMicros += passIngestMicros;
        if (out.bell != Ubiquitty::BellMode::Off || out.reachedEnd || m_runtime.isFileTransferHoldActive() || now - lastDeliverMicros >= m_deliverMinIntervalMicros.load(std::memory_order_relaxed)) {
            deliver(pending, pendingIngestMicros);
            havePending = false;
            pendingIngestMicros = 0;
            lastDeliverMicros = now;
        }
    }

    /// Hands one coalesced outcome to the hook, on the loop's thread.
    ///
    /// - Parameters:
    ///   - outcome: The merged outcome to deliver.
    ///   - ingestMicros: The accumulated parse time it covers.
    void deliver(const DrainOutcome& outcome, std::uint32_t ingestMicros)
    {
        if (m_deliverHook)
            m_deliverHook(m_deliverContext, outcome, ingestMicros);
    }

    /// Tells the release hook a held frame was let go without a pass.
    void announceRelease()
    {
        if (m_releaseHook)
            m_releaseHook(m_releaseContext);
    }

    /// Returns whether a zero-timeout wait finds the transport readable.
    ///
    /// - Parameter transportHandle: The transport's registration handle.
    /// - Returns: True when bytes, an end or an error are waiting.
    static bool isTransportReadableNow(PAL::FileDescriptor& transportHandle)
    {
        PAL::Watch watch { .handle = &transportHandle, .interest = PAL::Ready::Readable };
        return PAL::waitForReadiness({ &watch, 1 }, 0) > 0
            && PAL::contains(watch.result, PAL::Ready::Readable | PAL::Ready::HangUp | PAL::Ready::Error | PAL::Ready::Invalid);
    }

    /// Spins for one microsecond on the monotonic clock.
    ///
    /// The reader's refill wait only: syscall-free, so it keeps the kernel
    /// tty lock free for the writer while the write rearms.
    static void floodProbeWait() { spinFor(floodProbeMicros); }

    /// Spins after a short read so the slab behind it lands whole before the
    /// next read (``floodShortReadAccumulateMicros``); syscall-free for the
    /// same reason as the probe.
    static void floodAccumulateWait() { spinFor(floodShortReadAccumulateMicros); }

    /// Spins on the monotonic clock.
    ///
    /// - Parameter micros: How long, in microseconds.
    static void spinFor(std::uint32_t micros)
    {
        const std::uint32_t start = PAL::monotonicMicros();
        while (static_cast<std::uint32_t>(PAL::monotonicMicros() - start) < micros) {
        }
    }

    /// Sleeps the starved consumer for one nap (rationale at
    /// ``floodConsumerNapMicros``).
    static void floodConsumerNap()
    {
        std::this_thread::sleep_for(std::chrono::microseconds(floodConsumerNapMicros));
    }

    /// Parses one ring bite under the model lock, re-taken per lock bite.
    ///
    /// The ring bite stays 64 KB (its accounting and the pass cadence key off
    /// it); only the model lock is re-taken per ``effectiveModelLockBite``,
    /// so a main-thread reader gets a gap it can actually land in.
    ///
    /// - Parameters:
    ///   - readIndex: Where in the ring the bite starts.
    ///   - biteBytes: How much this bite parses.
    ///   - parsedBytes: How much of the bite actually entered the model.
    void parseFloodBite(std::size_t readIndex, int biteBytes, int& parsedBytes)
    {
        // The tap rides every ingest path: a ZMODEM handshake that lands
        // inside a pipelined burst must reach the detector exactly as one
        // inside a sequential pass does. Off the lock, matching drainAvailable.
        const int safePrefix = m_runtime.tapRawBytes({ m_floodRing.get() + readIndex, static_cast<std::size_t>(biteBytes) });
        const int parseEnd = (safePrefix >= 0) ? safePrefix : biteBytes;
        parsedBytes = parseEnd;
        const bool isPresentationPaced = m_host.drainPassIntervalMicroseconds() != 0;
        int offset = 0;
        while (offset < parseEnd) {
            const int lockEnd = std::min(parseEnd, offset + TerminalRuntime::effectiveModelLockBite());
            m_host.lockModel();
            while (offset < lockEnd) {
                if (isPresentationPaced)
                    m_runtime.noteDrainInput();
                const FeedResult fed = m_runtime.feedChunkUpToSyncClear({ m_floodRing.get() + readIndex + offset, static_cast<std::size_t>(lockEnd - offset) });
                offset += (fed.consumed > 0) ? fed.consumed : (lockEnd - offset); // guard: never spin
                if (fed.reachedSyncClear)
                    m_runtime.publishMidReadBoundaryIfCoherent(m_host);
            }
            m_host.unlockModel();
        }
    }

    /// Drains the transport into the ring until quiet, end or stop.
    ///
    /// Runs on the burst's reader thread, against the adopted endpoint —
    /// never the Stream, whose state is the ingest thread's alone. An
    /// end-of-stream or a fatal status ends the reader; the sequential pass
    /// that follows the pipeline re-reads through the Stream, which
    /// classifies and latches the end state.
    void floodReaderLoop()
    {
        char* ring = m_floodRing.get();
        // Hoisted: the transport's cadence is a property of the endpoint, not
        // of any one gap. A wait-first endpoint parks on the first empty read
        // — each probe against it is a read() syscall reporting the same
        // absence, five per gap where one wait serves.
        const bool isWaitFirst = m_floodEndpoint->readinessWaitStrategy() == PAL::ReadinessWaitStrategy::WaitFirst;
        int quietProbes = 0;
        int quietWaits = 0;
        // The short read the last accumulate wait followed, 0 once judged;
        // see ``floodShortReadBytes`` for why a read that does not grow past
        // it ends the waits for the burst.
        int bytesBeforeAccumulateWait = 0;
        bool isAccumulateWaitRefuted = false;
        for (;;) {
            if (m_stop.load(std::memory_order_relaxed) || m_runtime.isIngestHeld())
                break; // hold: the frontend answers a detected transfer; the kernel queue keeps the frame for its agent
            const std::uint64_t producer = m_floodRingControl->producer.load(std::memory_order_relaxed);
            const std::uint64_t consumerRaw = m_floodRingControl->consumer.load(std::memory_order_acquire);
            if (consumerRaw & floodRingDoneBit)
                break; // consumer stopped mid-burst; nobody will drain the ring
            const std::size_t freeBytes = floodRingCapacity - static_cast<std::size_t>(producer - (consumerRaw & floodRingPositionMask));
            if (freeBytes == 0) {
                // Parser is behind with a full ring queued — multi-millisecond
                // territory, so park on the consumer counter instead of
                // yielding in place; the consumer notifies after every bite.
                // A consume or a consumer stop changes the word, so the wake
                // cannot be lost. Once the kernel queue fills too, the writer
                // blocks — backpressure reaches the producer through the ring.
                m_floodRingControl->consumer.wait(consumerRaw, std::memory_order_acquire);
                continue;
            }
            const std::size_t writeIndex = static_cast<std::size_t>(producer) & (floodRingCapacity - 1);
            const std::size_t contiguous = std::min(freeBytes, floodRingCapacity - writeIndex);
            const int bytesRead = m_floodEndpoint->read({ ring + writeIndex, contiguous });
            if (bytesRead > 0) {
                // No notify: the consumer polls this counter on a timed nap
                // (floodConsumerNapMicros), never a futex — the publish path
                // stays a bare store.
                const std::uint64_t published = producer + static_cast<std::uint64_t>(bytesRead);
                m_floodRingControl->producer.store(published, std::memory_order_release);
                quietProbes = 0;
                quietWaits = 0;
                if (bytesBeforeAccumulateWait > 0) {
                    isAccumulateWaitRefuted = bytesRead <= bytesBeforeAccumulateWait;
                    bytesBeforeAccumulateWait = 0;
                }
                if (!isAccumulateWaitRefuted && bytesRead < floodShortReadBytes
                    && published - (consumerRaw & floodRingPositionMask) >= floodAccumulateBacklogBytes) {
                    bytesBeforeAccumulateWait = bytesRead;
                    m_floodAccumulateWaitCount.fetch_add(1, std::memory_order_relaxed);
                    floodAccumulateWait();
                }
                continue;
            }
            if (bytesRead == 0 || bytesRead == static_cast<int>(PAL::StreamStatus::EndOfFile))
                break; // end of stream — the post-pipeline sequential pass latches it
            if (bytesRead == static_cast<int>(PAL::StreamStatus::Again)) {
                bytesBeforeAccumulateWait = 0; // the writer paused: no evidence about the slab either way
                // Bounded probe band before the readiness wait: the wait is a
                // syscall, and the saturated-flood writer rearms almost at once.
                // Past the band, park: readability wakes it the instant bytes
                // land, the kernel buffer holds the slack across the wake,
                // and a parked wait never touches the tty lock the writer
                // needs.
                if (!isWaitFirst && quietProbes < floodSpinBudget) {
                    ++quietProbes;
                    floodProbeWait();
                    continue;
                }
                PAL::Watch watch;
                watch.handle = &m_floodEndpoint->handle();
                watch.interest = PAL::Ready::Readable;
                const int readyCount = PAL::waitForReadiness({ &watch, 1 }, floodReaderQuietPollMilliseconds);
                if (readyCount > 0 && PAL::contains(watch.result, PAL::Ready::Readable | PAL::Ready::HangUp | PAL::Ready::Error)) {
                    quietProbes = 0;
                    quietWaits = 0;
                    continue;
                }
                if (++quietWaits >= floodReaderQuietPollBudget)
                    break; // sustained quiet: the burst is over
                continue;
            }
            break; // fatal or closed: the Stream classifies next
        }
        // The napping consumer observes the done bit on its next poll (≤ one nap).
        m_floodRingControl->producer.fetch_or(floodRingDoneBit, std::memory_order_release);
    }

    /// Pumps one flood burst through the ring: reader thread in, parser here.
    ///
    /// Runs on the loop's thread. Spawns the reader, parses ring bites under
    /// the model lock, and closes a pass with the runtime's shared
    /// end-of-pass bookkeeping at the byte cap or presentation interval.
    /// The time check follows a whole ring bite, bounding its overshoot to
    /// that bite's parse time without discarding unread ring bytes. Returns
    /// when the burst ends (reader exited and the ring drained) or on stop.
    ///
    /// - Parameters:
    ///   - pendingOutcome: The caller's held outcome, merged into here.
    ///   - havePendingOutcome: Whether the held outcome is live.
    ///   - pendingIngestMicros: The held outcome's accumulated parse time.
    ///   - lastDeliverMicros: When the caller last delivered.
    void runFloodPipeline(DrainOutcome& pendingOutcome, bool& havePendingOutcome,
        std::uint32_t& pendingIngestMicros, std::uint32_t& lastDeliverMicros)
    {
        m_floodBurstCount.fetch_add(1, std::memory_order_relaxed);
        if (!m_floodRing)
            m_floodRing = std::make_unique<char[]>(floodRingCapacity);
        if (!m_floodRingControl)
            m_floodRingControl = std::make_unique<FloodRingControl>();
        m_floodRingControl->producer.store(0, std::memory_order_relaxed);
        m_floodRingControl->consumer.store(0, std::memory_order_relaxed); // also clears both done bits from the previous burst
        // Burst is live: hold the render on the last complete frame while
        // the pipeline pumps, exactly as drainAvailable does for a
        // sequential pass. Cleared at burst end, after the settled-frame
        // publish.
        m_runtime.setDrainPassActive(true);
        std::jthread reader([this] { floodReaderLoop(); });

        constexpr int passCapacity = TerminalRuntime::drainPassCapacityBytes;
        long passBytes = 0;
        std::uint32_t passStartMicros = PAL::monotonicMicros();
        const auto finishPass = [&](DrainPassEnd end) {
            const DrainOutcome out = m_runtime.finishExternalDrainPass(m_host, static_cast<int>(passBytes), end);
            const std::uint32_t now = PAL::monotonicMicros();
            coalesce(out, pendingOutcome, havePendingOutcome, pendingIngestMicros, now - passStartMicros, now, lastDeliverMicros);
            passBytes = 0;
            passStartMicros = now;
            m_runtime.applyPendingCommands(m_host);
            m_runtime.drainPendingWrite();
        };
        const auto duePassEnd = [&] {
            if (passBytes >= passCapacity)
                return DrainPassEnd::ByteBudget;
            const std::uint32_t presentationInterval = m_host.drainPassIntervalMicroseconds();
            if (passBytes > 0 && presentationInterval && PAL::monotonicMicros() - passStartMicros >= presentationInterval)
                return DrainPassEnd::TimeBudget;
            return DrainPassEnd::None;
        };
        bool isStopping = false;
        bool didNapOnEmptyRing = false;
        std::uint32_t heldFrameAskedAt = 0;
        std::uint32_t heldFrameWaitMicros = 0;
        for (;;) {
            if (m_stop.load(std::memory_order_relaxed)) {
                isStopping = true;
                break;
            }
            const std::uint64_t consumer = m_floodRingControl->consumer.load(std::memory_order_relaxed);
            const std::uint64_t producerRaw = m_floodRingControl->producer.load(std::memory_order_acquire);
            const std::uint64_t available = (producerRaw & floodRingPositionMask) - consumer;
            if (available == 0) {
                if (producerRaw & floodRingDoneBit)
                    break; // reader exited and the ring is drained
                // Ring empty across a whole nap with a partial pass in hand:
                // close the pass now so its content delivers, rather than
                // holding parsed bytes through the reader's quiet-poll
                // teardown — the burst's last frame otherwise waits tens of
                // milliseconds for bookkeeping the gap already has time for.
                // A saturating writer refills within microseconds, so a
                // single momentary empty (no nap yet) never closes a pass.
                if (passBytes > 0 && didNapOnEmptyRing) {
                    finishPass(DrainPassEnd::WouldBlock);
                    didNapOnEmptyRing = false;
                    continue;
                }
                // The writer paused exactly on a pass boundary: no partial pass
                // is left to close, and the full one before it left the drain
                // saturated, which holds the render off the live grid until the
                // reader's quiet budget ends the burst. An empty pass reports the
                // pause the way the sequential drain's empty read does, and its
                // outcome joins the held one like any pass's: the bits it alone
                // carries — a process state its probe sampled — are delivered
                // nowhere else. A paused writer leaves the throttle nothing to
                // coalesce, so what is held is delivered now rather than at the
                // end of the burst.
                if (passBytes == 0 && didNapOnEmptyRing && m_runtime.isDrainSaturated()) {
                    const DrainOutcome out = m_runtime.finishExternalDrainPass(m_host, 0, DrainPassEnd::WouldBlock);
                    const std::uint32_t now = PAL::monotonicMicros();
                    coalesce(out, pendingOutcome, havePendingOutcome, pendingIngestMicros, 0, now, lastDeliverMicros);
                    if (havePendingOutcome) {
                        deliver(pendingOutcome, pendingIngestMicros);
                        havePendingOutcome = false;
                        pendingIngestMicros = 0;
                        lastDeliverMicros = now;
                    }
                    passStartMicros = now;
                    didNapOnEmptyRing = false;
                    continue;
                }
                // A timed pass can leave a pacing or protocol hold with no
                // partial pass left to close. The ring stayed empty across a
                // nap, so service that hold on its own clock while the reader
                // remains alive. No new pass or delivery is implied.
                if (didNapOnEmptyRing && m_runtime.isFrameHeld() && !m_runtime.isIngestHeld()) {
                    const std::uint32_t now = PAL::monotonicMicros();
                    if (now - heldFrameAskedAt >= heldFrameWaitMicros) {
                        const std::optional<std::uint64_t> due = m_runtime.publishHeldFrameIfDue(m_host);
                        heldFrameAskedAt = PAL::monotonicMicros();
                        // Match the sequential timer's bounded retry floor:
                        // a refusal that is already due must not spin.
                        heldFrameWaitMicros = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
                            due.value_or(ingestFallbackTickMilliseconds * 1000), 1'000, ingestFallbackTickMilliseconds * 1000));
                        if (!m_runtime.isFrameHeld())
                            announceRelease();
                    }
                }
                // Ring momentarily empty mid-flood: timed nap, then re-poll
                // the counter (rationale at floodConsumerNapMicros). No
                // lost-wake hazard: the done bit and the stop flag are
                // re-read every wake.
                m_floodNapCount.fetch_add(1, std::memory_order_relaxed);
                floodConsumerNap();
                didNapOnEmptyRing = true;
                continue;
            }
            didNapOnEmptyRing = false;
            heldFrameWaitMicros = 0;
            const std::size_t readIndex = static_cast<std::size_t>(consumer) & (floodRingCapacity - 1);
            std::size_t bite = std::min<std::size_t>(static_cast<std::size_t>(available), floodRingCapacity - readIndex);
            bite = std::min({ bite, floodBiteCapacityBytes, static_cast<std::size_t>(passCapacity - passBytes) });
            const int biteBytes = static_cast<int>(bite);
            int parsedBytes = 0;
            parseFloodBite(readIndex, biteBytes, parsedBytes);
            m_floodRingControl->consumer.store(consumer + static_cast<std::uint64_t>(biteBytes), std::memory_order_release);
            // Wake a reader parked on ring-full (uncontended notify: no syscall).
            m_floodRingControl->consumer.notify_one();
            if (m_runtime.isIngestHeld()) {
                // A preamble fired inside this bite: what the tap kept of it
                // entered the model above, and the bite's remainder — frame
                // bytes the reader already lifted off the transport — is
                // dropped (the sender retransmits its lead-in until answered;
                // see drainAvailable). Stop the burst here; the reader observes
                // the same hold and stops filling the ring.
                passBytes += parsedBytes;
                break;
            }
            passBytes += biteBytes;
            if (const DrainPassEnd end = duePassEnd(); end != DrainPassEnd::None) {
                // Pass boundary: identical cadence to a capacity-limited
                // sequential pass — commands, pending writes, bookkeeping,
                // coalesced delivery.
                finishPass(end);
            }
        }
        // A stop can break out of the loop above while the reader is parked
        // on ring-full; mark the consumer done (changing the word it waits
        // on) so it wakes, observes the bit, and exits — otherwise the join
        // deadlocks.
        if (isStopping) {
            m_floodRingControl->consumer.fetch_or(floodRingDoneBit, std::memory_order_release);
            m_floodRingControl->consumer.notify_all();
        }
        reader.join();
        // Drain the residue the reader published after the last look. On
        // stop the residue is abandoned, matching the sequential loop, which
        // also leaves unread transport bytes behind on stop.
        if (!isStopping) {
            for (;;) {
                // Held: the consumer loop already stopped parsing the burst;
                // ring bytes the reader lifted before it saw the hold must
                // not paint either, and the sender retransmits its lead-in
                // for the transfer agent.
                if (m_runtime.isIngestHeld())
                    break;
                if (const DrainPassEnd end = duePassEnd(); end != DrainPassEnd::None)
                    finishPass(end);
                const std::uint64_t consumer = m_floodRingControl->consumer.load(std::memory_order_relaxed);
                const std::uint64_t producer = m_floodRingControl->producer.load(std::memory_order_acquire) & floodRingPositionMask;
                const std::uint64_t available = producer - consumer;
                if (available == 0)
                    break;
                const std::size_t readIndex = static_cast<std::size_t>(consumer) & (floodRingCapacity - 1);
                const int biteBytes = static_cast<int>(std::min({ floodBiteCapacityBytes, static_cast<std::size_t>(available), floodRingCapacity - readIndex,
                    static_cast<std::size_t>(passCapacity - passBytes) }));
                int parsedBytes = 0;
                parseFloodBite(readIndex, biteBytes, parsedBytes);
                m_floodRingControl->consumer.store(consumer + static_cast<std::uint64_t>(biteBytes), std::memory_order_release);
                passBytes += parsedBytes;
                if (!m_runtime.isIngestHeld()) {
                    if (const DrainPassEnd end = duePassEnd(); end != DrainPassEnd::None)
                        finishPass(end);
                }
            }
        }
        if (passBytes > 0) {
            const DrainOutcome out = m_runtime.finishExternalDrainPass(m_host, static_cast<int>(passBytes),
                isStopping || m_runtime.isIngestHeld() ? DrainPassEnd::OwnerFence
                    : passBytes >= passCapacity        ? DrainPassEnd::ByteBudget
                                                       : DrainPassEnd::WouldBlock);
            const std::uint32_t now = PAL::monotonicMicros();
            coalesce(out, pendingOutcome, havePendingOutcome, pendingIngestMicros, now - passStartMicros, now, lastDeliverMicros);
        }
        // Burst over: publish a complete settled frame (coherent-gated) so
        // the render has one to promote the instant the hold below lifts,
        // then drop the hold. On a stop the publish is skipped (the residue
        // was abandoned) but the flag still drops so a restart is not wedged
        // holding. The follow-up sequential drain re-asserts the flag if
        // more bytes arrive.
        if (!isStopping) {
            m_host.lockModel();
            // The writer has stopped, so a redraw behind its last whole-screen
            // erase has been parsed whole, including one whose last pass ended
            // on a budget and left no partial pass to close here.
            m_runtime.noteEraseRedrawParsed();
            // The writer has stopped, so the render's live path finds an idle
            // lock: this is a settle, whatever the last pass's size was -- and
            // the last one there will be, which is what keeps the throttle off
            // it.
            m_runtime.publishSettledFrameIfCoherent(m_host, TerminalRuntime::SettleContext::BurstEnd);
            m_host.unlockModel();
        }
        m_runtime.setDrainPassActive(false);
    }

    TerminalRuntime& m_runtime;
    DrainHost& m_host;
    // The delivery throttle interval, defaulted to the floor and raised to
    // the pane's display cadence by setDeliverMinInterval.
    std::atomic<std::uint32_t> m_deliverMinIntervalMicros { drainDeliverMinIntervalMicros };
    DeliverHook m_deliverHook = nullptr;
    void* m_deliverContext = nullptr;
    ReleaseHook m_releaseHook = nullptr;
    void* m_releaseContext = nullptr;
    std::atomic<bool> m_stop { false };
    std::shared_ptr<PAL::Waker> m_waker;
    /// The reader's endpoint; null keeps floods on the sequential passes.
    std::unique_ptr<PAL::PollableByteStream> m_floodEndpoint;
    /// Ring-empty naps, cumulatively; see ``floodNapCount()``.
    std::atomic<std::uint64_t> m_floodNapCount { 0 };
    /// Pipelined bursts, cumulatively; see ``floodBurstCount()``.
    std::atomic<std::uint64_t> m_floodBurstCount { 0 };
    /// Short-read accumulate waits, cumulatively; see ``floodAccumulateWaitCount()``.
    std::atomic<std::uint64_t> m_floodAccumulateWaitCount { 0 };
    /// The burst ring, allocated on the first flood and kept.
    std::unique_ptr<char[]> m_floodRing;
    /// The ring's SPSC counters, heap-held for their cache-line isolation.
    std::unique_ptr<FloodRingControl> m_floodRingControl;
};

} // namespace Ubiquitty::Runtime
