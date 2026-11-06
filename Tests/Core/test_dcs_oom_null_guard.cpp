// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Terminal.h"
#include "Core/VT/ParseState.h"
#include "Harness.h"

// test_dcs_oom_null_guard — the DECRQSS reply path and resetOSCBuffer
// must not dereference a null osc.data.
//
// THREAT / SHAPE: hardReset allocates the 256-byte osc.data up front, so it is
// null only if that malloc failed (OOM). decrqssDispatch (Source/Core/VT/VT.cpp)
// writes the NUL terminator `p[plen] = 0` on t->parser.osc.data, and resetOSCBuffer
// (Source/Core/VT/ParseState.cpp) writes `osc.data[0] = 0`; without a null check,
// under OOM an empty DECRQSS (ESC P $ q ESC \) or a parse-state reset would write
// through null → SIGSEGV. Not attacker-reachable on a system where a 256-byte
// malloc succeeds, so both writes carry a defensive null guard.
//
// This test SIMULATES the OOM post-condition: hardReset a Terminal, then free its
// osc.data and null the pointer/capacity, exactly the state a failed init malloc
// leaves. It then (1) drives an empty DECRQSS through the real parser so
// decrqssDispatch runs with a null buffer, and (2) calls resetOSCBuffer directly.
// With the guards this is crash-free; reverting either guard SIGSEGVs here.
//
// Exit 0 on success (reaching the end without crashing is the assertion).

#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Ubiquitty;

// Put `t` into the exact state a failed hardReset osc.data malloc leaves:
// buffer freed, pointer null, length/capacity zero.
static void forceOSCBufferNull(Ubiquitty::Terminal& t)
{
    std::free(t.parser.osc.data);
    t.parser.osc.data = nullptr;
    t.parser.osc.length = 0;
    t.parser.osc.capacity = 0;
}

// DECRQSS with an EMPTY selector: ESC P $ q ESC \. The header is recognized
// without allocating osc.data and there is no payload byte, so the only
// osc.data access is decrqssDispatch's terminator write — the null-guard site.
// A non-empty DECRQSS selector under OOM: the payload bytes route through the
// DCS accumulator (which must also tolerate a null buffer via reserveOSCBuffer),
// and decrqssDispatch still runs. Exercises the guard with osc.length > 0
// intent (the accumulator refuses to grow, so length stays 0 — still safe).
// resetOSCBuffer directly on a null-buffer Terminal (the wedge-recovery path a PAL
// may invoke): must reset length/parse-state without writing through null.
TEST(testDECRQSSEmptyNullBuffer)
{
    std::printf("testDecrqssEmptyNullBuffer\n");
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    forceOSCBufferNull(t);

    static const char decrqss[] = { 0x1b, 'P', '$', 'q', 0x1b, '\\' };
    Ubiquitty::processBytes(&t, std::string_view(decrqss, sizeof(decrqss)));

    // No crash → the guard held. osc.data stays null (nothing reallocated it).
    CHECK(t.parser.osc.data == nullptr);
}

TEST(testDECRQSSPayloadNullBuffer)
{
    std::printf("testDecrqssPayloadNullBuffer\n");
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    forceOSCBufferNull(t);

    static const char decrqss[] = { 0x1b, 'P', '$', 'q', 'm', 0x1b, '\\' }; // "$q m" = SGR query
    Ubiquitty::processBytes(&t, std::string_view(decrqss, sizeof(decrqss)));
    CHECK(t.parser.osc.length <= t.parser.osc.capacity); // invariant holds (0 <= 0)
}

TEST(testResetOSCBufferNull)
{
    std::printf("testResetOSCBufferNull\n");
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    forceOSCBufferNull(t);

    Ubiquitty::VT::resetOSCBuffer(&t);
    CHECK(t.parser.osc.length == 0);
    CHECK(t.parser.osc.data == nullptr);
}

UBIQUITTY_TEST_MAIN()
