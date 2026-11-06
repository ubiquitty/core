// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Poll.cpp — see PAL/Poll.h.

#include "PAL/Poll.h"

#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <utility>
#include <vector>

namespace Ubiquitty::PAL {

namespace {

// FileDescriptor's only implementation in a POSIX build is UnixFileDescriptor
// (the same "one concrete subclass per build" contract PTYStream relies on for
// duplicateAsPollableStream), so this recovers the descriptor without RTTI.
int rawDescriptor(FileDescriptor* handle)
{
    if (!handle || !handle->isValid())
        return -1;
    return static_cast<UnixFileDescriptor*>(handle)->get();
}

short requestedEvents(Ready interest)
{
    short events = 0;
    if (contains(interest, Ready::Readable))
        events |= POLLIN;
    if (contains(interest, Ready::Writable))
        events |= POLLOUT;
    return events;
}

Ready reportedReady(short revents)
{
    Ready ready = Ready::None;
    if (revents & POLLIN)
        ready = ready | Ready::Readable;
    if (revents & POLLOUT)
        ready = ready | Ready::Writable;
    if (revents & POLLERR)
        ready = ready | Ready::Error;
    if (revents & POLLHUP)
        ready = ready | Ready::HangUp;
    if (revents & POLLNVAL)
        ready = ready | Ready::Invalid;
    return ready;
}

int setNonBlocking(int fileDescriptor)
{
    const int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK);
}

// A pipe rather than an eventfd: eventfd is Linux-only, and the wake is one
// syscall either way on a path that already costs a queue push.
class PipeWaker final : public Waker {
public:
    explicit PipeWaker(UnixPipe pipe)
        : m_pipe(std::move(pipe))
    {
    }

    void wake() override
    {
        const char one = 1;
        // A full pipe means a wake is already pending and undrained, which is
        // exactly the coalescing this promises — so EAGAIN is success.
        (void)!::write(m_pipe.write.get(), &one, 1);
    }

    void drain() override
    {
        char discard[64];
        while (::read(m_pipe.read.get(), discard, sizeof(discard)) > 0) {
        }
    }

    [[nodiscard]] FileDescriptor& handle() override { return m_pipe.read; }

private:
    UnixPipe m_pipe;
};

} // namespace

int waitForReadiness(std::span<Watch> watches, int timeoutMilliseconds)
{
    if (watches.empty())
        return 0;

    // Small enough to cover a channel's carrier plus its waker without touching
    // the allocator on every loop iteration.
    constexpr size_t inlineCapacity = 8;
    pollfd inlineDescriptors[inlineCapacity];
    std::vector<pollfd> heapDescriptors;
    pollfd* descriptors = inlineDescriptors;
    if (watches.size() > inlineCapacity) {
        heapDescriptors.resize(watches.size());
        descriptors = heapDescriptors.data();
    }

    // A dead handle is ready forever. Reporting it while still parking on the
    // live ones would block with a ready entry outstanding, so an invalid watch
    // demotes this call to a poll.
    bool anyInvalid = false;
    for (size_t i = 0; i < watches.size(); ++i) {
        watches[i].result = Ready::None;
        const int fileDescriptor = rawDescriptor(watches[i].handle);
        if (fileDescriptor < 0) {
            watches[i].result = Ready::Invalid;
            anyInvalid = true;
        }
        descriptors[i] = { fileDescriptor, requestedEvents(watches[i].interest), 0 };
    }

    const int rc = ::poll(descriptors, static_cast<nfds_t>(watches.size()), anyInvalid ? 0 : timeoutMilliseconds);
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        return std::to_underlying(StreamStatus::Fatal);
    }

    int ready = 0;
    for (size_t i = 0; i < watches.size(); ++i) {
        if (descriptors[i].revents)
            watches[i].result = watches[i].result | reportedReady(descriptors[i].revents);
        if (watches[i].result != Ready::None)
            ++ready;
    }
    return ready;
}

std::unique_ptr<Waker> makeWaker()
{
    UnixPipe pipe = makeUnixPipe();
    if (!pipe)
        return nullptr;
    // Both ends: a blocking read would park the loop thread inside drain(), and a
    // blocking write would park a sender once the pipe filled.
    if (setNonBlocking(pipe.read.get()) < 0 || setNonBlocking(pipe.write.get()) < 0)
        return nullptr;
    return std::unique_ptr<Waker>(new (std::nothrow) PipeWaker(std::move(pipe)));
}

} // namespace Ubiquitty::PAL
