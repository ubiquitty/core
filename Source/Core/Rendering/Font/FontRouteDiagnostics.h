// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontFallbackCache.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

namespace Ubiquitty::Rendering::Font {

enum class FontRouteCacheState : std::uint8_t {
    Disabled,
    Hit,
    Miss,
    Stored,
    Negative,
};

enum class FontRouteAtlasFamily : std::uint8_t {
    Mono,
    Wide,
    Color,
    Ligature,
    DoubleSize,
    Procedural,
    Shaped,
    Missing,
};

struct FontRouteCounterSnapshot {
    std::uint64_t nativeCascadeCalls { 0 };
    std::uint64_t atlasMisses { 0 };
    std::uint64_t routeCacheHits { 0 };
    std::uint64_t routeCacheMisses { 0 };
    std::uint64_t hanCacheHits { 0 };
    std::uint64_t genericCacheHits { 0 };
    std::uint64_t negativeHits { 0 };
    std::uint64_t glyphUploads { 0 };
    std::uint64_t routePolicyMicros { 0 };
};

class FontRouteCounters {
public:
    void addCacheCounters(const FontFallbackCacheCounters& counters)
    {
        m_snapshot.routeCacheHits += counters.hits;
        m_snapshot.routeCacheMisses += counters.misses;
        m_snapshot.nativeCascadeCalls += counters.nativeCascadeCalls;
        m_snapshot.negativeHits += counters.negativeHits;
    }

    void addHanCacheHits(std::uint64_t count) { m_snapshot.hanCacheHits += count; }
    void addGenericCacheHits(std::uint64_t count) { m_snapshot.genericCacheHits += count; }
    void addAtlasMisses(std::uint64_t count) { m_snapshot.atlasMisses += count; }
    void addGlyphUploads(std::uint64_t count) { m_snapshot.glyphUploads += count; }
    void addRoutePolicyMicros(std::uint64_t count) { m_snapshot.routePolicyMicros += count; }

    [[nodiscard]] FontRouteCounterSnapshot snapshot() const { return m_snapshot; }
    void reset() { m_snapshot = {}; }

private:
    FontRouteCounterSnapshot m_snapshot;
};

struct FontRouteDiagnostic {
    std::span<const std::uint32_t> codePoints;
    ScriptClass script { ScriptClass::Other };
    LanguageTag language;
    CellWidthClass width { CellWidthClass::Narrow };
    FontSource source { FontSource::Missing };
    std::string_view familyName;
    std::string_view postScriptName;
    GlyphID glyph;
    FontRouteCacheState cacheState { FontRouteCacheState::Disabled };
    FontRouteAtlasFamily atlas { FontRouteAtlasFamily::Missing };
    bool shapedRun { false };
    bool color { false };
    bool missing { false };
    bool lastResort { false };
    std::uint64_t cascadeCalls { 0 };
};

constexpr const char* scriptClassName(ScriptClass script)
{
    switch (script) {
    case ScriptClass::Latin:
        return "Latin";
    case ScriptClass::Han:
        return "Han";
    case ScriptClass::Kana:
        return "Kana";
    case ScriptClass::Hangul:
        return "Hangul";
    case ScriptClass::Cyrillic:
        return "Cyrillic";
    case ScriptClass::Greek:
        return "Greek";
    case ScriptClass::Arabic:
        return "Arabic";
    case ScriptClass::Hebrew:
        return "Hebrew";
    case ScriptClass::Devanagari:
        return "Devanagari";
    case ScriptClass::Bengali:
        return "Bengali";
    case ScriptClass::Gurmukhi:
        return "Gurmukhi";
    case ScriptClass::Thai:
        return "Thai";
    case ScriptClass::Lao:
        return "Lao";
    case ScriptClass::Khmer:
        return "Khmer";
    case ScriptClass::Myanmar:
        return "Myanmar";
    case ScriptClass::Emoji:
        return "Emoji";
    case ScriptClass::Symbols:
        return "Symbols";
    case ScriptClass::Math:
        return "Math";
    case ScriptClass::Other:
        return "Other";
    }
    return "Other";
}

constexpr const char* widthClassName(CellWidthClass width)
{
    switch (width) {
    case CellWidthClass::Narrow:
        return "narrow";
    case CellWidthClass::Wide:
        return "wide";
    case CellWidthClass::Zero:
        return "zero";
    case CellWidthClass::Control:
        return "control";
    case CellWidthClass::Cluster:
        return "cluster";
    }
    return "narrow";
}

constexpr const char* fontSourceName(FontSource source)
{
    switch (source) {
    case FontSource::Primary:
        return "Primary";
    case FontSource::RangeMap:
        return "RangeMap";
    case FontSource::ExplicitHan:
        return "ExplicitHan";
    case FontSource::AutoHanCascade:
        return "AutoHanCascade";
    case FontSource::SymbolFallback:
        return "SymbolFallback";
    case FontSource::EmojiCascade:
        return "EmojiCascade";
    case FontSource::ScriptFallback:
        return "ScriptFallback";
    case FontSource::GenericCache:
        return "GenericCache";
    case FontSource::NativeCascade:
        return "NativeCascade";
    case FontSource::Missing:
        return "Missing";
    }
    return "Missing";
}

constexpr const char* cacheStateName(FontRouteCacheState state)
{
    switch (state) {
    case FontRouteCacheState::Disabled:
        return "disabled";
    case FontRouteCacheState::Hit:
        return "hit";
    case FontRouteCacheState::Miss:
        return "miss";
    case FontRouteCacheState::Stored:
        return "stored";
    case FontRouteCacheState::Negative:
        return "negative";
    }
    return "disabled";
}

constexpr const char* atlasFamilyName(FontRouteAtlasFamily atlas)
{
    switch (atlas) {
    case FontRouteAtlasFamily::Mono:
        return "mono";
    case FontRouteAtlasFamily::Wide:
        return "wide";
    case FontRouteAtlasFamily::Color:
        return "color";
    case FontRouteAtlasFamily::Ligature:
        return "ligature";
    case FontRouteAtlasFamily::DoubleSize:
        return "double";
    case FontRouteAtlasFamily::Procedural:
        return "procedural";
    case FontRouteAtlasFamily::Shaped:
        return "shaped";
    case FontRouteAtlasFamily::Missing:
        return "missing";
    }
    return "missing";
}

inline int formatFontRouteDiagnostic(const FontRouteDiagnostic& diagnostic, char* buffer, std::size_t capacity)
{
    if (!buffer || capacity == 0)
        return 0;
    std::uint32_t lead = !diagnostic.codePoints.empty() ? diagnostic.codePoints[0] : 0;
    return std::snprintf(buffer, capacity,
        "U+%04X script=%s width=%s source=%s font=%.*s postscript=%.*s glyph=%u cache=%s atlas=%s shaped=%d color=%d "
        "missing=%d lastResort=%d cascade=%llu",
        lead, scriptClassName(diagnostic.script), widthClassName(diagnostic.width), fontSourceName(diagnostic.source), static_cast<int>(diagnostic.familyName.size()), diagnostic.familyName.data(), static_cast<int>(diagnostic.postScriptName.size()), diagnostic.postScriptName.data(),
        diagnostic.glyph.value, cacheStateName(diagnostic.cacheState), atlasFamilyName(diagnostic.atlas), diagnostic.shapedRun ? 1 : 0, diagnostic.color ? 1 : 0, diagnostic.missing ? 1 : 0, diagnostic.lastResort ? 1 : 0, static_cast<unsigned long long>(diagnostic.cascadeCalls));
}

} // namespace Ubiquitty::Rendering::Font
