// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Isolated CSI/SGR ingest microbench. Feeds the REAL parser
// (Terminal::processBytes) a stream of per-cell SGR escapes and reports
// MB/s + Mcell/s. A plain-ASCII control at equal byte volume isolates
// the SGR/CSI overhead from raw throughput. No GUI, no PTY, ms-scale.
// Built ad hoc (like perf_core).
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static double feedMBps(const std::string& chunk, long target, long* cellsOut, long cellsPerChunk)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const char* buffer = chunk.data();
    const int length = (int)chunk.size();
    Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length))); // warm up
    long fed = 0, cells = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (fed < target) {
        Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length)));
        fed += length;
        cells += cellsPerChunk;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    *cellsOut = cells;
    return (double)fed / 1e6 / secs;
}

int main()
{
    const long target = 60L * 1024 * 1024; // 60 MB — sub-second per scenario

    // truecolor: one full 24-bit SGR escape + one printable per cell.
    std::string tc;
    long tcCells = 0;
    for (int r = 0; r < 200; ++r) {
        tc += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            int rr = (c * 3) & 255, gg = (c * 5) & 255, bb = (c * 7) & 255;
            tc += "\x1b[38;2;" + std::to_string(rr) + ";" + std::to_string(gg) + ";" + std::to_string(bb) + "m";
            tc += (char)('A' + (c % 26));
            ++tcCells;
        }
    }
    // 256-color variant.
    std::string p256;
    long p256Cells = 0;
    for (int r = 0; r < 200; ++r) {
        p256 += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            p256 += "\x1b[38;5;" + std::to_string((c * 11) & 255) + "m";
            p256 += (char)('A' + (c % 26));
            ++p256Cells;
        }
    }
    // Combined sequence: reset + truecolor + bold in one SGR. The standalone
    // fast path declines (parameterCount != 5), so this drives the general
    // loop + parseExtendedColor per cell.
    std::string combined;
    long combinedCells = 0;
    for (int r = 0; r < 200; ++r) {
        combined += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            int rr = (c * 3) & 255, gg = (c * 5) & 255, bb = (c * 7) & 255;
            combined += "\x1b[0;38;2;" + std::to_string(rr) + ";" + std::to_string(gg) + ";" + std::to_string(bb) + ";1m";
            combined += (char)('A' + (c % 26));
            ++combinedCells;
        }
    }

    // ITU T.416 colon spellings of the truecolor/256 scenarios above —
    // standalone, so they take the fast path's uniform-colon arm.
    std::string colonTc;
    long colonTcCells = 0;
    for (int r = 0; r < 200; ++r) {
        colonTc += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            int rr = (c * 3) & 255, gg = (c * 5) & 255, bb = (c * 7) & 255;
            colonTc += "\x1b[38:2:" + std::to_string(rr) + ":" + std::to_string(gg) + ":" + std::to_string(bb) + "m";
            colonTc += (char)('A' + (c % 26));
            ++colonTcCells;
        }
    }
    std::string colon256;
    long colon256Cells = 0;
    for (int r = 0; r < 200; ++r) {
        colon256 += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            colon256 += "\x1b[38:5:" + std::to_string((c * 11) & 255) + "m";
            colon256 += (char)('A' + (c % 26));
            ++colon256Cells;
        }
    }

    // plain ASCII control: same printable density, no SGR.
    std::string ascii;
    long asciiCells = 0;
    for (int r = 0; r < 200; ++r) {
        ascii += "\x1b[H";
        for (int c = 0; c < 80; ++c) {
            ascii += (char)('A' + (c % 26));
            ++asciiCells;
        }
    }

    struct {
        const char* name;
        std::string* s;
        long cells;
    } S[] = {
        { "sgr_truecolor", &tc, tcCells },
        { "sgr_256", &p256, p256Cells },
        { "sgr_combined_tc", &combined, combinedCells },
        { "sgr_colon_tc", &colonTc, colonTcCells },
        { "sgr_colon_256", &colon256, colon256Cells },
        { "plain_ascii", &ascii, asciiCells },
    };
    std::printf("%-14s %10s %12s\n", "scenario", "MB/s", "Mcell/s");
    for (auto& e : S) {
        long cells = 0;
        double mbps = feedMBps(*e.s, target, &cells, e.cells);
        double secs = (double)target / 1e6 / mbps;
        double mcells = cells / 1e6 / secs;
        std::printf("%-14s %10.1f %12.2f\n", e.name, mbps, mcells);
    }
    return 0;
}
