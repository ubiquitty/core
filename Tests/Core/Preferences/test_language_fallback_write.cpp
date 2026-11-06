// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_language_fallback_write.cpp — writing `[[font.language_fallbacks]]` back
// to config.toml.
//
// Every case that claims the writer produced something usable proves it by
// parsing the result with the real reader rather than by comparing strings: the
// reader drops an entry failing isValidLanguageFallback without reporting it,
// so output that reads correctly and parses to nothing satisfies a string
// comparison and still routes no glyph.

#include "Core/Preferences/LanguageFallbackWrite.h"
#include "Core/Preferences/Preferences.h"

#include <sstream>
#include <string>

using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::removeLanguageFallback;
using Ubiquitty::Preferences::renderLanguageFallbackBlock;
using Ubiquitty::Preferences::spliceLanguageFallback;
using Ubiquitty::Preferences::Values;

namespace {

Values parsed(const std::string& body)
{
    Values values;
    std::istringstream input(body);
    parse(input, values);
    return values;
}

// The routing the file actually expresses: which family answers this script,
// empty for none.
std::string familyFor(const Values& values, const std::string& script)
{
    for (const auto& entry : values.languageFallbacks) {
        if (entry.script == script)
            return entry.family;
    }
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

} // namespace

TEST(testAWrittenScriptFallbackParsesBackAsRouting)
{
    const std::string body = spliceLanguageFallback("[font]\nfamily = \"JetBrains Mono\"\n", "arabic", "Geeza Pro");
    const Values values = parsed(body);

    CHECK(values.fontFamily.value_or("") == "JetBrains Mono"); // the unrelated key survives
    CHECK(values.languageFallbacks.size() == 1);
    CHECK(familyFor(values, "arabic") == "Geeza Pro");
}

TEST(testAWrittenEntryCarriesNoLanguage)
{
    // The route matches on script alone, so a language would be a value the
    // file carries and nothing reads. The entry must still be one the reader
    // keeps, which is what the parse below is asserting.
    const std::string block = renderLanguageFallbackBlock("arabic", "Geeza Pro");
    CHECK(!contains(block, "language ="));
    CHECK(!contains(block, "language="));

    const Values values = parsed(block);
    CHECK(values.languageFallbacks.size() == 1);
    if (values.languageFallbacks.size() == 1)
        CHECK(values.languageFallbacks[0].language.empty());
}

TEST(testAFamilyNeedingEscapesSurvivesTheRoundTrip)
{
    // The family is not the writer's: it comes from a font catalogue, and a
    // quote in it would close the string and make the file mean something else.
    const std::string hostile = "Ev\"il\nFont";
    const Values values = parsed(spliceLanguageFallback("", "greek", hostile));
    CHECK(values.languageFallbacks.size() == 1);
    CHECK(familyFor(values, "greek") == hostile);
}

TEST(testTheLastWriteWinsForOneScript)
{
    std::string body = spliceLanguageFallback("", "arabic", "Geeza Pro");
    body = spliceLanguageFallback(std::move(body), "arabic", "Al Bayan");
    const Values values = parsed(body);

    // One entry, not two: the route answers from one of them, so a file with
    // both says something it cannot mean.
    CHECK(values.languageFallbacks.size() == 1);
    CHECK(familyFor(values, "arabic") == "Al Bayan");
}

TEST(testScriptsAreMatchedTheWayTheReaderMatchesThem)
{
    // The reader compares scripts case-insensitively, so a differently-cased
    // write has to replace rather than add beside.
    std::string body = spliceLanguageFallback("", "Arabic", "Geeza Pro");
    body = spliceLanguageFallback(std::move(body), "arabic", "Al Bayan");
    const Values values = parsed(body);
    CHECK(values.languageFallbacks.size() == 1);
    CHECK(familyFor(values, "arabic") == "Al Bayan");
}

TEST(testAnotherScriptIsLeftAlone)
{
    std::string body = spliceLanguageFallback("", "arabic", "Geeza Pro");
    body = spliceLanguageFallback(std::move(body), "thai", "Thonburi");
    body = spliceLanguageFallback(std::move(body), "arabic", "Al Bayan");
    const Values values = parsed(body);

    CHECK(values.languageFallbacks.size() == 2);
    CHECK(familyFor(values, "arabic") == "Al Bayan");
    CHECK(familyFor(values, "thai") == "Thonburi"); // untouched by the rewrite above
}

TEST(testRemovalTakesOnlyTheNamedScript)
{
    std::string body = spliceLanguageFallback("", "arabic", "Geeza Pro");
    body = spliceLanguageFallback(std::move(body), "thai", "Thonburi");
    body = removeLanguageFallback(std::move(body), "arabic");
    const Values values = parsed(body);

    CHECK(familyFor(values, "arabic").empty());
    CHECK(familyFor(values, "thai") == "Thonburi");

    // Removing the rest leaves no entry at all rather than an empty one.
    const Values emptied = parsed(removeLanguageFallback(body, "thai"));
    CHECK(emptied.languageFallbacks.empty());
}

TEST(testBlocksForOtherScriptsAreByteIdentical)
{
    // A hand-authored block with a comment and spelling the writer would not
    // produce: it must come back exactly as written.
    const std::string handAuthored = "\n[[font.language_fallbacks]]\n"
                                     "# kept by hand\n"
                                     "script = \"thai\"\n"
                                     "language = \"th\"\n"
                                     "mode = \"custom\"\n"
                                     "family = \"Thonburi\"\n";
    const std::string body = "[font]\nfamily = \"JetBrains Mono\"\n" + handAuthored;
    const std::string after = spliceLanguageFallback(body, "arabic", "Geeza Pro");

    CHECK(contains(after, handAuthored));
    CHECK(contains(after, "# kept by hand"));
    CHECK(contains(after, "language = \"th\"")); // a field this writer never emits

    const Values values = parsed(after);
    CHECK(familyFor(values, "thai") == "Thonburi");
    CHECK(familyFor(values, "arabic") == "Geeza Pro");
}

TEST(testABlockNamingNoScriptIsLeftAlone)
{
    // An entry this writer cannot identify is not its to rewrite: it cannot
    // know which script the block claims, so it must not decide the block does
    // not match and quietly cut it.
    const std::string opaque = "\n[[font.language_fallbacks]]\nmode = \"auto\"\n";
    const std::string after = spliceLanguageFallback(opaque, "arabic", "Geeza Pro");
    CHECK(contains(after, "mode = \"auto\""));
    CHECK(contains(after, "arabic"));
}

TEST(testAnEmptyScriptOrFamilyIsRefused)
{
    // Both would produce an entry the reader drops, so writing one would be a
    // silent no-op in the file rather than a refusal at the call.
    const std::string body = "[font]\nfamily = \"JetBrains Mono\"\n";
    CHECK(spliceLanguageFallback(body, "arabic", "") == body);
    CHECK(spliceLanguageFallback(body, "", "Geeza Pro") == body);
    CHECK(removeLanguageFallback(body, "") == body);
}

UBIQUITTY_TEST_MAIN()
