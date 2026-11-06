// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Shared route-cache policy and counters for Han, script, generic, emoji,
// symbol, range, and negative font-fallback decisions.

#include "Harness.h"

#include "Core/Rendering/Font/FontFallbackCache.h"

#include <cstdio>

using namespace Ubiquitty::Rendering::Font;

static FontFallbackCacheKey makeKey(ScriptClass script, FontSource source)
{
    FontFallbackCacheKey key;
    key.primaryFont = { 1, 1 };
    key.metricsGeneration = 2;
    key.preferenceGeneration = 3;
    key.catalogGeneration = 4;
    key.languageHash = 5;
    key.script = script;
    key.style = FontStyle::Regular;
    key.variableWeight = 0;
    key.routeMode = source;
    return key;
}

TEST(testHanCacheIsolation)
{
    HanFallbackCache han(4);
    GenericFallbackCache generic(4);

    FontFallbackCacheKey kanaKey = makeKey(ScriptClass::Kana, FontSource::GenericCache);
    generic.store({ kanaKey, { 20, 1 }, FontSource::GenericCache, false });

    FontFallbackCacheEntry out;
    CHECK(generic.lookup(kanaKey, out));
    CHECK(out.font.value == 20);
    CHECK(!han.lookup(makeKey(ScriptClass::Han, FontSource::AutoHanCascade), out));
    CHECK(han.counters().misses == 1);
}

TEST(testGenerationAndStyleMisses)
{
    ScriptFallbackCache cache(8);
    FontFallbackCacheKey key = makeKey(ScriptClass::Arabic, FontSource::ScriptFallback);
    cache.store({ key, { 30, 1 }, FontSource::ScriptFallback, false });

    FontFallbackCacheEntry out;
    CHECK(cache.lookup(key, out));

    FontFallbackCacheKey changedStyle = key;
    changedStyle.style = FontStyle::Bold;
    CHECK(!cache.lookup(changedStyle, out));

    FontFallbackCacheKey changedCatalog = key;
    changedCatalog.catalogGeneration += 1;
    CHECK(!cache.lookup(changedCatalog, out));
}

TEST(testNegativeCacheAndCounters)
{
    NegativeRouteCache negative(4);
    FontFallbackCacheKey key = makeKey(ScriptClass::Han, FontSource::Missing);
    negative.store({ key, { 0, 0 }, FontSource::Missing, true });
    negative.recordLastResortReject();

    FontFallbackCacheEntry out;
    CHECK(negative.lookup(key, out));
    negative.recordNegativeHit();
    CHECK(out.source == FontSource::Missing);
    CHECK(out.lastResort);
    CHECK(negative.counters().negativeHits == 1);
    CHECK(negative.counters().lastResortRejects == 1);
}

TEST(testWarmHanRouteUsesOneCascadeCall)
{
    HanFallbackCache han(4);
    FontFallbackCacheKey key = makeKey(ScriptClass::Han, FontSource::AutoHanCascade);
    FontFallbackCacheEntry out;

    for (int i = 0; i < 1000; ++i) {
        if (!han.lookup(key, out)) {
            han.recordNativeCascadeCall();
            han.store({ key, { 40, 1 }, FontSource::AutoHanCascade, false });
        }
    }

    CHECK(han.counters().nativeCascadeCalls == 1);
    CHECK(han.counters().hits == 999);
}

UBIQUITTY_TEST_MAIN()
