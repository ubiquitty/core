// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Tests/Host/quic_test_mint.h — mint a one-time QUIC token from a test daemon.
//
// Two mints, because the daemon answers the two differently.
//
// mintQUICBootstrap is the product's own path and the one a test that wants a
// token uses: `ubiquitty-host --quic-bootstrap` connects to the running daemon
// as the bundled binary, so originOf resolves Local and the grant lands.
//
// mintQUICTokenForTest uses the bounded daemon-local producer as the test
// binary itself — a same-uid local agent, which the daemon refuses. It is the
// negative control; only the bundled trusted owner may mint a token.
//

#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Session/DaemonSocketLeaf.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <utility>

/// Mints the way the product mints.
///
/// Runs `<hostBinaryPath> --quic-bootstrap` against the daemon at `macHome` and
/// parses the line it prints (quic_port / quic_token / quic_cert_sha256), which
/// is what the ssh bootstrap relays to the dialer. The bootstrap connects to the
/// daemon as the bundled binary, so the daemon's kernel-verified peer resolves
/// Local and the grant carries the interactive-user mark.
///
/// The bootstrap is pinned to the socket the binary's own daemon binds. One
/// that is not ensures a daemon first, and where none answers it starts one
/// detached, in a session of its own, that the test never ends. The daemon must
/// already be up with QUIC enabled; where none answers, the mint fails.
///
/// - Parameters:
///   - hostBinaryPath: The built ubiquitty-host, the same one the daemon runs.
///   - macHome: The daemon's control directory.
///   - info: Receives the port, the one-time token bytes and the certificate pin.
/// - Returns: Whether the bootstrap printed a complete, well-formed line.
inline bool mintQUICBootstrap(const std::string& hostBinaryPath, const std::string& macHome, Ubiquitty::Protocol::Sync::QUICBootstrapInfo& info)
{
    const std::string socketLeaf = Ubiquitty::Session::socketLeafForBinary(hostBinaryPath.c_str());
    if (socketLeaf.empty())
        return false;
    int pipeFileDescriptors[2] = { -1, -1 };
    if (::pipe(pipeFileDescriptors) != 0)
        return false;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        return false;
    }
    if (pid == 0) {
        ::close(pipeFileDescriptors[0]);
        ::dup2(pipeFileDescriptors[1], 1); // the bootstrap line
        ::close(pipeFileDescriptors[1]);
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::execl(hostBinaryPath.c_str(), hostBinaryPath.c_str(), "--quic-bootstrap", "--socket", socketLeaf.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(pipeFileDescriptors[1]);
    // Bounded: a bootstrap that wedges leaves the suite reporting a failure
    // rather than hanging as a stalled run.
    std::string line;
    for (int elapsed = 0; elapsed < 20000; elapsed += 50) {
        pollfd p { pipeFileDescriptors[0], POLLIN, 0 };
        if (::poll(&p, 1, 50) <= 0)
            continue;
        char buffer[512];
        const ssize_t n = ::read(pipeFileDescriptors[0], buffer, sizeof(buffer));
        if (n <= 0)
            break;
        line.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(pipeFileDescriptors[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    return Ubiquitty::Protocol::Sync::parseQUICBootstrapLine(line, info);
}

/// Mints the way the product mints (``mintQUICBootstrap``), and hands back the
/// line's three fields.
///
/// - Parameters:
///   - hostBinaryPath: The built ubiquitty-host, the same one the daemon runs.
///   - macHome: The daemon's control directory.
///   - token: Receives the one-time token bytes.
///   - port: Receives the QUIC data-plane port.
///   - certSHA256: Receives the certificate pin.
/// - Returns: Whether the bootstrap printed a complete, well-formed line.
inline bool mintQUICTokenViaBootstrap(const std::string& hostBinaryPath, const std::string& macHome,
    std::string& token, std::uint16_t& port, std::string& certSHA256)
{
    Ubiquitty::Protocol::Sync::QUICBootstrapInfo info;
    if (!mintQUICBootstrap(hostBinaryPath, macHome, info))
        return false;
    token = std::move(info.token);
    port = info.port;
    certSHA256 = std::move(info.certificateSHA256);
    return true;
}

// Connect to the daemon at `sockPath` as this test binary and ask for a token.
// A same-uid local agent is refused the mint, so this returns false against a
// daemon that enforces the gate — it is the negative control, not a way to get a
// token. Fills `token` + `port` on a daemon that grants one.
inline bool mintQUICTokenForTest(const std::string& sockPath, std::string& token, std::uint16_t& port, std::string& certSHA256)
{
    std::string error;
    return Ubiquitty::Protocol::Sync::requestQUICToken(sockPath, port, token, certSHA256, error);
}
