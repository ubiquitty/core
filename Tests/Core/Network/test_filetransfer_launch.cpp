// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_filetransfer_launch.cpp — end-to-end ZMODEM agent run over a pty pair,
// through the portable runner (Core/Network/FileTransferLaunch) and the PAL
// spawn realization (Platform/POSIX/Spawn.cpp). The master end is O_NONBLOCK,
// exactly as the ingest poll opens it: a realization that leaves the flag set
// kills lrzsz (rz exits 128 on a first-read EAGAIN) and this test goes red.
// Requires lrzsz (rz/sz) on PATH-style agent locations; the suite reports
// SKIPPED without it rather than silently passing.

#include "Core/Network/FileTransferLaunch.h"
#include "PAL/PAL.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Ubiquitty;

namespace {

// How long the agent under test may run before the limit terminates it.
//
// Far below the product's own hour, because a suite cannot spend one: the
// transfer here moves a few dozen bytes and finishes in hundredths of a second,
// so anything approaching this is the wedge rather than slow work. Passed
// explicitly rather than defaulted, because a limit longer than ctest's own
// timeout is no limit here: the runner would still be waiting when the harness
// gave up, and the suite would report a timeout naming neither the agent nor
// what it was waiting on.
constexpr double agentLimitSeconds = 15.0;

// How long a `sz`/`rz` on the far end of the pty may take to exit.
//
// Generous against a loaded machine and still finite: the transfer itself
// finishes in hundredths of a second, so anything approaching this is a child
// that is not going to exit at all.
constexpr double childExitSeconds = 20.0;

// How many times one direction runs when its agent keeps reaching the limit.
constexpr int attemptLimit = 3;

// Waits for a child, bounded, and kills it rather than hanging.
//
// A `waitpid` with no options waits forever, and when the child does not exit —
// a write blocking on a reader that has gone, or a spawn that failed in a way
// the parent cannot see — the test stops instead of failing. ctest can then
// only report a timeout, which names neither the child nor what it was waited
// on for; this leaves the suite a failure that does.
//
// - Parameters:
//   - child: The process to reap.
//   - seconds: How long to allow before killing it.
//   - status: Receives the wait status when the child exited on its own.
// - Returns: Whether the child exited within the deadline.
bool waitForChild(pid_t child, double seconds, int& status)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(static_cast<long>(seconds * 1000.0));
    for (;;) {
        const pid_t reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child)
            return true;
        if (reaped < 0)
            return false;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Killed and reaped, so the next case does not inherit a live child.
    kill(child, SIGKILL);
    int discarded = 0;
    waitpid(child, &discarded, 0);
    return false;
}

constexpr const char* fixtureFileName = "ubiquitty-launch-send.txt";
const char fixtureBytes[] = {
    'T', 'H', 'E', ' ', 'Q', 'U', 'I', 'C', 'K', ' ',
    'B', 'R', 'O', 'W', 'N', ' ', 'F', 'O', 'X', '\n',
    '\xC3', '\xA9', '\x00', '\xFF', '\r', '\n', 0x00
};

// A directory under /tmp that removes itself and every file a case left in it.
//
// Each exit from the case passes through the destructor, the early ones
// included, so a run that fails or stops short leaves nothing behind in /tmp.
class ScratchDirectory final {
public:
    ScratchDirectory()
    {
        char pattern[] = "/tmp/ubiquitty-launch-XXXXXX";
        if (mkdtemp(pattern))
            m_path = pattern;
    }

    ~ScratchDirectory() { (void)removeWithContents(); }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::string& path() const { return m_path; }
    [[nodiscard]] bool isValid() const { return !m_path.empty(); }

    /// Removes the files in the directory, then the directory.
    ///
    /// - Returns: Whether the directory is gone.
    bool removeWithContents()
    {
        if (m_path.empty())
            return true;
        if (DIR* directory = opendir(m_path.c_str())) {
            while (const dirent* entry = readdir(directory)) {
                const std::string_view name(entry->d_name);
                if (name != "." && name != "..")
                    unlink((m_path + "/" + entry->d_name).c_str());
            }
            closedir(directory);
        }
        if (rmdir(m_path.c_str()) != 0)
            return false;
        m_path.clear();
        return true;
    }

private:
    std::string m_path;
};

std::string readFile(const std::string& path)
{
    int fileDescriptor = open(path.c_str(), O_RDONLY);
    if (fileDescriptor < 0)
        return {};
    std::string contents;
    char chunk[4096];
    for (;;) {
        ssize_t count = read(fileDescriptor, chunk, sizeof(chunk));
        if (count <= 0)
            break;
        contents.append(chunk, static_cast<size_t>(count));
    }
    close(fileDescriptor);
    return contents;
}

bool writeFixture(const std::string& directory)
{
    std::string path = directory + "/" + fixtureFileName;
    int fileDescriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fileDescriptor < 0)
        return false;
    bool isWritten = write(fileDescriptor, fixtureBytes, sizeof(fixtureBytes)) == static_cast<ssize_t>(sizeof(fixtureBytes));
    close(fileDescriptor);
    return isWritten;
}

int makePTYPair(int& masterOut, int& slaveOut)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }
    masterOut = master;
    slaveOut = slave;
    return 0;
}

// What one round trip did, each side apart, so a failure names the side it was.
struct RoundTripOutcome {
    bool didStart = false;
    bool didAgentSucceed = false;
    double agentSeconds = 0.0;
    bool didFarSideExit = false;
    int farSideStatus = 0;

    [[nodiscard]] bool didSucceed() const
    {
        return didAgentSucceed && didFarSideExit && WIFEXITED(farSideStatus) && WEXITSTATUS(farSideStatus) == 0;
    }

    // An agent the limit ended, as opposed to one that failed on its own.
    [[nodiscard]] bool wasAgentWedged() const
    {
        return didStart && !didAgentSucceed && agentSeconds >= agentLimitSeconds;
    }
};

// Runs the portable runner on the nonblocking master end, exactly as the
// frontend ingest does, with the far side's program already on the slave end.
// The agent is timed on its own, apart from the wait for the far side to exit.
RoundTripOutcome runAgainstFarSide(char* const farSideArguments[], const char* farSideDirectory,
    Network::TransferDirection direction, std::string_view operand)
{
    RoundTripOutcome outcome;
    int master = -1;
    int slave = -1;
    if (makePTYPair(master, slave) != 0)
        return outcome;
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    pid_t child = fork();
    if (child < 0) {
        close(master);
        close(slave);
        return outcome;
    }
    if (child == 0) {
        setsid();
        if (farSideDirectory && chdir(farSideDirectory) != 0)
            _exit(126);
        dup2(slave, 0);
        dup2(slave, 1);
        close(master);
        close(slave);
        execv(farSideArguments[0], farSideArguments);
        _exit(127);
    }
    close(slave);
    outcome.didStart = true;

    const auto agentStarted = std::chrono::steady_clock::now();
    outcome.didAgentSucceed = Network::runTransferAgent(master, direction, operand, agentLimitSeconds);
    outcome.agentSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - agentStarted).count();
    close(master);
    outcome.didFarSideExit = waitForChild(child, childExitSeconds, outcome.farSideStatus);
    return outcome;
}

// The far side sends with `sz`; the agent receives with `rz`.
RoundTripOutcome receiveRoundTrip(const std::string& fixturePath, const std::string& downloadDirectory)
{
    std::string szPath = Network::resolveTransferAgentPath(Network::TransferDirection::Send);
    char* arguments[] = { const_cast<char*>(szPath.c_str()), const_cast<char*>(fixturePath.c_str()), nullptr };
    return runAgainstFarSide(arguments, nullptr, Network::TransferDirection::Receive, downloadDirectory);
}

// The far side receives with `rz` in its own directory; the agent sends with `sz`.
RoundTripOutcome sendRoundTrip(const std::string& fixturePath, const std::string& sendWorkDirectory)
{
    std::string rzPath = Network::resolveTransferAgentPath(Network::TransferDirection::Receive);
    char* arguments[] = { const_cast<char*>(rzPath.c_str()), nullptr };
    return runAgainstFarSide(arguments, sendWorkDirectory.c_str(), Network::TransferDirection::Send, fixturePath);
}

// Says what each side of a failed attempt did.
//
// On standard error, which is unbuffered, so the line survives a harness
// timeout that kills this process before standard output is flushed.
void reportFailedAttempt(const char* what, int attempt, const RoundTripOutcome& outcome)
{
    if (!outcome.didStart) {
        std::fprintf(stderr, "    %s attempt %d of %d: the pty pair or the far side's fork could not be made\n",
            what, attempt, attemptLimit);
        return;
    }
    char farSide[64];
    if (!outcome.didFarSideExit)
        std::snprintf(farSide, sizeof(farSide), "did not exit within %.0fs and was killed", childExitSeconds);
    else if (WIFEXITED(outcome.farSideStatus))
        std::snprintf(farSide, sizeof(farSide), "exited %d", WEXITSTATUS(outcome.farSideStatus));
    else if (WIFSIGNALED(outcome.farSideStatus))
        std::snprintf(farSide, sizeof(farSide), "was ended by signal %d", WTERMSIG(outcome.farSideStatus));
    else
        std::snprintf(farSide, sizeof(farSide), "stopped without an exit status");
    std::fprintf(stderr, "    %s attempt %d of %d: agent %s after %.1fs of its %.0fs limit; far side %s\n",
        what, attempt, attemptLimit, outcome.didAgentSucceed ? "succeeded" : "failed", outcome.agentSeconds,
        agentLimitSeconds, farSide);
}

// Runs one direction, retrying an agent the limit had to terminate and nothing else.
//
// The agent stops inside its own terminal setup on roughly one launch in
// twenty-five here — sampled as `lsz` in `io_mode`, an ioctl that does not
// return — and no signal on this side separates that from a slow start, so the
// runner kills it at the limit. That is a third-party binary's coin flip, and
// what this file is for is our spawn and our runner: a case that went red on
// somebody else's dice would report nothing about either, and would be ignored
// for the same reason a red that is always somebody else's gets ignored.
//
// The wedge is told apart by the agent's own run, timed without the wait for
// the far side: an agent that ran the whole limit was ended by it, and is
// retried. Every other failure is one this suite owns — an agent that failed
// sooner, or a far side still running after the agent finished — and is
// reported at once rather than retried until it passes, which is the difference
// between tolerating a known third-party stall and hiding our own. Counting the
// far side's wait as the agent's would make a far side that never exits look
// like a wedged agent, and three such attempts outlast ctest's timeout.
template<typename Attempt>
bool roundTripToleratingAWedgedAgent(const char* what, Attempt&& attempt)
{
    for (int tries = 1; tries <= attemptLimit; ++tries) {
        const RoundTripOutcome outcome = attempt();
        if (outcome.didSucceed())
            return true;
        reportFailedAttempt(what, tries, outcome);
        if (!outcome.wasAgentWedged())
            return false;
    }
    return false;
}

} // namespace

TEST(filetransferlaunch)
{
    Network::TransferDirection send = Network::TransferDirection::Send;
    Network::TransferDirection receive = Network::TransferDirection::Receive;

    if (Network::resolveTransferAgentPath(receive).empty()
        || Network::resolveTransferAgentPath(send).empty()) {
        // The CI image installs lrzsz and names it in UBQ_REQUIRED_TOOLS, so
        // there its absence is a broken image rather than a machine without it.
        CHECK_MSG(!UbiquittyTest::isRequiredTool("lrzsz"), "UBQ_REQUIRED_TOOLS names lrzsz, but no rz and sz are on an agent search path");
        if (!UbiquittyTest::isRequiredTool("lrzsz"))
            UbiquittyTest::skip("needs lrzsz (rz and sz) on an agent search path");
        return;
    }

    ScratchDirectory fixtureDirectory;
    ScratchDirectory downloadDirectory;
    ScratchDirectory sendWorkDirectory;
    REQUIRE(fixtureDirectory.isValid());
    REQUIRE(downloadDirectory.isValid());
    REQUIRE(sendWorkDirectory.isValid());
    REQUIRE(writeFixture(fixtureDirectory.path()));
    const std::string fixturePath = fixtureDirectory.path() + "/" + fixtureFileName;
    const std::string expected(fixtureBytes, sizeof(fixtureBytes));

    // ---- receive: remote sz streams over the pty, agent rz lands the file ----
    CHECK(roundTripToleratingAWedgedAgent("receive", [&] { return receiveRoundTrip(fixturePath, downloadDirectory.path()); }));
    CHECK(readFile(downloadDirectory.path() + "/" + fixtureFileName) == expected);

    // ---- send: agent sz uploads the fixture, remote rz receives it ----
    CHECK(roundTripToleratingAWedgedAgent("send", [&] { return sendRoundTrip(fixturePath, sendWorkDirectory.path()); }));
    CHECK(readFile(sendWorkDirectory.path() + "/" + fixtureFileName) == expected);

    CHECK(fixtureDirectory.removeWithContents());
    CHECK(downloadDirectory.removeWithContents());
    CHECK(sendWorkDirectory.removeWithContents());
}

UBIQUITTY_TEST_MAIN()
