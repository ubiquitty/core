// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverGlobalTables.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"

#include <bit>
#include <cstring>
#include <new>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
using Error = Sync::HandoverBodyError;
using Reader = Sync::WireReader;
using EventLog = Ubiquitty::Session::EventLog;
using BackgroundEvents = Ubiquitty::Session::BackgroundEvents;
constexpr std::uint16_t version = 1;
constexpr std::uint32_t magic = 0x47544248;

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
    void count(std::size_t value)
    {
        if (value > UINT32_MAX)
            m_didOverflow = true;
        scalar(static_cast<std::uint32_t>(value));
    }
    void flag(bool isSet) { scalar(static_cast<std::uint8_t>(isSet)); } // bool-ok: Encodes a stored Boolean field.
    void text(std::string_view value)
    {
        count(value.size());
        raw({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
    }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    bool m_didOverflow = false;
};

void writeTables(Writer& writer, const HandoverGlobalTables& state, std::span<const std::uint8_t> configuration)
{
    writer.scalar(version);
    writer.scalar(magic);
    writer.count(configuration.size());
    writer.raw(configuration);
    writer.text(state.shellPath);
    writer.text(state.paneHostName);
    writer.text(state.paneHomeDirectory);
    writer.scalar(static_cast<std::uint32_t>(state.columns));
    writer.scalar(static_cast<std::uint32_t>(state.rows));
    writer.count(state.events.ring.size());
    writer.count(state.events.head);
    writer.count(state.events.count);
    writer.scalar(state.events.nextCursor);
    for (const auto& event : state.events.ring) {
        writer.scalar(event.cursor);
        writer.scalar(std::to_underlying(event.kind));
        writer.scalar(event.sessionID);
        writer.scalar(event.paneID);
        writer.scalar(std::bit_cast<std::uint32_t>(event.exitStatus));
        writer.scalar(std::to_underlying(event.state));
        writer.flag(event.isAtPrompt);
        writer.scalar(event.count);
        writer.text(event.text);
        writer.text(event.detail);
    }
    const auto& background = state.backgroundEvents;
    writer.scalar(background.epoch);
    writer.scalar(background.latest);
    writer.scalar(background.droppedThrough);
    writer.scalar(background.revision);
    writer.count(background.events.size());
    for (const auto& event : background.events) {
        writer.scalar(event.sequence);
        writer.scalar(event.createdUnixMilliseconds);
        writer.text(event.paneID);
        writer.scalar(event.sessionID);
        writer.text(event.title);
        writer.text(event.body);
        writer.text(event.statusID);
        writer.text(event.notificationID);
        writer.scalar(std::to_underlying(event.occasion));
        writer.flag(event.isCurrent);
    }
    writer.scalar(state.backgroundInventoryRevision);
    writer.text(state.backgroundEventPath);
    writer.scalar(state.backgroundEventWriteDue);
    writer.scalar(state.writtenBackgroundEventRevision);
    writer.flag(state.isBackgroundEventStorageAvailable);
    writer.scalar(state.eventDeliveries);
    writer.scalar(state.nextScrollbackFoldMilliseconds);
    writer.scalar(state.lastScrollbackFoldSessionID);
    writer.count(state.closuresOwed.size());
    for (const auto identity : state.closuresOwed)
        writer.scalar(identity);
    writer.count(state.terminationsOwed.size());
    for (const auto& [session, pane] : state.terminationsOwed) {
        writer.scalar(session);
        writer.scalar(pane.low);
        writer.scalar(pane.high);
    }
    writer.flag(state.hasHadSession);
}

struct Budget {
    std::size_t strings;
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

std::string_view text(Reader& reader, Budget& budget, std::size_t maximum = SIZE_MAX)
{
    const auto size = count(reader, budget, std::min(maximum, budget.strings), 1);
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

std::expected<std::span<const std::uint8_t>, Error> readTables(std::span<const std::uint8_t> bytes, const HandoverGlobalLimits& limits, HandoverGlobalTables* target)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(Error::OverCapacity);
    Reader reader(bytes);
    const auto receivedVersion = reader.u16();
    if (reader.ok() && receivedVersion > version)
        return std::unexpected(Error::NewerVersion);
    if (receivedVersion != version || reader.u32() != magic)
        return std::unexpected(Error::Malformed);
    Budget budget { limits.maxStringBytes };
    const auto configurationBytes = count(reader, budget, limits.configuration.maxSectionBytes, 1);
    const auto configuration = reader.take(configurationBytes);
    const auto shell = text(reader, budget);
    const auto host = text(reader, budget);
    const auto home = text(reader, budget);
    const auto columns = reader.u32();
    const auto rows = reader.u32();
    if (!columns || columns > maxColumns || !rows || rows > maxRows)
        reader.fail();
    if (target) {
        target->shellPath = shell;
        target->paneHostName = host;
        target->paneHomeDirectory = home;
        target->columns = static_cast<int>(columns);
        target->rows = static_cast<int>(rows);
    }
    const auto ringSize = count(reader, budget, limits.maxEvents, 39);
    const auto head = reader.u32();
    const auto held = reader.u32();
    const auto next = reader.u64();
    if (!ringSize || head >= ringSize || held > ringSize || !next || next == UINT64_MAX
        || held != std::min<std::uint64_t>(next - 1, ringSize) || (held < ringSize && head))
        reader.fail();
    if (target) {
        target->events.head = head;
        target->events.count = held;
        target->events.nextCursor = next;
    }
    for (std::size_t index = 0; index < ringSize && reader.ok(); ++index) {
        const auto cursor = reader.u64();
        const auto kind = reader.u8();
        const auto session = reader.u32();
        const auto pane = reader.u32();
        const auto exitStatus = std::bit_cast<std::int32_t>(reader.u32());
        const auto state = reader.u8();
        const bool isAtPrompt = flag(reader);
        const auto dropped = reader.u64();
        const auto eventText = text(reader, budget, Ubiquitty::Session::maxEventTextLength);
        const auto detail = text(reader, budget, Ubiquitty::Session::maxEventTextLength);
        if (kind > Ubiquitty::Session::lastEventKind || state > std::to_underlying(Ubiquitty::Session::EventProgramState::Error))
            reader.fail();
        const auto ordinal = index >= head ? index - head : ringSize - head + index;
        if (ordinal < held) {
            if (!kind || cursor != next - held + ordinal)
                reader.fail();
        } else if (cursor || kind || session || pane || exitStatus != Ubiquitty::Session::noExitStatus || state || isAtPrompt || dropped || !eventText.empty() || !detail.empty())
            reader.fail();
        if (target)
            target->events.ring.push_back({ cursor, static_cast<Ubiquitty::Session::EventKind>(kind), session, pane, exitStatus,
                static_cast<Ubiquitty::Session::EventProgramState>(state), isAtPrompt, dropped, std::string(eventText), std::string(detail) });
    }
    const auto epoch = reader.u64();
    const auto latest = reader.u64();
    const auto droppedThrough = reader.u64();
    const auto revision = reader.u64();
    const auto backgrounds = count(reader, budget, BackgroundEvents::capacity, 42);
    if (!epoch || droppedThrough > latest || revision == UINT64_MAX)
        reader.fail();
    if (target)
        target->backgroundEvents = { epoch, latest, droppedThrough, revision, {} };
    auto previous = droppedThrough;
    for (std::size_t index = 0; index < backgrounds && reader.ok(); ++index) {
        const auto sequence = reader.u64();
        const auto created = reader.u64();
        const auto pane = text(reader, budget, 32);
        const auto session = reader.u32();
        const auto title = text(reader, budget, BackgroundEvents::titleCapacity);
        const auto body = text(reader, budget, BackgroundEvents::bodyCapacity);
        const auto status = text(reader, budget, BackgroundEvents::statusCapacity);
        const auto notification = text(reader, budget, BackgroundEvents::notificationIDCapacity);
        const auto occasion = reader.u8();
        const bool isCurrent = flag(reader);
        if (sequence <= previous || sequence > latest || !session || !Ubiquitty::Session::isValidBackgroundPaneID(pane)
            || (!status.empty() && !notification.empty()) || occasion > std::to_underlying(NotificationOccasion::Invisible))
            reader.fail();
        previous = sequence;
        if (target)
            target->backgroundEvents.events.push_back({ sequence, created, std::string(pane), session, std::string(title), std::string(body),
                std::string(status), std::string(notification), static_cast<NotificationOccasion>(occasion), isCurrent });
    }
    if (previous != latest)
        reader.fail();
    const auto inventory = reader.u64();
    const auto path = text(reader, budget);
    const auto writeDue = reader.u64();
    const auto written = reader.u64();
    const bool isStorageAvailable = flag(reader);
    const auto deliveries = reader.u64();
    const auto foldDue = reader.u64();
    const auto foldSession = reader.u32();
    if (!inventory || inventory == UINT64_MAX)
        reader.fail();
    if (target) {
        target->backgroundInventoryRevision = inventory;
        target->backgroundEventPath = path;
        target->backgroundEventWriteDue = writeDue;
        target->writtenBackgroundEventRevision = written;
        target->isBackgroundEventStorageAvailable = isStorageAvailable;
        target->eventDeliveries = deliveries;
        target->nextScrollbackFoldMilliseconds = foldDue;
        target->lastScrollbackFoldSessionID = foldSession;
    }
    const auto closures = count(reader, budget, limits.maxClosures, 4);
    for (std::size_t index = 0; index < closures && reader.ok(); ++index) {
        const auto identity = reader.u32();
        if (!identity)
            reader.fail();
        if (target)
            target->closuresOwed.push_back(identity);
    }
    const auto terminations = count(reader, budget, limits.maxTerminations, 20);
    for (std::size_t index = 0; index < terminations && reader.ok(); ++index) {
        const auto session = reader.u32();
        const Sync::ID128 pane { reader.u64(), reader.u64() };
        if (!session || pane == Sync::ID128 {})
            reader.fail();
        if (target)
            target->terminationsOwed.emplace_back(session, pane);
    }
    const bool hasHadSession = flag(reader);
    if (target)
        target->hasHadSession = hasHadSession;
    const auto digestOffset = reader.offset();
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (budget.didExceed)
        return std::unexpected(Error::OverCapacity);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(Error::Malformed);
    if (Sync::digestOf(bytes.first(digestOffset)) != digest)
        return std::unexpected(Error::DigestMismatch);
    return configuration;
}

Error configurationError(Preferences::HandoverValuesError error)
{
    switch (error) {
    case Preferences::HandoverValuesError::NewerVersion:
        return Error::NewerVersion;
    case Preferences::HandoverValuesError::Malformed:
        return Error::Malformed;
    case Preferences::HandoverValuesError::OverCapacity:
        return Error::OverCapacity;
    case Preferences::HandoverValuesError::DigestMismatch:
        return Error::DigestMismatch;
    }
    return Error::Malformed;
}

} // namespace

std::expected<Sync::HandoverBytes, Error> encodeHandoverGlobals(const HandoverGlobalTables& state, const HandoverGlobalLimits& limits)
{
    if (!EventLog::isValidContinuation(state.events, state.events.ring.size()) || !BackgroundEvents::isValidContinuation(state.backgroundEvents))
        return std::unexpected(Error::Malformed);
    const auto configuration = Preferences::encodeHandoverValues(state.configuration, limits.configuration);
    if (!configuration)
        return std::unexpected(configurationError(configuration.error()));
    Writer measure(limits.maxSectionBytes);
    writeTables(measure, state, *configuration);
    measure.scalar<std::uint64_t>(0);
    measure.scalar<std::uint64_t>(0);
    if (!measure.ok())
        return std::unexpected(Error::OverCapacity);
    Sync::HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!result.data)
        return std::unexpected(Error::AllocationRefused);
    Writer writer(result.size, { result.data.get(), result.size });
    writeTables(writer, state, *configuration);
    const auto digest = Sync::digestOf(result.view().first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    if (const auto admitted = readTables(result.view(), limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    return result;
}

std::expected<HandoverGlobalTables, Error> decodeHandoverGlobals(std::span<const std::uint8_t> bytes, const HandoverGlobalLimits& limits)
{
    const auto preflight = readTables(bytes, limits, nullptr);
    if (!preflight)
        return std::unexpected(preflight.error());
    auto configuration = Preferences::decodeHandoverValues(*preflight, limits.configuration);
    if (!configuration)
        return std::unexpected(configurationError(configuration.error()));
    HandoverGlobalTables result;
    result.configuration = std::move(*configuration);
    if (const auto restored = readTables(bytes, limits, &result); !restored)
        return std::unexpected(restored.error());
    return result;
}

} // namespace Ubiquitty::Host
