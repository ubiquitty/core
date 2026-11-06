// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// WorkspaceSession.h — the window model behind thin frontends
// (Docs/design/shell-workspace-session.md). One per app: it owns the
// Layout::Workspace that models every window's splits, tabs and zoom, and
// one TerminalSession per local pane, and it resolves every pane verb end
// to end — the backend consult, the topology mutation, the session mint.
// A frontend holds a SurfaceID per window, projects the pane tree into its
// native views, and makes a view per pane this model announces; nothing
// else is the frontend's.

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Session/LaunchContext.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/ProcessExitPolicy.h"
#include "Core/Session/RestorePlan.h"
#include "Core/Session/SharedWorkspace.h"
#include "Core/Session/WorkspaceCapture.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {
class WorkspaceSession;
}

/// Claims a reference to `model` on Swift's behalf.
///
/// Declared at global scope because the Swift importer resolves the retain and
/// release named by `UBIQUITTY_SHARED_REFERENCE` there and nowhere else; a
/// namespace-scoped pair is reported as missing rather than as misplaced.
///
/// - Parameter model: The model to retain. A null pointer is ignored.
void retainWorkspaceSession(Ubiquitty::Runtime::WorkspaceSession*);

/// Drops a reference to `model`, destroying it at zero.
///
/// - Parameter model: The model to release. A null pointer is ignored.
void releaseWorkspaceSession(Ubiquitty::Runtime::WorkspaceSession*);

namespace Ubiquitty::Runtime {

/// The window model: topology, sessions and verbs in one Core owner.
///
/// Pane ids are the Workspace allocator's alone — a hosted pane's foreign
/// id (a tmux `%M`) never enters this model, per the session model's
/// doctrine. Single-threaded: every call arrives on the frontend's main
/// thread, the same thread its views live on.
class WorkspaceSession {
public:
    /// What performing a verb produced, beyond the backend's answer.
    struct VerbResult {
        /// How the verb resolved.
        TerminalSession::PaneVerbOutcome outcome = TerminalSession::PaneVerbOutcome::Unsupported;
        /// The pane a local split created, else `Layout::invalidPane`; its
        /// session is retained here, read through ``session``.
        Session::Layout::PaneID createdPane = Session::Layout::invalidPane;
        /// The pane a local close removed, else `Layout::invalidPane` —
        /// the view to drop.
        Session::Layout::PaneID removedPane = Session::Layout::invalidPane;
        /// The surface a last-pane close ended, else `Layout::invalidSurface`
        /// — the window to drop; its sessions are already closed here.
        Session::Layout::SurfaceID removedSurface = Session::Layout::invalidSurface;
    };

    /// How a local pane's PTY is drained after the model launches it.
    enum class IngestPolicy : std::uint8_t {
        /// The session's own ingest thread drains it.
        ThreadDrained UBIQUITTY_SWIFT_NAME(threadDrained),
        /// The frontend drains on its own loop — the GTK arm's fd watch;
        /// the model starts no thread.
        CallerDrained UBIQUITTY_SWIFT_NAME(callerDrained),
    };

    /// Installs the frontend's per-terminal clients on a session the model
    /// just made, called before that session's ingest starts — the one moment
    /// its Terminal is reachable with no other thread driving it.
    ///
    /// A function pointer with a context, not an interface: the model calls
    /// through a seam the frontend owns, and the platform types the installer
    /// touches must never appear here.
    using TerminalClientInstaller = void (*)(void* context, TerminalSession& session);

    /// Sets the installer every session made after this call goes through.
    ///
    /// Set it before the first surface opens: sessions made earlier ran
    /// without clients and are not revisited, because their ingest is live
    /// and a client swap under a running drain would race it.
    ///
    /// - Parameters:
    ///   - installer: The seam, or null to stop installing.
    ///   - context: Handed back verbatim on every call.
    void setTerminalClientInstaller(TerminalClientInstaller installer, void* context)
    {
        m_clientInstaller = installer;
        m_clientInstallerContext = context;
    }

    /// Seeds the installer every model made after this call starts with.
    ///
    /// The process-wide default, for the shell whose model is made inside a
    /// module its platform code cannot reach into: the platform registers at
    /// startup, before any model exists, and construction copies the seed —
    /// a later change reaches later models only, the ordering the per-model
    /// setter states.
    ///
    /// - Parameters:
    ///   - installer: The seam, or null to seed none.
    ///   - context: Handed back verbatim on every call.
    static void setDefaultTerminalClientInstaller(TerminalClientInstaller installer, void* context)
    {
        s_defaultClientInstaller = installer;
        s_defaultClientInstallerContext = context;
    }

    /// Installs the process default's clients on a session no model made,
    /// under the lock its terminal is read under.
    ///
    /// A pane the frontend adopted and a surface a shell builds by itself
    /// pass through no model's installer, so the shell asks for its clients
    /// here. Call once per terminal, before exposing the session to the
    /// frontend.
    ///
    /// - Parameter session: The session to equip.
    static void installDefaultClients(TerminalSession& session)
    {
        const std::scoped_lock hold(*session.modelLockPointer());
        if (s_defaultClientInstaller) {
            s_defaultClientInstaller(s_defaultClientInstallerContext, session);
        }
    }

    /// Installs frontend clients on an adopted model under its owner's lock.
    ///
    /// Call once after adoption, before exposing the session to the frontend.
    /// - Parameter session: The newly adopted session to equip.
    static void installDefaultClientsForAdoptedSession(TerminalSession& session) { installDefaultClients(session); }

    WorkspaceSession(const WorkspaceSession&) = delete;
    WorkspaceSession& operator=(const WorkspaceSession&) = delete;

    /// Allocates the model, owned by whoever holds the returned pointer.
    ///
    /// The only way to make one, for the reason ``TerminalSession::make``
    /// states: a stack instance would be released as heap memory the first
    /// time Swift saw a pointer to it. The two arguments are the frontend's
    /// standing capabilities, fixed for the model's life; everything else a
    /// launch needs resolves from config inside the model.
    ///
    /// The model borrows `configuration` for its whole life and hands the same
    /// instance to every session it makes, so the owner keeps that instance
    /// alive past the model. A host giving each model its own configuration
    /// passes the model's own instance; nothing here consults
    /// ``Preferences::values``.
    ///
    /// - Parameters:
    ///   - ingestPolicy: Who drains a launched pane's PTY.
    ///   - drawnText: Which OSC 66 text the frontend's renderer draws.
    ///   - configuration: The preferences every launch resolves from.
    /// - Returns: A model with one reference, or null if allocation failed.
    [[nodiscard]] static WorkspaceSession* make(IngestPolicy ingestPolicy, DrawnText drawnText, const Preferences::Values& configuration) UBIQUITTY_RETURNS_RETAINED
    {
        return new (std::nothrow) WorkspaceSession(ingestPolicy, drawnText, configuration);
    }

    /// Allocates a model reading the process-wide preferences.
    ///
    /// The frontends' form: one process, one configuration, and a configure
    /// (``Preferences::configure``) reaches every launch because the
    /// model borrows the one live instance.
    ///
    /// - Parameters:
    ///   - ingestPolicy: Who drains a launched pane's PTY.
    ///   - drawnText: Which OSC 66 text the frontend's renderer draws.
    /// - Returns: A model with one reference, or null if allocation failed.
    [[nodiscard]] static WorkspaceSession* make(IngestPolicy ingestPolicy, DrawnText drawnText) UBIQUITTY_RETURNS_RETAINED
    {
        return make(ingestPolicy, drawnText, Preferences::values());
    }

    /// The reference count Swift manipulates through the global retain and
    /// release. Public because those are free functions.
    std::atomic<int> swiftReferences { 1 };

    /// Ends and releases every retained session.
    ~WorkspaceSession()
    {
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr) {
                binding.session->close();
                releaseTerminalSession(binding.session);
                binding.session = nullptr;
            }
        }
    }

    /// Opens one window's surface: a root pane with a launched login
    /// session, its grid and seeding resolved from config in here.
    ///
    /// A window opening registers a login session; a pane joining one does
    /// not. Splits and restored panes take ``LaunchCommand::loginShell``.
    ///
    /// - Returns: The surface, or `Layout::invalidSurface` when the pool
    ///   is full or the session cannot start.
    Session::Layout::SurfaceID addWindowSurface() { return addWindowSurface(LaunchCommand::loginSession()); }

    /// Opens one window's surface running a drafted command — the shape a
    /// frontend takes when the argument vector came from its own platform
    /// (an open-with event, a picker) rather than from config.
    ///
    /// - Parameter draft: The launch the caller spelled argument by argument.
    /// - Returns: The surface, or `Layout::invalidSurface` when the pool
    ///   is full or the session cannot start.
    Session::Layout::SurfaceID addWindowSurface(const LaunchCommandDraft& draft) { return addWindowSurface(draft.build()); }

    /// Opens one window's surface running a requested command — the shape a
    /// launch request's first window takes.
    ///
    /// - Parameter command: What the root pane runs.
    /// - Returns: The surface, or `Layout::invalidSurface` when the pool
    ///   is full or the session cannot start.
    Session::Layout::SurfaceID addWindowSurface(const LaunchCommand& command)
    {
        const Session::Layout::SurfaceID surface = m_workspace.addSurface();
        if (surface == Session::Layout::invalidSurface)
            return surface;
        const Session::Layout::PaneID rootPane = m_workspace.focusedPane(surface);
        TerminalSession* session = makeLocalSession(command, rootPane);
        if (session == nullptr) {
            m_workspace.closeSurface(surface);
            return Session::Layout::invalidSurface;
        }
        bind(surface, rootPane, session);
        return surface;
    }

    /// Opens one window's surface on a serial line instead of a child.
    ///
    /// The pane's session starts as any other and then adopts the line's
    /// stream, so everything above — the drain, the model, the verbs — is the
    /// shape a shell pane has; only the bytes come from elsewhere. A device
    /// that will not open leaves no surface behind.
    ///
    /// - Parameters:
    ///   - devicePath: The line to open, e.g. `/dev/cu.usbserial`.
    ///   - baudRate: The speed, 0 for the line's current one.
    /// - Returns: The surface, or `Layout::invalidSurface` when the pool is
    ///   full or the line cannot be opened.
    Session::Layout::SurfaceID addWindowSurfaceOnSerialLine(const std::string& devicePath, int baudRate)
    {
        const Session::Layout::SurfaceID surface = m_workspace.addSurface();
        if (surface == Session::Layout::invalidSurface)
            return surface;
        const Session::Layout::PaneID rootPane = m_workspace.focusedPane(surface);
        TerminalSession* session = makeSession();
        if (session != nullptr) {
            std::unique_ptr<PAL::Stream> line = PAL::openSerial(devicePath, baudRate);
            bool isAdopted = false;
            if (line) {
                auto adoption = session->adoptStream(std::move(line));
                if (adoption)
                    isAdopted = true;
                else
                    line = std::move(adoption.error());
            }
            if (!isAdopted) {
                session->close();
                releaseTerminalSession(session);
                session = nullptr;
            }
        }
        if (session == nullptr) {
            m_workspace.closeSurface(surface);
            return Session::Layout::invalidSurface;
        }
        if (m_ingestPolicy == IngestPolicy::ThreadDrained && !session->startIngest()) {
            session->close();
            releaseTerminalSession(session);
            m_workspace.closeSurface(surface);
            return Session::Layout::invalidSurface;
        }
        bind(surface, rootPane, session);
        return surface;
    }

    /// Opens one window's surface rebuilt from a captured split layout.
    ///
    /// Session restore, and the round trip is Core's on both halves: the
    /// layout was emitted by `Tmux::emitPaneLayout` from a tree like this
    /// one, and `parseLayoutTree` reads it back. Every leaf gets a freshly
    /// launched shell — the bytes a window held are gone with its processes,
    /// and what persists is the arrangement.
    ///
    /// A layout that will not parse, or one naming a single pane, opens the
    /// plain window instead: a restore that cannot rebuild the split is
    /// still a restore of the window.
    ///
    /// - Parameter layout: The captured layout string.
    /// - Returns: The surface, or `Layout::invalidSurface` when the pool is
    ///   full or the first session cannot start.
    Session::Layout::SurfaceID addWindowSurfaceRestoring(const std::string& layout)
    {
        Protocol::Tmux::LayoutNode parsed;
        if (!Protocol::Tmux::parseLayoutTree(layout, parsed))
            return addWindowSurface(); // unparseable: the plain window is still a restored window

        // Built before the surface takes it, and seeded with the LAYOUT's own
        // leftmost id because that is what `buildPaneTreeInto` nests against;
        // seeded with anything else it finds no leaf to split and answers the
        // lone pane, which reads as a window that restored but lost its
        // arrangement. The ids the layout carries are this allocator's from a
        // previous run, and `adoptTree` advances the allocator past every one
        // so a later split cannot re-mint a live id.
        Ubiquitty::Panes::Tree rebuilt(Protocol::Tmux::leftmostPaneID(parsed));
        Protocol::Tmux::buildPaneTreeInto(rebuilt, parsed);
        if (rebuilt.paneCount() <= 1)
            return addWindowSurface(); // a layout naming one pane is the plain window

        const Session::Layout::SurfaceID surface = m_workspace.addSurface();
        if (surface == Session::Layout::invalidSurface)
            return surface;
        // Adopted before any session is minted, so the surface's own
        // pre-minted pane — which the adopt replaces — never holds one.
        if (!m_workspace.adoptTree(surface, rebuilt)) {
            m_workspace.closeSurface(surface);
            return addWindowSurface();
        }
        const Ubiquitty::Panes::Tree* adopted = m_workspace.activePaneTree(surface);
        if (adopted == nullptr) {
            m_workspace.closeSurface(surface);
            return Session::Layout::invalidSurface;
        }
        int launched = 0;
        Ubiquitty::Panes::forEachLeaf(*adopted, [&](int leaf) {
            const Session::Layout::PaneID pane = static_cast<Session::Layout::PaneID>(leaf);
            if (session(pane) != nullptr)
                return;
            if (TerminalSession* restored = makeLocalSession(LaunchCommand::loginShell(), pane)) {
                bind(surface, pane, restored);
                ++launched;
            }
        });
        if (launched == 0) {
            m_workspace.closeSurface(surface);
            return Session::Layout::invalidSurface;
        }
        return surface;
    }

    /// Captures one local window's exact layout and per-pane launch directories.
    ///
    /// - Parameter surface: The surface to capture.
    /// - Returns: A text archive, or empty when the surface does not exist.
    [[nodiscard]] std::string captureWindowArrangement(Session::Layout::SurfaceID surface)
    {
        if (!m_workspace.surface(surface)) {
            return {};
        }
        const auto snapshot = Session::Layout::captureToSnapshot(m_workspace, [](void* context, Session::Layout::PaneID pane) -> Session::Layout::PaneInfo {
                auto& model = *static_cast<WorkspaceSession*>(context);
                Session::Layout::PaneInfo info;
                if (TerminalSession* session = model.session(pane)) {
                    if (session->isRemoteAttached()) {
                        info.origin.transport = Transport::OriginKind::Daemon;
                    }
                    const std::scoped_lock hold(*session->modelLockPointer());
                    info.directory = session->representedLocalDirectory();
                    if (const auto* terminal = session->terminal()) {
                        info.title = terminal->titleUTF8;
                    }
                }
                return info; }, this, surface);
        return Session::serializeWorkspaceText(snapshot);
    }

    /// Appends a captured local arrangement, launching a fresh shell per pane.
    ///
    /// A refused launch closes the new surface and all shells already started
    /// for it. Existing surfaces and their allocator identities are preserved.
    ///
    /// - Parameter archive: The text archive from `captureWindowArrangement`.
    /// - Returns: The new surface, or `invalidSurface` on any failure.
    Session::Layout::SurfaceID addWindowSurfaceFromArrangement(const std::string& archive)
    {
        Session::Workspace snapshot;
        if (!Session::deserializeWorkspaceText(archive, snapshot) || snapshot.windows.size() != 1) {
            return Session::Layout::invalidSurface;
        }
        struct RestoreContext {
            WorkspaceSession& model;
            bool hasFailed = false;
        } context { *this };
        const auto previous = m_workspace.activeSurface();
        const bool didRestore = Session::Layout::restoreFromSnapshot(snapshot, m_workspace, [](void* opaque, Session::Layout::PaneID pane, const Session::Layout::PaneInfo& info) {
                auto& context = *static_cast<RestoreContext*>(opaque);
                if (context.hasFailed) {
                    return;
                }
                if (info.origin.transport != Transport::OriginKind::Local) {
                    context.hasFailed = true;
                    return;
                }
                LaunchCommand command = LaunchCommand::loginShell();
                command.setInitialDirectory(info.directory);
                TerminalSession* session = context.model.makeLocalSession(command, pane);
                if (!session) {
                    context.hasFailed = true;
                    return;
                }
                session->applyTriggerTitle(info.title);
                context.model.bind(context.model.m_workspace.activeSurface(), pane, session); }, &context, Session::Layout::RestorePlacement::Append);
        if (!didRestore) {
            return Session::Layout::invalidSurface;
        }
        const auto surface = m_workspace.activeSurface();
        if (context.hasFailed) {
            closeWindowSurface(surface);
            m_workspace.setActiveSurface(previous);
            return Session::Layout::invalidSurface;
        }
        return surface;
    }

    /// The captured layout of one window's surface, for session restore.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - dimensionsForPane: Answers a pane's columns and rows; the emitter
    ///     re-derives every ratio from the extents.
    ///   - context: Handed back to `dimensionsForPane`.
    /// - Returns: The layout string, empty for an unsplit or dead surface —
    ///   a window with nothing to rebuild stores nothing.
    [[nodiscard]] std::string captureLayout(Session::Layout::SurfaceID surface,
        Protocol::Tmux::PaneDimensionsLookup dimensionsForPane, void* context) const
    {
        const Ubiquitty::Panes::Tree* tree = m_workspace.activePaneTree(surface);
        if (tree == nullptr || tree->paneCount() <= 1)
            return {};
        std::string layout;
        Protocol::Tmux::emitPaneLayout(*tree, tree->root(), dimensionsForPane, context, layout);
        // The leading checksum is what tmux writes; the parser tolerates any
        // value, so a constant round-trips our own capture.
        return "0000," + layout;
    }

    /// The captured layout of one window's surface, each pane at the grid its
    /// own session holds — what session restore stores for a split window.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The layout string, empty for an unsplit or dead surface.
    [[nodiscard]] std::string captureLayoutAtSessionGrids(Session::Layout::SurfaceID surface)
    {
        return captureLayout(
            surface,
            [](void* context, Protocol::Tmux::PaneID pane) -> Protocol::Tmux::PaneDimensions {
                auto* model = static_cast<WorkspaceSession*>(context);
                const TerminalSession* paneSession = model->session(static_cast<Session::Layout::PaneID>(static_cast<std::uint32_t>(pane)));
                if (paneSession == nullptr)
                    return { 0, 0 };
                const DimensionSnapshot grid = paneSession->dimensions();
                return { grid.columns, grid.rows };
            },
            this);
    }

    /// Closes one window's surface, ending every pane's session.
    /// An unresolved local adoption leaves the entire surface unchanged.
    ///
    /// - Parameter surface: The window's surface.
    void closeWindowSurface(Session::Layout::SurfaceID surface)
    {
        if (!canCloseSurfaceSessions(surface))
            return;
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.surface == surface) {
                binding.session->close();
                releaseTerminalSession(binding.session);
                binding = {};
            }
        }
        m_workspace.closeSurface(surface);
    }

    /// Splits the focused pane, resolving against its backend first: a
    /// hosted pane's authority performs it and echoes the layout back; a
    /// local pane splits here, minting the pane and launching its session.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - orientation: Which way the split divides.
    /// - Returns: What happened, including the view work.
    VerbResult splitFocusedPane(Session::Layout::SurfaceID surface, Ubiquitty::Panes::Orientation orientation)
    {
        VerbResult result;
        TerminalSession* focused = session(m_workspace.focusedPane(surface));
        if (focused != nullptr) {
            const TerminalSession::PaneVerb verb = orientation == Ubiquitty::Panes::Orientation::Vertical
                ? TerminalSession::PaneVerb::SplitBeside
                : TerminalSession::PaneVerb::SplitBelow;
            result.outcome = focused->requestPaneVerb(verb);
            if (result.outcome != TerminalSession::PaneVerbOutcome::LocalActionRequired)
                return result;
        }
        const Session::Layout::PaneID pane = m_workspace.splitFocusedPane(surface, orientation);
        if (pane == Session::Layout::invalidPane) {
            result.outcome = TerminalSession::PaneVerbOutcome::Unsupported;
            return result;
        }
        TerminalSession* created = makeLocalSession(LaunchCommand::loginShell(), pane);
        if (created == nullptr) {
            m_workspace.closePane(surface, pane);
            result.outcome = TerminalSession::PaneVerbOutcome::Unsupported;
            return result;
        }
        bind(surface, pane, created);
        result.outcome = TerminalSession::PaneVerbOutcome::LocalActionRequired;
        result.createdPane = pane;
        return result;
    }

    /// Closes a pane, resolving against its backend first; a local close
    /// collapses the tree and ends the session here.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - pane: The pane to close.
    /// - Returns: What happened; `removedPane` names the view to drop.
    VerbResult closePane(Session::Layout::SurfaceID surface, Session::Layout::PaneID pane)
    {
        VerbResult result;
        const Ubiquitty::Panes::Tree* tree = m_workspace.activePaneTree(surface);
        if (tree == nullptr || !tree->holdsPane(static_cast<int>(pane)))
            return result;
        TerminalSession* bound = session(pane);
        if (bound != nullptr) {
            if (!bound->canClose())
                return result;
            result.outcome = bound->requestPaneVerb(TerminalSession::PaneVerb::Close);
            if (result.outcome != TerminalSession::PaneVerbOutcome::LocalActionRequired)
                return result;
        }
        if (!m_workspace.closePane(surface, pane)) {
            // Membership was checked before consulting the backend. Only a
            // last-pane refusal may escalate to the tab or surface close.
            if (tree->paneCount() > 1) {
                result.outcome = TerminalSession::PaneVerbOutcome::Unsupported;
                return result;
            }
            // A tab's last pane closes the tab while siblings remain; the
            // surface's last pane closes the window itself.
            if (m_workspace.tabCount(surface) > 1) {
                result = closeTab(surface, m_workspace.activeTab(surface));
                if (result.outcome == TerminalSession::PaneVerbOutcome::LocalActionRequired)
                    result.removedPane = pane;
                return result;
            }
            if (!canCloseSurfaceSessions(surface))
                return {};
            closeWindowSurface(surface);
            result.outcome = TerminalSession::PaneVerbOutcome::LocalActionRequired;
            result.removedPane = pane;
            result.removedSurface = surface;
            return result;
        }
        unbind(pane);
        result.outcome = TerminalSession::PaneVerbOutcome::LocalActionRequired;
        result.removedPane = pane;
        return result;
    }

    /// Toggles zoom on the focused pane, the backend consulted first; the
    /// local intent is the Workspace's own.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: How the verb resolved.
    TerminalSession::PaneVerbOutcome toggleZoomFocused(Session::Layout::SurfaceID surface)
    {
        const Session::Layout::PaneID pane = m_workspace.focusedPane(surface);
        TerminalSession* bound = session(pane);
        if (bound != nullptr) {
            const TerminalSession::PaneVerbOutcome outcome = bound->requestPaneVerb(TerminalSession::PaneVerb::Zoom);
            if (outcome != TerminalSession::PaneVerbOutcome::LocalActionRequired)
                return outcome;
        }
        return m_workspace.toggleZoom(surface, pane)
            ? TerminalSession::PaneVerbOutcome::LocalActionRequired
            : TerminalSession::PaneVerbOutcome::Unsupported;
    }

    /// Writes one split's divider ratio, for a pointer drag settling.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - splitIndex: The split node's index in the projected tree.
    ///   - ratio: The first child's fraction of the split's extent.
    /// - Returns: Whether a live split took the ratio.
    bool setDividerRatio(Session::Layout::SurfaceID surface, int splitIndex, float ratio) { return m_workspace.setDividerRatio(surface, splitIndex, ratio); }

    /// Moves existing sessions into a split without closing or relaunching them.
    ///
    /// - Parameters:
    ///   - source: The surface containing the source pane.
    ///   - pane: The pane to move and focus at the destination.
    ///   - target: The pane beside which to insert the transferred layout.
    ///   - orientation: The direction of the destination split.
    ///   - side: The side of the target occupied by the transferred layout.
    ///   - scope: The portion of the source layout to transfer.
    ///   - insertTarget: Whether the destination split surrounds a pane or its tab.
    /// - Returns: Whether the layout moved and its session bindings were updated.
    bool moveToPane(Session::Layout::SurfaceID source, Session::Layout::PaneID pane, Session::Layout::PaneID target,
        Ubiquitty::Panes::Orientation orientation, Ubiquitty::Panes::InsertSide side, TerminalSession::TransferScope scope,
        Ubiquitty::Panes::InsertTarget insertTarget = Ubiquitty::Panes::InsertTarget::Pane)
    {
        const auto destination = m_workspace.surfaceHolding(target);
        const bool didMove = scope == TerminalSession::TransferScope::Tab
            ? m_workspace.moveTabToPane(source, m_workspace.tabHolding(source, pane), target, orientation, side, insertTarget)
            : m_workspace.movePaneToTab(source, pane, m_workspace.tabHolding(destination, target), target, orientation, side, insertTarget);
        if (!didMove) {
            return false;
        }
        for (auto& binding : m_bindings) {
            if (binding.session) {
                binding.surface = m_workspace.surfaceHolding(binding.pane);
            }
        }
        m_workspace.focusPane(destination, pane);
        return true;
    }

    /// Detaches a pane into a new surface while retaining its session.
    ///
    /// - Parameters:
    ///   - source: The surface containing the pane.
    ///   - pane: The pane to extract.
    /// - Returns: The created surface, or `invalidSurface` if the pane is unknown,
    ///   the surface limit is reached, or the pane already occupies a surface alone.
    Session::Layout::SurfaceID extractPaneToSurface(Session::Layout::SurfaceID source, Session::Layout::PaneID pane)
    {
        const auto created = m_workspace.extractPaneToSurface(source, pane);
        if (created != Session::Layout::invalidSurface) {
            for (auto& binding : m_bindings) {
                if (binding.pane == pane && binding.session) {
                    binding.surface = created;
                }
            }
        }
        return created;
    }

    // ---- tabs (the GTK arm's tab strip; macOS uses native window tabs) ----

    /// Opens a new tab on a surface: a root pane with a launched login
    /// shell, made active by the Workspace's own rule.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: What happened; `createdPane` names the new tab's root.
    VerbResult addTab(Session::Layout::SurfaceID surface)
    {
        VerbResult result;
        const Session::Layout::TabID tab = m_workspace.addTab(surface);
        if (tab == Session::Layout::invalidTab)
            return result;
        const Session::Layout::PaneID rootPane = m_workspace.focusedPane(surface);
        TerminalSession* session = makeLocalSession(LaunchCommand::loginShell(), rootPane);
        if (session == nullptr) {
            (void)m_workspace.closeTab(surface, tab);
            return result;
        }
        bind(surface, rootPane, session);
        result.outcome = TerminalSession::PaneVerbOutcome::LocalActionRequired;
        result.createdPane = rootPane;
        return result;
    }

    /// Closes one tab, ending every one of its panes' sessions; the last
    /// tab's close ends the surface, reported as `removedSurface`.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - tab: The tab to close.
    /// - Returns: What happened; the frontend drops the views it kept.
    VerbResult closeTab(Session::Layout::SurfaceID surface, Session::Layout::TabID tab)
    {
        VerbResult result;
        const Ubiquitty::Panes::Tree* tree = m_workspace.paneTree(surface, tab);
        if (tree == nullptr)
            return result;
        if (!canCloseTreeSessions(*tree)
            || (m_workspace.tabCount(surface) == 1 && !canCloseSurfaceSessions(surface)))
            return result;
        unbindTreeLeaves(*tree);
        if (!m_workspace.closeTab(surface, tab)) {
            // The last tab: its close is the surface's close.
            closeWindowSurface(surface);
            result.removedSurface = surface;
        }
        result.outcome = TerminalSession::PaneVerbOutcome::LocalActionRequired;
        return result;
    }

    /// Switches a surface's active tab.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - tab: The tab to activate.
    /// - Returns: Whether the tab changed.
    bool setActiveTab(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) { return m_workspace.setActiveTab(surface, tab); }

    /// The number of tabs a surface holds.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The count, 0 for a dead surface.
    [[nodiscard]] int tabCount(Session::Layout::SurfaceID surface) const { return m_workspace.tabCount(surface); }

    /// A surface's active tab.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The tab, or `Layout::invalidTab`.
    [[nodiscard]] Session::Layout::TabID activeTab(Session::Layout::SurfaceID surface) const { return m_workspace.activeTab(surface); }

    /// The tab at an ordinal position, for a tab strip's enumeration.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - index: The position, 0 to ``tabCount`` exclusive.
    /// - Returns: The tab, or `Layout::invalidTab` out of range.
    [[nodiscard]] Session::Layout::TabID tabAt(Session::Layout::SurfaceID surface, int index) const { return m_workspace.tabAt(surface, index); }

    /// The Workspace's active surface — the window that last took a verb.
    ///
    /// - Returns: The surface, or `Layout::invalidSurface`.
    [[nodiscard]] Session::Layout::SurfaceID activeSurface() const { return m_workspace.activeSurface(); }

    /// The authoritative number of surfaces, including a fresh unbound root.
    [[nodiscard]] int surfaceCount() const { return m_workspace.surfaceCount(); }

    /// A surface in the model's creation order, or invalidSurface past the end.
    [[nodiscard]] Session::Layout::SurfaceID surfaceAt(int index) const { return m_workspace.surfaceIDAt(index); }

    /// Borrows a tab's custom title until the next topology mutation.
    [[nodiscard]] std::string_view tabTitle(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) const { return m_workspace.tabTitle(surface, tab); }

    /// A tab's custom title as an owned string, for a reader that keeps it
    /// past the next mutation or crosses into Swift.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - tab: The tab.
    /// - Returns: The title, empty for an automatic title or an unknown tab.
    [[nodiscard]] std::string customTabTitle(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) const { return std::string(m_workspace.tabTitle(surface, tab)); }

    /// Sets a tab's custom title, the one store a local window's tab title
    /// lives in: the arrangement capture and the workspace presentation read
    /// it here.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - tab: The tab.
    ///   - title: The title as typed; empty restores the automatic title.
    /// - Returns: Whether the tab exists.
    bool renameTab(Session::Layout::SurfaceID surface, Session::Layout::TabID tab, const std::string& title) { return m_workspace.renameTab(surface, tab, title); }

    /// Moves focus spatially; the newly focused pane's backend hears it.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - direction: Which way the eye moves.
    /// - Returns: Whether focus changed.
    bool moveFocus(Session::Layout::SurfaceID surface, Ubiquitty::Panes::Direction direction)
    {
        if (!m_workspace.moveFocus(surface, direction))
            return false;
        noteFocus(surface);
        return true;
    }

    /// Focuses a pane directly — the click's landing.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - pane: The pane the press landed in.
    /// - Returns: Whether focus changed.
    bool focusPane(Session::Layout::SurfaceID surface, Session::Layout::PaneID pane)
    {
        if (!m_workspace.focusPane(surface, pane))
            return false;
        noteFocus(surface);
        return true;
    }

    /// The focused pane of a window's surface.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The pane the user is typing into.
    [[nodiscard]] Session::Layout::PaneID focusedPane(Session::Layout::SurfaceID surface) const { return m_workspace.focusedPane(surface); }

    /// The zoom intent of a window's surface.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The zoomed pane, or `Layout::invalidPane`.
    [[nodiscard]] Session::Layout::PaneID zoomedPane(Session::Layout::SurfaceID surface) const { return m_workspace.zoomedPane(surface); }

    /// The split tree a window projects.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The tree, or null for a dead surface.
    [[nodiscard]] const Ubiquitty::Panes::Tree* paneTree(Session::Layout::SurfaceID surface) const { return m_workspace.activePaneTree(surface); }

    /// One tab's split tree, active or not — a tab strip labels every tab
    /// by facts of its tree, not only the visible one's.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - tab: The tab to read.
    /// - Returns: The tab's tree, or null for an unknown id. The pointer is
    ///   owned by the model and invalidated by the next mutating call.
    [[nodiscard]] const Ubiquitty::Panes::Tree* paneTree(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) const { return m_workspace.paneTree(surface, tab); }

    /// A value copy of the split tree, for a caller that holds no pointer
    /// into the model — the shape a frontend walks while mirroring.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The tree; a dead surface answers a fresh single-pane one.
    [[nodiscard]] Ubiquitty::Panes::Tree paneTreeSnapshot(Session::Layout::SurfaceID surface) const
    {
        const Ubiquitty::Panes::Tree* tree = m_workspace.activePaneTree(surface);
        return tree != nullptr ? *tree : Ubiquitty::Panes::Tree(1);
    }

    /// The live pane count of a window's surface.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: How many panes the window holds.
    [[nodiscard]] std::ptrdiff_t paneCount(Session::Layout::SurfaceID surface) const
    {
        const Ubiquitty::Panes::Tree* tree = m_workspace.activePaneTree(surface);
        return tree != nullptr ? tree->paneCount() : 0;
    }

    /// Whether "Share This Window" can hand a window's session to the local
    /// daemon.
    ///
    /// Every pane must be local or already handed over by this client. Including
    /// committed panes permits retry after a partial handoff without adopting
    /// the same PTY twice.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: Whether every tab and pane can participate in the handoff.
    [[nodiscard]] bool isSurfaceShareable(Session::Layout::SurfaceID surface)
    {
        if (!m_workspace.surface(surface))
            return false;
        bool hasPane = false;
        bool canShare = true;
        for (int index = 0; index < m_workspace.tabCount(surface); ++index) {
            const auto* tree = m_workspace.paneTree(surface, m_workspace.tabAt(surface, index));
            if (!tree)
                return false;
            Panes::forEachLeaf(*tree, [&](int leaf) {
                hasPane = true;
                TerminalSession* bound = session(static_cast<Session::Layout::PaneID>(leaf));
                canShare &= bound && (bound->isSharedIntoLocalDaemon() || bound->isShareableLocalSession());
            });
        }
        return hasPane && canShare;
    }

    /// Whether the local daemon holds every pane of a window: each pane's
    /// session was shared into it or handed to it at quit by this client.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: Whether every pane of every tab is the daemon's; false for a
    ///   window with no pane.
    [[nodiscard]] bool isSurfaceHeldByLocalDaemon(Session::Layout::SurfaceID surface)
    {
        if (!m_workspace.surface(surface))
            return false;
        bool hasPane = false;
        bool isHeld = true;
        for (int index = 0; index < m_workspace.tabCount(surface); ++index) {
            const auto* tree = m_workspace.paneTree(surface, m_workspace.tabAt(surface, index));
            if (!tree)
                return false;
            Panes::forEachLeaf(*tree, [&](int leaf) {
                hasPane = true;
                const TerminalSession* bound = session(static_cast<Session::Layout::PaneID>(leaf));
                isHeld &= bound && bound->isSharedIntoLocalDaemon();
            });
        }
        return hasPane && isHeld;
    }

    /// Captures a shared surface with the daemon's session IDs in leaf order.
    /// An incomplete handoff returns an empty value and cannot publish a layout.
    [[nodiscard]] Session::SharedWorkspace captureSharedSurface(Session::Layout::SurfaceID surface)
    {
        Session::SharedWorkspace result;
        if (!m_workspace.surface(surface))
            return result;
        struct Capture {
            WorkspaceSession& model;
            Session::SharedWorkspace& result;
            bool isComplete = true;
        } capture { *this, result };
        result.arrangement = Session::Layout::captureToSnapshot(m_workspace, [](void* context, Session::Layout::PaneID pane) {
            auto& capture = *static_cast<Capture*>(context);
            const auto* bound = capture.model.session(pane);
            const auto id = bound ? bound->sharedSessionID() : 0;
            capture.isComplete &= id != 0;
            capture.result.sessions.push_back(id);
            return Session::Layout::PaneInfo {}; }, &capture, surface);
        return capture.isComplete ? result : Session::SharedWorkspace {};
    }

    /// What the session capture decides one window's tab by
    /// (``Session::planCapturedTab``), for a window whose panes this model
    /// owns.
    ///
    /// A shared window is a thin client of the local daemon: the daemon holds
    /// its shell, so the tab is stored as the adopted session and a restore
    /// rejoins it through this machine's bridge. Stored as a fresh shell
    /// instead, the next launch would open an empty one while the shell and
    /// its jobs ran on in the daemon with no window to reach them.
    ///
    /// A split the daemon can hold whole is one a quit hands over whole,
    /// including one whose panes it already holds from a quit that was
    /// cancelled after the handoff. A split it holds only in part is no thin
    /// client: the panes it does not hold end with the application.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: The facts; empty for a window whose panes another authority
    ///   owns.
    [[nodiscard]] Session::TabCaptureFacts tabCaptureFacts(Session::Layout::SurfaceID surface)
    {
        Session::TabCaptureFacts facts;
        facts.paneLayout = captureLayoutAtSessionGrids(surface);
        const TerminalSession* focused = session(m_workspace.focusedPane(surface));
        const bool isHeld = isSurfaceHeldByLocalDaemon(surface);
        facts.isShareableLocalSession = m_workspace.tabCount(surface) == 1 && isSurfaceShareable(surface) && (paneCount(surface) > 1 || !isHeld);
        if (focused != nullptr && isHeld) {
            facts.isRemote = true;
            facts.remoteSessionID = focused->sharedSessionID();
            facts.remoteHostLabel = Session::localHostLabel;
            facts.remoteBridgeCommand = focused->sharedBridgeCommand();
            facts.remoteSocketName = focused->remoteSocketName();
        }
        return facts;
    }

    /// Hands every pane's running shell of a window to the local daemon at
    /// quit, leaving the window as it is
    /// (``TerminalSession::handOverToLocalDaemon``).
    ///
    /// Panes go in leaf order, and a pane the daemon already holds is not
    /// offered again. The first refusal stops the walk: a daemon that took no
    /// shell is offered no other, for the reason
    /// ``Session::conditionsAfterHandOff`` gives, and the panes it did take
    /// stay the daemon's.
    ///
    /// - Parameters:
    ///   - surface: The window's surface.
    ///   - socketPath: The daemon's control socket, already answering.
    ///   - handoffAddress: A ``Session::AdoptHandoff``, as an address.
    /// - Returns: The first leaf's adopted session id once the daemon holds
    ///   every pane, which is the session a restore attaches through
    ///   (``captureSharedSurface``); zero for a window that is not shareable
    ///   (``isSurfaceShareable``) or a daemon that refused a pane.
    std::uint32_t handSurfaceToLocalDaemon(Session::Layout::SurfaceID surface, const std::string& socketPath, std::uintptr_t handoffAddress)
    {
        const Ubiquitty::Panes::Tree* tree = m_workspace.activePaneTree(surface);
        if (m_workspace.tabCount(surface) != 1 || tree == nullptr || !isSurfaceShareable(surface))
            return 0;
        bool isRefused = false;
        Panes::forEachLeaf(*tree, [&](int leaf) {
            TerminalSession* bound = session(static_cast<Session::Layout::PaneID>(leaf));
            if (isRefused || bound == nullptr || bound->isSharedIntoLocalDaemon())
                return;
            isRefused = bound->handOverToLocalDaemon(socketPath, handoffAddress) == 0;
        });
        if (isRefused)
            return 0;
        const Session::SharedWorkspace held = captureSharedSurface(surface);
        return held.sessions.empty() ? 0 : held.sessions.front();
    }

    /// A pane's session, or null for an unbound id.
    ///
    /// Borrowed, not handed over: the reference `TerminalSession::make` returned
    /// lives in `m_bindings` and is released there, and this looks one up
    /// without retaining it. That is what `UBIQUITTY_RETURNS_UNRETAINED` states,
    /// and the Swift importer has no way to infer it — a foreign reference type
    /// crossing the seam with neither convention spelled makes the caller either
    /// leak the session or release one the model still holds.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: The session this model retains.
    [[nodiscard]] TerminalSession* session(Session::Layout::PaneID pane) UBIQUITTY_RETURNS_UNRETAINED
    {
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.pane == pane)
                return binding.session;
        }
        return nullptr;
    }

    /// Whether one pane can close without killing a live job.
    ///
    /// The shared close-confirmation rule (`Session::needsCloseConfirmation`)
    /// over the pane's fresh process facts: false only for a real foreground
    /// job, never for a shell sitting at its prompt.
    ///
    /// - Parameter pane: The pane to ask about.
    /// - Returns: Whether closing it kills nothing the user would miss.
    [[nodiscard]] bool isPaneSafeToClose(Session::Layout::PaneID pane)
    {
        TerminalSession* bound = session(pane);
        return bound == nullptr || !bound->needsCloseConfirmation();
    }

    /// Whether a surface's close button should carry the running-job marker
    /// (macOS's edited dot).
    ///
    /// Answered from each pane's last process sample, the instant the
    /// `processStateChanged` that redraws the marker announced; see
    /// `TerminalSession::needsRunningJobMarker`. Every pane of every tab
    /// counts, as it does for the close itself.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: Whether any pane's last sample found a job a close would kill.
    [[nodiscard]] bool needsRunningJobMarker(Session::Layout::SurfaceID surface)
    {
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.surface == surface && binding.session->needsRunningJobMarker())
                return true;
        }
        return false;
    }

    /// Whether a whole surface can close without killing a live job — the
    /// fact behind any close-confirmation sheet. Every pane of every tab
    /// counts: a job in a background tab dies with the window just the same.
    ///
    /// - Parameter surface: The window's surface.
    /// - Returns: Whether closing the window kills nothing.
    [[nodiscard]] bool isSurfaceSafeToClose(Session::Layout::SurfaceID surface)
    {
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.surface == surface
                && binding.session->needsCloseConfirmation())
                return false;
        }
        return true;
    }

    /// Describes the processes terminated by closing a surface or one of its panes.
    ///
    /// - Parameters:
    ///   - surface: The surface being closed.
    ///   - scope: The noun used in the confirmation.
    ///   - pane: A single pane, or invalidPane for the whole surface.
    /// - Returns: The shared close-confirmation body.
    [[nodiscard]] std::string closeConfirmationBody(Session::Layout::SurfaceID surface, Session::CloseScope scope,
        Session::Layout::PaneID pane = Session::Layout::invalidPane)
    {
        std::vector<std::string> names;
        for (auto& binding : m_bindings) {
            if (!binding.session || binding.surface != surface || (pane != Session::Layout::invalidPane && binding.pane != pane)) {
                continue;
            }
            for (auto& name : binding.session->runningProcessNamesForClose()) {
                names.push_back(std::move(name));
            }
        }
        return Session::closeConfirmationBody(scope, names);
    }

    /// Resolves an ended local pane's exit policy without acting on remote disconnects.
    ///
    /// - Parameters:
    ///   - pane: The local pane whose child ended.
    ///   - behavior: The effective profile choice.
    /// - Returns: The next action, bounded by this pane's restart budget.
    [[nodiscard]] Session::ProcessExitAction processExitAction(Session::Layout::PaneID pane, Session::ProcessExitBehavior behavior)
    {
        for (auto& binding : m_bindings) {
            if (binding.pane != pane || !binding.session || !binding.session->endedLocalCommand()) {
                continue;
            }
            binding.session->collectSessionExitStatus();
            const auto action = binding.exitPolicy.action(behavior, binding.session->sessionEnd(), PAL::monotonicTicks());
            if (action == Session::ProcessExitAction::Restart) {
                binding.isRestartPending = true;
            }
            return action;
        }
        return Session::ProcessExitAction::KeepOutput;
    }

    /// Sets the socket every local pane's child is told serves it.
    ///
    /// Read at each launch, so a socket that starts after the first window
    /// reaches the panes opened from then on.
    ///
    /// - Parameter path: The control socket's path; empty when none is served.
    void setContextSocketPath(const std::string& path) { m_contextSocketPath = path; }

    /// Returns where a pane's child runs: the socket and the pane, written as
    /// the identities a tool inside it resolves `here` with.
    ///
    /// Only what the socket can resolve is named. The control socket addresses
    /// a pane by this workspace's pane ID, but a window by its window number
    /// and a tab not at all, so the workspace's window and tab IDs would name
    /// nothing a tool could ask about. A local pane has no host session either.
    ///
    /// - Parameter pane: A pane the workspace holds.
    /// - Returns: The context; a pane the workspace does not hold names only
    ///   the socket.
    [[nodiscard]] Session::LaunchContext launchContext(Session::Layout::PaneID pane) const
    {
        Session::LaunchContext context;
        context.socketPath = m_contextSocketPath;
        if (m_workspace.surfaceHolding(pane) == Session::Layout::invalidSurface)
            return context;
        context.pane = Session::Identity::ObjectID { Session::Identity::ObjectKind::Pane, std::to_underlying(pane) };
        return context;
    }

    /// Replaces an ended local process while preserving its pane and restart budget.
    ///
    /// Views retain the ended session until they bind the replacement. A failed
    /// launch leaves the old output and layout available.
    ///
    /// - Parameter pane: The ended local pane.
    /// - Returns: Whether a fresh process was created.
    bool restartEndedPane(Session::Layout::PaneID pane)
    {
        for (auto& binding : m_bindings) {
            if (binding.pane != pane || !binding.session) {
                continue;
            }
            if (!binding.isRestartPending) {
                return false;
            }
            if (!binding.session->canClose()) {
                return false;
            }
            binding.isRestartPending = false;
            const LaunchCommand* command = binding.session->endedLocalCommand();
            if (!command) {
                return false;
            }
            TerminalSession* replacement = makeLocalSession(*command, pane);
            if (!replacement) {
                return false;
            }
            TerminalSession* ended = binding.session;
            binding.session = replacement;
            binding.exitPolicy.didLaunch(PAL::monotonicTicks());
            releaseTerminalSession(ended);
            return true;
        }
        return false;
    }

    /// The last launch's seeding report — trigger-config diagnostics a
    /// frontend may want to log; empty when there was nothing to say.
    ///
    /// By value: a frontend that reads this through the Swift importer cannot
    /// take a reference into the model's storage, and the report is one short
    /// string per window rather than anything a copy is felt in.
    ///
    /// - Returns: The report text.
    [[nodiscard]] std::string lastLaunchReport() const { return m_lastLaunchReport; }

private:
#if defined(UBIQUITTY_ENABLE_TEST_HOOKS)
    friend struct WorkspaceSessionTestAccess;
#endif

    WorkspaceSession(IngestPolicy ingestPolicy, DrawnText drawnText, const Preferences::Values& configuration)
        : m_ingestPolicy(ingestPolicy)
        , m_drawnText(drawnText)
        , m_configuration(configuration)
        , m_clientInstaller(s_defaultClientInstaller)
        , m_clientInstallerContext(s_defaultClientInstallerContext)
    {
    }

    /// One pane's backing session, retained here, with the surface the
    /// pane was minted into — the teardown key, so a close accounts for
    /// background tabs' panes without walking any tree.
    struct Binding {
        Session::Layout::PaneID pane = Session::Layout::invalidPane;
        Session::Layout::SurfaceID surface = Session::Layout::invalidSurface;
        TerminalSession* session = nullptr;
        Session::ProcessExitPolicy exitPolicy;
        bool isRestartPending = false;
    };

    /// Makes, starts, launches and seeds one local pane's session — the
    /// whole launch fold resolved from config: the grid, the scrollback,
    /// the per-terminal seeding, and the ingest start the policy asks for.
    ///
    /// - Parameter command: What the pane runs.
    /// - Returns: The running session, or null when any stage refuses.
    /// Makes and starts a session, seeded from config but running nothing.
    ///
    /// The half a launch shares with a line: the grid, the scrollback and the
    /// per-terminal seeding are the same whether bytes come from a child or
    /// from a serial port, and only the source differs.
    ///
    /// - Returns: The started session, or null when allocation refused.
    [[nodiscard]] TerminalSession* makeSession()
    {
        TerminalSession* session = TerminalSession::make(m_configuration);
        if (session == nullptr)
            return nullptr;
        const Preferences::Values& config = m_configuration;
        RuntimeInitParameters parameters;
        parameters.rows = Preferences::Effective::launchRows(config);
        parameters.columns = Preferences::Effective::launchColumns(config);
        parameters.allowReflowOnInit = false;
        parameters.drawsSizedText = drawsSizedText(m_drawnText);
        parameters.drawsProportionalText = drawsProportionalText(m_drawnText);
        session->start(parameters);
        if (Ubiquitty::Terminal* term = session->terminal())
            m_lastLaunchReport = seedTerminalConfiguration(*term, config);
        if (m_clientInstaller != nullptr)
            m_clientInstaller(m_clientInstallerContext, *session);
        return session;
    }

    /// Starts a local session for a pane the workspace holds.
    ///
    /// The child is told where it runs (Core/Session/LaunchContext.h): the socket
    /// this model's frontend serves, and the window, tab, and pane.
    ///
    /// - Parameters:
    ///   - launch: What to run.
    ///   - pane: The pane the session will back, already in the workspace.
    /// - Returns: The running session, or null when it could not start.
    [[nodiscard]] TerminalSession* makeLocalSession(const LaunchCommand& launch, Session::Layout::PaneID pane)
    {
        LaunchCommand command = launch;
        // A relaunched command carries the context it was first stamped with;
        // the pane's current one replaces it, and any other override stays.
        std::vector<std::string> environment;
        for (const std::string& entry : launch.environmentOverrides()) {
            if (!Session::isContextEnvironmentEntry(entry))
                environment.push_back(entry);
        }
        for (std::string& entry : Session::contextEnvironment(launchContext(pane)))
            environment.push_back(std::move(entry));
        command.setEnvironmentOverrides(std::move(environment));
        TerminalSession* session = TerminalSession::make(m_configuration);
        if (session == nullptr)
            return nullptr;
        const Preferences::Values& config = m_configuration;
        RuntimeInitParameters parameters;
        parameters.rows = Preferences::Effective::launchRows(config);
        parameters.columns = Preferences::Effective::launchColumns(config);
        parameters.allowReflowOnInit = false;
        parameters.drawsSizedText = drawsSizedText(m_drawnText);
        parameters.drawsProportionalText = drawsProportionalText(m_drawnText);
        session->start(parameters);
        if (Ubiquitty::Terminal* term = session->terminal())
            m_lastLaunchReport = seedTerminalConfiguration(*term, config);
        if (!session->launch(command)) {
            session->close();
            releaseTerminalSession(session);
            return nullptr;
        }
        if (m_clientInstaller != nullptr)
            m_clientInstaller(m_clientInstallerContext, *session);
        if (m_ingestPolicy == IngestPolicy::ThreadDrained && !session->startIngest()) {
            session->close();
            releaseTerminalSession(session);
            return nullptr;
        }
        return session;
    }

    /// Records a pane's backing session in the retained table.
    ///
    /// - Parameters:
    ///   - surface: The surface the pane was minted into.
    ///   - pane: The pane the Workspace minted.
    ///   - session: The session this model takes ownership of.
    void bind(Session::Layout::SurfaceID surface, Session::Layout::PaneID pane, TerminalSession* session)
    {
        for (auto& binding : m_bindings) {
            if (binding.session == nullptr) {
                binding.pane = pane;
                binding.surface = surface;
                binding.session = session;
                binding.exitPolicy.didLaunch(PAL::monotonicTicks());
                return;
            }
        }
        // The pool mirrors the Workspace's own pane bound; a full table
        // means the workspace refused the pane first.
        session->close();
        releaseTerminalSession(session);
    }

    /// Preflights every affected owner before any close can change the layout
    /// or release a sibling. Workspace mutations run on one frontend thread.
    [[nodiscard]] bool canCloseSurfaceSessions(Session::Layout::SurfaceID surface) const
    {
        for (const auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.surface == surface && !binding.session->canClose())
                return false;
        }
        return true;
    }

    [[nodiscard]] bool canCloseTreeSessions(const Ubiquitty::Panes::Tree& tree) const
    {
        for (const auto& binding : m_bindings) {
            if (binding.session != nullptr && tree.holdsPane(static_cast<int>(binding.pane)) && !binding.session->canClose())
                return false;
        }
        return true;
    }

    /// Ends every session bound to a leaf of one tree — a closing tab's.
    ///
    /// - Parameter tree: The tree whose leaves' sessions end.
    void unbindTreeLeaves(const Ubiquitty::Panes::Tree& tree)
    {
        for (int index = 0; index < Ubiquitty::Panes::maxNodes; ++index) {
            if (!tree.isLeaf(index))
                continue;
            const int leafPane = tree.node(index).paneID;
            for (auto& binding : m_bindings) {
                if (binding.session != nullptr && static_cast<int>(binding.pane) == leafPane) {
                    binding.session->close();
                    releaseTerminalSession(binding.session);
                    binding = {};
                }
            }
        }
    }

    /// Ends a pane's session and frees its table slot.
    ///
    /// - Parameter pane: The pane whose session ends.
    void unbind(Session::Layout::PaneID pane)
    {
        for (auto& binding : m_bindings) {
            if (binding.session != nullptr && binding.pane == pane) {
                binding.session->close();
                releaseTerminalSession(binding.session);
                binding = {};
                return;
            }
        }
    }

    /// Tells the focused pane's backend that focus arrived.
    ///
    /// - Parameter surface: The window's surface.
    void noteFocus(Session::Layout::SurfaceID surface)
    {
        if (TerminalSession* focused = session(m_workspace.focusedPane(surface)))
            (void)focused->requestPaneVerb(TerminalSession::PaneVerb::Focus);
    }

    Session::Layout::Workspace m_workspace;
    // Bounded generously past any real window count; the Workspace's own
    // pools refuse first.
    static constexpr std::size_t maxBindings = 256;
    std::array<Binding, maxBindings> m_bindings {};
    IngestPolicy m_ingestPolicy = IngestPolicy::ThreadDrained;
    DrawnText m_drawnText = DrawnText::SizedAndProportional;
    // Borrowed, never owned: the maker keeps the instance alive past this
    // model, and every session this model makes borrows the same instance.
    const Preferences::Values& m_configuration;
    std::string m_contextSocketPath;
    std::string m_lastLaunchReport;
    // The frontend's client seam — see setTerminalClientInstaller. Null = no
    // clients installed, which is what a frontend without them means. The
    // statics are the process-wide seed construction copies.
    TerminalClientInstaller m_clientInstaller = nullptr;
    void* m_clientInstallerContext = nullptr;
    inline static TerminalClientInstaller s_defaultClientInstaller = nullptr;
    inline static void* s_defaultClientInstallerContext = nullptr;
} UBIQUITTY_SHARED_REFERENCE(retainWorkspaceSession, releaseWorkspaceSession);

} // namespace Ubiquitty::Runtime

/// Claims a reference to `model` on Swift's behalf.
///
/// - Parameter model: The model to retain. A null pointer is ignored.
inline void retainWorkspaceSession(Ubiquitty::Runtime::WorkspaceSession* model)
{
    if (model)
        model->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops a reference to `model`, destroying it at zero.
///
/// - Parameter model: The model to release. A null pointer is ignored.
inline void releaseWorkspaceSession(Ubiquitty::Runtime::WorkspaceSession* model)
{
    if (model && model->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete model;
}
