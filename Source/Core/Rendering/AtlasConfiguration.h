// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/RenderConfiguration.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Rendering {

/// The provider's resolved native font resources, before GPU storage exists.
struct FontResourceFact {
    std::string role;
    std::string path;
    std::uint64_t faceIndex = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t byteSize = 0;
    std::uint64_t modifiedSeconds = 0;
    std::uint64_t modifiedNanoseconds = 0;
};

/// Describes the resolved font catalog and routing facts that determine glyph sharing.
struct FontProviderResourceFacts {
    /// Describes one ordered language or code-point fallback rule.
    struct FallbackRuleFact {
        int script = 0;
        std::string language;
        std::string family;
        std::uint32_t firstCodePoint = 0;
        std::uint32_t lastCodePoint = 0;
        int sizing = 0;
        bool shouldYieldToEmoji = false;
    };
    std::uint64_t catalogIdentity = 0;
    std::uint64_t catalogGeneration = 0;
    double rasterPixelSize = 0;
    int deviceScale = 0;
    int lineSpacingInPixels = 0;
    int trackingInPixels = 0;
    int bitmapScale = 0;
    std::vector<FontResourceFact> faces;
    std::string cjkFallbackFamily;
    std::string symbolFallbackFamily;
    std::string emojiFallbackFamily;
    bool shouldMatchFallbackSize = true;
    std::vector<FallbackRuleFact> languageFallbacks;
    std::vector<FallbackRuleFact> rangeMaps;
    /// Only typography participates: colors, padding and other view state do not.
    RenderConfiguration configuration;
};

/// Names the immutable facts that determine one set of shared glyph sheets.
///
/// Frontends supply provider and resource facts; Core owns identity and its
/// encoding. Construction and key generation belong at configuration changes,
/// never in the per-cell path. A resource domain is an identity token, not a
/// hash, and cannot be reused while an atlas from that domain remains alive.
/// Font resources resolved under the same configuration must remain stable
/// while its atlases are shared; font-database replacement requires a provider
/// lifetime boundary outside this configuration record.
struct UBIQUITTY_NONCOPYABLE AtlasConfiguration {
    AtlasConfiguration() = default;

    std::uint64_t resourceDomain = 0;
    std::string familyName;
    double pointSize = 0;
    double pixelsPerPoint = 0;
    GlyphSlotGeometry geometry {};
    int slotCapacity = 0;
    double baselinePenInPixels = 0;
    double wideGlyphBoostLimit = 1;
    int unifontScale = 0;
    int fontWeight = 0;
    int boldFontWeight = 0;
    double fontWidthPercent = 0;
    std::string proportionalFamilyName;
    double proportionalPointSize = 0;
    std::string cjkFallbackFamily;
    std::string cjkFallbackLanguage;
    std::string symbolFamilyName;
    std::string symbolFontPath;
    std::string ligatureFeatureSpecification;
    std::string userFeatureSpecification;
    std::string userVariationSpecification;
    bool doesFontLigate = false;
    bool wantsLigatures = false;
    FontProviderResourceFacts providerResourceFacts;

    /// Native font identity using the same exact encoder as the GPU sheet key.
    /// Does not depend on a device, sheet capacity or slot layout.
    [[nodiscard]] std::string fontSharingKey() const;

    /// Encodes every fact without delimiter, rounding, or hash collisions.
    ///
    /// Numeric values retain their bits and strings retain their bytes, even
    /// across clients whose native strings compare Unicode canonically.
    /// Geometry must describe a positive mono slot with nonnegative margins,
    /// paired optional sheet extents, and finite metrics. Provider settings
    /// retain their exact requested values; their clamping is font policy.
    ///
    /// - Returns: An ASCII identity, or empty for invalid configuration facts.
    [[nodiscard]] std::string sharingKey() const;
};

}
