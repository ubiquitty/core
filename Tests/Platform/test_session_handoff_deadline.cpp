// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The handoff handshake gives up on a schedule the caller can live with.
//
// It runs on a thread that has a window to close: at quit, one call per
// shareable tab, and interactively behind the Share action. Per-syscall
// timeouts bound a read, not the loop around it — and the loop reads until the
// daemon acknowledges, so a peer that never does costs a timeout per read.
//
// The peer here accepts the connection and then says nothing, which is what a
// paused daemon looks like from this side: connected, silent. The call has to
// come back on the handshake's own deadline rather than on a read's.
//
// What the deadline bounds is silence. A payload with a long history takes
// longer to cross and to apply than any time fit for a screen alone, so the
// send goes on while the peer reads, and the answer is waited for as long as
// that many bytes take to apply. The paced peers below read at a pace, stop,
// or never answer, and the call is held to each.

#include "Assertions.h"
#include "Harness.h"

#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Session/SessionShare.h"
#include "Platform/POSIX/ReceivedDescriptor.h"
#include "Platform/POSIX/SessionHandoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// The silence interval the local handoff limits carry.
constexpr double silenceSeconds = 2.0;

// Room for a loaded machine past a deadline: a thread scheduled late, a poll
// woken late. A read's own timeout is more than twice the deadline plus this,
// so the two cannot be confused.
constexpr double loadedMachineSeconds = 1.5;

// The deadline the handshake carries, plus room for a loaded machine.
constexpr double maxHandshakeSeconds = silenceSeconds + loadedMachineSeconds;

std::string temporarySocketPath()
{
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/ubq-handoff-%d.sock", static_cast<int>(::getpid()));
    return path;
}

} // namespace

TEST(aHandoffToASilentPeerGivesUpOnTheHandshakeDeadline)
{
    const std::string path = temporarySocketPath();
    ::unlink(path.c_str());

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    struct sockaddr_un address;
    address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    CHECK(::bind(listener, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 1) == 0);

    // Accept and hold. Saying nothing is the whole fixture: the caller's loop
    // reads until an acknowledgement that never comes.
    std::atomic<bool> accepted { false };
    std::thread peer([&] {
        const int connection = ::accept(listener, nullptr, nullptr);
        accepted = connection >= 0;
        if (connection >= 0) {
            // Held open until the caller gives up; closing early would end the
            // read on end-of-file and prove nothing about the deadline.
            std::this_thread::sleep_for(std::chrono::seconds(6));
            ::close(connection);
        }
    });

    // A terminal body and a descriptor to ride with it. The content does not
    // matter here — the peer never reads it.
    const std::vector<std::uint8_t> frame(64, 0x5A);
    int descriptors[2] = { -1, -1 };
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);

    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t sessionID = Ubiquitty::Session::reHomeViaAdopt(
        path.c_str(), std::as_bytes(std::span { frame }), descriptors[0], Ubiquitty::Session::localShareHandoffLimits())
                                        .adopted.sessionID;
    const double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    CHECK_MSG(sessionID == 0, "a silent peer cannot have acknowledged anything");
    CHECK_MSG(accepted.load(), "the peer never accepted, so the call failed before it could wait");
    CHECK_MSG(elapsedSeconds < maxHandshakeSeconds,
        "the handshake outlived its deadline, so a read's timeout is bounding it instead");

    peer.join();
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    ::close(listener);
    ::unlink(path.c_str());
}

namespace {

/// A peer that accepts one handoff and reads it at a pace of its own.
class PacedPeer {
public:
    /// What the peer does once it has read what it was told to.
    enum class Then : std::uint8_t {
        /// Answers Prepare with a Prepared outcome reserving `answeredSessionID`,
        /// then answers the client's Commit with Committed.
        Answers,
        /// Holds the connection and says nothing.
        StaysSilent,
    };

    /// The session a peer that answers names.
    static constexpr std::uint32_t answeredSessionID = 77;

    /// Starts a peer.
    ///
    /// - Parameters:
    ///   - path: The socket to listen on.
    ///   - bytesToRead: How many Terminal body bytes it reads, after Prepare's
    ///     fixed header, before it stops reading.
    ///   - bytesPerStep: How many it reads between two pauses.
    ///   - pause: How long it pauses.
    ///   - then: What it does once it has read `bytesToRead`.
    PacedPeer(const std::string& path, std::size_t bytesToRead, std::size_t bytesPerStep, std::chrono::milliseconds pause, Then then)
    {
        // An answer written to a caller that gave up is an error to the
        // write, not a signal to this process.
        ::signal(SIGPIPE, SIG_IGN);
        ::unlink(path.c_str());
        m_listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un address;
        address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (m_listener < 0 || ::bind(m_listener, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 || ::listen(m_listener, 1) != 0)
            return;
        m_isListening = true;
        m_thread = std::thread([this, bytesToRead, bytesPerStep, pause, then] {
            const int connection = ::accept(m_listener, nullptr, nullptr);
            if (connection < 0)
                return;
            namespace Local = Ubiquitty::Protocol::Local;
            std::array<std::uint8_t, Local::localControlRequestBytes> requestBytes;
            std::size_t requestRead = 0;
            while (requestRead < requestBytes.size()) {
                const auto count = ::read(connection, requestBytes.data() + requestRead, requestBytes.size() - requestRead);
                if (count <= 0) {
                    ::close(connection);
                    return;
                }
                requestRead += static_cast<std::size_t>(count);
            }
            Local::LocalControlReader reader;
            if (reader.consume(requestBytes) != Local::LocalControlReader::State::Ready) {
                ::close(connection);
                return;
            }
            const auto request = reader.take();
            if (!request || request->operation != Local::LocalControlOperation::SessionHandoff) {
                ::close(connection);
                return;
            }
            const auto ready = Local::encodeLocalHandoffReady({ .correlation = request->correlation,
                .daemonInstanceID = 17,
                .status = Local::HandoffReadyStatus::Ready });
            if (::write(connection, ready.data(), ready.size()) != static_cast<ssize_t>(ready.size())) {
                ::close(connection);
                return;
            }
            // Prepare's fixed header comes first, its kind byte carrying the
            // master; the body follows at the peer's pace.
            std::vector<std::uint8_t> record(Local::adoptWirePrepareHeaderBytes);
            if (!receiveAll(connection, record)) {
                ::close(connection);
                return;
            }
            const auto header = Local::decodeAdoptWireHeader(std::span(record).first(Local::adoptWireFrameHeaderBytes),
                Ubiquitty::Session::localShareHandoffLimits().maxRecordBytes);
            if (!header || header->kind != Local::AdoptWireMessage::Prepare) {
                ::close(connection);
                return;
            }
            const std::size_t bodyBytes = Local::adoptWireFrameHeaderBytes + header->bodyBytes - Local::adoptWirePrepareHeaderBytes;
            record.resize(Local::adoptWirePrepareHeaderBytes + bodyBytes);
            const std::size_t toRead = std::min(bytesToRead, bodyBytes);
            std::size_t read = 0;
            std::size_t sinceThePause = 0;
            while (read < toRead) {
                const std::size_t step = std::min<std::size_t>(64 * 1024, toRead - read);
                const auto count = ::read(connection, record.data() + Local::adoptWirePrepareHeaderBytes + read, step);
                if (count <= 0)
                    break;
                read += static_cast<std::size_t>(count);
                lastReadAt = std::chrono::steady_clock::now().time_since_epoch().count();
                sinceThePause += static_cast<std::size_t>(count);
                if (sinceThePause >= bytesPerStep) {
                    sinceThePause = 0;
                    std::this_thread::sleep_for(pause);
                }
            }
            bytesRead = read;
            if (then == Then::Answers && read == bodyBytes)
                answerPrepareAndCommit(connection, record);
            // Held open until the caller is done with it; closing early would
            // end its read on end-of-file and prove nothing about a deadline.
            while (!m_isDone.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::close(connection);
        });
    }

    PacedPeer(const PacedPeer&) = delete;
    PacedPeer& operator=(const PacedPeer&) = delete;

    /// Ends the peer.
    ~PacedPeer()
    {
        m_isDone = true;
        if (m_listener >= 0)
            ::shutdown(m_listener, SHUT_RDWR);
        if (m_thread.joinable())
            m_thread.join();
        if (m_listener >= 0)
            ::close(m_listener);
    }

    /// Whether the peer is listening.
    [[nodiscard]] bool isListening() const { return m_isListening; }

    /// How many bytes the peer read.
    std::atomic<std::size_t> bytesRead { 0 };

    /// When the peer last took bytes off the connection, as a steady-clock
    /// count; zero before its first read.
    std::atomic<std::chrono::steady_clock::rep> lastReadAt { 0 };

    /// Measures how long after the peer's last read a moment came.
    ///
    /// A deadline the handoff keeps runs from the last byte the peer took, so
    /// that is what it is judged against: the time the peer spent reading
    /// before it stopped stretches with the machine's load and says nothing
    /// about the deadline.
    ///
    /// - Parameter moment: The moment, on the steady clock.
    /// - Returns: The seconds between the peer's last read and `moment`.
    [[nodiscard]] double secondsSinceLastRead(std::chrono::steady_clock::time_point moment) const
    {
        const std::chrono::steady_clock::time_point last { std::chrono::steady_clock::duration { lastReadAt.load() } };
        return std::chrono::duration<double>(moment - last).count();
    }

private:
    /// Reads exactly `bytes`, owning and closing any descriptor that rides along.
    static bool receiveAll(int connection, std::span<std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            alignas(cmsghdr) char control[Ubiquitty::POSIX::receivedDescriptorControlCapacity];
            iovec part { bytes.data(), bytes.size() };
            msghdr message {};
            message.msg_iov = &part;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof(control);
            const ssize_t count = ::recvmsg(connection, &message, Ubiquitty::POSIX::receivedDescriptorFlags);
            if (count <= 0 || !Ubiquitty::POSIX::takeReceivedDescriptor(message))
                return false;
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
        return true;
    }

    /// Answers a whole Prepare as a daemon that reserves `answeredSessionID`
    /// does, then answers the Commit that follows with Committed.
    static void answerPrepareAndCommit(int connection, std::span<const std::uint8_t> record)
    {
        namespace Local = Ubiquitty::Protocol::Local;
        const auto prepare = Local::decodeAdoptWirePrepare(record, Ubiquitty::Session::localShareHandoffLimits().maxRecordBytes);
        if (!prepare)
            return;
        const Local::AdoptWireTarget target { .sessionID = answeredSessionID, .paneID = { .low = 0x77, .high = 0x78 } };
        const auto prepared = Local::encodeAdoptWireOutcome({ .correlation = prepare->correlation,
            .operation = prepare->preparation.operation,
            .disposition = Local::AdoptWireDisposition::Prepared,
            .target = target });
        if (!prepared || ::write(connection, prepared->data(), prepared->size()) != static_cast<ssize_t>(prepared->size()))
            return;
        std::array<std::uint8_t, Local::adoptWireCommandBytes> commandBytes {};
        if (!receiveAll(connection, commandBytes))
            return;
        const auto command = Local::decodeAdoptWireCommand(commandBytes);
        if (!command || command->kind != Local::AdoptWireMessage::Commit || command->operation != prepare->preparation.operation)
            return;
        const auto committed = Local::encodeAdoptWireOutcome({ .correlation = command->correlation,
            .operation = command->operation,
            .disposition = Local::AdoptWireDisposition::Committed,
            .target = target });
        if (committed)
            (void)!::write(connection, committed->data(), committed->size());
    }

    int m_listener = -1;
    bool m_isListening = false;
    std::atomic<bool> m_isDone { false };
    std::thread m_thread;
};

/// What one handoff did, and how long it took.
struct Timed {
    /// The session the peer acknowledged, or 0.
    std::uint32_t sessionID = 0;
    /// The handoff's duration in seconds.
    double seconds = 0;
    /// When the handoff returned.
    std::chrono::steady_clock::time_point finishedAt;
};

/// Hands frames of a length to the peer at a path.
///
/// - Parameters:
///   - path: The peer's socket.
///   - bytes: How many bytes of frames to send.
/// - Returns: What the handoff did.
Timed handOver(const std::string& path, std::size_t bytes)
{
    const std::vector<std::uint8_t> frames(bytes, 0x5A);
    int descriptors[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
        return {};
    const auto started = std::chrono::steady_clock::now();
    Timed result;
    result.sessionID = Ubiquitty::Session::reHomeViaAdopt(path.c_str(), std::as_bytes(std::span { frames }), descriptors[0], Ubiquitty::Session::localShareHandoffLimits()).adopted.sessionID;
    result.finishedAt = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(result.finishedAt - started).count();
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return result;
}

constexpr std::size_t mebibyte = 1024 * 1024;

} // namespace

// A daemon that keeps reading is waited for, however long the whole takes:
// the handoff outlasts the silence interval and is adopted, since no single
// wait was that long.
TEST(aHandoffThatKeepsMovingIsNotLostToATimer)
{
    const std::string path = temporarySocketPath();
    PacedPeer peer(path, 4 * mebibyte, mebibyte, std::chrono::milliseconds(900), PacedPeer::Then::Answers);
    REQUIRE(peer.isListening());
    const Timed handoff = handOver(path, 4 * mebibyte);
    CHECK_MSG(handoff.sessionID == PacedPeer::answeredSessionID, "a handoff whose bytes were moving the whole time was given up on");
    CHECK_MSG(handoff.seconds > 2.4, "premise: the handoff took longer than the silence interval");
    CHECK_EQ(peer.bytesRead.load(), 4 * mebibyte);
    ::unlink(path.c_str());
}

// A daemon that stops reading part-way is given up on at the silence
// interval, measured from the last byte it took and not from the start.
TEST(aHandoffWhoseBytesStopMovingGivesUpAtTheSilenceInterval)
{
    const std::string path = temporarySocketPath();
    // A second and a half of reading, then nothing.
    PacedPeer peer(path, 3 * mebibyte, mebibyte, std::chrono::milliseconds(500), PacedPeer::Then::StaysSilent);
    REQUIRE(peer.isListening());
    const Timed handoff = handOver(path, 16 * mebibyte);
    CHECK_MSG(handoff.sessionID == 0, "a peer that stopped reading cannot have acknowledged anything");
    REQUIRE(peer.lastReadAt.load() != 0);
    // Below the silence interval, a timer other than the silence fired. The
    // interval runs from the last write the socket took, and the kernel can
    // take one more buffer's worth after the peer's last read, which starts
    // the interval once more; past two intervals, the handoff waited on a
    // stopped peer.
    const double silent = peer.secondsSinceLastRead(handoff.finishedAt);
    CHECK_MSG(silent > silenceSeconds - 0.1, "the handoff gave up before the silence interval had passed since the peer last read");
    CHECK_MSG(silent < 2 * silenceSeconds + loadedMachineSeconds, "the handoff waited on a peer that had stopped reading");
    ::unlink(path.c_str());
}

// A daemon that has the payload whole is allowed the time it takes to apply
// that much before it answers, and no more.
TEST(aHandoffWaitsForItsAnswerAsLongAsThePayloadTakesToApply)
{
    const std::string path = temporarySocketPath();
    PacedPeer peer(path, 2 * mebibyte, 64 * mebibyte, std::chrono::milliseconds(0), PacedPeer::Then::StaysSilent);
    REQUIRE(peer.isListening());
    const Timed handoff = handOver(path, 2 * mebibyte);
    CHECK_MSG(handoff.sessionID == 0, "a peer that never answered cannot have acknowledged anything");
    REQUIRE(peer.lastReadAt.load() != 0);
    // The silence interval, and a second for each of the two mebibytes,
    // counted from when the last byte was handed to the socket. That moment
    // falls after the call began and before the peer's last read, so the
    // allowance is judged from the start below and from the last read above.
    const double allowance = silenceSeconds + 2.0;
    CHECK_MSG(handoff.seconds > allowance - 0.1, "the wait for the answer allowed nothing for applying what was sent");
    CHECK_MSG(peer.secondsSinceLastRead(handoff.finishedAt) < allowance + loadedMachineSeconds,
        "the wait for the answer outlived the silence interval and the allowance");
    ::unlink(path.c_str());
}

UBIQUITTY_TEST_MAIN()
