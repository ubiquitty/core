// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Bits/OptionSet.h"

#include <bit>
#include <cstdint>

// Kitty keyboard protocol progressive-enhancement flags. Terminal stores them
// raw (uint16_t per screen); boundary crossings go through
// KeyFlagSet::fromRaw()/toRaw().

namespace Ubiquitty::Kitty {

/// One kitty keyboard-protocol progressive-enhancement flag.
///
/// The values are the protocol's bit numbers, so a flag set crosses the wire and
/// the DECRQM-style report without a mapping table. `Terminal` stores the set
/// raw as a `uint16_t` per screen; boundary crossings go through
/// `KeyFlagSet::fromRaw()` and `toRaw()`.
enum class KeyFlag : uint16_t {
    Disambiguate = 1,
    ReportEvents = 2,
    ReportAlternateKeys = 4,
    ReportAll = 8,
    ReportText = 16,
};

/// Returns whether a flag is a single bit, for the static assertion below.
///
/// - Parameter flag: The enumerator to test.
/// - Returns: True when exactly one bit is set.
constexpr bool keyFlagIsSingleBit(KeyFlag flag) { return std::has_single_bit(static_cast<uint16_t>(flag)); }
static_assert(
    keyFlagIsSingleBit(KeyFlag::Disambiguate) && keyFlagIsSingleBit(KeyFlag::ReportEvents) && keyFlagIsSingleBit(KeyFlag::ReportAlternateKeys) && keyFlagIsSingleBit(KeyFlag::ReportAll) && keyFlagIsSingleBit(KeyFlag::ReportText), "every KeyFlag enumerator must be a single bit (power of two)");

using KeyFlagSet = Bits::OptionSet<KeyFlag>;

} // namespace Ubiquitty::Kitty
