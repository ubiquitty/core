// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_damage_walk.cpp — the paint session's damage walk, driven directly.
//
// The walk answers "which runs of this row differ from what was last painted",
// and the sync capture path is what asks: a remote client's frame is built from
// its answer. No local frontend reaches it, so no other bench here does either,
// and a rewrite of it has no other number to answer to.
//
// Two shapes, because the walk's cost is shaped by how much it finds rather than
// by how much it reads: a screen that changed one row, which is a shell prompt
// or a log line, and one that changed every row, which is an alt-screen redraw.

#include "BenchHarness.h"

#include "Core/CellGrid.h"
#include "Core/PaintSession.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Ubiquitty::Bench;

namespace {

// A terminal with `rows` x `columns` of text, painted once so the shadow
// matches: the walk starts from agreement, as it does on a steady screen.
std::unique_ptr<Ubiquitty::Terminal> paintedTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::hardReset(*terminal);
    Ubiquitty::resize(*terminal, rows, columns);
    std::string screen;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column)
            screen.push_back(static_cast<char>(0x21 + ((row * 7 + column) % 0x5E)));
        screen += "\r\n";
    }
    Ubiquitty::processBytes(terminal.get(), screen);
    return terminal;
}

// Drains every run the walk offers, which is what a capture does, and returns
// the count so the work cannot be optimised away.
int walkWholeFrame(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::PaintSession session = terminal.grid->beginPaint();
    Ubiquitty::PaintRun run;
    int runs = 0;
    while (session.nextDirtyRun(&run)) {
        session.ackRun(run);
        ++runs;
    }
    return runs;
}

} // namespace

int main(int argc, char** argv)
{
    int rows = 48;
    int columns = 200;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--rows" && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (std::string(argv[i]) == "--columns" && i + 1 < argc)
            columns = std::atoi(argv[++i]);
    }

    std::vector<Result> results;
    const double cells = static_cast<double>(rows) * static_cast<double>(columns);

    {
        // One row changed: the walk reads every row and finds one, which is the
        // rejection path the two-array scan exists for.
        auto terminal = paintedTerminal(rows, columns);
        (void)walkWholeFrame(*terminal); // settle the shadow
        int sink = 0;
        const Timing timing = measure(3, 40, [&] {
            Ubiquitty::processBytes(terminal.get(), "\033[24;1Hone changed row");
            sink += walkWholeFrame(*terminal);
        });
        Result result;
        result.name = "damage_walk_one_row";
        result.unit = "Mcell/s";
        result.value = timing.medianSeconds > 0.0 ? cells / timing.medianSeconds / 1e6 : 0.0;
        results.push_back(result);
        // The walk has to have found runs, or the lane timed a loop that
        // rejected every row before reaching the scan it exists to measure.
        if (!witnessPathTaken(result.name.c_str(), "a dirty run", sink))
            return 1;
    }

    {
        // Every row changed: the walk finds a run per row, so the rejection path
        // never runs and the per-run bookkeeping is what shows.
        auto terminal = paintedTerminal(rows, columns);
        (void)walkWholeFrame(*terminal);
        // A different screenful each frame, so every row genuinely differs from
        // the shadow rather than repainting to the same bytes.
        std::string redrawBytes;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column)
                redrawBytes.push_back(static_cast<char>(0x21 + ((row * 11 + column * 3) % 0x5E)));
            redrawBytes += "\r\n";
        }
        int sink = 0;
        const Timing timing = measure(3, 40, [&] {
            Ubiquitty::processBytes(terminal.get(), "\033[H");
            Ubiquitty::processBytes(terminal.get(), redrawBytes.c_str());
            sink += walkWholeFrame(*terminal);
        });
        Result result;
        result.name = "damage_walk_full_frame";
        result.unit = "Mcell/s";
        result.value = timing.medianSeconds > 0.0 ? cells / timing.medianSeconds / 1e6 : 0.0;
        results.push_back(result);
        if (!witnessPathTaken(result.name.c_str(), "a dirty run", sink))
            return 1;
    }

    emitJSON(results, "core", std::getenv("UBIQUITTY_BENCH_COMMIT"));
    return 0;
}
