// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_outbound_capacity.cpp — a stalled/malicious QUIC daemon can't
// OOM the frontend: the client's TOTAL outbound backlog (queued-in-send() PLUS
// already-moved-onto-the-relay-stream-but-undrained) is bounded at
// maxOutboundBytes (16 MiB) and fails the connection closed on overflow.
//
// Capping send()'s `outbound` queue alone does not close the OOM. ioLoop moves
// all of `outbound` onto `toSend` and clears `outbound` every i/o tick — so
// under a stalled daemon (writePump can't drain; the receive window never
// opens) with steady, individually-under-cap input (normal typing/paste),
// `outbound` is emptied every tick and never re-trips the per-send guard, while
// `toSend` grows forever. A test that drives send() with no running ioLoop
// exercises only the `outbound` vector, and passes with no backlog bound at
// all. This one drives the same budget check ioLoop calls every tick (via a
// staging seam — no live ngtcp2 connection needed) against `toSend` directly,
// so the backlog-accumulation defect is reachable.
//
// Guards:
//   (a) SINGLE-BURST BOUND — a real send() burst past the limit flags overflow
//       and the outbound queue never grows beyond the limit.
//   (b) BACKLOG BOUND — toSend accumulating past the limit,
//       exactly as ioLoop's move-then-check step would leave it, flags overflow
//       via checkOutboundBudgetOverflowed() — with the `outbound` vector never
//       having gone over cap on any single send().
//   (c) no false-positive on a legit large paste — a single send() at/near the
//       16 MiB limit on a healthy (non-backlogged) connection does not overflow
//       (a 4 MiB limit fails such a paste closed).
//   (d) OVERFLOW-SAFE (INT30-C) — outboundWouldOverflow tests against the
//       limit's remainder, so a near-SIZE_MAX length is caught instead of
//       wrapping pending+length below the limit and reading as "under".
//   (e) exactly one fail-close — checkOutboundBudgetOverflowed() called twice
//       over budget still fires onClose(Failed) only once (reportClose is
//       idempotent via closeReported).
//   (f) CURSOR DRAIN — writePump advances a cursor per packet and the
//       acknowledgement releases whole blocks behind it, so nothing is ever
//       shifted. Drives the REAL cursor step (advanceToSend) over a seeded 4 MiB
//       position-derived pattern, reading the gather ngtcp2 reads per packet:
//       byte-exact ordered delivery, storage bounded by what is retained after
//       every packet, and the buffer released on full acknowledgement.
//   (g) BUDGET KEYS ON RETAINED — the outbound budget counts written but
//       unacknowledged bytes, which ngtcp2 may still resend, as well as unsent
//       ones; only an acknowledgement takes bytes out of the count.
//   (h) STABLE ADDRESSES — appending and acknowledging never move a byte the
//       buffer still holds.
//
// Verify-by-revert: removing the ioLoop's checkOutboundBudgetOverflowed() call
// (or checking `outbound` alone) fails (b).
// Keying the budget on unsent bytes rather than retained ones fails (g); a send
// buffer that moves bytes it holds (a vector that reallocates or shifts) fails (h).
//
// Standalone: compiles QUICClient.cpp directly (its only dependency is ngtcp2 +
// AWS-LC). QUIC-gated. Exit 0 on success, 1 on any failed check.
//

#include "Core/Transport/QUICClient.h"
#include "Platform/POSIX/QUICSendBuffer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <vector>

using Ubiquitty::Transport::Health;
using Ubiquitty::Transport::OutboundCapacityAccess;
using Ubiquitty::Transport::QUICClient;

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Position-derived byte: any drop, duplication, or reorder anywhere in the
// multi-MiB drain mismatches at the exact offset.
static std::uint8_t patternByte(std::size_t i) { return static_cast<std::uint8_t>((i * 131u) ^ (i >> 8)); }

int main()
{
    constexpr std::size_t limit = QUICClient::maxOutboundBytes;

    // Image admission projects fixed-block allocations, not logical pending
    // bytes. A partially acknowledged block cannot reuse its prefix, while a
    // spare block and the last block's suffix can be reused without allocation.
    {
        Ubiquitty::POSIX::QUICSendBuffer buffer;
        constexpr auto block = Ubiquitty::POSIX::QUICSendBuffer::blockBytes;
        check(buffer.additionalStorageBytes(0) == 0, "empty append needs no block");
        check(buffer.additionalStorageBytes(1) == block, "first byte reserves one full fixed block");
        check(buffer.additionalStorageBytes(block + 1) == 2 * block, "a block boundary reserves the next full block");
        check(buffer.additionalStorageBytes(std::numeric_limits<std::size_t>::max()) == std::numeric_limits<std::size_t>::max(),
            "unrepresentable rounded capacity saturates instead of wrapping");
        std::vector<std::uint8_t> bytes(block + 1, 0x5a);
        buffer.append(std::span(bytes).first(block - 1));
        buffer.markWritten(block - 1);
        buffer.acknowledge(block - 2);
        check(buffer.retainedBytes() == 1 && buffer.storageBytes() == block, "a tiny unacknowledged suffix retains its complete block");
        check(buffer.additionalStorageBytes(1) == 0 && buffer.additionalStorageBytes(2) == block,
            "an acknowledged prefix is not available for append");
        const auto before = buffer.storageBytes();
        const auto projected = buffer.additionalStorageBytes(2);
        buffer.append(std::span(bytes).first(2));
        check(buffer.storageBytes() - before == projected, "projected growth equals the actual block allocation");
        buffer.markWritten(2);
        buffer.acknowledge(block);
        check(buffer.storageBytes() == 2 * block && buffer.retainedBytes() == 1, "one live block and one spare remain charged");
        check(buffer.additionalStorageBytes(2 * block - 1) == 0 && buffer.additionalStorageBytes(2 * block) == block,
            "the spare and live suffix are both reusable but still charged");
        buffer.acknowledge(block + 1);
        check(buffer.retainedBytes() == 0 && buffer.storageBytes() == block, "full acknowledgement keeps only the real spare allocation");
        check(buffer.additionalStorageBytes(block) == 0 && buffer.additionalStorageBytes(block + 1) == block,
            "a drained buffer reuses its spare before allocating");
    }

    // (a) SINGLE-BURST BOUND — drive the real send() past the limit (no start():
    // send() only appends under the outbound lock, so no network/handshake is
    // needed). 256 KiB frames; limit/256KiB + a few more frames comfortably
    // exceeds the 16 MiB limit regardless of its exact value.
    {
        QUICClient client;
        std::vector<std::uint8_t> chunk(256u * 1024u, 0xAB);
        const int framesToExceed = static_cast<int>(limit / chunk.size()) + 8;
        for (int i = 0; i < framesToExceed; ++i)
            client.send(chunk);
        check(OutboundCapacityAccess::didOverflow(client), "queuing past the limit in outbound (single burst) flags overflow");
        check(OutboundCapacityAccess::queuedBytes(client) <= limit, "the outbound queue never grows beyond maxOutboundBytes");
    }

    // A well-behaved sender under the limit is untouched.
    {
        QUICClient client;
        std::vector<std::uint8_t> chunk(1024u * 1024u, 0xCD); // 1 MiB
        client.send(chunk);
        client.send(chunk);
        check(!OutboundCapacityAccess::didOverflow(client), "a sender under the limit does not overflow");
        check(OutboundCapacityAccess::queuedBytes(client) == 2u * 1024u * 1024u, "under-limit bytes queue intact");
    }

    // (b) BACKLOG BOUND. Simulate ioLoop's move-then-check step
    // directly: stage the relay-stream backlog (toSend) — not outbound — to a
    // size no single send() ever produced, and assert the same check ioLoop
    // runs every tick catches it.
    {
        QUICClient client;
        // Just under budget: many moderate sends (well under any per-send guard)
        // accumulated onto the backlog by repeated ioLoop ticks that never drained
        // (a stalled daemon). No single send() here is anywhere near the limit.
        OutboundCapacityAccess::stageBacklogBytes(client, limit - 1);
        check(!OutboundCapacityAccess::checkBudget(client), "a backlog just under the limit does not overflow");
        check(!OutboundCapacityAccess::didOverflow(client), "the outbound-burst flag is untouched by the backlog path (independent bounds)");

        // One more tick's worth of moved bytes pushes the backlog over — exactly
        // the sustained-moderate-input scenario.
        OutboundCapacityAccess::stageBacklogBytes(client, limit + 4096);
        check(OutboundCapacityAccess::checkBudget(client), "a backlog over the limit overflows — the toSend accumulation defect is closed");
    }

    // (c) no false-positive — a single legit large paste at the (resized) 16 MiB
    // limit does not fail-close a healthy connection.
    {
        QUICClient client;
        std::vector<std::uint8_t> chunk(limit, 0xEF); // exactly the limit, one send
        client.send(chunk);
        check(!OutboundCapacityAccess::didOverflow(client), "a single send() exactly at the 16 MiB limit does not overflow (no false-positive)");
        check(OutboundCapacityAccess::queuedBytes(client) == limit, "the full legit paste is queued, not dropped");
    }

    // (d) OVERFLOW-SAFE (INT30-C) — the pure predicate against adversarial lengths.
    check(!QUICClient::outboundWouldOverflow(0, limit), "exactly the limit fits (pending 0)");
    check(QUICClient::outboundWouldOverflow(0, limit + 1), "one byte past the limit overflows");
    check(QUICClient::outboundWouldOverflow(limit, 1), "a full queue overflows on any further byte");
    // The wrap case: pending near the limit, length near SIZE_MAX. A naive
    // pending+length wraps below the limit and would read as "fits"; the
    // remainder test must still catch it.
    check(QUICClient::outboundWouldOverflow(limit - 1, std::numeric_limits<std::size_t>::max()), "near-SIZE_MAX length is caught, not wrapped (INT30-C)");
    check(QUICClient::outboundWouldOverflow(1, std::numeric_limits<std::size_t>::max() - 1), "SIZE_MAX-class length with small pending overflows");

    // (e) exactly one fail-close — checkBudget's reportClose is idempotent:
    // calling it again after an overflow (ioLoop would tick again before
    // actually tearing down) must not fire onClose a second time.
    {
        std::vector<std::pair<Health, std::string>> closes;
        QUICClient client;
        OutboundCapacityAccess::installOnCloseForTest(client, [&closes](Health health, const std::string& reason) { closes.emplace_back(health, reason); });
        OutboundCapacityAccess::stageBacklogBytes(client, limit + 1);
        check(OutboundCapacityAccess::checkBudget(client), "first over-budget check overflows");
        check(OutboundCapacityAccess::checkBudget(client), "second over-budget check still reports overflowed");
        check(closes.size() == 1, "onClose(Failed) fires exactly once, not once per tick");
        check(!closes.empty() && closes[0].first == Health::Failed, "the fail-close health is Failed, not Closed");
    }

    // (f) CURSOR DRAIN — drive the REAL per-packet cursor step over a
    // seeded multi-MiB backlog and assert byte-exact ordered delivery, that the
    // storage held stays bounded by what is retained as acknowledgements arrive,
    // and that acknowledging everything releases it. The pending window is exactly
    // the gather ngtcp2 reads into each packet; advanceToSend is the same cursor
    // step writePump runs.
    {
        QUICClient client;
        constexpr std::size_t totalBytes = 4u * 1024u * 1024u;
        std::vector<std::uint8_t> payload(totalBytes);
        for (std::size_t i = 0; i < totalBytes; ++i)
            payload[i] = patternByte(i);
        OutboundCapacityAccess::seedToSend(client, payload);

        const std::size_t block = OutboundCapacityAccess::blockBytes();
        constexpr std::size_t packetStreamBytes = 1400; // ~ one QUIC packet's stream payload
        std::size_t drained = 0;
        bool bytesMatch = true;
        bool didStorageStayBounded = true;
        std::size_t iterations = 0;
        const std::size_t iterationCap = 8u * (totalBytes / packetStreamBytes + 1);
        // A write does not release: ngtcp2 retransmits from these bytes, so the
        // peer's acknowledgement is what frees them. The loop therefore drives both
        // events — write a packet, then acknowledge the one before it, which is the
        // ordering a real connection produces.
        while (OutboundCapacityAccess::pendingBacklogBytes(client) > 0 && iterations < iterationCap) {
            ++iterations;
            const std::size_t pending = OutboundCapacityAccess::pendingBacklogBytes(client);
            const std::size_t take = pending < packetStreamBytes ? pending : packetStreamBytes;
            const std::vector<std::uint8_t> window = OutboundCapacityAccess::toSendWindow(client, take);
            if (window.size() != take)
                bytesMatch = false;
            for (std::size_t i = 0; i < window.size(); ++i)
                if (window[i] != patternByte(drained + i))
                    bytesMatch = false;
            drained += take;
            OutboundCapacityAccess::advanceToSend(client, take);
            if (drained > packetStreamBytes) // one packet in flight, as a peer would
                OutboundCapacityAccess::ackThrough(client, drained - packetStreamBytes);
            // What is acknowledged is released a block at a time: the storage held
            // never exceeds what is retained by more than the partly acknowledged
            // block at the front, the partly filled one at the back, and the spare.
            if (OutboundCapacityAccess::storageBytes(client) > OutboundCapacityAccess::retainedBacklogBytes(client) + 3 * block)
                didStorageStayBounded = false;
        }
        check(drained == totalBytes, "(f) every seeded byte drains through the cursor window");
        check(bytesMatch, "(f) the drained window is byte-exact and ordered across packets and blocks");
        check(didStorageStayBounded, "(f) acknowledged bytes are released as they are acknowledged, a block at a time");
        // The last packet is still unacknowledged, which is exactly the point: it is
        // what a retransmission would need. Acknowledging it releases the buffer.
        check(OutboundCapacityAccess::retainedBacklogBytes(client) == packetStreamBytes,
            "(f) the written-but-unacknowledged tail is still held");
        OutboundCapacityAccess::ackThrough(client, totalBytes);
        check(OutboundCapacityAccess::retainedBacklogBytes(client) == 0 && OutboundCapacityAccess::storageBytes(client) <= block,
            "(f) acknowledging everything releases the buffer, keeping at most one spare block");
    }

    // (h) a queued byte never moves. ngtcp2 keeps pointers to the bytes it takes
    // and rebuilds a lost packet from them, so appending more, or releasing an
    // acknowledged prefix, must leave every unacknowledged byte where it was.
    {
        QUICClient client;
        std::vector<std::uint8_t> payload(64u * 1024u);
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = patternByte(i);
        OutboundCapacityAccess::seedToSend(client, payload);
        OutboundCapacityAccess::advanceToSend(client, 20000);
        const std::uint8_t* written = OutboundCapacityAccess::unsentFrontAddress(client) - 1; // the last byte handed over
        std::vector<std::uint8_t> more(8u * 1024u * 1024u, 0x5A);
        OutboundCapacityAccess::seedToSend(client, more);
        OutboundCapacityAccess::ackThrough(client, 19000);
        check(OutboundCapacityAccess::unsentFrontAddress(client) == written + 1 && *written == patternByte(19999),
            "(h) appending and acknowledging leave an in-flight byte at its address");
    }

    // (g) budget keys on the unacknowledged obligation — written is not
    // finished: ngtcp2 retransmits from bytes it has taken, so they are memory the
    // client is still holding and the budget counts them. Only an acknowledgement
    // takes bytes out of the count.
    {
        // 20 MiB written but unacknowledged is over the 16 MiB budget: all of it is
        // still needed. Counting only the unwritten 8 MiB would pass.
        QUICClient client;
        OutboundCapacityAccess::stageConsumedBacklog(client, 20u * 1024u * 1024u, 12u * 1024u * 1024u);
        check(OutboundCapacityAccess::pendingBacklogBytes(client) == 8u * 1024u * 1024u,
            "(g) pending (what is left to write) still excludes the written prefix");
        check(OutboundCapacityAccess::retainedBacklogBytes(client) == 20u * 1024u * 1024u,
            "(g) retained counts the written-but-unacknowledged bytes as well");
        check(OutboundCapacityAccess::checkBudget(client),
            "(g) 20 MiB held with nothing acknowledged is OVER the 16 MiB budget");

        // Acknowledging the written prefix is what brings it under.
        QUICClient acked;
        OutboundCapacityAccess::stageConsumedBacklog(acked, 20u * 1024u * 1024u, 12u * 1024u * 1024u);
        OutboundCapacityAccess::ackThrough(acked, 12u * 1024u * 1024u);
        check(OutboundCapacityAccess::retainedBacklogBytes(acked) == 8u * 1024u * 1024u,
            "(g) an acknowledgement releases the obligation");
        check(!OutboundCapacityAccess::checkBudget(acked),
            "(g) 8 MiB unacknowledged is under the 16 MiB budget");
        check(!OutboundCapacityAccess::didOverflow(acked), "(g) the outbound-burst flag is untouched by the backlog path");
    }
    {
        // Genuinely-pending bytes over the budget still fail-close: 17 MiB - 512 KiB
        // consumed = 16.5 MiB pending > 16 MiB.
        QUICClient client;
        OutboundCapacityAccess::stageConsumedBacklog(client, 17u * 1024u * 1024u, 512u * 1024u);
        check(OutboundCapacityAccess::checkBudget(client), "(g) pending over the 16 MiB budget still overflows despite the dead prefix");
    }

    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: quic_outbound_capacity\n");
    return 0;
}
