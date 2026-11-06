// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_controlpaths.cpp -- Session/ControlPaths control-directory security.

#include "Core/Session/ControlPaths.h"

#include "Platform/Host/HostPaths.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static unsigned modeOf(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<unsigned>(st.st_mode & 0777);
}

static bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

static bool pathExists(const std::string& path)
{
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

enum class SocketListenMode {
    Listen,
    BindOnly
};

// Bind (and optionally listen()) a real AF_UNIX socket at `path`. Returns the
// listening fd on success (caller closes it to make the socket stale/dead), or
// -1 on failure.
static int bindRealSocket(const std::string& path, SocketListenMode mode)
{
    int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return -1;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fileDescriptor, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fileDescriptor);
        return -1;
    }
    if (mode == SocketListenMode::Listen && ::listen(fileDescriptor, 1) != 0) {
        ::close(fileDescriptor);
        ::unlink(path.c_str());
        return -1;
    }
    return fileDescriptor;
}

int main()
{
    char templatePath[] = "/tmp/ub-controlpaths-XXXXXX";
    char* temporaryRoot = ::mkdtemp(templatePath);
    CHECK(temporaryRoot != nullptr);
    if (!temporaryRoot)
        return 1;

    const std::string root = temporaryRoot;
    const std::string control = root + "/control";
    ::setenv("UBIQUITTY_HOME", control.c_str(), 1);

    CHECK(Ubiquitty::Session::ensureControlDirectory());
    CHECK(modeOf(control) == 0700);

    char path[1024];
    // The unversioned "host.sock" is REJECTED: a shared well-known leaf is the
    // cross-binary-bleed surface.
    CHECK(Ubiquitty::Host::socketPathForName("host.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(!Ubiquitty::Host::isHostSocketLeaf("host.sock"));
    CHECK(!Ubiquitty::Host::isHostSocketLeaf("host-0123456789abcdef.sock.old"));
    CHECK(Ubiquitty::Host::isHostSocketLeaf("host-0123456789abcdef.sock"));
    CHECK(Ubiquitty::Host::socketPathForName("host-0123456789abcdef.sock", path) != 0);
    CHECK(contains(path, "/control/host-0123456789abcdef.sock"));
    CHECK(Ubiquitty::Host::processIDFilePathForSocketName("host-0123456789abcdef.sock", path) != 0);
    CHECK(contains(path, "/control/host-0123456789abcdef.pid"));
    // Each daemon logs to a file of its own, beside its socket.
    CHECK(Ubiquitty::Host::logfilePathForSocketName("host-0123456789abcdef.sock", path) != 0);
    CHECK(contains(path, "/control/host-0123456789abcdef.log"));

    CHECK(Ubiquitty::Host::socketPathForName("../victim.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::socketPathForName("/tmp/victim.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::socketPathForName("host/evil.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::socketPathForName("host-ABCDEF0123456789.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::processIDFilePathForSocketName("../victim.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::logfilePathForSocketName("../victim.sock", path) == 0);
    CHECK(path[0] == '\0');
    CHECK(Ubiquitty::Host::logfilePathForSocketName("host.sock", path) == 0);
    CHECK(path[0] == '\0');

    CHECK(::chmod(control.c_str(), 0777) == 0);
    CHECK(Ubiquitty::Session::ensureControlDirectory());
    CHECK(modeOf(control) == 0700);

    std::filesystem::remove_all(control);
    const std::string target = root + "/target";
    CHECK(::mkdir(target.c_str(), 0700) == 0);
    CHECK(::symlink(target.c_str(), control.c_str()) == 0);
    CHECK(!Ubiquitty::Session::ensureControlDirectory());

    // prepareSocketPathForBind must refuse to touch anything except a
    // proven-dead, owner-matching AF_UNIX socket, and must leave any refused
    // path completely untouched.
    {
        const std::string socketDir = root + "/sockets";
        CHECK(::mkdir(socketDir.c_str(), 0700) == 0);
        // A refusal carries its reason, so every one below is asserted to name
        // something. A `std::string error` local that the call can leave
        // untouched would make `!error.empty()` a check on the previous
        // iteration's text.
        const auto prepare = [](const std::string& path) {
            return Ubiquitty::Session::prepareSocketPathForBind(path);
        };

        // No entry at the path — nothing to clear, safe to proceed.
        const std::string missing = socketDir + "/missing.sock";
        CHECK(prepare(missing).has_value());

        // A regular file (the typo case): refused, untouched.
        const std::string regularFile = socketDir + "/document.txt";
        {
            std::FILE* f = std::fopen(regularFile.c_str(), "w");
            CHECK(f != nullptr);
            if (f) {
                std::fputs("do not delete me", f);
                std::fclose(f);
            }
        }
        const auto fileRefusal = prepare(regularFile);
        CHECK(!fileRefusal.has_value());
        CHECK(!fileRefusal.error().empty());
        CHECK(pathExists(regularFile)); // still there — a regular file is never unlinked

        // A directory: refused, untouched.
        const std::string directory = socketDir + "/adir";
        CHECK(::mkdir(directory.c_str(), 0700) == 0);
        const auto directoryRefusal = prepare(directory);
        CHECK(!directoryRefusal.has_value());
        CHECK(!directoryRefusal.error().empty());
        CHECK(pathExists(directory));

        // A symlink (even to a nonexistent target): refused, untouched. lstat
        // never follows it, so it never reads as S_ISSOCK.
        const std::string symlinkPath = socketDir + "/link.sock";
        CHECK(::symlink("/nonexistent/wherever", symlinkPath.c_str()) == 0);
        const auto symlinkRefusal = prepare(symlinkPath);
        CHECK(!symlinkRefusal.has_value());
        CHECK(!symlinkRefusal.error().empty());
        struct stat linkStat;
        CHECK(::lstat(symlinkPath.c_str(), &linkStat) == 0 && S_ISLNK(linkStat.st_mode));

        // A LIVE socket (something is listening): refused, untouched, and
        // still fully functional (bind()able by nothing else while we hold it).
        const std::string liveSocket = socketDir + "/live.sock";
        int liveFileDescriptor = bindRealSocket(liveSocket, SocketListenMode::Listen);
        CHECK(liveFileDescriptor >= 0);
        const auto liveRefusal = prepare(liveSocket);
        CHECK(!liveRefusal.has_value());
        CHECK(!liveRefusal.error().empty());
        CHECK(pathExists(liveSocket));
        if (liveFileDescriptor >= 0)
            ::close(liveFileDescriptor);
        ::unlink(liveSocket.c_str());

        // A DEAD socket (bound, never listened on, then closed — nothing
        // answers connect()): proven stale, safely cleared.
        const std::string deadSocket = socketDir + "/dead.sock";
        int deadFileDescriptor = bindRealSocket(deadSocket, SocketListenMode::BindOnly);
        CHECK(deadFileDescriptor >= 0);
        if (deadFileDescriptor >= 0)
            ::close(deadFileDescriptor); // socket file remains; nothing listens
        CHECK(pathExists(deadSocket));
        CHECK(prepare(deadSocket).has_value());
        CHECK(!pathExists(deadSocket)); // cleared — a fresh bind() can claim it now
    }

    // The newest-first socket walk: real bound sockets ordered by bind mtime,
    // decoys that match the name pattern but not S_ISSOCK, and the capture
    // that hands the newest to a fixed buffer.
    {
        // The symlink case above destroyed the control directory; the walk
        // needs a real one again.
        std::filesystem::remove(control);
        std::filesystem::remove_all(root + "/target");
        CHECK(Ubiquitty::Session::ensureControlDirectory());

        char directory[1024];
        CHECK(Ubiquitty::Host::controlDirectory(directory) != 0);
        CHECK(contains(directory, "/control"));

        // An empty directory yields no path and leaves the buffer empty.
        char newest[1024];
        newest[0] = 'x';
        CHECK(Ubiquitty::Host::newestSocketPath(newest) == 0);
        CHECK(newest[0] == '\0');

        // Three real sockets with distinct bind times, oldest to newest, plus
        // decoys: a regular file wearing a socket name (fails S_ISSOCK), a log
        // file, and real sockets at names no daemon binds (fail the name
        // filter).
        const std::string oldest = control + "/host-00000000000000aa.sock";
        const std::string middle = control + "/host-00000000000000bb.sock";
        const std::string latest = control + "/host-00000000000000cc.sock";
        const std::string unversioned = control + "/host.sock";
        const std::string shortHash = control + "/host-cc.sock";
        for (const std::string* socketPath : { &unversioned, &shortHash, &oldest, &middle, &latest }) {
            const int boundFileDescriptor = bindRealSocket(*socketPath, SocketListenMode::BindOnly);
            CHECK(boundFileDescriptor >= 0);
            if (boundFileDescriptor >= 0)
                ::close(boundFileDescriptor);
        }
        struct timeval past[2] = { { 1000000, 0 }, { 1000000, 0 } };
        CHECK(::utimes(oldest.c_str(), past) == 0);
        struct timeval middleTime[2] = { { 2000000, 0 }, { 2000000, 0 } };
        CHECK(::utimes(middle.c_str(), middleTime) == 0);
        {
            std::FILE* decoy = std::fopen((control + "/host-00000000000000dd.sock").c_str(), "w");
            CHECK(decoy != nullptr);
            if (decoy)
                std::fclose(decoy);
            std::FILE* log = std::fopen((control + "/host.log").c_str(), "w");
            CHECK(log != nullptr);
            if (log)
                std::fclose(log);
        }

        const std::string insideLink = control + "/host-00000000000000ee.sock";
        const std::string outsideSocket = root + "/host-00000000000000ff.sock";
        const std::string outsideLink = control + "/host-00000000000000ff.sock";
        const int outsideDescriptor = bindRealSocket(outsideSocket, SocketListenMode::BindOnly);
        CHECK(outsideDescriptor >= 0);
        if (outsideDescriptor >= 0)
            ::close(outsideDescriptor);
        CHECK(::symlink(latest.c_str(), insideLink.c_str()) == 0);
        CHECK(::symlink(outsideSocket.c_str(), outsideLink.c_str()) == 0);
        CHECK(Ubiquitty::Host::isOwnHostSocketPath(latest.c_str()));
        CHECK(!Ubiquitty::Host::isOwnHostSocketPath(insideLink.c_str()));
        CHECK(!Ubiquitty::Host::isOwnHostSocketPath(outsideLink.c_str()));
        CHECK(!Ubiquitty::Host::isOwnHostSocketPath(outsideSocket.c_str()));
        CHECK(!Ubiquitty::Host::isOwnHostSocketPath((control + "/host-00000000000000dd.sock").c_str()));
        CHECK(::chmod(control.c_str(), 0777) == 0);
        CHECK(!Ubiquitty::Host::isOwnHostSocketPath(latest.c_str()));
        CHECK(::chmod(control.c_str(), 0700) == 0);

        CHECK(Ubiquitty::Host::newestSocketPath(newest) != 0);
        CHECK(contains(newest, "host-00000000000000cc.sock"));

        // The visitor sees every real socket, newest first, and never a decoy.
        struct WalkLog {
            std::string joined;
            int visits = 0;
        } walkLog;
        const auto record = [](const char* path, void* ctx) {
            WalkLog* log = static_cast<WalkLog*>(ctx);
            log->joined += path;
            log->joined += '\n';
            log->visits++;
            return false; // keep walking
        };
        CHECK(!Ubiquitty::Host::eachHostSocketNewestFirst(record, &walkLog));
        CHECK(walkLog.visits == 3);
        CHECK(walkLog.joined.find("cc.sock") < walkLog.joined.find("bb.sock"));
        CHECK(walkLog.joined.find("bb.sock") < walkLog.joined.find("aa.sock"));
        CHECK(!contains(walkLog.joined, "dd.sock"));
        CHECK(!contains(walkLog.joined, "ee.sock"));
        CHECK(!contains(walkLog.joined, "ff.sock"));
        CHECK(!contains(walkLog.joined, "host.log"));
        CHECK(!contains(walkLog.joined, "/host.sock"));
        CHECK(!contains(walkLog.joined, "/host-cc.sock"));

        // A visitor that accepts stops the walk and reports it.
        const auto acceptFirst = [](const char*, void*) { return true; };
        CHECK(Ubiquitty::Host::eachHostSocketNewestFirst(acceptFirst, nullptr));
        CHECK(!Ubiquitty::Host::eachHostSocketNewestFirst(nullptr, nullptr));

        // A buffer too small for the newest path captures nothing rather than
        // truncating a path a caller would then connect to.
        char tiny[8];
        CHECK(Ubiquitty::Host::newestSocketPath(tiny) == 0);
        CHECK(tiny[0] == '\0');

        // Every socket the directory holds is ordered before any is visited:
        // eighty more, each bound at a time of its own after the three above,
        // all come back newest first, wherever the directory lists them.
        {
            constexpr unsigned extra = 80;
            const auto later = static_cast<long>(::time(nullptr)) + 1000;
            for (unsigned index = 0; index < extra; ++index) {
                char leaf[40];
                std::snprintf(leaf, sizeof(leaf), "/host-%016x.sock", 0x1000u + index);
                const std::string socketPath = control + leaf;
                const int boundFileDescriptor = bindRealSocket(socketPath, SocketListenMode::BindOnly);
                CHECK(boundFileDescriptor >= 0);
                if (boundFileDescriptor >= 0)
                    ::close(boundFileDescriptor);
                struct timeval at[2] = { { later + index, 0 }, { later + index, 0 } };
                CHECK(::utimes(socketPath.c_str(), at) == 0);
            }
            std::vector<std::string> order;
            const auto collect = [](const char* path, void* ctx) {
                static_cast<std::vector<std::string>*>(ctx)->emplace_back(path);
                return false;
            };
            CHECK(!Ubiquitty::Host::eachHostSocketNewestFirst(collect, &order));
            CHECK_EQ(order.size(), static_cast<std::size_t>(extra + 3));
            for (unsigned index = 0; index < extra && index < order.size(); ++index) {
                char leaf[40];
                std::snprintf(leaf, sizeof(leaf), "/host-%016x.sock", 0x1000u + extra - 1 - index);
                CHECK(contains(order[index], leaf));
            }
            CHECK(order.size() == extra + 3 && contains(order[extra], "cc.sock") && contains(order[extra + 2], "aa.sock"));
        }

        // A listener whose backlog is full is still served, though a connect
        // to it now fails; one that has closed is not, and neither is a path
        // with no socket.
        {
            const std::string busy = control + "/host-00000000000000f1.sock";
            const int listener = bindRealSocket(busy, SocketListenMode::Listen);
            CHECK(listener >= 0);
            struct sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::strncpy(address.sun_path, busy.c_str(), sizeof(address.sun_path) - 1);
            std::vector<int> queued;
            bool isFull = false;
            for (int attempt = 0; attempt < 256 && !isFull; ++attempt) {
                const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (client < 0)
                    break;
                ::fcntl(client, F_SETFL, O_NONBLOCK);
                if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                    queued.push_back(client);
                    continue;
                }
                isFull = errno == ECONNREFUSED || errno == EAGAIN || errno == EWOULDBLOCK;
                ::close(client);
            }
            CHECK(isFull); // premise: the backlog is full and a connect fails
            CHECK(Ubiquitty::Host::isSocketPathServed(busy.c_str()));
            for (const int client : queued)
                ::close(client);
            if (listener >= 0)
                ::close(listener);
            CHECK(!Ubiquitty::Host::isSocketPathServed(busy.c_str()));
            ::unlink(busy.c_str());
            CHECK(!Ubiquitty::Host::isSocketPathServed(busy.c_str()));
        }
    }

    std::filesystem::remove_all(root);
    ::unsetenv("UBIQUITTY_HOME");

    if (failures == 0) {
        printf("controlpaths tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
