// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/FontMetrics.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering::FontMetrics {

double fallbackScale(const FallbackMetrics& primary, const FallbackMetrics& fallback)
{
    const double primaryMetrics[] = { primary.xHeight, primary.capHeight, primary.lineHeight };
    const double fallbackMetrics[] = { fallback.xHeight, fallback.capHeight, fallback.lineHeight };
    for (unsigned index = 0; index < 3; ++index) {
        const double target = primaryMetrics[index];
        const double source = fallbackMetrics[index];
        if (!std::isfinite(target) || !std::isfinite(source) || target <= 0 || source <= 0)
            continue;
        const double scale = target / source;
        if (std::isfinite(scale) && scale > 0)
            return std::clamp(scale, 0.5, 2.0);
    }
    return 1.0;
}

CellVerticalMetrics layoutCell(double ascent, double descent, double leading, double lineSpacing, double snapUnit)
{
    const double inkHeight = ascent + descent;
    // Leading is slack, not ink: it goes into the gap (split evenly below),
    // alongside the user's spacing and the snap remainder.
    double lineHeight = inkHeight + leading + lineSpacing;
    // A large negative lineSpacing must not collapse the cell below half the
    // ink box (glyphs would overprint their neighbours unusably).
    if (lineHeight < inkHeight * 0.5)
        lineHeight = inkHeight * 0.5;
    if (snapUnit > 0.0)
        lineHeight = std::ceil(lineHeight / snapUnit) * snapUnit;

    double gapTotal = lineHeight - inkHeight;
    if (gapTotal < 0.0)
        gapTotal = 0.0;
    // Half above, half below; the top half snaps to a whole unit so the pen
    // origin lands on a pixel boundary and the bottom half is the remainder —
    // top + bottom == gapTotal exactly, no fractional drift biasing one side.
    double gapTop = std::round(gapTotal * 0.5);
    if (gapTop < 0.0)
        gapTop = 0.0;
    if (gapTop > gapTotal)
        gapTop = gapTotal;

    const double baselineFromBottom = descent + (gapTotal - gapTop);
    return CellVerticalMetrics { lineHeight, gapTotal, gapTop, baselineFromBottom };
}

namespace {

double widestAdvance(AdvanceSpan advances)
{
    double widest = 0.0;
    for (const double advance : advances) {
        // A missing glyph advances 0; a negative one is a broken face. Neither is
        // a cell width, and letting either win would collapse the grid.
        if (advance > widest)
            widest = advance;
    }
    return widest;
}

} // namespace

double cellAdvance(FacePitch pitch, AdvanceSpan advances)
{
    if (pitch == FacePitch::Fixed)
        return widestAdvance(advances);
    constexpr std::size_t digitZero = static_cast<std::size_t>(U'0' - firstProbeCharacter);
    if (digitZero < advances.size() && advances[digitZero] > 0.0)
        return advances[digitZero];
    double sum = 0.0;
    int count = 0;
    for (char32_t letter = U'a'; letter <= U'z'; ++letter) {
        const std::size_t index = static_cast<std::size_t>(letter - firstProbeCharacter);
        if (index < advances.size() && advances[index] > 0.0) {
            sum += advances[index];
            ++count;
        }
    }
    return count > 0 ? sum / count : widestAdvance(advances);
}

bool isMonospace(AdvanceSpan advances)
{
    if (advances.empty())
        return false;
    const double reference = advances.front();
    for (const double advance : advances) {
        // A missing glyph advances 0 and a broken face reports a negative.
        // Either means the face does not draw printable ASCII, and a face that
        // does not is not a terminal face whatever its other advances do.
        if (!(advance > 0.0))
            return false;
        if (std::fabs(advance - reference) > 0.001)
            return false;
    }
    return true;
}

FacePitch facePitch(AdvanceSpan advances)
{
    double narrowest = 0.0;
    double widest = 0.0;
    for (const double advance : advances) {
        if (!(advance > 0.0))
            continue;
        if (narrowest == 0.0 || advance < narrowest)
            narrowest = advance;
        if (advance > widest)
            widest = advance;
    }
    return widest - narrowest > 0.001 ? FacePitch::Proportional : FacePitch::Fixed;
}

double trackedCellAdvance(double advance, double tracking)
{
    // A face that reported nothing keeps reporting nothing: the caller's own
    // fallback owns that case, and widening zero would hand back a cell width
    // derived from a measurement that never happened.
    if (!(advance > 0.0))
        return advance;
    const double tracked = advance + tracking;
    return tracked < 1.0 ? 1.0 : tracked;
}

} // namespace Ubiquitty::Rendering::FontMetrics
