// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_keys.cpp — pins the shared settings-WRITE recipe table
// (Source/Core/Preferences/Key.h) and the platform-override field list
// (Source/Core/Preferences/Profile.h's platformOverrideFields).
//
// Three contracts:
//   1. Coverage: every WriteDescriptor names a key the parser reads, in the
//      kind it reads it — checked by writing each key's default into a document
//      and reading it back by key, so the table is compared with the parser
//      and not with a transcription of itself.
//   2. Round-trip: for a representative key of each kind, descriptorFor() yields
//      the right section/key/kind, and serializeWrite() renders the value into
//      the same TOML literal the matching tomlBool/tomlInt/tomlDouble/tomlString
//      call produced — i.e. the table walk reproduces the recipe.
//   3. Platform overrides: the shared platformOverrideFields list maps each
//      bridge token to its [<plat>] key + kind, the single source the macOS
//      bridge, and the Swift PlatformPane once it consumes them, read.

#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/Profile.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using Ubiquitty::Preferences::descriptorFor;
using Ubiquitty::Preferences::effectiveValue;
using Ubiquitty::Preferences::Key;
using Ubiquitty::Preferences::PlatformFieldKind;
using Ubiquitty::Preferences::platformOverrideFieldCount;
using Ubiquitty::Preferences::platformOverrideFieldForToken;
using Ubiquitty::Preferences::platformOverrideFields;
using Ubiquitty::Preferences::SerializeKind;
using Ubiquitty::Preferences::serializeWrite;
using Ubiquitty::Preferences::Value;
using Ubiquitty::Preferences::Values;
using Ubiquitty::Preferences::WriteDescriptor;
using Ubiquitty::Preferences::writeDescriptorCount;
using Ubiquitty::Preferences::writeDescriptors;
using Ubiquitty::Preferences::writeValueIntoBody;

static bool sameString(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// Whether two key-addressed values say the same thing, in the slot their kind
// reads.
static bool sameValue(const Value& a, const Value& b)
{
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case SerializeKind::Bool:
        return a.boolean == b.boolean;
    case SerializeKind::LegacyBoolOrToken:
        return a.boolean == b.boolean && a.text == b.text;
    case SerializeKind::Int:
        return a.integer == b.integer;
    case SerializeKind::Double:
        return a.number == b.number;
    case SerializeKind::EnumToken:
    case SerializeKind::String:
        return a.text == b.text;
    }
    return false;
}

// Contract 1: every descriptor names a key the parser reads, in the kind the
// parser reads it. Each key's default is written into an empty document under
// the descriptor's own section, name and serializer, parsed strictly, and read
// back by key: a section or a name the parser does not know is a diagnostic,
// and a kind it does not read is a diagnostic or a different value. Derived
// from the table and the parser alone, so neither can drift from a copy.
TEST(testEveryDescriptorRoundTripsThroughADocument)
{
    for (int i = 0; i < writeDescriptorCount; ++i) {
        const WriteDescriptor& descriptor = writeDescriptors[i];
        const std::optional<Value> fallback = effectiveValue(descriptor.id, Values {});
        CHECK_MSG(fallback.has_value(), descriptor.key);
        if (!fallback)
            continue;
        const std::optional<std::string> document = writeValueIntoBody(descriptor.id, *fallback, std::string());
        CHECK_MSG(document.has_value(), descriptor.key);
        if (!document)
            continue;
        Values parsed;
        std::istringstream in(*document);
        const bool didParse = Ubiquitty::Preferences::parse(in, parsed, Ubiquitty::Preferences::ParseMode::Strict);
        CHECK_MSG(didParse && parsed.skippedKeys.empty(), document->c_str());
        const std::optional<Value> readBack = effectiveValue(descriptor.id, parsed);
        CHECK_MSG(readBack.has_value() && sameValue(*readBack, *fallback), document->c_str());
    }
}

// Every row's id is unique and every row is itself reachable through
// descriptorFor — guards a copy/paste that duplicates an enumerator or strands
// a row the lookup never returns.
TEST(testTableRowsAreUniqueAndReachable)
{
    for (int i = 0; i < writeDescriptorCount; ++i) {
        const WriteDescriptor* viaLookup = descriptorFor(writeDescriptors[i].id);
        CHECK(viaLookup == &writeDescriptors[i]); // first match is this row
    }
    for (int i = 0; i < writeDescriptorCount; ++i)
        for (int j = i + 1; j < writeDescriptorCount; ++j)
            CHECK(writeDescriptors[i].id != writeDescriptors[j].id);
}

// Contract 2: the table walk reproduces the hand-coded recipe. For a
// representative key of each kind, descriptorFor + serializeWrite emit the same
// section/key/literal the pre-extraction `writeKeyValue("…", tomlXxx(v))` did.
TEST(testRoundTripMatchesHandCodedRecipe)
{
    // bool: [selection] copy_on_select = true  ← tomlBool(true)
    {
        const WriteDescriptor* d = descriptorFor(Key::SelectionCopyOnSelect);
        CHECK(d && sameString(d->section, "selection") && sameString(d->key, "copy_on_select"));
        CHECK(serializeWrite(d->kind, true) == Ubiquitty::Preferences::tomlBool(true));
        CHECK(serializeWrite(d->kind, true) == "true");
    }
    // int: [font] size = 14  ← tomlInt(14)
    {
        const WriteDescriptor* d = descriptorFor(Key::FontSize);
        CHECK(d && sameString(d->section, "font") && sameString(d->key, "size"));
        CHECK(serializeWrite(d->kind, (long long)14) == Ubiquitty::Preferences::tomlInt(14));
        CHECK(serializeWrite(d->kind, (long long)14) == "14");
    }
    // double: [appearance] background_opacity = 0.85  ← tomlDouble(0.85)
    {
        const WriteDescriptor* d = descriptorFor(Key::AppearanceBackgroundOpacity);
        CHECK(d && sameString(d->section, "appearance") && sameString(d->key, "background_opacity"));
        CHECK(serializeWrite(d->kind, 0.85) == Ubiquitty::Preferences::tomlDouble(0.85));
        CHECK(serializeWrite(d->kind, 0.85) == "0.85");
    }
    // enum-token: [cursor] shape = "bar"  ← tomlString("bar")
    {
        const WriteDescriptor* d = descriptorFor(Key::CursorShape);
        CHECK(d && sameString(d->section, "cursor") && sameString(d->key, "shape"));
        CHECK(serializeWrite(d->kind, std::string_view("bar")) == Ubiquitty::Preferences::tomlString("bar"));
        CHECK(serializeWrite(d->kind, std::string_view("bar")) == "\"bar\"");
    }
    // string: [term] program = "Ubiquitty"  ← tomlString("Ubiquitty")
    {
        const WriteDescriptor* d = descriptorFor(Key::TermProgram);
        CHECK(d && sameString(d->section, "term") && sameString(d->key, "program"));
        CHECK(serializeWrite(d->kind, std::string_view("Ubiquitty")) == Ubiquitty::Preferences::tomlString("Ubiquitty"));
        CHECK(serializeWrite(d->kind, std::string_view("Ubiquitty")) == "\"Ubiquitty\"");
    }
}

// Contract 3: the platform-override field list maps each bridge token to its
// [<plat>] key + kind — the table the macOS application bridge and the Swift
// platformFields array read.
TEST(testPlatformOverrideFieldList)
{
    struct ExpectedField {
        const char* token;
        const char* key;
        PlatformFieldKind kind;
    };
    static const ExpectedField expected[] = {
        { "theme", "theme", PlatformFieldKind::String },
        { "fontFamily", "font_family", PlatformFieldKind::String },
        { "fallbackFont", "font_fallback", PlatformFieldKind::String },
        { "fontSize", "font_size", PlatformFieldKind::Integer }, // read back as int
        { "lineSpacing", "line_spacing", PlatformFieldKind::Number },
        { "paddingLeft", "padding_left", PlatformFieldKind::Number },
        { "paddingRight", "padding_right", PlatformFieldKind::Number },
        { "paddingTop", "padding_top", PlatformFieldKind::Number },
        { "titlelessPaddingTop", "titleless_padding_top", PlatformFieldKind::Number },
        { "paddingBottom", "padding_bottom", PlatformFieldKind::Number },
        { "startupHost", "startup_host", PlatformFieldKind::String },
    };
    const int n = static_cast<int>(sizeof(expected) / sizeof(expected[0]));
    CHECK(platformOverrideFieldCount == n);
    for (int i = 0; i < n; ++i) {
        const auto* got = platformOverrideFieldForToken(expected[i].token);
        CHECK(got != nullptr);
        if (!got)
            continue;
        CHECK(sameString(got->tomlKey, expected[i].key));
        CHECK(got->kind == expected[i].kind);
    }
    // font_size is the one integer key (rounded on write); the others are
    // numbers or strings — the bridge's numeric-vs-string branch reads this.
    CHECK(platformOverrideFieldForToken("fontSize")->kind == PlatformFieldKind::Integer);
    CHECK(platformOverrideFieldForToken("lineSpacing")->kind == PlatformFieldKind::Number);
    CHECK(platformOverrideFieldForToken("theme")->kind == PlatformFieldKind::String);
    // An unknown token resolves to nullptr so the bridge rejects the write.
    CHECK(platformOverrideFieldForToken("notAField") == nullptr);
    // The table row order matches the editor's display order (the constexpr
    // list is what a generated frontend would iterate).
    CHECK(sameString(platformOverrideFields[0].token, "theme"));
    CHECK(sameString(platformOverrideFields[n - 1].token, "startupHost"));
}

UBIQUITTY_TEST_MAIN()
