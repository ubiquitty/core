// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Terminal.h"
#include "Harness.h"

// test_pane_directory.cpp — pins which directory a pane answers with, and
// the resolve that keeps its two sources apart (Core/Terminal.h).
//
// The two sources disagree on purpose: a shell reports where it is, and the
// PTY's foreground process is sampled for the directory that process is in,
// which for an ssh or a tmux client is the client's own directory. Both arms
// arbitrated privately and the model held one string they took turns
// assigning, so whichever wrote last won. Bare libc++; no platform code.

using Ubiquitty::hasReportOutlivedItsProgram;
using Ubiquitty::paneDirectory;

TEST(testTheReportWinsAndTheSampleStandsIn)
{
    CHECK(paneDirectory({ .reportedDirectory = "/reported", .sampledDirectory = "/sampled" }) == "/reported");
    CHECK_MSG(paneDirectory({ .reportedDirectory = {}, .sampledDirectory = "/sampled" }) == "/sampled",
        "a shell with no integration still carries a directory");
    CHECK(paneDirectory({ .reportedDirectory = "/reported", .sampledDirectory = {} }) == "/reported");
    CHECK(paneDirectory({}).empty());
}

TEST(testAReportOutlivesItsProgramOnlyWhenBothGroupsAreKnownAndDiffer)
{
    CHECK_MSG(hasReportOutlivedItsProgram(41, 42), "the reporter is gone, so its directory is too");
    CHECK_MSG(!hasReportOutlivedItsProgram(42, 42), "the same program still holds the terminal");
    CHECK_MSG(!hasReportOutlivedItsProgram(0, 42), "nothing was reported");
    CHECK_MSG(!hasReportOutlivedItsProgram(42, 0), "an unsampled foreground retires nothing");
}

// The defect this exists for: a sample that assigns the same field the report
// does lets a probe landing after an ssh's report replace a remote directory
// with the local client's.
TEST(testTheSampleDoesNotOverwriteTheReport)
{
    Ubiquitty::Terminal term;
    CHECK(Ubiquitty::setReportedDirectoryString(term, "/remote/tree"));
    CHECK(term.currentDirectory == "/remote/tree");

    CHECK_MSG(!Ubiquitty::setSampledDirectoryString(term, "/local/home"),
        "a sample under a live report moves nothing the frontend can see");
    CHECK(term.currentDirectory == "/remote/tree");
    CHECK_MSG(term.sampledDirectory == "/local/home", "and is still recorded, for when the report retires");
}

TEST(testRetiringTheReportUncoversTheSample)
{
    Ubiquitty::Terminal term;
    Ubiquitty::setSampledDirectoryString(term, "/local/home");
    Ubiquitty::setReportedDirectoryString(term, "/remote/tree");
    CHECK(term.currentDirectory == "/remote/tree");

    CHECK_MSG(!Ubiquitty::retireReportedDirectoryIfStale(term, 900), "the reporter still holds the terminal");
    CHECK(term.currentDirectory == "/remote/tree");

    CHECK_MSG(Ubiquitty::retireReportedDirectoryIfStale(term, 901), "the reporter exited");
    CHECK_MSG(term.currentDirectory == "/local/home", "so the sample stands in");
    CHECK(term.reportedDirectory.empty());
    CHECK(term.reportingProcessGroup == 0);

    CHECK_MSG(!Ubiquitty::retireReportedDirectoryIfStale(term, 902), "and retiring nothing moves nothing");
}

// A report can land before any probe has run, and one with no reporter could
// never be found to have outlived one: the first program observed holding the
// terminal adopts it.
TEST(testAnUnstampedReportIsAdoptedByTheProgramHoldingTheTerminal)
{
    Ubiquitty::Terminal term;
    Ubiquitty::setSampledDirectoryString(term, "/local/home");
    Ubiquitty::setReportedDirectoryString(term, "/reported");
    CHECK(term.currentDirectory == "/reported");

    CHECK_MSG(!Ubiquitty::retireReportedDirectoryIfStale(term, 700), "the first observed program adopts it");
    CHECK(term.reportingProcessGroup == 700);
    CHECK(term.currentDirectory == "/reported");

    CHECK_MSG(Ubiquitty::retireReportedDirectoryIfStale(term, 701), "and the next one retires it");
    CHECK(term.currentDirectory == "/local/home");
}

TEST(testResolvingIsIdempotentAndReportsOnlyRealMovement)
{
    Ubiquitty::Terminal term;
    CHECK(Ubiquitty::setSampledDirectoryString(term, "/one"));
    CHECK_MSG(!Ubiquitty::setSampledDirectoryString(term, "/one"), "a re-sample of the same directory is not a change");
    CHECK(!Ubiquitty::resolvePaneDirectory(term));
    CHECK(Ubiquitty::setSampledDirectoryString(term, "/two"));
    CHECK(term.currentDirectory == "/two");
}

// A window title belongs to the program that set it, exactly as a directory
// report does: an editor that sets one and quits must not leave it on a window
// running something else, and a shell that sets no title writes nothing that
// would ever clear it.
TEST(testATitleRetiresWithTheProgramThatSetIt)
{
    Ubiquitty::Terminal term;
    Ubiquitty::setTitleUTF8String(term, "editing a file");
    CHECK_MSG(!Ubiquitty::retireTitleIfStale(term, 500), "the probe after the title stamps the program holding the terminal");
    CHECK(term.titleOwnerProcessGroup == 500);

    CHECK_MSG(!Ubiquitty::retireTitleIfStale(term, 500), "its program still holds the terminal");
    CHECK(term.titleUTF8 == "editing a file");

    CHECK_MSG(Ubiquitty::retireTitleIfStale(term, 501), "its program exited");
    CHECK_MSG(term.titleUTF8.empty(), "so the composed fallback answers again");
    CHECK(term.titleOwnerProcessGroup == 0);
}

// The last probe can predate the bytes that set a title. A shell that titles
// its pane right after a command exits — `git log; printf '\e]2;git log\a'` —
// is parsed while the last probe still names the command, and a title stamped
// with that group would be retired by the very next probe, which finds the shell.
// Which program set a title is read after the bytes that set it, never before.
TEST(testATitleSetAfterACommandExitedBelongsToTheShellThatSetIt)
{
    Ubiquitty::Terminal term;
    term.foregroundProcessGroup = 41; // the last probe ran while the command held the terminal
    Ubiquitty::setTitleUTF8String(term, "git log");

    CHECK_MSG(!Ubiquitty::retireTitleIfStale(term, 40), "the shell that set it holds the terminal");
    CHECK(term.titleUTF8 == "git log");
    CHECK(term.titleOwnerProcessGroup == 40);

    CHECK_MSG(Ubiquitty::retireTitleIfStale(term, 42), "and it retires with the shell's turn at the terminal");
}

TEST(testATitleSetBeforeAnyProbeIsAdoptedThenRetired)
{
    Ubiquitty::Terminal term;
    // No probe has run, so the setter has no group to stamp.
    Ubiquitty::setTitleUTF8String(term, "set before any probe");
    CHECK(term.titleOwnerProcessGroup == 0);

    CHECK_MSG(!Ubiquitty::retireTitleIfStale(term, 800), "the first observed program adopts it");
    CHECK(term.titleOwnerProcessGroup == 800);
    CHECK(term.titleUTF8 == "set before any probe");

    CHECK(Ubiquitty::retireTitleIfStale(term, 801));
    CHECK(term.titleUTF8.empty());
}

// A window with no title has no owner to remember, and retiring nothing is not
// a change the frontend should be told about.
TEST(testAnEmptyTitleRetiresNothing)
{
    Ubiquitty::Terminal term;
    term.titleOwnerProcessGroup = 42;
    CHECK(!Ubiquitty::retireTitleIfStale(term, 43));
    CHECK(term.titleOwnerProcessGroup == 0);
}

// The host an OSC 7 report named belongs to the shell that sent it, as its
// directory does. It outranks the walk when a session's location is resolved,
// so one that outlived its shell would name the machine the reader left rather
// than the one an ssh with no integration took them to.
TEST(testAReportedHostRetiresWithTheProgramThatReportedIt)
{
    Ubiquitty::Terminal term;
    CHECK(Ubiquitty::setReportedHostString(term, "example-host"));
    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(term, 500), "the probe after the report stamps the program holding the terminal");
    CHECK(term.reportedHostProcessGroup == 500);

    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(term, 500), "its program still holds the terminal");
    CHECK(term.reportedHost == "example-host");
    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(term, 0), "an unsampled foreground retires nothing");
    CHECK(term.reportedHost == "example-host");

    CHECK_MSG(Ubiquitty::retireReportedHostIfStale(term, 501), "its program left the foreground");
    CHECK_MSG(term.reportedHost.empty(), "so the walk answers where the session is");
    CHECK(term.reportedHostProcessGroup == 0);

    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(term, 502), "and retiring nothing moves nothing");
}

TEST(testAHostReportedBeforeAnyProbeIsAdoptedThenRetired)
{
    Ubiquitty::Terminal term;
    // No probe has run, so the setter has no group to stamp.
    CHECK(Ubiquitty::setReportedHostString(term, "example-host"));
    CHECK(term.reportedHostProcessGroup == 0);

    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(term, 800), "the first observed program adopts it");
    CHECK(term.reportedHostProcessGroup == 800);
    CHECK(term.reportedHost == "example-host");

    CHECK(Ubiquitty::retireReportedHostIfStale(term, 801));
    CHECK(term.reportedHost.empty());
}

// No host has no reporter to remember, and retiring nothing is not a change the
// frontend should be told about.
TEST(testAnEmptyReportedHostRetiresNothing)
{
    Ubiquitty::Terminal term;
    term.reportedHostProcessGroup = 42;
    CHECK(!Ubiquitty::retireReportedHostIfStale(term, 43));
    CHECK_MSG(term.reportedHostProcessGroup == 0, "a stamp names the program a host belongs to, and there is no host");
}

// The stamp names the program a host belongs to, and the probe after a report
// takes it. A program repeating a host comes to own it even though the host
// does not change, and a report naming no host clears the one standing and so
// leaves no program to stamp.
TEST(aRepeatedHostIsRestampedAndNoHostLeavesNoStamp)
{
    Ubiquitty::Terminal term;
    CHECK(Ubiquitty::setReportedHostString(term, "example-host"));
    CHECK(!Ubiquitty::retireReportedHostIfStale(term, 500));
    CHECK_MSG(!Ubiquitty::setReportedHostString(term, "example-host"), "a repeated host is not a change");
    CHECK_MSG(term.reportedHostProcessGroup == 0, "but it waits for the probe to name who repeated it");
    CHECK(!Ubiquitty::retireReportedHostIfStale(term, 600));
    CHECK_MSG(term.reportedHostProcessGroup == 600, "and the program repeating it owns it");

    CHECK_MSG(Ubiquitty::setReportedHostString(term, ""), "a report naming no host clears the one standing");
    CHECK(term.reportedHost.empty());
    CHECK_MSG(term.reportedHostProcessGroup == 0, "and stamps no program, since no host belongs to one");
    CHECK(!Ubiquitty::retireReportedHostIfStale(term, 700));
    CHECK_MSG(term.reportedHostProcessGroup == 0, "nor does a probe adopt a host that is not there");
}

// A drain that reported something while the full probe was throttled names
// the owner with one foreground read. Only what is unstamped is stamped, and
// only a drain whose latches say it reported anything asks.
TEST(aThrottledDrainStampsOnlyTheReportsItLeftUnstamped)
{
    Ubiquitty::Terminal term;
    CHECK_MSG(!Ubiquitty::didDrainLeaveAReportUnstamped(term), "a drain that reported nothing reads no foreground");

    Ubiquitty::setTitleUTF8String(term, "set on an earlier drain");
    Ubiquitty::setReportedDirectoryString(term, "/srv/app");
    Ubiquitty::setReportedHostString(term, "example-host");
    CHECK_MSG(!Ubiquitty::didDrainLeaveAReportUnstamped(term), "reports from an earlier drain wait for the full probe");
    term.currentDirectoryChanged = true;
    CHECK_MSG(Ubiquitty::didDrainLeaveAReportUnstamped(term), "a directory or host reported on this drain asks");
    term.currentDirectoryChanged = false;
    term.titleChanged = true;
    CHECK_MSG(Ubiquitty::didDrainLeaveAReportUnstamped(term), "and so does a title set on it");
    term.currentDirectoryChanged = true;

    Ubiquitty::stampUnstampedReports(term, 0);
    CHECK_MSG(term.titleOwnerProcessGroup == 0 && term.reportingProcessGroup == 0 && term.reportedHostProcessGroup == 0,
        "an unknown foreground stamps nothing");

    term.reportingProcessGroup = 90; // stamped by an earlier probe
    Ubiquitty::stampUnstampedReports(term, 77);
    CHECK(term.titleOwnerProcessGroup == 77);
    CHECK_MSG(term.reportingProcessGroup == 90, "a stamped report is left for the full probe to judge");
    CHECK(term.reportedHostProcessGroup == 77);
    CHECK_MSG(!Ubiquitty::didDrainLeaveAReportUnstamped(term), "and nothing is left waiting");
}

UBIQUITTY_TEST_MAIN()
