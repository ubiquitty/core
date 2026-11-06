// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Envelope.h — the one header every Sync frame travels under
// (Docs/sync.md §7.2):
//
//     frame := kind:u8 channel:varint length:varint body[length]      length ≤ 65,535
//
// - Channel 0 is the connection; a channel n ≥ 1 is a pane handle a `PaneBind`
//   bound. A kind belongs to one of the two, and an envelope pairing a kind
//   with the other is malformed.
// - The lane a frame rides is implied by its kind and never encoded.
// - The body cap is what keeps one frame from holding the stream: at most
//   65,535 bytes, so a payload above it travels as a bulk transfer, chunked.
//
// A reader fails closed: a length above the cap, an unknown kind, an overlong
// varint or a channel of the wrong scope stops the decoder for good, and the
// connection answers it with `Bye`. Nothing after a failure is read, so a
// hostile length can never make the reader buffer more than one capped frame.
//

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The protocol version this build speaks. It covers the canonical grammar,
/// typed operations and records, and the Unicode width table. There is one
/// active version: two builds connect only when they speak the same one, and
/// a peer at any other is refused. Every wire change increments this version.
inline constexpr std::uint16_t wireVersion = 2;

/// The wire version published in v0.3.3, paired with its frozen fingerprint.
inline constexpr std::uint16_t lastPublishedWireVersion = 2;

/// The digest of every body layout this build's wire defines: the SHA-256 of
/// the corpus `Tests/Core/Protocol/Sync/test_sync_wire_fingerprint.cpp`
/// encodes, which fails until this is re-minted whenever any layout moves.
///
/// Greet and Welcome carry it, and a peer whose fingerprint differs is refused
/// as `ByeReason::VersionMismatch` before the rest of its body is read. This
/// also refuses a build that changed its grammar without advancing the wire
/// version. An editor client that writes the wire itself keeps a copy, which
/// `Scripts/check-wire-version-pins.py` holds to this one.
inline constexpr std::array<std::uint8_t, 32> wireFingerprint {
    0x6a,
    0x3d,
    0xd7,
    0x16,
    0x63,
    0x3a,
    0xf4,
    0xc7,
    0x00,
    0x2e,
    0x77,
    0xbf,
    0x39,
    0x0a,
    0xe0,
    0xd9,
    0xc5,
    0xe8,
    0xe9,
    0x8d,
    0x70,
    0x36,
    0x56,
    0xa9,
    0xcb,
    0x17,
    0x85,
    0x6a,
    0xda,
    0x25,
    0x80,
    0x11,
};

/// The fingerprint published in v0.3.3. This release pin is independent of
/// the active grammar; it is never a second accepted handshake identity.
inline constexpr std::array<std::uint8_t, 32> lastPublishedWireFingerprint {
    0x6a,
    0x3d,
    0xd7,
    0x16,
    0x63,
    0x3a,
    0xf4,
    0xc7,
    0x00,
    0x2e,
    0x77,
    0xbf,
    0x39,
    0x0a,
    0xe0,
    0xd9,
    0xc5,
    0xe8,
    0xe9,
    0x8d,
    0x70,
    0x36,
    0x56,
    0xa9,
    0xcb,
    0x17,
    0x85,
    0x6a,
    0xda,
    0x25,
    0x80,
    0x11,
};

/// The sentence a wire says when two builds speak different versions or
/// carry different fingerprints.
/// It names no side: the handshake that refused carries both builds, and the
/// sentence a person reads is composed from them (Session/DeployPolicy.h).
[[nodiscard]] constexpr std::string_view versionMismatchNotice()
{
    return "This app and the host are from different builds of Ubiquitty. Update the older one.";
}

/// The largest body one frame may carry: the grammar's `length ≤ 65,535`.
inline constexpr std::size_t maxFrameBodyBytes = 65'535;

/// Accounting slack used with retained payload budgets, not the canonical
/// envelope's header size (`maxEnvelopeHeaderBytes`).
inline constexpr std::size_t catalogFrameHeaderBytes = 5;

/// Connection-owned payload memory ceiling. Typed image, sample and effect
/// owners share this budget; no single remote frame may approach this size.
inline constexpr std::size_t maxCatalogMessageBytes = catalogFrameHeaderBytes + 72u * 1024u * 1024u;

/// How a bulk transfer's payload is coded on the wire (Docs/sync.md §7.3).
enum class Compression : std::uint8_t {
    None,
    /// A zlib stream (RFC 1950) whose inflated length the last chunk declares.
    Deflate,
};

/// Client-owned clipboard payload budget, including bounded MIME metadata
/// and accounting slack. It is not an envelope body limit.
inline constexpr std::size_t maxClientCatalogMessageBytes = catalogFrameHeaderBytes + 4u * 1024u * 1024u + 32u * (0xFFFFu + 2u + 4u) + 16u;
static_assert(maxClientCatalogMessageBytes <= maxCatalogMessageBytes);

/// The largest channel number; a pane handle is a `u32` on every peer.
inline constexpr std::uint64_t maxChannel = 0xFFFF'FFFF;

/// The most bytes an envelope header can take: the kind, a five-byte channel
/// and a three-byte length.
inline constexpr std::size_t maxEnvelopeHeaderBytes = 1 + 5 + 3;

/// What a frame is. The values are the wire's `kind` byte.
enum class FrameKind : std::uint8_t {
    Greet = 1,
    Welcome = 2,
    Facts = 3,
    Ack = 4,
    Probe = 5,
    Echo = 6,
    PaneBind = 7,
    PaneUnbind = 8,
    BulkChunk = 9,
    Bye = 10,
    /// One bound pane's renderer revision, capability and typography table.
    /// Uses that pane's channel because font choice and zoom are per surface.
    TypographyFacts = 11,
    /// The host abandoned a transfer it had started: no further chunk of it
    /// follows, and the client frees what of it it holds (Docs/sync.md §6.5).
    BulkAbort = 12,
    /// A JSON-RPC 2.0 request, either way (Docs/sync.md §7.3).
    Call = 13,
    /// The answer to a `Call`, inline or announcing the transfer that
    /// carries it.
    Result = 14,
    /// Something the host tells a client unasked: a `NotifyKind` and that
    /// kind's record (Docs/sync.md §7.3). No answer follows.
    Notify = 15,
    /// A workspace's structure: its windows, tabs, splits and what each is
    /// called (Docs/sync.md §7.3). The record is in the frame when it fits
    /// one and follows as a bulk transfer when it does not. Sent to a client
    /// attached to the workspace and to one listing the host.
    WorkspaceStructure = 16,
    /// Lists a pane on this connection: the client is sent its `PaneFacts`
    /// and `StatusRecords` on the handle and nothing else of it, no screen
    /// and no history, and can address nothing to it. `PaneUnbind` ends it.
    PaneListed = 17,
    /// The faces the client draws proportional runs in, as its measurer
    /// counts them (Docs/proportional-text.md): a fact about the client the
    /// host counts a hosted pane's runs in, in a frame of its own beside
    /// `TypographyFacts` because rich text and runs are measured apart.
    ProportionalFaces = 18,
    /// A `Dormant` pane's palette moved: its palette block, whole, as an
    /// update carries it, so what the client draws of the pane outside its
    /// screen, a tab's label among it, wears the pane's colours while no
    /// screen is sent (Docs/sync.md §6.8).
    PanePalette = 19,
    CheckpointHead = 32,
    Update = 33,
    Resync = 34,
    /// The client's view of a pane: `Live`, `Dormant` or `RecordsOnly`
    /// (Docs/sync.md §6.8).
    View = 36,
    /// A `Dormant` pane changed: sent once per span the client keeps it
    /// `Dormant`.
    PaneDirty = 37,
    /// Appends that fit one frame, ahead of the update that scrolls them
    /// (Docs/sync.md §7.5).
    HistoryInline = 40,
    /// Announces history lines whose body follows as a bulk transfer.
    HistoryHead = 41,
    /// A destructive clear: the client purges every line of the old epoch
    /// before it applies anything else.
    HistoryReset = 42,
    /// The client's wanted ranges at one priority, replacing what it wanted
    /// at that priority before.
    HistoryWant = 43,
    /// What the host says about a pane beside its picture: what a title, a
    /// tab and a listing are drawn from (Docs/sync.md §7.7). Whole, under a
    /// revision, in every view state.
    PaneFacts = 48,
    /// The status a pane's programs posted, for a pane whose screen the
    /// client is not receiving. Whole, under a revision; a set too large for
    /// one frame travels as several, back to back.
    StatusRecords = 49,
    /// One record or completion cursor of a pane's ordered effect batch.
    Effect = 50,
    /// One correlated, pane-bound request to an authorized clipboard server.
    ClipboardRequest = 51,
    /// Decoded image identity and an optional raw pixel transfer.
    ImageHead = 52,
    /// Whole image bindings and placements, independent of pixel transfers.
    ImagePlacements = 53,
    /// An exact immutable PCM identity and its raw sample transfer.
    SampleHead = 54,
    /// Shape an application document in the displaying attachment's fonts.
    RichTextMeasure = 55,
    NativeDragCommand = 56,
    Receipt = 64,
    /// Keys a client typed, encoded on the host under the modes live when it
    /// writes them (Docs/sync.md §6.7).
    Keys = 65,
    /// Pointer events the client's pointer ladder routed to the program,
    /// encoded on the host likewise.
    Pointer = 66,
    /// A piece of a paste, on the bulk lane, written under the bracket and
    /// the paste normaliser when the host writes it (Docs/sync.md §6.7).
    Paste = 67,
    /// The client gives up a paste it began; on the interactive lane, so it
    /// never waits behind the pieces it cancels.
    PasteAbort = 68,
    /// Input a client sends as bytes, written to the pane's program verbatim
    /// (Docs/sync.md §6.7). Numbers 65 to 68 and 70 are the other input kinds
    /// of §7.6, in its table's order.
    Raw = 69,
    /// Whether the client's view of the pane holds the keyboard focus.
    Focus = 70,
    /// How much of an unfinished paste the host has written, so its client
    /// sends no more than a window past it (Docs/sync.md §6.7).
    PasteReceipt = 71,
    /// A run of the client's events the pane dropped unwritten: a signal
    /// character discarded them with the input the terminal held unread
    /// (Docs/sync.md §6.7).
    InputDiscarded = 72,
    /// The size a driver shows a pane at: its viewport of that pane, in
    /// cells and in pixels (Docs/sync.md §6.10). The window's own size
    /// travels in `Facts`.
    Viewport = 73,
    /// A correlated clipboard answer, inline or announcing exact raw bulk.
    ClipboardData = 74,
    /// Pixels missing from this pane's verified image cache.
    ImageWant = 75,
    RichTextMeasured = 76,
    NativeDragEvent = 77,
};

/// Every frame kind, in ascending wire order: the one list of the kind
/// numbers. The reader (`frameKindOf`) accepts exactly these, which
/// `test_sync_envelope` checks byte by byte.
inline constexpr FrameKind frameKinds[] {
    FrameKind::Greet,
    FrameKind::Welcome,
    FrameKind::Facts,
    FrameKind::Ack,
    FrameKind::Probe,
    FrameKind::Echo,
    FrameKind::PaneBind,
    FrameKind::PaneUnbind,
    FrameKind::BulkChunk,
    FrameKind::Bye,
    FrameKind::TypographyFacts,
    FrameKind::BulkAbort,
    FrameKind::Call,
    FrameKind::Result,
    FrameKind::Notify,
    FrameKind::WorkspaceStructure,
    FrameKind::PaneListed,
    FrameKind::ProportionalFaces,
    FrameKind::PanePalette,
    FrameKind::CheckpointHead,
    FrameKind::Update,
    FrameKind::Resync,
    FrameKind::View,
    FrameKind::PaneDirty,
    FrameKind::HistoryInline,
    FrameKind::HistoryHead,
    FrameKind::HistoryReset,
    FrameKind::HistoryWant,
    FrameKind::PaneFacts,
    FrameKind::StatusRecords,
    FrameKind::Effect,
    FrameKind::ClipboardRequest,
    FrameKind::ImageHead,
    FrameKind::ImagePlacements,
    FrameKind::SampleHead,
    FrameKind::RichTextMeasure,
    FrameKind::NativeDragCommand,
    FrameKind::Receipt,
    FrameKind::Keys,
    FrameKind::Pointer,
    FrameKind::Paste,
    FrameKind::PasteAbort,
    FrameKind::Raw,
    FrameKind::Focus,
    FrameKind::PasteReceipt,
    FrameKind::InputDiscarded,
    FrameKind::Viewport,
    FrameKind::ClipboardData,
    FrameKind::ImageWant,
    FrameKind::RichTextMeasured,
    FrameKind::NativeDragEvent,
};

// Strictly ascending, so no two kinds share a number: a kind given another's
// number would be read as that other kind.
static_assert([] {
    for (std::size_t index = 1; index < std::size(frameKinds); ++index) {
        if (std::to_underlying(frameKinds[index - 1]) >= std::to_underlying(frameKinds[index]))
            return false;
    }
    return true;
}(),
    "every frame kind has a number of its own");

/// What a `Notify` says. The value is the body's leading `kind:varint`; the
/// rest of the body is that kind's record (Messages.h). A reader skips a kind
/// it does not know by the frame's length, so a host may add one without
/// breaking an older client.
enum class NotifyKind : std::uint8_t {
    /// Who else holds the panes this connection has bound.
    Attachments = 1,
    /// A batch of host events, to a connection that subscribed.
    Events = 2,
    /// The phase of this attachment's own consent question.
    ConsentStatus = 3,
    /// An attach, terminate or capability question for a person to decide.
    ConsentQuestion = 4,
    /// An exact tree operation an agent asked for, for a person to decide.
    StructureConsent = 5,
    /// A question this recipient was asked is no longer open.
    ConsentResolved = 6,
    /// Upload credit for an admitted `paste.push`.
    PasteReady = 7,
};

/// Maps a `Notify`'s leading varint to a kind.
///
/// - Parameter value: The decoded `kind:varint`.
/// - Returns: The kind, or `nullopt` for a value no kind has, which a reader
///   skips.
[[nodiscard]] std::optional<NotifyKind> notifyKindOf(std::uint64_t value);

/// Which half of the stream a frame waits in (Docs/sync.md §6.5).
enum class Lane : std::uint8_t {
    /// Control, acknowledgements, receipts, input and screen updates.
    Interactive,
    /// Chunks of a transfer too large for one frame.
    Bulk,
};

/// Which peer may send a kind.
enum class Direction : std::uint8_t {
    HostToClient,
    ClientToHost,
    Both,
};

/// Maps a wire byte to a kind.
///
/// - Parameter value: The `kind` byte.
/// - Returns: The kind, or `nullopt` for a byte no kind has.
[[nodiscard]] std::optional<FrameKind> frameKindOf(std::uint8_t value);

/// Returns the canonical kind name or an immutable hexadecimal byte label.
[[nodiscard]] const char* frameKindName(std::uint8_t value);

/// The lane a kind rides.
[[nodiscard]] Lane laneOf(FrameKind kind);

/// The peer that may send a kind.
[[nodiscard]] Direction directionOf(FrameKind kind);

/// Whether a kind travels on channel 0, the connection, rather than on a pane
/// handle.
[[nodiscard]] bool isConnectionKind(FrameKind kind);

/// One decoded frame. `body` borrows the decoder's storage and is valid until
/// the decoder is next written to or read from.
struct Frame {
    FrameKind kind = FrameKind::Bye;
    std::uint32_t channel = 0;
    std::span<const std::uint8_t> body;
};

/// Why a decoder stopped.
enum class EnvelopeError : std::uint8_t {
    None,
    /// `length` above `maxFrameBodyBytes`.
    Oversize,
    /// A `kind` byte no kind has.
    UnknownKind,
    /// An overlong or out-of-range varint, a channel of the wrong scope, or a
    /// datagram that is not exactly one envelope.
    Malformed,
};

/// A header's progress before any frame body is retained.
enum class FrameHeaderState : std::uint8_t {
    NeedsMore,
    Complete,
    Failed,
};

/// The envelope fields needed to admit a body before allocating it.
struct FrameHeader {
    FrameKind kind = FrameKind::Bye;
    std::uint32_t channel = 0;
    std::size_t bodyBytes = 0;
};

/// Reads only the header, using the same canonical grammar as datagrams.
/// Complete fills header; Failed fills error. No storage is allocated.
[[nodiscard]] FrameHeaderState readFrameHeader(std::span<const std::uint8_t> bytes, FrameHeader& header, EnvelopeError& error);

/// Appends one frame.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - kind: The frame's kind.
///   - channel: 0 for a connection kind, a pane handle otherwise.
///   - body: At most `maxFrameBodyBytes`.
/// - Returns: False, with `output` unchanged, when the body is over the cap or
///   the channel is of the wrong scope for the kind.
[[nodiscard]] bool appendFrame(std::vector<std::uint8_t>& output, FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body);

/// The bytes a frame with a body of `bodyBytes` takes on the wire.
[[nodiscard]] std::size_t frameBytes(std::uint32_t channel, std::size_t bodyBytes);

/// Splits a byte stream into frames.
///
/// It holds every byte appended until `next` takes the frame they belong to,
/// with no cap of its own, so it is for bytes whose amount the caller already
/// bounds. A peer's stream goes through `FrameIngress`, which admits each
/// header before it holds the body.
class FrameDecoder {
public:
    /// Appends bytes read from the stream.
    ///
    /// - Parameter bytes: The next bytes, in stream order. Ignored once the
    ///   decoder has failed.
    void append(std::span<const std::uint8_t> bytes);

    /// Takes the next complete frame.
    ///
    /// - Returns: The frame, or `nullopt` when the buffered bytes hold no
    ///   complete frame or the decoder has failed.
    [[nodiscard]] std::optional<Frame> next();

    /// Why the decoder stopped, or `None` while it has not.
    [[nodiscard]] EnvelopeError error() const { return m_error; }

    /// The bytes buffered and not yet returned as a frame.
    [[nodiscard]] std::size_t bufferedBytes() const { return m_buffer.size() - m_offset; }

private:
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_offset = 0;
    EnvelopeError m_error = EnvelopeError::None;
};

/// Decodes a datagram, which carries exactly one envelope.
///
/// - Parameters:
///   - datagram: The datagram's payload.
///   - error: Receives why the datagram was refused, or `None`.
/// - Returns: The frame, borrowing `datagram`, or `nullopt` when refused.
[[nodiscard]] std::optional<Frame> decodeDatagram(std::span<const std::uint8_t> datagram, EnvelopeError& error);

} // namespace Ubiquitty::Protocol::Sync
