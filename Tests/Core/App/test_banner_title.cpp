// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_banner_title.cpp — the title a banner's place line names a window or a
// tab by (Core/App/BannerTitle.h).
//
// A tab's label leads with its host and a window's row draws its dimensions
// beside a bare dash, so the two titles of one pane never compared equal as
// text and a place line built from them said the title twice, once with the
// host inside it. `composeBannerTitle` asks the composer for both in one form.
//
// What the cases hold to:
//
//   A window and a tab weaving the same parts compose the same text.
//
//   The host, the pin and the tag are not in it, and neither are the
//   dimensions or a program's status.
//
//   A name a person gave reaches the title as it does on the surface, so a
//   window named apart from its tab still reads apart.
//

#include "Harness.h"

#include "Core/App/BannerTitle.h"

#include <string>

namespace {

using namespace Ubiquitty;
using App::composeBannerTitle;
using Preferences::Effective::TitleComposition;
using Session::TitleSurface;

TitleComposition showing(bool dimensions, bool programStatus) // bool-ok: a fixture, each flag named at its one definition
{
    return TitleComposition {
        .showsDirectory = true,
        .showsDirectoryFullPath = false,
        .showsAppTitle = false,
        .showsProcess = true,
        .showsProcessArguments = true,
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

App::PaneTitleFacts remoteShell()
{
    App::PaneTitleFacts pane;
    pane.currentDirectory = "/Users/x";
    pane.homeDirectory = "/Users/x";
    pane.processChain = "ssh";
    pane.processChainWithArguments = "ssh build-box";
    pane.programStatus = "Working";
    pane.columns = 120;
    pane.rows = 40;
    pane.hostName = "Studio";
    return pane;
}

App::TitleViewerFacts marked()
{
    App::TitleViewerFacts viewer;
    viewer.fallbackName = "Terminal";
    viewer.isPinned = true;
    return viewer;
}

} // namespace

TEST(aWindowAndATabOfOnePaneComposeOneTitle)
{
    // The window's composition shows what its row draws beside the text; the
    // tab's does not. Neither reaches the banner's title.
    const std::string window = composeBannerTitle(remoteShell(), marked(), showing(true, true), TitleSurface::Window);
    const std::string tab = composeBannerTitle(remoteShell(), marked(), showing(false, false), TitleSurface::Tab);
    CHECK_STR_EQ(tab, "~ — ssh build-box");
    CHECK_STR_EQ(window, tab);

    // What the surfaces themselves show is two different texts, which is the
    // comparison this exists to avoid.
    const std::string shownOnTab = App::composeTitle(remoteShell(), marked(), showing(false, false), TitleSurface::Tab).title;
    const std::string shownOnWindow = App::composeTitle(remoteShell(), marked(), showing(true, true), TitleSurface::Window).title;
    CHECK(shownOnTab != shownOnWindow);
    CHECK_CONTAINS(shownOnTab, "Studio");
    CHECK(tab.find("Studio") == std::string::npos);
}

TEST(aTabThatShowsItsDimensionsStillNamesOnlyThePlace)
{
    const std::string tab = composeBannerTitle(remoteShell(), marked(), showing(true, true), TitleSurface::Tab);
    CHECK_STR_EQ(tab, "~ — ssh build-box");
}

TEST(aNameAPersonGaveIsTheTitleOfTheSurfaceItWasGivenTo)
{
    App::PaneTitleFacts pane = remoteShell();
    pane.windowTitle = "Deploys";
    // A custom title takes the process segments' place and the directory
    // stays; a tab with no title of its own answers to its window's.
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(true, true), TitleSurface::Window), "~ — Deploys");
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(false, false), TitleSurface::Tab), "~ — Deploys");

    // The tab's own title wins on the tab and never reaches the window.
    pane.tabTitle = "release";
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(false, false), TitleSurface::Tab), "~ — release");
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(true, true), TitleSurface::Window), "~ — Deploys");
}

TEST(aPaneWithNothingToWeaveFallsBackAsItsSurfaceDoes)
{
    App::PaneTitleFacts pane;
    pane.hostName = "Studio";
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(false, false), TitleSurface::Tab), "Terminal");
    CHECK_STR_EQ(composeBannerTitle(pane, marked(), showing(true, true), TitleSurface::Window), "Terminal");
}

UBIQUITTY_TEST_MAIN()
