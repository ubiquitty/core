// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Session/AdoptTransactionWire.h"
#include "Core/Session/SessionShare.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "HostConnection.h"
#include "PAL/PAL.h"
#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/POSIX/ReceivedDescriptor.h"
#include "Platform/POSIX/SessionHandoff.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace Ubiquitty::HostTest {

/// A finite local connect, with close-on-exec and no SIGPIPE side effects.
inline PAL::UnixFileDescriptor connectAdoptionSocket(const std::string& path)
{
    struct sockaddr_un address {};
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        return {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    PAL::UnixFileDescriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket || ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) < 0)
        return {};
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return {};
#endif
    const int flags = ::fcntl(socket.get(), F_GETFL);
    if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0)
        return {};
    const auto connected = ::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EINTR)
        return {};
    if (connected < 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                return {};
            struct pollfd wait { .fd = socket.get(), .events = POLLOUT, .revents = 0 };
            const int result = ::poll(&wait, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR)
                continue;
            int failure = 0;
            socklen_t length = sizeof(failure);
            if (result != 1 || ::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &failure, &length) != 0 || failure)
                return {};
            break;
        }
    }
    if (::fcntl(socket.get(), F_SETFL, flags) < 0 || !PAL::peerIsSameUser(socket)
        || !PAL::setSendTimeout(socket.get(), std::chrono::seconds(3)) || !PAL::setReceiveTimeout(socket.get(), std::chrono::seconds(3)))
        return {};
    return socket;
}

/// Sole reaper of one direct fork child. No wait-any or SIGCHLD auto-reaper may
/// run in a fixture using this owner. An unreaped child pins its PID; an ECHILD
/// result retracts ownership before any signal can target a reused number.
class AdoptionChild {
public:
    AdoptionChild() = default;
    AdoptionChild(const AdoptionChild&) = delete;
    AdoptionChild& operator=(const AdoptionChild&) = delete;
    ~AdoptionChild() { stop(); }

    template<typename Run>
    bool spawn(Run&& run)
    {
        if (m_process > 0)
            return false;
        struct sigaction disposition {};
        if (::sigaction(SIGCHLD, nullptr, &disposition) != 0 || disposition.sa_handler != SIG_DFL
            || (disposition.sa_flags & SA_NOCLDWAIT))
            return false;
        const pid_t child = ::fork();
        if (child < 0)
            return false;
        if (!child)
            ::_exit(run());
        m_process = child;
        m_status.reset();
        return true;
    }

    [[nodiscard]] bool waitForSuccess(std::chrono::milliseconds budget)
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (m_process > 0 && std::chrono::steady_clock::now() < deadline) {
            if (reap(WNOHANG))
                return m_status && WIFEXITED(*m_status) && WEXITSTATUS(*m_status) == 0;
            ::usleep(10'000);
        }
        return false;
    }

    void stop()
    {
        if (m_process <= 0 || reap(WNOHANG))
            return;
        // reap() kept this positive direct child exclusively unreaped. Neither
        // a PID file nor tcgetsid/process-group discovery supplies this target.
        if (::kill(m_process, SIGTERM) < 0 && errno != ESRCH)
            return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (m_process > 0 && std::chrono::steady_clock::now() < deadline) {
            if (reap(WNOHANG))
                return;
            ::usleep(10'000);
        }
        if (m_process > 0 && !reap(WNOHANG)) {
            if (::kill(m_process, SIGKILL) < 0 && errno != ESRCH)
                return;
            (void)reap(0);
        }
    }

private:
    // True means ownership ended, including ECHILD without a success status.
    bool reap(int flags)
    {
        if (m_process <= 0)
            return true;
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(m_process, &status, flags);
        } while (result < 0 && errno == EINTR);
        if (result == m_process) {
            m_status = status;
            m_process = -1;
            return true;
        }
        if (result < 0 && errno == ECHILD) {
            m_process = -1;
            m_status.reset();
            return true;
        }
        return false;
    }

    pid_t m_process = -1;
    std::optional<int> m_status;
};

/// Actual in-process owner: terminal, immutable capture, original PTY, and exact
/// attempt remain together even when transport returns Unresolved. This is not
/// an FD-only stand-in for a helper process that discarded the real attempt.
struct AdoptionOwner {
    std::unique_ptr<Terminal> terminal;
    PAL::UnixFileDescriptor master;
    PAL::UnixFileDescriptor slave;
    AdoptionChild leader;
    Protocol::Sync::HandoverBytes frozen;
    Session::ShareHandoffResult result;
    std::chrono::milliseconds elapsed {};

    [[nodiscard]] bool isCommitted() const
    {
        return result.ownership == Session::ShareOwnership::Committed && result.attempt && result.attempt->isCommitted()
            && result.adopted.sessionID && result.adopted.sessionID == result.attempt->target().sessionID
            && result.adopted.daemonInstanceID == result.attempt->operation().daemonInstanceID;
    }
};

/// A daemon forked from the fixture executable captures that same executable
/// through the unchanged production trust verifier. No peer label, token, or
/// test allowlist grants adoption authority. All jobs belong to this fixture.
class LocalAdoptionFixture {
public:
    static constexpr std::size_t maxOwners = 4;

    LocalAdoptionFixture()
    {
        m_context.monotonicClock = &PAL::monotonicTicks;
        PAL::installContext(m_context);
    }

    ~LocalAdoptionFixture()
    {
        // Explicitly destroy this test world before releasing any unresolved
        // owner. This is not Local evidence, recovery, or a passing assertion.
        m_daemon.stop();
        // The terminal goes before its leader: a leader exiting with output
        // still queued blocks in its last close of the replica until someone
        // reads the primary, and with the daemon gone nobody does. Closing the
        // primary hangs the terminal up, which ends that wait.
        for (auto& owner : m_owners) {
            owner->master = {};
            owner->slave = {};
            owner->leader.stop();
        }
        m_owners.clear();
    }

    bool start(const std::string& home)
    {
        const std::string selfPath = Host::resolveSelfExecutablePath();
        m_socketLeaf = Host::socketLeafForBinary(selfPath.c_str());
        if (m_socketLeaf.empty() || (::mkdir(home.c_str(), 0700) != 0 && errno != EEXIST))
            return false;
        m_socketPath = home + "/" + m_socketLeaf;
        const std::string processIDPath = m_socketPath + ".pid";
        if (!m_daemon.spawn([&] {
                if (::setenv("UBIQUITTY_HOME", home.c_str(), 1) != 0)
                    return 1;
                Host::Daemon daemon;
                if (!daemon.start(m_socketPath.c_str(), processIDPath.c_str(), "/bin/sh", 80, 24))
                    return 1;
                return daemon.run();
            }))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            if (connectAdoptionSocket(m_socketPath))
                return true;
            ::usleep(10'000);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    [[nodiscard]] const std::string& socketPath() const { return m_socketPath; }
    [[nodiscard]] const std::string& socketLeaf() const { return m_socketLeaf; }

    [[nodiscard]] AdoptionOwner* makeOwner(int columns = 80, int rows = 24)
    {
        if (m_owners.size() >= maxOwners || columns < minColumns || columns > maxColumns || rows < minRows || rows > maxRows)
            return nullptr;
        auto owner = std::make_unique<AdoptionOwner>();
        int master = -1;
        int slave = -1;
        struct winsize size {};
        size.ws_col = static_cast<unsigned short>(columns);
        size.ws_row = static_cast<unsigned short>(rows);
        if (::openpty(&master, &slave, nullptr, nullptr, &size) != 0)
            return nullptr;
        owner->master.reset(master);
        owner->slave.reset(slave);
        const int flags = ::fcntl(master, F_GETFL);
        if (flags < 0 || ::fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0)
            return nullptr;
        owner->terminal = std::make_unique<Terminal>();
        hardReset(*owner->terminal);
        resize(*owner->terminal, rows, columns);
        auto* borrowed = owner.get();
        m_owners.push_back(std::move(owner));
        return borrowed;
    }

    template<typename Run>
    bool startLeader(AdoptionOwner& owner, Run&& run)
    {
        int ready[2] { -1, -1 };
        if (::pipe(ready) != 0)
            return false;
        PAL::UnixFileDescriptor readEnd(ready[0]);
        PAL::UnixFileDescriptor writeEnd(ready[1]);
        if (!owner.leader.spawn([&] {
                readEnd.reset();
                owner.master.reset();
                if (::setsid() < 0 || ::ioctl(owner.slave.get(), TIOCSCTTY, 0) < 0)
                    return 1;
                const char message = 'r';
                if (::write(writeEnd.get(), &message, 1) != 1)
                    return 1;
                writeEnd.reset();
                return run(owner.slave.get());
            }))
            return false;
        writeEnd.reset();
        struct pollfd wait { .fd = readEnd.get(), .events = POLLIN, .revents = 0 };
        int readyCount;
        do {
            readyCount = ::poll(&wait, 1, 3000);
        } while (readyCount < 0 && errno == EINTR);
        char message = 0;
        return readyCount == 1 && (wait.revents & POLLIN) && ::read(readEnd.get(), &message, 1) == 1 && message == 'r';
    }

    bool startPausedLeader(AdoptionOwner& owner)
    {
        return startLeader(owner, [](int) -> int {
            for (;;)
                ::pause();
        });
    }

    bool adopt(AdoptionOwner& owner)
    {
        if (owner.frozen.data || owner.result.attempt)
            return false;
        auto captured = Session::captureShareTerminal(*owner.terminal, Protocol::Local::maxAdoptBodyBytes);
        if (!captured)
            return false;
        owner.frozen = std::move(*captured);
        const auto start = std::chrono::steady_clock::now();
        owner.result = Session::reHomeViaAdopt(m_socketPath.c_str(), std::as_bytes(owner.frozen.view()), owner.master.get(), Session::localShareHandoffLimits());
        owner.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return owner.isCommitted();
    }

    [[nodiscard]] PAL::UnixFileDescriptor connectOwner() const { return connectAdoptionSocket(m_socketPath); }

private:
    PAL::Context m_context;
    AdoptionChild m_daemon;
    std::vector<std::unique_ptr<AdoptionOwner>> m_owners;
    std::string m_socketPath;
    std::string m_socketLeaf;
};

namespace AdoptionProbe {

inline bool sendBytes(int socket, std::span<const std::uint8_t> bytes)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!bytes.empty()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || !PAL::setSendTimeout(socket, remaining))
            return false;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto sent = ::send(socket, bytes.data(), bytes.size(), flags);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(sent));
    }
    return true;
}

inline bool receiveBytes(int socket, std::span<std::uint8_t> bytes)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!bytes.empty()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || !PAL::setReceiveTimeout(socket, remaining))
            return false;
        alignas(cmsghdr) std::array<char, POSIX::receivedDescriptorControlCapacity> control {};
        struct iovec vector { .iov_base = bytes.data(), .iov_len = bytes.size() };
        struct msghdr message {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const auto count = ::recvmsg(socket, &message, POSIX::receivedDescriptorFlags);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        auto descriptor = POSIX::takeReceivedDescriptor(message);
        if (!descriptor || *descriptor)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

enum class AdmissionExpectation {
    NotOwner,
    PreparedThenAborted
};

/// Opens one selected handoff exchange on a fresh connection.
///
/// - Parameters:
///   - path: The daemon's socket.
///   - correlation: The handoff request's correlation.
///   - daemonInstanceID: Receives the serving instance; when nonzero on entry,
///     the exchange must reach that same instance.
/// - Returns: The connection, or an invalid descriptor.
inline PAL::UnixFileDescriptor openExchange(const std::string& path, std::uint64_t correlation, std::uint64_t& daemonInstanceID)
{
    namespace Local = Protocol::Local;
    PAL::UnixFileDescriptor socket = connectAdoptionSocket(path);
    if (!socket || !PAL::peerIsSameUser(socket)
        || !PAL::setSendTimeout(socket.get(), std::chrono::seconds(3)) || !PAL::setReceiveTimeout(socket.get(), std::chrono::seconds(3)))
        return {};
    const auto request = Local::encodeLocalHandoffRequest(correlation);
    std::array<std::uint8_t, Local::localControlReplyBytes> reply {};
    if (!request || !sendBytes(socket.get(), *request) || !receiveBytes(socket.get(), reply))
        return {};
    const auto selected = Local::decodeLocalHandoffReady(reply);
    if (!selected || selected->correlation != correlation || selected->status != Local::HandoffReadyStatus::Ready || !selected->daemonInstanceID
        || (daemonInstanceID && selected->daemonInstanceID != daemonInstanceID))
        return {};
    daemonInstanceID = selected->daemonInstanceID;
    return socket;
}

/// Reads one outcome record and holds it to the attempt.
///
/// - Parameters:
///   - socket: The exchange.
///   - attempt: The client's record of the operation.
///   - correlation: The command's correlation.
///   - disposition: The outcome expected.
/// - Returns: Whether the exact outcome arrived and the attempt accepted it.
inline bool receiveOutcome(int socket, Session::AdoptClientAttempt& attempt, std::uint64_t correlation, Session::AdoptDisposition disposition)
{
    namespace Local = Protocol::Local;
    std::array<std::uint8_t, Local::adoptWireOutcomeBytes> bytes {};
    if (!receiveBytes(socket, bytes))
        return false;
    const auto outcome = Local::decodeAdoptWireOutcome(bytes);
    if (!outcome)
        return false;
    const auto imported = Session::adoptOutcomeFromWire(*outcome, attempt.operation(), correlation);
    if (!imported || imported->disposition != disposition || !attempt.observe(*imported))
        return false;
    if (disposition == Session::AdoptDisposition::Rejected)
        return imported->target == Session::AdoptTarget {} && imported->rejection == Session::AdoptRejection::NotOwner;
    return imported->target.sessionID && imported->target.paneID != Protocol::Sync::ID128 {} && !imported->rejection;
}

/// Sends one command record for the attempt's operation.
///
/// - Parameters:
///   - socket: The exchange.
///   - attempt: The client's record of the operation.
///   - kind: The command.
///   - correlation: The command's correlation.
/// - Returns: Whether the whole record was sent.
inline bool sendCommand(int socket, const Session::AdoptClientAttempt& attempt, Protocol::Local::AdoptWireMessage kind, std::uint64_t correlation)
{
    const auto operation = Session::adoptOperationToWire(attempt.operation());
    if (!operation)
        return false;
    const auto command = Protocol::Local::encodeAdoptWireCommand({ .kind = kind, .correlation = correlation, .operation = *operation });
    return command && sendBytes(socket, *command);
}

/// Whether the daemon ended the connection: a read sees end of file within
/// the receive timeout, rather than silence.
///
/// - Parameter socket: The exchange, with its receive timeout set.
/// - Returns: Whether the daemon closed it.
inline bool isEndedByDaemon(int socket)
{
    std::array<std::uint8_t, 1> byte {};
    ssize_t count;
    do {
        count = ::recv(socket, byte.data(), byte.size(), 0);
    } while (count < 0 && errno == EINTR);
    return count == 0 || (count < 0 && errno == ECONNRESET);
}

/// Admission-only probes never send Commit. A refusal is a Rejected NotOwner
/// outcome that ends its exchange: a further record on that connection is
/// answered by the daemon closing it, and the retained evidence is read by
/// Status on a new exchange, as `resolveAdopt` reads it. A positive admission
/// requires exact Prepared, Prepared again for Status, then Aborted.
///
/// - Parameters:
///   - path: The daemon's socket.
///   - descriptor: The descriptor the Prepare offers as the terminal.
///   - body: The terminal body the Prepare carries.
///   - expectation: Which answer the daemon owes.
/// - Returns: Whether the daemon answered exactly that way.
inline bool checkAdmission(const std::string& path, int descriptor, std::span<const std::uint8_t> body, AdmissionExpectation expectation)
{
    namespace Local = Protocol::Local;
    std::uint64_t daemonInstanceID = 0;
    PAL::UnixFileDescriptor socket = openExchange(path, 1, daemonInstanceID);
    if (!socket)
        return false;
    Protocol::Sync::ID128 identifier;
    if (::getentropy(&identifier, sizeof(identifier)) != 0 || identifier == Protocol::Sync::ID128 {})
        return false;
    Session::AdoptClientAttempt attempt({ identifier, daemonInstanceID });
    Local::AdoptWirePrepare prepare { .correlation = 2,
        .preparation = { .operation = { identifier, daemonInstanceID }, .bodyBytes = body.size() },
        .terminalBody = body };
    Crypto::sha256(body, prepare.preparation.bodySHA256);
    const auto encoded = Local::encodeAdoptWirePrepare(prepare, Session::localShareHandoffLimits().maxRecordBytes);
    if (!encoded)
        return false;
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    struct iovec vector { .iov_base = const_cast<std::uint8_t*>(encoded->header.data()), .iov_len = 1 };
    struct msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* ancillary = CMSG_FIRSTHDR(&message);
    ancillary->cmsg_level = SOL_SOCKET;
    ancillary->cmsg_type = SCM_RIGHTS;
    ancillary->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(ancillary), &descriptor, sizeof(descriptor));
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    ssize_t sent;
    do {
        sent = ::sendmsg(socket.get(), &message, flags);
    } while (sent < 0 && errno == EINTR);
    if (sent != 1 || !sendBytes(socket.get(), std::span { encoded->header }.subspan(1)) || !sendBytes(socket.get(), encoded->terminalBody))
        return false;
    if (expectation == AdmissionExpectation::NotOwner) {
        if (!receiveOutcome(socket.get(), attempt, 2, Session::AdoptDisposition::Rejected))
            return false;
        // The refusal ended the exchange. A record sent after it may be
        // refused at the send or read as end of file, never left unanswered.
        (void)sendCommand(socket.get(), attempt, Local::AdoptWireMessage::Status, 3);
        if (!isEndedByDaemon(socket.get()))
            return false;
        PAL::UnixFileDescriptor resolution = openExchange(path, 4, daemonInstanceID);
        if (!resolution || !sendCommand(resolution.get(), attempt, Local::AdoptWireMessage::Status, 5)
            || !receiveOutcome(resolution.get(), attempt, 5, Session::AdoptDisposition::Rejected))
            return false;
        return attempt.mayResumeLocal() && !attempt.isCommitted();
    }
    if (!receiveOutcome(socket.get(), attempt, 2, Session::AdoptDisposition::Prepared)
        || !sendCommand(socket.get(), attempt, Local::AdoptWireMessage::Status, 3)
        || !receiveOutcome(socket.get(), attempt, 3, Session::AdoptDisposition::Prepared)
        || !sendCommand(socket.get(), attempt, Local::AdoptWireMessage::Abort, 4))
        return false;
    return receiveOutcome(socket.get(), attempt, 4, Session::AdoptDisposition::Aborted) && attempt.mayResumeLocal() && !attempt.isCommitted();
}

inline bool isRejectedWithoutActivation(const std::string& path, int descriptor, std::span<const std::uint8_t> body)
{
    return checkAdmission(path, descriptor, body, AdmissionExpectation::NotOwner);
}

} // namespace AdoptionProbe

/// Public listing corroborates that a refused Prepare published no pane.
inline std::optional<std::size_t> publishedAdoptionPaneCount(const std::string& path)
{
    namespace Sync = Protocol::Sync;
    PAL::UnixFileDescriptor socket = connectAdoptionSocket(path);
    if (!socket)
        return std::nullopt;
    Connection connection;
    connection.fileDescriptor = socket.get();
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = "adoption-fixture";
    if (::getentropy(&greeting.clientInstance, sizeof(greeting.clientInstance)) != 0 || greeting.clientInstance == Sync::ID128 {})
        return std::nullopt;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    std::string body;
    if (!sendGreeting(connection, greeting) || !waitFor(connection, Sync::FrameKind::Welcome, body, 3000)
        || !connection.welcome || connection.welcome->admission != Sync::AttachmentAdmission::Granted
        || connection.welcome->selection.target != Sync::AdmissionTarget::Control || !connection.panes.empty())
        return std::nullopt;
    Sync::Call call;
    call.text = R"({"jsonrpc":"2.0","id":"adopt.count","method":"host.sessions.subscribe","params":{}})";
    std::vector<std::uint8_t> encoded;
    Sync::encode(encoded, call);
    if (!sendFrame(connection, Sync::FrameKind::Call, { reinterpret_cast<const char*>(encoded.data()), encoded.size() })
        || !waitFor(connection, Sync::FrameKind::Result, body, 3000))
        return std::nullopt;
    const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    if (!result || result->transfer)
        return std::nullopt;
    bool isJSON = false;
    const auto answer = Codec::JSON::parse(result->text, isJSON);
    const auto* value = answer.find("result");
    const auto* count = value ? value->find("panes") : nullptr;
    if (!isJSON || answer.stringOr("id") != "adopt.count" || answer.find("error") || !count || !count->isNumber()
        || count->asNumber() < 0 || count->asNumber() > LocalAdoptionFixture::maxOwners
        || count->asNumber() != static_cast<std::size_t>(count->asNumber()))
        return std::nullopt;
    return static_cast<std::size_t>(count->asNumber());
}

} // namespace Ubiquitty::HostTest
