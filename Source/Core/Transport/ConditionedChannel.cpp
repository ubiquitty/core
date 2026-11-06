// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Transport/ConditionedChannel.cpp — see ConditionedChannel.h.

#include "Core/Transport/ConditionedChannel.h"

#include <algorithm>
#include <utility>

namespace Ubiquitty::Transport {

namespace {

// A stop reached from an observer callback cannot wait for that callback to finish: it is the
// callback. Keeping the active operations as a thread-local stack makes that exemption exact even
// when one conditioned channel calls into another on the same thread.
struct OperationFrame {
    explicit OperationFrame(const void* state)
        : m_state(state)
        , m_previous(s_current)
    {
        s_current = this;
    }

    ~OperationFrame() { s_current = m_previous; }

    const void* m_state;
    OperationFrame* m_previous;
    static thread_local OperationFrame* s_current;
};

thread_local OperationFrame* OperationFrame::s_current = nullptr;

int operationDepthForState(const void* state)
{
    int depth = 0;
    for (OperationFrame* frame = OperationFrame::s_current; frame; frame = frame->m_previous) {
        if (frame->m_state == state)
            ++depth;
    }
    return depth;
}

} // namespace

// ---- ConditionedChannel ----------------------------------------------

// An admitted release or observer callback keeps the shared State alive and is counted until it
// leaves external code. stop() closes admission first, then waits for these leases to drain.
class ConditionedChannel::OperationLease final {
public:
    enum class ObserverAccess {
        None,
        Borrow,
    };

    OperationLease(std::shared_ptr<State> state, std::uint64_t generation, ObserverAccess observerAccess)
        : m_state(std::move(state))
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        if (m_state->generation != generation || !m_state->deliveringLocked())
            return;
        if (observerAccess == ObserverAccess::Borrow)
            m_observer = m_state->observer;
        ++m_state->operationsInFlight;
        m_isActive = true;
    }

    // A terminal callback is reserved while the stopped transition holds the mutex. Adopting that
    // reservation lets a concurrent stop wait even before the callback itself begins.
    OperationLease(std::shared_ptr<State> state, ChannelObserver* observer)
        : m_state(std::move(state))
        , m_observer(observer)
        , m_isActive(true)
    {
    }

    ~OperationLease()
    {
        if (!m_isActive)
            return;
        {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            --m_state->operationsInFlight;
        }
        m_state->operationsDone.notify_all();
    }

    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;

    [[nodiscard]] explicit operator bool() const { return m_isActive; }
    [[nodiscard]] ChannelObserver* observer() const { return m_observer; }

private:
    std::shared_ptr<State> m_state;
    ChannelObserver* m_observer = nullptr;
    bool m_isActive = false;
};

ConditionedChannel::ConditionedChannel(std::unique_ptr<Channel> inner, LinkScheduler* scheduler,
    LinkConditions conditions)
    : m_state(std::make_shared<State>())
    , m_inner(std::move(inner))
    , m_scheduler(scheduler)
{
    m_state->conditions = conditions;
    m_state->inner = m_inner.get();
}

ConditionedChannel::~ConditionedChannel()
{
    stop();
    // A peer close marks State stopped without asking the already-closed carrier to join. Calling
    // stop again here is harmless and makes destruction wait for every carrier implementation.
    if (m_inner)
        m_inner->stop();
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->inner = nullptr;
    }
}

std::uint64_t ConditionedChannel::reserveRelease(const LinkConditions& conditions, Direction& direction,
    std::uint32_t& jitterRandomState, std::uint64_t nowMilliseconds, std::size_t byteCount)
{
    std::uint64_t delay = conditions.latencyMilliseconds;
    if (conditions.jitterMilliseconds > 0) {
        // Deterministic draw: a harness that reproduces a failure needs the same schedule twice, and
        // a conditioner seeded from the clock cannot give one.
        jitterRandomState = jitterRandomState * 1664525u + 1013904223u;
        delay += jitterRandomState % (conditions.jitterMilliseconds + 1u);
    }
    // The clamp is the ordering invariant: a chunk never releases before the one ahead of it.
    std::uint64_t release = std::max(nowMilliseconds + delay, direction.nextReleaseMilliseconds);
    if (conditions.bytesPerSecond > 0) {
        const std::uint64_t serialization
            = (static_cast<std::uint64_t>(byteCount) * 1000u) / conditions.bytesPerSecond;
        release += serialization;
    }
    direction.nextReleaseMilliseconds = release;
    return release;
}

void ConditionedChannel::releaseOrHold(const std::shared_ptr<State>& state, LinkScheduler* scheduler,
    Direction& direction, std::size_t byteCount, std::function<void()> work)
{
    const std::uint64_t generation = state->generation;
    std::weak_ptr<State> weak = state;
    auto releaseWork = [weak, generation, work = std::move(work)]() mutable {
        auto alive = weak.lock();
        if (!alive)
            return; // the channel outlived by its schedule; nothing to deliver to
        OperationLease lease(alive, generation, OperationLease::ObserverAccess::None);
        if (!lease)
            return;
        OperationFrame operationFrame(alive.get());
        work();
    };

    if (state->conditions.blackout) {
        // Held, not dropped: a link that comes back delivers what it was holding, and a link that
        // never does is torn down with the channel.
        direction.held.push_back(std::move(releaseWork));
        return;
    }
    const std::uint64_t releaseTime = reserveRelease(state->conditions, direction,
        state->jitterRandomState, scheduler->nowMilliseconds(), byteCount);
    scheduler->scheduleAt(releaseTime, std::move(releaseWork));
}

void ConditionedChannel::setConditions(LinkConditions conditions)
{
    std::vector<std::function<void()>> released;
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        const bool wasBlackedOut = m_state->conditions.blackout;
        m_state->conditions = conditions;
        if (wasBlackedOut && !conditions.blackout) {
            // Both directions, in arrival order within each. Re-scheduled from now rather than from
            // their original deadlines, which is what a link resuming looks like.
            released.reserve(m_state->inbound.held.size() + m_state->outbound.held.size());
            for (auto& work : m_state->inbound.held)
                released.push_back(std::move(work));
            for (auto& work : m_state->outbound.held)
                released.push_back(std::move(work));
            m_state->inbound.held.clear();
            m_state->outbound.held.clear();
            const std::uint64_t now = m_scheduler->nowMilliseconds();
            m_state->inbound.nextReleaseMilliseconds = now;
            m_state->outbound.nextReleaseMilliseconds = now;
        }
    }
    for (auto& work : released)
        work();
}

LinkConditions ConditionedChannel::conditions() const
{
    std::lock_guard<std::mutex> guard(m_state->mutex);
    return m_state->conditions;
}

void ConditionedChannel::beginBlackout()
{
    LinkConditions next = conditions();
    next.blackout = true;
    setConditions(next);
}

void ConditionedChannel::endBlackout()
{
    LinkConditions next = conditions();
    next.blackout = false;
    setConditions(next);
}

void ConditionedChannel::cut(const std::string& reason)
{
    const std::shared_ptr<State> state = m_state;
    ChannelObserver* observer = nullptr;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->deliveringLocked())
            return;
        observer = state->observer;
        // A cut is terminal for this carrier: what the blackout held dies with it, because a reset
        // does not deliver what was in flight.
        state->inbound.held.clear();
        state->outbound.held.clear();
        state->observer = nullptr;
        state->hasStarted = false;
        state->stopped = true;
        state->innerClosed = true;
        ++state->generation;
        ++state->operationsInFlight;
    }
    OperationLease callback(state, observer);
    OperationFrame operationFrame(state.get());
    if (m_inner)
        m_inner->stop();
    observer->onClose(reason);
}

bool ConditionedChannel::innerIsOpen() const
{
    std::lock_guard<std::mutex> guard(m_state->mutex);
    return !m_state->innerClosed;
}

std::uint64_t ConditionedChannel::datagramsDropped() const
{
    std::lock_guard<std::mutex> guard(m_state->mutex);
    return m_state->datagramsDropped;
}

bool ConditionedChannel::start(ChannelObserver* observer)
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        if (m_state->stopped || !observer || !m_inner)
            return false;
        if (m_state->hasStarted)
            return true;
        m_state->observer = observer;
        m_state->hasStarted = true;
        generation = m_state->generation;
    }
    if (m_inner->start(this))
        return true;

    // The inner carrier promises that a refused start emits no callback. Advancing the generation
    // also makes any work queued before that refusal inert before this wrapper permits a retry.
    std::lock_guard<std::mutex> guard(m_state->mutex);
    if (m_state->generation == generation && !m_state->stopped) {
        m_state->observer = nullptr;
        m_state->hasStarted = false;
        ++m_state->generation;
        m_state->inbound.held.clear();
        m_state->outbound.held.clear();
    }
    return false;
}

void ConditionedChannel::send(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> copy(data.begin(), data.end());
    const std::size_t byteCount = copy.size();
    std::lock_guard<std::mutex> guard(m_state->mutex);
    if (m_state->stopped)
        return;
    const std::uint64_t generation = m_state->generation;
    std::weak_ptr<State> weak = m_state;
    releaseOrHold(m_state, m_scheduler, m_state->outbound, byteCount,
        [weak, generation, copy = std::move(copy)] {
            auto alive = weak.lock();
            if (!alive)
                return;
            Channel* inner = nullptr;
            {
                std::lock_guard<std::mutex> guard(alive->mutex);
                if (alive->stopped || alive->generation != generation)
                    return;
                inner = alive->inner;
            }
            if (inner)
                inner->send(copy);
        });
}

void ConditionedChannel::stop()
{
    const std::shared_ptr<State> state = m_state;
    bool shouldStopInner = false;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->stopped) {
            state->stopped = true;
            state->hasStarted = false;
            state->observer = nullptr;
            ++state->generation;
            state->inbound.held.clear();
            state->outbound.held.clear();
            shouldStopInner = true;
        }
    }
    if (shouldStopInner && m_inner)
        m_inner->stop();

    const int operationsOnThisThread = operationDepthForState(state.get());
    std::unique_lock<std::mutex> lock(state->mutex);
    state->operationsDone.wait(lock, [state, operationsOnThisThread] {
        return state->operationsInFlight <= operationsOnThisThread;
    });
}

void ConditionedChannel::suspendReading()
{
    if (m_inner)
        m_inner->suspendReading();
}

void ConditionedChannel::resumeReading()
{
    if (m_inner)
        m_inner->resumeReading();
}

std::int64_t ConditionedChannel::pendingOutboundBytes() const
{
    return m_inner ? m_inner->pendingOutboundBytes() : -1;
}

std::int64_t ConditionedChannel::outboundCapacityBytes() const
{
    return m_inner ? m_inner->outboundCapacityBytes() : -1;
}

Origin ConditionedChannel::origin() const { return m_inner ? m_inner->origin() : Origin {}; }

Health ConditionedChannel::health() const
{
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        // A cut carrier reads Closed even while the inner one is still winding down, so a session
        // polling health after its onClose does not see a link that is gone reported as up.
        if (m_state->innerClosed)
            return Health::Closed;
    }
    return m_inner ? m_inner->health() : Health::Idle;
}

std::string ConditionedChannel::description() const
{
    return m_inner ? m_inner->description() + " (conditioned)" : "conditioned";
}

bool ConditionedChannel::queryStats(TransportStats& out) const
{
    return m_inner ? m_inner->queryStats(out) : false;
}

// ---- inbound, from the inner carrier's I/O thread ---------------------

void ConditionedChannel::onConnect()
{
    // Not delayed: connecting is the carrier's own handshake, which the latency here has already
    // been paid into. Delaying it would double-count the setup cost and hide a session's real
    // time-to-first-frame behind an artefact of the harness.
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        generation = m_state->generation;
    }
    OperationLease callback(m_state, generation, OperationLease::ObserverAccess::Borrow);
    if (!callback)
        return;
    OperationFrame operationFrame(m_state.get());
    callback.observer()->onConnect();
}

void ConditionedChannel::onData(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> copy(data.begin(), data.end());
    const std::size_t byteCount = copy.size();
    std::lock_guard<std::mutex> guard(m_state->mutex);
    if (!m_state->deliveringLocked())
        return;
    const std::uint64_t generation = m_state->generation;
    std::weak_ptr<State> weak = m_state;
    releaseOrHold(m_state, m_scheduler, m_state->inbound, byteCount,
        [weak, generation, copy = std::move(copy)] {
            auto alive = weak.lock();
            if (!alive)
                return;
            OperationLease callback(alive, generation, OperationLease::ObserverAccess::Borrow);
            if (!callback)
                return;
            OperationFrame operationFrame(alive.get());
            callback.observer()->onData(copy);
        });
}

void ConditionedChannel::onDatagram(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> copy(data.begin(), data.end());
    const std::size_t byteCount = copy.size();
    std::lock_guard<std::mutex> guard(m_state->mutex);
    if (!m_state->deliveringLocked())
        return;
    if (m_state->conditions.datagramLossPercent > 0) {
        m_state->lossRandomState = m_state->lossRandomState * 1664525u + 1013904223u;
        if ((m_state->lossRandomState >> 16) % 100u < m_state->conditions.datagramLossPercent) {
            ++m_state->datagramsDropped;
            return; // unreliable by contract: the sender does not repeat it
        }
    }
    const std::uint64_t generation = m_state->generation;
    std::weak_ptr<State> weak = m_state;
    releaseOrHold(m_state, m_scheduler, m_state->inbound, byteCount,
        [weak, generation, copy = std::move(copy)] {
            auto alive = weak.lock();
            if (!alive)
                return;
            OperationLease callback(alive, generation, OperationLease::ObserverAccess::Borrow);
            if (!callback)
                return;
            OperationFrame operationFrame(alive.get());
            callback.observer()->onDatagram(copy);
        });
}

void ConditionedChannel::onClose(const std::string& reason)
{
    // Reported straight through, undelayed: a close is the carrier saying it is gone, and holding it
    // behind the link's own latency would have the session keep writing into a carrier that has
    // already ended.
    const std::shared_ptr<State> state = m_state;
    ChannelObserver* observer = nullptr;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->innerClosed = true;
        if (!state->deliveringLocked())
            return;
        observer = state->observer;
        state->observer = nullptr;
        state->hasStarted = false;
        state->stopped = true;
        ++state->generation;
        state->inbound.held.clear();
        state->outbound.held.clear();
        ++state->operationsInFlight;
    }
    OperationLease callback(state, observer);
    OperationFrame operationFrame(state.get());
    observer->onClose(reason);
}

} // namespace Ubiquitty::Transport
