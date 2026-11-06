// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// DEC locator reporting — DECELR / DECSLE / DECRQLP / DECEFR.
//
// A VT340-era pointing-device protocol: the host arms the locator, optionally
// filters to a rectangle, and the terminal reports button transitions and
// positions. Distinct from xterm mouse tracking, which Core/Mouse/Mouse.cpp handles.
//
// The DECELR/DECSLE/DECRQLP handlers stay in the sequence layer and call in here.

#include "Core/Mouse/Locator.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/Terminal.h"
#include <span>

namespace Ubiquitty::Mouse::Locator {

int encodeHiliteEndReport(int startColumn, int startRow, int endColumn, int endRow, int mouseColumn, int mouseRow, HiliteEndForm form, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int maxLength = static_cast<int>(output.size());
    if (!outputBuffer || maxLength <= 0)
        return 0;
    // Classic legacy byte encoding: each coordinate is value+0x20, clamped into
    // a single byte's reportable range so a wild coordinate can't write garbage.
    auto encode = [](int v) -> unsigned char {
        if (v < 1)
            v = 1;
        if (v > 223)
            v = 223; // 223+0x20 == 0xFF, the legacy ceiling
        return static_cast<unsigned char>(v + 0x20);
    };
    char buffer[16];
    int n = 0;
    buffer[n++] = '\033';
    buffer[n++] = '[';
    if (form == HiliteEndForm::RegionAndPointer) {
        buffer[n++] = 'T';
        buffer[n++] = static_cast<char>(encode(startColumn));
        buffer[n++] = static_cast<char>(encode(startRow));
        buffer[n++] = static_cast<char>(encode(endColumn));
        buffer[n++] = static_cast<char>(encode(endRow));
        buffer[n++] = static_cast<char>(encode(mouseColumn));
        buffer[n++] = static_cast<char>(encode(mouseRow));
    } else {
        buffer[n++] = 't';
        buffer[n++] = static_cast<char>(encode(endColumn));
        buffer[n++] = static_cast<char>(encode(endRow));
    }
    if (n > maxLength)
        return 0;
    std::memcpy(outputBuffer, buffer, static_cast<size_t>(n));
    return n;
}

// DECLRP wire-format encoder, shared by csiH_decrqlp and the public
// encodeLocatorReport. Emits CSI Pe ; Pb ; Pr ; Pc ; Pp & w (no NUL); Pe = 0
// (unavailable) is the short form CSI 0 & w, and an undefined position (row or
// column 0) omits Pr, Pc and Pp, as DEC STD 070 DECLRP has it. Returns bytes
// written, 0 on overflow / bad input.
int encodeReport(int event, int buttons, int row, int column, int page, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int maxLength = static_cast<int>(output.size());
    if (!outputBuffer || maxLength <= 0)
        return 0;
    char tmp[64];
    int n;
    if (event == 0)
        n = std::snprintf(tmp, sizeof(tmp), "\033[0&w");
    else if (row <= 0 || column <= 0)
        n = std::snprintf(tmp, sizeof(tmp), "\033[%d;%d&w", event, buttons);
    else
        n = std::snprintf(tmp, sizeof(tmp), "\033[%d;%d;%d;%d;%d&w", event, buttons, row, column, page);
    if (n <= 0 || n > maxLength)
        return 0;
    std::memcpy(outputBuffer, tmp, n);
    return n;
}

bool isActive(const Ubiquitty::Terminal* t) { return t && t->locatorEnabled; }

// Translate an xterm button code (0 = left, 1 = middle, 2 = right, 3 = M4)
// to the DECLRP Pb button-state bit (bit & 1 = right, & 2 = middle, & 4 = left,
// & 8 = M4). The frontend ORs these for every button currently held to build
// the Pb field. Returns 0 for "no button" codes (e.g. 3 used as the
// any-event hover sentinel does not map here — pass the real held set).
int buttonBit(int xtermButton)
{
    switch (xtermButton) {
    case 0:
        return 0x04; // left
    case 1:
        return 0x02; // middle
    case 2:
        return 0x01; // right
    case 3:
        return 0x08; // M4
    default:
        return 0;
    }
}

// DECLRP Pe event code for a press/release of an xterm button:
// left 2/3, middle 4/5, right 6/7, M4 8/9 (down/up). 0 = no mappable event.
static int eventCode(Ubiquitty::Mouse::EventType type, int xtermButton)
{
    bool down;
    if (type == Ubiquitty::Mouse::EventType::Press)
        down = true;
    else if (type == Ubiquitty::Mouse::EventType::Release)
        down = false;
    else
        return 0; // motion is not a button transition — never a DECLRP here
    int base;
    switch (xtermButton) {
    case 0:
        base = 2;
        break; // left
    case 1:
        base = 4;
        break; // middle
    case 2:
        base = 6;
        break; // right
    case 3:
        base = 8;
        break; // M4
    default:
        return 0;
    }
    return down ? base : base + 1;
}

// Is (row1,col1) inside the armed filter rectangle? Bounds arrive already
// resolved to real coordinates (csiH_decefr substitutes the live locator
// position for an omitted one), so nothing is defaulted here; only the ordering
// is normalised, since DECEFR does not require top <= bottom / left <= right.
static bool insideFilter(const Ubiquitty::Terminal& t, int row1, int col1)
{
    int top = t.locatorFilterTop;
    int left = t.locatorFilterLeft;
    int bottom = t.locatorFilterBottom;
    int right = t.locatorFilterRight;
    if (top > bottom) {
        int tmp = top;
        top = bottom;
        bottom = tmp;
    }
    if (left > right) {
        int tmp = left;
        left = right;
        right = tmp;
    }
    return row1 >= top && row1 <= bottom && col1 >= left && col1 <= right;
}

int reportEvent(Ubiquitty::Terminal* t, Ubiquitty::Mouse::EventType type, int button, int row, int column, int buttonState, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int maxLength = static_cast<int>(output.size());
    if (!t || !t->locatorEnabled || !outputBuffer || maxLength <= 0)
        return 0;
    if (row < 1 || column < 1)
        return 0;

    // Recorded before any event gating: DECRQLP answers with the locator's
    // CURRENT position and DECEFR resolves an omitted bound against it, so both
    // must stay live across the events that emit nothing — a move with no armed
    // rectangle, a transition DECSLE did not select.
    t->locatorRow = static_cast<uint16_t>(row > 0xFFFF ? 0xFFFF : row);
    t->locatorColumn = static_cast<uint16_t>(column > 0xFFFF ? 0xFFFF : column);
    t->locatorButtons = static_cast<uint8_t>(buttonState & 0x0F);

    // A motion (or any) event with an armed filter rectangle fires once, with
    // Pe = 10 ("outside filter rectangle"), the first time the locator leaves
    // the rect; the rect then disarms. This is the only motion-driven report.
    if (type == Ubiquitty::Mouse::EventType::Motion) {
        if (t->locatorFilterActive && !insideFilter(*t, row, column)) {
            t->locatorFilterActive = false;
            int n = Ubiquitty::Mouse::Locator::encodeReport(10, buttonState, row, column, 1, output);
            if (n > 0 && t->locatorOneShot) {
                t->locatorEnabled = false;
                t->locatorOneShot = false;
            }
            return n;
        }
        return 0;
    }

    // Button transition: honour the DECSLE down/up selection.
    bool isDown = (type == Ubiquitty::Mouse::EventType::Press);
    if (isDown && !t->locatorReportDown)
        return 0;
    if (!isDown && !t->locatorReportUp)
        return 0;
    int pe = eventCode(type, button);
    if (pe == 0)
        return 0;
    int n = Ubiquitty::Mouse::Locator::encodeReport(pe, buttonState, row, column, 1, output);
    if (n > 0 && t->locatorOneShot) {
        t->locatorEnabled = false;
        t->locatorOneShot = false;
    }
    return n;
}

} // namespace Ubiquitty::Mouse::Locator
