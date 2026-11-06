// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Base/Types.h — portable primitive-type seam. Core compiles against the C++
// standard library alone, with no platform headers: no Metal, no AppKit, no
// sockets, no UI. This is the one include that guarantees the <cstdint> names
// plus std::size_t on every host.

// The toolchain floor rides here because most portable translation units reach
// this header, so a build on a compiler below the floor names the floor instead
// of failing only on the first facility the compiler lacks.
#include "Base/ToolchainFloor.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace Ubiquitty {

/// A count of 60 Hz ticks.
///
/// Every age budget in Core is denominated in these. The unit is what gets lost:
/// a count of milliseconds is the same `uint32_t`, so a backend supplying one
/// compiles and runs while every budget expires about sixteen times early. A
/// distinct type is the only thing that catches it.
///
/// Costs nothing over the count it carries — trivially copyable, passed in a
/// register. The constructor is `explicit` so a raw number cannot drift in, and
/// the named conversions carry the ratio so a backend never re-derives it.
///
/// `std::chrono::duration` carries a unit the same way and would be the obvious
/// choice. It is not used because the model layer counts in ticks without being
/// able to see PAL, and this type has to be nameable on both sides of that
/// boundary — Base is the only layer both can include.
///
/// Not because the STL cannot cross the seam: Docs/coding-conventions.md §1
/// allows it in Core, Runtime and the PAL seam alike, and PAL.h passes
/// `std::span` and `std::string_view` across it.
struct Ticks {
    uint32_t count = 0;

    constexpr Ticks() = default;
    explicit constexpr Ticks(uint32_t rawCount)
        : count(rawCount)
    {
    }

    /// Ticks covering `milliseconds`.
    [[nodiscard]] static constexpr Ticks fromMilliseconds(uint64_t milliseconds) { return Ticks(static_cast<uint32_t>(milliseconds * 60ull / 1000ull)); }

    /// Ticks covering `nanoseconds` — for a backend reading a `timespec` or a
    /// high-resolution browser clock.
    [[nodiscard]] static constexpr Ticks fromNanoseconds(uint64_t nanoseconds) { return Ticks(static_cast<uint32_t>(nanoseconds * 60ull / 1000000000ull)); }

    /// Ticks covering `seconds` plus `nanoseconds`, rounded to nearest — the
    /// shape `clock_gettime` hands back. Rounding rather than truncating keeps a
    /// measurement straddling a tick boundary from always landing short.
    [[nodiscard]] static constexpr Ticks fromTimespec(uint64_t seconds, uint64_t nanoseconds)
    {
        constexpr uint64_t nanosecondsPerTick = 1000000000ull / 60ull;
        return Ticks(static_cast<uint32_t>(seconds * 60ull + (nanoseconds + nanosecondsPerTick / 2) / nanosecondsPerTick));
    }

    /// Ticks elapsed since `earlier`. The counter wraps, and unsigned arithmetic
    /// makes this exact across the wrap — which subtracting the raw values by
    /// hand only gets right by accident.
    [[nodiscard]] constexpr uint32_t since(Ticks earlier) const { return count - earlier.count; }

    [[nodiscard]] constexpr Ticks operator+(uint32_t addedTicks) const { return Ticks(count + addedTicks); }

    [[nodiscard]] friend constexpr bool operator==(Ticks, Ticks) = default;
};

static_assert(sizeof(Ticks) == sizeof(uint32_t), "Ticks must cost nothing over the count it carries");

/// Converts `value` to `To`, clamping to that type's range instead of wrapping.
///
/// The one operation behind a recurring defect class: an unsigned length
/// narrowed to a signed count. Written by hand it is
/// `static_cast<int>(std::min<std::size_t>(n, INT_MAX))`, which is correct only
/// while the reader remembers that `std::min` must come first. Bounded after
/// the narrowing, a two-gibibyte length is already negative and the clamp passes
/// it through. Every instance of that shape is the same bug, so the ordering
/// belongs in one function rather than in each caller's memory.
///
/// `std::in_range` and `std::cmp_less` do the mixed-signedness comparisons that
/// `<` gets wrong: comparing a `std::size_t` against a negative `int` promotes
/// the int and comes out backwards, which is the second half of the same class.
///
/// C++26 standardises this as `std::saturate_cast`. When the toolchain floor
/// (Source/Base/ToolchainFloor.h) reaches it, this becomes an alias and then
/// nothing. The name and the saturating semantics are the standard's, so the
/// replacement is a deletion rather than a migration.
///
/// - Parameter value: the number to convert.
/// - Returns: `value` as `To`, or that type's nearest bound when it does not fit.
template<std::integral To, std::integral From>
[[nodiscard]] constexpr To saturatingCast(From value) noexcept
{
    if (std::in_range<To>(value))
        return static_cast<To>(value);
    return std::cmp_less(value, std::numeric_limits<To>::min())
        ? std::numeric_limits<To>::min()
        : std::numeric_limits<To>::max();
}

static_assert(saturatingCast<int>(std::size_t { 42 }) == 42, "a value in range converts unchanged");
static_assert(saturatingCast<int>(std::numeric_limits<std::size_t>::max()) == std::numeric_limits<int>::max(),
    "a length past INT_MAX saturates rather than wrapping negative");
static_assert(saturatingCast<unsigned>(-1) == 0u, "a negative saturates to zero rather than to the top");
static_assert(saturatingCast<std::uint8_t>(300) == 255, "a narrowing that overflows takes the ceiling");

} // namespace Ubiquitty
