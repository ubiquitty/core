// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/resize_clamp.cpp — over-limit Greet and Viewport dims are clamped
// to the Core sanity ceiling before both the PTY and the Core model.
//
// Drives the REAL ubiquitty-host daemon over the unix socket:
//   - a Greet with 65535x65535 creates a session; the typed directory reports
//     Core dimensions capped to maxColumns(1024) x maxRows(256),
//   - the PTY the child sees is capped too: `stty size` (the child's TIOCGWINSZ)
//     written to a file shows rows<=256, cols<=1024,
//   - an over-limit Viewport from a person's client, which reaches the daemon
//     through its attach bridge, is likewise clamped in both Core and PTY.
//
// Asserts only the clamped DIMENSIONS (no screen-plane wire internals). Exit 0 on
// success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <chrono>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Core sanity ceiling comes from Source/Core/VT/Constants.h
// (maxColumns / maxRows), included via the harness headers.

namespace Sync = Ubiquitty::Protocol::Sync;

using Ubiquitty::maxColumns;
using Ubiquitty::maxRows;
using Ubiquitty::Protocol::Sync::Scope;

using namespace Ubiquitty::HostTest;

bool greet(Connection& connection, Sync::AdmissionRequest request, Sync::ClientKind kind,
    std::uint16_t columns, std::uint16_t rows, std::string_view token = {})
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(connection.fileDescriptor) + 1, .high = 0x726573697a65 };
    greeting.clientKind = kind;
    greeting.requestedRole = kind == Sync::ClientKind::Agent ? Sync::Role::Agent : Sync::Role::Owner;
    greeting.request = std::move(request);
    greeting.facts.viewportColumns = columns;
    greeting.facts.viewportRows = rows;
    if (!token.empty())
        greeting.credential.emplace(token.begin(), token.end());
    return sendGreeting(connection, std::move(greeting));
}

// Read "<rows> <cols>" from the child-written stty file. Returns false until it
// has two integers.
bool readSttySize(const std::string& path, int& rows, int& columns)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return false;
    int r = 0;
    int c = 0;
    const int got = std::fscanf(file, "%d %d", &r, &c);
    std::fclose(file);
    if (got != 2)
        return false;
    rows = r;
    columns = c;
    return true;
}

// Drive the child to write its PTY winsize (TIOCGWINSZ) to `path` and read it
// back. Types it again periodically (the shell may not be at a prompt yet —
// still starting, or repainting after a SIGWINCH). Does not read the socket,
// because an over-limit grid clamps to the largest a host holds, and this
// test asserts nothing about its screen. Draining it is unnecessary — the daemon may drop this client for
// output backlog, but the multiplexed SESSION (and its shell) persists, so the
// input already delivered still runs and writes the file. Returns false on
// timeout. The path must not already exist.
bool measurePTYSize(Connection& client, const std::string& path, int& rows, int& columns)
{
    ::unlink(path.c_str());
    const std::string in = "stty size > " + path + "\n";
    // Bounded by the clock rather than by a number of passes. What is being
    // waited on is a shell inside the PTY starting, running stty and writing a
    // file — work that takes longer exactly when the machine is loaded, while a
    // loop counted in iterations gives it less time under the same conditions.
    //
    // The line is typed again periodically because the first one can land before
    // the shell is reading, and a resend is harmless: the file is the result and
    // writing it twice says the same thing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    auto nextSend = std::chrono::steady_clock::now();
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSend) {
            (void)sendInput(client, in);
            nextSend = now + std::chrono::seconds(1);
        }
        ::usleep(50 * 1000);
        if (readSttySize(path, rows, columns))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
    }
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "resize_clamp: FAIL — %s\n", msg);
    return false;
}

// Query the Core dims for `sessionID` via a control client. Returns true on a
// successful typed-directory hit on the original daemon.
bool queryCoreDimensions(const std::string& sockPath, const std::string& token, std::uint64_t daemonInstance,
    std::uint32_t sessionID, int& columns, int& rows)
{
    Connection control;
    control.fileDescriptor = connectDaemon(sockPath);
    if (control.fileDescriptor < 0)
        return false;
    std::string body;
    if (!greet(control, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = daemonInstance }, Sync::ClientKind::Agent, 80, 24, token)
        || !waitFor(control, Sync::FrameKind::Welcome, body, 3000) || !control.welcome
        || control.welcome->admission != Sync::AttachmentAdmission::Granted || !control.panes.empty()) {
        ::close(control.fileDescriptor);
        return false;
    }
    bool ok = false;
    if (const auto directory = readSessionDirectory(control, 1)) {
        for (const auto& session : *directory) {
            if (session.id != sessionID)
                continue;
            columns = session.columns;
            rows = session.rows;
            ok = true;
            break;
        }
    }
    ::close(control.fileDescriptor);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-resize.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    std::string macHome = std::string(home) + "/.ubiquitty";
    std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);
    const std::string sttyHello = std::string(home) + "/stty-hello.txt";
    const std::string sttyResize = std::string(home) + "/stty-resize.txt";

    // Seed a token store entry covering every scope this test's connections
    // need: Observe+Drive+NewSession for creation, Control for the directory.
    const std::string token = "resizeclamptokenresizeclamptokenresize1";
    const std::uint8_t scope = (Scope::Observe | Scope::Drive | Scope::Control | Scope::NewSession).toRaw();
    {
        const std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 resizeclamp\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scope));
        std::fclose(file);
    }

    pid_t bridge = -1;
    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
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

        int coreColumns = 0;
        int coreRows = 0;
        int ptyRows = 0;
        int ptyColumns = 0;

        // An over-limit Greet clamps both Core and the child's PTY.
        Connection helloClient;
        helloClient.fileDescriptor = connectDaemon(sockPath);
        if (helloClient.fileDescriptor < 0) {
            fail("Greet client connect");
            break;
        }
        if (!greet(helloClient, { .target = Sync::AdmissionTarget::Session }, Sync::ClientKind::Agent, 65535, 65535, token)
            || !waitFor(helloClient, Sync::FrameKind::Welcome, body, 3000) || !helloClient.welcome
            || helloClient.welcome->admission != Sync::AttachmentAdmission::Granted
            || !helloClient.welcome->selection.sessionID || !helloClient.welcome->selection.daemonInstanceID) {
            fail("over-limit Greet should create a granted session");
            break;
        }
        const auto helloSession = helloClient.welcome->selection.sessionID;
        const auto daemonInstance = helloClient.welcome->selection.daemonInstanceID;

        if (!queryCoreDimensions(sockPath, token, daemonInstance, helloSession, coreColumns, coreRows)) {
            fail("could not read Core dimensions through the typed directory after Greet");
            break;
        }
        if (coreColumns > maxColumns || coreRows > maxRows || coreColumns <= 0 || coreRows <= 0) {
            std::fprintf(stderr, "resize_clamp: Greet Core dimensions %dx%d not clamped to %dx%d\n", coreColumns, coreRows, maxColumns, maxRows);
            fail("Greet Core dimensions not clamped");
            break;
        }
        if (!measurePTYSize(helloClient, sttyHello, ptyRows, ptyColumns)) {
            fail("child never reported its PTY size after Greet");
            break;
        }
        if (ptyColumns > maxColumns || ptyRows > maxRows || ptyColumns <= 0 || ptyRows <= 0) {
            std::fprintf(stderr, "resize_clamp: Greet PTY dimensions %dx%d (columns x rows) not clamped\n", ptyColumns, ptyRows);
            fail("Greet PTY dimensions not clamped");
            break;
        }

        // A normal Greet isolates the later over-limit Viewport path.
        // Starting at a normal size isolates the Viewport path: unclamped the
        // PTY would receive 60000x50000; clamped, the same cap. Only a
        // person's client has a viewport of a pane, so this one reaches the
        // daemon through its own attach bridge.
        Connection resizeClient;
        resizeClient.fileDescriptor = connectThroughAttachBridge(argv[1], macHome, bridge);
        if (resizeClient.fileDescriptor < 0) {
            fail("Viewport client connect");
            break;
        }
        // The bridge is verified by the daemon as a person. Presenting an
        // Agent token here would lower it to an Agent.
        if (!greet(resizeClient, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = daemonInstance }, Sync::ClientKind::Human, 80, 24)
            || !waitFor(resizeClient, Sync::FrameKind::Welcome, body, 3000) || !resizeClient.welcome
            || resizeClient.welcome->admission != Sync::AttachmentAdmission::Granted
            || resizeClient.welcome->grantedRole != Sync::Role::Owner || !resizeClient.welcome->selection.sessionID) {
            fail("the genuine human bridge should create a granted session");
            break;
        }
        const auto resizeSession = resizeClient.welcome->selection.sessionID;

        if (!sendViewport(resizeClient, 60000, 50000)) {
            fail("the host never bound the Viewport client's pane");
            break;
        }
        // Measure the PTY FIRST: the over-limit Viewport makes the grid maximal,
        // so the checkpoint it brings (unread) would soon hit the client output
        // cap. measurePTYSize types on its own clock so the line lands before
        // any drop; the multiplexed session persists regardless.
        ptyRows = ptyColumns = 0;
        if (!measurePTYSize(resizeClient, sttyResize, ptyRows, ptyColumns)) {
            fail("child never reported its PTY size after the Viewport");
            break;
        }
        if (ptyColumns > maxColumns || ptyRows > maxRows || ptyColumns <= 0 || ptyRows <= 0) {
            std::fprintf(stderr, "resize_clamp: Viewport PTY dims %dx%d (cols x rows) not clamped\n", ptyColumns, ptyRows);
            fail("Viewport PTY dims not clamped");
            break;
        }
        // Core dims via a fresh control client (small frames; never overflows).
        coreColumns = coreRows = 0;
        if (!queryCoreDimensions(sockPath, token, daemonInstance, resizeSession, coreColumns, coreRows)) {
            fail("could not read Core dimensions through the typed directory after Viewport");
            break;
        }
        if (coreColumns > maxColumns || coreRows > maxRows || coreColumns <= 0 || coreRows <= 0) {
            std::fprintf(stderr, "resize_clamp: Viewport Core dims %dx%d not clamped to %dx%d\n", coreColumns, coreRows, maxColumns, maxRows);
            fail("Viewport Core dims not clamped");
            break;
        }

        rc = 0;
    } while (false);

    if (rc == 0)
        std::printf("resize_clamp: over-limit Greet and Viewport clamp Core and PTY dimensions to "
                    "%dx%d\n",
            maxColumns, maxRows);

    if (bridge > 0) {
        ::kill(bridge, SIGTERM);
        ::waitpid(bridge, nullptr, 0);
    }
    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, WNOHANG);
    return rc;
}
