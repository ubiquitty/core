// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// throughput_bench — headless byte-stream throughput (the vtebench axis).
//
// How fast the emulator consumes a PTY byte stream. This drives processBytes
// with vtebench-style workloads and reports MiB/s + ns/byte.
//
// Workloads:
//   scroll   — long ASCII lines + LF (dense scroll-up, the classic `cat`)
//   dense    — full-width random printable rows (per-cell write cost)
//   sgr      — heavy SGR color churn (parser + pen cost)
//   lf       — bare line feeds (scroll path only, no glyph writes)
//
// Default mode feeds Core directly. `--pty` replays each workload through a
// forkpty writer and drains the master fd into Core, adding the kernel PTY / read
// loop axis while staying headless. `--both` prints direct and PTY runs together.

#include "../../Support/PTYReplay.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int maxBenchRows = 1000;
constexpr int maxBenchColumns = 1000;
constexpr int maxBenchReps = 100000;
constexpr int maxBenchLines = 1000000;

enum class Mode {
    Direct,
    PTY,
    Both,
};

void run(const char* name, Ubiquitty::Terminal* t, const std::string& stream, int reps, int inputLines)
{
    // Warm once so allocation / first-touch isn't charged to the timed run.
    Ubiquitty::processBytes(t, stream);

    Clock::time_point t0 = Clock::now();
    for (int i = 0; i < reps; ++i)
        Ubiquitty::processBytes(t, stream);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    double bytes = static_cast<double>(stream.size()) * reps;
    double lines = static_cast<double>(inputLines) * reps;
    double mibps = (bytes / (1024.0 * 1024.0)) / (ms / 1000.0);
    double nsPerByte = (ms * 1e6) / bytes;
    double mlinesPerSecond = (lines / 1000000.0) / (ms / 1000.0);
    double nsPerLine = (ms * 1e6) / lines;
    std::printf("  %-8s %8.1f MiB/s   %6.2f ns/byte   %6.1f Mlines/s   %6.1f ns/line   (%.0f bytes x%d in %.1f ms)\n",
        name, mibps, nsPerByte, mlinesPerSecond, nsPerLine,
        static_cast<double>(stream.size()), reps, ms);
}

void runPTY(const char* name, Ubiquitty::Terminal* t, const std::string& stream, int reps, int inputLines,
    int rows, int columns)
{
    Ubiquitty::Bench::PTYReplayResult warm = Ubiquitty::Bench::replayPTY(t,
        std::span<const char>(stream.data(), stream.size()), 1, rows, columns);
    if (!warm.ok || warm.bytesRead != stream.size()) {
        std::printf("  %-8s PTY replay failed during warmup\n", name);
        return;
    }

    Clock::time_point t0 = Clock::now();
    Ubiquitty::Bench::PTYReplayResult result = Ubiquitty::Bench::replayPTY(t,
        std::span<const char>(stream.data(), stream.size()), reps, rows, columns);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!result.ok) {
        std::printf("  %-8s PTY replay failed\n", name);
        return;
    }

    double bytes = static_cast<double>(stream.size()) * reps;
    const double bytesRead = static_cast<double>(result.bytesRead);
    if (bytesRead != bytes) {
        std::printf("  %-8s PTY byte-count mismatch: expected %.0f payload bytes, read %.0f\n",
            name, bytes, bytesRead);
        return;
    }
    double lines = static_cast<double>(inputLines) * reps;
    double mibps = (bytesRead / (1024.0 * 1024.0)) / (ms / 1000.0);
    double nsPerByte = (ms * 1e6) / bytesRead;
    double mlinesPerSecond = (lines / 1000000.0) / (ms / 1000.0);
    double nsPerLine = (ms * 1e6) / lines;
    const double averageRead = result.readCalls
        ? bytesRead / static_cast<double>(result.readCalls)
        : 0.0;
    std::printf("  %-8s %8.1f MiB/s   %6.2f ns/byte   %6.1f Mlines/s   %6.1f ns/line   (%.0f bytes x%d; %.0f read in %.1f ms; reads=%zu avg=%.1f min=%zu max=%zu)\n",
        name, mibps, nsPerByte, mlinesPerSecond, nsPerLine,
        static_cast<double>(stream.size()), reps, bytesRead, ms,
        result.readCalls, averageRead, result.minReadBytes, result.maxReadBytes);
}

bool validateArguments(int rows, int columns, int reps, int lines)
{
    if (rows <= 0 || rows > maxBenchRows) {
        std::fprintf(stderr, "bad --rows: %d (1..%d)\n", rows, maxBenchRows);
        return false;
    }
    if (columns <= 0 || columns > maxBenchColumns) {
        std::fprintf(stderr, "bad --columns: %d (1..%d)\n", columns, maxBenchColumns);
        return false;
    }
    if (reps <= 0 || reps > maxBenchReps) {
        std::fprintf(stderr, "bad --reps: %d (1..%d)\n", reps, maxBenchReps);
        return false;
    }
    if (lines <= 0 || lines > maxBenchLines) {
        std::fprintf(stderr, "bad --lines: %d (1..%d)\n", lines, maxBenchLines);
        return false;
    }
    return true;
}

void runOne(const char* name, const std::string& stream, int rows, int columns, int reps,
    int inputLines, bool throughPTY)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);
    if (throughPTY)
        runPTY(name, &t, stream, reps, inputLines, rows, columns);
    else
        run(name, &t, stream, reps, inputLines);
}

std::string buildScroll(int columns, int lines)
{
    std::string s;
    s.reserve(static_cast<size_t>((columns + 1) * lines));
    for (int l = 0; l < lines; ++l) {
        for (int c = 0; c < columns; ++c)
            s.push_back(static_cast<char>('A' + ((l + c) % 26)));
        s.push_back('\n');
    }
    return s;
}

std::string buildLf(int lines)
{
    return std::string(static_cast<size_t>(lines), '\n');
}

std::string buildSGR(int columns, int lines)
{
    std::string s;
    for (int l = 0; l < lines; ++l) {
        for (int c = 0; c < columns; ++c) {
            char buffer[24];
            int foreground = 30 + (c % 8), background = 40 + (l % 8);
            int n = std::snprintf(buffer, sizeof(buffer), "\x1b[%d;%dm%c", foreground, background,
                static_cast<char>('A' + (c % 26)));
            s.append(buffer, static_cast<size_t>(n));
        }
        s.append("\x1b[0m\n");
    }
    return s;
}

} // namespace

int main(int argc, const char* argv[])
{
    int rows = 24, columns = 80, reps = 200, lines = 2000;
    Mode mode = Mode::Direct;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pty")) {
            mode = Mode::PTY;
        } else if (!std::strcmp(argv[i], "--both")) {
            mode = Mode::Both;
        } else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) {
            rows = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--columns") && i + 1 < argc) {
            columns = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--lines") && i + 1 < argc) {
            lines = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: throughput_bench [--pty|--both] [--rows N] [--columns N] [--reps N] [--lines N]\n");
            return 2;
        }
    }

    if (!validateArguments(rows, columns, reps, lines))
        return 2;

    std::printf("throughput-bench: rows=%d cols=%d reps=%d lines=%d mode=%s\n",
        rows, columns, reps, lines,
        mode == Mode::PTY ? "pty" : mode == Mode::Both ? "both"
                                                       : "direct");

    const std::string scroll = buildScroll(columns, lines);
    const std::string lf = buildLf(lines);
    const std::string sgr = buildSGR(columns, lines);

    auto runSet = [&](bool throughPTY) {
        if (mode == Mode::Both)
            std::printf("%s:\n", throughPTY ? "pty" : "direct");
        runOne("scroll", scroll, rows, columns, reps, lines, throughPTY);
        runOne("lf", lf, rows, columns, reps, lines, throughPTY);
        runOne("sgr", sgr, rows, columns, reps, lines, throughPTY);
    };
    if (mode == Mode::Direct) {
        runSet(false);
    } else if (mode == Mode::PTY) {
        runSet(true);
    } else {
        runSet(false);
        runSet(true);
    }
    return 0;
}
