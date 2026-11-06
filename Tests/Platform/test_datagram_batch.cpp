// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_datagram_batch — several datagrams per call, both directions.
//
// The QUIC client is the bulk receiver, and it read one datagram per syscall;
// the host is the bulk sender, and it wrote one per syscall.
// The fill this covers takes a whole array at once where the platform offers
// it, so a keyframe burst crosses into the kernel a fraction as often.
//
// The claim is checked rather than assumed. A loopback echo has one datagram in
// flight at a time and would batch nothing, so it proves the code runs and not
// that it batches: these cases queue the datagrams FIRST and then read, which
// is the shape a burst actually has.
//
// What must hold on every platform is that the caller cannot tell which fill it
// got — same datagrams, same order, same lengths, same drained signal. The
// count per call is where they differ, and only Linux is asked for more than
// one.

#include "Harness.h"

#include "Platform/POSIX/DatagramBatch.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <span>
#include <string>
#include <vector>

using Ubiquitty::POSIX::DatagramBatch;
using Ubiquitty::POSIX::datagramBatchSize;
using Ubiquitty::POSIX::fillDatagramBatch;

namespace {

// A bound, non-blocking UDP socket on the loopback, and its address.
struct BoundSocket {
    int descriptor = -1;
    sockaddr_in address {};
};

bool openBound(BoundSocket& out)
{
    out.descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (out.descriptor < 0)
        return false;
    sockaddr_in wanted {};
    wanted.sin_family = AF_INET;
    wanted.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    wanted.sin_port = 0; // the kernel picks
    if (::bind(out.descriptor, reinterpret_cast<sockaddr*>(&wanted), sizeof(wanted)) != 0)
        return false;
    socklen_t length = sizeof(out.address);
    if (::getsockname(out.descriptor, reinterpret_cast<sockaddr*>(&out.address), &length) != 0)
        return false;
    // Non-blocking, which is what makes "nothing left" an answer rather than a
    // wait. The fill's drained signal depends on it.
    return ::fcntl(out.descriptor, F_SETFL, ::fcntl(out.descriptor, F_GETFL, 0) | O_NONBLOCK) == 0;
}

} // namespace

TEST(datagramBatchReadsWhatWasQueued)
{
    BoundSocket receiver;
    CHECK(openBound(receiver));
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sender >= 0);

    // Queued before any read, which is what a burst looks like and what a
    // ping-pong never does.
    constexpr int queued = 8;
    std::vector<std::string> sent;
    for (int index = 0; index < queued; ++index) {
        // Distinct lengths as well as contents: a fill that returned the right
        // bytes with the wrong length would still hand a truncated packet to
        // the decoder, where it fails authentication and reads as a lossy link.
        std::string payload(static_cast<std::size_t>(16 + index * 7), static_cast<char>('a' + index));
        const ssize_t wrote = ::sendto(sender, payload.data(), payload.size(), 0,
            reinterpret_cast<const sockaddr*>(&receiver.address), sizeof(receiver.address));
        CHECK(wrote == static_cast<ssize_t>(payload.size()));
        sent.push_back(std::move(payload));
    }

    // Drain through the fill, however many calls that takes.
    std::vector<std::string> received;
    int fills = 0;
    for (int attempt = 0; attempt < 64 && static_cast<int>(received.size()) < queued; ++attempt) {
        DatagramBatch batch;
        const int count = fillDatagramBatch(receiver.descriptor, batch);
        if (count < 0)
            continue; // the send has not landed yet; loopback is not instant
        ++fills;
        CHECK(count > 0); // zero would leave a caller's drain loop with two exits
        CHECK(count <= datagramBatchSize);
        for (int slot = 0; slot < count; ++slot)
            received.emplace_back(reinterpret_cast<const char*>(batch.storage[slot].data()), batch.length[slot]);
    }

    // Every datagram, in order, whole.
    CHECK(received.size() == sent.size());
    for (std::size_t index = 0; index < received.size() && index < sent.size(); ++index) {
        CHECK(received[index].size() == sent[index].size());
        CHECK(received[index] == sent[index]);
    }

    // Never more calls than datagrams, on any platform: a fill that returned
    // nothing while leaving data queued would loop a caller forever.
    CHECK(fills > 0);
    CHECK(fills <= queued);
#if defined(__linux__)
    // The point of the change: eight queued datagrams did not cost eight calls.
    CHECK_MSG(fills < queued, "eight queued datagrams took a call each, so nothing is being batched");
#endif

    // A drained socket says so, and keeps saying so.
    {
        DatagramBatch batch;
        CHECK(fillDatagramBatch(receiver.descriptor, batch) < 0);
        CHECK(fillDatagramBatch(receiver.descriptor, batch) < 0);
    }

    ::close(sender);
    ::close(receiver.descriptor);
}

TEST(datagramSendBatchDeliversWhatWasQueued)
{
    // The mirror of the case above, on the direction the HOST is bulk in: a
    // daemon streaming a snapshot writes the packets the client reads.
    BoundSocket receiver;
    if (!openBound(receiver)) {
        CHECK_MSG(false, "no loopback UDP socket");
        return;
    }
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sender >= 0);
    if (sender < 0) {
        ::close(receiver.descriptor);
        return;
    }

    constexpr int queued = 8;
    std::vector<std::string> sent;
    Ubiquitty::POSIX::DatagramSendBatch batch;
    for (int index = 0; index < queued; ++index) {
        // Distinct lengths as well as contents: a send that carried the right
        // bytes with a wrong length would deliver a truncated packet, which a
        // QUIC peer drops as a failed authentication rather than reporting.
        const std::string payload = "send-" + std::to_string(index) + std::string(static_cast<std::size_t>(index) * 7, 'x');
        sent.push_back(payload);
        const bool didQueue = Ubiquitty::POSIX::queueDatagram(batch,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()),
            reinterpret_cast<const sockaddr*>(&receiver.address), sizeof(receiver.address));
        CHECK(didQueue);
    }
    CHECK(batch.count == queued);

    const int accepted = Ubiquitty::POSIX::flushDatagramBatch(sender, batch);
    CHECK(accepted == queued);
    // Emptied whatever happened, or a caller flushing on full and again at the
    // end would send the same packets twice.
    CHECK(batch.count == 0);
    CHECK(Ubiquitty::POSIX::flushDatagramBatch(sender, batch) == 0);

    // Every packet arrived, in order, whole.
    std::vector<std::string> received;
    for (int attempt = 0; attempt < 64 && static_cast<int>(received.size()) < queued; ++attempt) {
        DatagramBatch inbound;
        const int count = fillDatagramBatch(receiver.descriptor, inbound);
        if (count < 0)
            continue;
        for (int slot = 0; slot < count; ++slot)
            received.emplace_back(reinterpret_cast<const char*>(inbound.storage[slot].data()), inbound.length[slot]);
    }
    CHECK(received.size() == sent.size());
    for (std::size_t index = 0; index < received.size() && index < sent.size(); ++index) {
        CHECK(received[index].size() == sent[index].size());
        CHECK(received[index] == sent[index]);
    }

    // What a full batch refuses, so a caller that ignores the answer cannot
    // silently drop a packet it believed was queued.
    {
        Ubiquitty::POSIX::DatagramSendBatch full;
        const std::string one = "x";
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(one.data()), one.size());
        const sockaddr* destination = reinterpret_cast<const sockaddr*>(&receiver.address);
        for (int index = 0; index < datagramBatchSize; ++index)
            CHECK(Ubiquitty::POSIX::queueDatagram(full, bytes, destination, sizeof(receiver.address)));
        CHECK(full.isFull());
        CHECK(!Ubiquitty::POSIX::queueDatagram(full, bytes, destination, sizeof(receiver.address)));

        Ubiquitty::POSIX::DatagramSendBatch other;
        // A packet too large for a slot is refused rather than truncated: a
        // truncated QUIC packet fails authentication at the peer, which reads
        // as a lossy link rather than as a buffer that was too small.
        const std::vector<std::uint8_t> oversized(Ubiquitty::POSIX::datagramSlotCapacity + 1, 0x5A);
        CHECK(!Ubiquitty::POSIX::queueDatagram(other, oversized, destination, sizeof(receiver.address)));
        CHECK(!Ubiquitty::POSIX::queueDatagram(other, bytes, nullptr, sizeof(receiver.address)));
        CHECK(other.count == 0);
    }

    ::close(sender);
    ::close(receiver.descriptor);
}

UBIQUITTY_TEST_MAIN()
