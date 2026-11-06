// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/partial_frame_budget.cpp — canonical header/body admission and the
// silence deadline for an incomplete, otherwise admitted frame.
//
// A remote envelope carries at most 65,535 body bytes, so even all 64 daemon
// clients holding one maximum canonical body cannot reach the physical
// aggregate allowance, and nothing here is aggregate-pressure coverage.
// adopt_in_parts.cpp exercises the actual competing 36 MiB local reservations,
// replay charges and allocation refusal.
//
// This fixture proves:
//   - an over-cap header is refused before retaining any advertised body;
//   - an admitted partial body that goes silent is dropped by its deadline;
//   - a valid exact-cap host.describe Call gets its exact Result, followed by
//     a successful Probe/Echo on the same connection;
//   - both pre-admission and admitted over-cap headers get a typed Oversize Bye;
//   - partial progress rearms the deadline rather than expiring by total age.
//
// This is an actual Host/process fixture, not a pure test. Runtime is quarantined
// independently of source or compile validation. argv[1] = ubiquitty-host.
//

#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Scope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Local = Ubiquitty::Protocol::Local;
using Ubiquitty::HostTest::Connection;

constexpr auto maxDeclaredFrameBytes = static_cast<std::uint32_t>(Sync::maxFrameBodyBytes);
// These mirror Daemon.cpp's current client count and physical inbound allowance,
// not a new protocol cap. This comparison covers only one partial frame per
// client, not every other retained owner included in daemon accounting.
constexpr std::size_t daemonClientCount = 64;
constexpr std::size_t physicalAggregateAllowance = 4u * (Ubiquitty::Protocol::Sync::maxClientFramePayloadLength + 64u * 1024u) + Local::maxAdoptBodyBytes;
static_assert(daemonClientCount * (Sync::maxFrameBodyBytes + Sync::maxEnvelopeHeaderBytes) < physicalAggregateAllowance);

class DaemonProcess {
public:
    explicit DaemonProcess(pid_t processID)
        : m_processID(processID)
    {
    }
    DaemonProcess(const DaemonProcess&) = delete;
    DaemonProcess& operator=(const DaemonProcess&) = delete;
    ~DaemonProcess()
    {
        if (m_processID <= 0)
            return;
        ::kill(m_processID, SIGTERM);
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pid_t result = ::waitpid(m_processID, nullptr, WNOHANG);
            if (result == m_processID || (result < 0 && errno == ECHILD))
                return;
            ::poll(nullptr, 0, 10);
        }
        ::kill(m_processID, SIGKILL);
        while (::waitpid(m_processID, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    pid_t processID() const { return m_processID; }

private:
    pid_t m_processID = -1;
};

// Independent fixture ownership/fairness controls; they do not assert that the
// canonical Host can reach its aggregate allowance.
struct AggregatePeer {
    AggregatePeer() = default;
    AggregatePeer(const AggregatePeer&) = delete;
    AggregatePeer& operator=(const AggregatePeer&) = delete;
    ~AggregatePeer() { close(); }

    void close()
    {
        if (fileDescriptor >= 0) {
            ::close(fileDescriptor);
            fileDescriptor = -1;
        }
    }

    int fileDescriptor = -1;
    std::size_t sentBytes = 0;
};

bool makeNonblocking(int fileDescriptor)
{
    const int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::size_t advanceAggregatePeers(std::span<AggregatePeer> peers, std::span<const std::uint8_t> chunk, std::size_t maxSentBytes)
{
    std::size_t advancedBytes = 0;
    for (auto& peer : peers) {
        if (peer.fileDescriptor < 0 || peer.sentBytes >= maxSentBytes)
            continue;
        const std::size_t remaining = maxSentBytes - peer.sentBytes;
        const ssize_t written = ::write(peer.fileDescriptor, chunk.data(), std::min(remaining, chunk.size()));
        if (written > 0) {
            peer.sentBytes += static_cast<std::size_t>(written);
            advancedBytes += static_cast<std::size_t>(written);
        } else if (written == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            peer.close();
    }
    return advancedBytes;
}

bool fail(const char* message)
{
    std::fprintf(stderr, "partial_frame_budget: FAIL — %s\n", message);
    return false;
}

bool verifyAggregatePeerControls()
{
    std::array<int, 2> cleanupPair { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, cleanupPair.data()) != 0)
        return fail("cleanup control: socketpair");
    AggregatePeer unrelated;
    unrelated.fileDescriptor = cleanupPair[1];
    {
        std::array<AggregatePeer, 12> partiallyConnected;
        partiallyConnected[0].fileDescriptor = cleanupPair[0];
    }
    if (::fcntl(cleanupPair[0], F_GETFD) >= 0 || errno != EBADF || ::fcntl(unrelated.fileDescriptor, F_GETFD) < 0)
        return fail("cleanup control: closes owned peers and preserves unrelated descriptors");

    std::array<AggregatePeer, 2> peers;
    std::array<AggregatePeer, 2> receivers;
    for (std::size_t index = 0; index < peers.size(); ++index) {
        std::array<int, 2> pair { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0)
            return fail("fairness control: socketpair");
        peers[index].fileDescriptor = pair[0];
        receivers[index].fileDescriptor = pair[1];
        if (!makeNonblocking(peers[index].fileDescriptor))
            return fail("fairness control: nonblocking socket");
    }
    const std::array<std::uint8_t, 64u * 1024u> chunk {};
    constexpr std::size_t fillLimit = 16u * 1024u * 1024u;
    while (peers[0].sentBytes < fillLimit && advanceAggregatePeers(std::span(peers).first(1), chunk, fillLimit)) {
    }
    const std::size_t blockedBytes = peers[0].sentBytes;
    if (blockedBytes == fillLimit || !blockedBytes)
        return fail("fairness control: first peer must have a full socket");
    const std::size_t advanced = advanceAggregatePeers(peers, chunk, fillLimit);
    if (!advanced || peers[0].sentBytes != blockedBytes || peers[1].sentBytes != advanced)
        return fail("fairness control: blocked first peer must not stall the next peer");
    return true;
}

int connectDaemon(const std::string& path)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fileDescriptor < 0)
            return -1;
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fileDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
            return fileDescriptor;
        ::close(fileDescriptor);
        ::poll(nullptr, 0, 50);
    }
    return -1;
}

bool writeAllOrDropped(int fileDescriptor, std::span<const std::uint8_t> bytes)
{
    if (!makeNonblocking(fileDescriptor))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::size_t sent = 0;
    while (sent < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const auto count = ::write(fileDescriptor, bytes.data() + sent, bytes.size() - sent);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd writable { fileDescriptor, POLLOUT, 0 };
            ::poll(&writable, 1, 20);
            if (writable.revents & (POLLHUP | POLLERR))
                return false;
        } else if (count >= 0 || errno != EINTR)
            return false;
    }
    return sent == bytes.size();
}

bool waitDisconnected(int fileDescriptor, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd readable { fileDescriptor, POLLIN, 0 };
        if (::poll(&readable, 1, 50) <= 0)
            continue;
        if (readable.revents & (POLLHUP | POLLERR))
            return true;
        char buffer[4096];
        const auto count = ::read(fileDescriptor, buffer, sizeof(buffer));
        if (!count || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return true;
    }
    return false;
}

std::vector<std::uint8_t> declaredHeader(Sync::FrameKind kind, std::uint32_t bodyBytes)
{
    std::vector<std::uint8_t> header;
    Sync::WireWriter writer(header);
    writer.u8(std::to_underlying(kind));
    writer.varint(0);
    writer.varint(bodyBytes);
    return header;
}

// The actual canonical header reader refuses before the actual ingress can
// allocate the advertised body. This is an in-memory storage oracle, separate
// from the process cases below, which observe typed refusal and continued service.
bool verifyHeaderAdmission()
{
    const auto over = declaredHeader(Sync::FrameKind::Greet, maxDeclaredFrameBytes + 1);
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    Sync::EnvelopeError error = Sync::EnvelopeError::None;
    bool wasDelivered = false;
    const auto result = ingress.consume(over, [&](std::span<const std::uint8_t> prefix) {
        Sync::FrameHeader header;
        switch (Sync::readFrameHeader(prefix, header, error)) {
        case Sync::FrameHeaderState::NeedsMore:
            return Sync::FrameHeaderDecision {};
        case Sync::FrameHeaderState::Failed:
            return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Refused };
        case Sync::FrameHeaderState::Complete:
            return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
        }
        return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Refused }; }, [&](auto, auto) {
        wasDelivered = true;
        return Sync::FrameDelivery::Continue; });
    return result.state == Sync::FrameIngress::State::Refused && result.consumedBytes == over.size()
        && error == Sync::EnvelopeError::Oversize && ingress.retainedBytes() == 0 && !wasDelivered;
}

bool admitControl(Connection& connection, const std::string& token, const std::string& socketLeaf, std::uint64_t instance)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = "partial-budget";
    greeting.clientInstance = { 0xB0D6E7, instance };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.credential = std::vector<std::uint8_t>(token.begin(), token.end());
    std::string body;
    if (!Ubiquitty::HostTest::sendGreeting(connection, greeting)
        || !Ubiquitty::HostTest::waitFor(connection, Sync::FrameKind::Welcome, body, 3000) || !connection.welcome)
        return false;
    const auto& welcome = *connection.welcome;
    return welcome.admission == Sync::AttachmentAdmission::Granted && welcome.selection.target == Sync::AdmissionTarget::Control
        && !welcome.selection.sessionID && !welcome.selection.workspaceID && welcome.selection.daemonInstanceID
        && welcome.selection.socketName == socketLeaf && connection.panes.empty() && !connection.hasFailed;
}

void drainFor(int fileDescriptor, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd readable { fileDescriptor, POLLIN, 0 };
        if (::poll(&readable, 1, 10) <= 0 || !(readable.revents & POLLIN))
            continue;
        char sink[8192];
        if (::read(fileDescriptor, sink, sizeof(sink)) <= 0)
            return;
    }
}

std::string slurp(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return {};
    std::string output;
    char buffer[8192];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
        output.append(buffer, count);
    std::fclose(file);
    return output;
}

int countInboundDropLines(const std::string& log, const char* cause)
{
    int count = 0;
    const std::string prefix = "inbound budget dropped client ";
    for (std::size_t at = log.find(prefix); at != std::string::npos; at = log.find(prefix, at)) {
        const auto end = log.find('\n', at);
        if (log.substr(at, end == std::string::npos ? end : end - at).find(cause) != std::string::npos)
            ++count;
        at = end == std::string::npos ? log.size() : end + 1;
    }
    return count;
}

bool exactDescribeResult(Connection& connection)
{
    constexpr std::string_view correlation = "exact-cap";
    Sync::Call call { .text = R"({"jsonrpc":"2.0","id":"exact-cap","method":"host.describe","params":{}})" };
    call.text.resize(maxDeclaredFrameBytes, ' ');
    std::vector<std::uint8_t> body;
    Sync::encode(body, call);
    if (body.size() != maxDeclaredFrameBytes || !Sync::decodeCall(body))
        return false;
    std::vector<std::uint8_t> frame;
    if (!Sync::appendFrame(frame, Sync::FrameKind::Call, 0, body) || !writeAllOrDropped(connection.fileDescriptor, frame))
        return false;
    std::string received;
    if (!Ubiquitty::HostTest::waitFor(connection, Sync::FrameKind::Result, received, 3000))
        return false;
    const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(received.data()), received.size() });
    if (!result || result->transfer)
        return false;
    bool isJSON = false;
    const auto answer = Ubiquitty::Codec::JSON::parse(result->text, isJSON);
    const auto* description = answer.find("result");
    return isJSON && answer.isObject() && answer.members().size() == 3 && answer.stringOr("jsonrpc") == "2.0"
        && answer.stringOr("id") == correlation && !answer.find("error") && description && description->isObject()
        && description->members().size() == 3 && description->numberOr("wireVersion") == Sync::wireVersion
        && description->find("appVersion")
        && description->find("appVersion")->isString()
        && description->find("scopes")
        && description->find("scopes")->isArray();
}

bool probeIsEchoed(Connection& connection)
{
    constexpr std::uint64_t probeTime = 0x50465042;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Probe { .time = probeTime });
    if (!Ubiquitty::HostTest::sendFrame(connection, Sync::FrameKind::Probe, { reinterpret_cast<const char*>(body.data()), body.size() }))
        return false;
    std::string received;
    if (!Ubiquitty::HostTest::waitFor(connection, Sync::FrameKind::Echo, received, 3000))
        return false;
    const auto echo = Sync::decodeEcho({ reinterpret_cast<const std::uint8_t*>(received.data()), received.size() });
    return echo && echo->timeEcho == probeTime && !connection.hasFailed;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    if (!verifyHeaderAdmission()) {
        fail("an oversized canonical header retained a body or reached delivery");
        return 1;
    }
    if (!verifyAggregatePeerControls())
        return 1;

    char homeTemplate[] = "/tmp/ubiquitty-partial.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string socketLeaf = Ubiquitty::Host::socketLeafForBinary(argv[1]);
    const std::string socketPath = macHome + "/" + socketLeaf;
    ::mkdir(macHome.c_str(), 0700);
    const std::string token = "partialbudgettokenpartialbudgettoken1";
    {
        using Ubiquitty::Protocol::Sync::Scope;
        const auto scope = Ubiquitty::Protocol::Sync::ScopeSet { Scope::Control }.toRaw();
        FILE* file = std::fopen((macHome + "/tokens").c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 partialbudget\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scope));
        std::fclose(file);
    }
    const std::string daemonLog = macHome + "/daemon.log";
    DaemonProcess daemon(::fork());
    if (daemon.processID() < 0)
        return 1;
    if (!daemon.processID()) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("UBIQUITTY_PARTIAL_FRAME_DEADLINE_MS", "600", 1);
        const int logFileDescriptor = ::open(daemonLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (logFileDescriptor < 0 || ::dup2(logFileDescriptor, STDERR_FILENO) < 0)
            ::_exit(126);
        ::close(logFileDescriptor);
        ::execl(argv[1], argv[1], "--daemon", "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (!Ubiquitty::HostTest::waitForDaemonReady(socketPath)) {
        fail("daemon never accepted a connection");
        return 1;
    }

    // Every incomplete Call below begins after real Control admission. No
    // greeting grace, session creation, PTY echo, or fake admission can account
    // for success or for the completion-deadline drop.
    {
        AggregatePeer owned;
        owned.fileDescriptor = connectDaemon(socketPath);
        Connection stalled;
        stalled.fileDescriptor = owned.fileDescriptor;
        if (stalled.fileDescriptor < 0 || !admitControl(stalled, token, socketLeaf, 1)) {
            fail("deadline: Control admission");
            return 1;
        }
        const auto header = declaredHeader(Sync::FrameKind::Call, maxDeclaredFrameBytes);
        const std::array<std::uint8_t, 8192> partial {};
        if (!writeAllOrDropped(stalled.fileDescriptor, header) || !writeAllOrDropped(stalled.fileDescriptor, partial)
            || !waitDisconnected(stalled.fileDescriptor, 3000)
            || countInboundDropLines(slurp(daemonLog), "completion deadline") != 1) {
            fail("deadline: a silent admitted partial must be dropped specifically by completion deadline");
            return 1;
        }
    }
    {
        AggregatePeer owned;
        owned.fileDescriptor = connectDaemon(socketPath);
        Connection positive;
        positive.fileDescriptor = owned.fileDescriptor;
        if (positive.fileDescriptor < 0 || !admitControl(positive, token, socketLeaf, 2)
            || !exactDescribeResult(positive) || !probeIsEchoed(positive) || waitDisconnected(positive.fileDescriptor, 500)) {
            fail("positive: exact-cap valid Call must return its exact Result and continue serving");
            return 1;
        }
    }
    for (const bool isAdmitted : { false, true }) { // bool-ok: independent pre/post admission boundaries
        AggregatePeer owned;
        owned.fileDescriptor = connectDaemon(socketPath);
        Connection over;
        over.fileDescriptor = owned.fileDescriptor;
        if (over.fileDescriptor < 0 || (isAdmitted && !admitControl(over, token, socketLeaf, 3))) {
            fail("ceiling: connection/admission");
            return 1;
        }
        // No body follows: the exact header must suffice for typed refusal,
        // including before a Greet's advertised body could be allocated.
        const auto header = declaredHeader(isAdmitted ? Sync::FrameKind::Call : Sync::FrameKind::Greet, maxDeclaredFrameBytes + 1);
        std::string body;
        if (!writeAllOrDropped(over.fileDescriptor, header) || !Ubiquitty::HostTest::waitFor(over, Sync::FrameKind::Bye, body, 3000)) {
            fail("ceiling: over-cap header did not receive typed Bye without sending any body");
            return 1;
        }
        const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
        if (!bye || bye->reason != Sync::ByeReason::Oversize || !waitDisconnected(over.fileDescriptor, 3000)) {
            fail("ceiling: header must close with exact Oversize reason");
            return 1;
        }
    }
    {
        AggregatePeer owned;
        owned.fileDescriptor = connectDaemon(socketPath);
        Connection slow;
        slow.fileDescriptor = owned.fileDescriptor;
        if (slow.fileDescriptor < 0 || !admitControl(slow, token, socketLeaf, 4)) {
            fail("progress: Control admission");
            return 1;
        }
        const auto header = declaredHeader(Sync::FrameKind::Call, maxDeclaredFrameBytes);
        std::array<std::uint8_t, 4096> chunk;
        chunk.fill(' ');
        constexpr int chunkCount = 8;
        static_assert(chunk.size() * chunkCount < maxDeclaredFrameBytes);
        bool didSurvive = writeAllOrDropped(slow.fileDescriptor, header);
        for (int index = 0; index < chunkCount && didSurvive; ++index) {
            if (index)
                drainFor(slow.fileDescriptor, 200); // under the 600 ms silence deadline
            didSurvive = writeAllOrDropped(slow.fileDescriptor, chunk);
        }
        // Seven 200 ms gaps exceed the total age cap twice; only progress may
        // rearm it. The body stays incomplete and below the canonical ceiling.
        if (!didSurvive || waitDisconnected(slow.fileDescriptor, 500)) {
            fail("progress: advancing partial frame must not expire by total age");
            return 1;
        }
    }
    {
        AggregatePeer owned;
        owned.fileDescriptor = connectDaemon(socketPath);
        Connection healthy;
        healthy.fileDescriptor = owned.fileDescriptor;
        if (healthy.fileDescriptor < 0 || !admitControl(healthy, token, socketLeaf, 5) || !probeIsEchoed(healthy)) {
            fail("fresh Control connection must still be served after the negative cases");
            return 1;
        }
    }
    std::printf("partial_frame_budget: exact canonical cap, header-only Oversize, silence deadline, progress rearming and continued service\n");
    return 0;
}
