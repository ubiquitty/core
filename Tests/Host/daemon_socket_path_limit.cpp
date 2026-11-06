// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/daemon_socket_path_limit.cpp — a socket path the platform cannot
// bind is refused by name, and leaves nothing behind.
//
// sockaddr_un::sun_path is 104 bytes on Darwin and 108 on Linux, so a control
// directory deep enough to overrun it is reachable in ordinary use — a scratch
// path or a sandboxed container is enough. bindSocket refuses such a path by
// name, because a bare false leaves the caller to name two causes that are not
// true: a live daemon, or a failed shell spawn.
//
// The limit is taken from sizeof(sun_path) rather than a literal. A test that
// hard-coded 104 would pass on Darwin and assert nothing on Linux, where a
// 104-byte path binds successfully.
//
// Cases:
//   - a path one byte past the platform limit is refused as PathTooLong
//   - the refusal creates no lock file, because validation precedes the lock
//   - the longest path the platform accepts still binds
//

#include "Assertions.h"

#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using Ubiquitty::Host::BindFailure;
using Ubiquitty::Host::Daemon;

namespace {

std::string makeControlDirectory()
{
    char pattern[] = "/tmp/ubiquitty-sock-limit-XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    CHECK(directory != nullptr);
    return directory ? std::string(directory) : std::string();
}

bool pathExists(const std::string& path)
{
    struct stat entry {};
    return ::stat(path.c_str(), &entry) == 0;
}

// A socket path of exactly `length` bytes inside `directory`, padded with a
// repeated leaf character so only its length is under test.
std::string socketPathOfLength(const std::string& directory, std::size_t length)
{
    const std::string prefix = directory + "/";
    CHECK(length > prefix.size());
    return prefix + std::string(length - prefix.size(), 's');
}

} // namespace

int main()
{
    const std::string controlDirectory = makeControlDirectory();
    if (controlDirectory.empty())
        return 1;

    // The platform's own limit, never a literal: Darwin 103, Linux 107 (both
    // excluding the terminator sun_path must hold).
    const std::size_t maximum = Ubiquitty::Host::maximumSocketPathLength();
    struct sockaddr_un address;
    CHECK(maximum == sizeof(address.sun_path) - 1);

    // One byte past the limit is refused, and named.
    {
        const std::string tooLong = socketPathOfLength(controlDirectory, maximum + 1);
        CHECK(tooLong.size() == maximum + 1);
        const std::string processIDPath = controlDirectory + "/pid-long";

        Daemon daemon;
        CHECK(!daemon.start(tooLong.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24));
        CHECK(daemon.lastBindFailure() == BindFailure::PathTooLong);

        // The lock is taken only after the path is validated, so a refused path
        // cannot leave a lock file beside a socket that was never created.
        CHECK(!pathExists(tooLong + ".lock"));
        CHECK(!pathExists(tooLong));
    }

    // The longest path the platform does accept still binds, which is what
    // pins the boundary rather than merely proving that something long fails.
    {
        const std::string longest = socketPathOfLength(controlDirectory, maximum);
        CHECK(longest.size() == maximum);
        const std::string processIDPath = controlDirectory + "/pid-max";

        Daemon daemon;
        CHECK(daemon.start(longest.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24));
        CHECK(daemon.lastBindFailure() == BindFailure::None);
        CHECK(pathExists(longest));
    }

    if (failures == 0)
        std::printf("daemon_socket_path_limit: ok (maximum %zu bytes)\n", maximum);
    return UBIQUITTY_TEST_SUMMARY();
}
