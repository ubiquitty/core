// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_vocabulary.cpp — pins the closed-vocabulary table
// (Source/Core/Preferences/Vocabulary.h) against the converters that already
// decide what a token means.
//
// The point of the table is that a frontend stops carrying its own copy, so the
// only test worth writing compares it to something INDEPENDENTLY written. Every
// assertion below therefore runs a term through Core's own parser
// (`cursorShapeFromString`, `encodingFromName`, …) rather than against a second
// transcription of the same list, which would agree with a typo.
//
// Three contracts:
//   1. Well-formed: no empty token or label, no duplicate token within a
//      vocabulary, and every term findable by `vocabularyIndexOf`.
//   2. Canonical: each token PARSES, and writing the parsed value back yields
//      that same token — so the table offers what the file will hold, not an
//      alias the writer would immediately respell.
//   3. Index is the enum value: term i parses to the enumerator whose numeric
//      value is i. This is what lets a picker hand back a bare index. An
//      enumerator inserted mid-list without a matching row fails here instead
//      of silently renumbering every frontend's picker.

#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/Vocabulary.h"
#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/FontMetrics.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Unicode/Encodings.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

using Ubiquitty::Preferences::hasVocabulary;
using Ubiquitty::Preferences::Key;
using Ubiquitty::Preferences::SerializeKind;
using Ubiquitty::Preferences::vocabularyFor;
using Ubiquitty::Preferences::vocabularyIndexOf;
using Ubiquitty::Preferences::VocabularyTerm;
using Ubiquitty::Preferences::writeDescriptorCount;
using Ubiquitty::Preferences::writeDescriptors;

namespace {

bool sameString(const char* a, const char* b) { return a && b && std::string_view(a) == std::string_view(b); }

// Contract 2+3 for one enum-backed key, as one call: walk the terms, parse each
// through `fromString`, check the enumerator's numeric value is the term's
// index, and check `toString` of it spells the token back.
//
// Templated on the enum so the two converters keep their real signatures — a
// pair that has drifted apart fails to instantiate rather than being coerced.
template<typename Enum, typename FromString, typename ToString>
void checkEnumVocabulary(Key key, FromString fromString, ToString toString)
{
    const std::span<const VocabularyTerm> terms = vocabularyFor(key);
    CHECK(!terms.empty());
    for (std::size_t i = 0; i < terms.size(); ++i) {
        const auto parsed = fromString(std::string(terms[i].token));
        CHECK(parsed.has_value());
        if (!parsed.has_value())
            continue;
        // The index equals the enum value, so a picker may hand back a bare index.
        CHECK(static_cast<int>(*parsed) == static_cast<int>(i));
        // And the token is the canonical spelling, not an accepted alias.
        CHECK(sameString(toString(*parsed), terms[i].token));
    }
}

} // namespace

// Contract 1, over every vocabulary the table declares: nothing empty, nothing
// duplicated, everything findable. Walks `writeDescriptors` rather than a list
// of keys, so a key that gains a vocabulary is covered without editing this.
TEST(testEveryVocabularyIsWellFormed)
{
    int withVocabulary = 0;
    for (int d = 0; d < writeDescriptorCount; ++d) {
        const Key key = writeDescriptors[d].id;
        const std::span<const VocabularyTerm> terms = vocabularyFor(key);
        if (terms.empty()) {
            CHECK(!hasVocabulary(key));
            continue;
        }
        ++withVocabulary;
        // A vocabulary is a closed token list, so the key must serialize as a
        // token — a vocabulary on a Bool or Double row would be nonsense the
        // writer could not honour.
        CHECK(writeDescriptors[d].kind == SerializeKind::EnumToken);
        for (std::size_t i = 0; i < terms.size(); ++i) {
            CHECK(terms[i].token != nullptr && terms[i].token[0] != '\0');
            CHECK(terms[i].label != nullptr && terms[i].label[0] != '\0');
            // Findable, and at its own index — the lookup returns the FIRST
            // match, so this also catches a duplicated token.
            CHECK(vocabularyIndexOf(key, terms[i].token) == static_cast<int>(i));
        }
    }
    // The table is not vacuously well-formed: it really does cover keys.
    CHECK(withVocabulary == 15);
}

TEST(testBrailleVocabularyRoundTripsInEnumOrder)
{
    using namespace Ubiquitty::Rendering;
    const auto terms = vocabularyFor(Key::FontBrailleStyle);
    CHECK_EQ(terms.size(), 7u);
    for (std::size_t index = 0; index < terms.size(); ++index) {
        const auto style = brailleStyleFromString(terms[index].token);
        CHECK_EQ(static_cast<std::size_t>(style), index);
        CHECK(brailleStyleToString(style) == terms[index].token);
    }
}

TEST(testShadeBlocksVocabularyRoundTripsInEnumOrder)
{
    using namespace Ubiquitty::Rendering;
    const auto terms = vocabularyFor(Key::AppearanceShadeBlocks);
    CHECK_EQ(terms.size(), 2u);
    for (std::size_t index = 0; index < terms.size(); ++index) {
        const auto style = shadeStyleFromString(terms[index].token);
        CHECK_EQ(static_cast<std::size_t>(style), index);
        CHECK(shadeStyleToString(style) == terms[index].token);
    }
}

// A key outside any vocabulary answers empty rather than something arbitrary,
// and the free-text / numeric / toggle majority is genuinely uncovered.
TEST(testKeysWithoutVocabularyAnswerEmpty)
{
    CHECK(!hasVocabulary(Key::FontFamily)); // free text
    CHECK(!hasVocabulary(Key::FontSize)); // number
    CHECK(!hasVocabulary(Key::CursorBlink)); // toggle
    CHECK(vocabularyFor(Key::FontFamily).empty());
    CHECK(vocabularyIndexOf(Key::FontFamily, "Menlo") == -1);
    // A value cast in from outside the enumeration resolves to nothing rather
    // than reading a neighbouring key's list.
    CHECK(vocabularyFor(static_cast<Key>(9999)).empty());
    CHECK(vocabularyIndexOf(Key::CursorShape, nullptr) == -1);
}

// `[window] drag_modifier` is an EnumToken key with no vocabulary. Its value is
// a `+`-separated chord (`control+command`, `shift`, `none`) parsed a token at a
// time by Core's Input::isWindowDragPress, so its admissible values are
// combinations rather than a list, and a list offering some of them would read
// as all of them. Pinned, because an EnumToken key with no terms is otherwise
// indistinguishable from one somebody forgot.
TEST(testDragModifierIsOpenByDesign)
{
    CHECK(!hasVocabulary(Key::WindowDragModifier));
    const auto* descriptor = Ubiquitty::Preferences::descriptorFor(Key::WindowDragModifier);
    CHECK(descriptor != nullptr);
    if (descriptor)
        CHECK(descriptor->kind == SerializeKind::EnumToken);
}

// Contracts 2+3 for the six vocabularies backed by a C++ enum.
TEST(testEnumVocabulariesRoundTripThroughTheirConverters)
{
    using namespace Ubiquitty::Preferences;
    checkEnumVocabulary<CursorShape>(Key::CursorShape, cursorShapeFromString, cursorShapeToString);
    checkEnumVocabulary<CursorMotionMode>(Key::CursorMotion, cursorMotionFromString, cursorMotionToString);
    checkEnumVocabulary<TabShortcutHints>(Key::TabsShowShortcuts, tabShortcutHintsFromString, tabShortcutHintsToString);
    checkEnumVocabulary<TitlebarStyle>(Key::AppearanceTitlebarStyle, titlebarStyleFromString, titlebarStyleToString);
    checkEnumVocabulary<ColorSpace>(Key::AppearanceColorSpace, colorSpaceFromString, colorSpaceToString);
    checkEnumVocabulary<BackgroundImageMode>(
        Key::AppearanceBackgroundImageMode, backgroundImageModeFromString, backgroundImageModeToString);
    checkEnumVocabulary<Ubiquitty::Accessibility::ColorblindMode>(
        Key::AppearanceColorblindMode, colorblindModeFromString, colorblindModeToString);
}

// `[terminal] encoding` has no toString — the value is carried as a string —
// so the round-trip is one-directional: every token must RESOLVE, and to the
// enumerator at its own index.
TEST(testEncodingVocabularyResolvesInEnumOrder)
{
    const std::span<const VocabularyTerm> terms = vocabularyFor(Key::TerminalEncoding);
    CHECK(terms.size() == 10);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        auto resolved = Ubiquitty::Unicode::encodingFromName(terms[i].token);
        CHECK(resolved);
        CHECK(static_cast<int>(*resolved) == static_cast<int>(i));
    }
    // The separators in the offered spellings are cosmetic — the reader drops
    // them — but an offered token that resolved to the WRONG encoding would
    // pass a laxer check, so the index comparison above is the real assertion.
    CHECK(sameString(terms[0].token, "utf-8"));
    CHECK(sameString(terms[9].token, "big5"));
}

// The labels a picker displays, pinned here rather than left to the frontend
// that draws them. A label is what a person recognises the setting by, so
// changing one changes the product, and this is the line that has to move
// with it.
TEST(testLabelsMatchTheShippedPickers)
{
    const std::span<const VocabularyTerm> shape = vocabularyFor(Key::CursorShape);
    CHECK(sameString(shape[0].label, "Block"));
    CHECK(sameString(shape[1].label, "Underline"));
    CHECK(sameString(shape[2].label, "Bar"));

    const std::span<const VocabularyTerm> motion = vocabularyFor(Key::CursorMotion);
    CHECK(sameString(motion[0].label, "Off"));
    CHECK(sameString(motion[3].label, "Always"));

    // The two a title-cased token would have got wrong.
    const std::span<const VocabularyTerm> space = vocabularyFor(Key::AppearanceColorSpace);
    CHECK(sameString(space[0].label, "sRGB"));
    CHECK(sameString(space[1].label, "Display P3"));
}

UBIQUITTY_TEST_MAIN()
