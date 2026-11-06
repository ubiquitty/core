// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace_clients.cpp — the workspace model's terminal-client seam
// (Core/Runtime/WorkspaceSession.h): every session the model makes goes
// through the installer the frontend set, before that session's ingest
// starts, and a model with no installer makes sessions with no clients.

#include "Core/Runtime/WorkspaceSession.h"

#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/PaneReplies.h"
#include "Core/Protocol/Tmux/Session.h"
#include "Core/Runtime/TmuxControlHost.h"
#include "Core/Terminal.h"
#include "PAL/TerminalClients.h"
#include <charconv>

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unistd.h>

using namespace Ubiquitty::Runtime;

namespace {

// The client the installer attaches: the smallest ClipboardClient there is,
// existing so a test can see the terminal it landed on. The counters are
// atomic because Core drives the client on the ingest thread while the test
// watches from its own.
struct StubClipboard final : Ubiquitty::ClipboardClient {
    std::atomic<int> readRequests { 0 };
    std::atomic<int> readCalls { 0 };

    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char>) override { }

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> out) override
    {
        readCalls.fetch_add(1);
        if (out.size() < 2)
            return -1;
        out[0] = 'H';
        out[1] = 'i';
        return 2;
    }

    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view,
        Ubiquitty::ClipboardReadConfirmation) override
    {
        readRequests.fetch_add(1);
        return true;
    }
};

// Polls until `condition` holds or the budget runs out.
template<typename Condition>
bool waitFor(Condition condition, int budgetMilliseconds)
{
    for (int waited = 0; waited < budgetMilliseconds; waited += 10) {
        if (condition())
            return true;
        ::usleep(10 * 1000);
    }
    return condition();
}

// What the installer saw: how often it ran, and the client it minted for
// the latest session — reachable from the fn-pointer through context. The
// clients themselves are session-owned through adoptClientOwnership, the
// shape a real installer takes, so teardown order is the session's problem
// and never the test's.
struct InstallLog {
    Ubiquitty::ClipboardClient* lastAttached = nullptr;
    int installedCount = 0;
    bool sawNullTerminal = false;
    bool didSeeIngestRunning = false;
};

void installClients(void* context, TerminalSession& session)
{
    auto* log = static_cast<InstallLog*>(context);
    ++log->installedCount;
    Ubiquitty::Terminal* term = session.terminal();
    if (term == nullptr) {
        log->sawNullTerminal = true;
        return;
    }
    // startIngest() answers false on a session whose loop already runs; a
    // second start here proves the installer ran before the first.
    if (session.startIngest())
        session.stopIngest();
    else
        log->didSeeIngestRunning = true;
    auto client = std::make_unique<StubClipboard>();
    term->clipboardClient = client.get();
    log->lastAttached = client.get();
    session.adoptClientOwnership(std::move(client), [](Ubiquitty::Terminal& terminal, Ubiquitty::ClipboardClient& owned) {
        if (terminal.clipboardClient.get() == &owned) {
            terminal.clipboardClient = nullptr;
        }
    });
}

// Releases the model and every session it retains.
struct ModelHandle {
    WorkspaceSession* model = nullptr;
    explicit ModelHandle(WorkspaceSession::IngestPolicy policy)
        : model(WorkspaceSession::make(policy, DrawnText::SizedAndProportional))
    {
    }
    ~ModelHandle()
    {
        if (model != nullptr)
            releaseWorkspaceSession(model);
    }
};

} // namespace

TEST(testEverySessionTheModelMakesGoesThroughTheInstaller)
{
    InstallLog log;
    ModelHandle handle(WorkspaceSession::IngestPolicy::CallerDrained);
    WorkspaceSession& model = *handle.model;
    model.setTerminalClientInstaller(installClients, &log);

    const auto surface = model.addWindowSurface();
    CHECK(surface != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(log.installedCount == 1);
    CHECK(!log.sawNullTerminal);

    // A split makes its pane's session through the same fold.
    const auto split = model.splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical);
    CHECK(split.createdPane != Ubiquitty::Session::Layout::invalidPane);
    CHECK(log.installedCount == 2);

    // The installed client is reachable from the terminal the model handed
    // out — the seam delivered a live attachment, not a callback for show —
    // and it is the session-owned one the installer minted last.
    TerminalSession* session = model.session(split.createdPane);
    CHECK(session != nullptr && session->terminal() != nullptr);
    CHECK(log.lastAttached != nullptr);
    CHECK(session->terminal()->clipboardClient.get() == log.lastAttached);
}

TEST(testTheInstallerRunsBeforeIngestStarts)
{
    InstallLog log;
    ModelHandle handle(WorkspaceSession::IngestPolicy::ThreadDrained);
    WorkspaceSession& model = *handle.model;
    model.setTerminalClientInstaller(installClients, &log);

    const auto surface = model.addWindowSurface();
    CHECK(surface != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(log.installedCount == 1);
    // Under the thread-drained policy the model starts the loop itself —
    // and the installer's own probe start proved the loop was not yet
    // running when it saw the session.
    CHECK(!log.didSeeIngestRunning);
}

TEST(testAModelWithNoInstallerMakesClientlessSessions)
{
    ModelHandle handle(WorkspaceSession::IngestPolicy::CallerDrained);
    WorkspaceSession& model = *handle.model;

    const auto surface = model.addWindowSurface();
    CHECK(surface != Ubiquitty::Session::Layout::invalidSurface);
    TerminalSession* session = model.session(model.focusedPane(surface));
    CHECK(session != nullptr && session->terminal() != nullptr);
    CHECK(session->terminal()->clipboardClient.get() == nullptr);
}

TEST(testTheProcessDefaultSeedsAModelMadeAfterIt)
{
    InstallLog log;
    WorkspaceSession::setDefaultTerminalClientInstaller(installClients, &log);
    ModelHandle handle(WorkspaceSession::IngestPolicy::CallerDrained);
    WorkspaceSession& model = *handle.model;
    // The default is copied at construction; retract it before any other
    // test makes a model, and the copy keeps working regardless.
    WorkspaceSession::setDefaultTerminalClientInstaller(nullptr, nullptr);

    const auto surface = model.addWindowSurface();
    CHECK(surface != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(log.installedCount == 1);
}

TEST(testAPostedClipboardResolveDrivesTheCompletionOnTheModelThread)
{
    StubClipboard clip;
    TerminalSession* session = TerminalSession::make();
    CHECK(session != nullptr);
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    session->start(parameters);
    Ubiquitty::Terminal* term = session->terminal();
    CHECK(term != nullptr);
    // Attached before ingest starts, the way the installer seam does it, and
    // the read policy prompts so the read defers to the frontend.
    term->clipboardClient = &clip;
    term->osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    // The application on the far side asks for the clipboard: a 5522 read of
    // text/plain, printed by the child so it arrives through the real drain.
    CHECK(session->launchShellCommand(
        "printf '\\033]5522;type=read;dGV4dC9wbGFpbg==\\033\\\\'; sleep 2"));
    CHECK(session->startIngest());
    CHECK(waitFor([&] { return clip.readRequests.load() == 1; }, 5000));
    CHECK(clip.readCalls.load() == 0); // deferred: nothing read before the answer

    // The frontend answers from its own thread; the decision rides the
    // command ring and the completion reads the client under the model lock.
    session->resolveClipboardRead("c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    CHECK(waitFor([&] { return clip.readCalls.load() == 1; }, 5000));

    session->stopIngest();
    term->clipboardClient = nullptr;
    session->close();
    releaseTerminalSession(session);
}

TEST(testAdoptedSessionClientsResolveAndDetachFromTheirOwnModel)
{
    auto* owner = TerminalSession::make();
    auto* adapter = TerminalSession::make();
    CHECK(owner && adapter);
    owner->start(24, 80);
    adapter->start(24, 80);
    auto* anchor = adapter->terminal();
    auto* external = owner->terminal();
    adapter->adoptExternalTerminal(external, owner->modelLockAddressForAdoption());
    WorkspaceSession::setDefaultTerminalClientInstaller([](void*, TerminalSession& session) {
        auto client = std::make_unique<StubClipboard>();
        session.terminal()->clipboardClient = client.get();
        session.adoptClientOwnership(std::move(client), [](Ubiquitty::Terminal& terminal, Ubiquitty::ClipboardClient& owned) {
            if (terminal.clipboardClient.get() == &owned) {
                terminal.clipboardClient = nullptr;
            }
        });
    },
        nullptr);
    WorkspaceSession::installDefaultClientsForAdoptedSession(*adapter);
    WorkspaceSession::setDefaultTerminalClientInstaller(nullptr, nullptr);
    CHECK(external->clipboardClient);
    CHECK(!anchor->clipboardClient);
    external->isOSC52ClipboardReadInFlight = true;
    adapter->resolveClipboardRead("c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    CHECK(!external->isOSC52ClipboardReadInFlight);
    CHECK(static_cast<StubClipboard*>(external->clipboardClient.get())->readCalls == 1);
    CHECK(anchor->outputLength == 0);
    adapter->endExternalAdoption();
    CHECK(!external->clipboardClient);
    CHECK(adapter->terminal() == anchor);
    releaseTerminalSession(adapter);
    releaseTerminalSession(owner);
}

TEST(anAdoptedTmuxPaneQueuesItsOwnWindowRequestsAndBanners)
{
    // Two panes of one tmux window, each Terminal minted by the Session and
    // born with no clients at all — the shape a hosted pane has.
    Ubiquitty::Protocol::Tmux::Session tmux;
    tmux.applyLayout(0, "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}");
    Ubiquitty::Protocol::Tmux::Pane* leftPane = tmux.pane(1);
    Ubiquitty::Protocol::Tmux::Pane* rightPane = tmux.pane(2);
    CHECK(leftPane && rightPane && leftPane->term() && rightPane->term());
    if (!leftPane || !rightPane || !leftPane->term() || !rightPane->term())
        return;
    CHECK(!leftPane->term()->windowClient.installed());
    CHECK(!leftPane->term()->shellClient);

    auto* left = TerminalSession::make();
    auto* right = TerminalSession::make();
    CHECK(left && right);
    left->start(24, 40);
    right->start(24, 39);
    left->adoptExternalTerminal(leftPane->term(), reinterpret_cast<std::uintptr_t>(leftPane->lockHandle()));
    right->adoptExternalTerminal(rightPane->term(), reinterpret_cast<std::uintptr_t>(rightPane->lockHandle()));
    CHECK(left->hasAdoptedTerminalClients());
    CHECK(right->hasAdoptedTerminalClients());
    (void)left->takeEventFlags();
    (void)right->takeEventFlags();

    // The left pane's program asks for a banner and raises the window; the
    // right pane's asks for nothing. Delivered the way %output lands.
    const std::string_view asks = "\033]9;left pane done\007\033[5t";
    tmux.deliverOutput(1, { reinterpret_cast<const std::uint8_t*>(asks.data()), asks.size() });

    const std::uint32_t leftEvents = left->takeEventFlags();
    CHECK((leftEvents & (1u << static_cast<unsigned>(EventKind::NotificationPosted))) != 0);
    CHECK((leftEvents & (1u << static_cast<unsigned>(EventKind::WindowRequestPending))) != 0);
    CHECK(right->takeEventFlags() == 0);
    const TerminalSession::Notification banner = left->takeNotification();
    CHECK(banner.isPresent && banner.title == "left pane done");
    const TerminalSession::WindowRequest raise = left->takeWindowRequest();
    CHECK(raise.kind == TerminalSession::WindowRequest::Kind::WindowOp && raise.first == 5);
    CHECK(!right->hasPendingNotification());
    CHECK(!right->hasPendingWindowRequest());

    // An unbind takes every slot back out: the Session keeps the terminal
    // after the adapter is gone, and a slot left naming the adapter's member
    // is the CheckedPtr abort at its release.
    left->endExternalAdoption();
    CHECK(!left->hasAdoptedTerminalClients());
    CHECK(!leftPane->term()->windowClient.installed());
    CHECK(!leftPane->term()->shellClient);
    CHECK(!leftPane->term()->triggerClient);
    CHECK(right->hasAdoptedTerminalClients());
    releaseTerminalSession(left);
    releaseTerminalSession(right);
    CHECK(!rightPane->term()->shellClient);
}

TEST(anAdoptionLeavesAClientAnotherOwnerInstalled)
{
    Ubiquitty::Protocol::Tmux::Session tmux;
    tmux.applyLayout(0, "ef01,80x24,0,0,7");
    Ubiquitty::Protocol::Tmux::Pane* pane = tmux.pane(7);
    CHECK(pane && pane->term());
    if (!pane || !pane->term())
        return;
    Ubiquitty::ShellClient ownersClient;
    pane->term()->shellClient = &ownersClient;

    auto* adapter = TerminalSession::make();
    CHECK(adapter);
    adapter->start(24, 80);
    adapter->adoptExternalTerminal(pane->term(), reinterpret_cast<std::uintptr_t>(pane->lockHandle()));
    CHECK(pane->term()->shellClient.get() == &ownersClient);
    CHECK(pane->term()->windowClient.installed());
    CHECK(!adapter->hasAdoptedTerminalClients());
    adapter->endExternalAdoption();
    CHECK(pane->term()->shellClient.get() == &ownersClient);
    CHECK(!pane->term()->windowClient.installed());
    pane->term()->shellClient = nullptr;
    releaseTerminalSession(adapter);
}

TEST(aHostedPaneResizesThroughItsSourceAheadOfTheServer)
{
    struct RecordingStream final : Ubiquitty::PAL::ByteStream {
        std::string written;
        int read(std::span<char>) override { return 0; }
        int write(std::span<const char> bytes) override { return writeAll(bytes); }
        int writeAll(std::span<const char> bytes) override
        {
            written.append(bytes.data(), bytes.size());
            return static_cast<int>(bytes.size());
        }
        bool drainPendingWrite() override { return false; }
        bool hasPendingWrite() const override { return false; }
        int lastError() const override { return 0; }
    } stream;
    // The host a session running `tmux -CC` makes, over a primary terminal
    // that has listed one window with one pane.
    auto primary = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*primary, 24, 80);
    std::mutex primaryLock;
    HostedWindowSeams seams;
    seams.primary = primary.get();
    seams.primaryLock = &primaryLock;
    seams.stream = &stream;
    TmuxControlHost host(seams);
    host.attach();
    Ubiquitty::processBytes(primary.get(), "%begin 1 1 0\n%end 1 1 0\n");
    Ubiquitty::processBytes(primary.get(), "%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n");
    const HostedPaneFacts facts = host.paneFacts(1);
    auto* paneTerminal = reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress);
    CHECK(paneTerminal != nullptr);
    if (!paneTerminal)
        return;

    // The pane's view: a session adopting the pane's model, its input routed
    // through the primary's channel and its resize through the host.
    auto* adapter = TerminalSession::make();
    CHECK(adapter);
    adapter->start(24, 80);
    adapter->adoptExternalTerminal(paneTerminal, facts.lockAddress);
    adapter->routeInputViaTmuxAddresses(facts.inputClientAddress, facts.inputStreamAddress, 1);
    adapter->routeResizeThroughHostedSource(reinterpret_cast<std::uintptr_t>(&host), 1);
    stream.written.clear();

    // The view's grid moves: the pane's model follows now, before any
    // %layout-change from the server says so.
    adapter->resize(30, 100, TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Top);
    CHECK(paneTerminal->columns == 100 && paneTerminal->rows == 30);
    CHECK(host.paneFacts(1).columns == 100);
    CHECK(adapter->reportTmuxClientSize(100, 30));
    CHECK(stream.written.find(Ubiquitty::Protocol::Tmux::refreshClientSize(100, 30) + "\n") != std::string::npos);

    // The route goes with the adoption.
    adapter->endExternalAdoption();
    adapter->resize(20, 60, TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Top);
    CHECK(paneTerminal->columns == 100);
    releaseTerminalSession(adapter);
    host.releasePane(1);
    host.detach();
}

TEST(testTmuxPaneRepliesDrainBulkBytesAndKeepTheOrigin)
{
    struct RecordingStream final : Ubiquitty::PAL::ByteStream {
        std::string written;
        int read(std::span<char>) override { return 0; }
        int write(std::span<const char> bytes) override { return writeAll(bytes); }
        int writeAll(std::span<const char> bytes) override
        {
            written.append(bytes.data(), bytes.size());
            return static_cast<int>(bytes.size());
        }
        bool drainPendingWrite() override { return false; }
        bool hasPendingWrite() const override { return false; }
        int lastError() const override { return 0; }
    } stream;
    auto* session = TerminalSession::make();
    CHECK(session);
    session->start(24, 80);
    auto* terminal = session->terminal();
    std::string payload;
    for (int index = 0; index < 4096; ++index) {
        payload.push_back(static_cast<char>(index % 256));
    }
    CHECK(terminal->bulkOutput.append(payload, 8192));
    CHECK(terminal->outputLength == 0);
    Ubiquitty::Protocol::Tmux::TmuxClient client {};
    client.state = Ubiquitty::Protocol::Tmux::TmuxState::Passthrough;
    Ubiquitty::Protocol::Tmux::flushPaneReplies(*terminal, &client, 43, stream);
    CHECK(stream.written.empty());
    CHECK(terminal->bulkOutput.length == 4096);
    client.state = Ubiquitty::Protocol::Tmux::TmuxState::Line;
    Ubiquitty::Protocol::Tmux::flushPaneReplies(*terminal, &client, 43, stream);
    CHECK(terminal->bulkOutput.length == 0);
    std::string decoded;
    std::string_view remaining = stream.written;
    constexpr std::string_view prefix = "send-keys -t %43 -H";
    while (!remaining.empty()) {
        CHECK(remaining.starts_with(prefix));
        remaining.remove_prefix(prefix.size());
        while (!remaining.empty() && remaining.front() == ' ') {
            CHECK(remaining.size() >= 3);
            unsigned value = 0;
            const auto result = std::from_chars(remaining.data() + 1, remaining.data() + 3, value, 16);
            CHECK(result.ec == std::errc());
            decoded.push_back(static_cast<char>(value));
            remaining.remove_prefix(3);
        }
        CHECK(!remaining.empty() && remaining.front() == '\n');
        remaining.remove_prefix(1);
    }
    CHECK(decoded == payload);
    releaseTerminalSession(session);
}

TEST(testTransferredSessionsSurviveSourceWindowClose)
{
    ModelHandle handle(WorkspaceSession::IngestPolicy::CallerDrained);
    auto& model = *handle.model;
    const auto source = model.addWindowSurface();
    const auto pane = model.focusedPane(source);
    auto* session = model.session(pane);
    CHECK(session != nullptr);
    const auto destination = model.addWindowSurface();
    const auto target = model.focusedPane(destination);
    CHECK(model.moveToPane(source, pane, target, Ubiquitty::Panes::Orientation::Vertical, Ubiquitty::Panes::InsertSide::First, TerminalSession::TransferScope::Tab));
    model.closeWindowSurface(source);
    CHECK(model.session(pane) == session);
    CHECK(model.paneCount(destination) == 2);
    const auto extracted = model.extractPaneToSurface(destination, pane);
    CHECK(extracted != Ubiquitty::Session::Layout::invalidSurface);
    model.closeWindowSurface(destination);
    CHECK(model.session(pane) == session);
    CHECK(model.paneCount(extracted) == 1);
}

UBIQUITTY_TEST_MAIN()
