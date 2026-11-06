// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/FallibleVector.h"
#include "Base/SwiftInterop.h"
#include "Core/Input/PointerInput.h"
#include "Core/Input/PointerLockBreakout.h"
#include "Core/Input/PointerReportingPolicy.h"
#include "Core/Input/TransientHint.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Screen/ScrollTuning.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

// The pointer ladder every frontend keeps: relative pointer lock, the DEC
// locator, Tek GIN, the mode-1001 hilite gesture, xterm mouse reporting with
// its dedup, and the local selection gesture, in the order the terminal's
// modes rank them; and beside it the wheel ladder -- report, alternate-scroll
// keys, or the local view. A frontend turns its toolkit's event into a
// `PointerFact` or a `ScrollFact`, runs it here under its model lock, writes
// the returned bytes to the host and applies the verdicts. Arms that keep
// these ladders by hand reach different answers about which modifier keeps a
// drag local, whether a reported press drops the selection, and how many wheel
// reports a trackpad flick is worth; here each question has one answer.

namespace Ubiquitty::Input {

/// One pointer fact, as a frontend's event loop saw it — coordinates and
/// modifiers, no decisions.
struct PointerFact {
    /// What the pointer did.
    enum class Kind : std::uint8_t {
        Press UBIQUITTY_SWIFT_NAME(press),
        Release UBIQUITTY_SWIFT_NAME(release),
        Motion UBIQUITTY_SWIFT_NAME(motion),
    };
    Kind kind = Kind::Motion;
    /// The button, in xterm's identity space; ignored for a plain move.
    PointerButton button = PointerButton::Left;
    /// The cell under the pointer, zero-based, insets already applied.
    std::ptrdiff_t row = 0;
    std::ptrdiff_t column = 0;
    /// The device pixel, 1-based from the grid origin, for SGR-pixels.
    std::ptrdiff_t pixelX = -1;
    std::ptrdiff_t pixelY = -1;
    /// Where the pointer is along the row, in cells from the grid's left
    /// edge, unrounded. The caret a plain press or drag stands on is resolved
    /// from it by the ladder, under the model lock it already holds: the
    /// nearest cell boundary, and inside a proportional run the letter the
    /// run's layout puts there, so a drag through a run moves a letter at a
    /// time. It reaches the selection and no further: `row` and `column` stay
    /// the cell under the pointer, which is what every report rung reads,
    /// because an application's mouse protocol is in cells and a reader's
    /// caret is not the program's cursor. Non-finite from a frontend that
    /// cannot measure it, which keeps the gesture on whole cells.
    double xInCells = std::numeric_limits<double>::quiet_NaN();
    /// Where the pointer is down the viewport, in rows from its top,
    /// unrounded and unclamped: negative above the grid, at least the row
    /// count below it. A linear selection drag beyond either edge takes the
    /// edge row through its start or end. Inside a rich text block, whose
    /// lines are not rows, its layout resolves the line under this point.
    /// Non-finite from a frontend that cannot measure it, which keeps the
    /// cell coordinates and takes the middle of `row` inside a rich text block.
    double yInRows = std::numeric_limits<double>::quiet_NaN();
    /// The motion since the last event, in device pixels, signed. Under
    /// relative pointer lock (DECSET 7403) this is what the host receives;
    /// a frontend that cannot measure it leaves zero, which reports nothing.
    std::ptrdiff_t deltaX = 0;
    std::ptrdiff_t deltaY = 0;
    /// The held modifiers, as facts rather than a platform mask.
    bool hasShift = false;
    bool hasAlternate = false;
    bool hasControl = false;
    bool hasCommand = false;
    /// The OS's click count — its double-click speed already applied.
    std::ptrdiff_t clickCount = 1;
    /// Rows the viewport is scrolled back.
    std::ptrdiff_t viewOffset = 0;
    /// Whether a button is down and the motion extends a gesture.
    bool isDragging = false;
    /// Whether this press is the one that brought an inactive window
    /// forward -- AppKit asks `acceptsFirstMouse:` for exactly that press.
    /// Core keeps a standing selection through it.
    bool isWindowActivating = false;
    /// The buttons held at this event, as DECLRP's Pb bitmap built with
    /// `Mouse::Locator::buttonBit`; zero when the frontend cannot say.
    std::ptrdiff_t heldButtonBits = 0;
    /// Whether the reader has modifier keys to reach through with.
    ///
    /// A device fact, carried on the event like the modifiers themselves
    /// because the one rung that reads it is a drag: naming ⌘ or ⇧ to a reader
    /// holding a keyboard-less tablet describes a gesture they cannot perform.
    /// Every arm with a keyboard leaves it true.
    bool hasModifierKeys = true;
    /// Whether this press, and the drag and release that continue it, are the
    /// platform's own selection gesture, which on a touch surface is the only
    /// way to select because touch has no modifier to hold.
    ///
    /// A device fact, like ``hasModifierKeys``: the frontend says which of its
    /// gestures the platform reserves for selecting text, and the ladder keeps
    /// it local the way it keeps a Shift drag, however the application has
    /// asked for the mouse. A pointer that has a selection modifier to hold
    /// never reports it, so every desktop arm leaves it false.
    bool isSelectionGesture = false;
};

/// The modifiers a gesture carries, for a question that is only about them.
///
/// Also how a configured chord is carried, because the two are compared: a
/// chord is what the reader must hold, a held set is what they do hold, and
/// both are the same four answers.
struct HeldModifiers {
    bool hasCommand = false;
    bool hasControl = false;
    bool hasAlternate = false;
    bool hasShift = false;

    friend bool operator==(const HeldModifiers&, const HeldModifiers&) = default;
};

/// Whether a held set is exactly a chord, and that chord names something.
///
/// Exact is the gesture's whole separation from its neighbours. Armed by "this
/// modifier is among the ones down", a chord also fires for every larger chord
/// containing it: a block chord of Option would arm under Shift+Option too, and
/// the reader reaching for one gesture gets two. A chord that names no modifier
/// is a gesture switched off, and must not match the bare pointer — which names
/// no modifier either, and is how every plain drag arrives.
///
/// - Parameters:
///   - held: What the reader holds.
///   - chord: What the gesture asks for.
/// - Returns: Whether this gesture is armed.
[[nodiscard]] inline bool chordArmed(HeldModifiers held, HeldModifiers chord)
{
    const bool isNamingAModifier = chord.hasCommand || chord.hasControl || chord.hasAlternate || chord.hasShift;
    return isNamingAModifier && held == chord;
}

/// The chord a preference's text names — `"cmd+shift"`, `"opt"`, `"ctrl+opt"`.
///
/// Names are folded to lower case and joined by `+`; each of Command, Control,
/// Option and Shift answers to the spellings its platforms use. A name this
/// does not know contributes nothing, so a misspelt spec switches its gesture
/// off rather than arming a different one.
///
/// Called where a preference is read and not where a pointer is handled: the
/// chord is four bools to compare, while the text costs a scan, and a pointer
/// move asks this question of every gesture.
///
/// - Parameter spec: The preference's text.
/// - Returns: The chord it names, naming nothing if the text does.
[[nodiscard]] HeldModifiers chordFromSpec(std::string_view spec);

/// The preferences the ladder reads, resolved once by the owner.
struct PointerPolicy {
    /// Whether a double click grows to a URL/path/hash token.
    Selection::SmartTokenMode smartToken = Selection::SmartTokenMode::Disabled;
    /// The pane is a tmux -CC surface this program renders itself.
    bool isControlModePane = false;
    /// [selection] links_override_reporting.
    bool isLinkOverrideEnabled = false;
    /// [selection] block_select_modifier — the chord that makes a drag
    /// rectangular, matched exactly so it cannot also read as the Shift
    /// bypass that reaches a selection through a tracking application. Held
    /// parsed rather than as the preference's text, which the policy does not
    /// own and could not outlive.
    HeldModifiers blockSelectChord { .hasAlternate = true };
    HeldModifiers selectChord { .hasShift = true };
    /// The PTY erase character; absent when VERASE is disabled.
    std::optional<std::uint8_t> readlineEraseCharacter = 0x7f;
};

/// One scroll gesture, as a frontend observes it.
///
/// Each device reports its deltas in its own units and Core folds them.
/// `isPrecise` selects the fold: a notched
/// wheel is whole lines, a continuous surface accumulates fractions.
struct ScrollFact {
    /// The gesture's deltas, in the platform's own units. Positive `deltaY`
    /// brings older history toward the reader.
    double deltaX = 0.0;
    double deltaY = 0.0;
    /// Whether the deltas are continuous rather than notched.
    bool isPrecise = false;
    /// A finger dragging the screen, rather than a wheel or trackpad.
    /// Without mouse tracking, a touch navigates an alternate-screen
    /// application with cursor keys because that screen has no scrollback.
    bool isDirectTouch = false;
    /// The cell under the pointer, zero-based -- where a reported wheel
    /// press is reported from.
    std::ptrdiff_t row = 0;
    std::ptrdiff_t column = 0;
    /// The device pixel, 1-based from the grid origin, for SGR-pixels.
    std::ptrdiff_t pixelX = -1;
    std::ptrdiff_t pixelY = -1;
    /// The held modifiers, as facts rather than a platform mask.
    bool hasShift = false;
    bool hasAlternate = false;
    bool hasControl = false;
};

/// What the wheel ladder settled, and what remains for the frontend.
struct ScrollRoute {
    /// The viewport owns this event, including sub-line and zero-delta gestures.
    bool isViewportScroll = false;
    /// Rows to move the local viewport by, positive toward older history.
    /// Zero on every other route -- the view holds still while a program
    /// or the alternate-scroll fold consumes the gesture.
    std::ptrdiff_t linesToOffsetView = 0;
    /// The gesture produced bytes or a view movement.
    bool wasConsumed = false;
    /// The wheel reports, or the alternate-scroll burst, to write to the
    /// host. Repetitions past the capacity are dropped whole rather than
    /// truncated mid-sequence.
    std::array<char, 512> bytes {};
    int byteLength = 0;
};

/// The per-view state one ladder carries between events.
struct PointerLadder {
    /// xterm reports motion only when the coordinate changes.
    MotionDedup dedup;
    /// The sub-line residue of a continuous scroll, per axis, so a slow
    /// swipe still advances a whole line over several samples.
    FractionalAccumulator scrollAccumulator;
    FractionalAccumulator scrollAccumulatorHorizontal;
    /// The mode-1001 gesture: armed by a reported left press, ended by the
    /// release with the hilite end report.
    bool isHiliteGestureActive = false;
    int hiliteStartColumn = 1;
    int hiliteStartRow = 1;
    /// A local selection gesture is open: the next drag extends it and the
    /// next release ends it.
    bool isSelectionGestureOpen = false;
    bool isReadlineClickPending = false;
    int readlinePressRow = 0;
    int readlinePressColumn = 0;
    std::uint32_t readlinePressTicks = 0;
    bool hasReadlineRightRelease = false;
    bool isReadlineDeletePending = false;
    int readlineRightRow = 0;
    int readlineRightColumn = 0;
    std::uint32_t readlineRightReleaseTicks = 0;
    std::uint32_t readlineRightPressTicks = 0;
    /// How many times this session has taught the reach-through modifiers, and
    /// whether the user has since performed one. A drag swallowed by an
    /// application is the only moment the lesson is relevant, and someone who
    /// has reached through once does not need telling again.
    int selectionHintShowCount = 0;
    bool hasUsedSelectionOverride = false;
    /// Whether the drag in flight has already taught. A drag is a stream of
    /// motions, and teaching on each would spend the whole budget on one
    /// gesture; this makes one drag one lesson.
    bool hasTaughtThisDrag = false;
    /// The cell a reported left press landed on, so a swallowed DRAG can be
    /// told from a click that wobbled. Negative when no press is outstanding.
    std::ptrdiff_t reportedPressRow = -1;
    std::ptrdiff_t reportedPressColumn = -1;
    /// The transient lesson in flight, and when it was raised.
    HintClock hintClock;
    /// Whether the frontend holds the pointer grab it performs for 7403. An
    /// OS fact only the frontend sees: Core holds the mode bit, but an arm
    /// that never grabs -- or declined this time because its window was not
    /// key -- has no trap to name the way out of.
    bool isPointerLockHeld = false;
    /// How many grabs this ladder has seen, for the lesson budget. An
    /// application that locks on every click must not nag.
    int pointerLockEngagements = 0;
    /// The break-out chord: armed at a tick while the grab is held, or not.
    PointerLockChord pointerLockChord;
};

/// What the ladder settled, and what remains for the frontend.
struct PointerRoute {
    /// The event was consumed — reported, gestured, or swallowed. False leaves
    /// it to the frontend's native fallback (a context menu, a link click).
    bool wasConsumed = false;
    /// The selection or a highlight changed; repaint.
    bool needsDisplay = false;
    /// The selection changed; tell the observer.
    bool didChangeSelection = false;
    /// A press opened a local selection gesture.
    bool didOpenSelectionGesture = false;
    /// A release ended a local selection gesture; copy-on-select applies.
    bool didFinishSelectionGesture = false;
    /// The hilite await or region changed; republish the snapshot.
    bool needsRepublish = false;
    /// Tek GIN owns the pointer: the frontend converts the point to Tek
    /// coordinates and calls `tekGINKey` for a press, `tekUpdateGINPosition`
    /// for a move.
    bool wantsTekGIN = false;
    /// The mouse report, or the hilite end report, to write to the host.
    std::array<char, 160> report {};
    int reportLength = 0;
    /// Repeated cursor keys for a readline gesture, bounded by the visible grid.
    FallibleVector<char> readlineInput;
    /// The facts `report` was encoded from, when it is an xterm mouse report
    /// at a cell: what a remote session sends its host in place of the
    /// bytes, since the host encodes a pointer event under the modes live
    /// when it writes it. Absent for the hilite end report and for a
    /// pointer-lock delta, which travel as the bytes they are.
    struct Reported {
        Mouse::EventType type = Mouse::EventType::Motion;
        int buttonCode = 0;
        int modifiers = 0;
        int row = 0;
        int column = 0;
        /// The device pixel, negative when the frontend resolved none.
        int pixelX = -1;
        int pixelY = -1;
    };
    std::optional<Reported> reported;
    /// The DEC locator report to write to the host, beside `report`.
    std::array<char, 32> locatorReport {};
    int locatorReportLength = 0;
    /// A gesture worth teaching, raised at the moment it would have helped.
    /// The frontend surfaces ``hintText`` and holds it for the lifetime
    /// ``isHintPersistent`` states.
    Hint hint = Hint::None;
};

/// Runs one fact through the ladder.
///
/// Under the model lock: the modes that rank the rungs and the selection's
/// rows are the ingest thread's to write. The bytes in the route are the
/// caller's to send after the lock is released.
///
/// - Parameters:
///   - ladder: The view's ladder state.
///   - terminal: The terminal the view shows.
///   - selection: The view's selection session, or null for a frontend with
///     no selection model.
///   - fact: The event's facts.
///   - policy: The preferences the ladder reads.
///   - nowTicks: The monotonic 60 Hz tick, for the hilite await's deadline.
/// - Returns: What the frontend has left to do.
[[nodiscard]] PointerRoute routePointer(PointerLadder& ladder, Terminal& terminal, Selection::Session* selection, const PointerFact& fact, const PointerPolicy& policy, std::uint32_t nowTicks);

/// Tells the DEC locator where a pointer that has not moved is, and tells
/// nothing else.
///
/// The locator's arm edge. xterm always knows where its locator is, so a
/// frontend that sees DECELR arm under a stationary pointer publishes that
/// pointer's position, and a DECRQLP arriving before the next move answers it
/// rather than an undefined position. Only the locator's rung runs: a pointer that did not
/// move is not a move to the xterm modes, the hilite gesture or the selection.
/// A report can still come back -- a DECEFR in the same drain can have armed a
/// rectangle the pointer already stands outside -- and it is the caller's to
/// send.
///
/// Under the model lock, like ``routePointer``.
///
/// - Parameters:
///   - terminal: The terminal the view shows.
///   - fact: Where the pointer is; its kind and button are not read.
/// - Returns: The locator report, if one came back; nothing else is set.
[[nodiscard]] PointerRoute seedLocator(Terminal& terminal, const PointerFact& fact);

/// Records the frontend's pointer grab, so the lesson naming the way out
/// stands for exactly as long as the trap does.
///
/// The grab is the frontend's to report and everything after is the
/// ladder's: whether this capture is still within the teaching budget, and
/// that the lesson outranks a transient one while it stands.
///
/// - Parameters:
///   - ladder: The ladder's state.
///   - grab: Whether the grab is held.
void notePointerLockGrab(PointerLadder& ladder, PointerLockGrab grab);

/// The hint a frontend should be showing at a tick.
///
/// The one question a presentation asks. The pointer-lock lesson stands while
/// the grab is held and the budget allows; otherwise the transient lesson
/// stands until its interval elapses. A frontend asks again after any pointer
/// fact, after any grab report, and once at ``hintExpiryTicks``.
///
/// - Parameters:
///   - ladder: The ladder's state.
///   - nowTicks: The current tick, in ``PAL::monotonicTicks`` units.
/// - Returns: The hint, or ``Hint::None``.
[[nodiscard]] Hint hintShowing(const PointerLadder& ladder, std::uint32_t nowTicks);

/// The tick at which the standing hint withdraws itself, or 0 when none will.
///
/// - Parameter ladder: The ladder's state.
/// - Returns: The expiry tick of a transient lesson; 0 while the lock lesson
///   stands or nothing does.
[[nodiscard]] std::uint32_t hintExpiryTicks(const PointerLadder& ladder);

/// Records the modifier state the frontend saw, arming the break-out chord on
/// the edge where it becomes the sole modifiers held under a grab.
///
/// Modifiers do not auto-repeat, so the hold is measured from this tick to the
/// tick ``tickPointerLockChord`` is asked; a frontend asks once, after
/// ``pointerLockChordTicksUntilDeadline``.
///
/// - Parameters:
///   - ladder: The ladder's state.
///   - keys: The modifiers and whether any other key is down.
///   - nowTicks: The current tick.
void notePointerLockChord(PointerLadder& ladder, const PointerLockKeyState& keys, std::uint32_t nowTicks);

/// Decides whether the armed chord has been held for the threshold.
///
/// Re-checked against the live keys, so a release in the last instant
/// aborts. A break-out disarms the chord; the caller clears the mode and
/// releases the grab.
///
/// - Parameters:
///   - ladder: The ladder's state.
///   - keysNow: The modifiers as they are at this tick.
///   - nowTicks: The current tick.
/// - Returns: Whether to break out.
[[nodiscard]] PointerLockChordVerdict tickPointerLockChord(PointerLadder& ladder, const PointerLockKeyState& keysNow, std::uint32_t nowTicks);

/// How long until the armed chord's hold elapses, or 0 when it is not armed.
///
/// The one wake-up a frontend owes the chord.
///
/// - Parameters:
///   - ladder: The ladder's state.
///   - nowTicks: The current tick.
/// - Returns: Ticks until the deadline, or 0.
[[nodiscard]] std::uint32_t pointerLockChordTicksUntilDeadline(const PointerLadder& ladder, std::uint32_t nowTicks);

/// Runs one scroll gesture through the wheel ladder.
///
/// Mouse tracking outranks alternate scroll outranks the local view, and
/// relative pointer lock outranks them all: a locked wheel rides the locked
/// frame with a zero delta. A notched wheel is one report per event; a
/// continuous surface is one report per line the fold produces, so a flick
/// on a trackpad scrolls an application as far as it scrolls history.
///
/// - Parameters:
///   - ladder: The view's ladder state; its accumulators carry the residue.
///   - terminal: The terminal the view shows.
///   - fact: The gesture's facts.
///   - tuning: The [mouse] wheel preferences, resolved for this view.
/// - Returns: What the frontend has left to do.
[[nodiscard]] ScrollRoute routeScroll(PointerLadder& ladder, Terminal& terminal, const ScrollFact& fact, const Screen::ScrollTuning::Policy& tuning);

/// The ladder's preferences, read from one store.
///
/// The tmux control-mode pane is not a preference; a frontend that renders
/// one sets `isControlModePane` on the result.
///
/// - Parameter values: The effective preferences.
/// - Returns: The policy.
[[nodiscard]] PointerPolicy pointerPolicyFromPreferences(const Preferences::Values& values);

/// Whether a window carries the title bar a reader would otherwise grab.
enum class WindowChrome : uint8_t {
    /// The window has a title bar, so its body is only the terminal.
    Titled UBIQUITTY_SWIFT_NAME(titled),
    /// The window has none, so its body has to serve as one.
    Chromeless UBIQUITTY_SWIFT_NAME(chromeless),
};

/// Whether these modifiers arm a window move.
///
/// Two ways in. The `[window] drag_modifier` spec is matched exactly — the
/// press carries the modifiers it names and no others, so a selection drag
/// and the reach-through modifiers keep their own meanings; the spec is the
/// preference's own text ("cmd+shift"), and an empty one never matches. And
/// Command in a chromeless window, where the title bar that would otherwise
/// be grabbed is not there and the body has to serve as one.
///
/// Asked without a press as well as with one, because the pointer advertises
/// the gesture before it is made.
///
/// - Parameters:
///   - modifiers: What is held.
///   - dragModifierSpec: The [window] drag_modifier preference.
///   - chrome: Whether the window has a title bar.
/// - Returns: Whether a drag now would move the window.
[[nodiscard]] bool windowDragArmed(HeldModifiers modifiers, std::string_view dragModifierSpec, WindowChrome chrome);

/// The chord that reaches a link through an application holding the mouse.
/// Command, and not a preference: it is the platform's own gesture for
/// following one.
inline constexpr HeldModifiers linkChord { .hasCommand = true };

/// What a frontend sees at one pointer moment.
///
/// The facts, and none of the rules: which gesture each chord arms is the next
/// function's answer.
struct PointerMoment {
    /// The modifiers down at this moment.
    HeldModifiers held;
    /// Whether the application has xterm mouse reporting on.
    bool isMouseReporting = false;
    /// A rectangular drag already running.
    ///
    /// It answers for the chord that opened it and not for the keyboard, the
    /// way Core keeps an open gesture local: a reader who lets the chord go
    /// halfway through is still dragging a block, and the crosshair that
    /// promised it must not turn back into an I-beam under their hand.
    bool isBlockSelectionInFlight = false;
};

/// The report-vs-local inputs for one pointer moment.
///
/// The one place the three gesture chords are matched, and the reason it is
/// exported: a frontend filling this state field by field is deciding which
/// modifier means what, and four arms deciding that separately is four
/// answers — the arm that forgets the exactness rule arms two gestures with one
/// chord, and the arm that hard-codes Option keeps a preference it cannot see.
/// So an arm reports what is held and what its surface is, and Core says which
/// gestures that arms.
///
/// - Parameters:
///   - moment: What the frontend sees.
///   - policy: The reader's preferences, and whether this surface is a `-CC`
///     pane the program renders itself.
/// - Returns: The state `allowsLocalLinkInteraction`, `allowsLocalDragSelect`,
///   `localDragIsRectangular` and `linkProbe` read.
[[nodiscard]] PointerReportingState pointerReportingState(const PointerMoment& moment, const PointerPolicy& policy) noexcept;

/// Whether a press opens a gesture the caret model drives.
///
/// A plain single click is a text-editor drag: the caret snaps to the boundary
/// nearest the pointer and an open threshold keeps a sloppy click from opening
/// a selection at all. The three exclusions each hand the press to a gesture
/// that is not that one — a second click is a word or line, Shift extends the
/// standing selection from its anchor, and the block modifier makes a column
/// span whose ends are cells rather than boundaries.
///
/// Asked of the fact rather than of the toolkit's event so both shells answer
/// alike, and asked once the fact is built rather than before: the caret is
/// resolved and written onto it only for the presses this admits.
///
/// - Parameter fact: The press or drag, its modifiers and click count filled.
/// - Returns: Whether to resolve a caret for it.
[[nodiscard]] constexpr bool isCaretDrivenGesture(const PointerFact& fact)
{
    return fact.clickCount == 1 && !fact.hasShift && !fact.hasAlternate && std::isfinite(fact.xInCells);
}

/// Whether a press begins a window move.
///
/// - Parameters:
///   - fact: The press. A motion or a release begins nothing.
///   - dragModifierSpec: The [window] drag_modifier preference.
///   - chrome: Whether the window has a title bar.
/// - Returns: Whether the frontend moves its window with this press.
[[nodiscard]] bool isWindowDragPress(const PointerFact& fact, std::string_view dragModifierSpec, WindowChrome chrome);

} // namespace Ubiquitty::Input
