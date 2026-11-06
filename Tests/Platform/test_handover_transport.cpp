// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Platform/POSIX/HandoverTransport.h"
#include "Platform/POSIX/ReceivedDescriptor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::POSIX;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

struct Channel {
    PAL::UnixFileDescriptor sender;
    PAL::UnixFileDescriptor receiver;

    Channel()
    {
        int pair[2] { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        sender.reset(pair[0]);
        receiver.reset(pair[1]);
    }
};

std::size_t openDescriptorCount()
{
    DIR* directory = ::opendir("/dev/fd");
    CHECK(directory);
    if (!directory)
        return 0;
    std::size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.')
            ++count;
    }
    ::closedir(directory);
    return count;
}

void rawSend(int socket, std::span<const std::uint8_t> bytes, std::span<const int> descriptors = {})
{
    union Control {
        char bytes[CMSG_SPACE(64 * sizeof(int))];
        cmsghdr alignment;
    } control {};
    iovec buffer { const_cast<std::uint8_t*>(bytes.data()), bytes.size() };
    msghdr message {};
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    if (!descriptors.empty()) {
        message.msg_control = control.bytes;
        message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(descriptors.size_bytes());
        std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
    }
    REQUIRE(::sendmsg(socket, &message, 0) == static_cast<ssize_t>(bytes.size()));
}

constexpr int timeoutMilliseconds = 1000;
constexpr std::array<std::uint8_t, 3> payload { 4, 5, 6 };

} // namespace

TEST(aRecordCarriesItsDescriptorWithoutTransferringTheSendersOwnership)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    REQUIRE(sendHandoverRecord(channel.sender.get(), payload, descriptors, timeoutMilliseconds));
    auto received = receiveHandoverRecord(channel.receiver.get(), payload.size(), 1, timeoutMilliseconds);
    REQUIRE(received);
    CHECK(std::ranges::equal(received->bytes(), payload));
    REQUIRE(received->descriptorCount == 1);
    CHECK(::fcntl(received->descriptors[0].get(), F_GETFD) & FD_CLOEXEC);
    CHECK(::fcntl(pipe.read.get(), F_GETFD) >= 0);
    REQUIRE(::write(pipe.write.get(), "x", 1) == 1);
    char value = 0;
    CHECK(::read(received->descriptors[0].get(), &value, 1) == 1);
    CHECK_EQ(value, 'x');
}

TEST(emptyPayloadAndConsecutiveRecordsRemainDistinct)
{
    Channel channel;
    REQUIRE(sendHandoverRecord(channel.sender.get(), {}, {}, timeoutMilliseconds));
    REQUIRE(sendHandoverRecord(channel.sender.get(), payload, {}, timeoutMilliseconds));
    auto empty = receiveHandoverRecord(channel.receiver.get(), 0, 0, timeoutMilliseconds);
    REQUIRE(empty);
    CHECK(empty->bytes().empty());
    auto second = receiveHandoverRecord(channel.receiver.get(), payload.size(), 0, timeoutMilliseconds);
    REQUIRE(second);
    CHECK(std::ranges::equal(second->bytes(), payload));
}

TEST(theMaximumDescriptorBatchArrivesCloseOnExec)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    std::array<int, maxHandoverDescriptors> descriptors;
    descriptors.fill(pipe.read.get());
    REQUIRE(sendHandoverRecord(channel.sender.get(), {}, descriptors, timeoutMilliseconds));
    auto received = receiveHandoverRecord(channel.receiver.get(), 0, descriptors.size(), timeoutMilliseconds);
    REQUIRE(received);
    CHECK_EQ(received->descriptorCount, descriptors.size());
    for (const auto& descriptor : received->descriptors)
        CHECK(::fcntl(descriptor.get(), F_GETFD) & FD_CLOEXEC);
}

TEST(extraDescriptorsAreRefusedAndEveryInstalledHandleIsClosed)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    std::array<int, 33> descriptors;
    descriptors.fill(pipe.read.get());
    const std::array<std::uint8_t, 4> empty { 0, 0, 0, 0 };
    const auto before = openDescriptorCount();
    rawSend(channel.sender.get(), empty, descriptors);
    auto received = receiveHandoverRecord(channel.receiver.get(), 0, 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::DescriptorCount);
    CHECK_EQ(openDescriptorCount(), before);
}

TEST(oversizedLengthRefusesBeforeWaitingForTheBodyAndClosesDescriptors)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    const std::array<std::uint8_t, 4> large { 255, 255, 255, 255 };
    const auto before = openDescriptorCount();
    rawSend(channel.sender.get(), large, descriptors);
    auto received = receiveHandoverRecord(channel.receiver.get(), 1024, 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::Oversize);
    CHECK_EQ(openDescriptorCount(), before);
}

TEST(aClosedPeerCannotCompleteAPartialHeaderOrBody)
{
    for (const std::vector<std::uint8_t>& bytes : { std::vector<std::uint8_t> { 1, 0 }, std::vector<std::uint8_t> { 2, 0, 0, 0, 9 } }) {
        Channel channel;
        rawSend(channel.sender.get(), bytes);
        channel.sender.reset();
        auto received = receiveHandoverRecord(channel.receiver.get(), 8, 0, timeoutMilliseconds);
        REQUIRE(!received);
        CHECK(received.error() == HandoverChannelError::Closed);
    }
}

TEST(descriptorsInTheBodyAreRefusedAndClosed)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    const std::array<std::uint8_t, 4> header { 3, 0, 0, 0 };
    const auto before = openDescriptorCount();
    rawSend(channel.sender.get(), header);
    rawSend(channel.sender.get(), payload, descriptors);
    auto received = receiveHandoverRecord(channel.receiver.get(), 8, 0, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::DescriptorCount);
    CHECK_EQ(openDescriptorCount(), before);
}

TEST(largePartialWritesKeepThePayloadAndSendDescriptorsOnlyOnce)
{
    Channel channel;
    const int bufferBytes = 4096;
    REQUIRE(::setsockopt(channel.sender.get(), SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes)) == 0);
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    std::vector<std::uint8_t> bytes(1024 * 1024);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(index);
    std::expected<void, HandoverChannelError> sent;
    std::thread writer([&] { sent = sendHandoverRecord(channel.sender.get(), bytes, descriptors, 5000); });
    auto received = receiveHandoverRecord(channel.receiver.get(), bytes.size(), 1, 5000);
    writer.join();
    REQUIRE(sent);
    REQUIRE(received);
    CHECK(std::ranges::equal(received->bytes(), bytes));
    CHECK_EQ(received->descriptorCount, 1u);
}

TEST(aSilentPeerAndABlockedSenderHaveBoundedDeadlines)
{
    Channel channel;
    const int senderFlags = ::fcntl(channel.sender.get(), F_GETFL);
    const int receiverFlags = ::fcntl(channel.receiver.get(), F_GETFL);
    const auto started = std::chrono::steady_clock::now();
    auto received = receiveHandoverRecord(channel.receiver.get(), 1, 0, 30);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::TimedOut);
    std::vector<std::uint8_t> bytes(1024 * 1024);
    const int bufferBytes = 4096;
    REQUIRE(::setsockopt(channel.sender.get(), SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes)) == 0);
    auto sent = sendHandoverRecord(channel.sender.get(), bytes, {}, 30);
    REQUIRE(!sent);
    CHECK(sent.error() == HandoverChannelError::TimedOut);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK_EQ(::fcntl(channel.sender.get(), F_GETFL), senderFlags);
    CHECK_EQ(::fcntl(channel.receiver.get(), F_GETFL), receiverFlags);
}

TEST(successfulRecordsPreserveBlockingAndNonblockingChannelFlags)
{
    for (const int flags : { 0, O_NONBLOCK }) {
        Channel channel;
        REQUIRE(::fcntl(channel.sender.get(), F_SETFL, flags) == 0);
        REQUIRE(::fcntl(channel.receiver.get(), F_SETFL, flags) == 0);
        const int senderFlags = ::fcntl(channel.sender.get(), F_GETFL);
        const int receiverFlags = ::fcntl(channel.receiver.get(), F_GETFL);
        REQUIRE(sendHandoverRecord(channel.sender.get(), payload, {}, timeoutMilliseconds));
        REQUIRE(receiveHandoverRecord(channel.receiver.get(), payload.size(), 0, timeoutMilliseconds));
        CHECK_EQ(::fcntl(channel.sender.get(), F_GETFL), senderFlags);
        CHECK_EQ(::fcntl(channel.receiver.get(), F_GETFL), receiverFlags);
    }
}

TEST(aPeerClosedBeforeSendingItsDescriptorIsReportedClosed)
{
    Channel channel;
    channel.sender.reset();
    auto received = receiveHandoverRecord(channel.receiver.get(), 8, 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::Closed);
}

TEST(missingDescriptorsAreRefused)
{
    Channel channel;
    REQUIRE(sendHandoverRecord(channel.sender.get(), {}, {}, timeoutMilliseconds));
    auto received = receiveHandoverRecord(channel.receiver.get(), 0, 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::DescriptorCount);
}

TEST(aTimeoutAfterReceivingDescriptorsClosesThem)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    const std::array<std::uint8_t, 4> header { 3, 0, 0, 0 };
    const auto before = openDescriptorCount();
    rawSend(channel.sender.get(), header, descriptors);
    auto received = receiveHandoverRecord(channel.receiver.get(), 3, 1, 30);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::TimedOut);
    CHECK_EQ(openDescriptorCount(), before);
}

TEST(payloadAllocationRefusalClosesEveryReceivedDescriptor)
{
    Channel channel;
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const std::array<int, 1> descriptors { pipe.read.get() };
    REQUIRE(sendHandoverRecord(channel.sender.get(), payload, descriptors, timeoutMilliseconds));
    const auto before = openDescriptorCount();
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal(payload.size());
    auto received = receiveHandoverRecord(channel.receiver.get(), payload.size(), 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::AllocationRefused);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusals + 1);
    CHECK_EQ(openDescriptorCount(), before);
    CHECK(::fcntl(pipe.read.get(), F_GETFD) >= 0);
}

TEST(partialProgressDoesNotRestartTheRecordDeadline)
{
    Channel channel;
    const std::array<std::uint8_t, 9> bytes { 5, 0, 0, 0, 1, 2, 3, 4, 5 };
    std::thread writer([&] {
        for (const auto byte : bytes) {
            rawSend(channel.sender.get(), { &byte, 1 });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    auto received = receiveHandoverRecord(channel.receiver.get(), 8, 0, 60);
    writer.join();
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::TimedOut);
}

TEST(invalidCallsDoNotConsumeTheChannel)
{
    Channel channel;
    auto invalid = sendHandoverRecord(channel.sender.get(), payload, {}, 0);
    REQUIRE(!invalid);
    CHECK(invalid.error() == HandoverChannelError::InvalidArgument);
    const std::array<int, 1> badDescriptor { -1 };
    invalid = sendHandoverRecord(channel.sender.get(), payload, badDescriptor, timeoutMilliseconds);
    REQUIRE(!invalid);
    CHECK(invalid.error() == HandoverChannelError::InvalidArgument);
    auto received = receiveHandoverRecord(channel.receiver.get(), 8, maxHandoverDescriptors + 1, timeoutMilliseconds);
    REQUIRE(!received);
    CHECK(received.error() == HandoverChannelError::DescriptorCount);
    REQUIRE(sendHandoverRecord(channel.sender.get(), payload, {}, timeoutMilliseconds));
    REQUIRE(receiveHandoverRecord(channel.receiver.get(), payload.size(), 0, timeoutMilliseconds));
}

TEST(theSingleDescriptorWrapperStillClosesTruncatedBatches)
{
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const int descriptor = pipe.write.release();
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    msghdr message {};
    message.msg_flags = MSG_CTRUNC;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    CHECK(!takeReceivedDescriptor(message));
    CHECK_EQ(::fcntl(descriptor, F_GETFD), -1);
    char byte = 0;
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0);
}

TEST(aClosedReceiverDoesNotRaiseABrokenPipeSignal)
{
    Channel channel;
    channel.receiver.reset();
    auto sent = sendHandoverRecord(channel.sender.get(), payload, {}, timeoutMilliseconds);
    REQUIRE(!sent);
    CHECK(sent.error() == HandoverChannelError::Closed);
}

UBIQUITTY_TEST_MAIN()
