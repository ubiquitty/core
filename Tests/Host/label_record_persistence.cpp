// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/label_record_persistence.cpp — a tab's label and a workspace's
// label are host state like a window's and a pane's: bounded and validated by
// the one label book, kept while the object lives, written to the workspace's
// record and set again on the workspace a restarted host restores from it.
// The tags a person put on a workspace, a window, a tab, a pane and the
// session behind a pane are kept the same way.
//
// One daemon holds a named workspace of two windows and three tabs, labels the
// workspace, two of the tabs, a window, a pane and the session behind it, tags
// something at every level, and describes it as its record. The record's bytes go through the file's codec to a second
// daemon, which restores the workspace under fresh identities and is asked for
// the labels and tags of those.
//
// They are set through the friend seam, by the functions the label and tag
// calls reach.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Ubiquitty::Session::Identity::LabelBook;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Layout = Ubiquitty::Session::Layout;

namespace {

ObjectID tabIdentity(Layout::TabID tab) { return { ObjectKind::Tab, std::to_underlying(tab) }; }

} // namespace

int main()
{
    std::vector<std::uint8_t> file;
    {
        Daemon d;
        Screen::setRestoreDefaults(d);
        const std::array<std::string, 3> command { "/bin/sh", "-c", "sleep 60" };
        auto* session = Screen::spawnSession(d, 80, 24, command);
        CHECK_MSG(session != nullptr, "premise: the session was not spawned");
        if (!session)
            return UBIQUITTY_TEST_SUMMARY();
        const ObjectID workspace { ObjectKind::Workspace, Screen::nameWorkspace(d, *session, "work") };
        Layout::Workspace& layout = Screen::layout(d, *session);
        const Layout::SurfaceID firstWindow = layout.surfaceIDAt(0);
        const Layout::TabID firstTab = layout.tabAt(firstWindow, 0);
        const Layout::TabID secondTab = layout.addTab(firstWindow);
        const Layout::SurfaceID secondWindow = layout.addSurface();
        const Layout::TabID thirdTab = layout.tabAt(secondWindow, 0);
        CHECK_MSG(secondTab != Layout::invalidTab && thirdTab != Layout::invalidTab, "premise: three tabs in two windows");

        // The book's validation, through the daemon's setter.
        CHECK(Screen::setLabel(d, workspace, "client work"));
        CHECK(Screen::setLabel(d, tabIdentity(firstTab), "editing"));
        CHECK(Screen::setLabel(d, tabIdentity(thirdTab), "far"));
        CHECK_MSG(!Screen::setLabel(d, workspace, "workspace:3"), "a label that reads as an identity is refused");
        CHECK_MSG(!Screen::setLabel(d, tabIdentity(firstTab), std::string(65, 'x')), "a label past the bound is refused");
        CHECK_MSG(!Screen::setLabel(d, tabIdentity(firstTab), "two\nlines"), "a control character is refused");
        CHECK_STR_EQ(Screen::label(d, workspace), "client work");
        CHECK_STR_EQ(Screen::label(d, tabIdentity(firstTab)), "editing");
        CHECK_STR_EQ(Screen::label(d, tabIdentity(secondTab)), "");

        // The tags a person put on a window, through the daemon's setter.
        const ObjectID secondWindowIdentity { ObjectKind::Window, std::to_underlying(secondWindow) };
        const std::vector<std::string> tags { "production", "billing" };
        CHECK(Screen::setTags(d, secondWindowIdentity, tags));
        CHECK_MSG(!Screen::setTags(d, secondWindowIdentity, std::vector<std::string> { "" }), "a tag with no name is refused");
        CHECK(Screen::tags(d, secondWindowIdentity) == tags);
        CHECK(Screen::tags(d, ObjectID { ObjectKind::Window, std::to_underlying(firstWindow) }).empty());
        // A full book makes room by dropping the tags of windows that ended.
        const std::vector<std::string> stale { "gone" };
        for (std::uint64_t number = 1; number <= Ubiquitty::Session::Identity::TagBook::capacity && Screen::setTags(d, ObjectID { ObjectKind::Window, 200'000 + number }, stale); ++number) {
        }
        CHECK(Screen::setTags(d, ObjectID { ObjectKind::Window, std::to_underlying(firstWindow) }, stale));
        CHECK_MSG(Screen::tags(d, secondWindowIdentity) == tags && Screen::tags(d, ObjectID { ObjectKind::Window, 200'001 }).empty(), "the live window kept its tags and the ended ones lost theirs");
        CHECK(Screen::setTags(d, ObjectID { ObjectKind::Window, std::to_underlying(firstWindow) }, {}));
        // And on every other level.
        const Layout::PaneID firstPane = static_cast<Layout::PaneID>(static_cast<std::uint32_t>(layout.paneTree(firstWindow, firstTab)->focusedPaneID()));
        const Layout::PaneID thirdPane = static_cast<Layout::PaneID>(static_cast<std::uint32_t>(layout.paneTree(secondWindow, thirdTab)->focusedPaneID()));
        CHECK(Screen::setTags(d, workspace, std::vector<std::string> { "client" }));
        CHECK(Screen::setTags(d, tabIdentity(secondTab), std::vector<std::string> { "noisy", "logs" }));
        CHECK(Screen::setTags(d, ObjectID { ObjectKind::Pane, std::to_underlying(firstPane) }, std::vector<std::string> { "watch" }));
        CHECK(Screen::setTags(d, ObjectID { ObjectKind::Pane, std::to_underlying(thirdPane) }, std::vector<std::string> { "far pane" }));
        CHECK(Screen::setTags(d, ObjectID { ObjectKind::Session, session->id }, std::vector<std::string> { "mine" }));
        // Labels on a window, a pane and the session behind it.
        CHECK(Screen::setLabel(d, secondWindowIdentity, "billing window"));
        CHECK(Screen::setLabel(d, ObjectID { ObjectKind::Pane, std::to_underlying(firstPane) }, "editor"));
        CHECK(Screen::setLabel(d, ObjectID { ObjectKind::Session, session->id }, "main shell"));

        // Both are live objects, so a prune of a full book keeps their labels.
        CHECK(Screen::isLiveObject(d, workspace));
        CHECK(!Screen::isLiveObject(d, ObjectID { ObjectKind::Workspace, workspace.number + 1000 }));
        CHECK(Screen::isLiveObject(d, tabIdentity(thirdTab)));
        for (std::uint64_t number = 1; Screen::setLabel(d, ObjectID { ObjectKind::Pane, 100'000 + number }, "gone") && number <= LabelBook::capacity; ++number) {
        }
        CHECK_MSG(Screen::setLabel(d, tabIdentity(secondTab), "logs"), "a full book made room by dropping what had ended");
        CHECK_STR_EQ(Screen::label(d, workspace), "client work");
        CHECK_STR_EQ(Screen::label(d, tabIdentity(firstTab)), "editing");
        CHECK_STR_EQ(Screen::label(d, tabIdentity(thirdTab)), "far");
        CHECK_STR_EQ(Screen::label(d, secondWindowIdentity), "billing window");
        CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Pane, std::to_underlying(firstPane) }), "editor");
        CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Session, session->id }), "main shell");
        CHECK(Screen::setLabel(d, tabIdentity(secondTab), ""));

        const auto record = Screen::captureLayoutRecord(d, *session);
        CHECK_MSG(record.has_value(), "premise: the workspace was described");
        if (!record)
            return UBIQUITTY_TEST_SUMMARY();
        CHECK_STR_EQ(record->workspaceLabel, "client work");
        CHECK_MSG(record->tabLabels == std::vector<std::string>({ "editing", "", "far" }), "one label per tab, windows in order and their tabs in order");
        CHECK_MSG(record->windowTags == (std::vector<std::vector<std::string>> { {}, { "production", "billing" } }), "one run of tags per window");
        CHECK(record->workspaceTags == std::vector<std::string> { "client" });
        CHECK_MSG(record->tabTags == (std::vector<std::vector<std::string>> { {}, { "noisy", "logs" }, {} }), "one run of tags per tab, in the order of their labels");
        CHECK_MSG(record->panes.size() == 3 && record->panes[0].tags == std::vector<std::string> { "watch" } && record->panes[0].sessionTags == std::vector<std::string> { "mine" }, "a pane's tags and its session's, apart");
        CHECK(record->panes.size() == 3 && record->panes[1].tags.empty() && record->panes[1].sessionTags.empty() && record->panes[2].tags == std::vector<std::string> { "far pane" });
        CHECK_MSG(record->windowLabels == std::vector<std::string>({ "", "billing window" }), "one label per window, in order");
        CHECK_MSG(record->panes.size() == 3 && record->panes[0].label == "editor" && record->panes[0].sessionLabel == "main shell" && record->panes[1].label.empty()
                && record->panes[1].sessionLabel.empty(),
            "a pane's label and its session's, apart");
        const std::array records { *record };
        Ubiquitty::Session::encodeHostLayoutRecords(records, file);
    }

    Daemon d;
    Screen::setRestoreDefaults(d);
    auto decoded = Ubiquitty::Session::decodeHostLayoutRecords(file);
    CHECK_MSG(decoded.has_value() && decoded->size() == 1, "premise: the record file decodes");
    if (!decoded)
        return UBIQUITTY_TEST_SUMMARY();
    auto* session = Screen::restoreFirstRecord(d, std::move(*decoded));
    CHECK_MSG(session != nullptr, "premise: the workspace was restored");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    Layout::Workspace& layout = Screen::layout(d, *session);
    CHECK_MSG(layout.surfaceCount() == 2 && layout.tabCount(layout.surfaceIDAt(0)) == 2 && layout.tabCount(layout.surfaceIDAt(1)) == 1, "premise: the restored arrangement");
    CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Workspace, Screen::workspaceID(d, *session) }), "client work");
    CHECK_STR_EQ(Screen::label(d, tabIdentity(layout.tabAt(layout.surfaceIDAt(0), 0))), "editing");
    CHECK_STR_EQ(Screen::label(d, tabIdentity(layout.tabAt(layout.surfaceIDAt(0), 1))), "");
    CHECK_STR_EQ(Screen::label(d, tabIdentity(layout.tabAt(layout.surfaceIDAt(1), 0))), "far");

    CHECK(Screen::tags(d, ObjectID { ObjectKind::Window, std::to_underlying(layout.surfaceIDAt(0)) }).empty());
    CHECK_MSG(Screen::tags(d, ObjectID { ObjectKind::Window, std::to_underlying(layout.surfaceIDAt(1)) }) == (std::vector<std::string> { "production", "billing" }), "the restored window wears the tags it was recorded with");

    CHECK(Screen::tags(d, ObjectID { ObjectKind::Workspace, Screen::workspaceID(d, *session) }) == std::vector<std::string> { "client" });
    CHECK(Screen::tags(d, tabIdentity(layout.tabAt(layout.surfaceIDAt(0), 0))).empty());
    CHECK_MSG(Screen::tags(d, tabIdentity(layout.tabAt(layout.surfaceIDAt(0), 1))) == (std::vector<std::string> { "noisy", "logs" }), "the restored tab wears the tags it was recorded with");
    const auto restoredPane = [&](int window, int tab) {
        const Layout::SurfaceID surface = layout.surfaceIDAt(window);
        return ObjectID { ObjectKind::Pane, static_cast<std::uint32_t>(layout.paneTree(surface, layout.tabAt(surface, tab))->focusedPaneID()) };
    };
    CHECK(Screen::tags(d, restoredPane(0, 0)) == std::vector<std::string> { "watch" });
    CHECK(Screen::tags(d, restoredPane(0, 1)).empty());
    CHECK(Screen::tags(d, restoredPane(1, 0)) == std::vector<std::string> { "far pane" });
    CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Window, std::to_underlying(layout.surfaceIDAt(1)) }), "billing window");
    CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Window, std::to_underlying(layout.surfaceIDAt(0)) }), "");
    CHECK_STR_EQ(Screen::label(d, restoredPane(0, 0)), "editor");
    CHECK_STR_EQ(Screen::label(d, restoredPane(0, 1)), "");
    const std::uint32_t restoredSession = Screen::workspaceSessionFor(d, *session, static_cast<Layout::PaneID>(restoredPane(0, 0).number));
    CHECK_MSG(restoredSession != 0, "premise: the restored pane has a session");
    CHECK_STR_EQ(Screen::label(d, ObjectID { ObjectKind::Session, restoredSession }), "main shell");

    // And it describes itself as the record it came from.
    const auto again = Screen::captureLayoutRecord(d, *session);
    CHECK(again.has_value() && again->workspaceLabel == "client work" && again->tabLabels == std::vector<std::string>({ "editing", "", "far" }));
    CHECK(again.has_value() && again->windowTags == (std::vector<std::vector<std::string>> { {}, { "production", "billing" } }));
    CHECK(again.has_value() && again->workspaceTags == std::vector<std::string> { "client" } && again->tabTags == (std::vector<std::vector<std::string>> { {}, { "noisy", "logs" }, {} }));
    CHECK_MSG(again.has_value() && again->panes.size() == 3 && again->panes[0].tags == std::vector<std::string> { "watch" } && again->panes[0].sessionTags == std::vector<std::string> { "mine" } && again->panes[1].sessionTags.empty(),
        "the session a restored pane was given wears the tags the pane's session was recorded with");
    CHECK_MSG(again.has_value() && again->windowLabels == std::vector<std::string>({ "", "billing window" }) && again->panes.size() == 3 && again->panes[0].label == "editor"
            && again->panes[0].sessionLabel == "main shell",
        "the restored window, pane and session carry the labels they were recorded with");
    return UBIQUITTY_TEST_SUMMARY();
}
