// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontFallbackCache.h"
#include "Core/Rendering/Font/FontInventory.h"
#include "Core/Rendering/Font/ScriptClassifier.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::Rendering::Font {

struct RangeMapPreference {
    std::uint32_t firstCodePoint { 0 };
    std::uint32_t lastCodePoint { 0 };
    std::string_view family;
    FontMetrics::FallbackSizing sizing { FontMetrics::FallbackSizing::Inherit };
    /// Whether a request for emoji presentation passes this range by, so the
    /// emoji stage still answers `▶️` while the range answers `▶`. Set on the
    /// built-in ranges, which were never asked to draw anything in colour.
    bool shouldYieldToEmoji { false };
};

/// The bundled face the geometric symbols fall back to.
///
/// Monaspace Neon draws every one of them: the circles, squares, triangles and
/// half discs status lines and spinners print, which many primary faces lack
/// in part. Without a range the gaps go to whatever system face the symbol
/// stages reach, and a line that mixes a primary face's `●` with another
/// face's `⏺` or `◯` no longer agrees with itself.
inline constexpr std::string_view bundledGeometricSymbolFamily = "Monaspace Neon";

/// The ranges every configuration routes after the reader's own
/// `[[font.range_map]]` entries, which come first and so override them.
///
/// Each is consulted only after the primary face misses, so a face that draws
/// these keeps drawing them; and each yields to emoji presentation. A family
/// the platform does not carry, or a code point it lacks, falls through to the
/// stages after the range maps as if no range had named it.
inline constexpr std::array<RangeMapPreference, 3> defaultRangeMaps { {
    // ⏩..⏺: the media controls, the record dot among them.
    { 0x23E9, 0x23FA, bundledGeometricSymbolFamily, FontMetrics::FallbackSizing::Inherit, true },
    // Geometric Shapes: ■ □ ▪ ▶ ◆ ◇ ○ ◎ ● ◐ ◑ ◒ ◓ ◯ and the rest of the block.
    { 0x25A0, 0x25FF, bundledGeometricSymbolFamily, FontMetrics::FallbackSizing::Inherit, true },
    // The large squares, circles, diamonds and ellipses of Miscellaneous
    // Symbols and Arrows, ⬤ among them.
    { 0x2B12, 0x2B2F, bundledGeometricSymbolFamily, FontMetrics::FallbackSizing::Inherit, true },
} };

struct LanguageFallbackPreference {
    ScriptClass script { ScriptClass::Other };
    LanguageTag language;
    std::string_view family;
};

/// Everything the route consults beyond the request itself, as borrowed views.
///
/// Every field is a constructor parameter and there is no default constructor,
/// so a caller cannot name the two fields it remembers and leave the rest
/// empty: an omitted range map is not an empty route stage, it is a stage the
/// policy skips entirely. A field added here breaks every construction site at
/// compile time, which is the only way a frontend cannot go on reading a
/// preference the configuration parses and validates.
///
/// The views borrow. `Preferences::FontRoutePreferences` owns the storage and
/// builds one of these from a configuration; frontends should route through it
/// rather than assemble their own, so the mapping from configuration to route
/// lives in one place.
struct FontFallbackPreferences {
    FontFallbackPreferences() = delete;

    /// - Parameters:
    ///   - rangeMaps: Ordered per-range families, consulted after a primary miss.
    ///   - languageFallbacks: Per-script/language families.
    ///   - cjkFallbackFamily: The family unified Han routes to.
    ///   - symbolFallbackFamily: The family symbols route to.
    ///   - emojiFallbackFamily: The family pictographs route to.
    ///   - metricsGeneration: The generation resolved routes are cached under.
    constexpr FontFallbackPreferences(
        std::span<const RangeMapPreference> rangeMaps,
        std::span<const LanguageFallbackPreference> languageFallbacks,
        std::string_view cjkFallbackFamily,
        std::string_view symbolFallbackFamily,
        std::string_view emojiFallbackFamily,
        std::uint32_t metricsGeneration)
        : rangeMaps(rangeMaps)
        , languageFallbacks(languageFallbacks)
        , cjkFallbackFamily(cjkFallbackFamily)
        , symbolFallbackFamily(symbolFallbackFamily)
        , emojiFallbackFamily(emojiFallbackFamily)
        , metricsGeneration(metricsGeneration)
    {
    }

    /// The set that configures nothing: every stage falls through to the
    /// native cascade.
    ///
    /// Named rather than spelled as six blanks at each call, and safe in a way
    /// a default constructor is not: it says the caller configured nothing on
    /// purpose, so a field added later leaves it correct, where a caller that
    /// filled in the fields it remembered would be wrong and silent.
    ///
    /// - Returns: Preferences naming no family and carrying no override.
    [[nodiscard]] static constexpr FontFallbackPreferences none()
    {
        return FontFallbackPreferences({}, {}, {}, {}, {}, 0);
    }

    std::span<const RangeMapPreference> rangeMaps;
    std::span<const LanguageFallbackPreference> languageFallbacks;
    std::string_view cjkFallbackFamily;
    std::string_view symbolFallbackFamily;
    std::string_view emojiFallbackFamily;
    std::uint32_t metricsGeneration { 0 };
};

struct FontFallbackPolicyCaches {
    HanFallbackCache& han;
    ScriptFallbackCache& script;
    GenericFallbackCache& generic;
    EmojiFallbackCache& emoji;
    SymbolFallbackCache& symbol;
    RangeMapCache& rangeMap;
    NegativeRouteCache& negative;
};

class FontFallbackPolicy {
public:
    static constexpr std::string_view defaultSymbolFallbackFamily = "Symbols Nerd Font Mono";
    static constexpr std::string_view defaultTechnicalSymbolFamily = "STIX Two Math";

    FontFallbackPolicy(FontInventory& inventory, FontFallbackPolicyCaches caches)
        : m_inventory(inventory)
        , m_caches(caches)
    {
    }

    GlyphRoute route(const FontRequest& request, const FontFallbackPreferences& preferences)
    {
        FontHandle primary = m_inventory.primaryFont(request.style, request.variableWeight);
        GlyphRoute primaryRoute = routeFromProbe(request, FontSource::Primary, primary, probeFont(primary, request));
        if (!isMissing(primaryRoute))
            return primaryRoute;

        // A character the whole ladder already refused. Asked before the rest of
        // it runs, because the rest is what the answer cost: a primary probe, a
        // range-map scan, two cache lookups and a native cascade call, and a
        // page of a script nobody installed pays that for every distinct
        // character on it, every frame it is on screen.
        FontFallbackCacheEntry negativeEntry;
        // A scalar miss says nothing about another cluster sharing its lead.
        const bool canCacheNegative = request.codePoints.size() == 1;
        if (canCacheNegative && m_caches.negative.lookup(negativeKey(request, primary, preferences), negativeEntry)) {
            m_caches.negative.recordNegativeHit();
            return missingRoute(request);
        }

        GlyphRoute rangeRoute = routeRangeMap(request, preferences, primary);
        if (!isMissing(rangeRoute))
            return rangeRoute;

        if (request.script == ScriptClass::Han) {
            GlyphRoute hanRoute = routeHan(request, preferences, primary);
            if (!isMissing(hanRoute))
                return hanRoute;
            return missingRoute(request);
        }

        if (request.script == ScriptClass::Symbols) {
            GlyphRoute symbolRoute = routeNamedFamily(request, preferences.symbolFallbackFamily, FontSource::SymbolFallback, m_caches.symbol, primary, preferences);
            if (!isMissing(symbolRoute))
                return symbolRoute;
            if (isPrivateUse(leadCodePoint(request))) {
                GlyphRoute bundledRoute = routePrivateUse(request, preferences, primary);
                if (!isMissing(bundledRoute))
                    return bundledRoute;
                if (canCacheNegative)
                    m_caches.negative.store({ negativeKey(request, primary, preferences), {}, FontSource::Missing, false });
                return missingRoute(request);
            }
            if (preferences.symbolFallbackFamily != defaultTechnicalSymbolFamily) {
                GlyphRoute technicalRoute = routeNamedFamily(request, defaultTechnicalSymbolFamily, FontSource::SymbolFallback, m_caches.symbol, primary, preferences);
                if (!isMissing(technicalRoute))
                    return technicalRoute;
            }
        }

        if (wantsEmoji(request)) {
            GlyphRoute emojiRoute = routeNamedFamily(request, preferences.emojiFallbackFamily, FontSource::EmojiCascade, m_caches.emoji, primary, preferences);
            if (!isMissing(emojiRoute))
                return emojiRoute;
            GlyphRoute emojiCascade = routeNativeCascade(request, FontSource::EmojiCascade, m_caches.emoji, primary, preferences);
            if (!isMissing(emojiCascade))
                return emojiCascade;
        }

        GlyphRoute scriptRoute = routeScriptFallback(request, preferences, primary);
        if (!isMissing(scriptRoute))
            return scriptRoute;

        GlyphRoute genericRoute = routeFromCache(request, FontSource::GenericCache, m_caches.generic, primary, preferences);
        if (!isMissing(genericRoute))
            return genericRoute;

        GlyphRoute nativeRoute = routeNativeCascade(request, FontSource::NativeCascade, m_caches.generic, primary, preferences);
        if (!isMissing(nativeRoute))
            return nativeRoute;

        if (canCacheNegative) {
            m_caches.negative.store({ negativeKey(request, primary, preferences), {}, FontSource::Missing, false });
        }
        return missingRoute(request);
    }

private:
    /// Resolves whether the request requires emoji presentation.
    ///
    /// - Parameter request: The cluster and its presentation override.
    /// - Returns: Whether the explicit or default presentation is emoji.
    static bool wantsEmoji(const FontRequest& request)
    {
        return request.presentation == Presentation::Emoji || (request.presentation == Presentation::Default && request.script == ScriptClass::Emoji);
    }

    /// Checks the face against the complete requested cluster when necessary.
    ///
    /// - Parameters:
    ///   - font: The candidate face.
    ///   - request: The cluster and presentation to cover.
    /// - Returns: The coverage result without native font substitution.
    GlyphProbeResult probeFont(FontHandle font, const FontRequest& request)
    {
        if (request.codePoints.size() > 1 || wantsEmoji(request) || (request.presentation == Presentation::Text && request.script == ScriptClass::Emoji)) {
            return m_inventory.glyphForCluster(font, request.codePoints, wantsEmoji(request) ? Presentation::Emoji : request.presentation);
        }
        return m_inventory.glyphForCodePoint(font, leadCodePoint(request));
    }

    static std::uint32_t leadCodePoint(const FontRequest& request)
    {
        if (!request.codePoints.empty())
            return request.codePoints[0];
        return 0;
    }

    static std::uint32_t languageHash(LanguageTag language)
    {
        if (language.value.empty())
            return 0;
        std::uint32_t hash = 2166136261u;
        for (char ch : language.value) {
            hash ^= static_cast<std::uint8_t>(ch);
            hash *= 16777619u;
        }
        return hash;
    }

    static std::uint32_t hashString(std::string_view value)
    {
        std::uint32_t hash = 2166136261u;
        for (char ch : value) {
            hash ^= static_cast<std::uint8_t>(ch);
            hash *= 16777619u;
        }
        return value.empty() ? 0 : hash;
    }

    static RouteFlags routeFlags(const FontRequest& request, FontSource source)
    {
        RouteFlags flags = 0;
        if (request.width == CellWidthClass::Wide)
            flags = flags | RouteFlag::WideAtlas;
        else
            flags = flags | RouteFlag::MonoAtlas;
        if (wantsEmoji(request))
            flags = flags | RouteFlag::ColorAtlas;
        if (clusterRequiresShapedRun(request.codePoints))
            flags = flags | RouteFlag::ShapedRun;
        if (source == FontSource::SymbolFallback)
            flags = flags | RouteFlag::SymbolPath;
        if (source == FontSource::Missing)
            flags = flags | RouteFlag::MissingGlyph;
        return flags;
    }

    static FontFallbackCacheKey cacheKey(const FontRequest& request, FontSource source, FontHandle primary, const FontFallbackPreferences& preferences)
    {
        FontFallbackCacheKey key;
        key.primaryFont = primary;
        key.metricsGeneration = preferences.metricsGeneration;
        key.preferenceGeneration = request.preferenceGeneration;
        key.catalogGeneration = primary.generation;
        key.languageHash = languageHash(request.language);
        key.script = request.script;
        key.presentation = request.presentation;
        key.style = request.style;
        key.variableWeight = request.variableWeight;
        key.routeMode = source;
        return key;
    }

    /// The key one character's negative route lives under.
    ///
    /// - Parameter request: The request being routed.
    /// - Parameter primary: The primary face, which the key is scoped to.
    /// - Parameter preferences: The preferences, for their generations.
    /// - Returns: The shared key narrowed to this request's lead character.
    static FontFallbackCacheKey negativeKey(const FontRequest& request, FontHandle primary, const FontFallbackPreferences& preferences)
    {
        return withCodePoint(cacheKey(request, FontSource::Missing, primary, preferences), request);
    }

    static FontFallbackCacheKey withFamily(FontFallbackCacheKey key, std::string_view family)
    {
        key.familyHash = hashString(family);
        return key;
    }

    /// The key a negative route is stored and found under.
    ///
    /// The character rather than its class, for the reason
    /// ``FontFallbackCacheKey::codePoint`` gives: a negative is about one
    /// character, and a class-grained one would answer for every other
    /// character sharing it.
    ///
    /// - Parameter key: The key the shared derivation produced.
    /// - Parameter request: The request whose lead character is the subject.
    /// - Returns: The key, narrowed to that character.
    static FontFallbackCacheKey withCodePoint(FontFallbackCacheKey key, const FontRequest& request)
    {
        key.codePoint = leadCodePoint(request);
        return key;
    }

    GlyphRoute routeFromProbe(const FontRequest& request, FontSource source, FontHandle font, GlyphProbeResult probe)
    {
        if (!font.isValid() || !probe.present || probe.lastResort) {
            if (probe.lastResort)
                m_caches.negative.recordLastResortReject();
            return {};
        }
        return { source, font, probe.glyph, routeFlags(request, source), request.script, request.width };
    }

    GlyphRoute routeFromCache(const FontRequest& request, FontSource source, FontFallbackCache& cache, FontHandle primary, const FontFallbackPreferences& preferences)
    {
        FontFallbackCacheEntry entry;
        if (!cache.lookup(cacheKey(request, source, primary, preferences), entry))
            return {};
        return routeFromProbe(request, source, entry.font, probeFont(entry.font, request));
    }

    GlyphRoute routeNativeCascade(const FontRequest& request, FontSource source, FontFallbackCache& cache, FontHandle primary, const FontFallbackPreferences& preferences)
    {
        FontFallbackCacheKey key = cacheKey(request, source, primary, preferences);
        FontFallbackCacheEntry entry;
        if (cache.lookup(key, entry)) {
            GlyphRoute cachedRoute = routeFromProbe(request, source, entry.font, probeFont(entry.font, request));
            if (cachedRoute)
                return cachedRoute;
        }

        cache.recordNativeCascadeCall();
        CascadeRequest cascade;
        cascade.baseFont = primary;
        cascade.codePoints = request.codePoints;
        cascade.language = request.language;
        cascade.presentation = wantsEmoji(request) ? Presentation::Emoji : request.presentation;
        cascade.style = request.style;
        cascade.variableWeight = request.variableWeight;
        cascade.shapedRun = clusterRequiresShapedRun(request.codePoints);
        CascadeResult result = request.codePoints.size() > 1 ? m_inventory.cascadeForRun(cascade) : m_inventory.cascadeForCodePoint(cascade);
        if (!result.present || result.lastResort) {
            if (result.lastResort)
                m_caches.negative.recordLastResortReject();
            return {};
        }
        cache.store({ key, result.font, source, false });
        return { source, result.font, result.glyph, routeFlags(request, source), request.script, request.width };
    }

    GlyphRoute routeNamedFamily(const FontRequest& request, std::string_view family, FontSource source, FontFallbackCache& cache, FontHandle primary, const FontFallbackPreferences& preferences, FontMetrics::FallbackSizing sizing = FontMetrics::FallbackSizing::Inherit)
    {
        if (family.empty())
            return {};
        // Private-use icon fonts are designed against the terminal cell, not
        // a text x-height. An explicit range choice can still resize them.
        if (sizing == FontMetrics::FallbackSizing::Inherit && isPrivateUse(leadCodePoint(request)))
            sizing = FontMetrics::FallbackSizing::Nominal;
        FontFallbackCacheKey key = withFamily(cacheKey(request, source, primary, preferences), family);
        key.sizing = sizing;
        FontFallbackCacheEntry entry;
        if (cache.lookup(key, entry)) {
            GlyphRoute cachedRoute = routeFromProbe(request, source, entry.font, probeFont(entry.font, request));
            if (cachedRoute)
                return cachedRoute;
        }
        FontHandle font = m_inventory.validateFamily({ family, request.style, request.variableWeight, primary, sizing });
        if (!font.isValid())
            return {};
        GlyphRoute route = routeFromProbe(request, source, font, probeFont(font, request));
        if (route.source != FontSource::Missing)
            cache.store({ key, route.font, source, false });
        return route;
    }

    /// Routes a private-use character the configured symbol family lacks.
    ///
    /// A private-use code point means whatever the face carrying it says, so
    /// the bundled symbol family, which defines the icon sets terminal programs
    /// print, answers for one whatever symbol family is configured, and no
    /// other face does: a system cascade ranked by coverage hands the cell to
    /// whichever installed face fills that code point with something else. A
    /// symbol family naming the primary family asks for the primary's own
    /// tofu, and gets it.
    ///
    /// - Parameters:
    ///   - request: The private-use cluster.
    ///   - preferences: The configured routing preferences.
    ///   - primary: The primary face for the request's style.
    /// - Returns: The bundled symbol family's route, or a missing route.
    GlyphRoute routePrivateUse(const FontRequest& request, const FontFallbackPreferences& preferences, FontHandle primary)
    {
        const std::string_view primaryFamily = m_inventory.familyName(primary);
        if (equalsIgnoringCase(preferences.symbolFallbackFamily, defaultSymbolFallbackFamily)
            || (!preferences.symbolFallbackFamily.empty() && equalsIgnoringCase(preferences.symbolFallbackFamily, primaryFamily)))
            return {};
        return routeNamedFamily(request, defaultSymbolFallbackFamily, FontSource::SymbolFallback, m_caches.symbol, primary, preferences);
    }

    /// Compares two family names as the inventories match them, ASCII
    /// letters without regard to case.
    ///
    /// - Parameters:
    ///   - left: One family name.
    ///   - right: The other.
    /// - Returns: Whether the names match.
    static bool equalsIgnoringCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto lower = [](char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character; };
            if (lower(left[index]) != lower(right[index]))
                return false;
        }
        return true;
    }

    GlyphRoute routeRangeMap(const FontRequest& request, const FontFallbackPreferences& preferences, FontHandle primary)
    {
        std::uint32_t lead = leadCodePoint(request);
        for (const RangeMapPreference& range : preferences.rangeMaps) {
            if (lead < range.firstCodePoint || lead > range.lastCodePoint || (range.shouldYieldToEmoji && wantsEmoji(request)))
                continue;
            return routeNamedFamily(request, range.family, FontSource::RangeMap, m_caches.rangeMap, primary, preferences, range.sizing);
        }
        return {};
    }

    GlyphRoute routeHan(const FontRequest& request, const FontFallbackPreferences& preferences, FontHandle primary)
    {
        if (!request.language.value.empty()) {
            for (const LanguageFallbackPreference& preference : preferences.languageFallbacks) {
                if (preference.script != ScriptClass::Han || !languagesMatch(preference.language, request.language)) {
                    continue;
                }
                GlyphRoute explicitRoute = routeNamedFamily(request, preference.family, FontSource::ExplicitHan, m_caches.han, primary, preferences);
                if (!isMissing(explicitRoute)) {
                    return explicitRoute;
                }
            }
        }
        for (const LanguageFallbackPreference& preference : preferences.languageFallbacks) {
            if (preference.script == ScriptClass::Han && (preference.language.value.empty() || preference.language.value == "*")) {
                GlyphRoute explicitRoute = routeNamedFamily(request, preference.family, FontSource::ExplicitHan, m_caches.han, primary, preferences);
                if (!isMissing(explicitRoute))
                    return explicitRoute;
            }
        }
        GlyphRoute cjkRoute = routeNamedFamily(request, preferences.cjkFallbackFamily, FontSource::ExplicitHan, m_caches.han, primary, preferences);
        if (!isMissing(cjkRoute))
            return cjkRoute;
        return routeNativeCascade(request, FontSource::AutoHanCascade, m_caches.han, primary, preferences);
    }

    /// Compares complete language tags without ASCII case sensitivity.
    ///
    /// - Parameters:
    ///   - preference: The configured language tag.
    ///   - requested: The requested language tag.
    /// - Returns: Whether the tags contain the same language and subtags.
    static bool languagesMatch(LanguageTag preference, LanguageTag requested)
    {
        if (preference.value.size() != requested.value.size()) {
            return false;
        }
        const auto lowercase = [](char character) {
            return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        };
        for (std::size_t index = 0; index < preference.value.size(); ++index) {
            if (lowercase(preference.value[index]) != lowercase(requested.value[index])) {
                return false;
            }
        }
        return true;
    }

    GlyphRoute routeScriptFallback(const FontRequest& request, const FontFallbackPreferences& preferences, FontHandle primary)
    {
        for (const LanguageFallbackPreference& preference : preferences.languageFallbacks) {
            if (preference.script == request.script && request.script != ScriptClass::Han)
                return routeNamedFamily(request, preference.family, FontSource::ScriptFallback, m_caches.script, primary, preferences);
        }
        return {};
    }

    static GlyphRoute missingRoute(const FontRequest& request) { return { FontSource::Missing, {}, {}, routeFlags(request, FontSource::Missing), request.script, request.width }; }

    FontInventory& m_inventory;
    FontFallbackPolicyCaches m_caches;
};

} // namespace Ubiquitty::Rendering::Font
