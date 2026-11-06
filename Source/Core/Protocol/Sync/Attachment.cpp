// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Attachment.h"

#include "Core/Crypto/HMAC.h"
#include "Core/Protocol/Sync/HandoverBody.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

bool constantTimeEqual(const ID128& left, const ID128& right)
{
    // Every bit is compared whatever the first difference: a secret checked
    // byte by byte leaks how much of a guess was right through its timing.
    const std::uint64_t difference = (left.low ^ right.low) | (left.high ^ right.high);
    volatile std::uint64_t sink = difference;
    return sink == 0;
}

PaneBaselines* AttachmentRecord::findPane(ID128 paneID)
{
    const auto found = std::ranges::find_if(panes, [&](const std::unique_ptr<PaneBaselines>& pane) { return pane->paneID == paneID; });
    return found == panes.end() ? nullptr : found->get();
}

PaneBaselines& AttachmentRecord::paneFor(ID128 paneID)
{
    const auto found = std::ranges::find_if(panes, [&](const std::unique_ptr<PaneBaselines>& pane) { return pane->paneID == paneID; });
    if (found != panes.end())
        return **found;
    panes.push_back(std::make_unique<PaneBaselines>(PaneBaselines { .paneID = paneID, .sentGeneration = 0, .sentBaseline = {}, .acknowledgedGeneration = std::nullopt, .acknowledgedBaseline = {}, .inFlight = {}, .isHeld = true }));
    return *panes.back();
}

AttachmentTable::AttachmentTable(ID128 hostInstanceID, const ResumePolicy& policy, SecretSource& secrets)
    : m_hostInstanceID(hostInstanceID)
    , m_policy(policy)
    , m_secrets(secrets)
{
}

void AttachmentTable::setRetireRecord(RetireRecord retire)
{
    m_retireRecord = std::move(retire);
}

AttachmentTable::Continuation AttachmentTable::exportForHandover(std::uint64_t now) const
{
    Continuation continuation { .hostInstanceID = m_hostInstanceID, .inheritedInstances = m_inheritedInstances, .records = {}, .revision = m_revision, .notifyRevision = m_notifyRevision };
    for (const auto& record : m_records) {
        if (record->state == AttachmentState::Detached || record->admission != AttachmentAdmission::Granted || !record->isResumable)
            continue;
        auto carried = std::make_unique<AttachmentRecord>();
        carried->attachmentID = record->attachmentID;
        carried->issuer = record->issuer;
        carried->resumeSecret = record->resumeSecret;
        carried->authority = record->authority;
        carried->kind = record->kind;
        carried->clientInstance = record->clientInstance;
        carried->state = AttachmentState::Lost;
        carried->lostSinceMicroseconds = record->state == AttachmentState::Live ? now : record->lostSinceMicroseconds;
        carried->inheritedProportionalFaces = record->inheritedProportionalFaces;
        for (const auto& pane : record->panes)
            carried->panes.push_back(std::make_unique<PaneBaselines>(*pane));
        continuation.records.push_back(std::move(carried));
    }
    return continuation;
}

bool AttachmentTable::isPristine() const
{
    return m_records.empty() && m_inheritedInstances.empty() && m_nextAttachment == 1 && !m_revision && !m_notifyRevision && m_freed.empty() && m_superseded.empty();
}

std::expected<void, AttachmentTable::InheritanceRefusal> AttachmentTable::inherit(Continuation&& continuation, ID128 predecessor, std::uint64_t now)
{
    if (!isPristine())
        return std::unexpected(InheritanceRefusal::DestinationNotEmpty);
    if (m_hostInstanceID == ID128 {} || predecessor == ID128 {} || predecessor == m_hostInstanceID || continuation.hostInstanceID != predecessor
        || continuation.inheritedInstances.size() > maxInheritedInstances)
        return std::unexpected(InheritanceRefusal::InvalidIdentity);
    for (std::size_t index = 0; index < continuation.inheritedInstances.size(); ++index) {
        const ID128 issuer = continuation.inheritedInstances[index];
        if (issuer == ID128 {} || issuer == m_hostInstanceID || issuer == predecessor
            || std::find(continuation.inheritedInstances.begin(), continuation.inheritedInstances.begin() + index, issuer) != continuation.inheritedInstances.begin() + index)
            return std::unexpected(InheritanceRefusal::InvalidIdentity);
    }
    if (continuation.revision == std::numeric_limits<std::uint64_t>::max() || continuation.notifyRevision == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(InheritanceRefusal::ExhaustedIdentity);
    std::uint64_t nextAttachment = 1;
    for (std::size_t index = 0; index < continuation.records.size(); ++index) {
        const auto& record = continuation.records[index];
        if (!record || record->attachmentID == ID128 {} || record->resumeSecret == ID128 {}
            || (record->issuer != predecessor && std::ranges::find(continuation.inheritedInstances, record->issuer) == continuation.inheritedInstances.end())
            || record->state != AttachmentState::Lost || record->admission != AttachmentAdmission::Granted || !record->isResumable || record->servingConnection || record->lostSinceMicroseconds > now
            || record->consumedSecret || record->handshakeConnection || record->proofConnection
            || (record->inheritedProportionalFaces && !OSC::TextSizing::isAdoptable(*record->inheritedProportionalFaces)))
            return std::unexpected(InheritanceRefusal::InvalidRecord);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (continuation.records[previous]->attachmentID == record->attachmentID)
                return std::unexpected(InheritanceRefusal::InvalidRecord);
        }
        if (record->attachmentID.high == m_hostInstanceID.low) {
            if (record->attachmentID.low == std::numeric_limits<std::uint64_t>::max())
                return std::unexpected(InheritanceRefusal::ExhaustedIdentity);
            nextAttachment = std::max(nextAttachment, record->attachmentID.low + 1);
        }
        for (std::size_t paneIndex = 0; paneIndex < record->panes.size(); ++paneIndex) {
            const auto& pane = record->panes[paneIndex];
            if (!pane || pane->paneID == ID128 {} || pane->effectsAcknowledged > pane->effectsSent
                || (pane->acknowledgedGeneration && *pane->acknowledgedGeneration > pane->sentGeneration))
                return std::unexpected(InheritanceRefusal::InvalidRecord);
            for (std::size_t previous = 0; previous < paneIndex; ++previous) {
                if (record->panes[previous]->paneID == pane->paneID)
                    return std::unexpected(InheritanceRefusal::InvalidRecord);
            }
        }
    }
    auto issuers = continuation.inheritedInstances;
    issuers.push_back(predecessor);
    const std::optional<ID128> retired = issuers.size() > maxInheritedInstances ? std::optional { issuers.front() } : std::nullopt;
    if (retired)
        issuers.erase(issuers.begin());
    const auto revisionHeadroom = std::numeric_limits<std::uint64_t>::max() - 1 - continuation.revision;
    std::uint64_t retirements = 0;
    for (const auto& record : continuation.records) {
        const bool isPast = now - record->lostSinceMicroseconds >= m_policy.lostGraceMicroseconds;
        if ((retired && record->issuer == *retired) || isPast) {
            if (retirements == revisionHeadroom)
                return std::unexpected(InheritanceRefusal::ExhaustedIdentity);
            ++retirements;
        }
    }
    m_records = std::move(continuation.records);
    m_inheritedInstances = std::move(issuers);
    m_nextAttachment = nextAttachment;
    m_revision = continuation.revision;
    m_notifyRevision = continuation.notifyRevision;
    std::erase_if(m_records, [&](const auto& record) {
        if (!retired || record->issuer != *retired)
            return false;
        if (m_retireRecord)
            m_retireRecord(record->attachmentID);
        m_freed.push_back({ .attachmentID = record->attachmentID, .clientInstance = record->clientInstance });
        ++m_revision;
        return true;
    });
    expire(now);
    return {};
}

bool AttachmentTable::isResumableIssuer(ID128 issuer) const
{
    return issuer == m_hostInstanceID || std::ranges::find(m_inheritedInstances, issuer) != m_inheritedInstances.end();
}

bool AttachmentTable::holdsResumablePane(ID128 issuer, std::string_view principal, ID128 paneID,
    std::optional<ID128> proven, std::uint64_t now) const
{
    if (!isResumableIssuer(issuer))
        return false;
    return std::ranges::any_of(m_records, [&](const auto& record) {
        if (record->issuer != issuer || record->authority.principal != principal
            || (proven && record->attachmentID != *proven) || record->state == AttachmentState::Detached || record->admission != AttachmentAdmission::Granted || !record->isResumable)
            return false;
        if (record->state == AttachmentState::Lost
            && (now < record->lostSinceMicroseconds || now - record->lostSinceMicroseconds >= m_policy.lostGraceMicroseconds))
            return false;
        return std::ranges::any_of(record->panes, [&](const auto& pane) { return pane->paneID == paneID && pane->isHeld; });
    });
}

AttachmentRecord& AttachmentTable::create(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now)
{
    AttachmentRecord& created = createRecord(clientInstance, kind, std::move(authority), connection, now, AttachmentAdmission::Granted);
    supersedeLostRecords(created);
    return created;
}

AttachmentRecord& AttachmentTable::createPending(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now)
{
    return createRecord(clientInstance, kind, std::move(authority), connection, now, AttachmentAdmission::AwaitingConsent);
}

AttachmentRecord& AttachmentTable::createUnbound(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now)
{
    AttachmentRecord& created = createRecord(clientInstance, kind, std::move(authority), connection, now, AttachmentAdmission::Granted);
    created.isResumable = false;
    return created;
}

AttachmentRecord& AttachmentTable::createRecord(ID128 clientInstance, ClientKind kind, ConnectionAuthority authority, std::uint64_t connection, std::uint64_t now, AttachmentAdmission admission)
{
    expire(now);
    auto record = std::make_unique<AttachmentRecord>();
    // The identity's high half stays tied to its birth host; the full issuer
    // of each rotated secret is carried separately.
    record->attachmentID = { .low = m_nextAttachment++, .high = m_hostInstanceID.low };
    record->issuer = m_hostInstanceID;
    record->resumeSecret = m_secrets.nextSecret();
    record->authority = std::move(authority);
    record->kind = kind;
    record->clientInstance = clientInstance;
    record->state = AttachmentState::Live;
    record->admission = admission;
    record->servingConnection = connection;
    m_records.push_back(std::move(record));
    return *m_records.back();
}

bool AttachmentTable::promotePending(ID128 attachmentID, const ConnectionAuthority& authority, std::uint64_t connection)
{
    if (!canPromotePending(attachmentID, authority, connection))
        return false;
    AttachmentRecord* record = findMutable(attachmentID);
    record->admission = AttachmentAdmission::Granted;
    supersedeLostRecords(*record);
    return true;
}

bool AttachmentTable::canPromotePending(ID128 attachmentID, const ConnectionAuthority& authority, std::uint64_t connection) const
{
    const AttachmentRecord* record = find(attachmentID);
    return record && record->admission == AttachmentAdmission::AwaitingConsent && record->state == AttachmentState::Live
        && record->servingConnection == connection && record->authority == authority;
}

void AttachmentTable::supersedeLostRecords(const AttachmentRecord& admitted)
{
    assert(admitted.admission == AttachmentAdmission::Granted);
    // The client is back by another door: what it left Lost is not coming
    // back for.
    std::erase_if(m_records, [&](const std::unique_ptr<AttachmentRecord>& held) {
        const bool isReplaced = held->isResumable && held->state == AttachmentState::Lost && isSameClient(*held, admitted);
        if (isReplaced) {
            if (m_retireRecord)
                m_retireRecord(held->attachmentID);
            m_freed.push_back({ .attachmentID = held->attachmentID, .clientInstance = held->clientInstance });
            ++m_revision;
        }
        return isReplaced;
    });
}

bool AttachmentTable::isSameClient(const AttachmentRecord& one, const AttachmentRecord& other)
{
    return &one != &other && one.clientInstance == other.clientInstance && one.authority.principal == other.authority.principal;
}

std::expected<AttachmentRecord*, ResumeRefusal> AttachmentTable::resume(const ResumeRequest& request, const ConnectionAuthority& authority, std::uint64_t connection, std::uint64_t now)
{
    expire(now);
    AttachmentRecord* record = findMutable(request.attachmentID);
    if (!record || !isResumableIssuer(record->issuer))
        return std::unexpected(ResumeRefusal::UnknownAttachment);
    if (record->admission != AttachmentAdmission::Granted || !record->isResumable)
        return std::unexpected(ResumeRefusal::NotResumable);
    if (authority.principal != record->authority.principal)
        return std::unexpected(ResumeRefusal::OtherPrincipal);
    // The secret the admission in its handshake consumed: a racing second
    // resume, not a wrong one.
    if (request.resumeSecret && record->handshakeConnection && record->consumedSecret && constantTimeEqual(*request.resumeSecret, *record->consumedSecret))
        return std::unexpected(ResumeRefusal::InProgress);
    // A proof stands in for the secret on the connection it admitted and on
    // no other; there the secret is never sent, so one that is sent is not
    // taken for it.
    const bool isProven = record->proofConnection == std::optional { connection };
    const bool isSecretRight = request.resumeSecret && constantTimeEqual(*request.resumeSecret, record->resumeSecret);
    if (isProven ? request.resumeSecret.has_value() : !isSecretRight)
        return std::unexpected(ResumeRefusal::WrongSecret);
    record->proofConnection.reset();
    // The swap: the resume succeeds first, and the connection still serving
    // the attachment is closed second.
    if (record->state == AttachmentState::Live && record->servingConnection != connection)
        m_superseded.push_back(record->servingConnection);
    record->state = AttachmentState::Live;
    record->servingConnection = connection;
    record->authority = authority;
    record->consumedSecret = record->resumeSecret;
    record->handshakeConnection = connection;
    record->resumeSecret = m_secrets.nextSecret();
    record->issuer = m_hostInstanceID;
    ++m_revision;
    return record;
}

std::expected<const AttachmentRecord*, ResumeRefusal> AttachmentTable::admitProof(const ResumeProof& proof, std::span<const std::uint8_t, 32> exporter, std::uint64_t connection, std::uint64_t now)
{
    expire(now);
    AttachmentRecord* record = findMutable(proof.attachmentID);
    // A detached attachment is freed, so it is unknown here: it goes back
    // through SSH.
    if (!record || !isResumableIssuer(record->issuer))
        return std::unexpected(ResumeRefusal::UnknownAttachment);
    // The MAC is checked before anything says what state the attachment is
    // in: a peer without the secret learns only that it is wrong.
    const std::array<std::uint8_t, 32> expected = resumeProofMAC(record->resumeSecret, exporter, proof.attachmentID, record->issuer);
    if (!Crypto::constantTimeEqual(expected, proof.mac))
        return std::unexpected(ResumeRefusal::WrongSecret);
    if (record->admission != AttachmentAdmission::Granted || !record->isResumable)
        return std::unexpected(ResumeRefusal::NotResumable);
    if (record->handshakeConnection)
        return std::unexpected(ResumeRefusal::InProgress);
    // A connection a proof admitted that has not greeted gives way to a newer
    // proof: both proved the secret, and the older is the dial a client that
    // changed networks left behind, which the host may not see die for as
    // long as its idle timeout. It is closed as a superseded connection is.
    if (record->proofConnection && *record->proofConnection != connection)
        m_superseded.push_back(*record->proofConnection);
    record->proofConnection = connection;
    return record;
}

std::optional<ID128> AttachmentTable::provenAttachment(std::uint64_t connection) const
{
    const auto found = std::ranges::find_if(m_records, [&](const std::unique_ptr<AttachmentRecord>& record) { return record->proofConnection == std::optional { connection }; });
    return found == m_records.end() ? std::nullopt : std::optional { (*found)->attachmentID };
}

void AttachmentTable::abandonProof(std::uint64_t connection)
{
    for (const std::unique_ptr<AttachmentRecord>& record : m_records) {
        if (record->proofConnection == std::optional { connection })
            record->proofConnection.reset();
    }
}

void AttachmentTable::noteTransportLost(ID128 attachmentID, std::uint64_t connection, std::uint64_t now)
{
    AttachmentRecord* record = findMutable(attachmentID);
    if (!record || record->state != AttachmentState::Live || record->servingConnection != connection)
        return;
    if (record->admission != AttachmentAdmission::Granted || !record->isResumable) {
        noteDetached(attachmentID);
        return;
    }
    record->state = AttachmentState::Lost;
    record->lostSinceMicroseconds = now;
    ++m_revision;
    // Its client already holds a newer attachment: the host has only now
    // found the transport that client left dead, and nothing will resume
    // this one.
    const auto position = std::ranges::find_if(m_records, [&](const auto& held) { return held.get() == record; });
    const bool isReplaced = std::any_of(std::next(position), m_records.end(), [&](const auto& held) { return held->admission == AttachmentAdmission::Granted && held->isResumable && isSameClient(*held, *record); });
    if (isReplaced)
        noteDetached(attachmentID);
}

void AttachmentTable::noteFirstFrame(ID128 attachmentID, std::uint64_t connection)
{
    AttachmentRecord* record = findMutable(attachmentID);
    if (!record || record->handshakeConnection != connection)
        return;
    record->handshakeConnection.reset();
    record->consumedSecret.reset();
}

std::vector<std::uint64_t> AttachmentTable::takeSuperseded()
{
    return std::exchange(m_superseded, {});
}

void AttachmentTable::noteDetached(ID128 attachmentID)
{
    std::erase_if(m_records, [&](const std::unique_ptr<AttachmentRecord>& record) {
        if (record->attachmentID != attachmentID)
            return false;
        if (m_retireRecord)
            m_retireRecord(record->attachmentID);
        m_freed.push_back({ .attachmentID = attachmentID, .clientInstance = record->clientInstance });
        ++m_revision;
        return true;
    });
}

void AttachmentTable::expire(std::uint64_t now)
{
    std::erase_if(m_records, [&](const std::unique_ptr<AttachmentRecord>& record) {
        const bool isPast = record->state == AttachmentState::Lost && now >= record->lostSinceMicroseconds
            && now - record->lostSinceMicroseconds >= m_policy.lostGraceMicroseconds;
        if (isPast) {
            if (m_retireRecord)
                m_retireRecord(record->attachmentID);
            m_freed.push_back({ .attachmentID = record->attachmentID, .clientInstance = record->clientInstance });
            ++m_revision;
        }
        return isPast;
    });
}

std::optional<ID128> AttachmentTable::primary(const GeometryArbiter& geometry) const
{
    if (const std::optional<ID128> owner = geometry.drivingOwner()) {
        if (const AttachmentRecord* record = find(*owner); record && record->state == AttachmentState::Live && record->admission == AttachmentAdmission::Granted)
            return owner;
    }
    // The arbiter records typing from human drivers alone.
    for (const ID128& typist : geometry.typistsNewestFirst()) {
        if (const AttachmentRecord* record = find(typist); record && record->state == AttachmentState::Live && record->admission == AttachmentAdmission::Granted)
            return typist;
    }
    return std::nullopt;
}

std::vector<const AttachmentRecord*> AttachmentTable::holding(std::span<const ID128> paneIDs) const
{
    std::vector<const AttachmentRecord*> held;
    for (const std::unique_ptr<AttachmentRecord>& record : m_records) {
        if (record->admission != AttachmentAdmission::Granted)
            continue;
        const bool isHolding = std::ranges::any_of(record->panes, [&](const std::unique_ptr<PaneBaselines>& pane) { return pane->isHeld && std::ranges::find(paneIDs, pane->paneID) != paneIDs.end(); });
        if (isHolding)
            held.push_back(record.get());
    }
    return held;
}

std::size_t AttachmentTable::inheritedFaceOffers(ID128 paneID, std::span<const AttachmentRecord*> output) const
{
    std::size_t count = 0;
    for (const auto& record : m_records) {
        if (count == output.size())
            break;
        if (record->state == AttachmentState::Detached || record->admission != AttachmentAdmission::Granted
            || record->kind != ClientKind::Human || record->authority.verifiedKind != ClientKind::Human
            || !record->authority.mayDrive || !record->inheritedProportionalFaces)
            continue;
        if (std::ranges::any_of(record->panes, [&](const auto& pane) { return pane->isHeld && pane->paneID == paneID; }))
            output[count++] = record.get();
    }
    return count;
}

std::vector<const AttachmentRecord*> AttachmentTable::retainingPanes(std::span<const ID128> paneIDs) const
{
    std::vector<const AttachmentRecord*> retained;
    for (const auto& record : m_records) {
        if (record->admission != AttachmentAdmission::Granted)
            continue;
        if (std::ranges::any_of(record->panes, [&](const auto& pane) { return std::ranges::find(paneIDs, pane->paneID) != paneIDs.end(); }))
            retained.push_back(record.get());
    }
    return retained;
}

bool AttachmentTable::holdsClient(ID128 clientInstance) const
{
    return std::ranges::any_of(m_records, [&](const std::unique_ptr<AttachmentRecord>& record) { return record->clientInstance == clientInstance; });
}

std::vector<AttachmentTable::Freed> AttachmentTable::takeFreed()
{
    return std::exchange(m_freed, {});
}

std::optional<std::uint64_t> AttachmentTable::nextExpiryMicroseconds() const
{
    std::optional<std::uint64_t> next;
    for (const std::unique_ptr<AttachmentRecord>& record : m_records) {
        if (record->state == AttachmentState::Lost) {
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto deadline = m_policy.lostGraceMicroseconds > maximum - record->lostSinceMicroseconds ? maximum : record->lostSinceMicroseconds + m_policy.lostGraceMicroseconds;
            next = std::min(next.value_or(maximum), deadline);
        }
    }
    return next;
}

const AttachmentRecord* AttachmentTable::find(ID128 attachmentID) const
{
    const auto found = std::ranges::find_if(m_records, [&](const std::unique_ptr<AttachmentRecord>& record) { return record->attachmentID == attachmentID; });
    return found == m_records.end() ? nullptr : found->get();
}

AttachmentRecord* AttachmentTable::findMutable(ID128 attachmentID)
{
    return const_cast<AttachmentRecord*>(std::as_const(*this).find(attachmentID));
}

bool AttachmentTable::forgetUnheldPane(ID128 attachmentID, ID128 paneID)
{
    auto* record = findMutable(attachmentID);
    return record && std::erase_if(record->panes, [&](const auto& pane) { return pane->paneID == paneID && !pane->isHeld; }) > 0;
}

} // namespace Ubiquitty::Protocol::Sync
