// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/DragDrop/Session.h"

#include <array>
#include <string>
#include <vector>

using namespace Ubiquitty::DragDrop;

namespace {

struct RecordingChannel final : Channel {
    struct RecordedCommand {
        CommandKind kind;
        std::uint64_t session;
        std::uint64_t request;
        Metadata metadata;
        std::string payload;
        bool isFinal;
    };
    std::vector<std::string> replies;
    std::vector<RecordedCommand> commands;
    bool canWrite = true;
    bool canSend = true;
    bool shouldRetainCommands = true;
    std::size_t dataBytes = 0;

    bool writeReply(std::span<const char> bytes) override
    {
        if (!canWrite)
            return false;
        replies.emplace_back(bytes.data(), bytes.size());
        return true;
    }
    bool sendCommand(const Command& command) override
    {
        if (!canSend)
            return false;
        if (command.kind == CommandKind::PrepareData || command.kind == CommandKind::DragData || command.kind == CommandKind::RemoteFileData)
            dataBytes += command.payload.size();
        if (shouldRetainCommands)
            commands.push_back({ command.kind, command.sessionID, command.requestID, command.metadata, std::string(command.payload), command.isFinal });
        return true;
    }
    PacketView lastReply() const
    {
        CHECK(!replies.empty());
        const auto& bytes = replies.back();
        CHECK(bytes.starts_with("\033]72;"));
        CHECK(bytes.ends_with("\033\\"));
        const auto parsed = parsePacket(std::string_view(bytes).substr(5, bytes.size() - 7));
        CHECK(parsed.has_value());
        return parsed.value_or(PacketView {});
    }
};

Metadata location()
{
    Metadata value;
    value.x = 2;
    value.y = 3;
    value.pixelX = 16;
    value.pixelY = 48;
    value.operation = 3;
    return value;
}

void enter(Session& session, RecordingChannel& channel, std::string_view mimes = "text/plain text/uri-list")
{
    session.receive("t=a:i=4294967295;", channel);
    session.receive({ .kind = EventKind::Enter, .sessionID = 1, .metadata = location(), .payload = mimes }, channel);
}

void drop(Session& session, RecordingChannel& channel)
{
    enter(session, channel);
    session.receive("t=m:o=1;text/plain", channel);
    session.receive({ .kind = EventKind::Drop, .sessionID = 1, .metadata = location() }, channel);
}

void offer(Session& session, RecordingChannel& channel)
{
    session.receive("t=o:x=1:i=42", channel);
    session.receive({ .kind = EventKind::BeginGesture, .sessionID = 1, .metadata = location() }, channel);
    session.receive("t=o:o=3;text/plain image/png", channel);
}

void start(Session& session, RecordingChannel& channel)
{
    offer(session, channel);
    session.receive("t=P:x=-1", channel);
    session.receive({ .kind = EventKind::Started, .sessionID = 1 }, channel);
}

} // namespace

TEST(metadataAdmitsTheFullIntegerDomainAndRejectsAmbiguousFields)
{
    for (const auto value : { "0", "2147483647", "2147483648", "4294967295" }) {
        const auto parsed = parsePacket(std::string("t=q:i=") + value);
        CHECK(parsed.has_value());
        CHECK(parsed->metadata.identity.has_value());
    }
    CHECK(parsePacket("t=p:x=-2147483648").has_value());
    for (const auto text : { "t=q:i=4294967296", "t=q:i=-1", "t=q:i=1:i=2", "t=q:t=o", "t=q:i=+1", "t=q:i=1x",
             "t=p:x=-2147483649", "t=p:x=9999999999999999999999", "t=qq", "t=z", "m=2", "t=q::i=1", "t=q:bad" })
        CHECK(!parsePacket(text));
    CHECK(!parsePacket("t=p;" + std::string(4097, 'A')));
    CHECK(!parsePacket(std::string(257, 'a')));
    CHECK(parsePacket("t=q;" + std::string(4096, 'A')));
}

TEST(queriesEchoExplicitZeroAndUnsignedIdentifiers)
{
    Session session;
    RecordingChannel channel;
    for (const auto value : { "0", "2147483648", "4294967295" }) {
        session.receive(std::string("t=q:i=") + value, channel);
        CHECK(channel.replies.back() == std::string("\033]72;t=q:i=") + value + ";\033\\");
    }
    session.receive("t=q", channel);
    CHECK(channel.replies.back() == "\033]72;t=q;\033\\");
}

TEST(movementDoesNotGrantReadAuthorityAndLeavingRevokesTheDrag)
{
    Session session;
    RecordingChannel channel;
    session.receive("t=r:x=4294967295", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    CHECK_EQ(channel.lastReply().metadata.x.value(), 4294967295LL);
    enter(session, channel);
    CHECK(channel.lastReply().metadata.type == Type::Move);
    CHECK(channel.lastReply().payload == "text/plain text/uri-list");
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    session.receive("t=m:o=1", channel);
    CHECK(channel.commands.back().kind == CommandKind::AcceptDrop);
    session.receive({ .kind = EventKind::Leave, .sessionID = 1 }, channel);
    CHECK_EQ(channel.lastReply().metadata.x.value(), -1);
    CHECK_EQ(channel.lastReply().metadata.y.value(), -1);
    CHECK(channel.lastReply().payload.empty());
    session.receive({ .kind = EventKind::Drop, .sessionID = 1, .metadata = location() }, channel);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(aDropRequiresAcceptanceAndCarriesTheCompleteMIMEList)
{
    Session session;
    RecordingChannel channel;
    enter(session, channel);
    session.receive({ .kind = EventKind::Drop, .sessionID = 1, .metadata = location() }, channel);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    session.receive({ .kind = EventKind::Enter, .sessionID = 2, .metadata = location(), .payload = "text/plain text/uri-list" }, channel);
    session.receive("t=m:o=1;text/plain", channel);
    session.receive({ .kind = EventKind::Drop, .sessionID = 2, .metadata = location() }, channel);
    CHECK(channel.lastReply().metadata.type == Type::Drop);
    CHECK(channel.lastReply().payload == "text/plain text/uri-list");
    CHECK_EQ(channel.lastReply().metadata.identity.value(), UINT32_MAX);
}

TEST(droppedDataIsCorrelatedAndLateCompletionsAreDiscarded)
{
    Session session;
    RecordingChannel channel;
    drop(session, channel);
    session.receive("t=r:x=1", channel);
    const auto request = channel.commands.back().request;
    CHECK(request);
    session.receive({ .kind = EventKind::ReadData, .sessionID = 2, .requestID = request, .payload = "secret" }, channel);
    CHECK(channel.lastReply().metadata.type == Type::Drop);
    session.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .payload = "hello" }, channel);
    CHECK(channel.lastReply().payload == "aGVsbG8=");
    CHECK_EQ(channel.lastReply().metadata.x.value(), 1);
    session.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .isFinal = true }, channel);
    CHECK(channel.lastReply().payload.empty());
    session.receive("t=r:o=1", channel);
    CHECK(channel.commands.back().kind == CommandKind::FinishDrop);
    const auto count = channel.replies.size();
    session.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .payload = "late" }, channel);
    CHECK_EQ(channel.replies.size(), count);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(uriEntriesRequireACompletedURIListAndValidDirectoryHandles)
{
    Session session;
    RecordingChannel channel;
    drop(session, channel);
    session.receive("t=r:x=2:y=1", channel);
    CHECK(channel.lastReply().payload == "EINVAL");
    CHECK(channel.commands.back().kind == CommandKind::FinishDrop);

    Session next;
    RecordingChannel other;
    drop(next, other);
    next.receive("t=r:x=2", other);
    auto request = other.commands.back().request;
    next.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .payload = "file:///selected\r\n" }, other);
    next.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .isFinal = true }, other);
    next.receive("t=r:x=2:y=1", other);
    CHECK(other.commands.back().kind == CommandKind::ReadDrop);
    request = other.commands.back().request;
    Metadata directory;
    directory.pixelX = 19;
    next.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .metadata = directory, .payload = "child" }, other);
    next.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .metadata = directory, .isFinal = true }, other);
    next.receive("t=r:Y=19:x=1", other);
    CHECK(other.commands.back().kind == CommandKind::ReadDrop);
    next.receive("t=r:Y=19", other);
    CHECK(other.commands.back().kind == CommandKind::CloseDirectory);
    next.receive("t=r:Y=19:x=1", other);
    CHECK(other.lastReply().payload == "EINVAL");
}

TEST(pendingReadsAreBoundedAndExhaustionTerminatesTheDrop)
{
    Session session;
    RecordingChannel channel;
    drop(session, channel);
    for (std::size_t index = 0; index < maxPendingReads; ++index)
        session.receive("t=r:x=1", channel);
    CHECK(channel.commands.back().kind == CommandKind::ReadDrop);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EMFILE");
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(anOutgoingOfferRequiresAnActiveNativeGesture)
{
    Session session;
    RecordingChannel channel;
    session.receive("t=o:o=1;text/plain", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    session.receive("t=o:x=1", channel);
    session.receive("t=o:o=1;text/plain", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    session.receive({ .kind = EventKind::BeginGesture, .sessionID = 1, .metadata = location() }, channel);
    CHECK(channel.lastReply().metadata.type == Type::Offer);
    session.receive({ .kind = EventKind::EndGesture, .sessionID = 1 }, channel);
    session.receive("t=o:o=1;text/plain", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(chunkMetadataAndMultiplexerIdentityBelongToTheFirstPacket)
{
    Session session;
    RecordingChannel channel;
    offer(session, channel);
    session.receive("t=p:x=0:m=1;Y", channel);
    session.receive("t=q:i=7", channel);
    CHECK_EQ(channel.lastReply().metadata.identity.value(), 7U);
    session.receive("t=p:x=1:i=99:m=0;Q==", channel);
    CHECK(channel.commands.back().payload == "a");
    CHECK_EQ(channel.commands.back().metadata.x.value(), 0);
    session.receive("t=p:x=0", channel);
    CHECK(channel.commands.back().isFinal);
    session.receive("t=P:x=-1", channel);
    CHECK(channel.commands.back().kind == CommandKind::BeginDrag);
    session.receive({ .kind = EventKind::Started, .sessionID = 1 }, channel);
    CHECK(channel.lastReply().payload == "OK");
    CHECK_EQ(channel.lastReply().metadata.identity.value(), 42U);
}

TEST(base64FramingAndIncompleteDataPreventNativeDragStart)
{
    for (const auto payload : { "Y!Q=", "YQ=", "YQ===", "YQ==X", "YQ==\n" }) {
        Session session;
        RecordingChannel channel;
        offer(session, channel);
        session.receive(std::string("t=p:x=0;") + payload, channel);
        CHECK(channel.lastReply().payload == "EINVAL");
        CHECK(channel.commands.back().kind == CommandKind::CancelDrag);
    }
    Session session;
    RecordingChannel channel;
    offer(session, channel);
    session.receive("t=p:x=0;YQ", channel);
    CHECK(channel.commands.back().payload == "a");
    session.receive("t=P:x=-1", channel);
    CHECK(channel.lastReply().payload == "EINVAL");
}

TEST(preSentDataAcceptsSixtyFourMiBAndRefusesAnExtraByte)
{
    Session session;
    RecordingChannel channel;
    offer(session, channel);
    channel.shouldRetainCommands = false;
    const std::string packet = "t=p:x=0;" + std::string(4096, 'A');
    const auto chunks = maxPreparedDataBytes / 3072;
    for (std::size_t index = 0; index < chunks; ++index)
        session.receive(packet, channel);
    const auto remaining = maxPreparedDataBytes - chunks * 3072;
    session.receive("t=p:x=0;" + std::string(remaining / 3 * 4, 'A') + "AA==", channel);
    CHECK_EQ(channel.dataBytes, maxPreparedDataBytes);
    session.receive("t=p:x=0;AA==", channel);
    CHECK(channel.lastReply().payload == "EFBIG");
}

TEST(rawThumbnailDimensionsAndOrderAreChecked)
{
    Session session;
    RecordingChannel channel;
    offer(session, channel);
    session.receive("t=p:x=-1:y=24:X=1:Y=1;AAAA", channel);
    session.receive("t=p:x=-1", channel);
    CHECK(channel.commands.back().isFinal);
    session.receive("t=p:x=-2:y=32:X=1:Y=1;AAAA", channel);
    session.receive("t=p:x=-2", channel);
    CHECK(channel.lastReply().payload == "EINVAL");

    Session other;
    RecordingChannel second;
    offer(other, second);
    other.receive("t=p:x=-2:y=24:X=1:Y=1;AAAA", second);
    CHECK(second.lastReply().payload == "EINVAL");
}

TEST(outgoingDataIsRequestedByTheNativeDestination)
{
    Session session;
    RecordingChannel channel;
    start(session, channel);
    CHECK(channel.lastReply().payload == "OK");
    Metadata metadata;
    metadata.y = 0;
    session.receive({ .kind = EventKind::RequestData, .sessionID = 1, .metadata = metadata }, channel);
    CHECK_EQ(channel.lastReply().metadata.x.value(), 5);
    session.receive("t=e:y=0;YQ==", channel);
    CHECK(channel.commands.back().kind == CommandKind::DragData);
    CHECK(channel.commands.back().payload == "a");
    session.receive("t=e:y=0", channel);
    CHECK(channel.commands.back().isFinal);
    session.receive("t=e:y=0;Yg==", channel);
    CHECK(channel.lastReply().payload == "EINVAL");
}

TEST(earlyDataAndResetCannotStartOrContinueADrag)
{
    for (const auto command : { "t=e:y=0;YQ==", "t=E:y=-1" }) {
        Session session;
        RecordingChannel channel;
        offer(session, channel);
        session.receive(command, channel);
        CHECK(channel.lastReply().payload == "EINVAL");
    }
    Session session;
    RecordingChannel channel;
    start(session, channel);
    session.reset(channel);
    CHECK(!session.acceptsDrops());
    CHECK(!session.offersDrags());
    session.receive("t=e:y=0;YQ==", channel);
    CHECK(channel.lastReply().payload == "EINVAL");
}

TEST(remoteFilesMustCorrespondToARequestedURI)
{
    Session session;
    RecordingChannel channel;
    start(session, channel);
    Metadata metadata;
    metadata.x = 3;
    session.receive({ .kind = EventKind::RequestFile, .sessionID = 1, .metadata = metadata }, channel);
    CHECK(channel.lastReply().metadata.type == Type::File);
    CHECK_EQ(channel.lastReply().metadata.x.value(), 3);
    session.receive("t=k:x=3;YQ==", channel);
    CHECK(channel.commands.back().kind == CommandKind::RemoteFileData);
    CHECK(channel.commands.back().payload == "a");
    session.receive("t=k:x=2;YQ==", channel);
    CHECK(channel.lastReply().payload == "EINVAL");
}

TEST(changedMIMEsRequireANewAcceptance)
{
    Session session;
    RecordingChannel channel;
    enter(session, channel);
    session.receive("t=m:o=1;text/plain", channel);
    session.receive({ .kind = EventKind::Move, .sessionID = 1, .metadata = location(), .payload = "image/png" }, channel);
    CHECK(channel.lastReply().payload == "image/png");
    CHECK(channel.commands.back().kind == CommandKind::AcceptDrop);
    CHECK(channel.commands.back().metadata.operation == 0);
    session.receive({ .kind = EventKind::Drop, .sessionID = 1, .metadata = location() }, channel);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(nativeCancellationRevokesACompletedDrop)
{
    Session session;
    RecordingChannel channel;
    drop(session, channel);
    session.receive("t=r:x=1", channel);
    const auto request = channel.commands.back().request;
    session.receive({ .kind = EventKind::Cancel, .sessionID = 1 }, channel);
    CHECK(channel.commands.back().kind == CommandKind::FinishDrop);
    const auto replies = channel.replies.size();
    session.receive({ .kind = EventKind::ReadData, .sessionID = 1, .requestID = request, .payload = "private" }, channel);
    CHECK_EQ(channel.replies.size(), replies);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
}

TEST(nativeOwnerReplacementPreservesRegistrationAndRevokesOldWork)
{
    Session session;
    RecordingChannel channel;
    drop(session, channel);
    session.rebindNativeOwner(channel);
    CHECK(session.acceptsDrops());
    CHECK(!session.hasPendingWork());
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().payload == "EPERM");
    session.receive({ .kind = EventKind::Enter, .sessionID = 1, .metadata = location(), .payload = "text/plain" }, channel);
    CHECK(channel.lastReply().metadata.type == Type::Move);
}

TEST(remoteRootsAreBoundedIndependentlyOfPendingDropReads)
{
    Session session;
    RecordingChannel channel;
    start(session, channel);
    Metadata request;
    for (std::size_t index = 1; index <= maxRemoteRoots; ++index) {
        request.x = static_cast<std::int64_t>(index);
        session.receive({ .kind = EventKind::RequestFile, .sessionID = 1, .metadata = request }, channel);
        CHECK(channel.lastReply().metadata.type == Type::File);
    }
    const auto replies = channel.replies.size();
    session.receive({ .kind = EventKind::RequestFile, .sessionID = 1, .metadata = request }, channel);
    CHECK_EQ(channel.replies.size(), replies);
    request.x = static_cast<std::int64_t>(maxRemoteRoots + 1);
    session.receive({ .kind = EventKind::RequestFile, .sessionID = 1, .metadata = request }, channel);
    CHECK(channel.lastReply().payload == "EMFILE");
}

TEST(mimeParametersAndPrivateNamesRemainIntact)
{
    const auto count = countMIMETypes("text/plain;charset=utf-8 application/x-private:part");
    REQUIRE(count);
    CHECK_EQ(*count, 2u);
    CHECK(mimeAt("text/plain;charset=utf-8", 0) == "text/plain;charset=utf-8");
}

TEST(multiplexerIdentityPersistsUntilRegistrationEnds)
{
    Session session;
    RecordingChannel channel;
    session.receive("t=a:i=42;text/plain", channel);
    session.receive("t=a;text/plain", channel);
    session.receive("t=r:x=1", channel);
    CHECK(channel.lastReply().metadata.identity == 42);
    session.receive("t=A", channel);
    session.receive("t=r:x=1", channel);
    CHECK(!channel.lastReply().metadata.identity);
    offer(session, channel);
    session.receive("t=o:x=2", channel);
    session.receive("t=o:o=1;text/plain", channel);
    CHECK(!channel.lastReply().metadata.identity);
}

TEST(aDragOfferCanSetTheMultiplexerIdentity)
{
    Session session;
    RecordingChannel channel;
    session.receive("t=o:x=1", channel);
    session.receive({ .kind = EventKind::BeginGesture, .sessionID = 1, .metadata = location() }, channel);
    session.receive("t=o:o=1:i=4294967295;text/plain", channel);
    session.receive("t=P:x=-1", channel);
    session.receive({ .kind = EventKind::Started, .sessionID = 1 }, channel);
    CHECK(channel.lastReply().metadata.identity == UINT32_MAX);
    CHECK(channel.lastReply().payload == "OK");
}

TEST(aMIMEErrorDoesNotCancelOtherRequestedTypes)
{
    Session session;
    RecordingChannel channel;
    start(session, channel);
    Metadata metadata;
    metadata.y = 0;
    session.receive({ .kind = EventKind::RequestData, .sessionID = 1, .metadata = metadata }, channel);
    metadata.y = 1;
    session.receive({ .kind = EventKind::RequestData, .sessionID = 1, .metadata = metadata }, channel);
    session.receive("t=E:y=0;ENOENT", channel);
    CHECK(channel.commands.back().kind == CommandKind::DragData);
    CHECK(channel.commands.back().metadata.type == Type::Error);
    CHECK(session.hasPendingWork());
    session.receive("t=e:y=1;YQ==", channel);
    CHECK(channel.commands.back().payload == "a");
    session.receive("t=e:y=1:m=0", channel);
    CHECK(channel.commands.back().isFinal);
    session.receive("t=E:y=-1", channel);
    CHECK(!session.hasPendingWork());
    CHECK(channel.commands.back().kind == CommandKind::CancelDrag);
}

UBIQUITTY_TEST_MAIN()
