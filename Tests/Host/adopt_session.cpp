// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Complete local adoption, bidirectional input, and the existing established-
// attachment reclaim assertions. The latter remain quarantined until complete
// reclaim-state transfer is implemented; this fixture does not certify it.

#include "Core/Protocol/Control/Reclaim.h"
#include "LocalAdoptionFixture.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty::HostTest;

bool fail(const char* message)
{
    std::fprintf(stderr, "adopt_session: FAIL — %s\n", message);
    return false;
}

// Reclaim uses the same authenticated live attachment, not a fresh socket or
// fabricated credential. A carrier closing is never an exact success result.
bool reclaimExistingAttachment(Connection& connection, const Ubiquitty::Protocol::Control::ReclaimTarget& target)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    namespace Control = Ubiquitty::Protocol::Control;
    if (!target.handle || !target.sessionID || !target.daemonInstanceID || target.hostInstanceID == Sync::ID128 {}
        || target.attachmentID == Sync::ID128 {} || target.paneID == Sync::ID128 {}
        || !connection.welcome || connection.welcome->selection.sessionID != target.sessionID
        || connection.welcome->selection.daemonInstanceID != target.daemonInstanceID
        || connection.welcome->hostInstanceID != target.hostInstanceID || connection.welcome->attachmentID != target.attachmentID
        || connection.panes.size() != 1 || connection.panes.front().handle != target.handle || connection.panes.front().paneID != target.paneID)
        return false;
    constexpr std::string_view correlation = "adopt-fixture.reclaim";
    Sync::Call call;
    call.text = Control::reclaimRequest({ .handle = target.handle, .daemonInstanceID = target.daemonInstanceID }, correlation);
    std::vector<std::uint8_t> body;
    Sync::encode(body, call);
    if (!sendFrame(connection, Sync::FrameKind::Call, { reinterpret_cast<const char*>(body.data()), body.size() }))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string received;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (!waitFor(connection, Sync::FrameKind::Result, received, static_cast<int>(left)))
            return false;
        const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(received.data()), received.size() });
        if (!result || result->transfer)
            return false;
        bool isJSON = false;
        const auto answer = Ubiquitty::Codec::JSON::parse(result->text, isJSON);
        if (isJSON && answer.stringOr("id") == correlation)
            return Control::isReclaimReleasedResult(answer, correlation, target);
    }
    return false;
}

bool waitForExactUnbind(Connection& connection, std::uint32_t handle)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    std::string received;
    if (!waitFor(connection, Sync::FrameKind::PaneUnbind, received, 3000))
        return false;
    const auto unbind = Sync::decodePaneUnbind({ reinterpret_cast<const std::uint8_t*>(received.data()), received.size() });
    return unbind && unbind->handle == handle;
}

Ubiquitty::Protocol::Sync::Greet existingGreeting(std::uint32_t sessionID, std::uint64_t daemonInstanceID, std::uint64_t instance)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = "adopt-fixture";
    greeting.clientInstance = { 0xAD0F7, instance };
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.sessionID = sessionID;
    greeting.request.expectedDaemonInstanceID = daemonInstanceID;
    greeting.request.isAttachOnly = true;
    return greeting;
}

std::optional<Ubiquitty::Protocol::Sync::ServingSelection> controlSelection(Connection& control, LocalAdoptionFixture& fixture)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto greeting = existingGreeting(0, 0, 1);
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.request.isAttachOnly = false;
    std::string body;
    if (!sendGreeting(control, greeting) || !waitFor(control, Sync::FrameKind::Welcome, body, 3000) || !control.welcome)
        return std::nullopt;
    const auto& welcome = *control.welcome;
    if (welcome.admission != Sync::AttachmentAdmission::Granted || welcome.selection.target != Sync::AdmissionTarget::Control
        || welcome.selection.sessionID || welcome.selection.workspaceID || !welcome.selection.daemonInstanceID
        || welcome.selection.socketName != fixture.socketLeaf() || !control.panes.empty() || control.hasFailed)
        return std::nullopt;
    return welcome.selection;
}

} // namespace

int main()
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    char directory[] = "/tmp/ubiquitty-adopt.XXXXXX";
    const char* home = ::mkdtemp(directory);
    if (!home)
        return 1;
    LocalAdoptionFixture fixture;
    if (!fixture.start(std::string(home) + "/.ubiquitty"))
        return 1;
    auto* owner = fixture.makeOwner();
    if (!owner || !fixture.startPausedLeader(*owner) || !fixture.adopt(*owner)) {
        fail("the real owner did not receive exact Committed evidence");
        return 1;
    }
    const std::uint32_t sessionID = owner->result.adopted.sessionID;
    const auto& socketLeaf = fixture.socketLeaf();
    Ubiquitty::PAL::UnixFileDescriptor controlSocket = fixture.connectOwner();
    Ubiquitty::PAL::UnixFileDescriptor humanSocket = fixture.connectOwner();
    Ubiquitty::PAL::UnixFileDescriptor observerSocket = fixture.connectOwner();
    if (!controlSocket || !humanSocket || !observerSocket)
        return 1;
    Connection control;
    control.fileDescriptor = controlSocket.get();
    const auto selected = controlSelection(control, fixture);
    if (!selected || selected->daemonInstanceID != owner->result.adopted.daemonInstanceID) {
        fail("independent Control receipt did not match the committed daemon pin");
        return 1;
    }
    Connection agent;
    agent.fileDescriptor = humanSocket.get();
    if (!sendGreeting(agent, existingGreeting(sessionID, selected->daemonInstanceID, 2)) || !waitForScreen(agent, 3000)
        || !agent.welcome || agent.welcome->admission != Sync::AttachmentAdmission::Granted
        || agent.welcome->selection.target != Sync::AdmissionTarget::Session
        || agent.welcome->selection.sessionID != sessionID || !agent.welcome->selection.workspaceID
        || agent.welcome->selection.daemonInstanceID != selected->daemonInstanceID || agent.welcome->selection.socketName != socketLeaf
        || agent.panes.size() != 1 || !agent.panes.front().handle || agent.panes.front().paneID == Sync::ID128 {}) {
        fail("the exact native-owner attachment never received its checkpoint");
        return 1;
    }
    const Ubiquitty::Protocol::Control::ReclaimTarget target {
        .handle = agent.panes.front().handle,
        .sessionID = sessionID,
        .daemonInstanceID = selected->daemonInstanceID,
        .hostInstanceID = agent.welcome->hostInstanceID,
        .attachmentID = agent.welcome->attachmentID,
        .paneID = agent.panes.front().paneID,
    };
    if (!sendInput(agent, "probe123\r"))
        return 1;
    std::string fromSlave;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && fromSlave.find("probe123") == std::string::npos) {
        struct pollfd wait { .fd = owner->slave.get(), .events = POLLIN, .revents = 0 };
        if (::poll(&wait, 1, 50) > 0 && (wait.revents & POLLIN)) {
            std::array<char, 256> bytes {};
            const auto count = ::read(owner->slave.get(), bytes.data(), bytes.size());
            if (count > 0)
                fromSlave.append(bytes.data(), static_cast<std::size_t>(count));
        }
    }
    if (fromSlave.find("probe123") == std::string::npos) {
        fail("the exact attachment's input never reached the adopted PTY");
        return 1;
    }
    Connection observer;
    observer.fileDescriptor = observerSocket.get();
    if (!sendGreeting(observer, existingGreeting(sessionID, target.daemonInstanceID, 3)) || !waitForScreen(observer, 3000)
        || !observer.welcome || observer.welcome->hostInstanceID != target.hostInstanceID
        || observer.welcome->selection.daemonInstanceID != target.daemonInstanceID
        || observer.welcome->selection.workspaceID != agent.welcome->selection.workspaceID
        || observer.welcome->selection.sessionID != sessionID || observer.welcome->selection.socketName != socketLeaf
        || observer.panes.size() != 1 || observer.panes.front().paneID != target.paneID || !observer.panes.front().handle) {
        fail("the observer never received its exact pane binding");
        return 1;
    }
    const auto observerHandle = observer.panes.front().handle;
    if (!reclaimExistingAttachment(agent, target) || !waitForExactUnbind(observer, observerHandle)) {
        fail("the existing requester/observer lacked exact reclaim Result and PaneUnbind");
        return 1;
    }
    // This retains the existing thin-reclaim assertion, not a claim that its
    // terminal state now agrees with the frozen local model.
    struct winsize dimensions {};
    if (::ioctl(owner->master.get(), TIOCGWINSZ, &dimensions) != 0 || ::write(owner->master.get(), "local\r", 6) != 6) {
        fail("the retained master did not remain usable after reclaim");
        return 1;
    }
    const auto before = publishedAdoptionPaneCount(fixture.socketPath());
    int pipeEnds[2] { -1, -1 };
    if (!before || ::pipe(pipeEnds) != 0)
        return 1;
    Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeEnds[0]);
    Ubiquitty::PAL::UnixFileDescriptor writeEnd(pipeEnds[1]);
    if (!AdoptionProbe::isRejectedWithoutActivation(fixture.socketPath(), readEnd.get(), owner->frozen.view())) {
        fail("a non-PTY Prepare lacked exact retained NotOwner evidence");
        return 1;
    }
    const auto after = publishedAdoptionPaneCount(fixture.socketPath());
    if (!after || *after != *before) {
        fail("the non-PTY Prepare changed the public owner set");
        return 1;
    }
    std::printf("adopt_session: exact adoption, live input, established-attachment reclaim assertions and exact non-PTY rejection\n");
    return 0;
}
