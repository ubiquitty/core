// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Portable diagnostic record serialization plus aggregate counters. Records are
// disabled unless a caller requests them.

#include "Harness.h"

#include "Core/Rendering/Font/FontRouteDiagnostics.h"

#include <cstdio>
#include <cstring>

using namespace Ubiquitty::Rendering::Font;

static bool contains(const char* haystack, const char* needle) { return std::strstr(haystack, needle) != nullptr; }

TEST(testDiagnosticFormatting)
{
    std::uint32_t codePoints[] { 0x6771 };
    FontRouteDiagnostic diagnostic;
    diagnostic.codePoints = std::span<const std::uint32_t>(codePoints, 1);
    diagnostic.script = ScriptClass::Han;
    diagnostic.width = CellWidthClass::Wide;
    diagnostic.source = FontSource::ExplicitHan;
    diagnostic.familyName = "Hiragino Sans";
    diagnostic.postScriptName = "HiraginoSans-W3";
    diagnostic.glyph = { 1234 };
    diagnostic.cacheState = FontRouteCacheState::Hit;
    diagnostic.atlas = FontRouteAtlasFamily::Wide;
    diagnostic.cascadeCalls = 1;

    char buffer[512];
    int written = formatFontRouteDiagnostic(diagnostic, buffer, sizeof(buffer));
    CHECK(written > 0);
    CHECK(contains(buffer, "U+6771"));
    CHECK(contains(buffer, "script=Han"));
    CHECK(contains(buffer, "width=wide"));
    CHECK(contains(buffer, "source=ExplicitHan"));
    CHECK(contains(buffer, "font=Hiragino Sans"));
    CHECK(contains(buffer, "cache=hit"));
    CHECK(contains(buffer, "atlas=wide"));
}

TEST(testMissingAndClusterFormatting)
{
    std::uint32_t codePoints[] { 0x1F468, 0x200D, 0x1F469 };
    FontRouteDiagnostic diagnostic;
    diagnostic.codePoints = std::span<const std::uint32_t>(codePoints, 3);
    diagnostic.script = ScriptClass::Emoji;
    diagnostic.width = CellWidthClass::Cluster;
    diagnostic.source = FontSource::EmojiCascade;
    diagnostic.glyph = { 55 };
    diagnostic.cacheState = FontRouteCacheState::Stored;
    diagnostic.atlas = FontRouteAtlasFamily::Color;
    diagnostic.shapedRun = true;
    diagnostic.color = true;

    char buffer[512];
    formatFontRouteDiagnostic(diagnostic, buffer, sizeof(buffer));
    CHECK(contains(buffer, "script=Emoji"));
    CHECK(contains(buffer, "width=cluster"));
    CHECK(contains(buffer, "source=EmojiCascade"));
    CHECK(contains(buffer, "atlas=color"));
    CHECK(contains(buffer, "shaped=1"));
    CHECK(contains(buffer, "color=1"));

    diagnostic.source = FontSource::Missing;
    diagnostic.cacheState = FontRouteCacheState::Negative;
    diagnostic.atlas = FontRouteAtlasFamily::Missing;
    diagnostic.missing = true;
    diagnostic.lastResort = true;
    formatFontRouteDiagnostic(diagnostic, buffer, sizeof(buffer));
    CHECK(contains(buffer, "source=Missing"));
    CHECK(contains(buffer, "cache=negative"));
    CHECK(contains(buffer, "missing=1"));
    CHECK(contains(buffer, "lastResort=1"));
}

TEST(testCounters)
{
    FontFallbackCacheCounters cacheCounters;
    cacheCounters.hits = 3;
    cacheCounters.misses = 2;
    cacheCounters.nativeCascadeCalls = 1;
    cacheCounters.negativeHits = 4;

    FontRouteCounters counters;
    counters.addCacheCounters(cacheCounters);
    counters.addHanCacheHits(3);
    counters.addGenericCacheHits(1);
    counters.addAtlasMisses(2);
    counters.addGlyphUploads(5);
    counters.addRoutePolicyMicros(8);
    FontRouteCounterSnapshot snapshot = counters.snapshot();
    CHECK(snapshot.routeCacheHits == 3);
    CHECK(snapshot.routeCacheMisses == 2);
    CHECK(snapshot.nativeCascadeCalls == 1);
    CHECK(snapshot.negativeHits == 4);
    CHECK(snapshot.hanCacheHits == 3);
    CHECK(snapshot.genericCacheHits == 1);
    CHECK(snapshot.atlasMisses == 2);
    CHECK(snapshot.glyphUploads == 5);
    CHECK(snapshot.routePolicyMicros == 8);
    counters.reset();
    CHECK(counters.snapshot().routeCacheHits == 0);
}

// Every enumerator answers with a name of its own. Uniqueness is the
// assertion, not the exact strings: a case dropped from a switch falls to the
// fallback and collides with the enumerator that legitimately owns that word,
// while an exact-string table would just restate the switch and rot with it.
template<typename Enum, std::size_t N>
static void checkDistinctNames(const Enum (&values)[N], const char* (*name)(Enum), const char* label)
{
    for (std::size_t i = 0; i < N; ++i) {
        const char* a = name(values[i]);
        CHECK_MSG(a != nullptr && a[0] != '\0', label);
        for (std::size_t j = i + 1; j < N; ++j)
            CHECK_MSG(std::strcmp(a, name(values[j])) != 0, label);
    }
}

TEST(testEveryEnumeratorNamesItself)
{
    static constexpr ScriptClass scripts[] = { ScriptClass::Latin, ScriptClass::Han, ScriptClass::Kana,
        ScriptClass::Hangul, ScriptClass::Cyrillic, ScriptClass::Greek, ScriptClass::Arabic, ScriptClass::Hebrew,
        ScriptClass::Devanagari, ScriptClass::Bengali, ScriptClass::Gurmukhi, ScriptClass::Thai, ScriptClass::Lao,
        ScriptClass::Khmer, ScriptClass::Myanmar, ScriptClass::Emoji, ScriptClass::Symbols, ScriptClass::Math,
        ScriptClass::Other };
    checkDistinctNames(scripts, scriptClassName, "script");

    static constexpr CellWidthClass widths[] = { CellWidthClass::Narrow, CellWidthClass::Wide, CellWidthClass::Zero,
        CellWidthClass::Control, CellWidthClass::Cluster };
    checkDistinctNames(widths, widthClassName, "width");

    static constexpr FontSource sources[] = { FontSource::Primary, FontSource::RangeMap, FontSource::ExplicitHan,
        FontSource::AutoHanCascade, FontSource::SymbolFallback, FontSource::EmojiCascade, FontSource::ScriptFallback,
        FontSource::GenericCache, FontSource::NativeCascade, FontSource::Missing };
    checkDistinctNames(sources, fontSourceName, "source");

    static constexpr FontRouteCacheState cacheStates[] = { FontRouteCacheState::Disabled, FontRouteCacheState::Hit,
        FontRouteCacheState::Miss, FontRouteCacheState::Stored, FontRouteCacheState::Negative };
    checkDistinctNames(cacheStates, cacheStateName, "cache");

    static constexpr FontRouteAtlasFamily atlases[] = { FontRouteAtlasFamily::Mono, FontRouteAtlasFamily::Wide,
        FontRouteAtlasFamily::Color, FontRouteAtlasFamily::Ligature, FontRouteAtlasFamily::DoubleSize,
        FontRouteAtlasFamily::Procedural, FontRouteAtlasFamily::Shaped, FontRouteAtlasFamily::Missing };
    checkDistinctNames(atlases, atlasFamilyName, "atlas");
}

// The formatter's own refusals: no buffer, no room.
TEST(testFormatterRefusesADegenerateBuffer)
{
    FontRouteDiagnostic diagnostic;
    CHECK(formatFontRouteDiagnostic(diagnostic, nullptr, 128) == 0);
    char one;
    CHECK(formatFontRouteDiagnostic(diagnostic, &one, 0) == 0);
}

UBIQUITTY_TEST_MAIN()
