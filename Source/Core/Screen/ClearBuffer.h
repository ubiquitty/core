// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Screen/Marks.h" // ClearToMarkOutcome

#include <cstdint>
#include <functional>
#include <span>

namespace Ubiquitty {

struct Terminal;
struct SelectionState;

// The three clear *commands*, as a user invokes them.
//
// Distinct from the model primitives in `Terminal.h` and `Screen/Scrollback.h`, which
// change the grid and stop there. A command also drops what the change made
// stale — a selection naming rows that are gone, a view scrolled into them —
// and tells the process to redraw when the screen moved under it. Those steps
// are the ones a frontend omits, so no frontend performs them.
//
// The namespace also keeps `Screen::clearBuffer` distinct from
// `Ubiquitty::clearBuffer`, which is the raw grid edit.
namespace Screen {

/// Snaps the view back to the live tail; the history it showed is gone.
///
/// A frontend displaying only the live grid passes an empty lambda.
using ResetViewOffset = std::function<void()>;

/// Forwards `bytes` to the foreground process.
///
/// Clearing moves the prompt to row 0 in the model while the shell's line
/// editor still believes it sits where it was, so without this the next
/// characters echo at the old row. Whether the bytes travel as a PTY write,
/// a protocol frame or a socket send is the frontend's business; that they
/// travel is not. A caller with no live process — a headless render — passes
/// an empty lambda.
using SendToProcess = std::function<void(std::span<const char> bytes)>;

/// Where a screen clear stops: the first row worth keeping on screen.
enum class ClearBoundary : uint8_t {
    /// The current prompt's top row (OSC 133 A), falling back to the cursor row
    /// when no prompt is pending. Shell integration makes the boundary exact;
    /// without it the cursor row is the same guess every terminal makes.
    CurrentPrompt,
    /// The cursor row, whatever sits above it. Needs no shell integration.
    CursorRow,
};

/// How ``clearScreen`` resolved the request, for a caller that reports it.
enum class ClearOutcome : uint8_t {
    Scrolled, // the rows above the boundary moved into scrollback
    HandedOff, // a program owns the layout, so it got the redraw request instead
    NoOp, // the boundary is already the top row; there is nothing above it
};

/// Moves the boundary row to the top of the screen by scrolling, not erasing.
///
/// The rows above it go into scrollback exactly as they would had the user
/// pressed Return enough times, so the screen comes up clean and nothing is
/// destroyed. That choice is what makes this safe: there is no sequence for
/// "I erased your screen, redraw it", so a terminal that erases behind a
/// running program leaves damage nothing can repair — while a scroll is the
/// operation every program already survives, because ordinary output performs
/// it on every line that overflows the screen.
///
/// Two consequences fall out. No bytes go to the process on the scrolling
/// path: the prompt did not move relative to its own content, so the shell's
/// line editor is still right about where it sits. And a mirrored grid needs
/// no baseline reset, for the same reason a screenful of output does not.
///
/// When a program owns the layout — the alternate screen, a scroll region or
/// left/right margins — the terminal cannot tell which rows are dead, so it
/// stops guessing and forwards the redraw request instead. That is a handoff
/// rather than a refusal: Ctrl-L is what shells, editors, pagers and
/// multiplexers all read as "redraw", so the user's intent still reaches the
/// layer that can act on it.
///
/// - Parameter selection: Dropped when present; null for no selection model.
/// - Returns: Which path ran, for a caller that reports it.
ClearOutcome clearScreen(Terminal*, ClearBoundary, SelectionState* selection, const ResetViewOffset&, const SendToProcess&);

/// Clears the screen and the scrollback, keeping the line being typed.
///
/// Destructive, and what Cmd-K reaches: a clear that leaves the rows one
/// scroll away has not cleared anything. The Ctrl-L that follows is not
/// optional — the shell's line editor still believes its prompt sits where the
/// screen no longer shows it.
///
/// - Parameter selection: Dropped when present; null when the frontend has
///   no selection model.
void clearBuffer(Terminal*, SelectionState* selection, const ResetViewOffset&, const SendToProcess&);

/// Drops the scrollback and leaves the visible grid alone.
///
/// History is not the screen, so a caller wanting the display to change
/// wants ``clearBuffer``. No process bytes: the screen did not move, so the
/// shell's idea of where its prompt sits is still correct.
///
/// - Parameter selection: Dropped when present; null for no selection model.
void clearScrollback(Terminal*, SelectionState* selection, const ResetViewOffset&);

/// Drops history above the most recent executed prompt, or clears the buffer
/// when there is no such prompt.
///
/// The stale state is dropped only when rows actually went. A search that
/// finds no prompt changes nothing on its own, so a selection valid before
/// is still valid — but the fallback then clears the buffer, which does
/// invalidate it, and does need the redraw request.
///
/// - Parameter selection: Dropped when present; null for no selection model.
/// - Returns: Which of the two paths ran, for a caller that reports it.
ClearToMarkOutcome clearToPreviousMark(Terminal*, SelectionState* selection, const ResetViewOffset&, const SendToProcess&);

/// The clear commands as a menu offers them, so a frontend names the one the
/// user asked for rather than reproducing the choice between them.
///
/// Which command a gesture reaches is a product decision, and the four
/// signatures above differ enough that every frontend restating the mapping
/// gets a chance to restate it differently. This is that mapping, once.
///
/// Attached clients name these commands in a typed `pane.clear` Call; the
/// host runs the command and publishes its authoritative screen and history.
enum class ClearCommand : uint8_t {
    /// Screen and scrollback both, keeping the line being typed. Cmd-K.
    Buffer = 0,
    /// The history the screen is not showing; the screen stands. Opt-Cmd-K.
    Scrollback = 1,
    /// The current prompt scrolls to the top; nothing is destroyed.
    Screen = 2,
    /// Everything above the most recent executed prompt. Cmd-L.
    ToPreviousMark = 3,
};

/// Runs one clear command.
///
/// The one place the command a user invoked becomes the call that performs it.
/// A frontend supplies what only it holds — the view's selection, the route to
/// the process — and is told the one thing it must then do itself.
///
/// - Parameters:
///   - command: Which clear the user asked for.
///   - selection: Dropped when the rows it names go; null for a frontend with
///     no selection model.
///   - sendToProcess: The route to the foreground process, for the redraw
///     request a moved screen owes the shell.
/// - Returns: Whether the viewport must return to the live tail. False when
///   the command handed off to a program that owns the layout, and false when
///   there is no terminal to clear.
bool runClearCommand(ClearCommand command, Terminal*, SelectionState* selection, const SendToProcess& sendToProcess);

} // namespace Screen
} // namespace Ubiquitty
