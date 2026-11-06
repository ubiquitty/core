// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>

namespace Ubiquitty::VT {

// DECUDK (User-Defined Keys) storage. Stored inline on Terminal; the input encoder
// (Source/Core/Keys/Keymap.cpp)
// reads it to resolve a programmed key's emission, and the DCS parser
// (Source/Core/VT/VT.cpp) populates it.
//
// Keys are addressed by their DEC function-key code (the same numbers DECUDK's
// payload uses, e.g. F6 = 17, Remove/Delete = 3). A programmed value is the
// decoded byte string the key transmits in place of its default sequence.
inline constexpr int udkMaxEntries = 64; // distinct programmable keys
inline constexpr int udkMaxValueBytes = 64; // decoded bytes per key

/// One programmed key: its DEC function-key code and the bytes it transmits.
struct UDKEntry {
    uint16_t keyCode; // DEC function-key code (0 = empty slot)
    uint16_t length; // decoded byte count in value
    char value[udkMaxValueBytes]; // decoded transmit string (not NUL-terminated)
};

/// Every DECUDK-programmed key for one terminal, stored inline.
///
/// Fixed capacity rather than a container because this rides on ``Terminal``
/// and is snapshotted per frame; a heap allocation here would be one per pane
/// per resize.
struct UserDefinedKeys {
    UDKEntry entries[udkMaxEntries];
    int count; // live entries in [0, udkMaxEntries)
    bool locked; // DECUDK Pl = 0 locked the keys; further DECUDK is ignored
                 // until a hard reset (RIS) clears the lock.
};

} // namespace Ubiquitty::VT
