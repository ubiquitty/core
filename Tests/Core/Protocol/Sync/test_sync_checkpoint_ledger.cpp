// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_checkpoint_ledger.cpp — the checkpoint ledger names every member,
// and what it says travels does.
//
// The build already refuses a ledger that does not tile `Terminal`. A member
// small enough to land in a padding hole moves no offset, so this enumerates
// the members by name and requires each to be classified. Then it holds the
// ledger to the codec: every mode the ledger says the checkpoint carries is
// changed on a host, and must arrive changed.
//
// The parser has a ledger of its own (Docs/sync.md §10, §11): what is
// pending between two bytes never travels, and what a parser at ground reads
// does. The build refuses a parser member the ledger leaves out; the proof
// that it does is here, at compile time, against a ledger missing one.

#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/CheckpointLedger.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/ParseState.h"
#include "LoopbackPaneLink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Ubiquitty::Protocol::Sync::ApplyOutcome;
using Ubiquitty::Protocol::Sync::AttachmentBaseline;
using Ubiquitty::Protocol::Sync::Carriage;
using Ubiquitty::Protocol::Sync::FrameClass;
using Ubiquitty::Protocol::Sync::LedgerEntry;
using Ubiquitty::Protocol::Sync::parserLedger;
using Ubiquitty::Protocol::Sync::ParserLedgerEntry;
using Ubiquitty::Protocol::Sync::ParseRole;
using Ubiquitty::Protocol::Sync::ScreenPublisher;
using Ubiquitty::Protocol::Sync::ScreenReplica;

namespace {

#if defined(__has_builtin)
#if __has_builtin(__builtin_dump_struct)
#define UBQ_HAS_DUMP_STRUCT 1
#endif
#endif

#if defined(UBQ_HAS_DUMP_STRUCT)
// Collects the names of a structure's own members: `__builtin_dump_struct`
// calls with (format, indent, type, name, value...) per member, and a member
// of a member is indented further.
struct MemberNames {
    std::vector<std::string>* names;

    template<typename... Arguments>
    void operator()(const char*, Arguments... arguments) const { take(arguments...); }

    void take(...) const { }

    template<typename Value>
    void take(const char* indent, const char*, const char* name, Value) const
    {
        if (std::strcmp(indent, "  ") == 0)
            names->push_back(name);
    }

    void take(const char* indent, const char*, const char* name) const
    {
        if (std::strcmp(indent, "  ") == 0)
            names->push_back(name);
    }
};
#endif

template<std::size_t count>
bool isListed(const std::array<Ubiquitty::Protocol::Sync::LedgerEntry, count>& ledger, std::string_view name)
{
    return std::ranges::any_of(ledger, [name](const auto& entry) { return entry.name == name; });
}

/// The parser ledger's entries without one member, as a ledger that forgot
/// it would be.
///
/// - Parameter name: The member left out.
/// - Returns: The other entries.
consteval std::array<LedgerEntry, parserLedger.size() - 1> parserLedgerWithout(std::string_view name)
{
    std::array<LedgerEntry, parserLedger.size() - 1> entries {};
    std::size_t kept = 0;
    for (const ParserLedgerEntry& member : parserLedger) {
        if (member.entry.name != name && kept < entries.size())
            entries[kept++] = member.entry;
    }
    return entries;
}

/// The parser ledger with one member's carriage changed.
///
/// - Parameters:
///   - name: The member.
///   - carriage: Its new carriage.
/// - Returns: The ledger.
consteval std::array<ParserLedgerEntry, parserLedger.size()> parserLedgerCarrying(std::string_view name, Carriage carriage)
{
    std::array<ParserLedgerEntry, parserLedger.size()> ledger = parserLedger;
    for (ParserLedgerEntry& member : ledger) {
        if (member.entry.name == name)
            member.entry.carriage = carriage;
    }
    return ledger;
}

constexpr std::size_t parserSize = sizeof(Ubiquitty::Terminal::Parser);
constexpr std::size_t parserAlignment = alignof(Ubiquitty::Terminal::Parser);

// A ledger that forgets a pending member does not tile, so the build that
// holds it fails: a partial UTF-8 sequence, a CSI's parameters and an open
// cluster each.
static_assert(!Ubiquitty::Protocol::Sync::tiles(parserLedgerWithout("utf8"), parserSize, parserAlignment));
static_assert(!Ubiquitty::Protocol::Sync::tiles(parserLedgerWithout("parameters"), parserSize, parserAlignment));
static_assert(!Ubiquitty::Protocol::Sync::tiles(parserLedgerWithout("clusterCodePoints"), parserSize, parserAlignment));
static_assert(Ubiquitty::Protocol::Sync::tiles(Ubiquitty::Protocol::Sync::entriesOf(parserLedger), parserSize, parserAlignment));

// A pending member the checkpoint would carry, or a ground member it would
// leave behind, breaks the rule the build asserts.
static_assert(!Ubiquitty::Protocol::Sync::isParseRoleRuleHeld(parserLedgerCarrying("osc", Carriage::Checkpointed)));
static_assert(!Ubiquitty::Protocol::Sync::isParseRoleRuleHeld(parserLedgerCarrying("parseState", Carriage::Digested)));
static_assert(!Ubiquitty::Protocol::Sync::isParseRoleRuleHeld(parserLedgerCarrying("precedingItem", Carriage::HostOnly)));

// A log mode continues parsing from a checkpoint, so only from one taken
// with the parser at ground.
static_assert(Ubiquitty::Protocol::Sync::canStartLogMode(FrameClass::Boundary));
static_assert(Ubiquitty::Protocol::Sync::canStartLogMode(FrameClass::Settled));
static_assert(!Ubiquitty::Protocol::Sync::canStartLogMode(FrameClass::Flood));
static_assert(!Ubiquitty::Protocol::Sync::canStartLogMode(FrameClass::Forced));
static_assert(!Ubiquitty::Protocol::Sync::canStartLogMode(FrameClass::Structural));

/// Takes a checkpoint of a terminal and applies it to a fresh one.
///
/// - Parameter host: The terminal.
/// - Returns: The client's terminal, or null when the checkpoint did not apply.
std::unique_ptr<Ubiquitty::Terminal> checkpointed(const Ubiquitty::Terminal& host)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(host) || !publisher.checkpoint(host, 1, body, baseline))
        return nullptr;
    auto client = std::make_unique<Ubiquitty::Terminal>();
    ScreenReplica replica;
    if (applyCheckpoint(*client, body, replica) != ApplyOutcome::Applied)
        return nullptr;
    return client;
}

} // namespace

TEST(everyParserMemberIsClassified)
{
#if defined(UBQ_HAS_DUMP_STRUCT)
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    std::vector<std::string> names;
    __builtin_dump_struct(&terminal->parser, MemberNames { &names });
    CHECK_EQ(names.size(), parserLedger.size());
    for (const std::string& name : names) {
        const bool isListed = std::ranges::any_of(parserLedger, [&](const ParserLedgerEntry& member) { return member.entry.name == name; });
        CHECK_MSG(isListed, ("Terminal::Parser::" + name + " is not in the parser ledger").c_str());
    }
#else
    UbiquittyTest::skip("this compiler has no __builtin_dump_struct; the build's tiling check alone holds the ledger");
#endif
}

// What CSI b repeats is the parser's ground state: a checkpoint carries it,
// so a parser continuing from the client's copy repeats what the host's would.
TEST(theRepeatRecordSurvivesACheckpoint)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 10, 40);
    Ubiquitty::processBytes(host.get(), "x\x1b[3b");
    REQUIRE(host->parser.precedingItem.kind == Ubiquitty::PrecedingItem::Kind::Graphic);
    const std::unique_ptr<Ubiquitty::Terminal> client = checkpointed(*host);
    REQUIRE(client != nullptr);
    CHECK(client->parser.precedingItem.kind == host->parser.precedingItem.kind);
    CHECK_EQ(client->parser.precedingItem.codePoint, host->parser.precedingItem.codePoint);
    CHECK_EQ(client->parser.precedingItem.multiplier, host->parser.precedingItem.multiplier);

    // Both repeat the same character now.
    Ubiquitty::processBytes(host.get(), "\x1b[2b");
    Ubiquitty::processBytes(client.get(), "\x1b[2b");
    for (int column = 0; column < 8; ++column) {
        std::uint32_t sent[4] {};
        std::uint32_t received[4] {};
        (void)Ubiquitty::cellClusterCodePoints(host.get(), host->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column)), sent, 4);
        (void)Ubiquitty::cellClusterCodePoints(client.get(), client->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column)), received, 4);
        CHECK_MSG(sent[0] == received[0], ("column " + std::to_string(column) + " differs after the repeat").c_str());
    }
}

// A checkpoint taken mid-sequence carries none of it: the client's parser is
// at ground, and the rest of the sequence reaches it only as screen state.
TEST(aCheckpointTakenMidSequenceCarriesNoParserState)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 10, 40);
    Ubiquitty::processBytes(host.get(), "\x1b[3");
    REQUIRE(!Ubiquitty::VT::isParserAtGround(host.get()));
    const std::unique_ptr<Ubiquitty::Terminal> client = checkpointed(*host);
    REQUIRE(client != nullptr);
    CHECK(Ubiquitty::VT::isParserAtGround(client.get()));
    CHECK_EQ(client->parser.parameterCount, 0);
}

// A cursor saved at column 30 of a pane narrowed to 20 columns stays saved at
// 30: a later DECRC clamps it to the width then. A checkpoint of that pane
// applies, and the client's DECRC lands where the host's does. The same holds
// for a pane resized until its saved row is past the screen.
TEST(aSavedCursorPastANarrowedWidthSurvivesACheckpoint)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 10, 40);
    Ubiquitty::processBytes(host.get(), "\x1b[3;31H\x1b"
                                        "7\x1b[1;1H");
    Ubiquitty::resize(*host, 10, 20);
    REQUIRE(host->screens.active->savedX >= host->columns);
    const std::unique_ptr<Ubiquitty::Terminal> client = checkpointed(*host);
    REQUIRE(client != nullptr);
    CHECK_EQ(client->screens.active->savedX, host->screens.active->savedX);
    Ubiquitty::processBytes(host.get(), "\x1b"
                                        "8");
    Ubiquitty::processBytes(client.get(), "\x1b"
                                          "8");
    CHECK_EQ(client->screens.active->cursorX, host->screens.active->cursorX);
    CHECK_EQ(client->screens.active->cursorY, host->screens.active->cursorY);
}

// The saved row, on the screen not in use: the alternate screen saves a
// cursor low on a tall pane, the program leaves it, and the pane shrinks.
TEST(aSavedRowPastAShrunkScreenSurvivesACheckpoint)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 30, 40);
    Ubiquitty::processBytes(host.get(), "\x1b[?1049h\x1b[25;5H\x1b"
                                        "7\x1b[?1049l");
    Ubiquitty::resize(*host, 8, 40);
    REQUIRE(host->screens.alternate.savedY >= host->rows);
    const std::unique_ptr<Ubiquitty::Terminal> client = checkpointed(*host);
    REQUIRE(client != nullptr);
    CHECK_EQ(client->screens.alternate.savedY, host->screens.alternate.savedY);
}

TEST(everyTerminalMemberIsClassified)
{
#if defined(UBQ_HAS_DUMP_STRUCT)
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    std::vector<std::string> names;
    __builtin_dump_struct(terminal.get(), MemberNames { &names });
    CHECK_EQ(names.size(), Ubiquitty::Protocol::Sync::terminalLedger.size());
    for (const std::string& name : names)
        CHECK_MSG(isListed(Ubiquitty::Protocol::Sync::terminalLedger, name), ("Terminal::" + name + " is not in the checkpoint ledger").c_str());

    names.clear();
    __builtin_dump_struct(&terminal->modes, MemberNames { &names });
    CHECK_EQ(names.size(), Ubiquitty::Protocol::Sync::terminalModesLedger.size());
    for (const std::string& name : names)
        CHECK_MSG(isListed(Ubiquitty::Protocol::Sync::terminalModesLedger, name), ("Terminal::Modes::" + name + " is not in the checkpoint ledger").c_str());

    names.clear();
    __builtin_dump_struct(&terminal->screens.primary, MemberNames { &names });
    CHECK_EQ(names.size(), Ubiquitty::Protocol::Sync::screenStateLedger.size());
    for (const std::string& name : names)
        CHECK_MSG(isListed(Ubiquitty::Protocol::Sync::screenStateLedger, name), ("Screen::State::" + name + " is not in the checkpoint ledger").c_str());
#else
    UbiquittyTest::skip("this compiler has no __builtin_dump_struct; the build's tiling check alone holds the ledger");
#endif
}

// Every mode the ledger says a checkpoint carries is flipped on the host, and
// the client must hold the flipped value: a mode in the ledger that the modes
// block forgot arrives at its default.
TEST(everyCarriedModeArrives)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 10, 40);
    auto* bytes = reinterpret_cast<unsigned char*>(&host->modes); // the modes viewed as bytes, to change each member in place
    for (const auto& entry : Ubiquitty::Protocol::Sync::terminalModesLedger) {
        if (entry.carriage != Carriage::Digested && entry.carriage != Carriage::Checkpointed)
            continue;
        if (entry.size == 1) {
            // A bool, or the one byte-wide field: 1 is valid for both, and
            // flipping the low bit always changes the value.
            bytes[entry.offset] ^= 1;
        } else {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + entry.offset, sizeof(value));
            value += 3;
            std::memcpy(bytes + entry.offset, &value, sizeof(value));
        }
    }

    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = std::make_unique<Ubiquitty::Terminal>();
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto* sent = reinterpret_cast<const unsigned char*>(&host->modes); // the modes viewed as bytes, to compare member by member
    const auto* received = reinterpret_cast<const unsigned char*>(&client->modes); // the modes viewed as bytes, to compare member by member
    for (const auto& entry : Ubiquitty::Protocol::Sync::terminalModesLedger) {
        if (entry.carriage == Carriage::Digested || entry.carriage == Carriage::Checkpointed)
            CHECK_MSG(std::memcmp(sent + entry.offset, received + entry.offset, entry.size) == 0, ("Terminal::Modes::" + std::string(entry.name) + " did not arrive").c_str());
    }
}

// Every member the ledger says a record carries is one the oracle compares,
// and the oracle compares no member the ledger says nothing travels for: a
// member moved to `Recorded` with no comparison, or compared with no carrier,
// fails here rather than drifting unseen on a client.
TEST(everyRecordedMemberIsComparedByTheOracle)
{
    std::vector<std::string_view> recorded;
    for (const auto& entry : Ubiquitty::Protocol::Sync::terminalLedger) {
        if (entry.carriage == Carriage::Recorded)
            recorded.push_back(entry.name);
    }
    CHECK(!recorded.empty());
    CHECK_EQ(recorded.size(), UbiquittyTest::recordedMembers.size());
    for (const std::string_view name : recorded)
        CHECK_MSG(std::ranges::find(UbiquittyTest::recordedMembers, name, &UbiquittyTest::RecordedMember::name) != UbiquittyTest::recordedMembers.end(), ("Terminal::" + std::string(name) + " is Recorded and the oracle does not compare it").c_str());
    for (const UbiquittyTest::RecordedMember& member : UbiquittyTest::recordedMembers)
        CHECK_MSG(std::ranges::find(recorded, member.name) != recorded.end(), ("the oracle compares Terminal::" + std::string(member.name) + ", which the ledger does not class Recorded").c_str());
}

// Every recorded member is set on a host to a value a fresh replica does not
// hold, and the replica must hold it once the pane's facts have crossed their
// wire form: a member the ledger lists that the record forgot stays at its
// default.
TEST(everyRecordedMemberArrives)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 10, 40);
    Ubiquitty::processBytes(host.get(), "\033]9;4;4;40\007\033]21337;indicator=#102030;status=busy\007");
    Ubiquitty::processBytes(host.get(), "\033]6;1;bg;red;brightness;200\007\033]6;1;bg;green;brightness;100\007\033]6;1;bg;blue;brightness;50\007");
    Ubiquitty::processBytes(host.get(), "\033]1337;SetBadgeFormat=cHJvZA==\007\033]1337;SetUserVar=branch=bWFpbg==\007\007\033]777;notify;Deploy;went out\007");
    Ubiquitty::setCurrentFileURLString(*host, "file:///tmp/doc.txt");
    auto client = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*client, 10, 40);
    for (const UbiquittyTest::RecordedMember& member : UbiquittyTest::recordedMembers)
        CHECK_MSG(!member.isSame(*host, *client), ("the host's Terminal::" + std::string(member.name) + " was left at a fresh replica's value, so its arrival proves nothing").c_str());

    std::vector<std::uint8_t> factsBody;
    encodePaneFacts(factsBody, paneFactsOf(*host, {}));
    const std::optional<PaneFacts> facts = decodePaneFacts(factsBody);
    REQUIRE(facts.has_value());
    applyRecorded(*client, *facts);
    for (const UbiquittyTest::RecordedMember& member : UbiquittyTest::recordedMembers)
        CHECK_MSG(member.isSame(*host, *client), ("Terminal::" + std::string(member.name) + " did not arrive").c_str());
}

UBIQUITTY_TEST_MAIN()
