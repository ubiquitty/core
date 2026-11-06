// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Socket.cpp — see PAL/Socket.h.

#include "PAL/Socket.h"

#include "PAL/OutputQueue.h"
#include "PAL/UnixFileDescriptor.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace Ubiquitty::PAL {

namespace {

int setNonBlocking(int fileDescriptor)
{
    const int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK);
}

// Writing to a peer that has gone away must surface as an error on this stream,
// not as a signal that kills the process. Darwin suppresses per socket at setup;
// Linux has no such option and suppresses per send instead (see sendBytes).
void suppressSignalOnBrokenPipe(int fileDescriptor)
{
#ifdef SO_NOSIGPIPE
    const int on = 1;
    (void)::setsockopt(fileDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fileDescriptor;
#endif
}

ssize_t sendBytes(int fileDescriptor, std::string_view bytes)
{
#ifdef MSG_NOSIGNAL
    // send() carries the no-signal flag, which is the only way to get it on a
    // socket per-call. It refuses anything that is not a socket, and this stream
    // also carries pipes now — so ENOTSOCK is a descriptor kind, not an error,
    // and write() is the same operation for it. A pipe cannot take the flag; the
    // process-wide disposition is what covers it.
    const ssize_t sent = ::send(fileDescriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent >= 0 || errno != ENOTSOCK)
        return sent;
#endif
    return ::write(fileDescriptor, bytes.data(), bytes.size());
}

class DescriptorStream final : public PollableByteStream {
public:
    DescriptorStream(UnixFileDescriptor socketFileDescriptor, ReadinessWaitStrategy readinessWaitStrategy)
        : m_socket(std::move(socketFileDescriptor))
        , m_readinessWaitStrategy(readinessWaitStrategy)
    {
    }

    ~DescriptorStream() override { delete m_outputQueue; }

    std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        if (!m_socket)
            return nullptr;
        // One endpoint, a second owner: a carrier that polls each direction
        // separately closes each copy exactly once, and this stream keeps its own.
        const int duplicate = ::dup(m_socket.get());
        if (duplicate < 0)
            return nullptr;
        return adoptByteStream(duplicate, m_readinessWaitStrategy);
    }

    int read(std::span<char> buffer) override
    {
        if (buffer.empty())
            return std::to_underlying(StreamStatus::Fatal);
        if (!m_socket)
            return std::to_underlying(StreamStatus::Closed);
        if (m_endOfFile)
            return std::to_underlying(StreamStatus::EndOfFile);

        const ssize_t n = ::read(m_socket.get(), buffer.data(), buffer.size());
        if (n > 0) {
            m_lastError = 0;
            return static_cast<int>(n);
        }
        if (n == 0) {
            m_endOfFile = true;
            m_lastError = 0;
            return std::to_underlying(StreamStatus::EndOfFile);
        }

        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            m_lastError = 0;
            return std::to_underlying(StreamStatus::Again);
        }
        m_lastError = err;
        return std::to_underlying(StreamStatus::Fatal);
    }

    int write(std::span<const char> bytes) override
    {
        if (!m_socket)
            return std::to_underlying(StreamStatus::Closed);
        if (bytes.empty()) {
            m_lastError = 0;
            return 0;
        }

        const ssize_t n = sendBytes(m_socket.get(), { bytes.data(), bytes.size() });
        if (n >= 0) {
            m_lastError = 0;
            return static_cast<int>(n);
        }

        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            m_lastError = 0;
            return std::to_underlying(StreamStatus::Again);
        }
        if (err == EPIPE || err == ECONNRESET) {
            m_lastError = 0;
            return std::to_underlying(StreamStatus::EndOfFile);
        }
        m_lastError = err;
        return std::to_underlying(StreamStatus::Fatal);
    }

    int writeAll(std::span<const char> bytes) override
    {
        if (!m_socket)
            return std::to_underlying(StreamStatus::Closed);
        if (bytes.empty())
            return 0;

        const std::scoped_lock guard(m_outputMutex);
        if (!m_outputQueue)
            m_outputQueue = new (std::nothrow) OutputQueue();
        if (!m_outputQueue)
            return std::to_underlying(StreamStatus::Fatal);
        if (m_outputQueue->submit(bytes, writeSink, this) < 0)
            return static_cast<int>(m_socket ? StreamStatus::Fatal : StreamStatus::Closed);
        return 0;
    }

    bool drainPendingWrite() override
    {
        const std::scoped_lock guard(m_outputMutex);
        if (!m_outputQueue)
            return false;
        (void)m_outputQueue->drain(writeSink, this);
        return m_outputQueue->hasPending();
    }

    bool hasPendingWrite() const override
    {
        const std::scoped_lock guard(m_outputMutex);
        return m_outputQueue && m_outputQueue->hasPending();
    }

    int lastError() const override { return m_lastError; }

    [[nodiscard]] FileDescriptor& handle() override { return m_socket; }

    [[nodiscard]] ReadinessWaitStrategy readinessWaitStrategy() const override { return m_readinessWaitStrategy; }

private:
    // OutputQueue's sink contract: bytes accepted, 0 when full, negative when the
    // peer is gone. A short write is backpressure, so the queue keeps the tail.
    static int writeSink(void* context, std::span<const char> bytes)
    {
        auto* self = static_cast<DescriptorStream*>(context);
        const int rc = self->write(bytes);
        if (rc >= 0)
            return rc;
        if (rc == std::to_underlying(StreamStatus::Again))
            return 0;
        return -1;
    }

    UnixFileDescriptor m_socket;
    ReadinessWaitStrategy m_readinessWaitStrategy;
    mutable std::mutex m_outputMutex; // guards m_outputQueue (sender versus poll drain)
    /* owned */ OutputQueue* m_outputQueue = nullptr; // allocated on first writeAll
    int m_lastError = 0;
    bool m_endOfFile = false;
};

LocalSocketError errorForErrno(int err)
{
    switch (err) {
    case ENOENT:
    case ECONNREFUSED:
        return LocalSocketError::NotFound;
    case EACCES:
    case EPERM:
        return LocalSocketError::PermissionDenied;
    case ENAMETOOLONG:
        return LocalSocketError::PathTooLong;
    default:
        return LocalSocketError::Failed;
    }
}

} // namespace

std::unique_ptr<PollableByteStream> connectLocal(std::string_view path, LocalSocketError* outError)
{
    const auto fail = [outError](LocalSocketError error) -> std::unique_ptr<PollableByteStream> {
        if (outError)
            *outError = error;
        return nullptr;
    };

    if (path.empty())
        return fail(LocalSocketError::Failed);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return fail(LocalSocketError::PathTooLong);
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';

    UnixFileDescriptor socketFileDescriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socketFileDescriptor)
        return fail(errorForErrno(errno));
    suppressSignalOnBrokenPipe(socketFileDescriptor.get());

    // Connect while still blocking: an AF_UNIX connect either completes or fails
    // at once, so there is no in-progress state for a caller to poll for, and
    // this keeps the migrated callers' behaviour identical.
    if (::connect(socketFileDescriptor.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        return fail(errorForErrno(errno));
    if (setNonBlocking(socketFileDescriptor.get()) < 0)
        return fail(LocalSocketError::Failed);

    auto stream = std::unique_ptr<PollableByteStream>(new (std::nothrow) DescriptorStream(std::move(socketFileDescriptor), ReadinessWaitStrategy::WaitFirst));
    if (!stream)
        return fail(LocalSocketError::Failed);
    if (outError)
        *outError = LocalSocketError::None;
    return stream;
}

bool localSocketsAvailable() { return true; }

const char* describeLocalSocketError(LocalSocketError reason)
{
    switch (reason) {
    case LocalSocketError::None:
        return "no error";
    case LocalSocketError::NotSupported:
        return "local sockets are not available on this platform";
    case LocalSocketError::PathTooLong:
        return "the socket path is longer than the platform allows";
    case LocalSocketError::PermissionDenied:
        return "permission denied";
    case LocalSocketError::NotFound:
        return "nothing is listening on that name";
    case LocalSocketError::Failed:
        return "the connection failed";
    }
    return "the connection failed";
}

std::unique_ptr<PollableByteStream> adoptByteStream(int fileDescriptor, ReadinessWaitStrategy readinessWaitStrategy)
{
    UnixFileDescriptor owned(fileDescriptor);
    if (!owned)
        return nullptr;
    // Non-blocking on the way in, so an adopted descriptor is indistinguishable
    // from one this layer opened. A consumer that had to ask would be one call
    // away from a blocking read on a descriptor it assumed was not.
    if (setNonBlocking(owned.get()) != 0)
        return nullptr;
    return std::make_unique<DescriptorStream>(std::move(owned), readinessWaitStrategy);
}

std::optional<PeerIdentity> peerIdentity(const FileDescriptor& socket)
{
    if (!socket.isValid())
        return std::nullopt;
    const int descriptor = static_cast<const UnixFileDescriptor&>(socket).get();
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    // The BSD-origin call, present on every BSD and Darwin.
    uid_t user = 0;
    gid_t group = 0;
    if (::getpeereid(descriptor, &user, &group) != 0)
        return std::nullopt;
#elif defined(__linux__)
    // glibc has no getpeereid; SO_PEERCRED reports the same three facts.
    struct ucred credentials {};
    socklen_t credentialsLength = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentialsLength) != 0)
        return std::nullopt;
    const uid_t user = credentials.uid;
    const gid_t group = credentials.gid;
#else
    // An unknown platform reports nothing rather than guessing, and the caller
    // is documented to read that as a refusal.
    (void)descriptor;
    return std::nullopt;
#endif
    return PeerIdentity { static_cast<uint32_t>(user), static_cast<uint32_t>(group) };
}

bool peerIsSameUser(const FileDescriptor& socket)
{
    const std::optional<PeerIdentity> identity = peerIdentity(socket);
    return identity && identity->userID == static_cast<uint32_t>(::geteuid());
}

SocketReadOutcome readSocket(int socket, std::span<char> into, std::size_t* outBytes)
{
    const ssize_t received = ::read(socket, into.data(), into.size());
    if (received > 0) {
        if (outBytes)
            *outBytes = static_cast<std::size_t>(received);
        return SocketReadOutcome::Bytes;
    }
    if (received == 0)
        return SocketReadOutcome::PeerClosed;
    if (errno == EINTR)
        return SocketReadOutcome::Interrupted;
    // A receive bound that elapses surfaces as one of these rather than as its
    // own code, which is why the timeout and a dead socket are told apart here
    // and not by the caller.
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return SocketReadOutcome::TimedOut;
    return SocketReadOutcome::Failed;
}

bool writeSocketAll(int socket, std::span<const char> bytes)
{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    // Where the flag is per-socket rather than per-call (Darwin), set it here:
    // this is the only place this file writes to a socket, so the two spellings
    // cover the same ground.
    const int suppressSignal = 1;
    (void)::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &suppressSignal, sizeof(suppressSignal));
#endif
#if defined(MSG_NOSIGNAL)
    constexpr int sendFlags = MSG_NOSIGNAL;
#else
    constexpr int sendFlags = 0;
#endif
    std::size_t written = 0;
    while (written < bytes.size()) {
        // send() rather than write(), for the flag: a peer that vanished
        // mid-reply otherwise raises SIGPIPE, whose default action kills the
        // process. The daemon survives that only because it ignores the signal
        // globally, which is a property of one embedder rather than of this
        // function, and a bounded server is a thing other programs embed.
        const ssize_t sent = ::send(socket, bytes.data() + written, bytes.size() - written, sendFlags);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void shutdownSocket(int socket)
{
    ::shutdown(socket, SHUT_RDWR);
}

SocketAcceptOutcome acceptConnection(const UnixFileDescriptor& listener, UnixFileDescriptor* outClient)
{
    if (setNonBlocking(listener.get()) != 0)
        return SocketAcceptOutcome::Unusable;
    UnixFileDescriptor client(::accept(listener.get(), nullptr, nullptr));
    if (client) {
        const int flags = ::fcntl(client.get(), F_GETFL, 0);
        if (flags < 0 || ((flags & O_NONBLOCK) && ::fcntl(client.get(), F_SETFL, flags & ~O_NONBLOCK) != 0))
            return SocketAcceptOutcome::Retry;
        if (outClient)
            *outClient = std::move(client);
        return SocketAcceptOutcome::Connected;
    }
    switch (errno) {
    case EINTR:
        return SocketAcceptOutcome::Interrupted;
    case EBADF:
    case EINVAL:
    case ENOTSOCK:
    case EOPNOTSUPP:
        return SocketAcceptOutcome::Unusable;
    default:
        // A failed pending connection or exhausted resources do not invalidate
        // the listener. The caller backs off so persistent pressure cannot spin.
        return SocketAcceptOutcome::Retry;
    }
}

namespace {

// The one conversion and the one syscall, so a bound in seconds and a bound in
// microseconds cannot disagree about what zero means or how a remainder rounds.
bool setSocketTimeout(int socket, int option, std::chrono::microseconds duration)
{
    struct timeval timeout {};
    if (duration.count() > 0) {
        timeout.tv_sec = static_cast<time_t>(duration.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(duration.count() % 1000000);
    }
    return ::setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout)) == 0;
}

} // namespace

bool setReceiveTimeout(int socket, std::chrono::microseconds timeout) { return setSocketTimeout(socket, SO_RCVTIMEO, timeout); }

bool setSendTimeout(int socket, std::chrono::microseconds timeout) { return setSocketTimeout(socket, SO_SNDTIMEO, timeout); }

bool setReceiveTimeoutSeconds(int socket, int seconds) { return setReceiveTimeout(socket, std::chrono::seconds(seconds)); }

bool setSendTimeoutSeconds(int socket, int seconds) { return setSendTimeout(socket, std::chrono::seconds(seconds)); }

} // namespace Ubiquitty::PAL
