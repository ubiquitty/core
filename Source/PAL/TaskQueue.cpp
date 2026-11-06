// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/TaskQueue.h"

namespace Ubiquitty {

bool TaskQueue::submit(WorkerFunction worker, void* userData)
{
    worker(userData); // inline: the result is ready before this returns
    {
        std::scoped_lock lock(m_mutex);
        m_completed.push_back(userData);
    }
    // Release pairs with pendingCount()'s acquire: a thread that observes
    // the new count also observes the deque push above.
    m_pendingCount.fetch_add(1, std::memory_order_release);
    return true;
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

} // namespace Ubiquitty
