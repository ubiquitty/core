// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/handover_refusal_keeps_serving.cpp — an install never ends a
// session (Docs/sync.md §6.13 "Install policy"): when the running host refuses
// the handover, it keeps every pane and keeps serving, and the new binary
// runs beside it with none, having been told why.
//
// One pane is held inside an operating-system command its program never
// terminates, so it cannot reach ground. The binary at the holder's path is
// replaced and started. The holder refuses with PaneBusy and keeps both
// panes; the successor logs which reason the holder gave; the person on the
// other pane is not disconnected and still types into it; the holder still
// runs.
//
// argv[1] = the built ubiquitty-host. Exit 0 on success.
//

#include "Host/HandoverProcesses.h"

#include <cstdio>

using namespace UbiquittyTest::HandoverProcesses;
namespace HostTest = Ubiquitty::HostTest;

namespace {

void run(const std::string& built)
{
    Install install;
    REQUIRE(install.create(built, "handover-refusal"));
    const pid_t holder = startDaemon(install);
    const std::string holderLeaf = install.socketLeaf();
    REQUIRE(holder > 0 && HostTest::waitForDaemonReady(install.home + "/" + holderLeaf));

    Person served;
    served.instance = { .low = 0x726566757365, .high = 1 };
    served.workspace = "refusal-served";
    REQUIRE(served.attach(install));
    REQUIRE(served.waitForShell());
    REQUIRE(HostTest::sendInput(*served.connection, "echo be\"\"fore\r"));
    REQUIRE(served.waitForText("before"));

    Person busy;
    busy.instance = { .low = 0x726566757365, .high = 2 };
    busy.workspace = "refusal-busy";
    REQUIRE(busy.attach(install));
    REQUIRE(busy.waitForShell());
    // A window title begun and never ended: the parser stays inside it.
    REQUIRE(HostTest::sendInput(*busy.connection, "printf '\\033]2;never ended'; sleep 120\r"));
    ::usleep(300'000);
    // Each person reads everything they were sent before the install starts,
    // so the pane is the holder's only reason to refuse, and the reason it
    // gives is that one exactly.
    std::string body;
    for (Person* person : { &served, &busy })
        (void)HostTest::waitFor(*person->connection, Sync::FrameKind::Bye, body, 500);

    CHECK_MSG(install.replace(), "the successor is installed in place");
    const std::string successorLog = install.root + "/successor.log";
    const pid_t successor = startDaemon(install, successorLog);
    REQUIRE(successor > 0);
    CHECK_MSG(waitForFileText(successorLog, "kept its sessions (a pane did not reach ground)", 30'000),
        "the successor is told which reason the holder gave");
    CHECK_MSG(waitForFileText(successorLog, "took over 0 sessions; 1 older hosts kept theirs", 10'000), "the successor takes no session");
    CHECK_MSG(HostTest::waitForDaemonReady(install.home + "/" + install.socketLeaf()), "the successor serves beside the holder");

    CHECK_MSG(!waitForExit(holder, 500), "the holder keeps running");
    CHECK_MSG(!HostTest::waitFor(*served.connection, Sync::FrameKind::Bye, body, 200), "the holder disconnects nobody");
    CHECK_MSG(HostTest::sendInput(*served.connection, "echo af\"\"ter\r") && served.waitForText("after"),
        "the holder still serves the person typing into its pane");
    const auto busyText = busy.capture();
    CHECK_MSG(busyText.has_value(), "the holder still serves the pane it could not hand over");

    ::close(served.connection->fileDescriptor);
    ::close(busy.connection->fileDescriptor);
    endChildren();
    install.finish();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    run(argv[1]);
    endChildren();
    return UBIQUITTY_TEST_SUMMARY();
}
