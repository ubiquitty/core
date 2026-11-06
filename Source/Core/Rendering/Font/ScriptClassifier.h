// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontRoute.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/Width.h"

#include <cstdint>

namespace Ubiquitty::Rendering::Font {

constexpr bool isUnifiedHan(std::uint32_t codePoint)
{
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) || (codePoint >= 0x4E00 && codePoint <= 0x9FFF) || (codePoint >= 0xF900 && codePoint <= 0xFAFF) || (codePoint >= 0x20000 && codePoint <= 0x2A6DF) || (codePoint >= 0x2A700 && codePoint <= 0x2B73F)
        || (codePoint >= 0x2B740 && codePoint <= 0x2B81F) || (codePoint >= 0x2B820 && codePoint <= 0x2CEAF) || (codePoint >= 0x2CEB0 && codePoint <= 0x2EBEF) || (codePoint >= 0x30000 && codePoint <= 0x323AF);
}

constexpr bool isKana(std::uint32_t codePoint) { return (codePoint >= 0x3040 && codePoint <= 0x30FF) || (codePoint >= 0x31F0 && codePoint <= 0x31FF) || (codePoint >= 0xFF66 && codePoint <= 0xFF9F); }

constexpr bool isHangul(std::uint32_t codePoint) { return (codePoint >= 0x1100 && codePoint <= 0x11FF) || (codePoint >= 0x3130 && codePoint <= 0x318F) || (codePoint >= 0xA960 && codePoint <= 0xA97F) || (codePoint >= 0xAC00 && codePoint <= 0xD7AF) || (codePoint >= 0xD7B0 && codePoint <= 0xD7FF); }

/// Returns whether a code point should be shaped as emoji.
///
/// Reads the Emoji_Presentation bit from `Unicode::Width::classify`, the same
/// flat-table SSOT `Attribute::Emoji` reads, so the shaper and the cell
/// attribute cannot disagree about what a code point is.
///
/// VS16 (U+FE0F) is special-cased: it is never Emoji_Presentation=Yes, but a
/// cluster it is attached to has to shape as one run, so it answers true here.
///
/// - Parameter codePoint: The code point to classify.
/// - Returns: True when the code point shapes as emoji.
inline bool isEmoji(std::uint32_t codePoint) { return codePoint == 0xFE0F || (Ubiquitty::Unicode::Width::classify(codePoint) & 0x04) != 0; }

inline bool isEmojiClusterCodePoint(std::uint32_t codePoint) { return isEmoji(codePoint) || codePoint == 0x200D || (codePoint >= 0x1F1E6 && codePoint <= 0x1F1FF) || (codePoint >= 0x1F3FB && codePoint <= 0x1F3FF) || codePoint == 0xFE0F; }

/// Derives natural cluster presentation when no stored terminal presentation is available.
///
/// Pictographic selectors apply in input order, and a keycap requires its
/// enclosing mark. A caller holding the cell's presentation uses that fact
/// instead, because SGR can override the cluster's natural presentation.
[[nodiscard]] inline Presentation presentationForCluster(std::span<const std::uint32_t> codePoints)
{
    if (codePoints.empty()) {
        return Presentation::Default;
    }
    const auto lead = codePoints.front();
    Presentation presentation = Unicode::Width::isEmojiPresentation(lead) ? Presentation::Emoji : Presentation::Default;
    const bool isPictographicBase = Unicode::isExtendedPictographic(lead);
    const bool isKeycapBase = Unicode::isKeycapBase(lead);
    for (auto codePoint : codePoints.subspan(1)) {
        if (isPictographicBase && codePoint == Unicode::Width::variationSelector15) {
            presentation = Presentation::Text;
        } else if ((isPictographicBase && codePoint == Unicode::Width::variationSelector16) || (isKeycapBase && codePoint == 0x20E3)) {
            presentation = Presentation::Emoji;
        }
    }
    return presentation;
}

constexpr bool scriptRequiresShapedRun(ScriptClass script)
{
    return script == ScriptClass::Arabic || script == ScriptClass::Hebrew || script == ScriptClass::Devanagari || script == ScriptClass::Bengali || script == ScriptClass::Gurmukhi || script == ScriptClass::Thai || script == ScriptClass::Lao || script == ScriptClass::Khmer
        || script == ScriptClass::Myanmar;
}

/// Whether a code point lies in a Unicode private-use area: the BMP block and
/// planes 15 and 16.
///
/// - Parameter codePoint: The character to classify.
/// - Returns: True for a private-use code point.
constexpr bool isPrivateUse(std::uint32_t codePoint)
{
    return (codePoint >= 0xE000 && codePoint <= 0xF8FF) || (codePoint >= 0xF0000 && codePoint <= 0xFFFFD) || (codePoint >= 0x100000 && codePoint <= 0x10FFFD);
}

constexpr bool isSymbol(std::uint32_t codePoint) { return (codePoint >= 0x2500 && codePoint <= 0x259F) || isPrivateUse(codePoint) || (codePoint >= 0xE0A0 && codePoint <= 0xE0D7) || (codePoint >= 0x2300 && codePoint <= 0x23FF) || (codePoint >= 0x2B00 && codePoint <= 0x2BFF); }

constexpr bool isMath(std::uint32_t codePoint) { return (codePoint >= 0x2200 && codePoint <= 0x22FF) || (codePoint >= 0x27C0 && codePoint <= 0x27EF) || (codePoint >= 0x2980 && codePoint <= 0x29FF) || (codePoint >= 0x2A00 && codePoint <= 0x2AFF) || (codePoint >= 0x1D400 && codePoint <= 0x1D7FF); }

// Not constexpr: isEmoji() reads the runtime-populated flat property table.
inline ScriptClass classifyScript(std::uint32_t codePoint)
{
    // Fast path: ASCII + Latin-1/Extended (<= 0x024F) is the majority of cells
    // and cannot be Han/Kana/Hangul (all >= 0x1100) or any later script, so
    // short-circuit before those range checks.
    if (codePoint <= 0x024F)
        return ScriptClass::Latin;
    if (isUnifiedHan(codePoint))
        return ScriptClass::Han;
    if (isKana(codePoint))
        return ScriptClass::Kana;
    if (isHangul(codePoint))
        return ScriptClass::Hangul;
    if ((codePoint >= 0x0370 && codePoint <= 0x03FF) || (codePoint >= 0x1F00 && codePoint <= 0x1FFF))
        return ScriptClass::Greek;
    if ((codePoint >= 0x0400 && codePoint <= 0x052F) || (codePoint >= 0x2DE0 && codePoint <= 0x2DFF) || (codePoint >= 0xA640 && codePoint <= 0xA69F))
        return ScriptClass::Cyrillic;
    if ((codePoint >= 0x0600 && codePoint <= 0x06FF) || (codePoint >= 0x0750 && codePoint <= 0x077F) || (codePoint >= 0x08A0 && codePoint <= 0x08FF))
        return ScriptClass::Arabic;
    if (codePoint >= 0x0590 && codePoint <= 0x05FF)
        return ScriptClass::Hebrew;
    if (codePoint >= 0x0900 && codePoint <= 0x097F)
        return ScriptClass::Devanagari;
    if (codePoint >= 0x0980 && codePoint <= 0x09FF)
        return ScriptClass::Bengali;
    if (codePoint >= 0x0A00 && codePoint <= 0x0A7F)
        return ScriptClass::Gurmukhi;
    if (codePoint >= 0x0E00 && codePoint <= 0x0E7F)
        return ScriptClass::Thai;
    if (codePoint >= 0x0E80 && codePoint <= 0x0EFF)
        return ScriptClass::Lao;
    if (codePoint >= 0x1780 && codePoint <= 0x17FF)
        return ScriptClass::Khmer;
    if (codePoint >= 0x1000 && codePoint <= 0x109F)
        return ScriptClass::Myanmar;
    if (isEmoji(codePoint))
        return ScriptClass::Emoji;
    if (isMath(codePoint))
        return ScriptClass::Math;
    if (isSymbol(codePoint))
        return ScriptClass::Symbols;
    return ScriptClass::Other;
}

inline bool codePointRequiresShapedRun(std::uint32_t codePoint)
{
    // Fast path: the Latin range (<= 0x024F) never needs a shaped run and is
    // never part of an emoji cluster (ZWJ, regional-indicator/skin-tone/VS16, and
    // every Emoji_Presentation codepoint all live above 0x024F), so skip both the
    // range walk and the table lookup on ASCII-dominant content.
    if (codePoint <= 0x024F)
        return false;
    return scriptRequiresShapedRun(classifyScript(codePoint)) || isEmojiClusterCodePoint(codePoint);
}

inline bool clusterRequiresShapedRun(std::span<const std::uint32_t> codePoints)
{
    if (codePoints.empty())
        return false;
    if (codePoints.size() > 1)
        return true;
    return codePointRequiresShapedRun(codePoints[0]);
}

} // namespace Ubiquitty::Rendering::Font
