// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Reclaim's existing-owner semantics are covered by the pure
// RemoteSessionCore cases. This fixture checks the descriptor-aware local
// adoption exchange from the client's side: Ready, a Prepare carrying the
// master, a Prepared outcome, then Commit and a Committed outcome. Every way
// the Prepared answer can be wrong leaves the local endpoint its owner, and
// every unexpected descriptor in the Ready or outcome replies is closed. It
// opens owned test sockets and is not a pure-memory test.

#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Session/SessionShare.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/POSIX/ReceivedDescriptor.h"
#include "Platform/POSIX/SessionHandoff.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <vector>

using namespace Ubiquitty;
namespace Local = Protocol::Local;

namespace {

enum class Reply {
    Valid,
    Fragmented,
    ClosedAfterCommitted,
    Missing,
    Malformed,
    WrongCorrelation,
    WrongDaemon,
    UnexpectedDescriptor,
    ReadyDescriptor,
    MultipleDescriptors,
    ThreeDescriptors
};

std::optional<std::uint64_t> selectOperation(int descriptor, int unexpectedDescriptor)
{
    std::array<std::uint8_t, Local::localControlRequestBytes> bytes {};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        alignas(cmsghdr) char control[POSIX::receivedDescriptorControlCapacity];
        iovec part { bytes.data() + offset, bytes.size() - offset };
        msghdr message {};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        const auto count = ::recvmsg(descriptor, &message, POSIX::receivedDescriptorFlags);
        if (count <= 0)
            return std::nullopt;
        const auto received = POSIX::takeReceivedDescriptor(message);
        if (!received || *received)
            return std::nullopt; // The descriptor must wait for Ready.
        offset += static_cast<std::size_t>(count);
    }
    Local::LocalControlReader reader;
    if (reader.consume(bytes) != Local::LocalControlReader::State::Ready)
        return std::nullopt;
    const auto request = reader.take();
    if (!request || request->operation != Local::LocalControlOperation::SessionHandoff)
        return std::nullopt;
    const auto ready = Local::encodeLocalHandoffReady({ .correlation = request->correlation,
        .daemonInstanceID = 17,
        .status = Local::HandoffReadyStatus::Ready });
    iovec part { const_cast<std::uint8_t*>(ready.data()), ready.size() };
    msghdr message {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    if (unexpectedDescriptor >= 0) {
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &unexpectedDescriptor, sizeof(int));
    }
    if (::sendmsg(descriptor, &message, 0) != static_cast<ssize_t>(ready.size()))
        return std::nullopt;
    return request->correlation;
}

/// The parts of a received Prepare that its answers must echo, and the master
/// it carried, owned here until the peer ends.
struct ReceivedPrepare {
    std::uint64_t correlation = 0;
    Local::AdoptWireOperation operation;
    PAL::UnixFileDescriptor master;
};

bool receiveAll(int socket, std::span<std::uint8_t> bytes, PAL::UnixFileDescriptor* master)
{
    while (!bytes.empty()) {
        alignas(cmsghdr) char control[POSIX::receivedDescriptorControlCapacity];
        iovec part { bytes.data(), bytes.size() };
        msghdr message {};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        const auto count = ::recvmsg(socket, &message, POSIX::receivedDescriptorFlags);
        if (count <= 0)
            return false;
        auto received = POSIX::takeReceivedDescriptor(message);
        if (!received || (*received && (!master || *master)))
            return false;
        if (*received)
            *master = std::move(*received);
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

/// Reads one whole Prepare. The master must ride with it, close-on-exec, and
/// be the only descriptor the record carries.
std::optional<ReceivedPrepare> readPrepare(int socket)
{
    ReceivedPrepare received;
    std::vector<std::uint8_t> record(Local::adoptWirePrepareHeaderBytes);
    if (!receiveAll(socket, record, &received.master) || !received.master)
        return std::nullopt;
    const auto limit = Session::localShareHandoffLimits().maxRecordBytes;
    const auto header = Local::decodeAdoptWireHeader(std::span(record).first(Local::adoptWireFrameHeaderBytes), limit);
    if (!header || header->kind != Local::AdoptWireMessage::Prepare)
        return std::nullopt;
    record.resize(Local::adoptWireFrameHeaderBytes + header->bodyBytes);
    if (!receiveAll(socket, std::span(record).subspan(Local::adoptWirePrepareHeaderBytes), nullptr))
        return std::nullopt;
    const auto prepare = Local::decodeAdoptWirePrepare(record, limit);
    if (!prepare || (::fcntl(received.master.get(), F_GETFD) & FD_CLOEXEC) == 0)
        return std::nullopt;
    received.correlation = prepare->correlation;
    received.operation = prepare->preparation.operation;
    return received;
}

/// How a record reaches the client: in one write, or split after its first
/// byte so the client proves it reassembles.
enum class Delivery : std::uint8_t {
    Whole,
    Fragmented,
};

/// Whether the peer answers Prepare with a decodable, matching Prepared and so
/// goes on to answer the client's Commit.
bool answersCorrectly(Reply reply)
{
    return reply == Reply::Valid || reply == Reply::Fragmented || reply == Reply::ClosedAfterCommitted;
}

/// Sends one record, whole or as its first byte and then the rest, with
/// `descriptorCount` copies of `descriptor` riding on its first byte.
bool sendRecord(int socket, std::span<const std::uint8_t> bytes, int descriptor, std::size_t descriptorCount, Delivery delivery)
{
    const std::array<int, 3> descriptors { descriptor, descriptor, descriptor };
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(descriptors))> control {};
    const std::size_t firstLength = delivery == Delivery::Fragmented ? 1 : bytes.size();
    iovec part { const_cast<std::uint8_t*>(bytes.data()), firstLength };
    msghdr message {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    if (descriptorCount) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(descriptorCount * sizeof(int));
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(descriptorCount * sizeof(int));
        std::memcpy(CMSG_DATA(header), descriptors.data(), descriptorCount * sizeof(int));
    }
    if (::sendmsg(socket, &message, 0) != static_cast<ssize_t>(firstLength))
        return false;
    return delivery == Delivery::Whole
        || ::write(socket, bytes.data() + firstLength, bytes.size() - firstLength) == static_cast<ssize_t>(bytes.size() - firstLength);
}

/// Answers the Prepare as `reply` names. Valid, Fragmented, and
/// ClosedAfterCommitted answer correctly and go on to answer the client's
/// Commit; every other reply is a wrong answer where Prepared belongs, after
/// which the client sends nothing.
bool answerPrepare(int socket, int descriptor, const ReceivedPrepare& prepare, Reply reply)
{
    const Local::AdoptWireTarget target { .sessionID = 7, .paneID = { .low = 0x7a, .high = 0x7b } };
    if (reply == Reply::Missing) {
        // A record that is no answer at all stands where the outcome belongs.
        const auto status = Local::encodeAdoptWireCommand({ .kind = Local::AdoptWireMessage::Status,
            .correlation = prepare.correlation,
            .operation = prepare.operation });
        return status && sendRecord(socket, *status, descriptor, 0, Delivery::Whole);
    }
    auto operation = prepare.operation;
    if (reply == Reply::WrongDaemon)
        operation.daemonInstanceID = 18;
    auto prepared = Local::encodeAdoptWireOutcome({ .correlation = prepare.correlation + (reply == Reply::WrongCorrelation ? 1 : 0),
        .operation = operation,
        .disposition = Local::AdoptWireDisposition::Prepared,
        .target = target });
    if (!prepared)
        return false;
    if (reply == Reply::Malformed) {
        // Framing intact, disposition undefined: no decodable outcome.
        (*prepared)[Local::adoptWireCommandBytes] = 0xff;
    }
    const std::size_t descriptorCount = reply == Reply::ThreeDescriptors ? 3 : reply == Reply::MultipleDescriptors ? 2
        : reply == Reply::UnexpectedDescriptor                                                                     ? 1
                                                                                                                   : 0;
    const Delivery delivery = reply == Reply::Fragmented ? Delivery::Fragmented : Delivery::Whole;
    if (!sendRecord(socket, *prepared, descriptor, descriptorCount, delivery))
        return false;
    if (!answersCorrectly(reply))
        return true;
    std::array<std::uint8_t, Local::adoptWireCommandBytes> commandBytes {};
    if (!receiveAll(socket, commandBytes, nullptr))
        return false;
    const auto command = Local::decodeAdoptWireCommand(commandBytes);
    if (!command || command->kind != Local::AdoptWireMessage::Commit || command->operation != prepare.operation)
        return false;
    const auto committed = Local::encodeAdoptWireOutcome({ .correlation = command->correlation,
        .operation = prepare.operation,
        .disposition = Local::AdoptWireDisposition::Committed,
        .target = target });
    return committed && sendRecord(socket, *committed, descriptor, 0, delivery);
}

void checkReply(Reply reply)
{
    std::array<char, 32> directory {};
    std::strcpy(directory.data(), "/tmp/ubq-adopt-XXXXXX");
    REQUIRE(::mkdtemp(directory.data()));
    const std::string path = std::string(directory.data()) + "/socket";
    struct Cleanup {
        const std::string& path;
        const char* directory;
        ~Cleanup()
        {
            ::unlink(path.c_str());
            ::rmdir(directory);
        }
    } cleanup { path, directory.data() };
    PAL::UnixFileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM, 0));
    REQUIRE(listener);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    REQUIRE(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener.get(), 1) == 0);
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe.isValid());
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, O_NONBLOCK) == 0);
    std::atomic<bool> peerSucceeded { false };
    std::thread peer([&] {
        PAL::UnixFileDescriptor connection(::accept(listener.get(), nullptr, nullptr));
        if (!connection)
            return;
        timeval timeout { 3, 0 };
        if (::setsockopt(connection.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            return;
        const auto correlation = selectOperation(connection.get(), reply == Reply::ReadyDescriptor ? pipe.write.get() : -1);
        if (!correlation)
            return;
        if (reply == Reply::ReadyDescriptor) {
            peerSucceeded.store(true);
            return;
        }
        const auto prepare = readPrepare(connection.get());
        if (!prepare)
            return;
        const bool didAnswer = answerPrepare(connection.get(), pipe.write.get(), *prepare, reply);
        // The daemon holds a completed exchange until the client hangs up. A
        // peer that closes at once leaves Committed buffered behind end-of-file,
        // and the client still owes it a read.
        if (reply == Reply::ClosedAfterCommitted) {
            peerSucceeded.store(didAnswer);
            return;
        }
        // The hang-up is end of file, or a reset where the client closed with
        // bytes it never read, as Linux reports it. A byte is the client
        // speaking after the exchange, and any other error is the receive
        // timeout passing first.
        char byte = 0;
        const ssize_t count = ::read(connection.get(), &byte, 1);
        const bool didHangUp = count == 0 || (count < 0 && errno == ECONNRESET);
        peerSucceeded.store(didAnswer && didHangUp);
    });
    // The Terminal body is opaque to the exchange; the peer checks only its digest.
    const std::vector<std::uint8_t> terminalBody(256, 0x5a);
    const auto adopted = Session::reHomeViaAdopt(path.c_str(), std::as_bytes(std::span(terminalBody)), pipe.write.get(), Session::localShareHandoffLimits());
    peer.join();
    CHECK(peerSucceeded.load());
    const bool shouldSucceed = answersCorrectly(reply);
    // A wrong answer to Prepare comes before any Commit byte, so the local
    // endpoint is proven its own owner rather than left unresolved.
    CHECK(adopted.ownership == (shouldSucceed ? Session::ShareOwnership::Committed : Session::ShareOwnership::Local));
    CHECK_EQ(adopted.adopted.sessionID, shouldSucceed ? 7u : 0u);
    CHECK_EQ(adopted.adopted.daemonInstanceID, shouldSucceed ? 17u : 0u);
    pipe.write.reset();
    char byte = 0;
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0); // No received descriptor survived.
}

}

TEST(testLocalAdoptRequiresMatchingAcknowledgment) { checkReply(Reply::Valid); }
TEST(testLocalAdoptAcceptsFragmentedAcknowledgment) { checkReply(Reply::Fragmented); }
TEST(testLocalAdoptReadsCommittedBufferedBeforeEndOfFile) { checkReply(Reply::ClosedAfterCommitted); }
TEST(testLocalAdoptRejectsMissingAcknowledgment) { checkReply(Reply::Missing); }
TEST(testLocalAdoptRejectsMalformedAcknowledgment) { checkReply(Reply::Malformed); }
TEST(testLocalAdoptRejectsWrongCorrelation) { checkReply(Reply::WrongCorrelation); }
TEST(testLocalAdoptRejectsWrongDaemon) { checkReply(Reply::WrongDaemon); }
TEST(testLocalAdoptRejectsUnexpectedDescriptor) { checkReply(Reply::UnexpectedDescriptor); }
TEST(testLocalAdoptRejectsDescriptorOnReady) { checkReply(Reply::ReadyDescriptor); }
TEST(testLocalAdoptRejectsMultipleDescriptors) { checkReply(Reply::MultipleDescriptors); }
TEST(testLocalAdoptRejectsThreeDescriptors) { checkReply(Reply::ThreeDescriptors); }

UBIQUITTY_TEST_MAIN()
