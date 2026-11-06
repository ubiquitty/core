// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/consent_gate.cpp — the host-side consent gate, end to end.
//
// Spawns the real ubiquitty-host daemon and drives two hand-rolled Sync clients
// over its AF_UNIX socket: a HUMAN that owns a session, and an AGENT (a client
// naming an agent label) that attaches it. Asserts the agent is HELD — it gets
// an AwaitingConsent Welcome and exact Pending status, not a checkpoint — until
// the human's consent.respond either allows it or denies it. Consent notices,
// revocation, presence and termination use their canonical control codecs.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "ConsentClient.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"
#include "TerminationClient.h"

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

std::size_t liveAttendees(const Sync::AttachmentsList& roster, Sync::ID128 paneID, Sync::ID128& agentAttachmentID)
{
    agentAttachmentID = {};
    std::size_t count = 0;
    for (const auto& entry : roster.attachments) {
        if (entry.state != Sync::AttachmentState::Live || std::ranges::find(entry.panes, paneID) == entry.panes.end())
            continue;
        ++count;
        if (entry.clientKind == Sync::ClientKind::Agent)
            agentAttachmentID = entry.attachmentID;
    }
    return count;
}

enum class Actor : std::uint8_t {
    Human,
    Agent
};

Sync::Greet greetingFor(Actor actor, std::uint64_t instance, const std::string& label, const std::string& token)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = instance, .high = 0x636f6e73656e74 };
    greeting.clientKind = actor == Actor::Human ? Sync::ClientKind::Human : Sync::ClientKind::Agent;
    greeting.requestedRole = actor == Actor::Human ? Sync::Role::Owner : Sync::Role::Agent;
    greeting.request.agentLabel = label;
    if (!token.empty())
        greeting.credential = std::vector<std::uint8_t>(token.begin(), token.end());
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

// A new session has no previously issued ID. Its authenticated creation receipt
// is checked against the fixture's actual socket before it pins later attaches.
std::optional<Sync::ServingSelection> createSession(Connection& connection, Actor actor, std::uint64_t instance,
    std::string_view socketName, std::uint64_t daemonID = 0, const std::string& label = {}, const std::string& token = {})
{
    auto greeting = greetingFor(actor, instance, label, token);
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.expectedDaemonInstanceID = daemonID;
    if (!sendGreeting(connection, std::move(greeting)))
        return std::nullopt;
    std::string body;
    if (!waitFor(connection, Sync::FrameKind::Welcome, body, 3000))
        return std::nullopt;
    const auto welcome = Sync::decodeWelcome(bytes(body));
    if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted
        || welcome->selection.target != Sync::AdmissionTarget::Session || !welcome->selection.sessionID || !welcome->selection.daemonInstanceID
        || welcome->selection.socketName != socketName || (daemonID && welcome->selection.daemonInstanceID != daemonID))
        return std::nullopt;
    return welcome->selection;
}

bool requestAgentAttachment(Connection& connection, const Sync::ServingSelection& target, std::uint64_t instance,
    const std::string& label, const std::string& token)
{
    if (target.target != Sync::AdmissionTarget::Session || !target.sessionID || !target.daemonInstanceID)
        return false;
    auto greeting = greetingFor(Actor::Agent, instance, label, token);
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.sessionID = target.sessionID;
    greeting.request.expectedDaemonInstanceID = target.daemonInstanceID;
    greeting.request.isAttachOnly = true;
    return sendGreeting(connection, std::move(greeting));
}

bool leave(Connection& connection)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal });
    return sendFrame(connection, Sync::FrameKind::Bye, { reinterpret_cast<const char*>(body.data()), body.size() });
}

bool waitForNormalBye(Connection& connection, int milliseconds)
{
    std::string body;
    if (!waitFor(connection, Sync::FrameKind::Bye, body, milliseconds))
        return false;
    const auto bye = Sync::decodeBye(bytes(body));
    return bye && bye->reason == Sync::ByeReason::Normal;
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "consent_gate: FAIL — %s\n", msg);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-consent.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    // Every exit below is a return, so the scope guard is what removes the
    // home: this stanza runs on every Host suite, and a leaked home per run
    // accumulates in /tmp.
    struct HomeCleanup {
        std::string path;
        ~HomeCleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } homeCleanup { home };
    std::string macHome = std::string(home) + "/.ubiquitty";
    // The daemon names its socket by a content hash of the binary
    // (host-<hash>.sock, see HostSocketName.h), so derive the same leaf
    // from the binary under test rather than assuming a fixed "host.sock".
    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(argv[1]);
    std::string sockPath = macHome + "/" + socketName;

    // Seed a token-store entry so an agent can be a token-bound principal — only
    // those get a PERSISTED always-grant, which the StopAlways scoping test needs.
    const std::string agentToken = "revokescopetokenrevokescopetokenrevoke";
    ::mkdir(macHome.c_str(), 0700);
    // Each remote operation needs its actual scope before consent is relevant.
    const std::string terminatorToken = "consentgateterminatortokenconsentgate1";
    // A principal whose token carries no Observe: admitted as a client, never to a screen.
    const std::string noObserveToken = "consentgatenoobservetokenconsentgate22";
    if (FILE* tokenFile = std::fopen((macHome + "/tokens").c_str(), "w")) {
        using Ubiquitty::Protocol::Sync::Scope;
        const std::uint8_t agentScope = (Scope::Observe | Scope::Drive | Scope::NewSession).toRaw();
        const std::uint8_t terminatorScope = (Scope::Observe | Scope::NewSession | Scope::Control).toRaw();
        const std::uint8_t noObserveScope = (Scope::NewSession | Scope::Control).toRaw();
        std::fprintf(tokenFile, "%s %u 0 revoke-scope\n", Ubiquitty::Crypto::sha256Hex(agentToken).c_str(), static_cast<unsigned>(agentScope));
        std::fprintf(tokenFile, "%s %u 0 terminator\n", Ubiquitty::Crypto::sha256Hex(terminatorToken).c_str(), static_cast<unsigned>(terminatorScope));
        std::fprintf(tokenFile, "%s %u 0 no-observe\n", Ubiquitty::Crypto::sha256Hex(noObserveToken).c_str(), static_cast<unsigned>(noObserveScope));
        std::fclose(tokenFile);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        // Enforcement on: every agent leg presents a token, every person is the
        // Local bridge, and the tokenless leg expects the refusal an enforcing
        // host gives.
        ::setenv("UBIQUITTY_REQUIRE_TOKEN", "1", 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    // Wait until the daemon answers, not until its socket file appears.
    bool isDaemonReady = waitForDaemonReady(sockPath);

    // Every "human" role below must be genuinely Local-origin:
    // sessionHasHumanClient()/the consent gate itself only holds an agent
    // when a client with origin==Local (not this raw test binary) is
    // attached — a token can't satisfy this, it's an origin check, not a
    // scope one. connectDaemonAsLocalBridge spawns the real
    // "ubiquitty-host --attach" relay so the daemon's kernel-verified peer
    // for these connections is the actual bundled binary, exactly like a
    // real local GUI attach. Track each spawned relay's process id to reap at exit.
    std::vector<pid_t> bridgeProcessIDs;
    auto connectAsHuman = [&](void) -> int {
        LocalBridgeConnection bridge = connectDaemonAsLocalBridge(argv[1], macHome);
        if (bridge.bridgeProcessID > 0)
            bridgeProcessIDs.push_back(bridge.bridgeProcessID);
        return bridge.fileDescriptor;
    };

    int rc = 1;
    std::string body;
    do {
        if (!isDaemonReady) {
            fail("daemon never accepted a connection");
            break;
        }

        // --- Human owns a fresh session ---
        Connection human;
        ConsentClient humanConsent(human);
        human.fileDescriptor = connectAsHuman();
        if (human.fileDescriptor < 0) {
            fail("human connect");
            break;
        }
        const auto humanSelection = createSession(human, Actor::Human, 1, socketName);
        if (!humanSelection) {
            fail("human authenticated fresh-session Welcome");
            break;
        }
        const auto sessionID = humanSelection->sessionID;
        if (!waitForScreen(human, 3000)) {
            fail("human checkpoint");
            break;
        }
        const auto humanPaneHandle = human.paneHandle;
        if (human.panes.size() != 1) {
            fail("human fresh session must bind its one exact pane");
            break;
        }
        const auto humanPaneID = human.panes.front().paneID;

        // --- Agent attaches that session → must be HELD ---
        Connection agent;
        ConsentClient agentConsent(agent);
        agent.fileDescriptor = connectDaemon(sockPath);
        if (agent.fileDescriptor < 0) {
            fail("agent connect");
            break;
        }
        if (!requestAgentAttachment(agent, *humanSelection, 101, "test-agent", agentToken)
            || !waitFor(agent, Sync::FrameKind::Welcome, body, 3000)) {
            fail("agent exact-session Welcome");
            break;
        }
        const auto pending = agentConsent.status(Sync::ConsentStatusPhase::Pending);
        if (!pending || pending->question.sessionID != sessionID) {
            fail("agent exact pending Welcome/status (gate did not hold the attach)");
            break;
        }
        // The agent must not have a checkpoint while held.
        if (waitForScreen(agent, 400)) {
            fail("agent got a checkpoint while pending (gate leaked the screen)");
            break;
        }

        // --- Human is prompted, allows once → agent gets its checkpoint ---
        const auto question = humanConsent.question(Sync::ConsentKind::Attach);
        if (!question || question->requestID != pending->question.requestID || question->sessionID != sessionID) {
            fail("human exact attach question");
            break;
        }
        if (!humanConsent.respond(*question, Sync::ConsentChoice::AllowOnce) || !agentConsent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("the exact allow-once reaches the pending attachment before its pane");
            break;
        }
        if (!waitForPane(agent, 3000) || agent.panes.size() != 1 || agent.panes.front().paneID != humanPaneID
            || !waitFor(agent, Sync::FrameKind::PaneFacts, body, 3000, agent.paneHandle)) {
            fail("agent PaneFacts seed after allow-once");
            break;
        }
        if (!waitForScreen(agent, 3000)) {
            fail("agent checkpoint after allow-once");
            break;
        }

        // --- Second agent → human DENIES → exact terminal own status ---
        Connection agent2;
        ConsentClient agent2Consent(agent2);
        agent2.fileDescriptor = connectDaemon(sockPath);
        if (agent2.fileDescriptor < 0) {
            fail("agent2 connect");
            break;
        }
        if (!requestAgentAttachment(agent2, *humanSelection, 102, "test-agent-2", agentToken)
            || !agent2Consent.status(Sync::ConsentStatusPhase::Pending)) {
            fail("agent2 Pending");
            break;
        }
        const auto question2 = humanConsent.question(Sync::ConsentKind::Attach);
        if (!question2) {
            fail("human attach question #2");
            break;
        }
        if (!humanConsent.respond(*question2, Sync::ConsentChoice::Deny) || !agent2Consent.status(Sync::ConsentStatusPhase::Denied)) {
            fail("agent2 exact Denied status");
            break;
        }

        // --- A control agent terminating a HUMAN-occupied session is also gated.
        // agent3 owns its OWN (agent-only, ungated) session, then asks to kill the
        // human's session: held → human prompted with action=terminate → deny
        // keeps the session alive, a re-asked allow tears it down. ---
        Connection agent3;
        agent3.fileDescriptor = connectDaemon(sockPath);
        if (agent3.fileDescriptor < 0) {
            fail("agent3 connect");
            break;
        }
        const auto agent3Selection = createSession(agent3, Actor::Agent, 103, socketName, humanSelection->daemonInstanceID, "killer", terminatorToken);
        if (!agent3Selection || agent3Selection->sessionID == sessionID) {
            fail("agent3 authenticated distinct fresh-session Welcome");
            break;
        }
        if (!waitForScreen(agent3, 3000)) { // its own ungated session
            fail("agent3 checkpoint for its own session");
            break;
        }
        TerminationClient termination(agent3);
        if (!termination.send(sessionID, "consent.deny")) {
            fail("agent3 sends its typed termination");
            break;
        }
        const auto terminationQuestion = humanConsent.question(Sync::ConsentKind::Terminate);
        if (!terminationQuestion || terminationQuestion->sessionID != sessionID) {
            fail("human exact terminate question");
            break;
        }
        if (!humanConsent.respond(*terminationQuestion, Sync::ConsentChoice::Deny)) {
            fail("human exact terminate denial");
            break;
        }
        if (termination.wait() != Ubiquitty::Protocol::Control::TerminationOutcome::Denied) {
            fail("agent3 receives its correlated termination denial");
            break;
        }
        // The denied kill must not unbind the human's pane or close its link.
        if (waitFor(human, Sync::FrameKind::PaneUnbind, body, 600)
            || waitFor(human, Sync::FrameKind::Bye, body, 0)) {
            fail("denied terminate still unbound or closed the human session");
            break;
        }

        // Re-ask and ALLOW → the exact pane is unbound, then the link closes.
        if (!termination.send(sessionID, "consent.allow")) {
            fail("agent3 sends a fresh correlated termination");
            break;
        }
        const auto terminationQuestion2 = humanConsent.question(Sync::ConsentKind::Terminate);
        if (!terminationQuestion2 || terminationQuestion2->sessionID != sessionID || terminationQuestion2->requestID == terminationQuestion->requestID) {
            fail("human fresh terminate question");
            break;
        }
        if (!humanConsent.respond(*terminationQuestion2, Sync::ConsentChoice::AllowOnce)) {
            fail("human exact terminate approval");
            break;
        }
        // The requester is not attached to the destroyed session: its exact
        // typed result, not a pane unbind or an uncorrelated notice, is authoritative.
        if (termination.wait() != Ubiquitty::Protocol::Control::TerminationOutcome::Terminated) {
            fail("allowed termination did not return its exact typed result");
            break;
        }
        if (!waitFor(human, Sync::FrameKind::PaneUnbind, body, 3000)) {
            fail("allowed terminate did not unbind the human pane");
            break;
        }
        const auto unbind = Sync::decodePaneUnbind(bytes(body));
        if (!unbind || unbind->handle != humanPaneHandle || !waitForNormalBye(human, 3000)) {
            fail("allowed terminate must unbind the exact pane and close its last-pane link normally");
            break;
        }

        // --- Presence + revoke: a presence-aware human watches an agent attach
        // its session, then disconnects it; the roster updates on both events. ---
        Connection human2;
        ConsentClient human2Consent(human2);
        human2.fileDescriptor = connectAsHuman();
        if (human2.fileDescriptor < 0) {
            fail("human2 connect");
            break;
        }
        const auto human2Selection = createSession(human2, Actor::Human, 2, socketName, humanSelection->daemonInstanceID);
        if (!human2Selection || !waitForScreen(human2, 3000) || human2.panes.size() != 1) {
            fail("human2 authenticated fresh-session Welcome and pane");
            break;
        }
        const auto sessionID2 = human2Selection->sessionID;
        const auto human2PaneID = human2.panes.front().paneID;
        // Its own arrival yields an initial roster of exactly itself.
        Sync::ID128 ignored;
        const auto initialRoster = human2Consent.roster();
        if (!initialRoster || liveAttendees(*initialRoster, human2PaneID, ignored) != 1) {
            fail("human2 initial canonical roster should list only itself");
            break;
        }

        Connection agent4;
        ConsentClient agent4Consent(agent4);
        agent4.fileDescriptor = connectDaemon(sockPath);
        if (agent4.fileDescriptor < 0) {
            fail("agent4 connect");
            break;
        }
        if (!requestAgentAttachment(agent4, *human2Selection, 104, "watcher", agentToken)) {
            fail("agent4 pinned attachment request");
            break;
        }
        const auto agent4Pending = agent4Consent.status(Sync::ConsentStatusPhase::Pending);
        const auto agent4Question = human2Consent.question(Sync::ConsentKind::Attach);
        if (!agent4Pending || !agent4Question || agent4Question->requestID != agent4Pending->question.requestID) {
            fail("human2 exact question for agent4");
            break;
        }
        if (!human2Consent.respond(*agent4Question, Sync::ConsentChoice::AllowOnce) || !agent4Consent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("agent4 exact approval");
            break;
        }
        if (!waitForScreen(agent4, 3000) || agent4.panes.size() != 1 || agent4.panes.front().paneID != human2PaneID || !agent4.welcome) {
            fail("agent4 checkpoint after allow");
            break;
        }
        // The roster now shows two, including the exact attachment to revoke.
        Sync::ID128 agentAttachmentID;
        bool sawAgent = false;
        for (int tries = 0; tries < 40 && !sawAgent; ++tries) {
            const auto roster = human2Consent.roster();
            if (!roster)
                break;
            Sync::ID128 attachmentID;
            if (liveAttendees(*roster, human2PaneID, attachmentID) == 2 && attachmentID == agent4.welcome->attachmentID) {
                agentAttachmentID = attachmentID;
                sawAgent = true;
            }
        }
        if (!sawAgent) {
            fail("human2 canonical roster never listed the attached agent");
            break;
        }
        // Revoke → disconnect the agent now.
        const auto disconnectCall = human2Consent.revoke({ .sessionID = sessionID2, .action = Sync::ConsentRevocationKind::DisconnectNow, .attachmentID = agentAttachmentID, .identity = {} });
        if (!disconnectCall) {
            fail("exact attachment revoke encoding");
            break;
        }
        const auto disconnectResult = human2Consent.result(*disconnectCall);
        if (!disconnectResult || !disconnectResult->find("result") || disconnectResult->find("error")) {
            fail("exact attachment revoke result");
            break;
        }
        if (!waitForNormalBye(agent4, 3000)) {
            fail("agent4 normal Bye after revoke disconnect-now");
            break;
        }
        // The roster shrinks back to just the human.
        bool backToSelf = false;
        for (int tries = 0; tries < 40 && !backToSelf; ++tries) {
            const auto roster = human2Consent.roster();
            if (!roster)
                break;
            if (liveAttendees(*roster, human2PaneID, ignored) == 1)
                backToSelf = true;
        }
        if (!backToSelf) {
            fail("human2 canonical roster did not shrink to just the human after revoke");
            break;
        }

        // --- Allow-this-session must persist across the agent reconnecting ---
        // (#2 regression.) A human grants allow-session; the agent attaches, then
        // cleanly detaches and reconnects to the same session. The standing
        // session grant has to let it back in SILENTLY — no second prompt, and
        // certainly no immediate terminal consent timeout.
        Connection human3;
        ConsentClient human3Consent(human3);
        human3.fileDescriptor = connectAsHuman();
        if (human3.fileDescriptor < 0) {
            fail("human3 connect");
            break;
        }
        const auto human3Selection = createSession(human3, Actor::Human, 3, socketName, humanSelection->daemonInstanceID);
        if (!human3Selection || !waitForScreen(human3, 3000)) {
            fail("human3 authenticated fresh-session Welcome and checkpoint");
            break;
        }
        const auto sessionID3 = human3Selection->sessionID;

        Connection sAgent;
        ConsentClient sAgentConsent(sAgent);
        sAgent.fileDescriptor = connectDaemon(sockPath);
        if (sAgent.fileDescriptor < 0) {
            fail("session-grant agent connect");
            break;
        }
        if (!requestAgentAttachment(sAgent, *human3Selection, 105, "session-agent", agentToken)
            || !sAgentConsent.status(Sync::ConsentStatusPhase::Pending)) {
            fail("session-agent Pending");
            break;
        }
        const auto sessionQuestion = human3Consent.question(Sync::ConsentKind::Attach);
        if (!sessionQuestion || sessionQuestion->sessionID != sessionID3) {
            fail("human3 exact session-agent question");
            break;
        }
        if (!human3Consent.respond(*sessionQuestion, Sync::ConsentChoice::AllowSession) || !sAgentConsent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("session-agent exact session approval");
            break;
        }
        if (!waitForScreen(sAgent, 3000)) {
            fail("session-agent checkpoint after allow-session");
            break;
        }
        // Detach cleanly, then reconnect from the same process (same verified
        // identity) to the same session.
        if (!leave(sAgent)) {
            fail("session-agent normal detach");
            break;
        }
        ::close(sAgent.fileDescriptor);
        sAgent.fileDescriptor = -1;

        Connection sAgent2;
        ConsentClient sAgent2Consent(sAgent2);
        sAgent2.fileDescriptor = connectDaemon(sockPath);
        if (sAgent2.fileDescriptor < 0) {
            fail("session-grant agent reconnect");
            break;
        }
        if (!requestAgentAttachment(sAgent2, *human3Selection, 106, "session-agent", agentToken)) {
            fail("session-agent pinned reconnect request");
            break;
        }
        if (sAgent2Consent.status(Sync::ConsentStatusPhase::Pending, 600)) {
            fail("allow-session did NOT persist — the re-attach was prompted again (#2)");
            break;
        }
        if (!sAgent2Consent.isGranted(0) || !waitForScreen(sAgent2, 3000)) {
            fail("allow-session re-attach got no checkpoint — the #2 instant failure");
            break;
        }

        // --- Allow-once must not persist across the agent reconnecting ---
        // The mirror image of the allow-session case above. Each agent "observe"
        // is a fresh connect→attach→detach (AgentMCP::attachSession), so a second
        // observe after an allow-once is a NEW attach. Allow-once is a single-attach
        // grant, so that re-attach must be re-prompted: a fresh Pending status for
        // the agent and a fresh exact question for the human. It must not silently
        // admit (allow-once leaking into a standing grant) and must not silently
        // stall on "no checkpoint within 3s" without ever prompting.
        Connection human5;
        ConsentClient human5Consent(human5);
        human5.fileDescriptor = connectAsHuman();
        if (human5.fileDescriptor < 0) {
            fail("human5 connect");
            break;
        }
        const auto human5Selection = createSession(human5, Actor::Human, 5, socketName, humanSelection->daemonInstanceID);
        if (!human5Selection || !waitForScreen(human5, 3000)) {
            fail("human5 authenticated fresh-session Welcome and checkpoint");
            break;
        }
        const auto sessionID5 = human5Selection->sessionID;

        Connection onceAgent;
        ConsentClient onceAgentConsent(onceAgent);
        onceAgent.fileDescriptor = connectDaemon(sockPath);
        if (onceAgent.fileDescriptor < 0) {
            fail("once-agent connect");
            break;
        }
        if (!requestAgentAttachment(onceAgent, *human5Selection, 107, "once-agent", agentToken)
            || !onceAgentConsent.status(Sync::ConsentStatusPhase::Pending)) {
            fail("once-agent Pending");
            break;
        }
        const auto onceQuestion = human5Consent.question(Sync::ConsentKind::Attach);
        if (!onceQuestion || onceQuestion->sessionID != sessionID5) {
            fail("human5 exact once-agent question");
            break;
        }
        if (!human5Consent.respond(*onceQuestion, Sync::ConsentChoice::AllowOnce) || !onceAgentConsent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("once-agent exact approval");
            break;
        }
        if (!waitForScreen(onceAgent, 3000)) {
            fail("once-agent checkpoint after allow-once");
            break;
        }
        // Detach cleanly, then reconnect from the same process/identity to the same
        // session — exactly what a second "observe" tool call does.
        if (!leave(onceAgent)) {
            fail("once-agent normal detach");
            break;
        }
        ::close(onceAgent.fileDescriptor);
        onceAgent.fileDescriptor = -1;

        Connection onceAgent2;
        ConsentClient onceAgent2Consent(onceAgent2);
        onceAgent2.fileDescriptor = connectDaemon(sockPath);
        if (onceAgent2.fileDescriptor < 0) {
            fail("once-agent reconnect");
            break;
        }
        if (!requestAgentAttachment(onceAgent2, *human5Selection, 108, "once-agent", agentToken)) {
            fail("once-agent pinned reconnect request");
            break;
        }
        // The re-attach must be re-prompted (this guards against allow-once leaking).
        if (!onceAgent2Consent.status(Sync::ConsentStatusPhase::Pending)) {
            fail("allow-once re-attach was not re-prompted");
            break;
        }
        // ...and must not have been silently admitted (a leaked standing grant).
        if (waitForScreen(onceAgent2, 400)) {
            fail("allow-once leaked a standing grant — re-attach got a checkpoint without a new prompt");
            break;
        }
        const auto onceQuestion2 = human5Consent.question(Sync::ConsentKind::Attach);
        if (!onceQuestion2 || onceQuestion2->sessionID != sessionID5 || onceQuestion2->requestID == onceQuestion->requestID) {
            fail("allow-once re-attach did not re-prompt the human");
            break;
        }
        // Allow it through so teardown drains cleanly.
        if (!human5Consent.respond(*onceQuestion2, Sync::ConsentChoice::AllowOnce) || !onceAgent2Consent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("once-agent exact fresh approval");
            break;
        }
        if (!waitForScreen(onceAgent2, 3000)) {
            fail("once-agent re-attach checkpoint after re-allow");
            break;
        }

        // Admission is settled before consent is asked. A tokenless agent is
        // refused for the missing token, and a token without Observe for its
        // scope; neither prompts the person, exposes a pane or leaves a grant,
        // so consent never supplies what admission refused.
        // consent_answer_attach separately tests the non-persistence policy for
        // mutable executable identities without fabricating a tokenless attach.
        Connection human4;
        ConsentClient human4Consent(human4);
        human4.fileDescriptor = connectAsHuman();
        if (human4.fileDescriptor < 0) {
            fail("human4 connect");
            break;
        }
        const auto human4Selection = createSession(human4, Actor::Human, 4, socketName, humanSelection->daemonInstanceID);
        if (!human4Selection || !waitForScreen(human4, 3000)) {
            fail("human4 authenticated fresh-session Welcome and checkpoint");
            break;
        }
        Connection tokenlessAgent;
        tokenlessAgent.fileDescriptor = connectDaemon(sockPath);
        if (tokenlessAgent.fileDescriptor < 0) {
            fail("tokenless agent connect");
            break;
        }
        if (!requestAgentAttachment(tokenlessAgent, *human4Selection, 109, "tokenless-agent", "")
            || !waitFor(tokenlessAgent, Sync::FrameKind::Bye, body, 3000)) {
            fail("tokenless attach has a canonical refusal");
            break;
        }
        const auto tokenlessRefusal = Sync::decodeBye(bytes(body));
        if (!tokenlessRefusal || tokenlessRefusal->reason != Sync::ByeReason::TokenMissing || tokenlessAgent.welcome
            || waitForScreen(tokenlessAgent, 0) || human4Consent.question(Sync::ConsentKind::Attach, 100)) {
            fail("tokenless attach must be refused for its missing token without prompting or exposing a pane");
            break;
        }
        Connection noObserveAgent;
        noObserveAgent.fileDescriptor = connectDaemon(sockPath);
        if (noObserveAgent.fileDescriptor < 0) {
            fail("no-observe agent connect");
            break;
        }
        if (!requestAgentAttachment(noObserveAgent, *human4Selection, 112, "no-observe-agent", noObserveToken)
            || !waitFor(noObserveAgent, Sync::FrameKind::Bye, body, 3000)) {
            fail("an attach without Observe has a canonical refusal");
            break;
        }
        const auto noObserveRefusal = Sync::decodeBye(bytes(body));
        if (!noObserveRefusal || noObserveRefusal->reason != Sync::ByeReason::OutsideScope || noObserveAgent.welcome
            || waitForScreen(noObserveAgent, 0) || human4Consent.question(Sync::ConsentKind::Attach, 100)) {
            fail("an attach without Observe must fail admission without prompting or exposing a pane");
            break;
        }
        // No unreachable allow-always response can create a permanent grant.
        std::string consentPath = macHome + "/consent";
        struct stat cst;
        if (::stat(consentPath.c_str(), &cst) == 0 && cst.st_size > 0) {
            fail("a refused attach persisted a grant");
            break;
        }

        // --- StopAlways revoke is scoped to the sender's LIVE
        // presence. A TOKEN-BOUND agent on session B earns a PERSISTED always-grant;
        // a presence human on a DIFFERENT session A then sends StopAlways targeting
        // B's agent. The scoping rejects it (B's agent isn't on A's session), so
        // B's grant SURVIVES — B's agent reconnects auto-admitted, not re-prompted.
        // A global m_alwaysAllowedAgents match (unscoped) would erase it. ---
        Connection humanB;
        ConsentClient humanBConsent(humanB);
        humanB.fileDescriptor = connectAsHuman();
        if (humanB.fileDescriptor < 0) {
            fail("humanB connect");
            break;
        }
        const auto humanBSelection = createSession(humanB, Actor::Human, 6, socketName, humanSelection->daemonInstanceID);
        if (!humanBSelection || !waitForScreen(humanB, 3000)) {
            fail("humanB authenticated fresh-session Welcome and checkpoint");
            break;
        }
        const auto sessionB = humanBSelection->sessionID;
        Connection tokenAgent;
        ConsentClient tokenAgentConsent(tokenAgent);
        tokenAgent.fileDescriptor = connectDaemon(sockPath);
        if (tokenAgent.fileDescriptor < 0) {
            fail("token-agent connect");
            break;
        }
        if (!requestAgentAttachment(tokenAgent, *humanBSelection, 110, "token-agent", agentToken)
            || !tokenAgentConsent.status(Sync::ConsentStatusPhase::Pending)) {
            fail("token-agent Pending");
            break;
        }
        const auto alwaysQuestion = humanBConsent.question(Sync::ConsentKind::Attach);
        if (!alwaysQuestion || alwaysQuestion->sessionID != sessionB) {
            fail("humanB exact token-agent question");
            break;
        }
        if (!humanBConsent.respond(*alwaysQuestion, Sync::ConsentChoice::AllowAlways) || !tokenAgentConsent.status(Sync::ConsentStatusPhase::Approved)) {
            fail("token-agent exact persistent approval");
            break;
        }
        if (!waitForScreen(tokenAgent, 3000)) {
            fail("token-agent checkpoint after allow-always");
            break;
        }
        // Session A: a presence human on a DIFFERENT session.
        Connection humanA;
        ConsentClient humanAConsent(humanA);
        humanA.fileDescriptor = connectAsHuman();
        if (humanA.fileDescriptor < 0) {
            fail("humanA connect");
            break;
        }
        const auto humanASelection = createSession(humanA, Actor::Human, 7, socketName, humanSelection->daemonInstanceID);
        if (!humanASelection || !waitForScreen(humanA, 3000) || humanASelection->sessionID == sessionB) {
            fail("humanA needs a distinct session");
            break;
        }
        const auto sessionA = humanASelection->sessionID;
        // Human A sends forgetAlways keyed by B's verified grant identity:
        // a global m_alwaysAllowedAgents match by identity, no session check. The
        // scoping requires a live agent on A's OWN session to hold that key;
        // B's agent isn't there, so the revoke is rejected and B's grant stands.
        const std::string bGrantKey = "token:" + Ubiquitty::Crypto::sha256Hex(agentToken);
        const auto forgetCall = humanAConsent.revoke({ .sessionID = sessionA, .action = Sync::ConsentRevocationKind::StopAlways, .attachmentID = {}, .identity = bGrantKey });
        if (!forgetCall) {
            fail("cross-session forgetAlways encoding");
            break;
        }
        const auto forgetResult = humanAConsent.result(*forgetCall);
        const auto* rejection = forgetResult ? forgetResult->find("error") : nullptr;
        if (!rejection || rejection->numberOr("code") != std::to_underlying(Ubiquitty::Protocol::Control::ErrorCode::NotFound)) {
            fail("cross-session forgetAlways must return its exact correlated NotFound refusal");
            break;
        }
        // The grant must SURVIVE: B's agent disconnects + reconnects with the same
        // token and is admitted by a Granted Welcome without a new question.
        ::close(tokenAgent.fileDescriptor);
        ::usleep(250 * 1000); // let the daemon process the StopAlways + the disconnect
        Connection tokenAgent2;
        ConsentClient tokenAgent2Consent(tokenAgent2);
        tokenAgent2.fileDescriptor = connectDaemon(sockPath);
        if (tokenAgent2.fileDescriptor < 0) {
            fail("token-agent reconnect");
            break;
        }
        if (!requestAgentAttachment(tokenAgent2, *humanBSelection, 111, "token-agent", agentToken)) {
            fail("token-agent pinned reconnect request");
            break;
        }
        if (tokenAgent2Consent.status(Sync::ConsentStatusPhase::Pending, 1500)) {
            fail("cross-session StopAlways wrongly revoked B's always-grant (cross-session scoping broken)");
            break;
        }
        if (!tokenAgent2Consent.isGranted(0) || !waitForScreen(tokenAgent2, 3000)) {
            fail("B's agent not auto-admitted after the cross-session StopAlways (grant should survive)");
            break;
        }

        rc = 0;
    } while (false);

    if (rc == 0)
        std::printf("consent_gate: agent attach HELD → allow-once delivers the checkpoint, "
                    "deny delivers exact Denied status; allow-once does NOT persist — a "
                    "re-attach is re-prompted, never silently admitted nor silently "
                    "stalled; terminating a human session is "
                    "gated too (action=terminate prompt, deny spares it, allow tears "
                    "it down); a presence-aware human sees the roster grow on attach + "
                    "shrink on exact attachment revocation; a tokenless attach is refused "
                    "for its missing token, a token without Observe cannot borrow it "
                    "from consent, and cross-session forgetAlways cannot revoke a "
                    "verified principal's standing grant\n");

    for (pid_t bridgeProcessID : bridgeProcessIDs)
        ::kill(bridgeProcessID, SIGKILL);
    for (pid_t bridgeProcessID : bridgeProcessIDs)
        ::waitpid(bridgeProcessID, nullptr, 0);

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
