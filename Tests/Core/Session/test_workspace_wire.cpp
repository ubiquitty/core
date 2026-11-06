// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace_wire — a client's mirror of a workspace is rebuilt from the
// structure record the host sends, with the host's ids.
//
// The host's side is its own: a Layout::Workspace, the structure a host
// builds of it (buildSessionTree) and the record that carries it
// (encodeWorkspaceStructure). The cases build a model with every construct a
// mirror holds — two windows, a second tab, a split, a zoom, an active choice
// that is not the first, a tab title — send its structure through the codec,
// adopt it, and hold the mirror to the model as text: same windows, tabs,
// panes, splits and choices under the same ids, and the same ids minted next.

#include "WorkspaceMirrorFixture.h"

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/WorkspaceCapture.h"
#include "Core/Session/WorkspaceWire.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Session::Layout;
using Ubiquitty::Session::TreeWorkspace;
using UbiquittyTest::MirrorPane;

namespace {

/// A host that names each pane's session as ten times its id and gives each
/// a grid that names it, so a session or a size in a mirror says which pane
/// produced it.
MirrorPane hostPane(PaneID pane)
{
    const int id = static_cast<int>(std::to_underlying(pane));
    return { .sessionID = std::to_underlying(pane) * 10, .columns = 40 + id, .rows = 12 + id };
}

/// What a mirror was told about its panes' sessions.
struct Bindings {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> panes;

    /// The pane lookup a mirror's text is written with: the session it was
    /// told, and the host's grid for the pane.
    [[nodiscard]] UbiquittyTest::MirrorPaneLookup lookup() const
    {
        return [this](PaneID pane) {
            MirrorPane found = hostPane(pane);
            found.sessionID = 0;
            for (const auto& [bound, session] : panes) {
                if (bound == std::to_underlying(pane))
                    found.sessionID = session;
            }
            return found;
        };
    }
};

void recordBinding(void* context, PaneID pane, std::uint32_t sessionID)
{
    static_cast<Bindings*>(context)->panes.emplace_back(std::to_underlying(pane), sessionID);
}

/// The structure of a model as a client reads it: built as a host builds it,
/// encoded, and decoded.
std::optional<TreeWorkspace> received(const Workspace& live, std::string name = "shared")
{
    const TreeWorkspace built = UbiquittyTest::structureOf(live, { .workspaceID = 7, .name = std::move(name), .wasRestoredAfterHostRestart = false, .daemonInstanceID = 17 }, hostPane);
    std::vector<std::uint8_t> record;
    if (!Ubiquitty::Session::encodeWorkspaceStructure(record, built))
        return std::nullopt;
    return Ubiquitty::Session::decodeWorkspaceStructure(record);
}

/// Two windows, a second tab, a split, a zoom, both active choices away from
/// the defaults, and focus on the split's first pane rather than the new one
/// a split leaves focused: every construct a mirror holds.
std::unique_ptr<Workspace> everyConstruct()
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID second = live->addSurface();
    const TabID secondTab = live->addTab(second);
    live->setActiveTab(second, secondTab);
    const PaneID original = live->focusedPane(second);
    const PaneID split = live->splitFocusedPane(second, Ubiquitty::Panes::Orientation::Vertical, 0.5f);
    live->toggleZoom(second, split);
    live->focusPane(second, original);
    live->setActiveSurface(second);
    return live;
}

} // namespace

TEST(aFreshWorkspaceIsMirroredAsOneWindowOneTabOnePane)
{
    auto live = std::make_unique<Workspace>();
    const std::optional<TreeWorkspace> structure = received(*live);
    REQUIRE(structure.has_value());
    auto mirror = std::make_unique<Workspace>();
    Bindings bindings;
    REQUIRE(adoptWorkspace(*structure, *mirror, recordBinding, &bindings));
    CHECK_EQ(mirror->surfaceCount(), 1);
    const SurfaceID surface = live->surfaceIDAt(0);
    CHECK(mirror->surfaceIDAt(0) == surface);
    CHECK(mirror->activeTab(surface) == live->activeTab(surface));
    CHECK(mirror->focusedPane(surface) == live->focusedPane(surface));
    REQUIRE(bindings.panes.size() == 1);
    const std::uint32_t pane = std::to_underlying(live->focusedPane(surface));
    CHECK_EQ(bindings.panes[0].first, pane);
    CHECK_EQ(bindings.panes[0].second, pane * 10);
    CHECK(UbiquittyTest::mirrorText(*mirror, bindings.lookup()) == UbiquittyTest::mirrorText(*live, hostPane));
}

TEST(theStructureIsAdoptedAsAMirrorWithTheSameIDs)
{
    auto live = everyConstruct();
    const std::optional<TreeWorkspace> structure = received(*live);
    REQUIRE(structure.has_value());

    auto mirror = std::make_unique<Workspace>();
    Bindings bindings;
    REQUIRE(adoptWorkspace(*structure, *mirror, recordBinding, &bindings));
    CHECK_EQ(bindings.panes.size(), 4u);
    const std::string host = UbiquittyTest::mirrorText(*live, hostPane);
    CHECK_MSG(UbiquittyTest::mirrorText(*mirror, bindings.lookup()) == host, (UbiquittyTest::mirrorText(*mirror, bindings.lookup()) + "\nis not\n" + host).c_str());
    // The text holds every construct: a second window active, its second tab
    // active, a zoom, and a split of two panes side by side.
    const SurfaceID second = live->activeSurface();
    CHECK(mirror->activeSurface() == second);
    CHECK(mirror->zoomedPane(second) != invalidPane);
    CHECK(host.find('{') != std::string::npos);

    // Nothing was ever closed in either, so what the mirror mints next is what
    // the original mints next: a verb the mirror predicts names the host's id.
    CHECK(mirror->splitFocusedPane(second, Ubiquitty::Panes::Orientation::Horizontal) == live->splitFocusedPane(second, Ubiquitty::Panes::Orientation::Horizontal));
    CHECK(mirror->addTab(second) == live->addTab(second));
    CHECK(mirror->addSurface() == live->addSurface());
}

// A divider is where the host's cells put it: the mirror's tree is the
// host's, split for split, so a divider a client names by its place in the
// tree is the divider the host holds there.
TEST(aMirrorsSplitsAreTheHostsSplitForSplit)
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID surface = live->surfaceIDAt(0);
    const PaneID first = live->focusedPane(surface);
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical, 0.5f) != invalidPane);
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Horizontal, 0.5f) != invalidPane);
    REQUIRE(live->focusPane(surface, first));
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Horizontal, 0.5f) != invalidPane);
    const std::optional<TreeWorkspace> structure = received(*live);
    REQUIRE(structure.has_value());
    auto mirror = std::make_unique<Workspace>();
    Bindings bindings;
    REQUIRE(adoptWorkspace(*structure, *mirror, recordBinding, &bindings));
    CHECK(UbiquittyTest::mirrorText(*mirror, bindings.lookup()) == UbiquittyTest::mirrorText(*live, hostPane));
    const Ubiquitty::Panes::Tree* hostTree = live->paneTree(surface, live->activeTab(surface));
    const Ubiquitty::Panes::Tree* mirrorTree = mirror->paneTree(surface, mirror->activeTab(surface));
    REQUIRE(hostTree && mirrorTree);
    CHECK_EQ(mirrorTree->paneCount(), hostTree->paneCount());
}

// The host's quarter split and its settled cells agree.
TEST(aMirrorsDividerStandsWhereTheHostsCellsPutIt)
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID surface = live->surfaceIDAt(0);
    const PaneID narrow = live->focusedPane(surface);
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical, 0.25f) != invalidPane);
    const auto unequal = [narrow](PaneID pane) { return MirrorPane { .sessionID = std::to_underlying(pane) * 10, .columns = pane == narrow ? 30 : 90, .rows = 24 }; };
    const TreeWorkspace built = UbiquittyTest::structureOf(*live, { .workspaceID = 7, .name = "shared", .wasRestoredAfterHostRestart = false, .daemonInstanceID = 17 }, unequal);
    std::vector<std::uint8_t> record;
    REQUIRE(Ubiquitty::Session::encodeWorkspaceStructure(record, built));
    const std::optional<TreeWorkspace> structure = Ubiquitty::Session::decodeWorkspaceStructure(record);
    REQUIRE(structure.has_value());
    auto mirror = std::make_unique<Workspace>();
    Bindings bindings;
    REQUIRE(adoptWorkspace(*structure, *mirror, recordBinding, &bindings));
    const Ubiquitty::Panes::Tree* mirrorTree = mirror->paneTree(surface, mirror->activeTab(surface));
    REQUIRE(mirrorTree != nullptr);
    const auto& root = structure->windows[0].tabs[0].split;
    REQUIRE(root.children.size() == 2);
    const double share = static_cast<double>(root.children[0].columns) / (root.children[0].columns + root.children[1].columns);
    const double mirrored = mirrorTree->node(mirrorTree->root()).ratio;
    CHECK_MSG(share > 0.2 && share < 0.3, "premise: the host's cells put the divider a quarter of the way across");
    CHECK_MSG(mirrored > share - 0.005 && mirrored < share + 0.005, ("the mirror's divider is at " + std::to_string(mirrored) + ", the host's cells put it at " + std::to_string(share)).c_str());
}

TEST(aMirrorKeepsCanonicalRatiosAcrossAsynchronousPaneGeometry)
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID surface = live->activeSurface();
    const PaneID first = live->focusedPane(surface);
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical, 0.5f) != invalidPane);
    REQUIRE(live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Horizontal, 0.3f) != invalidPane);
    auto mirror = std::make_unique<Workspace>();
    // A new viewport arrives for one pane before its peers. Its actual PTY
    // grid does not redefine the host's layout ratio on the way back.
    for (const int columns : { 39, 60, 75, 39 }) {
        const auto grids = [first, columns](PaneID pane) { return MirrorPane { .sessionID = std::to_underlying(pane) * 10, .columns = pane == first ? columns : 80, .rows = pane == first ? 22 : 24 }; };
        const auto built = UbiquittyTest::structureOf(*live, { .workspaceID = 7, .name = "shared", .wasRestoredAfterHostRestart = false, .daemonInstanceID = 17 }, grids);
        std::vector<std::uint8_t> record;
        REQUIRE(Ubiquitty::Session::encodeWorkspaceStructure(record, built));
        const auto structure = Ubiquitty::Session::decodeWorkspaceStructure(record);
        REQUIRE(structure);
        Bindings bindings;
        REQUIRE(adoptWorkspace(*structure, *mirror, recordBinding, &bindings));
        const auto* tree = mirror->paneTree(surface, mirror->activeTab(surface));
        REQUIRE(tree);
        CHECK(tree->node(tree->root()).ratio == 0.5f);
        CHECK(tree->node(tree->node(tree->root()).second).ratio == 0.3f);
        auto changedRatio = *structure;
        changedRatio.windows[0].tabs[0].split.ratio = 0.6;
        CHECK(mirrorSignature(changedRatio) != mirrorSignature(*structure));
    }
}

TEST(aStructureThatDoesNotHoldTogetherIsRefused)
{
    auto live = everyConstruct();
    const std::optional<TreeWorkspace> sent = received(*live);
    REQUIRE(sent.has_value());
    const auto refused = [&](const TreeWorkspace& broken) {
        auto mirror = std::make_unique<Workspace>();
        const PaneID before = mirror->focusedPane(mirror->activeSurface());
        Bindings bindings;
        const bool wasAdopted = adoptWorkspace(broken, *mirror, recordBinding, &bindings); // bool-ok: the adopt's verdict
        // Refused whole: the mirror is the fresh workspace it was, told nothing.
        return !wasAdopted && bindings.panes.empty() && mirror->surfaceCount() == 1 && mirror->focusedPane(mirror->activeSurface()) == before;
    };
    {
        Bindings bindings;
        auto mirror = std::make_unique<Workspace>();
        REQUIRE(adoptWorkspace(*sent, *mirror, recordBinding, &bindings));
    }
    TreeWorkspace twice = *sent;
    twice.windows[1].tabs[1].panes[1].id = twice.windows[1].tabs[1].panes[0].id;
    CHECK(refused(twice));
    for (const double ratio : { -0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        TreeWorkspace invalidRatio = *sent;
        invalidRatio.windows[1].tabs[1].split.ratio = ratio;
        CHECK(refused(invalidRatio));
    }
    TreeWorkspace shortList = *sent;
    shortList.windows[1].tabs[1].panes.pop_back();
    CHECK(refused(shortList));
    TreeWorkspace longList = *sent;
    longList.windows[1].tabs[1].panes.push_back(longList.windows[1].tabs[1].panes[0]);
    longList.windows[1].tabs[1].panes.back().id = PaneID { 999 };
    CHECK(refused(longList));
    TreeWorkspace focusOnNoPane = *sent;
    focusOnNoPane.windows[1].tabs[1].hostFocusedPane = 9;
    CHECK(refused(focusOnNoPane));
    TreeWorkspace activeTabUnknown = *sent;
    activeTabUnknown.windows[1].hostActiveTab = 9;
    CHECK(refused(activeTabUnknown));
    TreeWorkspace activeWindowUnknown = *sent;
    activeWindowUnknown.hostActiveWindow = 9;
    CHECK(refused(activeWindowUnknown));
    TreeWorkspace zoomOnNoPane = *sent;
    zoomOnNoPane.windows[1].hostZoomedPane = Ubiquitty::Session::TreeZoom { .tab = 1, .pane = 9 };
    CHECK(refused(zoomOnNoPane));
    TreeWorkspace tabless = *sent;
    tabless.windows[0].tabs.clear();
    CHECK(refused(tabless));
    TreeWorkspace paneless = *sent;
    paneless.windows[0].tabs[0].panes.clear();
    CHECK(refused(paneless));
    TreeWorkspace windowless = *sent;
    windowless.windows.clear();
    CHECK(refused(windowless));
}

// What a mirror shows is the same text for two structures exactly when a
// mirror of either is the same: a revision that only says who is attached
// leaves it, and one that moves a divider, a focus, a title or a name
// changes it.
TEST(theMirrorSignatureMovesWithWhatAMirrorShowsAndNothingElse)
{
    auto live = everyConstruct();
    const std::optional<TreeWorkspace> sent = received(*live);
    REQUIRE(sent.has_value());
    const std::string signature = mirrorSignature(*sent);
    TreeWorkspace attended = *sent;
    attended.revision += 5;
    attended.windows[1].tabs[1].panes[0].attendees.push_back({});
    attended.windows[1].tabs[1].panes[0].sessionLabel = "build";
    CHECK(mirrorSignature(attended) == signature);

    TreeWorkspace labelled = *sent;
    labelled.label = "release";
    CHECK(mirrorSignature(labelled) != signature);
    labelled = *sent;
    labelled.windows[1].label = "Editor";
    CHECK(mirrorSignature(labelled) != signature);
    labelled = *sent;
    labelled.windows[1].tabs[1].label = "source";
    CHECK(mirrorSignature(labelled) != signature);

    TreeWorkspace renamed = *sent;
    renamed.name = "other";
    CHECK(mirrorSignature(renamed) != signature);
    TreeWorkspace retitled = *sent;
    retitled.windows[1].tabs[1].name = "Logs";
    CHECK(mirrorSignature(retitled) != signature);
    TreeWorkspace refocused = *sent;
    refocused.windows[1].tabs[1].hostFocusedPane = 1;
    CHECK(mirrorSignature(refocused) != signature);
    TreeWorkspace unzoomed = *sent;
    unzoomed.windows[1].hostZoomedPane.reset();
    CHECK(mirrorSignature(unzoomed) != signature);
    TreeWorkspace resized = *sent;
    resized.windows[1].tabs[1].split.children[0].columns += 3;
    CHECK(mirrorSignature(resized) != signature);
    TreeWorkspace rebound = *sent;
    rebound.windows[1].tabs[1].panes[0].sessionID += 1;
    CHECK(mirrorSignature(rebound) != signature);
    TreeWorkspace restored = *sent;
    restored.state = Ubiquitty::Session::TreeWorkspaceState::RestoredAfterHostRestart;
    CHECK(mirrorSignature(restored) != signature);
}

// How a structure arranges its panes moves with what an operation on the tree
// or a question about it names, and with nothing a pane's cells or its
// attendees say: a pane the zoom resized still stands where it stood.
TEST(aRecordSaysTheArrangementOfTheWorkspaceItWasMadeFrom)
{
    // A host handed a record over knows the arrangement its revision says
    // from the record alone: read back, it signs as the workspace it was
    // made from, every construct and every name and label included.
    auto live = everyConstruct();
    TreeWorkspace built = UbiquittyTest::structureOf(*live, { .workspaceID = 7, .name = "shared", .wasRestoredAfterHostRestart = false, .daemonInstanceID = 17 }, hostPane);
    built.label = "release";
    built.windows[1].label = "logs window";
    built.windows[1].tabs[1].name = "Logs";
    built.windows[1].tabs[1].label = "tail";
    std::vector<std::uint8_t> record;
    REQUIRE(Ubiquitty::Session::encodeWorkspaceStructure(record, built));
    const std::optional<TreeWorkspace> read = Ubiquitty::Session::decodeWorkspaceStructure(record);
    REQUIRE(read.has_value());
    CHECK(arrangementSignature(*read) == arrangementSignature(built));
}

TEST(theArrangementSignatureMovesWithTheArrangementAndNothingElse)
{
    auto live = everyConstruct();
    const std::optional<TreeWorkspace> sent = received(*live);
    REQUIRE(sent.has_value());
    const std::string signature = arrangementSignature(*sent);

    TreeWorkspace unmoved = *sent;
    unmoved.revision += 5;
    unmoved.windows[1].tabs[1].panes[0].attendees.push_back({});
    unmoved.windows[1].tabs[1].split.children[0].columns += 3;
    unmoved.windows[1].tabs[1].split.children[0].rows += 2;
    unmoved.windows[1].tabs[1].panes[0].facts.rows += 2;
    unmoved.windows[1].tabs[1].panes[0].sessionLabel = "build";
    CHECK(arrangementSignature(unmoved) == signature);

    TreeWorkspace renamed = *sent;
    renamed.windows[1].tabs[1].name = "Logs";
    CHECK(arrangementSignature(renamed) != signature);
    TreeWorkspace labelled = *sent;
    labelled.label = "release";
    CHECK(arrangementSignature(labelled) != signature);
    TreeWorkspace rebound = *sent;
    rebound.windows[1].tabs[1].panes[0].sessionID += 1;
    CHECK(arrangementSignature(rebound) != signature);

    TreeWorkspace refocused = *sent;
    refocused.windows[1].tabs[1].hostFocusedPane = 1;
    CHECK(arrangementSignature(refocused) != signature);
    TreeWorkspace unzoomed = *sent;
    unzoomed.windows[1].hostZoomedPane.reset();
    CHECK(arrangementSignature(unzoomed) != signature);
    TreeWorkspace reactivated = *sent;
    reactivated.windows[1].hostActiveTab = 0;
    CHECK(arrangementSignature(reactivated) != signature);
    TreeWorkspace divided = *sent;
    divided.windows[1].tabs[1].split.ratio = 0.25;
    CHECK(arrangementSignature(divided) != signature);
    TreeWorkspace swapped = *sent;
    std::swap(swapped.windows[1].tabs[1].panes[0].id, swapped.windows[1].tabs[1].panes[1].id);
    CHECK(arrangementSignature(swapped) != signature);
}

// A layout operation stays current until the arrangement changes after the
// revision it was asked under; closing stays bound to the exact revision.
TEST(aLayoutOperationStaysCurrentUntilTheArrangementChanges)
{
    using Ubiquitty::Protocol::Sync::isStructureRevisionCurrent;
    CHECK(isStructureRevisionCurrent("pane.zoom", 7, 9, 7));
    CHECK(isStructureRevisionCurrent("window.moveFocus", 8, 9, 7));
    CHECK(!isStructureRevisionCurrent("pane.zoom", 6, 9, 7));
    CHECK(!isStructureRevisionCurrent("pane.zoom", 0, 9, 7));
    CHECK(!isStructureRevisionCurrent("pane.zoom", 10, 9, 7));
    for (const auto* closing : { "window.close", "workspace.closeTab", "workspace.closePane" }) {
        CHECK(isStructureRevisionCurrent(closing, 9, 9, 7));
        CHECK(!isStructureRevisionCurrent(closing, 8, 9, 7));
    }
}

TEST(theMirrorSignatureKeepsTextBoundariesUnambiguous)
{
    auto live = everyConstruct();
    const auto sent = received(*live);
    REQUIRE(sent);
    TreeWorkspace first = *sent;
    TreeWorkspace second = *sent;
    first.name = "work\nline";
    first.label = "tag";
    second.name = "work";
    second.label = "\nlinetag";
    CHECK(mirrorSignature(first) != mirrorSignature(second));

    first = *sent;
    second = *sent;
    first.windows[0].tabs[0].name = "Editor\nt1 0 ";
    first.windows[0].tabs[0].label = "source:7";
    second.windows[0].tabs[0].name = "Editor";
    second.windows[0].tabs[0].label = "\nt1 0 source:7";
    CHECK(mirrorSignature(first) != mirrorSignature(second));
}

TEST(testTabTitlesSurviveProjectionAndSlotReuse)
{
    auto live = std::make_unique<Workspace>();
    const auto surface = live->surfaceIDAt(0);
    const auto first = live->activeTab(surface);
    CHECK(live->renameTab(surface, first, "Build — 日本語"));
    const auto second = live->addTab(surface);
    CHECK(live->renameTab(surface, second, "Logs"));
    const std::optional<TreeWorkspace> structure = received(*live);
    REQUIRE(structure.has_value());
    CHECK(structure->windows[0].tabs[0].name == "Build — 日本語");
    auto mirror = std::make_unique<Workspace>();
    CHECK(adoptWorkspace(*structure, *mirror, [](void*, PaneID, std::uint32_t) { }, nullptr));
    CHECK(mirror->tabTitle(surface, first) == "Build — 日本語");
    CHECK(mirror->tabTitle(surface, second) == "Logs");
    CHECK(mirror->closeTab(surface, second));
    const auto replacement = mirror->addTab(surface);
    CHECK(mirror->tabTitle(surface, replacement).empty());
    CHECK(mirror->renameTab(surface, first, ""));
    CHECK(mirror->tabTitle(surface, first).empty());
    CHECK(!mirror->renameTab(surface, second, "closed"));
}

namespace {

/// A mirror adopted from a model's structure, with what it was told of its
/// panes' sessions.
struct AdoptedMirror {
    std::unique_ptr<Workspace> workspace = std::make_unique<Workspace>();
    Bindings bindings;
};

std::optional<AdoptedMirror> mirrorOf(const Workspace& live)
{
    AdoptedMirror mirror;
    const std::optional<TreeWorkspace> structure = received(live);
    if (!structure || !adoptWorkspace(*structure, *mirror.workspace, recordBinding, &mirror.bindings))
        return std::nullopt;
    return mirror;
}

PaneInfo noPaneInfo(void*, PaneID) { return {}; }

void ignoreRestoredPane(void*, PaneID, const PaneInfo&) { }

/// What a host that took in `stopped`'s record restores, under ids of its
/// own: it already holds a workspace, so its ids run on past the ones the
/// stopped host minted, and some land on numbers the stopped host gave
/// other panes.
///
/// - Returns: The restored workspace, or null when the record did not restore.
std::unique_ptr<Workspace> restoredBySuccessor(const Workspace& stopped)
{
    auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    auto occupied = std::make_unique<Workspace>(identities);
    (void)occupied->addSurface();
    auto restored = std::make_unique<Workspace>(identities);
    if (!restoreFromSnapshot(captureToSnapshot(stopped, noPaneInfo, nullptr), *restored, ignoreRestoredPane, nullptr))
        return nullptr;
    return restored;
}

/// The place of a pane in a mirror: its window, its tab, and its leaf first
/// child first.
std::optional<std::array<int, 3>> placeOf(const Workspace& mirror, SurfaceID surface, TabID tab, PaneID pane)
{
    for (int surfaceIndex = 0; surfaceIndex < mirror.surfaceCount(); ++surfaceIndex) {
        if (mirror.surfaceIDAt(surfaceIndex) != surface)
            continue;
        for (int tabIndex = 0; tabIndex < mirror.tabCount(surface); ++tabIndex) {
            if (mirror.tabAt(surface, tabIndex) != tab)
                continue;
            int leaf = 0;
            std::optional<std::array<int, 3>> found;
            Ubiquitty::Panes::forEachLeaf(*mirror.paneTree(surface, tab), [&](int id) {
                if (static_cast<std::uint32_t>(id) == std::to_underlying(pane))
                    found = std::array { surfaceIndex, tabIndex, leaf };
                ++leaf;
            });
            return found;
        }
    }
    return std::nullopt;
}

/// One window of one tab: a pane beside two stacked ones.
std::unique_ptr<Workspace> paneBesideAStack()
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID surface = live->surfaceIDAt(0);
    (void)live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical, 0.5f);
    (void)live->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Horizontal, 0.5f);
    return live;
}

/// The panes of a workspace's first tab, first child first.
std::vector<PaneID> leavesOf(const Workspace& workspace)
{
    std::vector<PaneID> leaves;
    const SurfaceID surface = workspace.surfaceIDAt(0);
    if (const Ubiquitty::Panes::Tree* tree = workspace.paneTree(surface, workspace.tabAt(surface, 0)))
        Ubiquitty::Panes::forEachLeaf(*tree, [&](int pane) { leaves.push_back(PaneID { static_cast<std::uint32_t>(pane) }); });
    return leaves;
}

} // namespace

// A host that took in the record of a host that stopped restores its
// workspace with ids and sessions of its own. Each pane a client's mirror
// held pairs with the pane restored in its place, and none by number.
TEST(aRestoredWorkspacesPanesPairWithThePanesInTheirPlaces)
{
    auto stopped = everyConstruct();
    auto restored = restoredBySuccessor(*stopped);
    REQUIRE(restored);
    const std::optional<AdoptedMirror> before = mirrorOf(*stopped);
    const std::optional<AdoptedMirror> after = mirrorOf(*restored);
    REQUIRE(before && after);
    // The premise: the restoration renumbered the window, tab and pane ids.
    REQUIRE(UbiquittyTest::mirrorText(*before->workspace, before->bindings.lookup()) != UbiquittyTest::mirrorText(*after->workspace, after->bindings.lookup()));

    const std::vector<RestoredPane> pairs = pairRestoredPanes(*before->workspace, before->bindings.panes, *after->workspace, after->bindings.panes);
    REQUIRE(pairs.size() == before->bindings.panes.size());
    bool isAnyRenumbered = false;
    for (const RestoredPane& pair : pairs) {
        const auto formerPlace = placeOf(*before->workspace, pair.formerSurface, pair.formerTab, pair.formerPane);
        const auto place = placeOf(*after->workspace, pair.surface, pair.tab, pair.pane);
        REQUIRE(formerPlace.has_value() && place.has_value());
        CHECK(*formerPlace == *place);
        CHECK_EQ(pair.formerSessionID, std::to_underlying(pair.formerPane) * 10);
        CHECK_EQ(pair.sessionID, std::to_underlying(pair.pane) * 10);
        isAnyRenumbered = isAnyRenumbered || pair.formerPane != pair.pane || pair.formerTab != pair.tab;
    }
    CHECK(isAnyRenumbered);
}

// A pane the restoring host could not start leaves its tab split another
// way. Of two panes side by side that lost one, the one left could be
// either, so that tab's panes pair with none; every other tab's still pair.
TEST(aRestoredTabThatLostAPanePairsNone)
{
    auto stopped = everyConstruct();
    auto restored = restoredBySuccessor(*stopped);
    REQUIRE(restored);
    const SurfaceID second = restored->surfaceIDAt(1);
    const TabID split = restored->tabAt(second, 1);
    const Ubiquitty::Panes::Tree* tree = restored->paneTree(second, split);
    REQUIRE(tree && tree->paneCount() == 2);
    REQUIRE(restored->removePane(PaneID { static_cast<std::uint32_t>(tree->focusedPaneID()) }));
    const std::optional<AdoptedMirror> before = mirrorOf(*stopped);
    const std::optional<AdoptedMirror> after = mirrorOf(*restored);
    REQUIRE(before && after);

    const std::vector<RestoredPane> pairs = pairRestoredPanes(*before->workspace, before->bindings.panes, *after->workspace, after->bindings.panes);
    CHECK_EQ(pairs.size(), 2u);
    for (const RestoredPane& pair : pairs)
        CHECK(pair.tab != split);
}

// A window none of whose panes the restoring host could start is not
// restored. Only losing its one pane leaves the other window as it was, so
// that window's panes all pair with theirs.
TEST(aRestorationMissingAWindowPairsTheOtherWindowsPanes)
{
    auto stopped = everyConstruct();
    auto restored = restoredBySuccessor(*stopped);
    REQUIRE(restored && restored->closeSurface(restored->surfaceIDAt(0)));
    const std::optional<AdoptedMirror> before = mirrorOf(*stopped);
    const std::optional<AdoptedMirror> after = mirrorOf(*restored);
    REQUIRE(before && after);
    const std::vector<RestoredPane> pairs = pairRestoredPanes(*before->workspace, before->bindings.panes, *after->workspace, after->bindings.panes);
    REQUIRE(pairs.size() == 3);
    const SurfaceID kept = before->workspace->surfaceIDAt(1);
    for (const RestoredPane& pair : pairs) {
        CHECK(pair.formerSurface == kept);
        const auto formerPlace = placeOf(*before->workspace, pair.formerSurface, pair.formerTab, pair.formerPane);
        const auto place = placeOf(*after->workspace, pair.surface, pair.tab, pair.pane);
        REQUIRE(formerPlace.has_value() && place.has_value());
        CHECK_EQ((*formerPlace)[0], 1);
        CHECK_EQ((*place)[0], 0);
        CHECK_EQ((*formerPlace)[1], (*place)[1]);
        CHECK_EQ((*formerPlace)[2], (*place)[2]);
    }
}

// Inside one tab, a lost pane is told apart from its neighbours wherever
// the split it left can only have come from losing it: the panes that kept
// their places pair, and only a place two ways of losing a pane disagree on
// is left unpaired.
TEST(aRestoredTabThatLostAPaneKeepsThePanesOnlyThatLossLeaves)
{
    for (const std::size_t lost : { std::size_t { 0 }, std::size_t { 1 } }) {
        auto stopped = paneBesideAStack();
        auto restored = restoredBySuccessor(*stopped);
        REQUIRE(restored);
        const std::vector<PaneID> formerPanes = leavesOf(*stopped);
        const std::vector<PaneID> restoredPanes = leavesOf(*restored);
        REQUIRE(formerPanes.size() == 3 && restoredPanes.size() == 3);
        REQUIRE(restored->removePane(restoredPanes[lost]));
        const std::optional<AdoptedMirror> before = mirrorOf(*stopped);
        const std::optional<AdoptedMirror> after = mirrorOf(*restored);
        REQUIRE(before && after);
        const std::vector<RestoredPane> pairs = pairRestoredPanes(*before->workspace, before->bindings.panes, *after->workspace, after->bindings.panes);
        if (lost == 0) {
            // Two stacked panes are left, which only losing the one beside
            // them leaves.
            REQUIRE(pairs.size() == 2);
            CHECK(pairs[0].formerPane == formerPanes[1] && pairs[0].pane == restoredPanes[1]);
            CHECK(pairs[1].formerPane == formerPanes[2] && pairs[1].pane == restoredPanes[2]);
            CHECK_EQ(pairs[0].sessionID, std::to_underlying(restoredPanes[1]) * 10);
        } else {
            // Two panes side by side are left: the first is the first either
            // way, and the second is whichever of the stacked two survived.
            REQUIRE(pairs.size() == 1);
            CHECK(pairs[0].formerPane == formerPanes[0] && pairs[0].pane == restoredPanes[0]);
            CHECK_EQ(pairs[0].formerSessionID, std::to_underlying(formerPanes[0]) * 10);
        }
    }
}

// A frontend that did not take one restoration's pairs before the next holds
// the ids from before the first: each pair is followed through the second.
TEST(anUntakenRestorationIsFollowedThroughTheNext)
{
    const RestoredPane first { .formerSurface = SurfaceID { 1 }, .formerTab = TabID { 1 }, .formerPane = PaneID { 1 }, .formerSessionID = 1, .surface = SurfaceID { 4 }, .tab = TabID { 4 }, .pane = PaneID { 4 }, .sessionID = 4 };
    const RestoredPane dropped { .formerSurface = SurfaceID { 1 }, .formerTab = TabID { 1 }, .formerPane = PaneID { 2 }, .formerSessionID = 2, .surface = SurfaceID { 4 }, .tab = TabID { 4 }, .pane = PaneID { 5 }, .sessionID = 5 };
    const RestoredPane second { .formerSurface = SurfaceID { 4 }, .formerTab = TabID { 4 }, .formerPane = PaneID { 4 }, .formerSessionID = 4, .surface = SurfaceID { 9 }, .tab = TabID { 9 }, .pane = PaneID { 9 }, .sessionID = 9 };

    const Restoration once = followRestoration({}, std::array { first, dropped });
    CHECK(once.isRestored);
    const std::vector<RestoredPane> both { first, dropped };
    CHECK(once.panes == both);

    const Restoration twice = followRestoration(once, std::array { second });
    CHECK(twice.isRestored);
    REQUIRE(twice.panes.size() == 1);
    CHECK(twice.panes[0].formerPane == PaneID { 1 });
    CHECK_EQ(twice.panes[0].formerSessionID, 1u);
    CHECK(twice.panes[0].pane == PaneID { 9 });
    CHECK_EQ(twice.panes[0].sessionID, 9u);

    // One that paired nothing leaves nothing to follow.
    const Restoration none = followRestoration({ .isRestored = true, .panes = {} }, std::array { second });
    CHECK(none.isRestored);
    CHECK(none.panes.empty());
}

UBIQUITTY_TEST_MAIN()
