// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Messages.h — the bodies of the connection plane and the screen-plane
// frames that carry pacing (Docs/sync.md §7.3, §7.4):
//
//     Greet          := wireVersion:u16 appVersion:utf8 engineFingerprint:[32]
//                       clientInstance:u128 clientKind:u8 requestedRole:u8 request:AdmissionRequest
//                       hasCredential:u8 [credential:bytes] facts:Facts
//                       historyCacheLines:u32 backfillDepthLines:u32
//                       capabilities:varint (a bit per `ClientCapability`)
//                       hasResume:u8 [ResumeRequest when 1]
//     ResumeRequest  := attachmentID:u128 hasSecret:u8 [resumeSecret:u128]
//                       panes:[PaneResume] (at most 256)
//     PaneResume     := paneID:u128 ackedGeneration:u64 ackedRowHashesDigest:u128
//                       historyEpoch:u32 cachedRanges:[(from:u64, to:u64)] (at most 64)
//                       lastInputSeqSent:u64 view:u8 effectsAcknowledged:u64
//     Welcome        := wireVersion:u16 appVersion:utf8 engineFingerprint:[32] hostInstanceID:u128
//                       attachmentID:u128 resumeSecret:u128 grantedRole:u8
//                       selection:ServingSelection admission:u8 consentRequest:u32 consentSession:u32
//                       principal:utf8 limits:Limits hostTime:u64
//                       hasPredecessor:u8 [predecessorHostInstanceID:u128]
//                       resumed:[PaneResumed] hasPaste:u8 [PasteLimits]
//     PaneResumed    := paneID:u128 outcome:u8 {Diffed, Checkpointed, Gone}
//                       hasInputWritten:u8 [inputWritten:u64]
//
// Presence flags and every field outside brackets are mandatory. Greet and
// Welcome open with `wireVersion appVersion engineFingerprint`, a prefix every
// build reads alike, and a peer at another wire version or with another
// fingerprint is refused from it before the rest of its body is decoded.
//     Limits         := maxOutstandingCalls:u16 maxQueuedBulkBytes:u32
//                       maxPendingRanges:u16 probeScheduleMicros:[u32]
//     Facts          := viewportCells:(u16,u16) cellPixels:(u16,u16) textAreaPixels:(u16,u16)
//                       refreshIntervalMicros:u32 link:u8 power:u8 focused:u8
//     Ack            := hostTimeEcho:u64 holdMicros:u32 bytesReceived:u64
//                       panes:[(handle:varint, generation:u64, historyAcked:u64, effectsAcknowledged:u64)]
//     Probe          := time:u64, in the sender's clock
//     Echo           := timeEcho:u64 holdMicros:u32
//     PaneBind       := handle:varint paneID:u128
//     PaneUnbind     := handle:varint reason:u8
//     BulkChunk      := transfer:u32 index:u32 last:u8 bytes:bytes
//                       [sha256:[32] [compression:u8 {Deflate} inflatedBytes:u32
//                        when deflated] when last]
//     Bye            := reason:u8 detail:utf8 hasPeer:u8
//                       [wireVersion:u16 appVersion:utf8 platform:utf8]
//     CheckpointHead := generation:u64 transfer:u32 frameClass:u8 stateDigest:u128
//     Update         := mode:u8 baseGeneration:u64 generation:u64 frameClass:u8
//                       hostTime:u64 lagMicros:varint inputWritten:u64
//                       inputSettled:u64 historyHead:u64 effectsThrough:u64
//                       body (a `RowUpdate`, ScreenPlane.h, to the end)
//     Resync         := reason:u8 generation:u64 sectionHashes:[u64]
//     View           := state:u8 {Live, Dormant, RecordsOnly}
//     PaneDirty      := (empty)
//     BulkAbort      := transfer:u32
//     Call           := text (a JSON-RPC 2.0 request, UTF-8, to the end)
//     Result         := transfer:varint, then when transfer is 0 the text of a
//                       JSON-RPC 2.0 response (UTF-8, to the end); otherwise
//                       nothing, and that bulk transfer carries the text
//     Notify         := kind:varint (a `NotifyKind`) record (that kind's, to the end)
//     HistoryInline  := historyEpoch:u32 firstLine:u64
//                       body (a history body, Docs/sync.md §7.5, to the end)
//     HistoryHead    := transfer:u32 historyEpoch:u32 firstLine:u64 count:u16
//                       order:u8 {Ascending, Descending}
//     HistoryReset   := newEpoch:u32 clearedThrough:u64 historyHead:u64
//     HistoryWant    := historyEpoch:u32 ranges:[(from:u64, to:u64)]
//                       priority:u8 {Viewport, Search, Backfill}
//     Receipt        := inputWritten:u64 inputSettled:u64
//     Raw            := inputSeq:u64 bytes:bytes
//     Keys           := inputSeq:u64 events:[KeyEvent]
//     KeyEvent       := key:u32 modifiers:u16 kind:u8 {Press, Repeat, Release}
//                       text:utf8 unshifted:u32 shifted:u32 baseLayout:u32
//                       flags:u8 {Modified}
//     Pointer        := inputSeq:u64 events:[PointerEvent]
//     PointerEvent   := kind:u8 {Press, Release, Motion} button:u8 modifiers:u16
//                       cell:(row:u16, column:u16) pixel:(x:u16, y:u16)
//     Focus          := focused:u8
//
// Every decoder reads the whole body and refuses it unless it ends exactly
// where the body does; a refused body is a malformed frame, which the
// connection answers with `Bye{Malformed}`.
//
// `engineFingerprint` is the sender's `wireFingerprint` (Envelope.h): two
// builds whose body layouts differ carry different ones.
//

#include "Base/Bits/OptionSet.h"
#include "Core/GridBounds.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Sync/Admission.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/Roles.h"
#include "Core/RichText/TypographyFacts.h"
#include "Core/TermPalette.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// A 128-bit identity: an instance, an attachment, a pane or a secret.
struct ID128 {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    friend constexpr bool operator==(const ID128&, const ID128&) = default;
};

/// The digest of a build's body layouts, as `wireFingerprint` (Envelope.h).
using EngineFingerprint = std::array<std::uint8_t, 32>;

/// What kind of client attaches.
enum class ClientKind : std::uint8_t {
    Human,
    Agent,
    Viewer,
};

/// What the client's link costs it.
enum class LinkCost : std::uint8_t {
    Unmetered,
    Metered,
    Constrained,
};

/// The client's power state.
enum class PowerState : std::uint8_t {
    Normal,
    Saver,
};

/// Why a peer closes the connection.
enum class ByeReason : std::uint8_t {
    /// An orderly close.
    Normal,
    /// The peers speak different wire versions, or the same version with
    /// different fingerprints: builds whose body layouts differ (Envelope.h).
    VersionMismatch,
    /// A frame failed its grammar, its kind's direction or the handshake
    /// state.
    Malformed,
    /// A frame's length was above the cap.
    Oversize,
    /// A frame's kind byte names no kind.
    UnknownKind,
    /// A pane frame named a handle no `PaneBind` bound.
    UnboundHandle,
    /// A bulk transfer exceeded its cap or failed its SHA-256.
    TransferRefused,
    /// A resume of the attachment moved it to another connection.
    Superseded,
    /// A resume of the attachment is already being admitted on another
    /// connection.
    ResumeInProgress,
    /// The client is going to sleep: the attachment is `Lost` at once and
    /// keeps what a resume needs.
    Suspending,
    /// The host is replacing its daemon without ending its sessions.
    HandingOver,
    /// The host requires a token and the client presented none.
    TokenMissing,
    /// The host does not know the token.
    TokenInvalid,
    /// The token is past its expiry.
    TokenExpired,
    /// The token was revoked.
    TokenRevoked,
    /// The handshake or an operation is outside what the token, or the
    /// client's standing, allows.
    OutsideScope,
    /// A second handshake on a connection that has had one.
    DuplicateHandshake,
    /// Nobody who could consent is attached and the client has no standing
    /// grant.
    NoConsent,
    /// The remote host the client asked the session to reach is not one the
    /// host will hand to ssh.
    RemoteHostRefused,
    /// The session, workspace or host instance the client named is not here.
    SessionGone,
    /// The host has no capacity for another session or pane attachment.
    HostFull,
    /// The client read the host's output too slowly: what the host held for
    /// it passed the per-client mark, so the host dropped what was queued and
    /// closed. Not a refusal; the client reconnects, waiting longer each time
    /// it is dropped this way.
    OutputBacklog,
    /// The host heard nothing from the client for the silence bound while it
    /// awaited an answer, and dropped the connection as lost. The attachment
    /// keeps what a resume needs. Not a refusal.
    Silent,
};

/// The last reason a `Bye` frame carries.
inline constexpr ByeReason lastByeReasonOnWire = ByeReason::Silent;

/// Reports whether a reason is a host declining to serve the client: its
/// handshake, or an operation on an established connection.
///
/// - Parameter reason: Why the connection closed.
/// - Returns: Whether the host refused, as against a connection that ended
///   or failed.
[[nodiscard]] constexpr bool isRefusal(ByeReason reason)
{
    switch (reason) {
    case ByeReason::VersionMismatch:
    case ByeReason::TokenMissing:
    case ByeReason::TokenInvalid:
    case ByeReason::TokenExpired:
    case ByeReason::TokenRevoked:
    case ByeReason::OutsideScope:
    case ByeReason::DuplicateHandshake:
    case ByeReason::NoConsent:
    case ByeReason::RemoteHostRefused:
    case ByeReason::SessionGone:
    case ByeReason::HostFull:
        return true;
    case ByeReason::Normal:
    case ByeReason::Malformed:
    case ByeReason::Oversize:
    case ByeReason::UnknownKind:
    case ByeReason::UnboundHandle:
    case ByeReason::TransferRefused:
    case ByeReason::Superseded:
    case ByeReason::ResumeInProgress:
    case ByeReason::Suspending:
    case ByeReason::HandingOver:
    case ByeReason::OutputBacklog:
    case ByeReason::Silent:
        return false;
    }
    return false;
}

/// Reports whether a reason is the client breaking the protocol: a frame it
/// should not have sent, or input past its share. What such a sender queued
/// unwritten is dropped; a connection that ended, failed or was refused an
/// operation keeps the input already accepted from it, which a resume
/// continues.
///
/// - Parameter reason: Why the connection closed.
/// - Returns: Whether the client broke the protocol.
[[nodiscard]] constexpr bool isViolation(ByeReason reason)
{
    switch (reason) {
    case ByeReason::Malformed:
    case ByeReason::Oversize:
    case ByeReason::UnknownKind:
    case ByeReason::UnboundHandle:
    case ByeReason::TransferRefused:
    case ByeReason::DuplicateHandshake:
        return true;
    case ByeReason::Normal:
    case ByeReason::VersionMismatch:
    case ByeReason::Superseded:
    case ByeReason::ResumeInProgress:
    case ByeReason::Suspending:
    case ByeReason::HandingOver:
    case ByeReason::TokenMissing:
    case ByeReason::TokenInvalid:
    case ByeReason::TokenExpired:
    case ByeReason::TokenRevoked:
    case ByeReason::OutsideScope:
    case ByeReason::NoConsent:
    case ByeReason::RemoteHostRefused:
    case ByeReason::SessionGone:
    case ByeReason::HostFull:
    case ByeReason::OutputBacklog:
    case ByeReason::Silent:
        return false;
    }
    return false;
}

/// Answers whether repeating the same handshake can only be refused again.
///
/// A refusal is permanent when the client cannot change the outcome by trying
/// again: the token it holds is the token that was refused, the version it
/// speaks is the version that mismatched, the session it named does not come
/// back under that name. A client that retries a permanent refusal hammers
/// the host and shows the person a reconnect that never lands, so a permanent
/// refusal is a terminal state.
///
/// Three are not: a second handshake on one connection is a different
/// request on a fresh one, consent is refused because nobody was attached to
/// give it, and a full host has room once a session ends.
///
/// - Parameter reason: A refusal (`isRefusal`), or `Malformed` for a refusal
///   this build could not read.
/// - Returns: Whether retrying is futile.
[[nodiscard]] constexpr bool isPermanentRefusal(ByeReason reason)
{
    switch (reason) {
    case ByeReason::DuplicateHandshake:
    case ByeReason::NoConsent:
    case ByeReason::HostFull:
        return false;
    case ByeReason::VersionMismatch:
    case ByeReason::TokenMissing:
    case ByeReason::TokenInvalid:
    case ByeReason::TokenExpired:
    case ByeReason::TokenRevoked:
    case ByeReason::OutsideScope:
    case ByeReason::RemoteHostRefused:
    case ByeReason::SessionGone:
    // A refusal this build could not read is one it cannot satisfy.
    case ByeReason::Malformed:
        return true;
    case ByeReason::Normal:
    case ByeReason::Oversize:
    case ByeReason::UnknownKind:
    case ByeReason::UnboundHandle:
    case ByeReason::TransferRefused:
    case ByeReason::Superseded:
    case ByeReason::ResumeInProgress:
    case ByeReason::Suspending:
    case ByeReason::HandingOver:
    case ByeReason::OutputBacklog:
    case ByeReason::Silent:
        return false;
    }
    return true;
}

/// The first QUIC application error code that carries a `Bye` reason.
///
/// A host that cannot put a `Bye` on a QUIC connection's stream, because the
/// stream holds a backlog the `Bye` would wait behind, closes the connection
/// with an application CONNECTION_CLOSE instead, which no stream data delays.
/// Its error code is this base plus the reason's wire value, and its reason
/// phrase is the `Bye`'s detail (Docs/sync.md §7.3).
inline constexpr std::uint64_t quicByeCloseCodeBase = 0x5542'5900;

/// Returns the QUIC application error code that carries a `Bye` reason.
///
/// - Parameter reason: Why the host closes the connection.
/// - Returns: The CONNECTION_CLOSE error code.
[[nodiscard]] constexpr std::uint64_t quicCloseCodeOf(ByeReason reason)
{
    return quicByeCloseCodeBase + std::to_underlying(reason);
}

/// Reads the `Bye` reason a QUIC application CONNECTION_CLOSE carries.
///
/// - Parameter errorCode: The application error code the peer closed with.
/// - Returns: The reason, or nothing for a code that names none.
[[nodiscard]] constexpr std::optional<ByeReason> byeReasonOfQUICClose(std::uint64_t errorCode)
{
    if (errorCode < quicByeCloseCodeBase || errorCode - quicByeCloseCodeBase > std::to_underlying(lastByeReasonOnWire))
        return std::nullopt;
    return static_cast<ByeReason>(errorCode - quicByeCloseCodeBase);
}

/// Why a client asks for a checkpoint.
enum class ResyncReason : std::uint8_t {
    /// An update built a state whose digest differs from the one it carried.
    DigestMismatch,
    /// A chain update's base is not the generation the client holds.
    BaseMissing,
    /// An update or checkpoint body did not decode.
    DecodeFailed,
    /// A checkpoint failed to apply.
    CheckpointRefused,
    /// Updates buffered behind a checkpoint exceeded their cap.
    BufferOverflow,
};

/// Why the host took a frame (Docs/sync.md §6.1); the values are the wire's
/// `frameClass` byte. The coherence rule that classifies a frame is a runtime
/// concern, so its owner maps `Runtime::FrameClass` onto this one.
enum class FrameClass : std::uint8_t {
    /// A DEC 2026 block closed.
    Boundary = 0,
    /// Every condition of the rule held.
    Settled = 1,
    /// A writer that never went quiet, paced.
    Flood = 2,
    /// The deadline fired.
    Forced = 3,
    /// A resize, a reset or a checkpoint.
    Structural = 4,
};

/// Whether a log mode may start from a checkpoint of a class: one taken with
/// the parser at ground, which only a closed block and a settled writer
/// promise. A flood, the deadline and a resize take a frame in whatever
/// state the parser is in, and the parser's pending state never travels
/// (Docs/sync.md §11).
///
/// - Parameter frameClass: The checkpoint's class.
/// - Returns: Whether a parser may continue from it.
[[nodiscard]] constexpr bool canStartLogMode(FrameClass frameClass) { return frameClass == FrameClass::Boundary || frameClass == FrameClass::Settled; }

/// How an update's base relates to the client's state (Docs/sync.md §6.3).
enum class UpdateMode : std::uint8_t {
    /// Applies onto exactly `baseGeneration`; rides the reliable stream.
    Chain,
    /// Applies onto any generation from `baseGeneration` up to, not
    /// including, `generation`, so a lost or reordered one costs only the
    /// next.
    Superset,
};

/// The colours a client draws a pane in: the live palette and the baseline
/// a reset returns to, as a theme sets them. The geometry owner's answer the
/// program's colour queries (Docs/sync.md §6.11).
struct ThemePalette {
    Ubiquitty::TermPalette live;
    Ubiquitty::TermPalette baseline;

    friend bool operator==(const ThemePalette&, const ThemePalette&) = default;
};

/// What the client reports about itself.
struct Facts {
    std::uint16_t viewportRows = 0;
    std::uint16_t viewportColumns = 0;
    std::uint16_t cellPixelWidth = 0;
    std::uint16_t cellPixelHeight = 0;
    std::uint16_t textAreaPixelWidth = 0;
    std::uint16_t textAreaPixelHeight = 0;
    /// The display's refresh interval; the host sends no more than one update
    /// per interval.
    std::uint32_t refreshIntervalMicroseconds = 0;
    LinkCost link = LinkCost::Unmetered;
    PowerState power = PowerState::Normal;
    bool isFocused = false;
    /// The client's theme, sent in a `Facts` message of its own, never in the
    /// `Greet`; absent when the client sets none.
    std::optional<ThemePalette> themePalette {};

    friend bool operator==(const Facts&, const Facts&) = default;
};

/// What a client can take past what every client takes, one bit each in
/// `Greet::capabilities` (Docs/sync.md §7.3). A host refuses a Greet naming a
/// bit it does not know; a peer of its fingerprint never names one.
enum class ClientCapability : std::uint32_t {
    /// A checkpoint body may cross deflated.
    DeflateCheckpoints = 1u << 0,
    /// A chain update may say a row by its changed spans (`UpdateOp::Spans`).
    SpanOps = 1u << 1,
    /// A chain update may move rows with `UpdateOp::ScrollRect`.
    ScrollRectOps = 1u << 2,
    /// A chain update may carry `UpdateOp::EchoContext`.
    EchoContext = 1u << 3,
    /// The frontend serves authenticated native drag commands.
    NativeDrag = 1u << 4,
    /// The client asks its own person about a clipboard write or notification
    /// a host on another machine delivers to it, so that host forwards one
    /// without asking a question of its own.
    AsksAboutHostEffects = 1u << 5,
    /// The client shows a person none of the host's consent questions, so a
    /// host asks it none: a question waits only on recipients that can answer.
    ShowsNoQuestions = 1u << 6,
    /// Image pixels cross as a zlib stream of the decoded bytes.
    DeflateImages = 1u << 7,
    /// The client's renderer draws proportional runs and publishes its faces
    /// for hosted measure answers.
    PublishesFaces = 1u << 8,
};

/// Every bit a client may name.
///
/// The wire fingerprint covers them, so two builds that differ in them meet
/// as different versions rather than one ignoring what the other asks for.
inline constexpr std::uint32_t knownClientCapabilityBits = std::to_underlying(ClientCapability::DeflateCheckpoints)
    | std::to_underlying(ClientCapability::SpanOps) | std::to_underlying(ClientCapability::ScrollRectOps)
    | std::to_underlying(ClientCapability::EchoContext) | std::to_underlying(ClientCapability::NativeDrag)
    | std::to_underlying(ClientCapability::AsksAboutHostEffects) | std::to_underlying(ClientCapability::ShowsNoQuestions)
    | std::to_underlying(ClientCapability::DeflateImages) | std::to_underlying(ClientCapability::PublishesFaces);

/// A client's capabilities.
using ClientCapabilities = Ubiquitty::Bits::OptionSet<ClientCapability>;

/// What a client wants of a pane (Docs/sync.md §6.8). The host enforces it.
enum class ViewState : std::uint8_t {
    /// Updates, records and effects: a pane on screen.
    Live,
    /// Records and effects, and one `PaneDirty` the first time the pane
    /// changes: a background tab, an occluded window, a backgrounded app.
    Dormant,
    /// Records and effects alone: an agent, which reads the screen through a
    /// call.
    RecordsOnly,
};

/// A run of history line numbers, half-open: `[from, to)`, never empty.
struct LineRange {
    std::uint64_t from = 0;
    std::uint64_t to = 0;

    friend constexpr bool operator==(const LineRange&, const LineRange&) = default;
};

/// The most cached history ranges one pane names in a resume.
inline constexpr std::size_t maxResumeCachedRanges = 64;

/// How the host continued one pane of a resumed attachment.
enum class ResumeOutcome : std::uint8_t {
    /// From the baseline the client holds: an update, no checkpoint.
    Diffed,
    /// The client's picture matched no baseline the host kept: a checkpoint.
    Checkpointed,
    /// The pane is gone.
    Gone,
};

/// What a resuming client holds of one pane (Docs/sync.md §6.9).
struct PaneResume {
    ID128 paneID {};
    /// The newest generation the client acknowledged.
    std::uint64_t ackedGeneration = 0;
    /// The digest of the row hashes and state blocks the client holds for
    /// that generation.
    Digest128 ackedRowHashesDigest {};
    std::uint32_t historyEpoch = 0;
    /// The history lines it holds, ascending and disjoint.
    std::vector<LineRange> cachedRanges {};
    /// The newest input it numbered for the pane.
    std::uint64_t lastInputSeqSent = 0;
    ViewState view = ViewState::Live;
    /// The last complete effect batch retained by this stable pane's reader.
    std::uint64_t effectsAcknowledged = 0;

    friend bool operator==(const PaneResume&, const PaneResume&) = default;
};

/// A `Greet`'s claim to continue an attachment.
struct ResumeRequest {
    ID128 attachmentID;
    /// Absent on a QUIC connection the resume proof already admitted.
    std::optional<ID128> resumeSecret;
    std::vector<PaneResume> panes;

    friend bool operator==(const ResumeRequest&, const ResumeRequest&) = default;
};

/// How the host continued one pane, as `Welcome` reports it.
struct PaneResumed {
    ID128 paneID {};
    ResumeOutcome outcome = ResumeOutcome::Gone;
    /// The newest of the client's input the host wrote, absent when it
    /// remembers none: never 0 for that (Docs/sync.md §6.7).
    std::optional<std::uint64_t> inputWritten {};

    friend bool operator==(const PaneResumed&, const PaneResumed&) = default;
};

/// The client's opening frame. Its wire version opens it, so a host reads the
/// version of a client whatever else changed.
struct Greet {
    std::uint16_t wireVersion = 0;
    std::string appVersion;
    /// This build's unless a test states another: no other value is one a
    /// host of this build admits.
    EngineFingerprint engineFingerprint = wireFingerprint;
    ID128 clientInstance;
    ClientKind clientKind = ClientKind::Human;
    /// The most the client asks to be; the host grants this or less.
    Role requestedRole = Role::Owner;
    AdmissionRequest request;
    /// Supplied for this physical greeting, never persisted in a resume record.
    std::optional<std::vector<std::uint8_t>> credential;
    Facts facts;
    /// The most lines of each pane's history the client keeps. A client that
    /// keeps none is sent no history but `HistoryReset` (Docs/sync.md §6.6).
    std::uint32_t historyCacheLines = 0;
    /// How far below the head an unmetered attach is backfilled, which the
    /// client chooses; never deeper than it keeps.
    std::uint32_t backfillDepthLines = 0;
    /// What the client can take past what every client takes. One with none,
    /// as an editor's, is sent every body as it is and every row whole.
    ClientCapabilities capabilities;
    /// The attachment the client asks to continue, absent for a fresh one.
    std::optional<ResumeRequest> resume;
};

/// The most steps an idle `Probe` schedule names.
inline constexpr std::size_t maxProbeSteps = 8;

/// The caps a client can reach, which the host advertises in `Welcome`
/// (Docs/sync.md §7.3, §7.9). Over one the host answers busy and keeps the
/// connection.
struct Limits {
    /// The calls the host holds answers for at once; one past it is answered
    /// busy.
    std::uint16_t maxOutstandingCalls = 0;
    /// The bytes the host holds queued for the attachment; a call whose
    /// answer would pass it is answered busy.
    std::uint32_t maxQueuedBulkBytes = 0;
    /// The history ranges a client may have pending, summed over every
    /// priority.
    std::uint16_t maxPendingRanges = 0;
    /// The host's idle `Probe` schedule (Docs/sync.md §6.4): the intervals
    /// after screen data stops, the last repeating. From it a client knows how
    /// long a quiet host may be silent.
    std::array<std::uint32_t, maxProbeSteps> probeScheduleMicroseconds {};
    /// How many of `probeScheduleMicroseconds` the schedule uses; 0 when the
    /// host does not probe an idle connection.
    std::uint8_t probeSteps = 0;

    friend constexpr bool operator==(const Limits&, const Limits&) = default;
};

/// The most a host takes of a paste, per piece and in all.
struct PasteLimits {
    std::uint32_t chunkBytes = 0;
    std::uint32_t pasteBytes = 0;
    /// The most of a paste a client may have sent beyond what the host
    /// reported written (`PasteReceipt`); 0 from a host that states none,
    /// which a client sends a paste to without waiting.
    std::uint32_t windowBytes = 0;

    friend bool operator==(const PasteLimits&, const PasteLimits&) = default;
};

/// Whether an attachment has received the person's required consent.
enum class AttachmentAdmission : std::uint8_t {
    AwaitingConsent = 0,
    Granted = 1,
};

/// The exact pending question and session an unbound Welcome names.
struct ConsentBinding {
    std::uint32_t requestID = 0;
    std::uint32_t sessionID = 0;

    friend bool operator==(const ConsentBinding&, const ConsentBinding&) = default;
};

/// The host's answer to a `Greet`, including its exact admission boundary.
struct Welcome {
    std::uint16_t wireVersion = 0;
    std::string appVersion;
    /// This build's unless a test states another: no other value is one a
    /// client of this build accepts.
    EngineFingerprint engineFingerprint = wireFingerprint;
    ID128 hostInstanceID;
    ID128 attachmentID;
    ID128 resumeSecret;
    /// What the host lets this attachment be.
    Role grantedRole = Role::Owner;
    ServingSelection selection;
    /// A pending Welcome grants no pane, resume or operation authority.
    AttachmentAdmission admission = AttachmentAdmission::Granted;
    /// Nonzero exactly when admission is AwaitingConsent; otherwise both zero.
    ConsentBinding consent {};
    std::string principal;
    Limits limits;
    std::uint64_t hostTime = 0;
    /// The host instance whose attachments this one took over, absent unless
    /// a handover made it.
    std::optional<ID128> predecessorHostInstanceID;
    /// How each pane of a resume was continued, empty for a fresh attachment.
    std::vector<PaneResumed> resumed;
    /// The host's paste caps, after the resume's fields; absent from a host
    /// that takes no paste.
    std::optional<PasteLimits> pasteLimits;
};

/// Whether a welcomed daemon continues the held instance's sessions.
enum class InstanceContinuity : std::uint8_t {
    Same,
    HandedOver,
    Restarted,
};

/// Classifies an authenticated host identity without treating a restart as a resume.
///
/// - Parameters:
///   - held: The instance whose sessions the client holds.
///   - welcomed: The instance that accepted this connection.
///   - predecessor: The issuer the host says it inherited for this attachment.
/// - Returns: Continuity only for the same instance or an exact predecessor.
[[nodiscard]] constexpr InstanceContinuity continuityOf(ID128 held, ID128 welcomed, std::optional<ID128> predecessor)
{
    if (held == welcomed)
        return InstanceContinuity::Same;
    return held != ID128 {} && predecessor == held ? InstanceContinuity::HandedOver : InstanceContinuity::Restarted;
}

/// One pane's acknowledged generation.
struct PaneAck {
    std::uint32_t handle = 0;
    std::uint64_t generation = 0;
    /// The end of the newest run of history lines the client holds: every
    /// append below it arrived (Docs/sync.md §7.3).
    std::uint64_t historyAcked = 0;
    /// The last complete effect batch, independent of the screen generation.
    std::uint64_t effectsAcknowledged = 0;
};

/// The client's acknowledgement of what it has received and applied.
struct Ack {
    /// The `hostTime` of the newest frame that carried one.
    std::uint64_t hostTimeEcho = 0;
    /// How long the client held that frame before this `Ack` left.
    std::uint32_t holdMicroseconds = 0;
    /// Every stream byte the client has received on this connection.
    std::uint64_t bytesReceived = 0;
    std::vector<PaneAck> panes {};
};

/// The host's request for a round-trip sample while no screen data flows.
struct Probe {
    std::uint64_t time = 0;
};

/// The client's answer to a `Probe`.
struct Echo {
    std::uint64_t timeEcho = 0;
    std::uint32_t holdMicroseconds = 0;
};

/// Binds a pane handle on this connection to a pane.
struct PaneBind {
    std::uint32_t handle = 0;
    ID128 paneID;
};

/// Why the host ended a binding, as `PaneUnbind.reason` carries it. A client
/// reads a value it does not know as `Ended`.
enum class PaneUnbindReason : std::uint8_t {
    /// The binding ended: the pane closed, moved, or the client let it go.
    Ended = 0,
    /// The pane's checkpoint is larger than the host's checkpoint cap
    /// (`HostPolicy::maxCheckpointBytes`), so the host cannot bring an
    /// attachment up; the client tells the person why the pane went.
    CheckpointTooLarge = 1,
};

/// Releases a pane handle.
struct PaneUnbind {
    std::uint32_t handle = 0;
    std::uint8_t reason = 0;
};

/// One chunk of a bulk transfer.
struct BulkChunk {
    std::uint32_t transfer = 0;
    std::uint32_t index = 0;
    bool isLast = false;
    std::span<const std::uint8_t> bytes;
    /// The SHA-256 of the reassembled payload as it crossed the wire; carried
    /// by the last chunk only.
    std::array<std::uint8_t, 32> sha256 {};
    /// How the reassembled payload is coded; carried by the last chunk only.
    Compression compression = Compression::None;
    /// The payload's length once inflated, for `Deflate`.
    std::uint32_t inflatedBytes = 0;
};

/// A peer's closing frame.
struct Bye {
    ByeReason reason = ByeReason::Normal;
    std::string detail {};
    std::optional<PeerMetadata> peer {};
};

/// Announces a checkpoint whose body follows as a bulk transfer.
struct CheckpointHead {
    std::uint64_t generation = 0;
    std::uint32_t transfer = 0;
    FrameClass frameClass = FrameClass::Structural;
    Digest128 stateDigest;
};

/// The client's view of the pane the frame's channel names.
struct View {
    ViewState state = ViewState::Live;
};

/// A transfer the host abandoned after announcing it.
struct BulkAbort {
    std::uint32_t transfer = 0;
};

/// A JSON-RPC 2.0 request.
struct Call {
    std::string text;
};

/// The most bytes of a workspace's structure record a connection carries.
/// The record's own bound, checked where it is written and read
/// (Source/Core/Session/SessionTreeWire.h), is asserted to fit.
inline constexpr std::size_t maxWorkspaceStructureRecordBytes = std::size_t { 1 } << 20;

/// How often a host sends a listed pane's records at most, and how often it
/// reads them for a pane only a listing reads: the listing's cadence. A
/// listing shows a title or a status within a second of its change, and a
/// pane that writes without pause costs a listing one read a second.
inline constexpr std::uint64_t listedRecordsIntervalMicroseconds = 1'000'000;

/// The most workspaces whose structures a client holds, and the most bytes
/// they take together. A host runs at most a pane a workspace and lists as
/// many again it could restore; these are several times that.
inline constexpr std::size_t maxHeldStructures = 512;
inline constexpr std::size_t maxHeldStructureBytes = std::size_t { 32 } << 20;

/// The first handle a listed pane takes. Bound panes count up from one and a
/// connection binds at most `maxPanesPerConnection`, so no handle names both.
inline constexpr std::uint32_t firstListedHandle = 0x4000'0000;

/// A workspace's structure, or the news that the workspace is gone.
struct WorkspaceStructureHead {
    std::uint32_t workspaceID = 0;
    /// The structure's revision; a client keeps the newest it was sent.
    std::uint64_t revision = 0;
    /// The transfer carrying the record, or 0 when `record` holds it or the
    /// workspace is gone. Transfer ids start at 1.
    std::uint32_t transfer = 0;
    /// Whether the host no longer holds the workspace. Nothing follows.
    bool isGone = false; // bool-ok: names the one state it reports
    /// The record, empty when `transfer` carries it or the workspace is gone.
    std::vector<std::uint8_t> record {};
};

/// The answer to a `Call`: inline, or announcing the bulk transfer that
/// carries it when it is larger than one frame.
struct Result {
    /// The transfer carrying the text, or 0 when `text` holds it. Transfer
    /// ids start at 1.
    std::uint32_t transfer = 0;
    /// The JSON-RPC 2.0 response, empty when `transfer` carries it.
    std::string text;
};

/// Something the host tells a client unasked: a kind this build knows and that
/// kind's record. Nothing answers it.
struct Notify {
    NotifyKind kind = NotifyKind::Attachments;
    /// The record, everything after the leading varint.
    std::vector<std::uint8_t> record;
};

/// A `Notify` body split at its kind, whatever the kind. A reader that does
/// not know the kind skips the frame by its length.
struct NotifyHead {
    std::uint64_t kind = 0;
    /// The record, borrowing the decoded body.
    std::span<const std::uint8_t> record;
};

/// Which want a history range answers (Docs/sync.md §6.6). A want replaces
/// the client's set at its priority, and the bulk lane serves the priorities
/// in this order.
enum class HistoryPriority : std::uint8_t {
    /// The lines the user is looking at.
    Viewport,
    /// The lines a search result names.
    Search,
    /// History the client keeps ahead of the user, as the link allows.
    Backfill,
};

/// The order a history body lists its lines in.
enum class HistoryOrder : std::uint8_t {
    /// From `firstLine` up.
    Ascending,
    /// From `firstLine` down: backfill, newest first.
    Descending,
};

/// The fields ahead of a `HistoryInline`'s history body.
struct HistoryInlineHeader {
    std::uint32_t historyEpoch = 0;
    std::uint64_t firstLine = 0;
};

/// A `HistoryInline`'s header and where its history body starts.
struct DecodedHistoryInlineHeader {
    HistoryInlineHeader header;
    std::size_t bytes = 0;
};

/// Announces `count` history lines whose body follows as a bulk transfer.
struct HistoryHead {
    std::uint32_t transfer = 0;
    std::uint32_t historyEpoch = 0;
    std::uint64_t firstLine = 0;
    /// At least one.
    std::uint16_t count = 0;
    HistoryOrder order = HistoryOrder::Ascending;
};

/// The largest payload one `HistoryHead` announces (Docs/sync.md §7.9); a
/// receiver refuses the chunk that would take a history transfer past it.
inline constexpr std::size_t maxHistoryTransferBytes = std::size_t { 4 } << 20;

/// A destructive clear opened `newEpoch`. Every line below `clearedThrough`
/// is gone, and the lines of the new epoch run from it to `historyHead`.
struct HistoryReset {
    std::uint32_t newEpoch = 0;
    std::uint64_t clearedThrough = 0;
    std::uint64_t historyHead = 0;
};

/// The client's wanted ranges at one priority, replacing its earlier want at
/// that priority. The ranges are sorted and disjoint; none may be empty.
struct HistoryWant {
    std::uint32_t historyEpoch = 0;
    std::vector<LineRange> ranges;
    HistoryPriority priority = HistoryPriority::Viewport;
};

/// The most ranges one `HistoryWant` names, refused before any range is
/// read. The cap on ranges pending over every priority is the host's policy,
/// which `Limits.maxPendingRanges` advertises (Docs/sync.md §7.9).
inline constexpr std::size_t maxHistoryWantRanges = 64;

/// The fields ahead of an update's row-update body.
struct UpdateHeader {
    UpdateMode mode = UpdateMode::Chain;
    std::uint64_t baseGeneration = 0;
    std::uint64_t generation = 0;
    FrameClass frameClass = FrameClass::Settled;
    std::uint64_t hostTime = 0;
    /// `hostTime` less the host time of the first change in
    /// `baseGeneration + 1`: the age of the oldest change this update
    /// delivers (Docs/sync.md §7.4).
    std::uint64_t lagMicroseconds = 0;
    /// The client's newest input written to the program, by `inputSeq`; 0
    /// when none has been written.
    std::uint64_t inputWritten = 0;
    /// The client's newest input whose effect this frame shows; 0 when none
    /// has settled.
    std::uint64_t inputSettled = 0;
    /// The number the host's next history line takes when the update was cut;
    /// the lines below it precede the update on the stream (Docs/sync.md §6.6).
    std::uint64_t historyHead = 0;
    /// The newest generation whose effects the host has sent on the pane's
    /// channel; always 0, because effects do not travel as frames of their
    /// own.
    std::uint64_t effectsThrough = 0;
};

/// An update's header and where its row-update body starts.
struct DecodedUpdateHeader {
    UpdateHeader header;
    /// The bytes the header took, which the lag's varint makes vary.
    std::size_t bytes = 0;
};

/// The client's request for a checkpoint.
struct Resync {
    ResyncReason reason = ResyncReason::DigestMismatch;
    /// The generation `sectionHashes` describe: for `DigestMismatch` the
    /// refused update's target, otherwise the generation the client holds.
    std::uint64_t generation = 0;
    /// The active screen's row hashes at `generation` as the client built
    /// them, so the host can name the first row that diverged.
    std::vector<std::uint64_t> sectionHashes;
};

/// The host's report of how far a client's input has been written.
struct Receipt {
    std::uint64_t inputWritten = 0;
    std::uint64_t inputSettled = 0;
};

/// Input a client sends as bytes, numbered in its order for the pane.
struct Raw {
    /// From 1, per pane, per client instance.
    std::uint64_t inputSeq = 0;
    std::span<const std::uint8_t> bytes;
};

/// The most events one `Keys` or `Pointer` carries.
inline constexpr std::size_t maxInputEventsPerFrame = 64;

/// The most text one `Paste` carries (Docs/sync.md §7.9).
inline constexpr std::size_t maxPasteChunkBytes = std::size_t { 32 } << 10;

/// The most text one paste carries across its pieces (Docs/sync.md §7.9).
inline constexpr std::uint64_t maxPasteBytes = std::uint64_t { 16 } << 20;

/// Whether a `Paste` is the paste's last piece.
enum class PasteState : std::uint8_t {
    More,
    Final,
};

/// A piece of a paste, on the bulk lane: one event numbered once, its text
/// in order across pieces (Docs/sync.md §6.7).
struct Paste {
    std::uint64_t inputSeq = 0;
    /// The paste's bytes before this piece. A resend starts again at 0, and
    /// the host takes only what it has not.
    std::uint64_t offset = 0;
    PasteState state = PasteState::More;
    /// UTF-8, whole scalars.
    std::span<const std::uint8_t> text;
};

/// The client gives up a paste it began: the host drops what of it is still
/// to come and closes the bracket it opened.
struct PasteAbort {
    std::uint64_t inputSeq = 0;
};

/// The most bytes a key event's text carries: eight code points, what one key
/// event of the terminal holds, at four bytes each.
inline constexpr std::size_t maxKeyEventTextBytes = 32;

/// A key's transition.
enum class KeyEventKind : std::uint8_t {
    Press,
    Repeat,
    Release,
};

/// One key, as the client's platform reported it: the facts the host's key
/// encoder reads under the modes live when it writes (Docs/sync.md §6.7).
struct KeyEvent {
    /// The key, by `Keys::VirtualKey`.
    std::uint32_t key = 0;
    /// Low byte: key-encoder flags. High byte: DECEKBD sided modifier state.
    std::uint16_t modifiers = 0;
    KeyEventKind kind = KeyEventKind::Press;
    /// What the key produced, UTF-8.
    std::string text;
    /// The layout's code point with every modifier removed.
    std::uint32_t unshiftedCodePoint = 0;
    /// The code point with Shift applied, when it differs.
    std::uint32_t shiftedCodePoint = 0;
    /// The standard layout's code point at the key's position, or 0.
    std::uint32_t baseLayoutCodePoint = 0;
    /// Whether any modifier was held, whether or not it is in `modifiers`.
    bool wasModified = false;

    friend bool operator==(const KeyEvent&, const KeyEvent&) = default;
};

/// Keys a client typed into a pane, numbered in its order for the pane.
struct Keys {
    std::uint64_t inputSeq = 0;
    std::vector<KeyEvent> events;
};

/// What a pointer did.
enum class PointerEventKind : std::uint8_t {
    Press,
    Release,
    Motion,
};

/// The pixel coordinate a pointer event carries when it has none.
inline constexpr std::uint16_t noPointerPixel = 0xFFFF;

/// One pointer event the client's pointer ladder routed to the program.
struct PointerEvent {
    PointerEventKind kind = PointerEventKind::Motion;
    /// xterm's button code: 0 to 2 the buttons, 3 none, 64 and up the wheel.
    std::uint8_t button = 0;
    /// xterm's report modifier bits.
    std::uint16_t modifiers = 0;
    std::uint16_t row = 0;
    std::uint16_t column = 0;
    /// The device pixel, or `noPointerPixel` when the client has none.
    std::uint16_t pixelX = noPointerPixel;
    std::uint16_t pixelY = noPointerPixel;

    friend bool operator==(const PointerEvent&, const PointerEvent&) = default;
};

/// Pointer events a client routed to a pane's program, numbered in its order.
struct Pointer {
    std::uint64_t inputSeq = 0;
    std::vector<PointerEvent> events;
};

/// Whether a client's view of a pane holds the keyboard focus.
struct Focus {
    bool isFocused = false;
};

/// The size a driver shows a pane at: its viewport of that pane. The pixel
/// sizes are zero where the client has measured none.
struct Viewport {
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    std::uint16_t cellPixelWidth = 0;
    std::uint16_t cellPixelHeight = 0;
    std::uint16_t textAreaPixelWidth = 0;
    std::uint16_t textAreaPixelHeight = 0;
    std::uint16_t drawablePixelWidth = 0;
    std::uint16_t drawablePixelHeight = 0;

    friend bool operator==(const Viewport&, const Viewport&) = default;
};

/// The most section hashes a `Resync` carries: one per row of the tallest grid.
inline constexpr std::size_t maxSectionHashes = Ubiquitty::maxRows;

/// The most panes one connection holds bound at once (Docs/sync.md §7.9).
/// Each bound pane is a terminal the client keeps, so a host past the cap is
/// refused rather than allowed to grow the client without bound.
inline constexpr std::size_t maxPanesPerConnection = 256;

/// Appends the body of a `Greet`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Greet& message);

/// Appends the tail a resuming `Greet` ends with: `hasResume` set, then the
/// request. The tail is the Greet's last field, so a client that writes the
/// rest of its Greet itself ends it with these bytes in place of `hasResume`
/// clear.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - resume: The request; nothing is appended when it names more panes than
///     a connection binds or a view the wire has no value for.
void encodeGreetResume(std::vector<std::uint8_t>& output, const ResumeRequest& resume);
/// Appends an image asset announcement.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The announcement to encode.
void encode(std::vector<std::uint8_t>& output, const ImageHead& message);
/// Appends a missing-asset request.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The request to encode.
void encode(std::vector<std::uint8_t>& output, const ImageWant& message);
/// Appends a whole placement record that satisfies `isValid`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The record to encode.
void encode(std::vector<std::uint8_t>& output, const ImagePlacements& message);
/// Appends the body of a `Welcome`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Welcome& message);
/// Appends the body of a `Facts`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
/// The palette presence flag is required even when no palette is present.
/// Greet uses its own embedded base-facts layout, not this standalone body.
void encode(std::vector<std::uint8_t>& output, const Facts& message);
/// Appends the body of a `TypographyFacts`:
///
///     rendererRevision:u64 cellWidth:u16 cellHeight:u16 baseline:u16
///     proportionalSize:u16 monospaceSize:u16 rendersBlocks:u8 shaping:u8
///     fontCatalogScope:u8 faces:u8 Face×faces
///     Face := name:utf8 class:u8 styles:u8 ascent:u16 descent:u16
///             lineHeight:u16 averageAdvance:u16 advances:u16×560
///
/// At most `RichText::maxFaces` faces, so the body stays under a frame's.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The table.
void encode(std::vector<std::uint8_t>& output, const RichText::TypographyFacts& message);
/// Appends the body of a `ProportionalFaces`:
///
///     cellAdvance:u32 untabledAdvances:u16×4
///     advances:u16×(4×1072) weightedAdvances:u8×(4×1072)
///
/// Every value in 1/64 of the client measurer's pixel, the faces in
/// `Rendering::FontFace` order for U+0020–U+024F, U+0300–U+036F,
/// U+0370–U+03FF and U+0400–U+04FF. Weight zero is exact in 1/64 pixel;
/// weights one through seven share the widest advance, rounded up in
/// sixteenths of a cell. The fixed body fits one frame.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The faces.
void encode(std::vector<std::uint8_t>& output, const OSC::TextSizing::ProportionalFaceFacts& message);
/// Appends the body of an `Ack`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Ack& message);
/// Appends the body of a `Probe`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Probe& message);
/// Appends the body of an `Echo`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Echo& message);
/// Appends the body of a `PaneBind`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const PaneBind& message);
/// Appends the body of a `PaneUnbind`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const PaneUnbind& message);
/// Appends the body of a `BulkChunk`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const BulkChunk& message);
/// Appends the body of a `Bye`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Bye& message);
/// Appends the body of a `CheckpointHead`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const CheckpointHead& message);
/// Appends the body of an `UpdateHeader`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const UpdateHeader& message);
/// Appends the body of a `Resync`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Resync& message);
/// Appends the body of a `View`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const View& message);
/// Appends the body of a `BulkAbort`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const BulkAbort& message);
/// Appends the body of a `Call`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Call& message);
/// Appends the body of a `Result`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode; `text` is ignored when `transfer` is
///     not 0.
void encode(std::vector<std::uint8_t>& output, const Result& message);
/// Appends the body of a `Notify`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Notify& message);
/// Appends the body of a `Receipt`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Receipt& message);
/// Appends the body of a `Raw`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Raw& message);

/// Appends the body of a `Paste`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Paste& message);

/// How much of an unfinished paste the host has written to the program
/// (Docs/sync.md §6.7). A paste's end is reported by `inputWritten`, as any
/// event's is.
struct PasteReceipt {
    std::uint64_t inputSeq = 0;
    /// The paste's bytes written, from its start.
    std::uint64_t bytesWritten = 0;

    friend bool operator==(const PasteReceipt&, const PasteReceipt&) = default;
};

/// Appends the body of a `PasteReceipt`.
///
/// - Parameters:
///   - output: The buffer to append to.
///   - message: The message.
void encode(std::vector<std::uint8_t>& output, const PasteReceipt& message);

/// Reads a `PasteReceipt`.
///
/// - Parameter body: The frame's body.
/// - Returns: The message, or `nullopt` for a body that is not one.
[[nodiscard]] std::optional<PasteReceipt> decodePasteReceipt(std::span<const std::uint8_t> body);

/// A run of the client's events a pane dropped unwritten (Docs/sync.md
/// §6.7): every event numbered above `inputWritten` and at or below
/// `throughSeq`. A signal character discarded them with the input the
/// terminal held unread. It travels ahead of any count that passes them, so
/// `inputWritten` never tells the client the program was written them.
struct InputDiscarded {
    /// The client's newest event written, or dropped, before the run.
    std::uint64_t inputWritten = 0;
    /// The newest event of the run.
    std::uint64_t throughSeq = 0;

    friend bool operator==(const InputDiscarded&, const InputDiscarded&) = default;
};

/// Appends the body of an `InputDiscarded`.
///
/// - Parameters:
///   - output: The buffer to append to.
///   - message: The message.
void encode(std::vector<std::uint8_t>& output, const InputDiscarded& message);

/// Reads an `InputDiscarded`.
///
/// - Parameter body: The frame's body.
/// - Returns: The message, or `nullopt` for a body that is not one, or one
///   whose run is empty.
[[nodiscard]] std::optional<InputDiscarded> decodeInputDiscarded(std::span<const std::uint8_t> body);

/// Appends the body of a `PasteAbort`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const PasteAbort& message);
/// Appends the body of a `Keys`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Keys& message);
/// Appends the body of a `Pointer`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Pointer& message);
/// Appends the body of a `Focus`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const Focus& message);

/// Appends the body of a `Viewport`.
///
/// - Parameters:
///   - output: The buffer the body is appended to.
///   - message: The viewport.
void encode(std::vector<std::uint8_t>& output, const Viewport& message);
/// Appends a `HistoryInline`'s header; its history body follows.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const HistoryInlineHeader& message);
/// Appends the body of a `HistoryHead`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const HistoryHead& message);
/// Appends the body of a `HistoryReset`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const HistoryReset& message);
/// Appends the body of a `HistoryWant`.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - message: The message to encode.
void encode(std::vector<std::uint8_t>& output, const HistoryWant& message);

/// Decodes the body of a `Greet`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Greet> decodeGreet(std::span<const std::uint8_t> body);

/// Reads only the wire version that opens a handshake body.
/// - Parameter body: The handshake body, possibly otherwise malformed.
/// - Returns: The stated version, or no value when it is incomplete or zero.
[[nodiscard]] std::optional<std::uint16_t> decodeHandshakeVersion(std::span<const std::uint8_t> body);

/// Reads the bounded build prefix; the complete body is validated separately.
/// - Parameter body: A Greet or Welcome body.
/// - Returns: The stated build when its required prefix is complete.
[[nodiscard]] std::optional<PeerBuild> decodeHandshakeBuild(std::span<const std::uint8_t> body);

/// Reads the fingerprint the build prefix ends with; the complete body is
/// validated separately.
/// - Parameter body: A Greet or Welcome body.
/// - Returns: The stated fingerprint when the prefix through it is complete.
[[nodiscard]] std::optional<EngineFingerprint> decodeHandshakeFingerprint(std::span<const std::uint8_t> body);

/// Validates a serving answer before attachment state can be created.
/// - Parameter selection: The authenticated host's routing and diagnostic values.
/// - Returns: Whether its target, IDs, basename and metadata are well formed.
[[nodiscard]] bool isValidSelection(const ServingSelection& selection);
/// Decodes the body of a `Welcome`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Welcome> decodeWelcome(std::span<const std::uint8_t> body);
/// Decodes the body of a `Facts`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Facts> decodeFacts(std::span<const std::uint8_t> body);
/// Decodes the body of a `TypographyFacts`, refusing one the grammar does not
/// consume exactly or a table that is not well formed.
[[nodiscard]] std::optional<RichText::TypographyFacts> decodeTypographyFacts(std::span<const std::uint8_t> body);
/// Decodes the body of a `ProportionalFaces`, refusing one the grammar does
/// not consume exactly or faces a host cannot count in.
[[nodiscard]] std::optional<OSC::TextSizing::ProportionalFaceFacts> decodeProportionalFaces(std::span<const std::uint8_t> body);
/// Decodes the body of a `Ack`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Ack> decodeAck(std::span<const std::uint8_t> body);
/// Decodes the body of a `Probe`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Probe> decodeProbe(std::span<const std::uint8_t> body);
/// Decodes the body of a `Echo`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Echo> decodeEcho(std::span<const std::uint8_t> body);
/// Appends the body of a `WorkspaceStructure`.
///
/// - Parameters:
///   - output: The buffer to extend.
///   - message: The head, with its record when no transfer carries it.
void encode(std::vector<std::uint8_t>& output, const WorkspaceStructureHead& message);
/// Decodes the body of a `WorkspaceStructure`.
///
/// - Parameter body: The frame's body.
/// - Returns: The head, or nothing for a body cut short, a flag no flag
///   names, a workspace that is gone and carries a record or a transfer, a
///   transfer with a record beside it, or neither a transfer nor a record.
[[nodiscard]] std::optional<WorkspaceStructureHead> decodeWorkspaceStructureHead(std::span<const std::uint8_t> body);
/// Decodes the body of a `PaneBind`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<PaneBind> decodePaneBind(std::span<const std::uint8_t> body);
/// Decodes the body of a `PaneUnbind`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<PaneUnbind> decodePaneUnbind(std::span<const std::uint8_t> body);
/// Decodes a chunk; its `bytes` borrow `body`.
[[nodiscard]] std::optional<BulkChunk> decodeBulkChunk(std::span<const std::uint8_t> body);
/// Decodes the body of a `Bye`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Bye> decodeBye(std::span<const std::uint8_t> body);
/// Decodes the body of a `CheckpointHead`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<CheckpointHead> decodeCheckpointHead(std::span<const std::uint8_t> body);
/// Decodes an update's header; the row-update body is the rest of the frame,
/// from the header's `bytes` on.
[[nodiscard]] std::optional<DecodedUpdateHeader> decodeUpdateHeader(std::span<const std::uint8_t> body);
/// Decodes the body of a `Resync`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Resync> decodeResync(std::span<const std::uint8_t> body);
/// Decodes the body of a `View`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<View> decodeView(std::span<const std::uint8_t> body);
/// Decodes the body of a `BulkAbort`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<BulkAbort> decodeBulkAbort(std::span<const std::uint8_t> body);
/// Decodes the body of a `Call`, refusing text that is not UTF-8.
[[nodiscard]] std::optional<Call> decodeCall(std::span<const std::uint8_t> body);
/// Decodes the body of a `Result`, refusing text that is not UTF-8 and text
/// beside a transfer.
[[nodiscard]] std::optional<Result> decodeResult(std::span<const std::uint8_t> body);
/// Splits the body of a `Notify` at its kind, refusing a body with no
/// canonical leading varint. The record is not read here: each kind's reader
/// decodes and bounds its own.
[[nodiscard]] std::optional<NotifyHead> decodeNotifyHead(std::span<const std::uint8_t> body);
/// Decodes the body of a `Receipt`, refusing one the grammar does not consume exactly.
[[nodiscard]] std::optional<Receipt> decodeReceipt(std::span<const std::uint8_t> body);
/// Decodes the body of a `Raw`, refusing one numbered 0 or one the grammar
/// does not consume exactly. The bytes borrow from `body`.
[[nodiscard]] std::optional<Raw> decodeRaw(std::span<const std::uint8_t> body);

/// Reads a `Paste`: a number, an offset, the state, and text no longer than
/// `maxPasteChunkBytes` that ends no later than `maxPasteBytes` into the
/// paste.
///
/// - Parameter body: The frame body.
/// - Returns: The piece, its text a view into `body`, or `nullopt` when the
///   body is not one.
[[nodiscard]] std::optional<Paste> decodePaste(std::span<const std::uint8_t> body);

/// Reads a `PasteAbort`.
///
/// - Parameter body: The frame body.
/// - Returns: The message, or `nullopt` when the body is not one.
[[nodiscard]] std::optional<PasteAbort> decodePasteAbort(std::span<const std::uint8_t> body);
/// Decodes the body of a `Keys`, refusing one numbered 0, one with no event
/// or more than `maxInputEventsPerFrame`, a key past `Keys::VirtualKey`'s
/// range, text that is not UTF-8 or longer than `maxKeyEventTextBytes`, or one
/// the grammar does not consume exactly. The count is checked before any
/// event is read.
[[nodiscard]] std::optional<Keys> decodeKeys(std::span<const std::uint8_t> body);
/// Decodes the body of a `Pointer`, refusing one numbered 0, one with no
/// event or more than `maxInputEventsPerFrame`, or one the grammar does not
/// consume exactly. The count is checked before any event is read.
[[nodiscard]] std::optional<Pointer> decodePointer(std::span<const std::uint8_t> body);
/// Decodes the body of a `Focus`, refusing one the grammar does not consume
/// exactly.
[[nodiscard]] std::optional<Focus> decodeFocus(std::span<const std::uint8_t> body);

/// Reads a `Viewport`.
///
/// - Parameter body: The frame's body.
/// - Returns: The viewport, or `nullopt` for a body that is not exactly one.
[[nodiscard]] std::optional<Viewport> decodeViewport(std::span<const std::uint8_t> body);
/// Decodes a `HistoryInline`'s header; its history body is the rest of the
/// frame, from the header's `bytes` on.
[[nodiscard]] std::optional<DecodedHistoryInlineHeader> decodeHistoryInlineHeader(std::span<const std::uint8_t> body);
/// Decodes the body of a `HistoryHead`, refusing an unknown order and a head
/// that announces no line.
[[nodiscard]] std::optional<HistoryHead> decodeHistoryHead(std::span<const std::uint8_t> body);
/// Decodes the body of a `HistoryReset`, refusing one whose head is below the
/// lines it cleared.
[[nodiscard]] std::optional<HistoryReset> decodeHistoryReset(std::span<const std::uint8_t> body);
/// Decodes the body of a `HistoryWant`, refusing a range count over
/// `maxHistoryWantRanges` before any range is read, an empty or inverted
/// range, ranges out of order or overlapping, and an unknown priority.
[[nodiscard]] std::optional<HistoryWant> decodeHistoryWant(std::span<const std::uint8_t> body);

/// The sentence a frontend shows for a peer's `Bye`.
///
/// A refused attach delivers no screen, so this sentence is what tells the
/// person why the window is blank: what happened and what they can do, never
/// the wire's terms. The `Bye`'s detail is the diagnostic, for the log.
///
/// - Parameter reason: The reason the `Bye` carried.
/// - Returns: One or two sentences, each ending in a period.
[[nodiscard]] std::string_view byeNotice(ByeReason reason);

} // namespace Ubiquitty::Protocol::Sync
