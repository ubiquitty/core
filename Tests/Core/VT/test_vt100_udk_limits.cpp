// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_vt100_udk_limits — DECUDK's two capacities, from the far side.
//
// `udkMaxValueBytes` and `udkMaxEntries` bound a fixed array that rides on
// `Terminal`, so they are what stands between a DCS payload and a write past the
// end of `UDKEntry::value`. The sender picks both the value's length and how many
// keys it programs.
//
// Neither constant was named by any test. That is what a limit gets by default:
// the cases that drive DECUDK program one key with a short value, because a case
// written to exercise a feature supplies input the feature accepts, and the far
// side of a bound is the one place no such case goes.
//
// The verdicts are measured. The interesting one is that an over-long value is
// truncated rather than refused, which for a key whose value is an escape
// sequence means the application receives a partial one.
//
// What these cases cannot reach, stated rather than left to be discovered: the
// length clamp inside `udkSet`. The decode loop caps its buffer before the call
// and `udkSet` is file-static with that one caller, so no DECUDK payload makes
// the clamp fire — removing it fails nothing here. Removing the decode-loop cap
// instead aborts, so the two are not interchangeable. The clamp is a guard for a
// second caller that does not exist yet, and the sync decoder
// (Source/Core/Protocol/Sync/NativeAttachment.cpp) enforces both bounds again in its
// own copy rather than becoming that caller.

#include "VT100Fixture.h"

#include <string>
#include <string_view>

namespace {

/// The DECUDK sequence programming `keyCode` with `byteCount` copies of 'A'.
///
/// `Pl = 1` leaves the keys unlocked, because the default locks them after the
/// first load and every case here programs more than once.
std::string programKey(int keyCode, int byteCount)
{
    std::string sequence = "\033P1;1|" + std::to_string(keyCode) + "/";
    for (int i = 0; i < byteCount; ++i)
        sequence += "41"; // 'A'
    return sequence + "\033\\";
}

/// The entry for `keyCode`, or nullptr when no slot holds it.
const Ubiquitty::VT::UDKEntry* entryFor(const Ubiquitty::Terminal& t, int keyCode)
{
    const Ubiquitty::VT::UserDefinedKeys* keys = Ubiquitty::userDefinedKeysOrNull(t);
    if (!keys)
        return nullptr;
    for (int i = 0; i < keys->count; ++i) {
        if (keys->entries[i].keyCode == keyCode)
            return &keys->entries[i];
    }
    return nullptr;
}

} // namespace

TEST(decudkStoresAValueThatExactlyFillsTheBound)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    feed(t, programKey(17, Ubiquitty::VT::udkMaxValueBytes));
    const Ubiquitty::VT::UDKEntry* entry = entryFor(t, 17);
    CHECK(entry != nullptr);
    CHECK_EQ(static_cast<int>(entry->length), Ubiquitty::VT::udkMaxValueBytes);
}

TEST(decudkTruncatesAnOverLongValueRatherThanRefusingIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // One byte past the bound, and far past it. Both store the bound's worth:
    // the decode loop keeps consuming hex pairs and stops appending, so the key
    // ends up holding the value's FIRST bytes and the rest are dropped.
    feed(t, programKey(17, Ubiquitty::VT::udkMaxValueBytes + 1));
    const Ubiquitty::VT::UDKEntry* entry = entryFor(t, 17);
    CHECK(entry != nullptr);
    CHECK_EQ(static_cast<int>(entry->length), Ubiquitty::VT::udkMaxValueBytes);

    feed(t, programKey(18, Ubiquitty::VT::udkMaxValueBytes * 4));
    const Ubiquitty::VT::UDKEntry* long_ = entryFor(t, 18);
    CHECK(long_ != nullptr);
    CHECK_EQ(static_cast<int>(long_->length), Ubiquitty::VT::udkMaxValueBytes);

    // Truncated, not emptied: a key whose value is an escape sequence transmits a
    // partial one, which is a different thing for the application to receive than
    // nothing at all. Pinned so a change from truncate to refuse is a visible diff.
    CHECK_EQ(long_->value[0], 'A');
    CHECK_EQ(long_->value[Ubiquitty::VT::udkMaxValueBytes - 1], 'A');
}

TEST(decudkDropsAKeyPastTheTableCapacity)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // Key code 0 is the empty-slot marker, so the codes start at 1.
    for (int key = 1; key <= Ubiquitty::VT::udkMaxEntries; ++key)
        feed(t, programKey(key, 2));
    CHECK_EQ(Ubiquitty::ensureUserDefinedKeys(t).count, Ubiquitty::VT::udkMaxEntries);

    feed(t, programKey(Ubiquitty::VT::udkMaxEntries + 1, 2));
    CHECK_EQ(Ubiquitty::ensureUserDefinedKeys(t).count, Ubiquitty::VT::udkMaxEntries);
    CHECK(entryFor(t, Ubiquitty::VT::udkMaxEntries + 1) == nullptr);

    // A key already in the table is reprogrammed rather than appended, so a full
    // table still accepts a change to what it holds.
    feed(t, programKey(1, 5));
    CHECK_EQ(Ubiquitty::ensureUserDefinedKeys(t).count, Ubiquitty::VT::udkMaxEntries);
    const Ubiquitty::VT::UDKEntry* first = entryFor(t, 1);
    CHECK(first != nullptr);
    CHECK_EQ(static_cast<int>(first->length), 5);
}

TEST(decudkFreesASlotByMovingTheLastEntryIntoIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    for (int key = 1; key <= Ubiquitty::VT::udkMaxEntries; ++key)
        feed(t, programKey(key, 2));

    // An empty value clears the key. The freed slot takes the LAST live entry, so
    // the table's order is not the programming order — a case that assumed it was
    // would pass here and fail on whichever key happened to move.
    feed(t, programKey(1, 0));
    CHECK_EQ(Ubiquitty::ensureUserDefinedKeys(t).count, Ubiquitty::VT::udkMaxEntries - 1);
    CHECK(entryFor(t, 1) == nullptr);
    CHECK(entryFor(t, Ubiquitty::VT::udkMaxEntries) != nullptr);

    // And the freed capacity is real: a key the full table refused now lands.
    feed(t, programKey(Ubiquitty::VT::udkMaxEntries + 1, 2));
    CHECK_EQ(Ubiquitty::ensureUserDefinedKeys(t).count, Ubiquitty::VT::udkMaxEntries);
    CHECK(entryFor(t, Ubiquitty::VT::udkMaxEntries + 1) != nullptr);
}

// DECUDK's table is not part of a Terminal that never programmed a key.
TEST(testUserDefinedKeysTableIsAbsentUntilProgrammed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK_MSG(Ubiquitty::userDefinedKeysOrNull(t) == nullptr, "no DECUDK, no table");

    // Pc = 0 with no definition, Pl = 1: nothing to store, nothing to allocate.
    feed(t, "\033P0;1|\033\\");
    CHECK_MSG(Ubiquitty::userDefinedKeysOrNull(t) == nullptr, "a clear with no definition allocates nothing");

    feed(t, "\033P0;1|17/41\033\\");
    const Ubiquitty::VT::UserDefinedKeys* keys = Ubiquitty::userDefinedKeysOrNull(t);
    CHECK_MSG(keys != nullptr, "a programmed key allocates the table");
    CHECK(keys->count == 1);

    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::userDefinedKeysOrNull(t) == nullptr);
}

// A DECUDK with Pl 0 locks the keys, and the UDK status report says so: CSI
// ? 21 n once locked, CSI ? 20 n before. A later DECUDK is ignored.
TEST(testUDKStatusReportsTheLock)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char reply[32];
    feed(t, "\033[?25n");
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033[?20n");
    feed(t, "\033P1;0|17/414243\033\\\033[?25n");
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033[?21n");
    feed(t, "\033P1;1|18/444546\033\\");
    CHECK(t.udk != nullptr && t.udk->count == 1);
}

// DECSR resets the terminal as RIS does, the locked keys included, and answers
// a Pn from 1 to 16383 with DECSRC carrying it back. A Pn past 16383 voids the
// sequence, and none asks for no answer.
TEST(testSecureResetResetsAndConfirms)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char reply[32];
    feed(t, "\033P1;0|17/414243\033\\AB\033[2;3H\033[16384+p");
    CHECK(drain(t, reply) == 0);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "\033[7+p");
    drain(t, reply);
    CHECK(std::string_view(reply) == "\033[7*q");
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.udk == nullptr || (!t.udk->locked && t.udk->count == 0));
    feed(t, "AB\033[+p");
    CHECK(drain(t, reply) == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

UBIQUITTY_TEST_MAIN()
