// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_quicbootstrap.cpp — the portable QUIC-bootstrap LINE parser
// (Protocol/Sync/QUICBootstrap.h::parseQUICBootstrapLine) and its cert-pin gate, which
// every frontend's remote session dials through. Pins:
//   * a valid 3-field line decodes port (decimal) + token + cert (hex) and returns true
//     only when the pin is exactly a 32-byte SHA-256 and the token is within the 64-byte cap;
//   * a line missing the cert pin (or token, or port) returns false — an unpinned token is
//     MITM-stealable, so the dialer must refuse;
//   * a WRONG-LENGTH pin or an over-cap token returns false — SUCCESS must mean a real,
//     daemon-acceptable result, not a half-decode the downstream guard would refuse;
//   * hex decoding (odd-length / non-hex values are dropped, failing the gate);
//   * order-independence + extra-whitespace tolerance;
//   * the producer format string round-trips through the parser.
//
// Links standalone against Source/Core/Protocol/Sync/QUICBootstrap.cpp.

#include "Core/Protocol/Local/Control.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using Ubiquitty::Protocol::Sync::decodeHexBytes;
using Ubiquitty::Protocol::Sync::parseQUICBootstrapLine;
using Ubiquitty::Protocol::Sync::QUICBootstrapInfo;
using Ubiquitty::Protocol::Sync::quicBootstrapLineFormat;

TEST(quicbootstrapCases)
{

    // A valid pin is exactly a 32-byte SHA-256 (64 hex chars); a valid token is within the
    // 64-byte cap. The parser populates the decoded fields even when the gate refuses, so
    // the decode and the gate are checked independently below.
    constexpr char validPinHex[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const std::string validPin = decodeHexBytes(validPinHex); // 32 bytes
    const std::string validTokenHex = "41424344"; // "ABCD", 4 bytes (<= cap)
    const std::string pin = validPinHex;

    // ---- valid 3-field line: port decimal, token + 32-byte cert pin ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_port=44321 quic_token=" + validTokenHex + " quic_cert_sha256=" + pin + "\n";
        CHECK(parseQUICBootstrapLine(line, info));
        CHECK(info.port == 44321);
        CHECK(info.token == "ABCD");
        CHECK(info.certificateSHA256 == validPin);
        CHECK(info.certificateSHA256.size() == 32);
    }

    // ---- trailing newline optional; no newline still parses ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_port=1 quic_token=00 quic_cert_sha256=" + pin;
        CHECK(parseQUICBootstrapLine(line, info));
        CHECK(info.port == 1);
        CHECK(info.token == std::string("\x00", 1));
        CHECK(info.certificateSHA256 == validPin);
    }

    // ---- order-independent fields ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_cert_sha256=" + pin + " quic_token=beef quic_port=8080\n";
        CHECK(parseQUICBootstrapLine(line, info));
        CHECK(info.port == 8080);
        CHECK(info.token == std::string("\xbe\xef", 2));
        CHECK(info.certificateSHA256 == validPin);
    }

    // ---- extra whitespace (runs of spaces, leading/trailing, CR) tolerated ----
    {
        QUICBootstrapInfo info;
        const std::string line = "  quic_port=22   quic_token=aa\tquic_cert_sha256=" + pin + " \r\n";
        CHECK(parseQUICBootstrapLine(line, info));
        CHECK(info.port == 22);
        CHECK(info.token == std::string("\xaa", 1));
        CHECK(info.certificateSHA256 == validPin);
    }

    // ---- unknown fields ignored ----
    {
        QUICBootstrapInfo info;
        const std::string line = "warning=stuff quic_port=5 quic_token=11 noise=x quic_cert_sha256=" + pin;
        CHECK(parseQUICBootstrapLine(line, info));
        CHECK(info.port == 5);
        CHECK(info.certificateSHA256.size() == 32);
    }

    // ---- MISSING CERT PIN → refuse (the security gate) ----
    {
        QUICBootstrapInfo info;
        CHECK(!parseQUICBootstrapLine("quic_port=44321 quic_token=41424344\n", info));
        CHECK(info.certificateSHA256.empty());
    }

    // ---- WRONG-LENGTH pin → refuse (short and long): the field decodes, but the gate
    //      requires exactly 32 bytes, so SUCCESS never carries a partial SHA-256 ----
    {
        QUICBootstrapInfo shortPin;
        CHECK(!parseQUICBootstrapLine("quic_port=5 quic_token=41424344 quic_cert_sha256=dead", shortPin));
        CHECK(shortPin.certificateSHA256.size() == 2); // decoded, but refused

        QUICBootstrapInfo longPin;
        const std::string line = "quic_port=5 quic_token=41424344 quic_cert_sha256=" + pin + "ff"; // 33 bytes
        CHECK(!parseQUICBootstrapLine(line, longPin));
        CHECK(longPin.certificateSHA256.size() == 33);
    }

    // ---- over-cap token (> 64 bytes) → refuse, so SUCCESS never advertises a token the
    //      daemon would re-cap and reject ----
    {
        QUICBootstrapInfo info;
        const std::string bigTokenHex(130, 'a'); // 65 decoded bytes (> 64-byte cap)
        const std::string line = "quic_port=5 quic_token=" + bigTokenHex + " quic_cert_sha256=" + pin;
        CHECK(!parseQUICBootstrapLine(line, info));
        CHECK(info.token.size() == 65); // decoded, but refused
    }

    // ---- missing token → refuse ----
    {
        QUICBootstrapInfo info;
        CHECK(!parseQUICBootstrapLine("quic_port=44321 quic_cert_sha256=" + pin, info));
    }

    // ---- missing / zero port → refuse ----
    {
        QUICBootstrapInfo info;
        CHECK(!parseQUICBootstrapLine("quic_token=41424344 quic_cert_sha256=" + pin, info));
        QUICBootstrapInfo zero;
        CHECK(!parseQUICBootstrapLine("quic_port=0 quic_token=41424344 quic_cert_sha256=" + pin, zero));
    }

    // ---- empty / garbage input → refuse ----
    {
        QUICBootstrapInfo info;
        CHECK(!parseQUICBootstrapLine("", info));
        CHECK(!parseQUICBootstrapLine("\n", info));
        CHECK(!parseQUICBootstrapLine("not a bootstrap line at all", info));
    }

    // ---- malformed hex (odd length) drops that field → fails the gate ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_port=5 quic_token=abc quic_cert_sha256=" + pin; // token odd-length
        CHECK(!parseQUICBootstrapLine(line, info));
        CHECK(info.token.empty());
        CHECK(info.certificateSHA256 == validPin); // cert still decoded
    }

    // ---- malformed hex (non-hex digit) drops that field ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_port=5 quic_token=zz quic_cert_sha256=" + pin;
        CHECK(!parseQUICBootstrapLine(line, info));
        CHECK(info.token.empty());
    }

    // ---- port overflow (> 65535) is rejected, leaving port 0 → refuse ----
    {
        QUICBootstrapInfo info;
        const std::string line = "quic_port=70000 quic_token=41424344 quic_cert_sha256=" + pin;
        CHECK(!parseQUICBootstrapLine(line, info));
        CHECK(info.port == 0);
    }

    // ---- the producer format round-trips through the parser ----
    {
        char line[256];
        std::snprintf(line, sizeof(line), quicBootstrapLineFormat, 12345u, validTokenHex.c_str(), validPinHex);
        QUICBootstrapInfo info;
        CHECK(parseQUICBootstrapLine(line, info));
        // Where to dial is never the line's to say: a field naming a host
        // is an unknown field, and the address stays empty for the client
        // that ran the bootstrap to fill in.
        CHECK(parseQUICBootstrapLine(std::string("quic_host=elsewhere.example reached_address=203.0.113.9 ") + line, info));
        CHECK(info.reachedAddress.empty());
        CHECK(info.port == 12345);
        CHECK(info.token == "ABCD");
        CHECK(info.certificateSHA256 == validPin);
    }
}

// The other half of this file's subject: requestQUICToken, which lives in
// QUICBootstrap.cpp. parseQUICBootstrapLine is inline in the header, so its
// cases alone leave every branch of the request unevaluated.
//
// A daemon that is not there is the case a client meets whenever the socket is
// gone — a restart, a stale path in a config, a session whose owner exited. What
// it must not do is fail silently or blame the wrong thing, so the assertion is
// that the error names the backend's reason and the path it tried, together. A
// message carrying only one of the two sends the reader to the wrong place, and
// the path is the half a caller can act on.
//
namespace {

// Writes every byte, retrying an interrupted or partial write.
bool writeAll(int fileDescriptor, std::span<const std::uint8_t> bytes)
{
    while (!bytes.empty()) {
        const ssize_t count = ::write(fileDescriptor, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

// A scripted daemon: listens on a fresh socket, accepts one client, drains its
// request, answers with the scripted bytes, closes. Runs on a thread so the
// client side under test blocks against a real peer rather than a mock.
struct ScriptedDaemon {
    std::string path;
    int listenFileDescriptor = -1;
    // Set by the server and read after its join: a reply cut short would test
    // the client against bytes no case scripted.
    bool isReplyShort = false;
    std::thread server;

    explicit ScriptedDaemon(std::vector<std::uint8_t> reply, int delayMilliseconds = 0)
    {
        char templatePath[] = "/tmp/ub-quicboot-XXXXXX";
        char* dir = ::mkdtemp(templatePath);
        path = std::string(dir ? dir : "/tmp") + "/daemon.sock";
        listenFileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        ::bind(listenFileDescriptor, reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
        ::listen(listenFileDescriptor, 1);
        server = std::thread([acceptFileDescriptor = listenFileDescriptor, reply = std::move(reply), delayMilliseconds, &isReplyShort = isReplyShort] {
            const int client = ::accept(acceptFileDescriptor, nullptr, nullptr);
            if (client < 0)
                return;
            Ubiquitty::Protocol::Local::LocalControlReader request;
            auto state = Ubiquitty::Protocol::Local::LocalControlReader::State::Awaiting;
            std::array<std::uint8_t, Ubiquitty::Protocol::Local::localControlRequestBytes> drain {};
            while (state == Ubiquitty::Protocol::Local::LocalControlReader::State::Awaiting) {
                const auto count = ::read(client, drain.data(), drain.size());
                if (count <= 0)
                    break;
                state = request.consume({ drain.data(), static_cast<std::size_t>(count) });
            }
            const auto decoded = request.take();
            if (!decoded || decoded->operation != Ubiquitty::Protocol::Local::LocalControlOperation::QUICToken
                || decoded->correlation != 1) {
                ::close(client);
                return;
            }
            if (delayMilliseconds > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMilliseconds));
            isReplyShort = !writeAll(client, reply);
            ::close(client);
        });
    }

    ~ScriptedDaemon()
    {
        if (server.joinable())
            server.join();
        CHECK_MSG(!isReplyShort, "the scripted daemon wrote its whole reply");
        if (listenFileDescriptor >= 0)
            ::close(listenFileDescriptor);
        ::unlink(path.c_str());
    }
};

// One request against a daemon scripted to answer `reply`.
struct RequestOutcome {
    bool granted = false;
    std::uint16_t port = 4242;
    std::string token = "stale-token";
    std::string certSHA256 = "stale-pin";
    std::string error;
    Ubiquitty::Protocol::Sync::QUICTokenRefusal refusal = Ubiquitty::Protocol::Sync::QUICTokenRefusal::Unreachable;
};
RequestOutcome requestAgainst(std::vector<std::uint8_t> reply, int delayMilliseconds = 0)
{
    ScriptedDaemon daemon(std::move(reply), delayMilliseconds);
    RequestOutcome outcome;
    outcome.granted = Ubiquitty::Protocol::Sync::requestQUICToken(daemon.path, outcome.port, outcome.token, outcome.certSHA256, outcome.error, outcome.refusal);
    if (!outcome.granted) {
        CHECK_EQ(outcome.port, 0);
        CHECK(outcome.token.empty());
        CHECK(outcome.certSHA256.empty());
    }
    return outcome;
}

std::vector<std::uint8_t> tokenReply(Ubiquitty::Protocol::Local::LocalQUICTokenStatus status)
{
    using namespace Ubiquitty::Protocol::Local;
    LocalQUICTokenReply grant { .correlation = 1, .daemonInstanceID = 42, .status = status };
    if (status == LocalQUICTokenStatus::Granted) {
        grant.port = 44321;
        constexpr std::string_view token = "one-time";
        grant.tokenLength = static_cast<std::uint8_t>(token.size());
        std::copy(token.begin(), token.end(), grant.token.begin());
        grant.certificateSHA256.fill(0x5a);
    }
    const auto encoded = encodeLocalQUICTokenReply(grant);
    if (!encoded)
        return {};
    return { encoded->view().begin(), encoded->view().end() };
}

} // namespace

// The loop past the connect, against a real (scripted) peer: the grant, the
// refusal, the malformed payload, the framing fault, the early close, and the
// deadline.
TEST(testTokenRequestReadsWhatTheDaemonAnswers)
{
    using namespace Ubiquitty::Protocol::Local;

    // A clean grant fills every out-parameter.
    {
        const RequestOutcome outcome = requestAgainst(tokenReply(LocalQUICTokenStatus::Granted));
        CHECK(outcome.granted);
        CHECK(outcome.port == 44321);
        CHECK(outcome.token == "one-time");
        CHECK(outcome.certSHA256 == std::string(32, '\x5a'));
        CHECK(outcome.error.empty());
        CHECK(outcome.refusal == Ubiquitty::Protocol::Sync::QUICTokenRefusal::None);
    }

    // status 1: the daemon has no QUIC listener to hand out.
    {
        const RequestOutcome outcome = requestAgainst(tokenReply(LocalQUICTokenStatus::Disabled));
        CHECK(!outcome.granted);
        CHECK(outcome.error.find("not enabled") != std::string::npos);
        // The one refusal a restart can fix, and the only answer that may read
        // as one: --ensure-daemon kills and respawns on this verdict, so a
        // transport failure promoted to it would cost every hosted session.
        CHECK(outcome.refusal == Ubiquitty::Protocol::Sync::QUICTokenRefusal::Disabled);
    }

    // A grant with an invalid zero listener port does not decode.
    {
        auto malformed = tokenReply(LocalQUICTokenStatus::Granted);
        malformed[localControlReplyBytes] = 0;
        malformed[localControlReplyBytes + 1] = 0;
        const RequestOutcome outcome = requestAgainst(malformed);
        CHECK(!outcome.granted);
        CHECK(outcome.error.find("malformed") != std::string::npos);
        CHECK(outcome.refusal == Ubiquitty::Protocol::Sync::QUICTokenRefusal::Unreachable);
    }

    // A reply with trailing bytes past the fixed ceiling is refused.
    {
        auto oversize = tokenReply(LocalQUICTokenStatus::Granted);
        oversize.resize(maxLocalQUICTokenReplyBytes + 1);
        const RequestOutcome outcome = requestAgainst(oversize);
        CHECK(!outcome.granted);
        CHECK(outcome.error.find("malformed") != std::string::npos);
    }

    // The daemon hangs up without answering.
    {
        const RequestOutcome outcome = requestAgainst({});
        CHECK(!outcome.granted);
        CHECK(outcome.error.find("closed") != std::string::npos);
    }

    // Nothing arrives before the deadline. This case costs the client's full
    // ~3s budget — the deadline arm cannot run any other way.
    {
        const RequestOutcome outcome = requestAgainst({}, 3600);
        CHECK(!outcome.granted);
        CHECK(outcome.error.find("did not answer in time") != std::string::npos);
    }
}

// The connect is exercised here; everything past it runs against the scripted
// daemon above.
TEST(testTokenRequestAgainstAnAbsentDaemonNamesTheReasonAndThePath)
{
    const std::string absent = "/tmp/ubiquitty-no-such-socket-for-this-case.sock";
    std::uint16_t port = 4242;
    std::string token = "unchanged";
    std::string certSHA256 = "unchanged";
    std::string error;

    const bool didGrant = Ubiquitty::Protocol::Sync::requestQUICToken(absent, port, token, certSHA256, error);

    CHECK(!didGrant);
    CHECK_MSG(error.find(absent) != std::string::npos,
        "the error names the path it tried, which is the half a caller can act on");
    CHECK_MSG(error.find(':') != std::string::npos,
        "and the backend's reason before it, rather than the path alone");

    // The out-parameters are a caller's buffers, and a refused request must not
    // leave a half-filled grant behind for one that ignores the return.
    CHECK_EQ(port, 0);
    CHECK(token.empty());
    CHECK(certSHA256.empty());
}

UBIQUITTY_TEST_MAIN()
