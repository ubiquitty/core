// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Core/Session/RemoteSessionCore.h"

#include <functional>
#include <memory>

namespace Ubiquitty::Session {

struct WorkspaceConnectionStorage;

/// Owns one authenticated carrier and topology mirror for every workspace pane.
class WorkspaceConnection {
public:
    using Storage = WorkspaceConnectionStorage;

    /// Removes an event subscription when its session releases the token.
    class Subscription {
    public:
        /// Creates an inactive subscription token.
        Subscription() = default;
        /// Removes the owned callback from its live workspace.
        ~Subscription();
        /// Transfers callback ownership from another token.
        Subscription(Subscription&&) noexcept;
        /// Replaces callback ownership with another token.
        ///
        /// - Returns: This subscription after the transfer.
        Subscription& operator=(Subscription&&) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

    private:
        friend class WorkspaceConnection;
        /// Binds callback removal to its workspace and registration identity.
        Subscription(std::weak_ptr<Storage>, std::uint64_t);
        std::weak_ptr<Storage> m_owner;
        std::uint64_t m_identity { 0 };
    };

    /// Holds the host's questions about panes no endpoint here has joined
    /// yet, and routes them when the last hold ends.
    class QuestionHold {
    public:
        /// Releases this hold; the last one routes what was held.
        ~QuestionHold();
        /// Takes over another hold, which then releases nothing.
        QuestionHold(QuestionHold&&) noexcept = default;
        QuestionHold& operator=(QuestionHold&&) = delete;
        QuestionHold(const QuestionHold&) = delete;
        QuestionHold& operator=(const QuestionHold&) = delete;

    private:
        friend class WorkspaceConnection;
        /// Binds the hold to its workspace.
        explicit QuestionHold(std::weak_ptr<Storage>);
        std::weak_ptr<Storage> m_owner;
    };

    /// Retains the dispatcher and transport factory until the last pane releases the workspace.
    WorkspaceConnection(std::unique_ptr<Dispatcher>, std::unique_ptr<ChannelFactory>);
    /// Releases workspace resources after active dispatch callbacks finish.
    ~WorkspaceConnection();
    WorkspaceConnection(const WorkspaceConnection&) = delete;
    WorkspaceConnection& operator=(const WorkspaceConnection&) = delete;

    /// Returns the sole owner of authentication, reconnect, topology, and clipboard policy.
    [[nodiscard]] RemoteSessionCore& connection();
    [[nodiscard]] const RemoteSessionCore& connection() const;
    /// Returns the dispatcher shared by the physical carrier and pane decoders.
    [[nodiscard]] Dispatcher& dispatcher();
    /// Observes connection, workspace, and clipboard changes with owned callback lifetime.
    [[nodiscard]] Subscription subscribe(std::function<void()>);
    /// Reports whether the authoritative topology admits a pane session.
    [[nodiscard]] bool containsSession(std::uint32_t) const;
    /// Reports whether a known session has no live endpoint attached.
    [[nodiscard]] bool canAttachSession(std::uint32_t) const;
    /// Ends an unneeded provisional carrier with an explicit duplicate-attachment notice.
    void refuseDuplicateAttachment();
    /// Holds the host's questions about panes its topology places here
    /// while endpoints join them.
    ///
    /// A host can ask about a pane in the same read as the structure that
    /// places it, and ahead of it; a pane joining in that update is not yet
    /// established when the carrier first routes the question. While a hold
    /// lasts, a consent or structure question for a placed pane no attached
    /// endpoint shows waits, up to a bound, and a resolution retires its
    /// waiting question. When the last hold ends each one is routed as it
    /// would have been on arrival: to the endpoint that joined its pane, or
    /// declined as unshown. A connection that drops lets them go. The
    /// connection holds questions the same way while it establishes its own
    /// endpoints from a structure, as it does after every reconnect.
    ///
    /// - Returns: The hold, which lasts until it is destroyed.
    [[nodiscard]] QuestionHold holdQuestionsWhileJoining();
    /// Hands every pane endpoint on this connection to another connection of
    /// the same workspace, which survives in its place.
    ///
    /// Two connections in one process carry one workspace when panes were
    /// attached before their workspace was known to be one. The successor is
    /// chosen by the caller, which also vouches that both reach the same
    /// authority. Each endpoint here is told
    /// (`RemoteSessionObserver::onConnectionRetiring`) and moves itself once it
    /// can: its old input settled and its questions answered here, its pane
    /// placed by the successor's topology with a verified picture held there.
    /// An endpoint that joins this connection while it retires is told too.
    /// Once the last endpoint has gone, releasing this connection detaches
    /// its attachment; nothing of it passes to the successor.
    ///
    /// - Parameter successor: A connected owner admitted to the same
    ///   workspace of the same daemon process.
    /// - Returns: Whether this connection now retires into the successor:
    ///   false while either is not connected or they name different
    ///   workspaces or daemon processes, when the successor already shows one
    ///   of this connection's panes, since a second view of a pane is not a
    ///   move, when this one already retires into another, and when the
    ///   successor itself retires. Asked again with the same successor, it
    ///   tells no pane again.
    [[nodiscard]] bool retireInto(const std::shared_ptr<WorkspaceConnection>& successor);
    /// The connection this one retires into, while it lives.
    [[nodiscard]] std::shared_ptr<WorkspaceConnection> successor() const;
    /// Reports whether this connection's carrier holds a verified picture of
    /// a session's pane, whether or not an endpoint shows it.
    [[nodiscard]] bool holdsPicture(std::uint32_t sessionID) const;

private:
    friend class RemoteSessionCore;
    /// Reports whether a request an endpoint made here still awaits its
    /// answer here: a workspace verb or a directory listing.
    [[nodiscard]] bool hasRequestsFor(const RemoteSessionCore&) const;
    /// Registers a decoder without creating a channel or connection retry timer.
    ///
    /// - Returns: Whether the authoritative workspace admitted the decoder.
    bool attach(RemoteSessionCore&, std::uint32_t);
    /// Releases a decoder while preserving the workspace carrier and other panes.
    void detach(RemoteSessionCore&);
    /// Records whether an attached pane's surface is focused, and tells the
    /// carrier's screen plane whether any is.
    ///
    /// A pane that never reported counts as focused, as a connection whose
    /// frontend cannot observe focus does.
    ///
    /// - Parameters:
    ///   - endpoint: The pane's decoder; one not attached is ignored.
    ///   - focus: Whether its surface has the keyboard.
    void noteFocus(RemoteSessionCore& endpoint, Ubiquitty::Modes::FocusState focus);
    /// Notes that a pane sent input, which makes it the pane a request with
    /// no pane of its own is answered for.
    ///
    /// - Parameter endpoint: The pane's decoder; one not attached is ignored.
    void noteInput(RemoteSessionCore& endpoint);
    /// Retires only the exact pane whose fully matched reclaim Result arrived.
    /// - Parameter endpoint: The current attached endpoint, never selected by focus.
    void noteReclaimed(RemoteSessionCore& endpoint);
    /// Routes an attached pane's topology request through the physical request namespace.
    ///
    /// - Returns: The correlated request identity, or zero when the request is refused.
    std::uint32_t sendWorkspaceVerb(RemoteSessionCore&, Ubiquitty::Session::WorkspaceIntent);
    /// Routes a revision-bound window close through the shared physical caller.
    /// - Parameters:
    ///   - endpoint: The attached endpoint that owns the result.
    ///   - surface: The captured host window.
    ///   - revision: The workspace revision captured by its confirmation.
    /// - Returns: A request identity, or zero without an admitted request.
    std::uint32_t sendWindowClose(RemoteSessionCore& endpoint, Layout::SurfaceID surface, std::uint64_t revision);
    /// Requests the authority's directory for one attached endpoint.
    ///
    /// - Returns: The correlated request identity, or zero while another request is pending.
    std::uint32_t requestSessionList(RemoteSessionCore&);
    std::shared_ptr<Storage> m_storage;
};

} // namespace Ubiquitty::Session
