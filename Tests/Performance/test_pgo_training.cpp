// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Exercise the same header configuration pgo-train.sh gives bench_stack, while
// test_benchharness keeps checking ordinary adaptive measurement separately.
#define UBIQUITTY_PGO_TRAINING 1
#include "BenchHarness.h"
#include "Harness.h"

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

static_assert(Ubiquitty::Bench::isProfileTraining);

TEST(trainingWorkDoesNotDependOnCallbackDelay)
{
    constexpr int warmup = 3;
    constexpr int iterations = 5;
    constexpr std::string_view input = "line\r\n";
    constexpr std::array delays { std::chrono::milliseconds(0), std::chrono::milliseconds(1) };
    for (const auto delay : delays) {
        std::string captured;
        std::size_t bytesProduced = 0;
        int captures = 0;
        (void)Ubiquitty::Bench::measure(warmup, iterations, [&] {
            bytesProduced += input.size();
            captured += input;
            ++captures;
            if (delay.count())
                std::this_thread::sleep_for(delay);
        });
        CHECK_EQ(captures, warmup + iterations);
        CHECK_EQ(bytesProduced, input.size() * (warmup + iterations));
        CHECK_CSTR_EQ(captured.c_str(), "line\r\nline\r\nline\r\nline\r\nline\r\nline\r\nline\r\nline\r\n");
    }
}

TEST(trainingIgnoresAnExplicitAdaptiveRepeatRequest)
{
    // Count a short callback as well as the stateful producer above.
    // Re-enabling clock calibration must fail this assertion, even when the
    // stateful callback happens to exceed the clock floor on a loaded machine.
    int calls = 0;
    (void)Ubiquitty::Bench::measure(3, 5, [&] { ++calls; }, Ubiquitty::Bench::Repeat::ToClearClock);
    CHECK_EQ(calls, 8);
}

TEST(trainingPreservesEmptyAndOneShotLanes)
{
    int calls = 0;
    (void)Ubiquitty::Bench::measure(0, 0, [&] { ++calls; });
    CHECK_EQ(calls, 0);
    (void)Ubiquitty::Bench::measure(0, 1, [&] { ++calls; });
    CHECK_EQ(calls, 1);
    (void)Ubiquitty::Bench::measure(2, 4, [&] { ++calls; }, Ubiquitty::Bench::Repeat::Once);
    CHECK_EQ(calls, 7);
}

UBIQUITTY_TEST_MAIN()
