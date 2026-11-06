// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/origin_unlabeled_agent.cpp — security negative for the
// unlabeled-local-peer origin escalation:
//
// The hazard (Daemon.cpp::originOf):
//   peerProcess non-empty + quicConnection null ⟹ verified-local.
//   An origin read from the label alone — Agent when agentLabel is set, Local
//   otherwise — would hand Local to a client that omits agentLabel, whatever
//   binary it is. Local resolves to HostCapabilityPolicy Allow for every tier,
//   so an agent / MCP client that omits agentLabel would escape the Agent-tier
//   gate and get Allow for HostEscaping capabilities (ExecuteTrigger, …)
//   without a human prompt (AgentSession.h: agentLabel is a "spoofable" display hint).
//
// The contract:
//   originOf is fail-safe. After the Remote guard, Local is granted only
//   when basename(peerProcess) is one of the daemon's own trusted binaries —
//   "ubiquitty-host" (the attach bridge) or "Ubiquitty" (the GUI app connecting
//   directly for ADOPT_SESSION). Every other unlabeled verified-local
//   peer is Agent (the gated tier).
//
// This file covers the cases that contract names:
//
//   (a) ESCALATION PROOF / GUI SPECIFICITY — arbitrary non-trusted peer
//       ("/bin/zsh"): originOf must return Agent (not Local), and the policy
//       must not Allow ExecuteTrigger. This is also the guard that the GUI's
//       Local grant stays specific — an unlabeled non-GUI same-uid local peer
//       is still Agent: label-absence earns an unrecognised binary nothing.
//       What this guards is recognition, not the account; a same-user process
//       can start the bridge itself (Docs/capability-origin.md).
//
//   (b) BRIDGE REGRESSION GUARD — the ubiquitty-host attach bridge
//       ("/Applications/Ubiquitty.app/Contents/Helpers/ubiquitty-host"):
//       originOf must return Local; policy Allows ExecuteTrigger.
//
//   (c) GUI-APP ADOPT — the Ubiquitty GUI app itself
//       ("/Applications/Ubiquitty.app/Contents/MacOS/Ubiquitty"), which
//       connects DIRECTLY for Share This Window's ADOPT_SESSION (SCM_RIGHTS
//       can't cross the --attach relay): originOf must return Local so the
//       adopt's !isAgentClient gate admits it. The ownership walk remains
//       the hard gate (an adopt of a non-owned session still rejected — the
//       ownership half is covered by adopt_fd_cli's non-owner-sibling case).
//
// Exit 0 only when all cases pass.
//

#include "PAL/Capability.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/TrustedLocalBinaries.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Friend seam: construct a minimal Daemon, inject one Client with the given
// peerProcess path (empty agentLabel, null quicConnection = verified-local),
// and return originOf's classification + the ExecuteTrigger policy decision.
// Daemon::Client is private and cannot be named outside the seam.
struct UnlabeledAgentAccess {
    static void classify(const std::string& peerProcess, Ubiquitty::CapabilityOrigin& outOrigin, Ubiquitty::CapabilityDecision& outDecision)
    {
        classify(peerProcess, std::vector<BinaryIdentity> {}, outOrigin, outDecision);
    }

    // Accept a peer at one path, then put a link to another file in its place
    // before classifying. The daemon takes identity at accept, so the swap must
    // not move the answer; a check that resolved the path later would.
    //
    // False, with nothing classified, when the link could not be made: the
    // case would then ask about a path that names no file and prove nothing.
    [[nodiscard]] static bool classifyAfterSwap(const std::string& peerProcess, const std::string& becomes,
        const std::vector<BinaryIdentity>& trusted,
        Ubiquitty::CapabilityOrigin& outOrigin, Ubiquitty::CapabilityDecision& outDecision)
    {
        Daemon d;
        d.m_trustedLocalBinaries = trusted;
        {
            Daemon::Client c {};
            c.sessionID = 1;
            c.paneSubscriptions = { { .sessionID = 1 } };
            c.workspaceID = 1;
            c.peerProcess = peerProcess;
            c.quicConnection = nullptr;
            c.agentLabel = "";
            c.hasPeerBinary = Ubiquitty::Host::binaryIdentityOf(peerProcess, c.peerBinary);
            d.m_clients.push_back(std::move(c));
        }
        ::unlink(peerProcess.c_str());
        if (::symlink(becomes.c_str(), peerProcess.c_str()) != 0)
            return false;
        outOrigin = d.originOf(d.m_clients[0]);
        Ubiquitty::HostCapabilityPolicy policy;
        outDecision = policy.check(Ubiquitty::Capability::ExecuteTrigger, outOrigin, {});
        return true;
    }

    // The daemon resolves the binaries it trusts from its own executable, which
    // for this test is the test binary. Cases about the bridge and the interface
    // hand it the identities of files they made instead, because what they are
    // about is a peer being one of those exact files.
    static void classify(const std::string& peerProcess, const std::vector<BinaryIdentity>& trusted,
        Ubiquitty::CapabilityOrigin& outOrigin, Ubiquitty::CapabilityDecision& outDecision)
    {
        Daemon d;
        d.m_trustedLocalBinaries = trusted;
        {
            Daemon::Client c {};
            c.sessionID = 1;
            c.paneSubscriptions = { { .sessionID = 1 } };
            c.workspaceID = 1;
            c.peerProcess = peerProcess; // daemon-verified path (non-empty = local)
            // What the daemon takes at accept, from what the kernel resolved.
            // Taken here the same way, so a case that swaps the path afterwards
            // is asking the question the daemon asks.
            c.hasPeerBinary = Ubiquitty::Host::binaryIdentityOf(peerProcess, c.peerBinary);
            c.quicConnection = nullptr;
            c.agentLabel = ""; // no label sent — the path a peer takes when it names nothing
            d.m_clients.push_back(std::move(c));
        }
        outOrigin = d.originOf(d.m_clients[0]);

        // Check the policy at the origin originOf() produced.
        // Local  → HostCapabilityPolicy returns Allow  for every tier (the escalation).
        // Agent  → HostCapabilityPolicy returns Prompt for HostEscaping (gated, correct).
        // ExecuteTrigger is CapabilityTier::HostEscaping (`tierOf` in PAL/Capability.h).
        Ubiquitty::HostCapabilityPolicy policy;
        outDecision = policy.check(Ubiquitty::Capability::ExecuteTrigger, outOrigin, {});
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::CapabilityDecision;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::BinaryIdentity;
using Ubiquitty::Host::binaryIdentityOf;
using Ubiquitty::Host::bundledInterfacePathForHost;
using Ubiquitty::Host::mergeTrustedLocalBinaries;
using Ubiquitty::Host::pathWithoutDeletedMarker;
using Ubiquitty::Host::resolveTrustedLocalBinaries;
using Ubiquitty::Host::UnlabeledAgentAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "origin_unlabeled_agent: FAIL — %s\n", msg);
        ++failures;
    }
}

namespace {

// A bundle laid out the way the installed one is, plus a copy of the daemon's
// own name somewhere anyone can write. Real files, because the check compares
// device and inode and there is nothing to compare about a path that is not one.
struct FakeInstall {
    std::string root;
    std::string host; // <root>/Ubiquitty.app/Contents/Helpers/ubiquitty-host
    std::string interface; // <root>/Ubiquitty.app/Contents/MacOS/Ubiquitty
    std::string copy; // <root>/ubiquitty-host — the same name, a different file

    FakeInstall()
    {
        char pattern[] = "/tmp/ubiquitty-origin-XXXXXX";
        root = ::mkdtemp(pattern);
        const std::string bundle = root + "/Ubiquitty.app";
        ::mkdir(bundle.c_str(), 0700);
        ::mkdir((bundle + "/Contents").c_str(), 0700);
        ::mkdir((bundle + "/Contents/Helpers").c_str(), 0700);
        ::mkdir((bundle + "/Contents/MacOS").c_str(), 0700);
        host = bundle + "/Contents/Helpers/ubiquitty-host";
        interface = bundle + "/Contents/MacOS/Ubiquitty";
        copy = root + "/ubiquitty-host";
        for (const std::string& path : { host, interface, copy })
            std::fclose(std::fopen(path.c_str(), "w"));
    }

    ~FakeInstall()
    {
        for (const std::string& path : { host, interface, copy })
            ::unlink(path.c_str());
        const std::string bundle = root + "/Ubiquitty.app";
        ::rmdir((bundle + "/Contents/MacOS").c_str());
        ::rmdir((bundle + "/Contents/Helpers").c_str());
        ::rmdir((bundle + "/Contents").c_str());
        ::rmdir(bundle.c_str());
        ::rmdir(root.c_str());
    }
};

} // namespace

int main()
{
    const FakeInstall install;
    const std::vector<BinaryIdentity> trusted = resolveTrustedLocalBinaries(install.host);
    // (a) ESCALATION PROOF — arbitrary non-bridge peer.
    //     An unlabeled verified-local binary that is not ubiquitty-host must be
    //     Agent-gated, not Local; an originOf() that returns Local here is the escalation.
    {
        CapabilityOrigin origin {};
        CapabilityDecision decision {};
        UnlabeledAgentAccess::classify("/bin/zsh", origin, decision);

        check(origin == CapabilityOrigin::Agent, "an arbitrary unlabeled local binary must be Agent-gated, not Local/Allow");

        check(decision != CapabilityDecision::Allow,
            "an arbitrary unlabeled local binary must NOT get Allow for a host-escaping "
            "capability (ExecuteTrigger): policy returned Allow, proving the escalation");
    }

    // (b) GUI-ATTACH REGRESSION GUARD — the trusted attach bridge.
    //     The bridge is a second process of the daemon's own executable, so the
    //     peer it presents is that exact file. It must stay Local or the GUI
    //     attach path is over-gated.
    {
        CapabilityOrigin origin {};
        CapabilityDecision decision {};
        UnlabeledAgentAccess::classify(install.host, trusted, origin, decision);

        check(origin == CapabilityOrigin::Local,
            "the ubiquitty-host attach bridge must stay Local — "
            "the marker must not over-gate the GUI");

        check(decision == CapabilityDecision::Allow,
            "the ubiquitty-host attach bridge must get Allow for ExecuteTrigger at Local origin — "
            "the marker must not over-gate the GUI");
    }

    // (b2) The copy. A file with the daemon's own name, in a directory anyone
    //      can write, is a different file. Comparing the last component of the
    //      path made the whole Local tier reachable by choosing a filename:
    //      `cp /bin/sh /tmp/ubiquitty-host` and any program running as the user
    //      held full Allow, past the consent prompt and past the Agent floor.
    {
        CapabilityOrigin origin {};
        CapabilityDecision decision {};
        UnlabeledAgentAccess::classify(install.copy, trusted, origin, decision);

        check(origin == CapabilityOrigin::Agent,
            "a file named ubiquitty-host that is not the daemon's own must be Agent-gated — "
            "the Local tier is an identity, not a filename");
        check(decision != CapabilityDecision::Allow,
            "a file named ubiquitty-host that is not the daemon's own must not get Allow "
            "for a host-escaping capability");
    }

    // (b3) The interface is found beside the daemon rather than named, and a
    //      daemon that is not in a bundle has none to find. Both halves, because
    //      a derivation that returned something for every path would hand Local
    //      to whatever happened to sit at that offset.
    {
        check(bundledInterfacePathForHost(install.host) == install.interface,
            "the bundled interface is derived from the daemon's own path");
        check(bundledInterfacePathForHost("/usr/local/bin/ubiquitty-host").empty(),
            "a daemon outside a bundle has no interface beside it, and gets no guess");
        check(resolveTrustedLocalBinaries("/usr/local/bin/ubiquitty-host").empty(),
            "a path that names no file yields no identity to trust");
    }

    // (b3b) The swap. The identity is taken once, at accept, because a path is
    //       a name too: `::stat` resolves it as it stands at the moment of the
    //       call, and the peer chooses what stands at its own. Connect from a
    //       file nobody trusts, put a link to the daemon's binary in its place,
    //       then send HELLO — a check that reads the path at classification time
    //       reads the wrong file and answers Local.
    {
        const std::string swapped = install.root + "/swapped";
        std::fclose(std::fopen(swapped.c_str(), "w"));

        // Accepted while it is its own file, which is not trusted.
        CapabilityOrigin origin {};
        CapabilityDecision decision {};
        const bool didSwap = UnlabeledAgentAccess::classifyAfterSwap(swapped, install.host, trusted, origin, decision);

        check(didSwap, "the peer's path became a link to the daemon's binary");
        check(origin == CapabilityOrigin::Agent,
            "a peer that replaces its own path with a link to the daemon's binary after "
            "connecting stays Agent — the identity was taken at accept");
        check(decision != CapabilityDecision::Allow,
            "a peer that swaps its path after connecting does not get Allow for a "
            "host-escaping capability");
        ::unlink(swapped.c_str());
    }

    // (b4) The Linux half, which is string handling and so runs everywhere.
    //      /proc/self/exe tracks the inode rather than the name, so a daemon
    //      whose file has been replaced under it reads `<path> (deleted)` — a
    //      string that stats to nothing. Stripping the marker is what lets the
    //      replacement be found: without it such a daemon trusts only a file
    //      that is gone, and the next bridge is an agent facing a consent
    //      prompt that, on a headless host, nobody is there to answer.
    {
        check(pathWithoutDeletedMarker("/usr/bin/ubiquitty-host (deleted)") == "/usr/bin/ubiquitty-host",
            "the deleted-file marker is stripped, so the replacement can be found");
        check(pathWithoutDeletedMarker("/usr/bin/ubiquitty-host") == "/usr/bin/ubiquitty-host",
            "a path without the marker is returned unchanged");
        check(pathWithoutDeletedMarker(" (deleted)") == " (deleted)",
            "a path that is only the marker names no file and is left alone");
        check(pathWithoutDeletedMarker("").empty(), "an unresolved path stays empty");

        // The daemon's own file, met again through a path carrying the marker:
        // the identity is the same, so an upgraded daemon still trusts itself.
        std::vector<BinaryIdentity> merged = resolveTrustedLocalBinaries(install.host);
        const std::size_t before = merged.size();
        mergeTrustedLocalBinaries(install.host + " (deleted)", merged);
        check(merged.size() == before,
            "merging the same files through a marked path adds no duplicate identity");
    }

    // (c) GUI-APP ADOPT — the Ubiquitty GUI app connects
    //     directly (not via the bridge) for Share This Window's ADOPT_SESSION,
    //     so its own binary basename must be Local or the adopt's !isAgentClient
    //     gate rejects it. An originOf that returns Agent here blocks the adopt
    //     (Share This Window dead even with the ownership check working).
    {
        CapabilityOrigin origin {};
        CapabilityDecision decision {};
        UnlabeledAgentAccess::classify(install.interface, trusted, origin, decision);

        check(origin == CapabilityOrigin::Local,
            "the Ubiquitty GUI app must be Local so its direct ADOPT_SESSION passes the "
            "!isAgentClient gate — the Share This Window unblock");
        check(decision == CapabilityDecision::Allow, "the Ubiquitty GUI app must get Allow at Local origin");
    }

    if (failures == 0)
        std::printf("origin_unlabeled_agent: PASSED — "
                    "arbitrary unlabeled local binary is Agent-gated (not Local); "
                    "the bridge and the interface beside it are Local by identity, and a "
                    "file that merely bears the bridge's name is not; "
                    "the unlabeled-binary escalation is closed and the GUI adopt is unblocked "
                    "without opening Local to any other local peer\n");
    return failures == 0 ? 0 : 1;
}
