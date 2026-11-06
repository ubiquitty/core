// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

// BundledFonts.h — which font families this project ships, and the file each
// one lives in.
//
// One table because the answer is one fact. A frontend that spells a family
// name in an `if` covers the fonts whoever wrote the `if` remembered: one that
// matches "JetBrains Mono" alone sends a configuration naming another shipped
// face to the platform's font matcher, which draws whatever it substitutes — a
// face the project does not ship, selected by a config that named one it does.
//
// Resources/fonts/manifest.json is the provenance record — where each file came
// from, its hash, its axes — and Scripts/fonts.py gates it. This is the part the
// RENDERER needs: the family a user can name, and the file to open for it.

namespace Ubiquitty::Rendering {

/// A family this project ships, and the file it is in.
struct BundledFont {
    /// The family name as the face itself declares it, which is the name a
    /// configuration has to spell. Not a friendly label: "Monaspace Neon Var"
    /// is what the bundled variable build reports, and a config naming
    /// "Monaspace Neon" matches nothing.
    std::string_view family;
    /// The file's name inside the bundled font directory.
    std::string_view file;
    /// The bold member's file, for a family shipped as static instances; empty
    /// for a variable face, whose weight is an axis of the one file.
    std::string_view boldFile = {};
    /// The italic member's file, empty for a family shipping no italic file:
    /// none at all, or a slant axis on the upright. A variable italic carries
    /// its weights on its own `wght` axis.
    std::string_view italicFile = {};
    /// The bold italic member's file, for a family shipped as static
    /// instances; empty where the italic file's axis carries the weight.
    std::string_view boldItalicFile = {};
};

/// Every family this project ships as a loadable file.
///
/// GNU Unifont is absent on purpose: it is baked into C++ rather than shipped
/// as a font file, so there is no path to open for it.
///
/// - Returns: The table, in no significant order.
[[nodiscard]] constexpr std::span<const BundledFont> bundledFonts()
{
    // Symbols Nerd Font Mono is here as a family a user may name, though its
    // role is the glyph-only fallback tier the frontends install separately.
    static constexpr BundledFont fonts[] = {
        { "JetBrains Mono", "JetBrainsMono[wght].ttf", {}, "JetBrainsMono-Italic[wght].ttf" },
        { "Monaspace Neon", "MonaspaceNeon-Regular.otf", "MonaspaceNeon-Bold.otf", "MonaspaceNeon-Italic.otf", "MonaspaceNeon-BoldItalic.otf" },
        { "Monaspace Neon Var", "MonaspaceNeonVar.ttf" },
        { "Noto Sans", "NotoSans-Regular.ttf", "NotoSans-Bold.ttf" },
        { "Noto Color Emoji", "NotoColorEmoji-COLRv1.ttf" },
        { "Symbols Nerd Font Mono", "SymbolsNerdFontMono-Regular.ttf" },
    };
    return fonts;
}

/// Owned family names for language boundaries that cannot borrow the table.
/// A renderer must still verify that each bundled file can be opened locally.
[[nodiscard]] inline std::vector<std::string> bundledFontFamilies()
{
    std::vector<std::string> families;
    families.reserve(bundledFonts().size());
    for (const auto& font : bundledFonts())
        families.emplace_back(font.family);
    return families;
}

/// The bundled file a family name selects, or empty for a family we do not ship.
///
/// An empty answer is the caller's signal to ask the platform's font matcher
/// instead — which is the right route for a face the user installed themselves.
///
/// - Parameter family: The family a configuration named.
/// - Returns: The file name inside the bundled font directory, or an empty view.
[[nodiscard]] constexpr std::string_view bundledFontFile(std::string_view family)
{
    for (const BundledFont& font : bundledFonts()) {
        if (font.family == family)
            return font.file;
    }
    return {};
}

/// The bundled file a family's bold member is in, or empty when the family is
/// not shipped or ships its weight as an axis.
///
/// - Parameter family: The family a configuration named.
/// - Returns: The bold file's name inside the bundled font directory, or empty.
[[nodiscard]] constexpr std::string_view bundledBoldFontFile(std::string_view family)
{
    for (const BundledFont& font : bundledFonts()) {
        if (font.family == family)
            return font.boldFile;
    }
    return {};
}

/// The bundled file a family's italic member is in, or empty when the family
/// is not shipped or ships no italic file.
///
/// - Parameter family: The family a configuration named.
/// - Returns: The italic file's name inside the bundled font directory, or empty.
[[nodiscard]] constexpr std::string_view bundledItalicFontFile(std::string_view family)
{
    for (const BundledFont& font : bundledFonts()) {
        if (font.family == family)
            return font.italicFile;
    }
    return {};
}

/// The bundled file a family's bold italic member is in, or empty when the
/// family is not shipped or carries that weight on its italic file's axis.
///
/// - Parameter family: The family a configuration named.
/// - Returns: The bold italic file's name inside the bundled font directory, or empty.
[[nodiscard]] constexpr std::string_view bundledBoldItalicFontFile(std::string_view family)
{
    for (const BundledFont& font : bundledFonts()) {
        if (font.family == family)
            return font.boldItalicFile;
    }
    return {};
}

/// The four style files of a shipped family, by value for a frontend that
/// cannot hold a view: empty strings for a family we do not ship, and for a
/// style the family carries on an axis of another file or not at all.
struct BundledFaceFiles {
    std::string regular;
    std::string bold;
    std::string italic;
    std::string boldItalic;
};

/// The files a family's four styles are drawn from, when we ship the family.
///
/// A frontend resolving a shipped family's style places it on one of these
/// files by axis and never asks the platform's matcher by trait: the matcher
/// answers from every face carrying the family's name, so a machine with its
/// own copy of the family installed would draw that copy instead.
///
/// - Parameter family: The family a configuration named.
/// - Returns: The files, all empty for a family we do not ship.
[[nodiscard]] inline BundledFaceFiles bundledFaceFiles(const std::string& family)
{
    for (const BundledFont& font : bundledFonts()) {
        if (font.family == family)
            return { std::string(font.file), std::string(font.boldFile), std::string(font.italicFile), std::string(font.boldItalicFile) };
    }
    return {};
}

} // namespace Ubiquitty::Rendering
