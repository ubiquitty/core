// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Keys/KeyEvent.h"
#include "Core/VT/UserDefinedKeys.h"
#include "Forward.h"
#include <array>
#include <cstdint>

namespace Ubiquitty::Runtime {

// Mirrors Core's xtermKeyModifierCategoryCount (= 8). A static_assert in
// TerminalRuntime.cpp pins them equal so this stays in lockstep with the Ubiquitty::Terminal field.
inline constexpr int xtermKeyModifierCategories = 8;

// Terminal modes a frontend reads off the published snapshot: the ones the input
// path consults on every keystroke, and the few chrome bits a shell applies each
// drain. Field types match the Core Ubiquitty::Terminal fields exactly, so the
// mirror can never silently truncate.
struct ModeSnapshot {
    bool applicationCursorKeys = false; // DECCKM        (Terminal: appCursorKeys)
    bool shouldReportKeyPositions = false;
    bool shouldTransmit8BitC1 = false;
    std::uint8_t keyPositionReportedModifiers = 0x30;
    std::uint8_t keyPositionDisabledModifiers = 0;
    bool applicationKeypad = false; // DECKPAM       (Terminal: appKeypad)
    bool shouldHonorNumLock = true;
    Keys::KeyboardType keyboardType = Keys::KeyboardType::Default;
    Keys::TermcapProfile termcapProfile = Keys::TermcapProfile::Ubiquitty;
    bool shouldReportPasteEvents = false;
    bool bracketedPaste = false; // DECSET 2004   (Terminal: bracketedPaste)
    bool sendReceiveMode = true; // SRM           (Terminal: srm — true at init/reset)
    bool vt52Mode = false; // DECANM=off    (Terminal: vt52Mode)
    bool lineFeedNewLineMode = false; // LNM (mode 20) (Terminal: lnm)
    bool autoRepeat = true; // DECARM (8)    (Terminal: autoRepeat)
    // Meta/Alternate keyboard discipline (xterm 1036/1039/1034): the encoder picks
    // escape-prefix vs. 8th-bit byte for an Alternate/Meta-modified printable.
    bool metaSendsEscape = true; // mode 1036    (Terminal: metaSendsEscape)
    // foot 77096: reset means the program wants raw keys, so a frontend hands
    // the event to its key encoder without offering it to the input method.
    bool allowsInputMethodEditor = true; // mode 77096  (Terminal: allowsInputMethodEditor)
    bool alternateSendsEscape = false; // mode 1039    (Terminal: alternateSendsEscape)
    bool eightBitInput = false; // mode 1034     (Terminal: eightBitInput)
    bool deleteKeySendsDelete = false; // mode 1037    (Terminal: deleteKeySendsDelete)
    bool alternateScreenActive = false; // alternate-screen    (Terminal: isUsingAlternateScreen) — kitty slot
    // rxvt DECSET/DECRST ?30: whether the application asks for a scroll bar at
    // all. Chrome rather than input, and here because this is the one mode
    // surface both macOS shells read each drain; Runtime::scrollerChrome turns
    // it into what a bar does. Shown by default, matching a reset terminal, so
    // a snapshot read before the first publish cannot take a bar away.
    // (Terminal: scrollbarVisible)
    bool isScrollbarVisible = true;
    // A password prompt is on screen: private mode 7401, or the [security]
    // password_detect heuristic settled on a prompt row. The frontend masks
    // local echo and shows its lock while this holds. (Terminal: passwordPromptActive)
    bool isPasswordPromptActive = false;
    // Ubiquitty-private DECSET 7402, fullscreen content: a fitted image fills
    // the whole drawable from its top-left corner, whatever the letterbox, so
    // a pixel report and the extent CSI 14t advertises measure the drawable
    // while it holds. Whether the GRID drops its padding is the letterbox's
    // half of the answer, published resolved on the runtime snapshot as
    // isEdgeToEdge. (Terminal: fullscreenContent)
    bool isFullscreenContentActive = false;
    // Linux-console DPMS: the program has the display blanked, and CSI 13 ]
    // wakes it. A shell that installs no WindowClient reads this each drain
    // and covers its view while it holds. (Terminal: screenBlanked)
    bool isScreenBlanked = false;
    std::array<std::uint16_t, 2> kittyKeyboardFlags { { 0, 0 } }; // [main, alternate]
    std::array<std::uint8_t, xtermKeyModifierCategories> xtermKeyModifiers {};
    std::array<std::uint8_t, xtermKeyModifierCategories> xtermKeyFormats {};
    // How many keys DECUDK has programmed. The table itself is published beside
    // this snapshot rather than in it: it dwarfs every other field here, it is
    // empty in every session that sends no DECUDK, and this snapshot is copied
    // per key press and per mouse query. Zero here means the encoder needs no
    // table at all.
    int userDefinedKeyCount = 0;
};

/// Samples the keyboard/terminal modes off a live terminal.
///
/// The one place the mapping is written. The runtime samples under its lock so
/// the platform can read lock-free; a frontend that holds the terminal
/// directly — the web page, the wscons console, the GTK shell — samples here
/// and gets the same fields, rather than reaching into `Terminal::modes` for
/// each one and picking a different member or array index than its siblings.
///
/// - Parameter term: The terminal to read.
/// - Returns: Its modes.
[[nodiscard]] ModeSnapshot sampleModes(const Ubiquitty::Terminal& term);

/// The terminal's DECUDK table, or nullptr when it has none.
///
/// The pointer `encodeKey` takes beside the modes `sampleModes` returns. Valid
/// only while the caller holds the terminal.
///
/// - Parameter term: The terminal to read.
/// - Returns: Its DECUDK table, or nullptr when no key was programmed.
[[nodiscard]] const VT::UserDefinedKeys* sampleUserDefinedKeys(const Ubiquitty::Terminal& term);

} // namespace Ubiquitty::Runtime
