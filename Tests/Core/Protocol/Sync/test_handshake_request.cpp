// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandshakeRequest.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Harness.h"

#include <cstdint>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

TEST(aHandshakeRequestPreservesEveryAdmissionField)
{
    HandshakeRequest request;
    request.target = HandshakeTarget::Session;
    request.columns = 137;
    request.rows = 43;
    request.sessionID = 91;
    request.intents = { HelloIntent::AttachesWorkspace, HelloIntent::ServesClipboard };
    request.agentLabel = "helper";
    request.authToken = "test-token";
    request.workspaceName = "build";
    request.sessionFlags = helloSessionAttachOnly | helloSessionEndWhenDetached;
    request.workspaceID = 23;
    request.expectedDaemonInstanceID = 0x1020304050607080ULL;

    namespace Sync = Ubiquitty::Protocol::Sync;
    SyncClientCore core(nullptr, nullptr);
    core.setClock([] { return 1; });
    core.setClientInstance({ 3, 4 });
    std::vector<std::uint8_t> actualWire { 0xAA };
    core.buildHello(request, actualWire);
    REQUIRE(!actualWire.empty() && actualWire.front() == 0xAA);
    Sync::FrameDecoder decoder;
    decoder.append(std::span(actualWire).subspan(1));
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet);
    const auto greeting = Sync::decodeGreet(frame->body);
    REQUIRE(greeting);
    CHECK(greeting->facts.viewportColumns == 137 && greeting->facts.viewportRows == 43);
    CHECK(greeting->request.target == Sync::AdmissionTarget::Workspace);
    CHECK(greeting->request.sessionID == 91 && greeting->request.workspaceID == 23);
    CHECK(greeting->request.workspaceName == "build");
    CHECK(greeting->request.expectedDaemonInstanceID == request.expectedDaemonInstanceID);
    CHECK(greeting->request.isAttachOnly && greeting->request.endsWhenDetached && greeting->request.servesClipboard);
    CHECK(greeting->request.agentLabel == "helper" && greeting->request.sshHost.empty());
    const std::optional credential { std::vector<std::uint8_t> { 't', 'e', 's', 't', '-', 't', 'o', 'k', 'e', 'n' } };
    CHECK(greeting->credential == credential);
    CHECK(!decoder.next());

    HandshakeRequest fresh;
    fresh.sshHost = "example.invalid";
    actualWire.clear();
    core.buildHello(fresh, actualWire);
    Sync::FrameDecoder freshDecoder;
    freshDecoder.append(actualWire);
    const auto freshFrame = freshDecoder.next();
    REQUIRE(freshFrame && freshFrame->kind == Sync::FrameKind::Greet);
    const auto remote = Sync::decodeGreet(freshFrame->body);
    REQUIRE(remote);
    CHECK(remote->request.sshHost == "example.invalid");
    CHECK(remote->request.sessionID == 0 && remote->request.workspaceID == 0);

    HandshakeRequest control;
    control.target = HandshakeTarget::Control;
    control.intents = { HelloIntent::RequestsManagement };
    actualWire.clear();
    core.buildHello(control, actualWire);
    Sync::FrameDecoder controlDecoder;
    controlDecoder.append(actualWire);
    const auto controlFrame = controlDecoder.next();
    REQUIRE(controlFrame && controlFrame->kind == Sync::FrameKind::Greet);
    const auto management = Sync::decodeGreet(controlFrame->body);
    REQUIRE(management);
    CHECK(management->request.target == Sync::AdmissionTarget::Management);
    CHECK(management->request.sessionID == 0 && management->request.workspaceID == 0);
    CHECK(management->request.workspaceName.empty() && management->request.sshHost.empty());
    CHECK(!management->request.isAttachOnly && !management->request.endsWhenDetached);
    CHECK(!management->resume);
    CHECK(!controlDecoder.next());
}

UBIQUITTY_TEST_MAIN()
