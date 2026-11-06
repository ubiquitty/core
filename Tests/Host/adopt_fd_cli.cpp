// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A genuine owner adopts through the production two-phase sender. A sibling of
// the PTY's session leader, holding the same master, receives exact NotOwner
// evidence on Prepare and Status. No helper exit or empty stdout proves refusal.

#include "LocalAdoptionFixture.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Ubiquitty::HostTest;

int main()
{
    char directory[] = "/tmp/ubiquitty-adoptfd.XXXXXX";
    const char* home = ::mkdtemp(directory);
    if (!home)
        return 1;
    LocalAdoptionFixture fixture;
    if (!fixture.start(std::string(home) + "/.ubiquitty")) {
        std::fprintf(stderr, "adopt_fd_cli: FAIL — fixture daemon did not start\n");
        return 1;
    }
    auto* owner = fixture.makeOwner();
    if (!owner || !fixture.startPausedLeader(*owner) || !fixture.adopt(*owner)) {
        std::fprintf(stderr, "adopt_fd_cli: FAIL — the genuine ancestor did not receive exact Committed evidence\n");
        return 1;
    }
    const auto before = publishedAdoptionPaneCount(fixture.socketPath());
    if (!before || *before != 1)
        return 1;

    // Both this child and the leader are direct children of this fixture. The
    // child has the same executable and master but is not the leader's ancestor.
    AdoptionChild attacker;
    if (!attacker.spawn([&] {
            return AdoptionProbe::isRejectedWithoutActivation(fixture.socketPath(), owner->master.get(), owner->frozen.view()) ? 0 : 1;
        })
        || !attacker.waitForSuccess(std::chrono::seconds(20))) {
        std::fprintf(stderr, "adopt_fd_cli: FAIL — the unrelated sibling lacked exact retained NotOwner evidence\n");
        return 1;
    }
    const auto after = publishedAdoptionPaneCount(fixture.socketPath());
    if (!after || *after != *before || !owner->isCommitted()) {
        std::fprintf(stderr, "adopt_fd_cli: FAIL — a refused Prepare changed the published owners\n");
        return 1;
    }

    // Preserve the separate claimedOwner == sessionLeader admission shortcut.
    // This branch prepares and exactly aborts; the parent/ancestor above is the
    // full Commit/publication proof. No branch parks an unresolved helper.
    auto* selfLeader = fixture.makeOwner();
    if (!selfLeader)
        return 1;
    auto body = Ubiquitty::Session::captureShareTerminal(*selfLeader->terminal, Ubiquitty::Protocol::Local::maxAdoptBodyBytes);
    if (!body)
        return 1;
    selfLeader->frozen = std::move(*body);
    AdoptionChild selfAdmission;
    if (!selfAdmission.spawn([&] {
            if (::setsid() < 0 || ::ioctl(selfLeader->slave.get(), TIOCSCTTY, 0) < 0)
                return 1;
            return AdoptionProbe::checkAdmission(fixture.socketPath(), selfLeader->master.get(), selfLeader->frozen.view(), AdoptionProbe::AdmissionExpectation::PreparedThenAborted) ? 0 : 1;
        })
        || !selfAdmission.waitForSuccess(std::chrono::seconds(20))) {
        std::fprintf(stderr, "adopt_fd_cli: FAIL — the genuine session leader lacked exact Prepared/Aborted evidence\n");
        return 1;
    }
    const auto finalCount = publishedAdoptionPaneCount(fixture.socketPath());
    if (!finalCount || *finalCount != *before)
        return 1;
    std::printf("adopt_fd_cli: ancestor Commit, sibling NotOwner refusal, and session-leader Prepared/Aborted admission all retain exact evidence\n");
    return 0;
}
