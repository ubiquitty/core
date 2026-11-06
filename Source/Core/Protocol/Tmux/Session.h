// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable model of a tmux -CC session: windows → panes, each pane owning a
// Ubiquitty::Terminal + a per-pane lock. The events→model layer (Tmux.cpp emits the
// control-mode events). Geometry is in CELLS; Session OWNS the Terms + locks and
// frontends attach as thin renderers. A tmux pane has no drain loop (its bytes arrive
// via the primary's %output deliver path), so modes are republished via the
// snapshot-due flag rather than on a drain.

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/StateRecovery.h"
#include "Core/TerminalOperations.h"
#include "PAL/Capability.h"
#include <span>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Protocol/Tmux/LayoutNode.h"
#include "Core/Protocol/Tmux/Pane.h"
#include "Core/Protocol/Tmux/PaneID.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Protocol/Tmux/Window.h"

namespace Ubiquitty::Protocol::Tmux {

// The session: owns the windows (and, transitively, the panes + Terms). The
// frontend reads windows()/panes() and renders; the deliver path calls
// deliverOutput / applyLayout / addWindow etc. from the control-mode events.
class Session {
public:
    Session() = default;
    ~Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Consumes an identity reply without retaining a malformed prior identity.
    ///
    /// - Parameter line: One control-mode response line.
    /// - Returns: Whether the line carries the identity marker.
    bool consumeSessionReference(std::string_view line);

    /// Returns the attached session's identity under the structural lock.
    ///
    /// - Returns: The confirmed identity, or no value until a valid reply arrives.
    [[nodiscard]] std::optional<SessionReference> sessionReference();

    /// Clears identity when the control connection enters or leaves a session.
    void clearSessionReference();

    /// Sets the `[security]` per-OSC / per-DCS gate every pane created from
    /// here on is born with.
    ///
    /// This module reads no config, so the frontend that attaches the session
    /// hands the gate in, and hands it in again on a reload. Panes already
    /// live keep their own terminal's gate; the frontend re-seeds those as it
    /// does every other live terminal.
    ///
    /// - Parameter sequenceGate: The gate, as the config describes it.
    void setPaneSequenceGate(const OSC::SequenceGate& sequenceGate);

    /// Sets the `[security]` OSC 52 clipboard policy every pane created from
    /// here on is born with, handed in the way the sequence gate is.
    ///
    /// - Parameter osc52Policy: The policy pair, as the config describes it.
    void setPaneOSC52Policy(const OSC::OSC52Policy& osc52Policy);

    /// Sets the `[terminal]` ambiguous-width choice every pane created from
    /// here on is born with, handed in the way the sequence gate is.
    ///
    /// A pane's terminal measures East Asian Ambiguous text under its own
    /// choice and nowhere else, so a pane born without this one measures it
    /// narrow whatever the config says.
    ///
    /// - Parameter ambiguousWidth: The choice, as the config describes it.
    void setPaneAmbiguousWidth(Unicode::Width::AmbiguousWidth ambiguousWidth);

    /// Sets the controlling session colors inherited by newly created panes.
    ///
    /// Existing panes retain their program-selected colors and reset baseline.
    ///
    /// - Parameters:
    ///   - palette: The controlling terminal's current colors.
    ///   - baseline: Its configured colors restored by OSC resets.
    void setPanePalette(const TermPalette& palette, const TermPalette& baseline);

    /// Sets the authority inherited by the session's pane terminals.
    ///
    /// - Parameter origin: The controlling connection's capability origin.
    void setPaneCapabilityOrigin(CapabilityOrigin origin);

    Window* addWindow(int windowID);
    void closeWindow(int windowID);
    void renameWindow(int windowID, std::string_view name);

    // Parse `layout` into a tree, then reconcile this window's panes/Terms to
    // match it (create new panes, drop vanished ones, update geometry).
    void applyLayout(int windowID, std::string_view layout);

    /// Routes a run of %output bytes to a pane's terminal, under that pane's lock.
    ///
    /// Flags a snapshot republish. A pane the session does not know is a no-op
    /// rather than an error: %output can name a pane that closed between the
    /// server sending it and this arriving.
    ///
    /// - Parameters:
    ///   - paneID: The pane the server addressed.
    ///   - bytes: The payload, already unescaped.
    void deliverOutput(PaneID paneID, std::span<const std::uint8_t> bytes);

    /// Writes a capture-pane recovery into a pane's terminal, atomically.
    ///
    /// The same locked delivery as ``deliverOutput``, with the recovery's
    /// extras under the same hold: a buffer clear first for a full-history
    /// screen (it re-scrolls from nothing, and stale local scrollback
    /// underneath it would sit below tmux's re-fetched truth), and a geometry
    /// guard (rows issued against one size, applied after a resize, were
    /// wrapped for a width the terminal no longer has).
    ///
    /// The rows are written as ``recoveryFeed`` composes them, and each row
    /// tmux wrapped is marked wrapped once the row after it is reached, as the
    /// terminal marks a wrap of its own, so a copy, a selection and a reflow
    /// join it to the next as they would the output that wrote it.
    ///
    /// tmux reports a wrap it deferred at the right edge as a cursor column
    /// past the last, which a cursor position cannot carry; such a cursor is
    /// left on the last column with its wrap deferred, so the pane's next
    /// character starts the next row.
    ///
    /// - Parameters:
    ///   - paneID: The pane the capture answered for.
    ///   - screen: The recovered rows and cursor.
    ///   - issuedColumns: The width the capture was issued against; 0 skips the guard.
    ///   - issuedRows: The height, likewise.
    /// - Returns: Whether the screen was applied (false: unknown pane or stale geometry).
    bool deliverRecovery(PaneID paneID, const RecoveredScreen& screen, int issuedColumns, int issuedRows);

    void setActivePane(int windowID, PaneID paneID);

    Window* window(int windowID);
    Pane* pane(PaneID paneID); // searches all windows
    [[nodiscard]] const std::vector<std::unique_ptr<Window>>& windows() const { return m_windows; }

    // Deliver-path entry (ingest thread): look up `paneID` and pin it under the
    // structural lock so a concurrent removal can't free it between lookup and
    // use. Null if gone. The caller takes the pane's own lock for the Terminal, then
    // unpin()s after the run and may takeRetired() on the owning thread.
    Pane* pinPane(PaneID paneID);

    /// One pane's render adoption facts, resolved under the structure lock.
    ///
    /// Addresses rather than pointers, because the caller on the far side of
    /// a language seam adopts by address — the arrangement the runtime
    /// session's adoption seam rides. The addresses stay valid while the
    /// pane is live; a frontend view binds them and unbinds on the layout
    /// pass that drops the pane.
    struct PaneRenderFacts {
        /// The pane's terminal's address; zero when the pane is not live.
        std::uintptr_t terminalAddress = 0;
        /// The pane's content lock's address, the one the deliver path takes.
        std::uintptr_t lockAddress = 0;
        /// The pane's grid columns — the server's resize authority.
        std::ptrdiff_t columns = 0;
        /// The pane's grid rows, likewise.
        std::ptrdiff_t rows = 0;
    };

    /// Resolves ``PaneRenderFacts`` for a pane, under the structure lock.
    ///
    /// - Parameter paneID: The tmux pane.
    /// - Returns: The facts; zero addresses when the pane is not live yet.
    [[nodiscard]] PaneRenderFacts paneRenderFacts(PaneID paneID);

    // The structural lock for the window/pane vectors (distinct from each
    // Pane's content lock). Order: m_mutex < PaneLock; never take a PaneLock
    // while holding m_mutex. Exposed so a frontend can bracket a multi-step
    // read (e.g. the layout reconcile) against concurrent deliver-path lookups.
    void lockStructure() { m_mutex.lock(); }
    void unlockStructure() { m_mutex.unlock(); }

    /// Disposes one retired pane, if present and unpinned.
    ///
    /// Call on the owning (render) thread the instant its frontend view has
    /// unbound from this pane's terminal — that unbind, not any layout or
    /// close timing, is what makes the terminal safe to free.
    ///
    /// - Parameter paneID: The pane whose view unbound.
    /// - Parameter context: The associated model owner's callback context.
    /// - Parameter willDispose: Runs off the structural lock, before the
    ///   unpinned model is freed; associated owner metadata can retire by its
    ///   exact live identity before an allocator can reuse the address.
    /// - Returns: Whether a pane was disposed; false when it is not retired
    ///   or a delivery run still holds it.
    bool disposeRetiredPane(PaneID paneID, void* context = nullptr, void (*willDispose)(void*, const Ubiquitty::Terminal&) = nullptr);

    // Retire every pane of every window (tmux %exit): each Ubiquitty::Terminal stays alive until
    // its frontend view unbinds and reclaims it via disposeRetiredPane. Leaves
    // the session window-less; the panes drain as their views tear down.
    void retireAll();

    // Drain every unpinned retired pane (a final sweep — e.g. teardown). The
    // returned unique_ptrs dispose on destruction; the caller must already have
    // unbound every frontend view. Prefer disposeRetiredPane() during normal
    // operation so disposal stays tied to each pane's unbind.
    std::vector<std::unique_ptr<Pane>> takeRetired();

private:
    // Unlocked cores (caller holds m_mutex) so the locked public methods don't
    // re-enter m_mutex (a std::mutex is not recursive).
    Window* windowLocked(int windowID);
    Window* addWindowLocked(int windowID);
    Pane* paneLocked(PaneID paneID);
    void applyLayoutLocked(int windowID, std::string_view layout);
    // Send a removed pane to disposal: immediately if unpinned (the common
    // path), else deferred to m_retired (drained by takeRetired) so the deliver
    // path never has its Ubiquitty::Terminal freed mid-run.
    void retireOrDispose(std::unique_ptr<Pane>);

    std::vector<std::unique_ptr<Window>> m_windows;
    std::optional<SessionReference> m_sessionReference;
    std::vector<std::unique_ptr<Pane>> m_retired; // pinned-at-removal, await main drain
    OSC::SequenceGate m_paneSequenceGate; // under m_mutex; copied into each new pane's terminal
    OSC::OSC52Policy m_paneOSC52Policy; // under m_mutex; installed on each new pane's terminal
    Unicode::Width::AmbiguousWidth m_paneAmbiguousWidth = Unicode::Width::AmbiguousWidth::Narrow; // under m_mutex; installed on each new pane's terminal
    std::optional<Pane::PaletteSeed> m_panePaletteSeed;
    CapabilityOrigin m_paneCapabilityOrigin = CapabilityOrigin::Local;
    PaneLock m_mutex; // structural lock over m_windows / each Window's panes
};

} // namespace Ubiquitty::Protocol::Tmux
