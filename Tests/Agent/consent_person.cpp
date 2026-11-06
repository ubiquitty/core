// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A real client of the existing trusted `ubiquitty-host --attach` bridge.
// Used only by the hermetic ubq smoke: no daemon seam, trust-list override,
// invented interactive token, or production authorization switch. Its mode
// explicitly models the person's decision for each exact question received.

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/Admission.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Session/LaunchContext.h"
#include "Host/ScreenClient.h"
#include "NotifyRead.h"
#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" char** environ;

namespace {
namespace Sync = Ubiquitty::Protocol::Sync;

std::uint64_t now()
{
    timespec value {};
    ::clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(value.tv_nsec) / 1000;
}

bool writeAll(int descriptor, std::span<const std::uint8_t> bytes)
{
    const auto deadline = now() + 5'000'000;
    while (!bytes.empty() && now() < deadline) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno != EAGAIN && errno != EINTR)
            return false;
        pollfd writable { descriptor, POLLOUT, 0 };
        (void)::poll(&writable, 1, 20);
    }
    return bytes.empty();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: consent_person HOST SESSION allow|deny|viewer|late\n");
        return 2;
    }
    std::uint32_t session = 0;
    const std::string_view number(argv[2]);
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), session);
    const std::string_view mode(argv[3]);
    if (parsed.ec != std::errc {} || parsed.ptr != number.data() + number.size() || !session || (mode != "allow" && mode != "deny" && mode != "viewer" && mode != "late"))
        return 2;
    int toBridge[2];
    int fromBridge[2];
    if (::pipe(toBridge) != 0)
        return 1;
    if (::pipe(fromBridge) != 0) {
        ::close(toBridge[0]);
        ::close(toBridge[1]);
        return 1;
    }
    // A local attach: a bridge that inherited an SSH session's variables would
    // mark the person as relayed from SSH.
    std::vector<char*> environment;
    for (char** entry = environ; *entry; ++entry) {
        const std::string_view text(*entry);
        if (!Ubiquitty::Session::isCredentialEnvironmentEntry(*entry) && !text.starts_with("SSH_CONNECTION=") && !text.starts_with("SSH_CLIENT="))
            environment.push_back(*entry);
    }
    environment.push_back(nullptr);
    const pid_t bridge = ::fork();
    if (bridge == 0) {
        (void)::dup2(toBridge[0], STDIN_FILENO);
        (void)::dup2(fromBridge[1], STDOUT_FILENO);
        for (int descriptor : { toBridge[0], toBridge[1], fromBridge[0], fromBridge[1] })
            ::close(descriptor);
        char attach[] = "--attach";
        std::array<char*, 3> arguments { argv[1], attach, nullptr };
        ::execve(argv[1], arguments.data(), environment.data());
        ::_exit(127);
    }
    ::close(toBridge[0]);
    ::close(fromBridge[1]);
    Ubiquitty::PAL::UnixFileDescriptor input(toBridge[1]);
    Ubiquitty::PAL::UnixFileDescriptor output(fromBridge[0]);
    if (bridge < 0)
        return 1;
    ::signal(SIGPIPE, SIG_IGN);
    sigset_t stopping;
    sigemptyset(&stopping);
    sigaddset(&stopping, SIGTERM);
    sigaddset(&stopping, SIGINT);
    (void)::sigprocmask(SIG_BLOCK, &stopping, nullptr);
    (void)::fcntl(input.get(), F_SETFL, ::fcntl(input.get(), F_GETFL, 0) | O_NONBLOCK);
    (void)::fcntl(output.get(), F_SETFL, ::fcntl(output.get(), F_GETFL, 0) | O_NONBLOCK);
    const auto role = mode == "viewer" ? Sync::Role::Viewer : Sync::Role::Owner;
    // `late` allows, as a person does who reads the question first: past the
    // ten seconds ubq waits for an answer from the host itself.
    const bool isLate = mode == "late";
    constexpr std::uint64_t lateAnswerMicroseconds = 12'000'000;
    std::vector<std::pair<std::uint64_t, std::string>> heldAnswers;
    UbiquittyTest::ScreenClient client(24, 80, 0, Sync::LinkCost::Unmetered, role);
    // Fresh, unpinned admission through the spawned trusted bridge. The seed
    // session selects its existing workspace; this is not protected continuity.
    bool isAlive = client.start({ .target = Sync::AdmissionTarget::Workspace,
        .sessionID = session,
        .workspaceID = 0,
        .expectedDaemonInstanceID = 0,
        .isAttachOnly = true });
    const auto wire = client.takeUpstream(now());
    Sync::EnvelopeError error {};
    const auto frame = Sync::decodeDatagram(wire, error);
    const auto greet = frame && frame->kind == Sync::FrameKind::Greet && !frame->channel ? Sync::decodeGreet(frame->body) : std::nullopt;
    isAlive = isAlive && greet && greet->request.target == Sync::AdmissionTarget::Workspace
        && greet->request.sessionID == session && !greet->request.workspaceID
        && !greet->request.expectedDaemonInstanceID && greet->request.isAttachOnly
        && !greet->credential && !greet->resume && greet->clientKind == Sync::ClientKind::Human
        && greet->requestedRole == role;
    if (isAlive)
        isAlive = writeAll(input.get(), wire);
    else
        std::fprintf(stderr, "consent_person: the exact workspace greeting could not be encoded\n");
    bool isReady = false;
    // A loaded host binds the pane well after the Welcome; the smoke's own
    // wait for READY allows the same.
    const auto attachDeadline = now() + 30'000'000;
    int result = isAlive ? 0 : 1;
    while (isAlive) {
        sigset_t pending;
        (void)::sigpending(&pending);
        if (sigismember(&pending, SIGTERM) || sigismember(&pending, SIGINT))
            break;
        if (!writeAll(input.get(), client.takeUpstream(now()))) {
            result = 1;
            break;
        }
        pollfd readable { output.get(), POLLIN, 0 };
        if (::poll(&readable, 1, 20) > 0) {
            std::array<std::uint8_t, 65536> bytes;
            const ssize_t got = ::read(output.get(), bytes.data(), bytes.size());
            if (got <= 0) {
                result = got == 0 ? 0 : 1;
                break;
            }
            const auto received = std::span(bytes).first(static_cast<std::size_t>(got));
            client.read(received, now());
        }
        for (const auto& text : client.takeNotifies()) {
            Ubiquitty::Protocol::Control::ConsentResponse response;
            if (const auto question = UbiquittyTest::consentQuestionOf(text)) {
                response.requestID = question->requestID;
                response.kind = static_cast<Sync::ConsentKind>(question->action);
                response.decision = mode == "allow" || isLate ? Sync::ConsentChoice::AllowSession : Sync::ConsentChoice::Deny;
            } else if (const auto structure = UbiquittyTest::structureConsentOf(text)) {
                response.requestID = structure->requestID;
                response.kind = Sync::ConsentKind::Structure;
                response.decision = mode == "allow" || isLate ? Sync::ConsentChoice::AllowOnce : Sync::ConsentChoice::Deny;
            } else {
                continue;
            }
            const auto request = Ubiquitty::Protocol::Control::consentResponseRequest(response, "person:" + std::to_string(response.requestID));
            if (isLate && !request.empty()) {
                heldAnswers.emplace_back(now() + lateAnswerMicroseconds, request);
                continue;
            }
            if (request.empty() || !client.call(request)) {
                result = 1;
                isAlive = false;
            }
        }
        while (!heldAnswers.empty() && heldAnswers.front().first <= now()) {
            if (!client.call(heldAnswers.front().second)) {
                result = 1;
                isAlive = false;
            }
            heldAnswers.erase(heldAnswers.begin());
        }
        (void)client.takeResults();
        if (!isReady && client.connection().state() == Sync::ConnectionState::Attached) {
            const auto welcome = client.welcome();
            if (!welcome || welcome->selection.target != Sync::AdmissionTarget::Workspace
                || welcome->selection.sessionID != session || !welcome->selection.workspaceID) {
                std::fprintf(stderr, "consent_person: the host did not admit the requested workspace seed\n");
                result = 1;
                break;
            }
            // A person may approve only once the host has bound it a pane of
            // the workspace; the Welcome comes first, and on a loaded host a
            // question asked between the two finds no person attached.
            if (client.connection().handleForPane({ .low = session, .high = welcome->selection.daemonInstanceID })) {
                isReady = true;
                std::puts("READY");
                std::fflush(stdout);
            }
        }
        if (!isReady && now() >= attachDeadline) {
            std::fprintf(stderr, "consent_person: attached person did not become ready\n");
            result = 1;
            break;
        }
    }
    input.reset();
    output.reset();
    int status = 0;
    (void)::waitpid(bridge, &status, 0);
    return result;
}
