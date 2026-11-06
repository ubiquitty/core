// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_core.cpp — headless Core throughput harness. Feeds a byte stream
// through Terminal::processBytes (no rendering) and times it, so scrolling /
// unicode / cell hot paths can be profiled in isolation (sample this binary).
//
//   bench_core --bench scrolling --rows 24 --columns 80 --mib 1 --iters 200
//   bench_core --pty --file stream.vte --rows 60 --columns 200 --iters 50
//
// Each iteration builds a fresh Terminal (bounds scrollback growth) and times the
// full processBytes pass; reports best + avg ms and MiB/s.

#include "../../Support/PTYReplay.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <memory>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <sys/stat.h>

using clk = std::chrono::steady_clock;

constexpr int argMaxRows = 1000;
constexpr int argMaxColumns = 1000;
constexpr int maxIters = 100000;
constexpr int maxChunk = 16 * 1024 * 1024;
constexpr double maxMiB = 256.0;
constexpr off_t maxFileBytes = 256LL * 1024 * 1024;

static void feedDirect(Ubiquitty::Terminal& t, const std::vector<char>& stream, int chunk)
{
    size_t off = 0;
    while (off < stream.size()) {
        int nseg = static_cast<int>(stream.size() - off < static_cast<size_t>(chunk) ? stream.size() - off : static_cast<size_t>(chunk));
        Ubiquitty::processBytes(&t, std::string_view(&stream[off], static_cast<std::size_t>(nseg)));
        off += static_cast<size_t>(nseg);
    }
}

static bool validateArguments(int rows, int columns, int iters, double mib, int chunk)
{
    if (rows <= 0 || rows > argMaxRows) {
        std::fprintf(stderr, "bad --rows: %d (1..%d)\n", rows, argMaxRows);
        return false;
    }
    if (columns <= 0 || columns > argMaxColumns) {
        std::fprintf(stderr, "bad --columns: %d (1..%d)\n", columns, argMaxColumns);
        return false;
    }
    if (iters <= 0 || iters > maxIters) {
        std::fprintf(stderr, "bad --iters: %d (1..%d)\n", iters, maxIters);
        return false;
    }
    if (mib <= 0.0 || mib > maxMiB) {
        std::fprintf(stderr, "bad --mib: %.3f (0..%.0f]\n", mib, maxMiB);
        return false;
    }
    if (chunk <= 0 || chunk > maxChunk) {
        std::fprintf(stderr, "bad --chunk: %d (1..%d)\n", chunk, maxChunk);
        return false;
    }
    return true;
}

static bool readFile(const char* path, std::vector<char>& out)
{
    struct stat st;
    if (::stat(path, &st) != 0) {
        std::fprintf(stderr, "cannot stat %s\n", path);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        std::fprintf(stderr, "%s is not a regular file\n", path);
        return false;
    }
    if (st.st_size <= 0 || st.st_size > maxFileBytes) {
        std::fprintf(stderr, "%s is too large (%lld bytes, max %lld)\n", path, static_cast<long long>(st.st_size), static_cast<long long>(maxFileBytes));
        return false;
    }
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size()) {
        std::fclose(f);
        std::fprintf(stderr, "short read\n");
        return false;
    }
    std::fclose(f);
    return true;
}

static std::vector<char> buildStream(const std::string& bench, size_t targetBytes)
{
    std::vector<char> s;
    s.reserve(targetBytes + 64);
    if (bench == "scrolling" || bench == "scrolling_fullscreen") {
        // vtebench: printf "y\n" repeated. A program's newline reaches the
        // terminal as CR LF — the line discipline's ONLCR is on by default —
        // so the carriage return is part of the scenario, not decoration.
        // Without it the cursor never leaves the last column, and every line
        // lands its glyph mid-row: the write falls outside the recycled row's
        // deferred-blank window and materialises the whole row to place one
        // cell, a path no program's output produces.
        while (s.size() < targetBytes) {
            s.push_back('y');
            s.push_back('\r');
            s.push_back('\n');
        }
    } else if (bench == "scrolling_region" || bench == "scrolling_small_region") {
        // vtebench scrolling_top_region: a scroll region set below the first row.
        // The margin is what makes it interesting — a band scrolls by a different
        // path from a full-viewport scroll. Nothing else in this battery reaches
        // that code.
        const int firstRow = (bench == "scrolling_small_region") ? 12 : 2;
        char setup[32];
        std::snprintf(setup, sizeof(setup), "\x1b[?1049h\x1b[%d;24r", firstRow);
        for (const char* p = setup; *p; ++p)
            s.push_back(*p);
        while (s.size() < targetBytes) {
            s.push_back('y');
            s.push_back('\r');
            s.push_back('\n');
        }
    } else if (bench == "scrolling_bare_lf") {
        // The same scroll without the carriage return, so the cursor stays in
        // the last column: the worst case for the deferred-blank window, where
        // every line materialises a whole row to place one cell.
        while (s.size() < targetBytes) {
            s.push_back('y');
            s.push_back('\n');
        }
    } else if (bench == "dense_cells") {
        // A screenful of distinct SGR-coloured glyphs, repeated.
        static const char* row = "\x1b[1m\x1b[38;5;82mX\x1b[0m";
        while (s.size() < targetBytes) {
            for (const char* p = row; *p; ++p)
                s.push_back(*p);
        }
    } else if (bench == "unicode") {
        // Mixed multi-byte + combining, the grapheme/width hot path.
        static const char* u = "\xe4\xbd\xa0\xe5\xa5\xbd"
                               "a\xcc\x81 "; // 你好á + space
        while (s.size() < targetBytes) {
            for (const char* p = u; *p; ++p)
                s.push_back(*p);
            if ((s.size() % 160) == 0) {
                s.push_back('\r');
                s.push_back('\n');
            }
        }
    } else if (bench == "emoji") {
        // ZWJ + skin-tone emoji — the GB11 ExtPict-ZWJ cluster path.
        // 👨‍👩‍👧‍👦 (family, 4× ExtPict joined by ZWJ), 👩🏽‍💻 (skin-tone +
        // ZWJ), 🇯🇵 (RI flag pair). Heavy graphemeExtends / isExtendedPictographic.
        static const char* e = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6"
                               "\xf0\x9f\x91\xa9\xf0\x9f\x8f\xbd\xe2\x80\x8d\xf0\x9f\x92\xbb"
                               "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5 ";
        while (s.size() < targetBytes) {
            for (const char* p = e; *p; ++p)
                s.push_back(*p);
            if ((s.size() % 160) == 0) {
                s.push_back('\r');
                s.push_back('\n');
            }
        }
    } else {
        // default: same as scrolling
        while (s.size() < targetBytes) {
            s.push_back('y');
            s.push_back('\r');
            s.push_back('\n');
        }
    }
    return s;
}

int main(int argc, char** argv)
{
    bool laneEnabled = true;
    bool bandLaneEnabled = true;
    int rows = 24, columns = 80, iters = 200;
    double mib = 1.0;
    std::string bench = "scrolling";
    const char* file = nullptr;
    int chunkArgument = 65536; // --chunk simulates PTY read sizes (splits runs)
    bool throughPTY = false;

    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* f) { return std::strcmp(argv[i], f) == 0; };
        if (eq("--lane") && i + 1 < argc) {
            laneEnabled = std::atoi(argv[++i]) != 0;
            continue;
        }
        if (eq("--band-lane") && i + 1 < argc) {
            bandLaneEnabled = std::atoi(argv[++i]) != 0;
            continue;
        }
        if (eq("--rows") && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (eq("--pty"))
            throughPTY = true;
        else if (eq("--columns") && i + 1 < argc)
            columns = std::atoi(argv[++i]);
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--mib") && i + 1 < argc)
            mib = std::atof(argv[++i]);
        else if (eq("--bench") && i + 1 < argc)
            bench = argv[++i];
        else if (eq("--file") && i + 1 < argc)
            file = argv[++i];
        else if (eq("--chunk") && i + 1 < argc)
            chunkArgument = std::atoi(argv[++i]);
    }

    if (!validateArguments(rows, columns, iters, mib, chunkArgument))
        return 2;

    std::vector<char> stream;
    if (file) {
        if (!readFile(file, stream))
            return 1;
    } else {
        stream = buildStream(bench, static_cast<size_t>(mib * 1024 * 1024));
    }

    const double streamMiB = stream.size() / 1048576.0;
    double best = 1e18, total = 0.0;
    const int chunk = chunkArgument;
    std::size_t totalReadCalls = 0;
    std::size_t totalBytesRead = 0;
    std::size_t minReadBytes = 0;
    std::size_t maxReadBytes = 0;

    for (int it = 0; it < iters; ++it) {
        auto t = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*t, rows, columns);
        t->isScrollLaneEnabled = laneEnabled;
        t->isBandScrollLaneEnabled = bandLaneEnabled;

        const auto t0 = clk::now();
        bool ok = true;
        if (throughPTY) {
            Ubiquitty::Bench::PTYReplayResult result = Ubiquitty::Bench::replayPTY(t.get(), std::span<const char>(stream.data(), stream.size()), 1, rows, columns);
            ok = result.ok && result.bytesRead == stream.size();
            totalReadCalls += result.readCalls;
            totalBytesRead += result.bytesRead;
            if (result.minReadBytes > 0 && (minReadBytes == 0 || result.minReadBytes < minReadBytes))
                minReadBytes = result.minReadBytes;
            if (result.maxReadBytes > maxReadBytes)
                maxReadBytes = result.maxReadBytes;
        } else {
            feedDirect(*t, stream, chunk);
        }
        const auto t1 = clk::now();
        if (!ok) {
            std::fprintf(stderr, "PTY replay failed\n");
            t.reset();
            return 1;
        }

        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total += ms;
        if (ms < best)
            best = ms;

        t.reset();
    }

    std::printf("bench=%s rows=%d cols=%d  stream=%.2f MiB iters=%d mode=%s\n", bench.c_str(), rows, columns, streamMiB, iters, throughPTY ? "pty" : "direct");
    std::printf("  best=%.3f ms  avg=%.3f ms  (%.1f MiB/s best)\n", best, total / iters, streamMiB / (best / 1000.0));
    if (throughPTY) {
        const double averageRead = totalReadCalls ? static_cast<double>(totalBytesRead) / static_cast<double>(totalReadCalls) : 0.0;
        std::printf("  pty reads=%zu avg=%.1f bytes min=%zu max=%zu\n", totalReadCalls, averageRead, minReadBytes, maxReadBytes);
    }
    return 0;
}
