// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Core/Session/SessionTree.h"
#include "Core/Session/WorkspaceIntent.h"
#include "UbiquittyKit/Session.h"
#include <memory>
#include <string>
#include <string_view>

namespace UbiquittyKit {

/// Drives a host-owned workspace without a UI toolkit or a renderer.
///
/// One carrier is shared by every pane; a second watches the host directory.
/// All methods and observer calls run on the supplied dispatcher's thread.
/// Call synchronize() after draining that dispatcher, before reading a frame.
/// This reconciles pane owners outside the decoder callback that reported a
/// topology change. The observer outlives the workspace. The workspace owns
/// the dispatcher, factory, replicated terminals and their transports.
class Workspace final {
public:
    /// Selects an existing session or workspace, or creates a workspace when empty.
    struct Target {
        std::uint32_t sessionID = 0;
        std::uint32_t workspaceID = 0;
        std::string name;
    };
    /// Receives workspace changes on the owned dispatcher's thread.
    class Observer {
    public:
        virtual ~Observer() = default;
        /// A terminal, connection, topology or directory may have changed.
        virtual void onChanged() { }
        /// A workspace operation received its host-authoritative answer.
        virtual void onIntentResult(const Ubiquitty::Session::WorkspaceIntentResult&) { }
        /// A directory watch stopped or could not start; the carrier may live.
        virtual void onDirectoryError(std::string_view) { }
    };

    /// Takes ownership of dispatch and transport services for one workspace.
    ///
    /// - Parameters:
    ///   - dispatcher: The queue on which all workspace work runs.
    ///   - factory: The transport factory shared by the workspace and directory.
    ///   - observer: The change recipient, which outlives this workspace.
    Workspace(std::unique_ptr<Ubiquitty::Session::Dispatcher> dispatcher,
        std::unique_ptr<Ubiquitty::Session::ChannelFactory> factory, Observer& observer);
    /// Stops the client carriers while leaving host sessions running.
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    /// Starts once. A nonzero session attaches only; an absent session fails.
    /// An empty target creates a workspace; an ID or name selects one.
    ///
    /// - Parameters:
    ///   - target: The requested host-owned session or workspace.
    ///   - columns: The initial viewport width, clamped to Core's limits.
    ///   - rows: The initial viewport height, clamped to Core's limits.
    void start(const Target& target, int columns, int rows);
    /// Reconciles topology and derives the tree from the host's pane records.
    void synchronize();
    /// Stops the client carriers; the daemon's sessions keep running.
    void stop();
    /// The owned dispatcher, for an embedder that pumps its own event queue.
    [[nodiscard]] Ubiquitty::Session::Dispatcher& dispatcher();
    /// The current workspace, absent until its topology has arrived.
    [[nodiscard]] const Ubiquitty::Session::TreeWorkspace* tree() const;
    /// The host directory, updated while the workspace is connected.
    [[nodiscard]] const Ubiquitty::Session::SessionTree& directory() const;
    /// Whether the directory watch has delivered a live snapshot.
    [[nodiscard]] bool directoryIsLive() const;
    /// Restarts a failed directory watch without recreating the workspace.
    void watchDirectory();
    /// The pane's replicated terminal, borrowed until synchronize() or stop().
    ///
    /// - Parameter sessionID: The host-owned pane session identity.
    /// - Returns: The borrowed terminal, or null for an absent or ended pane.
    [[nodiscard]] const Ubiquitty::Terminal* terminal(std::uint32_t sessionID) const;
    /// The initially attached session, once acknowledged; zero before then.
    [[nodiscard]] std::uint32_t initialSessionID() const;
    /// The acknowledged daemon socket, retained across reconnects.
    [[nodiscard]] std::string socketName() const;
    /// The workspace carrier's current connection state.
    [[nodiscard]] Ubiquitty::Session::State state() const;
    /// A connection failure, empty while the workspace is healthy.
    [[nodiscard]] std::string_view error() const;
    /// Whether every host-owned pane has ended.
    [[nodiscard]] bool hasEnded() const;
    /// Sets a pane's visible viewport and focus. Zero geometry makes it dormant.
    ///
    /// - Parameters:
    ///   - sessionID: The host-owned pane session identity.
    ///   - columns: The visible width, or zero to make the pane dormant.
    ///   - rows: The visible height, or zero to make the pane dormant.
    ///   - focus: Whether this pane receives the surface's keyboard focus.
    /// - Returns: Whether this workspace currently holds that pane.
    bool setView(std::uint32_t sessionID, int columns, int rows, Ubiquitty::Modes::FocusState focus);
    /// Stages pane pixel metrics for the next viewport report through setView().
    ///
    /// - Parameters:
    ///   - sessionID: The host-owned pane session identity.
    ///   - geometry: Cell, text area and drawable sizes in device pixels.
    /// - Returns: Whether this workspace currently holds that pane.
    bool setPixelGeometry(std::uint32_t sessionID, const Ubiquitty::PixelGeometry& geometry);
    /// Sets a pane's history demand relative to its live tail.
    ///
    /// - Parameters:
    ///   - sessionID: The host-owned pane session identity.
    ///   - rowsAboveTail: Rows of history above the live viewport; zero follows output.
    /// - Returns: Whether this workspace currently holds that pane.
    bool setViewOffset(std::uint32_t sessionID, int rowsAboveTail);
    /// Sends exact bytes, including NULs, to an established pane.
    ///
    /// - Parameters:
    ///   - sessionID: The host-owned pane session identity.
    ///   - bytes: Application input, borrowed for the duration of the call.
    /// - Returns: Whether the pane exists and its input was handed to Core.
    bool sendInput(std::uint32_t sessionID, std::string_view bytes);
    /// Sends an operation through one established pane's authenticated carrier.
    /// Core and the host validate identities, revisions and permissions.
    ///
    /// - Parameters:
    ///   - sessionID: The pane whose carrier sends the operation.
    ///   - intent: The workspace operation and its host-owned target identities.
    /// - Returns: Whether Core accepted the operation for delivery.
    bool sendIntent(std::uint32_t sessionID, Ubiquitty::Session::WorkspaceIntent intent);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace UbiquittyKit
