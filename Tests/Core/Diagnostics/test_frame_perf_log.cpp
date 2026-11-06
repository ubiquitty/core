// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Diagnostics/FramePerfLog.cpp — the
// UBIQUITTY_PERFLOG aggregator every frontend feeds.
//
// Pins:
//   - off unless the environment or a caller turns it on;
//   - one report per half-second window, with the window's figures;
//   - a gap is a drop only while host bytes arrived within 100 ms, counted as
//     the refreshes it missed;
//   - a frame whose predecessor predates the clock's resume is neither a gap
//     nor a drop, so an idle pause is never booked against the renderer;
//   - the pacing and arm lines print the counters' growth, and only when
//     they grew.

#include "Core/Diagnostics/FramePerfLog.h"

#include <cstdlib>
#include <string>

namespace {

using Ubiquitty::Diagnostics::FramePerfLog;
using Ubiquitty::Diagnostics::PerfDrainSample;
using Ubiquitty::Diagnostics::PerfFrameSample;

constexpr double refresh = 1.0 / 60.0;

PerfFrameSample frameAt60Hz()
{
    PerfFrameSample sample;
    sample.drawSeconds = 0.002;
    sample.refreshSeconds = refresh;
    return sample;
}

PerfDrainSample drainOf(std::uint64_t bytes)
{
    PerfDrainSample sample;
    sample.bytes = bytes;
    sample.ingestSeconds = 0.001;
    sample.longestIngestSeconds = 0.001;
    sample.columns = 80;
    sample.rows = 24;
    return sample;
}

bool hasLineStartingWith(const FramePerfLog& log, const std::string& prefix)
{
    for (const auto& line : log.lastLines()) {
        if (line.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

TEST(testOffUnlessAsked)
{
    unsetenv("UBIQUITTY_PERFLOG");
    FramePerfLog quiet;
    CHECK_MSG(!quiet.isActive(), "no environment, no log");
    quiet.activate();
    CHECK(quiet.isActive());

    setenv("UBIQUITTY_PERFLOG", "1", 1);
    FramePerfLog asked;
    CHECK_MSG(asked.isActive(), "UBIQUITTY_PERFLOG turns it on");
    unsetenv("UBIQUITTY_PERFLOG");
    CHECK_MSG(asked.isActive(), "the environment is read once");
    asked.deactivate();
    CHECK(!asked.isActive());
}

TEST(testASlowDrainHandlerPrintsASpike)
{
    FramePerfLog log;
    log.silence();
    log.noteDrainHandler(0.002, 100);
    CHECK(!hasLineStartingWith(log, "[perf-drain] SPIKE"));
    log.noteDrainHandler(0.012, 4096);
    CHECK(hasLineStartingWith(log, "[perf-drain] SPIKE 12.0ms bytes=4096"));
}

TEST(testOneReportPerWindow)
{
    FramePerfLog log;
    log.silence();
    double now = 10.0;
    log.noteDrain(drainOf(4096), now);
    for (int frame = 0; frame < 29; ++frame) {
        now += refresh;
        log.noteFrame(frameAt60Hz(), now);
    }
    CHECK_MSG(log.reportCount() == 0, "a window shorter than half a second has not flushed");
    CHECK(log.currentWindow().frames == 29);
    now = 10.0 + FramePerfLog::windowSeconds;
    log.noteFrame(frameAt60Hz(), now);
    CHECK(log.reportCount() == 1);
    CHECK(log.lastReport().frames == 30);
    CHECK(log.lastReport().bytes == 4096);
    CHECK(log.lastReport().drains == 1);
    CHECK(log.lastReport().seconds >= FramePerfLog::windowSeconds);
    CHECK(hasLineStartingWith(log, "[perf-drain] "));
    CHECK(hasLineStartingWith(log, "[perf] "));
    CHECK_MSG(log.currentWindow().frames == 0, "the next window starts empty");
}

TEST(testAGapCountsAsDropsOnlyWhileOutputDrives)
{
    FramePerfLog driven;
    driven.silence();
    double now = 1.0;
    driven.noteFrame(frameAt60Hz(), now);
    now += 0.050;
    driven.noteDrain(drainOf(512), now - 0.010);
    driven.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(driven.currentWindow().droppedFrames == 2, "a 50 ms gap under output missed two refreshes at 60 Hz");
    CHECK(driven.currentWindow().longestGapSeconds > 0.049);

    FramePerfLog idle;
    idle.silence();
    now = 1.0;
    idle.noteDrain(drainOf(512), now - 0.2);
    idle.noteFrame(frameAt60Hz(), now);
    now += 0.050;
    idle.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(idle.currentWindow().droppedFrames == 0, "a gap with no output in the last 100 ms is not a drop");
    CHECK_MSG(idle.currentWindow().longestGapSeconds > 0.049, "it is still the window's longest gap");

    FramePerfLog steady;
    steady.silence();
    now = 1.0;
    steady.noteFrame(frameAt60Hz(), now);
    now += refresh;
    steady.noteDrain(drainOf(512), now);
    steady.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(steady.currentWindow().droppedFrames == 0, "a frame one refresh after the last dropped nothing");
}

TEST(testTheResumeEdgeKeepsAnIdlePauseOutOfTheStats)
{
    FramePerfLog log;
    log.silence();
    double now = 1.0;
    log.noteFrame(frameAt60Hz(), now);
    now += 0.2;
    log.noteResume(now);
    CHECK(log.lastResumeSeconds() == now);
    log.noteDrain(drainOf(512), now);
    now += 0.004;
    log.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(log.currentWindow().droppedFrames == 0, "the burst's first frame does not book the pause as drops");
    CHECK_MSG(log.currentWindow().longestGapSeconds == 0.0, "nor as a gap");

    now += 0.050;
    log.noteDrain(drainOf(512), now - 0.005);
    log.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(log.currentWindow().droppedFrames == 2, "the frames after it are measured again");

    FramePerfLog unmarked;
    unmarked.silence();
    now = 1.0;
    unmarked.noteFrame(frameAt60Hz(), now);
    now += 0.2;
    unmarked.noteDrain(drainOf(512), now);
    now += 0.004;
    unmarked.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(unmarked.currentWindow().droppedFrames > 10, "without the resume the pause reads as a stall");
}

TEST(testPacingAndArmLinesPrintGrowth)
{
    FramePerfLog log;
    log.silence();
    double now = 1.0;
    log.notePacing(10, 2, 0, 1);
    log.noteArm(5, 0, 0, 0, 4);
    log.noteFrame(frameAt60Hz(), now);
    now += 0.6;
    log.noteFrame(frameAt60Hz(), now);
    CHECK(hasLineStartingWith(log, "[perf-pace] content promote=10 overwrite=2 discard=0 slip=1"));
    CHECK(hasLineStartingWith(log, "[perf-arm] swift drew=5 reused=4"));

    now += 0.6;
    log.noteFrame(frameAt60Hz(), now);
    CHECK_MSG(!hasLineStartingWith(log, "[perf-pace]"), "unchanged pacing prints no line");
    CHECK(!hasLineStartingWith(log, "[perf-arm]"));

    log.notePacing(13, 2, 0, 1);
    now += 0.6;
    log.noteFrame(frameAt60Hz(), now);
    CHECK(hasLineStartingWith(log, "[perf-pace] content promote=3 overwrite=0"));
}

} // namespace

UBIQUITTY_TEST_MAIN()
