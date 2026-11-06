// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_bounded_utf8 — the length cap nine of Terminal's string fields share.
//
// `boundedUTF8String` is the one place `font`, `remoteHost`, `commandLine`,
// `currentDirectory`, `currentFileURL`, `answerback`, `title` and both user
// variable halves are cut to size. Every caller reaches it from an OSC payload —
// OSC 1337's user-variable setter, OSC 7, OSC 2 — so the length is chosen by
// whatever is on the other side of the pseudoterminal, and the bound is the only
// thing between that choice and the stored field.
//
// Nothing named it. That is the gap a bound has by construction rather than by
// oversight: the twenty-odd cases driving these OSC sequences end to end all use
// payloads the feature accepts, because a test that exercises a feature supplies
// inputs on the passing side of its limits. Reaching a function says nothing
// about crossing its threshold.
//
// The values are measured, not read off the prose.

#include "Core/Terminal.h"

#include <string>
#include <string_view>

using Ubiquitty::boundedUTF8String;

namespace {

/// The bytes of a bounded copy, so a case names the encoding rather than a size.
std::string bounded(std::string_view source, std::size_t maxBytes)
{
    return boundedUTF8String(source, maxBytes);
}

} // namespace

TEST(boundedUTF8StringKeepsAnythingWithinTheBound)
{
    CHECK_STR_EQ(bounded("abc", 8), "abc");
    CHECK_STR_EQ(bounded("abcd", 4), "abcd"); // exactly at the bound, not over it
    CHECK_STR_EQ(bounded("", 0), "");
}

TEST(boundedUTF8StringCutsToTheBoundOnASCII)
{
    CHECK_STR_EQ(bounded("abcdef", 4), "abcd");
    CHECK_STR_EQ(bounded("abc", 0), "");
}

TEST(boundedUTF8StringBacksOffToAWholeCharacter)
{
    // The cut lands inside U+65E5, so the character goes rather than half of it.
    // Two of the three byte positions inside it are checked, because backing off
    // one byte and backing off two are different walks of the same loop.
    CHECK_STR_EQ(bounded("aa\xE6\x97\xA5", 3), "aa");
    CHECK_STR_EQ(bounded("aa\xE6\x97\xA5", 4), "aa");
    CHECK_STR_EQ(bounded("aa\xE6\x97\xA5", 5), "aa\xE6\x97\xA5"); // the whole of it fits

    // Four bytes, so the walk is longer than any three-byte case can make it.
    CHECK_STR_EQ(bounded("a\xF0\x9F\x98\x80z", 3), "a");
    CHECK_STR_EQ(bounded("a\xF0\x9F\x98\x80z", 5), "a\xF0\x9F\x98\x80");
}

TEST(boundedUTF8StringYieldsNothingRatherThanHalfOfTheFirstCharacter)
{
    // A bound smaller than the leading character has nothing whole to keep, and
    // the walk runs to zero. A field cleared is what a caller can handle; a field
    // holding two thirds of a character is not.
    CHECK_STR_EQ(bounded("\xE6\x97\xA5\x61", 1), "");
    CHECK_STR_EQ(bounded("\xE6\x97\xA5\x61", 2), "");
    CHECK_STR_EQ(bounded("\xE6\x97\xA5\x61", 3), "\xE6\x97\xA5");
}

TEST(boundedUTF8StringYieldsNothingForARunOfContinuationBytes)
{
    // Not reachable from a well-formed sender and reachable from any other: the
    // walk sees a continuation byte at every position and stops at zero.
    CHECK_STR_EQ(bounded("\x80\x80\x80\x80", 2), "");
    CHECK_STR_EQ(bounded("\x80\x80\x80\x80", 4), "\x80\x80\x80\x80"); // within the bound, untouched
}

TEST(boundedUTF8StringDropsALeadWhoseContinuationsTheBoundCutOff)
{
    // The byte at the bound is `c`, not a continuation, so the backoff over
    // continuations does nothing — and the copy would still end on 0xE6, a lead
    // that announces two more bytes the result does not contain. The cut has to
    // consider the retained tail and not only the byte it lands on.
    //
    // These bytes arrive inside an OSC payload and the sender chooses them, so
    // the malformed case is the reachable one rather than the exotic one.
    CHECK_STR_EQ(bounded("ab\xE6"
                         "cd",
                     3),
        "ab");

    // The same shape one byte further in: 0xE6 with ONE of its two continuations
    // inside the bound is still incomplete, and the whole sequence goes.
    CHECK_STR_EQ(bounded("ab\xE6\xA0"
                         "d",
                     4),
        "ab");

    // And with both continuations inside the bound it is a whole character and
    // stays. Without this the case above is satisfied by dropping every
    // multi-byte sequence at the cut, which would be a different bug.
    CHECK_STR_EQ(bounded("ab\xE6\xA0\xBC"
                         "d",
                     5),
        "ab\xE6\xA0\xBC");
}

TEST(boundedUTF8StringDropsAFourByteSequenceTheBoundCutShort)
{
    // Four bytes is the longest sequence, so this is the deepest the walk back
    // to a lead has to reach: three retained continuations before it.
    CHECK_STR_EQ(bounded("a\xF0\x9F\x98\x80"
                         "b",
                     4),
        "a");
    CHECK_STR_EQ(bounded("a\xF0\x9F\x98\x80"
                         "b",
                     5),
        "a\xF0\x9F\x98\x80");
}

TEST(boundedUTF8StringNeverReturnsMoreThanTheBound)
{
    // The bound holds for every input, not only the ones a case names one at a
    // time, so a rewrite that reorders the walk still has to satisfy it.
    //
    // It is also the property the source states twice. `source.size() <=
    // maxBytes` returns early, and the walk's `length < source.size()` refuses
    // the same condition again — either one alone is enough, and removing either
    // one alone passes every case here. Removing the pair passes too, and that
    // is the limit of what this file can see: the walk then indexes one past the
    // view and reads a byte that happens not to be a continuation. Under
    // AddressSanitizer the same binary reports a global-buffer-overflow. So the
    // pair is redundant and the redundancy is not free to collapse, which is a
    // distinction no run without a sanitizer can make.
    static const char* const sources[] = {
        "abc",
        "abcdef",
        "aa\xE6\x97\xA5",
        "a\xF0\x9F\x98\x80z",
        "\xE6\x97\xA5\x61",
        "\x80\x80\x80\x80",
        ("ab\xE6"
         "cd"),
    };
    for (const char* source : sources) {
        for (std::size_t maxBytes = 0; maxBytes <= 8; ++maxBytes)
            CHECK(bounded(source, maxBytes).size() <= maxBytes);
    }
}

UBIQUITTY_TEST_MAIN()
