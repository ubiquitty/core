// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Host/TerminationClient.h"
#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <string_view>
#include <sys/socket.h>

using namespace Ubiquitty;

namespace {

namespace Sync = Protocol::Sync;
namespace Control = Protocol::Control;

constexpr std::uint32_t sessionID = 42;
constexpr std::string_view requestID = "fixture.terminate.half-close";

template<typename Message>
std::string frame(Sync::FrameKind kind, const Message& message)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, message);
    std::vector<std::uint8_t> bytes;
    if (!Sync::appendFrame(bytes, kind, 0, body))
        return {};
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

class TerminationPeer {
public:
    TerminationPeer()
    {
        std::array<int, 2> descriptors { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
            return;
        m_client.reset(descriptors[0]);
        m_peer.reset(descriptors[1]);
        m_connection.fileDescriptor = m_client.get();
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = "fixture";
        greeting.clientInstance = { .low = 1 };
        greeting.request.target = Sync::AdmissionTarget::Session;
        greeting.request.sessionID = sessionID;
        greeting.request.isAttachOnly = true;
        greeting.request.expectedDaemonInstanceID = 1;
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        m_connection.greeting = greeting;
        Sync::Welcome welcome;
        welcome.wireVersion = Sync::wireVersion;
        welcome.appVersion = "fixture";
        welcome.hostInstanceID = { .low = 2 };
        welcome.attachmentID = { .low = 3 };
        welcome.resumeSecret = { .low = 4 };
        welcome.selection.target = Sync::AdmissionTarget::Session;
        welcome.selection.sessionID = sessionID;
        welcome.selection.daemonInstanceID = 1;
        welcome.selection.socketName = "host.sock";
        m_connection.rx = frame(Sync::FrameKind::Welcome, welcome)
            + frame(Sync::FrameKind::PaneBind, Sync::PaneBind { .handle = 1, .paneID = { .low = 5 } });
        HostTest::parseFrames(m_connection);
    }

    [[nodiscard]] bool isReady() const
    {
        return m_client.isValid() && m_peer.isValid() && !m_connection.hasFailed
            && m_connection.greeting && !HostTest::greetFrame(*m_connection.greeting).empty()
            && m_connection.welcome && m_connection.paneHandle == 1;
    }

    HostTest::Connection& connection() { return m_connection; }

    bool closeClientOutput() { return ::shutdown(m_client.get(), SHUT_WR) == 0; }

    bool forceEchoAfterCall()
    {
        // This frame has already reached the fixture transcript, but not the
        // independent termination reader. wait() must produce an Echo before
        // reading the pending answer, without waiting for an Ack timer.
        m_connection.rx += frame(Sync::FrameKind::Probe, Sync::Probe { .time = 1 });
        HostTest::parseFrames(m_connection);
        return !m_connection.hasFailed && closeClientOutput();
    }

    bool queueResult(std::uint32_t target, std::string_view identity, std::string_view outcome = "terminated")
    {
        using Codec::JSON::Value;
        auto result = Value::object();
        result.set("session", Value::number(target));
        result.set("outcome", Value::string(std::string(outcome)));
        const auto id = Value::string(std::string(identity));
        const auto answer = Control::rpcResult(&id, std::move(result));
        return HostTest::writeToDaemon(m_peer.get(), frame(Sync::FrameKind::Result, Sync::Result { .transfer = 0, .text = Codec::JSON::serialize(answer) }));
    }

    bool queueMalformedResult()
    {
        const std::array<std::uint8_t, 1> body { 0xff };
        std::vector<std::uint8_t> bytes;
        return Sync::appendFrame(bytes, Sync::FrameKind::Result, 0, body)
            && HostTest::writeToDaemon(m_peer.get(), { reinterpret_cast<const char*>(bytes.data()), bytes.size() });
    }

    bool finish()
    {
        return HostTest::writeToDaemon(m_peer.get(),
                   frame(Sync::FrameKind::PaneUnbind, Sync::PaneUnbind { .handle = 1, .reason = 0 })
                       + frame(Sync::FrameKind::Bye, Sync::Bye { .reason = Sync::ByeReason::Normal }))
            && ::shutdown(m_peer.get(), SHUT_WR) == 0;
    }

private:
    PAL::UnixFileDescriptor m_client;
    PAL::UnixFileDescriptor m_peer;
    HostTest::Connection m_connection;
};

} // namespace

TEST(terminationResultSurvivesFailedServiceOutput)
{
    TerminationPeer peer;
    CHECK(peer.isReady());
    if (!peer.isReady())
        return;
    HostTest::TerminationClient client(peer.connection());
    CHECK(client.send(sessionID, std::string(requestID)));
    CHECK(peer.forceEchoAfterCall());
    CHECK(peer.queueResult(sessionID, requestID));
    CHECK(peer.finish());
    CHECK(client.wait() == Control::TerminationOutcome::Terminated);
    CHECK(!peer.connection().hasFailed);
    std::string body;
    CHECK(HostTest::waitFor(peer.connection(), Sync::FrameKind::PaneUnbind, body, 0));
    CHECK(std::ranges::none_of(peer.connection().frames, [](const auto& received) { return received.kind == Sync::FrameKind::Result; }));
}

TEST(terminationRefusalSurvivesFailedServiceOutput)
{
    TerminationPeer peer;
    CHECK(peer.isReady());
    if (!peer.isReady())
        return;
    HostTest::TerminationClient client(peer.connection());
    CHECK(client.send(sessionID, std::string(requestID)));
    CHECK(peer.forceEchoAfterCall());
    CHECK(peer.queueResult(sessionID, requestID, "denied"));
    CHECK(peer.finish());
    CHECK(client.wait() == Control::TerminationOutcome::Denied);
}

TEST(terminationWithoutExactResultStillFailsAfterServiceOutputCloses)
{
    enum class Reply {
        Missing,
        WrongRequest,
        WrongSession,
        Malformed
    };
    for (const auto reply : { Reply::Missing, Reply::WrongRequest, Reply::WrongSession, Reply::Malformed }) {
        TerminationPeer peer;
        CHECK(peer.isReady());
        if (!peer.isReady())
            return;
        HostTest::TerminationClient client(peer.connection());
        CHECK(client.send(sessionID, std::string(requestID)));
        CHECK(peer.forceEchoAfterCall());
        switch (reply) {
        case Reply::Missing:
            break;
        case Reply::WrongRequest:
            CHECK(peer.queueResult(sessionID, "unrelated"));
            break;
        case Reply::WrongSession:
            CHECK(peer.queueResult(sessionID + 1, requestID));
            break;
        case Reply::Malformed:
            CHECK(peer.queueMalformedResult());
            break;
        }
        CHECK(peer.finish());
        CHECK(client.wait() == Control::TerminationOutcome::Failed);
    }
}

TEST(terminationCallWriteFailureStillFailsWithReadableResult)
{
    TerminationPeer peer;
    CHECK(peer.isReady());
    if (!peer.isReady())
        return;
    HostTest::TerminationClient client(peer.connection());
    CHECK(peer.closeClientOutput());
    CHECK(peer.queueResult(sessionID, requestID));
    CHECK(peer.finish());
    CHECK(!client.send(sessionID, std::string(requestID)));
    CHECK(client.wait() == Control::TerminationOutcome::Failed);
}

UBIQUITTY_TEST_MAIN()
