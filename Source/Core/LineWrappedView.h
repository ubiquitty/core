// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty {
class CellGrid;

// See LineAttributeView.h for the CellGrid row-metadata seam.
struct LineWrappedView {
    Ubiquitty::CellGrid* owner;
    int screen;

    /// A writable stand-in for one row's soft-wrap bit.
    ///
    /// The flag is a bit inside ``RowMeta::flags``, which has no address, so
    /// `view[r] = w` needs an object that remembers the row and writes back on
    /// assignment. Reads convert, which is what every existing caller does.
    class Reference {
    public:
        Reference(Ubiquitty::CellGrid* owner, int screen, int row)
            : m_owner(owner)
            , m_screen(screen)
            , m_row(row)
        {
        }

        /// - Returns: The row's current soft-wrap state.
        operator bool() const;
        /// Writes `isWrapped` through to the row's flag.
        ///
        /// - Returns: This reference, so an assignment chains.
        Reference& operator=(bool isWrapped); // bool-ok: a proxy for a bool assigns the value it stands in for
        Reference& operator=(const Reference& other) { return *this = static_cast<bool>(other); }

    private:
        Ubiquitty::CellGrid* m_owner;
        int m_screen;
        int m_row;
    };

    Reference operator[](int r) { return { owner, screen, r }; }
    [[nodiscard]] bool operator[](int r) const;
};

} // namespace Ubiquitty
