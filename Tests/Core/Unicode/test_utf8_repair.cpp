// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_utf8_repair — text that has to be UTF-8 is made so, and text that
// already is stays byte for byte what it was (Core/Unicode/UTF8Repair.h).

#include "Harness.h"

#include "Core/Protocol/Sync/Wire.h"
#include "Core/Unicode/UTF8Repair.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using Ubiquitty::Unicode::repairUTF8;

namespace {

constexpr std::string_view replacement = "\xEF\xBF\xBD";

/// A text after repair.
std::string repaired(std::string text)
{
    (void)repairUTF8(text);
    return text;
}

/// Whether a text is well formed, by the wire's own validator.
bool isWellFormed(std::string_view text)
{
    return Ubiquitty::Protocol::Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

/// `count` replacement characters.
std::string replacements(int count)
{
    std::string text;
    for (int index = 0; index < count; ++index)
        text += replacement;
    return text;
}

} // namespace

TEST(wellFormedTextIsLeftAsItIs)
{
    for (const std::string_view text : { std::string_view {}, std::string_view { "make all" }, std::string_view { "caf\xC3\xA9" }, std::string_view { "\xE2\x82\xAC" }, std::string_view { "\xF0\x9F\x98\x80 done" },
             std::string_view { "\xEF\xBF\xBD" }, std::string_view { "\xED\x9F\xBF\xEE\x80\x80" }, std::string_view { "\xF4\x8F\xBF\xBF" }, std::string_view { "\xC2\x80\xDF\xBF\xE0\xA0\x80\xF0\x90\x80\x80" } }) {
        std::string copy(text);
        CHECK_MSG(!repairUTF8(copy), std::string(text).c_str());
        CHECK(copy == text);
    }
    // A NUL is a character like any other.
    std::string withNUL("a\0b", 3);
    CHECK(!repairUTF8(withNUL));
    CHECK_EQ(withNUL.size(), 3u);
}

TEST(aByteThatBeginsNoSequenceBecomesOneReplacement)
{
    std::string title = "make \xFF"
                        "all";
    CHECK(repairUTF8(title));
    CHECK_STR_EQ(title, "make \xEF\xBF\xBD"
                        "all");
    // A lone continuation, the two leads of an overlong pair, and the leads
    // past U+10FFFF: one each.
    CHECK_STR_EQ(repaired("\x80"), replacements(1));
    CHECK_STR_EQ(repaired("\xC0\xAF"), replacements(2));
    CHECK_STR_EQ(repaired("\xC1\xBF"), replacements(2));
    CHECK_STR_EQ(repaired("\xF5\x80\x80\x80"), replacements(4));
    CHECK_STR_EQ(repaired("\xFE\xFF"), replacements(2));
}

TEST(aSequenceCutShortIsOneReplacementForWhatWasThere)
{
    // The maximal subpart of each truncated sequence is replaced once, and
    // the byte that ended it is read on its own.
    CHECK_STR_EQ(repaired("\xE2\x82"), replacements(1));
    CHECK_STR_EQ(repaired("\xE2\x82"
                          "A"),
        replacements(1) + "A");
    CHECK_STR_EQ(repaired("\xF0\x9F\x98"), replacements(1));
    CHECK_STR_EQ(repaired("\xF0\x9F"
                          "x"),
        replacements(1) + "x");
    CHECK_STR_EQ(repaired("\xC3"), replacements(1));
    CHECK_STR_EQ(repaired("\xC3\xC3\xA9"), replacements(1) + "\xC3\xA9");
    // The standard's own example: 61 F1 80 80 E1 80 C2 62 80 63 80 BF 64.
    CHECK_STR_EQ(repaired("a\xF1\x80\x80\xE1\x80\xC2"
                          "b\x80"
                          "c\x80\xBF"
                          "d"),
        "a" + replacements(3) + "b" + replacements(1) + "c" + replacements(2) + "d");
}

TEST(whatNoScalarEncodesToIsRefusedByteByByte)
{
    // Overlong forms, surrogates and values past the last scalar: the lead
    // is ill formed with that second byte, so each byte is its own fault.
    CHECK_STR_EQ(repaired("\xE0\x80\x80"), replacements(3));
    CHECK_STR_EQ(repaired("\xE0\x9F\xBF"), replacements(3));
    CHECK_STR_EQ(repaired("\xED\xA0\x80"), replacements(3));
    CHECK_STR_EQ(repaired("\xED\xBF\xBF"), replacements(3));
    CHECK_STR_EQ(repaired("\xF0\x80\x80\x80"), replacements(4));
    CHECK_STR_EQ(repaired("\xF0\x8F\xBF\xBF"), replacements(4));
    CHECK_STR_EQ(repaired("\xF4\x90\x80\x80"), replacements(4));
}

TEST(whateverGoesInWhatComesOutIsWellFormed)
{
    // Every pair of bytes, and every pair behind a lead that wants three.
    for (int first = 0; first < 256; ++first) {
        for (int second = 0; second < 256; ++second) {
            const std::string pair { static_cast<char>(first), static_cast<char>(second) };
            for (const std::string& text : { pair, "\xE2" + pair, "\xF0\x9F" + pair, pair + "\xBF\xBF" }) {
                std::string copy = text;
                const bool didRepair = repairUTF8(copy); // bool-ok: the function's own answer
                if (!isWellFormed(copy)) {
                    CHECK_MSG(false, "a repaired text is not well formed");
                    return;
                }
                if (didRepair == isWellFormed(text) || (!didRepair && copy != text)) {
                    CHECK_MSG(false, "a text was repaired exactly when it was already well formed, or changed without saying so");
                    return;
                }
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
