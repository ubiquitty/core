// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A grid that cannot grow keeps the grid it has.
//
// The Core builds without exceptions, so a throwing allocation that fails is
// std::terminate — a window drag under memory pressure taking down every
// session in the daemon. The reflow and sixel paths already degrade instead;
// the grid's own buffers did not.
//
// Allocation failure cannot be waited for, so it is injected: the nothrow
// operator new is a replaceable function, and this binary replaces it with one
// that can be told to refuse. What that proves is the part that matters —
// after a refused grow the grid is still the grid it was, and still readable.

#include "Assertions.h"
#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Resize.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {

// Set while a case wants allocations to fail. Only the nothrow forms are
// replaced: a throwing one that failed would terminate, which is the outcome
// this file exists to show cannot happen on the paths it covers.
//
// A granted allocation comes from the ordinary operator new, because the
// ordinary operator delete is what frees it: the Core pairs a nothrow new with
// a plain delete, and a replacement that hands out malloc memory breaks that
// pair under an allocator that tells the two apart.
bool shouldRefuseNothrowAllocations = false;

struct AllocationFailure {
    int remaining = -1;
    bool didRefuse = false;

    static AllocationFailure& singleton()
    {
        static AllocationFailure state;
        return state;
    }

    bool shouldRefuse()
    {
        if (remaining < 0)
            return false;
        if (remaining-- > 0)
            return false;
        didRefuse = true;
        return true;
    }
};

} // namespace

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (shouldRefuseNothrowAllocations || AllocationFailure::singleton().shouldRefuse())
        return nullptr;
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (shouldRefuseNothrowAllocations || AllocationFailure::singleton().shouldRefuse())
        return nullptr;
    return ::operator new[](size);
}

void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }

TEST(aGridThatCannotGrowKeepsTheGridItHas)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 40);
    Ubiquitty::processBytes(&t, "\x1b[1;1Hfirst row\x1b[5;1Hfifth row");

    const int rowsBefore = t.rows;
    const int columnsBefore = t.columns;
    CHECK(rowsBefore == 10 && columnsBefore == 40);

    // A grow past the current capacity, with every nothrow allocation refused.
    shouldRefuseNothrowAllocations = true;
    Ubiquitty::resize(t, 400, 400);
    shouldRefuseNothrowAllocations = false;

    CHECK_MSG(t.rows == rowsBefore, "a refused grow changed the row count");
    CHECK_MSG(t.columns == columnsBefore, "a refused grow changed the column count");

    // And the grid still reads: the refusal left buffers behind, not a
    // half-built geometry that indexes past them.
    const Ubiquitty::Cell* first = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    CHECK_MSG(first != nullptr && first->codePoint == 'f', "the first row lost its content");
    const Ubiquitty::Cell* fifth = Ubiquitty::Screen::visibleCell(&t, 0, 4, 0);
    CHECK_MSG(fifth != nullptr && fifth->codePoint == 'f', "the fifth row lost its content");

    // Writing still lands where it should, so the grid is not merely intact but
    // usable — which a caller that ignored the refusal would have broken.
    Ubiquitty::processBytes(&t, "\x1b[2;1Hsecond row");
    const Ubiquitty::Cell* second = Ubiquitty::Screen::visibleCell(&t, 0, 1, 0);
    CHECK_MSG(second != nullptr && second->codePoint == 's', "a write after the refusal did not land");
}

TEST(aGrowThatSucceedsStillGrows)
{
    // The other side, so the case above cannot pass by refusing everything: the
    // same grow with allocation allowed reaches the geometry it asked for.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 40);
    Ubiquitty::processBytes(&t, "\x1b[1;1Hfirst row");

    Ubiquitty::resize(t, 400, 400);
    CHECK_MSG(t.rows == 400, "the grow did not reach its rows");
    CHECK_MSG(t.columns == 400, "the grow did not reach its columns");
    const Ubiquitty::Cell* first = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    CHECK_MSG(first != nullptr && first->codePoint == 'f', "the grow lost the content it carried");
}

TEST(aShapedRowCacheThatCannotBeSizedIsNoCache)
{
    // The shaped-row memo is sized to the live rows, so it is the grid's other
    // geometry-bound allocation. Refused, the row still shapes — the same
    // cells the memo would have handed back — and the next fetch shapes it
    // again, which is what an absent memo means. Allowed, the memo is built
    // and the fetch after that is a hit.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);
    // Arabic BEH, three times: cursive, so the row shapes.
    Ubiquitty::processBytes(&t, "\xD8\xA8\xD8\xA8\xD8\xA8");

    Ubiquitty::Cell expected[40];
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, expected, 40);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);
    Ubiquitty::processBytes(&t, "\xD8\xA8\xD8\xA8\xD8\xA8");

    Ubiquitty::Cell refused[40];
    shouldRefuseNothrowAllocations = true;
    const std::uint64_t before = Ubiquitty::Screen::shapedRowInvocationCount();
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, refused, 40);
    const std::uint64_t afterFirst = Ubiquitty::Screen::shapedRowInvocationCount();
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, refused, 40);
    const std::uint64_t afterSecond = Ubiquitty::Screen::shapedRowInvocationCount();
    shouldRefuseNothrowAllocations = false;

    CHECK_MSG(afterFirst == before + 1, "the row was not shaped with the memo refused");
    CHECK_MSG(afterSecond == afterFirst + 1, "a fetch after a refused memo did not shape again");
    CHECK_MSG(std::memcmp(expected, refused, sizeof(expected)) == 0, "a refused memo changed what the row shapes to");

    Ubiquitty::Screen::getDisplayRow(t, 0, 0, refused, 40);
    const std::uint64_t afterAllowed = Ubiquitty::Screen::shapedRowInvocationCount();
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, refused, 40);
    const std::uint64_t afterHit = Ubiquitty::Screen::shapedRowInvocationCount();
    CHECK_MSG(afterAllowed == afterSecond + 1, "the fetch that could build the memo did not shape");
    CHECK_MSG(afterHit == afterAllowed, "the fetch after the memo was built was not a hit");
}

TEST(aResetUnderRefusalKeepsAUsableTerminal)
{
    // RIS destroys the Terminal and rebuilds it. The rebuild must not free
    // working buffers to reallocate them: under pressure the realloc is
    // refused where keeping would have succeeded, and a refused fresh grid
    // initializes to a degenerate geometry the Terminal then disagrees with.
    // The live grid is carried across the wipe, so a reset under refusal
    // keeps the geometry it had, blanks it (RIS still clears), and stays
    // writable — which a caller that ignored the refusal would have broken.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 40);
    Ubiquitty::processBytes(&t, "\x1b[1;1Hfirst row");

    shouldRefuseNothrowAllocations = true;
    Ubiquitty::hardReset(t);
    shouldRefuseNothrowAllocations = false;

    CHECK_MSG(t.rows == 10, "a refused rebuild changed the row count");
    CHECK_MSG(t.columns == 40, "a refused rebuild changed the column count");

    // RIS clears: the carried grid is re-blanked, not carried with content.
    const Ubiquitty::Cell* cleared = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    CHECK_MSG(cleared != nullptr && cleared->codePoint == 0, "the reset did not clear the screen");

    // And the grid is usable, not merely intact.
    Ubiquitty::processBytes(&t, "\x1b[2;1Hsecond row");
    const Ubiquitty::Cell* second = Ubiquitty::Screen::visibleCell(&t, 0, 1, 0);
    CHECK_MSG(second != nullptr && second->codePoint == 's', "a write after the refused rebuild did not land");
}

TEST(aRefusedReflowPreservesGeometryContentAndPendingSettle)
{
    using namespace Ubiquitty;
    for (const auto urgency : { Modes::ResizeUrgency::Settled, Modes::ResizeUrgency::Interactive }) {
        for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
            for (int allocation = 0; allocation < 3; ++allocation) {
                Terminal terminal;
                hardReset(terminal);
                resize(terminal, 6, 40);
                processBytes(&terminal, "first\r\nsecond\r\nthird\r\nfourth\r\nfifth\r\nsixth\r\nlast");
                if (screen == ScreenMode::Alternate)
                    processBytes(&terminal, "\x1b[?1049hEXISTING ALTERNATE");
                REQUIRE(terminal.grid->reserveCapacity(8, 60));
                const auto epoch = terminal.absoluteRowEpoch;
                const int history = terminal.scrollbackCount;
                const int cursorRow = terminal.screens.active->cursorY;
                const int cursorColumn = terminal.screens.active->cursorX;
                std::vector<Cell> before(6 * 40);
                for (int row = 0; row < 6; ++row)
                    terminal.grid->readRow(Row(row), before.data() + row * 40);
                auto& failure = AllocationFailure::singleton();
                failure = { allocation, false };
                const auto outcome = Screen::resizeGrid(&terminal, 8, 60, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
                failure.remaining = -1;
                CHECK(failure.didRefuse);
                CHECK(!outcome.geometryChanged);
                CHECK(terminal.rows == 6 && terminal.columns == 40);
                CHECK(terminal.grid->rows() == terminal.rows && terminal.grid->columns() == terminal.columns);
                CHECK(terminal.absoluteRowEpoch == epoch);
                CHECK(terminal.scrollbackCount == history);
                CHECK(terminal.pendingHistoryRewrapColumns == 0);
                CHECK(terminal.screens.active->cursorY == cursorRow && terminal.screens.active->cursorX == cursorColumn);
                std::vector<Cell> after(6 * 40);
                for (int row = 0; row < 6; ++row)
                    terminal.grid->readRow(Row(row), after.data() + row * 40);
                CHECK(std::memcmp(before.data(), after.data(), before.size() * sizeof(Cell)) == 0);
                resize(terminal, 8, 60, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, urgency);
                CHECK(terminal.rows == 8 && terminal.columns == 60);
                CHECK(terminal.grid->rows() == 8 && terminal.grid->columns() == 60);
            }
        }
    }
    for (int allocation = 0; allocation < 3; ++allocation) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 6, 40);
        for (int line = 0; line < 30; ++line)
            processBytes(&terminal, "retained history\r\n");
        resize(terminal, 6, 20, Modes::ReflowMode::Reflow, Modes::AnchorMode::Bottom, Modes::ResizeUrgency::Interactive);
        REQUIRE(terminal.pendingHistoryRewrapColumns == 20);
        const auto epoch = terminal.absoluteRowEpoch;
        const int history = terminal.scrollbackCount;
        auto& failure = AllocationFailure::singleton();
        failure = { allocation, false };
        settleResize(&terminal);
        failure.remaining = -1;
        CHECK(failure.didRefuse);
        CHECK(terminal.pendingHistoryRewrapColumns == 20);
        CHECK(terminal.pendingHistoryRewrapAnchor == Modes::AnchorMode::Bottom);
        CHECK(terminal.scrollbackCount == history && terminal.absoluteRowEpoch == epoch);
        CHECK(terminal.rows == terminal.grid->rows() && terminal.columns == terminal.grid->columns());
        settleResize(&terminal);
        CHECK(terminal.pendingHistoryRewrapColumns == 0);
    }
}

UBIQUITTY_TEST_MAIN()
