// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PAL implementation for ubiquitty-host (the portable-POSIX remote-Core daemon).
// Supplies the NON-stream "Core calls down" half of the PAL seam: the monotonic
// clock + stderr diag sink. The "up" hooks live in the portable PAL.cpp; the
// byte-stream transport is the shared openPTY (PTYStream.cpp). Clipboard /
// preferences / bell are stubs — a headless daemon has none.

#include "PAL/PAL.h"

#include <cstdio>
#include <ctime>

namespace Ubiquitty {
namespace PAL {

Ticks monotonicTicks()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return Ticks::fromTimespec(static_cast<uint64_t>(ts.tv_sec), static_cast<uint64_t>(ts.tv_nsec));
}

uint32_t monotonicMicros()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u);
}

void diagWrite(std::string_view message)
{
    if (message.empty())
        return;
    (void)::fwrite(message.data(), 1, message.size(), stderr);
    (void)::fputc('\n', stderr);
}

// Stubs: a headless daemon has no clipboard or bell. Preferences are not among
// them any more — the backend is bundled (Platform/POSIX/Preferences.cpp), so the
// daemon reads the same configuration file as every other POSIX arm rather than
// being told there is none.
void clipboardSetText(std::string_view /*utf8*/) { }
int clipboardGetText(char* /*outputBuffer*/, int /*outputCapacity*/) { return -1; }
void bellRing(BellMode /*mode*/) { }

} // namespace PAL
} // namespace Ubiquitty
