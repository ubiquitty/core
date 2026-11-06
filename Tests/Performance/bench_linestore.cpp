// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_linestore.cpp — focused LineStore microbenchmarks: append/eviction,
// and the fold of cold blocks into the row form with its decode back.
//
// This isolates scrollback slab churn from the VT parser and renderer:
//
//   clang++ -std=c++23 -O3 -DNDEBUG -I Source \
//     Tests/Performance/bench_linestore.cpp Source/Core/LineStore.cpp \
//     -o bench_linestore
//   ./bench_linestore --length 0 --lines 5000000 --iters 7
//   ./bench_linestore --length 1024 --lines 200000 --iters 7
//   ./bench_linestore --lane fold --shape rainbow
//   ./bench_linestore --lane decode --shape shell
//   ./bench_linestore --lane ratio
//
// Lanes:
//
//   append   the steady-state append into a full ring (the flood's cost)
//   fold     LineStore::foldColdBlocks over every cold block of a store filled
//            with one row shape: microseconds per block and cells per second
//   decode   a sequential read of every line after the fold, which decodes
//            each folded block once, against the same read over raw blocks
//   ratio    the bytes a folded block takes against the raw slab it replaced
//            and against the cells it holds, per shape
//
// Shapes (80 cells a row unless the shape says otherwise):
//
//   onepen     one pen, letters
//   threepens  three pen runs of a row
//   rainbow    every cell its own colour
//   shell      a shell-like corpus: prompts, listings in a few colours, blank
//              rows, long wrapped rows, of mixed length

#include "Core/LineStore.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using clk = std::chrono::steady_clock;

namespace {

void fillRow(std::vector<Ubiquitty::Cell>& row)
{
    for (std::size_t i = 0; i < row.size(); ++i) {
        row[i] = Ubiquitty::Cell {};
        row[i].codePoint = static_cast<uint32_t>('a' + (i % 26));
    }
}

double runAppendOnce(int capacity, int lines, int length, std::size_t& arenaBytes)
{
    Ubiquitty::LineStore store;
    store.init(capacity);

    const int rowCells = std::max(1, length);
    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(rowCells));
    fillRow(row);

    const auto t0 = clk::now();
    for (int i = 0; i < lines; ++i) {
        if (length > 0)
            row[0].codePoint = static_cast<uint32_t>(0x100 + (i & 0xffff));
        store.append(row.data(), length, 0, 0, false, false);
    }
    const auto t1 = clk::now();

    arenaBytes = store.arenaBytes();
    volatile int keep = store.count();
    keep += static_cast<int>(arenaBytes & 0xffff);
    (void)keep;

    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

enum class Weight : std::uint8_t {
    Regular,
    Bold,
};

Ubiquitty::Pen penOf(uint16_t foreground, uint16_t background, Weight weight)
{
    Ubiquitty::Pen pen {};
    Ubiquitty::penReset(&pen);
    pen.foregroundIndex = foreground;
    pen.backgroundIndex = background;
    if (weight == Weight::Bold)
        pen.attributes.add(Ubiquitty::VT::Attribute::Bold);
    return pen;
}

Ubiquitty::Cell cellOf(uint32_t codePoint, const Ubiquitty::Pen& pen)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = codePoint;
    cell.pen = pen;
    return cell;
}

// One row of a shape; `index` varies the text so rows differ as real ones do.
std::vector<Ubiquitty::Cell> shapedRow(std::string_view shape, int index)
{
    std::vector<Ubiquitty::Cell> row;
    const Ubiquitty::Pen plain = penOf(Ubiquitty::colorDefault, Ubiquitty::colorDefault, Weight::Regular);
    if (shape == "onepen") {
        row.resize(80);
        for (int column = 0; column < 80; ++column)
            row[static_cast<std::size_t>(column)] = cellOf(static_cast<uint32_t>('a' + (column + index) % 26), plain);
    } else if (shape == "threepens") {
        row.resize(80);
        const Ubiquitty::Pen pens[3] = { penOf(1, Ubiquitty::colorDefault, Weight::Regular), penOf(2, Ubiquitty::colorDefault, Weight::Bold), penOf(Ubiquitty::colorDefault, 4, Weight::Regular) };
        for (int column = 0; column < 80; ++column)
            row[static_cast<std::size_t>(column)] = cellOf(static_cast<uint32_t>('a' + (column + index) % 26), pens[column / 27]);
    } else if (shape == "rainbow") {
        row.resize(80);
        for (int column = 0; column < 80; ++column) {
            Ubiquitty::Pen pen = plain;
            pen.foregroundIndex = Ubiquitty::colorRGB;
            pen.foregroundRed = static_cast<uint8_t>(column * 3);
            pen.foregroundGreen = static_cast<uint8_t>(column * 5 + index);
            pen.foregroundBlue = static_cast<uint8_t>(column * 7);
            row[static_cast<std::size_t>(column)] = cellOf(static_cast<uint32_t>('a' + (column + index) % 26), pen);
        }
    } else {
        // shell: a prompt, a listing in three colours, a plain log line, a
        // blank row and a long wrapped row, in rotation.
        switch (index % 6) {
        case 0: {
            const std::string prompt = "user@host:~/projects/ubiquitty $ ls -la Source/Core";
            const Ubiquitty::Pen green = penOf(2, Ubiquitty::colorDefault, Weight::Bold);
            for (std::size_t i = 0; i < prompt.size(); ++i)
                row.push_back(cellOf(static_cast<uint32_t>(prompt[i]), i < 9 ? green : plain));
            break;
        }
        case 1:
        case 2: {
            const std::string listing = "-rw-r--r--  1 user  staff   12345 Oct 12 17:00 LineStore.cpp";
            const Ubiquitty::Pen blue = penOf(4, Ubiquitty::colorDefault, Weight::Bold);
            const Ubiquitty::Pen dim = penOf(8, Ubiquitty::colorDefault, Weight::Regular);
            for (std::size_t i = 0; i < listing.size(); ++i)
                row.push_back(cellOf(static_cast<uint32_t>(listing[i]), i < 10 ? dim : (i >= 50 ? blue : plain)));
            break;
        }
        case 3: {
            const std::string log = "[info] request " + std::to_string(index) + " served in 3ms";
            for (const char character : log)
                row.push_back(cellOf(static_cast<uint32_t>(character), plain));
            break;
        }
        case 4:
            break;
        default:
            row.resize(80);
            for (int column = 0; column < 80; ++column)
                row[static_cast<std::size_t>(column)] = cellOf(static_cast<uint32_t>('0' + (column + index) % 10), plain);
            break;
        }
    }
    return row;
}

struct ShapedStore {
    Ubiquitty::LineStore store;
    std::size_t cells = 0;
};

// A store holding `lines` rows of a shape, every one appended raw.
void fillShaped(ShapedStore& shaped, std::string_view shape, int lines)
{
    shaped.store.init(lines);
    shaped.cells = 0;
    for (int index = 0; index < lines; ++index) {
        const std::vector<Ubiquitty::Cell> row = shapedRow(shape, index);
        // A blank row still hands the store a row pointer, as the capture does.
        const Ubiquitty::Cell* cells = row.empty() ? &Ubiquitty::defaultBlankCell : row.data();
        shaped.store.append(cells, static_cast<int>(row.size()), 0, 0, (index % 6) == 5 && shape == "shell", false);
        shaped.cells += row.size();
    }
}

int foldEverythingCold(Ubiquitty::LineStore& store)
{
    while (store.foldColdBlocks(1)) {
    }
    return store.foldedBlockCount();
}

uint32_t readEveryLine(const Ubiquitty::LineStore& store)
{
    uint32_t acc = 0;
    for (int line = 0; line < store.count(); ++line) {
        const Ubiquitty::StoredLine* stored = store.at(line);
        for (int i = 0; i < stored->length; ++i)
            acc += stored->cells[i].codePoint;
    }
    return acc;
}

void runFoldLane(std::string_view shape, int lines, int iters)
{
    double best = 1e18;
    int foldedBlocks = 0;
    std::size_t foldedCells = 0;
    for (int iteration = 0; iteration < iters; ++iteration) {
        ShapedStore shaped;
        fillShaped(shaped, shape, lines);
        const auto t0 = clk::now();
        const int folded = foldEverythingCold(shaped.store);
        const auto t1 = clk::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) {
            best = ms;
            foldedBlocks = folded;
            // The newest two blocks stay raw; what folded is the rest of the cells.
            foldedCells = shaped.cells * static_cast<std::size_t>(folded) / static_cast<std::size_t>(std::max(1, shaped.store.blockCount()));
        }
    }
    std::printf("bench=linestore lane=fold shape=%.*s lines=%d blocks=%d iters=%d\n", static_cast<int>(shape.size()), shape.data(), lines, foldedBlocks, iters);
    std::printf("  best=%.3f ms  %.1f us/block  %.1f Mcells/s\n", best, foldedBlocks > 0 ? best * 1000.0 / foldedBlocks : 0.0, best > 0 ? foldedCells / (best * 1000.0) : 0.0);
}

void runDecodeLane(std::string_view shape, int lines, int iters)
{
    ShapedStore shaped;
    fillShaped(shaped, shape, lines);
    volatile uint32_t sink = 0;
    double rawBest = 1e18;
    for (int iteration = 0; iteration < iters; ++iteration) {
        const auto t0 = clk::now();
        sink = readEveryLine(shaped.store);
        const auto t1 = clk::now();
        rawBest = std::min(rawBest, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const int folded = foldEverythingCold(shaped.store);
    // Every pass walks the whole store, so with more folded blocks than the
    // cache holds each pass decodes every folded block again.
    double foldedBest = 1e18;
    for (int iteration = 0; iteration < iters; ++iteration) {
        const auto t0 = clk::now();
        sink = readEveryLine(shaped.store);
        const auto t1 = clk::now();
        foldedBest = std::min(foldedBest, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    (void)sink;
    const std::size_t foldedCells = shaped.cells * static_cast<std::size_t>(folded) / static_cast<std::size_t>(std::max(1, shaped.store.blockCount()));
    const double decodeMilliseconds = foldedBest - rawBest;
    std::printf("bench=linestore lane=decode shape=%.*s lines=%d blocks=%d folded=%d iters=%d\n", static_cast<int>(shape.size()), shape.data(), lines, shaped.store.blockCount(), folded, iters);
    std::printf("  raw walk=%.3f ms  folded walk=%.3f ms  decode=%.1f us/block  %.1f Mcells/s\n", rawBest, foldedBest, folded > 0 ? decodeMilliseconds * 1000.0 / folded : 0.0,
        decodeMilliseconds > 0 ? foldedCells / (decodeMilliseconds * 1000.0) : 0.0);
}

void runRatioLane(std::string_view shape, int lines)
{
    ShapedStore shaped;
    fillShaped(shaped, shape, lines);
    const std::size_t rawBytes = shaped.store.arenaBytes();
    const int folded = foldEverythingCold(shaped.store);
    const std::size_t foldedBytes = shaped.store.foldedBytes();
    const std::size_t rawSlabBytes = static_cast<std::size_t>(folded) * (rawBytes / static_cast<std::size_t>(std::max(1, shaped.store.blockCount())));
    const std::size_t foldedCells = shaped.cells * static_cast<std::size_t>(folded) / static_cast<std::size_t>(std::max(1, shaped.store.blockCount()));
    std::printf("bench=linestore lane=ratio shape=%.*s lines=%d blocks=%d folded=%d\n", static_cast<int>(shape.size()), shape.data(), lines, shaped.store.blockCount(), folded);
    std::printf("  folded=%.1f KiB  raw slabs=%.1f KiB (%.3f)  cells=%.1f KiB (%.3f)  %.2f bytes/cell\n", foldedBytes / 1024.0, rawSlabBytes / 1024.0,
        rawSlabBytes > 0 ? static_cast<double>(foldedBytes) / static_cast<double>(rawSlabBytes) : 0.0, foldedCells * sizeof(Ubiquitty::Cell) / 1024.0,
        foldedCells > 0 ? static_cast<double>(foldedBytes) / static_cast<double>(foldedCells * sizeof(Ubiquitty::Cell)) : 0.0,
        foldedCells > 0 ? static_cast<double>(foldedBytes) / static_cast<double>(foldedCells) : 0.0);
}

} // namespace

int main(int argc, char** argv)
{
    int capacity = Ubiquitty::VT::fallbackScrollbackLines;
    int lines = 1000000;
    int length = maxColumns;
    int iters = 7;
    std::string lane = "append";
    std::string shape = "all";

    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (eq("--capacity") && i + 1 < argc)
            capacity = std::atoi(argv[++i]);
        else if (eq("--lines") && i + 1 < argc)
            lines = std::atoi(argv[++i]);
        else if ((eq("--length") || eq("--len")) && i + 1 < argc)
            length = std::atoi(argv[++i]);
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--lane") && i + 1 < argc)
            lane = argv[++i];
        else if (eq("--shape") && i + 1 < argc)
            shape = argv[++i];
        else {
            // Refused, not ignored. An unrecognised flag leaves `length` at its
            // default, so the lane runs a different measurement than the command
            // named — full-width appends where zero-length ring bookkeeping was
            // asked for — and every line of the output still reads plausibly
            // enough to quote.
            std::fprintf(stderr, "bench_linestore: unrecognised argument \"%s\"\n", argv[i]);
            std::fprintf(stderr, "  usage: --lane append|fold|decode|ratio --shape onepen|threepens|rainbow|shell|all --capacity N --lines N --length N --iters N\n");
            return 2;
        }
    }

    if (capacity < 1)
        capacity = 1;
    if (lines < 1)
        lines = 1;
    if (length < 0)
        length = 0;
    if (length > maxColumns)
        length = maxColumns;
    if (iters < 1)
        iters = 1;

    if (lane != "append") {
        if (lane != "fold" && lane != "decode" && lane != "ratio") {
            std::fprintf(stderr, "bench_linestore: unknown lane \"%s\"\n", lane.c_str());
            return 2;
        }
        // The shaped lanes size the store by lines; 20k rows of 80 cells is
        // ~200 blocks, enough that the cache turns over many times.
        const int shapedLines = lines == 1000000 ? 20000 : lines;
        const char* shapes[] = { "onepen", "threepens", "rainbow", "shell" };
        for (const char* candidate : shapes) {
            if (shape != "all" && shape != candidate)
                continue;
            if (lane == "fold")
                runFoldLane(candidate, shapedLines, iters);
            else if (lane == "decode")
                runDecodeLane(candidate, shapedLines, iters);
            else
                runRatioLane(candidate, shapedLines);
        }
        return 0;
    }

    double best = 1e18;
    double total = 0.0;
    std::size_t arenaBytes = 0;
    for (int i = 0; i < iters; ++i) {
        std::size_t bytes = 0;
        const double ms = runAppendOnce(capacity, lines, length, bytes);
        if (ms < best) {
            best = ms;
            arenaBytes = bytes;
        }
        total += ms;
    }

    std::printf("bench=linestore capacity=%d lines=%d len=%d iters=%d\n", capacity, lines, length, iters);
    std::printf("  best=%.3f ms  avg=%.3f ms  best=%.1f Mlines/s  arena=%.1f KiB\n", best, total / iters, lines / (best * 1000.0), arenaBytes / 1024.0);
    return 0;
}
