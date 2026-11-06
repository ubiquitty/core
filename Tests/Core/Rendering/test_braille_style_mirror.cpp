// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// `BrailleStyle` is declared twice on purpose, and this is what stops the two
// from drifting.
//
// UbiquittyKit ships a textual interface, and an interface naming a C++ type
// cannot be read by a consumer with no interop configured —
// the kit's interface gate compiles one with `-F` and nothing else, and that
// compile is the assertion. So the framework publishes a Swift enum whose
// raw values are these enumerators' own, and converts internally.
//
// A second declaration is a second thing to update, and nothing in a Swift file
// fails when a C++ enumerator is added beside it. This does: adding, removing
// or reordering a case here breaks a case below, and the message names the
// Swift case that has to move with it.

#include "Core/Rendering/BrailleLattice.h"

#include <cstdio>

using Ubiquitty::Rendering::BrailleStyle;

namespace {

// The Swift mirror's raw values, which are these ordinals. Kept as a table so
// the failure names the case rather than a count.
struct Mirror {
    BrailleStyle style;
    unsigned rawValue;
    const char* swiftCase;
};

constexpr Mirror mirror[] {
    { BrailleStyle::Regular, 0, "regular" },
    { BrailleStyle::Outline6Dot, 1, "outline6Dot" },
    { BrailleStyle::Outline8Dot, 2, "outline8Dot" },
    { BrailleStyle::Pinpoint6Dot, 3, "pinpoint6Dot" },
    { BrailleStyle::Pinpoint8Dot, 4, "pinpoint8Dot" },
    { BrailleStyle::Square, 5, "square" },
    { BrailleStyle::Raster, 6, "raster" },
};

} // namespace

// Each enumerator sits where the Swift raw value says it does. A reorder moves
// one of these and the case that names it fails.
TEST(testEachBrailleStyleKeepsTheOrdinalSwiftPublishes)
{
    for (const Mirror& row : mirror) {
        char message[192];
        std::snprintf(message, sizeof(message),
            "BrailleStyle::%s is no longer raw value %u — "
            "UbiquittyKit's `public enum BrailleStyle` declares `case %s = %u`",
            row.swiftCase, row.rawValue, row.swiftCase, row.rawValue);
        CHECK_MSG(static_cast<unsigned>(row.style) == row.rawValue, message);
    }
}

// The count is the half a reorder cannot catch: a case ADDED to one side and
// not the other leaves every existing ordinal correct.
TEST(testNoBrailleStyleIsAddedWithoutItsSwiftCase)
{
    // Raster is last, so one past it is the count. A new enumerator appended
    // after it makes this the wrong number, which is the point.
    constexpr unsigned declared = static_cast<unsigned>(BrailleStyle::Raster) + 1;
    constexpr unsigned mirrored = static_cast<unsigned>(sizeof(mirror) / sizeof(mirror[0]));
    CHECK_MSG(declared == mirrored,
        "a BrailleStyle enumerator was added or removed — add the matching case to "
        "UbiquittyKit's `public enum BrailleStyle`, its `core`/`from` arms, and a row "
        "to the table in this file");
}

UBIQUITTY_TEST_MAIN()
