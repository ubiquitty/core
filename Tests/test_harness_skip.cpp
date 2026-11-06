// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_harness_skip — what a binary reports when its cases skip.
//
// A case that prints SKIP and returns exits 0 like one that passed, and CTest
// counts it green. The runner counts UbiquittyTest::skip instead, says how many
// cases skipped, and exits UbiquittyTest::skippedExitCode when every case that
// ran skipped, which CTest reads as Skipped. Each case here runs the runner
// over a list of its own in a child process, so the skipping cases it drives
// are not cases of this binary.

#include <array>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void skippingCase() { UbiquittyTest::skip("the probe has nothing to run here"); }

void passingCase() { CHECK(true); }

void failingCase() { CHECK(false); }

struct RunOutcome {
    int status = -1;
    std::string output;
};

/// Runs the harness over `bodies` in a child, as a binary holding only them.
///
/// - Parameters:
///   - bodies: The cases, registered in order.
///   - arguments: The runner's arguments after the program name, at most two.
RunOutcome runCases(std::initializer_list<void (*)()> bodies, std::initializer_list<const char*> arguments = {})
{
    std::array<int, 2> pipe {};
    if (::pipe(pipe.data()) != 0)
        return {};
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe[0]);
        ::close(pipe[1]);
        return {};
    }
    if (child == 0) {
        ::dup2(pipe[1], STDOUT_FILENO);
        ::close(pipe[0]);
        ::close(pipe[1]);
        static std::array<UbiquittyTest::Case, 4> cases {};
        static const std::array<const char*, 4> names { "first", "second", "third", "fourth" };
        UbiquittyTest::Case* previous = nullptr;
        std::size_t count = 0;
        for (void (*body)() : bodies) {
            cases[count] = { names[count], body, nullptr };
            if (previous)
                previous->next = &cases[count];
            else
                UbiquittyTest::caseHead() = &cases[count];
            previous = &cases[count];
            ++count;
        }
        UbiquittyTest::caseTail() = previous;
        failures = 0;
        std::array<char*, 4> argv { const_cast<char*>("probe"), nullptr, nullptr, nullptr };
        int argc = 1;
        for (const char* argument : arguments) {
            if (argc + 1 < static_cast<int>(argv.size()))
                argv[argc++] = const_cast<char*>(argument);
        }
        const int status = UbiquittyTest::runRegisteredCases(argc, argv.data());
        std::fflush(stdout);
        ::_exit(status);
    }
    ::close(pipe[1]);
    RunOutcome outcome;
    std::array<char, 512> buffer {};
    for (ssize_t got; (got = ::read(pipe[0], buffer.data(), buffer.size())) > 0;)
        outcome.output.append(buffer.data(), static_cast<std::size_t>(got));
    ::close(pipe[0]);
    int status = 0;
    if (::waitpid(child, &status, 0) == child && WIFEXITED(status))
        outcome.status = WEXITSTATUS(status);
    return outcome;
}

} // namespace

TEST(aBinaryWhoseEveryCaseSkippedExitsWithTheSkipStatus)
{
    const RunOutcome outcome = runCases({ skippingCase, skippingCase });
    CHECK_EQ(outcome.status, UbiquittyTest::skippedExitCode);
    CHECK_CONTAINS(outcome.output, "test-case: SKIPPED — first: the probe has nothing to run here");
    CHECK_CONTAINS(outcome.output, "test-harness: SKIPPED — every one of 2 cases skipped");
}

TEST(aBinaryWithOnePassingCaseExitsZeroAndCountsTheSkip)
{
    const RunOutcome outcome = runCases({ skippingCase, passingCase });
    CHECK_EQ(outcome.status, 0);
    CHECK_CONTAINS(outcome.output, "test-harness: ok (2 cases;");
    CHECK_CONTAINS(outcome.output, "; 1 skipped)");
}

TEST(aFailureOutranksASkip)
{
    const RunOutcome outcome = runCases({ skippingCase, failingCase });
    CHECK_EQ(outcome.status, 1);
    CHECK_CONTAINS(outcome.output, "test-harness: FAILED");
}

TEST(aShardThatDrawsNoCasePasses)
{
    // One case lands in shard 0 of 2, so shard 1 runs nothing and skips nothing.
    const RunOutcome outcome = runCases({ passingCase }, { "--shard", "1/2" });
    CHECK_EQ(outcome.status, 0);
    CHECK_CONTAINS(outcome.output, "test-harness: ok (0 cases;");
    CHECK_NOT_CONTAINS(outcome.output, "SKIPPED");
}

TEST(aBinaryWithNoSkipsReportsAsBefore)
{
    const RunOutcome outcome = runCases({ passingCase });
    CHECK_EQ(outcome.status, 0);
    CHECK_NOT_CONTAINS(outcome.output, "skipped");
}

TEST(aRequiredToolIsAWholeWordOfTheList)
{
    const char* saved = std::getenv("UBQ_REQUIRED_TOOLS");
    const std::string restore = saved ? saved : "";
    ::setenv("UBQ_REQUIRED_TOOLS", "emacs nvim lrzsz", 1);
    CHECK(UbiquittyTest::isRequiredTool("lrzsz"));
    CHECK(UbiquittyTest::isRequiredTool("emacs"));
    CHECK(!UbiquittyTest::isRequiredTool("lrz"));
    CHECK(!UbiquittyTest::isRequiredTool("nvi"));
    CHECK(!UbiquittyTest::isRequiredTool(""));
    ::unsetenv("UBQ_REQUIRED_TOOLS");
    CHECK(!UbiquittyTest::isRequiredTool("lrzsz"));
    if (saved)
        ::setenv("UBQ_REQUIRED_TOOLS", restore.c_str(), 1);
}

UBIQUITTY_TEST_MAIN()
