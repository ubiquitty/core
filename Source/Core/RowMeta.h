// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Bits/OptionSet.h"

#include <cstdint>

// DECDWL / DECDHL per-row width/height attribute, set by ESC # 3/4/5/6 and
// stored in Ubiquitty::RowMeta::lineAttribute. CellGrid is the store of record; the
// renderer reads it to scale glyphs horizontally (DWL) or vertically (DHL
// top + bottom halves). Global scope (its long-standing home) so the many
// unqualified call sites resolve unchanged.
//   0  NORMAL              single-width, single-height
//   1  DOUBLE_WIDTH        DECDWL (ESC # 6)
//   2  DOUBLE_HEIGHT_TOP   DECDHL top half     (ESC # 3)
//   3  DOUBLE_HEIGHT_BOT   DECDHL bottom half  (ESC # 4)

namespace Ubiquitty {

constexpr uint8_t lineAttributeNormal = 0;
constexpr uint8_t lineAttributeDoubleWidth = 1;
constexpr uint8_t lineAttributeDoubleHeightTop = 2;
constexpr uint8_t lineAttributeDoubleHeightBottom = 3;

/// How much larger than one cell a run of text draws, and which part of the
/// scaled glyph a row shows.
///
/// Scales are integer multiples of the cell: `widthScale` 2 spans two columns
/// per logical column, `heightScale` 2 draws from a master rasterised at twice
/// the cell height. DEC's line attributes reach a subset of this — DECDWL is
/// 2x1, and the two DECDHL halves are 2x2 differing only in ``half`` — and the
/// kitty text-sizing protocol reaches the rest, so a consumer reads a scale
/// rather than asking which mechanism produced it.
struct TextScale {
    /// Which part of a vertically scaled glyph this row draws.
    enum class Half : uint8_t {
        Whole, ///< The glyph occupies this row entirely.
        Top, ///< The upper half of a master spanning two rows.
        Bottom, ///< The lower half.
    };

    uint8_t widthScale = 1; ///< Columns per logical column, >= 1.
    uint8_t heightScale = 1; ///< Cell heights the master spans, >= 1.
    Half half = Half::Whole;

    /// - Returns: True when a glyph draws wider than a single cell.
    [[nodiscard]] constexpr bool wide() const { return widthScale > 1; }

    /// The advance multiplier a renderer applies per logical column.
    [[nodiscard]] constexpr float xStep() const { return static_cast<float>(widthScale); }
};

/// - Returns: The ``TextScale`` a DEC line attribute selects.
constexpr TextScale textScaleForLineAttribute(uint8_t lineAttribute)
{
    TextScale out;
    switch (lineAttribute) {
    case lineAttributeDoubleWidth:
        out.widthScale = 2;
        break;
    case lineAttributeDoubleHeightTop:
        out.widthScale = 2;
        out.heightScale = 2;
        out.half = TextScale::Half::Top;
        break;
    case lineAttributeDoubleHeightBottom:
        out.widthScale = 2;
        out.heightScale = 2;
        out.half = TextScale::Half::Bottom;
        break;
    default:
        break;
    }
    return out;
}

// Per-row text metadata, the row-level sibling of the per-cell Cell.
// DECDWL/DECDHL line width-height, OSC-133 prompt mark, and the
// soft-wrap continuation flag for one physical row. POD, 4 bytes,
// trivially copyable: it rides CellGrid's row-pointer ring exactly like
// m_rowFilled, so a row's whole record (cells + filled-width + meta)
// moves together on scroll. Never folded into Cell — that would fatten
// the per-cell hot path; this is read once per row in paint.
/// A per-row property set on write and read at render.
///
/// One byte rather than a `bool` apiece because ``RowMeta`` is full at four and
/// the budget rests on that. These share a byte legitimately: none of them
/// changes how a row draws, each exists so the common row can skip a slower
/// path, and every reader must say which bit it means — a `bool` that is
/// secretly a bitfield reads as true for a flag it does not name.
///
/// ``lineAttribute`` does not belong in here. It says how every cell in the
/// row draws, and its readers compare it as an enumeration, so a bit packed
/// beside it silently stops matching.
enum class RowFlag : uint8_t {
    /// The row soft-wraps into the next one rather than ending at a newline.
    Wrapped = 1 << 0,
    /// The row holds a cursive or right-to-left codepoint
    /// (``Unicode::mayNeedShaping``), so the render-time `getDisplayRow` gate
    /// must shape it.
    NeedsShaping = 1 << 1,
    /// The row holds at least one cell wider or taller than a single cell, so a
    /// reader must resolve each cell against the run it belongs to rather than
    /// on its own.
    ///
    /// Latched on write and cleared only when the row is recycled, so it is
    /// conservative in the safe direction: a stale true costs a slower walk, and
    /// only a missing true is a correctness fault.
    ///
    /// **Resolve by tracking during a left-to-right row walk; scan backwards only
    /// for a one-off query.** A walk has already passed the run's leading cell,
    /// so remembering it is constant work, where asking each cell to find its own
    /// leader is quadratic in the run width — and the protocol admits runs up to
    /// 49 columns. Tracking costs the plain loop on a row with no multicell
    /// content, which is nearly every row, so a reader takes the tracking loop
    /// only where this stored bit is set.
    HasMulticell = 1 << 2,
    /// The row holds a proportional run (Docs/proportional-text.md): text set
    /// in the proportional face at its own advances, spanning the cells it
    /// claims. A reader that maps pixels to text, or text to cells, must ask
    /// the run's layout on such a row where the grid pitch answers on every
    /// other; and the renderer's flowed arm runs only here, so a row without
    /// the bit pays nothing for the runs it does not hold.
    ///
    /// Latched on write like ``HasMulticell``, on the host by the run's own
    /// writer and on a mirror by the wire's row decoder, and cleared only
    /// when the row is recycled: a stale true costs a slower walk, and only a
    /// missing true is a correctness fault.
    Flowed = 1 << 3,
};

struct RowMeta {
    uint8_t lineAttribute; // LINE_ATTR_* — DECDWL / DECDHL
    uint8_t oscMark; // OSC-133 prompt mark
    Ubiquitty::Bits::OptionSet<RowFlag> flags;
    uint8_t reserved; // keeps the four-byte shape while a fifth field waits

    /// - Returns: True when `flag` is set.
    [[nodiscard]] constexpr bool has(RowFlag flag) const { return flags.contains(flag); }

    /// Sets or clears `flag`.
    ///
    /// - Parameters:
    ///   - flag: The single bit to write.
    ///   - isSet: The flag's target state, not a toggle.
    constexpr void set(RowFlag flag, bool isSet) { flags.set(flag, isSet); } // bool-ok: mirrors Bits::OptionSet::set(E, bool), the type this wraps
};
// Keep RowMeta a flat 4 bytes (the per-Ubiquitty::Terminal footprint budget rests on it;
// never fold it into Cell, which would fatten the per-cell hot path).
static_assert(sizeof(RowMeta) == 4, "RowMeta must stay 4 bytes");
static_assert(alignof(RowMeta) == 1, "RowMeta rides CellGrid's row ring as raw bytes");

} // namespace Ubiquitty
