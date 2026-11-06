// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// ThreadCPUClock.h — the calling thread's CPU time, for a test that compares
// two measurements of its own work.
//
// A scaling test asks whether one input costs a bounded multiple of another.
// Wall-clock time answers a different question on a shared runner: it counts
// the time the thread spent descheduled while other jobs ran, and on a busy
// self-hosted runner that alone can carry a ratio past its bound with no
// defect present. Thread CPU time counts only the time this thread was on a
// core, which is the quantity the ratio is about. It still carries cache and
// frequency effects from the neighbours, so a test keeps its best-of-N and
// leaves room either side of its bound.

#pragma once

#include <ctime>

namespace UbiquittyTest {

/// The calling thread's CPU time in seconds.
///
/// - Returns: Seconds this thread has spent on a core, from an arbitrary epoch;
///   only differences mean anything.
inline double threadCPUSeconds()
{
    timespec now {};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

} // namespace UbiquittyTest
