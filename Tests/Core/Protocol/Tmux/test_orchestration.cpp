// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_orchestration.cpp — host tests for the promoted tmux control-mode
// orchestration: the window/pane registry (Protocol/Tmux/WindowRegistry.h), the
// attach state-recovery state machine (Protocol/Tmux/StateRecovery.h), the launch
// argv assembly (Network/TmuxLaunch.h), and the mutation command builders
// (Protocol/Tmux/Commands.h). Every -CC client — each frontend and the host —
// is built from this one pure logic, so its dedup / cap / ordering / recovery /
// argv-shape rules are pinned here, headless, once for all of them.

#include "Core/Network/TmuxLaunch.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/ContextActions.h"
#include "Core/Protocol/Tmux/StateRecovery.h"
#include "Core/Protocol/Tmux/WindowRegistry.h"
#include "Core/Transport/SSHHost.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Network;
using namespace Ubiquitty::Protocol::Tmux;

// ===== command builders =====================================================

TEST(testMutationCommandBuilders)
{
    // Wire bytes are the tmux command text, less the trailing '\n' the
    // transport / caller appends (matching the query builders in this
    // header).
    CHECK_STR_EQ(splitWindow(3, SplitDirection::SideBySide), "split-window -h -t %3");
    CHECK_STR_EQ(splitWindow(3, SplitDirection::Stacked), "split-window -v -t %3");
    CHECK_STR_EQ(splitWindow(0, SplitDirection::SideBySide), "split-window -h -t %0");
    CHECK_STR_EQ(killPane(7), "kill-pane -t %7");
    CHECK_STR_EQ(killPane(0), "kill-pane -t %0");
    CHECK_STR_EQ(killWindow(7), "kill-window -t @7");
    CHECK_STR_EQ(killWindow(0), "kill-window -t @0");
    CHECK_STR_EQ(selectPane(12), "select-pane -t %12");
    CHECK_STR_EQ(zoomPane(4), "resize-pane -Z -t %4");
    CHECK_STR_EQ(zoomPane(0), "resize-pane -Z -t %0");
    CHECK_STR_EQ(newWindow(), "new-window");
    CHECK_STR_EQ(detachClient(), "detach-client");
    CHECK_STR_EQ(killSession(), "kill-session");
    CHECK_STR_EQ(enableAggressiveResize(), "set-option aggressive-resize on");
    CHECK_STR_EQ(continuePane(3), "refresh-client -A %3:continue");
    CHECK_STR_EQ(continuePane(0), "refresh-client -A %0:continue");
    // renameWindow quotes for tmux's command grammar (backslash + quote
    // escaped); a pane target resolves to its containing window.
    CHECK_STR_EQ(renameWindow(5, "build"), "rename-window -t %5 \"build\"");
    CHECK_STR_EQ(renameWindow(5, "a b"), "rename-window -t %5 \"a b\"");
    CHECK_STR_EQ(renameWindow(5, "say \"hi\" \\ok"), "rename-window -t %5 \"say \\\"hi\\\" \\\\ok\"");
    CHECK_STR_EQ(renameWindow(5, ""), "rename-window -t %5 \"\"");
}

// ===== context-action synthesis =============================================

TEST(testContextActionSynthesis)
{
    using Transport = ContextAction::Transport;

    // ControlMode: intents map onto the command builders, pane-targeted.
    ContextAction a = synthesizeAction(PaneContext::ControlMode, ActionIntent::SplitSideBySide, 7, "");
    CHECK(a.transport == Transport::ControlCommand);
    CHECK_STR_EQ(a.bytes, "split-window -h -t %7");
    a = synthesizeAction(PaneContext::ControlMode, ActionIntent::SplitStacked, 7, "");
    CHECK_STR_EQ(a.bytes, "split-window -v -t %7");
    a = synthesizeAction(PaneContext::ControlMode, ActionIntent::KillPane, 7, "");
    CHECK_STR_EQ(a.bytes, "kill-pane -t %7");
    a = synthesizeAction(PaneContext::ControlMode, ActionIntent::NewWindow, 7, "");
    CHECK_STR_EQ(a.bytes, "new-window");
    a = synthesizeAction(PaneContext::ControlMode, ActionIntent::RenameWindow, 7, "logs");
    CHECK(a.transport == Transport::ControlCommand);
    CHECK_STR_EQ(a.bytes, "rename-window -t %7 \"logs\"");
    // Control bytes in a typed value are stripped before the command grammar.
    a = synthesizeAction(PaneContext::ControlMode, ActionIntent::RenameWindow, 7,
        "a\x1b\x02"
        "b\x7f");
    CHECK_STR_EQ(a.bytes, "rename-window -t %7 \"ab\"");

    // RawTmuxAttach: prefix chords typed into the pane. Default prefix C-b.
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::SplitSideBySide, 7, "");
    CHECK(a.transport == Transport::Keystrokes);
    CHECK_STR_EQ(a.bytes, "\x02%");
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::SplitStacked, 7, "");
    CHECK_STR_EQ(a.bytes, "\x02\"");
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::KillPane, 7, "");
    CHECK_STR_EQ(a.bytes, "\x02x"); // tmux's own y/n confirm gates the kill
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::NewWindow, 7, "");
    CHECK_STR_EQ(a.bytes,
        "\x02"
        "c");
    // Rename: prompt opens pre-filled, C-u clears it, CR commits.
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::RenameWindow, 7, "logs");
    CHECK_STR_EQ(a.bytes, "\x02,\x15logs\r");
    // Control bytes in the value can't inject chords into the prompt.
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::RenameWindow, 7,
        "a\x02x\x1b"
        "b");
    CHECK_STR_EQ(a.bytes,
        "\x02,\x15"
        "axb\r");
    // A remapped prefix (C-a) rides through.
    a = synthesizeAction(PaneContext::RawTmuxAttach, ActionIntent::NewWindow, 7, "", 0x01);
    CHECK_STR_EQ(a.bytes,
        "\x01"
        "c");

    // NativePane: no synthesis — the frontend's own operation applies.
    a = synthesizeAction(PaneContext::NativePane, ActionIntent::SplitSideBySide, 7, "");
    CHECK(a.transport == Transport::None);
    CHECK(a.bytes.empty());
    a = synthesizeAction(PaneContext::NativePane, ActionIntent::RenameWindow, 7, "logs");
    CHECK(a.transport == Transport::None);
}

// Which transport a pane's intents take is read from three facts, and the
// discrimination between a raw attach and a typed -CC is the -CC flag alone:
// a keystroke chord typed into a control-mode client's stdin is protocol
// garbage to it.
TEST(testPaneContextClassification)
{
    PaneContextFacts facts;
    CHECK(classifyPaneContext(facts) == PaneContext::NativePane);

    facts.foregroundName = "vim";
    CHECK_MSG(classifyPaneContext(facts) == PaneContext::NativePane, "a program other than tmux contributes nothing");

    facts.foregroundName = "tmux";
    CHECK_MSG(classifyPaneContext(facts) == PaneContext::RawTmuxAttach, "a plain `tmux attach` in the foreground takes prefix chords");

    facts.isForegroundControlModeTmux = true;
    CHECK_MSG(classifyPaneContext(facts) == PaneContext::NativePane, "a typed `tmux -CC` is never driven by keystrokes");

    facts.isControlModePane = true;
    CHECK_MSG(classifyPaneContext(facts) == PaneContext::ControlMode, "a -CC render-only pane takes control commands");
    facts.isForegroundControlModeTmux = false;
    facts.foregroundName = "";
    CHECK_MSG(classifyPaneContext(facts) == PaneContext::ControlMode, "whatever its process facts say");
}

// ===== launch argv assembly =================================================

static std::string joinArgs(const std::vector<std::string>& argv)
{
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i)
            out.push_back('|');
        out += argv[i];
    }
    return out;
}

TEST(testLaunchLocal)
{
    // Default control args: -CC new-session -A -s main, attach-or-create.
    CHECK_STR_EQ(joinArgs(buildLocalLaunchArguments("/opt/homebrew/bin/tmux", "")), "/opt/homebrew/bin/tmux|-CC|new-session|-A|-s|main");
    CHECK_STR_EQ(std::string(defaultControlArguments), "-CC new-session -A -s main");
    // A bare "tmux" fallback (terminal-launched build) is honored verbatim.
    CHECK_STR_EQ(joinArgs(buildLocalLaunchArguments("tmux", "")), "tmux|-CC|new-session|-A|-s|main");
    // An override replaces the default control string.
    CHECK_STR_EQ(joinArgs(buildLocalLaunchArguments("/usr/bin/tmux", "-CC attach -t work")), "/usr/bin/tmux|-CC|attach|-t|work");
    // Double / leading / trailing spaces in the override can't inject blanks.
    CHECK_STR_EQ(joinArgs(buildLocalLaunchArguments("/usr/bin/tmux", "  -CC   ls  ")), "/usr/bin/tmux|-CC|ls");
    // Empty tmux path → no argv (nothing to exec).
    CHECK(buildLocalLaunchArguments("", "").empty());
}

TEST(testLaunchRemote)
{
    // ssh -t -e none <host> tmux <control args>: the pty + escape-char-drop
    // flags, then the host, then the literal tmux and the control tokens,
    // handed to /bin/sh whatever the account's login shell is.
    CHECK_STR_EQ(joinArgs(buildRemoteLaunchArguments("/usr/bin/ssh", "build.example", "")),
        "/usr/bin/ssh|-t|-e|none|build.example|" + Transport::remoteShellCommand("'tmux' '-CC' 'new-session' '-A' '-s' 'main'"));
    // [user@]host passes through verbatim.
    CHECK_STR_EQ(joinArgs(buildRemoteLaunchArguments("/usr/bin/ssh", "user@host", "-CC attach")),
        "/usr/bin/ssh|-t|-e|none|user@host|" + Transport::remoteShellCommand("'tmux' '-CC' 'attach'"));
    // Leading-dash host (ssh argv injection) is rejected → empty argv.
    CHECK(buildRemoteLaunchArguments("/usr/bin/ssh", "-oProxyCommand=evil", "").empty());
    CHECK(!isSafeSSHHost("-x"));
    CHECK(!isSafeSSHHost(""));
    CHECK(isSafeSSHHost("host"));
    CHECK(isSafeSSHHost("user@host"));
    // Empty ssh path → empty argv.
    CHECK(buildRemoteLaunchArguments("", "host", "").empty());
    CHECK(buildRemoteLaunchArguments("/usr/bin/ssh", "host;bad", "").empty());
    CHECK(buildRemoteLaunchArguments("/usr/bin/ssh", "user@-host", "").empty());
    const auto quoted = buildSSHControlArguments("/usr/bin/ssh", "host", { "tmux", "-S", "/tmp/a b'c", "$0", "$(exit 7)" });
    CHECK(quoted.size() == 6);
    CHECK_STR_EQ(quoted.back(), Transport::remoteShellCommand("'tmux' '-S' '/tmp/a b'\\''c' '$0' '$(exit 7)'"));
}

// ===== window/pane registry =================================================

// A liveness model over a set of "alive" handles, so the tests can drop a handle
// to simulate a weak slot going nil (a closed tab's surface deallocing).
#include <set>
static std::set<std::uintptr_t> g_alive;
static bool aliveProbe(void* /*context*/, std::uintptr_t handle) { return g_alive.count(handle) != 0; }
// pane handle → owning controller handle, for the sibling / exit queries.
#include <map>
static std::map<std::uintptr_t, std::uintptr_t> g_paneOwner;
static std::uintptr_t ownerOfProbe(void* /*context*/, std::uintptr_t pane)
{
    auto it = g_paneOwner.find(pane);
    return it != g_paneOwner.end() ? it->second : 0;
}

TEST(testRegistryDedup)
{
    g_alive.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);

    // First discovery of pane %5 claims the spawn.
    CHECK(reg.reservePaneForSpawn(5));
    // A second discovery (list-panes flood + a %window-add for the same pane)
    // while the first is mid-spawn must back off — this is the duplicate-tab fix.
    CHECK(!reg.reservePaneForSpawn(5));
    // Another pane is independent.
    CHECK(reg.reservePaneForSpawn(6));
    // The first spawn finishes and registers its (live) surface.
    reg.endPaneSpawn(5);
    g_alive.insert(100);
    reg.registerPane(5, 100);
    // Now a re-discovery is deduped by the live registry entry, not the
    // reservation.
    CHECK(!reg.reservePaneForSpawn(5));
    // A pane whose surface deallocs (weak slot nil) can be re-claimed.
    g_alive.erase(100);
    CHECK(reg.reservePaneForSpawn(5));
}

// The registry tiling primitives the frontends call.
TEST(testRegistryTilingPhase1)
{
    g_alive.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);

    // Control-client grid: written by the resize path, read by sibling sync.
    reg.setClientGrid(174, 56);
    CHECK(reg.clientGrid().columns == 174 && reg.clientGrid().rows == 56);

    // paneCountForWindow: @4 is a 2-pane split, @5 a single pane.
    g_alive.insert(100);
    g_alive.insert(101);
    g_alive.insert(102);
    reg.registerPane(8, 100);
    reg.notePaneWindow(8, 4);
    reg.registerPane(12, 101);
    reg.notePaneWindow(12, 4);
    reg.registerPane(3, 102);
    reg.notePaneWindow(3, 5);
    CHECK(reg.paneCountForWindow(4) == 2);
    CHECK(reg.paneCountForWindow(5) == 1);
    CHECK(reg.paneCountForWindow(99) == 0);
    // A pane whose surface deallocs (weak slot nil) drops out of the count.
    g_alive.erase(101);
    CHECK(reg.paneCountForWindow(4) == 1);
    reg.unregisterPane(8);
    CHECK(reg.paneCountForWindow(4) == 0);

    // Window-level spawn dedup: mirrors reservePaneForSpawn at the @N grain.
    CHECK(reg.reserveWindowForSpawn(7));
    CHECK(!reg.reserveWindowForSpawn(7)); // in flight
    reg.endWindowSpawn(7);
    g_alive.insert(200);
    reg.setControllerForWindow(7, 200);
    CHECK(!reg.reserveWindowForSpawn(7)); // a live controller already owns @7
    g_alive.erase(200);
    CHECK(reg.reserveWindowForSpawn(7)); // controller gone → re-claimable

    // Representative-pane remap: repoint @N to a live pane after a reconcile drop.
    reg.mapWindowToPane(9, 50);
    CHECK(reg.paneForWindow(9) == 50);
    reg.updateWindowRepresentativePane(9, 51);
    CHECK(reg.paneForWindow(9) == 51);
    reg.updateWindowRepresentativePane(9, -1); // invalid id is a no-op
    CHECK(reg.paneForWindow(9) == 51);

    // Cached layout ownership, held by the registry.
    CHECK(reg.cachedLayout(4) == nullptr);
    reg.cacheLayout(4, "abcd,80x24,0,0");
    const std::string* cached = reg.cachedLayout(4);
    CHECK(cached != nullptr && *cached == "abcd,80x24,0,0");
    reg.dropCachedLayout(4);
    CHECK(reg.cachedLayout(4) == nullptr);

    // Attach-time active pane: recorded per window, consumed once by the first
    // pane's spawn to pick tmux's real active pane (not just the first discovered).
    CHECK(reg.activePaneForWindow(4) == -1);
    reg.noteActivePane(4, 12);
    CHECK(reg.activePaneForWindow(4) == 12);
    reg.noteActivePane(4, 8); // a later row's active flag overwrites the earlier one
    CHECK(reg.activePaneForWindow(4) == 8);
    reg.dropActivePane(4);
    CHECK(reg.activePaneForWindow(4) == -1);
}

TEST(testRegistryCapacity)
{
    g_alive.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);
    // Fill to the cap with live panes.
    for (int i = 0; i < maxTmuxPanes; ++i) {
        const std::uintptr_t h = 1000 + i;
        g_alive.insert(h);
        reg.registerPane(i, h);
    }
    CHECK(reg.registeredPaneCount() == maxTmuxPanes);
    CHECK(reg.atPaneCapacity());
    // Dead weak slots don't count toward the cap.
    g_alive.erase(1000);
    CHECK(reg.registeredPaneCount() == maxTmuxPanes - 1);
    CHECK(!reg.atPaneCapacity());
}

TEST(testRegistryWindowMaps)
{
    g_alive.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);
    g_alive.insert(200);
    reg.registerPane(5, 200);
    reg.mapWindowToPane(2, 5);
    CHECK(reg.paneForWindow(2) == 5);
    CHECK(reg.paneForWindow(99) == -1);
    CHECK(reg.paneForPaneID(5) == 200);
    // Unregistering the pane drops the @window → %pane entry too.
    reg.unregisterPane(5);
    CHECK(reg.paneForWindow(2) == -1);
    CHECK(reg.paneForPaneID(5) == 0);

    // Controller-for-window with weak pruning.
    g_alive.insert(300);
    reg.setControllerForWindow(2, 300);
    CHECK(reg.controllerForWindow(2) == 300);
    g_alive.erase(300);
    CHECK(reg.controllerForWindow(2) == 0); // weak slot went nil → dropped
    g_alive.insert(301);
    reg.setControllerForWindow(2, 301);
    reg.forgetWindow(2);
    CHECK(reg.controllerForWindow(2) == 0);
}

TEST(testRegistryTabAnchorOrdering)
{
    g_alive.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);

    // No other window yet → standalone placement.
    {
        TabAnchor a = reg.tabAnchorForWindow(5, 0);
        CHECK(a.placement == TabPlacement::None);
        CHECK(a.anchor == 0);
    }
    // Windows @1 (ctrl 10) and @9 (ctrl 90) exist; a new @5 anchors after @1
    // (nearest smaller @N), keeping @N order.
    g_alive.insert(10);
    g_alive.insert(90);
    reg.setControllerForWindow(1, 10);
    reg.setControllerForWindow(9, 90);
    {
        TabAnchor a = reg.tabAnchorForWindow(5, 0);
        CHECK(a.placement == TabPlacement::AfterAnchor);
        CHECK(a.anchor == 10);
    }
    // With only a larger @N live, anchor before it.
    g_alive.erase(10);
    {
        TabAnchor a = reg.tabAnchorForWindow(5, 0);
        CHECK(a.placement == TabPlacement::BeforeAnchor);
        CHECK(a.anchor == 90);
    }
    // The nearest smaller is chosen, not just any smaller: add @3 (ctrl 30) and
    // @1 back; @5 anchors after @3, not @1. (The prior BeforeAnchor query pruned
    // @1's now-dead weak slot, so re-register it along with re-marking it live —
    // re-inserting the handle alone does not restore a pruned map entry.)
    g_alive.insert(10);
    g_alive.insert(30);
    reg.setControllerForWindow(1, 10);
    reg.setControllerForWindow(3, 30);
    {
        TabAnchor a = reg.tabAnchorForWindow(5, 0);
        CHECK(a.placement == TabPlacement::AfterAnchor);
        CHECK(a.anchor == 30);
    }
    // The excluded controller (the window's own) is skipped even if it sorts
    // nearest.
    {
        TabAnchor a = reg.tabAnchorForWindow(5, 30);
        CHECK(a.placement == TabPlacement::AfterAnchor);
        CHECK(a.anchor == 10); // @3 excluded → falls back to @1
    }
}

TEST(testRegistrySiblingsAndExit)
{
    g_alive.clear();
    g_paneOwner.clear();
    WindowRegistry reg;
    reg.setLivenessProbe(aliveProbe, nullptr);

    // Three panes: %1/%2 under controller A (500), %3 under controller B (600).
    g_alive.insert(401);
    g_alive.insert(402);
    g_alive.insert(403);
    g_paneOwner[401] = 500;
    g_paneOwner[402] = 500;
    g_paneOwner[403] = 600;
    reg.registerPane(1, 401);
    reg.registerPane(2, 402);
    reg.registerPane(3, 403);

    // Resizing controller A's grid resizes the siblings not owned by A → only %3.
    std::vector<PaneHandle> sibs = reg.siblingPanesExcept(500, ownerOfProbe, nullptr);
    CHECK(sibs.size() == 1);
    CHECK(sibs.empty() || sibs[0] == 403);

    // Exit snapshots the DISTINCT owning controllers (deduped) and clears maps.
    std::vector<ControllerHandle> owners = reg.takeOwningControllersForExit(ownerOfProbe, nullptr);
    CHECK(owners.size() == 2);
    bool sawA = false, sawB = false;
    for (auto o : owners) {
        sawA = sawA || o == 500;
        sawB = sawB || o == 600;
    }
    CHECK(sawA && sawB);
    CHECK(reg.registeredPaneCount() == 0); // maps cleared
    CHECK(reg.paneForPaneID(1) == 0);
}

// ===== attach state-recovery state machine ==================================

// Drive a scripted UBCAPBEGIN + capture-pane response stream through the
// recovery machine the way the ingest thread does: consumeLine per body line,
// blockEnded on each %begin/%end (or %error) block boundary.

TEST(testRecoveryBasicCapture)
{
    CaptureRecovery rec;
    CHECK(!rec.active());

    // Block 1: the UBCAPBEGIN marker (its own command response).
    std::string marker = std::string(captureBeginPrefix) + "7 4 2 0"; // pane 7, cursor (4,2), retry 0
    CHECK(rec.consumeLine(marker));
    CHECK(rec.active());
    // A non-marker line in idle is not consumed (it's a pane record) — but we're
    // past idle now; the marker block ends, arming Capturing.
    CaptureOutcome o1 = rec.blockEnded();
    CHECK(!o1.didComplete); // marker block end is not a capture completion

    // Block 2: the capture-pane content rows.
    const char* row1 = "hello world";
    const char* row2 = "$ ";
    CHECK(rec.consumeLine(row1));
    CHECK(rec.consumeLine(row2));
    CaptureOutcome o2 = rec.blockEnded();
    CHECK(o2.didComplete);
    CHECK(o2.paneID == 7);
    CHECK(o2.screen.cursorX == 4 && o2.screen.cursorY == 2);
    CHECK(!o2.blank);
    CHECK(!o2.shouldRetry);
    // Each row as tmux printed it; a text capture says nothing of wraps.
    REQUIRE(o2.screen.rows.size() == 2);
    CHECK(o2.screen.rows[0].text == row1 && !o2.screen.rows[0].isWrapped);
    CHECK(o2.screen.rows[1].text == row2 && !o2.screen.rows[1].isWrapped);
    CHECK(!rec.active()); // reset after completion
}

// A marker without a 5th field (or captureBeginMarker's CurrentScreen
// default) reads as CaptureScope::CurrentScreen, the in-place repaint. A "1"
// there reads as FullHistory, the natural-newline-scroll feed instead.
TEST(testRecoveryCaptureScope)
{
    {
        CaptureRecovery rec;
        std::string marker = std::string(captureBeginPrefix) + "7 4 2 0"; // no 5th field
        rec.consumeLine(marker);
        (void)rec.blockEnded();
        rec.consumeLine("row");
        CaptureOutcome o = rec.blockEnded();
        CHECK(o.didComplete);
        CHECK(o.screen.scope == CaptureScope::CurrentScreen);
        CHECK(recoveryFeed(o.screen) == std::vector<std::string> { "\x1b[0m\x1b[2J\x1b[1;1Hrow\x1b[3;5H" });
    }
    {
        CaptureRecovery rec;
        std::string marker = std::string(captureBeginPrefix) + "7 4 2 0 1"; // scope=1 (FullHistory)
        rec.consumeLine(marker);
        (void)rec.blockEnded();
        rec.consumeLine("row");
        CaptureOutcome o = rec.blockEnded();
        CHECK(o.didComplete);
        CHECK(o.screen.scope == CaptureScope::FullHistory);
        CHECK(recoveryFeed(o.screen) == std::vector<std::string> { "\x1b[Hrow\x1b[3;5H" });
    }
}

// A marker whose 6th field is 1 announces rows printed behind their line
// flags (capture-pane -F): the flags come off each row, and a row whose
// flags hold W is one tmux wrapped into the next, whatever other flags sit
// beside it. The rows keep every space after the flags' separator.
TEST(testRecoveryFlaggedRowsCarryTheirWraps)
{
    CaptureRecovery rec;
    rec.consumeLine(std::string(captureBeginPrefix) + "7 4 2 0 1 1");
    (void)rec.blockEnded();
    for (const char* row : { "W 0123456789", "- tail", "WX \x1b[31mred  ", "X wide", "-", "- ", "D  lead" })
        CHECK(rec.consumeLine(row));
    CaptureOutcome o = rec.blockEnded();
    CHECK(o.didComplete);
    CHECK(o.screen.scope == CaptureScope::FullHistory);
    REQUIRE(o.screen.rows.size() == 7);
    const std::vector<std::pair<std::string, bool>> expected {
        { "0123456789", true },
        { "tail", false },
        { "\x1b[31mred  ", true },
        { "wide", false },
        { "", false },
        { "", false },
        { " lead", false },
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_MSG(o.screen.rows[index].text == expected[index].first, ("row " + std::to_string(index) + " text").c_str());
        CHECK_MSG(o.screen.rows[index].isWrapped == expected[index].second, ("row " + std::to_string(index) + " wrap").c_str());
    }

    // Without the field the rows are text alone, a leading W included.
    CaptureRecovery text;
    text.consumeLine(std::string(captureBeginPrefix) + "7 0 0 0 0");
    (void)text.blockEnded();
    text.consumeLine("W 0123");
    CaptureOutcome plain = text.blockEnded();
    REQUIRE(plain.screen.rows.size() == 1);
    CHECK(plain.screen.rows[0].text == "W 0123" && !plain.screen.rows[0].isWrapped);
}

// A flagged capture of an unpainted pane is still blank: the flags are not
// the pane's text.
TEST(testRecoveryFlaggedBlankCaptureRetries)
{
    CaptureRecovery rec;
    rec.consumeLine(std::string(captureBeginPrefix) + "9 0 0 0 0 1");
    (void)rec.blockEnded();
    rec.consumeLine("- ");
    rec.consumeLine("-");
    CaptureOutcome o = rec.blockEnded();
    CHECK(o.blank);
    CHECK(o.shouldRetry);
}

// The current-screen feed resets the pen, clears and writes each row at its
// own row; the full-history feed starts at home, because the clear before it
// leaves the cursor where it was, and separates rows rather than terminating
// them, because a newline after the last row would scroll one row past
// tmux's screen. Both end at tmux's cursor, 1-indexed.
TEST(testRecoveryFeedComposesBothScopes)
{
    RecoveredScreen screen;
    screen.rows = { { "ab", false }, { "cd", false } };
    screen.cursorX = 2;
    screen.cursorY = 1;
    CHECK(recoveryFeed(screen) == std::vector<std::string> { "\x1b[0m\x1b[2J\x1b[1;1Hab\x1b[2;1Hcd\x1b[2;3H" });
    screen.scope = CaptureScope::FullHistory;
    screen.rows.push_back({ "ef", false });
    CHECK(recoveryFeed(screen) == std::vector<std::string> { "\x1b[Hab\r\ncd\r\nef\x1b[2;3H" });
    screen.rows = { { "only", false } };
    screen.cursorX = 0;
    screen.cursorY = 0;
    CHECK(recoveryFeed(screen) == std::vector<std::string> { "\x1b[Honly\x1b[1;1H" });
}

// The feed is cut where the cursor has just reached the row after a wrapped
// one, past that row's separator or position, so the writer marks the row
// above the cursor; nowhere else, and not after a wrapped last row, which has
// no row to join.
TEST(testRecoveryFeedCutsWhereTheRowAfterAWrapBegins)
{
    RecoveredScreen screen;
    screen.rows = { { "aaaa", true }, { "bb", false }, { "cccc", true }, { "dddd", true }, { "e", false } };
    screen.cursorX = 1;
    screen.cursorY = 4;
    CHECK(recoveryFeed(screen)
        == (std::vector<std::string> { "\x1b[0m\x1b[2J\x1b[1;1Haaaa\x1b[2;1H", "bb\x1b[3;1Hcccc\x1b[4;1H", "dddd\x1b[5;1H", "e\x1b[5;2H" }));
    screen.scope = CaptureScope::FullHistory;
    CHECK(recoveryFeed(screen) == (std::vector<std::string> { "\x1b[Haaaa\r\n", "bb\r\ncccc\r\n", "dddd\r\n", "e\x1b[5;2H" }));
    screen.rows = { { "x", false }, { "last", true } };
    CHECK(recoveryFeed(screen) == std::vector<std::string> { "\x1b[Hx\r\nlast\x1b[5;2H" });
}

TEST(testRecoveryBlankRetryPolicy)
{
    // A blank capture under the retry cap schedules a retry with retry+1; at the
    // cap it stops. capture-pane trims trailing whitespace, so a blank pane comes
    // back as just newlines.
    for (int retry = 0; retry <= maxCaptureRetries; ++retry) {
        CaptureRecovery rec;
        std::string marker = std::string(captureBeginPrefix) + "9 0 0 " + std::to_string(retry);
        rec.consumeLine(marker);
        (void)rec.blockEnded(); // arm Capturing
        const char* blankRow = "   "; // whitespace only
        rec.consumeLine(blankRow);
        CaptureOutcome o = rec.blockEnded();
        CHECK(o.didComplete);
        CHECK(o.blank);
        if (retry < maxCaptureRetries) {
            CHECK(o.shouldRetry);
            CHECK(o.nextRetry == retry + 1);
        } else {
            CHECK(!o.shouldRetry); // bounded: no retry past the limit
        }
    }
    // The blank heuristic directly.
    CHECK(CaptureRecovery::isEffectivelyBlank("\n\n  \t\r\n"));
    CHECK(!CaptureRecovery::isEffectivelyBlank("\n x\n"));
}

TEST(testRecoveryMarkerOnlyInIdle)
{
    CaptureRecovery rec;
    // In idle, a line that is not a UBCAPBEGIN marker is not consumed — the
    // caller parses it as a pane record.
    const char* record = "@1 %5 80 24 1 layout sess name";
    CHECK(!rec.consumeLine(record));
    // A truncated marker prefix (shorter than the sentinel) is also not consumed.
    CHECK(!rec.consumeLine("UBCAP"));
}

TEST(testRecoveryLimitAndReset)
{
    CaptureRecovery rec;
    std::string marker = std::string(captureBeginPrefix) + "1 0 0 0";
    rec.consumeLine(marker);
    (void)rec.blockEnded(); // arm Capturing
    // Stream more than the 1 MiB cap; rows past the cap are swallowed (still
    // consumed) but not appended, so the machine doesn't blow memory and the
    // block end still routes.
    std::string bigRow(4096, 'x');
    for (int i = 0; i < 300; ++i) // ~1.2 MiB
        CHECK(rec.consumeLine(bigRow));
    CaptureOutcome o = rec.blockEnded();
    CHECK(o.didComplete);
    CHECK(!o.blank);

    // reset() abandons an in-flight capture so a prior session's rows can't bleed
    // into the next.
    rec.consumeLine(marker);
    CHECK(rec.active());
    rec.reset();
    CHECK(!rec.active());
    // After reset, a content-looking line in idle is treated as a pane record.
    CHECK(!rec.consumeLine("plain row"));
}

UBIQUITTY_TEST_MAIN()
