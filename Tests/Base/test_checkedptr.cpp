// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_checkedptr.cpp — the referent count survives copying, moving and
// self-assignment.
//
// `CanMakeCheckedPtr` counts the `CheckedPtr`s aimed at it and asserts none
// remain when it is destroyed, so the count is what turns a dangling pointer
// into a debug trap. Every operation that could get the count wrong is one a
// compiler would generate correctly for an ordinary member — which is why they
// are written out by hand here, and why nothing else exercises them.
//
// The copy and move constructors do nothing, and that is the contract: a copy is
// a different object with zero referents. A defaulted copy would carry the
// source's count into an object nothing points at, and the destructor would trap
// on a program doing nothing wrong.

#include "Base/CheckedPtr.h"

#include <cassert>
#include <csignal>

#if UBIQUITTY_COVERAGE_INSTRUMENTED
extern "C" int __llvm_profile_write_file(void);
#endif
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using Ubiquitty::CanMakeCheckedPtr;
using Ubiquitty::CheckedPtr;

namespace {

struct Target : CanMakeCheckedPtr {
    int value { 0 };
};

} // namespace

TEST(aCopiedTargetStartsWithNoReferents)
{
    Target original;
    original.value = 7;
    {
        CheckedPtr<Target> pointer(&original);
        CHECK(pointer.get() == &original);

        // The copy is a distinct object nothing points at. If it inherited the
        // source's count it would trap at the closing brace below, on a program
        // that is doing nothing wrong.
        Target copied(original);
        CHECK(copied.value == 7);
    }

    // And the same for a move: the source keeps its referent, the destination
    // takes none.
    {
        CheckedPtr<Target> pointer(&original);
        Target moved(std::move(original));
        CHECK(pointer.get() == &original); // the pointer still names the source
    }
}

TEST(anAssignedTargetKeepsItsOwnReferentCount)
{
    Target left;
    Target right;
    left.value = 1;
    right.value = 2;

    CheckedPtr<Target> pointerToLeft(&left);
    CHECK(pointerToLeft.get() == &left);

    // Assignment copies the value and not the bookkeeping. There is no accessor
    // for the count — it is `private` and read only by the destructor's
    // assertion — so the observable is that this scope ends without trapping in
    // a debug build. An assignment that copied the count would leave `right`
    // believing it has a referent, and its destructor would fire.
    left = right;
    right = left;

    Target third;
    third = std::move(right);
    CHECK(pointerToLeft.get() == &left); // the one real referent is untouched
}

TEST(assigningAPointerWhatItAlreadyHoldsChangesNothing)
{
    Target target;
    CheckedPtr<Target> pointer(&target);

    // `reset` is private; `operator=` is the public route to it. What this
    // asserts is that self-assignment leaves the pointer and the count where
    // they were — not that the early return inside `reset` is doing it.
    //
    // Deleting that return leaves every case here passing, measured rather than
    // assumed: the count goes down and straight back up, the decrement's own
    // assertion holds at every step because it only forbids going below zero,
    // and no window between them is observable from outside. The guard saves
    // two operations; it does not change an answer, and no case can show that
    // it does. Stated here so the next reader does not spend the time twice.
    CheckedPtr<Target> second(&target);
    pointer = &target;
    CHECK(pointer.get() == &target);

    pointer = &target;
    pointer = &target;
    CHECK(pointer.get() == &target);
    CHECK(second.get() == &target);

    // Assigning something else takes the other path, so the case above is about
    // the guard rather than about assignment never doing anything.
    Target other;
    pointer = &other;
    CHECK(pointer.get() == &other);

    // And back to nothing, which is what leaves both targets destructible.
    pointer = nullptr;
    second = nullptr;
    CHECK(pointer.get() == nullptr);
}

namespace {

/// Runs `body` in a child process and reports whether it aborted.
///
/// The assertions in this type are its entire purpose, and an assertion that
/// never fires is indistinguishable from one compiled out or written with a
/// condition that cannot be false. Taking those arms means the process ends, so
/// the only way to assert on them is to spend a process doing it.
///
/// The child's stderr goes to /dev/null: a passing run of this file would
/// otherwise print two assertion messages that read as failures.
///
/// - Parameter body: What to run in the child.
/// - Returns: True when the child died on SIGABRT, false on any other outcome
///   including a clean exit.
// A child that dies on the assertion never reaches the exit hook that writes its
// counters, so without a flush the arm a death test exercises is executed and
// then reported as unexecuted — the case costs coverage rather than adding it.

/// Writes the counters this process accumulated, in an instrumented build.
///
/// Guarded by the build rather than by a weak symbol or a `dlsym`: on Mach-O a
/// weak declaration of an absent symbol is still a link error, and a statically
/// linked profile runtime is not in the dynamic symbol table for `dlsym` to
/// find. Both were tried.
void flushCoverage()
{
#if UBIQUITTY_COVERAGE_INSTRUMENTED
    __llvm_profile_write_file();
#endif
}

template<typename Body>
bool abortsInAChild(Body body)
{
    const pid_t child = ::fork();
    if (child == 0) {
        // The core this child leaves records an abort the case asked for, so
        // nothing reads it, and a runner running systemd-coredump captures,
        // compresses and stores one per run -- journal noise and disk on a box
        // every job shares. Dropped before anything else, so every path here
        // that ends in a trap is covered; a genuine crash elsewhere still dumps.
        const ::rlimit noCore { 0, 0 };
        static_cast<void>(::setrlimit(RLIMIT_CORE, &noCore));
        // The harness installs a SIGABRT handler so a crashing case names
        // itself, and a forked child inherits it — the assertion below would
        // then leave through that handler's `_Exit` and the parent would see an
        // ordinary exit status rather than a signal. Restoring the default is
        // what makes the trap observable as one.
        // Write the counters, then let the abort through as a signal so the
        // parent sees a trap rather than an exit code.
        std::signal(SIGABRT, [](int) {
            flushCoverage();
            std::signal(SIGABRT, SIG_DFL);
            ::raise(SIGABRT);
        });
        const int quiet = ::open("/dev/null", O_WRONLY);
        if (quiet >= 0) {
            ::dup2(quiet, STDERR_FILENO);
            ::close(quiet);
        }
        body();
        flushCoverage();
        ::_exit(0); // reached only when the assertion did not fire
    }
    if (child < 0)
        return false;
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace

TEST(aTargetDestroyedWhileStillReferencedTraps)
{
    // Taken in every configuration, because the trap is. `CanMakeCheckedPtr`
    // ends the process with `std::abort` rather than an assertion, so a build
    // that skipped this case would be skipping the one where the ordering fault
    // it catches actually happens.
    //
    // The trap this type exists for. Without it a `CheckedPtr` outliving its
    // target is a dangling pointer nothing reports.
    CHECK(abortsInAChild([] {
        CheckedPtr<Target> pointer;
        {
            Target target;
            pointer = &target;
        } // target dies here with one referent outstanding
    }));

    // And the control: the same shape with the pointer released first ends
    // cleanly, so the case above traps on the outstanding referent rather than
    // on anything else in the scope.
    CHECK(!abortsInAChild([] {
        CheckedPtr<Target> pointer;
        {
            Target target;
            pointer = &target;
            pointer = nullptr;
        }
    }));
}

TEST(releasingAReferenceThatWasNeverTakenTraps)
{
    // The other assertion: the count must not go below zero. A decrement with no
    // matching increment is what an unbalanced `CheckedPtr` would do, and the
    // guard is what turns it into a trap instead of a wrap to four billion.
    CHECK(abortsInAChild([] {
        Target target;
        target.decrementCheckedPtrCount();
    }));

    // Balanced, so the control ends cleanly.
    CHECK(!abortsInAChild([] {
        Target target;
        target.incrementCheckedPtrCount();
        target.decrementCheckedPtrCount();
    }));
}

UBIQUITTY_TEST_MAIN()
