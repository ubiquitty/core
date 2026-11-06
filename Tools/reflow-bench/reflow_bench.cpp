// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// reflow_bench — headless reflow go/no-go measurement.
//
// Fills the scrollback to capacity, then times repeated width resizes
// (the reflowPrimary path) and reports p50/p99 latency, plus the
// steady-state scrollback arena footprint. The thresholds: a full-scrollback
// width resize should stay under ~16 ms (one 60 Hz frame) and steady-state
// scrollback should stay within the ~1 MB/Ubiquitty::Terminal budget.

#include "Core/LineStore.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Feed one logical line of `width` printable columns plus CR/LF, so it
// lands as a hard-broken row that the width reflow must re-split.
void feedLine(Ubiquitty::Terminal* t, int width, int seed)
{
    std::string s;
    s.reserve(static_cast<size_t>(width) + 2);
    for (int c = 0; c < width; ++c)
        s.push_back(static_cast<char>('A' + ((seed + c) % 26)));
    s.push_back('\r');
    s.push_back('\n');
    Ubiquitty::processBytes(t, s);
}

} // namespace

int main(int argc, const char* argv[])
{
    int rows = 24;
    int colsA = 120;
    int colsB = 80;
    int fillLines = 2000; // overfills SCROLLBACK_LINES (800) to reach capacity
    int iterations = 200;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--rows"))
            rows = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--columnsA"))
            colsA = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--columnsB"))
            colsB = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--fill"))
            fillLines = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--iter"))
            iterations = std::atoi(argv[i + 1]);
    }

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, colsA);

    for (int i = 0; i < fillLines; ++i)
        feedLine(&t, colsA, i);

    const int sbCount = Ubiquitty::Screen::scrollbackCount(&t);

    // Steady-state scrollback footprint: the content-trimmed LineStore's
    // block slabs (Tier 2). Zero before any line is captured.
    const long arenaBytes = t.lineStore ? static_cast<long>(t.lineStore->arenaBytes()) : 0;

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    int toggle = 0;
    for (int i = 0; i < iterations; ++i) {
        const int target = (toggle ^= 1) ? colsB : colsA;
        Clock::time_point t0 = Clock::now();
        Ubiquitty::resize(t, rows, target);
        samples.push_back(millisSince(t0));
    }
    std::sort(samples.begin(), samples.end());
    const double p50 = samples[samples.size() / 2];
    const double p99 = samples[(samples.size() * 99) / 100];
    const double worst = samples.back();

    std::printf("reflow-bench: rows=%d colsA=%d colsB=%d fill=%d iter=%d\n",
        rows, colsA, colsB, fillLines, iterations);
    std::printf("  scrollbackCount=%d  cap=%d  sizeof(Cell)=%zu\n",
        sbCount, t.scrollbackCapacity, sizeof(Ubiquitty::Cell));
    std::printf("  arena: %ld bytes (%.2f MiB)\n",
        arenaBytes, arenaBytes / (1024.0 * 1024.0));
    std::printf("  width-resize latency: p50=%.3f ms  p99=%.3f ms  worst=%.3f ms\n",
        p50, p99, worst);
    std::printf("  trigger(latency>16ms)=%s  trigger(arena>1MiB)=%s\n",
        p99 > 16.0 ? "YES" : "no",
        arenaBytes > 1024 * 1024 ? "YES" : "no");

    return 0;
}
