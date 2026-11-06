// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/host_ptyfacts_cwd.cpp — ubiquitty-host's processCurrentDirectory
// (Source/Platform/POSIX/PTYFacts.cpp) actually reports a real cwd on Darwin,
// not an always-empty stub.
//
// PaneFacts.currentDirectory falls back to
// PAL::Stream::foregroundCurrentDirectory() -> PAL::processCurrentDirectory()
// whenever the shell stays OSC-7-silent (Daemon.cpp's refreshPaneFacts). This
// drives a REAL bare shell (no shell-integration OSC-7) through a `cd`, then
// asserts the daemon's own PaneFacts reports that real directory — proving
// the Darwin proc_pidinfo path actually works end to end, not just compiles.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <climits>
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

bool fail(const char* msg)
{
    std::fprintf(stderr, "host_ptyfacts_cwd: FAIL — %s\n", msg);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-ptyfacts.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string homeDirectory(home);
    const std::string macHome = homeDirectory + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // This raw test binary is classified Agent-origin (only a
    // kernel-verified "ubiquitty-host" peer is Local), so input (the `cd`
    // below) needs a token carrying Scope::Drive or the host closes the
    // connection and the shell never receives it.
    const std::string token = "ptyfactscwdtokenptyfactscwdtokenptyfacts1";
    {
        using Ubiquitty::Protocol::Sync::Scope;
        const std::uint8_t scope = (Scope::Observe | Scope::Drive | Scope::NewSession).toRaw();
        const std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 ptyfactscwd\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scope));
        std::fclose(file);
    }

    // Real, resolved directory the bare shell will `cd` into — realpath'd
    // because the daemon's own proc_pidinfo report is already canonical
    // (e.g. a /tmp -> /private/tmp symlink), so a literal-string compare
    // against an un-resolved path would spuriously fail.
    char resolvedHome[PATH_MAX];
    if (::realpath(homeDirectory.c_str(), resolvedHome) == nullptr)
        return 1;
    const std::string workdir = std::string(resolvedHome) + "/workdir";
    ::mkdir(workdir.c_str(), 0700);

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        // Default shell (/bin/sh): no shell-integration OSC-7, so
        // Terminal::currentDirectory stays silent and refreshPaneFacts must fall
        // back to processCurrentDirectory.
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    bool isDaemonUp = waitForDaemonReady(sockPath);

    int rc = 1;
    do {
        if (!isDaemonUp) {
            fail("daemon did not come up");
            break;
        }

        Connection client;
        client.fileDescriptor = connectDaemon(sockPath);
        if (client.fileDescriptor < 0) {
            fail("connect");
            break;
        }
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { .low = 1, .high = 0x707479637764 };
        greeting.clientKind = Sync::ClientKind::Agent;
        greeting.requestedRole = Sync::Role::Agent;
        greeting.request.target = Sync::AdmissionTarget::Session;
        greeting.facts.viewportColumns = 80;
        greeting.facts.viewportRows = 24;
        greeting.credential.emplace(token.begin(), token.end());
        std::string body;
        if (!sendGreeting(client, std::move(greeting)) || !waitFor(client, Sync::FrameKind::Welcome, body, 3000)) {
            fail("no canonical Welcome");
            break;
        }
        const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
        if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted
            || welcome->selection.target != Sync::AdmissionTarget::Session || !welcome->selection.sessionID
            || !welcome->selection.daemonInstanceID || welcome->selection.socketName != Ubiquitty::Host::socketLeafForBinary(argv[1])
            || !waitForPane(client, 3000)) {
            fail("the creation receipt did not select the actual daemon and bind its pane");
            break;
        }

        bool sawCwd = false;
        std::string lastSeenCurrentDirectory;
        for (int attempt = 0; attempt < 12 && !sawCwd; ++attempt) {
            const std::string in = "cd " + workdir + "\n";
            (void)sendInput(client, in);
            for (int i = 0; i < 20; ++i) { // ~1s per attempt
                std::string frame;
                if (waitFor(client, Sync::FrameKind::PaneFacts, frame, 50, client.paneHandle)) {
                    const auto facts = Sync::decodePaneFacts({ reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size() });
                    if (facts) {
                        lastSeenCurrentDirectory = facts->currentDirectory;
                        if (facts->currentDirectory == workdir) {
                            sawCwd = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!sawCwd) {
            std::fprintf(stderr, "host_ptyfacts_cwd: last PaneFacts.currentDirectory='%s' want='%s'\n", lastSeenCurrentDirectory.c_str(), workdir.c_str());
            fail("PaneFacts never reported the real cwd (processCurrentDirectory still empty?)");
            break;
        }

        rc = 0;
        ::close(client.fileDescriptor);
    } while (false);

    if (rc == 0)
        std::printf("host_ptyfacts_cwd: PaneFacts' OSC-7-silent fallback reports the bare shell's "
                    "real cwd via processCurrentDirectory (Darwin proc_pidinfo)\n");

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
