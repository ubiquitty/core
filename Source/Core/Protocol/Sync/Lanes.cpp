// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Lanes.h"

#include "Core/Codec/ZlibInflate.h"

#include "Core/Crypto/SHA256.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

std::array<std::uint8_t, 32> sha256Of(std::span<const std::uint8_t> bytes)
{
    std::array<std::uint8_t, 32> digest {};
    Crypto::sha256(bytes, digest);
    return digest;
}

} // namespace

std::size_t LaneScheduler::retainedOf(const QueuedFrame& frame)
{
    return frame.bytes.capacity() + frame.releasedTransfers.capacity() * sizeof(std::uint32_t);
}

std::size_t LaneScheduler::retainedOf(const Transfer& transfer)
{
    return transfer.payload.capacity() + transfer.ownedPayloadBytes;
}

std::size_t LaneScheduler::countRetainedPayloadBytes() const
{
    std::size_t bytes = 0;
    for (const auto& frame : m_interactive)
        bytes += retainedOf(frame);
    for (const auto& frame : m_bulkFrames)
        bytes += retainedOf(frame);
    for (const auto& transfer : m_transfers)
        bytes += retainedOf(transfer);
    return bytes;
}

std::size_t LaneScheduler::enqueue(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body, FrameRole role)
{
    const bool isBulkFrame = kind == FrameKind::NativeDragCommand || kind == FrameKind::NativeDragEvent;
    if (laneOf(kind) != Lane::Interactive && !isBulkFrame)
        return 0;
    if (body.size() > maxFrameBodyBytes || (channel == 0) != isConnectionKind(kind))
        return 0;
    QueuedFrame frame { .kind = kind, .channel = channel, .role = role, .bytes = {}, .releasedTransfers = {}, .headOf = std::nullopt };
    frame.bytes.reserve(frameBytes(channel, body.size()));
    if (!appendFrame(frame.bytes, kind, channel, body))
        return 0;
    const std::size_t bytes = frame.bytes.size();
    m_retainedPayloadBytes += retainedOf(frame);
    if (isBulkFrame) {
        m_bulkFrames.push_back(std::move(frame));
        m_queuedBulkBytes += bytes;
    } else
        m_interactive.push_back(std::move(frame));
    m_queuedBytes += bytes;
    m_queuedReplies += role == FrameRole::Reply ? 1 : 0;
    return bytes;
}

std::size_t LaneScheduler::enqueueTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::vector<std::uint8_t> payload, TransferRank rank, FrameRole role, PayloadCoding coding)
{
    if (laneOf(headKind) != Lane::Interactive)
        return 0;
    if (headBody.size() > maxFrameBodyBytes || (headChannel == 0) != isConnectionKind(headKind))
        return 0;
    QueuedFrame head { .kind = headKind, .channel = headChannel, .role = FrameRole::Message, .bytes = {}, .releasedTransfers = {}, .headOf = std::nullopt };
    head.bytes.reserve(frameBytes(headChannel, headBody.size()));
    if (!appendFrame(head.bytes, headKind, headChannel, headBody))
        return 0;
    const std::uint32_t id = m_nextTransferID++;
    head.releasedTransfers.push_back(id);
    head.headOf = id;
    const std::size_t bytes = head.bytes.size();
    m_retainedPayloadBytes += retainedOf(head);
    m_interactive.push_back(std::move(head));
    m_queuedBytes += bytes + payload.size();
    m_queuedBulkBytes += payload.size();
    m_queuedReplies += role == FrameRole::Reply ? 1 : 0;
    Transfer transfer;
    transfer.id = id;
    transfer.rank = rank;
    transfer.role = role;
    transfer.coding = coding;
    transfer.sha256 = sha256Of(payload);
    transfer.payload = std::move(payload);
    m_retainedPayloadBytes += retainedOf(transfer);
    m_transfers.push_back(std::move(transfer));
    return bytes;
}

std::size_t LaneScheduler::enqueueCopiedTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::span<const std::uint8_t> payload, TransferRank rank, FrameRole role)
{
    if (laneOf(headKind) != Lane::Interactive || headBody.size() > maxFrameBodyBytes
        || (headChannel == 0) != isConnectionKind(headKind))
        return 0;
    std::unique_ptr<std::uint8_t[]> owner;
    if (!payload.empty()) {
        owner.reset(new (std::nothrow) std::uint8_t[payload.size()]);
        if (!owner)
            return 0;
        std::copy(payload.begin(), payload.end(), owner.get());
    }
    QueuedFrame head { .kind = headKind, .channel = headChannel, .role = FrameRole::Message, .bytes = {}, .releasedTransfers = {}, .headOf = std::nullopt };
    head.bytes.reserve(frameBytes(headChannel, headBody.size()));
    if (!appendFrame(head.bytes, headKind, headChannel, headBody))
        return 0;
    const std::uint32_t id = m_nextTransferID++;
    head.releasedTransfers.push_back(id);
    head.headOf = id;
    const std::size_t bytes = head.bytes.size();
    Transfer transfer;
    transfer.id = id;
    transfer.rank = rank;
    transfer.role = role;
    transfer.sha256 = sha256Of(payload);
    transfer.ownedPayload = std::move(owner);
    transfer.ownedPayloadBytes = payload.size();
    m_retainedPayloadBytes += retainedOf(head) + retainedOf(transfer);
    m_interactive.push_back(std::move(head));
    m_transfers.push_back(std::move(transfer));
    m_queuedBytes += bytes + payload.size();
    m_queuedBulkBytes += payload.size();
    m_queuedReplies += role == FrameRole::Reply ? 1 : 0;
    return bytes;
}

std::size_t LaneScheduler::enqueueSourceTransfer(FrameKind headKind, std::uint32_t headChannel, std::span<const std::uint8_t> headBody, std::unique_ptr<BulkSource> source, TransferRank rank, FrameRole role, PayloadCoding coding)
{
    if (!source || source->bytes().empty() || source->retainedBytes() < source->bytes().size()
        || source->retainedBytes() - source->bytes().size() < bulkSourceFrameReserveBytes || laneOf(headKind) != Lane::Interactive
        || (coding.compression == Compression::None) != (coding.inflatedBytes == 0))
        return 0;
    if (headBody.size() > maxFrameBodyBytes || (headChannel == 0) != isConnectionKind(headKind))
        return 0;
    QueuedFrame head { .kind = headKind, .channel = headChannel, .role = FrameRole::Message, .bytes = {}, .releasedTransfers = {}, .headOf = std::nullopt };
    head.bytes.reserve(frameBytes(headChannel, headBody.size()));
    if (!appendFrame(head.bytes, headKind, headChannel, headBody))
        return 0;
    const std::uint32_t id = m_nextTransferID++;
    head.releasedTransfers.push_back(id);
    head.headOf = id;
    const std::size_t bytes = head.bytes.size();
    Transfer transfer;
    transfer.id = id;
    transfer.rank = rank;
    transfer.role = role;
    transfer.sha256 = sha256Of(source->bytes());
    transfer.coding = coding;
    transfer.source = std::move(source);
    m_retainedSourceBytes += transfer.source->retainedBytes();
    m_queuedBytes += bytes;
    m_queuedReplies += role == FrameRole::Reply ? 1 : 0;
    m_retainedPayloadBytes += retainedOf(head) + retainedOf(transfer);
    m_interactive.push_back(std::move(head));
    m_transfers.push_back(std::move(transfer));
    return bytes;
}

std::optional<std::size_t> LaneScheduler::sendableTransfer() const
{
    std::optional<std::size_t> best;
    for (std::size_t index = 0; index < m_transfers.size(); ++index) {
        const Transfer& transfer = m_transfers[index];
        if (transfer.isReleased && (!best || transfer.rank < m_transfers[*best].rank))
            best = index;
    }
    return best;
}

std::pair<std::size_t, bool> LaneScheduler::nextSlice(const Transfer& transfer, std::size_t payloadLimit)
{
    const std::size_t capacity = std::min(payloadLimit, bulkChunkPayloadBytes);
    const std::size_t data = std::min(capacity, transfer.bytes().size() - transfer.offset);
    return { data, transfer.offset + data == transfer.bytes().size() };
}

std::size_t LaneScheduler::nextBulkFrameBytes(std::size_t payloadLimit) const
{
    if (!m_bulkFrames.empty() && m_bulkFrames.front().bytes.size() <= payloadLimit)
        return m_bulkFrames.front().bytes.size();
    const std::optional<std::size_t> index = sendableTransfer();
    if (!index || !payloadLimit)
        return 0;
    const Transfer& transfer = m_transfers[*index];
    const auto [data, isLast] = nextSlice(transfer, payloadLimit);
    std::size_t lengthBytes = 1;
    for (std::size_t value = data; value >= 0x80; value >>= 7)
        ++lengthBytes;
    const std::size_t codingBytes = transfer.coding.compression == Compression::Deflate ? 1 + 4 : 0;
    return frameBytes(0, 4 + 4 + 1 + lengthBytes + data + (isLast ? 32 + codingBytes : 0));
}

bool LaneScheduler::isQueued(FrameKind kind, std::uint32_t channel) const
{
    const auto matches = [&](const QueuedFrame& frame) { return frame.kind == kind && frame.channel == channel; };
    return std::ranges::any_of(m_interactive, matches) || std::ranges::any_of(m_bulkFrames, matches);
}

std::deque<LaneScheduler::QueuedFrame>::iterator LaneScheduler::eraseFrame(std::deque<QueuedFrame>::iterator frame)
{
    m_retainedPayloadBytes -= retainedOf(*frame);
    const std::vector<std::uint32_t> released = std::move(frame->releasedTransfers);
    m_queuedBytes -= frame->bytes.size();
    m_queuedReplies -= frame->role == FrameRole::Reply ? 1 : 0;
    const auto position = m_interactive.erase(frame);
    if (position != m_interactive.begin()) {
        QueuedFrame& previous = *std::prev(position);
        m_retainedPayloadBytes -= retainedOf(previous);
        previous.releasedTransfers.insert(previous.releasedTransfers.end(), released.begin(), released.end());
        m_retainedPayloadBytes += retainedOf(previous);
    } else {
        for (Transfer& transfer : m_transfers) {
            if (std::ranges::find(released, transfer.id) != released.end())
                transfer.isReleased = true;
        }
    }
    return position;
}

std::size_t LaneScheduler::withdraw(FrameKind kind, std::uint32_t channel)
{
    std::size_t bytes = 0;
    std::erase_if(m_bulkFrames, [&](const QueuedFrame& frame) {
        if (frame.kind != kind || frame.channel != channel)
            return false;
        bytes += frame.bytes.size();
        m_queuedBytes -= frame.bytes.size();
        m_queuedBulkBytes -= frame.bytes.size();
        m_queuedReplies -= frame.role == FrameRole::Reply ? 1 : 0;
        m_retainedPayloadBytes -= retainedOf(frame);
        return true;
    });
    for (auto frame = m_interactive.begin(); frame != m_interactive.end();) {
        if (frame->kind != kind || frame->channel != channel || frame->headOf) {
            ++frame;
            continue;
        }
        bytes += frame->bytes.size();
        frame = eraseFrame(frame);
    }
    return bytes;
}

bool LaneScheduler::isTransferQueued(std::uint32_t transfer) const
{
    return std::ranges::any_of(m_transfers, [&](const Transfer& queued) { return queued.id == transfer; });
}

bool LaneScheduler::isTransferAnnounced(std::uint32_t transfer) const
{
    return std::ranges::any_of(m_transfers, [&](const Transfer& queued) { return queued.id == transfer && queued.isReleased; });
}

void LaneScheduler::noteTransferGone(const Transfer& transfer)
{
    m_retainedPayloadBytes -= retainedOf(transfer);
    if (transfer.source) {
        m_retainedSourceBytes -= transfer.source->retainedBytes();
    } else {
        m_queuedBytes -= transfer.bytes().size() - transfer.offset;
        m_queuedBulkBytes -= transfer.bytes().size() - transfer.offset;
    }
    m_queuedReplies -= transfer.role == FrameRole::Reply ? 1 : 0;
}

void LaneScheduler::abandonTransfer(std::uint32_t transfer)
{
    std::erase_if(m_transfers, [&](const Transfer& queued) {
        if (queued.id != transfer)
            return false;
        noteTransferGone(queued);
        return true;
    });
    if (m_transfers.empty() && m_bulkFrames.empty()) {
        m_bytesSinceBulkPending = 0;
        m_bulkBytesSinceBulkPending = 0;
    }
}

TransferAbort LaneScheduler::abortTransfer(std::uint32_t transfer)
{
    const auto queued = std::ranges::find_if(m_transfers, [&](const Transfer& candidate) { return candidate.id == transfer; });
    if (queued == m_transfers.end())
        return TransferAbort::NotQueued;
    const bool wasAnnounced = queued->isReleased;
    noteTransferGone(*queued);
    m_transfers.erase(queued);
    if (m_transfers.empty() && m_bulkFrames.empty()) {
        m_bytesSinceBulkPending = 0;
        m_bulkBytesSinceBulkPending = 0;
    }
    if (wasAnnounced)
        return TransferAbort::Aborted;
    // The head is still queued: it goes too, and the receiver never learns of
    // the transfer. A run released by the head's departure waits for the
    // frame before it instead.
    const auto head = std::ranges::find_if(m_interactive, [&](const QueuedFrame& frame) { return frame.headOf == transfer; });
    if (head != m_interactive.end())
        (void)eraseFrame(head);
    return TransferAbort::DroppedBeforeHead;
}

void LaneScheduler::abandonTransfers()
{
    for (const Transfer& queued : m_transfers)
        noteTransferGone(queued);
    m_transfers.clear();
    m_bytesSinceBulkPending = 0;
    m_bulkBytesSinceBulkPending = 0;
}

void LaneScheduler::abandonAll()
{
    abandonTransfers();
    for (const QueuedFrame& frame : m_interactive) {
        m_queuedBytes -= frame.bytes.size();
        m_queuedReplies -= frame.role == FrameRole::Reply ? 1 : 0;
        m_retainedPayloadBytes -= retainedOf(frame);
    }
    m_interactive.clear();
    for (const auto& frame : m_bulkFrames) {
        m_queuedBytes -= frame.bytes.size();
        m_queuedBulkBytes -= frame.bytes.size();
        m_queuedReplies -= frame.role == FrameRole::Reply ? 1 : 0;
        m_retainedPayloadBytes -= retainedOf(frame);
    }
    m_bulkFrames.clear();
}

bool LaneScheduler::isBulkBehindShare() const
{
    return m_bulkBytesSinceBulkPending * 1000 < static_cast<std::uint64_t>(m_policy.bulkSharePermille) * m_bytesSinceBulkPending;
}

std::optional<Lane> LaneScheduler::popFrame(std::vector<std::uint8_t>& output, BulkPacing bulkPacing, std::size_t payloadLimit)
{
    const std::optional<std::size_t> sendable = sendableTransfer();
    const bool hasBulkFrame = !m_bulkFrames.empty() && m_bulkFrames.front().bytes.size() <= payloadLimit;
    const bool isBulkSendable = payloadLimit && bulkPacing == BulkPacing::Admitted && (hasBulkFrame || sendable.has_value());
    const bool isInteractiveWaiting = !m_interactive.empty();
    if (!isBulkSendable && !isInteractiveWaiting)
        return std::nullopt;
    const bool shouldSendBulk = isBulkSendable && (!isInteractiveWaiting || (!m_wasLastFrameBulk && isBulkBehindShare()));
    const std::size_t before = output.size();
    if (shouldSendBulk) {
        if (hasBulkFrame) {
            auto frame = std::move(m_bulkFrames.front());
            m_bulkFrames.pop_front();
            m_retainedPayloadBytes -= retainedOf(frame);
            m_queuedBytes -= frame.bytes.size();
            m_queuedBulkBytes -= frame.bytes.size();
            m_queuedReplies -= frame.role == FrameRole::Reply ? 1 : 0;
            output.insert(output.end(), frame.bytes.begin(), frame.bytes.end());
        } else
            popChunk(*sendable, output, payloadLimit);
        noteSent(Lane::Bulk, output.size() - before);
        return Lane::Bulk;
    }
    QueuedFrame frame = std::move(m_interactive.front());
    m_interactive.pop_front();
    m_retainedPayloadBytes -= retainedOf(frame);
    m_queuedBytes -= frame.bytes.size();
    m_queuedReplies -= frame.role == FrameRole::Reply ? 1 : 0;
    output.insert(output.end(), frame.bytes.begin(), frame.bytes.end());
    for (const std::uint32_t released : frame.releasedTransfers) {
        for (Transfer& transfer : m_transfers) {
            if (transfer.id == released)
                transfer.isReleased = true;
        }
    }
    noteSent(Lane::Interactive, output.size() - before);
    return Lane::Interactive;
}

void LaneScheduler::popChunk(std::size_t index, std::vector<std::uint8_t>& output, std::size_t payloadLimit)
{
    Transfer& transfer = m_transfers[index];
    const auto [data, isLast] = nextSlice(transfer, payloadLimit);
    BulkChunk chunk;
    chunk.transfer = transfer.id;
    chunk.index = transfer.nextIndex++;
    chunk.bytes = transfer.bytes().subspan(transfer.offset, data);
    transfer.offset += data;
    if (!transfer.source) {
        m_queuedBytes -= data;
        m_queuedBulkBytes -= data;
    }
    chunk.isLast = isLast;
    chunk.sha256 = transfer.sha256;
    chunk.compression = transfer.coding.compression;
    chunk.inflatedBytes = transfer.coding.inflatedBytes;
    {
        std::vector<std::uint8_t> body;
        body.reserve(data + bulkChunkOverheadBytes);
        encode(body, chunk);
        // A chunk is cut to one capped frame and travels on channel 0.
        (void)appendFrame(output, FrameKind::BulkChunk, 0, body);
    }
    if (chunk.isLast) {
        noteTransferGone(transfer);
        m_transfers.erase(m_transfers.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void LaneScheduler::noteSent(Lane lane, std::size_t bytes)
{
    m_wasLastFrameBulk = lane == Lane::Bulk;
    if (lane == Lane::Bulk)
        m_bulkBytesSinceBulkPending += bytes;
    m_bytesSinceBulkPending += bytes;
    if (m_transfers.empty() && m_bulkFrames.empty()) {
        m_bytesSinceBulkPending = 0;
        m_bulkBytesSinceBulkPending = 0;
    }
}

bool BulkReassembler::expect(std::uint32_t transfer, std::size_t maxBytes)
{
    if (isFenced(transfer))
        return false;
    const auto [entry, isNew] = m_transfers.try_emplace(transfer);
    if (isNew) {
        entry->second.maxBytes = maxBytes;
        entry->second.charge = MemoryCharge(m_budget);
    }
    return isNew;
}

std::size_t BulkReassembler::heldBytes() const
{
    std::size_t bytes = 0;
    for (const auto& [transfer, entry] : m_transfers)
        bytes += entry.payload.capacity();
    return bytes;
}

bool BulkReassembler::reserve(Transfer& transfer, std::size_t additional) const
{
    const std::size_t needed = transfer.payload.size() + additional;
    const std::size_t capacity = transfer.payload.capacity();
    if (needed <= capacity)
        return true;
    const std::size_t grown = std::min(std::max(needed, capacity > transfer.maxBytes / 2 ? transfer.maxBytes : 2 * capacity), transfer.maxBytes);
    if (m_budget && !transfer.charge.resize(grown))
        return false;
    transfer.payload.reserve(grown);
    if (m_budget)
        transfer.charge.settle(transfer.payload.capacity());
    return true;
}

ChunkOutcome BulkReassembler::accept(const BulkChunk& chunk)
{
    const auto found = m_transfers.find(chunk.transfer);
    if (found == m_transfers.end()) {
        if (!isFenced(chunk.transfer))
            return ChunkOutcome::Refused;
        // Nothing of the transfer follows its last chunk.
        if (chunk.isLast)
            (void)releaseFence(chunk.transfer);
        return ChunkOutcome::Discarded;
    }
    Transfer& transfer = found->second;
    if (transfer.isComplete || chunk.index != transfer.nextIndex || chunk.bytes.size() > transfer.maxBytes - transfer.payload.size()) {
        m_transfers.erase(found);
        return ChunkOutcome::Refused;
    }
    if (!reserve(transfer, chunk.bytes.size())) {
        abandon(chunk.transfer);
        // Nothing of the transfer follows its last chunk.
        if (chunk.isLast)
            (void)releaseFence(chunk.transfer);
        return ChunkOutcome::OverBudget;
    }
    transfer.payload.insert(transfer.payload.end(), chunk.bytes.begin(), chunk.bytes.end());
    ++transfer.nextIndex;
    if (!chunk.isLast)
        return ChunkOutcome::Pending;
    if (sha256Of(transfer.payload) != chunk.sha256) {
        m_transfers.erase(found);
        return ChunkOutcome::Refused;
    }
    if (chunk.compression == Compression::Deflate) {
        const ChunkOutcome inflated = inflate(transfer, chunk.inflatedBytes);
        if (inflated == ChunkOutcome::OverBudget) {
            abandon(chunk.transfer);
            (void)releaseFence(chunk.transfer);
            return inflated;
        }
        if (inflated != ChunkOutcome::Complete) {
            m_transfers.erase(found);
            return inflated;
        }
    }
    transfer.isComplete = true;
    return ChunkOutcome::Complete;
}

ChunkOutcome BulkReassembler::inflate(Transfer& transfer, std::uint32_t inflatedBytes) const
{
    // The declared length is checked before anything is allocated, and the
    // inflater writes into exactly that much and one byte more: a stream that
    // fills the extra byte inflates past what it declared, and is refused
    // before it can make the client hold more than its bound.
    if (inflatedBytes > transfer.maxBytes)
        return ChunkOutcome::Refused;
    // The deflated payload and the inflated body coexist until the swap; the
    // owner is charged for both before the second is allocated.
    const std::size_t deflatedCapacity = transfer.payload.capacity();
    if (m_budget && !transfer.charge.resize(deflatedCapacity + inflatedBytes + 1))
        return ChunkOutcome::OverBudget;
    std::vector<std::uint8_t> inflated(std::size_t { inflatedBytes } + 1);
    auto inflater = std::make_unique<Codec::Zlib::Inflater>();
    std::span<const std::uint8_t> input(transfer.payload);
    std::size_t produced = 0;
    for (;;) {
        const Codec::Zlib::Inflater::DecodeResult result = inflater->decode(input, std::span<std::uint8_t>(inflated).subspan(produced));
        input = input.subspan(result.bytesConsumed);
        produced += result.bytesProduced;
        if (produced > inflatedBytes)
            return ChunkOutcome::Refused;
        switch (result.status) {
        case Codec::Zlib::InflateStatus::Progressed:
            continue;
        case Codec::Zlib::InflateStatus::Done:
            if (produced != inflatedBytes || !input.empty())
                return ChunkOutcome::Corrupt;
            inflated.resize(produced);
            transfer.payload = std::move(inflated);
            if (m_budget)
                transfer.charge.settle(transfer.payload.capacity());
            return ChunkOutcome::Complete;
        case Codec::Zlib::InflateStatus::NeedMoreOutput:
            // The buffer holds one byte past the declared length, so a stream
            // that wants room past it has already overrun.
            return ChunkOutcome::Refused;
        case Codec::Zlib::InflateStatus::NeedMoreInput:
        case Codec::Zlib::InflateStatus::Error:
            return ChunkOutcome::Corrupt;
        }
        return ChunkOutcome::Corrupt;
    }
}

std::vector<std::uint8_t> BulkReassembler::take(std::uint32_t transfer, MemoryCharge* charge)
{
    const auto found = m_transfers.find(transfer);
    if (found == m_transfers.end() || !found->second.isComplete)
        return {};
    std::vector<std::uint8_t> payload = std::move(found->second.payload);
    if (charge)
        *charge = std::move(found->second.charge);
    m_transfers.erase(found);
    return payload;
}

void BulkReassembler::abandon(std::uint32_t transfer)
{
    if (m_transfers.erase(transfer) == 0)
        return;
    if (m_fenced.size() == maxFencedTransfers)
        m_fenced.pop_front();
    m_fenced.push_back(transfer);
}

bool BulkReassembler::isFenced(std::uint32_t transfer) const
{
    return std::ranges::find(m_fenced, transfer) != m_fenced.end();
}

bool BulkReassembler::releaseFence(std::uint32_t transfer)
{
    const auto found = std::ranges::find(m_fenced, transfer);
    if (found == m_fenced.end())
        return false;
    m_fenced.erase(found);
    return true;
}

} // namespace Ubiquitty::Protocol::Sync
