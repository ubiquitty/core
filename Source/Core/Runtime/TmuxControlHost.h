// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// TmuxControlHost.h — a tmux -CC client running in a session, speaking for the
// windows it hosts.
//
// Everything a frontend would otherwise write again to host tmux lives here:
// the portable Session that owns every pane's terminal, the event client the
// primary's parser calls, the pane router that delivers %output into a pane's
// terminal under that pane's lock, the pane registry the windows are keyed
// by, the attach-time screen and mode recovery, the resize resync, the
// copy-mode clipboard pull and the client-size report. A frontend reads
// ``HostedWindowEvent``s and binds views to panes; it never learns
// that the program behind them is tmux.
//
// Threads. The event client and the router run on the primary's ingest
// thread with its model lock held. The frontend's calls run on its own loop.
// Work the host does later — a debounced resync, a capture retry — is posted
// to that loop. Shared state sits under one leaf lock that is never held
// while a pane lock or the primary's lock is taken.

#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Protocol/Tmux/Session.h"
#include "Core/Protocol/Tmux/StateRecovery.h"
#include "Core/Runtime/HostedPanePublication.h"
#include "Core/Runtime/HostedWindows.h"
#include "Core/Runtime/PostedDispatcher.h"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Runtime {

/// A tmux control-mode client's windows, hosted for a frontend.
class TmuxControlHost final : public HostedWindowSource {
    using PaneID = Protocol::Tmux::PaneID;
    using CaptureScope = Protocol::Tmux::CaptureScope;
    using CaptureForm = Protocol::Tmux::CaptureForm;
    using TmuxSession = Protocol::Tmux::Session;
    using TmuxPane = Protocol::Tmux::Pane;

public:
    /// Builds a host over a session's primary terminal and channel.
    ///
    /// - Parameter seams: The primary, its lock and channel, and the session
    ///   and frontend-loop calls the host makes.
    explicit TmuxControlHost(const HostedWindowSeams& seams);

    /// Frees the Session and every pane terminal still in it.
    ///
    /// The frontend has unbound every pane view and the host is detached by
    /// then; posted work still in flight finds the host gone and does nothing.
    ~TmuxControlHost() override;

    TmuxControlHost(const TmuxControlHost&) = delete;
    TmuxControlHost& operator=(const TmuxControlHost&) = delete;

    /// Installs the event client, the pane router and control-mode detection
    /// on the primary under its lock, and asks tmux to emit its state again.
    void attach() override;

    /// Takes the host's hooks back out of the primary under its lock, which
    /// returns the primary's parser to passthrough.
    void detach() override;

    /// Reads the server identity confirmed by the control connection.
    ///
    /// - Returns: The reference, or an unconfirmed reference before the reply.
    [[nodiscard]] HostedSessionReference sessionReference() override
    {
        auto reference = m_session.sessionReference();
        return reference ? HostedSessionReference { .tmux = *reference, .isConfirmed = true } : HostedSessionReference {};
    }

    /// The oldest hosted-window change not yet shown, removed from the queue.
    ///
    /// - Returns: The event, whose kind is `None` when the queue is empty.
    [[nodiscard]] HostedWindowEvent takeEvent() override;

    /// The panes whose terminals changed since the last call, each once.
    ///
    /// - Returns: The pane ids; empty when nothing changed.
    [[nodiscard]] std::vector<int> takeRepaintedPanes() override;

    /// A pane's terminal, its lock, and the client and channel its input
    /// wraps through.
    ///
    /// - Parameter paneID: The tmux pane.
    /// - Returns: Its facts; zero addresses when the Session holds no such pane.
    [[nodiscard]] HostedPaneFacts paneFacts(int paneID) override;
    [[nodiscard]] bool hasPanePublications() const override { return true; }
    [[nodiscard]] std::optional<Session::FrameReady> panePublication(int paneID, const Ubiquitty::Terminal&) override;
    [[nodiscard]] std::uint64_t publicationClockMicroseconds() const override;
    [[nodiscard]] bool setPaneFocus(int paneID, const Ubiquitty::Terminal&, Modes::FocusState) override;

    /// Frees the pane once tmux has retired it, now that no view reads it.
    ///
    /// - Parameter paneID: The tmux pane whose view unbound.
    void releasePane(int paneID) override;

    /// Retires a closed window's panes, freeing each one its view released.
    ///
    /// - Parameter windowID: The tmux window.
    void releaseWindow(int windowID) override;

    /// Resizes a pane's terminal to its view's grid, geometry only, ahead of
    /// tmux's `%layout-change`.
    ///
    /// - Parameters:
    ///   - paneID: The tmux pane.
    ///   - columns: The view's columns.
    ///   - rows: The view's rows.
    void resizePane(int paneID, int columns, int rows) override;

    /// Sends `refresh-client -C` and sizes the panes of every window not
    /// holding `paneID` to the new client size, in proportion.
    ///
    /// - Parameters:
    ///   - paneID: A pane of the reporting window.
    ///   - columns: The window's columns.
    ///   - rows: The window's rows.
    /// - Returns: Whether the report was sent: false for a grid with no cells
    ///   or a host with no channel.
    bool reportClientSize(int paneID, int columns, int rows) override;

    /// The Session every hosted pane's terminal belongs to, for a test that
    /// reads a pane's model.
    ///
    /// - Returns: The session model.
    [[nodiscard]] TmuxSession& sessionForTesting() { return m_session; }

private:
    /// One window the host has told its frontend about.
    struct WindowRecord {
        /// The pane tmux named active on the listing, consumed by the first
        /// layout the frontend is sent.
        PaneID activePaneID = Protocol::Tmux::invalidPaneID;
        /// Whether the frontend has been told the window exists.
        bool isOpened = false;
    };

    /// The pane geometry a capture was asked against, so a feed that lands
    /// after the pane resized again is dropped rather than written at the
    /// wrong width.
    struct IssuedGeometry {
        int columns = 0;
        int rows = 0;
    };

    /// A resync waiting for a live resize to settle.
    struct PendingResync {
        std::uint64_t generation = 0;
        CaptureScope scope = CaptureScope::CurrentScreen;
    };

    /// Asks tmux for the client's size, its version and the attached
    /// session's panes, on the parser's switch into control mode.
    ///
    /// - Parameter context: The host.
    static void onEnterControlMode(void* context);

    /// Lists a new window's panes, whose rows open it.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - windowID: The tmux window.
    static void onWindowAdd(void* context, int windowID);

    /// Queues a closed window for the frontend, which answers with
    /// ``releaseWindow`` once its views are unbound.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - windowID: The tmux window.
    static void onWindowClose(void* context, int windowID);

    /// Queues a window's new name.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - windowID: The tmux window.
    ///   - name: Its name.
    static void onWindowRenamed(void* context, int windowID, std::string_view name);

    /// Queues the session's new name.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - name: Its name.
    static void onSessionRenamed(void* context, std::string_view name);

    /// Queues the end of the hosting; later output is dropped.
    ///
    /// - Parameter context: The host.
    static void onExit(void* context);

    /// Routes one command-response line: a mode report, a clipboard pull, a
    /// capture, the session's identity, the server's version, or a pane
    /// listing row.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - line: The line, bounded by its own length.
    static void onCommandResponseLine(void* context, std::string_view line);
    /// Invalidates the saved identity and queries the newly selected session.
    ///
    /// - Parameter context: The hosting control connection.
    static void onSessionChanged(void* context);

    /// Advances the capture and clipboard-pull machines on a block boundary.
    ///
    /// - Parameter context: The host.
    static void onCommandResponseEnd(void* context);

    /// Reconciles a window's panes to a new layout and queues it.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - windowID: The tmux window.
    ///   - layout: tmux's layout string.
    static void onLayoutChange(void* context, int windowID, std::string_view layout);

    /// Resumes a paused pane at once.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - paneID: The tmux pane.
    static void onPaneDidPause(void* context, PaneID paneID);

    /// Asks for a pane's modes again and pulls its paste buffer.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - paneID: The tmux pane.
    static void onPaneDidChangeMode(void* context, PaneID paneID);

    /// The terminal a run of `%output` for a pane lands in.
    ///
    /// - Parameters:
    ///   - context: The host.
    ///   - paneID: The tmux pane.
    /// - Returns: The pane's terminal, held for the run; the primary for a
    ///   pane not modelled yet; null once the hosting ended.
    static Ubiquitty::Terminal* routePane(void* context, PaneID paneID);

    /// Releases the pane a parse pass held.
    ///
    /// - Parameter context: The host.
    static void endDeliveryPass(void* context);

    /// Writes one command line to the channel and wakes a queued tail.
    ///
    /// - Parameter command: The command, with or without its newline.
    void sendCommand(std::string_view command);

    /// Writes command lines to the channel in one write, and wakes a queued
    /// tail.
    ///
    /// A marker and the command whose block it names are answered as two
    /// blocks the reader pairs by order. The ingest thread and the frontend
    /// loop both ask for captures, and a second capture's marker written
    /// between another's two lines makes it the first one's screen. One write
    /// lands whole: the channel queues each under its own lock.
    ///
    /// - Parameter commands: The commands, each with or without its newline.
    void sendCommands(std::initializer_list<std::string_view> commands);

    /// Opens a listed pane's window once, applying its cached layout.
    ///
    /// - Parameters:
    ///   - windowID: The tmux window.
    ///   - paneID: The listed pane.
    ///   - sessionName: The session the listing named.
    ///   - windowName: The window's name.
    void discoverPane(int windowID, PaneID paneID, const std::string& sessionName, const std::string& windowName);

    /// Reconciles the Session to a layout, queues it, recovers new panes and
    /// schedules the resync of the rest.
    ///
    /// - Parameters:
    ///   - windowID: The tmux window.
    ///   - layout: tmux's layout string.
    ///   - activePaneID: The pane to focus, or invalid to keep the focus.
    void applyLayout(int windowID, std::string_view layout, PaneID activePaneID);

    /// Replays a pane's reported modes into its terminal.
    ///
    /// - Parameter body: The report after its prefix.
    void applyModeReport(std::string_view body);

    /// Feeds a completed capture into its pane and schedules a retry for a
    /// blank one.
    void finishCapture();

    /// Writes a completed paste-buffer pull through the named pane's own
    /// clipboard client, when its policy allows the write.
    void finishClipboardPull();

    /// Asks for a pane's modes, once, and its screen.
    ///
    /// - Parameters:
    ///   - paneID: The tmux pane.
    ///   - retry: How many blank captures came before this one.
    void requestStateRecovery(PaneID paneID, int retry);

    /// Asks for a pane's current screen.
    ///
    /// - Parameters:
    ///   - paneID: The tmux pane.
    ///   - retry: The retry count the marker carries.
    void requestCapture(PaneID paneID, int retry);

    /// Asks for a pane's whole history, which tmux rewrapped at a new width.
    ///
    /// - Parameter paneID: The tmux pane.
    void requestFullHistoryCapture(PaneID paneID);

    /// Fetches a pane's screen once a resize has settled, escalating to its
    /// history when any step changed the width.
    ///
    /// - Parameters:
    ///   - paneID: The tmux pane.
    ///   - scope: What this step needs fetched.
    void scheduleResync(PaneID paneID, CaptureScope scope);

    /// Records the geometry a capture is asked against, and reads the form it
    /// is asked in under the same hold.
    ///
    /// - Parameter paneID: The tmux pane.
    /// - Returns: The form the server's captures print.
    CaptureForm noteIssuedGeometry(PaneID paneID);

    /// The terminal a delivery run writes, pinned and locked for the run.
    ///
    /// - Parameter paneID: The tmux pane.
    /// - Returns: The pane's terminal, or the primary for an unknown pane.
    [[nodiscard]] Ubiquitty::Terminal* deliveryTarget(PaneID paneID);

    /// Ends a delivery run: flushes the pane's replies to tmux, releases its
    /// lock and pin, and marks it for a repaint.
    void releaseDelivery();

    /// Appends a change for the frontend and raises the session's bit.
    ///
    /// - Parameter event: The change.
    void queueEvent(HostedWindowEvent event);

    /// Marks a pane for the frontend's next repaint.
    ///
    /// - Parameter paneID: The tmux pane.
    void noteRepainted(PaneID paneID);

    enum class PaneChange {
        Output,
        Recovery,
        Geometry
    };
    struct PublicationRecord {
        std::shared_ptr<HostedPanePublication> publication;
        std::uint64_t timerGeneration = 0;
        bool hasTimer = false;
    };
    /// Lookup is a leaf lock; publication state itself belongs to the pane lock.
    std::shared_ptr<HostedPanePublication> publicationFor(PaneID, const Ubiquitty::Terminal&);
    [[nodiscard]] Ticks publicationTicks() const;
    void notePaneChange(PaneID, PaneChange);
    HostedPanePublication::Progress notePaneChangeLocked(HostedPanePublication&, Ubiquitty::Terminal&, PaneChange);
    void finishPublication(PaneID, const std::shared_ptr<HostedPanePublication>&, const HostedPanePublication::Progress&);
    void schedulePublication(PaneID, const std::shared_ptr<HostedPanePublication>&, std::uint64_t delayMicroseconds);
    void advancePublication(PaneID, const std::shared_ptr<HostedPanePublication>&);

    /// Frees every released pane tmux has retired and no run holds.
    void disposeReleasedPanes();

    HostedWindowSeams m_seams;
    PostedDispatcher m_dispatcher;
    // Posted work holds this weakly and does nothing once the host is gone.
    std::shared_ptr<TmuxControlHost*> m_lifetime;

    TmuxSession m_session;

    // Ingest-thread state: the parser's callbacks and the router own it.
    Protocol::Tmux::CaptureRecovery m_capture;
    Protocol::Tmux::ClipboardPullRecovery m_clipboardPull;
    std::string m_lastSyncedClipboard;
    TmuxPane* m_deliveryPane = nullptr;
    Ubiquitty::Terminal* m_deliveryTerminal = nullptr;
    PaneID m_deliveryPaneID = Protocol::Tmux::invalidPaneID;
    bool m_hasEnded = false;
    bool m_isAttached = false;

    // Shared between the ingest thread and the frontend loop, under
    // m_stateLock. A leaf: nothing takes a pane lock or the primary's lock
    // while holding it.
    std::mutex m_stateLock;
    std::deque<HostedWindowEvent> m_events;
    std::set<int> m_repaintedPanes;
    std::map<int, WindowRecord> m_windows;
    std::map<int, std::string> m_layouts;
    std::set<int> m_releasedPanes;
    std::map<int, IssuedGeometry> m_issuedGeometry;
    // The form this server's captures print, from its version reply.
    CaptureForm m_captureForm = CaptureForm::Text;
    std::map<int, PendingResync> m_pendingResyncs;
    std::map<int, int> m_paneColumns;
    std::set<int> m_knownPanes;

    // Publication lookup is a distinct leaf: never take state, structure, or
    // a model lock while holding it. Layout already orders structure → model.
    std::mutex m_publicationLock;
    std::map<int, PublicationRecord> m_publications;
};

/// Makes a tmux control host, the factory a session calls when its
/// foreground program is a tmux control-mode client.
///
/// - Parameter seams: The session's seams.
/// - Returns: The host, not yet attached.
[[nodiscard]] std::unique_ptr<HostedWindowSource> makeTmuxControlHost(const HostedWindowSeams& seams);

} // namespace Ubiquitty::Runtime
