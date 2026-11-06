// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_selection_mode.cpp — pin Ubiquitty::SelectionMode's enumerator
// values and exercise them through the real begin() public API.
//
// This does two things — neither of which a bare a == a tautology
// over the enumerators in the same TU would prove:
//
//   1. static_assert pins each enumerator to its historic integer
//      value (0/1/2/3), so a reordering regresses at compile time.
//   2. A behaviour test passes each mode through the real
//      Ubiquitty::Selection::begin() API and asserts the struct field
//      receives it unchanged, proving the enum is actually wired to
//      the public boundary, not just numerically correct in isolation.

#include "Core/Selection/Selection.h"

#include <cstdio>

// Compile-time pinning: the enumerators must keep their historic
// integer values. If a future refactor renumbers them, the build
// fails here long before any runtime check.
static_assert(static_cast<int>(Ubiquitty::SelectionMode::Character) == 0, "SelectionMode::Character == 0");
static_assert(static_cast<int>(Ubiquitty::SelectionMode::Word) == 1, "SelectionMode::Word == 1");
static_assert(static_cast<int>(Ubiquitty::SelectionMode::Line) == 2, "SelectionMode::Line == 2");
static_assert(static_cast<int>(Ubiquitty::SelectionMode::Rectangle) == 3, "SelectionMode::Rectangle == 3");

/// Behaviour test — pass each defined enumerator through the real
/// public Ubiquitty::Selection::begin() API and assert the struct's
/// `mode` field receives the matching value.
TEST(behaviourThroughPublicAPI)
{
    const Ubiquitty::SelectionMode modes[] = {
        Ubiquitty::SelectionMode::Character,
        Ubiquitty::SelectionMode::Word,
        Ubiquitty::SelectionMode::Line,
        Ubiquitty::SelectionMode::Rectangle,
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        Ubiquitty::SelectionState selection;
        Ubiquitty::Selection::init(&selection);
        Ubiquitty::Selection::begin(&selection, 10, 20, 3, 5, modes[i]);
        CHECK(selection.active == true);
        CHECK(selection.mode == modes[i]);
    }

    /// Sanity: distinct mode inputs must reach the struct as distinct
    /// values (no collapsing).
    Ubiquitty::SelectionState a, b, c;
    Ubiquitty::Selection::init(&a);
    Ubiquitty::Selection::init(&b);
    Ubiquitty::Selection::init(&c);
    Ubiquitty::Selection::begin(&a, 10, 20, 0, 0, Ubiquitty::SelectionMode::Character);
    Ubiquitty::Selection::begin(&b, 10, 20, 0, 0, Ubiquitty::SelectionMode::Word);
    Ubiquitty::Selection::begin(&c, 10, 20, 0, 0, Ubiquitty::SelectionMode::Line);
    CHECK(a.mode != b.mode);
    CHECK(b.mode != c.mode);
    CHECK(a.mode != c.mode);
}

UBIQUITTY_TEST_MAIN()
