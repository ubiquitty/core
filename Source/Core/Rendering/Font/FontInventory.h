// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontRoute.h"
#include "Core/Rendering/FontMetrics.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::Rendering::Font {

struct FontFamilyRequest {
    std::string_view family;
    FontStyle style { FontStyle::Regular };
    std::uint8_t variableWeight { 0 };
    FontHandle sizingBase {};
    FontMetrics::FallbackSizing sizing { FontMetrics::FallbackSizing::Inherit };
};

struct GlyphProbeResult {
    FontHandle font;
    GlyphID glyph;
    bool present { false };
    bool lastResort { false };
};

/// Carries a complete cluster and presentation across the inventory callback seam.
struct ClusterProbeRequest {
    FontHandle font;
    std::span<const std::uint32_t> codePoints;
    Presentation presentation { Presentation::Default };
};

struct CascadeRequest {
    FontHandle baseFont;
    std::span<const std::uint32_t> codePoints;
    LanguageTag language;
    Presentation presentation { Presentation::Default };
    FontStyle style { FontStyle::Regular };
    std::uint8_t variableWeight { 0 };
    bool shapedRun { false };
};

struct CascadeResult {
    FontHandle font;
    GlyphID glyph;
    bool present { false };
    bool lastResort { false };
};

class FontInventory {
public:
    virtual ~FontInventory() = default;

    [[nodiscard]] virtual std::uint32_t catalogGeneration() const = 0;
    virtual void bumpCatalogGeneration() = 0;

    virtual FontHandle primaryFont(FontStyle, std::uint8_t variableWeight) = 0;
    virtual FontHandle validateFamily(const FontFamilyRequest&) = 0;
    virtual GlyphProbeResult glyphForCodePoint(FontHandle, std::uint32_t codePoint) = 0;
    /// Reports whether this face covers the whole cluster and its presentation without substitution.
    ///
    /// - Returns: The face and representative glyph when the entire cluster is covered.
    virtual GlyphProbeResult glyphForCluster(FontHandle, std::span<const std::uint32_t>, Presentation) = 0;
    virtual CascadeResult cascadeForCodePoint(const CascadeRequest&) = 0;
    virtual CascadeResult cascadeForRun(const CascadeRequest&) = 0;
    [[nodiscard]] virtual bool isLastResort(FontHandle) const = 0;
    [[nodiscard]] virtual std::string_view familyName(FontHandle) const = 0;
    [[nodiscard]] virtual std::string_view postScriptName(FontHandle) const = 0;
};

} // namespace Ubiquitty::Rendering::Font
