// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Headless render-path micro-bench: times the per-row snapshot copy a
// renderer runs every frame (getDisplayRow), against an inline per-cell
// visibleCell loop (OLD). Isolates the render-copy
// cost from ingest — no GUI, no PTY. Fills the grid with the `combining`
// pattern (base 'e' + U+0301), then renders all rows many times.
// Built ad hoc (not part of the suite), like perf_core.cpp. Build with the
// same Core source set as test_vt100; run for a now/old ratio.
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t); // default 80x24
    const int columns = t.columns, rows = t.rows;

    // Fill every visible row with combining clusters.
    std::string feed;
    feed += "\x1b[H";
    for (int r = 0; r < rows; ++r) {
        feed += "\x1b[" + std::to_string(r + 1) + ";1H";
        for (int c = 0; c < columns; ++c) {
            feed += 'e';
            feed += (char)0xCC;
            feed += (char)0x81;
        }
    }
    Ubiquitty::processBytes(&t, feed);

    const long frames = 200000;
    std::vector<Ubiquitty::Cell> out(columns);

    // Variant OLD: an inline per-cell visibleCell copy.
    auto t0 = std::chrono::steady_clock::now();
    volatile std::uint32_t sink = 0;
    for (long f = 0; f < frames; ++f)
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < columns; ++c) {
                const Ubiquitty::Cell* s = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
                out[c] = s ? *s : Ubiquitty::Cell {};
            }
            sink ^= out[columns - 1].codePoint;
        }
    auto t1 = std::chrono::steady_clock::now();

    // Variant getDisplayRow: what buildSnapshot calls now (bulk memcpy).
    for (long f = 0; f < frames; ++f)
        for (int r = 0; r < rows; ++r) {
            Ubiquitty::Screen::getDisplayRow(t, 0, r, out.data(), columns);
            sink ^= out[columns - 1].codePoint;
        }
    auto t2 = std::chrono::steady_clock::now();

    double oldS = std::chrono::duration<double>(t1 - t0).count();
    double newS = std::chrono::duration<double>(t2 - t1).count();
    double cellsPerFrame = double(columns) * rows;
    auto rate = [&](double s) { return cellsPerFrame * frames / 1e6 / s; };
    std::printf("grid %dx%d, %ld frames  (sink=%u)\n", columns, rows, frames, (unsigned)sink);
    std::printf("OLD  inline visibleCell : %7.1f Mcell/s  (%.3fs)\n", rate(oldS), oldS);
    std::printf("NOW  getDisplayRow         : %7.1f Mcell/s  (%.3fs)\n", rate(newS), newS);
    std::printf("ratio NOW/OLD              : %.3f\n", rate(newS) / rate(oldS));
    return 0;
}
