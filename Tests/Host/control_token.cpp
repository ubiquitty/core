// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/control_token.cpp — a Control-only capability token.
//
// A token scoped for control operations only (no observe) must be usable as a
// control-only client (agent ls/kill, AdmissionTarget::Control) yet must
// still be unable to attach to (observe/drive) a real session.
//
// Drives the REAL ubiquitty-host daemon (UBIQUITTY_REQUIRE_TOKEN=1) over the unix
// socket:
//   - a canonical control-only Greet is granted without binding a pane,
//   - a Greet carrying the same control-only token but a real session id is
//     refused with OutsideScope — a control token cannot observe a session.
// The real session is first created by a full-scope token so the negative case
// proves denial on an EXISTING session, not the create/sessionID=0 path.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
// Scope bits.
constexpr std::uint8_t scopeObserve = 0x01;
constexpr std::uint8_t scopeDrive = 0x02;
constexpr std::uint8_t scopeControl = 0x04;
constexpr std::uint8_t scopeNewSession = 0x10;
using namespace Ubiquitty::HostTest;

bool greetAgent(Connection& connection, Sync::AdmissionRequest request, std::string_view token)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(connection.fileDescriptor) + 1, .high = 0x636f6e74726f6c };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request = std::move(request);
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.credential.emplace(token.begin(), token.end());
    return sendGreeting(connection, std::move(greeting));
}

// A direct child remains ours until reaped. Never signal an already-reaped or
// unobservable PID, which may have been reused by another process.
void stopDaemon(pid_t processID)
{
    if (processID <= 0)
        return;
    const auto observe = [processID] {
        pid_t result;
        do {
            result = ::waitpid(processID, nullptr, WNOHANG);
        } while (result < 0 && errno == EINTR);
        return result;
    };
    if (observe() != 0)
        return;
    if (::kill(processID, SIGTERM) != 0)
        return;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (observe() != 0)
            return;
        ::usleep(10 * 1000);
    }
    if (observe() != 0 || ::kill(processID, SIGKILL) != 0)
        return;
    while (::waitpid(processID, nullptr, 0) < 0 && errno == EINTR) {
    }
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "control_token: FAIL — %s\n", msg);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-ctrl.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    std::string macHome = std::string(home) + "/.ubiquitty";
    std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // Seed: a full-scope token (creates the real session) and a control-only
    // token (no observe/drive) used for both the positive and negative cases.
    const std::string fullToken = "fulltokenfulltokenfulltokenfulltoken";
    const std::string controlToken = "controltokencontroltokencontroltoken";
    const std::uint8_t fullScope = scopeObserve | scopeDrive | scopeNewSession;
    const std::uint8_t controlScope = scopeControl; // control only
    {
        std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 full\n", Ubiquitty::Crypto::sha256Hex(fullToken).c_str(), static_cast<unsigned>(fullScope));
        std::fprintf(file, "%s %u 0 control\n", Ubiquitty::Crypto::sha256Hex(controlToken).c_str(), static_cast<unsigned>(controlScope));
        std::fclose(file);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("UBIQUITTY_REQUIRE_TOKEN", "1", 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    bool isDaemonReady = waitForDaemonReady(sockPath);

    int rc = 1;
    std::string body;
    do {
        if (!isDaemonReady) {
            fail("daemon never accepted a connection");
            break;
        }

        // --- A full-scope token creates a REAL session; capture its id. ---
        Connection full;
        full.fileDescriptor = connectDaemon(sockPath);
        if (full.fileDescriptor < 0) {
            fail("full connect");
            break;
        }
        if (!greetAgent(full, { .target = Sync::AdmissionTarget::Session }, fullToken)
            || !waitFor(full, Sync::FrameKind::Welcome, body, 3000) || !full.welcome
            || full.welcome->admission != Sync::AttachmentAdmission::Granted
            || full.welcome->selection.target != Sync::AdmissionTarget::Session) {
            fail("full-scope token should create a granted session");
            break;
        }
        const auto realSession = full.welcome->selection.sessionID;
        const auto daemonInstance = full.welcome->selection.daemonInstanceID;
        if (!realSession || !daemonInstance || !waitForPane(full)) {
            fail("the granted session must have its actual owner and bound pane");
            break;
        }

        // --- POSITIVE: a control-only token connects as control-only. ---
        Connection control;
        control.fileDescriptor = connectDaemon(sockPath);
        if (control.fileDescriptor < 0) {
            fail("control connect");
            break;
        }
        if (!greetAgent(control, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = daemonInstance }, controlToken)
            || !waitFor(control, Sync::FrameKind::Welcome, body, 3000) || !control.welcome
            || control.welcome->admission != Sync::AttachmentAdmission::Granted
            || control.welcome->selection.target != Sync::AdmissionTarget::Control
            || control.welcome->selection.daemonInstanceID != daemonInstance
            || control.welcome->selection.sessionID || control.welcome->selection.workspaceID
            || !control.panes.empty() || control.paneHandle || control.hasFailed) {
            fail("control-only token must grant only the unbound Control target on the same host");
            break;
        }

        // --- NEGATIVE: the same control-only token attaching the REAL session
        // is DENIED — a control token cannot observe/drive a session. ---
        Connection attach;
        attach.fileDescriptor = connectDaemon(sockPath);
        if (attach.fileDescriptor < 0) {
            fail("attach connect");
            break;
        }
        if (!greetAgent(attach, { .target = Sync::AdmissionTarget::Session, .sessionID = realSession, .expectedDaemonInstanceID = daemonInstance, .isAttachOnly = true }, controlToken)
            || !waitFor(attach, Sync::FrameKind::Bye, body, 3000)) {
            fail("control-only token attaching a real session should receive a typed refusal");
            break;
        }
        const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
        if (!bye || bye->reason != Sync::ByeReason::OutsideScope || attach.welcome
            || !attach.panes.empty() || attach.paneHandle || attach.hasFailed) {
            fail("control-only token must get OutsideScope without Welcome or a bound pane");
            break;
        }
        if (waitForScreen(attach, 500)) {
            fail("control-only token must NOT receive a checkpoint of a real session");
            break;
        }

        rc = 0;
    } while (false);

    if (rc == 0)
        std::printf("control_token: control-only token accepted as control-only; "
                    "the same token is scope-denied when attaching a real session\n");

    stopDaemon(daemon);
    return rc;
}
