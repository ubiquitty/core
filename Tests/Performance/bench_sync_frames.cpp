// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_sync_frames.cpp — the encode side of the sync wire, timed and
// fingerprinted.
//
// Two claims live here. The first is speed: a checkpoint writes every row of
// both screens, and a file transfer carries megabytes. The second is that the
// bytes did not move, which for a wire format is the claim that matters — a
// header field out of order or a length backpatched wrong corrupts frames for
// every deployed peer, and no round-trip inside one build can see it.
//
// So the fingerprint covers a state space rather than one terminal: screens
// that are blank, partly filled and full, an alternate screen with a frozen
// primary behind it, wide characters, colour, and scrollback. A row's trailing
// blanks are not absent from the wire, so "the trailing columns are constant"
// is not an argument that the bytes are unchanged — only the bytes are.
//
//   bench_sync_frames --iters 200 [--json]

#include "BenchHarness.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty;

namespace {

std::uint64_t absorb(std::uint64_t hash, const std::vector<std::uint8_t>& bytes)
{
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// One terminal per shape the checkpoint encode branches on.
void paintShape(Terminal& t, int shape)
{
    hardReset(t);
    if (shape == 5) {
        // A large screen whose every column is a trailing blank, which is what
        // a fresh attach to a big window sends.
        resize(t, 200, 400);
        return;
    }
    resize(t, 40, 120);
    if (shape == 0)
        return; // blank: every column of every row is a trailing blank
    if (shape >= 1) {
        // Rows of unlike lengths, so the live span and the trailing run vary.
        for (int row = 0; row < 40; ++row) {
            char cursor[32];
            std::snprintf(cursor, sizeof(cursor), "\033[%d;1H", row + 1);
            processBytes(&t, cursor);
            processBytes(&t, std::string(static_cast<std::size_t>((row * 7) % 120), 'x'));
        }
    }
    if (shape >= 2) {
        processBytes(&t, "\033[3;1H\033[1;38;5;82;48;5;17mcoloured\033[0m");
        processBytes(&t, "\033[4;1H\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E wide");
        processBytes(&t, "\033[5;1H\033[4:3;58;5;9mcurly underline\033[0m");
    }
    if (shape >= 3) {
        for (int line = 0; line < 60; ++line)
            processBytes(&t, "scrollback line\r\n");
    }
    if (shape >= 4) {
        processBytes(&t, "\033[?1049h"); // alternate screen, primary frozen behind it
        processBytes(&t, "\033[2;3Halternate content");
    }
}

// Encodes a checkpoint of a terminal into `out`, cleared first.
bool encodeCheckpoint(const Terminal& t, std::vector<std::uint8_t>& out)
{
    Protocol::Sync::ScreenPublisher publisher;
    Protocol::Sync::AttachmentBaseline attachment;
    out.clear();
    return publisher.capture(t) && publisher.checkpoint(t, 77u, out, attachment);
}

// The same inline/head choice as HostConnection::sendResult. The production
// scheduler, not this benchmark, owns chunk boundaries and the final SHA-256.
bool encodeResult(std::string text, std::vector<std::uint8_t>& out)
{
    using namespace Protocol::Sync;
    LaneScheduler scheduler(measuredHostPolicy(0).lanes);
    std::vector<std::uint8_t> body;
    encode(body, Result { .transfer = 0, .text = text });
    if (body.size() <= maxFrameBodyBytes) {
        if (!scheduler.enqueue(FrameKind::Result, 0, body, FrameRole::Reply))
            return false;
    } else {
        body.clear();
        encode(body, Result { .transfer = scheduler.nextTransferID(), .text = {} });
        if (!scheduler.enqueueTransfer(FrameKind::Result, 0, body,
                std::vector<std::uint8_t>(text.begin(), text.end()), TransferRank::Answer, FrameRole::Reply))
            return false;
    }
    out.clear();
    while (scheduler.popFrame(out, BulkPacing::Admitted)) {
    }
    return scheduler.isEmpty() && scheduler.queuedReplies() == 0 && !out.empty();
}

// Daemon::serveFileRead is the producer: its exact five result fields and the
// registry's file.read schema are authoritative. No file I/O is timed here.
bool encodeFileReply(std::span<const std::uint8_t> bytes, std::string_view path, std::string_view requestID, std::vector<std::uint8_t>& out)
{
    using Codec::JSON::Value;
    if (bytes.size() > Protocol::Control::maxReadFileBytes)
        return false;
    std::string encoded(Codec::base64EncodedLength(bytes.size()), '\0');
    if (!Codec::base64Encode({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, encoded))
        return false;
    auto result = Value::object();
    result.set("pane", Value::number(77));
    result.set("path", Value::string(std::string(path)));
    result.set("status", Value::string(std::string(Protocol::Control::fileReadStatusName(Protocol::Control::FileReadStatus::OK))));
    result.set("bytes", Value::number(static_cast<double>(bytes.size())));
    result.set("data", Value::string(std::move(encoded)));
    const auto id = Value::string(std::string(requestID));
    return encodeResult(Codec::JSON::serialize(Protocol::Control::rpcResult(&id, std::move(result))), out);
}

// Daemon::serveScreenCall's pane.capture/rows result. This is a serialization
// size sweep, not a terminal extraction test: the text byte counts are fixed,
// zero included, so its sizes compare from run to run.
bool encodeCaptureReply(std::string text, std::vector<std::uint8_t>& out)
{
    using Codec::JSON::Value;
    auto result = Value::object();
    result.set("pane", Value::number(77));
    result.set("source", Value::string("rows"));
    result.set("first", Value::number(5));
    result.set("last", Value::number(9));
    result.set("totalRows", Value::number(10));
    result.set("columns", Value::number(120));
    result.set("complete", Value::boolean(true));
    result.set("text", Value::string(std::move(text)));
    const auto id = Value::string("capture-100");
    return encodeResult(Codec::JSON::serialize(Protocol::Control::rpcResult(&id, std::move(result))), out);
}

// Decode every envelope and complete the production bulk reassembler before
// comparing JSON. A nonempty announcement alone must never count as delivery.
std::optional<Codec::JSON::Value> decodeReply(const std::vector<std::uint8_t>& wire, std::string_view method, std::string_view requestID)
{
    using namespace Protocol::Sync;
    FrameDecoder decoder;
    decoder.append(wire);
    BulkReassembler reassembler;
    std::optional<std::uint32_t> transfer;
    std::optional<std::string> response;
    bool sawResult = false;
    while (const auto frame = decoder.next()) {
        if (frame->channel != 0 || response)
            return std::nullopt;
        if (frame->kind == FrameKind::Result) {
            const auto result = decodeResult(frame->body);
            if (!result || sawResult)
                return std::nullopt;
            sawResult = true;
            if (!result->transfer)
                response = result->text;
            else {
                transfer = result->transfer;
                if (!reassembler.expect(*transfer, ClientPolicy {}.maxResultBytes))
                    return std::nullopt;
            }
        } else if (frame->kind == FrameKind::BulkChunk) {
            const auto chunk = decodeBulkChunk(frame->body);
            if (!chunk || !transfer || chunk->transfer != *transfer)
                return std::nullopt;
            const auto outcome = reassembler.accept(*chunk);
            if (outcome == ChunkOutcome::Complete) {
                const auto bytes = reassembler.take(*transfer);
                response = std::string(bytes.begin(), bytes.end());
            } else if (outcome != ChunkOutcome::Pending)
                return std::nullopt;
        } else
            return std::nullopt;
    }
    if (!response || decoder.error() != EnvelopeError::None || decoder.bufferedBytes())
        return std::nullopt;
    bool isValid = false;
    auto answer = Codec::JSON::parse(*response, isValid);
    const auto* descriptor = Protocol::Control::findSyncMethod(method);
    const auto* result = answer.find("result");
    if (!isValid || !descriptor || !result || answer.members().size() != 3
        || answer.stringOr("jsonrpc") != "2.0" || answer.stringOr("id") != requestID)
        return std::nullopt;
    const auto schema = Protocol::Control::parseRegistryJSON(descriptor->resultSchema);
    if (Protocol::Control::checkSchema(schema) || Protocol::Control::validate(*result, schema))
        return std::nullopt;
    return answer;
}

Protocol::Sync::FrameHeaderDecision readHeader(std::span<const std::uint8_t> bytes)
{
    using namespace Protocol::Sync;
    FrameHeader header;
    EnvelopeError error = EnvelopeError::None;
    switch (readFrameHeader(bytes, header, error)) {
    case FrameHeaderState::NeedsMore:
        return {};
    case FrameHeaderState::Complete:
        return { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
    case FrameHeaderState::Failed:
        return { .state = FrameHeaderDecision::State::Refused };
    }
    return { .state = FrameHeaderDecision::State::Refused };
}

} // namespace

int main(int argc, char** argv)
{
    int iters = 200;
    const char* platform = "portable";
    const char* commit = "";
    bool shouldEmitJSON = false;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--json"))
            shouldEmitJSON = true;
    }
    if (iters <= 0 || iters > 1000000)
        iters = 200;

    constexpr int shapeCount = 6;
    std::vector<std::unique_ptr<Terminal>> terminals;
    for (int shape = 0; shape < shapeCount; ++shape) {
        auto t = std::make_unique<Terminal>();
        paintShape(*t, shape);
        terminals.push_back(std::move(t));
    }

    std::uint64_t fingerprint = 1469598103934665603ull;
    std::size_t checkpointBytes = 0;
    std::vector<std::uint8_t> out;
    for (int shape = 0; shape < shapeCount; ++shape) {
        if (!encodeCheckpoint(*terminals[static_cast<std::size_t>(shape)], out)) {
            std::fprintf(stderr, "shape %d could not be checkpointed\n", shape);
            return 2;
        }
        fingerprint = absorb(fingerprint, out);
        checkpointBytes += out.size();
    }

    // File and captured-text Results from empty to past 64 KiB. The canonical
    // envelope is capped; larger replies use bulk.
    std::size_t messageBytes = 0;
    for (std::size_t size : { std::size_t(0), std::size_t(1), std::size_t(255), std::size_t(65535), std::size_t(65536), std::size_t(300000) }) {
        const std::vector<std::uint8_t> bytes(size, static_cast<std::uint8_t>(size & 0xFF));
        constexpr std::string_view path = "/some/path/with/a/name.txt";
        if (!encodeFileReply(bytes, path, "file-99", out))
            return 2;
        fingerprint = absorb(fingerprint, out);
        messageBytes += out.size();
        const auto file = decodeReply(out, "file.read", "file-99");
        if (!file)
            return 2;
        const auto decoded = Protocol::Control::decodeFileReadReply(*file, "file-99", 77, path);
        if (!decoded || decoded->status != Protocol::Control::FileReadStatus::OK || decoded->path != path
            || decoded->length != bytes.size() || !std::ranges::equal(decoded->data(), bytes))
            return 2;

        const std::string text(size, 'r');
        if (!encodeCaptureReply(text, out))
            return 2;
        fingerprint = absorb(fingerprint, out);
        messageBytes += out.size();
        const auto capture = decodeReply(out, "pane.capture", "capture-100");
        const auto* region = capture ? capture->find("result") : nullptr;
        if (!region || region->numberOr("pane") != 77 || region->stringOr("source") != "rows"
            || region->numberOr("first") != 5 || region->numberOr("last") != 9 || region->numberOr("totalRows") != 10
            || region->numberOr("columns") != 120 || !region->boolOr("complete") || region->stringOr("text") != text)
            return 2;
    }

    // Each encode captures into a fresh publisher, so the lane is what an
    // attach to each shape costs, capture included.
    const auto checkpointTiming = Bench::measure(10, iters, [&] {
        for (int shape = 0; shape < shapeCount; ++shape) {
            (void)encodeCheckpoint(*terminals[static_cast<std::size_t>(shape)], out);
            Bench::doNotOptimize(shape);
        }
    });

    // Header-first ingress over a coalesced burst of valid small Probe frames.
    std::vector<std::uint8_t> burst;
    std::vector<std::uint8_t> probe;
    Protocol::Sync::encode(probe, Protocol::Sync::Probe { .time = 7 });
    std::size_t expectedFrames = 0;
    while (burst.size() < 1u << 20) {
        if (!Protocol::Sync::appendFrame(burst, Protocol::Sync::FrameKind::Probe, 0, probe))
            return 2;
        ++expectedFrames;
    }
    // Validate exact bodies and delivery count outside timing. Splitting the
    // first header also proves a partial header is not counted as a frame.
    Protocol::Sync::FrameIngress preflight(Protocol::Sync::maxFrameBodyBytes);
    std::size_t validatedFrames = 0;
    const auto validateProbe = [&](auto header, auto body) {
        const auto decoded = Protocol::Sync::decodeProbe(body);
        if (header[0] != std::to_underlying(Protocol::Sync::FrameKind::Probe) || !decoded || decoded->time != 7)
            return Protocol::Sync::FrameDelivery::Stop;
        ++validatedFrames;
        return Protocol::Sync::FrameDelivery::Continue;
    };
    const auto prefix = preflight.consume(std::span { burst }.first(1), readHeader, validateProbe);
    if (prefix.consumedBytes != 1 || prefix.state != Protocol::Sync::FrameIngress::State::Accepting || validatedFrames)
        return 2;
    const auto suffix = preflight.consume(std::span { burst }.subspan(1), readHeader, validateProbe);
    if (suffix.consumedBytes != burst.size() - 1 || suffix.state != Protocol::Sync::FrameIngress::State::Accepting
        || preflight.error() != Protocol::Sync::FrameIngressError::None || preflight.pendingBytes() || validatedFrames != expectedFrames)
        return 2;
    std::size_t framesDrained = 0;
    bool didDrain = true;
    const auto drainTiming = Bench::measure(3, iters < 40 ? iters : 40, [&] {
        Protocol::Sync::FrameIngress ingress(Protocol::Sync::maxFrameBodyBytes);
        std::size_t drained = 0;
        const auto consumed = ingress.consume(burst, readHeader, [&](auto, auto) {
            ++drained;
            return Protocol::Sync::FrameDelivery::Continue;
        });
        didDrain = didDrain && consumed.consumedBytes == burst.size()
            && consumed.state == Protocol::Sync::FrameIngress::State::Accepting
            && ingress.error() == Protocol::Sync::FrameIngressError::None && !ingress.pendingBytes()
            && drained == expectedFrames;
        framesDrained = drained;
        Bench::doNotOptimize(framesDrained);
    });

    // Base64, JSON and canonical inline/bulk encoding at file.read's 4 MiB
    // input cap.
    const std::vector<std::uint8_t> large(Protocol::Control::maxReadFileBytes, 0x5A);
    constexpr std::string_view largePath = "/var/log/some.log";
    if (!encodeFileReply(large, largePath, "file-7", out))
        return 2;
    const auto largeAnswer = decodeReply(out, "file.read", "file-7");
    if (!largeAnswer)
        return 2;
    const auto largeDecoded = Protocol::Control::decodeFileReadReply(*largeAnswer, "file-7", 77, largePath);
    if (!largeDecoded || largeDecoded->status != Protocol::Control::FileReadStatus::OK
        || largeDecoded->length != large.size() || !std::ranges::equal(largeDecoded->data(), large))
        return 2;
    bool didEncodeFile = true;
    const auto fileTiming = Bench::measure(3, iters < 40 ? iters : 40, [&] {
        didEncodeFile = encodeFileReply(large, largePath, "file-7", out) && didEncodeFile;
        Bench::doNotOptimize(out);
    });

    std::printf("sync_frames wireVersion=%u checkpoints=%d bytes=%zu messageBytes=%zu\n",
        static_cast<unsigned>(Protocol::Sync::wireVersion), shapeCount, checkpointBytes, messageBytes);
    std::printf("  drain of %zu frames from %zu bytes best=%.2f us\n", framesDrained, burst.size(), drainTiming.medianSeconds * 1e6);
    std::printf("  4 MiB file.read Result encode best=%.2f us\n", fileTiming.medianSeconds * 1e6);
    std::printf("  checkpoint set best=%.2f us  p95=%.2f us\n",
        checkpointTiming.medianSeconds * 1e6, checkpointTiming.p95Seconds * 1e6);
    std::printf("  wire fingerprint=%016llx\n", static_cast<unsigned long long>(fingerprint));
    // An encoder that produced nothing fingerprints as stably as one that
    // produced the right bytes.
    if (checkpointBytes == 0 || messageBytes == 0 || !didEncodeFile) {
        std::fprintf(stderr, "no frame carried bytes: this fingerprinted an empty encode\n");
        return 2;
    }
    // A drain that yielded nothing times a decoder that refused the burst.
    if (!didDrain || framesDrained != expectedFrames) {
        std::fprintf(stderr, "the ingress did not deliver the exact Probe burst\n");
        return 2;
    }

    Bench::Result result;
    result.name = "sync_checkpoint_set";
    result.unit = "us";
    result.value = checkpointTiming.medianSeconds * 1e6;
    result.p95 = checkpointTiming.p95Seconds * 1e6;
    Bench::Result drain;
    drain.name = "sync_ingress_probe_burst";
    drain.unit = "us";
    drain.value = drainTiming.medianSeconds * 1e6;
    drain.p95 = drainTiming.p95Seconds * 1e6;
    Bench::Result fileEncode;
    fileEncode.name = "sync_file_read_result_encode";
    fileEncode.unit = "us";
    fileEncode.value = fileTiming.medianSeconds * 1e6;
    fileEncode.p95 = fileTiming.p95Seconds * 1e6;
    std::vector<Bench::Result> results { result, drain, fileEncode };
    if (shouldEmitJSON)
        Bench::emitJSON(results, platform, commit);
    return 0;
}
