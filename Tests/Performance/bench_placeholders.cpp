// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_placeholders.cpp — headless timing for resolvePlaceholders, the pass a
// frame runs whenever a Unicode-placeholder image is on screen.
//
// The scene is the one the pass exists for: a virtual placement covering the
// whole grid, and every cell a placeholder. Each row opens with a cell naming
// its row and the rest inherit their column from the left, which is how a
// program actually writes them.
//
//   bench_placeholders --iters 500 [--json]

#include "BenchHarness.h"

#include "Core/Image/ImagePlacement.h"
#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

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
constexpr int benchImageID = 31;

void appendCodePoint(std::string& out, std::uint32_t codePoint)
{
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
}

void appendDiacritic(std::string& out, std::uint32_t codePoint)
{
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
}

} // namespace

int main(int argc, char** argv)
{
    int iters = 500;
    const char* platform = "portable";
    const char* commit = "";
    bool shouldEmitJSON = false;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--json"))
            shouldEmitJSON = true;
        else if (argv[i][0] != '-')
            iters = std::atoi(argv[i]);
    }
    if (iters <= 0 || iters > 1000000)
        iters = 500;

    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, benchRows, benchColumns);
    // A virtual placement covering the grid, so every placeholder cell resolves.
    // The image is one pixel per grid cell: any smaller and a cell's source
    // sub-rectangle rounds to nothing and the pass skips it, which times a walk
    // that decides not to place rather than one that places.
    std::string transmit = "\033_Ga=T,U=1,i=31,f=24,s=200,v=50,c=200,r=50;";
    // Zero bytes, so the payload is a run of the base64 digit for zero. 24-bit
    // colour over the whole image is three bytes a pixel.
    transmit.append(static_cast<std::size_t>(benchColumns) * benchRows * 3 * 4 / 3, 'A');
    transmit += "\033\\";
    processBytes(terminal.get(), transmit);

    // The row diacritics for the first four rows, cycled: the decode's cost does
    // not turn on which row a cell names, and the table lookup is the same one.
    static const std::uint32_t rowDiacritics[] = { 0x0305, 0x030D, 0x030E, 0x0310 };
    std::string screen;
    char cursor[32];
    for (int row = 0; row < benchRows; ++row) {
        std::snprintf(cursor, sizeof(cursor), "\033[%d;1H\033[38;5;%dm", row + 1, benchImageID);
        screen += cursor;
        appendCodePoint(screen, Kitty::placeholderCodePoint);
        appendDiacritic(screen, rowDiacritics[row % 4]);
        for (int column = 1; column < benchColumns; ++column)
            appendCodePoint(screen, Kitty::placeholderCodePoint);
    }
    processBytes(terminal.get(), screen);

    // One quad per cell: the pass stops the moment the span is full, and a span
    // sized to the placement table fills after a handful of cells — which times
    // the first rows of the screen rather than the screen.
    std::vector<Image::ImageQuad> out(static_cast<std::size_t>(benchRows) * benchColumns);
    int count = 0;
    const auto timing = Bench::measure(20, iters, [&] {
        count = Kitty::resolvePlaceholders(terminal.get(), { 8.0f, 16.0f }, { 0.0f, 0.0f }, 1.0f, 0,
            std::span<Image::ImageQuad> { out });
        Bench::doNotOptimize(count);
    });
    std::printf("placeholders %dx%d  best=%.2f us  p95=%.2f us  quads=%d\n",
        benchRows, benchColumns, timing.medianSeconds * 1e6, timing.p95Seconds * 1e6, count);
    // A walk that resolved nothing times the decision to decode nothing, which
    // is not the pass this exists to measure.
    if (count <= 0) {
        std::fprintf(stderr, "no placeholder resolved: this timed a walk that decoded nothing\n");
        return 2;
    }
    Bench::Result result;
    result.name = "placeholder_pass";
    result.unit = "us";
    result.value = timing.medianSeconds * 1e6;
    result.p95 = timing.p95Seconds * 1e6;
    std::vector<Bench::Result> results { result };
    if (shouldEmitJSON)
        Bench::emitJSON(results, platform, commit);
    return 0;
}
