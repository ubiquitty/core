// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/consent_install_grant.cpp — the install-time grant, end to end:
// a host with no human attached admits the agent whose token the operator
// issued with --allow-always, and nobody else.
//
// Runs the real CLI and the real daemon under a private UBIQUITTY_HOME. A
// session exists that no client is attached to — the state a headless server
// is in. An agent presenting a plainly issued token is refused for consent
// before Welcome, without a checkpoint. An agent presenting
// the granted token is admitted with a Granted Welcome and checkpoint. A
// grant issued while the daemon runs reaches it through authenticated local reload, and
// revoking that token withdraws the grant without disturbing the other.
//
// ConsentClient uses the canonical control codecs alongside HostConnection's
// independent screen reader. Exit 0 on success. argv[1] is the host binary.
//

#include "ConsentClient.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Platform/Host/HostSocketName.h"

#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

int failures = 0;

void expect(bool isSatisfied, const char* what) // bool-ok: an assertion takes the truth it asserts
{
    if (!isSatisfied) {
        std::fprintf(stderr, "consent_install_grant: FAIL — %s\n", what);
        ++failures;
    }
}

struct RunResult {
    int exitCode = -1;
    std::string output; // stdout + stderr, interleaved
};

// Runs the host binary with `arguments` under the sandbox home, capturing both
// output streams.
RunResult run(const char* hostBinary, const std::string& macHome, const std::vector<std::string>& arguments)
{
    RunResult result;
    int pipeFileDescriptors[2] = { -1, -1 };
    if (::pipe(pipeFileDescriptors) != 0)
        return result;
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        return result;
    }
    if (child == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::dup2(pipeFileDescriptors[1], STDOUT_FILENO);
        ::dup2(pipeFileDescriptors[1], STDERR_FILENO);
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(hostBinary));
        for (const std::string& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(hostBinary, argv.data());
        _exit(127);
    }
    ::close(pipeFileDescriptors[1]);
    char buffer[4096];
    ssize_t bytesRead = 0;
    while ((bytesRead = ::read(pipeFileDescriptors[0], buffer, sizeof(buffer))) > 0)
        result.output.append(buffer, static_cast<std::size_t>(bytesRead));
    ::close(pipeFileDescriptors[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

// The secret `token issue` prints: the one line of 64 hex digits. The streams
// interleave in the capture, so the line is found by shape rather than position.
std::string tokenIn(const std::string& output)
{
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        const std::string line = output.substr(start, end - start);
        bool isHex = line.size() == 64;
        for (char c : line)
            isHex = isHex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (isHex)
            return line;
        start = end + 1;
    }
    return {};
}

// Whether a token is issued with its standing attach consent recorded.
enum class Grant : std::uint8_t {
    None,
    AllowAlways,
};

// Issues a token through the CLI and returns its secret, or empty on refusal.
std::string issue(const char* hostBinary, const std::string& macHome, const std::string& label, const std::string& scope, Grant grant, RunResult* outResult = nullptr)
{
    std::vector<std::string> arguments { "token", "issue", "--label", label, "--scope", scope };
    if (grant == Grant::AllowAlways)
        arguments.push_back("--allow-always");
    const RunResult result = run(hostBinary, macHome, arguments);
    if (outResult)
        *outResult = result;
    return result.exitCode == 0 ? tokenIn(result.output) : std::string();
}

// Sends one complete Agent Greet. Only fresh creation may omit a daemon pin;
// every existing-session request uses the retained creation receipt.
bool connectAgent(Connection& agent, const std::string& sockPath, std::uint32_t sessionID, std::uint64_t daemonInstanceID, const char* label, const std::string& token)
{
    if (sessionID && !daemonInstanceID)
        return false;
    agent.fileDescriptor = connectDaemon(sockPath);
    if (agent.fileDescriptor < 0)
        return false;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(agent.fileDescriptor) + 1, .high = 0x696e7374616c6c };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = daemonInstanceID, .agentLabel = label, .isAttachOnly = sessionID != 0 };
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.credential.emplace(token.begin(), token.end());
    return sendGreeting(agent, std::move(greeting));
}

// Whether `agent` is admitted: a Granted Welcome and checkpoint arrive.
bool isAdmitted(Connection& agent, std::string_view socketName)
{
    if (!waitForScreen(agent, 5000))
        return false;
    ConsentClient control(agent);
    return control.isGranted(0) && agent.greeting && agent.welcome && !agent.hasFailed
        && agent.welcome->selection.target == Sync::AdmissionTarget::Session
        && agent.welcome->selection.sessionID == agent.greeting->request.sessionID
        && agent.welcome->selection.daemonInstanceID == agent.greeting->request.expectedDaemonInstanceID
        && agent.welcome->selection.socketName == socketName;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-installgrant.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    struct HomeCleanup {
        std::string path;
        ~HomeCleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } homeCleanup { home };
    const std::string macHome = std::string(home) + "/.ubiquitty";
    ::mkdir(macHome.c_str(), 0700);
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary);
    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(hostBinary);

    // The operator, installing the host, mints the tokens before any daemon
    // runs: one that creates the session, one granted standing consent, and
    // one that is not.
    const std::string creatorToken = issue(hostBinary, macHome, "creator", "observe,new", Grant::None);
    const std::string grantedToken = issue(hostBinary, macHome, "granted", "observe", Grant::AllowAlways);
    const std::string plainToken = issue(hostBinary, macHome, "plain", "observe", Grant::None);
    expect(creatorToken.size() == 64 && grantedToken.size() == 64 && plainToken.size() == 64, "the three tokens issue");

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { const_cast<char*>(hostBinary), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBinary, args);
        _exit(127);
    }
    const bool isDaemonReady = waitForDaemonReady(sockPath);

    std::string body;
    do {
        if (!isDaemonReady) {
            expect(false, "daemon never accepted a connection");
            break;
        }

        // A session nobody is attached to: its creator makes it and leaves.
        // The daemon keeps the session alive across the departure, which is
        // the headless state every attach below is judged in.
        std::uint32_t sessionID = 0;
        std::uint64_t daemonInstanceID = 0;
        {
            Connection creator;
            if (!connectAgent(creator, sockPath, 0, 0, "creator", creatorToken)
                || !waitFor(creator, Sync::FrameKind::Welcome, body, 5000)) {
                expect(false, "the creator's canonical Welcome");
                break;
            }
            const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
            if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted
                || welcome->selection.target != Sync::AdmissionTarget::Session || !welcome->selection.sessionID
                || !welcome->selection.daemonInstanceID || welcome->selection.socketName != socketName) {
                expect(false, "the creator's receipt selected the actual fixture daemon");
                ::close(creator.fileDescriptor);
                break;
            }
            sessionID = welcome->selection.sessionID;
            daemonInstanceID = welcome->selection.daemonInstanceID;
            ::close(creator.fileDescriptor);
        }
        if (sessionID == 0) {
            expect(false, "the creator got no session id");
            break;
        }

        // With nobody present to consent, current admission refuses before
        // Welcome. It must not create an unanswerable pending attachment.
        {
            Connection plain;
            if (!connectAgent(plain, sockPath, sessionID, daemonInstanceID, "plain", plainToken)) {
                expect(false, "the plain agent's connect");
                break;
            }
            const bool didRefuse = waitFor(plain, Sync::FrameKind::Bye, body, 5000);
            const auto bye = didRefuse ? Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }) : std::nullopt;
            expect(bye && bye->reason == Sync::ByeReason::NoConsent, "an ungranted agent without a present human is refused for consent");
            expect(!plain.welcome && plain.panes.empty() && !waitForScreen(plain, 0) && !plain.hasFailed,
                "the refusal admits no Welcome, pane or checkpoint");
            ::close(plain.fileDescriptor);
        }

        // The granted token: admitted outright, the standing grant read at
        // daemon start.
        Connection granted;
        {
            if (!connectAgent(granted, sockPath, sessionID, daemonInstanceID, "granted", grantedToken)) {
                expect(false, "the granted agent's connect");
                break;
            }
            expect(isAdmitted(granted, socketName), "the agent whose token was issued with --allow-always is admitted with no human attached");
        }

        // A grant issued while the daemon runs: the CLI requests a reload, and the
        // list it holds in memory is the list on disk again.
        RunResult issued;
        const std::string liveToken = issue(hostBinary, macHome, "live", "observe", Grant::AllowAlways, &issued);
        expect(liveToken.size() == 64, "a grant issues while the daemon runs");
        expect(contains(issued.output, "credential reload acknowledged by 1 daemon(s); 0 socket candidate(s) unconfirmed"), "the owner acknowledged the CLI's authenticated local reload");
        Connection live;
        {
            if (!connectAgent(live, sockPath, sessionID, daemonInstanceID, "live", liveToken)) {
                expect(false, "the live agent's connect");
                break;
            }
            expect(isAdmitted(live, socketName), "a grant issued while the daemon runs admits its agent without a restart");
        }

        // Revoking the live token withdraws its grant and drops its client;
        // the grant issued before the daemon started is untouched by the
        // re-read, and its agent is still admitted afresh.
        {
            const std::string liveDigest = [&] {
                std::string digest;
                std::FILE* store = std::fopen((macHome + "/tokens").c_str(), "r");
                if (!store)
                    return digest;
                char line[4096];
                while (std::fgets(line, sizeof(line), store) != nullptr)
                    if (std::strstr(line, " live") != nullptr) {
                        char hash[128] = {};
                        if (std::sscanf(line, "%127s", hash) == 1)
                            digest = hash;
                    }
                std::fclose(store);
                return digest;
            }();
            expect(liveDigest.size() == 64, "the live token's digest is in the store");
            const RunResult revoked = run(hostBinary, macHome, { "token", "revoke", liveDigest });
            expect(revoked.exitCode == 0 && contains(revoked.output, "withdrew its standing consent"), "revoking the live token withdraws its grant");
            Connection liveAgain;
            if (!connectAgent(liveAgain, sockPath, sessionID, daemonInstanceID, "live", liveToken)) {
                expect(false, "the revoked agent's connect");
                break;
            }
            const bool didRefuse = waitFor(liveAgain, Sync::FrameKind::Bye, body, 5000);
            const auto bye = didRefuse ? Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }) : std::nullopt;
            expect(bye && bye->reason == Sync::ByeReason::TokenInvalid && !liveAgain.welcome && liveAgain.panes.empty(),
                "a fresh Greet with the revoked token is refused before admission");
            ::close(liveAgain.fileDescriptor);

            Connection grantedAgain;
            if (!connectAgent(grantedAgain, sockPath, sessionID, daemonInstanceID, "granted", grantedToken)) {
                expect(false, "the granted agent's second connect");
                break;
            }
            expect(isAdmitted(grantedAgain, socketName), "the grant issued at install survives the re-read the revoke triggered");
            ::close(grantedAgain.fileDescriptor);
        }
        ::close(live.fileDescriptor);
        ::close(granted.fileDescriptor);
    } while (false);

    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, WNOHANG);

    if (failures == 0) {
        std::printf("consent_install_grant: with no human attached, the token issued with --allow-always "
                    "is admitted, a plain one is refused for consent without a checkpoint, a grant issued to a running "
                    "daemon takes effect through authenticated local reload, and revoke withdraws it alone\n");
        return 0;
    }
    std::fprintf(stderr, "consent_install_grant: %d failure(s)\n", failures);
    return 1;
}
