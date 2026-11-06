// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Agent/agent_screen_diff.cpp — computeScreenDiff, the client-side screen
// diff behind the agent's read_changes tool.
//
// computeScreenDiff (Source/Agent/AgentSession.cpp) is a pure projection→projection
// function: it takes two screenText() renderings and reports what moved between them,
// independently of the wire's Updates. It is pure (std::string in, std::string out),
// so this drives it with no daemon and no AgentSession instance; the rest of that TU
// is dropped at link (dead_strip / --gc-sections), which is why the stanza links only
// AgentSession.cpp and no Sync/PAL/Terminal sources.
//
// Standalone: owns main(), returns its own failure count. Exit 0 on success.
//

#include "Agent/AgentSession.h"

#include <cstdio>
#include <string>

using Ubiquitty::Agent::computeScreenDiff;

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
    // First read: an empty previous is the baseline, so the whole current comes back
    // verbatim rather than as a diff of every row against nothing.
    MUST(computeScreenDiff("", "a\nb") == "a\nb");

    // Two identical projections report the sentinel, not an empty string — the caller
    // distinguishes "nothing changed" from "no answer".
    MUST(computeScreenDiff("a\nb", "a\nb") == "(no change)");

    // A row that changed prints ~rowN with the new text; row indices are 0-based.
    MUST(computeScreenDiff("a\nb", "a\nX") == "~row1: X\n");
    MUST(computeScreenDiff("a\nb\nc", "a\nB\nc") == "~row1: B\n");

    // A row present only in the current projection is an addition (+rowN: text).
    MUST(computeScreenDiff("a", "a\nb") == "+row1: b\n");

    // A row present only in the previous projection is a removal (-rowN, no text).
    MUST(computeScreenDiff("a\nb", "a") == "-row1\n");

    if (failures) {
        std::fprintf(stderr, "agent_screen_diff: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("agent_screen_diff: ok");
    return 0;
}
