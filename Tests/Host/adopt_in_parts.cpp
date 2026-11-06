// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/adopt_in_parts.cpp — a shared window's complete continuation is
// prepared privately from one bounded raw record, then published only on an
// explicit Commit. Fragmentation, deadlines, aggregate admission, allocation
// refusal and retired one-phase frames cannot create a partial session.
//
// The window's shell is a child of this process that leads a session on a
// PTY, which is what the daemon's ownership check asks of an adopted master.
// Friend seam, the QUIC-off host build. Exit 0 on success.
//

#include "Assertions.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Crypto/SHA256.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/SessionShare.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Socket.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
bool refusesObservedAdoptAllocation(std::size_t);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return refusesObservedAdoptAllocation(size) || UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size) ? nullptr : ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size) ? nullptr : ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace Ubiquitty {
namespace Host {

/// Reaches the daemon's clients and sessions, as its accept loop, its frame
/// dispatch and its budget pass do.
struct AdoptInPartsAccess {
    /// Pins the executables a local adopt may come from, as a daemon does when
    /// it starts serving; one that never starts admits no adopt at all.
    static void trustLocalAdopt(Daemon& d) { d.captureLocalAdoptExecutables(); }

    /// Adds a same-user client with its actual kernel-verified process identity.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - socket: The daemon's end of the client's socket.
    /// - Returns: The client's id.
    static std::uint32_t addAdopter(Daemon& d, PAL::UnixFileDescriptor socket)
    {
        Daemon::Client client {};
        client.clientID = ++d.m_nextClientID;
        client.fileDescriptor = std::move(socket);
        client.hasOwnerLocalCredentials = PAL::peerIsSameUser(client.fileDescriptor);
        d.capturePeerIdentity(client);
        CHECK(client.hasOwnerLocalCredentials && client.peerProcessID == static_cast<std::uint32_t>(::getpid()) && client.hasPeerBinary);
        d.m_clients.push_back(std::move(client));
        return d.m_clients.back().clientID;
    }

    /// Models the post-recvmsg ownership transfer only after operation 3 is ready.
    /// The descriptor transport itself is exercised by the real CLI fixture.
    static bool receiveDescriptor(Daemon& d, std::uint32_t clientID, PAL::UnixFileDescriptor master)
    {
        const auto index = indexOf(d, clientID);
        if (!index)
            return false;
        auto& client = d.m_clients[*index];
        if (!master || !d.mayReceiveLocalHandoff(client) || client.pendingAdoptFileDescriptor || !client.localAdopt || client.localAdopt->pendingBytes())
            return false;
        client.pendingAdoptFileDescriptor = std::move(master);
        return true;
    }

    static std::uint64_t daemonInstanceID(const Daemon& d) { return d.m_daemonInstanceID; }

    /// The client with an id.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - clientID: The id.
    /// - Returns: Its index, or nothing when the daemon holds no such client.
    static std::optional<std::size_t> indexOf(const Daemon& d, std::uint32_t clientID)
    {
        for (std::size_t index = 0; index < d.m_clients.size(); ++index) {
            if (d.m_clients[index].clientID == clientID)
                return index;
        }
        return std::nullopt;
    }

    /// Hands the daemon bytes a client wrote, as one read pass does: every
    /// whole frame is dispatched and the partial-frame clock is noted.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - clientID: The client.
    ///   - bytes: The bytes.
    static void receive(Daemon& d, std::uint32_t clientID, std::span<const std::uint8_t> bytes)
    {
        const std::optional<std::size_t> index = indexOf(d, clientID);
        if (!index)
            return;
        d.feedClientFrames(*index, std::as_bytes(bytes));
    }

    /// Runs the daemon's inbound budget pass.
    ///
    /// - Parameter d: The daemon.
    static void enforceInboundBudget(Daemon& d) { d.enforceInboundBudget(); }

    /// Whether the daemon still serves a client.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - clientID: The client.
    /// - Returns: False once the client is dropped.
    static bool isServed(const Daemon& d, std::uint32_t clientID)
    {
        const std::optional<std::size_t> index = indexOf(d, clientID);
        return index && !d.m_clients[*index].didOverflow;
    }

    /// The bytes of an unfinished adopt the daemon holds for a client.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - clientID: The client.
    /// - Returns: Actual pending and in-dispatch allocation capacity.
    static std::size_t heldBytes(const Daemon& d, std::uint32_t clientID)
    {
        const std::optional<std::size_t> index = indexOf(d, clientID);
        return index && d.m_clients[*index].localAdopt ? d.m_clients[*index].localAdopt->retainedBytes() : 0;
    }

    static std::size_t byteCount(const Daemon& d, std::uint32_t clientID)
    {
        const auto index = indexOf(d, clientID);
        return index && d.m_clients[*index].localAdopt ? d.m_clients[*index].localAdopt->pendingBytes() : 0;
    }

    static int pendingDescriptor(const Daemon& d, std::uint32_t clientID)
    {
        const auto index = indexOf(d, clientID);
        return index ? d.m_clients[*index].pendingAdoptFileDescriptor.get() : -1;
    }

    static std::size_t aggregateBytes(const Daemon& d)
    {
        std::size_t bytes = 0;
        for (const auto& client : d.m_clients)
            bytes += d.heldInboundBytes(client);
        return bytes;
    }

    /// The first session allocation reached by a complete admitted Prepare.
    static std::size_t sessionAllocationBytes() { return sizeof(Daemon::Session); }

    static void drop(Daemon& d, std::uint32_t clientID)
    {
        if (const auto index = indexOf(d, clientID))
            d.dropClient(*index);
    }

    static void reapDeferredDrop(Daemon& d, std::uint32_t clientID)
    {
        if (const auto index = indexOf(d, clientID); index && d.m_clients[*index].didOverflow)
            d.reapClient(*index);
    }

    enum class MissingAuthority {
        Descriptor,
        LocalSocket,
        TrustedBinary
    };

    static void removeAuthority(Daemon& d, std::uint32_t clientID, MissingAuthority missing)
    {
        const auto index = indexOf(d, clientID);
        if (!index)
            return;
        auto& client = d.m_clients[*index];
        switch (missing) {
        case MissingAuthority::Descriptor:
            client.pendingAdoptFileDescriptor.reset();
            break;
        case MissingAuthority::LocalSocket:
            client.fileDescriptor.reset();
            break;
        case MissingAuthority::TrustedBinary:
            if (client.localAdoptAuthority)
                client.localAdoptAuthority->executableSHA256[0] ^= 1;
            break;
        }
    }

    /// The sessions the daemon holds.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: How many.
    static std::size_t sessionCount(const Daemon& d) { return d.m_sessions.size(); }
    static std::size_t preparedCount(const Daemon& d) { return d.m_preparedLocalAdopts; }

    /// Adds idle sessions, each a terminal over an in-memory stream that never
    /// ends, until the daemon holds `count`.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - count: How many sessions it holds afterwards.
    static void fillSessions(Daemon& d, std::size_t count)
    {
        while (d.m_sessions.size() < count) {
            auto session = std::make_unique<Daemon::Session>();
            session->id = d.m_nextSessionID++;
            session->term = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::resize(*session->term, 24, 80);
            session->stream = std::make_unique<TestDoubles::StreamDouble>();
            session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = session->id, .high = 1 });
            d.wireBackgroundNotifications(*session);
            (void)d.wireRemoteInput(*session);
            d.m_sessions.push_back(std::move(session));
        }
    }

    /// How many history lines the session the daemon made last was told
    /// were left behind.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The count; zero when there is no session.
    static std::uint32_t linesNotCarried(const Daemon& d) { return d.m_sessions.empty() ? 0 : d.m_sessions.back()->historyLinesNotCarried; }

    /// The terminal of the session the daemon made last.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The terminal, or null when there is no session.
    static const Ubiquitty::Terminal* newestTerminal(const Daemon& d) { return d.m_sessions.empty() ? nullptr : d.m_sessions.back()->term.get(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::PAL::UnixFileDescriptor;
using Access = Ubiquitty::Host::AdoptInPartsAccess;
namespace Local = Ubiquitty::Protocol::Local;
namespace Share = Ubiquitty::Session;

namespace {

/// Observes the complete Prepare borrow at its first session allocation.
/// The fixture drives the daemon synchronously; the callback never allocates.
class ScopedAdoptReplayObservation {
public:
    enum class Action {
        Observe,
        DropAndRefuse
    };

    ScopedAdoptReplayObservation(Daemon& daemon, std::uint32_t clientID, Action action)
        : m_daemon(daemon)
        , m_clientID(clientID)
        , m_action(action)
        , m_previous(active().exchange(this))
    {
    }

    ~ScopedAdoptReplayObservation() { active().store(m_previous); }
    ScopedAdoptReplayObservation(const ScopedAdoptReplayObservation&) = delete;
    ScopedAdoptReplayObservation& operator=(const ScopedAdoptReplayObservation&) = delete;

    static bool refuses(std::size_t size)
    {
        auto* observation = active().load();
        if (!observation || observation->didObserve || size != Access::sessionAllocationBytes())
            return false;
        observation->didObserve = true;
        observation->heldAtAllocation = Access::heldBytes(observation->m_daemon, observation->m_clientID);
        observation->pendingAtAllocation = Access::byteCount(observation->m_daemon, observation->m_clientID);
        observation->aggregateAtAllocation = Access::aggregateBytes(observation->m_daemon);
        if (observation->m_action != Action::DropAndRefuse)
            return false;
        Access::drop(observation->m_daemon, observation->m_clientID);
        observation->heldAfterDrop = Access::heldBytes(observation->m_daemon, observation->m_clientID);
        return true;
    }

    bool didObserve = false;
    std::size_t heldAtAllocation = 0;
    std::size_t pendingAtAllocation = 0;
    std::size_t aggregateAtAllocation = 0;
    std::size_t heldAfterDrop = 0;

private:
    static std::atomic<ScopedAdoptReplayObservation*>& active()
    {
        static std::atomic<ScopedAdoptReplayObservation*> observation { nullptr };
        return observation;
    }

    Daemon& m_daemon;
    const std::uint32_t m_clientID;
    const Action m_action;
    ScopedAdoptReplayObservation* const m_previous;
};

bool refusesObservedAdoptAllocation(std::size_t size) { return ScopedAdoptReplayObservation::refuses(size); }

/// A shell for a window to share: a child of this process leading a session
/// on a PTY, and the PTY's master.
class Shell {
public:
    /// Starts the shell.
    Shell()
    {
        int master = -1;
        int slave = -1;
        if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
            return;
        const int masterFlags = ::fcntl(master, F_GETFL);
        if (masterFlags < 0 || ::fcntl(master, F_SETFL, masterFlags | O_NONBLOCK) < 0) {
            ::close(master);
            ::close(slave);
            return;
        }
        int ready[2];
        if (::pipe(ready) != 0)
            return;
        m_leader = ::fork();
        if (m_leader == 0) {
            ::close(master);
            ::close(ready[0]);
            ::setsid();
            ::ioctl(slave, TIOCSCTTY, 0);
            const char done = 1;
            (void)!::write(ready[1], &done, 1);
            for (;;)
                ::pause();
        }
        ::close(slave);
        ::close(ready[1]);
        char done = 0;
        (void)!::read(ready[0], &done, 1);
        ::close(ready[0]);
        m_master = UnixFileDescriptor(master);
    }

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    /// Ends the shell.
    ~Shell()
    {
        if (m_leader > 0) {
            ::kill(m_leader, SIGKILL);
            ::waitpid(m_leader, nullptr, 0);
        }
    }

    /// A duplicate of the master, as a descriptor passed over a socket is.
    ///
    /// - Returns: The duplicate.
    UnixFileDescriptor master() const { return UnixFileDescriptor(::dup(m_master.get())); }

private:
    pid_t m_leader = -1;
    UnixFileDescriptor m_master;
};

/// A local client of the daemon that brought a shell's master to adopt.
struct Adopter {
    /// The client's id.
    std::uint32_t clientID = 0;
    /// This test's end of its socket.
    UnixFileDescriptor socket;
    Local::AdoptWireOperation operation;
};

enum class MasterTransfer {
    Prepare,
    None
};

/// Connects an adopter.
///
/// - Parameters:
///   - d: The daemon.
///   - shell: The shell whose master it brings.
/// - Returns: The adopter.
Adopter connectAdopter(Daemon& d, const Shell& shell, MasterTransfer transfer = MasterTransfer::Prepare)
{
    int descriptors[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
        std::abort();
    ::fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    Adopter adopter;
    adopter.clientID = Access::addAdopter(d, UnixFileDescriptor(descriptors[0]));
    adopter.socket = UnixFileDescriptor(descriptors[1]);
    const auto request = Local::encodeLocalHandoffRequest(adopter.clientID);
    CHECK(request.has_value());
    if (!request)
        std::abort();
    Access::receive(d, adopter.clientID, *request);
    std::array<std::uint8_t, Local::localControlReplyBytes> reply {};
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received < reply.size() && std::chrono::steady_clock::now() < deadline) {
        pollfd readable { adopter.socket.get(), POLLIN, 0 };
        if (::poll(&readable, 1, 20) <= 0)
            continue;
        const auto count = ::read(adopter.socket.get(), reply.data() + received, reply.size() - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<std::size_t>(count);
    }
    const auto ready = Local::decodeLocalHandoffReady(std::span(reply).first(received));
    const bool isReady = ready && ready->status == Local::HandoffReadyStatus::Ready
        && ready->correlation == adopter.clientID && ready->daemonInstanceID == Access::daemonInstanceID(d);
    CHECK_MSG(isReady, "the real operation-3 request must receive its exact Ready before any descriptor");
    if (!isReady)
        std::abort();
    adopter.operation = { .operationID = { .low = adopter.clientID, .high = 0xA11CE }, .daemonInstanceID = ready->daemonInstanceID };
    CHECK(Access::pendingDescriptor(d, adopter.clientID) == -1);
    if (transfer == MasterTransfer::Prepare && !Access::receiveDescriptor(d, adopter.clientID, shell.master()))
        std::abort();
    return adopter;
}

/// A window with history lines that share no run with their neighbours.
///
/// - Parameters:
///   - columns: Its width.
///   - lines: How many lines to print.
/// - Returns: The window.
std::unique_ptr<Ubiquitty::Terminal> windowWithHistory(int columns, int lines)
{
    auto window = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::hardReset(*window);
    Ubiquitty::resize(*window, 6, columns);
    window->scrollbackLimit = lines + 100;
    std::string line;
    for (int index = 0; index < lines; ++index) {
        line.clear();
        for (int column = 0; column + 1 < columns; ++column)
            line += "\x1b[38;5;" + std::to_string((index + column * 7) % 256) + "m" + static_cast<char>('a' + (index + column) % 26);
        line += "\r\n";
        Ubiquitty::processBytes(window.get(), line.c_str());
    }
    Ubiquitty::processBytes(window.get(), "\x1b[mvisible prompt");
    return window;
}

/// Whether two terminals hold the same history and show the same screen.
///
/// - Parameters:
///   - left: One terminal.
///   - right: The other.
/// - Returns: True when every history line and every visible row reads alike.
bool holdTheSame(const Ubiquitty::Terminal& left, const Ubiquitty::Terminal& right)
{
    if (!left.lineStore || !right.lineStore || left.lineStore->count() != right.lineStore->count() || left.rows != right.rows || left.columns != right.columns)
        return false;
    for (int index = 0; index < left.lineStore->count() + left.rows; ++index) {
        if (Ubiquitty::Screen::absoluteRowText(left, index, left.columns) != Ubiquitty::Screen::absoluteRowText(right, index, right.columns))
            return false;
    }
    return true;
}

std::vector<std::uint8_t> terminalBody(const Ubiquitty::Terminal& terminal)
{
    const auto body = Share::captureShareTerminal(terminal, Local::maxAdoptBodyBytes);
    CHECK(body.has_value());
    if (!body)
        return {};
    return { body->view().begin(), body->view().end() };
}

std::uint64_t prepareCorrelation(const Adopter& adopter) { return std::uint64_t { adopter.clientID } + 1000; }

std::vector<std::uint8_t> prepareRecord(const Adopter& adopter, std::span<const std::uint8_t> body)
{
    Local::AdoptWirePrepare request { .correlation = prepareCorrelation(adopter),
        .preparation = { .operation = adopter.operation, .bodyBytes = body.size() },
        .terminalBody = body };
    Ubiquitty::Crypto::sha256(body, request.preparation.bodySHA256);
    const auto encoded = Local::encodeAdoptWirePrepare(request, Share::localShareHandoffLimits().maxRecordBytes);
    CHECK(encoded.has_value());
    if (!encoded)
        return {};
    std::vector<std::uint8_t> record(encoded->header.begin(), encoded->header.end());
    record.insert(record.end(), encoded->terminalBody.begin(), encoded->terminalBody.end());
    return record;
}

std::optional<Local::AdoptWireOutcome> readOutcome(const Adopter& adopter, std::uint64_t correlation)
{
    std::array<std::uint8_t, Local::adoptWireOutcomeBytes> bytes {};
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        pollfd readable { adopter.socket.get(), POLLIN, 0 };
        if (::poll(&readable, 1, 20) <= 0)
            continue;
        const auto count = ::read(adopter.socket.get(), bytes.data() + received, bytes.size() - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<std::size_t>(count);
    }
    const auto outcome = Local::decodeAdoptWireOutcome(std::span(bytes).first(received));
    CHECK_MSG(outcome.has_value(), "the selected exchange did not return a complete retained Outcome");
    if (!outcome)
        return std::nullopt;
    CHECK(outcome->correlation == correlation && outcome->operation == adopter.operation);
    return *outcome;
}

std::optional<Local::AdoptWireOutcome> receiveCommand(Daemon& daemon, const Adopter& adopter, Local::AdoptWireMessage kind)
{
    const auto correlation = std::uint64_t { adopter.clientID } + 2000 + static_cast<std::uint8_t>(kind);
    const auto command = Local::encodeAdoptWireCommand({ .kind = kind, .correlation = correlation, .operation = adopter.operation });
    CHECK(command.has_value());
    if (!command)
        return std::nullopt;
    Access::receive(daemon, adopter.clientID, *command);
    return readOutcome(adopter, correlation);
}

std::array<std::uint8_t, Local::adoptWireFrameHeaderBytes> declaredPrepareHeader(std::uint32_t bodyBytes)
{
    return { static_cast<std::uint8_t>(Local::AdoptWireMessage::Prepare), static_cast<std::uint8_t>(bodyBytes),
        static_cast<std::uint8_t>(bodyBytes >> 8), static_cast<std::uint8_t>(bodyBytes >> 16), static_cast<std::uint8_t>(bodyBytes >> 24) };
}

/// One adopt-lane record as the wire frames it, whatever its body says.
///
/// - Parameters:
///   - kind: The record's kind byte.
///   - payload: Its body, written whole.
/// - Returns: The kind, the body's length as a little-endian u32, the body.
std::vector<std::uint8_t> adoptLaneRecord(Local::AdoptWireMessage kind, std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> record { static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(payload.size()),
        static_cast<std::uint8_t>(payload.size() >> 8), static_cast<std::uint8_t>(payload.size() >> 16), static_cast<std::uint8_t>(payload.size() >> 24) };
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    // The deadline a silent partial frame has, short enough to wait out.
    ::setenv("UBIQUITTY_PARTIAL_FRAME_DEADLINE_MS", "1500", 1);
    Daemon d;
    Access::trustLocalAdopt(d);

    // 1. Small and large complete records prepare privately, then Commit once.
    struct Window {
        const char* name;
        int columns;
        int lines;
        bool isInParts;
    };
    for (const Window& each : { Window { "a window smaller than the ordinary frame bound with long history", 10, 12'000, false }, Window { "a window larger than the ordinary frame bound", 400, 1'000, true } }) { // bool-ok: the row's expected relation to the ordinary frame ceiling
        const std::unique_ptr<Ubiquitty::Terminal> window = windowWithHistory(each.columns, each.lines);
        const auto body = terminalBody(*window);
        if (body.empty()) {
            CHECK_MSG(false, (std::string(each.name) + " did not encode").c_str());
            return 1;
        }
        CHECK_MSG((body.size() > Ubiquitty::Protocol::Sync::maxClientFramePayloadLength) == each.isInParts, "the fixture must cover both sides of the ordinary frame bound");
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const auto frames = prepareRecord(adopter, body);
        CHECK(!frames.empty());
        const std::size_t before = Access::sessionCount(d);
        const std::size_t preparedBefore = Access::preparedCount(d);
        const std::size_t split = frames.size() - 1;
        Access::receive(d, adopter.clientID, std::span(frames).first(split));
        CHECK_MSG(Access::sessionCount(d) == before, "an incomplete Prepare published a session");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == frames.size() && Access::byteCount(d, adopter.clientID) == split,
            "a partial raw Prepare did not retain and charge its exact complete record owner");
        {
            ScopedAdoptReplayObservation observation(d, adopter.clientID, ScopedAdoptReplayObservation::Action::Observe);
            Access::receive(d, adopter.clientID, std::span(frames).subspan(split));
            CHECK_MSG(observation.didObserve, "the complete Prepare did not reach its session allocation");
            CHECK_MSG(observation.pendingAtAllocation == frames.size() && observation.heldAtAllocation == frames.size(),
                "the complete Prepare borrow was uncharged during candidate construction");
            constexpr std::size_t maxAggregateBytes = 4u * (Ubiquitty::Protocol::Sync::maxClientFramePayloadLength + 64u * 1024u) + Local::maxAdoptBodyBytes;
            CHECK_MSG(observation.aggregateAtAllocation <= maxAggregateBytes, "Prepare exceeded its aggregate ingress admission");
        }
        const auto prepared = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(prepared && prepared->disposition == Local::AdoptWireDisposition::Prepared, "Prepare did not return a retained Prepared outcome");
        CHECK_MSG(Access::sessionCount(d) == before, "Prepared became visible before Commit");
        CHECK_MSG(Access::preparedCount(d) == preparedBefore + 1, "Prepared did not retain exactly one inactive candidate");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0 && Access::byteCount(d, adopter.clientID) == 0,
            "the decoder retained a delivered Prepare after its borrow ended");
        // A duplicate Prepare returns the same candidate and consumes only its
        // new duplicate descriptor, not the candidate's retained master.
        auto duplicate = shell.master();
        const int duplicateDescriptor = duplicate.get();
        CHECK(Access::receiveDescriptor(d, adopter.clientID, std::move(duplicate)));
        Access::receive(d, adopter.clientID, frames);
        const auto repeated = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(prepared && repeated && *repeated == *prepared, "duplicate Prepare replaced its retained target");
        CHECK_MSG(::fcntl(duplicateDescriptor, F_GETFD) == -1 && errno == EBADF, "duplicate Prepare leaked its newly supplied master");
        CHECK_MSG(Access::sessionCount(d) == before, "duplicate Prepare published a session");
        CHECK_MSG(Access::preparedCount(d) == preparedBefore + 1, "duplicate Prepare constructed a second inactive candidate");
        const auto committed = receiveCommand(d, adopter, Local::AdoptWireMessage::Commit);
        CHECK_MSG(committed && committed->disposition == Local::AdoptWireDisposition::Committed
                && prepared && committed->target == prepared->target,
            "Commit did not preserve the exact Prepared target");
        if (Access::sessionCount(d) != before + 1) {
            CHECK_MSG(false, (std::string(each.name) + " was not adopted").c_str());
            continue;
        }
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0, "the daemon still holds a completed record");
        CHECK_MSG(Access::preparedCount(d) == preparedBefore, "publication retained its prepared-candidate reservation");
        const Ubiquitty::Terminal* session = Access::newestTerminal(d);
        CHECK_MSG(session && holdTheSame(*session, *window), (std::string(each.name) + ": the adopted session does not hold the window's screen and whole history").c_str());
        CHECK_MSG(session && session->lineStore && window->lineStore && session->lineStore->count() == window->lineStore->count() && session->lineStore->count() >= each.lines - 6,
            (std::string(each.name) + ": history lines were left behind").c_str());

        Adopter resolver = connectAdopter(d, shell, MasterTransfer::None);
        resolver.operation = adopter.operation;
        const auto retained = receiveCommand(d, resolver, Local::AdoptWireMessage::Status);
        CHECK_MSG(committed && retained && retained->disposition == committed->disposition && retained->target == committed->target,
            "a fresh descriptor-free Status lost the exact committed outcome");
        CHECK_MSG(Access::sessionCount(d) == before + 1, "Status activated a second owner");

        if (each.isInParts) {
            const Shell interruptedShell;
            const Adopter interrupted = connectAdopter(d, interruptedShell);
            const auto interruptedFrames = prepareRecord(interrupted, body);
            const int descriptor = Access::pendingDescriptor(d, interrupted.clientID);
            const std::size_t beforeRefusal = Access::sessionCount(d);
            Access::receive(d, interrupted.clientID, std::span(interruptedFrames).first(interruptedFrames.size() - 1));
            {
                ScopedAdoptReplayObservation observation(d, interrupted.clientID, ScopedAdoptReplayObservation::Action::DropAndRefuse);
                Access::receive(d, interrupted.clientID, std::span(interruptedFrames).last(1));
                CHECK_MSG(observation.didObserve, "the refused Prepare did not reach its session allocation");
                CHECK_MSG(observation.heldAtAllocation == interruptedFrames.size() && observation.heldAfterDrop == interruptedFrames.size(),
                    "dropping a requester released its still-borrowed whole record");
            }
            CHECK_MSG(!Access::isServed(d, interrupted.clientID), "the allocation-time drop left its requester served");
            CHECK_MSG(Access::heldBytes(d, interrupted.clientID) == 0, "the replay owner remained charged after final dispatch returned");
            CHECK_MSG(Access::sessionCount(d) == beforeRefusal, "the refused Prepare created a session");
            CHECK_MSG(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "the refused Prepare leaked its handed-off descriptor");
            CHECK_MSG(Access::isServed(d, adopter.clientID), "dropping a replay requester closed the previously adopted neighbor");
        }
    }

    // 1b. A share past its total captures nothing and leaves the window's
    // history; the complete one is adopted whole.
    {
        const std::unique_ptr<Ubiquitty::Terminal> window = windowWithHistory(40, 300);
        const std::vector<std::uint8_t> whole = terminalBody(*window);
        if (whole.empty()) {
            CHECK_MSG(false, "a handover did not encode");
            return 1;
        }
        const auto rows = window->lineStore->count();
        const auto refused = Share::captureShareTerminal(*window, whole.size() / 2);
        CHECK_MSG(!refused && refused.error() == Share::ShareEncoding::TooLong, "a handover past the total was not refused whole");
        CHECK_MSG(window->lineStore->count() == rows, "refusing a handover dropped source history");
        CHECK_MSG(terminalBody(*window) == whole, "a refused handover changed what the complete one carries");
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const std::size_t before = Access::sessionCount(d);
        const auto record = prepareRecord(adopter, whole);
        Access::receive(d, adopter.clientID, record);
        const auto prepared = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(prepared && prepared->disposition == Local::AdoptWireDisposition::Prepared && Access::sessionCount(d) == before,
            "a complete handover was not prepared privately");
        const auto committed = receiveCommand(d, adopter, Local::AdoptWireMessage::Commit);
        CHECK_MSG(committed && committed->disposition == Local::AdoptWireDisposition::Committed, "a complete handover did not commit");
        const Ubiquitty::Terminal* session = Access::newestTerminal(d);
        if (Access::sessionCount(d) != before + 1 || !session || !session->lineStore) {
            CHECK_MSG(false, "a handover was not adopted");
        } else {
            const std::uint32_t left = Access::linesNotCarried(d);
            CHECK_MSG(left == 0 && holdTheSame(*session, *window), "a handover within the total is not the whole window, or says lines were left");
        }
    }

    // The first channel may disappear after Prepared. A fresh authenticated
    // descriptor-free Abort resolves the same operation and closes only its
    // inactive duplicate master; the source shell remains this fixture's child.
    {
        const auto window = windowWithHistory(40, 20);
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const int descriptor = Access::pendingDescriptor(d, adopter.clientID);
        const auto before = Access::sessionCount(d);
        const auto preparedBefore = Access::preparedCount(d);
        const auto record = prepareRecord(adopter, terminalBody(*window));
        Access::receive(d, adopter.clientID, record);
        const auto prepared = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(prepared && prepared->disposition == Local::AdoptWireDisposition::Prepared, "the disconnect fixture did not prepare");
        Access::drop(d, adopter.clientID);
        CHECK_MSG(Access::preparedCount(d) == preparedBefore + 1 && Access::sessionCount(d) == before,
            "dropping a channel discarded or published its table-owned Prepared candidate");
        CHECK_MSG(::fcntl(descriptor, F_GETFD) >= 0, "channel loss closed the retained candidate's master");
        Adopter resolver = connectAdopter(d, shell, MasterTransfer::None);
        resolver.operation = adopter.operation;
        const auto aborted = receiveCommand(d, resolver, Local::AdoptWireMessage::Abort);
        CHECK_MSG(aborted && prepared && aborted->disposition == Local::AdoptWireDisposition::Aborted && aborted->target == prepared->target,
            "a fresh descriptor-free Abort did not resolve the original Prepared target");
        CHECK_MSG(Access::preparedCount(d) == preparedBefore && Access::sessionCount(d) == before, "Abort retained or published its inactive candidate");
        CHECK_MSG(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "Abort leaked the candidate's duplicate master");
    }

    // 2. A transfer that stops inside its raw body adopts nothing, and is dropped
    //    at the deadline with nothing held.
    {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const auto record = prepareRecord(adopter, std::vector<std::uint8_t>(4096, 0x5A));
        const std::size_t before = Access::sessionCount(d);
        constexpr std::size_t prefixBytes = Local::adoptWirePrepareHeaderBytes + 17;
        Access::receive(d, adopter.clientID, std::span(record).first(prefixBytes));
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == record.size(), "the header did not admit the exact whole record owner");
        CHECK_MSG(Access::byteCount(d, adopter.clientID) == prefixBytes, "the daemon did not retain exactly the partial raw record it received");
        Access::enforceInboundBudget(d);
        CHECK_MSG(Access::isServed(d, adopter.clientID), "a client sending a fragmented Prepare was dropped inside the deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        Access::enforceInboundBudget(d);
        CHECK_MSG(!Access::isServed(d, adopter.clientID), "a client that stopped midway through an adopt was not dropped at the deadline");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0, "the parts of an adopt that stopped midway are still held");
        CHECK_MSG(Access::sessionCount(d) == before, "a transfer that stopped midway adopted a session");
    }

    // 3. One maximum raw record fits. A second complete owner is refused
    //    before allocation while preserving the first requester's owner.
    {
        const Shell firstShell;
        const Shell secondShell;
        const Adopter first = connectAdopter(d, firstShell);
        const auto record = prepareRecord(first, std::vector<std::uint8_t>(Local::maxAdoptBodyBytes, 0x5A));
        const auto header = std::span(record).first(Local::adoptWireFrameHeaderBytes);
        Access::receive(d, first.clientID, header);
        Access::enforceInboundBudget(d);
        CHECK_MSG(Access::isServed(d, first.clientID), "one complete record owner exceeded the aggregate inbound budget");
        CHECK_MSG(Access::heldBytes(d, first.clientID) == record.size() && Access::byteCount(d, first.clientID) == header.size(),
            "one maximum Prepare did not reserve exactly its whole record");
        const Adopter second = connectAdopter(d, secondShell);
        const int secondDescriptor = Access::pendingDescriptor(d, second.clientID);
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        const std::size_t beforeSecond = Access::sessionCount(d);
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(record.size());
            Access::receive(d, second.clientID, header);
        }
        CHECK_MSG(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == refusals, "aggregate refusal attempted the competing allocation");
        CHECK_MSG(!Access::isServed(d, second.clientID), "the competing reservation was not refused immediately");
        CHECK_MSG(Access::heldBytes(d, second.clientID) == 0 && Access::byteCount(d, second.clientID) == 0, "a refused adopter still holds its parts");
        Access::enforceInboundBudget(d);
        CHECK_MSG(!Access::isServed(d, second.clientID), "the budget pass resumed a client refused by admission");
        CHECK_MSG(Access::pendingDescriptor(d, second.clientID) == -1, "a dropped requester retained its unconsumed master");
        Access::reapDeferredDrop(d, second.clientID);
        CHECK_MSG(::fcntl(secondDescriptor, F_GETFD) == -1 && errno == EBADF, "aggregate refusal leaked its handoff descriptor");
        CHECK_MSG(Access::isServed(d, first.clientID), "admitting a competing adopt dropped the older client");
        CHECK_MSG(Access::sessionCount(d) == beforeSecond, "a refused adopt left a session");
        constexpr std::size_t maxAggregateBytes = 4u * (Ubiquitty::Protocol::Sync::maxClientFramePayloadLength + 64u * 1024u) + Local::maxAdoptBodyBytes;
        CHECK_MSG(Access::aggregateBytes(d) <= maxAggregateBytes, "Adopt capacity exceeds the aggregate before enforcement");

        Access::drop(d, first.clientID);
        CHECK_MSG(Access::heldBytes(d, first.clientID) == 0, "dropping a partial Prepare retained its record owner");
        Access::reapDeferredDrop(d, first.clientID);
    }

    // A null allocation closes only its requester and releases its descriptor.
    {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const auto record = prepareRecord(adopter, std::vector<std::uint8_t>(256, 0x5A));
        const int descriptor = Access::pendingDescriptor(d, adopter.clientID);
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(record.size());
            Access::receive(d, adopter.clientID, record);
        }
        CHECK_MSG(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == refusals + 1, "the authorized adopt allocation was not attempted");
        CHECK_MSG(!Access::isServed(d, adopter.clientID) && Access::heldBytes(d, adopter.clientID) == 0, "a null allocation left an adopter or its capacity live");
        CHECK_MSG(Access::pendingDescriptor(d, adopter.clientID) == -1, "allocation refusal retained the unconsumed master");
        Access::reapDeferredDrop(d, adopter.clientID);
        CHECK_MSG(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "allocation refusal leaked its handoff descriptor");
    }

    // The authority gates run before any 36 MiB reservation or allocation.
    for (const auto missing : { Access::MissingAuthority::Descriptor, Access::MissingAuthority::LocalSocket, Access::MissingAuthority::TrustedBinary }) {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const auto record = prepareRecord(adopter, std::vector<std::uint8_t>(256, 0x5A));
        Access::removeAuthority(d, adopter.clientID, missing);
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(record.size());
            Access::receive(d, adopter.clientID, record);
        }
        CHECK_MSG(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == refusals, "an unauthorized part reached the adopt allocator");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0 && Access::byteCount(d, adopter.clientID) == 0, "an unauthorized part retained adopt storage");
    }

    // A valid Prepare digest is not permission to interpret malformed Terminal
    // contents as a thin payload. Minimum and maximum bodies retain Rejected.
    for (const bool usesFullCapacity : { false, true }) {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const int descriptor = Access::pendingDescriptor(d, adopter.clientID);
        const auto before = Access::sessionCount(d);
        const auto ending = prepareRecord(adopter, std::vector<std::uint8_t>(usesFullCapacity ? Local::maxAdoptBodyBytes : 1, 0x5A));
        Access::receive(d, adopter.clientID, ending);
        const auto rejected = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(rejected && rejected->disposition == Local::AdoptWireDisposition::Rejected
                && rejected->rejection == Local::AdoptWireRejection::InvalidPayload,
            "malformed full Terminal contents did not retain an explicit inactive rejection");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0 && Access::byteCount(d, adopter.clientID) == 0, "final dispatch retained its pending or replay owner");
        CHECK_MSG(Access::sessionCount(d) == before, "malformed adopt contents created a session");
        CHECK_MSG(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "final dispatch leaked its handoff descriptor");
    }

    // New framing checks the independent complete-record limit, exact Prepare
    // metadata length and direction before any record allocation or publication.
    const auto oversized = declaredPrepareHeader(static_cast<std::uint32_t>(Share::localShareHandoffLimits().maxRecordBytes - Local::adoptWireFrameHeaderBytes + 1));
    for (const auto& header : { oversized, declaredPrepareHeader(72), declaredPrepareHeader(0),
             std::array<std::uint8_t, 5> { 0xFF, 0, 0, 0, 0 },
             std::array<std::uint8_t, 5> { static_cast<std::uint8_t>(Local::AdoptWireMessage::Outcome), 54, 0, 0, 0 } }) {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const auto before = Access::sessionCount(d);
        const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
        Access::receive(d, adopter.clientID, header);
        CHECK_MSG(!Access::isServed(d, adopter.clientID) && Access::heldBytes(d, adopter.clientID) == 0 && Access::byteCount(d, adopter.clientID) == 0,
            "an invalid, oversized or reply-direction header retained a record owner");
        CHECK_MSG(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts, "a refused header reached an allocator");
        CHECK_MSG(Access::sessionCount(d) == before && Access::pendingDescriptor(d, adopter.clientID) == -1,
            "a refused header published a session or retained a descriptor");
    }

    // 4b. Records the adopt lane does not read are refused before anything is
    // held: a Prepare whose body is no prepared record, at the ordinary frame
    // bound, a Commit whose body is no command, and an empty Commit.
    {
        const std::vector<std::uint8_t> filler(Ubiquitty::Protocol::Sync::maxClientFramePayloadLength, 0x5A);
        const std::vector<std::uint8_t> shortCommand(Local::adoptWireCommandBytes - Local::adoptWireFrameHeaderBytes - 1, 0x5A);
        struct Refused {
            const char* what;
            std::vector<std::uint8_t> record;
        };
        for (const Refused& each : { Refused { "a Prepare whose body is no prepared record", adoptLaneRecord(Local::AdoptWireMessage::Prepare, filler) },
                 Refused { "a Commit whose body is no command", adoptLaneRecord(Local::AdoptWireMessage::Commit, shortCommand) },
                 Refused { "an empty Commit", adoptLaneRecord(Local::AdoptWireMessage::Commit, {}) } }) {
            const Shell shell;
            const Adopter adopter = connectAdopter(d, shell);
            const auto before = Access::sessionCount(d);
            Access::receive(d, adopter.clientID, each.record);
            CHECK_MSG(!Access::isServed(d, adopter.clientID) && Access::heldBytes(d, adopter.clientID) == 0,
                (std::string(each.what) + " was held").c_str());
            CHECK_MSG(Access::sessionCount(d) == before, (std::string(each.what) + " published a session").c_str());
        }
    }

    // 4d. A frame that does not end the adopt.
    {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell, MasterTransfer::None);
        const std::size_t before = Access::sessionCount(d);
        const auto missing = receiveCommand(d, adopter, Local::AdoptWireMessage::Status);
        CHECK_MSG(missing && missing->disposition == Local::AdoptWireDisposition::Unknown, "an unused operation was not genuinely Unknown");
        // Unknown in the selected local handoff grammar, not a remote tag.
        const std::array<std::uint8_t, Local::adoptWireFrameHeaderBytes> unknown { 0xFF, 0, 0, 0, 0 };
        Access::receive(d, adopter.clientID, unknown);
        CHECK_MSG(!Access::isServed(d, adopter.clientID), "a client that sent parts and then another frame is still served");
        CHECK_MSG(Access::heldBytes(d, adopter.clientID) == 0, "the parts of an adopt another frame ended are still held");
        CHECK_MSG(Access::sessionCount(d) == before, "parts and another frame adopted a session");
    }

    // A complete Prepare with changed payload bytes fails its exact digest;
    // a thin decoder is not a fallback for a failed raw record.
    {
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        auto record = prepareRecord(adopter, std::vector<std::uint8_t>(256, 0x5A));
        record.back() ^= 1;
        const auto before = Access::sessionCount(d);
        Access::receive(d, adopter.clientID, record);
        CHECK_MSG(!Access::isServed(d, adopter.clientID) && Access::heldBytes(d, adopter.clientID) == 0 && Access::byteCount(d, adopter.clientID) == 0,
            "a digest mismatch retained a record or tried another decoder");
        CHECK_MSG(Access::sessionCount(d) == before && Access::pendingDescriptor(d, adopter.clientID) == -1,
            "a digest mismatch published a session or retained a descriptor");
    }

    // A host at its 64-session cap refuses a whole, valid Prepare as full: the
    // candidate is not at fault, and a host with room would take it. Last,
    // because it leaves the daemon full.
    {
        constexpr std::size_t maxSessions = 64;
        Access::fillSessions(d, maxSessions);
        const std::unique_ptr<Ubiquitty::Terminal> window = windowWithHistory(20, 40);
        const auto body = terminalBody(*window);
        if (body.empty()) {
            CHECK_MSG(false, "the full-host window did not encode");
            return 1;
        }
        const Shell shell;
        const Adopter adopter = connectAdopter(d, shell);
        const int descriptor = Access::pendingDescriptor(d, adopter.clientID);
        Access::receive(d, adopter.clientID, prepareRecord(adopter, body));
        const auto rejected = readOutcome(adopter, prepareCorrelation(adopter));
        CHECK_MSG(rejected && rejected->disposition == Local::AdoptWireDisposition::Rejected
                && rejected->rejection == Local::AdoptWireRejection::HostFull,
            "a full host did not reject a valid Prepare as HostFull");
        CHECK_MSG(Access::sessionCount(d) == maxSessions && Access::preparedCount(d) == 0, "a full host made or reserved a session");
        CHECK_MSG(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "a full host's refusal leaked its handoff descriptor");
    }

    if (failures == 0)
        std::printf("adopt_in_parts: complete raw state prepares privately and commits once; fragmented, refused, malformed and retired records publish nothing; a full host rejects a Prepare as full\n");
    return failures == 0 ? 0 : 1;
}
