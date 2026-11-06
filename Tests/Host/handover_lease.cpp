// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"
#include "Platform/POSIX/HandoverLease.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <type_traits>

namespace POSIX = Ubiquitty::POSIX;
using POSIX::HandoverLease;
using POSIX::HandoverLeaseError;
using Ubiquitty::PAL::UnixFileDescriptor;

namespace Ubiquitty::POSIX {
/// Injects torn metadata while preserving the sole descriptor owner.
struct HandoverLeaseAccess {
    static bool corrupt(HandoverLease& lease, off_t offset)
    {
        const std::uint8_t byte = 0xFF;
        return ::pwrite(lease.m_descriptor.get(), &byte, 1, offset) == 1;
    }
};
}

static_assert(!std::is_copy_constructible_v<HandoverLease>);
static_assert(!std::is_copy_assignable_v<HandoverLease>);

namespace {

constexpr int timeoutMilliseconds = 2000;
constexpr std::uint8_t ready = 1;
constexpr std::uint8_t acquired = 2;
constexpr std::uint8_t busy = 3;
constexpr std::uint8_t released = 4;
constexpr std::uint8_t refused = 5;
constexpr Ubiquitty::Protocol::Sync::ID128 requester { 71, 113 };

enum class Command : std::uint8_t {
    Acquire,
    Claim,
    DuplicateClaim,
    ClaimWithoutLock,
    ClaimAndExit,
    ReleaseAnnounced,
    AcquireAfterCancellation,
    CancelAsSuccessor,
    Release,
    Exit
};
enum class Predecessor : std::uint8_t {
    Exact,
    Wrong,
    Partial,
    SharedDescriptionProbe,
    WrongClaim
};

bool sendByte(int channel, std::uint8_t value)
{
    const std::array bytes { value };
    return POSIX::sendHandoverRecord(channel, bytes, {}, timeoutMilliseconds).has_value();
}

std::optional<std::uint8_t> receiveByte(int channel)
{
    const auto record = POSIX::receiveHandoverRecord(channel, 1, 0, timeoutMilliseconds);
    if (!record || record->length != 1)
        return std::nullopt;
    return record->bytes()[0];
}

int runReceiver(int channel, pid_t predecessor, Predecessor identity, int serviceDescriptor)
{
    ::alarm(10);
    auto offer = POSIX::receiveHandoverRecord(channel, 1, 1, timeoutMilliseconds);
    if (!offer || offer->length != 1 || offer->bytes()[0] != ready)
        return 10;
    const int incoming = offer->descriptors[0].get();
    const int descriptorFlags = ::fcntl(incoming, F_GETFD);
    if (descriptorFlags < 0 || !(descriptorFlags & FD_CLOEXEC))
        return 11;
    if (identity == Predecessor::SharedDescriptionProbe) {
        // The predecessor still holds this flock through the same description.
        if (::flock(incoming, LOCK_EX | LOCK_NB) != 0)
            return 20;
        return sendByte(channel, ready) ? 0 : 22;
    }
    auto expected = requester;
    if (identity == Predecessor::WrongClaim)
        expected.high ^= 1;
    auto lease = HandoverLease::accept(std::move(offer->descriptors[0]), identity == Predecessor::Wrong ? 1 : predecessor, { ::getpid(), expected });
    if (identity == Predecessor::WrongClaim) {
        if (lease || lease.error() != HandoverLeaseError::WrongSuccessor)
            return 23;
        return sendByte(channel, refused) ? 0 : 24;
    }
    if (identity == Predecessor::Wrong || identity == Predecessor::Partial) {
        if (lease || lease.error() != HandoverLeaseError::WrongPredecessor || ::fcntl(incoming, F_GETFD) != -1 || errno != EBADF)
            return 12;
        return sendByte(channel, refused) ? 0 : 13;
    }
    if (!lease || !sendByte(channel, ready))
        return 14;
    for (;;) {
        const auto command = receiveByte(channel);
        if (!command)
            return 15;
        switch (static_cast<Command>(*command)) {
        case Command::ReleaseAnnounced:
            // A received Release is not authority: the original can still win
            // the kernel lock and cancel before this delayed process acquires.
            if (!sendByte(channel, ready))
                return 30;
            break;
        case Command::AcquireAfterCancellation: {
            const auto acquiredLease = lease->tryAcquire();
            if (acquiredLease || acquiredLease.error() != HandoverLeaseError::SuccessorCancelled)
                return 31;
            const auto claimed = lease->claimSuccessor();
            if (claimed || claimed.error() != HandoverLeaseError::NotHeld)
                return 32;
            return sendByte(channel, refused) ? 0 : 33;
        }
        case Command::CancelAsSuccessor: {
            const auto cancelled = lease->cancelSuccessor();
            if (cancelled || cancelled.error() != HandoverLeaseError::WrongPredecessor || !sendByte(channel, refused))
                return 34;
            break;
        }
        case Command::ClaimAndExit:
            if (!lease->claimSuccessor())
                return 28;
            return serviceDescriptor >= 0 && ::write(serviceDescriptor, "served", 6) == 6 ? 0 : 29;
        case Command::Claim:
            if (!lease->claimSuccessor() || !sendByte(channel, acquired))
                return 25;
            break;
        case Command::DuplicateClaim: {
            const auto result = lease->claimSuccessor();
            if (result || result.error() != HandoverLeaseError::AlreadyClaimed || !sendByte(channel, refused))
                return 26;
            break;
        }
        case Command::ClaimWithoutLock: {
            const auto result = lease->claimSuccessor();
            if (result || result.error() != HandoverLeaseError::NotHeld || !sendByte(channel, refused))
                return 27;
            break;
        }
        case Command::Acquire: {
            const auto result = lease->tryAcquire();
            if (!result && result.error() != HandoverLeaseError::Busy)
                return 16;
            if (!sendByte(channel, result ? acquired : busy))
                return 17;
            break;
        }
        case Command::Release:
            if (!lease->release() || !sendByte(channel, released))
                return 18;
            break;
        case Command::Exit:
            return 0;
        default:
            return 19;
        }
    }
}

class Peer {
public:
    explicit Peer(Predecessor identity = Predecessor::Exact, int serviceDescriptor = -1)
    {
        int sockets[2] { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            return;
        UnixFileDescriptor parent(sockets[0]);
        UnixFileDescriptor child(sockets[1]);
        const pid_t predecessor = ::getpid();
        m_process = ::fork();
        if (!m_process) {
            parent.reset();
            const int result = runReceiver(child.get(), predecessor, identity, serviceDescriptor);
            ::_exit(result);
        }
        m_channel = std::move(parent);
    }

    ~Peer()
    {
        if (m_process > 0) {
            ::kill(m_process, SIGKILL);
            (void)join();
        }
    }

    int channel() const { return m_channel.get(); }
    pid_t process() const { return m_process; }

    bool command(Command command, std::uint8_t expected)
    {
        if (!sendByte(channel(), std::to_underlying(command)))
            return false;
        const auto response = receiveByte(channel());
        return response && *response == expected;
    }

    int join()
    {
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(m_process, &status, 0);
        } while (result < 0 && errno == EINTR);
        CHECK_EQ(result, m_process);
        m_process = -1;
        return status;
    }

private:
    UnixFileDescriptor m_channel;
    pid_t m_process = -1;
};

struct Directory {
    Ubiquitty::Test::TempDir temporary;
    UnixFileDescriptor descriptor { ::open(temporary.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
};

bool offerTo(HandoverLease& lease, Peer& peer)
{
    const std::array payload { ready };
    if (!lease.bindSuccessor({ peer.process(), requester }))
        return false;
    if (!lease.sendOffer(peer.channel(), payload, timeoutMilliseconds))
        return false;
    const auto response = receiveByte(peer.channel());
    return response && *response == ready;
}

void finishPeer(Peer& peer)
{
    REQUIRE(sendByte(peer.channel(), std::to_underlying(Command::Exit)));
    const int status = peer.join();
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

enum class FileKind : std::uint8_t {
    Safe,
    Linked,
    ReadOnly,
    PublicMode,
    Nonempty,
    Inheritable
};

UnixFileDescriptor candidate(Directory& directory, FileKind kind)
{
    const char* leaf = "candidate";
    UnixFileDescriptor descriptor(::openat(directory.descriptor.get(), leaf, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
    if (!descriptor)
        return {};
    CHECK_EQ(::ftruncate(descriptor.get(), 64), 0);
    if (kind == FileKind::ReadOnly)
        descriptor.reset(::openat(directory.descriptor.get(), leaf, O_RDONLY | O_CLOEXEC));
    if (kind != FileKind::Linked)
        CHECK_EQ(::unlinkat(directory.descriptor.get(), leaf, 0), 0);
    if (kind == FileKind::PublicMode)
        CHECK_EQ(::fchmod(descriptor.get(), 0640), 0);
    if (kind == FileKind::Nonempty)
        CHECK_EQ(::ftruncate(descriptor.get(), 65), 0);
    if (kind == FileKind::Inheritable)
        CHECK_EQ(::fcntl(descriptor.get(), F_SETFD, 0), 0);
    return descriptor;
}

} // namespace

TEST(releaseTransfersServiceExclusivelyAcrossSCMRights)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    CHECK(std::filesystem::is_empty(directory.temporary.path()));
    REQUIRE(offerTo(*holder, peer));
    CHECK(peer.command(Command::Acquire, busy));
    REQUIRE(holder->release());
    CHECK(peer.command(Command::Acquire, acquired));
    const auto rollback = holder->tryAcquire();
    CHECK(!rollback && rollback.error() == HandoverLeaseError::Busy);
    CHECK(peer.command(Command::Release, released));
    CHECK(holder->tryAcquire());
    CHECK(peer.command(Command::Acquire, busy));
    finishPeer(peer);
    CHECK(std::filesystem::is_empty(directory.temporary.path()));
}

TEST(deadRequesterReleasesTheKernelLease)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder && offerTo(*holder, peer));
    REQUIRE(holder->release());
    REQUIRE(peer.command(Command::Acquire, acquired));
    CHECK(!holder->tryAcquire());
    REQUIRE(::kill(peer.process(), SIGKILL) == 0);
    const int status = peer.join();
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(holder->tryAcquire());
}

TEST(aSuccessorClaimSurvivesProcessDeathBeforeServingAcknowledgement)
{
    Directory directory;
    UnixFileDescriptor serviced(::openat(directory.descriptor.get(), "serviced", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
    REQUIRE(serviced);
    Peer peer(Predecessor::Exact, serviced.get());
    REQUIRE(peer.process() > 0);
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder && offerTo(*holder, peer));
    REQUIRE(holder->release());
    REQUIRE(peer.command(Command::Acquire, acquired));
    REQUIRE(sendByte(peer.channel(), std::to_underlying(Command::ClaimAndExit)));
    const int status = peer.join();
    REQUIRE(WIFEXITED(status) && !WEXITSTATUS(status));
    std::array<char, 6> output {};
    REQUIRE(::pread(serviced.get(), output.data(), output.size(), 0) == static_cast<ssize_t>(output.size()));
    CHECK(std::string_view(output.data(), output.size()) == "served");
    const auto rollback = holder->tryAcquire();
    CHECK(!rollback && rollback.error() == HandoverLeaseError::SuccessorClaimed);
    const auto again = holder->tryAcquire();
    CHECK(!again && again.error() == HandoverLeaseError::SuccessorClaimed);
    const auto release = holder->release();
    CHECK(!release && release.error() == HandoverLeaseError::NotHeld);
}

TEST(onlyTheBoundSuccessorMayClaimExactlyOnceWhileHoldingTheLock)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder && offerTo(*holder, peer));
    const auto wrongRole = holder->claimSuccessor();
    CHECK(!wrongRole && wrongRole.error() == HandoverLeaseError::WrongSuccessor);
    const auto duplicateBinding = holder->bindSuccessor({ peer.process(), requester });
    CHECK(!duplicateBinding && duplicateBinding.error() == HandoverLeaseError::AlreadyBound);
    CHECK(peer.command(Command::ClaimWithoutLock, refused));
    REQUIRE(holder->release());
    REQUIRE(peer.command(Command::Acquire, acquired));
    CHECK(peer.command(Command::Claim, acquired));
    CHECK(peer.command(Command::DuplicateClaim, refused));
    CHECK(peer.command(Command::Release, released));
    const auto rollback = holder->tryAcquire();
    CHECK(!rollback && rollback.error() == HandoverLeaseError::SuccessorClaimed);
    finishPeer(peer);
}

TEST(aDifferentAuthenticatedRequestCannotAdoptTheLease)
{
    Peer peer(Predecessor::WrongClaim);
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    REQUIRE(holder->bindSuccessor({ peer.process(), requester }));
    const std::array payload { ready };
    REQUIRE(holder->sendOffer(peer.channel(), payload, timeoutMilliseconds));
    const auto response = receiveByte(peer.channel());
    REQUIRE(response && *response == refused);
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(holder->tryAcquire());
}

TEST(tornOrCorruptedMetadataNeverAuthorizesRollback)
{
    for (const auto offset : { off_t { 0 }, off_t { 12 }, off_t { 24 }, off_t { 56 } }) {
        Directory directory;
        auto holder = HandoverLease::create(directory.descriptor.get());
        REQUIRE(holder);
        REQUIRE(holder->bindSuccessor({ ::getpid() + 1, requester }));
        REQUIRE(Ubiquitty::POSIX::HandoverLeaseAccess::corrupt(*holder, offset));
        REQUIRE(holder->release());
        const auto rollback = holder->tryAcquire();
        CHECK(!rollback && rollback.error() == HandoverLeaseError::InvalidMetadata);
        const auto released = holder->release();
        CHECK(!released && released.error() == HandoverLeaseError::NotHeld);
    }
}

TEST(rollbackWinnerExcludesLateSuccessor)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder && offerTo(*holder, peer));
    REQUIRE(holder->release());
    REQUIRE(holder->tryAcquire());
    CHECK(peer.command(Command::Acquire, busy));
    CHECK(peer.command(Command::Acquire, busy));
    finishPeer(peer);
}

TEST(cancelledReleaseCannotWakeALateSuccessorAfterPredecessorResumesAndReleases)
{
    Directory directory;
    UnixFileDescriptor serviced(::openat(directory.descriptor.get(), "serviced", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
    REQUIRE(serviced);
    Peer peer(Predecessor::Exact, serviced.get());
    REQUIRE(peer.process() > 0);
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder && offerTo(*holder, peer));
    REQUIRE(holder->release());
    REQUIRE(peer.command(Command::ReleaseAnnounced, ready));
    REQUIRE(holder->tryAcquire());
    REQUIRE(holder->cancelSuccessor());
    REQUIRE(::write(serviced.get(), "resumed", 7) == 7);
    REQUIRE(holder->release());
    holder = std::unexpected(HandoverLeaseError::InvalidArgument);
    REQUIRE(peer.command(Command::AcquireAfterCancellation, refused));
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    std::array<char, 8> output {};
    REQUIRE(::pread(serviced.get(), output.data(), output.size(), 0) == 7);
    CHECK(std::string_view(output.data(), 7) == "resumed");
}

TEST(onlyTheHeldOriginalOwnerCanCancelAnExactPendingOffer)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    const auto unbound = holder->cancelSuccessor();
    CHECK(!unbound && unbound.error() == HandoverLeaseError::WrongSuccessor);
    REQUIRE(offerTo(*holder, peer));
    REQUIRE(holder->release());
    const auto unheld = holder->cancelSuccessor();
    CHECK(!unheld && unheld.error() == HandoverLeaseError::NotHeld);
    REQUIRE(peer.command(Command::Acquire, acquired));
    CHECK(peer.command(Command::CancelAsSuccessor, refused));
    REQUIRE(peer.command(Command::Release, released));
    REQUIRE(holder->tryAcquire());
    REQUIRE(holder->cancelSuccessor());
    const auto duplicate = holder->cancelSuccessor();
    CHECK(!duplicate && duplicate.error() == HandoverLeaseError::SuccessorCancelled);
    const auto rebound = holder->bindSuccessor({ peer.process(), requester });
    CHECK(!rebound && rebound.error() == HandoverLeaseError::AlreadyBound);
    REQUIRE(holder->release());
    REQUIRE(peer.command(Command::AcquireAfterCancellation, refused));
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}

TEST(tornCancellationMetadataNeverPermitsResumeOrLateClaim)
{
    for (const auto offset : { off_t { 0 }, off_t { 12 }, off_t { 24 }, off_t { 56 } }) {
        Directory directory;
        auto holder = HandoverLease::create(directory.descriptor.get());
        REQUIRE(holder);
        REQUIRE(holder->bindSuccessor({ ::getpid() + 1, requester }));
        REQUIRE(Ubiquitty::POSIX::HandoverLeaseAccess::corrupt(*holder, offset));
        const auto cancelled = holder->cancelSuccessor();
        CHECK(!cancelled && cancelled.error() == HandoverLeaseError::InvalidMetadata);
        REQUIRE(holder->release());
        const auto rollback = holder->tryAcquire();
        CHECK(!rollback && rollback.error() == HandoverLeaseError::InvalidMetadata);
        const auto released = holder->release();
        CHECK(!released && released.error() == HandoverLeaseError::NotHeld);
    }
}

TEST(movePreservesOneLocalOwnerAndDestructorReleases)
{
    Peer peer;
    REQUIRE(peer.process() > 0);
    Directory directory;
    {
        auto holder = HandoverLease::create(directory.descriptor.get());
        REQUIRE(holder);
        auto moved = std::move(*holder);
        REQUIRE(offerTo(moved, peer));
        CHECK(!holder->release());
        CHECK(!holder->tryAcquire());
        CHECK(peer.command(Command::Acquire, busy));
        // No service continues here; dropping the sole owner relinquishes it.
    }
    CHECK(peer.command(Command::Acquire, acquired));
    finishPeer(peer);
    CHECK(std::filesystem::is_empty(directory.temporary.path()));
}

TEST(receivedDescriptorIsCloseOnExecAndBoundToPredecessor)
{
    Peer peer(Predecessor::Wrong);
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    const std::array payload { ready };
    REQUIRE(holder->bindSuccessor({ peer.process(), requester }));
    REQUIRE(holder->sendOffer(peer.channel(), payload, timeoutMilliseconds));
    const auto response = receiveByte(peer.channel());
    CHECK(response && *response == refused);
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(holder->tryAcquire());
}

TEST(unsafeOrUnlockedDescriptorsAreConsumedWithoutAuthority)
{
    for (auto kind : { FileKind::Safe, FileKind::Linked, FileKind::ReadOnly, FileKind::PublicMode, FileKind::Nonempty, FileKind::Inheritable }) {
        Directory directory;
        auto descriptor = candidate(directory, kind);
        REQUIRE(descriptor);
        const int raw = descriptor.get();
        auto accepted = HandoverLease::accept(std::move(descriptor), ::getpid() + 1, { ::getpid(), requester });
        CHECK(!accepted);
        CHECK(accepted.error() == (kind == FileKind::Safe ? HandoverLeaseError::WrongPredecessor : HandoverLeaseError::UnsafeDescriptor));
        CHECK(::fcntl(raw, F_GETFD) == -1 && errno == EBADF);
    }
    int pipeHandles[2] { -1, -1 };
    REQUIRE(::pipe(pipeHandles) == 0);
    UnixFileDescriptor reader(pipeHandles[0]);
    UnixFileDescriptor writer(pipeHandles[1]);
    CHECK(!HandoverLease::accept(std::move(reader), ::getpid() + 1, { ::getpid(), requester }));
}

TEST(partialPredecessorLockDoesNotAuthorizeTheWholeService)
{
    Peer peer(Predecessor::Partial);
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto descriptor = candidate(directory, FileKind::Safe);
    REQUIRE(descriptor);
    struct flock lock {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 1;
    REQUIRE(::fcntl(descriptor.get(), F_SETLK, &lock) == 0);
    const std::array payload { ready };
    const std::array descriptors { descriptor.get() };
    REQUIRE(POSIX::sendHandoverRecord(peer.channel(), payload, descriptors, timeoutMilliseconds));
    const auto response = receiveByte(peer.channel());
    CHECK(response && *response == refused);
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}

TEST(sharedOpenDescriptionFlockIsNotAnExclusiveProcessLease)
{
    Peer peer(Predecessor::SharedDescriptionProbe);
    REQUIRE(peer.process() > 0);
    Directory directory;
    auto descriptor = candidate(directory, FileKind::Safe);
    REQUIRE(descriptor);
    REQUIRE(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0);
    const std::array payload { ready };
    const std::array descriptors { descriptor.get() };
    REQUIRE(POSIX::sendHandoverRecord(peer.channel(), payload, descriptors, timeoutMilliseconds));
    const auto response = receiveByte(peer.channel());
    CHECK(response && *response == ready);
    const int status = peer.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}

TEST(privateDirectoryAndOfferStateAreRequired)
{
    Directory directory;
    REQUIRE(directory.descriptor);
    REQUIRE(::fchmod(directory.descriptor.get(), 0750) == 0);
    CHECK(!HandoverLease::create(directory.descriptor.get()));
    CHECK(std::filesystem::is_empty(directory.temporary.path()));
    REQUIRE(::fchmod(directory.descriptor.get(), 0700) == 0);
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    REQUIRE(holder->bindSuccessor({ ::getpid() + 1, requester }));
    std::array<std::uint8_t, POSIX::maxHandoverLeaseOfferBytes + 1> oversized {};
    auto offered = holder->sendOffer(-1, oversized, timeoutMilliseconds);
    CHECK(!offered && offered.error() == HandoverLeaseError::InvalidArgument);
    offered = holder->sendOffer(-1, {}, timeoutMilliseconds);
    CHECK(!offered && offered.error() == HandoverLeaseError::TransferFailed);
    CHECK(holder->tryAcquire());
    REQUIRE(holder->release());
    offered = holder->sendOffer(-1, {}, timeoutMilliseconds);
    CHECK(!offered && offered.error() == HandoverLeaseError::NotHeld);
    CHECK(!holder->release());
    CHECK(holder->tryAcquire());
}

TEST(forkedOwnerCannotClaimInheritedAuthority)
{
    Directory directory;
    auto holder = HandoverLease::create(directory.descriptor.get());
    REQUIRE(holder);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (!child) {
        const auto acquired = holder->tryAcquire();
        const auto released = holder->release();
        const auto offered = holder->sendOffer(-1, {}, timeoutMilliseconds);
        const auto bound = holder->bindSuccessor({ ::getpid() + 1, requester });
        const auto claimed = holder->claimSuccessor();
        const auto cancelled = holder->cancelSuccessor();
        ::_exit(!acquired && acquired.error() == HandoverLeaseError::WrongProcess
                    && !released && released.error() == HandoverLeaseError::WrongProcess
                    && !offered && offered.error() == HandoverLeaseError::WrongProcess
                    && !bound && bound.error() == HandoverLeaseError::WrongProcess
                    && !claimed && claimed.error() == HandoverLeaseError::WrongProcess
                    && !cancelled && cancelled.error() == HandoverLeaseError::WrongProcess
                ? 0
                : 1);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(holder->tryAcquire());
}

UBIQUITTY_TEST_MAIN()
