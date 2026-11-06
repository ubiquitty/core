// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace Ubiquitty::VT {

// DECDLD / DRCS downloadable soft character set (VT220+). A soft font is
// downloaded via `DCS Pfn;Pcn;Pe;Pcmw;Pw;Pt;Pcmh;Pss { Dscs <sixel-bitmaps> ST`
// and designated to a G-set via SCS. This is the portable engine store + the
// DECDLD parse target.
//
// Downloaded bitmaps PAINT: a designated byte resolves to the sentinel code point
// below, the frame snapshot carries a copy of this struct (re-copied only when
// `serial` changes), and Rendering/DRCSRaster.h scales `glyph[][]` onto the cell
// box of a glyph-atlas slot. The cell then blits through the ordinary cell quad,
// so a DRCS cell picks up foreground/background, inverse, dim and the cursor like
// any other inked cell. A position the host never downloaded paints as background
// only — DEC leaves an undefined DRCS slot blank.
//
// Caps are FIXED (no wire-driven allocation): an oversize matrix is rejected and
// every cell/column/row index is clamped before any write, so a hostile DECDLD
// can neither allocate nor write out of bounds.
inline constexpr int drcsMaxCells = 96; // a 94- or 96-char set
inline constexpr int drcsMaxMatrixWidth = 16; // columns (VT320 <= 15); reject wider
inline constexpr int drcsMaxMatrixHeight = 24; // rows; reject taller

// A soft-font glyph cell carries this private sentinel code point (base + glyph
// index, 0..drcsMaxCells-1) so a DRCS cell is distinguishable from real text and
// its bitmap findable in Terminal::softFont. Plane-15 SPUA-A, chosen clear of
// the U+10EEEE Unicode-placeholder base used by the kitty graphics protocol.
//
// Written by the Core resolver (VT/VT.cpp); the renderer's per-cell test is the
// unsigned range check `codePoint - drcsCodePointBase < drcsMaxCells`, so the
// base must stay far above any code point real text can carry.
inline constexpr uint32_t drcsCodePointBase = 0xF5000;

/// Returns a fresh process-wide identity for a committed soft-font download.
///
/// The glyph atlas is SHARED across panes and its key has no room for a
/// terminal identity, so two panes' soft fonts must not be able to name one
/// slot. Keying on the serial also makes a re-download invalidate the previous
/// rasterisations rather than serving their pixels under the new font's name.
///
/// 0 is reserved and means no font has ever been downloaded — the frame
/// snapshot's nothing-to-carry state.
///
/// - Returns: A non-zero serial, distinct from every previous one.
inline uint32_t nextSoftFontSerial()
{
    static std::atomic<uint32_t> counter { 0 };
    uint32_t serial;
    do {
        serial = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    } while (serial == 0); // 0 is reserved; reachable only on a 2^32 wrap
    return serial;
}

/// One DECDLD-downloaded character set and the bitmaps it defines.
struct SoftFont {
    bool valid = false; // a set has been downloaded
    bool set96 = false; // Pss: 96-char set (vs 94)
    uint8_t designIntermediate = 0; // Dscs intermediate (0x20..0x2F, 0 if none)
    char designFinal = 0; // Dscs final (0x30..0x7E) — the SCS selector
    uint8_t matrixWidth = 0; // resolved Pcmw (<= drcsMaxMatrixWidth)
    uint8_t matrixHeight = 0; // resolved Pcmh (<= drcsMaxMatrixHeight)
    uint32_t serial = 0; // nextSoftFontSerial() per committed download; 0 = never downloaded
    // glyph[cell][row]: bit (1u << col) set = pixel on at (col, row). `cell` is
    // the 0-based position within the set; row 0 = top.
    std::array<std::array<uint16_t, drcsMaxMatrixHeight>, drcsMaxCells> glyph {};
    std::array<bool, drcsMaxCells> glyphSet {}; // cell received a glyph in this/a prior load
};

} // namespace Ubiquitty::VT
