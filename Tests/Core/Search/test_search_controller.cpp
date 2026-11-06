// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_search_controller.cpp — the sliced find search
// (Core/Search/SearchController.h, SearchExecutor.h) finds the input
// text, keeps every hit on its text while output keeps arriving,
// publishes nothing from a request a newer one replaced, and answers the same
// pumped as on the worker.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Addressing.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Search/RefillPlan.h"
#include "Core/Search/SearchController.h"
#include "Core/Search/SearchExecutor.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using Ubiquitty::Search::Request;
using Ubiquitty::Search::SearchController;
using Ubiquitty::Search::Status;
using UbiquittyTest::feed;
namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;

namespace {

constexpr std::chrono::milliseconds waitLimit { 60'000 };

std::unique_ptr<Ubiquitty::Terminal> terminalOf(int rows, int columns, int scrollbackLines)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), scrollbackLines);
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

Request literal(std::string pattern)
{
    return Request { .pattern = std::move(pattern) };
}

Request regex(std::string pattern)
{
    return Request { .pattern = std::move(pattern), .form = Ubiquitty::Search::PatternForm::Regex };
}

using Place = std::pair<int, int>;

// Where each hit starts now, as rows and columns.
std::set<Place> hitPlaces(const Ubiquitty::Terminal& t, const SearchController& controller)
{
    std::set<Place> places;
    for (const auto& hit : controller.hits()) {
        if (const auto place = Ubiquitty::Screen::resolvePosition(t, hit.start))
            places.insert({ place->absoluteRow, place->column });
    }
    return places;
}

// Maps matches in the original ASCII input onto its physical rows, without
// reading the terminal's history storage, snapshot scanner or logical positions.
std::set<Place> inputPlaces(const std::vector<std::string>& lines, int columns, const std::string& pattern, Ubiquitty::Search::PatternForm form)
{
    ScrollSearch::CompiledRegex compiled;
    char error[128] {};
    const std::string source = form == Ubiquitty::Search::PatternForm::Literal ? ScrollSearch::escapeLiteral(pattern) : pattern;
    std::set<Place> places;
    const bool didCompile = ScrollSearch::compile(source.c_str(), Ubiquitty::CaseSensitivity::Insensitive, &compiled, error);
    CHECK(didCompile);
    if (!didCompile)
        return {};
    struct Context {
        std::set<Place>& places;
        int columns;
        int firstRow = 0;
    } context { places, columns };
    const auto visit = [](void* raw, std::size_t start, std::size_t length) {
        auto& context = *static_cast<Context*>(raw);
        if (length)
            context.places.emplace(context.firstRow + static_cast<int>(start) / context.columns, static_cast<int>(start) % context.columns);
        return true;
    };
    for (const std::string& line : lines) {
        ScrollSearch::forEachLineMatch(compiled, line, visit, &context);
        context.firstRow += std::max(1, (static_cast<int>(line.size()) + columns - 1) / columns);
    }
    return places;
}

// The text from a place on, continuing across soft wraps.
std::string textAt(const Ubiquitty::Terminal& t, int row, int column, std::size_t count)
{
    std::string text;
    const int total = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;
    while (text.size() < count && row < total) {
        int length = 0;
        const Ubiquitty::Cell* cells = Ubiquitty::Screen::absoluteRowCells(&t, row, &length);
        for (; cells && column < length && text.size() < count; ++column) {
            const char32_t codePoint = cells[column].codePoint;
            if (codePoint >= 0x20 && codePoint < 0x7F)
                text.push_back(static_cast<char>(codePoint));
        }
        if (!Ubiquitty::Screen::absoluteRowWrapped(&t, row))
            break;
        ++row;
        column = 0;
    }
    return text;
}

std::string tokenLine(int index, int length)
{
    std::string line = "<T" + std::to_string(100000 + index) + ">";
    while (static_cast<int>(line.size()) < length)
        line.push_back(static_cast<char>('a' + line.size() % 26));
    return line;
}

} // namespace

TEST(slicedNavigationStartsAtTheLastMatchWithinEachLine)
{
    for (const int appendedRows : { 0, 3 }) {
        for (const char* suffix : { "", " \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" }) {
            auto terminal = terminalOf(2, 30, 100);
            feed(*terminal, std::string("AAA AAA AAA") + suffix);
            for (int row = 0; row < appendedRows; ++row)
                feed(*terminal, "\r\nquiet");
            std::mutex modelLock;
            auto controller = SearchController::make(*terminal, modelLock);
            REQUIRE(controller);
            const auto status = controller->searchAndWait(literal("AAA"), waitLimit);
            CHECK(status.isComplete);
            CHECK_EQ(status.foundCount, 3);
            {
                const std::scoped_lock model(modelLock);
                REQUIRE(controller->hits().size() == 3);
                for (const int column : { 8, 4, 0 }) {
                    const auto active = controller->status().activeIndex;
                    REQUIRE(active >= 0 && active < 3);
                    const auto place = Ubiquitty::Screen::resolvePosition(*terminal, controller->hits()[static_cast<std::size_t>(active)].start);
                    REQUIRE(place);
                    CHECK_EQ(place->column, column);
                    CHECK(controller->step(Ubiquitty::Search::StepDirection::Older));
                }
                CHECK_EQ(controller->status().activeIndex, 0);
                CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
                CHECK_EQ(controller->status().activeIndex, 2);
                const auto last = Ubiquitty::Screen::resolvePosition(*terminal, controller->hits().back().start);
                REQUIRE(last);
                CHECK_EQ(last->column, 0);
            }
            controller->detach();
        }
    }
}

TEST(theSlicedSearchFindsTheOriginalInputMatches)
{
    std::mt19937 random(3);
    const auto pick = [&random](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
    auto t = terminalOf(8, 41, 5000);
    std::mutex modelLock;
    std::vector<std::string> input;
    for (int index = 0; index < 300; ++index) {
        std::string line = "line " + std::to_string(index);
        for (int word = pick(0, 12); word > 0; --word)
            line += pick(0, 9) == 0 ? " needle" : " hay";
        input.push_back(line);
        feed(*t, line + "\r\n");
    }
    input.emplace_back("the open line runs past the margin with a needle in its tail");
    feed(*t, input.back());
    auto controller = SearchController::make(*t, modelLock);
    for (const auto& [pattern, form] : { std::pair { std::string("needle"), Ubiquitty::Search::PatternForm::Literal },
             { "line 3[0-9] ", Ubiquitty::Search::PatternForm::Regex },
             { "hay needle", Ubiquitty::Search::PatternForm::Literal },
             { "e n", Ubiquitty::Search::PatternForm::Literal } }) {
        const Status status = controller->searchAndWait(Request { .pattern = pattern, .form = form }, waitLimit);
        CHECK(status.isComplete);
        const std::set<Place> sliced = hitPlaces(*t, *controller);
        const std::set<Place> expected = inputPlaces(input, t->columns, pattern, form);
        CHECK_MSG(sliced == expected, pattern.c_str());
        CHECK_EQ(status.foundCount, static_cast<std::int64_t>(sliced.size()));
    }
    controller->detach();
}

TEST(aSearchOnTheAlternateScreenReadsOnlyThatScreen)
{
    auto t = terminalOf(6, 30, 1000);
    std::mutex modelLock;
    for (int index = 0; index < 20; ++index)
        feed(*t, "history needle " + std::to_string(index) + "\r\n");
    feed(*t, "\x1b[?1049h");
    feed(*t, "screen needle\r\n");
    auto controller = SearchController::make(*t, modelLock);
    const Status status = controller->searchAndWait(literal("needle"), waitLimit);
    CHECK(status.isComplete);
    CHECK_EQ(status.foundCount, 1);
    controller->detach();
}

TEST(aPatternThatDoesNotCompileSaysSoAndAnEmptyOneClears)
{
    auto t = terminalOf(6, 30, 1000);
    std::mutex modelLock;
    feed(*t, "needle\r\n");
    auto controller = SearchController::make(*t, modelLock);
    CHECK_EQ(controller->searchAndWait(literal("needle"), waitLimit).foundCount, 1);
    const Status invalid = controller->searchAndWait(regex("(unclosed"), waitLimit);
    CHECK(invalid.isPatternInvalid);
    CHECK_EQ(invalid.foundCount, 0);
    const Status cleared = controller->searchAndWait(literal(""), waitLimit);
    CHECK(!cleared.isPatternInvalid);
    CHECK_EQ(cleared.foundCount, 0);
    CHECK(controller->hits().empty());
    controller->detach();
}

TEST(aRepeatedRequestDoesNotRestartTheSearch)
{
    auto t = terminalOf(6, 30, 1000);
    std::mutex modelLock;
    auto controller = SearchController::make(*t, modelLock);
    CHECK(controller->setQuery(literal("needle")));
    CHECK(!controller->setQuery(literal("needle")));
    CHECK(controller->setQuery(literal("hay")));
    controller->detach();
}

// More matches than the store keeps: the count stays exact and says so.
TEST(aCountPastTheStoreIsExactAndMarkedTruncated)
{
    auto t = terminalOf(4, 80, 30000);
    std::mutex modelLock;
    std::string line;
    for (int word = 0; word < 20; ++word)
        line += "x ";
    for (int index = 0; index < 1000; ++index)
        feed(*t, line + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    const Status status = controller->searchAndWait(literal("x"), waitLimit);
    CHECK(status.isComplete);
    CHECK_EQ(status.foundCount, 20000);
    CHECK(status.isTruncated);
    CHECK_EQ(controller->hits().size(), Ubiquitty::Search::maxStoredHits);
    controller->detach();
}

// Stepping walks the hits newest first and wraps, and moves the reveal.
TEST(steppingWalksTheHitsAndWraps)
{
    auto t = terminalOf(6, 30, 1000);
    std::mutex modelLock;
    for (int index = 0; index < 3; ++index)
        feed(*t, "needle " + std::to_string(index) + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    const Status first = controller->searchAndWait(literal("needle"), waitLimit);
    REQUIRE(first.foundCount == 3);
    CHECK_EQ(first.activeIndex, 0);
    {
        const std::lock_guard hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Older));
        CHECK_EQ(controller->status().activeIndex, 1);
        CHECK(controller->status().revealEpoch > first.revealEpoch);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
        CHECK_EQ(controller->status().activeIndex, 2);
    }
    controller->detach();
}

// The worker scans while a writer floods output under the model lock. Every
// hit published resolves onto its token, or names nothing; none lands on
// other text.
TEST(hitsStayOnTheirTextWhileOutputFloods)
{
    auto t = terminalOf(8, 60, 3000);
    std::mutex modelLock;
    for (int index = 0; index < 2000; ++index)
        feed(*t, tokenLine(index, 40) + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    std::atomic<bool> isDone { false };
    std::atomic<bool> hasLanded { false };
    std::thread writer([&] {
        for (int index = 2000; index < 6000; ++index) {
            // The flood evicts every line the search started on, so a writer
            // that ran ahead of the checks would leave no hit to resolve. Hold
            // the flood, before anything is evicted, until one has.
            if (index == 2500) {
                const auto deadline = std::chrono::steady_clock::now() + waitLimit;
                while (!hasLanded.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const std::lock_guard hold(modelLock);
            feed(*t, tokenLine(index, 40) + "\r\n");
            if (index % 997 == 0)
                Ubiquitty::resize(*t, 8, index % 2 ? 47 : 60);
        }
        isDone.store(true);
    });
    controller->setQuery(regex("<T1[0-9]*7>"));
    Ubiquitty::Search::SearchExecutor::shared().schedule(controller);
    int checks = 0;
    int landed = 0;
    while (!isDone.load() || controller->hasWork()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::lock_guard hold(modelLock);
        for (const auto& hit : controller->hits()) {
            const auto place = Ubiquitty::Screen::resolvePosition(*t, hit.start);
            if (!place)
                continue;
            ++landed;
            hasLanded.store(true);
            const std::string text = textAt(*t, place->absoluteRow, place->column, 9);
            if (text.size() < 9 || text[0] != '<' || text[8] != '>' || text[7] != '7') {
                CHECK_MSG(false, ("a hit resolved to '" + text + "'").c_str());
                break;
            }
        }
        ++checks;
    }
    writer.join();
    CHECK(checks > 0);
    CHECK_MSG(landed > 0, "some hits resolved");
    controller->detach();
}

// A new request during a scan abandons the old one: nothing it found is
// published for the new one.
TEST(aNewerRequestReplacesTheRunningOne)
{
    auto t = terminalOf(6, 50, 20000);
    std::mutex modelLock;
    for (int index = 0; index < 8000; ++index)
        feed(*t, "old" + std::to_string(index) + " new" + std::to_string(index) + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    controller->setQuery(literal("old"));
    controller->runSlice();
    controller->runSlice();
    const Status status = controller->searchAndWait(literal("new7"), waitLimit);
    CHECK(status.isComplete);
    CHECK(status.foundCount > 0);
    {
        const std::lock_guard hold(modelLock);
        for (const auto& hit : controller->hits()) {
            const auto place = Ubiquitty::Screen::resolvePosition(*t, hit.start);
            CHECK(place.has_value());
            if (place)
                CHECK(textAt(*t, place->absoluteRow, place->column, 4) == "new7");
        }
    }
    controller->detach();
}

// Run slice by slice on the calling thread, as Web pumps it, the search
// answers as it does on the worker.
TEST(aPumpedSearchAnswersAsTheWorkerDoes)
{
    auto t = terminalOf(6, 50, 5000);
    std::mutex modelLock;
    for (int index = 0; index < 3000; ++index)
        feed(*t, "row " + std::to_string(index) + (index % 7 == 0 ? " needle" : "") + "\r\n");
    auto worked = SearchController::make(*t, modelLock);
    worked->setQuery(literal("needle"));
    Ubiquitty::Search::SearchExecutor::shared().schedule(worked);
    const auto deadline = std::chrono::steady_clock::now() + waitLimit;
    while (worked->hasWork() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto pumped = SearchController::make(*t, modelLock);
    pumped->setQuery(literal("needle"));
    Ubiquitty::Search::SearchExecutor executor(Ubiquitty::Search::SearchExecutor::ExecutionMode::Pumped);
    executor.schedule(pumped);
    CHECK(!pumped->status().isComplete);
    while (executor.pump(std::chrono::steady_clock::now() + std::chrono::milliseconds(4)) && std::chrono::steady_clock::now() < deadline) {
    }
    CHECK(worked->status().isComplete);
    CHECK(pumped->status().isComplete);
    CHECK_EQ(pumped->status().foundCount, worked->status().foundCount);
    CHECK(hitPlaces(*t, *pumped) == hitPlaces(*t, *worked));
    pumped->detach();
    worked->detach();
}

// Detached mid-scan, a controller never reaches its terminal again, even
// with the worker still holding it.
TEST(aDetachedControllerLetsItsTerminalGo)
{
    auto t = terminalOf(6, 50, 20000);
    std::mutex modelLock;
    for (int index = 0; index < 8000; ++index)
        feed(*t, "row " + std::to_string(index) + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    controller->setQuery(literal("row 1"));
    Ubiquitty::Search::SearchExecutor::shared().schedule(controller);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    controller->detach();
    t.reset();
    CHECK(!controller->hasWork());
    CHECK(!controller->runSlice());
}

// The frame reads a sliced search's hits through the search state; the
// segments follow their text when output evicts history and a resize
// rewraps it, where the joined search's segments stayed on their rows.
TEST(publishedHitsStayOnTheirTextThroughEvictionAndResizes)
{
    auto t = terminalOf(6, 40, 200);
    std::mutex modelLock;
    for (int index = 0; index < 150; ++index)
        feed(*t, tokenLine(index, 30) + "\r\n");
    auto controller = SearchController::make(*t, modelLock);
    const Status status = controller->searchAndWait(regex("<T100120>"), waitLimit);
    REQUIRE(status.foundCount == 1);
    ScrollSearch::publishLogicalHits(*t, controller->hits(), status.activeIndex, status.activeEpoch);
    const auto readActive = [&t]() -> std::string {
        const ScrollSearch::Match* active = ScrollSearch::activeMatch(*t);
        return active ? textAt(*t, active->absoluteRow, active->column, 9) : std::string();
    };
    CHECK(readActive() == "<T100120>");
    for (int index = 150; index < 220; ++index)
        feed(*t, tokenLine(index, 30) + "\r\n");
    CHECK_MSG(readActive() == "<T100120>", "eviction moved the rows, not the match");
    Ubiquitty::resize(*t, 6, 17);
    CHECK_MSG(readActive() == "<T100120>", "a narrowing rewrap kept the match on its text");
    Ubiquitty::resize(*t, 9, 50);
    CHECK_MSG(readActive() == "<T100120>", "a widening rewrap kept it too");
    CHECK_EQ(ScrollSearch::matchCount(*t), 1);
    for (int index = 220; index < 500; ++index)
        feed(*t, tokenLine(index, 30) + "\r\n");
    CHECK_MSG(ScrollSearch::activeMatch(*t) == nullptr, "a match whose line was evicted shows nothing");
    controller->detach();
}

TEST(progressKeepsTheFocusClockAndAnEmptyPublicationDisarmsIt)
{
    auto terminal = terminalOf(4, 30, 100);
    std::mutex modelLock;
    feed(*terminal, "needle one\r\nneedle two\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status status = controller->searchAndWait(literal("needle"), waitLimit);
    REQUIRE(status.foundCount == 2);
    {
        const std::scoped_lock hold(modelLock);
        const auto hits = controller->hits();
        ScrollSearch::publishLogicalHits(*terminal, hits.first(1), 0, status.activeEpoch);
        const auto firstEpoch = ScrollSearch::activeMatchEpoch(*terminal);
        CHECK(firstEpoch != 0);
        ScrollSearch::publishLogicalHits(*terminal, hits, 0, status.activeEpoch);
        CHECK_EQ(ScrollSearch::matchCount(*terminal), 2);
        CHECK_EQ(ScrollSearch::activeMatchEpoch(*terminal), firstEpoch);
        ScrollSearch::publishLogicalHits(*terminal, {}, -1, status.activeEpoch + 1);
        CHECK_EQ(ScrollSearch::activeMatchEpoch(*terminal), 0);
        CHECK_EQ(ScrollSearch::matchCount(*terminal), 0);
    }
    controller->detach();
}

TEST(detachWaitsForTheChangedCallbackToReleaseItsContext)
{
    auto terminal = terminalOf(4, 30, 100);
    std::mutex modelLock;
    auto controller = SearchController::make(*terminal, modelLock);
    struct CallbackState {
        std::mutex mutex;
        std::condition_variable changed;
        bool hasEntered = false;
        bool mayReturn = false;
        bool hasDetached = false;
    } state;
    controller->setChangedCallback([](void* context) {
        auto& state = *static_cast<CallbackState*>(context);
        std::unique_lock hold(state.mutex);
        state.hasEntered = true;
        state.changed.notify_all();
        state.changed.wait(hold, [&] { return state.mayReturn; });
    },
        &state);
    controller->setQuery(literal("needle"));
    std::thread worker([&] { controller->runSlice(); });
    {
        std::unique_lock hold(state.mutex);
        state.changed.wait(hold, [&] { return state.hasEntered; });
    }
    std::thread detacher([&] {
        controller->detach();
        const std::scoped_lock hold(state.mutex);
        state.hasDetached = true;
        state.changed.notify_all();
    });
    {
        std::unique_lock hold(state.mutex);
        CHECK(!state.changed.wait_for(hold, std::chrono::milliseconds(50), [&] { return state.hasDetached; }));
        state.mayReturn = true;
        state.changed.notify_all();
    }
    worker.join();
    detacher.join();
    CHECK(state.hasDetached);
    CHECK(!controller->runSlice());
}

TEST(aScopedSearchCountsOnlyItsLogicalLinesAndBoundsItsLabels)
{
    auto terminal = terminalOf(6, 40, 300);
    std::mutex modelLock;
    for (int row = 0; row < 30; ++row)
        feed(*terminal, "needle " + std::to_string(row) + "\r\n");
    const auto first = Ubiquitty::Screen::capturePosition(*terminal, 5, 0);
    const auto last = Ubiquitty::Screen::capturePosition(*terminal, 12, 0);
    REQUIRE(first && last);
    auto request = literal("needle");
    request.scope = Ubiquitty::Search::Scope { *first, *last };
    request.labelLimit = 3;
    auto controller = SearchController::make(*terminal, modelLock);
    CHECK_EQ(controller->searchAndWait(request, waitLimit).foundCount, 8);
    {
        const std::scoped_lock hold(modelLock);
        const auto labels = controller->labels();
        REQUIRE(labels.size() == 3);
        CHECK(labels.front().text == "needle 12");
        CHECK_EQ(labels.front().absoluteRow, 12);
        CHECK(labels.back().text == "needle 10");
    }
    Ubiquitty::resize(*terminal, 6, 6);
    CHECK_EQ(controller->searchAndWait(request, waitLimit).foundCount, 8);
    controller->searchAndWait({}, waitLimit);
    CHECK(controller->labels().empty());
    controller->detach();
}

TEST(searchLabelsKeepOriginalClustersAndRunTextWithinTheirBound)
{
    auto terminal = terminalOf(6, 1200, 300);
    std::mutex modelLock;
    terminal->drawsProportionalText = true;
    feed(*terminal, "needle e\xCC\x81\xCC\xA7 \033]66;p=1:c=8;run text\033\\\r\n");
    feed(*terminal, "needle " + std::string(1100, 'x') + "\r\n");
    auto request = literal("needle");
    request.labelLimit = 2;
    auto controller = SearchController::make(*terminal, modelLock);
    CHECK_EQ(controller->searchAndWait(request, waitLimit).foundCount, 2);
    const auto labels = controller->labels();
    REQUIRE(labels.size() == 2);
    CHECK_EQ(labels.front().text.size(), Ubiquitty::Search::maxLabelCodePoints);
    CHECK_MSG(labels.back().text == "needle e\xCC\x81\xCC\xA7 run text", labels.back().text.c_str());
    request.labelLimit = 0;
    controller->searchAndWait(request, waitLimit);
    CHECK(controller->labels().empty());
    controller->detach();
}

TEST(aDestroyedScopeCannotFindUnrelatedReplacementText)
{
    auto terminal = terminalOf(6, 40, 300);
    std::mutex modelLock;
    feed(*terminal, "needle one\r\nneedle two\r\n");
    const auto first = Ubiquitty::Screen::capturePosition(*terminal, 0, 0);
    const auto last = Ubiquitty::Screen::capturePosition(*terminal, 1, 0);
    REQUIRE(first && last);
    auto request = literal("needle");
    request.scope = Ubiquitty::Search::Scope { *first, *last };
    auto controller = SearchController::make(*terminal, modelLock);
    CHECK_EQ(controller->searchAndWait(request, waitLimit).foundCount, 2);
    feed(*terminal, "\033[2J\033[Hneedle replacement\r\n");
    CHECK_EQ(controller->searchAndWait(request, waitLimit).foundCount, 0);
    controller->detach();
}

TEST(aCompletedSearchRefreshesWithoutRetypingOrMovingItsFocus)
{
    auto terminal = terminalOf(4, 40, 100);
    std::mutex modelLock;
    feed(*terminal, "needle first");
    auto controller = SearchController::make(*terminal, modelLock);
    Ubiquitty::Search::SearchExecutor executor(Ubiquitty::Search::SearchExecutor::ExecutionMode::Pumped);
    controller->setQuery(literal("needle"));
    executor.schedule(controller);
    CHECK(!executor.pump(std::chrono::steady_clock::now() + waitLimit));
    const Status first = controller->status();
    REQUIRE(first.foundCount == 1);
    const auto focused = controller->hits().front().start;
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\r\nneedle second");
        for (int publication = 0; publication < 100; ++publication)
            controller->noteModelChanged(*terminal);
    }
    CHECK(controller->hasWork());
    CHECK(!executor.pump(std::chrono::steady_clock::now() + waitLimit));
    const Status next = controller->status();
    CHECK_EQ(next.generation, first.generation);
    CHECK_EQ(next.foundCount, 2);
    CHECK_EQ(next.revealEpoch, first.revealEpoch);
    CHECK_EQ(next.activeEpoch, first.activeEpoch);
    REQUIRE(next.activeIndex >= 0);
    CHECK(controller->hits()[static_cast<std::size_t>(next.activeIndex)].start == focused);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[2;1H\033[2K\033[3;1Hneedle moved");
        controller->noteModelChanged(*terminal);
    }
    CHECK(!executor.pump(std::chrono::steady_clock::now() + waitLimit));
    const Status moved = controller->status();
    CHECK_EQ(moved.foundCount, next.foundCount);
    CHECK_EQ(moved.revealEpoch, next.revealEpoch);
    CHECK(moved.screenGeneration > next.screenGeneration);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[3;1H\033[2Kquiet");
        controller->noteModelChanged(*terminal);
    }
    CHECK(!executor.pump(std::chrono::steady_clock::now() + waitLimit));
    CHECK_EQ(controller->status().foundCount, 1);
    CHECK_EQ(controller->status().revealEpoch, first.revealEpoch);
    {
        const std::scoped_lock hold(modelLock);
        controller->noteModelChanged(*terminal);
    }
    CHECK(!controller->hasWork());
    controller->detach();
}

// The focus clock is what the pill's arrival and the reveal are read off, and
// it moves only when the person's search does. Output elsewhere leaves it. So
// does a program repainting the focused match's own line in place, an erase
// and the same text written back the way a full-screen program redraws on
// every keystroke: the line keeps its name, only its erase revision moves, and
// the rescan finds the match where it stood. Output that scrolls the match
// into history leaves it too. A step moves it, and so does a new query.
TEST(onlyTheSearchItselfMovesTheFocusClock)
{
    auto terminal = terminalOf(4, 40, 100);
    std::mutex modelLock;
    feed(*terminal, "o general-purpose\r\no general-tools\r\n> ");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("gene"), waitLimit);
    REQUIRE(initial.foundCount == 2);
    REQUIRE(initial.activeIndex == 0);
    REQUIRE(initial.activeEpoch != 0);

    // The focused hit's place and the text there, read under the lock.
    const auto focusedText = [&]() -> std::string {
        const std::scoped_lock hold(modelLock);
        const Status status = controller->status();
        if (status.activeIndex < 0 || static_cast<std::size_t>(status.activeIndex) >= controller->hits().size())
            return {};
        const auto place = Ubiquitty::Screen::resolvePosition(*terminal, controller->hits()[static_cast<std::size_t>(status.activeIndex)].start);
        if (!place)
            return {};
        return std::to_string(place->absoluteRow) + ":" + std::to_string(place->column) + " " + textAt(*terminal, place->absoluteRow, place->column, 13);
    };
    const std::string initialFocus = focusedText();
    CHECK_STR_EQ(initialFocus, "1:2 general-tools");

    // Writes the bytes as program output and runs the rescan they owe.
    const auto output = [&](std::string_view bytes) {
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, bytes);
            controller->noteModelChanged(*terminal);
        }
        while (controller->runSlice()) {
        }
    };

    output("x");
    const Status typed = controller->status();
    CHECK(typed.screenGeneration > initial.screenGeneration);
    CHECK_EQ(typed.foundCount, 2);
    CHECK_EQ(typed.activeEpoch, initial.activeEpoch);
    CHECK_EQ(typed.revealEpoch, initial.revealEpoch);

    output("\033[1;1H\033[2Ko general-purpose\033[2;1H\033[2Ko general-tools\033[3;1H\033[2K> x");
    const Status repainted = controller->status();
    CHECK(repainted.screenGeneration > typed.screenGeneration);
    CHECK_EQ(repainted.foundCount, 2);
    CHECK_EQ(repainted.activeIndex, 0);
    CHECK_EQ(repainted.activeEpoch, initial.activeEpoch);
    CHECK_EQ(repainted.revealEpoch, initial.revealEpoch);
    CHECK_STR_EQ(focusedText(), initialFocus);

    output("\r\nmore\r\nmore\r\nmore");
    const Status scrolled = controller->status();
    CHECK_EQ(scrolled.foundCount, 2);
    CHECK_EQ(scrolled.activeIndex, 0);
    CHECK_EQ(scrolled.activeEpoch, initial.activeEpoch);
    CHECK_EQ(scrolled.revealEpoch, initial.revealEpoch);
    CHECK_STR_EQ(focusedText(), initialFocus);

    {
        const std::scoped_lock hold(modelLock);
        REQUIRE(controller->step(Ubiquitty::Search::StepDirection::Older));
    }
    const Status stepped = controller->status();
    CHECK_EQ(stepped.activeIndex, 1);
    CHECK_NE(stepped.activeEpoch, scrolled.activeEpoch);
    CHECK_NE(stepped.revealEpoch, scrolled.revealEpoch);

    const Status requeried = controller->searchAndWait(literal("general"), waitLimit);
    CHECK_EQ(requeried.foundCount, 2);
    CHECK_NE(requeried.activeEpoch, stepped.activeEpoch);
    controller->detach();
}

TEST(screenHitsPromoteToHistoryExactlyOnce)
{
    auto terminal = terminalOf(2, 40, 100);
    std::mutex modelLock;
    feed(*terminal, "needle first\r\nquiet");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("needle"), waitLimit);
    REQUIRE(initial.foundCount == 1);
    for (int line = 0; line < 3; ++line) {
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, "\r\nquiet");
            controller->noteModelChanged(*terminal);
        }
        while (controller->runSlice()) {
        }
        CHECK_EQ(controller->status().foundCount, 1);
        CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
        REQUIRE(controller->hits().size() == 1);
        CHECK_EQ(controller->hits().front().start.line.value, 0);
        CHECK_EQ(controller->status().activeIndex, 0);
    }
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\r\nneedle second");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().foundCount, 2);
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
    controller->detach();
}

TEST(screenRefreshRecountsMatchesBeyondTheStoredHitCapacity)
{
    auto terminal = terminalOf(100, 200, 100);
    std::mutex modelLock;
    for (int row = 0; row < 99; ++row)
        feed(*terminal, std::string(199, 'a') + "\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    CHECK_EQ(initial.foundCount, 99 * 199);
    CHECK(initial.isTruncated);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[H\033[2K");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().foundCount, 98 * 199);
    CHECK(controller->status().isTruncated);
    controller->detach();
}

TEST(aJoinedScreenLineKeepsItsFocusThroughHistoryPromotion)
{
    for (const bool shouldRefreshBeforePromotion : { false, true }) {
        auto terminal = terminalOf(2, 10, 100);
        std::mutex modelLock;
        feed(*terminal, "left\r\nneedle");
        auto controller = SearchController::make(*terminal, modelLock);
        const Status initial = controller->searchAndWait(literal("needle"), waitLimit);
        REQUIRE(initial.foundCount == 1);
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, "\033[1;1Habcdefghijn");
            controller->noteModelChanged(*terminal);
        }
        if (shouldRefreshBeforePromotion) {
            while (controller->runSlice()) {
            }
            CHECK_EQ(controller->status().foundCount, 1);
            CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
        }
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, "\033[2;7H\r\nquiet\r\nquiet");
            controller->noteModelChanged(*terminal);
        }
        while (controller->runSlice()) {
        }
        const auto status = controller->status();
        CHECK_EQ(status.foundCount, 1);
        CHECK_EQ(status.revealEpoch, initial.revealEpoch);
        REQUIRE(status.activeIndex >= 0);
        const auto& hit = controller->hits()[static_cast<std::size_t>(status.activeIndex)];
        const auto place = Ubiquitty::Screen::resolvePosition(*terminal, hit.start);
        REQUIRE(place);
        CHECK(textAt(*terminal, place->absoluteRow, place->column, 6) == "needle");
        controller->detach();
    }
}

TEST(historyExtensionAndEvictionKeepExactCountsBeyondStoredHits)
{
    auto terminal = terminalOf(6, 200, 700);
    std::mutex modelLock;
    for (int line = 0; line < 1000; ++line)
        feed(*terminal, std::string(150 + line % 50, 'a') + "\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    REQUIRE(initial.foundCount > static_cast<std::int64_t>(Ubiquitty::Search::maxStoredHits));
    const auto bruteForceCount = [&] {
        std::int64_t count = 0;
        const int rows = Ubiquitty::Screen::scrollbackCount(terminal.get()) + terminal->rows;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < terminal->columns; ++column) {
                const auto* cell = Ubiquitty::Screen::absoluteCell(terminal.get(), row, column);
                count += cell && cell->codePoint == 'a';
            }
        }
        return count;
    };
    CHECK_EQ(initial.foundCount, bruteForceCount());
    for (int burst = 0; burst < 12; ++burst) {
        {
            const std::scoped_lock hold(modelLock);
            for (int line = 0; line < 100; ++line)
                feed(*terminal, burst < 3 ? std::string(line % 80, 'a') + "\r\n" : "quiet\r\n");
            controller->noteModelChanged(*terminal);
        }
        const auto deadline = std::chrono::steady_clock::now() + waitLimit;
        while (controller->hasWork() && std::chrono::steady_clock::now() < deadline)
            controller->runSlice();
        const Status status = controller->status();
        CHECK(!controller->hasWork());
        CHECK(status.isComplete);
        CHECK_EQ(status.generation, initial.generation);
        CHECK_EQ(status.foundCount, bruteForceCount());
        CHECK(controller->hits().size() <= Ubiquitty::Search::maxStoredHits);
        CHECK_EQ(status.isTruncated, status.foundCount > static_cast<std::int64_t>(controller->hits().size()));
    }
    CHECK_EQ(controller->status().foundCount, 0);
    CHECK(controller->hits().empty());
    controller->detach();
}

TEST(anOwnedScreenScanRefillsWithinALineBeyondStoredCapacity)
{
    auto terminal = terminalOf(2, 200, 1000);
    feed(*terminal, std::string(20000, 'a'));
    std::string error;
    const auto compiled = ScrollSearch::compilePattern("a", Ubiquitty::CaseSensitivity::Insensitive, Ubiquitty::Unicode::NormalizationForm::None, error);
    REQUIRE(compiled);
    Ubiquitty::Search::ScreenScan scan;
    scan.begin(*terminal, std::nullopt);
    while (!scan.isCaptured())
        REQUIRE(scan.capturePrefix(*terminal));
    const auto finish = [&] {
        int slices = 0;
        bool hasMore = false;
        do {
            ++slices;
            hasMore = scan.step(*compiled, Ubiquitty::Unicode::NormalizationForm::NFC);
        } while (hasMore);
        return slices;
    };
    const int initialSlices = finish();
    REQUIRE(initialSlices > 1);
    REQUIRE(scan.lines().size() == 1);
    CHECK_EQ(scan.lines().front().count, 20000);
    CHECK_EQ(scan.hits().size(), Ubiquitty::Search::maxStoredHits);
    scan.replay(16380, 16400);
    CHECK_EQ(finish(), initialSlices);
    REQUIRE(scan.hits().size() == 20);
    CHECK_EQ(scan.hits().front().start.cell, 3619u);
    CHECK_EQ(scan.hits().back().start.cell, 3600u);
    CHECK_EQ(scan.lines().front().count, 20000);
    CHECK_EQ(scan.skippedCount(), 16380);
    const auto wanted = scan.hits().back();
    const auto boundary = wanted.start;
    scan.locate(wanted);
    CHECK_EQ(finish(), initialSlices);
    REQUIRE(scan.locatedOrdinal());
    CHECK_EQ(*scan.locatedOrdinal(), 16399u);
    CHECK(scan.hits().empty());
    scan.replay(0, 10);
    CHECK_EQ(finish(), initialSlices);
    REQUIRE(scan.hits().size() == 10);
    CHECK_EQ(scan.hits().front().start.cell, 19999u);
    CHECK_EQ(scan.hits().back().start.cell, 19990u);
    CHECK_EQ(scan.lines().front().count, 20000);
    scan.begin(*terminal, std::nullopt, boundary);
    while (!scan.isCaptured())
        REQUIRE(scan.capturePrefix(*terminal));
    CHECK_EQ(finish(), initialSlices);
    REQUIRE(scan.hits().size() == 3601);
    CHECK_EQ(scan.hits().front().start.cell, 3600u);
    CHECK_EQ(scan.hits().back().start.cell, 0u);
    CHECK_EQ(scan.lines().front().count, 20000);
    CHECK_EQ(scan.skippedCount(), 16399);
}

TEST(aRefillPlanVisitsRetainedCountsInBoundedSlices)
{
    Ubiquitty::Search::LineCounts counts;
    counts.prepare(500, 10500);
    counts.replace(1000, 20000);
    counts.replace(500, 2);
    Ubiquitty::Search::RefillPlan plan;
    plan.begin(counts, 19998, 20002, 0);
    int slices = 1;
    while (plan.step(16))
        ++slices;
    CHECK_EQ(slices, 625);
    REQUIRE(plan.lines().size() == 2);
    CHECK_EQ(plan.lines().front().number, 1000u);
    CHECK_EQ(plan.lines().front().newerCount, 0u);
    CHECK_EQ(plan.lines().front().count, 20000u);
    CHECK_EQ(plan.lines().back().number, 500u);
    CHECK_EQ(plan.lines().back().newerCount, 20000u);
    plan.begin(counts, 0, 10, 20);
    CHECK(!plan.step(1));
    CHECK(plan.lines().empty());
    plan.begin(counts, 0, 10, 8);
    while (plan.step(16)) {
    }
    REQUIRE(plan.lines().size() == 1);
    CHECK_EQ(plan.lines().front().number, 1000u);
    CHECK_EQ(plan.lines().front().newerCount, 8u);
    CHECK_EQ(plan.lines().front().count, 20000u);
    plan.beginLine(counts, 500, 8);
    slices = 1;
    while (plan.step(16))
        ++slices;
    CHECK_EQ(slices, 625);
    REQUIRE(plan.lines().size() == 1);
    CHECK_EQ(plan.lines().front().number, 500u);
    CHECK_EQ(plan.lines().front().newerCount, 20008u);
    plan.beginLine(counts, 499, 8);
    CHECK(!plan.step(16));
    CHECK(plan.lines().empty());
}

TEST(refillNavigatesAcrossHistoryAndWithinOneLongLine)
{
    for (const bool isWrapped : { false, true }) {
        auto terminal = terminalOf(2, 200, 30000);
        std::mutex modelLock;
        if (isWrapped)
            feed(*terminal, std::string(20000, 'a'));
        else {
            for (int line = 0; line < 20000; ++line)
                feed(*terminal, "a\r\n");
        }
        auto controller = SearchController::make(*terminal, modelLock);
        const Status initial = controller->searchAndWait(literal("a"), waitLimit);
        REQUIRE(initial.foundCount == 20000);
        REQUIRE(initial.isComplete);
        const auto finish = [&] {
            int slices = 0;
            while (controller->hasWork() && slices < 10000) {
                controller->runSlice();
                ++slices;
            }
            CHECK(!controller->hasWork());
            return slices;
        };
        {
            const std::scoped_lock hold(modelLock);
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
            for (int step = 0; step < 3615; ++step)
                CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
            CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
        }
        CHECK(finish() > 1);
        CHECK_EQ(controller->status().activeIndex, 16384);
        CHECK_EQ(controller->status().foundCount, 20000);
        CHECK(controller->status().isComplete);
        CHECK_EQ(controller->hits().size(), Ubiquitty::Search::maxStoredHits);
        const auto* active = ScrollSearch::activeMatch(*terminal);
        REQUIRE(active);
        CHECK_EQ(active->absoluteRow, isWrapped ? 18 : 3615);
        CHECK_EQ(active->column, isWrapped ? 15 : 0);
        {
            const std::scoped_lock hold(modelLock);
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Older));
        }
        CHECK(finish() > 1);
        CHECK_EQ(controller->status().activeIndex, 16385);
        CHECK_EQ(controller->status().foundCount, 20000);
        {
            const std::scoped_lock hold(modelLock);
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
        }
        CHECK(finish() > 1);
        CHECK_EQ(controller->status().activeIndex, 16384);
        CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch + 3);
        controller->detach();
    }
}

TEST(clearingOrReplacingAQueryDiscardsAnInFlightRefill)
{
    auto terminal = terminalOf(2, 200, 1000);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a'));
    auto controller = SearchController::make(*terminal, modelLock);
    for (const std::string& replacement : { std::string {}, std::string { "absent" } }) {
        REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
        {
            const std::scoped_lock hold(modelLock);
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
        }
        for (int slice = 0; slice < 10; ++slice)
            CHECK(controller->runSlice());
        const Status cleared = controller->searchAndWait(literal(replacement), waitLimit);
        CHECK(cleared.isComplete);
        CHECK_EQ(cleared.foundCount, 0);
        CHECK(controller->hits().empty());
        CHECK(!controller->hasWork());
        CHECK(!controller->runSlice());
        CHECK(controller->status() == cleared);
    }
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    CHECK(controller->runSlice());
    controller->detach();
    CHECK(!controller->hasWork());
    CHECK(!controller->runSlice());
}

TEST(anOlderHistoryWindowKeepsItsFocusThroughExtensionAndEviction)
{
    auto terminal = terminalOf(2, 40, 21000);
    std::mutex modelLock;
    for (int line = 0; line < 20000; ++line)
        feed(*terminal, "a\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        for (int step = 0; step < 3616; ++step)
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    const Status before = controller->status();
    REQUIRE(before.activeIndex == 16384);
    const auto focused = controller->hits().back().start;
    {
        const std::scoped_lock hold(modelLock);
        for (int line = 0; line < 2500; ++line)
            feed(*terminal, "a\r\n");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    const Status after = controller->status();
    CHECK(after.isComplete);
    CHECK_EQ(after.foundCount, Ubiquitty::Screen::scrollbackCount(terminal.get()) + 1);
    CHECK_EQ(after.activeIndex, before.activeIndex + 2500);
    CHECK_EQ(after.revealEpoch, before.revealEpoch);
    REQUIRE(!controller->hits().empty());
    CHECK(controller->hits().back().start == focused);
    controller->detach();
}

TEST(anOlderOpenLineWindowKeepsItsFocusThroughHistoryPromotion)
{
    auto terminal = terminalOf(2, 200, 1000);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a'));
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        for (int step = 0; step < 3616; ++step)
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    const Status before = controller->status();
    REQUIRE(before.activeIndex == 16384);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, std::string(1000, 'a'));
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 21000);
    CHECK_EQ(controller->status().activeIndex, 17384);
    CHECK_EQ(controller->status().revealEpoch, before.revealEpoch);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\r\nquiet\r\nquiet\r\n");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 21000);
    CHECK_EQ(controller->status().activeIndex, 17384);
    CHECK_EQ(controller->status().revealEpoch, before.revealEpoch);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 18);
    CHECK_EQ(active->column, 15);
    controller->detach();
}

TEST(anEvictedNavigationWindowRefillsFromRetainedText)
{
    auto terminal = terminalOf(2, 40, 20000);
    std::mutex modelLock;
    for (int line = 0; line < 20000; ++line)
        feed(*terminal, "a\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    REQUIRE(controller->status().activeIndex == 19999);
    {
        const std::scoped_lock hold(modelLock);
        for (int line = 0; line < 30000; ++line)
            feed(*terminal, "a\r\n");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK(controller->status().isComplete);
    CHECK_EQ(controller->status().foundCount, Ubiquitty::Screen::scrollbackCount(terminal.get()) + 1);
    CHECK_EQ(controller->status().activeIndex, 0);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, Ubiquitty::Screen::scrollbackCount(terminal.get()));
    controller->detach();
}

TEST(refillFindsAnOldMatchInACompletedLineWithPooledText)
{
    auto terminal = terminalOf(2, 200, 1000);
    std::mutex modelLock;
    std::string text;
    for (int match = 0; match < 20000; ++match)
        text += "e\xCC\x81";
    feed(*terminal, text + "\r\nquiet\r\nquiet\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    auto request = literal("\xC3\xA9");
    const auto first = Ubiquitty::Screen::capturePosition(*terminal, 0, 0);
    const auto last = Ubiquitty::Screen::capturePosition(*terminal, 99, 199);
    REQUIRE(first && last);
    request.scope = Ubiquitty::Search::Scope { *first, *last };
    REQUIRE(controller->searchAndWait(request, waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 20000);
    CHECK_EQ(controller->status().activeIndex, 19999);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 0);
    CHECK_EQ(active->column, 0);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Older));
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().activeIndex, 0);
    active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 99);
    CHECK_EQ(active->column, 199);
    controller->detach();
}

TEST(aHistoryRefillCanFinishWhileNewOutputContinues)
{
    auto terminal = terminalOf(2, 40, 30000);
    std::mutex modelLock;
    for (int line = 0; line < 20000; ++line)
        feed(*terminal, "a\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    REQUIRE(initial.foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    CHECK(controller->runSlice());
    int appended = 0;
    for (; appended < 1000 && controller->status().revealEpoch == initial.revealEpoch; ++appended) {
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, "a\r\n");
            controller->noteModelChanged(*terminal);
        }
        controller->runSlice();
    }
    CHECK(appended < 1000);
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch + 1);
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 20000 + appended);
    CHECK_EQ(controller->status().activeIndex, 19999 + appended);
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch + 1);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 0);
    controller->detach();
}

TEST(partialEvictionDropsACompletedLinesCountAndNavigationWindow)
{
    auto terminal = terminalOf(2, 200, 150);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a') + "\r\nquiet\r\nquiet\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    REQUIRE(controller->status().activeIndex == 19999);
    {
        const std::scoped_lock hold(modelLock);
        for (int line = 0; line < 55; ++line)
            feed(*terminal, "quiet\r\n");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK(controller->status().isComplete);
    CHECK_EQ(controller->status().foundCount, 0);
    CHECK_EQ(controller->status().activeIndex, -1);
    CHECK(controller->hits().empty());
    const Status maintained = controller->status();
    CHECK_EQ(controller->searchAndWait(literal("a"), waitLimit).foundCount, maintained.foundCount);
    controller->detach();
}

TEST(newMatchesBeyondCapacityPreserveTheSelectedText)
{
    for (const bool isWrapped : { false, true }) {
        auto terminal = terminalOf(2, 200, 30000);
        std::mutex modelLock;
        feed(*terminal, "a");
        auto controller = SearchController::make(*terminal, modelLock);
        const Status initial = controller->searchAndWait(literal("a"), waitLimit);
        REQUIRE(initial.foundCount == 1);
        {
            const std::scoped_lock hold(modelLock);
            if (isWrapped)
                feed(*terminal, std::string(20000, 'a'));
            else {
                for (int line = 0; line < 20000; ++line)
                    feed(*terminal, "\r\na");
            }
            controller->noteModelChanged(*terminal);
        }
        for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
            controller->runSlice();
        CHECK(!controller->hasWork());
        CHECK_EQ(controller->status().foundCount, 20001);
        CHECK_EQ(controller->status().activeIndex, 20000);
        CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
        const auto* active = ScrollSearch::activeMatch(*terminal);
        REQUIRE(active);
        CHECK_EQ(active->absoluteRow, 0);
        CHECK_EQ(active->column, 0);
        controller->detach();
    }
}

TEST(anOlderNavigationWindowRetainsItsFocusAcrossReflow)
{
    auto terminal = terminalOf(2, 200, 1000);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a'));
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        for (int step = 0; step < 3616; ++step)
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    const Status before = controller->status();
    REQUIRE(before.activeIndex == 16384);
    {
        const std::scoped_lock hold(modelLock);
        Ubiquitty::resize(*terminal, 2, 100);
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 20000);
    CHECK_EQ(controller->status().activeIndex, 16384);
    CHECK_EQ(controller->status().revealEpoch, before.revealEpoch);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 36);
    CHECK_EQ(active->column, 15);
    controller->detach();
}

TEST(destroyingAWindowBoundaryPreservesAnotherRetainedMatch)
{
    auto terminal = terminalOf(200, 20, 30000);
    std::mutex modelLock;
    for (int line = 0; line < 20000; ++line)
        feed(*terminal, "a\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        for (int step = 0; step < 3616; ++step)
            CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    while (controller->runSlice()) {
    }
    const Status before = controller->status();
    REQUIRE(before.activeIndex == 16384);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[198;1H\033[2K");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().foundCount, 19999);
    CHECK_EQ(controller->status().activeIndex, 16383);
    CHECK_EQ(controller->status().revealEpoch, before.revealEpoch);
    const auto* active = ScrollSearch::activeMatch(*terminal);
    REQUIRE(active);
    CHECK_EQ(active->absoluteRow, 3615);
    controller->detach();
}

TEST(refillRestartsAfterOutputEvictsItsCapturedText)
{
    auto terminal = terminalOf(2, 200, 21000);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a'));
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    REQUIRE(initial.foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    for (int slice = 0; slice < 10; ++slice)
        CHECK(controller->runSlice());
    {
        const std::scoped_lock hold(modelLock);
        for (int line = 0; line < 30000; ++line)
            feed(*terminal, "\r\na");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK(controller->status().isComplete);
    CHECK_EQ(controller->status().foundCount, Ubiquitty::Screen::scrollbackCount(terminal.get()) + 2);
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch + 1);
    REQUIRE(controller->status().activeIndex >= 0);
    CHECK(controller->status().activeIndex < controller->status().foundCount);
    for (const auto& hit : controller->hits())
        CHECK(Ubiquitty::Screen::resolvePosition(*terminal, hit.start).has_value());
    controller->detach();
}

TEST(navigationCanReplaceARefillDuringScreenReplay)
{
    auto terminal = terminalOf(2, 200, 1000);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a'));
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    REQUIRE(initial.foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    for (int slice = 0; slice < 3; ++slice)
        CHECK(controller->runSlice());
    {
        const std::scoped_lock hold(modelLock);
        CHECK(controller->step(Ubiquitty::Search::StepDirection::Newer));
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK_EQ(controller->status().activeIndex, 19998);
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch + 1);
    controller->detach();
}

TEST(historyExtensionReplacesAPromotedLineWithNoRemainingMatches)
{
    auto terminal = terminalOf(2, 20, 100);
    std::mutex modelLock;
    feed(*terminal, "aaaa");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("a"), waitLimit);
    REQUIRE(initial.foundCount == 4);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\rbbbb\r\nquiet\r\nquiet\r\n");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().generation, initial.generation);
    CHECK_EQ(controller->status().foundCount, 0);
    CHECK(controller->hits().empty());
    controller->detach();
}

TEST(aDestroyedScopeDropsItsPreviouslyCountedHistory)
{
    auto terminal = terminalOf(4, 40, 1000);
    std::mutex modelLock;
    for (int line = 0; line < 100; ++line)
        feed(*terminal, "needle\r\n");
    const auto first = Ubiquitty::Screen::capturePosition(*terminal, 0, 0);
    const auto last = Ubiquitty::Screen::capturePosition(*terminal, Ubiquitty::Screen::scrollbackCount(terminal.get()), 0);
    REQUIRE(first && last);
    auto request = literal("needle");
    request.scope = Ubiquitty::Search::Scope { *first, *last };
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(request, waitLimit).foundCount > 10);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[2J");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().foundCount, 0);
    CHECK(controller->hits().empty());
    controller->detach();
}

TEST(aScopeDestroyedDuringCaptureCannotPublishItsOtherLines)
{
    auto terminal = terminalOf(4, 40, 100);
    std::mutex modelLock;
    feed(*terminal, "needle one\r\nneedle two\r\nneedle three");
    const auto first = Ubiquitty::Screen::capturePosition(*terminal, 0, 0);
    const auto last = Ubiquitty::Screen::capturePosition(*terminal, 2, 0);
    REQUIRE(first && last);
    auto request = literal("needle");
    request.scope = Ubiquitty::Search::Scope { *first, *last };
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(request, waitLimit);
    REQUIRE(initial.foundCount == 3);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[4;1Hnoise");
        controller->noteModelChanged(*terminal);
    }
    CHECK(controller->runSlice());
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[1;1H\033[2K");
        controller->noteModelChanged(*terminal);
    }
    while (controller->status().screenGeneration == initial.screenGeneration && controller->hasWork())
        controller->runSlice();
    CHECK_EQ(controller->status().foundCount, 0);
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().foundCount, 0);
    controller->detach();
}

TEST(aPromotedFocusDoesNotDependOnTheLiveScreenPinBudget)
{
    auto terminal = terminalOf(2, 40, 5000);
    std::mutex modelLock;
    feed(*terminal, "needle first\r\nquiet");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("needle"), waitLimit);
    REQUIRE(initial.foundCount == 1);
    for (int line = 0; line < 2200; ++line) {
        {
            const std::scoped_lock hold(modelLock);
            feed(*terminal, "\r\nquiet");
            controller->noteModelChanged(*terminal);
        }
        while (controller->runSlice()) {
        }
    }
    const auto status = controller->status();
    CHECK_EQ(status.foundCount, 1);
    CHECK_EQ(status.revealEpoch, initial.revealEpoch);
    REQUIRE(status.activeIndex >= 0);
    const auto& hit = controller->hits()[static_cast<std::size_t>(status.activeIndex)];
    const auto place = Ubiquitty::Screen::resolvePosition(*terminal, hit.start);
    REQUIRE(place);
    CHECK(textAt(*terminal, place->absoluteRow, place->column, 6) == "needle");
    controller->detach();
}

TEST(aScreenCopyOwnsItsPooledTextAndSlicesItsStoredPrefix)
{
    auto terminal = terminalOf(3, 40, 2000);
    terminal->drawsProportionalText = true;
    feed(*terminal, std::string(12000, 'x') + "needle e\xCC\x81\xCC\xA7 \033]66;p=1:c=8;run text\033\\");
    Ubiquitty::Search::ScreenScan scan;
    scan.begin(*terminal, std::nullopt);
    CHECK(!scan.isCaptured());
    CHECK(scan.capturePrefix(*terminal));
    CHECK(!scan.isCaptured());
    while (!scan.isCaptured())
        REQUIRE(scan.capturePrefix(*terminal));
    terminal.reset();
    std::string error;
    const auto compiled = ScrollSearch::compilePattern("needle|run text", Ubiquitty::CaseSensitivity::Sensitive, Ubiquitty::Unicode::NormalizationForm::NFC, error);
    REQUIRE(compiled);
    int slices = 0;
    while (scan.step(*compiled, Ubiquitty::Unicode::NormalizationForm::NFC))
        ++slices;
    CHECK(slices > 0);
    REQUIRE(scan.lines().size() == 1);
    CHECK_EQ(scan.lines().front().count, 2);
    CHECK_EQ(scan.hits().size(), 2u);
}

TEST(aWorkerWakesForModelChangesAfterItsQueryCompleted)
{
    auto terminal = terminalOf(4, 40, 100);
    std::mutex modelLock;
    Ubiquitty::Search::SearchExecutor executor;
    auto controller = SearchController::make(*terminal, modelLock, executor);
    controller->setQuery(literal("needle"));
    executor.schedule(controller);
    const auto deadline = std::chrono::steady_clock::now() + waitLimit;
    while (controller->hasWork() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(controller->status().isComplete);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "needle");
        controller->noteModelChanged(*terminal);
    }
    while (controller->hasWork() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK_EQ(controller->status().foundCount, 1);
    CHECK(!controller->hasWork());
    controller->detach();
}

TEST(aNewQueryDiscardsAnUnpublishedScreenRefresh)
{
    auto terminal = terminalOf(100, 200, 100);
    std::mutex modelLock;
    feed(*terminal, "needle");
    auto controller = SearchController::make(*terminal, modelLock);
    CHECK_EQ(controller->searchAndWait(literal("needle"), waitLimit).foundCount, 1);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\r\nneedle replacement");
        controller->noteModelChanged(*terminal);
    }
    CHECK(controller->runSlice());
    const Status cleared = controller->searchAndWait({}, waitLimit);
    CHECK(cleared.isComplete);
    CHECK_EQ(cleared.foundCount, 0);
    CHECK(!controller->runSlice());
    CHECK(controller->hits().empty());
    controller->detach();
}

TEST(bufferChangesRefreshTheSameQueryAndResizeKeepsLogicalFocus)
{
    auto terminal = terminalOf(4, 40, 100);
    std::mutex modelLock;
    feed(*terminal, "needle first\r\nneedle second");
    auto controller = SearchController::make(*terminal, modelLock);
    const Status initial = controller->searchAndWait(literal("needle"), waitLimit);
    REQUIRE(initial.foundCount == 2);
    const auto focus = controller->hits().front().start;
    {
        const std::scoped_lock hold(modelLock);
        Ubiquitty::resize(*terminal, 4, 20);
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().revealEpoch, initial.revealEpoch);
    REQUIRE(controller->status().activeIndex >= 0);
    CHECK(controller->hits()[static_cast<std::size_t>(controller->status().activeIndex)].start == focus);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[?1049hneedle alternate");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().generation, initial.generation);
    CHECK_EQ(controller->status().foundCount, 1);
    CHECK(controller->hits().front().start.buffer == Ubiquitty::Screen::Buffer::Alternate);
    {
        const std::scoped_lock hold(modelLock);
        feed(*terminal, "\033[?1049l");
        controller->noteModelChanged(*terminal);
    }
    while (controller->runSlice()) {
    }
    CHECK_EQ(controller->status().foundCount, 2);
    controller->detach();
}

TEST(partialEvictionDropsACompletedLinesCount)
{
    auto terminal = terminalOf(2, 200, 150);
    std::mutex modelLock;
    feed(*terminal, std::string(20000, 'a') + "\r\nquiet\r\nquiet\r\n");
    auto controller = SearchController::make(*terminal, modelLock);
    REQUIRE(controller->searchAndWait(literal("a"), waitLimit).foundCount == 20000);
    {
        const std::scoped_lock hold(modelLock);
        for (int line = 0; line < 55; ++line)
            feed(*terminal, "quiet\r\n");
        controller->noteModelChanged(*terminal);
    }
    for (int slice = 0; slice < 10000 && controller->hasWork(); ++slice)
        controller->runSlice();
    CHECK(!controller->hasWork());
    CHECK(controller->status().isComplete);
    CHECK_EQ(controller->status().foundCount, 0);
    CHECK_EQ(controller->status().activeIndex, -1);
    CHECK(controller->hits().empty());
    const Status maintained = controller->status();
    CHECK_EQ(controller->searchAndWait(literal("a"), waitLimit).foundCount, maintained.foundCount);
    controller->detach();
}

UBIQUITTY_TEST_MAIN()
