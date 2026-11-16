// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Unicode/Width.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace Ubiquitty::Rendering {

/// Reports whether a complete cluster belongs to a complex syllabic script.
///
/// The base after Prepend characters chooses the script. At least two visible
/// code points are required; joiners and variation selectors alone do not make
/// a cluster eligible. Width and pictographic guards preserve native wide text.
/// This presentation policy follows selected Brahmic script blocks in
/// Unicode 18.0 Scripts.txt and Blocks.txt.
/// - Parameter codePoints: The complete cluster in logical order.
/// - Returns: Whether advance overflow may fit the complete shaped cluster.
[[nodiscard]] inline bool clusterHorizontalFitEligible(std::span<const std::uint32_t> codePoints)
{
    if (codePoints.size() < 2)
        return false;
    const auto baseIndex = Unicode::clusterBaseIndex(codePoints);
    const auto base = codePoints[baseIndex];
    if (Unicode::isGraphemePrepend(base) || Unicode::Width::forCodePoint(base, Unicode::Width::AmbiguousWidth::Narrow) != 1)
        return false;
    unsigned visibleCount = 0;
    for (const auto codePoint : codePoints.subspan(baseIndex)) {
        if (Unicode::isGraphemePrepend(codePoint) || Unicode::Width::isInvisibleFormat(codePoint))
            continue;
        if (Unicode::isExtendedPictographic(codePoint) || Unicode::Width::forCodePoint(codePoint, Unicode::Width::AmbiguousWidth::Narrow) > 1)
            return false;
        ++visibleCount;
    }
    if (visibleCount < 2)
        return false;
    struct ScriptBlock {
        std::uint32_t first;
        std::uint32_t last;
    };
    constexpr std::array blocks {
        ScriptBlock { 0x0900, 0x109f }, // Devanagari through Myanmar, including Sinhala, Thai, Lao, and Tibetan.
        ScriptBlock { 0x1700, 0x17ff }, // Philippine Brahmic scripts and Khmer.
        ScriptBlock { 0x1900, 0x19df }, // Limbu, Tai Le, and New Tai Lue.
        ScriptBlock { 0x1a00, 0x1aaf }, // Buginese and Tai Tham.
        ScriptBlock { 0x1b00, 0x1c4f }, // Balinese, Sundanese, Batak, and Lepcha.
        ScriptBlock { 0xa800, 0xa82f }, // Syloti Nagri.
        ScriptBlock { 0xa880, 0xa8ff }, // Saurashtra and Devanagari Extended.
        ScriptBlock { 0xa900, 0xa95f }, // Kayah Li and Rejang.
        ScriptBlock { 0xa980, 0xa9ff }, // Javanese and Myanmar Extended-B.
        ScriptBlock { 0xaa00, 0xaaff }, // Cham, Myanmar Extended-A, Tai Viet, and Meetei Mayek Extensions.
        ScriptBlock { 0xabc0, 0xabff }, // Meetei Mayek.
        ScriptBlock { 0x11000, 0x110cf }, // Brahmi and Kaithi.
        ScriptBlock { 0x11100, 0x1114f }, // Chakma.
        ScriptBlock { 0x11180, 0x111df }, // Sharada.
        ScriptBlock { 0x11200, 0x1124f }, // Khojki.
        ScriptBlock { 0x11280, 0x112ff }, // Multani and Khudawadi.
        ScriptBlock { 0x11300, 0x114df }, // Grantha, Tulu-Tigalari, Newa, and Tirhuta.
        ScriptBlock { 0x11580, 0x1165f }, // Siddham and Modi.
        ScriptBlock { 0x11680, 0x1174f }, // Takri, Myanmar Extended-C, and Ahom.
        ScriptBlock { 0x11800, 0x1184f }, // Dogra.
        ScriptBlock { 0x11900, 0x1195f }, // Dives Akuru.
        ScriptBlock { 0x119a0, 0x11aaf }, // Nandinagari, Zanabazar Square, and Soyombo.
        ScriptBlock { 0x11b00, 0x11b5f }, // Devanagari Extended-A.
        ScriptBlock { 0x11c00, 0x11cbf }, // Bhaiksuki and Marchen.
        ScriptBlock { 0x11d00, 0x11daf }, // Masaram Gondi and Gunjala Gondi.
        ScriptBlock { 0x11f00, 0x11f5f }, // Kawi.
    };
    for (const auto block : blocks) {
        if (base >= block.first && base <= block.last)
            return true;
    }
    return false;
}

/// Reports eligibility for a borrowed Swift cluster buffer.
[[nodiscard]] inline bool clusterHorizontalFitEligible(const std::uint32_t* codePoints, int count)
{
    return codePoints && count > 0 && clusterHorizontalFitEligible(std::span(codePoints, static_cast<std::size_t>(count)));
}

/// The horizontal cell allocation inside a padded coverage slot, in pixels.
struct ClusterRasterBounds {
    double left { 0 };
    double width { 0 };
};

/// One horizontal transform applied to every glyph in a shaped cluster.
struct ClusterHorizontalFit {
    double scaleX { 1 };
    double translationX { 0 };
};

/// Fits the union of a cluster's ink and advance without enlarging it.
///
/// - Parameters:
///   - sourceLeft: The measured interval's left edge in slot coordinates.
///   - sourceRight: The measured interval's right edge in slot coordinates.
///   - target: The allocated cell interval, excluding atlas padding.
/// - Returns: Identity when contained or invalid; otherwise a translation and
///   optional horizontal contraction. Vertical positions remain unchanged.
[[nodiscard]] inline ClusterHorizontalFit fitClusterHorizontal(double sourceLeft, double sourceRight, ClusterRasterBounds target)
{
    const double targetRight = target.left + target.width;
    const double sourceWidth = sourceRight - sourceLeft;
    if (!std::isfinite(sourceLeft) || !std::isfinite(sourceRight) || !std::isfinite(sourceWidth)
        || !std::isfinite(target.left) || !std::isfinite(target.width) || !std::isfinite(targetRight)
        || sourceWidth < 0 || target.width <= 0)
        return {};
    if (sourceLeft >= target.left && sourceRight <= targetRight)
        return {};
    if (sourceWidth <= target.width) {
        const double translation = sourceLeft < target.left ? target.left - sourceLeft : targetRight - sourceRight;
        return std::isfinite(translation) ? ClusterHorizontalFit { 1, translation } : ClusterHorizontalFit {};
    }
    const double scale = target.width / sourceWidth;
    const double translation = target.left - sourceLeft * scale;
    return scale > 0 && std::isfinite(translation) ? ClusterHorizontalFit { scale, translation } : ClusterHorizontalFit {};
}

} // namespace Ubiquitty::Rendering
