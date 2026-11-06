// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A target destroyed under a live CheckedPtr ends the process.
//
// The referents this counts are the frontend's clients on the terminal, and the
// ordering bug it exists to catch — a client destroyed while the terminal still
// holds it — happens on teardown paths a debug build rarely walks and a shipped
// one walks on every quit. Enforcement that compiles out under NDEBUG is
// enforcement in the configuration where the bug does not happen.
//
// An abort cannot be observed from inside the process that takes it, so each
// case runs in a forked child and the parent reads the exit status. What the
// parent proves is the pair: the violation ends its child, and the ordinary
// lifetime does not — a check that fired on everything would pass the first
// half and make the type useless.

#include "Assertions.h"
#include "Harness.h"

#include "Base/CheckedPtr.h"

#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

struct Target : Ubiquitty::CanMakeCheckedPtr {
    int value = 7;
};

// How a child ended. A child that ran to completion exits zero; one that hit
// the destructor's check dies on a signal.
enum class ChildOutcome {
    RanToCompletion,
    DiedOnSignal,
    ExitedNonZero,
};

template<typename Body> ChildOutcome runInChild(Body body)
{
    const pid_t child = ::fork();
    if (child == 0) {
        // The harness installs a handler for the fatal signals so it can name
        // the case that died, and a forked child inherits it — which would turn
        // the abort under test into an ordinary exit and hide it. The child
        // wants the default disposition, since dying is what it is here to do.
        ::signal(SIGABRT, SIG_DFL);
        // The core this child leaves records a death the case asked for, so
        // nothing reads it, and a runner running systemd-coredump captures,
        // compresses and stores one per run -- journal noise and disk on a box
        // every job shares. Dropped in the child alone, so a genuine crash
        // anywhere else still dumps.
        const ::rlimit noCore { 0, 0 };
        static_cast<void>(::setrlimit(RLIMIT_CORE, &noCore));
        body();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    if (WIFSIGNALED(status))
        return ChildOutcome::DiedOnSignal;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return ChildOutcome::RanToCompletion;
    return ChildOutcome::ExitedNonZero;
}

} // namespace

TEST(aTargetDestroyedUnderALiveCheckedPtrEndsTheProcess)
{
    const ChildOutcome outcome = runInChild([] {
        // The pointer outlives the target, which is the frontend-teardown
        // ordering this type exists to catch.
        auto* target = new Target;
        Ubiquitty::CheckedPtr<Target> pointer(target);
        delete target;
        // Read it, so a compiler that saw the pointer was never used could not
        // have removed the reference that makes this a violation.
        if (pointer && pointer->value == 7)
            ::_exit(0);
        ::_exit(0);
    });
    CHECK_MSG(outcome == ChildOutcome::DiedOnSignal,
        "a target destroyed under a live CheckedPtr did not end the process");
}

TEST(anOrdinaryLifetimeRunsToCompletion)
{
    // The other half: the check must fire on the violation and on nothing else.
    // Construction, copy, move, reassignment and reset in the ordinary order.
    const ChildOutcome outcome = runInChild([] {
        Target target;
        {
            Ubiquitty::CheckedPtr<Target> first(&target);
            Ubiquitty::CheckedPtr<Target> second(first);
            Ubiquitty::CheckedPtr<Target> third(std::move(second));
            Ubiquitty::CheckedPtr<Target> fourth;
            fourth = third;
            fourth = nullptr;
            Ubiquitty::CheckedPtr<Target> fifth(&target);
            fifth = std::move(third);
            if (!first || first->value != 7)
                ::_exit(2);
        }
        // Every referent released before the target leaves scope.
    });
    CHECK_MSG(outcome == ChildOutcome::RanToCompletion,
        "an ordinary CheckedPtr lifetime ended the process");
}

TEST(anUnbalancedReleaseEndsTheProcess)
{
    // The one way the destructor's check can be defeated: a release with no
    // matching reference wraps the count past zero, and a target with referents
    // then destroys quietly.
    const ChildOutcome outcome = runInChild([] {
        Target target;
        target.decrementCheckedPtrCount();
        ::_exit(0);
    });
    CHECK_MSG(outcome == ChildOutcome::DiedOnSignal,
        "a release with no matching reference did not end the process");
}

UBIQUITTY_TEST_MAIN()
