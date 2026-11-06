// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/sync_output_e2e.cpp — DEC private mode 2026 suppresses published
// frames in a REAL daemon driving a REAL shell, not in a friend seam.
//
// Tests/Host/sync_output_withhold.cpp already pins mayPublishFrame's decision
// directly. It cannot see anything between the PTY and that decision: whether
// the escape reaches the session's Terminal at all, whether the mode survives
// the parse, whether the poll loop reaches the emission step while the block is
// open. Every one of those is a way the gate can be correct and the daemon still
// publish, and ubiquitty-frontends measured exactly that against a live host.
//
// The workload has to make composition SPAN poll cycles, which is the only case
// the mechanism is for. servicePTY drains the PTY until empty before the loop
// reaches emission, so a burst carrying the whole block in one pass has already
// closed it — correct, but indistinguishable from a dead gate. Slices with a
// pause between them empty the PTY repeatedly while the block stays open.
//
// Two arms, because one arm cannot fail informatively: the same workload with
// and without the block, in the same daemon and the same session. A gate that
// does nothing gives two equal counts, which is the reading that started this.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/TokenStore.h"
#include "ScreenClient.h"

#include <csignal>
#include <ctime>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

bool fail(const char* what)
{
    std::fprintf(stderr, "sync_output_e2e: FAIL — %s\n", what);
    return false;
}

std::uint64_t nowMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// Eighteen slices with a pause between them: the PTY empties and the emission
// step is reached repeatedly, and the whole run stays inside maxHoldTicks so the
// watchdog never ends the block for us.
const char* const slicedOutput = "for i in $(seq 1 18); do seq 1 40; sleep 0.03; done";

// Counts the updates the client applies in the next `windowMilliseconds`
// after `command` is typed. Carries the screen plane first so a frame
// from an earlier arm can never be counted in this one.
int countUpdates(Connection& c, UbiquittyTest::ScreenClient& screen, const std::string& command, int windowMilliseconds)
{
    (void)UbiquittyTest::carryScreen(c.fileDescriptor, screen, 300);
    std::string input = command;
    input += "\r";
    (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, input);
    const std::uint64_t before = screen.connection().counters().updatesApplied;
    (void)UbiquittyTest::carryScreen(c.fileDescriptor, screen, windowMilliseconds);
    return static_cast<int>(screen.connection().counters().updatesApplied - before);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }

    char homeTemplate[] = "/tmp/ubiquitty-sync-output.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // A raw client is not the uid-gated Local attach bridge, so tokenScopeAllows
    // holds it to a token's scope ceiling — creating a session needs NewSession.
    const std::string token = "synctokensynctokensynctokensynctoken";
    {
        const std::string tokenPath = macHome + "/tokens";
        std::FILE* file = Ubiquitty::Host::openOwnerOnly(tokenPath, "w");
        if (!file)
            return 1;
        constexpr std::uint8_t scopeObserve = 0x01, scopeDrive = 0x02, scopeNewSession = 0x10;
        std::fprintf(file, "%s %u 0 sync-output\n", Ubiquitty::Crypto::sha256Hex(token).c_str(),
            static_cast<unsigned>(scopeObserve | scopeDrive | scopeNewSession));
        std::fclose(file);
    }

    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"),
            const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    // The socket file exists from bind(), before listen(); only an accepted
    // connection says the daemon is up.
    const bool isDaemonUp = waitForDaemonReady(sockPath);

    bool ok = false;
    do {
        if (!isDaemonUp) {
            fail("daemon did not come up");
            break;
        }

        Connection c;
        c.fileDescriptor = connectDaemon(sockPath);
        if (c.fileDescriptor < 0) {
            fail("connect");
            break;
        }
        // The stored token authorizes automation. Keep that Agent role and
        // use this one production reader from greeting through both arms.
        UbiquittyTest::ScreenClient screen(24, 80, Ubiquitty::VT::fallbackScrollbackLines, Sync::LinkCost::Unmetered, Sync::Role::Agent);
        if (!screen.start({ .target = Sync::AdmissionTarget::Session }, std::vector<std::uint8_t>(token.begin(), token.end()))) {
            fail("the canonical fresh-session greeting was not encoded");
            break;
        }
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, screen, 5000, [&] { return screen.connection().counters().checkpointsApplied > 0; });
        const auto welcome = screen.welcome();
        const auto panes = screen.connection().boundPanes();
        if (!welcome || screen.connection().state() != Sync::ConnectionState::Attached
            || welcome->admission != Sync::AttachmentAdmission::Granted || welcome->grantedRole != Sync::Role::Agent
            || welcome->selection.target != Sync::AdmissionTarget::Session || welcome->selection.sessionID == 0
            || welcome->selection.workspaceID == 0 || welcome->selection.daemonInstanceID == 0
            || welcome->selection.socketName != Ubiquitty::Host::socketLeafForBinary(argv[1])
            || welcome->hostInstanceID == Sync::ID128 {} || welcome->attachmentID == Sync::ID128 {} || welcome->resumeSecret == Sync::ID128 {}
            || screen.count(Sync::FrameKind::Welcome) != 1 || panes.size() != 1 || panes.front().first == 0 || panes.front().second == Sync::ID128 {}
            || !screen.pane() || screen.connection().counters().checkpointsApplied == 0) {
            fail("no granted agent selection and checkpoint from the selected daemon");
            break;
        }
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, screen, 400); // let the shell reach a prompt

        const int unblocked = countUpdates(c, screen, slicedOutput, 1500);
        const int blocked = countUpdates(c, screen,
            std::string("printf \"\\033[?2026h\"; ") + slicedOutput + "; printf \"\\033[?2026l\"", 1500);

        std::fprintf(stderr, "sync_output_e2e: sliced output — no block %d updates, one block %d\n", unblocked, blocked);

        // The control has to produce frames, or the comparison below is two
        // zeroes agreeing and proves nothing about the block.
        if (unblocked < 4) {
            fail("the no-block arm published too few frames to compare against");
            break;
        }
        // Composition spanning the whole run collapses to the frame at its close
        // (plus whatever the prompt draws after). Half the control is a wide
        // margin that still separates "suppressed" from "unaffected".
        if (blocked * 2 >= unblocked) {
            fail("an open DEC 2026 block did not suppress published frames");
            break;
        }
        ok = true;
    } while (false);

    ::kill(daemon, SIGTERM);
    int status = 0;
    ::waitpid(daemon, &status, 0);

    if (ok)
        std::fprintf(stderr, "sync_output_e2e: a real daemon withholds published frames while a real shell composes under DEC 2026\n");
    return ok ? 0 : 1;
}
