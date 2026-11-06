// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_history_lines.cpp — the address a history line keeps: the number of
// completed logical lines before it, in an epoch only a destructive clear
// advances (Docs/sync.md §6.6).

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/LineStore.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Marks.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using UbiquittyTest::feed;

namespace {

std::unique_ptr<Ubiquitty::Terminal> terminalOf(int rows, int columns, int scrollbackLines = 0)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    if (scrollbackLines > 0)
        Ubiquitty::setScrollbackLimit(t.get(), scrollbackLines);
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

// A line that names itself in its first five characters and is `length` long.
std::string namedLine(int index, int length)
{
    char name[16];
    std::snprintf(name, sizeof(name), "L%04d", index);
    std::string line = name;
    while (static_cast<int>(line.size()) < length)
        line.push_back(static_cast<char>('a' + line.size() % 26));
    line.resize(static_cast<std::size_t>(length));
    return line;
}

void feedNamedLines(Ubiquitty::Terminal& t, int first, int count, const std::function<int(int)>& lengthOf)
{
    for (int i = first; i < first + count; ++i)
        feed(t, namedLine(i, lengthOf(i)) + "\r\n");
}

// The text of a history line, its stored rows joined.
std::string lineText(const Ubiquitty::Terminal& t, std::uint64_t number)
{
    const auto rows = Ubiquitty::Screen::logicalLineRows(t, number);
    if (!rows)
        return {};
    std::string text;
    for (int index = rows->firstIndex; index < rows->firstIndex + rows->rowCount; ++index) {
        const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&t, index);
        if (!row)
            return {};
        for (int column = 0; column < row->length; ++column) {
            const char32_t codePoint = row->cells[column].codePoint;
            if (codePoint >= 0x20 && codePoint < 0x7F)
                text.push_back(static_cast<char>(codePoint));
        }
    }
    return text;
}

// Every history line in `[oldest, head)` resolves, and its rows join to the
// line that was fed under that number.
bool everyLineNamesItsNumber(const Ubiquitty::Terminal& t, std::uint64_t numberOfFirstFed = 0)
{
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(t);
    for (std::uint64_t number = address.oldest; number < address.head; ++number) {
        char name[16];
        std::snprintf(name, sizeof(name), "L%04d", static_cast<int>(number - numberOfFirstFed));
        if (lineText(t, number).rfind(name, 0) != 0) {
            std::fprintf(stderr, "line %llu reads '%s', not %s\n", static_cast<unsigned long long>(number), lineText(t, number).c_str(), name);
            return false;
        }
    }
    return true;
}

} // namespace

// The append count moves on every reflow, because the rebuild
// clears the store and appends every row again. A count of completed logical
// lines does not, and that is why it, and not the append count, is the address.
TEST(aReflowNeverRenumbersHistory)
{
    auto t = terminalOf(24, 80);
    feedNamedLines(*t, 0, 300, [](int) { return 130; });
    const std::uint64_t headBefore = Ubiquitty::Screen::historyAddress(*t).head;
    const long long totalBefore = Ubiquitty::Screen::scrollbackTotalAppended(t.get());
    CHECK(headBefore > 200u);

    Ubiquitty::resize(*t, 24, 100);
    Ubiquitty::resize(*t, 24, 80);

    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, headBefore);
    CHECK(Ubiquitty::Screen::scrollbackTotalAppended(t.get()) != totalBefore);
    CHECK(everyLineNamesItsNumber(*t));
}

TEST(aReflowToAnotherWidthKeepsEveryNumberOnItsLine)
{
    auto t = terminalOf(24, 80);
    feedNamedLines(*t, 0, 200, [](int i) { return 20 + (i * 37) % 200; });
    const std::uint32_t epochBefore = Ubiquitty::Screen::historyAddress(*t).epoch;
    for (int columns : { 53, 120, 31, 80 }) {
        Ubiquitty::resize(*t, 24, columns);
        CHECK(everyLineNamesItsNumber(*t));
        CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).epoch, epochBefore);
    }
}

TEST(eraseSavedLinesAdvancesTheEpoch)
{
    auto t = terminalOf(24, 80);
    feedNamedLines(*t, 0, 100, [](int) { return 10; });
    const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);
    CHECK(before.head > 0u);

    feed(*t, "\x1b[3J");

    const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(after.epoch, before.epoch + 1);
    CHECK_EQ(after.clearedThrough, before.head);
    CHECK_EQ(after.head, before.head);
    CHECK_EQ(after.oldest, after.head);
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, before.head - 1));

    // The next line to leave the screen takes the old head's number.
    feedNamedLines(*t, 100, 30, [](int) { return 10; });
    const Ubiquitty::Screen::HistoryAddress later = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(later.oldest, before.head);
    CHECK(later.head > before.head);
}

// A clear in an epoch where no line was numbered has nothing to purge, so it
// opens no epoch, however often it repeats; one after a single numbered line
// opens exactly one, and the next clear at that empty prompt opens none.
TEST(aClearWithNothingNumberedOpensNoEpoch)
{
    auto t = terminalOf(2, 20);
    const std::uint32_t epoch = Ubiquitty::Screen::historyAddress(*t).epoch;
    for (int index = 0; index < 10; ++index)
        feed(*t, "\x1b[3J");
    feed(*t, "\x1b"
             "c");
    Ubiquitty::clearBuffer(t.get());
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).epoch, epoch);

    // Rows on screen, none of them history yet: still nothing numbered.
    feed(*t, "prompt$ ");
    feed(*t, "\x1b[3J");
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).epoch, epoch);

    feed(*t, "\r\n\r\n");
    REQUIRE(Ubiquitty::Screen::historyAddress(*t).head == 1u);
    feed(*t, "\x1b[3J");
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).epoch, epoch + 1);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).clearedThrough, 1u);
    for (int index = 0; index < 10; ++index)
        feed(*t, "\x1b[3J");
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).epoch, epoch + 1);
}

TEST(aHardResetAdvancesTheEpochAndNeverRewindsTheHead)
{
    auto t = terminalOf(24, 80);
    feedNamedLines(*t, 0, 100, [](int) { return 10; });
    const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);

    feed(*t, "\x1b"
             "c");

    const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(after.epoch, before.epoch + 1);
    CHECK_EQ(after.head, before.head);
    CHECK_EQ(after.clearedThrough, before.head);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(t.get()), 0);

    feedNamedLines(*t, 0, 60, [](int) { return 10; });
    const Ubiquitty::Screen::HistoryAddress later = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(later.oldest, before.head);
    CHECK(later.head > before.head);
    CHECK(everyLineNamesItsNumber(*t, before.head));
}

// Each destructive verb opens exactly one epoch and never rewinds a number.
// The verbs that destroy nothing — scrolling the prompt to the top, a resize —
// open none.
TEST(everyClearVerbAdvancesTheEpoch)
{
    using Verb = std::pair<const char*, std::function<void(Ubiquitty::Terminal&)>>;
    const std::vector<Verb> destructive = {
        { "ED 3", [](Ubiquitty::Terminal& t) { feed(t, "\x1b[3J"); } },
        { "RIS", [](Ubiquitty::Terminal& t) { feed(t, "\x1b"
                                                      "c"); } },
        { "clearBuffer", [](Ubiquitty::Terminal& t) { Ubiquitty::clearBuffer(&t); } },
        { "clearBufferPreserveInput", [](Ubiquitty::Terminal& t) { Ubiquitty::clearBufferPreserveInput(&t); } },
        { "ClearCommand::Buffer", [](Ubiquitty::Terminal& t) { (void)Ubiquitty::Screen::runClearCommand(Ubiquitty::Screen::ClearCommand::Buffer, &t, nullptr, {}); } },
        { "ClearCommand::Scrollback", [](Ubiquitty::Terminal& t) { (void)Ubiquitty::Screen::runClearCommand(Ubiquitty::Screen::ClearCommand::Scrollback, &t, nullptr, {}); } },
        { "ClearCommand::ToPreviousMark", [](Ubiquitty::Terminal& t) { (void)Ubiquitty::Screen::runClearCommand(Ubiquitty::Screen::ClearCommand::ToPreviousMark, &t, nullptr, {}); } },
        { "dropScrollbackBefore", [](Ubiquitty::Terminal& t) { Ubiquitty::Screen::dropScrollbackBefore(&t, 10); } },
        { "dropRowsBefore in history", [](Ubiquitty::Terminal& t) { Ubiquitty::Screen::dropRowsBefore(&t, 10); } },
        { "dropRowsBefore on screen", [](Ubiquitty::Terminal& t) { Ubiquitty::Screen::dropRowsBefore(&t, Ubiquitty::Screen::scrollbackCount(&t) + 3); } },
        { "dropRowsToPreviousMark", [](Ubiquitty::Terminal& t) { (void)Ubiquitty::Screen::dropRowsToPreviousMark(&t); } },
    };
    for (const auto& [name, verb] : destructive) {
        auto t = terminalOf(12, 40);
        for (int i = 0; i < 20; ++i)
            feed(*t, "\033]133;D;0\007\033]133;A\007$ echo " + std::to_string(i) + "\r\n\033]133;C\007" + std::to_string(i) + "\r\n");
        feed(*t, "\033]133;D;0\007\033]133;A\007$ ");
        const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);
        verb(*t);
        const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
        CHECK_MSG(after.epoch == before.epoch + 1, name);
        CHECK_MSG(after.clearedThrough >= before.head, name);
        CHECK_MSG(after.oldest >= after.clearedThrough, name);
        CHECK_MSG(after.head >= before.head, name);
    }

    const std::vector<Verb> preserving = {
        { "ClearCommand::Screen", [](Ubiquitty::Terminal& t) { (void)Ubiquitty::Screen::runClearCommand(Ubiquitty::Screen::ClearCommand::Screen, &t, nullptr, {}); } },
        { "a width change", [](Ubiquitty::Terminal& t) { Ubiquitty::resize(t, 12, 25); } },
        { "a height change", [](Ubiquitty::Terminal& t) { Ubiquitty::resize(t, 20, 40); } },
        { "ED 2", [](Ubiquitty::Terminal& t) { feed(t, "\x1b[2J"); } },
    };
    for (const auto& [name, verb] : preserving) {
        auto t = terminalOf(12, 40);
        feedNamedLines(*t, 0, 40, [](int) { return 10; });
        feed(*t, "\033]133;A\007$ ");
        const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);
        verb(*t);
        CHECK_MSG(Ubiquitty::Screen::historyAddress(*t).epoch == before.epoch, name);
    }
}

TEST(evictionAtCapacityNeverAdvancesTheEpoch)
{
    auto t = terminalOf(24, 80, 100);
    const std::uint32_t epochBefore = Ubiquitty::Screen::historyAddress(*t).epoch;
    feedNamedLines(*t, 0, 500, [](int) { return 10; });

    const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(after.epoch, epochBefore);
    CHECK_EQ(after.clearedThrough, 0u);
    CHECK(after.oldest > 0u);
    CHECK_EQ(after.head - after.oldest, static_cast<std::uint64_t>(Ubiquitty::Screen::scrollbackCount(t.get())));
    CHECK(everyLineNamesItsNumber(*t));
}

// At capacity a narrower width spends more rows on the same lines, so the
// oldest leave. They leave whole, and no other number moves.
TEST(aReflowAtCapacityAdvancesOldestOnly)
{
    auto t = terminalOf(24, 80, 100);
    feedNamedLines(*t, 0, 80, [](int) { return 130; });
    feedNamedLines(*t, 80, 40, [](int) { return 10; });
    const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);
    CHECK(everyLineNamesItsNumber(*t));

    Ubiquitty::resize(*t, 24, 53);

    const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(after.epoch, before.epoch);
    CHECK_EQ(after.head, before.head);
    CHECK(after.oldest > before.oldest);
    CHECK(everyLineNamesItsNumber(*t));
}

// The lines a clear to the previous mark leaves are renumbered above the old
// head, so `clearedThrough` fences every number the old epoch used.
TEST(aClearToAPreviousMarkRenumbersSurvivorsAboveTheOldHead)
{
    auto t = terminalOf(8, 40);
    for (int i = 0; i < 12; ++i)
        feed(*t, "\033]133;D;0\007\033]133;A\007$ L" + std::to_string(1000 + i) + "\r\n\033]133;C\007" + std::to_string(i) + "\r\n");
    feed(*t, "\033]133;D;0\007\033]133;A\007$ ");
    const Ubiquitty::Screen::HistoryAddress before = Ubiquitty::Screen::historyAddress(*t);
    const std::string oldestText = lineText(*t, before.oldest);
    CHECK(!oldestText.empty());

    CHECK(Ubiquitty::Screen::dropRowsToPreviousMark(t.get()) >= 0);

    const Ubiquitty::Screen::HistoryAddress after = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(after.epoch, before.epoch + 1);
    CHECK_EQ(after.clearedThrough, before.head);
    CHECK_EQ(after.oldest, before.head);
    CHECK(after.head > after.oldest);
    CHECK_STR_EQ(lineText(*t, after.oldest), oldestText);
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, before.oldest));
}

TEST(aTallerResizeRetreatsTheHeadByWholeLines)
{
    // Nine rows: four two-row lines and the cursor's row, so the seam falls
    // between two lines.
    auto t = terminalOf(9, 80);
    feedNamedLines(*t, 0, 40, [](int) { return 130; });
    CHECK(!Ubiquitty::Screen::scrollbackLine(t.get(), Ubiquitty::Screen::scrollbackCount(t.get()) - 1)->wrapped);
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(*t).head;
    const std::string newest = lineText(*t, head - 1);
    CHECK(!newest.empty());

    // The last row of the newest line comes back: the line is no longer
    // history, though its first row is still stored.
    Ubiquitty::resize(*t, 10, 80);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, head - 1);
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, head - 1));
    // Its first row follows; the head stays where a whole line put it.
    Ubiquitty::resize(*t, 11, 80);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, head - 1);
    Ubiquitty::resize(*t, 12, 80);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, head - 2);

    // A shrink pushes the same lines out under the same numbers.
    Ubiquitty::resize(*t, 9, 80);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, head);
    CHECK_STR_EQ(lineText(*t, head - 1), newest);
    CHECK(everyLineNamesItsNumber(*t));
}

TEST(aSoftWrappedLineCountsOnceWhenItsLastRowLeaves)
{
    auto t = terminalOf(5, 10);
    feed(*t, namedLine(0, 25) + "\r\n"); // rows 0-2; the cursor sits on row 3
    feed(*t, "\r\n"); // row 4
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(t.get()), 0);
    feed(*t, "\r\n"); // the first row leaves
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(t.get()), 1);
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, 0u);
    feed(*t, "\r\n"); // the second row leaves
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, 0u);
    feed(*t, "\r\n"); // the last row leaves, and the line is history
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, 1u);
    CHECK_STR_EQ(lineText(*t, 0), namedLine(0, 25));
}

TEST(aLineStraddlingTheSeamIsNotHistory)
{
    auto t = terminalOf(5, 10);
    feed(*t, namedLine(0, 25) + "\r\n\r\n\r\n\r\n");
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(t.get()), 2);
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(*t);
    CHECK_EQ(address.oldest, 0u);
    CHECK_EQ(address.head, 0u);
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, 0));
}

// The rows a straddling line has in history are the open line, from its first
// row to the newest; it closes when its last row leaves, as line `head`.
TEST(theOpenLineIsTheStraddlingLinesRowsInHistory)
{
    auto t = terminalOf(5, 10);
    feed(*t, "ab\r\n");
    CHECK(!Ubiquitty::Screen::openLineRows(*t));
    feed(*t, namedLine(1, 75)); // rows 1-8; the screen shows 4-8
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, 1u);
    std::optional<Ubiquitty::LineStore::LogicalLineRows> open = Ubiquitty::Screen::openLineRows(*t);
    REQUIRE(open.has_value());
    CHECK_EQ(open->firstIndex, 1);
    CHECK_EQ(open->rowCount, 3);
    feed(*t, "\r\n");
    open = Ubiquitty::Screen::openLineRows(*t);
    REQUIRE(open.has_value());
    CHECK_EQ(open->rowCount, 4);
    feed(*t, "\r\n\r\n\r\n\r\n"); // the line's last row leaves
    CHECK(!Ubiquitty::Screen::openLineRows(*t));
    CHECK_EQ(Ubiquitty::Screen::historyAddress(*t).head, 2u);
    CHECK_STR_EQ(lineText(*t, 1), namedLine(1, 75));
}

// A straddling line whose first rows the ring evicted has no open line, as a
// completed one would have no number.
TEST(anOpenLineWhoseFirstRowsWereEvictedIsNone)
{
    auto t = terminalOf(5, 10, 50);
    feed(*t, namedLine(0, 1'000));
    CHECK(Ubiquitty::Screen::scrollbackCount(t.get()) > 0);
    CHECK(!Ubiquitty::Screen::openLineRows(*t));
}

// The index that finds a line's rows survives the ring wrapping over it many
// times, and a line whose first rows were evicted is no longer addressable.
TEST(logicalLineNamesTheRowsOfItsNumberAfterEviction)
{
    auto t = terminalOf(5, 10, 300);
    feedNamedLines(*t, 0, 2000, [](int i) { return 5 + (i * 7) % 40; });
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(*t);
    CHECK(address.oldest > 500u);
    CHECK(everyLineNamesItsNumber(*t));
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, address.oldest - 1));
    CHECK(!Ubiquitty::Screen::logicalLineRows(*t, address.head));

    // The front row is the tail of a line whose start was evicted: that line
    // is not addressable, the next one is.
    const Ubiquitty::Screen::ScrollLine* front = Ubiquitty::Screen::scrollbackLine(t.get(), 0);
    CHECK(front != nullptr);
    const auto oldest = Ubiquitty::Screen::logicalLineRows(*t, address.oldest);
    CHECK(oldest.has_value());
    if (oldest && oldest->firstIndex > 0)
        CHECK(!Ubiquitty::Screen::scrollbackLine(t.get(), oldest->firstIndex - 1)->wrapped);
}

UBIQUITTY_TEST_MAIN()
