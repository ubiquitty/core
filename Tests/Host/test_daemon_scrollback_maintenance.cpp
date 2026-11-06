// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/LineStore.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace Ubiquitty::Host {

struct ScrollbackMaintenanceAccess {
    static int timeout(const Daemon& daemon, std::uint64_t now)
    {
        return daemon.scrollbackMaintenanceTimeout(now);
    }

    static void service(Daemon& daemon, std::uint64_t now)
    {
        daemon.serviceScrollbackMaintenance(now);
    }
};

}

namespace {

using Ubiquitty::Host::Daemon;
using Maintenance = Ubiquitty::Host::ScrollbackMaintenanceAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Session = std::remove_reference_t<decltype(Screen::makeSession(std::declval<Daemon&>(), 24, 80))>;

constexpr int terminalRows = 24;
constexpr int terminalColumns = 80;
constexpr int retainedLines = 10'000;
constexpr std::uint64_t foldTick = Ubiquitty::Runtime::scrollbackFoldTickMilliseconds;

struct HistorySession {
    Session& session;
    Ubiquitty::TestDoubles::StreamDouble& stream;
    int nextOrdinal = 0;
};

void appendBurst(Daemon& daemon, HistorySession& fixture)
{
    std::string output;
    for (int line = 0; line < 128; ++line) {
        std::string row = "row " + std::to_string(fixture.nextOrdinal++);
        row.append(72 - row.size(), 'x');
        output += row + "\r\n";
    }
    fixture.stream.pushInbound(output);
    Screen::servicePTY(daemon, fixture.session);
    CHECK_EQ(fixture.stream.bytesRemaining(), 0u);
}

HistorySession makeHistorySession(Daemon& daemon, int blocks = 6)
{
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto& scripted = *stream;
    auto& session = Screen::makeSession(daemon, terminalRows, terminalColumns, std::move(stream));
    Ubiquitty::setScrollbackLimit(session.term.get(), retainedLines);
    HistorySession fixture { session, scripted };
    for (int burst = 0; burst < 32 && (!session.term->lineStore || session.term->lineStore->blockCount() < blocks); ++burst) {
        appendBurst(daemon, fixture);
    }
    CHECK(session.term->lineStore && session.term->lineStore->blockCount() >= blocks);
    return fixture;
}

std::vector<std::string> historyRows(const Ubiquitty::Terminal& terminal)
{
    std::vector<std::string> rows;
    const int count = Ubiquitty::Screen::scrollbackCount(&terminal);
    for (int row = 0; row < count; ++row) {
        rows.push_back(Ubiquitty::Screen::absoluteRowText(terminal, row, terminal.columns));
    }
    return rows;
}

void finishMaintenance(Daemon& daemon, std::uint64_t& now)
{
    for (int turn = 0; turn < 256; ++turn) {
        const int timeout = Maintenance::timeout(daemon, now);
        if (timeout < 0) {
            return;
        }
        now += static_cast<std::uint64_t>(std::max(timeout, Ubiquitty::Runtime::scrollbackFoldTickMilliseconds));
        Maintenance::service(daemon, now);
    }
    CHECK_MSG(false, "the daemon kept scheduling scrollback maintenance after its cold blocks were visited");
}

/// Gives the daemon a real pollable descriptor whose readiness the test controls.
class ReadablePipeStream final : public Ubiquitty::PAL::Stream {
public:
    ReadablePipeStream(Ubiquitty::PAL::UnixFileDescriptor reader, Ubiquitty::PAL::UnixFileDescriptor writer)
        : m_reader(std::move(reader))
        , m_writer(std::move(writer))
    {
    }

    bool makeReadable()
    {
        constexpr char byte = 'x';
        return ::write(m_writer.get(), &byte, 1) == 1;
    }

    bool makeQuiet()
    {
        char byte;
        return ::read(m_reader.get(), &byte, 1) == 1;
    }

    int read(std::span<char> bytes) override { return m_stream.read(bytes); }
    int write(std::span<const char> bytes) override { return m_stream.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_stream.writeAll(bytes); }
    bool drainPendingWrite() override { return m_stream.drainPendingWrite(); }
    bool hasPendingWrite() const override { return m_stream.hasPendingWrite(); }
    int lastError() const override { return m_stream.lastError(); }
    int fileDescriptor() const override { return m_reader.get(); }
    Ubiquitty::PAL::ProcessFacts& process() override { return m_stream.process(); }
    void setWindowSize(int columns, int rows, int xPixels, int yPixels) override
    {
        m_stream.setWindowSize(columns, rows, xPixels, yPixels);
    }

private:
    Ubiquitty::PAL::UnixFileDescriptor m_reader;
    Ubiquitty::PAL::UnixFileDescriptor m_writer;
    Ubiquitty::TestDoubles::StreamDouble m_stream;
};

}

TEST(aDaemonWakesAfterAnOutputBurstAndSleepsAfterFolding)
{
    Daemon daemon;
    std::uint64_t now = 1000;
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
    auto fixture = makeHistorySession(daemon);
    auto& terminal = *fixture.session.term;
    REQUIRE(terminal.lineStore);
    auto& store = *terminal.lineStore;
    const auto before = historyRows(terminal);
    const auto address = Ubiquitty::Screen::historyAddress(terminal);
    const std::size_t rawBytes = store.arenaBytes();
    const int blocks = store.blockCount();
    CHECK_EQ(store.foldedBlockCount(), 0);
    fixture.session.isFrameDue = false;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);

    Maintenance::service(daemon, now);
    CHECK_EQ(store.foldedBlockCount(), Ubiquitty::Runtime::scrollbackFoldBlockBudget);
    CHECK_EQ(Maintenance::timeout(daemon, now), Ubiquitty::Runtime::scrollbackFoldTickMilliseconds);
    finishMaintenance(daemon, now);
    CHECK_EQ(store.foldedBlockCount(), blocks - Ubiquitty::LineStore::rawBlockWindow);
    CHECK(store.arenaBytes() < rawBytes);
    CHECK(store.foldedBytes() > 0);
    CHECK(historyRows(terminal) == before);
    CHECK(!fixture.session.isFrameDue);
    const auto after = Ubiquitty::Screen::historyAddress(terminal);
    CHECK_EQ(after.epoch, address.epoch);
    CHECK_EQ(after.oldest, address.oldest);
    CHECK_EQ(after.head, address.head);
    CHECK_EQ(after.clearedThrough, address.clearedThrough);

    const int folded = store.foldedBlockCount();
    for (int turn = 0; turn < 20; ++turn) {
        now += foldTick;
        Maintenance::service(daemon, now);
        CHECK_EQ(Maintenance::timeout(daemon, now), -1);
        CHECK_EQ(store.foldedBlockCount(), folded);
    }
    appendBurst(daemon, fixture);
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    finishMaintenance(daemon, now);
    CHECK(store.foldedBlockCount() > folded);
}

TEST(daemonMaintenanceSharesOneTimedBudgetAcrossSessions)
{
    Daemon daemon;
    auto first = makeHistorySession(daemon, 10);
    auto second = makeHistorySession(daemon, 6);
    auto third = makeHistorySession(daemon, 6);
    std::array<Session*, 3> sessions { &first.session, &second.session, &third.session };
    std::uint64_t now = 1000;
    int totalFolded = 0;
    for (int turn = 0; turn < 6; ++turn) {
        Maintenance::service(daemon, now);
        int after = 0;
        for (const auto* session : sessions) {
            after += session->term->lineStore->foldedBlockCount();
        }
        CHECK_EQ(after - totalFolded, Ubiquitty::Runtime::scrollbackFoldBlockBudget);
        CHECK_EQ(sessions[static_cast<std::size_t>(turn % 3)]->term->lineStore->foldedBlockCount(), turn / 3 + 1);
        totalFolded = after;
        for (int repeated = 0; repeated < 8; ++repeated) {
            Maintenance::service(daemon, now);
        }
        int repeatedTotal = 0;
        for (const auto* session : sessions) {
            repeatedTotal += session->term->lineStore->foldedBlockCount();
        }
        CHECK_EQ(repeatedTotal, totalFolded);
        CHECK_EQ(Maintenance::timeout(daemon, now), Ubiquitty::Runtime::scrollbackFoldTickMilliseconds);
        now += foldTick;
    }
    finishMaintenance(daemon, now);
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
}

TEST(aReadablePTYDoesNotStarveAQuietSessionsMaintenance)
{
    Daemon daemon;
    auto busy = makeHistorySession(daemon);
    auto quiet = makeHistorySession(daemon);
    std::array<int, 2> descriptors;
    REQUIRE(::pipe(descriptors.data()) == 0);
    auto pipeStream = std::make_unique<ReadablePipeStream>(Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[1]));
    auto& readiness = *pipeStream;
    busy.session.stream = std::move(pipeStream);
    REQUIRE(readiness.makeReadable());
    std::uint64_t now = 1000;
    finishMaintenance(daemon, now);
    CHECK_EQ(busy.session.term->lineStore->foldedBlockCount(), 0);
    CHECK_EQ(quiet.session.term->lineStore->foldedBlockCount(), quiet.session.term->lineStore->blockCount() - Ubiquitty::LineStore::rawBlockWindow);
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
    REQUIRE(readiness.makeQuiet());
    now += foldTick;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    finishMaintenance(daemon, now);
    CHECK(busy.session.term->lineStore->foldedBlockCount() > 0);
}

TEST(daemonMaintenanceSurvivesReflowClearAndSessionDeletion)
{
    Daemon daemon;
    auto first = makeHistorySession(daemon);
    auto second = makeHistorySession(daemon);
    std::uint64_t now = 1000;
    Maintenance::service(daemon, now);
    Ubiquitty::resize(*second.session.term, terminalRows, 96);
    const auto resized = historyRows(*second.session.term);
    const auto removedID = first.session.id;
    Screen::destroySession(daemon, removedID);
    CHECK(!Screen::hasSession(daemon, removedID));
    finishMaintenance(daemon, now);
    CHECK(historyRows(*second.session.term) == resized);
    CHECK(second.session.term->lineStore->foldedBlockCount() > 0);

    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
    Ubiquitty::resize(*second.session.term, terminalRows, 60);
    const auto reflowed = historyRows(*second.session.term);
    now += foldTick;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    finishMaintenance(daemon, now);
    CHECK(historyRows(*second.session.term) == reflowed);
    CHECK(second.session.term->lineStore->foldedBlockCount() > 0);

    appendBurst(daemon, second);
    now += foldTick;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    Ubiquitty::Screen::dropScrollback(second.session.term.get());
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
    Maintenance::service(daemon, now + foldTick);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(second.session.term.get()), 0);
    auto replacement = makeHistorySession(daemon);
    now += foldTick;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    finishMaintenance(daemon, now);
    CHECK(replacement.session.term->lineStore->foldedBlockCount() > 0);
    appendBurst(daemon, replacement);
    now += foldTick;
    CHECK_EQ(Maintenance::timeout(daemon, now), 0);
    replacement.session.shellExited = true;
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
}

TEST(aRefusedColdBlockSpendsItsTurnAndDoesNotKeepTheDaemonAwake)
{
    Daemon daemon;
    auto fixture = makeHistorySession(daemon);
    auto& store = *fixture.session.term->lineStore;
    auto* first = store.writableAt(0);
    REQUIRE(first && first->length > 0);
    // A lone surrogate has no UTF-8 row form, so this block must stay raw.
    first->cells[0].codePoint = 0xD800;
    const int coldBlocks = store.blockCount() - Ubiquitty::LineStore::rawBlockWindow;
    std::uint64_t now = 1000;
    Maintenance::service(daemon, now);
    CHECK_EQ(store.foldedBlockCount(), 0);
    CHECK_EQ(Maintenance::timeout(daemon, now), Ubiquitty::Runtime::scrollbackFoldTickMilliseconds);
    Maintenance::service(daemon, now);
    CHECK_EQ(store.foldedBlockCount(), 0);
    finishMaintenance(daemon, now);
    CHECK_EQ(store.foldedBlockCount(), coldBlocks - 1);
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
    REQUIRE(store.at(0));
    CHECK_EQ(store.at(0)->cells[0].codePoint, 0xD800u);
}

TEST(aSyncClientReadsHistoryFoldedByDaemonMaintenance)
{
    Daemon daemon;
    auto fixture = makeHistorySession(daemon);
    const auto expected = historyRows(*fixture.session.term);
    std::uint64_t now = 1000;
    finishMaintenance(daemon, now);
    REQUIRE(fixture.session.term->lineStore->foldedBlockCount() > 0);
    const auto index = Screen::attach(daemon, fixture.session.id);
    Screen::grantScope(daemon, index, { Ubiquitty::Protocol::Sync::Scope::Observe });
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client(terminalRows, terminalColumns, retainedLines);
    Screen::greet(daemon, index, client);
    Screen::publish(daemon, fixture.session, Ubiquitty::Runtime::FrameClass::Structural);
    Screen::deliver(daemon, index, client);
    REQUIRE(client.pane());
    CHECK(client.historyBytes() > 0);
    CHECK(historyRows(*client.pane()) == expected);
    CHECK_EQ(Maintenance::timeout(daemon, now), -1);
}

UBIQUITTY_TEST_MAIN()
