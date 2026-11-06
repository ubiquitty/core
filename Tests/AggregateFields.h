// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/AggregateFields.h — pin how many fields an aggregate has, so growing one
// is a compile error where the fakes live.
//
// Adding a pure virtual to an interface breaks every fake that implements it,
// which is the good case: the compiler names each one and somebody decides what
// it should mean. Adding a field to a struct tests construct themselves does the
// opposite. It defaults, everything still compiles, every assertion still
// passes, and the new field is exercised by nothing — the same silence as a fake
// that keeps compiling after it has stopped meaning what it meant.
//
// A static_assert on the field count restores the loud version. It fails beside
// the fakes, at the people who have to choose a value, in the commit that adds
// the field rather than whenever someone next reads the test.
//
// Aggregates only. A type with a user-declared constructor is not
// brace-initialisable field-by-field, so pin its constructor's arity instead.
//

#include <cstddef>
#include <type_traits>
#include <utility>

namespace UbiquittyTest {

/// Converts to anything except `T` itself.
///
/// The exclusion does not change the count below, which was measured rather than
/// assumed: with it and without it, clang reports the same arity for aggregates
/// of 0, 1, 2 and 3 fields. Taking the LARGEST well-formed initialiser list is
/// what makes it safe — `T { one }` matching `T`'s copy constructor adds a
/// spurious success at one, and a spurious success cannot lower a maximum.
///
/// Retained because that safety belongs to the search strategy, not to the
/// operator. A count that stopped at the first well-formed list, or searched
/// downward from a bound, would report 1 for every type without this — which is
/// why the idiom carries the constraint in the wild, and why removing it here
/// would leave a trap for whoever rewrites the search.
template<typename T> struct AnyFieldBut {
    template<typename U>
        requires(!std::is_same_v<std::remove_cvref_t<U>, T>)
    constexpr operator U() const noexcept;
};

/// Whether `T` can be brace-initialised with exactly `sizeof...(I)` members.
///
/// - Returns: True when that many initialisers are well-formed for `T`.
template<typename T, std::size_t... I> constexpr bool acceptsFieldCount(std::index_sequence<I...>)
{
    return requires { T { (I, AnyFieldBut<T> {})... }; };
}

/// The number of fields in the aggregate `T`.
///
/// Counts up until brace initialisation stops being well-formed, so a field with
/// a default initialiser is counted the same as one without — which is the case
/// worth catching, since that is the one that changes nothing else.
///
/// - Returns: The field count, as a constant expression usable in a static_assert.
template<typename T, std::size_t N = 0> constexpr std::size_t aggregateFieldCount()
{
    if constexpr (acceptsFieldCount<T>(std::make_index_sequence<N + 1> {}))
        return aggregateFieldCount<T, N + 1>();
    else
        return N;
}

} // namespace UbiquittyTest
