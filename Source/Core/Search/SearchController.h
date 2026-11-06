// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// SearchController — one terminal's find search, run in short slices so the
// model lock is held only briefly at a time.
//
// A request starts a job. The job's first slice takes the model lock once:
// it captures the history's block directory and copies the live screen.
// The copies are then scanned without the lock, block by block, newest first
// (Core/Search/HistoryScan.h), and the hits it finds are published in short
// lock slices as logical positions. A new request abandons the running job at
// the next slice boundary, and a publish re-checks the request under the lock,
// so nothing an old request found is ever published for a new one.
// Completed queries remain subscribed to model publications. A coalesced
// notification refreshes the screen result set without restarting history.
//
// The controller is shared between its session and the executor that runs
// its slices (Core/Search/SearchExecutor.h). The session `detach`es it before
// the terminal goes; after `detach` returns, no slice reaches the terminal.
//
// Lock order: the job mutex, then the model lock. A slice holding the model
// lock never waits on the job mutex, and the reader gate's lock is taken only
// without the model lock.

#include "Core/LineStore.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Search/HistoryScan.h"
#include "Core/Search/LineCounts.h"
#include "Core/Search/RefillPlan.h"
#include "Core/Search/ScreenScan.h"
#include "Core/Search/Search.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Search {

class SearchExecutor;

/// One terminal's find search.
class SearchController {
public:
    /// Called after the controller publishes, without the model lock, on the
    /// thread that ran the slice. The job mutex remains held so detach waits
    /// for the callback; it must not reenter a job-mutex operation.
    using ChangedCallback = void (*)(void* context);

    /// Makes a controller for a terminal and the lock that guards it.
    ///
    /// - Parameters:
    ///   - terminal: The terminal; outlives the controller's attachment.
    ///   - modelLock: The lock every access to the terminal holds.
    /// - Returns: The controller, or null when the allocation was refused.
    [[nodiscard]] static std::shared_ptr<SearchController> make(Terminal& terminal, std::mutex& modelLock);

    /// Makes a controller subscribed to a particular executor's wake-up.
    /// - Parameters:
    ///   - terminal: The model.
    ///   - modelLock: Its lock.
    ///   - executor: The executor that will schedule this controller.
    /// - Returns: The attached controller.
    [[nodiscard]] static std::shared_ptr<SearchController> make(Terminal& terminal, std::mutex& modelLock, SearchExecutor& executor);

    /// Binds a controller to a terminal and the lock that guards it; `make`
    /// is the way to get one the executor can share.
    ///
    /// - Parameters:
    ///   - terminal: The terminal.
    ///   - modelLock: The lock every access to the terminal holds.
    ///   - wake: The executor's notification state; null for manual slicing.
    SearchController(Terminal& terminal, std::mutex& modelLock, std::shared_ptr<std::atomic<std::uint64_t>> wake = nullptr);
    SearchController(const SearchController&) = delete;
    SearchController& operator=(const SearchController&) = delete;

    /// Sets the callback a publish calls; null for none.
    ///
    /// - Parameters:
    ///   - callback: The callback.
    ///   - context: Handed to it.
    void setChangedCallback(ChangedCallback callback, void* context);

    /// Starts, replaces or clears the search; an empty pattern clears it, and
    /// a request equal to the running one changes nothing.
    ///
    /// Any thread, without the model lock: it records the request and leaves
    /// the work to the executor or a pumping caller.
    ///
    /// - Parameter request: The request.
    /// - Returns: Whether the request started a new job.
    bool setQuery(const Request& request);

    /// The current request's generation, including a scan not yet published.
    ///
    /// - Returns: The request generation; zero before any request.
    [[nodiscard]] std::uint64_t requestedGeneration() const { return m_generation.load(std::memory_order_acquire); }

    /// Returns the status as last published. Any thread.
    [[nodiscard]] Status status() const;

    /// Ends the controller's hold on its terminal. After it returns, no slice
    /// reaches the terminal; the caller does not hold the model lock.
    void detach();

    /// Runs one slice of the job on the calling thread, without the model
    /// lock held by the caller.
    ///
    /// - Returns: Whether work remains.
    bool runSlice();

    /// Returns whether a job has work left. Any thread.
    [[nodiscard]] bool hasWork() const;

    /// Coalesces a model publication into pending maintenance. The caller
    /// holds this controller's model lock; no other lock is acquired.
    /// - Parameter terminal: The presented terminal.
    void noteModelChanged(const Terminal& terminal);

    /// Returns the published hits in search order, newest first. The caller
    /// holds the model lock, and the span lasts until it drops it.
    [[nodiscard]] std::span<const Hit> hits() const { return m_hits; }

    /// Reads the requested prefix of result labels under the model lock.
    ///
    /// - Returns: Bounded row text and positions, omitting destroyed hits.
    [[nodiscard]] std::vector<Label> labels() const;

    /// Moves toward the next hit, wrapping at the ends of the whole query.
    /// A hit outside the stored window is published after a sliced refill.
    /// The caller holds the model lock.
    ///
    /// - Parameter direction: Toward older or newer text.
    /// - Returns: Whether the navigation request was accepted.
    bool step(StepDirection direction);

    /// Runs a request to completion on the calling thread, for tests and
    /// automation. A completed identical request is refreshed so repeated
    /// probes observe text that arrived since the previous scan.
    ///
    /// - Parameters:
    ///   - request: The request.
    ///   - timeout: How long the job may run.
    /// - Returns: The final status; incomplete when the time ran out.
    Status searchAndWait(const Request& request, std::chrono::milliseconds timeout);

private:
    /// Where the running job is.
    enum class Phase : std::uint8_t {
        Idle,
        Start,
        CaptureExtension,
        History,
        Deferred,
        LocateRefillPlan,
        LocateRefillScreen,
        FinishRefillLocation,
        PlanRefill,
        CaptureRefill,
        RefillScreen,
        RefillHistory,
        RefillDeferred,
        PublishRefill,
        Done,
    };

    /// Whether a publish ends the job.
    enum class Completion : std::uint8_t {
        Incomplete,
        Complete,
    };

    /// Begins a request: compiles its pattern and arms the start slice. Job
    /// mutex held.
    ///
    /// - Parameter generation: The request's generation.
    void beginJob(std::uint64_t generation);
    /// Captures the history directory and screen, under the model lock.
    /// Job mutex held.
    void startJob();
    /// Captures only completed lines beyond the last finished history scan.
    /// Job mutex held; takes the model lock only for the capture.
    void startExtension();
    /// Starts a queued navigation window after exact counting has finished.
    /// Job mutex held; reads the request under the model lock.
    void startRefill();
    /// Locates an anchor's history line in bounded batches of counts.
    /// Job mutex held, without the model lock.
    void locateRefillPlan();
    /// Locates an anchor in the owned screen, using its conversion budget.
    /// Job mutex held, without the model lock.
    void locateRefillScreen();
    /// Starts ordinary window planning at the anchor's exact ordinal.
    /// Job mutex held, without the model lock.
    void finishRefillLocation();
    /// Plans a window around a normalized target using captured job facts.
    /// Job mutex held, without the model lock.
    /// - Parameter target: The requested ordinal, before query-wide wrapping.
    void planRefillWindow(std::int64_t target);
    /// Locates the requested history interval in a bounded batch of counts.
    /// Job mutex held, without the model lock.
    void planRefill();
    /// Captures only the completed history lines in the navigation window.
    /// Job mutex held; takes the model lock for the capture.
    void captureRefill();
    /// Replays one screen slice into the requested navigation window.
    /// Job mutex held, without the model lock.
    void scanRefillScreen();
    /// Reads one history block into the requested navigation window.
    /// Job mutex held; copies missing facts under the model lock.
    void scanRefillHistory();
    /// Reads one deferred history line into the navigation window.
    /// Job mutex held; takes the model lock for pooled text.
    void scanRefillDeferred();
    /// Publishes a complete navigation window without changing query-wide counts.
    /// Job mutex held; checks cancellation under the model lock.
    void publishRefill();
    /// Discards the running refill while leaving its queued request available.
    /// Job mutex held; the caller holds no reader hazard.
    void cancelRefill();
    /// Queues a navigation window and wakes the executor. Model lock held.
    /// - Parameters:
    ///   - target: The desired match ordinal, normalized against the exact count.
    ///   - direction: Which side of the target the window should retain.
    void queueRefill(std::int64_t target, StepDirection direction);
    /// Queues navigation relative to retained text instead of its old ordinal.
    /// Model lock held.
    /// - Parameters:
    ///   - anchor: The text whose ordinal the worker will locate.
    ///   - shift: The requested steps relative to that text.
    ///   - direction: Which side of the target the window should retain.
    void queueAnchorRefill(const Hit& anchor, std::int64_t shift, StepDirection direction);
    /// Recaptures the window boundary before applying new counts. Model lock held.
    /// - Parameters:
    ///   - terminal: The presented model.
    ///   - historyHead: The first line owned by the captured screen.
    void prepareWindow(const Terminal& terminal, std::uint64_t historyHead);
    /// Replaces a history count and tracks matches newer than the window.
    /// Job mutex and model lock held.
    /// - Parameters:
    ///   - line: A prepared completed line.
    ///   - count: Its exact count, including zero.
    void replaceHistoryCount(std::uint64_t line, std::int64_t count);
    /// Updates the global window ordinal after publishing new counts.
    /// Job mutex and model lock held.
    void updateWindowOrdinal();
    /// Prepares retained-line count storage without holding the model lock.
    /// Job mutex held.
    void prepareHistoryCounts();
    /// Captures or scans the next part of a screen refresh. Job mutex held.
    void scanScreen();
    /// Replaces screen results while retaining history and logical focus.
    /// Job mutex held; takes the model lock only to publish.
    void publishScreen();
    /// Scans history blocks without the model lock. Job mutex held.
    void scanHistory();
    /// Scans the lines the history scan deferred, under the model lock. Job
    /// mutex held.
    void scanDeferred();
    /// Starts the job over from a fresh capture, keeping its pattern. Job
    /// mutex held.
    void restartJob();
    /// Publishes the history hits found since the last publish, under the
    /// model lock. Job mutex held.
    ///
    /// - Parameter completion: Whether the job is complete with this publish.
    void publishPending(Completion completion);
    /// Records a published status. Model lock held.
    ///
    /// - Parameter completion: Whether the job is complete.
    void recordStatus(Completion completion);
    /// Adds a hit to the next publication when its bounded store has room.
    /// Model lock held; exact counts are maintained separately.
    /// - Parameter hit: The hit to retain.
    void storeHit(const Hit& hit);
    /// Restores logical focus after replacing or trimming stored hits.
    /// Model lock held.
    /// - Parameter active: The text to keep focused, if any.
    void restoreActive(const std::optional<Hit>& active);
    /// Checks that the scope's two boundaries still name an ordered interval.
    /// Model lock held.
    /// - Parameter terminal: The presented model.
    /// - Returns: True for an unscoped request or a retained interval.
    [[nodiscard]] bool hasValidScope(const Terminal& terminal) const;
    /// Tests a hit against the request's logical line boundaries. Model lock held.
    ///
    /// - Parameter hit: The candidate.
    /// - Returns: Whether the hit belongs to the scope, or the request is unscoped.
    [[nodiscard]] bool containsHit(const Hit& hit) const;
    /// Calls the changed callback, without the model lock and with the job
    /// mutex held, so detach waits for its use of the callback context.
    void notifyChanged();

    /// Gathers a history hit for the next publish; a `HistoryHitSink`.
    ///
    /// - Parameters:
    ///   - context: The controller.
    ///   - hit: The hit.
    /// - Returns: Always true: the scan reads to the end.
    static bool collectHistoryHit(void* context, const HistoryHit& hit);
    /// Records completion even when a history line contains no matches.
    /// - Parameters:
    ///   - context: The controller.
    ///   - line: The completed line.
    static void collectHistoryLine(void* context, std::uint64_t line);
    /// Collects only history hits whose ordinals belong to the refill window.
    /// - Parameters:
    ///   - context: The controller.
    ///   - hit: The next hit in a selected completed line.
    /// - Returns: True to read every selected line completely.
    static bool collectRefillHit(void* context, const HistoryHit& hit);

    /// One complete line's count and bounded prefix of pending hit storage.
    struct PendingLine {
        std::uint64_t line = 0;
        std::int64_t count = 0;
        std::size_t firstHit = 0;
        std::size_t hitCount = 0;
        std::int64_t skippedCount = 0;
        bool shouldReplaceHits = true;
    };

    // The terminal and its lock, cleared by `detach` under the job mutex.
    Terminal* m_terminal = nullptr;
    std::mutex* m_modelLock = nullptr;

    // Held for each slice; `detach` takes it to wait out the one running.
    mutable std::mutex m_jobMutex;

    // The request, written by `setQuery`, read by the next slice.
    mutable std::mutex m_requestMutex;
    Request m_request;
    std::atomic<std::uint64_t> m_generation { 0 };
    std::atomic<std::uint64_t> m_consumedGeneration { 0 };
    std::atomic<bool> m_isAttached { true };
    std::atomic<bool> m_hasJobWork { false };
    std::atomic<bool> m_hasModelChange { false };
    std::atomic<bool> m_isWatchingModel { false };
    std::atomic<std::uint64_t> m_refillSequence { 0 };
    std::atomic<std::uint64_t> m_consumedRefillSequence { 0 };

    mutable std::mutex m_statusMutex;
    Status m_status;

    ChangedCallback m_changed = nullptr;
    void* m_changedContext = nullptr;

    // Job state, under the job mutex.
    Phase m_phase = Phase::Idle;
    std::uint64_t m_jobGeneration = 0;
    Request m_jobRequest;
    Screen::ScrollSearch::CompiledRegex m_compiled;
    Unicode::NormalizationForm m_form = Unicode::NormalizationForm::NFC;
    LineStore::Snapshot m_snapshot;
    HistoryScan m_historyScan;
    std::vector<HistoryHit> m_pending;
    std::vector<PendingLine> m_pendingLines;
    LineCounts m_historyCounts;
    std::uint64_t m_historyCoveredHead = 0;
    std::uint64_t m_historyScanEnd = 0;
    std::size_t m_deferredIndex = 0;
    std::chrono::steady_clock::time_point m_lastPublish {};
    ScreenScan m_screenScan;
    bool m_isScreenActive = false;
    bool m_didScanScreen = false;
    bool m_isRefilling = false;
    std::uint64_t m_refillJobSequence = 0;
    std::uint64_t m_refillFirst = 0;
    std::uint64_t m_refillEnd = 0;
    std::uint64_t m_refillTargetOrdinal = 0;
    std::uint64_t m_refillScreenCount = 0;
    std::int64_t m_refillTotalCount = 0;
    StepDirection m_refillJobDirection = StepDirection::Older;
    std::optional<Hit> m_refillJobAnchor;
    std::int64_t m_refillJobShift = 0;
    std::uint32_t m_refillAnchorLineCell = 0;
    std::optional<std::uint64_t> m_refillLocatedOrdinal;
    bool m_isLocatingAnchor = false;
    RefillPlan m_refillPlan;
    std::vector<HistoryHit> m_refillHistoryHits;
    std::vector<Hit> m_refillScreenHits;
    std::optional<std::uint64_t> m_refillCollectingLine;
    std::size_t m_refillCollectingPlan = 0;
    std::uint64_t m_refillLineOrdinal = 0;
    std::size_t m_refillPublishCursor = 0;

    // Published results, under the model lock.
    std::vector<Hit> m_hits;
    std::vector<Hit> m_replacementHits;
    std::int64_t m_foundCount = 0;
    std::int64_t m_screenCount = 0;
    std::int64_t m_windowStart = 0;
    std::optional<Screen::LogicalPosition> m_windowBoundary;
    std::optional<std::uint64_t> m_windowBoundaryLine;
    std::uint32_t m_windowLineCell = 0;
    std::int64_t m_windowNewerHistory = 0;
    std::int64_t m_windowSkippedInLine = 0;
    std::int64_t m_refillRequestedOrdinal = 0;
    std::optional<Hit> m_refillAnchor;
    std::int64_t m_refillAnchorShift = 0;
    StepDirection m_refillRequestedDirection = StepDirection::Older;
    std::int32_t m_activeIndex = -1;
    std::uint32_t m_activeEpoch = 0;
    std::uint32_t m_revealEpoch = 0;
    std::uint64_t m_publishedGeneration = 0;
    std::uint64_t m_screenGeneration = 0;
    bool m_isPatternInvalid = false;
    std::size_t m_labelLimit = 0;
    std::vector<ScreenScan::LineResult> m_screenLines;
    std::optional<Hit> m_preservedActive;
    const std::shared_ptr<std::atomic<std::uint64_t>> m_wakeSignal;
    ScreenStamp m_observedStamp;
};

/// Runs the sliced search over an exclusively owned terminal for automation.
///
/// The caller owns every access to the terminal for the duration of the call.
/// A frontend with a session uses that session's request and wait methods.
/// The resulting highlights remain attached to their text after the call.
///
/// - Parameters:
///   - terminal: The terminal whose text is searched.
///   - request: The query and matching policy.
///   - timeoutMilliseconds: The wait bound; an incomplete status names a timeout.
/// - Returns: The last publication, including count, progress and reveal.
[[nodiscard]] Status searchScrollbackAndWait(Terminal& terminal, const Request& request, std::uint32_t timeoutMilliseconds = 30'000);

} // namespace Ubiquitty::Search
