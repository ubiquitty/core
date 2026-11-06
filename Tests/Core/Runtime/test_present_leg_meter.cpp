// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Runtime/PresentLegMeter.h — the felt-latency
// present leg every renderer measures through.
//
// Pins:
//   - an unarmed frame hands out a poster that posts nothing;
//   - arming is consumed by the take, so only armed frames are timed;
//   - a poster posts model-to-glass milliseconds over the ring, from another
//     thread, and main collects them;
//   - a drawable that never reached the glass posts no leg and is counted
//     apart;
//   - a report closes every 120 drained frames, over the samples since the
//     last one;
//   - a poster outliving its meter still posts into a live ring.

#include "Core/Runtime/PresentLegMeter.h"

#include <memory>
#include <thread>

namespace {

using Ubiquitty::Runtime::PresentLegMeter;

TEST(testAnUnarmedFrameIsNotTimed)
{
    PresentLegMeter meter;
    auto poster = meter.takeArmedPoster();
    CHECK(!poster.isArmed());
    poster.post(5.0);
    (void)meter.drainFrame();
    CHECK_MSG(meter.samplesRecorded() == 0, "an unarmed poster posts nothing");

    meter.arm(1.0);
    CHECK(meter.takeArmedPoster().isArmed());
    CHECK_MSG(!meter.takeArmedPoster().isArmed(), "the take consumes the arm");
}

TEST(testThePresentThreadPostsTheLeg)
{
    PresentLegMeter meter;
    meter.arm(2.0);
    const auto poster = meter.takeArmedPoster();
    std::thread presentThread([poster] { poster.post(2.0125); });
    presentThread.join();
    (void)meter.drainFrame();
    CHECK(meter.samplesRecorded() == 1);
}

TEST(testAReportClosesEveryHundredAndTwentyFrames)
{
    PresentLegMeter meter;
    for (std::uint64_t frame = 1; frame <= PresentLegMeter::reportEveryFrames; ++frame) {
        const double model = static_cast<double>(frame);
        meter.arm(model);
        meter.takeArmedPoster().post(model + static_cast<double>(frame % 10 + 1) / 1000.0);
        const auto report = meter.drainFrame();
        if (frame < PresentLegMeter::reportEveryFrames) {
            CHECK(report.count == 0);
        } else {
            CHECK(report.count == PresentLegMeter::reportEveryFrames);
            CHECK(report.p50 > 5.0 && report.p50 < 7.0);
            CHECK(report.p99 > 9.9 && report.p99 < 10.1);
        }
    }
    CHECK(meter.lastReport().count == PresentLegMeter::reportEveryFrames);
    CHECK(PresentLegMeter::reportLine(meter.lastReport()).rfind("[perf] present_leg p50=", 0) == 0);

    for (std::uint64_t frame = 0; frame < PresentLegMeter::reportEveryFrames; ++frame)
        (void)meter.drainFrame();
    CHECK_MSG(meter.lastReport().count == PresentLegMeter::reportEveryFrames, "an empty window keeps the last report");
}

TEST(testADrawableThatNeverReachedTheGlassPostsNothing)
{
    PresentLegMeter meter;
    meter.arm(4.0);
    CHECK(meter.unshownPresents() == 0);
    meter.takeArmedPoster().post(0.0);
    (void)meter.drainFrame();
    CHECK_MSG(meter.samplesRecorded() == 0, "a presented time of zero is a drawable that was never shown");
    CHECK_MSG(meter.unshownPresents() == 1, "and it is counted apart from the legs");
}

TEST(testAPosterOutlivesItsMeter)
{
    auto meter = std::make_unique<PresentLegMeter>();
    meter->arm(3.0);
    const auto poster = meter->takeArmedPoster();
    meter.reset();
    poster.post(3.5);
    CHECK_MSG(poster.isArmed(), "the handler's reference keeps the ring alive after teardown");
}

TEST(testTheLegIsModelToGlass)
{
    PresentLegMeter meter;
    for (std::uint64_t frame = 0; frame < PresentLegMeter::reportEveryFrames; ++frame) {
        meter.arm(10.0);
        meter.takeArmedPoster().post(10.004);
        (void)meter.drainFrame();
    }
    CHECK_MSG(meter.lastReport().p50 > 3.999 && meter.lastReport().p50 < 4.001, "presented minus model, in milliseconds");
}

} // namespace

UBIQUITTY_TEST_MAIN()
