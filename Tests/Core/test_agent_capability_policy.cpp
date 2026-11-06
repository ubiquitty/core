// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Agent/AgentCapabilityPolicy.h"
#include "PAL/Capability.h"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace Ubiquitty;

/// The agent session's terminal refuses every capability, whoever asks.
///
/// No gate site is reachable on it today — its grid is assembled from wire
/// frames and its parser never runs — so nothing else in the suite can observe
/// this decision. That is the reason to assert it directly: the policy exists
/// for the client someone wires later, and a rule nothing checks is a rule that
/// drifts to whatever the next edit leaves behind.
TEST(testTheAgentPolicyRefusesEveryOriginAndCapability)
{
    Agent::AgentCapabilityPolicy policy;

    const Capability worst[] = {
        Capability::ExecuteTrigger,
        Capability::FileReadAccess,
        Capability::ClipboardRead,
        Capability::ClipboardWrite,
        Capability::ProgramKeys,
        Capability::ScreenContentRead,
    };
    const CapabilityOrigin origins[] = {
        CapabilityOrigin::Local,
        CapabilityOrigin::Agent,
        CapabilityOrigin::Remote,
    };

    for (Capability capability : worst)
        for (CapabilityOrigin origin : origins)
            CHECK(policy.check(capability, origin, {}) == CapabilityDecision::Deny);

    // Local is the one that matters most here and is easiest to get wrong: the
    // module stamps no origin, so its terminal carries Local by default, and a
    // Local-allows policy — the shape every other frontend uses — would grant
    // everything on the module whose principal the gate exists to refuse.
    CHECK(policy.check(Capability::ExecuteTrigger, CapabilityOrigin::Local, {}) == CapabilityDecision::Deny);

    // A benign-tier reply is refused too. Nothing here has a pty to answer, so
    // there is no reader to keep working and no reason to carve an exception
    // that a later capability could be filed under.
    CHECK(policy.check(Capability::StatusReport, CapabilityOrigin::Local, {}) == CapabilityDecision::Deny);
    CHECK(policy.check(Capability::None, CapabilityOrigin::Local, {}) == CapabilityDecision::Deny);
}

/// The refusal says so once, not per effect.
///
/// A mirror that started receiving effects would flood the channel, the flood
/// would get muted, and that is silence again with extra steps — which is the
/// state the line exists to fix.
TEST(testTheRefusalIsAnnouncedOnceNotPerEffect)
{
    char pattern[] = "/tmp/ubq-policy-XXXXXX";
    const int descriptor = ::mkstemp(pattern);
    if (descriptor >= 0)
        ::close(descriptor);

    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    CHECK(std::freopen(pattern, "w", stderr) != nullptr);

    Agent::AgentCapabilityPolicy policy;
    for (int attempt = 0; attempt < 8; ++attempt)
        (void)policy.check(Capability::ClipboardWrite, CapabilityOrigin::Agent, {});

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    std::string said;
    if (FILE* f = std::fopen(pattern, "r")) {
        char chunk[512];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            said.append(chunk, n);
        std::fclose(f);
    }
    std::remove(pattern);

    // Eight refusals, one line.
    int lines = 0;
    for (char c : said)
        if (c == '\n')
            ++lines;
    CHECK_EQ(lines, 1);
    CHECK(said.find("It is a mirror, not a surface that acts.") != std::string::npos);
    // The line reaches users of an installed binary, who have no source tree to
    // look a path up in.
    CHECK(said.find("Source/") == std::string::npos);
}

UBIQUITTY_TEST_MAIN()
