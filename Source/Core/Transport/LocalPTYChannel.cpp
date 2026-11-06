// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Transport/LocalPTYChannel.h"

namespace Ubiquitty::Transport {

LocalPTYChannel::LocalPTYChannel(std::unique_ptr<PAL::Stream> stream)
    // A PTY carries bytes with no framing over them, so a wedged child is not a
    // reason to end the session — it is a reason to make the writer wait, which
    // is what a real terminal does.
    : PollChannel(Transport::Origin { Transport::OriginKind::Local, std::string() }, OverflowPolicy::RefuseWrite)
    , m_stream(std::move(stream))
{
}

LocalPTYChannel::~LocalPTYChannel()
{
    stop();
}

bool LocalPTYChannel::openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
    std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error)
{
    // A PTY master is one endpoint read and written both ways, so both directions
    // are streams over the same descriptor. Duplicated rather than shared so the
    // base can close each exactly once without knowing that.
    if (!m_stream) {
        error = "no pty stream";
        return false;
    }
    readStream = m_stream->duplicateAsPollableStream();
    writeStream = m_stream->duplicateAsPollableStream();
    if (!readStream || !writeStream) {
        error = "stream offers no poll endpoints";
        readStream.reset();
        writeStream.reset();
        return false;
    }
    return true;
}

void LocalPTYChannel::closeTransport()
{
    // The base has closed its duplicates and joined the I/O thread; closing
    // the original master ends the child's terminal.
    m_stream.reset();
}

void LocalPTYChannel::setWindowSize(int columns, int rows)
{
    if (m_stream)
        m_stream->setWindowSize(columns, rows, 0, 0);
}

} // namespace Ubiquitty::Transport
