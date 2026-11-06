// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_toml_conformance — what the config grammar accepts, and what it
// accepts while getting wrong.
//
// The file users edit is TOML by extension and by the template Ubiquitty writes
// for them, and the parser is hand-rolled. Refusing a construct is a defensible
// choice for a subset; the cases that matter are the ones where a construct is
// ACCEPTED and produces something other than what TOML says it means, because a
// rejection is a diagnosable event and a wrong value is a setting that silently
// is not the one the user wrote.
//
// Every case carries a control: a parse that changes nothing leaves the default
// in place, so a default here reads as "never applied" rather than as "applied
// and happened to match". `size` is 12 by default and every positive case uses
// 13 for that reason.
//
// One of these still pins behaviour that disagrees with TOML — duplicate keys.
// It is written as the parser behaves rather than as it should, so a repair is a
// visible diff instead of a silent one: a case asserting the wrong answer fails
// the moment somebody writes the right one, and nobody has to notice on their
// own. Two earlier pins were repaired that way rather than by being read.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesDefaults.h"

#include <sstream>
#include <string>

namespace {

struct Parsed {
    bool isAccepted;
    std::string family;
    double size;
};

/// Parses `body` in Strict mode from a fresh config.
Parsed parseStrict(const std::string& body)
{
    Ubiquitty::Preferences::Values config;
    std::istringstream input(body);
    const bool isAccepted = Ubiquitty::Preferences::parse(input, config, Ubiquitty::Preferences::ParseMode::Strict);
    return { isAccepted, config.fontFamily.value_or(""), config.fontSize.value_or(Ubiquitty::Preferences::Defaults::fontSize) };
}

/// The value `size` reads as when nothing assigned it.
///
/// Read from the shared default rather than from the field: an unassigned field
/// is empty now, and the value it stands in for lives at the read.
double defaultSize() { return Ubiquitty::Preferences::Defaults::fontSize; }

} // namespace

TEST(theControlParses)
{
    // Without this every rejection below could be a broken fixture rather than a
    // refusal, and they would read the same.
    const Parsed ok = parseStrict("[font]\nsize = 13\n");
    CHECK(ok.isAccepted);
    CHECK_EQ(ok.size, 13.0);
    CHECK(defaultSize() != 13.0);
}

TEST(aScalarEscapeBecomesItsCharacter)
{
    // The four-digit form and the eight-digit one, and a code point past ASCII
    // so the result is the UTF-8 encoding rather than a single byte that a
    // truncating implementation would also produce.
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\u0041b\"\n").family, "aAb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\U00000041b\"\n").family, "aAb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"\\u00E9\"\n").family, "\xC3\xA9"); // é
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"\\U0001F600\"\n").family, "\xF0\x9F\x98\x80");
}

TEST(theOtherTwoCharacterEscapesAreTheirCharacters)
{
    // All six of TOML's two-character escapes, asserted together because one
    // switch decides them: a rewrite that drops the backslash for any of them
    // drops it for all six, and covering only the common two would leave that
    // invisible.
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\bb\"\n").family, "a\bb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\fb\"\n").family, "a\fb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\tb\"\n").family, "a\tb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\nb\"\n").family, "a\nb");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\\"b\"\n").family, "a\"b");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"a\\\\b\"\n").family, "a\\b");
}

TEST(aScalarEscapeThatNamesNoCharacterIsRefused)
{
    // A surrogate and a value past the last plane are not characters, and the
    // encoder emits nothing for either — so admitting them would swallow the
    // escape and store a shorter string than the file describes, which is the
    // failure this whole arm exists to stop.
    CHECK(!parseStrict("[font]\nfamily = \"\\uD800\"\n").isAccepted);
    CHECK(!parseStrict("[font]\nfamily = \"\\U00110000\"\n").isAccepted);

    // And a malformed one: a non-hexadecimal digit, and a form cut short by the
    // closing quote.
    CHECK(!parseStrict("[font]\nfamily = \"\\u00G1\"\n").isAccepted);
    CHECK(!parseStrict("[font]\nfamily = \"\\u00\"\n").isAccepted);
}

TEST(onlyWhitespaceOrACommentMayFollowAQuotedValue)
{
    // A quoted value ends at its closing quote, and reading the first quoted
    // run while discarding the rest of the line is how `"""Menlo"""` came to
    // parse as the empty string between the first two quotes.
    CHECK(!parseStrict("[font]\nfamily = \"a\" b\n").isAccepted);
    CHECK(!parseStrict("[font]\nfamily = \"a\"\"b\"\n").isAccepted);

    // A comment may follow: the grammar allows one, and the refusals above
    // must not take it away.
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"Menlo\" # the bundled face\n").family, "Menlo");
    CHECK_STR_EQ(parseStrict("[font]\nfamily = \"Menlo\"   \n").family, "Menlo");
}

TEST(aMultiLineStringIsRefusedRatherThanReadAsEmpty)
{
    // TOML's `"""` delimiter makes this family `Menlo`, and this grammar has no
    // multi-line string. Refusing says so; the alternative reads the empty run
    // between the first two quotes and stores nothing — and an empty family is
    // the "use the bundled face" setting, so the loss would be indistinguishable
    // from a choice.
    const Parsed tripleQuoted = parseStrict("[font]\nfamily = \"\"\"Menlo\"\"\"\n");
    CHECK(!tripleQuoted.isAccepted);
    CHECK(tripleQuoted.family.empty());

    // The single-quoted form is refused too, so the pair is consistent about a
    // delimiter the grammar does not carry rather than refusing one and reading
    // the other wrongly.
    CHECK(!parseStrict("[font]\nfamily = '''Menlo'''\n").isAccepted);
}

TEST(aDuplicateKeyIsTakenRatherThanRefusedAndTheLastOneWins)
{
    // TOML makes a duplicate key an error. This parser accepts the file and lets
    // the later assignment stand, so a config that says two different things
    // about one setting is a config, and which value applies depends on line
    // order rather than on anything the file states.
    const Parsed duplicated = parseStrict("[font]\nsize = 13\nsize = 14\n");
    CHECK(duplicated.isAccepted);
    CHECK_EQ(duplicated.size, 14.0);

    // Reversed, so the assertion above is about order rather than about 14 being
    // the value a duplicate happens to produce.
    const Parsed reversed = parseStrict("[font]\nsize = 14\nsize = 13\n");
    CHECK(reversed.isAccepted);
    CHECK_EQ(reversed.size, 13.0);
}

TEST(theGrammarIsASubsetAndTheseAreTheEdges)
{
    // Refusals, which are the safe half. Recorded so a parser replacement is
    // measured against what this one admits rather than against the template.
    CHECK(!parseStrict("[font]\nfamily = 'Menlo'\n").isAccepted); // literal string
    CHECK(!parseStrict("[font]\nsize = 1_3\n").isAccepted); // digit separator
    CHECK(!parseStrict("[font]\nsize = 0x0d\n").isAccepted); // hexadecimal
    CHECK(!parseStrict("[font]\nfeatures = [\n  \"calt\"\n]\n").isAccepted); // array over lines
    CHECK(!parseStrict("[font]\nx = { style = \"bar\" }\n").isAccepted); // inline table
    CHECK(!parseStrict("[font]\na.b = 13\n").isAccepted); // dotted key
    CHECK(!parseStrict("[font]\n\"a b\" = 13\n").isAccepted); // quoted key
    CHECK(!parseStrict("[font.sub]\nsize = 13\n").isAccepted); // dotted section

    // And none of them left a value behind, which is what separates a refusal
    // from the two cases above.
    CHECK_EQ(parseStrict("[font]\nsize = 0x0d\n").size, defaultSize());
    CHECK(parseStrict("[font]\nfamily = 'Menlo'\n").family.empty());
}

UBIQUITTY_TEST_MAIN()
