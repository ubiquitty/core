// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/OSC/ClipboardPolicy.h"
#include "Core/OSC/SequenceGate.h"
#include "Core/Protocol/Tmux/PaneID.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/TermPalette.h"
#include "Core/Unicode/Width.h"
#include <memory>
#include <string>

namespace Ubiquitty::Protocol::Tmux {

// One pane: its Terminal (owned by the session), geometry in cells, and the lock
// guarding Ubiquitty::Terminal access — the deliver path is the sole writer, the renderer /
// serializer are readers. Non-copyable (owns a Terminal + lock).
class Pane {
public:
    /// The controlling session's live colors and program-reset baseline.
    struct PaletteSeed {
        TermPalette palette;
        TermPalette baseline;
    };

    /// Builds a pane and the terminal it owns.
    ///
    /// - Parameters:
    ///   - paneID: The tmux pane id.
    ///   - columns: The grid's columns; a non-positive value reads as 80.
    ///   - rows: The grid's rows; a non-positive value reads as 24.
    ///   - sequenceGate: The `[security]` per-OSC / per-DCS gate the terminal
    ///     parses under. The session supplies its own, so a pane is born
    ///     with the policy every other terminal of the window carries.
    ///   - osc52Policy: The `[security]` OSC 52 clipboard policy the terminal
    ///     writes and reads under, supplied by the session the same way.
    ///   - ambiguousWidth: The `[terminal]` ambiguous-width choice the terminal
    ///     measures East Asian Ambiguous text under, supplied the same way.
    ///   - paletteSeed: The controlling session colors, or null for terminal defaults.
    explicit Pane(PaneID paneID, int columns, int rows, const OSC::SequenceGate& sequenceGate, const OSC::OSC52Policy& osc52Policy,
        Unicode::Width::AmbiguousWidth ambiguousWidth, const PaletteSeed* paletteSeed = nullptr);
    ~Pane();
    Pane(const Pane&) = delete;
    Pane& operator=(const Pane&) = delete;

    PaneID paneID() const { return m_paneID; }
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    void setGeometry(int x, int y, int columns, int rows);
    int x() const { return m_x; }
    int y() const { return m_y; }

    // The Terminal. Touch only between lock()/unlock() (or via withTermLocked).
    Ubiquitty::Terminal* term() { return m_term.get(); }

    void lock() { m_lock.lock(); }
    void unlock() { m_lock.unlock(); }
    // The underlying mutex, so a frontend's render adapter adopts the same lock
    // the deliver path takes — one canonical lock per pane.
    std::mutex* lockHandle() { return m_lock.nativeHandle(); }

    // Run `body(term)` with the pane lock held, and only when this pane has a
    // terminal: every caller means "if it has none, there is nothing to do", and
    // handing out the empty pointer made each of them say so again. body must not
    // re-lock this pane.
    template<class F> void withTermLocked(F&& body)
    {
        m_lock.lock();
        struct Unlocker {
            PaneLock& l;
            ~Unlocker() { l.unlock(); }
        } guard { m_lock };
        if (Ubiquitty::Terminal* term = m_term.get())
            body(*term);
    }

    // A tmux pane has no drain loop, so the deliver path flags the published
    // mode-snapshot stale here; the renderer/serializer reads+clears it and
    // re-publishes. Lock-free (a plain flag is enough — at-least-once republish).
    void markSnapshotDue() { m_snapshotDue = true; }
    bool consumeSnapshotDue()
    {
        bool due = m_snapshotDue;
        m_snapshotDue = false;
        return due;
    }

    // Deferred-dispose guard. The deliver path pins a pane before taking its
    // lock for a %output run; takeRetired() leaves a pinned pane on the retired
    // list (a deliver run still holds it) and reclaims it once the run unpins.
    void pin() { m_pinned.store(true, std::memory_order_release); }
    void unpin() { m_pinned.store(false, std::memory_order_release); }
    bool pinned() const { return m_pinned.load(std::memory_order_acquire); }

private:
    PaneID m_paneID;
    int m_columns, m_rows;
    int m_x = 0, m_y = 0;
    std::unique_ptr<Ubiquitty::Terminal> m_term;
    PaneLock m_lock;
    bool m_snapshotDue = false;
    std::atomic<bool> m_pinned { false };
};

} // namespace Ubiquitty::Protocol::Tmux
