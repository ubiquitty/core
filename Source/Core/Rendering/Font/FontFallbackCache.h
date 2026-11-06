// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontRoute.h"
#include "Core/Rendering/FontMetrics.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty::Rendering::Font {

struct FontFallbackCacheCounters {
    std::uint64_t hits { 0 };
    std::uint64_t misses { 0 };
    std::uint64_t stores { 0 };
    std::uint64_t nativeCascadeCalls { 0 };
    std::uint64_t lastResortRejects { 0 };
    std::uint64_t negativeHits { 0 };
    std::uint64_t clears { 0 };
};

struct FontFallbackCacheKey {
    FontHandle primaryFont;
    std::uint32_t metricsGeneration { 0 };
    std::uint32_t preferenceGeneration { 0 };
    std::uint32_t catalogGeneration { 0 };
    std::uint32_t languageHash { 0 };
    std::uint32_t familyHash { 0 };
    // The character a NEGATIVE route is about, and zero on every other key.
    //
    // The positive caches answer "which face serves this script", which is one
    // answer for thousands of characters and is why they key on the class. A
    // negative is the opposite: it says one character is nowhere, and a class
    // is the wrong grain for it — keyed that way, one absent character would
    // report every other character of its script absent too, and a page of a
    // script missing a single glyph would draw as nothing at all.
    std::uint32_t codePoint { 0 };
    ScriptClass script { ScriptClass::Other };
    Presentation presentation { Presentation::Default };
    FontStyle style { FontStyle::Regular };
    std::uint8_t variableWeight { 0 };
    FontSource routeMode { FontSource::NativeCascade };
    FontMetrics::FallbackSizing sizing { FontMetrics::FallbackSizing::Inherit };

    constexpr bool operator==(const FontFallbackCacheKey&) const = default;
};

struct FontFallbackCacheEntry {
    FontFallbackCacheKey key;
    FontHandle font;
    FontSource source { FontSource::NativeCascade };
    bool lastResort { false };
};

class FontFallbackCache {
public:
    explicit FontFallbackCache(std::size_t capacity)
        : m_capacity(capacity)
    {
    }

    bool lookup(const FontFallbackCacheKey& key, FontFallbackCacheEntry& out)
    {
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].key == key) {
                out = m_entries[i];
                FontFallbackCacheEntry entry = m_entries[i];
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                m_entries.insert(m_entries.begin(), entry);
                ++m_counters.hits;
                return true;
            }
        }
        ++m_counters.misses;
        return false;
    }

    void store(FontFallbackCacheEntry entry)
    {
        FontFallbackCacheEntry existing;
        if (lookup(entry.key, existing)) {
            m_entries.front() = entry;
            return;
        }
        if (m_capacity == 0)
            return;
        if (m_entries.size() == m_capacity)
            m_entries.pop_back();
        m_entries.insert(m_entries.begin(), entry);
        ++m_counters.stores;
    }

    void clear()
    {
        m_entries.clear();
        ++m_counters.clears;
    }

    void recordNativeCascadeCall() { ++m_counters.nativeCascadeCalls; }
    void recordLastResortReject() { ++m_counters.lastResortRejects; }
    void recordNegativeHit() { ++m_counters.negativeHits; }

    [[nodiscard]] const FontFallbackCacheCounters& counters() const { return m_counters; }
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    std::size_t m_capacity { 0 };
    std::vector<FontFallbackCacheEntry> m_entries;
    FontFallbackCacheCounters m_counters;
};

using HanFallbackCache = FontFallbackCache;
using ScriptFallbackCache = FontFallbackCache;
using GenericFallbackCache = FontFallbackCache;
using EmojiFallbackCache = FontFallbackCache;
using SymbolFallbackCache = FontFallbackCache;
using RangeMapCache = FontFallbackCache;
using NegativeRouteCache = FontFallbackCache;

} // namespace Ubiquitty::Rendering::Font
