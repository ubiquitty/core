// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SubprocessChannel.cpp — see SubprocessChannel.h.

#include "Core/Transport/SubprocessChannel.h"

#include "PAL/Socket.h"

namespace Ubiquitty::Transport {

SubprocessChannel::SubprocessChannel(std::string shellCommand, Transport::Origin origin)
    : PollChannel(std::move(origin))
    , m_command(std::move(shellCommand))
{
}

SubprocessChannel::~SubprocessChannel()
{
    stop(); // join the I/O thread before our fields tear down
}

bool SubprocessChannel::openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
    std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error)
{
    PAL::PipedCommand spawned = PAL::spawnPipedCommand(m_command, PAL::CommandStandardError::Capture);
    if (!spawned) {
        error = std::string("could not run: ") + m_command;
        return false;
    }
    // The two directions are genuinely different kernel objects here — one pipe
    // each way — which is why the base takes two streams rather than one.
    writeStream = std::move(spawned.input); // → child stdin
    readStream = std::move(spawned.output); // ← child stdout
    setDiagnosticStream(std::move(spawned.standardError));
    m_child = std::move(spawned.process);
    return true;
}

void SubprocessChannel::closeTransport()
{
    // The base has already closed both endpoints, which EOFs the child. Asking
    // it to exit is still necessary: a wedged peer would otherwise linger, and
    // the escalation and reap happen without blocking this thread.
    m_child.reset();
}

} // namespace Ubiquitty::Transport
