// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"

#include "Core/Modes/SyncOutput.h"
#include "Core/Mouse/Hilite.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

// test_sync_hold_clock.cpp — a mirror judges a synchronized update (DEC 2026)
// and a hilite await (DEC 1001) on its own clock, as a local terminal does.
//
// Both are bounded on the asker's 60 Hz monotonic clock: the update from the
// first frame decision that saw it, the await from its deadline. A host and
// its client do not share that clock: a remote host's ticks run from its own
// boot, and on one Mac the daemon's clock counts the machine's sleep while the
// app's does not. The screen plane carries the host's stamp as the wire
// version defines it, and the client keeps its own (Docs/sync.md, "Clocks").
// Each case stamps on the host at one clock, carries the host's state to a
// client whose clock is far behind or far ahead, and asks the client what a
// frontend asks.

using Ubiquitty::Ticks;
using Ubiquitty::Protocol::Sync::ApplyOutcome;
namespace SyncOutput = Ubiquitty::Modes::SyncOutput;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

constexpr std::uint32_t farBehind = 1'000;
constexpr std::uint32_t farAhead = 50'000'000;

std::unique_ptr<Ubiquitty::Terminal> makeTerminal()
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 24, 80);
    return terminal;
}

// A host whose update is open and stamped at `hostTick`, as the daemon's held
// frame timer stamps it.
std::unique_ptr<Ubiquitty::Terminal> hostWithStampedUpdate(std::uint32_t hostTick)
{
    auto host = makeTerminal();
    Ubiquitty::processBytes(host.get(), "\x1b[?2026hcomposing");
    CHECK_MSG(host->modes.syncOutput, "premise: the host's update is open");
    CHECK_MSG(SyncOutput::shouldWithholdFrame(*host, Ticks(hostTick)), "premise: the host's own decision holds and stamps it");
    return host;
}

// A host awaiting a highlight region from a press at `hostTick`.
std::unique_ptr<Ubiquitty::Terminal> hostAwaitingHilite(std::uint32_t hostTick)
{
    auto host = makeTerminal();
    Ubiquitty::processBytes(host.get(), "\x1b[?1001h");
    CHECK_MSG(host->modes.mouseHilite, "premise: hilite tracking is on");
    Ubiquitty::Mouse::beginHiliteAwait(host.get(), hostTick);
    CHECK_MSG(host->modes.hiliteAwaiting, "premise: the host awaits a region");
    return host;
}

// The client holds the update from its first decision for the budget, on its
// own clock, and then presents.
void checkHeldForTheBudgetFrom(Ubiquitty::Terminal& client, std::uint32_t clientTick, const char* clocks)
{
    CHECK_MSG(SyncOutput::shouldWithholdFrame(client, Ticks(clientTick)), clocks);
    CHECK_MSG(SyncOutput::ticksUntilHoldExpires(client, Ticks(clientTick)) == SyncOutput::maxHoldTicks + 1, clocks);
    CHECK_MSG(SyncOutput::shouldWithholdFrame(client, Ticks(clientTick + SyncOutput::maxHoldTicks)), clocks);
    CHECK_MSG(!SyncOutput::shouldWithholdFrame(client, Ticks(clientTick + SyncOutput::maxHoldTicks + 1)), clocks);
}

// The client awaits the region from its first check for the deadline, on its
// own clock, and then gives it up.
void checkAwaitedForTheDeadlineFrom(Ubiquitty::Terminal& client, std::uint32_t clientTick, const char* clocks)
{
    CHECK_MSG(!Ubiquitty::Mouse::tickHilite(&client, clientTick), clocks);
    CHECK_MSG(client.modes.hiliteAwaiting, clocks);
    CHECK_MSG(!Ubiquitty::Mouse::tickHilite(&client, clientTick + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks - 1), clocks);
    CHECK_MSG(client.modes.hiliteAwaiting, clocks);
    CHECK_MSG(Ubiquitty::Mouse::tickHilite(&client, clientTick + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks), clocks);
    CHECK_MSG(!client.modes.hiliteAwaiting, clocks);
}

// One host connection's screen plane as a client's replica takes it: a
// checkpoint, then row updates.
class Mirror {
public:
    explicit Mirror(Ubiquitty::Terminal& host)
        : m_host(host)
    {
        std::vector<std::uint8_t> body;
        CHECK_MSG(m_publisher.capture(m_host) && m_publisher.checkpoint(m_host, 1, body, m_baseline), "premise: the host checkpoints");
        CHECK_MSG(Sync::applyCheckpoint(*m_client, body, m_replica) == ApplyOutcome::Applied, "premise: the client takes the checkpoint");
    }

    // Carries what the host changed since the last copy as one row update.
    ApplyOutcome update()
    {
        std::vector<std::uint8_t> body;
        if (!m_publisher.capture(m_host) || m_publisher.rowUpdate(m_host, m_baseline, body) != Sync::UpdateOutcome::Encoded)
            return ApplyOutcome::Malformed;
        return Sync::applyRowUpdate(*m_client, body, m_replica);
    }

    Ubiquitty::Terminal& client() { return *m_client; }

private:
    Ubiquitty::Terminal& m_host;
    std::unique_ptr<Ubiquitty::Terminal> m_client = makeTerminal();
    Sync::ScreenPublisher m_publisher;
    Sync::AttachmentBaseline m_baseline;
    Sync::ScreenReplica m_replica;
};

} // namespace

// The client's clock far ahead of the host's: aged against a host stamp, the
// update reads as long outlived on the client's first decision and the client
// presents the frame the program was still composing.
TEST(anUpdateMirroredFromAHostBehindTheClientIsHeldForTheBudget)
{
    auto host = hostWithStampedUpdate(farBehind);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(client->modes.syncOutput);
    checkHeldForTheBudgetFrom(*client, farAhead, "client clock ahead of the host's");
}

// The client's clock far behind the host's: aged against a host stamp, the
// update never ages at all on the client, which holds the remote pane until
// the host itself ends the update.
TEST(anUpdateMirroredFromAHostAheadOfTheClientIsHeldForTheBudget)
{
    auto host = hostWithStampedUpdate(farAhead);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(client->modes.syncOutput);
    checkHeldForTheBudgetFrom(*client, farBehind, "client clock behind the host's");
}

// The client's own stamp survives the host's state arriving again while the
// same update stays open: the budget runs from the client's first decision,
// not from the latest copy.
TEST(aClientsStampSurvivesAnotherCopyOfTheSameOpenUpdate)
{
    auto host = hostWithStampedUpdate(farBehind);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    const std::uint32_t clientTick = 7'000;
    REQUIRE(SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick)));
    Ubiquitty::processBytes(host.get(), " more");
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    CHECK_MSG(SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick + SyncOutput::maxHoldTicks)), "still inside the budget the first decision started");
    CHECK_MSG(!SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick + SyncOutput::maxHoldTicks + 1)), "and out of it one tick later");
}

// An update the client saw close is over: the next one it sees open is
// stamped at the client's next decision, and its stamp of the old one does
// not age it.
TEST(anUpdateAfterOneTheClientSawCloseIsHeldForTheBudget)
{
    auto host = hostWithStampedUpdate(farBehind);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(SyncOutput::shouldWithholdFrame(*client, Ticks(7'000)));
    Ubiquitty::processBytes(host.get(), "\x1b[?2026l");
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(!client->modes.syncOutput);
    Ubiquitty::processBytes(host.get(), "\x1b[?2026hnext frame");
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(client->modes.syncOutput);
    checkHeldForTheBudgetFrom(*client, 7'000 + SyncOutput::maxHoldTicks + 30, "a new update after the one the client saw close");
}

// The wire version's limit: an update the host closed and opened again
// between two copies reads to the client as the one it stamped, so the budget
// runs from the earlier stamp. The hold then ends early, never late.
TEST(anUpdateReopenedBetweenTwoCopiesIsHeldNoLongerThanTheClientsFirstStamp)
{
    auto host = hostWithStampedUpdate(farBehind);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    const std::uint32_t clientTick = 7'000;
    REQUIRE(SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick)));
    Ubiquitty::processBytes(host.get(), "\x1b[?2026l\x1b[?2026hnext frame");
    REQUIRE(host->modes.syncOutput);
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    CHECK_MSG(SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick + SyncOutput::maxHoldTicks)), "held inside the first stamp's budget");
    CHECK_MSG(!SyncOutput::shouldWithholdFrame(*client, Ticks(clientTick + SyncOutput::maxHoldTicks + 1)), "and presented once it is spent");
}

// A peer at this wire version stamps on its own clock, at any offset from the
// client's, and its row updates carry that stamp. Whatever it says, the client
// holds the update from its own first decision for the budget.
TEST(anUpdateFromAPeerStampingAtAnyOffsetIsHeldForTheBudget)
{
    for (const std::uint32_t peerStamp : { 1u, 0x7fff'ffffu, 0x8000'0000u, 0xffff'fff0u }) {
        auto host = makeTerminal();
        Mirror mirror(*host);
        Ubiquitty::processBytes(host.get(), "\x1b[?2026hcomposing");
        REQUIRE(SyncOutput::shouldWithholdFrame(*host, Ticks(peerStamp)));
        REQUIRE(mirror.update() == ApplyOutcome::Applied);
        REQUIRE(mirror.client().modes.syncOutput);
        checkHeldForTheBudgetFrom(mirror.client(), farAhead, "the peer's stamp at a wild offset");
    }
}

// A row update carrying the same open update with a new stamp, as a peer that
// stamps after the client saw the update does, leaves the client's stamp.
TEST(aPeersLaterStampDoesNotMoveTheClientsBudget)
{
    auto host = makeTerminal();
    Mirror mirror(*host);
    Ubiquitty::processBytes(host.get(), "\x1b[?2026hcomposing");
    REQUIRE(mirror.update() == ApplyOutcome::Applied);
    const std::uint32_t clientTick = 7'000;
    REQUIRE(SyncOutput::shouldWithholdFrame(mirror.client(), Ticks(clientTick)));
    REQUIRE(SyncOutput::shouldWithholdFrame(*host, Ticks(farAhead)));
    REQUIRE(mirror.update() == ApplyOutcome::Applied);
    CHECK_MSG(SyncOutput::shouldWithholdFrame(mirror.client(), Ticks(clientTick + SyncOutput::maxHoldTicks)), "held inside the client's budget");
    CHECK_MSG(!SyncOutput::shouldWithholdFrame(mirror.client(), Ticks(clientTick + SyncOutput::maxHoldTicks + 1)), "and presented once it is spent");
}

// The client's clock far ahead of the host's: checked against the host's
// deadline, the await is abandoned on the client's first check, before the
// program could answer.
TEST(anAwaitMirroredFromAHostBehindTheClientRunsItsDeadline)
{
    auto host = hostAwaitingHilite(farBehind);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(client->modes.hiliteAwaiting);
    checkAwaitedForTheDeadlineFrom(*client, farAhead, "client clock ahead of the host's");
}

// The client's clock far behind the host's: checked against the host's
// deadline, the await outlives its bound by the clocks' distance.
TEST(anAwaitMirroredFromAHostAheadOfTheClientRunsItsDeadline)
{
    auto host = hostAwaitingHilite(farAhead);
    auto client = makeTerminal();
    REQUIRE(UbiquittyTest::copyThroughCheckpoint(*host, *client) == ApplyOutcome::Applied);
    REQUIRE(client->modes.hiliteAwaiting);
    checkAwaitedForTheDeadlineFrom(*client, farBehind, "client clock behind the host's");
}

// The client's deadline survives the host's state arriving again while the
// same await stands, and an await the host arms afresh after the client saw
// the old one end gets a deadline of its own.
TEST(aClientsDeadlineLastsAsLongAsTheAwaitItSawStart)
{
    auto host = hostAwaitingHilite(farBehind);
    Mirror mirror(*host);
    REQUIRE(mirror.client().modes.hiliteAwaiting);
    const std::uint32_t clientTick = 7'000;
    REQUIRE(!Ubiquitty::Mouse::tickHilite(&mirror.client(), clientTick));
    Ubiquitty::processBytes(host.get(), "\x1b[4h");
    REQUIRE(mirror.update() == ApplyOutcome::Applied);
    REQUIRE(mirror.client().modes.insertMode);
    CHECK(!Ubiquitty::Mouse::tickHilite(&mirror.client(), clientTick + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks - 1));
    CHECK(Ubiquitty::Mouse::tickHilite(&mirror.client(), clientTick + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks));

    REQUIRE(Ubiquitty::Mouse::tickHilite(host.get(), farBehind + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks));
    REQUIRE(mirror.update() == ApplyOutcome::Applied);
    Ubiquitty::Mouse::beginHiliteAwait(host.get(), farAhead);
    REQUIRE(mirror.update() == ApplyOutcome::Applied);
    REQUIRE(mirror.client().modes.hiliteAwaiting);
    checkAwaitedForTheDeadlineFrom(mirror.client(), clientTick + 100, "an await armed after the client saw the last one end");
}

UBIQUITTY_TEST_MAIN()
