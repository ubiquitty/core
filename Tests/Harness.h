// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Tests/Harness.h — all of the test framework, in one header.
//
// No library to link, no dependency to install: a Ubiquitty test is a .cpp that
// this header turns into a program. It supplies the assertion vocabulary and,
// through TEST, a registry so each case is addressable by name.
//
// A test file is laid out like this:
//
//     #include "Harness.h"                   // TEST, the CHECK family, the runner
//     #include "Core/TerminalOperations.h"   // whatever the cases exercise
//
//     TEST(arithmeticStillWorks) { CHECK_EQ(2 + 2, 4); }
//
//     UBIQUITTY_TEST_MAIN()
//
// The runner that UBIQUITTY_TEST_MAIN defines takes:
//
//     --list              print every case name, one per line
//     --shard I/N         run only the cases where index % N == I
//     <substring>         run only the cases whose name contains it
//
// Which buys three things a hand-written main() cannot. The executable's
// substring argument reaches one case, while CTest keeps binary/shard scheduling.
// A binary large enough to set the suite's wall clock shards without its author
// partitioning anything. And a case that crashes names itself: the runner catches
// the fatal signal and prints the case it was in, so a segfault cannot take the
// summary line with it and leave no indication of which of several hundred cases
// died.
//
// The older shape — plain functions called from a hand-written main() returning
// UBIQUITTY_TEST_SUMMARY() — still works, and most of the suite still uses it.
// Both compile into the same binary, so a file migrates when someone touches
// it, not in one sweep.
//
// Prefer CHECK_EQ and its siblings to a bare CHECK. CHECK can print only the
// expression it was handed, so a failed `CHECK(cellAt(t, 0, 0)->codePoint == 'x')`
// reports that the comparison was false and never what the code point was. The
// comparison macros print both operands, and to someone reading a failure in a
// test they did not write, that is most of what makes it debuggable.
//
// The assertion vocabulary lives in Assertions.h, which this includes. A test
// that must own its own main() includes that directly and returns
// UBIQUITTY_TEST_SUMMARY() — same macros, same counter, no second copy.
//
// A case that cannot do its work in this environment calls
// UbiquittyTest::skip(reason) and returns. The summary counts it, and a binary
// whose every case skipped exits 77, which CTest reports as Skipped rather
// than Passed.
//

#include "Assertions.h"

#include <atomic>
#include <chrono>

// The boundary tests link no Core and set UBIQUITTY_TEST_WITHOUT_CORE, so the
// Preferences pin below both compiles and links only where Core is present.
#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Case registry
// ---------------------------------------------------------------------------

namespace UbiquittyTest {

/// One registered case: its name, its body, and the next in declaration order.
struct Case {
    const char* name;
    void (*body)();
    Case* next;
};

/// The head and tail of the registration list.
///
/// Function-local statics rather than namespace-scope ones: a registrar runs
/// during static initialization, and a function-local static is constructed on
/// first use, so registration cannot depend on initialization order.
inline Case*& caseHead()
{
    static Case* head = nullptr;
    return head;
}

inline Case*& caseTail()
{
    static Case* tail = nullptr;
    return tail;
}

/// Appends a case at construction. Appending rather than prepending keeps cases
/// in source order, which some files rely on.
struct Registrar {
    Case node;
    Registrar(const char* name, void (*body)())
        : node { name, body, nullptr }
    {
        if (caseTail())
            caseTail()->next = &node;
        else
            caseHead() = &node;
        caseTail() = &node;
    }
};

/// Owns the lock-free case pointer read by the fatal-signal handler.
class RunningCaseState {
public:
    /// Returns the process-wide running-case state.
    ///
    /// - Returns: The state shared with the fatal-signal handler.
    static RunningCaseState& singleton()
    {
        static RunningCaseState state;
        return state;
    }

    /// Sets the case name visible to the fatal-signal handler.
    ///
    /// - Parameter name: The active case name, or `nullptr` between cases.
    void set(const char* name) { m_name.store(name, std::memory_order_relaxed); }

    /// Returns the case name visible to the fatal-signal handler.
    ///
    /// - Returns: The active case name, or `nullptr` between cases.
    const char* get() const { return m_name.load(std::memory_order_relaxed); }

private:
    std::atomic<const char*> m_name { nullptr };
};

static_assert(std::atomic<const char*>::is_always_lock_free);

/// The exit status of a binary whose every case that ran was skipped.
///
/// CTest reads it as Skipped through the `SKIP_RETURN_CODE` every registered
/// binary carries, so a binary that asserted nothing does not count as passed.
inline constexpr int skippedExitCode = 77;

/// Whether the running case has declined any of its work.
class SkipState {
public:
    /// Returns the process-wide skip state.
    ///
    /// - Returns: The state the runner reads after each case.
    static SkipState& singleton()
    {
        static SkipState state;
        return state;
    }

    /// Records that the running case skipped.
    void noteSkip() { m_isRunningCaseSkipped = true; }

    /// Reports whether the case that just ran skipped, and clears the record.
    ///
    /// - Returns: Whether `noteSkip` was called since the last call.
    bool takeSkip()
    {
        const bool wasSkipped = m_isRunningCaseSkipped;
        m_isRunningCaseSkipped = false;
        return wasSkipped;
    }

private:
    bool m_isRunningCaseSkipped = false;
};

/// Records that the running case cannot do its work here, and says why.
///
/// The case is counted as skipped rather than passed, and the summary gives
/// the count. A case usually returns straight after; one that goes on to check
/// what it can is still counted as skipped, since part of what it names did
/// not run. Reserve it for a real difference in the environment — running as
/// root, a resource limit the process is exempt from, a missing tool. A
/// fixture that could not be built is a failure: a broken temporary directory
/// must not turn a check green.
///
/// - Parameter reason: What the environment lacks, printed beside the case.
inline void skip(const char* reason)
{
    const char* name = RunningCaseState::singleton().get();
    std::printf("test-case: SKIPPED — %s: %s\n", name ? name : "(outside a case)", reason);
    SkipState::singleton().noteSkip();
}

/// Reports whether this environment promises a tool, so its absence fails.
///
/// The C++ side of `ubq_tool_is_required` in Scripts/run-tests.sh: an
/// environment built to carry a tool names it in `UBQ_REQUIRED_TOOLS`, and a
/// case that needs it fails there rather than skipping.
///
/// - Parameter tool: The tool's name, as `UBQ_REQUIRED_TOOLS` spells it.
/// - Returns: Whether `UBQ_REQUIRED_TOOLS` names it as a whole word.
inline bool isRequiredTool(const char* tool)
{
    const char* listed = std::getenv("UBQ_REQUIRED_TOOLS");
    if (!listed || !tool || !*tool)
        return false;
    const std::size_t length = std::strlen(tool);
    for (const char* at = listed; (at = std::strstr(at, tool)) != nullptr; at += length) {
        const bool isStart = at == listed || at[-1] == ' ';
        const bool isEnd = at[length] == '\0' || at[length] == ' ';
        if (isStart && isEnd)
            return true;
    }
    return false;
}

/// Names the case that died, then leaves without running any handler that could
/// fault again. Async-signal-safe: write(2) and _Exit only, no printf, no malloc.
inline void reportFatalSignal(int signalNumber)
{
    const char* name = RunningCaseState::singleton().get();
    const char prefix[] = "\ntest-harness: FATAL — signal in case ";
    ssize_t ignored = ::write(2, prefix, sizeof(prefix) - 1);
    if (name)
        ignored = ::write(2, name, std::strlen(name));
    ignored = ::write(2, "\n", 1);
    (void)ignored;
    (void)signalNumber;
    std::_Exit(2);
}

/// Runs the registered cases. See the header comment for the argument forms.
inline int runRegisteredCases(int argc, char** argv)
{
    const char* filter = nullptr;
    int shardIndex = 0;
    int shardCount = 1;
    bool listOnly = false;
    bool isVerbose = std::getenv("UBQ_TEST_VERBOSE") != nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            listOnly = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            isVerbose = true;
        } else if (std::strcmp(argv[i], "--shard") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d/%d", &shardIndex, &shardCount) != 2
                || shardCount < 1 || shardIndex < 0 || shardIndex >= shardCount) {
                std::fprintf(stderr, "bad --shard argument (want I/N with 0 <= I < N)\n");
                return 2;
            }
        } else {
            filter = argv[i];
        }
    }

    if (listOnly) {
        for (const Case* c = caseHead(); c; c = c->next)
            std::printf("%s\n", c->name);
        return 0;
    }

    auto& runningCase = RunningCaseState::singleton();
    std::signal(SIGSEGV, reportFatalSignal);
    std::signal(SIGBUS, reportFatalSignal);
    std::signal(SIGABRT, reportFatalSignal);

    auto& skipState = SkipState::singleton();
    int index = 0;
    int ran = 0;
    int skipped = 0;
    const auto suiteStarted = std::chrono::steady_clock::now();
    for (const Case* c = caseHead(); c; c = c->next, ++index) {
        if (index % shardCount != shardIndex)
            continue;
        if (filter && !std::strstr(c->name, filter))
            continue;
        const int before = failures;
        runningCase.set(c->name);
        if (isVerbose) {
            std::printf("test-case: start — %s\n", c->name);
        }
        const auto caseStarted = std::chrono::steady_clock::now();
        c->body();
        runningCase.set(nullptr);
        ++ran;
        const bool didSkip = skipState.takeSkip();
        if (didSkip)
            ++skipped;
        const double caseSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - caseStarted)
                                       .count();
        if (failures != before) {
            std::printf("test-case: FAILED — %s (%.4fs; %d assertion%s)\n",
                c->name, caseSeconds, failures - before,
                failures - before == 1 ? "" : "s");
        } else if (isVerbose && !didSkip) {
            std::printf("test-case: ok — %s (%.4fs)\n", c->name, caseSeconds);
        }
    }

    // A binary that registered no case at all is a defect, not a pass: it
    // compiled and linked, but every TEST() is gone — a lost merge, an #ifdef
    // that resolved false — and "0 cases" through ctest reads exactly like a
    // suite that ran and passed. `index` counted every registered case, shard
    // and filter aside, so zero means the registry is empty.
    if (index == 0) {
        std::fprintf(stderr, "no TEST() case is registered in this binary — "
                             "0 cases is not a pass\n");
        return 2;
    }

    // A filter that selected nothing is a typo, not a pass. A shard that draws
    // no case is legitimate, so only an explicit filter is held to this.
    if (filter && ran == 0) {
        std::fprintf(stderr, "no case matched \"%s\"\n", filter);
        return 2;
    }

    const double suiteSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - suiteStarted)
                                    .count();
    if (failures) {
        std::printf("test-harness: FAILED — %d assertion%s across %d case%s (%.4fs)\n",
            failures, failures == 1 ? "" : "s", ran, ran == 1 ? "" : "s",
            suiteSeconds);
        return 1;
    }
    // An empty shard ran nothing and skipped nothing; it passes, as above.
    if (ran > 0 && skipped == ran) {
        std::printf("test-harness: SKIPPED — every one of %d case%s skipped (%.4fs)\n",
            ran, ran == 1 ? "" : "s", suiteSeconds);
        return skippedExitCode;
    }
    if (skipped) {
        std::printf("test-harness: ok (%d case%s; %.4fs; %d skipped)\n",
            ran, ran == 1 ? "" : "s", suiteSeconds, skipped);
    } else {
        std::printf("test-harness: ok (%d case%s; %.4fs)\n",
            ran, ran == 1 ? "" : "s", suiteSeconds);
    }
    return 0;
}

} // namespace UbiquittyTest

/// Defines a registered case. The body follows the macro like a function body.
#define TEST(name)                                                            \
    static void name();                                                       \
    static const ::UbiquittyTest::Registrar ubqRegistrar_##name(#name, name); \
    static void name()

// Host tests assert against Core's compiled defaults, never the machine's
// config.toml; the pin replaces the lazy-loaded singleton before any case reads
// it.
//
// Empty where Preferences is not linked. That is the condition, not "no Core":
// a stanza compiling two or three Core files against a narrow link set has Core
// in it and no Preferences, and the pin is then an undefined symbol at link
// rather than a default at run. Such a test cannot read config.toml either --
// nothing it links looks at it -- so the pin has nothing to do there.
#ifndef UBIQUITTY_TEST_WITHOUT_CORE
namespace UbiquittyTest {

/// Points the configuration home at an empty directory of the process's own.
///
/// The pin's first read of the singleton loads it, and the load reads the
/// stored config.toml before the pin replaces what it parsed; so does anything
/// a case starts. With the home moved first, the machine's file is never
/// opened, and neither its keys nor its warnings reach a run. The directory
/// goes at exit, with whatever the cases left in it, and only from the process
/// that made it: a forked child that exits must not take it from its parent.
inline void isolateConfigurationHome()
{
    std::error_code error;
    std::string pattern = (std::filesystem::temp_directory_path(error) / "ubq-test-config.XXXXXX").string();
    if (error || !::mkdtemp(pattern.data()))
        return;
    ::setenv("XDG_CONFIG_HOME", pattern.c_str(), 1);
    static const std::string directory = pattern;
    static const pid_t owner = ::getpid();
    std::atexit([] {
        if (::getpid() != owner)
            return;
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    });
}

} // namespace UbiquittyTest

#define UBIQUITTY_TEST_PIN_PREFERENCES()          \
    (::UbiquittyTest::isolateConfigurationHome(), \
        (void)::Ubiquitty::Preferences::configure(std::string(), ::Ubiquitty::Preferences::ParseMode::Permissive))
#else
#define UBIQUITTY_TEST_PIN_PREFERENCES() ((void)0)
#endif

/// Defines the entry point that runs every case registered by TEST.
#define UBIQUITTY_TEST_MAIN()                                   \
    int main(int argc, char** argv)                             \
    {                                                           \
        UBIQUITTY_TEST_PIN_PREFERENCES();                       \
        return ::UbiquittyTest::runRegisteredCases(argc, argv); \
    }
