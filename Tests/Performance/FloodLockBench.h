// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BenchHarness.h"
#include "Core/CellGrid.h"
#include "Core/ModelLock.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace Ubiquitty::Bench {

/// Selects production fairness or the explicit raw-mutex counterexample.
enum class FloodLockService {
    Production,
    RawMutex
};

/// Records lock-acquisition tails while a drain parses a circular byte stream.
///
/// The production path uses the drain's handoff and the reader's counted hold.
/// The raw mutex records a counterexample under distinct metric names.
///
/// - Parameters:
///   - out: Receives p99 and maximum wait results in milliseconds.
///   - flood: The nonempty corpus, borrowed until both threads finish.
///   - rows: The positive number of visible rows.
///   - columns: The positive number of columns in each visible row.
///   - durationSeconds: The sampling window; zero emits empty tail results.
template<FloodLockService service = FloodLockService::Production>
void measureFloodLockWait(std::vector<Result>& out, std::span<const char> flood, int rows, int columns, double durationSeconds)
{
    assert(!flood.empty());
    assert(rows > 0 && columns > 0);
    constexpr std::size_t chunkBytes = 4096;
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    using FloodLock = std::conditional_t<service == FloodLockService::Production, Ubiquitty::ModelLock, std::mutex>;
    FloodLock modelLock;
    std::atomic<bool> stop { false };

    std::thread producer([&] {
        std::size_t off = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const std::size_t n = std::min(chunkBytes, flood.size() - off);
            {
                if constexpr (service == FloodLockService::Production)
                    modelLock.lockForDrain();
                else
                    modelLock.lock();
                Ubiquitty::processBytes(term.get(), std::string_view(flood.data() + off, static_cast<std::size_t>(n)));
                if constexpr (service == FloodLockService::Production)
                    (void)modelLock.unlockForDrain();
                else
                    modelLock.unlock();
            }
            off += n;
            if (off >= flood.size())
                off = 0; // wrap — a fixed deterministic stream replayed for the run's duration
        }
    });

    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(columns));
    std::vector<double> waitSeconds;
    const auto tickInterval = std::chrono::microseconds(16667); // 60 Hz cadence
    const auto runUntil = std::chrono::steady_clock::now() + std::chrono::duration<double>(durationSeconds);
    while (std::chrono::steady_clock::now() < runUntil) {
        const auto tickStart = std::chrono::steady_clock::now();
        const auto wantLock = std::chrono::steady_clock::now();
        double waitedSeconds = 0.0;
        {
            using ReadHold = std::conditional_t<service == FloodLockService::Production, Ubiquitty::ModelLockHold, std::unique_lock<std::mutex>>;
            const ReadHold hold(modelLock);
            const auto gotLock = std::chrono::steady_clock::now();
            waitedSeconds = std::chrono::duration<double>(gotLock - wantLock).count();
            for (int r = 0; r < rows; ++r)
                Ubiquitty::Screen::getDisplayRow(*term, 0, r, row.data(), columns);
        }
        waitSeconds.push_back(waitedSeconds);
        std::this_thread::sleep_until(tickStart + tickInterval);
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();
    term.reset();
    Ubiquitty::Bench::frameTailMilliseconds(out, service == FloodLockService::Production ? "model_lock_wait_under_flood" : "lock_wait_under_flood", std::move(waitSeconds));
}

} // namespace Ubiquitty::Bench
