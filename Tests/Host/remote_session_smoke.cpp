// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/remote_session_smoke.cpp — the daemon's remote-session path, end to
// end, headless. Drives the REAL portable Session::RemoteSessionCore over a REAL
// Transport::SubprocessChannel (the live macOS backend) bridged to a REAL
// ubiquitty-host daemon by `ubiquitty-host --attach` — i.e. the exact client stack
// the macOS app uses. The --ssh mode crosses an isolated OpenSSH server and
// exercises the bootstrap, Core deployment, directory, and session reattachment.
//
// Asserts the things the headless unit tests (fakes) can't: that the actual wire
// path works — attach → Granted Welcome → a checkpoint into the passive Terminal, input reaches
// the shell and its output comes back in a later Update, the session persists a
// clean detach, and a fresh core REATTACHES the same session id and still sees the
// marker. argv[1] = the built ubiquitty-host.
//
// Exit 0 iff: connected + a checkpoint applied; the typed marker appears; reattach
// to the same id still shows it.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Admission.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/HostInstall.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SubprocessChannel.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Ubiquitty;

namespace Ubiquitty::Runtime {
struct HostDeployTestAccess {
    /// Installs `binary` through `prefix` exactly as an update does: the
    /// digest of the bytes sent, the install script, the verdict it prints.
    static Session::HostInstallOutcome install(const std::string& prefix, const std::string& binary, Session::HostInstallMode mode)
    {
        std::ifstream source(binary, std::ios::binary);
        const std::string bytes { std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>() };
        const std::string script = Session::hostInstallCommand(Crypto::sha256Hex(bytes), mode);
        const TerminalSession::RemoteCommandRun run = TerminalSession::runRemoteCommand(prefix + " " + Transport::shellSingleQuote(script) + " 2>&1", bytes);
        if (!run.failure.empty()) {
            Session::HostInstallOutcome failed;
            failed.reason = run.failure;
            return failed;
        }
        return Session::readHostInstallOutput(run.output, Session::deployFailureDetail(run.output));
    }

    /// What the update's probe answers through `prefix`.
    static std::optional<Session::HostProbe> probe(const std::string& prefix)
    {
        const TerminalSession::RemoteCommandRun run = TerminalSession::runRemoteCommand(prefix + " " + Transport::shellSingleQuote(Session::hostProbeCommand()) + " 2>&1", {});
        return Session::parseHostProbe(run.output);
    }
};
}

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "remote_session_smoke: FAIL: %s\n", what);
        ++g_failures;
    }
}

std::unique_ptr<Ubiquitty::Terminal> makeTerm()
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    return t;
}

// The visible grid as a flat string (matches smoke_client's gridText). A
// soft-wrapped row runs on into the next, so text typed past the margin, such
// as a landed path under a long temporary directory, is found whole.
std::string gridText(Ubiquitty::Terminal& t)
{
    std::string out;
    for (int r = 0; r < t.rows; ++r) {
        for (int c = 0; c < t.columns; ++c) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            uint32_t ch = (cell && cell->codePoint) ? cell->codePoint : ' ';
            if (ch < 0x20 || ch > 0x7E)
                ch = (ch == 0) ? ' ' : '?';
            out.push_back(static_cast<char>(ch));
        }
        if (!t.screens.active->lineWrapped[r])
            out.push_back('\n');
    }
    return out;
}

// Thread-safe main-thread stand-in: the channel's I/O thread post()s; the test
// pumps on its own thread. (postDelayed fires immediately — the smoke induces
// no drops, so reconnect backoff never matters here.)
class TestDispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }
    // A delay is honoured rather than fired at once: the core arms timers for
    // moments that have not come, and one run early is armed again at once.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000.0)), std::move(work));
    }
    // Runs every piece of work whose time has come, in the order it was posted.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto due = std::ranges::find_if(m_queue, [&](const Posted& posted) { return posted.due <= now; });
                if (due == m_queue.end())
                    return;
                work = std::move(due->work);
                m_queue.erase(due);
            }
            std::move(work)();
        }
    }

private:
    struct Posted {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
    };

    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({ .due = due, .work = std::move(work) });
    }

    std::mutex m_mutex;
    std::vector<Posted> m_queue;
};

// Builds the native bridge and pins it to the daemon selected for this test.
// An SSH bootstrap is initially unpinned; its first verified receipt rewrites
// the production SOCK token before any later connection is constructed.
class AttachChannelFactory final : public Session::ChannelFactory {
public:
    explicit AttachChannelFactory(std::string command, std::string selectedSocket = {})
        : m_command(std::move(command))
        , m_selectedSocket(std::move(selectedSocket))
    {
    }
    void createChannel(Session::TransportMode, Done done) override { done(std::make_unique<Transport::SubprocessChannel>(m_command, Transport::Origin { Transport::OriginKind::Daemon, "" }), false); }
    bool pinDaemonSocket(std::string_view socketName) override
    {
        if (socketName.empty() || socketName.size() > Protocol::Sync::maxDaemonSocketNameBytes
            || socketName.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string_view::npos)
            return false;
        if (!m_selectedSocket.empty())
            return m_selectedSocket == socketName;
        const auto pinned = Session::substituteReattachSocket(m_command, socketName);
        if (pinned == m_command)
            return false;
        m_command = pinned;
        m_selectedSocket = socketName;
        return true;
    }

private:
    std::string m_command;
    std::string m_selectedSocket;
};

class RecordingObserver final : public Session::RemoteSessionObserver {
public:
    void onStateChanged(Session::State state, const std::string&) override { m_state = state; }
    void onScreenChanged() override { ++m_screens; }
    void onPastePushAck(const Protocol::Control::PastePushReply& ack) override
    {
        m_pastePushAcks++;
        m_lastPastePushStatus = ack.status;
        m_lastPastePushPath = ack.path;
    }
    void onExit() override { m_exited = true; }
    void onDeployRequested(const std::string& platform) override { m_deployPlatform = platform; }
    std::string m_deployPlatform;
    Session::State m_state = Session::State::Idle;
    int m_screens = 0;
    int m_pastePushAcks = 0;
    Protocol::Control::PastePushStatus m_lastPastePushStatus = Protocol::Control::PastePushStatus::IOError;
    std::string m_lastPastePushPath;
    bool m_exited = false;
};

// Pump the dispatcher until `pred` holds or `timeoutMs` elapses.
template<class Pred> bool pumpUntil(TestDispatcher& dispatcher, int timeoutMs, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    dispatcher.pump();
    return pred();
}

} // namespace

static int runSSHBootstrap(const std::string& prefix, const std::string& hostBinary)
{
    unsigned failureCount = 0;
    const auto check = [&failureCount](bool passed, const char* message) {
        if (!passed) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failureCount;
        }
    };
    TestDispatcher dispatcher;
    const auto command = prefix + " " + Transport::shellSingleQuote(Session::remoteBootstrapCommand());
    AttachChannelFactory factory(command);
    struct DirectoryObserver final : Session::SessionDirectoryObserver {
        std::string failure;
        Session::ListingFailure kind { Session::ListingFailure::None };
        std::string platform;
        bool hasListed { false };
        void onDirectoryFailed(const Session::DirectoryFailureReport& report) override
        {
            failure = Session::listingFailureNotice(report, Session::NoticePlacement::Embedded);
            kind = report.failure;
            platform = report.remotePlatform;
        }
        void onSessionList(const std::vector<Ubiquitty::Session::SessionRow>&) override { hasListed = true; }
    } directoryObserver;
    Session::SessionDirectoryCore directory(&dispatcher, &factory, &directoryObserver);
    check(directory.refresh(), "SSH: directory refresh starts");
    check(pumpUntil(dispatcher, 10000, [&] { return !directoryObserver.failure.empty(); }), "SSH: fresh-host directory finishes");
    check(directoryObserver.kind == Session::ListingFailure::NotInstalled && !directoryObserver.platform.empty(),
        "SSH: directory reports a host with nothing installed, and its platform");

    auto term = makeTerm();
    RecordingObserver observer;
    Session::RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);
    core.start(0, 80, 24);
    check(pumpUntil(dispatcher, 10000, [&] { return !observer.m_deployPlatform.empty(); }), "SSH: noisy startup reaches installation");
    check(core.state() == Session::State::Deploying, "SSH: deploy does not reconnect before installation");
    const auto freshProbe = Runtime::HostDeployTestAccess::probe(prefix);
    check(freshProbe && freshProbe->liveSessionCount == 0 && freshProbe->platform == directoryObserver.platform,
        "SSH: the update probe finds no sessions and the platform the directory reported");
    const auto installed = Runtime::HostDeployTestAccess::install(prefix, hostBinary, Session::HostInstallMode::IfIdle);
    check(installed.kind == Session::HostInstallOutcomeKind::Installed, "SSH: Core deployment verifies and installs the complete binary");
    if (installed.kind != Session::HostInstallOutcomeKind::Installed) {
        std::fprintf(stderr, "SSH deployment: %s\n", installed.reason.c_str());
        return 1;
    }
    core.deployFinished(Session::DeployResult::Succeeded, {});
    check(pumpUntil(dispatcher, 10000, [&] { return core.acknowledged() && observer.m_screens; }), "SSH: bootstrap attaches after deployment");
    const std::string expectedSocketName = Host::socketLeafForBinary(hostBinary.c_str());
    check(core.sessionID() && core.daemonInstanceID() && core.socketName() == expectedSocketName,
        "SSH: the verified deployment's receipt selects the expected binary socket and real daemon");
    if (!core.acknowledged() || !core.sessionID() || !core.daemonInstanceID() || core.socketName() != expectedSocketName)
        return 1;
    const auto identity = core.sessionID();
    const auto daemonInstanceID = core.daemonInstanceID();
    const auto socketName = core.socketName();
    core.sendInput("printf 'SSH_MARKER_%s\\n' 42\r");
    check(pumpUntil(dispatcher, 10000, [&] { return gridText(*term).find("SSH_MARKER_42") != std::string::npos; }), "SSH: shell output crosses the real hop");
    core.stop();
    RecordingObserver reattachedObserver;
    Session::RemoteSessionCore reattached(term.get(), &dispatcher, &factory, &reattachedObserver);
    reattached.setExpectedDaemonInstanceID(daemonInstanceID);
    reattached.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
    reattached.start(identity, 80, 24);
    check(pumpUntil(dispatcher, 10000, [&] { return reattached.acknowledged() && reattachedObserver.m_screens; }), "SSH: detached session reattaches");
    check(reattached.sessionID() == identity && gridText(*term).find("SSH_MARKER_42") != std::string::npos, "SSH: reattach preserves identity and output");
    check(reattached.daemonInstanceID() == daemonInstanceID && reattached.socketName() == socketName,
        "SSH: reattach preserves the selected daemon and socket");
    reattached.stop();

    // An update against a daemon holding a session: the probe counts it, an
    // install the probe found idle refuses rather than end it, and one told to
    // wait leaves the new host beside the running one. The session survives
    // both, which a third attach proves.
    const auto busyProbe = Runtime::HostDeployTestAccess::probe(prefix);
    check(busyProbe && busyProbe->liveSessionCount >= 1, "SSH: the update probe counts the live session");
    const auto refused = Runtime::HostDeployTestAccess::install(prefix, hostBinary, Session::HostInstallMode::IfIdle);
    check(refused.kind == Session::HostInstallOutcomeKind::Failed && refused.reason.find("sessions started") != std::string::npos,
        "SSH: an idle-only install refuses a host with a live session");
    const auto deferred = Runtime::HostDeployTestAccess::install(prefix, hostBinary, Session::HostInstallMode::WhenSessionsEnd);
    check(deferred.kind == Session::HostInstallOutcomeKind::Deferred && deferred.waitingSessionCount >= 1,
        "SSH: an install told to wait keeps the new host beside the running one");
    RecordingObserver survivorObserver;
    Session::RemoteSessionCore survivor(term.get(), &dispatcher, &factory, &survivorObserver);
    survivor.setExpectedDaemonInstanceID(daemonInstanceID);
    survivor.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
    survivor.start(identity, 80, 24);
    check(pumpUntil(dispatcher, 10000, [&] { return survivor.acknowledged() && survivorObserver.m_screens; }) && survivor.sessionID() == identity,
        "SSH: the session outlives both updates");
    check(survivor.daemonInstanceID() == daemonInstanceID && survivor.socketName() == socketName,
        "SSH: both updates preserve the original serving owner");
    survivor.stop();
    directoryObserver.failure.clear();
    check(directory.refresh(), "SSH: installed-host directory refresh starts");
    check(pumpUntil(dispatcher, 10000, [&] { return directoryObserver.hasListed; }), "SSH: installed-host directory crosses noisy startup");
    std::printf("SSH bootstrap: %s\n", failureCount ? "FAIL" : "PASS");
    return failureCount ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--ssh") {
        return runSSHBootstrap(argv[2], argv[3]);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const std::string hostBin = argv[1];

    // Isolated UBIQUITTY_HOME so the daemon + the attach children never touch the
    // developer's real ~/.ubiquitty. Set in our env so the attach child inherits it.
    char homeTemplate[] = "/tmp/mt-remote-smoke.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    ::mkdir(macHome.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);

    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(hostBin.c_str());
    const std::string socketPath = macHome + "/" + socketName;

    // Spawn the real daemon (tokenless local attach — no UBIQUITTY_REQUIRE_TOKEN).
    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        char* args[] = { const_cast<char*>(hostBin.c_str()), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBin.c_str(), args);
        ::_exit(127);
    }
    bool isDaemonReady = Ubiquitty::HostTest::waitForDaemonReady(socketPath);
    check(isDaemonReady, "daemon accepted a connection");

    const std::string attachCommand = Transport::shellSingleQuote(hostBin) + " --attach --socket " + Transport::shellSingleQuote(socketName);
    const std::string marker = "SMOKE_MARKER_42";
    uint32_t pinnedSessionID = 0;
    std::uint64_t pinnedDaemonInstanceID = 0;
    std::string pinnedSocketName;

    // ---- Session A: fresh attach, type a marker, see it, clean stop. ----
    {
        auto term = makeTerm();
        TestDispatcher dispatcher;
        AttachChannelFactory factory(attachCommand, socketName);
        RecordingObserver observer;
        Session::RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

        core.start(0, 80, 24);
        check(pumpUntil(dispatcher, 5000, [&] { return core.acknowledged() && observer.m_screens > 0; }), "A: connected + first checkpoint applied");
        check(core.state() == Session::State::Connected, "A: state Connected");
        check(core.sessionID() != 0, "A: session id pinned");
        check(core.daemonInstanceID() != 0 && core.socketName() == socketName,
            "A: the native creation receipt names the actual fixture socket and daemon");
        if (core.acknowledged() && core.sessionID() && core.daemonInstanceID() && core.socketName() == socketName) {
            pinnedSessionID = core.sessionID();
            pinnedDaemonInstanceID = core.daemonInstanceID();
            pinnedSocketName = core.socketName();
        }

        // Type a marker; the shell echoes/runs it and the output returns in an Update.
        const std::string input = "echo " + marker + "\r";
        core.sendInput(input);
        check(pumpUntil(dispatcher, 5000, [&] { return gridText(*term).find(marker) != std::string::npos; }), "A: typed marker reached the screen over the real wire");

        // ---- A pasted image crosses to the host and its path is typed. ----
        //
        // The whole feature over the real wire: the client frames the payload,
        // the daemon lands it as a file whose extension names the type, and
        // types the path into this session's PTY — which is the only proof
        // that what a user pastes reaches the program they are talking to.
        const std::vector<std::uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        const std::uint32_t pushID = core.pushPaste("image/png", png);
        check(pushID != 0, "A: the paste push was sent");
        check(pumpUntil(dispatcher, 5000, [&] { return observer.m_pastePushAcks > 0; }), "A: the host answered the paste push");
        check(observer.m_lastPastePushStatus == Protocol::Control::PastePushStatus::Landed, "A: the paste landed on the host");
        check(observer.m_lastPastePushPath.size() > 4
                && observer.m_lastPastePushPath.compare(observer.m_lastPastePushPath.size() - 4, 4, ".png") == 0,
            "A: the landed file's extension names the payload's type");

        // The bytes on disk are the bytes pasted — a path that names a file
        // holding something else is worse than no paste at all.
        {
            std::ifstream landed(observer.m_lastPastePushPath, std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(landed)), std::istreambuf_iterator<char>());
            check(contents.size() == png.size()
                    && std::memcmp(contents.data(), png.data(), png.size()) == 0,
                "A: the landed bytes are the pasted bytes");
        }

        // The path was typed into the shell, so it is on the screen the same
        // way a hand-typed path would be.
        {
            const std::string basename = observer.m_lastPastePushPath.substr(observer.m_lastPastePushPath.rfind('/') + 1);
            check(pumpUntil(dispatcher, 5000, [&] { return gridText(*term).find(basename) != std::string::npos; }),
                "A: the landed path reached the screen as typed input");
        }
        // Leave the shell's line clean for the reattach check below.
        core.sendInput(std::string("\r"));
        (void)pumpUntil(dispatcher, 2000, [&] { return false; });
        ::unlink(observer.m_lastPastePushPath.c_str());

        check(core.sessionID() == pinnedSessionID && core.daemonInstanceID() == pinnedDaemonInstanceID && core.socketName() == pinnedSocketName,
            "A: input and paste delivery preserved the first creation receipt");
        core.stop(); // clean detach — the daemon keeps the session alive (tmux model)
        dispatcher.pump();
        term.reset();
    }

    // ---- Session B: REATTACH the same id; the marker must still be there. ----
    if (pinnedSessionID && pinnedDaemonInstanceID && pinnedSocketName == socketName) {
        auto term = makeTerm();
        TestDispatcher dispatcher;
        AttachChannelFactory factory(attachCommand, pinnedSocketName);
        RecordingObserver observer;
        Session::RemoteSessionCore core(term.get(), &dispatcher, &factory, &observer);

        core.setExpectedDaemonInstanceID(pinnedDaemonInstanceID);
        core.setReattachPolicy(Session::ReattachPolicy::AttachOnly);
        core.start(pinnedSessionID, 80, 24);
        check(pumpUntil(dispatcher, 5000, [&] { return core.acknowledged() && observer.m_screens > 0; }), "B: reattach connected + checkpoint applied");
        check(core.sessionID() == pinnedSessionID, "B: reattached the SAME session id");
        check(core.daemonInstanceID() == pinnedDaemonInstanceID && core.socketName() == pinnedSocketName,
            "B: reattached the SAME daemon and socket");
        check(pumpUntil(dispatcher, 5000, [&] { return gridText(*term).find(marker) != std::string::npos; }), "B: marker persisted across the clean detach + reattach");

        core.stop();
        dispatcher.pump();
        term.reset();
    } else
        check(false, "B: no verified creation receipt was available for reattach");

    // Tear the daemon down.
    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 200; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, 0);

    if (g_failures == 0)
        std::printf("remote_session_smoke: RemoteSessionCore over SubprocessChannel — attach + checkpoint + "
                    "typed marker + clean-detach reattach persistence — all passed\n");
    return g_failures == 0 ? 0 : 1;
}
