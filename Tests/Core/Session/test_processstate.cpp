// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sessionprocessstate — pins the process/title reducer: applyProcessEvent()
// folds typed events into the value snapshot and reports render-visible change, and
// resolveAppTitle() encodes the tmux-rename > OSC-title precedence once. Driven
// over the event sequences the window controller and daemon probe produce
// (seed-before-first-poll, poll-then-OSC, OSC-then-rename, transient-chain
// churn), off any Terminal/AppKit.

#include "Core/Session/ProcessState.h"

#include <cstdio>

using namespace Ubiquitty;
using namespace Ubiquitty::Session;

// Designated, not positional: the event is an aggregate that grows, and a
// positional build re-binds every field after the one inserted.
static ProcessEvent seed(const char* name, uint64_t tick)
{
    return { .kind = ProcessEventKind::LaunchSeed, .text = name, .tick = tick };
}
static ProcessEvent poll(uint32_t pgid, const char* name, const char* cwd, const char* chain, uint64_t tick, const char* chainWithArguments = "")
{
    return { .kind = ProcessEventKind::PollResult, .foregroundProcessGroup = pgid, .foregroundName = name, .currentDirectory = cwd, .processChain = chain, .tick = tick, .processChainWithArguments = chainWithArguments };
}
static ProcessEvent osc(const char* title, uint64_t tick)
{
    return { .kind = ProcessEventKind::OSCTitle, .text = title, .tick = tick };
}
static ProcessEvent tmuxRename(const char* windowName, uint64_t tick)
{
    return { .kind = ProcessEventKind::TmuxRename, .text = windowName, .tick = tick };
}

TEST(sessionprocessstate)
{

    // --- Seed before the first poll: the launch basename is held; nothing else
    //     resolves yet, so the app-title segment is None (caller falls back). ---
    {
        ProcessState s;
        CHECK(applyProcessEvent(s, seed("zsh", 1)));
        CHECK(s.launchSeed == "zsh");
        CHECK(s.processChain.empty() && s.foregroundName.empty());
        CHECK(resolveAppTitle(s, TitleSurface::Window).source == TitleSource::None);
        CHECK(s.generation == 1 && s.lastChangeTick == 1);
    }

    // --- Poll then OSC: both retained; the window app-title is the OSC title. ---
    {
        ProcessState s;
        CHECK(applyProcessEvent(s, poll(4321, "vim", "/tmp", "zsh ▸ vim", 2)));
        CHECK(s.foregroundProcessGroup == 4321 && s.processChain == "zsh ▸ vim");
        CHECK(s.lastPollTick == 2);
        CHECK(applyProcessEvent(s, osc("editing README", 3)));
        ResolvedAppTitle window = resolveAppTitle(s, TitleSurface::Window);
        CHECK(window.source == TitleSource::OSCTitle && window.text == "editing README");
    }

    // --- OSC then tmux rename: the tab shows the window name (rename wins), the
    //     window title still shows the pane's OSC title (rename is tab-only). ---
    {
        ProcessState s;
        applyProcessEvent(s, osc("pane title", 4));
        applyProcessEvent(s, tmuxRename("build", 5));
        ResolvedAppTitle tab = resolveAppTitle(s, TitleSurface::Tab);
        CHECK(tab.source == TitleSource::TmuxRename && tab.text == "build");
        ResolvedAppTitle window = resolveAppTitle(s, TitleSurface::Window);
        CHECK(window.source == TitleSource::OSCTitle && window.text == "pane title");
    }

    // --- A cleared tmux window name lets the tab fall back to the pane OSC. ---
    {
        ProcessState s;
        applyProcessEvent(s, osc("pane title", 6));
        applyProcessEvent(s, tmuxRename("build", 7));
        CHECK(applyProcessEvent(s, tmuxRename("", 8))); // %window-renamed to empty
        ResolvedAppTitle tab = resolveAppTitle(s, TitleSurface::Tab);
        CHECK(tab.source == TitleSource::OSCTitle && tab.text == "pane title");
    }

    // --- Transient-chain churn: a transient chain sampled then the
    //     settled shell — the newest poll always wins, no latch. ---
    {
        ProcessState s;
        applyProcessEvent(s, poll(1000, "direnv", "/p", "zsh ▸ direnv", 9)); // transient
        CHECK(applyProcessEvent(s, poll(1000, "zsh", "/p", "zsh", 10))); // settled back
        CHECK(s.processChain == "zsh" && s.foregroundName == "zsh");
    }

    // --- Change detection: re-applying an identical event reports no change and
    //     leaves generation fixed (a poll still restamps lastPollTick). ---
    {
        ProcessState s;
        CHECK(applyProcessEvent(s, osc("t", 11)));
        uint64_t generation = s.generation;
        CHECK(!applyProcessEvent(s, osc("t", 12))); // same title → no visible change
        CHECK(s.generation == generation);
        CHECK(!applyProcessEvent(s, poll(0, "", "", "", 13))); // identical facts, but…
        CHECK(s.lastPollTick == 13); // …a poll always restamps its tick
        CHECK(s.generation == generation);
    }
}

// Which name a surface answers to, when several have been set.
TEST(resolveTitleNamesRanksTheCustomTitles)
{
    // A tab's custom title fills the tab's name segment and says nothing about
    // the window it sits in; neither is a replacement of the whole title.
    TitleNames names;
    names.tabTitle = "deploy";
    names.windowTitle = "the window";
    names.oscTitle = "vim";
    const ResolvedTitleName tab = resolveTitleNames(names, TitleSurface::Tab);
    CHECK(tab.replacement.empty() && tab.appTitle == "deploy" && tab.isCustomName);
    const ResolvedTitleName window = resolveTitleNames(names, TitleSurface::Window);
    CHECK_MSG(window.replacement.empty() && window.appTitle == "the window", "the tab's own name does not reach its window");
    CHECK_MSG(window.isCustomName, "a name a person typed is not hidden by a setting about titles a program sets");

    // The tab follows its window's name until it has its own.
    TitleNames windowNamed;
    windowNamed.windowTitle = "the window";
    windowNamed.oscTitle = "vim";
    const ResolvedTitleName follows = resolveTitleNames(windowNamed, TitleSurface::Tab);
    CHECK(follows.replacement.empty() && follows.appTitle == "the window" && follows.isCustomName);

    // A tmux window's name outranks the focused pane's own OSC title on a tab,
    // so a rename survives the pane's next prompt redraw; a custom title
    // outranks both.
    TitleNames tmuxNamed;
    tmuxNamed.tmuxWindowName = "build";
    tmuxNamed.oscTitle = "vim";
    CHECK(resolveTitleNames(tmuxNamed, TitleSurface::Tab).appTitle == "build");
    CHECK_MSG(resolveTitleNames(tmuxNamed, TitleSurface::Window).appTitle == "vim", "the window keeps the program's own title");
    tmuxNamed.tabTitle = "mine";
    CHECK(resolveTitleNames(tmuxNamed, TitleSurface::Tab).appTitle == "mine");

    // Nothing set is the ordinary case, and empty is automatic.
    TitleNames plain;
    plain.oscTitle = "vim";
    const ResolvedTitleName bare = resolveTitleNames(plain, TitleSurface::Tab);
    CHECK(bare.replacement.empty());
    CHECK(bare.appTitle == "vim");
    CHECK(!bare.isCustomName);
}

// A pane among several answers to its own names, never to its tab's or window's.
TEST(resolveTitleNamesNamesEachPaneByItsOwn)
{
    const auto paneOf = [](std::string_view oscTitle, std::string_view paneLabel) {
        TitleNames names;
        names.tabTitle = "build";
        names.windowTitle = "the window";
        names.tmuxWindowName = "tmux window";
        names.oscTitle = oscTitle;
        names.paneLabel = paneLabel;
        return names;
    };
    const ResolvedTitleName editor = resolveTitleNames(paneOf("nvim", {}), TitleSurface::Pane);
    const ResolvedTitleName history = resolveTitleNames(paneOf("git", {}), TitleSurface::Pane);
    CHECK_MSG(editor.replacement.empty() && history.replacement.empty(), "a name given to the tab or window replaced a pane's title");
    CHECK_MSG(editor.appTitle == "nvim" && history.appTitle == "git", "a pane did not show its own program's title");
    CHECK(!editor.isCustomName && !history.isCustomName);

    // The tab itself still answers to the name it was given.
    CHECK(resolveTitleNames(paneOf("nvim", {}), TitleSurface::Tab).appTitle == "build");

    // A label put on one pane names that pane, and only that one.
    const ResolvedTitleName labelled = resolveTitleNames(paneOf("nvim", "editor"), TitleSurface::Pane);
    CHECK(labelled.replacement == "editor");
    CHECK_MSG(resolveTitleNames(paneOf("git", {}), TitleSurface::Pane).appTitle == "git", "a pane took its neighbour's label");
    CHECK_MSG(resolveTitleNames(paneOf("nvim", "editor"), TitleSurface::Tab).appTitle == "build", "a pane's label reached its tab");
    TitleNames unnamedTab;
    unnamedTab.oscTitle = "nvim";
    unnamedTab.paneLabel = "editor";
    CHECK_MSG(resolveTitleNames(unnamedTab, TitleSurface::Window).replacement.empty(), "a pane's label reached its window");
}

// What a typed title is kept as, on every surface and from every peer.
TEST(aTypedCustomTitleIsKeptTrimmedBoundedAndFreeOfControls)
{
    CHECK(normalizedCustomTitle("").empty());
    CHECK_MSG(normalizedCustomTitle("   \t \n").empty(), "a name of spaces is no name: automatic again");
    CHECK(normalizedCustomTitle("  deploy  ") == "deploy");
    CHECK_MSG(normalizedCustomTitle("two\nlines") == "two lines", "a pasted line break reads as a space");
    CHECK_MSG(normalizedCustomTitle("a\x1b]0;evil\x07"
                                    "b")
            == "a]0;evilb",
        "an escape cannot ride a title into a surface");
    CHECK_MSG(normalizedCustomTitle("x\xC2\x9By") == "xy", "a C1 control is dropped");
    CHECK(normalizedCustomTitle("Build \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == "Build \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    CHECK_MSG(normalizedCustomTitle("bad\xFF"
                                    "byte")
            == "bad\xEF\xBF\xBD"
               "byte",
        "ill-formed UTF-8 is repaired");

    // Bounded on a character boundary.
    std::string longTitle(maxCustomTitleBytes - 1, 'a');
    longTitle += "\xE6\x97\xA5";
    const std::string kept = normalizedCustomTitle(longTitle);
    CHECK(kept.size() == maxCustomTitleBytes - 1);
    CHECK(kept.find('\xE6') == std::string::npos);
    CHECK(customTitleFromTyped(std::string(" x ")) == "x");
}

UBIQUITTY_TEST_MAIN()
