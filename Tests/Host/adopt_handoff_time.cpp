// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Measures real complete-Terminal Prepare/Commit handoffs, retaining the actual
// model, immutable body, master and attempt in this process for every outcome.
// These live fixtures are separate from the memory-only uncertainty regressions.

#include "LocalAdoptionFixture.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Ubiquitty::HostTest;

int main()
{
    char directory[] = "/tmp/ubiquitty-handoff.XXXXXX";
    const char* home = ::mkdtemp(directory);
    if (!home)
        return 1;
    LocalAdoptionFixture fixture;
    if (!fixture.start(std::string(home) + "/.ubiquitty")) {
        std::fprintf(stderr, "adopt_handoff_time: FAIL — fixture daemon did not start\n");
        return 1;
    }
    for (const int lines : { 80, 640, 2'800 }) {
        auto* owner = fixture.makeOwner(400, 24);
        if (!owner || !fixture.startPausedLeader(*owner))
            return 1;
        Ubiquitty::setScrollbackLimit(owner->terminal.get(), lines + 24);
        std::string line;
        for (int index = 0; index < lines; ++index) {
            line.clear();
            for (int column = 0; column + 1 < 400; ++column)
                line += "\x1b[38;5;" + std::to_string((index % 256 + column * 7) % 256) + "m" + static_cast<char>('a' + (index % 26 + column) % 26);
            line += "\r\n";
            Ubiquitty::processBytes(owner->terminal.get(), line.c_str());
        }
        Ubiquitty::termFlushScrollLane(*owner->terminal);
        if (!fixture.adopt(*owner)) {
            const auto* attempt = owner->result.attempt.get();
            const int rejection = attempt && attempt->rejection() ? static_cast<int>(*attempt->rejection()) : -1;
            std::fprintf(stderr, "adopt_handoff_time: FAIL — %d history lines lacked exact Committed evidence (body %zu bytes, elapsed %lld ms, ownership %u, attempt %d, rejection %d)\n",
                lines, owner->frozen.size, static_cast<long long>(owner->elapsed.count()), static_cast<unsigned>(owner->result.ownership), attempt != nullptr, rejection);
            return 1;
        }
        std::printf("adopt_handoff_time: %d lines, %.1f MiB, %lld ms, session %u\n", lines,
            static_cast<double>(owner->frozen.size) / (1024.0 * 1024.0), static_cast<long long>(owner->elapsed.count()), owner->result.adopted.sessionID);
    }
    std::printf("adopt_handoff_time: all three complete terminal handoffs committed to their exact retained targets\n");
    return 0;
}
