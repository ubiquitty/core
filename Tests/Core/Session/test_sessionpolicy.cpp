// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sessionpolicy.cpp — the close-confirmation predicate
// (Session/SessionPolicy.h::needsCloseConfirmation) the per-window close and
// app-quit paths ask. Pins the full truth table over the three ProcessFacts
// inputs:
//
//   streamPresent  hasRunningChild  foregroundIsShell  ->  confirm?
//   -------------  ---------------  -----------------      --------
//   no             *                *                  ->  no  (no process to kill)
//   yes            no               *                  ->  no  (pristine prompt)
//   yes            yes              yes                ->  no  (shell-foreground output)
//   yes            yes              no                 ->  yes (a real foreground job)
//
// Header-only policy, links standalone.

#include "Core/Session/SessionPolicy.h"

#include <array>
#include <cstdio>

using Ubiquitty::Session::needsCloseConfirmation;
using Ubiquitty::Session::ProcessFacts;

static bool confirm(bool streamPresent, bool hasRunningChild, bool foregroundIsShell)
{
    ProcessFacts facts;
    facts.streamPresent = streamPresent;
    facts.hasRunningChild = hasRunningChild;
    facts.foregroundIsShell = foregroundIsShell;
    return needsCloseConfirmation(facts);
}

TEST(sessionpolicy)
{

    // No live stream: nothing to terminate, never confirm — regardless of the
    // other two facts (a render-only / detached session owns no process).
    CHECK(confirm(/*streamPresent=*/false, /*hasRunningChild=*/false, /*foregroundIsShell=*/false) == false);
    CHECK(confirm(false, false, true) == false);
    CHECK(confirm(false, true, false) == false);
    CHECK(confirm(false, true, true) == false);

    // Live stream but no running child: a pristine shell prompt — never confirm,
    // regardless of the foreground-is-shell flag.
    CHECK(confirm(/*streamPresent=*/true, /*hasRunningChild=*/false, /*foregroundIsShell=*/false) == false);
    CHECK(confirm(true, false, true) == false);

    // Live stream WITH a running child, but the foreground is the shell (shell
    // output, e.g. a long `ls` the shell itself runs in-process / alt-screen):
    // never confirm.
    CHECK(confirm(true, /*hasRunningChild=*/true, /*foregroundIsShell=*/true) == false);

    // The one confirming case: a live stream, a running child, and the
    // foreground is a real job (not the shell). This is the only true row.
    CHECK(confirm(true, true, /*foregroundIsShell=*/false) == true);
}

// What a close confirms, once a pin is one of the facts.
TEST(closeConfirmationNamesWhatWouldBeLost)
{
    using Ubiquitty::Session::CloseConfirmation;
    using Ubiquitty::Session::closeConfirmation;
    using Ubiquitty::Session::CloseFacts;

    const auto facts = [](bool wouldKillARunningJob, bool isPinned) {
        return CloseFacts { wouldKillARunningJob, isPinned };
    };

    // Nothing running and nothing pinned: the close is not worth a question.
    CHECK(closeConfirmation(facts(false, false)) == CloseConfirmation::None);

    // The row needsCloseConfirmation already decides, now named.
    CHECK(closeConfirmation(facts(true, false)) == CloseConfirmation::RunningJob);

    // A pristine prompt loses nothing and closes without asking, so the pin is
    // the only fact here that can make it ask.
    CHECK(closeConfirmation(facts(false, true)) == CloseConfirmation::PinnedTab);

    // And it outranks the job: the reader asked for this tab to be hard to
    // close, so that is what the one prompt is about.
    CHECK_MSG(closeConfirmation(facts(true, true)) == CloseConfirmation::PinnedTab,
        "the claim the reader made themselves wins the prompt");

    // The process side arrives decided, so the two shells' different routes to
    // it meet here: whichever said "nothing would be lost" gets the same answer.
    CHECK(needsCloseConfirmation({ true, true, false }));
    CHECK(closeConfirmation(facts(needsCloseConfirmation({ true, true, false }), false)) == CloseConfirmation::RunningJob);
}

TEST(configuredCloseConfirmation)
{
    using namespace Ubiquitty;
    const ProcessFacts job { true, true, false };
    const ProcessFacts shell { true, false, true };
    CHECK(needsCloseConfirmation(shell, "always", "", {}, "zsh"));
    CHECK(!needsCloseConfirmation(job, "never", "", {}, "sleep"));
    CHECK(!needsCloseConfirmation(shell, "running", "", {}, "zsh"));
    CHECK(!needsCloseConfirmation(job, "running", "tmux, less", {}, "tmux"));
    CHECK(needsCloseConfirmation(job, "running", "tmux", {}, "tmux-helper"));
    CHECK(needsCloseConfirmation(job, "running", "tmux", {}, ""));
    CHECK(Session::isCloseIgnoredProcess("/usr/bin/less", " tmux, less\nview "));
    CHECK(!Session::isCloseIgnoredProcess("less", "lesspipe"));
    std::array<PAL::Process::Record, 2> processes;
    processes[0].name = "less";
    processes[0].isForeground = true;
    processes[1].name = "sleep";
    processes[1].isForeground = true;
    CHECK(needsCloseConfirmation(job, "running", "less", processes, "less"));
    CHECK(!needsCloseConfirmation(job, "running", "less, sleep", processes, "less"));
    CHECK(needsCloseConfirmation(job, "running", "less, sleep", processes, "vim"));
}

UBIQUITTY_TEST_MAIN()
