// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// BitField<Storage, Shift, Width>: constexpr shift/mask arithmetic for a small
// integer SUBFIELD packed into a wider unsigned integer (sibling to OptionSet.h,
// which carries single-bit flags). Holds no storage of its own, so the member
// stays a trivially-copyable integer. set() truncates to the field width, so a
// subfield write never disturbs neighbouring flags or fields in the same byte.

#include <concepts>

namespace Ubiquitty::Bits {

template<std::unsigned_integral Storage, unsigned Shift, unsigned Width> struct BitField {
    static_assert(Width > 0, "BitField requires a non-empty width");
    static_assert(Shift + Width <= sizeof(Storage) * 8, "BitField must fit its storage type");

    // A full-width field is all-ones spelt as ~0 rather than (1 << Width), to
    // avoid the undefined shift-by-type-width case.
    static constexpr Storage maxValue = Width >= sizeof(Storage) * 8 ? static_cast<Storage>(~Storage {}) : static_cast<Storage>((Storage { 1 } << Width) - 1);
    static constexpr Storage mask = static_cast<Storage>(maxValue << Shift);

    // Read the subfield out of `storage`.
    [[nodiscard]] static constexpr Storage get(Storage storage) { return static_cast<Storage>((storage & mask) >> Shift); }

    // Return `storage` with the subfield replaced by `value` (truncated to the
    // field width); every bit outside the field is preserved.
    [[nodiscard]] static constexpr Storage set(Storage storage, Storage value) { return static_cast<Storage>((storage & static_cast<Storage>(~mask)) | (static_cast<Storage>(value & maxValue) << Shift)); }
};

} // namespace Ubiquitty::Bits
