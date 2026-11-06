// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Base/Bits/BitField.h"

// UAX #29 extended grapheme cluster segmentation for the terminal grid:
// graphemeExtends(state, prev, next) returns true when `next` joins the cluster
// ending in `prev` (no boundary). Rules covered (tables generated from UCD 17.0.0):
//   GB6/7/8  Hangul jamo runs                — joined
//   GB9   × (Extend | ZWJ)                   — combining, VS-15/16, skin tone, ZWJ
//   GB9a  × SpacingMark / GB9b  Prepend ×    — joined
//   GB9c  Consonant [Extend|Linker]* Linker × Consonant — Indic conjuncts
//   GB11  ExtPict Extend* ZWJ × ExtPict      — ZWJ emoji sequences
//   GB12/13  RI × RI (even count)            — flag pairs
//   GB999 any ÷ any                          — default boundary
//   GB3/4/5  CR × LF; ÷ around Control       — graphemeControlDecision, asked first

namespace Ubiquitty::Unicode {

// Carried segmentation state across a single cluster's codePoints.
struct GraphemeState {
    // Consecutive Regional_Indicator count; GB12/GB13 join RI pairs only when an
    // even number precede the boundary.
    std::uint8_t riCount;
    // Cluster ends in an Extended_Pictographic base followed only by Extend/ZWJ
    // — the left side of the GB11 emoji-ZWJ rule.
    bool pictExtend;
    // Cluster's last codePoint is a ZWJ following a pictExtend run — GB11's
    // "... ZWJ ×" position where the next Extended_Pictographic joins.
    bool zwjAfterPict;
    // Property byte of the cluster's last codePoint — reads prev's GCB class
    // without a second table probe.
    std::uint8_t prevRaw;
    // GB9c: 0 the cluster cannot conjoin, 1 its base is an InCB=Consonant,
    // 2 a Linker has followed it so the next consonant joins.
    std::uint8_t incbChain;
};

// Reset state for a new cluster whose base codePoint is `base`, seeding the
// RI / pictographic carries.
void graphemeStateReset(GraphemeState* st, std::uint32_t base);

// True when `next` extends the cluster ending in `prev` (no boundary), false
// when a boundary falls between them. Advances `st`.
[[nodiscard]] bool graphemeExtends(GraphemeState* st, std::uint32_t prev, std::uint32_t next);

// Advance the GB9c conjunct chain across a joiner. Out of line and reached
// only when the chain is live, which no non-Indic cluster ever is.
void graphemeIncbAdvance(GraphemeState* st, std::uint32_t next);

namespace detail {
// The layout of every unicodeProps byte. These aliases are the single source of
// truth for the packing; nothing below re-states the bit numbers.
using PropWidthField = Ubiquitty::Bits::BitField<std::uint8_t, 0, 2>; // cell width 0/1/2, 3 East Asian Ambiguous
using PropEmojiPresentationField = Ubiquitty::Bits::BitField<std::uint8_t, 2, 1>;
using PropGCBClassField = Ubiquitty::Bits::BitField<std::uint8_t, 3, 4>;
using PropExtPictField = Ubiquitty::Bits::BitField<std::uint8_t, 7, 1>;
// grapheme-relevant == any class or ExtPict bit set (width/emoji excluded).
inline constexpr std::uint8_t propGraphemeRelevantMask = PropGCBClassField::mask | PropExtPictField::mask;

// Single full-plane property table shared by GraphemeBreak and Width, one byte
// per scalar in the Prop*Field layout above. Eagerly built once at startup (see
// GraphemeBreak.cpp), so reads are guard-free.
extern std::uint8_t unicodeProps[0x110000];
} // namespace detail

// Raw property byte for a scalar (the detail::Prop*Field layout).
[[nodiscard]] inline std::uint8_t graphemeBreakRawByte(std::uint32_t codePoint) noexcept { return codePoint < 0x110000 ? detail::unicodeProps[codePoint] : std::uint8_t(0); }

// Extended_Pictographic: emoji bases in ZWJ sequences (GB11) that a VS16/VS15
// selector can switch between emoji and text. Superset of Emoji_Presentation
// (includes text-default pictographs like U+2764). Gates the VS16 upgrade so a
// selector after a non-pictographic base does not spuriously widen the cell.
[[nodiscard]] inline bool isExtendedPictographic(std::uint32_t codePoint) noexcept
{
    return detail::PropExtPictField::get(graphemeBreakRawByte(codePoint)) != 0;
}

// Bases that form an emoji keycap when followed by (optional U+FE0F then) U+20E3:
// ASCII digits, '#', '*'. Not Extended_Pictographic, so the emit path gates the
// keycap upgrade on this + a trailing U+20E3, not on a bare VS16.
[[nodiscard]] bool isKeycapBase(std::uint32_t codePoint);

// True when codePoint participates in any UAX #29 grapheme-cluster rule. False
// for a plain base, which cannot extend a preceding cluster — the run-batch emit
// path uses this to skip the full break check.
[[nodiscard]] inline bool isGraphemeRelevant(std::uint32_t codePoint) noexcept
{
    return (graphemeBreakRawByte(codePoint) & detail::propGraphemeRelevantMask) != 0;
}

// GCB classes whose join rules are inlined by graphemeExtendsHot below.
// Pinned to GraphemeBreak.cpp's full class enum by static_asserts there.
inline constexpr std::uint8_t graphemeClassExtend = 1;
inline constexpr std::uint8_t graphemeClassZWJ = 2;

/// The Prepend class as the property table stores it.
///
/// Pinned to GraphemeBreak.cpp's class enum by a static_assert there, as the
/// two classes above are.
inline constexpr std::uint8_t graphemeClassPrepend = 4;

/// Reports whether a code point is Grapheme_Cluster_Break=Prepend.
///
/// A Prepend joins the code point after it (GB9b), so in a cluster it stands
/// before the base rather than after it.
///
/// - Parameter codePoint: The code point to classify.
/// - Returns: `true` for a Prepend.
[[nodiscard]] inline bool isGraphemePrepend(std::uint32_t codePoint) noexcept
{
    return detail::PropGCBClassField::get(graphemeBreakRawByte(codePoint)) == graphemeClassPrepend;
}

/// Returns where a cluster's base is: its first code point that is not a Prepend.
///
/// The base decides a cluster's face, width and presentation. A cluster that
/// opens with Prepend code points carries them ahead of it, so its first code
/// point is not necessarily its base.
///
/// - Parameter cluster: The cluster's code points, in order.
/// - Returns: The base's index, or 0 for an empty cluster or one of Prepends alone.
[[nodiscard]] inline std::size_t clusterBaseIndex(std::span<const std::uint32_t> cluster) noexcept
{
    for (std::size_t index = 0; index < cluster.size(); ++index) {
        if (!isGraphemePrepend(cluster[index]))
            return index;
    }
    return 0;
}

/// Reports whether a code point is Grapheme_Cluster_Break=Control here.
///
/// That means the Cc, Zl and Zp categories. The format characters UAX #29 also
/// files under Control are left out: the generated tables give each of them
/// Extend, ZWJ or Prepend, and the rules for those classes decide them.
///
/// - Parameter scalar: The code point to classify.
/// - Returns: `true` for a C0 or C1 control, DEL, U+2028 or U+2029.
[[nodiscard]] inline bool graphemeIsControl(std::uint32_t scalar) noexcept
{
    return scalar <= 0x1F // the C0 set, CR and LF among them
        || (scalar >= 0x7F && scalar <= 0x9F) // U+007F, then the C1 set
        || scalar == 0x2028 // U+2028, the one Zl character
        || scalar == 0x2029; // U+2029, the one Zp character
}

/// Settles GB3, GB4 and GB5, the rules that outrank every later one.
///
/// CR LF stays whole, and any other pair with a Control on either side splits.
/// graphemeExtendsHot and graphemeExtends both ask here before anything else; a
/// path that skipped it would let GB9 attach a mark to a control on that path
/// alone.
///
/// - Parameters:
///   - before: The last code point of the cluster so far.
///   - after: The code point that follows it.
///   - isControlPair: Set to `true` when either one is a Control, which makes
///     the returned answer final; `false` leaves the pair to the later rules.
/// - Returns: Whether `after` joins the cluster, meaningful only when
///   `isControlPair` is set.
[[nodiscard]] inline bool graphemeControlDecision(std::uint32_t before, std::uint32_t after, bool& isControlPair) noexcept
{
    isControlPair = graphemeIsControl(before) || graphemeIsControl(after);
    if (!isControlPair)
        return false;
    return before == 0x0D && after == 0x0A; // only CR followed by LF joins
}

// Inline fast path for the two dominant join rules, GB9 (× Extend | ZWJ) and
// GB11 (pictograph joining a pending "ExtPict Extend* ZWJ ×"). Everything else
// falls through to graphemeExtends with `st` untouched (this wrapper advances
// state only on its own early-true paths). Identical decisions + state to
// graphemeExtends for every input: the only rule the full cascade orders before
// GB11 is GB9a SpacingMark, and no SpacingMark is Extended_Pictographic, so the
// reordering is unobservable.
[[nodiscard]] inline bool graphemeExtendsHot(GraphemeState* st, std::uint32_t prev, std::uint32_t next) noexcept
{
    bool isControlPair = false;
    const bool shouldJoinControlPair = graphemeControlDecision(prev, next, isControlPair);
    if (isControlPair) {
        graphemeStateReset(st, next);
        return shouldJoinControlPair;
    }
    const std::uint8_t rawNext = graphemeBreakRawByte(next);
    const std::uint8_t nextClass = detail::PropGCBClassField::get(rawNext);
    if (nextClass == graphemeClassExtend || nextClass == graphemeClassZWJ) {
        if (nextClass == graphemeClassZWJ) {
            // GB11's Extend run precedes the ZWJ, so the arming must not
            // survive one after it: a diaeresis between the joiner and the next
            // pictograph means the two are separate clusters. And the pictograph
            // that armed this is consumed by it, so a second ZWJ cannot re-arm
            // from what the first already used.
            st->zwjAfterPict = st->pictExtend; // arm GB11
            st->pictExtend = false;
        } else {
            st->zwjAfterPict = false;
            if (st->incbChain != 0)
                graphemeIncbAdvance(st, next); // GB9c; ZWJ is InCB=Extend, no-op there
        }

        st->riCount = 0;
        st->prevRaw = rawNext;
        return true;
    }
    if (st->zwjAfterPict && detail::PropExtPictField::get(rawNext) != 0) {
        st->pictExtend = true;
        st->zwjAfterPict = false;
        st->riCount = 0;
        st->prevRaw = rawNext;
        return true;
    }
    return graphemeExtends(st, prev, next);
}

} // namespace Ubiquitty::Unicode
