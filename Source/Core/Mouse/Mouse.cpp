// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Mouse-report encoder. Ubiquitty::encodeMouseReport in
// VT.cpp is a one-line forwarder to Ubiquitty::Mouse::encodeReport;
// the wire-format body lives here so VT.cpp doesn't carry the
// encoding tables.

#include "Core/Mouse/Mouse.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/Terminal.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <span>

// Internal alias: refer to the strongly-typed enum class as `Evt`
// inside this TU. The global `Ubiquitty::Mouse::EventType` (an int typedef
// from TerminalOperations.h) survives unchanged so the public header signature
// matches.
using Evt = Ubiquitty::Mouse::EventType;

namespace Ubiquitty::Mouse {
bool tracksPointer(const Ubiquitty::Terminal* t)
{
    if (!t)
        return false;
    return t->modes.mouseX10 || t->modes.mouseButtonTracking || t->modes.mouseHilite || t->modes.mouseCellMotion
        || t->modes.mouseAnyEvent;
}

int encodeReport(const Ubiquitty::Terminal* t, Ubiquitty::Mouse::EventType type, int button, int row, int column, int modifiers, std::span<char> output, int pixelX, int pixelY)
{
    char* const outputBuffer = output.data();
    const int maxLength = static_cast<int>(output.size());
    int code;
    int n;
    char tmp[32];

    if (!t || !outputBuffer || maxLength <= 0)
        return 0;
    if (row < 0 || column < 0)
        return 0;
    // No tracking mode at all: nothing reports. A frontend that relied on the
    // encoder's refusal would otherwise send the legacy bytes into a modeless
    // application's input on every click, so the encoder enforces this rather
    // than each caller. It holds every mode flag, so it holds this rule too;
    // pointer lock stays exempt for the reason the motion gate states.
    if (!tracksPointer(t) && !t->modes.mouseRelativePointerLock)
        return 0;
    if (button < 0)
        button = Ubiquitty::Mouse::Button::Left;

    Evt evt = static_cast<Evt>(type);

    // X10 compatibility (DECSET 9) is the active tracking mode only when
    // no richer mode (1000/1002/1003) is also on — the richer mode wins.
    // Under true X10 the report is press-only with a bare button
    // Cb: no release, no motion, no modifier bits.
    const bool isX10Only = t->modes.mouseX10 && !t->modes.mouseButtonTracking && !t->modes.mouseCellMotion && !t->modes.mouseAnyEvent;
    if (isX10Only) {
        if (evt != Evt::Press)
            return 0;
        modifiers = 0;
    }

    // Which motion is reportable is the tracking mode's decision: 1000 tracks
    // button transitions only, 1002 adds motion while a button is held, 1003
    // adds free motion. A frontend checking this before calling holds a copy of
    // the rule, and a frontend written without the check leaks motion into a
    // 1000-only session. The encoder holds every mode flag, so it holds the rule.
    //
    // Relative pointer-lock (7403) is exempt: the mode exists to deliver motion
    // deltas to a mouselook client, so requiring 1003 on top would silence the
    // one thing it is for. macOS reports locked motion ahead of any tracking
    // check (UBTerminalView's mouseMoved: takes the lock path first), and macOS
    // is the reference for what a frontend sends.
    if (evt == Evt::Motion && !t->modes.mouseRelativePointerLock) {
        const bool hasNoButtonHeld = (button == Ubiquitty::Mouse::Button::None);
        const bool shouldReport = hasNoButtonHeld ? t->modes.mouseAnyEvent
                                                  : (t->modes.mouseCellMotion || t->modes.mouseAnyEvent);
        if (!shouldReport)
            return 0;
    }

    // SGR-pixels (DECSET 1016) — same CSI < … M/m envelope as SGR but
    // the coordinates are device pixels rather than 1-based cells. When
    // the frontend supplies a true device-pixel position (pixelX and
    // pixelY both >= 0) those are reported verbatim, giving sub-cell
    // resolution. Otherwise multiply the zero-based cell coords by the
    // published cell pixel size and add 1 (the report names the top-left
    // pixel of the cell, 1-based); when the frontend hasn't published a
    // cell size yet, fall back to a 1:1 mapping so the report still
    // distinguishes cells. Checked before plain SGR so 1016 wins when
    // both are on.
    if (t->modes.mouseSGRPixels) {
        int px, py;
        if (t->modes.mouseRelativePointerLock) {
            // Relative pointer-lock (DECSET 7403) rides the ?1016 frame but the
            // px;py are not absolute: they carry a CENTER-BIASED signed motion
            // delta. The frontend passes the raw per-report delta (event.deltaX/Y,
            // signed, ZERO on a button event) as pixelX/pixelY; bias by a FIXED
            // C = 0x8000 so the SGR decimal body stays positive (a literal minus
            // breaks conforming SGR parsers) and a fast flick can't underflow a
            // small text area. The client recovers the delta as (px - 0x8000).
            // Clamp >= 1 as a floor (unreachable per-report: one delta can't be < -0x7FFF).
            constexpr int relativePointerLockBias = 0x8000;
            px = relativePointerLockBias + pixelX;
            py = relativePointerLockBias + pixelY;
            if (px < 1)
                px = 1;
            if (py < 1)
                py = 1;
        } else {
            int cellWidth = t->cellPixelWidth ? t->cellPixelWidth : 1;
            int cellHeight = t->cellPixelHeight ? t->cellPixelHeight : 1;
            px = (pixelX >= 0) ? pixelX : column * cellWidth + 1;
            py = (pixelY >= 0) ? pixelY : row * cellHeight + 1;
        }
        code = button + modifiers;
        if (evt == Evt::Motion)
            code += 32;
        n = std::snprintf(tmp, sizeof(tmp), "\033[<%d;%d;%d%c", code, px, py, (evt == Evt::Release) ? 'm' : 'M');
        if (n <= 0 || n > maxLength)
            return 0;
        std::memcpy(outputBuffer, tmp, n);
        return n;
    }

    if (t->modes.mouseSGR) {
        code = button + modifiers;
        if (evt == Evt::Motion)
            code += 32;
        n = std::snprintf(tmp, sizeof(tmp), "\033[<%d;%d;%d%c", code, column + 1, row + 1, (evt == Evt::Release) ? 'm' : 'M');
        if (n <= 0 || n > maxLength)
            return 0;
        std::memcpy(outputBuffer, tmp, n);
        return n;
    }

    // The legacy/extended low-byte forms share the same Cb code: button
    // (or 3 on release) + motion bias + modifiers.
    code = (evt == Evt::Release) ? 3 : button;
    if (evt == Evt::Motion)
        code += 32;
    code += modifiers;
    if (code < 0)
        return 0;

    // URXVT (DECSET 1015) — decimal CSI Cb+32 ; Cx ; Cy M. No release
    // distinction (always 'M'); Cb carries +32 like the legacy byte.
    if (t->modes.mouseURXVT) {
        n = std::snprintf(tmp, sizeof(tmp), "\033[%d;%d;%dM", code + 32, column + 1, row + 1);
        if (n <= 0 || n > maxLength)
            return 0;
        std::memcpy(outputBuffer, tmp, n);
        return n;
    }

    // UTF-8 (DECSET 1005) — like the legacy CSI M form, but each of the
    // three bytes (Cb, Cx, Cy) is encoded as UTF-8 when it exceeds 0x7F,
    // lifting the 223-column ceiling to 2015. Values that still fit in
    // one byte stay single-byte, so a small click is byte-identical to
    // the legacy form.
    if (t->modes.mouseUTF8Coordinates) {
        // 1005 UTF-8 mode: each coordinate is UTF-8-encoded, and the 2-byte form
        // below tops out at U+07FF — a value past that would emit a 3-byte lead
        // with only one continuation byte (malformed UTF-8 on the wire). Clamp to
        // the max representable (v = coord + 33 must stay <= 0x7FF) — mode 1005's
        // ~2015-column ceiling — rather than corrupt the stream.
        constexpr int maxCoordinate = 0x7FF - 33;
        const int clampedColumn = column > maxCoordinate ? maxCoordinate : column;
        const int clampedRow = row > maxCoordinate ? maxCoordinate : row;
        unsigned vals[3] = {
            static_cast<unsigned>(code) + 32,
            static_cast<unsigned>(clampedColumn) + 33,
            static_cast<unsigned>(clampedRow) + 33,
        };
        // Assembled here and copied once, like the other forms. Written
        // straight into the caller's buffer, a refusal for want of room left
        // the lead bytes behind it: the return said nothing was written and
        // the buffer disagreed. Nine bytes is the longest this form reaches —
        // three of lead and two per coordinate.
        std::array<char, 9> utf8 {};
        int pos = 0;
        utf8[pos++] = '\033';
        utf8[pos++] = '[';
        utf8[pos++] = 'M';
        for (unsigned int v : vals) {
            if (v < 0x80) {
                utf8[pos++] = static_cast<char>(v);
            } else {
                utf8[pos++] = static_cast<char>(0xC0 | (v >> 6));
                utf8[pos++] = static_cast<char>(0x80 | (v & 0x3F));
            }
        }
        if (pos > maxLength)
            return 0;
        std::memcpy(outputBuffer, utf8.data(), static_cast<std::size_t>(pos));
        return pos;
    }

    if (maxLength < 6)
        return 0;
    if (code > 223 || column > 222 || row > 222)
        return 0;

    outputBuffer[0] = '\033';
    outputBuffer[1] = '[';
    outputBuffer[2] = 'M';
    outputBuffer[3] = static_cast<char>(code + 32);
    outputBuffer[4] = static_cast<char>(column + 33);
    outputBuffer[5] = static_cast<char>(row + 33);
    return 6;
}

} // namespace Ubiquitty::Mouse
