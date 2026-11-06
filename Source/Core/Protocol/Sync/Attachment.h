// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Attachment.h — attachments outlive the connections that carry them
// (Docs/sync.md §6.9).
//
// An attachment is a client's standing on the host: who it is, what it may
// do, and what the host last sent and the client acknowledged of each pane.
// A connection serves an attachment; a dropped transport leaves the attachment
// `Lost`, and a resume on a new connection continues it from what the client
// still holds, by digest, instead of a checkpoint.
//
// The invariants:
//
// - A resume is admitted only for an attachment this host issued or inherited,
//   `Live` or `Lost`, under the principal that created it, with the secret
//   the last `Welcome` issued. The secret is compared in constant time and
//   replaced on every admission, so it is single use. A refused resume changes
//   nothing.
// - A resume of an attachment a transport still serves succeeds first, and
//   the old connection is closed second (`Bye{Superseded}`); a failed resume
//   leaves it serving. A second resume presenting the consumed secret before
//   the first's client has spoken on its new connection is in progress.
// - A `Lost` attachment stays resumable for the grace window, then becomes
//   `Detached` and is freed. What was written to a pane is the pane's, not the
//   attachment's, and outlives both.
// - The baselines a resume diffs from live in the record, not in the
//   connection, so a new connection continues from them.
//

#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Protocol/Sync/ScreenPlane.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// How long a host keeps an attachment whose transport went (Docs/sync.md
/// §6.9). The silence that declares a transport gone is the pacing policy's
/// `lostSilenceMicroseconds`.
struct ResumePolicy {
    /// How long a `Lost` attachment stays resumable before it is `Detached`
    /// and freed.
    std::uint64_t lostGraceMicroseconds;
};

/// The default attachment-retention policy.
///
/// - Returns: The policy.
[[nodiscard]] constexpr ResumePolicy measuredResumePolicy()
{
    // A lost attachment can resume only while its grace period remains.
    return { .lostGraceMicroseconds = 600'000'000 };
}

/// Where an attachment stands.
enum class AttachmentState : std::uint8_t {
    /// A connection serves it.
    Live,
    /// Its transport went, or it suspended; it is resumable for the grace.
    Lost,
    /// It is gone: the client left, or the grace ran out.
    Detached,
};

/// Draws the secrets the host issues. The owner supplies one over the
/// system's entropy; a test supplies a sequence.
class SecretSource {
public:
    virtual ~SecretSource() = default;

    /// Draws one secret.
    ///
    /// - Returns: The secret.
    [[nodiscard]] virtual ID128 nextSecret() = 0;
};

/// What a connection's peer was authenticated as.
struct ConnectionAuthority {
    /// The principal: an SSH user, a uid, a token's principal.
    std::string principal;
    /// Whether its authenticated authority permits input.
    bool mayDrive = false;
    /// The most the host verified it to be: a person only by a token minted
    /// for one or a local binary the host trusts; an agent otherwise.
    ClientKind verifiedKind = ClientKind::Agent;

    friend bool operator==(const ConnectionAuthority&, const ConnectionAuthority&) = default;
};

/// The history publication accompanying a retained screen baseline. An epoch
/// of zero means no history publication is known for that baseline.
struct HistoryBaseline {
    std::uint32_t epoch = 0;
    std::uint64_t head = 0;
    std::uint64_t revision = 0;
};

/// One generation sent to a pane and the baseline it left the client at.
struct SentGeneration {
    std::uint64_t generation = 0;
    AttachmentBaseline baseline;
    HistoryBaseline history {};
};

/// What the host sent an attachment of one pane and what the client
/// acknowledged: what a resume is matched against.
struct PaneBaselines {
    ID128 paneID;
    std::uint64_t sentGeneration = 0;
    AttachmentBaseline sentBaseline;
    std::optional<std::uint64_t> acknowledgedGeneration;
    AttachmentBaseline acknowledgedBaseline;
    HistoryBaseline acknowledgedHistory {};
    /// Every generation sent after the acknowledged one, ascending.
    std::deque<SentGeneration> inFlight;
    /// Whether the attachment holds the pane now: bound on its connection,
    /// or bound when its transport went. False once the pane was unbound;
    /// the baselines stay for a bind that follows.
    bool isHeld = true;
    /// Whether this attachment has registered its pane-local effect cursor.
    bool hasEffectReader = false;
    /// Complete batches actually handed to the carrier and acknowledged.
    std::uint64_t effectsSent = 0;
    std::uint64_t effectsAcknowledged = 0;
};

/// One attachment.
struct AttachmentRecord {
    ID128 attachmentID;
    /// The full host identity that issued the current resume secret.
    ID128 issuer;
    ID128 resumeSecret;
    ConnectionAuthority authority;
    ClientKind kind = ClientKind::Human;
    ID128 clientInstance;
    AttachmentState state = AttachmentState::Live;
    /// Unapproved records carry identity only, never resumable pane authority.
    AttachmentAdmission admission = AttachmentAdmission::Granted;
    /// Unbound control identities exist only for their current transport.
    bool isResumable = true;
    /// The owner's name for the connection serving it.
    std::uint64_t servingConnection = 0;
    /// When it became `Lost`.
    std::uint64_t lostSinceMicroseconds = 0;
    /// The secret the newest admitted resume consumed, and the connection it
    /// was admitted on, until that connection's client sends its first frame
    /// after the `Welcome`: a second resume presenting that secret in the
    /// window races the first and is refused as in progress.
    std::optional<ID128> consumedSecret;
    std::optional<std::uint64_t> handshakeConnection;
    /// The connection a resume proof admitted, until its `Greet` resumes the
    /// attachment or it goes: the one connection whose resume carries no
    /// secret.
    std::optional<std::uint64_t> proofConnection;
    /// The face offer retained across host handover until this attachment's
    /// new connection publishes its own. It is never authority to drive.
    std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts> inheritedProportionalFaces;
    /// Its panes' baselines, each at a stable address a connection borrows.
    std::vector<std::unique_ptr<PaneBaselines>> panes;

    /// A pane's baselines, or null for a pane the attachment never held.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The baselines.
    [[nodiscard]] PaneBaselines* findPane(ID128 paneID);

    /// A pane's baselines, made empty the first time it is asked for.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The baselines.
    PaneBaselines& paneFor(ID128 paneID);
};

/// What an attachment may do with a pane, as a client is told it.
enum class AttachmentRole : std::uint8_t {
    /// It is shown the pane and sends no input.
    Observer,
    /// It may send input.
    Driver,
};

/// A display of an admitted per-pane grant, never authority supplied by a client.
enum class PresenceGrant : std::uint8_t {
    None = 0,
    Session = 1,
    Always = 2,
};

/// One shared pane's independently resolved grant in a presence row.
struct SharedPaneGrant {
    ID128 paneID;
    PresenceGrant grant = PresenceGrant::None;
    friend bool operator==(const SharedPaneGrant&, const SharedPaneGrant&) = default;
};

/// Verified live endpoint facts and a separately claimed display label.
struct AttachmentPresence {
    std::uint32_t clientID = 0;
    std::uint32_t processID = 0;
    std::string process {};
    std::string claimedLabel {};
    /// Verified daemon grant key; never the process path or claimed display label.
    std::string grantIdentity {};
    bool isQUIC = false;
    std::vector<SharedPaneGrant> sharedPanes {};
    friend bool operator==(const AttachmentPresence&, const AttachmentPresence&) = default;
};

/// One attachment as a client is told of it (Docs/sync.md §6.10).
struct AttachmentEntry {
    ID128 attachmentID;
    AttachmentRole role = AttachmentRole::Observer;
    ClientKind clientKind = ClientKind::Human;
    /// `Live`, or `Lost` while its grace runs; a freed attachment is not
    /// listed.
    AttachmentState state = AttachmentState::Live;
    /// Whether it owns the size of a pane the recipient shows.
    bool isGeometryOwner = false;
    /// Whether it is the primary of a pane the recipient shows.
    bool isPrimary = false;
    /// Only panes shared with this recipient; an empty intersection is self-only.
    std::vector<ID128> panes {};
    bool isSelf = false;
    /// The authenticated principal, present only for an Owner recipient.
    std::optional<std::string> principal;
    /// Present only for a verified Live endpoint and an eligible human audience.
    std::optional<AttachmentPresence> presence {};

    friend bool operator==(const AttachmentEntry&, const AttachmentEntry&) = default;
};

/// The attachments a client is told of: those that share a pane with it.
struct AttachmentsList {
    /// Orders the lists a host instance sends: a client keeps the list with
    /// the highest it has seen. It only grows across a client's connections.
    std::uint64_t revision = 0;
    std::vector<AttachmentEntry> attachments;
};

/// Why a resume was refused.
enum class ResumeRefusal : std::uint8_t {
    /// No attachment of this host instance has the identity.
    UnknownAttachment,
    /// The attachment exists but is not resumable now.
    NotResumable,
    /// The connection's principal is not the attachment's.
    OtherPrincipal,
    /// The secret is not the one the last `Welcome` issued.
    WrongSecret,
    /// Another resume of it is being admitted.
    InProgress,
};

/// Whether two identities are equal, in time that does not depend on where
/// they differ.
///
/// - Parameters:
///   - left: One identity.
///   - right: The other.
/// - Returns: Whether they are equal.
[[nodiscard]] bool constantTimeEqual(const ID128& left, const ID128& right);

/// The attachments a host keeps across its connections.
class AttachmentTable {
public:
    /// An owned, in-memory continuation of authenticated attachment state.
    /// Physical connections and consumed handshake secrets are not carried.
    /// Encoding, allocation limits and daemon token grants belong to the
    /// authenticated host-state transfer owner, not this value.
    struct Continuation {
        ID128 hostInstanceID;
        /// Ancestors from oldest to newest, excluding hostInstanceID.
        std::vector<ID128> inheritedInstances;
        std::vector<std::unique_ptr<AttachmentRecord>> records;
        std::uint64_t revision = 0;
        std::uint64_t notifyRevision = 0;
    };

    /// Why an in-memory continuation cannot replace a fresh table.
    enum class InheritanceRefusal : std::uint8_t {
        DestinationNotEmpty,
        InvalidIdentity,
        InvalidRecord,
        ExhaustedIdentity,
    };

    /// Retires a record's pane resources before its baselines are freed.
    using RetireRecord = std::function<void(ID128)>;

    /// Creates an empty table.
    ///
    /// - Parameters:
    ///   - hostInstanceID: This host's full identity and new-secret issuer.
    ///   - policy: The grace a `Lost` attachment keeps.
    ///   - secrets: Where secrets are drawn; it must outlive the table.
    AttachmentTable(ID128 hostInstanceID, const ResumePolicy& policy, SecretSource& secrets);

    /// Installs the pane owner's synchronous retirement callback.
    ///
    /// The callback runs before erasing a record and must not re-enter this
    /// table. It receives the identity by value, never a borrow of the record.
    /// A prepared handover table takes its owner only when committed.
    ///
    /// - Parameter retire: The owner that releases the record's pane resources.
    void setRetireRecord(RetireRecord retire);

    /// Whether this table has never held authority or emitted a retirement.
    /// Private handover preparation must not replace an already-used table,
    /// even after its last physical connection and attachment have gone.
    [[nodiscard]] bool isPristine() const;

    /// Copies retained attachments without changing the predecessor.
    /// Live entries become Lost at now; Lost entries keep their absolute age.
    /// - Parameter now: The shared monotonic clock at the freeze boundary.
    /// - Returns: Owned records without transport, proof or consumed-secret ties.
    [[nodiscard]] Continuation exportForHandover(std::uint64_t now) const;

    /// Admits an authenticated predecessor's continuation into a fresh table.
    /// Refusal changes neither the table nor the supplied continuation.
    /// - Parameters:
    ///   - continuation: Owned records from the frozen predecessor.
    ///   - predecessor: The full predecessor identity verified by the transfer.
    ///   - now: The shared monotonic clock; grace continues across the transfer.
    /// - Returns: Success, or the invalid boundary without consuming any owner.
    [[nodiscard]] std::expected<void, InheritanceRefusal> inherit(Continuation&& continuation, ID128 predecessor, std::uint64_t now);

    /// Checks a full secret-issuer identity against the admitted ancestry.
    /// - Parameter issuer: The full host identity, not an attachment's high half.
    /// - Returns: Whether this host may resume a secret issued there.
    [[nodiscard]] bool isResumableIssuer(ID128 issuer) const;

    /// Forgets the baselines an attachment keeps for a pane it no longer
    /// holds, so a later bind of that pane is sent a checkpoint.
    ///
    /// A pane's baselines are borrowed only while the attachment holds it, so
    /// none forgotten here is borrowed.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - paneID: The pane it no longer holds.
    /// - Returns: Whether unheld baselines were forgotten.
    bool forgetUnheldPane(ID128 attachmentID, ID128 paneID);

    /// Checks whether retained authority covers an exact existing pane.
    /// No secret is consumed; the later Greet still authenticates the resume.
    /// - Parameters:
    ///   - issuer: The authenticated inherited secret issuer.
    ///   - principal: The carrier's currently authenticated principal.
    ///   - paneID: The stable pane identity, not a numeric session guess.
    ///   - proven: The exact proof-bound attachment, when already established.
    ///   - now: The shared monotonic time for the original Lost grace.
    /// - Returns: Whether an unexpired matching record still holds the pane.
    [[nodiscard]] bool holdsResumablePane(ID128 issuer, std::string_view principal, ID128 paneID,
        std::optional<ID128> proven, std::uint64_t now) const;

    /// Creates a fresh attachment, and frees every `Lost` attachment of the
    /// same principal's same client instance: that client is back, by a
    /// login, and is not coming back for what it lost.
    ///
    /// - Parameters:
    ///   - clientInstance: The client's instance.
    ///   - kind: Its kind.
    ///   - authority: What its connection was authenticated as.
    ///   - connection: The owner's name for that connection.
    ///   - now: The clock.
    /// - Returns: The record, its address stable until it is freed.
    AttachmentRecord& create(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now);

    /// Creates a transport-local control identity without superseding pane resumes.
    /// It cannot resume, prove a new connection, or cross a handover, and is
    /// erased rather than retained when its exact serving transport goes.
    ///
    /// - Parameters:
    ///   - clientInstance: The client's instance, possibly shared with a pane attachment.
    ///   - kind: Its verified kind.
    ///   - authority: Its authenticated connection authority.
    ///   - connection: Its exact physical connection owner.
    ///   - now: The clock for ordinary Lost-record expiry.
    /// - Returns: The stable unbound identity.
    AttachmentRecord& createUnbound(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now);

    /// Creates an unapproved identity without superseding an admitted attachment.
    /// The daemon applies its existing pending-client limits before creating it.
    ///
    /// - Parameters:
    ///   - clientInstance: The client's instance.
    ///   - kind: Its verified kind.
    ///   - authority: What its connection was authenticated as.
    ///   - connection: The owner's name for that connection.
    ///   - now: The clock for ordinary Lost-record expiry.
    /// - Returns: The unbound record, erased if its pending connection goes.
    AttachmentRecord& createPending(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now);

    /// Grants the exact pending identity once after the owner's consent checks.
    /// The owner admits the matching approved status in the same non-reentrant
    /// transaction before this call, then binds panes only after it succeeds.
    ///
    /// - Parameters:
    ///   - attachmentID: The unchanged pending identity.
    ///   - authority: The revalidated, unchanged connection authority.
    ///   - connection: The connection that owns this pending identity.
    /// - Returns: Whether that exact pending record was granted; refusal changes nothing.
    [[nodiscard]] bool promotePending(ID128 attachmentID, const ConnectionAuthority& authority, std::uint64_t connection);

    /// Checks the exact record before an owner-thread approval transaction.
    /// - Parameters:
    ///   - attachmentID: The pending identity.
    ///   - authority: Its revalidated, unchanged authority.
    ///   - connection: Its current physical owner.
    /// - Returns: Whether promotion can follow without any intervening mutation.
    [[nodiscard]] bool canPromotePending(ID128 attachmentID, const ConnectionAuthority& authority, std::uint64_t connection) const;

    /// Admits a resume, or refuses it and changes nothing.
    ///
    /// - Parameters:
    ///   - request: The resume.
    ///   - authority: What the new connection was authenticated as.
    ///   - connection: The owner's name for the new connection.
    ///   - now: The clock.
    /// - Returns: The continued record, its secret replaced, or why not.
    [[nodiscard]] std::expected<AttachmentRecord*, ResumeRefusal> resume(const ResumeRequest& request, const ConnectionAuthority& authority, std::uint64_t connection, std::uint64_t now);

    /// Admits a resume proof (Docs/sync.md §6.9 "Resume without SSH"): the
    /// MAC must be the one this attachment's secret gives over the
    /// connection's exporter, under the recorded secret issuer. The attachment's
    /// resume is then in progress on that connection until its `Greet`, and
    /// only that `Greet` may resume it without a secret. Nothing is created:
    /// no attachment, principal, pane or shell.
    ///
    /// - Parameters:
    ///   - proof: What the client presented.
    ///   - exporter: The connection's TLS keying-material exporter.
    ///   - connection: The owner's name for the connection.
    ///   - now: The clock.
    /// - Returns: The attachment, whose authority the connection is served
    ///   under, or why not: `UnknownAttachment` for another instance's
    ///   identity or one freed, `WrongSecret` for a MAC that is not this
    ///   connection's, `InProgress` while a resume's handshake is under way.
    ///   A connection an earlier proof admitted, which has not greeted,
    ///   gives way: it is listed by `takeSuperseded`.
    [[nodiscard]] std::expected<const AttachmentRecord*, ResumeRefusal> admitProof(const ResumeProof& proof, std::span<const std::uint8_t, 32> exporter, std::uint64_t connection, std::uint64_t now);

    /// The attachment a proof admitted a connection for, while its `Greet`
    /// is awaited.
    ///
    /// - Parameter connection: The connection.
    /// - Returns: The attachment, or `nullopt` for a connection no proof
    ///   admitted or whose `Greet` has resumed.
    [[nodiscard]] std::optional<ID128> provenAttachment(std::uint64_t connection) const;

    /// Notes that a connection a proof admitted went, or was refused, before
    /// its `Greet` resumed: the attachment is free to be resumed again.
    ///
    /// - Parameter connection: The connection.
    void abandonProof(std::uint64_t connection);

    /// Notes that the connection serving an attachment lost its transport:
    /// the attachment is `Lost` and resumable for the grace. One whose
    /// client already holds a newer attachment is freed instead, as `create`
    /// frees the `Lost` ones a new attachment replaces: the same principal's
    /// same client instance came back by a login, and holds two of nothing.
    /// The caller keeps no record it held past this call.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - connection: The connection that lost it; a connection the
    ///     attachment has since moved off changes nothing.
    ///   - now: The clock.
    void noteTransportLost(ID128 attachmentID, std::uint64_t connection, std::uint64_t now);

    /// Notes the first frame a resumed connection's client sent after its
    /// `Welcome`: the resume's handshake is over.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - connection: The connection the frame arrived on.
    void noteFirstFrame(ID128 attachmentID, std::uint64_t connection);

    /// Takes the connections resumes moved attachments off, which the owner
    /// closes with `Bye{Superseded}` and reads nothing more from.
    ///
    /// - Returns: The connections, in the order they were superseded.
    [[nodiscard]] std::vector<std::uint64_t> takeSuperseded();

    /// Notes that the client left: the attachment is `Detached` and freed.
    ///
    /// - Parameter attachmentID: The attachment.
    void noteDetached(ID128 attachmentID);

    /// Frees every `Lost` attachment past the grace.
    ///
    /// - Parameter now: The clock.
    void expire(std::uint64_t now);

    /// The pane's primary: the geometry owner
    /// unless it is `Lost`, otherwise the most recent typing human driver
    /// that is `Live`. Only it posts OS notifications and takes clipboard
    /// writes.
    ///
    /// - Parameter geometry: The pane's geometry arbiter.
    /// - Returns: The attachment, or `nullopt` when no attachment qualifies.
    [[nodiscard]] std::optional<ID128> primary(const GeometryArbiter& geometry) const;

    /// Whether any attachment the table holds is a client's.
    ///
    /// - Parameter clientInstance: The client.
    [[nodiscard]] bool holdsClient(ID128 clientInstance) const;

    /// An attachment freed, and the client whose it was.
    struct Freed {
        ID128 attachmentID;
        ID128 clientInstance;
    };

    /// Takes the attachments freed since the last call, detached or past
    /// their grace, so what keys on them forgets them: a pane's geometry, and
    /// a paste the client will never finish, whose bracket is closed.
    ///
    /// - Returns: The attachments, in the order they were freed.
    [[nodiscard]] std::vector<Freed> takeFreed();

    /// When the next `Lost` attachment's grace ends.
    [[nodiscard]] std::optional<std::uint64_t> nextExpiryMicroseconds() const;

    /// An attachment, or null.
    ///
    /// - Parameter attachmentID: The attachment.
    /// - Returns: The record.
    [[nodiscard]] const AttachmentRecord* find(ID128 attachmentID) const;

    /// The attachments that hold any of the panes, in the order they were
    /// made: what a client showing those panes is told exists.
    ///
    /// - Parameter paneIDs: The panes.
    /// - Returns: The records, each at its stable address.
    [[nodiscard]] std::vector<const AttachmentRecord*> holding(std::span<const ID128> paneIDs) const;
    /// Offers carried face facts for a pane while its driving attachments
    /// have not republished after handover. The caller's span bounds the work.
    [[nodiscard]] std::size_t inheritedFaceOffers(ID128 paneID, std::span<const AttachmentRecord*> output) const;
    /// Finds granted records that can resume any named pane.
    ///
    /// A pane unbound from the live stream still has retained baselines and
    /// can be named by a resume, so authorization withdrawal includes it.
    ///
    /// - Parameter paneIDs: The panes whose retained authority is selected.
    /// - Returns: Borrowed records, valid until the table is mutated.
    [[nodiscard]] std::vector<const AttachmentRecord*> retainingPanes(std::span<const ID128> paneIDs) const;

    /// Moves whenever an attachment is continued, lost or freed, or takes or
    /// gives up a pane, so a connection knows the list it told its client
    /// may have changed. A new attachment shows nobody anything until it
    /// takes a pane.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

    /// Notes that an attachment bound or unbound a pane, which the clients
    /// that show the pane are told.
    void noteHoldingChanged() { ++m_revision; }

    /// Draws the revision of the next `attachments` notify. It is the
    /// table's, so what a client is sent only grows across its connections.
    ///
    /// - Returns: The revision, from 1.
    [[nodiscard]] std::uint64_t nextNotifyRevision() { return ++m_notifyRevision; }

    /// This host's instance.
    [[nodiscard]] ID128 hostInstanceID() const { return m_hostInstanceID; }

private:
    [[nodiscard]] AttachmentRecord* findMutable(ID128 attachmentID);

    /// Creates one identity at the requested admission boundary.
    /// - Parameters:
    ///   - clientInstance: The client's instance.
    ///   - kind: Its kind.
    ///   - authority: Its authenticated connection authority.
    ///   - connection: Its physical connection owner.
    ///   - now: The clock for Lost-record expiry.
    ///   - admission: Whether the required consent has been granted.
    /// - Returns: The stable new record.
    AttachmentRecord& createRecord(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now, AttachmentAdmission admission);

    /// Frees older Lost records only after their replacement has been admitted.
    /// - Parameter admitted: The admitted replacement, whose address stays stable.
    void supersedeLostRecords(const AttachmentRecord& admitted);

    /// Whether two records are one client's: the same instance, under the
    /// same principal. The instance is the client's own word, so it counts
    /// only between attachments one principal authenticated for.
    ///
    /// - Parameters:
    ///   - one: A record.
    ///   - other: Another.
    /// - Returns: Whether they are distinct records of one client.
    [[nodiscard]] static bool isSameClient(const AttachmentRecord& one, const AttachmentRecord& other);

    ID128 m_hostInstanceID;
    ResumePolicy m_policy;
    SecretSource& m_secrets;
    std::uint64_t m_nextAttachment = 1;
    std::vector<ID128> m_inheritedInstances;
    std::vector<std::unique_ptr<AttachmentRecord>> m_records;
    std::vector<std::uint64_t> m_superseded;
    /// The owner releases pane resources before a record is erased.
    RetireRecord m_retireRecord;
    std::vector<Freed> m_freed;
    std::uint64_t m_revision = 0;
    std::uint64_t m_notifyRevision = 0;
};

} // namespace Ubiquitty::Protocol::Sync
