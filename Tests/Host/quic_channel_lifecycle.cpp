// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_channel_lifecycle.cpp — QUICChannel's client-wrapper lifetime
// seam, driven without a daemon.
//
// QUICChannel (Source/Core/Transport/QUICChannel.cpp) is a Channel over
// Transport::QUICClient. Every other quic_* test here drives ngtcp2 or the daemon;
// none drives the Core wrapper, so its own logic — the cross-thread ref-swap under
// m_clientMutex and the drop-when-no-client guards — is covered here alone. A channel
// that never connects exercises exactly that logic: no QUICClient is ever created,
// so each call below is answered by QUICChannel, not by QUICClient. start() (which
// dials a real host) stays with quic_loopback / quic_daemon_accept.
//
// Built only under -DUBIQUITTY_QUIC. Exit 0 on success.
//

#include "Core/Transport/QUICChannel.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#if !defined(UBIQUITTY_QUIC)
// Built without the QUIC stack: nothing to test. Exit 0 so the stanza is a clean skip.
int main()
{
    std::puts("quic_channel_lifecycle: UBIQUITTY_QUIC not defined — skipped");
    return 0;
}
#else

using Ubiquitty::Transport::Health;
using Ubiquitty::Transport::Origin;
using Ubiquitty::Transport::OriginKind;
using Ubiquitty::Transport::QUICChannel;
using Ubiquitty::Transport::TransportStats;

static int failures = 0;
#define MUST(cond)                                                               \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main()
{
    // A channel that never connects: no QUICClient is ever created, so every call
    // below exercises QUICChannel's own down-state logic, not QUICClient's.
    QUICChannel ch("example.test", 4433, "token", "deadbeefsha", Origin { OriginKind::Daemon, "example.test" });

    // origin(): returned verbatim from what was stamped at construction.
    MUST(ch.origin().transport == OriginKind::Daemon);
    MUST(ch.origin().host == "example.test");
    MUST(ch.origin().isRemote());

    // health(): Idle before start() — no client, no connect attempt.
    MUST(ch.health() == Health::Idle);

    // description(): "quic host:port", derived from constructor fields only.
    MUST(ch.description() == "quic example.test:4433");

    // queryStats(): no client up, so nothing to report — false, and `out` untouched.
    TransportStats stats;
    MUST(ch.queryStats(stats) == false);
    MUST(stats.valid == false);

    // send() before start(): loadClient() is null, so the bytes are silently
    // dropped rather than dereferencing a null client.
    const std::uint8_t payload[] = { 0x01, 0x02, 0x03 };
    ch.send(std::span<const std::uint8_t>(payload, sizeof payload));

    // stop() before start(): no client to tear down; health stays Idle because it
    // is neither Connecting nor Connected.
    ch.stop();
    MUST(ch.health() == Health::Idle);

    // Idempotent stop() and a send() after stop(): still no client, still safe.
    ch.stop();
    ch.send(std::span<const std::uint8_t>(payload, sizeof payload));
    MUST(ch.health() == Health::Idle);

    // ~QUICChannel runs stop() a final time on a null client at scope exit.
    if (failures) {
        std::fprintf(stderr, "quic_channel_lifecycle: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("quic_channel_lifecycle: ok");
    return 0;
}
#endif
