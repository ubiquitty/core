// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Session/BoundedSocketServer.cpp — see BoundedSocketServer.h.
//

#include "Core/Session/BoundedSocketServer.h"

#include "PAL/Socket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace Ubiquitty::Session {

BoundedSocketServer::BoundedSocketServer(int listenFileDescriptor, Options options, PeerVerifyCallback verifyPeer, LineHandlerCallback handleLine)
    : m_listenFileDescriptor(listenFileDescriptor)
    , m_acceptWaker(PAL::makeWaker())
    , m_options(options)
    , m_verifyPeer(std::move(verifyPeer))
    , m_handleLine(std::move(handleLine))
{
}

BoundedSocketServer::~BoundedSocketServer() { stop(); }

bool BoundedSocketServer::tryAcquireSlot()
{
    std::scoped_lock lock(m_slotMutex);
    if (m_occupiedSlots >= m_options.maxConcurrentClients)
        return false;
    ++m_occupiedSlots;
    return true;
}

void BoundedSocketServer::releaseSlot()
{
    std::scoped_lock lock(m_slotMutex);
    --m_occupiedSlots;
}

void BoundedSocketServer::registerActiveClient(int clientFileDescriptor)
{
    std::scoped_lock lock(m_activeClientsMutex);
    m_activeClientFileDescriptors.push_back(clientFileDescriptor);
}

void BoundedSocketServer::unregisterActiveClient(int clientFileDescriptor)
{
    std::scoped_lock lock(m_activeClientsMutex);
    auto& v = m_activeClientFileDescriptors;
    std::erase(v, clientFileDescriptor);
}

void BoundedSocketServer::registerWorkerThread(std::thread worker)
{
    std::scoped_lock lock(m_lifecycleMutex);
    m_workerThreads.push_back(std::move(worker));
}

void BoundedSocketServer::markWorkerFinished(std::thread::id workerID)
{
    std::scoped_lock lock(m_lifecycleMutex);
    if (m_isStopping)
        return; // stop() owns every worker from here; it joins them all
    m_finishedWorkerIDs.push_back(workerID);
}

void BoundedSocketServer::reapFinishedWorkers()
{
    std::vector<std::thread> finished;
    {
        std::scoped_lock lock(m_lifecycleMutex);
        if (m_finishedWorkerIDs.empty())
            return;
        for (const std::thread::id workerID : m_finishedWorkerIDs) {
            const auto match = std::ranges::find_if(m_workerThreads,
                [workerID](const std::thread& worker) { return worker.get_id() == workerID; });
            if (match != m_workerThreads.end()) {
                finished.push_back(std::move(*match));
                m_workerThreads.erase(match);
            }
        }
        m_finishedWorkerIDs.clear();
    }
    // Outside the lock: a worker reporting itself finished takes the same one,
    // and this loop is what it would be waiting on.
    for (std::thread& worker : finished)
        if (worker.joinable())
            worker.join();
}

void BoundedSocketServer::run()
{
    {
        std::scoped_lock lock(m_lifecycleMutex);
        if (m_isStopping || m_isRunning)
            return;
        m_isRunning = true;
    }
    for (;;) {
        {
            std::scoped_lock lock(m_lifecycleMutex);
            if (m_isStopping)
                break;
        }
        // Before waiting for a peer: a server that is refusing
        // connections is a server whose previous clients are the ones worth
        // clearing, and this is the moment their threads have finished.
        reapFinishedWorkers();
        std::array<PAL::Watch, 2> watches { {
            { &m_listenFileDescriptor, PAL::Ready::Readable, PAL::Ready::None },
            { m_acceptWaker ? &m_acceptWaker->handle() : nullptr, PAL::Ready::Readable, PAL::Ready::None },
        } };
        // The bounded timeout is also the stop path when resource pressure
        // prevented creating a waker. The listener remains owned until exit.
        if (PAL::waitForReadiness(std::span(watches).first(m_acceptWaker ? 2 : 1), m_acceptWaker ? -1 : 100) < 0)
            break;
        if (m_acceptWaker && PAL::contains(watches[1].result, PAL::Ready::Readable))
            m_acceptWaker->drain();
        {
            std::scoped_lock lock(m_lifecycleMutex);
            if (m_isStopping)
                break;
        }
        if (!PAL::contains(watches[0].result, PAL::Ready::Readable)) {
            if (watches[0].result != PAL::Ready::None)
                break;
            continue;
        }
        PAL::UnixFileDescriptor clientFileDescriptor;
        const PAL::SocketAcceptOutcome accepted = PAL::acceptConnection(m_listenFileDescriptor, &clientFileDescriptor);
        if (accepted == PAL::SocketAcceptOutcome::Interrupted)
            continue;
        if (accepted == PAL::SocketAcceptOutcome::Retry) {
            std::unique_lock lock(m_lifecycleMutex);
            ++m_acceptRetryCount;
            if (m_acceptRetryWake.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_isStopping; }))
                break;
            continue;
        }
        if (accepted != PAL::SocketAcceptOutcome::Connected)
            break; // stop() broke the listener, or it failed outright
        if (!m_verifyPeer || !m_verifyPeer(clientFileDescriptor))
            continue;
        // Bound concurrent clients: a held-open idle/slow client occupies only
        // one of maxConcurrentClients slots, so it cannot starve every other
        // client as it would under a fully-synchronous accept-then-serve loop.
        // A connection with no free slot is refused immediately —
        // queuing it would just relocate the same unbounded-wait DoS.
        if (!tryAcquireSlot())
            continue;
        const int borrowedClientFileDescriptor = clientFileDescriptor.get();
        registerActiveClient(borrowedClientFileDescriptor);
        std::thread worker([this, clientFileDescriptor = std::move(clientFileDescriptor)]() mutable {
            serveClient(clientFileDescriptor.get());
            unregisterActiveClient(clientFileDescriptor.get());
            releaseSlot();
            // Last, so the accept loop's join returns immediately.
            markWorkerFinished(std::this_thread::get_id());
        });
        registerWorkerThread(std::move(worker));
    }
    {
        std::scoped_lock lock(m_lifecycleMutex);
        m_isRunning = false;
        m_runStopped.notify_all();
    }
}

void BoundedSocketServer::serveClient(int clientFileDescriptor)
{
    // Idle/partial-request deadline: a client that connects and never
    // completes a line (or never sends anything) is force-closed after this
    // timeout instead of holding its slot forever. Bounds every blocking
    // read() below.
    PAL::setReceiveTimeoutSeconds(clientFileDescriptor, m_options.idleTimeoutSeconds);
    // The same bound on the way out. A client can send a valid request and
    // then stop reading: once its receive buffer and our send buffer are both
    // full, the reply blocks with no deadline, and the slot the read bound
    // exists to reclaim is held by a peer doing nothing at all.
    PAL::setSendTimeoutSeconds(clientFileDescriptor, m_options.idleTimeoutSeconds);

    std::string buffer;
    std::array<char, 8192> chunk;
    for (;;) {
        std::size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            std::string response = m_handleLine(line);
            if (!response.empty()) {
                response.push_back('\n');
                if (!PAL::writeSocketAll(clientFileDescriptor, response))
                    return;
            }
        }
        // Oversized newline-free line: refuse to grow the buffer
        // further and close, rather than accumulating without bound.
        if (buffer.size() > m_options.maxLineLength)
            return;
        std::size_t received = 0;
        const PAL::SocketReadOutcome outcome = PAL::readSocket(clientFileDescriptor, chunk, &received);
        if (outcome == PAL::SocketReadOutcome::Bytes) {
            buffer.append(chunk.data(), received);
            continue;
        }
        if (outcome == PAL::SocketReadOutcome::Interrupted)
            continue;
        break; // peer closed, the idle bound elapsed, or the socket is unusable
    }
}

void BoundedSocketServer::stop()
{
    {
        std::scoped_lock lock(m_lifecycleMutex);
        if (m_isStopping)
            return;
        m_isStopping = true;
    }
    m_acceptRetryWake.notify_all();
    if (m_acceptWaker)
        m_acceptWaker->wake();

    std::vector<std::thread> workers;
    {
        std::unique_lock lock(m_lifecycleMutex);
        // The descriptor stays owned while accept borrows it. Once run has
        // stopped, no worker can be registered after this collection either.
        m_runStopped.wait(lock, [this] { return !m_isRunning; });
        workers = std::move(m_workerThreads);
        m_workerThreads.clear();
        m_finishedWorkerIDs.clear();
    }
    m_listenFileDescriptor.reset();
    {
        std::scoped_lock lock(m_activeClientsMutex);
        for (int fileDescriptor : m_activeClientFileDescriptors)
            PAL::shutdownSocket(fileDescriptor);
    }
    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
}

#if UBIQUITTY_ENABLE_TEST_HOOKS
std::size_t RetainedWorkerAccess::acceptRetryCount(BoundedSocketServer& server)
{
    std::scoped_lock lock(server.m_lifecycleMutex);
    return server.m_acceptRetryCount;
}

std::size_t RetainedWorkerAccess::retainedWorkerCount(BoundedSocketServer& server)
{
    std::scoped_lock lock(server.m_lifecycleMutex);
    return server.m_workerThreads.size();
}
#endif

} // namespace Ubiquitty::Session
