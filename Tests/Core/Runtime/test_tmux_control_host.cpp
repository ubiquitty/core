// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tmux_control_host.cpp — the tmux control host (Core/Runtime/
// TmuxControlHost.h) driven through the primary terminal's own parser, the way
// a session running `tmux -CC` drives it: the host enters control mode on the
// server's first block, opens a hosted window for each listed window, binds
// panes to Session-owned terminals, delivers %output into them, resizes them
// ahead of the server, and attributes a copy-mode pull to the pane it names.

#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Protocol/Tmux/Session.h"
#include "Core/Protocol/Tmux/Tiler.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Runtime/PostedWorkTable.h"
#include "Core/Runtime/TmuxControlHost.h"
#include "Core/Screen/Addressing.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "PAL/TerminalClients.h"

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty::Runtime;
namespace Tmux = Ubiquitty::Protocol::Tmux;

namespace {

// Records what the host writes to the control channel, and each write apart.
struct RecordingStream final : Ubiquitty::PAL::ByteStream {
    std::string written;
    std::vector<std::string> writes;
    int read(std::span<char>) override { return 0; }
    int write(std::span<const char> bytes) override { return writeAll(bytes); }
    int writeAll(std::span<const char> bytes) override
    {
        written.append(bytes.data(), bytes.size());
        writes.emplace_back(bytes.data(), bytes.size());
        return static_cast<int>(bytes.size());
    }
    bool drainPendingWrite() override { return false; }
    bool hasPendingWrite() const override { return false; }
    int lastError() const override { return 0; }
};

// Keeps what a pane's program writes to the clipboard.
struct RecordingClipboard final : Ubiquitty::ClipboardClient {
    std::vector<std::string> writes;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> data) override { writes.emplace_back(data.data(), data.size()); }
};

// The delayed tokens every harness's loop holds until a case runs them. One
// queue, because the loop is a pair of plain function pointers with no
// context; a case that keeps two harnesses runs neither's timers.
std::vector<PostedWorkToken>& delayedTokens()
{
    static std::vector<PostedWorkToken> tokens;
    return tokens;
}

// A primary terminal, its lock and channel, and the host over them. Posted
// work runs at once; delayed work waits for runDelayed.
struct HostHarness {
    RecordingStream stream;
    std::unique_ptr<Ubiquitty::Terminal> primary = std::make_unique<Ubiquitty::Terminal>();
    std::mutex lock;
    int announcements = 0;
    int publicationWakes = 0;
    std::uint64_t now = 12'000'000'000;
    std::vector<PostedWorkToken>& delayed = delayedTokens();
    std::unique_ptr<TmuxControlHost> host;

    explicit HostHarness(HostedControlState initialControlState = HostedControlState::Established, Ubiquitty::CapabilityOrigin origin = Ubiquitty::CapabilityOrigin::Local)
    {
        Ubiquitty::resize(*primary, 24, 80);
        HostedWindowSeams seams;
        seams.primary = primary.get();
        seams.primaryLock = &lock;
        seams.stream = &stream;
        seams.initialControlState = initialControlState;
        primary->currentCapabilityOrigin = origin;
        seams.sessionContext = this;
        seams.announce = [](void* context) { ++static_cast<HostHarness*>(context)->announcements; };
        seams.wakePresentation = [](void* context) { ++static_cast<HostHarness*>(context)->publicationWakes; };
        seams.clockMicroseconds = [](void* context) { return static_cast<HostHarness*>(context)->now; };
        seams.loop.runSoon = [](PostedWorkToken token) { runPostedSessionWork(token); };
        seams.loop.runAfter = [](double, PostedWorkToken token) { delayedTokens().push_back(token); };
        host = std::make_unique<TmuxControlHost>(seams);
    }

    ~HostHarness()
    {
        host->detach();
        host.reset();
        for (const PostedWorkToken token : std::exchange(delayed, {}))
            discardPostedSessionWork(token);
    }

    void feed(std::string_view bytes) { Ubiquitty::processBytes(primary.get(), bytes); }

    void runDelayed()
    {
        const std::vector<PostedWorkToken> due = std::exchange(delayed, {});
        for (const PostedWorkToken token : due)
            runPostedSessionWork(token);
    }

    std::string takeWritten() { return std::exchange(stream.written, std::string {}); }

    // Attaches, enters control mode, and lists one window with one pane.
    void attachWithOneWindow()
    {
        host->attach();
        feed("%begin 1 1 0\n%end 1 1 0\n");
        feed("%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n");
    }

    // As attachWithOneWindow, against a server that answers the version query
    // with `version` before it lists the pane.
    void attachWithOneWindow(std::string_view version)
    {
        host->attach();
        feed("%begin 1 1 0\n%end 1 1 0\n");
        feed("%begin 2 2 1\n" + std::string(Tmux::serverVersionReplyPrefix) + std::string(version) + "\n%end 2 2 1\n");
        feed("%begin 3 3 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 3 3 1\n");
    }

    std::vector<HostedWindowEvent> takeEvents()
    {
        std::vector<HostedWindowEvent> events;
        for (HostedWindowEvent event = host->takeEvent(); event.kind != HostedWindowEvent::Kind::None; event = host->takeEvent())
            events.push_back(std::move(event));
        return events;
    }
};

// A pane terminal's first row, as text.
std::string firstRow(Ubiquitty::Terminal* terminal)
{
    std::string text;
    if (!terminal)
        return text;
    for (int column = 0; column < terminal->columns; ++column) {
        const Ubiquitty::Cell* cell = terminal->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column));
        if (!cell || cell->codePoint == 0)
            break;
        text.push_back(static_cast<char>(cell->codePoint));
    }
    return text;
}

Ubiquitty::Terminal* paneTerminal(HostHarness& harness, int paneID)
{
    return reinterpret_cast<Ubiquitty::Terminal*>(harness.host->paneFacts(paneID).terminalAddress);
}

std::optional<Ubiquitty::Session::FrameReady> panePublication(HostHarness& harness, int paneID)
{
    const auto facts = harness.host->paneFacts(paneID);
    if (!facts.terminalAddress || !facts.lockAddress)
        return std::nullopt;
    const std::scoped_lock hold(*reinterpret_cast<std::mutex*>(facts.lockAddress));
    return harness.host->panePublication(paneID, *reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress));
}

// A screen's rows as `capture-pane -p` prints them, each trimmed of its
// trailing blanks.
std::vector<std::string> screenRows(const Ubiquitty::Terminal& terminal)
{
    std::vector<std::string> rows;
    for (int row = 0; row < terminal.rows; ++row) {
        std::string text;
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = terminal.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            const std::uint32_t codePoint = cell ? cell->codePoint : 0;
            text.push_back(codePoint > 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : ' ');
        }
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        rows.push_back(std::move(text));
    }
    return rows;
}

// The rows of a screen, each with whether it wraps into the next.
std::vector<std::pair<std::string, bool>> wrappedRows(const Ubiquitty::Terminal& terminal)
{
    std::vector<std::pair<std::string, bool>> rows;
    const std::vector<std::string> texts = screenRows(terminal);
    for (int row = 0; row < terminal.rows; ++row)
        rows.emplace_back(texts[static_cast<std::size_t>(row)], static_cast<bool>(terminal.screens.active->lineWrapped[row]));
    return rows;
}

// Replaces every `pattern` in `text` with `value`.
std::string replacingAll(std::string text, std::string_view pattern, std::string_view value)
{
    for (std::size_t at = text.find(pattern); at != std::string::npos; at = text.find(pattern, at + value.size()))
        text.replace(at, pattern.size(), value);
    return text;
}

// tmux's answer to the last capture the host wrote, for a pane whose screen
// is `server`: the marker's format with the cursor as `#{cursor_x}` and
// `#{cursor_y}` report it, then the capture block in the form its command
// asked for. tmux reports a wrap deferred at the right edge as the column past
// the last; `-F` puts each row behind its flags, `W` on a row wrapped into the
// next.
std::string answerLastCapture(std::string_view written, const Ubiquitty::Terminal& server)
{
    const std::size_t marker = written.rfind(Tmux::captureBeginPrefix);
    const std::size_t formatEnd = written.find('"', marker);
    const std::size_t command = written.find("capture-pane ", formatEnd);
    if (marker == std::string_view::npos || formatEnd == std::string_view::npos || command == std::string_view::npos)
        return {};
    const std::string_view captureCommand = written.substr(command, written.find('\n', command) - command);
    const Ubiquitty::Screen::State& state = *server.screens.active;
    std::string format(written.substr(marker, formatEnd - marker));
    format = replacingAll(format, "#{cursor_x}", std::to_string(state.cursorX + (state.wrapPending ? 1 : 0)));
    format = replacingAll(format, "#{cursor_y}", std::to_string(state.cursorY));
    const bool isFlagged = captureCommand.find(" -F") != std::string_view::npos;
    std::string answer = "%begin 20 20 1\n" + format + "\n%end 20 20 1\n%begin 21 21 1\n";
    for (const auto& [row, isWrapped] : wrappedRows(server))
        answer += (isFlagged ? (isWrapped ? "W " : "- ") : "") + row + "\n";
    return answer + "%end 21 21 1\n";
}

// What a selection from one visible cell to another copies.
std::string copiedText(Ubiquitty::Terminal& terminal, int fromRow, int fromColumn, int toRow, int toColumn)
{
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, terminal, fromRow, fromColumn, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, terminal, toRow, toColumn, 0);
    Ubiquitty::Selection::endGesture(selection);
    return Ubiquitty::Selection::selectedText(selection, terminal);
}

} // namespace

TEST(hostedIdentityIsClearedWhenControlModeEnds)
{
    HostHarness harness;
    harness.host->attach();
    harness.feed("%begin 1 1 0\n%end 1 1 0\n");
    harness.feed("%begin 2 2 1\nUBSESSION 321 1700000000 $0 1700000001 /tmp/s\n%end 2 2 1\n");
    const auto reference = harness.host->sessionForTesting().sessionReference();
    CHECK(reference.has_value());
    CHECK(reference->serverProcessID == 321);
    CHECK(harness.host->sessionReference().isConfirmed);
    CHECK(!harness.host->sessionReference().canReconnectLocally);
    harness.takeWritten();
    harness.feed("%session-changed $1 other\n");
    CHECK(!harness.host->sessionForTesting().sessionReference());
    CHECK(!harness.host->sessionReference().isConfirmed);
    CHECK(harness.takeWritten().find(Tmux::sessionReferenceQuery()) != std::string::npos);
    harness.feed("%begin 3 3 1\nUBSESSION 321 1700000000 $1 1700000002 /tmp/s\n%end 3 3 1\n");
    CHECK(harness.host->sessionForTesting().sessionReference()->sessionID == 1);
    harness.feed("%exit\n");
    CHECK(!harness.host->sessionForTesting().sessionReference());
}

TEST(anAttachKickstartsTheControlStreamAndEntersOnItsFirstBlock)
{
    HostHarness harness;
    harness.host->attach();
    const std::string attach = harness.takeWritten();
    CHECK(attach.find(Tmux::enableAggressiveResize() + "\n") == std::string::npos);
    CHECK(attach.find("refresh-client\n") != std::string::npos);
    CHECK(harness.primary->integration.paneRouter.lookup != nullptr);

    harness.feed("%begin 1 1 0\n%end 1 1 0\n");
    const std::string entered = harness.takeWritten();
    CHECK(entered.find(Tmux::enableAggressiveResize() + "\n") != std::string::npos);
    CHECK(entered.find(Tmux::sessionReferenceQuery() + "\n") != std::string::npos);
    // The client size is the primary's own grid until a hosting window reports.
    CHECK(entered.find(Tmux::refreshClientSize(80, 24) + "\n") != std::string::npos);
    CHECK(entered.find(Tmux::listPanesForSession() + "\n") != std::string::npos);
    // The version is answered before the panes are listed, so the first
    // capture a listed pane asks for already has its form.
    CHECK(entered.find(Tmux::serverVersionQuery() + "\n") < entered.find(Tmux::listPanesForSession() + "\n"));

    harness.host->detach();
    CHECK(harness.primary->integration.paneRouter.lookup == nullptr);
    CHECK(!Tmux::isActive(&harness.primary->integration.tmux));
}

TEST(anExplicitSSHClientWaitsForAuthenticationAndKeepsRemotePaneAuthority)
{
    HostHarness harness(HostedControlState::AwaitingHandshake, Ubiquitty::CapabilityOrigin::Remote);
    harness.host->attach();
    CHECK(harness.takeWritten().empty());
    harness.feed("The authenticity of host cannot be established.\r\nPassword: ");
    CHECK(harness.takeWritten().empty());
    CHECK(harness.takeEvents().empty());
    harness.feed("\r\n%begin 1 1 0\n%end 1 1 0\n");
    const auto entered = harness.takeWritten();
    CHECK(entered.find(Tmux::sessionReferenceQuery()) != std::string::npos);
    CHECK(entered.find(Tmux::enableAggressiveResize()) != std::string::npos);
    harness.feed("%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n");
    auto* terminal = paneTerminal(harness, 1);
    CHECK(terminal != nullptr);
    if (terminal) {
        CHECK(terminal->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
    }
}

TEST(aListedWindowOpensWithItsLayoutAndItsPaneIsRecovered)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    const std::vector<HostedWindowEvent> events = harness.takeEvents();
    CHECK(events.size() == 2);
    if (events.size() != 2)
        return;
    CHECK(events[0].kind == HostedWindowEvent::Kind::WindowOpened);
    CHECK(events[0].windowID == 1 && events[0].name == "editor" && events[0].sessionName == "work");
    CHECK(events[1].kind == HostedWindowEvent::Kind::LayoutChanged);
    CHECK(events[1].windowID == 1 && events[1].activePaneID == 1);
    CHECK(events[1].paneIDs == std::vector<int> { 1 });
    CHECK(events[1].layout == "ef01,80x24,0,0,1");
    CHECK(harness.announcements > 0);

    const HostedPaneFacts facts = harness.host->paneFacts(1);
    CHECK(facts.terminalAddress != 0 && facts.lockAddress != 0);
    CHECK(facts.columns == 80 && facts.rows == 24);
    CHECK(facts.inputClientAddress == reinterpret_cast<std::uintptr_t>(&harness.primary->integration.tmux));
    CHECK(facts.inputStreamAddress == reinterpret_cast<std::uintptr_t>(&harness.stream));

    // tmux %output carries only new output, so a new pane's screen and modes
    // are asked for once.
    const std::string written = harness.takeWritten();
    CHECK(written.find(Tmux::paneModeQuery(1) + "\n") != std::string::npos);
    CHECK(written.find(Tmux::captureBeginMarker(1, 0) + "\n") != std::string::npos);
    CHECK(written.find(Tmux::capturePane(1) + "\n") != std::string::npos);

    // A second row for the same window opens nothing more.
    harness.feed("%begin 3 3 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 3 3 1\n");
    CHECK(harness.takeEvents().empty());
}

// A marker and the command whose block it names are answered as two blocks
// the reader pairs by order, and captures are asked for from the ingest thread
// and the frontend loop alike. Written apart, another capture's marker could
// land between them and be read as this one's screen: a pane under churn
// showed the marker text itself.
TEST(aMarkerAndTheBlockItNamesGoOutInOneWrite)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    (void)harness.takeEvents();
    const auto writeHolding = [&](const std::string& line) -> const std::string* {
        for (const std::string& write : harness.stream.writes) {
            if (write.find(line) != std::string::npos)
                return &write;
        }
        return nullptr;
    };
    const std::string* capture = writeHolding(Tmux::captureBeginMarker(1, 0) + "\n");
    CHECK_MSG(capture && *capture == Tmux::captureBeginMarker(1, 0) + "\n" + Tmux::capturePane(1) + "\n", "a capture's marker went out apart from its capture-pane");

    harness.stream.writes.clear();
    harness.feed("%pane-mode-changed %1\n");
    const std::string* pull = writeHolding(Tmux::clipboardPullMarker(1) + "\n");
    CHECK_MSG(pull && *pull == Tmux::clipboardPullMarker(1) + "\n" + Tmux::clipboardPullShowBuffer() + "\n", "a clipboard pull's marker went out apart from its show-buffer");
}

TEST(aTmuxPaneDetectsPasswordPromptsWithoutLocalPTYFacts)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    auto* terminal = paneTerminal(harness, 1);
    REQUIRE(terminal);
    REQUIRE(terminal->replyAuthority == Ubiquitty::ReplyAuthority::ThisTerminal);
    terminal->passwordDetect = true;
    harness.feed("%output %1 Password:\n");
    CHECK(terminal->passwordPromptActive);
    harness.feed("%output %1 \\015\\012$ \n");
    CHECK(!terminal->passwordPromptActive);
}

TEST(aPanesOutputLandsInItsOwnTerminalAndAsksForARepaint)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    (void)harness.takeEvents();
    (void)harness.host->takeRepaintedPanes();
    harness.feed("%output %1 hello\n");
    CHECK(firstRow(paneTerminal(harness, 1)).starts_with("hello"));
    CHECK(firstRow(harness.primary.get()).find("hello") == std::string::npos);
    CHECK(harness.host->takeRepaintedPanes().empty());
    harness.now += readQuiescenceMicroseconds;
    harness.runDelayed();
    CHECK(harness.host->takeRepaintedPanes() == std::vector<int> { 1 });
    CHECK(harness.host->takeRepaintedPanes().empty());
}

TEST(aSplitAddsAPaneAndResyncsTheOneThatNarrowed)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    (void)harness.takeEvents();
    (void)harness.takeWritten();
    harness.feed("%layout-change @1 abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}\n");
    const std::vector<HostedWindowEvent> events = harness.takeEvents();
    CHECK(events.size() == 1);
    if (events.size() != 1)
        return;
    CHECK(events[0].kind == HostedWindowEvent::Kind::LayoutChanged);
    CHECK(events[0].paneIDs == (std::vector<int> { 1, 2 }));
    CHECK(events[0].activePaneID < 0);
    CHECK(paneTerminal(harness, 1) && paneTerminal(harness, 1)->columns == 40);
    CHECK(paneTerminal(harness, 2) && paneTerminal(harness, 2)->columns == 39);
    // The new pane is recovered at once; the narrowed one waits for the
    // layout to settle and then fetches its whole history, which tmux rewrapped.
    const std::string immediate = harness.takeWritten();
    CHECK(immediate.find(Tmux::capturePane(2) + "\n") != std::string::npos);
    CHECK(immediate.find(Tmux::capturePaneWithHistory(1)) == std::string::npos);
    CHECK(!harness.delayed.empty());
    harness.runDelayed();
    CHECK(harness.takeWritten().find(Tmux::capturePaneWithHistory(1) + "\n") != std::string::npos);
}

TEST(aPaneResizesAheadOfTheServerAndTheOtherWindowsFollowTheClientSize)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    harness.feed("%begin 4 4 1\n@2 %3 80 24 1 ef02,80x24,0,0,3 work logs\n%end 4 4 1\n");
    (void)harness.takeEvents();
    (void)harness.takeWritten();

    // The view's grid, now, before any %layout-change says so.
    harness.host->resizePane(1, 100, 30);
    CHECK(paneTerminal(harness, 1)->columns == 100 && paneTerminal(harness, 1)->rows == 30);

    // The window holding pane 1 reports its grid; the other window's pane is
    // sized to it in the same call, ahead of the server's re-layout.
    CHECK(harness.host->reportClientSize(1, 100, 30));
    CHECK(harness.takeWritten().find(Tmux::refreshClientSize(100, 30) + "\n") != std::string::npos);
    CHECK(paneTerminal(harness, 3)->columns == 100 && paneTerminal(harness, 3)->rows == 30);

    // The server's own layout at the same size changes nothing further.
    harness.feed("%layout-change @2 ef03,100x30,0,0,3\n");
    CHECK(paneTerminal(harness, 3)->columns == 100);
}

TEST(aClosedWindowKeepsItsPanesUntilTheFrontendReleasesThem)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    (void)harness.takeEvents();
    harness.feed("%window-renamed @1 renamed\n%session-renamed newname\n%window-close @1\n");
    const std::vector<HostedWindowEvent> events = harness.takeEvents();
    CHECK(events.size() == 3);
    if (events.size() != 3)
        return;
    CHECK(events[0].kind == HostedWindowEvent::Kind::WindowRenamed && events[0].windowID == 1 && events[0].name == "renamed");
    CHECK(events[1].kind == HostedWindowEvent::Kind::SessionRenamed && events[1].sessionName == "newname");
    CHECK(events[2].kind == HostedWindowEvent::Kind::WindowClosed && events[2].windowID == 1);
    // A view still reads the pane until the frontend answers.
    CHECK(harness.host->paneFacts(1).terminalAddress != 0);
    harness.host->releasePane(1);
    harness.host->releaseWindow(1);
    CHECK(harness.host->paneFacts(1).terminalAddress == 0);
    CHECK(harness.host->sessionForTesting().takeRetired().empty());

    harness.feed("%exit\n");
    const std::vector<HostedWindowEvent> ended = harness.takeEvents();
    CHECK(ended.size() == 1 && ended[0].kind == HostedWindowEvent::Kind::Ended);
}

TEST(aCopyModePullWritesThroughThePaneItNamesAndNoOther)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    harness.feed("%layout-change @1 abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}\n");
    (void)harness.takeEvents();
    RecordingClipboard paneOneClipboard;
    Ubiquitty::Terminal* paneOne = paneTerminal(harness, 1);
    CHECK(paneOne != nullptr);
    if (!paneOne)
        return;
    paneOne->clipboardClient = &paneOneClipboard;
    (void)harness.takeWritten();

    harness.feed("%pane-mode-changed %1\n");
    const std::string asked = harness.takeWritten();
    CHECK(asked.find(Tmux::clipboardPullMarker(1) + "\n") != std::string::npos);
    CHECK(asked.find(Tmux::clipboardPullShowBuffer() + "\n") != std::string::npos);
    harness.feed("%begin 5 5 1\nUBCLIPBEGIN %1\n%end 5 5 1\n%begin 6 6 1\ncopied from one\n%end 6 6 1\n");
    CHECK(paneOneClipboard.writes == std::vector<std::string> { "copied from one" });

    // Pane 2 is bound by no view, so it has no client to write through; a
    // pane id the session does not know resolves to nothing and fails closed.
    harness.feed("%begin 7 7 1\nUBCLIPBEGIN %2\n%end 7 7 1\n%begin 8 8 1\ncopied from two\n%end 8 8 1\n");
    harness.feed("%begin 9 9 1\nUBCLIPBEGIN %77\n%end 9 9 1\n%begin 10 10 1\nfrom nowhere\n%end 10 10 1\n");
    CHECK(paneOneClipboard.writes.size() == 1);
    paneOne->clipboardClient = nullptr;
}

TEST(aLayoutRefitKeepsItsSplitsInProportion)
{
    Tmux::LayoutNode root;
    CHECK(Tmux::parseLayoutTree("abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}", root));
    CHECK(Tmux::resizeLayout(root, 120, 30));
    const std::vector<Tmux::PaneSlot> slots = Tmux::layoutSlots(root);
    CHECK(slots.size() == 2);
    if (slots.size() != 2)
        return;
    // One divider between the two, the rest shared 40:39.
    CHECK(slots[0].columns + slots[1].columns + 1 == 120);
    CHECK(slots[0].columns == 60);
    CHECK(slots[1].x == slots[0].columns + 1);
    CHECK(slots[0].rows == 30 && slots[1].rows == 30);
    // Too small to hold both panes: the layout is left as it was.
    CHECK(!Tmux::resizeLayout(root, 2, 30));
    CHECK(Tmux::layoutSlots(root)[0].columns == 60);
}

TEST(hostedFocusTargetsOnlyItsExactModelAndReportsEachEdgeOnce)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    harness.feed("%begin 3 3 1\n@2 %3 80 24 1 ef02,80x24,0,0,3 work logs\n%end 3 3 1\n");
    auto* first = paneTerminal(harness, 1);
    auto* sibling = paneTerminal(harness, 3);
    REQUIRE(first && sibling);
    harness.feed("%output %1 \\033[?1004h\n");
    const bool siblingFocus = sibling->windowFocused;
    const bool primaryFocus = harness.primary->windowFocused;
    (void)harness.takeWritten();
    CHECK(!harness.host->setPaneFocus(1, *sibling, Ubiquitty::Modes::FocusState::Unfocused));
    CHECK(!harness.host->setPaneFocus(999, *first, Ubiquitty::Modes::FocusState::Unfocused));
    CHECK(harness.takeWritten().empty());

    REQUIRE(harness.host->setPaneFocus(1, *first, Ubiquitty::Modes::FocusState::Unfocused));
    CHECK(!first->windowFocused);
    CHECK(sibling->windowFocused == siblingFocus);
    CHECK(harness.primary->windowFocused == primaryFocus);
    CHECK_STR_EQ(harness.takeWritten(), "send-keys -t %1 -H 1b 5b 4f\n");
    CHECK(harness.host->setPaneFocus(1, *first, Ubiquitty::Modes::FocusState::Unfocused));
    CHECK(harness.takeWritten().empty());

    first->hasUnseenBell = true;
    ++first->attentionGeneration;
    CHECK(harness.host->setPaneFocus(1, *first, Ubiquitty::Modes::FocusState::Focused));
    CHECK(first->windowFocused);
    CHECK(!first->hasUnseenBell);
    CHECK_STR_EQ(harness.takeWritten(), "send-keys -t %1 -H 1b 5b 49\n");
    CHECK(harness.host->setPaneFocus(1, *first, Ubiquitty::Modes::FocusState::Focused));
    CHECK(harness.takeWritten().empty());
}

TEST(aHostedPublicationKeepsTheProgramsBoundaryAndExactModel)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    const auto first = panePublication(harness, 1);
    REQUIRE(first);
    CHECK(first->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    CHECK_EQ(harness.host->publicationClockMicroseconds(), harness.now);
    harness.feed("%output %1 \\033[?2026hpartial\n");
    CHECK(panePublication(harness, 1) == first);
    harness.now += 1'000;
    harness.feed("%output %1 complete\\033[?2026l\n");
    const auto next = panePublication(harness, 1);
    REQUIRE(next);
    CHECK_EQ(next->generation, first->generation + 1);
    CHECK(next->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Boundary);
    CHECK_EQ(next->lagMicroseconds, 1'000u);
    CHECK(firstRow(paneTerminal(harness, 1)).starts_with("partialcomplete"));
    Ubiquitty::Terminal foreign;
    std::mutex foreignLock;
    const std::scoped_lock hold(foreignLock);
    CHECK(!harness.host->panePublication(1, foreign));
    CHECK(!harness.host->panePublication(999, foreign));
}

TEST(aHostedQuietFrameWakesPresentationWithoutFurtherOutput)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    const auto first = panePublication(harness, 1);
    REQUIRE(first);
    (void)harness.host->takeRepaintedPanes();
    harness.feed("%output %1 hello\n");
    CHECK(panePublication(harness, 1) == first);
    CHECK(harness.host->takeRepaintedPanes().empty());
    REQUIRE(!harness.delayed.empty());
    harness.now += readQuiescenceMicroseconds;
    harness.runDelayed();
    const auto next = panePublication(harness, 1);
    REQUIRE(next);
    CHECK_EQ(next->generation, first->generation + 1);
    CHECK(next->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Settled);
    CHECK_EQ(next->lagMicroseconds, readQuiescenceMicroseconds);
    CHECK_EQ(harness.publicationWakes, 1);
    CHECK(harness.host->takeRepaintedPanes() == std::vector<int> { 1 });
    CHECK(panePublication(harness, 1) == next);
}

TEST(hostedRecoveryAndEachGeometryWriterPublishTheOwnedModel)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    const auto first = panePublication(harness, 1);
    REQUIRE(first);
    harness.feed("%begin 3 3 1\nUBCAPBEGIN 1 0 0 0\n%end 3 3 1\n%begin 4 4 1\nrecovered\n%end 4 4 1\n");
    const auto recovered = panePublication(harness, 1);
    REQUIRE(recovered);
    CHECK_EQ(recovered->generation, first->generation + 1);
    CHECK(recovered->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Settled);
    CHECK(firstRow(paneTerminal(harness, 1)).starts_with("recovered"));
    harness.host->resizePane(1, 100, 30);
    const auto resized = panePublication(harness, 1);
    REQUIRE(resized);
    CHECK_EQ(resized->generation, recovered->generation + 1);
    CHECK(resized->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    harness.host->resizePane(1, 100, 30);
    CHECK(panePublication(harness, 1) == resized);
    harness.feed("%layout-change @1 ef01,90x28,0,0,1\n");
    const auto laidOut = panePublication(harness, 1);
    REQUIRE(laidOut);
    CHECK_EQ(laidOut->generation, resized->generation + 1);
    CHECK(laidOut->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    harness.feed("%begin 5 5 1\n@2 %3 80 24 1 ef02,80x24,0,0,3 work logs\n%end 5 5 1\n");
    const auto sibling = panePublication(harness, 3);
    REQUIRE(sibling);
    REQUIRE(harness.host->reportClientSize(1, 120, 32));
    const auto siblingResized = panePublication(harness, 3);
    REQUIRE(siblingResized);
    CHECK_EQ(siblingResized->generation, sibling->generation + 1);
    CHECK(siblingResized->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Structural);
    CHECK_EQ(paneTerminal(harness, 3)->columns, 120);
    CHECK(panePublication(harness, 1) == laidOut);
    harness.feed("%begin 6 6 1\nUBMODES %1 100000\n%end 6 6 1\n");
    harness.now += readQuiescenceMicroseconds;
    harness.runDelayed();
    const auto modes = panePublication(harness, 1);
    REQUIRE(modes);
    CHECK_EQ(modes->generation, laidOut->generation + 1);
    CHECK(modes->frameClass == Ubiquitty::Protocol::Sync::FrameClass::Settled);
}

TEST(aRetiredHostedModelCannotBeReachedByItsPendingPublicationTimer)
{
    HostHarness harness;
    harness.attachWithOneWindow();
    (void)harness.host->takeRepaintedPanes();
    harness.feed("%output %1 waiting for quiescence\n");
    REQUIRE(!harness.delayed.empty());
    harness.feed("%window-close @1\n");
    harness.host->releasePane(1);
    harness.host->releaseWindow(1);
    CHECK(!paneTerminal(harness, 1));
    const auto wakes = harness.publicationWakes;
    harness.now += readQuiescenceMicroseconds;
    harness.runDelayed();
    CHECK_EQ(harness.publicationWakes, wakes);
    CHECK(harness.host->takeRepaintedPanes().empty());
}

// The pane a split narrowed fetches its whole history once the layout
// settles, and its program's output keeps arriving meanwhile, so the capture
// can land anywhere in a line still being written. Wherever it lands, the pane
// ends showing what the output alone leaves on tmux's own screen; a capture
// taken with the line filled to the right edge carries the wrap tmux deferred
// there, and the next character goes on the next row. A server that prints
// its rows' flags gives the pane tmux's wraps as well.
TEST(aResyncCaptureLandingMidLineLeavesThePaneAsTheOutputAlone)
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string line;
    for (std::size_t index = 0; index < 130; ++index)
        line.push_back(alphabet[index % alphabet.size()]);
    for (const std::string_view version : { std::string_view("3.6"), std::string_view("3.7") }) {
        const bool isFlagged = Tmux::captureFormForVersion(version) == Tmux::CaptureForm::FlaggedRows;
        int disagreements = 0;
        for (std::size_t cut = 1; cut < line.size(); ++cut) {
            HostHarness harness;
            harness.attachWithOneWindow(version);
            harness.feed("%layout-change @1 abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}\n");
            harness.feed("%output %1 " + line.substr(0, cut) + "\n");
            (void)harness.takeWritten();
            harness.runDelayed();
            const std::string written = harness.takeWritten();
            REQUIRE(written.find(Tmux::capturePaneWithHistory(1, Tmux::captureFormForVersion(version)) + "\n") != std::string::npos);
            // tmux's own screen for the pane, written the same bytes.
            auto server = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::resize(*server, 24, 40);
            Ubiquitty::processBytes(server.get(), line.substr(0, cut));
            harness.feed(answerLastCapture(written, *server));
            harness.feed("%output %1 " + line.substr(cut) + "\n");
            Ubiquitty::processBytes(server.get(), line.substr(cut));
            Ubiquitty::Terminal* pane = paneTerminal(harness, 1);
            REQUIRE(pane && pane->columns == 40);
            const bool isAgreeing = isFlagged ? wrappedRows(*pane) == wrappedRows(*server) : screenRows(*pane) == screenRows(*server);
            if (!isAgreeing) {
                ++disagreements;
                CHECK_MSG(false, ("tmux " + std::string(version) + ": the capture landed after " + std::to_string(cut) + " bytes, and the pane differs from tmux's").c_str());
            }
        }
        CHECK_EQ(disagreements, 0);
    }
}

// A pane recovered from a server that prints its rows' flags holds tmux's
// wraps: a 130-character line is one line across its two rows, and a pair of
// lines broken by a newline stays two. The recovery asks for the flagged
// form only of a server that has it.
TEST(aRecoveredPaneKeepsTheWrapsTmuxRecorded)
{
    std::string line;
    for (std::size_t index = 0; index < 130; ++index)
        line.push_back(static_cast<char>('a' + index % 26));
    auto server = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*server, 24, 80);
    Ubiquitty::processBytes(server.get(), line + "\r\nleft\r\nright\r\n$ ");
    REQUIRE(server->screens.active->lineWrapped[0] && !server->screens.active->lineWrapped[1]);

    HostHarness harness;
    harness.attachWithOneWindow("3.7");
    const std::string written = harness.takeWritten();
    CHECK(written.find(Tmux::capturePane(1, Tmux::CaptureForm::FlaggedRows) + "\n") != std::string::npos);
    harness.feed(answerLastCapture(written, *server));
    Ubiquitty::Terminal* pane = paneTerminal(harness, 1);
    REQUIRE(pane != nullptr);
    CHECK(wrappedRows(*pane) == wrappedRows(*server));
    CHECK(Ubiquitty::Screen::visibleRowWrapped(pane, 0, 0));
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(pane, 0, 1));
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(pane, 0, 2));
    CHECK(copiedText(*pane, 0, 0, 1, 49) == line);
    CHECK(copiedText(*pane, 2, 0, 3, 4) == "left\nright");

    HostHarness older;
    older.attachWithOneWindow("3.6");
    CHECK(older.takeWritten().find(Tmux::capturePane(1) + "\n") != std::string::npos);
}

UBIQUITTY_TEST_MAIN()
