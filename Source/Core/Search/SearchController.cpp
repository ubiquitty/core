// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SearchController — see SearchController.h.

#include "Core/Search/SearchController.h"
#include "Core/Search/SearchExecutor.h"

#include "Core/CellGrid.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/ViewPlacement.h"
#include "Core/Terminal.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Search {

namespace {

// The newest rows whose facts the start slice copies, so a search of recent
// history needs no second lock.
constexpr std::size_t startFacts = 16384;

// History hits wait for a publish until this many have gathered, or the time
// below has passed since the last.
constexpr std::size_t publishBatch = 512;
constexpr std::chrono::milliseconds publishInterval { 8 };

std::uint64_t retainedHistoryFront(const Terminal& terminal)
{
    if (!terminal.lineStore)
        return 0;
    const auto front = terminal.lineStore->frontLine();
    return front.number + (front.continuity == LineStore::LineContinuity::ContinuesLine ? 1 : 0);
}

std::optional<Screen::LogicalPosition> recapturePosition(const Terminal& terminal, const Screen::LogicalPosition& position)
{
    const auto place = Screen::resolvePosition(terminal, position);
    if (!place)
        return std::nullopt;
    const auto captured = Screen::capturePosition(terminal, place->absoluteRow, place->column, position.subCell);
    return captured ? std::optional(*captured) : std::nullopt;
}

Hit recaptureHit(const Terminal& terminal, const Hit& hit)
{
    const auto position = recapturePosition(terminal, hit.start);
    if (!position)
        return hit;
    return { .start = *position, .endCell = position->cell + (hit.endCell - hit.start.cell) };
}

// Whether two hits name one match: the same place in the same line, at any
// erase revision of that line. A program that repaints a line in place erases
// it and writes the text back, which keeps the line's name and moves only its
// revision, and the rescan finds the match there again. The focus that stood
// on it is the one the person chose, so neither the reveal nor the pill's
// arrival is owed for it.
bool isSameMatch(const Hit& left, const Hit& right)
{
    Screen::LogicalPosition start = left.start;
    start.revision = right.start.revision;
    return start == right.start && left.endCell == right.endCell;
}

} // namespace

std::shared_ptr<SearchController> SearchController::make(Terminal& terminal, std::mutex& modelLock)
{
    return make(terminal, modelLock, SearchExecutor::shared());
}

std::shared_ptr<SearchController> SearchController::make(Terminal& terminal, std::mutex& modelLock, SearchExecutor& executor)
{
    return std::make_shared<SearchController>(terminal, modelLock, executor.wakeSignal());
}

SearchController::SearchController(Terminal& terminal, std::mutex& modelLock, std::shared_ptr<std::atomic<std::uint64_t>> wake)
    : m_terminal(&terminal)
    , m_modelLock(&modelLock)
    , m_wakeSignal(std::move(wake))
{
}

void SearchController::setChangedCallback(ChangedCallback callback, void* context)
{
    const std::scoped_lock job(m_jobMutex);
    m_changed = callback;
    m_changedContext = context;
}

bool SearchController::setQuery(const Request& request)
{
    const std::scoped_lock hold(m_requestMutex);
    if (m_generation.load(std::memory_order_relaxed) != 0 && request == m_request)
        return false;
    m_request = request;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

Status SearchController::status() const
{
    const std::scoped_lock hold(m_statusMutex);
    return m_status;
}

void SearchController::detach()
{
    const std::scoped_lock job(m_jobMutex);
    m_isAttached.store(false, std::memory_order_release);
    m_changed = nullptr;
    m_changedContext = nullptr;
    m_terminal = nullptr;
    m_modelLock = nullptr;
    LineStore::releaseSnapshot(m_snapshot);
    m_phase = Phase::Done;
}

bool SearchController::hasWork() const
{
    return m_isAttached.load(std::memory_order_acquire)
        && (m_generation.load(std::memory_order_acquire) != m_consumedGeneration.load(std::memory_order_acquire)
            || m_hasJobWork.load(std::memory_order_acquire)
            || m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire)
            || (m_isWatchingModel.load(std::memory_order_acquire) && m_hasModelChange.load(std::memory_order_acquire)));
}

void SearchController::noteModelChanged(const Terminal& terminal)
{
    if (!m_isWatchingModel.load(std::memory_order_relaxed) || m_hasModelChange.load(std::memory_order_relaxed))
        return;
    const ScreenStamp stamp = ScreenStamp::read(terminal);
    if (stamp == m_observedStamp)
        return;
    m_observedStamp = stamp;
    if (!m_hasModelChange.exchange(true, std::memory_order_acq_rel) && m_wakeSignal) {
        m_wakeSignal->fetch_add(1, std::memory_order_release);
        m_wakeSignal->notify_one();
    }
}

bool SearchController::runSlice()
{
    bool hasMore = false;
    {
        const std::scoped_lock job(m_jobMutex);
        if (!m_terminal)
            return false;
        m_hasJobWork.store(true, std::memory_order_release);
        const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
        const Status before = status();
        if (generation != m_jobGeneration)
            beginJob(generation);
        if (m_isRefilling && m_refillJobSequence != m_refillSequence.load(std::memory_order_acquire))
            cancelRefill();
        if (!m_isRefilling && m_phase != Phase::Start && m_isWatchingModel.load(std::memory_order_relaxed)
            && (m_isScreenActive || ((m_phase == Phase::Done || !m_didScanScreen) && m_hasModelChange.load(std::memory_order_acquire)))) {
            scanScreen();
            m_didScanScreen = true;
        } else
            switch (m_phase) {
            case Phase::Start:
                startJob();
                break;
            case Phase::CaptureExtension:
                startExtension();
                break;
            case Phase::History:
                scanHistory();
                m_didScanScreen = false;
                break;
            case Phase::Deferred:
                scanDeferred();
                break;
            case Phase::LocateRefillPlan:
                locateRefillPlan();
                break;
            case Phase::LocateRefillScreen:
                locateRefillScreen();
                break;
            case Phase::FinishRefillLocation:
                finishRefillLocation();
                break;
            case Phase::PlanRefill:
                planRefill();
                break;
            case Phase::CaptureRefill:
                captureRefill();
                break;
            case Phase::RefillScreen:
                scanRefillScreen();
                break;
            case Phase::RefillHistory:
                scanRefillHistory();
                break;
            case Phase::RefillDeferred:
                scanRefillDeferred();
                break;
            case Phase::PublishRefill:
                publishRefill();
                break;
            case Phase::Idle:
                break;
            case Phase::Done:
                if (m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire))
                    startRefill();
                break;
            }
        if (status() != before)
            notifyChanged();
        m_hasJobWork.store(m_isScreenActive || (m_phase != Phase::Done && m_phase != Phase::Idle), std::memory_order_release);
        m_consumedGeneration.store(m_jobGeneration, std::memory_order_release);
        hasMore = hasWork();
    }
    return hasMore;
}

void SearchController::beginJob(std::uint64_t generation)
{
    m_jobGeneration = generation;
    {
        const std::scoped_lock hold(m_requestMutex);
        m_jobRequest = m_request;
    }
    LineStore::releaseSnapshot(m_snapshot);
    m_pending.clear();
    m_pendingLines.clear();
    m_historyCounts.clear();
    m_isRefilling = false;
    m_refillHistoryHits.clear();
    m_refillScreenHits.clear();
    m_isScreenActive = false;
    m_hasModelChange.store(false, std::memory_order_release);
    {
        const std::scoped_lock model(*m_modelLock);
        m_preservedActive.reset();
        m_refillAnchor.reset();
        m_windowStart = 0;
        m_windowBoundary.reset();
        m_windowBoundaryLine.reset();
        m_windowNewerHistory = 0;
        m_windowSkippedInLine = 0;
        m_consumedRefillSequence.store(m_refillSequence.load(std::memory_order_acquire), std::memory_order_release);
    }
    m_compiled = {};
    std::string error;
    std::optional<Screen::ScrollSearch::CompiledRegex> compiled;
    if (!m_jobRequest.pattern.empty()) {
        const std::string pattern = m_jobRequest.form == PatternForm::Literal ? Screen::ScrollSearch::escapeLiteral(m_jobRequest.pattern) : m_jobRequest.pattern;
        compiled = Screen::ScrollSearch::compilePattern(pattern, m_jobRequest.sensitivity, m_jobRequest.normalization, error);
    }
    if (compiled) {
        m_compiled = std::move(*compiled);
        m_form = Screen::ScrollSearch::matchingNormalizationForm(m_jobRequest.normalization);
        m_phase = Phase::Start;
        m_isWatchingModel.store(true, std::memory_order_release);
        return;
    }
    // An empty pattern clears the results; one that does not compile clears
    // them and says so.
    const std::scoped_lock model(*m_modelLock);
    if (m_generation.load(std::memory_order_acquire) != generation)
        return;
    m_hits.clear();
    m_screenLines.clear();
    m_labelLimit = 0;
    m_foundCount = 0;
    m_screenCount = 0;
    m_activeIndex = -1;
    ++m_activeEpoch;
    m_publishedGeneration = generation;
    m_isPatternInvalid = !m_jobRequest.pattern.empty();
    m_isWatchingModel.store(false, std::memory_order_release);
    recordStatus(Completion::Complete);
    m_phase = Phase::Done;
}

void SearchController::startJob()
{
    std::unique_lock model(*m_modelLock);
    if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
        return;
    const Terminal& terminal = *m_terminal;
    if (m_publishedGeneration == m_jobGeneration && m_activeIndex >= 0 && static_cast<std::size_t>(m_activeIndex) < m_hits.size())
        m_preservedActive = recaptureHit(terminal, m_hits[static_cast<std::size_t>(m_activeIndex)]);
    m_hits.clear();
    m_screenLines.clear();
    m_labelLimit = std::min(m_jobRequest.labelLimit, maxStoredLabels);
    m_foundCount = 0;
    m_screenCount = 0;
    m_historyCounts.clear();
    m_windowStart = 0;
    m_windowBoundary.reset();
    m_windowBoundaryLine.reset();
    m_windowNewerHistory = 0;
    m_windowSkippedInLine = 0;
    m_historyCoveredHead = 0;
    m_deferredIndex = 0;
    m_activeIndex = -1;
    if (!m_preservedActive)
        ++m_activeEpoch;
    m_isPatternInvalid = false;
    m_publishedGeneration = m_jobGeneration;
    // The history is the primary buffer's; the alternate screen has none.
    bool hasHistory = false;
    if (!terminal.screens.isUsingAlternateScreen && terminal.lineStore && terminal.lineStore->count() > 0) {
        if (!m_snapshot.spareLock)
            m_snapshot.spareLock = PAL::makePriorityInheritingLock();
        hasHistory = terminal.lineStore->captureSnapshot(m_snapshot, startFacts);
        if (hasHistory)
            m_historyScan.begin(m_snapshot, m_compiled, m_form);
    }
    m_screenScan.begin(terminal, m_jobRequest.scope);
    m_historyScanEnd = m_screenScan.stamp().historyHead;
    m_observedStamp = m_screenScan.stamp();
    m_hasModelChange.store(false, std::memory_order_release);
    m_isScreenActive = true;
    m_lastPublish = std::chrono::steady_clock::now();
    if (hasHistory) {
        recordStatus(Completion::Incomplete);
        m_phase = Phase::History;
    } else {
        m_historyCoveredHead = m_historyScanEnd;
        recordStatus(Completion::Incomplete);
        m_phase = Phase::Done;
    }
    model.unlock();
    prepareHistoryCounts();
}

void SearchController::prepareHistoryCounts()
{
    std::uint64_t first = 0;
    std::uint64_t end = 0;
    {
        const std::scoped_lock model(*m_modelLock);
        const ScreenStamp stamp = ScreenStamp::read(*m_terminal);
        if (stamp.buffer != m_screenScan.stamp().buffer || stamp.historyEpoch != m_screenScan.stamp().historyEpoch)
            return;
        if (!m_terminal->screens.isUsingAlternateScreen && m_terminal->lineStore) {
            first = retainedHistoryFront(*m_terminal);
            end = std::max(first, m_screenScan.stamp().historyHead);
        }
    }
    m_historyCounts.prepare(first, end);
}

void SearchController::startExtension()
{
    if (!m_snapshot.spareLock)
        m_snapshot.spareLock = PAL::makePriorityInheritingLock();
    bool hasHistory = false;
    {
        const std::scoped_lock model(*m_modelLock);
        if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
            return;
        const ScreenStamp stamp = ScreenStamp::read(*m_terminal);
        const ScreenStamp& captured = m_screenScan.stamp();
        if (stamp.buffer != captured.buffer || stamp.historyEpoch != captured.historyEpoch || stamp.historyHead < captured.historyHead) {
            restartJob();
            return;
        }
        m_historyScanEnd = captured.historyHead;
        if (m_terminal->lineStore && stamp.buffer == Screen::Buffer::Primary) {
            const std::uint64_t first = std::max(m_historyCoveredHead, retainedHistoryFront(*m_terminal));
            if (first < m_historyScanEnd) {
                hasHistory = m_terminal->lineStore->captureSnapshot(m_snapshot, startFacts, LineStore::SnapshotRange { first, m_historyScanEnd });
                if (!hasHistory) {
                    restartJob();
                    return;
                }
            }
        }
        if (hasHistory) {
            m_historyScan.begin(m_snapshot, m_compiled, m_form);
            m_deferredIndex = 0;
            m_phase = Phase::History;
        } else {
            m_historyCoveredHead = m_historyScanEnd;
            m_phase = Phase::Done;
        }
    }
    prepareHistoryCounts();
    if (!hasHistory)
        publishPending(Completion::Complete);
}

void SearchController::startRefill()
{
    std::int64_t target = 0;
    std::optional<std::uint64_t> anchorLine;
    {
        const std::scoped_lock model(*m_modelLock);
        if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
            return;
        m_refillJobSequence = m_refillSequence.load(std::memory_order_acquire);
        if (!m_foundCount) {
            m_refillAnchor.reset();
            m_consumedRefillSequence.store(m_refillJobSequence, std::memory_order_release);
            recordStatus(Completion::Complete);
            return;
        }
        const Terminal& terminal = *m_terminal;
        if (ScreenStamp::read(terminal) != m_screenScan.stamp()) {
            m_hasModelChange.store(true, std::memory_order_release);
            return;
        }
        target = m_refillRequestedOrdinal;
        m_refillTotalCount = m_foundCount;
        m_refillScreenCount = static_cast<std::uint64_t>(m_screenCount);
        m_refillJobDirection = m_refillRequestedDirection;
        m_refillJobShift = m_refillAnchorShift;
        m_refillJobAnchor.reset();
        m_refillLocatedOrdinal.reset();
        if (m_refillAnchor) {
            target = 0;
            if (m_refillAnchor->start.buffer == m_screenScan.stamp().buffer && Screen::resolvePosition(terminal, m_refillAnchor->start)) {
                m_refillJobAnchor = recaptureHit(terminal, *m_refillAnchor);
                const auto number = m_refillJobAnchor->start.buffer == Screen::Buffer::Primary
                    ? Screen::lineNumberOf(terminal, m_refillJobAnchor->start.line)
                    : std::nullopt;
                if (number && *number < m_screenScan.stamp().historyHead) {
                    anchorLine = number;
                    const auto position = Screen::historyPosition(terminal, *number, 0);
                    m_refillAnchorLineCell = position ? position->cell : 0;
                }
            }
        }
        m_isRefilling = true;
        m_isLocatingAnchor = m_refillJobAnchor.has_value();
    }
    m_refillHistoryHits.clear();
    m_refillHistoryHits.reserve(maxStoredHits);
    m_refillScreenHits.clear();
    m_refillCollectingLine.reset();
    m_refillPublishCursor = 0;
    if (anchorLine) {
        m_refillPlan.beginLine(m_historyCounts, *anchorLine, m_refillScreenCount);
        m_phase = Phase::LocateRefillPlan;
    } else if (m_refillJobAnchor) {
        m_screenScan.locate(*m_refillJobAnchor);
        m_phase = Phase::LocateRefillScreen;
    } else
        planRefillWindow(target);
}

void SearchController::locateRefillPlan()
{
    if (!m_refillPlan.step(publishBatch))
        m_phase = Phase::CaptureRefill;
}

void SearchController::locateRefillScreen()
{
    if (m_screenScan.step(m_compiled, m_form))
        return;
    m_refillLocatedOrdinal = m_screenScan.locatedOrdinal();
    m_phase = Phase::FinishRefillLocation;
}

void SearchController::finishRefillLocation()
{
    LineStore::releaseSnapshot(m_snapshot);
    m_isLocatingAnchor = false;
    m_refillCollectingLine.reset();
    const auto ordinal = static_cast<std::int64_t>(m_refillLocatedOrdinal.value_or(0));
    planRefillWindow(m_refillLocatedOrdinal ? ordinal + m_refillJobShift : 0);
}

void SearchController::planRefillWindow(std::int64_t target)
{
    target = (target % m_refillTotalCount + m_refillTotalCount) % m_refillTotalCount;
    m_refillTargetOrdinal = static_cast<std::uint64_t>(target);
    m_refillFirst = m_refillJobDirection == StepDirection::Older ? m_refillTargetOrdinal
                                                                 : static_cast<std::uint64_t>(std::max<std::int64_t>(0, target - static_cast<std::int64_t>(maxStoredHits) + 1));
    m_refillEnd = std::min(m_refillFirst + maxStoredHits, static_cast<std::uint64_t>(m_refillTotalCount));
    m_refillPlan.begin(m_historyCounts, m_refillFirst, m_refillEnd, m_refillScreenCount);
    m_phase = Phase::PlanRefill;
}

void SearchController::planRefill()
{
    if (m_refillPlan.step(publishBatch))
        return;
    if (m_refillFirst < m_refillScreenCount) {
        m_screenScan.replay(m_refillFirst, std::min(m_refillEnd, m_refillScreenCount));
        m_phase = Phase::RefillScreen;
    } else
        m_phase = Phase::CaptureRefill;
}

void SearchController::scanRefillScreen()
{
    if (m_screenScan.step(m_compiled, m_form))
        return;
    m_refillScreenHits.assign(m_screenScan.hits().begin(), m_screenScan.hits().end());
    m_phase = Phase::CaptureRefill;
}

void SearchController::captureRefill()
{
    const auto lines = m_refillPlan.lines();
    if (lines.empty()) {
        m_phase = m_isLocatingAnchor ? Phase::FinishRefillLocation : Phase::PublishRefill;
        return;
    }
    if (!m_snapshot.spareLock)
        m_snapshot.spareLock = PAL::makePriorityInheritingLock();
    const std::scoped_lock model(*m_modelLock);
    const ScreenStamp stamp = ScreenStamp::read(*m_terminal);
    const ScreenStamp& captured = m_screenScan.stamp();
    if (stamp.buffer != captured.buffer || stamp.historyEpoch != captured.historyEpoch || !m_terminal->lineStore
        || !m_terminal->lineStore->captureSnapshot(m_snapshot, startFacts, LineStore::SnapshotRange { lines.back().number, lines.front().number + 1 })) {
        cancelRefill();
        m_hasModelChange.store(true, std::memory_order_release);
        return;
    }
    m_historyScan.begin(m_snapshot, m_compiled, m_form);
    m_deferredIndex = 0;
    m_phase = Phase::RefillHistory;
}

bool SearchController::collectRefillHit(void* context, const HistoryHit& hit)
{
    auto& controller = *static_cast<SearchController*>(context);
    const auto lines = controller.m_refillPlan.lines();
    if (!controller.m_refillCollectingLine || *controller.m_refillCollectingLine != hit.line) {
        controller.m_refillCollectingLine = hit.line;
        const auto line = std::ranges::lower_bound(lines, hit.line, std::greater {}, &RefillPlan::Line::number);
        controller.m_refillCollectingPlan = static_cast<std::size_t>(line - lines.begin());
        controller.m_refillLineOrdinal = 0;
    }
    if (controller.m_refillCollectingPlan >= lines.size() || lines[controller.m_refillCollectingPlan].number != hit.line)
        return true;
    const RefillPlan::Line& line = lines[controller.m_refillCollectingPlan];
    const std::uint64_t visited = ++controller.m_refillLineOrdinal;
    if (visited > line.count)
        return true;
    const std::uint64_t ordinal = line.newerCount + line.count - visited;
    if (controller.m_isLocatingAnchor) {
        const Hit& anchor = *controller.m_refillJobAnchor;
        if (controller.m_refillAnchorLineCell + hit.startCell == anchor.start.cell
            && controller.m_refillAnchorLineCell + hit.endCell == anchor.endCell)
            controller.m_refillLocatedOrdinal = ordinal;
    } else if (ordinal >= controller.m_refillFirst && ordinal < controller.m_refillEnd && controller.m_refillHistoryHits.size() < maxStoredHits)
        controller.m_refillHistoryHits.push_back(hit);
    return true;
}

void SearchController::scanRefillHistory()
{
    switch (m_historyScan.step(1, collectRefillHit, this)) {
    case HistoryScanState::Scanning:
        break;
    case HistoryScanState::NeedsFacts: {
        const std::scoped_lock model(*m_modelLock);
        if (!m_terminal->lineStore || !m_terminal->lineStore->captureRegionFacts(m_snapshot, m_historyScan.pendingRegion())) {
            cancelRefill();
            m_hasModelChange.store(true, std::memory_order_release);
        }
        break;
    }
    case HistoryScanState::Stale:
        cancelRefill();
        m_hasModelChange.store(true, std::memory_order_release);
        break;
    case HistoryScanState::Complete:
        m_phase = Phase::RefillDeferred;
        break;
    }
}

void SearchController::scanRefillDeferred()
{
    const auto deferred = m_historyScan.deferredLines();
    if (m_deferredIndex == deferred.size()) {
        m_phase = m_isLocatingAnchor ? Phase::FinishRefillLocation : Phase::PublishRefill;
        return;
    }
    const std::scoped_lock model(*m_modelLock);
    if (m_snapshot.isStale()) {
        cancelRefill();
        m_hasModelChange.store(true, std::memory_order_release);
        return;
    }
    scanHistoryLine(*m_terminal, deferred[m_deferredIndex++], m_compiled, m_form, collectRefillHit, this);
}

void SearchController::publishRefill()
{
    if (!m_refillPublishCursor) {
        std::ranges::sort(m_refillHistoryHits, [](const HistoryHit& left, const HistoryHit& right) {
            return left.line != right.line ? left.line > right.line : left.startCell > right.startCell;
        });
    }
    const std::scoped_lock model(*m_modelLock);
    if (m_generation.load(std::memory_order_acquire) != m_jobGeneration || m_refillJobSequence != m_refillSequence.load(std::memory_order_acquire)) {
        cancelRefill();
        return;
    }
    const Terminal& terminal = *m_terminal;
    const ScreenStamp stamp = ScreenStamp::read(terminal);
    const ScreenStamp& captured = m_screenScan.stamp();
    if ((m_snapshot.isRegistered && m_snapshot.isStale()) || stamp.buffer != captured.buffer
        || stamp.historyEpoch != captured.historyEpoch || stamp.layout != captured.layout
        || (!m_refillScreenHits.empty() && stamp != captured) || !hasValidScope(terminal)) {
        cancelRefill();
        m_hasModelChange.store(true, std::memory_order_release);
        return;
    }
    if (stamp != captured)
        m_hasModelChange.store(true, std::memory_order_release);
    if (!m_refillPublishCursor)
        m_replacementHits.clear();
    std::size_t budget = publishBatch;
    std::optional<Screen::LineID> screenLine;
    std::optional<Screen::LogicalPosition> screenPosition;
    std::uint32_t screenCell = 0;
    while (m_refillPublishCursor < m_refillScreenHits.size() && budget) {
        --budget;
        const Hit& hit = m_refillScreenHits[m_refillPublishCursor++];
        if (!screenLine || *screenLine != hit.start.line) {
            screenLine = hit.start.line;
            screenPosition = recapturePosition(terminal, hit.start);
            screenCell = hit.start.cell;
        }
        if (!screenPosition)
            continue;
        const std::int64_t cell = static_cast<std::int64_t>(screenPosition->cell) + hit.start.cell - screenCell;
        assert(cell >= 0 && cell <= std::numeric_limits<std::uint32_t>::max());
        auto start = *screenPosition;
        start.cell = static_cast<std::uint32_t>(cell);
        start.voidCells = hit.start.voidCells;
        storeHit({ .start = start, .endCell = start.cell + (hit.endCell - hit.start.cell) });
    }
    std::optional<std::uint64_t> previousLine;
    std::optional<Screen::LogicalPosition> position;
    const std::size_t total = m_refillScreenHits.size() + m_refillHistoryHits.size();
    while (m_refillPublishCursor >= m_refillScreenHits.size() && m_refillPublishCursor < total && budget) {
        --budget;
        const HistoryHit& hit = m_refillHistoryHits[m_refillPublishCursor++ - m_refillScreenHits.size()];
        if (!previousLine || *previousLine != hit.line) {
            previousLine = hit.line;
            const auto capturedPosition = Screen::historyPosition(terminal, hit.line, 0);
            position = capturedPosition ? std::optional(*capturedPosition) : std::nullopt;
        }
        if (!position)
            continue;
        auto start = *position;
        start.cell += hit.startCell;
        storeHit({ .start = start, .endCell = position->cell + hit.endCell });
    }
    if (m_refillPublishCursor < total)
        return;
    if (m_replacementHits.size() != m_refillEnd - m_refillFirst) {
        cancelRefill();
        m_hasModelChange.store(true, std::memory_order_release);
        return;
    }
    const auto previous = m_activeIndex >= 0 && static_cast<std::size_t>(m_activeIndex) < m_hits.size()
        ? std::optional<Hit>(recaptureHit(terminal, m_hits[static_cast<std::size_t>(m_activeIndex)]))
        : m_preservedActive;
    m_hits.swap(m_replacementHits);
    m_windowStart = static_cast<std::int64_t>(m_refillFirst);
    m_windowBoundary = m_windowStart ? std::optional(m_hits.front().start) : std::nullopt;
    m_windowBoundaryLine.reset();
    m_windowNewerHistory = 0;
    m_windowSkippedInLine = 0;
    if (m_windowBoundary) {
        if (m_refillFirst < m_refillScreenCount)
            m_windowSkippedInLine = m_screenScan.skippedInBoundaryLine();
        else {
            const RefillPlan::Line& line = m_refillPlan.lines().front();
            m_windowBoundaryLine = line.number;
            m_windowNewerHistory = static_cast<std::int64_t>(line.newerCount - m_refillScreenCount);
            m_windowSkippedInLine = static_cast<std::int64_t>(m_refillFirst - line.newerCount);
            const auto lineStart = Screen::historyPosition(terminal, line.number, 0);
            m_windowLineCell = lineStart ? lineStart->cell : 0;
        }
    }
    m_activeIndex = static_cast<std::int32_t>(m_refillTargetOrdinal - m_refillFirst);
    const Hit& active = m_hits[static_cast<std::size_t>(m_activeIndex)];
    if (!previous || !isSameMatch(*previous, active)) {
        ++m_activeEpoch;
        ++m_revealEpoch;
    }
    m_preservedActive.reset();
    m_refillAnchor.reset();
    ++m_screenGeneration;
    m_consumedRefillSequence.store(m_refillJobSequence, std::memory_order_release);
    cancelRefill();
    recordStatus(Completion::Complete);
}

void SearchController::cancelRefill()
{
    if (m_phase == Phase::RefillScreen || m_phase == Phase::LocateRefillScreen)
        m_hasModelChange.store(true, std::memory_order_release);
    LineStore::releaseSnapshot(m_snapshot);
    m_refillHistoryHits.clear();
    m_refillScreenHits.clear();
    m_isRefilling = false;
    m_phase = Phase::Done;
}

void SearchController::queueRefill(std::int64_t target, StepDirection direction)
{
    m_refillRequestedOrdinal = target;
    m_refillAnchor.reset();
    m_refillAnchorShift = 0;
    m_refillRequestedDirection = direction;
    m_refillSequence.fetch_add(1, std::memory_order_release);
    if (m_wakeSignal) {
        m_wakeSignal->fetch_add(1, std::memory_order_release);
        m_wakeSignal->notify_one();
    }
}

void SearchController::queueAnchorRefill(const Hit& anchor, std::int64_t shift, StepDirection direction)
{
    const Hit preserved = anchor;
    queueRefill(0, direction);
    m_refillAnchor = preserved;
    m_refillAnchorShift = shift;
}

void SearchController::prepareWindow(const Terminal& terminal, std::uint64_t historyHead)
{
    if (!m_windowBoundary)
        return;
    const auto position = recapturePosition(terminal, *m_windowBoundary);
    if (!position) {
        const auto active = m_activeIndex >= 0 && static_cast<std::size_t>(m_activeIndex) < m_hits.size()
            ? std::optional(m_hits[static_cast<std::size_t>(m_activeIndex)])
            : m_preservedActive;
        const bool isPending = m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire);
        if (active && Screen::resolvePosition(terminal, active->start)) {
            if (!m_refillAnchor) {
                const auto shift = isPending ? m_refillRequestedOrdinal - (m_windowStart + std::max(m_activeIndex, 0)) : 0;
                queueAnchorRefill(*active, shift, isPending ? m_refillRequestedDirection : StepDirection::Older);
            }
            m_preservedActive = active;
        } else
            queueRefill(0, StepDirection::Older);
        m_windowBoundary.reset();
        m_windowBoundaryLine.reset();
        m_windowNewerHistory = 0;
        m_windowSkippedInLine = 0;
        m_windowStart = 0;
        return;
    }
    m_windowBoundary = *position;
    const auto number = position->buffer == Screen::Buffer::Primary ? Screen::lineNumberOf(terminal, position->line) : std::nullopt;
    if (number && *number < historyHead) {
        if (!m_windowBoundaryLine)
            m_windowNewerHistory = 0;
        m_windowBoundaryLine = *number;
        const auto lineStart = Screen::historyPosition(terminal, *number, 0);
        m_windowLineCell = lineStart ? lineStart->cell : 0;
    } else
        m_windowBoundaryLine.reset();
}

void SearchController::replaceHistoryCount(std::uint64_t line, std::int64_t count)
{
    if (m_windowBoundaryLine && line > *m_windowBoundaryLine)
        m_windowNewerHistory += count - m_historyCounts.count(line);
    m_historyCounts.replace(line, count);
}

void SearchController::updateWindowOrdinal()
{
    const std::int64_t previous = m_windowStart;
    if (!m_windowBoundary)
        m_windowStart = 0;
    else if (m_windowBoundaryLine)
        m_windowStart = m_windowNewerHistory + m_screenCount + m_windowSkippedInLine;
    else {
        m_windowStart = m_screenScan.skippedCount();
        m_windowSkippedInLine = m_screenScan.skippedInBoundaryLine();
    }
    if (!m_refillAnchor && m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire))
        m_refillRequestedOrdinal += m_windowStart - previous;
}

void SearchController::scanScreen()
{
    if (!m_isScreenActive) {
        const std::scoped_lock model(*m_modelLock);
        if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
            return;
        const ScreenStamp stamp = ScreenStamp::read(*m_terminal);
        const ScreenStamp& previous = m_screenScan.stamp();
        if (stamp.buffer != previous.buffer || stamp.historyEpoch != previous.historyEpoch
            || (stamp.layout != previous.layout && stamp.historyHead < previous.historyHead)) {
            m_hasModelChange.store(false, std::memory_order_release);
            restartJob();
            return;
        }
        prepareWindow(*m_terminal, stamp.historyHead);
        m_screenScan.begin(*m_terminal, m_jobRequest.scope, m_windowBoundary);
        m_observedStamp = m_screenScan.stamp();
        m_hasModelChange.store(false, std::memory_order_release);
        m_isScreenActive = true;
        return;
    }
    if (!m_screenScan.isCaptured()) {
        const std::scoped_lock model(*m_modelLock);
        if (!m_screenScan.capturePrefix(*m_terminal)) {
            m_isScreenActive = false;
            m_hasModelChange.store(true, std::memory_order_release);
        }
        return;
    }
    if (m_screenScan.step(m_compiled, m_form))
        return;
    prepareHistoryCounts();
    publishScreen();
    m_isScreenActive = false;
}

void SearchController::publishScreen()
{
    const std::scoped_lock model(*m_modelLock);
    if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
        return;
    const Terminal& terminal = *m_terminal;
    const ScreenStamp stamp = ScreenStamp::read(terminal);
    if (stamp.buffer != m_screenScan.stamp().buffer || stamp.layout != m_screenScan.stamp().layout || stamp.historyEpoch != m_screenScan.stamp().historyEpoch) {
        m_hasModelChange.store(true, std::memory_order_release);
        return;
    }
    prepareWindow(terminal, m_screenScan.stamp().historyHead);
    auto active = m_activeIndex >= 0 && static_cast<std::size_t>(m_activeIndex) < m_hits.size()
        ? std::optional<Hit>(m_hits[static_cast<std::size_t>(m_activeIndex)])
        : m_preservedActive;
    if (active)
        active = recaptureHit(terminal, *active);
    const bool isScopeValid = hasValidScope(terminal);
    if (!isScopeValid) {
        m_historyCounts.clear();
        m_hits.clear();
        m_screenLines.clear();
        m_pending.clear();
        m_pendingLines.clear();
        LineStore::releaseSnapshot(m_snapshot);
        m_historyCoveredHead = m_screenScan.stamp().historyHead;
        m_phase = Phase::Done;
    }
    const std::uint64_t front = retainedHistoryFront(terminal);
    m_historyCounts.trimBefore(front);
    for (const auto& line : m_screenLines) {
        const auto position = recapturePosition(terminal, line.position);
        const auto number = position && position->buffer == Screen::Buffer::Primary ? Screen::lineNumberOf(terminal, position->line) : std::nullopt;
        const bool isStillCaptured = position && std::ranges::any_of(m_screenScan.lines(), [&position](const auto& captured) {
            return captured.position.line == position->line && captured.position.buffer == position->buffer;
        });
        const bool isPromoted = !isStillCaptured && number && terminal.lineStore && *number >= terminal.lineStore->frontLineNumber()
            && *number < m_screenScan.stamp().historyHead;
        if (isPromoted) {
            replaceHistoryCount(*number, m_historyCounts.count(*number) + line.count);
            for (Hit& hit : m_hits) {
                if (hit.start.line != line.position.line || hit.start.buffer != line.position.buffer)
                    continue;
                const auto cell = position->cell + (hit.start.cell - line.position.cell);
                const auto endCell = cell + (hit.endCell - hit.start.cell);
                const auto voidCells = hit.start.voidCells;
                const auto subCell = hit.start.subCell;
                hit.start = *position;
                hit.start.cell = cell;
                hit.start.voidCells = voidCells;
                hit.start.subCell = subCell;
                hit.endCell = endCell;
            }
            continue;
        }
        std::erase_if(m_hits, [&line](const Hit& hit) { return hit.start.line == line.position.line && hit.start.buffer == line.position.buffer; });
    }
    std::optional<Screen::LineID> previousLine;
    std::optional<std::uint64_t> previousNumber;
    std::erase_if(m_hits, [&](const Hit& hit) {
        if (hit.start.buffer != Screen::Buffer::Primary)
            return false;
        if (!previousLine || hit.start.line != *previousLine) {
            previousLine = hit.start.line;
            previousNumber = Screen::lineNumberOf(terminal, *previousLine);
        }
        return previousNumber ? *previousNumber < front : !hit.start.line.isPinned();
    });
    m_screenLines.assign(m_screenScan.lines().begin(), m_screenScan.lines().end());
    m_screenCount = 0;
    m_replacementHits.clear();
    for (auto& line : m_screenLines) {
        if (!isScopeValid || !line.position.line.value || !Screen::resolvePosition(terminal, line.position)) {
            line.count = 0;
            continue;
        }
        m_screenCount += line.count;
        const auto hits = m_screenScan.hits().subspan(line.firstHit, line.hitCount);
        m_replacementHits.insert(m_replacementHits.end(), hits.begin(), hits.end());
    }
    const std::size_t retained = std::min(m_hits.size(), maxStoredHits - m_replacementHits.size());
    m_replacementHits.insert(m_replacementHits.end(), m_hits.begin(), m_hits.begin() + static_cast<std::ptrdiff_t>(retained));
    m_hits.swap(m_replacementHits);
    restoreActive(active);
    m_foundCount = m_historyCounts.total() + m_screenCount;
    updateWindowOrdinal();
    if (m_phase == Phase::Done && m_historyCoveredHead < m_screenScan.stamp().historyHead)
        m_phase = Phase::CaptureExtension;
    ++m_screenGeneration;
    recordStatus(m_phase == Phase::Done ? Completion::Complete : Completion::Incomplete);
}

void SearchController::restoreActive(const std::optional<Hit>& active)
{
    const auto found = active ? std::ranges::find_if(m_hits, [&active](const Hit& hit) { return isSameMatch(hit, *active); })
                              : m_hits.end();
    const std::int32_t oldIndex = m_activeIndex;
    const bool isPending = m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire);
    if (active && found == m_hits.end()) {
        if (m_hits.size() == maxStoredHits && Screen::resolvePosition(*m_terminal, active->start)) {
            m_preservedActive = active;
            if (!m_refillAnchor) {
                const auto shift = isPending ? m_refillRequestedOrdinal - (m_windowStart + std::max(oldIndex, 0)) : 0;
                queueAnchorRefill(*active, shift, isPending ? m_refillRequestedDirection : StepDirection::Older);
            }
        } else if (isPending)
            m_preservedActive = active;
    }
    m_activeIndex = found != m_hits.end()     ? static_cast<std::int32_t>(found - m_hits.begin())
        : m_hits.empty() || m_preservedActive ? -1
                                              : std::clamp(oldIndex, 0, static_cast<std::int32_t>(m_hits.size()) - 1);
    if (found != m_hits.end())
        m_preservedActive.reset();
    if (found != m_hits.end() && oldIndex >= 0 && isPending && !m_refillAnchor)
        m_refillRequestedOrdinal += m_activeIndex - oldIndex;
    if ((active && found == m_hits.end() && !m_preservedActive) || (!active && m_activeIndex >= 0)) {
        ++m_activeEpoch;
        if (m_activeIndex >= 0)
            ++m_revealEpoch;
    }
}

bool SearchController::collectHistoryHit(void* context, const HistoryHit& hit)
{
    auto& controller = *static_cast<SearchController*>(context);
    if (controller.m_pendingLines.empty() || controller.m_pendingLines.back().line != hit.line)
        controller.m_pendingLines.push_back({ .line = hit.line, .firstHit = controller.m_pending.size() });
    PendingLine& line = controller.m_pendingLines.back();
    ++line.count;
    if (controller.m_windowBoundaryLine && *controller.m_windowBoundaryLine == hit.line
        && controller.m_windowLineCell + hit.startCell > controller.m_windowBoundary->cell)
        ++line.skippedCount;
    const std::size_t room = maxStoredHits - line.firstHit;
    if (line.hitCount < room) {
        controller.m_pending.push_back(hit);
        ++line.hitCount;
    } else if (room)
        controller.m_pending[line.firstHit + (static_cast<std::size_t>(line.count) - 1) % room] = hit;
    return true;
}

void SearchController::collectHistoryLine(void* context, std::uint64_t number)
{
    auto& controller = *static_cast<SearchController*>(context);
    if (controller.m_pendingLines.empty() || controller.m_pendingLines.back().line != number)
        controller.m_pendingLines.push_back({ .line = number, .firstHit = controller.m_pending.size() });
}

void SearchController::scanHistory()
{
    const HistoryScanState state = m_historyScan.step(1, collectHistoryHit, this, collectHistoryLine);
    switch (state) {
    case HistoryScanState::Scanning:
        break;
    case HistoryScanState::NeedsFacts: {
        const std::scoped_lock model(*m_modelLock);
        if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
            return;
        if (m_terminal->lineStore && m_terminal->lineStore->captureRegionFacts(m_snapshot, m_historyScan.pendingRegion()))
            return;
        if (m_snapshot.isStale()) {
            restartJob();
            return;
        }
        // Not stale, so eviction took the region's rows, and every older
        // region's: the scan is done with history.
        m_historyScan.finishAtEvictedFront();
        m_phase = Phase::Deferred;
        break;
    }
    case HistoryScanState::Stale:
        // A rare mutation rewrote or freed history under the scan, and may
        // have renumbered it: the job starts over from a fresh capture.
        restartJob();
        return;
    case HistoryScanState::Complete:
        m_phase = Phase::Deferred;
        break;
    }
    if (m_pending.size() >= publishBatch || m_pendingLines.size() >= publishBatch || std::chrono::steady_clock::now() - m_lastPublish >= publishInterval)
        publishPending(Completion::Incomplete);
}

void SearchController::restartJob()
{
    LineStore::releaseSnapshot(m_snapshot);
    m_pending.clear();
    m_pendingLines.clear();
    m_phase = Phase::Start;
}

void SearchController::scanDeferred()
{
    const auto deferred = m_historyScan.deferredLines();
    if (m_deferredIndex < deferred.size()) {
        {
            const std::scoped_lock model(*m_modelLock);
            if (m_generation.load(std::memory_order_acquire) != m_jobGeneration)
                return;
            if (m_snapshot.isStale()) {
                restartJob();
                return;
            }
            scanHistoryLine(*m_terminal, deferred[m_deferredIndex++], m_compiled, m_form, collectHistoryHit, this, collectHistoryLine);
        }
        if (m_pending.size() >= publishBatch || m_pendingLines.size() >= publishBatch || std::chrono::steady_clock::now() - m_lastPublish >= publishInterval)
            publishPending(Completion::Incomplete);
        return;
    }
    m_historyCoveredHead = m_historyScanEnd;
    m_phase = m_historyCoveredHead < m_screenScan.stamp().historyHead ? Phase::CaptureExtension : Phase::Done;
    publishPending(m_phase == Phase::Done ? Completion::Complete : Completion::Incomplete);
    LineStore::releaseSnapshot(m_snapshot);
}

void SearchController::publishPending(Completion completion)
{
    for (const PendingLine& line : m_pendingLines) {
        auto first = m_pending.begin() + static_cast<std::ptrdiff_t>(line.firstHit);
        auto end = first + static_cast<std::ptrdiff_t>(line.hitCount);
        if (line.hitCount && line.count > static_cast<std::int64_t>(line.hitCount))
            std::rotate(first, first + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(line.count) % line.hitCount), end);
        std::reverse(first, end);
    }
    std::ranges::sort(m_pendingLines, std::greater {}, &PendingLine::line);
    prepareHistoryCounts();
    const std::scoped_lock model(*m_modelLock);
    if (m_generation.load(std::memory_order_acquire) != m_jobGeneration) {
        m_pending.clear();
        m_pendingLines.clear();
        return;
    }
    if (m_snapshot.isRegistered && m_snapshot.isStale()) {
        restartJob();
        return;
    }
    const Terminal& terminal = *m_terminal;
    const ScreenStamp stamp = ScreenStamp::read(terminal);
    if (stamp.buffer != m_screenScan.stamp().buffer || stamp.historyEpoch != m_screenScan.stamp().historyEpoch) {
        restartJob();
        return;
    }
    prepareWindow(terminal, m_screenScan.stamp().historyHead);
    const std::uint64_t front = retainedHistoryFront(terminal);
    m_historyCounts.trimBefore(front);
    auto active = m_activeIndex >= 0 && static_cast<std::size_t>(m_activeIndex) < m_hits.size()
        ? std::optional<Hit>(m_hits[static_cast<std::size_t>(m_activeIndex)])
        : m_preservedActive;
    if (active)
        active = recaptureHit(terminal, *active);
    const bool isScopeValid = hasValidScope(terminal);
    if (!isScopeValid) {
        m_historyCounts.clear();
        m_screenCount = 0;
        m_screenLines.clear();
        m_pending.clear();
        m_pendingLines.clear();
        LineStore::releaseSnapshot(m_snapshot);
        m_historyCoveredHead = m_screenScan.stamp().historyHead;
        m_phase = Phase::Done;
        completion = Completion::Complete;
    }
    bool needsRefill = false;
    for (PendingLine& line : m_pendingLines) {
        if (line.line < front || line.line >= m_screenScan.stamp().historyHead) {
            line.hitCount = 0;
            continue;
        }
        const auto position = Screen::historyPosition(terminal, line.line, 0);
        if (!position || !containsHit({ .start = *position, .endCell = position->cell })) {
            line.count = 0;
            line.hitCount = 0;
        }
        replaceHistoryCount(line.line, line.count);
        if (m_windowBoundaryLine) {
            if (line.line > *m_windowBoundaryLine)
                line.hitCount = 0;
            else if (line.line == *m_windowBoundaryLine) {
                m_windowSkippedInLine = line.skippedCount;
                if (line.count > static_cast<std::int64_t>(line.hitCount)) {
                    line.hitCount = 0;
                    line.shouldReplaceHits = false;
                    needsRefill = true;
                } else {
                    while (line.hitCount && m_windowLineCell + m_pending[line.firstHit].startCell > m_windowBoundary->cell) {
                        ++line.firstHit;
                        --line.hitCount;
                    }
                }
            }
        }
    }
    m_replacementHits.clear();
    std::size_t old = 0;
    std::optional<Screen::LineID> previousLine;
    std::optional<std::uint64_t> previousNumber;
    bool isReplaced = false;
    const auto oldNumber = [&]() -> std::optional<std::uint64_t> {
        if (!isScopeValid)
            return std::nullopt;
        const Hit& hit = m_hits[old];
        if (!previousLine || hit.start.line != *previousLine) {
            previousLine = hit.start.line;
            previousNumber = hit.start.buffer == Screen::Buffer::Primary ? Screen::lineNumberOf(terminal, *previousLine) : std::nullopt;
            isReplaced = false;
            if (previousNumber) {
                const auto line = std::ranges::lower_bound(m_pendingLines, *previousNumber, std::greater {}, &PendingLine::line);
                isReplaced = line != m_pendingLines.end() && line->line == *previousNumber && line->shouldReplaceHits;
            }
        }
        if (isReplaced || (previousNumber && *previousNumber < front) || (!previousNumber && !hit.start.line.isPinned()))
            return std::nullopt;
        return previousNumber.value_or(std::numeric_limits<std::uint64_t>::max());
    };
    for (const PendingLine& line : m_pendingLines) {
        if (!line.hitCount)
            continue;
        const auto position = Screen::historyPosition(terminal, line.line, 0);
        if (!position)
            continue;
        while (old < m_hits.size() && m_replacementHits.size() < maxStoredHits) {
            const auto number = oldNumber();
            if (!number) {
                ++old;
                continue;
            }
            if (*number < line.line)
                break;
            storeHit(m_hits[old++]);
        }
        for (const HistoryHit& hit : std::span(m_pending).subspan(line.firstHit, line.hitCount)) {
            if (m_replacementHits.size() >= maxStoredHits)
                break;
            auto start = *position;
            start.cell += hit.startCell;
            storeHit({ .start = start, .endCell = position->cell + hit.endCell });
        }
    }
    while (old < m_hits.size() && m_replacementHits.size() < maxStoredHits) {
        if (oldNumber())
            storeHit(m_hits[old]);
        ++old;
    }
    m_hits.swap(m_replacementHits);
    restoreActive(active);
    m_foundCount = m_historyCounts.total() + m_screenCount;
    updateWindowOrdinal();
    if (needsRefill && active && !m_refillAnchor) {
        const bool isPending = m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire);
        const auto shift = isPending ? m_refillRequestedOrdinal - (m_windowStart + std::max(m_activeIndex, 0)) : 0;
        queueAnchorRefill(*active, shift, isPending ? m_refillRequestedDirection : StepDirection::Older);
    }
    m_pending.clear();
    m_pendingLines.clear();
    m_lastPublish = std::chrono::steady_clock::now();
    recordStatus(completion);
}

void SearchController::storeHit(const Hit& hit)
{
    if (m_replacementHits.size() < maxStoredHits)
        m_replacementHits.push_back(hit);
}

bool SearchController::hasValidScope(const Terminal& terminal) const
{
    if (!m_jobRequest.scope)
        return true;
    const auto first = Screen::resolvePosition(terminal, m_jobRequest.scope->first);
    const auto last = Screen::resolvePosition(terminal, m_jobRequest.scope->last);
    return first && last && first->absoluteRow <= last->absoluteRow;
}

bool SearchController::containsHit(const Hit& hit) const
{
    if (!m_jobRequest.scope)
        return true;
    const Scope& scope = *m_jobRequest.scope;
    const auto first = Screen::resolvePosition(*m_terminal, scope.first);
    const auto last = Screen::resolvePosition(*m_terminal, scope.last);
    const auto place = Screen::resolvePosition(*m_terminal, hit.start);
    if (!first || !last || !place || first->absoluteRow > last->absoluteRow)
        return false;
    return hit.start.line == scope.first.line || hit.start.line == scope.last.line
        || (place->absoluteRow >= first->absoluteRow && place->absoluteRow <= last->absoluteRow);
}

std::vector<Label> SearchController::labels() const
{
    std::vector<Label> labels;
    if (!m_terminal || !m_labelLimit)
        return labels;
    labels.reserve(std::min(m_labelLimit, m_hits.size()));
    for (const Hit& hit : m_hits) {
        if (labels.size() >= m_labelLimit)
            break;
        const auto place = Screen::resolvePosition(*m_terminal, hit.start);
        if (!place)
            continue;
        Label label { .position = hit.start, .absoluteRow = place->absoluteRow, .column = place->column };
        std::size_t remaining = maxLabelCodePoints;
        for (int column = 0; column < m_terminal->columns && remaining; ++column) {
            const Cell* cell = Screen::absoluteCell(m_terminal, place->absoluteRow, column);
            if (!cell || isTextContinuation(*cell) || isProportionalRunContinuation(m_terminal, *cell))
                continue;
            if (cell->codePoint == wideContinuationSentinelCodePoint) {
                label.text.push_back(' ');
                --remaining;
                continue;
            }
            std::array<std::uint32_t, 16> cluster;
            std::span<const std::uint32_t> text = runTextCodePoints(m_terminal, *cell);
            if (text.empty()) {
                const int count = cellClusterCodePoints(m_terminal, cell, cluster.data(), static_cast<int>(cluster.size()));
                text = { cluster.data(), static_cast<std::size_t>(std::max(count, 0)) };
            }
            for (const std::uint32_t codePoint : text.first(std::min(text.size(), remaining))) {
                Unicode::appendUTF8(label.text, codePoint);
                --remaining;
            }
        }
        while (!label.text.empty() && label.text.back() == ' ')
            label.text.pop_back();
        labels.push_back(std::move(label));
    }
    return labels;
}

void SearchController::recordStatus(Completion completion)
{
    if (m_hasModelChange.load(std::memory_order_acquire)
        || m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire))
        completion = Completion::Incomplete;
    if (completion == Completion::Complete && m_preservedActive) {
        const Screen::Buffer buffer = m_terminal->screens.isUsingAlternateScreen ? Screen::Buffer::Alternate : Screen::Buffer::Primary;
        if (m_foundCount > 0 && m_preservedActive->start.buffer == buffer
            && Screen::resolvePosition(*m_terminal, m_preservedActive->start)) {
            queueAnchorRefill(*m_preservedActive, 0, StepDirection::Older);
            completion = Completion::Incomplete;
        } else {
            m_preservedActive.reset();
            m_activeIndex = m_hits.empty() ? -1 : 0;
            ++m_activeEpoch;
            if (m_activeIndex >= 0)
                ++m_revealEpoch;
        }
    }
    Status status;
    status.generation = m_publishedGeneration;
    status.screenGeneration = m_screenGeneration;
    status.foundCount = m_foundCount;
    status.activeIndex = m_activeIndex < 0 ? -1 : m_windowStart + m_activeIndex;
    status.activeEpoch = m_activeEpoch;
    status.revealEpoch = m_revealEpoch;
    status.isComplete = completion == Completion::Complete;
    status.isPatternInvalid = m_isPatternInvalid;
    status.isTruncated = m_foundCount > static_cast<std::int64_t>(m_hits.size());
    Screen::ScrollSearch::publishLogicalHits(*m_terminal, m_hits, m_activeIndex, status.activeEpoch);
    if (const auto* active = Screen::ScrollSearch::activeMatch(*m_terminal))
        status.revealViewOffset = Screen::viewOffsetToReveal(Screen::scrollbackCount(m_terminal), m_terminal->rows, active->absoluteRow, Screen::RevealPlacement::Match);
    const std::scoped_lock hold(m_statusMutex);
    m_status = status;
}

bool SearchController::step(StepDirection direction)
{
    const bool isPending = m_refillSequence.load(std::memory_order_acquire) != m_consumedRefillSequence.load(std::memory_order_acquire);
    if (isPending && m_refillAnchor) {
        queueAnchorRefill(*m_refillAnchor, m_refillAnchorShift + (direction == StepDirection::Older ? 1 : -1), direction);
        return true;
    }
    if (m_hits.empty())
        return false;
    const std::int64_t current = isPending ? m_refillRequestedOrdinal : m_windowStart + std::max(m_activeIndex, 0);
    std::int64_t target = direction == StepDirection::Older ? current + 1 : current - 1;
    const Status published = status();
    if (!isPending && published.isComplete)
        target = (target % m_foundCount + m_foundCount) % m_foundCount;
    m_preservedActive.reset();
    if (isPending || target < m_windowStart || target >= m_windowStart + static_cast<std::int64_t>(m_hits.size())) {
        queueRefill(direction == StepDirection::Older ? current + 1 : current - 1, direction);
        if (!isPending)
            recordStatus(Completion::Incomplete);
        return true;
    }
    const std::int32_t previous = m_activeIndex;
    m_activeIndex = static_cast<std::int32_t>(target - m_windowStart);
    if (m_activeIndex != previous) {
        ++m_activeEpoch;
        ++m_revealEpoch;
    }
    recordStatus(published.isComplete ? Completion::Complete : Completion::Incomplete);
    return true;
}

Status SearchController::searchAndWait(const Request& request, std::chrono::milliseconds timeout)
{
    if (!setQuery(request)) {
        const std::scoped_lock hold(m_requestMutex);
        const Status published = status();
        if (m_request == request && published.isComplete && published.generation == m_generation.load(std::memory_order_acquire))
            m_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (runSlice()) {
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }
    return status();
}

void SearchController::notifyChanged()
{
    if (m_changed)
        m_changed(m_changedContext);
}

Status searchScrollbackAndWait(Terminal& terminal, const Request& request, std::uint32_t timeoutMilliseconds)
{
    std::mutex modelLock;
    SearchController controller(terminal, modelLock);
    const Status status = controller.searchAndWait(request, std::chrono::milliseconds(timeoutMilliseconds));
    controller.detach();
    return status;
}

} // namespace Ubiquitty::Search
