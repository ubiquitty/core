// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_frame_cadence — what a frontend presents at each display tick while a
// real drain parses what a writer thread writes, with no display. A frontend
// thread ticks at 60 Hz and runs a frame tick's calls against the session:
// the deliveries' selection update, the frame clock, the frame asked of the
// session and the live read it may answer with, and the keys a reader types.
//
// Every case floods one pane with a selection standing and asserts:
//
// - a fresh frame reaches the glass at least every two refresh intervals
//   (nine in a sanitized build) while output flows, outside an open
//   synchronized update, whose hold is the rule's own; a live read is fresh
//   only when the model moved since the last one;
// - every frame presented while the selection stands draws it;
// - a flood's frames are the drain's captures, not live reads under the lock;
// - the view registers its capture geometry once;
// - the frame decision answers inside a millisecond (four in an unoptimized
//   or sanitized build), and no frontend call takes or waits longer than one
//   (ten in an unoptimized or sanitized build, whose drain holds the model
//   lock longer), including while the drain is stalled for half a second
//   between two bites.
//
// The tick asks TerminalSession::takeFrame. With
// UBIQUITTY_CADENCE_DECISION=primitives it runs instead the decision a
// frontend makes from the session's primitives when Core does not decide: the
// run then shows which cases that decision fails.

#include "Core/Modes/SyncOutput.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace Ubiquitty::PAL {

// The read end of a pipe the case's writer fills, read nonblocking: an empty
// pipe answers Again and readiness is the pipe's own, as a pty's is.
class CadencePipeEndpoint final : public PollableByteStream {
public:
    explicit CadencePipeEndpoint(UnixFileDescriptor descriptor)
        : m_descriptor(std::move(descriptor))
    {
    }

    int read(std::span<char> buffer) override
    {
        const ssize_t count = ::read(m_descriptor.get(), buffer.data(), buffer.size());
        if (count > 0)
            return static_cast<int>(count);
        if (count == 0)
            return std::to_underlying(StreamStatus::EndOfFile);
        return errno == EAGAIN ? std::to_underlying(StreamStatus::Again) : std::to_underlying(StreamStatus::Fatal);
    }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] FileDescriptor& handle() override { return m_descriptor; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        UnixFileDescriptor duplicate(::dup(m_descriptor.get()));
        if (!duplicate)
            return nullptr;
        return std::make_unique<CadencePipeEndpoint>(std::move(duplicate));
    }

private:
    UnixFileDescriptor m_descriptor;
};

// A session transport over that endpoint. What the session writes back, keys
// included, is accepted and dropped.
class CadencePipeStream final : public Stream, private ProcessFacts {
public:
    explicit CadencePipeStream(UnixFileDescriptor descriptor)
        : m_rawDescriptor(descriptor.get())
        , m_endpoint(std::move(descriptor))
    {
    }

    int read(std::span<char> buffer) override { return m_endpoint.read(buffer); }
    int write(std::span<const char> bytes) override { return m_endpoint.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_endpoint.writeAll(bytes); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int fileDescriptor() const override { return m_rawDescriptor; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override { return m_endpoint.duplicateAsPollableStream(); }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return false; }
    std::optional<ProcessID> foregroundProcessGroup() override { return {}; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }

private:
    int m_rawDescriptor;
    CadencePipeEndpoint m_endpoint;
};

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty;
using Clock = std::chrono::steady_clock;
using Rendering::FrameCapture;
using Rendering::SelectionRowSpan;
using Runtime::FrameDecline;
using Runtime::FrameRoute;
using Runtime::TerminalSession;
using namespace std::chrono_literals;

// A taken frame with its release bound in.
using TakenFrame = std::unique_ptr<FrameCapture, decltype([](FrameCapture* frame) { releaseFrameCapture(frame); })>;

constexpr double refreshSeconds = 1.0 / 60.0;
constexpr Clock::duration refreshInterval = std::chrono::microseconds(16'667);

// Whether a sanitizer instruments this build, Core compiled with it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool isCoreInstrumented = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
constexpr bool isCoreInstrumented = true;
#else
constexpr bool isCoreInstrumented = false;
#endif
#else
constexpr bool isCoreInstrumented = false;
#endif

// Whether the compiler optimizes this build, Core compiled with it.
#if defined(__OPTIMIZE__)
constexpr bool isCoreOptimized = true;
#else
constexpr bool isCoreOptimized = false;
#endif

// The longest stretch without a fresh frame while output flows: two refresh
// intervals of the display clock the frontend ticks on. A sanitized Core
// parses and captures several times slower than the build it instruments, so
// such a build bounds the stretch at nine intervals.
constexpr Clock::duration cadenceBound = (isCoreInstrumented ? 9 : 2) * refreshInterval;
// The frame decision reads published state, but judges an open synchronized
// update under the model lock, which waits one owner hold: inside a
// millisecond in the shipping build, and inside four where an unoptimized or
// sanitized Core holds the lock longer.
constexpr Clock::duration frameDecisionBound = isCoreOptimized && !isCoreInstrumented ? 1ms : 4ms;
// The longest a frontend call may wait. A wait for the model lock lasts the
// owner's current hold, which an unoptimized or instrumented Core makes
// several times as long as the shipping build does, a whole-grid capture
// above all; such a build bounds the wait at ten milliseconds. Either bound
// is well below the owner stall and the waits a first-fit lock gave a waiter
// behind a flood.
constexpr Clock::duration frontendWaitBound = isCoreOptimized && !isCoreInstrumented ? 1ms : 10ms;
// The longest any frontend call may take, waiting or working: a call that
// spins counts its spin as work, not as a wait, and is bounded all the same.
constexpr Clock::duration frontendCallBound = frontendWaitBound;

// Output before this much of a flood has passed is the drain electing its
// reader, not the cadence under test.
constexpr Clock::duration warmUp = 150ms;
constexpr Clock::duration ownerStall = 500ms;
// How much of the flood the writer puts inside one synchronized update around
// the stream. The drain's reader runs up to its flood ring ahead of the parse,
// so the parse reaches the close only once it has parsed what was read: a
// bounded update, not a span of the writer's time, keeps the close inside the
// rule's one-second budget however fast the drain parses.
constexpr std::size_t updateBytes = 1024 * 1024;

// Which per-tick decision the frontend runs.
enum class Decision : std::uint8_t {
    TakeFrame,
    FromPrimitives,
};

Decision decisionUnderTest()
{
    const char* named = std::getenv("UBIQUITTY_CADENCE_DECISION");
    return named && std::string_view(named) == "primitives" ? Decision::FromPrimitives : Decision::TakeFrame;
}

// How many reasons a tick can present nothing for.
constexpr std::size_t frameDeclineCount = 6;

constexpr std::string_view frameDeclineName(FrameDecline decline)
{
    switch (decline) {
    case FrameDecline::None:
        return "none";
    case FrameDecline::HeldByCoherence:
        return "heldByCoherence";
    case FrameDecline::HeldByPresentation:
        return "heldByPresentation";
    case FrameDecline::AwaitingCapture:
        return "awaitingCapture";
    case FrameDecline::BlockOpenUnderLock:
        return "blockOpenUnderLock";
    case FrameDecline::RedrawArriving:
        return "redrawArriving";
    }
    return "?";
}

// The frontend calls a tick makes, each timed on every call.
enum class FrontendCall : std::uint8_t {
    TakeEventFlags,
    UpdateSelectionAfterOutput,
    FrameClockFacts,
    ReadSnapshot,
    RegisterCaptureGeometry,
    SendKey,
    TakeFrame,
    LockForRead,
    PresentationHoldTicks,
    RecheckHold,
    NextAnimationDelay,
};
constexpr std::size_t frontendCallCount = 11;

constexpr std::string_view frontendCallName(FrontendCall call)
{
    switch (call) {
    case FrontendCall::TakeEventFlags:
        return "takeEventFlags";
    case FrontendCall::UpdateSelectionAfterOutput:
        return "updateSelectionAfterOutput";
    case FrontendCall::FrameClockFacts:
        return "frameClockFactsNow";
    case FrontendCall::ReadSnapshot:
        return "readSnapshot";
    case FrontendCall::RegisterCaptureGeometry:
        return "setSyncFrameCaptureGeometry";
    case FrontendCall::SendKey:
        return "sendKey";
    case FrontendCall::TakeFrame:
        return "takeFrame";
    case FrontendCall::LockForRead:
        return "lockForRead (the live read's wait)";
    case FrontendCall::PresentationHoldTicks:
        return "presentationHoldTicksRemaining";
    case FrontendCall::RecheckHold:
        return "shouldWithholdFrame (the hold's recheck)";
    case FrontendCall::NextAnimationDelay:
        return "nextAnimationDelay";
    }
    return "?";
}

// This thread's CPU time: what a call spent running rather than waiting.
Clock::duration threadCPUTime()
{
    timespec now {};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
}

// The longest each frontend call took, and the longest it spent off the CPU:
// the wall time its own work does not account for, which is the wait. A call
// that only reads published state answers to its wall time too.
struct CallTimes {
    std::array<Clock::duration, frontendCallCount> longest {};
    std::array<Clock::duration, frontendCallCount> longestBlocked {};
    std::array<Clock::time_point, frontendCallCount> longestBlockedAt {};
    std::array<int, frontendCallCount> count {};

    // Times one call, from its construction to the end of its scope.
    struct Stamp {
        CallTimes& times;
        FrontendCall call;
        Clock::time_point start = Clock::now();
        Clock::duration startCPU = threadCPUTime();
        ~Stamp()
        {
            const Clock::duration took = Clock::now() - start;
            const Clock::duration blocked = took - (threadCPUTime() - startCPU);
            const auto index = static_cast<std::size_t>(call);
            ++times.count[index];
            times.longest[index] = std::max(times.longest[index], took);
            if (blocked > times.longestBlocked[index]) {
                times.longestBlocked[index] = blocked;
                times.longestBlockedAt[index] = start;
            }
        }
    };

    template<typename Body>
    auto time(FrontendCall call, Body&& body)
    {
        const Stamp stamp { *this, call };
        return body();
    }
};

// One frame that reached the glass.
struct Present {
    // The display-clock slot of the tick that presented it.
    Clock::time_point at;
    // When the tick took it, on the same clock, at or after its slot.
    Clock::time_point takenAt;
    FrameRoute route = FrameRoute::Declined;
    bool isFresh = false;
    bool didSelectionStand = false;
    int selectionSpans = 0;
    int cursorRow = 0;
    int cursorColumn = 0;
};

// What a case's writer can do: write whole, sleep, and mark a moment.
class Writer {
public:
    Writer(int descriptor, const std::atomic<bool>& shouldStop)
        : m_descriptor(descriptor)
        , m_shouldStop(shouldStop)
    {
    }

    // A writer that feeds a session with no ingest loop, as an embedder does,
    // and stamps when its feed parsed a synchronized update's open.
    Writer(TerminalSession& session, const std::atomic<bool>& shouldStop, std::atomic<std::int64_t>& openFedAt)
        : m_descriptor(-1)
        , m_shouldStop(shouldStop)
        , m_session(&session)
        , m_openFedAt(&openFedAt)
    {
    }

    [[nodiscard]] bool isStopping() const { return m_shouldStop.load(std::memory_order_relaxed); }

    // Writes every byte, blocking while the pipe is full; false once the run
    // is stopping or the pipe broke.
    bool write(std::string_view bytes)
    {
        if (m_session) {
            if (isStopping())
                return false;
            m_session->feedBytes({ bytes.data(), bytes.size() });
            std::int64_t unset = -1;
            if (bytes.find("\x1b[?2026h") != std::string_view::npos)
                m_openFedAt->compare_exchange_strong(unset, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
            return true;
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            if (isStopping())
                return false;
            const ssize_t count = ::write(m_descriptor, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    void sleepFor(Clock::duration interval) const { std::this_thread::sleep_for(interval); }

private:
    int m_descriptor;
    const std::atomic<bool>& m_shouldStop;
    TerminalSession* m_session = nullptr;
    std::atomic<std::int64_t>* m_openFedAt = nullptr;
};

// What a case checks of each frame's coherence.
enum class Coherence : std::uint8_t {
    // Nothing beyond the cadence.
    Unchecked,
    // Every frame ends at the home position, as each synchronized update
    // does: a frame anywhere else was presented from inside one.
    EveryFrameAtHome,
    // One update wraps the output, opened over a screen whose cursor stands at
    // the sentinel: until the writer closes it, every frame shows that screen.
    SentinelUntilTheBlockCloses,
    // One update opens over one frame and never closes, and the writer goes
    // quiet: the rule expires the update once it has outlived its budget, and
    // the drain's capture of that frame follows.
    ExpiresUnclosedThenQuiet,
    // One update opens inside a flood and never closes: the drain's captures
    // resume once the rule expires it, and follow at the cadence.
    ExpiresUnclosedInFlood,
};

// How the case's bytes reach the model.
enum class Feeding : std::uint8_t {
    // Through a pipe the session's ingest loop drains.
    Ingest,
    // Through the session's own feed, with no ingest loop, as an embedder
    // feeds it: nothing but the frontend's own asks ages a held update.
    Fed,
};

// When the frontend asks for a frame.
enum class Ticking : std::uint8_t {
    // At every display tick, whether or not anything woke it.
    EveryRefresh,
    // Only when the pane reports a dirty screen, or when a hold's recheck it
    // booked comes due, as a frontend whose display link idles does.
    OnWake,
};

// The longest the coherence rule holds an open update: its sixty ticks.
constexpr Clock::duration updateBudget = std::chrono::microseconds(static_cast<std::int64_t>(Modes::SyncOutput::maxHoldTicks) * 1'000'000 / 60);

// A flood's frames are mostly the drain's captures: a live read stands in for
// one only at a tick no pass has captured for, a few times a run, and the
// bounds leave three times the most of those a case was measured to make. A
// sanitized Core's fill-bound passes straddle the refresh interval, so how
// many ticks a REP fill reads live there follows the machine's load: such a
// build holds a flood only to a tenth of its frames captured, a third of the
// least a case was measured to capture.
constexpr double floodCapturedShare = isCoreInstrumented ? 0.1 : 0.6;
constexpr int floodLiveReads = isCoreInstrumented ? std::numeric_limits<int>::max() : 21;

constexpr int sentinelRow = 5;
constexpr int sentinelColumn = 7;

// Where the selection stands, in visible rows.
struct SelectionPlacement {
    int row = 0;
    int fromColumn = 0;
    int toColumn = 0;
};

struct CadenceCase {
    const char* name = "";
    int rows = 24;
    int columns = 80;
    // Parsed before the selection is made and the writer starts.
    std::string prologue;
    // Rows the view stands scrolled back, holding them while output arrives.
    int viewOffset = 0;
    int scrollbackLimit = 0;
    SelectionPlacement selection;
    Clock::duration duration = 1200ms;
    // Writes from the writer's start, so the bytes it writes are built when
    // the case is made and none of the clock is the test's own formatting.
    std::function<void(Writer&)> write;
    bool shouldPostKeys = false;
    // How long after the writer starts the drain is stalled between two bites.
    std::optional<Clock::duration> stallAfter {};
    Coherence coherence = Coherence::Unchecked;
    // The least share of the frames presented in the flow windows that are
    // the drain's captures; a flood's frames are, and a live read under the
    // lock stands in for one only now and then.
    double minCapturedShare = 0;
    // The most live reads, each a wait for the model lock, the run may make.
    int maxLiveReads = std::numeric_limits<int>::max();
    // How many times the view may register its capture geometry: once, at
    // the selection, while the view's offset stands.
    int maxRegistrations = 1;
    Ticking ticking = Ticking::EveryRefresh;
    Feeding feeding = Feeding::Ingest;
};

std::int64_t microsecondsOf(Clock::time_point at) { return std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count(); }

std::optional<Clock::time_point> timeOf(const std::atomic<std::int64_t>& microseconds)
{
    const std::int64_t value = microseconds.load(std::memory_order_acquire);
    if (value < 0)
        return std::nullopt;
    return Clock::time_point(std::chrono::microseconds(value));
}

// The drain seen from the raw byte tap, which every bite passes through on
// the drain's own thread just before the bite is parsed, with the model lock
// released: where a stall lands between two bites, and when the drain
// reached a synchronized update's open and close.
struct DrainWatch {
    std::atomic<std::int64_t> stallArmedAt { -1 };
    std::atomic<std::int64_t> stallStartedAt { -1 };
    std::atomic<std::int64_t> stallEndedAt { -1 };
    std::atomic<std::int64_t> blockOpenReachedAt { -1 };
    std::atomic<std::int64_t> blockCloseReachedAt { -1 };
    // Set before the drain starts, then read on the drain's thread only.
    bool shouldWatchBlocks = false;
    std::string carriedTail;

    static void tap(void* context, std::string_view bytes)
    {
        auto* watch = static_cast<DrainWatch*>(context);
        if (watch->shouldWatchBlocks)
            watch->noteBlockMarks(bytes);
        watch->stallIfArmed();
    }

private:
    static constexpr std::string_view blockOpen = "\x1b[?2026h";
    static constexpr std::string_view blockClose = "\x1b[?2026l";

    // A mark split across two bites is found through the tail carried over.
    void noteBlockMarks(std::string_view bytes)
    {
        const std::string joined = carriedTail + std::string(bytes.substr(0, std::min(bytes.size(), blockOpen.size())));
        const auto contains = [&](std::string_view mark) {
            return joined.find(mark) != std::string::npos || bytes.find(mark) != std::string_view::npos;
        };
        const std::int64_t now = microsecondsOf(Clock::now());
        std::int64_t unset = -1;
        if (contains(blockOpen))
            blockOpenReachedAt.compare_exchange_strong(unset, now, std::memory_order_release);
        unset = -1;
        if (blockOpenReachedAt.load(std::memory_order_relaxed) >= 0 && contains(blockClose))
            blockCloseReachedAt.compare_exchange_strong(unset, now, std::memory_order_release);
        const std::size_t keep = std::min(bytes.size(), blockOpen.size() - 1);
        carriedTail.assign(bytes.substr(bytes.size() - keep));
    }

    void stallIfArmed()
    {
        const std::int64_t armedAt = stallArmedAt.load(std::memory_order_relaxed);
        if (armedAt < 0 || stallStartedAt.load(std::memory_order_relaxed) >= 0 || microsecondsOf(Clock::now()) < armedAt)
            return;
        stallStartedAt.store(microsecondsOf(Clock::now()), std::memory_order_release);
        std::this_thread::sleep_for(ownerStall);
        stallEndedAt.store(microsecondsOf(Clock::now()), std::memory_order_release);
    }
};

Runtime::SyncFrameCaptureGeometry captureGeometry(int rows, int columns)
{
    Runtime::SyncFrameCaptureGeometry geometry;
    geometry.metrics.cellWidth = 8.0f;
    geometry.metrics.cellHeight = 16.0f;
    geometry.metrics.baselineFromTop = 13.0f;
    geometry.metrics.underlineThickness = 1.0f;
    geometry.metrics.underlineDrop = 1.0f;
    geometry.metrics.strikeThickness = 1.0f;
    geometry.metrics.xHeight = 10.0f;
    geometry.glyphSlot = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
        /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
    geometry.viewportSizeInPixels = { static_cast<float>(columns) * 8.0f, static_cast<float>(rows) * 16.0f };
    geometry.style.uniformDefaultBackground = { 0.1f, 0.2f, 0.3f, 1.0f };
    return geometry;
}

Runtime::KeyPress characterPress(char character)
{
    Runtime::KeyPress press;
    press.unshiftedCodePoint = static_cast<unsigned char>(character);
    press.shiftedCodePoint = static_cast<unsigned char>(character);
    press.text[0] = static_cast<unsigned char>(character);
    press.textLength = 1;
    return press;
}

// Whether the view had a selection when its tick asked for the frame.
enum class SelectionAtAsk : std::uint8_t {
    Absent,
    Standing,
};

// A frontend's view of one pane: its selection, where it stands, what its
// last frame drew, and the frame tick it runs.
class Frontend {
public:
    Frontend(TerminalSession& session, const CadenceCase& cadenceCase, Decision decision)
        : m_session(session)
        , m_case(cadenceCase)
        , m_decision(decision)
        , m_viewOffset(cadenceCase.viewOffset)
    {
        Selection::init(m_selection);
        m_spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(cadenceCase.rows, cadenceCase.columns)));
    }

    // An on-wake frontend's wake: the drain signal Core raises for a screen
    // change, from whichever thread raised it.
    static void noteWake(void* context) { static_cast<Frontend*>(context)->m_wakes.fetch_add(1, std::memory_order_release); }

    // Selects the case's cells as a pointer gesture does and hands the
    // selection to the drain's captures, before the writer starts.
    bool select()
    {
        m_session.lockForRead();
        Terminal* terminal = m_session.terminal();
        if (terminal) {
            const SelectionPlacement& placement = m_case.selection;
            Selection::beginGesture(m_selection, *terminal, placement.row, placement.fromColumn, 1, m_viewOffset, {});
            Selection::dragGesture(m_selection, *terminal, placement.row, placement.toColumn, m_viewOffset);
            Selection::endGesture(m_selection);
        }
        m_session.unlockForRead();
        if (!Selection::hasSelection(m_selection))
            return false;
        followOutput();
        registerCaptureGeometry();
        if (m_decision == Decision::TakeFrame)
            m_session.publishSelectionForCaptures(&m_selection);
        if (m_case.ticking == Ticking::OnWake)
            m_reportedColors = m_session.readCoherentColors();
        return true;
    }

    // One display tick, at its slot on the display clock.
    void tick(int index, Clock::time_point slot)
    {
        m_slot = slot;
        if (m_case.ticking == Ticking::OnWake) {
            // Core wakes this frontend, and it takes the flags only then: a
            // booked recheck asks the withhold and draws nothing on its own.
            recheckHoldIfDue();
            const std::uint64_t wakes = m_wakes.load(std::memory_order_acquire);
            if (wakes == m_servedWakes)
                return;
            m_servedWakes = wakes;
        }
        const std::uint32_t flags = m_times.time(FrontendCall::TakeEventFlags, [&] { return m_session.takeEventFlags(); });
        const bool isScreenDirty = flags & (1u << static_cast<unsigned>(Runtime::EventKind::ScreenDirty));
        if (isScreenDirty)
            followOutput();
        if (m_case.ticking == Ticking::OnWake) {
            if (!isScreenDirty)
                return;
            reportColorsIfMoved();
        }
        static_cast<void>(m_times.time(FrontendCall::FrameClockFacts, [&] { return m_session.frameClockFactsNow(); }));
        static_cast<void>(m_times.time(FrontendCall::ReadSnapshot, [&] { return m_session.readSnapshot(); }));
        if (isCaptureViewStale())
            registerCaptureGeometry();
        if (m_case.shouldPostKeys && index % 6 == 0) {
            std::array<char, 64> encoded {};
            static_cast<void>(m_times.time(FrontendCall::SendKey, [&] { return m_session.sendKey(characterPress('x'), encoded); }));
        }
        if (m_decision == Decision::TakeFrame)
            askForFrame();
        else
            askForFrameFromPrimitives();
        if (m_case.ticking == Ticking::EveryRefresh)
            recheckHoldIfDue();
    }

    [[nodiscard]] const std::vector<Present>& presents() const { return m_presents; }
    [[nodiscard]] const CallTimes& times() const { return m_times; }
    [[nodiscard]] bool doesSelectionStand() const { return Selection::hasSelection(m_selection); }
    [[nodiscard]] int registrations() const { return m_registrations; }
    [[nodiscard]] const std::array<int, frameDeclineCount>& declines() const { return m_declines; }

private:
    // A delivery's viewport follow, as Kit's followTailOrHoldViewport runs it
    // on every delivery: the selection judged after output, and a view
    // scrolled back holding its rows.
    void followOutput()
    {
        const Screen::HistoryFacts history = m_session.readSnapshot().history;
        const Selection::OutputUpdate update = m_times.time(FrontendCall::UpdateSelectionAfterOutput,
            [&] { return m_session.updateSelectionAfterOutput(&m_selection, m_viewOffset, history); });
        const int next = static_cast<int>(update.viewport.viewOffset);
        if (update.viewport.hold != Input::ViewportHold::Hold && m_viewOffset == 0)
            return;
        if (next != m_viewOffset) {
            m_viewOffset = next;
            m_session.setPresentedViewOffset(next);
        }
    }

    // The view's half of Kit's registration key: the registered offset, held
    // over the history the view has seen since, no longer names the view's.
    [[nodiscard]] bool isCaptureViewStale() const
    {
        const int registeredNow = m_isRegisteredViewOffsetHeld
            ? Selection::heldViewOffset(m_registeredViewOffset, m_registeredViewHistory, m_selection.viewportObservedHistory)
            : m_registeredViewOffset;
        return registeredNow != m_viewOffset;
    }

    // Whether the view's offset holds its rows over history appended since
    // the view last observed it, for a reader that missed the deliveries.
    [[nodiscard]] bool isViewOffsetHeld() const { return m_selection.hasViewportObservation && !m_selection.usesLogicalViewport; }

    void registerCaptureGeometry()
    {
        Runtime::SyncFrameCaptureGeometry geometry = captureGeometry(m_case.rows, m_case.columns);
        geometry.viewOffset = m_viewOffset;
        geometry.viewHistory = m_selection.viewportObservedHistory;
        geometry.isViewOffsetHeld = isViewOffsetHeld();
        m_times.time(FrontendCall::RegisterCaptureGeometry, [&] { m_session.setSyncFrameCaptureGeometry(geometry); });
        m_registeredViewOffset = geometry.viewOffset;
        m_registeredViewHistory = geometry.viewHistory;
        m_isRegisteredViewOffsetHeld = geometry.isViewOffsetHeld;
        ++m_registrations;
    }

    void askForFrame()
    {
        const SelectionAtAsk selection = Selection::hasSelection(m_selection) ? SelectionAtAsk::Standing : SelectionAtAsk::Absent;
        const Runtime::FrameDemand demand {
            .nowTicks = PAL::monotonicTicks(),
            .selection = &m_selection,
            .selectionSpans = m_spans,
            .keepsSearchLive = m_didLastFrameDrawSearch,
        };
        Runtime::FrameSupply supply;
        const TakenFrame frame { m_times.time(FrontendCall::TakeFrame, [&] { return m_session.takeFrame(demand, supply); }) };
        switch (supply.route) {
        case FrameRoute::Captured:
            m_didLastFrameDrawSearch = false;
            presentCapture(*frame, selection, supply.selection.count);
            return;
        case FrameRoute::LiveRead:
            readLive(selection, [&](Terminal& terminal) UBIQUITTY_REQUIRES(Ubiquitty::modelLockRole) { return m_session.liveReadDecline(terminal, demand); });
            return;
        case FrameRoute::Declined:
            decline(supply.decline);
            return;
        }
    }

    // The decision a frontend makes from the session's primitives: the
    // boundary capture only for a view with no selection now or on its last
    // frame, the rule's hold, and the saturated drain's skip on the drain's
    // flag alone.
    void askForFrameFromPrimitives()
    {
        const bool hasSelection = Selection::hasSelection(m_selection);
        const SelectionAtAsk selection = hasSelection ? SelectionAtAsk::Standing : SelectionAtAsk::Absent;
        const bool didDrawSelection = m_didLastFrameDrawSelection;
        m_didLastFrameDrawSelection = hasSelection;
        const bool keepsSearchLive = m_didLastFrameDrawSearch && !m_session.isDrainSaturated();
        if (!hasSelection && !didDrawSelection && !keepsSearchLive) {
            if (const TakenFrame frame { m_times.time(FrontendCall::TakeFrame, [&] { return m_session.takeSyncFrame(); }) }) {
                m_didLastFrameDrawSearch = false;
                presentCapture(*frame, selection, 0);
                return;
            }
        }
        const Ticks now = PAL::monotonicTicks();
        if (m_times.time(FrontendCall::TakeFrame, [&] { return m_session.shouldWithholdFrame(now); })) {
            decline(FrameDecline::HeldByCoherence);
            return;
        }
        if (m_session.isDrainSaturated()) {
            decline(FrameDecline::AwaitingCapture);
            return;
        }
        readLive(selection, [&](Terminal& terminal) UBIQUITTY_REQUIRES(Ubiquitty::modelLockRole) {
            return m_session.shouldWithholdCapture(terminal, now) ? FrameDecline::BlockOpenUnderLock : FrameDecline::None;
        });
    }

    void presentCapture(const FrameCapture& frame, SelectionAtAsk selection, int selectionSpans)
    {
        const bool isFresh = frame.sequence() > m_lastSequence;
        m_lastSequence = std::max(m_lastSequence, frame.sequence());
        m_presents.push_back({ .at = m_slot,
            .takenAt = Clock::now(),
            .route = FrameRoute::Captured,
            .isFresh = isFresh,
            .didSelectionStand = selection == SelectionAtAsk::Standing,
            .selectionSpans = selectionSpans,
            .cursorRow = frame.cursorRow(),
            .cursorColumn = frame.cursorColumn() });
        afterDrawnFrame();
    }

    // What a frontend asks after each frame it draws: when anything on it
    // next animates.
    void afterDrawnFrame()
    {
        static_cast<void>(m_times.time(FrontendCall::NextAnimationDelay, [&] { return m_session.nextAnimationDelay(); }));
    }

    // The live read: the model lock, the decision's check under it, and the
    // selection resolved against the live terminal as the capture is taken,
    // over the rows the view holds across history the drain appended since
    // the view last observed it.
    template<typename DeclineUnderLock>
    void readLive(SelectionAtAsk selection, DeclineUnderLock&& declineUnderLock)
    {
        {
            const CallTimes::Stamp stamp { m_times, FrontendCall::LockForRead };
            m_session.lockForRead();
        }
        Terminal* terminal = m_session.terminal();
        FrameDecline declined = terminal ? declineUnderLock(*terminal) : FrameDecline::None;
        Present present { .at = m_slot, .takenAt = Clock::now(), .route = FrameRoute::LiveRead, .didSelectionStand = selection == SelectionAtAsk::Standing };
        if (terminal && declined == FrameDecline::None) {
            const LiveContent content { .grid = terminal->grid.get(),
                .generation = terminal->grid ? terminal->grid->contentGeneration() : 0,
                .cursorRow = terminal->screens.active->cursorY,
                .cursorColumn = terminal->screens.active->cursorX };
            present.isFresh = content != m_lastLiveContent;
            m_lastLiveContent = content;
            if (Selection::hasSelection(m_selection)) {
                Selection::ViewportEnds ends;
                const int viewOffset = isViewOffsetHeld()
                    ? Selection::heldViewOffset(m_viewOffset, m_selection.viewportObservedHistory, Screen::historyFacts(terminal))
                    : m_viewOffset;
                present.selectionSpans = Selection::viewportSpansAndEnds(m_selection, *terminal, viewOffset, m_spans, ends);
            }
            present.cursorRow = terminal->screens.active->cursorY;
            present.cursorColumn = terminal->screens.active->cursorX;
        }
        m_session.unlockForRead();
        if (!terminal)
            return;
        if (declined != FrameDecline::None) {
            decline(declined);
            return;
        }
        m_didLastFrameDrawSearch = false;
        m_presents.push_back(present);
        afterDrawnFrame();
    }

    // A declined tick keeps the frame owed, and a hold's budget is asked
    // when no recheck is booked, as Kit's deferHeldPresentation books one.
    void decline(FrameDecline declined)
    {
        ++m_declines[static_cast<std::size_t>(declined)];
        const bool isHeld = declined == FrameDecline::HeldByCoherence || declined == FrameDecline::HeldByPresentation
            || declined == FrameDecline::BlockOpenUnderLock || declined == FrameDecline::RedrawArriving;
        if (isHeld && !m_recheckAt)
            bookHoldRecheck();
    }

    void bookHoldRecheck()
    {
        const std::uint32_t ticks = m_times.time(FrontendCall::PresentationHoldTicks,
            [&] { return m_session.presentationHoldTicksRemaining(PAL::monotonicTicks()); });
        if (ticks > 0)
            m_recheckAt = Clock::now() + std::chrono::microseconds(static_cast<std::int64_t>(ticks) * 1'000'000 / 60);
    }

    void recheckHoldIfDue()
    {
        if (!m_recheckAt || Clock::now() < *m_recheckAt)
            return;
        m_recheckAt.reset();
        if (m_times.time(FrontendCall::RecheckHold, [&] { return m_session.shouldWithholdFrame(PAL::monotonicTicks()); }))
            bookHoldRecheck();
    }

    // What a host that draws nothing still owes on a screen change: the
    // colours the frame shows, once they have moved.
    void reportColorsIfMoved()
    {
        const Runtime::CoherentColors published = m_session.readCoherentColors();
        if (published.paletteEpoch == m_reportedColors.paletteEpoch)
            return;
        const ColorRGBA was = m_reportedColors.colors.background;
        const ColorRGBA now = published.colors.background;
        m_reportedColors = published;
        if (was.red != now.red || was.green != now.green || was.blue != now.blue)
            m_colorReports.push_back({ .at = m_slot, .background = now });
    }

public:
    // A change of background a host reported, and the tick it reported it at.
    struct ColorReport {
        Clock::time_point at;
        ColorRGBA background;
    };
    [[nodiscard]] const std::vector<ColorReport>& colorReports() const { return m_colorReports; }

private:
    TerminalSession& m_session;
    const CadenceCase& m_case;
    Decision m_decision;
    Selection::Session m_selection;
    std::vector<SelectionRowSpan> m_spans;
    int m_viewOffset = 0;
    int m_registeredViewOffset = 0;
    Screen::HistoryFacts m_registeredViewHistory {};
    bool m_isRegisteredViewOffsetHeld = false;
    int m_registrations = 0;
    std::array<int, frameDeclineCount> m_declines {};
    bool m_didLastFrameDrawSearch = false;
    bool m_didLastFrameDrawSelection = false;
    std::uint64_t m_lastSequence = 0;
    // What the last live read showed: the model it read moved when any of
    // this did.
    struct LiveContent {
        const void* grid = nullptr;
        std::uint64_t generation = 0;
        int cursorRow = -1;
        int cursorColumn = -1;
        friend bool operator==(const LiveContent&, const LiveContent&) = default;
    };
    LiveContent m_lastLiveContent;
    std::optional<Clock::time_point> m_recheckAt;
    std::atomic<std::uint64_t> m_wakes { 0 };
    std::uint64_t m_servedWakes = 0;
    Runtime::CoherentColors m_reportedColors {};
    std::vector<ColorReport> m_colorReports;
    Clock::time_point m_slot;
    std::vector<Present> m_presents;
    CallTimes m_times;
};

// What one case's run left behind for its checks.
struct CadenceRun {
    bool didStart = false;
    bool didSelect = false;
    bool doesSelectionStandAtEnd = false;
    Clock::time_point writerStartedAt;
    Clock::time_point writerStoppedAt;
    std::optional<Clock::time_point> blockOpenReachedAt;
    std::optional<Clock::time_point> blockCloseReachedAt;
    std::optional<Clock::time_point> stallStartedAt;
    std::optional<Clock::time_point> stallEndedAt;
    std::vector<Present> presents;
    std::vector<Frontend::ColorReport> colorReports;
    CallTimes times;
    int ticks = 0;
    int registrations = 0;
    std::array<int, frameDeclineCount> declines {};
};

// Waits until the drain has parsed `bytes` bytes, or two seconds pass.
bool waitForDrained(TerminalSession& session, std::uint64_t bytes)
{
    std::uint64_t drained = 0;
    for (int waited = 0; waited < 2000 && drained < bytes; ++waited) {
        drained += session.takeDrainedBytes();
        if (drained < bytes)
            std::this_thread::sleep_for(1ms);
    }
    return drained >= bytes;
}

CadenceRun runCadence(const CadenceCase& cadenceCase, Decision decision)
{
    CadenceRun run;
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return run;
    PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
    PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
    if (::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) != 0)
        return run;

    TerminalSession* session = TerminalSession::make();
    if (!session)
        return run;
    struct SessionRelease {
        TerminalSession* session;
        ~SessionRelease()
        {
            session->close();
            releaseTerminalSession(session);
        }
    } release { session };
    session->start(cadenceCase.rows, cadenceCase.columns);
    const bool isFed = cadenceCase.feeding == Feeding::Fed;
    if (!isFed && !session->adoptStream(std::make_unique<PAL::CadencePipeStream>(std::move(readEnd))).has_value())
        return run;
    if (cadenceCase.scrollbackLimit > 0) {
        session->lockForRead();
        if (Terminal* terminal = session->terminal())
            terminal->scrollbackLimit = cadenceCase.scrollbackLimit;
        session->unlockForRead();
    }
    DrainWatch watch;
    watch.shouldWatchBlocks = cadenceCase.coherence != Coherence::Unchecked && cadenceCase.coherence != Coherence::EveryFrameAtHome;
    session->setRawByteTap(&DrainWatch::tap, &watch);
    session->setSyncPublishRefreshSeconds(refreshSeconds);
    session->setSyncFrameCaptureGeometry(captureGeometry(cadenceCase.rows, cadenceCase.columns));
    session->setSyncFramePresentation(Runtime::PanePresentation::Presented);
    if (!isFed && !session->startIngest())
        return run;
    struct IngestStop {
        TerminalSession* session;
        ~IngestStop() { session->stopIngest(); }
    } ingestStop { session };
    run.didStart = true;

    // The screen the case starts from, parsed whole before the selection is
    // made over it.
    std::atomic<bool> isPrologueWriterStopping { false };
    if (isFed) {
        session->feedBytes({ cadenceCase.prologue.data(), cadenceCase.prologue.size() });
    } else {
        Writer prologueWriter(writeEnd.get(), isPrologueWriterStopping);
        if (!prologueWriter.write(cadenceCase.prologue) || !waitForDrained(*session, cadenceCase.prologue.size()))
            return run;
    }

    Frontend frontend(*session, cadenceCase, decision);
    run.didSelect = frontend.select();
    if (!run.didSelect)
        return run;
    // A frontend that ticks on wake has its host register the wake before its
    // first tick and clear it after its last, as a host registers a view's.
    const bool isWokenByCore = cadenceCase.ticking == Ticking::OnWake;
    if (isWokenByCore)
        session->setDrainSignal(&frontend, &Frontend::noteWake);

    std::atomic<bool> isWriterStopping { false };
    Writer writer = isFed ? Writer(*session, isWriterStopping, watch.blockOpenReachedAt) : Writer(writeEnd.get(), isWriterStopping);
    run.writerStartedAt = Clock::now();
    if (cadenceCase.stallAfter)
        watch.stallArmedAt.store(microsecondsOf(run.writerStartedAt + *cadenceCase.stallAfter), std::memory_order_relaxed);
    // The writer runs at the drain's priority, so a loaded machine starves
    // neither: the starved writer is the case that starves it on purpose.
    std::jthread writerThread([&writer, &cadenceCase] {
        PAL::raiseCurrentThreadToInteractivePriority();
        cadenceCase.write(writer);
    });

    // The frontend's thread runs at the priority a frontend's main thread
    // does, as the drain's runs at its own.
    std::jthread frontendThread([&] {
        PAL::raiseCurrentThreadToInteractivePriority();
        const Clock::time_point end = run.writerStartedAt + cadenceCase.duration;
        Clock::time_point next = run.writerStartedAt;
        int index = 0;
        while (next < end) {
            std::this_thread::sleep_until(next);
            frontend.tick(index++, next);
            next += refreshInterval;
            // A tick that overran its interval waits for the next one rather
            // than running a burst of late ones.
            const Clock::time_point now = Clock::now();
            while (next < now)
                next += refreshInterval;
        }
        run.ticks = index;
    });
    frontendThread.join();
    if (isWokenByCore)
        session->setDrainSignal(nullptr, nullptr);
    run.writerStoppedAt = Clock::now();
    isWriterStopping.store(true, std::memory_order_relaxed);
    writerThread.join();

    run.presents = frontend.presents();
    run.times = frontend.times();
    run.doesSelectionStandAtEnd = frontend.doesSelectionStand();
    run.registrations = frontend.registrations();
    run.declines = frontend.declines();
    run.colorReports = frontend.colorReports();
    run.blockOpenReachedAt = timeOf(watch.blockOpenReachedAt);
    run.blockCloseReachedAt = timeOf(watch.blockCloseReachedAt);
    run.stallStartedAt = timeOf(watch.stallStartedAt);
    run.stallEndedAt = timeOf(watch.stallEndedAt);
    return run;
}

double millisecondsOf(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

// The longest stretch inside [from, to) without a fresh frame.
Clock::duration longestGap(const std::vector<Present>& presents, Clock::time_point from, Clock::time_point to, Clock::time_point& gapEnd)
{
    Clock::duration longest {};
    Clock::time_point previous = from;
    gapEnd = from;
    for (const Present& present : presents) {
        if (!present.isFresh || present.at < from || present.at >= to)
            continue;
        if (present.at - previous > longest) {
            longest = present.at - previous;
            gapEnd = present.at;
        }
        previous = present.at;
    }
    if (to - previous > longest) {
        longest = to - previous;
        gapEnd = to;
    }
    return longest;
}

// Every check a case's run answers to.
void checkCadence(const CadenceCase& cadenceCase, const CadenceRun& run, double coreShare)
{
    const std::string name = cadenceCase.name;
    REQUIRE_MSG(run.didStart, (name + ": the session started").c_str());
    REQUIRE_MSG(run.didSelect, (name + ": the selection was made").c_str());

    // Where output flows outside a synchronized update's hold and the
    // drain's stall.
    std::vector<std::pair<Clock::time_point, Clock::time_point>> windows;
    const Clock::time_point flowFrom = run.writerStartedAt + warmUp;
    if (cadenceCase.coherence == Coherence::SentinelUntilTheBlockCloses) {
        REQUIRE_MSG(run.blockOpenReachedAt && run.blockCloseReachedAt,
            std::format("{}: the drain reached the update's open and close before the writer stopped {:.0f} ms in", name,
                millisecondsOf(run.writerStoppedAt - run.writerStartedAt))
                .c_str());
        REQUIRE_MSG(*run.blockCloseReachedAt - *run.blockOpenReachedAt < std::chrono::seconds(1),
            std::format("{}: the update closed {:.0f} ms after it opened, against the rule's one-second budget", name,
                millisecondsOf(*run.blockCloseReachedAt - *run.blockOpenReachedAt))
                .c_str());
        windows.emplace_back(*run.blockCloseReachedAt, run.writerStoppedAt);
    } else if (cadenceCase.coherence == Coherence::ExpiresUnclosedThenQuiet || cadenceCase.coherence == Coherence::ExpiresUnclosedInFlood) {
        REQUIRE_MSG(run.blockOpenReachedAt.has_value(), (name + ": the drain reached the update's open").c_str());
        const Clock::time_point open = *run.blockOpenReachedAt;
        // The rule stamps the update at the end of the pass, or the ask, that
        // first sees it open, up to one refresh interval after the open.
        const Clock::duration expiryBound = updateBudget + refreshInterval + cadenceBound;
        const Clock::time_point due = open + expiryBound;
        // The colours the program set inside the update reach the host once
        // the rule expires it, as a close would bring them.
        const auto reported = std::ranges::find_if(run.colorReports, [&](const Frontend::ColorReport& report) {
            return report.at > open && report.background.red == 0x00 && report.background.green == 0x77 && report.background.blue == 0x77;
        });
        REQUIRE_MSG(reported != run.colorReports.end(),
            std::format("{}: the host was never woken to report the colours of the update the drain reached {:.0f} ms into the writer", name,
                millisecondsOf(open - run.writerStartedAt))
                .c_str());
        CHECK_MSG(reported->at <= due,
            std::format("{}: the host reported the expired update's colours {:.0f} ms after its open, against the budget, the stamp and the cadence, {:.0f} ms",
                name, millisecondsOf(reported->at - open), millisecondsOf(expiryBound))
                .c_str());
        // A fresh frame follows the expiry; any before the budget ran out was
        // taken before the drain reached the open.
        const auto resumed = std::ranges::find_if(run.presents, [&](const Present& present) { return present.isFresh && present.at >= open + updateBudget - cadenceBound; });
        REQUIRE_MSG(resumed != run.presents.end(), (name + ": no fresh frame followed the expired update").c_str());
        CHECK_MSG(resumed->at <= due,
            std::format("{}: the first fresh frame after the open came {:.0f} ms after it, against the budget, the stamp and the cadence, {:.0f} ms", name,
                millisecondsOf(resumed->at - open), millisecondsOf(expiryBound))
                .c_str());
        if (cadenceCase.coherence == Coherence::ExpiresUnclosedInFlood)
            windows.emplace_back(resumed->at, run.writerStoppedAt);
    } else if (run.stallStartedAt) {
        REQUIRE_MSG(run.stallEndedAt.has_value(), (name + ": the stall ended").c_str());
        windows.emplace_back(flowFrom, *run.stallStartedAt);
        windows.emplace_back(*run.stallEndedAt + refreshInterval, run.writerStoppedAt);
    } else {
        windows.emplace_back(flowFrom, run.writerStoppedAt);
    }
    if (cadenceCase.stallAfter)
        CHECK_MSG(run.stallStartedAt.has_value(), (name + ": the drain was stalled").c_str());

    int presentsInWindows = 0;
    int capturedInWindows = 0;
    Clock::duration longestFlowGap {};
    for (const auto& [from, to] : windows) {
        if (to <= from)
            continue;
        Clock::time_point gapEnd;
        const Clock::duration gap = longestGap(run.presents, from, to, gapEnd);
        longestFlowGap = std::max(longestFlowGap, gap);
        CHECK_MSG(gap <= cadenceBound,
            std::format("{}: the longest stretch without a fresh frame was {:.1f} ms, ending {:.0f} ms into the writer, against {:.1f} ms",
                name, millisecondsOf(gap), millisecondsOf(gapEnd - run.writerStartedAt), millisecondsOf(cadenceBound))
                .c_str());
        presentsInWindows += static_cast<int>(std::ranges::count_if(
            run.presents, [&](const Present& present) { return present.isFresh && present.at >= from && present.at < to; }));
        capturedInWindows += static_cast<int>(std::ranges::count_if(run.presents, [&](const Present& present) {
            return present.isFresh && present.route == FrameRoute::Captured && present.at >= from && present.at < to;
        }));
    }
    const double capturedShare = presentsInWindows ? static_cast<double>(capturedInWindows) / presentsInWindows : 0;
    CHECK_MSG(capturedShare >= cadenceCase.minCapturedShare,
        std::format("{}: {} of {} fresh frames in the flow windows were the drain's captures, against at least {:.0f}%", name,
            capturedInWindows, presentsInWindows, cadenceCase.minCapturedShare * 100)
            .c_str());
    const int liveReads = run.times.count[static_cast<std::size_t>(FrontendCall::LockForRead)];
    CHECK_MSG(liveReads <= cadenceCase.maxLiveReads,
        std::format("{}: {} live reads took the model lock, against at most {}", name, liveReads, cadenceCase.maxLiveReads).c_str());
    CHECK_MSG(run.registrations <= cadenceCase.maxRegistrations,
        std::format("{}: the view registered its capture geometry {} times, against at most {}", name, run.registrations,
            cadenceCase.maxRegistrations)
            .c_str());

    int presentsWithSelection = 0;
    int presentsWithoutIt = 0;
    for (const Present& present : run.presents) {
        if (!present.didSelectionStand)
            continue;
        if (present.selectionSpans > 0)
            ++presentsWithSelection;
        else
            ++presentsWithoutIt;
    }
    CHECK_MSG(presentsWithoutIt == 0,
        std::format("{}: {} of {} frames presented while the selection stood drew none of it", name, presentsWithoutIt,
            presentsWithSelection + presentsWithoutIt)
            .c_str());
    CHECK_MSG(run.doesSelectionStandAtEnd, (name + ": the selection stood to the end, so every frame owed it").c_str());

    for (std::size_t index = 0; index < frontendCallCount; ++index) {
        if (run.times.count[index] == 0)
            continue;
        const auto call = static_cast<FrontendCall>(index);
        if (call != FrontendCall::TakeFrame) {
            CHECK_MSG(run.times.longest[index] <= frontendCallBound,
                std::format("{}: {} took {:.2f} ms, against {:.2f} ms", name, frontendCallName(call), millisecondsOf(run.times.longest[index]),
                    millisecondsOf(frontendCallBound))
                    .c_str());
        }
        const Clock::duration blocked = run.times.longestBlocked[index];
        CHECK_MSG(blocked <= frontendWaitBound,
            std::format("{}: {} waited {:.2f} ms at {:.0f} ms into the writer, against {:.2f} ms", name,
                frontendCallName(static_cast<FrontendCall>(index)), millisecondsOf(blocked),
                millisecondsOf(run.times.longestBlockedAt[index] - run.writerStartedAt), millisecondsOf(frontendWaitBound))
                .c_str());
    }
    const Clock::duration takeFrame = run.times.longest[static_cast<std::size_t>(FrontendCall::TakeFrame)];
    CHECK_MSG(takeFrame <= frameDecisionBound,
        std::format("{}: a frame decision took {:.2f} ms, against {:.2f} ms", name, millisecondsOf(takeFrame), millisecondsOf(frameDecisionBound)).c_str());

    if (cadenceCase.coherence == Coherence::EveryFrameAtHome) {
        const auto torn = std::ranges::count_if(run.presents, [](const Present& present) { return present.cursorRow != 0 || present.cursorColumn != 0; });
        CHECK_MSG(torn == 0, std::format("{}: {} frames were presented from inside a synchronized update", name, torn).c_str());
    }
    if (cadenceCase.coherence == Coherence::SentinelUntilTheBlockCloses) {
        const auto early = std::ranges::count_if(run.presents, [&](const Present& present) {
            return present.takenAt < *run.blockCloseReachedAt && (present.cursorRow != sentinelRow || present.cursorColumn != sentinelColumn);
        });
        CHECK_MSG(early == 0, std::format("{}: {} frames showed the update's content before the drain reached its close", name, early).c_str());
    }

    std::string calls;
    for (std::size_t index = 0; index < frontendCallCount; ++index) {
        if (run.times.count[index])
            calls += std::format(" {}={:.3f}/{:.3f}", frontendCallName(static_cast<FrontendCall>(index)), millisecondsOf(run.times.longest[index]),
                millisecondsOf(run.times.longestBlocked[index]));
    }
    std::string declines;
    for (std::size_t index = 0; index < run.declines.size(); ++index) {
        if (run.declines[index])
            declines += std::format(" {}={}", frameDeclineName(static_cast<FrameDecline>(index)), run.declines[index]);
    }
    std::string updateOpen;
    if (cadenceCase.coherence == Coherence::SentinelUntilTheBlockCloses)
        updateOpen = std::format(", the update open {:.0f} ms", millisecondsOf(*run.blockCloseReachedAt - *run.blockOpenReachedAt));
    std::printf("cadence %s: %.2f of a core, %d ticks, %zu presents (%d fresh in the flow windows, %d of them captures, longest gap %.1f ms%s), "
                "%d live reads, declines by reason:%s, %d registrations; longest call / longest wait (ms):%s\n",
        cadenceCase.name, coreShare, run.ticks, run.presents.size(), presentsInWindows, capturedInWindows, millisecondsOf(longestFlowGap),
        updateOpen.c_str(), liveReads, declines.c_str(), run.registrations, calls.c_str());
}

// The least share of a core any of a run's busy threads can have now: that
// many threads spin at the frontend's priority for a moment, and the
// least-served one's CPU time over the wall time is the figure. A machine
// that cannot give each of them half a core misses a 60 Hz cadence for
// reasons that are not the code's, so a case run on it is skipped, with the
// figure, rather than passed or failed.
double availableCoreShare()
{
    // The writer, the drain, the flood reader and the frontend.
    constexpr std::size_t busyThreads = 4;
    std::array<double, busyThreads> shares {};
    const Clock::time_point until = Clock::now() + 100ms;
    {
        std::vector<std::jthread> spinners;
        for (std::size_t index = 0; index < busyThreads; ++index) {
            spinners.emplace_back([&shares, index, until] {
                PAL::raiseCurrentThreadToInteractivePriority();
                const Clock::time_point start = Clock::now();
                const Clock::duration startCPU = threadCPUTime();
                while (Clock::now() < until) {
                }
                shares[index] = std::chrono::duration<double>(threadCPUTime() - startCPU).count()
                    / std::chrono::duration<double>(Clock::now() - start).count();
            });
        }
    }
    return std::ranges::min(shares);
}

constexpr double minCoreShare = 0.5;

// Runs a case between two readings of the machine, and judges it unless
// either finds less than the share a run needs.
void runAndCheck(const CadenceCase& cadenceCase)
{
    const double shareBefore = availableCoreShare();
    if (shareBefore < minCoreShare) {
        const std::string reason = std::format("{}: a busy thread could have {:.2f} of a core, below {:.2f}", cadenceCase.name, shareBefore, minCoreShare);
        UbiquittyTest::skip(reason.c_str());
        return;
    }
    const CadenceRun run = runCadence(cadenceCase, decisionUnderTest());
    const double share = std::min(shareBefore, availableCoreShare());
    if (share < minCoreShare) {
        const std::string reason
            = std::format("{}: by the run's end a busy thread could have {:.2f} of a core, below {:.2f}", cadenceCase.name, share, minCoreShare);
        UbiquittyTest::skip(reason.c_str());
        return;
    }
    checkCadence(cadenceCase, run, share);
}

// --- Workloads ---------------------------------------------------------------

// A screen whose first row stands outside a scrolling region below it, with
// the selection's text on that row: scrolling output keeps it on screen.
std::string protectedRowScreen(int rows)
{
    return std::format("\x1b[H\x1b[2Jprotected row with selected text\x1b[2;{}r\x1b[{};1H", rows, rows);
}

constexpr SelectionPlacement protectedSelection { .row = 0, .fromColumn = 4, .toColumn = 20 };

// One frame of a program that redraws every row in place, each cell under a
// truecolour SGR, ending at the home position: the sgr_churn shape.
std::string sgrChurnFrame(int rows, int columns, int frame)
{
    std::string bytes = "\x1b[H";
    for (int row = 0; row < rows; ++row) {
        if (row > 0)
            bytes += "\r\n";
        for (int column = 0; column < columns; ++column) {
            const int shade = (frame * 7 + row * 3 + column) & 0xff;
            bytes += std::format("\x1b[38;2;{};{};{}m{}", shade, 255 - shade, (shade * 5) & 0xff, static_cast<char>('a' + (frame + column) % 26));
        }
    }
    bytes += "\x1b[m\x1b[H";
    return bytes;
}

// One frame that fills every row with one character repeated by REP: eight
// bytes a row, so the parse is all fill.
std::string repFillFrame(int rows, int columns, int frame)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row)
        bytes += std::format("\x1b[{};1H{}\x1b[{}b", row + 1, static_cast<char>('a' + (frame + row) % 26), columns - 1);
    bytes += "\x1b[H";
    return bytes;
}

// One frame of a full-screen program on the alternate screen: every row
// erased and rewritten in place.
std::string redrawnInPlace(int rows, int frame)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row)
        bytes += std::format("\x1b[{};1H\x1b[2Kframe {:06} row {:02} redrawn in place", row + 1, frame, row);
    bytes += "\x1b[H";
    return bytes;
}

std::string asciiLines(int lines, int& counter)
{
    std::string bytes;
    for (int line = 0; line < lines; ++line)
        bytes += std::format("ascii flood line {:08} 0123456789abcdefghijklmnopqrstuvwxyz\r\n", counter++);
    return bytes;
}

// A writer the machine starves: a burst that elects the flood reader, then
// truecolour slices a consumer nap apart, each ending inside a sequence.
std::string asciiFlood(int lines)
{
    int counter = 0;
    return asciiLines(lines, counter);
}

std::function<void(Writer&)> starvedWriter()
{
    int counter = 0;
    std::string burst;
    while (burst.size() < 2 * static_cast<std::size_t>(Runtime::TerminalRuntime::drainPassCapacityBytes))
        burst += asciiLines(64, counter);
    std::string slice;
    while (slice.size() < 250)
        slice += "\x1b[38;2;111;222;123mx";
    slice.resize(250);
    return [burst = std::move(burst), slice = std::move(slice)](Writer& writer) {
        if (!writer.write(burst))
            return;
        while (!writer.isStopping()) {
            if (!writer.write(slice))
                return;
            writer.sleepFor(1500us);
        }
    };
}

constexpr int paneRows = 24;
constexpr int paneColumns = 80;

// A recorded stream of `frames` frames, as a program wrote them.
std::string recording(int frames, const std::function<std::string(int)>& frame)
{
    std::string bytes;
    for (int index = 1; index <= frames; ++index)
        bytes += frame(index);
    return bytes;
}

// Writes a recording over and over, as a `cat` of it in a loop does, until
// the run stops, `until` passes or `maxBytes` are written: the writer outruns
// the parse, so the drain is the bottleneck.
void replay(Writer& writer, const std::string& bytes, std::optional<Clock::time_point> until = std::nullopt,
    std::size_t maxBytes = std::numeric_limits<std::size_t>::max())
{
    for (std::size_t written = 0; (!until || Clock::now() < *until) && written < maxBytes; written += bytes.size()) {
        if (!writer.write(bytes))
            return;
    }
}

std::string sgrChurnRecording(int rows, int columns)
{
    return recording(16, [rows, columns](int frame) { return sgrChurnFrame(rows, columns, frame); });
}

std::string redrawnInPlaceRecording()
{
    return recording(32, [](int frame) { return redrawnInPlace(paneRows, frame); });
}

} // namespace

// A plain ASCII flood scrolling below a selection on the row outside its
// region.
TEST(cadenceOfAnASCIIFlood)
{
    runAndCheck({
        .name = "ascii flood",
        .prologue = protectedRowScreen(paneRows),
        .selection = protectedSelection,
        .write = [lines = asciiFlood(4096)](Writer& writer) { replay(writer, lines); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// The sgr_churn shape, every cell under its own truecolour SGR redrawn in
// place, with a selection on a row it redraws: the drain is parse-bound, so
// every pass ends on its budget, and a frame that waited for the model lock
// would wait behind every bite.
TEST(cadenceOfAnSGRChurnFlood)
{
    runAndCheck({
        .name = "sgr_churn flood",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .write = [frames = sgrChurnRecording(paneRows, paneColumns)](Writer& writer) { replay(writer, frames); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// The same at the geometry a maximized window has.
TEST(cadenceOfAnSGRChurnFloodAtTwoHundredColumns)
{
    runAndCheck({
        .name = "sgr_churn flood at 200x50",
        .rows = 50,
        .columns = 200,
        .prologue = sgrChurnFrame(50, 200, 0),
        .selection = { .row = 20, .fromColumn = 10, .toColumn = 120 },
        .write = [frames = sgrChurnRecording(50, 200)](Writer& writer) { replay(writer, frames); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// REP filling every row: eight bytes a row, the parse all fill.
TEST(cadenceOfARepFillFlood)
{
    runAndCheck({
        .name = "rep_fill flood",
        .prologue = repFillFrame(paneRows, paneColumns, 0),
        .selection = { .row = 3, .fromColumn = 0, .toColumn = 30 },
        .write = [frames = recording(64, [](int frame) { return repFillFrame(paneRows, paneColumns, frame); })](Writer& writer) { replay(writer, frames); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// A writer the machine starves: passes close at a would-block long before
// their budget, and each pass's frame must still reach the glass.
TEST(cadenceOfAStarvedWriter)
{
    runAndCheck({
        .name = "starved writer",
        .prologue = protectedRowScreen(paneRows),
        .selection = protectedSelection,
        .duration = 1500ms,
        .write = starvedWriter(),
        .maxLiveReads = floodLiveReads,
    });
}

// A full-screen program on the alternate screen redrawing every row in place,
// with a selection on one of them that every redraw rewrites.
TEST(cadenceOfAnAlternateScreenRedrawnInPlace)
{
    runAndCheck({
        .name = "alternate screen redrawn in place",
        .prologue = "\x1b[?1049h" + redrawnInPlace(paneRows, 0),
        .selection = { .row = 10, .fromColumn = 0, .toColumn = 25 },
        .write = [frames = redrawnInPlaceRecording()](Writer& writer) { replay(writer, frames); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// Each frame its own synchronized update: frames present at the updates'
// ends, at the cadence, and never from inside one.
TEST(cadenceOfSynchronizedUpdatesOnePerFrame)
{
    runAndCheck({
        .name = "one synchronized update per frame",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .write = [frames = recording(16, [](int frame) { return "\x1b[?2026h" + sgrChurnFrame(paneRows, paneColumns, frame) + "\x1b[?2026l"; })](
                     Writer& writer) { replay(writer, frames); },
        .coherence = Coherence::EveryFrameAtHome,
        .maxLiveReads = floodLiveReads,
    });
}

// One synchronized update around `updateBytes` of a flood, then the flood
// without one: the screen the update opened over stands until the drain
// reaches its close, inside the rule's one-second budget, and frames follow at
// the cadence from the close.
TEST(cadenceOfOneSynchronizedUpdateAroundTheStream)
{
    runAndCheck({
        .name = "one synchronized update around the stream",
        .prologue = "\x1b[?1049h" + redrawnInPlace(paneRows, 0) + std::format("\x1b[{};{}H", sentinelRow + 1, sentinelColumn + 1),
        .selection = { .row = 10, .fromColumn = 0, .toColumn = 25 },
        .write =
            [frames = redrawnInPlaceRecording()](Writer& writer) {
                if (!writer.write("\x1b[?2026h"))
                    return;
                replay(writer, frames, std::nullopt, updateBytes);
                if (!writer.write("\x1b[?2026l"))
                    return;
                replay(writer, frames);
            },
        .coherence = Coherence::SentinelUntilTheBlockCloses,
        .maxLiveReads = floodLiveReads,
    });
}

// A view scrolled back over a selection in the history while a flood redraws
// the screen below it and scrolls a row into the history each frame: the view
// holds its rows, and its registration stands.
TEST(cadenceOfAViewScrolledBack)
{
    std::string history = "\x1b[H\x1b[2J";
    for (int line = 0; line < 60; ++line)
        history += std::format("history line {:03} kept in view\r\n", line);
    runAndCheck({
        .name = "view scrolled back",
        .prologue = history,
        .viewOffset = 20,
        .scrollbackLimit = 50'000,
        .selection = { .row = 4, .fromColumn = 0, .toColumn = 20 },
        .write =
            [frames = recording(16, [](int frame) {
                return sgrChurnFrame(paneRows - 1, paneColumns, frame) + std::format("\x1b[{};1H\r\nscrolled line {:06}", paneRows, frame);
            })](Writer& writer) { replay(writer, frames); },
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// Keys typed through an sgr_churn flood, ten a second: each key's encoding
// and activity note take the model lock.
TEST(cadenceOfKeysTypedThroughAFlood)
{
    runAndCheck({
        .name = "keys through a flood",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .write = [frames = sgrChurnRecording(paneRows, paneColumns)](Writer& writer) { replay(writer, frames); },
        .shouldPostKeys = true,
        .minCapturedShare = floodCapturedShare,
        .maxLiveReads = floodLiveReads,
    });
}

// The drain stalled for half a second between two bites of an sgr_churn
// flood, the model lock released while it stalls, as a reader thread starved
// of the CPU stalls it: every frontend call still answers inside the bound,
// and frames resume at the cadence after it.
TEST(cadenceAcrossAnOwnerStall)
{
    runAndCheck({
        .name = "owner stall",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .duration = 1500ms,
        .write = [frames = sgrChurnRecording(paneRows, paneColumns)](Writer& writer) { replay(writer, frames); },
        .shouldPostKeys = true,
        .stallAfter = 400ms,
    });
}

// A program that opens a synchronized update over one frame, sets a colour
// inside it and goes quiet without closing it, as one killed mid-frame does,
// fed to a session with no ingest loop as an embedder feeds it. Nothing but
// the frontend's own recheck ages the update, and a frontend whose display
// link idles takes the pane's flags only when Core wakes it: the colour and
// the frame the expired update held reach it only if the expiry wakes it,
// within the rule's budget and the cadence of the feed that opened it.
TEST(cadenceOfAnUpdateLeftOpenThenQuiet)
{
    runAndCheck({
        .name = "update left open, then quiet",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .duration = 1800ms,
        .write =
            [opening = "\x1b[?2026h\x1b]11;rgb:00/77/77\x07" + sgrChurnFrame(paneRows, paneColumns, 1)](Writer& writer) {
                if (!writer.write(opening))
                    return;
                while (!writer.isStopping())
                    writer.sleepFor(5ms);
            },
        .coherence = Coherence::ExpiresUnclosedThenQuiet,
        .ticking = Ticking::OnWake,
        .feeding = Feeding::Fed,
    });
}

// An update opened inside a flood, with a colour set inside it, and never
// closed: the drain parses on inside it, and once the rule expires it the
// colour reaches a frontend that takes the pane's flags only when Core wakes
// it, and the drain's captures resume at the cadence.
TEST(cadenceOfAnUpdateLeftOpenInAFlood)
{
    runAndCheck({
        .name = "update left open in a flood",
        .prologue = sgrChurnFrame(paneRows, paneColumns, 0),
        .selection = { .row = 6, .fromColumn = 2, .toColumn = 40 },
        .duration = 2000ms,
        .write =
            [frames = sgrChurnRecording(paneRows, paneColumns)](Writer& writer) {
                replay(writer, frames, std::nullopt, updateBytes / 4);
                if (!writer.write("\x1b[?2026h\x1b]11;rgb:00/77/77\x07"))
                    return;
                replay(writer, frames);
            },
        .coherence = Coherence::ExpiresUnclosedInFlood,
        .ticking = Ticking::OnWake,
    });
}

UBIQUITTY_TEST_MAIN()
