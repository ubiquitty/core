// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/TerminalRuntime.h"

#include "Core/App/ProgramActivity.h"
#include "Core/CellColor.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/Modes/Focus.h"
#include "Core/Modes/Scroll.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/Mouse/Hilite.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/LinkColor.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Runtime/DrainHostPacing.h"
#include "Core/Runtime/DrainPass.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/ShellForeground.h"
#include "Core/Session/WalkedProcessFacts.h"
#include "Core/Tek/Tek4014.h"
#include "Core/Title/ProcessIcons.h"
#include "Core/Title/Truncate.h"
#include "Core/VT/ParseState.h"
#include <span>
#include <string_view>

#include "Core/CellGrid.h"
#include "Core/OSC/Dispatch.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/TerminalOperations.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"

#include "Core/Mouse/Mouse.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace Ubiquitty::Runtime {

namespace {

// 3 ticks at the 60 Hz PAL clock ≈ 50 ms, ~20 Hz worst case.
constexpr std::uint32_t foregroundProbeIntervalTicks = 3;
// A pixel-only window size within this of the last applied one is held for
// the ingest loop. A grid change is never held: a program redraws for the
// grid it was told, and a redraw for a width the grid no longer has wraps at
// the old width, every row after the first shifted right by one.
constexpr std::uint32_t windowSizeCoalesceMicros = 40000;
// After a whole-screen erase a present waits for the program's bytes to go
// quiet this long — a redraw arrives in bursts closer than this — and never
// longer than the watchdog, so a program that clears and stops still shows
// its blank screen.
constexpr std::uint32_t eraseHoldSettleMicros = 6000;
constexpr std::uint32_t eraseHoldMaxMicros = 50000;

// The microseconds from `then` to `now`, on the wrapping 32-bit clock, and
// zero for a `then` stamped after `now`: the ingest thread stamps bytes and
// erases while a presenter holds a `now` it read a moment earlier, and the
// unsigned difference of the two is a span of seventy minutes, which reads
// as a hold long settled and spent.
[[nodiscard]] constexpr std::uint32_t microsSince(std::uint32_t now, std::uint32_t then)
{
    return static_cast<std::int32_t>(now - then) > 0 ? now - then : 0;
}

void captureDrainState(const Ubiquitty::Terminal&, std::uint32_t version, RuntimeSnapshot&);

// The coherence rule's clock: monotonic microseconds that do not wrap, unlike
// the PAL's 32-bit counter, so a hold is never measured across a wrap.
std::uint64_t coherenceNowMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

TerminalRuntime::TerminalRuntime() = default;

TerminalRuntime::~TerminalRuntime() { close(); }

void TerminalRuntime::installTerminalClients(Ubiquitty::Terminal& term)
{
    term.effectRoute = &m_bellRoute;
    term.tickClock = &m_readClock;
}

void TerminalRuntime::tickTerminalClock()
{
    m_readClock.tick(PAL::monotonicTicks());
}

void TerminalRuntime::init(const RuntimeInitParameters& parameters)
{
    if (m_term)
        return; // idempotent guard
    m_hasDeferredReplies.store(false, std::memory_order_relaxed);
    auto term = std::make_unique<Ubiquitty::Terminal>();
    term->isScrollLaneEnabled = parameters.scrollLaneEnabled;
    term->isBandScrollLaneEnabled = parameters.scrollLaneEnabled;
    // Set before any byte reaches the parser: TextSizing reads it when deciding
    // whether to intern a spec, so a frontend setting it later would refuse the
    // sizes in its own first frame.
    term->drawsSizedText = parameters.drawsSizedText;
    term->drawsProportionalText = parameters.drawsProportionalText;
    Ubiquitty::resize(*term, parameters.rows, parameters.columns, parameters.allowReflowOnInit ? Ubiquitty::Modes::ReflowMode::Reflow : Ubiquitty::Modes::ReflowMode::Fixed);
    installTerminalClients(*term);
    m_term = std::move(term);
    // Publish the initial read-snapshot so dimensions()/snapshot readers are valid
    // before the first drain.
    RuntimeSnapshot initial;
    captureSnapshot(initial);
    publishReads(initial, m_term.get());
}

bool TerminalRuntime::launch(const LaunchCommand& command, const PAL::TerminalIdentity& identity)
{
    if (!isLocalStreamEnabled())
        return false;
    if (m_stream)
        return true;
    // Before the open, so a session whose PTY refused still answers for the
    // launch it was given rather than for the default.
    m_doesSessionEndWithChild = command.sessionLifetime() == SessionLifetime::EndsWithChild;
    // Before the open as well: the hold must stand before the first byte the
    // child writes could be read.
    m_isOpeningHoldActive.store(command.openingScreen() == OpeningScreen::Held, std::memory_order_relaxed);
    // Before the open too: a title composed on the first frame has this to
    // show, where the first walk is a poll interval away and the fallback
    // without it is the application's own name.
    seedTitleName(command.titleSeedName());
    // The runtime that reads a program's stream answers its queries, even
    // on a terminal that last held a host's picture.
    if (m_term)
        m_term->replyAuthority = ReplyAuthority::ThisTerminal;
    m_stream = PAL::openPTY(command.arguments(), { .initialDirectory = command.initialDirectory(), .characterLocalePolicy = command.characterLocalePolicy(), .identity = identity, .argument0Style = command.argument0Style(), .sessionRecord = command.sessionRecord(), .environmentOverrides = command.environmentOverrides() });
    handStreamItsWindowSize();
    attachProcessTreeNotifier();
    return m_stream != nullptr;
}

std::expected<void, std::unique_ptr<PAL::Stream>> TerminalRuntime::adoptStream(std::unique_ptr<PAL::Stream> stream)
{
    if (!stream || !isLocalStreamEnabled())
        return std::unexpected(std::move(stream));
    m_hasProcessWalk = false;
    if (m_term)
        m_term->replyAuthority = ReplyAuthority::ThisTerminal;
    m_stream = std::move(stream);
    handStreamItsWindowSize();
    attachProcessTreeNotifier();
    return {};
}

void TerminalRuntime::handStreamItsWindowSize()
{
    // The size a frontend already published carries its pixels; before any,
    // the grid's own dimensions stand in, so a program never reads a zero
    // winsize and lays out for eighty columns inside a wider window.
    if (!m_stream || !isLocalStreamEnabled())
        return;
    const std::scoped_lock hold(m_windowSizeMutex);
    m_isWindowSizePending.store(false, std::memory_order_release);
    m_windowSizeAppliedMicros.store(PAL::monotonicMicros() | 1u, std::memory_order_relaxed);
    if (m_hasWindowSize) {
        m_stream->setWindowSize(m_windowColumns, m_windowRows, m_windowXPixels, m_windowYPixels);
        m_windowSizeDeliveryCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto grid = dimensions();
    if (grid.rows > 0 && grid.columns > 0) {
        m_stream->setWindowSize(grid.columns, grid.rows, 0, 0);
        m_windowSizeDeliveryCount.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {
// What the watcher's thread is handed: the flag to raise and the loop to wake.
//
// Both are shared owners rather than borrows. The edge arrives on a thread that
// has no way to keep a lifetime rule about either, and waking a waker nobody is
// parked on is harmless — so the safe shape is for the context to keep them
// alive for exactly as long as it can be called.
// Runs on the watcher's OWN thread, so it raises a flag and wakes a loop and
// does nothing else. Raising without waking is what made this notification
// inert: the flag was only ever read by a timer tick, so a push arrived and
// then waited for a poll.
bool processTreeChanged(void* context)
{
    ProcessTreeSignal& signal = **static_cast<std::shared_ptr<ProcessTreeSignal>*>(context);
    signal.edges.fetch_add(1, std::memory_order_relaxed);
    signal.dirty.store(true, std::memory_order_relaxed);
    signal.isForegroundProbeOwed.store(true, std::memory_order_relaxed);
    if (signal.observation.load(std::memory_order_acquire) == ForegroundObservation::Unread) {
        return false;
    }
    if (PAL::Waker* waker = signal.waker.load(std::memory_order_acquire))
        waker->wake();
    return true;
}

// Called exactly once, after the watcher thread is joined, so no edge can be
// in flight when the last shared owners here go.
void releaseProcessTreeContext(void* context) { delete static_cast<std::shared_ptr<ProcessTreeSignal>*>(context); }
} // namespace

std::uint32_t TerminalRuntime::walkFallbackMilliseconds() const
{
    // Out of line because the header only forward-declares the stream, and the
    // answer is the stream's to give.
    if (!m_stream || foregroundObservation() == ForegroundObservation::Unread)
        return 0;
    return m_stream->process().deliversTreeEdges() ? 0 : Title::walkFallbackMilliseconds;
}

std::uint32_t TerminalRuntime::directorySampleMilliseconds() const
{
    if (!m_stream || !isLocalStreamEnabled() || foregroundObservation() == ForegroundObservation::Unread)
        return 0;
    // A shell report already owns the resolved directory. Its next prompt
    // reports any move, so a quiet process-directory probe buys nothing.
    if (m_term && !m_term->reportedDirectory.empty())
        return 0;
    return m_stream->process().deliversTreeEdges() ? Title::walkFallbackMilliseconds : 0;
}

void TerminalRuntime::attachProcessTreeNotifier()
{
    if (!m_stream || !isLocalStreamEnabled())
        return;
    // Core is the only registrant. The tracker holds one callback slot and a
    // second registrant silently replaces the first — a frontend that also
    // registered here would unhook this without any error, and every kernel
    // edge would reach Core zero times. A frontend learns the same fact from the
    // process-state delivery the walk below produces.
    m_stream->process().setTreeChangeNotifier(
        &processTreeChanged, new std::shared_ptr<ProcessTreeSignal>(m_processTree), &releaseProcessTreeContext);

    // A session starts owing one walk, because the child is forked before this
    // runs and the exec that turns `login` into the reader's shell can land in
    // the gap. That edge is gone: nothing raised it into a notifier that did
    // not exist yet. An edge reports a CHANGE, and a session with no initial
    // state has nothing for one to change — so the start is itself the first
    // edge, and the ingest loop answers it on its own settle like any other.
    //
    // Without this the title keeps whatever the launch seeded, forever, on any
    // session quiet after its first prompt: there is no wall-clock walk behind
    // it to notice.
    m_processTree->dirty.store(true, std::memory_order_relaxed);
    m_processTree->isForegroundProbeOwed.store(true, std::memory_order_relaxed);
}

void TerminalRuntime::seedTitleName(const std::string& name)
{
    Session::ProcessEvent seed;
    seed.kind = Session::ProcessEventKind::LaunchSeed;
    seed.text = name;
    seed.tick = PAL::monotonicTicks().count;
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        Session::applyProcessEvent(m_processState, seed);
    }
}

void TerminalRuntime::close()
{
    (void)tryClose();
}

std::expected<void, TerminalRuntime::CloseError> TerminalRuntime::tryClose()
{
    if (hasUnresolvedLocalAdoption())
        return std::unexpected(CloseError::OwnershipUnresolved);
    // Teardown order: the stream first (no more bytes can arrive), then the Terminal.
    // Callers must have cleared any borrowTermForRender() reference.
    m_stream.reset();
    m_term.reset();
    m_sink = nullptr;
    return {};
}

std::expected<int, TerminalRuntime::LocalAdoptionError> TerminalRuntime::freezeLocalStreamForAdoption()
{
    if (!isLocalStreamEnabled() || !m_stream || m_stream->fileDescriptor() < 0 || isFileTransferHoldActive() || isOpeningHoldActive())
        return std::unexpected(LocalAdoptionError::Unavailable);
    if (m_stream->hasPendingWrite())
        return std::unexpected(LocalAdoptionError::PendingWrite);
    // No local destructor may signal this child after a possible Commit. The
    // session's escrow reference prevents ordinary destruction while Frozen.
    m_stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    m_localStreamOwnership.store(LocalStreamOwnership::Frozen, std::memory_order_release);
    return m_stream->fileDescriptor();
}

void TerminalRuntime::confirmLocalStreamOwnership()
{
    if (m_stream)
        m_stream->setProcessClosePolicy(PAL::ProcessClosePolicy::Terminate);
    m_localStreamOwnership.store(LocalStreamOwnership::Local, std::memory_order_release);
}

void TerminalRuntime::confirmDaemonStreamOwnership()
{
    m_localStreamOwnership.store(LocalStreamOwnership::Daemon, std::memory_order_release);
}

FeedResult TerminalRuntime::feedChunkUpToSyncClear(std::span<const char> bytes)
{
    const int length = static_cast<int>(bytes.size());
    FeedResult result;
    if (!isLocalStreamEnabled())
        return result;
    if (!m_term || length <= 0) {
        result.consumed = (length > 0) ? length : 0;
        return result;
    }
    // The flood pipeline reads off this thread and parses here, so this is
    // where its bytes reach the settle clock the erase hold measures against.
    m_lastBytesMicros.store(PAL::monotonicMicros() | 1u, std::memory_order_relaxed);
    tickTerminalClock();
    bool stoppedAtSyncClear = false;
    const int consumed = Ubiquitty::processBytesUpToSyncClear(m_term.get(), std::string_view(bytes.data(), static_cast<std::size_t>(length)), &stoppedAtSyncClear);
#if UBIQUITTY_UI_TESTS
    uiTestBytesUnderModelLock.fetch_add(static_cast<std::uint64_t>((consumed > 0) ? consumed : length), std::memory_order_relaxed);
#endif
    // An ESU that is the span's last byte stops the parser with nothing left
    // over, so the flag, not the consumed count, says a 2026 frame completed.
    result.reachedSyncClear = stoppedAtSyncClear;
    if (stoppedAtSyncClear)
        noteSyncClearParsed();
    if (consumed > 0 && consumed < length) {
        // Stopped early: at reply pressure the queued replies are drained here
        // so the caller's next feed cannot overflow the reply buffer.
        result.consumed = consumed;
        if (!stoppedAtSyncClear)
            flushReplies();
    } else {
        // Consumed everything, or a <=0 guard against spinning.
        result.consumed = length;
    }
    emit(EventKind::ScreenDirty);
    return result;
}

void TerminalRuntime::feedHostBytes(std::span<const char> bytes)
{
    const int length = static_cast<int>(bytes.size());
    if (!m_term || length <= 0 || !isLocalStreamEnabled())
        return;
    tickTerminalClock();
    Ubiquitty::processBytes(m_term.get(), std::string_view(bytes.data(), static_cast<std::size_t>(length))); // local echo path
    flushReplies();
    emit(EventKind::ScreenDirty);
}

void TerminalRuntime::flushQueuedOutput(Terminal& term, PAL::Stream& stream, OutboundByteTap tap, void* tapContext)
{
    struct Writer {
        PAL::Stream& stream;
        OutboundByteTap tap;
        void* context;
    } writer { stream, tap, tapContext };
    Ubiquitty::flushOutput(term, [](void* context, std::span<const char> bytes) {
        auto& writer = *static_cast<Writer*>(context);
        if (writer.stream.writeAll(bytes) < 0)
            return false;
        if (writer.tap)
            writer.tap(writer.context, { bytes.data(), bytes.size() });
        return true; }, &writer);
}

void TerminalRuntime::flushReplies()
{
    if (!m_term || !isLocalStreamEnabled())
        return;
    if (m_stream) {
        flushQueuedOutput(*m_term, *m_stream, m_outboundByteTap, m_outboundByteTapContext);
        m_hasDeferredReplies.store(m_term->outputLength > 0 || m_term->bulkOutput.length > 0, std::memory_order_relaxed);
        return;
    }
    flushRepliesToSink();
}

void TerminalRuntime::flushRepliesToSink()
{
    if (!m_term || m_stream || !m_replySink || !isLocalStreamEnabled())
        return;
    std::array<char, 4096> reply;
    int n;
    while ((n = Ubiquitty::drainOutput(m_term.get(), { reply.data(), reply.size() })) > 0) {
        if (m_outboundByteTap)
            m_outboundByteTap(m_outboundByteTapContext, { reply.data(), static_cast<std::size_t>(n) });
        m_replySink(m_replySinkContext, { reply.data(), static_cast<std::size_t>(n) });
    }
}

int TerminalRuntime::effectiveModelLockBite()
{
#if UBIQUITTY_UI_TESTS
    // Read once: the sweep sets it per process, and this sits on the parse path.
    static const int bite = [] {
        const char* value = std::getenv("UBIQUITTY_MODEL_LOCK_BITE");
        if (value == nullptr)
            return modelLockBiteBytes;
        return std::clamp(std::atoi(value), 256, drainReadBufferBytes);
    }();
    return bite;
#else
    return modelLockBiteBytes;
#endif
}

DrainOutcome TerminalRuntime::drainAvailable(DrainHost& host)
{
    DrainOutcome outcome;
    if (!m_term || !m_stream || !isLocalStreamEnabled())
        return outcome;
    // A held stream is the transfer agent's: a pass here would lift frame
    // bytes off the descriptor the agent is about to read. An opening hold
    // keeps the child's first output queued behind the screen the window
    // opens with. The flood pipeline checks the same flag; this is the
    // sequential pass's half of the rule.
    if (isIngestHeld())
        return outcome;
    m_drainPassActive.store(true, std::memory_order_relaxed);

    // What this runtime adds to the shared read loop: the transfer watch on
    // every read, the clock the erase hold settles against, the 2026 counts,
    // and replies flushed to its own stream.
    struct Owner final : DrainPassOwner {
        explicit Owner(TerminalRuntime& owner, std::uint32_t interval)
            : runtime(owner)
            , presentationInterval(interval)
        {
        }
        int judgeRead(std::span<const char> bytes) override
        {
            runtime.m_lastBytesMicros.store(PAL::monotonicMicros() | 1u, std::memory_order_relaxed);
            runtime.tickTerminalClock();
#if UBIQUITTY_UI_TESTS
            // Counted here as well as in feedChunkUpToSyncClear: only a stream fast enough to fill a whole pass
            // escalates to the flood pipeline, so a counter on that path alone reports 0 MB/s for every
            // slower producer — which reads as "no bytes arrived" rather than "arrived by the other path".
            runtime.uiTestBytesUnderModelLock.fetch_add(static_cast<std::uint64_t>(bytes.size()), std::memory_order_relaxed);
#endif
            // Both read sites judge, or the watch sees a stream that depends on
            // which drain path carried it: this one reads for the sequential
            // pass, tapRawBytes for the flood pipeline's ring bites too. A hit
            // ends the pass: another read would keep consuming the stream with
            // the watch already disarmed, so the frame parses into the model as
            // garbage while its bytes vanish from the descriptor the transfer
            // agent is about to read. The frame bytes this read already lifted
            // are dropped — ZMODEM is receiver-driven after the opener, so the
            // agent's own greeting elicits an immediate resend.
            return runtime.tapRawBytes(bytes);
        }
        void noteParseBegin() override
        {
            if (presentationInterval)
                runtime.noteDrainInput();
        }
        void flushReplies() override { runtime.flushReplies(); }
        void noteSyncClear() override { runtime.noteSyncClearParsed(); }
        void publishMidReadSyncFrame(DrainHost& host) override
        {
            runtime.publishMidReadBoundaryIfCoherent(host);
            // This close has unread bytes behind it. If its capture cleared
            // the dirty epoch, the following bytes start the next one now.
            if (presentationInterval)
                runtime.noteDrainInput();
        }
        TerminalRuntime& runtime;
        std::uint32_t presentationInterval;
    };
    const std::uint32_t presentationInterval = host.drainPassIntervalMicroseconds();
    Owner owner(*this, presentationInterval);
    // Read once per pass so the ride-the-burst policy is consistent across
    // the whole burst. A throttled pane collapses the budgets to 0 (ends on the
    // first gap so the poll() loop parks the ingest thread at 0 CPU).
    const DrainPassResult pass = runDrainPass(*m_term, *m_stream, host, owner,
        { .doesRideBurst = m_drainRidesBurst.load(std::memory_order_relaxed), .modelLockBiteBytes = effectiveModelLockBite(), .maxMicroseconds = presentationInterval });
    outcome.bytesDrained = pass.bytesDrained;
    outcome.end = pass.end;
    outcome.caughtUp = pass.didCatchUp;
    outcome.reachedEnd = pass.didReachEnd;
    outcome.fatal = pass.isFatal;
    outcome.lastError = pass.lastError;

    finishDrainPass(host, outcome);
    m_drainPassActive.store(false, std::memory_order_relaxed);
    return outcome;
}

void TerminalRuntime::noteDrainInput()
{
    if (!m_coherence.isDirty()) {
        m_coherence.noteHostChange(coherenceNow());
        m_isFrameHeld.store(true, std::memory_order_release);
    }
}

DrainOutcome TerminalRuntime::finishExternalDrainPass(DrainHost& host, int bytesDrained, DrainPassEnd end)
{
    DrainOutcome outcome;
    if (!m_term || !m_stream || !isLocalStreamEnabled())
        return outcome;
    outcome.bytesDrained = bytesDrained;
    outcome.end = end;
    // The pipeline closes a pass of zero bytes only when its ring stayed empty
    // across a nap: the writer has paused, which is what a sequential pass's
    // empty read reports as caught up.
    outcome.caughtUp = bytesDrained == 0 && end == DrainPassEnd::WouldBlock;
    finishDrainPass(host, outcome);
    return outcome;
}

void TerminalRuntime::collectSessionExitStatus()
{
    // Defined here rather than in the header because PAL::Stream is only
    // forward-declared there; reaching childExit needs the whole type.
    if (!m_stream || !isLocalStreamEnabled() || m_sessionExitKnown.load(std::memory_order_acquire))
        return;
    m_stream->process().hasRunningChild(); // reaps, so the status below can exist
    const PAL::ChildExit exit = m_stream->childExit();
    if (exit.didExit)
        noteSessionExitStatus(exit.wasSignalled, exit.code);
}

// The session is over: say so now, and push it.
//
// Everything here is knowable from the outcome alone, which is why it runs on
// the ingest thread. The exit STATUS is not — the child is reaped after its
// last replica descriptor closes — so it is not asked for here; the frontend
// collects it on its own thread and hands it back through
// noteSessionExitStatus. The fact is named before the number because a
// title that waited for a status would go on naming a departed shell.
//
// - Parameter outcome: The drain that reached the end; marked as moving the
//   process state so this ride out is the delivery that pushes the change.
void TerminalRuntime::recordSessionEnd(DrainHost& host, DrainOutcome& outcome)
{
    if (m_sessionDidEnd.exchange(true, std::memory_order_acq_rel))
        return; // one end per session; a second end-of-file pass must not re-push

    // Cleared here rather than left to the next walk, because there is no next
    // walk: the ingest loop returns as soon as this outcome is delivered. A
    // title left to a later sample would keep the departed shell's name for as
    // long as the pane stayed open, which is exactly the reported defect.
    m_cachedForegroundName.clear();
    m_chainProcessGroup = 0;
    m_chainForegroundCount = 0;
    Session::ProcessEvent ended;
    ended.kind = Session::ProcessEventKind::PollResult;
    ended.tick = PAL::monotonicTicks().count;
    ended.currentDirectory = m_cachedForegroundDirectory; // the folder outlives the process
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        m_shellCommandName.clear();
        Session::applyProcessEvent(m_processState, ended);
    }

    if (m_term) {
        host.lockModel();
        // A control session the stream ended under ends with it, before the
        // line below reaches a parser still reading tmux's protocol.
        Protocol::Tmux::endOfStream(&m_term->integration.tmux);
        // Said in the terminal, not in the title.
        //
        // A word in a title segment reads as the name of a program, because
        // that is what every other segment in that line is — "~ — exited"
        // announces a program called exited. Here it reads as what it is: the
        // last line of a session, below the output it ends, scrolling and
        // selecting and copying with it. Written through the parser like any
        // other bytes, so every arm renders it with no arm-side work; there is
        // no live content to confuse it with, because nothing follows it.
        Ubiquitty::processBytes(m_term.get(), "\r\n[Process exited]\r\n");
        OSC::finishProgramActivity(*m_term);
        m_term->contexts = {};
        OSC::activityChanged(*m_term);

        // The caret goes with the process. There is nothing left to type into,
        // so a blinking cursor is an invitation the session cannot honour.
        //
        // Done by clearing DECTCEM rather than by adding an "ended" test to
        // each painter: the mode is the one gate every arm already honours,
        // including the two that read it raw instead of going through
        // Cursor::resolve, so one store covers every surface and no arm can be
        // forgotten. It is also true rather than a trick — the cursor really is
        // not visible — and a pane reused for a new session gets it back from
        // the reset that starts one. Cleared after the line above, which would
        // otherwise draw its own caret for a frame.
        m_term->modes.isCursorVisible = false;
        host.unlockModel();
    }

    // The push. Without this the frontend redraws only whatever else this
    // delivery happened to carry, and a session that ended quietly — no final
    // bytes, no bell — would leave every surface showing the process that left.
    m_didProcessStateMove = true;
    outcome.processStateChanged = true;
}

void TerminalRuntime::finishDrainPass(DrainHost& host, DrainOutcome& outcome)
{
    // A zero-byte pass with no posted-command effects is the burst's
    // confirming re-drain: everything below was already done for the bytes by
    // the pass that parsed them, the caller discards a byteless outcome, and
    // a bell or title can only arrive through parsed bytes or a command. The
    // one thing such a pass defers is the stale-mouse probe, which the next
    // real drain performs.
    const bool didCommandsTouchModel = m_didCommandsTouchModel;
    if (outcome.bytesDrained == 0 && !outcome.reachedEnd && !didCommandsTouchModel) {
        if (m_hasDeferredReplies.load(std::memory_order_relaxed)) {
            host.lockModel();
            flushReplies();
            host.unlockModel();
        }
        if (outcome.caughtUp) {
            m_isDrainSaturated.store(false, std::memory_order_relaxed);
            host.lockModel();
            // The program has nothing more to give, so whatever it wrote
            // behind its last whole-screen erase has been parsed, including
            // a redraw whose last bytes ended a pass on its budget.
            noteEraseRedrawParsed();
            // A prompt waits for a person, so a pause is where it is read: the
            // program has stopped, and has had the gap to turn echo off after
            // printing it. The facts are a tty ioctl, so never per bite.
            Ubiquitty::settlePasswordPrompt(*m_term, &m_stream->process());
            // The final bite can fall inside the publish floor. An empty read
            // settles that bite even though it brings no new model mutations.
            if (m_hasUnpublishedSnapshot.load(std::memory_order_acquire))
                publishSettledFrameIfCoherent(host, SettleContext::BurstEnd);
            host.unlockModel();
        }
        return;
    }
    // Stamped where every drain funnels, rather than at the delivery: a burst's
    // deliveries are throttled into clumps, and a pacing policy measuring those
    // reads a flood as an idle terminal between them.
    notePresentationActivity();
    m_didCommandsTouchModel = false;
    if (outcome.reachedEnd)
        recordSessionEnd(host, outcome);

    // Stale-mouse guard samples, off the model lock. The tcgetpgrp probes take the
    // kernel tty lock the writer hammers during a flood, so sampled per drain they
    // throttle the drain itself: while tracking is OFF sample on a tick budget and
    // reuse cached answers; while tracking is ON keep the per-drain probe so a dead
    // enabler is noticed on the very next drain.
    const Ticks probeTicks = PAL::monotonicTicks();
    // A refill gap only warrants the full sample when the foreground group
    // changed. Otherwise the interval bounds discovery within the same group.
    // EOF already retired the session above; sampling stale process facts here
    // could name it again or overwrite the exit notification.
    const bool shouldProbeFacts = !outcome.reachedEnd && shouldProbeForegroundFacts(probeTicks, outcome.caughtUp);
    if (shouldProbeFacts) {
        takeForegroundProbeOwed();
        const bool wasForegroundShell = m_cachedForegroundIsShell;
        std::string probedName = probeForegroundHolder();
        const std::string sampledDirectory = m_stream->process().foregroundCurrentDirectory();
        if (m_term) {
            // The shell has the terminal back: where it marks no prompt,
            // that is where a program's working or blocked record ends.
            if (m_cachedForegroundIsShell && !wasForegroundShell)
                OSC::foregroundReturnedToShell(*m_term);
            // The probe's facts, published beside each other rather than over the
            // shell's report: the sample is a source `resolvePaneDirectory`
            // arbitrates, and the group is what retires a report whose program has
            // exited. Both land before the resolve so one pass settles the answer.
            m_term->foregroundProcessGroup = m_cachedForegroundGroup;
            bool didDirectoryMove = Ubiquitty::retireReportedDirectoryIfStale(*m_term, m_cachedForegroundGroup);
            if (!sampledDirectory.empty() && sampledDirectory != m_cachedForegroundDirectory) {
                m_cachedForegroundDirectory = sampledDirectory;
                didDirectoryMove = Ubiquitty::setSampledDirectoryString(*m_term, sampledDirectory) || didDirectoryMove;
            }
            if (didDirectoryMove)
                m_term->currentDirectoryChanged = true;
            // The same report's host leaves with its program, on the edge that
            // brought it in: that edge is what copies it into the process facts
            // below, where a window tag reads it.
            if (Ubiquitty::retireReportedHostIfStale(*m_term, m_cachedForegroundGroup))
                m_term->currentDirectoryChanged = true;
            // A title outlives its program the same way a directory report does,
            // and is retired on the same probe rather than on a frontend's poll.
            if (Ubiquitty::retireTitleIfStale(*m_term, m_cachedForegroundGroup))
                m_term->titleChanged = true;
        }
        // Outside the Terminal guard above: the process facts a title is made
        // of belong to the session, not to the grid, and a pane whose Terminal
        // has not been built still has a child worth naming.
        sampleProcessState(std::move(probedName));
        outcome.processStateChanged = m_didProcessStateMove;
        m_lastForegroundProbeTicks = probeTicks;
        m_foregroundProbeValid = true;
    } else if (m_term && Ubiquitty::didDrainLeaveAReportUnstamped(*m_term)
        && foregroundObservation() != ForegroundObservation::Unread) {
        // A drain that set a title or reported a directory names their owner
        // now even when the full probe is throttled. Left to the next probe, a
        // program that reported and exited in between would hand its report to
        // the shell, and the report would outlive it. One group read, and only
        // on a drain that reported something.
        Ubiquitty::stampUnstampedReports(*m_term, m_stream->process().foregroundProcessGroup().value_or(0));
    }
    const int foregroundGroup = m_cachedForegroundGroup;
    const bool foregroundIsShell = m_cachedForegroundIsShell;

    // Replies + bell + grid dims read/mutate the Terminal, so under the lock. The
    // bell is consumed (cleared) here, matching the parser's own reply path.
    host.lockModel();
    flushReplies();
    // The writer paused with bytes in this pass: the prompt those bytes may
    // have printed is read now, and again on the confirming empty pass.
    if (outcome.caughtUp || outcome.end == DrainPassEnd::WouldBlock)
        Ubiquitty::settlePasswordPrompt(*m_term, &m_stream->process());
    // A drain that erased the whole screen arms the hold; the query on the
    // presenting thread releases it once the redraw has settled.
    armEraseHoldIfUnseen(*m_term, PAL::monotonicMicros());
    // A pass that ended with nothing left to read has parsed every byte the
    // program wrote behind its last erase, so that redraw is whole.
    if (outcome.end == DrainPassEnd::WouldBlock || outcome.reachedEnd)
        noteEraseRedrawParsed();
    const auto nowTicks = PAL::monotonicTicks();
    outcome.bell = resolveBellEvent(m_term.get(), nowTicks);
    outcome.bellsRung = m_bellRoute.take();
    outcome.bellWantsNotification = (outcome.bell != BellMode::Off) && Ubiquitty::Modes::bellWantsNotification(m_term.get());
    outcome.bellRaisesUrgency = (outcome.bell != BellMode::Off) && m_term->bellRaisesUrgency;
    outcome.popOnBell = (outcome.bell != BellMode::Off) && m_term->popOnBell;
    // The person at a focused pane is looking at it: what this pass latched
    // for whoever looks later, they have seen.
    if (m_lastFocusApplied == 1)
        (void)Ubiquitty::markAttentionSeen(*m_term);
    // Consumed (cleared) like the bell: the drain outcome is this terminal's
    // single title-change consumer, so the frontend hears about an OSC title
    // on the drain that parsed it instead of a poll tick finding it later.
    outcome.titleChanged = m_term->titleChanged;
    m_term->titleChanged = false;
    outcome.fontRequestChanged = m_term->fontRequestChanged;
    m_term->fontRequestChanged = false;
    // The snapshot's title generation counts the same edge the outcome
    // consumes: a poll-side reader compares generations instead of racing
    // the one-shot event bit for the same fact.
    if (outcome.titleChanged)
        ++m_titleGeneration;
    outcome.currentDirectoryChanged = m_term->currentDirectoryChanged;
    m_term->currentDirectoryChanged = false;
    if (outcome.currentDirectoryChanged) {
        emit(EventKind::DirectoryChanged);
        // The same report carried the machine it was made on. It crosses into
        // the process facts here, under their own lock: this runs on the ingest
        // thread and a window tag reads them from the main one.
        const std::scoped_lock factsLock(m_processFactsMutex);
        if (m_processState.reportedHost != m_term->reportedHost) {
            m_processState.reportedHost = m_term->reportedHost;
            outcome.processStateChanged = true;
        }
    }
    // The progress counter is monotone and the report dirties nothing, so the
    // edge is made here: a frontend cannot poll a counter it is never told to
    // look at, and the drain that parsed the report is the moment to say so.
    if (m_term->progressGeneration != m_progressGenerationSeen) {
        m_progressGenerationSeen = m_term->progressGeneration;
        emit(EventKind::ProgressChanged);
    }
    // OSC 21337's counter, the same monotone shape and the same reason: the
    // colour a session sets for itself is meant to be read while nobody is
    // looking at its screen, so the drain that parsed it is the only moment
    // that can say so.
    if (m_term->tabStatusGeneration != m_tabStatusGenerationSeen) {
        m_tabStatusGenerationSeen = m_term->tabStatusGeneration;
        emit(EventKind::TabStatusChanged);
    }
    // Stale-mouse guard: tracking left on by a gone foreground job (ssh dropped while
    // remote tmux had the mouse on) turns every pointer move into junk at the prompt,
    // with nothing left to send DECRST. Once the shell is back in the foreground under
    // a different group with tracking still on, clear the tracking modes locally —
    // before the snapshot below publishes them to the platform's input path.
    const bool isMouseTracking = Ubiquitty::Mouse::tracksPointer(m_term.get()) || m_term->modes.mouseRelativePointerLock;
    m_mouseTrackingActiveLastDrain = isMouseTracking; // next drain probes fresh while on
    (void)Ubiquitty::Mouse::retireTrackingIfStale(*m_term, m_mouseTrackingProcessGroup, foregroundGroup, foregroundIsShell);

    // Capture the per-pane read-state once and publish it lock-free (RuntimeLink);
    // DrainOutcome carries a parallel copy. Mid-flood a capacity-limited pass
    // completes far faster than any snapshot reader consumes, so those passes
    // publish at most at the sync-publish floor; a settled pass, a consumed
    // event, or a command's effects publish regardless — the newest state is
    // never held past the burst.
    const std::uint32_t passMicros = PAL::monotonicMicros();
    const bool isSnapshotDue = outcome.caughtUp || outcome.reachedEnd || didCommandsTouchModel
        || outcome.titleChanged || outcome.currentDirectoryChanged || outcome.fontRequestChanged || outcome.bell != BellMode::Off
        || passMicros - m_lastSnapshotPublishMicros >= syncPublishMinIntervalFloorMicros;
    RuntimeSnapshot snap;
    if (isSnapshotDue) {
        captureSnapshot(snap);
        publishReads(snap, m_term.get());
        m_lastSnapshotPublishMicros = passMicros;
    } else {
        captureDrainState(*m_term, m_generation, snap);
        m_hasUnpublishedSnapshot.store(true, std::memory_order_release);
        // The outcome carries these modes to a frontend that caches them and
        // resolves its keys against the link's table, so the table has to be
        // at least as new as the count leaving here, due snapshot or not. A
        // DECUDK program at the head of a burst longer than one pass is the
        // case: the count is fresh in this outcome, and nothing else would
        // publish the table before the next due snapshot.
        publishUserDefinedKeys(*m_term);
    }
    outcome.columns = snap.columns;
    outcome.rows = snap.rows;
    outcome.scrollbackCount = Ubiquitty::Screen::scrollableRowCount(m_term.get());
    outcome.history = Ubiquitty::Screen::historyFacts(m_term.get());
    outcome.mouseReportsActive = snap.mouseReportsActive;
    outcome.mouseMotionReportsActive = snap.mouseMotionReportsActive;
    outcome.mouseAnyEventActive = snap.mouseAnyEventActive;
    outcome.mouseHiliteActive = snap.mouseHiliteActive;
    outcome.mouseSGRPixelsActive = snap.mouseSGRPixelsActive;
    outcome.modes = snap.modes;
    // Set iff the grid's content generation advanced this drain: that counter
    // moves on a cell mutation, never on a mode / title / cursor write. Ingest
    // thread (sole writer), under the model lock held here.
    const std::uint64_t contentGeneration = m_term && m_term->grid ? m_term->grid->contentGeneration() : m_lastDrainContentGeneration;
    outcome.screenContentChanged = contentGeneration != m_lastDrainContentGeneration;
    m_lastDrainContentGeneration = contentGeneration;
    if (outcome.screenContentChanged)
        notePresentationContentDrain(outcome.bytesDrained >= drainPassCapacityBytes);
    // Non-2026 frame boundary: on settle (caughtUp), publish a complete frame for the
    // render to promote instead of sampling a half-drawn redraw live (fixes flicker in
    // apps that never use synchronized output). publishSettledFrameIfCoherent gates on
    // the parser at ground so a settle mid-sequence never publishes a torn grid.
    // Image apps flood continuously and never catch up, so the caughtUp gate alone
    // starves the publish — publish each drained frame too when placements are live.
    // A pass that filled its capacity is the flood itself: the writer is ahead and
    // will stay ahead, so a render that waits for a settle waits for the burst to
    // end, and one that goes to the model instead waits on this lock. Publishing
    // the pass's frame here, under the lock the drain already holds, is what
    // lets the render side present a flood without ever contending for it.
    const bool isSaturatedPass = outcome.end == DrainPassEnd::ByteBudget;
    const bool isTimedPass = outcome.end == DrainPassEnd::TimeBudget;
    const bool isPendingPass = isTimedPass || outcome.end == DrainPassEnd::OwnerFence || outcome.end == DrainPassEnd::None;
    const SettleContext context = isSaturatedPass ? SettleContext::SaturatedPass
        : isPendingPass                           ? SettleContext::ContinuingPass
                                                  : SettleContext::CaughtUp;
    // The pass boundary is where the rule samples the cursor for a bracket and
    // starts counting quiescence; a pass that parsed nothing changed nothing.
    // During a live resize an elapsed budget is not granted behind a
    // whole-screen erase whose redraw is still arriving: the grid between the
    // two is the erased screen the resize keeps off the glass, so the pass
    // stays pending until the drain catches up. Outside one, a flood that
    // opens with an erase keeps its budgeted frames.
    if (outcome.bytesDrained > 0) {
        const bool isBudgetWithheld = isTimedPass && m_isLiveResizeActive.load(std::memory_order_relaxed)
            && isEraseRedrawArriving(*m_term, PAL::monotonicMicros());
        const PassEnd passEnd = isSaturatedPass ? PassEnd::Capacity
            : isTimedPass && !isBudgetWithheld  ? PassEnd::TimeBudget
            : isPendingPass                     ? PassEnd::Pending
                                                : PassEnd::WouldBlock;
        m_coherence.noteDrainPass(*m_term, coherenceNow(), passEnd);
        m_isFrameHeld.store(m_coherence.isDirty(), std::memory_order_release);
    }
    // A stream that ends on a full pass has no next pass to clear this, and a
    // render side reading it as saturated would wait for a capture that never
    // comes.
    m_isDrainSaturated.store(isSaturatedPass && !outcome.reachedEnd, std::memory_order_relaxed);
    if (outcome.bytesDrained > 0 && (outcome.caughtUp || isSaturatedPass || isTimedPass || Ubiquitty::Image::placementCount(m_term.get()) > 0))
        publishSettledFrameIfCoherent(host, context);
    host.unlockModel();
}

// Caller holds the model lock. The frame is announced only when the coherence
// rule admits it: a partial sequence, an open 2026 block, a cursor-hidden
// redraw or a read that has not gone quiet each belong to an unfinished screen.
// A saturated stream is a flood the rule admits mid-sequence, presenting the
// cells already committed to the model wherever the byte budget cut the next
// escape sequence.
void TerminalRuntime::publishSettledFrameIfCoherent(DrainHost& host, SettleContext context)
{
    if (context == SettleContext::BurstEnd && m_hasUnpublishedSnapshot.load(std::memory_order_acquire)) {
        republishSnapshot();
    }
    const bool isSaturatedPass = context == SettleContext::SaturatedPass;
    const bool isContinuingPass = context == SettleContext::ContinuingPass;
    // Only a byte-budget pass carries the saturation flag. A timed pass
    // captures as continuing input without claiming that its writer paused.
    if (!isSaturatedPass)
        m_isDrainSaturated.store(false, std::memory_order_relaxed);
    if (!m_term) {
        ++m_settlePublishRefusedCount;
        return;
    }
    // The boundary route presents a published capture ahead of the live
    // capture's hold, so during a live resize none is taken of a redraw still
    // arriving behind a whole-screen erase, unless the program closed a
    // synchronized frame around it.
    if (m_isLiveResizeActive.load(std::memory_order_relaxed) && !m_didDrainSeeSyncClear
        && isEraseRedrawArriving(*m_term, PAL::monotonicMicros())) {
        ++m_settlePublishRefusedCount;
        return;
    }
    // A timed pass has not observed read quiescence. The held-frame timer's
    // burst-end ask, in contrast, follows its caller's readiness check.
    const std::uint64_t now = coherenceNow();
    m_coherence.setFramePacing(host.frameIntervalMicroseconds());
    const FrameAdmission admission = m_coherence.admit(*m_term, now, PAL::monotonicTicks(),
        isContinuingPass ? ReadReadiness::Pending : ReadReadiness::Quiet);
    if (!admission.isAdmitted) {
        ++m_settlePublishRefusedCount;
        return;
    }
    takeAdmittedFrame(admission.frameClass, now);
    // A settle that closed a 2026 block is the completion of a synchronized
    // frame — captured, like any boundary. A settle with only plain output is
    // announced but not captured, so the render takes it down the live path.
    // An admitted pass whose writer has not gone quiet needs a capture even
    // below capacity: its next read can reassert the hold before the render.
    const bool wasSyncFrame = m_didDrainSeeSyncClear;
    m_didDrainSeeSyncClear = false;
    const SyncFrameReason reason = wasSyncFrame       ? SyncFrameReason::Boundary
        : isSaturatedPass                             ? SyncFrameReason::SaturatedPass
        : isContinuingPass                            ? SyncFrameReason::ContinuingPass
        : context == SettleContext::BurstEnd          ? SyncFrameReason::BurstEnd
        : admission.frameClass != FrameClass::Settled ? SyncFrameReason::ContinuingPass
                                                      : SyncFrameReason::Settle;
    host.onSyncFrameReadyLocked(reason);
}

std::uint64_t TerminalRuntime::coherenceNow() const
{
    return m_coherenceClock ? m_coherenceClock(m_coherenceClockContext) : coherenceNowMicroseconds();
}

void TerminalRuntime::takeAdmittedFrame(FrameClass frameClass, std::uint64_t nowMicroseconds)
{
    const auto dirtySince = m_coherence.dirtySinceMicroseconds();
    m_lastFrameLagMicroseconds = dirtySince && nowMicroseconds >= *dirtySince ? nowMicroseconds - *dirtySince : 0;
    m_coherence.noteFrameTaken(*m_term, nowMicroseconds);
    m_isFrameHeld.store(false, std::memory_order_release);
    ++m_frameClassCounts[static_cast<std::size_t>(frameClass)];
    m_lastFrameClass = frameClass;
}

void TerminalRuntime::publishMidReadBoundaryIfCoherent(DrainHost& host)
{
    if (!m_term)
        return;
    const std::uint64_t now = coherenceNow();
    m_coherence.noteMidReadClose(*m_term, now);
    m_coherence.setFramePacing(host.frameIntervalMicroseconds());
    // The rest of the read is waiting behind the close.
    const FrameAdmission admission = m_coherence.admit(*m_term, now, PAL::monotonicTicks(), ReadReadiness::Pending);
    if (!admission.isAdmitted) {
        m_isFrameHeld.store(true, std::memory_order_release);
        return;
    }
    takeAdmittedFrame(admission.frameClass, now);
    // This boundary is published; the pass-end settle must not announce it as
    // another.
    m_didDrainSeeSyncClear = false;
    host.onSyncFrameReadyLocked(SyncFrameReason::Boundary);
}

void TerminalRuntime::releaseHeldFrame(DrainHost& host)
{
    host.lockModel();
    if (m_term && m_coherence.isDirty()) {
        const bool isCoherent = Ubiquitty::VT::isParserAtGround(m_term.get()) && !m_term->modes.syncOutput && !m_coherence.isBracketHolding();
        takeAdmittedFrame(isCoherent ? FrameClass::Settled : FrameClass::Forced, coherenceNow());
        host.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    }
    m_isFrameHeld.store(false, std::memory_order_release);
    host.unlockModel();
}

std::optional<std::uint64_t> TerminalRuntime::publishHeldFrameIfDue(DrainHost& host)
{
    host.lockModel();
    std::optional<std::uint64_t> due;
    if (m_term && m_coherence.isDirty()) {
        m_coherence.setFramePacing(host.frameIntervalMicroseconds());
        // This timer is the 2026 block's watchdog while the rule holds a
        // frame: the frontend's withhold answers from the held flag and never
        // reaches the block, and the rule names the block's expiry without
        // running it. So the ask stamps a block it has not seen and expires
        // one that has outlived its budget, and the frame is then asked for on
        // the rule's ordinary schedule.
        const Ticks ticks = PAL::monotonicTicks();
        static_cast<void>(Modes::SyncOutput::shouldWithholdFrame(*m_term, ticks));
        due = m_coherence.microsecondsUntilAsk(*m_term, coherenceNow(), ticks);
        // Released by the clock, not by a pass: the writer has stopped, so
        // this is the burst's last publish.
        if (due && *due == 0) {
            publishSettledFrameIfCoherent(host, SettleContext::BurstEnd);
            due = m_coherence.isDirty() ? m_coherence.microsecondsUntilAsk(*m_term, coherenceNow(), PAL::monotonicTicks()) : std::nullopt;
        }
    }
    host.unlockModel();
    return due;
}

TerminalRuntime::DisplayIdleResult TerminalRuntime::serviceDisplayIdle(DrainHost& host)
{
    DisplayIdleResult result;
    host.lockModel();
    if (m_term && !m_term->mirrorClient) {
        const Ticks now = PAL::monotonicTicks();
        result.didChange = Modes::DisplayIdle::advance(*m_term, now);
        result.remainingTicks = Modes::DisplayIdle::ticksUntilDeadline(*m_term, now);
        if (result.didChange)
            publishDisplayChange(host);
    }
    host.unlockModel();
    return result;
}

void TerminalRuntime::publishDisplayChange(DrainHost& host)
{
    if (!m_term)
        return;
    const bool isCoherent = VT::isParserAtGround(m_term.get()) && !m_term->modes.syncOutput && !m_coherence.isBracketHolding();
    takeAdmittedFrame(isCoherent ? FrameClass::Settled : FrameClass::Forced, coherenceNow());
    republishSnapshot();
    emit(EventKind::ScreenDirty);
    host.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
}

void TerminalRuntime::republishSnapshot()
{
    RuntimeSnapshot snap;
    captureSnapshot(snap);
    publishReads(snap, m_term.get());
}

void TerminalRuntime::republishSnapshotForTerm(Ubiquitty::Terminal* externalTerm)
{
    if (!externalTerm)
        return;
    RuntimeSnapshot snap;
    captureSnapshotFromTerm(externalTerm, m_generation, snap);
    snap.titleGeneration = m_titleGeneration;
    snap.search = m_searchStatus;
    publishReads(snap, externalTerm);
}

namespace {

void captureDrainState(const Ubiquitty::Terminal& term, std::uint32_t version, RuntimeSnapshot& out)
{
    out.version = version; // dimension generation (bumped only on resize)
    out.rows = term.rows;
    out.columns = term.columns;
    // Mouse-tracking state — one bool per hot main-thread accessor.
    out.mouseReportsActive = Ubiquitty::Mouse::tracksPointer(&term);
    out.mouseRelativePointerLockActive = term.modes.mouseRelativePointerLock; // private 7403
    out.mouseMotionReportsActive = term.modes.mouseCellMotion || term.modes.mouseAnyEvent;
    out.mouseAnyEventActive = term.modes.mouseAnyEvent;
    out.mouseHiliteActive = term.modes.mouseHilite;
    out.hiliteAwaiting = term.modes.hiliteAwaiting;
    out.syncHoldActive = term.modes.syncOutput;
    out.isColumnCountHeld = term.modes.columns132;
    out.isEdgeToEdge = Ubiquitty::Rendering::isFrameEdgeToEdge(term);
    out.letterboxColor = term.fullscreenContentDisplay.letterboxColor;
    out.fontRequestGeneration = static_cast<std::uint32_t>(term.fontRequests.generation());
    out.mouseSGRPixelsActive = term.modes.mouseSGRPixels;
    // DEC locator (DECELR) is a pointer grab parallel to the xterm mouse modes,
    // so it rides the same snapshot rather than a second publish path.
    out.locatorActive = term.locatorEnabled;
    // Keyboard / terminal modes the input path reads on every keystroke.
    out.modes = sampleModes(term);
}

void captureSnapshotOnlyState(const Ubiquitty::Terminal& term, RuntimeSnapshot& out)
{
    // Scrollback depth + alternate-scroll gate + cursor-animation inputs.
    out.scrollbackCount = Ubiquitty::Screen::scrollbackCount(&term);
    out.history = Ubiquitty::Screen::historyFacts(&term);
    out.alternateScrollActive = Ubiquitty::Modes::alternateScrollActive(&term);
    out.isTekModeActive = term.isTekModeActive;
    out.isTekGraphicInputActive = Ubiquitty::tekGINActive(term);
    // OSC 22's live shape, read here and not at each frontend: the stack is
    // Core's, and a shell resolving "top, or Default when empty" privately is
    // a second copy of a rule the dispatcher and the pointer ladder share.
    // Every published snapshot carries it; a shape set mid-flood inside the
    // publish floor arrives at the next due publish, bounded and never lost.
    out.isAppPointerShapeSet = Ubiquitty::OSC::isAppPointerShapeSet(term);
    out.appRequestedPointerShape = Ubiquitty::OSC::appRequestedPointerShape(term);
    Ubiquitty::getProgress(&term, &out.progressState, &out.progressValue);
    // The tab status colour rides beside the progress pair, read through the
    // same Core accessor every frontend reads it through, so none can
    // disagree about when a colour is set.
    const std::optional<ColorRGBA> tabStatus = App::tabActivityColor(&term);
    out.isTabStatusColorSet = tabStatus.has_value();
    out.tabStatusRed = tabStatus ? tabStatus->red : 0;
    out.tabStatusGreen = tabStatus ? tabStatus->green : 0;
    out.tabStatusBlue = tabStatus ? tabStatus->blue : 0;
    const std::optional<Themes::StatusColorSlot> tabStatusSlot = App::tabActivitySlot(&term);
    out.tabStatusSlot = tabStatusSlot ? std::to_underlying(*tabStatusSlot) : Themes::noStatusColorSlot;
    const auto* programStatus = term.programStatus.primary();
    App::PaneStatusFacts programFacts {};
    if (programStatus)
        programFacts.programState = programStatus->state;
    out.programStatusKind = programStatus ? std::to_underlying(App::paneStatusKind(programFacts)) : 0;
    out.programBlockKind = programStatus && programStatus->state == Ubiquitty::OSC::ProgramState::Blocked ? std::to_underlying(programStatus->kind) : 0;
    out.programProgress = programStatus && programStatus->state == Ubiquitty::OSC::ProgramState::Working ? static_cast<std::int8_t>(programStatus->progress) : -1;
    out.reportedProcessIcon = Title::processIconOrdinal(Title::processIconReported(term.programStatus));
    const std::string activityLabel = App::programActivityLabel(term.programStatus);
    const std::string_view tabStatusLabel = Title::titleBytePrefix(activityLabel.empty() ? Ubiquitty::tabStatusText(&term) : activityLabel, out.tabStatusText.size());
    out.tabStatusTextLength = static_cast<std::uint8_t>(tabStatusLabel.size());
    std::ranges::copy(tabStatusLabel, out.tabStatusText.begin());
    out.statusHelpDigest = App::statusHelpDigest(App::statusHelpText(term, activityLabel));
    out.isCursorVisible = term.modes.isCursorVisible && term.screens.active != nullptr;
    out.cursorColumn = term.screens.active ? term.screens.active->cursorX : 0;
    out.cursorRow = term.screens.active ? term.screens.active->cursorY : 0;
    out.cursorStyle = static_cast<uint8_t>(term.cursorStyle);
    out.cursorBlinkEnabled = term.cursorBlinkPref;
    out.hasBlinkHint = term.grid != nullptr && term.grid->hasBlinkHint();
    // Read only when the hint is armed: a disarmed hint already answers false,
    // and the scan is a pass over the screen that a terminal which has never
    // blinked should not pay for.
    out.hasBlinkCellOnScreen = out.hasBlinkHint && Ubiquitty::Rendering::anyCellBlinks(term);
    out.imagesAnimating = Ubiquitty::Image::anyAnimating(&term);
    // Normalised here rather than at each reader: the palette keeps 16-bit
    // channels and every renderer wants the 0-1 the shaders take.
    constexpr float fullChannel = 65535.0F;
    // The EFFECTIVE default pair, not the palette's raw one: DECSCNM swaps the
    // default foreground and background for the whole frame, and this field
    // exists so a renderer can paint the letterbox and the bare ground — under
    // reverse video that ground is the swapped colour. Applied here for the
    // reason the link colour is resolved here: two arms applying the mode
    // separately disagree exactly when it is on.
    const bool reversed = Ubiquitty::isReverseVideo(&term);
    const Ubiquitty::PaletteRGB& effectiveBackground = reversed ? term.palette.defaultForeground : term.palette.defaultBackground;
    const Ubiquitty::PaletteRGB& effectiveForeground = reversed ? term.palette.defaultBackground : term.palette.defaultForeground;
    out.defaultBackgroundRed = static_cast<float>(effectiveBackground.red) / fullChannel;
    out.defaultBackgroundGreen = static_cast<float>(effectiveBackground.green) / fullChannel;
    out.defaultBackgroundBlue = static_cast<float>(effectiveBackground.blue) / fullChannel;
    // Resolved here rather than per renderer: the choice between blue and
    // bright-blue depends on this palette's own background, so two arms
    // resolving it separately disagree exactly when a theme makes the choice
    // interesting. Eight-bit channels, which is what linkColor returns.
    const auto eightBit = [](const Ubiquitty::PaletteRGB& channel) {
        return Ubiquitty::ColorRGBA { Ubiquitty::channel16To8(channel.red), Ubiquitty::channel16To8(channel.green),
            Ubiquitty::channel16To8(channel.blue), 255 };
    };
    const Ubiquitty::ColorRGBA link = Ubiquitty::Rendering::resolveLinkColor(eightBit(term.palette.indexed[4]),
        eightBit(term.palette.indexed[12]), eightBit(effectiveBackground),
        eightBit(effectiveForeground));
    constexpr float fullByte = 255.0F;
    out.linkColorRed = static_cast<float>(link.red) / fullByte;
    out.linkColorGreen = static_cast<float>(link.green) / fullByte;
    out.linkColorBlue = static_cast<float>(link.blue) / fullByte;
    out.paletteEpoch = term.paletteEpoch;
}

} // namespace

void TerminalRuntime::publishUserDefinedKeys(const Ubiquitty::Terminal& term)
{
    // Only a terminal that has taken a DECUDK carries a table, so the common
    // session publishes nothing here.
    if (term.udk)
        m_link.userDefinedKeys.publish(*term.udk);
}

void TerminalRuntime::publishReads(const RuntimeSnapshot& snap, Ubiquitty::Terminal* term)
{
    if (term && m_modelPublishHook)
        m_modelPublishHook(m_modelPublishContext, *term);
    if (term)
        publishUserDefinedKeys(*term);
    // Inside an update the grid keeps the frame from before it, and the
    // colours stay the ones that frame was drawn in.
    if (term && !term->modes.syncOutput)
        publishCoherentColors(*term);
    m_link.reads.publish(snap);
    m_hasUnpublishedSnapshot.store(false, std::memory_order_release);
}

void TerminalRuntime::publishCoherentColors(const Ubiquitty::Terminal& term)
{
    m_link.coherentColors.publish({ term.paletteEpoch, terminalColors(term) });
    m_coherentColorsEpoch = term.paletteEpoch;
}

void TerminalRuntime::noteSyncClearParsed()
{
    m_didDrainSeeSyncClear = true; // a 2026 frame completed this pass
    ++m_syncClearSeenCount;
    if (m_term && m_term->paletteEpoch != m_coherentColorsEpoch)
        publishCoherentColors(*m_term);
}

void captureSnapshotFromTerm(Ubiquitty::Terminal* term, std::uint32_t version, RuntimeSnapshot& out)
{
    if (!term) {
        return;
    }
    // Snapshot reads paint/placement state: commit any open scroll-lane run
    // first (the pipeline's bite returns leave runs open by design).
    Ubiquitty::termFlushScrollLane(*term);
    captureDrainState(*term, version, out);
    captureSnapshotOnlyState(*term, out);
}

void TerminalRuntime::captureSnapshot(RuntimeSnapshot& out)
{
#if UBIQUITTY_ENABLE_TEST_HOOKS
    ++m_fullSnapshotCaptureCountForTest;
#endif
    captureSnapshotFromTerm(m_term.get(), m_generation, out);
    // Runtime-side state the term cannot carry: the free capture leaves it
    // zero, and only these member sites publish.
    out.titleGeneration = m_titleGeneration;
    out.search = m_searchStatus;
}

void TerminalRuntime::flushScrollLaneForModelRead()
{
    if (m_term)
        Ubiquitty::termFlushScrollLane(*m_term);
}

void TerminalRuntime::completeClipboardRead(std::string_view setSpec, Ubiquitty::OSC::ClipboardReadDecision decision)
{
    if (!m_term || !isLocalStreamEnabled())
        return;
    OSC::completeClipboardReadQuery(*m_term, setSpec, decision);
    flushReplies();
}

void TerminalRuntime::sendInputBytes(std::span<const char> bytes)
{
    if (!isLocalStreamEnabled() || bytes.empty() || (!m_stream && !m_replySink))
        return;
    if (m_outboundByteTap)
        m_outboundByteTap(m_outboundByteTapContext, { bytes.data(), bytes.size() });
    if (!m_stream) {
        m_replySink(m_replySinkContext, { bytes.data(), bytes.size() });
        return;
    }
    m_stream->writeAll(bytes);
}

int TerminalRuntime::streamFileDescriptor() const { return m_stream && isLocalStreamEnabled() ? m_stream->fileDescriptor() : -1; }

bool TerminalRuntime::hasHostStream() const { return m_stream != nullptr && isLocalStreamEnabled(); }

bool TerminalRuntime::hasPendingWrite() const { return m_stream && isLocalStreamEnabled() && (m_stream->hasPendingWrite() || m_hasDeferredReplies.load(std::memory_order_relaxed)); }

bool TerminalRuntime::drainPendingWrite() { return m_stream && isLocalStreamEnabled() ? m_stream->drainPendingWrite() : false; }

void TerminalRuntime::resizeGrid(int rows, int columns, Ubiquitty::Modes::ReflowMode reflow)
{
    resizeGrid(rows, columns, reflow, Ubiquitty::Modes::ResizeUrgency::Settled, Ubiquitty::Modes::AnchorMode::Top);
}

void TerminalRuntime::resizeGrid(int rows, int columns, Ubiquitty::Modes::ReflowMode reflow, Ubiquitty::Modes::ResizeUrgency urgency,
    Ubiquitty::Modes::AnchorMode anchor)
{
    if (!m_term || !isLocalStreamEnabled())
        return;
    // Runs persist across processBytes returns by design, and the band run
    // holds pointers into the grid's storage -- committed here, before the
    // resize can reallocate what the stash points at.
    Ubiquitty::termFlushScrollLane(*m_term);
    Ubiquitty::resize(*m_term, rows, columns, reflow, anchor, urgency);
    // The resize may have queued the DECSET 2048 in-band report (CSI 48 t).
    // Flushed here, under the lock the caller holds, rather than left for the
    // next input-driven drain: a mode-2048 program (Neovim) ignores SIGWINCH
    // and waits for this report, and a quiet resize never drains, so it would
    // not see the new size until the user typed. The reply buffer is drained
    // at the resize itself, under the lock the caller holds.
    flushReplies();
    ++m_generation; // a resize bumps the dimension generation
    // Republish so lock-free readers see the new grid immediately, not one drain late.
    RuntimeSnapshot snap;
    captureSnapshot(snap);
    publishReads(snap, m_term.get());
    // The resize may have queued the DECSET 2048 in-band report (CSI 48 t).
    // Written to the child here, under the model lock the caller holds and in
    // the model-then-queue order the ingest's flush takes: left in the buffer
    // it waits for the next input-driven drain, and a program on mode 2048
    // ignores SIGWINCH and reflows only on the report.
    flushReplies();
}

void TerminalRuntime::setWindowSize(int columns, int rows, int xpixel, int ypixel)
{
    if (!isLocalStreamEnabled())
        return;
    const std::scoped_lock hold(m_windowSizeMutex);
    // A size the stream already holds is not sent again: a SIGWINCH for an
    // unchanged winsize only makes a full-screen program redraw. A grid
    // change goes out at once; a change of pixels alone — a window step that
    // moves no cell boundary, a backing-scale change — cannot change the
    // program's picture, so those coalesce to one per interval.
    const bool isPending = m_isWindowSizePending.load(std::memory_order_acquire);
    const bool isGridChange = !m_hasWindowSize || columns != m_windowColumns || rows != m_windowRows;
    const bool isPixelChange = !m_hasWindowSize || xpixel != m_windowXPixels || ypixel != m_windowYPixels;
    m_hasWindowSize = true;
    m_windowColumns = columns;
    m_windowRows = rows;
    m_windowXPixels = xpixel;
    m_windowYPixels = ypixel;
    if (!m_stream || (!isGridChange && !isPixelChange && !isPending))
        return;
    const std::uint32_t now = PAL::monotonicMicros() | 1u;
    const std::uint32_t applied = m_windowSizeAppliedMicros.load(std::memory_order_relaxed);
    if (isGridChange || applied == 0 || now - applied >= windowSizeCoalesceMicros) {
        m_stream->setWindowSize(columns, rows, xpixel, ypixel);
        m_windowSizeDeliveryCount.fetch_add(1, std::memory_order_relaxed);
        m_windowSizeAppliedMicros.store(now, std::memory_order_relaxed);
        m_isWindowSizePending.store(false, std::memory_order_release);
        return;
    }
    m_isWindowSizePending.store(true, std::memory_order_release);
    // The loop parks with no timeout while nothing is due; woken, it reads
    // the due time and waits exactly that long.
    if (PAL::Waker* waker = m_processTree->waker.load(std::memory_order_acquire))
        waker->wake();
}

std::uint32_t TerminalRuntime::pendingWindowSizeDueInMillis(std::uint32_t nowMicros) const
{
    if (!m_isWindowSizePending.load(std::memory_order_acquire))
        return 0;
    const std::uint32_t elapsed = nowMicros - m_windowSizeAppliedMicros.load(std::memory_order_relaxed);
    if (elapsed >= windowSizeCoalesceMicros)
        return 0;
    return (windowSizeCoalesceMicros - elapsed + 999) / 1000;
}

void TerminalRuntime::applyPendingWindowSizeIfDue(std::uint32_t nowMicros)
{
    if (!isLocalStreamEnabled())
        return;
    if (!m_isWindowSizePending.load(std::memory_order_acquire))
        return;
    const std::scoped_lock hold(m_windowSizeMutex);
    if (!m_isWindowSizePending.load(std::memory_order_acquire))
        return;
    if (nowMicros - m_windowSizeAppliedMicros.load(std::memory_order_relaxed) < windowSizeCoalesceMicros)
        return;
    if (m_stream) {
        m_stream->setWindowSize(m_windowColumns, m_windowRows, m_windowXPixels, m_windowYPixels);
        m_windowSizeDeliveryCount.fetch_add(1, std::memory_order_relaxed);
    }
    m_windowSizeAppliedMicros.store(nowMicros | 1u, std::memory_order_relaxed);
    m_isWindowSizePending.store(false, std::memory_order_release);
}

void TerminalRuntime::reassertWindowSizeIfDue(std::uint32_t nowMicros)
{
    if (!isLocalStreamEnabled())
        return;
    if (!m_windowSizeReassert.shouldSendNow(nowMicros))
        return;
    const std::scoped_lock hold(m_windowSizeMutex);
    if (!m_stream || !m_hasWindowSize)
        return;
    // The size last published, pending or not: a held pixel change is the
    // newest size, and sending it here leaves the coalescing nothing to apply.
    // The group the window was opened for rides along: the policy allowed this
    // re-send because that group is not the shell, and the transport signals
    // only while that same group still holds the terminal.
    m_stream->reassertWindowSize(m_windowColumns, m_windowRows, m_windowXPixels, m_windowYPixels,
        static_cast<PAL::ProcessID>(m_windowSizeReassert.foregroundProcessGroup()));
    m_windowSizeDeliveryCount.fetch_add(1, std::memory_order_relaxed);
    m_windowSizeAppliedMicros.store(nowMicros | 1u, std::memory_order_relaxed);
    m_isWindowSizePending.store(false, std::memory_order_release);
}

std::optional<std::uint32_t> TerminalRuntime::windowSizeReassertDueInMillis(std::uint32_t nowMicros)
{
    const std::optional<std::uint32_t> wait = m_windowSizeReassert.microsUntilDue(nowMicros);
    if (!wait)
        return std::nullopt;
    return (*wait + 999) / 1000;
}

void TerminalRuntime::armEraseHoldIfUnseen(const Ubiquitty::Terminal& terminal, std::uint32_t nowMicros)
{
    const std::uint32_t generation = terminal.eraseDisplayGeneration;
    if (generation == m_seenEraseGeneration.load(std::memory_order_relaxed))
        return;
    m_seenEraseGeneration.store(generation, std::memory_order_relaxed);
    // A continuous resize can erase on every display refresh. Keep the first
    // erase's deadline so those redraws cannot postpone presentation forever.
    std::uint32_t expected = 0;
    m_eraseHoldStartMicros.compare_exchange_strong(expected, nowMicros | 1u, std::memory_order_relaxed);
}

bool TerminalRuntime::shouldWithholdPresentationAfterErase(std::uint32_t nowMicros)
{
    const std::uint32_t start = m_eraseHoldStartMicros.load(std::memory_order_relaxed);
    if (start == 0)
        return false;
    const bool isWatchdogSpent = microsSince(nowMicros, start) >= eraseHoldMaxMicros;
    const bool hasSettled = microsSince(nowMicros, m_lastBytesMicros.load(std::memory_order_relaxed)) >= eraseHoldSettleMicros;
    if (isWatchdogSpent || hasSettled) {
        m_eraseHoldStartMicros.store(0, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TerminalRuntime::noteEraseRedrawParsed()
{
    if (!m_term)
        return;
    m_parsedEraseGeneration.store(m_term->eraseDisplayGeneration, std::memory_order_relaxed);
    m_redrawArrivingSinceMicros.store(0, std::memory_order_relaxed);
}

std::uint32_t TerminalRuntime::eraseGeneration() const
{
    return m_term ? m_term->eraseDisplayGeneration : 0;
}

void TerminalRuntime::noteFeedParsed(std::uint32_t eraseGenerationBefore)
{
    if (!m_term || m_term->eraseDisplayGeneration == eraseGenerationBefore)
        return;
    if (m_parsedEraseGeneration.load(std::memory_order_relaxed) != eraseGenerationBefore)
        return;
    noteEraseRedrawParsed();
}

void TerminalRuntime::settlePasswordPromptAtPause()
{
    if (!m_term)
        return;
    Ubiquitty::settlePasswordPrompt(*m_term, m_stream ? &m_stream->process() : nullptr);
}

bool TerminalRuntime::isEraseRedrawArriving(const Ubiquitty::Terminal& terminal, std::uint32_t nowMicros)
{
    if (m_parsedEraseGeneration.load(std::memory_order_relaxed) == terminal.eraseDisplayGeneration) {
        m_redrawArrivingSinceMicros.store(0, std::memory_order_relaxed);
        return false;
    }
    // Erases inside a redraw that has not been parsed whole keep the first
    // ask's deadline, so a stream that never catches up is held for one
    // watchdog and no longer.
    std::uint32_t since = m_redrawArrivingSinceMicros.load(std::memory_order_relaxed);
    if (since == 0) {
        since = nowMicros | 1u;
        m_redrawArrivingSinceMicros.store(since, std::memory_order_relaxed);
    }
    const bool isWatchdogSpent = microsSince(nowMicros, since) >= eraseHoldMaxMicros;
    const bool hasSettled = microsSince(nowMicros, m_lastBytesMicros.load(std::memory_order_relaxed)) >= eraseHoldSettleMicros;
    return !isWatchdogSpent && !hasSettled;
}

bool TerminalRuntime::shouldWithholdPresentation(Ubiquitty::Terminal* terminal, Ticks nowTicks, std::uint32_t nowMicros)
{
    // An erase the parser has applied but whose drain has not finished is
    // armed here, under the caller's lock: the drain stamps the hold only
    // once its pass ends, and a capture taken between the two would show the
    // erased screen with nothing holding it back.
    if (terminal)
        armEraseHoldIfUnseen(*terminal, nowMicros);
    const bool isBlockHeld = terminal && Modes::SyncOutput::shouldWithholdFrame(*terminal, nowTicks);
    return isBlockHeld || shouldWithholdPresentationAfterErase(nowMicros);
}

std::uint32_t TerminalRuntime::presentationHoldTicksRemaining(const Ubiquitty::Terminal* terminal, Ticks nowTicks, std::uint32_t nowMicros) const
{
    std::uint32_t ticks = terminal ? Modes::SyncOutput::ticksUntilHoldExpires(*terminal, nowTicks) : 0;
    const std::uint32_t eraseMicros = eraseHoldMicrosRemaining(nowMicros);
    if (eraseMicros > 0) {
        const std::uint32_t eraseTicks = std::max<std::uint32_t>(1, (eraseMicros * 60 + 999999) / 1000000);
        ticks = ticks == 0 ? eraseTicks : std::min(ticks, eraseTicks);
    }
    return ticks;
}

bool TerminalRuntime::takeOwedPaint(Ticks nowTicks, std::uint32_t nowMicros)
{
    // A frame the coherence rule holds is painted when the rule admits it, by
    // the host; painting it here would show the screen the rule held back.
    if (!m_isPaintOwed || isFrameHeld() || shouldWithholdPresentation(m_term.get(), nowTicks, nowMicros))
        return false;
    m_isPaintOwed = false;
    return true;
}

int TerminalRuntime::owedPaintWaitMilliseconds(Ticks nowTicks, std::uint32_t nowMicros) const
{
    // A held frame's wake is the rule's, from publishHeldFrameIfDue.
    if (!m_isPaintOwed || isFrameHeld())
        return -1;
    const std::uint32_t ticks = presentationHoldTicksRemaining(m_term.get(), nowTicks, nowMicros);
    return static_cast<int>((ticks * 1000 + 59) / 60);
}

std::uint32_t TerminalRuntime::eraseHoldMicrosRemaining(std::uint32_t nowMicros) const
{
    const std::uint32_t start = m_eraseHoldStartMicros.load(std::memory_order_relaxed);
    if (start == 0)
        return 0;
    const std::uint32_t sinceStart = microsSince(nowMicros, start);
    const std::uint32_t sinceBytes = microsSince(nowMicros, m_lastBytesMicros.load(std::memory_order_relaxed));
    if (sinceStart >= eraseHoldMaxMicros || sinceBytes >= eraseHoldSettleMicros)
        return 0;
    return std::min(eraseHoldMaxMicros - sinceStart, eraseHoldSettleMicros - sinceBytes);
}

void TerminalRuntime::updatePaneHostFacts(Protocol::Sync::PaneHostFacts& facts) const
{
    const std::scoped_lock factsLock(m_processFactsMutex);
    const auto end = sessionEnd();
    const std::string empty;
    const auto resolve = [&](const std::string& sampled) -> const std::string& {
        if (end.didEnd)
            return empty;
        return sampled.empty() ? m_processState.launchSeed : sampled;
    };
    const auto& chain = resolve(m_processState.processChain);
    const auto& arguments = resolve(m_processState.processChainWithArguments);
    const auto& foreground = resolve(m_processState.foregroundName);
    const auto& shell = end.didEnd ? empty : m_shellCommandName;
    const auto processID = end.didEnd ? 0u : m_processState.foregroundProcessGroup;
    if (facts.foreground.processID != processID || facts.foreground.name != foreground) {
        facts.foreground.processID = processID;
        facts.foreground.name = foreground;
        ++facts.foreground.generation;
    }
    const auto update = [](std::string& value, const std::string& next) {
        if (value != next)
            value = next;
    };
    update(facts.foregroundDirectory, m_processState.currentDirectory);
    update(facts.processChain, chain);
    update(facts.processChainWithArguments, arguments);
    update(facts.shellCommandName, shell);
    // Until the existing process sampler resolves the leader tty it remains
    // unknown. A pane poll must not turn a cheap read into a stream probe.
    update(facts.terminalName, m_leaderTTYName);
    facts.foregroundLeadsTo = end.didEnd ? PAL::Process::RemoteKind::None : m_processState.remoteTarget.kind;
    facts.isAtPrompt = !end.didEnd && (processID != 0 || !foreground.empty()) && m_isSampledForegroundShell;
    facts.end = { .didEnd = end.didEnd, .hasStatus = end.hasStatus, .wasSignalled = end.wasSignalled, .code = end.code };
}

ForegroundProgram TerminalRuntime::foregroundProgram(std::uint8_t reportedProcessIcon) const
{
    ForegroundProgram program;
    Title::TerminalHolder holder = Title::TerminalHolder::Shell;
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        // Read under the lock: the end is marked before the facts are cleared
        // under it, so a reader holding it sees either the end or the facts
        // from before it.
        if (m_sessionDidEnd.load(std::memory_order_acquire))
            return {};
        const auto resolve = [&](const std::string& sampled) { return sampled.empty() ? m_processState.launchSeed : sampled; };
        program.name = resolve(m_processState.foregroundName);
        program.chain = resolve(m_processState.processChain);
        program.chainWithArguments = resolve(m_processState.processChainWithArguments);
        holder = m_isSampledForegroundShell ? Title::TerminalHolder::Shell : Title::TerminalHolder::Program;
    }
    program.key = Title::processIconKeyFor(reportedProcessIcon, program.name, holder);
    return program;
}

std::string TerminalRuntime::processIconKey(std::uint8_t reportedProcessIcon) const
{
    const std::scoped_lock factsLock(m_processFactsMutex);
    const std::string& name = m_processState.foregroundName.empty() ? m_processState.launchSeed : m_processState.foregroundName;
    const auto holder = m_isSampledForegroundShell ? Title::TerminalHolder::Shell : Title::TerminalHolder::Program;
    return std::string(Title::processIconKeyFor(reportedProcessIcon, name, holder));
}

std::string TerminalRuntime::ttyName() const
{
    // The walk's answer when there is one: it names the tty the PTY's session
    // leader holds, which is the device the segment is about. The stream's own
    // name stands in before the first walk, so a title has something to show
    // rather than a gap that fills a moment later.
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        if (!m_leaderTTYName.empty())
            return m_leaderTTYName;
    }
    return m_stream ? m_stream->process().ttyName() : std::string {};
}

/// Whether the foreground facts have never been read on this session.
///
/// - Returns: Whether no probe has answered yet.
bool TerminalRuntime::hasNeverProbedForegroundFacts() const
{
    return !m_foregroundProbeValid;
}

/// Whether a tracking application is reading the pointer this drain.
///
/// While tracking is on the enabler has to be re-read every drain, so a dead
/// one is noticed on the very next drain rather than a tick budget later.
///
/// - Returns: Whether the last drain saw mouse tracking active.
bool TerminalRuntime::isMouseTrackingActive() const
{
    return m_mouseTrackingActiveLastDrain;
}

/// Whether the tick budget between foreground probes has run out.
///
/// - Parameter now: This drain's tick.
/// - Returns: Whether the interval since the last probe has elapsed.
bool TerminalRuntime::hasForegroundProbeBudgetElapsed(Ticks now) const
{
    return now.since(m_lastForegroundProbeTicks) >= foregroundProbeIntervalTicks;
}

/// Whether this drain should re-read the foreground process facts.
///
/// Tracking applications need each drain checked. An observed session also
/// samples on first use, on the tick budget, or when a caught-up drain finds a
/// different foreground group. Short PTY refill gaps do not imply that a job
/// changed, so the cheap group query gates the full process-table walk.
///
/// - Parameters:
///   - now: This drain's tick.
///   - isCaughtUp: Whether the drain reached a refill gap.
/// - Returns: Whether to probe.
bool TerminalRuntime::shouldProbeForegroundFacts(Ticks now, bool isCaughtUp) const // bool-ok: the drain's own reading of whether it caught up, not a mode this chooses
{
    return isMouseTrackingActive()
        || (foregroundObservation() != ForegroundObservation::Unread
            && (hasNeverProbedForegroundFacts() || hasForegroundProbeBudgetElapsed(now)
                || (isCaughtUp && m_stream->process().foregroundProcessGroup().value_or(0) != m_cachedForegroundGroup)));
}

/// Whether this platform's subtree walk descends instead of scanning the host.
///
/// A descending walk is a handful of targeted reads whose cost follows the
/// SESSION's size, so it just runs: guessing when to skip it is what left a
/// title naming a program the session had moved on from. Where the walk has to
/// scan every process instead, the gate below earns its keep.
///
/// - Returns: Whether the walk is cheap enough to run unconditionally.
bool TerminalRuntime::descendingWalkIsCheapEnoughToAlwaysRun() const
{
    return m_stream->process().hasCheapProcessWalk();
}

/// Whether anything says the foreground chain's answer can have moved.
///
/// The kernel's fork/exec/exit edge is the signal that matters; the rest are
/// cheap proxies that stay because they also cover a host with no subtree
/// tracker, where there is no edge to wait for.
///
/// - Parameters:
///   - foregroundProcessGroup: The group this sample read.
///   - didNameChange: Whether the foreground process name moved.
///   - didTreeChange: Whether the kernel reported a tree edge since the last sample.
/// - Returns: Whether to walk.
bool TerminalRuntime::foregroundChainCouldHaveMoved(std::uint32_t foregroundProcessGroup, bool didNameChange, bool didTreeChange) const // bool-ok: kernel readings, not caller decisions
{
    return PAL::Process::shouldRewalkForegroundChain(foregroundProcessGroup, m_chainProcessGroup,
        !m_processState.processChain.empty(), didNameChange, m_chainForegroundCount, didTreeChange);
}

bool TerminalRuntime::foldColdScrollback(DrainHost& host, int blockBudget)
{
    if (!m_term)
        return false;
    host.lockModel();
    const bool hasColdBlocks = Ubiquitty::Screen::foldColdScrollback(*m_term, blockBudget);
    host.unlockModel();
    return hasColdBlocks;
}

DrainOutcome TerminalRuntime::sampleProcessStateOffDrain(DrainHost& host)
{
    DrainOutcome outcome;
    if (!m_stream || !isLocalStreamEnabled() || foregroundObservation() == ForegroundObservation::Unread) {
        return outcome;
    }
    takeForegroundProbeOwed();
    const bool wasForegroundShell = m_cachedForegroundIsShell;
    std::string probedName = probeForegroundHolder();
    const std::string directory = m_stream->process().foregroundCurrentDirectory();
    host.lockModel();
    if (m_term) {
        const bool didRetirePointer = Mouse::retireTrackingIfStale(*m_term, m_mouseTrackingProcessGroup, m_cachedForegroundGroup, m_cachedForegroundIsShell);
        // A quiet owner change still owes the frontend the new input modes.
        outcome.processStateChanged = didRetirePointer;
        if (m_cachedForegroundIsShell && !wasForegroundShell)
            OSC::foregroundReturnedToShell(*m_term);
        m_term->foregroundProcessGroup = m_cachedForegroundGroup;
        outcome.currentDirectoryChanged = Ubiquitty::retireReportedDirectoryIfStale(*m_term, m_cachedForegroundGroup);
        if (!directory.empty() && directory != m_cachedForegroundDirectory) {
            m_cachedForegroundDirectory = directory;
            outcome.currentDirectoryChanged = Ubiquitty::setSampledDirectoryString(*m_term, directory) || outcome.currentDirectoryChanged;
        }
        outcome.currentDirectoryChanged = Ubiquitty::retireReportedHostIfStale(*m_term, m_cachedForegroundGroup) || outcome.currentDirectoryChanged;
        outcome.titleChanged = Ubiquitty::retireTitleIfStale(*m_term, m_cachedForegroundGroup);
        if (outcome.titleChanged) {
            ++m_titleGeneration;
            m_term->titleChanged = false;
        }
        if (outcome.currentDirectoryChanged) {
            m_term->currentDirectoryChanged = false;
            const std::scoped_lock factsLock(m_processFactsMutex);
            if (m_processState.reportedHost != m_term->reportedHost) {
                m_processState.reportedHost = m_term->reportedHost;
                outcome.processStateChanged = true;
            }
        }
        if (outcome.titleChanged || outcome.currentDirectoryChanged || didRetirePointer) {
            RuntimeSnapshot snapshot;
            captureSnapshot(snapshot);
            publishReads(snapshot, m_term.get());
        }
    }
    host.unlockModel();
    if (outcome.currentDirectoryChanged) {
        emit(EventKind::DirectoryChanged);
    }
    sampleProcessState(std::move(probedName));
    outcome.processStateChanged = outcome.processStateChanged || m_didProcessStateMove;
    m_lastForegroundProbeTicks = PAL::monotonicTicks();
    m_foregroundProbeValid = true;
    return outcome;
}

DrainOutcome TerminalRuntime::sampleCurrentDirectoryOffDrain(DrainHost& host)
{
    DrainOutcome outcome;
    if (!m_stream || !isLocalStreamEnabled() || foregroundObservation() == ForegroundObservation::Unread)
        return outcome;
    const std::string directory = m_stream->process().foregroundCurrentDirectory();
    if (directory.empty() || directory == m_cachedForegroundDirectory)
        return outcome;
    host.lockModel();
    if (m_term) {
        m_cachedForegroundDirectory = directory;
        {
            const std::scoped_lock factsLock(m_processFactsMutex);
            m_processState.currentDirectory = directory;
        }
        outcome.currentDirectoryChanged = Ubiquitty::setSampledDirectoryString(*m_term, directory);
        if (outcome.currentDirectoryChanged) {
            m_term->currentDirectoryChanged = false;
            RuntimeSnapshot snapshot;
            captureSnapshot(snapshot);
            publishReads(snapshot, m_term.get());
        }
    }
    host.unlockModel();
    if (outcome.currentDirectoryChanged)
        emit(EventKind::DirectoryChanged);
    return outcome;
}

void TerminalRuntime::takeForegroundProbeOwed()
{
    m_processTree->isForegroundProbeOwed.exchange(false, std::memory_order_acq_rel);
}

std::string TerminalRuntime::probeForegroundHolder()
{
    m_cachedForegroundGroup = m_stream->process().foregroundProcessGroup().value_or(0);
    // The name is read for a sample, which only a reader of process state
    // takes; a probe for the mouse guard or the title and directory reads the
    // holder alone.
    if (foregroundObservation() != ForegroundObservation::ProcessState) {
        m_cachedForegroundIsShell = m_stream->process().foregroundIsShell();
        return {};
    }
    PAL::ForegroundHolder holder = m_stream->process().foregroundHolder();
    m_cachedForegroundIsShell = holder.isShell;
    return std::move(holder.name);
}

void TerminalRuntime::sampleProcessState(std::string probedName)
{
    // Cleared first so the flag always describes this sample. A session with
    // no stream takes the early return, and a held `true` from the sample
    // before it would raise the event a second time for one move.
    m_didProcessStateMove = false;
    if (!m_stream || !isLocalStreamEnabled() || foregroundObservation() != ForegroundObservation::ProcessState)
        return;
    // No pre-exec special case is needed here, unlike a frontend's own timer:
    // this runs on a drain, and a child that has produced output has exec'd.
    // The walk writes here, not into the members. Only the assignment at the
    // end takes the lock, so the costly half — the walk itself and the two
    // chain formats — is never done with a reader blocked behind it.
    // Kept only when a walk runs. An empty walk leaves the previous subtree
    // standing for the same reason it leaves the chain standing below: a
    // transient failure to read the process table is not the session ending.
    std::vector<PAL::Process::Record> nextRecords;
    bool didWalk = false;
    std::string nextShellCommandName = m_shellCommandName;
    std::string nextLeaderTTYName = m_leaderTTYName;
    // A just-forked child still reads as this application's own image until
    // it execs the shell. Naming the window after the terminal for that
    // window is worse than naming it nothing.
    const auto withoutHostName = [](std::string name) {
        if (name == PAL::Process::hostProcessName())
            name.clear();
        return name;
    };
    const auto readHolder = [&] {
        PAL::ForegroundHolder holder = m_stream->process().foregroundHolder();
        holder.name = withoutHostName(std::move(holder.name));
        return holder;
    };
    // The probe read the group, then the holder and its name from one reading
    // of the owner; the reading is settled against the one after it, so a
    // foreground that moved, or a name read in the moment a program execs, is
    // read again rather than published (`Session::settleForegroundReading`). A
    // reading equal to the last settled sample publishes nothing new, and is
    // not read again; one still moving is published unsettled, and the next
    // sample reads it again. One that names nothing publishes the last
    // published name beside the holder it read (`Session::holdUnnamedReading`).
    std::string firstName = withoutHostName(std::move(probedName));
    const bool isPublishedReading = m_isForegroundSampleSettled && firstName == m_cachedForegroundName && m_cachedForegroundIsShell == m_isSampledForegroundShell;
    Session::ForegroundReading reading = Session::settleForegroundReading(m_stream->process(),
        { .name = std::move(firstName), .processGroup = m_cachedForegroundGroup, .isShell = m_cachedForegroundIsShell, .isSettled = isPublishedReading }, readHolder);
    Session::holdUnnamedReading(reading, m_cachedForegroundName);
    m_isForegroundSampleSettled = reading.isSettled;
    std::string foregroundName = std::move(reading.name);
    const int foregroundGroup = reading.processGroup;
    const bool isForegroundShell = reading.isShell;
    const bool didNameChange = foregroundName != m_cachedForegroundName;
    m_cachedForegroundName = foregroundName;

    Session::ProcessEvent event;
    event.kind = Session::ProcessEventKind::PollResult;
    event.foregroundProcessGroup = static_cast<std::uint32_t>(foregroundGroup);
    event.foregroundName = foregroundName;
    event.currentDirectory = m_cachedForegroundDirectory;
    event.processChain = m_processState.processChain;
    event.processChainWithArguments = m_processState.processChainWithArguments;
    event.tick = PAL::monotonicTicks().count;

    // Walking every process on the machine is the costly half, so it runs only
    // when the gate says the answer can have moved.
    // Cleared as it is read: this sample is the one answering the edge, and a
    // flag left raised would re-walk on every tick after it.
    const bool treeChanged = m_processTree->dirty.exchange(false, std::memory_order_relaxed);
    // Walked only when something says the answer can have moved. The kernel's
    // fork/exec/exit edge is the signal that matters — the rest are cheap
    // proxies that stay because they also cover a host with no subtree tracker,
    // where there is no edge to wait for.
    // A descending walk just runs: it is a handful of targeted reads, paced by
    // the sampler's own cadence, and guessing when to skip it is what left a
    // title naming a program the session had moved on from. The gate below
    // still guards the scan-the-machine fallback, where guessing earns its keep.
    const Ticks walkTicks(static_cast<std::uint32_t>(event.tick));
    const bool didForegroundChange = event.foregroundProcessGroup != m_chainProcessGroup || didNameChange;
    const bool isWalkDue = !m_hasProcessWalk || didForegroundChange || treeChanged
        || walkTicks.since(m_lastProcessWalkTicks) >= Ticks::fromMilliseconds(Title::walkFallbackMilliseconds).count;
    // A multi-process foreground may change internally, but that possibility
    // is not a kernel edge. Full-table scans use Core's fallback cadence;
    // foreground ownership probes and cheap descendant walks remain prompt.
    const bool willWalk = descendingWalkIsCheapEnoughToAlwaysRun()
        || (isWalkDue && foregroundChainCouldHaveMoved(event.foregroundProcessGroup, didNameChange, treeChanged));
    if (willWalk) {
        m_hasProcessWalk = true;
        m_lastProcessWalkTicks = walkTicks;
        PAL::Process::Record records[processChainWalkCapacity];
        const int found = m_stream->process().gatherProcesses({ records, processChainWalkCapacity });
        if (found > 0) {
            // The walk is how the shell is discovered at all: the session's
            // leader is `login`, which is root, so nothing below it can be
            // learned from an edge until something we may watch is known. Hand
            // back what was found so the watch set grows to cover it.
            PAL::ProcessID observed[processChainWalkCapacity];
            for (int index = 0; index < found; ++index)
                observed[index] = static_cast<PAL::ProcessID>(records[index].processID);
            m_stream->process().observeProcesses({ observed, static_cast<std::size_t>(found) });
            int foregroundCount = 0;
            for (int index = 0; index < found; ++index) {
                if (records[index].isForeground)
                    ++foregroundCount;
            }
            didWalk = true;
            m_chainForegroundCount = foregroundCount;
            m_chainProcessGroup = event.foregroundProcessGroup;
            const std::span<const PAL::Process::Record> walked { records, static_cast<std::size_t>(found) };
            nextRecords.assign(walked.begin(), walked.end());
            // The reading a host pane's facts are made by too, so a pane
            // titles the same whichever side of the wire walked it. A chain
            // that names this application's own image comes back empty, and
            // the launch seed answers for it.
            Session::WalkedProcessFacts facts = Session::walkedProcessFacts(walked);
            nextShellCommandName = std::move(facts.shellCommandName);
            nextLeaderTTYName = std::move(facts.leaderTTYName);
            m_isForegroundControlModeTmux = PAL::Process::isForegroundControlModeTmux(walked);
            event.processChain = std::move(facts.processChain);
            event.processChainWithArguments = std::move(facts.processChainWithArguments);
            event.remoteTarget = PAL::Process::remoteTargetForChain(walked);
        }
        // No `else` clearing the chain on an empty walk. A walk can come back
        // empty for a session that is very much alive (a transient failure to
        // read the table), and blanking the process parts on that would make a
        // title flicker. The session ending is a fact with its own signal —
        // recordSessionEnd, on end-of-file — and that is what retires them.
    }
    // Compared after the walk, not before it: the walk is what can move the
    // chain, and the leader shell resolves on the first walk that finds one.
    //
    // The one place these three move, and so the only place a reader has to be
    // excluded from. They are std::strings that the ingest thread rewrites in
    // place while the main thread composes a title out of them — a reassignment
    // that changes capacity frees the buffer a reader is walking, which is not
    // a stale answer but a use-after-free. Hence a lock rather than a memo.
    {
        const std::scoped_lock factsLock(m_processFactsMutex);
        const bool didChainMove = event.processChain != m_processState.processChain
            || event.processChainWithArguments != m_processState.processChainWithArguments;
        // Who holds the terminal is a fact of its own: the close button's
        // marker reads it from here, and a shell that takes the terminal back
        // without the walk seeing any process come or go must still redraw it.
        const bool didForegroundOwnerMove = isForegroundShell != m_isSampledForegroundShell;
        m_isSampledForegroundShell = isForegroundShell;
        // The name is a fact of its own as well: a program's name and key
        // are read beside this edge, and a job that came and went between
        // two reads moves the name alone, and then moves it back.
        m_didProcessStateMove = didChainMove || didForegroundOwnerMove || didNameChange || nextShellCommandName != m_shellCommandName;
        m_shellCommandName = std::move(nextShellCommandName);
        m_leaderTTYName = std::move(nextLeaderTTYName);
        if (didWalk) {
            // The generation a view over the table keys on: bumped by what the
            // records say, not by the chain, so a reaped job and a stopped
            // program move it and a walk that found the same tree does not.
            if (didProcessRecordsMove(m_processRecords, nextRecords)) {
                m_processRecordsGeneration.fetch_add(1, std::memory_order_release);
                m_didProcessStateMove = true;
            }
            m_processRecords = std::move(nextRecords);
        }
        Session::applyProcessEvent(m_processState, event);
    }
    // A takeover's first re-send goes out with the sample that saw it; the
    // ingest loop's tick sends the rest.
    const std::uint32_t nowMicros = PAL::monotonicMicros();
    m_windowSizeReassert.noteSample({ .processGroup = event.foregroundProcessGroup, .isShell = isForegroundShell }, nowMicros);
    reassertWindowSizeIfDue(nowMicros);
}

void TerminalRuntime::setFocused(Ubiquitty::Modes::FocusState focus)
{
    if (!m_term || !isLocalStreamEnabled())
        return;
    Ubiquitty::Modes::focusChanged(m_term.get(), focus);
    // A local terminal's frontend reports the focus of the person at it, so
    // a pane that gains it has had its bell and notification seen.
    if (focus == Ubiquitty::Modes::FocusState::Focused)
        (void)Ubiquitty::markAttentionSeen(*m_term);
}

void TerminalRuntime::applyCommand(const Command& command)
{
    if (!isLocalStreamEnabled() && command.kind != CommandKind::SetPresentationFocus)
        return;
    switch (command.kind) {
    case CommandKind::SetFocus: {
        // Core's focusChanged is not idempotent — it emits DEC 1004 on every
        // call — so a no-op focus would re-report. Dedup against the last
        // applied state here.
        const int want = command.flag ? 1 : 0;
        if (want == m_lastFocusApplied)
            return;
        setFocused(command.flag ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused);
        m_lastFocusApplied = want;
        break;
    }
    case CommandKind::ResolveClipboardRead:
        // The posted form of the frontend's clipboard answer: the completion
        // touches the Terminal, and this switch already runs where the model
        // lock is held — the whole reason the answer rides the ring rather
        // than calling in from the UI thread.
        completeClipboardRead(command.set.data(),
            command.flag ? Ubiquitty::OSC::ClipboardReadDecision::Approved : Ubiquitty::OSC::ClipboardReadDecision::Refused);
        break;
    case CommandKind::SetPresentationFocus:
        if (m_term) {
            m_term->windowFocused = command.flag;
            if (command.flag)
                (void)Ubiquitty::markAttentionSeen(*m_term);
        }
        // This was not a program report. If the runtime later owns a local
        // program again, its first real focus fact must still be delivered.
        m_lastFocusApplied = -1;
        break;
    }
}

bool TerminalRuntime::applyPendingCommands(DrainHost& host)
{
    if (m_link.commands.empty())
        return false; // no lock taken when nothing is queued (the idle-tick common case)
    host.lockModel();
    Command command;
    bool didApply = false;
    while (m_link.commands.pop(command)) {
        applyCommand(command);
        didApply = true;
    }
    host.unlockModel();
    if (didApply)
        m_didCommandsTouchModel = true;
    return didApply;
}

DimensionSnapshot TerminalRuntime::dimensions() const
{
    // Lock-free: read the published RuntimeLink snapshot instead of the live Terminal,
    // so a main-thread geometry query never races the ingest writer.
    const RuntimeSnapshot snap = m_link.reads.read();
    DimensionSnapshot snapshot;
    snapshot.rows = snap.rows;
    snapshot.columns = snap.columns;
    snapshot.scrollbackCount = snap.scrollbackCount;
    snapshot.generation = static_cast<std::uint32_t>(snap.version);
    return snapshot;
}

Ubiquitty::Terminal* TerminalRuntime::borrowTermForRender() { return m_term.get(); }

PAL::Stream* TerminalRuntime::stream() { return isLocalStreamEnabled() ? m_stream.get() : nullptr; }

void TerminalRuntime::setEventSink(EventSink* sink) { m_sink = sink; }

void TerminalRuntime::emit(EventKind kind, std::uint32_t data0)
{
    if (!m_sink)
        return;
    Event event;
    event.kind = kind;
    event.data0 = data0;
    m_sink->onRuntimeEvent(event);
}

} // namespace Ubiquitty::Runtime
