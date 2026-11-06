// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/fuzz_quic_preamble.cpp — libFuzzer harness for the daemon's QUIC
// PRE-AUTH PREAMBLE state machine (Daemon::onQUICStreamData).
//
// A brand-new QUIC peer is unauthenticated: onQUICStreamData accumulates its
// leading bytes in Client::quicPreamble until they validate as a preamble,
// [1][u8 tokenLength][token] or [2][resume proof], authenticates, then
// slices the remainder off and feeds it as Sync frames. Every byte here comes
// from an untrusted peer before any auth, so the reassembly, the pre-insert OOM
// cap, the tokenLength parse, the incomplete-preamble wait, and the rest-slice
// are all reachable pre-auth — exactly the surface a fuzzer should hammer.
//
// The harness seeds one pending token so successful auth is REACHABLE: an input
// shaped [1][len]["fuzzpreambletoken"][sync bytes...] authenticates and drives the
// rest-slice + feedQUICClientFrames path, letting the fuzzer explore past the
// gate as well as every malformed-preamble branch in front of it.
//
// Non-vacuous invariant: after every onQUICStreamData call, while the peer is
// still present, unauthenticated, and not dropped, its pre-auth buffer stays
// within Sync::maxPreambleBytes (preambleCapacity) — the reader refuses
// the over-size chunk rather than accumulating it. Removing that cap trips this
// assertion within seconds.
//
// Built only under -DUBIQUITTY_QUIC=1 (onQUICStreamData is #ifdef-gated).
//

#include "Core/Protocol/Sync/ResumeProof.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"

#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace Ubiquitty {
namespace Host {

// The most a connection that has authenticated nothing is held of.
static constexpr std::size_t preambleCapacity = Protocol::Sync::maxPreambleBytes;

// The one-time token the harness pre-plants; an input leading with
// [1][17]["fuzzpreambletoken"] authenticates. 17 bytes, within the token cap.
static const char* const fuzzToken = "fuzzpreambletoken";

// Friend seam: reach the private client list, pending-token list, QUIC listener,
// and onQUICStreamData, and read back a client's auth/drop state + buffer size.
struct QUICPreambleFuzzAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); } // ephemeral listener → m_quic non-null

    // Reset to a single fresh UNAUTHED QUIC peer bound to `connection`, with one
    // valid pending token so auth is reachable. Called before every input so
    // each run starts from the same pre-auth state.
    static void resetForFuzz(Daemon& d, QUICConnection* connection)
    {
        d.m_clients.clear();
        Daemon::Client client;
        // fileDescriptor stays default-empty (-1): this is a QUIC-only peer with
        // no AF_UNIX socket — PAL::UnixFileDescriptor's default construction covers it.
        client.quicConnection = connection;
        client.quicAuthed = false;
        d.m_clients.push_back(std::move(client));

        d.m_pendingQUICTokens.clear();
        Daemon::PendingQUICToken pending;
        pending.token = fuzzToken;
        // Never-expiring deadline: pruneExpiredQUICTokens drops a token only when
        // nowMilliseconds() >= its deadline, so the max keeps auth reachable every
        // run without depending on the monotonic clock (deterministic steering).
        pending.deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max();
        pending.mintingClientID = 0;
        d.m_pendingQUICTokens.push_back(std::move(pending));
    }

    // Drive the PRIVATE pre-auth stream-data path with one stream fragment.
    static void feedStreamData(Daemon& d, QUICConnection* connection, const std::uint8_t* data, std::size_t length) { d.onQUICStreamData(connection, { data, length }); }

    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
    static bool authed(const Daemon& d) { return d.m_clients[0].quicAuthed; }
    static std::size_t authBufferSize(const Daemon& d) { return d.m_clients[0].quicPreamble.heldBytes(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::preambleCapacity;
using Ubiquitty::Host::QUICConnection;
using Ubiquitty::Host::QUICPreambleFuzzAccess;

// Persistent target reused across inputs (no per-call heap churn): the daemon +
// its live listener + one test connection are built once. gReady stays false if
// the QUIC listener could not start (missing cert env) — every input then skips
// cleanly rather than crashing.
static bool gInitDone = false;
static bool gReady = false;
static Daemon* gDaemon = nullptr;
static QUICConnection* gConnection = nullptr;

static void initOnce()
{
    if (gInitDone)
        return;
    gInitDone = true;

    // Isolated control dir so the listener's self-signed cert lands in a temp
    // tree, never a real ~/.ubiquitty.
    static char homeTemplate[] = "/tmp/ubiquitty-fuzz-quic-preamble.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return;
    static std::string ubiquittyHome = std::string(home) + "/.ubiquitty";
    ::setenv("UBIQUITTY_HOME", ubiquittyHome.c_str(), 1);

    gDaemon = new Daemon();
    if (!QUICPreambleFuzzAccess::startQUIC(*gDaemon))
        return; // m_quic null → skip every input cleanly

    gConnection = Ubiquitty::Host::makeTestQUICConnection();
    if (!gConnection)
        return;

    gReady = true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    initOnce();
    if (!gReady || size == 0)
        return 0;

    QUICPreambleFuzzAccess::resetForFuzz(*gDaemon, gConnection);

    // Deliver the whole input as the QUIC stream, fragmented at fuzzer-chosen
    // boundaries to simulate stream reassembly: the leading byte of each fragment
    // both selects the fragment length (1..256, clamped) and is part of the fed
    // data, so no byte is skipped and the steering is deterministic.
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t chunk = static_cast<std::size_t>(data[offset]) + 1; // 1..256
        if (chunk > size - offset)
            chunk = size - offset;
        QUICPreambleFuzzAccess::feedStreamData(*gDaemon, gConnection, data + offset, chunk);
        offset += chunk;

        // A post-auth handleFrame may have emptied the client list (EXIT/spawn
        // fail erase); the clientCount guard keeps the readback in bounds.
        if (QUICPreambleFuzzAccess::clientCount(*gDaemon) == 0)
            break;
        // The bound that matters: while still UNAUTHENTICATED, the pre-auth buffer
        // never exceeds the legit preamble ceiling. This holds even for a client
        // the daemon has DROPPED (overflowed) but not yet reaped: a pre-auth drop
        // on a malformed / mismatched preamble sets the flag without clearing the
        // buffer, so a retained over-ceiling buffer is exactly the OOM the cap must
        // prevent — gating on !dropped would make this check vacuous. A successful
        // auth clears + shrinks the buffer, so !authed is the true pre-auth gate.
        // Removing the pre-insert cap trips this assertion.
        if (!QUICPreambleFuzzAccess::authed(*gDaemon))
            assert(QUICPreambleFuzzAccess::authBufferSize(*gDaemon) <= preambleCapacity);
    }
    return 0;
}
