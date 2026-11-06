// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/flush_client_cursor.cpp — flushClient drains a backlogged client's
// outputBuffer through the consumed-bytes cursor.
//
// A client fd is O_NONBLOCK with a small (~8 KB) socket send buffer, so a multi-MiB
// backlog drains one small write per POLLOUT wake. flushClient advances outputConsumed
// instead of erasing the sent prefix per write (an O(n^2) memmove across the drain);
// the dead prefix is reclaimed only on a full drain, or by compactOutputBuffer once it
// is >= the compaction floor and >= half the buffer.
//
// This befriends the Daemon to stage a client on a real socketpair with a small
// SO_SNDBUF, feed several MiB through the PRIVATE queueToClient/flushClient while a
// slow reader pulls small chunks, and assert:
//   (a) delivery is byte-exact and ordered end to end (a position-derived pattern);
//   (b) after every flush the dead prefix obeys the compaction bound
//       (consumed < floor or consumed < size/2) — the O(n) drain invariant;
//   (c) a full drain releases the buffer and resets the cursor to 0;
//   (d) the high-water keys on PENDING bytes (size - consumed), not the raw
//       buffer size: a large-but-mostly-consumed buffer accepts more output, while
//       genuinely-pending bytes over the cap overflow and reset the cursor.
//
// Verify-by-revert: an erase-per-write flushClient still passes (a)/(c) but is
// O(n^2); a high-water keyed on outputBuffer.size() fails (d)'s accept case; a
// clear() that skips the cursor fails (d)'s reset case (the next queued frame
// would be skipped by a stale offset).
//
// AF_UNIX only; same QUIC-OFF host build as drop_defer (HostQUIC.cpp is an empty
// TU there). Exit 0 on success.
//

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/OutboundCapacity.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

// The daemon's own outbound caps (Platform/Host/OutboundCapacity.h).

// Friend seam: stage a client on a real fd, drive the private queue/flush pair,
// and read back the buffer/cursor state.
struct FlushClientCursorAccess {
    static std::size_t addClient(Daemon& d, PAL::UnixFileDescriptor fileDescriptor)
    {
        Daemon::Client c;
        c.fileDescriptor = std::move(fileDescriptor);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }
    static void queueToClient(Daemon& d, std::size_t index, const uint8_t* bytes, std::size_t length) { d.queueToClient(index, { bytes, length }); }
    static void flushClient(Daemon& d, std::size_t index) { d.flushClient(index); }
    static std::size_t bufferSize(const Daemon& d, std::size_t index) { return d.m_clients[index].outputBuffer.size(); }
    static std::size_t consumed(const Daemon& d, std::size_t index) { return d.m_clients[index].outputConsumed; }
    static std::size_t pending(const Daemon& d, std::size_t index) { return d.m_clients[index].pendingOutputBytes(); }
    /// Runs a bounded read pass after the write side failed.
    ///
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The physical client slot.
    static void readClient(Daemon& d, std::size_t index) { d.serviceClient(index); }
    /// Expires the fixed drain deadline without a wall-clock sleep.
    ///
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The physical client slot.
    static void expireFailedWrite(Daemon& d, std::size_t index)
    {
        d.m_clients[index].writeFailureDeadlineMilliseconds = 1;
        d.enforceInboundBudget();
    }
    /// Keeps elapsed time from masking the independent byte-cap check.
    ///
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The physical client slot.
    static void isolateFailedWriteByteBound(Daemon& d, std::size_t index)
    {
        d.m_clients[index].writeFailureDeadlineMilliseconds = std::numeric_limits<std::uint64_t>::max();
    }
    /// Returns the bytes decoded after the write side failed.
    ///
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The physical client slot.
    /// - Returns: The cumulative drain byte count.
    static std::size_t failedWriteReadBytes(const Daemon& d, std::size_t index) { return d.m_clients[index].writeFailureReadBytes; }
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static std::size_t compactionFloor() { return Daemon::Client::outputCompactionFloorBytes; }
    // Stage a buffer whose front is already consumed — the state a partially
    // drained backlog is in when more output is queued.
    static void stageConsumedBuffer(Daemon& d, std::size_t index, std::size_t size, std::size_t consumedBytes)
    {
        d.m_clients[index].outputBuffer.assign(size, 0u);
        d.m_clients[index].outputConsumed = consumedBytes;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::clientBufferHighWater;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::FlushClientCursorAccess;
using Ubiquitty::PAL::UnixFileDescriptor;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "flush_client_cursor: FAIL — %s\n", msg);
        ++failures;
    }
}

// Position-derived byte: any drop, duplication, or reorder anywhere in a multi-MiB
// stream mismatches at the exact offset.
static uint8_t patternByte(std::size_t i) { return static_cast<uint8_t>((i * 131u) ^ (i >> 8)); }

enum class FinalInput {
    Detach,
    End,
    Silent,
    Flood
};

static void writeFailureDrainsFinalFrames(FinalInput finalInput)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    using Screen = Ubiquitty::Host::ScreenPlaneAccess;
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 24, 80);
    const auto index = Screen::attach(daemon, session.id);
    Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(daemon, index, "drain-test");
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(daemon, index, client);
    const auto* connection = Screen::connection(daemon, index);
    check(connection && connection->attachment(), "drain premise: a granted attachment");
    if (!connection || !connection->attachment())
        return;
    const auto identity = connection->attachment()->attachmentID;
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check(false, "drain premise: socketpair");
        return;
    }
    UnixFileDescriptor peer(pair[1]);
    check(::fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0, "drain premise: nonblocking reader");
    // A half-close fails writes on every platform while preserving the read side.
    check(::shutdown(pair[0], SHUT_WR) == 0, "drain premise: failed write side");
    Screen::useTransport(daemon, index, UnixFileDescriptor(pair[0]));
    if (finalInput == FinalInput::Detach) {
        client.detach();
        const auto bye = client.takeStream(Screen::now());
        check(::write(peer.get(), bye.data(), bye.size()) == static_cast<ssize_t>(bye.size()), "drain premise: final Bye queued");
    }
    if (finalInput == FinalInput::Detach || finalInput == FinalInput::End)
        peer.reset();
    Ubiquitty::processBytes(session.term.get(), "pending output\r\n");
    Screen::publish(daemon, session);
    Screen::service(daemon, Screen::now() + 1000000);
    check(Screen::pending(daemon, index) != 0, "drain premise: output encounters the failed write");
    Screen::flush(daemon, index);
    check(!Screen::isDropped(daemon, index), "write failure keeps the read side for final frames");
    if (finalInput == FinalInput::Flood) {
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Probe { .time = 1 });
        std::vector<std::uint8_t> frame;
        check(Sync::appendFrame(frame, Sync::FrameKind::Probe, 0, body), "drain premise: valid probes");
        std::vector<std::uint8_t> flood;
        while (flood.size() < 128 * 1024)
            flood.insert(flood.end(), frame.begin(), frame.end());
        FlushClientCursorAccess::isolateFailedWriteByteBound(daemon, index);
        const int smallBuffer = 2048;
        check(::setsockopt(peer.get(), SOL_SOCKET, SO_SNDBUF, &smallBuffer, sizeof(smallBuffer)) == 0, "drain premise: small send buffer");
        check(::fcntl(peer.get(), F_SETFL, O_NONBLOCK) == 0, "drain premise: nonblocking sender");
        std::size_t sent = 0;
        for (std::size_t attempt = 0; attempt < 128 && sent < flood.size() && !Screen::isDropped(daemon, index); ++attempt) {
            const ssize_t count = ::write(peer.get(), flood.data() + sent, std::min<std::size_t>(4096, flood.size() - sent));
            if (count > 0)
                sent += static_cast<std::size_t>(count);
            else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                check(false, "drain premise: sending probes failed");
                break;
            }
            FlushClientCursorAccess::readClient(daemon, index);
        }
    }
    FlushClientCursorAccess::readClient(daemon, index);
    if (finalInput == FinalInput::Silent) {
        check(!Screen::isDropped(daemon, index), "a temporarily quiet read side waits for its fixed deadline");
        FlushClientCursorAccess::expireFailedWrite(daemon, index);
    }
    check(Screen::isDropped(daemon, index), "the final frames, EOF, byte bound or deadline finish the drain");
    const auto* retained = Screen::attachments(daemon).find(identity);
    if (finalInput == FinalInput::Detach)
        check(!retained, "an orderly Bye queued before write failure retires the attachment");
    else
        check(retained && retained->state == Sync::AttachmentState::Lost, "an unclean close preserves only resumable state");
    if (finalInput == FinalInput::Flood)
        check(FlushClientCursorAccess::failedWriteReadBytes(daemon, index) == 64 * 1024, "failed writes admit at most one read-pass budget");
}

int main()
{
    for (const auto input : { FinalInput::Detach, FinalInput::End, FinalInput::Silent, FinalInput::Flood })
        writeFailureDrainsFinalFrames(input);

    // Writing to a closed peer (e.g. ~Daemon's teardown EXIT after the reader end
    // closes) must return EPIPE, not kill this process.
    ::signal(SIGPIPE, SIG_IGN);

    Daemon d;

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::fprintf(stderr, "flush_client_cursor: FAIL — socketpair: %s\n", std::strerror(errno));
        return 1;
    }
    // Small kernel buffers so the O_NONBLOCK writer backlogs in userspace and the
    // drain proceeds a few KB per flush — the shape the cursor exists for.
    int smallBuffer = 8192;
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &smallBuffer, sizeof(smallBuffer));
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &smallBuffer, sizeof(smallBuffer));
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);

    const std::size_t clientIndex = FlushClientCursorAccess::addClient(d, UnixFileDescriptor(sv[0]));

    // (a)+(b)+(c): feed 4 MiB in 64 KiB chunks, flushing after each (the kernel
    // accepts only the first few KB; the rest backlogs), then pump it out through
    // a slow 4 KB-per-iteration reader.
    static constexpr std::size_t totalBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t feedChunk = 64u * 1024u;
    std::vector<uint8_t> chunk(feedChunk);
    for (std::size_t offset = 0; offset < totalBytes; offset += feedChunk) {
        for (std::size_t i = 0; i < feedChunk; ++i)
            chunk[i] = patternByte(offset + i);
        FlushClientCursorAccess::queueToClient(d, clientIndex, chunk.data(), feedChunk);
        FlushClientCursorAccess::flushClient(d, clientIndex);
    }
    check(!FlushClientCursorAccess::didOverflow(d, clientIndex), "a 4 MiB backlog stays under the high-water");

    const std::size_t floor = FlushClientCursorAccess::compactionFloor();
    std::size_t received = 0;
    bool bytesMatch = true;
    bool compactionBoundHeld = true;
    uint8_t readBuffer[4096];
    // Generous iteration cap: a wedged drain must fail, not hang.
    for (std::size_t iterations = 0; received < totalBytes && iterations < 8u * (totalBytes / sizeof(readBuffer)); ++iterations) {
        ssize_t n = ::read(sv[1], readBuffer, sizeof(readBuffer));
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i)
                if (readBuffer[i] != patternByte(received + static_cast<std::size_t>(i)))
                    bytesMatch = false;
            received += static_cast<std::size_t>(n);
        }
        FlushClientCursorAccess::flushClient(d, clientIndex);
        // The O(n) invariant: the dead prefix never dominates a large buffer.
        const std::size_t consumedNow = FlushClientCursorAccess::consumed(d, clientIndex);
        const std::size_t sizeNow = FlushClientCursorAccess::bufferSize(d, clientIndex);
        if (!(consumedNow < floor || consumedNow * 2 < sizeNow))
            compactionBoundHeld = false;
    }
    check(received == totalBytes, "(a) every queued byte reaches the reader");
    check(bytesMatch, "(a) delivery is byte-exact and ordered across partial drains");
    check(compactionBoundHeld,
        "(b) after every flush the dead prefix obeys the compaction bound "
        "(consumed < floor OR consumed < size/2) — the drain stays O(n)");
    check(FlushClientCursorAccess::bufferSize(d, clientIndex) == 0 && FlushClientCursorAccess::consumed(d, clientIndex) == 0, "(c) a full drain releases the buffer and resets the cursor");

    // (d) accept case: a buffer whose RAW size exceeds the high-water but whose
    // pending tail is small must accept more output — the cap keys on pending.
    // No real socket — a default-constructed owner is already empty.
    const std::size_t stageIndex = FlushClientCursorAccess::addClient(d, UnixFileDescriptor {});
    constexpr std::size_t mebibyte = 1024u * 1024u;
    FlushClientCursorAccess::stageConsumedBuffer(d, stageIndex, clientBufferHighWater + 4u * mebibyte, 12u * mebibyte);
    uint8_t byte = 0x5A;
    std::vector<uint8_t> oneMiB(mebibyte, byte);
    FlushClientCursorAccess::queueToClient(d, stageIndex, oneMiB.data(), oneMiB.size());
    check(!FlushClientCursorAccess::didOverflow(d, stageIndex),
        "(d) a buffer past the high-water whose pending tail is 7 MiB under it is NOT "
        "over the high-water (the cap keys on size - consumed, not raw size)");
    check(FlushClientCursorAccess::pending(d, stageIndex) == clientBufferHighWater - 7u * mebibyte, "(d) the queued bytes append after the pending tail");

    // (d) overflow case: genuinely-pending bytes over the cap overflow, and the
    // reclaim resets both the buffer and the cursor (a stale cursor would skip
    // the next queued frame).
    FlushClientCursorAccess::stageConsumedBuffer(d, stageIndex, clientBufferHighWater + mebibyte, 512u * 1024u);
    FlushClientCursorAccess::queueToClient(d, stageIndex, &byte, 1);
    check(FlushClientCursorAccess::didOverflow(d, stageIndex), "(d) pending bytes over the high-water overflow the client");
    check(FlushClientCursorAccess::bufferSize(d, stageIndex) == 0 && FlushClientCursorAccess::consumed(d, stageIndex) == 0, "(d) the overflow reclaim clears the buffer AND resets the cursor");

    ::close(sv[1]); // ~Daemon's teardownAll closes the client side (sv[0])

    if (failures == 0)
        std::printf("flush_client_cursor: a multi-MiB backlog drains byte-exact through the "
                    "consumed-bytes cursor (O(n), bounded dead prefix), the cursor resets on "
                    "full drain, and the high-water keys on pending bytes\n");
    return failures == 0 ? 0 : 1;
}
