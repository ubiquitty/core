// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ProcessSpawnAndWait.cpp — PAL::processSpawnAndWait for the POSIX backends.
//
// Its own translation unit, and the reason is a cut it was blocking. This is the
// only function in the POSIX system PAL that starts a child process, and it sat
// in SystemPAL.cpp beside forty that do not — so a platform forbidding child
// processes could not decline it without declining the whole backend it needs.
// The iOS arm carried a posix_spawn reference for a function nothing in its link
// calls, because the two could not be separated.
//
// One function per capability is what lets a per-platform swap say no to exactly
// one thing. Source/Platform/iOS/ProcessSpawnAndWait.cpp is the swap.

#include "PAL/PAL.h"

#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

namespace Ubiquitty::PAL {

int processSpawnAndWait(const char* path, const char* const argv[])
{
    if (!path || path[0] == '\0' || !argv)
        return -1;

    // A worker thread may block control signals. Its child is a new process,
    // whose daemon must be able to receive those signals on its own threads.
    posix_spawnattr_t attributes;
    if (::posix_spawnattr_init(&attributes) != 0)
        return -1;
    sigset_t unblocked;
    sigemptyset(&unblocked);
    if (::posix_spawnattr_setsigmask(&attributes, &unblocked) != 0
        || ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK) != 0) {
        ::posix_spawnattr_destroy(&attributes);
        return -1;
    }
    pid_t processID = 0;
    const int rc = ::posix_spawn(&processID, path, nullptr, &attributes, const_cast<char* const*>(argv), environ);
    ::posix_spawnattr_destroy(&attributes);
    if (rc != 0)
        return -1;

    int status = 0;
    while (::waitpid(processID, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

} // namespace Ubiquitty::PAL
