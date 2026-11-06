// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_optionset.cpp — unit tests for Bits::OptionSet<E> and the ExtendedAttributes
// packed wrapper built on it. OptionSet is a shared foundational primitive (sync
// capability flags, kitty keyboard, keymap modifiers, and Pen attributes), so
// its contract is pinned directly here rather than only through its users.

#include "Base/Bits/OptionSet.h"
#include "Core/VT/Constants.h"

#include <cstdint>
#include <cstdio>

using Ubiquitty::Bits::OptionSet;

// A standalone test enum: only bits 0x01..0x08 are known enumerators, leaving the
// high bits available to exercise the unknown-bit-preservation contract.
enum class Flag : uint8_t {
    A = 0x01,
    B = 0x02,
    C = 0x04,
    D = 0x08
};

TEST(testMembershipAndMutation)
{
    OptionSet<Flag> s;
    CHECK(s.isEmpty());
    CHECK(!s);
    s.add(Flag::A);
    CHECK(s.contains(Flag::A));
    CHECK(!s.contains(Flag::B));
    CHECK(static_cast<bool>(s));
    s.add({ Flag::B, Flag::C });
    CHECK(s.size() == 3);
    CHECK(s.containsAll({ Flag::A, Flag::B }));
    CHECK(!s.containsAll({ Flag::A, Flag::D }));
    CHECK(s.containsAny({ Flag::B, Flag::D }));
    CHECK(!s.containsAny(Flag::D));
    s.remove(Flag::B);
    CHECK(!s.contains(Flag::B));
    s.set(Flag::D, true);
    CHECK(s.contains(Flag::D));
    s.set(Flag::D, false);
    CHECK(!s.contains(Flag::D));
    s.toggle(Flag::A);
    CHECK(!s.contains(Flag::A));
    s.toggle(Flag::A);
    CHECK(s.contains(Flag::A));
}

// The forward-compat property nothing else exercises: fromRaw keeps bits that map
// to no known enumerator, and add/remove/toRaw leave them untouched.
TEST(testRawPreservesUnknownBits)
{
    OptionSet<Flag> s = OptionSet<Flag>::fromRaw(0xF1); // 0x01 known + 0xF0 unknown
    CHECK(s.contains(Flag::A));
    CHECK(s.toRaw() == 0xF1);
    s.add(Flag::B);
    CHECK(s.toRaw() == 0xF3); // add sets only its bit
    s.remove(Flag::A);
    CHECK(s.toRaw() == 0xF2); // remove clears only its bit; the 0xF0 bits survive
}

TEST(testOperatorsAndIterator)
{
    const OptionSet<Flag> a = { Flag::A, Flag::C };
    const OptionSet<Flag> b = { Flag::C, Flag::D };
    CHECK((a | b).toRaw() == 0x0D);
    CHECK((a & b).toRaw() == 0x04);
    CHECK((a - b).toRaw() == 0x01);
    CHECK((a ^ b).toRaw() == 0x09); // symmetric difference (the DECRARA toggle op)
    CHECK(a == OptionSet<Flag>({ Flag::A, Flag::C }));
    CHECK(a != b);

    OptionSet<Flag> s = { Flag::A, Flag::B, Flag::D };
    int count = 0;
    uint8_t seen = 0;
    for (Flag f : s) {
        seen |= static_cast<uint8_t>(f);
        ++count;
    }
    CHECK(count == 3);
    CHECK(seen == 0x0B);
}

// ExtendedAttributes packs single-bit flags (low bits) and a 3-bit weight (high
// bits) into one byte: setWeight/weight round-trip through the flag bits without
// disturbing them, flag writes leave the weight alone, and toRaw/fromRaw
// preserve both halves.
TEST(testExtendedAttributesWeightPacking)
{
    Ubiquitty::VT::ExtendedAttributes e;
    CHECK(e.weight() == 0);
    CHECK(e.flags.isEmpty());

    e.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    e.setWeight(5);
    CHECK(e.weight() == 5);
    CHECK(e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast)); // weight write kept the flag

    e.setWeight(7);
    CHECK(e.weight() == 7);
    CHECK(e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));

    e.setWeight(0);
    CHECK(e.weight() == 0);
    CHECK(e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast)); // clearing weight kept the flags

    // The other direction: the flag bits are written without touching the weight.
    e.setWeight(3);
    e.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    CHECK(e.weight() == 3); // flag clear didn't disturb the weight
    CHECK(!e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    e.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    CHECK(e.weight() == 3); // flag add didn't disturb the weight

    const Ubiquitty::VT::ExtendedAttributes r = Ubiquitty::VT::ExtendedAttributes::fromRaw(e.toRaw());
    CHECK(r == e); // full round-trip through the raw byte
}

namespace {

/// Returns `set` unchanged, by a route the compiler cannot fold across.
///
/// The operators are `constexpr`, so a case built from literal flags is settled
/// at compile time. The assertions still hold and a wrong operator still fails
/// them — but no instantiation runs, and a coverage report correctly says the
/// line was never executed. Routing an operand through a `volatile` forces the
/// runtime path, so what the report shows and what the case proves are the same
/// thing.
///
/// - Parameter set: The value to launder.
/// - Returns: An equal value the compiler must recompute at run time.
template<typename Set>
Set atRunTime(Set set)
{
    volatile typename Set::StorageType raw = set.toRaw();
    return Set::fromRaw(static_cast<typename Set::StorageType>(raw));
}

} // namespace

TEST(theSetOperatorsAgreeWithTheMembershipTheyDescribe)
{
    // `|` is exercised by the flag-building code every caller writes; the other
    // three are not, and each is one character away from another. `&` written as
    // `|` compiles, and so does `-` written as `^` — they differ only where the
    // right-hand side holds a flag the left does not.
    using Set = Ubiquitty::VT::AttributeSet;
    using Flag = Ubiquitty::VT::Attribute;

    const Set bold = atRunTime(Set {} | Flag::Bold);
    const Set italic = atRunTime(Set {} | Flag::Italic);
    const Set both = atRunTime(bold | italic);

    // Intersection keeps what is in both and nothing else.
    CHECK((both & bold) == bold);
    CHECK((bold & italic) == Set {});
    CHECK((both & both) == both);

    // Difference removes the right-hand side, and removing a flag the left does
    // not carry changes nothing — which is where `-` and `^` part company.
    CHECK((both - italic) == bold);
    CHECK((bold - italic) == bold);
    CHECK((both - both) == Set {});

    // Symmetric difference flips: a flag only one side carries survives, one
    // both carry does not. The second assertion is the one `-` would fail.
    CHECK((both ^ italic) == bold);
    CHECK((bold ^ italic) == both);
    CHECK((both ^ both) == Set {});

    // `|=` delegates to add() but is its own entry point a caller reaches as
    // `s |= flag`; add()'s coverage does not run the operator. It mutates in
    // place and returns *this, so both are checked.
    Set acc = atRunTime(Set {});
    CHECK((acc |= bold) == bold); // the returned reference carries the mutation
    CHECK(acc == bold); // and the mutation landed in place
    acc |= italic;
    CHECK(acc == both);
}

TEST(theSetOperatorsHoldForASecondInstantiationToo)
{
    // A template's coverage is per instantiation, so exercising the operators
    // through `AttributeSet` leaves the ones compiled for any other element type
    // unexecuted — the report names them separately and it is right to. This
    // file already defines a second element type, so the second set of
    // instantiations is one case away rather than a gap nobody can close.
    using Set = Ubiquitty::Bits::OptionSet<Flag>;

    const Set a = atRunTime(Set {} | Flag::A);
    const Set b = atRunTime(Set {} | Flag::B);
    const Set ab = atRunTime(a | b);

    CHECK((ab & a) == a);
    CHECK((a & b) == Set {});
    CHECK((ab - b) == a);
    CHECK((a - b) == a);
    CHECK((ab ^ b) == a);
    CHECK((a ^ b) == ab);

    // `|=` for this element type's instantiation, the same operator the case
    // above exercises for AttributeSet — a template's coverage is per instantiation.
    Set acc = atRunTime(Set {});
    CHECK((acc |= a) == a);
    acc |= b;
    CHECK(acc == ab);
}

UBIQUITTY_TEST_MAIN()
