// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_records.cpp — what a host says about a pane beside its picture
// (Docs/sync.md §6.11, §7.7): `PaneFacts` and `StatusRecords`.
//
// Each record is whole and under a revision. Its decoder checks every count
// and length against a cap before it reads an element, and a body it refuses
// changes nothing its reader holds. A bound pane is sent both in every view
// state, ahead of its picture and counted in the window; a frame the window
// refuses is owed, never queued, so a reader that stops reading holds the
// host to one window however many panes change. A status set too large for
// one frame travels as several and replaces the client's only once whole.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "SyncLink.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using Ubiquitty::Terminal;
using UbiquittyTest::SyncLink;
namespace OSC = Ubiquitty::OSC;

namespace {

/// Facts with every member away from its default, so a field a codec drops
/// reads back different.
PaneFacts sampleFacts()
{
    PaneFacts facts;
    facts.revision = 7;
    facts.programTitle = "vim — notes.md";
    facts.currentDirectory = "/home/user/src/notes";
    facts.homeDirectory = "/home/user";
    facts.currentFileURL = "file://host/home/user/src/notes/notes.md";
    facts.processChain = "vim ◂ -zsh";
    facts.processChainWithArguments = "vim notes.md ◂ -zsh";
    facts.shellCommandName = "zsh";
    facts.terminalName = "ttys003";
    facts.hostName = "build-host";
    facts.rows = 50;
    facts.columns = 132;
    facts.end = { .didEnd = true, .hasStatus = true, .wasSignalled = true, .code = 15 };
    facts.foreground = { .processID = 4242, .name = "vim", .generation = 9 };
    facts.isAtPrompt = true;
    facts.hasMarkedPrompt = true;
    facts.progress = { .state = OSC::ProgressPaused, .value = 40 };
    facts.indicator = IndicatorFacts { .red = 0x10, .green = 0x20, .blue = 0x30, .label = "deploying" };
    facts.tabColor = TabColorFacts { .red = 200, .green = 100, .blue = 50 };
    facts.badgeText = "prod";
    facts.userVariables = { { .name = "branch", .value = "main" }, { .name = "empty", .value = "" } };
    facts.exitStatus = -3;
    facts.pixels = { .cellWidth = 9, .cellHeight = 18, .textAreaWidth = 1188, .textAreaHeight = 900 };
    facts.geometry = { .owner = std::nullopt, .isPinned = true, .isOverridden = false, .pinnedBy = ID128 { .low = 11, .high = 12 } };
    facts.activity = { .lastOutputUnixMilliseconds = 1'700'000'000'123, .lastInputUnixMilliseconds = 1'700'000'000'456 };
    facts.createdUnixSeconds = 1'699'999'999;
    facts.isAdopted = true;
    facts.attention = { .hasUnseenBell = true, .hasUnseenNotification = true, .notificationTitle = "Deploy", .notificationBody = "went out" };
    return facts;
}

std::vector<std::uint8_t> encoded(const PaneFacts& facts)
{
    std::vector<std::uint8_t> body;
    encodePaneFacts(body, facts);
    return body;
}

OSC::ProgramStatus statusRecord(std::string id, OSC::ProgramState state, std::string message = "")
{
    OSC::ProgramStatus record;
    record.id = std::move(id);
    record.app = "agent";
    record.title = "Build";
    record.message = std::move(message);
    record.state = state;
    return record;
}

/// A status set of `count` records, each with a message of `messageBytes`.
StatusRecords statusSet(std::size_t count, std::size_t messageBytes, std::uint32_t revision = 1)
{
    StatusRecords records;
    records.revision = revision;
    for (std::size_t index = 0; index < count; ++index)
        records.store.records.push_back(statusRecord("job/" + std::to_string(index), OSC::ProgramState::Working, std::string(messageBytes, static_cast<char>('a' + index % 26))));
    return records;
}

/// Every frame a set takes, in order.
std::vector<std::vector<std::uint8_t>> fragmentsOf(const StatusRecords& records)
{
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t next = 0;
    do {
        std::vector<std::uint8_t> body;
        next = encodeStatusFragment(body, records, next);
        frames.push_back(std::move(body));
    } while (next < records.store.records.size());
    return frames;
}

/// Where a `PaneFacts` body's nine texts end and its grid begins, for a body
/// whose texts are all empty.
constexpr std::size_t emptyTextsEnd = 4 + 9;

/// The offset of the state flags in a body whose texts are all empty: past
/// the grid, the end and the foreground.
constexpr std::size_t emptyFlagsOffset = emptyTextsEnd + 2 + 2 + 1 + 1 + 4 + 1 + 4;

/// Counts the frames of each kind the host sends on one channel, in order.
class FrameLog {
public:
    FrameLog(SyncLink& link, std::uint32_t channel)
    {
        link.onHostFrame([this, channel](FrameKind kind, std::uint32_t on, Carrier, std::size_t) {
            if (on != channel)
                return;
            ++m_counts[kind];
            m_order.push_back(kind);
        });
    }

    [[nodiscard]] std::size_t operator[](FrameKind kind) const
    {
        const auto found = m_counts.find(kind);
        return found == m_counts.end() ? 0 : found->second;
    }

    [[nodiscard]] const std::vector<FrameKind>& order() const { return m_order; }

    void clear()
    {
        m_counts.clear();
        m_order.clear();
    }

private:
    std::map<FrameKind, std::size_t> m_counts;
    std::vector<FrameKind> m_order;
};

/// Sets a pane's two records from its terminal, as a host does at a publish.
void setRecords(SyncLink& link, std::uint32_t handle, const PaneHostFacts& host = {})
{
    link.source(handle).setFacts(paneFactsOf(link.hostTerminal(handle), host));
    link.source(handle).setStatus(statusRecordsOf(link.hostTerminal(handle)));
}

} // namespace

TEST(statusRecordsRefuseAnUnknownState)
{
    StatusRecords records = statusSet(1, 8);
    std::vector<std::uint8_t> body;
    CHECK_EQ(encodeStatusFragment(body, records, 0), std::size_t { 1 });
    REQUIRE(decodeStatusFragment(body).has_value());
    // The state is the third byte from the end: state, kind, progress.
    const std::size_t stateOffset = body.size() - 3;
    CHECK_EQ(static_cast<int>(body[stateOffset]), static_cast<int>(OSC::ProgramState::Working));
    for (const std::uint8_t state : { std::uint8_t { 0 }, std::uint8_t { 6 }, std::uint8_t { 255 } }) {
        std::vector<std::uint8_t> bent = body;
        bent[stateOffset] = state;
        CHECK_MSG(!decodeStatusFragment(bent).has_value(), ("a state byte of " + std::to_string(state) + " was read as a status").c_str());
    }
    // A block kind past the last, and one on a record that is not blocked.
    std::vector<std::uint8_t> bent = body;
    bent[stateOffset + 1] = 4;
    CHECK(!decodeStatusFragment(bent).has_value());
    bent[stateOffset + 1] = 1;
    CHECK(!decodeStatusFragment(bent).has_value());
}

TEST(aPaneFactsRoundTripsEveryField)
{
    const PaneFacts facts = sampleFacts();
    const std::optional<PaneFacts> read = decodePaneFacts(encoded(facts));
    REQUIRE(read.has_value());
    CHECK(*read == facts);
    // Every member is away from its default, so the comparison covers each.
    CHECK(!(facts == PaneFacts {}));

    // The optionals absent, and an owner in place of a pin.
    PaneFacts plain = sampleFacts();
    plain.indicator.reset();
    plain.tabColor.reset();
    plain.exitStatus.reset();
    plain.userVariables.clear();
    plain.isAtPrompt = false;
    plain.hasMarkedPrompt = false;
    plain.isAdopted = false;
    plain.end = {};
    plain.geometry = { .owner = ID128 { .low = 1, .high = 2 }, .isPinned = false, .isOverridden = true, .pinnedBy = std::nullopt };
    const std::optional<PaneFacts> readPlain = decodePaneFacts(encoded(plain));
    REQUIRE(readPlain.has_value());
    CHECK(*readPlain == plain);

    const std::optional<PaneFacts> readEmpty = decodePaneFacts(encoded(PaneFacts {}));
    REQUIRE(readEmpty.has_value());
    CHECK(*readEmpty == PaneFacts {});
}

TEST(aPaneFactsAtEveryLimitFitsOneFrame)
{
    PaneFacts facts = sampleFacts();
    facts.programTitle.assign(Terminal::maxTitleLength, 't');
    facts.currentDirectory.assign(Terminal::maxCurrentDirectoryLength, 'd');
    facts.homeDirectory.assign(maxHomeDirectoryBytes, 'h');
    facts.currentFileURL.assign(Terminal::maxCurrentFileURLLength, 'u');
    facts.processChain.assign(maxProcessChainBytes, 'c');
    facts.processChainWithArguments.assign(maxProcessChainWithArgumentsBytes, 'a');
    facts.shellCommandName.assign(maxProcessNameBytes, 's');
    facts.terminalName.assign(maxProcessNameBytes, 'y');
    facts.hostName.assign(maxProcessNameBytes, 'o');
    facts.foreground.name.assign(maxProcessNameBytes, 'f');
    facts.indicator->label.assign(OSC::tabStatusTextCapacity, 'l');
    facts.badgeText.assign(OSC::badgeTextCapacity, 'b');
    facts.userVariables.clear();
    for (int slot = 0; slot < Terminal::userVariableCapacity; ++slot)
        facts.userVariables.push_back({ .name = std::string(Terminal::maxUserVarNameLength - 2, 'n') + std::to_string(10 + slot), .value = std::string(Terminal::maxUserVarValueLength, 'v') });
    facts.exitStatus = std::numeric_limits<std::int32_t>::min();
    facts.end.code = std::numeric_limits<std::int32_t>::min();
    facts.geometry = { .owner = std::nullopt, .isPinned = true, .isOverridden = false, .pinnedBy = ID128 { .low = ~std::uint64_t { 0 }, .high = ~std::uint64_t { 0 } } };
    // At the caps nothing is cut.
    CHECK(boundedForWire(facts) == facts);
    const std::vector<std::uint8_t> body = encoded(facts);
    CHECK_MSG(body.size() <= maxFrameBodyBytes, ("facts at every cap take " + std::to_string(body.size()) + " bytes").c_str());
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::PaneFacts, 1, body));
    const std::optional<PaneFacts> read = decodePaneFacts(body);
    REQUIRE(read.has_value());
    CHECK(*read == facts);
}

TEST(factsPastALimitAreCutToItNotRefusedByTheirOwnReader)
{
    PaneFacts facts = sampleFacts();
    facts.programTitle.assign(Terminal::maxTitleLength + 100, 't');
    facts.badgeText.assign(OSC::badgeTextCapacity + 1, 'b');
    facts.indicator->label.assign(OSC::tabStatusTextCapacity + 9, 'l');
    facts.progress = { .state = 9, .value = 250 };
    facts.userVariables.clear();
    for (int slot = 0; slot < Terminal::userVariableCapacity + 5; ++slot)
        facts.userVariables.push_back({ .name = "v" + std::to_string(slot), .value = std::string(Terminal::maxUserVarValueLength + 3, 'x') });
    // A second variable under one name, one with no name, and one whose name
    // no terminal holds.
    facts.userVariables.insert(facts.userVariables.begin() + 1, { .name = "v0", .value = "twice" });
    facts.userVariables.insert(facts.userVariables.begin() + 2, { .name = "", .value = "nameless" });
    facts.userVariables.insert(facts.userVariables.begin() + 3, { .name = std::string(Terminal::maxUserVarNameLength + 1, 'n'), .value = "long" });
    facts.geometry = { .owner = ID128 { .low = 1, .high = 2 }, .isPinned = false, .isOverridden = false, .pinnedBy = ID128 { .low = 3, .high = 4 } };
    // Unbounded, its own reader refuses it; bounded, it reads back.
    CHECK(!decodePaneFacts(encoded(facts)).has_value());
    const PaneFacts bounded = boundedForWire(facts);
    CHECK_EQ(bounded.programTitle.size(), Terminal::maxTitleLength);
    CHECK_EQ(bounded.badgeText.size(), OSC::badgeTextCapacity);
    CHECK_EQ(bounded.indicator->label.size(), OSC::tabStatusTextCapacity);
    CHECK_EQ(static_cast<int>(bounded.progress.state), static_cast<int>(OSC::ProgressPaused));
    CHECK_EQ(static_cast<int>(bounded.progress.value), 100);
    CHECK_EQ(bounded.userVariables.size(), static_cast<std::size_t>(Terminal::userVariableCapacity));
    CHECK(bounded.userVariables[0].name == "v0" && bounded.userVariables[0].value.size() == Terminal::maxUserVarValueLength);
    CHECK(bounded.userVariables[1].name == "v1");
    CHECK(!bounded.geometry.pinnedBy.has_value());
    const std::optional<PaneFacts> read = decodePaneFacts(encoded(bounded));
    REQUIRE(read.has_value());
    CHECK(*read == bounded);
}

TEST(aPaneFactsBodyNoHostSendsIsRefused)
{
    const std::vector<std::uint8_t> body = encoded(sampleFacts());
    // Cut anywhere, or carrying a trailing byte.
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK_MSG(!decodePaneFacts(std::span { body }.first(length)).has_value(), ("a body cut to " + std::to_string(length) + " bytes was read").c_str());
    std::vector<std::uint8_t> trailing = body;
    trailing.push_back(0);
    CHECK(!decodePaneFacts(trailing).has_value());

    // In a body with no text the fixed fields sit at known offsets.
    const std::vector<std::uint8_t> empty = encoded(PaneFacts {});
    REQUIRE(decodePaneFacts(empty).has_value());
    const auto bent = [&](std::size_t offset, std::uint8_t value) {
        std::vector<std::uint8_t> changed = empty;
        changed[offset] = value;
        return decodePaneFacts(changed).has_value();
    };
    // A text past its cap: the title's length prefix says one byte over.
    {
        std::vector<std::uint8_t> long_ = empty;
        long_.erase(long_.begin() + 4);
        const std::size_t over = Terminal::maxTitleLength + 1;
        const std::vector<std::uint8_t> prefix { static_cast<std::uint8_t>(0x80 | (over & 0x7F)), static_cast<std::uint8_t>(over >> 7) };
        long_.insert(long_.begin() + 4, prefix.begin(), prefix.end());
        long_.insert(long_.begin() + 6, over, 't');
        CHECK(!decodePaneFacts(long_).has_value());
    }
    // An end flag, a state flag and a geometry flag no flag names.
    CHECK(!bent(emptyTextsEnd + 4, 0x08));
    CHECK(bent(emptyTextsEnd + 4, 0x07));
    CHECK(!bent(emptyFlagsOffset, 0x40));
    CHECK(!bent(emptyFlagsOffset, 0x80));
    // A progress state past the last, and a percentage past a hundred.
    CHECK(!bent(emptyFlagsOffset + 1, OSC::ProgressPaused + 1));
    CHECK(bent(emptyFlagsOffset + 1, OSC::ProgressPaused));
    CHECK(!bent(emptyFlagsOffset + 2, 101));
    CHECK(bent(emptyFlagsOffset + 2, 100));
    // Past the badge and the empty variable list, the pixels, then the
    // geometry flags.
    const std::size_t geometryOffset = emptyFlagsOffset + 3 + 1 + 1 + 8;
    CHECK(!bent(geometryOffset, 0x10));
    CHECK(bent(geometryOffset, 0x06));
    // Past the two stamps and the creation time, the attention: a bit no
    // flag names, and words with no notification to belong to.
    const std::size_t attentionOffset = geometryOffset + 1 + 8 + 8 + 8;
    CHECK(!bent(attentionOffset, 0x04));
    CHECK(bent(attentionOffset, 0x03));
    {
        PaneFacts worded;
        worded.attention = { .hasUnseenBell = false, .hasUnseenNotification = true, .notificationTitle = "words", .notificationBody = "" };
        std::vector<std::uint8_t> body = encoded(worded);
        REQUIRE(decodePaneFacts(body).has_value());
        const std::size_t flags = body.size() - (1 + 1 + 5 + 1);
        REQUIRE(body[flags] == 0x02);
        body[flags] = 0x01;
        CHECK(!decodePaneFacts(body).has_value());
        // Each word at its cap crosses; a byte past it does not.
        worded.attention.notificationTitle.assign(OSC::attentionTextCapacity, 'T');
        worded.attention.notificationBody.assign(OSC::attentionTextCapacity, 'B');
        CHECK(decodePaneFacts(encoded(worded)) == std::optional { worded });
        worded.attention.notificationBody.push_back('B');
        CHECK(!decodePaneFacts(encoded(worded)).has_value());
    }

    // A variable twice, one with no name, and more than a terminal holds.
    PaneFacts twice;
    twice.userVariables = { { .name = "k", .value = "1" }, { .name = "k", .value = "2" } };
    CHECK(!decodePaneFacts(encoded(twice)).has_value());
    PaneFacts nameless;
    nameless.userVariables = { { .name = "", .value = "1" } };
    CHECK(!decodePaneFacts(encoded(nameless)).has_value());
    PaneFacts many;
    for (int slot = 0; slot <= Terminal::userVariableCapacity; ++slot)
        many.userVariables.push_back({ .name = "v" + std::to_string(slot), .value = "x" });
    CHECK(!decodePaneFacts(encoded(many)).has_value());
    many.userVariables.pop_back();
    CHECK(decodePaneFacts(encoded(many)).has_value());
    // Who pinned, with no pin.
    PaneFacts unpinned;
    unpinned.geometry.pinnedBy = ID128 { .low = 1, .high = 1 };
    CHECK(!decodePaneFacts(encoded(unpinned)).has_value());
    // An exit code that does not fit thirty-two bits.
    {
        PaneFacts exited;
        exited.exitStatus = 1;
        std::vector<std::uint8_t> wide = encoded(exited);
        // The one-byte status sits ahead of the pixels, the geometry flags,
        // the three stamps and the attention; ten bytes of 0xFF... make it a
        // 64-bit value.
        const std::size_t statusOffset = wide.size() - (8 + 1 + 8 + 8 + 8 + 3) - 1;
        CHECK_EQ(static_cast<int>(wide[statusOffset]), 2);
        const std::vector<std::uint8_t> huge { 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        wide.erase(wide.begin() + static_cast<std::ptrdiff_t>(statusOffset));
        wide.insert(wide.begin() + static_cast<std::ptrdiff_t>(statusOffset), huge.begin(), huge.end());
        CHECK(!decodePaneFacts(wide).has_value());
    }
}

TEST(aStatusRecordAtTheCoreLimitRoundTrips)
{
    StatusRecords records;
    records.revision = 3;
    OSC::ProgramStatus record = statusRecord("a/b/c", OSC::ProgramState::Blocked, std::string(maxStatusMessageBytes, 'm'));
    record.kind = OSC::ProgramBlockKind::Auth;
    record.title.resize(maxStatusTitleBytes, 't');
    record.progress = 100;
    REQUIRE(OSC::validProgramStatus(record));
    records.store.records.push_back(record);
    std::vector<std::uint8_t> body;
    CHECK_EQ(encodeStatusFragment(body, records, 0), std::size_t { 1 });
    const std::optional<StatusFragment> fragment = decodeStatusFragment(body);
    REQUIRE(fragment.has_value());
    CHECK_EQ(fragment->revision, 3u);
    CHECK_EQ(fragment->total, 1u);
    CHECK_EQ(fragment->first, 0u);
    REQUIRE(fragment->records.size() == 1);
    CHECK(fragment->records[0] == record);
    // One byte past the message's cap is refused, as the OSC parser refuses it.
    records.store.records[0].message.push_back('m');
    body.clear();
    (void)encodeStatusFragment(body, records, 0);
    CHECK(!decodeStatusFragment(body).has_value());
}

TEST(aStatusSetTooLargeForOneFrameTravelsAsSeveralAndArrivesWhole)
{
    const StatusRecords records = statusSet(OSC::programStatusLimit, maxStatusMessageBytes, 9);
    const std::vector<std::vector<std::uint8_t>> frames = fragmentsOf(records);
    CHECK(frames.size() > 1);
    StatusAssembly assembly;
    std::size_t taken = 0;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        CHECK_MSG(frames[index].size() <= maxFrameBodyBytes, "a status frame is larger than a frame");
        std::optional<StatusFragment> fragment = decodeStatusFragment(frames[index]);
        REQUIRE(fragment.has_value());
        CHECK_EQ(fragment->first, static_cast<std::uint32_t>(taken));
        CHECK_EQ(fragment->total, static_cast<std::uint32_t>(OSC::programStatusLimit));
        taken += fragment->records.size();
        const StatusAssembly::Outcome outcome = assembly.accept(std::move(*fragment));
        CHECK(outcome == (index + 1 == frames.size() ? StatusAssembly::Outcome::Whole : StatusAssembly::Outcome::Partial));
    }
    CHECK(assembly.take() == records);

    // An empty set is one frame, and whole.
    StatusRecords none;
    none.revision = 2;
    const std::vector<std::vector<std::uint8_t>> noneFrames = fragmentsOf(none);
    REQUIRE(noneFrames.size() == 1);
    std::optional<StatusFragment> fragment = decodeStatusFragment(noneFrames[0]);
    REQUIRE(fragment.has_value());
    CHECK(assembly.accept(std::move(*fragment)) == StatusAssembly::Outcome::Whole);
    CHECK(assembly.take() == none);
}

TEST(aStatusFrameThatContinuesNoSetIsRefused)
{
    const StatusRecords records = statusSet(64, maxStatusMessageBytes, 4);
    const std::vector<std::vector<std::uint8_t>> frames = fragmentsOf(records);
    REQUIRE(frames.size() >= 3);
    const auto fragment = [&](std::size_t index) { return *decodeStatusFragment(frames[index]); };

    // A frame from the middle with no set begun.
    StatusAssembly cold;
    CHECK(cold.accept(fragment(1)) == StatusAssembly::Outcome::Refused);

    // A frame skipped.
    StatusAssembly skipping;
    CHECK(skipping.accept(fragment(0)) == StatusAssembly::Outcome::Partial);
    CHECK(skipping.accept(fragment(2)) == StatusAssembly::Outcome::Refused);
    // Refused, the assembly holds no set to continue.
    CHECK(skipping.accept(fragment(1)) == StatusAssembly::Outcome::Refused);

    // A frame of another revision, or of a set of another size.
    StatusAssembly mixing;
    CHECK(mixing.accept(fragment(0)) == StatusAssembly::Outcome::Partial);
    StatusFragment other = fragment(1);
    other.revision = 5;
    CHECK(mixing.accept(std::move(other)) == StatusAssembly::Outcome::Refused);
    StatusAssembly resizing;
    CHECK(resizing.accept(fragment(0)) == StatusAssembly::Outcome::Partial);
    StatusFragment larger = fragment(1);
    larger.total += 1;
    CHECK(resizing.accept(std::move(larger)) == StatusAssembly::Outcome::Refused);

    // A set begun again abandons the one in assembly: the newer arrives whole.
    StatusAssembly restarting;
    CHECK(restarting.accept(fragment(0)) == StatusAssembly::Outcome::Partial);
    const StatusRecords newer = statusSet(2, 16, 5);
    const std::vector<std::vector<std::uint8_t>> newerFrames = fragmentsOf(newer);
    REQUIRE(newerFrames.size() == 1);
    CHECK(restarting.accept(*decodeStatusFragment(newerFrames[0])) == StatusAssembly::Outcome::Whole);
    CHECK(restarting.take() == newer);

    // The same id in two frames of one set.
    StatusAssembly repeating;
    CHECK(repeating.accept(fragment(0)) == StatusAssembly::Outcome::Partial);
    StatusFragment repeated = fragment(1);
    repeated.records[0].id = "job/0";
    CHECK(repeating.accept(std::move(repeated)) == StatusAssembly::Outcome::Refused);
}

TEST(aStatusFrameBodyNoHostSendsIsRefused)
{
    StatusRecords records = statusSet(3, 8);
    std::vector<std::uint8_t> body;
    CHECK_EQ(encodeStatusFragment(body, records, 0), std::size_t { 3 });
    REQUIRE(decodeStatusFragment(body).has_value());
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK_MSG(!decodeStatusFragment(std::span { body }.first(length)).has_value(), ("a body cut to " + std::to_string(length) + " bytes was read").c_str());
    std::vector<std::uint8_t> trailing = body;
    trailing.push_back(0);
    CHECK(!decodeStatusFragment(trailing).has_value());

    const auto frame = [](std::uint32_t total, std::uint32_t first, std::span<const OSC::ProgramStatus> carried) {
        std::vector<std::uint8_t> out;
        WireWriter writer(out);
        writer.u32(1);
        writer.varint(total);
        writer.varint(first);
        writer.varint(carried.size());
        for (const OSC::ProgramStatus& record : carried)
            writeStatusRecord(writer, record);
        return out;
    };
    const std::span<const OSC::ProgramStatus> all(records.store.records.data(), records.store.records.size());
    CHECK(decodeStatusFragment(frame(3, 0, all)).has_value());
    // More records than a pane holds, before any is read.
    CHECK(!decodeStatusFragment(frame(OSC::programStatusLimit + 1, 0, all)).has_value());
    // Records running past the set's total, and a first past it.
    CHECK(!decodeStatusFragment(frame(2, 0, all)).has_value());
    CHECK(!decodeStatusFragment(frame(3, 1, all)).has_value());
    CHECK(!decodeStatusFragment(frame(3, 4, {})).has_value());
    // A frame that carries nothing of a set that has something.
    CHECK(!decodeStatusFragment(frame(3, 0, {})).has_value());
    CHECK(decodeStatusFragment(frame(0, 0, {})).has_value());
    // The same id twice in one frame.
    std::vector<OSC::ProgramStatus> twice(all.begin(), all.end());
    twice[2].id = twice[0].id;
    CHECK(!decodeStatusFragment(frame(3, 0, twice)).has_value());
}

TEST(theRevisionMovesOnlyWhenWhatAClientIsShownChanges)
{
    Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    PaneSource source(terminal, ID128 { .low = 1, .high = 2 });
    CHECK_EQ(source.facts().revision, 0u);
    CHECK_EQ(source.status().revision, 0u);

    PaneFacts facts = sampleFacts();
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 1u);
    // The geometry is the arbiter's, whatever the caller wrote.
    CHECK(source.facts().geometry == source.geometry().status());
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 1u);

    // New stamps ride along until something else moves, unless the host asks.
    facts.activity.lastOutputUnixMilliseconds += 5'000;
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 1u);
    CHECK_EQ(source.facts().activity.lastOutputUnixMilliseconds, facts.activity.lastOutputUnixMilliseconds);
    // A stamp that rode along is still one no client was sent: the host's
    // next publish of stamps moves the record, and the one after does not.
    source.setFacts(facts, PaneSource::ActivityStamps::Publish);
    CHECK_EQ(source.facts().revision, 2u);
    source.setFacts(facts, PaneSource::ActivityStamps::Publish);
    CHECK_EQ(source.facts().revision, 2u);
    facts.activity.lastInputUnixMilliseconds += 1;
    source.setFacts(facts, PaneSource::ActivityStamps::Publish);
    CHECK_EQ(source.facts().revision, 3u);

    facts.programTitle = "another";
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 4u);
    // A pin moves the record through the arbiter.
    source.geometry().pin({ .rows = 30, .columns = 100 }, ID128 { .low = 5, .high = 6 });
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 5u);
    CHECK(source.facts().geometry.isPinned && source.facts().geometry.pinnedBy == (ID128 { .low = 5, .high = 6 }));
    source.geometry().unpin();
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 6u);
    CHECK(!source.facts().geometry.isPinned && !source.facts().geometry.pinnedBy);

    StatusRecords records = statusSet(2, 8);
    source.setStatus(records);
    CHECK_EQ(source.status().revision, 1u);
    records.revision = 99;
    source.setStatus(records);
    CHECK_EQ(source.status().revision, 1u);
    records.store.records.pop_back();
    source.setStatus(records);
    CHECK_EQ(source.status().revision, 2u);
    // A record no frame can carry is dropped where it is set.
    records.store.records.push_back(statusRecord("big", OSC::ProgramState::Working, std::string(maxStatusMessageBytes + 1, 'x')));
    source.setStatus(records);
    CHECK_EQ(source.status().revision, 2u);
    // Attention nobody has seen is a fact of the pane: it moves the facts.
    facts.attention.hasUnseenBell = !facts.attention.hasUnseenBell;
    source.setFacts(facts);
    CHECK_EQ(source.facts().revision, 7u);
    CHECK_EQ(source.status().store.records.size(), std::size_t { 1 });
}

TEST(factsAHostBuildsAlwaysDecode)
{
    // Words with no notification to belong to are a body a client refuses;
    // a host that built one is cut to what a client reads.
    PaneFacts worded;
    worded.attention.notificationTitle = "left over";
    worded.attention.notificationBody = "from a notification already seen";
    CHECK(!decodePaneFacts(encoded(worded)).has_value());
    const PaneFacts bounded = boundedForWire(worded);
    CHECK(bounded.attention == AttentionFacts {});
    CHECK(decodePaneFacts(encoded(bounded)).has_value());
    // Words past their cap are cut, not refused.
    PaneFacts wordy;
    wordy.attention = { .hasUnseenBell = false, .hasUnseenNotification = true, .notificationTitle = std::string(OSC::attentionTextCapacity + 40, 'T'), .notificationBody = std::string(OSC::attentionTextCapacity + 40, 'B') };
    CHECK(!decodePaneFacts(encoded(wordy)).has_value());
    const PaneFacts cut = boundedForWire(wordy);
    CHECK_EQ(cut.attention.notificationTitle.size(), OSC::attentionTextCapacity);
    CHECK(decodePaneFacts(encoded(cut)).has_value());
}

TEST(aTerminalsFactsAreReadIntoItsRecords)
{
    Terminal terminal;
    Ubiquitty::resize(terminal, 30, 100);
    UbiquittyTest::feed(terminal, "\033]2;make test\007\033]9;4;1;40\007\033]21337;indicator=#102030;status=busy\007\033]6;1;bg;red;brightness;200\007\033]1337;SetBadgeFormat=cHJvZA==\007");
    UbiquittyTest::feed(terminal, "\033]1337;SetUserVar=branch=bWFpbg==\007\033]1337;SetUserVar=stage=Mg==\007\033]133;A\007\033]133;B\007\033]133;C\007\033]133;D;3\007");
    UbiquittyTest::feed(terminal, "\033]7501;id=agent:app=codex:state=blocked:kind=question\033\\\007\033]777;notify;Deploy;went out\007");
    PaneHostFacts host;
    host.processChain = "make ◂ -zsh";
    host.foreground = { .processID = 77, .name = "make", .generation = 2 };
    host.isAtPrompt = true;
    host.activity.lastOutputUnixMilliseconds = 1234;
    const PaneFacts facts = paneFactsOf(terminal, host);
    CHECK_EQ(facts.revision, 0u);
    CHECK(facts.programTitle == "make test");
    CHECK_EQ(facts.rows, 30);
    CHECK_EQ(facts.columns, 100);
    CHECK(facts.processChain == "make ◂ -zsh" && facts.foreground == host.foreground && facts.isAtPrompt);
    CHECK(facts.hasMarkedPrompt);
    CHECK((facts.progress == ProgressFacts { .state = OSC::ProgressNormal, .value = 40 }));
    REQUIRE(facts.indicator.has_value());
    CHECK(facts.indicator->red == 0x10 && facts.indicator->green == 0x20 && facts.indicator->blue == 0x30);
    REQUIRE(facts.tabColor.has_value());
    CHECK_EQ(static_cast<int>(facts.tabColor->red), 200);
    CHECK(facts.badgeText == "prod");
    REQUIRE(facts.userVariables.size() == 2);
    CHECK((facts.userVariables[0] == UserVariableFact { .name = "branch", .value = "main" }));
    CHECK((facts.userVariables[1] == UserVariableFact { .name = "stage", .value = "2" }));
    CHECK(facts.exitStatus == std::optional<std::int32_t> { 3 });
    CHECK_EQ(facts.activity.lastOutputUnixMilliseconds, 1234u);

    const StatusRecords records = statusRecordsOf(terminal);
    REQUIRE(records.store.records.size() == 1);
    CHECK(records.store.records[0].state == OSC::ProgramState::Blocked && records.store.records[0].kind == OSC::ProgramBlockKind::Question);
    CHECK(facts.attention.hasUnseenBell && facts.attention.hasUnseenNotification);
    CHECK(facts.attention.notificationTitle == "Deploy" && facts.attention.notificationBody == "went out");
    // Seen, the words go with the latch.
    CHECK(Ubiquitty::markAttentionSeen(terminal));
    CHECK(paneFactsOf(terminal, host).attention == AttentionFacts {});
    // The directory the host read stands in only while the program reported none.
    host.foregroundDirectory = "/home/user/probed";
    CHECK(paneFactsOf(terminal, host).currentDirectory == "/home/user/probed");
    UbiquittyTest::feed(terminal, "\033]7;file://host/home/user/reported\007");
    CHECK(paneFactsOf(terminal, host).currentDirectory == terminal.currentDirectory && !terminal.currentDirectory.empty());
}

// A pane whose foreground is a plain ssh is somewhere else: the directory the
// host reads for it is the ssh client's folder on the host, which the session
// has left. Its facts name only what the far side reported, so a client's
// listing and title never show the host's folder for a pane on another machine.
TEST(aHostedPaneUnderAPlainSSHNamesOnlyTheFarSidesDirectory)
{
    Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    PaneHostFacts host;
    host.foregroundDirectory = "/home/user/project";
    CHECK(paneFactsOf(terminal, host).currentDirectory == "/home/user/project");

    host.foregroundLeadsTo = Ubiquitty::PAL::Process::RemoteKind::Host;
    CHECK_MSG(paneFactsOf(terminal, host).currentDirectory.empty(), "a pane under ssh named the host's folder for its client");

    UbiquittyTest::feed(terminal, "\033]7;file://build-1/srv/far\007");
    CHECK(paneFactsOf(terminal, host).currentDirectory == "/srv/far");

    host.foregroundLeadsTo = Ubiquitty::PAL::Process::RemoteKind::None;
    CHECK(paneFactsOf(terminal, host).currentDirectory == "/srv/far");
}

TEST(aRecordIsCarriedIntoAReplica)
{
    Terminal host;
    Ubiquitty::resize(host, 24, 80);
    UbiquittyTest::feed(host, "\033]9;4;1;40\007\033]21337;indicator=#102030;status=busy\007\033]6;1;bg;red;brightness;200\007\033]1337;SetBadgeFormat=cHJvZA==\007");
    UbiquittyTest::feed(host, "\033]1337;SetUserVar=branch=bWFpbg==\007\033]1337;SetUserVar=stage=Mg==\007\007\033]777;notify;Deploy;went out\007");
    Terminal replica;
    Ubiquitty::resize(replica, 24, 80);
    const std::uint32_t progressBefore = replica.progressGeneration;
    const std::uint32_t tabBefore = replica.tabStatusGeneration;
    const std::uint32_t attentionBefore = replica.attentionGeneration;
    applyRecorded(replica, paneFactsOf(host, {}));
    CHECK_EQ(static_cast<int>(replica.progressState), static_cast<int>(OSC::ProgressNormal));
    CHECK_EQ(static_cast<int>(replica.progressValue), 40);
    CHECK(Ubiquitty::tabStatusColor(&replica).has_value() && Ubiquitty::tabStatusColor(&replica)->green == 0x20);
    CHECK(Ubiquitty::tabStatusText(&replica) == Ubiquitty::tabStatusText(&host));
    CHECK(Ubiquitty::tabColor(&replica).has_value() && Ubiquitty::tabColor(&replica)->red == 200);
    CHECK(Ubiquitty::badgeText(&replica) == "prod");
    CHECK(replica.userVariables[0].used && replica.userVariables[0].name == "branch" && replica.userVariables[0].value == "main");
    CHECK(replica.userVariables[1].used && replica.userVariables[1].name == "stage");
    CHECK(!replica.userVariables[2].used);
    const Ubiquitty::UnseenAttention attention = Ubiquitty::unseenAttention(&replica);
    CHECK(attention.hasBell && attention.hasNotification && attention.notificationTitle == "Deploy");
    // Each member that changed moved the replica's own generation for it.
    CHECK(replica.progressGeneration != progressBefore);
    CHECK(replica.tabStatusGeneration != tabBefore);
    CHECK(replica.attentionGeneration != attentionBefore);

    // The same records again move nothing a frontend redraws on.
    const std::uint32_t progressAfter = replica.progressGeneration;
    const std::uint32_t tabAfter = replica.tabStatusGeneration;
    const std::uint32_t attentionAfter = replica.attentionGeneration;
    applyRecorded(replica, paneFactsOf(host, {}));
    CHECK_EQ(replica.progressGeneration, progressAfter);
    CHECK_EQ(replica.tabStatusGeneration, tabAfter);
    CHECK_EQ(replica.attentionGeneration, attentionAfter);

    // What the program cleared leaves the replica.
    UbiquittyTest::feed(host, "\033]9;4;0\007\033]21337;indicator=;status=;status-color=\007\033]1337;SetUserVar=branch=\007\033]1337;SetBadgeFormat=\007");
    CHECK(Ubiquitty::markAttentionSeen(host));
    applyRecorded(replica, paneFactsOf(host, {}));
    CHECK_EQ(static_cast<int>(replica.progressState), static_cast<int>(OSC::ProgressNone));
    CHECK(!Ubiquitty::tabStatusColor(&replica).has_value());
    CHECK(Ubiquitty::tabStatusText(&replica).empty());
    CHECK(Ubiquitty::badgeText(&replica).empty());
    CHECK(replica.userVariables[0].used && replica.userVariables[0].name == "stage");
    CHECK(!replica.userVariables[1].used && replica.userVariables[1].name.empty());
    CHECK(!Ubiquitty::unseenAttention(&replica).hasBell && !Ubiquitty::unseenAttention(&replica).hasNotification);
    CHECK(replica.unseenNotificationTitle.empty());
}

TEST(remoteProgressRecordsReachPaneAndTabPresentationWithoutGrantingExternalEffects)
{
    Ubiquitty::HostCapabilityPolicy policy;
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const auto handle = link.addPane();
    link.stepUntilQuiet();
    CHECK(policy.check(Ubiquitty::Capability::ProgressIndicator, Ubiquitty::CapabilityOrigin::Remote, {}) == Ubiquitty::CapabilityDecision::Deny);
    for (const auto sample : { ProgressFacts { .state = OSC::ProgressIndeterminate, .value = 0 },
             ProgressFacts { .state = OSC::ProgressNormal, .value = 42 },
             ProgressFacts { .state = OSC::ProgressNone, .value = 0 } }) {
        const auto generation = link.clientTerminal().progressGeneration;
        link.edit([&](Terminal& terminal) {
            terminal.capabilityPolicy = &policy;
            terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
            UbiquittyTest::feed(terminal, "\033]9;4;" + std::to_string(sample.state) + ";" + std::to_string(sample.value) + "\007");
        });
        setRecords(link, handle);
        link.step();
        link.step();
        const PaneFacts* facts = link.client().paneFacts(handle);
        REQUIRE(facts);
        CHECK(facts->progress == sample);
        CHECK_EQ(link.clientTerminal().progressState, sample.state);
        CHECK_EQ(link.clientTerminal().progressValue, sample.value);
        CHECK(link.clientTerminal().progressGeneration != generation);
        CHECK(OSC::paneBarVisible(facts->progress.state, OSC::SplitLayout::Split) == (sample.state != OSC::ProgressNone));
        CHECK(OSC::tabButtonBarVisible(facts->progress.state, OSC::TabSelectionState::Active, OSC::TabLayout::Tabbed, OSC::SplitLayout::Unsplit) == (sample.state != OSC::ProgressNone));
        CHECK(policy.check(Ubiquitty::Capability::ProgressIndicator, Ubiquitty::CapabilityOrigin::Remote, {}) == Ubiquitty::CapabilityDecision::Deny);
    }
}

TEST(recordsReachABoundPaneInEveryViewState)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t live = link.addPane();
    const std::uint32_t dormant = link.addPane();
    const std::uint32_t recordsOnly = link.addPane();
    link.client().setView(dormant, ViewState::Dormant);
    link.client().setView(recordsOnly, ViewState::RecordsOnly);
    link.step();
    // Before a host sets them a pane has no records, and none is sent.
    CHECK(link.client().paneFacts(live) == nullptr);
    CHECK(!link.client().paneStatus(dormant).has_value());
    CHECK_EQ(link.host().counters().paneFactsSent, 0u);

    for (const std::uint32_t handle : { live, dormant, recordsOnly }) {
        link.write(handle, "\033]2;pane " + std::to_string(handle) + "\007\033]7501;id=job:state=working\033\\\033]9;4;1;" + std::to_string(10 * handle) + "\007");
        setRecords(link, handle);
    }
    link.step();
    link.step();
    for (const std::uint32_t handle : { live, dormant, recordsOnly }) {
        const PaneFacts* facts = link.client().paneFacts(handle);
        const std::optional<StatusRecords> status = link.client().paneStatus(handle);
        REQUIRE(facts != nullptr && status.has_value());
        CHECK(*facts == link.source(handle).facts());
        // One carrier a reader: the live pane's status came with its screen
        // and carries no revision, the others' as the set the host sent.
        CHECK(status->store == link.source(handle).status().store);
        CHECK_EQ(status->revision, handle == live ? 0u : link.source(handle).status().revision);
        REQUIRE(status->store.records.size() == 1);
        CHECK(facts->programTitle == "pane " + std::to_string(handle));
        CHECK_EQ(static_cast<int>(facts->progress.value), static_cast<int>(10 * handle));
        // The replica's terminal holds what the record carries, shown or not.
        CHECK_EQ(static_cast<int>(link.clientTerminal(handle).progressValue), static_cast<int>(10 * handle));
    }
    CHECK_EQ(link.host().counters().paneFactsSent, 3u);
    CHECK_EQ(link.host().counters().statusSetsSent, 2u);
    CHECK_EQ(link.client().counters().recordsApplied, 5u);
    CHECK(link.host().state() == ConnectionState::Attached && link.client().state() == ConnectionState::Attached);

    // Nothing moved, so nothing more is sent, and nothing is due.
    setRecords(link, live);
    link.step();
    link.step();
    CHECK_EQ(link.host().counters().paneFactsSent, 3u);
    CHECK_EQ(link.host().counters().statusSetsSent, 2u);
    CHECK(link.host().nextServiceMicroseconds(link.now()) != std::optional { link.now() });
    // A record that moves on a pane with nothing else to send is due at once.
    PaneFacts moved = link.source(recordsOnly).facts();
    moved.badgeText = "moved";
    link.source(recordsOnly).setFacts(moved);
    CHECK(link.host().nextServiceMicroseconds(link.now()) == std::optional { link.now() });
    link.step();
    REQUIRE(link.client().paneFacts(recordsOnly) != nullptr);
    CHECK(link.client().paneFacts(recordsOnly)->badgeText == "moved");
    CHECK_EQ(link.host().counters().paneFactsSent, 4u);
}

TEST(aPaneBoundAfterItsRecordsWereSetIsSentThemAheadOfItsPicture)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t first = link.addPane();
    link.settleAttach();
    // The host has had facts for a pane since before anyone bound it.
    const std::uint32_t late = link.bindPane(24, 80, "$ late");
    link.source(late).setFacts(sampleFacts());
    link.source(late).setStatus(statusSet(2, 16));
    FrameLog log(link, late);
    for (int steps = 0; steps < 64 && link.held(late) != link.newest(late); ++steps)
        link.step();
    const std::vector<FrameKind>& order = log.order();
    const auto facts = std::ranges::find(order, FrameKind::PaneFacts);
    const auto picture = std::ranges::find(order, FrameKind::CheckpointHead);
    REQUIRE(facts != order.end() && picture != order.end());
    CHECK(facts < picture);
    // The client shows the pane, so no status set left for it: not even in
    // the passes between its bind and the client's View reaching the host.
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 0 });
    CHECK_EQ(link.host().counters().statusSetsSent, 0u);
    REQUIRE(link.client().paneFacts(late) != nullptr);
    CHECK(link.client().paneFacts(late)->programTitle == "vim — notes.md");
    (void)first;
}

TEST(aPaneIsSentNoStatusSetUntilItsClientSaysHowItHoldsIt)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t first = link.addPane();
    link.settleAttach();
    // The client means to keep the next pane hidden: the view the host
    // starts every pane in, which the client states all the same.
    link.setViewOnBind([](std::uint32_t) { return ViewState::Dormant; });
    link.holdUpstream();
    const std::uint32_t hidden = link.bindPane(24, 80, "$ hidden");
    link.source(hidden).setFacts(sampleFacts());
    link.source(hidden).setStatus(statusSet(2, 16));
    FrameLog log(link, hidden);
    for (int steps = 0; steps < 8; ++steps)
        link.step();
    // Its facts left; its status waits for the client's word.
    CHECK_EQ(log[FrameKind::PaneFacts], std::size_t { 1 });
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 0 });
    link.releaseUpstream();
    for (int steps = 0; steps < 8; ++steps)
        link.step();
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 1 });
    REQUIRE(link.client().paneStatus(hidden).has_value());
    CHECK(link.client().paneStatus(hidden)->store == link.source(hidden).status().store);
    CHECK(link.host().paneView(hidden) == std::optional { ViewState::Dormant });
    (void)first;
}

TEST(aPaneLeavingLiveIsOwedItsStatusSetAndOneReturningIsNot)
{
    SyncLink link(0, 24);
    link.settleAttach();
    const std::uint32_t pane = link.handle();
    FrameLog log(link, pane);
    link.write("\033]7501;id=job:state=working\033\\");
    setRecords(link, pane);
    link.step();
    link.step();
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 0 });
    REQUIRE(link.client().paneStatus(pane).has_value());
    CHECK(link.client().paneStatus(pane)->store == link.source(pane).status().store);

    // Hidden, the screen stops carrying the status: the set is owed whole,
    // though its revision has not moved.
    const std::uint32_t revision = link.source(pane).status().revision;
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    link.step();
    CHECK_EQ(link.source(pane).status().revision, revision);
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 1 });
    REQUIRE(link.client().paneStatus(pane).has_value());
    CHECK_EQ(link.client().paneStatus(pane)->revision, revision);
    // A report posted while hidden reaches the client as a set.
    link.write("\033]7501;id=job:state=done\033\\");
    setRecords(link, pane);
    link.step();
    link.step();
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 2 });
    CHECK(link.client().paneStatus(pane)->store.records[0].state == OSC::ProgramState::Done);

    // Shown again, the set the host sent is not kept beside the screen, and
    // a later report rides the screen alone.
    link.client().setView(pane, ViewState::Live);
    for (int steps = 0; steps < 16 && link.held(pane) != link.newest(pane); ++steps)
        link.step();
    link.write("\033]7501;id=job:state=blocked:kind=question\033\\");
    setRecords(link, pane);
    for (int steps = 0; steps < 16 && link.held(pane) != link.newest(pane); ++steps)
        link.step();
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 2 });
    REQUIRE(link.client().paneStatus(pane).has_value());
    CHECK_EQ(link.client().paneStatus(pane)->revision, 0u);
    CHECK(link.client().paneStatus(pane)->store == link.source(pane).status().store);
    CHECK(link.client().paneStatus(pane)->store.records[0].state == OSC::ProgramState::Blocked);

    // Hidden once more with nothing posted since: the client dropped the set
    // it was sent when it showed the pane, so the host owes it again.
    const std::uint32_t standing = link.source(pane).status().revision;
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    link.step();
    CHECK_EQ(link.source(pane).status().revision, standing);
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 3 });
    link.client().setView(pane, ViewState::Live);
    link.step();
    link.client().setView(pane, ViewState::Dormant);
    // What the client was sent before it showed the pane is not what it
    // holds now: the screen has been the carrier since.
    CHECK(!link.client().paneStatus(pane).has_value());
    link.step();
    link.step();
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 4 });
    REQUIRE(link.client().paneStatus(pane).has_value());
    CHECK_EQ(link.client().paneStatus(pane)->revision, standing);
    CHECK(link.host().state() == ConnectionState::Attached && link.client().state() == ConnectionState::Attached);
}

// A pane not live is sent no screen, so the set its host sends is its status:
// the set stands in the replica's terminal, where a frontend reads it, and
// moves the tab status a frontend redraws on. Shown again, the terminal holds
// the status the screen last carried, which the host's next screen goes on
// from, so a host whose status returned to it while the pane was hidden is
// shown as it is although its screen sends nothing for it.
TEST(aStatusSetStandsInTheTerminalOfAPaneNotShown)
{
    SyncLink link(0, 24);
    link.settleAttach();
    const std::uint32_t pane = link.handle();
    const Terminal& terminal = link.clientTerminal(pane);
    const auto settle = [&] {
        for (int steps = 0; steps < 16 && link.held(pane) != link.newest(pane); ++steps)
            link.step();
    };
    const auto shownState = [&] { return terminal.programStatus.records.empty() ? OSC::ProgramState::None : terminal.programStatus.records[0].state; };
    link.write("\033]7501;id=job:state=working\033\\");
    setRecords(link, pane);
    settle();
    CHECK(shownState() == OSC::ProgramState::Working);

    link.client().setView(pane, ViewState::Dormant);
    link.step();
    link.step();
    const std::uint32_t generation = terminal.tabStatusGeneration;
    link.write("\033]7501;id=job:state=blocked:kind=question\033\\");
    setRecords(link, pane);
    link.step();
    link.step();
    CHECK(shownState() == OSC::ProgramState::Blocked);
    CHECK(terminal.tabStatusGeneration != generation);

    // Shown: the screen's status first, then the screen catches up.
    link.client().setView(pane, ViewState::Live);
    CHECK(shownState() == OSC::ProgramState::Working);
    settle();
    CHECK(terminal.programStatus == link.source(pane).status().store);
    CHECK(shownState() == OSC::ProgramState::Blocked);

    // Hidden, the host posts a record, and is back to what the screen last
    // carried by the time the pane is shown: no screen carries the change.
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    link.step();
    link.write("\033]7501;id=job:state=done\033\\");
    setRecords(link, pane);
    link.step();
    link.step();
    CHECK(shownState() == OSC::ProgramState::Done);
    link.client().setView(pane, ViewState::Live);
    link.write("\033]7501;id=job:state=blocked:kind=question\033\\");
    setRecords(link, pane);
    settle();
    CHECK(terminal.programStatus == link.source(pane).status().store);
    CHECK(shownState() == OSC::ProgramState::Blocked);
    CHECK(link.host().state() == ConnectionState::Attached && link.client().state() == ConnectionState::Attached);
}

TEST(aPaneReturningToLiveAbandonsAStatusSetStillLeaving)
{
    SyncLink link(0, 24);
    link.settleAttach();
    const std::uint32_t pane = link.handle();
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    FrameLog log(link, pane);
    link.source(pane).setStatus(statusSet(OSC::programStatusLimit, maxStatusMessageBytes));
    link.step();
    link.step();
    const std::size_t left = log[FrameKind::StatusRecords];
    REQUIRE(left >= 1);
    CHECK_EQ(link.host().counters().statusSetsSent, 0u);
    // Shown before the set finished: the rest of it never leaves, and the
    // screen is not held behind it.
    link.client().setView(pane, ViewState::Live);
    link.write("shown again");
    for (int steps = 0; steps < 64 && link.held(pane) != link.newest(pane); ++steps)
        link.step();
    CHECK_EQ(link.held(pane), link.newest(pane));
    CHECK(log[FrameKind::StatusRecords] <= left + 1);
    CHECK_EQ(link.host().counters().statusSetsSent, 0u);
    CHECK(link.client().state() == ConnectionState::Attached);
}

TEST(recordsPrecedeTheUpdateInOnePublish)
{
    SyncLink link(0, 24);
    link.settleAttach();
    FrameLog log(link, link.handle());
    link.write("\033]2;built\007\033]7501;id=job:state=done\033\\ok");
    setRecords(link, link.handle());
    link.step();
    link.step();
    const std::vector<FrameKind>& order = log.order();
    const auto facts = std::ranges::find(order, FrameKind::PaneFacts);
    const auto update = std::ranges::find(order, FrameKind::Update);
    REQUIRE(facts != order.end() && update != order.end());
    CHECK(facts < update);
    CHECK_EQ(log[FrameKind::PaneFacts], std::size_t { 1 });
    // The pane is shown, so the update is the status report's one carrier.
    CHECK_EQ(log[FrameKind::StatusRecords], std::size_t { 0 });
    REQUIRE(link.client().paneStatus(link.handle()).has_value());
    CHECK(link.client().paneStatus(link.handle())->store == link.source(link.handle()).status().store);
    CHECK(link.client().paneStatus(link.handle())->store.records.size() == 1);
}

TEST(aSuspendedReaderWithTenPanesHoldsOneWindow)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    std::vector<std::uint32_t> panes;
    for (int index = 0; index < 10; ++index)
        panes.push_back(link.addPane());
    link.settleAttach();
    for (const std::uint32_t handle : panes)
        setRecords(link, handle);
    link.step();
    link.step();
    const std::uint64_t appliedBefore = link.client().counters().recordsApplied;
    const std::uint64_t factsBefore = link.host().counters().paneFactsSent;

    // The client stops acknowledging while every pane's facts and status
    // change on every publish.
    link.holdUpstream();
    for (int frame = 0; frame < 200; ++frame) {
        for (const std::uint32_t handle : panes) {
            link.write(handle, "\033]2;frame " + std::to_string(frame) + " " + std::string(600, 'x') + "\007\033]7501;id=job:state=working:progress=" + std::to_string(frame % 100) + "\033\\");
            setRecords(link, handle);
        }
        link.step();
        const std::uint64_t outstanding = link.host().pacer().outstandingBytes();
        CHECK_MSG(outstanding <= link.host().pacer().windowBytes(link.now()) + maxEnvelopeHeaderBytes + maxFrameBodyBytes,
            ("frame " + std::to_string(frame) + ": " + std::to_string(outstanding) + " bytes outstanding against a window of " + std::to_string(link.host().pacer().windowBytes(link.now()))).c_str());
    }
    CHECK(link.host().counters().windowRefusals > 0);
    // Owed, not queued: far fewer frames left than revisions moved.
    CHECK(link.host().counters().paneFactsSent - factsBefore < 400);

    link.releaseUpstream();
    for (int steps = 0; steps < 64; ++steps)
        link.step();
    for (const std::uint32_t handle : panes) {
        REQUIRE(link.client().paneFacts(handle) != nullptr && link.client().paneStatus(handle).has_value());
        CHECK_EQ(link.client().paneFacts(handle)->revision, link.source(handle).facts().revision);
        CHECK(link.client().paneStatus(handle)->store == link.source(handle).status().store);
        CHECK(*link.client().paneFacts(handle) == link.source(handle).facts());
    }
    // Each pane took the newest of each record, not one per revision skipped.
    CHECK(link.client().counters().recordsApplied - appliedBefore < 600);
    CHECK(link.host().state() == ConnectionState::Attached && link.client().state() == ConnectionState::Attached);
}

TEST(aStatusSetOverAFrameCrossesAConnectionWhole)
{
    SyncLink link(0, 24);
    link.settleAttach();
    // A set travels for a pane whose screen the client is not receiving.
    link.client().setView(link.handle(), ViewState::Dormant);
    link.step();
    FrameLog log(link, link.handle());
    const StatusRecords large = statusSet(OSC::programStatusLimit, maxStatusMessageBytes);
    link.source(link.handle()).setStatus(large);
    // The set leaves a frame a pass, each counted in the window.
    for (int steps = 0; steps < 256 && (!link.client().paneStatus(link.handle()) || link.client().paneStatus(link.handle())->store.records.size() != large.store.records.size()); ++steps) {
        link.step();
        CHECK(link.host().pacer().outstandingBytes() <= link.host().pacer().windowBytes(link.now()) + maxEnvelopeHeaderBytes + maxFrameBodyBytes);
    }
    CHECK(log[FrameKind::StatusRecords] > 1);
    CHECK_EQ(link.host().counters().statusSetsSent, 1u);
    CHECK_EQ(link.client().counters().recordsApplied, 1u);
    CHECK(link.client().state() == ConnectionState::Attached);
    const std::size_t framesOfFirst = log[FrameKind::StatusRecords];

    // A set that moves while its frames are leaving starts again, and the
    // client holds the newer one whole, never a mix. The client stops
    // acknowledging, so the window shuts partway through the second set.
    for (int steps = 0; steps < 8; ++steps)
        link.step();
    CHECK_EQ(link.host().pacer().outstandingBytes(), 0u);
    link.holdUpstream();
    StatusRecords second = large;
    for (OSC::ProgramStatus& record : second.store.records)
        record.title = "Second";
    link.source(link.handle()).setStatus(second);
    const std::uint64_t refusalsBefore = link.host().counters().windowRefusals;
    for (int steps = 0; steps < 64 && link.host().counters().windowRefusals == refusalsBefore; ++steps)
        link.step();
    REQUIRE(link.host().counters().windowRefusals > refusalsBefore);
    // Some of its frames left and the set did not: it is in transit.
    CHECK(log[FrameKind::StatusRecords] > framesOfFirst);
    CHECK(log[FrameKind::StatusRecords] < 2 * framesOfFirst);
    CHECK_EQ(link.host().counters().statusSetsSent, 1u);
    CHECK(link.client().paneStatus(link.handle())->store == large.store);
    StatusRecords third = second;
    for (OSC::ProgramStatus& record : third.store.records)
        record.title = "Third";
    link.source(link.handle()).setStatus(third);
    link.releaseUpstream();
    for (int steps = 0; steps < 256 && link.client().paneStatus(link.handle())->revision != link.source(link.handle()).status().revision; ++steps)
        link.step();
    CHECK(link.client().paneStatus(link.handle())->store == third.store);
    CHECK_EQ(link.host().counters().statusSetsSent, 2u);
    CHECK_EQ(link.client().counters().recordsApplied, 2u);
    CHECK(link.client().state() == ConnectionState::Attached && link.host().state() == ConnectionState::Attached);
}

// A window with room for several frames does not let the screen past a set
// that is still leaving: records precede the screen, a set's last frame
// included (Docs/sync.md §7.8).
TEST(theScreenWaitsBehindAStatusSetStillLeaving)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.pacing.windowFloorBytes = 4 << 20;
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    const std::uint32_t pane = link.addPane();
    link.settleAttach();
    // A hidden pane's screen is the one notice that it changed.
    link.client().setView(pane, ViewState::Dormant);
    link.step();
    FrameLog log(link, pane);
    link.source(pane).setStatus(statusSet(OSC::programStatusLimit, maxStatusMessageBytes));
    link.write(pane, "written while the set leaves");
    for (int steps = 0; steps < 64 && link.host().counters().statusSetsSent == 0; ++steps)
        link.step();
    link.step();
    const std::vector<FrameKind>& order = log.order();
    const auto firstOfSet = std::ranges::find(order, FrameKind::StatusRecords);
    const auto lastOfSet = std::ranges::find(order.rbegin(), order.rend(), FrameKind::StatusRecords).base();
    const auto notice = std::ranges::find(order, FrameKind::PaneDirty);
    REQUIRE(firstOfSet != order.end() && notice != order.end());
    CHECK(lastOfSet - firstOfSet > 1);
    CHECK_MSG(notice >= lastOfSet, "a pane's change left while its status set was still leaving");
    CHECK(link.client().paneStatus(pane).has_value() && link.client().paneStatus(pane)->store.records.size() == OSC::programStatusLimit);
}

TEST(aRecordNoHostSendsClosesTheConnectionAndLeavesWhatWasHeld)
{
    const auto deliver = [](SyncLink& link, FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body) {
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, channel, body));
        link.deliverToClient(frame);
    };
    {
        SyncLink link(0, 24);
        link.settleAttach();
        link.client().setView(link.handle(), ViewState::Dormant);
        link.step();
        link.write("\033]7501;id=job:state=working\033\\");
        setRecords(link, link.handle());
        link.step();
        link.step();
        REQUIRE(link.client().paneStatus(link.handle()).has_value());
        const StatusRecords held = *link.client().paneStatus(link.handle());
        const std::uint64_t applied = link.client().counters().recordsApplied;
        // An unknown state byte on the wire.
        std::vector<std::uint8_t> body;
        (void)encodeStatusFragment(body, statusSet(1, 8, 50), 0);
        body[body.size() - 3] = 6;
        deliver(link, FrameKind::StatusRecords, link.handle(), body);
        CHECK(link.client().state() == ConnectionState::Closed);
        CHECK(link.client().byeReason() == ByeReason::Malformed);
        CHECK(*link.client().paneStatus(link.handle()) == held);
        CHECK_EQ(link.client().counters().recordsApplied, applied);
    }
    {
        // A frame from the middle of a set nobody began.
        SyncLink link(0, 24);
        link.settleAttach();
        link.client().setView(link.handle(), ViewState::Dormant);
        link.step();
        const std::vector<std::vector<std::uint8_t>> frames = fragmentsOf(statusSet(64, maxStatusMessageBytes));
        REQUIRE(frames.size() >= 2);
        deliver(link, FrameKind::StatusRecords, link.handle(), frames[1]);
        CHECK(link.client().state() == ConnectionState::Closed);
        CHECK(!link.client().paneStatus(link.handle()).has_value());
    }
    {
        // Facts cut short, and facts for a pane never bound.
        SyncLink link(0, 24);
        link.settleAttach();
        std::vector<std::uint8_t> body = encoded(sampleFacts());
        body.pop_back();
        deliver(link, FrameKind::PaneFacts, link.handle(), body);
        CHECK(link.client().state() == ConnectionState::Closed);
        CHECK(link.client().paneFacts(link.handle()) == nullptr);
    }
    {
        SyncLink link(0, 24);
        link.settleAttach();
        deliver(link, FrameKind::PaneFacts, 77, encoded(sampleFacts()));
        CHECK(link.client().state() == ConnectionState::Closed);
    }
    {
        // A client says neither record: the host closes on one.
        CHECK(directionOf(FrameKind::PaneFacts) == Direction::HostToClient);
        CHECK(directionOf(FrameKind::StatusRecords) == Direction::HostToClient);
        CHECK(laneOf(FrameKind::PaneFacts) == Lane::Interactive && laneOf(FrameKind::StatusRecords) == Lane::Interactive);
        SyncLink link(0, 24);
        link.settleAttach();
        link.sendToHost(FrameKind::PaneFacts, encoded(sampleFacts()), link.handle());
        link.step();
        CHECK(link.host().state() == ConnectionState::Closed);
    }
}

UBIQUITTY_TEST_MAIN()
