// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_checkpoint.cpp — a Sync checkpoint rebuilds its host.
//
// A host terminal is driven by escape sequences, checkpointed, and the body
// applied to a passive client; the client must then pass the oracle's
// comparison (Tests/LoopbackPaneLink.h, firstDisagreement) and hold the
// parse-affecting state no frame shows: tab stops, the designated and invoked
// character sets, and the left and right margins. A body that is cut short or
// corrupted must leave the client exactly as it was.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using Ubiquitty::Protocol::Sync::ApplyOutcome;
using Ubiquitty::Protocol::Sync::AttachmentBaseline;
using Ubiquitty::Protocol::Sync::ScreenPublisher;
using Ubiquitty::Protocol::Sync::ScreenReplica;
using Ubiquitty::Protocol::Sync::UpdateOutcome;

TEST(promptRolesAndTheActiveSectionSurviveACheckpoint)
{
    using Ubiquitty::SemanticRole;
    Ubiquitty::Terminal host;
    Ubiquitty::Terminal client;
    Ubiquitty::resize(host, 3, 12);
    UbiquittyTest::feed(host, "\033]133;A\007$ \033]133;B\007ls");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(host));
    REQUIRE(publisher.checkpoint(host, 1, body, baseline));
    REQUIRE(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);
    CHECK(client.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->pen.semanticRole == SemanticRole::Prompt);
    CHECK(client.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(2))->pen.semanticRole == SemanticRole::Input);
    CHECK(client.screens.active->pen.semanticRole == SemanticRole::Input);
    UbiquittyTest::feed(client, "X\033]133;C\007Y");
    CHECK(client.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(4))->pen.semanticRole == SemanticRole::Input);
    CHECK(client.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(5))->pen.semanticRole == SemanticRole::Output);
}

TEST(explicitWidthTextSurvivesCheckpointAndLaterRunReplacement)
{
    Ubiquitty::Terminal host;
    Ubiquitty::Terminal client;
    Ubiquitty::resize(host, 4, 20);
    host.drawsSizedText = client.drawsSizedText = true;
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;
    std::vector<std::uint8_t> body;
    for (const auto text : { "abcdefghijklmnopqrstu", "ABCDEFGHIJKLMNOPQRSTU" }) {
        UbiquittyTest::feed(host, std::string("\033[H\033]66;s=2:w=7:n=1:d=3;") + text + "\007");
        REQUIRE(publisher.capture(host));
        body.clear();
        REQUIRE(publisher.checkpoint(host, 1, body, baseline));
        const auto outcome = applyCheckpoint(client, body, replica);
        CHECK_EQ(static_cast<int>(outcome), static_cast<int>(ApplyOutcome::Applied));
        REQUIRE(outcome == ApplyOutcome::Applied);
        for (int row : { 0, 1 }) {
            const auto* anchor = client.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(0));
            REQUIRE(anchor);
            CHECK(std::ranges::equal(Ubiquitty::runTextCodePoints(&client, *anchor), std::string_view(text)));
            const auto* spec = client.textSizeTable.lookup(anchor->pen.extendedAttributes.textSizeSpecID());
            REQUIRE(spec);
            CHECK(spec->isTextRun == 1);
            CHECK(spec->isProportional == 0);
        }
    }
}

TEST(penWireRejectsUnknownSemanticRolesAndReservedBits)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    for (const std::uint8_t highByte : { 6, 32, 128 }) {
        auto bytes = Sync::encodePen(Ubiquitty::defaultBlankCell.pen);
        bytes[7] = highByte;
        Sync::WireReader reader(bytes);
        (void)Sync::readPen(reader);
        CHECK(!reader.ok());
    }
    for (const auto role : { Ubiquitty::SemanticRole::Output, Ubiquitty::SemanticRole::Prompt, Ubiquitty::SemanticRole::Input }) {
        Ubiquitty::Pen pen = Ubiquitty::defaultBlankCell.pen;
        pen.semanticRole = role;
        pen.underlineIndex = Ubiquitty::colorRGB;
        const auto bytes = Sync::encodePen(pen);
        Sync::WireReader reader(bytes);
        const auto decoded = Sync::readPen(reader);
        CHECK(reader.ok());
        CHECK(decoded.underlineIndex == Ubiquitty::colorRGB);
        CHECK(decoded.semanticRole == role);
    }
}

// The DECAC marks cross the wire in bits 11 and 12 of the underline word, so a
// successor or client still tells a normal text colour standing in for the
// default from the same index selected by SGR.
TEST(penWireCarriesTheAssignedDefaultMarks)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    for (const int marks : { 0, 1, 2, 3 }) {
        Ubiquitty::Pen pen = Ubiquitty::defaultBlankCell.pen;
        pen.foregroundIndex = 1;
        pen.backgroundIndex = 2;
        pen.isForegroundAssignedDefault = marks & 1;
        pen.isBackgroundAssignedDefault = (marks >> 1) & 1;
        const auto bytes = Sync::encodePen(pen);
        CHECK_EQ(bytes[7] >> 3, marks);
        Sync::WireReader reader(bytes);
        const auto decoded = Sync::readPen(reader);
        CHECK(reader.ok());
        CHECK_EQ(decoded.isForegroundAssignedDefault, pen.isForegroundAssignedDefault);
        CHECK_EQ(decoded.isBackgroundAssignedDefault, pen.isBackgroundAssignedDefault);
        CHECK(std::memcmp(&decoded, &pen, sizeof pen) == 0);
    }
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

// Refuse a requested nothrow buffer size. Grid-only cases prepare outside
// the scope because the decoded screen arrays can have the same size.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

void drain(Ubiquitty::Terminal& terminal)
{
    std::array<char, 4096> discard;
    while (Ubiquitty::drainOutput(&terminal, { discard.data(), discard.size() }) > 0) {
    }
}

std::vector<std::uint8_t> checkpointOf(const Ubiquitty::Terminal& host)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK_MSG(publisher.capture(host) && publisher.checkpoint(host, 1, body, baseline), "the host could not be checkpointed");
    return body;
}

std::string transferred(const Ubiquitty::Terminal& host, Ubiquitty::Terminal& client)
{
    ScreenReplica replica;
    const ApplyOutcome outcome = applyCheckpoint(client, checkpointOf(host), replica);
    if (outcome != ApplyOutcome::Applied)
        return "the checkpoint was refused: outcome " + std::to_string(static_cast<int>(outcome));
    return UbiquittyTest::firstDisagreement(host, client);
}

constexpr std::size_t titleStringCount = 22;
using TitleValues = std::array<std::string, titleStringCount>;
static_assert(titleStringCount == 2 + 2 * std::tuple_size_v<decltype(Ubiquitty::Terminal::titleUTF8Stack)>);
static_assert(std::tuple_size_v<decltype(Ubiquitty::Terminal::titleUTF8Stack)> == std::tuple_size_v<decltype(Ubiquitty::Terminal::iconNameUTF8Stack)>);

TitleValues longTitleValues(char marker)
{
    constexpr std::size_t longTitleLength = 256;
    static_assert(longTitleLength + titleStringCount < Ubiquitty::Terminal::maxTitleLength);
    TitleValues values;
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index].assign(longTitleLength + index, static_cast<char>('a' + index));
        values[index].front() = marker;
    }
    return values;
}

void setTitleState(Ubiquitty::Terminal& terminal, const TitleValues& values)
{
    terminal.titleUTF8 = values[0];
    terminal.iconNameUTF8 = values[1];
    const auto stackSize = terminal.titleUTF8Stack.size();
    for (std::size_t index = 0; index < stackSize; ++index) {
        terminal.titleUTF8Stack[index] = values[2 + index];
        terminal.iconNameUTF8Stack[index] = values[2 + stackSize + index];
    }
    // Entries beyond the active frames remain serialized owners too.
    terminal.titleStackLength = 3;
    terminal.titleStackValid = 5;
    terminal.iconNameStackValid = 2;
    terminal.characterSetG = { 0, 1, 2, 65 };
    terminal.characterSetActive = 2;
    terminal.characterSetGR = 3;
    terminal.singleShift = 2;
    terminal.mousePointerShapeStack.fill(Ubiquitty::MousePointerShape::Wait);
    terminal.mousePointerShapeStack.front() = Ubiquitty::MousePointerShape::Pointer;
    terminal.mousePointerShapeStack.back() = Ubiquitty::MousePointerShape::Text;
    terminal.mousePointerShapeStackLength = 2;
}

std::array<std::string_view, titleStringCount> titleStrings(const Ubiquitty::Terminal& terminal)
{
    std::array<std::string_view, titleStringCount> values { terminal.titleUTF8, terminal.iconNameUTF8 };
    const auto stackSize = terminal.titleUTF8Stack.size();
    for (std::size_t index = 0; index < stackSize; ++index) {
        values[2 + index] = terminal.titleUTF8Stack[index];
        values[2 + stackSize + index] = terminal.iconNameUTF8Stack[index];
    }
    return values;
}

std::array<const char*, titleStringCount> titleOwners(const Ubiquitty::Terminal& terminal)
{
    const auto strings = titleStrings(terminal);
    std::array<const char*, titleStringCount> owners {};
    for (std::size_t index = 0; index < owners.size(); ++index)
        owners[index] = strings[index].data();
    return owners;
}

void checkTitleValues(const Ubiquitty::Terminal& terminal, const TitleValues& expected)
{
    const auto strings = titleStrings(terminal);
    for (std::size_t index = 0; index < strings.size(); ++index)
        CHECK(strings[index] == expected[index]);
}

void checkTextStateFields(const Ubiquitty::Terminal& actual, const Ubiquitty::Terminal& expected)
{
    CHECK(actual.characterSetG == expected.characterSetG);
    CHECK(actual.characterSetActive == expected.characterSetActive);
    CHECK(actual.characterSetGR == expected.characterSetGR);
    CHECK(actual.singleShift == expected.singleShift);
    CHECK(actual.currentLinkID == expected.currentLinkID);
    CHECK(actual.titleStackLength == expected.titleStackLength);
    CHECK(actual.titleStackValid == expected.titleStackValid);
    CHECK(actual.iconNameStackValid == expected.iconNameStackValid);
    CHECK(actual.mousePointerShapeStack == expected.mousePointerShapeStack);
    CHECK(actual.mousePointerShapeStackLength == expected.mousePointerShapeStackLength);
}

// A screen that exercises most of what a cell can hold: colours, a wide
// character, a combining mark, a pool-resident emoji cluster, a link, an
// erased run carrying a background colour, a cell never written mid-row, a
// double-width line, a soft wrap and a prompt mark.
constexpr std::string_view richScreen = "\x1b]133;A\x07$ \x1b[1;31mred\x1b[0m \x1b[44m  \x1b[0m"
                                        "\xe4\xb8\xad\xe6\x96\x87 e\xcc\x81 "
                                        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 "
                                        "\x1b]8;id=x;https://example.com/\x1b\\link\x1b]8;;\x1b\\\x1b[5C!\r\n"
                                        "\x1b[43m\x1b[K\x1b[0m\r\n"
                                        "\x1b#6wide line\r\n"
                                        "0123456789012345678901234567890123456789wrapped";

} // namespace

// The host answers every program query; the replica a client connection
// draws its picture into is marked as answered elsewhere, so a query is
// answered once.
TEST(aReplicaLeavesEveryQueryToItsHost)
{
    UbiquittyTest::LoopbackPaneLink link(UbiquittyTest::LoopbackOptions { .rows = 6, .columns = 20 });
    link.feedHost("prompt$ ");
    Ubiquitty::processBytes(&link.host(), "\x1b[c");
    std::array<char, 256> reply {};
    CHECK(Ubiquitty::drainOutput(&link.host(), { reply.data(), reply.size() }) > 0);
    link.publish(Ubiquitty::Runtime::FrameClass::Settled);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
    CHECK(link.host().replyAuthority == Ubiquitty::ReplyAuthority::ThisTerminal);
    CHECK(link.client().replyAuthority == Ubiquitty::ReplyAuthority::Elsewhere);
}

TEST(aCheckpointRebuildsTheVisibleScreen)
{
    auto host = makeTerminal(6, 40);
    UbiquittyTest::feed(*host, richScreen);
    drain(*host);
    auto client = makeTerminal(24, 80);
    const std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(checkpointGeometryAdmissionUsesTheFullDecodersHeader)
{
    using namespace Ubiquitty::Protocol::Sync;
    auto host = makeTerminal(12, 37);
    const auto body = checkpointOf(*host);
    const auto bytes = checkpointCellStorageBytes(body);
    REQUIRE(bytes);
    CHECK(*bytes == 2 * 12 * 37 * sizeof(Ubiquitty::Cell));
    const auto prepared = prepareCheckpoint(body);
    REQUIRE(prepared);
    CHECK(prepared->rows() == 12);
    CHECK(prepared->columns() == 37);
    // Admission stops after activeScreen; the full decoder validates the
    // remaining body.
    for (std::size_t length = 0; length < 17; ++length)
        CHECK(!checkpointCellStorageBytes(std::span(body).first(length)));
    CHECK(checkpointCellStorageBytes(std::span(body).first(17)) == bytes);
    for (std::size_t field : { std::size_t { 12 }, std::size_t { 14 }, std::size_t { 16 } }) {
        auto malformed = body;
        malformed[field] = field == 16 ? 2 : 0;
        if (field != 16)
            malformed[field + 1] = 0;
        CHECK(!checkpointCellStorageBytes(malformed));
        const auto result = prepareCheckpoint(malformed);
        REQUIRE(!result);
        CHECK(result.error() == ApplyOutcome::Malformed);
    }
}

TEST(aCheckpointMarksProportionalRowsAfterInstallingTheirTextSpecs)
{
    auto host = makeTerminal(32, 77);
    host->drawsSizedText = true;
    host->drawsProportionalText = true;
    UbiquittyTest::feed(*host, "\x1b[6;10H\x1b]66;p=1:c=58;Review queue\x1b\\"
                               "\x1b[?1049h\x1b[8;10H\x1b]66;p=1:c=58;Check image placement after reconnecting\x1b\\");
    auto client = makeTerminal(32, 77);
    client->drawsSizedText = true;
    client->drawsProportionalText = true;
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*host), replica) == ApplyOutcome::Applied);
    for (int screen = 0; screen < 2; ++screen) {
        const int row = screen == 0 ? 5 : 7;
        const auto mode = screen == 0 ? Ubiquitty::ScreenMode::Primary : Ubiquitty::ScreenMode::Alternate;
        const auto source = host->grid->rowCells(mode, Ubiquitty::Row(row));
        const auto target = client->grid->rowCells(mode, Ubiquitty::Row(row));
        REQUIRE(Ubiquitty::isProportionalRunAnchor(client.get(), target.data[9]));
        CHECK(std::ranges::equal(Ubiquitty::runTextCodePoints(host.get(), source.data[9]),
            Ubiquitty::runTextCodePoints(client.get(), target.data[9])));
        // The renderer takes its proportional-font route only on flowed rows.
        // Correct cells alone otherwise paint just the monospace anchor.
        CHECK(client->grid->rowMetaAt(screen, row).has(Ubiquitty::RowFlag::Flowed));
    }
}

TEST(aPreparedCheckpointOwnsItsDecodedStateWithoutChangingTheDestination)
{
    auto host = makeTerminal(6, 40);
    UbiquittyTest::feed(*host, richScreen);
    drain(*host);
    auto client = makeTerminal(3, 20);
    UbiquittyTest::feed(*client, "previous picture");
    const auto before = checkpointOf(*client);
    auto body = checkpointOf(*host);
    auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
    REQUIRE(prepared);
    const auto generation = prepared->generation();
    const auto epoch = prepared->historyEpoch();
    const auto head = prepared->historyHead();
    const auto digest = prepared->stateDigest();
    CHECK(checkpointOf(*client) == before);
    // No span into an input carrier or decrypted plaintext may survive the
    // preparation call. Poison and free that storage before committing.
    std::fill(body.begin(), body.end(), 0xA5);
    std::vector<std::uint8_t>().swap(body);
    ScreenReplica replica;
    REQUIRE(commitCheckpoint(*client, std::move(*prepared), replica) == ApplyOutcome::Applied);
    CHECK_EQ(replica.generation, generation);
    CHECK_EQ(replica.historyEpoch, epoch);
    CHECK_EQ(replica.historyHead, head);
    CHECK(replica.stateDigest == digest);
    const auto difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(aCheckpointPreservesAllEighteenLongTitleValues)
{
    auto host = makeTerminal(6, 40);
    const auto expected = longTitleValues('h');
    setTitleState(*host, expected);
    auto client = makeTerminal(3, 20);
    setTitleState(*client, longTitleValues('c'));
    auto body = checkpointOf(*host);
    auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
    REQUIRE(prepared);
    std::fill(body.begin(), body.end(), 0xA5);
    std::vector<std::uint8_t>().swap(body);

    ScreenReplica replica;
    REQUIRE(commitCheckpoint(*client, std::move(*prepared), replica) == ApplyOutcome::Applied);
    checkTitleValues(*client, expected);
    checkTextStateFields(*client, *host);
    CHECK(client->titleChanged);
}

TEST(aRefusedCheckpointKeepsAllEighteenTitleOwners)
{
    constexpr int rows = 64;
    constexpr int columns = 128;
    constexpr std::size_t selectionBytes = rows * ((columns + 7) / 8);
    auto host = makeTerminal(rows, columns);
    setTitleState(*host, longTitleValues('h'));
    const auto body = checkpointOf(*host);
    auto client = makeTerminal(3, 20);
    const auto expected = longTitleValues('c');
    setTitleState(*client, expected);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    const auto ownersBefore = titleOwners(*client);
    const auto before = checkpointOf(*client);
    const auto digestBefore = replica.stateDigest;
    const auto hashesBefore = replica.blockHashes;

    CHECK(applyCheckpoint(*client, std::span(body).first(body.size() - 1), replica) == ApplyOutcome::Malformed);
    CHECK(titleOwners(*client) == ownersBefore);
    checkTitleValues(*client, expected);
    CHECK(checkpointOf(*client) == before);
    auto corrupted = body;
    corrupted.back() ^= 1;
    CHECK(applyCheckpoint(*client, corrupted, replica) == ApplyOutcome::DigestMismatch);
    CHECK(titleOwners(*client) == ownersBefore);
    checkTitleValues(*client, expected);
    CHECK(checkpointOf(*client) == before);

    auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
    REQUIRE(prepared);
    const auto refusalsBefore = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    ApplyOutcome outcome;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(selectionBytes);
        outcome = commitCheckpoint(*client, std::move(*prepared), replica);
    }
    REQUIRE(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > refusalsBefore);
    CHECK(outcome == ApplyOutcome::AllocationRefused);
    CHECK(titleOwners(*client) == ownersBefore);
    checkTitleValues(*client, expected);
    CHECK(checkpointOf(*client) == before);
    CHECK(replica.stateDigest == digestBefore);
    CHECK(replica.blockHashes == hashesBefore);
}

TEST(anUpdateConsumesAllEighteenDecodedTitleOwners)
{
    auto host = makeTerminal(6, 40);
    auto expected = longTitleValues('a');
    setTitleState(*host, expected);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(6, 40);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    // Reusing the moved-from scratch on a later update must remain valid.
    for (const char marker : { 'b', 'c' }) {
        const auto ownersBefore = titleOwners(*client);
        const auto before = checkpointOf(*client);
        const auto digestBefore = replica.stateDigest;
        const auto hashesBefore = replica.blockHashes;
        const auto next = longTitleValues(marker);
        setTitleState(*host, next);
        body.clear();
        REQUIRE(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
        REQUIRE(!body.empty());

        CHECK(applyRowUpdate(*client, std::span(body).first(body.size() - 1), replica) == ApplyOutcome::Malformed);
        CHECK(titleOwners(*client) == ownersBefore);
        checkTitleValues(*client, expected);
        CHECK(checkpointOf(*client) == before);
        auto corrupted = body;
        corrupted.back() ^= 1;
        REQUIRE(applyRowUpdate(*client, corrupted, replica) == ApplyOutcome::DigestMismatch);
        CHECK(titleOwners(*client) == ownersBefore);
        checkTitleValues(*client, expected);
        CHECK(checkpointOf(*client) == before);
        CHECK(replica.stateDigest == digestBefore);
        CHECK(replica.blockHashes == hashesBefore);
        REQUIRE(replica.scratch);
        checkTitleValues(*replica.scratch, next);
        const auto decodedOwners = titleOwners(*replica.scratch);
        for (std::size_t index = 0; index < decodedOwners.size(); ++index)
            REQUIRE(decodedOwners[index] != ownersBefore[index]);

        // The configured string implementations reuse an equal-length
        // assign's capacity on the retry. The subsequent move must give each
        // exact decoded heap buffer to the destination, not copy its text.
        REQUIRE(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
        CHECK(titleOwners(*client) == decodedOwners);
        checkTitleValues(*client, next);
        checkTextStateFields(*client, *host);
        CHECK(client->titleChanged);
        expected = next;
    }
    const auto transferredOwners = titleOwners(*client);
    replica.scratch.reset();
    CHECK(titleOwners(*client) == transferredOwners);
    checkTitleValues(*client, expected);
}

TEST(discardingAPreparedCheckpointDoesNotPublishItsPicture)
{
    auto host = makeTerminal(6, 40);
    UbiquittyTest::feed(*host, richScreen);
    auto client = makeTerminal(3, 20);
    UbiquittyTest::feed(*client, "previous picture");
    const auto before = checkpointOf(*client);
    {
        auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(checkpointOf(*host));
        REQUIRE(prepared);
    }
    CHECK(checkpointOf(*client) == before);
}

TEST(aRefusedCheckpointPreparationOwnsNoDestinationState)
{
    auto host = makeTerminal(6, 40);
    UbiquittyTest::feed(*host, richScreen);
    const auto body = checkpointOf(*host);
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(sizeof(Ubiquitty::Terminal));
        auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
        REQUIRE(!prepared);
        CHECK(prepared.error() == ApplyOutcome::AllocationRefused);
    }
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > refusals);
    REQUIRE(Ubiquitty::Protocol::Sync::prepareCheckpoint(body));
}

TEST(aCheckpointWhoseGridCannotGrowLeavesItsTerminalAndReplicaIntact)
{
    constexpr int rows = 64;
    constexpr int columns = 128;
    constexpr std::size_t cellBytes = rows * columns * sizeof(Ubiquitty::Cell);
    constexpr std::size_t selectionBytes = rows * ((columns + 7) / 8);
    auto host = makeTerminal(rows, columns);
    UbiquittyTest::feed(*host, richScreen);
    drain(*host);
    const auto body = checkpointOf(*host);

    auto client = makeTerminal(3, 20);
    UbiquittyTest::feed(*client, "\x1b[32mbefore\x1b[0m");
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, checkpointOf(*client), replica) == ApplyOutcome::Applied);
    UbiquittyTest::feed(*client, "\r\nfirst\r\nsecond\r\nthird\r\nfourth");
    drain(*client);
    const auto before = checkpointOf(*client);
    const auto beforeReplica = replica;
    const int historyLines = Ubiquitty::Screen::scrollbackCount(client.get());
    REQUIRE(historyLines > 0);
    // Exercise both the first allocation and a later refusal after the two
    // screen stores and their row tables have already been allocated.
    for (const std::size_t refusedBytes : { cellBytes, selectionBytes }) {
        auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
        REQUIRE(prepared);
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        ApplyOutcome outcome;
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(refusedBytes);
            outcome = commitCheckpoint(*client, std::move(*prepared), replica);
        }
        REQUIRE(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > refusals);
        CHECK(outcome == ApplyOutcome::AllocationRefused);
        CHECK_EQ(client->rows, 3);
        CHECK_EQ(client->columns, 20);
        CHECK(checkpointOf(*client) == before);
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(client.get()), historyLines);
        CHECK_EQ(replica.generation, beforeReplica.generation);
        CHECK_EQ(replica.dictionaryEpoch, beforeReplica.dictionaryEpoch);
        REQUIRE(replica.pens().size() == beforeReplica.pens().size());
        CHECK(std::memcmp(replica.pens().data(), beforeReplica.pens().data(), replica.pens().size() * sizeof(Ubiquitty::Pen)) == 0);
        CHECK_EQ(replica.maxPens, beforeReplica.maxPens);
        CHECK(replica.storage == beforeReplica.storage);
        CHECK(replica.blockHashes == beforeReplica.blockHashes);
        CHECK_EQ(replica.historyEpoch, beforeReplica.historyEpoch);
        CHECK_EQ(replica.historyHead, beforeReplica.historyHead);
        CHECK(replica.stateDigest == beforeReplica.stateDigest);
    }

    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(aCheckpointUsesGridCapacityAlreadyHeldDuringAllocationRefusal)
{
    constexpr int rows = 64;
    constexpr int columns = 128;
    constexpr std::size_t cellBytes = rows * columns * sizeof(Ubiquitty::Cell);
    auto host = makeTerminal(rows, columns);
    UbiquittyTest::feed(*host, richScreen);
    drain(*host);
    const auto body = checkpointOf(*host);

    auto client = makeTerminal(rows, columns);
    Ubiquitty::resize(*client, 3, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    ScreenReplica replica;
    auto prepared = Ubiquitty::Protocol::Sync::prepareCheckpoint(body);
    REQUIRE(prepared);
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    ApplyOutcome outcome;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(cellBytes);
        outcome = commitCheckpoint(*client, std::move(*prepared), replica);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusals);
    REQUIRE(outcome == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(aCheckpointWithAnInvalidDigestDoesNotReserveDestinationGridCapacity)
{
    constexpr int rows = 64;
    constexpr int columns = 128;
    // Distinguish destination grid backing from the private decoded cells.
    constexpr std::size_t selectionBytes = rows * ((columns + 7) / 8);
    auto host = makeTerminal(rows, columns);
    auto body = checkpointOf(*host);
    REQUIRE(!body.empty());
    body.back() ^= 1;
    auto client = makeTerminal(3, 20);
    ScreenReplica replica;
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    ApplyOutcome outcome;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(selectionBytes);
        outcome = applyCheckpoint(*client, body, replica);
    }
    CHECK(outcome == ApplyOutcome::DigestMismatch);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusals);
    CHECK_EQ(client->rows, 3);
    CHECK_EQ(client->columns, 20);
}

// A checkpoint names where the history stands — its epoch and head — and
// carries none of its lines: they travel on the history plane, so a resize
// that needs a checkpoint costs the screen and nothing of the history.
TEST(aCheckpointCarriesTheHistoryAddressAndNoLines)
{
    auto host = makeTerminal(4, 20);
    for (int line = 0; line < 30; ++line)
        UbiquittyTest::feed(*host, "\x1b[3" + std::to_string(line % 8) + "mline " + std::to_string(line) + "\x1b[0m\r\n");
    UbiquittyTest::feed(*host, "\x1b[3J");
    for (int line = 0; line < 30; ++line)
        UbiquittyTest::feed(*host, "line " + std::to_string(line) + "\r\n");
    drain(*host);
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(*host);
    CHECK(address.head > 0u);
    CHECK(address.epoch > 1u);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(4, 20);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    CHECK_EQ(replica.historyEpoch, address.epoch);
    CHECK_EQ(replica.historyHead, address.head);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(client.get()), 0);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(aCheckpointTakenOnTheAlternateScreenKeepsThePrimary)
{
    auto host = makeTerminal(5, 30);
    UbiquittyTest::feed(*host, "primary text\r\n\x1b[?1049h\x1b[2J\x1b[Halternate\x1b[3;4H");
    drain(*host);
    auto client = makeTerminal(5, 30);
    std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    // Leaving the alternate screen on both shows the primary the checkpoint carried.
    UbiquittyTest::feed(*host, "\x1b[?1049l");
    UbiquittyTest::feed(*client, "\x1b[?1049l");
    drain(*host);
    drain(*client);
    difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(checksumExtensionsSurviveACheckpoint)
{
    auto host = makeTerminal(2, 6);
    UbiquittyTest::feed(*host, "ABC\033[1#y");
    auto client = makeTerminal(2, 6);
    const std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK(client->checksumExtensions == host->checksumExtensions);
    for (Ubiquitty::Terminal* terminal : { host.get(), client.get() }) {
        UbiquittyTest::feed(*terminal, "\033[1;1;1;1;1;3*y");
        char reply[64] {};
        (void)UbiquittyTest::drain(*terminal, reply);
        CHECK(std::string_view(reply) == "\033P1!~00C6\033\\");
    }
}

TEST(keyboardSelectionSurvivesACheckpoint)
{
    auto host = makeTerminal(2, 6);
    UbiquittyTest::feed(*host, "\033[?1050h\033[?1035l\033P+p6c696e7578\033\\\033[>0;15m\033[>6;4m\033[>7;1f");
    auto client = makeTerminal(2, 6);
    const std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK(!client->modes.shouldHonorNumLock);
    CHECK(client->modes.keyboardType == Ubiquitty::Keys::KeyboardType::Termcap);
    CHECK(client->modes.termcapProfile == Ubiquitty::Keys::TermcapProfile::Linux);
    CHECK(client->xtermKeyModifiers[0] == 15);
    CHECK(client->xtermKeyModifiers[6] == 4);
    CHECK(client->xtermKeyFormats[7] == 1);
}

// The state a frame never shows but the next byte depends on. The shipped
// snapshot carries none of the three, so a client rebuilt from it prints the
// next tab, the next shifted-out character and the next margined line
// somewhere the host does not.
TEST(tabStopsCharacterSetsAndMarginsRoundTrip)
{
    auto host = makeTerminal(10, 40);
    // Clear every stop, then set them at 5, 13 and 29.
    UbiquittyTest::feed(*host, "\x1b[3g\x1b[1;6H\x1bH\x1b[1;14H\x1bH\x1b[1;30H\x1bH");
    UbiquittyTest::feed(*host, "\x1b[?2005;2006h\x1b[5;1H\x1bJ\x1b[9;1H\x1bJ");
    // G0 UK, G1 DEC special graphics, G2 and G3 designated too; SO invokes G1.
    UbiquittyTest::feed(*host, "\x1b(A\x1b)0\x1b*0\x1b+A\x0e");
    // DECLRMM on, margins at columns 5..20, a scroll region at rows 2..8.
    UbiquittyTest::feed(*host, "\x1b[?69h\x1b[5;20s\x1b[2;8r\x1b[1;1H");
    drain(*host);

    auto client = makeTerminal(10, 40);
    const std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK(client->tabStops == host->tabStops);
    CHECK(client->modes.shouldQuotePasteCharacters && host->modes.shouldQuotePasteCharacters);
    CHECK(client->modes.shouldPreservePasteNewlines && host->modes.shouldPreservePasteNewlines);
    CHECK(client->lineTabStops == host->lineTabStops);
    CHECK(client->lineTabStops[4] && client->lineTabStops[8] && !client->lineTabStops[0]);
    CHECK(client->tabStops[5] && client->tabStops[13] && client->tabStops[29] && !client->tabStops[8]);
    CHECK(client->characterSetG == host->characterSetG);
    CHECK_EQ(client->characterSetActive, 1);
    CHECK_EQ(client->characterSetActive, host->characterSetActive);
    CHECK(client->modes.leftRightMarginMode);
    CHECK_EQ(client->screens.active->scrollLeft, 4);
    CHECK_EQ(client->screens.active->scrollRight, 19);
    CHECK_EQ(client->screens.active->scrollTop, host->screens.active->scrollTop);
    CHECK_EQ(client->screens.active->scrollBottom, host->screens.active->scrollBottom);

    // The same bytes fed to both now land identically: a tab, a shifted-out
    // line-drawing character, and a line that wraps at the right margin.
    const std::string_view next = "\tq\tq x\x1b[5;19Habcdef";
    UbiquittyTest::feed(*host, next);
    UbiquittyTest::feed(*client, next);
    const std::string after = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(after.empty(), after.c_str());
}

TEST(characterTabStopsRoundTripAtStorageBoundaries)
{
    auto host = makeTerminal(2, Ubiquitty::maxColumns);
    UbiquittyTest::feed(*host, "\x1b[3g\x1bH\x1b[64G\x1bH\x1b[65G\x1bH");
    UbiquittyTest::feed(*host, "\x1b[" + std::to_string(Ubiquitty::maxColumns) + "G\x1bH");
    drain(*host);

    auto client = makeTerminal(2, Ubiquitty::maxColumns);
    const std::string difference = transferred(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK(client->tabStops == host->tabStops);
    CHECK_EQ(client->tabStops.count(), 4u);
    CHECK(client->tabStops[0]);
    CHECK(client->tabStops[63]);
    CHECK(client->tabStops[64]);
    CHECK(client->tabStops[Ubiquitty::maxColumns - 1]);
    UbiquittyTest::feed(*client, "\r\t");
    CHECK_EQ(client->screens.active->cursorX, 63);
    UbiquittyTest::feed(*client, "\t");
    CHECK_EQ(client->screens.active->cursorX, 64);
    UbiquittyTest::feed(*client, "\t");
    CHECK_EQ(client->screens.active->cursorX, Ubiquitty::maxColumns - 1);
}

// Every prefix of a body is refused, and a refused body changes nothing.
TEST(aTruncatedCheckpointIsRefusedBeforeAnyMutation)
{
    auto host = makeTerminal(6, 40);
    UbiquittyTest::feed(*host, richScreen);
    drain(*host);
    const std::vector<std::uint8_t> body = checkpointOf(*host);

    auto client = makeTerminal(3, 10);
    UbiquittyTest::feed(*client, "before");
    drain(*client);
    auto reference = makeTerminal(3, 10);
    UbiquittyTest::feed(*reference, "before");
    drain(*reference);
    for (std::size_t length = 0; length < body.size(); ++length) {
        ScreenReplica replica;
        const ApplyOutcome outcome = applyCheckpoint(*client, { body.data(), length }, replica);
        if (outcome == ApplyOutcome::Applied) {
            CHECK_MSG(false, ("a body cut to " + std::to_string(length) + " bytes applied").c_str());
            return;
        }
    }
    const std::string difference = UbiquittyTest::firstDisagreement(*reference, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A byte changed inside a row record that still decodes — a letter for
// another letter — builds a screen the host never had. The digest refuses it
// before the client is touched.
TEST(aCorruptedRowInACheckpointIsRefusedByTheDigest)
{
    auto host = makeTerminal(4, 20);
    UbiquittyTest::feed(*host, "hello world");
    drain(*host);
    std::vector<std::uint8_t> body = checkpointOf(*host);
    const std::string_view needle = "hello";
    const auto found = std::search(body.begin(), body.end(), needle.begin(), needle.end());
    CHECK(found != body.end());
    if (found == body.end())
        return;
    *found = 'j';

    auto client = makeTerminal(4, 20);
    UbiquittyTest::feed(*client, "untouched");
    drain(*client);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::DigestMismatch);
    auto reference = makeTerminal(4, 20);
    UbiquittyTest::feed(*reference, "untouched");
    drain(*reference);
    const std::string difference = UbiquittyTest::firstDisagreement(*reference, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// Updates after a checkpoint bring the client along, defining a new pen,
// a new link and a new cluster before the row that names them, and send
// nothing for rows whose hash the client already holds.
TEST(rowUpdatesFollowTheHostWithIncrementalDictionaries)
{
    auto host = makeTerminal(5, 30);
    UbiquittyTest::feed(*host, "first line\r\nsecond line");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(5, 30);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, "\r\n\x1b[35mmagenta \x1b]8;;https://example.org/\x1b\\here\x1b]8;;\x1b\\ "
                               "\xf0\x9f\x8f\xb3\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x8c\x88");
    drain(*host);
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());

    // Nothing changed: the update carries no op.
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    const std::size_t emptyUpdate = body.size();
    CHECK_MSG(emptyUpdate <= 10 + 1 + 16, ("an update with nothing to say took " + std::to_string(emptyUpdate) + " bytes").c_str());
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
}

// A row update whose row bytes were altered in flight fails its digest and
// is not applied; the client stays on the screen it held.
TEST(aCorruptedRowUpdateIsRefusedBeforeAnyMutation)
{
    auto host = makeTerminal(4, 20);
    UbiquittyTest::feed(*host, "steady");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(4, 20);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, "\r\nchanged");
    drain(*host);
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    const std::string_view needle = "changed";
    const auto found = std::search(body.begin(), body.end(), needle.begin(), needle.end());
    CHECK(found != body.end());
    if (found == body.end())
        return;
    *found = 'k';
    const ScreenReplica before = replica;
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::DigestMismatch);
    CHECK(replica.storage == before.storage);
    CHECK_EQ(Ubiquitty::Screen::visibleCell(client.get(), 0, 1, 0)->codePoint, 0u);
}

// An update built against another geometry or screen names a base the client
// does not hold, and only a checkpoint can follow.
TEST(aRowUpdateAcrossAResizeNeedsACheckpoint)
{
    auto host = makeTerminal(4, 20);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    Ubiquitty::resize(*host, 6, 20);
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::NeedsCheckpoint);
    UbiquittyTest::feed(*host, "\x1b[?1049h");
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 2, body, baseline));
    CHECK(baseline.isAlternate);
}

// An update is a whole frame, not a row stream: the cursor, a mode, a tab
// stop and the title travel with the rows, so after one update the client's
// screen passes the oracle without a checkpoint. The lines that scrolled into
// history are not the screen plane's: they travel on the history plane.
TEST(anUpdateCarriesTheCursorAndTheStateBlocks)
{
    auto host = makeTerminal(4, 20);
    UbiquittyTest::feed(*host, "one");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(4, 20);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, "\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\x1b[?2004h\x1b[?1h\x1b[3g\x1b[5G\x1bH\x1b]2;titled\x07\x1b[2;7H");
    drain(*host);
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(client.get()), 0);
    CHECK(client->tabStops[4]);
    CHECK(!client->tabStops[8]);
    CHECK(client->titleUTF8 == "titled");
}

// Entering and leaving the alternate screen are updates too: the rows of
// both screens and every block travel, so a full-screen program costs its
// rows rather than a checkpoint carrying the history.
TEST(aScreenSwitchTravelsAsAnUpdate)
{
    auto host = makeTerminal(4, 20);
    UbiquittyTest::feed(*host, "prompt$ ");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(4, 20);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    for (const std::string_view step : { std::string_view("\x1b[?1049h\x1b[2;3Hfull screen"), std::string_view("\x1b[?1049lback") }) {
        UbiquittyTest::feed(*host, step);
        drain(*host);
        body.clear();
        CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
        CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
        const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
        CHECK_MSG(difference.empty(), difference.c_str());
    }
}

// One attachment's checkpoint leaves every other attachment's baseline good:
// a client attaching to a pane must not force a checkpoint on the clients
// already there.
TEST(aCheckpointForOneAttachmentKeepsAnotherOnUpdates)
{
    auto host = makeTerminal(4, 20);
    UbiquittyTest::feed(*host, "\x1b[31mred");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline first;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, first));
    auto client = makeTerminal(4, 20);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, "\x1b[32m green");
    drain(*host);
    AttachmentBaseline second;
    std::vector<std::uint8_t> secondBody;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 2, secondBody, second));
    body.clear();
    CHECK(publisher.rowUpdate(*host, first, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// An update cut from an older base applies on a client already holding a
// later generation, which is what lets a datagram superset land on any state
// since its base.
TEST(anUpdateFromAnOlderBaseAppliesOnALaterGeneration)
{
    auto host = makeTerminal(3, 20);
    UbiquittyTest::feed(*host, "a\r\nb\r\nc");
    drain(*host);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(3, 20);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const AttachmentBaseline acknowledged = baseline;

    UbiquittyTest::feed(*host, "\r\nd\r\ne");
    drain(*host);
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, "\r\nf");
    drain(*host);
    AttachmentBaseline fromAcknowledged = acknowledged;
    body.clear();
    CHECK(publisher.capture(*host) && publisher.rowUpdate(*host, fromAcknowledged, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

TEST(testCheckpointRetainsPaletteStackForLaterPop)
{
    auto host = makeTerminal(3, 10);
    auto client = makeTerminal(3, 10);
    UbiquittyTest::feed(*host, "\033]4;1;#123456\007\033]13;#fedcba\007\033[#P\033]4;1;#abcdef\007");
    CHECK(transferred(*host, *client).empty());
    UbiquittyTest::feed(*host, "\033[#Q");
    UbiquittyTest::feed(*client, "\033[#Q");
    CHECK(host->palette == client->palette);
    CHECK(client->palette.indexed[1].red == 0x1212);
    CHECK(client->mousePointerForegroundRed == 0xfefe);
    CHECK(client->savedPalettePosition == 0);
}

TEST(kittyColorStackSurvivesACheckpoint)
{
    auto host = makeTerminal(3, 10);
    auto client = makeTerminal(3, 10);
    UbiquittyTest::feed(*host, "\033]10;blue\007\033]17;#123456\007\033]30001\007\033]10;red\007\033]117\007");
    CHECK(transferred(*host, *client).empty());
    UbiquittyTest::feed(*host, "\033]30101\007");
    UbiquittyTest::feed(*client, "\033]30101\007");
    CHECK(host->palette == client->palette);
    CHECK(client->palette.defaultForeground.blue == 0xffff);
    CHECK(client->palette.defaultForeground.red == 0);
    CHECK(client->selectionBackgroundSet);
    CHECK(client->selectionBackgroundRed == 0x1212);
    CHECK(client->savedPalettePosition == 0);
}

UBIQUITTY_TEST_MAIN()

TEST(allPointerShapesSurviveACheckpoint)
{
    using Shape = Ubiquitty::MousePointerShape;
    for (unsigned raw = 0; raw <= static_cast<unsigned>(Shape::NorthwestResize); ++raw) {
        Ubiquitty::Terminal host;
        Ubiquitty::Terminal client;
        const auto shape = static_cast<Shape>(raw);
        host.mousePointerShapeStack[0] = shape;
        host.mousePointerShapeStackLength = 1;
        ScreenPublisher publisher;
        AttachmentBaseline baseline;
        ScreenReplica replica;
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(host));
        REQUIRE(publisher.checkpoint(host, 1, body, baseline));
        REQUIRE(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);
        CHECK_EQ(client.mousePointerShapeStackLength, 1);
        CHECK(client.mousePointerShapeStack[0] == shape);
    }
}

TEST(screenPointerStacksAndLineSemanticsSurviveACheckpoint)
{
    Ubiquitty::Terminal host;
    Ubiquitty::Terminal client;
    UbiquittyTest::feed(host, "\033]22;>pointer,text\007\033]133;I\007\033[?47h\033]22;>wait,n-resize\007");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(host));
    REQUIRE(publisher.checkpoint(host, 1, body, baseline));
    REQUIRE(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);
    CHECK(client.mousePointerShapeStack == host.mousePointerShapeStack);
    CHECK(client.alternateMousePointerShapeStack == host.alternateMousePointerShapeStack);
    CHECK_EQ(client.mousePointerShapeStackLength, 2);
    CHECK_EQ(client.alternateMousePointerShapeStackLength, 2);
    CHECK(client.shouldEndSemanticRoleAtLineBreak);
}

TEST(crtSaverCheckpointRetainsModeAndWakeReason)
{
    Ubiquitty::Terminal host;
    Ubiquitty::Terminal client;
    Ubiquitty::resize(host, 3, 12);
    host.modes.shouldSaveCRT = true;
    host.screenBlanked = true;
    host.isCRTBlanked = true;
    CHECK(!client.modes.shouldSaveCRT);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(host));
    REQUIRE(publisher.checkpoint(host, 1, body, baseline));
    REQUIRE(applyCheckpoint(client, body, replica) == ApplyOutcome::Applied);
    CHECK(client.modes.shouldSaveCRT);
    CHECK(client.screenBlanked);
    CHECK(client.isCRTBlanked);
    UbiquittyTest::feed(client, "\033[?97l");
    CHECK(!client.screenBlanked);
}
