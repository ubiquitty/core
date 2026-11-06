// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Platform/Host/HandoverRequest.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;
using Host::HandoverRequestReader;
using Host::HandoverRequestState;

namespace {

struct Channel {
    PAL::UnixFileDescriptor sender;
    PAL::UnixFileDescriptor receiver;
    PAL::UnixPipe descriptor = PAL::makeUnixPipe();
    HandoverRequestReader reader;

    Channel()
    {
        int sockets[2] { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        sender.reset(sockets[0]);
        receiver.reset(sockets[1]);
        REQUIRE(descriptor);
    }

    HandoverRequestState read(std::uint64_t now = 1) { return reader.receive(receiver.get(), now, 100); }

    void send(std::span<const std::uint8_t> bytes, std::span<const int> descriptors = {})
    {
        alignas(cmsghdr) std::array<char, CMSG_SPACE(2 * sizeof(int))> control {};
        iovec data { const_cast<std::uint8_t*>(bytes.data()), bytes.size() };
        msghdr message {};
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        if (!descriptors.empty()) {
            REQUIRE(descriptors.size() <= 2);
            message.msg_control = control.data();
            message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
            auto* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(descriptors.size_bytes());
            std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
        }
        REQUIRE(::sendmsg(sender.get(), &message, 0) == static_cast<ssize_t>(bytes.size()));
    }
};

constexpr HandoverRequest request { handoverBodyVersion, { 17, 91 } };

std::vector<std::uint8_t> record()
{
    auto body = encodeHandoverMessage(request);
    const auto size = static_cast<std::uint32_t>(body.size());
    std::vector<std::uint8_t> bytes;
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(size >> shift));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::size_t descriptorCount()
{
    auto* directory = ::opendir("/dev/fd");
    CHECK(directory);
    if (!directory)
        return 0;
    std::size_t count = 0;
    while (const auto* entry = ::readdir(directory))
        count += entry->d_name[0] != '.';
    ::closedir(directory);
    return count;
}

void requireEmptySocket(int socket)
{
    std::uint8_t byte = 0;
    CHECK(::recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
}

} // namespace

TEST(anOrdinaryFirstByteIsUntouchedAndPermanentlySelectsOrdinary)
{
    Channel channel;
    CHECK(channel.read() == HandoverRequestState::Awaiting);
    const std::array<std::uint8_t, 3> ordinary { 1, 2, 3 };
    channel.send(ordinary);
    CHECK(channel.read() == HandoverRequestState::Ordinary);
    std::array<std::uint8_t, 3> received {};
    REQUIRE(::recv(channel.receiver.get(), received.data(), received.size(), MSG_DONTWAIT) == 3);
    CHECK(received == ordinary);
    channel.send(handoverPreamble);
    CHECK(channel.read() == HandoverRequestState::Ordinary);
    std::array<std::uint8_t, 16> prefix {};
    REQUIRE(::recv(channel.receiver.get(), prefix.data(), prefix.size(), MSG_DONTWAIT) == 16);
    CHECK(prefix == handoverPreamble);
    CHECK(!channel.reader.take());
}

TEST(anOrdinaryFirstByteAfterTheDeadlineStillSelectsOrdinary)
{
    Channel channel;
    const std::array<std::uint8_t, 1> ordinary { 1 };
    channel.send(ordinary);
    CHECK(channel.read(100) == HandoverRequestState::Ordinary);
    Channel silent;
    CHECK(silent.read(100) == HandoverRequestState::Refused);
}

TEST(everyFragmentIsConsumedWithoutSpinningOrReadingTheFollowingRecord)
{
    Channel channel;
    const auto body = record();
    for (const auto byte : handoverPreamble) {
        channel.send(std::span { &byte, 1 });
        REQUIRE(channel.read() == HandoverRequestState::Awaiting);
        requireEmptySocket(channel.receiver.get());
    }
    const std::array listener { channel.descriptor.read.get() };
    for (std::size_t index = 0; index < body.size(); ++index) {
        channel.send(std::span { body }.subspan(index, 1), index ? std::span<const int> {} : listener);
        const auto expected = index + 1 == body.size() ? HandoverRequestState::Ready : HandoverRequestState::Awaiting;
        REQUIRE(channel.read() == expected);
        requireEmptySocket(channel.receiver.get());
    }
    auto received = channel.reader.take();
    REQUIRE(received);
    CHECK(received->request == request);
    CHECK(::fcntl(received->listener.get(), F_GETFD) & FD_CLOEXEC);
    CHECK(channel.read() == HandoverRequestState::Consumed);
    CHECK(!channel.reader.take());
}

TEST(aCompleteRequestLeavesTheNextTransactionRecordInTheSocket)
{
    Channel channel;
    channel.send(handoverPreamble);
    const auto body = encodeHandoverMessage(request);
    const std::array listener { channel.descriptor.read.get() };
    REQUIRE(POSIX::sendHandoverRecord(channel.sender.get(), body, listener, 1000));
    const auto next = encodeHandoverMessage(HandoverAccept {});
    REQUIRE(POSIX::sendHandoverRecord(channel.sender.get(), next, {}, 1000));
    REQUIRE(channel.read() == HandoverRequestState::Ready);
    auto received = channel.reader.take();
    REQUIRE(received);
    CHECK(received->request == request);
    auto untouched = POSIX::receiveHandoverRecord(channel.receiver.get(), 1, 0, 1000);
    REQUIRE(untouched);
    CHECK(std::ranges::equal(untouched->bytes(), next));
}

TEST(aChangedPreambleIsRefusedWithoutFallingBackToOrdinaryFrames)
{
    for (std::size_t changed = 1; changed < handoverPreamble.size(); ++changed) {
        Channel channel;
        auto prefix = handoverPreamble;
        prefix[changed] ^= 1;
        channel.send(prefix);
        CHECK(channel.read() == HandoverRequestState::Refused);
        CHECK(!channel.reader.take());
        CHECK(channel.read() == HandoverRequestState::Refused);
    }
}

TEST(aDescriptorOnThePreambleIsClosedAndNeverAuthorizesARequest)
{
    Channel channel;
    const auto before = descriptorCount();
    const std::array descriptors { channel.descriptor.read.get() };
    channel.send(handoverPreamble, descriptors);
    CHECK(channel.read() == HandoverRequestState::Refused);
    CHECK_EQ(descriptorCount(), before);
}

TEST(theFirstRequestByteMustCarryExactlyOneDescriptor)
{
    for (std::size_t count = 0; count <= 2; count += 2) {
        Channel channel;
        const auto before = descriptorCount();
        channel.send(handoverPreamble);
        REQUIRE(channel.read() == HandoverRequestState::Awaiting);
        const std::array descriptors { channel.descriptor.read.get(), channel.descriptor.write.get() };
        channel.send(record(), std::span { descriptors }.first(count));
        CHECK(channel.read() == HandoverRequestState::Refused);
        CHECK_EQ(descriptorCount(), before);
    }
}

TEST(aDescriptorArrivingAfterTheFirstRequestByteClosesBothOwners)
{
    Channel channel;
    const auto before = descriptorCount();
    channel.send(handoverPreamble);
    const auto bytes = record();
    const std::array descriptors { channel.descriptor.read.get() };
    channel.send(std::span { bytes }.first(1), descriptors);
    REQUIRE(channel.read() == HandoverRequestState::Awaiting);
    REQUIRE(descriptorCount() == before + 1);
    channel.send(std::span { bytes }.subspan(1), descriptors);
    CHECK(channel.read() == HandoverRequestState::Refused);
    CHECK_EQ(descriptorCount(), before);
}

TEST(aWrongFixedLengthIsRefusedBeforeAnyBodyAllocation)
{
    for (const std::uint8_t length : { 0, 20, 22, 255 }) {
        Channel channel;
        const auto before = descriptorCount();
        channel.send(handoverPreamble);
        auto bytes = record();
        bytes[0] = length;
        const std::array descriptors { channel.descriptor.read.get() };
        channel.send(bytes, descriptors);
        CHECK(channel.read() == HandoverRequestState::Refused);
        CHECK_EQ(descriptorCount(), before);
    }
}

TEST(aMalformedRequestNeverEscapesItsOwnedDescriptor)
{
    for (const std::size_t changed : { 4u, 5u, 7u }) {
        Channel channel;
        const auto before = descriptorCount();
        channel.send(handoverPreamble);
        auto bytes = record();
        bytes[changed] = 0;
        const std::array descriptors { channel.descriptor.read.get() };
        channel.send(bytes, descriptors);
        CHECK(channel.read() == HandoverRequestState::Refused);
        CHECK_EQ(descriptorCount(), before);
    }
}

TEST(aDeadlineClosesAPartiallyReceivedListenerWithoutWaiting)
{
    Channel channel;
    const auto before = descriptorCount();
    channel.send(handoverPreamble);
    const auto bytes = record();
    const std::array descriptors { channel.descriptor.read.get() };
    channel.send(std::span { bytes }.first(5), descriptors);
    REQUIRE(channel.read() == HandoverRequestState::Awaiting);
    REQUIRE(descriptorCount() == before + 1);
    requireEmptySocket(channel.receiver.get());
    CHECK(channel.read(100) == HandoverRequestState::Refused);
    CHECK_EQ(descriptorCount(), before);
    CHECK(!channel.reader.take());
}

TEST(aPeerClosingMidRequestReleasesItsReceivedListener)
{
    Channel channel;
    channel.send(handoverPreamble);
    const auto bytes = record();
    const std::array descriptors { channel.descriptor.read.get() };
    channel.send(std::span { bytes }.first(5), descriptors);
    REQUIRE(channel.read() == HandoverRequestState::Awaiting);
    channel.sender.reset();
    const auto before = descriptorCount();
    CHECK(channel.read() == HandoverRequestState::Refused);
    CHECK_EQ(descriptorCount(), before - 1);
}

UBIQUITTY_TEST_MAIN()
