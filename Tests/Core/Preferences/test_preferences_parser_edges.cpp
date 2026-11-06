// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The config parser's refusals, and the fields nothing else reaches.
//
// A config file is untrusted in the way that matters here: a user edits it by
// hand, and the same text arrives over the wire on the capability-gated CONFIG
// frame. So every malformed shape has to leave a usable configuration rather
// than a half-applied one, and the parser's answer to "I do not understand
// this line" has to be "skip it" rather than "stop", because stopping loses
// every setting below the typo.
//
// The cases are grouped by what goes wrong: inside a quoted string, inside an
// array, in a section header, in a body line, and in the per-section field
// handlers that only one spelling of one key ever reaches.

#include "Harness.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace Ubiquitty::Preferences;

namespace {

Values parseString(const std::string& text)
{
    Values configuration;
    std::istringstream in(text);
    parse(in, configuration);
    return configuration;
}

// A file that exists for the length of one case, and removes itself.
class TemporaryFile {
public:
    explicit TemporaryFile(const std::string& contents)
    {
        char pattern[] = "/tmp/ubiquitty-config-XXXXXX";
        const int descriptor = ::mkstemp(pattern);
        if (descriptor >= 0) {
            ::close(descriptor);
            m_path = pattern;
            std::ofstream out(m_path, std::ios::binary);
            out << contents;
        }
    }
    ~TemporaryFile()
    {
        if (!m_path.empty())
            ::unlink(m_path.c_str());
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

} // namespace

// A quoted value is scanned escape by escape. An escape the parser does not
// know keeps its literal character rather than being dropped, so a config
// written for a later version reads sensibly here instead of losing text.
TEST(testQuotedStringEscapesAndTheUnterminatedCase)
{
    // The escapes with a meaning, including the carriage return no other case
    // uses.
    const Values escapes = parseString("[font]\nfamily = \"a\\rb\\nc\\td\\\\e\\\"f\"\n");
    CHECK(escapes.fontFamily->find('\r') != std::string::npos);
    CHECK(escapes.fontFamily->find('\n') != std::string::npos);
    CHECK(escapes.fontFamily->find('\t') != std::string::npos);
    CHECK(escapes.fontFamily->find('\\') != std::string::npos);
    CHECK(escapes.fontFamily->find('"') != std::string::npos);

    // An escape nobody defines: the character survives, the backslash does not.
    const Values unknown = parseString("[font]\nfamily = \"a\\qb\"\n");
    CHECK_MSG(unknown.fontFamily == "aqb", "an unknown escape keeps its character");

    // A string with no closing quote: the line is refused, and the parse
    // continues rather than swallowing the rest of the file.
    const Values unterminated = parseString(
        "[font]\nfamily = \"unterminated\nsize = 17\n");
    CHECK_MSG(unterminated.fontSize == 17, "the setting after an unterminated string still applies");

    // A unicode escape naming an unpaired surrogate is refused: it is not a
    // scalar value, so no encoding of it exists.
    const Values surrogate = parseString("[font]\nfamily = \"\\uD800\"\nsize = 13\n");
    CHECK(surrogate.fontSize == 13);
}

// The value scanner's edges: nothing after the equals sign, an empty array, an
// array holding something that is not a string, and trailing text after a
// closing quote.
TEST(testValueScannerEdges)
{
    // Each of these is a line the parser must skip while keeping the rest.
    const char* const malformed[] = {
        "[font]\nfamily =\nsize = 11\n", // empty value
        "[[themes]]\nname = \"t\"\nansi = []\n[font]\nsize = 11\n", // empty array
        "[[themes]]\nname = \"t\"\nansi = [1]\n[font]\nsize = 11\n", // non-string element
        "[[themes]]\nname = \"t\"\nansi = [\"\\uD800\"]\n[font]\nsize = 11\n", // bad element
        "[[themes]]\nname = \"t\"\nansi = [\"a\" b]\n[font]\nsize = 11\n", // trailing garbage
        "[font]\nfamily = \"x\" trailing\nsize = 11\n", // trailing after a quote
    };
    for (const char* text : malformed) {
        const Values configuration = parseString(text);
        CHECK_MSG(configuration.fontSize == 11, "a malformed value does not stop the parse");
    }

    // A comment after a value is not part of it, and a value that is nothing
    // but a comment is empty rather than the comment text.
    const Values baseline;
    CHECK(parseString("[font]\nsize = 15 # the size\n").fontSize == 15);
    CHECK(parseString("[font]\nsize = # nothing but a comment\n").fontSize == baseline.fontSize);

    // A float that overflows to infinity is refused rather than stored: a
    // non-finite size reaches layout and lays out nothing.
    for (const char* text : {
             "[font]\nsize = 1e999\n", // overflows to infinity
             "[font]\nsize = -1e999\n",
             "[font]\nsize = 12.5.5\n", // trailing garbage after a float
             "[font]\nsize = 12abc\n",
         }) {
        CHECK_MSG(parseString(text).fontSize == baseline.fontSize, "a malformed number is refused");
    }
    // And a real float still applies.
    CHECK(parseString("[font]\nsize = 12.5\n").fontSize == 12.5);
}

// [sound] round trip. The two keys reach the model through different tables —
// the bool through the plain binder, the volume through the clamped one that
// reads its range from Schema.h — so a key can be declared, documented and
// defaulted and still not arrive. This is the parse actually happening.
TEST(testSoundSectionParsesAndClamps)
{
    // Unset is the default, which is what the config template shows commented.
    const Values unset = parseString("[font]\nsize = 11\n");
    CHECK(Effective::soundAudible(unset) == Defaults::soundAudible);
    CHECK(Effective::soundVolume(unset) == Defaults::soundVolume);

    const Values set = parseString("[sound]\naudible = false\nvolume = 0.4\n");
    CHECK(set.soundAudible.has_value() && *set.soundAudible == false);
    CHECK(Effective::soundAudible(set) == false);
    CHECK(set.soundVolume.has_value());
    CHECK(std::fabs(Effective::soundVolume(set) - 0.4) < 1e-9);

    // The schema's range is the clamp, so a config cannot ask for more level
    // than the synth has or for a negative one.
    const Values loud = parseString("[sound]\nvolume = 4.0\n");
    CHECK(std::fabs(Effective::soundVolume(loud) - 1.0) < 1e-9);
    const Values negative = parseString("[sound]\nvolume = -2.0\n");
    CHECK(std::fabs(Effective::soundVolume(negative) - 0.0) < 1e-9);
}

// Section headers are how the parser decides which handler a line belongs to,
// so a malformed one must not silently leave the previous section selected —
// a following key would then be applied to whatever section came before.
TEST(testMalformedSectionHeadersAreRefused)
{
    const Values baseline;
    const char* const headers[] = {
        "[abc\nsize = 9\n", // no closing bracket at all
        "[]\nsize = 9\n", // an empty name
        "[[incomplete\nsize = 9\n", // an unterminated array-of-tables
        "[[bogus]]\nsize = 9\n", // an array-of-tables nobody defines
        "[unknown.section]\nsize = 9\n", // a single header nobody defines
    };
    for (const char* text : headers) {
        const Values configuration = parseString(text);
        // The parse survives, and the stray key did not land on the font
        // section through a header that was never opened.
        CHECK_MSG(configuration.fontSize == baseline.fontSize,
            "a key under a refused header does not apply");
    }

    // And a well-formed header still works after any of them.
    CHECK(parseString("[[bogus]]\nx = 1\n[font]\nsize = 21\n").fontSize == 21);
}

// A body line that is not blank, not a comment and not a header must be
// `key = value`. The two ways it can fail are a missing separator and a
// missing key, and both have to be skipped rather than stored under an empty
// name.
TEST(testBodyLinesNeedAKeyAndASeparator)
{
    const Values configuration = parseString(
        "[font]\n"
        "garbage no equals sign\n"
        "= value with no key\n"
        "   \n"
        "# a comment\n"
        "size = 19\n");
    CHECK_MSG(configuration.fontSize == 19, "the good line after two bad ones applies");
}

// A TOML array may be split across lines. The parser folds the continuation
// back into one value by tracking bracket and quote depth, which has to
// survive a CRLF file, a blank line inside the array, an escaped quote inside
// an element, and a comment line between elements.
TEST(testMultiLineArraysFoldBackTogether)
{
    // One line, with an escaped quote inside an element.
    const Values escaped = parseString("[[themes]]\nname = \"t\"\nansi = [\"a\\\"b\", \"#000000\"]\n");
    CHECK(!escaped.themes.empty());

    // Split across lines, CRLF-terminated, with a blank line and a comment
    // inside the array.
    const Values folded = parseString(
        "[[themes]]\r\n"
        "name = \"multi\"\r\n"
        "ansi = [\r\n"
        "\r\n"
        "  \"#000000\", # the first slot\r\n"
        "  \"#111111\"\r\n"
        "]\r\n"
        "background = \"#222222\"\r\n");
    CHECK(!folded.themes.empty());
    if (!folded.themes.empty())
        CHECK_MSG(folded.themes[0].name == "multi", "the fold did not consume the name above it");

    // A nested array inside the continuation: the depth counter has to go up
    // as well as down, or the fold ends at the first inner close bracket and
    // the rest of the array is read as body lines.
    const Values nested = parseString(
        "[[themes]]\n"
        "name = \"nested\"\n"
        "ansi = [\n"
        "  [\"#000000\"],\n"
        "  \"#111111\"\n"
        "]\n"
        "background = \"#222222\"\n");
    CHECK(!nested.themes.empty());
    if (!nested.themes.empty())
        CHECK_MSG(nested.themes[0].name == "nested", "the fold spans the inner array");

    // An array left open at end of input: refused, and it does not hang.
    const Values unclosed = parseString("[[themes]]\nname = \"open\"\nansi = [\n  \"#000000\"\n");
    CHECK(unclosed.themes.size() <= 1);
}

// `[tabs] show_shortcuts` takes its three tokens and nothing else: neither a
// bool nor another word for one of them.
TEST(testTabShortcutHintsAcceptsOnlyItsTokens)
{
    const struct {
        const char* value;
        bool isAccepted; // sets the field; a refused word leaves it unset
    } spellings[] = {
        { "\"off\"", true },
        { "\"always\"", true },
        { "\"command\"", true },
        { "true", false },
        { "false", false },
        { "\"on\"", false },
        { "\"none\"", false },
        { "\"cmd\"", false },
        { "\"hold\"", false },
        { "\"nonsense\"", false },
    };
    for (const auto& spelling : spellings) {
        const std::string text = std::string("[tabs]\nshow_shortcuts = ") + spelling.value + "\n";
        const Values configuration = parseString(text);
        // A refused line is skipped: the field it names is never set, so the
        // value the rest of the program reads is the default a fresh parse
        // gives, not a half-applied one. An accepted word sets the field.
        if (spelling.isAccepted)
            CHECK_MSG(configuration.tabShortcutHints.has_value(), spelling.value);
        else
            CHECK_MSG(!configuration.tabShortcutHints.has_value() && configuration.skippedKeys.size() == 1,
                spelling.value);
    }
}

// The font range map is the one place a config names raw code points, so the
// hex parser is the one place a typo becomes a range covering the wrong half
// of Unicode.
TEST(testFontRangeMapCodePointParsing)
{
    // A well-formed entry survives.
    const Values good = parseString(
        "[[font.range_map]]\nrange = \"0400-04FF\"\nmode = \"custom\"\nfamily = \"Cyrillic Face\"\n");
    CHECK_MSG(!good.rangeMap.empty(), "a valid range is kept");

    // Each malformed spelling is dropped rather than stored as a range that
    // would then claim code points it was never given.
    const char* const bad[] = {
        "[[font.range_map]]\nrange = \"GG\"\nmode = \"custom\"\nfamily = \"X\"\n", // not hex
        "[[font.range_map]]\nrange = \"FFFFFF\"\nmode = \"custom\"\nfamily = \"X\"\n", // past the last plane
        "[[font.range_map]]\nrange = \"\"\nmode = \"custom\"\nfamily = \"X\"\n", // empty
        "[[font.range_map]]\nrange = \"0400-\"\nmode = \"custom\"\nfamily = \"X\"\n", // no end
        "[[font.range_map]]\nrange = \"-04FF\"\nmode = \"custom\"\nfamily = \"X\"\n", // no start
        "[[font.range_map]]\nrange = \"04FF-0400\"\nmode = \"custom\"\nfamily = \"X\"\n", // inverted
        "[[font.range_map]]\nrange = \"0400-04FF\"\nmode = \"custom\"\n", // custom mode with no family
        "[[font.range_map]]\nrange = \"0400-04FF\"\nmode = \"bogus\"\nfamily = \"X\"\n", // unknown mode
    };
    for (const char* text : bad) {
        const Values configuration = parseString(text);
        CHECK_MSG(configuration.rangeMap.empty(), "a malformed range entry is dropped");
    }

    // A single code point, with no dash, is a range of one.
    const Values single = parseString(
        "[[font.range_map]]\nrange = \"1F600\"\nmode = \"custom\"\nfamily = \"Emoji Face\"\n");
    CHECK(!single.rangeMap.empty());
}

// Language fallbacks name a script and a mode. An entry naming a script or a
// mode nobody defines is dropped — keeping it would put a fallback in the
// resolver that can never match.
//
// `language` is not required, and an entry without one is kept. The resolver
// matches a script fallback on its script: FontFallbackPolicy consults
// `preference.script` in both the Han and the non-Han stage and reads
// `preference.language` in neither, so a language-less entry matches exactly as
// well as one carrying a tag. Requiring it dropped entries that would have
// routed, for a value nothing reads.
TEST(testLanguageFallbackValidation)
{
    const Values good = parseString(
        "[[font.language_fallbacks]]\nlanguage = \"zh\"\nscript = \"han\"\nmode = \"auto\"\n");
    CHECK_MSG(!good.languageFallbacks.empty(), "a valid fallback is kept");

    // A non-string value for any field of either block is ignored rather than
    // stored: the fields are names and modes, and a number in one of them
    // would resolve against nothing.
    const Values numeric = parseString(
        "[[font.language_fallbacks]]\nlanguage = 42\nscript = 7\nmode = 1\nfamily = 3\n");
    CHECK_MSG(numeric.languageFallbacks.empty(), "a numeric language fallback is dropped");
    const Values numericRange = parseString(
        "[[font.range_map]]\nrange = 42\nmode = 7\nfamily = 3\n");
    CHECK(numericRange.rangeMap.empty());

    const char* const bad[] = {
        "[[font.language_fallbacks]]\nlanguage = \"zh\"\nscript = \"han\"\nmode = \"bogus\"\n", // unknown mode
        "[[font.language_fallbacks]]\nlanguage = \"zh\"\nscript = \"klingon\"\nmode = \"auto\"\n", // unknown script
        "[[font.language_fallbacks]]\nlanguage = \"zh\"\nscript = \"han\"\nmode = \"custom\"\n", // custom naming no family
    };
    for (const char* text : bad) {
        const Values configuration = parseString(text);
        CHECK_MSG(configuration.languageFallbacks.empty(), "an invalid fallback is dropped");
    }

    // A script fallback with no language is kept, because the route matches it
    // on its script alone. This is the entry a font-routing editor writes when
    // the reader named a script and a family and nothing else, and dropping it
    // made that editor's writes vanish.
    const Values noLanguage = parseString(
        "[[font.language_fallbacks]]\nscript = \"arabic\"\nmode = \"custom\"\nfamily = \"Geeza Pro\"\n");
    CHECK_MSG(noLanguage.languageFallbacks.size() == 1, "a language-less script fallback is kept");
    if (noLanguage.languageFallbacks.size() == 1) {
        CHECK(noLanguage.languageFallbacks[0].script == "arabic");
        CHECK(noLanguage.languageFallbacks[0].family == "Geeza Pro");
        CHECK(noLanguage.languageFallbacks[0].language.empty());
    }
}

// A theme may name its palette slots one at a time as `ansiN`. Nothing used
// that spelling, so the index parse and its bounds were dark — and an index
// the guard lets through would write past the palette.
TEST(testThemeAnsiSlotShorthand)
{
    const Values configuration = parseString(
        "[[themes]]\nname = \"slots\"\nansi0 = \"#ff0000\"\nansi15 = \"#ffffff\"\n");
    CHECK(!configuration.themes.empty());

    // Out of range and non-numeric suffixes are refused rather than indexed.
    for (const char* text : {
             "[[themes]]\nname = \"x\"\nansi16 = \"#000000\"\n",
             "[[themes]]\nname = \"x\"\nansi99 = \"#000000\"\n",
             "[[themes]]\nname = \"x\"\nansix = \"#000000\"\n",
             "[[themes]]\nname = \"x\"\nansi0 = \"not-a-colour\"\n",
         }) {
        const Values refused = parseString(text);
        CHECK_MSG(!refused.themes.empty(), "the theme survives a refused slot");
    }
}

// Profile font fallback, key bindings written as bare tokens, and character
// classes written as quoted keys: three field handlers whose other spelling is
// covered and whose own is not.
TEST(testProfileAndKeyFieldSpellings)
{
    const Values fallback = parseString("[[profiles]]\nname = \"p\"\nfont_fallback = \"Menlo\"\n");
    CHECK_MSG(!fallback.profiles.empty(), "the profile survives its fallback field");

    // A character class written with a quoted key, which is the spelling the
    // bare-token branch beside it does not take.
    const Values quotedClass = parseString("[characterClass]\n\"word\" = \"[A-Za-z]+\"\n");
    CHECK_MSG(!quotedClass.characterClasses.empty(), "a quoted character-class key is accepted");
    if (!quotedClass.characterClasses.empty())
        CHECK_MSG(quotedClass.characterClasses[0].name == "word", "the quotes are not part of the name");
    const Values bareClass = parseString("[characterClass]\nword = \"[A-Za-z]+\"\n");
    CHECK(!bareClass.characterClasses.empty());

    // A binding key written bare, rather than quoted.
    const Values bare = parseString("[keys]\nctrl+a = \"send:hi\"\n");
    CHECK_MSG(!bare.keyBindings.empty(), "a bare key token binds");

    // And one written quoted, which is the covered spelling — asserted beside
    // it so the pair cannot both be satisfied by ignoring the key.
    const Values quoted = parseString("[keys]\n\"ctrl+b\" = \"send:hi\"\n");
    CHECK(!quoted.keyBindings.empty());
}

// The scripting-rule parser reads the output of a user's script, so every line
// of it is untrusted text. A line it cannot read is skipped rather than
// ending the parse, because the directives after it are still wanted.
TEST(testScriptingRulesSkipUnreadableLines)
{
    ScriptingRules rules;
    const int accepted = parseScriptingRules(
        "bareword\n" // no separator at all
        "trigger keyonly\n" // still no separator
        "trigger =value\n" // an empty key
        "\n"
        "# comment\n",
        rules);
    CHECK_MSG(accepted == 0, "no unreadable line is accepted");
}

// Padding is resolved per edge and clamped to what a window can carry: a
// config naming a negative or enormous padding must not hand the renderer an
// inset it will subtract from the content area forever.
TEST(testResolvedPaddingIsClampedPerEdge)
{
    const Values negative = parseString(
        "[appearance]\npadding_left = -10\npadding_right = -10\npadding_top = -10\npadding_bottom = -10\n");
    const Values::PaddingEdges low = negative.resolvedPadding();
    CHECK(low.left >= 0 && low.right >= 0 && low.top >= 0 && low.bottom >= 0);

    const Values enormous = parseString(
        "[appearance]\npadding_left = 9999\npadding_right = 9999\npadding_top = 9999\npadding_bottom = 9999\n");
    const Values::PaddingEdges high = enormous.resolvedPadding();
    CHECK(high.left <= 64 && high.right <= 64 && high.top <= 64 && high.bottom <= 64);
}

// Loading from disk: a path that is not there, and a file with nothing in it.
// Both have to answer rather than throw, because the caller is a startup path
// with no user to ask.
TEST(testConfigureFromFileAnswersForMissingAndEmptyFiles)
{
    const std::string previous = liveDocument();
    CHECK_MSG(configureFromFile("/definitely/does/not/exist/ubiquitty.toml", ParseMode::Strict).isApplied,
        "a missing file is the empty document");
    CHECK(!values().fontSize.has_value());

    const TemporaryFile empty("");
    CHECK(!empty.path().empty());
    if (!empty.path().empty())
        CHECK_MSG(configureFromFile(empty.path(), ParseMode::Strict).isApplied, "an empty file configures the defaults");

    const TemporaryFile real("[font]\nsize = 23\n");
    if (!real.path().empty()) {
        CHECK(configureFromFile(real.path(), ParseMode::Strict).isApplied);
        CHECK_MSG(values().fontSize == 23, "a real file's setting arrives");
    }
    (void)configure(previous, ParseMode::Permissive);
}

// The plain enum-to-int converters, which the round-trip cases skip because
// they go through the string spellings instead. An int cast that stopped
// matching its enum would move every value stored by index.
TEST(testEnumToIntConvertersAgreeWithTheirEnums)
{
    CHECK(backgroundImageModeToInt(BackgroundImageMode::Stretch)
        == static_cast<int>(BackgroundImageMode::Stretch));
    CHECK(cursorMotionToInt(CursorMotionMode::Short)
        == static_cast<int>(CursorMotionMode::Short));
    CHECK(colorblindModeToInt(Ubiquitty::Accessibility::ColorblindMode::None)
        == static_cast<int>(Ubiquitty::Accessibility::ColorblindMode::None));

    // The process-wide current config: replacing it is what a settings apply
    // does, and resolving its bindings is what the input layer reads.
    const std::string previous = liveDocument();
    (void)configure("[keys]\nctrl+q = \"send:bye\"\n", ParseMode::Strict);
    const std::vector<KeyBindingEntry> bindings = resolveCurrentKeyBindings(values());
    CHECK(!bindings.empty());
    (void)configure(previous, ParseMode::Permissive); // leave the process as it was found
}

// A skipped key is recorded rather than only skipped.
//
// Permissive parsing has to keep going past a key it does not know, or a typo
// costs every setting below it — that is what the cases above pin. The cost is
// that a misspelled key and a key from a newer build look identical from
// inside: both are dropped, both leave the field at its default. Recording the
// drop is what lets a frontend tell the user which line did nothing, without
// Core deciding that an unknown key is an error.
//
// Fails without the recording: the vector stays empty and the first CHECK
// reports a skipped key that nothing can report.
TEST(testAKeyTheParseSkipsIsRecordedWithItsLineAndSection)
{
    const Values configuration = parseString(
        "[cursor]\n"
        "shape = \"block\"\n"
        "shpae = \"bar\"\n" // the typo: a real section, a key no handler claims
    );

    // Guarded rather than indexed straight through: without the recording the
    // vector is empty, and an unguarded [0] turns a legible failure into a
    // signal with no case summary behind it.
    CHECK(configuration.skippedKeys.size() == 1);
    if (configuration.skippedKeys.size() == 1) {
        CHECK(configuration.skippedKeys[0].key == "shpae");
        CHECK(configuration.skippedKeys[0].section == "cursor");
        // Line 3: the header and the good key precede it.
        CHECK(configuration.skippedKeys[0].line == 3);
    }
    // The key beside it still landed — recording a skip must not cost the parse
    // the setting the user spelled correctly.
    CHECK(configuration.cursorShape.has_value());
}

// A file whose keys all land records nothing, so a caller can treat a non-empty
// vector as "there is something to say" without comparing against a baseline.
TEST(testAFileWithNoSkippedKeysRecordsNone)
{
    const Values configuration = parseString(
        "[cursor]\n"
        "shape = \"block\"\n");
    CHECK(configuration.skippedKeys.empty());
}

// The record describes the parse that produced it, not every parse the Values
// has seen. Values values merge across layers on purpose; a skip from another
// file would be a claim about a file the caller did not ask about.
TEST(testASecondParseReplacesTheFirstRecordRatherThanAppending)
{
    Values configuration;
    {
        std::istringstream first("[cursor]\nnope = 1\n");
        parse(first, configuration);
    }
    CHECK(configuration.skippedKeys.size() == 1);
    {
        std::istringstream second("[cursor]\nshape = \"block\"\n");
        parse(second, configuration);
    }
    CHECK(configuration.skippedKeys.empty());
}

// The line number survives a multi-line value. The nested-table skip consumes
// its continuation lines from the same stream, so a counter that only advanced
// in the outer loop would report every later key short by the number of lines
// that value spanned.
TEST(testTheLineNumberCountsContinuationLinesOfAMultiLineValue)
{
    const Values configuration = parseString(
        "[font]\n"
        "range_map = { start = 1,\n"
        "              end = 2 }\n"
        "[cursor]\n"
        "shpae = \"bar\"\n");

    // The inline table spanning two lines is not TOML this reader takes, and
    // each half is named as a malformed line at its own number; the unknown
    // key after it still lands on line 5.
    CHECK(configuration.skippedKeys.size() == 3);
    if (configuration.skippedKeys.size() == 3) {
        CHECK(configuration.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::MalformedLine);
        CHECK(configuration.skippedKeys[0].line == 2);
        CHECK(configuration.skippedKeys[1].reason == Ubiquitty::Preferences::SkipReason::MalformedLine);
        CHECK(configuration.skippedKeys[1].line == 3);
        CHECK(configuration.skippedKeys[2].key == "shpae");
        CHECK(configuration.skippedKeys[2].reason == Ubiquitty::Preferences::SkipReason::UnknownKey);
        CHECK(configuration.skippedKeys[2].line == 5);
    }
}

UBIQUITTY_TEST_MAIN()

// ---- residue ledger: Preferences/Preferences.cpp -------------------------------
// What stays dark after these cases, and why each arm cannot execute:
//
// - parseQuotedString's "no opening quote" return: all seven call sites check
//   the quote themselves before calling, so the function is never entered on a
//   value that does not start with one.
// - The per-section assign() fallthroughs for keys that no section defines:
//   the key tables are exhaustive over the settings the writer emits, and a
//   key outside them is refused one level up, by the section dispatch.
// - The Strict-mode error returns of handlers whose Permissive path this file
//   drives: Strict differs only in whether an already-refused line also stops
//   the parse, which the differential cases in test_preferences_parse_differential
//   cover from the other side.
