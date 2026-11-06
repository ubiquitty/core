// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_token_ttl.cpp — QUIC bootstrap tokens expire.
//
// The live QUIC mint/consume wire path is #ifdef UBIQUITTY_QUIC and can't run in
// a non-QUIC build, but the token-store TTL logic (mint stamps a deadline;
// consume rejects + GCs expired entries; pruning runs even below the cap) is
// build-agnostic. This befriends Daemon (QUICTokenTTLAccess) and drives those
// methods directly:
//   - a freshly minted token consumes once (one-time), and again fails,
//   - a token aged past its deadline is REJECTED by consume,
//   - an expired token is PRUNED even with the pending list below the cap,
//   - a not-yet-expired token survives a prune and still consumes.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <string>

namespace Ubiquitty {
namespace Host {

// Friend seam: reach the private token store + helpers.
struct QUICTokenTTLAccess {
    // TTL logic here is ownership-agnostic — every mint in this file is
    // attributed to the same fixed test client id.
    static std::string mint(Daemon& d) { return d.mintQUICToken(/*mintingClientID*/ 1, Ubiquitty::Protocol::Sync::ScopeSet { Ubiquitty::Protocol::Sync::Scope::Observe }); }
    static bool consume(Daemon& d, const std::string& token)
    {
        Ubiquitty::Protocol::Sync::ScopeSet scope;
        std::string tokenID;
        return d.consumeQUICToken(token, scope, tokenID);
    }
    static void prune(Daemon& d, std::uint64_t now) { d.pruneExpiredQUICTokens(now); }
    static std::uint64_t earliest(const Daemon& d) { return d.earliestQUICTokenDeadline(); }
    static std::size_t count(const Daemon& d) { return d.m_pendingQUICTokens.size(); }
    // Inject a token carrying an explicit deadline (bypasses the real-time TTL
    // so the test needn't sleep for the live 60s window).
    static void inject(Daemon& d, const std::string& token, std::uint64_t deadlineMilliseconds) { d.m_pendingQUICTokens.push_back({ token, deadlineMilliseconds, /*mintingClientID*/ 1 }); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICTokenTTLAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "quic_token_ttl: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    Daemon d;

    // --- A freshly minted token consumes once, then never again. ---
    const std::string fresh = QUICTokenTTLAccess::mint(d);
    check(!fresh.empty(), "mint should produce a token");
    check(QUICTokenTTLAccess::earliest(d) != 0, "a pending token arms a deadline");
    check(QUICTokenTTLAccess::consume(d, fresh), "a fresh token consumes before its TTL");
    check(!QUICTokenTTLAccess::consume(d, fresh), "a consumed token cannot be reused (one-time)");

    // --- A token aged past its deadline is REJECTED by consume. ---
    QUICTokenTTLAccess::inject(d, "expired-token", /*deadline*/ 1); // ~epoch: long past
    check(QUICTokenTTLAccess::count(d) == 1, "injected expired token is pending");
    check(!QUICTokenTTLAccess::consume(d, "expired-token"), "an expired token must not consume");
    check(QUICTokenTTLAccess::count(d) == 0, "consume prunes the expired token it rejected");

    // --- Pruning drops an expired entry even BELOW the cap. ---
    QUICTokenTTLAccess::inject(d, "stale", /*deadline*/ 1);
    check(QUICTokenTTLAccess::count(d) == 1, "stale token pending");
    QUICTokenTTLAccess::prune(d, /*now*/ 1000);
    check(QUICTokenTTLAccess::count(d) == 0, "prune GCs the expired token with the list under cap");

    // --- A far-future token survives a prune and still consumes. ---
    QUICTokenTTLAccess::inject(d, "future", /*deadline*/ 0xFFFFFFFFFFFFFFFFull);
    QUICTokenTTLAccess::prune(d, /*now*/ 1000);
    check(QUICTokenTTLAccess::count(d) == 1, "a future token survives the prune");
    check(QUICTokenTTLAccess::consume(d, "future"), "a not-yet-expired token still consumes");

    if (failures == 0)
        std::printf("quic_token_ttl: minted tokens are one-time; expired tokens are rejected by "
                    "consume and pruned even below the cap; future tokens survive\n");
    return failures == 0 ? 0 : 1;
}
