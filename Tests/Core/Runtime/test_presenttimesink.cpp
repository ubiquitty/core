// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// Tests/Core/Runtime/test_presenttimesink.cpp — the portable P2 felt-latency
// accumulator (Runtime/PresentTimeSink.h): nearest-rank p50/p99 over a bounded
// recent-sample ring, felt vs present-leg kept independent. Header-only seam.
//

#include "Core/Runtime/PresentTimeSink.h"

#include <cstdio>

using Ubiquitty::Runtime::PresentTimeSink;

static bool approx(double a, double b) { return (a - b) < 1e-9 && (b - a) < 1e-9; }

TEST(presenttimesinkCases)
{

    // Empty sink → zero quantiles, count 0 (no division/out-of-range on no data).
    {
        PresentTimeSink sink;
        CHECK(sink.feltLatency().count == 0);
        CHECK(approx(sink.feltLatency().p50, 0.0));
        CHECK(approx(sink.feltLatency().p99, 0.0));
        CHECK(sink.presentLeg().count == 0);
    }

    // Known samples 1..100 → nearest-rank: p50 = sorted[50] = 51, p99 = sorted[99] = 100.
    {
        PresentTimeSink sink;
        for (int i = 1; i <= 100; ++i)
            sink.recordFeltLatencyMilliseconds(static_cast<double>(i));
        const auto q = sink.feltLatency();
        CHECK(q.count == 100);
        CHECK(approx(q.p50, 51.0));
        CHECK(approx(q.p99, 100.0));
        // The two metrics are independent — present-leg was never fed.
        CHECK(sink.presentLeg().count == 0);
    }

    // Bounded ring keeps the most recent `capacity` samples (overwrites oldest):
    // pushing 1..100 into a capacity-8 ring retains 93..100.
    {
        PresentTimeSink sink(8);
        for (int i = 1; i <= 100; ++i)
            sink.recordPresentLegMilliseconds(static_cast<double>(i));
        const auto q = sink.presentLeg();
        CHECK(q.count == 8);
        CHECK(approx(q.p50, 97.0)); // sorted{93..100}[4]
        CHECK(approx(q.p99, 100.0)); // sorted{93..100}[7]
    }

    // reset() clears both metrics.
    {
        PresentTimeSink sink;
        sink.recordFeltLatencyMilliseconds(5.0);
        sink.recordPresentLegMilliseconds(7.0);
        sink.reset();
        CHECK(sink.feltLatency().count == 0);
        CHECK(sink.presentLeg().count == 0);
    }

    // A reported present-leg window is consumed; the next report contains
    // only samples recorded after the prior drain.
    {
        PresentTimeSink sink;
        sink.recordPresentLegMilliseconds(3.0);
        sink.recordPresentLegMilliseconds(9.0);
        const auto first = sink.drainPresentLeg();
        CHECK(first.count == 2);
        CHECK(approx(first.p50, 9.0));
        CHECK(sink.presentLeg().count == 0);

        sink.recordPresentLegMilliseconds(5.0);
        const auto second = sink.drainPresentLeg();
        CHECK(second.count == 1);
        CHECK(approx(second.p50, 5.0));
        CHECK(sink.drainPresentLeg().count == 0);
    }
}

UBIQUITTY_TEST_MAIN()
