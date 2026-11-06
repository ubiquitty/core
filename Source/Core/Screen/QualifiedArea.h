// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Screen/QualifiedArea.h -- where DAQ began each qualified area of a screen
// (ECMA-48 6.5.2, 8.3.25). A qualified area runs from the position DAQ marks to
// the position before the next DAQ, so the extent of the area holding the
// active position, which EA needs, is the pair of neighbouring starts. The
// protection each cell obeys travels with the cell; the store holds only what a
// cell cannot, the boundaries.
//
// The starts are packed into fixed words, so the store holds no allocation and
// adds a fixed 520 bytes to a Terminal.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Ubiquitty {
/// Which of a terminal's two screens; defined in `Core/CellGrid.h`.
enum class ScreenMode : std::uint8_t;
}

namespace Ubiquitty::Screen {

/// Whether the contents of a qualified area are protected against erasure
/// while the erasure mode is PROTECT.
enum class AreaProtection : std::uint8_t {
    Unprotected,
    Protected,
};

/// The first character position of one qualified area.
struct AreaStart {
    int row = 0;
    int column = 0;
    AreaProtection protection = AreaProtection::Unprotected;

    /// - Returns: Whether this start is strictly before the position, in reading order.
    [[nodiscard]] constexpr bool isBefore(int otherRow, int otherColumn) const
    {
        return row < otherRow || (row == otherRow && column < otherColumn);
    }

    /// - Returns: Whether this start is at or before the position, in reading order.
    [[nodiscard]] constexpr bool isAtOrBefore(int otherRow, int otherColumn) const
    {
        return row < otherRow || (row == otherRow && column <= otherColumn);
    }
};

/// The qualified areas of both screens, in reading order.
class QualifiedAreaStore {
public:
    /// The most areas one screen records; defining another past this drops the earliest.
    static constexpr int maxAreasPerScreen = 64;

    /// - Parameter screen: The screen whose areas to count.
    /// - Returns: How many areas the screen records.
    [[nodiscard]] int count(ScreenMode screen) const
    {
        return static_cast<int>(std::min<std::uint32_t>(m_words[index(screen)], maxAreasPerScreen));
    }

    /// - Parameters:
    ///   - screen: The screen whose area to read.
    ///   - position: An index below ``count``.
    /// - Returns: The area's start.
    [[nodiscard]] AreaStart start(ScreenMode screen, int position) const
    {
        return unpack(m_words[slot(screen, position)]);
    }

    /// - Returns: Whether neither screen records an area.
    [[nodiscard]] bool isEmpty() const { return m_words[0] == 0 && m_words[1] == 0; }

    /// Records the start of an area, replacing one that begins at the same position.
    ///
    /// - Parameters:
    ///   - screen: The screen holding the area.
    ///   - area: The area's first position and protection.
    void define(ScreenMode screen, AreaStart area)
    {
        const int total = count(screen);
        int position = 0;
        while (position < total && start(screen, position).isBefore(area.row, area.column))
            ++position;
        if (position < total && start(screen, position).row == area.row && start(screen, position).column == area.column) {
            m_words[slot(screen, position)] = pack(area);
            return;
        }
        int used = total;
        if (used == maxAreasPerScreen) {
            for (int moving = 1; moving < used; ++moving)
                m_words[slot(screen, moving - 1)] = m_words[slot(screen, moving)];
            --used;
            position = std::max(0, position - 1);
        }
        for (int moving = used; moving > position; --moving)
            m_words[slot(screen, moving)] = m_words[slot(screen, moving - 1)];
        m_words[slot(screen, position)] = pack(area);
        m_words[index(screen)] = static_cast<std::uint32_t>(used + 1);
    }

    /// Moves the starts in a band of rows with the content that scrolled.
    ///
    /// A start the move carries out of the band is dropped: its area's
    /// beginning is no longer on the page. A band moves its starts as one and
    /// the rows outside it stay put, so the survivors keep their order.
    ///
    /// - Parameters:
    ///   - screen: The screen that scrolled.
    ///   - top: The band's first row.
    ///   - bottom: The band's last row.
    ///   - rows: The rows the content moved up; negative moves it down.
    void shiftRows(ScreenMode screen, int top, int bottom, int rows)
    {
        const int total = count(screen);
        int kept = 0;
        for (int position = 0; position < total; ++position) {
            AreaStart area = start(screen, position);
            if (area.row >= top && area.row <= bottom) {
                area.row -= rows;
                if (area.row < top || area.row > bottom)
                    continue;
            }
            m_words[slot(screen, kept++)] = pack(area);
        }
        m_words[index(screen)] = static_cast<std::uint32_t>(kept);
    }

    /// Forgets the areas of one screen.
    ///
    /// - Parameter screen: The screen to forget.
    void clear(ScreenMode screen) { m_words[index(screen)] = 0; }

    /// Forgets the areas of both screens.
    void clear() { m_words.fill(0); }

private:
    [[nodiscard]] static std::size_t index(ScreenMode screen) { return static_cast<std::size_t>(screen); }
    [[nodiscard]] static std::size_t slot(ScreenMode screen, int position)
    {
        return 2 + index(screen) * maxAreasPerScreen + static_cast<std::size_t>(position);
    }

    // Bit 31 protection, bits 16...30 the row, bits 0...15 the column.
    [[nodiscard]] static std::uint32_t pack(AreaStart area)
    {
        return (area.protection == AreaProtection::Protected ? 0x80000000u : 0u)
            | ((static_cast<std::uint32_t>(area.row) & 0x7FFFu) << 16) | (static_cast<std::uint32_t>(area.column) & 0xFFFFu);
    }
    [[nodiscard]] static AreaStart unpack(std::uint32_t word)
    {
        return { static_cast<int>((word >> 16) & 0x7FFFu), static_cast<int>(word & 0xFFFFu), (word & 0x80000000u) ? AreaProtection::Protected : AreaProtection::Unprotected };
    }

    std::array<std::uint32_t, 2 + 2 * maxAreasPerScreen> m_words {};
};

} // namespace Ubiquitty::Screen
