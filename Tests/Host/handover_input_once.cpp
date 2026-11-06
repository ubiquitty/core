// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/handover_input_once.cpp — input typed while a host hands over is
// written to the program exactly once (Docs/sync.md §6.7, §6.13), between real
// daemons and a real PTY whose reader is slower than the typing.
//
// A person types numbered lines into a shell loop that appends each to a file
// and sleeps between reads, so the PTY and the pane's queue hold input when
// the handover freezes them. The binary at the holder's path is replaced and
// started after the first hundred lines, and typing goes on until the holder
// says it is handing over and for some lines after, so the handover freezes
// typing the program has not yet taken however long the successor takes to
// start. The client reads what the host sends as it types, as a client does:
// a holder waits for what it owes its clients before it freezes anything. The
// client resumes on the successor and sends again what the successor reports
// it never wrote. Every line reaches the file once, in order.
//
// argv[1] = the built ubiquitty-host. Exit 0 on success.
//

#include "Host/HandoverProcesses.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <poll.h>
#include <sstream>
#include <unistd.h>

using namespace UbiquittyTest::HandoverProcesses;
namespace HostTest = Ubiquitty::HostTest;

namespace {

/// The lines typed before the successor is started.
constexpr int linesBeforeSuccessor = 100;
/// The lines typed after the holder says it is handing over, which it never
/// reads and the client sends again on the successor.
constexpr int linesAfterBye = 30;
/// The most lines typed while the holder has not yet said so.
constexpr int maxLines = 3000;

/// Sends one numbered event, as a client numbers its input.
bool sendEvent(Connection& connection, std::uint64_t seq, const std::string& bytes)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Raw { .inputSeq = seq, .bytes = { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() } });
    return HostTest::sendFrame(connection, Sync::FrameKind::Raw, { reinterpret_cast<const char*>(body.data()), body.size() }, connection.paneHandle);
}

std::string lineOf(int number)
{
    return "n-" + std::to_string(number);
}

/// Takes what the host has sent so far without waiting for more.
void readAvailable(Connection& connection)
{
    for (;;) {
        pollfd readable { connection.fileDescriptor, POLLIN, 0 };
        if (::poll(&readable, 1, 0) <= 0)
            return;
        char buffer[8192];
        const ssize_t got = ::read(connection.fileDescriptor, buffer, sizeof(buffer));
        if (got <= 0)
            return;
        connection.rx.append(buffer, static_cast<std::size_t>(got));
        HostTest::parseFrames(connection);
    }
}

void run(const std::string& built)
{
    Install install;
    REQUIRE(install.create(built, "handover-input"));
    const std::string log = install.root + "/typed.log";
    const pid_t holder = startDaemon(install);
    REQUIRE(holder > 0 && HostTest::waitForDaemonReady(install.home + "/" + install.socketLeaf()));

    Person person;
    person.instance = { .low = 0x696e7075746f6e63, .high = 1 };
    person.workspace = "handover-input";
    REQUIRE(person.attach(install));
    REQUIRE(person.waitForShell());
    // The loop says it runs before anything is typed for it: a line typed
    // while the shell still edits the command can stay in its line editor
    // and never reach the loop.
    REQUIRE(HostTest::sendInput(*person.connection, std::format("l={}; echo reader-started >> \"$l\"; while IFS= read -r x; do printf '%s\\n' \"$x\" >> \"$l\"; sleep 0.002; done\r", log)));
    if (!waitForFileText(log, "reader-started\n", 30'000)) {
        const auto screen = person.capture();
        std::fprintf(stderr, "handover_input_once: the reader never started; the pane shows:\n%s\n", screen ? screen->c_str() : "(nothing)");
        CHECK_MSG(false, "the reader loop starts");
        return;
    }

    // Each event and the number it was sent under.
    std::map<std::uint64_t, std::string> sent;
    pid_t successor = -1;
    int lineCount = 0;
    int linesSinceBye = -1;
    const auto hasBye = [&] {
        return std::ranges::any_of(person.connection->frames, [](const auto& frame) { return frame.kind == Sync::FrameKind::Bye; });
    };
    while (lineCount < maxLines && linesSinceBye < linesAfterBye) {
        const std::uint64_t seq = person.connection->nextInputSeq++;
        sent[seq] = lineOf(lineCount) + "\r";
        // Events the holder never reads are sent again on the successor.
        (void)sendEvent(*person.connection, seq, sent[seq]);
        if (++lineCount == linesBeforeSuccessor) {
            CHECK_MSG(install.replace(), "the successor is installed in place");
            successor = startDaemon(install);
        }
        readAvailable(*person.connection);
        if (linesSinceBye >= 0)
            ++linesSinceBye;
        else if (hasBye())
            linesSinceBye = 0;
        ::usleep(1'000);
    }
    std::string body;
    const bool sawBye = HostTest::waitFor(*person.connection, Sync::FrameKind::Bye, body, 30'000);
    const auto bye = sawBye ? Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }) : std::nullopt;
    CHECK_MSG(bye && bye->reason == Sync::ByeReason::HandingOver, "the connection is told the holder is handing over");
    ::close(person.connection->fileDescriptor);
    CHECK_MSG(waitForExit(holder, 30'000) == std::optional { 0 }, "the holder exits cleanly after handing over");
    REQUIRE(successor > 0 && HostTest::waitForDaemonReady(install.home + "/" + install.socketLeaf()));

    const std::uint64_t lastSent = sent.rbegin()->first;
    Sync::ResumeRequest resume { .attachmentID = person.attachmentID, .resumeSecret = person.resumeSecret, .panes = {} };
    resume.panes.push_back({ .paneID = person.paneID, .ackedGeneration = 0, .ackedRowHashesDigest = {}, .historyEpoch = 0, .cachedRanges = {}, .lastInputSeqSent = lastSent, .view = Sync::ViewState::Live });
    REQUIRE(person.attach(install, resume));
    const auto& welcome = *person.connection->welcome;
    CHECK_MSG(welcome.attachmentID == person.attachmentID, "the reconnect resumes the attachment the holder issued");
    REQUIRE(welcome.resumed.size() == 1);
    const std::uint64_t written = welcome.resumed[0].inputWritten.value_or(0);
    CHECK_MSG(written < lastSent, "premise: the handover froze typing the program had not yet taken");
    for (const auto& [seq, bytes] : sent) {
        if (seq > written)
            CHECK_MSG(sendEvent(*person.connection, seq, bytes), "an event is sent again on the successor");
    }

    CHECK_MSG(waitForFileText(log, lineOf(lineCount - 1) + "\n", 30'000), "every line reaches the program");
    std::ifstream file(log);
    std::map<std::string, int> occurrences;
    std::vector<std::string> order;
    for (std::string line; std::getline(file, line);) {
        if (!line.starts_with("n-"))
            continue;
        if (!occurrences[line]++)
            order.push_back(line);
    }
    for (int number = 0; number < lineCount; ++number) {
        const int count = occurrences[lineOf(number)];
        if (count != 1)
            std::fprintf(stderr, "handover_input_once: %s written %d times\n", lineOf(number).c_str(), count);
        CHECK_MSG(count == 1, "every line is written exactly once");
    }
    bool isInOrder = order.size() == static_cast<std::size_t>(lineCount);
    for (int number = 0; isInOrder && number < lineCount; ++number)
        isInOrder = order[static_cast<std::size_t>(number)] == lineOf(number);
    CHECK_MSG(isInOrder, "the lines reach the program in the order they were typed");
    ::close(person.connection->fileDescriptor);
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
