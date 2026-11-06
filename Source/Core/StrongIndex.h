// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty {

// --- Strong index types ----------------------------------------------------
// Catch "passed column where row was expected" at compile time. Zero-cost
// wrapper around int, written once; each address space mints its own alias
// with a distinct tag so mixing them is un-compilable.

template<typename Tag>
class StrongIndex {
public:
    constexpr StrongIndex()
        : m_value(0)
    {
    }
    constexpr explicit StrongIndex(int v)
        : m_value(v)
    {
    }
    [[nodiscard]] constexpr int value() const { return m_value; }
    constexpr bool operator==(StrongIndex o) const { return m_value == o.m_value; }
    constexpr bool operator!=(StrongIndex o) const { return m_value != o.m_value; }
    constexpr bool operator<(StrongIndex o) const { return m_value < o.m_value; }
    constexpr bool operator<=(StrongIndex o) const { return m_value <= o.m_value; }
    constexpr StrongIndex operator+(int d) const { return StrongIndex(m_value + d); }
    constexpr StrongIndex operator-(int d) const { return StrongIndex(m_value - d); }

private:
    int m_value;
};

// Visible viewport row / column (the CellGrid paint address space).
using Row = StrongIndex<struct RowTag>;
using Column = StrongIndex<struct ColumnTag>;

} // namespace Ubiquitty
