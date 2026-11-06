// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <csignal>

// test_boundedsocketserver.cpp -- Session/BoundedSocketServer DoS guards.
//
// An enabled control socket was trivially monopolized by one same-uid
// client (synchronous accept-then-serve blocked all others), and a
// newline-free request grew the server's buffer without bound. This drives a
// REAL BoundedSocketServer over a REAL AF_UNIX socket (bound in a temp dir)
// with a background thread running run(), and real client sockets connecting
// to it, proving:
//   (a) CONCURRENCY BOUND + RECOVERY -- with maxConcurrentClients=2, two idle
//       clients occupy both slots; a third is refused (closed immediately,
//       not queued); closing one idle client frees its slot for a fourth.
//   (b) OVERSIZED LINE -- a client that sends more than maxLineLength bytes
//       with no newline is disconnected (EOF on its end), not accumulated
//       without bound.
//   (c) IDLE TIMEOUT -- a client that sends nothing is disconnected after
//       idleTimeoutSeconds (a short timeout here), not held forever.
//   (d) stop() FORCE-CLOSES ACTIVE CLIENTS -- an idle client (holding a slot,
//       well under its own idle timeout) is disconnected the instant stop()
//       runs, not left to outlive the server.
//   (e) NORMAL FUNCTION PRESERVED -- a well-behaved client's request still
//       gets exactly the handler's response.
//
// Exit 0 on success.

#include "Core/Session/BoundedSocketServer.h"
#include "PAL/Socket.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using Ubiquitty::Session::BoundedSocketServer;

namespace {

// Bind + listen a fresh AF_UNIX socket at a unique temp path. Returns the
// listening fd (caller owns it -- BoundedSocketServer takes ownership once
// constructed) and the path (caller unlinks when done).
int bindListeningSocket(std::string& outPath)
{
    char templatePath[] = "/tmp/ub-boundedsocketserver-XXXXXX";
    const int templateFileDescriptor = ::mkstemp(templatePath);
    if (templateFileDescriptor < 0)
        return -1;
    ::close(templateFileDescriptor);
    ::unlink(templatePath); // reuse the unique name as a socket path, not a file
    outPath = templatePath;

    const int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return -1;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", outPath.c_str());
    if (::bind(fileDescriptor, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fileDescriptor, 16) != 0) {
        ::close(fileDescriptor);
        return -1;
    }
    return fileDescriptor;
}

int connectClient(const std::string& path)
{
    const int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return -1;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::connect(fileDescriptor, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fileDescriptor);
        return -1;
    }
    return fileDescriptor;
}

// A connect() to a bound-but-unaccepted or immediately-refused server can
// still succeed at the socket layer (the listen backlog absorbs it) even
// though the server closes it right after accept()+refuse — so "refused"
// is observed as an immediate EOF (read() returns 0) rather than connect()
// failing. Polls briefly since the refusal happens on a background thread.
bool observesImmediateEOF(int fileDescriptor, int timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    char buffer[1];
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(fileDescriptor, buffer, sizeof(buffer));
        if (n == 0)
            return true; // EOF -- the server closed it
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (n < 0)
            return false;
        return false; // got real data -- not a refusal
    }
    return false;
}

// True once the server has accepted this client and answered it. A fixed sleep
// waits for exactly that and guesses how long it takes; a round-trip observes
// it instead, so the test costs what the server costs rather than a constant.
bool servesRoundTrip(int fileDescriptor, int timeoutMilliseconds)
{
    const std::string request = "ping\n";
    if (::write(fileDescriptor, request.data(), request.size()) != static_cast<ssize_t>(request.size()))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    std::string received;
    char buffer[64];
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(fileDescriptor, buffer, sizeof(buffer));
        if (n > 0) {
            received.append(buffer, static_cast<std::size_t>(n));
            if (received.find('\n') != std::string::npos)
                return received == "echo:ping\n";
        } else if (n == 0) {
            return false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void setNonBlocking(int fileDescriptor)
{
    // Non-blocking so observesImmediateEOF's read() never blocks past its poll.
    int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags != -1)
        ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

TEST(boundedsocketserverCases)
{
    // A client here writes to sockets the server may already have refused and
    // closed, which is EPIPE — and EPIPE's default action kills the process.
    // Every real client of a bounded server has to make this same arrangement.
    ::signal(SIGPIPE, SIG_IGN);

    // (a) CONCURRENCY BOUND + RECOVERY.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.maxConcurrentClients = 2;
        options.idleTimeoutSeconds = 30; // long -- this test controls lifecycle explicitly
        BoundedSocketServer server(
            listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, // accept every peer -- uid check is the control server's concern
            [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        const int clientA = connectClient(socketPath);
        const int clientB = connectClient(socketPath);
        CHECK(clientA >= 0 && clientB >= 0);
        setNonBlocking(clientA);
        setNonBlocking(clientB);
        // Both are accepted and served, so both hold a slot. That is the state
        // the refusal below depends on.
        CHECK(servesRoundTrip(clientA, 2000));
        CHECK(servesRoundTrip(clientB, 2000));

        const int clientC = connectClient(socketPath);
        CHECK(clientC >= 0); // the connect() itself succeeds (kernel backlog)...
        setNonBlocking(clientC);
        CHECK(observesImmediateEOF(clientC, 500)); // ...but the server refuses it: immediate EOF
        ::close(clientC);

        // Free clientA's slot by closing it (simulating a client disconnect).
        ::close(clientA);

        // The server releases the slot on its own thread. Retry until a fresh
        // client is served rather than refused; a refused one is closed by the
        // server, so a failed attempt holds nothing.
        bool acceptedAfterRelease = false;
        const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!acceptedAfterRelease && std::chrono::steady_clock::now() < releaseDeadline) {
            const int clientD = connectClient(socketPath);
            CHECK(clientD >= 0);
            setNonBlocking(clientD);
            acceptedAfterRelease = servesRoundTrip(clientD, 100);
            ::close(clientD);
        }
        CHECK(acceptedAfterRelease); // a free slot -- not refused this time

        ::close(clientB);
        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }

    // (b) OVERSIZED LINE.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.maxLineLength = 1024; // small, for a fast test
        options.idleTimeoutSeconds = 30;
        BoundedSocketServer server(listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        const int client = connectClient(socketPath);
        CHECK(client >= 0);
        std::string oversized(options.maxLineLength + 1, 'x'); // no '\n' -- never a complete line
        CHECK(::write(client, oversized.data(), oversized.size()) == static_cast<ssize_t>(oversized.size()));
        setNonBlocking(client);
        CHECK(observesImmediateEOF(client, 1000)); // closed for exceeding maxLineLength
        ::close(client);

        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }

    // (c) IDLE TIMEOUT.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.idleTimeoutSeconds = 1; // short, for a fast test
        BoundedSocketServer server(listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        const int client = connectClient(socketPath);
        CHECK(client >= 0);
        setNonBlocking(client);
        // Sends nothing. Must be disconnected once idleTimeoutSeconds elapses.
        CHECK(observesImmediateEOF(client, 2500));
        ::close(client);

        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }

    // (d) stop() FORCE-CLOSES ACTIVE CLIENTS.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.idleTimeoutSeconds = 30; // long -- stop() must close it well before this
        BoundedSocketServer server(listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        const int client = connectClient(socketPath);
        CHECK(client >= 0);
        setNonBlocking(client);
        CHECK(servesRoundTrip(client, 2000)); // accepted and served: now "active"

        server.stop(); // must force-close the still-idle, still-connected client
        CHECK(observesImmediateEOF(client, 1000));
        ::close(client);

        serverThread.join();
        ::unlink(socketPath.c_str());
    }

    // (e) NORMAL FUNCTION PRESERVED.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        BoundedSocketServer server(listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        const int client = connectClient(socketPath);
        CHECK(client >= 0);
        const std::string request = "hello\n";
        CHECK(::write(client, request.data(), request.size()) == static_cast<ssize_t>(request.size()));

        char buffer[256] = {};
        ssize_t total = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (total < 10 && std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::read(client, buffer + total, sizeof(buffer) - total);
            if (n > 0)
                total += n;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(std::string(buffer, static_cast<std::size_t>(total)) == "echo:hello\n");
        ::close(client);

        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }
    // (e) A client that stops reading releases its slot anyway.
    //
    // The receive bound reclaims a slot from a peer that says nothing. It does
    // nothing about a peer that speaks and then stops listening: once its
    // receive buffer and the server's send buffer are both full, the reply
    // blocks, and the worker holds its slot with no deadline at all. One such
    // peer per slot denies the socket to everyone.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.maxConcurrentClients = 1; // one slot, so holding it is total
        options.idleTimeoutSeconds = 1; // the bound under test
        options.maxLineLength = 1024 * 1024;
        // "flood" answers with a reply far larger than any socket buffer, so
        // the write cannot complete into a peer that never reads; every other
        // line gets the ordinary echo, which is what the second client checks.
        BoundedSocketServer server(
            listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; },
            [](const std::string& line) {
                return line == "flood" ? std::string(8 * 1024 * 1024, 'r') : "echo:" + line;
            });
        std::thread serverThread([&server] { server.run(); });

        const int silentReader = connectClient(socketPath);
        CHECK(silentReader >= 0);
        // Ask for the reply, then never read a byte of it. The kernel's own
        // buffers absorb the first part; the rest blocks the worker.
        CHECK(::write(silentReader, "flood\n", 6) == 6);

        // The slot must come back on the send bound alone. Poll for a second
        // client being served rather than refused.
        bool didServeWhileTheFirstHoldsOn = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!didServeWhileTheFirstHoldsOn && std::chrono::steady_clock::now() < deadline) {
            const int later = connectClient(socketPath);
            if (later >= 0) {
                setNonBlocking(later);
                didServeWhileTheFirstHoldsOn = servesRoundTrip(later, 200);
                ::close(later);
            }
        }
        CHECK(didServeWhileTheFirstHoldsOn);

        ::close(silentReader);
        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }

    // (f) Finished workers are joined as they finish.
    //
    // A std::thread that has run to completion still owns its stack and its
    // control block until someone joins it. A server that only joins at
    // shutdown keeps the remains of every connection it ever accepted, and the
    // agent reconnects once per verb by design — so a long-lived daemon
    // accumulates thousands of them. The slot bound does not cover this: a
    // finished worker has already released its slot.
    {
        std::string socketPath;
        const int listenFileDescriptor = bindListeningSocket(socketPath);
        CHECK(listenFileDescriptor >= 0);

        BoundedSocketServer::Options options;
        options.maxConcurrentClients = 4;
        options.idleTimeoutSeconds = 30;
        BoundedSocketServer server(
            listenFileDescriptor, options, [](const Ubiquitty::PAL::FileDescriptor&) { return true; },
            [](const std::string& line) { return "echo:" + line; });
        std::thread serverThread([&server] { server.run(); });

        // Serially, so at most one is ever live: any thread the process still
        // holds afterwards is one that finished and was never joined.
        for (int connection = 0; connection < 64; ++connection) {
            const int client = connectClient(socketPath);
            CHECK(client >= 0);
            setNonBlocking(client);
            CHECK(servesRoundTrip(client, 2000));
            ::close(client);
        }
        // One more connection, whose accept is the point the loop reaps.
        const int last = connectClient(socketPath);
        CHECK(last >= 0);
        setNonBlocking(last);
        CHECK(servesRoundTrip(last, 2000));
        ::close(last);

        // The threads the server still HOLDS. An exited pthread leaves the
        // OS thread table at once, so nothing outside the process can see this:
        // what accumulates is the std::thread objects and what they own, one
        // per connection ever accepted.
        CHECK(Ubiquitty::Session::RetainedWorkerAccess::retainedWorkerCount(server) < 16);

        server.stop();
        serverThread.join();
        ::unlink(socketPath.c_str());
    }
}

TEST(acceptDistinguishesResourcePressureFromAnUnusableListener)
{
    using namespace Ubiquitty;
    std::string path;
    PAL::UnixFileDescriptor listener(bindListeningSocket(path));
    REQUIRE(listener);
    PAL::UnixFileDescriptor peer(connectClient(path));
    REQUIRE(peer);
    struct rlimit original {};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    struct rlimit bounded = original;
    bounded.rlim_cur = std::min<rlim_t>(original.rlim_cur, 128);
    std::vector<PAL::UnixFileDescriptor> held;
    held.reserve(static_cast<std::size_t>(bounded.rlim_cur));
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &bounded) == 0);
    while (true) {
        PAL::UnixFileDescriptor descriptor(::open("/dev/null", O_RDONLY));
        if (!descriptor)
            break;
        held.push_back(std::move(descriptor));
    }
    const int exhaustedError = errno;
    PAL::UnixFileDescriptor accepted;
    const auto underPressure = PAL::acceptConnection(listener, &accepted);
    held.clear();
    const int restored = ::setrlimit(RLIMIT_NOFILE, &original);
    CHECK(restored == 0);
    CHECK(exhaustedError == EMFILE);
    CHECK(underPressure == PAL::SocketAcceptOutcome::Retry);
    CHECK(!accepted);
    // The kernel may discard the queued peer on EMFILE. A new connection
    // must still be accepted by the same listener after pressure lifts.
    peer.reset();
    peer.reset(connectClient(path));
    REQUIRE(peer);
    CHECK(PAL::acceptConnection(listener, &accepted) == PAL::SocketAcceptOutcome::Connected);
    CHECK(accepted);
    CHECK((::fcntl(accepted.get(), F_GETFL, 0) & O_NONBLOCK) == 0);
    // Platforms that preserve the first pending peer can still have it queued.
    const auto remaining = PAL::acceptConnection(listener, nullptr);
    CHECK(remaining == PAL::SocketAcceptOutcome::Connected || remaining == PAL::SocketAcceptOutcome::Retry);
    CHECK(PAL::acceptConnection(listener, nullptr) == PAL::SocketAcceptOutcome::Retry);
    PAL::UnixFileDescriptor invalid;
    CHECK(PAL::acceptConnection(invalid, nullptr) == PAL::SocketAcceptOutcome::Unusable);
    PAL::UnixFileDescriptor ordinaryFile(::open("/dev/null", O_RDONLY));
    CHECK(PAL::acceptConnection(ordinaryFile, nullptr) == PAL::SocketAcceptOutcome::Unusable);
    PAL::UnixFileDescriptor datagram(::socket(AF_UNIX, SOCK_DGRAM, 0));
    REQUIRE(datagram);
    CHECK(PAL::acceptConnection(datagram, nullptr) == PAL::SocketAcceptOutcome::Unusable);
    ::unlink(path.c_str());
}

TEST(transientAcceptFailuresRecoverAndStopDuringBackoff)
{
    using Ubiquitty::Session::RetainedWorkerAccess;
    for (const bool shouldRecover : { false, true }) {
        std::string path;
        const int listener = bindListeningSocket(path);
        REQUIRE(listener >= 0);
        BoundedSocketServer server(listener, {}, [](const Ubiquitty::PAL::FileDescriptor&) { return true; }, [](const std::string& line) { return "echo:" + line; });
        Ubiquitty::PAL::UnixFileDescriptor peer(connectClient(path));
        REQUIRE(peer);
        struct rlimit original {};
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
        struct rlimit bounded = original;
        bounded.rlim_cur = std::min<rlim_t>(original.rlim_cur, 128);
        std::vector<Ubiquitty::PAL::UnixFileDescriptor> held;
        held.reserve(static_cast<std::size_t>(bounded.rlim_cur));
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &bounded) == 0);
        while (true) {
            Ubiquitty::PAL::UnixFileDescriptor descriptor(::open("/dev/null", O_RDONLY));
            if (!descriptor)
                break;
            held.push_back(std::move(descriptor));
        }
        std::thread worker([&server] { server.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!RetainedWorkerAccess::acceptRetryCount(server) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(RetainedWorkerAccess::acceptRetryCount(server) > 0);
        if (!shouldRecover)
            server.stop();
        held.clear();
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &original) == 0);
        if (shouldRecover) {
            peer.reset();
            peer.reset(connectClient(path));
            CHECK(peer);
            setNonBlocking(peer.get());
            CHECK(servesRoundTrip(peer.get(), 2000));
        }
        server.stop();
        worker.join();
        ::unlink(path.c_str());
    }
}

UBIQUITTY_TEST_MAIN()
