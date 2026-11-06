// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/aggregate_outbound_capacity.cpp — the daemon-wide aggregate output
// cap bounds the total queued output across all clients, dropping newest-first.
//
// The per-client high-water (clientBufferHighWater) alone admits N of them
// across N clients — a many-client memory exhaustion below the per-client
// limit. enforceOutboundBudget sums each client's queued output (outputBuffer,
// plus the QUIC stream/datagram backlog when QUIC is built in) and drops the
// NEWEST clients until the total is back under maxAggregateOutboundBytes (4 x
// the per-client high-water). This AF_UNIX-only test exercises the outputBuffer
// summand; the QUIC addend is #ifdef'd out at QUIC-OFF.
//
// This befriends the Daemon to stage N clients each parked just under the
// per-client cap (so none individually overflows), then drives the PRIVATE
// enforceOutboundBudget and reads back c.didOverflow:
//   - the total (5 x 7/8 of the per-client cap) exceeds the aggregate cap;
//   - only the NEWEST client is dropped (4 x 7/8 <= the cap);
//   - the older clients, within the aggregate, are KEPT.
//
// Verify-by-revert: drop the `enforceOutboundBudget()` call and all 5 clients
// stay queued — assertion (a) fails.
//
// AF_UNIX clients only (the cap sums outputBuffer for these); enforceOutboundBudget is
// QUIC-agnostic, so this links the QUIC-OFF host objects (HostQUIC.cpp is an empty TU
// there) — no ngtcp2/AWS-LC needed, same as drop_defer / origin_staleness. Exit 0 on
// success.
//

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/OutboundCapacity.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace Ubiquitty {
namespace Host {

// The daemon's own outbound caps (Platform/Host/OutboundCapacity.h).
static constexpr std::size_t maxAggregateOutbound = maxAggregateOutboundBytes;

// Friend seam: stage plain (AF_UNIX) clients with a chosen output footprint, drive
// the private aggregate enforcement, and read back the per-client overflow flag.
struct AggregateOutboundCapacityAccess {
    static std::size_t addClient(Daemon& d)
    {
        Daemon::Client c; // no real socket — the default-constructed UnixFileDescriptor is already empty (-1)
        c.clientID = d.m_nextClientID++;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }
    static void setOutputBufferSize(Daemon& d, std::size_t index, std::size_t bytes)
    {
        d.m_clients[index].outputBuffer.assign(bytes, 0u); // a real allocation the cap sums
    }
    static void enforceOutboundBudget(Daemon& d) { d.enforceOutboundBudget(); }
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static void prepareImageBudgets(Daemon& d, std::size_t index)
    {
        if (!d.m_imageSourceBudget)
            d.m_imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(16u * 1024u * 1024u);
        auto& client = d.m_clients[index];
        client.imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(5u * 1024u * 1024u);
        client.syncConnection = std::make_unique<Protocol::Sync::HostConnection>(Protocol::Sync::measuredHostPolicy(0), Protocol::Sync::HostIdentity {});
        // The canonical carrier retains spare vector capacity as well as its
        // already-sent prefix. There is no separate outer Catalog buffer.
        client.outputBuffer.reserve(3u * 1024u * 1024u + 512u * 1024u);
    }
    static Protocol::Sync::BulkSourceAdmission imageAdmission(const Daemon& d, std::size_t index)
    {
        return d.imageSourceAdmission(d.m_clients[index].clientID);
    }
    static std::size_t fullPayloadCapacity(const Daemon& d, std::size_t index) { return d.outboundPayloadCapacity(d.m_clients[index]); }
    static std::size_t pendingBytes(const Daemon& d, std::size_t index) { return d.outboundPendingBytes(d.m_clients[index]); }
    static void markOutputSentExceptOne(Daemon& d, std::size_t index)
    {
        auto& client = d.m_clients[index];
        client.outputConsumed = client.outputBuffer.size() - 1;
    }
    static void finishOutput(Daemon& d, std::size_t index) { d.m_clients[index].clearPendingOutput(); }
    static void releaseIdleCarrierCapacity(Daemon& d) { d.releaseIdleImageCarrierCapacity(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::AggregateOutboundCapacityAccess;
using Ubiquitty::Host::clientBufferHighWater;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::maxAggregateOutbound;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "aggregate_outbound_capacity: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    Daemon d;

    // 5 clients, each parked at seven eighths of the per-client cap, so none
    // individually overflows. Together they hold 35/8 of the cap, past the
    // aggregate's 4; without the newest they hold 28/8, under it.
    static constexpr std::size_t perClient = clientBufferHighWater / 8u * 7u;
    constexpr int clients = 5;
    static_assert(perClient < clientBufferHighWater, "per-client stays under the per-client high-water");
    static_assert(static_cast<std::size_t>(clients) * perClient > maxAggregateOutbound, "the staged total must exceed the aggregate cap");

    std::vector<std::size_t> idx;
    for (int i = 0; i < clients; ++i) {
        std::size_t client = AggregateOutboundCapacityAccess::addClient(d);
        AggregateOutboundCapacityAccess::setOutputBufferSize(d, client, perClient);
        idx.push_back(client);
    }

    AggregateOutboundCapacityAccess::enforceOutboundBudget(d);

    static_assert(static_cast<std::size_t>(clients - 1) * perClient <= maxAggregateOutbound, "without the newest the total is within the aggregate cap");
    check(AggregateOutboundCapacityAccess::didOverflow(d, idx[4]),
        "(a) the NEWEST client is dropped when the total queued output exceeds the "
        "daemon-wide aggregate cap (the per-client high-water alone admits N of them)");
    check(!AggregateOutboundCapacityAccess::didOverflow(d, idx[0]) && !AggregateOutboundCapacityAccess::didOverflow(d, idx[1]) && !AggregateOutboundCapacityAccess::didOverflow(d, idx[2]) && !AggregateOutboundCapacityAccess::didOverflow(d, idx[3]),
        "(b) the 4 older clients, whose total is within the aggregate cap, are KEPT "
        "(newest-first eviction, not oldest)");

    {
        namespace Sync = Ubiquitty::Protocol::Sync;
        Daemon images;
        const auto first = AggregateOutboundCapacityAccess::addClient(images);
        const auto second = AggregateOutboundCapacityAccess::addClient(images);
        for (const auto index : { first, second }) {
            AggregateOutboundCapacityAccess::prepareImageBudgets(images, index);
            AggregateOutboundCapacityAccess::setOutputBufferSize(images, index, 3u * 1024u * 1024u);
            AggregateOutboundCapacityAccess::markOutputSentExceptOne(images, index);
        }
        const auto capacity = AggregateOutboundCapacityAccess::fullPayloadCapacity(images, first);
        const auto admission = AggregateOutboundCapacityAccess::imageAdmission(images, first);
        check(AggregateOutboundCapacityAccess::pendingBytes(images, first) == 1, "image admission does not alter the transport's one-byte drain progress");
        check(capacity > 3u * 1024u * 1024u + 512u * 1024u, "image admission counts sent prefixes, spare carrier storage and bounded frame room");
        check(admission.otherConnectionBytes == capacity && admission.otherAggregateBytes == 2 * capacity,
            "image source admission includes every client's full payload capacity");
        check(!Sync::BulkSourceReservation::acquire(admission, 2u * 1024u * 1024u),
            "logical drain progress cannot admit pixels into still-owned capacity");
        AggregateOutboundCapacityAccess::finishOutput(images, first);
        check(AggregateOutboundCapacityAccess::fullPayloadCapacity(images, first) == capacity,
            "clearing a vector does not pretend its retained allocation was freed");
        AggregateOutboundCapacityAccess::releaseIdleCarrierCapacity(images);
        check(AggregateOutboundCapacityAccess::fullPayloadCapacity(images, first) < capacity,
            "a truly drained large carrier allocation is released before image retry");
        check(AggregateOutboundCapacityAccess::fullPayloadCapacity(images, second) == capacity,
            "a carrier with one unread byte keeps its entire allocation charged");
        auto source = Sync::BulkSourceReservation::acquire(AggregateOutboundCapacityAccess::imageAdmission(images, first), 2u * 1024u * 1024u);
        check(source.has_value(), "released carrier storage becomes available for a subsequent image source");
        check(admission.connection->heldBytes() == 2u * 1024u * 1024u && admission.aggregate->heldBytes() == 2u * 1024u * 1024u,
            "the newly retained source stays charged in both distinct ledgers");
        source.reset();
        check(admission.connection->heldBytes() == 0 && admission.aggregate->heldBytes() == 0, "source destruction releases both charges exactly once");
    }

    if (failures == 0)
        std::printf("aggregate_outbound_capacity: total queued output across clients is bounded by the "
                    "daemon-wide aggregate cap, dropping newest-first — N clients can't each "
                    "sit at the per-client high-water unbounded\n");
    return failures == 0 ? 0 : 1;
}
