// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What running an agent against a borrowed terminal leaves behind.
//
// processSpawnWithTerminalAndWait runs a file-transfer agent on a live session's
// pseudo-terminal: the frontend stops reading, the agent speaks through the
// master, and the frontend reads on afterwards. What the user typed ahead and
// what the far side wrote past the agent's last read belong to the session, so
// the run may change the terminal's mode and the master's non-blocking flag but
// must hand both back and discard no byte. A setup step that fails leaves the
// terminal and this process's descriptor table exactly as it found them, and
// the agent sees no descriptor beyond the terminal it was given.

#include "Harness.h"

#include "PAL/PAL.h"
#include "PAL/UnixFileDescriptor.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

using Ubiquitty::PAL::UnixFileDescriptor;

namespace {

// How long an agent may run. A shell that answers one line needs a fraction of
// this; an agent that reaches it is waiting for input that was discarded.
constexpr double agentLimitSeconds = 5.0;

// How long a read waits for bytes that should already be queued.
constexpr auto queuedBytesPatience = std::chrono::seconds(5);

struct SessionTerminal {
    UnixFileDescriptor master;
    UnixFileDescriptor replica;
};

// Opens a pseudo-terminal as a session holds one.
//
// The master is non-blocking, as the ingest poll opens it. The replica is in a
// line mode with neither echo nor output processing: distinguishable from the
// raw mode the agent runs in, and writing exactly the bytes a case counts.
SessionTerminal openSessionTerminal()
{
    SessionTerminal terminal;
    terminal.master.reset(::posix_openpt(O_RDWR | O_NOCTTY));
    if (!terminal.master.isValid() || ::grantpt(terminal.master.get()) != 0 || ::unlockpt(terminal.master.get()) != 0)
        return {};
    const char* replicaPath = ::ptsname(terminal.master.get());
    if (!replicaPath)
        return {};
    terminal.replica.reset(::open(replicaPath, O_RDWR | O_NOCTTY));
    if (!terminal.replica.isValid())
        return {};

    struct termios mode;
    if (::tcgetattr(terminal.replica.get(), &mode) != 0)
        return {};
    mode.c_lflag |= ICANON;
    mode.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHOE | ECHOK | ECHONL);
    mode.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    if (::tcsetattr(terminal.replica.get(), TCSANOW, &mode) != 0)
        return {};

    const int flags = ::fcntl(terminal.master.get(), F_GETFL);
    if (flags < 0 || ::fcntl(terminal.master.get(), F_SETFL, flags | O_NONBLOCK) != 0)
        return {};
    return terminal;
}

// Whether two modes agree on every flag a session depends on.
//
// PENDIN is left out: Darwin raises it on a switch into canonical mode until the
// next read retypes the queue, which is the queue being kept rather than the
// mode differing.
bool isSameMode(const struct termios& expected, const struct termios& actual)
{
    tcflag_t localMask = ~static_cast<tcflag_t>(0);
#if defined(PENDIN)
    localMask &= ~static_cast<tcflag_t>(PENDIN);
#endif
    return expected.c_iflag == actual.c_iflag
        && expected.c_oflag == actual.c_oflag
        && expected.c_cflag == actual.c_cflag
        && (expected.c_lflag & localMask) == (actual.c_lflag & localMask);
}

bool writeAll(int descriptor, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written <= 0)
            return false;
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

// Reads what `descriptor` has queued until `wanted` has arrived.
//
// A discarded byte never arrives, so the bounded wait is what turns a loss into
// a failed check rather than a case that never ends.
std::string readUntil(int descriptor, std::string_view wanted)
{
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + queuedBytesPatience;
    while (received.find(wanted) == std::string::npos) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            break;
        struct pollfd readable = { descriptor, POLLIN, 0 };
        if (::poll(&readable, 1, static_cast<int>(remaining)) <= 0)
            break;
        char buffer[256];
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count <= 0)
            break;
        received.append(buffer, static_cast<std::size_t>(count));
    }
    return received;
}

int runShellAgent(int terminal, const char* script, const char* workingDirectory = "")
{
    const char* const arguments[] = { "/bin/sh", "-c", script, nullptr };
    return Ubiquitty::PAL::processSpawnWithTerminalAndWait("/bin/sh", arguments, workingDirectory, terminal, agentLimitSeconds);
}

// Every descriptor number this process has open below a ceiling far past any a
// test process reaches.
std::vector<int> openDescriptors()
{
    constexpr int ceiling = 1024;
    std::vector<int> open;
    for (int descriptor = 0; descriptor < ceiling; ++descriptor) {
        if (::fcntl(descriptor, F_GETFD) != -1)
            open.push_back(descriptor);
    }
    return open;
}

// What a setup that failed left behind, as bits of the observing child's exit status.
constexpr int findingAgentReportedRunning = 1 << 0;
constexpr int findingTerminalModeChanged = 1 << 1;
constexpr int findingMasterFlagsChanged = 1 << 2;
constexpr int findingDescriptorsLeftOpen = 1 << 3;
constexpr int findingUnobservable = 1 << 4;
constexpr int findingLimitNotEnforced = 1 << 5;

enum class ExhaustedResource {
    Descriptors,
    Processes,
};

// Runs an agent from a child process that can open no further descriptor, or
// start no further process, and returns what the failed setup left behind.
//
// A child, because the limit belongs to the process lowering it and a lowered
// one cannot always be raised back: Darwin refuses to restore a soft process
// limit above the per-user maximum, and every later case would fork into it.
//
// - Returns: The finding bits, or -1 when the child could not be run or reaped.
int findingsOfASetupThatExhausts(const SessionTerminal& terminal, ExhaustedResource resource)
{
    const pid_t child = ::fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        struct termios sessionMode;
        const int sessionFlags = ::fcntl(terminal.master.get(), F_GETFL);
        if (sessionFlags < 0 || ::tcgetattr(terminal.replica.get(), &sessionMode) != 0)
            ::_exit(findingUnobservable);

        // The next number this process would be given: every descriptor the
        // setup opens fails, and every one already open keeps working.
        const int nextDescriptor = ::open("/dev/null", O_RDONLY);
        if (nextDescriptor < 0)
            ::_exit(findingUnobservable);
        ::close(nextDescriptor);
        const std::vector<int> descriptorsBefore = openDescriptors();

        const int limitedResource = (resource == ExhaustedResource::Descriptors) ? RLIMIT_NOFILE : RLIMIT_NPROC;
        struct rlimit limit;
        if (::getrlimit(limitedResource, &limit) != 0)
            ::_exit(findingUnobservable);
        limit.rlim_cur = (resource == ExhaustedResource::Descriptors) ? static_cast<rlim_t>(nextDescriptor) : 1;
        if (::setrlimit(limitedResource, &limit) != 0)
            ::_exit(findingUnobservable);

        if (resource == ExhaustedResource::Processes) {
            // Root, or a process holding the resource capability, forks past
            // the limit, and then there is no failed fork to observe.
            const pid_t probe = ::fork();
            if (probe == 0)
                ::_exit(0);
            if (probe > 0) {
                int discarded = 0;
                ::waitpid(probe, &discarded, 0);
                ::_exit(findingLimitNotEnforced);
            }
        }

        const int status = runShellAgent(terminal.master.get(), "exit 0");
        int findings = 0;
        if (status != -1)
            findings |= findingAgentReportedRunning;
        if (openDescriptors() != descriptorsBefore)
            findings |= findingDescriptorsLeftOpen;
        struct termios modeAfter;
        if (::tcgetattr(terminal.replica.get(), &modeAfter) != 0 || !isSameMode(sessionMode, modeAfter))
            findings |= findingTerminalModeChanged;
        if (::fcntl(terminal.master.get(), F_GETFL) != sessionFlags)
            findings |= findingMasterFlagsChanged;
        ::_exit(findings);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void checkNothingLeftBehind(int findings)
{
    CHECK_MSG((findings & findingAgentReportedRunning) == 0, "an agent was reported running though its setup failed");
    CHECK_MSG((findings & findingTerminalModeChanged) == 0, "a setup that failed left the session's terminal mode changed");
    CHECK_MSG((findings & findingMasterFlagsChanged) == 0, "a setup that failed left the master's flags changed");
    CHECK_MSG((findings & findingDescriptorsLeftOpen) == 0, "a setup that failed left descriptors open");
}

} // namespace

TEST(bytesQueuedBeforeTheAgentStartsOutliveTheRun)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());
    struct termios sessionMode;
    REQUIRE(::tcgetattr(terminal.replica.get(), &sessionMode) == 0);

    // Typed into the session before the agent started, not yet read by it.
    CHECK(writeAll(terminal.master.get(), "typed ahead\n"));
    // The far side's output: a line the agent reads, and one queued behind it
    // that only the frontend reads, after the agent is gone.
    constexpr std::string_view agentLine = "for the agent\n";
    CHECK(writeAll(terminal.replica.get(), std::string(agentLine) + "trailing output\n"));

    // dd reads a byte per call, so the agent takes its line and nothing behind
    // it; a shell's own `read` buffers ahead on a descriptor it can seek, and a
    // Darwin terminal is one.
    const std::string script = "line=$(dd bs=1 count=" + std::to_string(agentLine.size()) + " 2>/dev/null) && test \"$line\" = 'for the agent'";
    const int status = runShellAgent(terminal.master.get(), script.c_str());
    CHECK_MSG(status == 0, "the agent never received the line queued for it before it started");

    CHECK_MSG(readUntil(terminal.replica.get(), "typed ahead\n").find("typed ahead\n") != std::string::npos,
        "input typed ahead of the agent was discarded");
    CHECK_MSG(readUntil(terminal.master.get(), "trailing output\n").find("trailing output\n") != std::string::npos,
        "output queued behind the agent's line was discarded");

    struct termios restoredMode;
    CHECK(::tcgetattr(terminal.replica.get(), &restoredMode) == 0);
    CHECK_MSG(isSameMode(sessionMode, restoredMode), "the session's terminal mode was not handed back");
    CHECK_MSG((::fcntl(terminal.master.get(), F_GETFL) & O_NONBLOCK) != 0,
        "the master was left blocking");
}

TEST(bytesQueuedAsTheAgentExitsOutliveTheRun)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());
    const char* replicaPath = ::ptsname(terminal.master.get());
    REQUIRE(replicaPath != nullptr);

    // The agent's last output to the far side, and output the far side wrote
    // after the agent's last read, both unread when the terminal is handed
    // back. The agent writes the second itself, through the replica, so it
    // lands after the last read without racing a separate process.
    const std::string script = std::string("printf 'agent reply\\n' && printf 'far side output\\n' > ") + replicaPath;
    CHECK(runShellAgent(terminal.master.get(), script.c_str()) == 0);

    CHECK_MSG(readUntil(terminal.replica.get(), "agent reply\n").find("agent reply\n") != std::string::npos,
        "the agent's last output was discarded before the far side read it");
    CHECK_MSG(readUntil(terminal.master.get(), "far side output\n").find("far side output\n") != std::string::npos,
        "output the far side wrote after the agent's last read was discarded");
}

TEST(aSetupThatCannotOpenADescriptorLeavesTheTerminalAsItWas)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());

    const int findings = findingsOfASetupThatExhausts(terminal, ExhaustedResource::Descriptors);
    REQUIRE_MSG(findings >= 0 && (findings & findingUnobservable) == 0,
        "the child without spare descriptors could not observe the setup");
    checkNothingLeftBehind(findings);
}

TEST(aForkThatFailsLeavesTheTerminalAndDescriptorsAsTheyWere)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());

    const int findings = findingsOfASetupThatExhausts(terminal, ExhaustedResource::Processes);
    REQUIRE_MSG(findings >= 0 && (findings & findingUnobservable) == 0,
        "the child without a spare process could not observe the setup");
    if ((findings & findingLimitNotEnforced) != 0) {
        UbiquittyTest::skip("this process is exempt from RLIMIT_NPROC, so no fork can be made to fail");
        return;
    }
    checkNothingLeftBehind(findings);
}

TEST(theAgentHoldsOnlyTheTerminalItWasGiven)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());

    // A pipe made the ordinary way carries no close-on-exec mark, which is how
    // another session's master or a daemon socket is held.
    int leakProbe[2] = { -1, -1 };
    REQUIRE(::pipe(leakProbe) == 0);
    const std::string script = "test -t 0 && test -t 1 && test ! -e /dev/fd/" + std::to_string(leakProbe[0]);
    const int status = runShellAgent(terminal.master.get(), script.c_str());
    CHECK_MSG(status == 0, "the agent can reach a descriptor of this process it was never given");
    ::close(leakProbe[0]);
    ::close(leakProbe[1]);
}

TEST(theAgentsOutcomeReachesTheCaller)
{
    SessionTerminal terminal = openSessionTerminal();
    REQUIRE(terminal.master.isValid());

    CHECK_MSG(runShellAgent(terminal.master.get(), "exit 3") == 3, "the agent's exit status was lost");

    const char* const missingAgent[] = { "/nonexistent/ubq", nullptr };
    CHECK_MSG(Ubiquitty::PAL::processSpawnWithTerminalAndWait(missingAgent[0], missingAgent, "", terminal.master.get(), agentLimitSeconds) == -1,
        "an agent that could not be executed was reported as run");
    CHECK_MSG(runShellAgent(terminal.master.get(), "exit 0", "/nonexistent/ubiquitty-directory") == -1,
        "an agent ran outside the working directory it was given");
}

UBIQUITTY_TEST_MAIN()
