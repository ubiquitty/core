// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_socket_leaf_identity.cpp — the daemon's unix-socket
// LEAF is a per-binary identity that FAILS CLOSED (never a shared well-known
// leaf) when it can't be computed, and self-exe resolution keeps it stable even
// under a PATH launch where argv[0] is unresolvable.
//
// A predictable shared leaf (a fixed "host.sock" fallback) is a cross-binary-
// bleed surface: any binary that couldn't hash would bind/reuse the same socket.
// So an uncomputable identity yields an EMPTY leaf and the caller refuses to
// run (main.cpp) / connect (macOS) rather than sharing a well-known namespace.
//

#include "Assertions.h"

#include "Platform/Host/HostSocketName.h"

#include <cstdio>
#include <string>

int main()
{
    std::printf("socket_leaf_identity\n");

    // Self-exe resolves to an ABSOLUTE, readable path — this test binary. A
    // PATH-launched daemon's argv[0] is bare ("ubiquitty-host"), but it hashes
    // resolveSelfExecutablePath() instead, so a bare-argv[0] launch still yields a
    // per-binary leaf (the gap a host.sock fallback would paper over).
    const std::string self = Ubiquitty::Host::resolveSelfExecutablePath();
    CHECK(!self.empty());
    CHECK(!self.empty() && self.front() == '/'); // absolute

    // A per-binary leaf: "host-" + 16 hex + ".sock" (26 chars), deterministic.
    const std::string leaf = Ubiquitty::Host::socketLeafForBinary(self.c_str());
    CHECK(!leaf.empty());
    CHECK(leaf.rfind("host-", 0) == 0);
    CHECK(leaf.size() == 5 + 16 + 5);
    CHECK(leaf == Ubiquitty::Host::socketLeafForBinary(self.c_str())); // pure function

    // FAIL CLOSED — an uncomputable identity yields EMPTY, never a shared leaf.
    CHECK(Ubiquitty::Host::socketLeafForBinary(nullptr).empty());
    CHECK(Ubiquitty::Host::socketLeafForBinary("").empty());
    CHECK(Ubiquitty::Host::socketLeafForBinary("/nonexistent/ubiquitty-host-xyz").empty());

    if (failures == 0)
        std::printf("socket_leaf_identity: per-binary leaf + self-exe resolution + "
                    "fail-closed on uncomputable identity, never a shared leaf\n");
    else
        std::printf("%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
