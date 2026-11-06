// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Tmux/LayoutNode.h"
#include "Core/Protocol/Tmux/Pane.h"
#include "Core/Protocol/Tmux/PaneID.h"
#include <memory>
#include <string>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

// One tmux window: an id, a name, its layout tree, and its panes.
class Window {
public:
    explicit Window(int windowID)
        : m_windowID(windowID)
    {
    }

    [[nodiscard]] int windowID() const { return m_windowID; }
    [[nodiscard]] const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] const LayoutNode& layout() const { return m_layout; }
    void setLayout(LayoutNode layout) { m_layout = std::move(layout); }

    [[nodiscard]] const std::vector<std::unique_ptr<Pane>>& panes() const { return m_panes; }
    Pane* pane(PaneID paneID);
    [[nodiscard]] PaneID activePaneID() const { return m_activePaneID; }
    Pane* activePane() { return pane(m_activePaneID); }
    void setActivePaneID(PaneID paneID) { m_activePaneID = paneID; }

    // Pane lifecycle (used by Session::applyLayout reconcile).
    Pane* addPane(PaneID paneID, int columns, int rows, const OSC::SequenceGate& sequenceGate, const OSC::OSC52Policy& osc52Policy,
        Unicode::Width::AmbiguousWidth ambiguousWidth, const Pane::PaletteSeed* paletteSeed = nullptr);
    void removePane(PaneID paneID);
    // Remove `paneID` from the window without disposing it — the caller owns
    // the returned pane (disposes now, or defers it if pinned). Null if absent.
    std::unique_ptr<Pane> extractPane(PaneID paneID);
    /// Adopts a pane moved from another window without replacing its terminal.
    ///
    /// - Parameter pane: The pane whose ownership transfers to this window.
    void adoptPane(std::unique_ptr<Pane> pane) { m_panes.push_back(std::move(pane)); }
    // Move every pane out (for window close), same ownership transfer.
    std::vector<std::unique_ptr<Pane>> extractAllPanes();

private:
    int m_windowID;
    std::string m_name;
    LayoutNode m_layout;
    std::vector<std::unique_ptr<Pane>> m_panes;
    PaneID m_activePaneID = invalidPaneID;
};

} // namespace Ubiquitty::Protocol::Tmux
