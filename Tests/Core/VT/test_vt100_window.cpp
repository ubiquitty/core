// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The terminal as a window, and the frontend calls that reach out of it.
//
// XTWINOPS in full — the resize, move, raise, iconify and report operations,
// plus the title stack — alongside focus reporting, the bell, the status
// display, printing via media copy, and the DECPS tone generator.
//
// What unites them is that Core cannot carry any of them out. Each is a request
// a frontend may honour, ignore or refuse, so the cases assert what Core asks
// for and what it does when nothing is listening, which is the state a headless
// terminal is always in.

#include "Core/Keys/Keymap.h"
#include "Core/Rendering/StatusRow.h"
#include "Harness.h"
#include "VT100Fixture.h"

/// 96-set designation + GR invocation (ECMA-35 / ISO 2022). ESC - A
/// designates ISO 8859-1 (Latin-1 right half) into G1; LS1R (ESC ~)
/// invokes G1 into GR; bytes 0xA0..0xFF then resolve through Latin-1
/// (identity: byte == U+00A0..U+00FF).
/// DEC Supplemental Graphic Set as a 96-set into G2, invoked into GR via
/// LS2R (ESC }). Probes the positions where DEC-MCS diverges from Latin-1:
/// 0xD7 → Œ, 0xDD → Ÿ, 0xF7 → œ, 0xFD → ÿ, 0xA8 → ¤. The Latin-1-identical
/// positions (À, é) must still map straight through.
/// LS3R (ESC |) invokes G3 into GR. ESC / A designates Latin-1 into G3.
/// LS2/LS3 (ESC n / ESC o) lock G2/G3 into GL; an ASCII byte then resolves
/// through the GL-invoked set. Designate DEC graphics into G2, LS2, then 'l'
/// draws the box corner. ESC n then ESC o back to G3 (left ASCII).
/// Single-shift onto a GR byte: SS2 (ESC N) selects G2 for exactly the next
/// character even when that character arrives in the GR range (0xA0..0xFF).
/// Designate Latin-1 into G2, then SS2 + a GR byte resolves via G2; the
/// following GR byte (no shift, GR still default) is not Latin-1-translated.
/// 94-character sets invoked into GR use positions 10/01..15/14 only; the
/// 10/00 and 15/15 endpoints stay outside the set and must not fold onto SPACE
/// or DELETE. This holds for both hard sets and DRCS soft sets.
/// SS3 (ESC O) single-shift onto a GL byte works for a 94-set:
/// designate DEC graphics into G3, SS3, then 'l' → ┌ for one char only.
/// DEC Supplemental as a 94-set via the xterm ESC ( < form, invoked in GL.
/// A GL byte (0x20..0x7F) folds up into the 0xA0..0xFF home before lookup:
/// 0x57 ('W') + 0x80 = 0xD7 → Œ. Confirms the 94-vs-96 designation registry
/// routes ESC ( < to the same DEC-MCS table.
/// GR invocation is inert by default: with no LS*R / single-shift, a GR byte
/// stays on the UTF-8 path. A lone 0xA9 (a UTF-8 continuation byte with no
/// lead) is immediately invalid UTF-8 → U+FFFD under the default policy —
/// not Latin-1 '©'. This pins that the GR path does not steal bytes from
/// the common stream when no GR set is invoked.
/// DECSC/DECRC round-trips the GR invocation: invoke G1→GR, save, clear GR
/// via RIS-like reinvocation... actually LS0R has no escape; instead clobber
/// GR with LS2R, then DECRC must restore GR=1 (G1).
/// DECSC/DECRC preserve the pending-wrap latch (G6): printing at the last
/// column arms a deferred wrap; ESC 7 / ESC 8 round-trip it so the next glyph
/// wraps instead of overwriting the last cell.
/// S8C1T makes we emit 8-bit C1 controls in replies; S7C1T keeps the 7-bit
/// ESC-prefixed form. Drive a Primary DA query (CSI c) and inspect the reply
/// introducer: S8C1T → single byte 0x9B; S7C1T → ESC '['.
// G5: a truncated semicolon ext-color introducer consumes its whole
// span and applies no color — leftover params never leak back into the SGR
// loop as standalone codes (38;5 must not enable blink; 38;2;1;2 must not set
// bold+dim). The rule is selector-based, so 48/bg is covered alongside 38/fg.
/// Exhaustive sweep of the basic ANSI foreground range 30-37. The
/// implementation maps these to indexed colors 0-7.
/// Exhaustive sweep of the basic ANSI background range 40-47.
/// Bright foreground sweep 90-97 maps to indices 8-15.
/// Bright background sweep 100-107 maps to indices 8-15.
/// \033[39m / \033[49m reset foreground / background to the renderer default
/// (Ubiquitty::colorDefault), without disturbing the other channel.
/// A single CSI must apply each semicolon-separated selector in
/// order: \033[1;31;42m -> bold + red foreground + green background.
/// SGR attribute sweep: every attribute set (1..9, with 5/6 aliased to
/// BLINK) lands the right ATTR_* bit in the pen, and the matching
/// reset (21..29, with 21/22 both clearing BOLD|DIM) clears it. Fills
/// the applySGRAttribute switch arms that were unreachable from the
/// pre-existing foreground/background colour tests.
/// Extended underline styles (SGR 4 / 4:0..4:5 / 24). The bare 4 is
/// single, 24 is none; the colon-grouped form selects the shape. The
/// style lives in Pen::underlineStyle and Attribute::Underline tracks
/// non-zero styles so bit-only paint paths keep working.
/// Underline color (SGR 58 / 59), parsed with the same 5:n / 2:r:g:b
/// grammar as 38/48 and stored in Pen::underlineIndex + underlineRed/underlineGreen/underlineBlue. The reset
/// (59 and SGR 0) returns underlineIndex to Ubiquitty::colorDefault = follow text color.
/// The power-on / reset pen leaves the underline channel at Ubiquitty::colorDefault
/// ("follow text color"), never palette entry 0. Regression test for the
/// penReset copy in the screen-reset path that memset the pen and forgot
/// the underline channel, so text typed right after init or DECSTR
/// (before any SGR) carried a palette-0 underline color.
/// Overline (53/55), framed (51), encircled (52), not-framed-or-
/// encircled (54). Each lands its ATTR_* bit; 54 clears both frame
/// bits at once; SGR 0 clears all of them.
/// CSI m with no parameters resets the pen — equivalent to CSI 0 m
/// but takes a different early-exit branch (parameterCount == 0).
/// Out-of-range U8 clamp inside parseExtendedColor: r/g/b components
/// below 0 or above 255 land at the clamp endpoints rather than
/// wrapping. Drives the n<0 / n>255 arms of clampU8Parameter.
/// Colon-form extended palette with a truncated tail (`38:2:r:g` —
/// blue parameter missing) must leave the pen colour untouched: the
/// `k >= 5` guard rejects it. Drives the else-branch on that guard.
/// A colon run whose mode is unrecognized (38:3:c:m:y — ITU T.416 CMY)
/// or truncated before its data (38:5 with no index) is skipped
/// atomically. A run left unconsumed leaks its sub-parameters into the
/// top-level SGR loop as standalone codes, where the 0 in 38:3:0:... performs a
/// full pen reset and a leaked 5 turns on blink — so an unrecognized colour
/// silently rewrites the pen.
/// Title decoder preserves the raw UTF-8 source in Terminal::titleUTF8 so
/// a Unicode-capable frontend renders the original Unicode. The MacRoman-folded
/// Terminal::title drops graphic-ID-mapped lead glyphs to match a 1-byte title API's
/// capabilities; titleUTF8 keeps them.
/// Malformed UTF-8 in OSC title: a 3-byte lead (0xE0..0xEF) followed
/// by a non-continuation byte must reconstruct the lead as 0xE0|low —
/// not 0xC0|low (a 2-byte-only fallback corrupts Latin-1 /
/// MacRoman titles emitted by mis-encoded shells).
/// 4-byte lead (0xF0..0xF4) followed by a non-continuation byte:
/// reconstruct as 0xF0|low.
/// XTWINOPS: CSI 21 t replies OSC l <title> ST.
/// A title longer than 160 bytes must still report in FULL — the report builds
/// a std::string, with no fixed buffer to clip it or to suppress the reply.
/// Alternate-screen feature gate (DEC 1046): default enabled, DECRQM
/// round-trip, gating of modes 47/1047/1049 when reset, and the
/// forced alternate→primary switch when reset while on the alternate screen.
/// DECRST 1047 clears the alternate buffer on exit (G9): re-entering the
/// alternate screen shows a clean buffer, not the prior session's content. 47 never
/// clears, on entry or exit.
/// Alternate scroll mode (DEC 1007): set/reset flag, DECRQM report,
/// gating on alternate-screen, and the DECCKM-aware wheel→key byte formatting.
/// Meta/Alternate keyboard discipline (xterm DEC private 1034 / 1036 / 1039):
/// power-on defaults, DECSET/DECRST flag flips, and DECRQM round-trip.
/// deleteSendsDel (DEC private 1037): power-on default reset, DECSET/DECRST
/// flag flips, and DECRQM round-trip.
/// Font-shifting (DEC private 35, rxvt): power-on default reset, DECSET/DECRST
/// flag flips, and DECRQM round-trip. Stored only — Ubiquitty renders from one
/// font, so there is no glyph effect.
/// keepClipboard / reuse-selection-as-clipboard (DEC private 1044): power-on
/// default reset, DECSET/DECRST flag flips, and DECRQM round-trip. The frontend
/// reads t.modes.keepClipboard to copy the selection without an explicit copy.
/// At&T 610 cursor blink (DEC private 12): DECRQM round-trip plus the resolved
/// blink behavior. Mode 12 gates the default-shape cursor's blink only when the
/// user has not explicitly set a blink preference; an explicit pref or a
/// DECSCUSR style overrides it.
/// termcap/terminfo function-key flavor modes (DEC private 1050-1061): each is
/// a stored flag that round-trips through DECSET/DECRST + DECRQM (the alternate
/// F-key encoding tables are not implemented — default VT220/xterm stays).
/// DECUDK (DCS Pc ; Pl | Ky/St … ST): program a key, clear-all (Pc=0),
/// per-key clear (Pc=1), and the lock (Pl=0).
/// Push, change, pop — original title returns.
// 9 pushes against an 8-deep stack: stack stays capped, oldest drops.
/// XTWINOPS op 8 — host-requested resize. The core model resizes;
/// the PAL hook (when installed) physically resizes the Mac window.
/// Test exercises the model side; PAL hook isn't installed in host
/// tests so the resize is silent (which proves the core-only path
/// works without a PAL).
/// XTWINOPS mutators 1..7, 9, 10 route through WindowClient::
/// performWindowOperation with (op, a, b) carrying the raw Ps
/// sub-parameters.
namespace {
struct WinOpClient : Ubiquitty::WindowClient {
    int calls = 0, op = 0, a = 0, b = 0;
    void performWindowOperation(int o, int x, int y) override
    {
        calls++;
        op = o;
        a = x;
        b = y;
    }
};
}

static Ubiquitty::PAL::Context palContextWithPlayTones(Ubiquitty::PAL::PlayTonesCallback synth)
{
    Ubiquitty::PAL::Context c;
    c.monotonicClock = &osc133FakeClock;
    c.playTones = synth;
    return c;
}

/// DECSERA erases the whole (unprotected) rect like DECERA when nothing is
/// protected, and respects VT400 conformance gating.
/// rxvt show/hide scrollbar (?30): default shown, DECSET/DECRST flag flips +
/// DECRQM round-trip.
/// rxvt show/hide toolbar (?10): default shown, flag flip + DECRQM.
/// more(1) fix (?41): default off, flag flip + DECRQM round-trip. The wrap
/// model is already more(1)-safe; this verifies the mode bit round-trips.
/// A last-column char followed by LF must advance exactly one row (the more(1)
/// bug would double it). The deferred-wrap model holds this unconditionally.
/// DECPFF print form feed (?18) + DECPEX print extent (?19): defaults + DECRQM.
/// Start/stop logging (?46): security-gated. Without an installed log path,
/// ?46 h is a silent no-op (cannot open an arbitrary file); with a path
/// installed it round-trips and toggles capture.
/// DECMC media copy — print screen (CSI 0 i) routes a UTF-8 dump to the sink.
/// DECMC print-line (CSI ? 1 i) dumps the cursor's row only.
/// DECMC auto-print (CSI ? 5 i on / ? 4 i off): each completed line is copied
/// to the sink as the cursor leaves it.
/// DECMC with no sink installed is inert (security): print-screen does nothing
/// and cannot crash; printer-controller state still round-trips.
/// DECSLPP page memory (CSI Ps t, Ps >= 24) + NP/PP (CSI U / V) navigation.
/// ---- Batch 2: modifyCursorKeys / FunctionKeys / KeypadKeys -------
///
/// The modifyXxxKeys family is wired through `CSI > Pn ; Pv m` (set),
/// `CSI ? Pn m` (query), and `CSI > Pn n` (disable). Pn picks the resource,
/// and only the ones the key encoder reads are accepted:
///   1 = modifyCursorKeys
///   2 = modifyFunctionKeys
///   3 = modifyKeypadKeys
///   4 = modifyOtherKeys
///
/// The storage lives on Terminal::xtermKeyModifiers[] and
/// Terminal::xtermKeyFormats[]; Tests/Core/Runtime/test_key_encode.cpp reads
/// each accepted resource back through the key it governs.

/// Format setter: CSI > 4 ; Pv f stores into xtermKeyFormats[4], the one
/// format the encoder reads.
/// Out-of-range category (>= xtermKeyModifierCategoryCount) silently
/// drops — no buffer write, no crash.
/// VT52 ESC Z (identify) — host receives "ESC / Z".
// ---- DECPS play-sound (CSI Pvol ; Pdur ; Pnote ... , ~) ----------
//
// Records the last PAL::playTones invocation so the test can assert the
// parser extracted volume / duration / notes and the sub-parameters, and
// called the hook with the right request.
namespace {
struct DECPSCapture {
    int calls;
    Ubiquitty::PAL::PlayTonesRequest request;
};
DECPSCapture decPSCapture;
void decpsRecord(const Ubiquitty::PAL::PlayTonesRequest& request)
{
    decPSCapture.calls++;
    decPSCapture.request = request;
}
} // namespace

TEST(testMarginBellOncePerPass)
{
    Ubiquitty::Terminal t;
    int column;
    Ubiquitty::hardReset(t);
    // Enable margin bell.
    feed(t, "\033[?44h");
    CHECK(t.marginBell);
    // Walk the cursor to one cell below the threshold column.
    t.marginBellColumn = 4;
    t.marginBellArmed = true;
    // Position cursor at column 0 then emit chars up to and past the
    // threshold column. The bell must fire exactly once.
    feed(t, "\033[H");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(!t.bellPending);
    // Write 4 chars: cursor lands at column 4 (threshold). Fires once.
    feed(t, "abcd");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.bellPending);
    t.bellPending = false;
    // Walk back with BS then forward over the threshold again.
    // Each BS drops cursorX below the threshold, re-arming. Then the
    // next char that lands on column 4 fires once.
    feed(t, "\b\b\b\b");
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "wxyz");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.bellPending);
    t.bellPending = false;
    // Now write 3 more chars past the threshold without dropping
    // below it. No further bells should fire.
    feed(t, "12");
    CHECK(!t.bellPending);
    // And overwriting the threshold cell via BS + char without
    // re-arming (cursor stays at column 5 after backing up one) must
    // not fire.
    feed(t, "\b"); // x = 5
    feed(t, "Q"); // lands at x=5 again — not on threshold
    CHECK(!t.bellPending);
    (void)column;
}

TEST(testResolveBellEventModeAndCoalesce)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // No pending bell → Off regardless of mode.
    t.bellMode = Ubiquitty::BellMode::Both;
    CHECK(Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(100)) == Ubiquitty::BellMode::Off);

    // BEL sets pending; resolve returns the configured mode and clears
    // the flag. tick=100 records the coalescer baseline.
    feed(t, "\a");
    CHECK(t.bellPending);
    CHECK(Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(100)) == Ubiquitty::BellMode::Both);
    CHECK(!t.bellPending);

    // A second BEL within the coalesce window (6 ticks) is suppressed.
    feed(t, "\a");
    CHECK(Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(103)) == Ubiquitty::BellMode::Off);

    // Past the window it fires again (mode is Both per the setup).
    feed(t, "\a");
    CHECK(Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(110)) == Ubiquitty::BellMode::Both);

    /// Off mode suppresses even a fresh pending bell.
    Ubiquitty::hardReset(t);
    t.bellMode = Ubiquitty::BellMode::Off;
    feed(t, "\a");
    CHECK(Ubiquitty::resolveBellEvent(&t, Ubiquitty::Ticks(500)) == Ubiquitty::BellMode::Off);
    // The pending flag is still cleared so it doesn't leak to a later
    // mode change.
    CHECK(!t.bellPending);
}

TEST(testBellFlashAlphaEnvelope)
{
    // Zero start tick = no flash.
    CHECK(Ubiquitty::bellFlashAlpha(0, 1000) == 0.0f);
    // At onset, alpha is the peak; it decays to 0 by bellFlashTicks.
    float atStart = Ubiquitty::bellFlashAlpha(100, 100);
    float midway = Ubiquitty::bellFlashAlpha(100, 100 + Ubiquitty::bellFlashTicks / 2);
    CHECK(atStart > 0.0f);
    CHECK(midway > 0.0f);
    CHECK(midway < atStart);
    CHECK(Ubiquitty::bellFlashAlpha(100, 100 + Ubiquitty::bellFlashTicks) == 0.0f);
    CHECK(Ubiquitty::bellFlashAlpha(100, 100 + Ubiquitty::bellFlashTicks + 50) == 0.0f);
}

TEST(testDECSASDWithDECSCNM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    // DECSCNM on.
    feed(t, "\033[?5h");
    CHECK(t.modes.reverseVideo);
    // DECSASD to status (CSI 1 $ }) — moves writes to status row.
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    feed(t, "OK");
    /// The data screen should not have 'O' at (0,0); the status row should.
    const Ubiquitty::Cell* dataCell = cell(t, 0, 0);
    CHECK(dataCell == nullptr || dataCell->codePoint != 'O');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'O');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'K');
}

// The status-row writer clamps a wedged negative cursor column to 0 instead of
// indexing the statusRow array out of bounds (a defensive floor; no control
// path drives it negative).
TEST(testStatusRowWriteClampsNegativeCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    feed(t, "\033[1$}"); // DECSASD: status display active
    t.statusCursorX = -1; // wedged cursor column
    feed(t, "x");
    CHECK(t.statusCursorX == 1);
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'x');
}

TEST(testDECSASDControlsRouteToStatusDisplay)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    feed(t, "data"); // something on the data screen to prove untouched
    feed(t, "\033[2$~\033[1$}"); // DECSASD: status display active

    // CR: AB, CR, C overwrites the first cell.
    feed(t, "AB\rC");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'C');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'B');
    CHECK(t.statusCursorX == 1);

    // BS steps back; the next glyph overwrites.
    feed(t, "X\010Y"); // X at 1, BS to 1, Y at 1
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'Y');

    // HT advances to the next default tab stop (column 8).
    feed(t, "\r\011");
    CHECK(t.statusCursorX == 8);

    // CHA / HPA / CUF / CUB move the STATUS cursor (1-based columns).
    feed(t, "\033[5G");
    CHECK(t.statusCursorX == 4);
    feed(t, "\033[3`");
    CHECK(t.statusCursorX == 2);
    feed(t, "\033[4C");
    CHECK(t.statusCursorX == 6);
    feed(t, "\033[2D");
    CHECK(t.statusCursorX == 4);
    // CUP: one row — only the column component applies.
    feed(t, "\033[10;3H");
    CHECK(t.statusCursorX == 2);

    // SGR applies to subsequent status writes (shared pen).
    feed(t, "\033[1m");
    feed(t, "\rZ");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    feed(t, "\033[0m");

    // ICH shifts right and blanks the gap; DCH pulls left; ECH blanks in
    // place; EL clears cursor-to-end.
    feed(t, "\r");
    feed(t, "abcd\r"); // status: a b c d, cursor at 0
    feed(t, "\033[2@"); // ICH 2: _ _ a b c d...
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 0 || Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == ' ');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 2).codePoint == 'a');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 3).codePoint == 'b');
    feed(t, "\033[2P"); // DCH 2: back to a b c d
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'a');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'b');
    feed(t, "\033[2X"); // ECH 2: blanks a b in place, cursor stays
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 0 || Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == ' ');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 2).codePoint == 'c');
    feed(t, "\033[K"); // EL 0: cursor to end
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 2).codePoint == 0 || Ubiquitty::Rendering::statusCellAt(t, 2).codePoint == ' ');

    // The data screen never moved or changed: "data" intact, cursor where
    // the data-screen writes left it.
    CHECK(cell(t, 0, 0)->codePoint == 'd');
    CHECK(cell(t, 0, 3)->codePoint == 'a');
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 0);

    // DECSASD 0 returns writes (and controls) to the data screen.
    feed(t, "\033[0$}");
    CHECK(t.statusLineActive == 0);
    feed(t, "\rQ");
    CHECK(cell(t, 0, 0)->codePoint == 'Q');

    // (DECSTR's status-line exit is pinned by the soft-reset contract
    // tests alongside the rest of its reset list.)
}

TEST(testXTWINOPSQueryWindowTitle)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    Ubiquitty::hardReset(t);
    feed(t, "\033]2;Ubiquitty title\007");
    CHECK(t.titleUTF8 == "Ubiquitty title");
    feed(t, "\033[21t");
    int n = drain(t, outputBuffer);
    CHECK(n == (int)strlen("\033]lUbiquitty title\033\\"));
    CHECK(strcmp(outputBuffer, "\033]lUbiquitty title\033\\") == 0);

    // CSI 20 t reports the ICON name, which OSC 2 above did not set.
    feed(t, "\033[20t");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]L\033\\") == 0);
    feed(t, "\033]1;icon\007");
    feed(t, "\033[20t");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]Licon\033\\") == 0);
}

TEST(testXTWINOPSTitleStackPushPop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]2;first\007");
    CHECK(t.titleUTF8 == "first");
    // Push title (sub-op 2).
    feed(t, "\033[22;2t");
    CHECK(t.titleStackLength == 1);
    // Replace and confirm new title.
    feed(t, "\033]2;second\007");
    CHECK(t.titleUTF8 == "second");
    // Pop — original restored.
    feed(t, "\033[23;2t");
    CHECK(t.titleStackLength == 0);
    CHECK(t.titleUTF8 == "first");
    CHECK(t.titleChanged);

    // Empty pop is a no-op.
    feed(t, "\033]2;third\007");
    feed(t, "\033[23;2t");
    CHECK(t.titleUTF8 == "third");
    CHECK(t.titleStackLength == 0);

    // Sub-op 0 records both labels in one frame; sub-op 1 restores only the icon but
    // still spends that frame, so the following window pop finds an empty stack and
    // the window title keeps the value set after the push. Pops are counted per
    // frame, never per label.
    feed(t, "\033[22;0t");
    CHECK(t.titleStackLength == 1);
    feed(t, "\033]2;fourth\007");
    feed(t, "\033[23;1t");
    CHECK(t.titleUTF8 == "fourth");
    CHECK(t.titleStackLength == 0);
    feed(t, "\033[23;2t");
    CHECK(t.titleUTF8 == "fourth");
}

TEST(testXTWINOPSTitleStackOverflowIsCapped)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    for (i = 0; i < 11; i++) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\033]2;t%d\007", i);
        feed(t, buffer);
        feed(t, "\033[22;2t");
    }
    CHECK(t.titleStackLength == 10);
    // After 11 pushes (t0..t10), the FIFO-drop leaves t1..t10 in slots
    // 0..9. Popping once returns the most recently pushed ("t10") then
    // count drops to 9.
    CHECK(t.titleUTF8Stack[9] == "t10");
    CHECK(t.titleUTF8Stack[0] == "t1");
    // Pop all 10 entries — must not underflow.
    for (i = 0; i < 10; i++)
        feed(t, "\033[23;2t");
    CHECK(t.titleStackLength == 0);
    // One extra pop is still a no-op.
    feed(t, "\033[23;2t");
    CHECK(t.titleStackLength == 0);
}

TEST(testXTWINOPS8ClampsAnOversizeRequest)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Both values must EXCEED the caps for this to test gridFloor at all: 999 rows
    // stopped doing so when maxRows went 256 -> 1024, which quietly turned this
    // into an assertion that a legal size gets rounded up.
    feed(t, "\033[8;99999;99999t");
    CHECK(t.rows == Ubiquitty::maxRows);
    CHECK(t.columns == Ubiquitty::maxColumns);
}

TEST(testXtwinops8NoOpWithZeroParameters)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[8;0;0t");
    CHECK(t.rows == 24);
    CHECK(t.columns == 80);
}

TEST(testXTWINOPSMutatorsDispatchClient)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    WinOpClient client;
    t.windowClient = &client;

    feed(t, "\033[2t"); // iconify
    CHECK(client.calls == 1);
    CHECK(client.op == 2);

    feed(t, "\033[3;40;60t"); // move to x=40, y=60
    CHECK(client.calls == 2);
    CHECK(client.op == 3);
    CHECK(client.a == 40);
    CHECK(client.b == 60);

    feed(t, "\033[10;2t"); // toggle full-screen
    CHECK(client.calls == 3);
    CHECK(client.op == 10);
    CHECK(client.a == 2);

    /// allowWindowOperations gate suppresses the dispatch entirely.
    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[5t");
    CHECK(client.calls == 3);

    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    t.windowClient = nullptr;
}

TEST(test_xtwinops_8_blocked_by_allowWindowOps)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[8;30;100t");
    CHECK(t.rows == 24);
    CHECK(t.columns == 80);
}

TEST(testXtermModifierAndFocusModes)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];

    Ubiquitty::hardReset(t);
    feed(t, "\033[?1004h\033[?2031h\033[>4;2m");
    CHECK(t.modes.focusReporting);
    CHECK(t.modes.colorThemeReporting);
    CHECK(t.xtermKeyModifiers[4] == 2);
    feed(t, "\033[?4m");
    CHECK(drain(t, outputBuffer) == 7);
    CHECK(strcmp(outputBuffer, "\033[>4;2m") == 0);
    feed(t, "\033[>4;1f\033[>4g");
    CHECK(t.xtermKeyFormats[4] == 1);
    CHECK(drain(t, outputBuffer) == 7);
    CHECK(strcmp(outputBuffer, "\033[>4;1f") == 0);
    feed(t, "\033[>4n");
    CHECK(t.xtermKeyModifiers[4] == Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled);
    feed(t, "\033[?996n");
    CHECK(drain(t, outputBuffer) == 9);
    CHECK(strcmp(outputBuffer, "\033[?997;1n") == 0);
    feed(t, "\033[?1004l\033[?2031l");
    CHECK(!t.modes.focusReporting);
    CHECK(!t.modes.colorThemeReporting);
}

TEST(testModernCliStartupKeyboardCSIsDoNotPaint)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    feed(t, "\0337\033[r\0338\033[?25h\033[?25l\033[?2004h");
    feed(t, "\033[?1004h\033[?2031h\033[<u\033[>1u\033[>4;2m");
    feed(t, "\033[38;5;174mX");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 174);
    CHECK(t.modes.bracketedPaste);
    CHECK(t.modes.focusReporting);
    CHECK(t.modes.colorThemeReporting);
    CHECK(t.kittyKeyboardFlags[0] == std::to_underlying(Kitty::KeyFlag::Disambiguate));
    CHECK(t.xtermKeyModifiers[4] == 2);
}

TEST(testPaintCacheElThenSameContentNoDirty)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;1Hhello world");
    drainPaintCycle(t, nullptr);
    // EL+rewrite of the same content. CellGrid::setCell is idempotent
    // — identical writes don't bump m_anyMutationsSinceLastPaint and
    // the cells stay byte-equal to the shadow.
    feed(t, "\033[5;1H\033[Khello world");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(testPaintCacheElThenDifferentContentDirtiesDiff)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;1Hhello world");
    drainPaintCycle(t, nullptr);
    // EL clears all cells of row 4 to blank, then "hello earth"
    // rewrites columns 0..10. Cells that end up byte-equal to the
    // previous painted state (columns 0..5 of "hello " and column 8 'r'
    // in "wor"/"earth") yield nothing; only the genuinely changed
    // cells do.
    feed(t, "\033[5;1H\033[Khello earth");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[4]);
    // Columns 0..5 = "hello " unchanged from prior paint.
    for (c = 0; c <= 5; c++)
        CHECK(!tr.isCellYielded[4][c]);
    // Columns 6,7,9,10 changed ('w'→'e', 'o'→'a', 'l'→'t', 'd'→'h').
    CHECK(tr.isCellYielded[4][6]);
    CHECK(tr.isCellYielded[4][7]);
    CHECK(tr.isCellYielded[4][9]);
    CHECK(tr.isCellYielded[4][10]);
    // Column 8 = 'r' in both old "world" and new "earth" — unchanged.
    CHECK(!tr.isCellYielded[4][8]);
}

TEST(testPaintCacheIdenticalEmitAfterPaintNoDirty)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "abc");
    drainPaintCycle(t, nullptr);
    // Re-emit identical chars at the same cells.
    feed(t, "\033[1;1Habc");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(test_paint_cache_invalidated_by_markAllDirty)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int r;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "content");
    drainPaintCycle(t, nullptr);
    // CellGrid::requestFullRedraw is the new markAllDirty equivalent.
    // After it, the next paint must yield every row of content as
    // dirty (selection-of-cells differs because m_fullRedraw is on).
    t.grid->requestFullRedraw();
    peekPaintTrace(t, &tr);
    // At least row 0 (where "content" lives) should be yielded.
    CHECK(tr.isRowYielded[0]);
    // With full-redraw, every row gets visited.
    (void)r;
}

TEST(testPaintCacheRepaintIdempotentRow)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;1Hhello");
    drainPaintCycle(t, nullptr);
    // Re-emit the same row content twice. Neither emission should
    // produce dirty work.
    feed(t, "\033[5;1Hhello");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    feed(t, "\033[5;1Hhello");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(testPaintCacheDirtyBoundsTightenAfterIdempotentRewrite)
{
    /// A tightening of the dirty column bounds is not asserted numerically after
    /// an idempotent rewrite. CellGrid does this implicitly: the
    /// PaintRun's firstColumn/lastColumn cover only the cells that
    /// differ from shadow. Re-asserted here as "after a same-content
    /// rewrite, no cells yielded; after a partial-change rewrite,
    /// only the changed columns are inside any yielded run."
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;1Habcdefghij");
    drainPaintCycle(t, nullptr);
    // Same content → no dirty.
    feed(t, "\033[5;1Habcdefghij");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // Change only cell 5 ('f' -> 'F'). Only column 5 should yield.
    feed(t, "\033[5;6HF");
    peekPaintTrace(t, &tr);
    CHECK(tr.isCellYielded[4][5]);
    for (c = 0; c < 5; c++)
        CHECK(!tr.isCellYielded[4][c]);
    for (c = 6; c < 80; c++)
        CHECK(!tr.isCellYielded[4][c]);
}

TEST(testLsThenAlternateNoPaintShadowLeak)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int i;
    char buffer[80];
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    drainPaintCycle(t, nullptr);
    // 30 lines of ls -la-style output. Each scroll: cells shift,
    // CellGrid::scrollRegion queues a pending scroll, drainPaintCycle
    // does the blit + ackScroll. After drain, shadow must match
    // cells — peek yields 0 runs.
    for (i = 0; i < 30; i++) {
        int n = snprintf(buffer, sizeof(buffer), "filename_%02d.txt and some trailing text\r\n", i);
        feed(t, std::string_view(buffer, n));
        drainPaintCycle(t, nullptr);
        peekPaintTrace(t, &tr);
        CHECK(tr.runs == 0);
    }
    // vi/emacs alternate-screen entry. After the paint, alternate blanks must
    // be on screen with no leak from the primary's ls output.
    feed(t, "\033[?1049h");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // Sparse vi-style UI on alternate.
    for (i = 0; i < t.rows - 1; i++) {
        int n = snprintf(buffer, sizeof(buffer), "\033[%d;1H~", i + 1);
        feed(t, std::string_view(buffer, n));
    }
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // Back to primary — the primary cells haven't changed, but BB
    // was overwritten with the alternate UI; the next paint must repaint
    // the primary content. After drain, sync.
    feed(t, "\033[?1049l");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(testPaintSkipsNoShadowLiesAboutBb)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    drainPaintCycle(t, nullptr);
    // Sparse ls-like content.
    feed(t, "drwxr-xr-x  10 user staff   320  May 12 14:00 ARCHITECTURE.md\r\n");
    feed(t, "-rw-r--r--   1 user staff  1024  May 12 14:00 CMakeLists.txt\r\n");
    feed(t, "-rwxr-xr-x   1 user staff   512  May 12 14:00 build.sh\r\n");
    feed(t, "$ ");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // Full clear via \033[H\033[2J. After drain, the shadow must
    // match the now-blank active screen — no leaked ls content.
    feed(t, "\033[H\033[2J");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(testScrollKeepsPaintedShadowInSync)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int i;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    drainPaintCycle(t, nullptr);
    feed(t, "\033[24;1Hbottom-row-text");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // LF at the bottom margin scrolls. After each scroll's paint
    // cycle, shadow must stay in sync with cells.
    for (i = 0; i < 10; i++) {
        feed(t, "\nnext-line-content");
        drainPaintCycle(t, nullptr);
        peekPaintTrace(t, &tr);
        CHECK(tr.runs == 0);
    }
}

TEST(testXTWINOPSCSI20TIconLabel)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    int n;
    Ubiquitty::hardReset(t);
    // OSC 0 sets both labels; OSC 2 alone would leave the icon name empty.
    feed(t, "\033]0;hello\007");
    CHECK(t.titleUTF8 == "hello");
    CHECK(t.iconNameUTF8 == "hello");

    feed(t, "\033[20t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    // Response must be ESC ] L hello ESC \ — capital L.
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == ']');
    CHECK(outputBuffer[2] == 'L');
    CHECK(strcmp(outputBuffer, "\033]Lhello\033\\") == 0);

    // CSI 21 t differs only in the prefix byte case.
    feed(t, "\033[21t");
    n = drain(t, outputBuffer);
    CHECK(outputBuffer[2] == 'l');
}

TEST(testXTWINOPSPopEmptyIsNoop)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    // Set a title so we can verify it doesn't get clobbered.
    feed(t, "\033]2;keep\007");
    CHECK(t.titleStackLength == 0);

    // Pop with empty stack — no crash, title unchanged.
    feed(t, "\033[23;2t");
    CHECK(t.titleStackLength == 0);
    CHECK(t.titleUTF8 == "keep");

    // Pop emits no host response (the operator only writes to osc.data
    // indirectly through titleStackPop, which itself produces no DCS
    // reply).
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testXTWINOPSPushOpVariantsSelectLabel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]2;A\007"); // window title only
    feed(t, "\033]1;icon-A\007"); // icon name only — must not touch the title
    CHECK(t.titleUTF8 == "A");
    CHECK(t.iconNameUTF8 == "icon-A");

    feed(t, "\033[22;1t"); // frame 0: icon only
    CHECK(t.titleStackLength == 1);
    feed(t, "\033[22;2t"); // frame 1: window only
    CHECK(t.titleStackLength == 2);
    feed(t, "\033[22;0t"); // frame 2: both
    CHECK(t.titleStackLength == 3);

    // Change both, then pop the icon: only the icon comes back, and frame 2 is spent
    // — its window label goes with it.
    feed(t, "\033]2;Z\007");
    feed(t, "\033]1;icon-Z\007");
    feed(t, "\033[23;1t");
    CHECK(t.iconNameUTF8 == "icon-A");
    CHECK(t.titleUTF8 == "Z");
    CHECK(t.titleStackLength == 2);

    // Frame 1 recorded the window label.
    feed(t, "\033[23;2t");
    CHECK(t.titleUTF8 == "A");
    CHECK(t.iconNameUTF8 == "icon-A");
    CHECK(t.titleStackLength == 1);

    // Frame 0 recorded only the icon: a window pop finds no window label anywhere at
    // or below it, so the title is left alone — but the frame is still consumed.
    feed(t, "\033]2;Y\007");
    feed(t, "\033[23;2t");
    CHECK(t.titleUTF8 == "Y");
    CHECK(t.titleStackLength == 0);
}

TEST(testXTWINOPSPushChangePopRoundTrip)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033]2;foo\007");
    feed(t, "\033[22;2t"); // push
    CHECK(t.titleStackLength == 1);
    feed(t, "\033]2;bar\007");
    CHECK(t.titleUTF8 == "bar");
    feed(t, "\033[23;2t"); // pop
    CHECK(t.titleUTF8 == "foo");
    // Query — must respond with the restored title.
    feed(t, "\033[21t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033]lfoo\033\\") == 0);
}

TEST(testBellWantsNotificationWhenUnfocused)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Fresh Ubiquitty::Terminal is assumed focused → a bell would not notify.
    CHECK(t.windowFocused == true);
    CHECK(Ubiquitty::Modes::bellWantsNotification(&t) == false);

    /// Losing focus (no DECSET 1004) flips the gate on without any wire
    /// output, so a subsequent bell can raise a banner.
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);
    char outputBuffer[8];
    CHECK(drain(t, outputBuffer) == 0);
    CHECK(t.windowFocused == false);
    CHECK(Ubiquitty::Modes::bellWantsNotification(&t) == true);

    /// Regaining focus closes the gate again.
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    CHECK(t.windowFocused == true);
    CHECK(Ubiquitty::Modes::bellWantsNotification(&t) == false);

    // Null Ubiquitty::Terminal is a safe false.
    CHECK(Ubiquitty::Modes::bellWantsNotification(nullptr) == false);
}

TEST(testDECSSDTDECSASDSpecDollarForm)
{
    Ubiquitty::Terminal t;
    char outputBuffer[32];
    int n;
    Ubiquitty::hardReset(t);
    // DECSSDT type 2 (host-writable).
    feed(t, "\033[2$~");
    CHECK(t.statusLineType == 2);
    // No host response, no stray cells.
    n = drain(t, outputBuffer);
    CHECK(n == 0);
    CHECK(cell(t, 0, 0) == nullptr || cell(t, 0, 0)->codePoint == ' ' || cell(t, 0, 0)->codePoint == 0);

    // DECSASD 1 → status line active.
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    feed(t, "HI");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'H');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'I');
    /// Data screen row 0 untouched by the redirected writes.
    const Ubiquitty::Cell* c0 = cell(t, 0, 0);
    CHECK(c0 == nullptr || (c0->codePoint != 'H' && c0->codePoint != 'I'));

    // DECSASD 0 → back to data screen; DECSSDT 0 → no status line.
    feed(t, "\033[0$}");
    CHECK(t.statusLineActive == 0);
    feed(t, "\033[0$~");
    CHECK(t.statusLineType == 0);
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testDECSSDTClampAndStatusCursorRewind)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // An undefined type leaves the selected status display unchanged.
    feed(t, "\033[5$~");
    CHECK(t.statusLineType == 0);
    // Type 2 selects the host-writable line.
    feed(t, "\033[2$~");
    CHECK(t.statusLineType == 2);

    // Activate the status line with a bold-red pen, write two cells.
    feed(t, "\033[1;31m");
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    CHECK(t.statusCursorX == 0);
    feed(t, "AB");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'A');
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint == 'B');
    CHECK(t.statusCursorX == 2);
    // The pen rides onto the status cell (bold attribute + indexed red fg).
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).pen.foregroundIndex == 1);

    // Re-selecting the status display preserves its independent cursor.
    feed(t, "\033[0$}");
    CHECK(t.statusLineActive == 0);
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    CHECK(t.statusCursorX == 2);
    feed(t, "Z");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 2).codePoint == 'Z');
    CHECK(t.statusCursorX == 3);
}

TEST(testDECSSDTTypeSelectionNoneIndicatorHost)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Default is 0 (none).
    CHECK(t.statusLineType == 0);

    // 1 → indicator (terminal-drawn).
    feed(t, "\033[1$~");
    CHECK(t.statusLineType == 1);

    // 2 → host-writable.
    feed(t, "\033[2$~");
    CHECK(t.statusLineType == 2);

    // Back to 1, then 0.
    feed(t, "\033[1$~");
    CHECK(t.statusLineType == 1);
    feed(t, "\033[0$~");
    CHECK(t.statusLineType == 0);

    // Default param (no Ps) → 0 (none).
    feed(t, "\033[2$~");
    feed(t, "\033[$~");
    CHECK(t.statusLineType == 0);

    // No host response, no stray cells from any type select.
    char outputBuffer[32];
    CHECK(drain(t, outputBuffer) == 0);
}

TEST(testDECPSSingleNote)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // volume 5, 8/32 s (= 250 ms), note 1 (C5 = 523 Hz).
    feed(t, "\033[5;8;1,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 5);
    CHECK(decPSCapture.request.durationMilliseconds == 250);
    CHECK(decPSCapture.request.count == 1);
    CHECK(decPSCapture.request.freqs[0] == 523);
    // A plain request is the VT520's: the speaker's wave, the speaker's
    // channel, the voice's own envelope, no gap, no transposition.
    CHECK(decPSCapture.request.wave == 0);
    CHECK(decPSCapture.request.channel == 0);
    CHECK(decPSCapture.request.attackMilliseconds == Ubiquitty::PAL::PlayTonesRequest::waveDefaultEnvelope);
    CHECK(decPSCapture.request.releaseMilliseconds == Ubiquitty::PAL::PlayTonesRequest::waveDefaultEnvelope);
    CHECK(decPSCapture.request.gapMilliseconds == 0);
    CHECK(decPSCapture.request.semitonesDown == 0);
    CHECK(decPSCapture.request.noteVolumes[0] == 0);
}

TEST(testDECPSSubParametersNameTheVoice)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // volume 3, channel 2, sine, an octave down, 10 ms attack, no decay, a
    // full sustain, 20 ms release; 8/32 s with a 2/32 s gap; C5, then C6 at
    // its own volume 7.
    feed(t, "\033[3:2:1:12:10:0:7:20;8:2;1;13:7,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 3);
    CHECK(decPSCapture.request.wave == 1);
    CHECK(decPSCapture.request.channel == 2);
    CHECK(decPSCapture.request.attackMilliseconds == 10);
    CHECK(decPSCapture.request.releaseMilliseconds == 20);
    CHECK(decPSCapture.request.semitonesDown == 12);
    CHECK(decPSCapture.request.durationMilliseconds == 250);
    CHECK(decPSCapture.request.gapMilliseconds == 62);
    CHECK(decPSCapture.request.count == 2);
    CHECK(decPSCapture.request.freqs[0] == 523);
    CHECK(decPSCapture.request.freqs[1] == 1047);
    CHECK(decPSCapture.request.noteVolumes[0] == 0);
    CHECK(decPSCapture.request.noteVolumes[1] == 7);
    CHECK(decPSCapture.request.decayMilliseconds == 0);
    CHECK(decPSCapture.request.sustainLevel == 7);
    CHECK(decPSCapture.request.reverbSend == 0);
    CHECK(decPSCapture.request.gridUnits == 0);
    CHECK(decPSCapture.request.lowpassHertz == 0);
    CHECK(decPSCapture.request.highpassHertz == 0);
    // The envelope's decay and sustain, the filters, the hall send, the
    // grid, and a note's effect and its byte.
    feed(t, "\033[5:1:1:0:0:120:3:0:800:200:5;8:0:64;13:7:4:36;1::14:200,~");
    CHECK(decPSCapture.calls == 2);
    CHECK(decPSCapture.request.decayMilliseconds == 120);
    CHECK(decPSCapture.request.sustainLevel == 3);
    CHECK(decPSCapture.request.lowpassHertz == 800);
    CHECK(decPSCapture.request.highpassHertz == 200);
    CHECK(decPSCapture.request.reverbSend == 5);
    CHECK(decPSCapture.request.gridUnits == 64);
    CHECK(decPSCapture.request.noteEffects[0] == 4);
    CHECK(decPSCapture.request.noteParameters[0] == 36);
    CHECK(decPSCapture.request.noteVolumes[1] == 0);
    CHECK(decPSCapture.request.noteEffects[1] == 14);
    CHECK(decPSCapture.request.noteParameters[1] == 200);
    // Every sub-parameter is bounded: a wave past the last, a channel past
    // eight, a transposition past four octaves, a gap past the duration.
    feed(t, "\033[7:99:9:99:0:9999:9:0:99999:99999:9;300:400:999;1::99:999,~");
    CHECK(decPSCapture.calls == 3);
    CHECK(decPSCapture.request.wave == 6);
    CHECK(decPSCapture.request.lowpassHertz == 20000);
    CHECK(decPSCapture.request.highpassHertz == 20000);
    CHECK(decPSCapture.request.reverbSend == 7);
    CHECK(decPSCapture.request.gridUnits == 255);
    CHECK(decPSCapture.request.decayMilliseconds == 5000);
    CHECK(decPSCapture.request.sustainLevel == 7);
    CHECK(decPSCapture.request.noteEffects[0] == 15);
    CHECK(decPSCapture.request.noteParameters[0] == 255);
    CHECK(decPSCapture.request.channel == 8);
    CHECK(decPSCapture.request.semitonesDown == 48);
    CHECK(decPSCapture.request.durationMilliseconds == 255u * 1000u / 32u); // the VT520's bound
    CHECK(decPSCapture.request.gapMilliseconds == decPSCapture.request.durationMilliseconds);
}

TEST(testDECPSVolumeZeroIsAStop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // No notes, volume 0, channel 3: a stop for channel 3 reaches the hook.
    feed(t, "\033[0:3,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 0);
    CHECK(decPSCapture.request.channel == 3);
    CHECK(decPSCapture.request.count == 0);
    // The bare form stops everything: channel 0.
    feed(t, "\033[0,~");
    CHECK(decPSCapture.calls == 2);
    CHECK(decPSCapture.request.channel == 0);
}

namespace {
// A four-frame 8 kHz mono 16-bit WAV, then its base64, for the download tests.
std::string tinyWavBase64()
{
    std::vector<uint8_t> wav;
    const auto put16 = [&wav](uint16_t v) { wav.push_back(static_cast<uint8_t>(v & 0xFF)); wav.push_back(static_cast<uint8_t>(v >> 8)); };
    const auto put32 = [&wav](uint32_t v) { for (int i = 0; i < 4; ++i) wav.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
    const auto putTag = [&wav](const char* tag) { for (int i = 0; i < 4; ++i) wav.push_back(static_cast<uint8_t>(tag[i])); };
    putTag("RIFF");
    put32(36 + 8);
    putTag("WAVE");
    putTag("fmt ");
    put32(16);
    put16(1); // PCM
    put16(1); // mono
    put32(8000);
    put32(16000);
    put16(2);
    put16(16);
    putTag("data");
    put32(8);
    for (uint16_t v : { 1000, 2000, 3000, 4000 })
        put16(v);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < wav.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(wav[i]) << 16;
        if (i + 1 < wav.size())
            n |= static_cast<uint32_t>(wav[i + 1]) << 8;
        if (i + 2 < wav.size())
            n |= wav[i + 2];
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < wav.size() ? alphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < wav.size() ? alphabet[n & 63] : '=');
    }
    return out;
}
} // namespace

TEST(testSampleDownloadFeedsTheSampleVoice)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // DCS Pslot ; Pformat + s <base64 WAV> ST: slot 1 holds the file at the
    // synth's rate, four frames at 8 kHz becoming twenty-two at 44.1 kHz.
    const std::string download = "\033P1;0+s" + tinyWavBase64() + "\033\\";
    feed(t, download.c_str());
    CHECK(t.sampleBank != nullptr);
    CHECK(t.sampleBank->slot(1) != nullptr);
    CHECK(t.sampleBank->slot(1)->size() == 22u);
    CHECK(t.sampleBank->slot(2) == nullptr);
    CHECK(t.sampleBank->serial == 1u);
    // A slot past the bank, a format the terminal does not decode, and a
    // payload that is not a WAV are refused without touching the bank.
    feed(t, "\033P26;0+sAAAA\033\\");
    feed(t, "\033P2;1+sAAAA\033\\");
    feed(t, "\033P2;0+sAAAA\033\\");
    CHECK(t.sampleBank->slot(2) == nullptr);
    CHECK(t.sampleBank->serial == 1u);

    // The sample voice: the first sounding note names the slot, every
    // sounding note replays it, and the hook is handed the slot's frames.
    feed(t, "\033[5:2:6;0;1;0;1,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.wave == 6);
    CHECK(decPSCapture.request.channel == 2);
    CHECK(decPSCapture.request.sampleSlot == 1);
    CHECK(decPSCapture.request.samplePCM != nullptr);
    CHECK(decPSCapture.request.sampleFrames == 22u);
    CHECK(decPSCapture.request.count == 3);
    CHECK(decPSCapture.request.durationMilliseconds == 0);
    // The queued host request owns the exact parser-selected frames.
    CHECK(t.pendingSound != nullptr);
    CHECK(t.pendingSound->count == 1);
    REQUIRE(t.pendingSound->front());
    const auto forwarded = t.pendingSound->front()->request;
    CHECK(forwarded.sampleSlot == 1);
    CHECK(forwarded.samplePCM != nullptr);
    CHECK(forwarded.samplePCM != decPSCapture.request.samplePCM);
    CHECK(forwarded.sampleFrames == 22u);
    t.pendingSound->pop();
    CHECK(!t.pendingSound->front());
    // A slot nothing was downloaded to plays nothing, and says so.
    feed(t, "\033[5:0:6;8;2,~");
    CHECK(decPSCapture.calls == 2);
    CHECK(decPSCapture.request.sampleSlot == 2);
    CHECK(decPSCapture.request.samplePCM == nullptr);
    CHECK(decPSCapture.request.sampleFrames == 0u);
    // RIS drops the bank with the terminal, as it drops the soft font.
    feed(t, "\033c");
    CHECK(t.sampleBank == nullptr);
}

TEST(testRISStopsEveryChannel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    feed(t, "\033[5:3:1;8;1,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 5);
    // The program is reset out from under its sequence: the hook sees the
    // stop for every channel.
    feed(t, "\033c");
    CHECK(decPSCapture.calls == 2);
    CHECK(decPSCapture.request.volume == 0);
    CHECK(decPSCapture.request.channel == 0);
    CHECK(decPSCapture.request.count == 0);
}

TEST(testDECPSThreeNoteSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // volume 7, 16/32 s (= 500 ms), notes 1 / 13 / 25 → C5 / C6 / C7.
    feed(t, "\033[7;16;1;13;25,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 7);
    CHECK(decPSCapture.request.durationMilliseconds == 500);
    CHECK(decPSCapture.request.count == 3);
    CHECK(decPSCapture.request.freqs[0] == 523); // C5
    CHECK(decPSCapture.request.freqs[1] == 1047); // C6 (octave up)
    CHECK(decPSCapture.request.freqs[2] == 2093); // C7 (two octaves up)
    // Past the VT520's two octaves the scale goes on to C9; past that is a rest.
    feed(t, "\033[7;16;37;49;50,~");
    CHECK(decPSCapture.calls == 2);
    CHECK(decPSCapture.request.freqs[0] == 4186); // C8
    CHECK(decPSCapture.request.freqs[1] == 8372); // C9
    CHECK(decPSCapture.request.freqs[2] == 0);
}

TEST(testDECPSClampsVolumeAndRestNote)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // volume 99 clamps to 7; note 0 is a rest → 0 Hz frequency.
    feed(t, "\033[99;32;0,~");
    CHECK(decPSCapture.calls == 1);
    CHECK(decPSCapture.request.volume == 7);
    CHECK(decPSCapture.request.durationMilliseconds == 1000); // 32/32 s
    CHECK(decPSCapture.request.count == 1);
    CHECK(decPSCapture.request.freqs[0] == 0);
}

TEST(testDECPSNoNotesIsNoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    decPSCapture = DECPSCapture {};
    Ubiquitty::PAL::ScopedContext scopedTones(palContextWithPlayTones(&decpsRecord));
    // No note parameters → nothing to play; the hook must not fire.
    feed(t, "\033[5;8,~");
    CHECK(decPSCapture.calls == 0);
}

TEST(testBellWindowModes)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(t.bellRaisesUrgency == false);
    CHECK(t.popOnBell == false);
    feed(t, "\033[?1042$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1042;2$y") == 0);
    feed(t, "\033[?1043$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1043;2$y") == 0);
    feed(t, "\033[?1042h");
    CHECK(t.bellRaisesUrgency == true);
    feed(t, "\033[?1042$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1042;1$y") == 0);
    feed(t, "\033[?1043h");
    CHECK(t.popOnBell == true);
    feed(t, "\033[?1043$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1043;1$y") == 0);
    feed(t, "\033[?1042l");
    feed(t, "\033[?1043l");
    CHECK(t.bellRaisesUrgency == false);
    CHECK(t.popOnBell == false);
}

TEST(testMediaCopyPrintScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char path[] = "/tmp/ubq_print_test_XXXXXX";
    int fileDescriptor = mkstemp(path);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor >= 0)
        close(fileDescriptor);
    CHECK(Ubiquitty::setPrintSinkPath(&t, path) == true);
    feed(t, "Line1\r\nLine2");
    feed(t, "\033[0i"); // MC print screen
    Ubiquitty::setPrintSinkPath(&t, nullptr); // flush + close
    FILE* f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[4096] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n > 0);
        CHECK(strstr(buf, "Line1") != nullptr);
        CHECK(strstr(buf, "Line2") != nullptr);
    }
    remove(path);
}

TEST(testMediaCopyPrintLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char path[] = "/tmp/ubq_printline_test_XXXXXX";
    int fileDescriptor = mkstemp(path);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor >= 0)
        close(fileDescriptor);
    CHECK(Ubiquitty::setPrintSinkPath(&t, path) == true);
    feed(t, "RowA\r\nRowB"); // cursor now on row 1 (RowB)
    feed(t, "\033[?1i"); // print line containing cursor → RowB
    Ubiquitty::setPrintSinkPath(&t, nullptr);
    FILE* f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[256] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n > 0);
        CHECK(strstr(buf, "RowB") != nullptr);
        CHECK(strstr(buf, "RowA") == nullptr); // only the cursor row
    }
    remove(path);
}

TEST(testMediaCopyAutoPrint)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char path[] = "/tmp/ubq_autoprint_test_XXXXXX";
    int fileDescriptor = mkstemp(path);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor >= 0)
        close(fileDescriptor);
    CHECK(Ubiquitty::setPrintSinkPath(&t, path) == true);
    CHECK(t.autoPrint == false);
    feed(t, "\033[?5i"); // auto-print on
    CHECK(t.autoPrint == true);
    feed(t, "alpha\r\nbeta\r\n"); // two completed lines leave the row
    feed(t, "\033[?4i"); // auto-print off
    CHECK(t.autoPrint == false);
    feed(t, "gamma\r\n"); // not captured
    Ubiquitty::setPrintSinkPath(&t, nullptr);
    FILE* f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[512] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n > 0);
        CHECK(strstr(buf, "alpha") != nullptr);
        CHECK(strstr(buf, "beta") != nullptr);
        CHECK(strstr(buf, "gamma") == nullptr);
    }
    remove(path);
}

static std::string printerOutput(Ubiquitty::Terminal& terminal)
{
    auto* sink = static_cast<FILE*>(terminal.printSinkFile);
    std::rewind(sink);
    std::array<char, 1024> bytes {};
    const auto length = std::fread(bytes.data(), 1, bytes.size(), sink);
    return { bytes.data(), length };
}

TEST(printerControllerDivertsRawBytesAtEveryChunkBoundary)
{
    const std::string payload = "raw\a\r\n\033[6n\033]52;c;YWJj\a\xc3\xa9\xff\033X\033\033[5i!";
    const std::string expected = "raw\a\r\n\033[6n\033]52;c;YWJj\a\xc3\xa9\xff\033X\033!";
    const std::string input = "A\033[5i" + payload + "\033[4iB";
    for (std::size_t split = 0; split <= input.size(); ++split) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        terminal.printSinkFile = std::tmpfile();
        REQUIRE(terminal.printSinkFile);
        feed(terminal, std::string_view(input).substr(0, split));
        feed(terminal, std::string_view(input).substr(split));
        CHECK(printerOutput(terminal) == expected);
        CHECK(cell(terminal, 0, 0)->codePoint == 'A');
        CHECK(cell(terminal, 0, 1)->codePoint == 'B');
        CHECK(terminal.screens.active->cursorX == 2);
        CHECK(terminal.printMode == 0);
        std::array<char, 128> reply {};
        CHECK(drain(terminal, reply) == 0);
    }
}

TEST(printerControllerSharesTheByteAndSynchronizedDrivers)
{
    const std::string input = "A\033[5iP\033[?2026lQ\033[4iB\033[?2026lC";
    Ubiquitty::Terminal bytes;
    Ubiquitty::hardReset(bytes);
    bytes.printSinkFile = std::tmpfile();
    REQUIRE(bytes.printSinkFile);
    for (const auto byte : input)
        Ubiquitty::processByte(&bytes, static_cast<uint8_t>(byte));
    CHECK(printerOutput(bytes) == "P\033[?2026lQ");
    CHECK(cell(bytes, 0, 0)->codePoint == 'A');
    CHECK(cell(bytes, 0, 1)->codePoint == 'B');
    CHECK(cell(bytes, 0, 2)->codePoint == 'C');

    Ubiquitty::Terminal synchronized;
    Ubiquitty::hardReset(synchronized);
    synchronized.printSinkFile = std::tmpfile();
    REQUIRE(synchronized.printSinkFile);
    feed(synchronized, "\033[?2026h");
    bool didStop = false;
    const auto consumed = Ubiquitty::processBytesUpToSyncClear(&synchronized, input, &didStop);
    CHECK(didStop);
    CHECK(consumed == static_cast<int>(input.size() - 1));
    CHECK(printerOutput(synchronized) == "P\033[?2026lQ");
    CHECK(cell(synchronized, 0, 1)->codePoint == 'B');
    CHECK(cell(synchronized, 0, 2)->codePoint == 0);
}

TEST(printerControllerPreservesPendingWrapAndCancelsAutoprint)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 5);
    feed(terminal, "ABCDE\033[?5i\033[5iX\r\nY\033[4iF");
    CHECK(!terminal.autoPrint);
    CHECK(terminal.printMode == 0);
    CHECK(cell(terminal, 0, 4)->codePoint == 'E');
    CHECK(cell(terminal, 1, 0)->codePoint == 'F');
    CHECK(terminal.screens.active->cursorX == 1);
    CHECK(terminal.screens.active->cursorY == 1);
}

TEST(printerNULPolicyFiltersOnlyPrinterData)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    terminal.printSinkFile = std::tmpfile();
    REQUIRE(terminal.printSinkFile);
    CHECK(terminal.shouldIgnorePrinterNUL);
    constexpr char input[] = "\033[5iA\0B\021C\023D\033[4i\033[?102l\033[5iE\0F\021G\023H\033[4iI\0J";
    feed(terminal, std::string_view(input, sizeof(input) - 1));
    CHECK(printerOutput(terminal) == std::string("ABCDE\0FGH", 9));
    CHECK(cell(terminal, 0, 0)->codePoint == 'I');
    CHECK(cell(terminal, 0, 1)->codePoint == 'J');
    CHECK(!terminal.shouldIgnorePrinterNUL);
    Ubiquitty::hardReset(terminal);
    CHECK(terminal.shouldIgnorePrinterNUL);
    CHECK(terminal.printMode == 0);
}

TEST(printerControllerBypassesTheSelectedTextEncoding)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    terminal.printSinkFile = std::tmpfile();
    REQUIRE(terminal.printSinkFile);
    feed(terminal, "\033%@\033[5i\xe9\2335i\xff\2334i\xe9");
    CHECK(printerOutput(terminal) == "\xe9\xff");
    CHECK(cell(terminal, 0, 0)->codePoint == 0xE9);
    CHECK(terminal.screens.active->cursorX == 1);
    CHECK(terminal.printMode == 0);
}

TEST(printerNULModeReportsAndRestoresItsActualPolicy)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[?102$p\033[?102s\033[?102l\033[?102$p\033[?102r\033[?102$p");
    std::array<char, 128> reply {};
    const auto length = drain(terminal, reply);
    CHECK(std::string_view(reply.data(), length) == "\033[?102;1$y\033[?102;2$y\033[?102;1$y");
    CHECK(terminal.shouldIgnorePrinterNUL);
}

TEST(testMediaCopyNoSinkIsInert)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "X");
    feed(t, "\033[0i"); // no sink — no-op
    feed(t, "\033[5i"); // printer controller on
    CHECK(t.printMode == 5);
    feed(t, "\033[4i"); // printer controller off
    CHECK(t.printMode == 0);
    // Auto-print toggles even without a sink (writes are gated separately).
    feed(t, "\033[?5i");
    CHECK(t.autoPrint == true);
}

// Display memory is the one page on screen: DECSLPP sets its height, and NP, PP, PPA, PPR and PPB,
// with no other page to move to, are ignored, the cursor staying where it was.
TEST(testPageMovementIsIgnoredWithOnePageOfMemory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[24t");
    CHECK(t.rows == 24);

    feed(t, "\033[2;3H\033[U\033[V\033[2 P\033[ Q\033[ RX");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 3);
    CHECK(cell(t, 1, 2)->codePoint == 'X');
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

// The status row's 32 KB is not part of a Terminal with no status line.
TEST(testStatusRowStorageIsAbsentUntilWritten)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK_MSG(Ubiquitty::Rendering::statusRow(t).empty(), "no status line, no row");

    // Absent reads as blanks, in range and out.
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 0);
    CHECK(Ubiquitty::Rendering::statusCellAt(t, Ubiquitty::maxColumns - 1).codePoint == 0);

    CHECK(Ubiquitty::Rendering::statusCellAt(t, -1).codePoint == 0);
    CHECK(Ubiquitty::Rendering::statusCellAt(t, Ubiquitty::maxColumns).codePoint == 0);

    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    feed(t, "Z");
    CHECK_MSG(!Ubiquitty::Rendering::statusRow(t).empty(), "the first write allocates the row");
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 'Z');
}

// XTTITLEPOS reports the stack's length and its maximum index, ten, as xterm's
// MAX_SAVED_TITLES; a third XTWINOPS parameter stores into and reads from that
// entry without moving the length (ctlseqs, XTWINOPS 22 and 23).
TEST(testXTTITLEPOSAndDirectTitleStackEntries)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[64] = {};
    feed(t, "\033[#S");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[0;10#S") == 0);

    feed(t, "\033]2;one\007\033[22;0t\033[22;0t\033[23;0t\033[5#S");
    CHECK(drain(t, buffer) > 0);
    CHECK(strcmp(buffer, "\033[1;10#S") == 0);

    feed(t, "\033]2;stored\007\033[22;0;3t\033]2;two\007\033[23;0;3t");
    CHECK(t.titleUTF8 == "stored");
    CHECK(t.titleStackLength == 1);
    feed(t, "\033[23;0;3t");
    CHECK(t.titleStackLength == 1);
}

TEST(testStatusDisplayRequiresWritableTypeAndExitsWhenDisabled)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1$}A");
    CHECK(!t.statusLineActive);
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'A');
    feed(t, "\033[2$~\033[1$}BC\033[9$~");
    CHECK(t.statusLineActive);
    CHECK(t.statusLineType == 2);
    CHECK(t.statusCursorX == 2);
    feed(t, "\033[0$~D");
    CHECK(!t.statusLineActive);
    CHECK(UbiquittyTest::cell(t, 0, 1)->codePoint == 'D');
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusCursorX == 0);
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint != 'B');
}

// DEC STD 070: DECSASD selects the status display only once DECSSDT has made
// it host-writable, and DECSSDT choosing another type exits it.
TEST(testDECSASDNeedsAHostWritableStatusLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5);
    feed(t, "\033[1$}AB\033[0$}C");
    CHECK(t.statusLineActive == 0);
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    feed(t, "\033[2$~\033[1$}");
    CHECK(t.statusLineActive == 1);
    feed(t, "\033[0$~");
    CHECK(t.statusLineActive == 0);
    feed(t, "D");
    CHECK(cell(t, 0, 3)->codePoint == 'D');
}

// DEC STD 070: with the host-writable status line selected, "most ANSI control
// functions received will affect only the Status Display", and on its one row
// ED acts on the character positions EL would.
TEST(testEraseInDisplayOnTheStatusLineLeavesTheMainDisplay)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5);
    feed(t, "XY\033[2$~\033[1$}ABCD\033[3G\033[J");
    auto status = Ubiquitty::Rendering::statusRow(t);
    REQUIRE(!status.empty());
    CHECK(status[1].codePoint == 'B');
    CHECK(status[2].codePoint == 0);
    CHECK(status[3].codePoint == 0);
    feed(t, "\033[1J");
    CHECK(status[0].codePoint == 0);
    CHECK(status[1].codePoint == 0);
    feed(t, "\033[5GE\033[2J\033[3J\033[0$}");
    CHECK(status[4].codePoint == 0);
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
}

UBIQUITTY_TEST_MAIN()
