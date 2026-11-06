// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/Blocks.h — what a terminal does with its rich text blocks
// (Docs/rich-text.md, "The box model").
//
// A block's cells are covered spaces; the block lives as long as its rectangle
// still holds them. Placing writes them, `reconcileBlocks` removes every block
// whose rectangle something changed, and the scroll and resize paths move the
// blocks that move with their rows. The caps are checked before anything is
// mutated.

#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Layout.h"

#include <cstdint>
#include <expected>
#include <optional>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::RichText {

/// Queues missing native geometry for blocks on the active screen.
///
/// Requests share the bounded application-measurement queue and expire when
/// their placement or typography epoch changes.
///
/// - Parameter t: The host terminal whose frontend owns the fonts.
void requestBlockMeasurements(Ubiquitty::Terminal& t);

/// Applies a correlated native block measurement without issuing a PTY reply.
///
/// - Parameters:
///   - t: The terminal holding the original placement.
///   - pending: The request's placement and font epoch.
///   - measurement: The frontend geometry, consumed on acceptance.
/// - Returns: Whether the current placement accepted the geometry.
[[nodiscard]] bool adoptBlockMeasurement(Ubiquitty::Terminal& t, const PendingMeasurement& pending, Measurement measurement);

/// Where the cursor goes after a placement.
enum class CursorPolicy : std::uint8_t {
    /// To the block's left column on the row below it.
    Move,
    /// Nowhere.
    Stay,
};

/// A placement's geometry and options, as the sequence's keys give them.
struct Placement {
    std::uint16_t id = 0;
    std::uint32_t documentRevision = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    Overflow overflow = Overflow::Wrap;
    Alignment alignment = Alignment::Left;
    CursorPolicy cursor = CursorPolicy::Move;
    LineMode lineMode = LineMode::Natural;
    PointerMode pointerMode = PointerMode::Cells;
    Lifetime lifetime = Lifetime::Cells;
    /// Character boundary for the native cursor; 65535 means no override.
    std::uint16_t caret = 65535;
    /// Unicode scalar anchor of the visible content, in terminal-line mode.
    std::uint16_t viewportTop = 0;
    Insets padding {};
    VerticalPlacement verticalPlacement = VerticalPlacement::Top;
    FallibleVector<VisibleRect> visibleRects {};
};

/// Identifies the constraint that refused a block placement.
enum class PlacementError : std::uint8_t {
    Geometry,
    Denied,
    Limit,
    Revision
};

/// Places a block at the active screen's cursor.
///
/// Refused — nothing mutated — when the rectangle does not fit the screen's
/// width or height, when it reaches past the bottom under a narrowed scroll
/// region, when a row it covers is double-width or double-height, when
/// `RichTextRender` is not allowed, or when the block would exceed a cap
/// once the blocks it displaces are gone. A block with the same nonzero id,
/// and every block the rectangle overlaps, is displaced.
///
/// - Parameters:
///   - t: The terminal.
///   - placement: The block's geometry and options.
///   - document: The block's content.
/// - Returns: Success, or the geometry, permission or capacity refusal.
[[nodiscard]] std::expected<void, PlacementError> placeBlock(Ubiquitty::Terminal& t, const Placement& placement, Document document);

/// Removes the active screen's block with an id, or every block of the
/// active screen when `id` is empty.
///
/// - Parameters:
///   - t: The terminal.
///   - id: The block's id; zero names no block.
void removeBlocks(Ubiquitty::Terminal& t, std::optional<std::uint16_t> id);

/// Removes application overlays at a full-screen erase or alternate-screen exit.
/// Internal screen lifecycle cleanup does not depend on rendering permission.
///
/// - Parameter t: The terminal whose active screen is being cleared.
void removeOverlays(Ubiquitty::Terminal& t);

/// Removes every block whose rectangle no longer holds its own covered
/// cells, or whose top row the scrollback no longer holds, and clears the
/// cover from the cells a removed block leaves.
///
/// Runs after each parse pass and each resize; costs one test while no block
/// exists.
///
/// - Parameter t: The terminal.
void reconcileBlocks(Ubiquitty::Terminal& t);

/// Moves the primary screen's blocks up by rows a scroll fed to the
/// scrollback; the reconcile pass removes those whose top left it.
///
/// - Parameters:
///   - t: The terminal.
///   - lines: The rows the scroll fed to the scrollback.
void scrollBlocksIntoHistory(Ubiquitty::Terminal& t, int lines);

/// Follows a scroll or line edit that moves the active screen's rows
/// `[top, bottom]` in place, up by `rows` (down when negative).
///
/// A block wholly inside the band that stays inside it moves with its rows;
/// any other block the band touches is removed, because the move separates
/// its rows from each other or from the rows it was placed among.
///
/// - Parameters:
///   - t: The terminal.
///   - top: The first row the move moves.
///   - bottom: The last row the move moves.
///   - rows: How far the rows move up; negative moves them down.
void noteRowsShifted(Ubiquitty::Terminal& t, int top, int bottom, int rows);

/// Moves a screen's blocks down by `rows` (up when negative), as a resize
/// moves the rows under them.
///
/// - Parameters:
///   - t: The terminal.
///   - screen: The screen whose blocks move.
///   - rows: How far they move down.
void shiftBlocks(Ubiquitty::Terminal& t, ScreenMode screen, int rows);

/// The block covering a cell of a screen, if any.
///
/// - Parameters:
///   - t: The terminal.
///   - screen: The screen.
///   - screenRow: The row, negative into the scrollback.
///   - column: The column.
/// - Returns: The block, or null.
[[nodiscard]] const Block* blockCovering(const Ubiquitty::Terminal& t, ScreenMode screen, int screenRow, int column);

/// The screen a terminal is showing.
[[nodiscard]] ScreenMode activeScreen(const Ubiquitty::Terminal& t);

/// How many code points of a block a copy yields: its text through its last
/// shown line.
///
/// - Parameters:
///   - t: The terminal whose table lays the block out.
///   - block: The block.
/// - Returns: The length of the copyable prefix of the block's text.
[[nodiscard]] std::size_t copyableLength(const Ubiquitty::Terminal& t, const Block& block);

/// A block's layout under the terminal's table.
///
/// - Parameters:
///   - t: The terminal.
///   - block: One of its blocks.
/// - Returns: The layout, built on first use and when the table moves.
[[nodiscard]] const Layout& layoutOf(const Ubiquitty::Terminal& t, const Block& block);

/// Adopts the typography table a frontend publishes.
///
/// A table that is not well formed is refused and the terminal keeps the
/// one it had; one equal to the table held changes nothing, so a frontend
/// may publish on every frame it cuts without rebuilding a layout.
///
/// - Parameters:
///   - t: The terminal.
///   - facts: The table.
/// - Returns: Whether the table was adopted or already held.
bool adoptTypography(Ubiquitty::Terminal& t, const TypographyFacts& facts);

/// What a terminal's typography table says, for a caller that needs the
/// table's shape rather than its advances.
struct TypographySummary {
    /// The cell, in 1/64 point; zero without a table.
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint8_t faceCount = 0;
    /// Moves on every publication.
    std::uint32_t epoch = 0;
};

/// The shape of the table a terminal lays its blocks out by.
///
/// - Parameter t: The terminal.
/// - Returns: The summary; all zero before any table is published.
[[nodiscard]] TypographySummary typographySummary(const Ubiquitty::Terminal& t);

/// Whether a different renderer now owns measurement, even with equal metrics.
enum class TypographyAuthority : std::uint8_t {
    Unchanged,
    Changed
};

/// Adopts a table another terminal already holds, sharing it.
///
/// - Parameters:
///   - t: The terminal.
///   - typography: The other terminal's table; null facts publish none.
///   - authority: Whether the native measurement owner changed.
void adoptTypography(Ubiquitty::Terminal& t, const std::shared_ptr<const TypographyFacts>& typography, TypographyAuthority authority = TypographyAuthority::Unchanged);

} // namespace Ubiquitty::RichText
