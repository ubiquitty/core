// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/ClientAttachment.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace Ubiquitty::Protocol::Sync {

namespace {

/// Whether an event is one numbered frame.
bool isSendable(const InputIntent& intent)
{
    // The wire has no empty `Raw`, so one is never numbered: no connection
    // would send it, and the pane's later events would wait behind it.
    if (const auto* raw = std::get_if<RawInput>(&intent))
        return !raw->bytes.empty() && raw->bytes.size() < maxFrameBodyBytes;
    if (const auto* keys = std::get_if<KeyInput>(&intent))
        return !keys->events.empty() && keys->events.size() <= maxInputEventsPerFrame;
    if (const auto* pointer = std::get_if<PointerInput>(&intent))
        return !pointer->events.empty() && pointer->events.size() <= maxInputEventsPerFrame;
    // A paste is handed over whole: one final piece from its start.
    if (const auto* paste = std::get_if<PasteInput>(&intent))
        return paste->end == PasteEnd::Final && paste->offset == 0 && paste->text.size() <= maxPasteBytes;
    return false;
}

/// A held picture as its `replica` describes it, for as long as this lives:
/// a status set standing in it for a view that is not live is set aside, and
/// stands again when this ends. What is checkpointed or compared against the
/// replica is then the screen the host diffs from.
class ScreenPicture {
public:
    explicit ScreenPicture(ReplicaSet::Held& held)
        : m_held(held)
    {
        exchangeStatus();
    }
    ~ScreenPicture() { exchangeStatus(); }
    ScreenPicture(const ScreenPicture&) = delete;
    ScreenPicture& operator=(const ScreenPicture&) = delete;

    /// The picture.
    [[nodiscard]] const Ubiquitty::Terminal& terminal() const { return *m_held.picture(); }

private:
    void exchangeStatus()
    {
        if (m_held.screenStatus)
            std::swap(m_held.picture()->programStatus, *m_held.screenStatus);
    }

    ReplicaSet::Held& m_held;
};

} // namespace

ClientAttachment::ClientAttachment(const ClientPolicy& policy, const LivenessPolicy& liveness, ClientIdentity identity, ClientPaneDelegate* delegate, ResumeRecordStore* store, ClientWelcomeSink* identitySink)
    : m_policy(policy)
    , m_liveness(liveness)
    , m_identity(std::move(identity))
    , m_delegate(delegate)
    , m_identitySink(identitySink)
    , m_store(store)
{
    m_identity.resume.reset();
}

void ClientAttachment::restore(const HeldAttachment& held)
{
    m_hasCommittedRecord = false;
    m_held = HeldAttachment { .hostInstanceID = held.hostInstanceID, .attachmentID = held.attachmentID, .resumeSecret = held.resumeSecret, .panes = {} };
    for (const HeldAttachment::Pane& pane : held.panes) {
        HeldPane& input = heldPane(pane.paneID);
        input.lastInputSeq = std::max(input.lastInputSeq, pane.lastInputSeq);
    }
}

bool ClientAttachment::restoreFromStore()
{
    if (m_hasPendingRecordPurge || m_isStoreBindingRefused || m_isMemoryOnly || m_connection || m_held)
        return false;
    std::optional<ResumeRecord> record = m_store ? m_store->read() : std::nullopt;
    if (!record || record->panes.size() > maxPanesPerConnection)
        return false;
    // Stores are not all wire decoders. Each destination must have one owner
    // before preparing pictures or consulting a frontend delegate.
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        if (record->panes[index].paneID == ID128 {} || record->panes[index].effects.consumed < record->panes[index].effects.acknowledged)
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (record->panes[prior].paneID == record->panes[index].paneID)
                return false;
        }
    }
    struct PreparedPane {
        std::optional<PreparedCheckpoint> checkpoint;
        Ubiquitty::Terminal* destination = nullptr;
        std::unique_ptr<Ubiquitty::Terminal> owned;
        std::shared_ptr<HistoryCache> history;
    };
    // A sparse checkpoint can describe a much larger grid than its encoded
    // body. Admit the sum of both-screen decoded cells before preparing any.
    std::size_t cellBytes = 0;
    for (const ResumeRecordPane& stored : record->panes) {
        if (stored.checkpoint.empty())
            continue;
        const auto bytes = checkpointCellStorageBytes(stored.checkpoint);
        if (!bytes || *bytes > m_policy.maxDecodedScreenCellBytes || *bytes > m_policy.maxCheckpointBytes - cellBytes)
            return false;
        cellBytes += *bytes;
    }
    std::unique_ptr<PreparedPane[]> prepared(new (std::nothrow) PreparedPane[record->panes.size()]);
    if (!prepared)
        return false;
    std::size_t retainedHistoryBytes = cellBytes;
    std::size_t encodedHistoryBytes = 0;
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        const auto& stored = record->panes[index];
        if (stored.history.empty() || m_store->historyPersistence() != HistoryPersistence::Enabled)
            continue;
        if (stored.history.size() > m_policy.maxCheckpointBytes - encodedHistoryBytes)
            return false;
        encodedHistoryBytes += stored.history.size();
        auto cache = std::make_shared<HistoryCache>(record->hostInstanceID, stored.paneID, m_policy.historyCacheLines, m_policy.historyCacheBytes);
        if (!cache->restoreSnapshot(stored.history, m_policy.maxCheckpointBytes, m_policy.maxCheckpointBytes - retainedHistoryBytes)
            || (!stored.checkpoint.empty() && (cache->key().epoch != stored.replica.historyEpoch || cache->head() != stored.replica.historyHead)))
            return false;
        retainedHistoryBytes += cache->retainedStorageBytes();
        prepared[index].history = std::move(cache);
    }
    // Every restored picture and cache is a retained replica: their sum is
    // admitted against the attachment's replica allowance, all or nothing.
    std::size_t replicaBytes = m_replicas.retainedBytes();
    const auto admitReplica = [&](std::size_t bytes) {
        if (replicaBytes > m_policy.maxRetainedReplicaBytes || bytes > m_policy.maxRetainedReplicaBytes - replicaBytes)
            return false;
        replicaBytes += bytes;
        return true;
    };
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        if (prepared[index].history && !admitReplica(prepared[index].history->retainedStorageBytes()))
            return false;
    }
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        const ResumeRecordPane& stored = record->panes[index];
        if (stored.checkpoint.empty() || digestOf(stored.checkpoint) != stored.checkpointDigest)
            continue;
        auto& pane = prepared[index];
        pane.destination = m_delegate ? m_delegate->terminalForPane(stored.paneID) : nullptr;
        if (!pane.destination) {
            pane.owned.reset(new (std::nothrow) Ubiquitty::Terminal);
            pane.destination = pane.owned.get();
        }
        if (!pane.destination)
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (prepared[prior].destination == pane.destination)
                return false;
        }
        const auto plan = checkpointStoragePlan(stored.checkpoint);
        if (!plan) {
            if (plan.error() == ApplyOutcome::AllocationRefused)
                return false;
            pane.destination = nullptr;
            pane.owned.reset();
            continue;
        }
        const auto cost = checkpointStorageCost(*plan, *pane.destination);
        if (!cost || *cost > m_policy.maxCheckpointStorageCost)
            return false;
        if (!pane.destination->grid)
            return false;
        const auto backing = pane.destination->grid->backingStoragePlan(plan->rows, plan->columns);
        if (!backing || !admitReplica(backing->retainedBytes))
            return false;
    }
    // Every destination and cost is checked before materializing any picture.
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        auto& pane = prepared[index];
        if (!pane.destination)
            continue;
        auto checkpoint = prepareCheckpoint(record->panes[index].checkpoint, *pane.destination,
            { .maxDecodedScreenCellBytes = m_policy.maxDecodedScreenCellBytes, .maxStorageCost = m_policy.maxCheckpointStorageCost });
        if (!checkpoint)
            return false;
        pane.checkpoint = std::move(*checkpoint);
    }
    // Capacity changes preserve the displayed cells. No pane is published
    // unless every destination can hold its complete prepared picture.
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        auto& pane = prepared[index];
        if (pane.destination && reserveCheckpointDestination(*pane.destination, *pane.checkpoint) != ApplyOutcome::Applied)
            return false;
    }
    ReplicaSet replicas;
    (void)replicas.noteHostInstance(record->hostInstanceID, std::nullopt);
    std::vector<HeldPane> panes;
    panes.reserve(record->panes.size());
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        ResumeRecordPane& stored = record->panes[index];
        panes.emplace_back();
        auto& input = panes.back();
        input.paneID = stored.paneID;
        input.lastInputSeq = stored.lastInputSeq;
        input.view = stored.view;
        if (!replicas.keepEffectReceipt(stored.paneID, stored.effects))
            return false;
        auto& pane = prepared[index];
        if (pane.history && !replicas.keepHistory(pane.history))
            return false;
        if (!pane.destination)
            continue;
        ReplicaSet::Held& held = pane.owned ? replicas.hold(stored.paneID, std::move(pane.owned)) : replicas.holdLent(stored.paneID, *pane.destination);
        held.replica = std::move(stored.replica);
        held.replica.scratch.reset();
        held.replica.echoContext.reset();
        held.hasCheckpoint = true;
    }
    for (std::size_t index = 0; index < record->panes.size(); ++index) {
        auto& pane = prepared[index];
        if (!pane.destination)
            continue;
        ScreenReplica drawn;
        [[maybe_unused]] const auto outcome = commitCheckpoint(*pane.destination, std::move(*pane.checkpoint), drawn);
        assert(outcome == ApplyOutcome::Applied);
        pane.destination->replyAuthority = ReplyAuthority::Elsewhere; // its host answers
        if (pane.history)
            pane.history->project(*pane.destination, pane.destination->columns);
        panes[index].restoredPictureDigest = replicaDigest(drawn);
    }
    m_hasCommittedRecord = false;
    m_identity.clientInstance = record->clientInstance;
    m_certificatePin = record->certificatePin;
    m_replicas = std::move(replicas);
    m_panes = std::move(panes);
    m_held = HeldAttachment { .hostInstanceID = record->hostInstanceID, .attachmentID = record->attachmentID, .resumeSecret = record->resumeSecret, .panes = {} };
    m_drafts = std::move(record->drafts);
    return true;
}

void ClientAttachment::persist()
{
    writeRecord();
}

std::optional<std::uint64_t> ClientAttachment::restoredPictureGeneration(ID128 paneID) const
{
    if (m_connection)
        return std::nullopt;
    for (const auto& pane : m_panes) {
        if (pane.paneID != paneID || !pane.restoredPictureDigest)
            continue;
        if (const auto* held = m_replicas.find(paneID); held && held->hasCheckpoint)
            return held->replica.generation;
    }
    return std::nullopt;
}

void ClientAttachment::suspend()
{
    if (m_connection && m_state == ClientAttachmentState::Live) {
        // A suspended client draws nothing. The views the client set are
        // kept: the resume names them, and the panes show again.
        for (const auto& [handle, paneID] : m_connection->boundPanes()) {
            if (m_connection->paneView(handle) == std::optional { ViewState::Live })
                m_connection->setView(handle, ViewState::Dormant);
        }
    }
    writeRecord();
    if (m_connection)
        m_connection->leave(ByeReason::Suspending);
    m_state = ClientAttachmentState::Lost;
}

void ClientAttachment::noteTopology(TopologyTree tree)
{
    // The newest tree stands; one a reordering carrier delivers late does not
    // replace it.
    if (m_topology && m_hasConnectionsTopology && tree.revision < m_topology->revision)
        return;
    m_topology = std::move(tree);
    m_hasConnectionsTopology = true;
}

std::optional<PaneRectangle> ClientAttachment::paneRectangle(ID128 paneID) const
{
    const Ubiquitty::Terminal* terminal = paneTerminal(paneID);
    const TopologyNode* leaf = m_topology ? findLeaf(*m_topology, paneID) : nullptr;
    if (!terminal || !leaf)
        return std::nullopt;
    // The connection holds the generation while it carries the pane; the
    // replica set holds what the last connection left.
    std::uint64_t generation = 0;
    if (const std::uint32_t handle = m_connection ? m_connection->handleForPane(paneID) : 0)
        generation = m_connection->paneGeneration(handle);
    else if (const ReplicaSet::Held* held = m_replicas.find(paneID))
        generation = held->replica.generation;
    return paneRectangleFor(*leaf, { .generation = generation, .rows = static_cast<std::uint16_t>(terminal->rows), .columns = static_cast<std::uint16_t>(terminal->columns) });
}

void ClientAttachment::noteAttachments(AttachmentsList list)
{
    // The newest list stands; one a reordering carrier delivers late does not
    // replace it.
    if (list.revision >= m_attachments.revision)
        m_attachments = std::move(list);
}

std::vector<Notify> ClientAttachment::takeNotifies()
{
    return m_connection ? m_connection->takeNotifies() : std::vector<Notify> {};
}

void ClientAttachment::discardDrafts()
{
    m_drafts.clear();
    writeRecord();
}

void ClientAttachment::copyHistoryTo(ID128 paneID, Ubiquitty::Terminal& destination) const
{
    if (const auto history = m_replicas.history(paneID)) {
        history->invalidateProjection();
        history->project(destination, destination.columns);
        history->invalidateProjection();
    }
}

HistoryPurgeOutcome ClientAttachment::purge(const HistoryKey& retired, std::uint64_t)
{
    if (!m_store || m_isMemoryOnly)
        return HistoryPurgeOutcome::Purged;
    // A clear is a purge before anything else is applied: the record goes
    // now, and is written again, without the pane's picture, once the read
    // that carried the reset returns.
    const bool didPurge = purgeRecord();
    m_isRecordStale = true;
    const std::uint32_t handle = m_connection ? m_connection->handleForPane(retired.paneID) : 0;
    heldPane(retired.paneID).pictureWithheldAt = handle != 0 ? m_connection->paneGeneration(handle) : 0;
    return didPurge ? HistoryPurgeOutcome::Purged : HistoryPurgeOutcome::Refused;
}

bool ClientAttachment::purgeRecord()
{
    m_hasCommittedRecord = false;
    m_hasPendingRecordPurge = m_store && !m_isMemoryOnly && m_store->purge() != ResumePurgeOutcome::Purged;
    return !m_hasPendingRecordPurge;
}

bool ClientAttachment::commitEffectReceipt(ID128 paneID, EffectReceiptState state)
{
    if (!m_store || m_isMemoryOnly)
        return true;
    if (!m_connection || !m_connection->welcome() || !m_connection->handleForPane(paneID))
        return false;
    // Welcome and the first effect may share one carrier read, before settle
    // has recorded the rotated secret or discovered the new pane binding.
    if (!m_isWelcomeApplied) {
        m_isWelcomeApplied = true;
        applyWelcome(*m_connection->welcome());
        m_state = ClientAttachmentState::Live;
    }
    for (const auto& [handle, boundPaneID] : m_connection->boundPanes())
        (void)heldPane(boundPaneID);
    writeRecord(std::pair { paneID, state });
    return m_hasCommittedRecord;
}

void ClientAttachment::writeRecord(std::optional<std::pair<ID128, EffectReceiptState>> receipt)
{
    m_isRecordStale = false;
    if (!m_store || m_isMemoryOnly)
        return;
    m_hasCommittedRecord = false;
    if (m_hasPendingRecordPurge && !purgeRecord())
        return;
    if (m_isStoreBindingRefused)
        return;
    if (!m_held)
        return;
    if (m_connection)
        m_connection->storeReplicas();
    ResumeRecord record { .hostInstanceID = m_held->hostInstanceID, .clientInstance = m_identity.clientInstance, .attachmentID = m_held->attachmentID, .resumeSecret = m_held->resumeSecret, .certificatePin = m_certificatePin, .panes = {}, .drafts = m_drafts };
    std::size_t draftBytes = 0;
    for (const ResumeDraft& draft : record.drafts)
        draftBytes += intentBytes(draft.intent);
    for (HeldPane& pane : m_panes) {
        if (pane.hasEnded)
            continue;
        ResumeRecordPane stored { .paneID = pane.paneID, .lastInputSeq = pane.lastInputSeq, .view = pane.view, .checkpoint = {}, .checkpointDigest = {}, .replica = {}, .effects = receipt && receipt->first == pane.paneID ? receipt->second : m_replicas.effectReceipt(pane.paneID) };
        ReplicaSet::Held* held = m_replicas.find(pane.paneID);
        if (held && pane.pictureWithheldAt && held->replica.generation > *pane.pictureWithheldAt)
            pane.pictureWithheldAt.reset();
        if (held && held->hasCheckpoint && !pane.pictureWithheldAt) {
            // The record keeps the screen `replica` describes, which a resume
            // from it continues; a status set standing in it is not kept.
            const ScreenPicture picture(*held);
            ScreenPublisher publisher;
            AttachmentBaseline baseline;
            if (publisher.capture(picture.terminal()) && publisher.checkpoint(picture.terminal(), held->replica.generation, stored.checkpoint, baseline)) {
                stored.checkpointDigest = digestOf(stored.checkpoint);
                stored.replica = held->replica;
                stored.replica.scratch.reset();
                stored.replica.echoContext.reset();
            } else {
                stored.checkpoint.clear();
            }
        }
        // What was typed and not reported written is kept to be shown, within
        // the bound a lost client's queue has.
        const auto keep = [&](const PendingInput& input) {
            const std::size_t bytes = intentBytes(input.intent);
            if (input.isAborted || draftBytes + bytes > maxLostInputBytes)
                return;
            draftBytes += bytes;
            record.drafts.push_back({ .paneID = pane.paneID, .intent = input.intent });
        };
        for (const PendingInput& input : pane.inDoubt)
            keep(input);
        for (const Queued& queued : pane.pending)
            keep(queued.input);
        record.panes.push_back(std::move(stored));
    }
    // Pictures have priority over optional history. Measure all of them before
    // assigning history room, so a later pane cannot consume an earlier pane's
    // already-retained budget.
    const auto pictures = pictureBytes(record);
    if (pictures <= m_policy.maxCheckpointBytes && m_store->historyPersistence() == HistoryPersistence::Enabled) {
        auto remaining = m_policy.maxCheckpointBytes - pictures;
        auto retainedRemaining = m_policy.maxCheckpointBytes;
        for (const auto& stored : record.panes) {
            if (stored.checkpoint.empty())
                continue;
            const auto cells = checkpointCellStorageBytes(stored.checkpoint);
            if (!cells || *cells > retainedRemaining) {
                retainedRemaining = 0;
                break;
            }
            retainedRemaining -= *cells;
        }
        for (auto& stored : record.panes) {
            if (stored.checkpoint.empty())
                continue;
            if (const auto cache = m_replicas.history(stored.paneID)) {
                if (auto snapshot = cache->snapshot(remaining, retainedRemaining)) {
                    remaining -= snapshot->bytes.size();
                    retainedRemaining -= snapshot->retainedBytes;
                    stored.history = std::move(snapshot->bytes);
                }
            }
        }
    }
    // A stored record's secret may be one a `Welcome` consumed, so a record
    // that cannot be written leaves none behind.
    if (pictures > m_policy.maxCheckpointBytes || m_store->write(record) != ResumeWriteOutcome::Committed) {
        (void)purgeRecord();
        return;
    }
    m_hasCommittedRecord = true;
}

std::optional<HeldAttachment> ClientAttachment::held() const
{
    if (!m_held)
        return std::nullopt;
    HeldAttachment held = *m_held;
    for (const HeldPane& pane : m_panes) {
        if (!pane.hasEnded)
            held.panes.push_back({ .paneID = pane.paneID, .lastInputSeq = pane.lastInputSeq });
    }
    return held;
}

std::optional<ResumeProof> ClientAttachment::resumeProof(std::span<const std::uint8_t, 32> exporter) const
{
    if (!m_held || isUnboundTarget(m_identity.request.target))
        return std::nullopt;
    return ResumeProof { .attachmentID = m_held->attachmentID, .mac = resumeProofMAC(m_held->resumeSecret, exporter, m_held->attachmentID, m_held->hostInstanceID) };
}

namespace {

/// Whether a replica still describes a terminal: the rows and state blocks
/// the terminal hashes to now are those the replica holds.
///
/// - Parameters:
///   - digest: The committed picture's content digest.
///   - terminal: The terminal.
/// - Returns: Whether a diff cut against the replica applies to the terminal.
bool isPictureOf(Digest128 digest, const Ubiquitty::Terminal& terminal)
{
    ScreenPublisher publisher;
    std::vector<std::uint8_t> body;
    AttachmentBaseline baseline;
    return publisher.capture(terminal) && publisher.checkpoint(terminal, 1, body, baseline) && baselineDigest(baseline) == digest;
}

} // namespace

ClientConnection& ClientAttachment::connect(std::uint64_t now, ResumeCarrier carrier)
{
    transportLost(now);
    ClientIdentity identity = m_identity;
    if (isUnboundTarget(identity.request.target))
        identity.resume.reset();
    else if (m_held) {
        // The secret is a MAC key: on a connection a proof admitted it has
        // already vouched, and is not sent.
        ResumeRequest resume { .attachmentID = m_held->attachmentID, .resumeSecret = carrier == ResumeCarrier::Secret ? std::optional { m_held->resumeSecret } : std::nullopt, .panes = {} };
        // Range hints are optional. Bound their sum as well as each pane so a
        // fragmented cache cannot expand the fixed-size greeting without limit.
        std::size_t remainingCachedRanges = maxResumeCachedRanges;
        for (HeldPane& pane : m_panes) {
            if (pane.hasEnded)
                continue;
            const ViewState shown = m_identity.kind == ClientKind::Agent ? ViewState::RecordsOnly : ViewState::Live;
            PaneResume named { .paneID = pane.paneID, .ackedGeneration = 0, .ackedRowHashesDigest = {}, .historyEpoch = 0, .cachedRanges = {}, .lastInputSeqSent = pane.lastInputSeq, .view = pane.view.value_or(shown) };
            // What is known of a pane is known of the terminal it was drawn
            // into: a pane the delegate now draws elsewhere, or no longer
            // lends a terminal for, is named as unknown, or a diff would
            // land on a picture it was never cut against.
            Ubiquitty::Terminal* const lent = m_delegate ? m_delegate->terminalForPane(pane.paneID) : nullptr;
            // A picture restored into a terminal the set owned, before the
            // frontend named one of its own, moves into the frontend's: what
            // is known of it stays known, and the host can go on with a diff.
            if (ReplicaSet::Held* kept = m_replicas.find(pane.paneID); kept && lent && kept->terminal && kept->hasCheckpoint && pane.restoredPictureDigest
                && copyPicture(*kept->terminal, *lent, { .maxDecodedScreenCellBytes = m_policy.maxDecodedScreenCellBytes, .maxStorageCost = m_policy.maxCheckpointStorageCost })) {
                kept->lentTerminal = lent;
                kept->terminal.reset();
            } else if (kept && (lent ? kept->picture() != lent : kept->lentTerminal != nullptr))
                m_replicas.forget(pane.paneID);
            // A lent terminal is its owner's to write to between connections,
            // and nothing obliges the owner to say it did: the terminal is
            // read, and what no longer describes it is forgotten. It is read
            // as the replica describes it, a status set standing in it aside.
            const auto describes = [](ReplicaSet::Held& kept, Digest128 digest) {
                const ScreenPicture picture(kept);
                return isPictureOf(digest, picture.terminal());
            };
            if (ReplicaSet::Held* kept = m_replicas.find(pane.paneID); kept && kept->lentTerminal && kept->hasCheckpoint && !describes(*kept, pane.restoredPictureDigest.value_or(replicaDigest(kept->replica))))
                m_replicas.forget(pane.paneID);
            pane.restoredPictureDigest.reset();
            // A replica that never took a checkpoint describes nothing: the
            // host checkpoints it.
            if (const ReplicaSet::Held* replica = m_replicas.find(pane.paneID); replica && replica->hasCheckpoint) {
                named.ackedGeneration = replica->replica.generation;
                named.ackedRowHashesDigest = replicaDigest(replica->replica);
                named.historyEpoch = replica->replica.historyEpoch;
            }
            named.effectsAcknowledged = m_replicas.effectReceipt(pane.paneID).acknowledged;
            if (const auto cache = m_replicas.history(pane.paneID); cache && cache->key().hostInstanceID == m_held->hostInstanceID) {
                named.historyEpoch = cache->key().epoch;
                named.cachedRanges = cache->cachedRanges();
                if (named.cachedRanges.size() > remainingCachedRanges)
                    named.cachedRanges.erase(named.cachedRanges.begin(), named.cachedRanges.end() - static_cast<std::ptrdiff_t>(remainingCachedRanges));
                remainingCachedRanges -= named.cachedRanges.size();
            }
            resume.panes.push_back(std::move(named));
        }
        identity.resume = std::move(resume);
    }
    m_connection = std::make_unique<ClientConnection>(m_policy, std::move(identity), m_delegate, static_cast<HistoryPurgeSink*>(this), &m_replicas, static_cast<EffectReceiptSink*>(this), static_cast<ClientWelcomeSink*>(this));
    m_connection->start();
    // Another host instance numbers its lists from 1: the list held stands
    // until this connection's first, whatever its revision.
    m_attachments.revision = 0;
    m_hasConnectionsTopology = false;
    m_state = ClientAttachmentState::Connecting;
    m_isWelcomeApplied = false;
    m_wantsRedial = false;
    return *m_connection;
}

void ClientAttachment::transportLost(std::uint64_t)
{
    if (m_connection) {
        ActiveObjectUse<ClientConnection>::preserve(m_activeConnectionUse, m_retired);
        // A retired destructor must not overwrite the current replicas.
        m_retired.reset();
        // A notification may reconnect inside the read that brought Welcome,
        // before settle applies its rotated secret. Keep that Welcome before
        // retiring the reader, unless the host ended its authority in the
        // same read. Retiring that reader must not revive an ended attachment.
        if (!m_isWelcomeApplied && m_connection->welcome() && m_connection->byeReason() != ByeReason::SessionGone) {
            m_isWelcomeApplied = true;
            applyWelcome(*m_connection->welcome());
        }
        m_connection->abandon(); // stores once, then detaches the retired writer
    }
    const bool hadConnection = m_connection != nullptr;
    if (m_connection) {
        m_retired = std::move(m_connection);
    }
    m_state = ClientAttachmentState::Lost;
    // Whatever the old connection sent is the next one's to send again,
    // unless its Welcome says the host wrote it.
    for (HeldPane& pane : m_panes) {
        for (Queued& queued : pane.pending) {
            if (queued.sentOn == SentOn::CurrentConnection)
                queued.sentOn = SentOn::EarlierConnection;
        }
    }
    // The picture the lost connection left is the one a relaunch draws.
    if (hadConnection)
        writeRecord();
}

void ClientAttachment::receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now)
{
    if (!m_connection)
        return;
    ActiveObjectUse use(m_activeConnectionUse, *m_connection);
    use.get()->receiveStream(bytes, now);
    if (m_connection.get() != use.get())
        return;
    if (m_isRecordStale)
        writeRecord();
    settle(now);
}

void ClientAttachment::receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now)
{
    if (!m_connection)
        return;
    ActiveObjectUse use(m_activeConnectionUse, *m_connection);
    use.get()->receiveDatagram(datagram, now);
    if (m_connection.get() != use.get())
        return;
    if (m_isRecordStale)
        writeRecord();
    settle(now);
}

void ClientAttachment::tick(std::uint64_t now)
{
    if (!m_connection)
        return;
    ActiveObjectUse use(m_activeConnectionUse, *m_connection);
    use.get()->tick(now);
    if (m_connection.get() != use.get())
        return;
    // The liveness probe went unanswered: the transport is dead, and the
    // attachment resumes over a new one at once (Docs/sync.md §6.9).
    if (m_connection->state() == ConnectionState::Lost) {
        transportLost(now);
        m_wantsRedial = true;
        return;
    }
    settle(now);
}

void ClientAttachment::checkLiveness(std::uint64_t now)
{
    if (m_connection)
        m_connection->checkLiveness(now);
}

LinkState ClientAttachment::linkState(std::uint64_t now) const
{
    return m_connection ? m_connection->linkState(now) : LinkState {};
}

std::vector<std::uint8_t> ClientAttachment::takeStreamOutput()
{
    return m_connection ? m_connection->takeStreamOutput() : std::vector<std::uint8_t> {};
}

std::expected<std::uint64_t, InputRefusal> ClientAttachment::send(ID128 paneID, InputIntent intent, std::uint64_t now)
{
    if (!isSendable(intent))
        return std::unexpected(InputRefusal::Unsendable);
    if (isEnded(paneID))
        return std::unexpected(InputRefusal::PaneEnded);
    const HeldPane* known = findHeldPane(paneID);
    const bool isCarried = m_state == ClientAttachmentState::Live && (!m_store || m_isMemoryOnly || m_hasCommittedRecord) && m_connection && m_connection->handleForPane(paneID) != 0 && (!known || known->inDoubt.empty());
    // Input that cannot go now waits, within the bound; past it the event is
    // refused so its sender knows, rather than lost.
    if (!isCarried && unsentBytes() + intentBytes(intent) > maxLostInputBytes)
        return std::unexpected(InputRefusal::LostQueueFull);
    HeldPane& pane = heldPane(paneID);
    const std::uint32_t handle = m_connection ? m_connection->handleForPane(paneID) : 0;
    if (handle != 0)
        pane.interruptCharacters = m_connection->interruptCharacters(handle);
    if (isInterrupt(intent, pane.interruptCharacters))
        abortPastes(pane, handle);
    const std::uint64_t inputSeq = ++pane.lastInputSeq;
    pane.pending.push_back({ .input = { .paneID = paneID, .inputSeq = inputSeq, .intent = std::move(intent), .isAborted = false }, .sentOn = SentOn::NoConnection });
    flush(pane, now);
    return inputSeq;
}

std::span<const PendingInput> ClientAttachment::inDoubt(ID128 paneID) const
{
    const HeldPane* pane = findHeldPane(paneID);
    return pane ? std::span<const PendingInput> { pane->inDoubt } : std::span<const PendingInput> {};
}

void ClientAttachment::resendInDoubt(ID128 paneID, std::uint64_t now)
{
    const auto found = std::ranges::find(m_panes, paneID, &HeldPane::paneID);
    if (found == m_panes.end() || found->hasEnded || found->inDoubt.empty())
        return;
    std::vector<Queued> resent;
    resent.reserve(found->inDoubt.size() + found->pending.size());
    for (PendingInput& input : found->inDoubt)
        resent.push_back({ .input = std::move(input), .sentOn = SentOn::EarlierConnection });
    found->inDoubt.clear();
    for (Queued& queued : found->pending)
        resent.push_back(std::move(queued));
    std::ranges::stable_sort(resent, {}, [](const Queued& queued) { return queued.input.inputSeq; });
    found->pending = std::move(resent);
    flush(*found, now);
}

void ClientAttachment::discardInDoubt(ID128 paneID, std::uint64_t now)
{
    const auto found = std::ranges::find(m_panes, paneID, &HeldPane::paneID);
    if (found == m_panes.end() || found->inDoubt.empty())
        return;
    found->discarded += found->inDoubt.size();
    found->inDoubt.clear();
    flush(*found, now);
}

void ClientAttachment::setView(ID128 paneID, ViewState view)
{
    HeldPane& pane = heldPane(paneID);
    pane.view = view;
    if (const std::uint32_t handle = m_connection ? m_connection->handleForPane(paneID) : 0)
        m_connection->setView(handle, view);
}

std::optional<ViewState> ClientAttachment::view(ID128 paneID) const
{
    const HeldPane* pane = findHeldPane(paneID);
    return pane ? pane->view : std::nullopt;
}

PaneInputStatus ClientAttachment::inputStatus(ID128 paneID) const
{
    const HeldPane* pane = findHeldPane(paneID);
    if (!pane)
        return {};
    const auto waiting = std::ranges::count_if(pane->pending, [](const Queued& queued) { return queued.sentOn != SentOn::CurrentConnection; });
    return { .inDoubt = pane->inDoubt.size(), .waiting = static_cast<std::size_t>(waiting), .discarded = pane->discarded };
}

std::size_t ClientAttachment::pendingCount(ID128 paneID) const
{
    const HeldPane* pane = findHeldPane(paneID);
    return pane ? pane->pending.size() : 0;
}

bool ClientAttachment::isEnded(ID128 paneID) const
{
    if (const HeldPane* pane = findHeldPane(paneID))
        return pane->hasEnded;
    // A pane the client knows only as an ended replica.
    if (m_replicas.terminal(paneID))
        return false;
    return std::ranges::any_of(m_replicas.ended(), [&](const ReplicaSet::Ended& ended) { return ended.paneID == paneID; });
}

ClientAttachment::HeldPane& ClientAttachment::heldPane(ID128 paneID)
{
    if (const auto found = std::ranges::find(m_panes, paneID, &HeldPane::paneID); found != m_panes.end())
        return *found;
    m_panes.push_back({ .restoredPictureDigest = std::nullopt, .paneID = paneID, .lastInputSeq = 0, .pending = {}, .inDoubt = {}, .discarded = 0, .discardedThrough = 0, .view = std::nullopt, .hasEnded = false, .interruptCharacters = InterruptCharacters::terminalDefaults(), .pictureWithheldAt = std::nullopt });
    return m_panes.back();
}

const ClientAttachment::HeldPane* ClientAttachment::findHeldPane(ID128 paneID) const
{
    const auto found = std::ranges::find(m_panes, paneID, &HeldPane::paneID);
    return found == m_panes.end() ? nullptr : &*found;
}

void ClientAttachment::settle(std::uint64_t now)
{
    if (!m_connection)
        return;
    if (m_connection->state() == ConnectionState::Closed && m_connection->byeReason() == ByeReason::SessionGone && m_held) {
        endHost();
        return;
    }
    if (m_connection->state() == ConnectionState::Closed && m_connection->byeReason() == ByeReason::HandingOver) {
        transportLost(now);
        m_wantsRedial = true;
        return;
    }
    if (m_connection->state() == ConnectionState::Closed || m_connection->state() == ConnectionState::Lost) {
        m_state = ClientAttachmentState::Lost;
        return;
    }
    if (m_connection->state() == ConnectionState::AwaitingConsent) {
        m_state = ClientAttachmentState::AwaitingConsent;
        return;
    }
    if (!m_isWelcomeApplied && m_connection->welcome() && m_connection->state() == ConnectionState::Attached) {
        m_isWelcomeApplied = true;
        if (!isUnboundTarget(m_identity.request.target))
            applyWelcome(*m_connection->welcome());
        m_state = ClientAttachmentState::Live;
        // The Welcome consumed the stored secret and issued another: the
        // record holds the new one before any input goes on this connection.
        if (!isUnboundTarget(m_identity.request.target))
            writeRecord();
    }
    if (m_state != ClientAttachmentState::Live)
        return;
    for (const auto& [handle, paneID] : m_connection->boundPanes()) {
        HeldPane& pane = heldPane(paneID);
        if (pane.hasEnded)
            continue;
        // A pane bound fresh, the resume refused, starts as any other; the
        // view the client set is its own.
        if (pane.view && m_connection->paneView(handle) != pane.view)
            m_connection->setView(handle, *pane.view);
        for (const InputDiscarded& run : m_connection->takeInputDiscards(handle))
            noteDiscarded(pane, run);
        noteWritten(pane, m_connection->receipt(handle).inputWritten);
        flush(pane, now);
    }
}

bool ClientAttachment::acceptWelcome(const Welcome& welcome)
{
    if (m_identitySink && !m_identitySink->acceptWelcome(welcome))
        return false;
    // Identity checks still run, but an unapproved secret never replaces a
    // resumable record or reaches the protected persistence owner.
    if (welcome.admission == AttachmentAdmission::AwaitingConsent || isUnboundTarget(welcome.selection.target))
        return true;
    m_hasCommittedRecord = false;
    m_isMemoryOnly = false;
    const auto binding = m_store ? m_store->bindWelcome(welcome, m_identity.clientInstance, m_certificatePin) : ResumeBindingOutcome::Bound;
    if (binding == ResumeBindingOutcome::MemoryOnly) {
        // Welcome rotates the secret. No record carrying the previous
        // secret may survive a connection that cannot commit its successor.
        m_isStoreBindingRefused = !purgeRecord();
        m_isMemoryOnly = !m_isStoreBindingRefused;
        return !m_isStoreBindingRefused;
    }
    m_isStoreBindingRefused = binding == ResumeBindingOutcome::Refused;
    if (m_isStoreBindingRefused)
        (void)purgeRecord();
    return !m_isStoreBindingRefused;
}

void ClientAttachment::endHost()
{
    if (m_connection)
        m_connection->leave(ByeReason::SessionGone);
    if (m_held) {
        for (auto& pane : m_panes) {
            if (pane.hasEnded)
                continue;
            pane.hasEnded = true;
            m_replicas.end(pane.paneID, ReplicaSet::EndReason::HostRestarted);
            discardWithPane(pane, m_held->hostInstanceID);
        }
    }
    m_held.reset();
    m_state = ClientAttachmentState::Lost;
    m_wantsRedial = false;
    (void)purgeRecord();
}

void ClientAttachment::applyWelcome(const Welcome& welcome)
{
    // A prior commit names the consumed secret, never this Welcome's secret.
    m_hasCommittedRecord = false;
    const bool hasResumed = m_held.has_value();
    const bool hasRestarted = hasResumed && continuityOf(m_held->hostInstanceID, welcome.hostInstanceID, welcome.predecessorHostInstanceID) == InstanceContinuity::Restarted;
    if (hasRestarted) {
        // The old instance's programs are gone, and its panes' replicas
        // ended with the Welcome: nothing of them carries over, and their
        // input is counted as discarded on each ended replica.
        for (HeldPane& pane : m_panes)
            discardWithPane(pane, m_held->hostInstanceID);
        m_panes.clear();
    } else if (hasResumed) {
        for (HeldPane& pane : m_panes) {
            if (pane.hasEnded)
                continue;
            const auto resumed = std::ranges::find(welcome.resumed, pane.paneID, &PaneResumed::paneID);
            if (resumed != welcome.resumed.end() && resumed->outcome == ResumeOutcome::Gone) {
                pane.hasEnded = true;
                m_replicas.end(pane.paneID, ReplicaSet::EndReason::PaneGone);
                discardWithPane(pane, welcome.hostInstanceID);
                continue;
            }
            // With no count the host's fate for the events is unknown, and
            // sending them again could write one twice.
            if (resumed == welcome.resumed.end() || !resumed->inputWritten) {
                moveToDoubt(pane);
                continue;
            }
            noteWritten(pane, *resumed->inputWritten);
        }
    }
    m_held = HeldAttachment { .hostInstanceID = welcome.hostInstanceID, .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
}

void ClientAttachment::noteWritten(HeldPane& pane, std::uint64_t inputWritten)
{
    // A client restored from an older record never numbers an event the
    // host already wrote.
    pane.lastInputSeq = std::max(pane.lastInputSeq, inputWritten);
    std::erase_if(pane.pending, [&](const Queued& queued) { return queued.sentOn != SentOn::NoConnection && queued.input.inputSeq <= inputWritten; });
    // What no connection took was not written, whatever its number: such a
    // client may have numbered it at or below the host's count before it
    // learned the count. What no connection took is the queue's tail, so
    // from the first such event the tail is numbered after the count, in
    // order.
    const auto stale = std::ranges::find_if(pane.pending, [&](const Queued& queued) { return queued.sentOn == SentOn::NoConnection && queued.input.inputSeq <= inputWritten; });
    for (auto queued = stale; queued != pane.pending.end(); ++queued)
        queued->input.inputSeq = ++pane.lastInputSeq;
}

void ClientAttachment::noteDiscarded(HeldPane& pane, const InputDiscarded& run)
{
    const std::uint64_t counted = std::max(run.inputWritten, pane.discardedThrough);
    if (run.throughSeq <= counted)
        return;
    pane.discarded += static_cast<std::size_t>(run.throughSeq - counted);
    pane.discardedThrough = run.throughSeq;
    pane.lastInputSeq = std::max(pane.lastInputSeq, run.throughSeq);
    std::erase_if(pane.pending, [&](const Queued& queued) { return queued.input.inputSeq > run.inputWritten && queued.input.inputSeq <= run.throughSeq; });
}

void ClientAttachment::moveToDoubt(HeldPane& pane)
{
    // Only an event a connection took can have reached the host. One typed
    // while none carried the pane stays queued, behind those in doubt.
    const auto isTaken = [](const Queued& queued) { return queued.sentOn != SentOn::NoConnection; };
    for (Queued& queued : pane.pending) {
        if (isTaken(queued))
            pane.inDoubt.push_back(std::move(queued.input));
    }
    std::erase_if(pane.pending, isTaken);
}

void ClientAttachment::discardWithPane(HeldPane& pane, ID128 hostInstanceID)
{
    const std::size_t count = pane.pending.size() + pane.inDoubt.size();
    pane.pending.clear();
    pane.inDoubt.clear();
    pane.discarded += count;
    if (ReplicaSet::Ended* ended = m_replicas.findEnded(hostInstanceID, pane.paneID))
        ended->discardedInputEvents += count;
}

void ClientAttachment::flush(HeldPane& pane, std::uint64_t now)
{
    // New input waits behind events in doubt until the person decides, so no
    // later event is written before them.
    if (m_state != ClientAttachmentState::Live || (m_store && !m_isMemoryOnly && !m_hasCommittedRecord) || !m_connection || !pane.inDoubt.empty())
        return;
    const std::uint32_t handle = m_connection->handleForPane(pane.paneID);
    if (handle == 0)
        return;
    bool hasSent = false;
    for (Queued& queued : pane.pending) {
        if (queued.sentOn == SentOn::CurrentConnection)
            continue;
        // The host writes a pane's events in order, so one that could not go
        // holds back those after it. A paste given up goes as its abort,
        // which ends it wherever the host's taking of it stopped.
        const bool isTaken = queued.input.isAborted ? m_connection->sendPasteAbort(handle, queued.input.inputSeq) : m_connection->sendNumbered(handle, queued.input.inputSeq, queued.input.intent);
        if (!isTaken)
            break;
        queued.sentOn = SentOn::CurrentConnection;
        hasSent = true;
    }
    if (hasSent)
        m_connection->noteInputSent(now);
}

void ClientAttachment::abortPastes(HeldPane& pane, std::uint32_t handle)
{
    for (Queued& queued : pane.pending) {
        if (queued.input.isAborted || !std::holds_alternative<PasteInput>(queued.input.intent))
            continue;
        queued.input.isAborted = true;
        // What the connection has of it is dropped, and the abort it sends
        // goes ahead of the interrupt.
        if (queued.sentOn == SentOn::CurrentConnection && handle != 0)
            (void)m_connection->abortPaste(handle);
    }
}

std::size_t ClientAttachment::unsentBytes() const
{
    std::size_t bytes = 0;
    for (const HeldPane& pane : m_panes) {
        for (const Queued& queued : pane.pending)
            bytes += queued.sentOn == SentOn::CurrentConnection ? 0 : intentBytes(queued.input.intent);
    }
    return bytes;
}

} // namespace Ubiquitty::Protocol::Sync
