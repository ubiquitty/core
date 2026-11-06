// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/daemon_start_arbitration.cpp — two starters cannot both decide
// they own the socket.
//
// bindSocket recovers a stale socket by probing it: connect, and if the connect
// is refused, unlink and bind. A daemon that has bound and not yet called
// listen() refuses connects too, so with two starters racing, the second can
// read "stale" about the first and unlink the socket out from under it. The
// first then runs forever on an unlinked inode: alive, holding sessions, and
// unreachable by every later attach.
//
// The window is microseconds wide, so racing two processes proves nothing
// either way. What is deterministic is the arbitration itself: while a starter
// holds the lock, another must not start, and must leave the socket alone.
//
// Cases:
//   - a daemon starts, and its lock file exists
//   - with the lock held by this test, start() refuses
//   - a refused start leaves an existing socket in place
//   - once the lock is released, start() succeeds again
//   - a daemon with no descriptors left refuses a pending connection rather
//     than leaving it in the backlog to be re-offered forever
//

#include "Assertions.h"

#include "Platform/Host/Daemon.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using Ubiquitty::Host::Daemon;

namespace {

std::string makeControlDirectory()
{
    char pattern[] = "/tmp/ubiquitty-daemon-arb-XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    CHECK(directory != nullptr);
    return directory ? std::string(directory) : std::string();
}

bool pathExists(const std::string& path)
{
    struct stat entry {};
    return ::stat(path.c_str(), &entry) == 0;
}

// What a crashed daemon leaves behind: a bound socket path with nothing
// listening. Closing the descriptor does not unlink it, which is the whole
// reason bindSocket has a stale-socket probe at all.
void leaveStaleSocket(const std::string& path)
{
    const int socketDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(socketDescriptor >= 0);
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    CHECK(::bind(socketDescriptor, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    ::close(socketDescriptor);
}

} // namespace

int main()
{
    const std::string controlDirectory = makeControlDirectory();
    if (controlDirectory.empty())
        return 1;
    const std::string socketPath = controlDirectory + "/sock";
    const std::string processIDPath = controlDirectory + "/pid";
    const std::string lockPath = socketPath + ".lock";

    // A daemon that starts normally claims the lock beside its socket.
    {
        Daemon daemon;
        CHECK(daemon.start(socketPath.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24));
        CHECK(pathExists(socketPath));
        CHECK(pathExists(lockPath));
    }
    // A clean shutdown unlinks its own socket, so the stale one a crash would
    // have left is planted here — that is the state the probe exists for, and
    // the state in which the loser of a race does the damage.
    CHECK(!pathExists(socketPath));
    leaveStaleSocket(socketPath);
    CHECK(pathExists(socketPath));

    // With the lock held here, a starter must refuse rather than probe, decide
    // "stale", and unlink. This is the loser of the race, made deterministic.
    {
        const int heldLock = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0600);
        CHECK(heldLock >= 0);
        CHECK(::flock(heldLock, LOCK_EX | LOCK_NB) == 0);

        Daemon blocked;
        CHECK(!blocked.start(socketPath.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24));
        // The socket it did not win is still there. Without the arbitration
        // this is the assertion that fails: the loser unlinks it.
        CHECK(pathExists(socketPath));

        CHECK(::flock(heldLock, LOCK_UN) == 0);
        ::close(heldLock);
    }

    // Released: the next starter recovers the stale socket.
    {
        Daemon daemon;
        CHECK(daemon.start(socketPath.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24));
        CHECK(pathExists(socketPath));
    }

    // A daemon out of descriptors refuses connections instead of spinning on
    // them. The pending connection stays in the backlog and keeps the listener
    // readable, so a level-triggered poll hands it back on every pass — a full
    // CPU with every waiting client hung in connect() rather than told no.
    // Spending the reserve descriptor buys the one accept that drains it.
    {
        const std::string exhaustionSocket = controlDirectory + "/exhaust";
        const std::string exhaustionProcessID = controlDirectory + "/exhaust.pid";
        Daemon daemon;
        CHECK(daemon.start(exhaustionSocket.c_str(), exhaustionProcessID.c_str(), "/bin/sh", 80, 24));

        // Connect first, while descriptors are still available: this is the
        // client the exhausted daemon has to answer.
        const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(client >= 0);
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, exhaustionSocket.c_str(), sizeof(address.sun_path) - 1);
        CHECK(::connect(client, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);

        // Lower the ceiling rather than try to reach it. Opening until the
        // table ends is a race this test loses on a modern host: `ulimit -n` is
        // 524288 on current Fedora, so the loop below stopped at its own safety
        // cap with roughly four hundred thousand descriptors still free, the
        // precondition failed, and the case never reached the daemon behaviour
        // it exists to check. Raising the cap would fix one machine and break on
        // the next, and open 524288 files to do it.
        //
        // The daemon runs in this process, so a limit set here is the ceiling its accept
        // loop actually hits: the refusal under test is the daemon's own.
        struct rlimit savedDescriptorLimit {};
        CHECK(::getrlimit(RLIMIT_NOFILE, &savedDescriptorLimit) == 0);
        // Descriptor numbers are the lowest free slot, so a freshly opened one
        // names how many are already in use. Portable, and no /proc walk.
        const int inUse = ::open("/dev/null", O_RDONLY);
        CHECK(inUse >= 0);
        ::close(inUse);
        struct rlimit tightDescriptorLimit = savedDescriptorLimit;
        tightDescriptorLimit.rlim_cur = static_cast<rlim_t>(inUse) + 16;
        CHECK(::setrlimit(RLIMIT_NOFILE, &tightDescriptorLimit) == 0);

        // Fill the descriptor table, so the daemon's own accept fails with
        // EMFILE — the condition, produced rather than simulated. The ceiling
        // above makes this a handful of opens rather than a hundred thousand.
        std::vector<int> hogs;
        for (;;) {
            const int hog = ::open("/dev/null", O_RDONLY);
            if (hog < 0)
                break;
            hogs.push_back(hog);
            if (hogs.size() > 100000)
                break; // a backstop; the rlimit is what ends this now
        }
        CHECK(!hogs.empty());

        // The precondition, stated rather than assumed: with a descriptor
        // free the daemon's accept succeeds, the branch under test never runs,
        // and the case passes against the bug it exists to catch.
        const int spare = ::open("/dev/null", O_RDONLY);
        CHECK(spare < 0);
        if (spare >= 0)
            ::close(spare);

        // Several passes, because the real loop makes several: spending the
        // reserve frees one descriptor, and the daemon's own threads can take
        // it before the accept does. What must not happen is the connection
        // surviving every pass, which is the spin.
        struct pollfd listener {};
        listener.fd = Ubiquitty::Host::AcceptUnderExhaustionAccess::listenDescriptor(daemon);
        listener.events = POLLIN;
        CHECK(listener.fd >= 0);
        int stillPending = 1;
        for (int pass = 0; pass < 8 && stillPending != 0; ++pass) {
            Ubiquitty::Host::AcceptUnderExhaustionAccess::acceptOnce(daemon);
            stillPending = ::poll(&listener, 1, 0);
        }

        // Give the descriptors back before asserting, so the assertion itself
        // is not fighting for one. The ceiling goes back with them: the cases
        // after this one share the process and would otherwise inherit a
        // descriptor table sized for this test.
        for (const int hog : hogs)
            ::close(hog);
        CHECK(::setrlimit(RLIMIT_NOFILE, &savedDescriptorLimit) == 0);

        // The question that discriminates is the backlog, not the client: a
        // client under exhaustion is dropped either way, but a connection left
        // pending keeps the listener readable — which is precisely what makes
        // a level-triggered poll spin on it forever.
        CHECK(stillPending == 0); // drained, rather than re-offered on every pass
        ::close(client);
        ::unlink(exhaustionSocket.c_str());
        ::unlink((exhaustionSocket + ".lock").c_str());
        ::unlink(exhaustionProcessID.c_str());
    }

    ::unlink(socketPath.c_str());
    ::unlink(lockPath.c_str());
    ::unlink(processIDPath.c_str());
    ::rmdir(controlDirectory.c_str());

    if (failures == 0)
        std::printf("daemon_start_arbitration: all passed\n");
    return failures ? 1 : 0;
}
