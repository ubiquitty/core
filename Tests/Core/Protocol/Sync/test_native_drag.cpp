// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/NativeDrag.h"
#include "Harness.h"
#include "SyncLink.h"

#include <limits>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

TEST(nativeDragCommandsPreserveMetadataAndBinaryData)
{
    const std::string payload("a\0b", 3);
    DragDrop::Command input { .kind = DragDrop::CommandKind::RemoteFileData, .sessionID = 8, .requestID = 19, .metadata = { .type = DragDrop::Type::File, .x = -1, .y = UINT32_MAX, .pixelX = 7, .pixelY = 9, .identity = UINT32_MAX, .operation = 3, .hasType = true, .hasMore = true, .hasMoreChunks = true }, .payload = payload, .isFinal = true };
    std::vector<std::uint8_t> bytes;
    REQUIRE(encodeNativeDragCommand(bytes, 42, input));
    const auto decoded = decodeNativeDragCommand(bytes);
    REQUIRE(decoded);
    CHECK_EQ(decoded->epoch, 42u);
    CHECK(decoded->command.kind == input.kind);
    CHECK_EQ(decoded->command.sessionID, 8u);
    CHECK_EQ(decoded->command.requestID, 19u);
    CHECK(decoded->command.metadata.x == -1);
    CHECK(decoded->command.metadata.y == UINT32_MAX);
    CHECK(decoded->command.metadata.identity == UINT32_MAX);
    CHECK(decoded->command.metadata.hasMoreChunks);
    CHECK(decoded->command.isFinal);
    CHECK(std::string(decoded->command.bytes.begin(), decoded->command.bytes.end()) == payload);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK(!decodeNativeDragCommand(std::span(bytes).first(length)));
    bytes.push_back(0);
    CHECK(!decodeNativeDragCommand(bytes));
}

TEST(nativeDragEventsPreserveCompletionAndRejectTruncation)
{
    DragDrop::NativeEvent input { .kind = DragDrop::EventKind::ReadData, .sessionID = 17, .requestID = 21, .x = -1, .y = UINT32_MAX, .pixelX = 12, .pixelY = 3, .operation = 2, .error = DragDrop::Error::InputOutput, .isFinal = true, .isSameWindow = true };
    std::vector<std::uint8_t> bytes;
    REQUIRE(encodeNativeDragEvent(bytes, 45, input, {}));
    const auto decoded = decodeNativeDragEvent(bytes);
    REQUIRE(decoded);
    CHECK_EQ(decoded->epoch, 45u);
    CHECK_EQ(decoded->event.sessionID, 17u);
    CHECK_EQ(decoded->event.requestID, 21u);
    CHECK_EQ(decoded->event.x, -1);
    CHECK_EQ(decoded->event.y, UINT32_MAX);
    CHECK(decoded->event.isFinal && decoded->event.isSameWindow);
    CHECK(decoded->event.error == DragDrop::Error::InputOutput);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK(!decodeNativeDragEvent(std::span(bytes).first(length)));
    bytes.push_back(0);
    CHECK(!decodeNativeDragEvent(bytes));
}

TEST(nativeDragEncodersRejectInvalidIdentitiesCoordinatesAndOversizedChunks)
{
    std::vector<std::uint8_t> bytes;
    DragDrop::NativeEvent event { .kind = DragDrop::EventKind::ReadData, .sessionID = 1 };
    CHECK(!encodeNativeDragEvent(bytes, 0, event, {}));
    event.x = std::numeric_limits<std::int64_t>::max();
    CHECK(!encodeNativeDragEvent(bytes, 1, event, {}));
    event.x = 0;
    std::string data(3073, 'a');
    CHECK(!encodeNativeDragEvent(bytes, 1, event, data));
    event.kind = static_cast<DragDrop::EventKind>(255);
    CHECK(!encodeNativeDragEvent(bytes, 1, event, {}));
    DragDrop::Command command { .kind = DragDrop::CommandKind::Registration };
    CHECK(!encodeNativeDragCommand(bytes, 0, command));
    command.metadata.x = std::numeric_limits<std::int64_t>::min();
    CHECK(!encodeNativeDragCommand(bytes, 1, command));
    CHECK(bytes.empty());
}

TEST(nativeDragRequiresRegistrationAndCurrentOwnerAdmission)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, { 7, 8 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted, ClientKind::Human, UbiquittyTest::NativeDragSupport::Available);
    DragDrop::NativeEvent event { .kind = DragDrop::EventKind::BeginGesture, .sessionID = 1 };
    CHECK(!link.client.sendNativeDragEvent(link.handle, event, {}));
    const DragDrop::Command registration { .kind = DragDrop::CommandKind::Registration };
    REQUIRE(link.host.sendNativeDragCommand(link.handle, 5, registration));
    link.pump(60000);
    REQUIRE(link.client.sendNativeDragEvent(link.handle, event, {}));
    link.pump(90000);
    CHECK(link.host.takeNativeDragEvents().empty());
    std::uint64_t activeEpoch = 5;
    link.host.setNativeDragAdmission([&](auto handle, auto epoch) { return handle == link.handle && epoch == activeEpoch; });
    REQUIRE(link.client.sendNativeDragEvent(link.handle, event, {}));
    link.pump(120000);
    auto events = link.host.takeNativeDragEvents();
    REQUIRE(events.size() == 1);
    CHECK_EQ(events.front().epoch, 5u);
    CHECK_EQ(events.front().handle, link.handle);
    activeEpoch = 6;
    REQUIRE(link.client.sendNativeDragEvent(link.handle, event, {}));
    link.pump(150000);
    CHECK(link.host.takeNativeDragEvents().empty());
    REQUIRE(link.host.sendNativeDragCommand(link.handle, 6, registration));
    link.pump(180000);
    REQUIRE(link.client.sendNativeDragEvent(link.handle, event, {}));
    link.pump(210000);
    events = link.host.takeNativeDragEvents();
    REQUIRE(events.size() == 1);
    CHECK_EQ(events.front().epoch, 6u);
    link.host.unbindPane(link.handle, 0);
    link.pump(240000);
    CHECK(!link.client.sendNativeDragEvent(link.handle, event, {}));
}

TEST(nativeDragIsNotGrantedToViewersOrAgents)
{
    UbiquittyTest::OwnedPane pane;
    const DragDrop::Command registration { .kind = DragDrop::CommandKind::Registration };
    UbiquittyTest::PaneConnection viewer(*pane.source, { 7, 8 }, ClientKind::Human, UbiquittyTest::DriveScope::Withheld, ClientKind::Human, UbiquittyTest::NativeDragSupport::Available);
    CHECK(!viewer.host.sendNativeDragCommand(viewer.handle, 1, registration));
    UbiquittyTest::PaneConnection agent(*pane.source, { 9, 10 }, ClientKind::Agent, UbiquittyTest::DriveScope::Granted, ClientKind::Agent, UbiquittyTest::NativeDragSupport::Available);
    CHECK(!agent.host.sendNativeDragCommand(agent.handle, 1, registration));
    UbiquittyTest::PaneConnection editor(*pane.source, { 11, 12 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    CHECK(!editor.host.sendNativeDragCommand(editor.handle, 1, registration));
}

UBIQUITTY_TEST_MAIN()
