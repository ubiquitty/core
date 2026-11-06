// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// Arabic cursive joining: per-codePoint joining type, contextual form selection,
// and resolution to a Unicode Arabic Presentation Form, so even a MacRoman
// frontend connects letters. A letter's form is a pure
// function of its neighbours' joining types, skipping Transparent marks.
//
// Joining types (UAX #9 / ArabicShaping.txt "Joining_Type" field):
//   R — Right_Joining   (joins a preceding letter; alef, dal, reh)
//   L — Left_Joining    (joins a following letter; rare)
//   D — Dual_Joining    (joins on both sides; most Arabic letters)
//   C — Join_Causing    (forces a join, takes no shape; tatweel U+0640, ZWJ)
//   U — Non_Joining     (joins nobody; also ZWNJ U+200C, which breaks a join)
//   T — Transparent     (combining marks; invisible to the neighbour scan)
// A neighbour joins toward X when Dual or Join_Causing (either side), a
// Right_Joining on X's left, or a Left_Joining on X's right.

namespace Ubiquitty::Unicode {

// Arabic Joining_Type. Values are stable; the table stores them packed.
enum class JoiningType : std::uint8_t {
    NonJoining = 0, // U — joins nobody (also ZWNJ)
    Transparent, // T — combining marks; invisible to the scan
    RightJoining, // R — joins a preceding letter only
    LeftJoining, // L — joins a following letter only (rare)
    DualJoining, // D — joins on both sides
    JoinCausing, // C — forces a join across itself (tatweel, ZWJ)
};

// The contextual form a cursive letter resolves to.
enum class JoiningForm : std::uint8_t {
    Isolated = 0, // joins neither neighbour
    Final, // joins the preceding letter only
    Initial, // joins the following letter only
    Medial, // joins both neighbours
};

/// Which contextual form a lam-alef ligature takes.
enum class LamAlefLigatureForm : std::uint8_t {
    Isolated,
    Final
};

// Joining_Type for a codePoint. Non-Arabic-script codePoints are NonJoining
// (breaking a cursive run) except Transparent format marks and Join_Causing
// ZWJ / tatweel.
[[nodiscard]] JoiningType joiningType(std::uint32_t codePoint);

// True when codePoint is in a shaped script (Arabic / Supplement / Extended-A)
// or is a script-neutral joining control (ZWJ, ZWNJ, tatweel). Run-level gate:
// a row with no such codePoint needs no joining pass.
[[nodiscard]] bool isCursiveJoiner(std::uint32_t codePoint);

// Resolve letter `codePoint`'s form from its nearest NON-Transparent neighbours'
// joining types. Pass JoiningType::NonJoining at a run boundary. If `codePoint`
// is not a joinable letter (NonJoining / Transparent / JoinCausing) → Isolated.
[[nodiscard]] JoiningForm resolveForm(std::uint32_t codePoint, JoiningType prevType, JoiningType nextType);

// Arabic Presentation Form-B codePoint for `codePoint` in `form`, or `codePoint`
// unchanged when the letter has no presentation form for that shape.
[[nodiscard]] std::uint32_t presentationForm(std::uint32_t codePoint, JoiningForm);

// Lam-alef ligature: LAM (U+0644) followed by an alef variant forms one glyph
// with two shapes (isolated vs final). Returns the FB50-block ligature codePoint
// for the (alef, form) pair, or 0 when `alef` does not ligate with lam.
[[nodiscard]] std::uint32_t lamAlefLigature(std::uint32_t alef, LamAlefLigatureForm form);

// Shape a logical run in place to presentation forms, walking LOGICAL order
// (full UAX #9 -- embedding levels, isolates, the paragraph algorithm -- is
// separate; the minimal reordering this file's shapeVisualRow performs is not).
// A lam + ligating alef shapes as two connected
// forms, one per cell (all n cells written). `out` may alias `in` (each cell's
// neighbours are read before it is overwritten). CodePoints outside the cursive
// scripts pass through and act as run breakers.
void shapeRun(const std::uint32_t* in, std::uint32_t* outputBuffer, int n);

// True when codePoint is strong right-to-left for the minimal BiDi: the Arabic
// blocks (incl. Supplement / Extended-A and the Presentation Form A/B blocks
// shapeRun emits) and Hebrew. Strong-direction probe only; neutrals/numbers are
// resolved by shapeVisualRow's run logic, not here.
[[nodiscard]] bool isStrongRTL(std::uint32_t codePoint);

// Inlined superset gate: true when codePoint might need cursive shaping or RTL
// reordering. A superset refined by shapeVisualRow — widening it only
// risks a wasted shaping pass, never a missed shape. Keep ⊇ isCursiveJoiner ∪
// isStrongRTL.
[[nodiscard]] inline bool mayNeedShaping(std::uint32_t codePoint)
{
    // Nothing below U+0590 shapes (Hebrew is the lowest block; ZWNJ/ZWJ at
    // U+200C/D are above the cut).
    if (codePoint < 0x0590)
        return false;
    return codePoint <= 0x08FF // Hebrew + Arabic / Supplement / Extended-A
        || codePoint == 0x200C || codePoint == 0x200D // ZWNJ / ZWJ
        || (codePoint >= 0xFB1D && codePoint <= 0xFEFF); // Arabic Presentation Forms-A/B
}

// Shape one DISPLAY ROW of logical codePoints into visual paint order: shapeRun
// in logical order, then reverse each maximal contiguous strong-RTL run into
// visual right-to-left, leaving LTR/neutral runs alone. Outputs, length n,
// indexed by VISUAL column: outCodePoints[v] is the codePoint to paint at v;
// outLogical[v] is the logical cell index v came from (so the caller carries
// that cell's pen/marks/attributes). The logical store is never mutated.
// Returns true iff the row contained a cursive joiner or strong-RTL cell; on
// false the outputs are the identity row. Outputs must not alias `logical`.
[[nodiscard]] bool shapeVisualRow(const std::uint32_t* logical, std::uint32_t* outCodePoints, std::uint16_t* outLogical, int n);

} // namespace Ubiquitty::Unicode
