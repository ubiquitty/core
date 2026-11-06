// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Agent/agent_auth_text.cpp — authDeniedReasonText maps every ByeReason
// to a specific message, not the generic fallback, and names the token where
// the connection found it.
//
// authDeniedReasonText (Source/Agent/AgentSession.cpp) turns a wire ByeReason
// into the phrase the agent CLI and MCP server show when the daemon refuses a HELLO.
// Its default arm, for an out-of-range wire byte, reads "authentication denied",
// which is wrong for a refusal that is not about auth. A defined reason that falls
// through to it misleads: ByeReason::VersionMismatch (a handshake protocol-version
// mismatch) is reachable (Daemon.cpp sends it, SyncClientCore delivers it), and
// left to the default it tells the user their token is bad.
//
// This pins the invariant: every defined reason resolves to a specific message,
// and the generic fallback is reserved for an unknown wire value.
//
// Standalone: owns main(), returns its own failure count. Exit 0 on success.
//

#include "Agent/AgentSession.h"

#include <cstdio>
#include <cstring>

using Ubiquitty::Agent::authDeniedReasonText;
using Ubiquitty::Protocol::Sync::ByeReason;

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
    // The phrase authDeniedReasonText returns for a wire byte outside the enum.
    const char* fallback = "authentication denied";

    // Every defined reason must resolve to something other than the fallback — one
    // that falls through mislabels a non-auth refusal as an auth failure.
    const ByeReason defined[] = {
        ByeReason::TokenMissing,
        ByeReason::TokenInvalid,
        ByeReason::TokenExpired,
        ByeReason::TokenRevoked,
        ByeReason::OutsideScope,
        ByeReason::VersionMismatch,
        ByeReason::DuplicateHandshake,
        ByeReason::NoConsent,
        ByeReason::RemoteHostRefused,
        ByeReason::SessionGone,
        ByeReason::HostFull,
    };
    for (ByeReason reason : defined)
        MUST(std::strcmp(authDeniedReasonText(reason), fallback) != 0);

    // The reason a default arm would swallow: a handshake version mismatch
    // must read as a mismatch of builds, which is what a person updates,
    // rather than as an auth failure.
    MUST(std::strstr(authDeniedReasonText(ByeReason::VersionMismatch), "different builds") != nullptr);

    // The fallback is still reserved for an unknown wire value (the default arm stays).
    MUST(std::strcmp(authDeniedReasonText(static_cast<ByeReason>(200)), fallback) == 0);

    // Two specific mappings, so the loop above cannot be satisfied by every reason
    // returning one shared non-fallback string.
    MUST(std::strstr(authDeniedReasonText(ByeReason::TokenExpired), "expired") != nullptr);
    MUST(std::strstr(authDeniedReasonText(ByeReason::TokenRevoked), "revoked") != nullptr);

    // Over ssh a token is the remote host's own, so a refusal of it names the
    // variable ubq reads there and where it comes from, and never the local
    // token, which is not sent.
    using Ubiquitty::Agent::TokenSource;
    const char* remoteMissing = authDeniedReasonText(ByeReason::TokenMissing, TokenSource::Remote);
    MUST(std::strstr(remoteMissing, "UBQ_REMOTE_TOKEN") != nullptr);
    MUST(std::strstr(remoteMissing, "ubiquitty-host token issue") != nullptr);
    MUST(std::strstr(remoteMissing, "UBIQUITTY_AGENT_TOKEN") == nullptr);
    for (ByeReason reason : { ByeReason::TokenInvalid, ByeReason::TokenExpired, ByeReason::TokenRevoked })
        MUST(std::strstr(authDeniedReasonText(reason, TokenSource::Remote), "UBQ_REMOTE_TOKEN") != nullptr);
    MUST(std::strstr(authDeniedReasonText(ByeReason::TokenMissing), "UBIQUITTY_AGENT_TOKEN") != nullptr);
    MUST(std::strstr(authDeniedReasonText(ByeReason::TokenMissing), "UBQ_REMOTE_TOKEN") == nullptr);
    // A refusal that is not about the token reads the same from either side.
    MUST(std::strcmp(authDeniedReasonText(ByeReason::HostFull, TokenSource::Remote), authDeniedReasonText(ByeReason::HostFull)) == 0);

    if (failures) {
        std::fprintf(stderr, "agent_auth_text: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("agent_auth_text: ok");
    return 0;
}
