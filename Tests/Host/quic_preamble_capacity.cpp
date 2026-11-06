// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_preamble_capacity.cpp — an UNAUTHENTICATED QUIC peer can't grow the
// daemon's pre-auth preamble buffer without bound (pre-auth remote-OOM).
//
// Daemon::onQUICStreamData holds a new QUIC connection's leading bytes in
// Client::quicPreamble until they validate as a preamble: [1][u8 length][token]
// or [2][resume proof], at most Sync::maxPreambleBytes (66), consumed before
// any Sync byte. Without a cap, a peer that streams megabytes pre-auth grows
// what is held up to the connection's stream window (4 MiB), and x
// maxQUICConnections is a multi-hundred-MiB OOM — triggerable with no token
// (worse than the authed slow-consumer cap). The reader takes a preamble's
// worth of bytes and no more, whatever kind the first byte claims.
//
// This befriends the Daemon to add an UNAUTHED QUIC client (quicAuthed == false,
// a real handshake-less quicConnection) and drive the PRIVATE onQUICStreamData
// with a pre-auth flood far over a preamble, then reads back the client's
// dropped flag and how much its reader holds:
//   - DROPPED: dropClient marked the client dead (overflowed == true).
//   - BOUNDED: the reader never held the flood — it stays within
//     Sync::maxPreambleBytes. This is the assertion that fails on a revert:
//     without the bound the whole flood accumulates.
//
// Built only under -DUBIQUITTY_QUIC=ON (onQUICStreamData is #ifdef-gated). Exit 0
// on success.
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

// The most a connection that has authenticated nothing is held of.
static constexpr std::size_t preambleCapacity = Protocol::Sync::maxPreambleBytes;

// Friend seam: reach the private client list, the QUIC listener, and
// onQUICStreamData, and read back a client's drop flag + quicAuthBuffer size.
struct QUICPreambleCapacityAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); } // ephemeral listener → m_quic non-null
    static bool haveQUIC(const Daemon& d) { return d.m_quic != nullptr; }
    // The socket a serving daemon names in every Welcome's selection.
    static void serveAt(Daemon& d, const std::string& socketPath) { d.m_socketPath = socketPath; }

    // Add an UNAUTHED QUIC client (non-null connection, no socket fd, quicAuthed
    // still false) and return its index. Its id is unique, as the daemon's own
    // accept assigns it: admission finds the greeting client by id.
    static std::size_t addUnauthedQUICClient(Daemon& d, QUICConnection* connection)
    {
        Daemon::Client c;
        c.quicConnection = connection;
        c.clientID = d.m_nextClientID++;
        c.quicAuthed = false;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // Drive the PRIVATE pre-auth stream-data path with `bytes` of pre-auth data.
    static void feedStreamData(Daemon& d, QUICConnection* connection, const std::vector<std::uint8_t>& bytes) { d.onQUICStreamData(connection, bytes); }

    // Mirror onQUICConnectionClosed's unbind before a test connection is
    // destroyed: production nulls the client's quicConnection before the
    // listener frees it, so a dead pointer never lingers in m_clients. Without
    // this, malloc can hand the NEXT test connection the same address and
    // findOrCreateQUICClient then routes its bytes to the dead (overflowed)
    // client — the inbound gate eats them and the section flakes by heap
    // layout.
    static void unbindConnection(Daemon& d, std::size_t index) { d.m_clients[index].quicConnection = nullptr; }

    // dropClient marks a client dead by setting overflowed (deferred drop; the
    // client is not erased inline), so a dropped client is still readable here.
    static bool dropped(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static std::size_t authBufferSize(const Daemon& d, std::size_t index) { return d.m_clients[index].quicPreamble.heldBytes(); }
    static bool authed(const Daemon& d, std::size_t index) { return d.m_clients[index].quicAuthed; }
    static bool helloSeen(const Daemon& d, std::size_t index) { return d.m_clients[index].helloSeen; }
    static bool isAdmitted(const Daemon& d, std::size_t index)
    {
        const auto& connection = d.m_clients[index].syncConnection;
        return connection && connection->state() == Protocol::Sync::ConnectionState::Attached;
    }

    // A one-time token the next preamble may present, carrying the scope the
    // daemon's local mint bakes into a bootstrap token.
    static void addPendingToken(Daemon& d, const std::string& token)
    {
        Daemon::PendingQUICToken pending;
        pending.token = token;
        pending.deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max();
        pending.mintingClientID = 0;
        pending.scope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control,
            Protocol::Sync::Scope::RegionRead, Protocol::Sync::Scope::NewSession, Protocol::Sync::Scope::FileRead };
        d.m_pendingQUICTokens.push_back(std::move(pending));
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::preambleCapacity;
using Ubiquitty::Host::QUICPreambleCapacityAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "quic_preamble_capacity: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    // Isolated control dir so the listener's self-signed cert lands in a temp
    // tree, never a real ~/.ubiquitty.
    char homeTemplate[] = "/tmp/ubiquitty-quic-preamblecap.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_preamble_capacity: mkdtemp failed\n");
        return 1;
    }
    std::string ubiquittyHome = std::string(home) + "/.ubiquitty";
    ::setenv("UBIQUITTY_HOME", ubiquittyHome.c_str(), 1);

    Daemon d;
    QUICPreambleCapacityAccess::serveAt(d, ubiquittyHome + "/daemon.sock");
    // A real listener so m_quic is non-null. No peer handshake — onQUICStreamData
    // routes the passed connection through findOrCreateQUICClient (already in the
    // client list) and the pre-auth buffer path directly.
    if (!QUICPreambleCapacityAccess::startQUIC(d)) {
        std::fprintf(stderr, "quic_preamble_capacity: startQUIC failed (QUIC build/cert env?)\n");
        return 1;
    }
    check(QUICPreambleCapacityAccess::haveQUIC(d), "the QUIC listener is live (m_quic non-null)");

    // An unauthed QUIC peer floods pre-auth bytes far over the legit preamble
    // ceiling (1 + maxQUICTokenLength). A token-less peer can trigger this; the
    // first byte (0xFF = a 255-byte declared token, > maxQUICTokenLength) would be
    // rejected as malformed too, but the OOM defense must bite before any byte is
    // even buffered — the flood here (1 MiB) is what an attacker streams to grow
    // the buffer toward the 4 MiB stream window.
    {
        Ubiquitty::Host::QUICConnection* connection = Ubiquitty::Host::makeTestQUICConnection();
        check(connection != nullptr, "test QUIC connection allocates");
        std::size_t idx = QUICPreambleCapacityAccess::addUnauthedQUICClient(d, connection);
        check(!QUICPreambleCapacityAccess::authed(d, idx), "the client starts UNAUTHENTICATED");

        const std::size_t floodSize = 1u * 1024u * 1024u; // 1 MiB pre-auth flood, >> preambleCapacity
        std::vector<std::uint8_t> flood(floodSize, 0xFF);
        QUICPreambleCapacityAccess::feedStreamData(d, connection, flood);

        check(QUICPreambleCapacityAccess::dropped(d, idx), "a pre-auth flood over 1 + maxQUICTokenLength DROPS the unauthed QUIC peer");
        check(!QUICPreambleCapacityAccess::authed(d, idx), "the dropped peer never authenticated (no token was ever consumed)");
        // The bound that matters: the buffer must not have grown to hold the flood.
        // The drop comes before the insert, so quicAuthBuffer stays <= the legit
        // preamble ceiling (here: empty — nothing was buffered). With the insert
        // first, this assertion fails: the buffer accumulates the whole 1 MiB.
        const std::size_t buffered = QUICPreambleCapacityAccess::authBufferSize(d, idx);
        check(buffered <= preambleCapacity,
            "the pre-auth buffer stayed BOUNDED (<= 1 + maxQUICTokenLength) — the "
            "flood's allocation was refused, never accumulated");

        QUICPreambleCapacityAccess::unbindConnection(d, idx);
        Ubiquitty::Host::destroyTestQUICConnection(connection);
    }

    // A second flood spread across MANY small writes must also stay bounded: the
    // cap is checked on every onQUICStreamData call, so an attacker can't dribble
    // the buffer past the ceiling one chunk at a time either. Each chunk fits under
    // the cap on its own, but their running sum must trip the drop before it grows.
    {
        Ubiquitty::Host::QUICConnection* connection = Ubiquitty::Host::makeTestQUICConnection();
        check(connection != nullptr, "(dribble) test QUIC connection allocates");
        std::size_t idx = QUICPreambleCapacityAccess::addUnauthedQUICClient(d, connection);

        bool everDropped = false;
        // 40-byte chunks: each fits under preambleCapacity (65) but a few of them sum
        // past it, so the running buffer.size()+length test must fire.
        std::vector<std::uint8_t> chunk(40, 0xFE);
        for (int i = 0; i < 1000 && !everDropped; ++i) {
            QUICPreambleCapacityAccess::feedStreamData(d, connection, chunk);
            everDropped = QUICPreambleCapacityAccess::dropped(d, idx);
            // The buffer must never exceed the ceiling at any point in the dribble.
            check(QUICPreambleCapacityAccess::authBufferSize(d, idx) <= preambleCapacity, "(dribble) the pre-auth buffer stays bounded across many small writes");
            if (everDropped)
                break;
        }
        check(everDropped, "(dribble) the running pre-auth sum trips the cap and drops the peer");

        QUICPreambleCapacityAccess::unbindConnection(d, idx);
        Ubiquitty::Host::destroyTestQUICConnection(connection);
    }

    // A flood under each kind a preamble may claim is held a preamble's worth
    // and dropped: a megabyte behind the resume kind, which the daemon cannot
    // admit on a connection with no handshake to export from, and a megabyte
    // behind a token's longest length, which is no token it minted.
    for (const std::uint8_t kind : { std::uint8_t { 1 }, std::uint8_t { 2 } }) {
        Ubiquitty::Host::QUICConnection* connection = Ubiquitty::Host::makeTestQUICConnection();
        check(connection != nullptr, "(kinds) test QUIC connection allocates");
        std::size_t idx = QUICPreambleCapacityAccess::addUnauthedQUICClient(d, connection);
        std::vector<std::uint8_t> flood(1u * 1024u * 1024u, 0x40);
        flood[0] = kind;
        for (std::size_t offset = 0; offset < flood.size() && !QUICPreambleCapacityAccess::dropped(d, idx); offset += 4096) {
            QUICPreambleCapacityAccess::feedStreamData(d, connection, std::vector<std::uint8_t>(flood.begin() + static_cast<std::ptrdiff_t>(offset), flood.begin() + static_cast<std::ptrdiff_t>(offset + 4096)));
            check(QUICPreambleCapacityAccess::authBufferSize(d, idx) <= preambleCapacity, "(kinds) the reader holds no more than a preamble under a kind it knows");
        }
        check(QUICPreambleCapacityAccess::dropped(d, idx), "(kinds) a flood behind a known kind drops the unauthed QUIC peer");
        check(!QUICPreambleCapacityAccess::authed(d, idx), "(kinds) the dropped peer never authenticated");
        QUICPreambleCapacityAccess::unbindConnection(d, idx);
        Ubiquitty::Host::destroyTestQUICConnection(connection);
    }

    // A client's first frames may share a chunk with its preamble: a dialer
    // writes the token and the sole canonical Greet behind it as one
    // stream write. Only the preamble is held; the rest reaches the decoder
    // once the token checks out.
    {
        Ubiquitty::Host::QUICConnection* connection = Ubiquitty::Host::makeTestQUICConnection();
        check(connection != nullptr, "(coalesced) test QUIC connection allocates");
        const std::string token(32, 'T');
        QUICPreambleCapacityAccess::addPendingToken(d, token);
        std::size_t idx = QUICPreambleCapacityAccess::addUnauthedQUICClient(d, connection);
        std::vector<std::uint8_t> chunk;
        Ubiquitty::Protocol::Sync::encodePreamble(chunk, Ubiquitty::Protocol::Sync::TokenPreamble { .token = token });
        namespace Sync = Ubiquitty::Protocol::Sync;
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { 1, 0x707265616d626c65 };
        greeting.clientKind = Sync::ClientKind::Agent;
        greeting.requestedRole = Sync::Role::Agent;
        greeting.request = { .target = Sync::AdmissionTarget::Control, .agentLabel = std::string(120, 'a') };
        greeting.facts.viewportColumns = 80;
        greeting.facts.viewportRows = 24;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        check(Sync::decodeGreet(body).has_value(), "(coalesced) the complete current Control greeting is valid");
        check(Sync::appendFrame(chunk, Sync::FrameKind::Greet, 0, body), "(coalesced) the direct greeting follows the token preamble");
        check(chunk.size() > preambleCapacity, "(coalesced) premise: the chunk is larger than a preamble");
        QUICPreambleCapacityAccess::feedStreamData(d, connection, chunk);
        check(QUICPreambleCapacityAccess::authed(d, idx), "(coalesced) the preamble's token is consumed");
        check(QUICPreambleCapacityAccess::helloSeen(d, idx), "(coalesced) the Greet behind the preamble reaches actual admission");
        check(QUICPreambleCapacityAccess::isAdmitted(d, idx), "(coalesced) the token's scope admits that Control greeting");
        QUICPreambleCapacityAccess::unbindConnection(d, idx);
        Ubiquitty::Host::destroyTestQUICConnection(connection);
    }

    if (failures == 0)
        std::printf("quic_preamble_capacity: an unauthenticated QUIC peer's pre-auth preamble buffer is "
                    "capped at a preamble's length — a token-less byte flood is dropped before it "
                    "is accumulated, closing the pre-auth remote-OOM\n");
    return failures == 0 ? 0 : 1;
}
