// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/remote_clear_e2e.cpp — the four clear commands in a host session,
// end to end, with the client stack the macOS app attaches through: a
// Session::WorkspaceConnection carrying a pane RemoteSessionCore over
// `ubiquitty-host --attach` to a real forked daemon, beside a second window
// attached to the same session.
//
// The host owns the session's terminal, so a clear on a thin client is a request
// the host performs, exactly as a local session performs it. Its result reaches
// every window as ordinary Sync frames. A command that drops history drops it
// from every window, because a cleared history is a privacy action every
// attached copy honours; a command that drops none leaves every copy whole.
//
//   Clear Screen     the prompt rises to the top; history grows, nothing goes.
//   Clear Scrollback the history goes from both windows; the screen stands.
//   Clear to Mark    the last executed command's block goes from both windows;
//                    the history above it stays.
//   Clear Buffer     screen and history both go; the shell redraws its prompt.
//
// A clear changes the screen a person is looking at, so it is the Drive
// scope's, as typing is: an attachment holding only Observe is refused for
// every command and the history stays.
//
// Exit 0 iff every assertion held. argv[1] = the built ubiquitty-host.
//
#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/WorkspaceConnection.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/ConsentClient.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Ubiquitty;

namespace {

int failureCount = 0;

/// A token whose scope observes and nothing else: no Drive.
constexpr std::string_view observeToken = "remote-clear-observe-token-0123456789";

void check(bool ok, const char* what) // bool-ok: an assertion's outcome, as every Host smoke spells it
{
    if (!ok) {
        std::fprintf(stderr, "remote_clear_e2e: FAIL: %s\n", what);
        ++failureCount;
    }
}

std::unique_ptr<Ubiquitty::Terminal> makeTerm()
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    return t;
}

std::string gridText(Ubiquitty::Terminal& t)
{
    std::string out;
    for (int r = 0; r < t.rows; ++r) {
        for (int c = 0; c < t.columns; ++c) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            uint32_t ch = (cell && cell->codePoint) ? cell->codePoint : ' ';
            if (ch < 0x20 || ch > 0x7E)
                ch = ' ';
            out.push_back(static_cast<char>(ch));
        }
        out.push_back('\n');
    }
    return out;
}

/// Every row the terminal holds, history first, as text.
std::string allText(Ubiquitty::Terminal& t)
{
    std::string out;
    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    for (int row = 0; row < history + t.rows; ++row) {
        // The view offset that puts this absolute row on visible row 0, or on
        // its own visible row once the live screen is reached.
        const int offset = std::max(0, history - row);
        const int visibleRow = row - (history - offset);
        for (int c = 0; c < t.columns; ++c) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&t, offset, visibleRow, c);
            uint32_t ch = (cell && cell->codePoint) ? cell->codePoint : ' ';
            if (ch < 0x20 || ch > 0x7E)
                ch = ' ';
            out.push_back(static_cast<char>(ch));
        }
        out.push_back('\n');
    }
    return out;
}

bool holds(Ubiquitty::Terminal& t, std::string_view text) { return allText(t).find(text) != std::string::npos; }

/// The posted work every window's core leaves for the main thread; the test
/// is that thread.
struct WorkQueue {
    struct Posted {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
    };

    std::mutex mutex;
    std::vector<Posted> queue;

    /// Queues work for a moment; a delay is honoured rather than fired at
    /// once, since the core arms timers for moments that have not come.
    void post(Session::PostedWork work, double delaySeconds = 0)
    {
        const std::scoped_lock lock(mutex);
        queue.push_back({ .due = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(delaySeconds * 1'000'000.0)), .work = std::move(work) });
    }
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::scoped_lock lock(mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto due = std::ranges::find_if(queue, [&](const Posted& posted) { return posted.due <= now; });
                if (due == queue.end())
                    return;
                work = std::move(due->work);
                queue.erase(due);
            }
            std::move(work)();
        }
    }
};

class QueueDispatcher final : public Session::Dispatcher {
public:
    explicit QueueDispatcher(std::shared_ptr<WorkQueue> queue)
        : m_queue(std::move(queue))
    {
    }
    void post(Session::PostedWork work) override { m_queue->post(std::move(work)); }
    void postDelayed(double seconds, Session::PostedWork work) override { m_queue->post(std::move(work), seconds); }

private:
    std::shared_ptr<WorkQueue> m_queue;
};

/// The channel the macOS factory builds: `ubiquitty-host --attach`, which finds
/// the daemon socket through the inherited UBIQUITTY_HOME.
class AttachChannelFactory final : public Session::ChannelFactory {
public:
    AttachChannelFactory(std::string command, std::string socketName)
        : m_command(std::move(command))
        , m_socketName(std::move(socketName))
    {
    }
    void createChannel(Session::TransportMode, Done done) override
    {
        done(std::make_unique<Transport::SubprocessChannel>(m_command, Transport::Origin { Transport::OriginKind::Daemon, "" }), false);
    }
    bool pinDaemonSocket(std::string_view socketName) override { return !m_socketName.empty() && m_socketName == socketName; }

private:
    std::string m_command;
    std::string m_socketName;
};

class RecordingObserver final : public Session::RemoteSessionObserver {
public:
    void onScreenChanged() override { ++m_screens; }
    void onConsentRequest(const Protocol::Sync::ConsentQuestion& request) override
    {
        m_consentRequestID = request.requestID;
        m_consentKind = static_cast<Protocol::Sync::ConsentKind>(request.action);
        m_consentSessionID = request.sessionID;
    }
    int m_screens = 0;
    uint32_t m_consentRequestID = 0;
    uint32_t m_consentSessionID = 0;
    Protocol::Sync::ConsentKind m_consentKind = Protocol::Sync::ConsentKind::Attach;
};

template<class Pred> bool pumpUntil(WorkQueue& queue, int timeoutMilliseconds, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        queue.pump();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    queue.pump();
    return pred();
}

/// An agent attachment holding only Observe, let in by the person at the
/// session, sends the clear a Drive client would.
///
/// - Parameters:
///   - socketPath: The daemon's socket.
///   - sessionID: The session it attaches to.
///   - daemonInstanceID: The real owner retained from the native creation receipt.
///   - command: The clear it asks for.
///   - admit: Pumps the windows until one is asked for consent and allows it.
/// - Returns: Whether the host refused the clear for scope.
bool isClearRefusedWithoutDrive(const std::string& socketPath, uint32_t sessionID, std::uint64_t daemonInstanceID, Ubiquitty::Screen::ClearCommand command,
    const std::function<bool(const Protocol::Sync::ConsentBinding&)>& admit)
{
    using namespace Ubiquitty::HostTest;
    if (!sessionID || !daemonInstanceID)
        return false;
    Connection observer;
    observer.fileDescriptor = connectDaemon(socketPath);
    if (observer.fileDescriptor < 0)
        return false;
    Protocol::Sync::Greet greeting;
    greeting.wireVersion = Protocol::Sync::wireVersion;
    greeting.appVersion = Protocol::Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(observer.fileDescriptor) + 1, .high = 0x636c656172 };
    greeting.clientKind = Protocol::Sync::ClientKind::Agent;
    greeting.requestedRole = Protocol::Sync::Role::Agent;
    greeting.request = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = daemonInstanceID, .agentLabel = "clear-observer", .isAttachOnly = true };
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.credential.emplace(observeToken.begin(), observeToken.end());
    if (!sendGreeting(observer, std::move(greeting))) {
        ::close(observer.fileDescriptor);
        return false;
    }
    ConsentClient control(observer);
    const auto pending = control.status(Protocol::Sync::ConsentStatusPhase::Pending, 3000);
    const bool hasExactPending = pending && pending->question.sessionID == sessionID && !control.isGranted(0)
        && !waitForScreen(observer, 0);
    check(hasExactPending, "the Observe-only agent waits under the exact session's Pending status without a screen");
    const bool didAdmit = hasExactPending && admit(pending->question);
    check(didAdmit, "the person at the session was asked to admit the observer, and did");
    const auto approved = didAdmit ? control.status(Protocol::Sync::ConsentStatusPhase::Approved, 5000) : std::nullopt;
    const bool wasApproved = pending && approved && approved->question == pending->question && control.isGranted(0);
    check(wasApproved, "the exact pending question is approved before pane admission");
    if (!wasApproved || !waitForScreen(observer, 5000)) {
        check(false, "the admitted observer is sent the screen");
        ::close(observer.fileDescriptor);
        return false;
    }
    constexpr std::array names { "buffer", "scrollback", "screen", "previous-mark" };
    const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":\"clear\",\"method\":\"pane.clear\",\"params\":{\"pane\":"
        + std::to_string(observer.paneHandle) + ",\"command\":\"" + names[std::to_underlying(command)] + "\"}}";
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, Protocol::Sync::Call { .text = request });
    const bool didSend = sendFrame(observer, Protocol::Sync::FrameKind::Call,
        { reinterpret_cast<const char*>(body.data()), body.size() });
    const auto reply = didSend ? control.result("clear", 3000) : std::nullopt;
    const auto* refusal = reply ? reply->find("error") : nullptr;
    const bool isRefused = reply && reply->stringOr("id") == "clear" && refusal
        && refusal->numberOr("code") == static_cast<int>(Protocol::Control::ErrorCode::Forbidden) && !observer.hasFailed;
    ::close(observer.fileDescriptor);
    return isRefused;
}

void exercise(const std::string& attachCommand, const std::string& socketPath, const std::string& socketName)
{
    auto queue = std::make_shared<WorkQueue>();

    // ---- The clearing window: a workspace pane, as TerminalSession attaches one. ----
    auto clearingTerm = makeTerm();
    RecordingObserver clearingObserver;
    Session::WorkspaceConnection workspace(std::make_unique<QueueDispatcher>(queue), std::make_unique<AttachChannelFactory>(attachCommand, socketName));
    Session::RemoteSessionCore clearing(clearingTerm.get(), workspace, &clearingObserver);
    clearing.start(0, 80, 24);
    check(pumpUntil(*queue, 5000, [&] { return clearing.acknowledged() && clearingObserver.m_screens > 0; }), "the clearing window attached and applied a checkpoint");
    const auto daemonInstanceID = workspace.connection().daemonInstanceID();
    check(daemonInstanceID && workspace.connection().socketName() == socketName,
        "the tokenless Human workspace's creation receipt selects the fixture daemon");
    if (!clearing.acknowledged() || !daemonInstanceID || workspace.connection().socketName() != socketName)
        return;
    const uint32_t sessionID = clearing.sessionID();

    // History the clear has to take: far more lines than the screen holds.
    clearing.sendInput("seq 1 300; echo FILLED_\"\"MARKER\r");
    check(pumpUntil(*queue, 10000, [&] { return gridText(*clearingTerm).find("FILLED_MARKER") != std::string::npos; }), "the shell's output reached the clearing window");
    check(pumpUntil(*queue, 5000, [&] { return Ubiquitty::Screen::scrollbackCount(clearingTerm.get()) > 200; }), "the clearing window holds the history the output scrolled");

    // ---- A second window on the same session, with its own copy of the history. ----
    auto watchingTerm = makeTerm();
    AttachChannelFactory watchingFactory(attachCommand, socketName);
    RecordingObserver watchingObserver;
    QueueDispatcher watchingDispatcher(queue);
    Session::RemoteSessionCore watching(watchingTerm.get(), &watchingDispatcher, &watchingFactory, &watchingObserver);
    watching.setExpectedDaemonInstanceID(daemonInstanceID);
    watching.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
    watching.start(sessionID, 80, 24);
    check(pumpUntil(*queue, 5000, [&] { return watching.acknowledged() && watchingObserver.m_screens > 0; }), "the second window attached to the same session");
    check(watching.sessionID() == sessionID && watching.daemonInstanceID() == daemonInstanceID && watching.socketName() == socketName,
        "the second window preserves the exact session, daemon and socket");
    check(pumpUntil(*queue, 5000, [&] { return Ubiquitty::Screen::scrollbackCount(watchingTerm.get()) > 200; }), "the second window holds the same history");

    using Ubiquitty::Screen::ClearCommand;
    constexpr ClearCommand everyCommand[] = { ClearCommand::Buffer, ClearCommand::Scrollback, ClearCommand::Screen, ClearCommand::ToPreviousMark };

    // ---- An attachment without the Drive scope cannot clear. ----
    const int historyBefore = Ubiquitty::Screen::scrollbackCount(clearingTerm.get());
    for (const ClearCommand command : everyCommand) {
        watchingObserver.m_consentRequestID = 0;
        clearingObserver.m_consentRequestID = 0;
        watchingObserver.m_consentSessionID = 0;
        clearingObserver.m_consentSessionID = 0;
        const auto admit = [&](const Protocol::Sync::ConsentBinding& pending) {
            const auto matches = [&](const RecordingObserver& observer) {
                return observer.m_consentRequestID == pending.requestID && observer.m_consentSessionID == pending.sessionID
                    && observer.m_consentKind == Protocol::Sync::ConsentKind::Attach;
            };
            if (!pumpUntil(*queue, 5000, [&] { return matches(watchingObserver) || matches(clearingObserver); }))
                return false;
            const auto allowOnce = std::to_underlying(Protocol::Sync::ConsentChoice::AllowOnce);
            if (matches(watchingObserver)) {
                watching.respondToConsent(watchingObserver.m_consentRequestID, watchingObserver.m_consentKind, allowOnce);
            } else {
                clearing.respondToConsent(clearingObserver.m_consentRequestID, clearingObserver.m_consentKind, allowOnce);
            }
            queue->pump();
            return true;
        };
        check(isClearRefusedWithoutDrive(socketPath, sessionID, daemonInstanceID, command, admit), "an Observe-only attachment's clear is refused for scope");
    }
    // The wait outlasts the frame a performed clear would produce.
    (void)pumpUntil(*queue, 500, [] { return false; });
    check(Ubiquitty::Screen::scrollbackCount(clearingTerm.get()) >= historyBefore, "the refused clears left the history");
    check(gridText(*clearingTerm).find("FILLED_MARKER") != std::string::npos, "the refused clears left the screen");

    const auto historyOf = [](const std::unique_ptr<Ubiquitty::Terminal>& term) { return Ubiquitty::Screen::scrollbackCount(term.get()); };

    // ---- Clear Screen: the prompt rises to the top and nothing is destroyed. ----
    const int beforeScreen = historyOf(clearingTerm);
    check(clearing.sendClearBuffer(ClearCommand::Screen), "Clear Screen was sent to the host");
    check(pumpUntil(*queue, 5000, [&] { return gridText(*clearingTerm).find("FILLED_MARKER") == std::string::npos; }), "Clear Screen lifted the output off the clearing window's screen");
    check(historyOf(clearingTerm) > beforeScreen && holds(*clearingTerm, "FILLED_MARKER"), "Clear Screen moved the rows into the clearing window's history");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(watchingTerm) == historyOf(clearingTerm); }), "the second window's history grew with the host's");
    check(holds(*watchingTerm, "FILLED_MARKER"), "and the second window kept the rows");

    // ---- Clear Scrollback: the history goes everywhere; the screen stands. ----
    const std::string screenBeforeScrollback = gridText(*clearingTerm);
    check(clearing.sendClearBuffer(ClearCommand::Scrollback), "Clear Scrollback was sent to the host");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(clearingTerm) == 0; }), "Clear Scrollback emptied the clearing window's history");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(watchingTerm) == 0; }), "Clear Scrollback purged the second window's history");
    check(!holds(*watchingTerm, "FILLED_MARKER"), "and nothing of it is left in the second window");
    check(gridText(*clearingTerm) == screenBeforeScrollback, "Clear Scrollback left the screen as it was");

    // ---- Clear to Previous Mark: the last executed block goes, history above stays. ----
    // The block is marked by the command itself, so the test does not depend
    // on the shell's own integration: a prompt (A), its input (B), the start
    // of output (C) that makes it an executed command, the output, and its end.
    clearing.sendInput("seq 1 60; echo EARLIER_\"\"MARKER\r");
    check(pumpUntil(*queue, 10000, [&] { return gridText(*clearingTerm).find("EARLIER_MARKER") != std::string::npos; }), "the earlier output reached the clearing window");
    clearing.sendInput("printf '\\033]133;A\\007$ \\033]133;B\\007marked\\n\\033]133;C\\007BLOCK_%s\\nBLOCK_%s\\n\\033]133;D;0\\007' OUTPUT OUTPUT\r");
    check(pumpUntil(*queue, 10000, [&] { return gridText(*clearingTerm).find("BLOCK_OUTPUT") != std::string::npos; }), "the marked block reached the clearing window");
    check(pumpUntil(*queue, 5000, [&] { return holds(*watchingTerm, "BLOCK_OUTPUT"); }), "and the second window");
    check(clearing.sendClearBuffer(ClearCommand::ToPreviousMark), "Clear to Previous Mark was sent to the host");
    check(pumpUntil(*queue, 5000, [&] { return !holds(*clearingTerm, "BLOCK_OUTPUT"); }), "Clear to Previous Mark removed the block from the clearing window");
    check(pumpUntil(*queue, 5000, [&] { return !holds(*watchingTerm, "BLOCK_OUTPUT"); }), "and from the second window, history included");
    check(holds(*clearingTerm, "EARLIER_MARKER") && holds(*watchingTerm, "EARLIER_MARKER"), "the output above the block stayed in both windows");

    // ---- Clear Buffer (Cmd-K): screen and history go everywhere. ----
    clearing.sendInput("seq 1 300; echo FILLED_\"\"AGAIN\r");
    check(pumpUntil(*queue, 10000, [&] { return gridText(*clearingTerm).find("FILLED_AGAIN") != std::string::npos && historyOf(clearingTerm) > 200; }), "the clearing window holds history again");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(watchingTerm) > 200; }), "and so does the second window");
    check(clearing.sendClearBuffer(ClearCommand::Buffer), "Clear Buffer was sent to the host");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(clearingTerm) == 0; }), "the clearing window's history is gone");
    check(pumpUntil(*queue, 5000, [&] { return !holds(*clearingTerm, "FILLED_AGAIN") && !holds(*clearingTerm, "EARLIER_MARKER"); }), "the clearing window's screen is cleared");
    check(pumpUntil(*queue, 5000, [&] { return historyOf(watchingTerm) == 0; }), "the second window's history was purged with the host's");
    check(pumpUntil(*queue, 5000, [&] { return !holds(*watchingTerm, "FILLED_AGAIN") && !holds(*watchingTerm, "EARLIER_MARKER"); }), "the second window's screen is the cleared one");

    // The session is still the one typed into: the clears kept its prompt alive.
    clearing.sendInput("echo AFTER_\"\"CLEAR\r");
    check(pumpUntil(*queue, 5000, [&] { return gridText(*clearingTerm).find("AFTER_CLEAR") != std::string::npos; }), "the cleared session still answers input");
    check(pumpUntil(*queue, 5000, [&] { return gridText(*watchingTerm).find("AFTER_CLEAR") != std::string::npos; }), "and the second window sees it");

    watching.stop();
    clearing.stop();
    queue->pump();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBin = argv[1];

    // An isolated UBIQUITTY_HOME, inherited by the daemon and every attach child.
    char homeTemplate[] = "/tmp/mt-remote-clear.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    ::mkdir(macHome.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(hostBin.c_str());
    const std::string socketPath = macHome + "/" + socketName;
    {
        const std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 observe\n", Ubiquitty::Crypto::sha256Hex(std::string(observeToken)).c_str(),
            static_cast<unsigned>(std::to_underlying(Protocol::Sync::Scope::Observe)));
        std::fclose(file);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        char* args[] = { const_cast<char*>(hostBin.c_str()), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBin.c_str(), args);
        ::_exit(127);
    }
    check(Ubiquitty::HostTest::waitForDaemonReady(socketPath), "daemon accepted a connection");

    exercise(Transport::shellSingleQuote(hostBin) + " --attach --socket " + Transport::shellSingleQuote(socketName), socketPath, socketName);

    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 200; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, 0);

    if (failureCount == 0)
        std::printf("remote_clear_e2e: every clear command runs on the host and reaches every window\n");
    return failureCount == 0 ? 0 : 1;
}
