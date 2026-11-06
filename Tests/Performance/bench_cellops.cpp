// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_cellops.cpp — standalone microbenchmark for the two hot Cell memory
// operations on the flood path:
//
//   1. Scrollback append copy (LineStore::append): ~90 Cells (2880 B) from a
//      live row into a sequential-append slab. Source rows cycle through a
//      49-row x 202-column ring (L2-resident); destination slabs (8192 Cells,
//      256 KB each) append sequentially through a 256 MB pool (streaming/cold).
//   2. Blank fill (CellGrid fillBlank): ~90 Cells with a 32-byte pattern into
//      live grid rows (cache-hot, matching erase/scroll-region destinations).
//
// Candidates run >= 1 GB of traffic per repetition, 3 repetitions interleaved
// round-robin (the host is shared; only RELATIVE ordering is meaningful).
// Median GB/s is reported per candidate.
//
// Build (no repo dependencies):
//   clang++ -std=c++23 -O2 -DNDEBUG -fno-exceptions -fno-rtti \
//       -fno-builtin-memcpy -fno-builtin-memset -fno-strict-aliasing \
//       Tests/Performance/bench_cellops.cpp -o /tmp/bench_cellops
//
// -fno-builtin-memcpy keeps the loop candidates as loops (loop-idiom
// recognition would otherwise rewrite them into the very libc memcpy they
// are being compared against) and keeps the explicit memcpy candidate an
// actual libc call (matching the repo build, where the runtime-variable
// length is a libc call, not an inline expansion).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// --- Cell layout (mirrors Source/Core/Cell.h + Pen.h; 32 bytes, no padding) ---
//
// The prefix keeps these out of the production namespace of names. This file
// compiles with a bare clang++ line and no repo headers, so the layout has to
// be restated here; spelling it `Cell` puts a second global definition of a
// production type's name in the tree, and any check that resolves a forward
// declaration by pooling definitions across the repo will bind a production
// `struct Cell;` to this stand-in and report the tree clean.
//
// The size assertion catches a field added or dropped. It does not catch a
// reorder, which leaves the total at 32 bytes while moving where the stores
// land inside the line — the exact thing every candidate below is timing. Nor
// a rename, which is how a mirror comes to describe a struct that no longer
// exists while still measuring 32 bytes of something.
//
// Scripts/check-bench-mirrors.py is what covers both: it compares this field
// sequence against the real one textually, from outside a file that cannot
// include the header it copies.

// mirrors: Source/Core/Pen.h Pen
struct BenchPen {
    uint16_t attributes;
    uint16_t foregroundIndex;
    uint16_t backgroundIndex;
    uint16_t underlineIndex : 9;
    uint16_t semanticRole : 2;
    uint16_t isForegroundAssignedDefault : 1;
    uint16_t isBackgroundAssignedDefault : 1;
    uint16_t underlineReserved : 3;
    uint8_t foregroundRed, foregroundGreen, foregroundBlue;
    uint8_t backgroundRed, backgroundGreen, backgroundBlue;
    uint8_t underlineRed, underlineGreen, underlineBlue;
    uint8_t underlineStyle;
    uint8_t extendedAttributes;
    uint8_t font;
};

// mirrors: Source/Core/Cell.h Cell
struct BenchCell {
    uint32_t codePoint;
    BenchPen pen;
    uint32_t combining[1];
    uint8_t linkIDLo, linkIDHi;
    // One 16-bit word, not two bytes: production packs the combining length and
    // the cluster overflow id together here, so splitting them moves the link
    // ids and every store after them without changing the total.
    uint16_t clusterPacked;
};
static_assert(sizeof(BenchCell) == 32, "BenchCell must stay 32 bytes");

// --- Geometry (mirrors the live flood workload) -------------------------------

constexpr int ringRows = 49;
constexpr int ringColumns = 202;
constexpr int rowCells = 90; // trimmed line length copied / filled
constexpr int slabCells = 8192; // LineStore::blockCells
constexpr int slabCount = 1024; // 1024 x 256 KB = 256 MB destination pool
constexpr long operationsPerRun = 700000; // x 2880 B ~= 2.0 GB per repetition
constexpr int repetitions = 3;

static double secondsNow()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

// Defeats dead-store elimination across runs (slabs also escape via new).
static volatile uint8_t g_sink;

// --- Copy candidates -----------------------------------------------------------

using CopyFunction = void (*)(BenchCell*, const BenchCell*, int);

__attribute__((noinline)) static void copyLibcMemcpy(BenchCell* destination, const BenchCell* source, int count) { std::memcpy(destination, source, std::size_t(count) * sizeof(BenchCell)); }

__attribute__((noinline)) static void copyUint64Loop(BenchCell* destination, const BenchCell* source, int count)
{
    uint64_t* d = reinterpret_cast<uint64_t*>(destination);
    const uint64_t* s = reinterpret_cast<const uint64_t*>(source);
    const int words = count * 4;
    for (int i = 0; i < words; i += 2) {
        uint64_t a = s[i];
        uint64_t b = s[i + 1];
        d[i] = a;
        d[i + 1] = b;
    }
}

__attribute__((noinline)) static void copyMemcpyPrefetchDestination(BenchCell* destination, const BenchCell* source, int count)
{
    const std::size_t bytes = std::size_t(count) * sizeof(BenchCell);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    for (std::size_t offset = 0; offset < bytes; offset += 128) // Apple Silicon line = 128 B
        __builtin_prefetch(d + offset, 1, 0);
    std::memcpy(destination, source, bytes);
}

#if defined(__aarch64__)

// 16-byte general-register pairs: ldp/stp x-registers, 32 B (one Cell) per step.
__attribute__((noinline)) static void copyStpGeneralPairs(BenchCell* destination, const BenchCell* source, int count)
{
    const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    for (int i = 0; i < count; ++i) {
        uint64_t a, b, c, e;
        asm volatile("ldp %0, %1, [%4]\n\t"
                     "ldp %2, %3, [%4, #16]\n\t"
                     "stp %0, %1, [%5]\n\t"
                     "stp %2, %3, [%5, #16]"
            : "=&r"(a), "=&r"(b), "=&r"(c), "=&r"(e)
            : "r"(s), "r"(d)
            : "memory");
        s += 32;
        d += 32;
    }
}

// q-register pairs (stp q): 64 B per step, 32 B tail.
__attribute__((noinline)) static void copyStpVectorPairs(BenchCell* destination, const BenchCell* source, int count)
{
    const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        uint8x16_t a, b, c, e;
        asm volatile("ldp %q0, %q1, [%4]\n\t"
                     "ldp %q2, %q3, [%4, #32]\n\t"
                     "stp %q0, %q1, [%5]\n\t"
                     "stp %q2, %q3, [%5, #32]"
            : "=&w"(a), "=&w"(b), "=&w"(c), "=&w"(e)
            : "r"(s), "r"(d)
            : "memory");
        s += 64;
        d += 64;
    }
    if (i < count) {
        uint8x16_t a, b;
        asm volatile("ldp %q0, %q1, [%2]\n\t"
                     "stp %q0, %q1, [%3]"
            : "=&w"(a), "=&w"(b)
            : "r"(s), "r"(d)
            : "memory");
    }
}

// Non-temporal q-register pairs (stnp): 64 B per step, 32 B tail.
__attribute__((noinline)) static void copyStnpVectorPairs(BenchCell* destination, const BenchCell* source, int count)
{
    const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        uint8x16_t a, b, c, e;
        asm volatile("ldp %q0, %q1, [%4]\n\t"
                     "ldp %q2, %q3, [%4, #32]\n\t"
                     "stnp %q0, %q1, [%5]\n\t"
                     "stnp %q2, %q3, [%5, #32]"
            : "=&w"(a), "=&w"(b), "=&w"(c), "=&w"(e)
            : "r"(s), "r"(d)
            : "memory");
        s += 64;
        d += 64;
    }
    if (i < count) {
        uint8x16_t a, b;
        asm volatile("ldp %q0, %q1, [%2]\n\t"
                     "stnp %q0, %q1, [%3]"
            : "=&w"(a), "=&w"(b)
            : "r"(s), "r"(d)
            : "memory");
    }
}

// Non-temporal q-register pairs, 128 B (one Apple cache line) per step.
__attribute__((noinline)) static void copyStnpVectorPairs128(BenchCell* destination, const BenchCell* source, int count)
{
    const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        uint8x16_t a, b, c, e, f, g, h, j;
        asm volatile("ldp %q0, %q1, [%8]\n\t"
                     "ldp %q2, %q3, [%8, #32]\n\t"
                     "ldp %q4, %q5, [%8, #64]\n\t"
                     "ldp %q6, %q7, [%8, #96]\n\t"
                     "stnp %q0, %q1, [%9]\n\t"
                     "stnp %q2, %q3, [%9, #32]\n\t"
                     "stnp %q4, %q5, [%9, #64]\n\t"
                     "stnp %q6, %q7, [%9, #96]"
            : "=&w"(a), "=&w"(b), "=&w"(c), "=&w"(e), "=&w"(f), "=&w"(g), "=&w"(h), "=&w"(j)
            : "r"(s), "r"(d)
            : "memory");
        s += 128;
        d += 128;
    }
    for (; i < count; ++i) {
        uint8x16_t a, b;
        asm volatile("ldp %q0, %q1, [%2]\n\t"
                     "stnp %q0, %q1, [%3]"
            : "=&w"(a), "=&w"(b)
            : "r"(s), "r"(d)
            : "memory");
        s += 32;
        d += 32;
    }
}

// NEON intrinsics, 32 B (one Cell) per step via vld1q/vst1q pairs.
__attribute__((noinline)) static void copyNeonIntrinsics(BenchCell* destination, const BenchCell* source, int count)
{
    const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    for (int i = 0; i < count; ++i) {
        uint8x16_t a = vld1q_u8(s);
        uint8x16_t b = vld1q_u8(s + 16);
        vst1q_u8(d, a);
        vst1q_u8(d + 16, b);
        s += 32;
        d += 32;
    }
}

#endif // defined(__aarch64__)

// --- Fill candidates -----------------------------------------------------------

using FillFunction = void (*)(BenchCell*, const BenchCell&, int);

// The incumbent (Source/Core/CellGrid.cpp fillBlank): plain store loop up to
// 128 cells, memcpy-doubling above. At the realistic 90 cells this takes the
// plain-loop branch.
__attribute__((noinline)) static void fillIncumbent(BenchCell* destination, const BenchCell& blank, int count)
{
    if (count <= 0)
        return;
    if (count <= 128) {
        for (int i = 0; i < count; ++i)
            destination[i] = blank;
        return;
    }
    destination[0] = blank;
    int filled = 1;
    while (filled < count) {
        int chunk = filled < count - filled ? filled : count - filled;
        std::memcpy(destination + filled, destination, std::size_t(chunk) * sizeof(BenchCell));
        filled += chunk;
    }
}

__attribute__((noinline)) static void fillPlainLoop(BenchCell* destination, const BenchCell& blank, int count)
{
    for (int i = 0; i < count; ++i)
        destination[i] = blank;
}

// Portable candidate: same plain loop, but the pattern is copied by value to
// a local first. `const Cell& blank` may alias the destination, so the
// compiler must reload the pattern after every store; a local copy breaks the
// alias and lets the loads hoist out of the loop.
__attribute__((noinline)) static void fillPlainLoopLocalCopy(BenchCell* destination, const BenchCell& blank, int count)
{
    const BenchCell localBlank = blank;
    for (int i = 0; i < count; ++i)
        destination[i] = localBlank;
}

// memcpy-doubling with no plain-loop short-circuit (the >128 branch, forced).
__attribute__((noinline)) static void fillMemcpyDoubling(BenchCell* destination, const BenchCell& blank, int count)
{
    if (count <= 0)
        return;
    destination[0] = blank;
    int filled = 1;
    while (filled < count) {
        int chunk = filled < count - filled ? filled : count - filled;
        std::memcpy(destination + filled, destination, std::size_t(chunk) * sizeof(BenchCell));
        filled += chunk;
    }
}

#if defined(__aarch64__)

// Pattern pre-loaded into two q registers once; stp pair per Cell.
__attribute__((noinline)) static void fillStpVectorPairs(BenchCell* destination, const BenchCell& blank, int count)
{
    const uint8_t* pattern = reinterpret_cast<const uint8_t*>(&blank);
    uint8x16_t a = vld1q_u8(pattern);
    uint8x16_t b = vld1q_u8(pattern + 16);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        asm volatile("stp %q0, %q1, [%2]\n\t"
                     "stp %q0, %q1, [%2, #32]"
            :
            : "w"(a), "w"(b), "r"(d)
            : "memory");
        d += 64;
    }
    if (i < count)
        asm volatile("stp %q0, %q1, [%2]" : : "w"(a), "w"(b), "r"(d) : "memory");
}

// Same, with non-temporal stnp.
__attribute__((noinline)) static void fillStnpVectorPairs(BenchCell* destination, const BenchCell& blank, int count)
{
    const uint8_t* pattern = reinterpret_cast<const uint8_t*>(&blank);
    uint8x16_t a = vld1q_u8(pattern);
    uint8x16_t b = vld1q_u8(pattern + 16);
    uint8_t* d = reinterpret_cast<uint8_t*>(destination);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        asm volatile("stnp %q0, %q1, [%2]\n\t"
                     "stnp %q0, %q1, [%2, #32]"
            :
            : "w"(a), "w"(b), "r"(d)
            : "memory");
        d += 64;
    }
    if (i < count)
        asm volatile("stnp %q0, %q1, [%2]" : : "w"(a), "w"(b), "r"(d) : "memory");
}

#endif // defined(__aarch64__)

// --- Harness ---------------------------------------------------------------

struct CopyCandidate {
    const char* name;
    CopyFunction function;
};

struct FillCandidate {
    const char* name;
    FillFunction function;
};

static const CopyCandidate copyCandidates[] = {
    { "libc memcpy (incumbent)", copyLibcMemcpy },
    { "uint64 pair loop", copyUint64Loop },
    { "memcpy + dst prefetch", copyMemcpyPrefetchDestination },
#if defined(__aarch64__)
    { "ldp/stp x-pairs (16B)", copyStpGeneralPairs },
    { "ldp/stp q-pairs (32B)", copyStpVectorPairs },
    { "ldp/stnp q-pairs (NT)", copyStnpVectorPairs },
    { "ldp/stnp q x4 (NT 128B)", copyStnpVectorPairs128 },
    { "vld1q/vst1q NEON (32B)", copyNeonIntrinsics },
#endif
};

static const FillCandidate fillCandidates[] = {
    { "current fillBlank (incumbent)", fillIncumbent },
    { "plain store loop", fillPlainLoop },
    { "plain loop, blank by value", fillPlainLoopLocalCopy },
    { "memcpy doubling (forced)", fillMemcpyDoubling },
#if defined(__aarch64__)
    { "stp q-pairs, preloaded", fillStpVectorPairs },
    { "stnp q-pairs, preloaded", fillStnpVectorPairs },
#endif
};

constexpr int copyCandidateCount = int(sizeof(copyCandidates) / sizeof(copyCandidates[0]));
constexpr int fillCandidateCount = int(sizeof(fillCandidates) / sizeof(fillCandidates[0]));

// One run: operationsPerRun appends of rowCells cells from the L2-resident
// ring into the streaming slab pool. Returns GB/s of bytes WRITTEN (reads
// add another 1x; the ratio between candidates is what matters).
static double runCopy(CopyFunction function, BenchCell* ring, BenchCell** slabs)
{
    int slabIndex = 0;
    int used = 0;
    int ringRow = 0;
    const double start = secondsNow();
    for (long n = 0; n < operationsPerRun; ++n) {
        if (used + rowCells > slabCells) {
            used = 0;
            slabIndex = (slabIndex + 1) % slabCount;
        }
        function(slabs[slabIndex] + used, ring + std::size_t(ringRow) * ringColumns, rowCells);
        used += rowCells;
        ringRow = (ringRow + 1) % ringRows;
    }
    const double elapsed = secondsNow() - start;
    g_sink = reinterpret_cast<const uint8_t*>(slabs[slabIndex])[1];
    const double bytes = double(operationsPerRun) * rowCells * sizeof(BenchCell);
    return bytes / 1e9 / elapsed;
}

// One run: operationsPerRun fills of rowCells cells cycling through the live
// grid rows (cache-hot destination, like the erase/scroll-region paths).
static double runFill(FillFunction function, BenchCell* grid, const BenchCell& blank)
{
    int gridRow = 0;
    const double start = secondsNow();
    for (long n = 0; n < operationsPerRun; ++n) {
        function(grid + std::size_t(gridRow) * ringColumns, blank, rowCells);
        gridRow = (gridRow + 1) % ringRows;
    }
    const double elapsed = secondsNow() - start;
    g_sink = reinterpret_cast<const uint8_t*>(grid)[3];
    const double bytes = double(operationsPerRun) * rowCells * sizeof(BenchCell);
    return bytes / 1e9 / elapsed;
}

static double medianOfThree(double a, double b, double c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

int main()
{
    // Source ring + fill grid: 49 x 202 Cells (~317 KB each).
    const std::size_t ringTotal = std::size_t(ringRows) * ringColumns;
    BenchCell* ring = new BenchCell[ringTotal];
    BenchCell* grid = new BenchCell[ringTotal];
    srand(7);
    uint8_t* ringBytes = reinterpret_cast<uint8_t*>(ring);
    for (std::size_t i = 0; i < ringTotal * sizeof(BenchCell); ++i)
        ringBytes[i] = uint8_t(rand());
    std::memset(grid, 0, ringTotal * sizeof(BenchCell));

    // Destination slab pool, allocated like the real blocks (new Cell[8192]
    // each) and touched once so no candidate pays the page faults.
    BenchCell** slabs = new BenchCell*[slabCount];
    for (int i = 0; i < slabCount; ++i) {
        slabs[i] = new BenchCell[slabCells];
        std::memset(slabs[i], 0xAB, std::size_t(slabCells) * sizeof(BenchCell));
    }

    BenchCell blank = {};
    constexpr uint16_t defaultColorIndex = 256;
    blank.pen.foregroundIndex = defaultColorIndex;
    blank.pen.backgroundIndex = defaultColorIndex;
    blank.pen.underlineIndex = defaultColorIndex;

    // Correctness self-test: every candidate must produce memcpy / assignment
    // identical bytes.
    {
        BenchCell reference[rowCells];
        BenchCell scratch[rowCells];
        std::memcpy(reference, ring, sizeof(reference));
        for (const CopyCandidate& candidate : copyCandidates) {
            std::memset(scratch, 0x5C, sizeof(scratch));
            candidate.function(scratch, ring, rowCells);
            if (std::memcmp(scratch, reference, sizeof(reference)) != 0) {
                std::fprintf(stderr, "FAIL copy self-test: %s\n", candidate.name);
                return 1;
            }
        }
        for (int i = 0; i < rowCells; ++i)
            reference[i] = blank;
        for (const FillCandidate& candidate : fillCandidates) {
            std::memset(scratch, 0x5C, sizeof(scratch));
            candidate.function(scratch, blank, rowCells);
            if (std::memcmp(scratch, reference, sizeof(reference)) != 0) {
                std::fprintf(stderr, "FAIL fill self-test: %s\n", candidate.name);
                return 1;
            }
        }
        std::printf("self-test: all candidates byte-exact\n");
    }

    // Warmup (untimed) so caches/branch predictors settle.
    runCopy(copyLibcMemcpy, ring, slabs);
    runFill(fillIncumbent, grid, blank);

    // Timed: interleave candidates round-robin across repetitions.
    double copyResults[copyCandidateCount][repetitions];
    double fillResults[fillCandidateCount][repetitions];
    for (int rep = 0; rep < repetitions; ++rep) {
        for (int c = 0; c < copyCandidateCount; ++c)
            copyResults[c][rep] = runCopy(copyCandidates[c].function, ring, slabs);
        for (int c = 0; c < fillCandidateCount; ++c)
            fillResults[c][rep] = runFill(fillCandidates[c].function, grid, blank);
    }

    std::printf("\n%ld ops x %d cells x %zu B = %.2f GB written per run, %d reps, median GB/s\n", operationsPerRun, rowCells, sizeof(BenchCell), double(operationsPerRun) * rowCells * sizeof(BenchCell) / 1e9, repetitions);

    std::printf("\nCOPY (49x202 L2-resident source -> 256 MB streaming slab pool)\n");
    std::printf("%-32s %8s %8s %8s %10s %8s\n", "candidate", "rep1", "rep2", "rep3", "median", "vs inc");
    const double copyIncumbent = medianOfThree(copyResults[0][0], copyResults[0][1], copyResults[0][2]);
    for (int c = 0; c < copyCandidateCount; ++c) {
        const double median = medianOfThree(copyResults[c][0], copyResults[c][1], copyResults[c][2]);
        std::printf("%-32s %8.2f %8.2f %8.2f %10.2f %+7.1f%%\n", copyCandidates[c].name, copyResults[c][0], copyResults[c][1], copyResults[c][2], median, (median / copyIncumbent - 1.0) * 100.0);
    }

    std::printf("\nFILL (90 cells x 32 B pattern, cache-hot grid rows)\n");
    std::printf("%-32s %8s %8s %8s %10s %8s\n", "candidate", "rep1", "rep2", "rep3", "median", "vs inc");
    const double fillIncumbentMedian = medianOfThree(fillResults[0][0], fillResults[0][1], fillResults[0][2]);
    for (int c = 0; c < fillCandidateCount; ++c) {
        const double median = medianOfThree(fillResults[c][0], fillResults[c][1], fillResults[c][2]);
        std::printf("%-32s %8.2f %8.2f %8.2f %10.2f %+7.1f%%\n", fillCandidates[c].name, fillResults[c][0], fillResults[c][1], fillResults[c][2], median, (median / fillIncumbentMedian - 1.0) * 100.0);
    }

    return 0;
}
