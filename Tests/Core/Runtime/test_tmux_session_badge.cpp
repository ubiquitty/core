// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tmux_session_badge.cpp — the tmux session pill's colour and label
// (Source/Core/Runtime/TmuxSessionBadge.h): a stored pick wins when it names
// a fill, a session nobody picked for takes a default that holds across
// launches, and the label is whichever pole reads on the fill by APCA.

#include "Core/Runtime/TmuxSessionBadge.h"
#include "Core/Themes/Contrast.h"

#include <cmath>
#include <set>

using namespace Ubiquitty::Runtime;
namespace Contrast = Ubiquitty::Themes::Contrast;

// The default is a function of the name's bytes alone. Pinned to values, not
// to itself: a default computed from a seeded hash agrees with itself inside
// one process and gives the reader a different colour on every launch.
TEST(defaultColorIsAStableFunctionOfTheSessionName)
{
    CHECK(defaultTmuxSessionBadgeColorIndex("ui0") == 1);
    CHECK(defaultTmuxSessionBadgeColorIndex("build") == 3);
    CHECK(defaultTmuxSessionBadgeColorIndex("logs") == 6);
    CHECK(defaultTmuxSessionBadgeColorIndex("work") == 0);
    CHECK(defaultTmuxSessionBadgeColorIndex("") == 5);

    // Several sessions are told apart out of the box: the default spreads
    // names across the palette rather than giving them all one colour.
    std::set<int> seen;
    for (const char* name : { "ui0", "build", "logs", "work" })
        seen.insert(defaultTmuxSessionBadgeColorIndex(name));
    CHECK(seen.size() == 4);
}

// A pick is stored as an index. One that names a fill wins; one that does not
// is treated as no pick, so a preference written against a longer palette
// cannot index past this one.
TEST(storedPickWinsOnlyWhenItNamesAFill)
{
    CHECK(resolveTmuxSessionBadgeColorIndex("build", 6) == 6);
    CHECK(resolveTmuxSessionBadgeColorIndex("build", 0) == 0);
    CHECK(resolveTmuxSessionBadgeColorIndex("build", -1) == defaultTmuxSessionBadgeColorIndex("build"));
    CHECK(resolveTmuxSessionBadgeColorIndex("build", tmuxSessionBadgePaletteCount) == defaultTmuxSessionBadgeColorIndex("build"));
    CHECK(resolveTmuxSessionBadgeColorIndex("build", 42) == defaultTmuxSessionBadgeColorIndex("build"));

    CHECK(tmuxSessionBadgeFill(-1) == tmuxSessionBadgeFill(0));
    CHECK(tmuxSessionBadgeFill(tmuxSessionBadgePaletteCount) == tmuxSessionBadgeFill(0));
    std::set<std::uint32_t> fills;
    for (int index = 0; index < tmuxSessionBadgePaletteCount; ++index)
        fills.insert(tmuxSessionBadgeFill(index));
    CHECK_MSG(fills.size() == static_cast<std::size_t>(tmuxSessionBadgePaletteCount), "every palette slot is its own colour");
}

// The label is the pole with the greater |Lc| on its fill, so a light fill
// carries dark ink and a dark one light: the yellow slot reads dark, and no
// slot's label is the pole that reads worse.
TEST(labelIsThePoleThatReadsOnTheFill)
{
    CHECK(tmuxSessionBadgeLabel(2) == Contrast::labelDark);
    for (int index = 0; index < tmuxSessionBadgePaletteCount; ++index) {
        const std::uint32_t fill = tmuxSessionBadgeFill(index);
        const std::uint32_t label = tmuxSessionBadgeLabel(index);
        CHECK(label == Contrast::labelDark || label == Contrast::labelLight);
        const std::uint32_t other = label == Contrast::labelDark ? Contrast::labelLight : Contrast::labelDark;
        CHECK(std::fabs(Contrast::apcaLc(label, fill)) >= std::fabs(Contrast::apcaLc(other, fill)));
    }
}

UBIQUITTY_TEST_MAIN()
