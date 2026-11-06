// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_quic_bootstrap_run — a QUIC carrier dials the address the bootstrap's
// own ssh run reached, and nowhere when that run named none (Docs/sync.md
// §6.9). The plan's real commands run through the shell against a stand-in
// `ssh` first on PATH, which prints what OpenSSH prints for each route.

#include "Harness.h"

#include "Core/Session/QUICBootstrapRun.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Transport/SSHHost.h"
#include "PAL/Poll.h"

#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>

using namespace Ubiquitty::Session;

namespace {

/// A directory first on PATH holding a stand-in `ssh` and a stand-in host
/// binary, and the route the stand-in plays.
struct StandIn {
    std::filesystem::path directory;
    std::string savedPath;

    StandIn()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "ubq-bootstrap-run-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        directory = pattern;
        // The line a daemon's bootstrap prints: a port, a token and a pin.
        const std::string line = "quic_port=4433 quic_token=" + std::string(64, 'a') + " quic_cert_sha256=" + std::string(64, 'b');
        std::ofstream(directory / "ssh") << "#!/bin/sh\n"
                                            "D=\"$(dirname \"$0\")\"\n"
                                            "ROUTE=\"$(cat \"$D/route\")\"\n"
                                            "printf '%s\\n' \"$*\" >> \"$D/runs\"\n"
                                            "case \"$*\" in\n"
                                            "*ControlPath=none*)\n"
                                            "    printf '%s\\n' \"$*\" >> \"$D/probes\"\n"
                                            "    printf '%s\\n' \"$$\" > \"$D/probe.pid\"\n"
                                            "    case \"$ROUTE\" in\n"
                                            // The pair, then what a real ssh goes on to: the key exchange and the login.
                                            "    shared) printf 'debug1: Connecting to build.example.org [198.51.100.4] port 22.\\ndebug1: Connection established.\\n' >&2; sleep 1; : > \"$D/authenticated\"; exit 0 ;;\n"
                                            "    shared-unreachable) printf 'debug1: Connecting to build.example.org [198.51.100.4] port 22.\\nssh: connect to host build.example.org port 22: Operation timed out\\n' >&2; exit 255 ;;\n"
                                            "    shared-hanging) exec sleep 30 ;;\n"
                                            "    esac\n"
                                            "    exit 255 ;;\n"
                                            "esac\n"
                                            "case \"$ROUTE\" in\n"
                                            "direct) printf 'debug1: Reading configuration data /etc/ssh/ssh_config\\ndebug1: Connecting to build.example.org [2001:db8::7] port 22.\\ndebug1: connect to address 2001:db8::7 port 22: No route to host\\ndebug1: Connecting to build.example.org [192.0.2.1] port 22.\\ndebug1: Connection established.\\ndebug1: kex_exchange_identification: banner line 0: hello\\n' >&2 ;;\n"
                                            "shared|shared-unreachable|shared-hanging) printf \"debug1: auto-mux: Trying existing master at '$D/master'\\n\" >&2 ;;\n"
                                            "proxied) printf 'debug1: Setting implicit ProxyCommand from ProxyJump: ssh -W [%%h]:%%p bastion\\ndebug1: Executing proxy command: exec ssh -W [inner]:22 bastion\\n' >&2 ;;\n"
                                            "silent) ;;\n"
                                            "gone) printf 'debug1: Connecting to build.example.org [192.0.2.1] port 22.\\ndebug1: Connection established.\\n' >&2; printf 'UBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\\n'; exit 1 ;;\n"
                                            "no-daemon) printf 'debug1: Connecting to build.example.org [192.0.2.1] port 22.\\ndebug1: Connection established.\\nsh: ubiquitty-host: command not found\\n' >&2; exit 127 ;;\n"
                                            "unreachable) printf 'debug1: Connecting to build.example.org [192.0.2.1] port 22.\\ndebug1: connect to address 192.0.2.1 port 22: Operation timed out\\nssh: connect to host build.example.org port 22: Operation timed out\\n' >&2; exit 255 ;;\n"
                                            "esac\n"
                                            "printf '"
                                         << line << "\\n'\n";
        std::ofstream(directory / "host") << "#!/bin/sh\nprintf '" << line << "\\n'\n";
        std::ofstream(directory / "master") << "a control socket";
        ::chmod((directory / "ssh").c_str(), 0755);
        ::chmod((directory / "host").c_str(), 0755);
        savedPath = std::getenv("PATH") ? std::getenv("PATH") : "";
        ::setenv("PATH", (directory.string() + ":" + savedPath).c_str(), 1);
    }

    ~StandIn()
    {
        ::setenv("PATH", savedPath.c_str(), 1);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    /// Runs the plan's bootstrap for `alice@build.example.org` over a route.
    ///
    /// - Parameters:
    ///   - route: What the stand-in plays.
    ///   - sharedConnections: What is known of shared connections, or null.
    QUICBootstrapRun run(const char* route, SharedConnectionReaches* sharedConnections = nullptr)
    {
        std::ofstream(directory / "route") << route;
        std::error_code ignored;
        std::filesystem::remove(directory / "probes", ignored);
        std::filesystem::remove(directory / "runs", ignored);
        std::filesystem::remove(directory / "authenticated", ignored);
        std::filesystem::remove(directory / "probe.pid", ignored);
        const RemoteTransportPlan plan = remoteTransportPlan("alice@build.example.org");
        const std::unique_ptr<Ubiquitty::PAL::Waker> waker = Ubiquitty::PAL::makeWaker();
        CHECK(waker != nullptr);
        if (!waker)
            return {};
        return runQUICBootstrap(plan.quicBootstrapCommand, plan.quicHost, sharedConnections, std::stop_token {}, *waker);
    }

    /// The lines a file in the directory holds.
    int linesOf(const char* name) const
    {
        std::ifstream file(directory / name);
        int lines = 0;
        for (std::string line; std::getline(file, line);)
            ++lines;
        return lines;
    }
};

} // namespace

// ssh tried one address, then connected to another: the carrier dials the one
// it connected to, with the port the host's line names, and no probe is run.
TEST(theCarrierDialsTheAddressSSHReached)
{
    StandIn standIn;
    const QUICBootstrapRun run = standIn.run("direct");
    CHECK(run.read == QUICBootstrapRead::Answered);
    CHECK_STR_EQ(run.info.reachedAddress, std::string("192.0.2.1"));
    CHECK_EQ(run.info.port, 4433);
    CHECK_EQ(run.info.token.size(), 32u);
    CHECK_EQ(run.info.certificateSHA256.size(), 32u);
    CHECK(run.refusal.empty());
    CHECK_EQ(standIn.linesOf("probes"), 0);
    CHECK_EQ(standIn.linesOf("runs"), 1);
}

// The run went over a connection another ssh holds and named no address: one
// probe that shares nothing says where ssh connects, and the carrier dials
// that.
TEST(aSharedConnectionIsProbedForItsAddress)
{
    StandIn standIn;
    const QUICBootstrapRun run = standIn.run("shared");
    CHECK(run.read == QUICBootstrapRead::Answered);
    CHECK_STR_EQ(run.info.reachedAddress, std::string("198.51.100.4"));
    CHECK_EQ(run.info.port, 4433);
    CHECK_EQ(standIn.linesOf("probes"), 1);
    std::ifstream probes(standIn.directory / "probes");
    std::string probe;
    std::getline(probes, probe);
    // No shared connection, no prompt, and nothing run on the host.
    CHECK(probe.find("-o BatchMode=yes") != std::string::npos);
    CHECK(probe.find("-o ControlMaster=no -o ControlPath=none ") != std::string::npos);
    CHECK(probe.ends_with(" alice@build.example.org " + Ubiquitty::Transport::remoteShellCommand("true")));
    CHECK(probe.find("quic-bootstrap") == std::string::npos);
    // It knows no host key and requires one, so it could not get past the key
    // exchange to a login even if it were left running.
    CHECK(probe.find("-o StrictHostKeyChecking=yes -o UserKnownHostsFile=/dev/null -o GlobalKnownHostsFile=/dev/null -o VerifyHostKeyDNS=no ") != std::string::npos);
    // And it is not left running: it is ended once it has named its
    // connection, which the stand-in follows a second later with what a real
    // ssh would do next. What the host sees of a probe is one TCP connection
    // closed before authentication.
    ::usleep(1'500'000);
    CHECK(!std::filesystem::exists(standIn.directory / "authenticated"));
}

// One probe for a shared connection, however many sessions and redials go
// over it: the address is kept while that master lives. A master made anew at
// the same path is probed again, and one that is gone keeps nothing.
TEST(aSharedConnectionIsProbedOnce)
{
    StandIn standIn;
    SharedConnectionReaches reaches;
    const QUICBootstrapRun first = standIn.run("shared", &reaches);
    CHECK(first.read == QUICBootstrapRead::Answered);
    CHECK_EQ(standIn.linesOf("probes"), 1);
    for (int dial = 0; dial < 3; ++dial) {
        const QUICBootstrapRun again = standIn.run("shared", &reaches);
        CHECK(again.read == QUICBootstrapRead::Answered);
        CHECK_STR_EQ(again.info.reachedAddress, std::string("198.51.100.4"));
        CHECK_EQ(standIn.linesOf("probes"), 0);
    }
    // The master went and another was made at the same path.
    std::error_code error;
    const auto made = std::filesystem::last_write_time(standIn.directory / "master", error);
    REQUIRE(!error);
    std::filesystem::last_write_time(standIn.directory / "master", made + std::chrono::seconds(30), error);
    REQUIRE(!error);
    const QUICBootstrapRun renewed = standIn.run("shared", &reaches);
    CHECK(renewed.read == QUICBootstrapRead::Answered);
    CHECK_EQ(standIn.linesOf("probes"), 1);
    CHECK(standIn.run("shared", &reaches).read == QUICBootstrapRead::Answered);
    CHECK_EQ(standIn.linesOf("probes"), 0);
    // Nothing is at the path ssh named: what a probe finds is used and not
    // kept, since nothing says which master it was.
    std::filesystem::remove(standIn.directory / "master", error);
    for (int dial = 0; dial < 2; ++dial) {
        CHECK(standIn.run("shared", &reaches).read == QUICBootstrapRead::Answered);
        CHECK_EQ(standIn.linesOf("probes"), 1);
    }
    // Without anything to keep it in, every run probes.
    std::ofstream(standIn.directory / "master") << "a control socket";
    CHECK(standIn.run("shared").read == QUICBootstrapRead::Answered);
    CHECK(standIn.run("shared").read == QUICBootstrapRead::Answered);
    CHECK_EQ(standIn.linesOf("probes"), 1);
}

// A probe that hangs costs the session its grace and no more: the host's
// line was in hand, no carrier is offered, the session goes on over ssh, and
// the probe is ended.
TEST(aProbeThatHangsDoesNotHoldTheSession)
{
    StandIn standIn;
    SharedConnectionReaches reaches;
    const auto began = std::chrono::steady_clock::now();
    const QUICBootstrapRun run = standIn.run("shared-hanging", &reaches);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    CHECK(run.read == QUICBootstrapRead::Declined);
    CHECK_STR_EQ(run.refusal, std::string(sshReachRefusal(SSHReachKind::Multiplexed)));
    CHECK(run.info.reachedAddress.empty());
    CHECK_MSG(took >= 1'000 * reachProbeGraceSeconds - 200 && took < 1'000 * reachProbeGraceSeconds + 3'000, ("the run took " + std::to_string(took) + " ms").c_str());
    std::ifstream pidFile(standIn.directory / "probe.pid");
    int probePID = 0;
    pidFile >> probePID;
    REQUIRE(probePID > 0);
    bool isGone = false;
    for (int tries = 0; tries < 300 && !isGone; ++tries) {
        isGone = ::kill(probePID, 0) != 0;
        if (!isGone)
            ::usleep(10'000);
    }
    CHECK_MSG(isGone, "the probe was left running");
    // Nothing was learned, so nothing was kept: the next run probes again.
    CHECK(standIn.run("shared", &reaches).read == QUICBootstrapRead::Answered);
    CHECK_EQ(standIn.linesOf("probes"), 1);
}

// The probe cannot say either: no carrier is offered, and the reason says
// which route it was.
TEST(aSharedConnectionThatCannotBeProbedOffersNoCarrier)
{
    StandIn standIn;
    const QUICBootstrapRun run = standIn.run("shared-unreachable");
    CHECK(run.read == QUICBootstrapRead::Declined);
    CHECK(run.info.reachedAddress.empty());
    CHECK(run.info.token.empty());
    CHECK_STR_EQ(run.refusal, std::string(sshReachRefusal(SSHReachKind::Multiplexed)));
    CHECK_EQ(standIn.linesOf("probes"), 1);
}

// Through a proxy, or with a transcript that names no connection, no carrier
// is offered though the host printed its line, each with its own reason, and
// nothing is probed.
TEST(aRouteThatNamesNoAddressOffersNoCarrier)
{
    StandIn standIn;
    const QUICBootstrapRun proxied = standIn.run("proxied");
    CHECK(proxied.read == QUICBootstrapRead::Declined);
    CHECK(proxied.info.reachedAddress.empty());
    CHECK(proxied.info.token.empty());
    CHECK_STR_EQ(proxied.refusal, std::string(sshReachRefusal(SSHReachKind::Proxied)));
    CHECK_EQ(standIn.linesOf("probes"), 0);
    const QUICBootstrapRun silent = standIn.run("silent");
    CHECK(silent.read == QUICBootstrapRead::Declined);
    CHECK(silent.info.reachedAddress.empty());
    CHECK_STR_EQ(silent.refusal, std::string(sshReachRefusal(SSHReachKind::Unrecognised)));
    CHECK_EQ(standIn.linesOf("probes"), 0);
}

// A bootstrap pinned to a daemon that stopped says so in place of its line.
// That is neither a decline the stream could answer nor a miss worth asking
// again: no carrier is offered, and the outcome names why.
TEST(aBootstrapPinnedToAStoppedDaemonSaysSoAndOffersNoCarrier)
{
    StandIn standIn;
    const QUICBootstrapRun gone = standIn.run("gone");
    CHECK(gone.read == QUICBootstrapRead::InstanceGone);
    CHECK(!gone.refusal.empty());
    CHECK(gone.info.reachedAddress.empty());
    CHECK(reportsInstanceGone("banner\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\r\n"));
    CHECK(!reportsInstanceGone("UBIQUITTY-INSTANCE-GONE ../host.sock\n"));
    CHECK(!reportsInstanceGone("UBIQUITTY-INSTANCE-GONE\n"));
    CHECK(!reportsInstanceGone("said UBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\n"));
}

// A host that was reached and printed no line declined; one that was never
// reached declined nothing. The transcript's own words mid-line change
// neither.
TEST(aMissIsStillReadFromTheNoisierTranscript)
{
    StandIn standIn;
    const QUICBootstrapRun noDaemon = standIn.run("no-daemon");
    CHECK(noDaemon.read == QUICBootstrapRead::Declined);
    CHECK(!noDaemon.refusal.empty());
    CHECK_EQ(standIn.linesOf("probes"), 0);
    const QUICBootstrapRun unreachable = standIn.run("unreachable");
    CHECK(unreachable.read == QUICBootstrapRead::Unreached);
    CHECK(!unreachable.refusal.empty());
    CHECK(unreachable.refusal != noDaemon.refusal);
    CHECK(unreachable.info.reachedAddress.empty());
}

// The local daemon is reached at the loopback address as it stands: the plan
// runs no ssh, and nothing is read from a transcript.
TEST(theLocalDaemonIsDialledAtLoopback)
{
    StandIn standIn;
    std::ofstream(standIn.directory / "route") << "direct";
    const RemoteTransportPlan plan = localTransportPlan("'" + (standIn.directory / "host").string() + "'");
    const std::unique_ptr<Ubiquitty::PAL::Waker> waker = Ubiquitty::PAL::makeWaker();
    REQUIRE(waker != nullptr);
    const QUICBootstrapRun run = runQUICBootstrap(plan.quicBootstrapCommand, plan.quicHost, nullptr, std::stop_token {}, *waker);
    CHECK(run.read == QUICBootstrapRead::Answered);
    CHECK_STR_EQ(run.info.reachedAddress, std::string("127.0.0.1"));
    CHECK_EQ(run.info.port, 4433);
    CHECK_EQ(standIn.linesOf("runs"), 0);
    CHECK(isLoopbackAddress("::1"));
    CHECK(!isLoopbackAddress("127.0.0.2"));
    CHECK(!isLoopbackAddress("localhost"));
}

// A command that cannot be run, or a run that is stopped, offers no carrier.
TEST(aRunThatIsStoppedOffersNoCarrier)
{
    StandIn standIn;
    std::ofstream(standIn.directory / "route") << "direct";
    const RemoteTransportPlan plan = remoteTransportPlan("alice@build.example.org");
    const std::unique_ptr<Ubiquitty::PAL::Waker> waker = Ubiquitty::PAL::makeWaker();
    REQUIRE(waker != nullptr);
    std::stop_source source;
    source.request_stop();
    const QUICBootstrapRun run = runQUICBootstrap(plan.quicBootstrapCommand, plan.quicHost, nullptr, source.get_token(), *waker);
    CHECK(run.read != QUICBootstrapRead::Answered);
    CHECK(run.info.reachedAddress.empty());
    CHECK(run.info.token.empty());
}

UBIQUITTY_TEST_MAIN()
