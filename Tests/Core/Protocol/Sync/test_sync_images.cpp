// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "AttachmentLink.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Codec/Base64.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImageInternal.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/PAL.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
namespace Image = Ubiquitty::Image;

// Only the source object's nothrow allocation is injectable. Its pixel copy
// uses malloc; reaching this refusal proves that earlier copy succeeded.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

namespace {

std::string base64(std::span<const std::uint8_t> bytes)
{
    std::string result(Ubiquitty::Codec::base64EncodedLength(bytes.size()), '\0');
    const auto characters = Ubiquitty::Codec::base64Encode({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, result);
    CHECK(characters && characters->size() == result.size());
    return result;
}

std::string kitty(std::string_view controls, std::span<const std::uint8_t> bytes = {})
{
    return "\033_G" + std::string(controls) + ";" + base64(bytes) + "\033\\";
}

const Image::TerminalImage* findImage(const Ubiquitty::Terminal& terminal, std::uint32_t id)
{
    for (const auto& image : terminal.images.stored) {
        if (image.used && image.imageID == id)
            return &image;
    }
    return nullptr;
}

/// A kitty upload as a program writes one: the base64 payload cut into
/// 4096-byte escapes, the controls on the first and `m=1` on all but the last.
std::string kittyChunked(std::string_view controls, std::span<const std::uint8_t> payload)
{
    constexpr std::size_t chunkBytes = 4096;
    const std::string encoded = base64(payload);
    std::string output;
    std::size_t offset = 0;
    do {
        const bool isLast = encoded.size() - offset <= chunkBytes;
        output += "\033_G";
        if (!offset)
            output += std::string(controls) + ",";
        output += isLast ? "m=0;" : "m=1;";
        output += encoded.substr(offset, chunkBytes);
        output += "\033\\";
        offset += chunkBytes;
    } while (offset < encoded.size());
    return output;
}

/// The two charts of a dashboard that redraws once a second, each under its
/// own image and placement ID, as `deployment-monitor --graphics kitty` sends
/// them. The cell is 6 by 19 pixels.
struct DashboardChart {
    std::uint32_t imageID;
    std::uint32_t row;
    std::uint32_t rows;
};
constexpr std::array dashboardCharts { DashboardChart { .imageID = 7401, .row = 6, .rows = 5 }, DashboardChart { .imageID = 7402, .row = 14, .rows = 4 } };
constexpr std::uint32_t dashboardChartColumns = 55;
constexpr std::uint32_t dashboardChartWidth = dashboardChartColumns * 6;

/// A chart's picture at one frame: a transparent field crossed by a dashed
/// rule and an opaque two-pixel line whose shape moves with the frame. The
/// first pixel spells the frame, so a reader can tell which one it holds.
std::vector<std::uint8_t> chartPixels(const DashboardChart& chart, std::uint32_t frame)
{
    const std::uint32_t height = chart.rows * 19;
    std::vector<std::uint8_t> pixels(std::size_t { dashboardChartWidth } * height * 4, 0);
    const auto paint = [&](std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> colour) {
        std::ranges::copy(colour, pixels.begin() + static_cast<std::ptrdiff_t>((std::size_t { y } * dashboardChartWidth + x) * 4));
    };
    for (std::uint32_t x = 0; x < dashboardChartWidth; x += 8) {
        for (std::uint32_t dash = x; dash < std::min(dashboardChartWidth, x + 4); ++dash)
            paint(dash, height / 2, { 0x4A, 0x4D, 0x55, 0xCC });
    }
    for (std::uint32_t x = 0; x < dashboardChartWidth; ++x) {
        const std::uint32_t sample = (x / 6 + frame) % 300;
        const std::uint32_t y = 2 + (sample * 37 + chart.imageID + (sample * sample) % 23) % (height - 4);
        paint(x, y, { 0x39, 0x87, static_cast<std::uint8_t>(chart.imageID), 0xFF });
        paint(x, y + 1, { 0x39, 0x87, static_cast<std::uint8_t>(chart.imageID), 0xFF });
    }
    paint(0, 0, { static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8), 0x01, 0xFF });
    return pixels;
}

/// One dashboard frame, both charts uploaded again in place inside
/// synchronized output. The cells under the charts are not erased.
std::string dashboardFrame(std::uint32_t frame)
{
    std::string output = "\033[?2026h\033[1;1HFrame " + std::to_string(frame);
    for (const DashboardChart& chart : dashboardCharts) {
        const auto pixels = chartPixels(chart, frame);
        const std::string controls = "a=T,i=" + std::to_string(chart.imageID) + ",p=1,f=32,o=z,s=" + std::to_string(dashboardChartWidth)
            + ",v=" + std::to_string(chart.rows * 19) + ",c=" + std::to_string(dashboardChartColumns) + ",r=" + std::to_string(chart.rows) + ",C=1,q=2";
        output += "\033[" + std::to_string(chart.row) + ";9H" + kittyChunked(controls, Ubiquitty::Codec::Zlib::compress(pixels));
    }
    return output + "\033[?2026l";
}

/// Which frame's exact picture a terminal shows for a chart, if any.
std::optional<std::uint32_t> shownChartFrame(const Ubiquitty::Terminal& terminal, const DashboardChart& chart)
{
    const auto* image = findImage(terminal, chart.imageID);
    if (!image || !image->pixels || image->pixelLength < 4)
        return std::nullopt;
    const std::uint32_t frame = image->pixels[0] | (std::uint32_t { image->pixels[1] } << 8);
    const auto pixels = chartPixels(chart, frame);
    if (image->pixelLength != pixels.size() || !std::equal(pixels.begin(), pixels.end(), image->pixels))
        return std::nullopt;
    return frame;
}

/// Whether every chart is placed once, at its own cells, over its own image.
bool showsEveryChart(const Ubiquitty::Terminal& terminal)
{
    if (Image::placementCount(&terminal) != static_cast<int>(dashboardCharts.size()))
        return false;
    return std::ranges::all_of(dashboardCharts, [&](const DashboardChart& chart) {
        return std::ranges::count_if(terminal.images.placements, [&](const auto& placement) {
            return placement.used && placement.imageID == chart.imageID && placement.cursorY == static_cast<std::int16_t>(chart.row - 1)
                && placement.cursorX == 8 && placement.cellColumns == dashboardChartColumns && placement.cellRows == chart.rows;
        }) == 1;
    });
}

/// Exercises the image-state sender and receiver with real VT-decoded host
/// images. The connection's frame registration and scheduler are
/// outside this fixture; it does not stand in for their transport tests.
struct ImageLink {
    std::unique_ptr<Ubiquitty::Terminal> host = std::make_unique<Ubiquitty::Terminal>();
    std::unique_ptr<Ubiquitty::Terminal> client = std::make_unique<Ubiquitty::Terminal>();
    ImagePublisher publisher { *host };
    ImageReplica replica { *client };
    std::size_t transfers = 0;
    std::vector<std::uint8_t> wire;

    ImageLink()
    {
        Ubiquitty::resize(*host, 12, 40);
        Ubiquitty::resize(*client, 12, 40);
    }

    void write(std::string_view bytes) { Ubiquitty::processBytes(host.get(), bytes); }

    bool sendPlacements()
    {
        if (!publisher.capture())
            return false;
        std::vector<std::uint8_t> record;
        encode(record, publisher.state());
        wire.insert(wire.end(), record.begin(), record.end());
        const auto decoded = decodeImagePlacements(record);
        return decoded && replica.replace(*decoded);
    }

    bool serveMissing()
    {
        for (const auto& asset : replica.missing()) {
            std::vector<std::uint8_t> request;
            encode(request, ImageWant { .hash = asset.hash });
            const auto want = decodeImageWant(request);
            if (!want)
                return false;
            const auto pixels = publisher.copyPixels(want->hash, Image::maxDecodedImageBytes);
            if (!pixels)
                return false;
            std::vector<std::uint8_t> headBytes;
            encode(headBytes, ImageHead { .asset = pixels->asset, .transfer = static_cast<std::uint32_t>(transfers + 1) });
            wire.insert(wire.end(), headBytes.begin(), headBytes.end());
            wire.insert(wire.end(), pixels->pixels().begin(), pixels->pixels().end());
            const auto head = decodeImageHead(headBytes);
            if (!head || replica.accept(head->asset, pixels->pixels()) != ImagePixelsResult::Applied)
                return false;
            ++transfers;
        }
        return true;
    }

    bool synchronize() { return sendPlacements() && serveMissing(); }
};

constexpr std::array<std::uint8_t, 16> fourPixels { 255, 0, 0, 127, 0, 255, 0, 255, 0, 0, 255, 0, 255, 255, 255, 255 };

class MemoryFiles final : public Ubiquitty::FileReadClient {
public:
    int calls = 0;
    int readFile(const char*, bool, unsigned char* output, int capacity) override // bool-ok: FileReadClient override
    {
        ++calls;
        if (capacity < static_cast<int>(fourPixels.size()))
            return -1;
        std::memcpy(output, fourPixels.data(), fourPixels.size());
        return static_cast<int>(fourPixels.size());
    }
    int readSharedMemory(const char* path, bool consume, unsigned char* output, int capacity) override // bool-ok: FileReadClient override
    {
        return readFile(path, consume, output, capacity);
    }
    bool supportsSharedMemoryTransport() const override { return true; }
    bool supportsTemporaryFileTransport() const override { return true; }
};

/// Counts actual decoded BulkChunk payload bytes on the real connection wire,
/// not just calls to ImagePublisher. Each frame is delivered before the next
/// is popped, keeping the carrier scratch bounded to one frame.
struct ImageConnectionLink {
    UbiquittyTest::AttachmentLink::Host host { { 41, 42 } };
    ClientAttachment client;
    std::shared_ptr<BulkSourceBudget> connectionBudget;
    std::shared_ptr<BulkSourceBudget> aggregateBudget = std::make_shared<BulkSourceBudget>(4 * maxCatalogMessageBytes);
    std::uint64_t now = 1;
    std::size_t pixelBytes = 0;
    std::size_t pixelTransfers = 0;
    std::size_t offers = 0;
    std::size_t wants = 0;
    std::size_t placementRecords = 0;
    std::vector<std::uint32_t> imageTransfers;
    std::vector<std::uint32_t> announcedImageTransfers;

    explicit ImageConnectionLink(ClientPaneDelegate* delegate = nullptr, Compression coding = Compression::None)
        : client(policy(coding), measuredLivenessPolicy(), UbiquittyTest::AttachmentLink::clientIdentity(), delegate)
    {
    }

    /// The fixture's client policy, taking deflated images or raw ones.
    static ClientPolicy policy(Compression coding)
    {
        ClientPolicy policy = UbiquittyTest::syncClientPolicy;
        policy.capabilities.set(ClientCapability::DeflateImages, coding == Compression::Deflate);
        return policy;
    }

    void accept()
    {
        host.accept();
        connectionBudget = std::make_shared<BulkSourceBudget>(maxCatalogMessageBytes);
        host.connection->setImageSourceAdmission([this] {
            return BulkSourceAdmission { .connection = connectionBudget, .aggregate = aggregateBudget, .otherConnectionBytes = host.connection->retainedOutboundPayloadBytes(), .otherAggregateBytes = host.connection->retainedOutboundPayloadBytes() };
        });
        imageTransfers.clear();
        (void)client.connect(now);
    }

    void pump(std::size_t imageChunksToTake = 0)
    {
        now += 16'667;
        host.connection->service(now);
        std::vector<std::uint8_t> down;
        std::size_t imageChunks = 0;
        while (host.connection->popStreamFrame(down, now)) {
            FrameDecoder decoder;
            decoder.append(down);
            const auto frame = decoder.next();
            REQUIRE(frame);
            if (frame->kind == FrameKind::ImagePlacements)
                ++placementRecords;
            if (frame->kind == FrameKind::ImageHead) {
                const auto head = decodeImageHead(frame->body);
                REQUIRE(head);
                if (head->transfer) {
                    imageTransfers.push_back(head->transfer);
                    announcedImageTransfers.push_back(head->transfer);
                    ++pixelTransfers;
                } else {
                    ++offers;
                }
            } else if (frame->kind == FrameKind::BulkChunk) {
                const auto chunk = decodeBulkChunk(frame->body);
                REQUIRE(chunk);
                if (std::ranges::find(imageTransfers, chunk->transfer) != imageTransfers.end()) {
                    pixelBytes += chunk->bytes.size();
                    ++imageChunks;
                    if (chunk->isLast)
                        std::erase(imageTransfers, chunk->transfer);
                }
            } else if (frame->kind == FrameKind::BulkAbort) {
                const auto abort = decodeBulkAbort(frame->body);
                REQUIRE(abort);
                std::erase(imageTransfers, abort->transfer);
            }
            client.receiveStream(down, now);
            down.clear();
            if (imageChunksToTake && imageChunks >= imageChunksToTake)
                break;
        }
        client.tick(now);
        const auto up = client.takeStreamOutput();
        FrameDecoder decoder;
        decoder.append(up);
        while (const auto frame = decoder.next())
            wants += frame->kind == FrameKind::ImageWant ? 1 : 0;
        host.connection->receiveStream(up, now);
    }

    void settle()
    {
        for (int step = 0; step < 20; ++step)
            pump();
        CHECK(host.connection->state() == ConnectionState::Attached);
        CHECK(client.state() == ClientAttachmentState::Live);
    }

    void attach()
    {
        accept();
        pump();
        pump();
        REQUIRE(host.connection->bindPane(*host.pane.source) != 0);
        settle();
    }

    void write(std::string_view bytes)
    {
        Ubiquitty::processBytes(&host.pane.terminal, bytes);
        host.pane.source->publish(FrameClass::Structural, now);
    }

    const Ubiquitty::Terminal* terminal() const { return client.paneTerminal(UbiquittyTest::AttachmentLink::paneID); }

    void receive(FrameKind kind, std::uint32_t channel, const auto& value)
    {
        std::vector<std::uint8_t> body;
        encode(body, value);
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, channel, body));
        client.receiveStream(frame, ++now);
    }

    void reconnect()
    {
        UbiquittyTest::AttachmentLink::drop(host, client, now);
        CHECK_EQ(connectionBudget->heldBytes(), 0u);
        CHECK_EQ(aggregateBudget->heldBytes(), 0u);
        accept();
        settle();
        REQUIRE(client.connection() && client.connection()->welcome());
        REQUIRE(client.connection()->welcome()->resumed.size() == 1);
        CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    }
};

} // namespace

TEST(imageHeadsAndWantsRequireOneCompleteCanonicalBody)
{
    const ImageAssetIdentity asset { .hash = { 17, 19 }, .format = 32, .width = 2, .height = 3 };
    std::vector<std::uint8_t> head;
    encode(head, ImageHead { .asset = asset, .transfer = 23 });
    const auto decodedHead = decodeImageHead(head);
    REQUIRE(decodedHead);
    CHECK(decodedHead->asset == asset && decodedHead->transfer == 23);
    for (std::size_t cut = 0; cut < head.size(); ++cut)
        CHECK(!decodeImageHead(std::span(head).first(cut)));
    head.push_back(0);
    CHECK(!decodeImageHead(head));

    std::vector<std::uint8_t> want;
    encode(want, ImageWant { .hash = asset.hash });
    const auto decodedWant = decodeImageWant(want);
    REQUIRE(decodedWant);
    CHECK(decodedWant->hash == asset.hash);
    for (std::size_t cut = 0; cut < want.size(); ++cut)
        CHECK(!decodeImageWant(std::span(want).first(cut)));
    want.push_back(0);
    CHECK(!decodeImageWant(want));
}

TEST(invalidImageDimensionsNeverPublishOrAllocatePixels)
{
    for (const auto& dimensions : { std::pair { 0u, 1u }, std::pair { 1u, 0u }, std::pair { 32768u, 32768u } }) {
        const ImageAssetIdentity asset { .hash = { 17, 19 }, .format = 32, .width = dimensions.first, .height = dimensions.second };
        CHECK(!imagePixelBytes(asset));
        std::vector<std::uint8_t> body;
        encode(body, ImageHead { .asset = asset, .transfer = 1 });
        CHECK(!decodeImageHead(body));
        CHECK(!ImageTransfer::begin({ .asset = asset, .transfer = 1 }, Image::maxDecodedImageBytes));

        Ubiquitty::Terminal terminal;
        auto& stored = terminal.images.stored[0];
        stored.used = true;
        stored.imageID = 7;
        stored.format = 32;
        stored.width = static_cast<std::uint16_t>(dimensions.first);
        stored.height = static_cast<std::uint16_t>(dimensions.second);
        stored.pixelLength = 4;
        stored.pixels = static_cast<unsigned char*>(std::malloc(stored.pixelLength));
        REQUIRE(stored.pixels);
        std::memset(stored.pixels, 0, stored.pixelLength);
        ImagePublisher publisher(terminal);
        CHECK(!publisher.capture());
        CHECK_EQ(publisher.state().revision, 0u);
        CHECK_EQ(publisher.state().imageCount, 0u);
    }
}

TEST(anImageTransferRefusesAnOrphanOrOversizedFirstChunk)
{
    constexpr std::size_t pixelBytes = 400u * 100u * 4u;
    static_assert(pixelBytes > bulkChunkPayloadBytes);
    const ImageHead head { .asset = { .hash = { 17, 19 }, .format = 32, .width = 400, .height = 100 }, .transfer = 7 };
    std::vector<std::uint8_t> bytes(bulkChunkPayloadBytes + 1, 0x5A);
    for (const bool isOrphan : { true, false }) {
        auto transfer = ImageTransfer::begin(head, pixelBytes);
        REQUIRE(transfer);
        const BulkChunk chunk { .transfer = 7, .index = isOrphan ? 1u : 0u, .isLast = false, .bytes = isOrphan ? std::span(bytes).first(1) : std::span(bytes) };
        CHECK(transfer->accept(chunk) == ImageChunkResult::Refused);
        CHECK_EQ(transfer->heldBytes(), 0u);
        CHECK(!transfer->take());
    }
}

TEST(aLargeDecodedImageCompletesThroughBoundedRawChunks)
{
    constexpr std::uint32_t width = 2400;
    constexpr std::uint32_t height = 2000;
    constexpr std::size_t pixelBytes = std::size_t { width } * height * 4;
    static_assert(pixelBytes > 16u * 1024u * 1024u && pixelBytes <= Image::maxDecodedImageBytes);
    std::vector<std::uint8_t> pixels(pixelBytes, 0x5A);
    const ImageAssetIdentity asset { .hash = imageHash(pixels, 32, width, height), .format = 32, .width = width, .height = height };
    auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, pixelBytes);
    REQUIRE(transfer);
    std::array<std::uint8_t, 32> digest {};
    Ubiquitty::Crypto::sha256(pixels, digest);
    std::uint32_t index = 0;
    for (std::size_t offset = 0; offset < pixels.size(); offset += bulkChunkPayloadBytes) {
        const auto chunk = std::span(pixels).subspan(offset, std::min(bulkChunkPayloadBytes, pixels.size() - offset));
        const bool isLast = offset + chunk.size() == pixels.size();
        CHECK(transfer->accept({ .transfer = 7, .index = index++, .isLast = isLast, .bytes = chunk, .sha256 = isLast ? digest : std::array<std::uint8_t, 32> {} })
            == (isLast ? ImageChunkResult::Complete : ImageChunkResult::Pending));
        if (!isLast)
            CHECK(!transfer->take());
    }
    CHECK(index > 1);
    const auto complete = transfer->take();
    REQUIRE(complete);
    CHECK(complete->asset == asset);
    CHECK(std::ranges::equal(complete->pixels(), pixels));
    CHECK_EQ(transfer->heldBytes(), 0u);
    CHECK(!transfer->take());
}

/// Feeds a deflated transfer's wire bytes in chunks of at most `chunkBytes`,
/// the last naming Deflate, `inflatedBytes` and the bytes' SHA-256.
/// - Returns: Each chunk's result, in order, stopping after a refusal.
std::vector<ImageChunkResult> feedDeflated(ImageTransfer& transfer, std::span<const std::uint8_t> wire, std::size_t chunkBytes, std::uint32_t inflatedBytes, Compression declared = Compression::Deflate)
{
    std::array<std::uint8_t, 32> digest {};
    Ubiquitty::Crypto::sha256(wire, digest);
    std::vector<ImageChunkResult> results;
    std::uint32_t index = 0;
    for (std::size_t offset = 0; offset < wire.size(); offset += chunkBytes) {
        const auto bytes = wire.subspan(offset, std::min(chunkBytes, wire.size() - offset));
        const bool isLast = offset + bytes.size() == wire.size();
        BulkChunk chunk { .transfer = 7, .index = index++, .isLast = isLast, .bytes = bytes };
        if (isLast) {
            chunk.sha256 = digest;
            chunk.compression = declared;
            chunk.inflatedBytes = declared == Compression::Deflate ? inflatedBytes : 0;
        }
        results.push_back(transfer.accept(chunk));
        if (results.back() == ImageChunkResult::Refused)
            break;
    }
    return results;
}

/// A zlib stream of `bytes`, as a host writes one.
std::vector<std::uint8_t> zlibStream(std::span<const std::uint8_t> bytes)
{
    std::vector<std::uint8_t> stream(Ubiquitty::Codec::Zlib::compressBound(bytes.size()));
    stream.resize(Ubiquitty::Codec::Zlib::compressInto(bytes, stream));
    return stream;
}

TEST(aDeflatedImageInflatesAsItArrivesIntoExactlyItsDecodedSize)
{
    const DashboardChart& chart = dashboardCharts[0];
    const auto pixels = chartPixels(chart, 3);
    const ImageAssetIdentity asset { .hash = imageHash(pixels, 32, dashboardChartWidth, chart.rows * 19), .format = 32, .width = dashboardChartWidth, .height = chart.rows * 19 };
    const auto stream = zlibStream(pixels);
    CHECK(stream.size() * 10 < pixels.size());
    const auto assembly = ImageTransfer::assemblyBytes(asset, Compression::Deflate);
    REQUIRE(assembly && *assembly > pixels.size());
    CHECK(!ImageTransfer::begin({ .asset = asset, .transfer = 7 }, *assembly - 1, Compression::Deflate));
    for (const std::size_t chunkBytes : { std::size_t { 1 }, std::size_t { 97 }, stream.size() }) {
        auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, *assembly, Compression::Deflate);
        REQUIRE(transfer);
        CHECK_EQ(transfer->heldBytes(), *assembly);
        const auto results = feedDeflated(*transfer, stream, chunkBytes, static_cast<std::uint32_t>(pixels.size()));
        REQUIRE(!results.empty());
        CHECK(results.back() == ImageChunkResult::Complete);
        CHECK(std::ranges::all_of(std::span(results).first(results.size() - 1), [](auto result) { return result == ImageChunkResult::Pending; }));
        const auto complete = transfer->take();
        REQUIRE(complete);
        CHECK(complete->asset == asset);
        CHECK(std::ranges::equal(complete->pixels(), pixels));
        CHECK_EQ(transfer->heldBytes(), 0u);
    }
}

TEST(aDeflatedImageRefusesEveryStreamThatIsNotExactlyItsPixels)
{
    std::vector<std::uint8_t> pixels(64 * 32 * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = static_cast<std::uint8_t>((index / 4) % 7 * 31);
    const ImageAssetIdentity asset { .hash = imageHash(pixels, 32, 64, 32), .format = 32, .width = 64, .height = 32 };
    const auto inflatedBytes = static_cast<std::uint32_t>(pixels.size());
    const auto refuses = [&](std::span<const std::uint8_t> wire, std::uint32_t declaredBytes, Compression declared, std::size_t chunkBytes) {
        auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, *ImageTransfer::assemblyBytes(asset, Compression::Deflate), Compression::Deflate);
        CHECK(transfer);
        if (!transfer)
            return false;
        const auto results = feedDeflated(*transfer, wire, chunkBytes, declaredBytes, declared);
        const bool isRefused = !results.empty() && results.back() == ImageChunkResult::Refused;
        CHECK(isRefused);
        CHECK_EQ(transfer->heldBytes(), 0u);
        CHECK(!transfer->take());
        return isRefused;
    };
    const auto exact = zlibStream(pixels);
    // The declaration on the last chunk must be this transfer's.
    CHECK(refuses(exact, inflatedBytes + 1, Compression::Deflate, 64));
    CHECK(refuses(exact, 0, Compression::None, 64));
    // A stream of more pixels is refused at the chunk that overruns, before
    // the stream ends and before any byte lands past the decoded size.
    auto longer = pixels;
    longer.insert(longer.end(), pixels.begin(), pixels.begin() + 4);
    const auto overrun = zlibStream(longer);
    {
        auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, *ImageTransfer::assemblyBytes(asset, Compression::Deflate), Compression::Deflate);
        REQUIRE(transfer);
        const auto results = feedDeflated(*transfer, overrun, 1, inflatedBytes);
        REQUIRE(!results.empty());
        CHECK(results.back() == ImageChunkResult::Refused);
        CHECK(results.size() < overrun.size());
    }
    // A stream of fewer pixels ends short.
    const auto shorter = zlibStream(std::span(pixels).first(pixels.size() - 4));
    CHECK(refuses(shorter, inflatedBytes, Compression::Deflate, 64));
    // Bytes past the end of the stream, in its last chunk or a chunk of their own.
    auto trailing = exact;
    trailing.push_back(0);
    CHECK(refuses(trailing, inflatedBytes, Compression::Deflate, trailing.size()));
    CHECK(refuses(trailing, inflatedBytes, Compression::Deflate, exact.size()));
    // A stream that wants more wire bytes than any encoding of the pixels
    // needs: empty stored blocks inflate to nothing, so only the wire bound
    // ends them.
    std::vector<std::uint8_t> padded { 0x78, 0x01 };
    while (padded.size() <= Ubiquitty::Codec::Zlib::compressBound(pixels.size()))
        padded.insert(padded.end(), { 0x00, 0x00, 0x00, 0xFF, 0xFF });
    CHECK(refuses(padded, inflatedBytes, Compression::Deflate, 1024));
    // A damaged stream fails its inflation or its Adler-32.
    auto damaged = exact;
    damaged[damaged.size() / 2] ^= 0x40;
    CHECK(refuses(damaged, inflatedBytes, Compression::Deflate, 64));
    // A raw transfer refuses a deflated tail, and a deflated one a raw tail.
    auto raw = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, pixels.size());
    REQUIRE(raw);
    std::array<std::uint8_t, 32> digest {};
    Ubiquitty::Crypto::sha256(pixels, digest);
    CHECK(raw->accept({ .transfer = 7, .index = 0, .isLast = true, .bytes = pixels, .sha256 = digest, .compression = Compression::Deflate, .inflatedBytes = inflatedBytes }) == ImageChunkResult::Refused);
    CHECK(refuses(pixels, 0, Compression::None, pixels.size()));
}

TEST(aDeflatedTransferWhoseDigestDisagreesIsRefusedAfterItInflates)
{
    std::vector<std::uint8_t> pixels(16 * 16 * 4, 9);
    const ImageAssetIdentity asset { .hash = imageHash(pixels, 32, 16, 16), .format = 32, .width = 16, .height = 16 };
    const auto stream = zlibStream(pixels);
    auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 7 }, *ImageTransfer::assemblyBytes(asset, Compression::Deflate), Compression::Deflate);
    REQUIRE(transfer);
    // The digest names the bytes as they crossed, not the decoded pixels.
    std::array<std::uint8_t, 32> pixelDigest {};
    Ubiquitty::Crypto::sha256(pixels, pixelDigest);
    CHECK(transfer->accept({ .transfer = 7, .index = 0, .isLast = true, .bytes = stream, .sha256 = pixelDigest, .compression = Compression::Deflate, .inflatedBytes = static_cast<std::uint32_t>(pixels.size()) })
        == ImageChunkResult::Refused);
    CHECK_EQ(transfer->heldBytes(), 0u);
}

TEST(deflatedChartsCrossAsAFractionOfTheirPixelsAndArriveExact)
{
    ImageConnectionLink raw(nullptr, Compression::None);
    ImageConnectionLink deflated(nullptr, Compression::Deflate);
    for (ImageConnectionLink* link : { &raw, &deflated }) {
        link->attach();
        link->write(dashboardFrame(0));
        link->settle();
        REQUIRE(link->terminal());
        CHECK(showsEveryChart(*link->terminal()));
        for (const DashboardChart& chart : dashboardCharts)
            CHECK(shownChartFrame(*link->terminal(), chart) == std::optional<std::uint32_t> { 0 });
        CHECK_EQ(link->pixelTransfers, dashboardCharts.size());
    }
    std::size_t decodedBytes = 0;
    for (const DashboardChart& chart : dashboardCharts)
        decodedBytes += chartPixels(chart, 0).size();
    CHECK_EQ(raw.pixelBytes, decodedBytes);
    CHECK_EQ(raw.host.connection->counters().imageWireBytesQueued, decodedBytes);
    CHECK_EQ(deflated.host.connection->counters().imagePixelBytesQueued, decodedBytes);
    CHECK_EQ(deflated.host.connection->counters().imageWireBytesQueued, deflated.pixelBytes);
    CHECK(deflated.pixelBytes * 10 < decodedBytes);
    CHECK_EQ(deflated.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(deflated.client.connection()->inboundImageBytes(), 0u);
}

TEST(aVirtualImageReplicaResolvesItsPlaceholderGeometry)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,U=1,q=2,f=32,s=2,v=2,c=2,r=2,i=9,m=1"));
    link.write(kitty("m=0", fourPixels));
    link.write("\033[H\033[38;5;9m\U0010EEEE\u0305\u0305");
    link.attach();
    REQUIRE(link.terminal());
    const auto* image = findImage(*link.terminal(), 9);
    REQUIRE(image);
    CHECK(image->virtualPlacement && image->displayed);
    CHECK(image->virtualColumns == 2 && image->virtualRows == 2);
    CHECK_EQ(Image::placementCount(link.terminal()), 0);
    std::array<Image::ImageQuad, 4> quads {};
    const auto count = Ubiquitty::Rendering::collectImageQuads(link.terminal(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, quads);
    CHECK_EQ(count, 1);
    if (count == 1)
        CHECK(quads[0].u1 == 0.5f && quads[0].v1 == 0.5f);
}

// The record sizes the editors' clients are generated from are the sizes the
// encoder writes: one more binding or placement adds exactly one record.
TEST(imagePlacementsRecordsAreTheSizesTheEditorsAreGeneratedFrom)
{
    ImagePlacements state;
    state.revision = 1;
    const ImageAssetIdentity asset { .hash = { 1, 2 }, .format = 24, .width = 2, .height = 2 };
    const auto encoded = [&] {
        CHECK(isValid(state));
        std::vector<std::uint8_t> body;
        encode(body, state);
        return body.size();
    };
    state.imageCount = 1;
    state.images[0] = { .imageID = 7, .imageNumber = 0, .asset = asset, .isVirtual = false, .virtualColumns = 0, .virtualRows = 0, .virtualPlacementID = 0 };
    const std::size_t withoutPlacements = encoded();
    state.placementCount = 1;
    state.placements[0].hash = asset.hash;
    state.placements[0].geometry.used = true;
    state.placements[0].geometry.imageID = 7;
    state.placements[0].geometry.drawnPixelWidth = 24;
    state.placements[0].geometry.drawnPixelHeight = 24;
    const std::size_t withOnePlacement = encoded();
    state.placementCount = 2;
    state.placements[1] = state.placements[0];
    CHECK_EQ(withoutPlacements, 4 + 1 + imageBindingRecordBytes + 1);
    CHECK_EQ(withOnePlacement - withoutPlacements, imagePlacementRecordBytes);
    CHECK_EQ(encoded() - withOnePlacement, imagePlacementRecordBytes);
}

TEST(imageKindsUseTheSpecifiedNumbersAndDirections)
{
    CHECK_EQ(std::to_underlying(FrameKind::ImageHead), 52u);
    CHECK_EQ(std::to_underlying(FrameKind::ImagePlacements), 53u);
    CHECK_EQ(std::to_underlying(FrameKind::ImageWant), 75u);
    // Read the independently specified bytes, not the same enum that encodes
    // them: a shared sender/reader numbering mistake must fail this contract.
    CHECK(directionOf(static_cast<FrameKind>(52)) == Direction::HostToClient);
    CHECK(directionOf(static_cast<FrameKind>(53)) == Direction::HostToClient);
    CHECK(directionOf(static_cast<FrameKind>(75)) == Direction::ClientToHost);
    for (std::uint8_t value : { 52, 53, 75 }) {
        const auto kind = frameKindOf(value);
        REQUIRE(kind);
        CHECK(laneOf(*kind) == Lane::Interactive);
        CHECK(!isConnectionKind(*kind));
    }
}

TEST(aConnectionWithoutImageAdmissionEmitsNoImageFrames)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    link.host.accept();
    (void)link.client.connect(link.now);
    link.pump();
    link.pump();
    REQUIRE(link.host.connection->bindPane(*link.host.pane.source) != 0);
    link.settle();
    CHECK_EQ(link.placementRecords, 0u);
    CHECK_EQ(link.offers, 0u);
    CHECK_EQ(link.pixelTransfers, 0u);
    CHECK_EQ(link.pixelBytes, 0u);
}

TEST(realImageFramesMoveCropScrollAndReidentifyWithoutResendingPixels)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    link.attach();
    REQUIRE(link.terminal());
    const auto* image = findImage(*link.terminal(), 7);
    REQUIRE(image);
    CHECK(std::equal(fourPixels.begin(), fourPixels.end(), image->pixels));
    CHECK_EQ(link.pixelTransfers, 1u);
    CHECK_EQ(link.pixelBytes, fourPixels.size());
    CHECK_EQ(link.wants, 0u);

    link.write(kitty("a=d,d=i,i=7,q=2"));
    link.write("\033[3;4H" + kitty("a=p,i=7,p=12,q=2,C=1,c=3,r=4,x=1,y=1,w=1,h=1,X=2,Y=3,z=-7"));
    link.settle();
    REQUIRE(Image::placementCount(link.terminal()) == 1);
    const auto& placement = link.terminal()->images.placements[0];
    CHECK_EQ(placement.sourceX, 1);
    CHECK_EQ(placement.sourceY, 1);
    CHECK_EQ(placement.pixelOffsetX, 2);
    CHECK_EQ(placement.pixelOffsetY, 3);
    std::array<Image::ImageQuad, 2> quads {};
    CHECK_EQ(Image::resolveImagePlacements(link.terminal()->images.stored, link.terminal()->images.placements,
                 { .columns = 80, .rows = 24 }, { 8, 16 }, { 0, 0 }, 0, quads, {}, false),
        1);
    CHECK_EQ(quads[0].minXInPixels, 26.0f);
    CHECK_EQ(quads[0].minYInPixels, 35.0f);
    CHECK_EQ(quads[0].u0, 0.5f);
    CHECK_EQ(quads[0].v0, 0.5f);
    CHECK(quads[0].straightAlpha);
    CHECK_EQ(link.pixelBytes, fourPixels.size());

    link.write("\033[24;1H\n");
    link.settle();
    REQUIRE(Image::placementCount(link.terminal()) == 1);
    CHECK_EQ(link.terminal()->images.placements[0].cursorY, 1);
    CHECK_EQ(link.pixelBytes, fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 1u);
    // The new ID carries the same decoded content. The host may have accepted
    // another local upload, but the connection sends no duplicate pixel body.
    link.write(kitty("a=T,f=32,s=2,v=2,i=9,q=2,C=1", fourPixels) + kitty("a=d,d=I,i=7,q=2"));
    link.settle();
    CHECK(!findImage(*link.terminal(), 7));
    REQUIRE(findImage(*link.terminal(), 9));
    CHECK_EQ(link.pixelBytes, fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 1u);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
}

TEST(rebindingAPaneMovesItsImagesBeforeThePreviousTerminalDies)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=71,c=2,r=2,q=2", fourPixels));
    link.attach();
    REQUIRE(link.terminal());
    REQUIRE(findImage(*link.terminal(), 71));
    const auto* pixels = findImage(*link.terminal(), 71)->pixels;
    const auto transfers = link.pixelTransfers;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    auto borrowed = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*borrowed, 12, 40);
    link.client.connection()->setPaneTerminal(handle, borrowed.get());
    REQUIRE(findImage(*borrowed, 71));
    CHECK(findImage(*borrowed, 71)->pixels == pixels);
    link.settle();
    CHECK_EQ(link.pixelTransfers, transfers);

    // Releasing a view keeps the pane on the carrier. Its former terminal can
    // die immediately; later placement revisions and pixels must use the new one.
    link.client.connection()->setPaneTerminal(handle, nullptr);
    borrowed.reset();
    link.write(kitty("a=p,i=71,p=2,c=3,r=2,q=2"));
    link.settle();
    const auto* carried = link.client.connection()->paneTerminal(handle);
    REQUIRE(carried);
    REQUIRE(findImage(*carried, 71));
    CHECK(findImage(*carried, 71)->pixels == pixels);
    CHECK_EQ(link.pixelTransfers, transfers);
    link.write(kitty("a=d,d=I,i=71,q=2"));
    link.settle();
    CHECK(!findImage(*carried, 71));
}

TEST(imageDeliveryAfterRebindingCannotReachTheFreedTerminal)
{
    ImageConnectionLink link;
    link.attach();
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    auto borrowed = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*borrowed, 12, 40);
    link.client.connection()->setPaneTerminal(handle, borrowed.get());
    link.write(kitty("a=T,f=32,s=2,v=2,i=71,c=2,r=2,q=2", fourPixels));
    link.settle();
    REQUIRE(findImage(*borrowed, 71));
    link.client.connection()->setPaneTerminal(handle, nullptr);
    borrowed.reset();
    link.write(kitty("a=d,d=I,i=71,q=2"));
    link.settle();
    REQUIRE(link.client.connection()->paneTerminal(handle));
    CHECK(!findImage(*link.client.connection()->paneTerminal(handle), 71));
}

TEST(rebindingOneAliasKeepsEveryBorrowerOfItsTerminalAlive)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=71,c=2,r=2,q=2", fourPixels));
    link.attach();
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    const auto alias = handle + 1;
    // A second legal bound handle shares the retained pane's exact picture.
    // Deliver only its bind and placement frames: this test exercises client
    // ownership, not the host's scheduling of two picture baselines.
    link.receive(FrameKind::PaneBind, 0, PaneBind { .handle = alias, .paneID = UbiquittyTest::AttachmentLink::paneID });
    auto state = link.host.pane.source->images().state();
    link.receive(FrameKind::ImagePlacements, alias, state);
    REQUIRE(link.client.state() == ClientAttachmentState::Live);
    REQUIRE(link.client.connection()->paneTerminal(alias) == link.client.connection()->paneTerminal(handle));
    REQUIRE(findImage(*link.client.connection()->paneTerminal(alias), 71));
    const auto* pixels = findImage(*link.client.connection()->paneTerminal(alias), 71)->pixels;

    auto borrowed = std::make_unique<Ubiquitty::Terminal>();
    link.client.connection()->setPaneTerminal(handle, borrowed.get());
    for (const auto bound : { handle, alias }) {
        const auto* terminal = link.client.connection()->paneTerminal(bound);
        REQUIRE(terminal);
        REQUIRE(findImage(*terminal, 71));
        CHECK(findImage(*terminal, 71)->pixels == pixels);
    }

    // Releasing the other alias must also retire every borrow of the view;
    // neither the first handle nor a subsequent alias placement may touch it.
    link.client.connection()->setPaneTerminal(alias, nullptr);
    borrowed.reset();
    for (const auto bound : { handle, alias }) {
        const auto* terminal = link.client.connection()->paneTerminal(bound);
        REQUIRE(terminal);
        REQUIRE(findImage(*terminal, 71));
        CHECK(findImage(*terminal, 71)->pixels == pixels);
    }
    ++state.revision;
    state.imageCount = 0;
    state.placementCount = 0;
    link.receive(FrameKind::ImagePlacements, alias, state);
    CHECK(link.client.state() == ClientAttachmentState::Live);
    CHECK(!findImage(*link.client.connection()->paneTerminal(alias), 71));
    link.receive(FrameKind::PaneUnbind, 0, PaneUnbind { .handle = alias });
    ++state.revision;
    link.receive(FrameKind::ImagePlacements, handle, state);
    CHECK(link.client.state() == ClientAttachmentState::Live);
    REQUIRE(link.client.connection()->paneTerminal(handle));
    CHECK(!findImage(*link.client.connection()->paneTerminal(handle), 71));
}

TEST(bindingAnAliasWithANewDestinationRetiresExistingTerminalBorrows)
{
    struct Destination final : ClientPaneDelegate {
        Ubiquitty::Terminal* terminal = nullptr;
        Ubiquitty::Terminal* terminalForPane(ID128) override { return terminal; }
    } destination;
    auto borrowed = std::make_unique<Ubiquitty::Terminal>();
    ImageConnectionLink link(&destination);
    link.write(kitty("a=T,f=32,s=2,v=2,i=71,c=2,r=2,q=2", fourPixels));
    link.attach();
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    const auto alias = handle + 1;
    destination.terminal = borrowed.get();
    link.receive(FrameKind::PaneBind, 0, PaneBind { .handle = alias, .paneID = UbiquittyTest::AttachmentLink::paneID });
    REQUIRE(link.client.connection()->paneTerminal(handle) == borrowed.get());
    REQUIRE(link.client.connection()->paneTerminal(alias) == borrowed.get());
    REQUIRE(findImage(*borrowed, 71));
    auto state = link.host.pane.source->images().state();
    ++state.revision;
    state.imageCount = 0;
    state.placementCount = 0;
    link.receive(FrameKind::ImagePlacements, handle, state);
    CHECK(!findImage(*borrowed, 71));
    link.client.connection()->setPaneTerminal(alias, nullptr);
    destination.terminal = nullptr;
    borrowed.reset();
    link.receive(FrameKind::ImagePlacements, alias, state);
    CHECK(link.client.state() == ClientAttachmentState::Live);
}

TEST(rebindingAPaneRechecksPartialImageReservationsAgainstItsNewBudget)
{
    for (const std::uint32_t budget : { 8u, 16u, 32u }) {
        ImageConnectionLink link;
        link.attach();
        const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
        ImageLink picture;
        auto second = fourPixels;
        second[0] = 17;
        picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels) + kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", second));
        REQUIRE(picture.publisher.capture());
        auto state = picture.publisher.state();
        state.revision = 2;
        const auto firstAsset = state.images[0].asset;
        const auto secondAsset = state.images[1].asset;
        link.receive(FrameKind::ImagePlacements, handle, state);
        link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = firstAsset, .transfer = 100 });
        REQUIRE(link.client.connection()->inboundImageBytes() == 16u);
        auto borrowed = std::make_unique<Ubiquitty::Terminal>();
        borrowed->images.kittyImageBytesBudget = budget;
        link.client.connection()->setPaneTerminal(handle, borrowed.get());
        CHECK_EQ(link.client.connection()->inboundImageBytes(), budget < 16 ? 0u : 16u);
        link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = secondAsset, .transfer = 101 });
        CHECK_EQ(link.client.connection()->inboundImageBytes(), budget < 16 ? 0u : budget);
        BulkChunk first { .transfer = 100, .index = 0, .isLast = true, .bytes = fourPixels };
        Ubiquitty::Crypto::sha256(fourPixels, first.sha256);
        link.receive(FrameKind::BulkChunk, 0, first);
        BulkChunk last { .transfer = 101, .index = 0, .isLast = true, .bytes = second };
        Ubiquitty::Crypto::sha256(second, last.sha256);
        link.receive(FrameKind::BulkChunk, 0, last);
        CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
        CHECK_EQ(Image::totalBytes(borrowed.get()), budget < 16 ? 0u : budget);
        CHECK((findImage(*borrowed, 7) != nullptr) == (budget >= 16));
        CHECK((findImage(*borrowed, 8) != nullptr) == (budget >= 32));
        CHECK(link.client.state() == ClientAttachmentState::Live);
        // Destroy the view before the carrier, as native pane teardown does.
        link.client.connection()->setPaneTerminal(handle, nullptr);
    }
}

TEST(rebindingToASmallerBudgetEvictsPixelsButKeepsTheImageBinding)
{
    ImageConnectionLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=71,c=2,r=2,q=2", fourPixels));
    link.attach();
    REQUIRE(findImage(*link.terminal(), 71));
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    auto borrowed = std::make_unique<Ubiquitty::Terminal>();
    borrowed->images.kittyImageBytesBudget = 8;
    link.client.connection()->setPaneTerminal(handle, borrowed.get());
    CHECK_EQ(Image::totalBytes(borrowed.get()), 0u);
    link.settle();
    CHECK_EQ(Image::totalBytes(borrowed.get()), 0u);
    link.client.connection()->setPaneTerminal(handle, nullptr);
    borrowed.reset();
    link.settle();
    const auto* terminal = link.client.connection()->paneTerminal(handle);
    REQUIRE(terminal);
    REQUIRE(findImage(*terminal, 71));
    CHECK_EQ(link.pixelTransfers, 2u);
}

TEST(realImageResumeRequestsOnlyAnEvictedAssetAndDeletionCannotResurrectPixels)
{
    ImageConnectionLink link;
    auto second = fourPixels;
    second[0] = 17;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels) + kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", second));
    link.attach();
    REQUIRE(link.terminal());
    CHECK_EQ(link.pixelBytes, 2 * fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 2u);
    link.reconnect();
    CHECK_EQ(link.pixelBytes, 2 * fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 2u);
    CHECK_EQ(link.wants, 0u);
    CHECK_EQ(link.offers, 2u);

    // Model an actual client cache eviction while disconnected, not a forged
    // cached-image list in Greet. Only the missing identity is requested.
    UbiquittyTest::AttachmentLink::drop(link.host, link.client, link.now);
    auto* retained = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(retained);
    for (auto& image : retained->images.stored) {
        if (image.used && image.imageID == 8)
            Image::dispose(&image);
    }
    link.accept();
    link.settle();
    CHECK_EQ(link.pixelBytes, 3 * fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 3u);
    CHECK_EQ(link.wants, 1u);
    REQUIRE(findImage(*link.terminal(), 7));
    REQUIRE(findImage(*link.terminal(), 8));

    link.write(kitty("a=d,d=I,i=8,q=2"));
    link.settle();
    CHECK(!findImage(*link.terminal(), 8));
    CHECK_EQ(link.pixelBytes, 3 * fourPixels.size());
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", second));
    link.settle();
    REQUIRE(findImage(*link.terminal(), 8));
    CHECK_EQ(link.pixelBytes, 4 * fourPixels.size());
    CHECK_EQ(link.pixelTransfers, 4u);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
}

TEST(simultaneousImageHeadsReserveEveryDestinationBeforeTakingPixels)
{
    ImageConnectionLink link;
    link.attach();
    auto* terminal = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(terminal);
    terminal->images.kittyImageBytesBudget = 24;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    auto second = fourPixels;
    second[0] = 17;
    ImageLink picture;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels) + kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", second));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    const auto firstAsset = state.images[0].asset;
    const auto secondAsset = state.images[1].asset;
    link.receive(FrameKind::ImagePlacements, handle, state);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = firstAsset, .transfer = 100 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = secondAsset, .transfer = 101 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    CHECK_EQ(Image::totalBytes(terminal), 0u);
    BulkChunk refused { .transfer = 101, .index = 0, .isLast = true, .bytes = second };
    Ubiquitty::Crypto::sha256(second, refused.sha256);
    link.receive(FrameKind::BulkChunk, 0, refused);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    BulkChunk accepted { .transfer = 100, .index = 0, .isLast = true, .bytes = fourPixels };
    Ubiquitty::Crypto::sha256(fourPixels, accepted.sha256);
    link.receive(FrameKind::BulkChunk, 0, accepted);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(Image::totalBytes(terminal), 16u);
    CHECK(findImage(*terminal, 7));
    CHECK(!findImage(*terminal, 8));

    // Removing the installed first asset creates room. The retained offer for
    // the refused second one is requested without a new host ImageHead.
    picture.write(kitty("a=d,d=I,i=7,q=2"));
    REQUIRE(picture.publisher.capture());
    state = picture.publisher.state();
    state.revision = 3;
    (void)link.client.takeStreamOutput();
    link.receive(FrameKind::ImagePlacements, handle, state);
    FrameDecoder decoder;
    decoder.append(link.client.takeStreamOutput());
    std::size_t wanted = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind == FrameKind::ImageWant) {
            const auto want = decodeImageWant(frame->body);
            REQUIRE(want);
            CHECK(want->hash == secondAsset.hash);
            ++wanted;
        }
    }
    CHECK_EQ(wanted, 1u);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = secondAsset, .transfer = 102 });
    refused.transfer = 102;
    link.receive(FrameKind::BulkChunk, 0, refused);
    CHECK_EQ(Image::totalBytes(terminal), 16u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK(!findImage(*terminal, 7));
    CHECK(findImage(*terminal, 8));
    CHECK(link.client.connection()->state() == ConnectionState::Attached);
}

TEST(aRefusedImageWaitsForItsOwnFinalBeforeRetryAndAnAbortWaitsForANewOffer)
{
    ImageConnectionLink link;
    link.attach();
    auto* terminal = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(terminal);
    terminal->images.kittyImageBytesBudget = 16;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    auto second = fourPixels;
    second[0] = 71;
    ImageLink picture;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels) + kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", second));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    const auto first = state.images[0].asset;
    const auto next = state.images[1].asset;
    link.receive(FrameKind::ImagePlacements, handle, state);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = first, .transfer = 100 });
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = next, .transfer = 101 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    const auto takeWants = [&] {
        std::vector<Digest128> wants;
        FrameDecoder decoder;
        decoder.append(link.client.takeStreamOutput());
        while (const auto frame = decoder.next()) {
            if (frame->kind == FrameKind::ImageWant) {
                const auto want = decodeImageWant(frame->body);
                CHECK(want.has_value());
                if (want)
                    wants.push_back(want->hash);
            }
        }
        return wants;
    };
    (void)takeWants();
    terminal->images.kittyImageBytesBudget = 32;
    BulkChunk completed { .transfer = 100, .index = 0, .isLast = true, .bytes = fourPixels };
    Ubiquitty::Crypto::sha256(fourPixels, completed.sha256);
    link.receive(FrameKind::BulkChunk, 0, completed);
    CHECK(takeWants().empty()); // Transfer 101 still occupies its discard slot.
    BulkChunk discarded { .transfer = 101, .index = 0, .isLast = true, .bytes = second };
    Ubiquitty::Crypto::sha256(second, discarded.sha256);
    link.receive(FrameKind::BulkChunk, 0, discarded);
    CHECK(takeWants() == std::vector<Digest128> { next.hash });
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = next, .transfer = 102 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    link.receive(FrameKind::BulkAbort, 0, BulkAbort { .transfer = 102 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK(takeWants().empty()); // The host has not offered a new delivery yet.
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = next, .transfer = 0 });
    CHECK(takeWants() == std::vector<Digest128> { next.hash });
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = next, .transfer = 103 });
    discarded.transfer = 103;
    link.receive(FrameKind::BulkChunk, 0, discarded);
    REQUIRE(findImage(*terminal, 7));
    REQUIRE(findImage(*terminal, 8));
    CHECK_EQ(Image::totalBytes(terminal), 32u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
}

TEST(discardedImageTransfersUseTheSameFiniteConnectionSlotsAsPixelOwners)
{
    ImageConnectionLink link;
    link.attach();
    auto* terminal = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(terminal);
    terminal->images.kittyImageBytesBudget = 0;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    ImageLink picture;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    link.receive(FrameKind::ImagePlacements, handle, state);
    for (std::uint32_t transfer = 1; transfer <= Image::maxTerminalImageCount; ++transfer) {
        link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[0].asset, .transfer = 100 + transfer });
        CHECK(link.client.connection()->state() == ConnectionState::Attached);
        CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    }
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[0].asset, .transfer = 200 });
    CHECK(link.client.connection()->state() == ConnectionState::Closed);
    CHECK(link.client.connection()->byeReason() == ByeReason::TransferRefused);
}

TEST(abortingADiscardedImageSlotRetriesAnotherEligibleAsset)
{
    ImageConnectionLink link;
    link.attach();
    auto* terminal = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(terminal);
    terminal->images.kittyImageBytesBudget = 0;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    ImageLink picture;
    auto other = fourPixels;
    other[0] = 71;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels) + kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", other));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    link.receive(FrameKind::ImagePlacements, handle, state);
    for (std::uint32_t transfer = 1; transfer <= Image::maxTerminalImageCount; ++transfer)
        link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[0].asset, .transfer = 100 + transfer });
    terminal->images.kittyImageBytesBudget = fourPixels.size();
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[1].asset, .transfer = 0 });
    FrameDecoder before;
    before.append(link.client.takeStreamOutput());
    while (const auto frame = before.next())
        CHECK(frame->kind != FrameKind::ImageWant);

    link.receive(FrameKind::BulkAbort, 0, BulkAbort { .transfer = 101 });
    CHECK(link.client.connection()->state() == ConnectionState::Attached);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    FrameDecoder after;
    after.append(link.client.takeStreamOutput());
    std::size_t wants = 0;
    while (const auto frame = after.next()) {
        if (frame->kind != FrameKind::ImageWant)
            continue;
        const auto want = decodeImageWant(frame->body);
        REQUIRE(want);
        CHECK(want->hash == state.images[1].asset.hash);
        ++wants;
    }
    CHECK_EQ(wants, 1u);
}

TEST(addingDuplicateBindingsRetiresAnUnaffordablePartialImageAtomically)
{
    ImageConnectionLink link;
    link.attach();
    auto* terminal = const_cast<Ubiquitty::Terminal*>(link.terminal());
    REQUIRE(terminal);
    terminal->images.kittyImageBytesBudget = 16;
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    ImageLink picture;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    const auto asset = state.images[0].asset;
    link.receive(FrameKind::ImagePlacements, handle, state);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = asset, .transfer = 100 });
    link.receive(FrameKind::BulkChunk, 0, BulkChunk { .transfer = 100, .index = 0, .isLast = false, .bytes = std::span(fourPixels).first(7) });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    state.images[1] = state.images[0];
    state.images[1].imageID = 8;
    state.imageCount = 2;
    state.revision = 3;
    link.receive(FrameKind::ImagePlacements, handle, state);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(Image::totalBytes(terminal), 0u);
    CHECK_EQ(Image::placementCount(terminal), 0);
    BulkChunk final { .transfer = 100, .index = 1, .isLast = true, .bytes = std::span(fourPixels).subspan(7) };
    Ubiquitty::Crypto::sha256(fourPixels, final.sha256);
    link.receive(FrameKind::BulkChunk, 0, final);
    CHECK_EQ(Image::totalBytes(terminal), 0u);
    CHECK(link.client.connection()->state() == ConnectionState::Attached);
}

TEST(unbindingAPaneReleasesItsPartialImageAndFencesItsFinalChunk)
{
    ImageConnectionLink link;
    link.attach();
    const auto handle = link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID);
    ImageLink picture;
    picture.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(picture.publisher.capture());
    auto state = picture.publisher.state();
    state.revision = 2;
    link.receive(FrameKind::ImagePlacements, handle, state);
    link.receive(FrameKind::ImageHead, handle, ImageHead { .asset = state.images[0].asset, .transfer = 100 });
    link.receive(FrameKind::BulkChunk, 0, BulkChunk { .transfer = 100, .index = 0, .isLast = false, .bytes = std::span(fourPixels).first(7) });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 16u);
    link.receive(FrameKind::PaneUnbind, 0, PaneUnbind { .handle = handle, .reason = 0 });
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(link.client.connection()->handleForPane(UbiquittyTest::AttachmentLink::paneID), 0u);
    BulkChunk final { .transfer = 100, .index = 1, .isLast = true, .bytes = std::span(fourPixels).subspan(7) };
    Ubiquitty::Crypto::sha256(fourPixels, final.sha256);
    link.receive(FrameKind::BulkChunk, 0, final);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK(link.client.connection()->state() == ConnectionState::Attached);
}

TEST(aHigherPriorityReplyPreemptsImageOwnersThenMissingPixelsEventuallyComplete)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 91);
    link.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    for (int pass = 0; pass < 20 && link.pixelBytes == 0; ++pass)
        link.pump(1);
    REQUIRE(link.pixelBytes > 0 && link.pixelBytes < pixels.size());
    REQUIRE(link.connectionBudget->heldBytes() >= pixels.size());
    REQUIRE(link.aggregateBudget->heldBytes() >= pixels.size());
    REQUIRE(link.client.connection()->inboundImageBytes() == pixels.size());
    CHECK(!findImage(*link.terminal(), 7));
    const auto firstTransfer = link.imageTransfers.front();

    REQUIRE(link.client.connection()->call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"fixture\"}"));
    link.host.connection->receiveStream(link.client.takeStreamOutput(), ++link.now);
    // Admission of the call frees the immutable pixel owners before its
    // higher-priority response is built; it does not pretend client pixels
    // or already-delivered chunks have been freed.
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), pixels.size());
    const auto calls = link.host.connection->takeCalls();
    REQUIRE(calls.size() == 1);
    const std::string response(maxFrameBodyBytes + 100, 'x');
    CHECK(link.host.connection->answerCall(calls[0].callIndex, response) == CallAnswer::Sent);
    link.settle();
    const auto results = link.client.connection()->takeResults();
    REQUIRE(results.size() == 1);
    CHECK(results[0] == response);
    const auto* image = findImage(*link.terminal(), 7);
    REQUIRE(image);
    CHECK_EQ(image->pixelLength, pixels.size());
    CHECK(std::equal(pixels.begin(), pixels.end(), image->pixels));
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
    CHECK_EQ(link.host.connection->counters().bulkAborts, 1u);
    CHECK_EQ(link.pixelTransfers, 2u);
    REQUIRE(link.announcedImageTransfers.size() == 2);
    CHECK_EQ(link.announcedImageTransfers[0], firstTransfer);
    CHECK(link.announcedImageTransfers[1] != firstTransfer);
    CHECK(link.pixelBytes > pixels.size());
    CHECK(std::ranges::find(link.imageTransfers, firstTransfer) == link.imageTransfers.end());
}

TEST(replacingVideoFramesFinishPixelsWhileTheProducerKeepsRunning)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 91);
    std::size_t visibleFrames = 0;
    bool hasPresented = false;
    std::uint32_t presentedImage = 0;
    std::size_t completedPictures = 0;
    constexpr std::uint32_t lastImage = 96;
    for (std::uint32_t id = 1; id <= lastImage; ++id) {
        pixels[0] = static_cast<std::uint8_t>(id);
        std::string output = "\033[H" + kitty("a=T,f=32,s=256,v=256,i=" + std::to_string(id) + ",q=2,C=1", pixels);
        if (id > 1)
            output += kitty("a=d,d=I,i=" + std::to_string(id - 1) + ",q=2");
        Ubiquitty::processBytes(&link.host.pane.terminal, output);
        link.host.pane.source->publish(FrameClass::Boundary, link.now);
        // The producer replaces an image more quickly than this carrier can
        // deliver it. Each pass grants only one real bulk chunk of progress.
        link.pump(1);
        const bool isVisible = Image::placementCount(link.terminal()) != 0;
        if (hasPresented)
            CHECK(isVisible);
        hasPresented |= isVisible;
        visibleFrames += isVisible ? 1 : 0;
        if (isVisible && link.terminal()->images.placements[0].imageID != presentedImage) {
            presentedImage = link.terminal()->images.placements[0].imageID;
            ++completedPictures;
        }
        CHECK(link.connectionBudget->heldBytes() <= maxCatalogMessageBytes);
        CHECK(link.client.connection()->inboundImageBytes() <= maxCatalogMessageBytes);
    }
    CHECK(visibleFrames > 0);
    CHECK(completedPictures >= 2);
    CHECK(link.pixelBytes > pixels.size());
    link.settle();
    const auto* final = findImage(*link.terminal(), lastImage);
    REQUIRE(final);
    CHECK(std::equal(pixels.begin(), pixels.end(), final->pixels));
    CHECK_EQ(Image::placementCount(link.terminal()), 1);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
}

TEST(chartsReplacedInPlaceKeepTheirPicturesUntilTheirReplacementsArrive)
{
    for (const Compression coding : { Compression::None, Compression::Deflate }) {
        ImageConnectionLink link(nullptr, coding);
        link.attach();
        link.write(dashboardFrame(0));
        link.settle();
        REQUIRE(link.terminal());
        REQUIRE(showsEveryChart(*link.terminal()));
        for (const DashboardChart& chart : dashboardCharts)
            REQUIRE(shownChartFrame(*link.terminal(), chart) == std::optional<std::uint32_t> { 0 });
        for (std::uint32_t frame = 1; frame <= 6; ++frame) {
            link.write(dashboardFrame(frame));
            // A link that carries one chunk a pass: every pass in between shows
            // each chart's previous verified picture or its new one, never none.
            bool hasArrived = false;
            for (int pass = 0; pass < 64 && !hasArrived; ++pass) {
                link.pump(1);
                CHECK(showsEveryChart(*link.terminal()));
                hasArrived = true;
                for (const DashboardChart& chart : dashboardCharts) {
                    const auto shown = shownChartFrame(*link.terminal(), chart);
                    CHECK(shown == std::optional { frame } || shown == std::optional { frame - 1 });
                    hasArrived &= shown == std::optional { frame };
                }
            }
            CHECK(hasArrived);
        }
        CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
        CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    }
}

TEST(twoChartsReplacedFasterThanTheLinkCarriesThemBothKeepMoving)
{
    for (const Compression coding : { Compression::None, Compression::Deflate }) {
        ImageConnectionLink link(nullptr, coding);
        link.attach();
        link.write(dashboardFrame(0));
        link.settle();
        REQUIRE(link.terminal());
        REQUIRE(showsEveryChart(*link.terminal()));
        // The program redraws on every pass and the link carries one chunk a
        // pass, so no frame finishes before the next one replaces it.
        constexpr std::uint32_t roundsWanted = 3;
        std::array<std::uint32_t, dashboardCharts.size()> shownFrames {};
        std::array<std::uint32_t, dashboardCharts.size()> updates {};
        std::uint32_t frame = 0;
        while (frame < 600 && std::ranges::min(updates) < roundsWanted) {
            link.write(dashboardFrame(++frame));
            link.pump(1);
            CHECK(showsEveryChart(*link.terminal()));
            for (std::size_t index = 0; index < dashboardCharts.size(); ++index) {
                const auto shown = shownChartFrame(*link.terminal(), dashboardCharts[index]);
                REQUIRE(shown);
                CHECK(*shown >= shownFrames[index]);
                updates[index] += *shown != shownFrames[index] ? 1 : 0;
                shownFrames[index] = *shown;
            }
            // A round finishes both charts before the newest frame is sampled:
            // neither is ever a round ahead of the other.
            CHECK(std::ranges::max(updates) - std::ranges::min(updates) <= 1);
        }
        CHECK(std::ranges::min(updates) >= roundsWanted);
        link.settle();
        for (const DashboardChart& chart : dashboardCharts)
            CHECK(shownChartFrame(*link.terminal(), chart) == std::optional { frame });
        CHECK(showsEveryChart(*link.terminal()));
    }
}

TEST(aNewImageIDKeepsOnlyItsOldVerifiedPlacementUntilReplacementPixelsArrive)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.synchronize());
    auto changed = fourPixels;
    changed[0] = 42;
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", changed) + kitty("a=d,d=I,i=7,q=2"));
    REQUIRE(link.sendPlacements());
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    CHECK_EQ(link.client->images.placements[0].imageID, 7u);
    CHECK(findImage(*link.client, 7));
    CHECK(!findImage(*link.client, 8));
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    REQUIRE(link.serveMissing());
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    CHECK_EQ(link.client->images.placements[0].imageID, 8u);
    CHECK(!findImage(*link.client, 7));
    REQUIRE(findImage(*link.client, 8));
    CHECK_EQ(findImage(*link.client, 8)->pixels[0], 42u);
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
}

TEST(deletingTheLatestVideoFrameCancelsACoalescedTransferImmediately)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 91);
    link.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    for (int pass = 0; pass < 20 && !link.pixelBytes; ++pass)
        link.pump(1);
    REQUIRE(link.pixelBytes > 0 && link.pixelBytes < pixels.size());
    const auto records = link.placementRecords;
    pixels[0] = 42;
    link.write(kitty("a=T,f=32,s=256,v=256,i=8,q=2,C=1", pixels) + kitty("a=d,d=I,i=7,q=2"));
    link.pump(1);
    CHECK_EQ(link.placementRecords, records);
    REQUIRE(link.connectionBudget->heldBytes() >= pixels.size());
    link.write(kitty("a=d,d=I,i=8,q=2"));
    link.pump();
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(Image::placementCount(link.terminal()), 0);
    CHECK_EQ(link.host.connection->counters().bulkAborts, 1u);
    link.settle();
    CHECK(!findImage(*link.terminal(), 7));
    CHECK(!findImage(*link.terminal(), 8));
}

TEST(aRetainedVideoPictureStillObeysDeletionGeometryAndPixelBudget)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        ImageLink link;
        link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
        REQUIRE(link.synchronize());
        auto changed = fourPixels;
        changed[0] = 42;
        if (scenario == 0)
            link.client->images.kittyImageBytesBudget = fourPixels.size();
        if (scenario == 1)
            link.write("\033[2;2H");
        link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", changed) + kitty("a=d,d=I,i=7,q=2"));
        REQUIRE(link.sendPlacements());
        if (scenario != 2) {
            CHECK_EQ(Image::placementCount(link.client.get()), 0);
            CHECK(!findImage(*link.client, 7));
            REQUIRE(link.serveMissing());
            REQUIRE(findImage(*link.client, 8));
            CHECK_EQ(findImage(*link.client, 8)->pixels[0], 42u);
            CHECK(Image::totalBytes(link.client.get()) <= link.client->images.kittyImageBytesBudget);
        } else {
            REQUIRE(findImage(*link.client, 7));
            link.write(kitty("a=d,d=I,i=8,q=2"));
            REQUIRE(link.sendPlacements());
            CHECK_EQ(Image::placementCount(link.client.get()), 0);
            CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
            CHECK(!findImage(*link.client, 7));
        }
    }
}

TEST(anUnrelatedNewImageDoesNotDelayDeletingAnActiveTransfer)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 91);
    link.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    for (int pass = 0; pass < 20 && !link.pixelBytes; ++pass)
        link.pump(1);
    REQUIRE(link.pixelBytes > 0 && link.pixelBytes < pixels.size());
    REQUIRE(link.imageTransfers.size() == 1);
    const auto originalTransfer = link.imageTransfers[0];
    const auto records = link.placementRecords;
    pixels[0] = 42;
    link.write("\033[2;2H" + kitty("a=T,f=32,s=256,v=256,i=8,q=2,C=1", pixels) + kitty("a=d,d=I,i=7,q=2"));
    // An outstanding chunk may need its cumulative receipt before the new
    // metadata fits, but deletion does not wait for the old image's final.
    for (int pass = 0; pass < 4 && link.placementRecords == records; ++pass)
        link.pump(1);
    CHECK(link.placementRecords > records);
    CHECK_EQ(link.host.connection->counters().bulkAborts, 1u);
    CHECK(std::ranges::find(link.imageTransfers, originalTransfer) == link.imageTransfers.end());
    CHECK(!findImage(*link.terminal(), 7));
    link.settle();
    REQUIRE(findImage(*link.terminal(), 8));
    CHECK_EQ(link.terminal()->images.placements[0].cursorX, 1u);
    CHECK_EQ(link.terminal()->images.placements[0].cursorY, 1u);
}

TEST(interactiveNotificationsPreemptPixelsBeforeTheirQueuedStorageGrows)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 39);
    link.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    for (int pass = 0; pass < 20 && link.pixelBytes == 0; ++pass)
        link.pump(1);
    REQUIRE(link.pixelBytes > 0 && link.pixelBytes < pixels.size());
    REQUIRE(link.connectionBudget->heldBytes() >= pixels.size());
    const auto beforeQueue = link.host.connection->retainedOutboundPayloadBytes();
    std::size_t other = maxCatalogMessageBytes - link.connectionBudget->heldBytes() - beforeQueue;
    std::size_t preemptions = 0;
    link.host.connection->setImageSourceAdmission([&] {
        const auto retained = other + link.host.connection->retainedOutboundPayloadBytes();
        return BulkSourceAdmission { .connection = link.connectionBudget, .aggregate = link.aggregateBudget, .otherConnectionBytes = retained, .otherAggregateBytes = retained }; }, [&] {
        ++preemptions;
        CHECK_EQ(link.host.connection->retainedOutboundPayloadBytes(), beforeQueue);
        CHECK(link.connectionBudget->heldBytes() >= pixels.size());
        link.host.connection->preemptImageTransfers(); });
    // A full-sized notification needs no temporary encoded body before the
    // admitted scheduler copy. Several accepted frames may coexist afterward.
    // A kind no build knows is skipped by its reader, and only its size and
    // bytes matter here.
    const auto unknownNotify = [](std::size_t bytes, std::uint8_t fill) {
        std::vector<std::uint8_t> body(bytes, fill);
        body.front() = 0x7F;
        return body;
    };
    const auto notification = unknownNotify(maxFrameBodyBytes, 'n');
    const auto second = Ubiquitty::Protocol::Control::pasteReadyNotify({ "second", 1, 2 });
    const auto third = Ubiquitty::Protocol::Control::pasteReadyNotify({ "third", 1, 3 });
    REQUIRE(second && third);
    REQUIRE(link.host.connection->sendNotify(notification));
    CHECK_EQ(preemptions, 1u);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.aggregateBudget->heldBytes(), 0u);
    REQUIRE(link.host.connection->sendNotify(*second));
    REQUIRE(link.host.connection->sendNotify(*third));
    CHECK_EQ(preemptions, 1u);
    CHECK(link.host.connection->retainedOutboundPayloadBytes() + other <= maxCatalogMessageBytes);
    // Until the abort reaches the client its already allocated partial image
    // is still charged there, and is not yet an installed visible image.
    CHECK_EQ(link.client.connection()->inboundImageBytes(), pixels.size());
    CHECK(!findImage(*link.terminal(), 7));
    other = 0;
    link.settle();
    // The unknown kind arrived whole and was skipped by its length.
    CHECK_EQ(link.client.connection()->counters().notifiesSkipped, 1u);
    const auto notifies = link.client.connection()->takeNotifies();
    REQUIRE(notifies.size() == 2);
    CHECK(Ubiquitty::Protocol::Control::decodePasteReady(notifies[0].record)->requestID == "second");
    CHECK(Ubiquitty::Protocol::Control::decodePasteReady(notifies[1].record)->requestID == "third");
    REQUIRE(findImage(*link.terminal(), 7));
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
    CHECK_EQ(link.host.connection->counters().bulkAborts, 1u);
    REQUIRE(link.announcedImageTransfers.size() == 2);
    CHECK(link.announcedImageTransfers[0] != link.announcedImageTransfers[1]);
}

TEST(anImageOfferCanPreemptAnOlderSourceWithoutUsingItsRetiredDeliverySlot)
{
    ImageConnectionLink link;
    link.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 59);
    link.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", fourPixels));
    // One service queues exactly one asset head. Waiting for a pixel chunk
    // can take several paced services and offer both assets before pressure
    // is installed, so it does not establish the delivery-slot premise.
    link.host.connection->service(link.now += 16'667);
    REQUIRE(link.host.connection->counters().imageTransfersSent == 1u);
    REQUIRE(link.connectionBudget->heldBytes() >= pixels.size());
    const auto retained = link.host.connection->retainedOutboundPayloadBytes();
    std::size_t other = maxCatalogMessageBytes - link.connectionBudget->heldBytes() - retained;
    std::size_t preemptions = 0;
    link.host.connection->setImageSourceAdmission([&] {
        const auto bytes = other + link.host.connection->retainedOutboundPayloadBytes();
        return BulkSourceAdmission { .connection = link.connectionBudget, .aggregate = link.aggregateBudget, .otherConnectionBytes = bytes, .otherAggregateBytes = bytes }; }, [&] {
        ++preemptions;
        link.host.connection->preemptImageTransfers(); });
    // The next service owes the second asset's head. Its growth preempts the
    // first asset and resets every delivery slot, including the current one.
    link.host.connection->service(++link.now);
    CHECK_EQ(preemptions, 1u);
    CHECK(link.host.connection->state() == ConnectionState::Attached);
    CHECK_EQ(link.connectionBudget->heldBytes(), 0u);
    other = 0;
    link.settle();
    REQUIRE(findImage(*link.terminal(), 7));
    REQUIRE(findImage(*link.terminal(), 8));
    CHECK_EQ(link.client.connection()->inboundImageBytes(), 0u);
}

TEST(aDifferentConnectionsNotificationPreemptsImagesFromTheSharedAggregate)
{
    ImageConnectionLink images;
    ImageConnectionLink notifications;
    notifications.aggregateBudget = images.aggregateBudget;
    images.attach();
    notifications.attach();
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 81);
    images.write(kitty("a=T,f=32,s=256,v=256,i=7,q=2,C=1", pixels));
    for (int pass = 0; pass < 20 && images.pixelBytes == 0; ++pass)
        images.pump(1);
    REQUIRE(images.connectionBudget->heldBytes() >= pixels.size());
    const auto beforeQueue = notifications.host.connection->retainedOutboundPayloadBytes();
    const auto imageQueue = images.host.connection->retainedOutboundPayloadBytes();
    const auto other = 4 * maxCatalogMessageBytes - images.aggregateBudget->heldBytes() - imageQueue - beforeQueue;
    std::size_t preemptions = 0;
    notifications.host.connection->setImageSourceAdmission([&] {
        const auto own = notifications.host.connection->retainedOutboundPayloadBytes();
        return BulkSourceAdmission { .connection = notifications.connectionBudget, .aggregate = notifications.aggregateBudget,
            .otherConnectionBytes = own, .otherAggregateBytes = other + own + images.host.connection->retainedOutboundPayloadBytes() }; }, [&] {
        ++preemptions;
        CHECK_EQ(notifications.host.connection->retainedOutboundPayloadBytes(), beforeQueue);
        images.host.connection->preemptImageTransfers();
        notifications.host.connection->preemptImageTransfers(); });
    REQUIRE(notifications.host.connection->sendNotify(*Ubiquitty::Protocol::Control::pasteReadyNotify({ "neighbor", 1, 2 })));
    CHECK_EQ(preemptions, 1u);
    CHECK_EQ(images.connectionBudget->heldBytes(), 0u);
    CHECK_EQ(images.aggregateBudget->heldBytes(), 0u);
    CHECK_EQ(images.client.connection()->inboundImageBytes(), pixels.size());
    CHECK(notifications.host.connection->state() == ConnectionState::Attached);
    images.settle();
    REQUIRE(findImage(*images.terminal(), 7));
}

TEST(imageHashHasOneCanonicalByteSpelling)
{
    const auto hash = imageHash(fourPixels, 32, 2, 2);
    CHECK((hash == Digest128 { .low = 0xf41d324b7a34e7d5, .high = 0x3222dc756ac81739 }));
    std::vector<std::uint8_t> head;
    encode(head, ImageHead { .asset = { .hash = hash, .format = 32, .width = 2, .height = 2 } });
    constexpr std::array<std::uint8_t, 16> expected { 0xd5, 0xe7, 0x34, 0x7a, 0x4b, 0x32, 0x1d, 0xf4, 0x39, 0x17, 0xc8, 0x6a, 0x75, 0xdc, 0x22, 0x32 };
    CHECK(std::equal(expected.begin(), expected.end(), head.begin()));
    constexpr std::array<std::uint8_t, 3> rgb { 255, 0, 127 };
    CHECK((imageHash(rgb, 24, 1, 1) == Digest128 { .low = 0x0ae3626dfac59cf4, .high = 0xc32253239369ab5c }));
    constexpr std::array<std::uint8_t, 4> transparent {};
    CHECK((imageHash(transparent, 32, 1, 1) == Digest128 { .low = 0x23938e0eb9dd0d9b, .high = 0x83fa2c39a824b940 }));
    CHECK(imageHash(fourPixels, 32, 2, 2) != imageHash(fourPixels, 24, 2, 2));
    CHECK(imageHash(fourPixels, 32, 2, 2) != imageHash(fourPixels, 32, 1, 4));
}

TEST(sha256TwoRangesMatchKnownVectorsAtEverySplit)
{
    const std::array<std::pair<std::string, std::string_view>, 4> vectors { {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { std::string(128, 'a'), "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
    } };
    for (const auto& [input, expected] : vectors) {
        CHECK(Ubiquitty::Crypto::sha256Hex(input) == expected);
        const auto bytes = std::span { reinterpret_cast<const std::uint8_t*>(input.data()), input.size() };
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            std::array<std::uint8_t, 32> digest {};
            Ubiquitty::Crypto::sha256(bytes.first(split), bytes.subspan(split), digest);
            std::string hex;
            for (const auto byte : digest) {
                hex += "0123456789abcdef"[byte >> 4];
                hex += "0123456789abcdef"[byte & 15];
            }
            CHECK(hex == expected);
        }
    }
}

TEST(anImageTransferSourceReservesBeforeCopyAndOutlivesHostDeletion)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.publisher.capture());
    const auto asset = link.publisher.state().images[0].asset;
    BulkSourceAdmission admission { .connection = std::make_shared<BulkSourceBudget>(1u << 20),
        .aggregate = std::make_shared<BulkSourceBudget>(4u << 20),
        .otherConnectionBytes = 1u << 20 };
    CHECK(!link.publisher.copyForTransfer(asset.hash, admission));
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
    admission.otherConnectionBytes = 0;
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuseOwner;
        CHECK(!link.publisher.copyForTransfer(asset.hash, admission));
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusals + 1);
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
    auto source = link.publisher.copyForTransfer(asset.hash, admission);
    REQUIRE(source);
    const auto charge = source->retainedBytes();
    CHECK(charge >= fourPixels.size() + bulkSourceFrameReserveBytes);
    CHECK_EQ(admission.connection->heldBytes(), charge);
    CHECK_EQ(admission.aggregate->heldBytes(), charge);
    link.write(kitty("a=d,d=I,i=7,q=2"));
    CHECK(!link.publisher.copyForTransfer(asset.hash, admission));
    CHECK_EQ(admission.connection->heldBytes(), charge);
    CHECK(std::ranges::equal(source->bytes(), fourPixels));
    LaneScheduler scheduler(LanePolicy { .bulkSharePermille = 250 });
    // The source uses the existing scheduler grammar; ImageHead registration
    // and production reader activation are not part of this prerequisite.
    std::vector<std::uint8_t> head;
    encode(head, HistoryHead { .transfer = scheduler.nextTransferID(), .historyEpoch = 1, .firstLine = 0, .count = 1, .order = HistoryOrder::Ascending });
    REQUIRE(scheduler.enqueueSourceTransfer(FrameKind::HistoryHead, 1, head, std::move(source), TransferRank::Image) > 0);
    std::vector<std::uint8_t> frame;
    CHECK(scheduler.popFrame(frame, BulkPacing::Admitted) == Lane::Interactive);
    frame.clear();
    CHECK(scheduler.popFrame(frame, BulkPacing::Admitted) == Lane::Bulk);
    FrameDecoder decoder;
    decoder.append(frame);
    const auto decoded = decoder.next();
    REQUIRE(decoded && decoded->kind == FrameKind::BulkChunk);
    const auto chunk = decodeBulkChunk(decoded->body);
    REQUIRE(chunk && chunk->isLast);
    CHECK(std::ranges::equal(chunk->bytes, fourPixels));
    CHECK_EQ(scheduler.retainedSourceBytes(), 0u);
    CHECK_EQ(admission.connection->heldBytes(), 0u);
    CHECK_EQ(admission.aggregate->heldBytes(), 0u);
}

TEST(oneImageTravelsOnceForTwoPlacements)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    link.write("\033[4;8H");
    link.write(kitty("a=p,i=7,p=2,q=2,C=1"));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    CHECK_EQ(Image::placementCount(link.client.get()), 2);
    const auto* image = findImage(*link.client, 7);
    CHECK(image && image->pixelLength == fourPixels.size());
    if (image)
        CHECK(std::equal(fourPixels.begin(), fourPixels.end(), image->pixels));
}

TEST(placementChangesPreserveCropOffsetsDepthAndScreenWithoutPixelTransfers)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    link.write(kitty("a=d,d=i,i=7,q=2"));
    link.write("\033[?1049h\033[3;4H");
    link.write(kitty("a=p,i=7,p=12,q=2,C=1,c=3,r=4,x=1,y=1,w=1,h=1,X=2,Y=3,z=-7"));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
    const auto& placement = link.client->images.placements[0];
    CHECK_EQ(placement.placementID, 12u);
    CHECK_EQ(placement.cursorX, 3);
    CHECK_EQ(placement.cursorY, 2);
    CHECK_EQ(placement.sourceX, 1);
    CHECK_EQ(placement.sourceY, 1);
    CHECK_EQ(placement.sourceWidth, 1);
    CHECK_EQ(placement.sourceHeight, 1);
    CHECK_EQ(placement.pixelOffsetX, 2);
    CHECK_EQ(placement.pixelOffsetY, 3);
    CHECK_EQ(placement.cellColumns, 3);
    CHECK_EQ(placement.cellRows, 4);
    CHECK_EQ(placement.zIndex, -7);
    CHECK_EQ(placement.onAlternateScreen, 1);
    CHECK(!Image::placementIsOnActiveScreen(placement, false));
    CHECK(Image::placementIsOnActiveScreen(placement, true));
    std::array<Image::ImageQuad, 2> quads {};
    CHECK_EQ(Image::resolveImagePlacements(link.client->images.stored, link.client->images.placements,
                 { .columns = 40, .rows = 12 }, { 8, 16 }, { 0, 0 }, 0, quads, {}, false),
        0);
    CHECK_EQ(Image::resolveImagePlacements(link.client->images.stored, link.client->images.placements,
                 { .columns = 40, .rows = 12 }, { 8, 16 }, { 0, 0 }, 0, quads, {}, true),
        1);
    CHECK_EQ(quads[0].minXInPixels, 26.0f);
    CHECK_EQ(quads[0].minYInPixels, 35.0f);
    CHECK_EQ(quads[0].u0, 0.5f);
    CHECK_EQ(quads[0].v0, 0.5f);
    CHECK(quads[0].straightAlpha);
}

TEST(missingReplacementPixelsNeverDisplayTheOldContentUnderANewPlacement)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    auto changed = fourPixels;
    changed[0] = 42;
    link.write("\033[2;2H" + kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", changed));
    CHECK(link.sendPlacements());
    CHECK_EQ(Image::placementCount(link.client.get()), 0);
    CHECK(!findImage(*link.client, 7));
    CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
    CHECK_EQ(link.replica.missing().size(), 1u);
    CHECK(link.serveMissing());
    CHECK_EQ(link.transfers, 2u);
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    CHECK_EQ(link.client->images.placements[0].cursorX, 1u);
    const auto* image = findImage(*link.client, 7);
    CHECK(image && image->pixels[0] == 42);
}

TEST(aSameIDReplacementKeepsItsVerifiedPictureWhereItStoodUntilItsPixelsArrive)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    const auto before = link.client->images.placements[0];
    const auto generation = findImage(*link.client, 7)->contentGeneration;
    auto changed = fourPixels;
    changed[0] = 42;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", changed));
    REQUIRE(link.sendPlacements());
    CHECK_EQ(link.replica.missing().size(), 1u);
    // The old picture stays at the old cells, under the same ID, and is the
    // same picture a renderer already holds: its content stamp is unchanged.
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    CHECK(hasSamePlacementGeometry(link.client->images.placements[0], before));
    CHECK_EQ(link.client->images.placements[0].imageID, 7u);
    const auto* retained = findImage(*link.client, 7);
    REQUIRE(retained);
    CHECK(std::equal(fourPixels.begin(), fourPixels.end(), retained->pixels));
    CHECK_EQ(retained->contentGeneration, generation);
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    REQUIRE(link.serveMissing());
    CHECK_EQ(link.transfers, 2u);
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    CHECK(hasSamePlacementGeometry(link.client->images.placements[0], before));
    const auto* replaced = findImage(*link.client, 7);
    REQUIRE(replaced);
    CHECK_EQ(replaced->pixels[0], 42u);
    CHECK(replaced->contentGeneration != generation);
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    CHECK(link.replica.missing().empty());
}

TEST(aRetainedSameIDPictureObeysDeletionGeometryDimensionsAndPixelBudget)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        ImageLink link;
        link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", fourPixels));
        REQUIRE(link.synchronize());
        auto changed = fourPixels;
        changed[0] = 42;
        if (scenario == 0)
            link.client->images.kittyImageBytesBudget = fourPixels.size();
        if (scenario == 1)
            link.write("\033[2;2H");
        if (scenario == 2) {
            // Other dimensions are another picture, even at the same cells.
            const std::array<std::uint8_t, 8> wide { 1, 2, 3, 255, 4, 5, 6, 255 };
            link.write(kitty("a=T,f=32,s=2,v=1,i=7,p=1,q=2,C=1", wide));
        } else {
            link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", changed));
        }
        REQUIRE(link.sendPlacements());
        if (scenario != 3) {
            CHECK_EQ(Image::placementCount(link.client.get()), 0);
            CHECK(!findImage(*link.client, 7));
            CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
            REQUIRE(link.serveMissing());
            REQUIRE(findImage(*link.client, 7));
            CHECK_EQ(Image::placementCount(link.client.get()), 1);
            CHECK(Image::totalBytes(link.client.get()) <= link.client->images.kittyImageBytesBudget);
        } else {
            REQUIRE(findImage(*link.client, 7));
            CHECK_EQ(Image::placementCount(link.client.get()), 1);
            link.write(kitty("a=d,d=I,i=7,q=2"));
            REQUIRE(link.sendPlacements());
            CHECK_EQ(Image::placementCount(link.client.get()), 0);
            CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
            CHECK(!findImage(*link.client, 7));
            CHECK(link.replica.missing().empty());
        }
    }
}

TEST(aRetainedPictureGivesWayWhenItsReplacementNeedsTheRoom)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", fourPixels));
    REQUIRE(link.synchronize());
    auto changed = fourPixels;
    changed[0] = 42;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,p=1,q=2,C=1", changed));
    REQUIRE(link.sendPlacements());
    REQUIRE(Image::placementCount(link.client.get()) == 1);
    REQUIRE(Image::totalBytes(link.client.get()) == fourPixels.size());
    // The budget shrinks while the replacement is in flight: the old picture
    // is retired rather than keeping the new one out.
    link.client->images.kittyImageBytesBudget = fourPixels.size();
    REQUIRE(link.serveMissing());
    const auto* image = findImage(*link.client, 7);
    REQUIRE(image);
    CHECK_EQ(image->pixels[0], 42u);
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
}

TEST(imageDeletionRetiresPlacementsAndLatePixelsCannotResurrectThem)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.sendPlacements());
    const auto owner = link.publisher.copyPixels(link.publisher.state().images[0].asset.hash, fourPixels.size());
    CHECK(owner.has_value());
    if (!owner)
        return;
    link.write(kitty("a=d,d=I,i=7,q=2"));
    CHECK(link.synchronize());
    CHECK(link.replica.accept(owner->asset, owner->pixels()) == ImagePixelsResult::Unwanted);
    CHECK_EQ(Image::placementCount(link.client.get()), 0);
    CHECK(!findImage(*link.client, 7));
    CHECK(!link.publisher.copyPixels(owner->asset.hash, fourPixels.size()));
}

TEST(aRetainedReplicaNeedsNoPixelsAndOneEvictedAssetNeedsOnlyItself)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    auto other = fourPixels;
    other[0] = 17;
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", other));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 2u);
    ImageReplica resumed(*link.client);
    CHECK(resumed.replace(link.publisher.state()));
    CHECK(resumed.missing().empty());
    Image::dispose(&link.client->images.stored[0]);
    ImageReplica afterEviction(*link.client);
    CHECK(afterEviction.replace(link.publisher.state()));
    const auto missing = afterEviction.missing();
    CHECK_EQ(missing.size(), 1u);
    if (!missing.empty())
        CHECK(missing[0] == link.publisher.state().images[0].asset);
}

TEST(anAnimatedHostPublishesTheSelectedFrameAndKeepsAnEarlierSnapshotStable)
{
    ImageLink link;
    constexpr std::array<std::uint8_t, 4> red { 255, 0, 0, 255 };
    constexpr std::array<std::uint8_t, 4> green { 0, 255, 0, 255 };
    link.write(kitty("a=T,f=32,s=1,v=1,i=7,q=2,C=1", red));
    REQUIRE(link.synchronize());
    const auto initial = link.publisher.state();
    const auto snapshot = link.publisher.copyPixels(initial.images[0].asset.hash, red.size());
    REQUIRE(snapshot);
    link.write(kitty("a=f,f=32,s=1,v=1,i=7,z=40,q=2", green));
    link.write(kitty("a=a,i=7,c=3,q=2"));
    REQUIRE(link.synchronize());
    CHECK_EQ(link.publisher.state().revision, initial.revision);
    CHECK_EQ(link.transfers, 1u);
    Image::advanceFrames(link.host.get(), 40);
    const auto* animated = findImage(*link.host, 7);
    REQUIRE(animated && animated->frameCount == 2 && animated->currentFrame == 1);
    CHECK(std::equal(red.begin(), red.end(), animated->pixels));
    REQUIRE(link.synchronize());
    CHECK(link.publisher.state().revision > initial.revision);
    CHECK(link.publisher.state().images[0].asset.hash == imageHash(green, 32, 1, 1));
    CHECK_EQ(link.transfers, 2u);
    const auto* received = findImage(*link.client, 7);
    REQUIRE(received);
    CHECK(std::equal(green.begin(), green.end(), received->pixels));
    CHECK(std::ranges::equal(snapshot->pixels(), red));
    const auto greenHash = link.publisher.state().images[0].asset.hash;
    Image::advanceFrames(link.host.get(), 40);
    CHECK(!link.publisher.copyPixels(greenHash, green.size()));
    REQUIRE(link.synchronize());
    CHECK(link.publisher.state().images[0].asset.hash == initial.images[0].asset.hash);
}

TEST(aRetainedAnimatedCacheReusesOnlyItsSelectedFrameAndStopsItsLocalTimeline)
{
    ImageLink link;
    constexpr std::array<std::uint8_t, 4> red { 255, 0, 0, 255 };
    constexpr std::array<std::uint8_t, 4> green { 0, 255, 0, 255 };
    link.write(kitty("a=T,f=32,s=1,v=1,i=7,q=2,C=1", green));
    Ubiquitty::processBytes(link.client.get(), kitty("a=T,f=32,s=1,v=1,i=99,q=2,C=1", red));
    Ubiquitty::processBytes(link.client.get(), kitty("a=f,f=32,s=1,v=1,i=99,z=40,q=2", green));
    Ubiquitty::processBytes(link.client.get(), kitty("a=a,i=99,c=3,q=2"));
    Image::advanceFrames(link.client.get(), 40);
    const auto* animated = findImage(*link.client, 99);
    REQUIRE(animated && animated->currentFrame == 1);
    const auto* selected = animated->frames[1].pixels;
    REQUIRE(link.synchronize());
    CHECK_EQ(link.transfers, 0u);
    CHECK(link.replica.missing().empty());
    CHECK(!findImage(*link.client, 99));
    const auto* received = findImage(*link.client, 7);
    REQUIRE(received);
    CHECK(received->pixels == selected);
    CHECK_EQ(received->frameCount, 1);
    CHECK_EQ(received->currentFrame, 0);
    CHECK(received->animationState == Image::AnimationState::Stopped);
    CHECK_EQ(Image::totalBytes(link.client.get()), green.size());
    Image::advanceFrames(link.client.get(), 400);
    CHECK(std::equal(green.begin(), green.end(), received->pixels));
}

TEST(anAnimatedCacheDoesNotMistakeItsUnselectedBaseForTheAdvertisedAsset)
{
    ImageLink link;
    constexpr std::array<std::uint8_t, 4> red { 255, 0, 0, 255 };
    constexpr std::array<std::uint8_t, 4> green { 0, 255, 0, 255 };
    link.write(kitty("a=T,f=32,s=1,v=1,i=7,q=2,C=1", red));
    Ubiquitty::processBytes(link.client.get(), kitty("a=T,f=32,s=1,v=1,i=7,q=2,C=1", red));
    Ubiquitty::processBytes(link.client.get(), kitty("a=f,f=32,s=1,v=1,i=7,z=40,q=2", green));
    Ubiquitty::processBytes(link.client.get(), kitty("a=a,i=7,c=3,q=2"));
    Image::advanceFrames(link.client.get(), 40);
    const auto* animated = findImage(*link.client, 7);
    REQUIRE(animated && animated->currentFrame == 1);
    REQUIRE(link.sendPlacements());
    CHECK_EQ(link.replica.missing().size(), 1u);
    CHECK_EQ(Image::placementCount(link.client.get()), 0);
    REQUIRE(link.serveMissing());
    const auto* received = findImage(*link.client, 7);
    REQUIRE(received);
    CHECK(std::equal(red.begin(), red.end(), received->pixels));
    CHECK_EQ(received->frameCount, 1);
    CHECK(received->animationState == Image::AnimationState::Stopped);
}

TEST(identicalPixelsWithDifferentIDsUseOneTransferAndRetainVirtualMetadata)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,p=22,q=2,U=1,c=4,r=5", fourPixels));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    const auto* image = findImage(*link.client, 8);
    CHECK(image && image->virtualPlacement);
    if (image) {
        CHECK_EQ(image->virtualColumns, 4);
        CHECK_EQ(image->virtualRows, 5);
        CHECK_EQ(image->virtualPlacementID, 22u);
    }
    link.write(kitty("a=T,f=32,s=2,v=2,i=9,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    CHECK(findImage(*link.client, 9));
}

TEST(aPixelSnapshotOutlivesHostReplacementAndRequiresItsReservation)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.publisher.capture());
    const auto asset = link.publisher.state().images[0].asset;
    CHECK(!link.publisher.copyPixels(asset.hash, fourPixels.size() - 1));
    const auto snapshot = link.publisher.copyPixels(asset.hash, fourPixels.size());
    CHECK(snapshot.has_value());
    if (!snapshot)
        return;
    auto replacement = fourPixels;
    replacement[0] = 9;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", replacement));
    CHECK(std::equal(fourPixels.begin(), fourPixels.end(), snapshot->pixels().begin()));
    CHECK(!link.publisher.copyPixels(asset.hash, fourPixels.size()));
    CHECK(link.publisher.capture());
    CHECK(link.publisher.state().images[0].asset.hash != asset.hash);
    CHECK(!link.publisher.copyPixels(asset.hash, fourPixels.size()));
}

TEST(retransmittingIdenticalPixelsRefreshesTheSnapshotStampWithoutChangingTheRevision)
{
    ImageLink link;
    link.write(kitty("a=t,f=32,s=2,v=2,i=7,q=2", fourPixels));
    CHECK(link.publisher.capture());
    const auto revision = link.publisher.state().revision;
    const auto asset = link.publisher.state().images[0].asset;
    link.write(kitty("a=t,f=32,s=2,v=2,i=7,q=2", fourPixels));
    CHECK(link.publisher.capture());
    CHECK_EQ(link.publisher.state().revision, revision);
    CHECK(link.publisher.copyPixels(asset.hash, fourPixels.size()).has_value());
}

TEST(imageRecordsRefuseTruncationTrailingBytesCountsAndContradictoryHashes)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.synchronize());
    std::vector<std::uint8_t> record;
    encode(record, link.publisher.state());
    for (std::size_t cut = 0; cut < record.size(); ++cut)
        CHECK(!decodeImagePlacements(std::span(record).first(cut)));
    record.push_back(0);
    CHECK(!decodeImagePlacements(record));
    auto invalid = link.publisher.state();
    invalid.placements[0].hash.low ^= 1;
    CHECK(!link.replica.replace(invalid));
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
    invalid = link.publisher.state();
    invalid.imageCount = invalid.images.size() + 1;
    CHECK(!link.replica.replace(invalid));
    record.clear();
    WireWriter writer(record);
    writer.u32(1);
    writer.varint(Image::maxTerminalImageCount + 1);
    CHECK(!decodeImagePlacements(record));
}

TEST(imageHeadsAndPixelsAreBoundedBeforeInstallation)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    CHECK(link.sendPlacements());
    const auto asset = link.publisher.state().images[0].asset;
    auto corrupted = fourPixels;
    corrupted[0] ^= 1;
    CHECK(link.replica.accept(asset, corrupted) == ImagePixelsResult::Malformed);
    CHECK(link.replica.accept(asset, std::span(fourPixels).first(15)) == ImagePixelsResult::Malformed);
    link.client->images.kittyImageBytesBudget = fourPixels.size() - 1;
    CHECK(link.replica.accept(asset, fourPixels) == ImagePixelsResult::NoSpace);
    CHECK(!findImage(*link.client, 7));
    CHECK_EQ(Image::placementCount(link.client.get()), 0);
    link.client->images.kittyImageBytesBudget = fourPixels.size();
    CHECK(link.replica.accept(asset, fourPixels) == ImagePixelsResult::Applied);
    auto maximum = asset;
    maximum.width = 4096;
    maximum.height = 4096;
    CHECK(imagePixelBytes(maximum) == Image::maxDecodedImageBytes);
    maximum.width += 1;
    CHECK(!imagePixelBytes(maximum));
    std::vector<std::uint8_t> record;
    encode(record, ImageHead { .asset = maximum, .transfer = 1 });
    CHECK(!decodeImageHead(record));
    maximum.width = 2;
    maximum.height = 2;
    maximum.format = 31;
    CHECK(!imagePixelBytes(maximum));
}

TEST(aReceivedPixelOwnerBecomesTheRenderedImageWithoutASecondPixelAllocation)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.sendPlacements());
    const auto asset = link.publisher.state().images[0].asset;
    auto pixels = link.publisher.copyPixels(asset.hash, fourPixels.size());
    REQUIRE(pixels);
    const auto* received = pixels->bytes.get();
    link.client->images.kittyImageBytesBudget = fourPixels.size();
    CHECK(link.replica.accept(std::move(*pixels)) == ImagePixelsResult::Applied);
    CHECK(!pixels->bytes);
    const auto* image = findImage(*link.client, 7);
    REQUIRE(image);
    CHECK(image->pixels == received);
    CHECK(std::ranges::equal(std::span(image->pixels, image->pixelLength), fourPixels));
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
}

TEST(anImageTransferRetainsItsExactAllocationUntilTheReplicaTakesItsOwner)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.sendPlacements());
    const auto asset = link.publisher.state().images[0].asset;
    const ImageHead head { .asset = asset, .transfer = 19 };
    CHECK(!ImageTransfer::begin(head, fourPixels.size() - 1));
    CHECK(!ImageTransfer::begin({ .asset = asset, .transfer = 0 }, fourPixels.size()));
    auto transfer = ImageTransfer::begin(head, fourPixels.size());
    REQUIRE(transfer);
    CHECK_EQ(transfer->heldBytes(), fourPixels.size());
    CHECK(!transfer->take());
    BulkChunk chunk;
    chunk.transfer = head.transfer;
    chunk.bytes = std::span(fourPixels).first(7);
    CHECK(transfer->accept(chunk) == ImageChunkResult::Pending);
    CHECK_EQ(transfer->heldBytes(), fourPixels.size());
    CHECK(!transfer->take());
    auto moved = std::move(*transfer);
    CHECK_EQ(transfer->heldBytes(), 0u);
    CHECK_EQ(moved.heldBytes(), fourPixels.size());
    chunk.index = 1;
    chunk.bytes = std::span(fourPixels).subspan(7);
    chunk.isLast = true;
    Ubiquitty::Crypto::sha256(fourPixels, chunk.sha256);
    CHECK(moved.accept(chunk) == ImageChunkResult::Complete);
    CHECK_EQ(moved.heldBytes(), fourPixels.size());
    auto pixels = moved.take();
    REQUIRE(pixels);
    const auto* received = pixels->bytes.get();
    CHECK_EQ(moved.heldBytes(), 0u);
    CHECK(!moved.take());
    CHECK(std::ranges::equal(pixels->pixels(), fourPixels));
    CHECK(link.replica.accept(std::move(*pixels)) == ImagePixelsResult::Applied);
    const auto* installed = findImage(*link.client, 7);
    REQUIRE(installed);
    CHECK(installed->pixels == received);
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    CHECK(moved.accept(chunk) == ImageChunkResult::Refused);
    CHECK(findImage(*link.client, 7)->pixels == received);
}

TEST(aMalformedImageChunkReleasesItsOwnerWithoutChangingThePicture)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.synchronize());
    const auto asset = link.publisher.state().images[0].asset;
    const auto* installed = findImage(*link.client, 7);
    REQUIRE(installed);
    const auto* original = installed->pixels;
    for (int failure = 0; failure != 9; ++failure) {
        auto transfer = ImageTransfer::begin({ .asset = asset, .transfer = 19 }, fourPixels.size());
        REQUIRE(transfer);
        BulkChunk chunk;
        chunk.transfer = 19;
        chunk.bytes = std::span(fourPixels).first(7);
        REQUIRE(transfer->accept(chunk) == ImageChunkResult::Pending);
        chunk.index = 1;
        chunk.bytes = std::span(fourPixels).subspan(7);
        chunk.isLast = true;
        Ubiquitty::Crypto::sha256(fourPixels, chunk.sha256);
        switch (failure) {
        case 0:
            ++chunk.transfer;
            break;
        case 1:
            ++chunk.index;
            break;
        case 2:
            chunk.bytes = fourPixels;
            break;
        case 3:
            chunk.bytes = {};
            break;
        case 4:
            chunk.bytes = chunk.bytes.first(8);
            break;
        case 5:
            chunk.sha256[0] ^= 1;
            break;
        case 6:
            chunk.compression = Compression::Deflate;
            chunk.inflatedBytes = fourPixels.size();
            break;
        case 7:
            chunk.isLast = false;
            break;
        case 8:
            chunk.inflatedBytes = fourPixels.size();
            break;
        }
        CHECK(transfer->accept(chunk) == ImageChunkResult::Refused);
        CHECK_EQ(transfer->heldBytes(), 0u);
        CHECK(!transfer->take());
        CHECK(findImage(*link.client, 7)->pixels == original);
        CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    }
}

TEST(aReceivedOwnerAndItsDuplicateBindingsAreAdmittedAsOneAtomicPicture)
{
    for (const std::size_t budget : { 2 * fourPixels.size() - 1, 2 * fourPixels.size() }) {
        ImageLink link;
        link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
        link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", fourPixels));
        REQUIRE(link.sendPlacements());
        REQUIRE(link.replica.missing().size() == 1);
        const auto asset = link.publisher.state().images[0].asset;
        auto pixels = link.publisher.copyPixels(asset.hash, fourPixels.size());
        REQUIRE(pixels);
        const auto* received = pixels->bytes.get();
        link.client->images.kittyImageBytesBudget = budget;
        const auto result = link.replica.accept(std::move(*pixels));
        CHECK(!pixels->bytes);
        if (budget < 2 * fourPixels.size()) {
            CHECK(result == ImagePixelsResult::NoSpace);
            CHECK(!findImage(*link.client, 7));
            CHECK(!findImage(*link.client, 8));
            CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
            CHECK_EQ(Image::placementCount(link.client.get()), 0);
            continue;
        }
        CHECK(result == ImagePixelsResult::Applied);
        const auto* first = findImage(*link.client, 7);
        const auto* second = findImage(*link.client, 8);
        REQUIRE(first && second);
        CHECK(first->pixels == received);
        CHECK(second->pixels != received);
        CHECK(std::ranges::equal(std::span(second->pixels, second->pixelLength), fourPixels));
        CHECK_EQ(Image::totalBytes(link.client.get()), 2 * fourPixels.size());
        CHECK_EQ(Image::placementCount(link.client.get()), 2);
    }
}

TEST(aCorruptOrUnwantedReceivedOwnerCannotReplaceTheCurrentPicture)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    REQUIRE(link.synchronize());
    const auto* installed = findImage(*link.client, 7);
    REQUIRE(installed);
    const auto* original = installed->pixels;
    const auto asset = link.publisher.state().images[0].asset;
    for (const bool shouldCorrupt : { false, true }) {
        auto pixels = link.publisher.copyPixels(asset.hash, fourPixels.size());
        REQUIRE(pixels);
        if (shouldCorrupt)
            pixels->bytes.get()[0] ^= 1;
        CHECK(link.replica.accept(std::move(*pixels)) == (shouldCorrupt ? ImagePixelsResult::Malformed : ImagePixelsResult::Unwanted));
        CHECK(!pixels->bytes);
        CHECK(findImage(*link.client, 7)->pixels == original);
        CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
        CHECK_EQ(Image::placementCount(link.client.get()), 1);
    }
    CHECK(link.replica.accept(ImagePixels { .asset = asset, .bytes = {}, .size = fourPixels.size() }) == ImagePixelsResult::Malformed);
}

TEST(aReceivedOwnerCannotConsumeAnotherTransfersDestinationReservation)
{
    for (const std::size_t reserved : { fourPixels.size(), fourPixels.size() + 1 }) {
        ImageLink link;
        link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
        REQUIRE(link.sendPlacements());
        const auto asset = link.publisher.state().images[0].asset;
        CHECK(link.replica.bytesToInstall(asset) == fourPixels.size());
        auto pixels = link.publisher.copyPixels(asset.hash, fourPixels.size());
        REQUIRE(pixels);
        link.client->images.kittyImageBytesBudget = 2 * fourPixels.size();
        const auto result = link.replica.accept(std::move(*pixels), reserved);
        if (reserved == fourPixels.size()) {
            CHECK(result == ImagePixelsResult::Applied);
            CHECK(link.replica.bytesToInstall(asset) == 0u);
            CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
        } else {
            CHECK(result == ImagePixelsResult::NoSpace);
            CHECK(link.replica.bytesToInstall(asset) == fourPixels.size());
            CHECK_EQ(Image::totalBytes(link.client.get()), 0u);
        }
        auto foreign = asset;
        foreign.hash.low ^= 1;
        CHECK(!link.replica.bytesToInstall(foreign));
    }
}

TEST(aCachedAliasCannotAllocateIntoAPendingImageReservation)
{
    ImageLink link;
    link.write(kitty("a=T,f=32,s=2,v=2,i=7,q=2,C=1", fourPixels));
    auto other = fourPixels;
    other[0] = 17;
    link.write(kitty("a=T,f=32,s=2,v=2,i=9,q=2,C=1", other));
    REQUIRE(link.sendPlacements());
    const auto first = link.publisher.state().images[0].asset;
    const auto second = link.publisher.state().images[1].asset;
    auto firstPixels = link.publisher.copyPixels(first.hash, fourPixels.size());
    REQUIRE(firstPixels);
    link.client->images.kittyImageBytesBudget = 2 * fourPixels.size();
    REQUIRE(link.replica.accept(std::move(*firstPixels)) == ImagePixelsResult::Applied);
    auto incoming = link.publisher.copyPixels(second.hash, other.size());
    REQUIRE(incoming);
    const auto* pendingOwner = incoming->bytes.get();
    link.write(kitty("a=T,f=32,s=2,v=2,i=8,q=2,C=1", fourPixels));
    REQUIRE(link.publisher.capture());
    REQUIRE(link.replica.replace(link.publisher.state(), other.size()));
    CHECK(findImage(*link.client, 7));
    CHECK(!findImage(*link.client, 8));
    CHECK(!findImage(*link.client, 9));
    CHECK_EQ(Image::totalBytes(link.client.get()), fourPixels.size());
    CHECK(link.replica.bytesToInstall(first) == fourPixels.size());
    CHECK(link.replica.bytesToInstall(second) == other.size());
    CHECK(link.replica.accept(std::move(*incoming)) == ImagePixelsResult::Applied);
    REQUIRE(findImage(*link.client, 9));
    CHECK(findImage(*link.client, 9)->pixels == pendingOwner);
    CHECK(!findImage(*link.client, 8));
    CHECK_EQ(Image::totalBytes(link.client.get()), 2 * fourPixels.size());
}

TEST(hostFileAndSharedMemoryNamesNeverBecomeClientReadsOrImageRecords)
{
    for (const char medium : { 'f', 't', 's' }) {
        MemoryFiles hostFiles;
        MemoryFiles clientFiles;
        ImageLink link;
        link.host->fileReadClient = &hostFiles;
        link.client->fileReadClient = &clientFiles;
        // The same name also exercises t=s, whose namespace is capped at
        // 31 bytes on every host, matching the strictest supported platform,
        // and carries the marker without which a t=t is refused unread.
        const std::string path = "/tty-graphics-protocol-fixture";
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(path.data()), path.size());
        link.write(kitty(std::string("a=T,f=32,s=2,v=2,i=7,q=2,C=1,t=") + medium, bytes));
        Ubiquitty::drainAsyncDecodes(*link.host);
        CHECK(hostFiles.calls > 0);
        CHECK(link.synchronize());
        CHECK_EQ(link.transfers, 1u);
        CHECK_EQ(clientFiles.calls, 0);
        const std::string wire(link.wire.begin(), link.wire.end());
        CHECK(wire.find(path) == std::string::npos);
        CHECK(wire.find(base64(bytes)) == std::string::npos);
        CHECK(findImage(*link.client, 7));
        ImageLink otherPane;
        CHECK(otherPane.publisher.capture());
        CHECK(!otherPane.publisher.copyPixels(link.publisher.state().images[0].asset.hash, Image::maxDecodedImageBytes));
    }
}

TEST(aSixelUsesTheSameDecodedAssetAndPlacementModel)
{
    ImageLink link;
    link.write("\033Pq#1;2;100;0;0#1~\033\\");
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
    CHECK_EQ(link.publisher.state().imageCount, 1u);
    const auto& binding = link.publisher.state().images[0];
    const auto* image = findImage(*link.client, binding.imageID);
    CHECK(image && image->format == 32 && image->pixelLength > 0);
}

TEST(anITermInlineImageReachesTheSameReplicaAfterHostSideDecoding)
{
    Ubiquitty::PAL::Context context;
    context.monotonicClock = +[] { return Ubiquitty::Ticks(0); };
    context.imageDecoder = +[](std::span<const std::uint8_t> input, std::uint8_t** pixels, int* width, int* height) {
        if (!std::ranges::equal(input, fourPixels))
            return false;
        *pixels = static_cast<std::uint8_t*>(std::malloc(fourPixels.size()));
        if (!*pixels)
            return false;
        std::memcpy(*pixels, fourPixels.data(), fourPixels.size());
        *width = 2;
        *height = 2;
        return true;
    };
    Ubiquitty::PAL::ScopedContext scope(context);
    ImageLink link;
    link.write("\033]1337;File=width=2;height=2;inline=1:" + base64(fourPixels) + "\007");
    Ubiquitty::drainAsyncDecodes(*link.host);
    CHECK(link.synchronize());
    CHECK_EQ(link.transfers, 1u);
    CHECK_EQ(Image::placementCount(link.client.get()), 1);
    CHECK_EQ(link.publisher.state().imageCount, 1u);
    const auto* image = findImage(*link.client, link.publisher.state().images[0].imageID);
    CHECK(image && image->pixelLength == fourPixels.size());
    if (image)
        CHECK(std::equal(fourPixels.begin(), fourPixels.end(), image->pixels));
}

UBIQUITTY_TEST_MAIN()
