// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/PAL/test_socket.cpp
//
// The local-socket carrier (Source/PAL/Socket.h, backed by
// Source/Platform/POSIX/Socket.cpp). The peer is a plain AF_UNIX listener
// opened here: POSIX in a test is free, and standing one up directly keeps
// every assertion about connectLocal rather than about a second PAL entry point.
//
// Cases:
//   - connectLocal distinguishes empty, unbound and oversized names
//   - the error slot is optional
//   - a connected stream round-trips bytes in both directions
//   - writeAll queues through OutputQueue and drains clean
//   - a peer close reports EndOfFile, and stays EndOfFile

#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Ubiquitty;

static int status(PAL::StreamStatus s) { return static_cast<int>(s); }

namespace {

// A bound, listening AF_UNIX socket that unlinks its name on teardown, so a
// failing case cannot leave a stale endpoint for the next run to trip over.
class Listener {
public:
    explicit Listener(std::string path)
        : m_path(std::move(path))
    {
        ::unlink(m_path.c_str());
        m_fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_fileDescriptor < 0)
            return;
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
        if (::bind(m_fileDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(m_fileDescriptor, 4) != 0) {
            ::close(m_fileDescriptor);
            m_fileDescriptor = -1;
        }
    }

    ~Listener()
    {
        if (m_fileDescriptor >= 0)
            ::close(m_fileDescriptor);
        ::unlink(m_path.c_str());
    }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] bool isValid() const { return m_fileDescriptor >= 0; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    [[nodiscard]] int accept() const { return ::accept(m_fileDescriptor, nullptr, nullptr); }

private:
    std::string m_path;
    int m_fileDescriptor = -1;
};

std::string endpointPath(const char* name)
{
    const char* directory = ::getenv("TMPDIR");
    std::string base = directory && *directory ? directory : "/tmp";
    if (base.back() != '/')
        base += '/';
    return base + name + std::to_string(static_cast<long>(::getpid()));
}

// Read until `want` bytes arrive or the budget runs out. The carrier is
// non-blocking, so Again is the expected answer until the peer's write lands.
int drainInto(PAL::ByteStream& stream, char* out, int want)
{
    int got = 0;
    for (int ticks = 0; got < want && ticks < 500; ++ticks) {
        const int n = stream.read({ out + got, static_cast<size_t>(want - got) });
        if (n > 0) {
            got += n;
            continue;
        }
        if (n != status(PAL::StreamStatus::Again))
            break;
        ::usleep(2000);
    }
    return got;
}

} // namespace

TEST(socketCases)
{

    CHECK(PAL::localSocketsAvailable());

    // ---- establishment failures are distinguished, not collapsed ----
    {
        PAL::LocalSocketError error = PAL::LocalSocketError::None;
        CHECK(PAL::connectLocal("", &error) == nullptr);
        CHECK(error == PAL::LocalSocketError::Failed);

        error = PAL::LocalSocketError::None;
        CHECK(PAL::connectLocal(endpointPath("ubq-absent-"), &error) == nullptr);
        CHECK(error == PAL::LocalSocketError::NotFound);

        error = PAL::LocalSocketError::None;
        CHECK(PAL::connectLocal(std::string(512, 'x'), &error) == nullptr);
        CHECK(error == PAL::LocalSocketError::PathTooLong);

        // A caller that does not care must not be required to pass a slot.
        CHECK(PAL::connectLocal(endpointPath("ubq-absent-")) == nullptr);
    }

    Listener listener(endpointPath("ubq-sock-"));
    CHECK(listener.isValid());

    if (listener.isValid()) {
        PAL::LocalSocketError error = PAL::LocalSocketError::Failed;
        auto client = PAL::connectLocal(listener.path(), &error);
        CHECK(client != nullptr);
        CHECK(error == PAL::LocalSocketError::None);

        const int server = listener.accept();
        CHECK(server >= 0);

        if (client && server >= 0) {
            CHECK(client->handle().isValid());
            CHECK(client->readinessWaitStrategy() == PAL::ReadinessWaitStrategy::WaitFirst);

            auto duplicate = client->duplicateAsPollableStream();
            CHECK(duplicate != nullptr);
            if (duplicate)
                CHECK(duplicate->readinessWaitStrategy() == PAL::ReadinessWaitStrategy::WaitFirst);

            // ---- client -> server ----
            const char outbound[] = "ping";
            CHECK(client->write({ outbound, sizeof(outbound) - 1 }) == static_cast<int>(sizeof(outbound) - 1));
            char received[16] = { 0 };
            CHECK(::read(server, received, sizeof(received)) == static_cast<ssize_t>(sizeof(outbound) - 1));
            CHECK(std::strncmp(received, "ping", 4) == 0);

            // ---- server -> client ----
            const char reply[] = "pong";
            CHECK(::write(server, reply, sizeof(reply) - 1) == static_cast<ssize_t>(sizeof(reply) - 1));
            char inbound[16] = { 0 };
            CHECK(drainInto(*client, inbound, 4) == 4);
            CHECK(std::strncmp(inbound, "pong", 4) == 0);

            // ---- writeAll queues through OutputQueue and drains clean ----
            const char queued[] = "queued";
            CHECK(client->writeAll({ queued, sizeof(queued) - 1 }) == 0);
            CHECK(!client->hasPendingWrite());
            CHECK(!client->drainPendingWrite());
            char queuedBack[16] = { 0 };
            CHECK(::read(server, queuedBack, sizeof(queuedBack)) == static_cast<ssize_t>(sizeof(queued) - 1));
            CHECK(std::strncmp(queuedBack, "queued", 6) == 0);

            // ---- a peer close is EndOfFile, and stays EndOfFile ----
            ::close(server);
            char discard[8];
            int terminal = 0;
            for (int ticks = 0; ticks < 500; ++ticks) {
                terminal = client->read({ discard, sizeof(discard) });
                if (terminal != status(PAL::StreamStatus::Again))
                    break;
                ::usleep(2000);
            }
            CHECK(terminal == status(PAL::StreamStatus::EndOfFile));
            CHECK(client->read({ discard, sizeof(discard) }) == status(PAL::StreamStatus::EndOfFile));
        } else if (server >= 0) {
            ::close(server);
        }
    }
}

/// ---- The peer identity a local control socket decides access on ----
//
// The rule is that the peer runs as this process's user. Both halves are
// asserted, because a credential check that cannot fail is the shape that lets a
// relocated socket be driven cross-uid: a real connected pair reports us, and
// something that is not a connected local socket reports nothing — which the
// contract says a caller must read as a refusal, not as permission.
TEST(peerIdentityReportsTheConnectedPeerAndRefusesOtherwise)
{
    int pair[2] = { -1, -1 };
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    if (pair[0] < 0)
        return;
    Ubiquitty::PAL::UnixFileDescriptor near(pair[0]);
    Ubiquitty::PAL::UnixFileDescriptor far(pair[1]);

    const std::optional<Ubiquitty::PAL::PeerIdentity> identity = Ubiquitty::PAL::peerIdentity(near);
    CHECK_MSG(identity.has_value(), "a connected local socket reports its peer");
    CHECK_MSG(identity && identity->userID == static_cast<uint32_t>(::geteuid()), "the peer of our own socketpair is us");
    CHECK_MSG(Ubiquitty::PAL::peerIsSameUser(near), "so the same-user question answers yes");

    // A pipe is not a socket. Nothing to report, and nothing is a refusal.
    Ubiquitty::PAL::UnixPipe pipe = Ubiquitty::PAL::makeUnixPipe();
    if (pipe) {
        CHECK_MSG(!Ubiquitty::PAL::peerIdentity(pipe.read), "a pipe reports no peer");
        CHECK_MSG(!Ubiquitty::PAL::peerIsSameUser(pipe.read), "and refuses rather than allowing");
    }

    // A closed handle must refuse rather than reading a recycled descriptor.
    Ubiquitty::PAL::UnixFileDescriptor closed;
    CHECK_MSG(!Ubiquitty::PAL::peerIsSameUser(closed), "an invalid handle refuses");
}

// A path too long for sun_path must be refused by its own name, never probed.
//
// The probe copies into sun_path, so an over-length path would be truncated and
// the connect would describe a DIFFERENT socket from the one the caller named
// and is about to unlink: a live endpoint at the full path, with nothing at the
// truncation, reads as "no listener" and gets removed. The bound is read from
// sizeof(sun_path) because it differs per target — Darwin 104, Linux 108 — and a
// literal would assert nothing on the platform it was not written on.
TEST(anOverLengthSocketPathIsRefusedRatherThanTruncated)
{
    struct sockaddr_un address {};
    const std::size_t limit = sizeof(address.sun_path);

    // One byte past what sun_path holds. The leading slash keeps it an absolute
    // path so only its length is under test.
    const std::string tooLong = "/" + std::string(limit, 'p');
    CHECK_MSG(tooLong.size() > limit, "the fixture is actually over the limit");

    const std::expected<void, std::string> refused = Ubiquitty::PAL::prepareLocalSocketPathForBind(tooLong);
    CHECK_MSG(!refused.has_value(), "an over-length socket path is refused");
    // Named for what it is. Reported as "already listening" it would send a
    // caller hunting an instance that does not exist.
    CHECK_MSG(refused.error().find("longer than this platform can bind") != std::string::npos,
        "the refusal names the length, not a phantom listener");

    // The longest path that does fit is not refused for length. It does not
    // exist, so preparing it is a no-op success — which is what pins the
    // boundary rather than merely proving that something long fails.
    const std::string longest = "/tmp/" + std::string(limit - 1 - 5, 'p');
    CHECK_MSG(longest.size() == limit - 1, "the fixture sits exactly on the limit");
    const std::expected<void, std::string> allowed = Ubiquitty::PAL::prepareLocalSocketPathForBind(longest);
    CHECK_MSG(allowed.has_value(), "a path that fits sun_path is not refused for length");
}

UBIQUITTY_TEST_MAIN()
