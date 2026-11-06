// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/PAL/test_systemhome.cpp
//
// PAL::systemHomeDirectory (Source/PAL/PAL.h, backed by
// Source/Platform/POSIX/SystemPAL.cpp).
//
// The behaviour under test guards a divergence, not a convenience: an app that
// resolves home through the account database while the daemon it spawns reads
// $HOME puts the two in different control directories whenever the two
// disagree. So there is one resolution, $HOME first — and the fallback has to be
// the account database rather than failure, or a process with no HOME loses its
// config instead of finding it.
//

#include "Harness.h"

#include "PAL/PAL.h"

#include <cstring>
#include <string>

#include <pwd.h>
#include <unistd.h>

using namespace Ubiquitty;

namespace {

// Restores whatever HOME the harness was started with, so one failing case
// cannot leak an environment change into the next.
class ScopedHome {
public:
    explicit ScopedHome(const char* value)
    {
        const char* prior = ::getenv("HOME");
        m_hadPrior = prior != nullptr;
        if (m_hadPrior)
            m_prior = prior;
        if (value)
            ::setenv("HOME", value, 1);
        else
            ::unsetenv("HOME");
    }

    ~ScopedHome()
    {
        if (m_hadPrior)
            ::setenv("HOME", m_prior.c_str(), 1);
        else
            ::unsetenv("HOME");
    }

    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    std::string m_prior;
    bool m_hadPrior = false;
};

std::string accountDatabaseHome()
{
    const struct passwd* entry = ::getpwuid(::getuid());
    return (entry && entry->pw_dir) ? entry->pw_dir : std::string();
}

// HOME must be set to a path the account database does not hold, or the case
// cannot fail: a resolver that quietly preferred pw_dir still returns the right
// string when HOME was set to the real home.
TEST(testEnvironmentHomeWinsOverTheAccountDatabase)
{
    const ScopedHome home("/tmp/ubq-home-under-test");
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    const int n = PAL::systemHomeDirectory(buffer, sizeof(buffer));
    CHECK_EQ(n, static_cast<int>(std::strlen("/tmp/ubq-home-under-test")));
    CHECK_CSTR_EQ(buffer, "/tmp/ubq-home-under-test");
    CHECK_MSG(buffer[n] == '\0', "the result is NUL-terminated at the returned length");
}

TEST(testUnsetHomeFallsBackToTheAccountDatabase)
{
    const std::string expected = accountDatabaseHome();
    CHECK_MSG(!expected.empty(), "the account database has a home to fall back to");

    const ScopedHome home(nullptr);
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    CHECK(PAL::systemHomeDirectory(buffer, sizeof(buffer)) > 0);
    CHECK_CSTR_EQ(buffer, expected.c_str());
}

TEST(testEmptyHomeIsTreatedAsUnset)
{
    const std::string expected = accountDatabaseHome();
    const ScopedHome home("");
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    CHECK(PAL::systemHomeDirectory(buffer, sizeof(buffer)) > 0);
    CHECK_MSG(expected == buffer, "an empty HOME is not a valid empty path");
}

// Truncation is the dangerous failure here: a shortened path is still a valid
// path, just a different directory, and this one names the control directory.
TEST(testCapacityIsExactAndTooSmallFailsRatherThanTruncating)
{
    const char* value = "/tmp/ubq-exact";
    const int length = static_cast<int>(std::strlen(value));
    const ScopedHome home(value);

    char exact[64];
    CHECK_EQ(PAL::systemHomeDirectory(exact, length + 1), length);
    CHECK_CSTR_EQ(exact, value);

    char shortBuffer[64];
    std::memset(shortBuffer, 'x', sizeof(shortBuffer));
    CHECK_EQ(PAL::systemHomeDirectory(shortBuffer, length), -1);
    CHECK_MSG(shortBuffer[0] == 'x', "a rejected call leaves the buffer untouched");
}

TEST(testDegenerateArgumentsFail)
{
    const ScopedHome home("/tmp/ubq-args");
    char buffer[1024];

    CHECK_EQ(PAL::systemHomeDirectory(nullptr, sizeof(buffer)), -1);
    CHECK_EQ(PAL::systemHomeDirectory(buffer, 0), -1);
    CHECK_EQ(PAL::systemHomeDirectory(buffer, -1), -1);
}

} // namespace

UBIQUITTY_TEST_MAIN()
