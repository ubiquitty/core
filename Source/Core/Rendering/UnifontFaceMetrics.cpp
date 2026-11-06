// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/UnifontFaceMetrics.h"

#include <algorithm>

namespace Ubiquitty::Rendering {

FaceMetrics unifontFaceMetricsForScale(int scale, int lineSpacingInPixels)
{
    const float scaleF = static_cast<float>(scale);
    FaceMetrics face;
    face.fontSize = 16 * scale;
    face.ascent = 13.5f * scaleF;
    face.descent = 2.5f * scaleF;
    face.lineGapTopInPixels = static_cast<float>(lineSpacingInPixels / 2);
    // The ink spans from the top edge of its first row to the bottom edge of
    // its last, which is what the end row past the last one states: a one-row
    // bracket covers one row of ink, not zero.
    constexpr float baselineFromTopInRows = 13.5f;
    face.bracketInkTopInPixels = std::max(0.0f, (baselineFromTopInRows - static_cast<float>(unifontBracketFirstInkRow)) * scaleF);
    face.bracketInkBottomInPixels = std::max(0.0f, (static_cast<float>(unifontBracketInkRowEnd) - baselineFromTopInRows) * scaleF);
    return face;
}

} // namespace Ubiquitty::Rendering
