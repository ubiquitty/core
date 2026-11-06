// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverAuthorityTables.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
using Error = Sync::HandoverBodyError;
using Reader = Sync::WireReader;
constexpr std::uint16_t version = 1;
constexpr std::uint32_t magic = 0x43544248;
constexpr auto originCount = std::to_underlying(CapabilityOrigin::Remote) + 1;
constexpr auto tierCount = std::to_underlying(CapabilityTier::Rendering) + 1;
constexpr auto allScopes = Protocol::Sync::Scope::Observe | Protocol::Sync::Scope::Drive | Protocol::Sync::Scope::Control
    | Protocol::Sync::Scope::RegionRead | Protocol::Sync::Scope::NewSession | Protocol::Sync::Scope::FileRead | Protocol::Sync::Scope::Manage;

class Writer {
public:
    explicit Writer(std::size_t limit, std::span<std::uint8_t> output = {})
        : m_limit(limit)
        , m_output(output)
    {
    }
    bool ok() const { return !m_didOverflow; }
    std::size_t size() const { return m_size; }
    void raw(std::span<const std::uint8_t> bytes)
    {
        if (!ok() || bytes.size() > m_limit - m_size) {
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
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        raw(bytes);
    }
    void flag(bool isSet) { scalar(static_cast<std::uint8_t>(isSet)); } // bool-ok: Encodes a stored Boolean field.
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

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    bool m_didOverflow = false;
};

void writeKey(Writer& writer, const CapabilityGrantKey& key)
{
    writer.scalar(std::to_underlying(key.capability));
    writer.scalar(std::to_underlying(key.origin));
    writer.text(key.identity);
}

void writeTables(Writer& writer, const HandoverAuthorityTables& state)
{
    writer.scalar(version);
    writer.scalar(magic);
    for (unsigned origin = 0; origin < originCount; ++origin) {
        for (unsigned tier = 0; tier < tierCount; ++tier) {
            const auto value = state.decisions.configuredDecision(static_cast<CapabilityOrigin>(origin), static_cast<CapabilityTier>(tier));
            writer.scalar<std::uint8_t>(value ? std::to_underlying(*value) : 255);
        }
    }
    writer.count(state.grants.size());
    for (const auto& [key, record] : state.grants) {
        writeKey(writer, key);
        writer.scalar(std::to_underlying(record.scope));
        writer.scalar(record.sessionID);
    }
    writer.count(state.pendingConsent.size());
    for (const auto& key : state.pendingConsent)
        writeKey(writer, key);
    writer.count(state.newlyPendingConsent.size());
    for (const auto& key : state.newlyPendingConsent)
        writeKey(writer, key);
    // The layout keeps the count of a table of pending questions, which no
    // record fills: a question never crosses a handover.
    writer.count(0);
    writer.count(state.quicTokens.size());
    for (const auto& token : state.quicTokens) {
        writer.text(token.token);
        writer.scalar(token.deadlineMilliseconds);
        writer.scalar(token.mintingClientID);
        writer.scalar(token.scope.toRaw());
        writer.text(token.tokenID);
        writer.flag(token.isInteractiveUser);
    }
    writer.count(state.alwaysAllowedAgents.size());
    for (const auto& identity : state.alwaysAllowedAgents)
        writer.text(identity);
    writer.scalar(state.nextConsentRequestID);
    writer.scalar(state.nextClipboardRequestID);
    writer.flag(state.isTokenRequired);
    writer.flag(state.didFailManagementHistory);
}

struct Budget {
    std::size_t strings;
    std::size_t consent;
    bool didExceed = false;
};

std::size_t count(Reader& reader, Budget& budget, std::size_t maximum, std::size_t minBytes)
{
    const auto value = reader.u32();
    if (value > maximum) {
        budget.didExceed = true;
        reader.fail();
        return 0;
    }
    if (minBytes && value > reader.remaining() / minBytes) {
        reader.fail();
        return 0;
    }
    return value;
}

std::string_view text(Reader& reader, Budget& budget)
{
    const auto size = count(reader, budget, budget.strings, 1);
    budget.strings -= size;
    const auto bytes = reader.take(size);
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

bool flag(Reader& reader)
{
    const auto value = reader.u8();
    if (value > 1)
        reader.fail();
    return value != 0;
}

void readKey(Reader& reader, Budget& budget, CapabilityGrantKey* target)
{
    const auto capability = reader.u16();
    const auto origin = reader.u8();
    const auto identity = text(reader, budget);
    if (capability > std::to_underlying(Capability::None) || origin > std::to_underlying(CapabilityOrigin::Remote) || identity.empty())
        reader.fail();
    if (target)
        *target = { static_cast<Capability>(capability), static_cast<CapabilityOrigin>(origin), std::string(identity) };
}

std::expected<void, Error> readTables(std::span<const std::uint8_t> bytes, const HandoverAuthorityLimits& limits, HandoverAuthorityTables* target)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(Error::OverCapacity);
    Reader reader(bytes);
    const auto receivedVersion = reader.u16();
    if (reader.ok() && receivedVersion > version)
        return std::unexpected(Error::NewerVersion);
    if (receivedVersion != version || reader.u32() != magic)
        return std::unexpected(Error::Malformed);
    Budget budget { limits.maxStringBytes, limits.maxConsentEntries };
    for (unsigned origin = 0; origin < originCount; ++origin) {
        for (unsigned tier = 0; tier < tierCount; ++tier) {
            const auto value = reader.u8();
            if (value == 255)
                continue;
            if (value > std::to_underlying(CapabilityDecision::Prompt) || !isConfigurableTier(static_cast<CapabilityTier>(tier)))
                reader.fail();
            if (target)
                target->decisions = target->decisions.replacing(static_cast<CapabilityOrigin>(origin), static_cast<CapabilityTier>(tier), static_cast<CapabilityDecision>(value));
        }
    }
    const auto grants = count(reader, budget, limits.maxGrants, 12);
    for (std::size_t index = 0; index < grants && reader.ok(); ++index) {
        if (target)
            target->grants.emplace_back();
        readKey(reader, budget, target ? &target->grants.back().first : nullptr);
        const auto scope = reader.u8();
        const auto session = reader.u32();
        if (scope > std::to_underlying(CapabilityGrantScope::Always) || (scope == std::to_underlying(CapabilityGrantScope::Session) && !session))
            reader.fail();
        if (target)
            target->grants.back().second = { static_cast<CapabilityGrantScope>(scope), session };
    }
    for (unsigned part = 0; part < 2; ++part) {
        const auto keys = count(reader, budget, budget.consent, 7);
        budget.consent -= keys;
        auto* list = target ? (part ? &target->newlyPendingConsent : &target->pendingConsent) : nullptr;
        for (std::size_t index = 0; index < keys && reader.ok(); ++index) {
            if (list)
                list->emplace_back();
            readKey(reader, budget, list ? &list->back() : nullptr);
        }
    }
    // A predecessor that captured a pending question was not at ground: the
    // question belongs to its recipients there, and none is taken over.
    const auto questions = reader.u32();
    if (reader.ok() && questions) {
        // The entries that follow are not read, so the digest closing the
        // section is checked over it whole first: a damaged count must not
        // read as a predecessor that is only not yet at ground.
        if (bytes.size() < 16)
            return std::unexpected(Error::Malformed);
        Reader tail(bytes.last(16));
        const Sync::Digest128 digest { tail.u64(), tail.u64() };
        if (Sync::digestOf(bytes.first(bytes.size() - 16)) != digest)
            return std::unexpected(Error::DigestMismatch);
        return std::unexpected(Error::NotAtGround);
    }
    const auto tokens = count(reader, budget, std::min<std::size_t>(limits.maxQUICTokens, 64), 22);
    for (std::size_t index = 0; index < tokens && reader.ok(); ++index) {
        const auto token = text(reader, budget);
        const auto deadline = reader.u64();
        const auto minter = reader.u32();
        const auto scope = reader.u8();
        const auto tokenID = text(reader, budget);
        const bool isInteractive = flag(reader);
        if (token.size() != 32 || !minter || !deadline || (scope & ~allScopes.toRaw()) || tokenID.size() != 64)
            reader.fail();
        if (target)
            target->quicTokens.push_back({ std::string(token), deadline, minter, Protocol::Sync::ScopeSet::fromRaw(scope), std::string(tokenID), isInteractive });
    }
    const auto agents = count(reader, budget, limits.maxStandingAgents, 4);
    for (std::size_t index = 0; index < agents && reader.ok(); ++index) {
        const auto identity = text(reader, budget);
        if (identity.empty())
            reader.fail();
        if (target)
            target->alwaysAllowedAgents.emplace_back(identity);
    }
    const auto nextConsent = reader.u32();
    const auto nextClipboard = reader.u32();
    const bool isTokenRequired = flag(reader);
    const bool didFailManagementHistory = flag(reader);
    if (target) {
        target->nextConsentRequestID = nextConsent;
        target->nextClipboardRequestID = nextClipboard;
        target->isTokenRequired = isTokenRequired;
        target->didFailManagementHistory = didFailManagementHistory;
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

bool referencesAreValid(const HandoverAuthorityTables& state)
{
    const auto validKey = [](const CapabilityGrantKey& key) {
        return key.capability <= Capability::None && key.origin <= CapabilityOrigin::Remote && !key.identity.empty();
    };
    for (std::size_t index = 0; index < state.grants.size(); ++index) {
        const auto& [key, record] = state.grants[index];
        if (!validKey(key) || record.scope > CapabilityGrantScope::Always
            || (record.scope == CapabilityGrantScope::Session && !record.sessionID))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.grants[previous].first == key)
                return false;
        }
    }
    for (std::size_t index = 0; index < state.pendingConsent.size(); ++index) {
        if (!validKey(state.pendingConsent[index]))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.pendingConsent[previous] == state.pendingConsent[index])
                return false;
        }
    }
    if (std::ranges::any_of(state.newlyPendingConsent, [&](const auto& key) { return !validKey(key); }))
        return false;
    for (std::size_t index = 0; index < state.quicTokens.size(); ++index) {
        const auto& token = state.quicTokens[index];
        if (token.token.size() != 32 || !digestsEqual(token.tokenID, Crypto::sha256Hex(token.token)) || !token.mintingClientID || !token.deadlineMilliseconds
            || (token.scope.toRaw() & ~allScopes.toRaw()))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (digestsEqual(state.quicTokens[previous].token, token.token))
                return false;
        }
    }
    for (std::size_t index = 0; index < state.alwaysAllowedAgents.size(); ++index) {
        if (state.alwaysAllowedAgents[index].empty())
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.alwaysAllowedAgents[previous] == state.alwaysAllowedAgents[index])
                return false;
        }
    }
    return true;
}

} // namespace

std::expected<Sync::HandoverBytes, Error> encodeHandoverAuthority(const HandoverAuthorityTables& state, const HandoverAuthorityLimits& limits)
{
    Writer measure(limits.maxSectionBytes);
    writeTables(measure, state);
    measure.scalar<std::uint64_t>(0);
    measure.scalar<std::uint64_t>(0);
    if (!measure.ok())
        return std::unexpected(Error::OverCapacity);
    if (!referencesAreValid(state))
        return std::unexpected(Error::Malformed);
    Sync::HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!result.data)
        return std::unexpected(Error::AllocationRefused);
    Writer writer(result.size, { result.data.get(), result.size });
    writeTables(writer, state);
    const auto digest = Sync::digestOf(result.view().first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    if (const auto admitted = readTables(result.view(), limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    return result;
}

std::expected<HandoverAuthorityTables, Error> decodeHandoverAuthority(std::span<const std::uint8_t> bytes, const HandoverAuthorityLimits& limits)
{
    if (const auto admitted = readTables(bytes, limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    HandoverAuthorityTables result;
    if (const auto restored = readTables(bytes, limits, &result); !restored)
        return std::unexpected(restored.error());
    if (!referencesAreValid(result))
        return std::unexpected(Error::Malformed);
    return result;
}

} // namespace Ubiquitty::Host
