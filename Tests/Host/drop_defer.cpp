// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/drop_defer.cpp — dropClient never erases m_clients inline.
//
// dropClient() erasing m_clients mid-handler
// shifts indices, so a handler that holds an index across the drop (kickClient,
// refuseClient, the broadcastPresence/teardown forward loops) then marks or
// services the WRONG entry — the neighbor that shifted into the freed slot —
// which the reap pass closes. An innocent client gets reaped. So dropClient only
// marks the client (overflowed); reapClient does the close+erase, called solely
// from the run-loop reap pass and only from the end of m_clients, so no erase
// ever shifts a still-iterated index.
//
// The live-wire race can't be driven black-box on Darwin (SHUT_RD does not EPIPE
// the peer, the first write to a closed peer succeeds, and a closed peer's read
// EOF is reaped by the index-safe find-by-fd service loop first), so this
// befriends Daemon and tests the root invariant directly.
//
// Verify-by-revert: make dropClient call `reapClient(index)` inline and this
// FAILS — the size shrinks and the innocent neighbor shifts into the dropped slot.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <utility>

namespace Ubiquitty {
namespace Host {

// Friend seam: build a client table + drive dropClient with no real sockets.
struct DropDeferAccess {
    // The default Client has fileDescriptor = -1 and sessionID = 0, which keep
    // reapClient side-effect-free under the verify-by-revert (no fd to close;
    // broadcastPresence(0) early-returns), so the revert isolates the erase.
    static void addClient(Daemon& d, std::uint32_t clientID)
    {
        Daemon::Client c {};
        c.clientID = clientID;
        d.m_clients.push_back(std::move(c));
    }
    static void drop(Daemon& d, std::size_t index) { d.dropClient(index); }
    static std::size_t count(const Daemon& d) { return d.m_clients.size(); }
    static bool didOverflow(const Daemon& d, std::size_t i) { return d.m_clients[i].didOverflow; }
    static std::uint32_t clientID(const Daemon& d, std::size_t i) { return d.m_clients[i].clientID; }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::DropDeferAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "drop_defer: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    Daemon d;

    // [owner(10), agentBad(20), innocent(30)] — innocent sits after the dropped
    // client: exactly the neighbor that shifts into the freed slot on an inline erase.
    DropDeferAccess::addClient(d, 10);
    DropDeferAccess::addClient(d, 20);
    DropDeferAccess::addClient(d, 30);
    check(DropDeferAccess::count(d) == 3, "three clients seeded");

    // Drop the middle client. Deferred contract: MARK it, erase nothing.
    DropDeferAccess::drop(d, 1);

    check(DropDeferAccess::count(d) == 3, "dropClient must NOT erase m_clients inline — size unchanged (the deferred-drop invariant)");
    check(DropDeferAccess::didOverflow(d, 1), "the dropped client (index 1) is marked overflowed for the deferred reap");
    check(DropDeferAccess::clientID(d, 1) == 20, "index 1 is still agentBad (20) — not shifted");
    check(DropDeferAccess::clientID(d, 2) == 30, "the innocent neighbor (30) stays at index 2 — not shifted into the dropped slot, not reaped");
    check(!DropDeferAccess::didOverflow(d, 2), "the innocent neighbor is NOT wrongly marked for reap");
    check(DropDeferAccess::clientID(d, 0) == 10 && !DropDeferAccess::didOverflow(d, 0), "the owner (10) is untouched");

    if (failures == 0)
        std::printf("drop_defer: dropClient marks (overflowed) without erasing m_clients inline; "
                    "no later client shifts or is wrongly reaped — the deferred-drop invariant\n");
    return failures == 0 ? 0 : 1;
}
