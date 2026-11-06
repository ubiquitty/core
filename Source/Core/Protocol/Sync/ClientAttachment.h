// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// A client's attachment as the client keeps it across its connections
/// (Docs/sync.md §6.7 "Exactly-once across a drop", §6.9).
///
/// Each connection is one `ClientConnection`; the attachment outlives it. It
/// holds the panes' replicas, the resume it greets the next connection with,
/// and every input event the host has not yet reported written:
///
/// - Events are numbered per pane from 1 across connections. A `Welcome` that
///   reports the host's `inputWritten` for a pane resends only what lies
///   above it, under the numbers the events first had, and later events
///   follow the higher of the two counts.
/// - A `Welcome` that reports no value for a pane leaves in doubt the
///   unreported events a connection took, and the person decides: resend
///   them, still under their numbers so the host writes none twice, or
///   discard them. Until then the pane's new input waits behind them, so no
///   later event is written first: the host's count never passes an event in
///   doubt, and its ledger, which keeps the highest number written, tells a
///   written event from one that was not.
/// - An event no connection took cannot have reached the host, so it is
///   never in doubt and never counted written: it goes when a connection
///   carries the pane, numbered past anything the host reports written.
/// - Input no connection carries, including input held for a configured
///   resume store's commit, queues at most 64 KiB; past it an event is refused.
/// - A pane that ended takes its unwritten input with it, counted: input
///   meant for a program that is gone never reaches another. A host that
///   restarted unplanned ends every pane: its replica stays, read-only, and
///   the attachment starts fresh.

#include "Base/ActiveObjectUse.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Protocol/Sync/ResumeRecord.h"
#include "Core/Protocol/Sync/Topology.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Where a client's attachment stands.
enum class ClientAttachmentState : std::uint8_t {
    /// A connection was made and has not been welcomed.
    Connecting,
    /// Welcomed without authority while its exact consent question is pending.
    AwaitingConsent,
    /// A connection is attached.
    Live,
    /// The last connection's transport went; input queues.
    Lost,
};

/// The input local to the client that a pane may hold while no connection
/// carries it (Docs/sync.md §7.9).
inline constexpr std::size_t maxLostInputBytes = std::size_t { 64 } << 10;

/// One input event the host has not reported written.
struct PendingInput {
    ID128 paneID;
    std::uint64_t inputSeq = 0;
    InputIntent intent;
    /// A paste an interrupt gave up: what is sent again is its abort, never
    /// its text.
    bool isAborted = false;
};

/// Why an event was not taken.
enum class InputRefusal : std::uint8_t {
    /// No connection carries it, and the queue is full.
    LostQueueFull,
    /// The pane ended.
    PaneEnded,
    /// It is not one numbered frame: focus, an empty or oversized batch.
    Unsendable,
};

/// What the person is told of a pane's input. The attachment decides it; a
/// frontend shows it, and offers to resend or discard while a decision is
/// awaited.
struct PaneInputStatus {
    /// Events whose fate the host could not report.
    std::size_t inDoubt = 0;
    /// New events waiting behind them, or for a connection.
    std::size_t waiting = 0;
    /// Events discarded: at the person's word, with the pane, or by the
    /// program's terminal when a signal character discarded the input
    /// waiting ahead of it.
    std::size_t discarded = 0;

    /// Whether the person must resend or discard the events in doubt before
    /// the pane's input moves.
    [[nodiscard]] bool isAwaitingDecision() const { return inDoubt > 0; }
};

/// How a connection's `Greet` proves the client holds the attachment it
/// resumes.
enum class ResumeCarrier : std::uint8_t {
    /// The `Greet` carries the resume secret: SSH stdio, inside the channel.
    Secret,
    /// A resume proof admitted the connection (`ClientAttachment::resumeProof`),
    /// so the `Greet` carries no secret: QUIC, where it is never sent.
    Proof,
};

/// What a client holds of an attachment, to continue it on another
/// connection: what the last `Welcome` issued and how far it numbered each
/// pane's input.
struct HeldAttachment {
    ID128 hostInstanceID;
    ID128 attachmentID;
    ID128 resumeSecret;

    /// How far one pane's input was numbered.
    struct Pane {
        ID128 paneID;
        std::uint64_t lastInputSeq = 0;
    };
    std::vector<Pane> panes;
};

/// A client's attachment across its connections: the replicas, the resume,
/// and the input the host has not reported written. One thread drives it.
///
/// With a store it keeps the window's resume record by the record's rules
/// (Docs/sync.md §6.9): written after every `Welcome` before any input goes
/// on the new connection, when the transport goes, and when its owner asks;
/// purged at a `HistoryReset` before anything else is applied.
class ClientAttachment final : private HistoryPurgeSink, private EffectReceiptSink, private ClientWelcomeSink {
public:
    /// Creates an attachment with no connection.
    ///
    /// - Parameters:
    ///   - policy: Each connection's policy.
    ///   - liveness: How its liveness is judged.
    ///   - identity: The client; `resume` is the attachment's to set.
    ///   - delegate: What learns of each connection's panes, or null.
    ///   - store: Where the window's resume record is kept, or null for a
    ///     client that keeps none; borrowed, and it must outlive the
    ///     attachment.
    ///   - identitySink: A non-reentrant carrier identity barrier, checked before
    ///     a store or replica changes; borrowed for the attachment's lifetime.
    ClientAttachment(const ClientPolicy& policy, const LivenessPolicy& liveness, ClientIdentity identity, ClientPaneDelegate* delegate = nullptr, ResumeRecordStore* store = nullptr, ClientWelcomeSink* identitySink = nullptr);

    ClientAttachment(const ClientAttachment&) = delete;
    ClientAttachment& operator=(const ClientAttachment&) = delete;

    /// Continues an attachment a client held before, as one restored from a
    /// record: the next connection greets with a resume of it.
    ///
    /// - Parameter held: The attachment.
    void restore(const HeldAttachment& held);

    /// What the client holds of the attachment, absent before a `Welcome`.
    [[nodiscard]] std::optional<HeldAttachment> held() const;

    /// Continues from the stored record, as a relaunched process does before
    /// it connects: the client is the record's `clientInstance`, the next
    /// connection resumes the record's attachment, each pane whose stored
    /// picture verifies is drawn at once, and the record's unsent input
    /// becomes drafts. A picture whose body does not verify is not drawn, and
    /// the host checkpoints that pane.
    ///
    /// - Returns: Whether a record was stored and no retirement is pending.
    bool restoreFromStore();

    /// Writes the record as the attachment stands, as a client does at its
    /// background deadline. Nothing is written before a `Welcome`.
    /// A successful retry after a refused write permits the next input flush;
    /// input waits within its lost-queue bound until any pending purge and
    /// the replacement commit both succeed.
    void persist();

    /// Tells the host the client is about to be suspended, as at its
    /// background deadline or when protected data becomes unavailable: every
    /// pane it shows goes `Dormant`, the record is written, and only then
    /// `Bye{Suspending}` is sent and the connection closed, so the record
    /// never holds less than the host was told. The host holds the
    /// attachment `Lost` at once; the size the client owned stays its own.
    /// The owner takes the connection's last output, then drops the
    /// transport.
    void suspend();

    /// The attachments that share a pane with this one, as the host last
    /// listed them: which exist, what each is, and which owns the size and
    /// is the primary. A frontend says from it why a picture is letterboxed
    /// or silent, and that typing here takes the size. The list a lost
    /// connection left stands until the next connection's first.
    [[nodiscard]] std::span<const AttachmentEntry> attachments() const { return m_attachments.attachments; }

    /// The revision of the list held; 0 before the connection's first.
    [[nodiscard]] std::uint64_t attachmentsRevision() const { return m_attachments.revision; }

    /// The newest topology tree the host sent on this connection, absent
    /// before one.
    [[nodiscard]] const std::optional<TopologyTree>& topology() const { return m_topology; }

    /// Notes the topology of the tab the client draws, read from the
    /// structure the host sent for its workspace.
    ///
    /// - Parameter tree: The tree; one older than the tree held is dropped.
    void noteTopology(TopologyTree tree);

    /// Where to lay a pane out, and whether its picture fills that room
    /// (Docs/sync.md §6.10 "Topology never overtakes content"). A leaf's
    /// size applies only once the picture held is at least the leaf's
    /// generation and is that size; until then the pane keeps its picture's
    /// size, letterboxed. A rectangle that is not letterboxed is always the
    /// size of the grid drawn in it.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The rectangle, or `nullopt` for a pane with no picture or
    ///   no leaf in the tree held.
    [[nodiscard]] std::optional<PaneRectangle> paneRectangle(ID128 paneID) const;

    /// Notes an attachments list the host sent (`readNotifies`).
    ///
    /// - Parameter list: The list; one older than the list held is dropped.
    void noteAttachments(AttachmentsList list);

    /// Takes the host's `Notify`s since the last call, in arrival order, for
    /// `readNotifies` to read.
    [[nodiscard]] std::vector<Notify> takeNotifies();

    /// The input a restored record held unsent, oldest first. It is shown to
    /// the person and never sent: the foreground program may have changed
    /// since it was typed.
    [[nodiscard]] std::span<const ResumeDraft> drafts() const { return m_drafts; }

    /// Forgets the drafts, as the person dismissed them or typed them again.
    void discardDrafts();

    /// Sets the pin of the certificate the transport reaches the host under,
    /// which the record keeps for the relaunched process.
    ///
    /// - Parameter pin: The pin.
    void setCertificatePin(const std::array<std::uint8_t, 32>& pin) { m_certificatePin = pin; }

    /// The certificate pin set, or restored from the record; zero when none.
    [[nodiscard]] const std::array<std::uint8_t, 32>& certificatePin() const { return m_certificatePin; }

    /// The client instance the attachment greets as: the record's once one
    /// was restored.
    [[nodiscard]] ID128 clientInstance() const { return m_identity.clientInstance; }

    /// Makes a connection, greeting with a resume of the held attachment, or
    /// fresh when none is held. A connection still open is dropped first.
    ///
    /// - Parameters:
    ///   - now: The time.
    ///   - carrier: How the resume is proved. With `Proof` the transport has
    ///     presented `resumeProof` in its preamble, and the `Greet` names the
    ///     attachment without its secret.
    /// - Returns: The connection, which lives until the next `connect`.
    ClientConnection& connect(std::uint64_t now, ResumeCarrier carrier = ResumeCarrier::Secret);

    /// The proof that continues the held attachment over a connection with
    /// this exporter, in place of a bootstrap token (Docs/sync.md §6.9
    /// "Resume without SSH").
    ///
    /// - Parameter exporter: The new connection's TLS keying-material
    ///   exporter.
    /// - Returns: The proof, or `nullopt` when no attachment is held: a
    ///   fresh attach goes through SSH.
    [[nodiscard]] std::optional<ResumeProof> resumeProof(std::span<const std::uint8_t, 32> exporter) const;

    /// The connection, or null before the first `connect`.
    [[nodiscard]] ClientConnection* connection() { return m_connection.get(); }

    /// Keeps the current reader alive through callbacks dispatched by its owner.
    /// The attachment must outlive this scope; a connection must exist on entry.
    [[nodiscard]] ActiveObjectUse<ClientConnection> useConnection()
    {
        assert(m_connection);
        return { m_activeConnectionUse, *m_connection };
    }

    /// Reads one datagram of the connection's, then acts on what it brought.
    ///
    /// - Parameters:
    ///   - datagram: Exactly one envelope.
    ///   - now: The time.
    void receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now);

    /// Sets what the next connection's `Greet` says of the client: its
    /// viewport, focus, link and power as they stand now, not as they stood
    /// when the attachment was made.
    ///
    /// - Parameter facts: The facts.
    void setFacts(const Facts& facts) { m_identity.facts = facts; }

    /// Supplies the next physical greeting's intent and credential without persisting either.
    /// - Parameters:
    ///   - request: The exact session, workspace or host-control selection.
    ///   - credential: The bounded bearer bytes, absent for transport-authenticated peers.
    void setAdmission(AdmissionRequest request, std::optional<std::vector<std::uint8_t>> credential)
    {
        m_identity.request = std::move(request);
        m_identity.credential = std::move(credential);
    }

    /// The facts the next connection greets with.
    [[nodiscard]] const Facts& facts() const { return m_identity.facts; }

    /// The kind of client the attachment greets as.
    [[nodiscard]] ClientKind kind() const { return m_identity.kind; }

    /// Sets the largest datagram the next connection's carrier sends, 0 for
    /// a carrier without datagrams: one attachment crosses carriers.
    ///
    /// - Parameter bytes: The size.
    void setDatagramMaxBytes(std::size_t bytes) { m_policy.datagramMaxBytes = bytes; }

    /// Names a capability in the next connection's Greet, for one that
    /// follows the carrier and its observer rather than the attachment.
    ///
    /// - Parameter capability: The capability.
    void addCapability(ClientCapability capability) { m_policy.capabilities.add(capability); }

    /// Leaves a capability out of the next connection's Greet.
    ///
    /// - Parameter capability: The capability.
    void removeCapability(ClientCapability capability) { m_policy.capabilities.remove(capability); }

    /// Reports that the connection's transport went. Its replicas are kept,
    /// and events not reported written wait for the next connection.
    ///
    /// - Parameter now: The time.
    void transportLost(std::uint64_t now);

    /// Reads bytes from the connection's stream, then acts on what they
    /// brought: a `Welcome`, receipts, a pane bound.
    ///
    /// - Parameters:
    ///   - bytes: The bytes.
    ///   - now: The time.
    void receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now);

    /// Runs the connection's timers. A connection whose liveness probe went
    /// unanswered is dropped as a lost transport, and the attachment wants a
    /// redial.
    ///
    /// - Parameter now: The time.
    void tick(std::uint64_t now);

    /// Whether the transport was found dead and the owner should connect
    /// again at once; false again after `connect`.
    [[nodiscard]] bool wantsRedial() const { return m_wantsRedial; }

    /// Whether the last welcomed host requested a planned handover. The
    /// carrier must rediscover its successor rather than retrying a pinned
    /// predecessor socket; ordinary transport loss keeps that pin.
    [[nodiscard]] bool wantsHandoverRedial() const
    {
        return m_wantsRedial && m_retired && m_retired->welcome()
            && m_retired->byeReason() == ByeReason::HandingOver;
    }

    /// Probes the host before the client trusts the connection again: when
    /// the app returns to the foreground, or on an OS network-path event.
    /// Only the probe's own `Echo` answers it
    /// (`ClientConnection::isLivenessUnproven`), and the owner times the
    /// silence.
    ///
    /// - Parameter now: The time.
    void checkLiveness(std::uint64_t now);

    /// The link as the frontend shows it; nothing stalled while no connection
    /// is attached, when `state` says `Lost` instead.
    ///
    /// - Parameter now: The time.
    /// - Returns: The link's state.
    [[nodiscard]] LinkState linkState(std::uint64_t now) const;

    /// Takes what the connection has to send.
    [[nodiscard]] std::vector<std::uint8_t> takeStreamOutput();

    /// Numbers an input event for a pane and sends it, or queues it while no
    /// connection carries it or its configured store has not committed the
    /// current resume secret. An interrupt gives up the pane's unfinished
    /// paste first: its pieces not sent are dropped, a `PasteAbort` goes
    /// ahead of the event, and no later connection sends the paste again.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - intent: The event.
    ///   - now: The time.
    /// - Returns: Its number, or why it was not taken.
    [[nodiscard]] std::expected<std::uint64_t, InputRefusal> send(ID128 paneID, InputIntent intent, std::uint64_t now);

    /// The events of a pane whose fate the host could not report, oldest
    /// first.
    ///
    /// - Parameter paneID: The pane.
    [[nodiscard]] std::span<const PendingInput> inDoubt(ID128 paneID) const;

    /// Sends a pane's events in doubt, as the person asked, under the
    /// numbers they had and ahead of the input waiting behind them: the host
    /// writes each that it had not written, once.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - now: The time.
    void resendInDoubt(ID128 paneID, std::uint64_t now);

    /// Discards a pane's events in doubt, as the person asked, counting them,
    /// and lets the input waiting behind them go.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - now: The time.
    void discardInDoubt(ID128 paneID, std::uint64_t now);

    /// What the person is told of a pane's input.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The status; all zero for a pane not known.
    [[nodiscard]] PaneInputStatus inputStatus(ID128 paneID) const;

    /// Sets how much of a pane the client wants, which outlives the
    /// connection: a resume names it, so a pane kept hidden costs nothing
    /// when the attachment continues (Docs/sync.md §6.8).
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - view: The view.
    void setView(ID128 paneID, ViewState view);

    /// Sets who lends the terminals later connections draw panes into; null
    /// leaves them to the attachment.
    ///
    /// - Parameter delegate: The delegate, which outlives the attachment's
    ///   use of it.
    void setPaneDelegate(ClientPaneDelegate* delegate) { m_delegate = delegate; }

    /// The view the client set for a pane, or `nullopt` when it set none and
    /// the connection's own applies.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The view.
    [[nodiscard]] std::optional<ViewState> view(ID128 paneID) const;

    /// The events of a pane not yet reported written and not in doubt.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: How many.
    [[nodiscard]] std::size_t pendingCount(ID128 paneID) const;

    /// The picture of a live pane: the same terminal on every connection.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The terminal, or null for a pane not held.
    [[nodiscard]] const Ubiquitty::Terminal* paneTerminal(ID128 paneID) const { return m_replicas.terminal(paneID); }

    /// Takes back a terminal the client lent: no held picture is read from
    /// it again; see ``ReplicaSet/forgetLent(_:)``.
    ///
    /// - Parameter terminal: The lent terminal.
    void forgetLentTerminal(const Ubiquitty::Terminal& terminal) { m_replicas.forgetLent(terminal); }

    /// The verified cold picture, before any connection starts carrying it.
    [[nodiscard]] std::optional<std::uint64_t> restoredPictureGeneration(ID128 paneID) const;

    /// Projects retained history into an independent preview without changing
    /// the pane's screen or advertising a network acknowledgement.
    /// - Parameters:
    ///   - paneID: The stable pane whose history is retained.
    ///   - destination: The preview terminal, with its screen already applied.
    void copyHistoryTo(ID128 paneID, Ubiquitty::Terminal& destination) const;

    /// The panes that ended, read-only, each labelled with its instance.
    [[nodiscard]] std::span<const ReplicaSet::Ended> endedPanes() const { return m_replicas.ended(); }

    /// Whether a pane ended.
    ///
    /// - Parameter paneID: The pane.
    [[nodiscard]] bool isEnded(ID128 paneID) const;

    /// Freezes panes the host reports gone or whose authenticated replacement
    /// fails continuity admission. Pending input is discarded, pictures remain
    /// available read-only, and the stored resume authority is retired.
    void endHost();

    [[nodiscard]] ClientAttachmentState state() const { return m_state; }

    /// The rules its connections' liveness is judged by.
    [[nodiscard]] const LivenessPolicy& liveness() const { return m_liveness; }

private:
    /// Which connection took an event the host has not reported written.
    enum class SentOn : std::uint8_t {
        /// None did, so the host cannot have it.
        NoConnection,
        /// One before the current connection: only a count from the host
        /// says whether it was written.
        EarlierConnection,
        /// The current connection.
        CurrentConnection,
    };

    /// An event not reported written.
    struct Queued {
        PendingInput input;
        SentOn sentOn = SentOn::NoConnection;
    };

    /// A pane's input across connections.
    struct HeldPane {
        /// A cold picture's local dictionary differs from the host's retained
        /// dictionary. This digest checks the lent picture before its first bind.
        std::optional<Digest128> restoredPictureDigest;
        ID128 paneID;
        /// The highest number given or reported.
        std::uint64_t lastInputSeq = 0;
        /// Events not reported written, oldest first.
        std::vector<Queued> pending;
        std::vector<PendingInput> inDoubt;
        std::size_t discarded = 0;
        /// The newest event the host reported dropped unwritten.
        std::uint64_t discardedThrough = 0;
        /// The view the client set, kept across connections.
        std::optional<ViewState> view;
        bool hasEnded = false;
        /// What the pane's line discipline signals on, as its echo context
        /// last said, kept while no connection carries the pane.
        InterruptCharacters interruptCharacters = InterruptCharacters::terminalDefaults();
        /// The generation the replica held when a `HistoryReset` purged the
        /// record: the record takes no picture of the pane until a newer
        /// one, which is the screen after the clear.
        std::optional<std::uint64_t> pictureWithheldAt;
    };

    /// Purges the record a `HistoryReset` retires an epoch of, before the
    /// connection applies anything else.
    ///
    /// - Parameters:
    ///   - retired: The key the pane's history held before the reset.
    ///   - clearedThrough: Every line below it is gone.
    /// - Returns: Refusal keeps input, restore, and replacement writes gated.
    [[nodiscard]] HistoryPurgeOutcome purge(const HistoryKey& retired, std::uint64_t clearedThrough) override;

    /// Withdraws input permission and retires the stored record. A refusal
    /// remains pending until a later retry confirms retirement.
    ///
    /// - Returns: Whether the store confirms retirement.
    [[nodiscard]] bool purgeRecord();

    /// Writes the record as the attachment stands. A record whose pictures
    /// pass the checkpoint cap is not written, and the stored one is purged:
    /// it holds a secret a `Welcome` may have consumed. Refusal or purge
    /// prevents new numbered input from flushing until retirement and a
    /// replacement commit succeed.
    ///
    /// - Parameter receipt: A prospective receipt not yet published to replicas.
    void writeRecord(std::optional<std::pair<ID128, EffectReceiptState>> receipt = std::nullopt);

    /// Persists a receipt before its effect or complete-batch ACK can leave.
    ///
    /// - Parameters:
    ///   - paneID: The connection's exact stable pane binding.
    ///   - state: The candidate high-water marks.
    /// - Returns: Whether the store committed, or no store is configured.
    [[nodiscard]] bool commitEffectReceipt(ID128 paneID, EffectReceiptState state) override;

    /// A pane's input, made when the pane is new.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: Its input, its address stable until a pane is added.
    [[nodiscard]] HeldPane& heldPane(ID128 paneID);

    /// A pane's input, or null for a pane not known.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: Its input.
    [[nodiscard]] const HeldPane* findHeldPane(ID128 paneID) const;

    /// Acts on what the connection learnt: the `Welcome`, receipts, panes
    /// bound; then sends what waits.
    ///
    /// - Parameter now: The time.
    void settle(std::uint64_t now);

    /// Applies a `Welcome`: the host's count for each pane, and an unplanned
    /// restart.
    ///
    /// - Parameter welcome: The connection's `Welcome`.
    void applyWelcome(const Welcome& welcome);
    /// Admits the exact store identity before the connection applies Welcome.
    ///
    /// - Parameter welcome: The validated authenticated peer identity.
    /// - Returns: Whether storage binding succeeded, or no store is configured.
    [[nodiscard]] bool acceptWelcome(const Welcome& welcome) override;

    /// Drops a pane's events at or below what the host wrote.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - inputWritten: The host's count.
    void noteWritten(HeldPane& pane, std::uint64_t inputWritten);

    /// Counts a run of a pane's events the host dropped unwritten, and drops
    /// them here. Every number in the run is one of this client's events, so
    /// the run's length is the count whether or not a count that passed them
    /// arrived first; a run told twice, or overlapping one told, counts once.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - run: The run.
    void noteDiscarded(HeldPane& pane, const InputDiscarded& run);

    /// Gives up a pane's pastes not reported written, ahead of an interrupt.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - handle: Its handle on the connection, or 0 when none carries it.
    void abortPastes(HeldPane& pane, std::uint32_t handle);

    /// Moves into doubt every unreported event of a pane that a connection
    /// took; what none took stays queued behind them.
    ///
    /// - Parameter pane: The pane.
    static void moveToDoubt(HeldPane& pane);

    /// Discards every unwritten event of a pane that ended, counting them on
    /// the pane and on its ended replica.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - hostInstanceID: The instance whose pane it was.
    void discardWithPane(HeldPane& pane, ID128 hostInstanceID);

    /// Sends what waits for a pane the connection has bound, and starts the
    /// connection's wait for its receipt.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The time.
    void flush(HeldPane& pane, std::uint64_t now);

    /// The bytes queued and not sent.
    [[nodiscard]] std::size_t unsentBytes() const;

    ClientPolicy m_policy;
    LivenessPolicy m_liveness;
    ClientIdentity m_identity;
    ClientPaneDelegate* m_delegate = nullptr;
    ClientWelcomeSink* m_identitySink = nullptr;
    ReplicaSet m_replicas;
    std::unique_ptr<ClientConnection> m_connection;
    /// The connection the last lost transport left, kept until the next is
    /// lost: a transport may be dropped from inside that connection's own
    /// read, by a handler of a frame it delivered, and the read must have a
    /// connection to return into.
    std::unique_ptr<ClientConnection> m_retired;
    /// Read/timer scopes retain a replaced connection until its outermost
    /// callback returns, including more than one synchronous reconnect.
    ActiveObjectUse<ClientConnection>* m_activeConnectionUse = nullptr;
    ClientAttachmentState m_state = ClientAttachmentState::Lost;
    std::optional<HeldAttachment> m_held;
    bool m_isWelcomeApplied = false;
    bool m_wantsRedial = false;
    std::vector<HeldPane> m_panes;
    AttachmentsList m_attachments;
    std::optional<TopologyTree> m_topology;
    /// Whether this connection has sent a tree: its first replaces the one a
    /// lost connection left, whatever its revision.
    bool m_hasConnectionsTopology = false;
    ResumeRecordStore* m_store = nullptr;
    std::vector<ResumeDraft> m_drafts;
    std::array<std::uint8_t, 32> m_certificatePin {};
    /// Whether the record was purged inside a read and is owed a rewrite
    /// once the read returns.
    bool m_isRecordStale = false;
    /// Whether the configured store accepted the current resume secret and
    /// has not subsequently been purged or refused a replacement write.
    /// Without a configured store, input does not depend on this latch.
    bool m_hasCommittedRecord = false;
    /// A failed key retirement cannot be hidden by a replacement ciphertext.
    /// Writes and restores wait until the store confirms the purge.
    bool m_hasPendingRecordPurge = false;
    bool m_isStoreBindingRefused = false;
    /// A carrier without a persistent binding keeps receipts only in this
    /// attachment, after retiring the persistent resume secret.
    bool m_isMemoryOnly = false;
};

} // namespace Ubiquitty::Protocol::Sync
