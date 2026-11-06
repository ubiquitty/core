// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Operating System Command handlers.
//
// The largest surface in the parser and the least uniform: titles, colours, the
// clipboard, shell integration marks, hyperlinks, notifications, file transfer,
// and the vendor extensions that arrived alongside them.
//
// What OSC shares is the framing rather than the semantics — a numeric code, a
// semicolon, a payload of arbitrary length, and a terminator that may be BEL or
// ST. Most of what can go wrong is in that framing: a payload longer than a
// buffer, a missing terminator, a code that no handler claims, and a policy that
// has to decide whether a payload is allowed to act at all.

#include "Core/BuildVersion.h"
#include "Core/Codec/Base64.h"
#include "Core/Keys/Keymap.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/OSC/TabStatus.h"
#include "Core/Screen/Addressing.h"
#include "Core/TerminalShared.h"
#include "Core/VT/Parser.h"
#include "Harness.h"
#include "VT100Fixture.h"

#include <array>
#include <clocale>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {
struct AttentionClient : Ubiquitty::WindowClient {
    int calls = 0;
    Ubiquitty::AttentionRequest last = Ubiquitty::AttentionRequest::Once;
    void requestAttention(Ubiquitty::AttentionRequest kind) override
    {
        calls++;
        last = kind;
    }
};
}

/// OSC 1337 RequestAttention=yes/once/no → WindowClient::requestAttention.
namespace {
struct StealFocusClient : Ubiquitty::WindowClient {
    int calls = 0;
    void stealFocus() override { calls++; }
};
}

/// OSC 1337 StealFocus → WindowClient::stealFocus. Exact-match only: a
/// "StealFocus=…" or other prefix must not fire it. Both terminators.
namespace {
struct ColorPresetClient : Ubiquitty::WindowClient {
    int calls = 0;
    std::string last;
    void selectColorPreset(std::string_view name) override
    {
        calls++;
        last.assign(name.data(), name.size());
    }
};
}

/// OSC 22 (kitty pointer shapes): set / reset / push / pop → WindowClient::
/// setMousePointerShape, plus the '?' support query reply.
namespace {
struct FontClient : Ubiquitty::WindowClient {
    int calls = 0;
    std::string family;
    double sizePoints = 0.0;
    double sizeDelta = 0.0;
    int sizeResets = 0;
    [[nodiscard]] bool setFont(std::string_view fam, double size, double delta) override
    {
        calls++;
        family.assign(fam.data(), fam.size());
        sizePoints = size;
        sizeDelta = delta;
        return true;
    }
    [[nodiscard]] bool resetFontSize() override
    {
        sizeResets++;
        return true;
    }
};
}

/// other tab stops are untouched.
/// TBC Ps=3 (CSI 3 g) clears every horizontal tab stop.
/// DECST8C (CSI ? 5 W) restores the every-8-columns default tab grid.
/// DECSTR (soft reset, CSI ! p) must not touch tab stops — they are not in the
/// DEC STD 070 soft-reset list (only RIS / DECST8C / TBC affect them). A program's
/// custom tab map must survive a soft reset.
/// X10 / normal mouse encoding must refuse to emit a garbled report
/// when column+33 or row+33 would not fit in a single byte. SGR mode is
/// unaffected.
/// UTF-8 mouse mode (1005) must clamp a coordinate to the 2-byte UTF-8 range: a
/// value past U+07FF would otherwise emit a 3-byte lead with a single continuation
/// byte — malformed UTF-8 on the wire.
/// Mouse_EncodeReport defensive guards: nullptr term / nullptr out / non-
/// positive maxLength / negative row|column / negative button / undersized
/// X10 buffer all return 0 without scribbling. Drives the early-exit
/// arms that the higher-level mouse tests skip.
/// LNM (ANSI mode 20). When set, LF/VT/FF act like NEL: cursor
/// returns to column 0 before advancing the row.
/// DECSCNM (DEC private mode 5). Set the flag, expose it via
/// Ubiquitty::isReverseVideo, and ensure toggling marks every row dirty so
/// the renderer repaints with swapped foreground/background.
/// G2/G3 designators + SS2/SS3 single shifts. Designate the DEC
/// special graphics set as G2, then use SS2 (ESC N) to single-shift
/// the very next character. The character resolves through G2;
/// subsequent characters fall back to G0.
/// The UK national set maps '#' to '£' but otherwise mirrors US-ASCII.
/// Accept the designator on any of G0..G3.
/// NRCS — French (ESC ( R). The position 0x23 maps to £ (MacRoman 0xA3),
/// matching the UK set on '#'; ASCII positions outside the substitution
/// table pass through unchanged.
/// NRCS — German (ESC ( K). [\]^ → Ä Ö Ü (no caret substitution in this
/// set — ^ stays ASCII), then ~ → ß.
/// NRCS — Italian (ESC ( Y). [ → ° and ] → é, plus the lowercase
/// substitution `→ù and full row.
/// NRCS — Norwegian/Danish (E and 6 designators both select).
/// NRCS — Swedish (H and 7) and Finnish (C and 5) share the same table.
/// NRCS — Spanish (Z). [ → ¡, ] → ¿, \ → Ñ.
/// NRCS — Swiss (=). The Swiss set is unusual in replacing position
/// 0x5F (underscore) with 'è'.
/// NRCS — French Canadian (Q and 9).
/// Round trip: designate French on G0, write substituted text, switch
/// back to US-ASCII, write more text — must use the unsubstituted ASCII
/// glyphs for the second run. This guards against the substitution
/// table "sticking" past the de-designation.
/// Emoji width must come from Ubiquitty::Unicode::Width::forCodePoint so that the single-source
/// range table (Source/Core/Unicode/Width.cpp) governs whether U+2B50 occupies one cell or
/// two. Without this, a hard-coded width=2 whenever the codePoint
/// hit the emoji atlas would drift from the width table.
/// OSC codes must survive parser intact for values > 255 — Wezterm uses
/// OSC 7704, kitty 9001, etc. A decimalAppend(..., 255) clamps large codes
/// to 255 → table miss → silent drop.
/// DiagnosticsClient::onUnknownOSCCode lets us assert the parser saw the wire
/// code verbatim. Uses codes with no registered handler so the unknown-code
/// path is exercised (OSC 1337 dispatches to a handler).
namespace {
struct UnknownOSCCodeDiagClient : Ubiquitty::DiagnosticsClient {
    int code = -1;
    char payload[64] = { 0 };
    void onUnknownOSCCode(int observed, std::string_view tail) override
    {
        code = observed;
        int n = (int)tail.size();
        if (n > (int)sizeof(payload) - 1)
            n = (int)sizeof(payload) - 1;
        for (int i = 0; i < n; i++)
            payload[i] = tail[i];
        payload[n] = 0;
    }
};
}

/// OSC 104 — reset indexed palette entries to their startup defaults
/// (the xterm undo for OSC 4). Targeted reset leaves siblings alone; a
/// bare OSC 104 resets all 256 slots.
/// OSC payload byte 0x9C must not terminate the sequence — it's a
/// valid UTF-8 continuation byte, and many real-world OSC payloads
/// include codePoints whose UTF-8 encoding contains 0x9C (any 3-byte
/// codePoint of the form U+xy1C..U+xy5F, e.g. U+2713 ✓ → E2 9C 93,
/// U+2733 ✳ → E2 9C B3). If we honoured 0x9C as C1 ST we'd abort the
/// OSC mid-payload, leak the remaining bytes into the cell grid, and
/// leave the window title set to a partial / empty string.
/// DEL (0x7F) must be ignored in Ground per VT/VT220 spec
/// (legacy paper-tape erasure marker). The Ground default arm would
/// print it as a literal 0x7F.
/// Same invariant in VT52 ground (entered via DECRST 2).
/// Synchronized output (DECSET 2026) — host bytes still feed the cell
/// grid while the mode is active; the PAL is the one that holds off
/// painting. The watchdog API (Touch / ElapsedTicks / Expire) lets the
/// PAL force-clear the mode if an app forgets to disable it.
/// Fullscreen content mode (Ubiquitty private DECSET 7402) — edge-to-edge, no
/// padding + letterbox for a fullscreen Kitty-graphics stream. The Core holds the
/// modes bit + drives the frontend seam (setEdgeToEdge); DECRQM round-trips it, and
/// a soft reset (DECSTR) must not clear it — matching 2048/7401, a soft reset can't
/// yank the user out of full-screen. The window-inset zeroing / letterbox / image
/// contain-fit live in the frontend, not exercised here.
/// Relative pointer-lock / mouselook (Ubiquitty private DECSET 7403). The Core holds
/// the modes bit + reports it via DECRQM; the macOS frontend (observing the mode
/// snapshot) hides + dissociates the OS cursor and reports relative deltas. SAFETY:
/// unlike 7401/7402, a soft reset (DECSTR) must clear 7403 — a stranded hidden /
/// dissociated OS cursor is a system-wide hazard, so the reset releases the lock (and
/// the frontend's snapshot observer then runs the unhide/reassociate/warp-back).
/// OSC 7402 [ ; key=value ]* — the DECSET 7402 fullscreen-content display controller
/// (fit contain/stretch/native, Stretch ar=W:H, letterbox fill=RRGGBB|pass). Each OSC
/// fully re-specifies the state: named keys override, unnamed take defaults, so a bare
/// OSC 7402 resets to Contain + black-occlude. Malformed values fail the whole command
/// (fail-safe). Preserved across DECSTR (mirrors 7402), cleared by RIS. The null test
/// client makes the frontend seam a no-op, so assert on the Terminal struct.
/// XTVERSION (CSI > q) — applications probe for a terminal name+version on
/// startup. Respond with the DCS > | <name> ST envelope; failing to reply
/// produces a small stall while the app waits for the response.
/// OSC 9;4 — ConEmu progress reports. The state+value pair is parsed
/// into Terminal.progressState / Terminal.progressValue and progressGeneration
/// bumps on every push so the PAL can detect changes. The OSC must
/// not bleed into the title or cell stream.
namespace {
struct ProgressInvalidateClient : Ubiquitty::WindowClient {
    int invalidates = 0;
    std::vector<std::pair<uint8_t, int>> reports;
    void reportProgress(uint8_t state, int value) override { reports.emplace_back(state, value); }
    void invalidate() override { invalidates++; }
};
}

/// OSC 21337 — iTerm2's session status (Docs/tab-status.md).
/// `indicator=#RRGGBB` sets a dot color (and bumps tabStatusGeneration + nudges
/// the frontend); a bare key or empty value clears it. `status` is kept as its label; status-color and
/// detail are accepted and ignored.
/// The sequence must not bleed into the title or cell stream.
/// OSC 133 — FinalTerm prompt-mark protocol. Per-row marker bytes are
/// set on the cursor row by the sub-command letter (A/B/C/D), travel
/// with the row across Ubiquitty::Screen::scrollUp, and ride into scrollback when the row
/// is captured. Alternate-screen sequences are accepted but do not perturb
/// primary-screen markers.
/// OSC 133 A→C deferral: PROMPT_START lands at A, but the visible CMD_EXECUTED
/// mark (gutter indicator + Cmd-L target) is promoted onto the prompt row only
/// once its command runs (C). A bare prompt that never reaches C stays
/// CMD_EXECUTED-free, so findPromptMarkBefore(CMD_EXECUTED) skips it and keeps the
/// previous executed command — where a PROMPT_START search would stop short.
/// OSC 133;D;<n> with n>0 flags the just-finished command's prompt row with
/// CMD_FAILED (renderer tints it red); n==0 (or absent) leaves it unmarked. The
/// flag lands on the prompt row even after the output scrolled it into history.
/// The failed flag reaches a prompt that scrolled into scrollback before its
/// command ended (addRowMark writes the captured Ubiquitty::Screen::ScrollLine, not just live).
// ----------------------------------------------------------------------
// OSC prompt-mark matrix — the three shell-integration allocations
// (133 semantic prompt, 633 shell integration, 1337 SetMark). Every
// assertion reads the mark through the public rowMark accessor over the
// absolute row space: history rows occupy [0, scrollbackCount), live rows
// follow at scrollbackCount + r.

/// Cursor-row absolute index for the live screen.
static int markCursorAbsoluteRow(Ubiquitty::Terminal& t) { return Ubiquitty::Screen::scrollbackCount(&t) + t.screens.active->cursorY; }

/// 133 A/B/C/D each set their own bit on the cursor row; BEL terminator.
/// 133 A/B/C/D with the ESC \ (ST) terminator behave identically.
/// 633 A/B/C/D map to the same four bits as 133; both terminators.
/// 1337 SetMark sets PROMPT_START on the cursor row; both terminators.
/// Two and three distinct marks on the same row accumulate (bitwise or),
/// independent of the order they arrive in.
/// D with and without an exit code both set CMD_END; the exit code is
/// parsed and ignored for the mark (133 and 633).
/// A with trailing key=value options still sets PROMPT_START; the trailing
/// parameters are ignored for the mark (FinalTerm / VS Code semantic prompt).
/// Unknown subcommands are a no-op: they set nothing and leave any prior
/// marks on the row intact (133;Z, 633;Q).
/// 633 E (pre-exec command line) and 633 P (property, e.g. CurrentDirectory) set no
/// mark on the cursor row.
/// 1337 non-SetMark subcommands (CurrentDir=, RemoteHost=,
/// ShellIntegrationVersion=, File=) set no mark.
/// Alternate-screen suppression with a surviving primary mark: a PROMPT_START on
/// a primary row, then alternate screen + 133;A sets nothing on the alternate row, and
/// the primary mark reappears after returning to the primary screen.
/// Alternate-screen suppression also holds for the legacy 47 / 1047 alternate-screen
/// modes (not just 1049).
/// Plain newline scroll: a mark rides its row up the live screen, then
/// into scrollback. A fresh blank row scrolled in at the bottom carries
/// no mark. Addressed via the absolute index before and after.
/// A wrapped row that also carries an OSC 133 mark must keep both pieces of
/// metadata when it scrolls into history. The wrapped logical line stays
/// stitchable after capture, and the prompt mark moves with the history row.
/// DECSTBM scroll region: LF at the region bottom scrolls the region up;
/// marks inside the region shift up with their rows, the row scrolled out
/// of the region top loses the mark, and the blanked-in bottom row has
/// none. Region rows outside the margins are untouched.
/// DECSTBM reverse scroll (RI / ESC M at region top) scrolls the region
/// down; a mark on an interior region row shifts down one row, and the
/// blanked-in top region row has no mark.
/// IL (insert line) pushes the cursor row and everything below it down;
/// marks below the insertion point shift down, the inserted blank rows
/// carry no mark.
/// DL (delete line) pulls rows below the cursor up; marks below the
/// deletion shift up, and the blanked-in rows at the bottom carry none.
/// Erase-in-display (ESC[2J, the core of the `clear` command): a blanked
/// row hosts no prompt, so its OSC 133 mark must clear with the cells —
/// otherwise the renderer keeps drawing the prompt-mark edge brackets
/// ([ … ]) on a now-empty row (the stray brackets a `clear` left behind).
/// Erase-in-line (ESC[2K): observed contract — the row's mark survives an
/// in-line erase. Encodes current behavior; does not change production.
/// ESC[3J (erase scrollback) drops history rows and their marks; live
/// marks are untouched. After it, rowMark returns 0 for every former
/// history row.
/// Full clearScrollback drops every history mark; rowMark returns 0
/// for all former history rows while the live screen is untouched.
/// Width reflow: a marked logical line keeps its mark on its first row
/// after the row rewraps (narrow then wide). Mirrors the scrollback
/// reflow tests but asserts the mark via rowMark over absolute rows.
/// Height resize: shrink to push a marked row into scrollback, then grow
/// to pull it back. The mark tracks its row across the absolute-index
/// shift and the Ubiquitty::Screen::ScrollLine record carries it while in history.
/// Scrollback round-trip: a mark set on a live row that ages fully into
/// history is retrievable via rowMark at its history absolute index,
/// and the stored Ubiquitty::Screen::ScrollLine record carries the bit.
/// rowMark resolves absolute rows across the live screen + scrollback
/// ring; findPromptMarkBefore walks back from the live top to the most
/// recent OSC 133 A row.
/// clearScrollbackBefore drops history rows with absolute index < absoluteRow.
/// Rows >= absoluteRow shift to new absolute index = old - absoluteRow; their
/// prompt marks ride along, observed via rowMark. The live screen is
/// untouched.
/// dropRowsBefore with absoluteRow inside the live screen shifts the rows
/// above absoluteRow off the top, lands the targeted absolute row at row 0,
/// drops all scrollback, and moves the cursor up by the same shift.
/// dropRowsBefore with absoluteRow inside scrollback collapses to the
/// scrollback-only path: live screen + cursor untouched, scrollback
/// trims to start at absoluteRow.
/// clearScrollbackBefore is a no-op on empty / zero / negative inputs
/// and clamps oversize requests to the current count.
/// OSC 52 — Manipulate Selection Data (clipboard).
/// Write path: base64-encoded payload is decoded and handed to the
/// Terminal's ClipboardClient. Every legal selector (c/p/q/s/0-7, empty→s0)
/// names its own distinct buffer; an unknown selector invalidates the
/// request. (testOSC52Multibuffer proves the buffers are distinct.)
struct CaptureClipboard : Ubiquitty::ClipboardClient {
    // Most-recent write across all buffers (for the single-buffer assertions).
    char captured[256];
    int capturedLength = -1;
    int setCalls = 0;
    Ubiquitty::ClipboardBuffer lastBuffer = Ubiquitty::ClipboardBuffer::Clipboard;
    // Per-buffer stores, so the multi-buffer tests can prove `p` != `c`.
    char store[(int)Ubiquitty::ClipboardBuffer::Count][256];
    int storeLength[(int)Ubiquitty::ClipboardBuffer::Count];
    const char* getValue = nullptr; // fixture for a single-buffer readClipboard
    int getLength = 0;
    // Per-buffer read fixtures (override getValue when set for that buffer).
    const char* getStore[(int)Ubiquitty::ClipboardBuffer::Count] = {};
    int getStoreLength[(int)Ubiquitty::ClipboardBuffer::Count] = {};
    Ubiquitty::ClipboardBuffer lastReadBuffer = Ubiquitty::ClipboardBuffer::Clipboard;
    CaptureClipboard()
    {
        for (int i = 0; i < (int)Ubiquitty::ClipboardBuffer::Count; ++i)
            storeLength[i] = -1;
    }
    void writeClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<const char> data) override
    {
        setCalls++;
        lastBuffer = buffer;
        capturedLength = (int)data.size();
        if (!data.empty() && data.size() <= sizeof(captured))
            memcpy(captured, data.data(), data.size());
        int b = (int)buffer;
        storeLength[b] = (int)data.size();
        if (!data.empty() && data.size() <= sizeof(store[b]))
            memcpy(store[b], data.data(), data.size());
    }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<char> outputBuffer) override
    {
        lastReadBuffer = buffer;
        int b = (int)buffer;
        const char* value = getStore[b] ? getStore[b] : getValue;
        int valueLength = getStore[b] ? getStoreLength[b] : getLength;
        if (!value)
            return -1;
        int n = valueLength;
        if (n > (int)outputBuffer.size())
            n = (int)outputBuffer.size();
        memcpy(outputBuffer.data(), value, (size_t)n);
        return n;
    }
};

/// OSC 52 selector (Pc) routing. Every legal selector — c (clipboard),
/// p (primary), q (secondary), s (select), 0-7 (cut buffers) — names its own
/// DISTINCT buffer; a multi-selector list like "pc" writes each named buffer
/// once. An empty Pc defaults to s0 (the select buffer). A selector containing
/// any illegal byte makes the whole request invalid and is ignored — never
/// silently routed.
/// OSC 1337 ; Copy=:<base64> — iTerm2's clipboard write. Decodes through the
/// same ClipboardClient sink as OSC 52 and obeys the same write policy.
/// A clipboard of any size must still produce a valid, non-empty OSC 52 read
/// reply. Base64 grows the payload by a third, so a reply buffer sized for the
/// raw clipboard overflows on a large one — and a silently dropped read hangs
/// the requesting app forever. The reply buffers are sized so a full read always
/// frames, and any residual overflow falls back to an empty reply (never
/// nothing).
/// Per-Ubiquitty::Terminal clipboard client: two Terms, each with its own
/// ClipboardClient. OSC 52 on each Ubiquitty::Terminal routes to the right sink.
/// OSC 52 REAL multi-buffer routing: each selector names its OWN distinct
/// buffer (c/p/q/s/0-7), not a fold onto the single clipboard. A write to one
/// buffer is independent of the others; a query reads the buffer of the first
/// selector; a multi-selector SET writes each named buffer.
/// SECURITY: every buffer's REMOTE access stays behind the existing gates.
/// allowOSC52=false, write Deny, and read Deny each block all buffers — a
/// remote must not read or write Primary / cut buffers unguarded.
// OSC 52 chunked (multi-part) clipboard write.
//
// Convention: a SET whose base64 data ends in the continuation marker '!'
// accumulates into a per-Ubiquitty::Terminal pending buffer; the first SET to the same
// selection whose data lacks the marker terminates the transfer (appended +
// decoded + applied once). Bounded by maxOSC52ChunkSize; overflow / interleave
// abort cleanly. Single-shot SET (no marker, no transfer in flight) is the
// unchanged fast path.

/// Captures the full decoded payload (up to a generous cap) so a multi-KB
/// chunked transfer can be byte-compared against its single-shot form.
struct BigClipboard : Ubiquitty::ClipboardClient {
    static constexpr int capacity = 64 * 1024;
    char* captured = (char*)malloc(capacity);
    int capturedLength = -1;
    int setCalls = 0;
    ~BigClipboard() override { free(captured); }
    void writeClipboard(Ubiquitty::ClipboardBuffer /*buffer*/, std::span<const char> data) override
    {
        setCalls++;
        capturedLength = (int)data.size();
        if (!data.empty() && (int)data.size() <= capacity)
            memcpy(captured, data.data(), data.size());
    }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer /*buffer*/, std::span<char>) override { return -1; }
};

/// Overflow bound: chunks accumulating past maxOSC52ChunkSize abort cleanly —
/// no apply, no crash, pending state reset.
/// Interleave / abort: a chunked transfer to selection "c" interrupted by a
/// SET to a different selection "p" abandons the first transfer and the new
/// one applies on its own; a read query also aborts a pending transfer.
// OSC 52 READ security policy ([security] clipboard_read).
//
//  Allow  → read + inline reply (legacy).
//  Deny   → no read, empty reply (well-formed, leaks nothing).
//  Prompt → fires requestClipboardRead; no reply until the frontend calls
//           completeClipboardReadQuery. A deny-from-hook replies empty; an
//           allow-from-hook performs the read + reply.
// The SET (write) path is unaffected by the read policy.

/// Clipboard client that records the Prompt up-hook firing + the echoed
/// selection-set, and never reads inline (the read only happens on an
/// explicit completeClipboardReadQuery).
struct PromptClipboard : Ubiquitty::ClipboardClient {
    int requestCalls = 0;
    char requestSet[16] = { 0 };
    int requestSetLength = 0;
    int setCalls = 0;
    const char* getValue = "Hi";
    int getLength = 2;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char>) override { setCalls++; }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        int n = getLength;
        if (n > (int)outputBuffer.size())
            n = (int)outputBuffer.size();
        if (n > 0)
            memcpy(outputBuffer.data(), getValue, (size_t)n);
        return n;
    }
    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view set, Ubiquitty::ClipboardReadConfirmation /*confirmation*/) override
    {
        requestCalls++;
        int length = (int)set.size();
        requestSetLength = length;
        if (length > 0 && length < (int)sizeof(requestSet))
            memcpy(requestSet, set.data(), (size_t)length);
        requestSet[(length >= 0 && length < (int)sizeof(requestSet)) ? length : 0] = 0;
        return true;
    }
};

/// Async clipboard-read client: requiresAsynchronousClipboardRead() == true models a
/// frontend (macOS) that cannot read the pasteboard on Core's ingest thread, so
/// even policy Allow must route through requestClipboardRead → the frontend
/// stages the bytes (provideClipboardReadBytes) on its UI thread → readClipboard
/// serves only the staged buffer. This is the PRODUCTION macOS path; the inline
/// branch the other fixtures take never exercises it.
struct AsyncClipboard : Ubiquitty::ClipboardClient {
    int requestCalls = 0;
    char requestSet[16] = { 0 };
    int requestSetLength = 0;
    char staged[256] = { 0 };
    int stagedLength = 0;
    bool hasStagedReply = false;
    [[nodiscard]] bool requiresAsynchronousClipboardRead() override { return true; }
    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view set, Ubiquitty::ClipboardReadConfirmation /*confirmation*/) override
    {
        requestCalls++;
        requestSetLength = (int)set.size();
        if (requestSetLength > 0 && requestSetLength < (int)sizeof(requestSet))
            memcpy(requestSet, set.data(), set.size());
        requestSet[(requestSetLength >= 0 && requestSetLength < (int)sizeof(requestSet)) ? requestSetLength : 0] = 0;
        return true;
    }
    void provideClipboardReadBytes(Ubiquitty::ClipboardBuffer, std::span<const char> bytes) override
    {
        stagedLength = (int)bytes.size();
        if (stagedLength > (int)sizeof(staged))
            stagedLength = (int)sizeof(staged);
        if (stagedLength > 0)
            memcpy(staged, bytes.data(), (size_t)stagedLength);
        hasStagedReply = true;
    }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        if (!hasStagedReply)
            return -1; // nothing staged → no reply
        int n = stagedLength;
        if (n > (int)outputBuffer.size())
            n = (int)outputBuffer.size();
        if (n > 0)
            memcpy(outputBuffer.data(), staged, (size_t)n);
        hasStagedReply = false; // one-shot
        return n;
    }
};

// ---- OSC 8 — hyperlinks -----------------------------------------

/// sizeof(Cell) must stay 32: ch + Pen + a single inline combining slot +
/// the length / OSC 8 link id / cluster id, all packed with no padding (the
/// per-Ubiquitty::Terminal memory budget is sized around this figure). The cluster id is
/// also memcmp-safe: two cells holding the same overflow cluster compare
/// byte-exact (the id rides the whole-cell copy), and a cluster cell does
/// not compare equal to a bare base.
/// OSC 8 ; ; URI ST opens a link; emitted cells carry its id. OSC 8 ; ; ST
/// (empty URI) closes it; subsequent cells carry id 0 (no link). The id
/// resolves back to the URI via Ubiquitty::Attributes::Hyperlinks::uri.
/// The id=<token> parameter dedups: two OSC 8 runs sharing an id reuse one
/// table entry (one id), letting a wrapped link span rows cheaply. A
/// different id mints a fresh entry. Absent an id, an exact-URI match
/// also dedups.
/// The link table is a fixed-cap ring: interning more than linkTableCapacity
/// distinct URIs recycles the oldest entry. After overflow, the oldest
/// id no longer resolves while the newest still does.
// ---- OSC 8 linkIntern MRU edges (the one-slot repeat-open cache) ----------

/// A repeat-open of the same target returns the same id (the MRU hit path),
/// including across an intervening close.
/// An over-long target (key >= maxLinkKeyLength, URI >= maxLinkURILength)
/// never matches its truncated ring copy, so every open mints a FRESH id —
/// the MRU must not change that (its fit check bypasses the cache).
/// RING RECYCLE invalidation — the MRU edge that matters: after the ring
/// evicts the slot the MRU pointed at, a re-open of that target must mint a
/// fresh, correctly-resolving id, never the stale recycled one (a stale hit
/// would stamp cells with an id whose slot now holds a DIFFERENT URL).
/// A key and a URI with byte-identical values are DIFFERENT dedup targets:
/// the MRU (and the ring) must never cross-match them.
/// ---- OSC 17 / 117 / 19 / 119 — selection background / foreground color -----------
static uint16_t selectionBackgroundRed, selectionBackgroundGreen, selectionBackgroundBlue;

static int selectionBackgroundSetCalls = 0;

static int selectionBackgroundResetCalls = 0;

static void testOSCSelbgSetHook(Ubiquitty::Terminal* /*t*/, uint16_t r, uint16_t g, uint16_t b)
{
    selectionBackgroundSetCalls++;
    selectionBackgroundRed = r;
    selectionBackgroundGreen = g;
    selectionBackgroundBlue = b;
}

static void testOSCSelbgResetHook(Ubiquitty::Terminal* /*t*/) { selectionBackgroundResetCalls++; }

static uint16_t selectionForegroundRed, selectionForegroundGreen, selectionForegroundBlue;

static int selectionForegroundSetCalls = 0;

static int selectionForegroundResetCalls = 0;

static void testOSCSelfgSetHook(Ubiquitty::Terminal* /*t*/, uint16_t r, uint16_t g, uint16_t b)
{
    selectionForegroundSetCalls++;
    selectionForegroundRed = r;
    selectionForegroundGreen = g;
    selectionForegroundBlue = b;
}

static void testOSCSelfgResetHook(Ubiquitty::Terminal* /*t*/) { selectionForegroundResetCalls++; }

/// OSC 13/14/15/16/18 — the remaining xterm dynamic colors (mouse pointer
/// fg/bg, Tektronix fg/bg, Tektronix cursor). set (rgb:/#/hsl:), query (?),
/// reset (113/114/115/116/118). The Tek slots (15/16/18) drive the Tek 4014
/// renderer; the pointer slots (13/14) are store/query/reset only. We assert
/// on the Ubiquitty::Terminal storage + the `?` reply (the renderer wiring is covered by the
/// macOS frontend).
/// OSC 5 / 105 — xterm special colors (Pc: 0=bold, 1=underline, 2=blink,
/// 3=reverse, 4=italic), stored in Terminal::palette.special. OSC 5 sets/queries;
/// OSC 105 leaves them as they are. OSC 4 index 256+Pc is the documented
/// alias for the same slots.
/// ---- OSC 9 / 99 / 777 — desktop notification ---------------------
/// DesktopNotification: a denying policy gates OSC 9/99/777 notifications
/// (no postNotification reaches the client); an allowing policy lets them fire.
/// A hard reset cannot remove the capability gate from bytes that remain in
/// the same input batch.
/// ---- OSC 633 — VS Code shell integration -------------------------
/// ---- OSC 1337 — iTerm2 SetMark / CurrentDir / RemoteHost ---------
/// OSC 52 base64 decode error paths: non-alphabet character + non-pad
/// byte after '=' + trailing leftover-bits truncation. The decoder
/// returns -1 for all three; the write path silently drops the
/// payload (no writeClipboard call).
struct CountingClipboard : Ubiquitty::ClipboardClient {
    int setCalls = 0;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char>) override { setCalls++; }
};

/// oversized input is bounded to the buffer and stays NUL-terminated.
/// Niche VT400/500 reports: locator-identify DSR, DECRQDE, DECRQUPSS, DECCIR.
/// Mode 1001 hilite tracking: recognized via DECRQM, and the host's
/// XTHIMOUSE region reply is consumed non-blocking (where xterm hangs).
/// The hang-safe handshake: press arms a bounded wall-clock wait; an XTHIMOUSE
/// reply makes the region renderable; with no reply the wait aborts cleanly at
/// the deadline (where xterm would block forever). Deterministic — the clock is
/// an explicit tick value, never wall-clock.
/// Hilite button-RELEASE report wire format (CSI t short / CSI T long), classic
/// legacy value+0x20 byte encoding.
/// ---- Batch 5: DECSLRM (left/right margins) ----------------------- *
///
/// DECSLRM (CSI Pl ; Pr s) sets left/right margins; it requires
/// DECLRMM (DECSET 69) to be active first, otherwise CSI s falls
/// through to SCOSC (save cursor). The margins constrain horizontal
/// cursor motions + autowrap.

/// ICH/DCH respect the DECSLRM right margin (G2): an insert/delete inside
/// the margins shifts only within [cursor, right margin]; cells past the right
/// margin are untouched, not pushed across it.
/// CR under DECLRMM moves to the LEFT MARGIN when the cursor is at/right of it
/// (xterm CarriageReturn), only to column 0 when it starts left of the margin.
/// NEL (ESC E = CR + LF) honors the left margin for its CR component, like CR.
/// A bare CSI s under DECLRMM is DECSLRM (not SCOSC): with no params it resets the
/// left/right margins to full width and homes the cursor.
/// DECCOLM (a column-mode switch) resets the horizontal margins to full width and
/// clears DECLRMM, since the new page width invalidates any DECSLRM region.
/// ---- Batch 5b: DECSLRM scroll/tab/BS interaction -----------------

/// A DECSLRM region scroll must not silently drop or mis-attach a row's
/// per-row metadata (OSC 133 mark, DECDWL/DECDHL line attribute). A
/// bespoke per-cell loop that never touches RowMeta would leave a marked /
/// double-width row's metadata glued to the wrong row when content scrolls
/// under left/right margins.
///
/// Two sub-scenarios:
///  - PARTIAL-WIDTH rect (margins narrower than the row): scrollRectUp/Down keep
///    the per-cell loop, but metadata can't split along a column boundary, so by
///    policy it stays pinned to its row index (a horizontal-margined scroll
///    shifts cells, not whole lines) and rows OUTSIDE the vertical region keep
///    their metadata untouched.
///  - FULL-WIDTH DECLRMM scroll (margins span [0, columns-1]): the scroll guard
///    routes it through CellGrid::scrollRegion — the same RowMeta rotation the
///    scrollRect full-width branch reuses — so a DWL+marked row inside the
///    region RIDES its cells to the new row and revealed rows reset to NORMAL.
/// ---- New CSI handlers (post-DECSLRM batch) -----------------------

/// DECRQM must report real state (1=set, 2=reset) for the private modes
/// added to decrqmDecState: ?12 (cursor blink), ?40 (allow 80<->132), and
/// ?2031 (color-scheme notify).
/// DECRQCRA — Request Checksum of Rectangular Area. Feed "AB" into the top
/// two cells, query a 1x2 rect (top=1,left=1,bottom=1,right=2), and expect
/// DCS Pid ! ~ <4-hex> ST where the hex is the negated 16-bit sum of the
/// cell character values. 'A'+'B' = 0x83, negated 16-bit = 0xFF7D.
/// OSC 52 set then read round-trip through a per-Ubiquitty::Terminal fake clipboard
/// client. The set decodes base64 into the fake buffer; the read
/// base64-encodes the buffer back and frames it as OSC 52 ; c ; <b64> ST.
/// shouldReadBack gates the read direction (false → no reply).
struct RoundtripClipboard : Ubiquitty::ClipboardClient {
    char buffer[256];
    int length = 0;
    bool shouldReadBack = true;
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> data) override
    {
        int n = (int)data.size();
        if (n > (int)sizeof(buffer))
            n = (int)sizeof(buffer);
        memcpy(buffer, data.data(), (size_t)n);
        length = n;
    }
    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        if (!shouldReadBack)
            return -1;
        int n = length;
        if (n > (int)outputBuffer.size())
            n = (int)outputBuffer.size();
        memcpy(outputBuffer.data(), buffer, (size_t)n);
        return n;
    }
};

TEST(testLegacyLinuxStandardOSCUnaffected)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Linux);
    feed(t, "\033]0;hello\007"); // set window title
    feed(t, "X");
    CHECK(cell(t, 0, 0)->codePoint == 'X'); // OSC didn't leak to the grid
}

TEST(testLinuxPaletteCommandsSetResetAndPreserveFollowingText)
{
    for (const auto emulation : { Ubiquitty::VT::Emulation::VT, Ubiquitty::VT::Emulation::Linux }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setEmulation(&t, emulation);
        const auto epoch = t.paletteEpoch;
        feed(t, "\033]P1aB");
        feed(t, "12fFAB");
        CHECK(t.palette.indexed[1].red == 0xABAB);
        CHECK(t.palette.indexed[1].green == 0x1212);
        CHECK(t.palette.indexed[1].blue == 0xFFFF);
        CHECK(t.paletteEpoch > epoch);
        CHECK(!t.bellPending);
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(cell(t, 0, 1)->codePoint == 'B');
        feed(t, "\033]P1000000\033\\");
        CHECK(t.palette.indexed[1].red == 0);
        CHECK(!t.bellPending);
        feed(t, "\033]RCD");
        CHECK(t.palette.indexed[1] == t.paletteBaseline.indexed[1]);
        CHECK(cell(t, 0, 2)->codePoint == 'C');
        CHECK(cell(t, 0, 3)->codePoint == 'D');
        CHECK(!t.bellPending);
        feed(t, "\033]P1123456\007E");
        CHECK(t.bellPending);
        CHECK(cell(t, 0, 4)->codePoint == 'E');
    }
}

TEST(testLinuxPaletteAcceptsEachIndexedSelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int index = 0; index < 16; ++index) {
        const char selector = "0123456789abcdef"[index];
        feed(t, std::string("\033]P") + selector + "123456");
        CHECK(t.palette.indexed[index].red == 0x1212);
        CHECK(t.palette.indexed[index].green == 0x3434);
        CHECK(t.palette.indexed[index].blue == 0x5656);
    }
    feed(t, "\033]R");
    for (int index = 0; index < 16; ++index)
        CHECK(t.palette.indexed[index] == t.paletteBaseline.indexed[index]);
}

TEST(testLinuxPaletteRejectsMalformedColors)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const auto original = t.palette.indexed[1];
    for (const char* input : { "\033]P1z00000A", "\033]P10z0000A", "\033]P100000zA", "\033]P1ff\007A" }) {
        feed(t, input);
        CHECK(t.palette.indexed[1] == original);
        CHECK(!t.bellPending);
    }
    for (int column = 0; column < 4; ++column)
        CHECK(cell(t, 0, column)->codePoint == 'A');
}

TEST(testFixedPaletteRespectsColorPolicies)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const auto original = t.palette.indexed[1];
    t.allowColorOperations = false;
    feed(t, "\033]P1123456A");
    CHECK(t.palette.indexed[1] == original);
    t.allowColorOperations = true;
    const int denied[] = { 4, 10, 104 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, denied, 3));
    feed(t, "\033]P1123456\033]Pg123456\007B");
    CHECK(t.palette.indexed[1] == original);
    CHECK(t.palette.defaultForeground == t.paletteBaseline.defaultForeground);
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, nullptr, 0));
    feed(t, "\033]P1123456");
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    feed(t, "\033]P1000000\033]RC");
    CHECK(t.palette.indexed[1].red == 0x1212);
    CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::SetColor);
    CHECK(t.screens.active->cursorX == 3);
    t.capabilityPolicy = nullptr;
}

TEST(testITermPaletteSelectorsUseTerminatedColorCommands)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (const char* terminator : { "\007", "\033\\" }) {
        for (const char selector : std::string_view("ghijkl")) {
            const std::string command = std::string("\033]P") + selector + "123456" + terminator;
            for (const char byte : command)
                feed(t, std::string_view(&byte, 1));
        }
        CHECK(t.palette.defaultForeground.red == 0x1212);
        CHECK(t.palette.defaultBackground.green == 0x3434);
        CHECK(t.palette.special[0].blue == 0x5656);
        CHECK(t.palette.specialSet[0]);
        CHECK(t.selectionBackgroundSet && t.selectionBackgroundRed == 0x1212);
        CHECK(t.selectionForegroundSet && t.selectionForegroundGreen == 0x3434);
        CHECK(t.palette.cursor.blue == 0x5656);
        CHECK(!t.bellPending);
        CHECK(t.screens.active->cursorX == 0);
    }
}

TEST(testConEmuWorkingDirectoryUsesMetadataAndCapabilityGate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;
    feed(t, "\033]9;9;\"C:\\Users\\My Name\"\007");
    CHECK(t.currentDirectory == "C:\\Users\\My Name");
    CHECK(std::string_view(shell.currentDirectory) == t.currentDirectory);
    feed(t, "\033]9;9;/srv/work;one\033\\");
    CHECK(t.currentDirectory == "/srv/work;one");
    feed(t, "\033]9;9;/tmp/a\x01"
            "b\007");
    CHECK(t.currentDirectory == "/tmp/ab");
    CHECK(std::string_view(shell.currentDirectory) == t.currentDirectory);
    CHECK(shell.cwdCalls == 3);
    CHECK(shell.notifyCalls == 0);
    feed(t, "\033]9;9\007");
    CHECK(shell.cwdCalls == 3);
    CHECK(shell.notifyCalls == 0);
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    feed(t, "\033]9;9;\"C:\\denied\"\007");
    CHECK(t.currentDirectory == "/tmp/ab");
    CHECK(shell.cwdCalls == 3);
    CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::SetWorkingDirectory);
    t.capabilityPolicy = nullptr;
    feed(t, "\033]9;9;\"\"\033\\");
    CHECK(t.currentDirectory.empty());
    CHECK(shell.cwdCalls == 4);
    t.shellClient = nullptr;
}

TEST(testOSC52SetReadRoundtrip)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);
    RoundtripClipboard clip;
    t.clipboardClient = &clip;
    // Inline read+reply is the Allow behavior; the default is Prompt.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // Set: "Hi!" => base64 "SGkh".
    feed(t, "\033]52;c;SGkh\033\\");
    CHECK(clip.length == 3);
    CHECK(memcmp(clip.buffer, "Hi!", 3) == 0);
    (void)drain(t, outputBuffer);

    // Read: reply should base64-encode the fake clipboard back.
    feed(t, "\033]52;c;?\033\\");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033]52;c;SGkh\033\\") == 0);

    t.clipboardClient = nullptr;
}

TEST(testOSC52ReadGatedNoHook)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);
    RoundtripClipboard clip;
    clip.shouldReadBack = false;
    t.clipboardClient = &clip;
    // Pin the read-refusal under Allow (where the read is actually attempted);
    // the Prompt default would defer before ever reading.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    feed(t, "\033]52;c;?\033\\");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
    t.clipboardClient = nullptr;
}

TEST(testOSCTitleDoesNotDirtyCells)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int r;
    Ubiquitty::hardReset(t);
    feed(t, "hello");
    // Sync the shadow so subsequent paints reflect the OSC's effect
    // in isolation.
    drainPaintCycle(t, nullptr);
    feed(t, "\033]0;Ubiquitty title\007");
    CHECK(t.titleChanged);
    CHECK(!t.grid->isFullRedrawRequested());
    // OSC title parsing must not flag any cell as needing paint.
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    for (r = 0; r < t.rows; r++)
        CHECK(!tr.isRowYielded[r]);
}

TEST(testOSCTitlePreservesUTF8Source)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // U+2733 (✳) → grStar8 in MacRoman, which drops it. UTF-8: E2 9C B3.
    feed(t, "\033]0;\xe2\x9c\xb3 Foo\007");
    // Core stores the title verbatim as UTF-8; the MacRoman fold (which drops
    // U+2733) is a frontend concern, tested there.
    CHECK(t.titleUTF8 == "\xe2\x9c\xb3 Foo");
}

TEST(testDttermLetterFormsSetTheTitleAndIconName)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Everything after the letter is the label, a ';' included.
    feed(t, "\033]lHello\033\\");
    CHECK(t.titleUTF8 == "Hello");
    feed(t, "\033]l;two;parts\007");
    CHECK(t.titleUTF8 == ";two;parts");

    // The icon name alone, the title left as it was.
    feed(t, "\033]LIcon\007");
    CHECK(t.iconNameUTF8 == "Icon");
    CHECK(t.titleUTF8 == ";two;parts");

    // An icon file names a picture no window shows: nothing changes, and nothing reaches the screen.
    feed(t, "\033]Inone\007A");
    CHECK(t.titleUTF8 == ";two;parts");
    CHECK(t.iconNameUTF8 == "Icon");
    CHECK(cell(t, 0, 0)->codePoint == 'A');

    // An empty dtterm title is empty.
    feed(t, "\033]l\007");
    CHECK(t.titleUTF8.empty());

    // The forms are title operations: refused where titles are.
    t.allowTitleOperations = false;
    feed(t, "\033]lRefused\007");
    CHECK(t.titleUTF8.empty());
}

TEST(testOSCTitleUTF8Fallback3byte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]0;\xE2 X\007");
    CHECK(!t.titleUTF8.empty() && static_cast<uint8_t>(t.titleUTF8[0]) == 0xE2);
    CHECK(t.titleUTF8[1] == ' ');
    CHECK(t.titleUTF8[2] == 'X');
    CHECK(t.titleUTF8.size() == 3);
}

TEST(testOSCTitleUTF8Fallback4byte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]0;\xF1 Y\007");
    CHECK(!t.titleUTF8.empty() && static_cast<uint8_t>(t.titleUTF8[0]) == 0xF1);
    CHECK(t.titleUTF8[1] == ' ');
    CHECK(t.titleUTF8[2] == 'Y');
}

TEST(testOSCTitleTruncatesAtUTF8Boundary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string title(Ubiquitty::Terminal::maxTitleLength - 1, 'a');
    title += "Ã©";
    std::string wire = "]0;" + title + "";
    feed(t, wire.c_str());
    CHECK(t.titleUTF8.size() == Ubiquitty::Terminal::maxTitleLength - 1);
    CHECK(t.titleUTF8.find("Ã") == std::string::npos);
}

TEST(testMaxOSCNonimageBytes)
{
    const std::string big(Ubiquitty::VT::maxOSCNonImageSize + 4096, 'A'); // past the low ceiling

    // OSC 2 (title), no ST: clipped at the non-image ceiling; the buffer never
    // grows toward the 64 MiB image cap.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]2;");
        feed(t, big);
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
        CHECK(t.parser.osc.capacity <= 2 * Ubiquitty::VT::maxOSCNonImageSize); // bounded, not 64 MiB
    }
    // OSC 8 (hyperlink) is a non-image code too — same clip.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]8;;");
        feed(t, big);
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
    }
    // OSC 52 (clipboard): the same payload keeps growing past the low ceiling —
    // its large base64 is legitimate (the 64 MiB / maxOSC52ChunkSize caps still apply).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]52;c;");
        feed(t, big);
        CHECK(t.parser.osc.length > Ubiquitty::VT::maxOSCNonImageSize); // Not clipped at the low ceiling
    }
    // OSC 1337 (iTerm2 inline image): keeps the full ceiling as well.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;File=");
        feed(t, big);
        CHECK(t.parser.osc.length > Ubiquitty::VT::maxOSCNonImageSize);
    }
}

TEST(testOSC7CwdCapture)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.currentDirectory.empty());

    // OSC 7 file:// URL: %20 decoded to a space. `localhost` is a spelling of
    // this machine, so it names no host.
    feed(t, "\033]7;file://localhost/home/user/my%20directory\007");
    CHECK(t.currentDirectory == "/home/user/my directory");
    CHECK(t.reportedHost.empty());

    // OSC 1337 CurrentDir= sets a plain (non-URL) path.
    feed(t, "\033]1337;CurrentDir=/var/tmp\007");
    CHECK(t.currentDirectory == "/var/tmp");
}

// The authority of the same report names the machine the shell is on, and it
// is the only thing on the wire that can: this machine's process tree holds one
// ssh however many the reader has opened, so nothing local sees the second hop.
TEST(testOSC7CapturesTheHostItWasReportedFrom)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.reportedHost.empty());

    feed(t, "\033]7;file://example-host/home/user\007");
    CHECK(t.reportedHost == "example-host");
    CHECK(t.currentDirectory == "/home/user");

    // Both spellings of here clear it rather than recording a name: a session
    // that came home must not keep claiming the host it left.
    feed(t, "\033]7;file://localhost/tmp\007");
    CHECK(t.reportedHost.empty());
    feed(t, "\033]7;file://example-host/home/user\007");
    CHECK(t.reportedHost == "example-host");
    feed(t, "\033]7;file:///tmp\007");
    CHECK(t.reportedHost.empty());

    // Case folds, since a shell may report the name however it is configured.
    feed(t, "\033]7;file://LOCALHOST/tmp\007");
    CHECK(t.reportedHost.empty());

    const std::string localHost = Ubiquitty::PAL::systemHostName();
    if (!localHost.empty()) {
        const std::string report = "\033]7;file://" + localHost + "/tmp\007";
        feed(t, "\033]7;file://example-host/home/user\007");
        feed(t, report.c_str());
        CHECK(t.reportedHost.empty());
    }
}

TEST(testOSC6FileURLCapture)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.currentFileURL.empty());

    // OSC 6 file:// URL: stored verbatim (host segment kept, BEL-terminated).
    feed(t, "\033]6;file:///Users/x/doc.txt\007");
    CHECK(t.currentFileURL == "file:///Users/x/doc.txt");

    // Host segment is retained (distinguishes local vs SSH'd files); ST-terminated.
    feed(t, "\033]6;file://my.host/etc/passwd\033\\");
    CHECK(t.currentFileURL == "file://my.host/etc/passwd");

    // Leading whitespace is trimmed, matching OSC 7.
    feed(t, "\033]6;   file:///tmp/a\007");
    CHECK(t.currentFileURL == "file:///tmp/a");

    // Empty payload clears the URL (editors emit this for an unnamed buffer).
    feed(t, "\033]6;\007");
    CHECK(t.currentFileURL.empty());

    // Oversized URL is bounded to the buffer (NUL-terminated, no overflow).
    {
        std::string big = "\033]6;file:///";
        big.append(4096, 'a');
        big += '\007';
        feed(t, big.c_str());
        std::string expected = "file:///";
        expected.append(Ubiquitty::Terminal::maxCurrentFileURLLength - expected.size(), 'a');
        CHECK(t.currentFileURL == expected);
    }
}

// A bell and a notification are edges that reach whoever is attached when they
// happen. The terminal latches each until someone is recorded as having seen
// it, with or without a client to deliver to.
TEST(aBellAndANotificationStayLatchedUntilSeen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const uint32_t start = t.attentionGeneration;
    CHECK(!Ubiquitty::unseenAttention(&t).hasBell && !Ubiquitty::unseenAttention(&t).hasNotification);
    CHECK_MSG(!Ubiquitty::markAttentionSeen(t), "nothing latched, nothing to clear");
    CHECK(t.attentionGeneration == start);

    feed(t, "\007");
    CHECK(Ubiquitty::unseenAttention(&t).hasBell);
    CHECK(!Ubiquitty::unseenAttention(&t).hasNotification);
    CHECK(t.attentionGeneration == start + 1);
    feed(t, "\007\007");
    CHECK_MSG(t.attentionGeneration == start + 1, "a bell on a latched bell wakes nobody");
    // The frontend's own drain of the pending bell is not a person seeing it.
    (void)Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(100));
    CHECK(Ubiquitty::unseenAttention(&t).hasBell);

    // No shell client is attached: the notification is still latched.
    feed(t, "\033]9;Build finished\007");
    CHECK(Ubiquitty::unseenAttention(&t).hasNotification);
    CHECK(Ubiquitty::unseenAttention(&t).notificationTitle.empty());
    CHECK(Ubiquitty::unseenAttention(&t).notificationBody == "Build finished");
    feed(t, "\033]777;notify;Deploy;went out\007");
    CHECK_MSG(Ubiquitty::unseenAttention(&t).notificationTitle == "Deploy" && Ubiquitty::unseenAttention(&t).notificationBody == "went out",
        "the newest notification is the one kept");
    feed(t, "\033]99;p=body;kitty says\033\\");
    CHECK(Ubiquitty::unseenAttention(&t).notificationBody == "kitty says");
    // OSC 9;4 is progress, not a notification.
    const uint32_t beforeProgress = t.attentionGeneration;
    feed(t, "\033]9;4;1;50\007");
    CHECK(t.attentionGeneration == beforeProgress);

    // Bounded as the delivered notification is.
    feed(t, ("\033]9;" + std::string(1000, 'x') + "\007").c_str());
    CHECK(Ubiquitty::unseenAttention(&t).notificationBody.size() == Ubiquitty::OSC::attentionTextCapacity);

    // A hard reset is the program's doing, not a person looking.
    feed(t, "\033c");
    CHECK_MSG(Ubiquitty::unseenAttention(&t).hasBell && Ubiquitty::unseenAttention(&t).hasNotification, "a hard reset does not clear what nobody has seen");

    const uint32_t beforeSeen = t.attentionGeneration;
    CHECK(Ubiquitty::markAttentionSeen(t));
    const Ubiquitty::UnseenAttention seen = Ubiquitty::unseenAttention(&t);
    CHECK(!seen.hasBell && !seen.hasNotification && seen.notificationTitle.empty() && seen.notificationBody.empty());
    CHECK(t.attentionGeneration == beforeSeen + 1);
}

// A notification the host policy denies is not delivered and not latched.
TEST(aDeniedNotificationRaisesNoAttention)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    feed(t, "\033]9;Build finished\007");
    CHECK(!Ubiquitty::unseenAttention(&t).hasNotification);
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
    feed(t, "\033]9;Build finished\007");
    CHECK_MSG(Ubiquitty::unseenAttention(&t).hasNotification, "an allowed one is latched with no shell client to deliver it to");
    t.capabilityPolicy = nullptr;
}

// iTerm2's tab colour shares OSC 6 with the current-file URL. It is told
// apart by its `1;bg;` prefix, a channel at a time, and never touches the URL.
TEST(anITerm2TabColourIsSetAChannelAtATimeAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    ProgressInvalidateClient client;
    t.windowClient = &client;
    const auto isColor = [](std::optional<Ubiquitty::ColorRGBA> c, uint8_t r, uint8_t g, uint8_t b) {
        return c && c->red == r && c->green == g && c->blue == b;
    };
    feed(t, "\033]6;file:///tmp/doc.txt\007");
    const uint32_t before = Ubiquitty::tabStatusGeneration(&t);
    CHECK(!Ubiquitty::tabColor(&t));

    feed(t, "\033]6;1;bg;red;brightness;255\007");
    CHECK_MSG(isColor(Ubiquitty::tabColor(&t), 255, 0, 0), "one channel sets a colour whose other channels are zero");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == before + 1);
    feed(t, "\033]6;1;bg;green;brightness;128\033\\");
    feed(t, "\033]6;1;bg;blue;brightness;7\007");
    CHECK(isColor(Ubiquitty::tabColor(&t), 255, 128, 7));
    CHECK_MSG(t.currentFileURL == "file:///tmp/doc.txt", "the tab colour form leaves the current-file URL alone");

    // The same value again wakes nobody.
    const uint32_t settled = Ubiquitty::tabStatusGeneration(&t);
    feed(t, "\033]6;1;bg;blue;brightness;7\007");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == settled);

    // Malformed forms change nothing: a value past 255, no value, letters, an
    // unknown channel, an unknown property, trailing fields.
    for (const char* malformed : { "\033]6;1;bg;red;brightness;256\007", "\033]6;1;bg;red;brightness;\007", "\033]6;1;bg;red;brightness;1x\007",
             "\033]6;1;bg;alpha;brightness;1\007", "\033]6;1;bg;red;hue;1\007", "\033]6;1;bg;red;brightness;1;2\007", "\033]6;1;bg;red\007",
             "\033]6;1;bg;red;brightness;0000000000000000000001\007", "\033]6;1;bg;\007", "\033]6;1;bg;*;brightness;1\007" }) {
        feed(t, malformed);
        CHECK_MSG(isColor(Ubiquitty::tabColor(&t), 255, 128, 7), malformed);
    }
    CHECK(Ubiquitty::tabStatusGeneration(&t) == settled);
    CHECK_MSG(t.currentFileURL == "file:///tmp/doc.txt", "a malformed tab colour is not taken for a file URL");

    feed(t, "\033]6;1;bg;*;default\007");
    CHECK_MSG(!Ubiquitty::tabColor(&t), "the reset form clears the colour");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == settled + 1);
    feed(t, "\033]6;1;bg;green;brightness;9\007");
    CHECK_MSG(isColor(Ubiquitty::tabColor(&t), 0, 9, 0), "a channel set after a reset starts from black, not from the cleared colour");

    // The current-file form keeps working beside it, including its clear.
    feed(t, "\033]6;file:///tmp/other\007");
    CHECK(t.currentFileURL == "file:///tmp/other");
    feed(t, "\033]6;\007");
    CHECK(t.currentFileURL.empty());
    CHECK(isColor(Ubiquitty::tabColor(&t), 0, 9, 0));

    // A hard reset clears it and says so.
    const uint32_t beforeReset = Ubiquitty::tabStatusGeneration(&t);
    feed(t, "\033c");
    CHECK(!Ubiquitty::tabColor(&t));
    CHECK(Ubiquitty::tabStatusGeneration(&t) != beforeReset);
    t.windowClient = nullptr;
}

// OSC 1337 SetBadgeFormat: base64 text, bounded, cleared by an empty value.
TEST(anITerm2BadgeIsDecodedBoundedAndCleared)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    ProgressInvalidateClient client;
    t.windowClient = &client;
    const uint32_t before = Ubiquitty::tabStatusGeneration(&t);
    CHECK(Ubiquitty::badgeText(&t).empty());

    feed(t, "\033]1337;SetBadgeFormat=cHJvZA==\007"); // "prod"
    CHECK(Ubiquitty::badgeText(&t) == "prod");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == before + 1);
    feed(t, "\033]1337;SetBadgeFormat=cHJvZA==\007");
    CHECK_MSG(Ubiquitty::tabStatusGeneration(&t) == before + 1, "the same badge again wakes nobody");

    // Malformed base64 keeps the badge that was there.
    feed(t, "\033]1337;SetBadgeFormat=!!!!\007");
    CHECK(Ubiquitty::badgeText(&t) == "prod");

    // Control characters do not reach a label a frontend draws.
    feed(t, "\033]1337;SetBadgeFormat=YQdiG2MKZA==\007"); // "a\ab\033c\nd"
    CHECK(Ubiquitty::badgeText(&t) == "abcd");

    // Longer than the bound: cut at a code-point boundary, never past it.
    {
        std::string text;
        while (text.size() < 2 * Ubiquitty::OSC::badgeTextCapacity)
            text += "\xC3\xA9"; // é, two bytes
        text.insert(0, "x"); // so the bound falls inside a scalar
        std::string encoded(text.size() * 2, '\0');
        const auto characters = Ubiquitty::Codec::base64Encode({ text.data(), text.size() }, { encoded.data(), encoded.size() });
        CHECK(characters && !characters->empty());
        encoded.resize(characters ? characters->size() : 0);
        feed(t, ("\033]1337;SetBadgeFormat=" + encoded + "\007").c_str());
        const std::string_view badge = Ubiquitty::badgeText(&t);
        CHECK(badge.size() <= Ubiquitty::OSC::badgeTextCapacity);
        CHECK(badge.size() >= Ubiquitty::OSC::badgeTextCapacity - 1);
        CHECK_MSG(text.starts_with(badge) && (badge.size() % 2) == 1, "the cut falls between scalars");
    }

    feed(t, "\033]1337;SetBadgeFormat=\007");
    CHECK_MSG(Ubiquitty::badgeText(&t).empty(), "an empty value clears the badge");

    feed(t, "\033]1337;SetBadgeFormat=cHJvZA==\007");
    feed(t, "\033c");
    CHECK_MSG(Ubiquitty::badgeText(&t).empty(), "a hard reset clears the badge");
    t.windowClient = nullptr;
}

TEST(testOSC133ExitCode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.commandCompletedGeneration == 0);
    CHECK(t.lastCommandExitCode == -1);

    feed(t, "\033]133;D;0\007");
    CHECK(t.commandCompletedGeneration == 1);
    CHECK(t.lastCommandExitCode == 0);

    feed(t, "\033]133;D;42\007");
    CHECK(t.commandCompletedGeneration == 2);
    CHECK(t.lastCommandExitCode == 42);

    // D with no exit status -> unknown (-1), still counts as a completion.
    feed(t, "\033]133;D\007");
    CHECK(t.commandCompletedGeneration == 3);
    CHECK(t.lastCommandExitCode == -1);

    // Prompt / command-start / output-start marks must not bump completion.
    feed(t, "\033]133;A\007");
    feed(t, "\033]133;B\007");
    feed(t, "\033]133;C\007");
    CHECK(t.commandCompletedGeneration == 3);
}

TEST(testOSC133CommandDuration)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!t.hasCommandStartTick);

    // No clock installed: even a full C...D cycle reads unknown.
    feed(t, "\033]133;C\007");
    CHECK(!t.hasCommandStartTick);
    feed(t, "\033]133;D;0\007");
    CHECK(t.commandCompletedGeneration == 1);
    CHECK(t.commandDurationTicksRing[0] == Ubiquitty::Screen::commandDurationUnknown);

    // Install the fake clock for the rest of the test (restored on return); the
    // assertions above ran with no context — the degraded, clock-absent path.
    Ubiquitty::PAL::Context clockContext;
    clockContext.monotonicClock = &osc133FakeClock;
    Ubiquitty::PAL::ScopedContext scopedClock(clockContext);

    // D without any C: duration unknown.
    feed(t, "\033]133;D;0\007");
    CHECK(t.commandCompletedGeneration == 2);
    CHECK(t.commandDurationTicksRing[1] == Ubiquitty::Screen::commandDurationUnknown);

    // A full A/B/C...D cycle measures exactly the C->D tick delta.
    osc133FakeTicks = 100;
    feed(t, "\033]133;A\007");
    feed(t, "\033]133;B\007");
    feed(t, "\033]133;C\007");
    CHECK(t.hasCommandStartTick);
    osc133FakeTicks = 700; // 600 ticks = 10s at 60 Hz
    feed(t, "\033]133;D;0\007");
    CHECK(t.commandCompletedGeneration == 3);
    CHECK(t.commandDurationTicksRing[2] == 600);
    CHECK(!t.hasCommandStartTick); // consumed — the next bare D is unknown again

    feed(t, "\033]133;D;0\007");
    CHECK(t.commandDurationTicksRing[3] == Ubiquitty::Screen::commandDurationUnknown);

    // Tick wrap-around still yields the right unsigned delta.
    osc133FakeTicks = 0xFFFFFFF0u;
    feed(t, "\033]133;C\007");
    osc133FakeTicks = 0x00000010u;
    feed(t, "\033]133;D;0\007");
    CHECK(t.commandDurationTicksRing[4] == 0x20);
}

TEST(testOSCBufferGrowsForLargePayload)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    stubDecodeLength = 0;
    Ubiquitty::PAL::ScopedContext scopedImage(palContextWithImageDecoder(&stubImageDecoder));
    char seq[2048];
    int n = snprintf(seq, sizeof(seq), "\033]1337;File=inline=1:");
    memset(seq + n, 'A', 1200); // 1200 base64 chars → 900 raw bytes
    n += 1200;
    seq[n++] = '\007';
    feed(t, std::string_view(seq, n));
    CHECK(stubDecodeLength == 900);
}

TEST(testOSC1337RequestAttention)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    AttentionClient client;
    t.windowClient = &client;

    feed(t, "\033]1337;RequestAttention=yes\007");
    CHECK(client.calls == 1);
    CHECK(client.last == Ubiquitty::AttentionRequest::Continuous);

    feed(t, "\033]1337;RequestAttention=once\007");
    CHECK(client.last == Ubiquitty::AttentionRequest::Once);

    feed(t, "\033]1337;RequestAttention=no\007");
    CHECK(client.calls == 3);
    CHECK(client.last == Ubiquitty::AttentionRequest::Cancel);

    t.windowClient = nullptr;
}

TEST(testOSC1337StealFocus)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    StealFocusClient client;
    t.windowClient = &client;

    feed(t, "\033]1337;StealFocus\007");
    CHECK(client.calls == 1);

    // ST terminator works too.
    feed(t, "\033]1337;StealFocus\033\\");
    CHECK(client.calls == 2);

    // A trailing payload is not StealFocus — exact compare, no fire.
    feed(t, "\033]1337;StealFocusNow\007");
    CHECK(client.calls == 2);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    t.windowClient = nullptr;
}

TEST(testOSC1337SetColorsPreset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    ColorPresetClient client;
    t.windowClient = &client;

    feed(t, "\033]1337;SetColors=preset=Solarized Dark\007");
    CHECK(client.calls == 1);
    CHECK(client.last == "Solarized Dark");

    // ST terminator works too.
    feed(t, "\033]1337;SetColors=preset=One Dark\033\\");
    CHECK(client.calls == 2);
    CHECK(client.last == "One Dark");

    // Disabling color operations gates the live switch.
    t.allowColorOperations = false;
    feed(t, "\033]1337;SetColors=preset=Nord\007");
    CHECK(client.calls == 2);
    t.allowColorOperations = true;

    // A non-preset SetColors key (e.g. fg=) is not accepted — no fire.
    feed(t, "\033]1337;SetColors=fg=ff0000\007");
    CHECK(client.calls == 2);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    t.windowClient = nullptr;
}

TEST(testOSC50SetFont)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    FontClient client;
    t.windowClient = &client;

    // Bare family name → setFont(family, 0, 0).
    feed(t, "\033]50;Menlo\007");
    CHECK(client.calls == 1);
    CHECK(client.family == "Menlo");
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == 0.0);
    // Mirrored into Terminal::font for the '?' query.
    CHECK(t.font == "Menlo");

    // family:size=N → family + absolute size.
    feed(t, "\033]50;Menlo:size=14\007");
    CHECK(client.calls == 2);
    CHECK(client.family == "Menlo");
    CHECK(client.sizePoints == 14.0);
    CHECK(client.sizeDelta == 0.0);

    // rxvt absolute size #N → size only, family empty.
    feed(t, "\033]50;#18\007");
    CHECK(client.calls == 3);
    CHECK(client.family.empty());
    CHECK(client.sizePoints == 18.0);
    CHECK(client.sizeDelta == 0.0);

    // rxvt relative bump #+2 → delta only.
    feed(t, "\033]50;#+2\007");
    CHECK(client.calls == 4);
    CHECK(client.family.empty());
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == 2.0);

    // rxvt relative bump #-3 → negative delta.
    feed(t, "\033]50;#-3\007");
    CHECK(client.calls == 5);
    CHECK(client.sizeDelta == -3.0);

    // Empty payload → reset (family empty, no size, no delta).
    feed(t, "\033]50;\007");
    CHECK(client.calls == 6);
    CHECK(client.family.empty());
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == 0.0);
    CHECK(t.font.empty()); // query mirror cleared

    /// Malformed numeric # form (no digits) → ignored, no callback.
    int before = client.calls;
    feed(t, "\033]50;#xyz\007");
    CHECK(client.calls == before);

    // '?' query replies with the last mirrored family.
    feed(t, "\033]50;Courier\007");
    char outputBuffer[256];
    feed(t, "\033]50;?\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]50;Courier\033\\") == 0);

    // allowFontOperations gate: a set is dropped when disabled.
    t.allowFontOperations = false;
    before = client.calls;
    feed(t, "\033]50;Monaco\007");
    CHECK(client.calls == before);
    t.allowFontOperations = true;

    t.windowClient = nullptr;
}

// A reply reaches the host as typed input, so a control byte the host itself put
// in the family name must not come back out: the OSC payload scanner stops only
// at BEL / ESC / CAN / SUB / 8-bit ST, so CR and LF do reach Terminal::font.
TEST(testOSC50QueryDropsControlBytesFromTheReply)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    FontClient client;
    t.windowClient = &client;

    // LF inside the family, then the query. Unsanitized this replies
    // "ESC ] 50 ; Menlo\necho pwned\n ESC \" and the shell runs the echo.
    feed(t, "\033]50;Menlo\necho pwned\n\007");
    CHECK(t.font == "Menlo\necho pwned\n"); // stored raw — display is not the channel
    feed(t, "\033]50;?\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]50;Menloecho pwned\033\\") == 0);
    CHECK(strchr(outputBuffer, '\n') == nullptr);
    CHECK(strchr(outputBuffer, '\r') == nullptr);

    // A stray ESC inside the payload abandons the code: the family is not
    // set, and the byte after the ESC begins a sequence of its own — here
    // SOS, which swallows the "B" and the BEL — so no ESC reaches the family,
    // and none can frame a reply.
    feed(t, "\033]50;A\033XB\007\033\\");
    CHECK(t.font == "Menlo\necho pwned\n");
    feed(t, "\033]50;?\033\\");
    drain(t, outputBuffer);
    // The only ESCs are the reply's own opener and its ST.
    CHECK(strcmp(outputBuffer, "\033]50;Menloecho pwned\033\\") == 0);

    // DEL and the UTF-8 form of C1 go too; ordinary UTF-8 is untouched.
    feed(t, "\033]50;M\177en\302\233lo\303\251\007");
    feed(t, "\033]50;?\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]50;Menlo\303\251\033\\") == 0);

    t.windowClient = nullptr;
}

TEST(test_xtgettcap_gated_by_allowTerminalCapabilityOps)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);

    /// Denied: the "TN" query produces no response whatsoever.
    Ubiquitty::Modes::HostPolicy::setCapabilityQueryPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033P+q544e\033\\"); // "TN" = 544e
    CHECK(drain(t, outputBuffer) == 0);

    /// Allowed (the init default): the query answers as usual.
    Ubiquitty::Modes::HostPolicy::setCapabilityQueryPolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    feed(t, "\033P+q544e\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 4);
    outputBuffer[n] = 0;
    CHECK(strstr(outputBuffer, "544e=") != nullptr);

    Ubiquitty::Modes::HostPolicy::setCapabilityQueryPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033c\033P+q544e\033\\");
    CHECK(drain(t, outputBuffer) == 0);
    CHECK(!t.allowTerminalCapabilityOperations);
}

TEST(testOSCPayload0x9cNotTerminator)
{
    Ubiquitty::Terminal t;
    /// OSC 0; ✓ Demo Title BEL — ✓ (U+2713) encodes as E2 9C 93, so
    /// the middle byte is a 0x9C that a byte-wise C1 check would treat
    /// as ST.
    static const unsigned char bytes[] = { 0x1B, ']', '0', ';', 0xE2, 0x9C, 0x93, // U+2713 ✓
        ' ', 'D', 'e', 'm', 'o', ' ', 'T', 'i', 't', 'l', 'e', 0x07 };
    Ubiquitty::hardReset(t);
    feed(t, bytes);

    // The OSC must have completed: title is set and titleChanged
    // was raised. The leading ✓ is unrenderable in a MacRoman
    // Pascal-string title, so it (and the space that follows) are
    // dropped — the result must contain "Demo Title".
    CHECK(t.titleChanged == true);
    CHECK(t.titleUTF8.find("Demo Title") != std::string::npos);

    // And none of the payload bytes leaked into cell
    // row 0. A parser treating 0x9C as an abort renders 0x93 plus
    // " Demo Title\x07" as cells.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testOSCCodeAbove255NotClamped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    UnknownOSCCodeDiagClient diag;
    t.diagnosticsClient = &diag;

    // Wezterm OSC 7704: the parser must see code=7704 and the payload
    // "foo", not code=255 (a saturate-at-255 parse).
    diag.code = -1;
    diag.payload[0] = 0;
    feed(t, "\033]7704;foo\007");
    CHECK(diag.code == 7704);
    CHECK(strcmp(diag.payload, "foo") == 0);

    // kitty OSC 9001.
    diag.code = -1;
    feed(t, "\033]9001;bar\007");
    CHECK(diag.code == 9001);

    // Boundary: the OSC code saturates at 65535, its own cap (a CSI parameter
    // saturates at VT::maxParameterValue).
    diag.code = -1;
    feed(t, "\033]65535;baz\007");
    CHECK(diag.code == 65535);

    // Above the cap clamps to 65535 (saturate, not wrap) — better than
    // drop, and matches existing decimalAppend(..., 65535) elsewhere
    // in Dispatch.cpp.
    diag.code = -1;
    feed(t, "\033]999999;huge\007");
    CHECK(diag.code == 65535);

    // Known code still dispatches normally (observer not wasCalled).
    diag.code = -1;
    feed(t, "\033]0;Hello\007");
    CHECK(diag.code == -1);
    CHECK(t.titleUTF8.find("Hello") != std::string::npos);

    t.diagnosticsClient = nullptr;
}

TEST(testOSC104ResetPaletteIndex)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    Ubiquitty::PaletteRGB base1 = t.palette.indexed[1];
    feed(t, "\033]4;1;rgb:ffff/0000/0000\033\\");
    CHECK(t.palette.indexed[1].red == 0xffff);
    CHECK(t.palette.indexed[1].green == 0x0000);

    // OSC 104 ; 1 reverts slot 1 only.
    feed(t, "\033]104;1\033\\");
    CHECK(t.palette.indexed[1].red == base1.red);
    CHECK(t.palette.indexed[1].green == base1.green);
    CHECK(t.palette.indexed[1].blue == base1.blue);

    /// Bare OSC 104 resets every slot.
    Ubiquitty::PaletteRGB base2 = t.palette.indexed[2];
    feed(t, "\033]4;2;rgb:1234/5678/9abc\033\\");
    CHECK(t.palette.indexed[2].red == 0x1212);
    feed(t, "\033]104\033\\");
    CHECK(t.palette.indexed[2].red == base2.red);
    CHECK(t.palette.indexed[2].green == base2.green);
    CHECK(t.palette.indexed[2].blue == base2.blue);
}

// The theme's per-index opacity lives in the palette beside the colour. A
// program's OSC 4 write makes the slot it writes opaque and leaves the others
// dimmed; OSC 104 restores the baseline's alpha with its colour; and a write
// of the very colour the slot already holds still makes it opaque, since the
// program's colour has no alpha.
TEST(testOSC4MakesTheSlotOpaqueAndOSC104GivesTheThemeAlphaBack)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.palette.indexedAlpha[1] = 0x40;
    t.palette.indexedAlpha[2] = 0x80;
    t.paletteBaseline = t.palette;
    const uint64_t epochBefore = t.paletteEpoch;

    feed(t, "\033]4;1;rgb:ffff/0000/0000\033\\");
    CHECK(t.palette.indexedAlpha[1] == 255);
    CHECK(t.palette.indexedAlpha[2] == 0x80);
    CHECK(t.paletteEpoch != epochBefore);

    feed(t, "\033]104;1\033\\");
    CHECK(t.palette.indexedAlpha[1] == 0x40);
    CHECK(t.palette.indexed[1].red == t.paletteBaseline.indexed[1].red);

    // The same colour again: nothing moves in the channels, the alpha does,
    // and the frame must learn it.
    const Ubiquitty::PaletteRGB held = t.palette.indexed[2];
    const uint64_t epochHeld = t.paletteEpoch;
    char spec[64];
    std::snprintf(spec, sizeof spec, "\033]4;2;rgb:%04x/%04x/%04x\033\\", held.red, held.green, held.blue);
    feed(t, spec);
    CHECK(t.palette.indexedAlpha[2] == 255);
    CHECK(t.paletteEpoch != epochHeld);

    feed(t, "\033]104\033\\");
    CHECK(t.palette.indexedAlpha[2] == 0x80);
}

TEST(testUnknownOSCLongPayloadReachesDiagnostics)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    UnknownOSCCodeDiagClient diag;
    t.diagnosticsClient = &diag;

    /// OSC 20 is unhandled (no dispatch-table entry), so a long payload
    /// under it must reach the diagnostics client unmodified.
    std::string seq = "\033]20;";
    seq.append(9000, 'A');
    seq.push_back('\007');
    feed(t, seq);

    CHECK(diag.code == 20);
    CHECK(strncmp(diag.payload, "AAAAAAAAAAAAAAAA", 16) == 0);

    t.diagnosticsClient = nullptr;
}

// The three parsers behind OSC 7402 and the dispatcher itself, each against the
// input it exists to refuse. Every case above feeds a well-formed sequence and
// checks the state it sets, which leaves the refusals untested: relaxing the hex
// width to five digits, admitting a zero aspect width, or dropping the
// dispatcher's trailing-garbage check all left the other 120 cases green.
//
// A refusal here is the whole command ignored, so each case sets a known state
// first and requires the malformed one to leave it exactly.
TEST(testOSC7402RefusesMalformedValues)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]7402;fit=stretch;ar=4:3;fill=1a2b3c\033\\");
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);

    // A letterbox colour is exactly six hex digits. Five is a colour a client
    // meant to write and mistyped, and a parser that padded or truncated it
    // would paint a different one rather than keep the last.
    feed(t, "\033]7402;fill=1a2b3\033\\");
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);
    feed(t, "\033]7402;fill=1a2b3c4\033\\");
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);
    feed(t, "\033]7402;fill=1a2b3g\033\\");
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);

    // A zero on either side is refused, though not by the parser: the caller's
    // [1/8, 8] range test rejects it first, since 3 > 8 x 0. Relaxing the
    // parser's own `width <= 0` leaves these cases green, so they pin the
    // behaviour and the range test is what holds it.
    feed(t, "\033]7402;fit=stretch;ar=0:3\033\\");
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
    CHECK(t.fullscreenContentDisplay.aspectHeight == 3);
    feed(t, "\033]7402;fit=stretch;ar=4:0\033\\");
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
    feed(t, "\033]7402;fit=stretch;ar=4\033\\");
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
    feed(t, "\033]7402;fit=stretch;ar=:3\033\\");
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
}

// The dispatcher reads the leading Ps and requires a ';' or the end of the
// payload after it. Without that check "0xhello" dispatches as OSC 0 carrying
// "xhello", and a title the sender never set reaches the window.
//
// OSC 0 rather than OSC 4, which cannot see this: a palette payload beginning
// with a letter is refused by handleIndexedColor's own index parser, so the dispatcher
// could accept anything and the slot would still not move. A handler that takes
// its payload verbatim is the only one where the dispatcher's check is the check.
TEST(testOSCLeadingCodeRefusesTrailingGarbage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]0;before\033\\");
    CHECK(t.titleUTF8 == "before");

    feed(t, "\033]0xhello\033\\");
    CHECK(t.titleUTF8 == "before");

    // A code with neither digits nor a separator is refused on the same line;
    // an omitted number before the separator is 0, as xterm reads it.
    feed(t, "\033]hello\033\\");
    CHECK(t.titleUTF8 == "before");
    feed(t, "\033];omitted\033\\");
    CHECK(t.titleUTF8 == "omitted");

    // An explicit selector still works after both forms.
    feed(t, "\033]0;after\033\\");
    CHECK(t.titleUTF8 == "after");
}

// One feature, one number: the controller answers on 7402, beside the DECSET that
// turns the mode on, and on no other. The case pins the whole contract to that one
// number — the happy path, the bare reset and a refusal — because a controller whose
// smoke only sends one command can lose the rest of its surface without anything saying so.
TEST(testTheFullscreenControllerAnswersOnItsModesNumber)
{
    using Fit = Ubiquitty::FullscreenContentFit;
    using Letterbox = Ubiquitty::FullscreenContentLetterbox;

    const auto displayAfter = [](const char* sequence) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, sequence);
        return t.fullscreenContentDisplay;
    };

    const auto onNewNumber = displayAfter("\033]7402;fit=stretch;ar=4:3;fill=1a2b3c\033\\");
    CHECK_MSG(onNewNumber.fit == Fit::Stretch, "7402 reaches the controller");
    CHECK(onNewNumber.aspectWidth == 4 && onNewNumber.aspectHeight == 3);
    CHECK(onNewNumber.letterbox == Letterbox::Color && onNewNumber.letterboxColor == 0x1a2b3c);

    // 2501 is not this controller's number. The sequence is inert and the display
    // keeps its defaults, so a table edit cannot re-admit the number in silence.
    Ubiquitty::Terminal removed;
    Ubiquitty::hardReset(removed);
    const auto before = removed.fullscreenContentDisplay;
    feed(removed, "\033]2501;fit=stretch;ar=4:3;fill=1a2b3c\033\\");
    CHECK_MSG(removed.fullscreenContentDisplay.fit == before.fit, "2501 is not the fullscreen controller's number");
    CHECK(removed.fullscreenContentDisplay.aspectWidth == before.aspectWidth);
    CHECK(removed.fullscreenContentDisplay.letterboxColor == before.letterboxColor);

    // A bare command resets, on the number that stays.
    const auto bare = displayAfter("\033]7402\033\\");
    CHECK(bare.fit == Fit::Contain && bare.aspectWidth == 0);
    CHECK(bare.letterbox == Letterbox::Color && bare.letterboxColor == 0);

    // And a malformed value is refused on it, so the new number carries the whole
    // contract rather than only the happy path.
    Ubiquitty::Terminal strict;
    Ubiquitty::hardReset(strict);
    feed(strict, "\033]7402;fit=native;fill=1a2b3c\033\\");
    feed(strict, "\033]7402;fill=1a2b3g\033\\");
    CHECK_MSG(strict.fullscreenContentDisplay.fit == Fit::Native,
        "a malformed value leaves the selection intact on 7402");
    CHECK(strict.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);
}

TEST(testOSC7402FullscreenContentDisplay)
{
    using Fit = Ubiquitty::FullscreenContentFit;
    using Letterbox = Ubiquitty::FullscreenContentLetterbox;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.fullscreenContentDisplay.fit == Fit::Contain);
    CHECK(t.fullscreenContentDisplay.aspectWidth == 0);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Color);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0);

    // Stretch to 4:3 for a 320x200 DOS source; letterbox defaults to black.
    feed(t, "\033]7402;fit=stretch;ar=4:3\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    CHECK(t.fullscreenContentDisplay.aspectWidth == 4);
    CHECK(t.fullscreenContentDisplay.aspectHeight == 3);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Color);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0);

    // Native fit + a custom letterbox color.
    feed(t, "\033]7402;fit=native;fill=1a2b3c\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Native);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Color);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0x1a2b3c);
    // Complete-state: aspect is not carried over from the prior stretch.
    CHECK(t.fullscreenContentDisplay.aspectWidth == 0);

    // Scale fit + explicit source-px -> device-px multiplier.
    feed(t, "\033]7402;fit=scale;scale=3\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Scale);
    CHECK(t.fullscreenContentDisplay.scaleMultiplier == 3);

    // Stretch 16:9 with passthrough letterbox.
    feed(t, "\033]7402;fit=stretch;ar=16:9;fill=pass\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    CHECK(t.fullscreenContentDisplay.aspectWidth == 16);
    CHECK(t.fullscreenContentDisplay.aspectHeight == 9);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Passthrough);

    // DECSTR (soft reset) PRESERVES the selection — mirrors 7402 itself.
    feed(t, "\033[!p");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    CHECK(t.fullscreenContentDisplay.aspectWidth == 16);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Passthrough);

    // Malformed values fail the whole command — the selection is not clobbered.
    feed(t, "\033]7402;fit=bogus\033\\"); // unknown fit
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    feed(t, "\033]7402;fit=stretch\033\\"); // stretch without ar
    CHECK(t.fullscreenContentDisplay.aspectWidth == 16);
    feed(t, "\033]7402;fill=xyz\033\\"); // bad hex color
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Passthrough);
    feed(t, "\033]7402;ar=99:1\033\\"); // ratio outside [1/8, 8]
    CHECK(t.fullscreenContentDisplay.aspectWidth == 16);
    feed(t, "\033]7402;foo\033\\"); // token without '='
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    feed(t, "\033]7402;fit=scale\033\\"); // Scale without scale=N
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    feed(t, "\033]7402;fit=scale;scale=0\033\\"); // scale must be positive
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    feed(t, "\033]7402;fit=scale;scale=xy\033\\"); // non-numeric scale
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);

    // Unknown keys are ignored (forward-compat); the known key still applies.
    feed(t, "\033]7402;fit=contain;zoom=2\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Contain);

    // A bare OSC 7402 resets to defaults (Contain + black-occlude), clearing scale too.
    feed(t, "\033]7402;fit=scale;scale=5;fill=445566\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Scale);
    CHECK(t.fullscreenContentDisplay.scaleMultiplier == 5);
    feed(t, "\033]7402\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Contain);
    CHECK(t.fullscreenContentDisplay.scaleMultiplier == 0);
    CHECK(t.fullscreenContentDisplay.letterbox == Letterbox::Color);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0);

    // RIS (hard reset) clears a live selection.
    feed(t, "\033]7402;fit=stretch;ar=21:9\033\\");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Stretch);
    feed(t, "\033c");
    CHECK(t.fullscreenContentDisplay.fit == Fit::Contain);
    CHECK(t.fullscreenContentDisplay.aspectWidth == 0);
    CHECK(t.fullscreenContentDisplay.letterboxColor == 0);
}

TEST(testOSC9ProgressReported)
{
    Ubiquitty::Terminal t;
    uint8_t state = 0xFF;
    uint8_t value = 0xFF;
    uint32_t gen0;
    Ubiquitty::hardReset(t);
    ProgressInvalidateClient client;
    t.windowClient = &client;
    gen0 = Ubiquitty::progressGeneration(&t);
    feed(t, "AB\033]9;4;1;50\007CD");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    CHECK(cell(t, 0, 3)->codePoint == 'D');
    CHECK(t.titleUTF8.empty());
    Ubiquitty::getProgress(&t, &state, &value);
    CHECK(state == 1);
    CHECK(value == 50);
    CHECK(Ubiquitty::progressGeneration(&t) == gen0 + 1);
    // Each progress push nudges the frontend to repaint.
    CHECK(client.invalidates == 1);

    // state=2 (error) — value irrelevant, default 0.
    feed(t, "\033]9;4;2\007");
    Ubiquitty::getProgress(&t, &state, &value);
    CHECK(state == 2);
    CHECK(value == 0);
    CHECK(Ubiquitty::progressGeneration(&t) == gen0 + 2);

    // state=3 (indeterminate) bumps generation again.
    feed(t, "\033]9;4;3;0\007");
    CHECK(Ubiquitty::progressGeneration(&t) == gen0 + 3);

    // state=0 clears.
    feed(t, "\033]9;4;0\007");
    Ubiquitty::getProgress(&t, &state, &value);
    CHECK(state == 0);

    // Value out of range clamps; an OOB state falls back to 0.
    feed(t, "\033]9;4;1;250\007");
    Ubiquitty::getProgress(&t, &state, &value);
    CHECK(state == 1);
    CHECK(value == 100);

    feed(t, "\033]9;4;7;10\007");
    Ubiquitty::getProgress(&t, &state, &value);
    CHECK(state == 0);

    // OSC 9 non-progress sub-commands are accepted without mutating
    // progress state or generation, and do not nudge the frontend.
    {
        uint32_t gen = Ubiquitty::progressGeneration(&t);
        int inv = client.invalidates;
        feed(t, "\033]9;hello world\007");
        CHECK(Ubiquitty::progressGeneration(&t) == gen);
        CHECK(client.invalidates == inv);
        CHECK(t.titleUTF8.empty());
    }

    t.windowClient = nullptr;
}

TEST(testOSC9ReportsPreserveOrderAndOptionalValues)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    ProgressInvalidateClient client;
    t.windowClient = &client;
    feed(t, "\033]9;4;1;25\a\033]9;4;1;25\a\033]9;4;4;60\033\\\033]9;4;2\a\033]9;4;2;0\a\033]9;4;3\a\033]9;4;0\a");
    const std::vector<std::pair<uint8_t, int>> expected { { 1, 25 }, { 1, 25 }, { 4, 60 }, { 2, -1 }, { 2, 0 }, { 3, -1 }, { 0, -1 } };
    CHECK(client.reports == expected);
    CHECK(client.invalidates == 5);
    t.windowClient = nullptr;
}

TEST(testOSC21337TabStatus)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    ProgressInvalidateClient client;
    t.windowClient = &client;

    auto isColor = [](std::optional<Ubiquitty::ColorRGBA> c, uint8_t r, uint8_t g, uint8_t b) {
        return c && c->red == r && c->green == g && c->blue == b;
    };

    uint32_t gen0 = Ubiquitty::tabStatusGeneration(&t);
    CHECK(!Ubiquitty::tabStatusColor(&t)); // unset initially

    // waiting: blue #5f87ff, with the label that names it. status-color is
    // accepted and ignored; the indicator is the colour.
    feed(t, "XY\033]21337;indicator=#5f87ff;status=Waiting;status-color=#5f87ff\007Z");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
    CHECK(cell(t, 0, 2)->codePoint == 'Z');
    CHECK(t.titleUTF8.empty());
    CHECK(isColor(Ubiquitty::tabStatusColor(&t), 0x5f, 0x87, 0xff));
    CHECK(Ubiquitty::tabStatusText(&t) == "Waiting");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == gen0 + 1);
    CHECK(client.invalidates == 1);

    // busy: orange #ff9500, sent without a label. The label travels with the
    // escape that sets the colour, so "Waiting" does not stay beside busy.
    feed(t, "\033]21337;indicator=#ff9500\007");
    CHECK(isColor(Ubiquitty::tabStatusColor(&t), 0xff, 0x95, 0x00));
    CHECK_MSG(Ubiquitty::tabStatusText(&t).empty(),
        "a colour set without a label clears the label the last colour had");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == gen0 + 2);

    // Clear via empty value (CLEAR_TAB_STATUS form).
    feed(t, "\033]21337;indicator=;status=;status-color=\007");
    CHECK(!Ubiquitty::tabStatusColor(&t));
    CHECK(Ubiquitty::tabStatusText(&t).empty());
    CHECK(Ubiquitty::tabStatusGeneration(&t) == gen0 + 3);

    // A bare `indicator` key (no '=') also clears.
    feed(t, "\033]21337;indicator=#00d75f\007"); // set green
    CHECK(isColor(Ubiquitty::tabStatusColor(&t), 0x00, 0xd7, 0x5f));
    feed(t, "\033]21337;indicator\007");
    CHECK(!Ubiquitty::tabStatusColor(&t));

    // Escapes inside a status value are decoded into the label, and don't
    // break parsing of a trailing indicator.
    feed(t, "\033]21337;status=a\\;b\\\\c;indicator=#010203\007");
    CHECK(isColor(Ubiquitty::tabStatusColor(&t), 0x01, 0x02, 0x03));
    CHECK(Ubiquitty::tabStatusText(&t) == "a;b\\c");

    // Malformed color is ignored: prior indicator stays put, no nudge.
    {
        uint32_t gen = Ubiquitty::tabStatusGeneration(&t);
        int inv = client.invalidates;
        feed(t, "\033]21337;indicator=#zzzz\007");
        CHECK(isColor(Ubiquitty::tabStatusColor(&t), 0x01, 0x02, 0x03)); // still #010203
        CHECK(Ubiquitty::tabStatusGeneration(&t) == gen);
        CHECK(client.invalidates == inv);
    }

    t.windowClient = nullptr;
}

// The label is part of what changed. An agent moving between two states it
// paints in the same colour still moved, and a reader hovering the indicator
// must see the new label — so a label-only change wakes the frontend.
TEST(testATabStatusLabelChangeAloneIsAChange)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]21337;indicator=#010203;status=Thinking\007");
    const uint32_t gen = Ubiquitty::tabStatusGeneration(&t);
    feed(t, "\033]21337;indicator=#010203;status=Writing\007");
    CHECK(Ubiquitty::tabStatusText(&t) == "Writing");
    CHECK_MSG(Ubiquitty::tabStatusGeneration(&t) == gen + 1,
        "the same colour with a new label is a change");

    // And the same escape again is not.
    feed(t, "\033]21337;indicator=#010203;status=Writing\007");
    CHECK(Ubiquitty::tabStatusGeneration(&t) == gen + 1);
}

// A label longer than the budget is cut, and never through the middle of a
// multi-byte sequence: a split one would reach the tooltip as a replacement
// character where the reader's language had a letter.
TEST(testALongTabStatusLabelIsCutOnACodePointBoundary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string label;
    for (int i = 0; i < 40; ++i)
        label += "\xC3\xA9"; // é, two bytes each: 80 bytes, well over the budget
    feed(t, "\033]21337;indicator=#010203;status=" + label + "\007");
    const std::string_view kept = Ubiquitty::tabStatusText(&t);
    CHECK(kept.size() <= Ubiquitty::OSC::tabStatusTextCapacity);
    CHECK_MSG(kept.size() % 2 == 0, "every kept é is whole");
    CHECK(!kept.empty());
    CHECK(kept == std::string_view(label).substr(0, kept.size()));
}

// With no indicator there is no label, whatever the stored bytes still hold,
// for the reason the colour folds its flag in below.
TEST(testAClearedTabStatusHasNoLabel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]21337;indicator=#010203;status=Waiting\007");
    CHECK(Ubiquitty::tabStatusText(&t) == "Waiting");
    feed(t, "\033]21337;indicator\007");
    CHECK(Ubiquitty::tabStatusText(&t).empty());
}

// Clearing the indicator sets only the "is one set" flag; the terminal's three
// colour fields keep the colour that was in force. So the cleared state and a
// live #010203 differ in one byte of terminal state, and any reader that
// consults the colour without the flag sees an indicator that is no longer
// showing.
//
// The case above cannot see this: it reads the colour only where one is set.
TEST(testAClearedTabStatusYieldsNoColourThoughTheFieldsStillHoldOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]21337;indicator=#010203\007");
    const std::optional<Ubiquitty::ColorRGBA> set = Ubiquitty::tabStatusColor(&t);
    CHECK(set && set->red == 0x01 && set->green == 0x02 && set->blue == 0x03);

    feed(t, "\033]21337;indicator\007");
    CHECK_MSG(!Ubiquitty::tabStatusColor(&t),
        "a cleared indicator has no colour, though the fields behind it still "
        "hold the one that was showing");
    CHECK_MSG(t.tabStatusR == 0x01 && t.tabStatusG == 0x02 && t.tabStatusB == 0x03,
        "the fields are deliberately left alone on a clear — this is the state "
        "that makes the flag load-bearing, so the case fails if it stops being "
        "the state under test");

    // Alpha is not on the wire, so it must not vary with the indicator.
    CHECK(set->alpha == 255);
}

TEST(semanticRolesFollowPromptInputAndOutput)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007$ \033]133;B\007ls\033]133;C\007ok\033]133;D;0\007!");
    for (int column = 0; column < 2; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Prompt);
    for (int column = 2; column < 4; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Input);
    for (int column = 4; column < 8; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Output);
}

TEST(semanticRolesSurviveRenditionAndCursorRestores)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007\033[31m$\033[0m \0337\033]133;B\007\0338"
            "\033[mA\033[0;38;5;1;48;5;2mB\033[58;5;200mC\033[59mD");
    CHECK(cell(t, 0, 0)->pen.semanticRole == SemanticRole::Prompt);
    CHECK(cell(t, 0, 1)->pen.semanticRole == SemanticRole::Prompt);
    for (int column = 2; column < 6; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Input);
    CHECK(cell(t, 0, 4)->pen.underlineIndex == 200);
    CHECK(cell(t, 0, 5)->pen.underlineIndex == Ubiquitty::colorDefault);
}

TEST(reprintingTheSameGlyphUpdatesItsSemanticRole)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007Xab\r\033]133;B\007Xab");
    for (int column = 0; column < 3; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Input);
    feed(t, "\r\033]133;C\007Xab");
    for (int column = 0; column < 3; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Output);
}

TEST(semanticRolesMoveWithEditedCells)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007$ \033]133;B\007abc\r\033[2@\033[1P");
    CHECK(cell(t, 0, 0)->pen.semanticRole == SemanticRole::Output);
    CHECK(cell(t, 0, 1)->pen.semanticRole == SemanticRole::Prompt);
    CHECK(cell(t, 0, 3)->pen.semanticRole == SemanticRole::Input);
    feed(t, "\033[3X");
    for (int column = 0; column < 3; ++column)
        CHECK(cell(t, 0, column)->pen.semanticRole == SemanticRole::Output);
    CHECK(cell(t, 0, 3)->pen.semanticRole == SemanticRole::Input);
}

TEST(wideAndCombiningCellsKeepTheirSemanticRole)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007\xe4\xb8\xad\033]133;B\007e\xcc\x81");
    CHECK(cell(t, 0, 0)->pen.semanticRole == SemanticRole::Prompt);
    CHECK(cell(t, 0, 1)->pen.semanticRole == SemanticRole::Prompt);
    CHECK(cell(t, 0, 2)->pen.semanticRole == SemanticRole::Input);
    CHECK(cell(t, 0, 2)->combiningLength() == 1);
}

TEST(erasureClearsSemanticRoles)
{
    for (const char* erase : { "\033[2J", "\033[2K", "\033[3X", "\033[?2J", "\033[?2K", "\033[1;1;1;3$z", "\033[1;1;1;3${" }) {
        Ubiquitty::Terminal t;
        feed(t, "\033]133;A\007abc\r");
        feed(t, erase);
        for (int column = 0; column < 3; ++column)
            CHECK(cell(t, 0, column)->pen.semanticRole == Ubiquitty::SemanticRole::Output);
    }
}

TEST(semanticRolesAreIndependentAcrossScreensAndReset)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    feed(t, "\033]133;A\007$\033[?1049h\033]133;B\007X");
    CHECK(cell(t, 0, 0)->pen.semanticRole == SemanticRole::Output);
    feed(t, "\033[?1049lP");
    CHECK(cell(t, 0, 1)->pen.semanticRole == SemanticRole::Prompt);
    feed(t, "\033cX");
    CHECK(cell(t, 0, 0)->pen.semanticRole == SemanticRole::Output);
}

TEST(semanticRolesSurviveScrollingAndReflow)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 2, 6);
    feed(t, "\033]133;A\007$ \033]133;B\007abcdef\033]133;C\007\r\nout\r\n");
    const auto prompt = Ubiquitty::Screen::resolveAbsoluteRow(&t, 0);
    REQUIRE(prompt.base && prompt.length >= 6);
    CHECK(prompt.base[0].pen.semanticRole == SemanticRole::Prompt);
    CHECK(prompt.base[2].pen.semanticRole == SemanticRole::Input);
    Ubiquitty::resize(t, 2, 10);
    const auto reflowed = Ubiquitty::Screen::resolveAbsoluteRow(&t, 0);
    REQUIRE(reflowed.base && reflowed.length >= 8);
    CHECK(reflowed.base[0].pen.semanticRole == SemanticRole::Prompt);
    for (int column = 2; column < 8; ++column)
        CHECK(reflowed.base[column].pen.semanticRole == SemanticRole::Input);
}

TEST(testOSC133MarksCursorRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// Live visible rows address the absolute space at scrollbackCount + r.
    const int liveBase = Ubiquitty::Screen::scrollbackCount(&t);
    // Sub-command A on row 0.
    feed(t, "\033]133;A\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Move to row 1, B + C accumulate on that row.
    feed(t, "\n\033]133;B\007\033]133;C\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 1) == (Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart));
    // D with optional exit status — the exit code is parsed-and-ignored
    // but the bit lands.
    feed(t, "\n\033]133;D;0\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 2) == Ubiquitty::Screen::osc133MarkCmdEnd);
    // Row 0 also gained CMD_EXECUTED when C ran above: the prompt that started
    // on row 0 was promoted once its command executed.
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 0) == (Ubiquitty::Screen::osc133MarkPromptStart | Ubiquitty::Screen::osc133MarkCmdExecuted));
    // Unknown sub-commands drop silently.
    feed(t, "\033]133;Z\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 2) == Ubiquitty::Screen::osc133MarkCmdEnd);
}

TEST(testOSC133CmdExecutedDeferredToC)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const int base = Ubiquitty::Screen::scrollbackCount(&t);

    // Prompt 1 on row 0: A sets PROMPT_START but not CMD_EXECUTED yet.
    feed(t, "\033]133;A\007$ cmd1");
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 0) == Ubiquitty::Screen::osc133MarkPromptStart);

    // Submit: C on row 1 promotes CMD_EXECUTED onto prompt row 0; row 1 is the
    // output start.
    feed(t, "\r\n\033]133;C\007out1");
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 0) == (Ubiquitty::Screen::osc133MarkPromptStart | Ubiquitty::Screen::osc133MarkCmdExecuted));
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 1) == Ubiquitty::Screen::osc133MarkOutputStart);

    // End command 1, draw a fresh prompt 2 (A only) — bare, no CMD_EXECUTED.
    feed(t, "\r\n\033]133;D;0\007\r\n\033]133;A\007$ ");
    const int p2AbsoluteRow = Ubiquitty::Screen::scrollbackCount(&t) + t.screens.active->cursorY;
    CHECK((Ubiquitty::Screen::rowMark(&t, p2AbsoluteRow) & Ubiquitty::Screen::osc133MarkCmdExecuted) == 0);
    CHECK(Ubiquitty::Screen::rowMark(&t, p2AbsoluteRow) & Ubiquitty::Screen::osc133MarkPromptStart);

    /// Cmd-L's search skips the bare prompt 2 and lands on prompt 1, keeping the
    /// last executed command on screen. The same ceiling under PROMPT_START would
    /// instead stop at prompt 2, which makes Cmd-L behave like Cmd-K.
    const int ceiling = p2AbsoluteRow + 1;
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, ceiling, Ubiquitty::Screen::osc133MarkCmdExecuted) == base + 0);
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, ceiling, Ubiquitty::Screen::osc133MarkPromptStart) == p2AbsoluteRow);

    // Forward (Jump to Next Prompt) navigation: the only executed prompt is
    // base+0; the bare prompt 2 is not a CMD_EXECUTED target.
    CHECK(Ubiquitty::Screen::findPromptMarkAfter(&t, 0, Ubiquitty::Screen::osc133MarkCmdExecuted) == base + 0);
    CHECK(Ubiquitty::Screen::findPromptMarkAfter(&t, base + 1, Ubiquitty::Screen::osc133MarkCmdExecuted) == -1);
    CHECK(Ubiquitty::Screen::findPromptMarkAfter(&t, base + 1, Ubiquitty::Screen::osc133MarkPromptStart) == p2AbsoluteRow);
}

TEST(testOSC133FailedCommandMarksPrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const int base = Ubiquitty::Screen::scrollbackCount(&t);

    // Command 1 fails (exit 3).
    feed(t, "\033]133;A\007$ false\r\n\033]133;C\007\r\n\033]133;D;3\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 0) & Ubiquitty::Screen::osc133MarkCmdExecuted);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 0) & Ubiquitty::Screen::osc133MarkCmdFailed);

    // Command 2 succeeds (exit 0) — executed but not failed.
    feed(t, "\033]133;A\007$ true\r\n\033]133;C\007\r\n\033]133;D;0\007");
    const int p2 = base + 2;
    CHECK(Ubiquitty::Screen::rowMark(&t, p2) & Ubiquitty::Screen::osc133MarkCmdExecuted);
    CHECK((Ubiquitty::Screen::rowMark(&t, p2) & Ubiquitty::Screen::osc133MarkCmdFailed) == 0);
}

TEST(testOSC133FailedCommandMarksScrolledPrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Prompt + C on row 0, then push it fully into history before D.
    feed(t, "\033]133;A\007$ slow\r\n\033]133;C\007");
    for (int i = 0; i < t.rows; i++)
        feed(t, "\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    feed(t, "\033]133;D;1\007");
    // History absolute row 0 is the prompt; it carries both bits.
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdExecuted);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdFailed);
}

TEST(testOSC133MarkerFollowsRowIntoScrollback)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    // Mark row 0 as prompt-start, then push it off the top of the
    // screen via newlines. Once captured into scrollback, the marker
    // rides into history absolute row 0 (oldest).
    feed(t, "\033]133;A\007hello");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    for (i = 0; i < t.rows; i++)
        feed(t, "\n");
    // Exactly one row scrolled out — it landed at history absolute row 0.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Live row 0 is now blank — marker cleared.
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == 0);
}

TEST(testOSC133AlternateScreenDoesNotSetMarker)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Enter alternate screen (DECSET 1049).
    feed(t, "\033[?1049h");
    feed(t, "\033]133;A\007");
    // Markers untouched on either side.
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == 0);
    feed(t, "\033[?1049l");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == 0);
}

TEST(testOSCMarksSpec133AbcdBel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\r\n\033]133;B\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdStart);
    feed(t, "\r\n\033]133;C\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkOutputStart);
    feed(t, "\r\n\033]133;D\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
}

TEST(testOSCMarksSpec133AbcdSt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\r\n\033]133;B\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdStart);
    feed(t, "\r\n\033]133;C\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkOutputStart);
    feed(t, "\r\n\033]133;D\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
}

TEST(testOSCMarksSpec633Abcd)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]633;A\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\r\n\033]633;B\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdStart);
    feed(t, "\r\n\033]633;C\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkOutputStart);
    feed(t, "\r\n\033]633;D\033\\");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
}

TEST(testOSCMarksSpec1337Setmark)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;SetMark\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;SetMark\033\\");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSCMarksSpecOrCombination)
{
    // B then C → CMD_START | OUTPUT_START.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;B\007\033]133;C\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == (Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart));
    }
    // Reverse order yields the same combined value.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;C\007\033]133;B\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == (Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart));
    }
    // A + B + C accumulate across families (133 + 633 + 1337).
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;SetMark\007\033]633;B\007\033]133;C\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == (Ubiquitty::Screen::osc133MarkPromptStart | Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart));
    }
    // All bits on one row. A then C on the same row promotes CMD_EXECUTED onto
    // it as well, so the prompt-start row carries five bits, not four.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;A\007\033]133;B\007\033]133;C\007\033]133;D\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == (Ubiquitty::Screen::osc133MarkPromptStart | Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart | Ubiquitty::Screen::osc133MarkCmdEnd | Ubiquitty::Screen::osc133MarkCmdExecuted));
    }
}

TEST(testOSCMarksSpecDExitCode)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;D\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;D;0\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;D;127\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]633;D;42\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkCmdEnd);
    }
}

TEST(testOSCMarksSpecATrailingParameters)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;A;aid=foo;cl=m\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]633;A;cmdline\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSCMarksSpecUnknownSubcommandNoop)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;Z\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]633;Q\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    // Unknown subcommand must not clobber an existing mark on the row.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;A\007");
        const int abs = markCursorAbsoluteRow(t);
        CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
        feed(t, "\033]133;Z\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
        feed(t, "\033]633;Q\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSCMarksSpec633EPNoMark)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]633;E;somecmd;nonce\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]633;P;CurrentDirectory=/tmp\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    // E / P must not clobber a prior mark either.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]133;A\007");
        const int abs = markCursorAbsoluteRow(t);
        feed(t, "\033]633;E;ls -l\007\033]633;P;CurrentDirectory=/home\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSCMarksSpec1337NonSetmarkNoMark)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;CurrentDir=/tmp\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;RemoteHost=user@host\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]1337;ShellIntegrationVersion=12;shell=zsh\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    }
}

TEST(testOSCMarksSpecAlternateScreenPreservesPrimary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007");
    const int primaryAbsoluteRow = markCursorAbsoluteRow(t);
    CHECK(Ubiquitty::Screen::rowMark(&t, primaryAbsoluteRow) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Enter alternate screen, emit a mark on the alternate row: nothing recorded.
    feed(t, "\033[?1049h");
    feed(t, "\033]133;A\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
    // Return to primary; the original mark survived underneath.
    feed(t, "\033[?1049l");
    CHECK(Ubiquitty::Screen::rowMark(&t, primaryAbsoluteRow) == Ubiquitty::Screen::osc133MarkPromptStart);
}

TEST(testOSCMarksSpecAlternateScreen471047)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[?47h");
        feed(t, "\033]133;A\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
        feed(t, "\033[?47l");
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[?1047h");
        feed(t, "\033]133;A\007");
        CHECK(Ubiquitty::Screen::rowMark(&t, markCursorAbsoluteRow(t)) == 0);
        feed(t, "\033[?1047l");
    }
}

TEST(testOSCMarksSpecScrollNewlineTracksRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007hello");
    // Mark is on live row 0 (no history yet → absolute 0).
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Walk it up the live screen without scrolling out yet.
    feed(t, "\r\nrow1\r\nrow2");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // A blank row scrolled in below the cursor has no mark.
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + (t.rows - 1)) == 0);
    // Now push it off the top into scrollback; the mark rides to history.
    for (int i = 0; i < t.rows; i++)
        feed(t, "\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // The current live row 0 is blank — no mark.
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == 0);
}

TEST(testOSC133WrappedMarkedRowsSurviveScrollback)
{
    Ubiquitty::Terminal t;
    const Ubiquitty::Screen::ScrollLine* line;
    int i;

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "\033]133;A\007ABCDE");
    feed(t, "F");
    feed(t, "\r\nrow1\r\nrow2\r\nrow3");

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);
    line = nullptr;
    for (i = 0; i < Ubiquitty::Screen::scrollbackCount(&t); i++) {
        const Ubiquitty::Screen::ScrollLine* candidate = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (candidate && candidate->length >= 5 && candidate->cells[0].codePoint == 'A') {
            line = candidate;
            break;
        }
    }
    CHECK(line != nullptr);
    if (line) {
        CHECK(line->cells[4].codePoint == 'E');
        CHECK(line->wrapped);
        CHECK(line->mark == Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSCMarksSpecScrollRegionUp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Region rows 2..5 (1-based DECSTBM → 0-based rows 1..4).
    feed(t, "\033[2;5r");
    // Home cursor, walk to region top (row index 1) and mark it.
    feed(t, "\033[2;1H\033]133;B\007");
    const int base = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 1) == Ubiquitty::Screen::osc133MarkCmdStart);
    // Move to region bottom (row index 4) and emit LF → region scrolls up
    // by one; the mark that was on row 1 moves to row 0... but row 0 is
    // above the region top, so the row scrolled past the top margin is
    // discarded: the mark leaves the buffer.
    feed(t, "\033[5;1H\n");
    // The marked content scrolled off the region top: rows 1..4 no longer
    // carry CMD_START.
    for (int r = 1; r <= 4; r++)
        CHECK((Ubiquitty::Screen::rowMark(&t, base + r) & Ubiquitty::Screen::osc133MarkCmdStart) == 0);
}

TEST(testOSCMarksSpecScrollRegionDown)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2;5r");
    // Mark region row index 2 (third visible row).
    feed(t, "\033[3;1H\033]133;C\007");
    const int base = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 2) == Ubiquitty::Screen::osc133MarkOutputStart);
    // Cursor to region top, RI scrolls the region down by one.
    feed(t, "\033[2;1H\033M");
    // The mark rode down to row index 3.
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 3) == Ubiquitty::Screen::osc133MarkOutputStart);
    // The blanked-in region-top row (index 1) has no mark.
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 1) == 0);
}

TEST(testOSCMarksSpecInsertLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Mark row index 3.
    feed(t, "\033[4;1H\033]133;A\007");
    const int base = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 3) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Insert two lines at row index 1 → the marked row slides to index 5.
    feed(t, "\033[2;1H\033[2L");
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 5) == Ubiquitty::Screen::osc133MarkPromptStart);
    // The two inserted blank rows (indices 1,2) carry no mark.
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 1) == 0);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 2) == 0);
}

TEST(testOSCMarksSpecDeleteLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Mark row index 4.
    feed(t, "\033[5;1H\033]133;A\007");
    const int base = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 4) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Delete two lines at row index 1 → the marked row slides up to index 2.
    feed(t, "\033[2;1H\033[2M");
    CHECK(Ubiquitty::Screen::rowMark(&t, base + 2) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Row index 4 no longer carries the mark.
    CHECK((Ubiquitty::Screen::rowMark(&t, base + 4) & Ubiquitty::Screen::osc133MarkPromptStart) == 0);
}

TEST(testOSCMarksSpecEraseDisplayClearsMarks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007hello");
    const int abs = markCursorAbsoluteRow(t);
    CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\033[2J");
    // The mark dies with the erased cells.
    CHECK(Ubiquitty::Screen::rowMark(&t, abs) == 0);
}

TEST(testOSCMarksSpecEraseLineObserved)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007hello");
    const int abs = markCursorAbsoluteRow(t);
    CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\033[2K");
    // Observed: 2K leaves the mark in place.
    CHECK(Ubiquitty::Screen::rowMark(&t, abs) == Ubiquitty::Screen::osc133MarkPromptStart);
}

TEST(testOSCMarksSpecEraseScrollback3j)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007hist");
    for (int i = 0; i < t.rows; i++)
        feed(t, "\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    feed(t, "\033[3J");
    // Scrollback gone → no history rows carry marks.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == 0);
}

TEST(testOSCMarksSpecClearScrollbackClearsHistoryMarks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007p0\r\n");
    feed(t, "\033]133;B\007p1\r\n");
    for (int i = 0; i < t.rows; i++)
        feed(t, "\n");
    const int history = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(history >= 2);
    bool hasAnyMark = false;
    for (int r = 0; r < history; r++)
        if (Ubiquitty::Screen::rowMark(&t, r) != 0)
            hasAnyMark = true;
    CHECK(hasAnyMark);
    Ubiquitty::Screen::dropScrollback(&t);
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    for (int r = 0; r < history; r++)
        CHECK(Ubiquitty::Screen::rowMark(&t, r) == 0);
}

TEST(testOSCMarksSpecWidthReflowKeepsMark)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    // Row 0: a marked long line that will wrap when narrowed.
    feed(t, "\033]133;A\007");
    feed(t, "ABCDEFGHIJKLMNOPQR"); // 18 chars, fits at 20
    feed(t, "\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5"); // scroll the marked line into history
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkPromptStart);
    /// Narrow so the line wraps, then widen so it rejoins. The mark must
    /// remain on the line's first history row throughout.
    Ubiquitty::resize(t, 4, 10);
    int markedRow = -1;
    for (int r = 0; r < Ubiquitty::Screen::scrollbackCount(&t); r++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, r);
        if (line && line->length > 0 && line->cells[0].codePoint == 'A') {
            markedRow = r;
            break;
        }
    }
    CHECK(markedRow >= 0);
    if (markedRow >= 0)
        CHECK(Ubiquitty::Screen::rowMark(&t, markedRow) & Ubiquitty::Screen::osc133MarkPromptStart);
    Ubiquitty::resize(t, 4, 20);
    markedRow = -1;
    for (int r = 0; r < Ubiquitty::Screen::scrollbackCount(&t); r++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, r);
        if (line && line->length > 0 && line->cells[0].codePoint == 'A') {
            markedRow = r;
            break;
        }
    }
    CHECK(markedRow >= 0);
    if (markedRow >= 0)
        CHECK(Ubiquitty::Screen::rowMark(&t, markedRow) & Ubiquitty::Screen::osc133MarkPromptStart);
}

TEST(testOSCMarksSpecHeightResizeTracksMark)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 20);
    // Mark live row 0, fill rows so a shrink evicts it into history.
    feed(t, "\033]133;A\007TOP");
    feed(t, "\r\nR1\r\nR2\r\nR3\r\nR4\r\nR5");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    /// Shrink height → top rows evicted into scrollback; the marked row's
    /// record carries the mark.
    Ubiquitty::resize(t, 3, 20);
    int markedHist = -1;
    for (int r = 0; r < Ubiquitty::Screen::scrollbackCount(&t); r++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, r);
        if (line && line->length > 0 && line->cells[0].codePoint == 'T') {
            markedHist = r;
            break;
        }
    }
    CHECK(markedHist >= 0);
    if (markedHist >= 0) {
        CHECK(Ubiquitty::Screen::rowMark(&t, markedHist) & Ubiquitty::Screen::osc133MarkPromptStart);
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, markedHist);
        CHECK(line && (line->mark & Ubiquitty::Screen::osc133MarkPromptStart));
    }
    /// Grow tall enough to pull TOP back into the live screen; the mark
    /// lands on its live row, addressed via the absolute index.
    int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);
    Ubiquitty::resize(t, 3 + historyBefore + 2, 20);
    int liveRow = -1;
    for (int r = 0; r < t.rows; r++) {
        const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, r, 0);
        if (c && c->codePoint == 'T') {
            liveRow = r;
            break;
        }
    }
    CHECK(liveRow >= 0);
    if (liveRow >= 0)
        CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + liveRow) & Ubiquitty::Screen::osc133MarkPromptStart);
}

TEST(testOSCMarksSpecScrollbackRoundtripRecord)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A\007MARKED");
    for (int i = 0; i < t.rows; i++)
        feed(t, "\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    // History absolute row 0 (oldest) is the marked line.
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    const Ubiquitty::Screen::ScrollLine* rec = Ubiquitty::Screen::scrollbackLine(&t, 0);
    CHECK(rec != nullptr);
    if (rec) {
        CHECK(rec->cells[0].codePoint == 'M');
        CHECK(rec->mark & Ubiquitty::Screen::osc133MarkPromptStart);
    }
}

TEST(testOSC133AbsoluteRowAccessors)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    // Mark row 0 as prompt; live screen → rowMark on absolute row 0
    // (no history yet) returns the bit.
    feed(t, "\033]133;A\007hello");
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    // Push the marked row into scrollback; the marker rides along, the
    // absolute row index for the marked content stays 0.
    for (i = 0; i < t.rows; i++)
        feed(t, "\n");
    CHECK(t.scrollbackCount >= 1);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) == Ubiquitty::Screen::osc133MarkPromptStart);
    /// findPromptMarkBefore(absoluteRowExclusive) walks back from absoluteRow-1.
    /// Looking back from the live screen's top row finds absolute row 0.
    const int liveTop = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, liveTop, Ubiquitty::Screen::osc133MarkPromptStart) == 0);
    // No mark before absolute row 0 itself.
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, 0, Ubiquitty::Screen::osc133MarkPromptStart) == -1);
    // Add a second prompt on the current cursor row (somewhere in the
    // live screen). Searching back from one past it finds that newer
    // mark; searching back from the second prompt's row finds the older
    // (absolute row 0). The prompt starts on a fresh line, which scrolls
    // here, so its row is read after it.
    feed(t, "\033]133;A\007world");
    const int secondAbsoluteRow = Ubiquitty::Screen::scrollbackCount(&t) + t.screens.active->cursorY;
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, secondAbsoluteRow + 1, Ubiquitty::Screen::osc133MarkPromptStart) == secondAbsoluteRow);
    CHECK(Ubiquitty::Screen::findPromptMarkBefore(&t, secondAbsoluteRow, Ubiquitty::Screen::osc133MarkPromptStart) == 0);
}

TEST(testOSC52WritePath)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    t.clipboardClient = &clip;

    // "Hello World" → base64 "SGVsbG8gV29ybGQ=".
    feed(t, "\033]52;c;SGVsbG8gV29ybGQ=\033\\");
    CHECK(clip.setCalls == 1);
    CHECK(clip.capturedLength == 11);
    CHECK(memcmp(clip.captured, "Hello World", 11) == 0);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    // Empty data clears the clipboard.
    feed(t, "\033]52;c;\033\\");
    CHECK(clip.setCalls == 2);
    CHECK(clip.capturedLength == 0);

    // A multi-selector list "pq" writes each named distinct buffer (primary +
    // secondary) → +2 calls. "QQ==" = "A".
    feed(t, "\033]52;pq;QQ==\033\\");
    CHECK(clip.setCalls == 4);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'A');
    CHECK(clip.store[(int)Ubiquitty::ClipboardBuffer::Primary][0] == 'A');
    CHECK(clip.store[(int)Ubiquitty::ClipboardBuffer::Secondary][0] == 'A');

    // Data that is not base64 (`@` is not in the alphabet) clears the
    // selection, as xterm's ctlseqs gives it.
    feed(t, "\033]52;c;@@@@\033\\");
    CHECK(clip.setCalls == 5);
    CHECK(clip.capturedLength == 0);

    t.clipboardClient = nullptr;
}

TEST(testOSC52SelectorRouting)
{
    using B = Ubiquitty::ClipboardBuffer;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    t.clipboardClient = &clip;
    // Pin the gates so a prior test's Deny/kill can't bleed in.
    t.isOSC52Allowed = true;
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // c → clipboard. "QQ==" = "A".
    feed(t, "\033]52;c;QQ==\033\\");
    CHECK(clip.setCalls == 1);
    CHECK(clip.lastBuffer == B::Clipboard);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'A');

    // p (primary) → its own distinct buffer, not dropped, not the clipboard.
    feed(t, "\033]52;p;Qg==\033\\");
    CHECK(clip.setCalls == 2);
    CHECK(clip.lastBuffer == B::Primary);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'B');

    // Empty Pc defaults to "s0" (xterm) → the select buffer and cut buffer 0,
    // two distinct buffers → +2 writes.
    feed(t, "\033]52;;Qw==\033\\");
    CHECK(clip.setCalls == 4);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'C');
    CHECK(clip.store[(int)B::Select][0] == 'C');
    CHECK(clip.store[(int)B::Cut0][0] == 'C');

    // Multi-selector list "pc" writes each named buffer once → +2 calls.
    feed(t, "\033]52;pc;RA==\033\\");
    CHECK(clip.setCalls == 6);
    CHECK(clip.store[(int)B::Primary][0] == 'D');
    CHECK(clip.store[(int)B::Clipboard][0] == 'D');

    // Cut buffers 0-7 are also legal selectors → their own stores.
    feed(t, "\033]52;7;RQ==\033\\");
    CHECK(clip.setCalls == 7);
    CHECK(clip.lastBuffer == B::Cut7);
    CHECK(clip.captured[0] == 'E');

    // Unknown selector (`x` is not in cpqs01234567) — request invalid,
    // no write, no crash. Cut buffer 8 ('8') is likewise out of range.
    feed(t, "\033]52;x;Rg==\033\\");
    CHECK(clip.setCalls == 7);
    feed(t, "\033]52;8;Rw==\033\\");
    CHECK(clip.setCalls == 7);
    // A list with one bad char poisons the whole list (xterm-style).
    feed(t, "\033]52;cx;SA==\033\\");
    CHECK(clip.setCalls == 7);

    // The master kill-switch blocks all buffers (no bypass channel).
    t.isOSC52Allowed = false;
    feed(t, "\033]52;c;SQ==\033\\");
    feed(t, "\033]52;p;SQ==\033\\");
    feed(t, "\033]52;pc;SQ==\033\\");
    feed(t, "\033]52;;SQ==\033\\");
    feed(t, "\033]52;3;SQ==\033\\");
    CHECK(clip.setCalls == 7);
    t.isOSC52Allowed = true;

    // The write-Deny policy also blocks all buffers.
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    feed(t, "\033]52;c;SQ==\033\\");
    feed(t, "\033]52;p;SQ==\033\\");
    feed(t, "\033]52;pc;SQ==\033\\");
    feed(t, "\033]52;;SQ==\033\\");
    feed(t, "\033]52;5;SQ==\033\\");
    CHECK(clip.setCalls == 7);
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    t.clipboardClient = nullptr;
}

TEST(testOSC1337Copy)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    t.clipboardClient = &clip;
    // The default write policy is Allow; pin it so a prior test's Deny
    // can't bleed in.
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // "Hello World" → base64 "SGVsbG8gV29ybGQ=".
    feed(t, "\033]1337;Copy=:SGVsbG8gV29ybGQ=\007");
    CHECK(clip.setCalls == 1);
    CHECK(clip.capturedLength == 11);
    CHECK(memcmp(clip.captured, "Hello World", 11) == 0);

    // ST terminator works too.
    feed(t, "\033]1337;Copy=:QQ==\033\\");
    CHECK(clip.setCalls == 2);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'A');

    // Empty payload clears the clipboard.
    feed(t, "\033]1337;Copy=:\007");
    CHECK(clip.setCalls == 3);
    CHECK(clip.capturedLength == 0);

    // Malformed base64 is dropped silently — no set call, no crash.
    feed(t, "\033]1337;Copy=:@@@@\007");
    CHECK(clip.setCalls == 3);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    // Deny policy blocks the write entirely (same gate as OSC 52).
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    feed(t, "\033]1337;Copy=:SGk=\007");
    CHECK(clip.setCalls == 3);
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // The OSC 52 master kill-switch also disables Copy= (no bypass channel).
    t.isOSC52Allowed = false;
    feed(t, "\033]1337;Copy=:SGk=\007");
    CHECK(clip.setCalls == 3);
    t.isOSC52Allowed = true;

    t.clipboardClient = nullptr;
}

TEST(testOSC52QueryPath)
{
    Ubiquitty::Terminal t;
    char buffer[64];
    int n;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    clip.getValue = "Hi";
    clip.getLength = 2;
    t.clipboardClient = &clip;
    // The default read policy is Prompt; pin Allow for this inline-reply test.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]52;c;?\033\\");
    n = drain(t, buffer);
    // Expect framed reply: ESC ] 52 ; c ; SGk= ESC `\`
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]52;c;SGk=\033\\") != nullptr);

    // A query with a legal non-clipboard selector reads that buffer (here the
    // single getValue fixture backs every buffer); the selector echoes back.
    feed(t, "\033]52;p;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]52;p;SGk=\033\\") != nullptr);

    // A query with an unknown selector is invalid — no read, no reply,
    // so the clipboard can't be exfiltrated through a garbage selector.
    feed(t, "\033]52;x;?\033\\");
    n = drain(t, buffer);
    CHECK(n == 0);

    t.clipboardClient = nullptr;
}

TEST(testOSC52ReadLargeClipboard)
{
    Ubiquitty::Terminal t;
    char buffer[2048];
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    static char big[600];
    for (int i = 0; i < (int)sizeof(big); ++i)
        big[i] = static_cast<char>('A' + (i % 26));
    clip.getValue = big;
    clip.getLength = (int)sizeof(big);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]52;c;?\033\\");
    int n = drain(t, buffer);
    CHECK(n > 0); // a reply was sent (no hang)
    CHECK(strncmp(buffer, "\033]52;c;", 7) == 0); // well-formed framing
    CHECK(strstr(buffer, "\033\\") != nullptr); // terminated
    // Non-empty data field: the clipboard was actually encoded, not the empty
    // fallback. base64 of bytes 'A','B','C',... begins "QUJD".
    CHECK(strstr(buffer, "\033]52;c;QUJD") != nullptr);

    t.clipboardClient = nullptr;
}

TEST(testOSC52PrefGate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    clip.getValue = "Hi";
    clip.getLength = 2;
    t.clipboardClient = &clip;
    t.isOSC52Allowed = false;

    feed(t, "\033]52;c;SGk=\033\\");
    CHECK(clip.setCalls == 0); // write must be dropped

    {
        char buffer[64];
        int n;
        feed(t, "\033]52;c;?\033\\");
        n = drain(t, buffer);
        CHECK(n == 0); // query must not respond
    }

    t.isOSC52Allowed = true;
    t.clipboardClient = nullptr;
}

TEST(testOSC52PerTermOverride)
{
    Ubiquitty::Terminal tA, tB;
    Ubiquitty::hardReset(tA);
    Ubiquitty::hardReset(tB);
    CaptureClipboard clipA, clipB;
    tA.clipboardClient = &clipA;
    tB.clipboardClient = &clipB;

    // "A" → base64 "QQ==" routed to Ubiquitty::Terminal A only.
    feed(tA, "\033]52;c;QQ==\033\\");
    CHECK(clipA.capturedLength == 1);
    CHECK(clipA.captured[0] == 'A');
    CHECK(clipB.capturedLength == -1);

    // "B" → "Qg==" routed to Ubiquitty::Terminal B only.
    feed(tB, "\033]52;c;Qg==\033\\");
    CHECK(clipB.capturedLength == 1);
    CHECK(clipB.captured[0] == 'B');

    tA.clipboardClient = nullptr;
    tB.clipboardClient = nullptr;
}

TEST(testOSC52Multibuffer)
{
    using B = Ubiquitty::ClipboardBuffer;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    t.clipboardClient = &clip;
    t.isOSC52Allowed = true;
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // Write 'p' (primary) "P" → base64 "UA==", then 'c' (clipboard) "C" →
    // "Qw==". They land in DISTINCT buffers.
    feed(t, "\033]52;p;UA==\033\\");
    CHECK(clip.lastBuffer == B::Primary);
    CHECK(clip.storeLength[(int)B::Primary] == 1);
    CHECK(clip.store[(int)B::Primary][0] == 'P');

    feed(t, "\033]52;c;Qw==\033\\");
    CHECK(clip.lastBuffer == B::Clipboard);
    CHECK(clip.storeLength[(int)B::Clipboard] == 1);
    CHECK(clip.store[(int)B::Clipboard][0] == 'C');

    // Primary is UNTOUCHED by the clipboard write — they are distinct.
    CHECK(clip.store[(int)B::Primary][0] == 'P');
    CHECK(clip.storeLength[(int)B::Primary] == 1);

    // Read 'p' returns the primary buffer, distinct from 'c'. Stage a per-
    // buffer read fixture and exercise the Allow inline-read path.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    clip.getStore[(int)B::Primary] = "P";
    clip.getStoreLength[(int)B::Primary] = 1;
    clip.getStore[(int)B::Clipboard] = "C";
    clip.getStoreLength[(int)B::Clipboard] = 1;

    char buffer[64];
    feed(t, "\033]52;p;?\033\\");
    int n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(clip.lastReadBuffer == B::Primary);
    // "P" → base64 "UA==". The reply echoes the 'p' selector.
    CHECK(strstr(buffer, "\033]52;p;UA==\033\\") != nullptr);

    feed(t, "\033]52;c;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(clip.lastReadBuffer == B::Clipboard);
    // "C" → base64 "Qw==".
    CHECK(strstr(buffer, "\033]52;c;Qw==\033\\") != nullptr);

    /// A multi-selector SET writes each named buffer once. "pc" with "M" →
    /// "TQ==" writes both Primary and Clipboard.
    int before = clip.setCalls;
    feed(t, "\033]52;pc;TQ==\033\\");
    CHECK(clip.setCalls == before + 2); // one write per distinct buffer
    CHECK(clip.storeLength[(int)B::Primary] == 1);
    CHECK(clip.store[(int)B::Primary][0] == 'M');
    CHECK(clip.storeLength[(int)B::Clipboard] == 1);
    CHECK(clip.store[(int)B::Clipboard][0] == 'M');

    // A cut buffer routes to its own store. '3' "X" → "WA==".
    feed(t, "\033]52;3;WA==\033\\");
    CHECK(clip.lastBuffer == B::Cut3);
    CHECK(clip.storeLength[(int)B::Cut3] == 1);
    CHECK(clip.store[(int)B::Cut3][0] == 'X');

    t.clipboardClient = nullptr;
}

TEST(testOSC52MultibufferGates)
{
    using B = Ubiquitty::ClipboardBuffer;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    clip.getStore[(int)B::Primary] = "secret";
    clip.getStoreLength[(int)B::Primary] = 6;
    clip.getStore[(int)B::Cut0] = "secret";
    clip.getStoreLength[(int)B::Cut0] = 6;
    t.clipboardClient = &clip;

    // Master kill-switch blocks writes and reads to every buffer.
    t.isOSC52Allowed = false;
    feed(t, "\033]52;p;UA==\033\\"); // write primary
    feed(t, "\033]52;0;UA==\033\\"); // write cut 0
    CHECK(clip.setCalls == 0);
    {
        char buffer[64];
        feed(t, "\033]52;p;?\033\\"); // read primary
        CHECK(drain(t, buffer) == 0);
        feed(t, "\033]52;0;?\033\\"); // read cut 0
        CHECK(drain(t, buffer) == 0);
    }
    t.isOSC52Allowed = true;

    // Write Deny blocks writes to every buffer (not just the clipboard).
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    feed(t, "\033]52;p;UA==\033\\");
    feed(t, "\033]52;5;UA==\033\\");
    feed(t, "\033]52;pc;UA==\033\\");
    CHECK(clip.setCalls == 0);
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // Read Deny blocks remote reads of every buffer: a well-formed but EMPTY
    // reply (never the secret). The primary/cut buffers are not a bypass.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
    {
        char buffer[64];
        feed(t, "\033]52;p;?\033\\");
        int n = drain(t, buffer);
        CHECK(n > 0); // framed reply (no hang)
        CHECK(strstr(buffer, "\033]52;p;\033\\") != nullptr); // empty data field
        CHECK(strstr(buffer, "secret") == nullptr); // never leaks the buffer

        feed(t, "\033]52;0;?\033\\");
        n = drain(t, buffer);
        CHECK(n > 0);
        CHECK(strstr(buffer, "secret") == nullptr);
    }

    t.clipboardClient = nullptr;
}

TEST(testOSC52ChunkedWrite)
{
    // "Hello World" base64 "SGVsbG8gV29ybGQ=" split across 3 chunks. Each
    // non-final chunk carries the marker '!'; the final one does not. The
    // split lands mid-group and across the padding boundary on purpose.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        BigClipboard clip;
        t.clipboardClient = &clip;

        feed(t, "\033]52;c;SGVs!\033\\"); // chunk 1
        CHECK(clip.setCalls == 0); // nothing applied yet
        feed(t, "\033]52;c;bG8g!\033\\"); // chunk 2
        CHECK(clip.setCalls == 0);
        feed(t, "\033]52;c;V29ybGQ=\033\\"); // terminator (no marker)
        CHECK(clip.setCalls == 1);
        CHECK(clip.capturedLength == 11);
        CHECK(memcmp(clip.captured, "Hello World", 11) == 0);

        t.clipboardClient = nullptr;
    }

    // Padding across a chunk boundary: "A" → "QQ==". Split the base64 mid-
    // group ("QQ" | "==") so the '=' padding lands in a separate chunk from
    // its data sextets. Must still decode to the single byte 'A'.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        BigClipboard clip;
        t.clipboardClient = &clip;

        feed(t, "\033]52;c;QQ!\033\\"); // "QQ" + marker
        CHECK(clip.setCalls == 0);
        feed(t, "\033]52;c;==\033\\"); // "==" terminator
        CHECK(clip.setCalls == 1);
        CHECK(clip.capturedLength == 1);
        CHECK(clip.captured[0] == 'A');

        // Single '=' across a boundary too: "Man" → "TWFu" (no pad), and
        // "Ma" → "TWE=" split as "TW" | "E=".
        feed(t, "\033]52;c;TW!\033\\");
        feed(t, "\033]52;c;E=\033\\");
        CHECK(clip.setCalls == 2);
        CHECK(clip.capturedLength == 2);
        CHECK(memcmp(clip.captured, "Ma", 2) == 0);

        t.clipboardClient = nullptr;
    }

    // Chunked result is byte-identical to the single-shot form for a large
    // payload (12 KiB of pseudo-random bytes), with the base64 split at an
    // arbitrary point that crosses a 4-char group boundary.
    {
        const int rawByteCount = 12 * 1024;
        static char raw[12 * 1024];
        for (int i = 0; i < rawByteCount; i++)
            raw[i] = (char)((i * 31 + 7) & 0xFF);

        /// Reference: single-shot via the encoder is awkward here, so build
        /// the base64 with the same alphabet the decoder accepts.
        static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        static char b64[(12 * 1024 + 2) / 3 * 4 + 4];
        int o = 0, i = 0;
        while (i + 2 < rawByteCount) {
            unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
            b64[o++] = A[(v >> 18) & 63];
            b64[o++] = A[(v >> 12) & 63];
            b64[o++] = A[(v >> 6) & 63];
            b64[o++] = A[v & 63];
            i += 3;
        }
        int rem = rawByteCount - i;
        if (rem == 1) {
            unsigned v = (unsigned char)raw[i] << 16;
            b64[o++] = A[(v >> 18) & 63];
            b64[o++] = A[(v >> 12) & 63];
            b64[o++] = '=';
            b64[o++] = '=';
        } else if (rem == 2) {
            unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8);
            b64[o++] = A[(v >> 18) & 63];
            b64[o++] = A[(v >> 12) & 63];
            b64[o++] = A[(v >> 6) & 63];
            b64[o++] = '=';
        }
        int b64Length = o;

        /// Single-shot baseline.
        Ubiquitty::Terminal ts;
        Ubiquitty::hardReset(ts);
        BigClipboard single;
        ts.clipboardClient = &single;
        {
            static char seq[2 + 32 + sizeof(b64)];
            int n = snprintf(seq, sizeof(seq), "\033]52;c;%.*s\033\\", b64Length, b64);
            (void)n;
            feed(ts, seq);
        }
        CHECK(single.setCalls == 1);
        CHECK(single.capturedLength == rawByteCount);
        CHECK(memcmp(single.captured, raw, (size_t)rawByteCount) == 0);
        ts.clipboardClient = nullptr;

        /// Chunked: split base64 at 5000 (not a multiple of 4 — padding/group
        /// boundary lands mid-chunk).
        Ubiquitty::Terminal tc;
        Ubiquitty::hardReset(tc);
        BigClipboard chunked;
        tc.clipboardClient = &chunked;
        int split = 5000;
        {
            static char seq[5000 + 32];
            snprintf(seq, sizeof(seq), "\033]52;c;%.*s!\033\\", split, b64);
            feed(tc, seq);
        }
        CHECK(chunked.setCalls == 0);
        {
            static char seq[sizeof(b64) + 32];
            snprintf(seq, sizeof(seq), "\033]52;c;%.*s\033\\", b64Length - split, b64 + split);
            feed(tc, seq);
        }
        CHECK(chunked.setCalls == 1);
        CHECK(chunked.capturedLength == rawByteCount);
        CHECK(memcmp(chunked.captured, raw, (size_t)rawByteCount) == 0);
        // Pending state fully reset after apply.
        CHECK(tc.osc52ChunkActive == false);
        CHECK(tc.osc52Chunk.length == 0);
        tc.clipboardClient = nullptr;
    }
}

TEST(testOSC52ChunkedOverflow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    BigClipboard clip;
    t.clipboardClient = &clip;

    /// One marked chunk of 8 KiB base64 per iteration; maxOSC52ChunkSize is
    /// 1 MiB, so ~128 iterations overflow. Feed 200 to be safe.
    static char chunk[8 * 1024 + 16];
    int body = 8 * 1024;
    const char* pre = "\033]52;c;";
    int n = (int)strlen(pre);
    memcpy(chunk, pre, (size_t)n);
    memset(chunk + n, 'A', (size_t)body);
    chunk[n + body] = '!';
    chunk[n + body + 1] = '\033';
    chunk[n + body + 2] = '\\';
    chunk[n + body + 3] = 0;

    for (int i = 0; i < 200; i++)
        feed(t, chunk);

    // Overflow tripped: transfer poisoned (still active, swallowing the
    // stream), accumulated bytes dropped, nothing applied.
    CHECK(clip.setCalls == 0);
    CHECK(t.osc52ChunkActive == true);
    CHECK(t.osc52ChunkOverflow == true);
    CHECK(t.osc52Chunk.length == 0);

    // The terminating (unmarked) SET ends the doomed transfer — no apply,
    // state fully reset.
    feed(t, "\033]52;c;QQ==\033\\");
    CHECK(clip.setCalls == 0);
    CHECK(t.osc52ChunkActive == false);
    CHECK(t.osc52ChunkOverflow == false);

    // A subsequent single-shot SET now works (no lingering damage).
    feed(t, "\033]52;c;Qg==\033\\");
    CHECK(clip.setCalls == 1);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'B');

    t.clipboardClient = nullptr;
}

TEST(testOSC52ChunkedInterleave)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    BigClipboard clip;
    t.clipboardClient = &clip;

    // Start a chunked "c" transfer, then a single-shot "p" SET interrupts.
    feed(t, "\033]52;c;SGVs!\033\\"); // "c" chunk in flight
    CHECK(t.osc52ChunkActive == true);
    feed(t, "\033]52;p;QQ==\033\\"); // "p" interrupts → its own apply
    CHECK(clip.setCalls == 1);
    CHECK(clip.capturedLength == 1);
    CHECK(clip.captured[0] == 'A');
    CHECK(t.osc52ChunkActive == false); // old "c" transfer abandoned

    // A pending transfer aborted by a read query (no apply of the chunk).
    feed(t, "\033]52;c;SGVs!\033\\");
    CHECK(t.osc52ChunkActive == true);
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
    feed(t, "\033]52;c;?\033\\"); // read aborts the transfer
    CHECK(t.osc52ChunkActive == false);

    t.clipboardClient = nullptr;
}

TEST(testOSC52ReadPolicy)
{
    using Policy = Ubiquitty::OSC::ClipboardReadPolicy;
    char buffer[64];
    int n;

    // String mapping.
    CHECK(Ubiquitty::OSC::clipboardReadPolicyFromString("allow") == Policy::Allow);
    CHECK(Ubiquitty::OSC::clipboardReadPolicyFromString("DENY") == Policy::Deny);
    CHECK(Ubiquitty::OSC::clipboardReadPolicyFromString("prompt") == Policy::Prompt);
    CHECK(Ubiquitty::OSC::clipboardReadPolicyFromString("bogus") == Policy::Prompt);
    CHECK(Ubiquitty::OSC::clipboardReadPolicyFromString(std::string_view {}) == Policy::Prompt);

    // --- Deny: no read, empty reply; SET still works. ---
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        PromptClipboard clip;
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Policy::Deny;

        feed(t, "\033]52;c;?\033\\");
        CHECK(clip.requestCalls == 0); // no prompt under Deny
        n = drain(t, buffer);
        if (n < (int)sizeof(buffer))
            buffer[n] = 0;
        // Empty data field, no clipboard contents in the reply.
        CHECK(n > 0);
        CHECK(strstr(buffer, "\033]52;c;\033\\") != nullptr);
        CHECK(strstr(buffer, "SGk=") == nullptr);

        // SET (write) is unaffected by the read policy.
        feed(t, "\033]52;c;SGk=\033\\");
        CHECK(clip.setCalls == 1);

        t.clipboardClient = nullptr;
    }

    // --- Allow: inline read + reply. ---
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        PromptClipboard clip;
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Policy::Allow;

        feed(t, "\033]52;c;?\033\\");
        CHECK(clip.requestCalls == 0); // no prompt under Allow
        n = drain(t, buffer);
        if (n < (int)sizeof(buffer))
            buffer[n] = 0;
        CHECK(n > 0);
        CHECK(strstr(buffer, "\033]52;c;SGk=\033\\") != nullptr);

        t.clipboardClient = nullptr;
    }

    // --- Prompt: fires the hook, defers the reply. ---
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        PromptClipboard clip;
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Policy::Prompt;

        feed(t, "\033]52;c;?\033\\");
        CHECK(clip.requestCalls == 1); // hook fired
        CHECK(strcmp(clip.requestSet, "c") == 0); // selection echoed
        n = drain(t, buffer);
        CHECK(n == 0); // no reply yet

        /// Frontend denies → empty reply, no contents.
        Ubiquitty::OSC::completeClipboardReadQuery(t, std::string_view(clip.requestSet, static_cast<std::size_t>(clip.requestSetLength)), Ubiquitty::OSC::ClipboardReadDecision::Refused);
        n = drain(t, buffer);
        if (n < (int)sizeof(buffer))
            buffer[n] = 0;
        CHECK(n > 0);
        CHECK(strstr(buffer, "\033]52;c;\033\\") != nullptr);
        CHECK(strstr(buffer, "SGk=") == nullptr);

        /// A second completion with nothing outstanding answers nothing: the
        /// query it would answer was already completed above, and an
        /// unsolicited reply is a reply the stream never asked for.
        Ubiquitty::OSC::completeClipboardReadQuery(t, std::string_view(clip.requestSet, static_cast<std::size_t>(clip.requestSetLength)), Ubiquitty::OSC::ClipboardReadDecision::Approved);
        CHECK(drain(t, buffer) == 0);

        /// A fresh query is answered normally.
        feed(t, "\033]52;c;?\033\\");
        CHECK(clip.requestCalls == 2);
        Ubiquitty::OSC::completeClipboardReadQuery(t, std::string_view(clip.requestSet, static_cast<std::size_t>(clip.requestSetLength)), Ubiquitty::OSC::ClipboardReadDecision::Approved);
        n = drain(t, buffer);
        if (n < (int)sizeof(buffer))
            buffer[n] = 0;
        CHECK(n > 0);
        CHECK(strstr(buffer, "\033]52;c;SGk=\033\\") != nullptr);

        t.clipboardClient = nullptr;
    }
}

TEST(testOSC52AsyncClipboardRead)
{
    using Policy = Ubiquitty::OSC::ClipboardReadPolicy;
    char buffer[64];
    int n;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    AsyncClipboard clip;
    t.clipboardClient = &clip;
    // Allow policy, but the client requires async capture: Core must route the
    // read through requestClipboardRead (not the inline reply), so the frontend
    // can stage the pasteboard on its UI thread first.
    t.osc52ClipboardReadPolicy = Policy::Allow;

    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requestCalls == 1); // async routing fired, not the inline path
    CHECK(strcmp(clip.requestSet, "c") == 0);
    n = drain(t, buffer);
    CHECK(n == 0); // no reply until the frontend stages + completes

    /// Frontend captured the pasteboard on main and staged it, then completes.
    const char hi[] = "Hi";
    clip.provideClipboardReadBytes(Ubiquitty::ClipboardBuffer::Clipboard, std::span<const char>(hi, 2));
    Ubiquitty::OSC::completeClipboardReadQuery(t, std::string_view(clip.requestSet, static_cast<std::size_t>(clip.requestSetLength)), Ubiquitty::OSC::ClipboardReadDecision::Approved);
    n = drain(t, buffer);
    if (n < (int)sizeof(buffer))
        buffer[n] = 0;
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]52;c;SGk=\033\\") != nullptr); // base64("Hi") == "SGk="
    CHECK(clip.hasStagedReply == false); // readClipboard consumed the one-shot staging

    t.clipboardClient = nullptr;
}

TEST(testOSC4SetIndexedPalette)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // rgb:RRRR/GGGG/BBBB form.
    feed(t, "\033]4;1;rgb:ffff/0000/8080\033\\");
    CHECK(t.palette.indexed[1].red == 0xFFFF);
    CHECK(t.palette.indexed[1].green == 0x0000);
    CHECK(t.palette.indexed[1].blue == 0x8080);

    // Short-form rgb:R/G/B replicates nibbles.
    feed(t, "\033]4;2;rgb:f/0/8\007");
    CHECK(t.palette.indexed[2].red == 0xFFFF);
    CHECK(t.palette.indexed[2].green == 0x0000);
    CHECK(t.palette.indexed[2].blue == 0x8888);

    /// #RRGGBB hex form; an 8-digit spec is malformed and dropped.
    Ubiquitty::PaletteRGB before15 = t.palette.indexed[15];
    feed(t, "\033]4;15;#10203040\033\\");
    CHECK(t.palette.indexed[15].red == before15.red);
    CHECK(t.palette.indexed[15].green == before15.green);
    CHECK(t.palette.indexed[15].blue == before15.blue);
    feed(t, "\033]4;15;#102030\033\\");
    CHECK(t.palette.indexed[15].red == 0x1010);
    CHECK(t.palette.indexed[15].green == 0x2020);
    CHECK(t.palette.indexed[15].blue == 0x3030);

    // #RGB short-form. X11 left-justifies each component into 16 bits rather
    // than replicating the nibble, and the 8-bit visual then quantizes: 'f' is
    // 0xf000 -> 0xf0 -> 0xf0f0, not 0xffff.
    feed(t, "\033]4;200;#f0a\033\\");
    CHECK(t.palette.indexed[200].red == 0xF0F0);
    CHECK(t.palette.indexed[200].green == 0x0000);
    CHECK(t.palette.indexed[200].blue == 0xA0A0);

    // Out-of-range index is dropped silently (index 255 is the cap; 999
    // must not write past the array).
    feed(t, "\033]4;999;rgb:0/0/0\033\\");

    // Chained pairs in a single OSC 4.
    feed(t, "\033]4;3;rgb:1/2/3;4;rgb:4/5/6\033\\");
    CHECK(t.palette.indexed[3].red == 0x1111);
    CHECK(t.palette.indexed[4].red == 0x4444);
    CHECK(t.palette.indexed[4].green == 0x5555);
    CHECK(t.palette.indexed[4].blue == 0x6666);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testOSC4QueryIndexedPalette)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    // Seed two indices, then query them back.
    feed(t, "\033]4;5;rgb:0505/0606/0707\033\\");
    feed(t, "\033]4;5;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]4;5;rgb:0505/0606/0707\033\\") != nullptr);

    // BEL terminator on the query, which the reply ends with too.
    feed(t, "\033]4;0;rgb:0000/0101/0202\033\\");
    feed(t, "\033]4;0;?\007");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]4;0;rgb:0000/0101/0202\007") != nullptr);
}

TEST(testOSC10DefaultForeground)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    // Set via rgb:.
    feed(t, "\033]10;rgb:1234/5678/9abc\033\\");
    CHECK(t.palette.defaultForeground.red == 0x1212);
    CHECK(t.palette.defaultForeground.green == 0x5656);
    CHECK(t.palette.defaultForeground.blue == 0x9A9A);

    // Set via #RRGGBB.
    feed(t, "\033]10;#804020\033\\");
    CHECK(t.palette.defaultForeground.red == 0x8080);
    CHECK(t.palette.defaultForeground.green == 0x4040);
    CHECK(t.palette.defaultForeground.blue == 0x2020);

    // Query → response framed with the same OSC code.
    feed(t, "\033]10;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]10;rgb:8080/4040/2020\033\\") != nullptr);

    // OSC 110 — reset reverts to the baseline default foreground.
    feed(t, "\033]110\033\\");
    CHECK(t.palette.defaultForeground.red == t.paletteBaseline.defaultForeground.red);
    CHECK(t.palette.defaultForeground.green == t.paletteBaseline.defaultForeground.green);
    CHECK(t.palette.defaultForeground.blue == t.paletteBaseline.defaultForeground.blue);

    // Malformed spec must not change the slot.
    feed(t, "\033]10;#445566\033\\");
    feed(t, "\033]10;nope\033\\");
    CHECK(t.palette.defaultForeground.red == 0x4444);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testOSC11DefaultBackground)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033]11;rgb:0000/8000/ffff\033\\");
    CHECK(t.palette.defaultBackground.red == 0x0000);
    CHECK(t.palette.defaultBackground.green == 0x8080);
    CHECK(t.palette.defaultBackground.blue == 0xFFFF);

    feed(t, "\033]11;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]11;rgb:0000/8080/ffff\033\\") != nullptr);

    // OSC 111 — reset reverts to the baseline default background.
    feed(t, "\033]111\033\\");
    CHECK(t.palette.defaultBackground.red == t.paletteBaseline.defaultBackground.red);
    CHECK(t.palette.defaultBackground.green == t.paletteBaseline.defaultBackground.green);
    CHECK(t.palette.defaultBackground.blue == t.paletteBaseline.defaultBackground.blue);
}

TEST(testOSC12CursorColor)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    // Set with rgb: form.
    feed(t, "\033]12;rgb:1111/2222/3333\033\\");
    CHECK(t.palette.cursor.red == 0x1111);
    CHECK(t.palette.cursor.green == 0x2222);
    CHECK(t.palette.cursor.blue == 0x3333);

    // Set with #RRGGBB.
    feed(t, "\033]12;#aabbcc\033\\");
    CHECK(t.palette.cursor.red == 0xAAAA);
    CHECK(t.palette.cursor.green == 0xBBBB);
    CHECK(t.palette.cursor.blue == 0xCCCC);

    // Query (`?`) — echoes the stored cursor slot.
    feed(t, "\033]12;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]12;rgb:aaaa/bbbb/cccc\033\\") != nullptr);

    // Malformed spec (no colour space and no colour name) — slot unchanged.
    feed(t, "\033]12;notacolour\033\\");
    CHECK(t.palette.cursor.red == 0xAAAA);

    // OSC 112 — reset cursor colour to the baseline.
    feed(t, "\033]112\033\\");
    CHECK(t.palette.cursor.red == t.paletteBaseline.cursor.red);
    CHECK(t.palette.cursor.green == t.paletteBaseline.cursor.green);
    CHECK(t.palette.cursor.blue == t.paletteBaseline.cursor.blue);
}

TEST(testOSC7Cwd)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;

    // file:///<path> — empty host.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file:///usr/local\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/usr/local") == 0);

    // file://localhost/<path> — named host is ignored.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file://localhost/home/me\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/home/me") == 0);

    // file://hostname/<path> — same.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file://my.host/etc\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/etc") == 0);

    // Percent-decoded path; %20 → ' '.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file:///tmp/has%20space\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/tmp/has space") == 0);

    // Malformed %-escape passes the '%' through literally.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file:///tmp/bad%ZZend\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/tmp/bad%ZZend") == 0);

    // Non-file: scheme is dropped without a client call.
    shell.cwdCalls = 0;
    feed(t, "\033]7;ssh://example.com/x\033\\");
    CHECK(shell.cwdCalls == 0);

    // file:// without an absolute path — dropped.
    shell.cwdCalls = 0;
    feed(t, "\033]7;file://host\033\\");
    CHECK(shell.cwdCalls == 0);

    // Leading whitespace is trimmed.
    shell.cwdCalls = 0;
    feed(t, "\033]7;   file:///opt\033\\");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/opt") == 0);

    t.shellClient = nullptr;
}

TEST(testOSC8SetStampClear)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[H"); // home
    feed(t, "\033]8;;https://example.com\033\\");
    feed(t, "AB");
    feed(t, "\033]8;;\033\\"); // close
    feed(t, "CD");

    const Ubiquitty::Cell* a = cell(t, 0, 0);
    const Ubiquitty::Cell* b = cell(t, 0, 1);
    const Ubiquitty::Cell* c = cell(t, 0, 2);
    const Ubiquitty::Cell* d = cell(t, 0, 3);
    CHECK(a && a->linkID() != 0);
    CHECK(b && b->linkID() == a->linkID()); // same open link
    CHECK(c && c->linkID() == 0); // link closed
    CHECK(d && d->linkID() == 0);

    const char* uri = Ubiquitty::Attributes::Hyperlinks::uri(&t, a->linkID());
    CHECK(uri && strcmp(uri, "https://example.com") == 0);
    // id 0 resolves to nothing.
    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(&t, 0) == nullptr);
}

TEST(testOSC8DedupByIDAndURI)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    // Same id= across two runs → one shared id.
    feed(t, "\033]8;id=foo;https://a.example\033\\");
    feed(t, "X");
    feed(t, "\033]8;;\033\\");
    feed(t, "\033]8;id=foo;https://a.example\033\\");
    feed(t, "Y");
    feed(t, "\033]8;;\033\\");
    uint16_t idX = cell(t, 0, 0)->linkID();
    uint16_t idY = cell(t, 0, 1)->linkID();
    CHECK(idX != 0 && idX == idY);

    // No id=, exact-URI repeat → also dedups to one id.
    feed(t, "\033]8;;https://b.example\033\\");
    feed(t, "M");
    feed(t, "\033]8;;\033\\");
    feed(t, "\033]8;;https://b.example\033\\");
    feed(t, "N");
    feed(t, "\033]8;;\033\\");
    uint16_t idM = cell(t, 0, 2)->linkID();
    uint16_t idN = cell(t, 0, 3)->linkID();
    CHECK(idM != 0 && idM == idN);
    CHECK(idM != idX); // distinct URIs → distinct ids

    // Different id= on same URI → distinct id.
    feed(t, "\033]8;id=bar;https://a.example\033\\");
    feed(t, "Z");
    feed(t, "\033]8;;\033\\");
    uint16_t idZ = cell(t, 0, 4)->linkID();
    CHECK(idZ != 0 && idZ != idX);

    // Both dedups produce one id from two runs, so the id cannot say which of
    // them a cell came through. The entry's key can, and the link span walk
    // reads it to decide whether two runs on one row are one anchor.
    CHECK(Ubiquitty::Attributes::Hyperlinks::isExplicitlyKeyed(&t, idX));
    CHECK(Ubiquitty::Attributes::Hyperlinks::isExplicitlyKeyed(&t, idZ));
    CHECK(!Ubiquitty::Attributes::Hyperlinks::isExplicitlyKeyed(&t, idM));
    CHECK(!Ubiquitty::Attributes::Hyperlinks::isExplicitlyKeyed(&t, 0));
}

TEST(testOSC8TableOverflowRecycle)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    // Intern the first URI, capture its id.
    feed(t, "\033]8;;https://link0\033\\");
    feed(t, "a");
    feed(t, "\033]8;;\033\\");
    uint16_t firstID = cell(t, 0, 0)->linkID();
    CHECK(firstID != 0);
    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(&t, firstID) != nullptr);

    // Intern linkTableCapacity more distinct URIs to push the first out.
    char seq[64];
    for (int i = 1; i <= Ubiquitty::VT::linkTableCapacity; i++) {
        snprintf(seq, sizeof(seq), "\033]8;;https://link%d\033\\X\033]8;;\033\\", i);
        feed(t, seq);
    }

    // The first id has been recycled; it no longer resolves.
    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(&t, firstID) == nullptr);
    // The most-recently interned URI still resolves. Re-home so the
    // stamped cell lands at a known position.
    feed(t, "\033[H");
    feed(t, "\033]8;;https://latest\033\\");
    feed(t, "z");
    feed(t, "\033]8;;\033\\");
    uint16_t lastID = cell(t, 0, 0)->linkID();
    const char* uri = Ubiquitty::Attributes::Hyperlinks::uri(&t, lastID);
    CHECK(uri && strcmp(uri, "https://latest") == 0);
}

TEST(testOSC8MRURepeatOpenSameID)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    feed(t, "\033]8;;https://repeat.example\033\\");
    feed(t, "a");
    feed(t, "\033]8;;\033\\");
    uint16_t firstID = cell(t, 0, 0)->linkID();
    CHECK(firstID != 0);

    feed(t, "\033]8;;https://repeat.example\033\\");
    feed(t, "b");
    feed(t, "\033]8;;\033\\");
    uint16_t secondID = cell(t, 0, 1)->linkID();
    CHECK(secondID == firstID);
}

TEST(testOSC8MRUOverlongTargetsMintFresh)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    std::string longURI = "https://";
    while (longURI.size() < static_cast<size_t>(Ubiquitty::VT::maxLinkURILength) + 8)
        longURI += "x";
    std::string open = "\033]8;;" + longURI + "\033\\";
    feed(t, open.c_str());
    feed(t, "a");
    feed(t, "\033]8;;\033\\");
    uint16_t firstID = cell(t, 0, 0)->linkID();
    feed(t, open.c_str());
    feed(t, "b");
    feed(t, "\033]8;;\033\\");
    uint16_t secondID = cell(t, 0, 1)->linkID();
    CHECK(firstID != 0 && secondID != 0);
    CHECK(secondID != firstID);

    /// KEYS differ: the OSC 8 parameter parser truncates id= to
    /// maxLinkKeyLength - 1 before linkIntern (OSCState's fixed key buffer),
    /// so repeated over-long keys arrive identical-truncated and DEDUP to one
    /// id — with or without the MRU (the truncated key fits its ring field).
    std::string longKey(static_cast<size_t>(Ubiquitty::VT::maxLinkKeyLength) + 8, 'k');
    std::string keyedOpen = "\033]8;id=" + longKey + ";https://k.example\033\\";
    feed(t, keyedOpen.c_str());
    feed(t, "c");
    feed(t, "\033]8;;\033\\");
    uint16_t keyedFirstID = cell(t, 0, 2)->linkID();
    feed(t, keyedOpen.c_str());
    feed(t, "d");
    feed(t, "\033]8;;\033\\");
    uint16_t keyedSecondID = cell(t, 0, 3)->linkID();
    CHECK(keyedFirstID != 0 && keyedSecondID != 0);
    CHECK(keyedSecondID == keyedFirstID);
}

TEST(testOSC8MRURecycleInvalidation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    feed(t, "\033]8;;https://mru.target\033\\");
    feed(t, "a");
    feed(t, "\033]8;;\033\\");
    uint16_t staleID = cell(t, 0, 0)->linkID();
    CHECK(staleID != 0);

    // Force the target's slot out of the ring (and recycle repeatedly so
    // the MRU drop path runs many times).
    char seq[96];
    for (int i = 1; i <= Ubiquitty::VT::linkTableCapacity + 4; i++) {
        snprintf(seq, sizeof(seq), "\033]8;;https://filler%d\033\\X\033]8;;\033\\", i);
        feed(t, seq);
    }
    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(&t, staleID) == nullptr);

    // Re-open the original target: a fresh id that resolves to the right
    // URL, not the stale one.
    feed(t, "\033[H");
    feed(t, "\033]8;;https://mru.target\033\\");
    feed(t, "z");
    feed(t, "\033]8;;\033\\");
    uint16_t freshID = cell(t, 0, 0)->linkID();
    CHECK(freshID != 0);
    CHECK(freshID != staleID);
    const char* uri = Ubiquitty::Attributes::Hyperlinks::uri(&t, freshID);
    CHECK(uri && strcmp(uri, "https://mru.target") == 0);
}

TEST(testOSC8MRUKeyVsURIDistinct)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H");

    feed(t, "\033]8;id=sametext;https://keyed.example\033\\");
    feed(t, "a");
    feed(t, "\033]8;;\033\\");
    uint16_t keyedID = cell(t, 0, 0)->linkID();

    feed(t, "\033]8;;sametext\033\\");
    feed(t, "b");
    feed(t, "\033]8;;\033\\");
    uint16_t uriID = cell(t, 0, 1)->linkID();

    CHECK(keyedID != 0 && uriID != 0);
    CHECK(uriID != keyedID);
    // Each repeat-open sticks to its own id (MRU keyed on target kind).
    feed(t, "\033]8;id=sametext;https://keyed.example\033\\");
    feed(t, "c");
    feed(t, "\033]8;;\033\\");
    CHECK(cell(t, 0, 2)->linkID() == keyedID);
    feed(t, "\033]8;;sametext\033\\");
    feed(t, "d");
    feed(t, "\033]8;;\033\\");
    CHECK(cell(t, 0, 3)->linkID() == uriID);
}

TEST(testOSC1719SelectionColor)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::PAL::Context selectionContext;
    selectionContext.monotonicClock = &osc133FakeClock;
    selectionContext.selectionBackgroundSet = testOSCSelbgSetHook;
    selectionContext.selectionBackgroundReset = testOSCSelbgResetHook;
    selectionContext.selectionForegroundSet = testOSCSelfgSetHook;
    selectionContext.selectionForegroundReset = testOSCSelfgResetHook;
    Ubiquitty::PAL::ScopedContext scopedSelection(selectionContext);
    selectionBackgroundSetCalls = selectionBackgroundResetCalls = 0;
    selectionForegroundSetCalls = selectionForegroundResetCalls = 0;

    // OSC 17 — selection background set + Ubiquitty::Terminal storage.
    feed(t, "\033]17;rgb:1234/5678/9abc\033\\");
    CHECK(selectionBackgroundSetCalls == 1);
    CHECK(selectionBackgroundRed == 0x1212);
    CHECK(selectionBackgroundGreen == 0x5656);
    CHECK(selectionBackgroundBlue == 0x9A9A);
    CHECK(t.selectionBackgroundSet);
    CHECK(t.selectionBackgroundRed == 0x1212);

    // OSC 17 ; ? query echoes the stored value framed as OSC 17.
    feed(t, "\033]17;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]17;rgb:1212/5656/9a9a\033\\") != nullptr);

    // OSC 19 — selection foreground via #RRGGBB.
    feed(t, "\033]19;#804020\033\\");
    CHECK(selectionForegroundSetCalls == 1);
    CHECK(selectionForegroundRed == 0x8080);
    CHECK(selectionForegroundGreen == 0x4040);
    CHECK(selectionForegroundBlue == 0x2020);
    CHECK(t.selectionForegroundSet);

    feed(t, "\033]19;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]19;rgb:8080/4040/2020\033\\") != nullptr);

    // OSC 117 / 119 — reset clears the set flag + fires reset hook.
    feed(t, "\033]117\033\\");
    CHECK(selectionBackgroundResetCalls == 1);
    CHECK(!t.selectionBackgroundSet);
    feed(t, "\033]119\007");
    CHECK(selectionForegroundResetCalls == 1);
    CHECK(!t.selectionForegroundSet);

    // A query after reset reports the default selection colour.
    feed(t, "\033]17;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]17;rgb:") != nullptr);

    // Malformed spec does not call the setter.
    feed(t, "\033]17;nope\033\\");
    CHECK(selectionBackgroundSetCalls == 1);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

/// xterm defines OSC 13 and 14, and an unset pointer answers with its
/// pointerColor and pointerColorBackground resources, which default to the
/// configured foreground and background. OSC 10 and 11 change the text
/// colours, not those resources, so the answer stays the theme's.
TEST(testUnsetPointerColorsAnswerWithTheConfiguredForegroundAndBackground)
{
    Ubiquitty::Terminal t;
    char buffer[256];
    Ubiquitty::hardReset(t);
    t.paletteBaseline.defaultForeground = { 0x1111, 0x2222, 0x3333 };
    t.paletteBaseline.defaultBackground = { 0xEEEE, 0xDDDD, 0xCCCC };
    t.palette.defaultForeground = t.paletteBaseline.defaultForeground;
    t.palette.defaultBackground = t.paletteBaseline.defaultBackground;
    feed(t, "\033]10;#808080\033\\\033]11;#404040\033\\");
    drain(t, buffer);

    feed(t, "\033]13;?;?\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]13;rgb:1111/2222/3333\033\\\033]14;rgb:eeee/dddd/cccc\033\\") == 0);
}

TEST(testOSCSpecialColors)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    // OSC 13 — mouse pointer foreground, rgb: form.
    feed(t, "\033]13;rgb:1234/5678/9abc\033\\");
    CHECK(t.mousePointerForegroundSet);
    CHECK(t.mousePointerForegroundRed == 0x1212);
    CHECK(t.mousePointerForegroundGreen == 0x5656);
    CHECK(t.mousePointerForegroundBlue == 0x9A9A);
    feed(t, "\033]13;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]13;rgb:1212/5656/9a9a\033\\") != nullptr);
    feed(t, "\033]113\033\\");
    CHECK(!t.mousePointerForegroundSet);
    // A `?` after reset answers with the configured foreground.
    feed(t, "\033]13;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]13;rgb:c0c0/c0c0/c0c0\033\\") != nullptr);

    // OSC 14 — mouse pointer background, #RRGGBB form.
    feed(t, "\033]14;#804020\033\\");
    CHECK(t.mousePointerBackgroundSet);
    CHECK(t.mousePointerBackgroundRed == 0x8080);
    CHECK(t.mousePointerBackgroundGreen == 0x4040);
    CHECK(t.mousePointerBackgroundBlue == 0x2020);
    feed(t, "\033]14;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]14;rgb:8080/4040/2020\033\\") != nullptr);
    feed(t, "\033]114\033\\");
    CHECK(!t.mousePointerBackgroundSet);
    // A `?` after reset answers with the configured background.
    feed(t, "\033]14;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]14;rgb:1010/1010/1414\033\\") != nullptr);

    // OSC 15 — Tektronix foreground.
    feed(t, "\033]15;rgb:0000/8000/ffff\033\\");
    CHECK(t.tektronixForegroundSet);
    CHECK(t.tektronixForegroundRed == 0x0000);
    CHECK(t.tektronixForegroundGreen == 0x8080);
    CHECK(t.tektronixForegroundBlue == 0xFFFF);
    feed(t, "\033]15;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]15;rgb:0000/8080/ffff\033\\") != nullptr);
    feed(t, "\033]115\033\\");
    CHECK(!t.tektronixForegroundSet);

    // OSC 16 — Tektronix background.
    feed(t, "\033]16;rgb:1111/2222/3333\033\\");
    CHECK(t.tektronixBackgroundSet);
    CHECK(t.tektronixBackgroundRed == 0x1111);
    feed(t, "\033]16;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]16;rgb:1111/2222/3333\033\\") != nullptr);
    feed(t, "\033]116\033\\");
    CHECK(!t.tektronixBackgroundSet);

    // OSC 18 — Tektronix cursor (ST via BEL terminator).
    feed(t, "\033]18;#aabbcc\007");
    CHECK(t.tektronixCursorSet);
    CHECK(t.tektronixCursorRed == 0xAAAA);
    CHECK(t.tektronixCursorGreen == 0xBBBB);
    CHECK(t.tektronixCursorBlue == 0xCCCC);
    feed(t, "\033]18;?\007");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]18;rgb:aaaa/bbbb/cccc\007") != nullptr);
    feed(t, "\033]118\007");
    CHECK(!t.tektronixCursorSet);

    // Malformed spec must not set the slot.
    feed(t, "\033]13;nope\033\\");
    CHECK(!t.mousePointerForegroundSet);

    // hsl: color spec (Ubiquitty extension). hsl:0/100/50 is pure red:
    // chroma=100, h=0 → r=100%, g=0, b=0; m = 50-50 = 0.
    feed(t, "\033]13;hsl:0/100/50\033\\");
    CHECK(t.mousePointerForegroundSet);
    CHECK(t.mousePointerForegroundRed == 0xFFFF);
    CHECK(t.mousePointerForegroundGreen == 0x0000);
    CHECK(t.mousePointerForegroundBlue == 0x0000);
    feed(t, "\033]113\033\\");

    // hsl:120/100/50 is pure green; hsl:240/100/50 pure blue.
    feed(t, "\033]13;hsl:120/100/50\033\\");
    CHECK(t.mousePointerForegroundGreen == 0xFFFF);
    CHECK(t.mousePointerForegroundRed == 0x0000);
    CHECK(t.mousePointerForegroundBlue == 0x0000);
    feed(t, "\033]113\033\\");
    feed(t, "\033]13;hsl:240/100/50\033\\");
    CHECK(t.mousePointerForegroundBlue == 0xFFFF);
    CHECK(t.mousePointerForegroundRed == 0x0000);
    CHECK(t.mousePointerForegroundGreen == 0x0000);
    feed(t, "\033]113\033\\");

    // hsl: saturation 0 → gray at the lightness level (50% → mid-gray),
    // all three channels equal.
    feed(t, "\033]13;hsl:0/0/50\033\\");
    CHECK(t.mousePointerForegroundSet);
    CHECK(t.mousePointerForegroundRed == t.mousePointerForegroundGreen);
    CHECK(t.mousePointerForegroundGreen == t.mousePointerForegroundBlue);
    CHECK(t.mousePointerForegroundRed > 0x7000 && t.mousePointerForegroundRed < 0x9000);
    feed(t, "\033]113\033\\");

    // hsl: lightness 0 → black, lightness 100 → white.
    feed(t, "\033]13;hsl:200/80/0\033\\");
    CHECK(t.mousePointerForegroundRed == 0x0000);
    CHECK(t.mousePointerForegroundGreen == 0x0000);
    CHECK(t.mousePointerForegroundBlue == 0x0000);
    feed(t, "\033]113\033\\");
    feed(t, "\033]13;hsl:200/80/100\033\\");
    CHECK(t.mousePointerForegroundRed == 0xFFFF);
    CHECK(t.mousePointerForegroundGreen == 0xFFFF);
    CHECK(t.mousePointerForegroundBlue == 0xFFFF);
    feed(t, "\033]113\033\\");

    // Malformed hsl: (missing field) must not set the slot.
    feed(t, "\033]13;hsl:120/50\033\\");
    CHECK(!t.mousePointerForegroundSet);

    // hsl: also works as an OSC 11 (default background) spec, proving the
    // parser is shared by all color OSCs, not just the dynamic-color slots.
    feed(t, "\033]11;hsl:0/100/50\033\\");
    CHECK(t.palette.defaultBackground.red == 0xFFFF);
    CHECK(t.palette.defaultBackground.green == 0x0000);
    CHECK(t.palette.defaultBackground.blue == 0x0000);

    // The OSCs must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testOSC5SpecialColors)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    int n;
    Ubiquitty::hardReset(t);

    // Set bold (Pc=0) via rgb:RRRR/GGGG/BBBB.
    feed(t, "\033]5;0;rgb:ffff/0000/8080\033\\");
    CHECK(t.palette.specialSet[Ubiquitty::specialBold]);
    CHECK(t.palette.special[Ubiquitty::specialBold].red == 0xFFFF);
    CHECK(t.palette.special[Ubiquitty::specialBold].green == 0x0000);
    CHECK(t.palette.special[Ubiquitty::specialBold].blue == 0x8080);

    // Set italic (Pc=4) via #RRGGBB short fields, BEL terminator.
    feed(t, "\033]5;4;#102030\007");
    CHECK(t.palette.specialSet[Ubiquitty::specialItalic]);
    CHECK(t.palette.special[Ubiquitty::specialItalic].red == 0x1010);
    CHECK(t.palette.special[Ubiquitty::specialItalic].green == 0x2020);
    CHECK(t.palette.special[Ubiquitty::specialItalic].blue == 0x3030);

    // Query (`?`) replies framed as OSC 5 with the stored value.
    feed(t, "\033]5;0;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]5;0;rgb:ffff/0000/8080\033\\") != nullptr);

    // Chained pairs in one OSC 5: underline (1) + reverse (3).
    feed(t, "\033]5;1;rgb:1/2/3;3;rgb:4/5/6\033\\");
    CHECK(t.palette.special[Ubiquitty::specialUnderline].red == 0x1111);
    CHECK(t.palette.special[Ubiquitty::specialReverse].red == 0x4444);
    CHECK(t.palette.special[Ubiquitty::specialReverse].green == 0x5555);
    CHECK(t.palette.special[Ubiquitty::specialReverse].blue == 0x6666);

    // Out-of-range Pc is dropped silently.
    feed(t, "\033]5;9;rgb:0/0/0\033\\");

    // OSC 105 restores a special colour only from a configured resource, and
    // none is configurable, so neither form changes one.
    feed(t, "\033]105;0\033\\");
    CHECK(t.palette.specialSet[Ubiquitty::specialBold]);
    feed(t, "\033]105\033\\");
    CHECK(t.palette.specialSet[Ubiquitty::specialItalic]);
    feed(t, "\033]5;0;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]5;0;rgb:ffff/0000/8080\033\\") != nullptr);

    // OSC 4 alias: index 256+Pc addresses the same special slots.
    feed(t, "\033]4;257;rgb:abcd/1234/5678\033\\"); // 257 = 256 + underline
    CHECK(t.palette.specialSet[Ubiquitty::specialUnderline]);
    CHECK(t.palette.special[Ubiquitty::specialUnderline].red == 0xABAB);
    feed(t, "\033]4;257;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]4;257;rgb:abab/1212/5656\033\\") != nullptr);

    // Nor does OSC 104 through the same alias.
    feed(t, "\033]104;257\033\\");
    CHECK(t.palette.specialSet[Ubiquitty::specialUnderline]);

    // The OSCs must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testOSCNotifications)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;

    // OSC 9 ; <text> (iTerm) — body-only notification.
    feed(t, "\033]9;Build finished\007");
    CHECK(shell.notifyCalls == 1);
    CHECK(shell.notifyTitleLength == 0);
    CHECK(strcmp(shell.notifyBody, "Build finished") == 0);

    // OSC 9;4;... progress must not fire the notification client.
    feed(t, "\033]9;4;1;50\007");
    CHECK(shell.notifyCalls == 1);

    // OSC 99 (kitty) — a title chunk held open by d=0 posts nothing; the body
    // chunk under the same name finishes it, and the two arrive as one.
    feed(t, "\033]99;i=1:d=0;Agent\033\\");
    CHECK(shell.notifyCalls == 1);
    feed(t, "\033]99;i=1:p=body;is waiting\033\\");
    CHECK(shell.notifyCalls == 2);
    CHECK(strcmp(shell.notifyTitle, "Agent") == 0);
    CHECK(strcmp(shell.notifyBody, "is waiting") == 0);

    // OSC 99 with no metadata separator — the whole payload is the text, and
    // text with no p= is the title.
    feed(t, "\033]99;Bare text\033\\");
    CHECK(shell.notifyCalls == 3);
    CHECK(strcmp(shell.notifyTitle, "Bare text") == 0);
    CHECK(shell.notifyBodyLength == 0);

    // OSC 99 with empty metadata.
    feed(t, "\033]99;;Titled only\033\\");
    CHECK(shell.notifyCalls == 4);
    CHECK(strcmp(shell.notifyTitle, "Titled only") == 0);
    CHECK(shell.notifyBodyLength == 0);

    // OSC 99 t= is the protocol's type tag, not a title: it is ignored.
    feed(t, "\033]99;t=Alert;Text here\033\\");
    CHECK(shell.notifyCalls == 5);
    CHECK(strcmp(shell.notifyTitle, "Text here") == 0);
    CHECK(shell.notifyBodyLength == 0);

    // A value that contains another key's spelling is still one value.
    feed(t, "\033]99;i=ap=body;Text9\033\\");
    CHECK(shell.notifyCalls == 6);
    CHECK(strcmp(shell.notifyTitle, "Text9") == 0);
    CHECK(shell.notifyBodyLength == 0);

    // OSC 99 p=body alone — a body and no title.
    feed(t, "\033]99;p=body;Just a body\033\\");
    CHECK(shell.notifyCalls == 7);
    CHECK(shell.notifyTitleLength == 0);
    CHECK(strcmp(shell.notifyBody, "Just a body") == 0);

    // A query, a close and an icon post nothing and send nothing back.
    feed(t, "\033]99;i=q:p=?;\033\\");
    feed(t, "\033]99;i=1:p=close;\033\\");
    feed(t, "\033]99;i=1:p=icon:e=1;aWNvbg==\033\\");
    CHECK(shell.notifyCalls == 7);

    // OSC 777 ; notify ; <title> ; <body> (urxvt).
    feed(t, "\033]777;notify;Title here;Body here\033\\");
    CHECK(shell.notifyCalls == 8);
    CHECK(strcmp(shell.notifyTitle, "Title here") == 0);
    CHECK(strcmp(shell.notifyBody, "Body here") == 0);

    // OSC 777 with title only (no body).
    feed(t, "\033]777;notify;Only title\033\\");
    CHECK(shell.notifyCalls == 9);
    CHECK(strcmp(shell.notifyTitle, "Only title") == 0);
    CHECK(shell.notifyBodyLength == 0);

    // OSC 777 non-notify module is ignored.
    feed(t, "\033]777;something;else\033\\");
    CHECK(shell.notifyCalls == 9);

    // Empty OSC 9 payload does not fire.
    feed(t, "\033]9;\007");
    CHECK(shell.notifyCalls == 9);

    // The OSC must not leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    t.shellClient = nullptr;
}

TEST(testOSCNotificationsGatedByCapabilityPolicy)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CaptureShell shell;
        t.shellClient = &shell;
        fakeCapabilityPolicy.wasCalled = false;
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        feed(t, "\033]9;Build finished\007");
        CHECK(fakeCapabilityPolicy.wasCalled);
        CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::DesktopNotification);
        CHECK(shell.notifyCalls == 0); // denied → no notification
        t.capabilityPolicy = nullptr;
        t.shellClient = nullptr;
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CaptureShell shell;
        t.shellClient = &shell;
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        feed(t, "\033]9;Build finished\007");
        CHECK(shell.notifyCalls == 1); // allowed → notification fired
        t.capabilityPolicy = nullptr;
        t.shellClient = nullptr;
    }
}

TEST(testOSC633ShellIntegration)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    const int liveBase = Ubiquitty::Screen::scrollbackCount(&t);
    // A/B/C/D map onto the OSC 133 prompt-mark machinery (per row).
    feed(t, "\033]633;A\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 0) == Ubiquitty::Screen::osc133MarkPromptStart);

    feed(t, "\n\033]633;B\007\033]633;C\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 1) == (Ubiquitty::Screen::osc133MarkCmdStart | Ubiquitty::Screen::osc133MarkOutputStart));

    feed(t, "\n\033]633;D;0\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 2) == Ubiquitty::Screen::osc133MarkCmdEnd);

    // 633;E (command line) and 633;P (property) are consumed without
    // error and do not set a mark on the cursor row.
    feed(t, "\n\033]633;E;ls -la\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 3) == 0);
    feed(t, "\033]633;P;CurrentDirectory=/home/me\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 3) == 0);

    // Unknown sub-command is ignored gracefully.
    feed(t, "\033]633;Z\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, liveBase + 3) == 0);

    // No leak into the cell stream beyond the explicit newlines.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testOSC1337Subset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureShell shell;
    t.shellClient = &shell;

    // SetMark → prompt mark on the cursor row.
    feed(t, "\033]1337;SetMark\007");
    CHECK(Ubiquitty::Screen::rowMark(&t, Ubiquitty::Screen::scrollbackCount(&t) + 0) == Ubiquitty::Screen::osc133MarkPromptStart);

    // CurrentDir= reuses the OSC 7 currentDirectory path.
    feed(t, "\033]1337;CurrentDir=/var/log\007");
    CHECK(shell.cwdCalls == 1);
    CHECK(strcmp(shell.currentDirectory, "/var/log") == 0);

    // RemoteHost= stored on Terminal.
    feed(t, "\033]1337;RemoteHost=user@host.example\007");
    CHECK(t.remoteHost == "user@host.example");

    // Unhandled key (inline image, etc.) consumed without effect.
    feed(t, "\033]1337;File=name=foo:AAAA\007");
    CHECK(shell.cwdCalls == 1);

    // No leak into the cell stream.
    CHECK(cell(t, 0, 0)->codePoint == 0);

    t.shellClient = nullptr;
}

TEST(testOSC52DataThatIsNotBase64ClearsTheSelection)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureClipboard clip;
    t.clipboardClient = &clip;
    t.isOSC52Allowed = true;

    // "If the second parameter is neither a base64 string nor ?, then the
    // selection is cleared": a non-pad after '=', and a truncated tail.
    // A '!' after no base64 character marks no fragment of a transfer, so a
    // lone '!' is such data too.
    for (const char* data : { "AB==CD", "ABCDE", "@@@@", "!", "!!!!", "@@@!" }) {
        feed(t, "\033]52;c;SGk=\033\\");
        CHECK(clip.capturedLength == 2);
        feed(t, std::string("\033]52;c;") + data + "\033\\");
        CHECK(clip.capturedLength == 0);
        CHECK(clip.lastBuffer == Ubiquitty::ClipboardBuffer::Clipboard);
    }
    // A chunked transfer that is not base64 as a whole clears it the same way.
    feed(t, "\033]52;p;SGk=\033\\\033]52;p;@@@A!\033\\");
    CHECK(t.osc52ChunkActive);
    feed(t, "\033]52;p;SGk=\033\\");
    CHECK(clip.lastBuffer == Ubiquitty::ClipboardBuffer::Primary);
    CHECK(clip.capturedLength == 0);
    CHECK(!t.osc52ChunkActive);
    // The happy path still writes, so the clears are not a client that only
    // ever receives empty data.
    feed(t, "\033]52;c;SGk=\033\\");
    CHECK(clip.capturedLength == 2);
    // A refused write clears nothing.
    const int calls = clip.setCalls;
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    feed(t, "\033]52;c;@@@@\033\\");
    CHECK_EQ(clip.setCalls, calls);

    t.clipboardClient = nullptr;
}

TEST(testOSC94Progress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    uint8_t gen0 = t.progressGeneration;

    feed(t, "\033]9;4;1;42\033\\");
    CHECK(t.progressState == 1);
    CHECK(t.progressValue == 42);
    CHECK(t.progressGeneration == (uint8_t)(gen0 + 1));

    // value > 100 clamps to 100.
    feed(t, "\033]9;4;1;250\033\\");
    CHECK(t.progressValue == 100);

    // state > 4 clamps to 0 (no progress).
    feed(t, "\033]9;4;9;50\033\\");
    CHECK(t.progressState == 0);

    // Missing value defaults to 0.
    feed(t, "\033]9;4;3\033\\");
    CHECK(t.progressState == 3);
    CHECK(t.progressValue == 0);

    /// Sub-op != 4 is a silent no-op (no generation tick).
    uint8_t genBefore = t.progressGeneration;
    feed(t, "\033]9;7;1;1\033\\");
    CHECK(t.progressGeneration == genBefore);
}

TEST(testXTGETTCAPHexHelpers)
{
    char buffer[16];
    int n;
    // Round-trip "TN" <-> "544e".
    n = Ubiquitty::Terminfo::hexEncode("TN", buffer);
    CHECK(n == 4);
    CHECK(memcmp(buffer, "544e", 4) == 0);
    n = Ubiquitty::Terminfo::hexDecode("544e", buffer);
    CHECK(n == 2);
    CHECK(buffer[0] == 'T' && buffer[1] == 'N');
    // Mixed-case input still decodes.
    n = Ubiquitty::Terminfo::hexDecode("4B66", buffer);
    CHECK(n == 2);
    CHECK(buffer[0] == 'K' && buffer[1] == 'f');
    // Odd length is rejected.
    n = Ubiquitty::Terminfo::hexDecode("abc", buffer);
    CHECK(n == -1);
    // Non-hex byte is rejected.
    n = Ubiquitty::Terminfo::hexDecode("ZZ", buffer);
    CHECK(n == -1);
    // Output too small to hold the decoded bytes.
    n = Ubiquitty::Terminfo::hexDecode("544e", { buffer, 1 });
    CHECK(n == -1);
    // Output too small to hold the encoded pair.
    n = Ubiquitty::Terminfo::hexEncode("TN", { buffer, 3 });
    CHECK(n == -1);
    // Empty is nothing to do rather than malformed. A caller may split a payload
    // on any byte boundary, and one of the halves can be empty.
    CHECK(Ubiquitty::Terminfo::hexDecode("", buffer) == 0);
    CHECK(Ubiquitty::Terminfo::hexEncode("", buffer) == 0);

    // A zero-length name is a miss and never reaches the table.
    CHECK(!Ubiquitty::Terminfo::lookup("").has_value());
    // An unknown cap is a miss.
    CHECK(!Ubiquitty::Terminfo::lookup("XYZ").has_value());
    // A known string cap carries its value.
    CHECK(Ubiquitty::Terminfo::lookup("TN") == "xterm-ubiquitty"); // the TERM the app sets, which is what a program compares it to
    // A boolean cap HITS with an empty value, and that is not a miss. The wire
    // emits "<hex>=" with no bytes after the '=' for one and a 0+r refusal for
    // the other, so a caller conflating them answers the wrong sequence.
    const std::optional<std::string_view> booleanCapability = Ubiquitty::Terminfo::lookup("am");
    CHECK(booleanCapability.has_value());
    CHECK(booleanCapability->empty());
}

TEST(test_xtgettcap_hit_TN)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);
    // "TN" = "544e" — terminal name.
    feed(t, "\033P+q544e\033\\");
    n = drain(t, outputBuffer);
    // Expect: ESC P 1 + r 544e=<hex of "ubiquitty"> ESC `\`
    CHECK(n > 4);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == 'P');
    CHECK(outputBuffer[2] == '1');
    CHECK(outputBuffer[3] == '+');
    CHECK(outputBuffer[4] == 'r');
    // Body contains the cap hex echo + '=' + hex value.
    CHECK(strstr(outputBuffer, "544e=") != nullptr);
    // "ubiquitty" in hex = 756269717569747479
    CHECK(strstr(outputBuffer, "756269717569747479") != nullptr);
    // Trailing ST.
    CHECK(outputBuffer[n - 2] == 0x1B);
    CHECK(outputBuffer[n - 1] == '\\');
}

TEST(test_xtgettcap_hit_Co)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);
    // "Co" = "436f" — colour count. Value "256" = "323536".
    feed(t, "\033P+q436f\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 4);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == 'P');
    CHECK(outputBuffer[2] == '1');
    CHECK(strstr(outputBuffer, "436f=323536") != nullptr);
    CHECK(outputBuffer[n - 2] == 0x1B);
    CHECK(outputBuffer[n - 1] == '\\');
}

TEST(testXTGETTCAPMiss)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    // "ZZ" = "5a5a" — never in the cap table.
    feed(t, "\033P+q5a5a\033\\");
    n = drain(t, outputBuffer);
    // Miss response: ESC P 0 + r ESC \  (exactly 7 bytes).
    CHECK(n == 7);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == 'P');
    CHECK(outputBuffer[2] == '0');
    CHECK(outputBuffer[3] == '+');
    CHECK(outputBuffer[4] == 'r');
    CHECK(outputBuffer[5] == 0x1B);
    CHECK(outputBuffer[6] == '\\');
}

TEST(testXTGETTCAPMultiQuery)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int n;
    int dcsCount;
    int i;
    Ubiquitty::hardReset(t);
    // "TN;Co" → "544e;436f". Two `;`-separated cap probes in one frame are
    // answered in one DCS, joined by `;`, as xterm answers them.
    feed(t, "\033P+q544e;436f\033\\");
    n = drain(t, outputBuffer);
    dcsCount = 0;
    for (i = 0; i + 1 < n; i++) {
        if (outputBuffer[i] == 0x1B && outputBuffer[i + 1] == 'P')
            dcsCount++;
    }
    CHECK(dcsCount == 1);
    CHECK(strcmp(outputBuffer, "\033P1+r544e=787465726D2D756269717569747479;436f=323536\033\\") == 0);
    // Answering stops at the first name it does not know, after the `;` that
    // introduced it.
    feed(t, "\033P+q436f;6e6f7065;544e\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P1+r436f=323536;\033\\") == 0);
    // A request whose first name is unknown is answered with status 0 alone.
    feed(t, "\033P+q6e6f7065;436f\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P0+r\033\\") == 0);
}

TEST(testXTGETTCAPAnswersALongValueWhole)
{
    Ubiquitty::Terminal t;
    char outputBuffer[1024];
    Ubiquitty::hardReset(t);
    // "initc" = 696e697463; its value runs past a hundred bytes.
    feed(t, "\033P+q696e697463\033\\");
    const int n = drain(t, outputBuffer);
    CHECK(n > 200);
    CHECK(strncmp(outputBuffer, "\033P1+r696e697463=1B5D343B", 24) == 0);
    CHECK(strcmp(outputBuffer + n - 2, "\033\\") == 0);
}

TEST(testOSC110ResetsDefaultForeground)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::PaletteRGB baseline = t.paletteBaseline.defaultForeground;

    feed(t, "\033]10;rgb:1111/2222/3333\033\\");
    CHECK(t.palette.defaultForeground.red == 0x1111);
    feed(t, "\033]110\033\\");
    CHECK(t.palette.defaultForeground.red == baseline.red);
    CHECK(t.palette.defaultForeground.green == baseline.green);
    CHECK(t.palette.defaultForeground.blue == baseline.blue);
}

TEST(testOSC111ResetsDefaultBackground)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::PaletteRGB baseline = t.paletteBaseline.defaultBackground;

    feed(t, "\033]11;rgb:1111/2222/3333\033\\");
    CHECK(t.palette.defaultBackground.red == 0x1111);
    feed(t, "\033]111\033\\");
    CHECK(t.palette.defaultBackground.red == baseline.red);
    CHECK(t.palette.defaultBackground.green == baseline.green);
    CHECK(t.palette.defaultBackground.blue == baseline.blue);
}

TEST(testOSC4IndexOutOfRangeDropped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;256;rgb:0/0/0\033\\");
    feed(t, "\033]4;999;rgb:f/f/f\033\\");
    // A leading '-' isn't a digit, so the parser bails (sawDigit stays
    // false) and no write is performed.
    feed(t, "\033]4;-1;rgb:0/0/0\033\\");

    // In-range index still works after the malformed ones.
    feed(t, "\033]4;42;rgb:1/2/3\033\\");
    CHECK(t.palette.indexed[42].red == 0x1111);
    CHECK(t.palette.indexed[42].green == 0x2222);
    CHECK(t.palette.indexed[42].blue == 0x3333);
}

TEST(testOSCMalformedSpecRejected)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::PaletteRGB before = t.palette.defaultForeground;

    // `rgb:` alone — no channels.
    feed(t, "\033]10;rgb:\033\\");
    // `rgb:R` — missing the second separator + G + B.
    feed(t, "\033]10;rgb:f\033\\");
    // `rgb:R/G` — missing the B channel.
    feed(t, "\033]10;rgb:f/0\033\\");
    // `#XYZ` — non-hex characters.
    feed(t, "\033]10;#XYZ\033\\");
    // `#GGGGGG` — non-hex characters (length is 0 because the parser
    // stops at the first non-hex).
    feed(t, "\033]10;#GGGGGG\033\\");

    CHECK(t.palette.defaultForeground.red == before.red);
    CHECK(t.palette.defaultForeground.green == before.green);
    CHECK(t.palette.defaultForeground.blue == before.blue);

    // A valid spec right after must still work — parser state cleanly
    // reset between OSCs.
    feed(t, "\033]10;#102030\033\\");
    CHECK(t.palette.defaultForeground.red == 0x1010);
}

TEST(testOSCQueryReportsSeededDefaults)
{
    Ubiquitty::Terminal t;
    char buffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033]4;5;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]4;5;rgb:") != nullptr);

    feed(t, "\033]10;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]10;rgb:") != nullptr);

    feed(t, "\033]11;?\033\\");
    n = drain(t, buffer);
    CHECK(n > 0);
    CHECK(strstr(buffer, "\033]11;rgb:") != nullptr);

    // Reset codes must not crash.
    feed(t, "\033]110\033\\");
    feed(t, "\033]111\033\\");
}

TEST(testOSCTerminatorStAndBel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // BEL terminator.
    feed(t, "\033]10;rgb:0100/0200/0300\007");
    CHECK(t.palette.defaultForeground.red == 0x0101);
    CHECK(t.palette.defaultForeground.green == 0x0202);
    CHECK(t.palette.defaultForeground.blue == 0x0303);

    // ST terminator.
    feed(t, "\033]10;rgb:0a00/0b00/0c00\033\\");
    CHECK(t.palette.defaultForeground.red == 0x0A0A);
    CHECK(t.palette.defaultForeground.green == 0x0B0B);
    CHECK(t.palette.defaultForeground.blue == 0x0C0C);
}

TEST(testXTGETTCAPFunctionKeys)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int n;

    Ubiquitty::hardReset(t);
    // "kf1" = 6b6631 → "\eOP" = 1b 4f 50 = "1b4f50"
    feed(t, "\033P+q6b6631\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "6b6631=1B4F50") != nullptr);

    Ubiquitty::hardReset(t);
    // "kf5" = 6b6635 → "\e[15~" = 1b 5b 31 35 7e = "1b5b31357e"
    feed(t, "\033P+q6b6635\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "6b6635=1B5B31357E") != nullptr);

    Ubiquitty::hardReset(t);
    // "kf12" = 6b663132 → "\e[24~" = 1b 5b 32 34 7e = "1b5b32347e"
    feed(t, "\033P+q6b663132\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "6b663132=1B5B32347E") != nullptr);
}

// XTGETTCAP answers a key with what pressing it sends now: the arrow keys and
// Home in the CSI form until DECCKM selects SS3, by terminfo or termcap name.
TEST(testXTGETTCAPAnswersCursorKeysAsTheyAreSentNow)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q6b63757531;6b686f6d65;6b75;6b6631\033\\"); // kcuu1;khome;ku;kf1
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+r6b63757531=1B5B41;6b686f6d65=1B5B48;6b75=1B5B41;6b6631=1B4F50\033\\");
    feed(t, "\033[?1h");
    feed(t, "\033P+q6b63757531;6b686f6d65;6b75;6b6631\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+r6b63757531=1B4F41;6b686f6d65=1B4F48;6b75=1B4F41;6b6631=1B4F50\033\\");
}

// A modified key's answer follows modifyCursorKeys and modifyFunctionKeys.
TEST(testXTGETTCAPAnswersModifiedKeysAtTheModifyLevels)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q6b524954;6b663137;6b55503\033\\"); // kRIT;kf17;kUP5 (the last is malformed hex)
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+r6b524954=1B5B313B3243;6b663137=1B5B31353B327E;\033\\");
    feed(t, "\033[>1;3m\033[>2;3m");
    feed(t, "\033P+q6b524954;6b663137;6b55503\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+r6b524954=1B5B3E313B3243;6b663137=1B5B3E31353B327E;\033\\");
}

// The key answers are the bytes the key encoder sends for the same key, mode
// and level.
TEST(testXTGETTCAPKeyAnswersMatchTheKeyEncoder)
{
    struct Probe {
        const char* name;
        int kind;
        int symbol;
        uint32_t modifiers;
    };
    static const Probe probes[] = {
        { "kcuu1", 1, 'A', 0 },
        { "kend", 1, 'F', 0 },
        { "kLFT", 1, 'D', Ubiquitty::Keys::Keymap::keymapModifierShift },
        { "kRIT5", 1, 'C', Ubiquitty::Keys::Keymap::keymapModifierCtrl },
        { "kHOM3", 1, 'H', Ubiquitty::Keys::Keymap::keymapModifierAlternate },
        { "kf4", 2, 4, 0 },
        { "kf12", 2, 12, 0 },
        { "kf14", 2, 2, Ubiquitty::Keys::Keymap::keymapModifierShift },
        { "kf29", 2, 5, Ubiquitty::Keys::Keymap::keymapModifierCtrl },
    };
    for (const bool isApplicationMode : { false, true }) {
        for (const int level : { 0, 1, 2, 3 }) {
            for (const Probe& probe : probes) {
                Ubiquitty::Terminal t;
                Ubiquitty::hardReset(t);
                if (isApplicationMode)
                    feed(t, "\033[?1h");
                char setLevel[32];
                std::snprintf(setLevel, sizeof(setLevel), "\033[>1;%dm\033[>2;%dm", level, level);
                feed(t, setLevel);
                std::string request = "\033P+q";
                for (const char* c = probe.name; *c; ++c) {
                    char hex[3];
                    std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(*c));
                    request += hex;
                }
                request += "\033\\";
                feed(t, request.c_str());
                char outputBuffer[256];
                drain(t, outputBuffer);
                std::array<char, 32> encoded {};
                const int length = Ubiquitty::Keys::Keymap::encodeXtermSpecial(probe.kind, probe.symbol, probe.modifiers,
                    isApplicationMode ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, level, encoded);
                std::string want = request.substr(4, request.size() - 6) + "=";
                for (int index = 0; index < length; ++index) {
                    char hex[3];
                    std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned char>(encoded[static_cast<std::size_t>(index)]));
                    want += hex;
                }
                CHECK_MSG(std::string_view(outputBuffer) == "\033P1+r" + want + "\033\\", probe.name);
            }
        }
    }
}

TEST(testXTGETTCAPShortAndLongNamesSameValue)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int n;
    /// "name" = 6e616d65 → must yield the same "ubiquitty" body as TN.
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q6e616d65\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    // The cap echo is the hex actually queried; the value after '=' is the
    // hex of "xterm-ubiquitty", the TERM the app sets.
    CHECK(strstr(outputBuffer, "6e616d65=787465726D2D756269717569747479") != nullptr);

    /// "TN" short form for comparison.
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q544e\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "544e=787465726D2D756269717569747479") != nullptr);
}

// XTSETTCAP accepts a name the entry goes by and rings the bell for an empty
// name or one it cannot find; a session that refuses capability operations
// ignores it.
TEST(testXTSETTCAPRingsTheBellForANameItCannotFind)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.allowTerminalCapabilityOperations = true;
    feed(t, "\033P+p787465726d2d756269717569747479\033\\"); // xterm-ubiquitty
    CHECK(!t.bellPending);
    feed(t, "\033P+p\033\\");
    CHECK(t.bellPending);
    t.bellPending = false;
    feed(t, "\033P+p6e6f2d737563682d7465726d696e616c\033\\"); // no-such-terminal
    CHECK(t.bellPending);
    t.bellPending = false;
    t.allowTerminalCapabilityOperations = false;
    feed(t, "\033P+p\033\\");
    CHECK(!t.bellPending);
}

// XTGETXRES answers the resources it holds in one reply, values in hex, and
// stops at the first it does not have, echoing that name with no value.
TEST(testXTGETXRESAnswersTheResourcesItHolds)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    t.allowTerminalCapabilityOperations = true;
    t.scrollbackLimit = 1024;
    feed(t, "\033P+Q736176654C696E6573;616C6C6F77546361704F7073\033\\"); // saveLines;allowTcapOps
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+R736176654C696E6573=31303234;616C6C6F77546361704F7073=74727565\033\\");
    feed(t, "\033P+Q6E6F537563684E616D65;736176654C696E6573\033\\"); // noSuchName;saveLines
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P0+R6E6F537563684E616D65\033\\");
    feed(t, "\033P+Q736176654C696E6573;6E6F537563684E616D65\033\\"); // saveLines;noSuchName
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+R736176654C696E6573=31303234;6E6F537563684E616D65\033\\");
    // allowWindowOps;decTerminalID, at the VT420 level.
    t.allowWindowOperations = false;
    t.conformanceLevel = Ubiquitty::VT::ConformanceLevel::VT400;
    feed(t, "\033P+Q616C6C6F7757696E646F774F7073;6465635465726D696E616C4944\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+R616C6C6F7757696E646F774F7073=66616C7365;6465635465726D696E616C4944=343230\033\\");
    t.allowTerminalCapabilityOperations = false;
    feed(t, "\033P+Q736176654C696E6573\033\\");
    CHECK(drain(t, outputBuffer) == 0);
}

TEST(testXTGETTCAPEmptyCapabilitySilent)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q\033\\");
    n = drain(t, outputBuffer);
    // Acceptable: zero bytes (xterm behaviour) or a single miss frame.
    if (n != 0) {
        // If anything was emitted, it must be the canonical miss frame.
        CHECK(n == 7);
        CHECK(outputBuffer[0] == 0x1B && outputBuffer[1] == 'P');
        CHECK(outputBuffer[2] == '0' && outputBuffer[3] == '+' && outputBuffer[4] == 'r');
        CHECK(outputBuffer[5] == 0x1B && outputBuffer[6] == '\\');
    } else {
        CHECK(n == 0);
    }
}

TEST(testXTGETTCAPConsecutiveQueriesStateClean)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int n;
    Ubiquitty::hardReset(t);

    // First query: TN. Drain so the next probe starts on a clean buffer.
    feed(t, "\033P+q544e\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "544e=787465726D2D756269717569747479") != nullptr);

    // Second query: Co. Response must contain only the Co echo, not
    // TN's leftover cap hex.
    feed(t, "\033P+q436f\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "436f=323536") != nullptr);
    CHECK(strstr(outputBuffer, "544e") == nullptr); // no contamination

    // Third query: a miss. Response must be the miss frame, no echoes.
    feed(t, "\033P+q5a5a\033\\");
    n = drain(t, outputBuffer);
    CHECK(n == 7);
    CHECK(outputBuffer[2] == '0');
}

/// SOS (ESC X … ESC \) — string consumer; payload must be
/// discarded, parser must return to ground, no cell mutation.
TEST(testSOSConsumedSilently)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // CR so the cursor is anchored at column 0 before we look.
    feed(t, "\r");
    int startX = t.screens.active->cursorX;
    int startY = t.screens.active->cursorY;
    feed(t, "\033Xpayload-text\033\\");
    // SOS is a string consumer: the payload reaches no cell and the cursor
    // does not move over it.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(t.screens.active->cursorX == startX);
    CHECK(t.screens.active->cursorY == startY);
}

TEST(testC1OSC8bit)
{
    Ubiquitty::Terminal t;
    static const unsigned char seq[] = { 0x9D, '2', ';', 'h', 'e', 'l', 'l', 'o', 0x07 };
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, seq);
    CHECK(t.titleChanged == true);
    CHECK(t.titleUTF8 == "hello");
}

TEST(testOSCAllowlistGate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    seqGateClear(t);

    // Default (no lists): OSC 2 sets the title.
    feed(t, "\033]2;hello\033\\");
    CHECK(t.titleUTF8 == "hello");

    /// Deny OSC 2: a new title is dropped (the old one stays).
    int deny[] = { 2 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, deny, 1));
    feed(t, "\033]2;blocked\033\\");
    CHECK(t.titleUTF8 == "hello");
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, nullptr, 0));

    /// Allow-list {0}: OSC 2 is now not in the exclusive set → dropped; OSC 0
    /// (also a title) is allowed.
    int allow[] = { 0 };
    CHECK(Ubiquitty::OSC::setOSCAllowList(t, allow, 1));
    feed(t, "\033]2;nope\033\\");
    CHECK(t.titleUTF8 == "hello");
    feed(t, "\033]0;yes\033\\");
    CHECK(t.titleUTF8 == "yes");

    /// Deny wins over allow: add OSC 0 to the deny list while it's allowed.
    int deny0[] = { 0 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, deny0, 1));
    feed(t, "\033]0;blockedagain\033\\");
    CHECK(t.titleUTF8 == "yes");

    seqGateClear(t);
}

TEST(testOSCListParse)
{
    int outputBuffer[8];
    int n = Ubiquitty::OSC::parseOSCList("4, 52 1337", outputBuffer, 8);
    CHECK(n == 3);
    CHECK(outputBuffer[0] == 4 && outputBuffer[1] == 52 && outputBuffer[2] == 1337);

    // Empty / whitespace-only → 0.
    CHECK(Ubiquitty::OSC::parseOSCList("   ", outputBuffer, 8) == 0);
    CHECK(Ubiquitty::OSC::parseOSCList("", outputBuffer, 8) == 0);

    // Mixed junk: "abc 7 x9y 11" → {7, 11} (x9y is dropped — no leading
    // digit before the junk).
    n = Ubiquitty::OSC::parseOSCList("abc 7 x9y 11", outputBuffer, 8);
    CHECK(n == 2);
    CHECK(outputBuffer[0] == 7 && outputBuffer[1] == 11);
}

TEST(testOSC1337CursorShape)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    using Ubiquitty::Cursor::Style;
    feed(t, "\033]1337;CursorShape=1\007"); // bar; default style blinks
    CHECK(t.cursorStyle == Style::BlinkingBar);
    feed(t, "\033]1337;CursorShape=2\007"); // underline (still blink)
    CHECK(t.cursorStyle == Style::BlinkingUnderline);
    feed(t, "\033]1337;CursorShape=0\007"); // block (still blink)
    CHECK(t.cursorStyle == Style::BlinkingBlock);
    feed(t, "\033[2 q"); // DECSCUSR steady block
    CHECK(t.cursorStyle == Style::SteadyBlock);
    feed(t, "\033]1337;CursorShape=2\007"); // underline, now steady
    CHECK(t.cursorStyle == Style::SteadyUnderline);
}

TEST(testOSC1337SetUserVariable)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // The iTerm2 form is "SetUserVar=<name>=<base64>", so a payload of "bar"
    // base64-encoded arrives as the value of user.foo.
    feed(t, "\033]1337;SetUserVar=foo=YmFy\007");
    auto foo = Ubiquitty::userVariable(&t, "foo");
    CHECK(foo && *foo == "bar");

    // Overwrite: foo=<base64("hello")> → "hello"
    feed(t, "\033]1337;SetUserVar=foo=aGVsbG8=\007");
    foo = Ubiquitty::userVariable(&t, "foo");
    CHECK(foo && *foo == "hello");

    // A second name coexists.
    feed(t, "\033]1337;SetUserVar=baz=d29ybGQ=\007"); // "world"
    auto baz = Ubiquitty::userVariable(&t, "baz");
    CHECK(baz && *baz == "world");
    foo = Ubiquitty::userVariable(&t, "foo");
    CHECK(foo && *foo == "hello"); // unaffected

    // Empty value clears the slot.
    feed(t, "\033]1337;SetUserVar=foo=\007");
    CHECK(Ubiquitty::userVariable(&t, "foo") == std::nullopt);
    CHECK(Ubiquitty::userVariable(&t, "baz") != std::nullopt); // others survive

    // Unset name → nullptr.
    CHECK(Ubiquitty::userVariable(&t, "nope") == std::nullopt);

    // Malformed base64 is ignored (slot stays cleared / unset).
    feed(t, "\033]1337;SetUserVar=mal=not*base64!\007");
    CHECK(Ubiquitty::userVariable(&t, "mal") == std::nullopt);

    // Missing '=' value separator → ignored.
    feed(t, "\033]1337;SetUserVar=noeq\007");
    CHECK(Ubiquitty::userVariable(&t, "noeq") == std::nullopt);
}

TEST(testOSC1337UserVarOverflow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Fill every slot (userVariableCapacity == 16): vN = base64("vN").
    char seq[64];
    for (int i = 0; i < Ubiquitty::Terminal::userVariableCapacity; i++) {
        char raw[16];
        int rawLength = snprintf(raw, sizeof(raw), "v%d", i);
        // base64 of the short raw string, computed inline via the same alphabet.
        static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char b64[16];
        int o = 0;
        for (int j = 0; j < rawLength; j += 3) {
            unsigned v = (unsigned char)raw[j] << 16;
            int rem = rawLength - j;
            if (rem > 1)
                v |= (unsigned char)raw[j + 1] << 8;
            if (rem > 2)
                v |= (unsigned char)raw[j + 2];
            b64[o++] = alpha[(v >> 18) & 0x3F];
            b64[o++] = alpha[(v >> 12) & 0x3F];
            b64[o++] = (rem > 1) ? alpha[(v >> 6) & 0x3F] : '=';
            b64[o++] = (rem > 2) ? alpha[v & 0x3F] : '=';
        }
        b64[o] = 0;
        char name[16];
        snprintf(name, sizeof(name), "k%d", i);
        snprintf(seq, sizeof(seq), "\033]1337;SetUserVar=%s=%s\007", name, b64);
        feed(t, seq);
    }
    // All slots set.
    CHECK(Ubiquitty::userVariable(&t, "k0") != std::nullopt);
    CHECK(Ubiquitty::userVariable(&t, "k31") != std::nullopt);

    // Next distinct name overflows: dropped, no crash, existing intact.
    feed(t, "\033]1337;SetUserVar=k32=YmFy\007"); // "bar"
    CHECK(Ubiquitty::userVariable(&t, "k32") == std::nullopt);
    CHECK(Ubiquitty::userVariable(&t, "k0") != std::nullopt);

    // Overwriting an EXISTING name still works when the table is full.
    feed(t, "\033]1337;SetUserVar=k0=d29ybGQ=\007"); // "world"
    auto k0 = Ubiquitty::userVariable(&t, "k0");
    CHECK(k0 && *k0 == "world");
}

TEST(testOSC1337UserVarLongNameAndValue)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string longName(96, 'x');
    std::string seq = "\033]1337;SetUserVar=" + longName + "=YmFy\007";
    feed(t, seq.c_str());
    auto value = Ubiquitty::userVariable(&t, longName);
    CHECK(value && *value == "bar");

    std::string big = "\033]1337;SetUserVar=big=";
    for (int i = 0; i < 100; i++)
        big += "QUFB";
    big += '\007';
    feed(t, big.c_str());
    auto stored = Ubiquitty::userVariable(&t, "big");
    CHECK(stored && stored->size() == 300);
}

TEST(testOSC1337UserVarValueCapped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // 600 * "QUFB" = 2400 base64 chars -> 1800 decoded 'A' bytes (all ASCII, so the
    // cut lands cleanly at the byte cap), well over maxUserVarValueLength (1024).
    std::string big = "\033]1337;SetUserVar=v=";
    for (int i = 0; i < 600; i++)
        big += "QUFB";
    big += '\007';
    feed(t, big.c_str());

    auto v = Ubiquitty::userVariable(&t, "v");
    CHECK(v && v->size() == Ubiquitty::Terminal::maxUserVarValueLength); // 1800 -> capped at 1024
    CHECK(v && v->size() <= Ubiquitty::Terminal::maxUserVarValueLength);
}

TEST(testOSCEnormousParameterUsesMaxOSCNonImageSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // A Ps whose running value exceeds 100000 is not a large-payload code; the
    // per-command ceiling guard (which scans what the accumulator already holds)
    // returns the non-image cap without reading on.
    feed(t, "\033]999999");
    feed(t, "abc\033\\");
    CHECK(t.parser.osc.length == 9); // payload stored unclipped (it is tiny)
}

TEST(testOSCEightDigitParameterEndingInDigitIsClippedToMaxOSCNonImageSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // A Ps exactly as long as the scan bound, with more digits still to come,
    // is not a large-payload code either — the guard reads the next byte and
    // clips to the non-image ceiling.
    feed(t, "\033]00000012"); // 8 digits — a Ps as long as the scan bound
    feed(t, "3"); // the 9th digit lands; the next append re-scans the stored 9 bytes
    feed(t, "45x\033\\");
    CHECK(t.parser.osc.length == 12); // stored unclipped (tiny)
    CHECK(t.parser.osc.capacity == Ubiquitty::VT::oscBufferInitialCapacity);
}

TEST(testOSCLinuxEmulationClipsPerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Linux);
    // Linux emulation runs the OSC payload per byte (no batched runs), so the
    // per-byte ceiling clip and its one-shot boundary diagnostic fire here.
    std::string title = "\033]0;";
    title.append(Ubiquitty::VT::maxOSCNonImageSize, 'a');
    feed(t, title);
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);
    feed(t, "b"); // past the ceiling — dropped
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);
    feed(t, "\033\\");
    CHECK(t.titleChanged == true);
}

TEST(testOSCTitleOverflowClipsAndShrinks)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // A title payload past the non-image ceiling: the batched run clips at the
    // ceiling, the boundary diagnostic fires once, and the next run stores
    // nothing (already at the ceiling). The grown buffer is released after
    // dispatch.
    std::string payload = "\033]0;";
    payload.append(Ubiquitty::VT::maxOSCNonImageSize, 'a');
    feed(t, payload);
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);
    feed(t, "bbbbbbbbbb");
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);
    feed(t, "\033\\");
    CHECK(t.titleChanged == true);
    CHECK(t.parser.osc.capacity == Ubiquitty::VT::oscBufferInitialCapacity);
}

TEST(testOSCEmptyPayloadRunsThroughEnd)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // BEL terminator with no payload: actOSCEnd runs on a zero-capacity buffer.
    feed(t, "\033]\007");
    CHECK(t.titleChanged == false);
    feed(t, "\033]\033\\"); // ST with no payload
    CHECK(t.titleChanged == false);
}

// The non-image OSC ceiling (64 KB) clips a batched payload run at the
// boundary: the stored length is exactly the ceiling and the dispatch still
// runs on the terminator.
TEST(testMaxOSCNonImageSizeClipsPayload)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string big = "\033]0;x" + std::string(70000, 'y') + "\007";
    feed(t, big.c_str());
    CHECK(t.parser.osc.length == 65536); // clipped at the 64 KB ceiling
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

// A NUL reaches the OSC buffer verbatim — the payload accumulator ends a run on
// 0x07, 0x1b, 0x18, 0x1a and 0x9c, and on nothing else — so what a dynamic-colour
// handler does with the bytes behind one is decided by whether it was handed a
// length or a pointer.
// These cases pin the answer for both shapes of the `?` test: OSC 10's, which
// measures its field, and OSC 17's, which reads one byte past the '?'.
TEST(testDynamicColorPayloadCarriesItsNULs)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    Ubiquitty::hardReset(t);

    // A query with bytes hidden behind a NUL is trailing garbage, not a query.
    feed(t, "\033]10;#445566\033\\");
    static constexpr char oscTenQuery[] = "\033]10;?\0junk\033\\";
    feed(t, std::string_view(oscTenQuery, sizeof(oscTenQuery) - 1));
    CHECK(drain(t, buffer) == 0);

    // The same for a set: the spec ends where the payload does, not at the NUL.
    static constexpr char oscTenSet[] = "\033]10;#112233\0junk\033\\";
    feed(t, std::string_view(oscTenSet, sizeof(oscTenSet) - 1));
    CHECK(t.palette.defaultForeground.red == 0x4444);

    // OSC 17 spells the same test one byte at a time and answers the same way.
    feed(t, "\033]17;rgb:1111/2222/3333\033\\");
    static constexpr char oscSeventeenQuery[] = "\033]17;?\0junk\033\\";
    feed(t, std::string_view(oscSeventeenQuery, sizeof(oscSeventeenQuery) - 1));
    CHECK(drain(t, buffer) == 0);
}

// A dynamic-colour field is bounded by the payload, not by a staging buffer, so
// the length of a spec is XParseColor's business alone. CIELab writes three
// decimals and X11 puts no limit on their precision, so a spec that a fixed
// buffer would have to drop is well within what the grammar admits.
TEST(testDynamicColorSpecLengthIsTheParsersBusiness)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]10;CIELab:50.00000000000000000000/20.0000000000000000000/30.000000000000000000\033\\");
    CHECK(t.palette.defaultForeground.red == 0x0909);
    CHECK(t.palette.defaultForeground.green == 0x8282);
    CHECK(t.palette.defaultForeground.blue == 0xAEAE);
}

TEST(testLabelsDropControlsAndKeepTextAfterNUL)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    static constexpr char label[] = "\033]0;a\001b\nc\td\0e\177f\xc3\xa9\033\\";
    feed(t, std::string_view(label, sizeof(label) - 1));
    CHECK(t.titleUTF8 == "abcdefé");
    CHECK(t.iconNameUTF8 == "abcdefé");
    feed(t, "\033[20t\033[21t");
    char reply[128];
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033]Labcdefé\033\\\033]labcdefé\033\\");
    feed(t, "\033[>0t\033]2;61006209630A647F65\033\\");
    CHECK(t.titleUTF8 == "abcde");
}

TEST(testXTGETTCAPReportsRGBComponentBits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033P+q524742;436f\033\\");
    char reply[128];
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033P1+r524742=38;436f=323536\033\\");
    t.allowTerminalCapabilityOperations = false;
    feed(t, "\033P+q524742\033\\");
    CHECK(drain(t, reply) == 0);
}

// OSC 4 and OSC 5 bound a spec the same way the dynamic colours do, so they
// answer the same two questions: a spec is as long as the grammar admits, and
// a NUL is a byte of the payload rather than the end of it.
TEST(testIndexedPaletteSpecIsBoundedByThePayload)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    Ubiquitty::hardReset(t);

    // A CIELab spec longer than any fixed staging buffer still sets its slot.
    feed(t, "\033]4;1;CIELab:50.00000000000000000000/20.0000000000000000000/30.000000000000000000\033\\");
    CHECK(t.palette.indexed[1].red == 0x0909);
    CHECK(t.palette.indexed[1].green == 0x8282);
    CHECK(t.palette.indexed[1].blue == 0xAEAE);

    // A query with bytes hidden behind a NUL is trailing garbage, not a query.
    static constexpr char query[] = "\033]4;1;?\0junk\033\\";
    feed(t, std::string_view(query, sizeof(query) - 1));
    CHECK(drain(t, buffer) == 0);

    // The same for OSC 5's special colors.
    feed(t, "\033]5;0;CIELab:50.00000000000000000000/20.0000000000000000000/30.000000000000000000\033\\");
    CHECK(t.palette.special[0].red == 0x0909);
    static constexpr char specialQuery[] = "\033]5;0;?\0junk\033\\";
    feed(t, std::string_view(specialQuery, sizeof(specialQuery) - 1));
    CHECK(drain(t, buffer) == 0);
}

// OSC 21337 values escape their own ';' as "\;", which is the only thing that
// distinguishes a separator from a byte of a value. A status text carrying an
// escaped ';' therefore swallows everything after it — including text shaped
// like the next pair — and an indicator behind one is not an indicator.
TEST(testTabStatusEscapedSeparatorEndsNoPair)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // A literal ';' does separate, so the indicator behind it is read.
    feed(t, "\033]21337;status=x;indicator=#112233\007");
    const std::optional<Ubiquitty::ColorRGBA> separated = Ubiquitty::tabStatusColor(&t);
    CHECK(separated && separated->red == 0x11 && separated->green == 0x22 && separated->blue == 0x33);

    // Escaped, the same bytes are one status value and set no indicator, so the
    // colour above is still the one in force.
    feed(t, "\033]21337;indicator\007"); // clear
    CHECK(!Ubiquitty::tabStatusColor(&t));
    feed(t, "\033]21337;status=x\\;indicator=#445566\007");
    CHECK(!Ubiquitty::tabStatusColor(&t));
}

// allowColorOperations and allowTitleOperations are the host-level refusals: a
// host that turns one off means the terminal must not act on that class of code
// at all, queries included, since a reply is what tells a remote the refusal
// happened. hardReset sets both true and HostPolicy is what clears them, so the
// false side is reachable only from a host — and the capability-policy suite
// sets both true throughout, because it is testing the layer beneath these.
TEST(testHostLevelColorRefusalCoversSetAndQuery)
{
    Ubiquitty::Terminal t;
    char buffer[128];
    Ubiquitty::hardReset(t);

    feed(t, "\033]10;#445566\033\\");
    feed(t, "\033]4;1;#112233\033\\");
    CHECK(t.palette.defaultForeground.red == 0x4444);
    CHECK(t.palette.indexed[1].red == 0x1111);
    (void)drain(t, buffer);

    t.allowColorOperations = false;

    // No set lands, on a dynamic colour or an indexed one.
    feed(t, "\033]10;#778899\033\\");
    feed(t, "\033]4;1;#aabbcc\033\\");
    CHECK(t.palette.defaultForeground.red == 0x4444);
    CHECK(t.palette.indexed[1].red == 0x1111);

    // No reset lands either — 110 and 104 are refused with the setters.
    feed(t, "\033]110\033\\");
    feed(t, "\033]104;1\033\\");
    CHECK(t.palette.defaultForeground.red == 0x4444);
    CHECK(t.palette.indexed[1].red == 0x1111);

    // And no query answers: a reply would report the palette the refusal exists
    // to keep private, and would tell the sender which terminal refused it.
    feed(t, "\033]10;?\033\\");
    feed(t, "\033]4;1;?\033\\");
    CHECK(drain(t, buffer) == 0);
}

TEST(testHostLevelTitleRefusalKeepsTheLabel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]0;kept\033\\");
    CHECK(t.titleUTF8 == "kept");

    t.allowTitleOperations = false;
    feed(t, "\033]0;replaced\033\\");
    feed(t, "\033]1;replaced\033\\");
    feed(t, "\033]2;replaced\033\\");
    CHECK(t.titleUTF8 == "kept");
}

// An empty OSC 7 withdraws the report before it, host and directory both, so a
// shell that stops naming a place is not left described by it.
TEST(testEmptyWorkingDirectoryReportWithdrawsTheLast)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]7;file://elsewhere/home/user\007");
    CHECK(t.currentDirectory == "/home/user");
    CHECK(t.reportedHost == "elsewhere");
    feed(t, "\033]7;\007");
    CHECK(t.currentDirectory.empty());
    CHECK(t.reportedHost.empty());
}

// X11's colour names reach OSC 4 and OSC 10 as XParseColor resolves them, and
// a colour that does not parse ends an OSC 4 list where it stands.
TEST(testColorNamesSetColorsAndABadOneEndsTheList)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char reply[256];
    feed(t, "\033]4;1;light sky blue\007\033]4;1;?\007");
    CHECK(drain(t, reply) > 0);
    CHECK(std::string_view(reply) == "\033]4;1;rgb:8787/cece/fafa\007");

    feed(t, "\033]4;2;red\007\033]4;3;nosuch;2;blue\007\033]4;2;?\007");
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033]4;2;rgb:ffff/0000/0000\007");

    feed(t, "\033]10;Blue\007\033]10;?\007");
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033]10;rgb:0000/0000/ffff\007");
}

// The sequence that showed it: two named colours in one OSC 4, a reset of
// the first alone, and both asked back in one query. The second keeps the
// colour it was given; the reset touches only the index it names.
TEST(testResettingOneIndexedColorLeavesTheOthersItsListSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char reply[256];
    feed(t, "\033]4;1;red;2;blue\007\033]104;1\007\033]4;1;?;2;?\007");
    drain(t, reply);
    char want[160];
    std::snprintf(want, sizeof(want), "\033]4;1;rgb:%04x/%04x/%04x\007\033]4;2;rgb:0000/0000/ffff\007", t.paletteBaseline.indexed[1].red,
        t.paletteBaseline.indexed[1].green, t.paletteBaseline.indexed[1].blue);
    CHECK(std::string_view(reply) == want);
}

// OSC 22 takes the X cursor-font names xterm uses beside the CSS ones.
TEST(testPointerShapeTakesXCursorNames)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]22;watch\033\\");
    CHECK(Ubiquitty::OSC::appRequestedPointerShape(t) == Ubiquitty::MousePointerShape::Wait);
    feed(t, "\033]22;left_ptr\033\\");
    CHECK(Ubiquitty::OSC::appRequestedPointerShape(t) == Ubiquitty::MousePointerShape::Default);
    feed(t, "\033]22;xterm\033\\");
    CHECK(Ubiquitty::OSC::appRequestedPointerShape(t) == Ubiquitty::MousePointerShape::Text);
}

// An OSC reply ends with the terminator its request ended with: BEL for BEL,
// ST for ST (xterm do_osc). Each reply family is asked both ways.
static std::string oscReplyTo(Ubiquitty::Terminal& t, const std::string& request)
{
    char outputBuffer[512];
    feed(t, request.c_str());
    const int n = drain(t, outputBuffer);
    return std::string(outputBuffer, static_cast<std::size_t>(n));
}

TEST(testColorQueriesEndWithTheRequestsTerminator)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (const char* query : { "4;1;?", "5;0;?", "10;?", "11;?", "12;?" }) {
        const std::string belReply = oscReplyTo(t, std::string("\033]") + query + "\007");
        CHECK_MSG(belReply.starts_with("\033]") && belReply.ends_with('\007'), query);
        CHECK_MSG(belReply.find("\033\\") == std::string::npos, query);
        const std::string stReply = oscReplyTo(t, std::string("\033]") + query + "\033\\");
        CHECK_MSG(stReply.ends_with("\033\\") && stReply.find('\007') == std::string::npos, query);
        CHECK_MSG(belReply.substr(0, belReply.size() - 1) == stReply.substr(0, stReply.size() - 2), query);
    }
}

TEST(testFontAndPointerQueriesEndWithTheRequestsTerminator)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(oscReplyTo(t, "\033]50;?\007") == "\033]50;\007");
    CHECK(oscReplyTo(t, "\033]50;?\033\\") == "\033]50;\033\\");
    CHECK(oscReplyTo(t, "\033]22;?pointer,bogus\007") == "\033]22;1,0\007");
    CHECK(oscReplyTo(t, "\033]22;?pointer,bogus\033\\") == "\033]22;1,0\033\\");
}

TEST(testClipboardRepliesEndWithTheRequestsTerminator)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    AsyncClipboard clip;
    t.clipboardClient = &clip;
    // An answer the policy refuses on the spot.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
    CHECK(oscReplyTo(t, "\033]52;c;?\007") == "\033]52;c;\007");
    CHECK(oscReplyTo(t, "\033]52;c;?\033\\") == "\033]52;c;\033\\");
    // An answer the frontend completes after the request's dispatch has ended.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    for (const auto& [terminator, want] : { std::pair { "\007", "\033]52;c;SGk=\007" }, std::pair { "\033\\", "\033]52;c;SGk=\033\\" } }) {
        CHECK(oscReplyTo(t, std::string("\033]52;c;?") + terminator).empty());
        clip.provideClipboardReadBytes(Ubiquitty::ClipboardBuffer::Clipboard, std::span<const char>("Hi", 2));
        Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
        CHECK(oscReplyTo(t, "") == want);
    }
    // A reply framed outside any OSC request ends with ST.
    CHECK(Ubiquitty::oscReplyTerminator(t) == "\033\\");
    t.clipboardClient = nullptr;
}

// Konsole's `OSC 50 ; CursorShape=N` sets the cursor's shape, keeping its
// blink, and is never taken for a font name.
TEST(testKonsoleCursorShapeSetsTheCursorNotTheFont)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]50;Menlo\007");
    feed(t, "\033]50;CursorShape=1\007");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyBar);
    CHECK(t.font == "Menlo");
    feed(t, "\033[1 q\033]50;CursorShape=2\033\\");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingUnderline);
    feed(t, "\033]50;CursorShape=0\007");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingBlock);
    feed(t, "\033]50;CursorShape=7\007");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingBlock);
    CHECK(t.font == "Menlo");
}

// OSC 60, 61 and 62 answer from the gates Core enforces: the categories whose
// flag is on, the operations of a category refused now, and every operation a
// category gates. A name that is no category gets the bare code; the reply
// ends with the request's terminator.
TEST(testOSCAllowedOperationQueriesReportTheGates)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[1024] = {};
    t.allowWindowOperations = false;
    feed(t, "\033]60\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]60;allowColorOps,allowFontOps,allowMouseOps,allowTcapOps,allowTitleOps\007") == 0);
    feed(t, "\033]60;x\007");
    CHECK(drain(t, buffer) == 0);

    feed(t, "\033]62;allowTcapOps\033\\");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]62;SetTcap,GetTcap\033\\") == 0);
    feed(t, "\033]62;allowWindowOps\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]62;RestoreWin,MinimizeWin,SetWinPosition,SetWinSizePixels,RaiseWin,LowerWin,RefreshWin,SetWinSizeChars,MaximizeWin,FullscreenWin,GetWinState,GetWinPosition,GetWinSizePixels,GetWinSizeChars,GetScreenSizeChars,GetIconTitle,GetWinTitle,PushTitle,PopTitle,SetWinLines,ColumnMode,GetChecksum,SetChecksum,GetSelection,SetSelection,SetXprop\007") == 0);
    feed(t, "\033]62;nosuch\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]62\007") == 0);

    t.allowWindowOperations = true;
    feed(t, "\033]61;ALLOWWINDOWOPS\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]61;SetXprop,GetSelection,SetChecksum\007") == 0);
    t.allowColorOperations = false;
    feed(t, "\033]61;allowColorOps\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]61;SetColor,GetColor,GetAnsiColor\007") == 0);
    feed(t, "\033]61;allowColorOps;allowFontOps\007");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033]61\007") == 0);
    feed(t, "\033]61\007");
    CHECK(drain(t, buffer) == 0);
}

TEST(testAllowableWindowReportIsNotTruncated)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const std::string reply = oscReplyTo(t, "\033]62;allowWindowOps\007");
    CHECK(reply.starts_with("\033]62;RestoreWin,MinimizeWin,"));
    CHECK(reply.ends_with(",SetSelection,SetXprop\007"));
    CHECK(reply.size() > 256);
}

TEST(kittyColorStackRestoresPaletteAndSelectionColors)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]10;blue\007\033]11;#123456\007\033]12;#abcdef\007");
    feed(t, "\033]4;0;#010203;128;#040506;255;#070809\007");
    feed(t, "\033]17;#112233\007\033]19;#445566\007");
    const auto saved = t.palette;
    feed(t, "\033[10#P\033]30001\033\\");
    CHECK(t.savedPalettePosition == 1);
    feed(t, "\033]10;red\007\033]11;black\007\033]12;white\007\033]104\007");
    feed(t, "\033]117\007\033]119\007");
    const auto epoch = t.paletteEpoch;
    feed(t, "\033]30101\033\\");
    CHECK(t.palette == saved);
    CHECK(t.paletteEpoch == epoch + 1);
    CHECK(t.selectionBackgroundSet && t.selectionForegroundSet);
    CHECK(t.selectionBackgroundRed == 0x1111 && t.selectionBackgroundGreen == 0x2222 && t.selectionBackgroundBlue == 0x3333);
    CHECK(t.selectionForegroundRed == 0x4444 && t.selectionForegroundGreen == 0x5555 && t.selectionForegroundBlue == 0x6666);
    CHECK(t.savedPalettePosition == 0);
    feed(t, "\033]10;?\007");
    char reply[128] {};
    CHECK(drain(t, reply) > 0);
    CHECK(std::string(reply) == "\033]10;rgb:0000/0000/ffff\007");
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(kittyColorStackSharesNestedSavesWithXtermAndPreservesWrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    const auto original = t.palette;
    feed(t, "ABCD\033]30001\007\033]10;blue\007\033[#P\033]10;red\007");
    feed(t, "\033]30101\007");
    CHECK(t.palette.defaultForeground.blue == 0xffff && t.palette.defaultForeground.red == 0);
    feed(t, "\033[#QE");
    CHECK(t.palette == original);
    CHECK(cell(t, 1, 0)->codePoint == 'E');
    CHECK(t.savedPalettePosition == 0);
    const auto epoch = t.paletteEpoch;
    feed(t, "\033]30101\007\033]30101\033\\");
    CHECK(t.paletteEpoch == epoch);
    for (std::size_t index = 0; index < Ubiquitty::Terminal::paletteStackDepth + 2; ++index)
        feed(t, "\033]30001\007");
    CHECK(t.savedPalettePosition == Ubiquitty::Terminal::paletteStackDepth);
    for (std::size_t index = 0; index < Ubiquitty::Terminal::paletteStackDepth + 2; ++index)
        feed(t, "\033]30101\007");
    CHECK(t.savedPalettePosition == 0);
    feed(t, "\033]30001\007\033c");
    CHECK(t.savedPaletteValid == 0);
}

// cursor_text is stored as the other dynamic colours are, so a pushed slot
// keeps it and a pop restores it, set or unset.
TEST(theColorStackSavesAndRestoresTheCursorTextColor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]21;cursor_text=#336699\033\\\033]30001\007\033]21;cursor_text\033\\");
    CHECK(!t.hasCursorTextColor);
    const auto epoch = t.paletteEpoch;
    feed(t, "\033]30101\007");
    CHECK(t.hasCursorTextColor);
    CHECK_EQ(t.cursorTextRed, 0x3333);
    CHECK_EQ(t.cursorTextGreen, 0x6666);
    CHECK_EQ(t.cursorTextBlue, 0x9999);
    CHECK(t.paletteEpoch != epoch);
    feed(t, "\033]21;cursor_text\033\\\033[#P\033]21;cursor_text=#123456\033\\\033[#Q");
    CHECK(!t.hasCursorTextColor);
}

TEST(kittyColorStackHonorsColorPolicyAndRejectsPayloads)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]30001;1\007\033]30001;x;y\007");
    CHECK(!t.savedPalettes);
    t.allowColorOperations = false;
    feed(t, "\033]30001\007");
    CHECK(!t.savedPalettes);
    t.allowColorOperations = true;
    const int blocked = 30001;
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, &blocked, 1));
    feed(t, "\033]30001\007");
    CHECK(!t.savedPalettes);
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, nullptr, 0));
    feed(t, "\033]10;blue\007\033]30001\007\033]10;red\007\033]30101;x\007");
    CHECK(t.savedPalettePosition == 1);
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    feed(t, "\033]30101\007\033]30001\007\033[#Q\033[#P");
    CHECK(t.savedPalettePosition == 1);
    CHECK(t.palette.defaultForeground.red == 0xffff && t.palette.defaultForeground.blue == 0);
    CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::SetColor);
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
    feed(t, "\033]30101\007");
    CHECK(t.savedPalettePosition == 0);
    CHECK(t.palette.defaultForeground.blue == 0xffff && t.palette.defaultForeground.red == 0);
    t.capabilityPolicy = nullptr;
}

TEST(testTermpropProgressPublishesTypedValuesAndResets)
{
    ProgressInvalidateClient client;
    Ubiquitty::Terminal terminal;
    terminal.windowClient = &client;
    feed(terminal, "\033]666;vte.progress.value=50\033\\");
    CHECK_EQ(terminal.progressState, 1);
    CHECK_EQ(terminal.progressValue, 50);
    CHECK_EQ(client.reports.size(), 1u);
    CHECK_EQ(client.invalidates, 1);
    feed(terminal, "\033]666;vte.progress.value=50\033\\");
    CHECK_EQ(client.reports.size(), 1u);
    feed(terminal, "\033]666;vte.progress.hint=2;vte.progress.value=30\033\\");
    CHECK_EQ(terminal.progressState, 2);
    CHECK_EQ(terminal.progressValue, 30);
    CHECK_EQ(client.reports.size(), 2u);
    feed(terminal, "\033]666;vte.progress.hint\033\\");
    CHECK_EQ(terminal.progressState, 1);
    CHECK_EQ(terminal.progressValue, 30);
    feed(terminal, "\033]666;vte.progress.\033\\");
    CHECK_EQ(terminal.progressState, 0);
    CHECK_EQ(terminal.termpropProgressHint, -1);
    CHECK_EQ(terminal.termpropProgressValue, -1);
    CHECK_EQ(client.reports.back().first, 0);
    CHECK_EQ(client.reports.back().second, -1);
}

TEST(testTermpropInvalidValuesResetOnlyTheNamedProperty)
{
    for (const char* value : { "-1", "101", "18446744073709551616", "", "50x", "+5", " 5", "5 ", "-0" }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, "\033]666;vte.progress.hint=4;vte.progress.value=70\033\\");
        feed(terminal, std::string("\033]666;vte.progress.value=") + value + "\033\\");
        CHECK_EQ(terminal.termpropProgressValue, -1);
        CHECK_EQ(terminal.termpropProgressHint, 4);
        CHECK_EQ(terminal.progressState, 0);
        feed(terminal, "\033]666;vte.progress.value=0\033\\");
        CHECK_EQ(terminal.progressState, 4);
        CHECK_EQ(terminal.progressValue, 0);
    }
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]9;4;2;60\007\033]666;vte.progress.hint=0\033\\");
    CHECK_EQ(terminal.termpropProgressHint, -1);
    CHECK_EQ(terminal.progressState, 1);
    CHECK_EQ(terminal.progressValue, 60);
    feed(terminal, "\033]666;vte.progress.value!;vte.unknown=5;vte.progress.hint=4\007");
    CHECK_EQ(terminal.progressState, 1);
    CHECK_EQ(terminal.progressValue, 60);
}

TEST(testTermpropQueriesUseOneEmptySTReply)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "ABCDEF\033]666;vte.progress.value?;unknown.property?\033\\");
    char reply[80];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]666\033\\");
    CHECK(terminal.screens.active->wrapPending);
    feed(terminal, "\033]666;vte.progress.value?\007\033]666;vte.progress.value?extra\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    feed(terminal, "\033 G\033]666;unknown.property?\033\\");
    const int count = drain(terminal, reply);
    CHECK(std::string_view(reply, static_cast<std::size_t>(count)) == "\235666\234");
}

TEST(testTermpropLengthLimitCountsUnicodeCodepoints)
{
    Ubiquitty::Terminal terminal;
    const std::string tail = ";vte.progress.value?";
    const std::size_t padding = 4096 - 4 - tail.size();
    std::string payload;
    for (std::size_t index = 0; index < padding; ++index)
        payload += "\xc3\xa9";
    char reply[80];
    feed(terminal, "\033]666;" + payload + tail + "\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]666\033\\");
    feed(terminal, "\033]666;x" + payload + tail + "\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testKittyColorControlSetsQueriesAndPreservesTitleAndWrap)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "\033]2;Window\007ABCDEF");
    feed(terminal, "\033]21;foreground=#ff0000;background=#0000ff;1=#00ff00;foreground=?;background=?;1=?\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;foreground=rgb:ff/00/00;background=rgb:00/00/ff;1=rgb:00/ff/00\033\\");
    CHECK(terminal.titleUTF8 == "Window");
    CHECK(terminal.screens.active->wrapPending);
    feed(terminal, "\033]10;?\007\033]4;1;?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]10;rgb:ffff/0000/0000\007\033]4;1;rgb:0000/ffff/0000\007");
}

TEST(testKittyColorControlResetsIndexedAndDynamicSelectionColors)
{
    Ubiquitty::Terminal terminal;
    terminal.paletteBaseline.indexed[42] = { 0x1212, 0x3434, 0x5656 };
    terminal.paletteBaseline.indexedAlpha[42] = 120;
    feed(terminal, "\033]21;42=red;selection_background=blue;selection_foreground=white\007");
    CHECK_EQ(terminal.palette.indexedAlpha[42], 255);
    CHECK(terminal.selectionBackgroundSet && terminal.selectionForegroundSet);
    feed(terminal, "\033]21;42;42=?;selection_background=;selection_background=?;selection_foreground;selection_foreground=?\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;42=rgb:12/34/56;selection_background=;selection_foreground=\033\\");
    CHECK_EQ(terminal.palette.indexedAlpha[42], 120);
    CHECK(!terminal.selectionBackgroundSet && !terminal.selectionForegroundSet);
    feed(terminal, "\033]21;42=;42=invalid\033\\");
    CHECK(terminal.palette.indexed[42] == terminal.paletteBaseline.indexed[42]);
}

TEST(testKittyColorControlEncodesUnknownNamesWithoutPadding)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]21;;nonsense=?;256=red;-1;visual_bell=?;unknown@name=?;\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;unknown=bm9uc2Vuc2U;unknown=MjU2;unknown=LTE;unknown=dmlzdWFsX2JlbGw;unknown=dW5rbm93bkBuYW1l\033\\");
}

TEST(testKittyColorControlUsesLinearClampedRGBIntensity)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]21;foreground=rgbi:+2/-1/.5;foreground=?\007");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;foreground=rgb:ff/00/80\007");
    feed(terminal, "\033]21;foreground=rgbi:0/0/nan;foreground=rgbi:1/0/0junk;foreground=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;foreground=rgb:ff/00/80\033\\");
}

TEST(testKittyColorControlQueriesAllPaletteEntries)
{
    Ubiquitty::Terminal terminal;
    std::string query = "\033]21";
    std::string expected = "\033]21";
    for (int index = 0; index < 256; ++index) {
        terminal.palette.indexed[static_cast<std::size_t>(index)] = { 0xffff, 0, 0 };
        query += ";" + std::to_string(index) + "=?";
        expected += ";" + std::to_string(index) + "=rgb:ff/00/00";
    }
    feed(terminal, query + "\033\\");
    expected += "\033\\";
    std::array<char, 8192> reply;
    const int length = Ubiquitty::drainOutput(&terminal, reply);
    CHECK(std::string_view(reply.data(), static_cast<std::size_t>(length)) == expected);
    feed(terminal, "\033 G\033]21;foreground=white;foreground=?\033\\");
    const int eightBitLength = Ubiquitty::drainOutput(&terminal, reply);
    CHECK(std::string_view(reply.data(), static_cast<std::size_t>(eightBitLength)) == "\23521;foreground=rgb:ff/ff/ff\234");
}

TEST(testKittyColorControlHonorsOperationAndCapabilityGates)
{
    Ubiquitty::Terminal terminal;
    const auto original = terminal.palette.defaultForeground;
    terminal.allowColorOperations = false;
    feed(terminal, "\033]21;foreground=red;foreground=?\033\\");
    char reply[512];
    CHECK_EQ(drain(terminal, reply), 0);
    CHECK(terminal.palette.defaultForeground == original);
    terminal.allowColorOperations = true;
    const int denied[] = { 21 };
    CHECK(Ubiquitty::OSC::setOSCDenyList(terminal, denied, 1));
    feed(terminal, "\033]21;foreground=red;foreground=?\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    CHECK(terminal.palette.defaultForeground == original);
    CHECK(Ubiquitty::OSC::setOSCDenyList(terminal, nullptr, 0));
    terminal.capabilityPolicy = &fakeCapabilityPolicy;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(terminal, "\033]21;foreground=red;foreground=?;unknown=?\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    CHECK(terminal.palette.defaultForeground == original);
    terminal.capabilityPolicy = nullptr;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
}

UBIQUITTY_TEST_MAIN()

TEST(testColorRepliesKeepRequestOrderAcrossOutputBuffers)
{
    for (const auto framing : { std::string_view(), std::string_view("\033 G") }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, framing);
        feed(terminal, "\033]10;#000000\033\\\033[5n\033]21;foreground=?\033\\\033]10;?\033\\\033[5n");
        std::string reply;
        std::array<char, 7> piece;
        for (int count; (count = Ubiquitty::drainOutput(&terminal, piece)) > 0;)
            reply.append(piece.data(), static_cast<std::size_t>(count));
        const std::string_view expected = framing.empty()
            ? "\033[0n\033]21;foreground=rgb:00/00/00\033\\\033]10;rgb:0000/0000/0000\033\\\033[0n"
            : "\x9b"
              "0n\x9d"
              "21;foreground=rgb:00/00/00\x9c\x9d"
              "10;rgb:0000/0000/0000\x9c\x9b"
              "0n";
        CHECK(reply == expected);
    }
}

TEST(testQueuedBulkReplyDoesNotBypassLaterCapabilityChecks)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]21;foreground=black;foreground=?\033\\");
    terminal.capabilityPolicy = &fakeCapabilityPolicy;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(terminal, "\033]10;?\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;foreground=rgb:00/00/00\033\\");
    terminal.capabilityPolicy = nullptr;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
}

TEST(testPaletteQueryRepliesOverflowIntoTheBoundedBulkQueue)
{
    for (const auto framing : { std::string_view(), std::string_view("\033 G") }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, framing);
        std::string query = "\033]4;";
        std::string expected;
        for (int index = 0; index < 256; ++index) {
            terminal.palette.indexed[index] = { 0x1234, 0x5678, 0x9abc };
            if (index)
                query += ';';
            query += std::to_string(index) + ";?";
            expected += framing.empty() ? "\033]4;" : "\2354;";
            expected += std::to_string(index) + ";rgb:1234/5678/9abc";
            expected += framing.empty() ? "\033\\" : "\234";
        }
        query += "\033\\\033[5n";
        expected += framing.empty() ? "\033[0n" : "\2330n";
        feed(terminal, query);
        std::string actual;
        std::array<char, 17> piece;
        for (int count; (count = Ubiquitty::drainOutput(&terminal, piece)) > 0;)
            actual.append(piece.data(), static_cast<std::size_t>(count));
        CHECK(actual == expected);
        CHECK_EQ(terminal.bulkOutput.length, 0);
    }
}

TEST(testKittyNotificationSupportQueryPreservesPendingNotification)
{
    Ubiquitty::Terminal terminal;
    CaptureShell shell;
    terminal.shellClient = &shell;
    feed(terminal, "\033]99;i=part:d=0;Title\033\\\033]99;i=query_1:p=?;\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]99;i=query_1:p=?;p=title,body:o=always,unfocused,invisible\033\\");
    CHECK_EQ(shell.notifyCalls, 0);
    feed(terminal, "\033]99;i=part:p=body;Body\033\\");
    CHECK_EQ(shell.notifyCalls, 1);
    CHECK(std::string_view(shell.notifyTitle) == "Title");
    CHECK(std::string_view(shell.notifyBody) == "Body");
    terminal.shellClient = nullptr;
}

TEST(testKittyNotificationQuerySanitizesIdentifiersAndHonorsReplyPolicy)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]99;i=a!b/c=d:p=?;\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]99;i=abcd:p=?;p=title,body:o=always,unfocused,invisible\033\\");
    terminal.capabilityPolicy = &fakeCapabilityPolicy;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(terminal, "\033]99;i=denied:p=?;\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    terminal.capabilityPolicy = nullptr;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
}

TEST(testKittyColorControlAcceptsDebianRed)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033]21;0=DeBiAnReD;0=?\033\\");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;0=rgb:d7/07/51\033\\");
}

TEST(testITermCellSizeReportsLogicalPoints)
{
    Ubiquitty::Terminal terminal;
    auto facts = std::make_shared<Ubiquitty::RichText::TypographyFacts>();
    facts->cellWidth = 8 * 64 + 1;
    facts->cellHeight = 17 * 64 + 32;
    terminal.typography.facts = facts;
    terminal.cellPixelWidth = 16;
    terminal.cellPixelHeight = 35;
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "ABCDEF\033]1337;ReportCellSize\007G");
    char reply[512];
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]1337;ReportCellSize=17.5;8.015625\007");
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'G');
    feed(terminal, "\033 G\033]1337;ReportCellSize\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\2351337;ReportCellSize=17.5;8.015625\234");
}

TEST(testITermVariableQueriesReturnSessionAndUserValues)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 10);
    feed(terminal, "\033]2;Title\007\033]1337;SetUserVar=branch=bWFpbg==\007");
    const auto encode = [](std::string_view value) {
        std::string result(Ubiquitty::Codec::base64EncodedLength(value.size()), '\0');
        const auto characters = Ubiquitty::Codec::base64Encode({ value.data(), value.size() }, result);
        CHECK(characters && characters->size() == result.size());
        return result;
    };
    const auto query = [&](std::string_view name, std::string_view value) {
        feed(terminal, "\033]1337;ReportVariable=" + encode(name) + "\033\\\033[5n");
        std::string actual;
        std::array<char, 17> bytes;
        for (int length; (length = Ubiquitty::drainOutput(&terminal, bytes)) > 0;)
            actual.append(bytes.data(), static_cast<std::size_t>(length));
        CHECK(actual == "\033]1337;ReportVariable=" + encode(value) + "\033\\\033[0n");
    };
    query("session.name", "Title");
    query("session.columns", "10");
    query("session.rows", "3");
    query("user.branch", "main");
    query("session.user.branch", "main");
    query("unknown", "");
    const std::string longValue(1024, 'x');
    feed(terminal, "\033]1337;SetUserVar=branch=" + encode(longValue) + "\007");
    query("user.branch", longValue);
    feed(terminal, "\033]1337;SetUserVar=branch=\007");
    query("user.branch", "");
    feed(terminal, "\033]1337;ReportVariable=%%%\007");
    char reply[512];
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testITermQueriesHonorReplyPermissions)
{
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &fakeCapabilityPolicy;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(terminal, "\033]1337;ReportCellSize\007\033]1337;ReportVariable=c2Vzc2lvbi5yb3dz\007");
    char reply[512];
    CHECK_EQ(drain(terminal, reply), 0);
    terminal.capabilityPolicy = nullptr;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
    terminal.allowWindowOperations = false;
    feed(terminal, "\033]1337;ReportCellSize\007\033]1337;ReportVariable=c2Vzc2lvbi5yb3dz\007\033]1337;ReportVariable=c2Vzc2lvbi5uYW1l\007");
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testLongPointerQueriesReplyToEveryName)
{
    for (const bool usesEightBitControls : { false, true }) {
        Ubiquitty::Terminal terminal;
        if (usesEightBitControls)
            feed(terminal, "\033 G");
        feed(terminal, "\033]22;vertical-text\033\\\033[5n");
        std::string query = "\033]22;?";
        std::string expected = usesEightBitControls ? "\2330n\23522;" : "\033[0n\033]22;";
        for (int index = 0; index < 256; ++index) {
            if (index) {
                query += ',';
                expected += ',';
            }
            query += "__current__,unknown,text";
            expected += "vertical-text,0,1";
        }
        query += "\033\\\033[5n";
        expected += usesEightBitControls ? "\234\2330n" : "\033\\\033[0n";
        feed(terminal, query);
        std::string actual;
        std::array<char, 13> bytes;
        for (int length; (length = Ubiquitty::drainOutput(&terminal, bytes)) > 0;)
            actual.append(bytes.data(), static_cast<std::size_t>(length));
        CHECK(actual == expected);
    }
}

TEST(testDirectionalPointerShapesRemainDistinct)
{
    using Shape = Ubiquitty::MousePointerShape;
    Ubiquitty::Terminal terminal;
    const std::array shapes {
        Shape::NorthResize,
        Shape::NortheastResize,
        Shape::EastResize,
        Shape::SoutheastResize,
        Shape::SouthResize,
        Shape::SouthwestResize,
        Shape::WestResize,
        Shape::NorthwestResize,
    };
    for (const auto shape : shapes) {
        const std::string name = Ubiquitty::OSC::pointerShapeWireName(shape);
        feed(terminal, "\033]22;=" + name + "\033\\\033]22;?__current__\033\\");
        CHECK(Ubiquitty::OSC::appRequestedPointerShape(terminal) == shape);
        char reply[512];
        drain(terminal, reply);
        CHECK(std::string_view(reply) == "\033]22;" + name + "\033\\");
    }
}

TEST(testSemanticFreshLineReturnsAndScrollsOnlyWhenNeeded)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "\033]133;L\007A\033]133;L\033\\B");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 'A');
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'B');
    CHECK_EQ(terminal.screens.active->cursorX, 1);
    feed(terminal, "CDEFG\033]133;L\007Z");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 'B');
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, 'G');
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'Z');
    CHECK_EQ(terminal.screens.active->cursorX, 1);
    CHECK(!terminal.screens.active->wrapPending);
}

TEST(testExplicitPromptsAndVSCodeMarksSetSemanticRoles)
{
    using Ubiquitty::SemanticRole;
    for (const auto marker : { "\033]133;P\007", "\033]133;P;k=i\007", "\033]133;P;k=r\007", "\033]133;P;k=c\007", "\033]133;P;k=s\007", "\033]633;A\007" }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, marker);
        feed(terminal, "P\033]633;B\007I\033[0mJ\033]633;C\007O\033]633;D;0\007D");
        CHECK(cell(terminal, 0, 0)->pen.semanticRole == SemanticRole::Prompt);
        CHECK(cell(terminal, 0, 1)->pen.semanticRole == SemanticRole::Input);
        CHECK(cell(terminal, 0, 2)->pen.semanticRole == SemanticRole::Input);
        CHECK(cell(terminal, 0, 3)->pen.semanticRole == SemanticRole::Output);
        CHECK(cell(terminal, 0, 4)->pen.semanticRole == SemanticRole::Output);
    }
    Ubiquitty::Terminal alternate;
    feed(alternate, "\033]633;A\007P\033[?1049h\033]633;B\007X\033[?1049lY");
    CHECK(cell(alternate, 0, 1)->pen.semanticRole == SemanticRole::Prompt);
}

TEST(testUnknownShellIntegrationCommandsDoNotMatchAPrefix)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "X\033]133;Long\007\033]133;Prompt\007\033]633;Anything\007Y");
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    CHECK_EQ(terminal.screens.active->cursorY, 0);
    CHECK(cell(terminal, 0, 1)->pen.semanticRole == Ubiquitty::SemanticRole::Output);
    CHECK(!terminal.hasMarkedPrompt);
}

TEST(testPointerStacksAreIndependentAcrossScreenSwitches)
{
    using Shape = Ubiquitty::MousePointerShape;
    struct PointerClient final : Ubiquitty::WindowClient {
        Shape shape = Shape::Default;
        void setMousePointerShape(Shape value) override { shape = value; }
    };
    for (const int mode : { 47, 1047, 1049 }) {
        PointerClient client;
        Ubiquitty::Terminal terminal;
        terminal.windowClient = &client;
        const std::string select = "\033[?" + std::to_string(mode);
        feed(terminal, "\033]22;>pointer,text\007" + select + "h");
        CHECK(!Ubiquitty::OSC::isAppPointerShapeSet(terminal));
        CHECK(client.shape == Shape::Default);
        feed(terminal, "\033]22;>wait,n-resize\007" + select + "l");
        CHECK(Ubiquitty::OSC::appRequestedPointerShape(terminal) == Shape::Text);
        CHECK(client.shape == Shape::Text);
        feed(terminal, "\033]22;<\007" + select + "h");
        CHECK(client.shape == Shape::NorthResize);
        feed(terminal, "\033]22;<\007");
        CHECK(client.shape == Shape::Wait);
        feed(terminal, select + "l");
        CHECK(client.shape == Shape::Pointer);
        feed(terminal, "\033c");
        CHECK_EQ(terminal.mousePointerShapeStackLength, 0);
        CHECK_EQ(terminal.alternateMousePointerShapeStackLength, 0);
        CHECK(client.shape == Shape::Default);
    }
}

TEST(testLineScopedSemanticRolesEndAtHardLineBreaks)
{
    using Ubiquitty::SemanticRole;
    for (const auto marker : { "\033]133;I\007", "\033]133;P;k=r\007" }) {
        for (const auto newline : { "\r\n", "\033E", "\v\r", "\f\r", "\033%@\x85" }) {
            for (const bool isFragmented : { false, true }) {
                Ubiquitty::Terminal terminal;
                Ubiquitty::resize(terminal, 3, 6);
                const std::string text = std::string(marker) + "A" + newline + "B";
                if (isFragmented) {
                    for (const char byte : text)
                        feed(terminal, std::string_view(&byte, 1));
                } else
                    feed(terminal, text);
                CHECK_EQ(cell(terminal, 0, 0)->codePoint, 'A');
                CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'B');
                CHECK(cell(terminal, 0, 0)->pen.semanticRole != SemanticRole::Output);
                CHECK(cell(terminal, 1, 0)->pen.semanticRole == SemanticRole::Output);
                CHECK(!terminal.shouldEndSemanticRoleAtLineBreak);
                CHECK(!terminal.isReadingCommand);
            }
        }
    }
}

TEST(testLineScopedInputSurvivesWrapsAndAlternateScreens)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 4);
    feed(terminal, "\033]133;I\007abcde\rF");
    CHECK(cell(terminal, 1, 0)->pen.semanticRole == SemanticRole::Input);
    CHECK(terminal.shouldEndSemanticRoleAtLineBreak);
    feed(terminal, "\033[?1049h\033]133;B\007\r\n\033[?1049lG");
    CHECK(cell(terminal, 1, 1)->pen.semanticRole == SemanticRole::Input);
    CHECK(terminal.shouldEndSemanticRoleAtLineBreak);
    feed(terminal, "\r\nH");
    CHECK(cell(terminal, 2, 0)->pen.semanticRole == SemanticRole::Output);
    feed(terminal, "\033]133;I\007I\033]133;B\007\r\nJ");
    CHECK(cell(terminal, 3, 0)->pen.semanticRole == SemanticRole::Input);
    CHECK(!terminal.shouldEndSemanticRoleAtLineBreak);
}

TEST(testSemanticNewCommandStartsOnAFreshLine)
{
    for (const std::string_view terminator : { "\a", "\033\\" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 3, 6);
        feed(t, std::string("X\033]133;N;aid=shell") + std::string(terminator) + "$\033]133;I\aI\033]133;C\aO");
        CHECK_EQ(t.screens.active->cursorY, 1);
        CHECK_EQ(t.screens.active->cursorX, 3);
        CHECK_EQ(t.grid->cellAt(Ubiquitty::Row(1), Ubiquitty::Column(0))->pen.semanticRole, Ubiquitty::SemanticRole::Prompt);
        CHECK_EQ(t.grid->cellAt(Ubiquitty::Row(1), Ubiquitty::Column(1))->pen.semanticRole, Ubiquitty::SemanticRole::Input);
        CHECK_EQ(t.grid->cellAt(Ubiquitty::Row(1), Ubiquitty::Column(2))->pen.semanticRole, Ubiquitty::SemanticRole::Output);
        feed(t, "\r\033]133;N;aid=shell\a");
        CHECK_EQ(t.screens.active->cursorY, 1);
        CHECK_EQ(t.commandCompletedGeneration, 1u);
        feed(t, "123456\033]133;N;aid=shell\a$");
        CHECK_EQ(t.screens.active->cursorY, 2);
        CHECK_EQ(t.screens.active->cursorX, 1);
        CHECK_EQ(t.commandCompletedGeneration, 2u);
    }
}

TEST(testSemanticNewCommandClosesTheInnermostMatchingApplication)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;A;aid=shell\a\033]133;C\a");
    feed(t, "\033]133;A;aid=repl\a\033]133;C\a");
    feed(t, "\033]133;A;aid=shell\a\033]133;C\a");
    feed(t, "\033]133;A;aid=child\a\033]133;C\a");
    CHECK_EQ(t.shellCommandCount, 4);
    feed(t, "\033]133;N;aid=shell;future=ignored\a");
    CHECK_EQ(t.commandCompletedGeneration, 2u);
    CHECK_EQ(t.commandExitCodeRing[0], -1);
    CHECK_EQ(t.commandExitCodeRing[1], -1);
    CHECK_EQ(t.shellCommandCount, 3);
    feed(t, "\033]133;N;aid=repl\a");
    CHECK_EQ(t.commandCompletedGeneration, 4u);
    CHECK_EQ(t.shellCommandCount, 2);
    feed(t, "\033]133;D;0\a\033]133;D;7\a");
    CHECK_EQ(t.shellCommandCount, 0);
    CHECK_EQ(t.commandCompletedGeneration, 6u);
    CHECK_EQ(t.commandExitCodeRing[4], 0);
    CHECK_EQ(t.commandExitCodeRing[5], 7);
}

TEST(testSemanticNewCommandWithoutApplicationMatchesOnlyEmptyIdentifiers)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]133;N;aid=shell\a\033]133;C\a\033]133;N\a");
    CHECK_EQ(t.commandCompletedGeneration, 0u);
    CHECK_EQ(t.shellCommandCount, 2);
    feed(t, "\033]133;C\a\033]133;N;aid=child\a\033]133;N;aid=\a");
    CHECK_EQ(t.commandCompletedGeneration, 2u);
    CHECK_EQ(t.shellCommandCount, 2);
    CHECK_EQ((*t.shellCommands)[0].applicationIDLength, 5);
    CHECK_EQ((*t.shellCommands)[1].applicationIDLength, 0);
}

TEST(testNestedShellCommandsKeepParentTimingAndPromptMarks)
{
    Ubiquitty::PAL::Context clockContext;
    clockContext.monotonicClock = &osc133FakeClock;
    Ubiquitty::PAL::ScopedContext scopedClock(clockContext);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 12);
    osc133FakeTicks = 100;
    feed(t, "\033]133;N;aid=shell\a$\033]133;C\a\r\n");
    osc133FakeTicks = 200;
    feed(t, "\033]133;N;aid=repl\a>\033]133;C\a\r\n");
    osc133FakeTicks = 300;
    feed(t, "\033]133;D;0\a");
    CHECK_EQ(t.commandDurationTicksRing[0], 100u);
    CHECK(t.hasCommandStartTick);
    CHECK_EQ(t.commandStartTick, 100u);
    osc133FakeTicks = 400;
    feed(t, "\033]133;D;1\a");
    CHECK_EQ(t.commandDurationTicksRing[1], 300u);
    CHECK(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdFailed);
    CHECK(!(Ubiquitty::Screen::rowMark(&t, 1) & Ubiquitty::Screen::osc133MarkCmdFailed));
    CHECK(!t.hasCommandStartTick);
}

TEST(testSemanticCommandStorageIsBoundedAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (size_t index = 0; index < 2 * Ubiquitty::Terminal::shellCommandCapacity; ++index)
        feed(t, "\033]133;A;aid=" + std::to_string(index) + "\a\033]133;C\a");
    CHECK_EQ(t.shellCommandCount, Ubiquitty::Terminal::shellCommandCapacity);
    const auto generation = t.commandCompletedGeneration;
    feed(t, "\033]133;N;aid=" + std::string(257, 'x') + "\a");
    CHECK_EQ(t.commandCompletedGeneration, generation);
    CHECK_EQ(t.shellCommandCount, Ubiquitty::Terminal::shellCommandCapacity);
    feed(t, "\033[?1049h\033]133;N;aid=31\a");
    CHECK_EQ(t.commandCompletedGeneration, generation);
    CHECK_EQ(t.shellCommandCount, Ubiquitty::Terminal::shellCommandCapacity);
    feed(t, "\033[?1049l\033c");
    CHECK_EQ(t.shellCommandCount, 0);
}

TEST(testShellCommandFailureAfterResizeUsesItsMovedPrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 12);
    feed(t, "\033]133;A\a$\033]133;C\aABCDEFGHIJKLM\r\n");
    Ubiquitty::resize(t, 8, 6);
    const int absoluteRow = Ubiquitty::Screen::scrollbackCount(&t) + t.screens.active->cursorY;
    const int promptRow = Ubiquitty::Screen::findPromptMarkBefore(&t, absoluteRow + 1, Ubiquitty::Screen::osc133MarkCmdExecuted);
    REQUIRE(promptRow >= 0);
    feed(t, "\033]133;D;1\a");
    CHECK(Ubiquitty::Screen::rowMark(&t, promptRow) & Ubiquitty::Screen::osc133MarkCmdFailed);
}

TEST(testCancelledInputDoesNotMarkAnEarlierCommandAsFailed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 12);
    feed(t, "\033]133;A\a$\033]133;C\a\033]133;D;0\a\r\n");
    feed(t, "\033]133;A\a$\033]133;B\a\033]133;D;1\a");
    CHECK(!(Ubiquitty::Screen::rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkCmdFailed));
    CHECK(!(Ubiquitty::Screen::rowMark(&t, 1) & Ubiquitty::Screen::osc133MarkCmdExecuted));
}

TEST(testSemanticPromptStartBeginsWithAFreshLine)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "ABCDEF\033]133;A\007");
    CHECK_EQ(terminal.screens.active->cursorY, 1);
    CHECK_EQ(terminal.screens.active->cursorX, 0);
    CHECK(!terminal.screens.active->wrapPending);
    feed(terminal, "\033]133;A\033\\$");
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, '$');
    CHECK(cell(terminal, 1, 0)->pen.semanticRole == Ubiquitty::SemanticRole::Prompt);
    feed(terminal, "\033]133;A\007P");
    CHECK_EQ(cell(terminal, 2, 0)->codePoint, 'P');
    CHECK_EQ(terminal.screens.active->cursorY, 2);
}

TEST(testURxvtVersionQueryNamesTheProtocolAndThisTerminal)
{
    Ubiquitty::Terminal terminal;
    char reply[256];
    feed(terminal, "\033]702;?\007\033]702;?\033\\");
    drain(terminal, reply);
    // The major and minor numbers of the version this binary was built as.
    const std::string_view version = Ubiquitty::buildVersion();
    const std::size_t majorEnd = version.find('.');
    REQUIRE(majorEnd != std::string_view::npos);
    const std::size_t minorEnd = version.find('.', majorEnd + 1);
    REQUIRE(minorEnd != std::string_view::npos);
    const std::string one = "\033]702;rxvt-unicode;ubiquitty;" + std::string(version.substr(0, majorEnd)) + ";"
        + std::string(version.substr(majorEnd + 1, minorEnd - majorEnd - 1)) + "\033\\";
    CHECK_STR_EQ(std::string(reply), one + one);
    feed(terminal, "\033]702;rxvt\007\033]702\007");
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testURxvtCellSizeReportsThePublishedPixels)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 6);
    char reply[256];
    feed(terminal, "ABCDEF\033]776;?\007G");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]776;8;16;14\007");
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'G');
    auto facts = std::make_shared<Ubiquitty::RichText::TypographyFacts>();
    facts->cellHeight = 17 * 64 + 32;
    facts->baseline = 14 * 64;
    terminal.typography.facts = facts;
    terminal.cellPixelWidth = 16;
    terminal.cellPixelHeight = 35;
    feed(terminal, "\033]776;?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]776;16;35;28\033\\");
    feed(terminal, "\033]776;x\007");
    CHECK_EQ(drain(terminal, reply), 0);
    terminal.allowWindowOperations = false;
    feed(terminal, "\033]776;?\007");
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testMinttyFontSizeQueriesReportTheDrawnSize)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    FontClient client;
    terminal.windowClient = &client;
    char reply[256];
    feed(terminal, "\033]7770;?\007\033]7777;?\007");
    CHECK_EQ(drain(terminal, reply), 0);
    REQUIRE(terminal.fontRequests.publishActualFont("Menlo", 15.5));
    feed(terminal, "\033]7770;?\033\\\033]7777;?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]7770;15.5\033\\\033]7777;15.5\007");
    REQUIRE(terminal.fontRequests.publishActualFont("Menlo", 16));
    feed(terminal, "\033]7777;?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]7777;16\007");
    // The size-only control reaches the same font change, so the reply its
    // query sent restores the size it reported.
    feed(terminal, "\033]7770;+2\007");
    CHECK_EQ(client.calls, 1);
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == 2.0);
    feed(terminal, "\033]7770;15.5\033\\");
    CHECK_EQ(client.calls, 2);
    CHECK(client.family.empty());
    CHECK(client.sizePoints == 15.5);
    CHECK(client.sizeDelta == 0.0);
    feed(terminal, "\033]7770;-1\007\033]7770;\007");
    CHECK_EQ(client.calls, 3);
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == -1.0);
    CHECK_EQ(client.sizeResets, 1);
    feed(terminal, "\033]7770;1e3\007\033]7770;0\007\033]7770;x\007");
    CHECK_EQ(client.calls, 3);
    CHECK_EQ(client.sizeResets, 1);
    terminal.allowFontOperations = false;
    feed(terminal, "\033]7770;?\007\033]7777;?\007\033]7770;\007");
    CHECK_EQ(drain(terminal, reply), 0);
    CHECK_EQ(client.sizeResets, 1);
    terminal.windowClient = nullptr;
}

TEST(testMinttyFontAndWindowSizeChangesKeepTheGeometry)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    FontClient client;
    terminal.windowClient = &client;
    feed(terminal, "\033]7777;14\007");
    CHECK_EQ(client.calls, 1);
    CHECK(client.family.empty());
    CHECK(client.sizePoints == 14.0);
    CHECK(client.sizeDelta == 0.0);
    feed(terminal, "\033]7777;+2\033\\");
    CHECK_EQ(client.calls, 2);
    CHECK(client.sizePoints == 0.0);
    CHECK(client.sizeDelta == 2.0);
    feed(terminal, "\033]7777;-1.5\007");
    CHECK_EQ(client.calls, 3);
    CHECK(client.sizeDelta == -1.5);
    feed(terminal, "\033]7777;\007");
    CHECK_EQ(client.calls, 3);
    CHECK_EQ(client.sizeResets, 1);
    for (const char* malformed : { "\033]7777;abc\007", "\033]7777;1e3\007", "\033]7777;0\007", "\033]7777;+0\007", "\033]7777;--1\007", "\033]7777;1.2.3\007", "\033]7777;inf\007" })
        feed(terminal, malformed);
    CHECK_EQ(client.calls, 3);
    CHECK_EQ(client.sizeResets, 1);
    terminal.allowFontOperations = false;
    feed(terminal, "\033]7777;12\007\033]7777;\007");
    CHECK_EQ(client.calls, 3);
    CHECK_EQ(client.sizeResets, 1);
    terminal.windowClient = nullptr;
}

TEST(testMinttyFontSizeResetKeepsTheFamilyAProgramSet)
{
    // The empty operand restores the default size alone. A full font reset
    // would also drop the family OSC 50 set, which mintty's "default" for the
    // size does not ask for.
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    FontClient client;
    terminal.windowClient = &client;
    char reply[256];
    feed(terminal, "\033]50;Monaco\007\033]7770;\007\033]7777;\033\\");
    CHECK_EQ(client.calls, 1);
    CHECK(client.family == "Monaco");
    CHECK_EQ(client.sizeResets, 2);
    feed(terminal, "\033]50;?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]50;Monaco\007");
    terminal.windowClient = nullptr;
}

TEST(testMinttyFontSizesKeepTheirDecimalPointUnderACommaLocale)
{
    // A frontend's toolkit can adopt the user's locale, which moves the C
    // library's decimal point; a control's operand and reply are never
    // localized.
    const char* previousLocale = std::setlocale(LC_NUMERIC, nullptr);
    const std::string savedLocale = previousLocale ? previousLocale : "C";
    bool hasCommaLocale = false;
    for (const char* name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8" }) {
        if (std::setlocale(LC_NUMERIC, name) && std::string_view(std::localeconv()->decimal_point) == ",") {
            hasCommaLocale = true;
            break;
        }
    }
    if (!hasCommaLocale) {
        std::setlocale(LC_NUMERIC, savedLocale.c_str());
        UbiquittyTest::skip("this system has no locale whose decimal point is a comma");
        return;
    }
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    FontClient client;
    terminal.windowClient = &client;
    char reply[256];
    const bool isPublished = terminal.fontRequests.publishActualFont("Menlo", 15.5);
    feed(terminal, "\033]7777;?\007\033]7777;12.5\007\033]50;#13.5\007");
    const int length = drain(terminal, reply);
    const double requestedPoints = client.sizePoints;
    const int calls = client.calls;
    terminal.windowClient = nullptr;
    std::setlocale(LC_NUMERIC, savedLocale.c_str());
    REQUIRE(isPublished);
    CHECK(std::string_view(reply, static_cast<std::size_t>(length)) == "\033]7777;15.5\007");
    CHECK_EQ(calls, 2);
    CHECK(requestedPoints == 13.5);
}

TEST(testITermSessionNameFallsBackToTheApplicationName)
{
    Ubiquitty::Terminal terminal;
    const auto query = [&](std::string_view encodedName) {
        feed(terminal, "\033]1337;ReportVariable=" + std::string(encodedName) + "\033\\");
        std::string actual;
        std::array<char, 64> bytes;
        for (int length; (length = Ubiquitty::drainOutput(&terminal, bytes)) > 0;)
            actual.append(bytes.data(), static_cast<std::size_t>(length));
        return actual;
    };
    // "session.name" and "session.terminalWindowName".
    CHECK(query("c2Vzc2lvbi5uYW1l") == "\033]1337;ReportVariable=VWJpcXVpdHR5\033\\");
    CHECK(query("c2Vzc2lvbi50ZXJtaW5hbFdpbmRvd05hbWU=") == "\033]1337;ReportVariable=\033\\");
    feed(terminal, "\033]2;Build\007");
    CHECK(query("c2Vzc2lvbi5uYW1l") == "\033]1337;ReportVariable=QnVpbGQ=\033\\");
}

TEST(testKittyColorControlSetsAndReportsTheTextUnderTheCursor)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    char reply[256];
    // Unset, the glyph takes the renderer's contrast, which has no defined value.
    feed(terminal, "\033]21;cursor_text=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;cursor_text=\033\\");
    // "To set a color value, the client program sends this escape code with the
    // value field set to either an encoded color value or the empty value."
    const auto cursor = terminal.palette.cursor;
    const auto foreground = terminal.palette.defaultForeground;
    feed(terminal, "\033]21;selection_foreground=#112233\033\\\033]21;cursor_text=#445566\033\\");
    feed(terminal, "\033]21;selection_foreground=?\033\\\033]21;cursor_text=?;cursor_text=?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;selection_foreground=rgb:11/22/33\033\\\033]21;cursor_text=rgb:44/55/66;cursor_text=rgb:44/55/66\007");
    CHECK(terminal.hasCursorTextColor);
    CHECK(terminal.palette.cursor == cursor);
    CHECK(terminal.palette.defaultForeground == foreground);
    // RIS keeps it, as it keeps the other dynamic colours.
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033]21;cursor_text=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;cursor_text=rgb:44/55/66\033\\");
    // The empty value asks for the dynamic color, and the bare key for the
    // default, which is the same unset value.
    feed(terminal, "\033]21;cursor_text=;cursor_text=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;cursor_text=\033\\");
    feed(terminal, "\033]21;cursor_text=red\033\\\033]21;cursor_text;cursor_text=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;cursor_text=\033\\");
    CHECK(!terminal.hasCursorTextColor);
    // A value that names no color leaves it alone.
    feed(terminal, "\033]21;cursor_text=#445566\033\\\033]21;cursor_text=nonsense;cursor_text=?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]21;cursor_text=rgb:44/55/66\033\\");
    terminal.allowColorOperations = false;
    feed(terminal, "\033]21;cursor_text=?\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
}

TEST(testApplicationIdentifierQueryAnswersTheCurrentIdentifier)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 6);
    char reply[512];
    // Unset, the query is answered with the empty appID.
    feed(terminal, "\033]176;?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]176;\033\\");
    // A set is consumed, the cursor and its pending wrap untouched, and the
    // query answers "OSC 176 ; appID ST", ended with ST whatever ended it.
    feed(terminal, "ABCDEF\033]176;org.example.mail\007G\033]176;?\007");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]176;org.example.mail\033\\");
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 'G');
    CHECK_EQ(terminal.screens.active->cursorY, 1);
    CHECK_EQ(terminal.screens.active->cursorX, 1);
    // "If empty when setting, the app unsets its app ID."
    feed(terminal, "\033]176;\033\\\033]176;?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]176;\033\\");
    // An ID the 255 bytes kept for it cannot hold is not taken.
    feed(terminal, "\033]176;vlc\033\\");
    feed(terminal, "\033]176;" + std::string(256, 'a') + "\033\\\033]176;?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]176;vlc\033\\");
    feed(terminal, "\033]176;" + std::string(255, 'b') + "\033\\\033]176;?\033\\");
    std::string longest;
    std::array<char, 64> piece;
    for (int length; (length = Ubiquitty::drainOutput(&terminal, piece)) > 0;)
        longest.append(piece.data(), static_cast<std::size_t>(length));
    CHECK(longest == "\033]176;" + std::string(255, 'b') + "\033\\");
    // A control byte the program set never reaches the reply.
    feed(terminal, "\033]176;a\tb\033\\\033]176;?\033\\");
    drain(terminal, reply);
    CHECK(std::string_view(reply) == "\033]176;ab\033\\");
    // The reply is a title read, under the window operations and the policy
    // that gate one.
    terminal.allowWindowOperations = false;
    feed(terminal, "\033]176;?\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    terminal.allowWindowOperations = true;
    terminal.capabilityPolicy = &fakeCapabilityPolicy;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(terminal, "\033]176;?\033\\");
    CHECK_EQ(drain(terminal, reply), 0);
    terminal.capabilityPolicy = nullptr;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
}
