// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_sync.cpp — libFuzzer harness for canonical Sync frames and local
// terminal handoff payloads.
//
// The remote-Core/SSH split parses frames off a socket: arbitrary,
// potentially hostile bytes. This harness drives fuzzer input through
// every decode/apply path and asserts the decoder never reads OOB /
// crashes / triggers UB (caught by -fsanitize=fuzzer,address) — it must
// either cleanly reject (return false / fault) or yield a
// structurally-valid result.
//
// The first input byte selects a path so the fuzzer reaches the deep
// decoders without first satisfying an envelope header:
//   - canonical FrameIngress over split headers and bodies, with bounded
//     allocation before any current typed decoder sees the body;
//   - Greet / Welcome / Bye and native clipboard bodies directly;
//   - a local share's whole-terminal body, as the daemon admits the one
//     local descriptor handoff carries;
//   - canonical ImageHead / ImageWant / ImagePlacements bodies and bounded
//     mutations of valid image records and a 16-byte raw pixel transfer.
//     Canonical Calls and Results also reach the capture and file readers.
//   - canonical consent Calls and consent/presence notifications, both as
//     complete inner streams and bounded mutations of valid records.
//
// On an applied checkpoint the passive Ubiquitty::Terminal must satisfy the
// same structural invariants the renderer relies on; after a typed image
// record or pixel owner applies, the image store must also stay within its
// pixel-byte budget and slot/placement caps. Hooks up the same
// shape as Tests/Core/fuzz_vt100.cpp; build via
// Scripts/run-tests.sh --with-fuzz; the seed corpus under
// Tests/Sync/fuzz-corpus/ replays first, then libFuzzer explores.

#include "ConsentFuzz.h"

#include "Core/CellGrid.h"
#include "Core/Crypto/SHA256.h"
#include "Core/GridBounds.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Control/CaptureReply.h"
#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <stddef.h>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

// A passive Ubiquitty::Terminal is large (init news its CellGrid); reuse one per process.
static Ubiquitty::Terminal& term()
{
    static Ubiquitty::Terminal shared;
    return shared;
}

// A current message the decoder accepted must survive its own encoder. Encoding the
// decoded value and decoding that again has to reproduce the same bytes: a
// decoder reading a field from the wrong offset still returns true, and a
// corpus that discards the result cannot tell that from a correct parse. The
// second encoding can.
//
// This is a floor rather than a proof. A field the decoder ignores entirely is
// absent from both encodings and agrees with itself, so this bounds the fields
// that round-trip and says nothing about the ones that are dropped.
template<typename Message, typename Decode>
static void checkRoundTrip(const Message& message, Decode decode)
{
    std::vector<uint8_t> first;
    Ubiquitty::Protocol::Sync::encode(first, message);
    if (first.empty())
        return;

    // A payload this encoder produced is one its own decoder must accept. A
    // refusal means the pair disagree about the wire format, which no corpus
    // input can show on its own — arbitrary bytes are expected to be rejected,
    // so a rejection there carries no information.
    const auto again = decode(first);
    assert(again.has_value());

    std::vector<uint8_t> second;
    Ubiquitty::Protocol::Sync::encode(second, *again);
    assert(first == second);
}

// An applied checkpoint must leave the passive Ubiquitty::Terminal in a state
// the renderer can walk without OOB — the same contract fuzz_vt100 pins.
static void checkTermSanity(const Ubiquitty::Terminal& t)
{
    assert(t.rows > 0 && t.rows <= Ubiquitty::maxRows);
    assert(t.columns > 0 && t.columns <= Ubiquitty::maxColumns);
    assert(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
    assert(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < t.columns);
    assert(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < t.rows);
    assert(t.linkCount >= 0 && t.linkCount <= Ubiquitty::VT::linkTableCapacity);
    assert(t.clusterPool.size() >= 0 && t.clusterPool.size() <= Ubiquitty::ClusterPool::maxEntries);
    // Every visible cell must resolve (no OOB on the grid the renderer scans).
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c)
            (void)Ubiquitty::Screen::visibleCell(&t, 0, r, c);
}

// After a typed image record applies the passive store must still honor its
// bounds: at most maxTerminalImageCount used images whose pixel bytes stay within the
// per-Ubiquitty::Terminal budget, and at most maxTerminalImagePlacementCount live placements. A
// decoder that let attacker bytes pin unbounded pixels or placements past the cap
// is the exact OOM the Sync split must never admit.
static void checkImageStoreCapacity(const Ubiquitty::Terminal& t)
{
    uint64_t usedBytes = 0;
    int usedImages = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        const Ubiquitty::Image::TerminalImage* image = Ubiquitty::Image::stored(&t, i);
        if (image && image->used) {
            ++usedImages;
            usedBytes += image->pixelLength;
        }
    }
    assert(usedImages <= Ubiquitty::Image::maxTerminalImageCount);
    // A 0 budget means "evict everything"; only a positive budget is a ceiling.
    if (t.images.kittyImageBytesBudget > 0)
        assert(usedBytes <= t.images.kittyImageBytesBudget);
    const int placements = Ubiquitty::Image::placementCount(&t);
    assert(placements >= 0 && placements <= Ubiquitty::Image::maxTerminalImagePlacementCount);
}

// At most 64 byte flips of a small valid body keep deep decoder paths
// reachable without making one input an unbounded mutation script.
static void flipImageBytes(std::vector<std::uint8_t>& bytes, std::span<const std::uint8_t> input)
{
    if (bytes.empty())
        return;
    const auto count = std::min<std::size_t>(input.size(), 128);
    for (std::size_t index = 0; index + 1 < count; index += 2)
        bytes[input[index] % bytes.size()] ^= input[index + 1];
}

static void runCanonicalImageTransfer(std::span<const std::uint8_t> input)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    if (const auto head = Sync::decodeImageHead(input)) {
        std::vector<std::uint8_t> canonical;
        Sync::encode(canonical, *head);
        assert(std::ranges::equal(canonical, input));
    }
    if (const auto want = Sync::decodeImageWant(input)) {
        std::vector<std::uint8_t> canonical;
        Sync::encode(canonical, *want);
        assert(std::ranges::equal(canonical, input));
    }
    constexpr std::array<std::uint8_t, 16> pixels { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    const Sync::ImageAssetIdentity asset { .hash = Sync::imageHash(pixels, 32, 2, 2), .format = 32, .width = 2, .height = 2 };
    for (unsigned variant = 0; variant != 2; ++variant) {
        std::vector<std::uint8_t> headBytes;
        Sync::encode(headBytes, Sync::ImageHead { .asset = asset, .transfer = 7 });
        Sync::BulkChunk chunk { .transfer = 7, .index = 0, .isLast = true, .bytes = pixels };
        Ubiquitty::Crypto::sha256(pixels, chunk.sha256);
        std::vector<std::uint8_t> chunkBytes;
        Sync::encode(chunkBytes, chunk);
        if (variant) {
            flipImageBytes(headBytes, input);
            flipImageBytes(chunkBytes, input);
        }
        const auto head = Sync::decodeImageHead(headBytes);
        const auto decodedChunk = Sync::decodeBulkChunk(chunkBytes);
        if (!head || !decodedChunk)
            continue;
        // Mutated dimensions can never reserve more than this seed's 16 bytes.
        auto transfer = Sync::ImageTransfer::begin(*head, pixels.size());
        if (!transfer)
            continue;
        const auto result = transfer->accept(*decodedChunk);
        if (result == Sync::ImageChunkResult::Refused) {
            assert(transfer->heldBytes() == 0);
            assert(!transfer->take());
            continue;
        }
        if (result != Sync::ImageChunkResult::Complete)
            continue;
        auto owner = transfer->take();
        assert(owner);
        Sync::ImagePlacements state;
        state.revision = 1;
        state.imageCount = 1;
        state.images[0] = { .imageID = 7, .asset = head->asset };
        Sync::ImageReplica replica(term());
        if (replica.replace(state))
            (void)replica.accept(std::move(*owner));
        checkTermSanity(term());
        checkImageStoreCapacity(term());
    }
}

static void runCanonicalImagePlacements(std::span<const std::uint8_t> input)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto exercise = [](std::span<const std::uint8_t> body) {
        const auto state = Sync::decodeImagePlacements(body);
        if (!state)
            return;
        std::vector<std::uint8_t> canonical;
        Sync::encode(canonical, *state);
        assert(std::ranges::equal(canonical, body));
        Sync::ImageReplica replica(term());
        (void)replica.replace(*state);
        checkTermSanity(term());
        checkImageStoreCapacity(term());
    };
    exercise(input);
    Sync::ImagePlacements seed;
    seed.revision = 1;
    seed.imageCount = 1;
    seed.images[0] = { .imageID = 7, .asset = { .hash = { 17, 19 }, .format = 32, .width = 2, .height = 2 }, .isVirtual = true, .virtualColumns = 2, .virtualRows = 2, .virtualPlacementID = 23 };
    seed.placementCount = 1;
    seed.placements[0] = { .hash = seed.images[0].asset.hash,
        .geometry = { .used = true, .imageID = 7, .placementID = 23, .hasPlacementID = true, .cursorX = 1, .cursorY = -1, .cellColumns = 2, .cellRows = 2, .pixelOffsetX = 0, .pixelOffsetY = 0, .sourceX = 0, .sourceY = 0, .sourceWidth = 0, .sourceHeight = 0, .drawnPixelWidth = 0, .drawnPixelHeight = 0, .zIndex = -3, .cursorPolicy = 1, .onAlternateScreen = 0 } };
    std::vector<std::uint8_t> bytes;
    Sync::encode(bytes, seed);
    exercise(bytes);
    flipImageBytes(bytes, input);
    exercise(bytes);
}

static void runCanonicalBody(Ubiquitty::Protocol::Sync::FrameKind kind, std::span<const std::uint8_t> body)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    namespace Control = Ubiquitty::Protocol::Control;
    assert(body.size() <= Sync::maxFrameBodyBytes);
    switch (kind) {
    case Sync::FrameKind::Greet:
        if (const auto message = Sync::decodeGreet(body))
            checkRoundTrip(*message, Sync::decodeGreet);
        break;
    case Sync::FrameKind::Welcome:
        if (const auto message = Sync::decodeWelcome(body))
            checkRoundTrip(*message, Sync::decodeWelcome);
        break;
    case Sync::FrameKind::Bye:
        if (const auto message = Sync::decodeBye(body))
            checkRoundTrip(*message, Sync::decodeBye);
        break;
    case Sync::FrameKind::Call:
        if (const auto message = Sync::decodeCall(body)) {
            checkRoundTrip(*message, Sync::decodeCall);
            const auto request = Sync::decodeCallRequest(message->text);
            if (request.outcome == Control::RequestOutcome::Dispatch && request.method == "pane.reclaim")
                (void)Control::readReclaimRequest(request.params);
        }
        UbiquittyTest::ConsentFuzz::frameBody(kind, body);
        break;
    case Sync::FrameKind::Result:
        if (const auto message = Sync::decodeResult(body)) {
            checkRoundTrip(*message, Sync::decodeResult);
            if (message->transfer == 0) {
                bool isJSON = false;
                const auto response = Ubiquitty::Codec::JSON::parse(message->text, isJSON);
                if (isJSON) {
                    (void)Control::decodeFileReadReply(response, "fuzz.file", 7, "/fuzz/file");
                    (void)Control::decodeCaptureReply(response, "fuzz.capture", { .pane = 7, .rows = 24, .columns = 80 });
                    (void)Control::isReclaimReleasedResult(response, "fuzz.reclaim", { .handle = 7, .sessionID = 9, .daemonInstanceID = 11, .hostInstanceID = { 13, 15 }, .attachmentID = { 17, 19 }, .paneID = { 21, 23 } });
                }
            }
        }
        break;
    case Sync::FrameKind::Notify:
        UbiquittyTest::ConsentFuzz::frameBody(kind, body);
        break;
    case Sync::FrameKind::ImageHead:
        (void)Sync::decodeImageHead(body);
        break;
    case Sync::FrameKind::ImageWant:
        (void)Sync::decodeImageWant(body);
        break;
    case Sync::FrameKind::ImagePlacements:
        runCanonicalImagePlacements(body);
        break;
    case Sync::FrameKind::ClipboardRequest:
        (void)Sync::decodeClipboardRead(body);
        break;
    case Sync::FrameKind::ClipboardData:
        (void)Sync::decodeClipboardData(body);
        break;
    case Sync::FrameKind::BulkChunk:
        (void)Sync::decodeBulkChunk(body);
        break;
    default:
        break;
    }
}

// Production ingress owns allocation, fragmentation and its permanent refusal
// latch. The callback borrows exactly one admitted body and retains no view.
static void runFrameDecoder(const uint8_t* data, size_t size)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    std::span<const std::uint8_t> input(data, size);
    while (!input.empty()) {
        const auto chunk = input.first(std::min<std::size_t>(input.front() + 1u, input.size()));
        const auto result = ingress.consume(chunk, [](auto bytes) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error = Sync::EnvelopeError::None;
            switch (Sync::readFrameHeader(bytes, header, error)) {
            case Sync::FrameHeaderState::NeedsMore:
                return Sync::FrameHeaderDecision {};
            case Sync::FrameHeaderState::Failed:
                return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Refused };
            case Sync::FrameHeaderState::Complete:
                return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
            }
            std::unreachable(); }, [](auto bytes, auto body) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error = Sync::EnvelopeError::None;
            const auto state = Sync::readFrameHeader(bytes, header, error);
            assert(state == Sync::FrameHeaderState::Complete);
            if (state != Sync::FrameHeaderState::Complete)
                return Sync::FrameDelivery::Stop;
            assert(body.size() == header.bodyBytes);
            runCanonicalBody(header.kind, body);
            return Sync::FrameDelivery::Continue; });
        assert(ingress.retainedBytes() <= Sync::maxFrameBodyBytes);
        assert(result.consumedBytes <= chunk.size());
        if (result.state != Sync::FrameIngress::State::Accepting) {
            assert(ingress.consume(chunk, [](auto) { return Sync::FrameHeaderDecision {}; }, [](auto, auto) { return Sync::FrameDelivery::Continue; }).consumedBytes == 0);
            break;
        }
        assert(result.consumedBytes == chunk.size());
        input = input.subspan(result.consumedBytes);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    const uint8_t selector = data[0];
    const uint8_t* body = data + 1;
    const size_t bodyLength = size - 1;

    switch (selector % 10) {
    case 0:
        // Whole input as a transport byte stream through the framer.
        runFrameDecoder(data, size);
        break;
    case 1: {
        // Bypass only the envelope, never the canonical body's byte bound.
        if (bodyLength <= Ubiquitty::Protocol::Sync::maxFrameBodyBytes) {
            for (const auto kind : { Ubiquitty::Protocol::Sync::FrameKind::Greet, Ubiquitty::Protocol::Sync::FrameKind::Welcome,
                     Ubiquitty::Protocol::Sync::FrameKind::Bye, Ubiquitty::Protocol::Sync::FrameKind::Call,
                     Ubiquitty::Protocol::Sync::FrameKind::Result, Ubiquitty::Protocol::Sync::FrameKind::ClipboardRequest,
                     Ubiquitty::Protocol::Sync::FrameKind::ClipboardData })
                runCanonicalBody(kind, { body, bodyLength });
        }
        // The complete clipboard bundle may arrive through bulk rather than
        // one frame. Its decoder still owns one aggregate data/MIME budget;
        // no native clipboard or data sink is installed by this harness.
        namespace Sync = Ubiquitty::Protocol::Sync;
        if (bodyLength <= Sync::maxEffectClipboardEncodedReadBytes) {
            if (const auto bundle = Sync::decodeEffectClipboard({ body, bodyLength }, Sync::maxEffectClipboardReadBytes)) {
                const auto encoded = Sync::encodeEffectClipboard(*bundle, Sync::maxEffectClipboardReadBytes);
                assert(encoded);
                assert(std::ranges::equal(encoded->view(), std::span(body, bodyLength)));
            }
        }
        break;
    }
    case 2: {
        // A bare Sync checkpoint body into a passive Terminal.
        Ubiquitty::Protocol::Sync::ScreenReplica replica;
        if (Ubiquitty::Protocol::Sync::applyCheckpoint(term(), { body, bodyLength }, replica) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied)
            checkTermSanity(term());
        break;
    }
    case 3:
        // The input past its selector as a transport byte stream, so a
        // stream's first tag is not always one the selector's residue allows.
        runFrameDecoder(body, bodyLength);
        break;
    case 4: {
        // Whole input as the checkpoint body too, so the selector byte is the
        // generation's first byte rather than lost.
        Ubiquitty::Protocol::Sync::ScreenReplica replica;
        if (Ubiquitty::Protocol::Sync::applyCheckpoint(term(), { data, size }, replica) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied)
            checkTermSanity(term());
        break;
    }
    case 5: {
        // The direct canonical stream through the real client dispatch.
        // Admission is still required before any pane can write the mirror.
        Ubiquitty::Protocol::Sync::SyncClientCore core(&term(), nullptr);
        core.setClock([] { return 1; });
        core.setClientInstance({ 3, 4 });
        std::vector<uint8_t> greeting;
        HandshakeRequest hello;
        hello.columns = 80;
        hello.rows = 24;
        core.buildHello(hello, greeting);
        (void)core.feed(std::as_bytes(std::span { body, bodyLength }));
        if (core.hasScreen())
            checkTermSanity(term());
        break;
    }
    case 6: {
        // Only the explicitly local descriptor handoff carries this body: one
        // whole terminal, read under the daemon's limits without any FD/Host
        // transport operation.
        const std::span<const std::uint8_t> input(body, bodyLength);
        const auto restored = Ubiquitty::Protocol::Sync::decodeHandoverTerminal(input, Ubiquitty::Session::localShareTerminalLimits(Ubiquitty::Protocol::Local::maxAdoptBodyBytes));
        if (restored && restored->terminal)
            checkTermSanity(*restored->terminal);
        break;
    }
    case 7: {
        runCanonicalImageTransfer({ body, bodyLength });
        break;
    }
    case 8:
        runCanonicalImagePlacements({ body, bodyLength });
        break;
    case 9:
        UbiquittyTest::ConsentFuzz::stream({ body, bodyLength });
        UbiquittyTest::ConsentFuzz::seeded({ body, bodyLength });
        break;
    }
    return 0;
}
