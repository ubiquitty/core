// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// StreamDouble.h — shared PAL::Stream, ProcessFacts and DrainHost doubles.
//
// A test declares the behaviour it needs instead of implementing twelve
// overrides to expose one field. Header-only and free of link dependencies
// beyond PAL's own interfaces, so a bench or a test picks it up with an
// #include and no build-system change.
//
//   TestDoubles::StreamDouble stream;
//   stream.pushInbound("hello\r\n");
//   stream.setQuantumBytes(1024);          // one pty quantum per read
//   stream.refillAfterEagainCount(2);      // deterministic EAGAIN ladder
//   ...
//   assert(stream.written == "\x1b[0n");   // replies are captured
//
// The refill policy is the axis that serves every caller. A count-based ladder
// is deterministic and needs no clock, so correctness tests cannot flake under
// load. A nanosecond gap models the kernel writer, whose refill clock starts
// when the reader takes the data — ttwrite sleeps at the output-queue hiwat and
// ttwwakeup only wakes it once the reader drains to lowat.
//
// Pairing a nanosecond gap with setQuantumBytes(1024) reproduces the pty's shape
// without its cost, which is what makes the refill window sweepable: against the
// real transport, throughput is pinned at the kernel ceiling and parse hides
// inside the window, so the lane resolves regressions but not improvements.
// Sweeping the gap locates the knee where parse stops fitting, and the knee is
// parse cost per quantum.
//
// A simulated run is diagnostic. It omits the read syscall that dominates the
// real cycle, so its MB/s is not comparable to a real-transport figure, and
// flood or scroll work is certified against the real pty.

#include "Core/Runtime/TerminalRuntime.h"
#include "PAL/OutputQueue.h"
#include "PAL/Process.h"
#include "PAL/Stream.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Ubiquitty::TestDoubles {

// Scripted answers with a probe counter per question, so a caller can assert a
// probe budget rather than only a value.
class ProcessFactsDouble final : public PAL::ProcessFacts {
public:
    std::string name;
    bool isShell = true;
    std::optional<PAL::ProcessID> group; // empty: no foreground group resolves
    std::string currentDirectory;
    std::string tty;
    bool runningChild = false;
    std::vector<PAL::Process::Record> processes;

    int nameProbes = 0;
    int shellProbes = 0;
    int groupProbes = 0;
    int currentDirectoryProbes = 0;
    int gatherProbes = 0;
    std::vector<std::pair<PAL::ProcessID, int>> signalledGroups;
    bool canSignal = false;
    /// The discipline `lineDiscipline` reports; none, as a pipe has, until a
    /// test sets one.
    std::optional<PAL::LineDiscipline> discipline;
    int disciplineProbes = 0;

    /// Reports the configured line discipline.
    ///
    /// - Returns: `discipline`.
    std::optional<PAL::LineDiscipline> lineDiscipline() override
    {
        ++disciplineProbes;
        return discipline;
    }

    /// Records a process-group signal request.
    ///
    /// - Parameters:
    ///   - processGroup: The target process group.
    ///   - signal: The requested signal number.
    /// - Returns: The configured signal result.
    bool sendSignalToProcessGroup(PAL::ProcessID processGroup, int signal) override
    {
        signalledGroups.emplace_back(processGroup, signal);
        return canSignal;
    }

    std::string foregroundName() override
    {
        ++nameProbes;
        return name;
    }
    bool foregroundIsShell() override
    {
        ++shellProbes;
        return isShell;
    }
    std::optional<PAL::ProcessID> foregroundProcessGroup() override
    {
        ++groupProbes;
        return group;
    }
    std::string foregroundCurrentDirectory() override
    {
        ++currentDirectoryProbes;
        return currentDirectory;
    }
    std::string ttyName() override { return tty; }
    bool hasRunningChild() override { return runningChild; }
    int gatherProcesses(std::span<PAL::Process::Record> outputBuffer) override
    {
        ++gatherProbes;
        std::size_t count = processes.size();
        if (count > outputBuffer.size())
            count = outputBuffer.size();
        for (std::size_t i = 0; i < count; ++i)
            outputBuffer[i] = processes[i];
        return static_cast<int>(count);
    }
};

class StreamDouble final : public PAL::Stream {
public:
    // `destroyCounter` records this double's destruction, so an owner's
    // close-exactly-once contract is assertable.
    explicit StreamDouble(int* destroyCounter = nullptr)
        : m_destroyCounter(destroyCounter)
    {
    }

    ~StreamDouble() override
    {
        if (m_destroyCounter)
            ++*m_destroyCounter;
    }

    // --- scripting the peer ------------------------------------------------
    void pushInbound(std::string_view bytes) { m_inbound.insert(m_inbound.end(), bytes.begin(), bytes.end()); }
    void setInbound(std::vector<char> bytes)
    {
        m_inbound = std::move(bytes);
        m_offset = 0;
    }
    // Report EndOfFile rather than Again once the scripted bytes run out.
    void signalEndOfFile() { m_endOfFilePending = true; }
    // The next read reports Fatal and carries `errnoValue` in lastError().
    void failWith(int errnoValue) { m_pendingError = errnoValue; }

    // --- shape of the transport --------------------------------------------
    // Cap each read at one quantum. 0 hands back everything available, which is
    // what a pipe does; 1024 is what a macOS pty does.
    void setQuantumBytes(std::size_t bytes) { m_quantumBytes = bytes; }
    void refillImmediately()
    {
        m_eagainsBeforeRefill = 0;
        m_refillGap = std::chrono::nanoseconds::zero();
    }
    // Report Again exactly `count` times between quanta.
    void refillAfterEagainCount(int count)
    {
        m_eagainsBeforeRefill = count;
        m_refillGap = std::chrono::nanoseconds::zero();
        // Both policies open satisfied: the peer writes before a reader arrives,
        // so the gate governs the interval between quanta, not the first read.
        m_eagainsSinceRead = count;
    }
    // The simulated writer needs `nanoseconds` to refill after each read. A parse
    // that outlasts the gap finds the next quantum already waiting, so parse is
    // absorbed by the window until it exceeds it.
    void refillAfterNanoseconds(long nanoseconds)
    {
        m_eagainsBeforeRefill = 0;
        m_refillGap = std::chrono::nanoseconds(nanoseconds);
        m_readyAt = std::chrono::steady_clock::now();
    }

    void setPendingWrite(bool pending) { m_pendingWrite = pending; }

    // The write side backs up or breaks. A real pty short-writes and then
    // refuses with Again while the child is not reading, and Closed/Fatal once
    // it is gone; a caller that treats either as "sent" loses those bytes
    // silently. Accept `bytes` in total (one byte per write, so a partial write
    // is what the caller sees), then refuse every further write with `status`.
    // `bytes` of 0 refuses from the first write.
    void acceptBytesThenRefuse(std::size_t bytes, PAL::StreamStatus status)
    {
        m_writeAcceptBudget = bytes;
        m_writeRefusalStatus = static_cast<int>(status);
        m_writeBudgeted = true;
    }
    /// Makes `writeAll` queue as a PTY stream does.
    ///
    /// What `write` refuses is queued behind anything already queued and
    /// reported as taken, and `drainPendingWrite` pushes the queue through
    /// `write` in order. Off by default, where `writeAll` is one `write` and
    /// its partial count.
    void queueWriteAll() { m_shouldQueueWriteAll = true; }
    /// Bounds the `writeAll` queue as the PTY stream's output queue is bounded.
    ///
    /// A `writeAll` whose bytes would take the queue past `bytes` queues none
    /// of them and reports Again.
    ///
    /// - Parameter bytes: The queue's capacity; zero leaves it unbounded.
    void capWriteQueue(std::size_t bytes) { m_writeQueueCapacity = bytes; }

    // Back to a transport that takes everything, which is what shows whether a
    // deferred payload comes back rather than having been dropped.
    void allowWrites()
    {
        m_writeBudgeted = false;
        m_writeAcceptBudget = 0;
        m_writeRefusalStatus = 0;
    }

    // A caller that retries a refusal that will never clear spins forever, and
    // a test asserting "it stopped" would then HANG rather than fail — a timeout
    // with no diagnosis, which is worse than a red assertion. So the refusal
    // gives up after this many writes and starts accepting, leaving `spun` set:
    // the caller's loop terminates and the test fails on the flag instead.
    static constexpr int writeRefusalSpinGuard = 1000;
    bool spun = false; // the caller kept retrying a refusal past the guard

    // --- observation --------------------------------------------------------
    std::string written; // every write()/writeAll() payload, in order
    int readCalls = 0;
    int againCount = 0;
    int writeCalls = 0;
    int columns = -1;
    int rows = -1;
    int xPixels = -1;
    int yPixels = -1;
    int windowSizeCalls = 0; // setWindowSize calls: one SIGWINCH each on a real PTY

    [[nodiscard]] std::size_t bytesRemaining() const { return m_inbound.size() - m_offset; }

    // --- PAL::Stream --------------------------------------------------------
    int read(std::span<char> buffer) override
    {
        ++readCalls;
        if (m_pendingError) {
            m_lastError = m_pendingError;
            m_pendingError = 0;
            return static_cast<int>(PAL::StreamStatus::Fatal);
        }
        // An empty buffer is a caller bug rather than a transport state.
        if (buffer.empty())
            return static_cast<int>(PAL::StreamStatus::Fatal);
        if (m_offset >= m_inbound.size())
            return static_cast<int>(m_endOfFilePending ? PAL::StreamStatus::EndOfFile : PAL::StreamStatus::Again);
        if (!refillReady()) {
            ++againCount;
            ++m_eagainsSinceRead;
            return static_cast<int>(PAL::StreamStatus::Again);
        }
        std::size_t count = m_inbound.size() - m_offset;
        if (m_quantumBytes && count > m_quantumBytes)
            count = m_quantumBytes;
        if (count > buffer.size())
            count = buffer.size();
        std::memcpy(buffer.data(), m_inbound.data() + m_offset, count);
        m_offset += count;
        m_eagainsSinceRead = 0;
        if (m_refillGap != std::chrono::nanoseconds::zero())
            m_readyAt = std::chrono::steady_clock::now() + m_refillGap;
        return static_cast<int>(count);
    }

    int write(std::span<const char> bytes) override
    {
        ++writeCalls;
        std::size_t count = bytes.size();
        if (m_writeBudgeted) {
            if (m_writeAcceptBudget == 0) {
                if (++m_writeRefusals <= writeRefusalSpinGuard)
                    return m_writeRefusalStatus;
                spun = true;
                allowWrites(); // let the caller out, and let the test say why
            } else {
                count = 1; // one byte per write, so the caller sees a partial write
                --m_writeAcceptBudget;
            }
        }
        written.append(bytes.data(), count);
        return static_cast<int>(count);
    }
    int writeAll(std::span<const char> bytes) override
    {
        if (!m_shouldQueueWriteAll)
            return write(bytes);
        if (m_hasOwnedWrites)
            return m_ownedQueue.submit(bytes, ownedSink, this);
        if (m_writeQueueCapacity > 0) {
            (void)flushWriteQueue();
            if (m_writeQueue.size() + bytes.size() > m_writeQueueCapacity)
                return static_cast<int>(PAL::StreamStatus::Again);
        }
        m_writeQueue.append(bytes.data(), bytes.size());
        const int status = flushWriteQueue();
        return status < 0 && status != static_cast<int>(PAL::StreamStatus::Again) ? status : 0;
    }
    /// Queues through the real owned FIFO when queue emulation is enabled.
    /// - Parameters:
    ///   - owner: Server-issued nonzero owner.
    ///   - bytes: Submitted bytes.
    /// - Returns: Admission status, or nullopt for owner zero.
    std::optional<int> writeAllOwned(std::uint64_t owner, std::span<const char> bytes) override
    {
        if (!owner)
            return std::nullopt;
        if (!m_shouldQueueWriteAll)
            return write(bytes);
        if (!m_writeQueue.empty()) {
            (void)m_ownedQueue.submit(m_writeQueue, ownedSink, this);
            m_writeQueue.clear();
        }
        m_hasOwnedWrites = true;
        if (m_writeQueueCapacity && m_ownedQueue.pendingBytes() + bytes.size() > m_writeQueueCapacity)
            return static_cast<int>(PAL::StreamStatus::Again);
        const int result = m_ownedQueue.submitOwned(owner, bytes, ownedSink, this);
        return result == PAL::OutputQueue::errQueueFull ? static_cast<int>(PAL::StreamStatus::Again) : result;
    }
    /// Counts one emulated owner's queued suffix.
    /// - Parameter owner: Nonzero output owner.
    /// - Returns: Pending bytes, or nullopt for zero.
    std::optional<std::size_t> pendingOwnedWriteBytes(std::uint64_t owner) const override
    {
        return owner ? std::optional { m_ownedQueue.pendingBytes(owner) } : std::nullopt;
    }
    /// Removes only one emulated owner's suffix.
    /// - Parameter owner: Nonzero output owner.
    /// - Returns: Removed bytes, or nullopt for zero.
    std::optional<std::size_t> discardPendingWrite(std::uint64_t owner) override
    {
        return owner ? std::optional { m_ownedQueue.discardPending(owner) } : std::nullopt;
    }
    /// Replaces owned bytes with bounded host delimiter repair.
    /// - Parameters:
    ///   - owner: Retired output owner.
    ///   - repair: Trusted delimiter bytes.
    /// - Returns: Whether the FIFO admitted the replacement.
    bool repairPendingWrite(std::uint64_t owner, std::span<const char> repair) override
    {
        m_hasOwnedWrites = true;
        return m_ownedQueue.repairPending(owner, repair);
    }
    bool drainPendingWrite() override
    {
        (void)m_ownedQueue.drain(ownedSink, this);
        (void)flushWriteQueue();
        return hasPendingWrite();
    }
    [[nodiscard]] bool hasPendingWrite() const override { return m_pendingWrite || !m_writeQueue.empty() || m_ownedQueue.hasPending(); }
    [[nodiscard]] std::size_t pendingWriteBytes() const override { return m_pendingWrite ? std::numeric_limits<std::size_t>::max() : m_writeQueue.size() + m_ownedQueue.pendingBytes(); }

    void setWindowSize(int newColumns, int newRows, int newXPixels, int newYPixels) override
    {
        ++windowSizeCalls;
        columns = newColumns;
        rows = newRows;
        xPixels = newXPixels;
        yPixels = newYPixels;
    }
    [[nodiscard]] int lastError() const override { return m_lastError; }
    // No pollable descriptor: a caller that parks in poll() on this never wakes.
    [[nodiscard]] int fileDescriptor() const override { return -1; }
    PAL::ProcessFacts& process() override { return facts; }

    ProcessFactsDouble facts;

private:
    /// Adapts the double's backpressure to the FIFO sink contract.
    /// - Parameters:
    ///   - context: This stream double.
    ///   - bytes: Bytes offered by the FIFO.
    /// - Returns: Accepted count, zero for backpressure, or fatal status.
    static int ownedSink(void* context, std::span<const char> bytes)
    {
        const int result = static_cast<StreamDouble*>(context)->write(bytes);
        return result == static_cast<int>(PAL::StreamStatus::Again) ? 0 : result;
    }
    PAL::OutputQueue m_ownedQueue;
    bool m_hasOwnedWrites = false;
    /// Writes the queue's head until `write` refuses.
    ///
    /// - Returns: The refusal status, or 0 once the queue is empty.
    int flushWriteQueue()
    {
        while (!m_writeQueue.empty()) {
            const int n = write({ m_writeQueue.data(), m_writeQueue.size() });
            if (n < 0)
                return n;
            m_writeQueue.erase(0, static_cast<std::size_t>(n));
        }
        return 0;
    }

    [[nodiscard]] bool refillReady() const
    {
        if (m_eagainsBeforeRefill > 0)
            return m_eagainsSinceRead >= m_eagainsBeforeRefill;
        if (m_refillGap != std::chrono::nanoseconds::zero())
            return std::chrono::steady_clock::now() >= m_readyAt;
        return true;
    }

    std::vector<char> m_inbound;
    std::size_t m_offset = 0;
    std::size_t m_quantumBytes = 0;
    int m_eagainsBeforeRefill = 0;
    int m_eagainsSinceRead = 0;
    std::chrono::nanoseconds m_refillGap { 0 };
    std::chrono::steady_clock::time_point m_readyAt {};
    bool m_endOfFilePending = false;
    bool m_pendingWrite = false;
    bool m_shouldQueueWriteAll = false;
    std::string m_writeQueue; // writeAll bytes not yet taken by write()
    std::size_t m_writeQueueCapacity = 0; // 0: unbounded
    int m_pendingError = 0;
    bool m_writeBudgeted = false; // the write side is under acceptBytesThenRefuse
    std::size_t m_writeAcceptBudget = 0; // bytes left to accept before refusing
    int m_writeRefusalStatus = 0; // the StreamStatus returned once the budget runs out
    int m_writeRefusals = 0; // refusals issued, against writeRefusalSpinGuard
    int m_lastError = 0;
    int* m_destroyCounter = nullptr;
};

// A stream whose peer has already closed: the first read reports EndOfFile.
inline std::unique_ptr<StreamDouble> makeClosedStream()
{
    std::unique_ptr<StreamDouble> stream(new StreamDouble);
    stream->signalEndOfFile();
    return stream;
}

// A DrainHost that can contend for the model lock the way a renderer does.
//
// A zero frame period leaves the contending thread unstarted, which pins
// renderer cost at zero. That is a choice, not a neutral default: the drain loop
// takes the model lock once per read, and a renderer takes the same lock every
// frame. A real mutex and a real competing thread carry the genuine contention,
// cache traffic and scheduler interaction that a simulated delay approximates.
class DrainHostDouble final : public Runtime::DrainHost {
public:
    DrainHostDouble() = default;

    DrainHostDouble(std::chrono::microseconds framePeriod, std::chrono::microseconds lockHold)
        : m_framePeriod(framePeriod)
        , m_lockHold(lockHold)
    {
        if (m_framePeriod.count() > 0)
            m_renderer = std::thread([this] { renderLoop(); });
    }

    ~DrainHostDouble() override
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_renderer.joinable())
            m_renderer.join();
    }

    void lockModel() override
    {
        m_modelLock.lock();
        ++lockAcquisitions;
    }
    void unlockModel() override { m_modelLock.unlock(); }
    void onSyncFrameReadyLocked(Ubiquitty::Runtime::SyncFrameReason) override { ++syncFrames; }

    long lockAcquisitions = 0; // written under the lock by the draining thread
    long syncFrames = 0;
    long framesRendered() const { return m_framesRendered.load(std::memory_order_relaxed); }

private:
    // Spin while HOLDING, sleep while idle. Spinning under the lock is what a
    // renderer doing work looks like, and a sleeping holder would let the
    // draining thread run on another core and understate the stall. Spinning
    // BETWEEN frames is a different thing entirely: it burns a core continuously
    // and charges the draining thread for CPU competition unrelated to the lock,
    // which shows up as a duty-independent loss floor.
    void renderLoop()
    {
        while (!m_stop.load(std::memory_order_relaxed)) {
            const auto wakeAt = std::chrono::steady_clock::now() + m_framePeriod;
            {
                std::lock_guard<std::mutex> guard(m_modelLock);
                const auto releaseAt = std::chrono::steady_clock::now() + m_lockHold;
                while (std::chrono::steady_clock::now() < releaseAt) {
                }
            }
            m_framesRendered.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_until(wakeAt);
        }
    }

    std::mutex m_modelLock;
    std::chrono::microseconds m_framePeriod { 0 };
    std::chrono::microseconds m_lockHold { 0 };
    std::thread m_renderer;
    std::atomic<bool> m_stop { false };
    std::atomic<long> m_framesRendered { 0 };
};

// Shared command-line surface, so a knob is spelled the same way in every bench
// that adopts it and a result line stays readable across benches.
struct PacingOptions {
    long refillGapNanoseconds = 0; // 0 keeps the caller's real transport
    long quantumBytes = 1024;
    long framePeriodMicroseconds = 0; // 0 keeps the host inert
    long lockHoldMicroseconds = 0;

    static const char* usageText()
    {
        return "  --sim-gap-ns N        simulate the writer: N ns refill per quantum (0 = real transport)\n"
               "  --sim-quantum N       simulated quantum in bytes (default 1024)\n"
               "  --render-period-us N  renderer takes the model lock every N us (0 = inert)\n"
               "  --render-hold-us N    renderer holds the model lock N us per frame\n";
    }

    // Consumes one shared flag at argv[i], advancing i past its value. Returns
    // false when argv[i] belongs to the caller.
    bool consumeArgument(int argc, char** argv, int& i)
    {
        if (!std::strcmp(argv[i], "--sim-gap-ns") && i + 1 < argc) {
            refillGapNanoseconds = std::atol(argv[++i]);
            return true;
        }
        if (!std::strcmp(argv[i], "--sim-quantum") && i + 1 < argc) {
            quantumBytes = std::atol(argv[++i]);
            return true;
        }
        if (!std::strcmp(argv[i], "--render-period-us") && i + 1 < argc) {
            framePeriodMicroseconds = std::atol(argv[++i]);
            return true;
        }
        if (!std::strcmp(argv[i], "--render-hold-us") && i + 1 < argc) {
            lockHoldMicroseconds = std::atol(argv[++i]);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool simulatesWriter() const { return refillGapNanoseconds > 0; }
    [[nodiscard]] bool contendsRenderer() const { return framePeriodMicroseconds > 0; }

    // Null unless the writer is simulated, so a caller falls through to its real
    // transport. The result has no pollable descriptor: spin the lane.
    [[nodiscard]] std::unique_ptr<StreamDouble> makeStream(std::vector<char> payload) const
    {
        if (!simulatesWriter())
            return nullptr;
        std::unique_ptr<StreamDouble> stream(new StreamDouble);
        stream->setInbound(std::move(payload));
        stream->signalEndOfFile();
        stream->setQuantumBytes(static_cast<std::size_t>(quantumBytes));
        stream->refillAfterNanoseconds(refillGapNanoseconds);
        return stream;
    }

    [[nodiscard]] std::unique_ptr<DrainHostDouble> makeHost() const
    {
        return std::unique_ptr<DrainHostDouble>(new DrainHostDouble(
            std::chrono::microseconds(framePeriodMicroseconds), std::chrono::microseconds(lockHoldMicroseconds)));
    }

    // Belongs on every result line: a simulated figure quoted as a real-transport
    // one is the mistake this label prevents.
    [[nodiscard]] std::string label() const
    {
        std::string text = simulatesWriter()
            ? "writer=sim(" + std::to_string(refillGapNanoseconds) + "ns/" + std::to_string(quantumBytes) + "B)"
            : "writer=real";
        text += contendsRenderer()
            ? " renderer=" + std::to_string(lockHoldMicroseconds) + "us/" + std::to_string(framePeriodMicroseconds) + "us"
            : " renderer=inert";
        return text;
    }
};

} // namespace Ubiquitty::TestDoubles
