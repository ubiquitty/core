// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/SessionHandoff.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Session/AdoptTransactionWire.h"
#include "Core/Session/SessionShare.h"
#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/POSIX/ReceivedDescriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <poll.h>
#include <span>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace Ubiquitty::Session {

namespace {

namespace Local = Protocol::Local;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr std::uint64_t selectionCorrelation = 1;
constexpr std::uint64_t firstRecordCorrelation = 2;
constexpr std::uint64_t secondRecordCorrelation = 3;
constexpr std::size_t mebibyte = 1024 * 1024;
constexpr std::size_t maxBytesPerWrite = 256 * 1024;

#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif

bool areLimitsValid(const ShareHandoffLimits& limits)
{
    return limits.maxRecordBytes > Local::adoptWirePrepareHeaderBytes
        && static_cast<std::uint64_t>(limits.maxRecordBytes) <= std::uint64_t { std::numeric_limits<std::uint32_t>::max() } + Local::adoptWireFrameHeaderBytes
        && limits.silenceMicroseconds && limits.resolutionAttempts;
}

std::optional<Deadline> deadlineAfter(std::uint64_t microseconds)
{
    constexpr auto maxMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(Clock::duration::max()).count();
    if (!microseconds || microseconds > static_cast<std::uint64_t>(maxMicroseconds))
        return std::nullopt;
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(static_cast<std::int64_t>(microseconds)));
    const auto now = Clock::now();
    if (Deadline::max() - interval < now)
        return std::nullopt;
    return now + interval;
}

std::optional<Deadline> preparationDeadline(const ShareHandoffLimits& limits, std::size_t bodyBytes)
{
    const auto wholeMebibytes = static_cast<std::uint64_t>(bodyBytes / mebibyte);
    if (wholeMebibytes && limits.applyMicrosecondsPerMebibyte > (std::numeric_limits<std::uint64_t>::max() - limits.silenceMicroseconds) / wholeMebibytes)
        return std::nullopt;
    return deadlineAfter(limits.silenceMicroseconds + wholeMebibytes * limits.applyMicrosecondsPerMebibyte);
}

std::optional<std::chrono::microseconds> remainingTime(Deadline deadline)
{
    const auto now = Clock::now();
    if (deadline <= now)
        return std::nullopt;
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    if (remaining.count() <= 0)
        return std::nullopt;
    return remaining;
}

bool applySendBudget(int socket, Deadline deadline, const ShareHandoffLimits& limits)
{
    const auto remaining = remainingTime(deadline);
    if (!remaining)
        return false;
    const auto silence = std::chrono::microseconds(static_cast<std::int64_t>(std::min<std::uint64_t>(limits.silenceMicroseconds,
        static_cast<std::uint64_t>(remaining->count()))));
    return PAL::setSendTimeout(socket, silence);
}

// Waits until `events` are ready on `socket` or `deadline` passes. A readiness
// that includes hang-up or error still answers true: the following syscall
// reports what the peer left, including bytes buffered before end-of-file.
bool waitForReadiness(int socket, short events, Deadline deadline)
{
    for (;;) {
        const auto remaining = remainingTime(deadline);
        if (!remaining)
            return false;
        const auto milliseconds = (remaining->count() + 999) / 1000;
        struct pollfd descriptor { .fd = socket, .events = events, .revents = 0 };
        const auto result = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(milliseconds, std::numeric_limits<int>::max())));
        if (result < 0 && errno == EINTR)
            continue;
        return result > 0 && !(descriptor.revents & POLLNVAL);
    }
}

// Silence is counted from the last byte the socket took, and the wait is
// poll's. A send timeout bounds one call: a call that took bytes and then
// waited out the timeout returns its partial count, and the next call's
// wait began the interval again, so a peer that had stopped reading was
// waited on for two or three intervals.
bool sendBytes(int socket, std::span<const std::uint8_t> bytes, Deadline deadline, const ShareHandoffLimits& limits)
{
    const auto silence = std::chrono::microseconds(static_cast<std::int64_t>(std::min<std::uint64_t>(limits.silenceMicroseconds, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));
    auto lastProgress = Clock::now();
    while (!bytes.empty()) {
        if (!waitForReadiness(socket, POLLOUT, std::min(deadline, lastProgress + silence)))
            return false;
        const auto written = ::send(socket, bytes.data(), std::min(bytes.size(), maxBytesPerWrite), sendFlags | MSG_DONTWAIT);
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(written));
        lastProgress = Clock::now();
    }
    return true;
}

// The deadline is honoured by poll, never by SO_RCVTIMEO. On macOS, setting a
// receive timeout on an AF_UNIX stream whose peer has closed fails with EINVAL
// while the peer's last record is still buffered, so a timeout armed before
// each read would discard a reply the daemon wrote just before closing.
bool receiveBytes(int socket, std::span<std::uint8_t> bytes, Deadline deadline)
{
    while (!bytes.empty()) {
        if (!waitForReadiness(socket, POLLIN, deadline))
            return false;
        alignas(cmsghdr) char control[POSIX::receivedDescriptorControlCapacity];
        struct iovec vector { .iov_base = bytes.data(), .iov_len = bytes.size() };
        struct msghdr message {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        // Non-blocking, so a readiness that no longer holds returns to the
        // deadline-bounded wait rather than blocking past it.
        const auto received = ::recvmsg(socket, &message, POSIX::receivedDescriptorFlags | MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0)
            return false;
        auto descriptor = POSIX::takeReceivedDescriptor(message);
        if (!descriptor || *descriptor || received == 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(received));
    }
    return true;
}

bool waitForConnect(int socket, Deadline deadline)
{
    if (!waitForReadiness(socket, POLLOUT, deadline))
        return false;
    int socketError = 0;
    socklen_t length = sizeof(socketError);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 && !socketError;
}

struct SelectedChannel {
    PAL::UnixFileDescriptor socket;
    std::uint64_t daemonInstanceID = 0;
};

std::optional<SelectedChannel> selectChannel(const char* socketPath, std::uint64_t expectedDaemonInstanceID, Deadline deadline, const ShareHandoffLimits& limits)
{
    if (!socketPath || !*socketPath)
        return std::nullopt;
    struct sockaddr_un address {};
    if (std::strlen(socketPath) >= sizeof(address.sun_path))
        return std::nullopt;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath, std::strlen(socketPath) + 1);
    PAL::UnixFileDescriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket || ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) < 0)
        return std::nullopt;
#ifdef SO_NOSIGPIPE
    const int suppressSignal = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &suppressSignal, sizeof(suppressSignal)) != 0)
        return std::nullopt;
#endif
    const auto flags = ::fcntl(socket.get(), F_GETFL);
    if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0)
        return std::nullopt;
    const auto connected = ::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected < 0 && (errno != EINPROGRESS && errno != EAGAIN && errno != EINTR))
        return std::nullopt;
    if ((connected < 0 && !waitForConnect(socket.get(), deadline)) || ::fcntl(socket.get(), F_SETFL, flags) < 0
        || !PAL::peerIsSameUser(socket))
        return std::nullopt;
    const auto request = Local::encodeLocalHandoffRequest(selectionCorrelation);
    if (!request || !sendBytes(socket.get(), *request, deadline, limits))
        return std::nullopt;
    std::array<std::uint8_t, Local::localControlReplyBytes> bytes {};
    if (!receiveBytes(socket.get(), bytes, deadline))
        return std::nullopt;
    const auto ready = Local::decodeLocalHandoffReady(bytes);
    if (!ready || ready->correlation != selectionCorrelation || ready->status != Local::HandoffReadyStatus::Ready
        || !ready->daemonInstanceID || (expectedDaemonInstanceID && ready->daemonInstanceID != expectedDaemonInstanceID))
        return std::nullopt;
    return SelectedChannel { .socket = std::move(socket), .daemonInstanceID = ready->daemonInstanceID };
}

bool sendPrepare(int socket, const Local::EncodedAdoptWirePrepare& prepare, int masterFileDescriptor, Deadline deadline, const ShareHandoffLimits& limits)
{
    if (masterFileDescriptor < 0 || !applySendBudget(socket, deadline, limits))
        return false;
    // One byte binds ancillary ownership to Prepare's actual record boundary.
    struct iovec vector { .iov_base = const_cast<std::uint8_t*>(prepare.header.data()), .iov_len = 1 };
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr alignment;
    } control {};
    struct msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    auto* const header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &masterFileDescriptor, sizeof(int));
    ssize_t sent = 0;
    do {
        if (!applySendBudget(socket, deadline, limits))
            return false;
        sent = ::sendmsg(socket, &message, sendFlags);
    } while (sent < 0 && errno == EINTR);
    return sent == 1 && sendBytes(socket, std::span { prepare.header }.subspan(1), deadline, limits)
        && sendBytes(socket, prepare.terminalBody, deadline, limits);
}

bool observeReply(int socket, AdoptClientAttempt& attempt, std::uint64_t correlation, Deadline deadline)
{
    std::array<std::uint8_t, Local::adoptWireOutcomeBytes> bytes {};
    std::span<std::uint8_t> record { bytes };
    if (!receiveBytes(socket, record.first(Local::adoptWireFrameHeaderBytes), deadline))
        return false;
    const auto header = Local::decodeAdoptWireHeader(record.first(Local::adoptWireFrameHeaderBytes), bytes.size());
    if (!header || (header->kind != Local::AdoptWireMessage::Outcome && header->kind != Local::AdoptWireMessage::Refused))
        return false;
    record = record.first(Local::adoptWireFrameHeaderBytes + header->bodyBytes);
    if (!receiveBytes(socket, record.subspan(Local::adoptWireFrameHeaderBytes), deadline))
        return false;
    if (header->kind == Local::AdoptWireMessage::Refused) {
        const auto refused = Local::decodeAdoptWireRefused(record);
        if (refused)
            (void)adoptRefusedFromWire(*refused, attempt.operation(), correlation);
        return false;
    }
    const auto wire = Local::decodeAdoptWireOutcome(record);
    if (!wire)
        return false;
    const auto outcome = adoptOutcomeFromWire(*wire, attempt.operation(), correlation);
    return outcome && attempt.observe(*outcome).has_value();
}

bool exchangeCommand(int socket, AdoptClientAttempt& attempt, Local::AdoptWireMessage kind, std::uint64_t correlation,
    Deadline deadline, const ShareHandoffLimits& limits)
{
    const auto operation = adoptOperationToWire(attempt.operation());
    if (!operation)
        return false;
    const auto command = Local::encodeAdoptWireCommand({ .kind = kind, .correlation = correlation, .operation = *operation });
    return command && sendBytes(socket, *command, deadline, limits) && observeReply(socket, attempt, correlation, deadline);
}

ShareHandoffResult ownershipResult(std::unique_ptr<AdoptClientAttempt> attempt)
{
    ShareHandoffResult result;
    if (attempt && attempt->isCommitted()) {
        result.ownership = ShareOwnership::Committed;
        result.adopted = { .sessionID = attempt->target().sessionID, .daemonInstanceID = attempt->operation().daemonInstanceID };
    } else if (attempt && !attempt->mayResumeLocal()) {
        result.ownership = ShareOwnership::Unresolved;
    }
    result.attempt = std::move(attempt);
    return result;
}

} // namespace

ShareHandoffResult reHomeViaAdopt(const char* socketPath, std::span<const std::byte> terminalBody, int masterFileDescriptor, const ShareHandoffLimits& limits)
{
    if (!areLimitsValid(limits) || terminalBody.empty() || masterFileDescriptor < 0
        || terminalBody.size() > limits.maxRecordBytes - Local::adoptWirePrepareHeaderBytes)
        return {};
    const auto selectionDeadline = deadlineAfter(limits.silenceMicroseconds);
    if (!selectionDeadline)
        return {};
    auto channel = selectChannel(socketPath, 0, *selectionDeadline, limits);
    if (!channel)
        return {};
    Protocol::Sync::ID128 operationID;
    if (::getentropy(&operationID, sizeof(operationID)) != 0 || operationID == Protocol::Sync::ID128 {})
        return {};
    auto attempt = std::unique_ptr<AdoptClientAttempt>(new (std::nothrow) AdoptClientAttempt({ operationID, channel->daemonInstanceID }));
    if (!attempt)
        return {};
    const auto operation = adoptOperationToWire(attempt->operation());
    if (!operation)
        return ownershipResult(std::move(attempt));
    // Both views borrow the same immutable object representation until sending ends.
    const auto body = std::span(reinterpret_cast<const std::uint8_t*>(terminalBody.data()), terminalBody.size());
    Local::AdoptWirePrepare request { .correlation = firstRecordCorrelation,
        .preparation = { .operation = *operation, .bodyBytes = body.size() },
        .terminalBody = body };
    Crypto::sha256(body, request.preparation.bodySHA256);
    const auto encoded = Local::encodeAdoptWirePrepare(request, limits.maxRecordBytes);
    const auto sendingDeadline = preparationDeadline(limits, body.size());
    if (!encoded || !sendingDeadline || !sendPrepare(channel->socket.get(), *encoded, masterFileDescriptor, *sendingDeadline, limits))
        return ownershipResult(std::move(attempt));
    const auto preparedDeadline = preparationDeadline(limits, body.size());
    if (!preparedDeadline || !observeReply(channel->socket.get(), *attempt, firstRecordCorrelation, *preparedDeadline))
        return ownershipResult(std::move(attempt));
    const auto commitDeadline = deadlineAfter(limits.silenceMicroseconds);
    if (!commitDeadline || !attempt->beginCommit())
        return ownershipResult(std::move(attempt));
    // Uncertainty is recorded before exchangeCommand can write even one byte.
    (void)exchangeCommand(channel->socket.get(), *attempt, Local::AdoptWireMessage::Commit, secondRecordCorrelation, *commitDeadline, limits);
    channel.reset();
    return resolveAdopt(socketPath, std::move(attempt), limits);
}

ShareHandoffResult resolveAdopt(const char* socketPath, std::unique_ptr<AdoptClientAttempt> attempt, const ShareHandoffLimits& limits)
{
    if (!attempt || attempt->mayResumeLocal() || attempt->isCommitted() || !areLimitsValid(limits))
        return ownershipResult(std::move(attempt));
    for (std::size_t index = 0; index < limits.resolutionAttempts; ++index) {
        const auto deadline = deadlineAfter(limits.silenceMicroseconds);
        if (!deadline)
            break;
        auto channel = selectChannel(socketPath, attempt->operation().daemonInstanceID, *deadline, limits);
        if (!channel)
            continue;
        if (!exchangeCommand(channel->socket.get(), *attempt, Local::AdoptWireMessage::Status, firstRecordCorrelation, *deadline, limits))
            continue;
        if (attempt->mayResumeLocal() || attempt->isCommitted())
            break;
        (void)exchangeCommand(channel->socket.get(), *attempt, Local::AdoptWireMessage::Abort, secondRecordCorrelation, *deadline, limits);
        if (attempt->mayResumeLocal() || attempt->isCommitted())
            break;
    }
    return ownershipResult(std::move(attempt));
}

std::uintptr_t adoptHandoffAddress()
{
    const AdoptHandoff handoff = &reHomeViaAdopt;
    return reinterpret_cast<std::uintptr_t>(handoff);
}

std::uintptr_t resolveHandoffAddress()
{
    const AdoptResolve resolve = &resolveAdopt;
    return reinterpret_cast<std::uintptr_t>(resolve);
}

} // namespace Ubiquitty::Session
