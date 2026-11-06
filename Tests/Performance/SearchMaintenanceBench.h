// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BenchHarness.h"
#include "Core/Search/SearchController.h"
#include "Core/Search/SearchExecutor.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Ubiquitty::Bench::SearchMaintenance {

inline constexpr std::string_view ordinaryLine = "2026-08-14 INFO worker[3] processed batch of 128 records\r\n";
inline constexpr std::string_view matchingLine = "2026-08-14 ERROR worker[3] needle: batch rejected\r\n";
inline constexpr std::string_view query = "needle";

/// A model and its ordinary background search worker over retained log text.
class Fixture {
public:
    /// Seeds the model before any measured operation or worker begins.
    Fixture()
    {
        constexpr int historyLines = 100000;
        constexpr int rows = 50;
        m_terminal->scrollbackLimit = historyLines;
        resize(*m_terminal, rows, 200);
        std::string seed;
        seed.reserve((historyLines + rows) * ordinaryLine.size());
        for (int line = 0; line < historyLines + rows; ++line)
            seed += line % 1000 == 999 ? matchingLine : ordinaryLine;
        processBytes(m_terminal.get(), seed);
        if (!m_terminal->lineStore || m_terminal->lineStore->count() != historyLines) {
            std::fputs("search maintenance benchmark did not retain its full history fixture\n", stderr);
            std::exit(2);
        }
        m_controller = Search::SearchController::make(*m_terminal, m_modelLock, m_executor);
        m_controller->setChangedCallback(didPublish, this);
    }

    /// Waits out the worker's use of the model and publication callback.
    ~Fixture() { m_controller->detach(); }

    /// Starts one query, recording its first publication when a key is named.
    /// - Parameters:
    ///   - pattern: The complete current query.
    ///   - key: The query keystroke whose latency this request records.
    /// - Returns: The generation the worker must publish.
    std::uint64_t request(std::string_view pattern, std::optional<std::size_t> key = std::nullopt)
    {
        std::uint64_t generation = 0;
        {
            const std::scoped_lock hold(m_publicationLock);
            const auto start = std::chrono::steady_clock::now();
            const Search::Request request { .pattern = std::string(pattern), .sensitivity = CaseSensitivity::Sensitive };
            if (!m_controller->setQuery(request)) {
                std::fputs("search maintenance benchmark repeated a query without clearing it\n", stderr);
                std::exit(2);
            }
            generation = m_controller->requestedGeneration();
            m_hasQuery = !pattern.empty();
            if (key)
                m_keys[*key] = { .generation = generation, .start = start, .seconds = std::nullopt };
        }
        m_executor.schedule(m_controller);
        return generation;
    }

    /// Waits for exact counting without running worker slices on this thread.
    /// - Parameters:
    ///   - generation: The query being awaited.
    ///   - afterScreen: A prior publication that an appended line must advance.
    void waitComplete(std::uint64_t generation, std::optional<std::uint64_t> afterScreen = std::nullopt)
    {
        std::unique_lock hold(m_publicationLock);
        const bool isComplete = m_published.wait_for(hold, std::chrono::seconds(30), [&] {
            const auto status = m_controller->status();
            return status.generation == generation && status.isComplete
                && (!afterScreen || status.screenGeneration > *afterScreen);
        });
        if (!isComplete) {
            std::fputs("search maintenance benchmark timed out waiting for publication\n", stderr);
            std::exit(2);
        }
    }

    /// Appends one model publication and returns its lock-acquisition wait.
    /// - Parameter bytes: The terminal output to ingest.
    /// - Returns: Seconds spent waiting to acquire the model lock.
    double append(std::string_view bytes)
    {
        const auto start = std::chrono::steady_clock::now();
        const std::scoped_lock hold(m_modelLock);
        const auto acquired = std::chrono::steady_clock::now();
        processBytes(m_terminal.get(), bytes);
        m_controller->noteModelChanged(*m_terminal);
        return std::chrono::duration<double>(acquired - start).count();
    }

    /// Returns the worker's most recent publication.
    [[nodiscard]] Search::Status status() const { return m_controller->status(); }

    /// Clears the previous trial before any timed work begins.
    void reset()
    {
        if (m_hasQuery)
            waitComplete(request({}));
        const std::scoped_lock hold(m_publicationLock);
        m_keys = {};
        m_hasQuery = false;
    }

    /// Copies recorded key latencies and counts superseded requests.
    /// - Parameters:
    ///   - samples: Receives seconds to first publication.
    ///   - superseded: Receives keys replaced before their first publication.
    void collectKeys(std::vector<double>& samples, std::size_t& superseded)
    {
        const std::scoped_lock hold(m_publicationLock);
        for (const auto& key : m_keys) {
            if (key.seconds)
                samples.push_back(*key.seconds);
            else
                ++superseded;
        }
        if (!m_keys.back().seconds) {
            std::fputs("search maintenance benchmark did not publish its final query\n", stderr);
            std::exit(2);
        }
    }

private:
    /// Records a query's first publication independently of later progress.
    struct KeyPublication {
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point start;
        std::optional<double> seconds;
    };

    /// Records publication latency and wakes the foreground waiter.
    /// - Parameter context: The fixture, retained until controller detachment.
    static void didPublish(void* context)
    {
        auto& fixture = *static_cast<Fixture*>(context);
        const auto status = fixture.m_controller->status();
        const auto now = std::chrono::steady_clock::now();
        {
            const std::scoped_lock hold(fixture.m_publicationLock);
            for (auto& key : fixture.m_keys) {
                if (key.generation == status.generation && !key.seconds)
                    key.seconds = std::chrono::duration<double>(now - key.start).count();
            }
        }
        fixture.m_published.notify_all();
    }

    std::unique_ptr<Terminal> m_terminal = std::make_unique<Terminal>();
    std::mutex m_modelLock;
    Search::SearchExecutor m_executor;
    std::shared_ptr<Search::SearchController> m_controller;
    std::mutex m_publicationLock;
    std::condition_variable m_published;
    std::array<KeyPublication, query.size()> m_keys;
    bool m_hasQuery = false;
};

/// Measures one appended line through completion of incremental search.
/// - Parameters:
///   - out: Receives throughput and completion latency.
///   - warmup: Untimed iterations.
///   - iterations: Measured iterations.
inline void flowing(std::vector<Result>& out, int warmup, int iterations)
{
    if (warmup < 0 || iterations <= 0) {
        std::fputs("flowing search benchmark requires positive iterations and nonnegative warmup\n", stderr);
        std::exit(2);
    }
    Fixture fixture;
    const auto generation = fixture.request(query);
    fixture.waitComplete(generation);
    const auto timing = measure(warmup, iterations, [&] {
        const auto previous = fixture.status().screenGeneration;
        (void)fixture.append(matchingLine);
        fixture.waitComplete(generation, previous);
        auto count = fixture.status().foundCount;
        if (!count) {
            std::fputs("flowing search benchmark published no matches\n", stderr);
            std::exit(2);
        }
        doNotOptimize(count); }, Repeat::Once);
    out.push_back({ .name = "search_flowing_100k", .unit = "MB/s", .value = static_cast<double>(matchingLine.size()) / timing.medianSeconds / 1.0e6, .p95 = static_cast<double>(matchingLine.size()) / timing.p95Seconds / 1.0e6 });
    out.push_back({ .name = "search_flowing_100k_completion", .unit = "milliseconds", .value = timing.medianSeconds * 1.0e3, .p95 = timing.p95Seconds * 1.0e3 });
}

/// One fixed-cadence typing trial's ingest and publication observations.
struct TypingSample {
    double throughput = 0.0;
    std::vector<double> lockWait;
    std::vector<double> firstPublication;
    std::size_t superseded = 0;
};

/// Runs the same flood with query typing enabled or absent.
/// - Parameters:
///   - fixture: The retained model and worker.
///   - isSearching: Whether each key extends the query.
/// - Returns: Ingest throughput, model-lock wait and query-publication samples.
inline TypingSample typingTrial(Fixture& fixture, bool isSearching) // bool-ok: the paired arms enable or omit query typing.
{
    fixture.reset();
    TypingSample sample;
    constexpr std::size_t maxWaitSamples = 1'000'000;
    sample.lockWait.reserve(maxWaitSamples);
    std::string chunk;
    for (int line = 0; line < 64; ++line)
        chunk += line == 63 ? matchingLine : ordinaryLine;
    std::atomic<bool> shouldStop { false };
    std::latch ready(1);
    std::latch start(1);
    std::size_t bytes = 0;
    std::thread feeder([&] {
        ready.count_down();
        start.wait();
        while (!shouldStop.load(std::memory_order_acquire)) {
            const double wait = fixture.append(chunk);
            if (sample.lockWait.size() == maxWaitSamples) {
                std::fputs("typing search benchmark exceeded its lock-wait sample capacity\n", stderr);
                std::exit(2);
            }
            sample.lockWait.push_back(wait);
            bytes += chunk.size();
        }
    });
    ready.wait();
    std::uint64_t generation = 0;
    const auto timing = measure(0, 1, [&] {
        const auto beginning = std::chrono::steady_clock::now();
        constexpr auto cadence = std::chrono::milliseconds(30);
        start.count_down();
        for (std::size_t key = 0; key < query.size(); ++key) {
            std::this_thread::sleep_until(beginning + cadence * static_cast<int>(key));
            if (isSearching)
                generation = fixture.request(query.substr(0, key + 1), key);
        }
        std::this_thread::sleep_until(beginning + cadence * static_cast<int>(query.size()));
        shouldStop.store(true, std::memory_order_release);
        feeder.join(); }, Repeat::Once);
    if (!bytes || timing.medianSeconds <= 0.0) {
        std::fputs("typing search benchmark ingested no timed output\n", stderr);
        std::exit(2);
    }
    sample.throughput = static_cast<double>(bytes) / timing.medianSeconds / 1.0e6;
    if (isSearching) {
        fixture.waitComplete(generation);
        fixture.collectKeys(sample.firstPublication, sample.superseded);
    }
    return sample;
}

/// Reports paired ingest, first-publication latency and the model-lock tail.
/// - Parameters:
///   - out: Receives the measured metrics and publication coverage.
///   - warmup: Untimed paired trials.
///   - iterations: Measured paired trials.
inline void typing(std::vector<Result>& out, int warmup, int iterations)
{
    if (warmup < 0 || iterations <= 0) {
        std::fputs("typing search benchmark requires positive iterations and nonnegative warmup\n", stderr);
        std::exit(2);
    }
    Fixture baseline;
    Fixture searching;
    struct Samples {
        std::vector<double> throughput;
        std::vector<double> lockWait;
        std::vector<double> firstPublication;
        std::size_t superseded = 0;
        int trials = 0;
    };
    Samples closed;
    Samples opened;
    const auto run = [&](Fixture& fixture, Samples& samples, bool isSearching) { // bool-ok: forwards the paired arm.
        auto sample = typingTrial(fixture, isSearching);
        if (samples.trials++ < warmup)
            return;
        constexpr std::size_t maxRecordedWaitSamples = 4'000'000;
        if (sample.lockWait.size() > maxRecordedWaitSamples - samples.lockWait.size()) {
            std::fputs("typing search benchmark exceeded its aggregate lock-wait sample capacity\n", stderr);
            std::exit(2);
        }
        samples.throughput.push_back(sample.throughput);
        samples.lockWait.insert(samples.lockWait.end(), sample.lockWait.begin(), sample.lockWait.end());
        samples.firstPublication.insert(samples.firstPublication.end(), sample.firstPublication.begin(), sample.firstPublication.end());
        samples.superseded += sample.superseded;
    };
    (void)measurePaired(warmup, iterations, [&] { run(baseline, closed, false); }, [&] { run(searching, opened, true); });
    const auto emit = [&](Samples& samples, const char* suffix) {
        std::ranges::sort(samples.throughput, std::greater {});
        const std::string name = std::string("typing_while_searching_100k") + suffix;
        const auto slowTail = std::min(samples.throughput.size() - 1, static_cast<std::size_t>(samples.throughput.size() * 0.95));
        out.push_back({ .name = name, .unit = "MB/s", .value = samples.throughput[samples.throughput.size() / 2], .p95 = samples.throughput[slowTail] });
        latencyMilliseconds(out, name + "_lock_wait", std::move(samples.lockWait));
    };
    emit(closed, "_closed");
    emit(opened, "_open");
    const auto published = opened.firstPublication.size();
    latencyMilliseconds(out, "typing_while_searching_100k_first_publish", std::move(opened.firstPublication));
    out.push_back({ .name = "typing_while_searching_100k_published_queries", .unit = "queries", .value = static_cast<double>(published) });
    out.push_back({ .name = "typing_while_searching_100k_superseded_queries", .unit = "queries", .value = static_cast<double>(opened.superseded) });
}

} // namespace Ubiquitty::Bench::SearchMaintenance
