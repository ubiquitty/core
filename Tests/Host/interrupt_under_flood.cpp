// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/interrupt_under_flood.cpp — a ^C a client sends while its pane
// floods reaches the program, whatever was typed ahead of it and however far
// behind the client is.
//
// A live daemon runs a program that prints without end and reads nothing. The
// client types lines ahead, which the program never reads, stops reading for a
// while, as one drawing a flood does, then sends ^C and reads again, slowly.
// The program must have ended within a bound of the ^C.
//
// The terminal takes about a kilobyte of unread lines and then no character at
// all, its interrupt included, so the rest of what was typed waits in the
// daemon. The daemon stopped reading a client at the first byte queued for its
// program (Daemon::isInputHeld), so the ^C sat unread in the socket behind
// lines that only the ^C could have ended the wait for, and no client could
// end the flood. Read, and written in turn, it would have waited in the queue
// the same way; the stream lets it past (PTYStream::passSignalCharacter).
//
// The daemon logs what holds each client's input on SIGUSR1; the log is
// printed, so a failure says which hold it was.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Host/HostConnection.h"
#include "Host/ScreenClient.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/TokenStore.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

double secondsNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The process ID the flooding program wrote, or zero until it has.
pid_t readProcessID(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return 0;
    int processID = 0;
    const int read = std::fscanf(file, "%d", &processID);
    std::fclose(file);
    return read == 1 ? processID : 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: interrupt_under_flood <ubiquitty-host>\n");
        return 2;
    }
    const char* hostBinary = argv[1];
    ::signal(SIGPIPE, SIG_IGN);
    char homeTemplate[] = "/tmp/ubiquitty-interrupt-flood.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    CHECK_MSG(home != nullptr, "a scratch home");
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary);
    const std::string mark = std::string(home) + "/flooding.pid";
    ::mkdir(macHome.c_str(), 0700);
    const std::string token = "interrupttokeninterrupttokeninterrupt";
    {
        std::FILE* file = Ubiquitty::Host::openOwnerOnly(macHome + "/tokens", "w");
        CHECK_MSG(file != nullptr, "the fixture credential store was created");
        if (!file)
            return 1;
        constexpr std::uint8_t scopeObserve = 0x01, scopeDrive = 0x02, scopeNewSession = 0x10;
        std::fprintf(file, "%s %u 0 interrupt\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scopeObserve | scopeDrive | scopeNewSession));
        std::fclose(file);
    }
    const pid_t daemon = ::fork();
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        // A failed reopen leaves the daemon without its log and without
        // descriptor 2, which the next file it opened would take. The child
        // stops instead, and the wait below reports the daemon never came up.
        if (!std::freopen((macHome + "/daemon.log").c_str(), "a", stderr))
            _exit(126);
        char* args[] = { const_cast<char*>(hostBinary), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("160"),
            const_cast<char*>("--rows"), const_cast<char*>("48"), nullptr };
        ::execv(hostBinary, args);
        _exit(127);
    }
    // The socket file exists from bind(), before listen(); only an accepted
    // connection says the daemon is up.
    const bool isDaemonUp = Ubiquitty::HostTest::waitForDaemonReady(sockPath);
    double latency = -1;
    do {
        CHECK_MSG(isDaemonUp, "the daemon came up");
        if (!isDaemonUp)
            break;
        Ubiquitty::HostTest::Connection c;
        c.fileDescriptor = Ubiquitty::HostTest::connectDaemon(sockPath);
        CHECK_MSG(c.fileDescriptor >= 0, "the client connected");
        if (c.fileDescriptor < 0)
            break;
        // This raw stored credential is an agent's authority, not the trusted
        // interactive attach bridge. Fresh creation is requested exactly once.
        UbiquittyTest::ScreenClient client(48, 160, 10'000, Sync::LinkCost::Unmetered, Sync::Role::Agent);
        const bool didStart = client.start({ .target = Sync::AdmissionTarget::Session }, std::vector<std::uint8_t>(token.begin(), token.end()));
        CHECK_MSG(didStart, "the canonical fresh-session greeting was encoded");
        if (!didStart)
            break;
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return client.connection().counters().checkpointsApplied > 0; });
        const auto welcome = client.welcome();
        const auto panes = client.connection().boundPanes();
        const bool isGranted = welcome && client.connection().state() == Sync::ConnectionState::Attached
            && welcome->admission == Sync::AttachmentAdmission::Granted && welcome->grantedRole == Sync::Role::Agent
            && welcome->selection.target == Sync::AdmissionTarget::Session && welcome->selection.sessionID != 0
            && welcome->selection.workspaceID != 0 && welcome->selection.daemonInstanceID != 0
            && welcome->selection.socketName == Ubiquitty::Host::socketLeafForBinary(hostBinary)
            && welcome->hostInstanceID != Sync::ID128 {} && welcome->attachmentID != Sync::ID128 {} && welcome->resumeSecret != Sync::ID128 {}
            && client.count(Sync::FrameKind::Welcome) == 1 && panes.size() == 1 && panes.front().first != 0 && panes.front().second != Sync::ID128 {}
            && client.pane() && client.connection().counters().checkpointsApplied > 0;
        CHECK_MSG(isGranted, "the actual fresh session granted the agent its selected daemon and checkpoint");
        if (!isGranted)
            break;
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 400);

        // A flood of coloured lines that fills the screen with every write,
        // from a program that leaves on the signal alone.
        const std::string flood = "perl -e 'open(my $f, \">\", \"" + mark + ".tmp\"); print $f \"$$\\n\"; close $f; rename(\"" + mark + ".tmp\", \"" + mark
            + "\"); $| = 1; my $n = 0; while (1) { print \"\\e[3\", $n % 8, \"m\", $n++, \" \", \"x\" x 150, \"\\n\" }'\r";
        CHECK_MSG(UbiquittyTest::typeOverSocket(c.fileDescriptor, client, flood), "the flood's command was typed");
        pid_t flooding = 0;
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return (flooding = readProcessID(mark)) != 0; });
        CHECK_MSG(flooding != 0, "the flood started");
        if (!flooding)
            break;
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 500);

        // Lines typed ahead that the flooding program never reads: the
        // terminal's input queue fills and takes no more.
        std::string typedAhead;
        while (typedAhead.size() < 8 * 1024)
            typedAhead += "typed ahead of the interrupt\r";
        CHECK_MSG(UbiquittyTest::typeOverSocket(c.fileDescriptor, client, typedAhead), "the lines ahead of the interrupt were typed");

        // The client falls behind: it reads nothing for three seconds.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ::kill(daemon, SIGUSR1);
        const double sent = secondsNow();
        CHECK_MSG(UbiquittyTest::typeOverSocket(c.fileDescriptor, client, "\x03"), "the interrupt was typed");

        // And catches up slowly: a few milliseconds of reading in every fifty.
        while (secondsNow() - sent < 10 && ::kill(flooding, 0) == 0) {
            (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds(45));
        }
        if (::kill(flooding, 0) != 0)
            latency = secondsNow() - sent;
        else
            ::kill(flooding, SIGKILL);
        ::kill(daemon, SIGUSR1);
        ::usleep(200 * 1000);
    } while (false);
    ::kill(daemon, SIGTERM);
    int status = 0;
    ::waitpid(daemon, &status, 0);

    if (std::FILE* log = std::fopen((macHome + "/daemon.log").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), log))
            std::fprintf(stderr, "  %s", line);
        std::fclose(log);
    }
    CHECK_MSG(latency >= 0 && latency < 2.0,
        (latency < 0 ? std::string("the flood was still running 10 s after the ^C") : "the ^C took " + std::to_string(latency) + " s to end the flood").c_str());
    if (failures == 0)
        std::printf("interrupt_under_flood: a ^C behind 8 KiB of unread lines, from a client 3 s behind a flood, ended it in %.0f ms\n", latency * 1000);
    return failures == 0 ? 0 : 1;
}
