// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel over an already-opened local PTY Stream (the local-origin carrier).
// The caller opens the PTY and hands the Stream over; the channel duplicates the
// duplex master fd for the poll loop and keeps the Stream for window-size + teardown.

#include "Core/Transport/PollChannel.h"
#include "PAL/Stream.h"

#include <memory>
#include <string>

namespace Ubiquitty::Transport {

class LocalPTYChannel final : public PollChannel {
public:
    // `stream` is an open PTY (fileDescriptor() >= 0); the channel owns it.
    explicit LocalPTYChannel(std::unique_ptr<PAL::Stream> stream);
    ~LocalPTYChannel() override;

    std::string description() const override { return "local pty"; }

    // Forwards to the PTY (TIOCSWINSZ through the Stream). Session thread,
    // like every non-Channel method.
    void setWindowSize(int columns, int rows);

protected:
    bool openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
        std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error) override;
    void closeTransport() override;

private:
    std::unique_ptr<PAL::Stream> m_stream;
};

} // namespace Ubiquitty::Transport
