// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/daemon_startup.cpp — the socket a daemon claims, and the sessions
// it agrees to make.
//
// One socket path names the daemon for every client on the machine, and the
// daemon that binds it owns them all. Two things have to be true at once, and
// they pull against each other: a socket file left behind by a daemon that
// crashed must not block every future start, and a socket a LIVE daemon is
// serving must not be taken away from it. The daemon distinguishes the two by
// probing — connect to the path, and only unlink it if nothing answers.
//
// Getting that backwards in the permissive direction is the interesting one: a
// daemon that unlinked on EADDRINUSE without probing would take the socket from
// a running daemon, and every client of the old one would keep talking to an
// fd whose name now belongs to someone else. Nothing errors. The sessions just
// stop being reachable under the name they were reached by.
//
// The oracle for that is a second daemon: bind a path, then have another
// instance try the same path, then assert two things — that it was refused,
// and that the first daemon's socket still answers. A test that checked only
// the refusal would pass against a daemon that refused after unlinking.
//
// The second half is the session registry's own refusals, which are the
// resource story: every session pins a PTY, a Terminal and up to a 32 MiB input
// queue, so an unbounded caller spawning them is a resource-exhaustion vector
// against the machine, and an adopted file descriptor arrives across a trust
// boundary from a peer.
//
// What this leaves unexecuted, and why:
//
//   partialFrameDeadline  its env-var fallbacks. The value is a function-local
//                         static parsed once per process, so a single binary
//                         can exercise exactly one of the garbage / empty /
//                         zero / valid arms — the shipping stanza
//                         partial_frame_budget covers the valid one, and the
//                         fallbacks need a process each.
//   Daemon::bindSocket    socket(AF_UNIX, SOCK_STREAM, 0) failing, and listen()
//                         failing on a socket that just bound.
//   Daemon::createSession the structural allocations refusing — that is
//                         create_session_refusal's stanza, which replaces the
//                         nothrow operator new to refuse them; and the
//                         adopted-session success path, which needs a real pty
//                         master whose session leader descends from the claimed
//                         owner (adoptPTY's ownership check) — that is
//                         adopt_session's stanza, not this one.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"

#include "Core/GridBounds.h"
#include "Core/Terminal.h"
#include "PAL/Stream.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct DaemonStartupAccess {
    // bindSocket names the failure it hit. Most cases below only care that
    // the bind failed at all, so this keeps the boolean shape they read;
    // a case that must tell one refusal from another uses bindFailure.
    static bool bind(Daemon& d, const std::string& path) { return d.bindSocket(path.c_str()) == BindFailure::None; }
    static BindFailure bindFailure(Daemon& d, const std::string& path) { return d.bindSocket(path.c_str()); }
    static bool listening(const Daemon& d) { return static_cast<bool>(d.m_listenFileDescriptor); }
    static const std::string& socketPath(const Daemon& d) { return d.m_socketPath; }
    static const std::string& processIDFilePath(const Daemon& d) { return d.m_processIDFilePath; }
    static const std::string& shellPath(const Daemon& d) { return d.m_shellPath; }
    static int columns(const Daemon& d) { return d.m_columns; }
    static int rows(const Daemon& d) { return d.m_rows; }

    static void writeProcessIDFile(Daemon& d, const std::string& path) { d.writeProcessIDFile(path.c_str()); }

    static void useGeometry(Daemon& d, int columns, int rows)
    {
        d.m_columns = columns;
        d.m_rows = rows;
    }
    static void useShell(Daemon& d, const std::string& path) { d.m_shellPath = path; }

    // Session is a private nested type, so the geometry it ended up with comes
    // back rather than the pointer.
    static bool createSession(Daemon& d, int columns, int rows, int adoptFileDescriptor = -1,
        int* madeColumns = nullptr, int* madeRows = nullptr)
    {
        Daemon::Session* s = d.createSession(columns, rows, {}, adoptFileDescriptor, 0);
        if (s && s->term) {
            if (madeColumns)
                *madeColumns = s->term->columns;
            if (madeRows)
                *madeRows = s->term->rows;
        }
        return s != nullptr;
    }
    static bool createRemoteSession(Daemon& d, std::span<const std::string> command)
    {
        return d.createSession(80, 24, command, -1, 0) != nullptr;
    }

    // Sessions with no PTY and no Terminal, purely to occupy the registry up to
    // its cap — building 64 real ones would fork 64 shells to prove a count.
    static void fillRegistry(Daemon& d, std::size_t count)
    {
        while (d.m_sessions.size() < count) {
            auto session = std::make_unique<Daemon::Session>();
            session->id = static_cast<std::uint32_t>(d.m_sessions.size() + 1);
            d.m_sessions.push_back(std::move(session));
        }
    }
    static std::size_t sessionCount(const Daemon& d) { return d.m_sessions.size(); }
    static void useSocketPath(Daemon& d, const std::string& path) { d.m_socketPath = path; }
    /// The id of the session most recently created, and the pane it was placed in.
    static std::uint32_t lastSessionID(const Daemon& d) { return d.m_sessions.empty() ? 0 : d.m_sessions.back()->id; }
    static std::uint32_t lastSessionPane(const Daemon& d) { return d.m_sessions.empty() ? 0 : std::to_underlying(d.m_sessions.back()->placement.pane); }
    static std::uint32_t lastSessionWindow(const Daemon& d) { return d.m_sessions.empty() ? 0 : std::to_underlying(d.m_sessions.back()->placement.surface); }
    static std::uint32_t lastSessionTab(const Daemon& d) { return d.m_sessions.empty() ? 0 : std::to_underlying(d.m_sessions.back()->placement.tab); }
    static void useNextSessionID(Daemon& d, std::uint32_t id) { d.m_nextSessionID = id; }
    static std::uint32_t nextSessionID(const Daemon& d) { return d.m_nextSessionID; }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::BindFailure;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::DaemonStartupAccess;

namespace {

using A = DaemonStartupAccess;

std::string scratchDirectory;

// Can anything be reached at this socket path right now?
bool answersAt(const std::string& path)
{
    const int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return false;
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    const bool connected = ::connect(fileDescriptor, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fileDescriptor);
    return connected;
}

// A socket file with nothing behind it — what a daemon that died without
// cleaning up leaves on disk.
bool leaveStaleSocket(const std::string& path)
{
    const int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return false;
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    const bool bound = ::bind(fileDescriptor, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0
        && ::listen(fileDescriptor, 1) == 0;
    ::close(fileDescriptor); // the file stays; the listener does not
    return bound;
}

bool exists(const std::string& path)
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

} // namespace

int main()
{
    char directoryTemplate[] = "/tmp/ubiquitty-startup-XXXXXX";
    const char* made = ::mkdtemp(directoryTemplate);
    if (!made) {
        std::fprintf(stderr, "daemon_startup: could not create a scratch directory\n");
        return 1;
    }
    scratchDirectory = made;

    // ---- 1. A path too long for the address is refused before anything is
    // bound. strncpy would otherwise truncate it and bind a DIFFERENT, shorter
    // path — the daemon would be listening somewhere its clients never look.
    {
        Daemon d;
        const std::string tooLong(300, 'x');
        // which failure it was, not merely that there was one. A truncating bind
        // fails here too and fails identically, so only naming the reason
        // separates a refusal by name from a lucky error on a stray path.
        CHECK(A::bindFailure(d, tooLong) == BindFailure::PathTooLong);
        CHECK(!A::listening(d));
    }

    // ---- 2. A bind failure that is not an address collision is a hard
    // failure, not an invitation to the stale-socket probe: unlinking on a
    // permission or path error would delete something the daemon has no reason
    // to touch.
    {
        Daemon d;
        CHECK(!A::bind(d, "/nonexistent-directory-for-ubiquitty-startup/host.sock"));
        CHECK(!A::listening(d));
    }

    // ---- 3. A live daemon keeps its socket. The refusal alone is not the
    // property — a daemon that unlinked first and then failed to bind would
    // also refuse here, having already taken the path away from the daemon
    // still serving it. So the running daemon must still answer afterwards.
    {
        const std::string path = scratchDirectory + "/live.sock";
        Daemon first;
        CHECK(A::bind(first, path));
        CHECK(answersAt(path));

        Daemon second;
        CHECK(!A::bind(second, path)); // refused
        CHECK(!A::listening(second));
        CHECK(answersAt(path)); // and the first daemon is still reachable
        CHECK(A::socketPath(first) == path);
    }

    // ---- 4. A socket file left by a daemon that crashed does not block the
    // next start. Nothing answers at the path, so it is removed and rebound —
    // otherwise a crash would make the machine's terminal daemon unstartable
    // until someone deleted a file by hand.
    {
        const std::string path = scratchDirectory + "/stale.sock";
        CHECK(leaveStaleSocket(path));
        CHECK(exists(path));
        CHECK(!answersAt(path)); // the file is there; the daemon is not

        Daemon d;
        CHECK(A::bind(d, path));
        CHECK(A::listening(d));
        CHECK(answersAt(path)); // and now it is served again
    }

    // ---- 4b. The removal can itself fail — a directory the daemon may read but
    // not write. The rebind then fails too, and that has to be reported rather
    // than treated as success on the strength of having tried: a daemon that
    // returned true here would run with no listening socket at all.
    //
    // Skipped under root, which is not subject to the directory permission the
    // case depends on.
    if (::geteuid() != 0) {
        const std::string readOnlyDirectory = scratchDirectory + "/readonly";
        CHECK(::mkdir(readOnlyDirectory.c_str(), 0700) == 0);
        const std::string path = readOnlyDirectory + "/stuck.sock";
        CHECK(leaveStaleSocket(path));
        CHECK(::chmod(readOnlyDirectory.c_str(), 0500) == 0);

        Daemon d;
        CHECK(!A::bind(d, path));
        CHECK(!A::listening(d));

        CHECK(::chmod(readOnlyDirectory.c_str(), 0700) == 0);
        ::unlink(path.c_str());
        ::rmdir(readOnlyDirectory.c_str());
    }

    // ---- 5. start() stops at a bind failure. The steps after it commit real
    // state — a PID file that daemon-management tooling will find and signal, a
    // shell path, a geometry — and a daemon holding no socket must not be
    // advertised as running.
    {
        Daemon d;
        const std::string pidPath = scratchDirectory + "/never-written.pid";
        const std::string tooLong(300, 'x');

        CHECK(!d.start(tooLong.c_str(), pidPath.c_str(), "/bin/sh", 80, 24));
        CHECK(A::processIDFilePath(d).empty()); // writeProcessIDFile was never reached
        CHECK(!exists(pidPath));
        CHECK(A::shellPath(d).empty());
    }

    // ---- 6. A degenerate geometry from the command line becomes 80x24 rather
    // than being committed as the default for every session this daemon will
    // ever create.
    {
        Daemon d;
        const std::string path = scratchDirectory + "/geometry.sock";
        const std::string pidPath = scratchDirectory + "/geometry.pid";

        CHECK(d.start(path.c_str(), pidPath.c_str(), "/bin/sh", 0, -5));
        CHECK(A::columns(d) == 80);
        CHECK(A::rows(d) == 24);
        ::unlink(pidPath.c_str());
        ::unlink(path.c_str());
    }

    // ---- 7. A PID file that cannot be written is not fatal. The daemon serves
    // clients whether or not management tooling can find it, so a read-only or
    // missing directory must not take the terminal down with it.
    {
        Daemon d;
        A::writeProcessIDFile(d, "/nonexistent-directory-for-ubiquitty-startup/host.pid");
        CHECK(A::processIDFilePath(d) == "/nonexistent-directory-for-ubiquitty-startup/host.pid");
    }

    // ---- 8. A session asked for with no geometry gets the daemon's, and one
    // asked for beyond Core's limits is clamped. These arrive on the wire in a
    // HELLO and are not clamped before they get here, so a hostile or merely
    // wrong client sets the size of a real PTY.
    {
        Daemon d;
        A::useGeometry(d, 100, 30);
        A::useShell(d, "/bin/sh");

        int madeColumns = 0, madeRows = 0;
        CHECK(A::createSession(d, 0, 0, -1, &madeColumns, &madeRows));
        CHECK(madeColumns == 100);
        CHECK(madeRows == 30);

        CHECK(A::createSession(d, 5000, 4000, -1, &madeColumns, &madeRows));
        CHECK(madeColumns == Ubiquitty::maxColumns);
        CHECK(madeRows == Ubiquitty::maxRows);
    }

    // ---- 9. The registry is capped. Each session pins a PTY, a Terminal and
    // an input queue, so a caller that could spawn without bound would exhaust
    // the machine through a daemon that is happy to help.
    {
        Daemon d;
        A::useShell(d, "/bin/sh");
        A::fillRegistry(d, 64);
        CHECK(A::sessionCount(d) == 64);

        CHECK(!A::createSession(d, 80, 24));
        CHECK(A::sessionCount(d) == 64); // and nothing was half-added
    }

    // ---- 10. The id counter never hands out 0, and never wraps. Zero is the
    // wire's "make me a new session" sentinel, so a session carrying it could
    // not be addressed by the client it was made for; and a wrapped counter
    // would hand a new session an ID a client may still hold for an old one.
    {
        Daemon d;
        A::useShell(d, "/bin/sh");
        A::useNextSessionID(d, 0xFFFFFFFFu);

        // This call fails at the stream (see case 11) — the counter has already
        // advanced by then, which is the point: exhaustion must be handled
        // where the counter moves, not where a session succeeds.
        int pipes[2] = { -1, -1 };
        CHECK(::pipe(pipes) == 0);
        CHECK(!A::createSession(d, 80, 24, pipes[0]));
        CHECK(A::nextSessionID(d) == 0); // exhausted, not reset to 1
        ::close(pipes[1]);

        // A create that would otherwise succeed is refused rather than reusing 1.
        CHECK(!A::createSession(d, 80, 24));
        CHECK(A::sessionCount(d) == 0);
    }

    // ---- 11. An adopted file descriptor that is not a PTY master is refused.
    // It arrives over a socket from a peer, so it is whatever that peer chose
    // to send; driving a pipe or a regular file as terminal I/O is the peer
    // deciding what the daemon reads and writes.
    {
        Daemon d;
        int pipes[2] = { -1, -1 };
        CHECK(::pipe(pipes) == 0);

        CHECK(!A::createSession(d, 80, 24, pipes[0]));
        CHECK(A::sessionCount(d) == 0); // no half-built session left behind
        ::close(pipes[1]);
    }

    // ---- 12. A remote launch with no program to run is refused rather than
    // handed to exec. The command comes from a remote peer, and a session
    // created around a program that never started would look alive and answer
    // nothing.
    {
        Daemon d;
        const std::vector<std::string> nothingToRun { std::string() };

        CHECK(!A::createRemoteSession(d, nothingToRun));
        CHECK(A::sessionCount(d) == 0);
    }

    // ---- 13. The program a session starts is told where it runs: the daemon's
    // socket, its session, and the window, tab and pane it was placed in, each
    // written as a typed ID a tool inside it can name `here` with. A context
    // the daemon itself inherited never stands in for the session's own.
    {
        const std::string report = scratchDirectory + "/context.report";
        const std::string script = scratchDirectory + "/report-context.sh";
        if (std::FILE* file = std::fopen(script.c_str(), "w")) {
            std::fprintf(file, "#!/bin/sh\nfor name in UBIQUITTY_SOCKET UBIQUITTY_SESSION UBIQUITTY_WINDOW UBIQUITTY_TAB UBIQUITTY_PANE; do "
                               "printf '%%s=%%s\\n' \"$name\" \"$(printenv $name)\"; done > '%s.partial' && mv '%s.partial' '%s'\n",
                report.c_str(), report.c_str(), report.c_str());
            std::fclose(file);
        }
        CHECK(::chmod(script.c_str(), 0700) == 0);
        ::setenv("UBIQUITTY_PANE", "pane:4242", 1);

        Daemon d;
        A::useShell(d, script);
        A::useSocketPath(d, scratchDirectory + "/context.sock");
        CHECK(A::createSession(d, 80, 24));

        std::string contents;
        for (int attempt = 0; attempt < 500 && contents.empty(); ++attempt) {
            if (std::FILE* file = std::fopen(report.c_str(), "r")) {
                char buffer[1024];
                const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
                contents.assign(buffer, count);
                std::fclose(file);
            } else {
                ::usleep(10000);
            }
        }
        const auto expect = [&](const std::string& line) {
            CHECK_MSG(contents.find(line + "\n") != std::string::npos, line.c_str());
        };
        expect("UBIQUITTY_SOCKET=" + scratchDirectory + "/context.sock");
        expect("UBIQUITTY_SESSION=session:" + std::to_string(A::lastSessionID(d)));
        expect("UBIQUITTY_WINDOW=window:" + std::to_string(A::lastSessionWindow(d)));
        expect("UBIQUITTY_TAB=tab:" + std::to_string(A::lastSessionTab(d)));
        expect("UBIQUITTY_PANE=pane:" + std::to_string(A::lastSessionPane(d)));
        CHECK(A::lastSessionPane(d) != 0 && A::lastSessionPane(d) != 4242);

        ::unsetenv("UBIQUITTY_PANE");
        ::unlink(report.c_str());
        ::unlink(script.c_str());
        // Cleanup, after the assertions: a directory left under /tmp changes
        // no verdict, so the removal's status is not this case's to check.
        [[maybe_unused]] const int removalStatus = std::system(("rm -rf '" + scratchDirectory + "/clipboard-tools'").c_str());
    }

    ::unlink((scratchDirectory + "/live.sock").c_str());
    ::unlink((scratchDirectory + "/stale.sock").c_str());
    ::rmdir(scratchDirectory.c_str());

    if (failures == 0)
        std::printf("daemon_startup: a live daemon's socket survives another daemon's attempt on it while a "
                    "crashed daemon's leftover does not block the next start; start() commits nothing after a "
                    "bind failure; and the session registry refuses a degenerate geometry, a full registry, "
                    "the sentinel id, a handed-in descriptor that is not a pty, and a program that is not one; and a session's "
                    "child is told the socket, session, window, tab and pane it runs in\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
