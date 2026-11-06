// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_render_plan.cpp — headless timing for buildCellInstances, the per-frame
// pass that turns a grid into the instances a backend draws.
//
// The rest of this directory times the parse side, which runs when output
// arrives. This one runs every frame whether output arrived or not, and without
// this bench it can only be measured through a built app bundle — which is a
// cross-binary comparison of two links, the thing Docs/measuring-a-core-change.md
// says not to trust for a small change.
//
// The grid is filled by feeding a real corpus through the parser, so the cells
// carry the attribute mix that content produces rather than one this file
// invents. Only the build is timed.
//
//   bench_render_plan --file corpus.vte --rows 50 --columns 200 --iters 2000
//
// Reports best and average microseconds per frame, and the instance counts, so
// a change that builds fewer instances is not read as a faster build.

#include "BenchHarness.h"

#include "Core/CellGrid.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int argMaxRows = 1000;
constexpr int argMaxColumns = 1000;
constexpr int maxIters = 1000000;
constexpr off_t maxFileBytes = 256LL * 1024 * 1024;
// Enough padding that the border passes are a measurable share of the frame,
// and close to what a frontend actually asks for.
constexpr float snapshotPaddingInPixels = 12.0f;

bool readFile(const char* path, std::vector<char>& out)
{
    struct stat st;
    if (::stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > maxFileBytes) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    out.resize(static_cast<std::size_t>(st.st_size));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// A screenful of mixed content for the no-corpus case: text, colour, bold and a
// wide character, so the build walks more than one branch.
void paintDefaultScene(Terminal& t)
{
    char command[64];
    for (int row = 1; row <= t.rows; ++row) {
        std::snprintf(command, sizeof(command), "\x1b[%d;1H", row);
        processBytes(&t, command);
        processBytes(&t, "\x1b[1;38;5;82mword\x1b[0m ordinary text ");
        processBytes(&t, "\xE6\x97\xA5\xE6\x9C\xAC ");
        processBytes(&t, "\x1b[4;31munderlined\x1b[0m");
    }
}

} // namespace

int main(int argc, char** argv)
{
    int rows = 50, columns = 200, iters = 2000, warmup = 50;
    const char* file = nullptr;
    const char* platform = "portable";
    const char* commit = "";
    bool shouldEmitJSON = false;

    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* f) { return std::strcmp(argv[i], f) == 0; };
        if (eq("--rows") && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (eq("--columns") && i + 1 < argc)
            columns = std::atoi(argv[++i]);
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--file") && i + 1 < argc)
            file = argv[++i];
        else if (eq("--warmup") && i + 1 < argc)
            warmup = std::atoi(argv[++i]);
        else if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--json"))
            shouldEmitJSON = true;
    }
    if (rows <= 0 || rows > argMaxRows || columns <= 0 || columns > argMaxColumns || iters <= 0 || iters > maxIters) {
        std::fprintf(stderr, "bad geometry or iteration count\n");
        return 2;
    }

    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, rows, columns);
    if (file) {
        std::vector<char> stream;
        if (!readFile(file, stream))
            return 1;
        processBytes(terminal.get(), std::string_view(stream.data(), stream.size()));
    } else {
        paintDefaultScene(*terminal);
    }

    // The builder reads a flat row-major array, which is what the frontend hands
    // it: the grid's rows are a ring, so they are read out once here rather than
    // timed as part of the build.
    const std::size_t cellCount = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns);
    std::vector<Cell> cells(cellCount, defaultBlankCell);
    for (int row = 0; row < rows; ++row)
        terminal->grid->readRow(Row(row), cells.data() + static_cast<std::size_t>(row) * columns);

    TermPalette palette;
    palette.seedDefaults();

    // Every buffer the builder can write, because one left null is a lane it
    // silently skips: a scene full of text reported zero glyphs when only the
    // cell and decoration buffers were supplied, which times a build that never
    // reached the glyph path.
    std::vector<CellInstance> cellsOut(cellCount);
    std::vector<DecorationInstance> decorationsOut(cellCount * 8);
    std::vector<OverlayInstance> overlaysOut(cellCount * 2);
    std::vector<GlyphInstance> glyphsOut(cellCount);
    std::vector<GlyphInstance> wideGlyphsOut(cellCount);
    std::vector<GlyphInstance> proportionalGlyphsOut(cellCount);
    std::vector<GlyphInstance> proportionalStripsOut(cellCount);
    std::vector<GlyphInstance> colorGlyphsOut(cellCount);
    std::vector<GlyphInstance> ligatureGlyphsOut(cellCount);
    std::vector<GlyphInstance> doubleGlyphsOut(cellCount);
    std::vector<OverlayInstance> boxFillsOut(cellCount);

    BuildInputs in;
    in.cells = cells.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.textSizeTable = &terminal->textSizeTable;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;

    InstanceBuffers out;
    out.cells = cellsOut.data();
    out.cellCapacity = cellsOut.size();
    out.decorations = decorationsOut.data();
    out.decorationCapacity = decorationsOut.size();
    out.overlays = overlaysOut.data();
    out.overlayCapacity = overlaysOut.size();
    out.glyphs = glyphsOut.data();
    out.glyphCapacity = glyphsOut.size();
    out.wideGlyphs = wideGlyphsOut.data();
    out.wideGlyphCapacity = wideGlyphsOut.size();
    out.proportionalGlyphs = proportionalGlyphsOut.data();
    out.proportionalGlyphCapacity = proportionalGlyphsOut.size();
    out.proportionalStrips = proportionalStripsOut.data();
    out.proportionalStripCapacity = proportionalStripsOut.size();
    out.colorGlyphs = colorGlyphsOut.data();
    out.colorGlyphCapacity = colorGlyphsOut.size();
    out.ligatureGlyphs = ligatureGlyphsOut.data();
    out.ligatureGlyphCapacity = ligatureGlyphsOut.size();
    out.doubleGlyphs = doubleGlyphsOut.data();
    out.doubleGlyphCapacity = doubleGlyphsOut.size();
    out.boxFills = boxFillsOut.data();
    out.boxFillCapacity = boxFillsOut.size();

    SoftwareRenderer renderer { 8, 16 };
    const GlyphBackend backend = renderer.glyphBackend();

    // One build before timing: the first frame rasterises every glyph the scene
    // needs, and a cold atlas is a measurement of the rasteriser, not the build.
    InstanceCounts counts = buildCellInstances(in, backend, out);

    // Through the shared harness: one build is short enough that the clock's
    // quantum is a real share of it, and the harness repeats a region until it
    // clears that, barriers the result so a build nothing reads is not dropped,
    // and appends the machine-drift control every run carries.
    const auto timing = Bench::measure(warmup, iters, [&] {
        counts = buildCellInstances(in, backend, out);
        Bench::doNotOptimize(counts);
    });
    const double microseconds = timing.medianSeconds * 1e6;

    std::printf("render_plan rows=%d cols=%d cells=%zu iters=%d\n", rows, columns, cellCount, iters);
    std::printf("  best=%.2f us  p95=%.2f us  (%.1f frames/s at median)\n",
        microseconds, timing.p95Seconds * 1e6, 1.0 / timing.medianSeconds);
    std::printf("  instances: cells=%zu glyphs=%zu decorations=%zu wide=%zu boxFills=%zu\n",
        counts.cells, counts.glyphs, counts.decorations, counts.wideGlyphs, counts.boxFills);
    // A glyph rides in its own cell instance as an atlas rectangle, so the
    // separate glyph streams above are legitimately empty for this backend. What
    // must not be empty is coverage: a scene whose cells all carry a degenerate
    // rectangle was drawn as background, and the time above is then the cost of
    // deciding to draw nothing rather than of building a frame.
    std::size_t cellsCarryingCoverage = 0;
    for (std::size_t i = 0; i < counts.cells; ++i) {
        const CellInstance& instance = cellsOut[i];
        if (instance.atlasTopLeft.x != instance.atlasBottomRight.x
            || instance.atlasTopLeft.y != instance.atlasBottomRight.y)
            ++cellsCarryingCoverage;
    }
    std::printf("  cells carrying atlas coverage: %zu of %zu\n", cellsCarryingCoverage, counts.cells);

    // A fingerprint of everything the build produced, so two runs that differ
    // only in a path selection can be compared for equality rather than for
    // speed. FNV-1a over the written prefix of each buffer.
    {
        std::uint64_t hash = 1469598103934665603ull;
        const auto absorb = [&hash](const void* data, std::size_t bytes) {
            const auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < bytes; ++i) {
                hash ^= p[i];
                hash *= 1099511628211ull;
            }
        };
        absorb(cellsOut.data(), counts.cells * sizeof(CellInstance));
        absorb(decorationsOut.data(), counts.decorations * sizeof(DecorationInstance));
        absorb(overlaysOut.data(), counts.overlays * sizeof(OverlayInstance));
        absorb(glyphsOut.data(), counts.glyphs * sizeof(GlyphInstance));
        absorb(wideGlyphsOut.data(), counts.wideGlyphs * sizeof(GlyphInstance));
        absorb(boxFillsOut.data(), counts.boxFills * sizeof(OverlayInstance));
        std::printf("  fingerprint=%016llx\n", static_cast<unsigned long long>(hash));
    }
    if (cellsCarryingCoverage == 0) {
        std::fprintf(stderr, "no cell carries atlas coverage: this timed a background pass, not a frame\n");
        return 3;
    }
    std::vector<Bench::Result> results;
    Bench::Result frame;
    frame.name = "render_build_frame";
    frame.unit = "us";
    frame.value = microseconds;
    frame.p95 = timing.p95Seconds * 1e6;
    results.push_back(frame);

    // The composite: the build decides what to draw, this draws it, and it is
    // the pass that touches every pixel rather than every cell. Timed through
    // render(), which is the only entry the CPU backend offers and carries the
    // build with it — so the number below is a whole frame, and the difference
    // against the build above is what the pixels cost.
    //
    // The fingerprint is over the pixels, not the instances: an arithmetic
    // change in the composite leaves the instances identical and the frame
    // different, and the CPU's bytes are the ones the GPU is held to.
    {
        std::vector<std::uint8_t> rgba;
        int frameWidth = 0;
        int frameHeight = 0;
        if (!renderer.render(in, rgba, frameWidth, frameHeight)) {
            std::fprintf(stderr, "the renderer refused the scene, so no frame was composited\n");
            return 4;
        }
        const auto compositeTiming = Bench::measure(warmup, iters, [&] {
            renderer.render(in, rgba, frameWidth, frameHeight);
            Bench::doNotOptimize(frameWidth);
        });
        std::uint64_t pixelHash = 1469598103934665603ull;
        for (const std::uint8_t byte : rgba) {
            pixelHash ^= byte;
            pixelHash *= 1099511628211ull;
        }
        // A frame of one colour is a frame the composite never drew into, and it
        // fingerprints as stably as a correct one.
        std::size_t distinctFromFirstPixel = 0;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            if (rgba[i] != rgba[0] || rgba[i + 1] != rgba[1] || rgba[i + 2] != rgba[2])
                ++distinctFromFirstPixel;
        }
        std::printf("composite %dx%d px  best=%.2f us  p95=%.2f us\n",
            frameWidth, frameHeight, compositeTiming.medianSeconds * 1e6, compositeTiming.p95Seconds * 1e6);
        std::printf("  pixels unlike the first: %zu of %zu\n", distinctFromFirstPixel, rgba.size() / 4);
        std::printf("  frame fingerprint=%016llx\n", static_cast<unsigned long long>(pixelHash));
        if (!distinctFromFirstPixel) {
            std::fprintf(stderr, "every pixel matches the first: this timed a fill, not a composite\n");
            return 4;
        }
        Bench::Result composited;
        composited.name = "render_composite_frame";
        composited.unit = "us";
        composited.value = compositeTiming.medianSeconds * 1e6;
        composited.p95 = compositeTiming.p95Seconds * 1e6;
        results.push_back(composited);
    }

    // The snapshot entry: capture, build and composite, plus the padding a
    // frontend asks for. This is what the Web page renders every frame through,
    // and it is a longer path than render() — the two are reported apart so a
    // change to one is not read against the other's total.
    {
        TerminalSnapshotOptions snapshotOptions;
        // The cursor draws: it is the overlay pass, and a snapshot lane without
        // one leaves the pass that runs last out of both the time and the
        // fingerprint.
        snapshotOptions.drawCursor = true;
        snapshotOptions.style.paddingInPixels = { snapshotPaddingInPixels, snapshotPaddingInPixels, snapshotPaddingInPixels, snapshotPaddingInPixels };
        int snapshotWidth = 0;
        int snapshotHeight = 0;
        std::vector<std::uint8_t> snapshot = renderTerminalSnapshotRGBA(*terminal, snapshotOptions, snapshotWidth, snapshotHeight, {}, nullptr, &renderer);
        if (snapshot.empty()) {
            std::fprintf(stderr, "the snapshot entry returned no frame\n");
            return 5;
        }
        const auto snapshotTiming = Bench::measure(warmup, iters, [&] {
            snapshot = renderTerminalSnapshotRGBA(*terminal, snapshotOptions, snapshotWidth, snapshotHeight, {}, nullptr, &renderer);
            Bench::doNotOptimize(snapshotWidth);
        });
        std::uint64_t snapshotHash = 1469598103934665603ull;
        for (const std::uint8_t byte : snapshot) {
            snapshotHash ^= byte;
            snapshotHash *= 1099511628211ull;
        }
        std::printf("snapshot %dx%d px (padding %d)  best=%.2f us  p95=%.2f us\n",
            snapshotWidth, snapshotHeight, static_cast<int>(snapshotPaddingInPixels),
            snapshotTiming.medianSeconds * 1e6, snapshotTiming.p95Seconds * 1e6);
        std::printf("  snapshot fingerprint=%016llx\n", static_cast<unsigned long long>(snapshotHash));
        // The padded frame must be larger than the grid it wraps, or the padding
        // never reached the entry and the border passes are not in the number.
        if (snapshotWidth <= columns * renderer.cellWidth() || snapshotHeight <= rows * renderer.cellHeight()) {
            std::fprintf(stderr, "the snapshot is no larger than its grid: the padding did not reach it\n");
            return 5;
        }
        Bench::Result snapshotFrame;
        snapshotFrame.name = "render_snapshot_frame";
        snapshotFrame.unit = "us";
        snapshotFrame.value = snapshotTiming.medianSeconds * 1e6;
        snapshotFrame.p95 = snapshotTiming.p95Seconds * 1e6;
        results.push_back(snapshotFrame);
    }
    if (shouldEmitJSON)
        Bench::emitJSON(results, platform, commit);
    return 0;
}
