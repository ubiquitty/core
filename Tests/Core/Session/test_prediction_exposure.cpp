// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Core/Session/PredictionExposure.h"
#include "Harness.h"

#include <limits>

using namespace Ubiquitty::Session;

namespace {

// Deterministic examples of the contract, not a shipping measured policy.
constexpr PredictionExposurePolicy testPolicy {
    .showAboveMicroseconds = 30'000,
    .hideAtMicroseconds = 20'000,
    .flagAboveMicroseconds = 80'000,
    .clearFlagAtMicroseconds = 50'000,
    .glitchAfterMicroseconds = 250'000,
    .forceFlagAfterMicroseconds = 5'000'000,
};

} // namespace

TEST(predictionExposureRequiresAnExplicitPolicyAndPendingConfirmedEpoch)
{
    PredictionExposure exposure;
    exposure.noteEchoLatency(1'000'000);
    CHECK(!exposure.treatment(10'000'000, 0).isVisible);
    CHECK(!exposure.nextWakeDelay(10'000'000, 0));
    REQUIRE(exposure.configure(testPolicy));
    exposure.noteEchoLatency(1'000'000);
    CHECK(!exposure.treatment(10'000'000, std::nullopt).isVisible);
    CHECK(!exposure.nextWakeDelay(10'000'000, std::nullopt));
}

TEST(predictionExposureUsesSeparateShowAndPendingStyleHysteresis)
{
    PredictionExposure exposure;
    REQUIRE(exposure.configure(testPolicy));
    exposure.noteEchoLatency(30'000);
    CHECK(!exposure.treatment(1, 0).isVisible);
    exposure.noteEchoLatency(30'001);
    CHECK(exposure.treatment(1, 0).isVisible);
    exposure.noteEchoLatency(25'000);
    CHECK(exposure.treatment(1, 0).isVisible);
    exposure.noteEchoLatency(20'000);
    CHECK(!exposure.treatment(1, 0).isVisible);
    exposure.noteEchoLatency(80'000);
    CHECK(!exposure.treatment(1, 0).isFlagged);
    exposure.noteEchoLatency(80'001);
    CHECK(exposure.treatment(1, 0).isFlagged);
    exposure.noteEchoLatency(60'000);
    CHECK(exposure.treatment(1, 0).isFlagged);
    exposure.noteEchoLatency(50'000);
    CHECK(!exposure.treatment(1, 0).isFlagged);
}

TEST(aFastEchoStaysAuthoritativeUntilAPendingGlitchDeadline)
{
    PredictionExposure exposure;
    REQUIRE(exposure.configure(testPolicy));
    exposure.noteEchoLatency(5'000);
    CHECK(!exposure.treatment(249'999, 0).isVisible);
    CHECK_EQ(exposure.nextWakeDelay(249'999, 0).value_or(0), 1u);
    CHECK(exposure.treatment(250'000, 0).isVisible);
    CHECK(!exposure.treatment(250'000, 0).isFlagged);
    CHECK(exposure.treatment(400'000, 0).isVisible);
    CHECK(!exposure.treatment(4'999'999, 0).isFlagged);
    CHECK(exposure.treatment(5'000'000, 0).isFlagged);
    CHECK(!exposure.nextWakeDelay(5'000'000, 0));
}

TEST(predictionExposureRejectsInvalidPoliciesAndForgetsOldPathMeasurements)
{
    PredictionExposure exposure;
    REQUIRE(exposure.configure(testPolicy));
    exposure.noteEchoLatency(100'000);
    auto invalid = testPolicy;
    invalid.hideAtMicroseconds = invalid.showAboveMicroseconds + 1;
    CHECK(!exposure.configure(invalid));
    CHECK(!exposure.treatment(1'000'000, 0).isVisible);
    invalid = testPolicy;
    invalid.clearFlagAtMicroseconds = invalid.flagAboveMicroseconds + 1;
    CHECK(!exposure.configure(invalid));
    invalid = testPolicy;
    invalid.flagAboveMicroseconds = invalid.showAboveMicroseconds - 1;
    CHECK(!exposure.configure(invalid));
    invalid = testPolicy;
    invalid.clearFlagAtMicroseconds = invalid.hideAtMicroseconds - 1;
    CHECK(!exposure.configure(invalid));
    invalid = testPolicy;
    invalid.glitchAfterMicroseconds = 0;
    CHECK(!exposure.configure(invalid));
    invalid = testPolicy;
    invalid.forceFlagAfterMicroseconds = invalid.glitchAfterMicroseconds - 1;
    CHECK(!exposure.configure(invalid));
    REQUIRE(exposure.configure(testPolicy));
    exposure.noteEchoLatency(100'000);
    exposure.reset();
    CHECK(!exposure.treatment(1, 0).isVisible);
    REQUIRE(exposure.configure(std::nullopt));
    CHECK(!exposure.nextWakeDelay(1, 0));
}

TEST(predictionExposureUsesSubtractionWithoutAbsoluteDeadlineOverflow)
{
    PredictionExposure exposure;
    REQUIRE(exposure.configure(testPolicy));
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK_EQ(exposure.nextWakeDelay(maximum, maximum - 1).value_or(0), 249'999u);
    CHECK(!exposure.treatment(maximum - 1, maximum).isVisible);
    CHECK(!exposure.nextWakeDelay(maximum - 1, maximum));
}

UBIQUITTY_TEST_MAIN()
