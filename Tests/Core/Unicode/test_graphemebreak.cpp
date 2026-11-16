// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// UAX #29 extended grapheme cluster segmenter. Pins the break/no-break decision
// for combining marks, ZWJ emoji sequences, regional-indicator flag pairs,
// skin-tone modifiers, VS-15/16, Hangul jamo runs, and plain text boundaries
// against the property tables generated from UCD 18.0.0.

#include "Harness.h"

#include "Core/Unicode/GraphemeBreak.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

using Ubiquitty::Unicode::graphemeExtends;
using Ubiquitty::Unicode::GraphemeState;
using Ubiquitty::Unicode::graphemeStateReset;

TEST(freshBaseBoundaryProbeMatchesStatefulEngine)
{
    // Every GCB class, both Hangul syllable forms, emoji and text-default
    // pictographs, and controls that override all of the joining classes.
    constexpr std::array bases {
        0x61u, 0x301u, 0x200Du, 0x903u, 0x600u, 0x1F1E6u,
        0x1100u, 0x1161u, 0x11A8u, 0xAC00u, 0xAC01u, 0x915u,
        0x1F600u, 0x2764u, 0xDu, 0xAu, 0u, 0x7Fu, 0x9Fu,
        0x2028u, 0x2029u, 0x110000u, std::numeric_limits<std::uint32_t>::max()
    };
    for (const auto base : bases) {
        const auto doesSuccessorMatch = [base](std::uint32_t next) {
            GraphemeState state;
            graphemeStateReset(&state, base);
            const bool shouldExtend = graphemeExtends(&state, base, next);
            const bool doesExtend = Ubiquitty::Unicode::doesGraphemeExtendFromBase(base, next);
            if (doesExtend != shouldExtend)
                std::printf("fresh base U+%04X successor U+%04X\n", base, next);
            CHECK_EQ(doesExtend, shouldExtend);
            return doesExtend == shouldExtend;
        };
        // Include surrogates as well: the property API accepts uint32_t and
        // agrees with its stateful counterpart even for non-scalar values.
        for (std::uint32_t next = 0; next <= 0x110000; ++next) {
            if (!doesSuccessorMatch(next))
                return;
        }
        for (const auto next : { 0x110001u, 0x80000000u, std::numeric_limits<std::uint32_t>::max() }) {
            if (!doesSuccessorMatch(next))
                return;
        }
    }
}

/// Segment a codePoint sequence into clusters by walking graphemeExtends,
/// exactly as emitCharacter does. Returns the cluster count and, optionally,
/// the size of the first cluster.
static int clusterCount(const std::uint32_t* codePoints, int n, int* firstLength)
{
    if (n == 0) {
        if (firstLength)
            *firstLength = 0;
        return 0;
    }
    GraphemeState st;
    graphemeStateReset(&st, codePoints[0]);
    int clusters = 1;
    int currentLength = 1;
    int firstClusterLength = -1;
    std::uint32_t prev = codePoints[0];
    for (int i = 1; i < n; ++i) {
        if (graphemeExtends(&st, prev, codePoints[i])) {
            currentLength++;
        } else {
            if (firstClusterLength < 0)
                firstClusterLength = currentLength;
            clusters++;
            currentLength = 1;
        }
        prev = codePoints[i];
    }
    if (firstClusterLength < 0)
        firstClusterLength = currentLength;
    if (firstLength)
        *firstLength = firstClusterLength;
    return clusters;
}

TEST(graphemebreak)
{

    // Plain ASCII text — every codePoint is its own cluster.
    {
        const std::uint32_t s[] = { 'a', 'b', 'c' };
        CHECK(clusterCount(s, 3, nullptr) == 3);
    }

    // Base + single combining mark (e + combining acute) → one cluster.
    {
        const std::uint32_t s[] = { 0x0065, 0x0301 };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // Base + multiple combining marks → still one cluster.
    {
        const std::uint32_t s[] = { 0x0061, 0x0300, 0x0301, 0x0302 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 1);
        CHECK(first == 4);
    }

    // GB11 is "ExtPict Extend* ZWJ x ExtPict": the Extend run precedes the
    // joiner, and the joiner sits immediately before the pictograph it joins.
    // The vendored conformance corpus has no case with an Extend between the
    // two, so neither of these sequences is covered by it.
    //
    // A stop sign, a joiner, a combining diaeresis, a stop sign: the diaeresis
    // ends the join, so this is two clusters. Treated as one, the second sign
    // is absorbed into the first cell — it disappears from the grid and the
    // cursor does not move.
    {
        const std::uint32_t s[] = { 0x1F6D1, 0x200D, 0x0308, 0x1F6D1 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 2);
        CHECK(first == 3); // stop sign + joiner + diaeresis
    }

    // Two joiners in a row: the first consumes the pictograph that armed the
    // rule, so the second has nothing to arm from and the last sign stands
    // alone.
    {
        const std::uint32_t s[] = { 0x1F6D1, 0x200D, 0x200D, 0x1F6D1 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 2);
        CHECK(first == 3);
    }

    // The rule itself still joins: the Extend run before the joiner is what
    // GB11 allows, so this is one cluster.
    {
        const std::uint32_t s[] = { 0x1F6D1, 0x0308, 0x200D, 0x1F6D1 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 1);
        CHECK(first == 4);
    }

    // ZWJ family emoji 👨‍👩‍👧 (man ZWJ woman ZWJ girl) → one cluster.
    {
        const std::uint32_t s[] = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 };
        int first = 0;
        CHECK(clusterCount(s, 5, &first) == 1);
        CHECK(first == 5);
    }

    // GB9c — Linker Extend* × Consonant. Devanagari
    // KA VIRAMA TA is one conjunct cluster, and a second virama keeps it
    // open. A SpacingMark is not InCB=Extend and closes the chain.
    {
        const std::uint32_t s[] = { 0x0915, 0x094D, 0x0924 };
        int first = 0;
        CHECK(clusterCount(s, 3, &first) == 1);
        CHECK(first == 3);
    }
    {
        const std::uint32_t s[] = { 0x0915, 0x094D, 0x094D, 0x0924 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 1);
    }
    {
        /// No linker: two separate consonants.
        const std::uint32_t s[] = { 0x0915, 0x0924 };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 2);
    }
    {
        /// ZWNJ is the one joiner that is not InCB=Extend, so it breaks the
        /// chain and the following consonant starts a cluster.
        const std::uint32_t s[] = { 0x0915, 0x094D, 0x200C, 0x0924 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 2);
    }

    // ZWNJ is Grapheme_Cluster_Break=Extend, so it joins the cluster it
    // follows. Persian and Indic text place it between letters to suppress
    // a ligature. It joins the letter before it, so the sequence is two
    // clusters and not three.
    {
        const std::uint32_t s[] = { 0x0645, 0x200C, 0x06CC };
        int first = 0;
        CHECK(clusterCount(s, 3, &first) == 2);
        CHECK(first == 2);
    }

    // ZWJ couple 👨‍❤️‍👨 with a VS-16 inside → one cluster.
    {
        const std::uint32_t s[] = { 0x1F468, 0x200D, 0x2764, 0xFE0F, 0x200D, 0x1F468 };
        int first = 0;
        CHECK(clusterCount(s, 6, &first) == 1);
        CHECK(first == 6);
    }

    // Regional-indicator flag pair 🇺🇸 (U + S) → one cluster.
    {
        const std::uint32_t s[] = { 0x1F1FA, 0x1F1F8 };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // Four regional indicators 🇺🇸🇬🇧 → two flags (two clusters). The
    // GB12/GB13 even-count rule must not glue all four together.
    {
        const std::uint32_t s[] = { 0x1F1FA, 0x1F1F8, 0x1F1EC, 0x1F1E7 };
        int first = 0;
        CHECK(clusterCount(s, 4, &first) == 2);
        CHECK(first == 2);
    }

    // Skin-tone modifier 👍🏽 (thumbs up + medium skin tone) → one cluster.
    {
        const std::uint32_t s[] = { 0x1F44D, 0x1F3FD };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // VS-16 emoji presentation ❤️ (heart + VS16) → one cluster.
    {
        const std::uint32_t s[] = { 0x2764, 0xFE0F };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // VS-15 text presentation (heart + VS15) → one cluster.
    {
        const std::uint32_t s[] = { 0x2764, 0xFE0E };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // Two adjacent emoji with no joiner → two clusters (a boundary
    // falls between them).
    {
        const std::uint32_t s[] = { 0x1F600, 0x1F601 };
        CHECK(clusterCount(s, 2, nullptr) == 2);
    }

    // The zwj_storm benchmark's longest emoji sequences must stay intact.
    {
        const std::uint32_t family[] = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
        int first = 0;
        CHECK(clusterCount(family, 7, &first) == 1);
        CHECK(first == 7);
    }
    {
        const std::uint32_t handshake[] = { 0x1F9D1, 0x1F3FB, 0x200D, 0x1F91D, 0x200D, 0x1F9D1, 0x1F3FD };
        int first = 0;
        CHECK(clusterCount(handshake, 7, &first) == 1);
        CHECK(first == 7);
    }
    {
        const std::uint32_t couple[] = { 0x1F469, 0x1F3FC, 0x200D, 0x2764, 0xFE0F, 0x200D, 0x1F468, 0x1F3FE };
        int first = 0;
        CHECK(clusterCount(couple, 8, &first) == 1);
        CHECK(first == 8);
    }
    {
        const std::uint32_t prideFlag[] = { 0x1F3F3, 0xFE0F, 0x200D, 0x1F308 };
        int first = 0;
        CHECK(clusterCount(prideFlag, 4, &first) == 1);
        CHECK(first == 4);
    }
    {
        const std::uint32_t pirateFlag[] = { 0x1F3F4, 0x200D, 0x2620, 0xFE0F };
        int first = 0;
        CHECK(clusterCount(pirateFlag, 4, &first) == 1);
        CHECK(first == 4);
    }

    // Lone ZWJ between two non-pictographic letters does not merge them
    // into one cluster beyond GB9 attaching the ZWJ to the left: 'a' ZWJ
    // 'b' → the ZWJ joins 'a' (no boundary), then 'b' starts a new
    // cluster (GB9 only carries the ZWJ; GB11 needs pictographs on both
    // sides). Result: two clusters.
    {
        const std::uint32_t s[] = { 'a', 0x200D, 'b' };
        CHECK(clusterCount(s, 3, nullptr) == 2);
    }

    // Hangul jamo run L + V + T → one syllable cluster.
    {
        const std::uint32_t s[] = { 0x1100, 0x1161, 0x11A8 };
        int first = 0;
        CHECK(clusterCount(s, 3, &first) == 1);
        CHECK(first == 3);
    }

    // Precomposed Hangul LV + trailing T jamo → one cluster.
    {
        const std::uint32_t s[] = { 0xAC00, 0x11A8 };
        int first = 0;
        CHECK(clusterCount(s, 2, &first) == 1);
        CHECK(first == 2);
    }

    // isExtendedPictographic gates the VS16/VS15 emoji-presentation
    // override in the emit path: pictographs (incl. text-default ones)
    // true, ordinary letters / digits / CJK false.
    {
        const std::uint32_t pict[] = {
            0x2764 /* ❤ text-default pictograph */,
            0x1F600 /* 😀 */,
            0x263A /* ☺ narrow text-default pictograph */,
            0x1F468 /* 👨 */,
        };
        for (unsigned i = 0; i < sizeof(pict) / sizeof(pict[0]); ++i) {
            if (!Ubiquitty::Unicode::isExtendedPictographic(pict[i])) {
                printf("FAIL U+%04lX: expected Extended_Pictographic\n", (unsigned long)pict[i]);
                failures++;
            }
        }
        const std::uint32_t notPict[] = {
            0x0041 /* A */,
            0x0030 /* 0 */,
            0x4E00 /* CJK */,
            0x200D /* ZWJ */,
            0xFE0F /* VS-16 itself */,
        };
        for (unsigned i = 0; i < sizeof(notPict) / sizeof(notPict[0]); ++i) {
            if (Ubiquitty::Unicode::isExtendedPictographic(notPict[i])) {
                printf("FAIL U+%04lX: expected NOT Extended_Pictographic\n", (unsigned long)notPict[i]);
                failures++;
            }
        }
    }
}

TEST(clusterBaseFollowsPrepends)
{
    using Ubiquitty::Unicode::clusterBaseIndex;
    using Ubiquitty::Unicode::isGraphemePrepend;

    // ARABIC NUMBER SIGN, ARABIC NUMBER MARK ABOVE, SYRIAC ABBREVIATION MARK
    // and MALAYALAM LETTER DOT REPH are Prepend; a letter, a mark and an ideograph
    // are not.
    for (const std::uint32_t prepend : { 0x0600u, 0x0605u, 0x070Fu, 0x0D4Eu })
        CHECK_MSG(isGraphemePrepend(prepend), "a Prepend is classified as one");
    for (const std::uint32_t other : { 0x0061u, 0x0301u, 0x8FF2u, 0x1F1EFu })
        CHECK_MSG(!isGraphemePrepend(other), "a base or mark is not a Prepend");

    const std::uint32_t plain[] = { 0x0061, 0x0301 };
    CHECK(clusterBaseIndex(plain) == 0);
    const std::uint32_t onePrepend[] = { 0x0605, 0x8FF2 };
    CHECK(clusterBaseIndex(onePrepend) == 1);
    const std::uint32_t twoPrepends[] = { 0x0600, 0x0605, 0x0661, 0x0301 };
    CHECK(clusterBaseIndex(twoPrepends) == 2);
    const std::uint32_t prependsAlone[] = { 0x0600, 0x0605 };
    CHECK(clusterBaseIndex(prependsAlone) == 0);
    CHECK(clusterBaseIndex({}) == 0);
}

TEST(linkersArmConjunctsWithoutAPrecedingConsonant)
{
    const std::uint32_t linkers[] = { 0x094D, 0x1CF5, 0x1CF6, 0x11A3A };
    for (const auto linker : linkers) {
        CHECK(Ubiquitty::Unicode::isGraphemeRelevant(linker));
        for (const bool usesHotPath : { false, true }) {
            GraphemeState state {};
            graphemeStateReset(&state, linker);
            const auto extends = [&](std::uint32_t previous, std::uint32_t next) {
                return usesHotPath ? Ubiquitty::Unicode::graphemeExtendsHot(&state, previous, next) : graphemeExtends(&state, previous, next);
            };
            CHECK(extends(linker, 0x0308));
            CHECK(extends(0x0308, 0x0915));
            CHECK(!extends(0x0915, 0x0915));
            graphemeStateReset(&state, linker);
            CHECK(extends(linker, 0x200C));
            CHECK(!extends(0x200C, 0x0915));
        }
    }
    // A Linker can arm the suffix after a non-Indic base too. Joining an
    // emoji through GB11 then consumes it, because that pictograph is not Extend.
    for (const bool usesHotPath : { false, true }) {
        GraphemeState state {};
        graphemeStateReset(&state, 0x1F468);
        const auto extends = [&](std::uint32_t previous, std::uint32_t next) {
            return usesHotPath ? Ubiquitty::Unicode::graphemeExtendsHot(&state, previous, next) : graphemeExtends(&state, previous, next);
        };
        graphemeStateReset(&state, 'A');
        CHECK(extends('A', 0x094D));
        CHECK(extends(0x094D, 0x0915));
        graphemeStateReset(&state, 0x1F468);
        CHECK(extends(0x1F468, 0x094D));
        CHECK(extends(0x094D, 0x200D));
        CHECK(extends(0x200D, 0x1F469));
        CHECK(!extends(0x1F469, 0x0915));
    }
}

UBIQUITTY_TEST_MAIN()
