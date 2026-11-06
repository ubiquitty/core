// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/pane_facts_attachments.cpp — every attachment of a shared pane
// receives the same, complete pane facts, and a second attachment never
// changes what the first one sees.
//
// A login shell has the same dashed argv[0] as the app launches, with its master
// re-homed by the real in-process two-phase sender. The actual terminal, master
// and attempt remain in this fixture. A native owner attaches directly; an agent
// greets its screen plane as an agent and is let in by the human's allow-once.
//
// Asserted:
//   - the human's PaneFacts name the foreground process as a local pane's
//     title does: argv[0] with the login dash ("-zsh"), not the executable's
//     file name;
//   - the agent receives a PaneFacts record too, byte-identical to the
//     human's;
//   - after the agent attaches, a cwd change re-sends the record to both, and
//     both still carry the process: no attachment's record is partial.
//
// Exit 0 on success. This live fixture is quarantined, not a unit-test helper.
//

#include "ConsentClient.h"
#include "Core/Crypto/SHA256.h"
#include "LocalAdoptionFixture.h"
#include "Platform/Host/HostSocketName.h"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <climits>
#include <csignal>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;

using namespace Ubiquitty::HostTest;

/// The title inputs of one canonical record, plus its whole encoded body.
struct PaneFacts {
    std::string body;
    std::uint32_t revision = 0;
    std::string currentDirectory;
    std::string processChain;
    std::string processChainWithArguments;
    std::string foregroundName;
    std::string shellCommandName;
    std::string ttyName;
};

/// Decodes a body selected by the exact bound PaneFacts envelope.
std::optional<PaneFacts> parsePaneFacts(const std::string& body)
{
    const auto record = Sync::decodePaneFacts({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    if (!record)
        return std::nullopt;
    PaneFacts facts;
    facts.body = body;
    facts.revision = record->revision;
    facts.currentDirectory = record->currentDirectory;
    facts.processChain = record->processChain;
    facts.processChainWithArguments = record->processChainWithArguments;
    facts.foregroundName = record->foreground.name;
    facts.shellCommandName = record->shellCommandName;
    facts.ttyName = record->terminalName;
    return facts;
}

/// Reads PaneFacts records until one satisfies `accept` or the deadline
/// passes, keeping the last one read either way.
template<typename Accept>
bool waitForFacts(Connection& connection, std::optional<PaneFacts>& last, int milliseconds, Accept&& accept)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    if (!waitForPane(connection, milliseconds))
        return false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return false;
        std::string body;
        if (!waitFor(connection, Sync::FrameKind::PaneFacts, body, static_cast<int>(left), connection.paneHandle))
            return false;
        last = parsePaneFacts(body);
        if (last && accept(*last))
            return true;
    }
}

std::string describe(const std::optional<PaneFacts>& facts)
{
    if (!facts)
        return "(none)";
    return "revision=" + std::to_string(facts->revision) + " cwd='" + facts->currentDirectory + "' chain='" + facts->processChain + "' chainWithArguments='" + facts->processChainWithArguments + "' foreground='" + facts->foregroundName + "' shell='" + facts->shellCommandName + "' tty='" + facts->ttyName + "'";
}

int failures = 0;

void fail(const std::string& message)
{
    std::fprintf(stderr, "pane_facts_attachments: FAIL — %s\n", message.c_str());
    ++failures;
}

Sync::Greet greetingFor(Sync::AdmissionRequest request, Sync::ID128 instance,
    Sync::ClientKind kind, Sync::Role role, const std::string& token = {})
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = instance;
    greeting.clientKind = kind;
    greeting.requestedRole = role;
    greeting.request = std::move(request);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
    if (!token.empty())
        greeting.credential.emplace(token.begin(), token.end());
    return greeting;
}

/// The login shell's path, and the name a local pane's title gives it.
struct LoginShell {
    std::string path;
    std::string dashedName;
};

LoginShell loginShell()
{
    if (::access("/bin/zsh", X_OK) == 0)
        return { "/bin/zsh", "-zsh" };
    return { "/bin/sh", "-sh" };
}

} // namespace

int main()
{
    char homeTemplate[] = "/tmp/ubiquitty-panefacts.XXXXXX";
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
    char resolvedHome[PATH_MAX];
    if (::realpath(home, resolvedHome) == nullptr)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string zdotdir = std::string(home) + "/zdotdir";
    const std::string workdir = std::string(resolvedHome) + "/workdir";
    ::mkdir(macHome.c_str(), 0700);
    ::mkdir(zdotdir.c_str(), 0700);
    ::mkdir(workdir.c_str(), 0700);
    // A zsh with no startup file in ZDOTDIR runs its new-user menu where one
    // is installed, as on Fedora, and the menu reads the `cd` typed below as
    // its answers. An empty .zshrc is a configured user.
    if (FILE* zshrc = std::fopen((zdotdir + "/.zshrc").c_str(), "w"))
        std::fclose(zshrc);
    else
        return 1;
    const std::string agentToken = "panefactsagentpanefactsagentpanefactsagent";
    FILE* tokens = std::fopen((macHome + "/tokens").c_str(), "w");
    if (!tokens)
        return 1;
    std::fprintf(tokens, "%s %u 0 pane-facts\n", Ubiquitty::Crypto::sha256Hex(agentToken).c_str(), static_cast<unsigned>(Ubiquitty::Protocol::Sync::ScopeSet { Ubiquitty::Protocol::Sync::Scope::Observe }.toRaw()));
    std::fclose(tokens);
    const LoginShell shell = loginShell();
    LocalAdoptionFixture fixture;
    if (!fixture.start(macHome))
        return 1;
    const auto& socketLeaf = fixture.socketLeaf();
    const auto& socketPath = fixture.socketPath();
    std::vector<Ubiquitty::PAL::UnixFileDescriptor> connections;
    const auto connectOwner = [&] {
        Ubiquitty::PAL::UnixFileDescriptor socket = fixture.connectOwner();
        const int descriptor = socket.get();
        if (socket)
            connections.push_back(std::move(socket));
        return descriptor;
    };
    do {
        auto* owner = fixture.makeOwner();
        if (!owner || !fixture.startLeader(*owner, [&](int slave) {
                if (::dup2(slave, STDIN_FILENO) < 0 || ::dup2(slave, STDOUT_FILENO) < 0 || ::dup2(slave, STDERR_FILENO) < 0)
                    return 1;
                if (slave > STDERR_FILENO)
                    ::close(slave);
                if (::setenv("ZDOTDIR", zdotdir.c_str(), 1) != 0 || ::setenv("SHELL", shell.path.c_str(), 1) != 0)
                    return 1;
                // Direct exec preserves both the exact tracked child PID and
                // login argv[0], without login(1) orphaning another shell.
                ::execl(shell.path.c_str(), shell.dashedName.c_str(), static_cast<char*>(nullptr));
                return 127;
            })
            || !fixture.adopt(*owner)) {
            fail("the actual shell owner lacked exact Committed evidence");
            break;
        }
        const std::uint32_t sessionID = owner->result.adopted.sessionID;

        // A native Control receipt supplies the serving pin before either
        // targeted attach. The adopted session number is not a daemon
        // identity, and its later Welcome cannot independently pin itself.
        Connection control;
        control.fileDescriptor = connectOwner();
        if (control.fileDescriptor < 0
            || !sendGreeting(control, greetingFor({ .target = Sync::AdmissionTarget::Control }, { .low = 1, .high = 0x6661637473 }, Sync::ClientKind::Human, Sync::Role::Owner))) {
            fail("native Control greeting");
            break;
        }
        std::string body;
        if (!waitFor(control, Sync::FrameKind::Welcome, body, 3000) || !control.welcome || control.hasFailed
            || control.welcome->admission != Sync::AttachmentAdmission::Granted
            || control.welcome->selection.target != Sync::AdmissionTarget::Control
            || control.welcome->selection.sessionID || control.welcome->selection.workspaceID
            || !control.welcome->selection.daemonInstanceID || control.welcome->selection.socketName != socketLeaf
            || control.welcome->selection.daemonInstanceID != owner->result.adopted.daemonInstanceID) {
            fail("native Control receipt did not identify the exact fixture daemon");
            break;
        }
        const std::uint64_t expectedDaemon = control.welcome->selection.daemonInstanceID;
        std::vector<std::uint8_t> goodbye;
        Sync::encode(goodbye, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = {}, .peer = std::nullopt });
        if (!sendFrame(control, Sync::FrameKind::Bye, { reinterpret_cast<const char*>(goodbye.data()), goodbye.size() })) {
            fail("native Control detach");
            break;
        }

        // The human's real physical peer is the daemon's own fixture executable.
        Connection human;
        ConsentClient humanConsent(human);
        human.fileDescriptor = connectOwner();
        if (human.fileDescriptor < 0) {
            fail("human connect");
            break;
        }
        if (!sendGreeting(human, greetingFor({ .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true }, { .low = 2, .high = 0x6661637473 }, Sync::ClientKind::Human, Sync::Role::Owner))
            || !waitFor(human, Sync::FrameKind::Welcome, body, 3000) || !human.welcome || human.hasFailed
            || human.welcome->admission != Sync::AttachmentAdmission::Granted
            || human.welcome->selection.sessionID != sessionID || !humanConsent.isGranted()) {
            fail("the exact pinned human attachment was not granted");
            break;
        }
        std::optional<PaneFacts> humanFacts;
        const auto namesTheLoginShell = [&](const PaneFacts& facts) { return facts.processChain == shell.dashedName; };
        if (!waitForFacts(human, humanFacts, 15000, namesTheLoginShell))
            fail("the human's pane facts never named the login shell '" + shell.dashedName + "' — last: " + describe(humanFacts));

        // The agent, greeting its screen plane as one.
        Connection agent;
        ConsentClient agentConsent(agent);
        Ubiquitty::PAL::UnixFileDescriptor agentSocket = connectAdoptionSocket(socketPath);
        agent.fileDescriptor = agentSocket.get();
        if (agent.fileDescriptor < 0) {
            fail("agent connect");
            break;
        }
        if (!sendGreeting(agent, greetingFor({ .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = expectedDaemon, .agentLabel = "test-agent", .isAttachOnly = true }, { .low = 3, .high = 0x6661637473 }, Sync::ClientKind::Agent, Sync::Role::Agent, agentToken))) {
            fail("agent Greet was not sent");
            break;
        }
        const auto pending = agentConsent.status(Ubiquitty::Protocol::Sync::ConsentStatusPhase::Pending);
        const auto question = humanConsent.question(Ubiquitty::Protocol::Sync::ConsentKind::Attach);
        if (!pending || !question || !agent.welcome || agent.hasFailed
            || agent.welcome->admission != Sync::AttachmentAdmission::AwaitingConsent
            || agent.welcome->selection.sessionID != sessionID || agent.welcome->consent != pending->question
            || agent.admission != FixtureAdmission::Pending || agent.paneHandle
            || question->requestID != pending->question.requestID || question->sessionID != sessionID
            || pending->question.sessionID != sessionID) {
            fail("the human was never asked to let the agent in");
            break;
        }
        if (!humanConsent.respond(*question, Ubiquitty::Protocol::Sync::ConsentChoice::AllowOnce)
            || !agentConsent.status(Ubiquitty::Protocol::Sync::ConsentStatusPhase::Approved)
            || !agentConsent.isGranted() || agent.admission != FixtureAdmission::Granted) {
            fail("the exact consent decision never admitted the agent attachment");
            break;
        }

        std::optional<PaneFacts> agentFacts;
        if (!waitForFacts(agent, agentFacts, 5000, namesTheLoginShell))
            fail("the agent's pane facts never named the login shell '" + shell.dashedName + "' — last: " + describe(agentFacts));

        // A moved directory re-sends the record. Both attachments get it,
        // whole: the process is still in it, on the human's side too.
        if (!sendInput(human, "cd " + workdir + "\r")) {
            fail("the host never bound the human's pane");
            break;
        }
        const auto namesTheMove = [&](const PaneFacts& facts) { return facts.currentDirectory == workdir; };
        std::optional<PaneFacts> humanMoved;
        if (!waitForFacts(human, humanMoved, 10000, namesTheMove))
            fail("the human never saw the cwd move — last: " + describe(humanMoved));
        else if (humanMoved->processChain != shell.dashedName)
            fail("the human's record lost the process once the agent attached — " + describe(humanMoved));
        std::optional<PaneFacts> agentMoved;
        if (!waitForFacts(agent, agentMoved, 5000, namesTheMove))
            fail("the agent never saw the cwd move — last: " + describe(agentMoved));
        if (humanMoved && agentMoved && humanMoved->body != agentMoved->body)
            fail("the attachments disagree — human: " + describe(humanMoved) + "; agent: " + describe(agentMoved));
        if (humanMoved && humanFacts && humanMoved->revision == humanFacts->revision)
            fail("a changed record kept its revision — " + describe(humanMoved));
    } while (false);

    if (failures != 0)
        return 1;
    std::printf("pane_facts_attachments: a shared login shell's pane facts name it '%s' as a local pane does; "
                "the agent receives the human's record byte for byte, and its attachment leaves the human's whole\n",
        shell.dashedName.c_str());
    return 0;
}
