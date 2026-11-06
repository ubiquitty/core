// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/handover_diffed_resume.cpp — a client that held a pane's picture
// when its host handed over resumes on the successor and is continued with a
// diff, not drawn again (Docs/sync.md §6.9, §6.13), between real daemons.
//
// A person attaches with the production client, which keeps its pane's
// replica, has the shell print a line, and holds the picture with it. The
// binary at the holder's path is replaced and started, the holder hands over,
// and the client resumes on the successor naming the generation and digest it
// holds: the attachment is the one the holder issued, the successor names the
// holder as its predecessor, the pane is continued with a diff and applies no
// checkpoint, the picture it keeps shows the line, and the shell it reaches
// takes input.
//
// argv[1] = the built ubiquitty-host. Exit 0 on success.

#include "Host/HandoverProcesses.h"
#include "Host/ScreenClient.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace UbiquittyTest::HandoverProcesses;
namespace HostTest = Ubiquitty::HostTest;

namespace {

std::uint64_t nowMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Carries a client's stream over its bridge, both ways, until a condition
/// holds or the time is up.
///
/// - Returns: Whether the condition held.
bool pump(UbiquittyTest::ScreenClient& client, int fileDescriptor, const std::function<bool()>& isDone, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    std::array<std::uint8_t, 65'536> buffer {};
    for (;;) {
        const std::vector<std::uint8_t> up = client.takeStream(nowMicroseconds());
        for (std::size_t written = 0; written < up.size();) {
            const ssize_t count = ::write(fileDescriptor, up.data() + written, up.size() - written);
            if (count <= 0)
                return isDone();
            written += static_cast<std::size_t>(count);
        }
        if (isDone())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        pollfd readable { .fd = fileDescriptor, .events = POLLIN, .revents = 0 };
        if (::poll(&readable, 1, 10) > 0) {
            const ssize_t count = ::read(fileDescriptor, buffer.data(), buffer.size());
            if (count <= 0)
                return isDone();
            client.read(std::span(buffer.data(), static_cast<std::size_t>(count)), nowMicroseconds());
        }
    }
}

/// The visible grid of a client's pane, row by row.
std::string gridText(const UbiquittyTest::ScreenClient& client)
{
    const Ubiquitty::Terminal* terminal = client.pane();
    std::string text;
    if (!terminal)
        return text;
    for (int row = 0; row < terminal->rows; ++row) {
        for (int column = 0; column < terminal->columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

bool type(UbiquittyTest::ScreenClient& client, std::string_view text)
{
    return client.sendRaw({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }).has_value();
}

void run(const std::string& built)
{
    Install install;
    REQUIRE(install.create(built, "handover-diffed"));
    const pid_t holder = startDaemon(install);
    REQUIRE(holder > 0 && HostTest::waitForDaemonReady(install.home + "/" + install.socketLeaf()));

    // A person attaches with the production client and holds the pane.
    Sync::ReplicaSet replicas;
    auto bridge = HostTest::connectDaemonAsLocalBridge(install.binary, install.home);
    if (bridge.bridgeProcessID > 0)
        children().push_back(bridge.bridgeProcessID);
    REQUIRE(bridge.fileDescriptor >= 0);
    UbiquittyTest::ScreenClient held(24, 80, replicas, std::nullopt);
    REQUIRE(held.start({ .target = Sync::AdmissionTarget::Session, .workspaceName = "handover-diffed" }));
    REQUIRE_MSG(pump(held, bridge.fileDescriptor, [&] { return held.connection().welcome() && held.pane() && gridText(held).find_first_not_of(" \n") != std::string::npos; }, 30'000), "the holder welcomed the client and its shell drew");
    const Sync::Welcome welcome = *held.connection().welcome();
    const auto paneID = held.boundPane(1);
    REQUIRE(paneID.has_value());
    // The line is printed in two pieces, so only the shell's output, never
    // the command echoed, shows it whole.
    REQUIRE(type(held, "printf 'diff%s\\n' ed-before\r"));
    REQUIRE_MSG(pump(held, bridge.fileDescriptor, [&] { return gridText(held).find("diffed-before") != std::string::npos; }, 30'000), "the shell printed the line");
    // The client acknowledges what it holds a moment later; the host keeps
    // that baseline to continue from.
    (void)pump(held, bridge.fileDescriptor, [] { return false; }, 500);
    held.storeReplicas();

    // The holder hands over.
    REQUIRE(install.replace());
    const pid_t successor = startDaemon(install);
    REQUIRE(successor > 0);
    CHECK_MSG(pump(held, bridge.fileDescriptor, [&] { return held.connection().byeReason() == Sync::ByeReason::HandingOver; }, 30'000), "the client is told its host is handing over");
    held.storeReplicas();
    ::close(bridge.fileDescriptor);
    CHECK_MSG(waitForExit(holder, 30'000) == std::optional { 0 }, "the holder exits cleanly after handing over");
    REQUIRE(HostTest::waitForDaemonReady(install.home + "/" + install.socketLeaf()));

    // The client resumes on the successor, naming what it holds.
    Sync::ReplicaSet::Held* kept = replicas.find(*paneID);
    REQUIRE(kept);
    Sync::ResumeRequest resume { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    resume.panes.push_back({ .paneID = *paneID, .ackedGeneration = kept->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(kept->replica), .historyEpoch = kept->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 1, .view = Sync::ViewState::Live });
    bridge = HostTest::connectDaemonAsLocalBridge(install.binary, install.home);
    if (bridge.bridgeProcessID > 0)
        children().push_back(bridge.bridgeProcessID);
    REQUIRE(bridge.fileDescriptor >= 0);
    UbiquittyTest::ScreenClient resumed(24, 80, replicas, resume);
    REQUIRE(resumed.start({ .target = Sync::AdmissionTarget::Session, .sessionID = welcome.selection.sessionID, .isAttachOnly = true }));
    REQUIRE_MSG(pump(resumed, bridge.fileDescriptor, [&] { return resumed.connection().welcome() && resumed.pane(); }, 30'000), "the successor welcomed the resume");
    const Sync::Welcome& again = *resumed.connection().welcome();
    CHECK_MSG(again.attachmentID == welcome.attachmentID, "the resume did not continue the attachment the holder issued");
    CHECK_MSG(again.predecessorHostInstanceID == std::optional { welcome.hostInstanceID }, "the successor does not name the holder as its predecessor");
    REQUIRE(again.resumed.size() == 1);
    CHECK_MSG(again.resumed[0].paneID == *paneID, "the resumed pane is not the one the client held");
    CHECK_MSG(again.resumed[0].outcome == Sync::ResumeOutcome::Diffed, "the successor drew the pane again rather than continuing it with a diff");
    (void)pump(resumed, bridge.fileDescriptor, [] { return false; }, 500);
    CHECK_MSG(resumed.connection().counters().checkpointsApplied == 0, "the resumed client applied a checkpoint");
    CHECK_MSG(gridText(resumed).find("diffed-before") != std::string::npos, "the picture the client kept lost the line it held");

    // The shell the successor holds takes input.
    // Numbered past what the successor reports written, as the production
    // client numbers what it types after a resume.
    const std::string_view after = "printf 'diff%s\\n' ed-after\r";
    const std::uint64_t written = again.resumed[0].inputWritten.value_or(0);
    CHECK_MSG(written == 1, "the successor does not report the line the client typed before as written");
    REQUIRE(resumed.sendRawNumbered(1, written + 1, { reinterpret_cast<const std::uint8_t*>(after.data()), after.size() }));
    CHECK_MSG(pump(resumed, bridge.fileDescriptor, [&] { return gridText(resumed).find("diffed-after") != std::string::npos; }, 30'000), "the shell on the successor did not answer");
    ::close(bridge.fileDescriptor);
    (void)successor;
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
    if (failures == 0)
        std::printf("handover_diffed_resume: a client that held its pane when its host handed over resumes the same attachment on the successor, "
                    "is continued with a diff and no checkpoint, keeps its picture, and reaches the shell\n");
    return failures == 0 ? 0 : 1;
}
