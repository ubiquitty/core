// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The live configuration singleton is one object, read and replaced from the
// main thread only.
//
// A configure writes over the storage every reference from `values()`
// aliases; that aliasing is the design (a reload reaches every holder), and it
// is race-free only because no other thread reads the storage. A debug build
// asserts the thread at both entry points, and these cases pin that the
// assertion is there: a violation aborts the process rather than reading a
// half-written string.

#include "Harness.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"
#include "PAL/PAL.h"

#include <thread>

#if !defined(NDEBUG) && !defined(__EMSCRIPTEN__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Ubiquitty::Preferences;

TEST(mainThreadIsRecognised)
{
    CHECK(Ubiquitty::PAL::isMainThread());
    bool isMainElsewhere = true;
    std::thread([&isMainElsewhere] { isMainElsewhere = Ubiquitty::PAL::isMainThread(); }).join();
    CHECK(!isMainElsewhere);
}

TEST(replacementIsSeenThroughAReferenceTakenBeforeIt)
{
    // The property every frontend's cached reference relies on -- "every
    // holder of that reference sees the new value without being told" --
    // asserted rather than assumed.
    const Values& held = values();
    const std::string previous = liveDocument();
    (void)configure("[term]\nversion = \"main-thread-guard\"\n", ParseMode::Strict);
    CHECK(held.termProgramVersion.has_value());
    CHECK(*held.termProgramVersion == "main-thread-guard");
    CHECK(&values() == &held);
    (void)configure(previous, ParseMode::Permissive); // leave the process as it was found
}

#if !defined(NDEBUG) && !defined(__EMSCRIPTEN__)

namespace {

// Runs `body` on a non-main thread of a forked child and reports whether the
// child died by the assertion's abort. The fork keeps the abort out of this
// process; the thread is what the assertion is about.
bool abortsOffMain(void (*body)())
{
    const pid_t child = ::fork();
    if (child < 0)
        return false;
    if (child == 0) {
        // The harness catches SIGABRT to report a case; the child is judged
        // by the signal itself.
        ::signal(SIGABRT, SIG_DFL);
        std::thread(body).join();
        ::_exit(0); // reached only when the assertion did not fire
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child)
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace

TEST(readingOffMainAborts)
{
    CHECK(abortsOffMain([] { (void)values().termIdentifier; }));
}

TEST(replacingOffMainAborts)
{
    CHECK(abortsOffMain([] { (void)configure("", ParseMode::Permissive); }));
}

#endif

UBIQUITTY_TEST_MAIN()
