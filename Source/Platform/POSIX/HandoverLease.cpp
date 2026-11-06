// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/HandoverLease.h"

#include "Platform/POSIX/HandoverTransport.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Ubiquitty::POSIX {
namespace {

using Error = HandoverLeaseError;
constexpr std::size_t metadataSize = 64;
constexpr std::uint64_t metadataMagic = 0x455341454c514255;
constexpr std::uint32_t metadataVersion = 1;
enum class ClaimState : std::uint32_t {
    Unbound,
    Pending,
    Claimed,
    Cancelled
};

struct Metadata {
    ClaimState state = ClaimState::Unbound;
    pid_t predecessor = 0;
    HandoverLeaseClaim claim;
};

std::uint64_t checksum(std::span<const std::uint8_t> bytes)
{
    std::uint64_t result = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::array<std::uint8_t, metadataSize> encode(const Metadata& metadata)
{
    std::array<std::uint8_t, metadataSize> bytes {};
    std::size_t offset = 0;
    const auto scalar = [&]<typename T>(T value) {
        for (std::size_t index = 0; index < sizeof(T); ++index)
            bytes[offset++] = static_cast<std::uint8_t>(value >> (index * 8));
    };
    scalar(metadataMagic);
    scalar(metadataVersion);
    scalar(std::to_underlying(metadata.state));
    scalar(static_cast<std::uint64_t>(metadata.predecessor));
    scalar(static_cast<std::uint64_t>(metadata.claim.successor));
    scalar(metadata.claim.requester.low);
    scalar(metadata.claim.requester.high);
    scalar(std::uint64_t { 0 });
    scalar(checksum(std::span(bytes).first(metadataSize - 8)));
    return bytes;
}

std::expected<Metadata, Error> readMetadata(int descriptor)
{
    std::array<std::uint8_t, metadataSize> bytes {};
    ssize_t count;
    do {
        count = ::pread(descriptor, bytes.data(), bytes.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count != metadataSize)
        return std::unexpected(Error::InvalidMetadata);
    std::size_t offset = 0;
    const auto scalar = [&]<typename T>() {
        T value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index)
            value |= static_cast<T>(bytes[offset++]) << (index * 8);
        return value;
    };
    if (scalar.template operator()<std::uint64_t>() != metadataMagic
        || scalar.template operator()<std::uint32_t>() != metadataVersion)
        return std::unexpected(Error::InvalidMetadata);
    const auto state = scalar.template operator()<std::uint32_t>();
    const auto predecessor = scalar.template operator()<std::uint64_t>();
    const auto successor = scalar.template operator()<std::uint64_t>();
    const Protocol::Sync::ID128 requester { scalar.template operator()<std::uint64_t>(), scalar.template operator()<std::uint64_t>() };
    const auto reserved = scalar.template operator()<std::uint64_t>();
    const auto digest = scalar.template operator()<std::uint64_t>();
    if (state > std::to_underlying(ClaimState::Cancelled) || !predecessor || predecessor > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
        || successor > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) || reserved
        || digest != checksum(std::span(bytes).first(metadataSize - 8))
        || (state == std::to_underlying(ClaimState::Unbound) ? successor || requester != Protocol::Sync::ID128 {} : !successor || successor == predecessor || requester == Protocol::Sync::ID128 {}))
        return std::unexpected(Error::InvalidMetadata);
    return Metadata { static_cast<ClaimState>(state), static_cast<pid_t>(predecessor), { static_cast<pid_t>(successor), requester } };
}

std::expected<void, Error> writeMetadata(int descriptor, const Metadata& metadata)
{
    const auto bytes = encode(metadata);
    ssize_t count;
    do {
        count = ::pwrite(descriptor, bytes.data(), bytes.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count != metadataSize)
        return std::unexpected(Error::SystemFailure);
    return {};
}

struct flock wholeFileLock(short type)
{
    struct flock lock {};
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    return lock;
}

bool isSafeDescriptor(int descriptor, std::size_t expectedSize = metadataSize)
{
    struct stat facts {};
    if (::fstat(descriptor, &facts) != 0 || !S_ISREG(facts.st_mode)
        || facts.st_uid != ::geteuid() || facts.st_nlink || facts.st_size != static_cast<off_t>(expectedSize)
        || (facts.st_mode & 07777) != 0600)
        return false;
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    const int statusFlags = ::fcntl(descriptor, F_GETFL);
    return descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC)
        && statusFlags >= 0 && (statusFlags & O_ACCMODE) == O_RDWR;
}

} // namespace

HandoverLease::HandoverLease(PAL::UnixFileDescriptor descriptor)
    : m_descriptor(std::move(descriptor))
    , m_process(::getpid())
{
}

std::expected<HandoverLease, HandoverLeaseError> HandoverLease::create(int privateDirectory)
{
    struct stat directoryFacts {};
    if (::fstat(privateDirectory, &directoryFacts) != 0 || !S_ISDIR(directoryFacts.st_mode)
        || directoryFacts.st_uid != ::geteuid() || (directoryFacts.st_mode & 07777) != 0700)
        return std::unexpected(HandoverLeaseError::UnsafeDescriptor);

    constexpr unsigned maxCreationAttempts = 8;
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < maxCreationAttempts; ++attempt) {
        std::array<std::uint8_t, 16> random {};
        if (::getentropy(random.data(), random.size()) != 0)
            return std::unexpected(HandoverLeaseError::SystemFailure);
        std::array<char, 33> leaf {};
        for (std::size_t index = 0; index < random.size(); ++index) {
            leaf[index * 2] = hexadecimal[random[index] >> 4];
            leaf[index * 2 + 1] = hexadecimal[random[index] & 15];
        }
        PAL::UnixFileDescriptor descriptor(::openat(privateDirectory, leaf.data(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!descriptor) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(HandoverLeaseError::SystemFailure);
        }
        // Unlink before acquiring: no pathname can create another local owner.
        if (::unlinkat(privateDirectory, leaf.data(), 0) != 0)
            return std::unexpected(HandoverLeaseError::SystemFailure);
        if (::fchmod(descriptor.get(), 0600) != 0 || !isSafeDescriptor(descriptor.get(), 0))
            return std::unexpected(HandoverLeaseError::UnsafeDescriptor);
        HandoverLease lease(std::move(descriptor));
        auto lock = wholeFileLock(F_WRLCK);
        if (::fcntl(lease.m_descriptor.get(), F_SETLK, &lock) != 0)
            return std::unexpected(Error::SystemFailure);
        lease.m_isHeld = true;
        lease.m_predecessor = ::getpid();
        if (auto written = writeMetadata(lease.m_descriptor.get(), { ClaimState::Unbound, lease.m_predecessor, {} }); !written)
            return std::unexpected(written.error());
        return lease;
    }
    return std::unexpected(HandoverLeaseError::SystemFailure);
}

std::expected<void, HandoverLeaseError> HandoverLease::bindSuccessor(HandoverLeaseClaim claim)
{
    if (m_process != ::getpid())
        return std::unexpected(Error::WrongProcess);
    if (!m_isHeld || !m_descriptor)
        return std::unexpected(Error::NotHeld);
    if (m_isSuccessor || claim.successor <= 0 || claim.successor == m_process || claim.requester == Protocol::Sync::ID128 {})
        return std::unexpected(Error::InvalidArgument);
    if (!isSafeDescriptor(m_descriptor.get()))
        return std::unexpected(Error::UnsafeDescriptor);
    auto metadata = readMetadata(m_descriptor.get());
    if (!metadata || metadata->predecessor != m_process)
        return std::unexpected(Error::InvalidMetadata);
    if (metadata->state != ClaimState::Unbound)
        return std::unexpected(Error::AlreadyBound);
    metadata->state = ClaimState::Pending;
    metadata->claim = claim;
    auto written = writeMetadata(m_descriptor.get(), *metadata);
    if (!written)
        return written;
    m_claim = claim;
    return {};
}

std::expected<HandoverLease, HandoverLeaseError> HandoverLease::accept(PAL::UnixFileDescriptor descriptor, pid_t predecessor, HandoverLeaseClaim claim)
{
    if (predecessor <= 0 || predecessor == ::getpid() || claim.successor != ::getpid() || claim.requester == Protocol::Sync::ID128 {})
        return std::unexpected(HandoverLeaseError::InvalidArgument);
    if (!isSafeDescriptor(descriptor.get()))
        return std::unexpected(HandoverLeaseError::UnsafeDescriptor);
    auto lock = wholeFileLock(F_WRLCK);
    if (::fcntl(descriptor.get(), F_GETLK, &lock) != 0)
        return std::unexpected(HandoverLeaseError::SystemFailure);
    if (lock.l_type != F_WRLCK || lock.l_pid != predecessor
        || lock.l_whence != SEEK_SET || lock.l_start || lock.l_len)
        return std::unexpected(HandoverLeaseError::WrongPredecessor);
    const auto metadata = readMetadata(descriptor.get());
    if (!metadata || metadata->predecessor != predecessor)
        return std::unexpected(Error::InvalidMetadata);
    if (metadata->state != ClaimState::Pending || metadata->claim != claim)
        return std::unexpected(Error::WrongSuccessor);
    HandoverLease lease(std::move(descriptor));
    lease.m_isSuccessor = true;
    lease.m_predecessor = predecessor;
    lease.m_claim = claim;
    return lease;
}

std::expected<void, HandoverLeaseError> HandoverLease::sendOffer(int socket, std::span<const std::uint8_t> payload, int timeoutMilliseconds)
{
    if (m_process != ::getpid())
        return std::unexpected(HandoverLeaseError::WrongProcess);
    if (!m_isHeld || !m_descriptor)
        return std::unexpected(HandoverLeaseError::NotHeld);
    if (payload.size() > maxHandoverLeaseOfferBytes || timeoutMilliseconds <= 0)
        return std::unexpected(HandoverLeaseError::InvalidArgument);
    if (!isSafeDescriptor(m_descriptor.get()))
        return std::unexpected(Error::UnsafeDescriptor);
    const auto metadata = readMetadata(m_descriptor.get());
    if (!metadata || metadata->predecessor != m_process)
        return std::unexpected(Error::InvalidMetadata);
    if (m_isSuccessor || metadata->state != ClaimState::Pending || metadata->claim != m_claim)
        return std::unexpected(Error::WrongSuccessor);
    const std::array descriptors { m_descriptor.get() };
    if (!sendHandoverRecord(socket, payload, descriptors, timeoutMilliseconds))
        return std::unexpected(HandoverLeaseError::TransferFailed);
    return {};
}

std::expected<void, HandoverLeaseError> HandoverLease::tryAcquire()
{
    if (m_process != ::getpid())
        return std::unexpected(HandoverLeaseError::WrongProcess);
    if (!isSafeDescriptor(m_descriptor.get())) {
        if (m_isHeld)
            (void)release();
        return std::unexpected(HandoverLeaseError::UnsafeDescriptor);
    }
    auto lock = wholeFileLock(F_WRLCK);
    if (::fcntl(m_descriptor.get(), F_SETLK, &lock) != 0)
        return std::unexpected(errno == EACCES || errno == EAGAIN ? HandoverLeaseError::Busy : HandoverLeaseError::SystemFailure);
    m_isHeld = true;
    const auto metadata = readMetadata(m_descriptor.get());
    std::optional<Error> refusal;
    if (!metadata || metadata->predecessor != m_predecessor || metadata->claim != m_claim)
        refusal = Error::InvalidMetadata;
    else if (metadata->state == ClaimState::Claimed)
        refusal = Error::SuccessorClaimed;
    else if (metadata->state == ClaimState::Cancelled)
        refusal = Error::SuccessorCancelled;
    else if (m_isSuccessor && metadata->state != ClaimState::Pending)
        refusal = Error::WrongSuccessor;
    if (refusal) {
        (void)release();
        return std::unexpected(*refusal);
    }
    return {};
}

std::expected<void, HandoverLeaseError> HandoverLease::claimSuccessor()
{
    if (m_process != ::getpid())
        return std::unexpected(Error::WrongProcess);
    if (!m_isHeld || !m_descriptor)
        return std::unexpected(Error::NotHeld);
    if (!m_isSuccessor || m_claim.successor != m_process)
        return std::unexpected(Error::WrongSuccessor);
    if (!isSafeDescriptor(m_descriptor.get()))
        return std::unexpected(Error::UnsafeDescriptor);
    auto metadata = readMetadata(m_descriptor.get());
    if (!metadata || metadata->predecessor != m_predecessor || metadata->claim != m_claim)
        return std::unexpected(Error::InvalidMetadata);
    if (metadata->state == ClaimState::Claimed)
        return std::unexpected(Error::AlreadyClaimed);
    if (metadata->state == ClaimState::Cancelled)
        return std::unexpected(Error::SuccessorCancelled);
    if (metadata->state != ClaimState::Pending)
        return std::unexpected(Error::WrongSuccessor);
    metadata->state = ClaimState::Claimed;
    return writeMetadata(m_descriptor.get(), *metadata);
}

std::expected<void, HandoverLeaseError> HandoverLease::cancelSuccessor()
{
    if (m_process != ::getpid())
        return std::unexpected(Error::WrongProcess);
    if (!m_isHeld || !m_descriptor)
        return std::unexpected(Error::NotHeld);
    if (m_isSuccessor || m_predecessor != m_process)
        return std::unexpected(Error::WrongPredecessor);
    if (!isSafeDescriptor(m_descriptor.get()))
        return std::unexpected(Error::UnsafeDescriptor);
    auto metadata = readMetadata(m_descriptor.get());
    if (!metadata || metadata->predecessor != m_predecessor || metadata->claim != m_claim)
        return std::unexpected(Error::InvalidMetadata);
    if (metadata->state == ClaimState::Claimed)
        return std::unexpected(Error::SuccessorClaimed);
    if (metadata->state == ClaimState::Cancelled)
        return std::unexpected(Error::SuccessorCancelled);
    if (metadata->state != ClaimState::Pending)
        return std::unexpected(Error::WrongSuccessor);
    metadata->state = ClaimState::Cancelled;
    return writeMetadata(m_descriptor.get(), *metadata);
}

std::expected<void, HandoverLeaseError> HandoverLease::release()
{
    if (m_process != ::getpid())
        return std::unexpected(HandoverLeaseError::WrongProcess);
    if (!m_isHeld || !m_descriptor)
        return std::unexpected(HandoverLeaseError::NotHeld);
    m_isHeld = false;
    auto lock = wholeFileLock(F_UNLCK);
    if (::fcntl(m_descriptor.get(), F_SETLK, &lock) != 0)
        return std::unexpected(HandoverLeaseError::SystemFailure);
    return {};
}

} // namespace Ubiquitty::POSIX
