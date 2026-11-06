// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// TaskQueue — submit-then-drain job queue. submit() runs the worker inline (no
// background thread) and queues its result; drain() later pulls completed jobs
// FIFO and hands each to a completion callback. Callers carry a generation stamp
// in the job so a completion can drop a result whose Terminal was disposed
// between submit and drain.
#include <atomic>
#include <deque>
#include <mutex>

namespace Ubiquitty {

class TaskQueue {
public:
    using WorkerFunction = void (*)(void* userData); // runs at submit; fills userData
    using CompletionFunction = void (*)(void* userData); // runs at drain; integrates the result

    TaskQueue() = default;
    ~TaskQueue() = default;
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // Run the worker inline, then queue userData for a later drain. Returns true.
    bool submit(WorkerFunction, void* userData);
    // Pull completed jobs FIFO and invoke the completion on each. Returns the count
    // drained. Non-blocking: returns 0 immediately when nothing is ready.
    int drain(CompletionFunction);
    // Submitted but not yet drained. For diagnostics + the fast-path drain skip.
    [[nodiscard]] int pendingCount() const;

private:
    // The completed deque can be drained from a different thread than submit
    // (e.g. a per-tick drain), so guard it; pendingCount is atomic for the
    // lock-free fast-path check.
    mutable std::mutex m_mutex;
    std::deque<void*> m_completed;
    std::atomic<int> m_pendingCount { 0 };
};

} // namespace Ubiquitty
