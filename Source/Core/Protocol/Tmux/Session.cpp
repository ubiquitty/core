// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Session.cpp — see Session.h.

#include "Core/Protocol/Tmux/Session.h"
#include "Core/Image/TerminalImage.h"
#include "Core/OSC/Dispatch.h"
#include <span>

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Tmux {

bool Session::consumeSessionReference(std::string_view line)
{
    if (!line.starts_with(sessionReferenceReplyPrefix)) {
        return false;
    }
    auto reference = parseSessionReference(line);
    const std::scoped_lock hold(m_mutex);
    m_sessionReference = std::move(reference);
    return true;
}

std::optional<SessionReference> Session::sessionReference()
{
    const std::scoped_lock hold(m_mutex);
    return m_sessionReference;
}

void Session::clearSessionReference()
{
    const std::scoped_lock hold(m_mutex);
    m_sessionReference.reset();
}

void Session::setPaneCapabilityOrigin(CapabilityOrigin origin)
{
    const std::scoped_lock hold(m_mutex);
    m_paneCapabilityOrigin = origin;
}

// ---- Pane ----

Pane::Pane(PaneID paneID, int columns, int rows, const OSC::SequenceGate& sequenceGate, const OSC::OSC52Policy& osc52Policy,
    Unicode::Width::AmbiguousWidth ambiguousWidth, const PaletteSeed* paletteSeed)
    : m_paneID(paneID)
    , m_columns(columns > 0 ? columns : 80)
    , m_rows(rows > 0 ? rows : 24)
{
    m_term = std::make_unique<Ubiquitty::Terminal>();
    if (paletteSeed) {
        m_term->palette = paletteSeed->palette;
        m_term->paletteBaseline = paletteSeed->baseline;
    }
    m_term->sequenceGate = sequenceGate;
    m_term->ambiguousWidth = ambiguousWidth;
    // The session's OSC 52 policy, installed at birth like every other pane
    // Terminal's. A fresh Terminal defaults ClipboardWritePolicy::Allow, so a
    // pane that skips this leaves clipboard_write = "deny" unenforced for
    // every program running inside it -- the config gates the visible
    // terminals and not the tmux ones, which is the least visible place for
    // an escape hatch to open.
    Ubiquitty::OSC::applyOSC52Policy(*m_term, osc52Policy);
    // tmux panes carry UTF-8; the legacy-encoding transcode stays off.
    Ubiquitty::resize(*m_term, m_rows, m_columns, Ubiquitty::Modes::ReflowMode::Fixed);
}

Pane::~Pane() = default;

void Pane::setGeometry(int x, int y, int columns, int rows)
{
    // Under the pane's lock throughout: the layout reconcile on the ingest
    // thread and a frontend's resize ahead of it both set the geometry, and
    // the record and the terminal move together.
    m_lock.lock();
    m_x = x;
    m_y = y;
    if (columns <= 0 || rows <= 0 || (columns == m_columns && rows == m_rows)) {
        m_lock.unlock();
        return;
    }
    const int oldColumns = m_columns;
    const int oldRows = m_rows;
    m_columns = columns;
    m_rows = rows;
    if (m_term) {
        Ubiquitty::resize(*m_term, rows, columns, Ubiquitty::Modes::ReflowMode::Fixed);
        // Modes::ReflowMode::Fixed skips Core's grow-blanking, but a tmux pane resync lands
        // right after this, so blank the newly-revealed band now — the window shows
        // blank, not stale bytes from an earlier state, during the round-trip.
        //
        // Written cell by cell rather than erased: an erase trusts the row's content
        // bound, and a Fixed shrink keeps cells past the width that an erase to the
        // edge at the narrower width lowers the bound past. Erased, the band would
        // keep a line the program cleared.
        if (columns > oldColumns || rows > oldRows) {
            Cell resizeBlank = defaultBlankCell;
            if (columns > oldColumns) {
                const int rEnd = std::min(oldRows, rows);
                for (int r = 0; r < rEnd; r++) {
                    m_term->grid->fillCellRun(Ubiquitty::Row(r), Ubiquitty::Column(oldColumns), resizeBlank, columns - oldColumns);
                    m_term->grid->eraseRowRangeInactive(Ubiquitty::Row(r), Ubiquitty::Column(oldColumns), Ubiquitty::Column(columns - 1), resizeBlank);
                }
            }
            if (rows > oldRows) {
                for (int r = oldRows; r < rows; r++) {
                    m_term->grid->fillCellRun(Ubiquitty::Row(r), Ubiquitty::Column(0), resizeBlank, columns);
                    m_term->grid->eraseRowRangeInactive(Ubiquitty::Row(r), Ubiquitty::Column(0), Ubiquitty::Column(columns - 1), resizeBlank);
                }
            }
        }
    }
    m_lock.unlock();
}

// ---- Window ----

Pane* Window::pane(PaneID paneID)
{
    for (auto& p : m_panes) {
        if (p && p->paneID() == paneID)
            return p.get();
    }
    return nullptr;
}

Pane* Window::addPane(PaneID paneID, int columns, int rows, const OSC::SequenceGate& sequenceGate, const OSC::OSC52Policy& osc52Policy,
    Unicode::Width::AmbiguousWidth ambiguousWidth, const Pane::PaletteSeed* paletteSeed)
{
    if (Pane* existing = pane(paneID))
        return existing;
    m_panes.push_back(std::make_unique<Pane>(paneID, columns, rows, sequenceGate, osc52Policy, ambiguousWidth, paletteSeed));
    return m_panes.back().get();
}

void Window::removePane(PaneID paneID)
{
    for (auto it = m_panes.begin(); it != m_panes.end(); ++it) {
        if (*it && (*it)->paneID() == paneID) {
            m_panes.erase(it);
            return;
        }
    }
}

std::unique_ptr<Pane> Window::extractPane(PaneID paneID)
{
    for (auto it = m_panes.begin(); it != m_panes.end(); ++it) {
        if (*it && (*it)->paneID() == paneID) {
            std::unique_ptr<Pane> p = std::move(*it);
            m_panes.erase(it);
            return p;
        }
    }
    return nullptr;
}

std::vector<std::unique_ptr<Pane>> Window::extractAllPanes() { return std::move(m_panes); }

// ---- Session ----

Window* Session::windowLocked(int windowID)
{
    for (auto& w : m_windows) {
        if (w && w->windowID() == windowID)
            return w.get();
    }
    return nullptr;
}

Window* Session::window(int windowID)
{
    m_mutex.lock();
    Window* w = windowLocked(windowID);
    m_mutex.unlock();
    return w;
}

Pane* Session::paneLocked(PaneID paneID)
{
    for (auto& w : m_windows) {
        if (Pane* p = w->pane(paneID))
            return p;
    }
    return nullptr;
}

Pane* Session::pane(PaneID paneID)
{
    m_mutex.lock();
    Pane* p = paneLocked(paneID);
    m_mutex.unlock();
    return p;
}

Pane* Session::pinPane(PaneID paneID)
{
    m_mutex.lock();
    Pane* p = paneLocked(paneID);
    if (p)
        p->pin(); // held against disposal until the caller unpin()s
    m_mutex.unlock();
    return p;
}

Session::PaneRenderFacts Session::paneRenderFacts(PaneID paneID)
{
    PaneRenderFacts facts;
    m_mutex.lock();
    if (Pane* p = paneLocked(paneID); p && p->term() && p->lockHandle()) {
        facts.terminalAddress = reinterpret_cast<std::uintptr_t>(p->term());
        facts.lockAddress = reinterpret_cast<std::uintptr_t>(p->lockHandle());
        facts.columns = p->columns();
        facts.rows = p->rows();
    }
    m_mutex.unlock();
    return facts;
}

Window* Session::addWindowLocked(int windowID)
{
    if (Window* existing = windowLocked(windowID))
        return existing;
    // Bound the portable model against a hostile server minting unlimited
    // distinct window IDs (%layout-change @1, @2, …). Past the cap the add is
    // refused, so every caller and every frontend is bounded by construction —
    // not just those that consult the platform registry gate first.
    if (static_cast<int>(m_windows.size()) >= tmuxMaxSessionWindows)
        return nullptr;
    m_windows.push_back(std::make_unique<Window>(windowID));
    return m_windows.back().get();
}

void Session::setPaneSequenceGate(const OSC::SequenceGate& sequenceGate)
{
    std::scoped_lock hold(m_mutex);
    m_paneSequenceGate = sequenceGate;
}

void Session::setPaneOSC52Policy(const OSC::OSC52Policy& osc52Policy)
{
    std::scoped_lock hold(m_mutex);
    m_paneOSC52Policy = osc52Policy;
}

void Session::setPaneAmbiguousWidth(Unicode::Width::AmbiguousWidth ambiguousWidth)
{
    std::scoped_lock hold(m_mutex);
    m_paneAmbiguousWidth = ambiguousWidth;
}

void Session::setPanePalette(const TermPalette& palette, const TermPalette& baseline)
{
    std::scoped_lock hold(m_mutex);
    m_panePaletteSeed = Pane::PaletteSeed { palette, baseline };
}

Window* Session::addWindow(int windowID)
{
    m_mutex.lock();
    Window* w = addWindowLocked(windowID);
    m_mutex.unlock();
    return w;
}

void Session::closeWindow(int windowID)
{
    m_mutex.lock();
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (*it && (*it)->windowID() == windowID) {
            for (auto& p : (*it)->extractAllPanes())
                retireOrDispose(std::move(p));
            m_windows.erase(it);
            break;
        }
    }
    m_mutex.unlock();
}

void Session::retireOrDispose(std::unique_ptr<Pane> pane)
{
    if (!pane)
        return;
    // Always defer: a removal runs on the ingest thread (mid-parse reconcile),
    // but the frontend render thread also reads this pane's Ubiquitty::Terminal until it
    // unbinds its view. So never dispose inline here — hand the pane to the
    // retired list and let the owner drain it via takeRetired() on its render
    // thread, after the view is unbound (and after any deliver run unpins).
    m_retired.push_back(std::move(pane));
}

void Session::retireAll()
{
    m_mutex.lock();
    m_sessionReference.reset();
    for (auto& w : m_windows) {
        if (!w)
            continue;
        for (auto& p : w->extractAllPanes())
            retireOrDispose(std::move(p));
    }
    m_windows.clear();
    m_mutex.unlock();
}

bool Session::disposeRetiredPane(PaneID paneID, void* context, void (*willDispose)(void*, const Ubiquitty::Terminal&))
{
    std::unique_ptr<Pane> victim;
    m_mutex.lock();
    for (auto it = m_retired.begin(); it != m_retired.end(); ++it) {
        if (*it && (*it)->paneID() == paneID) {
            if ((*it)->pinned())
                break; // a deliver run still holds it — a later call reclaims it
            victim = std::move(*it);
            m_retired.erase(it);
            break;
        }
    }
    m_mutex.unlock();
    if (victim && victim->term() && willDispose)
        willDispose(context, *victim->term());
    // victim disposes its Ubiquitty::Terminal here, off-lock, after the view has unbound.
    return victim != nullptr;
}

std::vector<std::unique_ptr<Pane>> Session::takeRetired()
{
    m_mutex.lock();
    std::vector<std::unique_ptr<Pane>> out;
    for (auto it = m_retired.begin(); it != m_retired.end();) {
        if (*it && (*it)->pinned()) {
            ++it; // still in a deliver run — keep deferring
            continue;
        }
        out.push_back(std::move(*it));
        it = m_retired.erase(it);
    }
    m_mutex.unlock();
    return out; // the moved-out panes dispose on the caller's destruction, off-lock
}

void Session::renameWindow(int windowID, std::string_view name)
{
    m_mutex.lock();
    if (Window* w = windowLocked(windowID))
        w->setName(std::string(name));
    m_mutex.unlock();
}

void Session::setActivePane(int windowID, PaneID paneID)
{
    m_mutex.lock();
    if (Window* w = windowLocked(windowID))
        w->setActivePaneID(paneID);
    m_mutex.unlock();
}

void Session::deliverOutput(PaneID paneID, std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return;
    Pane* p = pinPane(paneID); // pinned under m_mutex: safe to use after unlock
    if (!p || !p->term()) {
        if (p)
            p->unpin();
        return;
    }
    p->withTermLocked([&](Ubiquitty::Terminal& term) { Ubiquitty::processBytes(&term, std::string_view(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(bytes.size()))); });
    p->markSnapshotDue();
    p->unpin();
}

bool Session::deliverRecovery(PaneID paneID, const RecoveredScreen& screen, int issuedColumns, int issuedRows)
{
    Pane* p = pinPane(paneID);
    if (!p || !p->term()) {
        if (p)
            p->unpin();
        return false;
    }
    bool didApply = false;
    p->withTermLocked([&](Ubiquitty::Terminal& term) {
        if (issuedColumns != 0 && (issuedColumns != term.columns || issuedRows != term.rows))
            return; // stale: the invalidating resize queued its own resync
        // The capture is TEXT (capture-pane -e); it cannot carry an image. The
        // keeper carries the pane's images across the refeed — without it the
        // clear below and the row rewrites destroyed every placement on each
        // resync, which on screen was a sixel vanishing at the resync cadence.
        Ubiquitty::Image::ContentKeeper keeper(term);
        // A full-history feed re-scrolls from nothing; stale local scrollback
        // underneath it would sit below tmux's re-fetched truth.
        if (screen.scope == CaptureScope::FullHistory)
            Ubiquitty::clearBuffer(&term);
        const std::vector<std::string> segments = recoveryFeed(screen);
        for (std::size_t index = 0; index < segments.size(); ++index) {
            Ubiquitty::processBytes(&term, segments[index]);
            // Every segment but the last leaves the cursor on the row after
            // one tmux wrapped, which is the row above it even when the line
            // feed scrolled; a later scroll carries the mark into history.
            const int wrappedRow = term.screens.active->cursorY - 1;
            if (index + 1 < segments.size() && wrappedRow >= 0)
                term.screens.active->lineWrapped[wrappedRow] = true;
        }
        // The feed's cursor position clamps a cursor past the right edge to
        // the last column, where the next character would overwrite that
        // column; tmux has the wrap deferred there.
        if (screen.cursorX >= term.columns)
            Ubiquitty::Screen::deferWrapAtLastColumn(term);
        keeper.restoreInto(term);
        didApply = true;
    });
    if (didApply)
        p->markSnapshotDue();
    p->unpin();
    return didApply;
}

// Walk a parsed layout tree, collecting each leaf's (paneID, geometry) so
// applyLayout can reconcile panes against it.
static void collectLeaves(const LayoutNode& node, std::vector<const LayoutNode*>& out)
{
    if (node.isLeaf()) {
        out.push_back(&node);
        return;
    }
    for (const LayoutNode& child : node.children)
        collectLeaves(child, out);
}

void Session::applyLayout(int windowID, std::string_view layout)
{
    m_mutex.lock();
    applyLayoutLocked(windowID, layout);
    m_mutex.unlock();
}

void Session::applyLayoutLocked(int windowID, std::string_view layout)
{
    LayoutNode tree;
    if (!parseLayoutTree(layout, tree))
        return;
    Window* w = addWindowLocked(windowID);
    if (!w)
        return; // window cap reached — the layout for a new window is refused

    std::vector<const LayoutNode*> leaves;
    collectLeaves(tree, leaves);

    // Create/update panes present in the layout.
    for (const LayoutNode* leaf : leaves) {
        if (!w->pane(leaf->paneID)) {
            std::unique_ptr<Pane> transferred;
            for (auto& other : m_windows) {
                if (other.get() != w && (transferred = other->extractPane(leaf->paneID))) {
                    break;
                }
            }
            if (!transferred) {
                for (auto retired = m_retired.begin(); retired != m_retired.end(); ++retired) {
                    if ((*retired)->paneID() == leaf->paneID) {
                        transferred = std::move(*retired);
                        m_retired.erase(retired);
                        break;
                    }
                }
            }
            if (transferred) {
                w->adoptPane(std::move(transferred));
            }
        }
        Pane* p = w->addPane(leaf->paneID, leaf->width, leaf->height, m_paneSequenceGate, m_paneOSC52Policy, m_paneAmbiguousWidth, m_panePaletteSeed ? &*m_panePaletteSeed : nullptr);
        if (p) {
            p->withTermLocked([this](Terminal& terminal) {
                terminal.currentCapabilityOrigin = m_paneCapabilityOrigin;
            });
            p->setGeometry(leaf->x, leaf->y, leaf->width, leaf->height);
        }
    }
    // Drop panes no longer in the layout (a pane was closed / the window
    // collapsed). Collect first, then remove (don't mutate while iterating).
    std::vector<PaneID> stale;
    for (const auto& existing : w->panes()) {
        bool stillPresent = false;
        for (const LayoutNode* leaf : leaves) {
            if (leaf->paneID == existing->paneID()) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent)
            stale.push_back(existing->paneID());
    }
    for (PaneID paneID : stale)
        retireOrDispose(w->extractPane(paneID));

    w->setLayout(std::move(tree));
}

} // namespace Ubiquitty::Protocol::Tmux
