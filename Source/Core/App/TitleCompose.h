// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/App/TitleCompose.h — one pane's title inputs to the finished title
// of its pane, its tab, its window and its workspace.
//
// The pieces were already Core's: which name wins (`resolveTitleNames`), how a
// directory is spelled (`titleDirectorySegment`), which parts show
// (`resolveTitle`), their order and join (`joinTitle`), and the marks a tab
// label wears (`markTabLabel`). Putting them together is a rule too: pick the
// composition, decide which parts the window's row draws apart from its text,
// fall back to a name when nothing wove, and mark the label. Assembled in each
// frontend, a tab on one platform and a pane listed from another machine
// would each have their own version of it.
//
// `composeTitle` is that assembly. Its inputs are in two records, by who knows
// them. `PaneTitleFacts` is plain facts about one pane, which is what a host
// sends for a pane it lists. `TitleViewerFacts` is what only the reader
// knows: its profile, its fallback name, how its windows draw a title and the
// marks it keeps on the tab. With the viewer's own `[title]` composition, a
// pane listed from another machine and a pane in a local window read by one
// rule and the viewer's settings.
//

#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Session/ProcessState.h"
#include "Core/Session/SessionPolicy.h"
#include "Core/Title/Directory.h"
#include "Core/Title/Fit.h"
#include "Core/Title/Format.h"
#include "PAL/Process.h"
#include "PAL/RemoteTarget.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::App {

/// How a frontend draws a window's title.
enum class WindowTitleForm : std::uint8_t {
    /// A title row with components: the dimensions and a program's status are
    /// drawn beside the text, which joins with a bare dash the row spaces.
    RowWithComponents,
    /// One string, as a window manager's title bar takes it: every part is
    /// woven in, joined with spaces.
    PlainText,
};

/// One pane's title facts: what its program, its host and the people naming
/// things on that host have said about it.
///
/// What a host knows and sends for a pane, whether or not the reader has it
/// open; nothing here depends on who reads it. Owned strings, so a host can
/// decode a listing straight into it and Swift can fill it without a lifetime
/// rule. Any may be empty, which is how a fact nobody stated is spelled.
struct PaneTitleFacts {
    /// What the program set as its title (OSC 0/2).
    std::string programTitle;
    /// The pane's working directory as reported, not yet shortened.
    std::string currentDirectory;
    /// The home directory `currentDirectory` is abbreviated against: the
    /// pane's user's, on the pane's host.
    std::string homeDirectory;
    /// The foreground process chain, and the same with the leaf's arguments.
    std::string processChain;
    std::string processChainWithArguments;
    /// The session leader shell's name.
    std::string shellCommandName;
    /// The terminal's name on its host ("ttys003").
    std::string ttyName;
    /// The label a program gave its status.
    std::string programStatus;
    /// The program holding the terminal, by its own name; empty to read it
    /// off the end of the process chain.
    std::string foregroundName;
    /// Whether a program fills the screen: the alternate screen is up.
    bool isFullScreen = false;
    /// The pane's grid.
    int columns = 0;
    int rows = 0;
    /// How the session ended, once it has.
    Title::SessionEnd end;

    /// The name put on this one pane.
    std::string paneLabel;
    /// The custom titles put on its tab and on its window, empty for automatic
    /// (`Session::CustomTitles`).
    std::string tabTitle;
    std::string windowTitle;
    /// What tmux calls the window.
    std::string tmuxWindowName;

    /// The host the pane's session lives on; empty for a plain local shell.
    std::string hostName;
};

/// What the reader of a title adds to a pane's facts: its own profile, how
/// its windows draw a title, and the state it keeps about the tab.
///
/// Filled by the frontend composing the title, never sent by a host.
struct TitleViewerFacts {
    /// The applied profile's name.
    std::string profileName;
    /// What names the surface when no part produced text.
    std::string fallbackName;
    /// How this frontend draws a window's title.
    WindowTitleForm windowTitleForm = WindowTitleForm::RowWithComponents;
    /// Whether pane headers name their host: true only where the panes shown
    /// together live in more than one place.
    bool doPaneHeadersNameHost = false; // bool-ok: names the one condition it reports
    /// The host the reader's window names in its own chrome, beside its
    /// title: the place every tab of the window is in, when they are all in
    /// one. Empty when the chrome names none, which is also how a reader with
    /// no window around its tabs, a listing, spells it.
    std::string windowHostName;
    /// Whether the tabs of the reader's window live in more than one place:
    /// tabs of two hosts, or a host's tab beside a plain shell's
    /// (`Session::PaneHostMix` over the window's tabs).
    bool doTabsMixPlaces = false; // bool-ok: names the one condition it reports
    /// The tab's pin mark.
    bool isPinned = false; // bool-ok: names the one state it reports
};

/// A finished title and the parts a title row draws beside its text.
struct ComposedTitle {
    /// The text drawn in the title row or label.
    std::string title;
    /// The spaced text supplied to system window identity surfaces.
    std::string systemTitle;
    /// Whether no configured title part supplied text and the fallback names it.
    bool usesFallbackName = false;
    /// The directory segment the text opens with, or empty.
    std::string lead;
    /// The columns×rows readout, where the surface draws it apart from its
    /// text; empty where it is woven in or not shown.
    std::string dimensions;
    /// A program's status label, where the surface draws it apart from its
    /// text; empty where it is woven in or not shown.
    std::string programStatus;
    /// UTF-8 byte boundary in `title` where a title row draws programStatus.
    std::size_t programStatusInsertionByteOffset = 0;
    /// What a tab's or pane's marks put before the label.
    std::string markPrefix;
    /// Shorter versions of a tab's or pane's label, longest first, for one
    /// too narrow for the whole (`Title::fittingSteps`); empty for a window's
    /// title, a typed name, and a label that fell back to a name.
    std::vector<Title::FittingStep> fitting;
};

/// Whether a process chain is only the hop to a host, and so says nothing a
/// chrome naming that host does not.
///
/// The chain qualifies when it is one command, with no program beside it, that
/// leads to the host: `ssh host`, `ssh -p 2222 user@host.local`,
/// `mosh host`. A program beside the hop is still worth naming, and a
/// hop to another machine is the only place that one is said. Hosts compare
/// without case and without a domain on either side, since a chip may name the
/// short name of a machine the command spelled in full.
///
/// - Parameters:
///   - chainWithArguments: The chain with the leaf's arguments, as the walk
///     formats it.
///   - hostName: The host the chrome names; empty matches nothing.
/// - Returns: True when the chain is the hop to `hostName` and nothing else.
[[nodiscard]] inline bool isChainTheHopToHost(std::string_view chainWithArguments, std::string_view hostName)
{
    if (hostName.empty() || chainWithArguments.find(" ▸ ") != std::string_view::npos
        || chainWithArguments.find(" ◂ ") != std::string_view::npos)
        return false;
    const PAL::Process::RemoteTarget target = PAL::Process::remoteTargetForCommand(chainWithArguments);
    if (!target.isRemote())
        return false;
    const auto shortName = [](std::string_view name) { return name.substr(0, name.find('.')); };
    const auto sameName = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
            if (lower(left[index]) != lower(right[index]))
                return false;
        }
        return true;
    };
    return sameName(target.name, hostName) || sameName(shortName(target.name), shortName(hostName));
}

/// Whether a title leaves its pane's directory out for what holds the
/// terminal: a program the reader listed, whose terminal shows another place
/// or none (a multiplexer, a remote shell), or with
/// `directory_hidden_in_full_screen`, any program that fills the screen.
///
/// - Parameters:
///   - pane: The pane's facts.
///   - composition: The reader's composition for the surface.
/// - Returns: Whether the directory is left out.
[[nodiscard]] inline bool isDirectoryHidden(const PaneTitleFacts& pane, const Preferences::Effective::TitleComposition& composition)
{
    if (composition.hidesDirectoryInFullScreen && pane.isFullScreen)
        return true;
    std::string_view foreground = pane.foregroundName;
    if (foreground.empty())
        foreground = Title::foregroundOfChain(pane.processChain.empty() ? pane.processChainWithArguments : pane.processChain);
    // A script names itself before its interpreter ("<script> ‹ python3"),
    // and a chain with arguments spells the program first.
    foreground = foreground.substr(0, foreground.find(" ‹ "));
    foreground = foreground.substr(0, foreground.find(' '));
    return Session::isProcessListed(foreground, composition.directoryHiddenFor);
}

/// Composes the title one surface shows for a pane.
///
/// A custom title on the window or the tab fills the app-title segment and
/// takes the process segments' place (`Session::resolveTitleNames`); every
/// other part is resolved from the composition as it is without one, so the
/// directory, the columns×rows readout, the status and the marks stay. Only a
/// pane's label replaces its title, components and all. Otherwise the parts
/// are resolved from the composition and joined in Core's order.
///
/// A window's row draws its dimensions and a program's status as components
/// beside the text, so a window's text carries neither and they come back
/// apart; it joins with a bare dash, which its title view spaces as it draws.
/// A frontend whose window title is one string says so
/// (`WindowTitleForm::PlainText`) and gets every part woven in with spaces.
/// A tab and a pane are plain text: they join every part with spaces and wear
/// the marks. A window wears none.
///
/// A host is named once per place a reader looks. A window whose tabs all
/// live on one host names it in its chrome, so none of its tab labels repeat
/// it, and the window's own title leaves out a process that is only the hop
/// to that host (``isChainTheHopToHost``); a tab carries its host where the
/// window's tabs live in more than one place (`doTabsMixPlaces`), which is
/// when the name tells the tabs apart, and where the window's chrome names no
/// host or another one (`windowHostName`), which is when the label is the only
/// place that says it. A pane's header names its host only where
/// `doPaneHeadersNameHost` says the panes shown together are in different
/// places.
///
/// A pane's label is the whole of its header: it is marked with its host where
/// the panes shown together are in different places, and with nothing else.
///
/// - Parameters:
///   - pane: The pane's facts, as its host states them.
///   - viewer: What the reader adds: its profile, its fallback and the tab's
///     marks.
///   - composition: The viewer's `[title]` composition for this surface.
///   - surface: Which surface is being titled.
/// - Returns: The title and the components drawn beside it.
[[nodiscard]] inline ComposedTitle composeTitle(const PaneTitleFacts& pane, const TitleViewerFacts& viewer, const Preferences::Effective::TitleComposition& composition, Session::TitleSurface surface)
{
    // A tab's and a pane's label wear marks; a window's title never does.
    const bool doesWearMarks = surface != Session::TitleSurface::Window;
    const bool doesRowDrawComponents = surface == Session::TitleSurface::Window && viewer.windowTitleForm == WindowTitleForm::RowWithComponents;
    const std::string directory = composition.showsDirectory && !pane.currentDirectory.empty() && !isDirectoryHidden(pane, composition)
        ? Title::titleDirectorySegment(pane.currentDirectory, pane.homeDirectory, composition.showsDirectoryFullPath ? Title::PathFormat::FullPath : Title::PathFormat::BasenameOnly)
        : std::string();
    const Session::ResolvedTitleName names = Session::resolveTitleNames(
        Session::TitleNames {
            .tabTitle = pane.tabTitle,
            .windowTitle = pane.windowTitle,
            .tmuxWindowName = pane.tmuxWindowName,
            .oscTitle = pane.programTitle,
            .paneLabel = pane.paneLabel,
        },
        surface);

    Title::TabLabelMarks marks;
    const bool doesTabNameHost = surface == Session::TitleSurface::Tab && names.replacement.empty()
        && (viewer.doTabsMixPlaces || viewer.windowHostName != pane.hostName);
    if (doesTabNameHost || (surface == Session::TitleSurface::Pane && viewer.doPaneHeadersNameHost))
        marks.hostName = pane.hostName;
    // The pin is the tab's state, so it leads whatever the tab is called: a
    // name someone typed says what the tab is, not whether it stays.
    marks.isPinned = doesWearMarks && viewer.isPinned;

    ComposedTitle composed;
    if (!names.replacement.empty()) {
        if (!doesWearMarks || (marks.hostName.empty() && !marks.isPinned)) {
            composed.title = names.replacement;
            composed.systemTitle = composed.title;
            return composed;
        }
        // A name that already says the host is not marked with it again.
        marks = Title::marksForLabel(names.replacement, marks);
        composed.title = Title::markTabLabel(names.replacement, marks);
        composed.systemTitle = composed.title;
        composed.markPrefix = Title::tabLabelMarkPrefix(marks);
        return composed;
    }

    const std::string& shownChain = composition.showsProcessArguments ? pane.processChainWithArguments : pane.processChain;
    const Title::TitleShowFlags show {
        .showsDirectory = composition.showsDirectory,
        .showsAppTitle = composition.showsAppTitle || names.isCustomName,
        // A custom title names what runs there, which is what the process
        // segments describe; beside it they would contradict the name.
        .showsShellCommand = composition.showsShellCommand && !names.isCustomName,
        // A window whose chrome names its host does not say again how it got
        // there. The hop is judged on the chain the title would show, the only
        // one a frontend hands over.
        .showsProcess = composition.showsProcess && !names.isCustomName
            && !(surface == Session::TitleSurface::Window && isChainTheHopToHost(shownChain, viewer.windowHostName)),
        .showsProfileName = composition.showsProfileName,
        .showsTTYName = composition.showsTTYName,
        .showsDimensions = composition.showsDimensions,
        .showsProgramStatus = composition.showsProgramStatus,
    };
    const Title::TitleSegments segments {
        .baseTitle = composition.baseTitle,
        .lead = directory,
        .osc = names.appTitle,
        .shellCommand = pane.shellCommandName,
        .chain = shownChain,
        .profileName = viewer.profileName,
        .ttyName = pane.ttyName,
        .programStatus = pane.programStatus,
    };
    const Title::ResolvedTitle parts = Title::resolveTitle(show, segments, pane.columns, pane.rows, pane.end);
    Title::ResolvedTitle textParts = parts;
    if (doesRowDrawComponents) {
        // Woven in as well, the row would read each twice: once in the title
        // and once dim beside it.
        composed.dimensions = parts.dimensions;
        composed.programStatus = parts.programStatus;
        textParts.dimensions.clear();
        textParts.programStatus.clear();
        composed.programStatusInsertionByteOffset = Title::joinedPrefixLengthBefore(textParts, Title::TitlePart::ProgramStatus, PAL::Process::TitleSeparator::Bare);
    }
    const std::string woven = Title::joinTitle(textParts, doesRowDrawComponents ? PAL::Process::TitleSeparator::Bare : PAL::Process::TitleSeparator::Spaced);
    composed.usesFallbackName = woven.empty();
    const std::string& named = woven.empty() ? viewer.fallbackName : woven;
    // Empty whenever nothing wove: a directory that shows is itself a part.
    composed.lead = directory;
    if (!doesWearMarks) {
        composed.title = named;
        composed.systemTitle = doesRowDrawComponents && !woven.empty()
            ? Title::joinTitle(textParts, PAL::Process::TitleSeparator::Spaced)
            : named;
        return composed;
    }
    // A label that fell back to the host's name already says it.
    marks = Title::marksForLabel(named, marks);
    composed.title = Title::markTabLabel(named, marks);
    composed.systemTitle = composed.title;
    composed.dimensions = parts.dimensions;
    composed.markPrefix = Title::tabLabelMarkPrefix(marks);
    if (!composed.usesFallbackName)
        composed.fitting = Title::fittingSteps(textParts, PAL::Process::TitleSeparator::Spaced, marks);
    return composed;
}

/// A run of titles, named so it crosses into Swift.
using TitleSpan = std::span<const std::string>;

/// A level of what a host holds, outermost first.
enum class ListedLevel : std::uint8_t {
    Workspace,
    Window,
    Tab,
    Pane,
};

/// What a listing shows for one thing: what it is called, and its handle.
struct ListedName {
    /// The text displayed.
    std::string title;
    /// The label drawn beside the title as a chip; empty when it has none, or
    /// when the label is the title.
    std::string labelChip;
};

/// Reports whether a label put on a thing of this level is its title.
///
/// Names and labels are two things. A name or title is what a listing
/// displays; a label is the handle a target addresses the thing by
/// (`Identity::LabelBook`). A workspace has a name and a tab has a title, so
/// their labels are shown beside them, never instead. A window and a pane
/// have no name of their own: a label put on one is what it is called, in a
/// pane's header and a window's row as in a listing.
///
/// - Parameter level: The level.
/// - Returns: True for a window and a pane.
[[nodiscard]] constexpr bool doesLabelReplaceTitle(ListedLevel level)
{
    return level == ListedLevel::Window || level == ListedLevel::Pane;
}

/// Decides what a listing displays for one thing and what it draws beside it.
///
/// - Parameters:
///   - level: What the thing is.
///   - title: What it is called without its label: a workspace's name, a
///     tab's finished title, and for a window or a pane the title composed
///     for it.
///   - label: Its label, or empty.
/// - Returns: The title and the chip, by the rule ``doesLabelReplaceTitle``
///   states.
[[nodiscard]] inline ListedName listedName(ListedLevel level, std::string_view title, std::string_view label)
{
    if (!doesLabelReplaceTitle(level))
        return { std::string(title), std::string(label) };
    return { std::string(label.empty() ? title : label), {} };
}

/// What a window shows in a listing of sessions.
struct ListedWindowTitle {
    /// The window's title.
    std::string title;
    /// Whether its tabs are listed beneath it. False for a window of one tab:
    /// that tab's title is the window's, and a row repeating it says nothing.
    bool hasTabRows = false; // bool-ok: names the one condition it reports
};

/// Titles a window in a listing from its tabs.
///
/// A name put on the window is its title. Without one a window is called what
/// its active tab is called. A window of one tab lists no tab beneath it.
///
/// - Parameters:
///   - windowLabel: The name put on the window, or empty.
///   - tabTitles: Its tabs' finished titles, in order.
///   - activeTab: The index of its active tab.
/// - Returns: The title and whether the tabs get rows.
[[nodiscard]] inline ListedWindowTitle listedWindowTitle(std::string_view windowLabel, TitleSpan tabTitles, std::size_t activeTab)
{
    ListedWindowTitle listed;
    listed.hasTabRows = tabTitles.size() > 1;
    const std::string_view activeTabTitle = tabTitles.empty() ? std::string_view() : std::string_view(tabTitles[activeTab < tabTitles.size() ? activeTab : 0]);
    listed.title = listedName(ListedLevel::Window, activeTabTitle, windowLabel).title;
    return listed;
}

/// Titles a workspace in a listing.
///
/// A workspace nobody named is called by its default name, which prefers a
/// word a person put on what it has in front and then its folder, to its
/// window's composed title: that can lead with the host and the whole path,
/// which a listing cuts before the part that tells two workspaces apart.
///
/// - Parameters:
///   - workspaceName: The workspace's name, or empty.
///   - defaultName: What it is called unnamed
///     (`Session::TreeWorkspace::defaultName`), or empty.
///   - windowTitles: Its windows' titles, as ``listedWindowTitle`` gave them.
///   - activeWindow: The index of its active window.
/// - Returns: The name, else the default name, else the active window's
///   title, else empty.
[[nodiscard]] inline std::string listedWorkspaceTitle(std::string_view workspaceName, std::string_view defaultName, TitleSpan windowTitles, std::size_t activeWindow)
{
    if (!workspaceName.empty())
        return std::string(workspaceName);
    if (!defaultName.empty())
        return std::string(defaultName);
    if (windowTitles.empty())
        return {};
    return windowTitles[activeWindow < windowTitles.size() ? activeWindow : 0];
}

} // namespace Ubiquitty::App
