// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/DragDrop/Client.h"
#include "Core/DragDrop/Queue.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/VT/Constants.h"

#include <array>
#include <string>

using namespace Ubiquitty;
using namespace Ubiquitty::DragDrop;
using UbiquittyTest::feed;

namespace {
std::string replies(Terminal& terminal)
{
    std::array<char, 32768> bytes;
    const auto size = drainOutput(&terminal, bytes);
    return { bytes.data(), static_cast<std::size_t>(size) };
}

void drop(Terminal& terminal)
{
    feed(terminal, "\033]72;t=a:i=7;text/plain\033\\");
    Metadata location;
    location.x = 0;
    location.y = 0;
    location.pixelX = 0;
    location.pixelY = 0;
    location.operation = 1;
    DragDrop::receive(terminal, { .kind = EventKind::Enter, .sessionID = 1, .metadata = location, .payload = "text/plain" });
    feed(terminal, "\033]72;t=m:o=1;text/plain\033\\");
    DragDrop::receive(terminal, { .kind = EventKind::Drop, .sessionID = 1, .metadata = location });
    (void)replies(terminal);
}
}

TEST(osc72DispatchPreservesQueryIdentityAndRejectsUngrantableReads)
{
    Terminal terminal;
    feed(terminal, "\033]72;t=q:i=4294967295\033\\");
    CHECK(replies(terminal) == "\033]72;t=q:i=4294967295;\033\\");
    feed(terminal, "\033]72;t=r:x=1\033\\");
    CHECK(replies(terminal) == "\033]72;t=R:x=1;EPERM\033\\");
    feed(terminal, "\033]72;t=o:o=1;text/plain\033\\");
    CHECK(replies(terminal) == "\033]72;t=E;EPERM\033\\");
}

TEST(nativeDropCompletionsAreDiscardedAfterRIS)
{
    Queue queue;
    Terminal terminal;
    terminal.dragDropClient = &queue;
    drop(terminal);
    feed(terminal, "\033]72;t=r:x=1\033\\");
    CommandRecord read;
    while (!queue.isEmpty()) {
        auto command = queue.take();
        if (command.kind == CommandKind::ReadDrop)
            read = std::move(command);
    }
    CHECK(read.isPresent);
    feed(terminal, "\033c");
    CHECK(terminal.dragDropClient.isInstalled(&queue));
    DragDrop::receive(terminal, { .kind = EventKind::ReadData, .sessionID = read.sessionID, .requestID = read.requestID, .payload = "secret" });
    CHECK(replies(terminal).empty());
    bool hasFinish = false;
    while (!queue.isEmpty())
        hasFinish |= queue.take().kind == CommandKind::FinishDrop;
    CHECK(hasFinish);
    feed(terminal, "\033]72;t=r:x=1\033\\");
    CHECK(replies(terminal).find("EPERM") != std::string::npos);
}

TEST(aFullReplyQueueReportsFailureAndRevokesNativeReads)
{
    Queue queue;
    Terminal terminal;
    terminal.dragDropClient = &queue;
    drop(terminal);
    feed(terminal, "\033]72;t=r:x=1\033\\");
    CommandRecord read;
    while (!queue.isEmpty()) {
        auto command = queue.take();
        if (command.kind == CommandKind::ReadDrop)
            read = std::move(command);
    }
    REQUIRE(read.isPresent);
    const std::string prefix(VT::maxBulkOutputBytes - 1024, 'a');
    REQUIRE(respondBulk(terminal, Capability::InputModeReport, prefix));
    DragDrop::receive(terminal, { .kind = EventKind::ReadData, .sessionID = read.sessionID, .requestID = read.requestID, .payload = "undeliverable" });
    const std::string_view queued(terminal.bulkOutput.data, terminal.bulkOutput.length);
    CHECK(queued.starts_with(prefix));
    CHECK(queued.substr(prefix.size()) == "\033]72;t=R:x=1:i=7;ENOMEM\033\\");
    CHECK(queue.take().kind == CommandKind::FinishDrop);
    const auto length = terminal.bulkOutput.length;
    DragDrop::receive(terminal, { .kind = EventKind::ReadData, .sessionID = read.sessionID, .requestID = read.requestID, .payload = "stale" });
    CHECK(terminal.bulkOutput.length == length);
}

TEST(sequencePolicyIsCheckedAgainForDeferredDropReplies)
{
    Queue queue;
    Terminal terminal;
    terminal.dragDropClient = &queue;
    drop(terminal);
    feed(terminal, "\033]72;t=r:x=1\033\\");
    CommandRecord read;
    while (!queue.isEmpty()) {
        auto command = queue.take();
        if (command.kind == CommandKind::ReadDrop)
            read = std::move(command);
    }
    const int code = 72;
    CHECK(OSC::setOSCDenyList(terminal, &code, 1));
    DragDrop::receive(terminal, { .kind = EventKind::ReadData, .sessionID = 1, .requestID = read.requestID, .payload = "secret" });
    CHECK(replies(terminal).empty());
    CHECK(!terminal.dragDropSession->acceptsDrops());
}

TEST(cancellationRemovesUndeliveredDataForOnlyItsSession)
{
    Queue queue;
    CHECK(queue.sendCommand({ .kind = CommandKind::PrepareData, .sessionID = 1, .payload = "discard" }));
    CHECK(queue.sendCommand({ .kind = CommandKind::ReadDrop, .sessionID = 2, .requestID = 4 }));
    CHECK(queue.sendCommand({ .kind = CommandKind::CancelDrag, .sessionID = 1 }));
    auto first = queue.take();
    CHECK(first.kind == CommandKind::ReadDrop);
    CHECK(first.sessionID == 2);
    CHECK(queue.take().kind == CommandKind::CancelDrag);
    CHECK(queue.isEmpty());
}

TEST(handoverCarriesRegistrationsButRefusesAnActiveNativeDrop)
{
    using namespace Ubiquitty::Protocol::Sync;
    Queue queue;
    Terminal source, restored;
    source.dragDropClient = &queue;
    feed(source, "\033]72;t=a:i=42;text/plain image/png\033\\\033]72;t=a:x=1;1:other\033\\\033]72;t=o:x=1:i=43;1:offer\033\\");
    auto encoded = encodeHandoverHostState(source, 1024 * 1024);
    CHECK(encoded.has_value());
    if (!encoded)
        return;
    CHECK(restoreHandoverHostState(encoded->view(), restored, 1024 * 1024).has_value());
    CHECK(restored.dragDropSession);
    if (!restored.dragDropSession)
        return;
    const auto state = restored.dragDropSession->registrationState();
    CHECK(state.acceptsDrops);
    CHECK(state.isOfferingDrags);
    CHECK(state.acceptedMIMEs == "text/plain image/png");
    CHECK(state.dropMachine == "1:other");
    CHECK(state.offerMachine == "1:offer");
    CHECK(state.dropIdentity == 42);
    CHECK(state.offerIdentity == 43);
    drop(source);
    auto active = encodeHandoverHostState(source, 1024 * 1024);
    CHECK(!active && active.error() == HandoverBodyError::NotAtGround);
}

TEST(registrationCoalescingPreservesRevocationBeforeReenable)
{
    Queue queue;
    Metadata stop;
    stop.type = Type::StopAccepting;
    CHECK(queue.sendCommand({ .kind = CommandKind::Registration, .metadata = stop }));
    stop.type = Type::Offer;
    stop.x = 2;
    CHECK(queue.sendCommand({ .kind = CommandKind::Registration, .metadata = stop }));
    Metadata accept;
    accept.type = Type::Accept;
    Metadata offer;
    offer.type = Type::Offer;
    offer.x = 1;
    for (int index = 0; index < 40000; ++index) {
        CHECK(queue.sendCommand({ .kind = CommandKind::Registration, .metadata = accept, .payload = "text/plain" }));
        CHECK(queue.sendCommand({ .kind = CommandKind::Registration, .metadata = offer }));
    }
    CHECK(queue.take().metadata.type == Type::StopAccepting);
    CHECK(queue.take().metadata.x == 2);
    CHECK(queue.take().metadata.type == Type::Accept);
    CHECK(queue.take().metadata.x == 1);
    CHECK(queue.isEmpty());
}

UBIQUITTY_TEST_MAIN()
