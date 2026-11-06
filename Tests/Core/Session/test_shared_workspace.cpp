// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/SharedWorkspace.h"
#include "Harness.h"

using namespace Ubiquitty;

namespace {

Session::SharedWorkspace sharedWindow()
{
    Session::SharedWorkspace shared;
    Session::WorkspaceWindow window;
    window.tabs.push_back({ "editor", Session::PaneNode::makeSplit(false, 0.3, Session::PaneNode::makeLeaf({}), Session::PaneNode::makeLeaf({})), 1 });
    window.tabs.push_back({ "build", Session::PaneNode::makeLeaf({}), 0 });
    window.activeTab = 0;
    window.zoomedPaneIndex = 1;
    shared.arrangement.windows.push_back(std::move(window));
    shared.sessions = { 41, 42, 43 };
    return shared;
}

Codec::JSON::Value parameters(const Session::SharedWorkspace& shared)
{
    bool isJSON = false;
    const auto request = Codec::JSON::parse(shared.request(7, "share-test"), isJSON);
    CHECK(isJSON);
    return *request.find("params");
}

} // namespace

TEST(sharedWindowKeepsTabsSplitsFocusAndDestination)
{
    auto shared = sharedWindow();
    shared.destinationWorkspaceID = 19;
    const auto decoded = Session::readSharedWorkspace(parameters(shared));
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->destinationWorkspaceID, 19u);
    CHECK(decoded->sessions == shared.sessions);
    CHECK(Session::equalWorkspaces(decoded->arrangement, shared.arrangement));
}

TEST(sharedWindowRejectsDuplicateMissingAndExtraSessions)
{
    auto shared = sharedWindow();
    shared.sessions[1] = shared.sessions[0];
    CHECK(!Session::readSharedWorkspace(parameters(shared)));
    shared.sessions = { 41, 42 };
    CHECK(!Session::readSharedWorkspace(parameters(shared)));
    shared.sessions = { 41, 42, 43, 44 };
    CHECK(!Session::readSharedWorkspace(parameters(shared)));
}

TEST(nativeTabsBecomeOneSharedWindow)
{
    Session::SharedWorkspace shared;
    auto first = sharedWindow();
    CHECK(shared.append(first, false));
    auto second = sharedWindow();
    second.sessions = { 51, 52, 53 };
    CHECK(shared.append(second, true));
    CHECK_EQ(shared.arrangement.windows.size(), 1u);
    CHECK_EQ(shared.arrangement.windows.front().tabs.size(), 4u);
    CHECK_EQ(shared.arrangement.windows.front().activeTab, 2);
    CHECK_EQ(shared.arrangement.windows.front().zoomedPaneIndex, 1);
    CHECK(!shared.append(first, false));
    CHECK_EQ(shared.sessions.size(), 6u);
}

TEST(sharedPanesKeepTheirHostIdentitiesAndExistingWindow)
{
    namespace Layout = Session::Layout;
    Layout::Workspace layout;
    const auto existing = layout.activeSurface();
    const auto existingPane = layout.focusedPane(existing);
    std::vector<std::pair<Layout::PaneID, Layout::PaneID>> replacements;
    CHECK(Layout::restoreFromSnapshot(sharedWindow().arrangement, layout, [](void* context, Layout::PaneID pane, const Layout::PaneInfo&) {
            auto& pairs = *static_cast<decltype(replacements)*>(context);
            pairs.emplace_back(pane, Layout::PaneID { static_cast<std::uint32_t>(101 + pairs.size()) }); }, &replacements, Layout::RestorePlacement::Append));
    CHECK(Session::rebindSharedPanes(layout, replacements));
    CHECK_EQ(layout.surfaceCount(), 2);
    CHECK(layout.focusedPane(existing) == existingPane);
    const auto shared = layout.activeSurface();
    CHECK(layout.focusedPane(shared) == Layout::PaneID { 102 });
    CHECK(layout.zoomedPane(shared) == Layout::PaneID { 102 });
    CHECK_EQ(layout.tabCount(shared), 2);
    const auto* tree = layout.paneTree(shared, layout.activeTab(shared));
    CHECK(tree->holdsPane(101));
    CHECK(tree->holdsPane(102));
    CHECK(tree->node(tree->root()).ratio > 0.29f && tree->node(tree->root()).ratio < 0.31f);
    const std::array duplicate { std::pair { Layout::PaneID { 101 }, existingPane } };
    CHECK(!Session::rebindSharedPanes(layout, duplicate));
    CHECK(layout.focusedPane(existing) == existingPane);
}

UBIQUITTY_TEST_MAIN()
