// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Synthesize one context-aware action intent ("split side-by-side", "rename this
// window") into the right bytes for wherever the focused pane's input lands. The
// frontend reads the pane's facts and routes the result (ControlCommand over -CC,
// Keystrokes through the PTY, or None → its own native action); `classifyPaneContext`
// here decides the context, so neither shell carries its own copy of the
// discrimination. Depends only on Commands/PaneID, the Swift-name spelling macro
// and the C++ standard library — no platform headers.

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/PaneID.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Tmux {

// Where the focused pane's input lands.
enum class PaneContext : uint8_t {
    NativePane UBIQUITTY_SWIFT_NAME(nativePane), // plain local PTY, no tmux anywhere
    RawTmuxAttach UBIQUITTY_SWIFT_NAME(rawAttach), // the pane runs a full-screen `tmux attach` —
                                                   // only prefix-chord keystrokes can drive it
    ControlMode UBIQUITTY_SWIFT_NAME(controlMode), // a -CC render-only pane — the control channel drives it
};

/// What a pane's context is read from.
///
/// The frontend reads these off the pane's session; the rule that turns them
/// into a context is `classifyPaneContext`, so neither shell carries its own
/// copy of the discrimination.
struct PaneContextFacts {
    /// Whether the pane's input rides a -CC primary's control channel.
    bool isControlModePane = false;
    /// The name of the process holding the pane's terminal, empty when unknown.
    std::string_view foregroundName;
    /// Whether a foreground process's command line carries `-CC`: a
    /// control-mode client, which the integration attaches to and which a
    /// prefix chord must never reach — its stdin is the control protocol, and
    /// a chord written there is garbage to it.
    bool isForegroundControlModeTmux = false;
};

/// Says where a pane's input lands.
///
/// A -CC render-only pane takes control commands; a foreground `tmux` whose
/// command line carries no `-CC` is a full-screen raw attach that only prefix
/// chords drive; anything else, a typed `tmux -CC` not yet attached included,
/// is a native pane.
///
/// - Parameter facts: What the pane's session says about it.
/// - Returns: The context the pane's intents take.
[[nodiscard]] inline PaneContext classifyPaneContext(const PaneContextFacts& facts)
{
    if (facts.isControlModePane)
        return PaneContext::ControlMode;
    if (facts.foregroundName == "tmux" && !facts.isForegroundControlModeTmux)
        return PaneContext::RawTmuxAttach;
    return PaneContext::NativePane;
}

enum class ActionIntent : uint8_t {
    SplitSideBySide UBIQUITTY_SWIFT_NAME(splitSideBySide),
    SplitStacked UBIQUITTY_SWIFT_NAME(splitStacked),
    KillPane UBIQUITTY_SWIFT_NAME(killPane),
    NewWindow UBIQUITTY_SWIFT_NAME(newWindow),
    RenameWindow UBIQUITTY_SWIFT_NAME(renameWindow), // carries `value` (the new name)
};

// What to send, and over which transport. ControlCommand bytes follow the
// Commands builder contract (no trailing newline — the transport appends
// it); Keystrokes are raw bytes for the pane's PTY input path.
struct ContextAction {
    enum class Transport : uint8_t {
        None, // no synthesis — the frontend's own native action applies
        ControlCommand,
        Keystrokes,
    };
    Transport transport = Transport::None;
    std::string bytes;
};

// tmux's default prefix, C-b. A user with a remapped prefix passes theirs.
inline constexpr char tmuxDefaultPrefixKey = 0x02;

// Strip control bytes from a user-typed value before it rides either
// transport: in a keystroke stream they would inject chords/escapes into
// tmux's rename prompt; in a command line they are junk in a window name.
inline std::string sanitizeActionValue(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)
            out.push_back(c);
    }
    return out;
}

// Synthesize `intent` for `context`. `focusedPane` is the -CC pane id (ControlMode
// only); `value` carries RenameWindow's new name; `prefixKey` is the raw-attach chord
// prefix. Native-pane intents return None (the frontend implements them natively).
inline ContextAction synthesizeAction(PaneContext context, ActionIntent intent, PaneID focusedPane, std::string_view value, char prefixKey = tmuxDefaultPrefixKey)
{
    using Transport = ContextAction::Transport;

    if (context == PaneContext::ControlMode) {
        switch (intent) {
        case ActionIntent::SplitSideBySide:
            return { Transport::ControlCommand, splitWindow(focusedPane, SplitDirection::SideBySide) };
        case ActionIntent::SplitStacked:
            return { Transport::ControlCommand, splitWindow(focusedPane, SplitDirection::Stacked) };
        case ActionIntent::KillPane:
            return { Transport::ControlCommand, killPane(focusedPane) };
        case ActionIntent::NewWindow:
            return { Transport::ControlCommand, newWindow() };
        case ActionIntent::RenameWindow:
            return { Transport::ControlCommand, renameWindow(focusedPane, sanitizeActionValue(value)) };
        }
        return {};
    }

    if (context == PaneContext::RawTmuxAttach) {
        std::string keys(1, prefixKey);
        switch (intent) {
        case ActionIntent::SplitSideBySide:
            keys += '%';
            break;
        case ActionIntent::SplitStacked:
            keys += '"';
            break;
        case ActionIntent::KillPane:
            // The bare chord — tmux's own y/n confirm prompt supplies the
            // safety gate, so no needsConfirm duplication is required here.
            keys += 'x';
            break;
        case ActionIntent::NewWindow:
            keys += 'c';
            break;
        case ActionIntent::RenameWindow:
            // The rename prompt opens pre-filled with the current name; C-u (\x15)
            // clears it (default emacs status-keys) before the new one is typed.
            keys += ',';
            keys += '\x15';
            keys += sanitizeActionValue(value);
            keys += '\r';
            break;
        }
        return { Transport::Keystrokes, std::move(keys) };
    }

    return {}; // NativePane: the frontend's own operation applies
}

} // namespace Ubiquitty::Protocol::Tmux
