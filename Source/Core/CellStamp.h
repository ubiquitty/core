// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>

#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>

#endif

namespace Ubiquitty {

/// Stores cells that differ from one template in their code point alone.
///
/// The template is loaded once, into registers, so it may be borrowed from
/// the row being written, and each cell is then two 16-byte stores with no
/// read of the destination. Left to the compiler, a loop that patches a code
/// point into a copied cell reloads the template from the stack for every
/// cell and writes each one as three overlapping stores.
class CellStamp {
public:
    /// Loads the template.
    ///
    /// - Parameter cellTemplate: The cell every stored cell copies, but for
    ///   its code point.
    explicit CellStamp(const Cell& cellTemplate)
#if defined(__aarch64__)
        : m_low(vreinterpretq_u32_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(&cellTemplate))))
        , m_high(vld1q_u8(reinterpret_cast<const uint8_t*>(&cellTemplate) + 16))
#elif defined(__x86_64__) || defined(_M_X64)
        : m_low(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&cellTemplate)), _mm_set_epi32(-1, -1, -1, 0)))
        , m_high(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&cellTemplate) + 1))
#else
        : m_cell(cellTemplate)
#endif
    {
        static_assert(sizeof(Cell) == 32 && offsetof(Cell, codePoint) == 0, "the stamp builds 32-byte cells led by the code point");
    }

    /// Stores the template with a code point of its own.
    ///
    /// - Parameters:
    ///   - destination: The cell to write.
    ///   - codePoint: Its code point.
    void store(Cell* destination, uint32_t codePoint) const
    {
#if defined(__aarch64__)
        auto* bytes = reinterpret_cast<uint8_t*>(destination);
        vst1q_u8(bytes, vreinterpretq_u8_u32(vsetq_lane_u32(codePoint, m_low, 0)));
        vst1q_u8(bytes + 16, m_high);
#elif defined(__x86_64__) || defined(_M_X64)
        auto* halves = reinterpret_cast<__m128i*>(destination);
        _mm_storeu_si128(halves, _mm_or_si128(m_low, _mm_cvtsi32_si128(static_cast<int>(codePoint))));
        _mm_storeu_si128(halves + 1, m_high);
#else
        *destination = m_cell;
        destination->codePoint = codePoint;
#endif
    }

    /// Whether a cell already holds the template with a code point.
    ///
    /// Compared from registers. A cell patched on the stack and then
    /// compared whole is a narrow store read back by a wide load, which the
    /// processor cannot forward: every such compare waits for the store to
    /// retire, and a repaint asks it of every cell.
    ///
    /// - Parameters:
    ///   - destination: The cell to read.
    ///   - codePoint: The code point the template would be stored with.
    /// - Returns: Whether storing would change nothing.
    [[nodiscard]] bool matches(const Cell* destination, uint32_t codePoint) const
    {
#if defined(__aarch64__)
        const auto* bytes = reinterpret_cast<const uint8_t*>(destination);
        const uint8x16_t low = vceqq_u8(vld1q_u8(bytes), vreinterpretq_u8_u32(vsetq_lane_u32(codePoint, m_low, 0)));
        const uint8x16_t high = vceqq_u8(vld1q_u8(bytes + 16), m_high);
        return vminvq_u8(vandq_u8(low, high)) == 0xFF;
#elif defined(__x86_64__) || defined(_M_X64)
        const auto* halves = reinterpret_cast<const __m128i*>(destination);
        const __m128i low = _mm_cmpeq_epi8(_mm_loadu_si128(halves), _mm_or_si128(m_low, _mm_cvtsi32_si128(static_cast<int>(codePoint))));
        const __m128i high = _mm_cmpeq_epi8(_mm_loadu_si128(halves + 1), m_high);
        return _mm_movemask_epi8(_mm_and_si128(low, high)) == 0xFFFF;
#else
        Cell cell = m_cell;
        cell.codePoint = codePoint;
        return std::memcmp(destination, &cell, sizeof(Cell)) == 0;
#endif
    }

private:
#if defined(__aarch64__)
    uint32x4_t m_low;
    uint8x16_t m_high;
#elif defined(__x86_64__) || defined(_M_X64)
    /// The template's first half, its code point cleared.
    __m128i m_low;
    __m128i m_high;
#else
    Cell m_cell;
#endif
};

/// A cell of a pen and a hyperlink, with no cluster.
///
/// Assembled in registers and stored as the two halves a ``CellStamp`` and a
/// whole-cell copy read back. A cell filled a field at a time is narrow
/// stores, and the wide load that reads it next cannot be forwarded from
/// them: it waits for every one to retire, on each write of a run.
///
/// - Parameters:
///   - pen: The pen the cell carries.
///   - linkID: The hyperlink the cell names, or zero.
///   - codePoint: The cell's code point; zero for a run's template.
/// - Returns: The cell.
[[nodiscard]] inline Cell cellFromPen(const Pen& pen, uint16_t linkID, uint32_t codePoint = 0)
{
    static_assert(offsetof(Cell, pen) == 4 && sizeof(Pen) == 20 && offsetof(Cell, linkIDLo) == 28 && offsetof(Cell, linkIDHi) == 29,
        "the template is assembled at the cell's own offsets");
    Cell cell;
#if defined(__x86_64__) || defined(_M_X64)
    const __m128i penLow = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&pen));
    std::uint32_t penTail;
    std::memcpy(&penTail, reinterpret_cast<const char*>(&pen) + 16, sizeof penTail);
    const __m128i low = _mm_or_si128(_mm_slli_si128(penLow, 4), _mm_cvtsi32_si128(static_cast<int>(codePoint)));
    __m128i high = _mm_or_si128(_mm_srli_si128(penLow, 12), _mm_slli_si128(_mm_cvtsi32_si128(static_cast<int>(penTail)), 4));
    high = _mm_insert_epi16(high, linkID, 6);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&cell), low);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&cell) + 1, high);
#else
    cell.codePoint = codePoint;
    cell.pen = pen;
    cell.combining = {};
    cell.setLinkID(linkID);
    cell.clusterPacked = 0;
#endif
    return cell;
}

/// A cell of a pen under another attribute set, with no cluster.
///
/// What a glyph placed a character at a time is: the pen in force with the
/// attributes of the glyph's own width. Assembled as ``cellFromPen`` is, for
/// the reason given there.
///
/// - Parameters:
///   - pen: The pen whose colours and styles the cell carries.
///   - attributes: The attribute set that replaces the pen's own.
///   - linkID: The hyperlink the cell names, or zero.
///   - codePoint: The cell's code point.
/// - Returns: The cell.
[[nodiscard]] inline Cell cellFromPen(const Pen& pen, Ubiquitty::VT::AttributeSet attributes, uint16_t linkID, uint32_t codePoint)
{
    static_assert(offsetof(Pen, attributes) == 0 && sizeof(pen.attributes) == 2, "the attribute set is the pen's first word");
#if defined(__x86_64__) || defined(_M_X64)
    Cell cell;
    const __m128i penLow = _mm_insert_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&pen)), attributes.toRaw(), 0);
    std::uint32_t penTail;
    std::memcpy(&penTail, reinterpret_cast<const char*>(&pen) + 16, sizeof penTail);
    const __m128i low = _mm_or_si128(_mm_slli_si128(penLow, 4), _mm_cvtsi32_si128(static_cast<int>(codePoint)));
    __m128i high = _mm_or_si128(_mm_srli_si128(penLow, 12), _mm_slli_si128(_mm_cvtsi32_si128(static_cast<int>(penTail)), 4));
    high = _mm_insert_epi16(high, linkID, 6);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&cell), low);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&cell) + 1, high);
    return cell;
#else
    Cell cell = cellFromPen(pen, linkID, codePoint);
    cell.pen.attributes = attributes;
    return cell;
#endif
}

} // namespace Ubiquitty
