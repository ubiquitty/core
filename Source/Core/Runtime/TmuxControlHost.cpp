// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// TmuxControlHost.cpp — see TmuxControlHost.h.

#include "Core/Runtime/TmuxControlHost.h"

#include "Core/EffectGate.h"
#include "Core/Modes/Focus.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Protocol/Tmux/PaneReplies.h"
#include "Core/Protocol/Tmux/Tiler.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Protocol/Tmux/WindowRegistry.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <chrono>
#include <span>
#include <utility>

namespace Ubiquitty::Runtime {

using namespace Ubiquitty::Protocol::Tmux;

namespace {

// How long a live resize's stream of %layout-change events has to go quiet
// before each pane's screen is fetched again: long enough to fold a drag's
// steps into one query, short enough to read as immediate. The round trip over
// a local control socket measures well under a millisecond, so this wait is
// the whole of the perceived latency.
constexpr double resyncSettleSeconds = 0.025;

// How long a capture that came back blank waits before asking again: the
// pane's program had not painted its first frame when the first one ran.
constexpr double captureRetrySeconds = 0.25;

TmuxControlHost* hostFrom(void* context) { return static_cast<TmuxControlHost*>(context); }

} // namespace

TmuxControlHost::TmuxControlHost(const HostedWindowSeams& seams)
    : m_seams(seams)
    , m_lifetime(std::make_shared<TmuxControlHost*>(this))
{
    if (seams.loop.isComplete())
        m_dispatcher = PostedDispatcher(seams.loop);
}

TmuxControlHost::~TmuxControlHost()
{
    m_lifetime.reset();
    // A pane a view still adopts is the frontend's to release first; what is
    // left here no view reads.
    (void)m_session.takeRetired();
}

void TmuxControlHost::attach()
{
    if (m_isAttached || !m_seams.primary || !m_seams.primaryLock)
        return;
    Ubiquitty::Terminal& primary = *m_seams.primary;
    {
        // The parser reads these hooks on the ingest thread under this lock,
        // so they are installed under it: a pass in flight sees all of them
        // or none.
        const std::scoped_lock hold(*m_seams.primaryLock);
        // The Session mints each pane's terminal and reads no config; the
        // policies a pane is born with are the session's.
        if (m_seams.configuration) {
            const Preferences::Values& configuration = *m_seams.configuration;
            m_session.setPaneSequenceGate(sequenceGateFromPreferences(configuration));
            m_session.setPaneOSC52Policy(osc52PolicyFromPreferences(configuration));
            m_session.setPaneAmbiguousWidth(Unicode::Width::ambiguousWidthFromCells(Preferences::Effective::ambiguousWidth(configuration)));
        }
        m_session.setPanePalette(primary.palette, primary.paletteBaseline);
        m_session.setPaneCapabilityOrigin(primary.currentCapabilityOrigin);
        configure(&primary.integration.tmux, IntegrationMode::Enabled, AutoDetectMode::Automatic);
        setEnterControlCallback(&primary.integration.tmux, &TmuxControlHost::onEnterControlMode, this);
        TmuxEventClient client;
        client.context = this;
        client.windowAdd = &TmuxControlHost::onWindowAdd;
        client.windowClose = &TmuxControlHost::onWindowClose;
        client.windowRenamed = &TmuxControlHost::onWindowRenamed;
        client.sessionRenamed = &TmuxControlHost::onSessionRenamed;
        client.sessionChanged = &TmuxControlHost::onSessionChanged;
        client.exit = &TmuxControlHost::onExit;
        client.commandResponseLine = &TmuxControlHost::onCommandResponseLine;
        client.commandResponseEnd = &TmuxControlHost::onCommandResponseEnd;
        client.layoutChange = &TmuxControlHost::onLayoutChange;
        client.paneDidPause = &TmuxControlHost::onPaneDidPause;
        client.paneDidChangeMode = &TmuxControlHost::onPaneDidChangeMode;
        setEventClient(&primary.integration.tmux, client);
        primary.integration.paneRouter.lookup = &TmuxControlHost::routePane;
        primary.integration.paneRouter.flush = &TmuxControlHost::endDeliveryPass;
        primary.integration.paneRouter.context = this;
        m_isAttached = true;
    }
    // Prompts tmux to emit its control stream again: the handshake that opened
    // it passed the parser before the integration was listening, and the
    // `%begin` this answer carries is what the detector enters control mode
    // on.
    if (m_seams.initialControlState == HostedControlState::Established) {
        sendCommand("refresh-client");
    }
}

void TmuxControlHost::detach()
{
    if (!m_isAttached || !m_seams.primary || !m_seams.primaryLock)
        return;
    Ubiquitty::Terminal& primary = *m_seams.primary;
    // Held, no ingest pass is in flight, and every pass releases its delivery
    // run before it lets go of this lock: no pane lock is held here to leak.
    const std::scoped_lock hold(*m_seams.primaryLock);
    setEventClient(&primary.integration.tmux, TmuxEventClient {});
    setEnterControlCallback(&primary.integration.tmux, nullptr, nullptr);
    configure(&primary.integration.tmux, IntegrationMode::Disabled, AutoDetectMode::Manual);
    primary.integration.paneRouter = PaneRouter {};
    invalidateTmuxPaneRoute(&primary);
    m_isAttached = false;
    m_session.clearSessionReference();
}

HostedWindowEvent TmuxControlHost::takeEvent()
{
    const std::scoped_lock hold(m_stateLock);
    if (m_events.empty())
        return {};
    HostedWindowEvent event = std::move(m_events.front());
    m_events.pop_front();
    return event;
}

std::vector<int> TmuxControlHost::takeRepaintedPanes()
{
    const std::scoped_lock hold(m_stateLock);
    std::vector<int> panes(m_repaintedPanes.begin(), m_repaintedPanes.end());
    m_repaintedPanes.clear();
    return panes;
}

HostedPaneFacts TmuxControlHost::paneFacts(int paneID)
{
    HostedPaneFacts facts;
    const std::scoped_lock hold(m_stateLock);
    const TmuxSession::PaneRenderFacts render = m_session.paneRenderFacts(paneID);
    if (render.terminalAddress == 0 || !m_seams.primary)
        return facts;
    m_releasedPanes.erase(paneID);
    facts.terminalAddress = render.terminalAddress;
    facts.lockAddress = render.lockAddress;
    facts.inputClientAddress = reinterpret_cast<std::uintptr_t>(&m_seams.primary->integration.tmux);
    facts.inputStreamAddress = reinterpret_cast<std::uintptr_t>(m_seams.stream);
    facts.columns = static_cast<int>(render.columns);
    facts.rows = static_cast<int>(render.rows);
    return facts;
}

std::uint64_t TmuxControlHost::publicationClockMicroseconds() const
{
    if (m_seams.clockMicroseconds)
        return m_seams.clockMicroseconds(m_seams.sessionContext);
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

Ticks TmuxControlHost::publicationTicks() const
{
    if (m_seams.clockTicks)
        return m_seams.clockTicks(m_seams.sessionContext);
    const auto now = publicationClockMicroseconds();
    return Ticks::fromTimespec(now / 1'000'000, (now % 1'000'000) * 1'000);
}

std::shared_ptr<HostedPanePublication> TmuxControlHost::publicationFor(PaneID paneID, const Ubiquitty::Terminal& terminal)
{
    const std::scoped_lock hold(m_publicationLock);
    auto& record = m_publications[paneID];
    if (!record.publication || !record.publication->isFor(terminal)) {
        record.publication = std::make_shared<HostedPanePublication>(terminal);
        ++record.timerGeneration;
        record.hasTimer = false;
    }
    return record.publication;
}

std::optional<Ubiquitty::Session::FrameReady> TmuxControlHost::panePublication(int paneID, const Ubiquitty::Terminal& terminal)
{
    std::shared_ptr<HostedPanePublication> publication;
    {
        const std::scoped_lock hold(m_publicationLock);
        const auto found = m_publications.find(paneID);
        if (found != m_publications.end())
            publication = found->second.publication;
    }
    // The caller holds this exact model's lock, not the unused adapter runtime's.
    return publication && publication->isFor(terminal) ? publication->lastFrame() : std::nullopt;
}

HostedPanePublication::Progress TmuxControlHost::notePaneChangeLocked(HostedPanePublication& publication, Ubiquitty::Terminal& terminal, PaneChange change)
{
    const auto now = publicationClockMicroseconds();
    switch (change) {
    case PaneChange::Output:
        publication.noteOutput(terminal, now);
        break;
    case PaneChange::Recovery:
        publication.noteRecovery(now);
        break;
    case PaneChange::Geometry:
        publication.noteGeometry(terminal, now);
        break;
    }
    return publication.advance(terminal, now, publicationTicks());
}

void TmuxControlHost::notePaneChange(PaneID paneID, PaneChange change)
{
    TmuxPane* pane = m_session.pinPane(paneID);
    if (!pane)
        return;
    std::shared_ptr<HostedPanePublication> publication;
    HostedPanePublication::Progress progress;
    pane->withTermLocked([&](Ubiquitty::Terminal& terminal) {
        publication = publicationFor(paneID, terminal);
        progress = notePaneChangeLocked(*publication, terminal, change);
    });
    pane->unpin();
    if (publication)
        finishPublication(paneID, publication, progress);
}

void TmuxControlHost::finishPublication(PaneID paneID, const std::shared_ptr<HostedPanePublication>& publication, const HostedPanePublication::Progress& progress)
{
    if (progress.hasPublished)
        noteRepainted(paneID);
    if (progress.askAfterMicroseconds)
        schedulePublication(paneID, publication, *progress.askAfterMicroseconds);
}

void TmuxControlHost::schedulePublication(PaneID paneID, const std::shared_ptr<HostedPanePublication>& publication, std::uint64_t delayMicroseconds)
{
    if (!m_seams.loop.isComplete())
        return;
    std::uint64_t generation = 0;
    {
        const std::scoped_lock hold(m_publicationLock);
        const auto found = m_publications.find(paneID);
        if (found == m_publications.end() || found->second.publication != publication || found->second.hasTimer)
            return;
        found->second.hasTimer = true;
        generation = ++found->second.timerGeneration;
    }
    std::weak_ptr<TmuxControlHost*> lifetime = m_lifetime;
    m_dispatcher.postDelayed(static_cast<double>(delayMicroseconds) / 1'000'000, [lifetime, paneID, publication, generation] {
        const auto owner = lifetime.lock();
        if (!owner)
            return;
        TmuxControlHost* host = *owner;
        {
            const std::scoped_lock hold(host->m_publicationLock);
            const auto found = host->m_publications.find(paneID);
            if (found == host->m_publications.end() || found->second.publication != publication || found->second.timerGeneration != generation)
                return;
            found->second.hasTimer = false;
        }
        host->advancePublication(paneID, publication);
    });
}

void TmuxControlHost::advancePublication(PaneID paneID, const std::shared_ptr<HostedPanePublication>& publication)
{
    TmuxPane* pane = m_session.pinPane(paneID);
    if (!pane)
        return;
    HostedPanePublication::Progress progress;
    pane->withTermLocked([&](Ubiquitty::Terminal& terminal) {
        if (publication->isFor(terminal))
            progress = publication->advance(terminal, publicationClockMicroseconds(), publicationTicks());
    });
    pane->unpin();
    finishPublication(paneID, publication, progress);
    if (progress.hasPublished && m_seams.wakePresentation)
        m_seams.wakePresentation(m_seams.sessionContext);
}

void TmuxControlHost::releasePane(int paneID)
{
    {
        const std::scoped_lock hold(m_stateLock);
        m_releasedPanes.insert(paneID);
    }
    disposeReleasedPanes();
}

void TmuxControlHost::releaseWindow(int windowID)
{
    {
        const std::scoped_lock hold(m_stateLock);
        m_windows.erase(windowID);
        m_layouts.erase(windowID);
    }
    // Retires the window's panes; each is freed once its view has released it
    // and no delivery run holds it.
    m_session.closeWindow(windowID);
    disposeReleasedPanes();
}

bool TmuxControlHost::setPaneFocus(int paneID, const Ubiquitty::Terminal& expected, Modes::FocusState focus)
{
    if (!m_seams.primary || !m_seams.primaryLock)
        return false;
    bool isOwned = false;
    bool shouldWake = false;
    std::shared_ptr<HostedPanePublication> publication;
    HostedPanePublication::Progress progress;
    {
        // The primary owns the control client's reply-encoding options. Keep
        // the established ingest order: primary, structure pin, pane, leaf
        // publication registry; no state lock is acquired from the pane.
        const std::scoped_lock primaryHold(*m_seams.primaryLock);
        TmuxPane* pane = m_session.pinPane(paneID);
        if (!pane)
            return false;
        pane->withTermLocked([&](Ubiquitty::Terminal& terminal) {
            if (&terminal != &expected)
                return;
            isOwned = true;
            auto owner = publicationFor(paneID, terminal);
            if (!owner->changeFocus(focus))
                return;
            publication = std::move(owner);
            Modes::focusChanged(&terminal, focus);
            if (focus == Modes::FocusState::Focused)
                (void)Ubiquitty::markAttentionSeen(terminal);
            if (m_seams.stream) {
                flushPaneReplies(terminal, &m_seams.primary->integration.tmux, paneID, *m_seams.stream);
                shouldWake = m_seams.stream->hasPendingWrite();
            }
            pane->markSnapshotDue();
            progress = notePaneChangeLocked(*publication, terminal, PaneChange::Recovery);
        });
        pane->unpin();
    }
    if (shouldWake && m_seams.wakeIngest)
        m_seams.wakeIngest(m_seams.sessionContext);
    if (publication)
        finishPublication(paneID, publication, progress);
    return isOwned;
}

void TmuxControlHost::resizePane(int paneID, int columns, int rows)
{
    if (columns < 1 || rows < 1)
        return;
    TmuxPane* pane = m_session.pinPane(paneID);
    if (!pane)
        return;
    // Geometry only, as the layout reconcile does it: tmux owns the screen and
    // repaints it at the new size, so a local rewrap would only move rows it
    // is about to overwrite. When the server's layout arrives at the same size
    // the reconcile finds nothing to do.
    pane->setGeometry(pane->x(), pane->y(), columns, rows);
    pane->unpin();
    notePaneChange(paneID, PaneChange::Geometry);
}

bool TmuxControlHost::reportClientSize(int paneID, int columns, int rows)
{
    if (columns < 1 || rows < 1 || !m_seams.stream)
        return false;
    sendCommand(refreshClientSize(columns, rows));
    // Every window of the session takes the client's size, and only the one
    // reporting has a view resizing its panes. The others are sized here, in
    // proportion, so a tab selected before the server's layouts arrive shows
    // its panes at the size the server is about to confirm.
    std::vector<LayoutNode> siblings;
    m_session.lockStructure();
    for (const auto& window : m_session.windows()) {
        if (window && !window->pane(paneID))
            siblings.push_back(window->layout());
    }
    m_session.unlockStructure();
    for (LayoutNode& layout : siblings) {
        if (layout.width < 1 || layout.height < 1 || !resizeLayout(layout, columns, rows))
            continue;
        for (const PaneSlot& slot : layoutSlots(layout)) {
            TmuxPane* pane = m_session.pinPane(slot.paneID);
            if (!pane)
                continue;
            pane->setGeometry(slot.x, slot.y, slot.columns, slot.rows);
            pane->unpin();
            notePaneChange(slot.paneID, PaneChange::Geometry);
        }
    }
    return true;
}

// ---- the event client ----

void TmuxControlHost::onEnterControlMode(void* context)
{
    TmuxControlHost* host = hostFrom(context);
    if (!host || !host->m_seams.primary)
        return;
    // A fresh attach drops any capture a prior one stranded, so its rows
    // cannot bleed into this one's.
    host->m_capture.reset();
    host->m_clipboardPull.reset();
    host->m_session.clearSessionReference();
    {
        const std::scoped_lock hold(host->m_stateLock);
        host->m_captureForm = CaptureForm::Text;
    }
    // Background windows must reflow too, but no command may reach an SSH
    // authentication prompt before the control-mode handshake.
    host->sendCommand(enableAggressiveResize());
    host->sendCommand(sessionReferenceQuery());
    // Answered before the pane listing below, so the form is known by the
    // time the first listed pane asks for its screen.
    host->sendCommand(serverVersionQuery());
    host->sendCommand(refreshClientSize(host->m_seams.primary->columns, host->m_seams.primary->rows));
    // The attached session's panes only: every other session's would open
    // windows for sessions nobody attached to.
    host->sendCommand(listPanesForSession());
}

void TmuxControlHost::onWindowAdd(void* context, int windowID)
{
    // The notification carries no pane; the listing it asks for does, and
    // its rows open the window.
    hostFrom(context)->sendCommand(listPanesForWindow(windowID));
}

void TmuxControlHost::onWindowClose(void* context, int windowID)
{
    TmuxControlHost* host = hostFrom(context);
    {
        const std::scoped_lock hold(host->m_stateLock);
        auto window = host->m_windows.find(windowID);
        if (window == host->m_windows.end() || !window->second.isOpened)
            return;
    }
    // The session keeps the window's panes until the frontend has unbound
    // their views and answers with releaseWindow: a view reads a pane's
    // terminal until then.
    HostedWindowEvent event;
    event.kind = HostedWindowEvent::Kind::WindowClosed;
    event.windowID = windowID;
    host->queueEvent(std::move(event));
}

void TmuxControlHost::onWindowRenamed(void* context, int windowID, std::string_view name)
{
    HostedWindowEvent event;
    event.kind = HostedWindowEvent::Kind::WindowRenamed;
    event.windowID = windowID;
    event.name = std::string(name);
    hostFrom(context)->queueEvent(std::move(event));
}

void TmuxControlHost::onSessionRenamed(void* context, std::string_view name)
{
    HostedWindowEvent event;
    event.kind = HostedWindowEvent::Kind::SessionRenamed;
    event.sessionName = std::string(name);
    hostFrom(context)->queueEvent(std::move(event));
}

void TmuxControlHost::onSessionChanged(void* context)
{
    TmuxControlHost* host = hostFrom(context);
    host->m_session.clearSessionReference();
    host->sendCommand(sessionReferenceQuery());
}

void TmuxControlHost::onExit(void* context)
{
    TmuxControlHost* host = hostFrom(context);
    host->releaseDelivery();
    host->m_hasEnded = true;
    host->m_session.clearSessionReference();
    HostedWindowEvent event;
    event.kind = HostedWindowEvent::Kind::Ended;
    host->queueEvent(std::move(event));
}

void TmuxControlHost::onCommandResponseLine(void* context, std::string_view line)
{
    TmuxControlHost* host = hostFrom(context);
    if (line.starts_with(paneModeReplyPrefix)) {
        host->applyModeReport(line.substr(paneModeReplyPrefix.size()));
        return;
    }
    // The parser reuses its line buffer, so only the view's own length bounds
    // this line; both machines take the view whole.
    if (host->m_clipboardPull.consumeLine(line))
        return;
    if (host->m_capture.consumeLine(line))
        return;
    if (host->m_session.consumeSessionReference(line)) {
        return;
    }
    if (line.starts_with(serverVersionReplyPrefix)) {
        const CaptureForm form = captureFormForVersion(line.substr(serverVersionReplyPrefix.size()));
        const std::scoped_lock hold(host->m_stateLock);
        host->m_captureForm = form;
        return;
    }
    int windowID = 0;
    PaneID paneID = invalidPaneID;
    int width = 0;
    int height = 0;
    int active = 0;
    std::string layout;
    std::string sessionName;
    std::string windowName;
    if (!parsePaneRecord(line, windowID, paneID, width, height, active, layout, sessionName, windowName))
        return;
    {
        const std::scoped_lock hold(host->m_stateLock);
        // Every row carries its window's whole layout, so a window split
        // before the attach is built at once: tmux does not repeat a
        // %layout-change for it.
        if (!layout.empty())
            host->m_layouts[windowID] = layout;
        if (active)
            host->m_windows[windowID].activePaneID = paneID;
    }
    host->discoverPane(windowID, paneID, sessionName, windowName);
}

void TmuxControlHost::onCommandResponseEnd(void* context)
{
    TmuxControlHost* host = hostFrom(context);
    // Both machines advance on the protocol's block boundary, never on body
    // text, so screen content that looks like a marker cannot end one.
    host->finishCapture();
    host->finishClipboardPull();
}

void TmuxControlHost::onLayoutChange(void* context, int windowID, std::string_view layout)
{
    TmuxControlHost* host = hostFrom(context);
    {
        const std::scoped_lock hold(host->m_stateLock);
        host->m_layouts[windowID] = std::string(layout);
        auto window = host->m_windows.find(windowID);
        // A window not opened yet takes this layout from the cache when its
        // first pane is listed.
        if (window == host->m_windows.end() || !window->second.isOpened)
            return;
    }
    host->applyLayout(windowID, layout, invalidPaneID);
}

void TmuxControlHost::onPaneDidPause(void* context, PaneID paneID)
{
    // This client does no flow control of its own, so a paused pane is
    // resumed at once or it stays frozen.
    hostFrom(context)->sendCommand(continuePane(paneID));
}

void TmuxControlHost::onPaneDidChangeMode(void* context, PaneID paneID)
{
    if (paneID < 0)
        return;
    TmuxControlHost* host = hostFrom(context);
    // The notification carries no value, so the modes are asked for again.
    host->sendCommand(paneModeQuery(paneID));
    // A copy-mode copy leaves copy mode, which is this same notification: the
    // pane's paste buffer is pulled, and an unchanged buffer writes nothing.
    host->sendCommands({ clipboardPullMarker(paneID), clipboardPullShowBuffer() });
}

Ubiquitty::Terminal* TmuxControlHost::routePane(void* context, PaneID paneID)
{
    TmuxControlHost* host = hostFrom(context);
    // Output that arrives after the session ended has nowhere to go; the
    // parser drops a null target.
    if (host->m_hasEnded)
        return nullptr;
    return host->deliveryTarget(paneID);
}

void TmuxControlHost::endDeliveryPass(void* context) { hostFrom(context)->releaseDelivery(); }

// ---- delivery ----

Ubiquitty::Terminal* TmuxControlHost::deliveryTarget(PaneID paneID)
{
    if (m_deliveryPane && m_deliveryPaneID == paneID && m_deliveryTerminal)
        return m_deliveryTerminal;
    releaseDelivery();
    // Pinned under the session's structure lock, atomically with the lookup,
    // so a removal on another thread defers the pane's disposal; then locked
    // with the lock its views read under, so the write neither tears the
    // model nor outlives it.
    TmuxPane* pane = m_session.pinPane(paneID);
    if (pane && pane->term()) {
        pane->lock();
        m_deliveryPane = pane;
        m_deliveryTerminal = pane->term();
        m_deliveryPaneID = paneID;
        return m_deliveryTerminal;
    }
    if (pane)
        pane->unpin();
    // A pane the session does not model yet: the primary, which the parser
    // drops as a self-delivery.
    return m_seams.primary;
}

void TmuxControlHost::releaseDelivery()
{
    if (!m_deliveryPane)
        return;
    Ubiquitty::settlePasswordPrompt(*m_deliveryTerminal, nullptr);
    // The route memo points at the pane about to be unpinned; it dies first,
    // so a later byte of this pass looks the pane up, and pins it, again.
    invalidateTmuxPaneRoute(m_seams.primary);
    // Replies the pane's program queued — a background colour, a cursor
    // report — go back to tmux as that pane's input, the way keystrokes do.
    // Without this a query from a pane is never answered.
    if (m_seams.stream && m_seams.primary && m_deliveryTerminal) {
        flushPaneReplies(*m_deliveryTerminal, &m_seams.primary->integration.tmux, m_deliveryPaneID, *m_seams.stream);
        if (m_seams.stream->hasPendingWrite() && m_seams.wakeIngest)
            m_seams.wakeIngest(m_seams.sessionContext);
    }
    TmuxPane* pane = m_deliveryPane;
    const PaneID paneID = m_deliveryPaneID;
    const auto publication = publicationFor(paneID, *m_deliveryTerminal);
    const auto progress = notePaneChangeLocked(*publication, *m_deliveryTerminal, PaneChange::Output);
    pane->markSnapshotDue();
    m_deliveryPane = nullptr;
    m_deliveryTerminal = nullptr;
    m_deliveryPaneID = invalidPaneID;
    pane->unlock();
    pane->unpin();
    finishPublication(paneID, publication, progress);
    disposeReleasedPanes();
}

// ---- windows and panes ----

void TmuxControlHost::discoverPane(int windowID, PaneID paneID, const std::string& sessionName, const std::string& windowName)
{
    if (paneID < 0)
        return;
    std::string layout;
    PaneID activePaneID = invalidPaneID;
    {
        const std::scoped_lock hold(m_stateLock);
        WindowRecord& window = m_windows[windowID];
        // A later pane of a window already opened arrives through its layout.
        if (window.isOpened)
            return;
        // A hostile or broken server listing thousands of panes would have a
        // window built for each; past the cap they are ignored.
        if (static_cast<int>(m_knownPanes.size()) >= maxTmuxPanes) {
            return;
        }
        window.isOpened = true;
        activePaneID = window.activePaneID >= 0 ? window.activePaneID : paneID;
        if (auto cached = m_layouts.find(windowID); cached != m_layouts.end())
            layout = cached->second;
    }
    HostedWindowEvent opened;
    opened.kind = HostedWindowEvent::Kind::WindowOpened;
    opened.windowID = windowID;
    opened.name = windowName;
    opened.sessionName = sessionName;
    queueEvent(std::move(opened));
    if (!layout.empty())
        applyLayout(windowID, layout, activePaneID);
}

void TmuxControlHost::applyLayout(int windowID, std::string_view layout, PaneID activePaneID)
{
    LayoutNode root;
    if (!parseLayoutTree(layout, root))
        return;
    // The delivery run may hold a pane of this window, and the reconcile
    // resizes panes under their own locks: it is released first, and the next
    // %output byte takes it again.
    releaseDelivery();
    const std::vector<PaneSlot> slots = layoutSlots(root);
    std::vector<PaneID> missingModels;
    for (const auto& slot : slots) {
        if (!m_session.paneRenderFacts(slot.paneID).terminalAddress) {
            missingModels.push_back(slot.paneID);
        }
    }
    m_session.applyLayout(windowID, layout);
    for (const auto& slot : slots)
        notePaneChange(slot.paneID, PaneChange::Geometry);
    HostedWindowEvent event;
    event.kind = HostedWindowEvent::Kind::LayoutChanged;
    event.windowID = windowID;
    event.activePaneID = activePaneID;
    event.layout = std::string(layout);
    std::vector<PaneID> discovered;
    std::vector<std::pair<PaneID, CaptureScope>> resyncs;
    {
        const std::scoped_lock hold(m_stateLock);
        for (const PaneSlot& slot : slots) {
            event.paneIDs.push_back(slot.paneID);
            const bool isNewPane = m_knownPanes.insert(slot.paneID).second;
            if (isNewPane || std::ranges::find(missingModels, slot.paneID) != missingModels.end()) {
                // New to this client: tmux %output carries only new output,
                // so its screen as it stands is fetched once.
                discovered.push_back(slot.paneID);
                m_paneColumns[slot.paneID] = slot.columns;
                continue;
            }
            // tmux reflows a pane on the server but never repaints the result
            // over %output, so every known pane is fetched again once the
            // layout settles: its whole history after a width change, which
            // rewraps all of it, and the screen alone after a height change.
            const auto columns = m_paneColumns.find(slot.paneID);
            const bool didWidthChange = columns != m_paneColumns.end() && columns->second != slot.columns;
            m_paneColumns[slot.paneID] = slot.columns;
            resyncs.push_back({ slot.paneID, didWidthChange ? CaptureScope::FullHistory : CaptureScope::CurrentScreen });
        }
    }
    queueEvent(std::move(event));
    for (PaneID paneID : discovered)
        requestStateRecovery(paneID, 0);
    for (const auto& [paneID, scope] : resyncs)
        scheduleResync(paneID, scope);
}

void TmuxControlHost::applyModeReport(std::string_view body)
{
    PaneID paneID = invalidPaneID;
    const std::string sequence = decodePaneModeReport(body, paneID);
    if (sequence.empty() || paneID < 0)
        return;
    // The modes a pane's program set before this client attached arrive as
    // the DECSETs that set them. The delivery lock is released first: this
    // runs inside a pass that may hold this very pane.
    releaseDelivery();
    m_session.deliverOutput(paneID, { reinterpret_cast<const std::uint8_t*>(sequence.data()), sequence.size() });
    notePaneChange(paneID, PaneChange::Output);
}

// ---- screen recovery ----

void TmuxControlHost::requestStateRecovery(PaneID paneID, int retry)
{
    if (paneID < 0)
        return;
    // Modes do not change while a blank capture waits for the program's first
    // frame, so they are asked for once.
    if (retry == 0)
        sendCommand(paneModeQuery(paneID));
    requestCapture(paneID, retry);
}

void TmuxControlHost::requestCapture(PaneID paneID, int retry)
{
    const CaptureForm form = noteIssuedGeometry(paneID);
    // The marker names the pane, the cursor to restore, the retry count and
    // the form; the capture-pane block right after it is the screen.
    sendCommands({ captureBeginMarker(paneID, retry, CaptureScope::CurrentScreen, form), capturePane(paneID, form) });
}

void TmuxControlHost::requestFullHistoryCapture(PaneID paneID)
{
    const CaptureForm form = noteIssuedGeometry(paneID);
    sendCommands({ captureBeginMarker(paneID, 0, CaptureScope::FullHistory, form), capturePaneWithHistory(paneID, form) });
}

CaptureForm TmuxControlHost::noteIssuedGeometry(PaneID paneID)
{
    const TmuxSession::PaneRenderFacts facts = m_session.paneRenderFacts(paneID);
    const std::scoped_lock hold(m_stateLock);
    m_issuedGeometry[paneID] = { static_cast<int>(facts.columns), static_cast<int>(facts.rows) };
    return m_captureForm;
}

void TmuxControlHost::scheduleResync(PaneID paneID, CaptureScope scope)
{
    std::uint64_t generation = 0;
    {
        const std::scoped_lock hold(m_stateLock);
        PendingResync& pending = m_pendingResyncs[paneID];
        generation = ++pending.generation;
        // Escalates within one settle window and never downgrades: a drag's
        // stream interleaves a width change with height-only steps.
        if (scope == CaptureScope::FullHistory)
            pending.scope = scope;
    }
    if (!m_seams.loop.isComplete())
        return;
    std::weak_ptr<TmuxControlHost*> lifetime = m_lifetime;
    m_dispatcher.postDelayed(resyncSettleSeconds, [lifetime, paneID, generation] {
        const std::shared_ptr<TmuxControlHost*> held = lifetime.lock();
        if (!held)
            return;
        TmuxControlHost* host = *held;
        CaptureScope firedScope = CaptureScope::CurrentScreen;
        {
            const std::scoped_lock hold(host->m_stateLock);
            auto pending = host->m_pendingResyncs.find(paneID);
            // A later resize superseded this one before it fired.
            if (pending == host->m_pendingResyncs.end() || pending->second.generation != generation)
                return;
            firedScope = pending->second.scope;
            pending->second.scope = CaptureScope::CurrentScreen;
        }
        if (firedScope == CaptureScope::FullHistory)
            host->requestFullHistoryCapture(paneID);
        else
            host->requestCapture(paneID, 0);
    });
}

void TmuxControlHost::finishCapture()
{
    CaptureOutcome outcome = m_capture.blockEnded();
    if (!outcome.didComplete)
        return;
    IssuedGeometry issued;
    {
        const std::scoped_lock hold(m_stateLock);
        if (auto found = m_issuedGeometry.find(outcome.paneID); found != m_issuedGeometry.end()) {
            issued = found->second;
            m_issuedGeometry.erase(found);
        }
    }
    // The screen is written under the pane's lock with the geometry guard and
    // the full-history clear held together; the delivery run is released
    // first, since it may hold that lock on this thread.
    releaseDelivery();
    const bool didApply = m_session.deliverRecovery(outcome.paneID, outcome.screen, issued.columns, issued.rows);
    if (didApply)
        notePaneChange(outcome.paneID, PaneChange::Recovery);
    // A full-history fetch never retries: it has no first frame to wait for,
    // and a pane that is truly blank would loop.
    if (!outcome.shouldRetry || outcome.screen.scope == CaptureScope::FullHistory || !m_seams.loop.isComplete())
        return;
    std::weak_ptr<TmuxControlHost*> lifetime = m_lifetime;
    const PaneID paneID = outcome.paneID;
    const int nextRetry = outcome.nextRetry;
    m_dispatcher.postDelayed(captureRetrySeconds, [lifetime, paneID, nextRetry] {
        if (const std::shared_ptr<TmuxControlHost*> held = lifetime.lock())
            (*held)->requestStateRecovery(paneID, nextRetry);
    });
}

void TmuxControlHost::finishClipboardPull()
{
    ClipboardPullOutcome outcome = m_clipboardPull.blockEnded();
    if (!outcome.didComplete)
        return;
    // The pull follows a server-sent notification, not a copy the reader was
    // seen to make, and the pane id is the server's word: a pane that does
    // not resolve refuses the write.
    releaseDelivery();
    TmuxPane* pane = outcome.paneID >= 0 ? m_session.pinPane(outcome.paneID) : nullptr;
    const PaneAttribution attribution = pane && pane->term() ? PaneAttribution::Resolved : PaneAttribution::Unresolved;
    ClipboardWriteGate gate = ClipboardWriteGate::Refused;
    bool isWriteAllowed = false;
    Ubiquitty::ClipboardClient* client = nullptr;
    if (attribution == PaneAttribution::Resolved) {
        pane->lock();
        Ubiquitty::Terminal& terminal = *pane->term();
        // The pane's own OSC 52 write policy and capability gate, judged at
        // the pane's own origin, and its own clipboard client: the write is
        // the pane's, and a pane no view has bound has no client to make it.
        isWriteAllowed = OSC::osc52WriteAllowed(terminal);
        if (effectAllowed(terminal, Capability::ClipboardWrite))
            gate = ClipboardWriteGate::Allowed;
        client = terminal.clipboardClient.get();
        const bool shouldWrite = client && shouldSyncClipboardPull(outcome.text, m_lastSyncedClipboard, isWriteAllowed)
            && clipboardPullShouldWrite(attribution, gate);
        if (shouldWrite) {
            // Recorded before the write, so the echo of this write cannot
            // trigger another.
            m_lastSyncedClipboard = outcome.text;
            client->writeClipboard(ClipboardBuffer::Clipboard, { outcome.text.data(), outcome.text.size() });
        }
        pane->unlock();
    }
    if (pane)
        pane->unpin();
}

// ---- plumbing ----

void TmuxControlHost::sendCommand(std::string_view command)
{
    sendCommands({ command });
}

void TmuxControlHost::sendCommands(std::initializer_list<std::string_view> commands)
{
    if (!m_seams.stream)
        return;
    std::string lines;
    for (const std::string_view command : commands) {
        if (command.empty())
            continue;
        lines.append(command);
        if (lines.back() != '\n')
            lines.push_back('\n');
    }
    if (lines.empty())
        return;
    (void)m_seams.stream->writeAll({ lines.data(), lines.size() });
    // A tail the channel could not take waits in its queue until the ingest
    // loop drains it, and the loop wakes only for input — which, on a quiet
    // channel, is this command's own reply.
    if (m_seams.stream->hasPendingWrite() && m_seams.wakeIngest)
        m_seams.wakeIngest(m_seams.sessionContext);
}

void TmuxControlHost::queueEvent(HostedWindowEvent event)
{
    {
        const std::scoped_lock hold(m_stateLock);
        m_events.push_back(std::move(event));
    }
    if (m_seams.announce)
        m_seams.announce(m_seams.sessionContext);
}

void TmuxControlHost::noteRepainted(PaneID paneID)
{
    if (paneID < 0)
        return;
    {
        const std::scoped_lock hold(m_stateLock);
        m_repaintedPanes.insert(paneID);
    }
    if (m_seams.announce)
        m_seams.announce(m_seams.sessionContext);
}

void TmuxControlHost::disposeReleasedPanes()
{
    std::vector<int> released;
    {
        const std::scoped_lock hold(m_stateLock);
        released.assign(m_releasedPanes.begin(), m_releasedPanes.end());
    }
    for (int paneID : released) {
        {
            const std::scoped_lock hold(m_stateLock);
            const auto forgetPublication = [](void* context, const Ubiquitty::Terminal& terminal) {
                auto* host = static_cast<TmuxControlHost*>(context);
                const std::scoped_lock publicationHold(host->m_publicationLock);
                std::erase_if(host->m_publications, [&](const auto& entry) { return entry.second.publication && entry.second.publication->isFor(terminal); });
            };
            if (!m_releasedPanes.contains(paneID) || !m_session.disposeRetiredPane(paneID, this, forgetPublication))
                continue;
            m_releasedPanes.erase(paneID);
            m_knownPanes.erase(paneID);
            m_paneColumns.erase(paneID);
            m_pendingResyncs.erase(paneID);
            m_issuedGeometry.erase(paneID);
        }
    }
}

std::unique_ptr<HostedWindowSource> makeTmuxControlHost(const HostedWindowSeams& seams)
{
    return std::make_unique<TmuxControlHost>(seams);
}

std::uintptr_t tmuxControlHostFactoryAddress()
{
    const HostedWindowSourceFactory factory = &makeTmuxControlHost;
    return reinterpret_cast<std::uintptr_t>(factory);
}

} // namespace Ubiquitty::Runtime
