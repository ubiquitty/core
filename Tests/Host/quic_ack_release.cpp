// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_ack_release.cpp — outbound stream bytes are released when the
// peer acknowledges them, and not when they are handed to ngtcp2.
//
// ngtcp2 does not copy stream data. ngtcp2_conn_writev_stream's contract:
//
//   The caller must keep the portion of data covered by |*pdatalen| bytes in
//   tact until ngtcp2_callbacks.acked_stream_data_offset indicates that they
//   are acknowledged by a remote endpoint or the stream is closed.
//
// A lost packet is rebuilt from that buffer. Releasing on the write instead
// lets a retransmission re-read bytes the next append has already overwritten,
// so the peer receives a STREAM frame whose payload is not what that offset
// first carried — silent corruption of the Sync stream, and only under packet
// loss, which is when it is least affordable.
//
// Why this shape rather than an end-to-end test: a retransmission needs loss,
// and the loopback harness has none. So this drives the two events directly —
// a write, then an acknowledgement — and asserts what the buffer retains
// between them. That is the property the contract states, and it is the one a
// regression would break.
//

#include "Platform/Host/HostQUIC.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "quic_ack_release: FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main()
{
    using namespace Ubiquitty::Host;

    // (a) Queuing more does not release what was already written. This is the arm
    // that fails without the retention rule: a sendStream erasing the written
    // prefix before appending, on the reasoning that written means done with,
    // loses the bytes a retransmission needs the moment anything else
    // was queued.
    {
        QUICListener listener;
        QUICConnection* connection = makeTestQUICConnection();
        check(connection != nullptr, "(a) test connection allocates");
        if (!connection)
            return 1;

        const std::size_t written = 4096;
        // toSendConsumed == size is the state right after writev_stream took every
        // queued byte: all of it written, none of it acknowledged.
        setTestQUICConnectionPending(connection, /*toSendSize*/ written, /*toSendConsumed*/ written);

        const std::vector<std::uint8_t> more(500, 0xAB);
        listener.sendStream(connection, { more.data(), more.size() });

        check(testRetainedSendBytes(connection) == written + more.size(),
            "(a) queuing more keeps the written-but-unacknowledged bytes — ngtcp2 "
            "retransmits from them");

        destroyTestQUICConnection(connection);
    }

    // (b) An acknowledgement releases exactly its range, and no more.
    {
        QUICConnection* connection = makeTestQUICConnection();
        check(connection != nullptr, "(b) test connection allocates");
        if (!connection)
            return 1;

        const std::size_t bytes = 1000;
        setTestQUICConnectionPending(connection, /*toSendSize*/ bytes, /*toSendConsumed*/ bytes);

        testAckStreamData(connection, /*offset*/ 0, /*datalen*/ 400);
        check(testRetainedSendBytes(connection) == 600,
            "(b) a partial acknowledgement releases only the acknowledged prefix");

        testAckStreamData(connection, /*offset*/ 400, /*datalen*/ 600);
        check(testRetainedSendBytes(connection) == 0,
            "(b) acknowledging the rest releases the rest");

        destroyTestQUICConnection(connection);
    }

    // (c) A duplicate or already-released range is ignored rather than eating
    // live bytes. ngtcp2 promises increasing, non-overlapping ranges, so this is
    // about staying correct if that ever fails to hold rather than about a
    // reachable case.
    {
        QUICConnection* connection = makeTestQUICConnection();
        check(connection != nullptr, "(c) test connection allocates");
        if (!connection)
            return 1;

        setTestQUICConnectionPending(connection, /*toSendSize*/ 500, /*toSendConsumed*/ 500);
        testAckStreamData(connection, 0, 500);
        check(testRetainedSendBytes(connection) == 0, "(c) the whole range releases");

        // A repeat of a range already released must not underflow into whatever
        // was appended after it.
        setTestQUICConnectionPending(connection, /*toSendSize*/ 300, /*toSendConsumed*/ 0);
        testAckStreamData(connection, 0, 500);
        check(testRetainedSendBytes(connection) == 300,
            "(c) a stale acknowledgement does not release bytes queued after it");

        destroyTestQUICConnection(connection);
    }

    // (d) The pre-auth deadline. A peer that finishes the handshake and never
    // authenticates can hold ngtcp2's reassembly buffer by sending stream data
    // above a gap: the bytes never reach recv_stream_data, so the daemon sees no
    // byte, creates no client, and its no-HELLO reaper never considers it. The
    // only other sweep is ngtcp2's idle expiry at 90 s, which is that long so an
    // authenticated client survives a tunnel outage.
    {
        check(testPreAuthReapSurvives(/*ageMilliseconds*/ 1000, /*authenticated*/ false),
            "(d) an unauthenticated peer inside the grace window survives");
        check(!testPreAuthReapSurvives(/*ageMilliseconds*/ 30000, /*authenticated*/ false),
            "(d) an unauthenticated peer past the grace window is reaped");
        check(testPreAuthReapSurvives(/*ageMilliseconds*/ 30000, /*authenticated*/ true),
            "(d) an AUTHENTICATED peer keeps the full idle window — the deadline is "
            "about not having said who you are, not about being quiet");
    }

    if (failures == 0)
        std::printf("quic_ack_release: outbound stream bytes are retained until acknowledged, "
                    "a partial ack releases only its prefix, a stale ack releases nothing, and an\n"
                    "unauthenticated peer is reaped on its own deadline\n");
    return failures == 0 ? 0 : 1;
}
