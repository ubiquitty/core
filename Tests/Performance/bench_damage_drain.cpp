// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_damage_drain.cpp — the per-frame damage drain, on a screen split into
// many runs by pen changes.
//
// The drain needs a dirty screen and dirtying one costs more than draining it,
// so the frame lane below carries both. The drain's own clock runs inside that
// lane rather than beside it, because a paint measured without a drain after it
// is not the same paint: the shadow never advances, every row stays hinted
// dirty, and subtracting the two lanes reads the drain as negative.
//
// The frames alternate. Writing the same one twice changes no cell, and a drain
// over an unchanged screen measures the row skip rather than the walk.
//
//   bench_damage_drain --iters 300 [--json]

#include "BenchHarness.h"

#include "Core/CellGrid.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;

namespace {

constexpr int benchRows = 50;
constexpr int benchColumns = 200;

} // namespace

int main(int argc, char** argv)
{
    int iters = 300;
    int warmup = 20;
    const char* platform = "portable";
    const char* commit = "";
    bool shouldEmitJSON = false;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--warmup") && i + 1 < argc)
            warmup = std::atoi(argv[++i]);
        else if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--json"))
            shouldEmitJSON = true;
    }
    if (iters <= 0 || iters > 1000000)
        iters = 300;

    // Two frames, alternated: every cell differs between them, so each drain has
    // a whole screen of changed cells to walk.
    std::string frames[2];
    for (int which = 0; which < 2; ++which) {
        for (int row = 0; row < benchRows; ++row) {
            frames[which] += "\x1b[" + std::to_string(row + 1) + ";1H";
            for (int cell = 0; cell < benchColumns; ++cell) {
                frames[which] += "\x1b[38;5;" + std::to_string(16 + ((row * benchColumns + cell + which * 37) % 200)) + "m";
                frames[which] += static_cast<char>('a' + ((cell + which) % 26));
            }
        }
    }

    Rendering::CaptureEligibility eligibility;
    eligibility.trustDamage = true;
    eligibility.currentRows = benchRows;
    eligibility.currentColumns = benchColumns;
    eligibility.lastRows = benchRows;
    eligibility.lastColumns = benchColumns;

    auto painted = std::make_unique<Terminal>();
    resize(*painted, benchRows, benchColumns);
    std::vector<std::uint8_t> rowDirty(benchRows, 0);
    int drainToggle = 0;
    int rowsDirtyLastDrain = 0;
    double drainMicrosecondsTotal = 0.0;
    int drainCalls = 0;
    const auto frameTiming = Bench::measure(warmup, iters, [&] {
        processBytes(painted.get(), frames[drainToggle & 1]);
        ++drainToggle;
        eligibility.generationBeforeDrain = painted->grid->currentDamageGeneration();
        const auto before = std::chrono::steady_clock::now();
        (void)Rendering::drainFrameDamage(*painted->grid, eligibility, rowDirty);
        const auto after = std::chrono::steady_clock::now();
        drainMicrosecondsTotal += std::chrono::duration<double, std::micro>(after - before).count();
        ++drainCalls;
        rowsDirtyLastDrain = static_cast<int>(std::ranges::count(rowDirty, std::uint8_t { 1 }));
        Bench::doNotOptimize(rowsDirtyLastDrain);
    });

    const double frameMicroseconds = frameTiming.medianSeconds * 1e6;
    const double drainMicroseconds = drainCalls > 0 ? drainMicrosecondsTotal / drainCalls : 0.0;
    std::printf("damage_drain %d rows %d columns  paint+drain=%.2f us\n", benchRows, benchColumns, frameMicroseconds);
    std::printf("  drain alone=%.2f us over %d calls  rows dirty at the last drain=%d\n",
        drainMicroseconds, drainCalls, rowsDirtyLastDrain);
    // A drain that found nothing dirty measured the row skip, not the walk.
    if (rowsDirtyLastDrain == 0) {
        std::fprintf(stderr, "no row came back dirty: this timed a drain over an unchanged screen\n");
        return 2;
    }

    Bench::Result drain;
    drain.name = "damage_drain_frame";
    drain.unit = "us";
    drain.value = drainMicroseconds;
    drain.p95 = drainMicroseconds;
    Bench::Result frame;
    frame.name = "damage_drain_frame_total";
    frame.unit = "us";
    frame.value = frameMicroseconds;
    frame.p95 = frameTiming.p95Seconds * 1e6;
    std::vector<Bench::Result> results { drain, frame };
    if (shouldEmitJSON)
        Bench::emitJSON(results, platform, commit);
    return 0;
}
