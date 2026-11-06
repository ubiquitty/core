// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Internal.h — cross-TU seam declarations for the OSC dispatcher split. Dispatch.cpp holds the dispatch
// surface; handler bodies live in capability-slice TUs (OSCColor/OSCClipboard/OSCState/OSCNotify/
// OSCShellIntegration/OSC1337). Slice-private handlers stay file-scope static; only cross-cut seams are
// declared here. Private to the OSC*.cpp group, never a frontend include.

#pragma once

#include "Core/OSC/Dispatch.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Ubiquitty::OSC {

// ---- Shared scalar helpers (definitions noted per seam) -------------

// Decimal accumulate with saturation at `limit`. Defined in Dispatch.cpp; the
// dispatch leading-Ps parse and several handler slices reuse it.

/// A buffer sized for the largest selection a clipboard reply may carry.
///
/// Both read paths need the same bounded allocation and the same answer when it cannot be taken.
/// What they do not share is when they read: OSC 52 reads once, while OSC 5522 asks a typed client
/// for each name it serves and may never read the untyped selection at all. So this is the buffer
/// and not the read — an eager read here would make a typed client answer a query nobody asked it.
///
/// The bound is the protocol's ceiling rather than the reply channel's: a buffer sized to the fixed
/// output buffer carries the first 168 bytes of the clipboard and says nothing about the rest.
class ClipboardReadBuffer {
public:
    /// Takes the allocation, or leaves the buffer empty when it cannot be had.
    ///
    /// Nothrow, because Core builds without exceptions: a throwing allocation that fails terminates
    /// the process over a clipboard query. A caller checks the buffer before reading into it.
    ClipboardReadBuffer();

    /// Whether the allocation was taken. A caller that finds it empty answers the query rather than
    /// reading into nothing.
    [[nodiscard]] explicit operator bool() const { return m_bytes != nullptr; }

    /// The writable span a read fills, empty when the allocation was refused.
    [[nodiscard]] std::span<char> span() const;

    /// The buffer's base, for a caller that needs the pointer itself.
    [[nodiscard]] char* data() const { return m_bytes.get(); }

private:
    std::unique_ptr<char[]> m_bytes;
};

// Single hex digit → 0..15, or -1. Defined in Color.cpp; the OSC 7 URL
// decoder and OSC 633;E command-line decoder also need it.
/// Converts one hexadecimal digit to its value.
///
/// - Parameters:
///   - c: The character.
/// - Returns: 0 to 15, or -1 when the character is not a hex digit.
int hexDigit(char c);

// Mirror a decoded working directory onto Terminal::currentDirectory (NUL-
// terminated, truncated). Defined in State.cpp; OSC 7 / 633;P;Cwd /
// 1337 CurrentDir share it.
/// Mirrors a decoded working directory onto the terminal, NUL-terminated and truncated to fit.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - path: The decoded absolute path.
void setCurrentDirectory(Ubiquitty::Terminal& t, std::string_view path);

// ---- Prompt-mark helpers (defined in ShellIntegration.cpp) -------
// Shared by OSC 133, OSC 633, and OSC 1337 SetMark.
/// Sets a prompt-mark bit on the row the cursor is on.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - bit: The mark to set.
void setPromptMark(Ubiquitty::Terminal& t, uint8_t bit);
/// Remembers a prompt mark whose row is not final yet.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void recordPendingPromptMark(Ubiquitty::Terminal& t);
/// Commits a remembered prompt mark once its row settles.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void promotePendingPromptMark(Ubiquitty::Terminal& t);

// ---- Handler bodies invoked by the Dispatch.cpp dispatch wrappers --------
// Each is the cold body for one (or a small family of) OSC code(s); the
// matching thin oscH_* wrapper in Dispatch.cpp forwards here, and the gate the
// code hangs on is declared beside it in that file's table.
//
// A name here says what the code does, unless the code is a namespace rather
// than an effect. handleOSC9, handleOSC133, handleOSC633, handleOSC777,
// handleOSC1337 and handleOSC21337 each dispatch a set of sub-commands that
// share nothing but the number that introduces them — OSC 1337 alone carries
// CurrentDir, RemoteHost, Copy and File among them — so the number is the only
// subject they have, and an effect-shaped name would describe one branch of
// several. The
// wrapper still names the effect where there is one to name, which is why
// oscH_progressOrNotification sits in front of handleOSC9.

// Color slice (Color.cpp).
/// Sets or queries an indexed palette colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleIndexedColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets, resets or queries named and indexed colors using kitty's OSC 21.
///
/// - Parameters:
///   - terminal: The terminal receiving the control.
///   - payload: The semicolon-separated color fields.
void handleKittyColors(Ubiquitty::Terminal& terminal, std::string_view payload);
/// Sets or queries a special colour, the codes above the 256 indexed ones.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleSpecialColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the cursor colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleCursorColor(Ubiquitty::Terminal& t, std::string_view p);
/// Resets indexed palette entries to their defaults, or all of them when the payload is empty.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void resetIndexedColor(Ubiquitty::Terminal& t, std::string_view p);
/// Resets special colours to their defaults.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void resetSpecialColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the dynamic colours, OSC 10 through 19, starting at `osc`.
///
/// Each ';'-separated field applies to the next code in the list, and each `?`
/// field replies under its own code.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
///   - osc: The wire code, which the first field addresses.
void handleDynamicColors(Ubiquitty::Terminal& t, std::string_view p, int osc);
/// Sets the mouse pointer shape by name.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleMousePointerShape(Ubiquitty::Terminal& t, std::string_view p);
/// Restores the default foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetDefaultForeground(Ubiquitty::Terminal& t);
/// Restores the default background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetDefaultBackground(Ubiquitty::Terminal& t);
/// Restores the cursor colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetCursorColor(Ubiquitty::Terminal& t);
/// Restores the selection background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetSelectionBackground(Ubiquitty::Terminal& t);
/// Restores the selection foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetSelectionForeground(Ubiquitty::Terminal& t);
/// Sets or queries the selection background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleSelectionBackgroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the selection foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleSelectionForegroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the mouse pointer foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleMousePointerForegroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the mouse pointer background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleMousePointerBackgroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the Tektronix foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleTektronixForegroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the Tektronix background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleTektronixBackgroundColor(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the Tektronix cursor colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleTektronixCursorColor(Ubiquitty::Terminal& t, std::string_view p);
/// Restores the mouse pointer foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetMousePointerForeground(Ubiquitty::Terminal& t);
/// Restores the mouse pointer background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetMousePointerBackground(Ubiquitty::Terminal& t);
/// Restores the Tektronix foreground colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetTektronixForeground(Ubiquitty::Terminal& t);
/// Restores the Tektronix background colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetTektronixBackground(Ubiquitty::Terminal& t);
/// Restores the Tektronix cursor colour.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
void resetTektronixCursor(Ubiquitty::Terminal& t);

// Clipboard slice (Clipboard.cpp).
/// Reads or writes the clipboard, subject to the per-terminal OSC 52 policy.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleClipboard(Ubiquitty::Terminal& t, std::string_view p);

/// Handles the OSC 5522 extended clipboard, subject to the same policy.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleExtendedClipboard(Ubiquitty::Terminal& t, std::string_view p);

/// Completes a deferred 5522 read with the frontend's decision.
///
/// The 5522 framing of the answer ``completeClipboardReadQuery`` routes here
/// when the outstanding read is the extended protocol's; frontends keep
/// calling the one completion they already call.
///
/// - Parameters:
///   - t: The terminal whose read was outstanding.
///   - decision: What the frontend decided.
void completeExtendedClipboardRead(Ubiquitty::Terminal& t, ClipboardReadDecision decision);

// State slice (State.cpp): cwd / file URL / hyperlink / font.
/// Records the current document URL, the file analogue of OSC 7.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleCurrentFileURL(Ubiquitty::Terminal& t, std::string_view p);
/// Records the working directory from a `file://` URL, ignoring the host segment.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleWorkingDirectory(Ubiquitty::Terminal& t, std::string_view p);
/// Opens or closes a hyperlink span, which subsequent cells carry.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleHyperlink(Ubiquitty::Terminal& t, std::string_view p);
/// Sets or queries the font.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleFont(Ubiquitty::Terminal& t, std::string_view p);
/// Sets, changes, restores or queries the font size, mintty's `OSC 7770`,
/// through the frontend's font change, which keeps columns and rows where
/// mintty keeps the window; the query answers the size the frontend draws in.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleFontSize(Ubiquitty::Terminal& t, std::string_view p);
/// Sets, changes, restores or queries the font size keeping the character
/// geometry, mintty's `OSC 7777`.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleFontAndWindowSize(Ubiquitty::Terminal& t, std::string_view p);

// Notification slice (Notify.cpp).
/// Posts a desktop notification, or routes ConEmu progress when the payload begins `4;`.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC9(Ubiquitty::Terminal& t, std::string_view p);
/// Applies VTE progress properties and answers termprop queries (OSC 666).
///
/// - Parameters:
///   - t: The terminal receiving the properties.
///   - payload: The statements following the selector.
void handleOSC666(Ubiquitty::Terminal& t, std::string_view payload);
/// Posts a desktop notification in kitty's form.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleKittyNotification(Ubiquitty::Terminal& t, std::string_view p);
/// Posts a desktop notification in urxvt's form.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC777(Ubiquitty::Terminal& t, std::string_view p);
/// Handles the extended iTerm2 code that shares the 1337 family.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC21337(Ubiquitty::Terminal& t, std::string_view p);
/// Handles iTerm2's tab colour, the `1;bg;` form of OSC 6.
///
/// `<channel>;brightness;<0-255>` sets one of `red`, `green` or `blue`, and
/// `*;default` clears the colour. Anything else changes nothing.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload after `1;bg;`.
void handleTabColor(Ubiquitty::Terminal& t, std::string_view p);
/// Stores iTerm2's badge, the value of OSC 1337 `SetBadgeFormat=`.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - encoded: The base64 text; empty clears the badge.
void handleBadgeFormat(Ubiquitty::Terminal& t, std::string_view encoded);

// Text-sizing slice (TextSizing.cpp): the kitty text-sizing wire surface.
/// Handles an OSC 66 text-sizing code: writes its text as a sized block, and drops
/// a malformed or oversized code whole.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC66(Ubiquitty::Terminal& t, std::string_view p);

// Shell-integration slice (ShellIntegration.cpp): prompt marks.
/// Records a prompt mark in FinalTerm's scheme, which drives prompt navigation.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC133(Ubiquitty::Terminal& t, std::string_view p);
/// Records a prompt mark in the VS Code scheme, and its command-line payload.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC633(Ubiquitty::Terminal& t, std::string_view p);

// iTerm2 slice (ITerm2.cpp).
/// Handles iTerm2's proprietary family, including inline images and the current directory.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
///   - p: The payload, after the code and its separator.
void handleOSC1337(Ubiquitty::Terminal& t, std::string_view p);

} // namespace Ubiquitty::OSC
