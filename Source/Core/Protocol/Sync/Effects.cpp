// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Effects.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

EffectDisposition effectDisposition(const EffectView& effect, ID128 clientInstance, ClientKind kind, Role role)
{
    if (kind == ClientKind::Agent || role == Role::Agent)
        return EffectDisposition::Ignore;
    const bool isDriver = kind == ClientKind::Human && (role == Role::Owner || role == Role::Driver);
    if (isDriver && clientInstance != ID128 {} && effect.primary == clientInstance)
        return EffectDisposition::Post;
    return effect.kind == EffectKind::ClipboardWrite ? EffectDisposition::Ignore : EffectDisposition::Badge;
}

EffectLog::EffectLog(ID128 paneID, EffectPolicy policy, std::uint64_t lastSequence)
    : m_paneID(paneID)
    , m_policy(policy)
    , m_lastSequence(lastSequence)
    , m_retiredThrough(lastSequence)
{
}

bool EffectLog::isPolicyValid() const
{
    return m_paneID != ID128 {} && m_policy.maxRecords && m_policy.maxRecords <= maxEffectRecords
        && m_policy.notificationsKept && m_policy.notificationsKept <= m_policy.maxRecords
        && m_policy.maxPayloadBytes <= m_policy.maxRetainedBytes;
}

EffectLog::Continuation EffectLog::continuation() const
{
    Continuation result;
    result.paneID = m_paneID;
    result.policy = m_policy;
    result.recordCount = m_size;
    result.lastSequence = m_lastSequence;
    result.lastGeneration = m_lastGeneration;
    result.retiredThrough = m_retiredThrough;
    for (std::size_t index = 0; index < m_size; ++index)
        result.records[index] = m_entries[index].view;
    for (const auto& reader : m_readers) {
        if (reader.state == AttachmentState::Detached || reader.isTransient)
            continue;
        result.readers[result.readerCount++] = { reader.attachmentID, reader.clientInstance,
            reader.state, reader.acknowledged, reader.sent, reader.lostSinceMicroseconds };
    }
    return result;
}

std::expected<void, EffectRefusal> EffectLog::restoreContinuation(const Continuation& continuation)
{
    if (!isPolicyValid() || m_size || m_retainedBytes || m_lastSequence || m_lastGeneration || m_retiredThrough || readerCount()
        || continuation.paneID != m_paneID || continuation.policy.maxRecords != m_policy.maxRecords
        || continuation.policy.maxPayloadBytes != m_policy.maxPayloadBytes
        || continuation.policy.maxRetainedBytes != m_policy.maxRetainedBytes
        || continuation.policy.notificationsKept != m_policy.notificationsKept
        || continuation.policy.lostGraceMicroseconds != m_policy.lostGraceMicroseconds
        || continuation.recordCount > m_policy.maxRecords || continuation.readerCount > maxEffectReaders
        || continuation.retiredThrough > continuation.lastSequence
        || continuation.lastSequence - continuation.retiredThrough != continuation.recordCount)
        return std::unexpected(EffectRefusal::Invalid);
    std::size_t retainedBytes = 0;
    std::uint64_t previousGeneration = 0;
    for (std::size_t index = 0; index < continuation.recordCount; ++index) {
        const auto& view = continuation.records[index];
        if (view.paneID != m_paneID || view.sequence != continuation.retiredThrough + index + 1
            || view.generation < previousGeneration || view.generation > continuation.lastGeneration || !view.count
            || std::to_underlying(view.kind) > std::to_underlying(EffectKind::ClipboardWrite)
            || (view.kind != EffectKind::ClipboardWrite && view.target)
            || (!view.data.empty() && view.kind != EffectKind::ClipboardWrite && view.kind != EffectKind::Sound))
            return std::unexpected(EffectRefusal::Invalid);
        if (view.payload.size() > m_policy.maxPayloadBytes || view.payload.size() > m_policy.maxRetainedBytes - retainedBytes
            || view.data.size() > m_policy.maxRetainedBytes - retainedBytes - view.payload.size())
            return std::unexpected(EffectRefusal::OverLimit);
        retainedBytes += view.payload.size() + view.data.size();
        previousGeneration = view.generation;
    }
    for (std::size_t index = 0; index < continuation.readerCount; ++index) {
        const auto& reader = continuation.readers[index];
        if (reader.attachmentID == ID128 {} || reader.clientInstance == ID128 {}
            || (reader.state != AttachmentState::Live && reader.state != AttachmentState::Lost)
            || reader.acknowledged < continuation.retiredThrough || reader.acknowledged > reader.sent
            || reader.sent > continuation.lastSequence)
            return std::unexpected(EffectRefusal::Invalid);
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (continuation.readers[prior].attachmentID == reader.attachmentID)
                return std::unexpected(EffectRefusal::Invalid);
        }
    }
    auto entries = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[maxEffectRecords]);
    if (!entries)
        return std::unexpected(EffectRefusal::NoMemory);
    for (std::size_t index = 0; index < continuation.recordCount; ++index) {
        const auto& view = continuation.records[index];
        auto& entry = entries[index];
        const auto bytes = view.payload.size() + view.data.size();
        if (bytes) {
            entry.payload.reset(new (std::nothrow) std::uint8_t[bytes]);
            if (!entry.payload)
                return std::unexpected(EffectRefusal::NoMemory);
            std::copy(view.payload.begin(), view.payload.end(), entry.payload.get());
            std::copy(view.data.begin(), view.data.end(), entry.payload.get() + view.payload.size());
        }
        entry.view = view;
        entry.view.payload = { entry.payload.get(), view.payload.size() };
        entry.view.data = { bytes ? entry.payload.get() + view.payload.size() : nullptr, view.data.size() };
    }
    for (std::size_t index = 0; index < continuation.recordCount; ++index)
        m_entries[index] = std::move(entries[index]);
    for (std::size_t index = 0; index < continuation.readerCount; ++index) {
        const auto& reader = continuation.readers[index];
        m_readers[index] = { .attachmentID = reader.attachmentID, .clientInstance = reader.clientInstance, .state = reader.state, .acknowledged = reader.acknowledged, .sent = reader.sent, .lostSinceMicroseconds = reader.lostSinceMicroseconds, .isTransient = false, .lifetime = {} };
    }
    m_size = continuation.recordCount;
    m_retainedBytes = retainedBytes;
    m_lastSequence = continuation.lastSequence;
    m_lastGeneration = continuation.lastGeneration;
    m_retiredThrough = continuation.retiredThrough;
    return {};
}

std::expected<void, EffectRefusal> EffectLog::admit(const EffectDraft& draft) const
{
    if (!isPolicyValid() || !draft.count || draft.generation < m_lastGeneration
        || std::to_underlying(draft.kind) > std::to_underlying(EffectKind::ClipboardWrite)
        || (draft.kind != EffectKind::ClipboardWrite && draft.target)
        || (!draft.data.empty() && draft.kind != EffectKind::Sound && draft.kind != EffectKind::ClipboardWrite))
        return std::unexpected(EffectRefusal::Invalid);
    if (m_lastSequence == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(EffectRefusal::ExhaustedIdentity);
    if (m_size == m_policy.maxRecords || draft.payload.size() > m_policy.maxPayloadBytes
        || draft.payload.size() > m_policy.maxRetainedBytes - m_retainedBytes
        || draft.data.size() > m_policy.maxRetainedBytes - m_retainedBytes - draft.payload.size())
        return std::unexpected(EffectRefusal::OverLimit);
    return {};
}

std::expected<std::uint64_t, EffectRefusal> EffectLog::append(const EffectDraft& draft)
{
    if (const auto admitted = admit(draft); !admitted)
        return std::unexpected(admitted.error());
    const std::size_t bytes = draft.payload.size() + draft.data.size();
    std::unique_ptr<std::uint8_t[]> payload;
    if (bytes) {
        payload.reset(new (std::nothrow) std::uint8_t[bytes]);
        if (!payload)
            return std::unexpected(EffectRefusal::NoMemory);
        std::copy(draft.payload.begin(), draft.payload.end(), payload.get());
        std::copy(draft.data.begin(), draft.data.end(), payload.get() + draft.payload.size());
    }
    const std::uint64_t sequence = m_lastSequence + 1;
    Entry& entry = m_entries[m_size++];
    entry.payload = std::move(payload);
    entry.view = { .paneID = m_paneID, .generation = draft.generation, .sequence = sequence, .primary = draft.primary, .kind = draft.kind, .count = draft.count, .target = draft.target, .payload = { entry.payload.get(), draft.payload.size() }, .data = { bytes ? entry.payload.get() + draft.payload.size() : nullptr, draft.data.size() } };
    m_retainedBytes += bytes;
    m_lastSequence = sequence;
    m_lastGeneration = draft.generation;
    return sequence;
}

std::expected<std::uint64_t, EffectRefusal> EffectLog::appendOwnedData(EffectDraft draft,
    std::unique_ptr<std::uint8_t[]>& data, std::size_t dataBytes)
{
    if (!draft.data.empty() || (!data && dataBytes) || (data && !dataBytes))
        return std::unexpected(EffectRefusal::Invalid);
    draft.data = { data.get(), dataBytes };
    if (const auto admitted = admit(draft); !admitted)
        return std::unexpected(admitted.error());
    std::unique_ptr<std::uint8_t[]> payload;
    if (!draft.payload.empty()) {
        payload.reset(new (std::nothrow) std::uint8_t[draft.payload.size()]);
        if (!payload)
            return std::unexpected(EffectRefusal::NoMemory);
        std::copy(draft.payload.begin(), draft.payload.end(), payload.get());
    }
    const std::uint64_t sequence = m_lastSequence + 1;
    Entry& entry = m_entries[m_size++];
    entry.payload = std::move(payload);
    entry.data = std::move(data);
    entry.view = { .paneID = m_paneID, .generation = draft.generation, .sequence = sequence, .primary = draft.primary, .kind = draft.kind, .count = draft.count, .target = draft.target, .payload = { entry.payload.get(), draft.payload.size() }, .data = { entry.data.get(), dataBytes } };
    m_retainedBytes += draft.payload.size() + dataBytes;
    m_lastSequence = sequence;
    m_lastGeneration = draft.generation;
    return sequence;
}

std::expected<EffectBatch, EffectRefusal> EffectLog::readAfter(std::uint64_t sequence, EffectReadMode mode) const
{
    if (mode != EffectReadMode::Consecutive && mode != EffectReadMode::Skipped)
        return std::unexpected(EffectRefusal::Invalid);
    if (sequence < m_retiredThrough)
        return std::unexpected(EffectRefusal::RetiredCursor);
    if (sequence > m_lastSequence)
        return std::unexpected(EffectRefusal::InvalidAcknowledgement);
    EffectBatch batch;
    batch.throughSequence = m_lastSequence;
    for (std::size_t index = 0; index < m_size; ++index) {
        const EffectView& effect = m_entries[index].view;
        if (effect.sequence <= sequence)
            continue;
        if (mode == EffectReadMode::Consecutive) {
            batch.records[batch.size++] = effect;
            continue;
        }
        if (effect.kind == EffectKind::Sound)
            continue;
        if (effect.kind == EffectKind::Notification) {
            const auto following = std::count_if(m_entries.begin() + index + 1, m_entries.begin() + m_size,
                [](const Entry& candidate) { return candidate.view.kind == EffectKind::Notification; });
            if (static_cast<std::size_t>(following) >= m_policy.notificationsKept)
                continue;
        }
        const auto matching = [&](const EffectView& candidate) {
            if (effect.kind == EffectKind::Notification || candidate.kind != effect.kind)
                return false;
            if (effect.kind == EffectKind::CommandFinished)
                return true;
            return candidate.primary == effect.primary && candidate.target == effect.target;
        };
        const auto previous = std::find_if(batch.records.begin(), batch.records.begin() + batch.size, matching);
        EffectView combined = effect;
        if (previous != batch.records.begin() + batch.size) {
            if (effect.kind == EffectKind::Bell) {
                if (previous->count > std::numeric_limits<std::uint32_t>::max() - effect.count)
                    return std::unexpected(EffectRefusal::OverLimit);
                combined.count += previous->count;
            }
            std::move(previous + 1, batch.records.begin() + batch.size, previous);
            --batch.size;
        }
        batch.records[batch.size++] = combined;
    }
    return batch;
}

EffectLog::Reader* EffectLog::reader(ID128 attachmentID)
{
    for (Reader& current : m_readers) {
        if (current.isTransient && current.lifetime.expired())
            current.state = AttachmentState::Detached;
    }
    const auto found = std::find_if(m_readers.begin(), m_readers.end(), [&](const Reader& candidate) {
        return candidate.state != AttachmentState::Detached && candidate.attachmentID == attachmentID;
    });
    return found == m_readers.end() ? nullptr : &*found;
}

std::expected<void, EffectRefusal> EffectLog::addReader(ID128 attachmentID, ID128 clientInstance, std::uint64_t acknowledgedSequence)
{
    if (attachmentID == ID128 {} || clientInstance == ID128 {} || reader(attachmentID))
        return std::unexpected(EffectRefusal::Invalid);
    if (acknowledgedSequence < m_retiredThrough || acknowledgedSequence > m_lastSequence)
        return std::unexpected(EffectRefusal::InvalidAcknowledgement);
    const auto free = std::find_if(m_readers.begin(), m_readers.end(), [](const Reader& candidate) { return candidate.state == AttachmentState::Detached; });
    if (free == m_readers.end())
        return std::unexpected(EffectRefusal::OverLimit);
    *free = { .attachmentID = attachmentID, .clientInstance = clientInstance, .state = AttachmentState::Live, .acknowledged = acknowledgedSequence, .sent = acknowledgedSequence, .lostSinceMicroseconds = 0, .isTransient = false, .lifetime = {} };
    return {};
}

std::expected<void, EffectRefusal> EffectLog::addTransientReader(ID128 attachmentID, ID128 clientInstance,
    std::uint64_t acknowledgedSequence, std::weak_ptr<const EffectReaderLifetime> lifetime)
{
    if (lifetime.expired())
        return std::unexpected(EffectRefusal::Invalid);
    const auto added = addReader(attachmentID, clientInstance, acknowledgedSequence);
    if (!added)
        return added;
    Reader* current = reader(attachmentID);
    current->isTransient = true;
    current->lifetime = std::move(lifetime);
    return {};
}

std::expected<void, EffectRefusal> EffectLog::noteSent(ID128 attachmentID, std::uint64_t throughSequence)
{
    Reader* current = reader(attachmentID);
    if (!current)
        return std::unexpected(EffectRefusal::UnknownReader);
    if (current->state != AttachmentState::Live || throughSequence < current->sent || throughSequence > m_lastSequence)
        return std::unexpected(EffectRefusal::InvalidAcknowledgement);
    current->sent = throughSequence;
    return {};
}

std::expected<void, EffectRefusal> EffectLog::resumeReader(ID128 attachmentID, ID128 clientInstance, std::uint64_t acknowledgedSequence)
{
    Reader* current = reader(attachmentID);
    if (!current)
        return std::unexpected(EffectRefusal::UnknownReader);
    if (clientInstance == ID128 {} || current->clientInstance != clientInstance)
        return std::unexpected(EffectRefusal::Invalid);
    if (acknowledgedSequence < current->acknowledged || acknowledgedSequence > current->sent)
        return std::unexpected(EffectRefusal::InvalidAcknowledgement);
    current->acknowledged = acknowledgedSequence;
    current->state = AttachmentState::Live;
    current->lostSinceMicroseconds = 0;
    return {};
}

void EffectLog::reconcileReaders(const AttachmentTable& table, std::uint64_t nowMicroseconds)
{
    for (Reader& current : m_readers) {
        if (current.isTransient)
            continue;
        if (current.state == AttachmentState::Detached)
            continue;
        const AttachmentRecord* record = table.find(current.attachmentID);
        const bool isHeld = record && std::ranges::any_of(record->panes, [&](const auto& pane) { return pane->paneID == m_paneID && pane->isHeld; });
        if (!record || record->clientInstance != current.clientInstance || !isHeld) {
            current.state = AttachmentState::Detached;
            continue;
        }
        current.state = record->state;
        if (record->state == AttachmentState::Lost)
            current.lostSinceMicroseconds = record->lostSinceMicroseconds;
    }
    trim(nowMicroseconds);
}

std::expected<void, EffectRefusal> EffectLog::acknowledge(ID128 attachmentID, std::uint64_t throughSequence)
{
    Reader* current = reader(attachmentID);
    if (!current)
        return std::unexpected(EffectRefusal::UnknownReader);
    if (current->state != AttachmentState::Live || throughSequence < current->acknowledged || throughSequence > current->sent)
        return std::unexpected(EffectRefusal::InvalidAcknowledgement);
    current->acknowledged = throughSequence;
    return {};
}

std::expected<void, EffectRefusal> EffectLog::noteState(ID128 attachmentID, AttachmentState state, std::uint64_t nowMicroseconds)
{
    Reader* current = reader(attachmentID);
    if (!current)
        return std::unexpected(EffectRefusal::UnknownReader);
    if (state != AttachmentState::Live && state != AttachmentState::Lost && state != AttachmentState::Detached)
        return std::unexpected(EffectRefusal::Invalid);
    if (state == AttachmentState::Lost && current->state != AttachmentState::Lost)
        current->lostSinceMicroseconds = nowMicroseconds;
    current->state = state;
    return {};
}

void EffectLog::trim(std::uint64_t nowMicroseconds)
{
    std::uint64_t through = m_lastSequence;
    for (Reader& current : m_readers) {
        if (current.isTransient && current.lifetime.expired())
            current.state = AttachmentState::Detached;
        if (current.state == AttachmentState::Lost && nowMicroseconds >= current.lostSinceMicroseconds
            && nowMicroseconds - current.lostSinceMicroseconds >= m_policy.lostGraceMicroseconds)
            current.state = AttachmentState::Detached;
        if (current.state != AttachmentState::Detached)
            through = std::min(through, current.acknowledged);
    }
    std::size_t count = 0;
    while (count < m_size && m_entries[count].view.sequence <= through) {
        m_retainedBytes -= m_entries[count].view.payload.size() + m_entries[count].view.data.size();
        ++count;
    }
    if (count)
        std::move(m_entries.begin() + count, m_entries.begin() + m_size, m_entries.begin());
    const std::size_t remaining = m_size - count;
    for (std::size_t index = remaining; index < m_size; ++index)
        m_entries[index] = {};
    m_size = remaining;
    m_retiredThrough = through;
}

std::size_t EffectLog::readerCount() const
{
    return static_cast<std::size_t>(std::count_if(m_readers.begin(), m_readers.end(), [](const Reader& current) {
        return current.state != AttachmentState::Detached && (!current.isTransient || !current.lifetime.expired());
    }));
}

} // namespace Ubiquitty::Protocol::Sync
