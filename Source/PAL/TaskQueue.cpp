// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/TaskQueue.h"

#include "PAL/PAL.h"

namespace Ubiquitty {

TaskQueue::TaskQueue(Execution execution, WakeFunction wake, void* wakeContext)
    : m_execution(execution)
    , m_wake(wake)
    , m_wakeContext(wakeContext)
{
}

TaskQueue::~TaskQueue()
{
    {
        std::scoped_lock lock(m_mutex);
        m_isStopping = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

bool TaskQueue::submit(WorkerFunction worker, void* userData)
{
    // Release pairs with pendingCount()'s acquire: a thread that observes
    // the new count also observes the job it counts.
    m_pendingCount.fetch_add(1, std::memory_order_release);
    if (m_execution == Execution::Inline) {
        worker(userData); // the result is ready before this returns
        {
            std::scoped_lock lock(m_mutex);
            m_completed.push_back(userData);
        }
        m_readyCount.fetch_add(1, std::memory_order_release);
        return true;
    }
    {
        std::scoped_lock lock(m_mutex);
        m_queued.emplace_back(worker, userData);
        if (!m_worker.joinable())
            m_worker = std::thread([this] { runWorker(); });
    }
    m_condition.notify_all();
    return true;
}

void TaskQueue::runWorker()
{
    // The thread that drains waits on what this one runs, so it runs in that
    // thread's band: a job ranked with a loaded machine's busy work holds the
    // drain back by its scheduling delay.
    PAL::raiseCurrentThreadToInteractivePriority();
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_condition.wait(lock, [this] { return m_isStopping || !m_queued.empty(); });
        if (m_queued.empty())
            return; // stopping, with every submitted job run
        const auto [worker, userData] = m_queued.front();
        m_queued.pop_front();
        m_isRunningJob = true;
        lock.unlock();
        worker(userData);
        lock.lock();
        m_isRunningJob = false;
        m_completed.push_back(userData);
        m_readyCount.fetch_add(1, std::memory_order_release);
        m_condition.notify_all();
        if (m_wake)
            m_wake(m_wakeContext);
    }
}

int TaskQueue::drain(CompletionFunction completion)
{
    int count = 0;
    for (;;) {
        void* userData;
        {
            std::scoped_lock lock(m_mutex);
            if (m_completed.empty())
                break;
            userData = m_completed.front();
            m_completed.pop_front();
        }
        m_readyCount.fetch_sub(1, std::memory_order_release);
        completion(userData);
        m_pendingCount.fetch_sub(1, std::memory_order_release);
        ++count;
    }
    return count;
}

int TaskQueue::pendingCount() const
{
    // Acquire so the fast-path "anything to drain?" poll can't act on a
    // count newer than the deque state it implies. A stale zero is benign
    // (the per-tick poll retries); the mutex orders the deque itself.
    return m_pendingCount.load(std::memory_order_acquire);
}

int TaskQueue::readyCount() const
{
    return m_readyCount.load(std::memory_order_acquire);
}

void TaskQueue::waitUntilIdle()
{
    std::unique_lock lock(m_mutex);
    m_condition.wait(lock, [this] { return m_queued.empty() && !m_isRunningJob; });
}

} // namespace Ubiquitty
