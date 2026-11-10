// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_history_rewrap — the settle's rewrap of the history a width drag deferred, in slices.
//
// The reference is the one pass: the same terminal, settled by re-entering resize at its own width
// (`Settled` urgency with the rewrap pending), which rewraps every row of history and screen in one go.
// The subject is `advanceHistoryRewrap` called with budgets from one microsecond (every line its own
// slice) to none (one slice). They must leave the same rows, flags, marks, numbering and cursor.

#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Resize.h"
#include "Core/Screen/Scrollback.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

/// A deterministic stream of numbers for building histories.
class Random {
public:
    explicit Random(std::uint32_t seed)
        : m_state(seed * 2654435761u + 1u)
    {
    }

    std::uint32_t next()
    {
        m_state = m_state * 1664525u + 1013904223u;
        return m_state >> 8;
    }

    int below(int bound) { return static_cast<int>(next() % static_cast<std::uint32_t>(bound)); }

private:
    std::uint32_t m_state;
};

/// Text a program might write: runs of letters, wide characters at any column (so some straddle a
/// margin), colour changes, prompt marks, double-width lines, and lines of every length from empty to
/// several rows long.
std::string makeHistoryText(std::uint32_t seed, int lines)
{
    Random random(seed);
    std::string text;
    for (int line = 0; line < lines; ++line) {
        if (random.below(12) == 0)
            text += "\033]133;A\007";
        if (random.below(15) == 0)
            text += "\033#6";
        const int length = random.below(6) == 0 ? 0 : random.below(130) + 1;
        for (int cell = 0; cell < length; ++cell) {
            const int pick = random.below(40);
            if (pick == 0)
                text += "\xE3\x81\x82"; // a wide character
            else if (pick == 1)
                text += "\033[3" + std::to_string(random.below(7) + 1) + "m";
            else if (pick == 2)
                text += "\033[0m";
            else
                text += static_cast<char>('a' + random.below(26));
        }
        text += "\033[0m\r\n";
    }
    return text;
}

/// Everything a settle may change, as a string: geometry, cursor, every history row with its flags and
/// cells, the numbering of the history's lines, and the visible rows.
std::string fingerprint(Ubiquitty::Terminal& t)
{
    std::string out;
    char header[160];
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(t);
    std::snprintf(header, sizeof header, "dims=%dx%d sb=%d cur=%d,%d oldest=%llu head=%llu|", t.rows, t.columns,
        Ubiquitty::Screen::scrollbackCount(&t), t.screens.primary.cursorY, t.screens.primary.cursorX,
        static_cast<unsigned long long>(address.oldest), static_cast<unsigned long long>(address.head));
    out += header;
    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    for (int index = 0; index < history; ++index) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, index);
        if (!line) {
            out += "<null>\n";
            continue;
        }
        char meta[64];
        std::snprintf(meta, sizeof meta, "[%d,%d,%u,%u,%d]", line->length, line->wrapped ? 1 : 0, line->lineAttribute, line->mark, line->needsShaping ? 1 : 0);
        out += meta;
        for (int cell = 0; cell < line->length; ++cell) {
            const Ubiquitty::Cell& value = line->cells[cell];
            std::snprintf(meta, sizeof meta, "%x:%x,", value.codePoint, static_cast<unsigned>(Ubiquitty::cellPen(value).foregroundIndex));
            out += meta;
        }
        out += '\n';
    }
    out += "--screen--\n";
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, column);
            char piece[24];
            std::snprintf(piece, sizeof piece, "%x,", cell ? cell->codePoint : 0u);
            out += piece;
        }
        out += Ubiquitty::Screen::visibleRowWrapped(&t, 0, row) ? "~\n" : ".\n";
    }
    return out;
}

/// A terminal with a history, taken through the interactive steps of a drag and left with its rewrap
/// pending.
struct Dragged {
    Ubiquitty::Terminal terminal;
};

void buildDragged(Ubiquitty::Terminal& t, std::uint32_t seed, int limit, const std::vector<int>& widths)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::setScrollbackLimit(&t, limit);
    Ubiquitty::resize(t, 9, 60);
    feed(t, makeHistoryText(seed, 140).c_str());
    for (const int width : widths)
        Ubiquitty::resize(t, 9, width, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
}

/// The one pass: settles by re-entering resize at the terminal's own width.
void settleInOnePass(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, t.rows, t.columns, Ubiquitty::Modes::ReflowMode::Reflow, t.pendingHistoryRewrapAnchor, Ubiquitty::Modes::ResizeUrgency::Settled);
}

/// The sliced settle, at a budget, until it finishes.
int settleInSlices(Ubiquitty::Terminal& t, std::uint32_t budgetMicros, int appendEvery = 0, const std::string& appended = {})
{
    int calls = 0;
    for (;;) {
        const auto progress = Ubiquitty::advanceHistoryRewrap(&t, budgetMicros);
        ++calls;
        if (progress != Ubiquitty::HistoryRewrapProgress::Pending)
            return progress == Ubiquitty::HistoryRewrapProgress::Finished ? calls : -calls;
        if (appendEvery && calls % appendEvery == 0)
            feed(t, appended.c_str());
        if (calls > 100000)
            return -calls;
    }
}

} // namespace

// Slices of every size leave what the one pass leaves, over histories that mix widths, wide characters
// at the margin, marks and double-width lines, for drags that narrow, widen and zigzag.
TEST(aSlicedRewrapEqualsTheOnePass)
{
    const std::vector<std::vector<int>> drags { { 41 }, { 90 }, { 80, 33, 47 }, { 20 }, { 70, 100, 30, 55 } };
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        for (const auto& widths : drags) {
            Ubiquitty::Terminal reference;
            buildDragged(reference, seed, 1000, widths);
            REQUIRE(reference.pendingHistoryRewrapColumns != 0);
            settleInOnePass(reference);
            REQUIRE(reference.pendingHistoryRewrapColumns == 0);
            const std::string expected = fingerprint(reference);
            for (const std::uint32_t budget : { 1u, 20u, 200u, 0u }) {
                Ubiquitty::Terminal subject;
                buildDragged(subject, seed, 1000, widths);
                REQUIRE(subject.pendingHistoryRewrapColumns != 0);
                CHECK(settleInSlices(subject, budget) > 0);
                CHECK(subject.pendingHistoryRewrapColumns == 0);
                CHECK(subject.historyRewrap == nullptr);
                CHECK(fingerprint(subject) == expected);
            }
        }
    }
}

// The test can fail: a row dropped where one call ended and the next began is seen, and the same fault
// in a single call, which has no boundary, is not.
TEST(theDifferentialSeesARowDroppedAtASliceBoundary)
{
#if UBIQUITTY_ENABLE_TEST_HOOKS
    Ubiquitty::Terminal reference;
    buildDragged(reference, 3, 1000, { 41 });
    settleInOnePass(reference);
    const std::string expected = fingerprint(reference);

    Ubiquitty::Screen::historyRewrapFaultForTest = Ubiquitty::Screen::HistoryRewrapFault::DropRowAfterSliceBoundary;
    Ubiquitty::Terminal sliced;
    buildDragged(sliced, 3, 1000, { 41 });
    CHECK(settleInSlices(sliced, 1) > 0);
    CHECK(fingerprint(sliced) != expected);

    Ubiquitty::Terminal whole;
    buildDragged(whole, 3, 1000, { 41 });
    CHECK(settleInSlices(whole, 0) > 0);
    CHECK(fingerprint(whole) == expected);
    Ubiquitty::Screen::historyRewrapFaultForTest = Ubiquitty::Screen::HistoryRewrapFault::None;
#endif
}

// Output that arrives between slices lands after the rewrapped history, as it does when it arrives after
// a settle in one pass.
TEST(outputBetweenSlicesLandsAfterTheRewrappedHistory)
{
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        const std::string more = makeHistoryText(seed + 100, 12);
        Ubiquitty::Terminal reference;
        buildDragged(reference, seed, 4000, { 37 });
        settleInOnePass(reference);
        feed(reference, more.c_str());

        Ubiquitty::Terminal subject;
        buildDragged(subject, seed, 4000, { 37 });
        // The same text, fed a piece per slice: the screen scrolls it into history at the new width.
        std::size_t fed = 0;
        const std::size_t piece = more.size() / 24 + 1;
        for (;;) {
            const auto progress = Ubiquitty::advanceHistoryRewrap(&subject, 1);
            if (fed < more.size()) {
                feed(subject, more.substr(fed, piece).c_str());
                fed += piece;
            }
            if (progress != Ubiquitty::HistoryRewrapProgress::Pending)
                break;
        }
        if (fed < more.size())
            feed(subject, more.substr(fed).c_str());
        CHECK(subject.pendingHistoryRewrapColumns == 0);
        CHECK(fingerprint(subject) == fingerprint(reference));
    }
}

// A resize while the rewrap is open cancels it, and the settle that follows starts again and ends where a
// settle with no interruption does.
TEST(aResizeBetweenSlicesCancelsTheJobAndTheNextSettleStartsAgain)
{
    Ubiquitty::Terminal reference;
    buildDragged(reference, 5, 1000, { 50, 31 });
    settleInOnePass(reference);

    Ubiquitty::Terminal subject;
    buildDragged(subject, 5, 1000, { 50 });
    REQUIRE(Ubiquitty::advanceHistoryRewrap(&subject, 1) == Ubiquitty::HistoryRewrapProgress::Pending);
    REQUIRE(subject.historyRewrap != nullptr);
    Ubiquitty::resize(subject, 9, 31, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    CHECK(subject.historyRewrap == nullptr);
    CHECK(subject.pendingHistoryRewrapColumns == 31);
    CHECK(settleInSlices(subject, 1) > 0);
    CHECK(fingerprint(subject) == fingerprint(reference));
}

// A clear while the rewrap is open (history is dropped and a new epoch opens) cancels it; nothing of the
// staged lines reaches the store.
TEST(aClearBetweenSlicesCancelsTheJobAndLeavesNoStagedLine)
{
    Ubiquitty::Terminal subject;
    buildDragged(subject, 7, 1000, { 44 });
    REQUIRE(Ubiquitty::advanceHistoryRewrap(&subject, 1) == Ubiquitty::HistoryRewrapProgress::Pending);
    feed(subject, "\033[3J");
    CHECK(Ubiquitty::Screen::scrollbackCount(&subject) == 0);
    const auto progress = Ubiquitty::advanceHistoryRewrap(&subject, 0);
    CHECK(progress != Ubiquitty::HistoryRewrapProgress::Pending);
    CHECK(subject.historyRewrap == nullptr);
    CHECK(Ubiquitty::Screen::scrollbackCount(&subject) == 0);
}

// Eviction while the rewrap is open: the scrollback keeps its bound, its lines stay numbered in order,
// and the lines it keeps are the newest ones the one pass keeps.
TEST(evictionBetweenSlicesKeepsTheBoundAndTheNewestLines)
{
    const int limit = 90;
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        const std::string more = makeHistoryText(seed + 200, 40);
        Ubiquitty::Terminal reference;
        buildDragged(reference, seed, limit, { 37 });
        settleInOnePass(reference);
        feed(reference, more.c_str());

        Ubiquitty::Terminal subject;
        buildDragged(subject, seed, limit, { 37 });
        std::size_t fed = 0;
        const std::size_t piece = more.size() / 40 + 1;
        for (;;) {
            const auto progress = Ubiquitty::advanceHistoryRewrap(&subject, 1);
            if (fed < more.size()) {
                feed(subject, more.substr(fed, piece).c_str());
                fed += piece;
            }
            if (progress != Ubiquitty::HistoryRewrapProgress::Pending)
                break;
        }
        if (fed < more.size())
            feed(subject, more.substr(fed).c_str());
        CHECK(Ubiquitty::Screen::scrollbackCount(&subject) <= limit);
        CHECK(subject.pendingHistoryRewrapColumns == 0);
        // The newest lines agree row for row with the one pass: the last forty rows of history and the
        // screen below them.
        const int compared = 40;
        const int subjectRows = Ubiquitty::Screen::scrollbackCount(&subject);
        const int referenceRows = Ubiquitty::Screen::scrollbackCount(&reference);
        REQUIRE(subjectRows >= compared);
        REQUIRE(referenceRows >= compared);
        for (int offset = 1; offset <= compared; ++offset) {
            const auto* got = Ubiquitty::Screen::scrollbackLine(&subject, subjectRows - offset);
            const auto* want = Ubiquitty::Screen::scrollbackLine(&reference, referenceRows - offset);
            REQUIRE(got);
            REQUIRE(want);
            CHECK(got->length == want->length);
            CHECK(got->wrapped == want->wrapped);
        }
    }
}

// What the slicing buys, measured: the one pass's pause against the longest slice, over a history of
// ~100k rows narrowed from 200 columns to 61. Prints its numbers and asserts nothing; run it from an
// optimised build with UBQ_BENCH_HISTORY_REWRAP=1.
TEST(benchHistoryRewrapSlicesAgainstTheOnePass)
{
    if (!std::getenv("UBQ_BENCH_HISTORY_REWRAP"))
        return;
    const auto build = [](Ubiquitty::Terminal& t) {
        Ubiquitty::hardReset(t);
        Ubiquitty::setScrollbackLimit(&t, 200000);
        Ubiquitty::resize(t, 50, 200);
        for (int block = 0; block < 100; ++block)
            feed(t, makeHistoryText(static_cast<std::uint32_t>(block + 1), 1000).c_str());
        Ubiquitty::resize(t, 50, 61, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    };
    using Clock = std::chrono::steady_clock;
    Ubiquitty::Terminal onePass;
    build(onePass);
    const int stale = Ubiquitty::Screen::scrollbackCount(&onePass);
    const auto onePassStarted = Clock::now();
    settleInOnePass(onePass);
    const double onePassMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - onePassStarted).count();

    Ubiquitty::Terminal sliced;
    build(sliced);
    double longest = 0;
    double total = 0;
    int calls = 0;
    for (;;) {
        const auto started = Clock::now();
        const auto progress = Ubiquitty::advanceHistoryRewrap(&sliced, 1000);
        const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        total += milliseconds;
        longest = std::max(longest, milliseconds);
        ++calls;
        if (progress != Ubiquitty::HistoryRewrapProgress::Pending)
            break;
    }
    std::printf("history rewrap: %d stored rows; one pass %.2f ms; sliced %d calls, longest %.2f ms, total %.2f ms (%.2fx)\n",
        stale, onePassMilliseconds, calls, longest, total, total / onePassMilliseconds);
}

UBIQUITTY_TEST_MAIN()
