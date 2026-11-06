// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// PointerReportingPolicy.h — the pure decision of whether a pointer event drives
// the terminal's OWN GUI layer (link hover/open, local selection) or is forwarded
// to a full-screen app under xterm mouse reporting.
//
// One modifier per gesture, and no preference can put two of them on the same
// chord. Shift reaches a SELECTION through an app that is holding the mouse —
// xterm's own bypass, so it is always there — and the block chord
// ([selection] block_select_modifier) reaches through the same way for a
// rectangular one. Command reaches a LINK, which is a different gesture on a
// different target. What flips underneath is the DEFAULT OWNER of a plain drag:
// a -CC pane is OURS (rendered natively) so a plain drag selects even under the
// pane app's mouse mode; a real/passthrough surface is the APP's once it reports
// the mouse. A modifier reaches through from app-owned to local, and is never
// repurposed to forward.

namespace Ubiquitty::Input {

// The inputs to the report-vs-local decision for one pointer event.
struct PointerReportingState {
    bool isMouseReporting = false; // the app enabled xterm mouse reporting
    // Each of these is a chord the reader holds and holds nothing else beside.
    // Matched as "this key is among the ones down" they overlap: a block chord
    // of "shift+opt" would arm the flowing selection as well, and one naming
    // Command would arm the link gesture — the reader reaches for one gesture
    // and gets two.
    bool isSelectionOverrideModifierArmed = false; // configured modifier: reach a selection through a tracking app
    bool isBlockSelectionOverrideModifierArmed = false; // [selection] block_select_modifier: the same reach, rectangular
    bool isLinkModifierArmed = false; // Command: reach a link
    bool isControlModePane = false; // the pane is a tmux -CC surface we render
    // [selection] links_override_reporting: unmodified link hover + click-open stay
    // LOCAL even under mouse reporting. Defaults false here (conservative — an
    // unpopulated state never steals a report) while the config accessor defaults true.
    bool isLinkOverrideEnabled = false;
};

/// Who owns a plain (unmodified) pointer drag by default. The surface owner: a
/// -CC pane is ours (rendered + driven natively), so it stays Terminal-owned
/// even under the pane app's mouse mode; a real/passthrough terminal is App-owned
/// once the app reports the mouse. Command reaches through from App to Terminal;
/// it never flips the owner.
enum class DragOwner : bool {
    Terminal,
    App
};
inline DragOwner plainDragDefaultOwner(const PointerReportingState& s)
{
    if (s.isControlModePane)
        return DragOwner::Terminal; // -CC surface is ours; the app's mouse mode is redundant
    return s.isMouseReporting ? DragOwner::App : DragOwner::Terminal;
}

// Local LINK interaction (hover reveal + click-open) may run when the app isn't
// reporting, when Command is held, or when links_override_reporting keeps links
// local under reporting. Command is unconditional here because it is the
// platform's own gesture for following a link and carries no other meaning on a
// link; links_override_reporting governs the PLAIN click instead. LINK-only: a
// plain drag under reporting still forwards; only a click/hover ON a link is local.
inline bool allowsLocalLinkInteraction(const PointerReportingState& s) { return !s.isMouseReporting || s.isLinkModifierArmed || s.isLinkOverrideEnabled; }

// The scheme allowlist — {http, https, file}, case-insensitive — for the
// no-modifier auto-detected link open/hover under reporting. Scheme = the run
// before the first ':' (RFC 3986), lowercased. A bare/scheme-less path is the
// file-path candidate (isFile), kept ⌘-only; every other scheme returns false.
inline bool isOpenableURLScheme(std::string_view url)
{
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false; // no scheme delimiter, or an empty scheme
    const std::string_view scheme = url.substr(0, colon);
    const auto equalsLowercased = [](std::string_view candidate, std::string_view lowercaseLiteral) {
        if (candidate.size() != lowercaseLiteral.size())
            return false;
        for (std::size_t i = 0; i < candidate.size(); ++i) {
            char c = candidate[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != lowercaseLiteral[i])
                return false;
        }
        return true;
    };
    return equalsLowercased(scheme, "http") || equalsLowercased(scheme, "https") || equalsLowercased(scheme, "file");
}

// The classified inputs to the plain-openable verdict for one detected link.
struct PlainOpenableLink {
    bool isOSC8 = false; // an app-stamped OSC 8 link (the app opted this span in)
    bool isFile = false; // a file-path candidate (the loose ⌘-path opens those)
    bool hasOpenableScheme = false; // the resolved URL's scheme is http/https/file
};

// Whether a link may be opened by a PLAIN (no-modifier) click / revealed by hover
// under mouse reporting. Kind/scheme verdict only (caller applies the context
// gate): OSC 8 always qualifies; a file-path candidate never does (⌘-only);
// otherwise an http/https/file scheme. The two errors are not symmetric: a link
// this rejects still opens with ⌘, while one it wrongly accepts hands a plain
// click to whatever the app printed — so it under-covers.
inline bool plainClickOpenableUnderReporting(const PlainOpenableLink& link)
{
    return link.isOSC8 ? true : (link.isFile ? false : link.hasOpenableScheme);
}

// Local SELECTION drag may run when we own the surface by default (not
// reporting, or a -CC pane), or a bypass modifier reaches through from an
// app-owned surface: Shift for a linear selection, the block chord for a
// rectangular one.
inline bool allowsLocalDragSelect(const PointerReportingState& s) { return plainDragDefaultOwner(s) == DragOwner::Terminal || s.isSelectionOverrideModifierArmed || s.isBlockSelectionOverrideModifierArmed; }

// True when a local drag is RECTANGULAR: the reader's block chord is held
// exactly. Every other local drag is linear.
inline bool localDragIsRectangular(const PointerReportingState& s) { return s.isBlockSelectionOverrideModifierArmed; }

/// What a pointer event asks of the link scan.
enum class LinkProbe : std::uint8_t {
    /// Nothing: no link interaction is local here, so no scan runs.
    None UBIQUITTY_SWIFT_NAME(none),
    /// Only a link the program declared with `OSC 8`.
    Declared UBIQUITTY_SWIFT_NAME(declared),
    /// A declared link, or text that reads like one.
    Any UBIQUITTY_SWIFT_NAME(any),
};

/// How much of the link scan this pointer event is worth running.
///
/// The whole scan flattens a soft-wrap group and sweeps it for a URL and then
/// a path. A modifier-less hover happens on every pointer move over any text,
/// and almost none of that text is a link, so running the sweep there spends a
/// row walk per move to answer no. Reading the hovered cell's `OSC 8` id
/// answers the common case in one read.
///
/// Which events may widen past that is the reveal policy, not a cost question:
///
/// - Command reaches through and asks for a guess by name, so it takes `Any` —
///   that is what a modifier-held hover is for.
/// - With no local link interaction at all, nothing is scanned.
/// - `links_override_reporting` under a reporting application widens to `Any`
///   as well, because the terminal is keeping a hover the application would
///   otherwise have received, and the auto-detected reveal is exactly the set
///   `plainClickOpenableUnderReporting` then admits.
/// - Otherwise a bare hover reveals only what the program declared, so
///   `Declared` is both the cheap answer and the whole answer.
///
/// `osc8_click_opens` off means a bare hover reveals nothing at all, so it
/// scans nothing; Command still reaches through, which is the one way to reach
/// a link when the setting is off.
///
/// - Parameters:
///   - state: The reporting state and modifiers for this event.
///   - isOSC8ClickOpensEnabled: `[selection] osc8_click_opens`.
/// - Returns: The scope to scan with, or `None` to scan nothing.
[[nodiscard]] inline LinkProbe linkProbe(const PointerReportingState& state, bool isOSC8ClickOpensEnabled)
{
    if (!allowsLocalLinkInteraction(state))
        return LinkProbe::None;
    if (state.isLinkModifierArmed)
        return LinkProbe::Any;
    if (!isOSC8ClickOpensEnabled)
        return LinkProbe::None;
    if (state.isMouseReporting && state.isLinkOverrideEnabled)
        return LinkProbe::Any;
    return LinkProbe::Declared;
}

} // namespace Ubiquitty::Input
