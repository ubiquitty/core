// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Agent/SessionListing.h"

using namespace Ubiquitty;

TEST(aListedSessionKeepsWindowTabPaneAndSessionLabelsDistinct)
{
    Session::TreeWindow window;
    window.id = Session::Layout::SurfaceID { 7 };
    window.label = "build server";
    Session::TreeTab tab;
    tab.id = Session::Layout::TabID { 13 };
    tab.label = "tests";
    tab.name = "not the label";
    Session::TreePane pane;
    pane.id = Session::Layout::PaneID { 19 };
    pane.sessionID = 23;
    pane.label = "output";
    pane.sessionLabel = "release";
    pane.program = "ninja";
    pane.title.programTitle = "not a label either";
    pane.title.currentDirectory = "/work/ubiquitty";
    pane.facts.isAtPrompt = true;
    const auto row = Agent::sessionJSON({ &window, &tab, &pane });
    CHECK(row.stringOr("session") == "session:23");
    CHECK(row.stringOr("label") == "release");
    CHECK(row.stringOr("window") == "window:7");
    CHECK(row.stringOr("windowLabel") == "build server");
    CHECK(row.stringOr("tab") == "tab:13");
    CHECK(row.stringOr("tabLabel") == "tests");
    CHECK(row.stringOr("pane") == "pane:19");
    CHECK(row.stringOr("paneLabel") == "output");
    CHECK(row.stringOr("foreground") == "ninja");
    CHECK(row.stringOr("directory") == "/work/ubiquitty");
    CHECK(row.stringOr("title") == "not a label either");
    CHECK(row.boolOr("atPrompt"));
}

TEST(anUnlabelledListedParentStillHasAnExplicitEmptyLabel)
{
    Session::TreeWindow window;
    Session::TreeTab tab;
    Session::TreePane pane;
    const auto row = Agent::sessionJSON({ &window, &tab, &pane });
    for (const auto key : { "label", "windowLabel", "tabLabel", "paneLabel" }) {
        const auto* value = row.find(key);
        REQUIRE(value);
        CHECK(value->isString() && value->asString().empty());
    }
    CHECK(row.stringOr("window").empty());
    CHECK(row.stringOr("tab").empty());
}

TEST(aListedSessionOwnsItsEscapedUnicodeLabelsAfterTheHostSnapshotChanges)
{
    Session::TreeWindow window;
    window.label = "remote \"build\"\n東京";
    Session::TreeTab tab;
    tab.label = "tests\t\\logs";
    Session::TreePane pane;
    const auto row = Agent::sessionJSON({ &window, &tab, &pane });
    window.label.clear();
    tab.label = "changed";
    bool isJSON = false;
    const auto decoded = Codec::JSON::parse(Codec::JSON::serialize(row), isJSON);
    REQUIRE(isJSON);
    CHECK(decoded.stringOr("windowLabel") == "remote \"build\"\n東京");
    CHECK(decoded.stringOr("tabLabel") == "tests\t\\logs");
}

UBIQUITTY_TEST_MAIN()
