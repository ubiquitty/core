// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The Core-owned LIVE presentation topology (Docs/session-model.md):
//   Workspace   the authority; owns surfaces + the monotonic id allocators
//     Surface   one top-level container (a window): ordered tabs + the active
//       Tab     identity + a Panes::Tree split layout
//         Pane  a leaf, carrying an opaque PaneID (never a Ubiquitty::Terminal*)
// Pure data + intent ops. Fixed topology pools with owned tab titles. The
// frontend is a PASSIVE PROJECTION (renders + sends intents, owns no layout state).
// Single-threaded by contract. Fixed bounds (§6); ids monotonic, never reused (§4).

#include "Base/Types.h"
#include "Core/Panes/PaneTree.h"
#include "Core/Session/Identity.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Ubiquitty::Session::Layout {

// Stable, never-reused handles. 0 means "none".
// NOTE: Session::Layout::SurfaceID (a u32 layout-tree handle) is a different ID
// space from Session::SurfaceID (the u64 surface-metadata handle in
// Session/Surface.h); they share a name, never a value — do not convert
// one into the other.
// Scoped enums rather than aliases, so the rule above is the compiler's rather
// than the reader's. As plain `uint32_t` all three would be one type: a
// function taking a PaneID would accept a TabID, and a call passing the wrong
// handle would compile and do the other handle's job.
//
// No runtime cost — the underlying type is `uint32_t` and these are handles, not
// arithmetic. Code that genuinely wants the number says std::to_underlying,
// which is a visible act rather than an implicit conversion.

/// A surface's stable handle in the layout tree. 0 means none.
///
/// Distinct from ``Session::SurfaceID`` in Session/Surface.h, which is a u64 in
/// a different id space — the two share a name and never a value.
enum class SurfaceID : uint32_t {
};

/// A tab's stable handle within one surface. 0 means none.
enum class TabID : uint32_t {
};

/// A pane's stable handle within one tab. 0 means none.
///
/// Distinct from ``Protocol::Tmux::PaneID``, which is a signed id issued by a
/// tmux server rather than by this tree.
enum class PaneID : uint32_t {
};

constexpr SurfaceID invalidSurface {};
constexpr TabID invalidTab {};
constexpr PaneID invalidPane {};

// The rule, as a check. A conversion added between any two of these stops
// compiling here rather than at whichever call site first relies on it.
static_assert(!std::is_convertible_v<TabID, PaneID>);
static_assert(!std::is_convertible_v<PaneID, SurfaceID>);
static_assert(!std::is_convertible_v<SurfaceID, TabID>);
static_assert(!std::is_convertible_v<PaneID, uint32_t>);

// Fixed bounds (session-model.md §6), chosen "effectively unlimited" for
// real use. Panes/tab matches Panes::Tree's own pool.
constexpr int maxSurfaces = 16;
constexpr int maxTabsPerSurface = 32;
constexpr int maxPanesPerTab = Ubiquitty::Panes::maxPanes; // 64

// One tab: identity + its split layout. The leaves of `panes` carry the
// tab's PaneIDs (Panes::Tree stores them as int; allocated ids are positive,
// so the round-trip is lossless).
struct Tab {
    TabID id = invalidTab;
    Ubiquitty::Panes::Tree panes; // default single-pane; re-rooted on creation
    std::string title;
};

// One surface: a top-level container (the frontend's window) holding an
// ordered set of tabs and the active one (an index into tabs[0..tabCount)).
//
// `zoomedPane` is the pane the surface has expanded to fill the window
// (invalidPane = none). This is presentation state the FRONTEND realizes
// (macOS mounts only that pane's chrome; the split tree is untouched and
// un-zoom rebuilds it) — the model only records the intent so it is single-
// sourced and survives a re-mirror. A zoom belongs to the active tab; it is
// cleared automatically when the zoomed pane closes or the active tab changes.
struct Surface {
    SurfaceID id = invalidSurface;
    Tab tabs[maxTabsPerSurface];
    int tabCount = 0;
    int activeTab = 0;
    PaneID zoomedPane = invalidPane;
};

// The Workspace: owns the surfaces, the active surface, and the monotonic
// id allocators. One per app/daemon. A fresh Workspace is the degenerate
// 1 surface / 1 tab / 1 pane — today's single-Ubiquitty::Terminal window.
//
// Large by design (the fixed pools pre-allocate the caps); own it by
// reference / on the heap, never copy it around.
/// One tab of a topology authored elsewhere, adopted with its id.
struct AdoptedTab {
    TabID id = invalidTab;
    const Ubiquitty::Panes::Tree* panes = nullptr;
    std::string_view title {};
};

/// One surface of a topology authored elsewhere, adopted with its ids.
struct AdoptedSurface {
    SurfaceID id = invalidSurface;
    TabID activeTab = invalidTab;
    PaneID zoomedPane = invalidPane;
    std::span<const AdoptedTab> tabs;
};

/// The ids of a fresh workspace's one surface, tab, and pane, minted before the
/// workspace that holds them exists.
///
/// A host that must name a pane to the program it launches does so before the
/// pane's workspace is built; the workspace then adopts the ids it was told.
struct RootIdentity {
    SurfaceID surface = invalidSurface;
    TabID tab = invalidTab;
    PaneID pane = invalidPane;
};

/// Mints a root's three ids from an allocator.
///
/// - Parameter identities: The allocator the root's workspace will share.
/// - Returns: The ids, or nothing when any kind is exhausted.
[[nodiscard]] std::optional<RootIdentity> mintRootIdentity(Identity::Allocator& identities);

class Workspace {
public:
    /// Creates the degenerate one-surface workspace, numbering its own ids.
    Workspace();

    /// Creates the degenerate workspace with ids drawn from an allocator other
    /// workspaces share, so no two of them ever hold the same id.
    ///
    /// - Parameter identities: The shared allocator; must not be null.
    explicit Workspace(std::shared_ptr<Identity::Allocator> identities);

    /// Creates the degenerate workspace around root ids already minted from
    /// `identities`.
    ///
    /// - Parameters:
    ///   - identities: The shared allocator the root was minted from.
    ///   - root: The surface, tab, and pane ids the workspace starts with.
    Workspace(std::shared_ptr<Identity::Allocator> identities, RootIdentity root);

    // ---- surfaces ----
    [[nodiscard]] int surfaceCount() const { return m_surfaceCount; }
    // The id of the surface at creation-order index, or invalidSurface.
    [[nodiscard]] SurfaceID surfaceIDAt(int index) const;
    // Non-owning lookup; nullptr if unknown. Stable until this surface closes.
    [[nodiscard]] const Surface* surface(SurfaceID) const;
    [[nodiscard]] SurfaceID activeSurface() const { return m_activeSurface; }
    bool setActiveSurface(SurfaceID);
    // New surface (1 tab, 1 pane), made active; invalidSurface if at the cap.
    SurfaceID addSurface();
    // Close a surface; false if unknown or it's the last surface (a Workspace
    // always keeps at least one).
    bool closeSurface(SurfaceID);

    // ---- tabs (within a surface) ----
    [[nodiscard]] int tabCount(SurfaceID) const;
    [[nodiscard]] TabID activeTab(SurfaceID) const;
    /// The tab at an ordinal position, for enumerating a surface's tabs.
    ///
    /// - Parameters:
    ///   - surface: The surface whose tabs are enumerated.
    ///   - index: The position, 0 to `tabCount` exclusive.
    /// - Returns: The tab, or `invalidTab` when either id is out of range.
    [[nodiscard]] TabID tabAt(SurfaceID surface, int index) const;
    /// Returns the tab's custom title, empty for an unknown tab or an automatic title.
    ///
    /// - Returns: A borrowed title, valid until the workspace changes.
    [[nodiscard]] std::string_view tabTitle(SurfaceID, TabID) const;
    /// Sets a tab's custom title, kept as `normalizedCustomTitle` keeps it;
    /// returns false when the tab does not exist.
    ///
    /// - Parameter title: The custom title, empty for automatic naming.
    /// - Returns: Whether the tab exists and its title was assigned.
    bool renameTab(SurfaceID, TabID, std::string_view title);
    // New tab (1 pane) in the surface, made active; invalidTab if full / gone.
    TabID addTab(SurfaceID);
    // Close a tab (and its panes); false if unknown or the surface's last tab.
    bool closeTab(SurfaceID, TabID tab);
    bool setActiveTab(SurfaceID, TabID tab);

    // ---- panes (within a surface's active tab) ----
    // The focused pane of the surface's active tab, or invalidPane.
    [[nodiscard]] PaneID focusedPane(SurfaceID) const;
    /// Splits the focused pane of the surface's active tab.
    ///
    /// The new leaf takes focus and a freshly allocated id, so a caller can
    /// address it immediately without re-reading the tree.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is split.
    ///   - orientation: Which way the new divider runs.
    ///   - ratio: The new pane's share of the split, 0 to 1.
    /// - Returns: The new pane's id, or `invalidPane` when the tab is at the
    ///   pane cap or unknown.
    PaneID splitFocusedPane(SurfaceID, Ubiquitty::Panes::Orientation, float ratio = Ubiquitty::Panes::defaultRatio);
    // Close the focused pane (collapsing one split level). If it was the tab's
    // last pane, the tab is closed instead (false if that was the last tab).
    bool closeFocusedPane(SurfaceID);
    // Close a SPECIFIC pane of the surface's active tab, by id, collapsing one
    // split level. Unlike closeFocusedPane this never moves focus when `pane`
    // was not the focused pane (Panes::Tree::closePane leaves m_focused put for
    // a non-focused close), so closing a background pane keeps keyboard focus
    // where it was; closing the focused pane promotes the sibling. Clears a now-
    // stale zoom. Returns false if `pane` isn't a live leaf or it's the last
    // pane (a lone pane closes the tab via closeFocusedPane, not this).
    bool closePane(SurfaceID, PaneID pane);
    /// Finds the surface holding a pane, in any of its tabs.
    ///
    /// - Parameter pane: The pane to look for.
    /// - Returns: The surface, or `invalidSurface` when no tab holds it.
    [[nodiscard]] SurfaceID surfaceHolding(PaneID pane) const;
    /// Finds the tab of a surface holding a pane.
    ///
    /// - Parameters:
    ///   - surface: The surface whose tabs are searched.
    ///   - pane: The pane to look for.
    /// - Returns: The tab, or `invalidTab` when none of the surface's tabs holds it.
    [[nodiscard]] TabID tabHolding(SurfaceID surface, PaneID pane) const;

    /// Takes a pane out of whichever tab holds it.
    ///
    /// The split collapses; a tab's last pane closes the tab; a surface's last
    /// tab closes the surface.
    ///
    /// - Parameter pane: The pane to remove.
    /// - Returns: False for the workspace's last pane or one it does not hold.
    bool removePane(PaneID pane);
    bool focusPane(SurfaceID, PaneID);

    /// Moves focus to the neighbouring pane in a direction.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is walked.
    ///   - direction: Which neighbour to focus.
    /// - Returns: True when focus moved; false when there is no pane that way.
    bool moveFocus(SurfaceID, Ubiquitty::Panes::Direction);

    // ---- pane rearrangement (within the surface's active tab) ----
    /// Moves a pane beside another under a fresh split.
    ///
    /// Focus stays with whichever pane had it. A zoom survives, because a zoom
    /// names the pane rather than the position.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is rearranged.
    ///   - pane: The pane to move.
    ///   - target: The pane to move it beside.
    ///   - orientation: Which way the new divider runs.
    ///   - side: Which side of `target` the moved pane lands on.
    /// - Returns: False when either id is not a live leaf of the active tab,
    ///   the ids are equal, or the tab has a single pane.
    bool movePane(SurfaceID, PaneID pane, PaneID target, Ubiquitty::Panes::Orientation, Ubiquitty::Panes::InsertSide side);
    // Swap two panes' POSITIONS in the active tab (shape + ratios stay put;
    // focus follows the pane). False if either id is not a live leaf there
    // or the ids are equal.
    bool swapPanes(SurfaceID, PaneID, PaneID);
    /// Moves a pane into a tab anywhere in the workspace.
    ///
    /// A same-tab request delegates to `movePane`. A move that empties its
    /// source tab closes that tab and an empty source surface. The active tab does not change, and a zoom
    /// naming the moved pane clears once the pane leaves the active tab.
    ///
    /// - Parameters:
    ///   - surface: The source surface.
    ///   - pane: The pane to move, in any tab of the surface.
    ///   - targetTab: The tab to move it into.
    ///   - targetPane: The pane in `targetTab` to land beside.
    ///   - orientation: Which way the new divider runs.
    ///   - side: Which side of `targetPane` the moved pane lands on.
    ///   - insertTarget: Whether to split `targetPane` or wrap its tab's layout.
    /// - Returns: False when an id is unknown, the target tab is at the pane
    ///   cap, or the move is the degenerate pane-onto-itself.
    bool movePaneToTab(SurfaceID, PaneID pane, TabID targetTab, PaneID targetPane, Ubiquitty::Panes::Orientation, Ubiquitty::Panes::InsertSide side, Ubiquitty::Panes::InsertTarget = Ubiquitty::Panes::InsertTarget::Pane);

    /// Moves a complete tab's layout beside a pane, preserving its split ratios.
    ///
    /// - Parameters:
    ///   - surfaceID: The surface containing the source tab.
    ///   - tabID: The tab whose layout moves.
    ///   - targetPane: The destination pane, outside the source tab.
    ///   - orientation: The direction of the enclosing split.
    ///   - side: The side of the target occupied by the source layout.
    ///   - insertTarget: Whether to split the destination pane or wrap its tab's layout.
    /// - Returns: Whether the endpoints are valid and the combined layout fits.
    bool moveTabToPane(SurfaceID, TabID, PaneID targetPane, Ubiquitty::Panes::Orientation, Ubiquitty::Panes::InsertSide, Ubiquitty::Panes::InsertTarget = Ubiquitty::Panes::InsertTarget::Pane);
    /// Moves a tab to another surface, retaining its identity, title, and layout.
    ///
    /// - Parameters:
    ///   - surfaceID: The source surface.
    ///   - tabID: The tab to move and activate at the destination.
    ///   - destination: The receiving surface, distinct from the source.
    /// - Returns: Whether both endpoints exist and the destination has tab capacity.
    bool moveTabToSurface(SurfaceID, TabID, SurfaceID destination);
    /// Moves a tab to a place among a surface's tabs, retaining its identity,
    /// title and layout.
    ///
    /// Within its own surface the tab is reordered: the surface goes on
    /// showing the tab it showed, and the workspace's active surface stays.
    /// Into another surface the tab arrives shown, as `moveTabToSurface`
    /// leaves it, the destination becomes the active surface and its zoom is
    /// cleared; a source left with no tab is closed.
    ///
    /// - Parameters:
    ///   - tab: The tab to move, in any surface of the workspace.
    ///   - destination: The surface it is to stand in.
    ///   - index: The place it takes there, counted from zero once it has
    ///     left where it stood: at most the last place within its own
    ///     surface, and at most one past the last in another.
    /// - Returns: Whether it moved. False, changing nothing, for a tab or a
    ///   surface the workspace does not hold, a place the destination does not
    ///   have, or a destination holding every tab it may.
    bool moveTab(TabID tab, SurfaceID destination, int index);
    /// Detaches a tab into a new surface, refusing a surface's only tab.
    ///
    /// - Parameters:
    ///   - surfaceID: The source surface, containing at least two tabs.
    ///   - tabID: The tab to extract.
    /// - Returns: The created surface, or `invalidSurface` for an unknown source,
    ///   a sole tab, or exhausted surface capacity.
    SurfaceID extractTabToSurface(SurfaceID, TabID);
    /// Moves a pane into its own tab on the destination surface.
    ///
    /// - Parameters:
    ///   - surfaceID: The surface containing the pane.
    ///   - pane: The pane to move, retaining its tab when it is already alone.
    ///   - destination: The receiving surface, which may equal the source.
    /// - Returns: Whether both endpoints exist and the destination has tab capacity.
    bool movePaneToNewTab(SurfaceID, PaneID, SurfaceID destination);

    // ---- pane extraction ----
    // Extract `pane` (found in any tab of the surface) into a NEW tab of the
    // same surface, rooted at that pane and made active. The source tree
    // collapses one level. invalidTab when the pane is unknown, it is its
    // tab's only pane (already tab-shaped), or the surface is at the tab cap.
    TabID extractPaneToTab(SurfaceID, PaneID);
    // Extract `pane` into a NEW surface (1 tab rooted at the pane), made
    // active — the model seed for extract-to-window. When the move empties
    // its source tab, that tab closes. invalidSurface when the pane is
    // unknown, the workspace is at the surface cap, or the pane is the
    // surface's only pane in its only tab (already surface-shaped).
    SurfaceID extractPaneToSurface(SurfaceID, PaneID);

    // ---- pane sizing (the surface's active tab) ----
    // Equalize every split of the active tab (each leaf gets an equal
    // share). False for an unknown surface.
    bool balanceActiveTab(SurfaceID);
    /// Nudges the divider on one side of the focused pane, for keyboard resize.
    ///
    /// See `Panes::Tree::adjustRatio` for the gridFloor.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is resized.
    ///   - direction: Which of the focused pane's dividers to move.
    ///   - step: A positive fraction of that split's extent.
    /// - Returns: True only when a ratio actually changed, so a caller can skip
    ///   a repaint at the clamp.
    bool adjustFocusedPaneRatio(SurfaceID, Ubiquitty::Panes::Direction, float step);
    /// Writes one split's divider ratio directly, for a pointer drag.
    ///
    /// The keyboard path above nudges by a step; a drag knows the final
    /// fraction, measured in the view that moved.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is resized.
    ///   - splitIndex: The split node's index in the projected tree.
    ///   - ratio: The first child's fraction of the split's extent.
    /// - Returns: True only when the node is a live split, so a caller can
    ///   skip a repaint otherwise.
    bool setDividerRatio(SurfaceID, int splitIndex, float ratio);

    /// Balances an explicit tab without changing the active tab or focus.
    /// - Parameters:
    ///   - surface: The containing surface.
    ///   - tab: The tab to balance, including an inactive tab.
    /// - Returns: Whether the named tab exists and was balanced.
    bool balanceTab(SurfaceID surface, TabID tab);
    /// Nudges an explicit pane's divider without changing focus.
    /// - Parameters:
    ///   - surface: The containing surface.
    ///   - tab: The containing tab.
    ///   - pane: The exact pane whose edge moves.
    ///   - direction: Which edge to adjust.
    ///   - step: The positive fraction to move.
    /// - Returns: Whether the pane exists in that tab and its ratio changed.
    bool adjustPaneRatio(SurfaceID surface, TabID tab, PaneID pane, Ubiquitty::Panes::Direction direction, float step);
    /// Writes a split in an explicit tab without selecting that tab.
    /// - Parameters:
    ///   - surface: The containing surface.
    ///   - tab: The exact tab to resize.
    ///   - splitIndex: A split index resolved in that tab's current tree.
    ///   - ratio: The requested first-child fraction.
    /// - Returns: Whether the named tab and split exist.
    bool setTabDividerRatio(SurfaceID surface, TabID tab, int splitIndex, float ratio);

    // ---- pane zoom (a surface fills the window with one pane) ----
    // The surface's zoomed pane, or invalidPane when not zoomed.
    [[nodiscard]] PaneID zoomedPane(SurfaceID) const;
    // Toggle `pane`'s zoom on the surface: if it is already the zoomed pane,
    // clear the zoom; otherwise zoom it. No-op (returns false, zoom unchanged)
    // when the surface is unknown, `pane` is not a live pane of the active tab,
    // or the active tab has a single pane (nothing to expand). Returns true
    // when the zoom state changed. Model-only — the frontend realizes it.
    bool toggleZoom(SurfaceID, PaneID pane);

    /// Returns the surface's active pane tree, for the frontend's projection.
    ///
    /// - Parameter surface: The surface to read.
    /// - Returns: The active tab's tree, or null for an unknown surface. The
    ///   pointer is owned here and invalidated by the next mutating call.
    [[nodiscard]] const Ubiquitty::Panes::Tree* activePaneTree(SurfaceID) const;

    /// Returns one tab's pane tree, active or not.
    ///
    /// The projection reads the active tree above; this is for a caller that
    /// accounts for every pane of a surface — a background tab's panes are
    /// just as alive as the visible ones.
    ///
    /// - Parameters:
    ///   - surface: The surface owning the tab.
    ///   - tab: The tab to read.
    /// - Returns: The tab's tree, or null for an unknown id. The pointer is
    ///   owned here and invalidated by the next mutating call.
    [[nodiscard]] const Ubiquitty::Panes::Tree* paneTree(SurfaceID surface, TabID tab) const;

    /// Replaces the surface's active-tab tree wholesale.
    ///
    /// The tmux and session-restore path, where the topology is authored
    /// elsewhere and adopted rather than grown through split and close. Two
    /// things are reconciled on the way in, and both exist because the adopted
    /// tree carries ids this instance never minted: a zoom naming a pane the
    /// new tree does not carry is dropped, and the id allocator is advanced past
    /// every live pane id across all tabs of the surface — otherwise a later
    /// `splitFocusedPane` could re-mint an id the adopted tree already injected,
    /// such as a tmux `%M`.
    ///
    /// - Parameters:
    ///   - surface: The surface whose active tab is replaced.
    ///   - tree: The topology to adopt.
    /// - Returns: False for an unknown surface, true on adopt.
    bool adoptTree(SurfaceID, const Ubiquitty::Panes::Tree& tree);

    /// Replaces the whole topology with one authored elsewhere, its ids included.
    ///
    /// The mirror of another Workspace, where a verb sent back must name that
    /// one's ids. Every allocator advances past every adopted id. A zoom naming
    /// a pane its surface's active tab does not hold is dropped.
    ///
    /// - Parameters:
    ///   - surfaces: The surfaces to adopt, in the order they are held.
    ///   - activeSurface: The id of the surface to make active; one of `surfaces`.
    /// - Returns: False, the topology unchanged, when a cap is exceeded, a
    ///   surface holds no tab or a tab no tree, an id is invalid or repeated,
    ///   or an active choice names nothing adopted.
    bool adoptWorkspace(std::span<const AdoptedSurface> surfaces, SurfaceID activeSurface);

private:
    Surface* findSurface(SurfaceID);
    [[nodiscard]] const Surface* findSurface(SurfaceID) const;
    Tab* activeTabOf(SurfaceID);
    /// Finds one exact tab without changing the surface's active selection.
    /// - Parameters:
    ///   - surface: Its containing surface.
    ///   - tab: The tab identity.
    /// - Returns: The borrowed tab, or null when it is absent.
    Tab* tabOf(SurfaceID surface, TabID tab);
    // Index into surface.tabs of the tab whose tree carries `pane`, or -1.
    static int tabIndexHolding(const Surface&, PaneID pane);
    // Initialize `surface` to a fresh 1-tab/1-pane surface with new ids; false,
    // the surface untouched, when an id kind is exhausted.
    bool initSurface(Surface&);
    // Clear the surface's zoom if its zoomed pane is no longer a live leaf of
    // the active tab (the pane closed, or the active tab changed). Called after
    // every structural mutation so the zoom can't dangle.
    static void clearZoomIfStale(Surface&);

    Surface m_surfaces[maxSurfaces];
    int m_surfaceCount = 0;
    SurfaceID m_activeSurface = invalidSurface;
    // The allocator does arithmetic — increment, compare against the highest
    // id a restored layout carried — on plain numbers, and a handle type that
    // permitted that would permit it everywhere else too. The id is
    // constructed where it is issued.

    /// Returns the allocator this workspace mints from: the shared one when
    /// it was given one, else its own.
    ///
    /// - Returns: The allocator, valid for the workspace's lifetime.
    [[nodiscard]] Identity::Allocator& identities() { return m_sharedIdentities ? *m_sharedIdentities : m_ownIdentities; }

    /// Mints the next number of a kind.
    ///
    /// - Parameter kind: The kind to mint.
    /// - Returns: The number, or 0 once the kind is exhausted.
    [[nodiscard]] std::uint32_t mintNumber(Identity::ObjectKind kind);

    /// Advances a kind past a number adopted from elsewhere.
    ///
    /// - Parameters:
    ///   - kind: The kind the number belongs to.
    ///   - number: The adopted number; 0 changes nothing.
    void reserveNumber(Identity::ObjectKind kind, std::uint32_t number);

    Identity::Allocator m_ownIdentities;
    std::shared_ptr<Identity::Allocator> m_sharedIdentities;
};

} // namespace Ubiquitty::Session::Layout
