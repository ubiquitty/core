// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// TaskQueue — submit-then-drain job queue. A queue runs each worker either
// inline at submit, so its result is ready before submit returns, or on a
// thread of its own, one job at a time in submission order. Either way drain()
// hands the completed jobs, FIFO, to a completion callback on the caller's
// thread. Callers carry a generation stamp in the job so a completion can drop
// a result whose Terminal was disposed between submit and drain.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace Ubiquitty {

class TaskQueue {
public:
    using WorkerFunction = void (*)(void* userData); // fills userData; inline at submit, or on the worker
    using CompletionFunction = void (*)(void* userData); // runs at drain; integrates the result
    using WakeFunction = void (*)(void* context); // called on the worker after each job it completes

    /// Where a queue runs its workers.
    enum class Execution : std::uint8_t {
        /// In submit, on the submitting thread.
        Inline,
        /// On the queue's own thread, started at the first submit.
        Worker,
    };

    /// Makes a queue.
    ///
    /// - Parameters:
    ///   - execution: Where its workers run.
    ///   - wake: Called on the worker after each job it completes, so the
    ///     thread that drains can be woken; never called inline.
    ///   - wakeContext: The wake's argument, outliving the queue.
    explicit TaskQueue(Execution execution = Execution::Inline, WakeFunction wake = nullptr, void* wakeContext = nullptr);
    /// Stops a worker queue once it has run every job submitted to it. Jobs
    /// left undrained are not freed: drain them first.
    ~TaskQueue();
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    /// Runs the worker inline, or queues it for the worker thread, and queues
    /// userData for a drain once the worker has run.
    ///
    /// - Returns: True.
    bool submit(WorkerFunction, void* userData);
    // Pull completed jobs FIFO and invoke the completion on each. Returns the count
    // drained. Non-blocking: returns 0 immediately when nothing is ready.
    int drain(CompletionFunction);
    // Submitted but not yet drained, whether or not its worker has run. For
    // diagnostics + the fast-path drain skip.
    [[nodiscard]] int pendingCount() const;
    /// Jobs whose worker has run and that wait for a drain.
    [[nodiscard]] int readyCount() const;
    /// Returns once every job submitted so far has run. Immediate inline.
    void waitUntilIdle();
    /// Where this queue runs its workers.
    [[nodiscard]] Execution execution() const { return m_execution; }

private:
    /// Runs the queued jobs in order on the worker thread until the queue
    /// stops with none left.
    void runWorker();

    const Execution m_execution;
    const WakeFunction m_wake;
    void* const m_wakeContext;
    // The completed deque can be drained from a different thread than submit
    // (e.g. a per-tick drain), so guard it; the counts are atomic for the
    // lock-free fast-path checks.
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::pair<WorkerFunction, void*>> m_queued;
    std::deque<void*> m_completed;
    bool m_isRunningJob = false;
    bool m_isStopping = false;
    std::atomic<int> m_pendingCount { 0 };
    std::atomic<int> m_readyCount { 0 };
    std::thread m_worker;
};

} // namespace Ubiquitty
