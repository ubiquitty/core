// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "PAL/OutputQueue.h"

#include <expected>
#include <memory>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// The state-format version a host writes; the format rule is in Docs/sync.md §6.13.
inline constexpr std::uint16_t handoverBodyVersion = 35;
/// The state format written by the last published release, v0.3.3, as v0.3.2 wrote it.
/// Release updates keep its frozen fixture alongside the writer's fixture.
inline constexpr std::uint16_t lastPublishedHandoverBodyVersion = 35;
/// The oldest state format a successor must read to carry running sessions
/// from the last published release across an update.
inline constexpr std::uint16_t oldestHandoverBodyVersion = lastPublishedHandoverBodyVersion;
static_assert(oldestHandoverBodyVersion <= lastPublishedHandoverBodyVersion);
static_assert(lastPublishedHandoverBodyVersion <= handoverBodyVersion);
inline constexpr std::size_t maxInheritedInstances = 8;

/// Classifies a state stream refused before it can become a serving host.
enum class HandoverBodyError : std::uint8_t {
    NewerVersion,
    Malformed,
    OverCapacity,
    DigestMismatch,
    AllocationRefused,
    NotAtGround,
};

/// Names the state issuer and bounded sections on the local successor channel.
/// Inherited identities preserve resume issuers across successive handovers;
/// they do not authenticate the sender or authorize any remote attachment.
struct HandoverHeader {
    ID128 predecessor;
    std::vector<ID128> inherited;
    std::uint16_t paneCount = 0;
    std::uint32_t tableBytes = 0;

    bool operator==(const HandoverHeader&) const = default;
};

/// Bounds resources according to the receiving daemon, not the sender.
struct HandoverHeaderLimits {
    std::uint16_t maxPanes = 0;
    std::uint32_t maxTableBytes = 0;
};

/// Appends the current state header, with its version before all other fields.
///
/// - Parameters:
///   - output: The destination; existing bytes are retained.
///   - header: A valid header whose ancestry contains at most eight distinct issuers.
void encodeHandoverHeader(std::vector<std::uint8_t>& output, const HandoverHeader& header);

/// Reads one complete state header without allocating any announced state body.
/// A newer version is refused after its two-byte prefix, before interpreting it.
///
/// - Parameters:
///   - bytes: Exactly one header, excluding pane records and the resume table.
///   - limits: The receiver's pane and table ceilings.
/// - Returns: The header, or a refusal with no state admitted.
[[nodiscard]] std::expected<HandoverHeader, HandoverBodyError> decodeHandoverHeader(std::span<const std::uint8_t> bytes, const HandoverHeaderLimits& limits);

/// Owns one exactly sized, fallibly allocated handover section.
struct HandoverBytes {
    std::unique_ptr<std::uint8_t[]> data;
    std::size_t size = 0;

    /// Borrows the encoded bytes while this owner lives.
    /// - Returns: The entire encoded section.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { data.get(), size }; }
};

/// Bounds both the encoded image section and every retained pixel owner.
struct HandoverImageLimits {
    std::size_t maxSectionBytes = 0;
    std::uint64_t maxPixelBytes = 0;
};

/// Encodes the complete image store, including animation, placement and allocator state.
///
/// The terminal must remain unchanged throughout measurement and encoding. An
/// open image transfer or pending decoder is refused, never finalized here.
/// - Parameters:
///   - terminal: The quiescent authoritative model.
///   - limits: The receiving host's admission bounds.
/// - Returns: Exact owned bytes, or a refusal without modifying the terminal.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverImages(const Ubiquitty::Terminal& terminal, const HandoverImageLimits& limits);

/// Validates and restores a complete image store transactionally.
///
/// All metadata, pixel lengths, references, and the section digest are checked
/// before allocating a pixel buffer. Refusal leaves the destination unchanged.
/// - Parameters:
///   - bytes: Exactly one image section produced by `encodeHandoverImages`.
///   - terminal: The receiving model, exclusively held by the caller.
///   - limits: The receiving host's bounds, independent of the encoded policy.
/// - Returns: Success after ownership transfer, or an explicit refusal.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverImages(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, const HandoverImageLimits& limits);

/// Bounds encoded history, descriptor capacity, retained rows and cell storage.
struct HandoverHistoryLimits {
    std::size_t maxSectionBytes = 0;
    std::uint32_t maxCapacity = 0;
    std::uint32_t maxRows = 0;
    std::uint64_t maxCells = 0;
};

/// Encodes every retained physical history row and its numbering continuation.
///
/// Cell references are preserved, not remapped. The containing pane must carry
/// their hyperlink, cluster and run-text stores too. Reading folded rows uses
/// the LineStore decode cache and requires the terminal's model lock.
/// - Parameters:
///   - terminal: The unchanged authoritative model.
///   - limits: The receiver's resource bounds.
/// - Returns: Exact owned bytes, or an explicit refusal.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverHistory(const Ubiquitty::Terminal& terminal, const HandoverHistoryLimits& limits);

/// Exact owned allocation overlap for replacing only the history component.
/// Includes fixed LineStore headers; excludes allocator bookkeeping, borrowed
/// wire bytes and all other Terminal components. No transport cap is a budget.
struct HandoverHistoryStoragePlan {
    std::size_t retainedCandidateBytes = 0;
    std::size_t stagingBytes = 0;
    std::size_t replacedBytes = 0;
    std::size_t peakPrepareBytes = 0;
};

/// Validates and measures through the existing history field walk, without
/// allocating or changing the destination, including its folded/cache owners.
[[nodiscard]] std::expected<HandoverHistoryStoragePlan, HandoverBodyError> handoverHistoryStoragePlan(std::span<const std::uint8_t> bytes,
    const Ubiquitty::Terminal& terminal, const HandoverHistoryLimits& limits);

/// Validates the entire history before constructing its replacement store.
///
/// Refusal leaves the destination unchanged. Staging and the detached raw
/// LineStore owners allocate fallibly after checked capacity measurement.
/// Normal live initialization, folding and cache policies are not changed.
/// - Parameters:
///   - bytes: Exactly one complete history section.
///   - terminal: The exclusively owned receiving model.
///   - limits: The receiver's limits, independent of carried configuration.
/// - Returns: Success after replacing history, or a refusal before mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverHistory(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, const HandoverHistoryLimits& limits);

/// Admits the exact history-only old/staging/new allocation overlap before
/// allocating. The default overload has no additional byte ceiling.
/// - Parameter maxPeakHistoryBytes: Explicit component allowance, not a full
///   terminal/preparation bound and not inferred from a transport limit.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverHistory(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal,
    const HandoverHistoryLimits& limits, std::size_t maxPeakHistoryBytes);

/// Encodes the hyperlink ring and the exact monotonic/reclaimed ID allocator.
/// - Parameters:
///   - terminal: The exclusively held, unchanged model.
///   - maxBytes: The receiver's encoded-section ceiling.
/// - Returns: Owned bytes, or a refusal for an inconsistent allocator or bound.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverLinks(const Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Restores link identity without making an old cell name a new target.
///
/// Both screens and complete history must already have been restored into the
/// private receiving model. Every reclaimed ID is checked against their cells
/// before admission. No caller may publish this intermediate receiving model.
/// - Parameters:
///   - bytes: One complete link section.
///   - terminal: The private receiving model with the carried cells installed.
///   - maxBytes: The receiver's independent encoded-section ceiling.
/// - Returns: Success after replacement, or refusal without mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverLinks(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Bounds cluster/run descriptors and their combined retained code points.
struct HandoverTextStoreLimits {
    std::size_t maxSectionBytes = 0;
    std::uint16_t maxClusterEntries = 0;
    std::uint16_t maxRunSlots = 0;
    std::uint32_t maxCodePoints = 0;
};

/// Encodes text stores together with the allocator state a checkpoint omits.
/// - Parameters:
///   - terminal: The exclusively held, unchanged model.
///   - limits: The receiving host's bounds.
/// - Returns: Exact owned bytes, or an explicit refusal.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverTextStores(const Ubiquitty::Terminal& terminal, const HandoverTextStoreLimits& limits);

/// Reconstructs complete stores before replacing either destination owner.
///
/// Every count, scalar and allocator reference is validated before reconstruction.
/// Cluster entries and lookup arrays are fallible final owners. Run storage
/// retains bounded STL allocation behavior, so this is not a complete
/// recoverable-allocation guarantee.
/// - Parameters:
///   - bytes: One complete text-store section.
///   - terminal: The private receiving model.
///   - limits: The receiving host's independent bounds.
/// - Returns: Success after replacement, or refusal without mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverTextStores(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, const HandoverTextStoreLimits& limits);

/// Encodes ground-mode policy and a still-open grapheme cluster.
/// - Parameters:
///   - terminal: The unchanged model at handover ground.
///   - maxBytes: The receiver's encoded-section ceiling.
/// - Returns: Owned parser state, or an explicit refusal without flushing it.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverParser(const Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Restores carried parser fields without replacing the checkpoint's REP record.
///
/// Both screens and text stores must already be restored into the private model.
/// Pending sequences and partial input characters are refused, not completed.
/// - Parameters:
///   - bytes: Exactly one ground parser section.
///   - terminal: The unpublished receiving model with checkpoint state installed.
///   - maxBytes: The receiver's independent byte ceiling.
/// - Returns: Success after validation, or refusal without mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverParser(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Independent limits for the fixed soft-font store and each Tek display list.
struct HandoverVisualLimits {
    std::size_t maxSectionBytes = 0;
    std::uint16_t maxTekPrimitives = 0;
};

/// Encodes downloaded glyphs, both Tek frames and the Tek decoder continuation.
/// - Parameters:
///   - terminal: The exclusively held model at handover ground.
///   - limits: The receiving host's section and display-list bounds.
/// - Returns: Exact owned bytes, or refusal without changing the model.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverVisualStores(const Ubiquitty::Terminal& terminal, const HandoverVisualLimits& limits);

/// Reconstructs both visual stores only after complete bounded validation.
///
/// The checkpoint supplies the active Tek mode and character-set designations.
/// The surrounding host transaction must reserve inherited process-wide font
/// serials before publishing any restored pane or minting new font identities.
/// - Parameters:
///   - bytes: One complete visual-store section.
///   - terminal: The private unpublished receiving model.
///   - limits: The receiver's independently chosen bounds.
/// - Returns: Success after replacement, or refusal with both stores unchanged.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverVisualStores(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, const HandoverVisualLimits& limits);

/// Bounds encoded sound state and the combined bank/queued PCM payload.
struct HandoverSoundLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxPCMBytes = 0;
};

/// Encodes the sample bank and exact immutable pending-sound owners.
/// - Parameters:
///   - terminal: The exclusively held model at handover ground.
///   - limits: The receiving host's independent admission limits.
/// - Returns: Exact owned bytes, or refusal without consuming a sound.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverSounds(const Ubiquitty::Terminal& terminal, const HandoverSoundLimits& limits);

/// Reconstructs sound owners without playing or consuming any request.
///
/// Every count, request and PCM span is validated before candidate allocation.
/// Bank vectors retain their existing bounded STL allocation behavior; this is
/// not a universal recoverable-out-of-memory contract.
/// - Parameters:
///   - bytes: One complete sound section.
///   - terminal: The private unpublished receiving model.
///   - limits: The receiver's independently chosen bounds.
/// - Returns: Success after replacing both owners, or refusal without mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverSounds(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, const HandoverSoundLimits& limits);

/// Encodes remaining host-only terminal state, including security policy and replies.
///
/// This section carries stable font, typography, trigger and integration state.
/// Callback bindings are never serialized. Stream-accepted PTY bytes belong to
/// the separate stream owner, not the pending terminal replies encoded here.
/// - Parameters:
///   - terminal: The exclusively held model at handover ground.
///   - maxBytes: The receiver's section ceiling.
/// - Returns: Exact owned bytes, or refusal without consuming any work.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverHostState(const Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Validates and prepares all replacement owners before committing host state.
///
/// Existing callback bindings remain untouched. Bounded strings and shared
/// typography retain their normal STL allocation behavior; this is not a
/// universal recoverable-allocation contract.
/// - Parameters:
///   - bytes: One complete host-state section.
///   - terminal: The private unpublished receiving model at ground.
///   - maxBytes: The receiver's independent section ceiling.
/// - Returns: Success after replacement, or refusal without mutation.
[[nodiscard]] std::expected<void, HandoverBodyError> restoreHandoverHostState(std::span<const std::uint8_t> bytes, Ubiquitty::Terminal& terminal, std::size_t maxBytes);

/// Receiver-owned limits for one joined terminal, independent of session/PTY state.
struct HandoverTerminalLimits {
    std::size_t maxRecordBytes = 0;
    std::size_t maxCheckpointBytes = 0;
    std::size_t maxScreenCellBytes = 0;
    std::size_t maxHostStateBytes = 0;
    std::size_t maxLinkBytes = 0;
    std::size_t maxParserBytes = 0;
    HandoverImageLimits images;
    HandoverHistoryLimits history;
    HandoverTextStoreLimits text;
    HandoverVisualLimits visual;
    HandoverSoundLimits sounds;
    /// Complete checkpoint preparation plus the candidate's grid backing peak.
    /// This provisional host allowance is independent of the client policy and
    /// other carried continuation sections; zero refuses every checkpoint.
    std::size_t maxCheckpointStorageCost = std::size_t { 256 } << 20;
};

/// What a process's decode owner admits across every unpublished adopt,
/// reclaim and handover candidate at once: two candidates at the host's
/// checkpoint allowance, so one maximal candidate never waits on another
/// ordinary one, and a third is refused until one publishes.
inline constexpr std::size_t defaultMaxHostDecodeBytes = std::size_t { 512 } << 20;

/// An unpublished terminal owner; host callbacks and external identities are not yet installed.
struct HandoverTerminal {
    std::unique_ptr<Ubiquitty::Terminal> terminal;
    std::uint64_t generation = 0;
    /// The candidate's charge to its process's decode owner. Its holder keeps
    /// it until the terminal is published or destroyed.
    MemoryCharge decodeCharge {};
};

/// Joins a checkpoint and every terminal continuation section under one digest.
///
/// The caller holds the quiescence lease through the entire call. This does not
/// encode PaneSource, SessionFacts, grants or accepted stream bytes. Encoder
/// scratch retains existing bounded container allocation behavior.
/// - Parameters:
///   - terminal: The authoritative, unchanged terminal at ground.
///   - generation: Its frozen nonzero screen generation.
///   - limits: The receiver's independently chosen admission bounds.
/// - Returns: One complete owned record, or refusal without source mutation.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverTerminal(const Ubiquitty::Terminal& terminal, std::uint64_t generation, const HandoverTerminalLimits& limits);

/// Validates continuation sections before constructing a private candidate.
///
/// Checkpoint and owner sections must agree on history and shared dictionaries.
/// No callbacks or program effects run, and no intermediate owner is published.
/// Checkpoint preparation and the candidate's grid peak are admitted together.
/// Existing bounded decoder containers are not a universal recoverable OOM path.
/// The candidate charges the receiving process's one decode owner, shared by
/// every adopt, reclaim and handover (Docs/sync.md §7.9), before anything
/// input-sized is allocated: its checkpoint preparation and grid peak, its
/// history store's planned capacity, and every other section at its carried
/// length. A candidate past the owner is refused as `AllocationRefused`.
/// - Parameters:
///   - bytes: Exactly one joined terminal record.
///   - limits: The receiving host's limits, never inferred from sender policy.
///   - decodeMemory: The process's decode owner; null bounds the candidate by
///     `limits` alone.
/// - Returns: A wholly owned terminal and generation, or explicit refusal.
[[nodiscard]] std::expected<HandoverTerminal, HandoverBodyError> decodeHandoverTerminal(std::span<const std::uint8_t> bytes, const HandoverTerminalLimits& limits,
    std::shared_ptr<MemoryBudget> decodeMemory = nullptr);

/// Bounds publication dictionaries and retained rewrite metadata independently.
struct HandoverPublicationLimits {
    std::size_t maxSectionBytes = 0;
    std::uint32_t maxPens = 0;
    std::uint32_t maxLinks = 0;
    std::uint32_t maxClusters = 0;
    std::uint32_t maxHistoryHashes = 0;
};

/// Portable Session-ledger facts, excluding external owners and policy objects.
///
/// The host adapter supplies these from one unchanged Session and restores them
/// only after its compound configuration, capability, coherence and input
/// encoder state have independently been admitted. Empty defaults do not prove
/// those separate owners were empty. Process probes and borrowed callbacks are
/// rewired; permission is never inferred from these presentation facts.
struct SessionFacts {
    /// Identifies the host layout without importing its mutable tree owner.
    struct Placement {
        std::uint32_t surface = 0;
        std::uint32_t tab = 0;
        std::uint32_t pane = 0;
    };
    /// Retains the acknowledged presentation owner and its original timing.
    struct Presented {
        ID128 owner;
        std::uint64_t revision = 0;
        std::uint64_t appliedAt = 0;
    };
    /// Retains the host's title probe cache without retaining process pointers.
    struct Title {
        std::uint32_t revision = 0;
        std::string currentDirectory;
        std::string processChain;
        std::string processChainWithArguments;
        std::string foregroundName;
        std::string shellCommandName;
        std::string ttyName;
    };
    std::uint32_t sessionID = 0;
    std::uint64_t createdUnix = 0;
    Placement placement;
    std::uint32_t lastInputClientID = 0;
    std::uint32_t lastHumanInputClientID = 0;
    PaneActivityFacts activity;
    std::uint64_t stampedTypedInputWrites = 0;
    std::uint64_t seenPersonFocusGains = 0;
    std::uint64_t lastRecordsMilliseconds = 0;
    std::uint64_t recordsOwedAtMilliseconds = 0;
    std::uint64_t lastActivityStampMilliseconds = 0;
    std::uint64_t describedStructuralGeneration = 0;
    std::optional<std::uint64_t> recordedActivityGeneration;
    SessionEndFacts end;
    CapabilityOrigin outputOrigin = CapabilityOrigin::Local;
    CapabilityOrigin driverOriginFloor = CapabilityOrigin::Local;
    /// The first principal whose input could have started surviving work.
    std::optional<std::string> firstInputPrincipal;
    /// More than one principal may have caused the session's output.
    bool hasMultipleInputPrincipals = false;
    bool isFrameDue = false;
    bool isDriverFocusRecorded = false;
    std::optional<PaneGeometry> pendingGeometry;
    bool isWindowSizeDue = false;
    std::uint64_t geometryChangesApplied = 0;
    std::optional<Presented> presentedApplied;
    std::optional<std::uint64_t> presentedDueAt;
    std::optional<ID128> presentedStandIn;
    std::optional<std::uint64_t> unpublishedSince;
    std::uint32_t lastEmittedCommandGeneration = 0;
    std::uint32_t lastEmittedCommandLineGeneration = 0;
    std::uint32_t lastEmittedCommandStartedGeneration = 0;
    bool wasForegroundShell = true;
    std::string lastForegroundName;
    std::uint32_t foregroundGeneration = 0;
    std::optional<PaneEchoFacts> publishedEchoFacts;
    std::vector<OSC::ProgramStatus> reportedProgramStatus;
    std::uint64_t reportedActivityGeneration = 0;
    Title paneFacts;
    bool didShellExit = false;
    std::string exitReason;
    std::int32_t mouseTrackingProcessGroup = 0;
    bool isAdopted = false;
    std::uint32_t historyLinesNotCarried = 0;
    std::string workspaceName;
    bool shouldEndWhenDetached = false;
    std::string sshHost;
    bool wasRestoredAfterHostRestart = false;
    bool wasKilledBySignal = false;
    std::uint64_t exitStatusDeadlineMilliseconds = 0;
    std::string observedDirectory;
    std::uint64_t graceExpiresMilliseconds = 0;
    bool isGraceAwaitingHuman = false;
    std::vector<std::string> landedPastePaths;
};

/// Receiver admission for a joined pane and its independent pending-byte owner.
struct HandoverPaneLimits {
    HandoverTerminalLimits terminal;
    HandoverPublicationLimits publication;
    std::size_t maxRecordBytes = 0;
    std::size_t maxSourceBytes = 0;
    std::size_t maxSessionBytes = 0;
    std::size_t maxEffectRetainedBytes = 0;
    std::size_t maxAcceptedPTYBytes = 0;
    std::uint32_t maxLandedPastePaths = 0;
};

/// Owns a complete private pane before host callbacks or a stream are attached.
/// Source destruction precedes its borrowed terminal. Accepted PTY bytes must
/// enter an empty stream queue directly, never numbered or encoded a second time.
struct HandoverPaneImage {
    ID128 paneID;
    std::unique_ptr<Ubiquitty::Terminal> terminal;
    std::unique_ptr<PaneSource> source;
    SessionFacts session;
    PAL::OutputQueue::Unwritten unwrittenPTY;
    /// The terminal's charge to the successor's decode owner, held until the
    /// pane is published or dropped.
    MemoryCharge decodeCharge {};
};

/// Encodes portable Session facts without serializing any external owner.
/// - Parameters:
///   - session: Unchanged facts extracted from one frozen Session.
///   - limits: Receiver byte and path-count bounds.
/// - Returns: One bounded section or refusal.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverSessionFacts(const SessionFacts& session, const HandoverPaneLimits& limits);

/// Decodes bounded portable Session facts before any pane can be installed.
/// - Parameters:
///   - bytes: Exactly one complete facts section.
///   - limits: Receiver-selected byte and path-count bounds.
/// - Returns: Owned facts or refusal, without authorizing a session.
[[nodiscard]] std::expected<SessionFacts, HandoverBodyError> decodeHandoverSessionFacts(std::span<const std::uint8_t> bytes, const HandoverPaneLimits& limits);

/// Encodes a frozen pane and its exact already-accepted stream suffix.
///
/// The host must additionally admit its compound Session owners and keep the
/// predecessor frozen through the transaction decision. Unfinished input intents
/// or paste encoders are not converted into a fabricated drained state.
/// - Parameters:
///   - source: Held publication and terminal, unchanged throughout encoding.
///   - session: Portable facts from the same frozen Session.
///   - unwrittenPTY: Exact stream-owned bytes and ownership partition.
///   - limits: Independent successor admission bounds.
/// - Returns: One record or refusal without changing the predecessor.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodePaneHandover(const PaneSource& source,
    const SessionFacts& session, const PAL::OutputQueue::Unwritten& unwrittenPTY, const HandoverPaneLimits& limits);

/// Reconstructs private terminal, source and stream-byte owners as one unit.
/// - Parameters:
///   - bytes: One entire checksummed pane record, excluding transport framing.
///   - limits: Successor admission bounds, never inferred from carried policy.
///   - decodeMemory: The successor's decode owner (`decodeHandoverTerminal`).
/// - Returns: Private owners ready for host validation/rebinding, or refusal.
[[nodiscard]] std::expected<HandoverPaneImage, HandoverBodyError> decodePaneHandover(std::span<const std::uint8_t> bytes,
    const HandoverPaneLimits& limits, std::shared_ptr<MemoryBudget> decodeMemory = nullptr);

/// Encodes exact publisher identity without capturing or advancing the source.
/// - Parameters:
///   - source: The held, encodable source under the owner's quiescence lease.
///   - limits: Independent receiver count and byte bounds.
/// - Returns: Exact owned publication bytes, or a source-preserving refusal.
[[nodiscard]] std::expected<HandoverBytes, HandoverBodyError> encodeHandoverPublication(const PaneSource& source, const HandoverPublicationLimits& limits);

/// Validates every publication span before constructing bounded owned vectors.
///
/// The resulting continuation must still match the reconstructed terminal via
/// PaneSource::restorePublication before that pane can become visible.
/// - Parameters:
///   - bytes: Exactly one checksummed publication section.
///   - limits: Receiver bounds, not sender-selected configuration.
/// - Returns: Owned continuation or explicit refusal.
[[nodiscard]] std::expected<PaneSource::PublicationContinuation, HandoverBodyError> decodeHandoverPublication(std::span<const std::uint8_t> bytes, const HandoverPublicationLimits& limits);

} // namespace Ubiquitty::Protocol::Sync
