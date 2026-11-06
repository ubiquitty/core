// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Images.h"

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Image/TerminalImageInternal.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Operations.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

void writeHash(WireWriter& writer, Digest128 hash)
{
    writer.u64(hash.low);
    writer.u64(hash.high);
}

Digest128 readHash(WireReader& reader)
{
    const std::uint64_t low = reader.u64();
    return { .low = low, .high = reader.u64() };
}

void writeAsset(WireWriter& writer, const ImageAssetIdentity& asset)
{
    writeHash(writer, asset.hash);
    writer.u8(asset.format);
    writer.u32(asset.width);
    writer.u32(asset.height);
}

ImageAssetIdentity readAsset(WireReader& reader)
{
    ImageAssetIdentity asset;
    asset.hash = readHash(reader);
    asset.format = reader.u8();
    asset.width = reader.u32();
    asset.height = reader.u32();
    return asset;
}

std::span<const std::uint8_t> selectedPixels(const Image::TerminalImage& image)
{
    if (image.currentFrame < image.frameCount && image.currentFrame < image.frames.size()) {
        const auto& frame = image.frames[image.currentFrame];
        if (frame.used && frame.pixels)
            return { frame.pixels, frame.pixelLength };
    }
    return { image.pixels, image.pixelLength };
}

ImageAssetIdentity identityOf(const Image::TerminalImage& image)
{
    ImageAssetIdentity asset { .hash = {}, .format = static_cast<std::uint8_t>(image.format), .width = image.width, .height = image.height };
    const auto length = imagePixelBytes(asset);
    const auto pixels = selectedPixels(image);
    if (length && pixels.data() && pixels.size() == *length)
        asset.hash = imageHash(pixels, asset.format, asset.width, asset.height);
    return asset;
}

void bindImage(Ubiquitty::Terminal& terminal, Image::TerminalImage& image, const ImageBinding& binding)
{
    if (image.imageID != binding.imageID)
        image.contentGeneration = ++terminal.images.kittyImageContentSeq;
    image.imageID = binding.imageID;
    image.imageNumber = binding.imageNumber;
    image.virtualPlacement = binding.isVirtual;
    image.virtualColumns = binding.virtualColumns;
    image.virtualRows = binding.virtualRows;
    image.virtualPlacementID = binding.virtualPlacementID;
    image.lastUsedSeq = ++terminal.images.kittyImageUseSeq;
    terminal.images.kittyNextImageID = std::max(terminal.images.kittyNextImageID, binding.imageID);
}

Image::OwnedPixels takeSelectedPixels(Image::TerminalImage& image)
{
    Image::OwnedPixels pixels;
    if (selectedPixels(image).data() == image.pixels) {
        pixels.reset(std::exchange(image.pixels, nullptr));
        image.frames[0] = {};
    } else {
        pixels.reset(std::exchange(image.frames[image.currentFrame].pixels, nullptr));
    }
    Image::dispose(&image);
    return pixels;
}

void installImage(Ubiquitty::Terminal& terminal, Image::TerminalImage& image, const ImageBinding& binding, Image::OwnedPixels pixels)
{
    Image::dispose(&image);
    image.used = true;
    image.width = static_cast<std::uint16_t>(binding.asset.width);
    image.height = static_cast<std::uint16_t>(binding.asset.height);
    image.format = binding.asset.format;
    image.pixels = pixels.release();
    image.pixelLength = static_cast<std::uint32_t>(*imagePixelBytes(binding.asset));
    image.contentGeneration = ++terminal.images.kittyImageContentSeq;
    Image::initFrameZero(&image);
    bindImage(terminal, image, binding);
}

/// Moves a previous owner's selected pixels into `destination` under
/// `binding`. An image that keeps its ID and its first frame keeps its content
/// generation, so a renderer keeps the texture it already holds for them.
void moveImage(Ubiquitty::Terminal& terminal, Image::TerminalImage& destination, const ImageBinding& binding, Image::TerminalImage& previous)
{
    const bool keepsTexture = previous.imageID == binding.imageID && previous.currentFrame == 0;
    const std::uint64_t generation = previous.contentGeneration;
    installImage(terminal, destination, binding, takeSelectedPixels(previous));
    if (keepsTexture)
        destination.contentGeneration = generation;
}

class ImageBulkSource final : public BulkSource {
public:
    ImageBulkSource(BulkSourceReservation reservation, ImagePixels pixels)
        : BulkSource(std::move(reservation))
        , m_pixels(std::move(pixels))
    {
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const override { return m_pixels.pixels(); }

private:
    ImagePixels m_pixels;
};

/// A zlib stream of one asset's decoded pixels, made when the transfer is.
class EncodedImageBulkSource final : public BulkSource {
public:
    EncodedImageBulkSource(BulkSourceReservation reservation, Image::OwnedPixels stream, std::size_t size)
        : BulkSource(std::move(reservation))
        , m_stream(std::move(stream))
        , m_size(size)
    {
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const override { return { m_stream.get(), m_size }; }

private:
    Image::OwnedPixels m_stream;
    std::size_t m_size;
};

} // namespace

bool hasSamePlacementGeometry(const Image::TerminalImagePlacement& left, const Image::TerminalImagePlacement& right)
{
    return left.cursorX == right.cursorX && left.cursorY == right.cursorY
        && left.cellColumns == right.cellColumns && left.cellRows == right.cellRows
        && left.pixelOffsetX == right.pixelOffsetX && left.pixelOffsetY == right.pixelOffsetY
        && left.sourceX == right.sourceX && left.sourceY == right.sourceY
        && left.sourceWidth == right.sourceWidth && left.sourceHeight == right.sourceHeight
        && left.drawnPixelWidth == right.drawnPixelWidth && left.drawnPixelHeight == right.drawnPixelHeight
        && left.zIndex == right.zIndex && left.cursorPolicy == right.cursorPolicy
        && left.onAlternateScreen == right.onAlternateScreen;
}

std::optional<std::size_t> imagePixelBytes(const ImageAssetIdentity& asset)
{
    if ((asset.format != 24 && asset.format != 32) || Image::isDecodedImageBytesRefused(asset.width, asset.height, asset.format / 8))
        return std::nullopt;
    return static_cast<std::size_t>(asset.width) * asset.height * (asset.format / 8);
}

bool isValid(const ImagePlacements& state)
{
    if (!state.revision || state.imageCount > state.images.size() || state.placementCount > state.placements.size())
        return false;
    for (std::size_t index = 0; index < state.imageCount; ++index) {
        const ImageBinding& binding = state.images[index];
        if (!binding.imageID || !imagePixelBytes(binding.asset))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            const ImageBinding& other = state.images[previous];
            if (other.imageID == binding.imageID || (other.asset.hash == binding.asset.hash && other.asset != binding.asset))
                return false;
        }
    }
    for (std::size_t index = 0; index < state.placementCount; ++index) {
        const ImagePlacement& placement = state.placements[index];
        const auto& geometry = placement.geometry;
        if (!geometry.used || geometry.onAlternateScreen > 1 || geometry.cursorPolicy > 1
            || (geometry.drawnPixelWidth == 0) != (geometry.drawnPixelHeight == 0))
            return false;
        const auto images = std::span(state.images).first(state.imageCount);
        const auto binding = std::ranges::find(images, geometry.imageID, &ImageBinding::imageID);
        if (binding == images.end() || binding->asset.hash != placement.hash)
            return false;
    }
    return true;
}

void encode(std::vector<std::uint8_t>& output, const ImageHead& head)
{
    WireWriter writer(output);
    writeAsset(writer, head.asset);
    writer.u32(head.transfer);
}

void encode(std::vector<std::uint8_t>& output, const ImageWant& want)
{
    WireWriter writer(output);
    writeHash(writer, want.hash);
}

void encode(std::vector<std::uint8_t>& output, const ImagePlacements& state)
{
    WireWriter writer(output);
    writer.u32(state.revision);
    writer.varint(state.imageCount);
    for (const ImageBinding& binding : std::span(state.images).first(state.imageCount)) {
        writer.u32(binding.imageID);
        writer.u32(binding.imageNumber);
        writeAsset(writer, binding.asset);
        writer.u8(binding.isVirtual ? 1 : 0);
        writer.u16(binding.virtualColumns);
        writer.u16(binding.virtualRows);
        writer.u32(binding.virtualPlacementID);
    }
    writer.varint(state.placementCount);
    for (const ImagePlacement& placement : std::span(state.placements).first(state.placementCount)) {
        writeHash(writer, placement.hash);
        const auto& geometry = placement.geometry;
        writer.u32(geometry.imageID);
        writer.u32(geometry.placementID);
        writer.u8(geometry.hasPlacementID ? 1 : 0);
        writer.u16(geometry.cursorX);
        writer.u16(std::bit_cast<std::uint16_t>(geometry.cursorY));
        writer.u16(geometry.cellColumns);
        writer.u16(geometry.cellRows);
        writer.u16(geometry.pixelOffsetX);
        writer.u16(geometry.pixelOffsetY);
        writer.u16(geometry.sourceX);
        writer.u16(geometry.sourceY);
        writer.u16(geometry.sourceWidth);
        writer.u16(geometry.sourceHeight);
        writer.u16(geometry.drawnPixelWidth);
        writer.u16(geometry.drawnPixelHeight);
        writer.u32(std::bit_cast<std::uint32_t>(geometry.zIndex));
        writer.u8(geometry.cursorPolicy);
        writer.u8(geometry.onAlternateScreen);
    }
}

std::optional<ImageHead> decodeImageHead(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    ImageHead head;
    head.asset = readAsset(reader);
    head.transfer = reader.u32();
    return reader.ok() && reader.remaining() == 0 && imagePixelBytes(head.asset) ? std::optional { head } : std::nullopt;
}

std::optional<ImageWant> decodeImageWant(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    ImageWant want { .hash = readHash(reader) };
    return reader.ok() && reader.remaining() == 0 ? std::optional { want } : std::nullopt;
}

std::optional<ImagePlacements> decodeImagePlacements(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    ImagePlacements state;
    state.revision = reader.u32();
    state.imageCount = reader.count(state.images.size(), imageBindingRecordBytes);
    for (ImageBinding& binding : std::span(state.images).first(state.imageCount)) {
        binding.imageID = reader.u32();
        binding.imageNumber = reader.u32();
        binding.asset = readAsset(reader);
        const std::uint8_t isVirtual = reader.u8();
        if (isVirtual > 1)
            return std::nullopt;
        binding.isVirtual = isVirtual != 0;
        binding.virtualColumns = reader.u16();
        binding.virtualRows = reader.u16();
        binding.virtualPlacementID = reader.u32();
    }
    state.placementCount = reader.count(state.placements.size(), imagePlacementRecordBytes);
    for (ImagePlacement& placement : std::span(state.placements).first(state.placementCount)) {
        placement.hash = readHash(reader);
        auto& geometry = placement.geometry;
        geometry.used = true;
        geometry.imageID = reader.u32();
        geometry.placementID = reader.u32();
        const std::uint8_t hasPlacementID = reader.u8();
        if (hasPlacementID > 1)
            return std::nullopt;
        geometry.hasPlacementID = hasPlacementID != 0;
        geometry.cursorX = reader.u16();
        geometry.cursorY = std::bit_cast<std::int16_t>(reader.u16());
        geometry.cellColumns = reader.u16();
        geometry.cellRows = reader.u16();
        geometry.pixelOffsetX = reader.u16();
        geometry.pixelOffsetY = reader.u16();
        geometry.sourceX = reader.u16();
        geometry.sourceY = reader.u16();
        geometry.sourceWidth = reader.u16();
        geometry.sourceHeight = reader.u16();
        geometry.drawnPixelWidth = reader.u16();
        geometry.drawnPixelHeight = reader.u16();
        geometry.zIndex = std::bit_cast<std::int32_t>(reader.u32());
        geometry.cursorPolicy = reader.u8();
        geometry.onAlternateScreen = reader.u8();
    }
    return reader.ok() && reader.remaining() == 0 && isValid(state) ? std::optional { state } : std::nullopt;
}

bool ImagePublisher::capture()
{
    ImagePlacements next;
    next.revision = 1; // Compare content independently of its revision.
    auto captured = m_captured;
    for (std::size_t index = 0; index < m_terminal.images.stored.size(); ++index) {
        const Image::TerminalImage& image = m_terminal.images.stored[index];
        if (!image.used || !image.pixels) {
            captured[index] = {};
            continue;
        }
        if (image.format != 24 && image.format != 32)
            return false;
        const auto pixels = selectedPixels(image);
        const auto stamp = Image::imageContentStamp(image);
        Captured& cached = captured[index];
        if (cached.pixels != pixels.data() || cached.stamp != stamp || cached.length != pixels.size())
            cached = { .pixels = pixels.data(), .stamp = stamp, .length = static_cast<std::uint32_t>(pixels.size()), .asset = identityOf(image) };
        if (imagePixelBytes(cached.asset) != pixels.size())
            return false;
        next.images[next.imageCount++] = { .imageID = image.imageID, .imageNumber = image.imageNumber, .asset = cached.asset, .isVirtual = image.virtualPlacement, .virtualColumns = image.virtualColumns, .virtualRows = image.virtualRows, .virtualPlacementID = image.virtualPlacementID };
    }
    for (const auto& geometry : m_terminal.images.placements) {
        if (!geometry.used)
            continue;
        const auto images = std::span(next.images).first(next.imageCount);
        const auto binding = std::ranges::find(images, geometry.imageID, &ImageBinding::imageID);
        if (binding == images.end())
            return false;
        next.placements[next.placementCount++] = { .hash = binding->asset.hash, .geometry = geometry };
    }
    if (!isValid(next))
        return false;
    std::vector<std::uint8_t> record;
    encode(record, next);
    if (record == m_record) {
        m_captured = captured;
        return true;
    }
    if (m_state.revision == std::numeric_limits<std::uint32_t>::max())
        return false;
    next.revision = m_state.revision + 1;
    m_state = next;
    m_captured = captured;
    m_record = std::move(record);
    return true;
}

bool ImagePublisher::restore(const ImagePlacements& state)
{
    if (!state.revision || !isValid(state))
        return false;
    ImagePublisher candidate(m_terminal);
    if (!candidate.capture())
        return false;
    auto normalized = state;
    normalized.revision = 1;
    std::vector<std::uint8_t> record;
    encode(record, normalized);
    if (record != candidate.m_record)
        return false;
    m_captured = candidate.m_captured;
    m_state = state;
    m_record = std::move(candidate.m_record);
    return true;
}

struct ImageInflation {
    Codec::Zlib::Inflater inflater;
    Crypto::SHA256Hasher wireDigest;
    /// Where a stream that has filled the decoded length would write next,
    /// so one that has more to give is caught rather than suspended.
    std::array<std::uint8_t, 1> overrun {};
};

ImageTransfer::ImageTransfer(std::uint32_t transfer, ImagePixels pixels, std::unique_ptr<ImageInflation> inflation)
    : m_transfer(transfer)
    , m_pixels(std::move(pixels))
    , m_inflation(std::move(inflation))
{
}

ImageTransfer::ImageTransfer(ImageTransfer&&) noexcept = default;
ImageTransfer& ImageTransfer::operator=(ImageTransfer&&) noexcept = default;
ImageTransfer::~ImageTransfer() = default;

std::optional<std::size_t> ImageTransfer::assemblyBytes(const ImageAssetIdentity& asset, Compression coding)
{
    const auto size = imagePixelBytes(asset);
    if (!size)
        return std::nullopt;
    return *size + (coding == Compression::Deflate ? sizeof(ImageInflation) : 0);
}

std::optional<ImageTransfer> ImageTransfer::begin(const ImageHead& head, std::size_t availableBytes, Compression coding)
{
    const auto size = imagePixelBytes(head.asset);
    const auto needed = assemblyBytes(head.asset, coding);
    if (!head.transfer || !size || !needed || *needed > availableBytes)
        return std::nullopt;
    std::unique_ptr<ImageInflation> inflation;
    if (coding == Compression::Deflate) {
        inflation.reset(new (std::nothrow) ImageInflation);
        if (!inflation)
            return std::nullopt;
    }
    Image::OwnedPixels pixels(static_cast<unsigned char*>(std::malloc(*size)));
    if (!pixels)
        return std::nullopt;
    return ImageTransfer(head.transfer, { .asset = head.asset, .bytes = std::move(pixels), .size = *size }, std::move(inflation));
}

std::size_t ImageTransfer::heldBytes() const
{
    if (!m_pixels.bytes)
        return 0;
    return m_pixels.size + (m_inflation ? sizeof(ImageInflation) : 0);
}

bool ImageTransfer::inflate(std::span<const std::uint8_t> input)
{
    while (!input.empty()) {
        if (m_isInflated)
            return false; // Bytes after the end of the stream.
        const bool isFull = m_received == m_pixels.size;
        const std::span<std::uint8_t> output = isFull ? std::span<std::uint8_t>(m_inflation->overrun)
                                                      : std::span<std::uint8_t>(m_pixels.bytes.get() + m_received, m_pixels.size - m_received);
        const auto result = m_inflation->inflater.decode(input, output);
        if (isFull && result.bytesProduced)
            return false; // The stream inflates past the decoded length.
        input = input.subspan(result.bytesConsumed);
        m_received += result.bytesProduced;
        switch (result.status) {
        case Codec::Zlib::InflateStatus::Done:
            m_isInflated = true;
            break;
        case Codec::Zlib::InflateStatus::Progressed:
        case Codec::Zlib::InflateStatus::NeedMoreInput:
        case Codec::Zlib::InflateStatus::NeedMoreOutput:
            if (!result.bytesConsumed && !result.bytesProduced && (isFull || !input.empty()))
                return false;
            break;
        case Codec::Zlib::InflateStatus::Error:
            return false;
        }
    }
    return true;
}

ImageChunkResult ImageTransfer::accept(const BulkChunk& chunk)
{
    const auto refuse = [&] {
        m_pixels = {};
        m_inflation.reset();
        return ImageChunkResult::Refused;
    };
    // Only the last chunk names the coding, and only Deflate the decoded size.
    const Compression declared = chunk.isLast && m_inflation ? Compression::Deflate : Compression::None;
    const std::size_t declaredBytes = chunk.isLast && m_inflation ? m_pixels.size : 0;
    const std::size_t maxWireBytes = m_inflation ? Codec::Zlib::compressBound(m_pixels.size) : m_pixels.size;
    if (!m_pixels.bytes || m_isComplete || chunk.transfer != m_transfer || chunk.index != m_nextIndex
        || chunk.bytes.empty() || chunk.bytes.size() > bulkChunkPayloadBytes
        || chunk.bytes.size() > maxWireBytes - m_wireBytes
        || chunk.compression != declared || chunk.inflatedBytes != declaredBytes)
        return refuse();
    m_wireBytes += chunk.bytes.size();
    ++m_nextIndex;
    if (m_inflation) {
        if (!m_inflation->wireDigest.update(chunk.bytes) || !inflate(chunk.bytes))
            return refuse();
    } else {
        std::memcpy(m_pixels.bytes.get() + m_received, chunk.bytes.data(), chunk.bytes.size());
        m_received += chunk.bytes.size();
    }
    if (!chunk.isLast)
        return m_received < m_pixels.size || (m_inflation && !m_isInflated) ? ImageChunkResult::Pending : refuse();
    if (m_received != m_pixels.size || (m_inflation && !m_isInflated))
        return refuse();
    std::array<std::uint8_t, 32> digest {};
    if (m_inflation) {
        if (!m_inflation->wireDigest.finalize(digest))
            return refuse();
        m_inflation.reset();
    } else {
        Crypto::sha256(m_pixels.pixels(), digest);
    }
    if (digest != chunk.sha256)
        return refuse();
    m_isComplete = true;
    return ImageChunkResult::Complete;
}

std::optional<ImagePixels> ImageTransfer::take()
{
    if (!m_isComplete || !m_pixels.bytes)
        return std::nullopt;
    return std::move(m_pixels);
}

std::optional<ImagePublisher::CapturedPixels> ImagePublisher::capturedPixels(Digest128 hash) const
{
    for (std::size_t index = 0; index < m_captured.size(); ++index) {
        const Captured& captured = m_captured[index];
        const auto& image = m_terminal.images.stored[index];
        const auto pixels = selectedPixels(image);
        if (!captured.pixels || captured.asset.hash != hash || !image.used || pixels.data() != captured.pixels
            || pixels.size() != captured.length || Image::imageContentStamp(image) != captured.stamp)
            continue;
        if (imagePixelBytes(captured.asset) != pixels.size())
            return std::nullopt;
        return CapturedPixels { .asset = captured.asset, .pixels = pixels };
    }
    return std::nullopt;
}

std::optional<ImagePixels> ImagePublisher::copyPixels(Digest128 hash, std::size_t availableBytes) const
{
    const auto captured = capturedPixels(hash);
    if (!captured || captured->pixels.size() > availableBytes)
        return std::nullopt;
    Image::OwnedPixels bytes(static_cast<unsigned char*>(std::malloc(captured->pixels.size())));
    if (!bytes)
        return std::nullopt;
    std::memcpy(bytes.get(), captured->pixels.data(), captured->pixels.size());
    return ImagePixels { .asset = captured->asset, .bytes = std::move(bytes), .size = captured->pixels.size() };
}

std::unique_ptr<BulkSource> ImagePublisher::copyForTransfer(Digest128 hash, const BulkSourceAdmission& admission, Compression coding) const
{
    const auto charge = transferReservationBytes(hash, coding);
    if (!charge)
        return {};
    auto reservation = BulkSourceReservation::acquire(admission, *charge);
    if (!reservation)
        return {};
    if (coding == Compression::None) {
        auto pixels = copyPixels(hash, *charge);
        if (!pixels)
            return {};
        return std::unique_ptr<BulkSource>(new (std::nothrow) ImageBulkSource(std::move(*reservation), std::move(*pixels)));
    }
    const auto captured = capturedPixels(hash);
    if (!captured)
        return {};
    const std::size_t bound = Codec::Zlib::compressBound(captured->pixels.size());
    Image::OwnedPixels stream(static_cast<unsigned char*>(std::malloc(bound)));
    if (!stream)
        return {};
    const std::size_t length = Codec::Zlib::compressInto(captured->pixels, { stream.get(), bound });
    // A shrinking realloc that fails leaves the larger owner valid and charged.
    if (auto* shrunk = static_cast<unsigned char*>(std::realloc(stream.get(), length))) {
        (void)stream.release();
        stream.reset(shrunk);
        reservation->shrink(*charge - (bound - length));
    }
    return std::unique_ptr<BulkSource>(new (std::nothrow) EncodedImageBulkSource(std::move(*reservation), std::move(stream), length));
}

std::optional<std::size_t> ImagePublisher::transferReservationBytes(Digest128 hash, Compression coding) const
{
    const auto bindings = std::span(m_state.images).first(m_state.imageCount);
    const auto binding = std::ranges::find_if(bindings, [&](const ImageBinding& candidate) { return candidate.asset.hash == hash; });
    if (binding == bindings.end())
        return std::nullopt;
    const auto bytes = imagePixelBytes(binding->asset);
    if (!bytes)
        return std::nullopt;
    if (coding == Compression::Deflate)
        return Codec::Zlib::compressBound(*bytes) + sizeof(EncodedImageBulkSource) + bulkSourceFrameReserveBytes;
    return *bytes + sizeof(ImageBulkSource) + bulkSourceFrameReserveBytes;
}

void ImageReplica::moveTo(ImageReplica& destination)
{
    if (this == &destination)
        return;
    if (Image::totalBytes(&m_terminal) <= destination.m_terminal.images.kittyImageBytesBudget) {
        Image::ContentKeeper kept(m_terminal);
        kept.restoreInto(destination.m_terminal);
        destination.m_cached = std::exchange(m_cached, {});
        destination.m_previousPlacements = std::exchange(m_previousPlacements, {});
    } else {
        Image::disposeAllImages(m_terminal);
        Image::disposeAllImages(destination.m_terminal);
        destination.m_cached = {};
        destination.m_previousPlacements = {};
        m_cached = {};
        m_previousPlacements = {};
    }
    destination.m_state = std::exchange(m_state, {});
    destination.installPlacements();
}

bool ImageReplica::holds(const ImageBinding& binding) const
{
    for (std::size_t index = 0; index < m_terminal.images.stored.size(); ++index) {
        const auto& image = m_terminal.images.stored[index];
        const Cached& cached = m_cached[index];
        if (image.used && image.imageID == binding.imageID && cached.asset == binding.asset
            && cached.pixels == selectedPixels(image).data() && cached.stamp == Image::imageContentStamp(image))
            return true;
    }
    return false;
}

bool ImageReplica::replace(const ImagePlacements& state, std::size_t pendingPixelBytes)
{
    if (!isValid(state))
        return false;
    if (state.revision <= m_state.revision)
        return true;
    // Copy-and-blank moves the malloc owners; no image-sized copy is needed
    // when a program assigns new IDs to pixels the client already holds.
    m_previousPlacements = m_terminal.images.placements;
    auto previous = std::exchange(m_terminal.images.stored, {});
    std::array<ImageAssetIdentity, Image::maxTerminalImageCount> assets {};
    std::array<bool, Image::maxTerminalImageCount> isVerified {};
    for (std::size_t index = 0; index < previous.size(); ++index) {
        const auto& image = previous[index];
        if (!image.used || !image.pixels || (image.format != 24 && image.format != 32))
            continue;
        const auto pixels = selectedPixels(image);
        const Cached& cached = m_cached[index];
        isVerified[index] = cached.pixels == pixels.data() && cached.stamp == Image::imageContentStamp(image);
        assets[index] = isVerified[index] ? cached.asset : identityOf(image);
        if (imagePixelBytes(assets[index]) != pixels.size()) {
            assets[index] = {};
            isVerified[index] = false;
        }
    }
    m_cached = {};
    for (std::size_t index = 0; index < state.imageCount; ++index) {
        const ImageBinding& binding = state.images[index];
        for (std::size_t old = 0; old < previous.size(); ++old) {
            if (!previous[old].used || !previous[old].pixels || assets[old] != binding.asset)
                continue;
            auto& image = m_terminal.images.stored[index];
            // The asset names one selected frame, not the retained terminal's
            // animation timeline. Keep its owner and release the other frames.
            moveImage(m_terminal, image, binding, previous[old]);
            m_cached[index] = { .asset = binding.asset, .pixels = image.pixels, .stamp = Image::imageContentStamp(image) };
            break;
        }
    }
    m_state = state;
    // Reserve room for every missing destination before retaining an old
    // presentation. Its pixels remain in the terminal's ordinary table and
    // byte count; neither slots nor image budgets grow for double buffering.
    const auto held = Image::totalBytes(&m_terminal);
    const auto budget = m_terminal.images.kittyImageBytesBudget;
    std::uint64_t room = held <= budget && pendingPixelBytes <= budget - held ? budget - held - pendingPixelBytes : 0;
    const auto bindings = std::span(m_state.images).first(m_state.imageCount);
    for (const auto& binding : bindings) {
        if (!holds(binding)) {
            const auto bytes = *imagePixelBytes(binding.asset);
            room = bytes <= room ? room - bytes : 0;
        }
    }
    // A missing binding keeps showing the picture this replica verified at
    // one of its placements: the previous pixels under the same image ID, or
    // under an ID the record no longer binds, of the same format and size.
    const auto standsAt = [&](const Image::TerminalImage& image, const ImageBinding& binding) {
        return std::ranges::any_of(std::span(m_state.placements).first(m_state.placementCount), [&](const auto& target) {
            return target.geometry.imageID == binding.imageID && std::ranges::any_of(m_previousPlacements, [&](const auto& oldPlacement) {
                return oldPlacement.used && oldPlacement.imageID == image.imageID && hasSamePlacementGeometry(oldPlacement, target.geometry);
            });
        });
    };
    for (std::size_t index = 0; index < m_state.imageCount; ++index) {
        const auto& binding = m_state.images[index];
        if (holds(binding))
            continue;
        std::optional<std::size_t> retainedIndex;
        for (std::size_t old = 0; old < previous.size(); ++old) {
            const auto& image = previous[old];
            const bool isSameImage = image.imageID == binding.imageID;
            if (!image.used || !image.pixels || !isVerified[old] || assets[old].width != binding.asset.width
                || assets[old].height != binding.asset.height || assets[old].format != binding.asset.format
                || (!isSameImage && std::ranges::any_of(bindings, [&](const auto& current) { return current.imageID == image.imageID; })))
                continue;
            const auto bytes = imagePixelBytes(assets[old]);
            if (!bytes || *bytes > room || !standsAt(image, binding))
                continue;
            if (!retainedIndex || isSameImage)
                retainedIndex = old;
            if (isSameImage)
                break;
        }
        if (!retainedIndex)
            continue;
        auto& image = previous[*retainedIndex];
        const ImageBinding retained { .imageID = image.imageID, .imageNumber = image.imageNumber, .asset = assets[*retainedIndex], .isVirtual = image.virtualPlacement, .virtualColumns = image.virtualColumns, .virtualRows = image.virtualRows, .virtualPlacementID = image.virtualPlacementID };
        auto& destination = m_terminal.images.stored[index];
        moveImage(m_terminal, destination, retained, image);
        m_cached[index] = { .asset = retained.asset, .pixels = destination.pixels, .stamp = Image::imageContentStamp(destination) };
        room -= *imagePixelBytes(retained.asset);
    }
    for (auto& image : previous)
        Image::dispose(&image);
    // Two program IDs may share one asset. Populate a new alias from a local
    // verified copy before asking the host for pixels it has already sent.
    for (const ImageBinding& binding : std::span(m_state.images).first(m_state.imageCount)) {
        if (holds(binding))
            continue;
        for (std::size_t index = 0; index < m_cached.size(); ++index) {
            const auto& image = m_terminal.images.stored[index];
            if (image.used && image.pixels && m_cached[index].asset == binding.asset) {
                (void)acceptPixels(binding.asset, selectedPixels(image), {}, pendingPixelBytes);
                break;
            }
        }
    }
    installPlacements();
    return true;
}

std::vector<ImageAssetIdentity> ImageReplica::missing() const
{
    std::vector<ImageAssetIdentity> missing;
    for (const ImageBinding& binding : std::span(m_state.images).first(m_state.imageCount)) {
        if (!holds(binding) && std::ranges::find(missing, binding.asset) == missing.end())
            missing.push_back(binding.asset);
    }
    return missing;
}

ImagePixelsResult ImageReplica::accept(const ImageAssetIdentity& asset, std::span<const std::uint8_t> pixels)
{
    return acceptPixels(asset, pixels, {}, 0);
}

std::optional<std::size_t> ImageReplica::bytesToInstall(const ImageAssetIdentity& asset) const
{
    const auto size = imagePixelBytes(asset);
    if (!size)
        return std::nullopt;
    std::optional<std::size_t> bytes;
    for (const auto& binding : std::span(m_state.images).first(m_state.imageCount)) {
        if (binding.asset != asset)
            continue;
        bytes = bytes.value_or(0) + (holds(binding) ? 0 : *size);
    }
    return bytes;
}

ImagePixelsResult ImageReplica::accept(ImagePixels pixels, std::size_t otherPendingPixelBytes)
{
    if (!pixels.bytes)
        return ImagePixelsResult::Malformed;
    const auto view = pixels.pixels();
    return acceptPixels(pixels.asset, view, std::move(pixels.bytes), otherPendingPixelBytes);
}

ImagePixelsResult ImageReplica::acceptPixels(const ImageAssetIdentity& asset, std::span<const std::uint8_t> pixels, Image::OwnedPixels owner, std::size_t pendingPixelBytes)
{
    const auto size = imagePixelBytes(asset);
    if (!size || *size != pixels.size() || imageHash(pixels, asset.format, asset.width, asset.height) != asset.hash)
        return ImagePixelsResult::Malformed;
    std::array<Image::OwnedPixels, Image::maxTerminalImageCount> allocated;
    std::size_t copies = 0;
    for (const auto& binding : std::span(m_state.images).first(m_state.imageCount)) {
        if (binding.asset == asset && !holds(binding))
            ++copies;
    }
    if (!copies)
        return ImagePixelsResult::Unwanted;
    const auto fits = [&] {
        const auto held = Image::totalBytes(&m_terminal);
        const auto budget = m_terminal.images.kittyImageBytesBudget;
        return held <= budget && pendingPixelBytes <= budget - held && copies <= (budget - held - pendingPixelBytes) / *size;
    };
    // A retained picture never takes the room its replacement needs.
    if (!fits() && (!retireRetainedPictures() || !fits()))
        return ImagePixelsResult::NoSpace;
    for (std::size_t index = 0; index < m_state.imageCount; ++index) {
        const auto& binding = m_state.images[index];
        if (binding.asset != asset || holds(binding))
            continue;
        if (owner) {
            allocated[index] = std::move(owner);
        } else {
            allocated[index].reset(static_cast<unsigned char*>(std::malloc(*size)));
            if (!allocated[index]) {
                (void)retireRetainedPictures();
                return ImagePixelsResult::NoSpace;
            }
            std::memcpy(allocated[index].get(), pixels.data(), *size);
        }
    }
    for (std::size_t index = 0; index < m_state.imageCount; ++index) {
        if (!allocated[index])
            continue;
        auto& image = m_terminal.images.stored[index];
        installImage(m_terminal, image, m_state.images[index], std::move(allocated[index]));
        m_cached[index] = { .asset = asset, .pixels = image.pixels, .stamp = Image::imageContentStamp(image) };
    }
    installPlacements();
    return ImagePixelsResult::Applied;
}

void ImageReplica::installPlacements()
{
    m_terminal.images.placements = {};
    std::uint32_t count = 0;
    const auto bindings = std::span(m_state.images).first(m_state.imageCount);
    for (const ImagePlacement& placement : std::span(m_state.placements).first(m_state.placementCount)) {
        const auto binding = std::ranges::find(bindings, placement.geometry.imageID, &ImageBinding::imageID);
        if (binding == bindings.end())
            continue;
        if (holds(*binding)) {
            m_terminal.images.placements[count++] = placement.geometry;
            continue;
        }
        // The binding's slot holds the picture retained for it, if any, under
        // the ID that picture was placed with; it is drawn only where it stood.
        const auto index = static_cast<std::size_t>(binding - bindings.begin());
        const auto& image = m_terminal.images.stored[index];
        if (!image.used || !image.pixels)
            continue;
        const auto previous = std::ranges::find_if(m_previousPlacements, [&](const auto& old) {
            return old.used && old.imageID == image.imageID && hasSamePlacementGeometry(old, placement.geometry);
        });
        if (previous != m_previousPlacements.end())
            m_terminal.images.placements[count++] = *previous;
    }
    m_terminal.images.livePlacementCount = count;
    Image::syncAllMirrors(m_terminal);
    Screen::markAllDirty(m_terminal);
}

bool ImageReplica::retireRetainedPictures()
{
    bool didRetire = false;
    for (std::size_t index = 0; index < m_state.imageCount; ++index) {
        auto& image = m_terminal.images.stored[index];
        if (!image.used || holds(m_state.images[index]))
            continue;
        Image::dispose(&image);
        m_cached[index] = {};
        didRetire = true;
    }
    if (didRetire)
        installPlacements();
    return didRetire;
}

} // namespace Ubiquitty::Protocol::Sync
