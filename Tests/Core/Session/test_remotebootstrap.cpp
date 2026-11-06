// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_remotebootstrap.cpp — the portable /bin/sh bootstrap script builder
// (Session/RemoteBootstrap.h). Pins
// the SHAPE of the script (so a frontend reproduces the exact contract the host
// binary + RemoteSessionCore expect) without launching a shell:
//
//   * the deploy sentinel (deploySentinel) — the single source of truth shared
//     with RemoteSessionCore's pre-ack sniff — appears in the producer, printed
//     with its `uname -sm` payload, and triggers `exit 1`;
//   * the connect-or-deploy contract: a working binary gates the run, and the
//     script never builds — no source-checkout loop, no compiler invocation;
//   * the SOCK reattach token is present and substitutable, and pins exactly
//     once; an empty socket or a token-less command is returned unchanged;
//   * localBridgeCommand carries the same SOCK token and the supplied binary.
//
// Header-only, links standalone.

#include "Core/Session/BootstrapResponse.h"
#include "Core/Session/RemoteBootstrap.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Ubiquitty::Session::deploySentinel;
using Ubiquitty::Session::localBridgeCommand;
using Ubiquitty::Session::reattachSocketToken;
using Ubiquitty::Session::remoteBootstrapCommand;
using Ubiquitty::Session::substituteReattachSocket;

static bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

static std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
        ++n;
    return n;
}

TEST(aHandoverDropsOnlyTheExactPredecessorSocketPin)
{
    const auto remote = Ubiquitty::Session::remoteTransportPlan("dev.example");
    const auto local = Ubiquitty::Session::localTransportPlan("'/Applications/Ubiquitty.app/host'");
    for (const auto& command : { remote.shellCommand, remote.quicBootstrapCommand, local.shellCommand, local.quicBootstrapCommand }) {
        const auto pinned = substituteReattachSocket(command, "predecessor.sock");
        CHECK(pinned != command);
        CHECK(Ubiquitty::Session::clearReattachSocket(pinned, "predecessor.sock") == command);
        CHECK(Ubiquitty::Session::clearReattachSocket(pinned, "other.sock") == pinned);
        CHECK(Ubiquitty::Session::clearReattachSocket(pinned, "") == pinned);
        CHECK(Ubiquitty::Session::clearReattachSocket(command, "predecessor.sock") == command);
        const auto repinned = substituteReattachSocket(Ubiquitty::Session::clearReattachSocket(pinned, "predecessor.sock"), "successor.sock");
        CHECK(contains(repinned, "SOCK=\"successor.sock\";"));
        CHECK(!contains(repinned, "predecessor.sock"));
    }
    CHECK(Ubiquitty::Session::clearReattachSocket("echo predecessor.sock", "predecessor.sock") == "echo predecessor.sock");
}

TEST(remotebootstrapCases)
{
    const auto plan = Ubiquitty::Session::remoteTransportPlan("user@host.local");
    CHECK(plan.shellCommand.starts_with("exec ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3 'user@host.local' "));
    // The bootstrap runs ssh under -v, whose transcript says where it
    // connected; it resolves nothing itself and names no host on its output.
    CHECK(contains(plan.quicBootstrapCommand, "ssh -o BatchMode=yes -o ConnectTimeout=10 "));
    CHECK(contains(plan.quicBootstrapCommand, "=3 -v 'user@host.local' 'exec /bin/sh -c "));
    CHECK(!contains(plan.quicBootstrapCommand, "ssh -G"));
    CHECK(!contains(plan.quicBootstrapCommand, "quic_host"));
    CHECK(!contains(plan.quicBootstrapCommand, "HostName="));
    CHECK(!contains(plan.quicBootstrapCommand, "HostKeyAlias"));
    // The session's own ssh is not made verbose: its standard error is the
    // person's to read.
    CHECK(!contains(plan.shellCommand, " -v "));
    // The probe is the same command with its token set: one connection that
    // shares no other's and runs nothing.
    CHECK(count(plan.quicBootstrapCommand, Ubiquitty::Session::reachProbeToken) == 1);
    CHECK(plan.quicBootstrapCommand.starts_with(Ubiquitty::Session::reachProbeToken));
    const std::string probe = Ubiquitty::Session::substituteReachProbe(plan.quicBootstrapCommand);
    CHECK(probe.starts_with("REACH_PROBE=\"1\"; "));
    CHECK(contains(probe, "-v -o ControlMaster=no -o ControlPath=none -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/dev/null "
                          "-o GlobalKnownHostsFile=/dev/null -o VerifyHostKeyDNS=no 'user@host.local' "
            + Ubiquitty::Transport::shellSingleQuote(Ubiquitty::Transport::remoteShellCommand("true")) + "; fi; "));
    CHECK(contains(probe, "then exec ssh -o BatchMode=yes "));
    CHECK(probe.size() == plan.quicBootstrapCommand.size() + 1);
    // A local plan runs no ssh and has nothing to probe.
    const auto localPlan = Ubiquitty::Session::localTransportPlan("'/Applications/Ubiquitty.app/host'");
    CHECK(Ubiquitty::Session::substituteReachProbe(localPlan.quicBootstrapCommand) == localPlan.quicBootstrapCommand);
    CHECK(localPlan.quicHost == "127.0.0.1");
    CHECK(plan.quicHost == "host.local");
    CHECK(contains(plan.shellCommand, "--attach"));
    CHECK(contains(plan.quicBootstrapCommand, "--quic-bootstrap"));

    const std::string sentinel = deploySentinel;
    CHECK(sentinel == "UBIQUITTY-NEED-DEPLOY"); // the wire contract — must not drift

    // ---- remoteBootstrapCommand SHAPE ----
    const std::string remote = remoteBootstrapCommand();

    // The sentinel rides the command's stdout (= the Sync pipe) with the
    // remote's `uname -sm`, then exits 1.
    CHECK(contains(remote, sentinel));
    CHECK(contains(remote, "printf \"\\n" + sentinel + " %s\\n\" \"$(uname -sm)\""));
    CHECK(contains(remote, "exit 1"));

    // Install root + binary path, binary-presence gate, ensure-daemon, attach.
    CHECK(contains(remote, "D=\"$HOME/.ubiquitty\""));
    CHECK(contains(remote, "[ ! -L \"$D\" ] || exit 1"));
    CHECK(contains(remote, "mkdir -p \"$D\" && chmod 700 \"$D\" || exit 1"));
    CHECK(contains(remote, "[ -d \"$D\" ] || exit 1"));
    CHECK(contains(remote, "B=\"$D/ubiquitty-host\""));
    CHECK(contains(remote, "[ -x \"$B\" ] && \"$B\" --help >/dev/null 2>&1")); // working-binary gate
    CHECK(contains(remote, "--ensure-daemon --quic-port 0"));
    CHECK(contains(remote, "exec \"$B\" --attach"));

    // The connect-or-deploy contract: the script never builds. No source
    // checkout discovery, no build-host invocation, no log file, no compiler.
    CHECK(!contains(remote, "build-host.sh"));
    CHECK(!contains(remote, "for S in"));
    CHECK(!contains(remote, "$UBIQUITTY_HOST_DIR"));
    CHECK(!contains(remote, "host-build.log"));
    CHECK(!contains(remote, "$(cc"));
    CHECK(!contains(remote, "$(clang"));
    CHECK(!contains(remote, "-newer \"$B\"")); // no mtime freshness gate remains
    CHECK(!contains(remote, "macterm"));

    // Exactly one SOCK reattach token to substitute.
    CHECK(contains(remote, reattachSocketToken));
    CHECK(count(remote, std::string(reattachSocketToken)) == 1);

    // Only double quotes at top level so the caller can wrap in single quotes.
    CHECK(!contains(remote, "'"));

    // ---- SOCK substitution ----
    const std::string pinned = substituteReattachSocket(remote, "host-0123456789abcdef.sock");
    CHECK(contains(pinned, "SOCK=\"host-0123456789abcdef.sock\";"));
    CHECK(!contains(pinned, reattachSocketToken)); // the empty token is gone
    CHECK(count(pinned, std::string("SOCK=\"host-0123456789abcdef.sock\";")) == 1); // pinned exactly once
    // The rest of the script is preserved (sentinel + attach still there).
    CHECK(contains(pinned, sentinel));
    CHECK(contains(pinned, "exec \"$B\" --attach"));

    // Empty socket name → unchanged (restore still works, just unpinned).
    CHECK(substituteReattachSocket(remote, "") == remote);

    // A command with no token → unchanged.
    const std::string noToken = "echo hello";
    CHECK(substituteReattachSocket(noToken, "host-x.sock") == noToken);

    // Empty command → unchanged (empty).
    CHECK(substituteReattachSocket("", "host-x.sock").empty());

    // ---- localBridgeCommand SHAPE ----
    const std::string local = localBridgeCommand("'/Applications/ub.app/ubiquitty-host'");
    CHECK(contains(local, reattachSocketToken)); // same reattach token
    CHECK(contains(local, "'/Applications/ub.app/ubiquitty-host'")); // the supplied (quoted) binary
    CHECK(contains(local, "--ensure-daemon --quic-port 0"));
    CHECK(contains(local, "--attach"));
    // The local bridge is also SOCK-substitutable for a pinned restore.
    const std::string localPinned = substituteReattachSocket(local, "host-fedcba9876543210.sock");
    CHECK(contains(localPinned, "SOCK=\"host-fedcba9876543210.sock\";"));
    CHECK(!contains(localPinned, reattachSocketToken));
}

TEST(testWorkspaceTransportPinsBothCarriersToTheDiscoveredDaemon)
{
    constexpr std::string_view socket = "host-0123456789abcdef.sock";
    for (const auto& plan : { Ubiquitty::Session::localTransportPlan("'/path/ubiquitty-host'"),
             Ubiquitty::Session::remoteTransportPlan("user@host.local") }) {
        const auto bridge = substituteReattachSocket(plan.shellCommand, socket);
        const auto bootstrap = substituteReattachSocket(plan.quicBootstrapCommand, socket);
        for (const auto& command : { bridge, bootstrap }) {
            CHECK(count(command, "SOCK=\"host-0123456789abcdef.sock\";") == 1);
            CHECK(!contains(command, reattachSocketToken));
            CHECK(contains(command, "if [ -n \"$SOCK\" ]; then exec "));
            CHECK(contains(command, "--socket \"$SOCK\"; else exec "));
            CHECK(!contains(command, "${SOCK:+"));
        }
        CHECK(contains(bridge, "--attach"));
        CHECK(contains(bootstrap, "--quic-bootstrap"));
    }
}

// Only this machine's own bridge places a host on this machine: the local
// plan, pinned to a socket or not, over shell-quoted binaries. A command over
// ssh, a QUIC carrier off the loopback address, or a second command smuggled
// beside a binary is another machine's. The bridge and the bootstrap may name
// different binaries, as a restored tab's do after the app moves.
TEST(onlyTheLocalTransportPlanPlacesItsHostOnThisMachine)
{
    using Ubiquitty::Session::isLocalTransportPlan;
    constexpr std::string_view socket = "host-0123456789abcdef.sock";
    const auto local = Ubiquitty::Session::localTransportPlan("'/Applications/Ubiquitty.app/ubiquitty host'");
    CHECK(isLocalTransportPlan(local.shellCommand, local.quicHost, local.quicBootstrapCommand));
    CHECK(isLocalTransportPlan(substituteReattachSocket(local.shellCommand, socket), local.quicHost, substituteReattachSocket(local.quicBootstrapCommand, socket)));
    CHECK(isLocalTransportPlan(local.shellCommand, "", ""));
    CHECK(isLocalTransportPlan(local.shellCommand, "::1", local.quicBootstrapCommand));
    const auto quoted = Ubiquitty::Session::localTransportPlan("'/opt/it'\\''s here/ubiquitty-host'");
    CHECK(isLocalTransportPlan(quoted.shellCommand, quoted.quicHost, quoted.quicBootstrapCommand));

    const auto remote = Ubiquitty::Session::remoteTransportPlan("user@host.local");
    CHECK(!isLocalTransportPlan(remote.shellCommand, remote.quicHost, remote.quicBootstrapCommand));
    CHECK(!isLocalTransportPlan(remote.shellCommand, "", ""));
    CHECK(!isLocalTransportPlan(local.shellCommand, "host.local", local.quicBootstrapCommand));
    CHECK(!isLocalTransportPlan(local.shellCommand, "127.0.0.2", local.quicBootstrapCommand));
    CHECK(!isLocalTransportPlan(local.shellCommand, local.quicHost, remote.quicBootstrapCommand));
    const auto other = Ubiquitty::Session::localTransportPlan("'/tmp/other-host'");
    CHECK(isLocalTransportPlan(local.shellCommand, local.quicHost, other.quicBootstrapCommand));
    CHECK(isLocalTransportPlan(substituteReattachSocket(other.shellCommand, socket), local.quicHost, local.quicBootstrapCommand));
    CHECK(!isLocalTransportPlan(local.shellCommand, local.quicHost, "SOCK=\"\"; '/tmp/other-host' --quic-bootstrap ${SOCK:+--socket \"$SOCK\"}; ssh far"));
    CHECK(!isLocalTransportPlan(localBridgeCommand("'/bin/host'; ssh far '/bin/host'"), "", ""));
    CHECK(!isLocalTransportPlan(localBridgeCommand("/bin/host"), "", ""));
    CHECK(!isLocalTransportPlan(localBridgeCommand("'/bin/host'") + "; ssh far", "", ""));
    CHECK(!isLocalTransportPlan("ssh far " + localBridgeCommand("'/bin/host'"), "", ""));
    std::string unsafeSocket = local.shellCommand;
    unsafeSocket.replace(0, std::string_view(reattachSocketToken).size(), "SOCK=\"$(ssh far)\";");
    CHECK(!isLocalTransportPlan(unsafeSocket, "", ""));
    CHECK(!isLocalTransportPlan("", "", ""));
}

TEST(passiveControlBootstrapOnlyReachesAnExistingDaemon)
{
    const std::string command = Ubiquitty::Session::passiveControlRemoteCommand();
    CHECK(contains(command, "[ -x \"$B\" ] || exit 1"));
    CHECK(contains(command, "B=\"$HOME/.ubiquitty/ubiquitty-host\""));
    CHECK(contains(command, "printf \"\\n" + std::string(Ubiquitty::Session::syncSentinel) + "\\n\""));
    CHECK(contains(command, "if [ -n \"$SOCK\" ]; then exec \"$B\" --attach --socket \"$SOCK\"; else exec \"$B\" --attach; fi"));
    for (const auto* forbidden : { "--ensure-daemon", "--quic-bootstrap", "mkdir", "chmod", "mv ", "rm ", "--help", "--install", "--daemon", deploySentinel })
        CHECK(!contains(command, forbidden));
    CHECK(!contains(command, "'"));
    const std::string pinned = substituteReattachSocket(command, "host-0123456789abcdef.sock");
    CHECK(count(pinned, "SOCK=\"host-0123456789abcdef.sock\";") == 1);
    CHECK(!contains(pinned, reattachSocketToken));
}

TEST(bootstrapResponseSeparatesNoiseAndBinaryAtEveryChunkBoundary)
{
    using Ubiquitty::Session::BootstrapResponse;
    const std::string text = "Welcome 👋\r\npartial startup line\nUBIQUITTY-SYNC\r\n";
    const std::array<std::uint8_t, 7> protocol { 0x81, 0, 0, 0, 2, 1, 0 };
    std::vector<std::uint8_t> stream(text.begin(), text.end());
    stream.insert(stream.end(), protocol.begin(), protocol.end());
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        BootstrapResponse parser;
        std::vector<std::uint8_t> decoded;
        const auto first = parser.consume(std::span(stream).first(split));
        decoded.insert(decoded.end(), first.protocolBytes.begin(), first.protocolBytes.end());
        const auto second = parser.consume(std::span(stream).subspan(split));
        decoded.insert(decoded.end(), second.protocolBytes.begin(), second.protocolBytes.end());
        CHECK(second.status == BootstrapResponse::Status::Ready);
        CHECK(std::ranges::equal(decoded, protocol));
    }
}

TEST(bootstrapResponseRecognizesNoisyFragmentedDeployAndBoundsStartup)
{
    using Ubiquitty::Session::BootstrapResponse;
    BootstrapResponse parser;
    const std::string startup = "banner\nUBIQUITTY-NEED-DEPLOY Darwin arm64\r\n";
    BootstrapResponse::Status status { BootstrapResponse::Status::Waiting };
    for (const char byte : startup) {
        const auto value = static_cast<std::uint8_t>(byte);
        status = parser.consume({ &value, 1 }).status;
    }
    CHECK(status == BootstrapResponse::Status::Deploy);
    CHECK(parser.platform() == "Darwin arm64");

    parser = {};
    const std::vector<std::uint8_t> longLine(4097, 'x');
    CHECK(parser.consume(longLine).status == BootstrapResponse::Status::Invalid);
    parser = {};
    const std::array<std::uint8_t, 2> shortLine { 'x', '\n' };
    for (unsigned index = 0; index < 8192; ++index) {
        CHECK(parser.consume(shortLine).status == BootstrapResponse::Status::Waiting);
    }
    CHECK(parser.consume(shortLine).status == BootstrapResponse::Status::Invalid);

    parser = {};
    const std::array<std::uint8_t, 3> malformed { 'x', '\n', 0 };
    CHECK(parser.consume(malformed).status == BootstrapResponse::Status::Invalid);
}

TEST(aCanonicalByeAndLeadingBlankStartupLinesHaveDistinctBoundaries)
{
    using Ubiquitty::Session::BootstrapResponse;
    const std::array<std::uint8_t, 6> protocol { 10, 0, 3, 0, 0, 0 };
    for (const std::string& startup : { std::string {}, std::string { "\nBanner\nUBIQUITTY-SYNC\n" } }) {
        std::vector<std::uint8_t> stream(startup.begin(), startup.end());
        stream.insert(stream.end(), protocol.begin(), protocol.end());
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            BootstrapResponse parser;
            std::vector<std::uint8_t> decoded;
            const auto accept = [&](std::span<const std::uint8_t> bytes) {
                const auto result = parser.consume(bytes);
                decoded.insert(decoded.end(), result.protocolPrefix.begin(), result.protocolPrefix.end());
                decoded.insert(decoded.end(), result.protocolBytes.begin(), result.protocolBytes.end());
                return result.status;
            };
            (void)accept(std::span(stream).first(split));
            CHECK(accept(std::span(stream).subspan(split)) == BootstrapResponse::Status::Ready);
            CHECK(std::ranges::equal(decoded, protocol));
        }
    }
    BootstrapResponse parser;
    const std::array<std::uint8_t, 1> first { 10 };
    CHECK(parser.consume(first).status == BootstrapResponse::Status::Waiting);
    CHECK(parser.isIncomplete());
    const std::array<std::uint8_t, 2> banner { 'x', '\n' };
    CHECK(parser.consume(banner).status == BootstrapResponse::Status::Waiting);
}

TEST(aDirectCanonicalWelcomeNeedsNoShellSentinel)
{
    using Ubiquitty::Session::BootstrapResponse;
    const std::array<std::uint8_t, 4> protocol { 2, 0, 1, 0 };
    for (std::size_t split = 0; split <= protocol.size(); ++split) {
        BootstrapResponse parser;
        std::vector<std::uint8_t> decoded;
        const auto accept = [&](std::span<const std::uint8_t> bytes) {
            const auto result = parser.consume(bytes);
            decoded.insert(decoded.end(), result.protocolPrefix.begin(), result.protocolPrefix.end());
            decoded.insert(decoded.end(), result.protocolBytes.begin(), result.protocolBytes.end());
            return result.status;
        };
        (void)accept(std::span(protocol).first(split));
        CHECK(accept(std::span(protocol).subspan(split)) == BootstrapResponse::Status::Ready);
        CHECK(std::ranges::equal(decoded, protocol));
    }
}

TEST(bootstrapNeverResynchronizesBinaryAfterUnannouncedShellText)
{
    using Ubiquitty::Session::BootstrapResponse;
    for (const std::uint8_t first : { std::uint8_t { 'x' }, std::uint8_t { 0x81 }, std::uint8_t { 0x8E } }) {
        BootstrapResponse parser;
        CHECK(parser.consume({ &first, 1 }).status == BootstrapResponse::Status::Waiting);
        const std::array<std::uint8_t, 4> welcome { 2, 0, 1, 0 };
        const auto result = parser.consume(welcome);
        CHECK(result.status == BootstrapResponse::Status::Invalid);
        CHECK(result.protocolPrefix.empty());
        CHECK(result.protocolBytes.empty());
    }
}

/// A bootstrap that never reached its host is told apart from one the host
/// declined.
///
/// The desktop factory gives up on QUIC for the session's life when a host
/// declines; a host the network did not reach yet has declined nothing, and
/// latching on it left a session that woke on new Wi-Fi on the stream carrier
/// for good, blaming the host for a missing pin.
TEST(quicBootstrapThatNeverReachedItsHostIsNotADecline)
{
    using Ubiquitty::Session::classifyQUICBootstrapMiss;
    using Ubiquitty::Session::QUICBootstrapMiss;
    CHECK(classifyQUICBootstrapMiss("ssh: connect to host build.example port 22: Operation timed out\n") == QUICBootstrapMiss::Unreached);
    CHECK(classifyQUICBootstrapMiss("ssh: connect to host 10.0.0.9 port 22: No route to host\n") == QUICBootstrapMiss::Unreached);
    CHECK(classifyQUICBootstrapMiss("ssh: Could not resolve hostname nowhere.invalid: nodename nor servname provided\n")
        == QUICBootstrapMiss::Unreached);
    CHECK(classifyQUICBootstrapMiss("kex_exchange_identification: Connection closed by remote host\n") == QUICBootstrapMiss::Unreached);

    // Under -v the same words turn up mid-line for a host that was reached:
    // only a line that begins with a failure is one.
    CHECK(classifyQUICBootstrapMiss("debug1: kex_exchange_identification: banner line 0: maintenance tonight\n"
                                    "debug1: Connection established.\nsh: ubiquitty-host: command not found\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("debug1: Connecting to dual [2001:db8::1] port 22.\n"
                                    "debug1: connect to address 2001:db8::1 port 22: No route to host\n"
                                    "debug1: Connecting to dual [192.0.2.1] port 22.\ndebug1: Connection established.\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("debug1: Reading configuration data /etc/ssh/ssh_config\n"
                                    "debug1: Connecting to build.example [192.0.2.1] port 22.\n"
                                    "debug1: connect to address 192.0.2.1 port 22: Operation timed out\n"
                                    "ssh: connect to host build.example port 22: Operation timed out\n")
        == QUICBootstrapMiss::Unreached);
    CHECK(classifyQUICBootstrapMiss("debug1: Connecting to build.example [192.0.2.1] port 22.\n"
                                    "ssh: connect to host build.example port 22: Connection refused\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("Welcome to ssh: connect to host is how you got here\n") == QUICBootstrapMiss::Declined);

    CHECK(classifyQUICBootstrapMiss("") == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("sh: ubiquitty-host: command not found\n") == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("user@build.example: Permission denied (publickey).\n") == QUICBootstrapMiss::Declined);
}

/// A run abandoned at its deadline reached its host.
///
/// ssh gives up on a connect and on the banner exchange within its own
/// timeout, which ends before the bootstrap's deadline, so a run still silent
/// at the deadline is a slow login or a slow daemon start. Reading it as
/// unreached handed Core no channel on every attempt, and a host whose daemon
/// takes longer than the deadline to start never fell back to the stream
/// carrier that would have worked.
TEST(aQUICBootstrapThatRanOutOfTimeReachedItsHost)
{
    using Ubiquitty::Session::classifyQUICBootstrapMiss;
    using Ubiquitty::Session::QUICBootstrapMiss;
    static_assert(Ubiquitty::Session::quicBootstrapDeadlineSeconds > Ubiquitty::Session::sshConnectTimeoutSeconds);
    // Under -v the same words turn up mid-line for a host that was reached:
    // only a line that begins with a failure is one.
    CHECK(classifyQUICBootstrapMiss("debug1: kex_exchange_identification: banner line 0: maintenance tonight\n"
                                    "debug1: Connection established.\nsh: ubiquitty-host: command not found\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("debug1: Connecting to dual [2001:db8::1] port 22.\n"
                                    "debug1: connect to address 2001:db8::1 port 22: No route to host\n"
                                    "debug1: Connecting to dual [192.0.2.1] port 22.\ndebug1: Connection established.\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("debug1: Reading configuration data /etc/ssh/ssh_config\n"
                                    "debug1: Connecting to build.example [192.0.2.1] port 22.\n"
                                    "debug1: connect to address 192.0.2.1 port 22: Operation timed out\n"
                                    "ssh: connect to host build.example port 22: Operation timed out\n")
        == QUICBootstrapMiss::Unreached);
    CHECK(classifyQUICBootstrapMiss("debug1: Connecting to build.example [192.0.2.1] port 22.\n"
                                    "ssh: connect to host build.example port 22: Connection refused\n")
        == QUICBootstrapMiss::Declined);
    CHECK(classifyQUICBootstrapMiss("Welcome to ssh: connect to host is how you got here\n") == QUICBootstrapMiss::Declined);

    CHECK(classifyQUICBootstrapMiss("") == QUICBootstrapMiss::Declined);
    const auto plan = Ubiquitty::Session::remoteTransportPlan("user@build.example");
    CHECK(plan.quicBootstrapCommand.find("-o ConnectTimeout=" + std::to_string(Ubiquitty::Session::sshConnectTimeoutSeconds) + " ")
        != std::string::npos);
}

/// A refused port 22 is the host's answer, as Core reads it everywhere else.
///
/// The machine is up and nothing listens on the port, so asking again hears
/// the same thing; the stream carrier is what the next attempt should try.
TEST(aRefusedSSHPortIsTheHostsAnswer)
{
    using Ubiquitty::Session::classifyQUICBootstrapMiss;
    using Ubiquitty::Session::QUICBootstrapMiss;
    CHECK(classifyQUICBootstrapMiss("ssh: connect to host build.example port 22: Connection refused\n") == QUICBootstrapMiss::Declined);
}

TEST(discoveredHostCommandsPreserveOwnerAndQuoteExecutable)
{
    using namespace Ubiquitty;
    const Session::HostInstanceInfo instance { "host-0123456789abcdef.sock", "/Applications/O'Brien $(touch nope)/ubiquitty-host", 7 };
    CHECK(Session::isValidHostInstance(instance));
    const auto executable = Transport::shellSingleQuote(instance.executable);
    for (const auto& command : { Session::passiveControlRemoteCommand(), Session::quicBootstrapRemoteCommand() }) {
        const auto pinned = Session::substituteReattachSocket(command, instance.socketName);
        const auto redirected = Session::hostInstanceBootstrap(pinned, instance);
        CHECK(contains(redirected, executable));
        CHECK(contains(redirected, instance.socketName));
        CHECK(!contains(redirected, "\"$HOME/.ubiquitty/ubiquitty-host\""));
        CHECK(Session::hostInstanceBootstrap(redirected, instance) == redirected);
    }
    const auto local = Session::hostInstanceTransportPlan("", instance, "'/path/bundled/ubiquitty-host'");
    CHECK(contains(local.shellCommand, executable));
    CHECK(contains(local.shellCommand, instance.socketName));
    CHECK(count(local.shellCommand, "SOCK=\"" + instance.socketName + "\";") == 1);
    const auto remote = Session::hostInstanceTransportPlan("laptop", instance, "");
    CHECK(!remote.shellCommand.empty());
    CHECK(contains(remote.shellCommand, instance.socketName));
    CHECK(contains(remote.quicBootstrapCommand, instance.socketName));
    auto invalid = instance;
    invalid.socketName = "../../host.sock";
    CHECK(Session::hostInstanceTransportPlan("laptop", invalid, "").shellCommand.empty());
    CHECK(Session::hostInstanceTransportPlan("", instance, "").shellCommand.empty());
    CHECK(Session::hostInstanceBootstrap(Session::quicBootstrapRemoteCommand(), invalid).empty());
}

namespace {

/// What a shell printed on standard output and how it ended.
struct ShellRun {
    std::string output;
    int status = -1;
};

/// Runs a bootstrap command under a login shell with an environment of its own.
///
/// - Parameters:
///   - command: The command.
///   - environment: `NAME=value ` words put in front of it.
///   - shell: The login shell, with its startup configuration disabled.
/// - Returns: What it printed and its exit status.
ShellRun runShell(const std::string& command, const std::string& environment, std::string_view shell = "/bin/sh")
{
    const std::string options = shell == "zsh" ? " -f -c " : shell == "fish" ? " --no-config -c "
                                                                             : " -c ";
    const std::string line = "env " + environment + " " + Ubiquitty::Transport::shellSingleQuote(shell) + options + Ubiquitty::Transport::shellSingleQuote(command) + " 2>&1";
    ShellRun run;
    FILE* pipe = ::popen(line.c_str(), "r");
    CHECK(pipe != nullptr);
    if (!pipe)
        return run;
    std::array<char, 512> buffer {};
    while (const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), pipe))
        run.output.append(buffer.data(), count);
    const int status = ::pclose(pipe);
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return run;
}

/// The installed login shells whose exec syntax the remote must survive.
std::vector<std::string> loginShells()
{
    std::vector<std::string> shells { "/bin/sh" };
    for (const char* shell : { "zsh", "fish" }) {
        if (runShell(std::string("command -v ") + shell + " >/dev/null", "").status == 0)
            shells.emplace_back(shell);
        else
            std::printf("remote shell: %s unavailable\n", shell);
    }
    return shells;
}

/// A private installation whose executable records argument boundaries.
class RecordingHost {
public:
    RecordingHost()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "ubq-remote-shell-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        REQUIRE(created);
        m_root = created;
        m_home = m_root / "home with ' quotes";
        std::filesystem::create_directories(m_home / ".ubiquitty");
        std::filesystem::create_directories(m_root / "bin");
        std::ofstream(binary()) << "#!/bin/sh\n"
                                   "case \"$1\" in\n"
                                   "--help|--ensure-daemon|--probe-daemon) exit 0 ;;\n"
                                   "esac\n"
                                   "printf '<%s>\\n' \"$@\"\n";
        ::chmod(binary().c_str(), 0700);
        const auto ssh = m_root / "bin" / "ssh";
        std::ofstream(ssh) << "#!/bin/sh\nfor last; do :; done\n"
                              "case \"$last\" in \"exec /bin/sh -c \"*) ;; *) echo missing-sh-wrapper; exit 64 ;; esac\n"
                              "case \"$LOGIN_SHELL\" in\n"
                              "zsh) exec zsh -f -c \"$last\" ;;\n"
                              "fish) exec fish --no-config -c \"$last\" ;;\n"
                              "*) exec /bin/sh -c \"$last\" ;;\n"
                              "esac\n";
        ::chmod(ssh.c_str(), 0700);
    }

    ~RecordingHost()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    std::filesystem::path binary() const { return m_home / ".ubiquitty" / "ubiquitty-host"; }

    std::string environment(std::string_view shell) const
    {
        return "UBIQUITTY_HOME= HOME=" + Ubiquitty::Transport::shellSingleQuote(m_home.string())
            + " PATH=" + Ubiquitty::Transport::shellSingleQuote((m_root / "bin").string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"))
            + " LOGIN_SHELL=" + Ubiquitty::Transport::shellSingleQuote(shell);
    }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_home;
};

} // namespace

TEST(remoteExecPreservesScriptBytesUnderLoginShells)
{
    using namespace Ubiquitty::Transport;
    const std::string payload = "apostrophe ' dollar $HOME $(false) `false`\nbackslashes \\\\ \\' tail\\\n";
    const auto command = remoteShellCommand("printf %s " + shellSingleQuote(payload));
    CHECK(command.starts_with("exec /bin/sh -c '"));
    for (const auto& shell : loginShells()) {
        const auto result = runShell(command, "", shell);
        CHECK_MSG(result.status == 0, shell.c_str());
        CHECK_STR_EQ(result.output, payload);
    }
}

TEST(remoteBootstrapsKeepSocketArgumentsAcrossLoginShells)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Session;
    RecordingHost host;
    constexpr std::string_view socket = "host-0123456789abcdef.sock";
    const HostInstanceInfo instance { std::string(socket), host.binary().string(), 7 };
    for (const auto& shell : loginShells()) {
        const auto environment = host.environment(shell);
        for (const bool isPinned : { false, true }) {
            for (const auto& script : { remoteBootstrapCommand(), passiveControlRemoteCommand(), quicBootstrapRemoteCommand(),
                     hostInstanceBootstrap(passiveControlRemoteCommand(), instance), hostInstanceBootstrap(quicBootstrapRemoteCommand(), instance) }) {
                const auto selected = isPinned ? substituteReattachSocket(script, socket) : script;
                const auto result = runShell(Transport::remoteShellCommand(selected), environment, shell);
                CHECK_MSG(result.status == 0, result.output.c_str());
                const auto verb = script.find("--quic-bootstrap") == std::string::npos ? "--attach" : "--quic-bootstrap";
                const std::string expected = std::string("<") + verb + ">\n" + (isPinned ? "<--socket>\n<" + std::string(socket) + ">\n" : "");
                CHECK_MSG(result.output.ends_with(expected), result.output.c_str());
                CHECK(result.output.find("<--socket ") == std::string::npos);
            }
            for (const auto& plan : { remoteTransportPlan("fixture"), hostInstanceTransportPlan("fixture", instance, "") }) {
                for (const auto& command : { plan.shellCommand, plan.quicBootstrapCommand }) {
                    const auto selected = isPinned ? substituteReattachSocket(command, socket) : clearReattachSocket(command, socket);
                    const auto result = runShell(selected, environment);
                    CHECK_MSG(result.status == 0, result.output.c_str());
                    const auto verb = command.find("--quic-bootstrap") == std::string::npos ? "--attach" : "--quic-bootstrap";
                    const std::string expected = std::string("<") + verb + ">\n" + (isPinned ? "<--socket>\n<" + std::string(socket) + ">\n" : "");
                    CHECK_MSG(result.output.ends_with(expected), result.output.c_str());
                }
            }
        }
    }
}

// The pinned bootstrap, run for real against a stand-in host binary whose
// probe answers as the environment says. A daemon that is gone is reported
// before any Sync byte and the attach never runs; a live one, or a host
// binary that refuses the probe as an unknown argument, attaches; an
// unpinned bootstrap ensures the daemon and runs no probe.
TEST(aPinnedBootstrapReportsAStoppedDaemonInsteadOfAttaching)
{
    using namespace Ubiquitty::Session;
    std::string pattern = (std::filesystem::temp_directory_path() / "ubq-pinned-probe-XXXXXX").string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    const std::filesystem::path directory = pattern;
    const auto binary = directory / "ubiquitty-host";
    std::ofstream(binary) << "#!/bin/sh\n"
                             "case \"$1\" in\n"
                             "--help) exit 0 ;;\n"
                             "--probe-daemon) exit \"$DAEMON_ANSWER\" ;;\n"
                             "--ensure-daemon) printf 'ensured\\n' ;;\n"
                             "--attach) printf 'attached %s\\n' \"$*\" ;;\n"
                             "esac\n";
    ::chmod(binary.c_str(), 0755);
    constexpr std::string_view socket = "host-c756f263bc841e61.sock";
    const std::string quoted = Ubiquitty::Transport::shellSingleQuote(binary.string());
    const std::string bridge = localBridgeCommand(quoted);
    const std::string passive = hostInstanceBootstrap(passiveControlRemoteCommand(), HostInstanceInfo { std::string(socket), binary.string(), 1 });
    REQUIRE(!passive.empty());
    for (const auto& command : { bridge, passive }) {
        const std::string pinned = substituteReattachSocket(command, socket);
        const ShellRun gone = runShell(pinned, "DAEMON_ANSWER=" + std::to_string(daemonProbeGoneStatus));
        CHECK_STR_EQ(gone.output, "\n" + std::string(instanceGoneSentinel) + " " + std::string(socket) + "\n");
        CHECK_EQ(gone.status, 1);
        BootstrapResponse parser;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(gone.output.data());
        CHECK(parser.consume({ bytes, gone.output.size() }).status == BootstrapResponse::Status::InstanceGone);
        CHECK_STR_EQ(parser.goneSocketName(), std::string(socket));
        for (const int answer : { 0, 1, 2 }) {
            const ShellRun live = runShell(pinned, "DAEMON_ANSWER=" + std::to_string(answer));
            CHECK(live.output.find("attached --attach --socket " + std::string(socket)) != std::string::npos);
            CHECK(live.output.find(instanceGoneSentinel) == std::string::npos);
        }
    }
    const ShellRun unpinned = runShell(bridge, "DAEMON_ANSWER=" + std::to_string(daemonProbeGoneStatus));
    CHECK_STR_EQ(unpinned.output, std::string("ensured\nattached --attach\n"));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

namespace {

/// A stand-in host binary that says which installation answered each mode.
///
/// - Parameters:
///   - path: Where it is written.
///   - role: The name it answers with.
void writeStandInHost(const std::filesystem::path& path, std::string_view role)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\n"
                           "case \"$1\" in\n"
                           "--help) exit 0 ;;\n"
                           "--probe-daemon) exit \"$DAEMON_ANSWER\" ;;\n"
                           "--ensure-daemon) printf 'ensured by "
                        << role << "\\n' ;;\n"
                                   "--attach) printf 'attached by "
                        << role << " %s\\n' \"$*\" ;;\n"
                                   "--quic-bootstrap) printf 'quic by "
                        << role << " %s\\n' \"$*\" ;;\n"
                                   "esac\n";
    ::chmod(path.c_str(), 0755);
}

} // namespace

// A picked daemon's plan, run for real on this machine and through a stand-in
// ssh that runs its command here. Pinned, it reaches the picked daemon with
// that daemon's executable and starts nothing. With the pin withdrawn, as a
// session withdraws it for a daemon reported gone, it is the host's own plan:
// the host's binary ensures its daemon and attaches to it, and the picked
// executable never attaches bare to a socket of its own.
TEST(aPickedDaemonsPlanIsTheHostsOwnOnceUnpinned)
{
    using namespace Ubiquitty::Session;
    std::string pattern = (std::filesystem::temp_directory_path() / "ubq-picked-plan-XXXXXX").string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    const std::filesystem::path directory = std::filesystem::path(pattern).lexically_normal();
    const auto picked = directory / "picked" / "ubiquitty-host";
    const auto bundled = directory / "bundled" / "ubiquitty-host";
    const auto home = directory / "home";
    writeStandInHost(picked, "picked");
    writeStandInHost(bundled, "bundled");
    writeStandInHost(home / ".ubiquitty" / "ubiquitty-host", "installed");
    std::filesystem::create_directories(directory / "bin");
    std::ofstream(directory / "bin" / "ssh") << "#!/bin/sh\nfor last; do :; done\nexec /bin/sh -c \"$last\"\n";
    ::chmod((directory / "bin" / "ssh").c_str(), 0755);
    const std::string environment = "HOME=" + Ubiquitty::Transport::shellSingleQuote(home.string()) + " PATH=" + Ubiquitty::Transport::shellSingleQuote((directory / "bin").string() + ":/usr/bin:/bin");
    const HostInstanceInfo instance { "host-c756f263bc841e61.sock", picked.string(), 1 };
    REQUIRE(isValidHostInstance(instance));
    const std::string live = environment + " DAEMON_ANSWER=0";
    const std::string gone = environment + " DAEMON_ANSWER=" + std::to_string(daemonProbeGoneStatus);
    const std::string goneLine = "\n" + std::string(instanceGoneSentinel) + " " + instance.socketName + "\n";

    const RemoteTransportPlan local = hostInstanceTransportPlan("", instance, Ubiquitty::Transport::shellSingleQuote(bundled.string()));
    REQUIRE(!local.shellCommand.empty());
    CHECK_STR_EQ(runShell(local.shellCommand, live).output, "attached by picked --attach --socket " + instance.socketName + "\n");
    CHECK_STR_EQ(runShell(local.shellCommand, gone).output, goneLine);
    CHECK_STR_EQ(runShell(clearReattachSocket(local.shellCommand, instance.socketName), gone).output, std::string("ensured by bundled\nattached by bundled --attach\n"));
    CHECK_STR_EQ(runShell(local.quicBootstrapCommand, live).output, "quic by picked --quic-bootstrap --socket " + instance.socketName + "\n");
    CHECK_STR_EQ(runShell(clearReattachSocket(local.quicBootstrapCommand, instance.socketName), live).output, std::string("quic by bundled --quic-bootstrap\n"));

    const HostInstanceInfo replaced { instance.socketName, bundled.string(), 1 };
    const auto samePath = hostInstanceTransportPlan("", replaced, Ubiquitty::Transport::shellSingleQuote(bundled.string()));
    CHECK_STR_EQ(runShell(samePath.shellCommand, live).output, "attached by bundled --attach --socket " + instance.socketName + "\n");
    CHECK_STR_EQ(runShell(samePath.shellCommand, gone).output, goneLine);

    const RemoteTransportPlan remote = hostInstanceTransportPlan("laptop", instance, "");
    REQUIRE(!remote.shellCommand.empty());
    const std::string sync = "\n" + std::string(syncSentinel) + "\n";
    CHECK_STR_EQ(runShell(remote.shellCommand, live).output, sync + "attached by picked --attach --socket " + instance.socketName + "\n");
    CHECK_STR_EQ(runShell(remote.shellCommand, gone).output, goneLine);
    CHECK_STR_EQ(runShell(clearReattachSocket(remote.shellCommand, instance.socketName), gone).output, "ensured by installed\n" + sync + "attached by installed --attach\n");
    CHECK_STR_EQ(runShell(remote.quicBootstrapCommand, live).output, "quic by picked --quic-bootstrap --socket " + instance.socketName + "\n");
    CHECK_STR_EQ(runShell(clearReattachSocket(remote.quicBootstrapCommand, instance.socketName), live).output, std::string("quic by installed --quic-bootstrap\n"));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// Only a probe of the selected socket can retire the selection.
TEST(aPickedDaemonGivesWayOnlyWhenItIsKnownGone)
{
    using Ubiquitty::Session::isPickedDaemonGone;
    constexpr std::string_view picked = "host-c756f263bc841e61.sock";
    constexpr std::string_view current = "host-b5b7658e15c5127a.sock";
    CHECK(isPickedDaemonGone(picked, picked, ""));
    CHECK(!isPickedDaemonGone(picked, "", current));
    CHECK(!isPickedDaemonGone(picked, "", picked));
    CHECK(!isPickedDaemonGone(picked, "", ""));
    CHECK(!isPickedDaemonGone(picked, current, picked));
    CHECK(!isPickedDaemonGone("", picked, current));
}

// One reader of the gone line serves every bootstrap output.
TEST(theGoneLineIsReadOnceForEveryBootstrap)
{
    using namespace Ubiquitty::Session;
    const InstanceGoneReading gone = readInstanceGoneLine("UBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\r");
    CHECK(gone.kind == InstanceGoneLine::Gone);
    CHECK(gone.socketName == "host-c756f263bc841e61.sock");
    CHECK(readInstanceGoneLine("UBIQUITTY-INSTANCE-GONE ../host.sock").kind == InstanceGoneLine::Malformed);
    CHECK(readInstanceGoneLine("UBIQUITTY-INSTANCE-GONE ").kind == InstanceGoneLine::Malformed);
    CHECK(readInstanceGoneLine("UBIQUITTY-INSTANCE-GONE").kind == InstanceGoneLine::Other);
    CHECK(readInstanceGoneLine("UBIQUITTY-INSTANCE-GONEX host.sock").kind == InstanceGoneLine::Other);
    CHECK(readInstanceGoneLine("quic_port=1").kind == InstanceGoneLine::Other);
}

TEST(bootstrapResponseReadsAPinnedDaemonGoneAndRefusesAPathForItsName)
{
    using Ubiquitty::Session::BootstrapResponse;
    BootstrapResponse parser;
    const std::string startup = "motd\n\nUBIQUITTY-INSTANCE-GONE host-4c6dfcc7ad63d335.sock\r\n";
    BootstrapResponse::Status status { BootstrapResponse::Status::Waiting };
    for (const char byte : startup) {
        const auto value = static_cast<std::uint8_t>(byte);
        status = parser.consume({ &value, 1 }).status;
    }
    CHECK(status == BootstrapResponse::Status::InstanceGone);
    CHECK_STR_EQ(parser.goneSocketName(), std::string("host-4c6dfcc7ad63d335.sock"));
    for (const std::string line : { "UBIQUITTY-INSTANCE-GONE ../../etc/passwd\n", "UBIQUITTY-INSTANCE-GONE a b\n" }) {
        BootstrapResponse refused;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(line.data());
        CHECK(refused.consume({ bytes, line.size() }).status == BootstrapResponse::Status::Invalid);
    }
}

// A window that creates a session or workspace dials the host's current
// daemon, never the daemon that answered the listing or that the person
// picked; one that opens what the listing showed pins that daemon.
TEST(aWindowThatCreatesNeverPinsTheListedDaemon)
{
    using namespace Ubiquitty::Session;
    constexpr std::string_view listed = "host-c756f263bc841e61.sock";
    for (const std::uint32_t reattach : { 0u, 9u }) {
        const DaemonPin created = pinForListedOpen(ListedOpen::Creates, reattach, listed, 17);
        CHECK(created.socketName.empty());
        CHECK_EQ(created.daemonInstanceID, 0u);
    }
    const DaemonPin reattached = pinForListedOpen(ListedOpen::OpensListed, 9, listed, 17);
    CHECK_STR_EQ(reattached.socketName, std::string(listed));
    CHECK_EQ(reattached.daemonInstanceID, 17u);
    const DaemonPin restored = pinForListedOpen(ListedOpen::OpensListed, 0, listed, 17);
    CHECK_STR_EQ(restored.socketName, std::string(listed));
    CHECK_EQ(restored.daemonInstanceID, 0u);
    CHECK(pinForListedOpen(ListedOpen::OpensListed, 9, "../host.sock", 17).socketName.empty());
    CHECK(pinForListedOpen(ListedOpen::OpensListed, 9, "", 17).socketName.empty());
}

UBIQUITTY_TEST_MAIN()
