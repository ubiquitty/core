// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The portable tmux control-mode command vocabulary + recovery sentinels, paired
// with the shared reply scanners (Protocol/Tmux/Tmux.h). Control mode itself is
// specified at https://github.com/tmux/tmux/wiki/Control-Mode. Every -CC frontend must
// emit the same query strings and recognise the same reply sentinels or the shared
// scanners silently stop matching, so producer format strings are co-located with
// their consumer scanners: change a `#{…}` field here and the round-trip test breaks
// in one place. Pure string builders; depends only on the C++ standard library.

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Tmux/PaneID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

/// Identifies one session within a particular tmux server lifetime.
struct SessionReference {
    std::uint64_t serverProcessID = 0;
    std::uint64_t serverStartTime = 0;
    std::uint64_t sessionID = 0;
    std::uint64_t sessionCreated = 0;
    std::string socketPath;

    bool operator==(const SessionReference&) const = default;
};

constexpr std::string_view sessionReferenceReplyPrefix = "UBSESSION ";

/// Queries the attached session's identity and server socket.
///
/// - Returns: A control-mode command whose reply carries the identity marker.
[[nodiscard]] std::string sessionReferenceQuery();

/// Decodes a complete identity reply, rejecting incomplete or unbounded fields.
///
/// - Parameter line: One control-mode response line.
/// - Returns: The validated identity, or no value for an invalid reply.
[[nodiscard]] std::optional<SessionReference> parseSessionReference(std::string_view line);

/// Builds an attach that refuses a different server or an ended session.
///
/// The server checks the identity inside the same command queue as the attach.
/// The returned arguments require direct execution, without shell splitting.
///
/// - Parameters:
///   - tmuxPath: The executable to launch on the reference's host.
///   - reference: The identity captured from that host's live control connection.
/// - Returns: The argument vector, or an empty vector for an invalid reference.
[[nodiscard]] std::vector<std::string> restoreSessionArguments(std::string_view tmuxPath, const SessionReference& reference);

// ---- Reply sentinels (self-identifying reply prefixes) -------------------
// `display-message -p` replies arrive on the same channel as `list-panes` pane
// records. Each query stamps a sentinel prefix so the router tells them apart.
// Keep the trailing space: the router matches the prefix INCLUDING it, so a window
// named e.g. "UBMODESX" can't be mistaken for a mode reply.
constexpr std::string_view paneModeReplyPrefix = "UBMODES "; // "UBMODES %<pane> <bits>"
constexpr std::string_view captureBeginPrefix = "UBCAPBEGIN "; // "UBCAPBEGIN <pane> <cx> <cy> <retry> <scope> <form>"
constexpr std::string_view serverVersionReplyPrefix = "UBVERSION "; // "UBVERSION <#{version}>"
// Clipboard pull: this marker announces that the NEXT command-response block is a
// tmux paste buffer's raw content (marker-then-content, like captureBeginPrefix).
// tmux never pushes OSC 52 to a control client, so this PULL is the only path from
// a tmux-side copy to the host clipboard.
constexpr std::string_view clipboardPullPrefix = "UBCLIPBEGIN "; // "UBCLIPBEGIN %<pane>"

// ---- The pane-enumeration query format -----------------------------------
// Format for `list-panes -F <format>`. Window ids (@N) and pane ids (%M) are
// SEPARATE namespaces, so each window's real pane id must be learned. `window_layout`
// is one space-free token carrying the window's FULL pane tree, repeated per pane row
// so an attach builds a pre-split window's panes at once (tmux does not re-emit
// %layout-change for existing windows on attach). `window_name` is LAST so its
// (possibly space-bearing) value is the rest of the line. One line per pane:
//   `@<win> %<pane> <w> <h> <active> <layout> <session> <window_name>`
// Pairs exactly with parsePaneRecord (Protocol/Tmux/Tmux.h).
constexpr std::string_view paneListFormat = "\"#{window_id} #{pane_id} #{pane_width} #{pane_height} #{pane_active} "
                                            "#{window_layout} #{session_name} #{window_name}\"";

// `list-panes -s -F <format>` for the ATTACHED session only (one tab per
// window). `-s` = current session; `-a` would drag in every other session on
// the server, spawning a (often blank) tab per unattached session.
[[nodiscard]] std::string listPanesForSession();

// `list-panes -t @<windowID> -F <format>` for one window (e.g. on %window-add,
// where the notification carries no pane id).
[[nodiscard]] std::string listPanesForWindow(int windowID);

// ---- Client-geometry sync (refresh-client -C) ----------------------------
//
// `refresh-client -C <columns>x<rows>` tells the server this control client's
// grid so it sizes the session and emits redraws. Sent once on entering control
// mode and again on every window resize; without it tmux sits silent after
// attach and the user sees a frozen terminal.
[[nodiscard]] std::string refreshClientSize(int columns, int rows);

// ---- Per-pane recovery queries -------------------------------------------
//
// tmux %output carries only NEW app output, never the DECSETs (mouse reporting,
// application cursor keys) or the existing screen the app drew before we
// attached. These queries recover both, each stamped with its sentinel so the
// reply router can route it.

// Query tmux's authoritative per-pane terminal-mode flags. The reply is a
// `UBMODES %<pane> <bits>` line whose 6-bit field decodePaneModeReport turns
// back into the DECSET escapes. `#{?flag,1,0}` forces exactly one digit per
// field so an unknown var can't shift the parsed bit positions. The field order
// is FIXED — it must match decodePaneModeReport's bit assignment:
//   1000 1002 1003 1006 1005 DECCKM
[[nodiscard]] std::string paneModeQuery(PaneID paneID);

// The two commands that pull a pane's tmux paste buffer into the host clipboard:
// a display-message marker (whose reply is `clipboardPullPrefix %<pane>`, marking
// the next response block) followed by `show-buffer`, whose response body is the
// buffer content. Issued together, in order, on a %pane-mode-changed for the pane.
[[nodiscard]] std::string clipboardPullMarker(PaneID paneID);
[[nodiscard]] std::string clipboardPullShowBuffer();

// Upper bound on a pulled paste buffer: the server-controlled show-buffer body is
// untrusted and unbounded, so without a cap a large remote buffer OOMs the frontend.
constexpr std::size_t maxClipboardPullBytes = 1u << 20; // 1 MiB

// Decide whether a freshly-pulled paste buffer should be written to the system
// clipboard: only when writing is permitted, the buffer is non-empty, and it
// differs from the last value this bridge synced (so re-entering copy-mode or an
// unrelated %pane-mode-changed doesn't rewrite an unchanged buffer, and our own
// write can't re-trigger). Returns true and leaves `text` as the value to write;
// false means skip. `lastSynced` is updated by the caller on a true result.
[[nodiscard]] bool shouldSyncClipboardPull(std::string_view pulled, std::string_view lastSynced, bool writeAllowed);

// Parse the pane id from a clipboard-pull marker reply body — the text after
// clipboardPullPrefix, i.e. "%<pane>". Returns the pane id, or -1 if the '%' or
// digits are absent/malformed. Bounded: an over-long digit run from the untrusted
// server returns -1 rather than overflowing.
[[nodiscard]] int parseClipboardPullMarkerPane(std::string_view afterPrefix);

// Decide whether an untrusted -CC clipboard pull should be written, given how the
// pull's pane attribution resolved. The pull is triggered by a SERVER-controlled
// marker, so a pane that fails to resolve is an anomaly (or a hostile server
// dodging attribution) and FAILS CLOSED.
//   Unresolved -> false (fail closed)
//   Resolved   -> whatever the capability gate answered
// The caller passes what `Ubiquitty::effectAllowed` answered for ClipboardWrite.
// It took the policy's presence and its verdict separately until the two spelled
// the gate's own unset-policy rule in a second place that could disagree with it.

/// Whether the server-supplied pane id resolved to a pane this client owns.
enum class PaneAttribution : std::uint8_t {
    Unresolved,
    Resolved
};

/// What the capability gate answered for the write.
enum class ClipboardWriteGate : std::uint8_t {
    Refused,
    Allowed
};

[[nodiscard]] bool clipboardPullShouldWrite(PaneAttribution attribution, ClipboardWriteGate gate);

/// Which capture-pane command a UBCAPBEGIN marker announces: the plain current-screen
/// capture (in-place repaint) or a ranged full-history capture (fed via newline-driven
/// scrolling). See StateRecovery.h's CaptureOutcome.
enum class CaptureScope : uint8_t {
    CurrentScreen,
    FullHistory
};

/// How a capture-pane command prints a pane's rows.
enum class CaptureForm : uint8_t {
    /// Each row's text with its trailing blanks trimmed. Every tmux prints it,
    /// and it says nothing of which rows tmux wrapped.
    Text,
    /// Each row behind its line flags (`-F`), `W` among them on a row tmux
    /// wrapped into the next, and every cell up to the row's last used one
    /// (`-N -T`): a wrapped row keeps its trailing spaces, and the padding a
    /// wide character left at the right edge stays out, as tmux's own join of
    /// the rows has them. tmux 3.7 and later: 3.7 added `-F`, and `-N` and `-T`
    /// are older.
    FlaggedRows,
};

/// Chooses the capture form a server prints.
///
/// - Parameter version: The server's `#{version}`, as `3.7`, `3.7c` or `next-3.9`.
/// - Returns: ``CaptureForm/FlaggedRows`` from tmux 3.7; ``CaptureForm/Text``
///   before it and for a version it does not read.
[[nodiscard]] CaptureForm captureFormForVersion(std::string_view version);

/// Asks the server for its version, whose reply chooses the capture form.
///
/// - Returns: A `display-message` whose reply carries ``serverVersionReplyPrefix``.
[[nodiscard]] std::string serverVersionQuery();

// Mark the start of a screen-content capture: a `display-message` line carrying
// the pane id, the cursor (restored after the captured content lands), the
// retry count, which capture-pane command follows (scope) and how it prints
// its rows (form). The IMMEDIATELY following `capture-pane` response block is
// the content; the capture router keys off the protocol block boundaries, so
// the marker needs no end token and no nonce.
[[nodiscard]] std::string captureBeginMarker(PaneID paneID, int retryCount, CaptureScope scope = CaptureScope::CurrentScreen, CaptureForm form = CaptureForm::Text);

// `capture-pane -t %<pane> -e -p` — dump the pane's existing screen with SGR
// escapes (`-e`) to stdout (`-p`), in `form`. The rows feed recoveryFeed.
[[nodiscard]] std::string capturePane(PaneID paneID, CaptureForm form = CaptureForm::Text);

// `capture-pane -t %<pane> -e -p -S - -E -` — dump tmux's FULL retained history plus
// the current screen in one ranged capture, in `form`. The `-S -`/`-E -` bounds are literal
// dashes, not a `#{...}` format substitution: embedding a live `#{history_size}` in a
// -CC command parse-errors and kills the session (or silently desyncs -E). tmux
// resolves both bounds server-side, so this can't go stale. The rows feed recoveryFeed.
[[nodiscard]] std::string capturePaneWithHistory(PaneID paneID, CaptureForm form = CaptureForm::Text);

// ---- Pane / window / session mutation commands ---------------------------
// Control-mode verbs the frontend emits to mutate server state. Each returns the
// bare command without a trailing newline — tmux dispatches on '\n', so the transport
// appends it. Targets address the tmux *pane* namespace (`%M`), distinct from window
// ids (`@N`). Split direction follows tmux: `-h` side-by-side (column), `-v` stacked (row).
enum class SplitDirection {
    SideBySide, // tmux `-h`: the new pane sits beside the source (a column split)
    Stacked // tmux `-v`: the new pane sits below the source (a row split)
};

// `split-window -h|-v -t %<pane>` — ask the server to split `paneID`. The
// resulting %layout-change builds the new pane locally; the frontend never
// constructs it directly off this command.
[[nodiscard]] std::string splitWindow(PaneID paneID, SplitDirection direction);

/// Moves a live pane beside another without restarting its process.
[[nodiscard]] std::string movePane(PaneID source, PaneID target, Panes::Orientation, Panes::InsertSide, int percentage = 50, Panes::InsertTarget = Panes::InsertTarget::Pane);
/// Moves a complete split tree, retaining pane identities and split proportions.
[[nodiscard]] std::string moveLayout(const Panes::Tree&, PaneID target, Panes::Orientation, Panes::InsertSide, Panes::InsertTarget = Panes::InsertTarget::Pane);
/// Extracts a pane into a new tmux window.
[[nodiscard]] std::string breakPane(PaneID);

// `kill-pane -t %<pane>` — close `paneID` server-side. Local teardown is
// deferred to the %layout-change (split survivor) / %window-close (last pane)
// echo the server sends back.
[[nodiscard]] std::string killPane(PaneID paneID);

/// Closes exactly one tmux window and its panes, retaining sibling windows.
[[nodiscard]] std::string killWindow(int windowID);

// `select-pane -t %<pane>` — move tmux's server-side active pane to `paneID`,
// so the server's own active-pane state follows the user's focus here.
[[nodiscard]] std::string selectPane(PaneID paneID);

// `resize-pane -Z -t %<pane>` — toggle the server-side zoom on `paneID`'s
// window. The %layout-change echo re-mirrors the result, so a local zoom
// never diverges from the server's.
[[nodiscard]] std::string zoomPane(PaneID paneID);

// `new-window` — open a fresh tmux window (a new native tab). No target: the
// server creates it in the attached session.
[[nodiscard]] std::string newWindow();

// `rename-window -t %<pane> "<name>"` — rename the window CONTAINING
// `paneID` (tmux resolves a pane target to its window for window commands;
// verified live). The name rides tmux's command-line grammar, so it is
// double-quoted with `\` and `"` escaped.
[[nodiscard]] std::string renameWindow(PaneID paneID, std::string_view name);

// `detach-client` — detach this control client; the session keeps running on
// the server for a later re-attach.
[[nodiscard]] std::string detachClient();

// `kill-session` — destroy the attached session and all its windows.
[[nodiscard]] std::string killSession();

// `refresh-client -A %<pane>:continue` — resume a pane tmux paused (%pause).
// This client does no flow control of its own, so every %pause is answered
// with this immediately; without it the pane's output stays frozen for good.
[[nodiscard]] std::string continuePane(PaneID paneID);

// `set-option aggressive-resize on` (applies to the attached session). Without it,
// tmux's default (`off`) reflows only the CURRENT window on `refresh-client -C`, so a
// background tab's pane keeps its old wrap until the user switches to it. Sent once on
// attach.
[[nodiscard]] std::string enableAggressiveResize();

// ---- Pane-mode report decode (pure) --------------------------------------
// Decode the 6-bit mode string from a `UBMODES …` reply BODY (bytes after
// paneModeReplyPrefix) into the DECSET escapes that re-arm the modes missed at
// attach. Body is `%<pane> <bits>`; the fixed-width bit field, in order:
//   bit 0 → 1000h  standard mouse tracking
//   bit 1 → 1002h  button-event tracking
//   bit 2 → 1003h  any-event tracking
//   bit 3 → 1006h  SGR extended coordinates
//   bit 4 → 1005h  UTF-8 extended coordinates
//   bit 5 → 1h     DECCKM application cursor keys
// Each bit is a single '1'/'0' (the query uses `#{?flag,1,0}`). Returns "" when no
// bit is set or on a malformed prefix. Re-applying a DECSET is idempotent.
[[nodiscard]] std::string decodePaneModeReport(std::string_view body, PaneID& paneID);

} // namespace Ubiquitty::Protocol::Tmux
