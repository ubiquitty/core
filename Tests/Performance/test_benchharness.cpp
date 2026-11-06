// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Performance/test_benchharness.cpp — the paired measurement mechanism.
//
// The timing itself is not asserted: wall-clock on a shared machine is exactly
// the thing measurePaired exists to cope with, so a test that pinned durations
// would fail for the reason the function was written. What is asserted is the
// mechanism that makes the pairing work — that both variants are driven the same
// number of times, that they interleave rather than run as blocks, and that the
// lead alternates so neither keeps the cold-cache position.

#include "BenchHarness.h"
#include "Harness.h"

#include <string>

using Ubiquitty::Bench::measurePaired;
using Ubiquitty::Bench::PairedTiming;

TEST(paired_interleaves_and_alternates_the_lead)
{
    std::string order;
    const int warmup = 2, iters = 6;
    const PairedTiming timing = measurePaired(
        warmup, iters, [&] { order += 'a'; }, [&] { order += 'b'; });

    // Warmup drives both, in a fixed order, untimed.
    const std::string warmupPrefix = order.substr(0, static_cast<std::size_t>(warmup) * 2);
    CHECK_CSTR_EQ(warmupPrefix.c_str(), "abab");

    // Timed iterations interleave, and the leading variant swaps every iteration:
    // a block-structured harness would read "aaaaaabbbbbb" here instead.
    const std::string timed = order.substr(static_cast<std::size_t>(warmup) * 2);
    CHECK_CSTR_EQ(timed.c_str(), "abbaabbaabba");

    // Each variant ran exactly once per iteration, so the pairing is one-to-one.
    std::size_t countA = 0, countB = 0;
    for (const char c : timed)
        (c == 'a' ? countA : countB)++;
    CHECK_EQ(countA, static_cast<std::size_t>(iters));
    CHECK_EQ(countB, static_cast<std::size_t>(iters));

    // A ratio is produced whenever any iteration had a measurable `a`; on a clock
    // too coarse to separate two empty lambdas it stays 0, which callers read as
    // "no comparison available" rather than as parity.
    CHECK(timing.ratio >= 0.0);
}

TEST(paired_handles_degenerate_iteration_counts)
{
    int callsA = 0, callsB = 0;
    const PairedTiming none = measurePaired(
        0, 0, [&] { ++callsA; }, [&] { ++callsB; });
    CHECK_EQ(callsA, 0);
    CHECK_EQ(callsB, 0);
    CHECK(none.ratio == 0.0);
    CHECK(none.a.medianSeconds == 0.0);
    CHECK(none.b.medianSeconds == 0.0);

    // One iteration is a valid comparison, just a weak one — it must not divide
    // by an empty sample set.
    const PairedTiming single = measurePaired(
        0, 1, [&] { ++callsA; }, [&] { ++callsB; });
    CHECK_EQ(callsA, 1);
    CHECK_EQ(callsB, 1);
    CHECK(single.ratio >= 0.0);
}

UBIQUITTY_TEST_MAIN()

// The repeat-to-clear-the-clock mechanism, asserted by COUNTING invocations
// rather than by timing them — for the reason at the top of this file, and
// because the count is the whole of what the mechanism does.

TEST(short_region_repeats_and_divides_by_the_repeat_count)
{
    // A callable fast enough to land under one clock tick. The harness must run it
    // many times per timed region, and the count it ran is recoverable: total
    // invocations divided by the timed iterations.
    int calls = 0;
    const int warmup = 3;
    const int iters = 5;
    const Ubiquitty::Bench::Timing timing = Ubiquitty::Bench::measure(warmup, iters, [&] { ++calls; });
    const int timedCalls = calls - warmup;
    CHECK(timedCalls > iters); // repeated, not one call per iteration
    CHECK(timedCalls % iters == 0); // the same repeat count every iteration
    const int repeats = timedCalls / iters;
    CHECK(repeats > 1);
    // The reported figure is per CALL, so reconstructing the region from it must
    // give back roughly the floor the repeat count was calibrated to reach — not
    // that floor multiplied by the count again.
    //
    // Asserted this way rather than as `medianSeconds < floor`, which a harness
    // that forgot to divide passes by a hair: the region is grown to just over the
    // floor, so the undivided figure lands right at the threshold and the
    // comparison decides on machine noise. Reconstructing separates the two cases
    // by the repeat count itself.
    CHECK(timing.medianSeconds > 0.0);
    const double regionSeconds = timing.medianSeconds * repeats;
    CHECK(regionSeconds < Ubiquitty::Bench::shortestTrustworthyCallSeconds * 4.0);
}

TEST(a_one_shot_lane_is_never_repeated)
{
    // `measure(0, 1, ...)` is how a one-shot operation is timed: nothing to warm,
    // one sample. There is no observation to calibrate a repeat count from, and
    // repeating anyway makes every call after the first a no-op whose zero cost is
    // averaged into the answer.
    int calls = 0;
    (void)Ubiquitty::Bench::measure(0, 1, [&] { ++calls; });
    CHECK(calls == 1);
}

TEST(repeat_once_opts_a_lane_out_of_repeating)
{
    // The explicit escape, for a callable whose work per call changes when it is
    // repeated. Asserted separately from the warmup-zero rule: they are two
    // different reasons a lane must run exactly as many times as it asked to, and
    // a harness could honour one while dropping the other.
    int calls = 0;
    const int warmup = 2;
    const int iters = 4;
    (void)Ubiquitty::Bench::measure(warmup, iters, [&] { ++calls; }, Ubiquitty::Bench::Repeat::Once);
    CHECK(calls == warmup + iters);
}
