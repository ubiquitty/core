// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tek 4014 emulation: mode switch, coordinate decode, plot and alpha
// primitives, PAGE, line style, char size, and GIN + ENQ report framing.

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;

#include "Core/Tek/Project.h"
#include "Core/Tek/Tek4014.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Parser.h"
#include "PAL/TerminalClients.h"
#include <cstdio>
#include <cstring>

using namespace Ubiquitty::Tek;

static void feed(Ubiquitty::Terminal& t, const char* s, int n) { Ubiquitty::processBytes(&t, std::string_view(s, static_cast<std::size_t>(n))); }

// CSI ? 38 h enters Tek mode; a printable byte then routes to Tek
// (appends an alpha char) rather than the VT100 screen.
// ESC ETX leaves Tek; a following VT sequence parses normally.
// ESC CAN also exits.
// GS dark move then a draw: first coordinate sets position (no line),
// second coordinate appends a line from the first to the second.
// GS BEL is the pen-down marker: the BEL clears the dark-vector flag so the
// next coordinate draws a line (not a dark move) and does not ring the bell.
// Emitters send "GS BEL" to draw and plain "GS" to move, so treating that BEL
// as a bell both beeps and leaves the pen up for the whole plot.
// Coordinate decode of a known group yields the expected (x, y).
// PAGE (ESC FF) commits the frame to the display buffer, clears the working
// tube, homes alpha. The committed frame is what the renderer shows.
// Alpha chars append + advance the cursor; CR/LF/BS move it.
// Line style + char size update state and are captured on primitives.
// GIN: tekGINKey transmits the exact framed bytes.
// ESC ENQ status/address report: status byte + current pos + CR.
// Incremental plot: RS then pen-down steps append points.
// Screen dims accessor reports Tek space.
// GIN crosshair tracks the pointer live via tekUpdateGINPosition while armed,
// clamps to the tube, and ignores updates once GIN is disabled.
// FS point-plot leaves the alpha cursor at the graphic position on US entry.
// Alpha text scrolling off the bottom flips to margin 2 (right column); PAGE
// resets to margin 1.
// After an ENQ report the terminal bypasses echoed bytes until a control.
// Coordinate byte-omission: a point re-sending only the changed Lo-X reuses the
// previous point's high bytes (only Lo-X is mandatory), not jump to the corner.
// tekCommitFrame commits the working tube to the display buffer (the frontend's
// ingest-idle hook), so a frame drawn without a trailing PAGE still displays.
// Real harness (tek_demo) byte-omission sequence: GS, full move to (400,1600),
// then two draws sharing Y + Hi-X sent as a single Lo-X byte each. A correct
// decoder reuses the prior high bytes (clean horizontal run); a naive one fires
// the vectors to the bottom-left corner.
// Re-entering Tek mode clears the tube, so the previous session's last frame
// never flashes before the new program's first draw.
// DECRST 38 arriving inside Tek mode exits to VT, mirroring the DECSET 38
// entry — hosts that enter via CSI expect to leave the same way.
/// Any other CSI inside Tek is swallowed whole: no exit, and none of its
/// bytes draw as alpha garbage.
namespace {
struct TekDiagClient : Ubiquitty::DiagnosticsClient {
    int listFull = 0;
    uint32_t lastDetail = 0;
    void log(std::string_view tag, uint32_t detail) override
    {
        if (tag == "tek-list-full") {
            listFull++;
            lastDetail = detail;
        }
    }
};
}

/// Overflowing the storage tube raises one tek-list-full diagnostic per
/// frame instead of dropping primitives silently; PAGE re-arms it.
TEST(testEnterSetsTekmode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!t.isTekModeActive);
    feed(t, "\033[?38h", 6);
    CHECK(t.isTekModeActive);
    /// An alpha char now lands in the Tek display list, not the grid.
    int before = Ubiquitty::tekPrimitiveCount(t);
    feed(t, "A", 1);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before + 1);
}

TEST(testExitReturnsToVT)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    CHECK(t.isTekModeActive);
    feed(t, "\033\003", 2); // ESC ETX
    CHECK(!t.isTekModeActive);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    // A VT CSI now parses: cursor position to row 5 column 3.
    feed(t, "\033[5;3H", 6);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testExitViaCan)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\033\030", 2); // ESC CAN
    CHECK(!t.isTekModeActive);
}

// A pane left in Tek mode is recovered by soft reset, which is the only recovery
// the user has.
//
// Tek mode is a byte-routing state: while it is set, every host byte goes to the
// Tek machine and the VT dispatch is unreachable. So no sequence the stream sends
// can leave it — RIS included, because 'c' (0x63) sits in Tek's 0x60-0x6F
// line-type range and is read as a line style. Leaving is reachable only from
// inside the Tek machine (ESC ETX, ESC CAN, CSI ?38l), which a stream that
// entered and stopped never sends. Any program that can print can therefore
// blank a pane permanently with six bytes.
//
// softReset is the escape hatch — the GUI's "Reset Terminal" calls it, and so
// does the host's DECSTR — and it already unwinds the other states a dead peer
// can strand.
//
// Fails-without-it: isTekModeActive stays set through the reset, so the CSI below is
// eaten by the Tek machine and the cursor never moves.
TEST(testSoftResetLeavesTekMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[?38h", 6);
    CHECK(t.isTekModeActive);
    feed(t, "A", 1); // one glyph reaches the display list, then the stream stops

    // Confirm the trap before asserting the escape: RIS does not get out.
    feed(t, "\033c", 2);
    CHECK(t.isTekModeActive); // 'c' was read as a Tek line style, not as a reset

    Ubiquitty::softReset(&t);
    CHECK(!t.isTekModeActive);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    // The VT dispatch is reachable again: cursor position to row 5 column 3.
    feed(t, "\033[5;3H", 6);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 2);
}

// Soft reset outside Tek mode resets no Tek state — the Tek branch is
// conditional, and a terminal that was never in Tek mode must not acquire a
// parser-state reset.
TEST(testSoftResetOutsideTekModeIsUnchanged)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!t.isTekModeActive);
    Ubiquitty::softReset(&t);
    CHECK(!t.isTekModeActive);
    feed(t, "\033[5;3H", 6);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testGraphDarkThenDraw)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    /// GS, then address (x=2000,y=1000): HiY=0x27 LoY=0x7A HiX=0x2F LoX=0x54
    const char first[] = { 0x1D, 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, first, 5);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 0); // dark move, no line
    CHECK(t.tek.currentX == 2000);
    CHECK(t.tek.currentY == 1000);
    /// Second address (x=2000,y=1000 again is fine; use x=100,y=200).
    /// y=200: hi=1, lo=(200>>2)&31=50&31=18 -> LoY=0x60|18=0x72; HiY=0x21.
    /// x=100: hi=0, lo=(100>>2)&31=25 -> LoX=0x40|25=0x59; HiX=0x20.
    const char second[] = { 0x21, 0x72, 0x20, 0x59 };
    feed(t, second, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 1);
    const Primitive* p = Ubiquitty::tekPrimitive(t, 0);
    CHECK(p != nullptr);
    CHECK(p->kind == PrimitiveKind::Line);
    CHECK(p->x0 == 2000 && p->y0 == 1000);
    CHECK(p->x1 == 100 && p->y1 == 200);
}

TEST(testGraphBelPenDown)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    /// GS + dark move to (2000,1000).
    const char move[] = { 0x1D, 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, move, 5);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 0);
    CHECK(t.bellPending == false);
    /// GS BEL + (100,200): the BEL pen-down makes this draw a line, no bell.
    const char draw[] = { 0x1D, 0x07, 0x21, 0x72, 0x20, 0x59 };
    feed(t, draw, 6);
    CHECK(t.bellPending == false); // BEL in vector mode = pen-down, not a bell
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 1);
    const Primitive* p2 = Ubiquitty::tekPrimitive(t, 0);
    CHECK(p2 != nullptr);
    CHECK(p2->kind == PrimitiveKind::Line);
    CHECK(p2->x0 == 2000 && p2->y0 == 1000);
    CHECK(p2->x1 == 100 && p2->y1 == 200);
}

TEST(testCoordDecode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point plot (each coord -> point)
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 }; // x=2000,y=1000
    feed(t, addr, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 1);
    const Primitive* p = Ubiquitty::tekPrimitive(t, 0);
    CHECK(p->kind == PrimitiveKind::Point);
    CHECK(p->x0 == 2000 && p->y0 == 1000);
}

TEST(testPageClearsAndHomes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1);
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) > 0);
    feed(t, "\033\014", 2); // ESC FF — PAGE: commit frame + clear working
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 0); // working cleared
    CHECK(Ubiquitty::tekDisplayCount(t) == 1); // frame committed
    CHECK(t.tek.plotMode == PlotMode::Alpha);
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 0);
    // Homed near the top of Tek space (origin bottom-left).
    CHECK(Ubiquitty::tekAlphaCursorY(t) > 0);
    // New drawing goes to the working tube; the committed frame keeps showing
    // until the next PAGE commits the new one.
    feed(t, "\034", 1); // FS point mode
    const char addr2[] = { 0x21, 0x72, 0x20, 0x59 };
    feed(t, addr2, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 1);
    CHECK(Ubiquitty::tekDisplayCount(t) == 1);
}

TEST(testAlphaCharsAndMotion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    int x0 = Ubiquitty::tekAlphaCursorX(t);
    int y0 = Ubiquitty::tekAlphaCursorY(t);
    feed(t, "AB", 2);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 2);
    const Primitive* a = Ubiquitty::tekPrimitive(t, 0);
    CHECK(a->kind == PrimitiveKind::Glyph);
    CHECK(a->codePoint == 'A');
    CHECK(a->x0 == x0);
    int xAfter = Ubiquitty::tekAlphaCursorX(t);
    CHECK(xAfter > x0); // advanced
    feed(t, "\010", 1); // BS
    CHECK(Ubiquitty::tekAlphaCursorX(t) < xAfter);
    feed(t, "\015", 1); // CR
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 0);
    feed(t, "\012", 1); // LF — down one line: y decreases
    CHECK(Ubiquitty::tekAlphaCursorY(t) < y0);
}

TEST(testLineStyleAndCharSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\033a", 2); // ESC a — dotted
    CHECK(Ubiquitty::tekLineStyle(t) == LineStyle::Dotted);
    // Draw a vector and confirm the style is captured.
    feed(t, "\035", 1); // GS
    const char p1[] = { 0x27, 0x7A, 0x2F, 0x54 }; // dark move
    feed(t, p1, 4);
    const char p2[] = { 0x21, 0x72, 0x20, 0x59 }; // draw
    feed(t, p2, 4);
    const Primitive* line = Ubiquitty::tekPrimitive(t, 0);
    CHECK(line->kind == PrimitiveKind::Line);
    CHECK(line->lineStyle == LineStyle::Dotted);

    // Char size: ESC : selects the third size; an alpha char captures it.
    feed(t, "\037", 1); // US — back to alpha
    feed(t, "\033:", 2); // ESC :
    CHECK(Ubiquitty::tekCharacterSize(t) == CharSize::Third);
    feed(t, "Z", 1);
    int n = Ubiquitty::tekPrimitiveCount(t);
    const Primitive* ch = Ubiquitty::tekPrimitive(t, n - 1);
    CHECK(ch->kind == PrimitiveKind::Glyph);
    CHECK(ch->charSize == CharSize::Third);
}

// Tek's two reports face the capability gate every other reply in Core faces.
//
// respond is the single chokepoint: it checks the named capability against the
// terminal's current origin and queues nothing unless the answer is Allow. Both
// Tek reports wrote to Terminal::outputBuffer directly instead, so
// Capability::TektronixAddressRead existed, had a tier, and decided nothing.
//
// TektronixAddressRead sits in the Benign tier, which answers Allow at every
// origin, so a consulted gate and an unconsulted one emit the same bytes under
// the shipping policy. Only a policy that refuses tells them apart, which is what
// the one below is for: it is the difference between a capability that decides
// something and a capability that is decoration until the tier moves.
//
// Fails-without-it: the reports queue their six bytes under a policy that
// refused them.
namespace {
struct DenyingPolicy final : Ubiquitty::CapabilityPolicy {
    Ubiquitty::Capability denied = Ubiquitty::Capability::TektronixAddressRead;
    int checks = 0;
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        ++checks;
        return capability == denied ? Ubiquitty::CapabilityDecision::Deny : Ubiquitty::CapabilityDecision::Allow;
    }
};
}

TEST(testTekReportsAreCapabilityGated)
{
    char buffer[64];

    // (a) ENQ status/address report, denied.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        DenyingPolicy policy;
        t.capabilityPolicy = &policy;
        feed(t, "\033[?38h", 6);
        CHECK(t.isTekModeActive);
        (void)drain(t, buffer); // clear anything the mode switch queued
        feed(t, "\033\005", 2); // ESC ENQ
        const int n = drain(t, buffer);
        t.capabilityPolicy = nullptr;
        CHECK_MSG(n == 0, "a denied ENQ report queues nothing");
        CHECK_MSG(policy.checks > 0, "and the gate was actually consulted");
    }

    // (b) GIN key report, denied.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        DenyingPolicy policy;
        t.capabilityPolicy = &policy;
        feed(t, "\033[?38h", 6);
        feed(t, "\033\032", 2); // ESC SUB — arm GIN
        (void)drain(t, buffer);
        Ubiquitty::tekGINKey(t, 'q', 100, 200);
        const int n = drain(t, buffer);
        t.capabilityPolicy = nullptr;
        CHECK_MSG(n == 0, "a denied GIN report queues nothing");
    }

    // (c) The same two reports emit when the policy allows. Without this arm the
    // two denied cases above would pass against a change that dropped both
    // reports entirely.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        DenyingPolicy policy;
        policy.denied = Ubiquitty::Capability::None; // deny nothing relevant
        t.capabilityPolicy = &policy;
        feed(t, "\033[?38h", 6);
        (void)drain(t, buffer);
        feed(t, "\033\005", 2);
        CHECK(drain(t, buffer) == 6);

        feed(t, "\033\032", 2);
        (void)drain(t, buffer);
        Ubiquitty::tekGINKey(t, 'q', 100, 200);
        CHECK(drain(t, buffer) == 6);
        t.capabilityPolicy = nullptr;
    }

    // (d) A GIN key of NUL still reports six bytes. respond's string form would
    // measure this report as empty, so the counted form is what keeps a keystroke
    // that happens to be 0x00 from silently sending nothing.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[?38h", 6);
        feed(t, "\033\032", 2);
        (void)drain(t, buffer);
        Ubiquitty::tekGINKey(t, '\0', 100, 200);
        CHECK_MSG(drain(t, buffer) == 6, "a NUL keystroke is a key, not a terminator");
    }
}

TEST(testGINKeyFraming)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\033\032", 2); // ESC SUB — enable GIN
    CHECK(Ubiquitty::tekGINActive(t) != 0);

    Ubiquitty::tekGINKey(t, 'l', 2000, 1000);
    CHECK(Ubiquitty::tekGINActive(t) == 0); // auto-disabled

    char outputBuffer[64];
    int n = drain(t, outputBuffer);
    CHECK(n == 6);
    // key, Hi-X, Lo-X, Hi-Y, Lo-Y, CR
    CHECK((unsigned char)outputBuffer[0] == 'l');
    CHECK((unsigned char)outputBuffer[1] == (0x20 | ((2000 >> 7) & 0x1F)));
    CHECK((unsigned char)outputBuffer[2] == (0x20 | ((2000 >> 2) & 0x1F)));
    CHECK((unsigned char)outputBuffer[3] == (0x20 | ((1000 >> 7) & 0x1F)));
    CHECK((unsigned char)outputBuffer[4] == (0x20 | ((1000 >> 2) & 0x1F)));
    CHECK((unsigned char)outputBuffer[5] == '\r');
}

TEST(testEnqReport)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    // Set the current position via a point at (2000,1000).
    feed(t, "\034", 1);
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    feed(t, "\033\005", 2); // ESC ENQ

    char outputBuffer[64];
    int n = drain(t, outputBuffer);
    CHECK(n == 6);
    CHECK((unsigned char)outputBuffer[0] == 0x20); // status
    CHECK((unsigned char)outputBuffer[1] == (0x20 | ((2000 >> 7) & 0x1F)));
    CHECK((unsigned char)outputBuffer[2] == (0x20 | ((2000 >> 2) & 0x1F)));
    CHECK((unsigned char)outputBuffer[3] == (0x20 | ((1000 >> 7) & 0x1F)));
    CHECK((unsigned char)outputBuffer[4] == (0x20 | ((1000 >> 2) & 0x1F)));
    CHECK((unsigned char)outputBuffer[5] == '\r');
}

TEST(testIncrementalPlot)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    // Position via GS dark move to (2000,1000).
    feed(t, "\035", 1);
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    feed(t, "\036", 1); // RS — incremental
    feed(t, "P", 1); // pen down
    int before = Ubiquitty::tekPrimitiveCount(t);
    feed(t, "D", 1); // printable step byte; bit 0x04 set => north step
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before + 1);
    const Primitive* p = Ubiquitty::tekPrimitive(t, Ubiquitty::tekPrimitiveCount(t) - 1);
    CHECK(p->kind == PrimitiveKind::Point);
}

TEST(testScreenDims)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::tekScreenWidth(t) == tekGridWidth);
    CHECK(Ubiquitty::tekScreenHeight(t) == tekGridHeight);
}

TEST(testGINLiveTracking)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\033\032", 2); // ESC SUB — arm GIN
    Ubiquitty::tekUpdateGINPosition(t, 1500, 800);
    CHECK(Ubiquitty::tekGINX(t) == 1500);
    CHECK(Ubiquitty::tekGINY(t) == 800);
    Ubiquitty::tekUpdateGINPosition(t, 99999, -5); // clamps to the tube
    CHECK(Ubiquitty::tekGINX(t) == 4095);
    CHECK(Ubiquitty::tekGINY(t) == 0);
    Ubiquitty::tekGINKey(t, 'l', 0, 0); // report disables GIN
    CHECK(Ubiquitty::tekGINActive(t) == 0);
    Ubiquitty::tekUpdateGINPosition(t, 200, 200); // no-op while off
    CHECK(Ubiquitty::tekGINX(t) != 200);
}

TEST(testPointAlphaSync)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point mode
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 }; // (2000,1000)
    feed(t, addr, 4);
    feed(t, "\037", 1); // US — alpha follows the graphic position
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 2000);
    CHECK(Ubiquitty::tekAlphaCursorY(t) == 1000);
}

TEST(testMargin2Wrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6); // large font: ~35 lines tall
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 0); // margin 1
    for (int i = 0; i < 40; ++i)
        feed(t, "\n", 1); // scroll past the bottom once
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 2048); // margin 2 (width/2)
    feed(t, "\033\014", 2); // ESC FF — PAGE resets to margin 1
    CHECK(Ubiquitty::tekAlphaCursorX(t) == 0);
}

TEST(testBypassAfterReport)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\035", 1); // GS dark move
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    feed(t, "\037", 1); // US — alpha
    int before = Ubiquitty::tekPrimitiveCount(t);
    feed(t, "\033\005", 2); // ESC ENQ — report, sets bypass
    feed(t, "ABC", 3); // echoed report bytes: swallowed, not drawn
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before);
    feed(t, "\r", 1); // control byte clears bypass
    feed(t, "X", 1); // now alpha draws again
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before + 1);
}

TEST(testCoordByteOmission)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point mode
    const char full[] = { 0x27, 0x7A, 0x2F, 0x54 }; // (2000,1000)
    feed(t, full, 4);
    const char loxOnly[] = { 0x50 }; // tag 0x40, lo5=16: only Lo-X re-sent
    feed(t, loxOnly, 1);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 2);
    const Primitive* p = Ubiquitty::tekPrimitive(t, 1);
    CHECK(p != nullptr);
    CHECK(p->kind == PrimitiveKind::Point);
    CHECK(p->x0 == 1984); // (15<<7)|(16<<2): Hi-X reused, Lo-X new
    CHECK(p->y0 == 1000); // Y fully reused
}

TEST(testByteOmissionGraphRun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    const char seq[] = { 0x1d, 0x2c, 0x70, 0x23, 0x44, 0x4e, 0x58 };
    feed(t, seq, 7);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 2); // 2 horizontal segments
    const Primitive* a = Ubiquitty::tekPrimitive(t, 0);
    const Primitive* b = Ubiquitty::tekPrimitive(t, 1);
    CHECK(a->kind == PrimitiveKind::Line);
    CHECK(a->x0 == 400 && a->y0 == 1600 && a->x1 == 440 && a->y1 == 1600);
    CHECK(b->x0 == 440 && b->y0 == 1600 && b->x1 == 480 && b->y1 == 1600);
}

TEST(testCommitFrameIdle)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point mode
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 1);
    CHECK(Ubiquitty::tekDisplayCount(t) == 0); // not committed yet
    Ubiquitty::tekCommitFrame(t); // ingest-idle hook
    CHECK(Ubiquitty::tekDisplayCount(t) == 1);
    const Primitive* p = Ubiquitty::tekDisplayPrimitives(t);
    CHECK(p[0].kind == PrimitiveKind::Point);
    CHECK(p[0].x0 == 2000 && p[0].y0 == 1000);
}

TEST(testReenterClearsTube)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point mode
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    feed(t, addr, 4);
    feed(t, "\033\014", 2); // ESC FF — PAGE: commits the frame
    CHECK(Ubiquitty::tekDisplayCount(t) > 0);
    feed(t, "\033\003", 2); // ESC ETX — exit Tek
    feed(t, "\033[?38h", 6); // re-enter Tek: fresh tube
    CHECK(Ubiquitty::tekPrimitiveCount(t) == 0);
    CHECK(Ubiquitty::tekDisplayCount(t) == 0);
}

TEST(testCSIDecrst38Exits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    CHECK(t.isTekModeActive);
    feed(t, "\033[?38l", 6);
    CHECK(!t.isTekModeActive);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    // A VT CSI now parses: cursor to row 5 column 3.
    feed(t, "\033[5;3H", 6);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testCSISwallowed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38h", 6);
    int before = Ubiquitty::tekPrimitiveCount(t);
    feed(t, "\033[2J", 4);
    CHECK(t.isTekModeActive);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before);
    // Wrong parameter does not exit either.
    feed(t, "\033[?39l", 6);
    CHECK(t.isTekModeActive);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before);
    // Normal alpha processing resumes after the swallowed sequence.
    feed(t, "A", 1);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == before + 1);
}

TEST(testListFullDiagnostic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    TekDiagClient diag;
    t.diagnosticsClient = &diag;
    feed(t, "\033[?38h", 6);
    feed(t, "\034", 1); // FS — point plot
    const char addr[] = { 0x27, 0x7A, 0x2F, 0x54 };
    for (int i = 0; i < tekMaxPrimitives + 8; i++)
        feed(t, addr, 4);
    CHECK(Ubiquitty::tekPrimitiveCount(t) == tekMaxPrimitives);
    CHECK(diag.listFull == 1);
    CHECK(diag.lastDetail == (uint32_t)tekMaxPrimitives);
    // PAGE clears the tube and re-arms the diagnostic.
    feed(t, "\033\014", 2);
    feed(t, "\034", 1);
    for (int i = 0; i < tekMaxPrimitives + 8; i++)
        feed(t, addr, 4);
    CHECK(diag.listFull == 2);
    t.diagnosticsClient = nullptr;
}

// A solid pattern and a sub-pixel segment both come back whole: the caller
// strokes what it is given without asking which case it was.
TEST(testStrokeRunsSolidPatternYieldsWholeSegment)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[8];
    DashPattern solid {};
    solid.count = 0;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 100.0f, 0.0f }, solid, runs);
    CHECK(n == 1);
    CHECK(runs[0].start.x == 0.0f);
    CHECK(runs[0].end.x == 100.0f);
}

TEST(testStrokeRunsSubPixelSegmentYieldsWholeSegment)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[8];
    DashPattern dash {};
    dash.count = 1;
    dash.on[0] = 4.0f;
    dash.off[0] = 4.0f;
    const int n = dashRunsForSegment(DevicePoint { 3.0f, 3.0f }, DevicePoint { 3.25f, 3.0f }, dash, runs);
    CHECK(n == 1);
    CHECK(runs[0].end.x == 3.25f);
}

// The stamp: on 4 / off 4 across 20px draws at 0-4, 8-12, 16-20.
TEST(testStrokeRunsStampsPatternAlongSegment)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[8];
    DashPattern dash {};
    dash.count = 1;
    dash.on[0] = 4.0f;
    dash.off[0] = 4.0f;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 20.0f, 0.0f }, dash, runs);
    CHECK(n == 3);
    CHECK(runs[0].start.x == 0.0f);
    CHECK(runs[0].end.x == 4.0f);
    CHECK(runs[1].start.x == 8.0f);
    CHECK(runs[1].end.x == 12.0f);
    CHECK(runs[2].start.x == 16.0f);
    CHECK(runs[2].end.x == 20.0f);
}

// The last run is clipped to the segment rather than overshooting it.
TEST(testStrokeRunsClipsFinalRunToSegment)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[8];
    DashPattern dash {};
    dash.count = 1;
    dash.on[0] = 6.0f;
    dash.off[0] = 2.0f;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 10.0f, 0.0f }, dash, runs);
    CHECK(n == 2);
    CHECK(runs[1].start.x == 8.0f);
    CHECK(runs[1].end.x == 10.0f);
}

// A pattern of zeros would advance the walk by nothing. The bound is what makes
// the loop finite, so this is the case that hangs if the guard is dropped.
TEST(testStrokeRunsZeroLengthPatternTerminates)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[8];
    DashPattern dash {};
    dash.count = 2;
    dash.on[0] = 0.0f;
    dash.off[0] = 0.0f;
    dash.on[1] = 0.0f;
    dash.off[1] = 0.0f;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 6.0f, 0.0f }, dash, runs);
    CHECK(n == 3);
    CHECK(runs[0].end.x == 1.0f);
}

// The output extent is the budget: a pattern finer than the span gets truncated
// rather than overrunning, which is the frontend's per-line run cap.
TEST(testStrokeRunsStopsWhenOutputIsFull)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[2];
    DashPattern dash {};
    dash.count = 1;
    dash.on[0] = 1.0f;
    dash.off[0] = 1.0f;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 100.0f, 0.0f }, dash, runs);
    CHECK(n == 2);
}

TEST(testStrokeRunsEmptyOutputWritesNothing)
{
    using namespace Ubiquitty::Tek;
    DashPattern dash {};
    dash.count = 0;
    CHECK(dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 9.0f, 0.0f }, dash, {}) == 0);
}

// Diagonals carry the stamp along the segment, not along an axis: a 3-4-5
// triangle's 5px run ends exactly on the far endpoint.
TEST(testStrokeRunsFollowSegmentDirection)
{
    using namespace Ubiquitty::Tek;
    StrokeRun runs[4];
    DashPattern dash {};
    dash.count = 1;
    dash.on[0] = 5.0f;
    dash.off[0] = 100.0f;
    const int n = dashRunsForSegment(DevicePoint { 0.0f, 0.0f }, DevicePoint { 3.0f, 4.0f }, dash, runs);
    CHECK(n == 1);
    CHECK(runs[0].end.x > 2.99f && runs[0].end.x < 3.01f);
    CHECK(runs[0].end.y > 3.99f && runs[0].end.y < 4.01f);
}

// The walk letters a Glyph with the beam, as the tube did: an A is four
// strokes of outline and one bar, between the two points' single runs. A
// frontend that draws the stream draws text without a pass of its own.
TEST(testStrokeWalkLettersGlyphPrimitives)
{
    using namespace Ubiquitty::Tek;
    Projection projection = makeProjection(tekGridWidth, tekGridHeight, 0, 0, 1024, 768);
    Primitive list[3] {};
    list[0].kind = PrimitiveKind::Point;
    list[1].kind = PrimitiveKind::Glyph;
    list[1].codePoint = 'A';
    list[2].kind = PrimitiveKind::Point;
    StrokeRun runs[16];
    const int n = buildStrokeRuns(std::span { list, 3 }, projection, runs);
    CHECK(n == 2 + 5);
}

// A point becomes one degenerate run at its position: the beam is round, so
// the capsule a renderer strokes round it is the beam's spot.
TEST(testStrokeWalkPointIsOneDegenerateRun)
{
    using namespace Ubiquitty::Tek;
    Projection projection = makeProjection(tekGridWidth, tekGridHeight, 0, 0, 1024, 768);
    Primitive list[1] {};
    list[0].kind = PrimitiveKind::Point;
    list[0].x0 = 100;
    list[0].y0 = 100;
    StrokeRun runs[4];
    const int n = buildStrokeRuns(std::span { list, 1 }, projection, runs);
    CHECK(n == 1);
    const DevicePoint at = project(projection, 100, 100);
    CHECK(runs[0].start.x == at.x && runs[0].start.y == at.y);
    CHECK(runs[0].end.x == at.x && runs[0].end.y == at.y);
}

// One pathological dashed line must not consume the whole output: the per-line
// limit is what leaves room for the primitives after it.
TEST(testStrokeWalkLimitsRunsPerLineSoLaterPrimitivesSurvive)
{
    using namespace Ubiquitty::Tek;
    Projection projection = makeProjection(tekGridWidth, tekGridHeight, 0, 0, 1024, 768);
    Primitive list[2] {};
    list[0].kind = PrimitiveKind::Line;
    list[0].lineStyle = LineStyle::Dotted;
    list[0].x0 = 0;
    list[0].y0 = 0;
    list[0].x1 = tekGridWidth - 1;
    list[0].y1 = 0;
    list[1].kind = PrimitiveKind::Point;
    list[1].x0 = 10;
    list[1].y0 = 10;
    StrokeRun runs[maxStrokeRunsPerLine + 4];
    const int n = buildStrokeRuns(std::span { list, 2 }, projection, runs);
    // The dotted line is capped, and the point after it still lands.
    CHECK(n == maxStrokeRunsPerLine + 1);
    CHECK(runs[maxStrokeRunsPerLine].start.y == runs[maxStrokeRunsPerLine].end.y);
}

// Writing stops at the caller's extent rather than past it.
TEST(testStrokeWalkStopsAtOutputExtent)
{
    using namespace Ubiquitty::Tek;
    Projection projection = makeProjection(tekGridWidth, tekGridHeight, 0, 0, 1024, 768);
    Primitive list[5] {};
    for (auto& primitive : list) {
        primitive.kind = PrimitiveKind::Point;
        primitive.x0 = 10;
        primitive.y0 = 10;
    }
    StrokeRun runs[2];
    CHECK(buildStrokeRuns(std::span { list, 5 }, projection, runs) == 2);
    CHECK(buildStrokeRuns(std::span { list, 5 }, projection, {}) == 0);
}

TEST(testGraphicInputKeyReportsOnlyWhileTheCrosshairIsUp)
{
    char buffer[64];
    // Tek mode off: an ordinary keystroke, and the caller is told so rather
    // than left to guess whether its key was spent.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CHECK(!Ubiquitty::tekReportGraphicInputKey(t, 'q', 10.0f, 20.0f, 800.0f, 600.0f));
        CHECK(drain(t, buffer) == 0);
    }

    // Tek mode on but no crosshair: still an ordinary keystroke. Without this
    // arm the mode alone would look like the gate.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[?38h", 6);
        (void)drain(t, buffer);
        CHECK(!Ubiquitty::tekReportGraphicInputKey(t, 'q', 10.0f, 20.0f, 800.0f, 600.0f));
        CHECK(drain(t, buffer) == 0);
    }

    // Crosshair up: the press is the trigger, and the report is the framed six
    // bytes leading with the typed key.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033[?38h", 6);
        feed(t, "\033\032", 2); // ESC SUB — arm GIN
        (void)drain(t, buffer);
        CHECK(Ubiquitty::tekReportGraphicInputKey(t, 'q', 10.0f, 20.0f, 800.0f, 600.0f));
        const int reported = drain(t, buffer);
        CHECK(reported == 6);
        CHECK(buffer[0] == 'q');

        // One-shot: the crosshair disables itself with the report, so the next
        // press is an ordinary keystroke again. A frontend that kept sending
        // reports would swallow every key until the host armed GIN afresh.
        CHECK(!Ubiquitty::tekReportGraphicInputKey(t, 'r', 10.0f, 20.0f, 800.0f, 600.0f));
        CHECK(drain(t, buffer) == 0);
    }

    // The projection is over the view rect handed in: the same point in a
    // taller view is a different Tek row, which is what makes passing the grid
    // box instead of the full view a real error rather than a rounding one.
    {
        Ubiquitty::Terminal wide;
        Ubiquitty::hardReset(wide);
        feed(wide, "\033[?38h", 6);
        feed(wide, "\033\032", 2);
        (void)drain(wide, buffer);
        CHECK(Ubiquitty::tekReportGraphicInputKey(wide, 'q', 400.0f, 100.0f, 800.0f, 600.0f));
        char wideReport[8] = {};
        CHECK(drain(wide, wideReport) == 6);

        Ubiquitty::Terminal tall;
        Ubiquitty::hardReset(tall);
        feed(tall, "\033[?38h", 6);
        feed(tall, "\033\032", 2);
        (void)drain(tall, buffer);
        CHECK(Ubiquitty::tekReportGraphicInputKey(tall, 'q', 400.0f, 100.0f, 800.0f, 1200.0f));
        char tallReport[8] = {};
        CHECK(drain(tall, tallReport) == 6);

        CHECK(std::memcmp(wideReport, tallReport, 6) != 0);
    }
    printf("testGraphicInputKeyReportsOnlyWhileTheCrosshairIsUp done\n");
}

// A vector chain along the bottom of the address space draws whole. The chain
// GS, Hi-Y 0, Lo-X 0 moves to the origin; Extra, Lo-Y 31, Hi-X 24, Lo-X 16
// draws to (3136, 124) in 12-bit space; Lo-Y 8, Lo-X 0 draws on to (3072, 32).
// Projected to the very edge of the device rect, a line at y = 0 left half its
// stroke and all of its lower halo below the drawable, which showed as a
// sliver at the bottom of the window.
TEST(testBeamProjectionKeepsALowVectorChainInsideTheViewport)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const char chain[] = "\033[?38h\033\014\035 @`\1778Ph@";
    feed(t, chain, static_cast<int>(sizeof(chain) - 1));
    const PrimitiveSpan primitives = Ubiquitty::tekVisiblePrimitives(t);
    CHECK(primitives.size() == 2);
    if (primitives.size() != 2)
        return;
    CHECK(primitives[0].kind == PrimitiveKind::Line);
    CHECK(primitives[0].x0 == 0 && primitives[0].y0 == 0);
    CHECK(primitives[0].x1 == 3136 && primitives[0].y1 == 124);
    CHECK(primitives[1].x0 == 3136 && primitives[1].y0 == 124);
    CHECK(primitives[1].x1 == 3072 && primitives[1].y1 == 32);

    // The snapshot case's viewport, 24 x 6 cells of 13 x 34 points, at 1x and
    // at the 2x the comparison renders at.
    for (const float scale : { 1.0f, 2.0f }) {
        const float width = 312.0f * scale;
        const float height = 204.0f * scale;
        const BeamGeometry beam = beamGeometry(scale);
        const Projection projection = makeBeamProjection(tekGridWidth, tekGridHeight, 0, 0, width, height, scale);
        StrokeRun runs[2 * maxStrokeRunsPerLine];
        const int count = buildStrokeRuns(primitives, projection, runs);
        CHECK(count == 2);
        // Everything the beam lights reaches `haloReach` past the run, which
        // also covers the stroke's own half-width.
        CHECK(beam.haloReach > beam.halfThickness + 1.0f);
        for (int index = 0; index < count; ++index) {
            for (const DevicePoint end : { runs[index].start, runs[index].end }) {
                CHECK(end.x - beam.haloReach >= 0.0f);
                CHECK(end.x + beam.haloReach <= width);
                CHECK(end.y - beam.haloReach >= 0.0f);
                CHECK(end.y + beam.haloReach <= height);
            }
        }
    }

    // A pointer on the drawn origin unprojects to the origin: the GIN path and
    // the drawing share the inset projection, one at points and one at pixels.
    const Projection drawn = makeBeamProjection(tekGridWidth, tekGridHeight, 0, 0, 624.0f, 408.0f, 2.0f);
    const DevicePoint origin = project(drawn, 0, 0);
    int tekX = -1;
    int tekY = -1;
    const Projection pointer = makeBeamProjection(tekGridWidth, tekGridHeight, 0, 0, 312.0f, 204.0f, 1.0f);
    unproject(pointer, origin.x * 0.5f, origin.y * 0.5f, &tekX, &tekY);
    CHECK(tekX == 0 && tekY == 0);
}

UBIQUITTY_TEST_MAIN()
