// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_term_lifecycle.cpp — ASan guard for the bare Ubiquitty::Terminal lifecycle.
//
// Constructing a Ubiquitty::Terminal runs CellGrid::init, whose grow path calls freeBuffers()
// before allocBuffers(); on a fresh CellGrid that frees every buffer pointer
// pre-alloc, so the ctor must null-initialize each one. A buffer pointer left
// uninitialized makes the plain `Ubiquitty::Terminal t;` below delete[] a garbage pointer —
// UB that ASan traps and release builds mask (the m_rowGeneration regression).
// Built + run under -fsanitize=address by run-tests.sh.
//

#include "Core/Modes/HostPolicy.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

static_assert(!std::is_polymorphic_v<Ubiquitty::Terminal>);
static_assert(!std::is_copy_constructible_v<Ubiquitty::Terminal>);
static_assert(!std::is_move_constructible_v<Ubiquitty::Terminal>);

// A fresh process's heap often hands out zeroed pages, on which delete[] of an
// UNINITIALIZED pointer member reads as delete[] nullptr and never faults —
// masking exactly the bug this guards (ASan's malloc fill also caps at 4 KiB,
// short of a large object's tail). Churn a spread of allocation sizes with a
// nonzero fill so the CellGrid lands on recycled dirty memory and any
// uninitialized buffer pointer holds trapping garbage, not zero.
static void dirtyHeap()
{
    enum {
        blockCount = 512
    };
    void* blocks[blockCount];
    for (int i = 0; i < blockCount; ++i) {
        const size_t size = 64 + static_cast<size_t>(i % 128) * 128; // 64 B … 16 KiB
        blocks[i] = malloc(size);
        if (blocks[i])
            memset(blocks[i], 0x5A, size);
    }
    for (int i = 0; i < blockCount; ++i)
        free(blocks[i]);
}

int main()
{
    dirtyHeap();
    {
        // Construct: CellGrid::init's first grow frees all buffers before the
        // first alloc — every buffer pointer must already be null.
        Ubiquitty::Terminal t;
        // Destruct without ever feeding input: freeBuffers runs again.
    }
    {
        // A real grow re-runs the freeBuffers→allocBuffers cycle on live buffers.
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 50, 200);
    }
    for (int iteration = 0; iteration < 64; ++iteration) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 24 + iteration % 4, 80 + iteration % 8);
        Ubiquitty::hardReset(terminal);
    }
    {
        // hardReset survival contract: frontend configuration, security
        // policy, and geometry survive; application-visible state (modes,
        // screen content, cursor) returns to power-on defaults.
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 30, 100);
        Ubiquitty::setScrollbackLimit(&t, 12345);
        Ubiquitty::setAnswerback(&t, "answer");
        Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
        Ubiquitty::Modes::HostPolicy::setTitlePolicy(&t, Ubiquitty::OperationPolicy::Denied);
        // App-visible state that must not survive: alternate screen, insert
        // mode, printed content.
        Ubiquitty::processBytes(&t, "\033[4h\033[?1049hX");
        if (!t.screens.isUsingAlternateScreen || !t.modes.insertMode) {
            printf("FAIL: pre-reset app state did not arm\n");
            return 1;
        }

        Ubiquitty::hardReset(t);

        int failures = 0;
        failures += !(t.rows == 30 && t.columns == 100);
        failures += !(t.scrollbackLimit == 12345);
        failures += !(t.answerback == "answer");
        failures += !(Ubiquitty::Modes::inputEncoding(&t) == Ubiquitty::Unicode::Encoding::Latin1);
        failures += !(t.allowTitleOperations == false);
        failures += t.screens.isUsingAlternateScreen;
        failures += t.modes.insertMode;
        failures += !(t.screens.active == &t.screens.primary);
        const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
        failures += !(c && (c->codePoint == ' ' || c->codePoint == 0));
        if (failures) {
            printf("FAIL: hardReset survival contract (%d)\n", failures);
            return 1;
        }
    }
    printf("term lifecycle ASan-clean\n");
    return 0;
}
