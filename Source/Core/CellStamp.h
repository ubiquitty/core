// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include <cstddef>
#include <cstdint>

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

} // namespace Ubiquitty
