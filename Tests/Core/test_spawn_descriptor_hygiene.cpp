// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What a spawned shell command inherits.
//
// The process that spawns transports is the process holding every local
// session's PTY master: a child that inherits one can read and write that
// session. openPTY's child runs a close sweep against exactly this;
// spawnPipedCommand carried the same exposure with no mitigation, so a
// descriptor created without close-on-exec must come back absent from the
// child.

#include "Harness.h"

#include "PAL/Stream.h"

#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Reads the child's whole standard output. The stream is non-blocking by
// contract — Again asks for a retry and EndOfFile is the peer closing — so the
// drain waits out Again under a deadline a wedged child cannot outlast.
std::string drainCommand(Ubiquitty::PAL::PipedCommand& command)
{
    std::string output;
    char buffer[512];
    int patienceMilliseconds = 5000;
    while (patienceMilliseconds > 0) {
        const int got = command.output->read(std::span<char> { buffer, sizeof(buffer) });
        if (got > 0) {
            output.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got == static_cast<int>(Ubiquitty::PAL::StreamStatus::Again)) {
            ::usleep(1000);
            --patienceMilliseconds;
            continue;
        }
        break; // EndOfFile, Closed, Fatal, or a zero read all end the drain
    }
    return output;
}

} // namespace

TEST(aShellChildDoesNotInheritAnUnrelatedDescriptor)
{
    // A pipe made the ordinary way carries no close-on-exec mark, which is
    // exactly how PTY masters and daemon sockets are held.
    int leakProbe[2] = { -1, -1 };
    CHECK(::pipe(leakProbe) == 0);

    const std::string probePath = "/dev/fd/" + std::to_string(leakProbe[0]);
    Ubiquitty::PAL::PipedCommand command = Ubiquitty::PAL::spawnPipedCommand(
        "if [ -e " + probePath + " ]; then echo LEAKED; else echo CLEAN; fi");
    CHECK(static_cast<bool>(command));
    if (command) {
        const std::string said = drainCommand(command);
        CHECK_MSG(said.find("CLEAN") != std::string::npos,
            "the shell child can see a descriptor it was never given");
        CHECK_MSG(said.find("LEAKED") == std::string::npos,
            "the leak probe descriptor crossed the spawn");
    }

    ::close(leakProbe[0]);
    ::close(leakProbe[1]);
}

TEST(theSpawnedCommandStillSpeaksBothPipes)
{
    // The hygiene must not have cost the plumbing: input reaches the child and
    // its output comes back.
    Ubiquitty::PAL::PipedCommand command = Ubiquitty::PAL::spawnPipedCommand("read line; echo \"got:$line\"");
    CHECK(static_cast<bool>(command));
    if (!command)
        return;

    const std::string line = "hello across the spawn\n";
    CHECK(command.input->writeAll(std::span<const char> { line.data(), line.size() }) >= 0);
    command.input.reset(); // EOF so `read` returns even without the newline path

    const std::string said = drainCommand(command);
    CHECK_MSG(said.find("got:hello across the spawn") != std::string::npos,
        "the child's answer never arrived");
}

TEST(closingOurEndsIsEnoughToEndTheChild)
{
    // The EOF-on-close teardown the channel relies on: with no leaked
    // duplicates of the pipe ends anywhere, dropping ours is the child's EOF.
    // A child that lingers here means some other process still holds a
    // write end it was never meant to have.
    Ubiquitty::PAL::PipedCommand command = Ubiquitty::PAL::spawnPipedCommand("cat");
    CHECK(static_cast<bool>(command));
    if (!command)
        return;

    command.input.reset();
    const std::string said = drainCommand(command); // EOF promptly, nothing said
    CHECK(said.empty());
}

UBIQUITTY_TEST_MAIN()

TEST(capturedStandardErrorIsSeparateFromProtocolOutput)
{
    auto command = Ubiquitty::PAL::spawnPipedCommand("printf protocol; printf 'Permission denied' >&2", Ubiquitty::PAL::CommandStandardError::Capture);
    CHECK(static_cast<bool>(command));
    if (!command)
        return;
    CHECK(drainCommand(command) == "protocol");
    command.output = std::move(command.standardError);
    CHECK(drainCommand(command) == "Permission denied");
}
