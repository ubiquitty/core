// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HostedWindows.h — a session whose program hosts windows of its own.
//
// A terminal multiplexer running in control mode is a program that speaks for
// a set of windows and panes rather than drawing into the terminal it runs
// in. The session that runs it becomes a source of hosted windows: it tells
// the frontend a window appeared, what its panes' layout is, what it is
// called and when it went, and it hands out each pane's model for a view to
// adopt. The frontend shows those windows and routes input to their panes;
// what speaks the multiplexer's protocol is behind this interface, so no
// frontend type is named for it.
//
// Platform-free and header-only: a frontend built without a source links
// none, and the session that owns one reaches it only through this seam.

#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Runtime/PostedWorkTable.h"
#include "Core/Session/PaneLinkEvents.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Ubiquitty {
struct Terminal;
namespace PAL {
class ByteStream;
}
namespace Preferences {
struct Values;
}
}

namespace Ubiquitty::Runtime {

/// Identifies a hosted session separately from its reconnect authority.
struct HostedSessionReference {
    Protocol::Tmux::SessionReference tmux;
    bool isConfirmed = false;
    bool canReconnectLocally = false;
    /// The explicit SSH destination, never a name inferred from server output.
    std::string sshHost {};
};

/// One change a hosted-window source asks its frontend to show.
///
/// A record rather than a callback, for the reason the window-request queue
/// gives: a Swift frontend cannot subclass a C++ class with virtual methods,
/// so the source queues and the frontend drains.
struct HostedWindowEvent {
    /// What changed. Numbered, because the Swift arm reads the kind by raw
    /// value.
    enum class Kind : std::uint8_t {
        /// The queue is empty. Not an event.
        None UBIQUITTY_SWIFT_NAME(none) = 0,
        /// A window appeared: `windowID`, `name` and `sessionName` describe it.
        WindowOpened UBIQUITTY_SWIFT_NAME(windowOpened) = 1,
        /// A window's panes were laid out anew: `layout` is the multiplexer's
        /// own layout string, `paneIDs` its panes in mount order, and
        /// `activePaneID` the pane to focus, or negative to keep the focus.
        LayoutChanged UBIQUITTY_SWIFT_NAME(layoutChanged) = 2,
        /// A window was renamed to `name`.
        WindowRenamed UBIQUITTY_SWIFT_NAME(windowRenamed) = 3,
        /// The hosting session was renamed to `sessionName`.
        SessionRenamed UBIQUITTY_SWIFT_NAME(sessionRenamed) = 4,
        /// A window went. The frontend unbinds its panes, then answers with
        /// ``HostedWindowSource/releaseWindow``.
        WindowClosed UBIQUITTY_SWIFT_NAME(windowClosed) = 5,
        /// The program stopped hosting: every window goes.
        Ended UBIQUITTY_SWIFT_NAME(ended) = 6,
    };

    Kind kind = Kind::None;
    int windowID = -1;
    int activePaneID = -1;
    std::string name;
    std::string sessionName;
    std::string layout;
    std::vector<int> paneIDs;
};

/// What a frontend binds a hosted pane's view to.
///
/// Addresses rather than pointers, because the frontend on the far side of a
/// language seam adopts by address — the arrangement a render adapter's
/// adoption already rides. Valid while the pane is live; a frontend unbinds
/// on the layout or close that drops the pane.
struct HostedPaneFacts {
    /// The pane's model; zero when the pane is not live.
    std::uintptr_t terminalAddress = 0;
    /// The lock that model is read and written under.
    std::uintptr_t lockAddress = 0;
    /// The control client whose options decide how the pane's input wraps.
    std::uintptr_t inputClientAddress = 0;
    /// The channel the pane's input rides.
    std::uintptr_t inputStreamAddress = 0;
    /// The pane's grid, the source's authority.
    int columns = 0;
    int rows = 0;
};

/// Distinguishes a client awaiting authentication from an established control stream.
enum class HostedControlState {
    AwaitingHandshake,
    Established,
};

/// What a hosted-window source runs on: the session that owns it.
///
/// Function pointers with a context, so the source reaches the session's
/// wake, event word and frontend hop without naming the session's type.
struct HostedWindowSeams {
    /// The terminal the hosting program writes to.
    Ubiquitty::Terminal* primary = nullptr;
    /// That terminal's model lock, held by the ingest pass that parses it.
    std::mutex* primaryLock = nullptr;
    /// The channel commands to the program ride.
    PAL::ByteStream* stream = nullptr;
    /// The configuration the hosted panes' terminals are born under.
    const Preferences::Values* configuration = nullptr;
    /// Waits for a newly launched client to authenticate and enter control mode.
    HostedControlState initialControlState = HostedControlState::Established;
    /// Handed back to the two calls below.
    void* sessionContext = nullptr;
    /// Wakes the session's ingest loop so a queued write drains.
    void (*wakeIngest)(void*) = nullptr;
    /// Raises the session's hosted-windows event bit. The source changes
    /// what it hosts only inside an ingest pass, whose own report carries the
    /// bit to the frontend.
    void (*announce)(void*) = nullptr;
    /// Wakes presentation after a timer admits a frame without an ingest pass.
    void (*wakePresentation)(void*) = nullptr;
    /// The owner's full-width monotonic publication clock; null uses steady time.
    std::uint64_t (*clockMicroseconds)(void*) = nullptr;
    /// The owner's 60 Hz hold-budget clock; null derives it from steady time.
    Ticks (*clockTicks)(void*) = nullptr;
    /// The frontend loop the source posts its timers and retries to.
    PostedWorkLoop loop;
};

/// A session's program speaking for windows of its own.
///
/// ``attach`` and ``detach`` take the primary's model lock; every other call
/// is the frontend's, on its own loop.
class HostedWindowSource {
public:
    virtual ~HostedWindowSource() = default;

    /// Installs the source on the primary terminal and asks the program to
    /// state its windows.
    virtual void attach() = 0;

    /// Takes the source back out of the primary terminal, which returns to
    /// passthrough. Called before the source is destroyed, after the frontend
    /// has unbound every pane view.
    virtual void detach() = 0;

    /// Reads the hosting backend's confirmed live identity.
    ///
    /// - Returns: The identity, without assuming a local transport authority.
    [[nodiscard]] virtual HostedSessionReference sessionReference() = 0;

    /// The oldest change not yet shown, removed from the queue.
    ///
    /// - Returns: The event, whose kind is `None` when the queue is empty.
    [[nodiscard]] virtual HostedWindowEvent takeEvent() = 0;

    /// The panes whose models changed since the last call, each once.
    ///
    /// - Returns: The pane ids; empty when nothing changed.
    [[nodiscard]] virtual std::vector<int> takeRepaintedPanes() = 0;

    /// What a view binds a pane to.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: Its facts; zero addresses when it is not live.
    [[nodiscard]] virtual HostedPaneFacts paneFacts(int paneID) = 0;

    /// Whether this source supplies actual model-owner frame publications.
    [[nodiscard]] virtual bool hasPanePublications() const { return false; }

    /// Captures metadata while the caller holds the adopted model's own lock.
    /// The expected model prevents a reused pane id from borrowing another picture.
    [[nodiscard]] virtual std::optional<Session::FrameReady> panePublication(int, const Ubiquitty::Terminal&) { return std::nullopt; }

    [[nodiscard]] virtual std::uint64_t publicationClockMicroseconds() const { return 0; }

    /// Reports focus through the exact adopted model's owner. No model lock
    /// is held by the caller; false means the source cannot route this report.
    [[nodiscard]] virtual bool setPaneFocus(int, const Ubiquitty::Terminal&, Modes::FocusState) { return false; }

    /// Tells the source no view reads a pane's model any more, so a pane
    /// the program retired can be freed.
    ///
    /// - Parameter paneID: The pane whose view unbound.
    virtual void releasePane(int paneID) = 0;

    /// Answers a ``HostedWindowEvent/Kind/WindowClosed``: the window's pane views
    /// are unbound and its panes can go.
    ///
    /// - Parameter windowID: The window that closed.
    virtual void releaseWindow(int windowID) = 0;

    /// Resizes a pane's model to its view's grid now, ahead of the program
    /// confirming the size.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - columns: The view's columns.
    ///   - rows: The view's rows.
    virtual void resizePane(int paneID, int columns, int rows) = 0;

    /// Reports the grid a hosting window holds as the program's client size,
    /// and resizes the panes of the program's other windows to it now.
    ///
    /// - Parameters:
    ///   - paneID: A pane of the reporting window.
    ///   - columns: The window's columns.
    ///   - rows: The window's rows.
    /// - Returns: Whether the report was sent.
    virtual bool reportClientSize(int paneID, int columns, int rows) = 0;
};

/// Makes the source for a session whose foreground program asked to host
/// windows.
using HostedWindowSourceFactory = std::unique_ptr<HostedWindowSource> (*)(const HostedWindowSeams& seams);

/// The tmux control host's factory, as the address
/// ``TerminalSession::enableHostedWindows`` takes.
///
/// Declared here, beside the seam it feeds, and defined with the host in
/// TmuxControlHost.cpp: a frontend that links the host enables hosting with
/// it, and one that does not never calls it.
///
/// - Returns: The address of the factory that makes a tmux control host.
[[nodiscard]] std::uintptr_t tmuxControlHostFactoryAddress();

} // namespace Ubiquitty::Runtime
