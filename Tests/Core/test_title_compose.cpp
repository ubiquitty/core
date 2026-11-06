// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_title_compose.cpp — App::composeTitle, the one assembly of a pane's
// title inputs into the title its pane, tab and window show, and the listing
// rule that a window of one tab is called what that tab is called.
//
// The pieces it assembles are pinned in their own tests (test_titleformat,
// test_titledirectory, test_titletags, test_title_composition). What is pinned
// here is the assembly: which parts a window draws apart from its text, which
// join each surface uses, which marks each wears, what a replacing name does
// to them, and the fallback.

#include "Core/App/TitleCompose.h"
#include "Core/Session/HealthBadge.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using App::ComposedTitle;
using Preferences::Effective::TitleComposition;
using Session::TitleSurface;

namespace {

TitleComposition showing(bool directory, bool appTitle, bool process, bool dimensions, bool programStatus) // bool-ok: a fixture, each flag named at its one definition
{
    return TitleComposition {
        .showsDirectory = directory,
        .showsDirectoryFullPath = false,
        .showsAppTitle = appTitle,
        .showsProcess = process,
        .showsProcessArguments = false,
        .showsShellCommand = false,
        .showsTTYName = false,
        .showsProfileName = false,
        .showsDimensions = dimensions,
        .showsProgramStatus = programStatus,
        .baseTitle = {},
        .directoryHiddenFor = {},
        .hidesDirectoryInFullScreen = false,
    };
}

/// A pane's facts and its reader's, as one value a case edits.
struct TitleInputs {
    App::PaneTitleFacts pane;
    App::TitleViewerFacts viewer;
};

ComposedTitle compose(const TitleInputs& inputs, const Preferences::Effective::TitleComposition& composition, TitleSurface surface)
{
    return App::composeTitle(inputs.pane, inputs.viewer, composition, surface);
}

TitleInputs editing()
{
    TitleInputs inputs;
    inputs.pane.programTitle = "vim";
    inputs.pane.currentDirectory = "/Users/x/proj";
    inputs.pane.homeDirectory = "/Users/x";
    inputs.pane.processChain = "vim";
    inputs.pane.processChainWithArguments = "vim notes.txt";
    inputs.pane.programStatus = "Working";
    inputs.pane.columns = 80;
    inputs.pane.rows = 24;
    inputs.viewer.fallbackName = "Ubiquitty";
    return inputs;
}

} // namespace

TEST(aWindowDrawsItsDimensionsAndStatusBesideItsText)
{
    const ComposedTitle window = compose(editing(), showing(true, true, false, true, true), TitleSurface::Window);
    CHECK_MSG(window.title == "proj—vim", window.title.c_str());
    CHECK_MSG(window.systemTitle == "proj — vim", window.systemTitle.c_str());
    CHECK(window.lead == "proj");
    CHECK(window.dimensions == "80×24");
    CHECK(window.programStatus == "Working");
    CHECK(window.programStatusInsertionByteOffset == std::string("proj—vim").size());
    CHECK_MSG(window.markPrefix.empty(), "a window title wears no marks");
    CHECK(window.systemTitle.find("80×24") == std::string::npos);
    CHECK(window.systemTitle.find("Working") == std::string::npos);

    const ComposedTitle withoutAppTitle = compose(editing(), showing(true, false, true, true, true), TitleSurface::Window);
    CHECK_STR_EQ(withoutAppTitle.title, "proj—vim");
    CHECK(withoutAppTitle.programStatusInsertionByteOffset == std::string("proj").size());

    // Marks a tab would wear leave a window alone.
    TitleInputs marked = editing();
    marked.pane.hostName = "build-box";
    marked.viewer.isPinned = true;
    CHECK(compose(marked, showing(true, true, false, true, true), TitleSurface::Window).title == "proj—vim");
    CHECK(compose(marked, showing(true, true, false, true, true), TitleSurface::Window).systemTitle == "proj — vim");
}

// A window whose chip names its host does not also say how it got there: a
// chain that is only the hop to that host leaves the window's title, whatever
// options or login the command spelled. A hop elsewhere, a program beside the
// hop, and a tab's label all keep it. A frontend hands over only the chain the
// title shows: the destination without its flags, or the whole argv when the
// reader asks for arguments.
TEST(aWindowLeavesOutTheHopToTheHostItsChipNames)
{
    const TitleComposition composition = showing(true, false, true, false, false);
    TitleInputs hop = editing();
    hop.pane.programTitle.clear();
    hop.pane.processChain = "ssh host.local";
    CHECK_STR_EQ(compose(hop, composition, TitleSurface::Window).title, "proj—ssh host.local");

    hop.viewer.windowHostName = "host";
    CHECK_STR_EQ(compose(hop, composition, TitleSurface::Window).title, "proj");
    hop.viewer.windowHostName = "HOST.local";
    CHECK_STR_EQ(compose(hop, composition, TitleSurface::Window).title, "proj");

    TitleComposition withArguments = composition;
    withArguments.showsProcessArguments = true;
    TitleInputs spelled = hop;
    spelled.pane.processChain.clear();
    spelled.pane.processChainWithArguments = "ssh -p 2222 user@host.local";
    CHECK_STR_EQ(compose(spelled, withArguments, TitleSurface::Window).title, "proj");

    TitleInputs elsewhere = hop;
    elsewhere.viewer.windowHostName = "laptop";
    CHECK_STR_EQ(compose(elsewhere, composition, TitleSurface::Window).title, "proj—ssh host.local");

    TitleInputs beside = hop;
    beside.pane.processChain = "ssh host.local ◂ claude";
    CHECK_STR_EQ(compose(beside, composition, TitleSurface::Window).title, "proj—ssh host.local ◂ claude");

    const ComposedTitle tab = compose(hop, composition, TitleSurface::Tab);
    CHECK_MSG(tab.title.find("ssh") != std::string::npos, tab.title.c_str());

    CHECK(App::isChainTheHopToHost("mosh host", "host"));
    CHECK(!App::isChainTheHopToHost("vim notes.txt", "host"));
    CHECK(!App::isChainTheHopToHost("ssh host", ""));
}

TEST(aFallbackHostNameIsDistinguishedFromAGenuineTitleWithTheSameText)
{
    TitleInputs inputs;
    inputs.viewer.fallbackName = "Studio";
    const TitleComposition composition = showing(false, true, false, false, false);
    const ComposedTitle fallback = compose(inputs, composition, TitleSurface::Window);
    CHECK_STR_EQ(fallback.title, "Studio");
    CHECK_STR_EQ(fallback.systemTitle, "Studio");
    CHECK(fallback.usesFallbackName);

    inputs.pane.programTitle = "Studio";
    const ComposedTitle program = compose(inputs, composition, TitleSurface::Window);
    CHECK_STR_EQ(program.title, "Studio");
    CHECK(!program.usesFallbackName);

    inputs.pane.windowTitle = "Studio";
    const ComposedTitle named = compose(inputs, composition, TitleSurface::Window);
    CHECK_STR_EQ(named.title, "Studio");
    CHECK_STR_EQ(named.systemTitle, "Studio");
    CHECK(!named.usesFallbackName);
}

// A window whose frontend draws its title as one string has nowhere to put a
// component: the dimensions and the status are woven in, with spaces.
TEST(aWindowTitleThatIsOneStringCarriesEveryPart)
{
    TitleInputs inputs = editing();
    inputs.viewer.windowTitleForm = App::WindowTitleForm::PlainText;
    inputs.pane.hostName = "build-box";
    inputs.viewer.isPinned = true;
    const ComposedTitle window = compose(inputs, showing(true, true, false, true, true), TitleSurface::Window);
    CHECK_MSG(window.title == "proj — vim — Working — 80×24", window.title.c_str());
    CHECK_MSG(window.systemTitle == window.title, window.systemTitle.c_str());
    CHECK_MSG(window.dimensions.empty() && window.programStatus.empty(), "nothing is handed back to draw beside a title that is one string");
    CHECK_MSG(window.markPrefix.empty(), "a window title wears no marks in either form");
    CHECK(window.lead == "proj");

    TitleInputs silent;
    silent.viewer.windowTitleForm = App::WindowTitleForm::PlainText;
    silent.viewer.fallbackName = "Ubiquitty";
    CHECK(compose(silent, showing(true, true, true, true, true), TitleSurface::Window).title == "Ubiquitty");
}

TEST(aTabAndAPaneAreOneLineOfTextJoinedWithSpaces)
{
    const ComposedTitle tab = compose(editing(), showing(true, true, false, true, true), TitleSurface::Tab);
    CHECK_MSG(tab.title == "proj — vim — Working — 80×24", tab.title.c_str());
    CHECK(tab.lead == "proj");
    CHECK_MSG(tab.dimensions == "80×24" && tab.programStatus.empty(), "a tab weaves its status in and reports its dimensions beside");
    CHECK(tab.markPrefix.empty());

    const ComposedTitle pane = compose(editing(), showing(false, true, true, false, false), TitleSurface::Pane);
    CHECK_MSG(pane.title == "vim — vim", pane.title.c_str());
    CHECK(pane.lead.empty());

    // The arguments spelling is the composition's to ask for.
    TitleComposition withArguments = showing(false, false, true, false, false);
    withArguments.showsProcessArguments = true;
    CHECK(compose(editing(), withArguments, TitleSurface::Tab).title == "vim notes.txt");

    // A session that ended names no process and no status.
    TitleInputs ended = editing();
    ended.pane.end.didEnd = true;
    CHECK(compose(ended, showing(true, false, true, false, true), TitleSurface::Tab).title == "proj");
}

TEST(aTabWearsItsMarksAndAPaneOnlyItsHostWherePlacesMix)
{
    TitleInputs inputs = editing();
    inputs.pane.hostName = "build-box";
    inputs.viewer.isPinned = true;
    const TitleComposition composition = showing(false, true, false, false, false);

    Title::TabLabelMarks tabMarks;
    tabMarks.isPinned = true;
    tabMarks.hostName = "build-box";
    const ComposedTitle tab = compose(inputs, composition, TitleSurface::Tab);
    CHECK_MSG(tab.title == Title::markTabLabel("vim", tabMarks), tab.title.c_str());
    CHECK(tab.title != "vim");
    CHECK(tab.markPrefix == Title::tabLabelMarkPrefix(tabMarks));

    // A pane whose window is all in one place names no host.
    const ComposedTitle plainPane = compose(inputs, composition, TitleSurface::Pane);
    Title::TabLabelMarks pinnedOnly;
    pinnedOnly.isPinned = true;
    CHECK_MSG(plainPane.title == Title::markTabLabel("vim", pinnedOnly), plainPane.title.c_str());

    inputs.viewer.doPaneHeadersNameHost = true;
    Title::TabLabelMarks paneMarks;
    paneMarks.isPinned = true;
    paneMarks.hostName = "build-box";
    const ComposedTitle mixedPane = compose(inputs, composition, TitleSurface::Pane);
    CHECK_MSG(mixedPane.title == Title::markTabLabel("vim", paneMarks), mixedPane.title.c_str());
    CHECK(mixedPane.title != plainPane.title);
}

TEST(aCustomTitleReplacesOnlyThePartItNames)
{
    const TitleComposition everything = showing(true, true, true, true, true);

    // A tab's custom title takes the program's place and the process
    // segments'; the folder, the readout and the status stay.
    TitleInputs named = editing();
    named.pane.tabTitle = "deploy";
    const ComposedTitle tab = compose(named, everything, TitleSurface::Tab);
    CHECK_MSG(tab.title == "proj — deploy — Working — 80×24", tab.title.c_str());
    CHECK(tab.lead == "proj");
    CHECK_MSG(compose(named, everything, TitleSurface::Pane).title.find("deploy") == std::string::npos, "a pane does not answer to its tab's name");
    CHECK_MSG(compose(named, everything, TitleSurface::Window).title == "proj—vim—vim", "a tab's name does not reach its window");

    // A window's custom title fills the window's name and keeps the
    // components its row draws beside the text: the readout survives.
    TitleInputs windowNamed = editing();
    windowNamed.pane.windowTitle = "ops";
    const ComposedTitle window = compose(windowNamed, everything, TitleSurface::Window);
    CHECK_MSG(window.title == "proj—ops", window.title.c_str());
    CHECK_MSG(window.systemTitle == "proj — ops", window.systemTitle.c_str());
    CHECK(window.dimensions == "80×24" && window.programStatus == "Working" && window.lead == "proj");
    CHECK(window.programStatusInsertionByteOffset == std::string("proj—ops").size());
    // Whatever the app-title switch says.
    const ComposedTitle kept = compose(windowNamed, showing(true, false, false, true, false), TitleSurface::Window);
    CHECK_MSG(kept.title == "proj—ops" && kept.dimensions == "80×24", kept.title.c_str());

    // The tab follows its window's name until it is named apart.
    CHECK_MSG(compose(windowNamed, everything, TitleSurface::Tab).title == "proj — ops — Working — 80×24", compose(windowNamed, everything, TitleSurface::Tab).title.c_str());
    windowNamed.pane.tabTitle = "deploy";
    CHECK(compose(windowNamed, everything, TitleSurface::Tab).title == "proj — deploy — Working — 80×24");
    CHECK(compose(windowNamed, everything, TitleSurface::Window).title == "proj—ops");

    // Empty is automatic: the program names the surfaces again.
    windowNamed.pane.tabTitle.clear();
    windowNamed.pane.windowTitle.clear();
    CHECK(compose(windowNamed, everything, TitleSurface::Window).title == "proj—vim—vim");
    CHECK(compose(windowNamed, everything, TitleSurface::Tab).title == "proj — vim — Working — vim — 80×24");

    // A program's later title does not move a custom title.
    TitleInputs retitled = editing();
    retitled.pane.windowTitle = "ops";
    retitled.pane.programTitle = "htop";
    CHECK(compose(retitled, everything, TitleSurface::Window).title == "proj—ops");

    // The tab's marks are its state and stay on a named tab: the pin.
    const TitleComposition nameOnly = showing(false, true, false, false, false);
    TitleInputs marked = editing();
    marked.pane.tabTitle = "deploy";
    marked.viewer.isPinned = true;
    Title::TabLabelMarks marks;
    marks.isPinned = true;
    const ComposedTitle markedTab = compose(marked, nameOnly, TitleSurface::Tab);
    CHECK_MSG(markedTab.title == Title::markTabLabel("deploy", marks), markedTab.title.c_str());
    CHECK(markedTab.markPrefix == Title::tabLabelMarkPrefix(marks));
    CHECK_MSG(compose(marked, nameOnly, TitleSurface::Window).title == "vim", "a window title wears no pin, and never its tab's name");

    // A pane's own label replaces a pane's title and nothing else's.
    TitleInputs labelled = editing();
    labelled.pane.paneLabel = "logs";
    CHECK(compose(labelled, everything, TitleSurface::Pane).title == "logs");
    CHECK(compose(labelled, nameOnly, TitleSurface::Tab).title == "vim");
}

/// A host is named once per place a reader looks: a tab's label carries its
/// host where the window's chrome does not already say it, or where the
/// window's tabs live in more than one place and the name tells them apart.
TEST(aTabNamesItsHostOnlyWhereItsWindowDoesNotSayIt)
{
    const TitleComposition composition = showing(false, true, false, false, false);
    const auto tabOn = [](std::string_view hostName) {
        TitleInputs inputs = editing();
        inputs.pane.hostName = std::string(hostName);
        return inputs;
    };
    const auto markedWith = [](std::string_view hostName) {
        Title::TabLabelMarks marks;
        marks.hostName = std::string(hostName);
        return Title::markTabLabel("vim", marks);
    };
    using Session::HostPlace;

    // Every tab on one host, and the window's chrome names it: no tab does.
    {
        Session::PaneHostMix tabs;
        tabs.add(HostPlace::Remote, "laptop");
        tabs.add(HostPlace::Remote, "laptop");
        TitleInputs tab = tabOn("laptop");
        tab.viewer.windowHostName = "laptop";
        tab.viewer.doTabsMixPlaces = tabs.shouldNameHost();
        const ComposedTitle composed = compose(tab, composition, TitleSurface::Tab);
        CHECK_MSG(composed.title == "vim" && composed.markPrefix.empty(), composed.title.c_str());
        // The window's own title and a pane's header are other places, with
        // rules of their own: neither is touched by what a tab leaves out.
        CHECK(compose(tab, composition, TitleSurface::Window).title == "vim");
        tab.viewer.doPaneHeadersNameHost = true;
        CHECK(compose(tab, composition, TitleSurface::Pane).title == markedWith("laptop"));
    }

    // A host's tab beside a plain shell's: the host's tab says where it is,
    // and the local one has no host to say.
    {
        Session::PaneHostMix tabs;
        tabs.add(HostPlace::Remote, "laptop");
        tabs.add(HostPlace::None, "");
        CHECK(tabs.shouldNameHost());
        TitleInputs remote = tabOn("laptop");
        remote.viewer.doTabsMixPlaces = tabs.shouldNameHost();
        // Whichever tab is in front decides what the chrome names; the mix
        // names the host on the remote tab either way.
        for (const std::string_view named : { std::string_view("laptop"), std::string_view() }) {
            remote.viewer.windowHostName = std::string(named);
            const ComposedTitle composed = compose(remote, composition, TitleSurface::Tab);
            CHECK_MSG(composed.title == markedWith("laptop"), composed.title.c_str());
        }
        TitleInputs local = tabOn("");
        local.viewer.doTabsMixPlaces = tabs.shouldNameHost();
        local.viewer.windowHostName = "laptop";
        const ComposedTitle composed = compose(local, composition, TitleSurface::Tab);
        CHECK_MSG(composed.title == "vim" && composed.markPrefix.empty(), composed.title.c_str());
    }

    // Tabs of two hosts: each says its own.
    {
        Session::PaneHostMix tabs;
        tabs.add(HostPlace::Remote, "laptop");
        tabs.add(HostPlace::Remote, "build-box");
        CHECK(tabs.shouldNameHost());
        for (const std::string_view host : { std::string_view("laptop"), std::string_view("build-box") }) {
            TitleInputs tab = tabOn(host);
            tab.viewer.doTabsMixPlaces = tabs.shouldNameHost();
            tab.viewer.windowHostName = "laptop";
            const ComposedTitle composed = compose(tab, composition, TitleSurface::Tab);
            CHECK_MSG(composed.title == markedWith(host), composed.title.c_str());
        }
    }

    // The same tab moved between windows: the pane's facts are the same and
    // the answer is the window's. In a window of its own host it is plain; in
    // a window of plain shells it names its host; in a window of another host
    // it names its own beside that one's.
    {
        const TitleInputs moving = tabOn("laptop");
        TitleInputs atHome = moving;
        atHome.viewer.windowHostName = "laptop";
        CHECK(compose(atHome, composition, TitleSurface::Tab).title == "vim");

        Session::PaneHostMix amongShells;
        amongShells.add(HostPlace::None, "");
        amongShells.add(HostPlace::Remote, "laptop");
        TitleInputs amongLocal = moving;
        amongLocal.viewer.doTabsMixPlaces = amongShells.shouldNameHost();
        CHECK(compose(amongLocal, composition, TitleSurface::Tab).title == markedWith("laptop"));

        Session::PaneHostMix amongOthers;
        amongOthers.add(HostPlace::Remote, "build-box");
        amongOthers.add(HostPlace::Remote, "laptop");
        TitleInputs amongRemote = moving;
        amongRemote.viewer.doTabsMixPlaces = amongOthers.shouldNameHost();
        amongRemote.viewer.windowHostName = "build-box";
        CHECK(compose(amongRemote, composition, TitleSurface::Tab).title == markedWith("laptop"));
    }

    // A window whose chrome names no host, or names another: the label is the
    // only place this tab's host is said, so it says it. A reader with no
    // window around its tabs is the same case.
    {
        TitleInputs tab = tabOn("laptop");
        CHECK(compose(tab, composition, TitleSurface::Tab).title == markedWith("laptop"));
        tab.viewer.windowHostName = "build-box";
        CHECK(compose(tab, composition, TitleSurface::Tab).title == markedWith("laptop"));
    }

    // A custom title names what runs there, not where: the host chip stays
    // by the same rule, and a name that already says the host is not marked
    // with it again.
    {
        Session::PaneHostMix tabs;
        tabs.add(HostPlace::Remote, "laptop");
        tabs.add(HostPlace::None, "");
        for (const bool isTabName : { true, false }) {
            TitleInputs named = tabOn("laptop");
            (isTabName ? named.pane.tabTitle : named.pane.windowTitle) = "deploy";
            named.viewer.doTabsMixPlaces = tabs.shouldNameHost();
            const ComposedTitle mixed = compose(named, composition, TitleSurface::Tab);
            Title::TabLabelMarks hostMark;
            hostMark.hostName = "laptop";
            CHECK_MSG(mixed.title == Title::markTabLabel("deploy", hostMark), mixed.title.c_str());
            named.viewer.doTabsMixPlaces = false;
            named.viewer.windowHostName = "laptop";
            CHECK(compose(named, composition, TitleSurface::Tab).title == "deploy");
            (isTabName ? named.pane.tabTitle : named.pane.windowTitle) = "deploy \xC2\xB7 laptop";
            named.viewer.windowHostName.clear();
            CHECK(compose(named, composition, TitleSurface::Tab).title == "deploy \xC2\xB7 laptop");
        }
    }
}

TEST(aSurfaceWithNothingToSayTakesItsFallbackName)
{
    TitleInputs silent;
    silent.viewer.fallbackName = "Ubiquitty";
    const TitleComposition composition = showing(true, true, true, false, true);
    const ComposedTitle window = compose(silent, composition, TitleSurface::Window);
    CHECK(window.title == "Ubiquitty" && window.lead.empty());
    CHECK(compose(silent, composition, TitleSurface::Tab).title == "Ubiquitty");

    // A tab that fell back to its host's name already says the host.
    silent.viewer.fallbackName = "build-box";
    silent.pane.hostName = "build-box";
    CHECK_MSG(compose(silent, composition, TitleSurface::Tab).title == "build-box", "the host is not said twice");
}

TEST(aWindowOfOneTabIsCalledWhatThatTabIsCalled)
{
    const std::vector<std::string> oneTab { "proj — vim" };
    const App::ListedWindowTitle single = App::listedWindowTitle("", oneTab, 0);
    CHECK(single.title == "proj — vim");
    CHECK_MSG(!single.hasTabRows, "one tab gets no row repeating the window's title");

    const std::vector<std::string> tabs { "proj — vim", "logs", "deploy" };
    const App::ListedWindowTitle several = App::listedWindowTitle("", tabs, 2);
    CHECK_MSG(several.title == "deploy" && several.hasTabRows, "several tabs: the window is called what its active tab is, and each tab has a row");
    CHECK_MSG(App::listedWindowTitle("", tabs, 9).title == "proj — vim", "an active index past the end names the first tab");

    const App::ListedWindowTitle labelled = App::listedWindowTitle("ops", tabs, 1);
    CHECK(labelled.title == "ops" && labelled.hasTabRows);
    CHECK_MSG(App::listedWindowTitle("ops", oneTab, 0).title == "ops" && !App::listedWindowTitle("ops", oneTab, 0).hasTabRows, "a label names the window; one tab still gets no row");
    CHECK(App::listedWindowTitle("", {}, 0).title.empty() && !App::listedWindowTitle("", {}, 0).hasTabRows);

    const std::vector<std::string> windows { "ops", "deploy" };
    CHECK(App::listedWorkspaceTitle("release", "api", windows, 1) == "release");
    CHECK_MSG(App::listedWorkspaceTitle("", "api", windows, 1) == "api", "an unnamed workspace is called by its folder before its window");
    CHECK(App::listedWorkspaceTitle("", "", windows, 1) == "deploy");
    CHECK(App::listedWorkspaceTitle("", "", windows, 7) == "ops");
    CHECK(App::listedWorkspaceTitle("", "", {}, 0).empty());
}

TEST(aNameIsShownAndALabelIsAChipExceptWhereTheLabelIsTheTitle)
{
    using App::ListedLevel;
    using App::ListedName;

    // A workspace and a tab have names: the name is displayed and the label,
    // the handle a target addresses it by, sits beside it.
    ListedName listed = App::listedName(ListedLevel::Workspace, "release", "rel");
    CHECK(listed.title == "release" && listed.labelChip == "rel");
    listed = App::listedName(ListedLevel::Tab, "proj — vim", "editor");
    CHECK_MSG(listed.title == "proj — vim" && listed.labelChip == "editor", "a tab's label never replaces what the tab is called");
    listed = App::listedName(ListedLevel::Tab, "proj — vim", "");
    CHECK(listed.title == "proj — vim" && listed.labelChip.empty());
    listed = App::listedName(ListedLevel::Workspace, "", "rel");
    CHECK_MSG(listed.title.empty() && listed.labelChip == "rel", "a label does not stand in for a missing name");

    // A window and a pane have no name: a label put on one is its title, and
    // is not said a second time beside itself.
    listed = App::listedName(ListedLevel::Window, "deploy", "ops");
    CHECK(listed.title == "ops" && listed.labelChip.empty());
    listed = App::listedName(ListedLevel::Pane, "vim", "logs");
    CHECK(listed.title == "logs" && listed.labelChip.empty());
    listed = App::listedName(ListedLevel::Window, "deploy", "");
    CHECK(listed.title == "deploy" && listed.labelChip.empty());
    listed = App::listedName(ListedLevel::Pane, "vim", "");
    CHECK(listed.title == "vim" && listed.labelChip.empty());

    CHECK(!App::doesLabelReplaceTitle(ListedLevel::Workspace) && !App::doesLabelReplaceTitle(ListedLevel::Tab));
    CHECK(App::doesLabelReplaceTitle(ListedLevel::Window) && App::doesLabelReplaceTitle(ListedLevel::Pane));

    // The composition agrees with the rule at each level it already decides.
    const TitleComposition everything = showing(true, true, true, true, true);
    TitleInputs labelled = editing();
    labelled.pane.paneLabel = "logs";
    const std::string composedPane = compose(editing(), everything, TitleSurface::Pane).title;
    CHECK_MSG(compose(labelled, everything, TitleSurface::Pane).title == App::listedName(ListedLevel::Pane, composedPane, "logs").title, "a pane's label is its title in a header and in a listing");
    const std::vector<std::string> tabs { "proj — vim", "logs" };
    CHECK(App::listedWindowTitle("ops", tabs, 1).title == App::listedName(ListedLevel::Window, "logs", "ops").title);
    CHECK(App::listedWindowTitle("", tabs, 1).title == App::listedName(ListedLevel::Window, "logs", "").title);
}

namespace {

TitleInputs runningIn(std::string chain)
{
    TitleInputs inputs;
    inputs.pane.currentDirectory = "/Users/user/src";
    inputs.pane.homeDirectory = "/Users/user";
    inputs.pane.processChain = std::move(chain);
    return inputs;
}

TitleComposition hidingFor(std::string_view names, bool inFullScreen) // bool-ok: a fixture, the flag named at its one definition
{
    TitleComposition composition = showing(true, true, true, false, false);
    composition.directoryHiddenFor = names;
    composition.hidesDirectoryInFullScreen = inFullScreen;
    return composition;
}

} // namespace

TEST(aMultiplexerOrARemoteShellLeavesTheDirectoryOut)
{
    const auto composition = hidingFor("tmux, tmate, zellij, screen, ssh, mosh-client, et", false);
    for (const char* chain : { "tmux", "ssh", "mosh-client", "zellij", "ssh ◂ deploy.sh" }) {
        const auto title = compose(runningIn(chain), composition, TitleSurface::Tab);
        CHECK_MSG(title.lead.empty() && title.title.find("src") == std::string::npos, chain);
    }
    // A program the list does not name keeps its directory.
    CHECK_STR_EQ(compose(runningIn("vim"), composition, TitleSurface::Tab).lead, "src");
    // The shell itself is not the program it runs.
    CHECK_STR_EQ(compose(runningIn("-zsh"), composition, TitleSurface::Window).lead, "src");
}

TEST(theProgramNamedIsTheOneHoldingTheTerminal)
{
    const auto composition = hidingFor("ssh", false);
    // The chain leads with the program holding the terminal, whichever
    // process stands beside it: an ssh mc runs leaves mc in front.
    CHECK_STR_EQ(compose(runningIn("mc ▸ ssh"), composition, TitleSurface::Tab).lead, "src");
    CHECK(compose(runningIn("ssh ◂ make"), composition, TitleSurface::Tab).lead.empty());
    CHECK_STR_EQ(compose(runningIn("make ◂ ssh"), composition, TitleSurface::Tab).lead, "src");
    // The facts' own foreground name wins over the chain.
    auto inputs = runningIn("vim");
    inputs.pane.foregroundName = "ssh";
    CHECK(compose(inputs, composition, TitleSurface::Tab).lead.empty());
    // A chain with arguments names its program first; a script names itself first.
    auto withArguments = runningIn("");
    withArguments.pane.processChainWithArguments = "ssh host ◂ make deploy";
    CHECK(compose(withArguments, composition, TitleSurface::Tab).lead.empty());
    CHECK_STR_EQ(compose(runningIn("ssh.py ‹ python3"), composition, TitleSurface::Tab).lead, "src");
}

TEST(aFullScreenProgramLeavesTheDirectoryOutOnlyWhenAsked)
{
    auto vim = runningIn("vim");
    vim.pane.isFullScreen = true;
    CHECK_STR_EQ(compose(vim, hidingFor("", false), TitleSurface::Tab).lead, "src");
    CHECK(compose(vim, hidingFor("", true), TitleSurface::Tab).lead.empty());
    vim.pane.isFullScreen = false;
    CHECK_STR_EQ(compose(vim, hidingFor("", true), TitleSurface::Tab).lead, "src");
}

TEST(aHostedPanesDirectoryIsSpelledByTheReadersOwnSettings)
{
    // As a host reports a pane it serves: the whole directory and the home
    // on that machine, which the reader abbreviates against itself.
    TitleInputs inputs;
    inputs.pane.currentDirectory = "/var/home/user/src";
    inputs.pane.homeDirectory = "/var/home/user";
    inputs.pane.processChain = "-zsh";
    inputs.pane.hostName = "devbox";
    for (const TitleSurface surface : { TitleSurface::Window, TitleSurface::Tab, TitleSurface::Pane }) {
        auto fullPath = showing(true, false, true, false, false);
        fullPath.showsDirectoryFullPath = true;
        CHECK_STR_EQ(compose(inputs, fullPath, surface).lead, "~/src");
        auto folder = showing(true, false, true, false, false);
        CHECK_STR_EQ(compose(inputs, folder, surface).lead, "src");
    }
    // Against no home, a path the host has not anchored shows whole.
    inputs.pane.homeDirectory.clear();
    auto fullPath = showing(true, false, true, false, false);
    fullPath.showsDirectoryFullPath = true;
    CHECK_STR_EQ(compose(inputs, fullPath, TitleSurface::Window).lead, "/var/home/user/src");
}

UBIQUITTY_TEST_MAIN()
