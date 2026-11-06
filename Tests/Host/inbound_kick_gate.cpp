// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A connection closed mid-read cannot apply the suffix of the same recvmsg.
// After real admission, one socket write contains duplicate Greet then palette
// Facts. The duplicate closes admission; its following Facts must not present
// a palette or reach the pane's program. A separately admitted positive peer
// applies those identical Facts, so refusal is not an invalid-body accident.
//
// This exercises the combined canonical-reader and daemon ingress fences, not
// one historical guard in isolation. The pane uses a capturing stream; the
// transport is a real owned socketpair, with no live PTY or child process.

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"

#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Captures every writeAll'd byte instead of driving a real PTY.
class CapturingStream final : public PAL::Stream {
public:
    int read(std::span<char>) override { return static_cast<int>(PAL::StreamStatus::EndOfFile); }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override
    {
        captured.append(bytes.data(), bytes.size());
        return static_cast<int>(bytes.size());
    }
    bool drainPendingWrite() override { return false; }
    bool hasPendingWrite() const override { return false; }
    void setWindowSize(int, int, int, int) override { }
    int lastError() const override { return 0; }
    int fileDescriptor() const override { return -1; }
    PAL::ProcessFacts& process() override { return m_facts; }

    std::string captured;

private:
    class Facts final : public PAL::ProcessFacts {
    public:
        std::string foregroundName() override { return {}; }
        bool foregroundIsShell() override { return true; }
        std::optional<PAL::ProcessID> foregroundProcessGroup() override { return std::nullopt; }
        std::string foregroundCurrentDirectory() override { return {}; }
        std::string ttyName() override { return {}; }
        bool hasRunningChild() override { return false; }
        int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }
    } m_facts;
};

// Friend seam: pre-seed a real session (no fork), add a client bound to a
// real socketpair fd (recvmsg needs a real fd — this is the exact call
// serviceClient makes), complete actual admission, then drive serviceClient
// exactly as the run loop's pfds dispatch does. No admitted state is preseeded.
struct InboundKickGateAccess {
    struct AdmittedClient {
        std::size_t index;
        std::vector<std::uint8_t> greeting;
    };

    static std::optional<AdmittedClient> addClient(Daemon& d, PAL::UnixFileDescriptor fileDescriptor, std::uint32_t sessionID)
    {
        namespace Sync = Protocol::Sync;
        const auto index = ScreenPlaneAccess::connect(d, std::move(fileDescriptor));
        ScreenPlaneAccess::markTrustedLocal(d, index);
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { index + 1, 0x6b69636b };
        greeting.requestedRole = Sync::Role::Owner;
        greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = ScreenPlaneAccess::daemonInstanceID(d), .isAttachOnly = true };
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        std::vector<std::uint8_t> frame;
        if (!Sync::decodeGreet(body) || !Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body))
            return std::nullopt;
        ScreenPlaneAccess::receive(d, index, frame);
        const auto* connection = ScreenPlaneAccess::connection(d, index);
        if (!connection || connection->state() != Sync::ConnectionState::Attached
            || !d.m_clients[index].helloSeen || d.m_clients[index].sessionID != sessionID)
            return std::nullopt;
        return AdmittedClient { index, std::move(frame) };
    }

    static void driveServiceClient(Daemon& d, std::size_t index) { d.serviceClient(index); }

    /// Whether the daemon took a theme from a client: what palette Facts it
    /// dispatched leaves behind.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - i: The client.
    /// - Returns: Whether the client presents a palette.
    static bool presentsPalette(const Daemon& d, std::size_t i) { return d.m_clients[i].presented.palette.has_value(); }
    static bool didOverflow(const Daemon& d, std::size_t i) { return d.m_clients[i].didOverflow; }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::CapturingStream;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::InboundKickGateAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

static int failures = 0;
static void check(bool ok, const char* msg) // bool-ok: established test-harness assert pattern (Tests/Host/*.cpp)
{
    if (!ok) {
        std::fprintf(stderr, "inbound_kick_gate: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    // As the daemon's own main does: a peer gone mid-write is an error to
    // read, not a signal.
    ::signal(SIGPIPE, SIG_IGN);
    Daemon d;
    auto ownedStream = std::make_unique<CapturingStream>();
    CapturingStream* stream = ownedStream.get();
    const std::uint32_t sessionID = Ubiquitty::Host::ScreenPlaneAccess::makeSession(d, 24, 80, std::move(ownedStream)).id;

    std::array<int, 2> fileDescriptors { -1, -1 };
    const bool hasPeer = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fileDescriptors.data()) == 0;
    check(hasPeer, "socketpair succeeds");
    if (!hasPeer)
        return 1;
    check(::fcntl(fileDescriptors[0], F_SETFL, O_NONBLOCK) == 0, "the fixture host socket is nonblocking");
    const auto admitted = InboundKickGateAccess::addClient(d, Ubiquitty::PAL::UnixFileDescriptor(fileDescriptors[0]), sessionID);
    check(admitted.has_value(), "the peer completes actual admission for the exact fixture session");
    if (!admitted)
        return 1;
    const std::size_t index = admitted->index;
    check(!InboundKickGateAccess::presentsPalette(d, index), "the admitted greeting carries no palette");

    // Coalesce [duplicate Greet][palette Facts] into one write, so a single recvmsg
    // inside serviceClient decodes both frames from one buffer — exactly the
    // same-batch shape the inbound gate guards.
    std::vector<std::uint8_t> batch = admitted->greeting;
    Sync::Facts facts;
    facts.viewportRows = 24;
    facts.viewportColumns = 80;
    facts.themePalette.emplace();
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, facts);
    std::vector<std::uint8_t> palette;
    check(Sync::appendFrame(palette, Sync::FrameKind::Facts, 0, body), "direct palette Facts encodes");
    batch.insert(batch.end(), palette.begin(), palette.end());

    check(::write(fileDescriptors[1], batch.data(), batch.size()) == static_cast<ssize_t>(batch.size()), "test peer writes the coalesced [duplicate Greet][palette Facts] batch");

    InboundKickGateAccess::driveServiceClient(d, index);

    // The duplicate Greet after actual admission closes this connection in
    // the read that decoded it.
    const auto* kicked = Ubiquitty::Host::ScreenPlaneAccess::connection(d, index);
    check(kicked && kicked->state() == Sync::ConnectionState::Closed, "the duplicate Greet closes the connection mid-pass");

    // The assertion the inbound access gate is about: the frame decoded from the same
    // batch, after the kick, must never be acted on.
    check(!InboundKickGateAccess::presentsPalette(d, index),
        "a frame decoded from the same batch AFTER the kick must not be dispatched — "
        "a just-kicked client must not still drive in the same service pass");
    check(stream->captured.empty(), "nothing reached the session's PTY");
    // The service pass after the read takes the closed connection out: the
    // client is dropped, or drains its Bye before the reap.
    Ubiquitty::Host::ScreenPlaneAccess::service(d);
    check(InboundKickGateAccess::didOverflow(d, index) || Ubiquitty::Host::ScreenPlaneAccess::isLeaving(d, index),
        "the closed connection was neither dropped nor left draining its Bye");
    check(!InboundKickGateAccess::presentsPalette(d, index), "the service pass applied a frame read after the kick");

    // The same frame alone, from a client in good standing, is acted on: the
    // refusal above is the kick's and not the frame's.
    std::array<int, 2> controlDescriptors { -1, -1 };
    const bool hasControl = ::socketpair(AF_UNIX, SOCK_STREAM, 0, controlDescriptors.data()) == 0;
    check(hasControl, "socketpair succeeds");
    if (!hasControl)
        return 1;
    // As the daemon holds an accepted socket: a client in good standing is
    // read until its socket is empty, not until it closes.
    (void)::fcntl(controlDescriptors[0], F_SETFL, ::fcntl(controlDescriptors[0], F_GETFL) | O_NONBLOCK);
    const auto admittedControl = InboundKickGateAccess::addClient(d, Ubiquitty::PAL::UnixFileDescriptor(controlDescriptors[0]), sessionID);
    check(admittedControl.has_value(), "the control peer completes independent actual admission");
    if (!admittedControl)
        return 1;
    const std::size_t control = admittedControl->index;
    check(::write(controlDescriptors[1], palette.data(), palette.size()) == static_cast<ssize_t>(palette.size()), "the control client writes its palette Facts");
    InboundKickGateAccess::driveServiceClient(d, control);
    check(InboundKickGateAccess::presentsPalette(d, control), "premise: palette Facts from a client in good standing are dispatched");

    ::close(fileDescriptors[1]);
    ::close(controlDescriptors[1]);

    if (failures == 0)
        std::printf("inbound_kick_gate: a duplicate Greet closes the connection in the read that "
                    "decoded it, the frames after it in the same batch are never dispatched, and "
                    "the next service pass drops the client or drains its Bye\n");
    return failures == 0 ? 0 : 1;
}
