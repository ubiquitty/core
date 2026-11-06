// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A portable, bounded newline-delimited request/response socket server.
// Serves an already-listening socket, bounding every resource a naive loop leaves open:
//   - CONCURRENCY: only maxConcurrentClients at once; an over-limit connection is refused (not queued).
//   - MEMORY: a line longer than maxLineLength closes the connection.
//   - TIME: idleTimeoutSeconds bounds every blocking read().
//   - SHUTDOWN: stop() force-closes every active client, not just the listener.
// Portable POSIX, headless-usable. Protocol/Control/Server supplies peer-uid verify +
// the JSON-RPC line handler as callbacks.

#include "PAL/FileDescriptor.h"
#include "PAL/Poll.h"
#include "PAL/UnixFileDescriptor.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Ubiquitty::Session {

class BoundedSocketServer {
public:
    struct Options {
        // No more than this many clients served concurrently. A connection
        // arriving with every slot taken is closed immediately.
        std::size_t maxConcurrentClients = 4;
        // A single request line longer than this (with no newline yet found)
        // closes the connection rather than growing the buffer further.
        std::size_t maxLineLength = 64 * 1024;
        // A client that sends nothing (or an incomplete line) for this long is
        // disconnected. Bounds every blocking read() via SO_RCVTIMEO.
        int idleTimeoutSeconds = 30;
    };

    /// Decides whether a freshly accepted connection may proceed.
    ///
    /// Takes the handle rather than a descriptor number so the decision can be
    /// made through the PAL — ``PAL::peerIsSameUser`` is what a local control
    /// socket wants — instead of every caller reaching for the platform's own
    /// credential call.
    ///
    /// - Returns: False to reject and close the connection.
    using PeerVerifyCallback = std::function<bool(const PAL::FileDescriptor&)>;
    // Parses + dispatches one newline-delimited request line, returning the
    // response to write back (without a trailing newline; empty = no reply,
    // e.g. a blank line or a notification).
    using LineHandlerCallback = std::function<std::string(const std::string& line)>;

    BoundedSocketServer(int listenFileDescriptor, Options options, PeerVerifyCallback verifyPeer, LineHandlerCallback handleLine);
    ~BoundedSocketServer();

    BoundedSocketServer(const BoundedSocketServer&) = delete;
    BoundedSocketServer& operator=(const BoundedSocketServer&) = delete;

    // Runs the accept loop on the calling thread until stop() or an unusable
    // listener. Transient failures back off; stop() joins the client workers.
    void run();

    // Closes the listening socket and every currently-active client
    // connection (unblocking any in-flight blocking read()), then waits for
    // run() to return and every client thread to join. Idempotent.
    void stop();

private:
    void serveClient(int clientFileDescriptor);
    // Non-blocking: true (occupies a slot) if one was free, else false (the caller
    // must refuse the connection, never queue/wait).
    bool tryAcquireSlot();
    void releaseSlot();
    void registerActiveClient(int clientFileDescriptor);
    void unregisterActiveClient(int clientFileDescriptor);
    /// Joins the workers that have finished, freeing what an exited thread
    /// still holds.
    ///
    /// Called from the accept loop, where the cost is bounded and predictable:
    /// each join returns at once, because a thread only reports itself
    /// finished as the last thing it does.
    void reapFinishedWorkers();

    /// Records that this worker is done and may be joined.
    ///
    /// - Parameter workerID: The finishing thread's id.
    void markWorkerFinished(std::thread::id workerID);

    /// Takes ownership of one client worker before the accept loop can finish.
    /// Stop waits for that loop before collecting and joining these workers.
    void registerWorkerThread(std::thread worker);

    PAL::UnixFileDescriptor m_listenFileDescriptor;
    std::unique_ptr<PAL::Waker> m_acceptWaker;
    Options m_options;
    PeerVerifyCallback m_verifyPeer;
    LineHandlerCallback m_handleLine;

    std::mutex m_slotMutex;
    std::size_t m_occupiedSlots = 0;

    std::mutex m_activeClientsMutex;
    std::vector<int> m_activeClientFileDescriptors; // borrowed, shutdown-only handles

#if UBIQUITTY_ENABLE_TEST_HOOKS
    // Test-only seam. What leaks here is invisible from outside the process:
    // an exited pthread is gone from the OS thread table while its std::thread
    // object, and what that object still owns, are held by this vector.
    friend struct RetainedWorkerAccess;
#endif

    std::size_t m_acceptRetryCount = 0;
    std::condition_variable m_acceptRetryWake;
    std::condition_variable m_runStopped;
    std::mutex m_lifecycleMutex; // guards run/stop state, retry count, and worker ownership
    bool m_isStopping = false;
    bool m_isRunning = false;
    std::vector<std::thread> m_workerThreads;
    // Workers that have finished serving and are waiting to be joined. A
    // std::thread that has run to completion still owns its stack and its
    // thread control block until someone joins it, so a server that only joins
    // at shutdown accumulates the remains of every connection it ever
    // accepted — and the agent reconnects once per verb by design.
    std::vector<std::thread::id> m_finishedWorkerIDs;
};

#if UBIQUITTY_ENABLE_TEST_HOOKS
/// Test-only accessor for the worker threads a server still holds.
struct RetainedWorkerAccess {
    /// Returns how many worker threads the server has not yet joined.
    ///
    /// - Parameter server: The server to inspect.
    /// - Returns: The retained count, including any worker still serving.
    static std::size_t retainedWorkerCount(BoundedSocketServer& server);
    /// Returns how many transient accept failures reached the retry wait.
    ///
    /// - Parameter server: The server to inspect.
    /// - Returns: The number of retry waits entered by the accept loop.
    static std::size_t acceptRetryCount(BoundedSocketServer& server);
};
#endif

} // namespace Ubiquitty::Session
