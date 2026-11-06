// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_fractionalaccumulator — the reusable fractional-banking accumulator
// (Source/Base/FractionalAccumulator.h): whole units released as they complete,
// the sub-unit remainder banked, truncation toward zero so a reversal doesn't
// bleed stale magnitude across.

#include "Base/FractionalAccumulator.h"

#include <cstdio>

using Ubiquitty::FractionalAccumulator;

static bool near(double a, double b) { return (a - b) < 1e-9 && (b - a) < 1e-9; }

TEST(fractionalaccumulator)
{

    // Whole deltas pass through with nothing banked.
    {
        FractionalAccumulator acc;
        CHECK(acc.take(3.0) == 3);
        CHECK(near(acc.residual(), 0.0));
        CHECK(acc.take(-2.0) == -2);
        CHECK(near(acc.residual(), 0.0));
    }

    // Sub-unit deltas bank until they complete a whole unit.
    {
        FractionalAccumulator acc;
        CHECK(acc.take(0.4) == 0); // 0.4 banked
        CHECK(acc.take(0.4) == 0); // 0.8 banked
        CHECK(acc.take(0.4) == 1); // 1.2 → 1, 0.2 left
        CHECK(acc.residual() > 0.15 && acc.residual() < 0.25);
    }

    // Truncation toward zero: a reversal doesn't spend banked opposite magnitude.
    {
        FractionalAccumulator acc;
        CHECK(acc.take(0.6) == 0); // +0.6 banked
        CHECK(acc.take(-1.0) == 0); // -0.4 → 0 (not -1), residual negative
        CHECK(acc.residual() < 0.0);
    }

    // reset() discards the bank.
    {
        FractionalAccumulator acc;
        (void)acc.take(0.9);
        CHECK(acc.residual() > 0.0);
        acc.reset();
        CHECK(near(acc.residual(), 0.0));
        CHECK(acc.take(0.5) == 0); // starts fresh, nothing carried
    }

    // A large delta releases all whole units at once, banking the fraction.
    {
        FractionalAccumulator acc;
        CHECK(acc.take(5.25) == 5);
        CHECK(acc.residual() > 0.2 && acc.residual() < 0.3);
    }
}

UBIQUITTY_TEST_MAIN()
