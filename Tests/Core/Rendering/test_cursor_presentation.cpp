// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/CursorPresentation.h"

#include <array>
#include <limits>

using Ubiquitty::Rendering::CursorPresentation;

namespace {

CursorPresentation::Frame frameAt(int column, int row)
{
    CursorPresentation::Frame frame;
    frame.cursor.column = column;
    frame.cursor.row = row;
    frame.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    frame.cursor.rgb = { 0.1f, 0.2f, 0.3f, 0.6f };
    frame.cursor.mixA = 1;
    frame.geometry.rows = 60;
    frame.geometry.columns = 140;
    frame.geometry.cellWidth = 8;
    frame.geometry.cellHeight = 16;
    frame.geometry.viewportWidth = 1120;
    frame.geometry.viewportHeight = 960;
    frame.isEligible = true;
    frame.isProgramVisible = true;
    return frame;
}

} // namespace

TEST(cursorMovementExcursionsAndInputBypass)
{
    CursorPresentation presentation;
    auto update = [&](int column, int row, double seconds, double interval = 0.016, bool shouldBypass = false) {
        return presentation.update(frameAt(column, row), seconds, interval, shouldBypass);
    };
    CHECK_EQ(update(2, 45, 0).column, 2);
    CHECK_EQ(update(102, 46, 1).column, 2);
    CHECK(presentation.isHolding());
    CHECK_EQ(update(2, 45, 1.008).column, 2);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    CHECK_EQ(update(102, 46, 2).column, 2);
    CHECK_EQ(update(102, 46, 2.017).column, 102);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    CHECK_EQ(update(2, 45, 3).column, 102);
    CHECK_EQ(update(15, 44, 3.010).column, 102);
    CHECK_EQ(update(2, 45, 3.017).column, 2);
    CHECK_EQ(update(102, 46, 4, 0.016, true).column, 2);
    CHECK(presentation.deadlineSeconds() != 0);
    CHECK_EQ(update(3, 45, 4.001, 0.016, true).column, 3);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    CHECK_EQ(update(102, 46, 4.1).column, 3);
    CHECK_EQ(update(102, 46, 4.117).column, 102);
    CHECK_EQ(update(2, 45, 5, 0).column, 2);
    CHECK_EQ(update(102, 46, 6, 1).column, 2);
    CHECK_EQ(update(102, 46, 6.017, 1).column, 102);
    CHECK_EQ(update(2, 45, 7, 1.0 / 120).column, 102);
    CHECK_EQ(update(2, 45, 7.009, 1.0 / 120).column, 2);
}

TEST(hiddenRedrawKeepsPromptStyleAndCurrentBlinkFocusAndOpacity)
{
    CursorPresentation presentation;
    const auto prompt = frameAt(2, 45);
    CHECK_EQ(presentation.update(prompt, 0, 0.016, false).row, 45);
    auto redraw = frameAt(102, 46);
    redraw.isProgramVisible = false;
    redraw.cursor.shape = Ubiquitty::Cursor::Shape::Block;
    redraw.cursor.rgb = { 1, 1, 1, 0.25f };
    redraw.cursor.mixA = 0.35f;
    redraw.cursor.focused = false;
    auto held = presentation.update(redraw, 1, 0.016, false);
    CHECK_EQ(held.column, 2);
    CHECK_EQ(held.row, 45);
    CHECK(held.shape == Ubiquitty::Cursor::Shape::Bar);
    CHECK_EQ(held.rgb.x, 0.1f);
    CHECK_EQ(held.rgb.w, 0.25f);
    CHECK_EQ(held.mixA, 0.35f);
    CHECK(!held.focused);
    CHECK(presentation.isHolding());
    const double deadline = presentation.deadlineSeconds();
    redraw.cursor.mixA = 0;
    held = presentation.update(redraw, 1.008, 0.016, true);
    CHECK_EQ(held.row, 45);
    CHECK_EQ(held.mixA, 0);
    CHECK_EQ(presentation.deadlineSeconds(), deadline);
    held = presentation.update(prompt, 1.012, 0.016, false);
    CHECK_EQ(held.row, 45);
    CHECK_EQ(held.column, 2);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    CHECK(!presentation.isHolding());
}

TEST(persistentHideExpiresWithoutRenewalAndInitialHideStaysHidden)
{
    CursorPresentation presentation;
    auto frame = frameAt(2, 45);
    frame.isProgramVisible = false;
    CHECK_EQ(presentation.update(frame, 0, 0.016, false).row, -1);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    frame.isProgramVisible = true;
    CHECK_EQ(presentation.update(frame, 1, 0.016, false).row, 45);
    frame.isProgramVisible = false;
    CHECK_EQ(presentation.update(frame, 2, 0.016, false).row, 45);
    const double deadline = presentation.deadlineSeconds();
    CHECK_EQ(presentation.update(frame, 2.049, 0.016, false).row, 45);
    CHECK_EQ(presentation.deadlineSeconds(), deadline);
    CHECK_EQ(presentation.update(frame, 2.051, 0.016, false).row, -1);
    CHECK_EQ(presentation.deadlineSeconds(), 0);
    CHECK(!presentation.isHolding());
    CHECK_EQ(presentation.update(frame, 2.100, 0.016, false).row, -1);
    frame.isProgramVisible = true;
    frame.cursor.column = 40;
    CHECK_EQ(presentation.update(frame, 3, 0.016, false).column, 40);
    frame.isProgramVisible = false;
    CHECK_EQ(presentation.update(frame, 4, 0, false).row, -1);
}

TEST(viewportAndScreenGeometryChangesDiscardHiddenPresentation)
{
    const auto prompt = frameAt(2, 45);
    std::array<CursorPresentation::Geometry, 12> geometries;
    geometries.fill(prompt.geometry);
    ++geometries[0].rows;
    ++geometries[1].columns;
    ++geometries[2].viewOffset;
    ++geometries[3].cellWidth;
    ++geometries[4].cellHeight;
    ++geometries[5].originX;
    ++geometries[6].originY;
    ++geometries[7].viewportWidth;
    ++geometries[8].viewportHeight;
    geometries[9].isAlternateActive = true;
    geometries[10].hasStatusLineOnBottomRow = true;
    geometries[11].cellWidth = std::numeric_limits<float>::quiet_NaN();
    for (const auto& geometry : geometries) {
        CursorPresentation presentation;
        CHECK_EQ(presentation.update(prompt, 0, 0.016, false).row, 45);
        auto hidden = prompt;
        hidden.isProgramVisible = false;
        CHECK_EQ(presentation.update(hidden, 1, 0.016, false).row, 45);
        hidden.geometry = geometry;
        CHECK_EQ(presentation.update(hidden, 1.001, 0.016, false).row, -1);
        CHECK_EQ(presentation.deadlineSeconds(), 0);
    }
}

TEST(ineligibleCursorAndResetNeverKeepAStaleOverlay)
{
    const auto prompt = frameAt(2, 45);
    std::array<CursorPresentation::Frame, 5> suppressed;
    suppressed.fill(prompt);
    suppressed[0].isEligible = false;
    suppressed[1].cursor.row = -1;
    suppressed[2].cursor.row = prompt.geometry.rows;
    suppressed[3].cursor.column = prompt.geometry.columns;
    suppressed[4].geometry.hasStatusLineOnBottomRow = true;
    suppressed[4].cursor.row = prompt.geometry.rows - 1;
    for (auto frame : suppressed) {
        CursorPresentation presentation;
        CHECK_EQ(presentation.update(prompt, 0, 0.016, false).row, 45);
        frame.isProgramVisible = false;
        CHECK_EQ(presentation.update(frame, 1, 0.016, false).row, -1);
        CHECK_EQ(presentation.deadlineSeconds(), 0);
    }
    CursorPresentation presentation;
    CHECK_EQ(presentation.update(prompt, 0, 0.016, false).row, 45);
    presentation.reset();
    auto hidden = prompt;
    hidden.isProgramVisible = false;
    CHECK_EQ(presentation.update(hidden, 1, 0.016, false).row, -1);
}

UBIQUITTY_TEST_MAIN()
