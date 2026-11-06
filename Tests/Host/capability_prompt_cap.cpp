// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/capability_prompt_cap.cpp — a dropped capability prompt is a
// question put back, not a question lost.
//
// This is the capability-grant consent path, which is not the attach gate.
// Nothing waits on it: a capability that needs a ruling is refused now and
// asked about again the next time the program reaches for it. Its records are
// therefore removed by a human ruling, a session ending, or a revoke — and an
// agent reconnecting under a fresh identity each time produces triples none of
// those ever reach.
//
// The table is capped for that. What the cap must not do is drop a record while
// its triple still counts as pending in the consent queue: the queue dedupes on
// the triple, so the next reach would read as a repeat, no prompt would go out,
// and the policy would answer Prompt forever with nobody able to rule. The
// eviction resolves and erases together, and that pairing is what this pins.
//
// Exit 0 on success.
//

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "PAL/Capability.h"
#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <limits>
#include <string>

namespace Ubiquitty {
namespace Host {

struct CapabilityPromptCapAccess {
    // One outstanding prompt, recorded the way sendCapabilityGrantRequests
    // records one: the queue holds the triple, the table holds what the request
    // id means.
    static void recordPrompt(Daemon& d, std::uint32_t requestID, Capability capability, const std::string& identity)
    {
        d.m_consentQueue.recordPending(capability, CapabilityOrigin::Agent, identity);
        d.m_pendingCapabilityGrants[requestID] = { .capability = capability, .origin = CapabilityOrigin::Agent, .identity = identity, .sessionID = 1, .deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max(), .recipients = {} };
    }

    static void evictIfFull(Daemon& d) { d.evictOldestCapabilityGrantIfFull(); }
    static std::size_t outstanding(const Daemon& d) { return d.m_pendingCapabilityGrants.size(); }
    static bool holds(const Daemon& d, std::uint32_t requestID) { return d.m_pendingCapabilityGrants.contains(requestID); }

    static bool queueHasPending(const Daemon& d, Capability capability, const std::string& identity)
    {
        return const_cast<Daemon&>(d).m_consentQueue.isPending(capability, CapabilityOrigin::Agent, identity);
    }

    // The cap itself is file-local to Daemon.cpp. Filling until eviction bites
    // finds it without naming it, so this test does not re-pin the number.
    static std::size_t fillUntilEviction(Daemon& d)
    {
        std::uint32_t requestID = 1;
        for (; requestID < 4096; ++requestID) {
            evictIfFull(d);
            recordPrompt(d, requestID, Capability::ClipboardWrite, "agent-" + std::to_string(requestID));
            if (outstanding(d) < requestID)
                return requestID; // this insert displaced an earlier one
        }
        return 0;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Capability;
using Ubiquitty::Host::CapabilityPromptCapAccess;
using Ubiquitty::Host::Daemon;

static int failures = 0;
static void check(bool isSo, const char* what) // bool-ok: matches every Tests/Host/*.cpp check() harness
{
    if (!isSo) {
        std::fprintf(stderr, "capability_prompt_cap: FAIL — %s\n", what);
        ++failures;
    }
}

int main()
{
    // (a) Below the cap nothing is displaced. A table that evicted eagerly
    //     would re-ask questions a human is still looking at.
    {
        Daemon d;
        for (std::uint32_t requestID = 1; requestID <= 8; ++requestID) {
            CapabilityPromptCapAccess::evictIfFull(d);
            CapabilityPromptCapAccess::recordPrompt(d, requestID, Capability::ClipboardWrite, "agent-" + std::to_string(requestID));
        }
        check(CapabilityPromptCapAccess::outstanding(d) == 8, "eight prompts below the cap are all outstanding");
        check(CapabilityPromptCapAccess::holds(d, 1), "including the first, which is not dropped to make room it did not need");
    }

    // (b) At the cap the oldest goes, and its triple goes back into reach.
    {
        Daemon d;
        const std::size_t cap = CapabilityPromptCapAccess::fillUntilEviction(d);
        check(cap != 0, "the table is capped rather than growing without bound");

        const std::string oldestIdentity = "agent-1";
        check(!CapabilityPromptCapAccess::holds(d, 1), "the oldest request id is the one displaced");
        check(!CapabilityPromptCapAccess::queueHasPending(d, Capability::ClipboardWrite, oldestIdentity),
            "and its triple is resolved, so the next reach asks again rather than reading as a repeat nobody answers");
        check(CapabilityPromptCapAccess::holds(d, 2), "the next-oldest is still outstanding");
        check(CapabilityPromptCapAccess::queueHasPending(d, Capability::ClipboardWrite, "agent-2"),
            "and its triple still counts as asked, because its prompt is still live");
    }

    // (c) The table stops growing. Past the cap each new prompt costs an old
    //     one rather than a slot.
    {
        Daemon d;
        const std::size_t cap = CapabilityPromptCapAccess::fillUntilEviction(d);
        const std::size_t atCap = CapabilityPromptCapAccess::outstanding(d);
        for (std::uint32_t requestID = static_cast<std::uint32_t>(cap) + 1; requestID <= cap + 64; ++requestID) {
            CapabilityPromptCapAccess::evictIfFull(d);
            CapabilityPromptCapAccess::recordPrompt(d, requestID, Capability::ClipboardWrite, "agent-" + std::to_string(requestID));
        }
        check(CapabilityPromptCapAccess::outstanding(d) == atCap, "sixty-four more prompts leave the table the same size");
        check(CapabilityPromptCapAccess::holds(d, static_cast<std::uint32_t>(cap) + 64), "the newest is outstanding");
    }

    if (failures == 0) {
        std::printf("capability_prompt_cap: PASSED — outstanding capability prompts are capped; below the cap none "
                    "is displaced; at it the oldest is dropped AND its triple resolved, so the capability is asked "
                    "about again rather than suppressed as a repeat nobody can answer\n");
        return 0;
    }
    std::fprintf(stderr, "capability_prompt_cap: %d failure(s)\n", failures);
    return 1;
}
