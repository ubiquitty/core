// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// OptionSet<E>: a type-safe set of the single-bit enumerators of an
// `enum class E : UnderlyingType`. Storage is e's underlying type, so a set is
// exactly as wide as a wire field and (de)serializes via fromRaw()/toRaw().

#include <bit>
#include <initializer_list>
#include <type_traits>

namespace Ubiquitty::Bits {

template<typename E> class OptionSet {
public:
    using StorageType = std::underlying_type_t<E>;
    static_assert(std::is_enum_v<E>, "OptionSet requires an enum class");
    static_assert(std::is_unsigned_v<StorageType>, "OptionSet requires an unsigned underlying type");

    constexpr OptionSet() = default;
    constexpr OptionSet(E flag)
        : m_storage(static_cast<StorageType>(flag))
    {
    }
    constexpr OptionSet(std::initializer_list<E> flags)
    {
        for (E flag : flags)
            m_storage |= static_cast<StorageType>(flag);
    }

    // fromRaw preserves unknown high bits (forward-compatible with a future
    // peer's newer enumerator).
    static constexpr OptionSet fromRaw(StorageType raw)
    {
        OptionSet set;
        set.m_storage = raw;
        return set;
    }
    [[nodiscard]] constexpr StorageType toRaw() const { return m_storage; }

    [[nodiscard]] constexpr bool isEmpty() const { return !m_storage; }
    constexpr explicit operator bool() const { return !isEmpty(); }
    [[nodiscard]] constexpr unsigned size() const { return static_cast<unsigned>(std::popcount(m_storage)); }

    [[nodiscard]] constexpr bool contains(E flag) const { return containsAll(flag); }
    // any-of (legacy `(caps & (A|B)) != 0`): true iff at least one bit overlaps.
    [[nodiscard]] constexpr bool containsAny(OptionSet other) const { return m_storage & other.m_storage; }
    // all-of: true iff every bit of `other` is present.
    [[nodiscard]] constexpr bool containsAll(OptionSet other) const { return (m_storage & other.m_storage) == other.m_storage; }

    constexpr void add(OptionSet other) { m_storage |= other.m_storage; }
    constexpr void remove(OptionSet other) { m_storage &= ~other.m_storage; }
    // `value` is the flag's target state, not a mode toggle.
    constexpr void set(E flag, bool value)
    {
        if (value)
            add(flag);
        else
            remove(flag);
    }

    friend constexpr OptionSet operator|(OptionSet lhs, OptionSet rhs) { return fromRaw(lhs.m_storage | rhs.m_storage); }
    friend constexpr OptionSet operator&(OptionSet lhs, OptionSet rhs) { return fromRaw(lhs.m_storage & rhs.m_storage); }
    friend constexpr OptionSet operator-(OptionSet lhs, OptionSet rhs) { return fromRaw(lhs.m_storage & ~rhs.m_storage); }
    // Symmetric difference — flips the bits of `other`.
    friend constexpr OptionSet operator^(OptionSet lhs, OptionSet rhs) { return fromRaw(lhs.m_storage ^ rhs.m_storage); }
    constexpr OptionSet& operator|=(OptionSet other)
    {
        add(other);
        return *this;
    }
    constexpr OptionSet& operator^=(OptionSet other)
    {
        m_storage ^= other.m_storage;
        return *this;
    }
    // Flip the given flags in place (present ⇢ absent and vice versa).
    constexpr void toggle(OptionSet other) { m_storage ^= other.m_storage; }

    friend constexpr bool operator==(OptionSet, OptionSet) = default;

    // Forward iteration over the contained enumerators, low bit first.
    class Iterator {
    public:
        constexpr E operator*() const { return static_cast<E>(m_remaining & (~m_remaining + 1)); }
        constexpr Iterator& operator++()
        {
            m_remaining &= m_remaining - 1; // clear lowest set bit
            return *this;
        }
        constexpr bool operator==(const Iterator&) const = default;

    private:
        friend class OptionSet;
        constexpr explicit Iterator(StorageType remaining)
            : m_remaining(remaining)
        {
        }
        StorageType m_remaining { 0 };
    };
    [[nodiscard]] constexpr Iterator begin() const { return Iterator(m_storage); }
    [[nodiscard]] constexpr Iterator end() const { return Iterator(0); }

private:
    StorageType m_storage { 0 };
};

} // namespace Ubiquitty::Bits
