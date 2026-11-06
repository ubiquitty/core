// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Transport/DelegatedChannel.h"

#include <utility>

namespace Ubiquitty::Transport {

DelegatedChannel::DelegatedChannel(Driver driver, Transport::Origin origin, std::string description)
    : m_driver(driver)
    , m_origin(std::move(origin))
    , m_description(std::move(description))
{
}

DelegatedChannel::~DelegatedChannel()
{
    stop();
    if (m_driver.release)
        m_driver.release(m_driver.context);
}

bool DelegatedChannel::start(ChannelObserver* observer)
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped)
            return false;
        if (m_hasStarted)
            return true;
        if (!observer || !m_driver.start) {
            m_health = Health::Failed;
            return false;
        }
        m_observer = observer;
        m_hasStarted = true;
        m_health = Health::Connecting;
    }

    // Outside the lock: a Driver may answer synchronously, and its answer
    // arrives back through notifyConnect/notifyClose, which take the lock.
    if (m_driver.start(m_driver.context))
        return true;

    // An immediate local setup error fires no observer callback, so the
    // observer is dropped rather than left borrowed.
    std::scoped_lock guard(m_mutex);
    m_observer = nullptr;
    m_hasStarted = false;
    m_health = Health::Failed;
    return false;
}

void DelegatedChannel::send(std::span<const std::uint8_t> data)
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped || !m_hasStarted)
            return;
    }
    if (m_driver.send && !data.empty())
        m_driver.send(m_driver.context, data.data(), data.size());
}

void DelegatedChannel::stop()
{
    bool isFirstStop = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_hasStopped) {
            m_hasStopped = true;
            // Dropped under the lock: no LATER callback can take a copy. A
            // callback already past the gate is still running, which is what
            // the drain below waits out.
            m_observer = nullptr;
            m_health = Health::Closed;
            isFirstStop = true;
        }
        // Every stop drains, not only the first: the destructor's stop may be
        // the second one racing another thread's, and destruction must not
        // proceed while an observer callback is executing. The exemption is
        // an observer stopping the channel from inside its own callback —
        // waiting there is waiting for oneself.
        if (m_callbacksInFlight > 0 && m_callbackThread != std::this_thread::get_id())
            m_callbackDone.wait(lock, [this] { return m_callbacksInFlight == 0; });
    }
    // Outside the lock: a Driver's stop joins its I/O thread, which may itself
    // be blocked acquiring this mutex inside deliver().
    if (isFirstStop && m_driver.stop)
        m_driver.stop(m_driver.context);
}

void DelegatedChannel::suspendReading()
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped)
            return;
    }
    if (m_driver.suspendReading)
        m_driver.suspendReading(m_driver.context);
}

void DelegatedChannel::resumeReading()
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped)
            return;
    }
    if (m_driver.resumeReading)
        m_driver.resumeReading(m_driver.context);
}

std::int64_t DelegatedChannel::pendingOutboundBytes() const
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped)
            return -1;
    }
    if (!m_driver.pendingOutboundBytes)
        return -1;
    const std::int64_t queued = m_driver.pendingOutboundBytes(m_driver.context);
    return queued < 0 ? -1 : queued;
}

std::int64_t DelegatedChannel::outboundCapacityBytes() const
{
    return m_driver.outboundCapacityBytes ? m_driver.outboundCapacityBytes(m_driver.context) : -1;
}

bool DelegatedChannel::peerAuthenticationFailed() const
{
    return m_driver.peerAuthenticationFailed && m_driver.peerAuthenticationFailed(m_driver.context);
}

bool DelegatedChannel::boundsInitialSetupFailures() const
{
    return m_driver.boundsInitialSetupFailures && m_driver.boundsInitialSetupFailures(m_driver.context);
}

void DelegatedChannel::setWindowSize(int columns, int rows)
{
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped)
            return;
    }
    if (m_driver.setWindowSize)
        m_driver.setWindowSize(m_driver.context, columns, rows);
}

Health DelegatedChannel::health() const
{
    std::scoped_lock guard(m_mutex);
    return m_health;
}

// Every observer callback below is made with the mutex RELEASED. Holding it
// across the callback would deadlock on the first thing the Sync client does:
// onConnect sends the HELLO, and send() takes this same mutex. What keeps the
// observer alive across the unlocked call is the Driver's quiescing contract —
// stop() does not return until the Driver's stop has joined its I/O thread, so
// an in-flight callback always completes before the caller regains control and
// can destroy the observer.

void DelegatedChannel::notifyConnect()
{
    ChannelObserver* observer = nullptr;
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped || !m_observer)
            return;
        m_health = Health::Connected;
        observer = m_observer;
        ++m_callbacksInFlight;
        m_callbackThread = std::this_thread::get_id();
    }
    observer->onConnect();
    {
        std::scoped_lock guard(m_mutex);
        --m_callbacksInFlight;
    }
    m_callbackDone.notify_all();
}

void DelegatedChannel::deliver(std::span<const std::uint8_t> data)
{
    ChannelObserver* observer = nullptr;
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped || !m_observer)
            return;
        observer = m_observer;
        ++m_callbacksInFlight;
        m_callbackThread = std::this_thread::get_id();
    }
    observer->onData(data);
    {
        std::scoped_lock guard(m_mutex);
        --m_callbacksInFlight;
    }
    m_callbackDone.notify_all();
}

void DelegatedChannel::notifyClose(Health outcome, const std::string& reason)
{
    ChannelObserver* observer = nullptr;
    {
        std::scoped_lock guard(m_mutex);
        if (m_hasStopped || !m_observer)
            return;
        m_health = (outcome == Health::Closed) ? Health::Closed : Health::Failed;
        // Dropped before the callback so a re-entrant notify — from the
        // observer, or from a Driver still draining — cannot deliver a second
        // onClose.
        observer = m_observer;
        m_observer = nullptr;
        ++m_callbacksInFlight;
        m_callbackThread = std::this_thread::get_id();
    }
    observer->onClose(reason);
    {
        std::scoped_lock guard(m_mutex);
        --m_callbacksInFlight;
    }
    m_callbackDone.notify_all();
}

} // namespace Ubiquitty::Transport
