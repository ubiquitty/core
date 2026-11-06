// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// SearchExecutor — the process's one search worker.
//
// Every terminal's search runs here, in slices (Core/Search/SearchController.h),
// so however many panes search at once, one thread at background priority does
// the scanning, and ingest and rendering keep the cores they had. The worker
// starts on the first search and parks when no search has work. It rotates
// through the controllers with work, the one asked most recently first, so a
// new query in one pane is not stuck behind a long scan in another.
//
// A build without threads (Web) has no worker: its frontend calls `pump` from
// its idle callback, which runs the same slices inline until a deadline.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Ubiquitty::Search {

class SearchController;

/// The process's search worker.
class SearchExecutor {
public:
    /// How scheduled slices are driven.
    enum class ExecutionMode : std::uint8_t {
        /// A background worker runs the slices when threads are available.
        Worker,
        /// Only an explicit pump runs slices, on its calling thread.
        Pumped,
    };

    /// Returns the process's executor, which owns its synchronization.
    [[nodiscard]] static SearchExecutor& shared();

    /// Creates an executor with the specified scheduling policy.
    ///
    /// - Parameter mode: Whether to start a worker or wait for a pump.
    explicit SearchExecutor(ExecutionMode mode = ExecutionMode::Worker)
        : m_mode(mode)
    {
    }
    /// Stops the worker and waits for the slice it is running.
    ~SearchExecutor();
    SearchExecutor(const SearchExecutor&) = delete;
    SearchExecutor& operator=(const SearchExecutor&) = delete;

    /// Subscribes a controller to slices for its query and model publications.
    ///
    /// Any thread, without the model lock. A controller already waiting moves
    /// to the front, since its request is now the newest.
    ///
    /// - Parameter controller: The controller with a new request.
    void schedule(const std::shared_ptr<SearchController>& controller);

    /// Notification state shared by this executor and its controllers. Keeping
    /// it alive does not keep the executor or its worker alive.
    /// - Returns: The publication wake-up, immutable after construction.
    [[nodiscard]] const std::shared_ptr<std::atomic<std::uint64_t>>& wakeSignal() const { return m_wake; }

    /// Runs waiting slices on the calling thread until the deadline or until
    /// none is left, for a build without a worker and for callers that wait.
    ///
    /// - Parameter deadline: When to stop starting slices.
    /// - Returns: Whether work is left.
    bool pump(std::chrono::steady_clock::time_point deadline);

private:
    /// The worker's loop: takes the newest controller with work, runs one of
    /// its slices, and moves on.
    void run();
    /// Takes the controller whose slice runs next, rotating the rest; null
    /// when none is waiting. The executor's mutex is held.
    [[nodiscard]] std::shared_ptr<SearchController> takeNext();
    /// Puts a controller that still has work back behind the others. The
    /// executor's mutex is held.
    ///
    /// - Parameter controller: The controller.
    void requeue(const std::shared_ptr<SearchController>& controller);

    const ExecutionMode m_mode;
    std::mutex m_mutex;
    std::shared_ptr<std::atomic<std::uint64_t>> m_wake = std::make_shared<std::atomic<std::uint64_t>>(0);
    // Subscribed controllers, including completed queries awaiting output.
    std::vector<std::weak_ptr<SearchController>> m_waiting;
    std::thread m_worker;
    bool m_isStopping = false;
};

} // namespace Ubiquitty::Search
