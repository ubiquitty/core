// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Preferences/PreferencesPaths.h"
#include "PAL/PAL.h"

#include <array>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

/// Saves one environment variable and puts it back on scope exit, keeping
/// unset and empty apart: the resolver treats the two differently, so a
/// restore that collapsed them would leave the next test reading a lie.
class EnvironmentGuard {
public:
    explicit EnvironmentGuard(const char* name)
        : m_name(name)
    {
        if (const char* value = std::getenv(name))
            m_saved = value;
    }
    ~EnvironmentGuard()
    {
        if (m_saved)
            ::setenv(m_name, m_saved->c_str(), 1);
        else
            ::unsetenv(m_name);
    }
    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_saved;
};

} // namespace

TEST(testDirectoryRespectsXDGThenHome)
{
    EnvironmentGuard xdg("XDG_CONFIG_HOME");
    EnvironmentGuard home("HOME");

    ::setenv("XDG_CONFIG_HOME", "/tmp/test-xdg", 1);
    ::setenv("HOME", "/tmp/test-home", 1);
    auto directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/tmp/test-xdg/ubiquitty", "XDG takes precedence");
    auto file = Ubiquitty::Preferences::filePath();
    CHECK_MSG(file && *file == "/tmp/test-xdg/ubiquitty/config.toml", "file under XDG");

    ::setenv("XDG_CONFIG_HOME", "", 1);
    directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/tmp/test-home/.config/ubiquitty", "empty XDG falls back to HOME");

    ::setenv("XDG_CONFIG_HOME", "relative/xdg", 1);
    directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/tmp/test-home/.config/ubiquitty", "a relative XDG is ignored");

    ::setenv("XDG_CONFIG_HOME", "/tmp/test-xdg///", 1);
    directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/tmp/test-xdg/ubiquitty", "trailing slashes trimmed");

    ::setenv("XDG_CONFIG_HOME", "/", 1);
    directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/ubiquitty", "a root XDG keeps its one slash");

    ::unsetenv("XDG_CONFIG_HOME");
    ::setenv("HOME", "/tmp/test-home/", 1);
    directory = Ubiquitty::Preferences::directoryPath();
    CHECK_MSG(directory && *directory == "/tmp/test-home/.config/ubiquitty", "HOME trailing slash trimmed");
}

TEST(testDirectoryFollowsThePALHomeWhenTheEnvironmentIsBare)
{
    EnvironmentGuard xdg("XDG_CONFIG_HOME");
    EnvironmentGuard home("HOME");
    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("HOME");

    // The oracle is the PAL resolver itself, read under the same environment:
    // whatever home it finds (the account database, here) is the one the
    // preferences directory must sit under, and when it finds none there is no
    // directory at all. Asserting against it rather than against a literal
    // keeps the test true on a runner whose account has no home.
    std::array<char, 1024> buffer {};
    const int length = Ubiquitty::PAL::systemHomeDirectory(buffer.data(), static_cast<int>(buffer.size()));
    const auto directory = Ubiquitty::Preferences::directoryPath();
    if (length <= 0) {
        CHECK_MSG(!directory, "no home, no directory");
        CHECK_MSG(!Ubiquitty::Preferences::filePath(), "no home, no file");
        return;
    }
    std::string expected(buffer.data());
    while (expected.size() > 1 && expected.back() == '/')
        expected.pop_back();
    expected += "/.config/ubiquitty";
    CHECK_MSG(directory && *directory == expected, "the account-database home is the fallback");
}

UBIQUITTY_TEST_MAIN()
