// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Modes/TerminalModes.h"
#include "Core/Network/FileTransferDetect.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Runtime/DrainPass.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/FrameDriver.h"
#include "Core/Runtime/ModeSnapshot.h"
#include "Core/Runtime/ProcessObservation.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/Runtime/TerminalEvents.h"
#include "Core/Runtime/TerminalSnapshots.h"
#include "Core/Runtime/WindowSizeReassert.h"
#include "Core/Session/ProcessState.h"
#include "Core/Title/Format.h"
#include "Core/Title/WalkPacing.h"
#include "PAL/BellMode.h"
#include "PAL/Poll.h"
#include "PAL/TerminalClients.h"
#include "PAL/TerminalIdentity.h"
#include <span>
#include <string_view>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// The portable local-terminal session: owns the local Terminal + PAL::Stream and
// the byte-pump/reply/resize/snapshot semantics above Core and PAL. Portable
// C++23: no AppKit/Metal/ObjC, no exceptions/RTTI, no Values singleton read.
//
// THREADING — single-threaded by contract; each method is annotated with the
// context the caller must run it on. The runtime takes no lock itself.
//   TERM-WRITER  — caller's serial writer context, under the model lock.
//   Stream-only  — touches only the PAL::Stream; safe off the writer.
//   STREAM-FACTS — reads the Stream (e.g. proc walk); must not hold the model
//                  lock (it would stall the writer for a multi-ms chain walk).

namespace Ubiquitty {
struct Terminal;
} // Ubiquitty::Terminal — Core's terminal model (Terminal.h kept out of this header)

namespace Ubiquitty::PAL {
class Stream;
}

namespace Ubiquitty::Runtime {

// Quiet-path maintenance shares one block budget and wake interval across
// the ingest loop and the host reactor. A host spends the budget across all
// sessions, so adding panes cannot multiply a turn's compression work.
inline constexpr int scrollbackFoldBlockBudget = 1;
inline constexpr int scrollbackFoldTickMilliseconds = 1;

class LaunchCommand; // Runtime/LaunchCommand.h — the structured launch intent
class TerminalSession;

using TermHolder = std::unique_ptr<Ubiquitty::Terminal>;

// Fill `out` from `term`'s live modes/cursor/dimensions. A free function so a
// frontend can snapshot a Ubiquitty::Terminal it does not own (e.g. a tmux pane) without a
// TerminalRuntime. `version` is the dimension generation readers use to spot a
// resize. Caller holds the Terminal's model lock.
void captureSnapshotFromTerm(Ubiquitty::Terminal*, std::uint32_t version, RuntimeSnapshot& out);

// Per-chunk feed result: reports how the byte-pump consumed an already-read chunk.
struct FeedResult {
    int consumed = 0; // bytes consumed from the chunk
    bool reachedSyncClear = false; // DECSET-2026 sync-output cleared at this point
};

// Per-drain summary from drainAvailable(). All-scalar — no platform types.
struct DrainOutcome {
    int bytesDrained = 0; // total bytes pumped this drain pass
    DrainPassEnd end = DrainPassEnd::None;
    bool reachedEnd = false; // peer closed (EOF) or fatal read error
    bool fatal = false; // reachedEnd was a fatal error, not clean EOF
    bool caughtUp = false; // pass ended because input settled (EAGAIN), not a capacity yield
    int lastError = 0; // backend errno when fatal
    BellMode bell = BellMode::Off; // resolved bell style this drain
    bool bellWantsNotification = false;
    bool bellRaisesUrgency = false; // DECSET 1042: nudge window urgency on bell
    bool popOnBell = false; // DECSET 1043: raise/focus the window on bell
    std::uint32_t bellsRung = 0; // bells the program rang since the last drain, as this terminal's own route counted them
    bool titleChanged = false; // OSC 0/2 or title-stack pop landed this drain (consumed from the Terminal flag)
    bool fontRequestChanged = false; // bool-ok: reads with its siblings titleChanged / currentDirectoryChanged; an OSC 50 font swap landed this drain
    bool currentDirectoryChanged = false; // OSC 7 / 633 / 1337 directory change landed this drain (consumed likewise)
    bool processStateChanged = false; // bool-ok: reads with its siblings titleChanged / currentDirectoryChanged; the drain's process sample moved
    bool screenContentChanged = false; // bool-ok: reads with its siblings titleChanged / currentDirectoryChanged; set when the grid's cell content moved this drain (contentGeneration advanced), not on a cell-less title/mode/cursor write
    int columns = 0; // grid dims after the drain
    int rows = 0;
    int scrollbackCount = 0;
    Screen::HistoryFacts history; // what a viewport policy judges the drain's output by
    // Mouse-tracking state, read under the lock so the platform caches it lock-free.
    bool mouseReportsActive = false; // any mode: X10 / 1000 / 1001 / 1002 / 1003
    bool mouseMotionReportsActive = false; // drag motion: 1002 cell-motion or 1003 any-event
    bool mouseAnyEventActive = false; // 1003 any-event tracking
    bool mouseHiliteActive = false; // 1001 hilite tracking
    bool mouseSGRPixelsActive = false; // 1016 SGR-pixel coordinates
    ModeSnapshot modes; // sampled under the lock so the platform caches them lock-free
};

/// Why a frame was announced ready. A boundary is a DECSET-2026 commit, whose
/// grid is gone by the next byte and so must be captured where it is announced.
/// A settle is ordinary output reaching a coherent moment — the grid stays put,
/// so a frontend whose live route can capture it for itself (against its own
/// damage) need not pay a whole-grid capture here. A saturated pass is a settle
/// the drain reached only because its pass filled: the writer is still ahead,
/// every row is changing, and a frontend that went to the model for this frame
/// would wait on the lock the parser holds all flood long — measured as a third
/// of the render thread's time, and the term that separated throughput from a
/// design whose ingest thread fills a snapshot the render only swaps. So it is
/// captured where it is announced, like a boundary. A continuing pass ended
/// below capacity but was admitted without read quiescence. It also needs a
/// capture: the next pass can reopen the coherence hold before a frontend reads
/// the announced grid. A frontend that always captures is free to ignore the
/// distinction. A burst-end publish is a settle with one difference that
/// matters to the pacing: it is the last publish there will be. Every throttle
/// here rests on "the next capture carries what this one would have", which
/// holds inside a running burst and not at its end -- a stream that stops has
/// no next capture, and whatever the withheld publish carried stays off the
/// screen until an unrelated edge asks for a frame.
enum class SyncFrameReason {
    Boundary,
    Settle,
    SaturatedPass,
    ContinuingPass,
    BurstEnd,
};

// Platform hooks drainAvailable() calls on the ingest thread. The runtime owns the
// read/parse/flush/bell logic; the platform owns the model-lock primitive and the
// per-frame snapshot capture. The runtime never touches a platform lock type.
class DrainHost {
public:
    virtual ~DrainHost() = default;
    virtual void lockModel() = 0; // acquire the per-pane model lock
    virtual void unlockModel() = 0; // release it
    // A frame just completed — called while the lock is held, so the platform can
    // capture/publish it race-free. The reason says whether the frame must be
    // captured here (Boundary, SaturatedPass, ContinuingPass) or only announced (Settle).
    virtual void onSyncFrameReadyLocked(SyncFrameReason reason) = 0;

    /// Returns the shortest interval this host presents frames at, which the
    /// coherence rule paces its frames to.
    ///
    /// A frame the rule admits inside it would be announced and then dropped
    /// by a host that throttles; paced by the rule instead, it waits and is
    /// asked again. Read on the writer thread.
    ///
    /// - Returns: Microseconds; zero for a host that presents every frame.
    [[nodiscard]] virtual std::uint32_t frameIntervalMicroseconds() const { return 0; }

    /// Returns the time budget for a pass with a local presentation consumer.
    ///
    /// A host can keep its frame throttle while disabling timed drains without
    /// a surface.
    ///
    /// - Returns: Microseconds; zero disables elapsed-time pass boundaries.
    [[nodiscard]] virtual std::uint32_t drainPassIntervalMicroseconds() const { return frameIntervalMicroseconds(); }
};

/// The pane's presentation state as the drain gate consumes it.
///
/// Presented means the selected tab of a non-miniaturized window — not focus,
/// activation, or occlusion, because partly visible pixels still deserve
/// current render state.
enum class PanePresentation : uint8_t {
    Hidden,
    Presented
};

/// How much of a local session's foreground a frontend reads.
///
/// Each level reads what the one below it does and more. Which group holds the
/// terminal is what retires a title or a directory report its program left
/// behind, and where that group is answers the directory of a shell that
/// reports none, so the title and directory are kept by that probe; the
/// process chain is a walk of its own, which only process state reads.
enum class ForegroundObservation : uint8_t {
    /// Nothing is probed, and an edge waits for a reader.
    Unread,
    /// The probe reads the terminal's holder and its directory, and walks no
    /// chain.
    TitleAndDirectory,
    /// The probe walks the foreground chain as well.
    ProcessState
};

/// Whether the frontend is in a live resize: a drag of the window's edge,
/// from the press that starts it to the release that ends it.
enum class LiveResize : uint8_t {
    Idle,
    Active
};

/// The machine-wide load lever: Low Power Mode or serious+ thermal pressure.
enum class MachineThrottle : uint8_t {
    FullRate,
    Throttled
};

/// What a frontend can see about where a pane's window sits.
///
/// Facts, not a verdict. Which of them means a pane has stopped earning the
/// machine's attention is the two folds below, so a shell reports what its
/// window system tells it and nothing else — two shells reading the same
/// window and answering differently is how one of them comes to run a
/// background tab at the panel's rate.
struct PaneVisibility {
    /// Whether the pane's view is in a window at all.
    bool hasWindow = false;
    /// Whether that window is miniaturized.
    bool isMiniaturized = false;
    /// Whether it is a native tab standing behind its group's front one.
    bool isBackgroundedTab = false;
    /// Whether the view is hidden, or stands under something hidden.
    bool isHiddenInWindow = false;
    /// Whether nothing of the window is on screen — covered whole by another
    /// window, or on a space that is not the current one.
    bool isFullyOccluded = false;
    /// Whether the pane's window was brought forward by an edge the window
    /// system's occlusion report has not yet answered. Until the report lands,
    /// `isFullyOccluded` describes the window as it stood before the edge.
    bool isRevealUnconfirmed = false;
};

/// Whether a pane's render state is worth keeping current.
///
/// Occlusion is not one of the facts this reads: a window a hair behind
/// another still has visible pixels, and a pane that stopped capturing for
/// them shows the state it stopped at the moment it comes forward.
///
/// - Parameter pane: What the frontend can see about the window.
/// - Returns: The state the drain gate consumes.
[[nodiscard]] constexpr PanePresentation panePresentation(const PaneVisibility& pane)
{
    const bool isPresented
        = pane.hasWindow && !pane.isMiniaturized && !pane.isBackgroundedTab && !pane.isHiddenInWindow;
    return isPresented ? PanePresentation::Presented : PanePresentation::Hidden;
}

/// Whether a frame drawn now can reach the glass.
enum class PaneFrameDelivery : uint8_t {
    Withhold,
    Draw
};

/// Whether a frame drawn now can reach the glass.
///
/// A covered pane withholds: the drawable it would present into is composited
/// by nobody, so the encode, the present and everything they animate are spent
/// on pixels no reader can see.
///
/// An unconfirmed reveal overrides the cover, and only for a pane the capture
/// fold presents. The window system reports occlusion a frame or more after the
/// reveal that ended it, so a pane that waited for the report would show the
/// layer it stopped at — after a long cover, content minutes old — on every tab
/// switch and every application activation. A tab behind its group's front one,
/// a miniaturized window and a hidden view each have a reveal edge of their
/// own, and an application-wide edge says nothing about them.
///
/// - Parameter pane: What the frontend can see about the window.
/// - Returns: Whether to draw.
[[nodiscard]] constexpr PaneFrameDelivery paneFrameDelivery(const PaneVisibility& pane)
{
    if (!pane.hasWindow || !pane.isFullyOccluded) {
        return PaneFrameDelivery::Draw;
    }
    const bool isRevealOverriding
        = pane.isRevealUnconfirmed && panePresentation(pane) == PanePresentation::Presented;
    return isRevealOverriding ? PaneFrameDelivery::Draw : PaneFrameDelivery::Withhold;
}

/// A reveal edge waiting on the window system's occlusion report.
struct PaneReveal {
    /// Whether an edge is standing unanswered.
    bool isPending = false;
    /// The 60 Hz monotonic tick the edge stops being trusted at.
    std::uint32_t deadlineTicks = 0;
};

/// How long a reveal edge stands unconfirmed before the last report is trusted
/// again, in 60 Hz ticks.
///
/// A second. Long enough for the report to arrive on a machine under load, and
/// short enough that an edge the window system never answers — a window brought
/// forward under another that still covers it — costs a second of frames and
/// not the session.
inline constexpr std::uint32_t revealConfirmationBudgetTicks = 60;

/// The reveal after an edge that brought the pane's window forward.
///
/// Only a covered pane the capture fold presents takes one: any other pane
/// already draws, or is hidden for a reason this edge says nothing about.
///
/// - Parameters:
///   - reveal: The reveal as it stands.
///   - pane: What the frontend can see about the window.
///   - nowTicks: The 60 Hz monotonic count.
/// - Returns: The reveal after the edge.
[[nodiscard]] constexpr PaneReveal notePaneRevealEdge(PaneReveal reveal, const PaneVisibility& pane, std::uint32_t nowTicks)
{
    if (!pane.hasWindow || !pane.isFullyOccluded || panePresentation(pane) != PanePresentation::Presented) {
        return reveal;
    }
    return { .isPending = true, .deadlineTicks = nowTicks + revealConfirmationBudgetTicks };
}

/// Whether a reveal edge still stands.
///
/// Wrap-safe: the tick count is a 60 Hz clock truncated to 32 bits, so the
/// comparison is on the signed difference rather than on the order of two
/// counts that wrap under each other every 828 days.
///
/// - Parameters:
///   - reveal: The reveal to weigh.
///   - nowTicks: The 60 Hz monotonic count.
/// - Returns: Whether the edge is still unanswered and inside its budget.
[[nodiscard]] constexpr bool isPaneRevealUnconfirmed(const PaneReveal& reveal, std::uint32_t nowTicks)
{
    return reveal.isPending && static_cast<std::int32_t>(reveal.deadlineTicks - nowTicks) > 0;
}

/// Whether a pane has earned the panel's full frame rate.
///
/// Everything above, and whether a frame drawn now reaches the glass at all: a
/// pane whose frames are withheld pays no rate, and one drawing through an
/// unconfirmed reveal takes the rate back for it, so the frames the edge asked
/// for are driven rather than asked for one at a time. The pane keeps
/// capturing — that is the question the fold above answers — and only the
/// rate it is drawn at falls.
///
/// - Parameter pane: What the frontend can see about the window.
/// - Returns: Presented where the panel rate is earned.
[[nodiscard]] constexpr PanePresentation paneFullRatePresentation(const PaneVisibility& pane)
{
    if (paneFrameDelivery(pane) == PaneFrameDelivery::Withhold) {
        return PanePresentation::Hidden;
    }
    return panePresentation(pane);
}

/// Whether two process walks describe different processes.
///
/// The chain moving is a different question, and the narrower one: a reaped
/// background job, a program stopped, and a walk that found the tree in another
/// order all leave the chain standing while what a reader is looking at has
/// moved. A view keyed on the chain alone shows the table as it stood.
///
/// - Parameters:
///   - previous: The sample a reader last saw.
///   - next: The sample the walk just took.
/// - Returns: Whether anything in the sample moved.
[[nodiscard]] inline bool didProcessRecordsMove(
    const std::vector<PAL::Process::Record>& previous, const std::vector<PAL::Process::Record>& next)
{
    return previous != next;
}

/// How hard the machine is being pushed thermally.
///
/// The four-step ladder every platform that reports one uses, so the step a
/// frontend reads maps across without a per-shell threshold.
enum class ThermalPressure : uint8_t {
    Nominal = 0,
    Fair = 1,
    Serious = 2,
    Critical = 3
};

/// What a frontend can see about the machine's own budget.
struct MachineBudget {
    /// Whether the reader has asked the machine to spend less.
    bool isInLowPowerMode = false;
    /// The thermal ladder's step.
    ThermalPressure thermal = ThermalPressure::Nominal;
};

/// Whether the machine is asking every pane to cost less.
///
/// Serious is the threshold because it is where the system itself begins
/// shedding work; below it a terminal that throttled would be giving up
/// responsiveness nobody asked it to.
///
/// - Parameter budget: What the frontend can see about the machine.
/// - Returns: The lever the drain gate folds in.
[[nodiscard]] constexpr MachineThrottle machineThrottle(const MachineBudget& budget)
{
    const bool isThrottled = budget.isInLowPowerMode
        || static_cast<uint8_t>(budget.thermal) >= static_cast<uint8_t>(ThermalPressure::Serious);
    return isThrottled ? MachineThrottle::Throttled : MachineThrottle::FullRate;
}

/// What a process-tree watcher is given, and what it does with an edge.
///
/// One object, shared with the watcher for as long as it can call back, rather
/// than two borrows it would have to keep alive itself. The waker arrives after
/// the watcher does — a stream is launched before the loop that drains it is
/// prepared — so it is stored into this rather than captured when the watcher
/// is registered, which would have captured nothing.
struct ProcessTreeSignal {
    /// What the frontend reads, which defers the probe, the walk and their
    /// wakeups it does not.
    std::atomic<ForegroundObservation> observation { ForegroundObservation::ProcessState };
    /// Raised by an edge, cleared by the walk that answers it. Starts raised so
    /// the first sample walks rather than trusting an empty cache.
    std::atomic<bool> dirty { true };
    /// Raised with `dirty` and cleared by the probe of the terminal's holder,
    /// so a reader of the title and directory alone answers an edge once,
    /// without the walk that clears `dirty`.
    std::atomic<bool> isForegroundProbeOwed { true };
    /// How many edges have arrived, ever. The loop watches this for MOVEMENT
    /// rather than for presence: a shell loop forking twenty times a second
    /// keeps `dirty` raised continuously, and answering each edge walks the
    /// process table ten times a second for a tree that is never still. The
    /// count lets the walk wait for the tree to SETTLE instead.
    std::atomic<std::uint32_t> edges { 0 };
    /// Keeps the waker alive for as long as an edge can reach it.
    std::shared_ptr<PAL::Waker> wakerOwner;
    /// What the edge actually reads. Published after `wakerOwner`, so a thread
    /// that sees it non-null sees a waker that is alive.
    std::atomic<PAL::Waker*> waker { nullptr };
};

/// The program a pane runs, as chrome drawn around the pane names it.
///
/// Every field is empty for a session that has ended: nothing runs there, and
/// a name left standing would label a dead pane with a process that is gone,
/// as a title would if it kept its process parts.
struct ForegroundProgram {
    /// The foreground process's own name, without its ancestors ("claude").
    std::string name;
    /// The program's catalog key (`Title::processIconCatalog`), empty for none.
    std::string key;
    /// The foreground process chain, as a title shows it.
    std::string chain;
    /// The same chain spelled with each program's arguments.
    std::string chainWithArguments;
};

class TerminalRuntime {
    friend class TerminalSession;

public:
    /// Why teardown must leave the retained local owner intact.
    enum class CloseError : std::uint8_t {
        OwnershipUnresolved,
    };

    /// Whether local I/O may reach the owned stream.
    [[nodiscard]] bool isLocalStreamEnabled() const { return m_localStreamOwnership.load(std::memory_order_acquire) == LocalStreamOwnership::Local; }
    /// Whether teardown must wait for an exact adoption outcome.
    [[nodiscard]] bool hasUnresolvedLocalAdoption() const { return m_localStreamOwnership.load(std::memory_order_acquire) == LocalStreamOwnership::Frozen; }
    /// Refuses teardown without changing any owner while adoption is unresolved.
    [[nodiscard]] std::expected<void, CloseError> tryClose();
    /// Completed DECSET-2026 frames the parser reported to a drain.
    ///
    /// Where a boundary frame goes missing is otherwise unobservable from
    /// outside: a parser that never reported one and a publish that dropped one
    /// both end with no capture.
    [[nodiscard]] std::uint64_t syncClearSeenCount() const { return m_syncClearSeenCount; }

    /// Settles that declined to publish because the frame was not coherent.
    [[nodiscard]] std::uint64_t settlePublishRefusedCount() const { return m_settlePublishRefusedCount; }

    /// Frames the coherence rule admitted in a class.
    ///
    /// - Parameter frameClass: The class.
    /// - Returns: The count since the runtime began. Writer-thread reads only.
    [[nodiscard]] std::uint64_t frameCount(FrameClass frameClass) const { return m_frameClassCounts[static_cast<std::size_t>(frameClass)]; }

    /// The class of the last frame the coherence rule admitted, or nothing
    /// before the first. Writer-thread reads only.
    [[nodiscard]] std::optional<FrameClass> lastFrameClass() const { return m_lastFrameClass; }

    /// The age of the oldest change when the newest coherent frame was taken.
    /// Reads require the same model lock as `lastFrameClass`.
    [[nodiscard]] std::uint64_t lastFrameLagMicroseconds() const { return m_lastFrameLagMicroseconds; }

    /// Reads the same monotonic clock used for admitted-frame timing.
    [[nodiscard]] std::uint64_t frameClockMicroseconds() const { return coherenceNow(); }

    /// Asks the coherence rule again for a frame it is holding, announces it
    /// as the burst's end when admitted, and returns how long the writer may
    /// wait before asking again.
    ///
    /// A frame held for read quiescence, an open bracket or a parser
    /// mid-sequence is otherwise asked again only when more bytes arrive, and a
    /// program that went quiet would hold it until then. So every driver of
    /// `drainAvailable` calls this when the wait it returned is up: the ingest
    /// loop cuts its poll to it, a single-threaded frontend its own. The rule
    /// reads the terminal, which other threads change under the model lock, so
    /// both the ask and the wait are taken under it. TERM-WRITER; takes the
    /// model lock through `host`.
    ///
    /// - Parameter host: The drain host whose lock and publish hook to use.
    /// - Returns: Microseconds until the rule may answer differently, or
    ///   nothing when no frame is held.
    std::optional<std::uint64_t> publishHeldFrameIfDue(DrainHost& host);

    /// Reports a display transition and the remaining idle interval.
    struct DisplayIdleResult {
        std::optional<std::uint32_t> remainingTicks;
        bool didChange = false;
    };

    /// Services display blanking under the model lock and publishes an edge.
    /// - Parameter host: The model lock and frame delivery hooks.
    /// - Returns: The next timer deadline and whether the display changed.
    DisplayIdleResult serviceDisplayIdle(DrainHost& host);

    /// Publishes a display blanking or wake edge while the caller holds the model lock.
    /// - Parameter host: The frame delivery hooks.
    void publishDisplayChange(DrainHost& host);

    /// Takes whatever frame the coherence rule still holds, because nothing
    /// will ask the rule for it again: the driver is stopping, or the stream
    /// ended. Announced as the burst's end, in the class its state earns (a
    /// screen left mid-sequence, in a 2026 block or in a redraw is forced).
    /// TERM-WRITER; takes the model lock through `host`.
    ///
    /// - Parameter host: The drain host whose lock and publish hook to use.
    void releaseHeldFrame(DrainHost& host);

    /// Whether the pane changed since the last frame the coherence rule
    /// admitted, so the live grid may be a screen the program has not
    /// finished drawing.
    ///
    /// A render that samples the live grid on its own clock samples it here
    /// only when this is false, and otherwise presents the last frame the rule
    /// admitted; any thread.
    [[nodiscard]] bool isFrameHeld() const { return m_isFrameHeld.load(std::memory_order_acquire); }

    /// The clock the coherence rule reads: monotonic microseconds.
    using CoherenceClock = std::uint64_t (*)(void* context);

    /// Replaces the coherence rule's clock, so a test can hold time still
    /// rather than sleep through a rule's window. TERM-WRITER, before the
    /// first drain.
    ///
    /// - Parameters:
    ///   - clock: The clock, or null for the steady clock.
    ///   - context: Handed back to the clock verbatim.
    void setCoherenceClock(CoherenceClock clock, void* context)
    {
        m_coherenceClock = clock;
        m_coherenceClockContext = context;
    }

    /// Takes the frame a DEC 2026 block closed in the middle of a read, when
    /// the coherence rule admits it, by announcing a boundary.
    ///
    /// The read goes on past the close, so this instant is the frame's only
    /// one: a refusal loses it, as it does on the daemon, which takes no frame
    /// inside a pass. Caller holds the model lock; TERM-WRITER.
    ///
    /// - Parameter host: The drain host whose publish hook announces the frame.
    void publishMidReadBoundaryIfCoherent(DrainHost& host);

#if UBIQUITTY_UI_TESTS
    // Bytes parsed while the model lock was held, for the contention scenarios:
    // a hold duration only means something next to the work it covered.
    std::atomic<std::uint64_t> uiTestBytesUnderModelLock { 0 };
#endif
#if UBIQUITTY_ENABLE_TEST_HOOKS
    /// Returns the number of complete runtime snapshots captured.
    ///
    /// Includes the initial snapshot. A throttled drain that publishes nothing
    /// must not advance it.
    ///
    /// - Returns: The cumulative number of complete captures.
    [[nodiscard]] std::uint64_t fullSnapshotCaptureCountForTest() const { return m_fullSnapshotCaptureCountForTest; }
#endif
    TerminalRuntime();
    ~TerminalRuntime();
    TerminalRuntime(const TerminalRuntime&) = delete;
    TerminalRuntime& operator=(const TerminalRuntime&) = delete;

    // --- lifecycle ---------------------------------------------------------
    // TERM-WRITER. Allocates + initialises the Ubiquitty::Terminal to parameters (no Values read).
    void init(const RuntimeInitParameters&);

    // Launch `command` on a fresh local PTY and OWN the resulting stream (RAII:
    // closed in close()/dtor). `identity` is what the child's environment is
    // stamped with; the runtime reads no Values of its own, so the owner's
    // configuration is the only source. Returns false if the open failed.
    // Pairs with close().
    bool launch(const LaunchCommand&, const PAL::TerminalIdentity& identity);

    /// Whether this session ends when the child it launched does.
    ///
    /// Recorded from the launch rather than read from a process-wide flag: two
    /// windows of one process hold different answers, and the one that ran a
    /// `-e` program must not close the one running a login shell.
    ///
    /// - Returns: Whether the launch asked for the child's lifetime.
    [[nodiscard]] bool doesSessionEndWithChild() const { return m_doesSessionEndWithChild; }

    /// The process facts a title is made of, as the drain's probe last
    /// sampled them.
    ///
    /// Sampled here rather than by each frontend's own timer because the walk
    /// of the process table is the costly part, and the gate that avoids it
    /// (`PAL::Process::shouldRewalkForegroundChain`) reads facts this probe
    /// already has in hand: a shell at a steady prompt moves neither its
    /// foreground group nor its name, and walks nothing.
    ///
    /// - Returns: The directory, empty until a probe has reported one.
    [[nodiscard]] std::string currentDirectory() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.currentDirectory;
    }

    /// Updates a pane's host facts from one cached process sample, without
    /// probing the stream or walking processes. Unchanged fields are not copied.
    /// - Parameter facts: The presentation owner's retained local host facts.
    void updatePaneHostFacts(Protocol::Sync::PaneHostFacts& facts) const;

    /// The process group holding the terminal, as of the last probe.
    ///
    /// - Returns: The group, or zero before one has been read.
    [[nodiscard]] std::uint32_t foregroundProcessGroup() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.foregroundProcessGroup;
    }

    /// The replica tty's basename, for the title segment that names it.
    ///
    /// - Returns: The name, or empty for a session with no local PTY.
    [[nodiscard]] std::string ttyName() const;

    /// The session-leader shell's name, for the title segment that names it.
    ///
    /// Taken from the same walk the chain comes from, because the walk already
    /// marks which record is the leader and a second pass would cost what the
    /// first one did.
    ///
    /// - Returns: The name, or empty before a walk has found one.
    [[nodiscard]] std::string shellCommandName() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_shellCommandName;
    }

    /// Names what this session is running, for a title composed before the
    /// first process walk resolves one.
    ///
    /// `launch` seeds from the command it spawns. A session adopting a stream
    /// has no command to read — a serial line is a device, not an argv — so its
    /// opener names it here or the title has nothing until a walk lands.
    ///
    /// - Parameter name: The name to show meanwhile.
    void seedTitleName(const std::string& name);

    /// The foreground process chain, as a title shows it.
    ///
    /// The launch seed answers until a walk has resolved a chain, so a title
    /// composed on the first frame names the shell that was launched rather
    /// than nothing — and so the pre-exec sample, which reads as this
    /// application's own image and is therefore dropped, has something behind
    /// it. One reader, so both shells spell an unresolved chain the same way.
    ///
    /// - Returns: The chain, the seed, or empty for a session never launched.
    [[nodiscard]] std::string processChain() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.processChain.empty() ? m_processState.launchSeed : m_processState.processChain;
    }

    /// Where this session's foreground process leads, if anywhere.
    ///
    /// Read under the same lock as the rest of the walk's facts, because it is
    /// one of them: a tag rule and the titlebar's remote badge ask the same
    /// question the title's chain answered, and a torn read would let them
    /// disagree about which machine the reader is on.
    ///
    /// - Returns: The destination, or a target whose kind is `None`.
    [[nodiscard]] PAL::Process::RemoteTarget remoteTarget() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.remoteTarget;
    }

    /// The host a shell on the far side reported, if one did.
    ///
    /// Set only while the program that reported it holds the terminal, and
    /// while set it outranks the walk, being the only source that sees past a
    /// nested ssh — this machine's tree holds one ssh however many the reader
    /// has opened.
    ///
    /// - Returns: `[user@]host` as the far side spelled it, or empty.
    [[nodiscard]] std::string reportedHost() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.reportedHost;
    }

    /// Every process the last walk found, root to leaf.
    ///
    /// The walk that resolves the title's chain already visits each of these and
    /// then discards all but the formatted line. A second reader — an inspector
    /// listing the subtree — that walks again asks the kernel the same questions
    /// twice and answers from a different instant than the title does, so the
    /// two disagree about what is running; and, having no edge to wake it, it can
    /// only re-ask on a timer, which is a poll wearing a push's clothes.
    ///
    /// Returned by value under the lock, like every other fact here: the walk
    /// runs on the ingest thread and a reader holding a reference into it would
    /// be reading a vector being rewritten.
    ///
    /// - Returns: The records the last walk kept, empty before the first one.
    [[nodiscard]] std::vector<PAL::Process::Record> processRecords() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processRecords;
    }

    /// Whether the shell held the terminal at the last process sample.
    ///
    /// The answer a reader that is redrawn on `processStateChanged` must use.
    /// That event announces a move of this sample, so a reader that answered
    /// from a live read instead would answer for a later instant than the one
    /// announced -- and a short job can hold the terminal between two samples.
    /// A live read landing there sees a job; the samples either side see the
    /// shell, so nothing moves and nothing redraws the reader again.
    ///
    /// - Returns: The sampled answer; the shell before the first sample.
    [[nodiscard]] bool isSampledForegroundShell() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_isSampledForegroundShell;
    }

    /// How many times the sampled process records have moved.
    ///
    /// The key a view over the table compares against, and lock-free because a
    /// frontend asks it on every screen update: copying the records to find out
    /// whether they changed would cost more than the refresh it is deciding
    /// against. The chain's own event is not this question — a job reaped and a
    /// program stopped leave the chain standing.
    ///
    /// - Returns: The generation, zero before the first walk.
    [[nodiscard]] std::uint64_t processRecordsGeneration() const
    {
        return m_processRecordsGeneration.load(std::memory_order_acquire);
    }

    /// The same chain spelled with each program's arguments.
    ///
    /// - Returns: The chain, the seed, or empty for a session never launched.
    [[nodiscard]] std::string processChainWithArguments() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.processChainWithArguments.empty()
            ? m_processState.launchSeed
            : m_processState.processChainWithArguments;
    }

    /// The foreground process's own name, without its ancestors.
    ///
    /// The launch seed answers while no live name resolves — the pre-exec gap
    /// on a fresh tab, or a group the walk could not read — so a title composed
    /// from it names the shell rather than falling through to the application.
    ///
    /// - Returns: The name, the seed, or empty for a session never launched.
    [[nodiscard]] std::string foregroundProcessName() const
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        return m_processState.foregroundName.empty() ? m_processState.launchSeed : m_processState.foregroundName;
    }

    /// The program this session runs, as one process sample published it.
    ///
    /// The name, who holds the terminal, both chains and whether the session
    /// has ended are copied under one hold of the lock a sample publishes them
    /// under, so no field answers for a different sample than another. The
    /// key is resolved from that name and holder.
    ///
    /// - Parameter reportedProcessIcon: The snapshot's `reportedProcessIcon`.
    /// - Returns: The program; every field empty for a session that ended.
    [[nodiscard]] ForegroundProgram foregroundProgram(std::uint8_t reportedProcessIcon) const;

    /// The catalog key of the program this session runs, from a name and a
    /// holder read under one hold of the lock; see ``Title::processIconKeyFor``.
    ///
    /// - Parameter reportedProcessIcon: The snapshot's `reportedProcessIcon`.
    /// - Returns: The key, empty for a session running none.
    [[nodiscard]] std::string processIconKey(std::uint8_t reportedProcessIcon) const;

    /// Whether control-mode tmux holds the terminal, as of the last walk.
    ///
    /// A frontend that speaks the control protocol attaches on this rather
    /// than walking the process table itself to find out.
    ///
    /// - Returns: Whether the foreground is `tmux -CC`.
    [[nodiscard]] bool isForegroundControlModeTmux() const { return m_isForegroundControlModeTmux; }

    /// Whether the last sample moved the facts a title is made of.
    ///
    /// - Returns: Yes when the chain, the leader shell, the records or whether
    ///   the shell holds the terminal differs from the sample before it.
    [[nodiscard]] bool didProcessStateMove() const { return m_didProcessStateMove; }

    /// How often to walk the process table for this session's title, from the
    /// window facts a frontend reports about itself.
    ///
    /// How this session's process ended, once it has.
    ///
    /// Two writers, because the two facts become knowable at different moments
    /// and on different threads. The END is known on the ingest thread the
    /// instant the transport reports end-of-file, and it is what a title and a
    /// caret both act on, so it is published there and pushed with that same
    /// delivery. The STATUS is not knowable then — the child is reaped after
    /// its last replica descriptor closes — so the frontend, which already
    /// handles the exit event on its own thread, collects it afterwards and
    /// hands it back through ``noteSessionExitStatus``. Waiting for the status
    /// before admitting the session ended would leave a departed process named
    /// in the title for as long as the reap took, which is the defect this
    /// whole path exists to remove.
    ///
    /// - Returns: The end, with `didEnd` false while the session still runs.
    [[nodiscard]] Title::SessionEnd sessionEnd() const
    {
        Title::SessionEnd end;
        end.didEnd = m_sessionDidEnd.load(std::memory_order_acquire);
        if (!end.didEnd)
            return end;
        end.hasStatus = m_sessionExitKnown.load(std::memory_order_acquire);
        end.wasSignalled = m_sessionExitWasSignal.load(std::memory_order_relaxed);
        end.code = m_sessionExitCode.load(std::memory_order_relaxed);
        return end;
    }

    /// Collects this session's exit status from the stream and records it.
    ///
    /// For a frontend to call on its own thread once it sees the session end.
    /// The status becomes knowable only after the child is reaped, which
    /// happens after the end-of-file that raised the end — so the ingest thread
    /// could not have had it, and naming the end did not wait for it. Reaping
    /// is what turns "not collected" into a number. Idempotent: a second call
    /// finds the status already latched and does nothing.
    void collectSessionExitStatus();

    /// Records the status a frontend collected after this session ended.
    ///
    /// Ordered so a reader that sees `hasStatus` sees the two fields behind it.
    ///
    /// - Parameters:
    ///   - wasSignalled: Whether a signal ended it rather than its own exit.
    ///   - code: The exit status, or the signal number when signalled.
    void noteSessionExitStatus(bool wasSignalled, int code)
    {
        m_sessionExitWasSignal.store(wasSignalled, std::memory_order_relaxed);
        m_sessionExitCode.store(code, std::memory_order_relaxed);
        m_sessionExitKnown.store(true, std::memory_order_release);
    }

    /// Whether a kernel process-tree edge is waiting to be answered.
    ///
    /// A peek, not a take: what the frontend reads says which sample answers
    /// an edge, and that sample clears it. The probe of the terminal's holder
    /// answers a reader of the title and directory, the walk answers a reader
    /// of process state, and an unread foreground is answered by neither.
    ///
    /// - Returns: Whether an edge waits for the probe or walk that the current
    ///   observation reads.
    [[nodiscard]] bool hasPendingProcessTreeEdge() const
    {
        switch (foregroundObservation()) {
        case ForegroundObservation::Unread:
            return false;
        case ForegroundObservation::TitleAndDirectory:
            return m_processTree->isForegroundProbeOwed.load(std::memory_order_relaxed);
        case ForegroundObservation::ProcessState:
            return m_processTree->dirty.load(std::memory_order_relaxed);
        }
        return false;
    }

    /// Defers what the frontend does not read, retaining an invalidation for
    /// the reader that reads it.
    ///
    /// - Parameter observation: What the frontend reads of the foreground.
    void setForegroundObservation(ForegroundObservation observation)
    {
        const ForegroundObservation previous = m_processTree->observation.exchange(observation, std::memory_order_acq_rel);
        if (observation <= previous)
            return;
        // A reader that reads more is owed what moved while nobody read it.
        m_processTree->dirty.store(true, std::memory_order_relaxed);
        m_processTree->isForegroundProbeOwed.store(true, std::memory_order_relaxed);
        if (PAL::Waker* waker = m_processTree->waker.load(std::memory_order_acquire))
            waker->wake();
    }

    /// How many process-tree edges have arrived on this session, ever.
    ///
    /// For a caller deciding whether the tree has stopped moving. Wraps, which
    /// is harmless: only inequality is ever asked of it.
    [[nodiscard]] std::uint32_t processTreeEdgeCount() const
    {
        return m_processTree->edges.load(std::memory_order_relaxed);
    }

    /// Adopts the waker of the loop that drains this runtime.
    ///
    /// So a process-tree edge, which arrives on the watcher's own thread, can
    /// cut short an idle park and have the sample that answers it run once the
    /// tree holds still instead of at the next backstop tick. Without it the
    /// edge only sets a flag that nobody looks at until a timer fires, which is
    /// polling wearing a push's clothes.
    ///
    /// - Parameter waker: The loop's waker, shared for its lifetime.
    void setIngestWaker(std::shared_ptr<PAL::Waker> waker)
    {
        PAL::Waker* raw = waker.get();
        m_processTree->wakerOwner = std::move(waker);
        m_processTree->waker.store(raw, std::memory_order_release);
    }

    /// How long to wait before walking a tree that announces nothing.
    ///
    /// Zero where the kernel raises fork/exec/exit edges: the edge is the
    /// signal, and a wall-clock walk beside it would be a second mechanism for
    /// a fact the first one already delivers. Non-zero where it does not, since
    /// a title whose only other signal is output stands still under a program
    /// that prints nothing.
    ///
    /// - Returns: Milliseconds between fallback walks, or zero for none.
    [[nodiscard]] std::uint32_t walkFallbackMilliseconds() const;

    /// A shell builtin can change its directory without a process-tree edge.
    /// Where edges replace the process-walk timer, sample only the directory
    /// on this cadence while a reader observes the pane.
    ///
    /// - Returns: Milliseconds between directory samples, or zero for none.
    [[nodiscard]] std::uint32_t directorySampleMilliseconds() const;

    /// Probes the foreground now, off any drain, and walks the process table
    /// while process state is read.
    ///
    /// The ingest loop's tick, for the case a drain cannot cover: a program
    /// that takes the foreground and writes nothing moves the facts without
    /// producing a byte, so no drain runs and the title would name whatever ran
    /// before it. Must be called on the ingest thread — it touches the same
    /// sample state ``drainAvailable`` does, with no lock between them.
    ///
    /// - Parameter host: The model lock shared with snapshot readers.
    /// - Returns: The process and metadata changes published by the sample.
    [[nodiscard]] DrainOutcome sampleProcessStateOffDrain(DrainHost& host);

    /// Samples only the foreground directory on the ingest thread's quiet
    /// path, without walking the process tree.
    ///
    /// - Parameter host: The model lock shared with snapshot readers.
    /// - Returns: The directory change published by the sample.
    [[nodiscard]] DrainOutcome sampleCurrentDirectoryOffDrain(DrainHost& host);

    /// Folds cold scrollback blocks under the model lock, off any drain.
    ///
    /// The ingest loop's quiet-path work: a pass that drained nothing is the
    /// moment the store is not being appended to, and one block is bounded
    /// work (`LineStore::foldColdBlocks`), so a flood's append path never
    /// meets it and a reader waits on the lock for one block at most.
    ///
    /// - Parameters:
    ///   - host: The model lock shared with snapshot readers.
    ///   - blockBudget: How many blocks this call may fold.
    /// - Returns: Whether a cold raw block remains, so the loop can wake for it.
    [[nodiscard]] bool foldColdScrollback(DrainHost& host, int blockBudget);

    /// Installs an already-open stream, consuming ownership only on success.
    ///
    /// A null stream is refused without changing the active stream or running
    /// installation hooks. A nonnull stream explicitly replaces the active owner.
    ///
    /// - Parameter stream: The endpoint to own, returned intact on refusal.
    /// - Returns: Success, or the exact refused owner; null is never installation.
    [[nodiscard]] std::expected<void, std::unique_ptr<PAL::Stream>> adoptStream(std::unique_ptr<PAL::Stream> stream);

    /// Subscribes to the stream's fork/exec/exit edge, so a tree that changed
    /// under a steady foreground still re-walks.
    ///
    /// Called at every site that installs a stream, immediately after the
    /// assignment: registration must never depend on another path running
    /// afterwards.
    void attachProcessTreeNotifier();
    /// Hands the stream the grid it serves the moment it is installed: the
    /// size a frontend published (with its pixels), or the grid's own
    /// dimensions before any was published. Called by both installers.
    void handStreamItsWindowSize();

    // Idempotent. Disposes the Ubiquitty::Terminal and closes the owned stream (teardown order:
    // clients must clear their borrow before this).
    void close();

    // --- host output → model (TERM-WRITER) ---------------------------------
    /// Pumps one already-read chunk into Core, stopping at the next DECSET-2026 sync clear.
    ///
    /// The caller loops over the remainder and publishes a snapshot at each
    /// `reachedSyncClear` boundary, which is what lets a frontend show completed
    /// frames rather than mid-update ones. Core's replies queue in the model
    /// and drain to the stream at reply-pressure stops (handled here: the
    /// parse yields, the queue flushes, the caller just feeds the remainder —
    /// such a stop returns with `reachedSyncClear` false) and at the end-of-
    /// pass bookkeeping; per-call flushing was measured off the flood path on
    /// purpose. TERM-WRITER.
    ///
    /// - Parameter bytes: The chunk, which need not end on a sequence boundary.
    /// - Returns: How much was consumed and whether a sync clear was reached.
    FeedResult feedChunkUpToSyncClear(std::span<const char> bytes);

    // Portable ingest drain: read all available host bytes and pump them into Core
    // up to each DECSET-2026 boundary, then flush replies + resolve the bell — all
    // under host.lockModel(), publishing each completed sync frame via
    // host.onSyncFrameReadyLocked(). outcome.reachedEnd => stop.
    DrainOutcome drainAvailable(DrainHost&);

    // A drain pass self-limits at this many bytes then yields. Public so a frontend
    // can recognize a capacity-limited pass — the flood signature.
    static constexpr int drainPassCapacityBytes = 1024 * 1024;

    /// Reports whether a drain pass reads again after a read that returned bytes.
    ///
    /// A pass ends at a would-block or at `drainPassCapacityBytes`, never at a
    /// short read. A PTY hands over at most its line discipline's buffer per
    /// read, about 1 KiB on macOS and 4 KiB on Linux, so a read shorter than the
    /// caller's buffer says nothing about whether the writer is still ahead.
    /// Ending the pass there would ask the publish gate part way through every
    /// redraw longer than one read, and no pass would ever reach the capacity.
    ///
    /// - Parameter passBytes: The bytes the pass has read so far.
    /// - Returns: Whether to read again; false means the budget is spent.
    [[nodiscard]] static constexpr bool drainPassReadsAgain(long long passBytes) { return passBytes < drainPassCapacityBytes; }

    // How many bytes the model lock is held across in one acquisition, on every
    // parse path (the sequential drain and the platform's flood consumer alike).
    //
    // A hold must stay short enough that a reader waiting on the model lock, the
    // renderer's snapshot among them, gets it between bites rather than after a
    // whole flood pass. The parser is a state machine that resumes cleanly at
    // any byte, so releasing between chunks is exactly what a smaller pty read
    // would have done; frame publishes are unaffected (they key off the parser's
    // own sync-clear signal, not the chunk boundary). Much smaller bites stop
    // amortising the acquisition itself.
    static constexpr int modelLockBiteBytes = 4 * 1024;

    // What the parse paths actually use. A shipping build folds this to the
    // constant; a test build reads UBIQUITTY_MODEL_LOCK_BITE (bytes, clamped to
    // [256, one read]) once, so the contention sweep can walk the hold/latency
    // trade-off without a rebuild per point.
    static int effectiveModelLockBite();

    /// Closes a pass whose bytes the caller fed through feedChunkUpToSyncClear.
    ///
    /// Performs drainAvailable's end-of-pass bookkeeping on the writer thread.
    /// A zero-byte WouldBlock reports a pause; a budget boundary does not.
    ///
    /// - Parameters:
    ///   - host: The owner of the model lock and frame publication hook.
    ///   - bytesDrained: Bytes consumed since the previous pass boundary.
    ///   - end: Why this pass stopped consuming input.
    /// - Returns: The pass's events and snapshot summary.
    DrainOutcome finishExternalDrainPass(DrainHost& host, int bytesDrained, DrainPassEnd end);

    /// Starts the pending frame's age before parsing a paced drain.
    ///
    /// Writer thread only, with the model lock held; an earlier dirty epoch is
    /// retained.
    void noteDrainInput();

    /// Takes the bells this terminal's route counted that no drain reported,
    /// for a feed that parses outside a drain pass.
    ///
    /// - Returns: How many bells.
    [[nodiscard]] std::uint32_t takeBellsRung() { return m_bellRoute.take(); }

    /// Feeds bytes that originate locally through the model path host output takes.
    ///
    /// Echo and pasted-then-echoed text go through here so their ordering against
    /// host output is the model's, not the caller's. TERM-WRITER.
    ///
    /// - Parameter bytes: The bytes to feed.
    void feedHostBytes(std::span<const char> bytes);

    // Drain Core's reply buffer to the stream. Internal step of the feed calls;
    // exposed for explicit flush points. TERM-WRITER (reply ordering).
    void flushReplies();

    /// Completes an OSC 52 clipboard read once the prompt policy has decided.
    ///
    /// Frames the reply, or suppresses it on a denial, and flushes to the stream.
    /// TERM-WRITER.
    ///
    /// - Parameters:
    ///   - setSpec: The selection specification the request named.
    ///   - decision: Whether to read the clipboard or reply empty.
    void completeClipboardRead(std::string_view setSpec, Ubiquitty::OSC::ClipboardReadDecision decision);

    // --- input → host (stream-only) ----------------------------------------
    /// Writes input bytes to the host, bypassing the model.
    ///
    /// Keystrokes and pastes reach the host this way; what returns from the host
    /// is what reaches the screen, so nothing is echoed here.
    ///
    /// - Parameter bytes: The bytes to send.
    void sendInputBytes(std::span<const char> bytes);
    [[nodiscard]] bool hasPendingWrite() const;
    bool drainPendingWrite();
    // The stream's pollable file descriptor, or -1. The platform's ingest thread polls it.
    [[nodiscard]] int streamFileDescriptor() const;
    /// Whether a host stream is attached -- the PTY a launch opened, or one
    /// handed in -- and so whether ``sendInputBytes`` reaches anything.
    [[nodiscard]] bool hasHostStream() const;

    /// Resizes the grid. TERM-WRITER.
    ///
    /// Split from ``setWindowSize`` so the bridge keeps the proven ordering: this
    /// runs first, under the model lock, and the ioctl runs after it without one.
    ///
    /// - Parameters:
    ///   - rows: The new row count.
    ///   - columns: The new column count.
    ///   - reflow: The foreground-is-shell gate the bridge computes; never decided
    ///     here.
    void resizeGrid(int rows, int columns, Ubiquitty::Modes::ReflowMode reflow);

    /// Resizes the grid, choosing when the history rewrap is paid for.
    ///
    /// - Parameters:
    ///   - rows: The new row count.
    ///   - columns: The new column count.
    ///   - reflow: Whether history rewraps at all.
    ///   - urgency: `Interactive` defers a width step's rewrap to the settle.
    ///   - anchor: Which edge of the grid stays put.
    void resizeGrid(int rows, int columns, Ubiquitty::Modes::ReflowMode reflow, Ubiquitty::Modes::ResizeUrgency urgency,
        Ubiquitty::Modes::AnchorMode anchor);
    // Stream-only. TIOCSWINSZ — run after resizeGrid, off the model lock.
    // A grid change is applied at once. A change of pixels alone is applied
    // when the interval since the last applied size has passed, otherwise
    // held, and the ingest loop applies the newest when due — so a resize
    // animation costs the program one SIGWINCH per grid, never one per pixel.
    void setWindowSize(int columns, int rows, int xpixel, int ypixel);

    /// Returns the number of window-size deliveries to the session stream.
    [[nodiscard]] std::uint64_t windowSizeDeliveryCount() const { return m_windowSizeDeliveryCount.load(std::memory_order_relaxed); }
    /// Whether a window size is held for the ingest loop to apply.
    [[nodiscard]] bool hasPendingWindowSize() const { return m_isWindowSizePending.load(std::memory_order_acquire); }
    /// Milliseconds until the held window size is due; 0 when due or none is held.
    [[nodiscard]] std::uint32_t pendingWindowSizeDueInMillis(std::uint32_t nowMicros) const;
    /// Applies the held window size if its interval has passed. Ingest thread.
    ///
    /// - Parameter nowMicros: The monotonic time, in microseconds.
    void applyPendingWindowSizeIfDue(std::uint32_t nowMicros);

    /// Re-sends the window size with a SIGWINCH when one is due after a
    /// takeover. Ingest thread.
    ///
    /// The schedule is ``WindowSizeReassert``'s: a program that took the
    /// terminal from the shell hears its size again four times in its first
    /// second, so a size change that landed before its handler stood still
    /// reaches it. The size is the one the frontend last published.
    ///
    /// - Parameter nowMicros: The monotonic time, in microseconds.
    void reassertWindowSizeIfDue(std::uint32_t nowMicros);
    /// Milliseconds until the next re-send is due, zero when one is due now,
    /// and nothing while no takeover's window is open. Ingest thread.
    [[nodiscard]] std::optional<std::uint32_t> windowSizeReassertDueInMillis(std::uint32_t nowMicros);

    /// Whether a present should wait for a whole-screen erase's redraw: the
    /// drain that parsed the erase is stamped, and the hold lasts until the
    /// program's bytes have been quiet for the settle or the watchdog runs
    /// out. Releasing consumes the hold; the next erase re-arms it.
    ///
    /// The settle stays shorter than a display frame during a resize too,
    /// so successive redraws can reach the display. Further erases during a
    /// hold keep its original watchdog deadline.
    [[nodiscard]] bool shouldWithholdPresentationAfterErase(std::uint32_t nowMicros);
    /// Microseconds until the erase hold can release on its own; 0 when none.
    [[nodiscard]] std::uint32_t eraseHoldMicrosRemaining(std::uint32_t nowMicros) const;

    /// Returns whether the redraw behind the newest whole-screen erase is
    /// still arriving: the parser has applied the erase, and the drain has not
    /// yet caught up with the bytes the program wrote after it.
    ///
    /// The live-resize capture's hold. A resize asks a full-screen program for
    /// a repaint on every step, so it waits for no quiet output: the hold
    /// lifts as soon as the redraw has been parsed whole, and the settle and
    /// the watchdog bound it for bytes that never catch up. Its watchdog runs
    /// from the first ask that finds a redraw arriving, and a redraw parsed
    /// whole ends it, so a later erase is not held against an earlier one's
    /// deadline. The ordinary erase hold is left as it is. The caller holds
    /// the model lock.
    ///
    /// - Parameters:
    ///   - terminal: The model the capture reads.
    ///   - nowMicros: Now, in monotonic microseconds.
    /// - Returns: True while the capture would show the erased screen.
    [[nodiscard]] bool isEraseRedrawArriving(const Ubiquitty::Terminal& terminal, std::uint32_t nowMicros);

    /// Records the redraw behind the newest whole-screen erase as parsed
    /// whole: the drain has caught up with its program, or the stream has
    /// ended. Caller holds the model lock.
    void noteEraseRedrawParsed();

    /// Returns the newest whole-screen erase's generation, which a feed reads
    /// before it parses so that ``noteFeedParsed(_:)`` can tell whether it
    /// erased the screen itself. Caller holds the model lock.
    ///
    /// - Returns: The model's erase generation, or 0 without a model.
    [[nodiscard]] std::uint32_t eraseGeneration() const;

    /// Records a feed's redraw as parsed whole when the feed itself erased the
    /// whole screen and no redraw from the stream was arriving before it.
    ///
    /// Fed bytes arrive whole, but a feed that only echoes a keystroke must
    /// not mark a program's redraw still on its way as parsed. Caller holds
    /// the model lock.
    ///
    /// - Parameter eraseGenerationBefore: ``eraseGeneration()`` before the feed.
    void noteFeedParsed(std::uint32_t eraseGenerationBefore);

    /// Reads the password prompt the program's output has left at the cursor,
    /// for a pause in that output the caller has observed.
    ///
    /// A runtime with a stream reads the line discipline from the stream's
    /// process; a streamless one falls back to the cursor row. One ioctl per
    /// pause, so a feed calls it once for the whole write. Caller holds the
    /// model lock.
    void settlePasswordPromptAtPause();

    /// Sets whether the frontend is in a live resize.
    ///
    /// While it is, a whole-screen erase whose redraw is still arriving keeps
    /// the drain from taking a frame of it: an elapsed time budget is not
    /// granted to that pass, and no capture is published for the boundary
    /// route, which presents ahead of the live capture's hold. Outside a live
    /// resize neither changes. Any thread.
    ///
    /// - Parameter resize: Whether a live resize is in progress.
    void setLiveResize(LiveResize resize) { m_isLiveResizeActive.store(resize == LiveResize::Active, std::memory_order_relaxed); }

    /// Returns what the frontend last said of its live resize. Any thread.
    ///
    /// - Returns: The state `setLiveResize` last stored.
    [[nodiscard]] LiveResize liveResize() const
    {
        return m_isLiveResizeActive.load(std::memory_order_relaxed) ? LiveResize::Active : LiveResize::Idle;
    }

    /// Returns whether a present now would show a frame the program asked to
    /// withhold.
    ///
    /// The one statement of the hold every frontend presents under: a
    /// synchronized-output block is open within its budget, or a whole-screen
    /// erase's redraw is still arriving. Asking stamps a block on first sight
    /// and expires it, so the caller holds whatever lock guards `terminal`. A
    /// frontend that withholds must ask again after
    /// ``presentationHoldTicksRemaining``, since an application can open a
    /// block and go quiet.
    ///
    /// - Parameters:
    ///   - terminal: The terminal presented, or null when no block can be open.
    ///   - nowTicks: Now, on the 60 Hz monotonic clock.
    ///   - nowMicros: Now, in monotonic microseconds.
    /// - Returns: True while the present should wait.
    [[nodiscard]] bool shouldWithholdPresentation(Ubiquitty::Terminal* terminal, Ticks nowTicks, std::uint32_t nowMicros);

    /// Returns the ticks until a withheld present may release on its own.
    ///
    /// The block's budget or the erase hold's settle or watchdog, whichever
    /// ends first.
    ///
    /// - Parameters:
    ///   - terminal: The terminal presented, or null when no block can be open.
    ///   - nowTicks: Now, on the 60 Hz monotonic clock.
    ///   - nowMicros: Now, in monotonic microseconds.
    /// - Returns: At least 1 while anything is held, 0 otherwise.
    [[nodiscard]] std::uint32_t presentationHoldTicksRemaining(const Ubiquitty::Terminal* terminal, Ticks nowTicks, std::uint32_t nowMicros) const;

    /// What a drain's settle did with its frame, as a frontend that drains and
    /// paints on one thread reports it.
    enum class SettlePaint : std::uint8_t {
        /// The drain handed the host a frame, which the host painted.
        Painted,
        /// The drain handed the host nothing, so it owes the frontend a paint.
        Owed,
    };

    /// Records a drain for a frontend that drains and paints on one thread.
    ///
    /// A drain whose settle announced a frame was painted by the host's
    /// `onSyncFrameReadyLocked`; one that did not owes the frontend a paint,
    /// made once ``takeOwedPaint`` allows it.
    ///
    /// - Parameter paint: What the drain's settle did with its frame.
    void noteDrainForOwnPaint(SettlePaint paint) { m_isPaintOwed = paint == SettlePaint::Owed; }

    /// Returns whether a frontend that drains and paints on one thread paints now.
    ///
    /// True when a drain owes a paint and nothing withholds it; the owed paint
    /// is then consumed, so a drain is painted once, by its settle or here. A
    /// frame the coherence rule holds withholds it: that frame is painted when
    /// ``publishHeldFrameIfDue`` releases it.
    ///
    /// - Parameters:
    ///   - nowTicks: Now, on the 60 Hz monotonic clock.
    ///   - nowMicros: Now, in monotonic microseconds.
    /// - Returns: Whether to paint.
    [[nodiscard]] bool takeOwedPaint(Ticks nowTicks, std::uint32_t nowMicros);

    /// Returns how long that frontend may wait before asking again.
    ///
    /// - Parameters:
    ///   - nowTicks: Now, on the 60 Hz monotonic clock.
    ///   - nowMicros: Now, in monotonic microseconds.
    /// - Returns: Milliseconds until a withheld paint may release, or -1 when
    ///   none is owed, or the coherence rule holds the frame and its own wait
    ///   (from ``publishHeldFrameIfDue``) wakes the loop.
    [[nodiscard]] int owedPaintWaitMilliseconds(Ticks nowTicks, std::uint32_t nowMicros) const;

    /// Records a focus change. TERM-WRITER. Gaining the focus is a person
    /// looking at the pane, which clears its unseen attention
    /// (`markAttentionSeen`).
    ///
    /// - Parameter focus: The new state.
    void setFocused(Ubiquitty::Modes::FocusState focus);

    // --- commands (main -> writer) -----------------------------------------
    // Apply one command main posted to the Terminal. TERM-WRITER (caller holds the
    // lock); the no-ingest-loop fallback path calls it inline.
    void applyCommand(const Command&);
    // Drain + apply every command queued in link().commands under the host's
    // lock. The ingest loop calls this each iteration; no-op + no lock when empty.
    // Returns true when at least one queued command was consumed, giving the
    // platform a cheap gate for reply/snapshot flush work.
    bool applyPendingCommands(DrainHost&);

    // --- reads -------------------------------------------------------------
    [[nodiscard]] DimensionSnapshot dimensions() const; // field copy; cheap

    // True while a drain pass is actively pumping host bytes into this pane's
    // Terminal. The render reads it to hold the last complete frame instead of
    // sampling a half-drawn redraw mid-burst (non-2026 flicker).
    [[nodiscard]] bool isDrainPassActive() const { return m_drainPassActive.load(std::memory_order_relaxed); }
    void setDrainPassActive(bool active) { m_drainPassActive.store(active, std::memory_order_relaxed); }
    /// Whether the last drain pass filled its capacity — the writer is ahead
    /// and the next pass will fill too. Each such pass publishes its frame
    /// through the host (`SyncFrameReason::SaturatedPass`), so a render side
    /// that finds nothing published is between two captures and has nothing
    /// to gain from the model but a wait on the lock the parser holds. Falls
    /// with the first pass that closes short, and with the end of the stream.
    [[nodiscard]] bool isDrainSaturated() const { return m_isDrainSaturated.load(std::memory_order_relaxed); }
    /// Pushes the pane's presentation state into the drain gate.
    ///
    /// One of the two platform facts behind the ride/park decision, pushed by
    /// the frontend on its presentation edges. The fold lives here so every
    /// ingest adapter shares one policy: only a presented, unthrottled pane
    /// rides mid-burst EAGAIN gaps (spin + sched_yield); anything else ends
    /// the pass on the first gap so the poll() loop parks the ingest thread
    /// at 0 CPU rather than busy-wait a core. The defaults presume a
    /// presented, full-rate pane — also what a frontend that never pushes an
    /// edge gets: the ride-the-burst throughput path.
    ///
    /// - Parameter presentation: The pane's presentation state.
    void setPresentation(PanePresentation presentation)
    {
        m_isPresented.store(presentation == PanePresentation::Presented, std::memory_order_relaxed);
        recomputeDrainRidesBurst();
    }

    /// Pushes the machine-wide load lever into the drain gate.
    ///
    /// The throttle side of the fold ``setPresentation`` describes: a
    /// throttled machine must not busy-wait a core even on the foreground
    /// pane.
    ///
    /// - Parameter throttle: The machine's throttle state.
    void setMachineThrottle(MachineThrottle throttle)
    {
        m_isThrottled.store(throttle == MachineThrottle::Throttled, std::memory_order_relaxed);
        recomputeDrainRidesBurst();
    }
    [[nodiscard]] bool drainRidesBurst() const { return m_drainRidesBurst.load(std::memory_order_relaxed); }
    /// Why the drain arrived at a publish, which decides how it is paced.
    enum class SettleContext : std::uint8_t {
        /// A pass that closed short: the writer is keeping up.
        CaughtUp,
        /// A pass that filled its capacity: the writer is ahead and the next
        /// pass will fill too.
        SaturatedPass,
        /// The reader yielded for presentation while more input may remain.
        ContinuingPass,
        /// The burst is over and the writer has stopped, so this publish is the
        /// last one until more bytes arrive.
        BurstEnd,
    };
    /// Announces the pane's frame when the coherence rule admits it.
    ///
    /// The rule is `FrameCoherence`'s, the one the host daemon publishes by: the
    /// parser at ground, no DEC 2026 block or cursor-hidden redraw open, the
    /// read quiescent, or the deadline passed. A refusal leaves the frame due;
    /// `publishHeldFrameIfDue` asks again when the rule says it may answer
    /// differently. Caller holds the model lock.
    ///
    /// - Parameters:
    ///   - host: The drain host whose publish hook announces the frame.
    ///   - context: How the drain arrived here, which decides the reason the
    ///     frame is announced with.
    void publishSettledFrameIfCoherent(DrainHost& host, SettleContext context);

    // Sample the per-pane read-state into `out` + bump its version. TERM-WRITER;
    // the writer calls it each drain and publishes link().reads.
    void captureSnapshot(RuntimeSnapshot& out);

    // Capture + publish the read-snapshot now, for a Ubiquitty::Terminal written without a
    // drain pass (the tmux render-only pane, whose bytes bypass drainAvailable so its
    // modes would stay stale). Caller must hold the model lock.
    void republishSnapshot();

    // Publish a read-snapshot captured from an EXTERNAL Ubiquitty::Terminal this runtime does
    // not own (the tmux render adapter: the Session owns the pane Terminal). Caller
    // holds that Terminal's model lock.
    void republishSnapshotForTerm(Ubiquitty::Terminal* externalTerm);

    /// Keeps the search publication in every subsequent read snapshot.
    /// The caller holds the presented terminal's model lock.
    ///
    /// - Parameter status: The search's current facts.
    void setSearchStatus(const Search::Status& status) { m_searchStatus = status; }

    /// Observes completed model publications under the presented model lock.
    /// The callback must not acquire another lock or mutate the model.
    using ModelPublishHook = void (*)(void*, const Ubiquitty::Terminal&);

    /// Installs or clears a publication observer. Caller holds the model lock;
    /// clearing it ends every borrow of context before that lock is released.
    /// - Parameters:
    ///   - hook: The observer, or null.
    ///   - context: Borrowed until the observer is cleared.
    void setModelPublishHook(ModelPublishHook hook, void* context)
    {
        m_modelPublishHook = hook;
        m_modelPublishContext = context;
    }

    // The per-pane cross-thread seam (RuntimeLink.h): the writer publishes the
    // read-snapshot here under the lock; main reads it lock-free.
    RuntimeLink& link() { return m_link; }
    [[nodiscard]] const RuntimeLink& link() const { return m_link; }

    // Non-const Ubiquitty::Terminal borrow for the renderer. Safety is the caller's lock
    // bracket, not constness (the renderer writes Terminal: palette write-back).
    // ObjC++-only; never reachable from Swift.
    Ubiquitty::Terminal* borrowTermForRender();
    // Commit any open scroll-lane run before a render model read (the flood
    // pipeline's bite returns leave runs open; paint state is stale until the
    // flush). Call under the model lock.
    void flushScrollLaneForModelRead();
    PAL::Stream* stream(); // ObjC++-only; never reachable from Swift

    /// Stamps host-output activity for the presentation-pacing policy.
    ///
    /// Called by the drain itself, so the policy measures the output rather
    /// than the deliveries, which a burst coalesces into clumps far apart
    /// enough to read as idle. Also called by a frontend with its own reason
    /// to keep painting.
    void notePresentationActivity() { m_frameDriver.noteActivity(presentationSeconds()); }

    /// Whether frames are being driven on a clock right now.
    [[nodiscard]] bool isPresentationContinuous() const { return m_frameDriver.active(); }

    /// Whether host output has been idle past the driver's timeout.
    [[nodiscard]] bool isPresentationIdleExpired() const { return m_frameDriver.idleExpired(presentationSeconds()); }

    /// Publishes how the frontend is driving frames.
    ///
    /// - Parameter mode: What the frontend is doing now.
    void setPresentationMode(FrameMode mode) { m_frameDriver.setMode(mode); }

    /// Enables the screen-change-rate recommendation on the presentation driver.
    /// Off, isPresentationRecommendedContinuous is always true.
    ///
    /// - Parameter drive: On to recommend a rate, Off to hold Continuous.
    void setAdaptivePresentation(AdaptiveDrive drive) { m_frameDriver.setAdaptiveDrive(drive); }

    /// Whether adaptive presentation is enabled.
    [[nodiscard]] bool isAdaptivePresentation() const { return m_frameDriver.isAdaptiveDrive(); }

    /// Feeds the presentation driver the resolved panel frame interval.
    ///
    /// - Parameter seconds: The interval (1/rate); a non-positive value is ignored.
    void setPresentationFrameIntervalSeconds(double seconds) { m_frameDriver.setFrameIntervalSeconds(seconds); }

    /// Stamps a cell-writing drain for the adaptive rate estimate. Ingest
    /// thread; a no-op unless adaptive presentation is enabled.
    ///
    /// - Parameter isCapacityLimited: Whether the drain exhausted its byte budget.
    void notePresentationContentDrain(bool isCapacityLimited) // bool-ok: publishes the drain’s byte-budget exhaustion flag.
    {
        if (m_frameDriver.isAdaptiveDrive())
            m_frameDriver.noteContentDrain(adaptiveSeconds(), isCapacityLimited);
    }

    /// Stamps a rendered frame that changed pixels. Render thread; a no-op
    /// unless adaptive presentation is enabled.
    void notePresentationNetChange()
    {
        if (m_frameDriver.isAdaptiveDrive())
            m_frameDriver.noteNetChange(adaptiveSeconds());
    }

    /// The adaptive drain streak, for a debug readout.
    [[nodiscard]] int presentationFastDrainStreak() const { return m_frameDriver.fastDrainStreak(); }

    /// The frame interval the adaptive estimate measures against; 0 until sourced.
    [[nodiscard]] double presentationFrameIntervalSeconds() const { return m_frameDriver.frameIntervalSeconds(); }

    /// Whether the screen-change rate warrants continuous drive as of now. Always
    /// true when adaptive presentation is off.
    [[nodiscard]] bool isPresentationRecommendedContinuous() const
    {
        return m_frameDriver.recommendedMode(adaptiveSeconds()) == FrameMode::Continuous;
    }

    /// Whether the foreground process was a shell at the last probe.
    ///
    /// The drain's cached answer rather than a fresh one: the probe costs a
    /// walk of the process table, and the callers that read this — the
    /// resize's reflow decision above all — run per drag step. A shell is
    /// assumed before the first probe, which is the answer that rewraps.
    [[nodiscard]] bool isForegroundShell() const { return m_cachedForegroundIsShell; }

    // --- events ------------------------------------------------------------
    void setEventSink(EventSink* sink); // inline, coalesced — see TerminalEvents.h

    // Raw host-byte tap (ingest thread). When set, drainAvailable calls it with each
    // chunk JUST READ, before Core parses it (the ZMODEM auto-transfer detector's
    // seam). nullptr (default) disables it. The fn-ptr+context form keeps the runtime
    // free of std::function.
    using RawByteTap = void (*)(void* context, std::string_view bytes);
    void setRawByteTap(RawByteTap tap, void* context)
    {
        m_rawByteTap = tap;
        m_rawByteTapContext = context;
    }

    // Outbound byte tap (Stream Debug). When set, sendInputBytes and
    // flushReplies call it with each chunk WRITTEN to the stream.
    // Mirrors RawByteTap shape. nullptr (default) disables it.
    using OutboundByteTap = void (*)(void* context, std::string_view bytes);
    /// Installs the outbound byte tap for Stream Debug.
    ///
    /// The tap observes every chunk written to the stream (user input +
    /// replies), on whichever thread writes (ingest thread for replies,
    /// main for input). The fn-ptr+context form keeps the runtime free
    /// of std::function. nullptr (default) disables it.
    ///
    /// - Parameter tap: The tap, or null to disarm.
    /// - Parameter context: Handed back to the tap verbatim.
    void setOutboundByteTap(OutboundByteTap tap, void* context)
    {
        m_outboundByteTap = tap;
        m_outboundByteTapContext = context;
    }

    // Where a session with no stream of its own sends what it writes to the host.
    using ReplySink = void (*)(void* context, std::string_view bytes);
    /// Installs where a streamless session's replies and input go.
    ///
    /// An embedder that feeds host bytes in itself, as an SSH carrier does,
    /// owns the way back too: the DA, DSR and colour reports a parse leaves
    /// queued are handed to the sink, on the thread that fed the bytes, rather
    /// than held for a stream that does not exist. Input takes the same way out
    /// on the thread that sends it — keys, pastes, and the button, wheel and
    /// alternate-scroll bytes the pointer ladder encodes, which no frontend
    /// sees and so no frontend could forward. A session with a stream writes
    /// both there and never calls the sink.
    ///
    /// - Parameters:
    ///   - sink: The sink, or null to disarm.
    ///   - context: Handed back to the sink verbatim.
    void setReplySink(ReplySink sink, void* context)
    {
        m_replySink = sink;
        m_replySinkContext = context;
    }

    /// Whether a sink is installed for a streamless session's writes.
    [[nodiscard]] bool hasReplySink() const { return m_replySink != nullptr; }

    /// Hands a streamless session's queued replies to its reply sink.
    ///
    /// Does nothing for a session with a stream, whose replies its own drain
    /// writes, or with no sink installed. TERM-WRITER.
    void flushRepliesToSink();

    /// Transfers queued replies to the stream, retaining refused bytes for retry.
    ///
    /// - Parameters:
    ///   - term: The terminal whose queued output is drained; the caller holds
    ///     its model lock.
    ///   - stream: Where the bytes go.
    ///   - tap: Sees accepted bytes exactly once, or null.
    ///   - tapContext: Handed to `tap` with each chunk.
    static void flushQueuedOutput(Terminal& term, PAL::Stream& stream, OutboundByteTap tap = nullptr, void* tapContext = nullptr);

    /// Watches the host stream for a file-transfer preamble.
    ///
    /// The watch is Core's rather than a frontend's because the bytes are, and
    /// because a frontend that owns the detector is the only frontend that has
    /// the feature: the same stream reaches macOS, Linux and the web build
    /// through here. A hit disarms the watch, so a continuing frame stream
    /// cannot re-fire it while the frontend sets a transfer up, and raises
    /// ``EventKind::FileTransferDetected`` for the frontend to answer.
    ///
    /// Idempotent: arming an armed watch restates it and forgets a partial
    /// match, which is what resuming after a transfer wants.
    void armFileTransferWatch()
    {
        m_fileTransferDetector.reset();
        m_detectedTransfer = {};
        // Arming lifts the hold a detection set. Every answer to a detection —
        // ran the agent, declined the dialog, no agent installed — funnels
        // through a re-arm, so this is the one release point a frontend cannot
        // forget; a hold with no release parks ingest for the pane's life.
        m_isFileTransferHoldActive.store(false, std::memory_order_relaxed);
        m_isFileTransferWatchArmed.store(true, std::memory_order_relaxed);
    }

    /// Stops the watch until it is armed again.
    void disarmFileTransferWatch() { m_isFileTransferWatchArmed.store(false, std::memory_order_relaxed); }

    /// Whether the watch would judge the next byte.
    [[nodiscard]] bool isFileTransferWatchArmed() const { return m_isFileTransferWatchArmed.load(std::memory_order_relaxed); }

    /// The detection the last event announced, or a falsy one.
    ///
    /// Read after ``EventKind::FileTransferDetected``; the event carries the
    /// protocol and direction as scalars, and this is the same answer for a
    /// caller that would rather have the type.
    ///
    /// - Returns: What fired, or a falsy Detection when nothing has.
    [[nodiscard]] Ubiquitty::Network::Transfer::Detection detectedFileTransfer() const { return m_detectedTransfer; }

    /// Whether a file-transfer hold is active — the frontend has taken the
    /// stream and the ingest loop must stop reading so the transfer frame
    /// stays in the kernel queue for the agent.
    [[nodiscard]] bool isFileTransferHoldActive() const { return m_isFileTransferHoldActive.load(std::memory_order_relaxed); }

    /// Lifts the hold set by a detected preamble. Called by the frontend
    /// after the transfer agent completes (success or failure). Idempotent.
    void releaseFileTransferHold() { m_isFileTransferHoldActive.store(false, std::memory_order_relaxed); }

    /// Whether the launch is still holding the child's output for the screen
    /// the window opens with; see ``OpeningScreen``.
    [[nodiscard]] bool isOpeningHoldActive() const { return m_isOpeningHoldActive.load(std::memory_order_relaxed); }

    /// Lifts the opening hold, so the child's queued output parses after
    /// whatever the frontend fed while it stood. Idempotent.
    void releaseOpeningHold() { m_isOpeningHoldActive.store(false, std::memory_order_relaxed); }

    /// Whether the ingest loop must leave the stream unread: a transfer agent
    /// owns it, or the opening screen has not been written yet. Either way
    /// the bytes stay in the kernel queue rather than reaching the model.
    [[nodiscard]] bool isIngestHeld() const { return !isLocalStreamEnabled() || isFileTransferHoldActive() || isOpeningHoldActive(); }

    /// Feeds one raw chunk to the tap, for an ingest path that pumps its own
    /// bytes (the flood pipeline's ring bites bypass drainAvailable's read, so
    /// they must bring the tap along or the detector goes blind mid-burst).
    /// Ingest thread, off the model lock — the same seam drainAvailable uses.
    ///
    /// - Parameter bytes: The chunk, before Core parses it.
    /// - Returns: The count of leading `bytes` the caller may safely parse
    ///   (`-1` when nothing matched): when a preamble fires, everything from
    ///   this offset on is the transfer frame — parse only the prefix and
    ///   drop the rest (both detected protocols retransmit their lead-in
    ///   until answered, so the transfer agent reads a fresh frame, not the
    ///   swallowed one; a write-back would risk blocking the ingest thread
    ///   on a full kernel queue). Zero means the preamble began in an earlier
    ///   chunk; the whole chunk is frame.
    int tapRawBytes(std::span<const char> bytes)
    {
        if (m_rawByteTap && !bytes.empty())
            m_rawByteTap(m_rawByteTapContext, { bytes.data(), bytes.size() });
        return judgeFileTransferPreamble({ bytes.data(), bytes.size() });
    }

private:
    /// Stamps the erase hold for a whole-screen erase not yet seen.
    ///
    /// Caller holds the model lock.
    ///
    /// - Parameters:
    ///   - terminal: The model whose erase generation is read.
    ///   - nowMicros: The stamp, in monotonic microseconds.
    void armEraseHoldIfUnseen(const Ubiquitty::Terminal& terminal, std::uint32_t nowMicros);

    enum class LocalStreamOwnership : std::uint8_t {
        Local,
        Frozen,
        Daemon,
    };
    enum class LocalAdoptionError : std::uint8_t {
        Unavailable,
        PendingWrite,
    };

    // Only TerminalSession enters this state, after reserving an owning escrow
    // reference and joining every reader. A standalone Runtime cannot acquire
    // an unresolved master whose owner could disappear at scope exit.
    [[nodiscard]] std::expected<int, LocalAdoptionError> freezeLocalStreamForAdoption();
    void confirmLocalStreamOwnership();
    void confirmDaemonStreamOwnership();

    /// Returns the coherence rule's clock, now.
    [[nodiscard]] std::uint64_t coherenceNow() const;

    /// Counts a frame the rule admitted, notes it taken, and publishes the
    /// rule's dirty bit. Caller holds the model lock.
    ///
    /// - Parameters:
    ///   - frameClass: The class the rule admitted it in.
    ///   - nowMicroseconds: When it was taken, on the rule's clock.
    void takeAdmittedFrame(FrameClass frameClass, std::uint64_t nowMicroseconds);

    /// Publishes the DECUDK table when `term` has one.
    ///
    /// Every ModeSnapshot that leaves the runtime, published on the link or
    /// carried in a DrainOutcome, is preceded by this: a frontend that caches
    /// the outcome's modes and reads the table off the link at key time must
    /// never hold a count the table has not caught up with.
    ///
    /// - Parameter term: The terminal whose table goes out; one with no table
    ///   publishes nothing.
    void publishUserDefinedKeys(const Ubiquitty::Terminal& term);

    /// Publishes `snap` on the link, preceded by the DECUDK table when `term`
    /// has one, and by its colours when no synchronized update is open.
    ///
    /// The order is the invariant the side channel rests on: the table goes
    /// out before the snapshot, so a reader that sees a nonzero
    /// `userDefinedKeyCount` finds a table at least that new. A terminal that
    /// has taken no DECUDK has no table, and publishes nothing there. Caller
    /// holds the model lock.
    ///
    /// - Parameters:
    ///   - snap: The snapshot to publish.
    ///   - term: The terminal whose table and colours ride ahead of it;
    ///     nullptr publishes the snapshot alone.
    void publishReads(const RuntimeSnapshot& snap, Ubiquitty::Terminal* term);

    /// Publishes a terminal's colours as the ones its frame shows.
    ///
    /// Caller holds the model lock, and no synchronized update is open: the
    /// grid draws the model as it stands.
    ///
    /// - Parameter term: The terminal.
    void publishCoherentColors(const Ubiquitty::Terminal& term);

    /// Counts a DECSET 2026 close the parser stopped at, and publishes the
    /// colours of the frame it completes.
    ///
    /// At the close rather than at the pass's end: a program writing updates
    /// back to back has opened the next one by then, and a pass ending inside
    /// every update would publish no colours for as long as it keeps writing.
    /// Writer thread, under the model lock.
    void noteSyncClearParsed();

    void emit(EventKind, std::uint32_t data0 = 0);

    /// Derives the drain gate from the two facts.
    ///
    /// Relaxed everywhere: the gate is advisory pacing, and a pass that reads
    /// one store late spins at most one extra budget.
    void recomputeDrainRidesBurst()
    {
        m_drainRidesBurst.store(m_isPresented.load(std::memory_order_relaxed) && !m_isThrottled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // End-of-pass bookkeeping shared by drainAvailable and
    // finishExternalDrainPass: probes, replies, bell, mouse guard, snapshot.
    void finishDrainPass(DrainHost&, DrainOutcome&);
    void recordSessionEnd(DrainHost&, DrainOutcome&);

    /// The terminal's clock: the instant its last bytes arrived, ticked before
    /// they are parsed, so a command's marks are timed by when its bytes were
    /// read and never by another terminal's clock.
    ///
    /// Atomic because the sequential pass ticks it off the model lock while a
    /// frontend's own feed may parse under it on another thread.
    class ReadClock final : public TerminalClock {
    public:
        /// Moves the clock to the instant the next bytes arrived.
        ///
        /// - Parameter instant: The arrival on the 60 Hz clock.
        void tick(Ticks instant) { m_now.store(instant.count, std::memory_order_relaxed); }

        /// Returns the instant the bytes being parsed arrived.
        ///
        /// - Returns: The last tick, or zero before the first bytes.
        Ticks now() override { return Ticks(m_now.load(std::memory_order_relaxed)); }

    private:
        std::atomic<std::uint32_t> m_now { 0 };
    };

    /// The terminal's effect route: counts the bells its program rang until a
    /// drain reports them, so a bell belongs to the pane that rang it.
    class BellRoute final : public EffectRoute {
    public:
        /// Counts one bell, saturating rather than wrapping.
        void ringBell() override
        {
            std::uint32_t bells = m_bells.load(std::memory_order_relaxed);
            while (bells != UINT32_MAX && !m_bells.compare_exchange_weak(bells, bells + 1, std::memory_order_relaxed)) {
            }
        }

        /// Takes the bells rung since the last take.
        ///
        /// - Returns: How many.
        std::uint32_t take() { return m_bells.exchange(0, std::memory_order_relaxed); }

    private:
        std::atomic<std::uint32_t> m_bells { 0 };
    };

    /// Installs on the terminal the clients this runtime owns: the route its
    /// bells ring and the clock its parse reads.
    ///
    /// - Parameter term: The terminal ``init`` made.
    void installTerminalClients(Ubiquitty::Terminal& term);

    /// Ticks the terminal's clock to now, before bytes that just arrived are
    /// parsed.
    void tickTerminalClock();

    // Declared before `m_term`: the Terminal holds CheckedPtrs to both, so they
    // must outlive it (members destroy in reverse order).
    ReadClock m_readClock;
    BellRoute m_bellRoute;
    TermHolder m_term;
    // The session's byte stream — OWNED (RAII). launch()/adoptStream() install it,
    // close()/dtor reset it. PAL::Stream is incomplete here — the dtor lives in the
    // .cpp where Stream.h completes it for unique_ptr's deleter.
    std::unique_ptr<PAL::Stream> m_stream;
    std::atomic<LocalStreamOwnership> m_localStreamOwnership { LocalStreamOwnership::Local };
    // The last window size a frontend published, replayed onto a stream adopted
    // after it: a window that opens at its grid publishes once, before or after
    // the spawn, and the PTY must hear it either way.
    bool m_hasWindowSize = false;
    int m_windowColumns = 0;
    int m_windowRows = 0;
    int m_windowXPixels = 0;
    int m_windowYPixels = 0;
    // The coalescing: the remembered size is written by a frontend thread
    // and read by the ingest thread's deferred apply, so the mutex guards it;
    // the flag and the stamp are read without it.
    std::atomic<std::uint64_t> m_windowSizeDeliveryCount { 0 };
    std::mutex m_windowSizeMutex;
    std::atomic<bool> m_isWindowSizePending { false };
    std::atomic<std::uint32_t> m_windowSizeAppliedMicros { 0 };
    // When the size is re-sent after a takeover. Read and written on the
    // ingest thread alone: the process sample opens it and the loop's tick
    // consumes it.
    WindowSizeReassert m_windowSizeReassert;
    // The erase hold: the ingest thread stamps, a frontend asks.
    // The ingest thread stamps after its pass and a capture under the model
    // lock stamps an erase the pass has not reached yet (armEraseHoldIfUnseen).
    std::atomic<std::uint32_t> m_seenEraseGeneration { 0 };
    std::atomic<std::uint32_t> m_eraseHoldStartMicros { 0 };
    // The erase whose redraw has been parsed whole: stamped under the model
    // lock when a drain pass catches up with its program, the stream ends, or
    // a feed carries its own erase.
    std::atomic<std::uint32_t> m_parsedEraseGeneration { 0 };
    // When the live-resize hold first found a redraw arriving; 0 when none
    // is, and cleared by every redraw parsed whole.
    std::atomic<std::uint32_t> m_redrawArrivingSinceMicros { 0 };
    std::atomic<bool> m_isLiveResizeActive { false };
    bool m_isPaintOwed = false; // single-threaded frontends only; see noteDrainForOwnPaint
    std::atomic<std::uint32_t> m_lastBytesMicros { 0 };
    // Resolved from LaunchCommand::sessionLifetime() rather than stored as the
    // enum: LaunchCommand stays a forward declaration here, and this header
    // does not take on Runtime/LaunchCommand.h's dependencies for one bit.
    bool m_doesSessionEndWithChild = false;
    // Raw host-byte tap (ZMODEM detect) — see setRawByteTap. nullptr = off.
    /// Feeds the armed watch one raw chunk, before Core parses it.
    ///
    /// Inline and arm-first so an unarmed runtime pays one relaxed load a chunk:
    /// the detector's own reject is cheap, but it is not free, and a session
    /// mid-transfer has already handed the stream away.
    ///
    /// - Parameter bytes: The chunk, as the tap sees it.
    /// - Returns: The count of leading `bytes` safe to parse, or `-1` when
    ///   nothing matched (see ``tapRawBytes``).
    int judgeFileTransferPreamble(std::string_view bytes)
    {
        if (!m_isFileTransferWatchArmed.load(std::memory_order_relaxed) || bytes.empty())
            return -1;
        const Ubiquitty::Network::Transfer::Detection detection = m_fileTransferDetector.feed(bytes);
        if (!detection) [[likely]]
            return -1;
        // Disarmed at the hit: the frontend takes the stream from here, and a
        // continuing preamble must not fire a second time under it.
        m_isFileTransferWatchArmed.store(false, std::memory_order_relaxed);
        // Hold: stop the ingest loop so the frame stays off the model and in
        // the kernel queue for the transfer agent to read.
        m_isFileTransferHoldActive.store(true, std::memory_order_relaxed);
        m_detectedTransfer = detection;
        // Protocol in the low byte, direction above it: the sink carries one
        // scalar, and a frontend that wants the pair unpacked reads
        // detectedFileTransfer() instead.
        emit(EventKind::FileTransferDetected,
            static_cast<std::uint32_t>(detection.protocol)
                | (static_cast<std::uint32_t>(detection.direction) << 8));
        // What precedes the preamble in this chunk is ordinary output — the
        // command echo, an rz banner — and parsing exactly that much keeps it
        // on screen while the frame stays off it. A preamble that began in an
        // earlier chunk answers zero: its head already parsed, and the few
        // lead-in bytes it printed are what any terminal shows for a ZMODEM
        // start.
        return std::max(0, m_fileTransferDetector.lastMatchOffsetInFeed());
    }

    // Ingest-thread state, read once per chunk: inline beside the flag so the
    // armed check and the detector's carry share the line the chunk already
    // brought in.
    std::atomic<bool> m_isFileTransferWatchArmed { false };
    std::atomic<bool> m_isFileTransferHoldActive { false };
    std::atomic<bool> m_isOpeningHoldActive { false };
    Ubiquitty::Network::Transfer::Detector m_fileTransferDetector;
    Ubiquitty::Network::Transfer::Detection m_detectedTransfer {};

    RawByteTap m_rawByteTap = nullptr;
    void* m_rawByteTapContext = nullptr;
    OutboundByteTap m_outboundByteTap = nullptr;
    void* m_outboundByteTapContext = nullptr;
    // The poll loop reads this outside the model lock to retry refused replies.
    std::atomic<bool> m_hasDeferredReplies { false };
    ReplySink m_replySink = nullptr;
    void* m_replySinkContext = nullptr;
    // Non-owning back-pointer to the bridge's sink — checked (Safer-CPP law).
    CheckedPtr<EventSink> m_sink;
    std::uint32_t m_generation = 0; // snapshot version / staleness counter
    std::uint32_t m_titleGeneration = 0; // bumps per consumed title edge; stamped into every published snapshot
    // The OSC 9;4 generation this runtime has already reported. Core bumps the
    // terminal's counter per accepted report; comparing here is what turns a
    // monotone counter into the one edge a frontend can act on.
    std::uint32_t m_progressGenerationSeen = 0;
    /// The OSC 21337 generation this runtime has already reported, the same
    /// shape and for the same reason as the progress counter above it.
    std::uint32_t m_tabStatusGenerationSeen = 0;
    RuntimeLink m_link; // per-pane cross-thread seam: publish reads + drain commands
    Search::Status m_searchStatus;
    ModelPublishHook m_modelPublishHook = nullptr;
    void* m_modelPublishContext = nullptr;
    std::atomic<bool> m_drainPassActive { false }; // ingest pumping bytes; render holds
    std::atomic<bool> m_isDrainSaturated { false }; // the last pass filled; render reads the published frame
    std::atomic<bool> m_drainRidesBurst { true }; // false => collapse mid-burst backoff to 0
    std::atomic<bool> m_isPresented { true }; // presume presented until the frontend pushes an edge
    std::atomic<bool> m_isThrottled { false };
    int m_lastFocusApplied = -1; // SetFocus edge-gate: -1 unset, else last 0/1 applied
    // Stale-mouse guard: the foreground process group that held the tty when mouse
    // tracking switched on. If the shell returns to the foreground under a DIFFERENT
    // group while tracking is still on, the enabler died without DECRST (ssh dropped
    // mid-tmux) and the drain clears the tracking modes itself. 0 = nothing recorded.
    int m_mouseTrackingProcessGroup = 0;
    // Rate limit for the foreground-group probes: tcgetpgrp contends on the kernel
    // tty lock the writer holds during floods. Cached values reused between samples;
    // consumers need ~20 Hz freshness at most.
    Ticks m_lastForegroundProbeTicks {};
    int m_cachedForegroundGroup = 0;
    bool m_cachedForegroundIsShell = true;
    FrameDriver m_frameDriver;

    /// The clock the pacing policy measures in, in seconds.
    ///
    /// Core's own hook rather than a platform call, and zero where no
    /// frontend installed one — which reads as idle forever and leaves such
    /// a build on demand rather than driving frames it cannot pace.
    /// Seconds on a monotonic clock with microsecond resolution, for the
    /// adaptive stamps: the 60 Hz presentation clock quantizes to 16.7 ms,
    /// wider than the keeping-pace window at 120 Hz.
    [[nodiscard]] static double adaptiveSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] static double presentationSeconds()
    {
        const PAL::MonotonicClockHook clock = PAL::monotonicClock();
        return clock ? static_cast<double>(clock().count) / 60.0 : 0.0;
    }
    bool m_foregroundProbeValid = false;
    std::string m_cachedForegroundDirectory;
    std::string m_cachedForegroundName;
    /// Whether the reading after the last sample's found the same group,
    /// holder and name; see `sampleProcessState`.
    bool m_isForegroundSampleSettled = true;
    // Written by the ingest thread, read wherever a title is composed. These
    // are std::strings rewritten in place, so an unguarded reader does not get
    // a stale answer — it walks a buffer the writer may have freed. The lock is
    // held only across the assignments, never across the walk that produces
    // them, so a title read waits on a copy and not on the process table.
    mutable std::mutex m_processFactsMutex;
    Session::ProcessState m_processState;
    // Kept from the walk that built the chain above, so the subtree has one
    // reader-visible answer rather than one per asker.
    std::vector<PAL::Process::Record> m_processRecords;
    // The probe's foreground answer as of the sample above, so a reader keyed
    // on the sample's event reads the same instant the event announced.
    bool m_isSampledForegroundShell = true;
    // Bumped by the walk that moves the records above, read lock-free by a
    // view keyed on the sample.
    std::atomic<std::uint64_t> m_processRecordsGeneration { 0 };
    std::string m_shellCommandName;
    std::string m_leaderTTYName;
    bool m_isForegroundControlModeTmux = false;
    bool m_didProcessStateMove = false;
    // Written on the ingest thread at end-of-file, read wherever a title or a
    // caret is decided. See sessionEnd().
    std::atomic<bool> m_sessionDidEnd { false };
    std::atomic<bool> m_sessionExitKnown { false };
    std::atomic<bool> m_sessionExitWasSignal { false };
    std::atomic<int> m_sessionExitCode { 0 };
    // The foreground group the chain was last walked for, and how many
    // processes shared it. Both feed the rewalk gate: a `$(...)` transient in
    // its own job group is why the count is part of the question.
    uint32_t m_chainProcessGroup = 0;
    int m_chainForegroundCount = 0;
    Ticks m_lastProcessWalkTicks {};
    bool m_hasProcessWalk = false;
    // Raised by the platform's fork/exec/exit watcher, cleared by the sample
    // that answers it. Shared rather than owned so an edge still in flight
    // during teardown has something live to write to: the watcher is joined
    // when the stream closes, and the stream is not the last thing to go.
    std::shared_ptr<ProcessTreeSignal> m_processTree { std::make_shared<ProcessTreeSignal>() };

    // The walk's bound, shared with the host's walk of a hosted pane.
    static constexpr std::size_t processChainWalkCapacity = Session::processChainWalkCapacity;

    /// What the frontend reads of the foreground, as it last said.
    [[nodiscard]] ForegroundObservation foregroundObservation() const { return m_processTree->observation.load(std::memory_order_acquire); }

    /// Walks the process table and folds the result into the session's state.
    ///
    /// Runs on the ingest thread only: the drain calls it on its probe and the
    /// loop's idle tick calls it when no drain will, and the two share this
    /// state with no lock between them.
    [[nodiscard]] bool hasNeverProbedForegroundFacts() const;
    [[nodiscard]] bool isMouseTrackingActive() const;
    [[nodiscard]] bool hasForegroundProbeBudgetElapsed(Ticks now) const;
    [[nodiscard]] bool shouldProbeForegroundFacts(Ticks now, bool isCaughtUp) const; // bool-ok: the drain's own reading of whether it caught up, not a mode this chooses
    [[nodiscard]] bool descendingWalkIsCheapEnoughToAlwaysRun() const;
    // Both arguments are facts this frontend reports rather than choices it makes: the
    // kernel either named a new process or delivered a tree edge. An enum would rename
    // a reading without removing a decision.
    [[nodiscard]] bool foregroundChainCouldHaveMoved(std::uint32_t foregroundProcessGroup, bool didNameChange, bool didTreeChange) const; // bool-ok: kernel readings, not caller decisions
    /// Samples the session's process facts, settling the probe's reading of
    /// the foreground.
    ///
    /// - Parameter probedName: The name the probe read beside the holder it
    ///   cached, from the same reading of the foreground's owner.
    void sampleProcessState(std::string probedName);
    /// Reads the foreground's group and holder into the probe's cache.
    ///
    /// - Returns: The holder's name, read with the holder.
    [[nodiscard]] std::string probeForegroundHolder();
    /// Clears the owed foreground probe before the probe reads the foreground.
    ///
    /// An edge raises the flag after the state it reports has moved, so a flag
    /// cleared ahead of the read leaves any later edge raised for the next
    /// probe; one cleared after the read would erase it.
    void takeForegroundProbeOwed();
    // True when the previous drain saw any mouse-tracking mode on: the guard
    // can act then, so the next drain's probe skips the tick budget.
    bool m_mouseTrackingActiveLastDrain = false;
    // True when a posted command mutated the model since the last end-of-pass
    // bookkeeping ran: the confirming zero-byte pass after a burst has nothing
    // of its own to flush or publish, and this is the one other producer whose
    // effects the bookkeeping must not skip. Writer-thread only.
    bool m_didCommandsTouchModel = false;
    // Grid content generation at the last finished drain, for the
    // DrainOutcome::screenContentChanged compare. Writer-thread only.
    std::uint64_t m_lastDrainContentGeneration = 0;
    // When the pass snapshot last published, for the flood-pass throttle: a
    // capacity-limited pass completes hundreds of times a second, far past
    // what any snapshot reader consumes, while a settled pass must publish
    // regardless so the final state is never held back. Writer-thread only.
    std::uint32_t m_lastSnapshotPublishMicros = 0;
    std::atomic<bool> m_hasUnpublishedSnapshot { false };
    // Whether this drain pass closed a DECSET-2026 block. A completed sync frame
    // can reach the settle publish rather than a mid-chunk boundary fire — when
    // the ESU is the last byte of the read, the mid-chunk gate (consumed <
    // remaining) never triggers and the frame settles like ordinary output. The
    // settle publish reads this to tell a real 2026 frame (which must be
    // captured) from plain output (which the render can take down its own cheap
    // path), and clears it on read. Writer-thread only.
    bool m_didDrainSeeSyncClear = false;
    // Where a 2026 frame is lost between the parser and the publish, countable
    // from outside: the parser reporting a clear, and the settle refusing to
    // publish one because the frame was not coherent.
    std::uint64_t m_syncClearSeenCount = 0;
    std::uint64_t m_settlePublishRefusedCount = 0;
    /// The palette epoch of the colours last published on the link, which a
    /// close compares before it reads and publishes them again. Under the
    /// model lock.
    std::uint64_t m_coherentColorsEpoch = 0;
    // When the pane's frame may be announced: the rule the daemon publishes
    // by. Pacing is the drain host's (PacedDrainHost throttles to the
    // display's rate), so the rule paces no flood of its own here. Writer-thread
    // only, like the passes it is fed.
    FrameCoherence m_coherence { CoherenceTuning { .isPaced = false } };
    std::array<std::uint64_t, frameClassCount> m_frameClassCounts {};
    std::optional<FrameClass> m_lastFrameClass;
    std::uint64_t m_lastFrameLagMicroseconds = 0;
    // The rule's dirty bit, for a render on another thread; written after
    // every change to the rule.
    std::atomic<bool> m_isFrameHeld { false };
    CoherenceClock m_coherenceClock = nullptr;
    void* m_coherenceClockContext = nullptr;
    // Declared in every build and counted only in the hooked one, so the
    // class keeps one layout across translation units built with and without
    // the flag.
    [[maybe_unused]] std::uint64_t m_fullSnapshotCaptureCountForTest = 0;
};

} // namespace Ubiquitty::Runtime
