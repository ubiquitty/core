// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel over `/bin/sh -c "<command>"`: the "ssh is the network" daemon carrier
// (<command> bridges to a ubiquitty-host daemon). Raw bytes ride the child's
// stdin/stdout; bounded stderr diagnostics accompany transport failures. Poll loop in PollChannel.

#include "Core/Transport/PollChannel.h"

#include <string>

namespace Ubiquitty::Transport {

class SubprocessChannel final : public PollChannel {
public:
    // SECURITY: `shellCommand` runs as `/bin/sh -c <command>`, so it must be
    // app-trusted. Never interpolate untrusted values unescaped — build argv
    // separately or allow-list any interpolated component at the call site.
    /// Creates a carrier that runs a command and speaks over its pipes.
    ///
    /// - Parameters:
    ///   - shellCommand: The command to run.
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    SubprocessChannel(std::string shellCommand, Transport::Origin);
    ~SubprocessChannel() override;

    std::string description() const override { return m_command; }

protected:
    bool openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
        std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error) override;
    void closeTransport() override; // SIGTERM + reap the /bin/sh wrapper

private:
    std::string m_command;
    /// The spawned peer. Destroying it terminates and reaps, so teardown order
    /// alone is what stops a child outliving its transport.
    std::unique_ptr<PAL::ChildProcess> m_child;
};

} // namespace Ubiquitty::Transport
