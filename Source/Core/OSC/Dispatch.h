// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/OSC/ClipboardPolicy.h"
#include "Core/OSC/SequenceGate.h"
#include "Core/Terminal.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// OSC (Operating System Command) dispatcher. The VT100 parser
// accumulates the OSC payload into Terminal::osc.data and calls
// Ubiquitty::OSC::dispatch once the terminator (BEL or ST) is seen.

namespace Ubiquitty::OSC {

/// Runs the accumulated OSC sequence, once its terminator has been seen.
///
/// - Parameters:
///   - t: The terminal whose `osc.data` holds the payload.
void dispatch(Ubiquitty::Terminal& t);

/// Applies a fixed-length Linux or iTerm2 palette command through the color gates.
///
/// - Parameters:
///   - t: The terminal receiving the command.
///   - payload: `R`, or `P` followed by a selector and six hexadecimal digits.
void dispatchFixedPalette(Ubiquitty::Terminal& t, std::string_view payload);

/// Puts all 256 indexed colours back to the baseline, as OSC 104 with no
/// number does.
///
/// A soft reset (DECSTR) does this as well as RIS: xterm's ReallyReset resets
/// the palette for both (charproc.c:14405). The default foreground,
/// background and cursor colours are not indexed and stay as they are.
///
/// - Parameters:
///   - t: The terminal whose palette is reset.
void resetIndexedColors(Ubiquitty::Terminal& t);

// OSC 52 — Manipulate Selection Data (clipboard). Policy is per-Terminal state and nothing else: Core's
// handler reads only the Terminal fields, so two Terms hold independent policies with no shared mutable
// state, and the value a terminal runs under is the one its session's configuration seeded.

/// Installs an OSC 52 read/write policy on one terminal.
///
/// Idempotent, so a config reload may call it on every live terminal. The
/// master toggle `isOSC52Allowed` is not part of the policy — no config key
/// names it — so it keeps the value the terminal holds.
///
/// - Parameters:
///   - t: The terminal to seed.
///   - policy: The pair, as the session's configuration describes it.
void applyOSC52Policy(Ubiquitty::Terminal& t, const OSC52Policy& policy);

/// Whether a programmatic clipboard write is allowed for this terminal.
///
/// Reads the terminal's own fields rather than the live config, because it is
/// consulted on the ingest thread.
///
/// - Parameters:
///   - t: The terminal the sequence arrived on.
/// - Returns: True when the terminal's master toggle is on and its write policy is not `Deny`.
[[nodiscard]] bool osc52WriteAllowed(Ubiquitty::Terminal& t);

/// Encodes the MIME names captured by a user paste, without clipboard data.
///
/// - Parameters:
///   - types: Space-separated MIME names captured by the paste gesture.
///   - buffer: The clipboard location selected by the gesture.
/// - Returns: One complete OSC 5522 notification, or EBUSY if the names exceed 4096 bytes.
[[nodiscard]] std::string clipboardPasteNotification(std::string_view types, ClipboardBuffer buffer);

/// What the frontend decided about a clipboard read it was asked to approve.
enum class ClipboardReadDecision : uint8_t {
    /// Read the clipboard and frame the reply.
    Approved,
    /// Reply with empty data, which is what a refusal looks like on the wire.
    Refused,
};

/// Answers a clipboard read the frontend was asked to approve.
///
/// Called back on the ingest thread under the model lock. A no-op when no
/// clipboard client is wired.
///
/// - Parameters:
///   - t: The terminal that issued the query.
///   - selectionSet: Echoes the query's selection-set specifier.
///   - decision: What the frontend chose.
void completeClipboardReadQuery(Ubiquitty::Terminal& t, std::string_view selectionSet, ClipboardReadDecision decision);

// OSC 4/10/11/12/110/111/112 (palette, default fg/bg, cursor color) mutate Terminal::palette directly, no
// hook; the frontend reads palette (resyncing on a WindowClient::invalidate() nudge).

// OSC 6 — current document/file URL (file analog of OSC 7). ESC ] 6 ; <file-url> ST (empty payload clears).
// Stored verbatim on Terminal::currentFileURL with no shellClient hook; consumers read it off the Terminal.

// OSC 7 — working-directory notification. ESC ] 7 ; file://<host>/<absolute-path> ST.
// The percent-decoded absolute path is routed through ShellClient::setCurrentDirectory.
// Once the directory and remote-host capabilities allow it, the authority becomes
// the reported host; a local authority clears that report.

// OSC 17/117/19/119 — selection background/foreground color (17/19 set, 117/119 reset). Same wire spec as
// OSC 4/10/11/12, 16-bit channels; stored on Terminal for `?` queries and routed to the Application layer.
// The setter/resetter handlers are installed via the process-wide PAL::Context; a NULL handler is stored-and-no-op.

// OSC 9/99/777 — desktop notification (9 iTerm, 99 kitty, 777 urxvt). Parsed to title+body and routed
// through ShellClient::postNotification; not surfaced with no client. OSC 9 doubles as ConEmu's progress
// channel (9;4;...), working-directory report (9;9;...) and prompt marker (9;12).
// Other payloads are notification text.

// The unknown-code drop path routes to DiagnosticsClient::onUnknownOSCCode, so a wired client sees the wire
// code verbatim (no silent clamp above 255).

// Per-OSC / per-DCS / per-APC sequence allowlisting (security). Each Terminal owns its own
// ``SequenceGate`` (Core/OSC/SequenceGate.h holds the policy and the DCS ids);
// the setters below install a whole list on one Terminal, and the checks read
// that Terminal's gate. Lists are bounded; an overflow is reported by the parsers.

/// Installs the OSC allowlist on one terminal. The call carries the whole set, never an addition to it.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - codes: The Ps codes to allow. Null, or a count of zero, is the empty set.
///   - count: How many.
/// - Returns: Whether the WHOLE list was installed. A list over the cap is
///   installed to the cap and the overflow reported; the answer is how a caller
///   learns the policy it asked for is not the policy in force.
[[nodiscard]] bool setOSCAllowList(Ubiquitty::Terminal& terminal, const int* codes, int count);

/// Installs the OSC denylist on one terminal. The call carries the whole set, and deny beats allow.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - codes: The Ps codes to block.
///   - count: How many.
/// - Returns: Whether the WHOLE list was installed. False means the dimension
///   is closed instead: codes past the cap are denied along with everything
///   else, and a caller that treats a deny list as a security control should
///   still refuse to proceed on a policy it did not ask for.
[[nodiscard]] bool setOSCDenyList(Ubiquitty::Terminal& terminal, const int* codes, int count);

/// Installs the DCS allowlist on one terminal. The call carries the whole set, never an addition to it.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - kinds: The sub-protocols to allow.
///   - count: How many.
/// - Returns: Whether the whole list was installed; see ``setOSCAllowList``.
[[nodiscard]] bool setDCSAllowList(Ubiquitty::Terminal& terminal, const DCSKind* kinds, int count);

/// Installs the DCS denylist on one terminal. The call carries the whole set, and deny beats allow.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - kinds: The sub-protocols to block.
///   - count: How many.
/// - Returns: Whether the whole list was installed. False means the DCS
///   dimension is closed instead of truncated open; see ``setOSCDenyList``.
[[nodiscard]] bool setDCSDenyList(Ubiquitty::Terminal& terminal, const DCSKind* kinds, int count);

/// Installs the APC allowlist on one terminal. The call carries the whole set, never an addition to it.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - introducers: The first bytes of the APCs to allow, such as `G` for kitty graphics.
/// - Returns: Whether the whole list was installed; see ``setOSCAllowList``.
[[nodiscard]] bool setAPCAllowList(Ubiquitty::Terminal& terminal, std::span<const uint8_t> introducers);

/// Installs the APC denylist on one terminal. The call carries the whole set, and deny beats allow.
///
/// - Parameters:
///   - terminal: The terminal the list gates.
///   - introducers: The first bytes of the APCs to block.
/// - Returns: Whether the whole list was installed. False means the APC
///   dimension is closed instead of truncated open; see ``setOSCDenyList``.
[[nodiscard]] bool setAPCDenyList(Ubiquitty::Terminal& terminal, std::span<const uint8_t> introducers);

/// Whether an OSC code passes one terminal's allow/deny gate.
///
/// - Parameters:
///   - terminal: The terminal whose gate decides.
///   - ps: The wire code.
/// - Returns: True to run the sequence. A blocked one is still parsed and
///   consumed cleanly, with no hook and no response.
[[nodiscard]] bool oscAllowed(const Ubiquitty::Terminal& terminal, int ps);

/// Whether a DCS sub-protocol passes one terminal's allow/deny gate.
///
/// - Parameters:
///   - terminal: The terminal whose gate decides.
///   - kind: The sub-protocol.
/// - Returns: True to run the sequence.
[[nodiscard]] bool dcsAllowed(const Ubiquitty::Terminal& terminal, DCSKind kind);

/// Whether an APC passes one terminal's allow/deny gate.
///
/// - Parameters:
///   - terminal: The terminal whose gate decides.
///   - introducer: The APC's first byte, or 0 for an empty APC, which no list names.
/// - Returns: True to run the sequence. A blocked one is still parsed and
///   consumed cleanly, with no hook and no response.
[[nodiscard]] bool apcAllowed(const Ubiquitty::Terminal& terminal, uint8_t introducer);

/// Parses a comma- or space-separated decimal code list.
///
/// - Parameters:
///   - spec: The list as configured. An unparseable token is skipped rather
///     than failing the whole list.
///   - out: Receives the codes.
///   - cap: How many `out` holds.
///   - droppedOut: Receives how many parsed codes did not fit, when non-null.
///     A caller folding this into a deny list must close the dimension on any
///     nonzero count rather than running truncated open.
/// - Returns: How many were written.
int parseOSCList(std::string_view spec, int* out, int cap, int* droppedOut = nullptr);
/// Parses a case-insensitive DCS name list, such as `sixel decudk decrqss`.
///
/// - Parameters:
///   - spec: The list as configured.
///   - out: Receives the kinds.
///   - cap: How many `out` holds.
///   - droppedOut: Receives how many recognised names did not fit, when
///     non-null; see ``parseOSCList``.
/// - Returns: How many were written.
int parseDCSList(std::string_view spec, DCSKind* out, int cap, int* droppedOut = nullptr);
/// Parses a comma- or space-separated list of APC introducers, such as `G`.
///
/// Each token is one byte that ``isAPCIntroducer`` accepts, matched
/// case-sensitively as the wire carries it; any other token is skipped.
///
/// - Parameters:
///   - spec: The list as configured.
///   - out: Receives the introducers; its size is the cap.
///   - droppedOut: Receives how many valid introducers did not fit, when
///     non-null; see ``parseOSCList``.
/// - Returns: How many were written.
int parseAPCList(std::string_view spec, std::span<uint8_t> out, int* droppedOut = nullptr);

} // namespace Ubiquitty::OSC
