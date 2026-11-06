// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_types.cpp — pins the portable integer-alias contract in
// Source/Base/Types.h.
//
// Compiles against bare libc++ — no platform headers. That on its own
// is the first half of the test: the
// compilation succeeding proves Base/Types.h has no platform
// transitive dependency. The body's static_asserts pin the sizes the
// Core code relies on; runtime exits 0 on success.
//
// NB: this test does not include any Apple header. The type ABI
// pin (`sizeof(::uint8_t) == sizeof(uint8_t)`) lives in the
// frontend adapter that bridges Apple types to ours, not here.

#include "Base/Types.h"

#include <cstdio>

TEST(typesCases)
{

    // Repeat the size pins at TU scope as a belt-and-braces check
    // separate from the header's own asserts: catches the case where
    // a future edit accidentally moves an alias outside the namespace
    // and the header's assert still passes against the alias in the wrong namespace.
    static_assert(sizeof(uint8_t) == 1, "");
    static_assert(sizeof(uint16_t) == 2, "");
    static_assert(sizeof(uint32_t) == 4, "");
    static_assert(sizeof(uint64_t) == 8, "");
    static_assert(sizeof(int8_t) == 1, "");
    static_assert(sizeof(int16_t) == 2, "");
    static_assert(sizeof(int32_t) == 4, "");
    static_assert(sizeof(int64_t) == 8, "");
    static_assert(sizeof(bool) == 1, "");

    // Signedness pins. The Apple Universal-Interfaces aliases are
    // unsigned for the `UInt*` family and two's-complement signed
    // for the `SInt*` family; the Core call sites assume this.
    static_assert(static_cast<uint8_t>(-1) == 0xFF, "");
    static_assert(static_cast<uint16_t>(-1) == 0xFFFF, "");
    static_assert(static_cast<uint32_t>(-1) == 0xFFFFFFFFu, "");
}

namespace {

/// Returns `value` unchanged, by a route the compiler cannot fold across.
///
/// Every `Ticks` factory is `constexpr`, so a case built from literals is
/// settled at compile time: the assertions hold, a wrong conversion still fails
/// them, and no instantiation runs. Laundering the argument forces the runtime
/// path so a coverage report and the case agree about what was exercised.
///
/// - Parameter value: The number to launder.
/// - Returns: An equal number the compiler must read at run time.
uint64_t atRunTime(uint64_t value)
{
    volatile uint64_t opaque = value;
    return opaque;
}

} // namespace

TEST(ticksConvertFromEveryUnitTheClocksHandBack)
{
    using Ubiquitty::Ticks;

    // Sixty ticks to the second, so each factory is a different way of naming
    // the same duration and all three have to land on it.
    CHECK(Ticks::fromMilliseconds(atRunTime(1000)).count == 60);
    CHECK(Ticks::fromNanoseconds(atRunTime(1000000000ull)).count == 60);
    CHECK(Ticks::fromTimespec(atRunTime(1), atRunTime(0)).count == 60);

    // Sub-tick durations truncate toward zero rather than rounding up, so a
    // poll shorter than a frame never reports one elapsed.
    CHECK(Ticks::fromMilliseconds(atRunTime(16)).count == 0);
    CHECK(Ticks::fromNanoseconds(atRunTime(16000000ull)).count == 0);

    // `fromTimespec` rounds to nearest instead, which is the one asymmetry
    // between them: half a tick up lands on the next tick, and just under it
    // does not.
    constexpr uint64_t nanosecondsPerTick = 1000000000ull / 60ull;
    CHECK(Ticks::fromTimespec(atRunTime(0), atRunTime(nanosecondsPerTick / 2 + 1)).count == 1);
    CHECK(Ticks::fromTimespec(atRunTime(0), atRunTime(nanosecondsPerTick / 2 - 1)).count == 0);

    // Seconds and nanoseconds add rather than one replacing the other.
    CHECK(Ticks::fromTimespec(atRunTime(2), atRunTime(nanosecondsPerTick)).count == 121);
}

TEST(ticksAdvanceAndSubtractAcrossTheCounterWrap)
{
    using Ubiquitty::Ticks;

    const Ticks base = Ticks::fromMilliseconds(atRunTime(1000));
    CHECK((base + 5).count == 65);
    CHECK((base + 0).count == base.count);

    // The counter is 32 bits and wraps. Advancing past the top and measuring
    // back is exact in unsigned arithmetic, which is the property the header
    // says subtracting raw values only gets right by accident.
    const Ticks nearTop = Ticks(static_cast<uint32_t>(atRunTime(0xFFFFFFFEu)));
    const Ticks wrapped = nearTop + 3;
    CHECK(wrapped.count == 1);
    CHECK(wrapped.since(nearTop) == 3);
}

UBIQUITTY_TEST_MAIN()
