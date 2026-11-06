// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/OverlayGeometry.h"
#include "Harness.h"

using Ubiquitty::Modes::ScreenBuffer;
using Ubiquitty::Rendering::selectionHistoryRows;

TEST(the_primary_screen_decodes_against_its_real_scrollback)
{
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Primary, 0), 0);
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Primary, 1), 1);
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Primary, 10000), 10000);
}

TEST(the_alternate_screen_decodes_against_no_history)
{
    // The alternate buffer has no scrollback of its own, and the gesture encodes
    // its rows with history forced to 0. Decoding with the PRIMARY's surviving
    // count would shift every highlight off the pointer by that many rows.
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Alternate, 0), 0);
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Alternate, 1), 0);
    CHECK_EQ(selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Alternate, 10000), 0);
}

// encode: absolute = visible + history - viewOffset
// decode: visible  = absolute - history + viewOffset
//
// Selection encoding and overlay decoding each read history from their
// own snapshot — so there is no round trip in Core to call, and a case
// that writes both halves here proves its own algebra. What can fail is the two
// halves disagreeing about which buffer they are on, and that is what these
// pin: the same buffer lands on the row the gesture meant, and a switch between
// gesture and paint misses by exactly the scrollback.
static int encodeAbsolute(Ubiquitty::Modes::ScreenBuffer buffer, int scrollback, int visible, int viewOffset)
{
    return visible + selectionHistoryRows(buffer, scrollback) - viewOffset;
}

static int decodeVisible(Ubiquitty::Modes::ScreenBuffer buffer, int scrollback, int absolute, int viewOffset)
{
    return absolute - selectionHistoryRows(buffer, scrollback) + viewOffset;
}

TEST(the_round_trip_lands_on_the_row_the_gesture_meant)
{
    for (int scrollback : { 0, 3, 500 }) {
        for (int viewOffset : { 0, 2, 40 }) {
            for (int visible : { 0, 7, 23 }) {
                for (Ubiquitty::Modes::ScreenBuffer buffer : { Ubiquitty::Modes::ScreenBuffer::Primary, Ubiquitty::Modes::ScreenBuffer::Alternate }) {
                    const int absolute = encodeAbsolute(buffer, scrollback, visible, viewOffset);
                    CHECK_EQ(decodeVisible(buffer, scrollback, absolute, viewOffset), visible);
                }
            }
        }
    }
}

TEST(a_screen_switch_between_gesture_and_paint_misses_by_the_scrollback)
{
    // The gesture happens on one buffer and the paint reads whichever is live
    // when the frame is built. If the screen switched in between, the two
    // derive different histories from the same scrollback — the alternate has
    // none — and the highlight lands that many rows away. Zero scrollback is
    // the case where the two agree ANYWAY, so it is asserted separately rather
    // than folded in: without it a decode that ignored the buffer entirely
    // would pass every row of the table.
    for (int scrollback : { 1, 3, 500 }) {
        const int visible = 7, viewOffset = 0;
        const int fromPrimary = encodeAbsolute(Ubiquitty::Modes::ScreenBuffer::Primary, scrollback, visible, viewOffset);
        const int paintedOnAlternate = decodeVisible(Ubiquitty::Modes::ScreenBuffer::Alternate, scrollback, fromPrimary, viewOffset);
        CHECK_EQ(paintedOnAlternate, visible + scrollback);
        CHECK(paintedOnAlternate != visible);

        const int fromAlternate = encodeAbsolute(Ubiquitty::Modes::ScreenBuffer::Alternate, scrollback, visible, viewOffset);
        const int paintedOnPrimary = decodeVisible(Ubiquitty::Modes::ScreenBuffer::Primary, scrollback, fromAlternate, viewOffset);
        CHECK_EQ(paintedOnPrimary, visible - scrollback);
    }

    // With no scrollback the two buffers agree, so a switch costs nothing and
    // the round trip still lands.
    const int absolute = encodeAbsolute(Ubiquitty::Modes::ScreenBuffer::Primary, 0, 7, 0);
    CHECK_EQ(decodeVisible(Ubiquitty::Modes::ScreenBuffer::Alternate, 0, absolute, 0), 7);
}

TEST(using_the_raw_count_on_the_alternate_screen_is_off_by_the_scrollback)
{
    // Pins the defect the rule exists to prevent, so a future change that
    // reintroduces the raw count fails here rather than in a screenshot.
    const int scrollback = 500, viewOffset = 0, visible = 4;
    const int correct = selectionHistoryRows(Ubiquitty::Modes::ScreenBuffer::Alternate, scrollback);
    const int absolute = visible + correct - viewOffset;
    const int decodedWithRawCount = absolute - scrollback + viewOffset;
    CHECK_EQ(decodedWithRawCount, visible - scrollback);
    CHECK(decodedWithRawCount != visible);
}

UBIQUITTY_TEST_MAIN()
