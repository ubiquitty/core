// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/Document.h"

#include "Core/RichText/FontCatalog.h"
#include "Core/Unicode/UTF8Decoder.h"

namespace Ubiquitty::RichText {

bool isWellFormed(const Document& document)
{
    if (document.fonts.size() > maxFonts || document.colorCount > maxColors)
        return false;
    return isWellFormed(DocumentView { document.text, document.runs, document.fontTable(), document.colorTable() });
}

bool isWellFormed(const DocumentView& document)
{
    if (document.text.size() > static_cast<std::size_t>(maxCodePoints) || document.runs.size() > static_cast<std::size_t>(maxRuns)
        || document.fonts.size() > maxFonts || document.colors.size() > maxColors)
        return false;
    for (const std::uint32_t codePoint : document.text) {
        if (!Unicode::isScalarValue(codePoint) || (codePoint < 0x20 && codePoint != '\t' && codePoint != '\n')
            || (codePoint >= 0x7F && codePoint <= 0x9F))
            return false;
    }
    for (const FontEntry& font : document.fonts) {
        if (font.nameLength > maxFamilyNameBytes || static_cast<std::uint8_t>(font.familyClass) > static_cast<std::uint8_t>(FamilyClass::Monospace))
            return false;
        if (font.nameLength && !isFontCatalogFamily(font.familyName()))
            return false;
    }
    std::size_t next = 0;
    const Run* previous = nullptr;
    for (const Run& run : document.runs) {
        if (run.start != next || run.length < 1 || run.length > document.text.size() - next)
            return false;
        if (run.halfPoints < minHalfPoints || run.halfPoints > maxHalfPoints || (run.styles & ~allStyleBits)
            || static_cast<std::uint8_t>(run.alignment) > static_cast<std::uint8_t>(Alignment::Right) || !isWellFormed(run.paragraph))
            return false;
        if (run.font != noFontEntry && run.font >= document.fonts.size())
            return false;
        if (run.color && (run.color >= document.colors.size() || document.colors[run.color].isAutomatic))
            return false;
        if (run.background && (run.background >= document.colors.size() || document.colors[run.background].isAutomatic))
            return false;
        if (previous && previous->halfPoints == run.halfPoints && previous->font == run.font && previous->color == run.color && previous->background == run.background
            && previous->styles == run.styles && previous->alignment == run.alignment && previous->paragraph == run.paragraph)
            return false;
        previous = &run;
        next += run.length;
    }
    return next == document.text.size();
}

} // namespace Ubiquitty::RichText
