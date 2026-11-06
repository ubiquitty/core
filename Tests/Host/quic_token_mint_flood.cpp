// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_token_mint_flood.cpp — security negative for QUIC token mint-flood ownership:
//
// The hazard: the pending-QUIC-token pool (m_pendingQUICTokens) is a
// single global, bounded (64-entry) vector shared by every uid-gated local
// client. A cap eviction that dropped the globally oldest entry — whichever
// client minted it — the instant the pool hit 64 would let any local peer
// (every local client shares one uid: a second GUI window, a rogue trusted
// owner repeatedly requesting local bootstrap) flood mints and evict a
// different client's still-pending, not-yet-consumed token: a local DoS
// against a peer waiting on its own SSH-relayed QUIC bootstrap.
//
// So mintQUICToken's cap eviction is self-eviction only — it drops the
// MINTING client's own oldest pending token (found by the clientID a
// PendingQUICToken is stamped with at mint). A client with none of its
// own pending yet (so self-eviction has nothing to reclaim) fails the mint
// closed (empty token, retryable) rather than taking a stranger's slot.
//
// This file:
//   (a) the exploit, BLOCKED — client A mints one token; client B floods the
//       pool to the cap with its OWN mints, then mints one more past the cap.
//       A's original token must still be pending — B's flood self-evicts
//       from B's own tokens, never A's.
//   (b) A brand-new client C, with zero tokens of its own, minting while the
//       pool is entirely full of other clients' tokens gets an EMPTY token
//       (mint refused) — it must not evict anyone to make room for itself.
//   (c) REGRESSION GUARD — a normal mint well under the cap still succeeds
//       and is consumable, proving the ownership gate doesn't over-refuse.
//
// Verify-by-revert: making mintQUICToken erase m_pendingQUICTokens.begin()
// unconditionally makes (a) fail — A's token is evicted by B's flood.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Ubiquitty {
namespace Host {

// Friend seam: drive the real private mintQUICToken/consumeQUICToken with
// distinct clientIDs (no live QUIC connection needed — the token store is
// wire-agnostic, mirroring quic_token_ttl.cpp's own rationale).
struct QUICMintFloodAccess {
    static std::string mint(Daemon& d, std::uint32_t clientID) { return d.mintQUICToken(clientID, Ubiquitty::Protocol::Sync::ScopeSet { Ubiquitty::Protocol::Sync::Scope::Observe }); }
    static bool consume(Daemon& d, const std::string& token)
    {
        Ubiquitty::Protocol::Sync::ScopeSet scope;
        std::string tokenID;
        return d.consumeQUICToken(token, scope, tokenID);
    }
    static std::size_t count(const Daemon& d) { return d.m_pendingQUICTokens.size(); }
    static bool isPending(const Daemon& d, const std::string& token)
    {
        return std::any_of(d.m_pendingQUICTokens.begin(), d.m_pendingQUICTokens.end(), [&token](const Daemon::PendingQUICToken& p) { return p.token == token; });
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICMintFloodAccess;

static int failures = 0;
static void check(bool ok, const char* msg) // bool-ok: established test-harness assert pattern (Tests/Host/*.cpp)
{
    if (!ok) {
        std::fprintf(stderr, "quic_token_mint_flood: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    Daemon d;
    constexpr std::uint32_t clientA = 1;
    constexpr std::uint32_t clientB = 2;
    constexpr std::uint32_t clientC = 3;
    constexpr std::size_t poolCapacity = 64; // mirrors maxPendingQUICTokens in Daemon.cpp

    // (a) the exploit, BLOCKED.
    const std::string tokenA = QUICMintFloodAccess::mint(d, clientA);
    check(!tokenA.empty(), "client A mints a token");
    check(QUICMintFloodAccess::count(d) == 1, "one token pending after A's mint");

    // Client B alone fills the pool to the cap (63 more, joining A's 1 = 64).
    for (std::size_t i = 0; i < poolCapacity - 1; ++i) {
        const std::string t = QUICMintFloodAccess::mint(d, clientB);
        check(!t.empty(), "client B's fill-to-cap mint succeeds (still under the cap)");
    }
    check(QUICMintFloodAccess::count(d) == poolCapacity, "pool is exactly at the cap");
    check(QUICMintFloodAccess::isPending(d, tokenA), "A's token is still pending before B floods past the cap");

    // Client B mints one more, past the cap — this is the flood step.
    const std::string floodToken = QUICMintFloodAccess::mint(d, clientB);
    check(!floodToken.empty(), "B's over-cap mint still succeeds — it self-evicts one of its OWN tokens");
    check(QUICMintFloodAccess::count(d) == poolCapacity, "the pool stays capped at 64 after the self-eviction");
    check(QUICMintFloodAccess::isPending(d, tokenA), "A's original token MUST survive B's flood — self-eviction never touches another client's token");

    // (b) A brand-new client with nothing of its own to self-evict fails closed.
    const std::string tokenC = QUICMintFloodAccess::mint(d, clientC);
    check(tokenC.empty(),
        "a client with zero pending tokens of its own gets an EMPTY token when the pool is full of "
        "OTHERS' tokens — it must not evict a stranger to make room for itself");
    check(QUICMintFloodAccess::count(d) == poolCapacity, "the pool is unchanged by C's refused mint");
    check(QUICMintFloodAccess::isPending(d, tokenA), "A's token still survives after C's refused mint");

    // (c) REGRESSION GUARD — a normal, well-under-cap mint still works and consumes.
    {
        Daemon fresh;
        const std::string normal = QUICMintFloodAccess::mint(fresh, clientA);
        check(!normal.empty(), "a normal mint well under the cap still succeeds");
        check(QUICMintFloodAccess::consume(fresh, normal), "and the minted token still consumes once");
    }

    if (failures == 0)
        std::printf("quic_token_mint_flood: mintQUICToken's cap eviction is self-only — a same-uid "
                    "flood from one client can no longer evict another client's still-pending QUIC "
                    "bootstrap token, and a client with nothing of its own to evict fails the mint "
                    "closed instead of taking a stranger's slot\n");
    return failures == 0 ? 0 : 1;
}
