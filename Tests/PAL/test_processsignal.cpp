// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_processsignal.cpp — the shape of the signal table, and the sentinel an
// unknown mnemonic maps to. Needs the C++ library alone: no PTY and no
// platform backend.

#include "Harness.h"

#include "PAL/ProcessSignal.h"

#include <csignal>
#include <cstddef>
#include <set>
#include <span>
#include <string_view>

namespace Signal = Ubiquitty::PAL::Process::Signal;
using Signal::numberForMnemonic;

namespace {

// INFO is listed only where the platform defines SIGINFO.
#ifdef SIGINFO
constexpr std::size_t expectedEntryCount = 14;
#else
constexpr std::size_t expectedEntryCount = 13;
#endif

// Strings that resemble a mnemonic without being one: a stray word, nothing,
// a real mnemonic in the wrong case, one with letters added, one cut short,
// and a control byte.
constexpr std::string_view nearMisses[] = { "nope", "", "term", "TERMINATE", "TER", "\x01" };

} // namespace

TEST(testTableShape)
{
    const std::span<const Signal::Entry> entries = Signal::table();
    CHECK_EQ(entries.size(), expectedEntryCount);
    CHECK(entries.front().mnemonic == "TSTP");

    for (const auto& [label, mnemonic, number] : entries) {
        CHECK(!mnemonic.empty());
        CHECK(!label.empty());
        // A listed signal has to be one that can be sent. Zero is taken as the
        // answer for an unknown mnemonic, and a negative number addresses a
        // whole process group.
        CHECK(number > 0);
    }
}

TEST(testMnemonicsAreUnique)
{
    std::set<std::string_view> mnemonicsSoFar;
    for (const Signal::Entry& entry : Signal::table()) {
        const bool isFirstSighting = mnemonicsSoFar.insert(entry.mnemonic).second;
        CHECK(isFirstSighting);
    }
}

TEST(testKnownLookups)
{
    CHECK_EQ(numberForMnemonic("TERM"), SIGTERM);
    CHECK_EQ(numberForMnemonic("KILL"), SIGKILL);
#ifdef SIGINFO
    CHECK_EQ(numberForMnemonic("INFO"), SIGINFO);
#else
    CHECK_EQ(numberForMnemonic("INFO"), 0);
#endif
    CHECK_EQ(numberForMnemonic("WINCH"), SIGWINCH);
}

TEST(testEveryTableEntryRoundTrips)
{
    for (const Signal::Entry& listed : Signal::table())
        CHECK_EQ(numberForMnemonic(listed.mnemonic), listed.number);
}

// Callers hand the answer straight to kill(2), which reads 0 as a harmless
// check that the process exists and a negative number as an entire process
// group. That makes the sentinel a safety property: no unknown mnemonic may
// reach further than the probe.
TEST(testUnknownMnemonicIsNotSendable)
{
    for (const std::string_view mnemonic : nearMisses) {
        const int signalNumber = numberForMnemonic(mnemonic);
        CHECK(signalNumber >= 0); // never a process-group target
        CHECK_EQ(signalNumber, 0); // and the documented sentinel itself
    }
}

UBIQUITTY_TEST_MAIN()
