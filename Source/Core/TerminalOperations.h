// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Everything a caller does to a terminal: feed it bytes, resize it, reset it,
// drain what it wants to send back. Twenty declarations, every one taking a
// `Terminal*` or `Terminal&` first, and no type of its own.
//
// At Core root rather than in Core/VT/ because that is a fact about the subject
// rather than about the implementation. The parser lives in Core/VT/ and
// implements most of this; `Ubiquitty::VT::Attribute` and
// `Ubiquitty::VT::maxParameters` are genuinely the parser's and are declared
// there. These are the terminal's, and were never VT's to name.

#include "Base/Geometry.h"
#include "Base/Types.h"
#include "Core/Color.h"
#include "Core/Modes/Emulation.h"
#include "Core/Modes/HostPolicy.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Rendering/PaletteColors.h"
#include "Core/Unicode/Encodings.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Forward.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::PAL {
class ProcessFacts;
}

namespace Ubiquitty {

// Null policy: a `Terminal&` cannot be absent, so the entry points spelled that
// way have no null case to handle and no caller can construct one. Where a
// `Terminal*` remains, null is a meaningful argument the body tests for and
// answers with the neutral value — the spelling is the whole of the contract,
// and check-terminal-pointer-intent fails a pointer that promises nothing.

/// Where a terminal writes bytes bound for the host: the caller's storage, never
/// grown.
///
/// Named rather than spelled `std::span<char>` at each signature because the
/// Swift importer has no spelling for the extent template parameter and drops
/// every function that names one, with no diagnostic — so the entry point is
/// absent on the Swift side and the call fails to resolve against a name that
/// exists in C++.
using OutboundBytes = std::span<char>;

/// The pixel extent a program is told the text area has: what CSI 14 t answers
/// and what an SGR-pixel mouse report is clamped to.
///
/// Under DECSET 7402 a fitted image fills the whole drawable from its corner,
/// whatever the letterbox, so the space a program reasons about is the drawable
/// while the mode holds; a report of the grid's extent there leaves the mouse
/// space about a percent smaller than the picture, and the edge overshoots.
/// Decided here, from what the frontend published, so a mode that flips
/// between two publishes changes the answer at once and neither frontend has
/// to choose. Zero on an axis nothing was published for.
///
/// - Parameter terminal: The terminal whose published extents decide.
/// - Returns: The extent in device pixels, or zero where none was published.
[[nodiscard]] IntSize reportedTextAreaPixels(const Terminal& terminal);

/// Resets application-visible state to power-on defaults.
///
/// The `Terminal` object's address does not change, so every pointer a frontend
/// holds stays valid. Frontend configuration, clients, security policy,
/// transport wiring and configured resources survive — this resets what the
/// host can see, not what the embedder set up.
///
/// - Parameter terminal: The terminal to reset.
void hardReset(Terminal& terminal);

/// Sets the configured scrollback depth.
///
/// Apply before the session produces output: this sizes the line store at its
/// lazy creation, and a store that already holds rows keeps its capacity until
/// the next session. Setting it mid-session is not an error and not a resize.
///
/// - Parameters:
///   - t: The terminal to configure.
///   - lines: Rows to retain, clamped up to a floor.
void setScrollbackLimit(Ubiquitty::Terminal* t, int lines);
/// Sets the string transmitted in reply to ENQ (0x05).
///
/// No host escape sequence reaches this; configuration is the only path, which
/// is what stops a remote host choosing what the terminal will say about itself.
///
/// - Parameters:
///   - t: The terminal to configure.
///   - utf8: The reply. Copied into a bounded buffer; empty disables the reply.
void setAnswerback(Ubiquitty::Terminal* t, std::string_view utf8);
/// Resizes the logical terminal.
///
/// Cells outside the new bounds are dropped;
// newly-revealed cells are blanked with the owning screen's current pen.
// Cursor and scroll region clamp to the new dimensions. Marks all rows dirty.
//
/// Survival rules across a resize:
/// - Both screens resized; cursor wrapPending cleared on both, so a later
///   DECSET 47/1047/1049 flip lands on a screen valid for the new size.
/// - DECSC saved state survives; its saved scrollTop/Bottom are not re-clamped
///   here — a DECRC after a shrink re-engages screenResize gridFloor on its
///   next mutation.
/// - Per-row line attributes (Ubiquitty::Screen::lineAttribute) and OSC 133 marks
///   (RowMeta::oscMark) live in fixed maxRows arrays, preserved per surviving
///   row index; rows past the new bottom persist but become unreachable until
///   a later grow re-exposes them.
/// - DECSLRM margins reset to full width only when already at the default span
///   [0, oldColumns - 1]; explicit margins are edge-clamped, then re-defaulted
///   only if the clamp collapses them (scrollLeft >= scrollRight).
/// - Tab stops in surviving columns keep their HTS state; new columns get the
///   (i % 8 == 0) default (a blanket resetTabStops would erase host stops).
//
/// allowReflow=false does a geometry-only resize (no rewrap) — for a thin
// client whose grid is authored by a remote snapshot.
//
/// anchorBottom=true pins the active primary screen's content to the BOTTOM
// across a height-only change; pass it for a top-edge drag so the prompt doesn't
// ride the moving top edge and snap back (the resize "jump").
//  - alternate screen: never anchored; a shrink drops BOTTOM rows and keeps the
//    top bar a full-screen program drew, which repaints the rest on SIGWINCH.
//  - primary + allowReflow=false: shrink pushes TOP rows into scrollback, grow
///    pulls them back — strict, even across a trailing blank band.
//  - primary + allowReflow=true already bottom-anchors via reflow; width
///    changes ignore the flag.
///
/// - Parameters:
///   - t: The terminal to resize.
///   - newRows: New height in cells.
///   - newColumns: New width in cells.
///   - reflow: `Modes::ReflowMode::None` for a geometry-only resize, for a thin client
///     whose grid is authored by a remote snapshot.
///   - anchor: `Modes::AnchorMode::Bottom` pins the primary screen's content to
///     the bottom across a height-only change; pass it for a top-edge drag.
///   - urgency: `Modes::ResizeUrgency::Interactive` defers the history rewrap to
///     `settleResize`.
void resize(Ubiquitty::Terminal& t, int newRows, int newColumns, Modes::ReflowMode reflow = Modes::ReflowMode::Reflow, Modes::AnchorMode anchor = Modes::AnchorMode::Top, Modes::ResizeUrgency urgency = Modes::ResizeUrgency::Settled);

/// The sizes a frontend draws a terminal at, in device pixels.
struct PixelGeometry {
    /// One cell.
    IntSize cell;
    /// The grid, columns by rows of cells as the frontend lays them out.
    IntSize textArea;
    /// The whole drawable, padding included.
    IntSize drawable;
};

/// Resolves the program's pixel extent from this client's complete metrics,
/// without copying them into a remote model whose host owns its geometry.
[[nodiscard]] IntSize reportedTextAreaPixels(const Terminal& terminal, const PixelGeometry& geometry);

/// Publishes the sizes a frontend draws a terminal at.
///
/// Core turns pixel measures into cells with these — an inline image sized in
/// pixels, the CSI 14, 16 and 18 t reports — and has no other source for them.
/// A frontend that never publishes leaves them zero, and Core then ignores a
/// pixel size it is given rather than guess a cell. Every frontend publishes
/// through here whenever its cell, its grid or its padding changes, so no arm
/// states a subset of the fields.
///
/// - Parameters:
///   - t: The terminal the frontend draws.
///   - geometry: The sizes; each saturates at 65535, the width of its field.
void publishPixelGeometry(Ubiquitty::Terminal& t, const PixelGeometry& geometry);

/// Finishes a history rewrap deferred by an `Interactive` resize.
///
/// Brings the stored-width scrollback up to the current column count. Call from
/// the frontend's end-of-live-resize hook; a `Settled` resize settles implicitly
/// before it runs, so only the interactive path needs this.
///
/// - Parameter t: The terminal to settle. A no-op when nothing is pending.
void settleResize(Ubiquitty::Terminal* t);

/// Decides whether a resize should rewrap history, for every frontend.
///
/// False (geometry-only, no rewrap) when a non-shell foreground app owns the PRIMARY screen: such an
/// app (less -X, git's pager) repaints its own rows on SIGWINCH, so rewrapping
/// underneath duplicates rows and jitters the view. Shell and alt-screen apps
/// keep normal reflow.
///
/// - Parameters:
///   - t: The terminal being resized.
///   - foregroundIsShell: Whether the foreground process is a shell.
/// - Returns: What to pass as `resize`'s `reflow` argument.
[[nodiscard]] bool shouldReflowOnResize(const Ubiquitty::Terminal* t, bool foregroundIsShell); // bool-ok: PAL answers this as a bool and 27 files already spell the concept this way; renaming one declaration would make it the odd one out

/// Clears like `clearBuffer`, keeping the cursor row's content.
///
/// The Cmd-K path, so a half-typed shell command survives clearing the screen.
///
/// - Parameter t: The terminal to clear.
void clearBufferPreserveInput(Ubiquitty::Terminal* t);

/// Clears the visible screen and the retained scrollback of the current buffer.
///
/// - Parameter t: The terminal to clear.
void clearBuffer(Ubiquitty::Terminal* t);

/// Applies a DECSTR soft reset (CSI ! p) locally.
///
/// The UI's "Reset Terminal" command: the host-initiated path's modes, pen,
/// character sets and tab stops return to power-on defaults while the screen
/// contents stay put, and bracketed paste, focus events and mouse tracking,
/// which DECSTR leaves, are turned off too.
///
/// - Parameter t: The terminal to reset.
void softReset(Ubiquitty::Terminal* t);

// Walk the placement array. Slots not in use return NULL. The renderer
// should iterate 0..maxTerminalImagePlacementCount-1, sort by zIndex (or use the
// order of insertion as a stable secondary key), and draw bottom-up.

// True when any stored image has more than one frame and an animation state
// other than Stopped, i.e. the per-frame ticker must keep advancing it. The
// frontend animation loop polls this (off the model lock, via the snapshot)
// to decide whether to keep its repaint timer armed. Portable: no platform or
// renderer state, only the Ubiquitty::Terminal image store.

/// Integrates completed async kitty-graphics and sixel decode jobs.
///
/// Pulls TaskQueue
/// worker results in FIFO order (install pixels, send kitty OK/EINVAL, advance
/// the cursor for sixel). Cheap when none are pending. The host inline-fallback
/// path also calls this from `processBytes` for synchronous tests.
///
/// - Parameter t: The terminal whose pending decodes are integrated. The decode
///   queues are process-global, so every terminal's work drains together.
void drainAsyncDecodes(Ubiquitty::Terminal& t);

// Kitty graphics transports t=f (regular file) and t=t (temp file) read
// through Terminal::fileReadClient (FileReadClient::readFile). With no client
// set, t=f / t=t respond with ENOTSUP. t=s (shared memory) is always
// rejected — the Core does not map shared memory.

// OSC 9;4 — ConEmu progress reports.
/// A counter that changes whenever the app pushes a new progress state.
///
/// Exists so a poller can skip the value read when nothing has moved. The
/// shipping frontends read the values directly each frame instead, so the
/// counter's consumer is the tests, which use it as the change-detection seam
/// — a caller that adopts polling gets the skip without a Core change.
///
/// - Parameter t: The terminal to query.
/// - Returns: The generation. Compare against the last one seen; do not
///   interpret the number.
[[nodiscard]] uint32_t progressGeneration(const Ubiquitty::Terminal* t);

/// Reads the most recent OSC 9;4 progress report.
///
/// - Parameters:
///   - t: The terminal to query.
///   - state: Receives a `ProgressState` (Core/OSC/ProgressIndicator.h) widened to
///     `uint8_t`.
///   - value: Receives 0..100, meaningful only when the state is
///     `ProgressNormal`.
void getProgress(const Ubiquitty::Terminal* t, uint8_t* state, uint8_t* value);

// OSC 21337 — tab-status indicator.
/// A counter that changes whenever the tab-status indicator changes.
///
/// - Parameter t: The terminal to query.
/// - Returns: The generation. Compare against the last one seen.
[[nodiscard]] uint32_t tabStatusGeneration(const Ubiquitty::Terminal* t);

/// Reads the OSC 21337 tab-status indicator.
///
/// Clearing the indicator leaves the terminal's colour fields holding the one
/// that was in force. Answering "is one set" apart from "what colour is it"
/// therefore lets the second answer outlive the first, and a stale colour is
/// indistinguishable from a current one. One value carries both.
///
/// - Parameter t: The terminal to query.
/// - Returns: The indicator's colour, or nothing when none is set. Alpha is not
///   part of the indicator and stays at the type's opaque default.
[[nodiscard]] std::optional<ColorRGBA> tabStatusColor(const Ubiquitty::Terminal* t);

/// Reads the OSC 21337 `status` label that describes the indicator's colour.
///
/// Empty whenever no indicator is set, for the reason ``tabStatusColor`` folds
/// its flag in: a label has nothing to describe without a colour, and a reader
/// that consulted the stored bytes alone would name a state that has cleared.
///
/// - Parameter t: The terminal to query.
/// - Returns: The label as UTF-8, empty for none; a view into the terminal, valid
///   until the next escape that changes it.
[[nodiscard]] std::string_view tabStatusText(const Ubiquitty::Terminal* t);

/// What a terminal's program asked attention for that nobody has seen yet.
struct UnseenAttention {
    /// Whether the bell rang since the last ``markAttentionSeen``.
    bool hasBell = false;
    /// Whether a notification (OSC 9, 99 or 777) was posted since then.
    bool hasNotification = false;
    /// The newest such notification's title and body, UTF-8, each at most
    /// `OSC::attentionTextCapacity` bytes; views into the terminal, valid
    /// until its next notification or ``markAttentionSeen``.
    std::string_view notificationTitle;
    std::string_view notificationBody;
};

/// Reads the attention a terminal's program raised that nobody has seen yet.
///
/// A bell and a notification are edges: the frames that carry them reach
/// whoever is attached at that moment and nobody afterwards. The terminal
/// keeps the fact, so a listing or a viewer that arrives later can still mark
/// the pane. It stays until ``markAttentionSeen``, which is called where a
/// person demonstrably looks at the pane: on a host when a person's driver
/// gains the focus on it, and in a local terminal when its frontend reports
/// the pane focused. While that focus is held, what arrives is seen at once.
///
/// - Parameter t: The terminal to query.
/// - Returns: The latched attention; all clear for a null terminal.
[[nodiscard]] UnseenAttention unseenAttention(const Ubiquitty::Terminal* t);

/// Records that a person has seen the terminal, clearing its unseen attention.
///
/// - Parameter t: The terminal a person looked at.
/// - Returns: Whether anything was latched.
bool markAttentionSeen(Ubiquitty::Terminal& t);

/// Reads the iTerm2 tab colour (OSC 6;1;bg).
///
/// One value for "is one set" and "what colour", for the reason
/// ``tabStatusColor`` gives. A change bumps ``tabStatusGeneration``.
///
/// - Parameter t: The terminal to query.
/// - Returns: The colour the program asked its tab to take, or nothing when
///   none is set or it was reset.
[[nodiscard]] std::optional<ColorRGBA> tabColor(const Ubiquitty::Terminal* t);

/// Reads the iTerm2 badge (OSC 1337 SetBadgeFormat).
///
/// The text is kept as the program sent it: iTerm2's `\(…)` interpolations
/// are not evaluated. A change bumps ``tabStatusGeneration``.
///
/// - Parameter t: The terminal to query.
/// - Returns: The badge as UTF-8, empty for none; a view into the terminal,
///   valid until the next escape that changes it.
[[nodiscard]] std::string_view badgeText(const Ubiquitty::Terminal* t);

// XTWINOPS resize (op 8), DECCOLM relayout, and the window mutators
// (operations 1..7, 9, 10) route through Terminal::windowClient
// (WindowClient::resize / setColumnMode / performWindowOperation),
// gated upstream by Terminal::allowWindowOperations. Without a client, op 8
// resizes the model directly for explicit non-zero dims; the rest no-op.

/// Appends every byte fed into `processBytes` to a file, verbatim.
///
/// The log is closed automatically by `dispose()`.
///
/// - Parameters:
///   - t: The terminal to log.
///   - path: The file to append to, or null to close any open log.
/// - Returns: `true` when the open succeeded or the close completed; `false`
///   when the file could not be opened.
bool setSessionLogPath(Ubiquitty::Terminal* t, const char* path);

/// Routes DEC MC (Media Copy) output to a file.
///
/// MC print-screen (CSI 0 i), print-line (CSI ? 1 i) and auto-print (CSI ? 5 i)
/// write their UTF-8 there.
///
/// This is the gated install point, and that is the security property: without
/// an installed sink the MC operations are inert, so a host cannot make the
/// terminal write an arbitrary file on its own. Closed automatically by
/// `dispose()`.
///
/// - Parameters:
///   - t: The terminal whose print output is redirected.
///   - path: The file to append to, or null to close any open sink.
/// - Returns: `true` when the open succeeded or the close completed; `false`
///   when the file could not be opened.
bool setPrintSinkPath(Ubiquitty::Terminal* t, const char* path);

/// Enables password-prompt detection ([security] password_detect).
///
/// At a settled PTY batch, canonical input with echo disabled activates
/// protection, and canonical input with echo enabled clears it. Raw or unknown
/// line discipline falls back to the cursor row's password-prompt text.
/// An owner without PTY facts also uses the text heuristic; a replica always
/// keeps its host's verdict. An app can assert protection directly via private
/// mode 7401, independently of detection.
/// Default off; the byte-hot path is untouched when off.
///
/// - Parameters:
///   - t: The terminal to configure.
///   - isEnabled: Whether to run the heuristic.
void setPasswordDetect(Ubiquitty::Terminal* t, bool isEnabled); // bool-ok: mirrors a boolean config key, [security] password_detect

/// Whether the terminal currently believes a password prompt is on screen.
///
/// - Parameter t: The terminal to query.
/// - Returns: `true` while the prompt is active, whether detected by the
///   heuristic or asserted by the app through private mode 7401.
[[nodiscard]] bool passwordPromptActive(const Ubiquitty::Terminal* t);

/// Updates password protection once the program's output has paused.
///
/// A prompt waits for a person, so the verdict is read when the writer pauses:
/// the runtime settles at a drain pass that caught up, never per parsed chunk.
/// The PTY facts cost a tty ioctl, and a chunk of a flood is one kernel quantum.
/// The caller holds the terminal's model lock. Replicas keep their host's
/// verdict; owners use canonical echo state or the text heuristic when the
/// discipline is raw or unknown. Mode 7401 remains authoritative.
///
/// - Parameters:
///   - terminal: The terminal whose output settled.
///   - process: Its PTY facts, or null when the owner has no local PTY.
void settlePasswordPrompt(Terminal& terminal, PAL::ProcessFacts* process);

// Diagnostic events route through Terminal::diagnosticsClient
// (DiagnosticsClient::log), DEBUG-only. Tags emitted:
//   core-scroll-up  — detail = scroll delta
//   core-scroll-down — detail = scroll delta
//   csi-miss        — unrecognised CSI dispatch; detail encodes
///                     (privatePrefix << 16) | (intermediate << 8) | final
///                     for prefixed misses, or just `final` for plain misses
//   esc-miss        — unrecognised ESC final byte; detail = byte
//   tek-list-full   — Tek storage tube overflowed and dropped primitives;
///                     detail = tekMaxPrimitives; once per frame
//   sixel-clipped   — sixel exceeded the decode window and was cropped;
///                     detail = (clamped width << 16) | clamped height

/// Feeds one byte to the parser.
///
/// - Parameters:
///   - t: The terminal to feed.
///   - c: The byte.
void processByte(Ubiquitty::Terminal* t, uint8_t c);

/// Feeds a run of bytes to the parser.
///
/// The per-byte path is hot; prefer this over looping on `processByte`.
///
/// - Parameters:
///   - t: The terminal to feed.
///   - buffer: The bytes. Not required to end on a sequence boundary — the
///     parser carries its state across calls. A NUL is a byte like any other
///     here, so a run holding one has to be built from a pointer and a length.
void processBytes(Ubiquitty::Terminal* t, std::string_view buffer);

/// Feeds bytes with the owning PTY's facts for settled password detection.
///
/// The facts are borrowed for this call, sampled only when detection is enabled
/// and no mode-7401 assertion stands. An owner without facts uses the text
/// heuristic; a replica never infers a verdict.
///
/// - Parameters:
///   - t: The terminal to feed, or null to do nothing.
///   - buffer: The bytes to parse.
///   - process: The PTY owner's facts, or null for unknown discipline.
void processBytes(Ubiquitty::Terminal* t, std::string_view buffer, PAL::ProcessFacts* process);
/// Feeds bytes up to and including the next DECSET 2026 clear, or up to the
/// reply-pressure line.
///
/// Lets a frontend snapshot the completed frame at the ESU instant rather than
/// mid-update. The parse also yields once `Terminal::outputReplyPressureBytes`
/// of replies are queued, so the caller can drain them to the stream before
/// the fixed buffer overflows and drops the surplus — the two stop reasons are
/// told apart by `stoppedAtSyncClear`.
///
/// - Parameters:
///   - t: The terminal to feed.
///   - buffer: The bytes.
///   - stoppedAtSyncClear: Set true when the early return is the 2026 clear
///     (snapshot the frame), false when it is reply pressure (drain the reply
///     buffer, then feed the rest).
/// - Returns: Bytes consumed. The whole run when neither stop is present.
int processBytesUpToSyncClear(Ubiquitty::Terminal* t, std::string_view buffer, bool* stoppedAtSyncClear);

/// The reason-blind form: an early return that is not a sync clear still needs
/// its replies drained before the next call, or they overflow the same way.
int processBytesUpToSyncClear(Ubiquitty::Terminal* t, std::string_view buffer);

/// Commits deferred scroll-lane accounting.
///
/// `processBytesUpToSyncClear` leaves a lane run OPEN across an input-exhausted
/// return, which is the flood pipeline's bite cadence. Every reader of paint or
/// placement state outside the parser — a render capture, a runtime snapshot —
/// must flush through here first or it reads a half-open run.
///
/// - Parameter t: The terminal whose lane is committed.
void termFlushScrollLane(Ubiquitty::Terminal& t);

/// Drops the memoized tmux %output pane route.
///
/// Must be called by any frontend path that releases the deliver-pane lock or
/// pin outside the end-of-pass flush, which calls it itself. A memo outliving
/// its pin delivers into an unpinned, unlocked, possibly freed pane terminal.
///
/// - Parameter t: The terminal whose memoized route is dropped.
void invalidateTmuxPaneRoute(Ubiquitty::Terminal* t);

/// Returns the terminal's present instant on the 60 Hz tick clock.
///
/// The terminal's own `TerminalClock` when one is installed, the process-wide
/// PAL clock otherwise. Everything that reads time for a terminal — Core while
/// it parses, a frontend for the blink phase and activity hold — reads it here,
/// so a terminal given a clock never mixes it with another.
///
/// - Parameter t: The terminal.
/// - Returns: Now, or nothing where no clock is installed at all; a caller
///   then degrades, as a command duration reads as unknown.
[[nodiscard]] std::optional<Ticks> terminalNow(const Ubiquitty::Terminal& t);

/// Takes bytes the terminal wants to send back to the host (DSR, DA, and so on).
///
/// - Parameters:
///   - t: The terminal to drain.
///   - output: Receives the bytes.
/// - Returns: Bytes written. Zero when nothing is queued; call again while it is
///   non-zero, since one call drains at most what `output` holds.
int drainOutput(Ubiquitty::Terminal* t, OutboundBytes output);

/// Encodes an xterm mouse report for zero-based cell coordinates.
///
/// See `Ubiquitty::Mouse::encodeReport` for the wire forms.
///
/// - Parameters:
///   - t: The terminal whose active mouse mode decides the encoding.
///   - button: Button index.
///   - row: Zero-based cell row.
///   - column: Zero-based cell column.
///   - modifiers: Modifier bitmask.
///   - output: Receives the encoded report.
///   - pixelX: Device-pixel position, read only by SGR-pixel mode (DECSET 1016).
///   - pixelY: The same, vertically.
/// - Returns: Bytes written, or 0 when the active mode reports nothing.
[[nodiscard]] int encodeMouseReport(const Ubiquitty::Terminal& t, Ubiquitty::Mouse::EventType, int button, int row, int column, int modifiers, OutboundBytes output, int pixelX = -1, int pixelY = -1);

// --- DEC VT300 locator protocol (DECELR/DECSLE/DECRQLP/DECEFR) -----
//
// The parser stores the locator state on Terminal (DECELR enable, DECSLE event
// selection, DECEFR filter rectangle) and answers DECRQLP itself. The
// button/motion → DECLRP emit path is driven by the frontend mouse handler:
// when locatorActive(t) is true it routes every pointer event through
// locatorReportEvent (a parallel reporting mode to the X10/SGR mouse modes —
// gate it so normal mouse reporting is unaffected) and writes the returned
// bytes to the host. Every event, not just the reportable ones: the call is
// also how the Core learns where the locator is, which is what DECRQLP
// answers with and what DECEFR resolves an omitted rectangle bound against.

/// Transfers complete reply buffers, retaining a refused buffer for retry.
/// The callback accepts its entire span or none of it and cannot mutate the terminal.
/// - Parameters:
///   - terminal: The terminal, with its model lock held.
///   - accept: Returns true after accepting every byte in the span.
///   - context: Borrowed state passed to the callback.
void flushOutput(Terminal& terminal, bool (*accept)(void*, std::span<const char>), void* context);

// Query DECSCNM (reverse video). The renderer swaps foreground/background at draw time when
// this is true; exposed as a function so PAL code does not need to know
// the Ubiquitty::Terminal layout.
[[nodiscard]] bool isReverseVideo(const Ubiquitty::Terminal* t);

/// The theme foreground chrome tints to — a tab title, a find bar — read off
/// the model's palette in 8-bit channels, the depth every theme arrives at.
///
/// One derivation for every frontend's chrome: a frontend narrowing the
/// 16-bit palette itself is a second reading of the theme, and the two drift
/// the day one of them rounds differently.
///
/// - Parameter t: The terminal. Null yields the compiled-in default.
/// - Returns: The colour, alpha opaque.
[[nodiscard]] ColorRGBA chromeForegroundColor(const Ubiquitty::Terminal* t);

/// The theme background, the chrome twin of ``chromeForegroundColor``.
///
/// - Parameter t: The terminal. Null yields the compiled-in default.
/// - Returns: The colour, alpha opaque.
[[nodiscard]] ColorRGBA chromeBackgroundColor(const Ubiquitty::Terminal* t);

/// The theme's accent, the hue chrome tints a highlight, a caret rule or a
/// command palette's selection with.
///
/// ANSI blue (index 4), never the cursor colour: it is the conventional UI
/// hue, it is read live so an OSC 4 override moves it, and the cursor
/// fallback one shell carried made an accent that matched the caret rather
/// than the theme. A frontend picking its own accent is chrome that stops
/// matching its theme.
///
/// - Parameter t: The terminal. Null yields the compiled-in default.
/// - Returns: The colour, alpha opaque.
[[nodiscard]] ColorRGBA chromeAccentColor(const Ubiquitty::Terminal* t);

/// The theme's cursor colour, for chrome that draws a caret of its own, so
/// it wears the colour the caret in the grid wears.
///
/// - Parameter t: The terminal. Null yields the compiled-in default.
/// - Returns: The colour, alpha opaque.
[[nodiscard]] ColorRGBA chromeCursorColor(const Ubiquitty::Terminal* t);

/// The colours a terminal draws with: its palette's, through the narrowing the
/// chrome colours above take, and the selection colours a program set.
///
/// - Parameter t: The terminal, read under the caller's model lock.
/// - Returns: ``Rendering/paletteColors`` of its palette, with the OSC 17 and
///   19 colours.
[[nodiscard]] Rendering::PaletteColors terminalColors(const Ubiquitty::Terminal& t);

// OSC 1337 "SetUserVar" — read back a per-session user variable's value by
// name. Returns a pointer into the Terminal's user-var table (NUL-terminated,
// valid until the slot is overwritten/cleared), or nullptr if `name` is
// unset. Set values via the OSC 1337 dispatcher in Core/OSC/Dispatch.cpp.
[[nodiscard]] std::optional<std::string_view> userVariable(const Ubiquitty::Terminal* t, std::string_view name);

} // namespace Ubiquitty
