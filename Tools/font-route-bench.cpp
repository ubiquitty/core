// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// font-route-bench: route a run of code points through FontFallbackPolicy and
// count native cascade calls. A fake inventory covers ASCII with one face and
// everything else with another, so a Han run must reach the system cascade at
// most once and answer the rest from the Han cache. Reads a UTF-8 file named
// on the command line, or 27,568 CJK ideographs without one; exits non-zero
// when the run took more than one cascade call.
//
//   font-route-bench [utf8-file]

#include "Core/Rendering/Font/FontFallbackPolicy.h"
#include "Core/Rendering/Font/ScriptClassifier.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>

using namespace Ubiquitty::Rendering::Font;

namespace {

class CoveringHanInventory final : public FontInventory {
public:
    std::uint32_t catalogGeneration() const override { return 1; }
    void bumpCatalogGeneration() override { }
    FontHandle primaryFont(FontStyle, std::uint8_t) override { return { 1, 1 }; }
    FontHandle validateFamily(const FontFamilyRequest&) override { return {}; }
    GlyphProbeResult glyphForCodePoint(FontHandle font, std::uint32_t codePoint) override
    {
        if (font.value == 1 && codePoint < 0x80)
            return { font, { codePoint }, true, false };
        if (font.value == 2 && codePoint >= 0x80)
            return { font, { codePoint }, true, false };
        return { font, {}, false, false };
    }
    GlyphProbeResult glyphForCluster(FontHandle font, std::span<const std::uint32_t> codePoints, Presentation) override
    {
        // The bench routes by base code point; a cluster answers as its base does.
        return codePoints.empty() ? GlyphProbeResult { font, {}, false, false } : glyphForCodePoint(font, codePoints[0]);
    }
    CascadeResult cascadeForCodePoint(const CascadeRequest& request) override
    {
        ++cascadeCalls;
        if (request.codePoints.empty())
            return {};
        return { { 2, 1 }, { request.codePoints[0] }, true, false };
    }
    CascadeResult cascadeForRun(const CascadeRequest& request) override { return cascadeForCodePoint(request); }
    bool isLastResort(FontHandle) const override { return false; }
    std::string_view familyName(FontHandle font) const override { return font.value == 2 ? "HanBench" : "Primary"; }
    std::string_view postScriptName(FontHandle font) const override { return familyName(font); }

    std::uint64_t cascadeCalls { 0 };
};

bool decodeNextUTF8(const std::string& text, std::size_t& index, std::uint32_t& codePoint)
{
    if (index >= text.size())
        return false;
    unsigned char lead = static_cast<unsigned char>(text[index++]);
    if (lead < 0x80) {
        codePoint = lead;
        return true;
    }
    int extra = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        codePoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        codePoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        codePoint = lead & 0x07;
    } else {
        codePoint = 0xFFFD;
        return true;
    }
    for (int i = 0; i < extra; ++i) {
        if (index >= text.size()) {
            codePoint = 0xFFFD;
            return true;
        }
        unsigned char continuation = static_cast<unsigned char>(text[index++]);
        if ((continuation & 0xC0) != 0x80) {
            codePoint = 0xFFFD;
            return true;
        }
        codePoint = (codePoint << 6) | (continuation & 0x3F);
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string payload;
    if (argc >= 2) {
        std::ifstream input(argv[1], std::ios::binary);
        payload.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (payload.empty()) {
        for (int i = 0; i < 27568; ++i) {
            std::uint32_t codePoint = 0x4E00 + static_cast<std::uint32_t>(i % (0x9FFF - 0x4E00));
            if (codePoint < 0x800) {
                payload.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                payload.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else {
                payload.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                payload.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                payload.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }
    }

    CoveringHanInventory inventory;
    HanFallbackCache han(64);
    ScriptFallbackCache script(64);
    GenericFallbackCache generic(16);
    EmojiFallbackCache emoji(64);
    SymbolFallbackCache symbol(64);
    RangeMapCache range(64);
    NegativeRouteCache negative(128);
    FontFallbackPolicy policy(inventory, { han, script, generic, emoji, symbol, range, negative });
    const FontFallbackPreferences preferences = FontFallbackPreferences::none();

    std::uint64_t total = 0;
    std::uint64_t hanCount = 0;
    std::size_t index = 0;
    while (index < payload.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeNextUTF8(payload, index, codePoint))
            break;
        FontRequest request;
        request.codePoints = std::span<const std::uint32_t>(&codePoint, 1);
        request.script = classifyScript(codePoint);
        request.width = request.script == ScriptClass::Han ? CellWidthClass::Wide : CellWidthClass::Narrow;
        request.preferenceGeneration = 1;
        GlyphRoute route = policy.route(request, preferences);
        if (route.script == ScriptClass::Han)
            ++hanCount;
        ++total;
    }

    std::printf("font-route-bench: total=%llu han=%llu nativeCascadeCalls=%llu hanCacheHits=%llu\n",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(hanCount),
        static_cast<unsigned long long>(han.counters().nativeCascadeCalls),
        static_cast<unsigned long long>(han.counters().hits));
    return han.counters().nativeCascadeCalls <= 1 ? 0 : 1;
}
