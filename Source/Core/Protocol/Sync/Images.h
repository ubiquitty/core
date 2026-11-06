// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Sync/BulkSource.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Hash.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

struct BulkChunk;

/// Decoded pixels addressed within one pane. No transport medium or filename
/// enters this model: the host has already decoded Kitty, Sixel or iTerm data.
struct ImageAssetIdentity {
    Digest128 hash;
    std::uint8_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const ImageAssetIdentity&, const ImageAssetIdentity&) = default;
};

/// Announces pixels, with zero `transfer` when the receiver may already hold
/// them. A nonzero transfer carries the decoded bytes of `asset`, raw or as
/// one zlib stream as the connection's image coding says.
struct ImageHead {
    ImageAssetIdentity asset;
    std::uint32_t transfer = 0;
};

/// A missing asset, looked up only in the pane the request addresses.
struct ImageWant {
    Digest128 hash;
};

/// The program's image identity and virtual-placement metadata. Different
/// image IDs may name identical pixels, but retain independent placements.
struct ImageBinding {
    std::uint32_t imageID = 0;
    std::uint32_t imageNumber = 0;
    ImageAssetIdentity asset;
    bool isVirtual = false;
    std::uint16_t virtualColumns = 0;
    std::uint16_t virtualRows = 0;
    std::uint32_t virtualPlacementID = 0;
};

/// One placement, naming both the program's image and its immutable pixels.
struct ImagePlacement {
    Digest128 hash;
    Image::TerminalImagePlacement geometry {};
};

/// The bytes one ImageBinding occupies in an ImagePlacements body: image ID,
/// image number, the asset identity, the virtual flag and its three fields.
///
/// The editors' clients cannot include this header, so their generated wire
/// tables carry it; a reader that walks a record of another length is
/// reading a layout the host does not send.
constexpr std::size_t imageBindingRecordBytes = 42;

/// The bytes one ImagePlacement occupies in an ImagePlacements body: the asset
/// hash, image and placement IDs, the placement-ID flag, the anchor, the cell
/// box, the pixel offset, the source rectangle, the drawn size, the z-index,
/// the cursor policy and the screen.
constexpr std::size_t imagePlacementRecordBytes = 55;

/// A whole replacement, including transmitted-but-not-displayed images. Fixed
/// tables bound every decoder allocation independently of wire counts.
struct ImagePlacements {
    std::uint32_t revision = 0;
    std::array<ImageBinding, Image::maxTerminalImageCount> images {};
    std::size_t imageCount = 0;
    std::array<ImagePlacement, Image::maxTerminalImagePlacementCount> placements {};
    std::size_t placementCount = 0;
};

/// Compares placement geometry without program image or placement IDs.
/// Pixel dimensions/format must be compared separately by the caller.
///
/// - Parameters:
///   - left: The previous placement.
///   - right: Its proposed replacement.
/// - Returns: Whether every geometry, screen and stacking field agrees.
[[nodiscard]] bool hasSamePlacementGeometry(const Image::TerminalImagePlacement& left, const Image::TerminalImagePlacement& right);

/// The exact decoded length, or no value for an unsupported format, invalid
/// dimensions or a body above the terminal's decoded-image cap.
[[nodiscard]] std::optional<std::size_t> imagePixelBytes(const ImageAssetIdentity&);

/// Whether every binding and placement is self-consistent and bounded.
[[nodiscard]] bool isValid(const ImagePlacements&);

/// Decodes a whole record; trailing bytes and noncanonical fields are refused.
[[nodiscard]] std::optional<ImageHead> decodeImageHead(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<ImageWant> decodeImageWant(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<ImagePlacements> decodeImagePlacements(std::span<const std::uint8_t>);

/// A stable decoded-pixel owner, suitable for lending bounded chunks until a
/// transfer ends. Replacing or deleting the terminal's image cannot change it.
/// The caller must reserve its full capacity before making this snapshot; a
/// connection's queued-bulk budget is not an asset-storage reservation.
struct ImagePixels {
    ImageAssetIdentity asset;
    Image::OwnedPixels bytes;
    std::size_t size = 0;

    [[nodiscard]] std::span<const std::uint8_t> pixels() const { return { bytes.get(), size }; }
};

/// What one raw image chunk did to an exact-sized pixel assembly.
enum class ImageChunkResult : std::uint8_t {
    Pending,
    Complete,
    Refused,
};

/// The inflater and wire digest of one deflated image transfer.
struct ImageInflation;

/// One announced pixel transfer, raw or deflated. Storage is allocated
/// exactly once from the validated ImageHead dimensions, never grown by
/// incoming chunks: a deflated stream inflates as it arrives into exactly the
/// decoded size and is refused the moment it would write past it. This owner
/// may be moved between fixed connection slots; moving it moves the retained
/// capacity, and taking a complete result transfers that capacity to the
/// returned ImagePixels until the replica consumes it.
class ImageTransfer {
public:
    ImageTransfer(const ImageTransfer&) = delete;
    ImageTransfer& operator=(const ImageTransfer&) = delete;
    /// Moves the assembly, its charged capacity and its inflater.
    ImageTransfer(ImageTransfer&&) noexcept;
    ImageTransfer& operator=(ImageTransfer&&) noexcept;
    /// Releases the assembly and its inflater.
    ~ImageTransfer();

    /// The bytes a transfer of `asset` holds while it assembles: the decoded
    /// pixels, and for a deflated one its inflater.
    /// - Returns: No value for an asset whose decoded size is refused.
    [[nodiscard]] static std::optional<std::size_t> assemblyBytes(const ImageAssetIdentity&, Compression);

    /// Admits the exact assembly before allocating. The caller has already
    /// charged its fixed transfer slot and subtracts all other live receive
    /// owners from the connection allowance. No maximum-sized buffer is
    /// reserved for a smaller image. Transfer zero is an offer, not data.
    [[nodiscard]] static std::optional<ImageTransfer> begin(const ImageHead&, std::size_t availableBytes, Compression = Compression::None);

    /// Accepts ordered nonempty chunks. A raw transfer carries exactly the
    /// decoded length; a deflated one carries at most
    /// `Codec::Zlib::compressBound` of it, inflates to exactly that length
    /// and ends where its stream does. The final chunk names the coding, the
    /// decoded length for Deflate, and the SHA-256 of the bytes as they
    /// crossed. Any refusal releases this assembly without touching a
    /// terminal image.
    [[nodiscard]] ImageChunkResult accept(const BulkChunk&);

    /// Takes a complete owner without copying; no value before completion.
    [[nodiscard]] std::optional<ImagePixels> take();

    /// Capacity remains fully charged while partial or complete, not merely
    /// for the received prefix. After take or refusal this is zero.
    [[nodiscard]] std::size_t heldBytes() const;

private:
    /// Wraps an admitted assembly; `begin` is the only maker.
    ///
    /// - Parameters:
    ///   - transfer: The transfer number the chunks carry.
    ///   - pixels: The decoded-size buffer, already charged.
    ///   - inflation: The inflater for a deflated transfer, or null.
    ImageTransfer(std::uint32_t transfer, ImagePixels pixels, std::unique_ptr<ImageInflation> inflation);

    [[nodiscard]] bool inflate(std::span<const std::uint8_t>);

    std::uint32_t m_transfer;
    ImagePixels m_pixels;
    std::unique_ptr<ImageInflation> m_inflation;
    std::size_t m_received = 0;
    std::size_t m_wireBytes = 0;
    std::uint32_t m_nextIndex = 0;
    bool m_isInflated = false;
    bool m_isComplete = false;
};

/// Captures one host pane's image metadata. Pixel hashes are recomputed only
/// when Core's content stamp changes; placement changes carry no pixel copy.
/// The terminal must outlive this publisher. A publisher is never shared
/// between sessions, even if they contain identical pixels.
class ImagePublisher {
public:
    explicit ImagePublisher(const Ubiquitty::Terminal& terminal)
        : m_terminal(terminal)
    {
    }
    ImagePublisher(const ImagePublisher&) = delete;
    ImagePublisher& operator=(const ImagePublisher&) = delete;

    /// Captures the current store. False leaves the last valid record intact.
    [[nodiscard]] bool capture();

    /// Rebuilds pixel lookup caches under an exact inherited placement revision.
    /// - Parameter state: Published metadata for the restored terminal.
    /// - Returns: False without mutation if its content differs from the terminal.
    [[nodiscard]] bool restore(const ImagePlacements& state);

    /// The last captured whole record; revision zero until the first capture.
    [[nodiscard]] const ImagePlacements& state() const { return m_state; }

    /// Pins an asset this pane still holds. The caller's available reservation
    /// is checked before allocation; an unknown, replaced or over-budget hash
    /// yields no owner. The returned capacity must remain charged until freed.
    [[nodiscard]] std::optional<ImagePixels> copyPixels(Digest128 hash, std::size_t availableBytes) const;

    /// Reserves in the connection and aggregate outbound ledgers before
    /// copying pixels. The returned immutable source includes its exact
    /// allocation, owner object and bounded frame scratch in that reservation.
    /// A deflated source is reserved at `Codec::Zlib::compressBound` of the
    /// decoded length, compresses straight from the terminal's pixels, and
    /// gives back what its stream did not use. Admission failure, a stale hash
    /// or allocation failure releases both charges without queuing a head.
    [[nodiscard]] std::unique_ptr<BulkSource> copyForTransfer(Digest128 hash, const BulkSourceAdmission&, Compression = Compression::None) const;

    /// The complete reservation a transfer of a captured asset needs, without
    /// allocating or reading its pixels. No value means this pane has no such
    /// captured asset. The actual copy rechecks that the terminal still holds it.
    [[nodiscard]] std::optional<std::size_t> transferReservationBytes(Digest128 hash, Compression = Compression::None) const;

private:
    /// Associates a content stamp with its cached decoded-pixel identity.
    struct Captured {
        const unsigned char* pixels = nullptr;
        std::uint64_t stamp = 0;
        std::uint32_t length = 0;
        ImageAssetIdentity asset;
    };

    /// A captured asset and the terminal's pixels for it.
    struct CapturedPixels {
        ImageAssetIdentity asset;
        std::span<const std::uint8_t> pixels;
    };

    /// The terminal's pixels for a captured asset it still holds unchanged.
    [[nodiscard]] std::optional<CapturedPixels> capturedPixels(Digest128 hash) const;

    const Ubiquitty::Terminal& m_terminal;
    std::array<Captured, Image::maxTerminalImageCount> m_captured {};
    ImagePlacements m_state;
    std::vector<std::uint8_t> m_record;
};

/// What receiving one complete, hash-checked asset did.
enum class ImagePixelsResult : std::uint8_t {
    Applied,
    /// The current metadata does not reference this asset.
    Unwanted,
    Malformed,
    NoSpace,
};

/// One pane's image replica. The terminal remains the only pixel owner and
/// the renderers keep reading its ordinary image/placement arrays. A replica
/// cannot be reused for another pane or host incarnation. Retaining this
/// terminal across a connection replacement retains its verified pixels.
class ImageReplica {
public:
    explicit ImageReplica(Ubiquitty::Terminal& terminal)
        : m_terminal(terminal)
    {
    }
    ImageReplica(const ImageReplica&) = delete;
    ImageReplica& operator=(const ImageReplica&) = delete;

    /// Rebinds metadata without taking or disposing either terminal's pixels.
    /// The caller moves shared pixel ownership once before retiring the source.
    ///
    /// - Parameters:
    ///   - terminal: The replacement terminal for this pane.
    ///   - previous: Another binding of the same pane and host incarnation.
    ImageReplica(Ubiquitty::Terminal& terminal, const ImageReplica& previous)
        : m_terminal(terminal)
        , m_state(previous.m_state)
        , m_cached(previous.m_cached)
        , m_previousPlacements(previous.m_previousPlacements)
    {
    }

    /// Moves this pane's metadata and terminal-owned pixels to a replacement
    /// replica before its old terminal can die. Pixels exceeding the new
    /// terminal's budget are evicted; the binding record remains requestable.
    /// Both replicas must represent the same pane and host incarnation.
    ///
    /// - Parameter destination: The replacement replica receiving pixel ownership.
    void moveTo(ImageReplica& destination);

    /// Atomically validates a whole metadata record before changing anything.
    /// Available pixels are reused by content, even when image IDs move.
    ///
    /// A binding whose pixels are missing keeps showing the verified picture
    /// that stood at one of its placements until its own pixels arrive: the
    /// previous pixels under the same image ID, or under an ID the record no
    /// longer binds, with the same format and dimensions, drawn only at the
    /// placements whose geometry is unchanged. Any other placement of a
    /// missing binding is withheld, never drawn with unverified or stale
    /// pixels. A retained picture occupies the binding's own image slot and
    /// is kept only while the terminal's image budget holds it beside every
    /// missing destination. `pendingPixelBytes` is capacity reserved for this
    /// terminal's other incoming owners and their required duplicate
    /// bindings. Reusing cached aliases must not allocate into that
    /// reservation.
    [[nodiscard]] bool replace(const ImagePlacements&, std::size_t pendingPixelBytes = 0);

    /// Disposes every retained picture, for a replacement that needs its room.
    /// - Returns: Whether any picture was retired.
    bool retireRetainedPictures();

    /// The unique missing assets, bounded by the image table's size. The
    /// connection decides when to ask, and bounds outstanding Wants across
    /// every pane; repeatedly reading this list allocates no pixel storage.
    [[nodiscard]] std::vector<ImageAssetIdentity> missing() const;

    /// Bytes needed by every missing binding of an asset, zero when all are
    /// held, or no value when the current metadata does not reference it.
    /// A connection reserves this destination capacity before receiving one
    /// pixel owner; the first binding consumes that owner, not another copy.
    [[nodiscard]] std::optional<std::size_t> bytesToInstall(const ImageAssetIdentity&) const;

    /// Verifies and installs a complete decoded asset. Every destination copy
    /// is admitted and allocated before any is installed, so a failed hash or
    /// allocation leaves the existing picture unchanged.
    [[nodiscard]] ImagePixelsResult accept(const ImageAssetIdentity&, std::span<const std::uint8_t> pixels);

    /// Consumes a complete decoded owner, including on refusal. The first
    /// missing binding takes that allocation directly; only additional image
    /// IDs sharing the asset need copies. All copies are admitted and made
    /// before any binding changes. The caller keeps the incoming capacity
    /// charged until this call returns, then accounts installed terminal bytes.
    /// `otherPendingPixelBytes` excludes this owner's destination reservation
    /// and includes every other incoming asset's reserved destination bytes.
    [[nodiscard]] ImagePixelsResult accept(ImagePixels pixels, std::size_t otherPendingPixelBytes = 0);

    /// The metadata revision held, independent of how many pixels are cached.
    [[nodiscard]] std::uint32_t revision() const { return m_state.revision; }

private:
    /// Records the terminal-owned pixels verified for one asset identity.
    struct Cached {
        ImageAssetIdentity asset;
        const unsigned char* pixels = nullptr;
        std::uint64_t stamp = 0;
    };

    /// Installs placements whose complete verified pixels are held.
    void installPlacements();
    [[nodiscard]] bool holds(const ImageBinding&) const;
    [[nodiscard]] ImagePixelsResult acceptPixels(const ImageAssetIdentity&, std::span<const std::uint8_t>, Image::OwnedPixels, std::size_t pendingPixelBytes);

    Ubiquitty::Terminal& m_terminal;
    ImagePlacements m_state;
    std::array<Cached, Image::maxTerminalImageCount> m_cached {};
    /// The placements shown before the current record, where a retained
    /// picture may still be drawn. These refer only to owners in the
    /// terminal's existing image table.
    std::array<Image::TerminalImagePlacement, Image::maxTerminalImagePlacementCount> m_previousPlacements {};
};

} // namespace Ubiquitty::Protocol::Sync
