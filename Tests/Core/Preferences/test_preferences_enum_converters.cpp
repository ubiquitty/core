// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_enum_converters — every settings enum survives a round trip.
//
// Eight enums in the config layer each carry three converters: a name a config
// file writes, a name it reads back, and an integer the settings surface and the
// sync protocol both use. They are the largest uncovered region of Preferences.cpp,
// and they are the kind of code a copy-paste writes: eight near-identical
// switches where one arm returning its neighbour's string compiles, reads
// plausibly, and silently rewrites the user's file with a different setting.
//
// The property is one sentence rather than eight files of cases: a value that
// goes out as a name comes back as itself, and so does one that goes out as an
// integer.
//
// The enumerators are found rather than listed. `fromInt` over a range reports
// which integers the enum admits, so a case here covers every member without
// naming any — an enumerator added tomorrow is round-tripped by this file
// without anyone remembering to add it, which a hand-written list cannot
// promise. The count is asserted too, so an enum that loses every member does
// not pass vacuously over an empty set.

#include "Core/Preferences/Preferences.h"

#include <optional>
#include <string>

using namespace Ubiquitty::Preferences;

namespace {

/// Round-trips every value `fromInt` admits, through both names and integers.
///
/// - Parameters:
///   - fromInt: The integer reader for this enum.
///   - toString: The name writer.
///   - fromString: The name reader.
///   - probeLimit: How far to search for admitted integers.
/// - Returns: The number of values found, for the caller to bound.
template<typename FromInt, typename ToString, typename FromString>
int roundTripped(FromInt fromInt, ToString toString, FromString fromString, int probeLimit = 64)
{
    int found = 0;
    for (int candidate = 0; candidate < probeLimit; ++candidate) {
        const auto value = fromInt(candidate);
        if (!value.has_value())
            continue;
        ++found;

        // Out as a name, back as the same value.
        const char* name = toString(*value);
        CHECK(name != nullptr);
        const auto viaName = fromString(std::string(name));
        CHECK(viaName.has_value());
        CHECK(*viaName == *value);

        // Out as an integer, back as the same value. The two paths are separate
        // switches, so one can drift without the other.
        const auto viaInt = fromInt(static_cast<int>(*value));
        CHECK(viaInt.has_value());
        CHECK(*viaInt == *value);
    }
    return found;
}

} // namespace

TEST(everyCursorShapeSurvivesBothRoundTrips)
{
    const int found = roundTripped(cursorShapeFromInt, cursorShapeToString, cursorShapeFromString);
    CHECK(found >= 3); // block, underline, bar
}

TEST(everyCursorMotionModeSurvivesBothRoundTrips)
{
    const int found = roundTripped(cursorMotionFromInt, cursorMotionToString, cursorMotionFromString);
    CHECK(found >= 2);
}

TEST(everyTitlebarStyleSurvivesBothRoundTrips)
{
    const int found = roundTripped(titlebarStyleFromInt, titlebarStyleToString, titlebarStyleFromString);
    CHECK(found >= 2);
}

TEST(everyBeamSurfaceSurvivesBothRoundTrips)
{
    const int found = roundTripped(beamSurfaceFromInt, beamSurfaceToString, beamSurfaceFromString);
    CHECK(found >= 2);
}

TEST(everyColorSpaceSurvivesBothRoundTrips)
{
    const int found = roundTripped(colorSpaceFromInt, colorSpaceToString, colorSpaceFromString);
    CHECK(found >= 2);
}

TEST(everyBackgroundImageModeSurvivesBothRoundTrips)
{
    const int found = roundTripped(backgroundImageModeFromInt, backgroundImageModeToString,
        backgroundImageModeFromString);
    CHECK(found >= 2);
}

TEST(everyColorblindModeSurvivesBothRoundTrips)
{
    const int found = roundTripped(colorblindModeFromInt, colorblindModeToString, colorblindModeFromString);
    CHECK(found >= 2);
}

TEST(aNameNoEnumCarriesIsRefusedRatherThanDefaulted)
{
    // The readers answer `nullopt` so a caller can keep what it had. Returning a
    // default instead would rewrite a setting on a typo, and the round trips
    // above cannot see that: they only ever feed names the writer produced.
    CHECK(!cursorShapeFromString("no-such-shape").has_value());
    CHECK(!cursorMotionFromString("no-such-motion").has_value());
    CHECK(!titlebarStyleFromString("no-such-style").has_value());
    CHECK(!beamSurfaceFromString("no-such-surface").has_value());
    CHECK(!colorSpaceFromString("no-such-space").has_value());
    CHECK(!backgroundImageModeFromString("no-such-mode").has_value());
    CHECK(!colorblindModeFromString("no-such-vision").has_value());

    // An empty name is the shape a half-written config line produces, and it is
    // not a member of any of these.
    CHECK(!cursorShapeFromString("").has_value());
    CHECK(!colorSpaceFromString("").has_value());
}

TEST(anIntegerNoEnumCarriesIsRefused)
{
    // The sync protocol and the settings surface both hand these integers across
    // a trust boundary, so an out-of-range one has to come back empty rather
    // than index something.
    for (const int outside : { -1, 1000 }) {
        CHECK(!cursorShapeFromInt(outside).has_value());
        CHECK(!cursorMotionFromInt(outside).has_value());
        CHECK(!titlebarStyleFromInt(outside).has_value());
        CHECK(!beamSurfaceFromInt(outside).has_value());
        CHECK(!colorSpaceFromInt(outside).has_value());
        CHECK(!backgroundImageModeFromInt(outside).has_value());
        CHECK(!colorblindModeFromInt(outside).has_value());
    }
}

UBIQUITTY_TEST_MAIN()
