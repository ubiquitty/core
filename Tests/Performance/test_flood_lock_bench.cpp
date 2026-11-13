// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "FloodLockBench.h"
#include "Harness.h"

#include <cmath>
#include <string>

TEST(theDefaultFloodLaneMeasuresTheProductionLock)
{
    std::vector<Ubiquitty::Bench::Result> results;
    const std::string flood(8192, 'a');
    Ubiquitty::Bench::measureFloodLockWait(results, std::span(flood), 24, 80, 0.02);
    REQUIRE(results.size() == 2);
    CHECK_CSTR_EQ(results[0].name.c_str(), "model_lock_wait_under_flood_p99_milliseconds");
    CHECK_CSTR_EQ(results[1].name.c_str(), "model_lock_wait_under_flood_max_milliseconds");
    for (const auto& result : results) {
        CHECK(std::isfinite(result.value));
        CHECK(result.value > 0.0);
    }
}

TEST(theRawMutexCounterexampleHasDistinctMetrics)
{
    std::vector<Ubiquitty::Bench::Result> results;
    const std::string flood(8192, 'a');
    Ubiquitty::Bench::measureFloodLockWait<Ubiquitty::Bench::FloodLockService::RawMutex>(results, std::span(flood), 24, 80, 0.0);
    REQUIRE(results.size() == 2);
    CHECK_CSTR_EQ(results[0].name.c_str(), "lock_wait_under_flood_p99_milliseconds");
    CHECK_CSTR_EQ(results[1].name.c_str(), "lock_wait_under_flood_max_milliseconds");
}

UBIQUITTY_TEST_MAIN()
