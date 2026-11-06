// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_proc_stat_fields.cpp — where the ids sit in a /proc/<pid>/stat line.
//
// The parse is pure, so this feeds it lines rather than processes: a crafted
// comm is the case that matters and no real process on the machine running
// this is guaranteed to have one. Runs on every host for the same reason —
// nothing here reads /proc.

#include "Platform/POSIX/ProcStatFields.h"

using Ubiquitty::PAL::parseProcStatFields;
using Ubiquitty::PAL::ProcStatFields;

TEST(anOrdinaryStatLineYieldsTheParentAndGroup)
{
    // Field order after comm: state, ppid, pgrp, session, tty_nr, ...
    ProcStatFields fields {};
    CHECK(parseProcStatFields("1234 (zsh) S 1000 1234 1234 34816 1234 4194304", fields));
    CHECK_EQ(fields.parentProcessID, 1000u);
    CHECK_EQ(fields.processGroupID, 1234u);
    CHECK_EQ(fields.state, 'S');
}

TEST(theRunStateIsTheKernelsCharacterAndZombieIsNotSpecial)
{
    // The inspector shows the zombie flag, so this is read rather than skipped.
    // No mapping happens here: the caller decides what 'Z' means, and a state
    // this parse does not recognise is still the state the kernel reported.
    ProcStatFields fields {};
    CHECK(parseProcStatFields("9 (defunct) Z 1 9 9 0", fields));
    CHECK_EQ(fields.state, 'Z');
    CHECK(parseProcStatFields("9 (x) T 1 9 9 0", fields));
    CHECK_EQ(fields.state, 'T');
    CHECK(parseProcStatFields("9 (x) I 1 9 9 0", fields));
    CHECK_EQ(fields.state, 'I');
}

TEST(aCommHoldingABracketAndSpacesDoesNotMoveTheFields)
{
    // The hazard the parse exists for. comm is the executable's own name, so
    // this is a name a process can actually have — and a whitespace split
    // reads `1` and `2` out of the NAME, answering plausible numbers that
    // describe nothing. Wrong here is worse than failing: the walk would take
    // a hostile name as a claim about ancestry.
    ProcStatFields fields {};
    CHECK(parseProcStatFields("1234 (foo) 1 2 3) S 1000 1234 1234 0 0 0", fields));
    CHECK_MSG(fields.parentProcessID == 1000u, "a bracket in comm moved the parent id");
    CHECK_MSG(fields.processGroupID == 1234u, "a bracket in comm moved the group id");
    // The state is the field a first-bracket parse takes straight out of the
    // name: it would read '1' here and report it as a run state the kernel
    // never named.
    CHECK_MSG(fields.state == 'S', "a bracket in comm moved the run state");
}

TEST(aCommThatIsOnlyBracketsAndSpacesStillResolves)
{
    ProcStatFields fields {};
    CHECK(parseProcStatFields("7 ()))  ( ) S 42 7 7 0", fields));
    CHECK_EQ(fields.parentProcessID, 42u);
    CHECK_EQ(fields.processGroupID, 7u);
}

TEST(aLineWithNoClosingBracketIsRefused)
{
    // Refusing is the answer: a zeroed node is skipped by the classifier, and
    // a guessed one would be a process claiming an ancestry nobody stated.
    ProcStatFields fields { .parentProcessID = 5u, .processGroupID = 6u, .state = 'X' };
    CHECK(!parseProcStatFields("1234 zsh S 1000 1234", fields));
    CHECK_MSG(fields.parentProcessID == 5u, "a refused parse wrote through its out-parameter");
    CHECK_MSG(fields.state == 'X', "a refused parse wrote a state through its out-parameter");
}

TEST(aTruncatedLineIsRefusedRatherThanHalfRead)
{
    // A short read leaves the group id absent. Half a pair is not a pair, and
    // taking the parent alone would pair it with whatever the caller had.
    ProcStatFields fields {};
    CHECK(!parseProcStatFields("1234 (zsh) S 1000", fields));
    CHECK(!parseProcStatFields("1234 (zsh) S", fields));
    CHECK(!parseProcStatFields("1234 (zsh)", fields));
    CHECK(!parseProcStatFields("", fields));
}

TEST(anIdTooLargeForAProcessIsRefused)
{
    // 32 bits is what RawNode carries, so a longer run of digits is not a pid
    // that got truncated — it is a line this parse cannot represent.
    ProcStatFields fields {};
    CHECK(!parseProcStatFields("1 (x) S 99999999999 1", fields));
}

TEST(aNonNumericFieldIsRefusedRatherThanReadAsZero)
{
    // strtol-style parsing answers 0 here, which is a live pid in every table
    // that uses 0 for "no parent" — so a refusal and a zero are different
    // claims and this must make the first one.
    ProcStatFields fields {};
    CHECK(!parseProcStatFields("1 (x) S - 1", fields));
}

TEST(theKernelFlagsAreTheNinthField)
{
    // state, ppid, pgrp, session, tty_nr, tpgid, flags. tpgid is -1 off a
    // terminal, and a fork that has not exec'd carries PF_FORKNOEXEC.
    using Ubiquitty::PAL::parseProcStatFlags;
    using Ubiquitty::PAL::processFlagForkedWithoutExec;
    const auto forked = parseProcStatFlags("601 (zsh) S 600 600 500 34816 600 4194368 120 0");
    CHECK(forked.has_value());
    CHECK(forked && (*forked & processFlagForkedWithoutExec) != 0);
    const auto executed = parseProcStatFlags("602 (sleep) S 600 600 500 0 -1 4194304 120 0");
    CHECK(executed.has_value());
    CHECK(executed && (*executed & processFlagForkedWithoutExec) == 0);
    // The same hazard as the ids: a bracketed comm does not move the field.
    CHECK(parseProcStatFlags("7 (a) 1 2 3 4 5 6) S 1 7 7 0 -1 64 0") == std::optional<std::uint32_t> { 64u });
    // A line that stops short of the flags has none to give.
    CHECK(!parseProcStatFlags("1234 (zsh) S 1000 1234 1234 34816 1234").has_value());
    CHECK(!parseProcStatFlags("1234 zsh S 1000 1234 1234 34816 1234 64").has_value());
    CHECK(!parseProcStatFlags("1 (x) S 1 1 1 0 -1 -").has_value());
}

TEST(theIgnoredInterruptIsBitOneOfSigIgn)
{
    using Ubiquitty::PAL::parseStatusIgnoresInterrupt;
    CHECK(parseStatusIgnoresInterrupt("Name:\tsleep\nSigBlk:\t0000000000000000\nSigIgn:\t0000000000000006\nSigCgt:\t0\n") == std::optional<bool> { true });
    CHECK(parseStatusIgnoresInterrupt("Name:\tsleep\nSigIgn:\t0000000000384004\n") == std::optional<bool> { false });
    CHECK(!parseStatusIgnoresInterrupt("Name:\tsleep\nSigCgt:\t0\n").has_value());
    CHECK(!parseStatusIgnoresInterrupt("Name:\tsleep\nSigIgn:\tzz\n").has_value());
}

TEST(theThreadCountIsReadFromStatus)
{
    // A process whose first thread ended reads Z with its others running on;
    // the count is what tells it from one that has ended.
    using Ubiquitty::PAL::parseStatusThreadCount;
    CHECK(parseStatusThreadCount("Name:\tthreadexit\nState:\tZ (zombie)\nTgid:\t37\nThreads:\t2\nSigQ:\t0/1\n") == std::optional<std::uint32_t> { 2 });
    CHECK(parseStatusThreadCount("Name:\tsleep\nState:\tZ (zombie)\nThreads:\t1\n") == std::optional<std::uint32_t> { 1 });
    CHECK(!parseStatusThreadCount("Name:\tsleep\nState:\tS (sleeping)\n").has_value());
    CHECK(!parseStatusThreadCount("Name:\tsleep\nThreads:\t-\n").has_value());
}

TEST(processBirthTicksAreExactAndIgnoreParenthesesInNames)
{
    using Ubiquitty::PAL::parseProcStatBirthTicks;
    constexpr std::string_view prefix = "9 (a) 1 2 3) S 1 9 9 0 -1 0 0 0 0 0 0 0 0 0 0 0 1 0 ";
    CHECK(parseProcStatBirthTicks(std::string(prefix) + "12345 678\n") == std::optional<std::uint64_t> { 12345 });
    CHECK(parseProcStatBirthTicks(std::string(prefix) + "18446744073709551615\n") == std::optional<std::uint64_t> { UINT64_MAX });
    CHECK(!parseProcStatBirthTicks(std::string(prefix) + "18446744073709551616\n"));
    CHECK(!parseProcStatBirthTicks(std::string(prefix) + "12x "));
    CHECK(!parseProcStatBirthTicks(std::string(prefix) + "-1 "));
    CHECK(!parseProcStatBirthTicks(prefix));
    CHECK(!parseProcStatBirthTicks("9 (short) S 1 9"));
    CHECK(!parseProcStatBirthTicks("9 unparenthesized S 1 9"));
}

UBIQUITTY_TEST_MAIN()
