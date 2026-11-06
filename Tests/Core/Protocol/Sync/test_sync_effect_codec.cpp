// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

static_assert(maxEffectClipboardReadBytes == Protocol::Sync::maxClipboardServeBytes);
static_assert(maxEffectClipboardItems == Protocol::Sync::maxClipboardServeItems);

namespace {

constexpr ID128 pane { 4, 7 };
constexpr ID128 person { 2, 9 };

EffectClipboardBundle bundleOf(std::span<const std::uint8_t> bytes, std::string_view mime = "text/plain")
{
    EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { mime, bytes };
    return bundle;
}

EffectClipboardPayload inlinePayload(const EffectCodecBytes& bytes)
{
    return { 0, static_cast<std::uint32_t>(bytes.size), bytes.view() };
}

std::vector<std::uint8_t> copy(std::span<const std::uint8_t> bytes)
{
    return { bytes.begin(), bytes.end() };
}

EffectView viewOf(const EffectCodecBytes& bytes, EffectKind kind, std::uint8_t target = 0)
{
    return { .paneID = pane, .generation = 3, .sequence = 8, .primary = person, .kind = kind, .count = 1, .target = target, .payload = bytes.view() };
}

void roundTrip(const EffectBody& body)
{
    const auto encoded = encodeEffectBody(body);
    REQUIRE(encoded);
    const auto decoded = decodeEffectBody(effectKind(body), encoded->view());
    REQUIRE(decoded);
    CHECK(effectKind(*decoded) == effectKind(body));
    const auto canonical = encodeEffectBody(*decoded);
    REQUIRE(canonical);
    CHECK(std::ranges::equal(encoded->view(), canonical->view()));
    for (std::size_t count = 0; count < encoded->size; ++count)
        CHECK(!decodeEffectBody(effectKind(body), encoded->view().first(count)));
    auto extra = copy(encoded->view());
    extra.push_back(0);
    CHECK(!decodeEffectBody(effectKind(body), extra));
}

} // namespace

TEST(everyEffectKindHasOneCompleteCanonicalBody)
{
    roundTrip(BellEffect {});
    roundTrip(AttentionEffect {});
    roundTrip(NotificationEffect { "Ready", "Build finished ✓", "build.1", NotificationOccasion::Unfocused });
    roundTrip(CommandFinishedEffect { -17, 9'123'456, "/tmp/build ✓", "printf 'done\\n'" });
    SoundEffect sound;
    auto& request = sound.request;
    request.volume = 7;
    request.wave = 6;
    request.channel = 8;
    request.attackMilliseconds = 5000;
    request.releaseMilliseconds = 65535;
    request.semitonesDown = 48;
    request.durationMilliseconds = 7968;
    request.gapMilliseconds = 17;
    request.sampleSlot = 25;
    request.decayMilliseconds = 4999;
    request.sustainLevel = 6;
    request.reverbSend = 5;
    request.gridUnits = 255;
    request.lowpassHertz = 19000;
    request.highpassHertz = 100;
    request.pan = 64;
    request.repeat = 255;
    request.count = PAL::PlayTonesRequest::maxNotes;
    for (int index = 0; index < request.count; ++index) {
        request.freqs[index] = 440 + index;
        request.noteVolumes[index] = index % 8;
        request.noteEffects[index] = index;
        request.noteParameters[index] = 255 - index;
    }
    roundTrip(sound);
    const auto clipboard = encodeEffectClipboard({}, maxEffectClipboardWriteBytes);
    REQUIRE(clipboard);
    roundTrip(ClipboardWriteEffect { ClipboardBuffer::Cut7, inlinePayload(*clipboard) });
}

TEST(effectHeadersPreserveIdentityPrimaryAndCoalescedCount)
{
    const auto body = encodeEffectBody(BellEffect {});
    REQUIRE(body);
    auto view = viewOf(*body, EffectKind::Bell);
    view.generation = 0;
    view.count = 19;
    const auto encoded = encodeEffect(view);
    REQUIRE(encoded);
    CHECK_EQ(encoded->size, effectRecordHeaderBytes);
    CHECK_EQ(effectRecordHeaderBytes, std::size_t { 37 });
    CHECK(encoded->view()[8] == 8);
    CHECK(encoded->view()[16] == 2);
    CHECK(encoded->view()[24] == 9);
    CHECK(encoded->view()[33] == 19);
    const auto decoded = decodeEffect(pane, encoded->view());
    REQUIRE(decoded);
    CHECK(decoded->paneID == pane);
    CHECK(decoded->generation == 0);
    CHECK(decoded->sequence == view.sequence);
    CHECK(decoded->primary == person);
    CHECK(decoded->count == 19);
    CHECK(decoded->payload.empty());
    CHECK(!decodeEffect({}, encoded->view()));
    view.primary = {};
    CHECK(encodeEffect(view));
    view.sequence = 0;
    CHECK(!encodeEffect(view));
    view.sequence = 1;
    view.count = 0;
    CHECK(!encodeEffect(view));
    view.count = 1;
    view.paneID = {};
    CHECK(!encodeEffect(view));
    for (std::size_t count = 0; count < encoded->size; ++count)
        CHECK(!decodeEffect(pane, encoded->view().first(count)));
    for (const std::size_t offset : { 8, 33 }) {
        auto changed = copy(encoded->view());
        changed[offset] = 0;
        CHECK(!decodeEffect(pane, changed));
    }
    auto changed = copy(encoded->view());
    changed[32] = 255;
    CHECK(!decodeEffect(pane, changed));
    changed = copy(encoded->view());
    changed.push_back(0);
    CHECK(!decodeEffect(pane, changed));
}

TEST(notificationBoundsAndUTF8ApplyToEveryField)
{
    const std::string maximum(255, 'x');
    const std::string identifier(32, 'i');
    roundTrip(NotificationEffect { maximum, maximum, identifier, NotificationOccasion::Invisible });
    const std::string over(256, 'x');
    CHECK(!encodeEffectBody(NotificationEffect { over, "", "" }));
    CHECK(!encodeEffectBody(NotificationEffect { "", over, "" }));
    CHECK(!encodeEffectBody(NotificationEffect { "", "", std::string(33, 'i') }));
    const std::string invalid("\xED\xA0\x80", 3);
    CHECK(!encodeEffectBody(NotificationEffect { invalid, "", "" }));
    CHECK(!encodeEffectBody(NotificationEffect { "", invalid, "" }));
    CHECK(!encodeEffectBody(NotificationEffect { "", "", invalid }));
    for (const int field : { 0, 1, 2 }) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        for (int index = 0; index < 3; ++index)
            writer.utf8(index == field ? invalid : "ok");
        writer.u8(0);
        CHECK(!decodeEffectBody(EffectKind::Notification, bytes));
    }
    const auto valid = encodeEffectBody(NotificationEffect {});
    REQUIRE(valid);
    auto bytes = copy(valid->view());
    bytes.back() = 3;
    CHECK(!decodeEffectBody(EffectKind::Notification, bytes));
    CHECK(!encodeEffectBody(NotificationEffect { "", "", "", static_cast<NotificationOccasion>(3) }));
}

TEST(commandStatusCannotNarrowOrUseAnOverlongInteger)
{
    roundTrip(CommandFinishedEffect { std::numeric_limits<std::int32_t>::min(), 0 });
    roundTrip(CommandFinishedEffect { std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::uint64_t>::max() });
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writer.svarint(std::int64_t { 1 } << 32);
    writer.u64(0);
    writer.utf8("");
    writer.utf8("");
    CHECK(!decodeEffectBody(EffectKind::CommandFinished, bytes));
    bytes = { 0x80, 0 };
    bytes.resize(12);
    CHECK(!decodeEffectBody(EffectKind::CommandFinished, bytes));
}

TEST(commandContextRetainsExistingBoundsAndRejectsMalformedText)
{
    const std::string directory(Terminal::maxCurrentDirectoryLength, 'd');
    const std::string line(Terminal::maxCommandLineLength, 'c');
    const auto encoded = encodeEffectBody(CommandFinishedEffect { 23, 91, directory, line });
    REQUIRE(encoded);
    const auto decoded = decodeEffectBody(EffectKind::CommandFinished, encoded->view());
    REQUIRE(decoded);
    const auto& command = std::get<CommandFinishedEffect>(*decoded);
    CHECK(command.exitStatus == 23);
    CHECK(command.durationMicroseconds == 91);
    CHECK(command.currentDirectory == directory);
    CHECK(command.commandLine == line);
    const std::string invalid("\xF4\x90\x80\x80", 4);
    for (const auto& badDirectory : { directory + 'x', invalid }) {
        CHECK(!encodeEffectBody(CommandFinishedEffect { 0, 0, badDirectory, line }));
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.svarint(0);
        writer.u64(0);
        writer.utf8(badDirectory);
        writer.utf8(line);
        CHECK(!decodeEffectBody(EffectKind::CommandFinished, bytes));
    }
    for (const auto& badLine : { line + 'x', invalid }) {
        CHECK(!encodeEffectBody(CommandFinishedEffect { 0, 0, directory, badLine }));
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.svarint(0);
        writer.u64(0);
        writer.utf8(directory);
        writer.utf8(badLine);
        CHECK(!decodeEffectBody(EffectKind::CommandFinished, bytes));
    }
}

TEST(soundRejectsOutOfRangeParametersAndNeverCopiesPCM)
{
    SoundEffect sound;
    sound.request.wave = 7;
    CHECK(!encodeEffectBody(sound));
    sound.request.wave = 0;
    sound.request.count = PAL::PlayTonesRequest::maxNotes + 1;
    CHECK(!encodeEffectBody(sound));
    sound.request.count = -1;
    CHECK(!encodeEffectBody(sound));
    sound.request.count = 0;
    const std::int16_t sample = 1;
    sound.request.samplePCM = &sample;
    CHECK(!encodeEffectBody(sound));
    sound.request.samplePCM = nullptr;
    sound.request.sampleFrames = 1;
    CHECK(!encodeEffectBody(sound));
    sound.request.sampleFrames = 0;
    const auto valid = encodeEffectBody(sound);
    REQUIRE(valid);
    auto bytes = copy(valid->view());
    bytes[1] = 7;
    CHECK(!decodeEffectBody(EffectKind::Sound, bytes));
    bytes = copy(valid->view());
    bytes.back() = 17;
    CHECK(!decodeEffectBody(EffectKind::Sound, bytes));
    bytes = copy(valid->view());
    bytes[0] = 8;
    CHECK(!decodeEffectBody(EffectKind::Sound, bytes));
    sound.request.pan = 65;
    CHECK(!encodeEffectBody(sound));
}

TEST(soundBodyPreservesEveryRequestField)
{
    SoundEffect sound;
    auto& request = sound.request;
    request.volume = 5;
    request.wave = 6;
    request.channel = 3;
    request.attackMilliseconds = 12;
    request.releaseMilliseconds = 34;
    request.semitonesDown = 24;
    request.durationMilliseconds = 250;
    request.gapMilliseconds = 31;
    request.sampleSlot = 7;
    request.count = 3;
    request.freqs[0] = 523;
    request.freqs[1] = 0;
    request.freqs[2] = 2093;
    request.noteVolumes[2] = 7;
    request.noteEffects[1] = 4;
    request.noteParameters[1] = 0x24;
    request.decayMilliseconds = 120;
    request.sustainLevel = 3;
    request.reverbSend = 5;
    request.gridUnits = 64;
    request.lowpassHertz = 800;
    request.highpassHertz = 120;
    request.pan = 27;
    request.repeat = 2;
    const auto encoded = encodeEffectBody(sound);
    REQUIRE(encoded);
    const auto decoded = decodeEffectBody(EffectKind::Sound, encoded->view());
    REQUIRE(decoded);
    const auto& received = std::get<SoundEffect>(*decoded).request;
    CHECK(received.volume == 5 && received.wave == 6 && received.channel == 3);
    CHECK(received.attackMilliseconds == 12 && received.releaseMilliseconds == 34);
    CHECK(received.semitonesDown == 24 && received.durationMilliseconds == 250 && received.gapMilliseconds == 31);
    CHECK(received.sampleSlot == 7 && received.count == 3);
    CHECK(received.freqs[0] == 523 && received.freqs[1] == 0 && received.freqs[2] == 2093);
    CHECK(received.noteVolumes[0] == 0 && received.noteVolumes[2] == 7);
    CHECK(received.noteEffects[1] == 4 && received.noteParameters[1] == 0x24);
    CHECK(received.decayMilliseconds == 120 && received.sustainLevel == 3);
    CHECK(received.reverbSend == 5 && received.gridUnits == 64);
    CHECK(received.lowpassHertz == 800 && received.highpassHertz == 120);
    CHECK(received.pan == 27 && received.repeat == 2);
    CHECK(received.samplePCM == nullptr && received.sampleFrames == 0u);

    // A complete oversized note list must fail at the count's cap, not
    // merely because a truncated body runs out before its last note.
    constexpr std::size_t bytesPerNote = 7;
    const auto countOffset = encoded->size - sampleIdentityBytes - request.count * bytesPerNote - 1;
    const std::size_t pastTheLimit = PAL::PlayTonesRequest::maxNotes + 1;
    auto tooManyNotes = copy(encoded->view().first(countOffset + 1));
    tooManyNotes[countOffset] = static_cast<std::uint8_t>(pastTheLimit);
    tooManyNotes.resize(countOffset + 1 + pastTheLimit * bytesPerNote + sampleIdentityBytes, 0);
    CHECK(!decodeEffectBody(EffectKind::Sound, tooManyNotes));
}

TEST(sampledSoundCarriesTheExactBankIdentity)
{
    constexpr std::array<std::int16_t, 3> pcm { -32768, 0, 32767 };
    auto captured = SampleBytes::capture(9, pcm, pcm.size() * 2);
    REQUIRE(captured);
    SoundEffect sound;
    sound.request.wave = 6;
    sound.request.sampleSlot = 2;
    sound.sample = captured->sample;
    roundTrip(sound);
    const auto encoded = encodeEffectBody(sound);
    REQUIRE(encoded);
    CHECK_EQ(encoded->size, 29u + sampleIdentityBytes);
    const auto decoded = decodeEffectBody(EffectKind::Sound, encoded->view());
    REQUIRE(decoded);
    CHECK(std::get<SoundEffect>(*decoded).sample == captured->sample);
    sound.request.wave = 0;
    CHECK(!encodeEffectBody(sound));
    sound.request.wave = 6;
    sound.request.sampleSlot = 0;
    CHECK(!encodeEffectBody(sound));
    sound.request.sampleSlot = 2;
    sound.sample.frames = Ubiquitty::VT::sampleMaxFrames + 1;
    CHECK(!encodeEffectBody(sound));
    sound.sample.frames = 0;
    CHECK(!encodeEffectBody(sound)); // empty identity cannot carry a stale digest
    sound.sample = {};
    roundTrip(sound);
}

TEST(clipboardBundlesRetainBinaryMIMEsAndRefuseAmbiguousNames)
{
    const std::array<std::uint8_t, 4> binary { 0, 0xFF, 1, 0x80 };
    auto bundle = bundleOf(binary, "image/png");
    bundle.count = 2;
    bundle.items[1] = { "text/plain;charset=utf-8", {} };
    const auto encoded = encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes);
    REQUIRE(encoded);
    const auto decoded = decodeEffectClipboard(encoded->view(), maxEffectClipboardWriteBytes);
    REQUIRE(decoded);
    CHECK(decoded->count == 2);
    CHECK(decoded->items[0].mime == "image/png");
    CHECK(std::ranges::equal(decoded->items[0].data, binary));
    CHECK(decoded->items[1].data.empty());
    for (std::size_t count = 0; count < encoded->size; ++count)
        CHECK(!decodeEffectClipboard(encoded->view().first(count), maxEffectClipboardWriteBytes));
    auto extra = copy(encoded->view());
    extra.push_back(0);
    CHECK(!decodeEffectClipboard(extra, maxEffectClipboardWriteBytes));
    bundle.items[1].mime = bundle.items[0].mime;
    CHECK(!encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes));
    for (const std::string& mime : { std::string(), std::string("a\0b", 3), std::string("\xC0\xAF", 2) }) {
        bundle.count = 1;
        bundle.items[0].mime = mime;
        CHECK(!encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes));
    }
    const std::array<std::uint8_t, 1> tooMany { 33 };
    CHECK(!decodeEffectClipboard(tooMany, maxEffectClipboardWriteBytes));
    std::vector<std::uint8_t> duplicate;
    WireWriter writer(duplicate);
    writer.varint(2);
    for (int index = 0; index < 2; ++index) {
        writer.utf8("text/plain");
        writer.bytes({});
    }
    CHECK(!decodeEffectClipboard(duplicate, maxEffectClipboardWriteBytes));
}

TEST(clipboardDataBudgetsAreAggregateAndCheckedBeforeAllocation)
{
    std::vector<std::uint8_t> data(maxEffectClipboardReadBytes, 0x61);
    const auto written = encodeEffectClipboard(bundleOf(std::span(data).first(maxEffectClipboardWriteBytes)), maxEffectClipboardWriteBytes);
    REQUIRE(written);
    CHECK(decodeEffectClipboard(written->view(), maxEffectClipboardWriteBytes));
    const auto read = encodeEffectClipboard(bundleOf(data), maxEffectClipboardReadBytes);
    REQUIRE(read);
    CHECK(decodeEffectClipboard(read->view(), maxEffectClipboardReadBytes));
    CHECK(!decodeEffectClipboard(read->view(), maxEffectClipboardWriteBytes));
    auto bundle = bundleOf(std::span(data).first(maxEffectClipboardWriteBytes));
    bundle.count = 2;
    bundle.items[1] = { "application/octet-stream", std::span(data).first(1) };
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    CHECK(!encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes));
    CHECK(!encodeEffectClipboard(bundle, std::numeric_limits<std::size_t>::max()));
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    CHECK(decodeEffectClipboard(read->view(), maxEffectClipboardReadBytes));
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
}

TEST(clipboardMIMEMetadataIsBoundedBeforeAnyOutputAllocation)
{
    const std::string maximum(maxEffectClipboardMIMEBytes, 'm');
    const auto encoded = encodeEffectClipboard(bundleOf({}, maximum), 0);
    REQUIRE(encoded);
    CHECK(decodeEffectClipboard(encoded->view(), 0));
    const std::string excess(maxEffectClipboardMIMEBytes + 1, 'm');
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    CHECK(!encodeEffectClipboard(bundleOf({}, excess), 0));
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
}

TEST(clipboardWritesFitOneFrameOrExplicitlyNameABoundedTransfer)
{
    // Fixed header 37, buffer 1, descriptor 8, one MIME and data prefixes 15.
    std::vector<std::uint8_t> data(maxFrameBodyBytes - 61, 0x61);
    const auto bundle = encodeEffectClipboard(bundleOf(data), maxEffectClipboardWriteBytes);
    REQUIRE(bundle);
    const auto body = encodeEffectBody(ClipboardWriteEffect { ClipboardBuffer::Cut3, inlinePayload(*bundle) });
    REQUIRE(body);
    const auto frame = encodeEffect(viewOf(*body, EffectKind::ClipboardWrite, std::to_underlying(ClipboardBuffer::Cut3)));
    REQUIRE(frame);
    CHECK_EQ(frame->size, maxFrameBodyBytes);
    const auto decoded = decodeEffect(pane, frame->view());
    REQUIRE(decoded);
    CHECK(decoded->target == std::to_underlying(ClipboardBuffer::Cut3));
    CHECK(!encodeEffect(viewOf(*body, EffectKind::ClipboardWrite)));
    data.push_back(0);
    const auto larger = encodeEffectClipboard(bundleOf(data), maxEffectClipboardWriteBytes);
    REQUIRE(larger);
    CHECK(!encodeEffectBody(ClipboardWriteEffect { ClipboardBuffer::Clipboard, inlinePayload(*larger) }));
    ClipboardWriteEffect transferred { ClipboardBuffer::Clipboard, { 17, static_cast<std::uint32_t>(larger->size), {} } };
    roundTrip(transferred);
    transferred.payload.inlineBytes = larger->view();
    CHECK(!encodeEffectBody(transferred));
    transferred.payload.inlineBytes = {};
    transferred.payload.encodedBytes = std::numeric_limits<std::uint32_t>::max();
    CHECK(!encodeEffectBody(transferred));
    transferred.payload.encodedBytes = 0;
    CHECK(!encodeEffectBody(transferred));
}

TEST(clipboardRequestsKeepEveryBufferAndExactTypedOperation)
{
    for (std::uint8_t buffer = 0; buffer < std::to_underlying(ClipboardBuffer::Count); ++buffer) {
        for (const auto kind : { EffectClipboardRequestKind::Types, EffectClipboardRequestKind::Item }) {
            const ClipboardRead request { 7, static_cast<ClipboardBuffer>(buffer), kind,
                kind == EffectClipboardRequestKind::Types ? "" : "image/png" };
            const auto encoded = encodeClipboardRead(request);
            REQUIRE(encoded);
            const auto decoded = decodeClipboardRead(encoded->view());
            REQUIRE(decoded);
            CHECK(decoded->requestID == 7);
            CHECK(decoded->buffer == request.buffer);
            CHECK(decoded->kind == request.kind);
            CHECK(decoded->mime == request.mime);
            for (std::size_t count = 0; count < encoded->size; ++count)
                CHECK(!decodeClipboardRead(encoded->view().first(count)));
            auto extra = copy(encoded->view());
            extra.push_back(0);
            CHECK(!decodeClipboardRead(extra));
        }
    }
    CHECK(!encodeClipboardRead({}));
    CHECK(!encodeClipboardRead({ 1, ClipboardBuffer::Count, EffectClipboardRequestKind::Types, {} }));
    CHECK(!encodeClipboardRead({ 1, ClipboardBuffer::Clipboard, EffectClipboardRequestKind::Types, "text/plain" }));
    CHECK(!encodeClipboardRead({ 1, ClipboardBuffer::Clipboard, EffectClipboardRequestKind::Item, "" }));
    CHECK(!encodeClipboardRead({ 1, ClipboardBuffer::Clipboard, static_cast<EffectClipboardRequestKind>(2), {} }));
    const auto valid = encodeClipboardRead({ 1, ClipboardBuffer::Clipboard, EffectClipboardRequestKind::Types, {} });
    REQUIRE(valid);
    for (const std::size_t offset : { 4, 5 }) {
        auto bad = copy(valid->view());
        bad[offset] = 255;
        CHECK(!decodeClipboardRead(bad));
    }
}

TEST(clipboardAnswersPreserveRefusalAndNeverSmugglePayloadOnFailure)
{
    const auto bundle = encodeEffectClipboard({}, maxEffectClipboardReadBytes);
    REQUIRE(bundle);
    for (const auto status : { EffectClipboardStatus::OK, EffectClipboardStatus::Empty, EffectClipboardStatus::Refused, EffectClipboardStatus::TooLarge }) {
        ClipboardData answer { 9, status, {} };
        if (status == EffectClipboardStatus::OK)
            answer.payload = inlinePayload(*bundle);
        const auto encoded = encodeClipboardData(answer);
        REQUIRE(encoded);
        const auto decoded = decodeClipboardData(encoded->view());
        REQUIRE(decoded);
        CHECK(decoded->requestID == 9);
        CHECK(decoded->status == status);
        for (std::size_t count = 0; count < encoded->size; ++count)
            CHECK(!decodeClipboardData(encoded->view().first(count)));
        auto extra = copy(encoded->view());
        extra.push_back(0);
        CHECK(!decodeClipboardData(extra));
        if (status != EffectClipboardStatus::OK) {
            answer.payload = inlinePayload(*bundle);
            CHECK(!encodeClipboardData(answer));
        }
    }
    CHECK(!encodeClipboardData({}));
    CHECK(!encodeClipboardData({ 1, static_cast<EffectClipboardStatus>(4), {} }));
    const auto transferred = encodeClipboardData({ 10, EffectClipboardStatus::OK, { 2, static_cast<std::uint32_t>(bundle->size), {} } });
    REQUIRE(transferred);
    CHECK(decodeClipboardData(transferred->view()));
    auto bad = copy(transferred->view());
    bad[4] = 255;
    CHECK(!decodeClipboardData(bad));
    std::fill(bad.begin(), bad.begin() + 4, 0);
    CHECK(!decodeClipboardData(bad));
}

TEST(logIdentitiesAndCoalescingSurviveTheTypedWireBoundary)
{
    const auto body = encodeEffectBody(BellEffect {});
    REQUIRE(body);
    EffectLog log(pane);
    REQUIRE(log.append({ .generation = 1, .primary = person, .kind = EffectKind::Bell, .payload = body->view() }));
    REQUIRE(log.append({ .generation = 1, .primary = person, .kind = EffectKind::Bell, .payload = body->view() }));
    const auto batch = log.readAfter(0, EffectReadMode::Skipped);
    REQUIRE(batch);
    REQUIRE(batch->size == 1);
    const auto frame = encodeEffect(batch->effects()[0]);
    REQUIRE(frame);
    const auto decoded = decodeEffect(pane, frame->view());
    REQUIRE(decoded);
    CHECK(decoded->sequence == 2);
    CHECK(decoded->generation == 1);
    CHECK(decoded->count == 2);
    CHECK(decoded->primary == person);
}

TEST(outputAllocationRefusalReturnsNoPartialEncoding)
{
    const std::array<std::uint8_t, 3> body { 1, 'x', 0 };
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    const auto failed = encodeEffectBody(BellEffect {});
    REQUIRE(!failed);
    CHECK(failed.error() == EffectCodecError::NoMemory);
    CHECK(!encodeClipboardRead({ 1, ClipboardBuffer::Clipboard, EffectClipboardRequestKind::Types, {} }));
    CHECK(!encodeClipboardData({ 1, EffectClipboardStatus::Empty, {} }));
    CHECK(!encodeEffectClipboard(bundleOf(body), maxEffectClipboardWriteBytes));
    CHECK(!decodeEffectBody(static_cast<EffectKind>(255), {}));
}

UBIQUITTY_TEST_MAIN()
