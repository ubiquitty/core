// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/BlockStore.h — the rich text blocks a terminal holds, per screen
// (Docs/rich-text.md, "The box model").
//
// A block is a rectangle of cells and the document drawn over it. The cells
// are the grid's, written as covered spaces when the block is placed; the
// store holds what the cells cannot: where each block is, its options and its
// document. Every cap is checked by the caller against `planAdmission` before
// anything is mutated, so a store never holds more than the caps allow.

#include "Core/RichText/Document.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Ubiquitty {
/// Which of a terminal's two screens; defined in `Core/CellGrid.h`.
enum class ScreenMode : std::uint8_t;
}

namespace Ubiquitty::RichText {

/// The most blocks one screen may hold.
inline constexpr int maxBlocksPerScreen = 256;
/// The retained source allowance, including block slots and document arrays.
/// Layouts, rasters, and pending measurements have separate bounded owners.
inline constexpr std::size_t maxRetainedBytes = 1024 * 1024;
/// A decoder bound implied by the source-byte allowance, not an admission quota.
inline constexpr int maxTerminalCodePoints = static_cast<int>(maxRetainedBytes / sizeof(std::uint32_t));

/// What a block does with text that does not fit its rectangle.
enum class Overflow : std::uint8_t {
    /// Lines wrap at the block's width; lines past its height are not shown.
    Wrap,
    /// Lines break only where the body says; what passes an edge is clipped.
    Clip,
    /// Lines wrap, and the last line shown ends with an ellipsis when text
    /// remains after it.
    Ellipsis,
};

/// A block's content insets, in points on each side: the padding between its
/// rectangle and the content box its text flows in.
struct Insets {
    std::uint8_t top = 0;
    std::uint8_t right = 0;
    std::uint8_t bottom = 0;
    std::uint8_t left = 0;

    friend bool operator==(const Insets&, const Insets&) = default;
};

/// Where a block's content sits in its content box when it needs less height.
enum class VerticalPlacement : std::uint8_t {
    Top,
    Center,
    Bottom,
};

/// Natural RTF metrics, or terminal-sized text on the grid's baselines.
enum class LineMode : std::uint8_t {
    Natural,
    Terminal
};
/// Whether application mouse input names cells or characters in this block.
enum class PointerMode : std::uint8_t {
    Cells,
    Characters
};

/// Application overlays survive grid repaint and are explicitly replaced/deleted.
enum class Lifetime : std::uint8_t {
    Cells,
    Overlay
};

/// A visible piece of a block, in cells relative to its placement.
/// Clipping never changes the paragraph width used for shaping.
struct VisibleRect {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    [[nodiscard]] bool contains(int row, int column) const
    {
        return column >= x && column < x + width && row >= y && row < y + height;
    }
    friend bool operator==(const VisibleRect&, const VisibleRect&) = default;
};

inline constexpr std::size_t maxVisibleRects = 32;

/// Checks bounded, nonoverlapping pieces inside a placement. Empty means full.
[[nodiscard]] bool validVisibleRects(std::span<const VisibleRect>, std::uint16_t width, std::uint16_t height);

/// One placed block.
struct Block {
    /// The terminal-unique number of this placement; a block placed again,
    /// even with the same id and body, gets another. A frontend keys what it
    /// draws for a block on it.
    std::uint32_t serial = 0;
    /// The program's id for the block; zero is anonymous.
    std::uint16_t id = 0;
    /// The application's source identity; geometry-only updates may retain it.
    std::uint32_t documentRevision = 0;
    /// The top row on its screen; negative once the rows are in the
    /// scrollback.
    std::int32_t row = 0;
    std::uint16_t column = 0;
    /// The rectangle, in cells and rows; at least one of each.
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    Overflow overflow = Overflow::Wrap;
    /// The alignment of a paragraph that names none.
    Alignment alignment = Alignment::Left;
    LineMode lineMode = LineMode::Natural;
    PointerMode pointerMode = PointerMode::Cells;
    Lifetime lifetime = Lifetime::Cells;
    /// Character boundary for the native cursor; 65535 means no override.
    std::uint16_t caret = 65535;
    /// The first visible Unicode scalar, resolved to a line by each renderer.
    std::uint16_t viewportTop = 0;
    /// The content insets, in points.
    Insets padding {};
    VerticalPlacement verticalPlacement = VerticalPlacement::Top;
    FallibleVector<VisibleRect> visibleRects {};
    Document document;

    /// Whether the block covers a cell of its screen.
    [[nodiscard]] bool covers(int screenRow, int screenColumn) const
    {
        if (screenRow < row || screenRow >= row + height || screenColumn < column || screenColumn >= column + width)
            return false;
        if (visibleRects.empty())
            return true;
        for (const auto& rectangle : visibleRects) {
            if (rectangle.contains(screenRow - row, screenColumn - column))
                return true;
        }
        return false;
    }

    friend bool operator==(const Block&, const Block&) = default;
};

/// The blocks of a terminal's two screens.
class BlockStore {
public:
    /// A placement's prepared capacity and oldest complete history to retain.
    struct Admission {
        std::size_t screenCapacity = 0;
        /// Rows are relative to the live screen; the minimum means no eviction.
        std::int32_t historyBoundary = std::numeric_limits<std::int32_t>::min();
    };
    /// A screen's blocks, in placement order.
    [[nodiscard]] std::span<const Block> blocks(ScreenMode screen) const { return m_screens[index(screen)]; }
    [[nodiscard]] std::span<Block> mutableBlocks(ScreenMode screen) { return m_screens[index(screen)]; }

    /// Whether either screen holds a block.
    [[nodiscard]] bool isEmpty() const { return m_screens[0].empty() && m_screens[1].empty(); }

    /// The code points every block holds together.
    [[nodiscard]] int codePointCount() const;

    /// Plans bounded source admission, reclaiming only a complete history prefix.
    ///
    /// - Parameters:
    ///   - screen: The screen the block is placed on.
    ///   - block: The independently prepared placement and document.
    ///   - leaving: One flag per block of the screen, set for the blocks the
    ///     placement removes; empty when it removes none.
    /// - Returns: Reserved capacity and the history prefix to retire, or refusal.
    [[nodiscard]] std::expected<Admission, StorageError> planAdmission(ScreenMode screen, const Block& block, std::span<const bool> leaving) const;

    /// Adds a block, stamping it with the next serial.
    ///
    /// - Precondition: ``planAdmission`` allowed it and capacity is reserved.
    /// - Parameters:
    ///   - screen: The screen the block is placed on.
    ///   - block: The block, whose serial is replaced.
    /// - Returns: The stored block.
    const Block& add(ScreenMode screen, Block block);

    /// Adds a block that already carries its serial, as a replica receives it.
    ///
    /// - Parameters:
    ///   - screen: The screen the block is on.
    ///   - block: The block.
    void restore(ScreenMode screen, Block block);

    /// Reserves one screen's exact block capacity before decoded placements arrive.
    /// - Parameters:
    ///   - screen: The screen receiving the blocks.
    ///   - count: The complete decoded block count for that screen.
    /// - Returns: Admission or refusal without changing visible blocks.
    [[nodiscard]] std::expected<void, StorageError> tryReserve(ScreenMode screen, std::size_t count);

    /// Adopts one privately prepared block without copying its document arrays.
    /// - Parameters:
    ///   - screen: The receiving screen.
    ///   - block: The independent block owner to consume.
    /// - Returns: Admission or refusal before its block or serial is published.
    [[nodiscard]] std::expected<void, StorageError> tryRestore(ScreenMode screen, Block block);

    /// The exact capacities of both block arrays and their variable-sized owners.
    [[nodiscard]] std::size_t storageBytes() const noexcept;

    /// The local source-storage admission quota; it is not replicated content.
    [[nodiscard]] std::size_t storageLimit() const { return m_storageLimit; }

    /// Sets a local quota without discarding content or exceeding the decoder bound.
    /// - Parameter bytes: At least the held capacity and at most `maxRetainedBytes`.
    /// - Returns: Admission or refusal with the old quota retained.
    [[nodiscard]] std::expected<void, StorageError> setStorageLimit(std::size_t bytes);

    /// Removes the block at `position` of a screen's blocks.
    ///
    /// - Parameters:
    ///   - screen: The screen.
    ///   - position: The block's index in ``blocks(screen)``.
    void remove(ScreenMode screen, std::size_t position);

    /// Removes every block of both screens.
    void clear();

    /// The serial the next placement takes.
    [[nodiscard]] std::uint32_t nextSerial() const { return m_nextSerial; }

    /// Continues the serials of a store this one replaces, so no serial is
    /// handed out twice by one terminal.
    ///
    /// - Parameter next: The replaced store's next serial.
    void resumeSerials(std::uint32_t next) { m_nextSerial = std::max(m_nextSerial, next); }

    friend bool operator==(const BlockStore& left, const BlockStore& right)
    {
        return left.m_screens == right.m_screens && left.m_nextSerial == right.m_nextSerial;
    }

private:
    [[nodiscard]] static std::size_t index(ScreenMode screen) { return static_cast<std::size_t>(screen) ? 1 : 0; }

    std::array<FallibleVector<Block>, 2> m_screens;
    std::uint32_t m_nextSerial = 1;
    std::size_t m_storageLimit = maxRetainedBytes;
};

} // namespace Ubiquitty::RichText
