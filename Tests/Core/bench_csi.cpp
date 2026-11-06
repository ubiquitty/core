// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Isolated CSI-dispatch microbench. Feeds Terminal::processBytes real CSI-dense
// streams and reports MB/s + Mcell/s. cursor_random mirrors the bench scenario
// (CUP + 256-background SGR + char, random positions); csi_moves is pure sequential
// CUP (isolates dispatch cost from random-write cache misses); plain_ascii is
// the control. Built ad hoc.
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <chrono>
#include <cstdio>
#include <string>

static std::string cup(int r, int c) { return "\x1b[" + std::to_string(r) + ";" + std::to_string(c) + "H"; }
static std::string bg256(int n) { return "\x1b[48;5;" + std::to_string(n) + "m"; }

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
    *cellsOut = cells;
    return (double)fed / 1e6 / std::chrono::duration<double>(t1 - t0).count();
}

int main()
{
    const long target = 60L * 1024 * 1024;
    const int rows = 58, columns = 180;

    // cursor_random: CUP(random) + bg256 + char, deterministic LCG (mirrors bench).
    std::string cr;
    long crCells = 0;
    {
        unsigned long st = 0x12345;
        for (int i = 0; i < 20000; ++i) {
            st = (st * 1103515245UL + 12345UL) & 0x7FFFFFFF;
            int r = (int)((st >> 8) % rows) + 1, c = (int)((st >> 16) % columns) + 1;
            cr += cup(r, c);
            cr += bg256(16 + (i % 216));
            cr += (char)('@' + (i % 26));
            ++crCells;
        }
    }
    // csi_moves: pure sequential CUP, no SGR/char — isolates dispatch.
    std::string mv;
    long mvCells = 0;
    for (int i = 0; i < 20000; ++i) {
        mv += cup((i % rows) + 1, (i % columns) + 1);
        ++mvCells;
    }
    // plain ASCII control.
    std::string ascii;
    long asciiCells = 0;
    for (int r = 0; r < 200; ++r) {
        ascii += "\x1b[H";
        for (int c = 0; c < columns; ++c) {
            ascii += (char)('A' + (c % 26));
            ++asciiCells;
        }
    }

    struct {
        const char* name;
        std::string* s;
        long cells;
    } S[] = {
        { "cursor_random", &cr, crCells },
        { "csi_moves", &mv, mvCells },
        { "plain_ascii", &ascii, asciiCells },
    };
    std::printf("%-14s %10s %12s\n", "scenario", "MB/s", "Mcell/s");
    for (auto& e : S) {
        long cells = 0;
        double mbps = feedMBps(*e.s, target, &cells, e.cells);
        double secs = (double)target / 1e6 / mbps;
        std::printf("%-14s %10.1f %12.2f\n", e.name, mbps, cells / 1e6 / secs);
    }
    return 0;
}
