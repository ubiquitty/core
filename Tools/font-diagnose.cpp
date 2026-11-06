// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// font-diagnose: print the font-route diagnostic line Core formats for each
// code point of a string: script class, cell width, font source, atlas and
// whether the run needs shaping. It classifies without a font inventory, so
// the line shows the route Core's tables choose, not what a system font
// answers. Compiled with the tests, never run by them.
//
//   font-diagnose ["text"]

#include "Core/Rendering/Font/FontRouteDiagnostics.h"
#include "Core/Rendering/Font/ScriptClassifier.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Ubiquitty::Rendering::Font;

namespace {

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
    codePoint = 0;
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

CellWidthClass widthForScript(ScriptClass script)
{
    if (script == ScriptClass::Han || script == ScriptClass::Kana || script == ScriptClass::Hangul || script == ScriptClass::Emoji)
        return CellWidthClass::Wide;
    return CellWidthClass::Narrow;
}

FontRouteAtlasFamily atlasFor(ScriptClass script, CellWidthClass width, bool shapedRun)
{
    if (script == ScriptClass::Emoji)
        return FontRouteAtlasFamily::Color;
    if (shapedRun)
        return FontRouteAtlasFamily::Shaped;
    if (width == CellWidthClass::Wide)
        return FontRouteAtlasFamily::Wide;
    if (script == ScriptClass::Symbols || script == ScriptClass::Math)
        return FontRouteAtlasFamily::Procedural;
    return FontRouteAtlasFamily::Mono;
}

FontSource sourceFor(ScriptClass script)
{
    if (script == ScriptClass::Han)
        return FontSource::AutoHanCascade;
    if (script == ScriptClass::Emoji)
        return FontSource::EmojiCascade;
    if (script == ScriptClass::Symbols || script == ScriptClass::Math)
        return FontSource::SymbolFallback;
    if (scriptRequiresShapedRun(script))
        return FontSource::ScriptFallback;
    return FontSource::Primary;
}

} // namespace

int main(int argc, char** argv)
{
    std::string input = argc >= 2 ? argv[1] : "東京オペレーション端末 가나다 سلام 😀 ";
    std::size_t index = 0;
    while (index < input.size()) {
        std::uint32_t codePoint = 0;
        if (!decodeNextUTF8(input, index, codePoint))
            break;
        ScriptClass script = classifyScript(codePoint);
        CellWidthClass width = widthForScript(script);
        bool shapedRun = codePointRequiresShapedRun(codePoint);
        FontRouteDiagnostic diagnostic;
        diagnostic.codePoints = std::span<const std::uint32_t>(&codePoint, 1);
        diagnostic.script = script;
        diagnostic.width = width;
        diagnostic.source = sourceFor(script);
        diagnostic.familyName = scriptClassName(script);
        diagnostic.postScriptName = scriptClassName(script);
        diagnostic.glyph = { codePoint };
        diagnostic.cacheState = FontRouteCacheState::Miss;
        diagnostic.atlas = atlasFor(script, width, shapedRun);
        diagnostic.shapedRun = shapedRun;
        diagnostic.color = script == ScriptClass::Emoji;
        diagnostic.missing = false;
        diagnostic.lastResort = false;
        diagnostic.cascadeCalls = diagnostic.source == FontSource::Primary ? 0 : 1;

        char buffer[512];
        formatFontRouteDiagnostic(diagnostic, buffer, sizeof(buffer));
        std::puts(buffer);
    }
    return 0;
}
