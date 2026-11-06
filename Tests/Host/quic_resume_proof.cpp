// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/quic_resume_proof.cpp — a QUIC connection resumes an attachment
// by a proof in its preamble, with no token and no secret sent (Docs/sync.md
// §6.9), over real loopback QUIC against a spawned host.
//
// A client attaches with a bootstrap token, is sent its pane and
// acknowledges an effect of it. Its connection goes without a word. It dials
// again and leads the stream with [2][proof], the proof a MAC under the
// attachment's resume secret over keying material the new handshake
// exports: the host continues the attachment, its pane with a diff and its
// effects from the batch the client acknowledged. The same preamble bytes on
// another connection are refused, since that handshake exports other
// material. A proof for an attachment whose client left on purpose is
// refused. A refused connection carries a generic application close but no
// stream data.
//
// Usage: quic_resume_proof <path-to-ubiquitty-host>. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Screen/Addressing.h"
#include "Core/Transport/QUICClient.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Host/ScreenClient.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;

namespace {

using UbiquittyTest::Dial;

constexpr std::string_view shellPrompt = "quic-resume-ready> ";

/// What a client holds of its pane, as a resume on a proof connection names
/// it: its picture and the last effect batch it acknowledged, as the
/// production client names them. No secret travels.
Sync::ResumeRequest resumeOf(Sync::ReplicaSet& replicas, Sync::ID128 paneID, const Sync::Welcome& welcome)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = std::nullopt, .panes = {} };
    if (Sync::ReplicaSet::Held* held = replicas.find(paneID))
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live, .effectsAcknowledged = replicas.effectReceipt(paneID).acknowledged });
    return request;
}

/// The credential a client resumes `welcome`'s attachment with: the proof for
/// whichever connection builds it. What it sent is kept for a replay.
Transport::QUICCredential proofFor(const Sync::Welcome& welcome, std::vector<std::uint8_t>& sent, std::mutex& sentLock)
{
    return Transport::ExportedPreamble { .exporterLabel = std::string(Sync::resumeExporterLabel), .build = [welcome, &sent, &sentLock](std::span<const std::uint8_t, 32> exporter) {
                                            std::vector<std::uint8_t> preamble = Sync::resumePreamble(welcome.resumeSecret, exporter, welcome.attachmentID, welcome.hostInstanceID);
                                            const std::scoped_lock held(sentLock);
                                            sent = preamble;
                                            return preamble;
                                        } };
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-quic-resume.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_resume_proof: mkdtemp failed\n");
        return 1;
    }
    {
        std::ofstream profile(std::string(home) + "/.profile");
        profile << "PS1='" << shellPrompt << "'\n";
        profile.close();
        if (!profile) {
            std::fprintf(stderr, "quic_resume_proof: could not write the shell profile\n");
            return 1;
        }
    }
    const std::string controlDirectory = std::string(home) + "/.ubiquitty";
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    const std::string portArgument = std::to_string(port);

    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", controlDirectory.c_str(), 1);
        // The pane's shell has a fixture-owned home and prompt, with no
        // user startup files or shell integration producing effects.
        ::setenv("HOME", home, 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        ::execl(hostBinary, hostBinary, "--daemon", "--quic-port", portArgument.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const auto stopDaemon = [&] {
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
    };
    if (!Ubiquitty::HostTest::waitForDaemonReady(controlDirectory + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary))) {
        std::fprintf(stderr, "quic_resume_proof: premise: the daemon never accepted a connection\n");
        stopDaemon();
        return 1;
    }
    std::string token;
    std::string pin;
    std::uint16_t mintedPort = 0;
    if (!mintQUICTokenViaBootstrap(hostBinary, controlDirectory, token, mintedPort, pin) || pin.size() != 32) {
        std::fprintf(stderr, "quic_resume_proof: premise: no QUIC token or certificate pin was minted\n");
        stopDaemon();
        return 1;
    }

    Sync::ReplicaSet replicas;
    std::optional<Sync::Welcome> held;
    Sync::ID128 paneID {};
    {
        // Attach with the token, and take the pane.
        Dial first(replicas, std::nullopt);
        CHECK_MSG(first.start(port, Transport::BootstrapToken { .token = token }, pin, { .target = Sync::AdmissionTarget::Session }), "premise: the token dial did not start");
        const bool wasSentThePane = first.pumpUntil(10'000, [](const UbiquittyTest::ScreenClient& client) { return client.connection().welcome().has_value() && client.pane() && client.generation() > 0; });
        CHECK_MSG(wasSentThePane, "premise: the client that attached with a token was not sent its pane");
        // The bell's picture follows the shell's initial prompt and echo
        // context, so startup does not force that update onto the stream.
        const bool didReachPrompt = first.pumpUntil(10'000, [](const UbiquittyTest::ScreenClient& client) {
            const auto* terminal = client.pane();
            if (!terminal || terminal->columns < static_cast<int>(shellPrompt.size()) || !client.connection().echoContext(1))
                return false;
            for (int row = 0; row < terminal->rows; ++row) {
                bool hasPrompt = true;
                for (std::size_t column = 0; column < shellPrompt.size(); ++column) {
                    const auto* cell = Ubiquitty::Screen::visibleCell(terminal, 0, row, static_cast<int>(column));
                    if (!cell || cell->codePoint != static_cast<unsigned char>(shellPrompt[column])) {
                        hasPrompt = false;
                        break;
                    }
                }
                if (hasPrompt)
                    return true;
            }
            return false;
        });
        CHECK_MSG(didReachPrompt, "premise: the pane's shell did not publish its ready prompt and echo context");
        // The pane rings while the client is attached, and the client
        // acknowledges the effect. The attachment's effect cursor is then
        // past zero, and a resume has to name it: one that names less than
        // its client acknowledged is refused after its Welcome. The shell
        // emits no effect of its own, so the pane is made to ring.
        std::optional<std::uint64_t> rang;
        {
            const std::string ring = "printf '\\a'\r";
            const std::scoped_lock lock(first.lock);
            rang = first.client.sendRaw({ reinterpret_cast<const std::uint8_t*>(ring.data()), ring.size() });
        }
        CHECK_MSG(rang.has_value(), "premise: the attached client could not type");
        const bool didAcknowledgeAnEffect = first.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient& client) {
            const auto bound = client.connection().boundPanes();
            return !bound.empty() && replicas.effectReceipt(bound.front().second).acknowledged > 0;
        });
        CHECK_MSG(didAcknowledgeAnEffect, "premise: the attached client acknowledged no effect of its pane ringing");
        // Let the acknowledgement of what it holds reach the host.
        (void)first.pumpUntil(300, [](const UbiquittyTest::ScreenClient&) { return false; });
        {
            const std::scoped_lock lock(first.lock);
            held = first.client.connection().welcome();
            if (!first.client.connection().boundPanes().empty())
                paneID = first.client.connection().boundPanes().front().second;
            first.client.storeReplicas();
        }
        // The transport goes without a word: the attachment is Lost, not left.
        first.carrier.stop();
    }
    if (!held) {
        stopDaemon();
        return UBIQUITTY_TEST_SUMMARY();
    }
    CHECK_MSG(held->selection.target == Sync::AdmissionTarget::Session && held->selection.sessionID != 0, "premise: the original dial was not admitted to its session");
    // The admitted seed is retained attachment context, not fresh discovery of
    // an expected daemon. The original bootstrap dial had no incarnation pin.
    const Sync::AdmissionRequest resumeTarget { .target = Sync::AdmissionTarget::Session,
        .sessionID = held->selection.sessionID,
        .expectedDaemonInstanceID = 0,
        .isAttachOnly = true };
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::vector<std::uint8_t> sentPreamble;
    std::mutex sentLock;
    // A connection the proof admits never greets, as a dial left behind when
    // the network under it changed. The host has not seen it go; the next
    // proof takes its place all the same, and the attachment is not held for
    // a Greet that will not come. The host frees the connection left behind
    // without a word, as it drops any, so nothing here can see that.
    Sync::ReplicaSet unusedReplicas;
    std::vector<std::uint8_t> abandonedPreamble;
    Dial abandoned(unusedReplicas, std::nullopt);
    CHECK_MSG(abandoned.start(port, proofFor(*held, abandonedPreamble, sentLock), pin, resumeTarget, Dial::Speaks::Nothing), "premise: the abandoned dial did not start");
    const bool didSendItsProof = abandoned.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient&) {
        const std::scoped_lock lock(sentLock);
        return !abandonedPreamble.empty();
    });
    CHECK_MSG(didSendItsProof, "premise: the abandoned dial never sent its proof");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // A preamble that cannot be built fails the dial before a byte leaves.
    {
        Sync::ReplicaSet noReplicas;
        Dial unbuilt(noReplicas, std::nullopt);
        const Transport::QUICCredential nothing = Transport::ExportedPreamble { .exporterLabel = std::string(Sync::resumeExporterLabel), .build = [](std::span<const std::uint8_t, 32>) { return std::vector<std::uint8_t> {}; } };
        CHECK_MSG(unbuilt.start(port, nothing, pin, resumeTarget), "premise: the dial with no preamble to build did not start");
        const bool didFail = unbuilt.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient&) { return unbuilt.hasClosed.load(); });
        CHECK_MSG(didFail && !unbuilt.hasConnected.load(), "a dial whose preamble could not be built connected, or never failed");
        unbuilt.carrier.stop();
    }

    // Dial again with the proof: no token, and no secret in the Greet.
    Dial second(replicas, resumeOf(replicas, paneID, *held));
    CHECK_MSG(second.start(port, proofFor(*held, sentPreamble, sentLock), pin, resumeTarget), "the proof dial did not start");
    // The Welcome, then the pane it continued or the Bye that refused it.
    (void)second.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient& client) {
        return (client.connection().welcome().has_value() && (client.pane() || client.connection().state() != Sync::ConnectionState::Attached)) || second.hasClosed.load();
    });
    const std::optional<Sync::Welcome> resumed = second.welcome();
    CHECK_MSG(resumed && resumed->resumed.size() == 1 && resumed->resumed.at(0).outcome == Sync::ResumeOutcome::Diffed, "a connection that led with a resume proof was not continued with a diff");
    CHECK_MSG(resumed && resumed->attachmentID == held->attachmentID, "the proof did not continue the attachment it named");
    CHECK_MSG(resumed && resumed->selection.target == held->selection.target
            && resumed->selection.sessionID == held->selection.sessionID
            && resumed->selection.workspaceID == held->selection.workspaceID
            && resumed->selection.daemonInstanceID == held->selection.daemonInstanceID,
        "the proof changed the attachment's retained serving selection");
    CHECK_MSG(!second.hasClosed.load(), "the connection the proof admitted was closed");
    {
        const std::scoped_lock lock(second.lock);
        CHECK_MSG(second.client.connection().state() == Sync::ConnectionState::Attached && second.client.pane(), "the host said Bye to the connection a proof admitted, or never bound the pane it continued");
    }
    abandoned.carrier.stop();
    {
        const std::scoped_lock lock(sentLock);
        CHECK_MSG(sentPreamble.size() == 1 + Sync::resumeProofBytes && sentPreamble.front() == std::to_underlying(Sync::PreambleKind::Resume), "premise: the proof dial sent no resume preamble");
    }

    // The connection is served under what the token that made the attachment
    // was granted: it may drive, and its input is written.
    {
        const std::string typed = "true\r";
        std::optional<std::uint64_t> sent;
        {
            const std::scoped_lock lock(second.lock);
            sent = second.client.sendRaw({ reinterpret_cast<const std::uint8_t*>(typed.data()), typed.size() });
        }
        CHECK_MSG(sent.has_value(), "premise: the resumed client could not send input");
        const bool wasWritten = second.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient& client) { return sent && client.connection().receipt(1).inputWritten >= *sent; });
        CHECK_MSG(wasWritten, "input from the connection a proof admitted was not written: it is not served under its attachment's grant");
        CHECK_MSG(!second.hasClosed.load(), "driving closed the connection a proof admitted");
    }

    // The same preamble bytes on another connection: that handshake exports
    // other keying material, so the MAC is not its own.
    {
        std::vector<std::uint8_t> replayed;
        {
            const std::scoped_lock lock(sentLock);
            replayed = sentPreamble;
        }
        Sync::ReplicaSet otherReplicas;
        Dial replay(otherReplicas, resumeOf(otherReplicas, paneID, *held));
        const Transport::QUICCredential replayedCredential = Transport::ExportedPreamble { .exporterLabel = std::string(Sync::resumeExporterLabel), .build = [replayed](std::span<const std::uint8_t, 32>) { return replayed; } };
        CHECK_MSG(replay.start(port, replayedCredential, pin, resumeTarget), "premise: the replaying dial did not start");
        // A refused proof carries no Sync frames and names no reason that
        // distinguishes an expired attachment from a wrong MAC.
        (void)replay.pumpUntil(2'000, [&](const UbiquittyTest::ScreenClient&) { return replay.hasClosed.load(); });
        CHECK_MSG(replay.hasConnected.load(), "premise: the replaying dial never completed its handshake");
        CHECK_MSG(replay.hasClosed.load(), "the host did not close a refused proof");
        CHECK_MSG(replay.carrier.peerApplicationCloseCode() == Sync::quicCloseCodeOf(Sync::ByeReason::Malformed), "a refused proof carried no generic application close");
        CHECK_MSG(replay.bytesReceived.load() == 0, "a connection replaying another connection's proof was answered");
        CHECK_MSG(!replay.welcome().has_value(), "a replayed proof was welcomed");
        replay.carrier.stop();
    }
    // The connection the proof admitted is still served.
    CHECK_MSG(!second.hasClosed.load(), "a refused replay closed the connection that holds the attachment");

    // The client leaves on purpose: its attachment is Detached, and a proof
    // for it, the right secret and this connection's material, is refused.
    const std::optional<Sync::Welcome> last = second.welcome();
    {
        std::vector<std::uint8_t> goodbye;
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = {}, .peer = std::nullopt });
        CHECK_MSG(Sync::appendFrame(goodbye, Sync::FrameKind::Bye, 0, body), "premise: the admitted client's Normal Bye could not be encoded");
        second.carrier.send(goodbye);
        (void)second.pumpUntil(300, [](const UbiquittyTest::ScreenClient&) { return false; });
        second.carrier.stop();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (last) {
        Sync::ReplicaSet lateReplicas;
        std::vector<std::uint8_t> lateSent;
        Dial late(lateReplicas, resumeOf(lateReplicas, paneID, *last));
        CHECK_MSG(late.start(port, proofFor(*last, lateSent, sentLock), pin, resumeTarget), "premise: the late dial did not start");
        (void)late.pumpUntil(2'000, [&](const UbiquittyTest::ScreenClient&) { return late.hasClosed.load(); });
        CHECK_MSG(late.hasConnected.load(), "premise: the late dial never completed its handshake");
        CHECK_MSG(late.hasClosed.load(), "the host did not close a proof for a detached attachment");
        CHECK_MSG(late.carrier.peerApplicationCloseCode() == Sync::quicCloseCodeOf(Sync::ByeReason::Malformed), "a detached attachment's refused proof carried no generic application close");
        CHECK_MSG(late.bytesReceived.load() == 0, "a proof for an attachment whose client left was answered");
        CHECK_MSG(!late.welcome().has_value(), "a proof for a detached attachment was welcomed");
        late.carrier.stop();
    }

    stopDaemon();
    if (failures == 0)
        std::printf("quic_resume_proof: a QUIC connection resumed its attachment with a proof and a diff, a replayed proof and a proof for a detached attachment were refused\n");
    return failures == 0 ? 0 : 1;
}
