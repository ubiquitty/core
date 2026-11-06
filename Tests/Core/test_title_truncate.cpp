// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_title_truncate.cpp — pins the semantic title truncation
// (Source/Core/Title/Truncate.h): truncateMiddle and truncatePath.
//
// The point of the helper is that it shortens with INTENT: a path keeps its
// anchor and its deepest (most identifying) components while the middle collapses
// to an ellipsis, and a generic title keeps both ends. These checks pin the exact
// elision points, the UTF-8 scalar counting (no mid-sequence splits), and the
// degenerate-budget behavior, so a regression shows here rather than as a
// meaningless "~/Projects/ubiqui…" on the title bar.

#include "Core/Title/Truncate.h"

#include <cstdio>
#include <string>

using Ubiquitty::Title::titleScalarLength;
using Ubiquitty::Title::truncateMiddle;
using Ubiquitty::Title::truncatePath;

TEST(testGlyphLength)
{
    CHECK(titleScalarLength("hello") == 5);
    CHECK(titleScalarLength("") == 0);
    CHECK(titleScalarLength("…") == 1); // U+2026, 3 bytes, 1 scalar
    CHECK(titleScalarLength("áé") == 2); // two 2-byte scalars
}

TEST(testTruncateMiddle)
{
    CHECK_STR_EQ(truncateMiddle("hello", 10), "hello"); // fits → unchanged
    CHECK_STR_EQ(truncateMiddle("hello", 5), "hello"); // exactly fits
    CHECK_STR_EQ(truncateMiddle("abcdefghij", 5), "ab…ij"); // head 2 + … + tail 2
    CHECK_STR_EQ(truncateMiddle("abcdefghij", 0), ""); // degenerate
    CHECK_STR_EQ(truncateMiddle("abcdefghij", 1), "…"); // degenerate
    // UTF-8: never split a multi-byte scalar; result is 5 scalars.
    const std::string got = truncateMiddle("áéíóúàèìòù", 5);
    CHECK_STR_EQ(got, "áé…òù");
    CHECK(titleScalarLength(got) == 5);
}

TEST(testTruncatePathDropsMiddle)
{
    const char* deep = "~/Projects/ubiquitty/Source/Core/VT100";
    CHECK_STR_EQ(truncatePath(deep, 40), deep); // fits → unchanged
    // Tight budget keeps only the anchor + deepest component.
    CHECK_STR_EQ(truncatePath(deep, 12), "~/…/VT100");
    // More room keeps more of the tail (deepest-first).
    CHECK_STR_EQ(truncatePath(deep, 15), "~/…/Core/VT100");
}

TEST(testTruncatePathAbsoluteRoot)
{
    // A root path has an empty anchor → the gap is "/…/".
    CHECK_STR_EQ(truncatePath("/usr/local/share/app", 8), "/…/app");
}

TEST(testTruncatePathSingleComponentFallsBack)
{
    // Only one component after the anchor: no middle to drop, so middle-truncate
    // the whole path (no misleading "/…/").
    const std::string got = truncatePath("~/reallyreallylongdirectoryname", 12);
    CHECK(titleScalarLength(got) <= 12);
    CHECK(got.rfind("~/", 0) == 0); // still anchored
    CHECK(got.find("/…/") == std::string::npos); // no fake gap
    CHECK(got.find("…") != std::string::npos); // did elide
}

TEST(testTruncatePathLongFinalComponent)
{
    // The last component alone overflows the gap → middle-truncate it in place.
    CHECK_STR_EQ(truncatePath("~/a/b/verylongfinalcomponent", 12), "~/…/ver…nent");
}

// The shape the Linux tab label feeds it: a woven title, thick with multi-byte
// scalars — the em dash between every segment, the arrow inside a process chain.
// Cutting at `substr(0, 23)` counts BYTES, which lands inside a sequence and
// draws a broken glyph in the strip.
TEST(testTruncateAWovenTitleNeverSplitsAScalar)
{
    // Valid UTF-8 by construction: no byte may be a lone continuation, and the
    // length a reader sees is scalars, not bytes.
    const auto isWellFormedUTF8 = [](const std::string& text) {
        std::size_t index = 0;
        while (index < text.size()) {
            const unsigned char lead = static_cast<unsigned char>(text[index]);
            std::size_t width = 0;
            if (lead < 0x80)
                width = 1;
            else if ((lead & 0xE0) == 0xC0)
                width = 2;
            else if ((lead & 0xF0) == 0xE0)
                width = 3;
            else if ((lead & 0xF8) == 0xF0)
                width = 4;
            else
                return false; // a continuation byte where a lead belongs
            if (index + width > text.size())
                return false; // a sequence cut short by the truncation
            for (std::size_t offset = 1; offset < width; ++offset) {
                if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80)
                    return false;
            }
            index += width;
        }
        return true;
    };

    // Every cut position across the range a tab label uses, so the assertion
    // does not depend on one lucky boundary.
    const std::string woven = "~/Projects/ubiquitty — mc ▸ vi — 120×30";
    for (std::size_t width = 1; width <= 40; ++width) {
        const std::string got = truncateMiddle(woven, width);
        char message[256];
        std::snprintf(message, sizeof(message), "width %zu produced ill-formed UTF-8: \"%s\"", width, got.c_str());
        CHECK_MSG(isWellFormedUTF8(got), message);
        std::snprintf(message, sizeof(message), "width %zu ran to %zu scalars", width, titleScalarLength(got));
        CHECK_MSG(titleScalarLength(got) <= width, message);
    }

    // A byte cut at the same offset splits a UTF-8 scalar.
    CHECK_MSG(!isWellFormedUTF8(woven.substr(0, 23)), "the byte cut this replaces really did split a scalar");
}

UBIQUITTY_TEST_MAIN()
