// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A sequence-gate list that does not fit has to say so.
//
// The per-OSC/DCS gate is a security control, and its two directions fail in
// opposite ways when a list is shortened. A truncated ALLOW list fails closed:
// fewer sequences pass than were asked for. A truncated DENY list fails OPEN --
// the codes past the cap are permitted, the control reports success, and it
// does less than it was told to.
//
// A parser that stopped at `cap` and discarded the remainder in silence would
// give an operator who denied more codes than the capacity holds a gate that
// quietly honoured the overflow.

#include "Assertions.h"
#include "Harness.h"

#include "Core/OSC/Dispatch.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace Ubiquitty::OSC;

TEST(anOverlongOSCListReportsWhatItDropped)
{
    // A list one longer than the array it is parsed into: the parser fills the
    // array, and the last code has nowhere to go.
    std::string spec;
    for (int i = 0; i < 9; ++i)
        spec += std::to_string(100 + i) + ",";
    spec.pop_back();

    int codes[8];
    int dropped = -1;
    const int n = parseOSCList(spec, codes, 8, &dropped);

    // Still fills what it can -- truncation is not refusal, and the entries
    // that fit are honoured.
    CHECK(n == 8);
    CHECK(codes[0] == 100);
    CHECK(codes[7] == 107);
    // And names the one it could not: the fold closes the deny dimension on
    // any nonzero count, so an unreported drop would run truncated open.
    CHECK(dropped == 1);
}

TEST(anOSCListThatFitsExactlyIsNotReportedAsTruncated)
{
    // The boundary is the case worth pinning: n reaches cap and the input is
    // exhausted at the same moment. Complaining here would cry wolf on every
    // list that happens to fill the array, which is the shape most likely to
    // train an operator to ignore the warning.
    int codes[8];
    CHECK(parseOSCList("1,2,3,4,5,6,7,8", codes, 8) == 8);

    // And with trailing separators, which are not dropped rules.
    CHECK(parseOSCList("1,2,3,4,5,6,7,8,  ", codes, 8) == 8);
}

TEST(anOverlongDCSListStillFillsWhatItCan)
{
    // Seven known DCS kinds, so a capacity of two truncates hard.
    DCSKind kinds[2];
    const int n = parseDCSList("sixel,decudk,decrqss,tmux", kinds, 2);
    CHECK(n == 2);
    CHECK(kinds[0] == DCSKind::Sixel);
    CHECK(kinds[1] == DCSKind::DECUDK);
}

TEST(anUnknownTailIsNotADroppedRule)
{
    // Unknown names are skipped by design, so a list that fills the array and
    // then carries only junk has lost no RULE. Reporting it as truncated would
    // be a lie in the direction of alarm.
    DCSKind kinds[2];
    CHECK(parseDCSList("sixel,decudk,notakind", kinds, 2) == 2);
}

TEST(anAPCListNamesIntroducersByTheirByte)
{
    // An APC is keyed by its first byte, which is case-sensitive on the wire:
    // `G` is kitty graphics and `g` is nothing. A token longer than one byte,
    // or one outside printable ASCII, names no introducer and is skipped.
    std::uint8_t introducers[4];
    const int n = parseAPCList("G, g kitty \x01 ~", introducers);
    CHECK(n == 3);
    CHECK(introducers[0] == 'G');
    CHECK(introducers[1] == 'g');
    CHECK(introducers[2] == '~');
}

TEST(anOverlongAPCListReportsWhatItDropped)
{
    std::uint8_t introducers[2];
    int dropped = -1;
    CHECK(parseAPCList("A B C", introducers, &dropped) == 2);
    CHECK(dropped == 1);
    // The skipped tokens are not dropped rules.
    CHECK(parseAPCList("A B kitty", introducers, &dropped) == 2);
    CHECK(dropped == 0);
}

UBIQUITTY_TEST_MAIN()
