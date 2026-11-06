// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SearchExecutor — see SearchExecutor.h.

#include "Core/Search/SearchExecutor.h"

#include "Core/Search/SearchController.h"
#include "PAL/PAL.h"

#include <algorithm>

namespace Ubiquitty::Search {

SearchExecutor& SearchExecutor::shared()
{
    static SearchExecutor executor;
    return executor;
}

SearchExecutor::~SearchExecutor()
{
    {
        const std::scoped_lock hold(m_mutex);
        m_isStopping = true;
    }
    m_wake->fetch_add(1, std::memory_order_release);
    m_wake->notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void SearchExecutor::schedule(const std::shared_ptr<SearchController>& controller)
{
    if (!controller)
        return;
    {
        const std::scoped_lock hold(m_mutex);
        std::erase_if(m_waiting, [&controller](const std::weak_ptr<SearchController>& waiting) {
            const auto held = waiting.lock();
            return !held || held == controller;
        });
        m_waiting.push_back(controller);
        if (m_mode == ExecutionMode::Worker) {
#if !defined(__EMSCRIPTEN__)
            if (!m_worker.joinable() && !m_isStopping)
                m_worker = std::thread(&SearchExecutor::run, this);
#endif
        }
    }
    m_wake->fetch_add(1, std::memory_order_release);
    m_wake->notify_one();
}

std::shared_ptr<SearchController> SearchExecutor::takeNext()
{
    for (std::size_t index = m_waiting.size(); index-- > 0;) {
        auto controller = m_waiting[index].lock();
        if (!controller || controller->hasWork()) {
            m_waiting.erase(m_waiting.begin() + static_cast<std::ptrdiff_t>(index));
        } else
            continue;
        if (controller)
            return controller;
    }
    return nullptr;
}

void SearchExecutor::requeue(const std::shared_ptr<SearchController>& controller)
{
    // A request that arrived during the slice already put it back, newest.
    const bool isWaiting = std::ranges::any_of(m_waiting, [&controller](const std::weak_ptr<SearchController>& waiting) { return waiting.lock() == controller; });
    if (!isWaiting)
        m_waiting.insert(m_waiting.begin(), controller);
}

void SearchExecutor::run()
{
    PAL::lowerCurrentThreadToUtilityPriority();
    std::unique_lock hold(m_mutex);
    while (true) {
        const std::uint64_t observed = m_wake->load(std::memory_order_acquire);
        if (m_isStopping)
            return;
        const auto controller = takeNext();
        if (!controller) {
            hold.unlock();
            m_wake->wait(observed, std::memory_order_acquire);
            hold.lock();
            continue;
        }
        hold.unlock();
        controller->runSlice();
        hold.lock();
        requeue(controller);
    }
}

bool SearchExecutor::pump(std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline) {
        std::shared_ptr<SearchController> controller;
        {
            const std::scoped_lock hold(m_mutex);
            controller = takeNext();
        }
        if (!controller)
            return false;
        controller->runSlice();
        {
            const std::scoped_lock hold(m_mutex);
            requeue(controller);
        }
    }
    const std::scoped_lock hold(m_mutex);
    return std::ranges::any_of(m_waiting, [](const auto& waiting) {
        const auto controller = waiting.lock();
        return controller && controller->hasWork();
    });
}

} // namespace Ubiquitty::Search
