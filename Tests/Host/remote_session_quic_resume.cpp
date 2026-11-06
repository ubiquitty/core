// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/remote_session_quic_resume.cpp — a remote session redials its
// QUIC carrier by a resume proof and runs no bootstrap (Docs/sync.md §6.9
// "Resume without SSH"), over real loopback QUIC against a spawned host.
//
// A RemoteSessionCore attaches through a factory whose bootstrap mints a
// token, as the ssh run does. After a long suspension the core replaces its
// carrier: the factory is asked for the carrier it reached before, with a
// resume key in place of a token, and the bootstrap is not run again. The
// host continues the attachment, so the picture arrives as a diff without a
// checkpoint. A suspension the core is told of beforehand ends the carrier
// with a word to the host and resumes the same way. A proof the host cannot
// verify is dropped without a word, and the core goes through the bootstrap
// within its deadline.
//
// Usage: remote_session_quic_resume <path-to-ubiquitty-host>. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/QUICChannel.h"
#include "Core/Transport/QUICClient.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Session = Ubiquitty::Session;
namespace Transport = Ubiquitty::Transport;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace Ubiquitty::Session {

struct RemoteSessionDatagramIngressAccess {
    /// Returns the current connection's screen plane for protocol assertions.
    static const Protocol::Sync::ClientConnection* screen(const RemoteSessionCore& core)
    {
        return core.m_core->screen();
    }
};

} // namespace Ubiquitty::Session

namespace {

/// Verifies that the same pane continued without a checkpoint or recovery.
void checkDiffedResume(const Session::RemoteSessionCore& core, Sync::ID128 paneID)
{
    const auto* screen = Session::RemoteSessionDatagramIngressAccess::screen(core);
    REQUIRE_MSG(screen, "the resumed session has no screen connection");
    REQUIRE_MSG(screen->welcome().has_value(), "the resumed screen was not welcomed");
    const auto& resumed = screen->welcome()->resumed;
    REQUIRE_MSG(resumed.size() == 1, "the resume did not continue exactly one pane");
    CHECK(resumed.front().paneID == paneID);
    CHECK(resumed.front().outcome == Sync::ResumeOutcome::Diffed);
    const auto panes = screen->boundPanes();
    REQUIRE_MSG(panes.size() == 1, "the resumed connection did not bind exactly one pane");
    CHECK(panes.front().second == paneID);
    const auto& counters = screen->counters();
    CHECK_EQ(counters.checkpointsApplied, 0u);
    CHECK(counters.updatesApplied > 0);
    CHECK_EQ(counters.digestMismatches, 0u);
    CHECK_EQ(counters.resyncsSent, 0u);
}

/// The visible grid as text, a line per row.
///
/// - Parameter terminal: The terminal.
/// - Returns: The text.
std::string gridText(Ubiquitty::Terminal& terminal)
{
    std::string text;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint <= 0x7E ? static_cast<char>(codePoint) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

/// The session thread's stand-in: the carrier's I/O thread posts, the test
/// pumps, and a delay is honoured, since the core arms timers for moments
/// that have not come.
class TestDispatcher final : public Session::Dispatcher {
public:
    /// Queues work for the next pump.
    ///
    /// - Parameter work: The work.
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }

    /// Queues work for after a delay.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: The work.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(seconds * 1'000'000.0)), std::move(work));
    }

    /// Runs every piece of work whose time has come, in the order posted.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::scoped_lock lock(m_mutex);
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

    /// Pumps until a condition holds or a time runs out.
    ///
    /// - Parameters:
    ///   - milliseconds: The longest to wait.
    ///   - isDone: The condition.
    /// - Returns: Whether the condition held.
    bool pumpUntil(int milliseconds, const std::function<bool()>& isDone)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (isDone())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        pump();
        return isDone();
    }

private:
    /// Work and the moment it is due.
    struct Posted {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
    };

    /// Queues work for a moment.
    ///
    /// - Parameters:
    ///   - due: The moment.
    ///   - work: The work.
    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work)
    {
        const std::scoped_lock lock(m_mutex);
        m_queue.push_back({ .due = due, .work = std::move(work) });
    }

    std::mutex m_mutex;
    std::vector<Posted> m_queue;
};

/// What the factory does with the preamble a resume key builds.
enum class Proof : std::uint8_t {
    /// Presents it as built.
    AsBuilt,
    /// Flips a bit of its MAC, as a client that holds the wrong secret would
    /// present.
    Forged,
};

/// Builds QUIC carriers to the spawned host as a frontend's factory does:
/// through a bootstrap that mints a one-time token, or, once a bootstrap has
/// reached the host, at the same port and pin with a resume key and no
/// bootstrap.
class ReachFactory final : public Session::ChannelFactory {
public:
    /// - Parameters:
    ///   - hostBinary: The host, whose bootstrap mints tokens.
    ///   - controlDirectory: The daemon's control directory.
    ///   - port: The daemon's QUIC port.
    ReachFactory(std::string hostBinary, std::string controlDirectory, std::uint16_t port)
        : m_hostBinary(std::move(hostBinary))
        , m_controlDirectory(std::move(controlDirectory))
        , m_port(port)
    {
    }

    /// Runs the bootstrap and builds a carrier that presents its token.
    ///
    /// - Parameters:
    ///   - transport: The carrier asked for; QUIC is all this factory builds.
    ///   - done: Receives the carrier, or null when no token was minted.
    void createChannel(Session::TransportMode, Done done) override
    {
        ++m_bootstraps;
        std::string token;
        std::uint16_t mintedPort = 0;
        if (!mintQUICTokenViaBootstrap(m_hostBinary.c_str(), m_controlDirectory, token, mintedPort, m_pin) || m_pin.size() != 32) {
            done(nullptr, false);
            return;
        }
        m_hasReach = true;
        done(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_port, token, m_pin, origin()), true);
    }

    /// Whether a bootstrap has reached the host.
    [[nodiscard]] bool canResumeChannel() const override { return m_hasReach; }

    /// Builds a carrier that presents the resume key at the reach kept.
    ///
    /// - Parameters:
    ///   - key: The key.
    ///   - done: Receives the carrier.
    void resumeChannel(Session::ResumeKey key, Done done) override
    {
        ++m_resumes;
        Transport::ExportedPreamble preamble { .exporterLabel = std::move(key.exporterLabel), .build = std::move(key.buildPreamble) };
        if (m_proof == Proof::Forged) {
            preamble.build = [build = std::move(preamble.build)](std::span<const std::uint8_t, 32> exporter) {
                std::vector<std::uint8_t> bytes = build(exporter);
                if (!bytes.empty())
                    bytes.back() ^= 0x01;
                return bytes;
            };
        }
        done(std::make_unique<Transport::QUICChannel>("127.0.0.1", m_port, std::move(preamble), m_pin, origin()), true);
    }

    /// Sets what the next resumed carrier presents.
    ///
    /// - Parameter proof: As built, or forged.
    void setProof(Proof proof) { m_proof = proof; }

    /// How many times the bootstrap ran.
    [[nodiscard]] int bootstraps() const { return m_bootstraps; }

    /// How many carriers were built from a resume key.
    [[nodiscard]] int resumes() const { return m_resumes; }

private:
    /// Where every carrier reaches.
    [[nodiscard]] static Transport::Origin origin() { return { Transport::OriginKind::Daemon, "" }; }

    std::string m_hostBinary;
    std::string m_controlDirectory;
    std::uint16_t m_port;
    std::string m_pin;
    bool m_hasReach = false;
    Proof m_proof = Proof::AsBuilt;
    int m_bootstraps = 0;
    int m_resumes = 0;
};

/// Counts what the core tells its frontend.
class Observer final : public Session::RemoteSessionObserver {
public:
    /// Records the state.
    ///
    /// - Parameters:
    ///   - state: The state.
    ///   - detail: Why.
    void onStateChanged(Session::State state, const std::string&) override { m_state = state; }

    /// Counts a screen change.
    void onScreenChanged() override { ++m_screens; }

    Session::State m_state = Session::State::Idle;
    int m_screens = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-quic-session-resume.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "remote_session_quic_resume: mkdtemp failed\n");
        return 1;
    }
    const std::string controlDirectory = std::string(home) + "/.ubiquitty";
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    const std::string portArgument = std::to_string(port);
    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", controlDirectory.c_str(), 1);
        ::execl(hostBinary, hostBinary, "--daemon", "--quic-port", portArgument.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const auto stopDaemon = [&] {
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
    };
    if (!Ubiquitty::HostTest::waitForDaemonReady(controlDirectory + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary))) {
        std::fprintf(stderr, "remote_session_quic_resume: premise: the daemon never accepted a connection\n");
        stopDaemon();
        return 1;
    }

    {
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*terminal, 24, 80);
        TestDispatcher dispatcher;
        ReachFactory factory(hostBinary, controlDirectory, port);
        Observer observer;
        Session::RemoteSessionCore core(terminal.get(), &dispatcher, &factory, &observer);
        const auto isShowing = [&](const char* marker) { return gridText(*terminal).find(marker) != std::string::npos; };
        const auto isAttached = [&] { return core.state() == Session::State::Connected && core.acknowledged(); };
        const auto hasImage = [&] {
            for (const auto& image : terminal->images.stored) {
                if (!image.used || image.imageID != 42 || image.width != 1 || image.height != 1 || !image.pixels)
                    continue;
                return std::ranges::any_of(terminal->images.placements, [&](const auto& placement) {
                    return placement.used && placement.imageID == image.imageID;
                });
            }
            return false;
        };

        core.start(0, 80, 24);
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isAttached() && observer.m_screens > 0; }), "premise: the session did not attach over QUIC");
        CHECK_MSG(core.isUsingQUIC() && factory.bootstraps() == 1 && factory.resumes() == 0, "premise: the first attach did not ride one bootstrapped QUIC carrier");
        core.sendInput("echo FIRST_$((40+2))\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isShowing("FIRST_42"); }), "premise: the shell's output did not reach the screen");
        // printf cannot consume a graphics-protocol reply; suppress it so it
        // cannot become unsolicited shell input before the next command.
        core.sendInput("printf '\\033_Gf=24,s=1,v=1,i=42,a=T,q=2;/wAA\\033\\\\'\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, hasImage), "the host's Kitty pixels and placement did not reach the native remote-session reader");
        const std::uint32_t session = core.sessionID();
        const std::uint64_t checkpointBytes = core.paneBytesReceived();
        CHECK_MSG(checkpointBytes > 0, "premise: the first attach was sent no screen");
        std::optional<Sync::ID128> paneID;
        if (const auto* screen = Session::RemoteSessionDatagramIngressAccess::screen(core)) {
            const auto panes = screen->boundPanes();
            if (panes.size() == 1)
                paneID = panes.front().second;
            CHECK_EQ(screen->counters().checkpointsApplied, 1u);
        }
        CHECK_MSG(paneID.has_value(), "premise: the first attach did not bind exactly one pane");

        // A suspension longer than the host keeps an idle connection: the
        // carrier is replaced, by proof.
        core.resumeAfterSuspension(120.0);
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isAttached(); }), "the session did not come back after the suspension");
        CHECK_MSG(factory.resumes() == 1, "the redial did not present a resume key");
        CHECK_MSG(factory.bootstraps() == 1, "the redial ran the bootstrap again");
        CHECK_MSG(core.isUsingQUIC() && core.sessionID() == session, "the redial is not the same session over QUIC");
        core.sendInput("echo SECOND_$((40+2))\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isShowing("SECOND_42"); }), "input typed after the resume did not reach the shell");
        CHECK_MSG(isShowing("FIRST_42"), "the picture from before the resume is gone");
        CHECK_MSG(hasImage(), "the image and placement from before the resume are gone");
        // Pane bytes include required records and command output; their ratio
        // to a small initial checkpoint does not identify how the pane resumed.
        if (paneID)
            checkDiffedResume(core, *paneID);
        const std::uint64_t resumedBytes = core.paneBytesReceived();
        std::printf("remote_session_quic_resume: attach %llu pane bytes, resume %llu pane bytes\n",
            static_cast<unsigned long long>(checkpointBytes), static_cast<unsigned long long>(resumedBytes));

        // Told beforehand, the core ends its carrier itself and waits.
        core.suspend();
        CHECK_MSG(core.state() == Session::State::Reconnecting, "a suspended session is not waiting to resume");
        CHECK_MSG(!dispatcher.pumpUntil(2'500, [&] { return factory.resumes() > 1 || factory.bootstraps() > 1; }), "a suspended session redialled before it was resumed");
        core.resumeAfterSuspension(3.0);
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isAttached(); }), "the session did not come back from a suspension it announced");
        CHECK_MSG(factory.resumes() == 2 && factory.bootstraps() == 1, "the resume after an announced suspension did not go by proof");
        core.sendInput("echo THIRD_$((40+2))\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isShowing("THIRD_42"); }), "input typed after the announced suspension did not reach the shell");
        CHECK_MSG(hasImage(), "the second resume lost the retained image");
        if (paneID)
            checkDiffedResume(core, *paneID);

        // A proof the host cannot verify: the host says nothing, and the
        // core's deadline sends it through the bootstrap.
        factory.setProof(Proof::Forged);
        const auto forgedAt = std::chrono::steady_clock::now();
        core.resumeAfterSuspension(120.0);
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return factory.bootstraps() == 2; }), "a refused proof never fell back to the bootstrap");
        const auto fellBackAfter = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - forgedAt).count();
        const std::string wait = "the fallback took " + std::to_string(fellBackAfter) + " ms";
        CHECK_MSG(fellBackAfter < 4'000, wait.c_str());
        CHECK_MSG(factory.resumes() == 3, "the forged proof was dialled more than once");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isAttached(); }), "the session did not come back through the bootstrap");
        CHECK_MSG(core.sessionID() == session, "the bootstrap attached another session");
        core.sendInput("echo FOURTH_$((40+2))\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return isShowing("FOURTH_42"); }), "input typed after the fallback did not reach the shell");
        CHECK_MSG(dispatcher.pumpUntil(10'000, hasImage), "the bootstrap fallback did not recover the host's image");
        core.sendInput("printf '\\033_Ga=d,d=I,i=42,q=2;\\033\\\\'\r");
        CHECK_MSG(dispatcher.pumpUntil(10'000, [&] { return !hasImage(); }), "the image deletion did not reach the resumed reader");
        core.stop();
        dispatcher.pump();
    }

    stopDaemon();
    if (failures == 0)
        std::printf("remote_session_quic_resume: a session redialled QUIC by proof with no bootstrap and was sent a diff, resumed after an announced suspension, and fell back to the bootstrap when its proof was refused\n");
    return failures == 0 ? 0 : 1;
}
