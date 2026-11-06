// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/quic_handover_resume.cpp — a QUIC client follows a whole-host
// handover on the same port and certificate (Docs/sync.md §6.13), over real
// loopback QUIC between real daemons.
//
// A client attaches to the holder with a bootstrap token. The binary at the
// holder's install path is replaced and the new one started with no QUIC port
// of its own. The holder closes the client's connection saying HandingOver
// and exits 0; the successor serves the holder's port; the client dials it
// again with a resume proof bound to the holder's instance, and the successor
// continues the attachment, naming the holder as its predecessor.
//
// Usage: quic_handover_resume <path-to-ubiquitty-host>. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Transport/QUICClient.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Host/ScreenClient.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;

namespace {

using UbiquittyTest::Dial;

pid_t startDaemon(const std::string& binary, const std::string& home, const std::string& port)
{
    const pid_t daemon = ::fork();
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        ::execl(binary.c_str(), binary.c_str(), "--daemon", "--quic-port", port.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return daemon;
}

/// Waits for one direct child, for a bounded time.
/// - Returns: Its exit status, or nullopt while it still runs.
std::optional<int> waitForExit(pid_t child, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        int status = 0;
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        ::usleep(10'000);
    }
}

bool copyFile(const std::string& from, const std::string& to, std::string_view suffix = {})
{
    {
        std::ifstream source(from, std::ios::binary);
        std::ofstream target(to, std::ios::binary | std::ios::trunc);
        if (!source || !target)
            return false;
        target << source.rdbuf();
        target.write(suffix.data(), static_cast<std::streamsize>(suffix.size()));
        if (!target)
            return false;
    }
    return ::chmod(to.c_str(), 0755) == 0;
}

/// The credential that resumes `welcome`'s attachment: a proof under its
/// secret, bound to the instance that issued it.
Transport::QUICCredential proofFor(const Sync::Welcome& welcome)
{
    return Transport::ExportedPreamble { .exporterLabel = std::string(Sync::resumeExporterLabel), .build = [welcome](std::span<const std::uint8_t, 32> exporter) {
                                            return Sync::resumePreamble(welcome.resumeSecret, exporter, welcome.attachmentID, welcome.hostInstanceID);
                                        } };
}

void run(const std::string& built)
{
    // Under /tmp, not $TMPDIR: the daemon's socket lives below this directory,
    // a Unix socket path has 103 bytes on macOS, and the per-user $TMPDIR
    // there is long enough to push it past them.
    std::string pattern = "/tmp/ubq-quic-handover.XXXXXX";
    if (!::mkdtemp(pattern.data())) {
        CHECK_MSG(false, "a private directory");
        return;
    }
    const std::string root = pattern;
    const std::string home = root + "/.ubiquitty";
    const std::string binary = root + "/bin/ubiquitty-host";
    ::mkdir(home.c_str(), 0700);
    ::mkdir((root + "/bin").c_str(), 0755);
    REQUIRE(copyFile(built, binary));
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    pid_t holder = startDaemon(binary, home, std::to_string(port));
    pid_t successor = -1;
    const auto stop = [&] {
        for (pid_t* daemon : { &holder, &successor }) {
            if (*daemon > 0) {
                ::kill(*daemon, SIGKILL);
                ::waitpid(*daemon, nullptr, 0);
                *daemon = -1;
            }
        }
    };
    if (holder <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(home + "/" + Ubiquitty::Host::socketLeafForBinary(binary.c_str()))) {
        CHECK_MSG(false, "premise: the holder listens");
        stop();
        return;
    }
    std::string token;
    std::string pin;
    std::uint16_t mintedPort = 0;
    if (!mintQUICTokenViaBootstrap(binary, home, token, mintedPort, pin) || pin.size() != 32 || mintedPort != port) {
        CHECK_MSG(false, "premise: the holder minted a QUIC token for its port");
        stop();
        return;
    }

    Sync::ReplicaSet replicas;
    std::optional<Sync::Welcome> held;
    Dial first(replicas, std::nullopt);
    CHECK_MSG(first.start(port, Transport::BootstrapToken { .token = token }, pin, { .target = Sync::AdmissionTarget::Session }), "premise: the token dial started");
    const bool wasSentThePane = first.pumpUntil(10'000, [](const UbiquittyTest::ScreenClient& client) { return client.connection().welcome().has_value() && client.pane() && client.generation() > 0; });
    CHECK_MSG(wasSentThePane, "premise: the client that attached with a token was sent its pane");
    (void)first.pumpUntil(300, [](const UbiquittyTest::ScreenClient&) { return false; });
    {
        const std::scoped_lock lock(first.lock);
        held = first.client.connection().welcome();
        first.client.storeReplicas();
    }
    if (!held) {
        stop();
        return;
    }

    // The binary at the holder's path is replaced; the new one names no port.
    const std::string staged = binary + ".next";
    CHECK_MSG(copyFile(binary, staged, "\nhandover successor\n") && ::rename(staged.c_str(), binary.c_str()) == 0, "the successor is installed in place");
    successor = startDaemon(binary, home, "-1");
    const auto holderExit = waitForExit(holder, 30'000);
    if (holderExit)
        holder = -1;
    CHECK_MSG(holderExit == std::optional { 0 }, "the holder exits cleanly after handing over");
    const bool didClose = first.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient&) { return first.hasClosed.load(); });
    const auto closeCode = first.carrier.peerApplicationCloseCode();
    CHECK_MSG(didClose && closeCode && Sync::byeReasonOfQUICClose(*closeCode) == std::optional { Sync::ByeReason::HandingOver },
        "the holder closes the QUIC connection saying it is handing over");
    first.carrier.stop();
    CHECK_MSG(Ubiquitty::HostTest::waitForDaemonReady(home + "/" + Ubiquitty::Host::socketLeafForBinary(binary.c_str())), "the successor listens");

    // The same port and pin; a proof bound to the holder's instance.
    const Sync::AdmissionRequest resumeTarget { .target = Sync::AdmissionTarget::Session, .sessionID = held->selection.sessionID, .expectedDaemonInstanceID = 0, .isAttachOnly = true };
    Dial second(replicas, Sync::ResumeRequest { .attachmentID = held->attachmentID, .resumeSecret = std::nullopt, .panes = {} });
    CHECK_MSG(second.start(port, proofFor(*held), pin, resumeTarget), "the proof dial started");
    (void)second.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient& client) { return client.connection().welcome().has_value() || second.hasClosed.load(); });
    const auto resumed = second.welcome();
    CHECK_MSG(resumed.has_value(), "the successor admits the proof on the holder's port and certificate");
    if (resumed) {
        CHECK_MSG(resumed->attachmentID == held->attachmentID, "the proof continues the attachment the holder issued");
        CHECK_MSG(resumed->hostInstanceID != held->hostInstanceID, "the successor is a new host instance");
        CHECK_MSG(resumed->predecessorHostInstanceID == std::optional { held->hostInstanceID }, "the successor names the holder as its predecessor");
        CHECK_MSG(resumed->selection.sessionID == held->selection.sessionID, "the session keeps its id");
    }
    second.carrier.stop();
    stop();
    if (!failures) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    run(argv[1]);
    return UBIQUITTY_TEST_SUMMARY();
}
