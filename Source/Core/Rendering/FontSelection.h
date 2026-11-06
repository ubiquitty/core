// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Ubiquitty::Rendering {

// Embedded rasterizers participate in the same family-selection contract as
// platform fonts. Frontends choose a GlyphRasterizer from this stable family
// name; no parallel boolean setting exists.
enum class FontRasterizerKind : std::uint8_t {
    Native,
    EmbeddedBitmap,
};

struct FontFamilyDescriptor {
    std::string_view family;
    FontRasterizerKind rasterizerKind;
};

inline constexpr std::array embeddedFontFamilies {
    FontFamilyDescriptor { "GNU Unifont", FontRasterizerKind::EmbeddedBitmap },
};

inline FontFamilyDescriptor selectFontFamily(std::string_view family)
{
    for (const auto& descriptor : embeddedFontFamilies) {
        if (descriptor.family == family) {
            return descriptor;
        }
    }
    return { family, FontRasterizerKind::Native };
}

} // namespace Ubiquitty::Rendering
