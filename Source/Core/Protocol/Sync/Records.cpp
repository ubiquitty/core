// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sync/Records.cpp — see Records.h.

#include "Core/Protocol/Sync/Records.h"

#include "Core/Protocol/Sync/Messages.h"
#include "Core/TerminalOperations.h"
#include "Core/Title/Directory.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

// The record's flags. A bit no flag names is a malformed record.
constexpr std::uint8_t endDidEnd = 1 << 0;
constexpr std::uint8_t endHasStatus = 1 << 1;
constexpr std::uint8_t endWasSignalled = 1 << 2;
constexpr std::uint8_t endKnownBits = endDidEnd | endHasStatus | endWasSignalled;

constexpr std::uint8_t paneIsAtPrompt = 1 << 0;
constexpr std::uint8_t paneHasIndicator = 1 << 1;
constexpr std::uint8_t paneHasTabColor = 1 << 2;
constexpr std::uint8_t paneHasExitStatus = 1 << 3;
constexpr std::uint8_t paneIsAdopted = 1 << 4;
constexpr std::uint8_t paneHasMarkedPrompt = 1 << 5;
constexpr std::uint8_t paneKnownBits = paneIsAtPrompt | paneHasIndicator | paneHasTabColor | paneHasExitStatus | paneIsAdopted | paneHasMarkedPrompt;

constexpr std::uint8_t geometryHasOwner = 1 << 0;
constexpr std::uint8_t geometryIsPinned = 1 << 1;
constexpr std::uint8_t geometryIsOverridden = 1 << 2;
constexpr std::uint8_t geometryHasPinnedBy = 1 << 3;
constexpr std::uint8_t geometryKnownBits = geometryHasOwner | geometryIsPinned | geometryIsOverridden | geometryHasPinnedBy;

constexpr std::uint8_t attentionHasBell = 1 << 0;
constexpr std::uint8_t attentionHasNotification = 1 << 1;
constexpr std::uint8_t attentionKnownBits = attentionHasBell | attentionHasNotification;

// The most a length prefix takes for a text no longer than a frame.
constexpr std::size_t lengthPrefixBytes = 3;

// The largest `PaneFacts` body: every text at its cap, every optional
// present, every user variable slot full. It must fit one frame, since the
// record is never split.
constexpr std::size_t maxPaneFactsBytes = 4
    + (lengthPrefixBytes + Terminal::maxTitleLength)
    + (lengthPrefixBytes + Terminal::maxCurrentDirectoryLength)
    + (lengthPrefixBytes + maxHomeDirectoryBytes)
    + (lengthPrefixBytes + Terminal::maxCurrentFileURLLength)
    + (lengthPrefixBytes + maxProcessChainBytes)
    + (lengthPrefixBytes + maxProcessChainWithArgumentsBytes)
    + 3 * (lengthPrefixBytes + maxProcessNameBytes)
    + 2 + 2
    + 1 + 5
    + 4 + (lengthPrefixBytes + maxProcessNameBytes) + 4
    + 1
    + 2
    + 3 + (lengthPrefixBytes + OSC::tabStatusTextCapacity)
    + 3
    + (lengthPrefixBytes + OSC::badgeTextCapacity)
    + 1 + static_cast<std::size_t>(Terminal::userVariableCapacity) * (2 * lengthPrefixBytes + Terminal::maxUserVarNameLength + Terminal::maxUserVarValueLength)
    + 5
    + 8
    + 1 + 16 + 16
    + 8 + 8
    + 8
    + 1 + 2 * (lengthPrefixBytes + OSC::attentionTextCapacity);
static_assert(maxPaneFactsBytes <= maxFrameBodyBytes, "a pane's facts at every cap fit one frame");

// The fixed part of a `StatusRecords` frame: its revision, total, first and
// the count of the records it carries.
constexpr std::size_t maxStatusFragmentHeaderBytes = 4 + 5 + 5 + 5;
constexpr std::size_t maxStatusRecordBytes = 4 * lengthPrefixBytes + maxStatusIDBytes + maxStatusAppBytes + maxStatusTitleBytes + maxStatusMessageBytes + 1 + 1 + 2;
static_assert(maxStatusFragmentHeaderBytes + maxStatusRecordBytes <= maxFrameBodyBytes, "a frame always has room for one more status record");

void putText(WireWriter& writer, std::string_view text)
{
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

std::string takeText(WireReader& reader, std::size_t maximum)
{
    const std::span<const std::uint8_t> bytes = reader.bytes(maximum);
    return { bytes.begin(), bytes.end() };
}

void putID(WireWriter& writer, const ID128& value)
{
    writer.u64(value.low);
    writer.u64(value.high);
}

ID128 takeID(WireReader& reader)
{
    ID128 value;
    value.low = reader.u64();
    value.high = reader.u64();
    return value;
}

// Reads a flags byte, failing the reader on a bit no flag names.
std::uint8_t takeFlags(WireReader& reader, std::uint8_t known)
{
    const std::uint8_t flags = reader.u8();
    if ((flags & ~known) != 0)
        reader.fail();
    return flags;
}

// Reads a signed value that must fit thirty-two bits.
std::int32_t takeInt32(WireReader& reader)
{
    const std::int64_t value = reader.svarint();
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        reader.fail();
        return 0;
    }
    return static_cast<std::int32_t>(value);
}

void cut(std::string& text, std::size_t maximum)
{
    if (text.size() > maximum)
        text = boundedUTF8String(text, maximum);
}

void putAttention(WireWriter& writer, const AttentionFacts& attention)
{
    writer.u8(static_cast<std::uint8_t>((attention.hasUnseenBell ? attentionHasBell : 0) | (attention.hasUnseenNotification ? attentionHasNotification : 0)));
    putText(writer, attention.notificationTitle);
    putText(writer, attention.notificationBody);
}

AttentionFacts takeAttention(WireReader& reader)
{
    AttentionFacts attention;
    const std::uint8_t flags = takeFlags(reader, attentionKnownBits);
    attention.hasUnseenBell = (flags & attentionHasBell) != 0;
    attention.hasUnseenNotification = (flags & attentionHasNotification) != 0;
    attention.notificationTitle = takeText(reader, OSC::attentionTextCapacity);
    attention.notificationBody = takeText(reader, OSC::attentionTextCapacity);
    // Words with no notification to belong to are a state no host holds.
    if (!attention.hasUnseenNotification && (!attention.notificationTitle.empty() || !attention.notificationBody.empty()))
        reader.fail();
    return attention;
}

} // namespace

PaneFacts paneFactsOf(const Terminal& terminal, const PaneHostFacts& host)
{
    PaneFacts facts;
    facts.programTitle = terminal.titleUTF8;
    // The directory a title names (Title::titleDirectory): under a plain ssh
    // the pane's own is the client's folder on this host, which the session
    // has left, so only what the far side reported is sent.
    const std::string_view paneDirectory = terminal.currentDirectory.empty() ? std::string_view(host.foregroundDirectory) : terminal.currentDirectory.view();
    facts.currentDirectory = Title::titleDirectory({ .reportedDirectory = terminal.reportedDirectory.view(), .paneDirectory = paneDirectory, .foregroundLeadsTo = host.foregroundLeadsTo });
    facts.homeDirectory = host.homeDirectory;
    facts.currentFileURL = terminal.currentFileURL;
    facts.processChain = host.processChain;
    facts.processChainWithArguments = host.processChainWithArguments;
    facts.shellCommandName = host.shellCommandName;
    facts.terminalName = host.terminalName;
    facts.hostName = host.hostName;
    facts.rows = static_cast<std::uint16_t>(std::clamp(terminal.rows, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
    facts.columns = static_cast<std::uint16_t>(std::clamp(terminal.columns, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
    facts.end = host.end;
    facts.foreground = host.foreground;
    facts.isAtPrompt = host.isAtPrompt;
    facts.hasMarkedPrompt = terminal.hasMarkedPrompt;
    facts.progress = { .state = terminal.progressState, .value = terminal.progressValue };
    if (const std::optional<ColorRGBA> colour = tabStatusColor(&terminal))
        facts.indicator = IndicatorFacts { .red = colour->red, .green = colour->green, .blue = colour->blue, .label = std::string(tabStatusText(&terminal)) };
    if (const std::optional<ColorRGBA> colour = tabColor(&terminal))
        facts.tabColor = TabColorFacts { .red = colour->red, .green = colour->green, .blue = colour->blue };
    facts.badgeText = std::string(badgeText(&terminal));
    for (const Terminal::UserVariable& variable : terminal.userVariables) {
        if (variable.used)
            facts.userVariables.push_back({ .name = variable.name, .value = variable.value });
    }
    if (terminal.lastCommandExitCode >= 0)
        facts.exitStatus = terminal.lastCommandExitCode;
    facts.pixels = host.pixels;
    facts.activity = host.activity;
    facts.createdUnixSeconds = host.createdUnixSeconds;
    facts.isAdopted = host.isAdopted;
    const UnseenAttention attention = unseenAttention(&terminal);
    facts.attention.hasUnseenBell = attention.hasBell;
    facts.attention.hasUnseenNotification = attention.hasNotification;
    if (attention.hasNotification) {
        facts.attention.notificationTitle = std::string(attention.notificationTitle);
        facts.attention.notificationBody = std::string(attention.notificationBody);
    }
    return boundedForWire(std::move(facts));
}

StatusRecords statusRecordsOf(const Terminal& terminal)
{
    StatusRecords records;
    records.store = terminal.programStatus;
    return boundedForWire(std::move(records));
}

PaneFacts boundedForWire(PaneFacts facts)
{
    cut(facts.programTitle, Terminal::maxTitleLength);
    cut(facts.currentDirectory, Terminal::maxCurrentDirectoryLength);
    cut(facts.homeDirectory, maxHomeDirectoryBytes);
    cut(facts.currentFileURL, Terminal::maxCurrentFileURLLength);
    cut(facts.processChain, maxProcessChainBytes);
    cut(facts.processChainWithArguments, maxProcessChainWithArgumentsBytes);
    cut(facts.shellCommandName, maxProcessNameBytes);
    cut(facts.terminalName, maxProcessNameBytes);
    cut(facts.hostName, maxProcessNameBytes);
    cut(facts.foreground.name, maxProcessNameBytes);
    facts.progress.state = std::min<std::uint8_t>(facts.progress.state, OSC::ProgressPaused);
    facts.progress.value = std::min<std::uint8_t>(facts.progress.value, 100);
    if (facts.indicator)
        cut(facts.indicator->label, OSC::tabStatusTextCapacity);
    cut(facts.badgeText, OSC::badgeTextCapacity);
    // A variable with no name, or a second under one name, is one a
    // terminal never holds; past the terminal's table there is no slot.
    std::vector<UserVariableFact> kept;
    for (UserVariableFact& variable : facts.userVariables) {
        if (kept.size() == static_cast<std::size_t>(Terminal::userVariableCapacity))
            break;
        if (variable.name.empty() || variable.name.size() > Terminal::maxUserVarNameLength || std::ranges::find(kept, variable.name, &UserVariableFact::name) != kept.end())
            continue;
        cut(variable.value, Terminal::maxUserVarValueLength);
        kept.push_back(std::move(variable));
    }
    facts.userVariables = std::move(kept);
    // A pin is what names who pinned; an owner is absent while one holds.
    if (!facts.geometry.isPinned)
        facts.geometry.pinnedBy.reset();
    // Words with no notification to belong to are a state no host holds.
    if (!facts.attention.hasUnseenNotification) {
        facts.attention.notificationTitle.clear();
        facts.attention.notificationBody.clear();
    }
    cut(facts.attention.notificationTitle, OSC::attentionTextCapacity);
    cut(facts.attention.notificationBody, OSC::attentionTextCapacity);
    return facts;
}

StatusRecords boundedForWire(StatusRecords records)
{
    const auto firstRemoved = std::remove_if(records.store.records.begin(), records.store.records.end(), [](const OSC::ProgramStatus& record) {
        return !OSC::validProgramStatus(record) || record.id.size() > maxStatusIDBytes || record.app.size() > maxStatusAppBytes;
    });
    records.store.records.erase(firstRemoved, records.store.records.end());
    if (records.store.records.size() > OSC::programStatusLimit)
        records.store.records.erase(records.store.records.begin(), records.store.records.end() - static_cast<std::ptrdiff_t>(OSC::programStatusLimit));
    return records;
}

bool differsBeyondActivity(const PaneFacts& left, const PaneFacts& right)
{
    PaneFacts aligned = right;
    aligned.revision = left.revision;
    aligned.activity = left.activity;
    return !(aligned == left);
}

void encodePaneFacts(std::vector<std::uint8_t>& output, const PaneFacts& facts)
{
    WireWriter writer(output);
    writer.u32(facts.revision);
    for (const std::string* text : { &facts.programTitle, &facts.currentDirectory, &facts.homeDirectory, &facts.currentFileURL, &facts.processChain, &facts.processChainWithArguments, &facts.shellCommandName, &facts.terminalName, &facts.hostName })
        putText(writer, *text);
    writer.u16(facts.rows);
    writer.u16(facts.columns);
    writer.u8(static_cast<std::uint8_t>((facts.end.didEnd ? endDidEnd : 0) | (facts.end.hasStatus ? endHasStatus : 0) | (facts.end.wasSignalled ? endWasSignalled : 0)));
    writer.svarint(facts.end.code);
    writer.u32(facts.foreground.processID);
    putText(writer, facts.foreground.name);
    writer.u32(facts.foreground.generation);
    writer.u8(static_cast<std::uint8_t>((facts.isAtPrompt ? paneIsAtPrompt : 0) | (facts.indicator ? paneHasIndicator : 0) | (facts.tabColor ? paneHasTabColor : 0) | (facts.exitStatus ? paneHasExitStatus : 0) | (facts.isAdopted ? paneIsAdopted : 0) | (facts.hasMarkedPrompt ? paneHasMarkedPrompt : 0)));
    writer.u8(facts.progress.state);
    writer.u8(facts.progress.value);
    if (facts.indicator) {
        writer.u8(facts.indicator->red);
        writer.u8(facts.indicator->green);
        writer.u8(facts.indicator->blue);
        putText(writer, facts.indicator->label);
    }
    if (facts.tabColor) {
        writer.u8(facts.tabColor->red);
        writer.u8(facts.tabColor->green);
        writer.u8(facts.tabColor->blue);
    }
    putText(writer, facts.badgeText);
    writer.varint(facts.userVariables.size());
    for (const UserVariableFact& variable : facts.userVariables) {
        putText(writer, variable.name);
        putText(writer, variable.value);
    }
    if (facts.exitStatus)
        writer.svarint(*facts.exitStatus);
    writer.u16(facts.pixels.cellWidth);
    writer.u16(facts.pixels.cellHeight);
    writer.u16(facts.pixels.textAreaWidth);
    writer.u16(facts.pixels.textAreaHeight);
    writer.u8(static_cast<std::uint8_t>((facts.geometry.owner ? geometryHasOwner : 0) | (facts.geometry.isPinned ? geometryIsPinned : 0) | (facts.geometry.isOverridden ? geometryIsOverridden : 0) | (facts.geometry.pinnedBy ? geometryHasPinnedBy : 0)));
    if (facts.geometry.owner)
        putID(writer, *facts.geometry.owner);
    if (facts.geometry.pinnedBy)
        putID(writer, *facts.geometry.pinnedBy);
    writer.u64(facts.activity.lastOutputUnixMilliseconds);
    writer.u64(facts.activity.lastInputUnixMilliseconds);
    writer.u64(facts.createdUnixSeconds);
    putAttention(writer, facts.attention);
}

std::optional<PaneFacts> decodePaneFacts(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    PaneFacts facts;
    facts.revision = reader.u32();
    facts.programTitle = takeText(reader, Terminal::maxTitleLength);
    facts.currentDirectory = takeText(reader, Terminal::maxCurrentDirectoryLength);
    facts.homeDirectory = takeText(reader, maxHomeDirectoryBytes);
    facts.currentFileURL = takeText(reader, Terminal::maxCurrentFileURLLength);
    facts.processChain = takeText(reader, maxProcessChainBytes);
    facts.processChainWithArguments = takeText(reader, maxProcessChainWithArgumentsBytes);
    facts.shellCommandName = takeText(reader, maxProcessNameBytes);
    facts.terminalName = takeText(reader, maxProcessNameBytes);
    facts.hostName = takeText(reader, maxProcessNameBytes);
    facts.rows = reader.u16();
    facts.columns = reader.u16();
    const std::uint8_t end = takeFlags(reader, endKnownBits);
    facts.end.didEnd = (end & endDidEnd) != 0;
    facts.end.hasStatus = (end & endHasStatus) != 0;
    facts.end.wasSignalled = (end & endWasSignalled) != 0;
    facts.end.code = takeInt32(reader);
    facts.foreground.processID = reader.u32();
    facts.foreground.name = takeText(reader, maxProcessNameBytes);
    facts.foreground.generation = reader.u32();
    const std::uint8_t flags = takeFlags(reader, paneKnownBits);
    facts.isAtPrompt = (flags & paneIsAtPrompt) != 0;
    facts.hasMarkedPrompt = (flags & paneHasMarkedPrompt) != 0;
    facts.isAdopted = (flags & paneIsAdopted) != 0;
    facts.progress.state = reader.u8();
    facts.progress.value = reader.u8();
    if (facts.progress.state > OSC::ProgressPaused || facts.progress.value > 100)
        reader.fail();
    if ((flags & paneHasIndicator) != 0) {
        IndicatorFacts indicator;
        indicator.red = reader.u8();
        indicator.green = reader.u8();
        indicator.blue = reader.u8();
        indicator.label = takeText(reader, OSC::tabStatusTextCapacity);
        facts.indicator = std::move(indicator);
    }
    if ((flags & paneHasTabColor) != 0) {
        TabColorFacts colour;
        colour.red = reader.u8();
        colour.green = reader.u8();
        colour.blue = reader.u8();
        facts.tabColor = colour;
    }
    facts.badgeText = takeText(reader, OSC::badgeTextCapacity);
    // Every variable takes at least its two length prefixes and a name byte.
    const std::size_t variables = reader.count(static_cast<std::size_t>(Terminal::userVariableCapacity), 3);
    for (std::size_t index = 0; index < variables && reader.ok(); ++index) {
        UserVariableFact variable;
        variable.name = takeText(reader, Terminal::maxUserVarNameLength);
        variable.value = takeText(reader, Terminal::maxUserVarValueLength);
        if (variable.name.empty() || std::ranges::find(facts.userVariables, variable.name, &UserVariableFact::name) != facts.userVariables.end()) {
            reader.fail();
            break;
        }
        facts.userVariables.push_back(std::move(variable));
    }
    if ((flags & paneHasExitStatus) != 0)
        facts.exitStatus = takeInt32(reader);
    facts.pixels.cellWidth = reader.u16();
    facts.pixels.cellHeight = reader.u16();
    facts.pixels.textAreaWidth = reader.u16();
    facts.pixels.textAreaHeight = reader.u16();
    const std::uint8_t geometry = takeFlags(reader, geometryKnownBits);
    facts.geometry.isPinned = (geometry & geometryIsPinned) != 0;
    facts.geometry.isOverridden = (geometry & geometryIsOverridden) != 0;
    if ((geometry & geometryHasOwner) != 0)
        facts.geometry.owner = takeID(reader);
    if ((geometry & geometryHasPinnedBy) != 0)
        facts.geometry.pinnedBy = takeID(reader);
    // Only a pin names who pinned.
    if (facts.geometry.pinnedBy && !facts.geometry.isPinned)
        reader.fail();
    facts.activity.lastOutputUnixMilliseconds = reader.u64();
    facts.activity.lastInputUnixMilliseconds = reader.u64();
    facts.createdUnixSeconds = reader.u64();
    facts.attention = takeAttention(reader);
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return facts;
}

void writeStatusRecord(WireWriter& writer, const OSC::ProgramStatus& record)
{
    for (const auto* field : { &record.id, &record.app, &record.title, &record.message })
        putText(writer, *field);
    writer.u8(std::to_underlying(record.state));
    writer.u8(std::to_underlying(record.kind));
    writer.svarint(record.progress);
}

OSC::ProgramStatus readStatusRecord(WireReader& reader)
{
    const auto view = readStatusRecordView(reader);
    if (!reader.ok())
        return {};
    OSC::ProgramStatus record;
    if (!record.id.tryAssign(view.id) || !record.app.tryAssign(view.app)
        || !record.title.tryAssign(view.title) || !record.message.tryAssign(view.message)) {
        reader.fail();
        return {};
    }
    record.state = view.state;
    record.kind = view.kind;
    record.progress = view.progress;
    return record;
}

OSC::ProgramStatusView readStatusRecordView(WireReader& reader)
{
    const auto text = [&](std::size_t maximum) {
        const auto bytes = reader.bytes(maximum);
        return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    OSC::ProgramStatusView record;
    record.id = text(maxStatusIDBytes);
    record.app = text(maxStatusAppBytes);
    record.title = text(maxStatusTitleBytes);
    record.message = text(maxStatusMessageBytes);
    const std::uint8_t state = reader.u8();
    const std::uint8_t kind = reader.u8();
    const std::int32_t progress = takeInt32(reader);
    if (!reader.ok() || state > std::to_underlying(OSC::ProgramState::Error) || kind > std::to_underlying(OSC::ProgramBlockKind::Auth)) {
        reader.fail();
        return {};
    }
    record.state = static_cast<OSC::ProgramState>(state);
    record.kind = static_cast<OSC::ProgramBlockKind>(kind);
    record.progress = progress;
    if (!OSC::validProgramStatus(record))
        reader.fail();
    return record;
}

std::size_t encodeStatusFragment(std::vector<std::uint8_t>& output, const StatusRecords& records, std::size_t first)
{
    WireWriter writer(output);
    const std::size_t begin = writer.size();
    const std::size_t total = records.store.records.size();
    writer.u32(records.revision);
    writer.varint(total);
    writer.varint(first);
    // The records that fit are counted before the count is written.
    std::vector<std::uint8_t> scratch;
    WireWriter measure(scratch);
    std::size_t next = first;
    const std::size_t budget = maxFrameBodyBytes - (writer.size() - begin) - 5;
    while (next < total) {
        const std::size_t before = scratch.size();
        writeStatusRecord(measure, records.store.records[next]);
        if (scratch.size() > budget) {
            scratch.resize(before);
            break;
        }
        ++next;
    }
    writer.varint(next - first);
    writer.raw(scratch);
    return next;
}

std::optional<StatusFragment> decodeStatusFragment(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    StatusFragment fragment;
    fragment.revision = reader.u32();
    fragment.total = static_cast<std::uint32_t>(reader.varint(OSC::programStatusLimit));
    fragment.first = static_cast<std::uint32_t>(reader.varint(OSC::programStatusLimit));
    const std::size_t count = reader.count(OSC::programStatusLimit, minStatusRecordBytes);
    if (!reader.ok() || fragment.first > fragment.total || count > fragment.total - fragment.first)
        return std::nullopt;
    // A frame that carries nothing finishes nothing, unless the set is empty.
    if (count == 0 && fragment.total != 0)
        return std::nullopt;
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        OSC::ProgramStatus record = readStatusRecord(reader);
        if (!reader.ok() || std::ranges::find(fragment.records, record.id, &OSC::ProgramStatus::id) != fragment.records.end())
            return std::nullopt;
        fragment.records.push_back(std::move(record));
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return fragment;
}

StatusAssembly::Outcome StatusAssembly::accept(StatusFragment fragment)
{
    if (fragment.first == 0) {
        // A frame at the start of a set abandons whatever was in assembly:
        // the host began again under a newer revision.
        m_assembling = {};
        m_assembling.revision = fragment.revision;
        m_total = fragment.total;
        m_isOpen = true;
    } else if (!m_isOpen || fragment.revision != m_assembling.revision || fragment.total != m_total || fragment.first != m_assembling.store.records.size()) {
        m_isOpen = false;
        return Outcome::Refused;
    }
    for (OSC::ProgramStatus& record : fragment.records) {
        if (std::ranges::find(m_assembling.store.records, record.id, &OSC::ProgramStatus::id) != m_assembling.store.records.end()) {
            m_isOpen = false;
            return Outcome::Refused;
        }
        m_assembling.store.records.push_back(std::move(record));
    }
    if (m_assembling.store.records.size() < m_total)
        return Outcome::Partial;
    m_isOpen = false;
    return Outcome::Whole;
}

StatusRecords StatusAssembly::take()
{
    m_total = 0;
    return std::exchange(m_assembling, {});
}

void applyRecorded(Terminal& terminal, const PaneFacts& facts)
{
    if (terminal.progressState != facts.progress.state || terminal.progressValue != facts.progress.value) {
        terminal.progressState = facts.progress.state;
        terminal.progressValue = facts.progress.value;
        ++terminal.progressGeneration;
    }
    const std::string_view label = facts.indicator ? std::string_view(facts.indicator->label) : std::string_view();
    const bool isIndicatorSame = (terminal.tabStatusSet != 0) == facts.indicator.has_value()
        && (!facts.indicator || (terminal.tabStatusR == facts.indicator->red && terminal.tabStatusG == facts.indicator->green && terminal.tabStatusB == facts.indicator->blue && tabStatusText(&terminal) == label));
    const bool isTabColorSame = (terminal.tabColorSet != 0) == facts.tabColor.has_value()
        && (!facts.tabColor || (terminal.tabColorRed == facts.tabColor->red && terminal.tabColorGreen == facts.tabColor->green && terminal.tabColorBlue == facts.tabColor->blue));
    const bool isBadgeSame = terminal.badgeText == facts.badgeText;
    if (!isIndicatorSame) {
        terminal.tabStatusSet = facts.indicator ? 1 : 0;
        terminal.tabStatusR = facts.indicator ? facts.indicator->red : 0;
        terminal.tabStatusG = facts.indicator ? facts.indicator->green : 0;
        terminal.tabStatusB = facts.indicator ? facts.indicator->blue : 0;
        terminal.tabStatusText = {};
        const std::size_t length = std::min(label.size(), terminal.tabStatusText.size());
        std::copy_n(label.begin(), length, terminal.tabStatusText.begin());
        terminal.tabStatusTextLength = static_cast<std::uint8_t>(length);
    }
    if (!isTabColorSame) {
        terminal.tabColorSet = facts.tabColor ? 1 : 0;
        terminal.tabColorRed = facts.tabColor ? facts.tabColor->red : 0;
        terminal.tabColorGreen = facts.tabColor ? facts.tabColor->green : 0;
        terminal.tabColorBlue = facts.tabColor ? facts.tabColor->blue : 0;
    }
    if (!isBadgeSame)
        terminal.badgeText = facts.badgeText;
    // One generation covers the indicator, the tab colour and the badge.
    if (!isIndicatorSame || !isTabColorSame || !isBadgeSame)
        ++terminal.tabStatusGeneration;
    terminal.currentFileURL = facts.currentFileURL;
    // The table whole: a slot the record does not fill is a variable cleared.
    for (std::size_t slot = 0; slot < terminal.userVariables.size(); ++slot) {
        Terminal::UserVariable& variable = terminal.userVariables[slot];
        if (slot < facts.userVariables.size()) {
            variable.name = facts.userVariables[slot].name;
            variable.value = facts.userVariables[slot].value;
            variable.used = true;
        } else {
            variable = {};
        }
    }
    const AttentionFacts& attention = facts.attention;
    if (terminal.hasUnseenBell == attention.hasUnseenBell && terminal.hasUnseenNotification == attention.hasUnseenNotification
        && terminal.unseenNotificationTitle == attention.notificationTitle && terminal.unseenNotificationBody == attention.notificationBody)
        return;
    terminal.hasUnseenBell = attention.hasUnseenBell;
    terminal.hasUnseenNotification = attention.hasUnseenNotification;
    terminal.unseenNotificationTitle = attention.notificationTitle;
    terminal.unseenNotificationBody = attention.notificationBody;
    ++terminal.attentionGeneration;
}

} // namespace Ubiquitty::Protocol::Sync
