// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_memory_owner — the connection-wide receive owner, the replica
// allowance and the host checkpoint cap (Docs/sync.md §7.9, §9), and the
// defaults Docs/sync-measurements.md "Memory admission" records. Every case
// is in memory: frames are written by hand as a hostile host would write
// them, or carried between a host and a client connection on a fake clock.

#include "CheckpointConnectionFixture.h"
#include "Harness.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/Codec/Base64.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/BulkSource.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::CheckpointConnection;

namespace {

/// Records what the client tells its owner of failed and unbound panes.
class FailureRecorder final : public ClientPaneDelegate {
public:
    void paneFailed(std::uint32_t handle, ID128, std::string_view reason) override { failures.emplace_back(handle, std::string(reason)); }
    void paneUnbound(std::uint32_t handle, ID128) override { unbound.push_back(handle); }
    void paneBound(std::uint32_t handle, ID128 paneID) override { bound.emplace_back(handle, paneID); }

    std::vector<std::pair<std::uint32_t, std::string>> failures;
    std::vector<std::uint32_t> unbound;
    std::vector<std::pair<std::uint32_t, ID128>> bound;
};

/// Welcomes a client connection as a host would.
bool welcome(ClientConnection& client)
{
    client.start();
    (void)client.takeStreamOutput();
    Welcome welcome;
    welcome.wireVersion = wireVersion;
    welcome.hostInstanceID = { 41, 42 };
    welcome.attachmentID = { 3, 4 };
    welcome.resumeSecret = { 5, 6 };
    welcome.selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "memory-owner.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    client.receiveStream(messageFrame(FrameKind::Welcome, 0, welcome), 1);
    return client.state() == ConnectionState::Attached;
}

/// Announces a checkpoint for a pane and sends its first, unfinished chunk.
void announceCheckpoint(ClientConnection& client, std::uint32_t handle, std::uint32_t transfer, std::span<const std::uint8_t> firstChunk)
{
    client.receiveStream(messageFrame(FrameKind::CheckpointHead, handle, CheckpointHead { .generation = 1, .transfer = transfer, .frameClass = FrameClass::Structural, .stateDigest = {} }), 3);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, BulkChunk { .transfer = transfer, .index = 0, .isLast = false, .bytes = firstChunk }), 3);
}

/// Counts the frames of one kind in a client's output.
std::size_t framesOfKind(std::span<const std::uint8_t> output, FrameKind kind)
{
    FrameDecoder decoder;
    decoder.append(output);
    std::size_t count = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind == kind)
            ++count;
    }
    return count;
}

ClientPolicy smallPolicy()
{
    ClientPolicy policy { .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 20, .minAckIntervalMicroseconds = 4'000 };
    // The pixels these cases send cross raw, so the client does not ask for
    // deflated images.
    policy.capabilities.set(ClientCapability::DeflateImages, false);
    return policy;
}

std::string base64(std::span<const std::uint8_t> bytes)
{
    std::string result(Ubiquitty::Codec::base64EncodedLength(bytes.size()), '\0');
    const auto characters = Ubiquitty::Codec::base64Encode({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, result);
    CHECK(characters && characters->size() == result.size());
    return result;
}

/// The placements and asset of one image a host terminal shows.
ImagePlacements imageState(std::span<const std::uint8_t> pixels, int side, std::uint32_t imageID)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 3, 9);
    UbiquittyTest::feed(*host, "\033_Ga=T,f=32,s=" + std::to_string(side) + ",v=" + std::to_string(side) + ",i=" + std::to_string(imageID) + ",q=2,C=1;" + base64(pixels) + "\033\\");
    ImagePublisher publisher(*host);
    CHECK(publisher.capture());
    ImagePlacements state = publisher.state();
    state.revision = 2;
    return state;
}

/// The recorded constants of Docs/sync-measurements.md "Memory admission",
/// in bytes.
std::map<std::string, std::uint64_t> recordedMemoryConstants()
{
    std::ifstream file(UBQ_MEASUREMENTS_DOC);
    std::stringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();
    std::map<std::string, std::uint64_t> constants;
    const std::size_t section = text.find("\n## Memory admission\n");
    if (section == std::string::npos)
        return constants;
    const std::size_t table = text.find("\n### The constants\n", section);
    if (table == std::string::npos)
        return constants;
    std::istringstream lines(text.substr(table));
    std::string line;
    std::getline(lines, line);
    while (std::getline(lines, line)) {
        if (line.starts_with("## "))
            break;
        // | `name` | value unit | rationale |
        if (!line.starts_with("| `"))
            continue;
        const std::size_t nameEnd = line.find('`', 3);
        const std::size_t valueStart = line.find("| ", nameEnd) + 2;
        const std::size_t valueEnd = line.find(" |", valueStart);
        if (nameEnd == std::string::npos || valueEnd == std::string::npos)
            continue;
        const std::string_view value = std::string_view(line).substr(valueStart, valueEnd - valueStart);
        std::uint64_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc {})
            continue;
        const std::string_view unit(end, value.data() + value.size());
        if (unit == " MiB")
            number <<= 20;
        else if (unit == " KiB")
            number <<= 10;
        else if (unit != " B")
            continue;
        constants[line.substr(3, nameEnd - 3)] = number;
    }
    return constants;
}

} // namespace

TEST(aConnectionRefusesCheckpointsPastItsReceiveOwnerWhen256PanesEachAnnounceOne)
{
    // Every pane's own 1 MiB transfer cap admits its chunk; only the
    // connection-wide owner can say that 256 of them at once are too many.
    ClientPolicy policy = smallPolicy();
    policy.maxReceiveBytes = 4 << 20;
    ClientConnection client(policy, identityFixture(), nullptr, nullptr, nullptr);
    REQUIRE(welcome(client));
    for (std::uint32_t handle = 1; handle <= maxPanesPerConnection; ++handle)
        client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = handle, .paneID = { handle, 77 } }), 2);
    REQUIRE(client.state() == ConnectionState::Attached);
    (void)client.takeStreamOutput();
    const std::vector<std::uint8_t> chunk(60'000, 0x5A);
    for (std::uint32_t handle = 1; handle <= maxPanesPerConnection; ++handle)
        announceCheckpoint(client, handle, 1'000 + handle, chunk);
    // The connection stays up; the panes past the owner are refused, each
    // asked for again by the bounded retry policy, never dropped silently.
    CHECK(client.state() == ConnectionState::Attached);
    CHECK(client.receiveMemory().heldBytes() <= policy.maxReceiveBytes);
    const std::size_t admitted = policy.maxReceiveBytes / chunk.size();
    CHECK(client.counters().transfersOverBudget >= maxPanesPerConnection - admitted);
    CHECK(client.counters().transfersOverBudget < maxPanesPerConnection);
    CHECK_EQ(client.counters().checkpointsRefused, client.counters().transfersOverBudget);
    CHECK_EQ(framesOfKind(client.takeStreamOutput(), FrameKind::Resync), client.counters().transfersOverBudget);
    // A refused transfer's later chunks are fenced, not taken for a fault.
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, BulkChunk { .transfer = 1'000 + maxPanesPerConnection, .index = 1, .isLast = false, .bytes = chunk }), 4);
    CHECK(client.state() == ConnectionState::Attached);
    CHECK(client.receiveMemory().heldBytes() <= policy.maxReceiveBytes);
}

TEST(aCheckpointPastTheReceiveOwnerFailsItsPaneWithAReasonThePersonIsTold)
{
    ClientPolicy policy = smallPolicy();
    policy.maxReceiveBytes = 32 << 10;
    policy.maxCheckpointRefusals = 1;
    FailureRecorder recorder;
    ClientConnection client(policy, identityFixture(), &recorder, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    const std::vector<std::uint8_t> chunk(60'000, 0x5A);
    announceCheckpoint(client, 1, 500, chunk);
    CHECK(client.state() == ConnectionState::Attached);
    REQUIRE(recorder.failures.size() == 1u);
    CHECK_EQ(recorder.failures.front().first, 1u);
    CHECK(recorder.failures.front().second.find("to receive within this connection’s memory limit") != std::string::npos);
    CHECK_EQ(client.receiveMemory().heldBytes(), 0u);
}

TEST(aHistoryTransferPastTheReceiveOwnerClosesWithATypedReason)
{
    ClientPolicy policy = smallPolicy();
    policy.maxReceiveBytes = 32 << 10;
    ClientConnection client(policy, identityFixture(), nullptr, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    client.receiveStream(messageFrame(FrameKind::HistoryHead, 1, HistoryHead { .transfer = 600, .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending }), 3);
    const std::vector<std::uint8_t> chunk(60'000, 0x5A);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, BulkChunk { .transfer = 600, .index = 0, .isLast = false, .bytes = chunk }), 3);
    CHECK(client.state() == ConnectionState::Closed);
    REQUIRE(client.byeReason());
    CHECK(*client.byeReason() == ByeReason::TransferRefused);
}

TEST(installedImagesAreSummedAcrossPanesAndAnUnbindMakesRoomAgain)
{
    // Each pane's terminal has room for both images under its own budget;
    // the connection's installed-image allowance holds one.
    ClientPolicy policy = smallPolicy();
    policy.maxInstalledImageBytes = 16;
    ClientConnection client(policy, identityFixture(), nullptr, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 2, .paneID = secondPaneID }), 2);
    const std::array<std::uint8_t, 16> firstPixels { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    const std::array<std::uint8_t, 16> secondPixels { 17, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    const ImagePlacements first = imageState(firstPixels, 2, 7);
    const ImagePlacements second = imageState(secondPixels, 2, 8);
    REQUIRE(first.imageCount == 1 && second.imageCount == 1);
    const auto send = [&](std::uint32_t handle, const ImagePlacements& state, std::uint32_t transfer, std::span<const std::uint8_t> pixels) {
        client.receiveStream(messageFrame(FrameKind::ImagePlacements, handle, state), 3);
        client.receiveStream(messageFrame(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[0].asset, .transfer = transfer }), 3);
        BulkChunk chunk { .transfer = transfer, .index = 0, .isLast = true, .bytes = pixels };
        Ubiquitty::Crypto::sha256(pixels, chunk.sha256);
        client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, chunk), 3);
    };
    send(1, first, 100, firstPixels);
    REQUIRE(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.installedImageBytes(), 16u);
    (void)client.takeStreamOutput();
    send(2, second, 101, secondPixels);
    REQUIRE(client.state() == ConnectionState::Attached);
    const Ubiquitty::Terminal* secondTerminal = client.paneTerminal(2);
    REQUIRE(secondTerminal);
    CHECK_EQ(Ubiquitty::Image::totalBytes(secondTerminal), 0u);
    CHECK_EQ(client.installedImageBytes(), 16u);
    // The first pane leaving frees its pixels; the refused image is asked for.
    client.receiveStream(messageFrame(FrameKind::PaneUnbind, 0, PaneUnbind { .handle = 1, .reason = 0 }), 4);
    REQUIRE(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.installedImageBytes(), 0u);
    CHECK_EQ(framesOfKind(client.takeStreamOutput(), FrameKind::ImageWant), 1u);
}

/// Fills a terminal so its checkpoint is long: a colour and a letter of
/// their own in every cell.
void fillDensely(Ubiquitty::Terminal& terminal, int salt)
{
    for (int row = 0; row < terminal.rows; ++row) {
        std::string text = "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int column = 0; column < terminal.columns; ++column)
            text += "\x1b[38;5;" + std::to_string((row * 7 + column + salt) % 256) + "m" + static_cast<char>('a' + (row + column + salt) % 26);
        UbiquittyTest::feed(terminal, text);
    }
}

TEST(aPaneUnboundForItsCheckpointSizeComesBackOnceItFitsAndBacksOffWhileItHovers)
{
    // The cap sits between a cleared screen's checkpoint and a dense one's.
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    const std::size_t cleared = checkpointOf(terminal).size();
    fillDensely(terminal, 0);
    const std::size_t dense = checkpointOf(terminal).size();
    REQUIRE(dense > 2 * cleared);
    PaneSource source(terminal, { 9, 10 });
    source.publish(FrameClass::Structural, 1);
    HostPolicy hostPolicy = UbiquittyTest::syncHostPolicy(0);
    hostPolicy.maxCheckpointBytes = (cleared + dense) / 2;
    HostConnection host(hostPolicy, HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .principal = "test", .mayDrive = true, .verifiedKind = ClientKind::Human, .selection = UbiquittyTest::syncDefaultSelection() });
    FailureRecorder recorder;
    ClientConnection client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = { 7, 8 }, .kind = ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts() }, &recorder);
    std::uint64_t now = 1;
    const auto pump = [&] {
        host.service(now);
        std::vector<std::uint8_t> stream;
        while (host.popStreamFrame(stream, now)) {
        }
        client.receiveStream(stream, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    };
    const auto runFor = [&](std::uint64_t microseconds) {
        for (const std::uint64_t until = now + microseconds; now < until; now += 16'667)
            pump();
    };
    const auto publish = [&](std::string_view bytes) {
        UbiquittyTest::feed(terminal, bytes);
        source.publish(FrameClass::Settled, now);
    };
    client.start();
    pump();
    pump();
    const std::uint32_t first = host.bindPane(source);
    REQUIRE(first != 0);
    runFor(100'000);

    // Past the cap: unbound, and the person is told why.
    CHECK_EQ(host.counters().checkpointsTooLarge, 1u);
    CHECK(client.state() == ConnectionState::Attached);
    REQUIRE(recorder.failures.size() == 1u);
    CHECK_EQ(recorder.failures.front().first, first);
    CHECK(recorder.failures.front().second.find("checkpoint limit") != std::string::npos);
    CHECK_EQ(recorder.unbound.size(), 1u);
    CHECK_EQ(host.oversizedPaneCount(), 1u);

    // Shrunk at once, it waits out the first back-off, then comes back under
    // a new handle with its picture.
    publish("\x1b[0m\x1b[2J\x1b[H$ ");
    runFor(100'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 0u);
    runFor(300'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 1u);
    CHECK_EQ(host.oversizedPaneCount(), 0u);
    const auto rebinds = host.takePaneRebinds();
    REQUIRE(rebinds.size() == 1u);
    CHECK_EQ(rebinds.front().previousHandle, first);
    const std::uint32_t second = rebinds.front().handle;
    REQUIRE(recorder.bound.size() == 2u);
    CHECK((recorder.bound.back() == std::pair<std::uint32_t, ID128> { second, ID128 { 9, 10 } }));
    CHECK(client.paneTerminal(second) != nullptr);
    CHECK(client.counters().checkpointsApplied >= 1u);

    // Past the cap again: a second refusal in a row. A change of geometry
    // makes the host owe a checkpoint rather than an update. Without a newer
    // frame the pane is never measured, and a newer frame still past the cap
    // is refused again after a back-off that has doubled.
    Ubiquitty::resize(terminal, 23, 80);
    fillDensely(terminal, 1);
    source.publish(FrameClass::Structural, now);
    runFor(100'000);
    CHECK_EQ(host.counters().checkpointsTooLarge, 2u);
    CHECK_EQ(recorder.failures.size(), 2u);
    const std::uint64_t measured = host.counters().oversizedPanesMeasured;
    runFor(2'000'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 1u);
    CHECK_EQ(host.counters().oversizedPanesMeasured, measured);
    fillDensely(terminal, 2);
    source.publish(FrameClass::Settled, now);
    runFor(200'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 1u);
    CHECK_EQ(host.counters().oversizedPanesMeasured, measured + 1);
    CHECK_EQ(host.oversizedPaneCount(), 1u);
    // Shrunk, it comes back within the longest back-off reached so far.
    publish("\x1b[0m\x1b[2J\x1b[H$ ");
    runFor(1'200'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 2u);
    CHECK_EQ(host.takePaneRebinds().size(), 1u);
    CHECK_EQ(recorder.failures.size(), 2u);

    // The owner letting an unbound pane go ends the wait to bind it again.
    Ubiquitty::resize(terminal, 24, 80);
    fillDensely(terminal, 3);
    source.publish(FrameClass::Structural, now);
    runFor(100'000);
    REQUIRE(recorder.failures.size() == 3u);
    host.unbindPane(recorder.failures.back().first, std::to_underlying(PaneUnbindReason::Ended));
    CHECK_EQ(host.oversizedPaneCount(), 0u);
    publish("\x1b[0m\x1b[2J\x1b[H$ ");
    runFor(5'000'000);
    CHECK_EQ(host.counters().oversizedPanesRebound, 2u);
}

TEST(aCheckpointWhoseGridWouldPassTheReplicaAllowanceIsRefusedWithAReason)
{
    // The bound pane's own small picture fits; growing it to 100 x 300 does not.
    ClientPolicy policy = smallPolicy();
    policy.maxCheckpointRefusals = 1;
    std::size_t bound = 0;
    {
        ClientConnection probe(policy, identityFixture(), nullptr, nullptr, nullptr);
        REQUIRE(welcome(probe));
        probe.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
        bound = probe.retainedReplicaBytes();
        REQUIRE(bound > 0);
    }
    policy.maxRetainedReplicaBytes = bound + 1'024;
    FailureRecorder recorder;
    ClientConnection client(policy, identityFixture(), &recorder, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    auto source = makeTerminal(100, 300);
    UbiquittyTest::feed(*source, "a picture larger than the replica allowance");
    const auto body = checkpointOf(*source);
    // The head names the body's own state, so only the allowance can refuse it.
    WireReader digestReader { std::span<const std::uint8_t>(body).last(16) };
    const Digest128 digest { .low = digestReader.u64(), .high = digestReader.u64() };
    client.receiveStream(messageFrame(FrameKind::CheckpointHead, 1, CheckpointHead { .generation = 1, .transfer = 701, .frameClass = FrameClass::Structural, .stateDigest = digest }), 3);
    BulkChunk chunk { .transfer = 701, .index = 0, .isLast = true, .bytes = body };
    Ubiquitty::Crypto::sha256(body, chunk.sha256);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, chunk), 3);
    CHECK(client.state() == ConnectionState::Attached);
    REQUIRE(recorder.failures.size() == 1u);
    CHECK(recorder.failures.front().second.find("to keep within this app’s memory limit") != std::string::npos);
    CHECK(client.retainedReplicaBytes() <= policy.maxRetainedReplicaBytes);
}

TEST(aCheckpointIsPreparedOnlyWithinTheReceiveOwner)
{
    // The body fits the owner; its preparation allowance does not.
    ClientPolicy policy = smallPolicy();
    policy.maxCheckpointRefusals = 1;
    policy.maxCheckpointStorageCost = 2 << 20;
    policy.maxReceiveBytes = 1 << 20;
    FailureRecorder recorder;
    ClientConnection client(policy, identityFixture(), &recorder, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    auto source = makeTerminal(3, 9);
    UbiquittyTest::feed(*source, "small");
    const auto body = checkpointOf(*source);
    WireReader digestReader { std::span<const std::uint8_t>(body).last(16) };
    const Digest128 digest { .low = digestReader.u64(), .high = digestReader.u64() };
    client.receiveStream(messageFrame(FrameKind::CheckpointHead, 1, CheckpointHead { .generation = 1, .transfer = 900, .frameClass = FrameClass::Structural, .stateDigest = digest }), 3);
    BulkChunk chunk { .transfer = 900, .index = 0, .isLast = true, .bytes = body };
    Ubiquitty::Crypto::sha256(body, chunk.sha256);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, chunk), 3);
    CHECK(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.counters().checkpointsApplied, 0u);
    REQUIRE(recorder.failures.size() == 1u);
    CHECK(recorder.failures.front().second.find("to prepare within this connection’s memory limit") != std::string::npos);
}

TEST(historyPastTheReplicaAllowanceTrimsTheIngestingPane)
{
    // A pane whose picture fits leaves no room for one more history line.
    auto source = makeTerminal(3, 9);
    for (int line = 0; line < 8; ++line)
        UbiquittyTest::feed(*source, "line " + std::to_string(line) + "\r\n");
    const auto body = checkpointOf(*source);
    ClientPolicy policy = smallPolicy();
    ConnectionFixture fixture(policy);
    REQUIRE(fixture.start());
    fixture.bind(1, firstPaneID);
    fixture.checkpoint(1, body, 950, 3);
    REQUIRE(fixture.client.counters().checkpointsApplied == 1u);
    ClientPolicy tight = policy;
    tight.maxRetainedReplicaBytes = fixture.client.retainedReplicaBytes() + 16;
    ConnectionFixture bounded(tight);
    REQUIRE(bounded.start());
    bounded.bind(1, firstPaneID);
    bounded.checkpoint(1, body, 951, 3);
    REQUIRE(bounded.client.counters().checkpointsApplied == 1u);
    const HistoryCache* history = bounded.client.historyCache(1);
    REQUIRE(history);
    REQUIRE(history->head() > 0);
    const std::uint64_t firstLine = history->head() - 1;
    HistoryPublisher publisher;
    std::vector<std::uint8_t> historyBody;
    REQUIRE(publisher.capture(*source));
    REQUIRE(publisher.writeBody(*source, { firstLine, firstLine + 1 }, HistoryOrder::Ascending, 4'096, historyBody) == 1);
    std::vector<std::uint8_t> inlineBody;
    encode(inlineBody, HistoryInlineHeader { .historyEpoch = history->key().epoch, .firstLine = firstLine });
    inlineBody.insert(inlineBody.end(), historyBody.begin(), historyBody.end());
    bounded.client.receiveStream(frameOf(FrameKind::HistoryInline, 1, inlineBody), 4);
    CHECK(bounded.client.state() == ConnectionState::Attached);
    CHECK_EQ(bounded.client.counters().historyTrimsForReplicas, 1u);
    CHECK_EQ(history->lineCount(), 0u);
    CHECK(bounded.client.retainedReplicaBytes() <= tight.maxRetainedReplicaBytes);
}

TEST(aHistoryCacheEvictsByBytesOnLiveIngestAsWellAsByLines)
{
    // One wide line's storage, then a cache with room for ten of them and a
    // line cap far above that.
    DecodedLine line;
    line.cells.resize(1'000);
    const std::size_t lineBytes = HistoryCache::lineStorageBytes(line);
    HistoryCache cache({ 1, 2 }, { 3, 4 }, 10'000, 10 * lineBytes);
    for (std::uint64_t number = 0; number < 100; ++number) {
        line.number = number;
        REQUIRE(cache.insert(1, std::span(&line, 1)) == CacheInsert::Stored);
        CHECK(cache.retainedStorageBytes() <= 10 * lineBytes);
    }
    CHECK_EQ(cache.lineCount(), 10u);
    CHECK(!cache.line(89));
    CHECK(cache.line(90));
    CHECK(cache.line(99));
    CHECK_EQ(cache.retainedStorageBytes(), 10 * lineBytes);
    // A replica owner with no room left trims further, oldest first.
    cache.trimToBytes(4 * lineBytes);
    CHECK_EQ(cache.lineCount(), 4u);
    CHECK(cache.line(96));
    CHECK(!cache.line(95));
}

TEST(anImageTransferWaitsWhenTheReceiveOwnerHasNoRoomForItsPixels)
{
    // A checkpoint assembly on one pane leaves the owner less room than the
    // other pane's image needs, though the catalog and image allowances and
    // the terminal's own budget all admit it.
    const std::vector<std::uint8_t> chunk(60'000, 0x5A);
    std::vector<std::uint8_t> pixels(16 * 16 * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = static_cast<std::uint8_t>(index * 7);
    const ImagePlacements state = imageState(pixels, 16, 9);
    REQUIRE(state.imageCount == 1);
    ClientPolicy policy = smallPolicy();
    policy.maxReceiveBytes = chunk.size() + 500;
    ClientConnection client(policy, identityFixture(), nullptr, nullptr, nullptr);
    REQUIRE(welcome(client));
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 1, .paneID = firstPaneID }), 2);
    client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = 2, .paneID = secondPaneID }), 2);
    announceCheckpoint(client, 2, 800, chunk);
    REQUIRE(client.receiveMemory().heldBytes() >= chunk.size());
    client.receiveStream(messageFrame(FrameKind::ImagePlacements, 1, state), 3);
    client.receiveStream(messageFrame(FrameKind::ImageHead, 1, ImageHead { .asset = state.images[0].asset, .transfer = 801 }), 3);
    CHECK(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.inboundImageBytes(), 0u);
    CHECK(client.receiveMemory().heldBytes() <= policy.maxReceiveBytes);
    BulkChunk pixelChunk { .transfer = 801, .index = 0, .isLast = true, .bytes = pixels };
    Ubiquitty::Crypto::sha256(pixels, pixelChunk.sha256);
    client.receiveStream(messageFrame(FrameKind::BulkChunk, 0, pixelChunk), 3);
    CHECK(client.state() == ConnectionState::Attached);
    CHECK_EQ(client.installedImageBytes(), 0u);
}

TEST(theHostDecodeOwnerBoundsTwoConcurrentAdoptCandidates)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 24, 80);
    UbiquittyTest::feed(*terminal, "a held adopt");
    const auto limits = Ubiquitty::Session::localShareTerminalLimits(Ubiquitty::Protocol::Local::maxAdoptBodyBytes);
    const auto encoded = encodeHandoverTerminal(*terminal, 1, limits);
    REQUIRE(encoded);
    // Measure one candidate's charge, then give the owner room for one and a half.
    auto measuring = std::make_shared<MemoryBudget>(std::size_t { 1 } << 40);
    std::size_t oneCandidate = 0;
    {
        auto measured = decodeHandoverTerminal(encoded->view(), limits, measuring);
        REQUIRE(measured);
        oneCandidate = measured->decodeCharge.bytes();
        CHECK(oneCandidate > 0);
        CHECK_EQ(measuring->heldBytes(), oneCandidate);
    }
    CHECK_EQ(measuring->heldBytes(), 0u);
    const auto decodeMemory = std::make_shared<MemoryBudget>(oneCandidate + oneCandidate / 2);
    auto first = decodeHandoverTerminal(encoded->view(), limits, decodeMemory);
    REQUIRE(first);
    // The second candidate waits on the first: refused, as NoMemory, not built.
    const auto second = decodeHandoverTerminal(encoded->view(), limits, decodeMemory);
    REQUIRE(!second);
    CHECK(second.error() == HandoverBodyError::AllocationRefused);
    CHECK_EQ(decodeMemory->heldBytes(), oneCandidate);
    // Once the first is published or dropped, the next is admitted.
    first->decodeCharge = MemoryCharge();
    const auto third = decodeHandoverTerminal(encoded->view(), limits, decodeMemory);
    CHECK(third.has_value());
}

TEST(aChargeAdmitsGrowthOnlyWithinItsOwnerAndReleasesOnDestruction)
{
    auto budget = std::make_shared<MemoryBudget>(100);
    MemoryCharge first(budget);
    CHECK(first.resize(60));
    {
        MemoryCharge second(budget);
        CHECK(!second.resize(41));
        CHECK(second.resize(40));
        CHECK_EQ(budget->availableBytes(), 0u);
        MemoryCharge moved = std::move(second);
        CHECK_EQ(second.bytes(), 0u);
        CHECK_EQ(moved.bytes(), 40u);
    }
    CHECK_EQ(budget->heldBytes(), 60u);
    CHECK(first.resize(10));
    CHECK_EQ(budget->heldBytes(), 10u);
    CHECK(!MemoryCharge().resize(1));
}

TEST(aReassemblerChargesItsOwnerAndRefusesAChunkPastIt)
{
    auto budget = std::make_shared<MemoryBudget>(100'000);
    BulkReassembler receiver(budget);
    const std::vector<std::uint8_t> bytes(60'000, 1);
    REQUIRE(receiver.expect(1, 1 << 20));
    REQUIRE(receiver.expect(2, 1 << 20));
    CHECK(receiver.accept({ .transfer = 1, .index = 0, .isLast = false, .bytes = bytes }) == ChunkOutcome::Pending);
    CHECK_EQ(budget->heldBytes(), receiver.heldBytes());
    CHECK(receiver.accept({ .transfer = 2, .index = 0, .isLast = false, .bytes = bytes }) == ChunkOutcome::OverBudget);
    CHECK(receiver.isFenced(2));
    CHECK(receiver.accept({ .transfer = 2, .index = 1, .isLast = true, .bytes = bytes }) == ChunkOutcome::Discarded);
    receiver.abandon(1);
    CHECK_EQ(budget->heldBytes(), 0u);
}

TEST(theMemoryDefaultsAreTheOnesTheMeasurementsRecord)
{
    const auto recorded = recordedMemoryConstants();
    const ClientPolicy client = measuredClientPolicy();
    const HostPolicy host = measuredHostPolicy(0);
    const auto pin = [&](const char* name, std::uint64_t value) {
        const auto found = recorded.find(name);
        CHECK(found != recorded.end());
        if (found != recorded.end())
            CHECK_EQ(found->second, value);
    };
    pin("maxCheckpointStorageCost", client.maxCheckpointStorageCost);
    CHECK_EQ(client.maxCheckpointStorageCost, std::size_t { 128 } << 20);
    pin("maxDecodedScreenCellBytes", client.maxDecodedScreenCellBytes);
    pin("maxCheckpointBytes", client.maxCheckpointBytes);
    pin("hostMaxCheckpointBytes", host.maxCheckpointBytes);
    pin("maxReceiveBytes", client.maxReceiveBytes);
    pin("maxInstalledImageBytes", client.maxInstalledImageBytes);
    pin("historyCacheBytes", client.historyCacheBytes);
    pin("maxRetainedReplicaBytes", client.maxRetainedReplicaBytes);
    pin("maxHostDecodeBytes", defaultMaxHostDecodeBytes);
    pin("maxCatalogMessageBytes", maxCatalogMessageBytes - catalogFrameHeaderBytes);
    // The owner admits one checkpoint deflated and inflated, its preparation,
    // and the image and catalog allowances at once.
    CHECK(client.maxReceiveBytes >= 2 * client.maxCheckpointBytes + client.maxCheckpointStorageCost + client.maxInstalledImageBytes + maxCatalogMessageBytes);
    CHECK(host.maxCheckpointBytes <= client.maxCheckpointBytes);
}

UBIQUITTY_TEST_MAIN()
