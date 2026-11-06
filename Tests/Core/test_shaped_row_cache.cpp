// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

//
// test_shaped_row_cache.cpp — getDisplayRow memoizes the Arabic-shape + bidi
// pass per row, keyed on a per-row content generation. These pin: a repeat
// fetch hits the cache (no reshape) with byte-identical output; a cell mutation
// bumps the generation so the next fetch reshapes; frozen history rows cache
// across fetches; and resize / alt-screen switch invalidate.
//

#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using Ubiquitty::hardReset;
using Ubiquitty::processBytes;
using Ubiquitty::resize;
using Ubiquitty::Screen::getDisplayRow;
using Ubiquitty::Screen::scrollbackCount;
using Ubiquitty::Screen::shapedRowInvocationCount;

// Arabic BEH (U+0628, UTF-8 D8 A8) and JEEM (U+062C, D8 AC) — both cursive, so a
// row holding them reports needsShaping and drives the shape path.
static const char* beh = "\xD8\xA8";
static const char* jeem = "\xD8\xAC";

// A repeat fetch of a static RTL row hits the cache: no extra shape pass, and
// byte-identical output.
TEST(testShapedRowCacheParity)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    feed(t, beh);
    feed(t, beh);
    feed(t, beh);

    Ubiquitty::Cell first[40], second[40];
    const uint64_t before = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, first, 40);
    const uint64_t afterFirst = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, second, 40);
    const uint64_t afterSecond = shapedRowInvocationCount();

    CHECK(afterFirst == before + 1); // first fetch runs the shape pass
    CHECK(afterSecond == afterFirst); // second fetch is a cache hit
    CHECK(std::memcmp(first, second, sizeof(first)) == 0);
}

// A cell mutation bumps the row generation, so the next fetch reshapes and
// reflects the change. Verify-by-revert: drop the generation bump in bumpRow and
// the reshape CHECK below fails (a stale glyph would ship).
TEST(testShapedRowCacheInvalidation)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    feed(t, beh);
    feed(t, beh);

    Ubiquitty::Cell before[40];
    getDisplayRow(t, 0, 0, before, 40);
    const uint64_t afterShape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, before, 40);
    CHECK(shapedRowInvocationCount() == afterShape); // cached before the mutation

    feed(t, "\r"); // cursor to column 0
    feed(t, jeem); // overwrite column 0 with a different cursive letter

    Ubiquitty::Cell after[40];
    const uint64_t beforeReshape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, after, 40);
    CHECK(shapedRowInvocationCount() == beforeReshape + 1); // mutation forced a reshape
    CHECK(std::memcmp(before, after, sizeof(before)) != 0); // and the output changed
}

// History rows are not cached: their absolute index rebinds to a different line
// on ring eviction, so a cached entry could serve stale glyphs. Each scrolled-
// back fetch reshapes, which is eviction-safe by construction. (The idle live
// screen — the target — still caches.)
TEST(testShapedRowCacheHistoryNotCached)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 4, 40);
    feed(t, beh);
    feed(t, beh);
    feed(t, "\r\n");
    for (int i = 0; i < 8; ++i)
        feed(t, "x\r\n"); // push the Arabic line into history

    const int viewOffset = scrollbackCount(&t); // scroll fully back
    Ubiquitty::Cell out[40];
    const uint64_t start = shapedRowInvocationCount();
    for (int r = 0; r < t.rows; ++r)
        getDisplayRow(t, viewOffset, r, out, 40);
    const uint64_t firstPass = shapedRowInvocationCount() - start;
    for (int r = 0; r < t.rows; ++r)
        getDisplayRow(t, viewOffset, r, out, 40);
    const uint64_t secondPass = shapedRowInvocationCount() - start - firstPass;

    CHECK(firstPass > 0); // shaped the RTL history row(s)
    CHECK(secondPass == firstPass); // reshaped the same count — history is not cached
}

// Resize remaps row identities, so it invalidates the cache: the next fetch
// reshapes.
TEST(testShapedRowCacheResizeInvalidates)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    feed(t, beh);
    feed(t, beh);

    Ubiquitty::Cell out[50];
    getDisplayRow(t, 0, 0, out, 40);
    const uint64_t afterShape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, out, 40);
    CHECK(shapedRowInvocationCount() == afterShape); // cached at the old size

    resize(t, 6, 50);
    const uint64_t beforeReshape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, out, 50);
    CHECK(shapedRowInvocationCount() == beforeReshape + 1); // invalidated by resize
}

// A full-viewport scroll remaps every live row. The cache invalidates as one
// epoch operation, so the row that moves into absolute row zero cannot reuse the
// shaped cells previously cached for a different row at that identity.
TEST(testShapedRowCacheFullScrollInvalidates)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 4, 40);
    feed(t, beh);
    feed(t, "\r\n");
    feed(t, jeem);

    Ubiquitty::Cell before[40], after[40];
    getDisplayRow(t, 0, 0, before, 40);
    const uint64_t afterShape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, before, 40);
    CHECK(shapedRowInvocationCount() == afterShape);

    feed(t, "\x1b[4;1H\r\n"); // scroll once: the JEEM row moves to live row zero
    const uint64_t beforeReshape = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, after, 40);
    CHECK(shapedRowInvocationCount() == beforeReshape + 1);
    CHECK(std::memcmp(before, after, sizeof(before)) != 0);
}

// Two live rows never share a slot, however tall the screen. A fixed-size direct
// map folded row N onto row N minus the size, and a screen taller than it made
// that pair evict each other on every frame that drew both — permanently, since
// both stay visible.
TEST(testShapedRowCacheHoldsBothRowsOfATallScreen)
{
    Ubiquitty::Terminal t;
    hardReset(t);
    resize(t, 200, 40); // taller than the cache's former fixed size
    feed(t, beh);
    feed(t, "\x1b[129;1H"); // row 128, zero-based: the one a 128-slot map folds onto row 0
    feed(t, jeem);

    Ubiquitty::Cell out[40];
    getDisplayRow(t, 0, 0, out, 40);
    getDisplayRow(t, 0, 128, out, 40);
    const uint64_t afterBoth = shapedRowInvocationCount();
    getDisplayRow(t, 0, 0, out, 40);
    getDisplayRow(t, 0, 128, out, 40);
    CHECK(shapedRowInvocationCount() == afterBoth);
}

UBIQUITTY_TEST_MAIN()
