// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/sync_prompt_mark_e2e.cpp — the OSC 133 bracket reaches a client
// on a Sync update, in a REAL daemon driving a REAL shell.
//
// A row's mark travels in its row record, so an update that carries the row
// carries the mark. Nothing in a unit test sees what lies between the shell
// and that record — whether the escape survives the PTY and the parse,
// whether the mark reaches the session's Terminal before the frame that
// publishes it, whether the client is sent an update at all rather than a
// checkpoint. Each of those is a way the encoder can be right and the bracket
// still arrive only with the next checkpoint, which is the shape of the report
// this came from.
//
// So the client here is the client end of a Sync connection fed the real
// frames in arrival order, and the assertion is the transition: a row that held
// no prompt mark holds one after an update is applied, with no checkpoint in
// between. A checkpoint carrying the same mark proves nothing.
//
// The workload sends the escape as a command rather than typing it at a shell
// with integration installed, so the test needs no shell configuration and no
// prompt string: `printf` emits OSC 133 A, and the daemon's next publish
// carries whatever moved.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/Scrollback.h"
#include "Core/TerminalOperations.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"
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

using namespace Ubiquitty::HostTest;

namespace {

bool fail(const char* what)
{
    std::fprintf(stderr, "sync_prompt_mark_e2e: FAIL — %s\n", what);
    return false;
}

std::uint64_t nowMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// The client: the client end of a Sync connection, and the one transition
// this test is about — an update gave a row a prompt mark the client did not
// have.
struct MirrorClient {
    UbiquittyTest::ScreenClient screen { 24, 80, Ubiquitty::VT::fallbackScrollbackLines,
        Ubiquitty::Protocol::Sync::LinkCost::Unmetered, Ubiquitty::Protocol::Sync::Role::Agent };
    bool didMarkArriveOnAnUpdate = false;

    [[nodiscard]] int promptMarkedRows() const
    {
        const Ubiquitty::Terminal* terminal = screen.pane();
        if (!terminal)
            return 0;
        int marked = 0;
        const int history = Ubiquitty::Screen::scrollbackCount(terminal);
        for (int r = 0; r < terminal->rows; ++r)
            if (Ubiquitty::Screen::rowMark(terminal, history + r) & Ubiquitty::Screen::osc133MarkPromptStart)
                ++marked;
        return marked;
    }

    [[nodiscard]] std::uint64_t checkpoints() const { return screen.connection().counters().checkpointsApplied; }
    [[nodiscard]] std::uint64_t updates() const { return screen.connection().counters().updatesApplied; }
};

// Carries the screen plane for `windowMilliseconds`, noting each read that
// marked a row by an update alone.
void pump(Connection& c, MirrorClient& client, int windowMilliseconds)
{
    int marked = client.promptMarkedRows();
    std::uint64_t checkpoints = client.checkpoints();
    (void)UbiquittyTest::carryScreen(c.fileDescriptor, client.screen, windowMilliseconds, [&] {
        const int nowMarked = client.promptMarkedRows();
        if (nowMarked > marked && client.checkpoints() == checkpoints)
            client.didMarkArriveOnAnUpdate = true;
        marked = nowMarked;
        checkpoints = client.checkpoints();
        return client.didMarkArriveOnAnUpdate;
    });
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }

    char homeTemplate[] = "/tmp/ubiquitty-promptmark.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // A raw client is not the uid-gated Local attach bridge, so tokenScopeAllows
    // holds it to a token's scope ceiling — creating a session needs NewSession.
    const std::string token = "promptmarktokenpromptmarktokenpromptmark";
    {
        const std::string tokenPath = macHome + "/tokens";
        std::FILE* file = std::fopen(tokenPath.c_str(), "w");
        if (!file)
            return 1;
        constexpr std::uint8_t scopeObserve = 0x01, scopeDrive = 0x02, scopeNewSession = 0x10;
        std::fprintf(file, "%s %u 0 promptmark\n", Ubiquitty::Crypto::sha256Hex(token).c_str(),
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

    bool ok = false;
    do {
        // A generous wait: the daemon is spawned cold beside a suite running at
        // -j8, and a connect that loses that race says nothing about the frames.
        if (!waitForDaemonReady(sockPath, 30)) {
            fail("daemon did not come up");
            break;
        }

        Connection c;
        c.fileDescriptor = connectDaemon(sockPath);
        if (c.fileDescriptor < 0) {
            fail("connect");
            break;
        }
        MirrorClient client;
        if (!client.screen.start({ .target = Ubiquitty::Protocol::Sync::AdmissionTarget::Session }, std::vector<std::uint8_t>(token.begin(), token.end()))) {
            fail("the canonical session greeting did not start");
            break;
        }
        pump(c, client, 4000);
        const auto welcome = client.screen.welcome();
        if (client.checkpoints() == 0 || !welcome || welcome->admission != Ubiquitty::Protocol::Sync::AttachmentAdmission::Granted
            || welcome->grantedRole != Ubiquitty::Protocol::Sync::Role::Agent
            || welcome->selection.target != Ubiquitty::Protocol::Sync::AdmissionTarget::Session
            || !welcome->selection.sessionID || !welcome->selection.daemonInstanceID
            || welcome->selection.socketName != Ubiquitty::Host::socketLeafForBinary(argv[1])) {
            fail("no checkpoint — the session never came up");
            break;
        }
        ::usleep(600 * 1000); // let the shell reach a prompt
        pump(c, client, 600);

        // Each command runs on a fresh row, so each attempt marks a row the
        // client does not have. Several of them, because a frame may legitimately
        // ship as a checkpoint — a history the shell cleared — and one attempt
        // landing there would say nothing about the update path.
        for (int attempt = 0; attempt < 6 && !client.didMarkArriveOnAnUpdate; ++attempt) {
            const std::string command = "printf '\\033]133;A\\007'\r";
            (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, client.screen, command);
            pump(c, client, 1200);
        }

        std::fprintf(stderr, "sync_prompt_mark_e2e: %llu checkpoints, %llu updates, prompt-marked rows %d\n",
            static_cast<unsigned long long>(client.checkpoints()), static_cast<unsigned long long>(client.updates()), client.promptMarkedRows());

        // The premise: this client was sent updates at all. Without it the run
        // below is a checkpoint-only session and its silence means nothing.
        if (client.updates() == 0) {
            fail("the daemon sent this client no update to carry a mark on");
            break;
        }
        if (!client.didMarkArriveOnAnUpdate) {
            fail("no update carried the prompt mark — the bracket waits for a checkpoint");
            break;
        }
        ok = true;
    } while (false);

    ::kill(daemon, SIGTERM);
    int status = 0;
    ::waitpid(daemon, &status, 0);

    if (ok)
        std::fprintf(stderr, "sync_prompt_mark_e2e: a real daemon lands an OSC 133 mark on a client's Sync update\n");
    return ok ? 0 : 1;
}
