// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Mouse/MouseEvent.h"
#include "Core/Terminal.h"
#include "Harness.h"

// test_mouse_event_type.cpp — Mouse::EventType wire behavior.
//
// 1. static_assert pins each enumerator's dispatch code — the encoder's
//    branch values, so a renumbering fails the build, not the wire.
// 2. A behavior test feeds each enumerator through the real public
//    Ubiquitty::Mouse::encodeReport() encoder and asserts the wire bytes
//    differ per event type. A regression that collapsed Release and
//    Motion to the same wire byte fails here.

#include "Core/Mouse/Mouse.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>

static_assert(static_cast<int>(Ubiquitty::Mouse::EventType::Press) == 0, "Press == 0");
static_assert(static_cast<int>(Ubiquitty::Mouse::EventType::Release) == 1, "Release == 1");
static_assert(static_cast<int>(Ubiquitty::Mouse::EventType::Motion) == 2, "Motion == 2");

/// Drive each event type through the real public Ubiquitty::Mouse::encodeReport()
/// encoder and assert the wire bytes differ per type. The encoder branches on
/// the event type to pick the X10 button code (Release → 3; Press → button;
/// Motion → button + 32) and the SGR trailer ('m' for release, 'M' for the
/// others), so distinct inputs must produce distinct outputs.
TEST(behaviourThroughEncoder)
{
    Ubiquitty::Terminal t {};
    // Any-event tracking (DECSET 1003), because this is a test about framing:
    // the encoder refuses motion under a mode that does not track it, and a
    // zero-initialized Terminal tracks nothing.
    t.modes.mouseAnyEvent = true;

    // X10 framing: 6-byte report; byte[3] = code + 32. Press, release,
    // motion must each yield a distinct byte[3].
    t.modes.mouseSGR = false;
    char x10[3][8];
    int x10n[3];
    const Ubiquitty::Mouse::EventType types[3] = {
        Ubiquitty::Mouse::EventType::Press,
        Ubiquitty::Mouse::EventType::Release,
        Ubiquitty::Mouse::EventType::Motion,
    };
    for (int i = 0; i < 3; ++i) {
        std::memset(x10[i], 0, sizeof(x10[i]));
        x10n[i] = Ubiquitty::Mouse::encodeReport(&t, types[i],
            /*button*/ 0, /*row*/ 1, /*column*/ 1, /*mod*/ 0, x10[i], static_cast<int>(sizeof(x10[i])));
        CHECK(x10n[i] == 6);
        CHECK(x10[i][0] == '\033');
        CHECK(x10[i][1] == '[');
        CHECK(x10[i][2] == 'M');
    }
    // The button-code byte (index 3) must differ per event type.
    CHECK(x10[0][3] != x10[1][3]); // press vs release
    CHECK(x10[1][3] != x10[2][3]); // release vs motion
    CHECK(x10[0][3] != x10[2][3]); // press vs motion

    // SGR framing: trailer is 'm' for release, 'M' otherwise. So the
    // three encodings still must be pairwise distinct (release has a
    // different terminator; press vs motion differ in the numeric
    // code because Motion adds 32).
    t.modes.mouseSGR = true;
    char sgr[3][32];
    int sgrn[3];
    for (int i = 0; i < 3; ++i) {
        std::memset(sgr[i], 0, sizeof(sgr[i]));
        sgrn[i] = Ubiquitty::Mouse::encodeReport(&t, types[i],
            /*button*/ 0, /*row*/ 1, /*column*/ 1, /*mod*/ 0, sgr[i], static_cast<int>(sizeof(sgr[i])));
        CHECK(sgrn[i] > 0);
    }
    // Distinct encodings end-to-end.
    CHECK(!(sgrn[0] == sgrn[1] && std::memcmp(sgr[0], sgr[1], sgrn[0]) == 0));
    CHECK(!(sgrn[1] == sgrn[2] && std::memcmp(sgr[1], sgr[2], sgrn[1]) == 0));
    CHECK(!(sgrn[0] == sgrn[2] && std::memcmp(sgr[0], sgr[2], sgrn[0]) == 0));
    // Release SGR trailer is 'm'; the other two are 'M'.
    CHECK(sgr[1][sgrn[1] - 1] == 'm');
    CHECK(sgr[0][sgrn[0] - 1] == 'M');
    CHECK(sgr[2][sgrn[2] - 1] == 'M');
}

/// SGR-pixel (?1016): a STATIONARY pointer's press (M) and release (m) must encode
/// the same px;py body — the frontend now routes both through one device-pixel
/// conversion (devicePixelAtWindowPoint), so for equal input pixels the encoder must
/// emit an identical coordinate body, differing only in the M/m trailer. Guards the
/// regression where release fell back to the cell-quantized column*cellPixelWidth+1
/// pixel and disagreed with the sub-cell motion/press pixel.
TEST(sgrPixelPressReleaseSameCoordinate)
{
    Ubiquitty::Terminal t {};
    // A tracking mode so the encoder emits at all; the two encodings below
    // shape the bytes.
    t.modes.mouseButtonTracking = true;
    t.modes.mouseSGR = true;
    t.modes.mouseSGRPixels = true; // ?1016

    const int pixelX = 918, pixelY = 968; // the wire-evidence motion pixel from the report
    char press[32], release[32];
    std::memset(press, 0, sizeof(press));
    std::memset(release, 0, sizeof(release));
    const int pressN = Ubiquitty::Mouse::encodeReport(&t, Ubiquitty::Mouse::EventType::Press,
        /*button*/ 0, /*row*/ 30, /*column*/ 40, /*mod*/ 0, press, pixelX, pixelY);
    const int releaseN = Ubiquitty::Mouse::encodeReport(&t, Ubiquitty::Mouse::EventType::Release,
        /*button*/ 0, /*row*/ 30, /*column*/ 40, /*mod*/ 0, release, pixelX, pixelY);
    CHECK(pressN > 0 && releaseN > 0);
    // Trailer: press 'M', release 'm'.
    CHECK(press[pressN - 1] == 'M');
    CHECK(release[releaseN - 1] == 'm');
    // Same length + identical body up to the trailer: <0;px;py for both.
    CHECK(pressN == releaseN);
    CHECK(std::memcmp(press, release, static_cast<size_t>(pressN - 1)) == 0);
    // The px;py are the passed device pixel, not a cell-quantized fallback.
    char expected[32];
    const int expectedN = std::snprintf(expected, sizeof(expected), "\033[<0;%d;%dM", pixelX, pixelY);
    CHECK(expectedN == pressN);
    CHECK(std::memcmp(press, expected, static_cast<size_t>(pressN)) == 0);
}

UBIQUITTY_TEST_MAIN()
