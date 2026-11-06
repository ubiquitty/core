// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Emulation.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"

#include <cstdio>
#include <cstring>

// Which dialect the terminal speaks, on both axes: the sequence family, and how
// incoming bytes become codepoints. An unknown personality name must leave the
// caller's default in place rather than silently selecting one.

TEST(testEmulationRoundTrips)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::Modes::emulation(&t) == Ubiquitty::VT::Emulation::VT);

    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::VT);
    CHECK(Ubiquitty::Modes::emulation(&t) == Ubiquitty::VT::Emulation::VT);

    // The null-Terminal contract: a query on nullptr answers the default rather
    // than trapping, so a frontend probing before construction is safe.
    CHECK(Ubiquitty::Modes::emulation(nullptr) == Ubiquitty::VT::Emulation::VT);
}

TEST(test_emulationFromName_leaves_default_on_unknown)
{
    auto vt = Ubiquitty::Modes::emulationFromName("vt");
    CHECK(vt.has_value());
    CHECK(*vt == Ubiquitty::VT::Emulation::VT);

    // Unknown name returns std::nullopt.
    CHECK(!Ubiquitty::Modes::emulationFromName("nonesuch").has_value());

    // Null name returns std::nullopt.
    CHECK(!Ubiquitty::Modes::emulationFromName(nullptr).has_value());
}

TEST(testInputEncodingRoundTrips)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::Modes::inputEncoding(&t) == Ubiquitty::Unicode::Encoding::UTF8);

    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    CHECK(Ubiquitty::Modes::inputEncoding(&t) == Ubiquitty::Unicode::Encoding::Latin1);

    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::UTF8);
    CHECK(Ubiquitty::Modes::inputEncoding(&t) == Ubiquitty::Unicode::Encoding::UTF8);

    CHECK(Ubiquitty::Modes::inputEncoding(nullptr) == Ubiquitty::Unicode::Encoding::UTF8);
}

TEST(testInvalidUTF8PolicyRoundTrips)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::Modes::invalidUTF8Policy(&t) == Ubiquitty::Unicode::InvalidUTF8Policy::FFFD);

    Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Latin1);
    CHECK(Ubiquitty::Modes::invalidUTF8Policy(&t) == Ubiquitty::Unicode::InvalidUTF8Policy::Latin1);

    CHECK(Ubiquitty::Modes::invalidUTF8Policy(nullptr) == Ubiquitty::Unicode::InvalidUTF8Policy::FFFD);
}

UBIQUITTY_TEST_MAIN()
