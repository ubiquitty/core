// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/SessionTreeWire.h"
#include "Harness.h"
#include "Platform/Host/HandoverTables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;
namespace Layout = Session::Layout;
namespace Identity = Session::Identity;
namespace Sync = Protocol::Sync;

namespace {

constexpr HandoverWorkspaceLimits limits { 1024 * 1024, 64, 128, 256, 1024, 128, 65536, 65536 };

std::vector<std::uint8_t> bytesOf(const Sync::HandoverBytes& bytes) { return { bytes.view().begin(), bytes.view().end() }; }

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}

void bindPanes(HandoverWorkspace& state)
{
    state.paneSessions.clear();
    for (const auto& window : state.windows) {
        for (const auto& tab : window.tabs) {
            for (int node = 0; node < Panes::maxNodes; ++node) {
                if (tab.panes.used[node] && tab.panes.nodes[node].first == Panes::invalidNode)
                    state.paneSessions.emplace_back(tab.panes.nodes[node].paneID, tab.panes.nodes[node].paneID + 100);
            }
        }
    }
}

HandoverWorkspaceTables stateOf(const Layout::Workspace& layout, const Identity::Allocator& allocator)
{
    HandoverWorkspaceTables result;
    result.identities = allocator.continuation();
    result.nextSessionID = 1000;
    result.nextClientID = 500;
    result.nextStructureConsentID = 97;
    result.workspaces.emplace_back();
    auto& state = result.workspaces.back();
    state.id = 7;
    state.name = "live";
    state.wasRestoredAfterHostRestart = true;
    captureHandoverLayout(layout, state);
    bindPanes(state);
    result.layoutRecordPath = "/private/owned/host.layouts";
    result.layoutRecordDueMilliseconds = 1249;
    result.layoutRecordSignMilliseconds = 1100;
    result.writtenLayoutRecords = { 0, 1, 0, 255 };
    result.isLayoutRecordKept = true;
    result.isStructureDirty = true;
    return result;
}

Session::HostLayoutRecord recipe()
{
    Session::HostLayoutRecord result;
    result.arrangement.name = "saved";
    Session::WorkspaceWindow window;
    window.zoomedPaneIndex = 1;
    Session::WorkspaceTab tab;
    tab.title = "stored title";
    tab.focusedPaneIndex = 1;
    auto first = Session::PaneNode::makeLeaf({ Transport::OriginKind::SSH, "example.invalid" }, "first");
    first.directory = "/tmp/one";
    first.ratio = 0.739123456789; // Latent scalar is carried, not written through the disk recipe's quantizer.
    auto second = Session::PaneNode::makeLeaf({ Transport::OriginKind::Local, {} }, "second");
    second.directory = "/tmp/two";
    tab.root = Session::PaneNode::makeSplit(true, 0.123456789123, std::move(first), std::move(second));
    window.tabs.push_back(std::move(tab));
    result.arrangement.windows.push_back(std::move(window));
    result.workspaceLabel = "saved label";
    result.workspaceTags = { "Green", "build" };
    result.windowLabels = { "main" };
    result.tabLabels = { "work" };
    result.windowTags = { { "outer" } };
    result.tabTags = { { "inner" } };
    result.panes = { { 80, 24, "pane", "session", { "one" }, { "two" } }, { 120, 40, {}, {}, {}, {} } };
    return result;
}

void compareTrees(const Panes::Tree::Continuation& first, const Panes::Tree::Continuation& second)
{
    CHECK_EQ(first.root, second.root);
    CHECK_EQ(first.focused, second.focused);
    CHECK_EQ(first.paneCount, second.paneCount);
    CHECK_EQ(first.nodeCount, second.nodeCount);
    CHECK(first.used == second.used);
    for (int index = 0; index < Panes::maxNodes; ++index) {
        if (!first.used[index])
            continue;
        const auto& a = first.nodes[index];
        const auto& b = second.nodes[index];
        CHECK_EQ(a.parent, b.parent);
        CHECK_EQ(a.first, b.first);
        CHECK_EQ(a.second, b.second);
        CHECK_EQ(a.paneID, b.paneID);
        CHECK(a.orientation == b.orientation);
        CHECK_EQ(std::bit_cast<std::uint32_t>(a.ratio), std::bit_cast<std::uint32_t>(b.ratio));
    }
}

} // namespace

TEST(liveWorkspaceRetainsIdentityHighWatersAndTheNextSplit)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto source = std::make_unique<Layout::Workspace>(identities);
    const auto window = source->activeSurface();
    const auto tab = source->activeTab(window);
    const auto removed = source->splitFocusedPane(window, Panes::Orientation::Horizontal, 0.31f);
    CHECK(source->splitFocusedPane(window, Panes::Orientation::Vertical, 0.61f) != Layout::invalidPane);
    CHECK(source->closePane(window, removed));
    const auto endedTab = source->addTab(window);
    CHECK(source->closeTab(window, endedTab));
    CHECK(source->renameTab(window, tab, "kept title"));
    CHECK(source->toggleZoom(window, source->focusedPane(window)));
    const auto second = source->addSurface();
    const auto discarded = source->addTab(second);
    CHECK(source->closeTab(second, discarded));
    CHECK(source->setActiveSurface(window));
    CHECK(second != window);
    auto state = stateOf(*source, *identities);
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    auto decoded = decodeHandoverWorkspaces(encoded->view(), limits);
    REQUIRE(decoded);
    CHECK(decoded->identities == state.identities);
    CHECK_EQ(decoded->nextStructureConsentID, 97u);
    CHECK_EQ(decoded->layoutRecordDueMilliseconds, 1249u);
    CHECK_EQ(decoded->layoutRecordSignMilliseconds, 1100u);
    CHECK(decoded->writtenLayoutRecords == state.writtenLayoutRecords);
    CHECK(decoded->isLayoutRecordKept && decoded->isStructureDirty);
    auto restoredIdentities = std::make_shared<Identity::Allocator>();
    REQUIRE(restoredIdentities->restoreContinuation(decoded->identities));
    auto restored = std::make_unique<Layout::Workspace>(restoredIdentities, Layout::RootIdentity {});
    REQUIRE(restoreHandoverLayout(decoded->workspaces[0], *restored));
    CHECK(restoredIdentities->continuation() == identities->continuation());
    CHECK(restored->zoomedPane(window) == source->zoomedPane(window));
    CHECK(restored->tabTitle(window, tab) == source->tabTitle(window, tab));
    CHECK(restored->splitFocusedPane(window, Panes::Orientation::Horizontal, 0.417f)
        == source->splitFocusedPane(window, Panes::Orientation::Horizontal, 0.417f));
    compareTrees(restored->paneTree(window, tab)->continuation(), source->paneTree(window, tab)->continuation());
    CHECK(restored->addTab(second) == source->addTab(second));
    CHECK(restoredIdentities->continuation() == identities->continuation());
}

TEST(layoutRecipePreservesExactDoublesAndEveryMetadataVector)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    auto state = stateOf(*layout, *identities);
    state.workspaces[0].recordedLayout = recipe();
    state.restorable.push_back({ 19, recipe() });
    state.restorable[0].record.arrangement.name = "dormant";
    state.labels = { { { Identity::ObjectKind::Workspace, 7 }, "workspace" }, { { Identity::ObjectKind::Client, 499 }, "former client" } };
    state.tags = { { { Identity::ObjectKind::Pane, 1 }, { "Red", "second" } } };
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    auto decoded = decodeHandoverWorkspaces(encoded->view(), limits);
    REQUIRE(decoded);
    const auto& actual = *decoded->workspaces[0].recordedLayout;
    const auto& expected = *state.workspaces[0].recordedLayout;
    CHECK_EQ(std::bit_cast<std::uint64_t>(actual.arrangement.windows[0].tabs[0].root.ratio),
        std::bit_cast<std::uint64_t>(expected.arrangement.windows[0].tabs[0].root.ratio));
    CHECK_EQ(std::bit_cast<std::uint64_t>(actual.arrangement.windows[0].tabs[0].root.children[0].ratio),
        std::bit_cast<std::uint64_t>(expected.arrangement.windows[0].tabs[0].root.children[0].ratio));
    CHECK(actual.panes == expected.panes);
    CHECK(actual.windowLabels == expected.windowLabels);
    CHECK(actual.tabLabels == expected.tabLabels);
    CHECK(actual.windowTags == expected.windowTags);
    CHECK(actual.tabTags == expected.tabTags);
    CHECK(decoded->labels == state.labels);
    CHECK(decoded->tags == state.tags);
    CHECK_EQ(decoded->restorable[0].id, 19u);
    const auto repeated = encodeHandoverWorkspaces(*decoded, limits);
    REQUIRE(repeated);
    CHECK(bytesOf(*repeated) == bytesOf(*encoded));
}

TEST(treeContinuationRefusesMalformedGraphWithoutMutatingReceiver)
{
    Panes::Tree source(1);
    CHECK(source.split(Panes::Orientation::Vertical, 2) != Panes::invalidNode);
    const auto valid = source.continuation();
    Panes::Tree receiver(71);
    const auto original = receiver.continuation();
    auto bad = valid;
    bad.nodes[bad.root].first = bad.root;
    CHECK(!receiver.restoreContinuation(bad));
    compareTrees(original, receiver.continuation());
    bad = valid;
    bad.nodes[bad.focused].paneID = 1;
    CHECK(!receiver.restoreContinuation(bad));
    bad = valid;
    bad.used[120] = true;
    CHECK(!receiver.restoreContinuation(bad));
    bad = valid;
    bad.nodes[bad.root].ratio = std::nanf("");
    CHECK(!receiver.restoreContinuation(bad));
    bad = valid;
    bad.focused = bad.root;
    CHECK(!receiver.restoreContinuation(bad));
    CHECK(receiver.restoreContinuation(valid));
    compareTrees(valid, receiver.continuation());
}

TEST(workspaceAdmissionRejectsFutureIDsAndDanglingSelectors)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    const auto valid = stateOf(*layout, *identities);
    auto bad = valid;
    bad.identities.next[std::to_underlying(Identity::ObjectKind::Pane)] = 1;
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.workspaces[0].windows[0].activeTab = 999;
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.workspaces[0].windows[0].zoomedPane = 1;
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.workspaces[0].paneSessions[0].first = 999;
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.workspaces.push_back(valid.workspaces[0]);
    bad.workspaces.back().id = 8;
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.tags = { { { Identity::ObjectKind::Pane, 1 }, { "Red", "red" } } };
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.labels = { { { Identity::ObjectKind::Client, 500 }, "future" } };
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad = valid;
    bad.nextClientID = 0;
    bad.labels = { { { Identity::ObjectKind::Client, static_cast<std::uint64_t>(UINT32_MAX) + 1 }, "too wide" } };
    CHECK(!encodeHandoverWorkspaces(bad, limits));
    bad.labels.clear();
    bad.nextSessionID = 0;
    bad.tags = { { { Identity::ObjectKind::Session, UINT64_MAX }, { "too wide" } } };
    CHECK(!encodeHandoverWorkspaces(bad, limits));
}

// A workspace a host restored under another id than its holders knew it by
// keeps each holder's id across a handover. A body of an earlier version is
// refused rather than read in another layout.
TEST(holdersKeepTheirWorkspaceIDAndEarlierBodiesAreRefused)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    auto state = stateOf(*layout, *identities);
    constexpr std::uint64_t stopped = 0x1122334455667788ull;
    constexpr std::uint64_t earlier = 0x8877665544332211ull;
    state.workspaces[0].formerHolders = { { .daemonInstanceID = stopped, .workspaceID = 3 } };
    state.restorable.push_back({ 19, recipe() });
    state.restorable[0].record.workspaceID = 19;
    state.restorable[0].record.holders = { { .daemonInstanceID = earlier, .workspaceID = 1 } };
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    const auto decoded = decodeHandoverWorkspaces(encoded->view(), limits);
    REQUIRE(decoded);
    CHECK(decoded->workspaces[0].formerHolders == state.workspaces[0].formerHolders);
    CHECK(decoded->restorable[0].record.holders == state.restorable[0].record.holders);

    auto older = bytesOf(*encoded);
    older[0] = 1;
    older[1] = 0;
    redigest(older);
    const auto refused = decodeHandoverWorkspaces(older, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == Sync::HandoverBodyError::Malformed);
}

TEST(workspaceBytesRejectEveryTruncationAndAggregateRefusal)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    const auto state = stateOf(*layout, *identities);
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    const std::array<std::uint8_t, 2> futureVersion { 3, 0 };
    const auto future = decodeHandoverWorkspaces(futureVersion, limits);
    REQUIRE(!future);
    CHECK(future.error() == Sync::HandoverBodyError::NewerVersion);
    for (std::size_t size = 0; size < encoded->size; ++size)
        CHECK(!decodeHandoverWorkspaces(encoded->view().first(size), limits));
    auto changed = bytesOf(*encoded);
    changed.back() ^= 1;
    CHECK(!decodeHandoverWorkspaces(changed, limits));
    changed = bytesOf(*encoded);
    changed.push_back(0);
    CHECK(!decodeHandoverWorkspaces(changed, limits));
    auto exact = limits;
    exact.maxSectionBytes = encoded->size;
    CHECK(decodeHandoverWorkspaces(encoded->view(), exact));
    --exact.maxSectionBytes;
    CHECK(!decodeHandoverWorkspaces(encoded->view(), exact));
    exact = limits;
    exact.maxPanes = 0;
    CHECK(!decodeHandoverWorkspaces(encoded->view(), exact));
    exact = limits;
    exact.maxStringBytes = 1;
    CHECK(!decodeHandoverWorkspaces(encoded->view(), exact));
    exact = limits;
    exact.maxRecordBytes = 3;
    CHECK(!decodeHandoverWorkspaces(encoded->view(), exact));
}

TEST(publishedStructureAndWithdrawnRevisionRemainExact)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    auto state = stateOf(*layout, *identities);
    Session::TreeWorkspace tree;
    tree.id = 7;
    tree.name = "live";
    HandoverStructureRecord structure;
    structure.workspaceID = 7;
    structure.revision = 91;
    REQUIRE(Session::encodeWorkspaceStructure(structure.unrevised, tree));
    tree.revision = 91;
    structure.record.emplace();
    REQUIRE(Session::encodeWorkspaceStructure(*structure.record, tree));
    state.structures = { structure, { 10, 513, {}, std::nullopt } };
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    const auto decoded = decodeHandoverWorkspaces(encoded->view(), limits);
    REQUIRE(decoded);
    CHECK(decoded->structures[0].record == structure.record);
    CHECK(decoded->structures[0].unrevised == structure.unrevised);
    CHECK_EQ(decoded->structures[1].revision, 513u);
    CHECK(!decoded->structures[1].record);
    state.structures[0].revision = 92;
    CHECK(!encodeHandoverWorkspaces(state, limits));
    state.structures[0] = structure;
    state.structures[0].workspaceID = 8;
    CHECK(!encodeHandoverWorkspaces(state, limits));
}

TEST(exhaustedAllocatorsRemainExhaustedAndCannotReplaceLiveOwners)
{
    Identity::Allocator source(2);
    REQUIRE(source.mint(Identity::ObjectKind::Pane));
    REQUIRE(source.mint(Identity::ObjectKind::Pane));
    Identity::Allocator target(2);
    REQUIRE(target.restoreContinuation(source.continuation()));
    CHECK(!target.mint(Identity::ObjectKind::Pane));
    CHECK(!target.restoreContinuation(source.continuation()));
    Identity::Allocator different(3);
    CHECK(!different.restoreContinuation(source.continuation()));
    Identity::Allocator maximal(UINT64_MAX);
    maximal.reserveThrough({ Identity::ObjectKind::Pane, UINT64_MAX });
    Identity::Allocator inherited(UINT64_MAX);
    REQUIRE(inherited.restoreContinuation(maximal.continuation()));
    CHECK(!inherited.mint(Identity::ObjectKind::Pane));
}

TEST(workspaceDecoderRejectsRechecksummedCountsFlagsAndHighWaters)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    const auto state = stateOf(*layout, *identities);
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    auto bad = bytesOf(*encoded);
    // The fixed prefix has version/magic, policy plus six counters, and three IDs.
    constexpr std::size_t workspaceCountOffset = 2 + 4 + 8 * 7 + 4 * 3;
    for (unsigned byte = 0; byte < 4; ++byte)
        bad[workspaceCountOffset + byte] = 255;
    redigest(bad);
    CHECK(!decodeHandoverWorkspaces(bad, limits));
    bad = bytesOf(*encoded);
    constexpr std::size_t firstFlagOffset = workspaceCountOffset + 4 + 4 + 4 + 4; // count, id, name length, "live"
    bad[firstFlagOffset] = 2;
    redigest(bad);
    CHECK(!decodeHandoverWorkspaces(bad, limits));
    bad = bytesOf(*encoded);
    const auto paneCounterOffset = 2 + 4 + 8 + std::to_underlying(Identity::ObjectKind::Pane) * 8;
    for (unsigned byte = 0; byte < 8; ++byte)
        bad[paneCounterOffset + byte] = byte ? 0 : 1;
    redigest(bad);
    CHECK(!decodeHandoverWorkspaces(bad, limits));
}

TEST(layoutRefusalPreservesCandidateAndLabelBooksRetainPhysicalOrder)
{
    auto identities = std::make_shared<Identity::Allocator>();
    auto layout = std::make_unique<Layout::Workspace>(identities);
    auto state = stateOf(*layout, *identities);
    Identity::LabelBook labels;
    REQUIRE(labels.set({ Identity::ObjectKind::Client, 19 }, "older") == Identity::LabelVerdict::Accepted);
    REQUIRE(labels.set({ Identity::ObjectKind::Client, 3 }, "newer") == Identity::LabelVerdict::Accepted);
    REQUIRE(labels.set({ Identity::ObjectKind::Client, 19 }, "replacement") == Identity::LabelVerdict::Accepted);
    state.labels.assign(labels.records().begin(), labels.records().end());
    Identity::TagBook tags;
    const std::vector<std::string> names { "Second", "first" };
    REQUIRE(tags.set({ Identity::ObjectKind::Workspace, 7 }, names) == Identity::TagVerdict::Accepted);
    state.tags.assign(tags.records().begin(), tags.records().end());
    const auto encoded = encodeHandoverWorkspaces(state, limits);
    REQUIRE(encoded);
    const auto decoded = decodeHandoverWorkspaces(encoded->view(), limits);
    REQUIRE(decoded);
    CHECK(decoded->labels == state.labels);
    CHECK(decoded->tags == state.tags);
    auto malformed = decoded->workspaces[0];
    malformed.activeWindow = 999;
    const auto before = identities->continuation();
    const auto window = layout->activeSurface();
    CHECK(!restoreHandoverLayout(malformed, *layout));
    CHECK(layout->activeSurface() == window);
    CHECK(identities->continuation() == before);
}

UBIQUITTY_TEST_MAIN()
