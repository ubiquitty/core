// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/session_shell_environment.cpp — what a shell the daemon starts
// inherits from it.
//
// A daemon brought up by `--ensure-daemon`, with no UBIQUITTY_INTEGRATION_DIR
// of its own, hands each shell the directory it wrote its carried
// shell-integration scripts to, and those scripts are the repository's. The
// documented rc line sources the integration only when that variable is set,
// so without it `run` exit codes and `tail` never work in a daemon's shells.
// The shell also runs under the umask --ensure-daemon was started with, not
// the owner-only mask the detached daemon keeps for its own files.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary, argv[2] = the
// repository root.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "DetachedDaemons.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <climits>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

int failures = 0;

void expect(bool isMet, const char* what) // bool-ok: an assertion takes the fact it checks
{
    if (!isMet) {
        std::fprintf(stderr, "session_shell_environment: FAIL — %s\n", what);
        ++failures;
    }
}

std::string contentsOf(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return file ? std::string { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() } : std::string {};
}

// Runs `--ensure-daemon` under the sandbox home with /bin/sh as the login
// shell and `mask` as the umask it is started under. Returns its exit code.
int ensureDaemon(const char* hostBinary, const std::string& home, const std::string& macHome, mode_t mask)
{
    const pid_t child = ::fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("HOME", home.c_str(), 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("NO_COLOR", "1", 1);
        ::setenv("CLICOLOR", "0", 1);
        ::setenv("FORCE_COLOR", "0", 1);
        ::setenv("CLICOLOR_FORCE", "0", 1);
        ::unsetenv("UBIQUITTY_INTEGRATION_DIR");
        ::umask(mask);
        char* args[] = { const_cast<char*>(hostBinary), const_cast<char*>("--ensure-daemon"), nullptr };
        ::execv(hostBinary, args);
        _exit(127);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// The lines the shell wrote to `path`, once the file is complete: the shell
// writes a temporary and renames it, so a file that exists is whole.
std::vector<std::string> factsWritten(const std::string& path, Connection& client, const std::string& command)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        (void)sendInput(client, command);
        for (int tick = 0; tick < 20; ++tick) {
            std::string ignored;
            (void)waitFor(client, Sync::FrameKind::PaneFacts, ignored, 50, client.paneHandle);
            struct stat info {};
            if (::stat(path.c_str(), &info) == 0) {
                std::vector<std::string> lines;
                std::istringstream stream(contentsOf(path));
                for (std::string line; std::getline(stream, line);)
                    lines.push_back(line);
                return lines;
            }
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host> <repository root>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    const std::string root = argv[2];
    char homeTemplate[] = "/tmp/ubiquitty-shellenv.XXXXXX";
    char* homeDirectory = ::mkdtemp(homeTemplate);
    if (!homeDirectory)
        return 1;
    char resolvedHome[PATH_MAX];
    if (::realpath(homeDirectory, resolvedHome) == nullptr)
        return 1;
    const std::string home = resolvedHome;
    const std::string macHome = home + "/.ubiquitty";
    const std::string socketPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary);
    ::mkdir(macHome.c_str(), 0700);

    // This raw test binary is Agent-origin, so creating a session and typing
    // into it need a token carrying NewSession and Drive.
    const std::string token = "shellenvironmenttokenshellenvironmenttoken";
    {
        using Ubiquitty::Protocol::Sync::Scope;
        const std::uint8_t scope = (Scope::Observe | Scope::Drive | Scope::NewSession).toRaw();
        std::FILE* file = std::fopen((macHome + "/tokens").c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 shellenvironment\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scope));
        std::fclose(file);
    }

    expect(ensureDaemon(hostBinary, home, macHome, 022) == 0, "--ensure-daemon brings the daemon up");
    expect(waitForDaemonReady(socketPath), "the daemon listens");

    Connection client;
    client.fileDescriptor = connectDaemon(socketPath);
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x7368656c6c656e76 };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.credential.emplace(token.begin(), token.end());
    std::string body;
    const bool isAttached = client.fileDescriptor >= 0 && sendGreeting(client, std::move(greeting)) && waitFor(client, Sync::FrameKind::Welcome, body, 3000) && waitForPane(client, 3000);
    expect(isAttached, "a session is created and its pane bound");

    if (isAttached) {
        const std::string facts = home + "/facts";
        const auto lines = factsWritten(facts, client, "{ printf '%s\\n' \"$UBIQUITTY_INTEGRATION_DIR\"; umask; printf '%s\\n' \"${NO_COLOR-unset}\" \"${CLICOLOR-unset}\" \"${FORCE_COLOR-unset}\" \"${CLICOLOR_FORCE-unset}\" \"$COLORTERM\"; } > " + facts + ".partial && mv " + facts + ".partial " + facts + "\n");
        expect(lines.size() == 7, "the shell reported its environment");
        // The daemon runs under 077 of its own; its shell creates files as
        // the person who ran --ensure-daemon does.
        const unsigned long mask = lines.size() >= 2 ? std::strtoul(lines[1].c_str(), nullptr, 8) : 0777;
        expect(mask == 022, "the shell runs under the umask --ensure-daemon was started with, not the daemon's 077");
        expect(lines.size() == 7 && lines[2] == "unset" && lines[3] == "unset"
                && lines[4] == "unset" && lines[5] == "unset" && lines[6] == "truecolor",
            "a terminal shell advertises truecolor without inheriting its daemon launcher's color controls");
        const std::string directory = lines.empty() ? std::string {} : lines[0];
        expect(directory == macHome + "/shell-integration", "the shell is handed the control directory's shell-integration directory");
        for (const char* script : { "bash.sh", "zsh.sh", "fish.fish" }) {
            const std::string carried = contentsOf(directory + "/" + script);
            expect(!carried.empty() && carried == contentsOf(root + "/Resources/shell-integration/" + script), "each script written out is the repository's");
        }
        ::close(client.fileDescriptor);
    }

    expect(endDetachedDaemons(macHome), "the daemon --ensure-daemon started has ended");
    if (failures)
        return 1;
    std::printf("session_shell_environment: a daemon's shell is handed UBIQUITTY_INTEGRATION_DIR naming the scripts it carries, under its launcher's umask\n");
    return 0;
}
