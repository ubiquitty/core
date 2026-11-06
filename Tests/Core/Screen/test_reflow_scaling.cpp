// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Rewrapping one logical line is linear in that line's length.
//
// A resize rejoins soft-wrapped rows into logical lines and re-splits them at
// the new width. The reflow lanes vary the line COUNT; nothing varies the length
// of a single line, which is what `cat` of a minified file produces — one line
// of megabytes wrapped across thousands of rows. A rejoin quadratic in that
// length turns a window drag into a hang, and every fixed-size lane stays green
// while it does.
//
// The work is counted, not timed. A clock on a shared runner reads the
// neighbours' turns as well as this thread's, and a 4x input once read x9.46
// against a wall-clock bound of 8 with no defect present. The reflow counts the
// cells it copies — each cell once into the row under construction and once
// into the ring — so two lengths 4x apart give a ratio of exactly 4, and the
// bound at 4.5 is room for a fixed per-row cost, not for noise. A ratio that
// drifts here is a change in what the rewrap does, and wants diagnosing rather
// than re-tuning.

#include "Core/Screen/Resize.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

constexpr std::size_t smallLine = 256u * 1024u;
constexpr std::size_t largeLine = 1024u * 1024u;
constexpr double quadraticRatio = 4.5;

std::uint64_t cellsCopiedRewrapping(std::size_t lineBytes)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    term->scrollbackLimit = 100000;
    Ubiquitty::resize(*term, 50, 200);
    Ubiquitty::processBytes(term.get(), std::string_view(std::string(lineBytes, 'x')));

    const std::uint64_t before = Ubiquitty::Screen::reflowCellsCopiedForTest;
    Ubiquitty::resize(*term, 50, 137); // a width no column count divides
    // A settled resize rewraps inside the call; the settle is the pair a
    // frontend makes and stays so the case drives what a frontend drives.
    Ubiquitty::settleResize(term.get());
    return Ubiquitty::Screen::reflowCellsCopiedForTest - before;
}

TEST(rewrappingOneLogicalLineIsLinearInItsLength)
{
    const std::uint64_t small = cellsCopiedRewrapping(smallLine);
    const std::uint64_t large = cellsCopiedRewrapping(largeLine);
    const double ratio = small > 0 ? static_cast<double>(large) / static_cast<double>(small) : 0.0;
    char message[192];
    std::snprintf(message, sizeof(message),
        "%zu bytes rewrapped with %llu cell copies, %zu bytes with %llu (x%.3f, quadratic past x%.1f)",
        smallLine, static_cast<unsigned long long>(small), largeLine, static_cast<unsigned long long>(large), ratio, quadraticRatio);
    CHECK_MSG(small > 0, "the rewrap copied something, so the count is wired");
    CHECK_MSG(ratio < quadraticRatio, message);
}

} // namespace

UBIQUITTY_TEST_MAIN()
