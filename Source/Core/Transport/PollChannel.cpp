// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PollChannel.cpp — see PollChannel.h.

#include "Core/Transport/PollChannel.h"

#include <algorithm>

#include "Core/Transport/CarrierCapacity.h"
#include "Core/Transport/CarrierCloseReason.h"
#include "PAL/Poll.h"
#include "PAL/Socket.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <span>

namespace Ubiquitty::Transport {

namespace {

// The outbound queue cap is the cross-carrier floor itself: this channel is
// the smallest-declared carrier, so its wedged-peer guard is what every other
// cap is measured against. Raising it is a decision made in CarrierCapacity.h,
// where the trade-off lives, not here.
constexpr std::size_t maxPendingOut = guaranteedOutboundFrameBytes;

// The largest piece the outbound queue holds as one unit. A piece is released
// when it has been written whole, so this is also the most a fully-delivered
// prefix can occupy — and it is large enough that a keystroke is still one
// piece and one write.
constexpr std::size_t outboundChunkBytes = 64u * 1024u;
constexpr std::size_t readChunk = 64u * 1024u;

} // namespace

PollChannel::PollChannel(Transport::Origin origin)
    : PollChannel(std::move(origin), OverflowPolicy::CloseChannel)
{
}

PollChannel::PollChannel(Transport::Origin origin, OverflowPolicy overflowPolicy)
    : m_origin(std::move(origin))
    , m_overflowPolicy(overflowPolicy)
{
}

PollChannel::~PollChannel() { stop(); }

bool PollChannel::start(ChannelObserver* observer)
{
    if (m_thread.joinable())
        return true; // already running (idempotent)
    if (m_health.load() != Health::Idle)
        return false; // already used (Failed / Closed) — make a fresh Channel to retry
    m_observer = observer;
    m_health.store(Health::Connecting, std::memory_order_release);

    std::string error;
    if (!openTransport(m_readStream, m_writeStream, error)) {
        m_health.store(Health::Failed, std::memory_order_release);
        return false; // immediate local setup error — no callback per the contract
    }
    // Self-pipe to wake poll() for new outbound bytes / stop().
    std::shared_ptr<PAL::Waker> waker = PAL::makeWaker();
    if (!waker) {
        m_readStream.reset();
        m_writeStream.reset();
        closeTransport();
        m_health.store(Health::Failed, std::memory_order_release);
        return false;
    }
    // Not made non-blocking here: every stream this layer hands out already is,
    // which is what lets the base stop touching a native handle at all.

    {
        std::scoped_lock lock(m_wakeMutex);
        m_waker = std::move(waker);
    }

    m_health.store(Health::Connected, std::memory_order_release);
    // Assign m_thread under the lifecycle mutex: a transport that drops at once
    // lets ioLoop reach joinAndCleanup (which reads m_thread) before this returns.
    {
        std::scoped_lock lock(m_lifecycleMutex);
        m_thread = std::thread([this] { ioLoop(); });
    }
    return true;
}

std::int64_t PollChannel::pendingOutboundBytes() const
{
    std::scoped_lock lock(m_outMutex);
    // The running total the cap is checked against: unwritten bytes across
    // every queued chunk, which is what a caller asking about backpressure
    // means by pending.
    return static_cast<std::int64_t>(m_pendingBytes);
}

std::int64_t PollChannel::outboundCapacityBytes() const
{
    // The same constant the overflow test at send() uses, so a caller asking
    // whether a frame fits gets the number that will actually judge it.
    return static_cast<std::int64_t>(maxPendingOut);
}

void PollChannel::suspendReading()
{
    // The poll set below drops the readable watch while this holds, so the
    // carrier goes unread and its receive buffer fills. Waking the loop is what
    // makes the change take effect before the current wait times out.
    m_isReadingSuspended.store(true, std::memory_order_release);
    wakeIOLoop();
}

void PollChannel::resumeReading()
{
    m_isReadingSuspended.store(false, std::memory_order_release);
    wakeIOLoop();
}

void PollChannel::send(std::span<const std::uint8_t> data)
{
    if (data.empty() || m_stopRequested.load(std::memory_order_acquire))
        return;
    {
        std::scoped_lock lock(m_outMutex);
        if (m_pendingBytes + data.size() > maxPendingOut) {
            // Peer wedged, buffer at cap. What that means depends on what the
            // stream carries. Over frames, bytes missing from the middle
            // desynchronise everything after them, so the channel ends and says
            // so — flagged here for the I/O loop, since callbacks must fire on
            // the I/O thread. Over a raw byte stream there is no framing to
            // desynchronise: the write is refused, counted, and the channel
            // lives, which is what a real terminal does by making the writer
            // wait rather than by ending the session.
            if (m_overflowPolicy == OverflowPolicy::CloseChannel)
                m_sendOverflow.store(true, std::memory_order_release);
            else
                m_refusedBytes.fetch_add(data.size(), std::memory_order_acq_rel);
        } else {
            // Split rather than queued whole: a chunk is reclaimed only once it
            // has been written completely, so one enormous chunk holds its own
            // delivered prefix for as long as it takes to drain. Bounding the
            // piece bounds that retention, and costs the same copy the queue
            // was going to make anyway.
            for (std::size_t offset = 0; offset < data.size(); offset += outboundChunkBytes) {
                const std::size_t pieceBytes = std::min(outboundChunkBytes, data.size() - offset);
                m_pendingChunks.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(offset),
                    data.begin() + static_cast<std::ptrdiff_t>(offset + pieceBytes));
            }
            m_pendingBytes += data.size();
        }
    }
    wakeIOLoop();
}

std::shared_ptr<PAL::Waker> PollChannel::loadWaker() const
{
    std::scoped_lock lock(m_wakeMutex);
    return m_waker;
}

void PollChannel::wakeIOLoop()
{
    // The strong reference is what makes this safe: it pins the waker alive for
    // the whole call, so teardown can neither destroy the endpoint a wake is
    // being written to nor let the kernel hand its descriptor number to an
    // unrelated open().
    const std::shared_ptr<PAL::Waker> waker = loadWaker();
    if (!waker)
        return;
    waker->wake();
}

void PollChannel::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    wakeIOLoop();
    joinAndCleanup();
}

void PollChannel::joinAndCleanup()
{
    // stop() may be called from the owner thread and re-entrantly from an observer
    // callback on the I/O thread — serialize so the two never race on m_thread.
    std::scoped_lock lock(m_lifecycleMutex);
    // Self-join: a callback called stop() on the I/O thread. Skip the join (the loop
    // unwinds on m_stopRequested); the owner's stop() / destructor do cleanup.
    if (m_thread.joinable()) {
        if (std::this_thread::get_id() == m_thread.get_id())
            return;
        m_thread.join();
    }
    if (m_cleanedUp)
        return;
    m_cleanedUp = true;
    m_readStream.reset();
    m_writeStream.reset();
    m_diagnosticStream.reset();
    m_diagnostics.clear();
    {
        // Drop this owner's reference; the pipe closes with the last in-flight
        // sender's. Releasing under m_wakeMutex — never held across the join
        // above — keeps a concurrent loadWakePipe() race-free.
        std::scoped_lock lock(m_wakeMutex);
        m_waker.reset();
    }
    closeTransport();
    // A clean stop reports Closed but never fires onClose (the contract).
    Health expected = m_health.load(std::memory_order_acquire);
    if (expected == Health::Connected || expected == Health::Connecting)
        m_health.store(Health::Closed, std::memory_order_release);
}

void PollChannel::emitCloseOnce(Health terminal, const std::string& reason)
{
    m_health.store(terminal, std::memory_order_release);
    if (m_closeEmitted.exchange(true, std::memory_order_acq_rel))
        return;
    drainDiagnostics();
    if (!m_stopRequested.load(std::memory_order_acquire) && m_observer)
        m_observer->onClose(m_diagnostics.empty() ? reason : reason + std::string(closeDiagnosticSeparator) + m_diagnostics);
}

void PollChannel::flushPending()
{
    Health closeHealth = Health::Connected;
    const char* closeReason = nullptr;

    for (;;) {
        const std::uint8_t* chunkData = nullptr;
        std::size_t chunkRemaining = 0;
        {
            std::scoped_lock lock(m_outMutex);
            if (m_pendingChunks.empty())
                break;
            const std::vector<std::uint8_t>& front = m_pendingChunks.front();
            chunkData = front.data() + m_pendingSent;
            chunkRemaining = front.size() - m_pendingSent;
        }
        // Unlocked: send() may append behind this chunk, and a deque leaves the
        // chunk itself where it is. Only the front is ever written, and only
        // this thread pops it.
        const int n = m_writeStream->write(std::span<const char>(reinterpret_cast<const char*>(chunkData), chunkRemaining));
        if (n > 0) {
            std::scoped_lock lock(m_outMutex);
            m_pendingSent += static_cast<std::size_t>(n);
            m_pendingBytes -= static_cast<std::size_t>(n);
            if (m_pendingSent == m_pendingChunks.front().size()) {
                // Written whole, so its storage goes back rather than being
                // carried as a delivered prefix nothing will read again.
                m_pendingChunks.pop_front();
                m_pendingSent = 0;
            }
            continue;
        }
        // As on the read side, a negative result is a StreamStatus. A peer that
        // has gone away arrives as EndOfFile or Closed rather than as EPIPE,
        // because the backend suppresses the signal and reports the status.
        const auto status = static_cast<PAL::StreamStatus>(n);
        if (status == PAL::StreamStatus::Again)
            break; // retry when the endpoint is next writable
        if (status == PAL::StreamStatus::EndOfFile || status == PAL::StreamStatus::Closed) {
            closeHealth = Health::Closed;
            closeReason = "the connection closed while sending";
        } else {
            closeHealth = Health::Failed;
            closeReason = "the connection failed while sending";
        }
        break;
    }

    if (closeReason)
        emitCloseOnce(closeHealth, closeReason);
}

void PollChannel::drainDiagnostics()
{
    std::array<char, 1024> buffer;
    // Bound each drain as well as retained bytes so noisy stderr cannot starve input.
    for (unsigned iteration = 0; m_diagnosticStream && iteration < 16; ++iteration) {
        const int count = m_diagnosticStream->read(buffer);
        if (count > 0) {
            m_diagnostics.append(buffer.data(), static_cast<std::size_t>(count));
            if (m_diagnostics.size() > 4096)
                m_diagnostics.erase(0, m_diagnostics.size() - 4096);
        } else {
            if (count != static_cast<int>(PAL::StreamStatus::Again))
                m_diagnosticStream.reset();
            break;
        }
    }
}

void PollChannel::ioLoop()
{
    // Hold the waker for the whole loop: teardown releases the channel's
    // reference the moment this thread joins, and a wait must not be handed a
    // handle that closed underneath it.
    const std::shared_ptr<PAL::Waker> waker = loadWaker();
    if (!waker)
        return;

    if (m_observer)
        m_observer->onConnect();

    std::array<char, readChunk> readBuffer;
    for (;;) {
        if (m_stopRequested.load(std::memory_order_acquire))
            return;
        if (m_sendOverflow.load(std::memory_order_acquire)) {
            emitCloseOnce(Health::Failed, "the connection stalled with too much unsent data");
            return;
        }

        bool wantWrite;
        {
            std::scoped_lock lock(m_outMutex);
            wantWrite = !m_pendingChunks.empty();
        }

        // A suspended reader drops the readable interest but keeps watching for
        // the peer hanging up: a consumer pacing itself still wants to hear the
        // far end go away, and the bytes it is not reading are not what tell it.
        // The interest stays non-empty either way, since a watch that asks for
        // nothing is a watch the poll layer has no answer for.
        const bool isReadingSuspended = m_isReadingSuspended.load(std::memory_order_acquire);
        PAL::Watch watches[4] = {
            { &m_readStream->handle(),
                isReadingSuspended ? PAL::Ready::HangUp : PAL::Ready::Readable,
                PAL::Ready::None },
            { &waker->handle(), PAL::Ready::Readable, PAL::Ready::None },
            { &m_writeStream->handle(), PAL::Ready::Writable, PAL::Ready::None },
        };
        std::size_t count = wantWrite ? 3u : 2u;
        const auto diagnosticIndex = count;
        if (m_diagnosticStream)
            watches[count++] = { &m_diagnosticStream->handle(), PAL::Ready::Readable, PAL::Ready::None };

        const int ready = PAL::waitForReadiness({ watches, count }, -1);
        if (ready < 0) {
            emitCloseOnce(Health::Failed, std::string("the connection failed (") + std::strerror(errno) + ")");
            return;
        }
        if (m_stopRequested.load(std::memory_order_acquire))
            return;
        if (ready == 0)
            continue; // a signal interrupted the wait; the timeout is infinite

        // A dead handle is ready forever, so a loop that ignored it would spin
        // instead of failing. Treat it as a fatal close.
        if (PAL::contains(watches[0].result, PAL::Ready::Invalid) || PAL::contains(watches[1].result, PAL::Ready::Invalid)
            || (wantWrite && PAL::contains(watches[2].result, PAL::Ready::Invalid))) {
            emitCloseOnce(Health::Failed, "the connection was lost");
            return;
        }

        // Clear delivered wakes; they only mean "recheck the queue / stop".
        if (PAL::contains(watches[1].result, PAL::Ready::Readable))
            waker->drain();

        if (diagnosticIndex < count && watches[diagnosticIndex].result != PAL::Ready::None)
            drainDiagnostics();

        // Outbound first, so a flood of inbound never starves replies.
        if (wantWrite && PAL::contains(watches[2].result, PAL::Ready::Writable | PAL::Ready::Error | PAL::Ready::HangUp)) {
            flushPending();
            if (m_closeEmitted.load(std::memory_order_acquire))
                return;
        }

        // The backstop, not the mechanism: dropping the interest above is what
        // makes a suspended loop wait, and a case measures that it does — a
        // loop that kept the interest burns a core on a socket that stays
        // readable. This covers a backend that reports readability it was not
        // asked for, where the interest alone would not hold. No case can
        // reach it while the interest drop works, which is why it is stated
        // here rather than left to look like a second mechanism.
        const PAL::Ready readableEvents = isReadingSuspended
            ? (PAL::Ready::HangUp | PAL::Ready::Error)
            : (PAL::Ready::Readable | PAL::Ready::HangUp | PAL::Ready::Error);
        if (PAL::contains(watches[0].result, readableEvents)) {
            for (;;) {
                const int n = m_readStream->read(readBuffer);
                if (n > 0) {
                    if (m_observer)
                        m_observer->onData({ reinterpret_cast<const std::uint8_t*>(readBuffer.data()), static_cast<std::size_t>(n) });
                    if (static_cast<std::size_t>(n) < readBuffer.size())
                        break; // a short read means the socket buffer is empty
                    continue; // full buffer — more may wait
                }
                // A negative result is a StreamStatus, not an errno: the stream
                // reports end-of-file as a status rather than as zero bytes, so a
                // loop that tested for zero would treat a closed peer as an idle
                // one and spin.
                const auto status = static_cast<PAL::StreamStatus>(n);
                if (status == PAL::StreamStatus::Again)
                    break;
                if (status == PAL::StreamStatus::EndOfFile || status == PAL::StreamStatus::Closed) {
                    emitCloseOnce(Health::Closed, std::string(peerClosedReason));
                    return;
                }
                emitCloseOnce(Health::Failed, "the connection failed while receiving");
                return;
            }
        }
    }
}

#if UBIQUITTY_ENABLE_TEST_HOOKS
std::size_t OutboundRetentionAccess::retainedBytes(PollChannel& channel)
{
    std::scoped_lock lock(channel.m_outMutex);
    std::size_t held = 0;
    for (const std::vector<std::uint8_t>& chunk : channel.m_pendingChunks)
        held += chunk.size();
    return held;
}
#endif

} // namespace Ubiquitty::Transport
