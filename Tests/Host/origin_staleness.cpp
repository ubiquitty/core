// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/origin_staleness.cpp — program OUTPUT is gated at the attached
// drivers' origin, never the stale last-input origin.
//
// The hazard: a session that stamped Terminal::currentCapabilityOrigin only when a client's
// input is written (the driving principal) would leave the gate reading the default Local
// between inputs — and on a fresh, never-driven session. A side effect the program emits
// on its own (DECUDK reprogramming a key) would then be gated as Local ⇒ Allowed, even
// though the only principal that can reach this session is a least-trusted Remote attach,
// and a remote-injected DECUDK would reprogram a key during a Local-stamped drain. So
// servicePTY recomputes the output-side origin as the most-restrictive origin among the
// session's attached, non-overflowed drivers (Remote > Agent > Local), caches it in
// Session::outputOrigin, and stamps it onto the Ubiquitty::Terminal before draining the
// PTY. A detached session keeps its last origin (no downgrade to Local).
//
// Driving the live wire would need a real remote attach plus a program that emits
// DECUDK on its own mid-drain — not deterministically synthesizable through the
// consent/PTY harness — so this befriends Daemon and exercises the recompute-stamp
// directly, then feeds a real DECUDK through Core to prove the gate's verdict.
//
// Verify-by-revert: delete the `s.term->currentCapabilityOrigin = s.outputOrigin`
// stamp (or the whole recompute block) and the Remote-DECUDK-denied + never-Local
// assertions FAIL — the stamp stays at the Local default and the reprogram lands.
//
// Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Capability.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"
#include "TrustedLocalPeer.h"

#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty {
namespace Host {

// A stream that is immediately at EOF: servicePTY's pre-read origin-recompute runs,
// then the very first read() returns EndOfFile so the drain loop exits without ever
// touching process()/writeAll (those live behind the gotData branch, never reached).
// This isolates the recompute-then-stamp — the unit under test — from real PTY data.

// Friend seam: build a Daemon + a fully-wired Session (Terminal + capabilityPolicy →
// m_grantStore) and a client table with no real sockets, then drive servicePTY's
// origin recompute and observe Terminal::currentCapabilityOrigin. Mirrors
// drop_defer.cpp's DropDeferAccess.
struct OriginStalenessAccess {
    // A session wired exactly as createSession does (policy → store, policy → term),
    // with a stub EOF stream so servicePTY's drain loop exits at once.
    static Daemon::Session& makeSession(Daemon& d)
    {
        auto s = std::make_unique<Daemon::Session>();
        s->id = d.m_nextSessionID++;
        s->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*s->term, 24, 80);
        s->capabilityPolicy.setGrantStore(&d.m_grantStore);
        s->term->capabilityPolicy = &s->capabilityPolicy;
        s->stream = TestDoubles::makeClosedStream();
        Daemon::Session& ref = *s;
        d.m_sessions.push_back(std::move(s));
        return ref;
    }

    // originOf keys on three Client fields (fail-safe):
    //   quicConnection || peerProcess.empty()        ⇒ Remote (no verified local identity)
    //   else !agentLabel.empty()                     ⇒ Agent  (verified-local, self-claims agent)
    //   else basename(peerProcess)==ubiquitty-host   ⇒ Local  (the local attach bridge)
    //   else                                         ⇒ Agent  (fail-safe: unlabeled non-bridge)
    // Local: the ubiquitty-host attach bridge, no agentLabel.
    static void addLocalClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        c.workspaceID = sessionID;
        // The bundled local attach bridge is the trusted local peer binary.
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        d.m_clients.push_back(std::move(c));
    }
    // Agent: a verified peerProcess and a self-claimed agentLabel. Carries a real
    // Drive-scoped token — tokenScopeAllows requires origin Local or a
    // real token for every scope (the tokenless free-pass is Local-only), so a
    // driver this test means to count toward the escalation must actually hold one,
    // matching what a real attached Agent driver has.
    static void addAgentClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        c.workspaceID = sessionID;
        c.peerProcess = "/usr/bin/agent-mcp"; // verified local identity
        c.agentLabel = "test-agent"; // claims to be an agent
        c.hasToken = true;
        c.tokenScope = Ubiquitty::Protocol::Sync::Scope::Drive;
        d.m_clients.push_back(std::move(c));
    }
    // Remote: no verified local identity (empty peerProcess) — a QUIC/remote attach.
    // Drive-scoped token for the same reason as addAgentClient above.
    static void addRemoteClient(Daemon& d, std::uint32_t sessionID, bool didOverflow = false)
    {
        Daemon::Client c {};
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        c.workspaceID = sessionID;
        c.peerProcess = ""; // no verified local identity ⇒ Remote
        c.agentLabel = "remote-attach"; // self-claimed; ignored without a verified peer
        c.didOverflow = didOverflow;
        c.hasToken = true;
        c.tokenScope = Ubiquitty::Protocol::Sync::Scope::Drive;
        d.m_clients.push_back(std::move(c));
    }
    // Observe-only Remote: token-authed with the Observe scope but not Drive — a
    // viewer that receives the screen but cannot inject input. It must not
    // escalate the output origin (it cannot author a side effect).
    static void addObserveOnlyRemoteClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        c.workspaceID = sessionID;
        c.peerProcess = ""; // ⇒ Remote
        c.hasToken = true;
        c.tokenScope = Ubiquitty::Protocol::Sync::ScopeSet { Ubiquitty::Protocol::Sync::Scope::Observe };
        d.m_clients.push_back(std::move(c));
    }
    // Drive-scoped Remote: token-authed WITH Drive — a real driver that can inject,
    // so it escalates the output origin like any driving principal.
    static void addDriveRemoteClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        c.workspaceID = sessionID;
        c.peerProcess = ""; // ⇒ Remote
        c.hasToken = true;
        c.tokenScope = Ubiquitty::Protocol::Sync::Scope::Observe | Ubiquitty::Protocol::Sync::Scope::Drive;
        d.m_clients.push_back(std::move(c));
    }

    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }
    // Simulate a driver DETACHING: drop its client row from the daemon table
    // (the run loop does this when a client EXITs), leaving any co-attendees.
    static void detachClientAt(Daemon& d, std::size_t i) { d.m_clients.erase(d.m_clients.begin() + i); }
    static Ubiquitty::CapabilityOrigin driverFloor(const Daemon::Session& s) { return s.driverOriginFloor; }
    static Ubiquitty::CapabilityOrigin originOf(const Daemon& d, const Daemon::Client& c) { return d.originOf(c); }
    static Daemon::Client& clientAt(Daemon& d, std::size_t i) { return d.m_clients[i]; }
    static void setOutputOrigin(Daemon::Session& s, Ubiquitty::CapabilityOrigin o) { s.outputOrigin = o; }

    // Session is a private nested type, so main() can't name its members; route the
    // observations through the friend.
    static Ubiquitty::Terminal* term(Daemon::Session& s) { return s.term.get(); }
    static std::uint32_t id(const Daemon::Session& s) { return s.id; }
    static Ubiquitty::CapabilityOrigin stampedOrigin(const Daemon::Session& s) { return s.term->currentCapabilityOrigin; }
    static int udkCount(const Daemon::Session& s)
    {
        // The read-only accessor, not ensureUserDefinedKeys: an observation that
        // allocated the table would defeat the absent-until-written invariant it
        // is observing, and an unprogrammed table counts zero either way.
        const Ubiquitty::VT::UserDefinedKeys* keys = Ubiquitty::userDefinedKeysOrNull(*s.term);
        return keys ? keys->count : 0;
    }
    static void setStampedOrigin(Daemon::Session& s, Ubiquitty::CapabilityOrigin o) { s.term->currentCapabilityOrigin = o; }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::OriginStalenessAccess;

// Feed one DECUDK that (re)programs key 11 → "A" (0x41). Pc=1 (don't pre-clear),
// Pl=1 (don't lock). DCS = ESC P, ST = ESC \. The gate (Capability::ProgramKeys,
// HostEscaping tier) ALLOWS this only at origin Local; Agent ⇒ Prompt, Remote ⇒
// Deny — both leave t->udk untouched.
static void feedDECUDK(Ubiquitty::Terminal& t)
{
    static const char seq[] = "\x1bP1;1|11/41\x1b\\";
    Ubiquitty::processBytes(&t, std::string_view(seq, sizeof(seq) - 1));
}

int main()
{
    // originOf classifies the three constructed clients as intended.
    {
        Daemon d;
        OriginStalenessAccess::addLocalClient(d, 1);
        OriginStalenessAccess::addAgentClient(d, 1);
        OriginStalenessAccess::addRemoteClient(d, 1);
        CHECK(OriginStalenessAccess::originOf(d, OriginStalenessAccess::clientAt(d, 0)) == CapabilityOrigin::Local);
        CHECK(OriginStalenessAccess::originOf(d, OriginStalenessAccess::clientAt(d, 1)) == CapabilityOrigin::Agent);
        CHECK(OriginStalenessAccess::originOf(d, OriginStalenessAccess::clientAt(d, 2)) == CapabilityOrigin::Remote);
    }

    using A = OriginStalenessAccess;

    // CORE fails-without-it: a Remote driver attached ⇒ servicePTY stamps the OUTPUT
    // origin Remote, and a DECUDK the program then emits is DENIED (udk unchanged).
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addRemoteClient(d, A::id(s));

        // The Ubiquitty::Terminal defaults to Local — the stale stamp a missing recompute leaves in place.
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Local);
        A::service(d, s); // recompute + stamp before the (EOF) drain
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Remote);

        CHECK(A::udkCount(s) == 0);
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 0); // ProgramKeys denied for Remote — no reprogram
    }

    // never-default-Local: a fresh session whose only driver is Remote, recompute
    // runs before any input — the output origin must be Remote, not the Local default.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addRemoteClient(d, A::id(s));
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Remote);
        // ...and a Local-default contrast: a DECUDK at Local would land, proving the
        // assertion above is non-vacuous (the sequence really programs a key).
        A::setStampedOrigin(s, CapabilityOrigin::Local);
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 1); // Local ⇒ ProgramKeys allowed ⇒ the key lands
    }

    // Agent driver ⇒ Agent: host-escaping ProgramKeys PROMPTS (not Allow) ⇒ denied
    // until a human grant, so the DECUDK is held off (udk unchanged).
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addAgentClient(d, A::id(s));
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Agent);
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 0); // Agent ⇒ Prompt (≠ Allow) ⇒ no reprogram
    }

    // most-restrictive wins: a Local and a Remote driver both attached ⇒ Remote.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addLocalClient(d, A::id(s));
        A::addRemoteClient(d, A::id(s));
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Remote);
    }

    // an OVERFLOWED Remote client is not a live driver: a Local + an overflowed
    // Remote ⇒ Local (the overflowed peer is excluded from the recompute).
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addLocalClient(d, A::id(s));
        A::addRemoteClient(d, A::id(s), /*didOverflow*/ true);
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Local);
    }

    // detached: a session that was Remote keeps its last outputOrigin when no driver
    // is attached — no silent downgrade to Local that would re-open the gate.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::setOutputOrigin(s, CapabilityOrigin::Remote); // last attached driver was Remote
        // No clients on s.id — every driver detached.
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Remote); // not downgraded to Local
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 0); // still gated as Remote ⇒ denied
    }

    // driver/viewer (the Scope::Drive filter): an OBSERVE-only Remote viewer — token
    // with Observe scope but not Drive, so it cannot inject — does not escalate the
    // output origin. The local drive-capable owner keeps it Local, so the program's
    // DECUDK still LANDS. Without the filter the viewer would escalate to Remote and
    // the DECUDK would be denied (the owner-on-a-2nd-device papercut).
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addLocalClient(d, A::id(s)); // the local drive-capable owner
        A::addObserveOnlyRemoteClient(d, A::id(s)); // a passive remote viewer
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Local); // viewer did not escalate
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 1); // Local ⇒ ProgramKeys allowed ⇒ the key lands
    }

    // ...but a DRIVE-scoped Remote (token WITH Drive — it can inject) does escalate,
    // so the filter excludes only observers, never real drivers: the DECUDK is denied.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addDriveRemoteClient(d, A::id(s));
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Remote); // drive-scoped escalates
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 0); // Remote ⇒ ProgramKeys denied
    }

    // Detach TOCTOU: an Agent driver co-attached with a Local human backgrounds
    // an autonomous side effect (here a DECUDK standing in for the report's OSC-52
    // clipboard write — same HostEscaping tier, Agent ⇒ Prompt), then DETACHES. The
    // backgrounded output lands with only the Local human attached. servicePTY must not
    // downgrade the gate to Local: the sticky driverOriginFloor keeps it at Agent, so
    // the reprogram is still held (udk unchanged). Verify-by-revert: drop the floor
    // ratchet/clamp in servicePTY and the post-detach stamp falls back to Local and the
    // DECUDK lands (udkCount == 1). A command-scoped floor (cleared at OSC-133 D) also
    // fails this — the backgrounded write lands at prompt time, after the boundary.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addLocalClient(d, A::id(s)); // index 0 — the human, stays attached
        A::addAgentClient(d, A::id(s)); // index 1 — the agent, will detach

        A::service(d, s); // both attached: Agent is most-restrictive
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Agent);
        CHECK(A::driverFloor(s) == CapabilityOrigin::Agent); // ratcheted while attached

        A::detachClientAt(d, 1); // the agent EXITs; only the Local human remains
        A::service(d, s); // the backgrounded output drains now
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Agent); // Not downgraded to Local
        CHECK(A::driverFloor(s) == CapabilityOrigin::Agent); // floor stays sticky

        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 0); // still gated as Agent ⇒ Prompt ⇒ no reprogram
    }

    // non-vacuous contrast: a session never driven by an Agent/Remote keeps the floor at
    // Local, so the same backgrounded DECUDK LANDS — proving the assertion above is the
    // floor doing work, not a blanket deny.
    {
        Daemon d;
        auto& s = A::makeSession(d);
        A::addLocalClient(d, A::id(s));
        A::service(d, s);
        CHECK(A::driverFloor(s) == CapabilityOrigin::Local); // no agent ever ⇒ no taint
        A::detachClientAt(d, 0);
        A::service(d, s);
        CHECK(A::stampedOrigin(s) == CapabilityOrigin::Local);
        feedDECUDK(*(A::term(s)));
        CHECK(A::udkCount(s) == 1); // Local ⇒ ProgramKeys allowed ⇒ the key lands
    }

    if (failures == 0)
        std::printf("origin_staleness: servicePTY stamps the OUTPUT origin as the most-restrictive "
                    "DRIVE-capable attached client (Remote>Agent>Local, overflowed + observe-only "
                    "excluded) BEFORE the drain, never the Local default; a detached session keeps its "
                    "last origin; a Remote/Agent DECUDK is denied while a Local one — or one under a "
                    "passive observe-only viewer — lands (driver/viewer)\n");
    return failures == 0 ? 0 : 1;
}
