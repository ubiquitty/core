// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The admissible values of every closed-vocabulary preference: the canonical
// TOML token beside the name a picker shows. `vocabularyFor(key)` yields the
// ordered terms. Pure header (no platform types), like Key.h and Schema.h.
//
// This is the third leg of the per-key tables, owned by Core rather than by a
// frontend. Key.h gives a preference its section, key and serializer,
// Schema.h gives the numeric ones their bounds, and this gives the closed ones
// their tokens.
// A picker built from anything else is a second vocabulary, and two lists of
// the same tokens in two languages have nothing comparing them — the labels a
// frontend shows and the tokens its parser accepts drift apart one rename at a
// time, and the file a person edits stops matching the menu they picked from.

#include <cstddef>
#include <span>

#include "Core/Preferences/Key.h"
#include "Core/Preferences/Schema.h"

namespace Ubiquitty::Preferences {

/// One admissible value of a closed-vocabulary preference.
///
/// Both halves are needed and neither substitutes for the other: `token` is
/// what the file holds and what the key's parser accepts, `label` is what a
/// person reads in a picker. Title-casing a token does not produce the label
/// (`display-p3` shows "Display P3", `srgb` shows "sRGB"), so the pair is
/// written out rather than derived.
struct VocabularyTerm {
    const char* token;
    const char* label;
};

namespace Internal {

inline constexpr VocabularyTerm brailleStyleTerms[] = {
    { "regular", "Regular" },
    { "outline-6", "Outline 6 Dot" },
    { "outline-8", "Outline 8 Dot" },
    { "pinpoint-6", "Pinpoint 6 Dot" },
    { "pinpoint-8", "Pinpoint 8 Dot" },
    { "square", "Square" },
    { "raster", "Raster" },
};

// [appearance] background_image_mode — BackgroundImageMode
inline constexpr VocabularyTerm backgroundImageModeTerms[] = {
    { "fill", "Fill" },
    { "fit", "Fit" },
    { "stretch", "Stretch" },
    { "tile", "Tile" },
    { "center", "Center" },
};

// [appearance] titlebar_style — TitlebarStyle
inline constexpr VocabularyTerm titlebarStyleTerms[] = {
    { "themed", "Themed" },
    { "native", "Native" },
};

// [appearance] color_space — ColorSpace.
inline constexpr VocabularyTerm colorSpaceTerms[] = {
    { "srgb", "sRGB" },
    { "display-p3", "Display P3" },
};

// [appearance] colorblind_mode — Accessibility::ColorblindMode
inline constexpr VocabularyTerm colorblindModeTerms[] = {
    { "none", "None" },
    { "deuteranopia", "Deuteranopia" },
    { "protanopia", "Protanopia" },
    { "tritanopia", "Tritanopia" },
};

// [appearance] shade_blocks — Rendering::ShadeStyle
inline constexpr VocabularyTerm shadeBlocksTerms[] = {
    { "pattern", "Pattern" },
    { "fill", "Fill" },
};

// [cursor] shape — CursorShape
inline constexpr VocabularyTerm cursorShapeTerms[] = {
    { "block", "Block" },
    { "underline", "Underline" },
    { "bar", "Bar" },
};

// [tabs] show_shortcuts — TabShortcutHints
inline constexpr VocabularyTerm tabShortcutHintsTerms[] = {
    { "off", "Off" },
    { "always", "Always" },
    { "command", "While Command Is Held" },
};

// [cursor] motion — CursorMotionMode
inline constexpr VocabularyTerm cursorMotionTerms[] = {
    { "off", "Off" },
    { "short", "Short" },
    { "long", "Long" },
    { "always", "Always" },
};

// Closed modifier choices keep normal and rectangular selection chords distinct.
inline constexpr VocabularyTerm closeConfirmationTerms[] = {
    { "always", "Always" },
    { "never", "Never" },
    { "running", "Only for Running Processes" },
};

inline constexpr VocabularyTerm selectModifierTerms[] = {
    { "shift", "Shift" },
    { "cmd", "Command" },
    { "ctrl", "Control" },
};

// [selection] style — Rendering::SelectionStyle, in that enum's order.
inline constexpr VocabularyTerm selectionStyleTerms[] = {
    { "native", "System Color" },
    { "tint", "Translucent Tint" },
};

inline constexpr VocabularyTerm nonOwnerPresentationTerms[] = {
    { "letterbox", "Letterbox" },
    { "pan", "Pan to Cursor" },
    { "fit", "Fit to View" },
};

inline constexpr VocabularyTerm blockSelectModifierTerms[] = {
    { "opt", "Option" },
    { "shift+opt", "Shift-Option" },
    { "cmd+opt", "Command-Option" },
    { "ctrl+opt", "Control-Option" },
};

// [terminal] encoding — Unicode::Encoding, in that enum's order. The value
// is carried as a string and resolved by `Unicode::encodingFromName`, which
// drops `-`, `_`, `.` and spaces before comparing; the token written here is
// the separator-bearing spelling a person reads, and it resolves to the
// enumerator at the same index. Each encoding has aliases (`sjis`, `cp932`,
// `gbk`, …) that the reader accepts and a picker does not offer.
inline constexpr VocabularyTerm terminalEncodingTerms[] = {
    { "utf-8", "UTF-8" },
    { "latin1", "Latin-1 (ISO-8859-1)" },
    { "iso-8859-15", "Latin-9 (ISO-8859-15)" },
    { "koi8-r", "KOI8-R (Cyrillic)" },
    { "windows-1252", "Windows-1252 (Western)" },
    { "shift_jis", "Shift_JIS (Japanese)" },
    { "euc-jp", "EUC-JP (Japanese)" },
    { "euc-kr", "EUC-KR (Korean)" },
    { "gb18030", "GB18030 (Simplified Chinese)" },
    { "big5", "Big5 (Traditional Chinese)" },
};

} // namespace Internal

/// The admissible values of `key`, in the order a picker should offer them.
///
/// For a preference backed by a C++ enum the order is the enumerators' own, so
/// a term's index equals its enum value, and a frontend holding a selected
/// index needs no second mapping. ``test_preferences_vocabulary`` pins that: an
/// enumerator inserted in the middle without a matching row here fails the
/// round-trip rather than silently renumbering everyone's picker.
///
/// Not every ``SerializeKind::EnumToken`` key appears. `[window] drag_modifier`
/// is a `+`-separated chord — `control+command`, `shift`, `none` — so its
/// admissible values are combinations rather than a list, and offering a subset
/// as if it were the whole would be worse than offering none.
///
/// - Parameter key: The preference to look up.
/// - Returns: Its terms, or an empty span when the key admits free text, a
///   number, a chord or a bare toggle. Empty is the answer for most keys and is
///   not an error.
[[nodiscard]] constexpr std::span<const VocabularyTerm> vocabularyFor(Key key)
{
    switch (key) {
    case Key::AppearanceBackgroundImageMode:
        return Internal::backgroundImageModeTerms;
    case Key::AppearanceTitlebarStyle:
        return Internal::titlebarStyleTerms;
    case Key::AppearanceColorSpace:
        return Internal::colorSpaceTerms;
    case Key::AppearanceColorblindMode:
        return Internal::colorblindModeTerms;
    case Key::AppearanceShadeBlocks:
        return Internal::shadeBlocksTerms;
    case Key::CursorShape:
        return Internal::cursorShapeTerms;
    case Key::CursorMotion:
        return Internal::cursorMotionTerms;
    case Key::TabsShowShortcuts:
        return Internal::tabShortcutHintsTerms;
    case Key::FontBrailleStyle:
        return Internal::brailleStyleTerms;
    case Key::TerminalEncoding:
        return Internal::terminalEncodingTerms;
    case Key::TerminalCloseConfirmation:
        return Internal::closeConfirmationTerms;
    case Key::SelectionSelectModifier:
        return Internal::selectModifierTerms;
    case Key::SelectionBlockSelectModifier:
        return Internal::blockSelectModifierTerms;
    case Key::SelectionStyle:
        return Internal::selectionStyleTerms;
    case Key::RemoteNonOwnerPresentation:
        return Internal::nonOwnerPresentationTerms;
    default:
        // A default arm rather than an enumeration of the other 128 keys,
        // because the alternative costs an edit here for every key added
        // anywhere — and the omission that matters is the opposite one, a key
        // that acquires a vocabulary and is not routed to it. The test walks
        // `writeDescriptors` and catches that.
        return {};
    }
}

/// The admissible values of the preference `[section] key` names.
///
/// The strings overload exists so a frontend already holding the pair — a
/// settings row keyed the way `Schema::rangeFor` is keyed — asks the same way
/// for both tables, instead of one of them being the odd one that needs a
/// ``Key`` looked up first.
///
/// - Parameters:
///   - section: The config section, without brackets.
///   - key: The key within that section.
/// - Returns: Its terms, or empty when the pair names no preference or the
///   preference admits no closed vocabulary.
[[nodiscard]] constexpr std::span<const VocabularyTerm> vocabularyFor(const char* section, const char* key)
{
    if (section == nullptr || key == nullptr)
        return {};
    for (int i = 0; i < writeDescriptorCount; ++i) {
        if (Schema::sameKey(writeDescriptors[i].section, section) && Schema::sameKey(writeDescriptors[i].key, key))
            return vocabularyFor(writeDescriptors[i].id);
    }
    return {};
}

/// Whether `key` admits a closed vocabulary a picker can offer.
///
/// - Parameter key: The preference to test.
/// - Returns: True when ``vocabularyFor`` yields at least one term.
[[nodiscard]] constexpr bool hasVocabulary(Key key) { return !vocabularyFor(key).empty(); }

/// The index of `token` within `key`'s vocabulary.
///
/// The comparison is exact, because this reads a token this table produced or a
/// parser already canonicalised; the case-insensitive and alias spellings
/// belong at the parse boundary, not here.
///
/// - Parameters:
///   - key: The preference whose vocabulary to search.
///   - token: The canonical token to find.
/// - Returns: The term's index, or -1 when the key has no vocabulary or the
///   token is not one of its terms.
[[nodiscard]] constexpr int vocabularyIndexOf(Key key, const char* token)
{
    if (token == nullptr)
        return -1;
    const std::span<const VocabularyTerm> terms = vocabularyFor(key);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (Schema::sameKey(terms[i].token, token))
            return static_cast<int>(i);
    }
    return -1;
}

static_assert(vocabularyFor(Key::CursorShape).size() == 3, "cursor shape offers block / underline / bar");
static_assert(vocabularyIndexOf(Key::CursorShape, "bar") == 2, "a term's index is its enum value");
static_assert(vocabularyIndexOf(Key::CursorShape, "diamond") == -1, "a token outside the vocabulary does not resolve");
static_assert(!hasVocabulary(Key::FontFamily), "a free-text key admits no closed vocabulary");
static_assert(!hasVocabulary(Key::WindowDragModifier), "a modifier chord is not a closed list");
static_assert(vocabularyFor("cursor", "shape").size() == 3, "the strings overload resolves the same row");
static_assert(vocabularyFor("cursor", "nonesuch").empty(), "an unknown pair names no preference");

} // namespace Ubiquitty::Preferences
