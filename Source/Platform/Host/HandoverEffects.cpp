// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverEffects.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
constexpr std::uint16_t version = 1;
constexpr std::array origins { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote };
constexpr std::array tiers { CapabilityTier::HostEscaping, CapabilityTier::ReadExfil, CapabilityTier::LowRiskInfo };

// The same walk measures before allocating and fills one exactly sized buffer.
class Writer {
public:
    explicit Writer(std::size_t limit, std::span<std::uint8_t> output = {})
        : m_limit(limit)
        , m_output(output)
    {
    }

    void raw(std::span<const std::uint8_t> bytes)
    {
        if (m_didOverflow || bytes.size() > m_limit - m_size) {
            m_didOverflow = true;
            return;
        }
        if (!m_output.empty() && !bytes.empty())
            std::memcpy(m_output.data() + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    template<typename T> void scalar(T value)
    {
        static_assert(std::is_unsigned_v<T>);
        std::array<std::uint8_t, sizeof(T)> bytes {};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
        raw(bytes);
    }
    void blob(std::span<const std::uint8_t> bytes)
    {
        scalar(static_cast<std::uint64_t>(bytes.size()));
        raw(bytes);
    }
    void text(std::string_view text) { blob({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }); }
    void identity(const std::optional<Sync::ID128>& identity)
    {
        scalar(static_cast<std::uint8_t>(identity.has_value()));
        if (identity) {
            scalar(identity->low);
            scalar(identity->high);
        }
    }
    [[nodiscard]] std::size_t size() const { return m_size; }
    [[nodiscard]] bool didOverflow() const { return m_didOverflow; }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    bool m_didOverflow = false;
};

struct BorrowedPending {
    CapabilityOrigin origin = CapabilityOrigin::Local;
    std::string_view identity;
    std::uint32_t requestID = 0;
    std::span<const std::uint8_t> bytes;
    std::array<ClipboardBuffer, static_cast<std::size_t>(ClipboardBuffer::Count)> buffers {};
    std::size_t bufferCount = 0;
};

struct BorrowedEffect {
    std::uint8_t kind = 0;
    std::optional<Sync::ID128> primary;
    std::uint32_t pendingPrimaryClientID = 0;
    ClipboardBuffer buffer = ClipboardBuffer::Clipboard;
    std::span<const std::uint8_t> clipboard;
    std::string_view title;
    std::string_view body;
    std::string_view identifier;
    NotificationOccasion occasion = NotificationOccasion::Always;
};

struct BorrowedState {
    CapabilityDecisionTable decisions;
    std::string_view currentIdentity;
    bool wasClipboardWriteRefused = false;
    std::optional<BorrowedPending> pending;
    std::array<BorrowedEffect, HostEffectQueue::capacity> effects {};
    std::size_t effectCount = 0;
};

bool readBoolean(Sync::WireReader& reader)
{
    const auto value = reader.u8();
    if (value > 1)
        reader.fail();
    return value != 0;
}

std::span<const std::uint8_t> readBlob(Sync::WireReader& reader, std::size_t limit)
{
    const auto size = reader.u64();
    if (size > limit || size > reader.remaining()) {
        reader.fail();
        return {};
    }
    return reader.take(static_cast<std::size_t>(size));
}

std::string_view readText(Sync::WireReader& reader, std::size_t limit)
{
    const auto bytes = readBlob(reader, limit);
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

std::expected<BorrowedState, HandoverEffectsError> readState(std::span<const std::uint8_t> bytes, const HandoverEffectsLimits& limits)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(HandoverEffectsError::OverCapacity);
    Sync::WireReader reader(bytes);
    const auto carriedVersion = reader.u16();
    if (reader.ok() && carriedVersion > version)
        return std::unexpected(HandoverEffectsError::NewerVersion);
    if (!reader.ok() || carriedVersion != version || bytes.size() < 18)
        return std::unexpected(HandoverEffectsError::Malformed);
    Sync::WireReader digestReader(bytes.last(16));
    const Sync::Digest128 digest { digestReader.u64(), digestReader.u64() };
    if (digest != Sync::digestOf(bytes.first(bytes.size() - 16)))
        return std::unexpected(HandoverEffectsError::DigestMismatch);
    reader = Sync::WireReader(bytes.subspan(2, bytes.size() - 18));
    BorrowedState state;
    for (const auto origin : origins) {
        for (const auto tier : tiers) {
            const auto value = reader.u8();
            if (value > std::to_underlying(CapabilityDecision::Prompt) + 1)
                reader.fail();
            else if (value)
                state.decisions = state.decisions.replacing(origin, tier, static_cast<CapabilityDecision>(value - 1));
        }
    }
    state.currentIdentity = readText(reader, limits.maxIdentityBytes);
    state.wasClipboardWriteRefused = readBoolean(reader);
    if (readBoolean(reader)) {
        auto& pending = state.pending.emplace();
        pending.origin = static_cast<CapabilityOrigin>(reader.u8());
        pending.identity = readText(reader, limits.maxIdentityBytes);
        pending.requestID = reader.u32();
        pending.bufferCount = reader.u8();
        if (pending.origin > CapabilityOrigin::Remote || pending.identity.empty()
            || !pending.bufferCount || pending.bufferCount > pending.buffers.size())
            return std::unexpected(HandoverEffectsError::Malformed);
        for (std::size_t i = 0; i < pending.bufferCount; ++i) {
            pending.buffers[i] = static_cast<ClipboardBuffer>(reader.u8());
            if (pending.buffers[i] >= ClipboardBuffer::Count)
                reader.fail();
        }
        pending.bytes = readBlob(reader, std::min(limits.maxClipboardBytes, Sync::maxEffectClipboardWriteBytes));
    }
    state.effectCount = reader.u8();
    if (state.effectCount > state.effects.size())
        return std::unexpected(HandoverEffectsError::OverCapacity);
    for (std::size_t i = 0; i < state.effectCount; ++i) {
        auto& effect = state.effects[i];
        effect.kind = reader.u8();
        if (readBoolean(reader)) {
            effect.primary = Sync::ID128 { reader.u64(), reader.u64() };
            if (*effect.primary == Sync::ID128 {})
                reader.fail();
        }
        effect.pendingPrimaryClientID = reader.u32();
        if (effect.kind == 0) {
            effect.buffer = static_cast<ClipboardBuffer>(reader.u8());
            effect.clipboard = readBlob(reader, limits.maxClipboardBytes);
            if (effect.buffer >= ClipboardBuffer::Count || !Sync::decodeEffectClipboard(effect.clipboard, Sync::maxEffectClipboardWriteBytes))
                reader.fail();
        } else if (effect.kind == 1) {
            effect.title = readText(reader, limits.maxNotificationBytes);
            effect.body = readText(reader, limits.maxNotificationBytes);
            effect.identifier = readText(reader, limits.maxNotificationBytes);
            effect.occasion = static_cast<NotificationOccasion>(reader.u8());
            if (effect.occasion > NotificationOccasion::Invisible)
                reader.fail();
        } else
            reader.fail();
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverEffectsError::Malformed);
    return state;
}

void writeState(Writer& writer, const HostCapabilityPolicy& policy, const HostEffectQueue& effects)
{
    writer.scalar(version);
    for (const auto origin : origins) {
        for (const auto tier : tiers) {
            const auto value = policy.decisions().configuredDecision(origin, tier);
            writer.scalar(static_cast<std::uint8_t>(value ? std::to_underlying(*value) + 1 : 0));
        }
    }
    writer.text(policy.currentIdentity());
    writer.scalar(static_cast<std::uint8_t>(policy.hasClipboardWriteRefusal()));
    const auto* pending = policy.pendingClipboardWrite();
    writer.scalar(static_cast<std::uint8_t>(pending != nullptr));
    if (pending) {
        writer.scalar(std::to_underlying(pending->origin));
        writer.text(pending->identity);
        writer.scalar(pending->requestID);
        writer.scalar(static_cast<std::uint8_t>(pending->bufferCount));
        for (std::size_t i = 0; i < pending->bufferCount; ++i)
            writer.scalar(std::to_underlying(pending->buffers[i]));
        writer.blob({ reinterpret_cast<const std::uint8_t*>(pending->bytes.get()), pending->byteCount });
    }
    writer.scalar(static_cast<std::uint8_t>(effects.heldEffects().size()));
    for (const auto& effect : effects.heldEffects()) {
        writer.scalar(static_cast<std::uint8_t>(effect.index()));
        std::visit([&](const auto& value) {
            writer.identity(value.primary);
            writer.scalar(value.pendingPrimaryClientID);
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, HostEffectQueue::ClipboardWrite>) {
                writer.scalar(std::to_underlying(value.buffer));
                writer.blob(value.encoded.view());
            } else {
                writer.text(value.title);
                writer.text(value.body);
                writer.text(value.identifier);
                writer.scalar(std::to_underlying(value.occasion));
            }
        },
            effect);
    }
}

} // namespace

std::expected<std::vector<std::uint8_t>, HandoverEffectsError> encodeHandoverEffects(
    const HostCapabilityPolicy& policy, const HostEffectQueue& effects, const HandoverEffectsLimits& limits)
{
    if (policy.currentIdentity().size() > limits.maxIdentityBytes || effects.heldEffects().size() > HostEffectQueue::capacity)
        return std::unexpected(HandoverEffectsError::OverCapacity);
    if (const auto* pending = policy.pendingClipboardWrite(); pending) {
        if (pending->bufferCount > pending->buffers.size() || pending->identity.size() > limits.maxIdentityBytes
            || pending->byteCount > limits.maxClipboardBytes)
            return std::unexpected(HandoverEffectsError::OverCapacity);
        if (pending->byteCount && !pending->bytes)
            return std::unexpected(HandoverEffectsError::Malformed);
    }
    for (const auto& effect : effects.heldEffects()) {
        if (const auto* clipboard = std::get_if<HostEffectQueue::ClipboardWrite>(&effect)) {
            if (clipboard->encoded.size > limits.maxClipboardBytes)
                return std::unexpected(HandoverEffectsError::OverCapacity);
            if (clipboard->encoded.size && !clipboard->encoded.bytes)
                return std::unexpected(HandoverEffectsError::Malformed);
        } else {
            const auto& notification = std::get<HostNotification>(effect);
            if (notification.title.size() > limits.maxNotificationBytes || notification.body.size() > limits.maxNotificationBytes
                || notification.identifier.size() > limits.maxNotificationBytes)
                return std::unexpected(HandoverEffectsError::OverCapacity);
        }
    }
    Writer measure(limits.maxSectionBytes);
    writeState(measure, policy, effects);
    measure.scalar(std::uint64_t { 0 });
    measure.scalar(std::uint64_t { 0 });
    if (measure.didOverflow())
        return std::unexpected(HandoverEffectsError::OverCapacity);
    std::vector<std::uint8_t> output(measure.size());
    Writer writer(output.size(), output);
    writeState(writer, policy, effects);
    const auto digest = Sync::digestOf(std::span(output).first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    // The same field admission is used for both sides. It borrows only fixed
    // metadata, so an inconsistent source never returns an unrestorable record.
    if (const auto admitted = readState(output, limits); !admitted)
        return std::unexpected(admitted.error());
    return output;
}

std::expected<HandoverEffects, HandoverEffectsError> decodeHandoverEffects(
    std::span<const std::uint8_t> bytes, const HandoverEffectsLimits& limits)
{
    auto admitted = readState(bytes, limits);
    if (!admitted)
        return std::unexpected(admitted.error());
    HandoverEffects state;
    state.policy.decisions = admitted->decisions;
    state.policy.currentIdentity = admitted->currentIdentity;
    state.policy.wasClipboardWriteRefused = admitted->wasClipboardWriteRefused;
    if (const auto& pending = admitted->pending; pending) {
        state.policy.pendingClipboardWrite = HostCapabilityPolicy::PendingClipboardWrite::capture(pending->origin,
            pending->identity, std::span(pending->buffers).first(pending->bufferCount),
            { reinterpret_cast<const char*>(pending->bytes.data()), pending->bytes.size() });
        if (!state.policy.pendingClipboardWrite)
            return std::unexpected(HandoverEffectsError::AllocationRefused);
        state.policy.pendingClipboardWrite->requestID = pending->requestID;
    }
    state.effects.reserve(admitted->effectCount);
    for (std::size_t i = 0; i < admitted->effectCount; ++i) {
        const auto& effect = admitted->effects[i];
        if (!effect.kind) {
            Sync::EffectCodecBytes owned { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[effect.clipboard.size()]), effect.clipboard.size() };
            if (!owned.bytes)
                return std::unexpected(HandoverEffectsError::AllocationRefused);
            if (!effect.clipboard.empty())
                std::memcpy(owned.bytes.get(), effect.clipboard.data(), effect.clipboard.size());
            state.effects.emplace_back(HostEffectQueue::ClipboardWrite { effect.buffer, std::move(owned), effect.primary, effect.pendingPrimaryClientID });
        } else
            state.effects.emplace_back(HostNotification { std::string(effect.title), std::string(effect.body), std::string(effect.identifier),
                effect.occasion, effect.primary, effect.pendingPrimaryClientID });
    }
    return state;
}

} // namespace Ubiquitty::Host
