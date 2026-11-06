// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_unicode_parse — parser throughput on a corpus of distinct symbols.
//
// The portable suite's parse lanes are ASCII, so none of them measures the
// cluster, grapheme and wide-cell path, where a regression would go unnoticed.
// This feeds a file of non-ASCII text through processBytes and reports the best
// of N passes.
//
// Best-of-N rather than the mean: the machine is shared, and load inflates the
// mean while leaving the minimum alone.
//
// Includes only Core/Terminal.h and forward-declares processBytes, so it builds
// against an older tree whose VT header sat elsewhere — a harness that fails to
// compile across a range cannot bisect it.

#include "Core/Terminal.h"
#include <memory>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Ubiquitty {
void processBytes(Terminal* t, std::string_view buffer);
}

int main(int argc, char** argv)
{
    // Defaults to the in-tree corpus so the lane needs no argument and no
    // external checkout; a path may still be given to measure another file.
    const char* path = argc > 1 ? argv[1] : "Tests/Performance/unicode-corpus.txt";
    const int passes = argc > 2 ? std::atoi(argv[2]) : 30;

    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::string corpus;
    char chunk[65536];
    for (;;) {
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
        if (got == 0)
            break;
        corpus.append(chunk, got);
    }
    std::fclose(file);
    if (corpus.empty()) {
        std::fprintf(stderr, "%s is empty\n", path);
        return 2;
    }

    double best = 1e300;
    double total = 0.0;
    for (int pass = 0; pass < passes; ++pass) {
        // A fresh terminal per pass: a reused one accumulates scrollback and the
        // later passes would measure a different grid than the first.
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        const auto start = std::chrono::steady_clock::now();
        Ubiquitty::processBytes(terminal.get(), corpus);
        const double milliseconds
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        terminal.reset();
        if (milliseconds < best)
            best = milliseconds;
        total += milliseconds;
    }

    std::printf("bytes %zu passes %d best %.4f ms mean %.4f ms\n",
        corpus.size(), passes, best, total / passes);
    return 0;
}
