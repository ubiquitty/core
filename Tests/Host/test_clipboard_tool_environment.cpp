// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_clipboard_tool_environment.cpp — the clipboard helpers a
// session's PATH names live beside an absolute socket path, and a relative one
// is refused (Platform/Host/ClipboardTool.cpp, clipboardToolEnvironment).
//
// A relative PATH entry resolves against the shell's current directory, and the
// helper directory a relative socket names is created in the daemon's.
//

#include "Harness.h"

#include "Platform/Host/ClipboardTool.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

/// A scratch directory removed with everything under it.
struct ScratchDirectory {
    ScratchDirectory()
    {
        std::string pattern = "/tmp/ubiquitty-clipboard-tool.XXXXXX";
        if (::mkdtemp(pattern.data()) != nullptr)
            path = std::filesystem::path(pattern);
    }
    ~ScratchDirectory()
    {
        std::error_code error;
        if (!path.empty())
            std::filesystem::remove_all(path, error);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    std::filesystem::path path;
};

bool hasEntryStartingWith(const std::vector<std::string>& environment, const std::string& prefix)
{
    return std::ranges::any_of(environment, [&](const std::string& entry) { return entry.starts_with(prefix); });
}

}

TEST(aRelativeSocketPathIsRefusedAndCreatesNothing)
{
    const ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    std::error_code error;
    const std::filesystem::path previous = std::filesystem::current_path(error);
    REQUIRE(!error);
    std::filesystem::current_path(scratch.path, error);
    REQUIRE(!error);

    const std::vector<std::string> environment = Ubiquitty::Host::clipboardToolEnvironment("relative.sock", "/bin/sh");
    const bool didLeaveHelpers = std::filesystem::exists(scratch.path / "clipboard-tools", error);
    std::filesystem::current_path(previous, error);

    CHECK_MSG(environment.empty(), "a relative socket path yields no helper environment");
    CHECK_MSG(!didLeaveHelpers, "no helper directory is created in the current directory");
}

TEST(anAbsoluteSocketPathInstallsHelpersBesideIt)
{
    const ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const std::filesystem::path socketPath = scratch.path / "host.sock";
    const std::filesystem::path helpers = scratch.path / "clipboard-tools" / "host.sock";

    const std::vector<std::string> environment = Ubiquitty::Host::clipboardToolEnvironment(socketPath.string(), "/bin/sh");
    CHECK(hasEntryStartingWith(environment, "PATH=" + helpers.string() + ":"));
    CHECK(hasEntryStartingWith(environment, "UBIQUITTY_CLIPBOARD_SOCKET=" + socketPath.string()));
    std::error_code error;
    CHECK(std::filesystem::is_symlink(helpers / "pbpaste", error));
}

UBIQUITTY_TEST_MAIN()
