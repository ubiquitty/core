// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Tek/Tek4014.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"

#include <array>
#include <cstring>

// Tek 4014 engine. The byte-level contract lives in Tek4014.h; this unit decodes the host stream into the
// storage-tube display list and answers the read-only accessors.

namespace Ubiquitty::Tek {
namespace {

// Per-char-size cell advance in Tek units. The four sizes pack 74/81/121/133 columns and 35/38/58/64 lines
// across the visible 4096×3120 space.
struct CellMetrics {
    int advX;
    int advY;
};

CellMetrics cellMetrics(CharSize size)
{
    switch (size) {
    case CharSize::Large:
        return CellMetrics { 56, 88 }; // 74 columns, 35 rows
    case CharSize::Second:
        return CellMetrics { 51, 82 }; // 81 columns, 38 rows
    case CharSize::Third:
        return CellMetrics { 34, 53 }; // 121 columns, 58 rows
    case CharSize::Small:
        return CellMetrics { 31, 48 }; // 133 columns, 64 rows
    }
    // Outside the enumeration, which is the whole tube at its coarsest text —
    // the same answer CharSize::Large gives, so a corrupted size draws large
    // rather than nothing. Not a `default:`: with all four covered, one would
    // let a fifth size compile as this fallback instead of failing -Wswitch.
    return CellMetrics { 56, 88 };
}

void clampX(int16_t& x)
{
    if (x < 0)
        x = 0;
    if (x > tekGridWidth - 1)
        x = static_cast<int16_t>(tekGridWidth - 1);
}

void clampY(int16_t& y)
{
    if (y < 0)
        y = 0;
    if (y > tekGridHeight - 1)
        y = static_cast<int16_t>(tekGridHeight - 1);
}

// Append a primitive; drop if the storage tube is full. The drop flag
// raises a diagnostic from tekByte, which holds the Terminal.
void push(State& s, const Primitive& p)
{
    if (s.primitiveCount >= tekMaxPrimitives) {
        s.didDropPrimitive = true;
        return;
    }
    s.primitives[s.primitiveCount++] = p;
}

// Swap the working frame into the display buffer the renderer reads (on PAGE and
// on ingest-idle), so the renderer never shows a half-drawn frame.
void commitFrame(State& s)
{
    if (s.primitiveCount > 0)
        std::memcpy(s.displayPrimitives, s.primitives, static_cast<size_t>(s.primitiveCount) * sizeof(Primitive));
    s.displayCount = s.primitiveCount;
}

void pushLine(State& s, int x0, int y0, int x1, int y1)
{
    Primitive p;
    p = {};
    p.kind = PrimitiveKind::Line;
    p.lineStyle = s.lineStyle;
    p.x0 = static_cast<int16_t>(x0);
    p.y0 = static_cast<int16_t>(y0);
    p.x1 = static_cast<int16_t>(x1);
    p.y1 = static_cast<int16_t>(y1);
    push(s, p);
}

void pushPoint(State& s, int x, int y)
{
    Primitive p;
    p = {};
    p.kind = PrimitiveKind::Point;
    p.lineStyle = s.lineStyle;
    p.x0 = static_cast<int16_t>(x);
    p.y0 = static_cast<int16_t>(y);
    push(s, p);
}

void pushCharacter(State& s, uint32_t codePoint, int x, int y)
{
    Primitive p;
    p = {};
    p.kind = PrimitiveKind::Glyph;
    p.charSize = s.charSize;
    p.x0 = static_cast<int16_t>(x);
    p.y0 = static_cast<int16_t>(y);
    p.codePoint = codePoint;
    push(s, p);
}

// Reset the in-flight coordinate-group accumulator.
void coordReset(State& s)
{
    // Seed from the current point so a group may omit any unchanged high-order byte and re-send only what
    // differs (only Lo-X is mandatory). Zeroing here made a Lo-X-only point jump to the bottom-left corner.
    s.pendingX = s.currentX;
    s.pendingY = s.currentY;
    s.coordHasHighY = false;
    s.coordHasExtra = false;
    s.coordHasLowY = false;
    s.coordHasHighX = false;
    s.parse = TekParse::Ground;
}

// Home the alpha cursor to the top-left text position (Tek space:
// origin bottom-left, so top is y = tekGridHeight - cellHeight).
void alphaHome(State& s)
{
    CellMetrics m = cellMetrics(s.charSize);
    s.alphaMargin = 0; // PAGE/home returns to margin 1 (left column)
    s.alphaX = s.alphaMargin;
    s.alphaY = static_cast<int16_t>(tekGridHeight - m.advY);
    clampY(s.alphaY);
}

// Move the alpha cursor down one line. Off the bottom of the tube it flips the
// margin (4014 two-column page) and homes y to the top in the other column.
void alphaAdvanceLine(State& s)
{
    CellMetrics m = cellMetrics(s.charSize);
    s.alphaY = static_cast<int16_t>(s.alphaY - m.advY);
    if (s.alphaY < 0) {
        s.alphaMargin = (s.alphaMargin == 0) ? static_cast<int16_t>(tekGridWidth / 2) : 0;
        s.alphaY = static_cast<int16_t>(tekGridHeight - m.advY);
        clampY(s.alphaY);
        s.alphaX = s.alphaMargin;
    }
}

// Apply a completed graphic address in the active plot mode.
void coordComplete(State& s)
{
    int x = s.pendingX;
    int y = s.pendingY;
    int16_t nx = static_cast<int16_t>(x);
    int16_t ny = static_cast<int16_t>(y);
    clampX(nx);
    clampY(ny);

    if (s.plotMode == PlotMode::Graph) {
        if (s.isNextVectorDark) {
            // First coordinate after GS: dark move, no line drawn.
            s.isNextVectorDark = false;
        } else {
            pushLine(s, s.currentX, s.currentY, nx, ny);
        }
        s.currentX = nx;
        s.currentY = ny;
        s.alphaX = nx;
        s.alphaY = ny;
    } else if (s.plotMode == PlotMode::Point) {
        pushPoint(s, nx, ny);
        s.currentX = nx;
        s.currentY = ny;
    }
    coordReset(s);
}

// Feed one byte to the graphic-address decoder. Each byte's tag (top two
// bits) classifies it; the group completes on the Lo-X byte (tag 0x40).
void coordByte(State& s, uint8_t b)
{
    uint8_t tag = static_cast<uint8_t>(b & 0x60);
    uint8_t lo5 = static_cast<uint8_t>(b & 0x1F);

    switch (tag) {
    case 0x20:
        // Hi-Y arrives before Lo-Y, Hi-X after it. Either may be omitted; the
        // seeded (previous-point) value then carries over untouched.
        if (s.coordHasLowY) {
            s.pendingX = (s.pendingX & ~(0x1F << 7)) | (lo5 << 7);
            s.coordHasHighX = true;
        } else {
            s.pendingY = (s.pendingY & ~(0x1F << 7)) | (lo5 << 7);
            s.coordHasHighY = true;
        }
        break;
    case 0x60:
        // A 0x60 byte is Lo-Y — even when Hi-Y was omitted. If a SECOND 0x60
        // arrives this group, the FIRST was the Extra/EGM byte (y bits[1:0],
        // x bits[1:0]); recover it and take this byte as the real Lo-Y.
        if (!s.coordHasLowY) {
            s.pendingY = (s.pendingY & ~(0x1F << 2)) | (lo5 << 2);
            s.coordHasLowY = true;
        } else if (!s.coordHasExtra) {
            uint8_t extra = static_cast<uint8_t>((s.pendingY >> 2) & 0x1F);
            s.pendingY = (s.pendingY & ~(0x1F << 2)) | (lo5 << 2);
            s.pendingY = (s.pendingY & ~0x03) | (extra & 0x03); // y bits[1:0]
            s.pendingX = (s.pendingX & ~0x03) | ((extra >> 2) & 0x03); // x[1:0]
            s.coordHasExtra = true;
        }
        break;
    case 0x40:
        // Lo-X terminates the group and plots.
        s.pendingX = (s.pendingX & ~(0x1F << 2)) | (lo5 << 2);
        coordComplete(s);
        break;
    default:
        break;
    }
}

// Incremental-plot byte: pen-state / direction step.
void incrByte(State& s, uint8_t b)
{
    // Each byte steps the pen ±1 per direction bit (0x04 N / 0x08 S / 0x10 E /
    // 0x20 W); 'P' lowers the pen, ' ' raises it; pen-down steps plot a point.
    int dx = 0, dy = 0;
    if (b & 0x04)
        dy += 1; // north
    if (b & 0x08)
        dy -= 1; // south
    if (b & 0x10)
        dx += 1; // east
    if (b & 0x20)
        dx -= 1; // west
    int16_t nx = static_cast<int16_t>(s.currentX + dx);
    int16_t ny = static_cast<int16_t>(s.currentY + dy);
    clampX(nx);
    clampY(ny);
    if (b == 'P') {
        s.incrementalPenDown = true;
        return;
    }
    if (b == ' ') {
        s.incrementalPenDown = false;
    }
    s.currentX = nx;
    s.currentY = ny;
    if (s.incrementalPenDown)
        pushPoint(s, nx, ny);
}

// Alpha-mode printable / motion handling.
void alphaByte(State& s, uint8_t b)
{
    CellMetrics m = cellMetrics(s.charSize);
    switch (b) {
    case 0x0D: // CR — to the current left margin (0 or mid-screen)
        s.alphaX = s.alphaMargin;
        break;
    case 0x0A: // LF — down one line (two-column page wrap at the bottom)
        alphaAdvanceLine(s);
        break;
    case 0x08: // BS — left one cell, stopping at the left margin
        s.alphaX = static_cast<int16_t>(s.alphaX - m.advX);
        if (s.alphaX < s.alphaMargin)
            s.alphaX = s.alphaMargin;
        break;
    case 0x09: // HT — single advance
        s.alphaX = static_cast<int16_t>(s.alphaX + m.advX);
        clampX(s.alphaX);
        break;
    default:
        if (b >= 0x20 && b <= 0x7E) {
            pushCharacter(s, b, s.alphaX, s.alphaY);
            s.alphaX = static_cast<int16_t>(s.alphaX + m.advX);
            if (s.alphaX > tekGridWidth - 1) {
                s.alphaX = s.alphaMargin;
                alphaAdvanceLine(s);
            }
        }
        break;
    }
}

} // anonymous namespace
} // namespace Ubiquitty::Tek

namespace Ubiquitty {

using namespace Ubiquitty::Tek;

void tekInit(Ubiquitty::Terminal& t)
{
    State& s = t.tek;
    s.primitiveCount = 0;
    s.displayCount = 0;
    s.didDropPrimitive = false;
    s.didSignalPrimitiveDrop = false;
    s.plotMode = PlotMode::Alpha;
    s.parse = TekParse::Ground;
    s.csiLength = 0;
    s.lineStyle = LineStyle::Solid;
    s.charSize = CharSize::Large;
    s.isNextVectorDark = false;
    s.incrementalPenDown = false;
    s.GINActive = false;
    s.currentX = 0;
    s.currentY = 0;
    s.GINX = 0;
    s.GINY = 0;
    coordReset(s);
    alphaHome(s);
}

void tekEnter(Ubiquitty::Terminal& t)
{
    t.isTekModeActive = true;
    State& s = t.tek;
    s.plotMode = PlotMode::Alpha;
    s.parse = TekParse::Ground;
    s.bypass = false;
    // Fresh tube on entry so a re-entered session doesn't flash the previous frame before its first draw.
    s.primitiveCount = 0;
    s.displayCount = 0;
    s.didDropPrimitive = false;
    s.didSignalPrimitiveDrop = false;
    coordReset(s);
}

void tekExit(Ubiquitty::Terminal& t)
{
    t.isTekModeActive = false;
    t.tek.parse = TekParse::Ground;
    // Bright-pen override (DECSET 7400) is Tek-scoped: clear it on exit so it never leaks into the next session.
    t.tekBrightPenOverride = 0;
    t.parser.parseState = Ubiquitty::VT::Parser::State::Ground;
}

// In-mode ESC final dispatch.
static void tekEscapeDispatch(Ubiquitty::Terminal& t, uint8_t b)
{
    State& s = t.tek;
    s.parse = TekParse::Ground;
    switch (b) {
    case 0x03: // ETX — return to VT
    case 0x18: // Can — return to VT
        tekExit(t);
        return;
    case 0x0C: // FF — PAGE: commit the frame, clear the tube, home alpha.
        commitFrame(s);
        s.primitiveCount = 0;
        s.didDropPrimitive = false;
        s.didSignalPrimitiveDrop = false;
        s.plotMode = PlotMode::Alpha;
        coordReset(s);
        alphaHome(s);
        return;
    case 0x05: // ENQ — status / address report
    {
        // Through respond, like every other reply in Core: it is the one place a
        // capability is checked against the current origin, and a reply that
        // reaches the buffer another way is gated by nothing. The five payload
        // bytes are each 0x20 | (5 bits), so the report is printable up to the
        // terminating CR whatever the coordinates are.
        std::array<char, 6> buffer {};
        const int x = s.currentX;
        const int y = s.currentY;
        buffer[0] = static_cast<char>(0x20); // status: alpha, no options
        buffer[1] = static_cast<char>(0x20 | ((x >> 7) & 0x1F)); // Hi-X
        buffer[2] = static_cast<char>(0x20 | ((x >> 2) & 0x1F)); // Lo-X
        buffer[3] = static_cast<char>(0x20 | ((y >> 7) & 0x1F)); // Hi-Y
        buffer[4] = static_cast<char>(0x20 | ((y >> 2) & 0x1F)); // Lo-Y
        buffer[5] = '\r';
        Ubiquitty::respond(t, Ubiquitty::Capability::TektronixAddressRead, buffer);
        s.bypass = true; // echoed report bytes are bypassed until the next control
    }
        return;
    case 0x1A: // SUB — enter GIN (crosshair)
        s.GINActive = true;
        return;
    case '[': // CSI arriving inside Tek mode
        s.parse = TekParse::CSI;
        s.csiLength = 0;
        return;
    default:
        break;
    }
    // Character sizes: ESC 8/9/:/;
    if (b >= 0x38 && b <= 0x3B) {
        s.charSize = static_cast<CharSize>(b - 0x38);
        return;
    }
    // Line types: ESC `..g normal, ESC h..o defocused/bold (folded).
    if (b >= 0x60 && b <= 0x6F) {
        uint8_t index = static_cast<uint8_t>((b - 0x60) & 0x07);
        switch (index) {
        case 0:
            s.lineStyle = LineStyle::Solid;
            break;
        case 1:
            s.lineStyle = LineStyle::Dotted;
            break;
        case 2:
            s.lineStyle = LineStyle::DotDash;
            break;
        case 3:
            s.lineStyle = LineStyle::ShortDash;
            break;
        case 4:
            s.lineStyle = LineStyle::LongDash;
            break;
        default:
            s.lineStyle = LineStyle::Solid;
            break;
        }
        return;
    }
    // Unmapped escape final: ignore.
}

// In-mode CSI recognizer. Only DECRST 38 ("CSI ? 3 8 l") acts — it exits to
// VT mode, mirroring the DECSET 38 entry. Every other sequence is swallowed
// whole so its bytes don't draw as alpha garbage.
static void tekCSIByte(Ubiquitty::Terminal& t, uint8_t b)
{
    State& s = t.tek;
    if (b == 0x18 || b == 0x1A) { // CAN / SUB abort the sequence
        s.parse = TekParse::Ground;
        return;
    }
    if (b == 0x1B) { // ESC restarts the escape
        s.parse = TekParse::Escape;
        return;
    }
    if (b >= 0x20 && b <= 0x3F) { // parameter / intermediate bytes
        if (s.csiLength < sizeof(s.csiBytes))
            s.csiBytes[s.csiLength] = static_cast<char>(b);
        if (s.csiLength < UINT8_MAX)
            s.csiLength++;
        return;
    }
    if (b >= 0x40 && b <= 0x7E) { // final byte
        s.parse = TekParse::Ground;
        if (b == 'l' && s.csiLength == 3 && s.csiBytes[0] == '?' && s.csiBytes[1] == '3' && s.csiBytes[2] == '8')
            tekExit(t);
        return;
    }
    // Other C0 bytes: swallowed; the sequence continues.
}

static void tekByteInner(Ubiquitty::Terminal& t, uint8_t b)
{
    State& s = t.tek;

    // Bypass: after a GIN/ENQ report the host often echoes it back; swallow those printable bytes until the
    // next control byte clears bypass, so the echoed coordinates don't draw as alpha garbage.
    if (s.bypass) {
        if (b < 0x20 || b == 0x7F)
            s.bypass = false; // control byte ends bypass; handle it normally
        else
            return; // swallow the echoed report byte
    }

    if (s.parse == TekParse::Escape) {
        tekEscapeDispatch(t, b);
        return;
    }

    if (s.parse == TekParse::CSI) {
        tekCSIByte(t, b);
        return;
    }

    // C0 control bytes are mode/plot switches everywhere.
    switch (b) {
    case 0x1B: // ESC
        s.parse = TekParse::Escape;
        return;
    case 0x1D: // GS — graph (vector) mode; next coord is a dark move
        s.plotMode = PlotMode::Graph;
        s.isNextVectorDark = true;
        coordReset(s);
        return;
    case 0x1C: // FS — point-plot mode
        s.plotMode = PlotMode::Point;
        coordReset(s);
        return;
    case 0x1E: // RS — incremental-plot mode
        s.plotMode = PlotMode::Incremental;
        s.incrementalPenDown = false;
        coordReset(s);
        return;
    case 0x1F: // US — alpha mode; the alpha cursor follows the graphic position
        s.plotMode = PlotMode::Alpha;
        s.alphaX = s.currentX; // plot-then-label resumes where plotting left off
        s.alphaY = s.currentY; // (FS point mode doesn't track the alpha cursor)
        coordReset(s);
        return;
    case 0x07: // BEL
        // BEL right after GS is the pen-down marker in vector mode: clear the dark-vector flag so the
        // next coordinate draws instead of ringing. Outside vector mode it is an ordinary bell.
        if (s.plotMode == PlotMode::Graph)
            s.isNextVectorDark = false;
        else
            Ubiquitty::ringBell(t);
        return;
    default:
        break;
    }

    if (s.plotMode == PlotMode::Graph || s.plotMode == PlotMode::Point) {
        if (b >= 0x20) {
            coordByte(s, b);
            return;
        }
        // Other C0 (CR/LF) in graph/point: treated as alpha motion.
        alphaByte(s, b);
        return;
    }
    if (s.plotMode == PlotMode::Incremental) {
        if (b >= 0x20) {
            incrByte(s, b);
            return;
        }
        alphaByte(s, b);
        return;
    }
    // Alpha mode.
    alphaByte(s, b);
}

void tekByte(Ubiquitty::Terminal& t, uint8_t b)
{
    tekByteInner(t, b);
    // Surface a storage-tube overflow once per frame instead of dropping primitives silently.
    State& s = t.tek;
    if (s.didDropPrimitive && !s.didSignalPrimitiveDrop) {
        s.didSignalPrimitiveDrop = true;
        Diagnostics::diagLog(t, "tek-list-full", static_cast<uint32_t>(tekMaxPrimitives));
    }
}

bool tekReportGraphicInputKey(Ubiquitty::Terminal& t, char key, float pointX, float pointY, float viewWidth, float viewHeight)
{
    if (!t.isTekModeActive || !tekGINActive(t))
        return false;
    // The view rect is in points, one device unit per point.
    const Tek::Projection projection
        = Tek::makeBeamProjection(tekScreenWidth(t), tekScreenHeight(t), 0.0f, 0.0f, viewWidth, viewHeight, 1.0f);
    int tekX = 0;
    int tekY = 0;
    Tek::unproject(projection, pointX, pointY, &tekX, &tekY);
    tekGINKey(t, key, tekX, tekY);
    return true;
}

void tekGINKey(Ubiquitty::Terminal& t, char key, int x, int y)
{
    State& s = t.tek;
    if (x < 0)
        x = 0;
    if (x > tekGridWidth - 1)
        x = tekGridWidth - 1;
    if (y < 0)
        y = 0;
    if (y > tekGridHeight - 1)
        y = tekGridHeight - 1;

    // Through respond, for the capability check the direct queue skipped. The
    // counted form rather than the string one: buffer[0] is the byte of the key
    // the user pressed, and a NUL keystroke would end a C string one byte in and
    // send a report of length zero.
    std::array<char, 6> buffer {};
    buffer[0] = key;
    buffer[1] = static_cast<char>(0x20 | ((x >> 7) & 0x1F)); // Hi-X
    buffer[2] = static_cast<char>(0x20 | ((x >> 2) & 0x1F)); // Lo-X
    buffer[3] = static_cast<char>(0x20 | ((y >> 7) & 0x1F)); // Hi-Y
    buffer[4] = static_cast<char>(0x20 | ((y >> 2) & 0x1F)); // Lo-Y
    buffer[5] = '\r';

    Ubiquitty::respond(t, Ubiquitty::Capability::TektronixAddressRead, buffer);

    s.GINActive = false;
    s.bypass = true; // echoed report bytes are bypassed until the next control
}

void tekUpdateGINPosition(Ubiquitty::Terminal& t, int x, int y)
{
    State& s = t.tek;
    if (!s.GINActive)
        return; // crosshair only tracks while GIN is armed
    if (x < 0)
        x = 0;
    if (x > tekGridWidth - 1)
        x = tekGridWidth - 1;
    if (y < 0)
        y = 0;
    if (y > tekGridHeight - 1)
        y = tekGridHeight - 1;
    s.GINX = static_cast<int16_t>(x);
    s.GINY = static_cast<int16_t>(y);
}

// ------------------------- accessors -------------------------

int tekPrimitiveCount(const Ubiquitty::Terminal& t) { return t.tek.primitiveCount; }

const Ubiquitty::Tek::Primitive* tekPrimitive(const Ubiquitty::Terminal& t, int index)
{
    if (index < 0 || index >= t.tek.primitiveCount)
        return nullptr;
    return &t.tek.primitives[index];
}

const Ubiquitty::Tek::Primitive* tekPrimitives(const Ubiquitty::Terminal& t)
{
    return t.tek.primitives; // base; first tekPrimitiveCount entries are live
}

int tekDisplayCount(const Ubiquitty::Terminal& t) { return t.tek.displayCount; }

const Ubiquitty::Tek::Primitive* tekDisplayPrimitives(const Ubiquitty::Terminal& t) { return t.tek.displayPrimitives; }

Ubiquitty::Tek::PrimitiveSpan tekVisiblePrimitives(const Ubiquitty::Terminal& t)
{
    const int displayed = t.tek.displayCount;
    if (displayed > 0)
        return { t.tek.displayPrimitives, static_cast<std::size_t>(displayed) };
    return { t.tek.primitives, static_cast<std::size_t>(t.tek.primitiveCount) };
}

void tekCommitFrame(Ubiquitty::Terminal& t) { commitFrame(t.tek); }

int tekAlphaCursorX(const Ubiquitty::Terminal& t) { return t.tek.alphaX; }
int tekAlphaCursorY(const Ubiquitty::Terminal& t) { return t.tek.alphaY; }

Tek::LineStyle tekLineStyle(const Ubiquitty::Terminal& t) { return t.tek.lineStyle; }
Tek::CharSize tekCharacterSize(const Ubiquitty::Terminal& t) { return t.tek.charSize; }

bool tekGINActive(const Ubiquitty::Terminal& t) { return t.tek.GINActive; }
int tekGINX(const Ubiquitty::Terminal& t) { return t.tek.GINX; }
int tekGINY(const Ubiquitty::Terminal& t) { return t.tek.GINY; }

int tekScreenWidth(const Ubiquitty::Terminal&) { return Ubiquitty::Tek::tekGridWidth; }
int tekScreenHeight(const Ubiquitty::Terminal&) { return Ubiquitty::Tek::tekGridHeight; }

} // namespace Ubiquitty
