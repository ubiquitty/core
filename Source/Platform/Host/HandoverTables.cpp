// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverTables.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
using Error = Sync::HandoverBodyError;
constexpr std::uint16_t attachmentVersion = 2;
constexpr std::uint32_t attachmentMagic = 0x41544248;

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
    void count(std::size_t size)
    {
        if (size > UINT32_MAX)
            m_didOverflow = true;
        scalar(static_cast<std::uint32_t>(size));
    }
    void text(std::string_view text)
    {
        count(text.size());
        raw({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    }
    void identity(Sync::ID128 value)
    {
        scalar(value.low);
        scalar(value.high);
    }
    void flag(bool isSet) { scalar(static_cast<std::uint8_t>(isSet)); } // bool-ok: Encodes an existing Boolean field without changing its type.
    [[nodiscard]] std::size_t size() const { return m_size; }
    [[nodiscard]] bool didOverflow() const { return m_didOverflow; }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    bool m_didOverflow = false;
};

void writeBaseline(Writer& writer, const Sync::AttachmentBaseline& value)
{
    writer.scalar(value.dictionaryEpoch);
    writer.scalar(static_cast<std::uint64_t>(value.marks.pens));
    writer.scalar(static_cast<std::uint64_t>(value.marks.links));
    writer.scalar(static_cast<std::uint64_t>(value.marks.clusters));
    writer.flag(value.isAlternate);
    writer.scalar(static_cast<std::uint32_t>(value.columns));
    for (const auto& rows : value.rowHashes) {
        writer.count(rows.size());
        for (const auto hash : rows)
            writer.scalar(hash);
    }
    for (const auto hash : value.blockHashes)
        writer.scalar(hash);
    writer.count(value.richTextSerials.size());
    for (const auto serial : value.richTextSerials)
        writer.scalar(serial);
}

void writeHistory(Writer& writer, const Sync::HistoryBaseline& value)
{
    writer.scalar(value.epoch);
    writer.scalar(value.head);
    writer.scalar(value.revision);
}

void writeProportionalFaces(Writer& writer, const OSC::TextSizing::ProportionalFaceFacts& faces)
{
    writer.scalar(faces.cellAdvance);
    for (const auto advance : faces.untabledAdvances)
        writer.scalar(advance);
    for (const auto& face : faces.advances)
        for (const auto advance : face)
            writer.scalar(advance);
    for (const auto& face : faces.weightedAdvances)
        for (const auto advance : face)
            writer.scalar(advance);
}

bool writeAttachments(Writer& writer, const AttachmentHandover& state)
{
    writer.scalar(attachmentVersion);
    writer.scalar(attachmentMagic);
    writer.identity(state.attachments.hostInstanceID);
    writer.scalar(state.attachments.revision);
    writer.scalar(state.attachments.notifyRevision);
    writer.count(state.attachments.inheritedInstances.size());
    for (const auto issuer : state.attachments.inheritedInstances)
        writer.identity(issuer);
    writer.count(state.hostIdentities.size());
    for (const auto& identity : state.hostIdentities) {
        writer.identity(identity.issuer);
        writer.scalar(identity.daemonInstanceID);
    }
    writer.count(state.attachments.records.size());
    for (const auto& record : state.attachments.records) {
        if (!record || record->state != Sync::AttachmentState::Lost || record->servingConnection
            || record->consumedSecret || record->handshakeConnection || record->proofConnection)
            return false;
        writer.identity(record->attachmentID);
        writer.identity(record->issuer);
        writer.identity(record->resumeSecret);
        writer.text(record->authority.principal);
        writer.flag(record->authority.mayDrive);
        writer.scalar(std::to_underlying(record->authority.verifiedKind));
        writer.scalar(std::to_underlying(record->kind));
        writer.identity(record->clientInstance);
        writer.scalar(record->lostSinceMicroseconds);
        writer.count(record->panes.size());
        for (const auto& pane : record->panes) {
            if (!pane)
                return false;
            writer.identity(pane->paneID);
            writer.scalar(pane->sentGeneration);
            writeBaseline(writer, pane->sentBaseline);
            writer.flag(pane->acknowledgedGeneration.has_value());
            if (pane->acknowledgedGeneration)
                writer.scalar(*pane->acknowledgedGeneration);
            writeBaseline(writer, pane->acknowledgedBaseline);
            writeHistory(writer, pane->acknowledgedHistory);
            writer.flag(pane->isHeld);
            writer.flag(pane->hasEffectReader);
            writer.scalar(pane->effectsSent);
            writer.scalar(pane->effectsAcknowledged);
            writer.count(pane->inFlight.size());
            for (const auto& sent : pane->inFlight) {
                writer.scalar(sent.generation);
                writeBaseline(writer, sent.baseline);
                writeHistory(writer, sent.history);
            }
        }
        if (record->inheritedProportionalFaces && !OSC::TextSizing::isAdoptable(*record->inheritedProportionalFaces))
            return false;
        writer.flag(static_cast<bool>(record->inheritedProportionalFaces));
        if (record->inheritedProportionalFaces)
            writeProportionalFaces(writer, *record->inheritedProportionalFaces);
    }
    writer.count(state.grants.size());
    for (const auto& grant : state.grants) {
        writer.identity(grant.attachmentID);
        writer.scalar(grant.scope.toRaw());
        writer.text(grant.tokenID);
        writer.scalar(grant.tokenExpiryUnix);
        writer.flag(grant.hasBootstrapToken);
        writer.flag(grant.allowsProof);
        writer.flag(grant.isInteractiveUser);
    }
    return true;
}

struct Budget {
    std::size_t panes;
    std::size_t inFlight;
    std::size_t rows;
    std::size_t serials;
    std::size_t text;
    bool didExceed = false;
};

std::size_t readCount(Sync::WireReader& reader, std::size_t& remaining, std::size_t minBytes, Budget& budget)
{
    const auto count = reader.u32();
    if (count > remaining) {
        budget.didExceed = true;
        reader.fail();
        return 0;
    }
    if (minBytes && count > reader.remaining() / minBytes) {
        reader.fail();
        return 0;
    }
    remaining -= count;
    return count;
}

Sync::ID128 readIdentity(Sync::WireReader& reader)
{
    const auto low = reader.u64();
    return { low, reader.u64() };
}

bool readFlag(Sync::WireReader& reader)
{
    const auto value = reader.u8();
    if (value > 1)
        reader.fail();
    return value != 0;
}

OSC::TextSizing::ProportionalFaceFacts readProportionalFaces(Sync::WireReader& reader)
{
    OSC::TextSizing::ProportionalFaceFacts faces;
    faces.cellAdvance = reader.u32();
    for (auto& advance : faces.untabledAdvances)
        advance = reader.u16();
    for (auto& face : faces.advances)
        for (auto& advance : face)
            advance = reader.u16();
    for (auto& face : faces.weightedAdvances)
        for (auto& advance : face)
            advance = reader.u8();
    if (!OSC::TextSizing::isAdoptable(faces))
        reader.fail();
    return faces;
}

std::string_view readText(Sync::WireReader& reader, Budget& budget)
{
    const auto count = readCount(reader, budget.text, 1, budget);
    const auto bytes = reader.take(count);
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

void readBaseline(Sync::WireReader& reader, Sync::AttachmentBaseline* target, Budget& budget)
{
    const auto epoch = reader.u32();
    const auto pens = reader.u64();
    const auto links = reader.u64();
    const auto clusters = reader.u64();
    if (pens > SIZE_MAX || links > SIZE_MAX || clusters > SIZE_MAX)
        reader.fail();
    const auto isAlternate = readFlag(reader);
    const auto columns = reader.u32();
    if (columns > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        reader.fail();
    if (target) {
        target->dictionaryEpoch = epoch;
        target->marks = { static_cast<std::size_t>(pens), static_cast<std::size_t>(links), static_cast<std::size_t>(clusters) };
        target->isAlternate = isAlternate;
        target->columns = static_cast<int>(columns);
    }
    for (unsigned screen = 0; screen < 2 && reader.ok(); ++screen) {
        const auto count = readCount(reader, budget.rows, sizeof(std::uint64_t), budget);
        if (target)
            target->rowHashes[screen].reserve(count);
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            const auto hash = reader.u64();
            if (target)
                target->rowHashes[screen].push_back(hash);
        }
    }
    for (unsigned index = 0; index < Sync::stateBlockCount; ++index) {
        const auto hash = reader.u64();
        if (target)
            target->blockHashes[index] = hash;
    }
    const auto count = readCount(reader, budget.serials, sizeof(std::uint32_t), budget);
    if (target)
        target->richTextSerials.reserve(count);
    std::uint32_t previous = 0;
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        const auto serial = reader.u32();
        if (!serial || serial <= previous)
            reader.fail();
        previous = serial;
        if (target)
            target->richTextSerials.push_back(serial);
    }
}

Sync::HistoryBaseline readHistory(Sync::WireReader& reader)
{
    const auto epoch = reader.u32();
    const auto head = reader.u64();
    return { epoch, head, reader.u64() };
}

void readPane(Sync::WireReader& reader, Sync::PaneBaselines* target, Budget& budget)
{
    const auto paneID = readIdentity(reader);
    const auto sentGeneration = reader.u64();
    if (paneID == Sync::ID128 {})
        reader.fail();
    readBaseline(reader, target ? &target->sentBaseline : nullptr, budget);
    const bool hasAcknowledged = readFlag(reader);
    const auto acknowledged = hasAcknowledged ? std::optional { reader.u64() } : std::nullopt;
    if (acknowledged && *acknowledged > sentGeneration)
        reader.fail();
    readBaseline(reader, target ? &target->acknowledgedBaseline : nullptr, budget);
    const auto history = readHistory(reader);
    const bool isHeld = readFlag(reader);
    const bool hasEffects = readFlag(reader);
    const auto effectsSent = reader.u64();
    const auto effectsAcknowledged = reader.u64();
    if (effectsAcknowledged > effectsSent)
        reader.fail();
    if (target) {
        target->paneID = paneID;
        target->sentGeneration = sentGeneration;
        target->acknowledgedGeneration = acknowledged;
        target->acknowledgedHistory = history;
        target->isHeld = isHeld;
        target->hasEffectReader = hasEffects;
        target->effectsSent = effectsSent;
        target->effectsAcknowledged = effectsAcknowledged;
    }
    const auto count = readCount(reader, budget.inFlight, sizeof(std::uint64_t), budget);
    auto previous = acknowledged.value_or(0);
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        const auto generation = reader.u64();
        if (generation <= previous || generation > sentGeneration)
            reader.fail();
        previous = generation;
        Sync::SentGeneration* sent = nullptr;
        if (target) {
            target->inFlight.emplace_back();
            sent = &target->inFlight.back();
            sent->generation = generation;
        }
        readBaseline(reader, sent ? &sent->baseline : nullptr, budget);
        const auto sentHistory = readHistory(reader);
        if (sent)
            sent->history = sentHistory;
    }
}

std::expected<void, Error> readAttachments(std::span<const std::uint8_t> bytes,
    const HandoverAttachmentLimits& limits, AttachmentHandover* target)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(Error::OverCapacity);
    Sync::WireReader reader(bytes);
    const auto version = reader.u16();
    if (version > attachmentVersion)
        return std::unexpected(Error::NewerVersion);
    if (version != attachmentVersion || reader.u32() != attachmentMagic)
        return std::unexpected(Error::Malformed);
    const auto host = readIdentity(reader);
    const auto revision = reader.u64();
    const auto notifyRevision = reader.u64();
    if (host == Sync::ID128 {} || revision == UINT64_MAX || notifyRevision == UINT64_MAX)
        reader.fail();
    Budget budget { limits.maxPaneRecords, limits.maxInFlightGenerations, limits.maxRowHashes, limits.maxRichTextSerials, limits.maxStringBytes };
    std::size_t issuerBudget = Sync::maxInheritedInstances;
    const auto inheritedCount = readCount(reader, issuerBudget, 16, budget);
    std::array<Sync::ID128, Sync::maxInheritedInstances + 1> issuers {};
    for (std::size_t index = 0; index < inheritedCount && reader.ok(); ++index) {
        const auto issuer = readIdentity(reader);
        if (issuer == Sync::ID128 {} || issuer == host
            || std::find(issuers.begin(), issuers.begin() + index, issuer) != issuers.begin() + index)
            reader.fail();
        issuers[index] = issuer;
        if (target)
            target->attachments.inheritedInstances.push_back(issuer);
    }
    issuers[inheritedCount] = host;
    std::size_t identityBudget = issuers.size();
    const auto identityCount = readCount(reader, identityBudget, 24, budget);
    if (identityCount != inheritedCount + 1)
        reader.fail();
    std::array<std::uint64_t, Sync::maxInheritedInstances + 1> outerIdentities {};
    for (std::size_t index = 0; index < identityCount && reader.ok(); ++index) {
        const auto issuer = readIdentity(reader);
        const auto outer = reader.u64();
        if (issuer != issuers[index] || !outer
            || std::find(outerIdentities.begin(), outerIdentities.begin() + index, outer) != outerIdentities.begin() + index)
            reader.fail();
        outerIdentities[index] = outer;
        if (target)
            target->hostIdentities.push_back({ issuer, outer });
    }
    if (target) {
        target->attachments.hostInstanceID = host;
        target->attachments.revision = revision;
        target->attachments.notifyRevision = notifyRevision;
    }
    std::size_t recordBudget = limits.maxAttachments;
    const auto recordCount = readCount(reader, recordBudget, 48, budget);
    for (std::size_t index = 0; index < recordCount && reader.ok(); ++index) {
        const auto id = readIdentity(reader);
        const auto issuer = readIdentity(reader);
        const auto secret = readIdentity(reader);
        const auto principal = readText(reader, budget);
        const auto mayDrive = readFlag(reader);
        const auto verifiedKind = reader.u8();
        const auto kind = reader.u8();
        const auto client = readIdentity(reader);
        const auto lostSince = reader.u64();
        if (id == Sync::ID128 {} || secret == Sync::ID128 {}
            || std::find(issuers.begin(), issuers.begin() + inheritedCount + 1, issuer) == issuers.begin() + inheritedCount + 1
            || verifiedKind > std::to_underlying(Sync::ClientKind::Viewer) || kind > std::to_underlying(Sync::ClientKind::Viewer))
            reader.fail();
        Sync::AttachmentRecord* record = nullptr;
        if (target && reader.ok()) {
            auto owner = std::unique_ptr<Sync::AttachmentRecord>(new (std::nothrow) Sync::AttachmentRecord);
            if (!owner)
                return std::unexpected(Error::AllocationRefused);
            record = owner.get();
            record->attachmentID = id;
            record->issuer = issuer;
            record->resumeSecret = secret;
            record->authority = { std::string(principal), mayDrive, static_cast<Sync::ClientKind>(verifiedKind) };
            record->kind = static_cast<Sync::ClientKind>(kind);
            record->clientInstance = client;
            record->state = Sync::AttachmentState::Lost;
            record->lostSinceMicroseconds = lostSince;
            target->attachments.records.push_back(std::move(owner));
        }
        const auto paneCount = readCount(reader, budget.panes, 16, budget);
        for (std::size_t paneIndex = 0; paneIndex < paneCount && reader.ok(); ++paneIndex) {
            Sync::PaneBaselines* pane = nullptr;
            if (record) {
                auto owner = std::unique_ptr<Sync::PaneBaselines>(new (std::nothrow) Sync::PaneBaselines);
                if (!owner)
                    return std::unexpected(Error::AllocationRefused);
                pane = owner.get();
                record->panes.push_back(std::move(owner));
            }
            readPane(reader, pane, budget);
        }
        if (reader.ok() && readFlag(reader)) {
            auto faces = readProportionalFaces(reader);
            if (record && reader.ok())
                record->inheritedProportionalFaces = std::make_shared<const OSC::TextSizing::ProportionalFaceFacts>(std::move(faces));
        }
    }
    std::size_t grantBudget = recordCount;
    const auto grantCount = readCount(reader, grantBudget, 16, budget);
    constexpr auto scopes = Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive,
        Protocol::Sync::Scope::Control, Protocol::Sync::Scope::RegionRead, Protocol::Sync::Scope::NewSession,
        Protocol::Sync::Scope::FileRead, Protocol::Sync::Scope::Manage };
    for (std::size_t index = 0; index < grantCount && reader.ok(); ++index) {
        const auto id = readIdentity(reader);
        const auto scope = reader.u8();
        const auto token = readText(reader, budget);
        const auto expiry = reader.u64();
        const auto bootstrap = readFlag(reader);
        const auto proof = readFlag(reader);
        const auto interactive = readFlag(reader);
        if (id == Sync::ID128 {} || token.empty() || (scope & ~scopes.toRaw()) || (interactive && !bootstrap))
            reader.fail();
        if (target)
            target->grants.push_back({ id, Protocol::Sync::ScopeSet::fromRaw(scope), std::string(token), expiry, bootstrap, proof, interactive });
    }
    const auto digestOffset = reader.offset();
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (budget.didExceed)
        return std::unexpected(Error::OverCapacity);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(Error::Malformed);
    if (Sync::digestOf(bytes.first(digestOffset)) != digest)
        return std::unexpected(Error::DigestMismatch);
    return {};
}

bool areReferencesValid(const AttachmentHandover& state)
{
    for (std::size_t index = 0; index < state.attachments.records.size(); ++index) {
        const auto& record = *state.attachments.records[index];
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.attachments.records[previous]->attachmentID == record.attachmentID)
                return false;
        }
        for (std::size_t paneIndex = 0; paneIndex < record.panes.size(); ++paneIndex) {
            for (std::size_t previous = 0; previous < paneIndex; ++previous) {
                if (record.panes[previous]->paneID == record.panes[paneIndex]->paneID)
                    return false;
            }
        }
        const auto grant = std::ranges::find(state.grants, record.attachmentID, &AttachmentGrant::attachmentID);
        if (record.authority.principal.starts_with("token:")) {
            if (grant == state.grants.end() || std::string_view(record.authority.principal).substr(6) != grant->tokenID
                || record.authority.mayDrive != grant->scope.contains(Protocol::Sync::Scope::Drive)
                || (record.authority.verifiedKind == Sync::ClientKind::Human && !grant->isInteractiveUser))
                return false;
        } else if (grant != state.grants.end())
            return false;
    }
    for (std::size_t index = 0; index < state.grants.size(); ++index) {
        const auto id = state.grants[index].attachmentID;
        if (std::ranges::none_of(state.attachments.records, [&](const auto& record) { return record->attachmentID == id; }))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.grants[previous].attachmentID == id)
                return false;
        }
    }
    return true;
}

} // namespace

std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> encodeHandoverAttachments(
    const AttachmentHandover& state, const HandoverAttachmentLimits& limits)
{
    Writer measure(limits.maxSectionBytes);
    if (!writeAttachments(measure, state))
        return std::unexpected(Error::Malformed);
    measure.identity({}); // Reserve the checksum before allocating.
    if (measure.didOverflow())
        return std::unexpected(Error::OverCapacity);
    if (!areReferencesValid(state))
        return std::unexpected(Error::Malformed);
    Sync::HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!result.data)
        return std::unexpected(Error::AllocationRefused);
    Writer writer(result.size, { result.data.get(), result.size });
    if (!writeAttachments(writer, state))
        return std::unexpected(Error::Malformed);
    const auto digest = Sync::digestOf(result.view().first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    if (const auto admitted = readAttachments(result.view(), limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    return result;
}

std::expected<AttachmentHandover, Protocol::Sync::HandoverBodyError> decodeHandoverAttachments(
    std::span<const std::uint8_t> bytes, const HandoverAttachmentLimits& limits)
{
    if (const auto admitted = readAttachments(bytes, limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    AttachmentHandover result;
    if (const auto restored = readAttachments(bytes, limits, &result); !restored)
        return std::unexpected(restored.error());
    if (!areReferencesValid(result))
        return std::unexpected(Error::Malformed);
    return result;
}

} // namespace Ubiquitty::Host
