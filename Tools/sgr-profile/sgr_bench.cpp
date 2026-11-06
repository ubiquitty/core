// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// sgr_bench.cpp — Terminal::processBytes microbench for the SGR-heavy scenarios
// (truecolor_text / sgr_churn / dense_truecolor / dense_sgr256), mirroring the
// byte patterns of the ubench generator (Scripts/run-parse-scenarios.sh). Built with -pg for gprof profiling of the
// Core SGR parse/apply path. Not a correctness test; a profiling harness.
//
// Build + run via the sibling driver (derives the Core link set from the
// canonical Scripts/portable-core-sources.txt, so it can't drift behind a split):
//   Tools/sgr-profile/run.sh [scenario] [reps]   # default: truecolor_text 20

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using std::string;

static void appendf(string& o, const char* fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0)
        o.append(b, (size_t)n);
}

// --- scenario byte generators (ported from the ubench generator) ------------------

static string gen_truecolor_text(int columns, long budget)
{
    string out;
    int lineCells = columns - 1 > 1 ? columns - 1 : 1;
    long lines = budget / lineCells;
    if (lines < 1)
        lines = 1;
    for (long i = 0; i < lines; i++) {
        for (int x = 0; x < lineCells; x++) {
            unsigned r = (unsigned)((x * 3L + i) & 0xFF);
            unsigned g = (unsigned)((x * 5L + i) & 0xFF);
            unsigned b = (unsigned)((x * 7L + i) & 0xFF);
            appendf(out, "\x1b[38;2;%u;%u;%um", r, g, b);
            out.push_back((char)(0x41 + ((x + i) % 26)));
        }
        out.append("\x1b[0m\r\n");
    }
    return out;
}

static string gen_sgr_churn(int columns, int rows, long budget)
{
    string out;
    const int attributes[4] = { 0, 1, 4, 7 };
    long per = (long)columns * rows;
    long frames = (budget / 3) / (per < 1 ? 1 : per);
    if (frames < 1)
        frames = 1;
    for (long f = 0; f < frames; f++) {
        out.append("\x1b[H");
        for (int y = 0; y < rows; y++) {
            appendf(out, "\x1b[%d;1H", y + 1);
            for (int x = 0; x < columns; x++) {
                int a = attributes[(x + y + f) & 3];
                long foreground = 16 + ((long)x * 3 + f) % 216;
                long background = 16 + ((long)y * 5 + f) % 216;
                appendf(out, "\x1b[%d;38;5;%ld;48;5;%ldm", a, foreground, background);
                out.push_back((char)(0x41 + ((x + y) % 26)));
            }
        }
        out.append("\x1b[0m");
    }
    return out;
}

static string gen_dense_truecolor(int columns, int rows, long budget)
{
    string out;
    long per = (long)columns * rows;
    long frames = (budget / 2) / (per < 1 ? 1 : per);
    if (frames < 1)
        frames = 1;
    for (long f = 0; f < frames; f++) {
        out.append("\x1b[H");
        for (int y = 0; y < rows; y++) {
            appendf(out, "\x1b[%d;1H", y + 1);
            for (int x = 0; x < columns; x++) {
                // Diagonal sweep like dense_sgr256: hue keyed on x+y+f.
                unsigned d = (unsigned)(((long)x + y + f) & 0xFF);
                unsigned r = d;
                unsigned g = (d + 85) & 0xFF;
                unsigned b = (d + 170) & 0xFF;
                appendf(out, "\x1b[48;2;%u;%u;%um ", r, g, b);
            }
        }
        out.append("\x1b[0m");
    }
    return out;
}

static string gen_dense_sgr256(int columns, int rows, long budget)
{
    string out;
    long per = (long)columns * rows;
    long frames = budget / (per < 1 ? 1 : per);
    if (frames < 1)
        frames = 1;
    for (long f = 0; f < frames; f++) {
        out.append("\x1b[H");
        for (int y = 0; y < rows; y++) {
            appendf(out, "\x1b[%d;1H", y + 1);
            for (int x = 0; x < columns; x++) {
                unsigned index = (unsigned)(16 + ((long)x + y + f) % 216);
                appendf(out, "\x1b[48;5;%um ", index);
            }
        }
        out.append("\x1b[0m");
    }
    return out;
}

int main(int argc, char** argv)
{
    const char* scen = argc > 1 ? argv[1] : "truecolor_text";
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    const int columns = 197, rows = 58;
    const long budget = 6000000;

    string data;
    if (!strcmp(scen, "truecolor_text"))
        data = gen_truecolor_text(columns, budget);
    else if (!strcmp(scen, "sgr_churn"))
        data = gen_sgr_churn(columns, rows, budget);
    else if (!strcmp(scen, "dense_truecolor"))
        data = gen_dense_truecolor(columns, rows, budget);
    else if (!strcmp(scen, "dense_sgr256"))
        data = gen_dense_sgr256(columns, rows, budget);
    else {
        fprintf(stderr, "unknown scenario: %s\n", scen);
        return 2;
    }

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);

    auto t0 = std::chrono::steady_clock::now();
    long totalBytes = 0;
    for (int r = 0; r < reps; r++) {
        Ubiquitty::processBytes(&t, data);
        totalBytes += (long)data.size();
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "%s: %d reps, %.1f MiB, %.3f s, %.1f MiB/s\n",
        scen, reps, totalBytes / 1048576.0, sec, totalBytes / 1048576.0 / sec);
    return 0;
}
