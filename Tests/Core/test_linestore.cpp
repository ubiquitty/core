// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_linestore — LineStore block-arena scrollback store (Tier 2).
//
// Pins: trimmed append + round-trip, oldest-first indexing, capacity
// eviction (FIFO), block freeing once drained, dropOldest/dropNewest/clear,
// the arena being content-proportional (not capacity × full-width), and
// retired-slab recycling (steady-state flood reuses drained slabs without
// fresh allocations; clear/dropNewest release the parked slab).
//
// And the fold: a cold block folded into the row form reads back byte for
// byte in every cell field, the newest blocks stay raw, folded blocks evict
// and drop like raw ones, the decode cache turns over at its capacity, a
// held line survives another block's fold, a write unfolds, and a row the
// form cannot say keeps its block raw. Blank rows cannot make a fold outgrow
// its block: the evicted part of a span costs it nothing, a block whose live
// span is too long stays raw, and a refused slab leaves a block raw.

#include "Core/LineStore.h"
#include "Core/Screen/RowForm.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

using Ubiquitty::LineStore;
using Ubiquitty::StoredLine;
// Cell + maxColumns live at global scope (Cell.h); maxColumns is a macro.

// Slab-allocation probe: LineStore backs cells with fixed-size `new Cell[]`
// slabs, so counting array allocations at or above the slab size observes
// block churn directly. Mirrors LineStore's private blockCells constant.
static constexpr std::size_t slabBytes = static_cast<std::size_t>((Ubiquitty::maxColumns > 8192) ? Ubiquitty::maxColumns : 8192) * sizeof(Ubiquitty::Cell);
static std::size_t g_slabAllocationCount = 0;

void* operator new[](std::size_t size)
{
    if (size >= slabBytes)
        ++g_slabAllocationCount;
    void* pointer = std::malloc(size);
    if (!pointer)
        std::abort();
    return pointer;
}
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

// While set, a nothrow request of a slab's size is refused, as an exhausted
// heap refuses it.
static bool isRefusingNothrowSlabs = false;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (isRefusingNothrowSlabs && size >= slabBytes)
        return nullptr;
    return operator new[](size);
}

// The largest scalar allocation since a test cleared it. The vectors a fold
// fills ask the heap through scalar new, so this is the fold's largest request.
static std::size_t largestScalarAllocation = 0;

// Both scalar forms are replaced, nothrow too, so every scalar allocation is
// the malloc the replaced delete frees: a sanitizer's own nothrow new would
// otherwise pair with this file's delete.
static void* probedScalarAllocation(std::size_t size)
{
    if (size > largestScalarAllocation)
        largestScalarAllocation = size;
    return std::malloc(size ? size : 1);
}

void* operator new(std::size_t size)
{
    void* pointer = probedScalarAllocation(size);
    if (!pointer)
        std::abort();
    return pointer;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return probedScalarAllocation(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

// Build a row of `length` cells whose ch encodes (seed + column) so a
// round-trip read can verify exact content + ordering.
static void fillRow(Ubiquitty::Cell* row, int length, int seed)
{
    for (int c = 0; c < length; ++c) {
        row[c] = Ubiquitty::Cell {};
        row[c].codePoint = static_cast<uint32_t>(0x100 + seed * 1000 + c);
    }
}

TEST(testAppendRoundtrip)
{
    LineStore s;
    s.init(8);
    Ubiquitty::Cell row[64];
    fillRow(row, 10, 1);
    s.append(row, 10, /*lineAttribute*/ 2, /*mark*/ 4, /*wrapped*/ true, /*needsShaping*/ false);

    CHECK(s.count() == 1);
    const StoredLine* l = s.at(0);
    CHECK(l != nullptr);
    if (l) {
        CHECK(l->length == 10);
        CHECK(l->lineAttribute == 2);
        CHECK(l->mark == 4);
        CHECK(l->wrapped == true);
        for (int c = 0; c < 10; ++c)
            CHECK(l->cells[c].codePoint == static_cast<uint32_t>(0x100 + 1000 + c));
    }
    CHECK(s.at(1) == nullptr);
    CHECK(s.at(-1) == nullptr);
}

TEST(testOldestFirstOrder)
{
    LineStore s;
    s.init(8);
    Ubiquitty::Cell row[64];
    for (int i = 0; i < 5; ++i) {
        fillRow(row, 4, i);
        s.append(row, 4, 0, 0, false, false);
    }
    CHECK(s.count() == 5);
    // at(0) oldest (seed 0) … at(4) newest (seed 4).
    for (int i = 0; i < 5; ++i) {
        const StoredLine* l = s.at(i);
        CHECK(l && l->cells[0].codePoint == static_cast<uint32_t>(0x100 + i * 1000));
    }
}

TEST(testCapacityEviction)
{
    LineStore s;
    s.init(3);
    Ubiquitty::Cell row[64];
    for (int i = 0; i < 6; ++i) {
        fillRow(row, 4, i);
        s.append(row, 4, 0, 0, false, false);
    }
    // Only the newest 3 (seeds 3,4,5) survive, oldest-first.
    CHECK(s.count() == 3);
    for (int i = 0; i < 3; ++i) {
        const StoredLine* l = s.at(i);
        CHECK(l && l->cells[0].codePoint == static_cast<uint32_t>(0x100 + (i + 3) * 1000));
    }
}

TEST(testDropAndClear)
{
    LineStore s;
    s.init(8);
    Ubiquitty::Cell row[64];
    for (int i = 0; i < 6; ++i) {
        fillRow(row, 4, i);
        s.append(row, 4, 0, 0, false, false);
    }
    s.dropOldest(2);
    CHECK(s.count() == 4);
    const StoredLine* l = s.at(0);
    CHECK(l && l->cells[0].codePoint == static_cast<uint32_t>(0x100 + 2 * 1000));
    s.dropOldest(100); // clamp
    CHECK(s.count() == 0);
    fillRow(row, 4, 9);
    s.append(row, 4, 0, 0, false, false);
    s.clear();
    CHECK(s.count() == 0);
    CHECK(s.arenaBytes() == 0);
}

TEST(testDropNewest)
{
    // dropNewest peels lines off the back; the surviving prefix is the
    // oldest lines, in order. Re-appending after the drop reuses the slack.
    LineStore s;
    s.init(8);
    Ubiquitty::Cell row[64];
    for (int i = 0; i < 6; ++i) {
        fillRow(row, 4, i);
        s.append(row, 4, 0, 0, false, false);
    }
    s.dropNewest(2); // drop seeds 5,4
    CHECK(s.count() == 4);
    for (int i = 0; i < 4; ++i) {
        const StoredLine* l = s.at(i);
        CHECK(l && l->cells[0].codePoint == static_cast<uint32_t>(0x100 + i * 1000));
    }
    CHECK(s.at(4) == nullptr);
    // Re-append a fresh line: it lands as the new newest, content intact.
    fillRow(row, 4, 7);
    s.append(row, 4, 0, 0, false, false);
    CHECK(s.count() == 5);
    const StoredLine* l = s.at(4);
    CHECK(l && l->cells[0].codePoint == static_cast<uint32_t>(0x100 + 7 * 1000));
    s.dropNewest(100); // clamp to all
    CHECK(s.count() == 0);
    s.dropNewest(1); // no-op on empty
    CHECK(s.count() == 0);
}

TEST(testZeroLengthLine)
{
    LineStore s;
    s.init(4);
    Ubiquitty::Cell row[4];
    fillRow(row, 0, 0);
    s.append(row, 0, 1, 0, false, false); // blank scrolled-off row
    CHECK(s.count() == 1);
    const StoredLine* l = s.at(0);
    CHECK(l && l->length == 0 && l->lineAttribute == 1);
    CHECK(s.arenaBytes() == 0);
}

TEST(testZeroLengthLinesAreCellFree)
{
    LineStore s;
    s.init(4);
    Ubiquitty::Cell row[4];
    fillRow(row, 0, 0);
    for (int i = 0; i < 4; ++i)
        s.append(row, 0, 1, 0, false, false);
    CHECK(s.count() == 4);
    CHECK(s.arenaBytes() == 0);

    fillRow(row, 2, 9);
    s.append(row, 2, 0, 0, false, false); // evict one blank, store one real row
    CHECK(s.count() == 4);
    CHECK(s.arenaBytes() > 0);
    const StoredLine* newest = s.at(3);
    CHECK(newest && newest->length == 2);
    CHECK(newest && newest->cells[0].codePoint == static_cast<uint32_t>(0x100 + 9 * 1000));

    s.dropNewest(1); // drops the only cell-backed line
    CHECK(s.count() == 3);
    CHECK(s.arenaBytes() == 0);
    s.dropNewest(1); // dropping a blank should stay cell-free
    CHECK(s.count() == 2);
    CHECK(s.arenaBytes() == 0);
}

TEST(testArenaIsContentProportional)
{
    // 800 lines of 4 cells each must cost far less than 800 × maxColumns × 40.
    LineStore s;
    s.init(800);
    Ubiquitty::Cell row[64];
    for (int i = 0; i < 800; ++i) {
        fillRow(row, 4, i);
        s.append(row, 4, 0, 0, false, false);
    }
    const std::size_t flatArena = static_cast<std::size_t>(800) * Ubiquitty::maxColumns * sizeof(Ubiquitty::Cell);
    CHECK(s.arenaBytes() < flatArena / 4); // dramatically smaller
}

// Fixed-length rows sized so one slab backs exactly `capacity` lines, making
// block park/reuse phase deterministic: with capacity 128 and 64-cell rows,
// appends 0..255 leave one full back block plus one parked retired slab.
static constexpr int recycleCapacity = 128;
static constexpr int recycleRowLength = 64;

static void appendSeededRow(LineStore& s, int seed)
{
    Ubiquitty::Cell row[recycleRowLength];
    fillRow(row, recycleRowLength, seed);
    s.append(row, recycleRowLength, 0, 0, false, false);
}

// Verify every cell of every live line after `totalAppended` fixed-length
// appends; recycled slabs must never leak stale cells into new lines.
static void checkSeededWindow(LineStore& s, int totalAppended)
{
    CHECK(s.count() == recycleCapacity);
    const int oldestSeed = totalAppended - recycleCapacity;
    for (int i = 0; i < recycleCapacity; ++i) {
        const StoredLine* l = s.at(i);
        CHECK(l && l->length == recycleRowLength);
        if (!l || l->length != recycleRowLength)
            return;
        const int seed = oldestSeed + i;
        for (int c = 0; c < recycleRowLength; ++c)
            CHECK(l->cells[c].codePoint == static_cast<uint32_t>(0x100 + seed * 1000 + c));
    }
}

// Front evictions and a back rollback meeting in one block. linesAppended
// counts every line the block ever backed, so a block serving as front and
// back at once frees only when the lines remaining are the evicted ones —
// held against zero it kept the evicted tally forever, and dropNewest's
// drained-store reset then orphaned the slab for the store's life.
TEST(testFrontEvictionsThenDropNewestFreeTheSharedBlock)
{
    LineStore s;
    s.init(4); // ring of four: the fifth append evicts the oldest
    Ubiquitty::Cell row[16];
    for (int i = 0; i < 6; ++i) {
        fillRow(row, 8, i);
        s.append(row, 8, 0, 0, false, false); // appends 5 and 6 evict from the front
    }
    CHECK(s.count() == 4);
    CHECK(s.arenaBytes() > 0);

    s.dropNewest(s.count());
    CHECK(s.count() == 0);
    CHECK_MSG(s.arenaBytes() == 0, "a drained store still held a slab whose lines were all evicted or dropped");

    // The store keeps working after the drain.
    fillRow(row, 8, 42);
    s.append(row, 8, 0, 0, false, false);
    CHECK(s.count() == 1);
    const StoredLine* line = s.at(0);
    CHECK(line != nullptr && line->length == 8);
}

TEST(testPartialDropAfterEvictionsKeepsSurvivorsReadable)
{
    // The hazard the accounting change carries is freeing EARLY: a slab
    // released while a descriptor still points into it. Survivors are read
    // back cell by cell after the drop, so an early free is a corrupted
    // payload here rather than a latent pointer.
    LineStore s;
    s.init(4);
    Ubiquitty::Cell row[16];
    for (int i = 0; i < 6; ++i) {
        fillRow(row, 8, i);
        s.append(row, 8, 0, 0, false, false);
    }
    s.dropNewest(2); // lines 4 and 5 go; lines 2 and 3 remain
    CHECK(s.count() == 2);
    for (int i = 0; i < 2; ++i) {
        const StoredLine* line = s.at(i);
        CHECK(line != nullptr && line->length == 8);
        if (line)
            for (int c = 0; c < 8; ++c)
                CHECK(line->cells[c].codePoint == static_cast<uint32_t>(0x100 + (i + 2) * 1000 + c));
    }

    s.dropNewest(2);
    CHECK(s.count() == 0);
    CHECK_MSG(s.arenaBytes() == 0, "the drained store held a slab");
}

TEST(testRingFullRecyclesSlabs)
{
    LineStore s;
    s.init(recycleCapacity);
    int appended = 0;
    for (; appended < 2 * recycleCapacity; ++appended)
        appendSeededRow(s, appended);
    // Steady state: each block cycle parks the drained front slab and the
    // next back block reuses it — zero fresh slab allocations from here on.
    g_slabAllocationCount = 0;
    for (; appended < 8 * recycleCapacity; ++appended)
        appendSeededRow(s, appended);
    CHECK(g_slabAllocationCount == 0);
    checkSeededWindow(s, appended);
}

TEST(testClearFreesRetiredSlab)
{
    LineStore s;
    s.init(recycleCapacity);
    int appended = 0;
    for (; appended < 2 * recycleCapacity; ++appended)
        appendSeededRow(s, appended);
    // A retired slab is parked here; clear must free it along with the blocks.
    s.clear();
    CHECK(s.count() == 0);
    CHECK(s.arenaBytes() == 0);
    g_slabAllocationCount = 0;
    appendSeededRow(s, 0);
    CHECK(g_slabAllocationCount == 1); // fresh slab: nothing was left parked
    const StoredLine* l = s.at(0);
    CHECK(l && l->cells[0].codePoint == 0x100);
    for (appended = 1; appended < 3 * recycleCapacity; ++appended)
        appendSeededRow(s, appended);
    checkSeededWindow(s, appended);
}

TEST(testDropNewestFreesRetiredSlab)
{
    LineStore s;
    s.init(recycleCapacity);
    int appended = 0;
    for (; appended < 2 * recycleCapacity; ++appended)
        appendSeededRow(s, appended);
    // A retired slab is parked here; dropNewest must free it (shrinking is
    // the idle direction), so the next block cycle allocates fresh.
    g_slabAllocationCount = 0;
    s.dropNewest(1);
    CHECK(s.count() == recycleCapacity - 1);
    appendSeededRow(s, appended - 1); // refills the back-block slack
    appendSeededRow(s, appended); // evicts + needs a new block
    ++appended;
    CHECK(g_slabAllocationCount == 1);
    checkSeededWindow(s, appended);
}

// ---------------------------------------------------------------------------
// Folding
// ---------------------------------------------------------------------------

// xorshift64*: deterministic, so a failing seed replays.
struct Random {
    std::uint64_t state;
    // The pens a seed's cells draw from. A block names a few dozen pens, as
    // real output does, and one naming more than `maxPensPerFoldedBlock`
    // stays raw by policy; every byte of each pen here is random all the same.
    std::vector<Ubiquitty::Pen> palette;
    std::uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    std::uint32_t below(std::uint32_t bound) { return static_cast<std::uint32_t>(next() % bound); }
    std::uint8_t byte() { return static_cast<std::uint8_t>(next() & 0xFF); }
    std::uint16_t halfword() { return static_cast<std::uint16_t>(next() & 0xFFFF); }
};

// Every byte of a pen drawn at random: the attributes, the three colour
// indices, nine colour bytes, the underline style byte, the extended
// attributes and the font byte.
static Ubiquitty::Pen randomPen(Random& random)
{
    Ubiquitty::Pen pen {};
    pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(random.halfword());
    pen.foregroundIndex = random.halfword();
    pen.backgroundIndex = random.halfword();
    pen.underlineIndex = random.halfword();
    pen.foregroundRed = random.byte();
    pen.foregroundGreen = random.byte();
    pen.foregroundBlue = random.byte();
    pen.backgroundRed = random.byte();
    pen.backgroundGreen = random.byte();
    pen.backgroundBlue = random.byte();
    pen.underlineRed = random.byte();
    pen.underlineGreen = random.byte();
    pen.underlineBlue = random.byte();
    pen.underlineStyle = random.byte();
    pen.extendedAttributes = Ubiquitty::VT::ExtendedAttributes::fromRaw(random.byte());
    pen.font = random.byte();
    return pen;
}

static Random seededRandom(std::uint64_t seed)
{
    Random random { seed, {} };
    for (int index = 0; index < 48; ++index)
        random.palette.push_back(randomPen(random));
    return random;
}

// A scalar value from every UTF-8 length, never a surrogate.
static std::uint32_t randomScalar(Random& random)
{
    switch (random.below(5)) {
    case 0:
        return 0x20 + random.below(0x5F);
    case 1:
        return 0x80 + random.below(0x780);
    case 2: {
        std::uint32_t value = 0x800 + random.below(0xF800);
        if (value >= 0xD800 && value <= 0xDFFF)
            value = 0xE000 + (value - 0xD800);
        return value;
    }
    case 3:
        return 0x10000 + random.below(0x100000);
    default:
        return 0x4E00 + random.below(0x51A6);
    }
}

// Every field a cell has, drawn at random: the code point (sometimes the
// unwritten 0 under a pen), a pen from the seed's palette, the inline
// combining slot, the link id and the cluster id.
static Ubiquitty::Cell randomCell(Random& random)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = random.below(8) == 0 ? 0 : randomScalar(random);
    cell.pen = random.palette[random.below(static_cast<std::uint32_t>(random.palette.size()))];
    if (random.below(2) == 0) {
        cell.combining[0] = randomScalar(random);
        cell.setCombiningLength(1);
    }
    if (random.below(2) == 0)
        cell.setLinkID(static_cast<std::uint16_t>(1 + random.below(0xFFFF)));
    if (random.below(2) == 0)
        cell.setClusterID(static_cast<std::uint16_t>(1 + random.below(0x7FFF)));
    return cell;
}

// A row of random cells: half the wide leads get the continuation the Core
// writes (one unit in the form), the rest keep whatever partner they drew
// (`wideKept`), and some rows end in canonical blanks the form drops.
static std::vector<Ubiquitty::Cell> randomRow(Random& random, int length)
{
    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(length), Ubiquitty::defaultBlankCell);
    const int trailingBlanks = random.below(4) == 0 ? static_cast<int>(random.below(static_cast<std::uint32_t>(length) + 1)) : 0;
    const int content = length - trailingBlanks;
    for (int column = 0; column < content; ++column) {
        row[static_cast<std::size_t>(column)] = randomCell(random);
        if (row[static_cast<std::size_t>(column)].pen.attributes.contains(Ubiquitty::VT::Attribute::Wide) && column + 1 < content && random.below(2) == 0) {
            row[static_cast<std::size_t>(column) + 1] = Ubiquitty::Screen::derivedContinuation(row[static_cast<std::size_t>(column)]);
            ++column;
        }
    }
    return row;
}

struct SeededLine {
    std::vector<Ubiquitty::Cell> cells;
    uint8_t lineAttribute;
    uint8_t mark;
    bool isWrapped;
    bool needsShaping;
};

static SeededLine appendRandomLine(LineStore& s, Random& random, int maxLength)
{
    const int length = random.below(6) == 0 ? 0 : static_cast<int>(random.below(static_cast<std::uint32_t>(maxLength) + 1));
    SeededLine line { randomRow(random, length), random.byte(), random.byte(), random.below(2) == 0, random.below(2) == 0 };
    // A blank row still hands the store a row pointer, as the capture does.
    const Ubiquitty::Cell* cells = line.cells.empty() ? &Ubiquitty::defaultBlankCell : line.cells.data();
    s.append(cells, length, line.lineAttribute, line.mark, line.isWrapped, line.needsShaping);
    return line;
}

static bool lineMatches(const StoredLine* stored, const SeededLine& expected)
{
    if (!stored || stored->length != static_cast<int>(expected.cells.size()) || stored->wrapped != expected.isWrapped
        || stored->lineAttribute != expected.lineAttribute || stored->mark != expected.mark || stored->needsShaping != expected.needsShaping)
        return false;
    if (stored->length == 0)
        return stored->cells == nullptr;
    return stored->cells && std::memcmp(stored->cells, expected.cells.data(), expected.cells.size() * sizeof(Ubiquitty::Cell)) == 0;
}

static void foldEverythingCold(LineStore& s)
{
    while (s.foldColdBlocks(1)) {
    }
}

static int firstLineWithCells(const std::vector<SeededLine>& lines)
{
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].cells.empty())
            return static_cast<int>(i);
    }
    return -1;
}

static int lastLineWithCells(const std::vector<SeededLine>& lines)
{
    for (std::size_t i = lines.size(); i > 0; --i) {
        if (!lines[i - 1].cells.empty())
            return static_cast<int>(i - 1);
    }
    return -1;
}

TEST(testFoldRoundTripsEveryCellField)
{
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        Random random = seededRandom(seed * 0x9E3779B97F4A7C15ull);
        LineStore s;
        s.init(4000);
        std::vector<SeededLine> lines;
        while (s.blockCount() < 6)
            lines.push_back(appendRandomLine(s, random, 160));
        const std::size_t rawBytes = s.arenaBytes();
        foldEverythingCold(s);
        CHECK_EQ(s.foldedBlockCount(), s.blockCount() - LineStore::rawBlockWindow);
        CHECK(s.arenaBytes() < rawBytes);
        CHECK(s.foldedBytes() > 0);
        int mismatches = 0;
        for (std::size_t i = 0; i < lines.size(); ++i)
            mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
        CHECK_MSG(mismatches == 0, "a line read from a folded block differs from the line appended");
        CHECK_EQ(s.count(), static_cast<int>(lines.size()));
    }
}

TEST(testRawBlockWindowStaysRaw)
{
    Random random = seededRandom(7);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 5)
        lines.push_back(appendRandomLine(s, random, 160));
    const int oldest = firstLineWithCells(lines);
    const int newest = lastLineWithCells(lines);
    const Ubiquitty::Cell* newestCells = s.at(newest)->cells;
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 3);
    // The back block is the append cursor: its lines keep their slab.
    CHECK(s.at(newest)->cells == newestCells);
    // The front block folded: its lines are read from a decode. The decode's
    // address proves nothing either way, since the allocator may hand it the
    // slab the fold just released; the count of decoded blocks is the fact.
    CHECK_EQ(s.decodedBlockCount(), 0);
    CHECK(s.at(oldest) != nullptr);
    CHECK_EQ(s.decodedBlockCount(), 1);
    CHECK(lineMatches(s.at(oldest), lines[static_cast<std::size_t>(oldest)]));
    // Nothing cold is left to fold, and asking again folds nothing.
    CHECK(!s.foldColdBlocks(1));
    CHECK_EQ(s.foldedBlockCount(), 3);
}

TEST(testFoldedBlocksEvictWithTheirLines)
{
    Random random = seededRandom(11);
    LineStore s;
    s.init(400);
    std::vector<SeededLine> lines;
    for (int i = 0; i < 400; ++i)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    CHECK(s.foldedBlockCount() >= 1);
    for (int i = 0; i < 400; ++i)
        lines.push_back(appendRandomLine(s, random, 160));
    // Every folded block backed only evicted lines by now and went with them.
    CHECK_EQ(s.foldedBlockCount(), 0);
    CHECK_EQ(s.decodedBlockCount(), 0);
    CHECK_EQ(s.foldedBytes(), 0u);
    int mismatches = 0;
    for (int i = 0; i < 400; ++i)
        mismatches += lineMatches(s.at(i), lines[static_cast<std::size_t>(400 + i)]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
}

TEST(testDecodeCacheHoldsItsCapacity)
{
    Random random = seededRandom(13);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 8)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 6);
    // A walk over every line decodes each folded block once; the cache holds
    // the most recent four and every line still reads exactly.
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(s.decodedBlockCount(), LineStore::decodedBlockCacheCapacity);
    CHECK_EQ(s.decodedCacheBytes(), static_cast<std::size_t>(LineStore::decodedBlockCacheCapacity) * slabBytes);
    // The front block's decode was dropped by the walk; reading it again decodes it again.
    const int oldest = firstLineWithCells(lines);
    CHECK(lineMatches(s.at(oldest), lines[static_cast<std::size_t>(oldest)]));
    CHECK_EQ(s.decodedBlockCount(), LineStore::decodedBlockCacheCapacity);
}

TEST(testFoldLeavesHeldLinesIntact)
{
    Random random = seededRandom(17);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 5)
        lines.push_back(appendRandomLine(s, random, 160));
    CHECK(s.foldColdBlocks(1)); // the front block folds; two cold ones remain
    const int oldest = firstLineWithCells(lines);
    const int newest = lastLineWithCells(lines);
    const StoredLine* held = s.at(oldest);
    const Ubiquitty::Cell* heldCells = held->cells;
    const std::vector<Ubiquitty::Cell> heldCopy(heldCells, heldCells + held->length);
    const Ubiquitty::Cell* newestCells = s.at(newest)->cells;
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 3);
    // A fold touches neither the decode a reader holds nor a raw block's slab.
    CHECK(s.at(oldest)->cells == heldCells);
    CHECK(std::memcmp(heldCells, heldCopy.data(), heldCopy.size() * sizeof(Ubiquitty::Cell)) == 0);
    CHECK(s.at(newest)->cells == newestCells);
    CHECK(lineMatches(s.at(newest), lines[static_cast<std::size_t>(newest)]));
}

TEST(testDropNewestReachesIntoAFoldedBlock)
{
    Random random = seededRandom(19);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 5)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 3);
    // Peel the two raw blocks off the back, then five lines of the folded one.
    while (s.blockCount() > 3) {
        s.dropNewest(1);
        lines.pop_back();
    }
    for (int i = 0; i < 5; ++i) {
        s.dropNewest(1);
        lines.pop_back();
    }
    CHECK_EQ(s.blockCount(), 3);
    CHECK_EQ(s.foldedBlockCount(), 3);
    CHECK_EQ(s.count(), static_cast<int>(lines.size()));
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
    // A folded back block takes no append: the next line opens a raw block.
    lines.push_back(appendRandomLine(s, random, 160));
    while (lines.back().cells.empty())
        lines.push_back(appendRandomLine(s, random, 160));
    CHECK_EQ(s.blockCount(), 4);
    CHECK_EQ(s.foldedBlockCount(), 3);
    mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
}

TEST(testWritableAtUnfoldsTheBlock)
{
    Random random = seededRandom(23);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 7)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    const int folded = s.foldedBlockCount();
    CHECK_EQ(folded, 5);
    const int oldest = firstLineWithCells(lines);
    StoredLine* writable = s.writableAt(oldest);
    CHECK(writable && writable->cells);
    writable->cells[0].codePoint = 0x5A;
    lines[static_cast<std::size_t>(oldest)].cells[0].codePoint = 0x5A;
    CHECK_EQ(s.foldedBlockCount(), folded - 1);
    // Churn the decode cache through every other folded block: the write
    // landed in the store's own slab, so it is still there.
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(s.at(oldest)->cells[0].codePoint, 0x5Au);
    // The unfolded block is cold again and folds on the next pass, with the write.
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), folded);
    CHECK(lineMatches(s.at(oldest), lines[static_cast<std::size_t>(oldest)]));
}

TEST(testFoldRefusesARowItCannotSayExactly)
{
    Random random = seededRandom(29);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    // The form carries text as UTF-8, which has no spelling for a lone surrogate.
    SeededLine unsayable { randomRow(random, 12), 0, 0, false, false };
    unsayable.cells[3].codePoint = 0xD800;
    s.append(unsayable.cells.data(), 12, 0, 0, false, false);
    lines.push_back(unsayable);
    while (s.blockCount() < 5)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    // The front block stays raw and is not offered again; the other cold ones fold.
    CHECK_EQ(s.foldedBlockCount(), 2);
    CHECK(!s.foldColdBlocks(1));
    CHECK_EQ(s.foldedBlockCount(), 2);
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
}

TEST(testFoldRefusesABlockNamingTooManyPens)
{
    Random random = seededRandom(31);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    // Every cell of the first block in a pen of its own: more pens than a fold
    // takes, so the block stays raw and the policy says so.
    while (s.blockCount() < 2) {
        std::vector<Ubiquitty::Cell> row(80, Ubiquitty::defaultBlankCell);
        for (std::size_t column = 0; column < row.size(); ++column) {
            row[column] = randomCell(random);
            row[column].pen = randomPen(random);
        }
        SeededLine line { row, 0, 0, false, false };
        s.append(line.cells.data(), 80, 0, 0, false, false);
        lines.push_back(line);
    }
    while (s.blockCount() < 5)
        lines.push_back(appendRandomLine(s, random, 160));
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 2);
    CHECK(!s.foldColdBlocks(1));
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
}

TEST(testBlankLinesNeedNoFold)
{
    LineStore s;
    s.init(400);
    Ubiquitty::Cell row[4];
    fillRow(row, 0, 0);
    for (int i = 0; i < 300; ++i)
        s.append(row, 0, 1, 0, false, false);
    // A blank line owns no cells and opens no block.
    CHECK_EQ(s.blockCount(), 0);
    CHECK_EQ(s.arenaBytes(), 0u);
    CHECK(!s.foldColdBlocks(1));
    CHECK_EQ(s.foldedBlockCount(), 0);
    const StoredLine* l = s.at(299);
    CHECK(l && l->length == 0 && l->cells == nullptr && l->lineAttribute == 1);
}

// Appends full-width rows until the store holds three blocks, so the front
// one is cold.
static std::vector<std::vector<Ubiquitty::Cell>> appendRowsUntilThreeBlocks(LineStore& s)
{
    std::vector<std::vector<Ubiquitty::Cell>> rows;
    while (s.blockCount() < 3) {
        std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(Ubiquitty::maxColumns));
        fillRow(row.data(), Ubiquitty::maxColumns, 100 + static_cast<int>(rows.size()));
        s.append(row.data(), Ubiquitty::maxColumns, 0, 0, false, false);
        rows.push_back(std::move(row));
    }
    return rows;
}

// Whether the newest rows of the store are these, oldest first.
static bool newestRowsMatch(LineStore& s, const std::vector<std::vector<Ubiquitty::Cell>>& rows)
{
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const StoredLine* stored = s.at(s.count() - static_cast<int>(rows.size() - i));
        if (!stored || !stored->cells || stored->length != static_cast<int>(rows[i].size())
            || std::memcmp(stored->cells, rows[i].data(), rows[i].size() * sizeof(Ubiquitty::Cell)) != 0)
            return false;
    }
    return true;
}

TEST(testFoldSkipsTheEvictedSpanOfTheFrontBlock)
{
    // A one-cell line, then blank rows until the line before it is evicted,
    // over and over: the newest line always holds the block, so one block
    // backs every line, and a blank row takes an ordinal and no cell, so its
    // span reaches thousands of capacities. Full-width rows then make it cold
    // with its newest line still held.
    constexpr int capacity = 1000;
    constexpr int rounds = 4096;
    constexpr int blanksAfterNewest = 8;
    LineStore s;
    s.init(capacity);
    Ubiquitty::Cell line[1];
    Ubiquitty::Cell blank[1];
    fillRow(blank, 1, 0);
    for (int round = 0; round < rounds; ++round) {
        fillRow(line, 1, round % 32);
        s.append(line, 1, 0, 0, false, false);
        const int blanks = round + 1 < rounds ? capacity - 2 : blanksAfterNewest;
        for (int i = 0; i < blanks; ++i)
            s.append(blank, 0, 0, 0, false, false);
    }
    CHECK_EQ(s.blockCount(), 1);
    const auto rows = appendRowsUntilThreeBlocks(s);
    const std::size_t rawBytes = s.arenaBytes();

    largestScalarAllocation = 0;
    CHECK(!s.foldColdBlocks(1));
    // The fold names only the rows the store holds: no request near the four
    // million rows of the span, and a folded block smaller than its slab.
    CHECK_EQ(s.foldedBlockCount(), 1);
    CHECK_MSG(largestScalarAllocation < slabBytes, "a fold asked for more than a slab");
    CHECK_MSG(s.arenaBytes() < rawBytes, "a folded block takes more than the slab it replaced");

    const int newest = s.count() - static_cast<int>(rows.size()) - blanksAfterNewest - 1;
    fillRow(line, 1, (rounds - 1) % 32);
    const StoredLine* stored = s.at(newest);
    CHECK(stored && stored->length == 1 && stored->cells && stored->cells[0].codePoint == line[0].codePoint);
    const StoredLine* after = s.at(newest + 1);
    CHECK(after && after->length == 0 && after->cells == nullptr);
    CHECK(newestRowsMatch(s, rows));
    CHECK_EQ(s.count(), capacity);
}

TEST(testFoldRefusesABlockSpanningTooManyRows)
{
    // Blank rows the store still holds lengthen a span past what a fold takes.
    const int capacity = static_cast<int>(LineStore::maxFoldSpan) + 64;
    LineStore s;
    s.init(capacity);
    Ubiquitty::Cell line[1];
    fillRow(line, 1, 7);
    s.append(line, 1, 0, 0, false, false);
    Ubiquitty::Cell blank[1];
    fillRow(blank, 1, 0);
    for (std::uint64_t i = 0; i < LineStore::maxFoldSpan; ++i)
        s.append(blank, 0, 0, 0, false, false);
    const auto rows = appendRowsUntilThreeBlocks(s);
    const std::size_t rawBytes = s.arenaBytes();

    // The block stays raw and is not offered again.
    CHECK(!s.foldColdBlocks(1));
    CHECK_EQ(s.foldedBlockCount(), 0);
    CHECK(!s.foldColdBlocks(1));
    CHECK_EQ(s.arenaBytes(), rawBytes);

    const StoredLine* stored = s.at(0);
    CHECK(stored && stored->length == 1 && stored->cells && stored->cells[0].codePoint == line[0].codePoint);
    const StoredLine* after = s.at(1);
    CHECK(after && after->length == 0 && after->cells == nullptr);
    CHECK(newestRowsMatch(s, rows));
}

TEST(testFoldLeavesItsBlockRawWhenItsSlabIsRefused)
{
    Random random = seededRandom(37);
    LineStore s;
    s.init(4000);
    std::vector<SeededLine> lines;
    while (s.blockCount() < 3)
        lines.push_back(appendRandomLine(s, random, 160));
    // Nothing was evicted, so no drained slab is parked for the fold's check:
    // it must ask the heap, which refuses.
    isRefusingNothrowSlabs = true;
    (void)s.foldColdBlocks(1);
    isRefusingNothrowSlabs = false;
    CHECK_EQ(s.foldedBlockCount(), 0);
    int mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
    // The block is offered again, and folds once the heap gives a slab.
    CHECK(s.foldColdBlocks(0));
    foldEverythingCold(s);
    CHECK_EQ(s.foldedBlockCount(), 1);
    mismatches = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mismatches += lineMatches(s.at(static_cast<int>(i)), lines[i]) ? 0 : 1;
    CHECK_EQ(mismatches, 0);
}

TEST(aRestoredHistoryKeepsEvictionPullbackAndAppendCounters)
{
    LineStore source;
    source.init(1000);
    Ubiquitty::Cell row[64];
    for (int index = 0; index < 5000; ++index) {
        fillRow(row, 64, index % 500);
        source.append(row, 64, 0, 0, index % 3 != 2, false);
    }
    source.dropNewest(5);
    while (source.foldColdBlocks(1)) {
    }
    CHECK(source.foldedBlockCount() > LineStore::decodedBlockCacheCapacity);
    const auto continuation = source.continuation();
    std::vector<std::vector<Ubiquitty::Cell>> cells;
    std::vector<StoredLine> rows;
    for (int index = 0; index < source.count(); ++index) {
        const auto& stored = *source.at(index);
        cells.emplace_back(stored.cells, stored.cells + stored.length);
        rows.push_back(stored);
        rows.back().cells = cells.back().data();
    }
    LineStore restored;
    restored.init(1000);
    REQUIRE(restored.restore(rows, continuation));
    CHECK_EQ(restored.totalAppended(), source.totalAppended());
    CHECK_EQ(restored.frontLineNumber(), source.frontLineNumber());
    CHECK_EQ(restored.headLineNumber(), source.headLineNumber());
    CHECK_EQ(restored.maxHeadLineNumber(), source.maxHeadLineNumber());
    CHECK(restored.frontLine().continuity == source.frontLine().continuity);
    for (int index = 0; index < source.count(); ++index) {
        const auto& before = *source.at(index);
        const auto& after = *restored.at(index);
        REQUIRE(after.length == before.length);
        CHECK(after.wrapped == before.wrapped);
        CHECK(std::memcmp(after.cells, before.cells, static_cast<std::size_t>(after.length) * sizeof(Ubiquitty::Cell)) == 0);
    }
    for (std::uint64_t number = source.frontLineNumber(); number < source.headLineNumber(); ++number) {
        const auto before = source.logicalLine(number);
        const auto after = restored.logicalLine(number);
        REQUIRE(before && after);
        CHECK_EQ(after->firstIndex, before->firstIndex);
        CHECK_EQ(after->rowCount, before->rowCount);
    }
    for (int index = 0; index < 8; ++index) {
        source.append(row, 4, 0, 0, false, false);
        restored.append(row, 4, 0, 0, false, false);
    }
    CHECK_EQ(restored.totalAppended(), source.totalAppended());
    CHECK_EQ(restored.headLineNumber(), source.headLineNumber());
    CHECK_EQ(restored.maxHeadLineNumber(), source.maxHeadLineNumber());
}

TEST(aHistoryRestoreValidatesEveryRowAndCounterBeforeMutation)
{
    LineStore restored;
    restored.init(2);
    Ubiquitty::Cell cell {};
    const std::array<StoredLine, 2> rows { StoredLine { &cell, 1, true, 0, 0, false, false }, StoredLine { nullptr, 0, false, 2, 3, true, false } };
    const LineStore::Continuation valid { .front = { 20, LineStore::LineContinuity::ContinuesLine }, .head = 21, .maxHead = 30, .totalAppended = 50 };
    for (int fault = 0; fault < 7; ++fault) {
        auto changed = valid;
        auto damaged = rows;
        switch (fault) {
        case 0:
            changed.totalAppended = 1;
            break;
        case 1:
            changed.head = 19;
            break;
        case 2:
            changed.head = 22;
            break;
        case 3:
            changed.maxHead = 20;
            break;
        case 4:
            damaged[1].length = Ubiquitty::maxColumns + 1;
            break;
        case 5:
            damaged[1].length = 1;
            break;
        case 6:
            changed.front.continuity = static_cast<LineStore::LineContinuity>(99);
            break;
        }
        CHECK(!restored.restore(damaged, changed));
        CHECK_EQ(restored.count(), 0);
        CHECK_EQ(restored.totalAppended(), 0);
        CHECK_EQ(restored.arenaBytes(), 0u);
    }
    REQUIRE(restored.restore(rows, valid));
    CHECK_EQ(restored.totalAppended(), 50);
    CHECK_EQ(restored.maxHeadLineNumber(), 30u);
    CHECK_EQ(restored.at(1)->length, 0);
    CHECK_EQ(restored.at(1)->lineAttribute, 2);
    CHECK(restored.at(1)->needsShaping);
    CHECK(!restored.restore(rows, valid));
    CHECK_EQ(restored.count(), 2);
}

TEST(anEmptyHistoryRetainsItsOpenFrontAndHighWater)
{
    LineStore source;
    source.init(2);
    Ubiquitty::Cell cell {};
    source.append(&cell, 1, 0, 0, false, false);
    source.append(&cell, 1, 0, 0, true, false);
    source.clear();
    LineStore restored;
    restored.init(2);
    REQUIRE(restored.restore({}, source.continuation()));
    restored.append(&cell, 1, 0, 0, false, false);
    source.append(&cell, 1, 0, 0, false, false);
    CHECK_EQ(restored.totalAppended(), source.totalAppended());
    CHECK_EQ(restored.frontLineNumber(), source.frontLineNumber());
    CHECK_EQ(restored.headLineNumber(), source.headLineNumber());
    CHECK(restored.frontLine().continuity == source.frontLine().continuity);
}

TEST(aRestoredHistoryCannotOverflowItsCounters)
{
    LineStore store;
    store.init(3);
    Ubiquitty::Cell cell {};
    const StoredLine row { &cell, 1, false, 0, 0, false, false };
    const auto maxLine = std::numeric_limits<std::uint64_t>::max();
    const auto maxAppends = std::numeric_limits<long long>::max();
    REQUIRE(store.restore({ &row, 1 }, { .front = { maxLine - 2, LineStore::LineContinuity::StartsLine }, .head = maxLine - 1, .maxHead = maxLine - 1, .totalAppended = maxAppends - 1 }));
    store.append(&cell, 1, 0, 0, false, false);
    CHECK_EQ(store.count(), 2);
    CHECK_EQ(store.totalAppended(), maxAppends);
    CHECK_EQ(store.headLineNumber(), maxLine);
    const auto first = *store.at(0)->cells;
    cell.codePoint = 'x';
    store.append(&cell, 1, 0, 0, false, false);
    CHECK_EQ(store.count(), 2);
    CHECK_EQ(store.totalAppended(), maxAppends);
    CHECK_EQ(store.headLineNumber(), maxLine);
    CHECK(std::memcmp(store.at(0)->cells, &first, sizeof(first)) == 0);
}

TEST(logicalPositionsRemainExactAcrossIndexStridesAndFrontEviction)
{
    LineStore store;
    store.init(256);
    Ubiquitty::Cell row[128];
    for (int index = 0; index < 200; ++index) {
        fillRow(row, 128, index);
        store.append(row, 128, 0, 0, index % 2 == 0, false);
    }
    const auto position = store.positionAt(127);
    REQUIRE(position);
    CHECK_EQ(position->line, 63u);
    CHECK_EQ(position->cell, 128u);
    REQUIRE(store.rowForPosition(*position));
    CHECK_EQ(*store.rowForPosition(*position), 127);
    (void)store.foldColdBlocks(8);
    CHECK_EQ(store.positionAt(127)->cell, 128u);
    store.dropOldest(9);
    CHECK(!store.positionAt(0));
    REQUIRE(store.rowForPosition(*position));
    CHECK_EQ(*store.rowForPosition(*position), 118);
    REQUIRE(store.positionAt(118));
    CHECK_EQ(store.positionAt(118)->line, 63u);
    CHECK_EQ(store.positionAt(118)->cell, 128u);
    CHECK(!store.positionAt(-1));
    CHECK(!store.positionAt(store.count()));
    CHECK(!store.rowForPosition({ 1000, 0 }));
}

TEST(prependRestoresEvictedRowsWithRawOrFoldedHistory)
{
    for (const bool shouldFold : { false, true }) {
        LineStore store;
        store.init(300);
        std::vector<std::vector<Ubiquitty::Cell>> rows(300, std::vector<Ubiquitty::Cell>(80));
        for (int index = 0; index < 300; ++index) {
            for (auto& cell : rows[index])
                cell.codePoint = static_cast<std::uint32_t>('A' + index % 26);
            store.append(rows[index].data(), 80, 0, 0, index % 3 != 2, false);
        }
        const auto front = store.frontLine();
        const auto head = store.headLineNumber();
        if (shouldFold) {
            foldEverythingCold(store);
            REQUIRE(store.foldedBlockCount() > 0);
        }
        const StoredLine original { rows[0].data(), 80, true, 0, 0, false, false };
        for (int cycle = 0; cycle < 20; ++cycle) {
            store.append(rows.back().data(), 80, 0, 0, false, false);
            store.dropNewest(1);
            REQUIRE(store.prepend(std::span(&original, 1), front));
            REQUIRE(store.count() == 300);
            CHECK(store.headLineNumber() == head);
            CHECK(store.frontLine().number == front.number);
            for (int index = 0; index < 300; ++index) {
                const auto* restored = store.at(index);
                REQUIRE(restored);
                CHECK(restored->length == 80);
                CHECK(std::memcmp(restored->cells, rows[index].data(), 80 * sizeof(Ubiquitty::Cell)) == 0);
                const auto position = store.positionAt(index);
                REQUIRE(position);
                CHECK(position->line == static_cast<std::uint64_t>(index / 3));
            }
        }
        CHECK(store.totalAppended() == 320);
    }
}

TEST(prependRestoresBlankRowsAndRebasesAnEmptyStore)
{
    LineStore store;
    store.init(4);
    Ubiquitty::Cell content;
    content.codePoint = 'A';
    store.append(&content, 1, 0, 0, false, false);
    store.dropNewest(1);
    const std::array<StoredLine, 3> rows { {
        { nullptr, 0, false, 0, 2, false, false },
        { &content, 1, true, 0, 4, false, false },
        { nullptr, 0, false, 0, 5, false, false },
    } };
    REQUIRE(store.prepend(rows, { 10, LineStore::LineContinuity::StartsLine }));
    CHECK(store.count() == 3);
    CHECK(store.headLineNumber() == 12);
    CHECK(store.totalAppended() == 1);
    CHECK(store.at(0)->cells == nullptr);
    CHECK(store.at(1)->cells[0].codePoint == 'A');
    CHECK(store.at(2)->mark == 5);
    store.append(&content, 1, 0, 0, false, false);
    store.append(&content, 1, 0, 0, false, false);
    CHECK(store.count() == 4);
    CHECK(store.at(0)->cells[0].codePoint == 'A');
    CHECK(store.frontLine().number == 11);
}

TEST(prependCopiesSeveralBlocksAndPreservesRetainedRows)
{
    LineStore store;
    store.init(200);
    Ubiquitty::Cell tail;
    tail.codePoint = 'T';
    store.append(&tail, 1, 0, 0, false, false);
    std::vector<Ubiquitty::Cell> content(128);
    fillRow(content.data(), 128, 1);
    std::vector<StoredLine> prefix(150, StoredLine { content.data(), 128, false, 0, 0, false, false });
    REQUIRE(store.prepend(prefix, { 50, LineStore::LineContinuity::StartsLine }));
    REQUIRE(store.count() == 151);
    for (int index = 0; index < 150; ++index)
        CHECK(std::memcmp(store.at(index)->cells, content.data(), content.size() * sizeof(Ubiquitty::Cell)) == 0);
    CHECK(store.at(150)->cells[0].codePoint == 'T');
    CHECK(store.headLineNumber() == 201);
    foldEverythingCold(store);
    for (int index = 0; index < 150; ++index)
        CHECK(std::memcmp(store.at(index)->cells, content.data(), content.size() * sizeof(Ubiquitty::Cell)) == 0);
    store.dropOldest(100);
    CHECK(store.frontLine().number == 150);
    CHECK(store.at(50)->cells[0].codePoint == 'T');
}

TEST(prependRefusesInvalidRowsWithoutChangingHistory)
{
    LineStore store;
    store.init(2);
    Ubiquitty::Cell content;
    content.codePoint = 'A';
    store.append(&content, 1, 0, 0, false, false);
    StoredLine invalid { nullptr, 1, false, 0, 0, false, false };
    CHECK(!store.prepend(std::span(&invalid, 1), store.frontLine()));
    invalid.cells = &content;
    invalid.length = -1;
    CHECK(!store.prepend(std::span(&invalid, 1), store.frontLine()));
    invalid.length = 1;
    CHECK(!store.prepend(std::span(&invalid, 1), { std::numeric_limits<std::uint64_t>::max(), LineStore::LineContinuity::StartsLine }));
    const std::array<StoredLine, 2> overflow { invalid, invalid };
    CHECK(!store.prepend(overflow, store.frontLine()));
    CHECK(!store.prepend(overflow, store.frontLine(), -1));
    CHECK(!store.prepend(overflow, store.frontLine(), 2));
    const std::array<StoredLine, 3> tooMany { invalid, invalid, invalid };
    CHECK(!store.prepend(tooMany, store.frontLine(), 1));
    CHECK(store.count() == 1);
    CHECK(store.at(0)->cells[0].codePoint == 'A');
    CHECK(store.headLineNumber() == 1);
}

// ---- Compact rows: appendDefaultASCII keeps bytes, a read expands them ----

static std::vector<std::uint8_t> asciiRowBytes(int seed, int length)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    for (int column = 0; column < length; ++column)
        bytes[static_cast<std::size_t>(column)] = static_cast<std::uint8_t>(0x20 + (seed * 7 + column) % 95);
    return bytes;
}

// The cells an ASCII row expands to: the default blank cell with the byte as
// its code point, and zero past the bytes up to the stored length.
static bool asciiRowMatches(const StoredLine* stored, const std::vector<std::uint8_t>& bytes, int length)
{
    if (!stored || stored->length != length || !stored->cells)
        return false;
    for (int column = 0; column < length; ++column) {
        Ubiquitty::Cell expected = Ubiquitty::defaultBlankCell;
        expected.codePoint = static_cast<std::size_t>(column) < bytes.size() ? bytes[static_cast<std::size_t>(column)] : 0;
        if (std::memcmp(&stored->cells[column], &expected, sizeof(Ubiquitty::Cell)) != 0)
            return false;
    }
    return true;
}

TEST(testDefaultASCIIRowsExpandOnFirstRead)
{
    LineStore s;
    s.init(64);
    for (int i = 0; i < 20; ++i) {
        const auto bytes = asciiRowBytes(i, 10 + i);
        // A stored length past the bytes is blank cells, as a capture trims.
        s.appendDefaultASCII(bytes, 10 + i + (i % 3), 0, static_cast<uint8_t>(i), i % 2 == 0, false);
    }
    CHECK_EQ(s.count(), 20);
    CHECK(s.arenaBytes() > 0);
    for (int i = 0; i < 20; ++i) {
        const auto bytes = asciiRowBytes(i, 10 + i);
        const StoredLine* line = s.at(i);
        CHECK_MSG(asciiRowMatches(line, bytes, 10 + i + (i % 3)), "an ASCII row read back differs from the bytes appended");
        CHECK(line && !line->isCompact);
        CHECK(line && line->mark == i && line->wrapped == (i % 2 == 0));
        CHECK(s.at(i) == line); // the second read is the same cells, expanded once
    }
}

TEST(testWritableAtExpandsACompactRowBeforeItIsWritten)
{
    LineStore s;
    s.init(8);
    const auto bytes = asciiRowBytes(3, 12);
    s.appendDefaultASCII(bytes, 12, 0, 0, false, false);
    StoredLine* line = s.writableAt(0);
    CHECK(line && !line->isCompact);
    CHECK(asciiRowMatches(line, bytes, 12));
    if (line) {
        line->cells[4].codePoint = 0x4E2D;
        CHECK(s.at(0)->cells[4].codePoint == 0x4E2D);
    }
}

TEST(testCompactRowsFoldReadAndEvictLikeCellRows)
{
    struct Expected {
        bool isASCII;
        SeededLine line;
        std::vector<std::uint8_t> bytes;
        int length;
    };
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        Random random = seededRandom(seed * 0x9E3779B97F4A7C15ull);
        LineStore s;
        s.init(4000);
        std::vector<Expected> lines;
        while (s.blockCount() < 6) {
            if (random.below(2) == 0) {
                const int length = static_cast<int>(random.below(161));
                const int written = static_cast<int>(random.below(static_cast<std::uint32_t>(length) + 1));
                Expected expected { true, {}, asciiRowBytes(static_cast<int>(lines.size()), written), length };
                s.appendDefaultASCII(expected.bytes, length, 0, 0, random.below(2) == 0, false);
                lines.push_back(std::move(expected));
            } else {
                lines.push_back(Expected { false, appendRandomLine(s, random, 160), {}, 0 });
            }
        }
        const auto checkAll = [&](const char* when) {
            int mismatches = 0;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const StoredLine* stored = s.at(static_cast<int>(i));
                const bool matches = lines[i].isASCII
                    ? (lines[i].length == 0 ? stored && stored->length == 0 : asciiRowMatches(stored, lines[i].bytes, lines[i].length))
                    : lineMatches(stored, lines[i].line);
                mismatches += matches ? 0 : 1;
            }
            CHECK_MSG(mismatches == 0, when);
        };
        // Read a few rows now, so some compact rows are expanded before the
        // fold and the rest by it.
        for (std::size_t i = 0; i < lines.size(); i += 5)
            (void)s.at(static_cast<int>(i));
        const std::size_t rawBytes = s.arenaBytes();
        foldEverythingCold(s);
        CHECK_EQ(s.foldedBlockCount(), s.blockCount() - LineStore::rawBlockWindow);
        CHECK(s.arenaBytes() < rawBytes);
        checkAll("a row read after the fold differs from the row appended");
        // Evict everything through the capacity, then drop the rest: every
        // slab and text slab goes with its block.
        for (int i = 0; i < 4000; ++i)
            s.appendDefaultASCII(asciiRowBytes(i, 8), 8, 0, 0, false, false);
        CHECK_EQ(s.count(), 4000);
        s.clear();
        CHECK_EQ(s.arenaBytes(), 0u);
    }
}

TEST(testDropNewestThroughCompactRows)
{
    LineStore s;
    s.init(32);
    for (int i = 0; i < 10; ++i)
        s.appendDefaultASCII(asciiRowBytes(i, 6 + i), 6 + i, 0, 0, false, false);
    (void)s.at(9); // one expanded row among the dropped
    s.dropNewest(4);
    CHECK_EQ(s.count(), 6);
    for (int i = 0; i < 6; ++i)
        CHECK(asciiRowMatches(s.at(i), asciiRowBytes(i, 6 + i), 6 + i));
    // The slack the drop freed is reused by the next rows, bytes and cells alike.
    for (int i = 6; i < 10; ++i)
        s.appendDefaultASCII(asciiRowBytes(100 + i, 6 + i), 6 + i, 0, 0, false, false);
    for (int i = 6; i < 10; ++i)
        CHECK(asciiRowMatches(s.at(i), asciiRowBytes(100 + i, 6 + i), 6 + i));
    s.dropNewest(10);
    CHECK_EQ(s.count(), 0);
    CHECK_EQ(s.arenaBytes(), 0u);
}

UBIQUITTY_TEST_MAIN()
