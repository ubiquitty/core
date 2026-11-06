// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_token_uid_local.cpp — only a uid-LOCAL client may mint a QUIC
// bootstrap token.
//
// The mint at Daemon::feedClientFrames' local-control boundary requires a live
// listener and a kernel-verified same-user local stream from a trusted owner
// executable, with no prior HELLO or descriptors. The listener makes the branch
// REACHABLE; the authority gate restricts the mint to the principal the daemon already
// trusts as the person. A QUIC NETWORK peer minting fresh tokens would grant
// itself renewable re-entry without returning through the ssh bootstrap, and a
// same-uid local agent — which has no quicConnection either — would mint itself
// the full interactive scope and the person mark before any HELLO, ahead of the
// require_token gate that refuses it at the handshake.
//
// This stands up a REAL QUICListener (binds an ephemeral UDP socket + a
// self-signed cert under an isolated UBIQUITTY_HOME, so fileDescriptor()>=0 with
// no peer handshake), befriends the Daemon to add clients + drive the handler,
// and checks both the mint count (m_pendingQUICTokens) and the encoded
// bounded local-control grant the client received:
//   - the attach bridge (peerProcess is ubiquitty-host, no connection): a token
//     is minted (grant.status==0, token non-empty, the pending count grew).
//                                               [positive control]
//   - QUIC peer (quicConnection!=nullptr): no token or reply; connection refused.
//     The pending count is unchanged.            [the core fails-without-it]
//   - local agent (another same-uid binary, no connection): no token or reply either.
//                                               [the same, one tier down]
//
// Built only under -DUBIQUITTY_QUIC=ON (the mint is #ifdef-gated). Exit 0 on success.
//

#include "Core/Protocol/Local/Control.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"
#include "TrustedLocalPeer.h"

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Friend seam: reach the private client list, the token store, and local ingress.
struct QUICTokenUIDLocalAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); } // ephemeral listener
    static int quicFileDescriptor(const Daemon& d) { return d.m_quic ? d.m_quic->fileDescriptor() : -1; }
    static std::size_t pendingTokenCount(const Daemon& d) { return d.m_pendingQUICTokens.size(); }
    static bool isRefused(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static bool hasOutput(const Daemon& d, std::size_t index) { return d.m_clients[index].pendingOutputBytes() != 0; }

    // Add a pre-HELLO client and return its index. `fileDescriptor` transfers
    // ownership of the daemon-side fd grants flush to (a local client);
    // `connection` marks a QUIC network peer (non-null) vs a uid-local
    // client (null).
    // Close a client's owned fd through the RAII owner. Later daemon writes to
    // this client then fail cleanly (EBADF path).
    static void closeClientFileDescriptor(Daemon& d, std::size_t index) { (void)d.m_clients[index].fileDescriptor.close(); }

    // Detach a client's QUIC connection before the test frees it. The client
    // entry outlives the test connection here (production order is the
    // reverse: the listener's connections outlive the client list), and
    // ~Daemon's teardown flushes every client — a flush through the dangling
    // pointer queues into freed memory (glibc aborts in sendStream).
    static void clearClientConnection(Daemon& d, std::size_t index) { d.m_clients[index].quicConnection = nullptr; }

    // `peerProcess` is the kernel-verified local binary originOf classifies on:
    // the daemon's own attach bridge answers Local, any other same-uid binary
    // answers Agent, and an empty one (with or without a connection) answers
    // Remote.
    static std::size_t addClient(Daemon& d, PAL::UnixFileDescriptor fileDescriptor, QUICConnection* connection, const std::string& peerProcess = {})
    {
        Daemon::Client c;
        c.fileDescriptor = std::move(fileDescriptor);
        c.quicConnection = connection;
        c.clientID = d.m_nextClientID++;
        c.hasOwnerLocalCredentials = c.fileDescriptor && !connection;
        c.peerProcess = peerProcess;
        // Local is an identity the daemon takes at accept, not the path
        // it prints: a seam that sets only the path is not a Local client.
        c.hasPeerBinary = Ubiquitty::Host::binaryIdentityOf(peerProcess, c.peerBinary);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // Drive the same first-packet ingress as a fresh local request.
    static void requestToken(Daemon& d, std::size_t index, std::uint64_t correlation)
    {
        const auto request = Protocol::Local::encodeLocalQUICTokenRequest(correlation);
        if (request)
            d.feedClientFrames(index, std::as_bytes(std::span { *request }));
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICTokenUIDLocalAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "quic_token_uid_local: FAIL — %s\n", msg);
        ++failures;
    }
}

// Decode one exact daemon-local grant, never a Sync carrier frame.
static bool decodeGrant(const std::vector<std::uint8_t>& bytes, Ubiquitty::Protocol::Local::LocalQUICTokenReply& out)
{
    const auto decoded = Ubiquitty::Protocol::Local::decodeLocalQUICTokenReply(bytes);
    if (!decoded)
        return false;
    out = *decoded;
    return true;
}

int main()
{
    // The daemon ignores SIGPIPE (main.cpp does the same): a write to a client
    // whose peer end is gone must surface as EPIPE, not kill the process.
    ::signal(SIGPIPE, SIG_IGN);
    // Isolated control dir so the listener's self-signed cert lands in a temp
    // tree, never a real ~/.ubiquitty.
    char homeTemplate[] = "/tmp/ubiquitty-quic-uidlocal.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_token_uid_local: mkdtemp failed\n");
        return 1;
    }
    std::string ubiquittyHome = std::string(home) + "/.ubiquitty";
    ::setenv("UBIQUITTY_HOME", ubiquittyHome.c_str(), 1);

    Daemon d;

    // Bring up a real QUIC listener so the mint branch is REACHABLE
    // (m_quic && fileDescriptor() >= 0). No peer handshake is needed — start()
    // binds an ephemeral UDP socket and generates the cert.
    if (!QUICTokenUIDLocalAccess::startQUIC(d)) {
        std::fprintf(stderr, "quic_token_uid_local: startQUIC failed (QUIC build/cert env?)\n");
        return 1;
    }
    check(QUICTokenUIDLocalAccess::quicFileDescriptor(d) >= 0, "the QUIC listener has a live socket (mint branch reachable)");

    // --- LOCAL client (no quicConnection) → a token is minted. [positive control] ---
    // A real socketpair fd so flushClient's write of the grant succeeds (no drop),
    // and we can read the grant back and decode it.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::fprintf(stderr, "quic_token_uid_local: socketpair failed\n");
        return 1;
    }
    const std::size_t before = QUICTokenUIDLocalAccess::pendingTokenCount(d);
    std::size_t localIndex = QUICTokenUIDLocalAccess::addClient(d, Ubiquitty::PAL::UnixFileDescriptor(sv[0]), /*connection*/ nullptr,
        /*peerProcess*/ Ubiquitty::Host::trustedLocalPeerPath());
    QUICTokenUIDLocalAccess::requestToken(d, localIndex, /*correlation*/ 7);

    check(QUICTokenUIDLocalAccess::pendingTokenCount(d) == before + 1, "a uid-local client's request MINTS a token (pending count grows)");

    std::uint8_t buffer[2048];
    ssize_t n = ::read(sv[1], buffer, sizeof(buffer));
    std::vector<std::uint8_t> localGrantBytes(buffer, buffer + (n > 0 ? n : 0));
    Ubiquitty::Protocol::Local::LocalQUICTokenReply localGrant {};
    check(decodeGrant(localGrantBytes, localGrant), "the local client receives a bounded local grant");
    check(localGrant.correlation == 7, "the grant echoes the correlation");
    check(localGrant.daemonInstanceID != 0, "the grant identifies its daemon instance");
    check(localGrant.status == Ubiquitty::Protocol::Local::LocalQUICTokenStatus::Granted, "a uid-local grant succeeds");
    check(localGrant.tokenLength != 0, "a uid-local grant carries a token");
    check(localGrant.certificateSHA256.size() == 32, "a uid-local grant carries the cert pin");
    // End the grant socketpair exactly as the pre-RAII test did: the Client's
    // owned end through the owner (a bare ::close(sv[0]) would double-close),
    // the test's peer end directly.
    QUICTokenUIDLocalAccess::closeClientFileDescriptor(d, localIndex);
    ::close(sv[1]);

    // --- QUIC peer (quicConnection != nullptr) → no token. [the core fails-without-it] ---
    // A real (handshake-less) QUICConnection verifies that a network carrier
    // cannot select daemon-local control and receives no grant bytes.
    Ubiquitty::Host::QUICConnection* peerConnection = Ubiquitty::Host::makeTestQUICConnection();
    check(peerConnection != nullptr, "test QUIC connection allocates");
    const std::size_t beforePeer = QUICTokenUIDLocalAccess::pendingTokenCount(d);
    std::size_t peerIndex = QUICTokenUIDLocalAccess::addClient(d, /*fileDescriptor*/ Ubiquitty::PAL::UnixFileDescriptor {}, peerConnection);
    QUICTokenUIDLocalAccess::requestToken(d, peerIndex, /*correlation*/ 9);

    check(QUICTokenUIDLocalAccess::pendingTokenCount(d) == beforePeer, "a QUIC network peer's request MINTS NOTHING (pending count unchanged)");

    check(QUICTokenUIDLocalAccess::isRefused(d, peerIndex), "a QUIC network peer's local request closes its connection");
    check(Ubiquitty::Host::testQUICConnectionPendingBytes(peerConnection).empty(), "the QUIC peer receives no grant bytes");
    check(!QUICTokenUIDLocalAccess::hasOutput(d, peerIndex), "the daemon queued no local reply for a network peer");

    QUICTokenUIDLocalAccess::clearClientConnection(d, peerIndex);
    Ubiquitty::Host::destroyTestQUICConnection(peerConnection);

    // --- Same-uid local AGENT (originOf Agent) → no token. ---
    // The absent quicConnection is not the local-person test: an MCP client or
    // any other same-uid binary that is not the daemon's own bridge has none
    // either, and the token minted here carries the full interactive scope plus
    // the person mark. Minting for it would hand automation, before any HELLO,
    // the scope require_token refuses it at the handshake.
    int agentEnds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, agentEnds) != 0) {
        std::fprintf(stderr, "quic_token_uid_local: socketpair failed\n");
        return 1;
    }
    const std::size_t beforeAgent = QUICTokenUIDLocalAccess::pendingTokenCount(d);
    std::size_t agentIndex = QUICTokenUIDLocalAccess::addClient(d, Ubiquitty::PAL::UnixFileDescriptor(agentEnds[0]), /*connection*/ nullptr,
        /*peerProcess*/ "/usr/local/bin/some-mcp-agent");
    QUICTokenUIDLocalAccess::requestToken(d, agentIndex, /*correlation*/ 11);

    check(QUICTokenUIDLocalAccess::pendingTokenCount(d) == beforeAgent, "a same-uid local agent's request MINTS NOTHING (pending count unchanged)");

    check(QUICTokenUIDLocalAccess::isRefused(d, agentIndex), "an untrusted local agent's request closes its connection");
    check(!QUICTokenUIDLocalAccess::hasOutput(d, agentIndex), "the daemon queued no local reply for an untrusted agent");
    check(::recv(agentEnds[1], buffer, sizeof(buffer), MSG_DONTWAIT) <= 0, "the local agent received no grant bytes");

    QUICTokenUIDLocalAccess::closeClientFileDescriptor(d, agentIndex);
    ::close(agentEnds[1]);

    if (failures == 0)
        std::printf("quic_token_uid_local: the daemon's own bridge mints a QUIC bootstrap token; a QUIC "
                    "network peer and a same-uid local agent mint nothing (connection refused, no token) — "
                    "renewable network re-entry and pre-HELLO self-issue closed\n");
    return failures == 0 ? 0 : 1;
}
