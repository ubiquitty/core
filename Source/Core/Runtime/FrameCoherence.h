// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// FrameCoherence.h — when a pane's frame may be taken, and why it was.
//
// One rule for every publisher: the host daemon deciding when its clients are
// sent a frame, the loopback oracle replaying the corpus through the daemon's
// decisions, and `TerminalRuntime::publishSettledFrameIfCoherent`, where the
// local frontends' frames are announced. A frame is taken only when all of
// these hold:
//
//   1. the pane changed since the last frame;
//   2. the parser is at ground;
//   3. no DEC 2026 block is open, or the block's own budget expired it;
//   4. no redraw bracket is open;
//   5. the PTY read is quiescent: the pass ended at a would-block, nothing was
//      read for `readQuiescenceMicroseconds` since, and a zero-timeout poll
//      finds nothing waiting. A DEC 2026 block closed since the last frame
//      waives this one: the program has said its frame is complete. So does a
//      writer still writing a frame interval after the pane went dirty, whose
//      frame is then a `Flood`: a read that never goes quiet would otherwise
//      be shown only on the deadline.
//
// A paced rule takes no frame closer than the frame interval to the last one,
// in any class; the frame waits, it is not dropped. A host that paces frames
// itself runs the rule unpaced.
//
// A redraw bracket is a non-2026 program's way of saying it is repainting: it
// hides the cursor (DECTCEM reset), writes, and shows the cursor again. The
// bracket opens when a drain pass ends with the cursor hidden and it was shown
// at the last frame, or hidden again since; it closes when the cursor is shown.
// A cursor already hidden at the last frame and kept hidden opens none, so a
// program that hides it for its whole life (a process monitor, a pager, an
// Ink-style agent UI) is never held by one. A bracket open longer than the
// deadline is *persistent*: it stops holding until the cursor is next shown, so
// a program that hides the cursor for an input burst pays the deadline once,
// not on every echo.
//
// Two escapes keep the rule from starving a client. A pane dirty for longer
// than the deadline is taken whatever holds it, and the frame is `Forced`. A
// drain pass that ended at its byte budget is a flood: the writer is ahead and
// will stay ahead, so its frames are taken at once, in any parser state, and
// marked `Flood` (or `Forced` when cut mid-sequence). An elapsed presentation
// budget also admits the already-applied state mid-sequence, but still honors
// a cursor-hidden redraw bracket. Both budget paths retain 2026 and pacing.
//
// Every frame is put in one class, the first that applies:
//
//   Structural  the first frame, or one whose geometry changed;
//   Forced      taken while incoherent: under the deadline, a flood cut
//               mid-sequence, or after a DEC 2026 block expired on its
//               budget or a bracket turned persistent since the last frame;
//   Flood       taken while the writer is still writing: after a drain pass
//               that ended at its byte or presentation budget, or a frame interval into a
//               read that never went quiet;
//   Boundary    taken after the program closed a DEC 2026 block;
//   Settled     anything else: ordinary output that went quiet.
//
// Time is the caller's: microseconds on a monotonic clock, and the 60 Hz tick
// a 2026 block ages against. Nothing here reads a clock or sleeps, so a replay
// on a recorded clock takes exactly the frames a live host would.
//
// Single-threaded: one instance per pane, owned by whoever drains it.

#include "Base/Types.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Runtime {

/// The display refresh interval the deadline, the pacing and the wait for a
/// writer that never goes quiet derive from: 60 Hz, the fastest refresh the
/// shipped wire was paced for. Per-attachment refresh intervals replace it
/// when the wire learns them.
inline constexpr std::uint64_t refreshIntervalMicroseconds = 16'667;

/// How long dirty state may wait before it is taken anyway, as a `Forced`
/// frame; also how long a redraw bracket holds before it turns persistent.
///
/// Twice the refresh interval, bounding a held screen to two missed refreshes:
/// long enough to keep a coherent repaint whole, short enough that a
/// hidden-cursor program cannot stall a client for more than two refreshes.
inline constexpr std::uint64_t coherenceDeadlineMicroseconds = 2 * refreshIntervalMicroseconds;

/// How long the PTY must stay unread after a would-block before the read
/// counts as quiescent.
///
/// A zero-timeout poll right after the would-block is the floor, and on the
/// Sync corpus it is not enough: with no window 17 of the Ink-style spinner's
/// frames (its cursor is hidden throughout, so no bracket holds them) are
/// published half drawn, with 1 ms 9, with 1.5 ms none. Those gaps are the
/// synthetic generator's, not a recording's: `generate.py` splits a spinner
/// frame with a gap drawn from 0.3 to 1.5 ms, an assumption about an Ink app
/// on an unloaded host that no recorded trace yet confirms. So the window is a
/// design choice with a third of margin over that assumed bound, not a measured
/// one. Its cost is measured: it merges writes closer than it that the corpus
/// does not mark as one repaint (shell output and the prompt 1.17 ms after it),
/// which costs a frame, and it is the p50 and p99 hold after input on the
/// corpus (2.00 ms).
inline constexpr std::uint64_t readQuiescenceMicroseconds = 2'000;

/// Why a frame was taken.
enum class FrameClass : std::uint8_t {
    Boundary = 0,
    Settled = 1,
    Flood = 2,
    Forced = 3,
    Structural = 4,
};

/// How many `FrameClass` values there are.
inline constexpr std::size_t frameClassCount = 5;

/// Returns a frame class's name, as the reap summary and the measurements
/// spell it.
///
/// - Parameter frameClass: The class.
/// - Returns: A static lowercase name.
[[nodiscard]] constexpr const char* frameClassName(FrameClass frameClass)
{
    switch (frameClass) {
    case FrameClass::Boundary:
        return "boundary";
    case FrameClass::Settled:
        return "settled";
    case FrameClass::Flood:
        return "flood";
    case FrameClass::Forced:
        return "forced";
    case FrameClass::Structural:
        return "structural";
    }
    return "unknown";
}

/// The class a frame travels under on the Sync wire.
///
/// - Parameter frameClass: The class the rule took the frame in.
/// - Returns: The wire's class of the same meaning.
[[nodiscard]] constexpr Protocol::Sync::FrameClass wireFrameClass(FrameClass frameClass)
{
    switch (frameClass) {
    case FrameClass::Boundary:
        return Protocol::Sync::FrameClass::Boundary;
    case FrameClass::Settled:
        return Protocol::Sync::FrameClass::Settled;
    case FrameClass::Flood:
        return Protocol::Sync::FrameClass::Flood;
    case FrameClass::Forced:
        return Protocol::Sync::FrameClass::Forced;
    case FrameClass::Structural:
        return Protocol::Sync::FrameClass::Structural;
    }
    return Protocol::Sync::FrameClass::Structural;
}

/// How a drain pass ended.
enum class PassEnd : std::uint8_t {
    /// The PTY had nothing more to give.
    WouldBlock,
    /// The pass spent its byte budget with the PTY still readable.
    Capacity,
    /// The presentation interval elapsed while the reader was still working.
    /// Unlike Capacity, this keeps the redraw-bracket guard.
    TimeBudget,
    /// Input remains pending without an unconsumed budget yield. This retains
    /// parser-ground and redraw-bracket guards.
    Pending,
};

/// What a zero-timeout poll of the PTY found, asked just before the rule is.
enum class ReadReadiness : std::uint8_t {
    /// Nothing is waiting.
    Quiet,
    /// The reader has not observed quiet input.
    Pending,
};

/// The rule's verdict on one question.
struct FrameAdmission {
    /// Whether a frame may be taken now.
    bool isAdmitted = false;
    /// The class the frame is taken in; meaningful when admitted.
    FrameClass frameClass = FrameClass::Settled;
};

/// The constants the rule runs on; the defaults are the shipped ones.
///
/// A test sweeps them to show what another value would have measured.
struct CoherenceTuning {
    std::uint64_t deadlineMicroseconds = coherenceDeadlineMicroseconds;
    std::uint64_t quiescenceMicroseconds = readQuiescenceMicroseconds;
    /// The interval frames are paced to, and how long a writer that never
    /// goes quiet waits for its frame.
    std::uint64_t frameIntervalMicroseconds = refreshIntervalMicroseconds;
    /// Whether no two frames are taken closer than the frame interval. Off
    /// where the host paces its frames itself.
    bool isPaced = true;
    /// Whether a bracket outliving the deadline stops holding. Off only to
    /// measure what the persistent rule saves.
    bool isPersistentBracketEnabled = true;
    bool operator==(const CoherenceTuning&) const = default;
};

/// One pane's coherence rule: when its next frame may be taken.
class FrameCoherence {
public:
    /// The redraw bracket's retained phase.
    enum class Bracket : std::uint8_t {
        /// No bracket: the cursor is shown or stayed hidden since the last frame.
        None,
        /// A bracket younger than the deadline holds the frame.
        Holding,
        /// The bracket outlived its deadline and holds nothing until the cursor shows.
        Persistent,
    };

    /// Exact scheduling state on the original monotonic clock and terminal counters.
    struct Continuation {
        CoherenceTuning tuning;
        std::uint64_t dirtySinceMicroseconds = 0;
        std::uint64_t lastPassEndMicroseconds = 0;
        std::uint64_t lastFrameMicroseconds = 0;
        std::uint64_t bracketOpenedMicroseconds = 0;
        std::uint32_t bracketHideCount = 0;
        std::uint32_t cursorHideCountAtLastFrame = 0;
        std::uint32_t syncCloseCountAtLastFrame = 0;
        std::uint32_t syncCloseCountAtLastPass = 0;
        std::int32_t frameRows = 0;
        std::int32_t frameColumns = 0;
        Bracket bracket = Bracket::None;
        PassEnd lastPassEnd = PassEnd::WouldBlock;
        bool isDirty = false;
        bool hasTakenFrame = false;
        bool hasPassEnded = false;
        bool wasCursorVisibleAtLastFrame = true;
        bool wasComposingAtLastPass = false;
        bool didExpireSinceLastFrame = false;
        bool operator==(const Continuation&) const = default;
    };

    /// Copies all scheduling state without asking, aging or taking a frame.
    /// - Returns: Exact bounded state, or nothing for unrepresentable deadlines.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Checks portable scheduling invariants without consulting a terminal or clock.
    /// - Parameter state: The candidate on its original monotonic clock.
    /// - Returns: Whether every phase, geometry and deadline is representable.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state);

    /// Restores a rule without aging the terminal or rebasing any clock.
    /// The caller carries the matching terminal counters/modes and uses the same
    /// monotonic clock. Receiver tuning must match the candidate exactly.
    /// - Parameter state: The portable candidate.
    /// - Returns: True on replacement; invalid state leaves the receiver unchanged.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    FrameCoherence() = default;
    explicit FrameCoherence(const CoherenceTuning& tuning)
        : m_tuning(tuning)
    {
    }

    /// Notes the end of a drain pass that parsed bytes.
    ///
    /// The pass end is the only instant a host can act on, so it is where the
    /// cursor is sampled for a bracket and where quiescence starts counting.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, after the pass.
    ///   - nowMicroseconds: The caller's monotonic clock.
    ///   - end: Why the pass ended.
    void noteDrainPass(const Terminal& terminal, std::uint64_t nowMicroseconds, PassEnd end);

    /// Notes a DEC 2026 block the program closed in the middle of a read, with
    /// more of the read still to parse.
    ///
    /// The close is the only instant that frame exists, so a host that takes
    /// frames there asks `admit` at once, with the read still pending. The
    /// pass has not ended, so quiescence keeps counting from the last one.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, stopped at the close.
    ///   - nowMicroseconds: The caller's monotonic clock.
    void noteMidReadClose(const Terminal& terminal, std::uint64_t nowMicroseconds);

    /// Notes a change the host made to the pane without the program writing:
    /// a resize, a palette, a clear, an adopted screen.
    ///
    /// Marks the pane dirty without starting a pass, so quiescence is judged
    /// against the program's last write.
    ///
    /// - Parameter nowMicroseconds: The caller's monotonic clock.
    void noteHostChange(std::uint64_t nowMicroseconds);

    /// Asks whether the pane's frame may be taken now.
    ///
    /// Not const: asking ages an open 2026 block towards its expiry and ages a
    /// bracket towards persistence, so a caller holding a frame must keep
    /// asking, on the schedule `microsecondsUntilAsk` names.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal.
    ///   - nowMicroseconds: The caller's monotonic clock.
    ///   - ticks: The 60 Hz clock a 2026 block ages against.
    ///   - readiness: What a zero-timeout poll of the PTY just found.
    /// - Returns: Whether to take the frame, and its class. A refusal defers
    ///   the frame; the pane stays dirty.
    [[nodiscard]] FrameAdmission admit(Terminal& terminal, std::uint64_t nowMicroseconds, Ticks ticks, ReadReadiness readiness);

    /// Notes that the frame was taken.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, as the frame shows it.
    ///   - nowMicroseconds: The caller's monotonic clock.
    void noteFrameTaken(const Terminal& terminal, std::uint64_t nowMicroseconds);

    /// Returns how long a caller holding a dirty pane may wait before asking
    /// again, or nothing when no frame is owed.
    ///
    /// A pane whose next frame waits on bytes (the parser mid-sequence, an
    /// open bracket) is still asked by the deadline, so a program that went
    /// quiet inside a redraw is shown within it.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal.
    ///   - nowMicroseconds: The caller's monotonic clock.
    ///   - ticks: The 60 Hz clock a 2026 block ages against.
    /// - Returns: Microseconds until `admit` may answer differently.
    [[nodiscard]] std::optional<std::uint64_t> microsecondsUntilAsk(const Terminal& terminal, std::uint64_t nowMicroseconds, Ticks ticks) const;

    /// Whether the pane changed since its last frame.
    [[nodiscard]] bool isDirty() const { return m_isDirty; }

    /// When the pane first changed since its last frame, or `nullopt` when it
    /// has not.
    [[nodiscard]] std::optional<std::uint64_t> dirtySinceMicroseconds() const { return m_isDirty ? std::optional(m_dirtySinceMicroseconds) : std::nullopt; }

    /// Whether a redraw bracket is holding the pane's next frame.
    [[nodiscard]] bool isBracketHolding() const { return m_bracket == Bracket::Holding; }

    /// The constants this rule runs on.
    [[nodiscard]] const CoherenceTuning& tuning() const { return m_tuning; }

    /// Paces the rule to the interval a host learns after the rule is made,
    /// such as a display's refresh.
    ///
    /// - Parameter frameIntervalMicroseconds: The interval to pace frames to;
    ///   zero runs the rule unpaced, with a writer that never goes quiet
    ///   waiting the refresh interval for its frame.
    void setFramePacing(std::uint64_t frameIntervalMicroseconds)
    {
        m_tuning.isPaced = frameIntervalMicroseconds != 0;
        m_tuning.frameIntervalMicroseconds = m_tuning.isPaced ? frameIntervalMicroseconds : refreshIntervalMicroseconds;
    }

private:
    /// Opens, continues or closes the bracket from the cursor as it stands.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal.
    ///   - nowMicroseconds: The caller's clock, stamped on a bracket it opens.
    void observeCursor(const Terminal& terminal, std::uint64_t nowMicroseconds);

    /// Makes a holding bracket persistent once it has outlived the deadline,
    /// and remembers that the frame it releases is the deadline's.
    ///
    /// - Parameter nowMicroseconds: The caller's clock.
    void agePersistence(std::uint64_t nowMicroseconds);

    /// Remembers a DEC 2026 block that ended without the program closing it:
    /// open at the last pass, shut now, and no close counted since.
    ///
    /// - Parameter terminal: The pane's terminal.
    void noteBlockExpiry(const Terminal& terminal);

    /// Returns how long the pacing still holds the next frame.
    ///
    /// - Parameter nowMicroseconds: The caller's clock.
    /// - Returns: Zero when the rule is unpaced or the interval has passed.
    [[nodiscard]] std::uint64_t pacingRemaining(std::uint64_t nowMicroseconds) const;

    /// Whether the pass budget releases the parser-ground guard. The caller
    /// still enforces synchronized output and presentation pacing.
    [[nodiscard]] bool canTakeBudgetedFrame() const;

    CoherenceTuning m_tuning;
    std::uint64_t m_dirtySinceMicroseconds = 0;
    std::uint64_t m_lastPassEndMicroseconds = 0;
    std::uint64_t m_lastFrameMicroseconds = 0;
    std::uint64_t m_bracketOpenedMicroseconds = 0;
    std::uint32_t m_bracketHideCount = 0;
    std::uint32_t m_cursorHideCountAtLastFrame = 0;
    std::uint32_t m_syncCloseCountAtLastFrame = 0;
    std::uint32_t m_syncCloseCountAtLastPass = 0;
    int m_frameRows = 0;
    int m_frameColumns = 0;
    Bracket m_bracket = Bracket::None;
    PassEnd m_lastPassEnd = PassEnd::WouldBlock;
    bool m_isDirty = false;
    bool m_hasTakenFrame = false;
    bool m_hasPassEnded = false;
    // A fresh pane shows its cursor, and so does the attach keyframe a client
    // starts from, so the first hide opens a bracket like any other.
    bool m_wasCursorVisibleAtLastFrame = true;
    bool m_wasComposingAtLastPass = false;
    // Set when a bracket turns persistent or a 2026 block expires, and kept
    // until a frame is taken: the ask that sees the expiry may be refused, and
    // the frame taken later is still the expiry's.
    bool m_didExpireSinceLastFrame = false;
};

} // namespace Ubiquitty::Runtime
