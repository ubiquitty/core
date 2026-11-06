// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_handover_resume — a client follows a whole-host handover
// (Docs/sync.md §6.13): the pane travels as its encoded record and the
// attachments as their continuation, the client is told `HandingOver`, and it
// resumes on the successor with a diff, its history kept under the new
// instance, the oracle holding on every frame after, and every input event
// written exactly once across the move.

#include "AttachmentLink.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "Platform/StreamDouble.h"
#include "SyncCorpus.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <string>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;

namespace {

constexpr ID128 predecessorInstance { 41, 42 };
constexpr ID128 successorInstance { 51, 52 };

void hostWrites(Ubiquitty::Terminal& terminal, PaneSource& source, std::string_view bytes, std::uint64_t now)
{
    UbiquittyTest::feed(terminal, bytes);
    source.noteTerminalChanged();
    source.publish(FrameClass::Settled, now);
}

/// Discards the replies a stream asks the terminal for; nothing reads them.
void discardReplies(Ubiquitty::Terminal& terminal)
{
    std::array<char, 4096> discard;
    while (Ubiquitty::drainOutput(&terminal, { discard.data(), discard.size() }) > 0) {
    }
}

std::string disagreement(const Ubiquitty::Terminal& host, const ClientAttachment& client)
{
    const auto* picture = client.paneTerminal(paneID);
    return picture ? UbiquittyTest::firstDisagreement(host, *picture) : std::string("the client holds no pane");
}

} // namespace

// The pane and its attachment move to a new instance; the client resumes
// with a diff, sends no history request it does not need, and the oracle
// holds on the frame it resumes at and on every frame after.
TEST(aHandedOverPaneResumesDiffedAndTheOracleHoldsAfter)
{
    Host predecessor(predecessorInstance);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(predecessor, client, now);
    for (int line = 0; line < 60; ++line)
        hostWrites(predecessor.pane.terminal, *predecessor.pane.source, "scrolled line " + std::to_string(line) + "\r\n", now);
    // A margined region, as a full-screen editor keeps.
    hostWrites(predecessor.pane.terminal, *predecessor.pane.source, "\x1b[3;20r\x1b[5;1Hinside the margins\x1b[r\x1b[24;1H$ ", now);
    pumpFor(predecessor, client, now, 200'000);
    REQUIRE(disagreement(predecessor.pane.terminal, client).empty());

    sayHandingOver(predecessor, client, now);
    CHECK(client.wantsHandoverRedial());
    auto successor = handOver(predecessor, successorInstance, now);
    REQUIRE(successor);
    CHECK_MSG(UbiquittyTest::firstDisagreement(predecessor.pane.terminal, *successor->terminal).empty(), "the carried pane differs from the one handed over");
    reconnect(*successor, client, now);

    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.connection() && client.connection()->welcome());
    const auto& welcome = *client.connection()->welcome();
    CHECK(welcome.hostInstanceID == successorInstance);
    CHECK(welcome.predecessorHostInstanceID == std::optional { predecessorInstance });
    REQUIRE(welcome.resumed.size() == 1);
    CHECK(welcome.resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(successor->connection->counters().checkpointsSent, 0u);
    CHECK_EQ(successor->connection->counters().historyBytesSent, 0u);
    CHECK_MSG(disagreement(*successor->terminal, client).empty(), disagreement(*successor->terminal, client).c_str());
    for (int line = 0; line < 30; ++line) {
        hostWrites(*successor->terminal, *successor->source, "after handover " + std::to_string(line) + "\r\n", now);
        pumpFor(*successor, client, now, 100'000);
        const std::string difference = disagreement(*successor->terminal, client);
        CHECK_MSG(difference.empty(), difference.c_str());
    }
    CHECK_EQ(successor->connection->counters().checkpointsSent, 0u);
}

// A password prompt an application asserted with mode 7401 stays its own on
// the successor, where the heuristic may neither clear nor raise it.
TEST(aModeAssertedPasswordPromptSurvivesAHandover)
{
    Host predecessor(predecessorInstance);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(predecessor, client, now);
    hostWrites(predecessor.pane.terminal, *predecessor.pane.source, "\x1b[?7401h", now);
    pumpFor(predecessor, client, now, 200'000);
    REQUIRE(predecessor.pane.terminal.isPasswordPromptAsserted);
    sayHandingOver(predecessor, client, now);
    auto successor = handOver(predecessor, successorInstance, now);
    REQUIRE(successor);
    CHECK(successor->terminal->isPasswordPromptAsserted);
    CHECK(successor->terminal->passwordPromptActive);
}

TEST(theHostReplicatesItsLineDisciplinePasswordVerdict)
{
    Host host(predecessorInstance);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    host.pane.terminal.passwordDetect = true;
    Ubiquitty::TestDoubles::ProcessFactsDouble process;
    process.discipline = Ubiquitty::PAL::LineDiscipline {};
    for (const bool isEchoEnabled : { false, true, false }) {
        process.discipline->isEchoEnabled = isEchoEnabled;
        Ubiquitty::processBytes(&host.pane.terminal, "\r\nPassword:", &process);
        host.pane.source->noteTerminalChanged();
        host.pane.source->publish(FrameClass::Settled, now);
        pumpFor(host, client, now, 200'000);
        const auto* replica = client.paneTerminal(paneID);
        REQUIRE(replica);
        CHECK_EQ(host.pane.terminal.passwordPromptActive, !isEchoEnabled);
        CHECK_EQ(replica->passwordPromptActive, !isEchoEnabled);
        CHECK_MSG(disagreement(host.pane.terminal, client).empty(), "the host's verdict must be part of the replicated screen");
    }
}

// The client's history moves to the successor's key: the lines it held stay
// held, and the successor sends none of them again.
TEST(aHandedOverWelcomeRekeysTheHistoryTheClientHolds)
{
    Host predecessor(predecessorInstance);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(predecessor, client, now);
    for (int line = 0; line < 200; ++line)
        hostWrites(predecessor.pane.terminal, *predecessor.pane.source, "history line " + std::to_string(line) + "\r\n", now);
    pumpFor(predecessor, client, now, 300'000);
    REQUIRE(client.connection());
    const HistoryCache* before = client.connection()->historyCache(1);
    REQUIRE(before);
    CHECK(before->key().hostInstanceID == predecessorInstance);
    const auto heldRanges = before->cachedRanges();
    REQUIRE(!heldRanges.empty());

    sayHandingOver(predecessor, client, now);
    auto successor = handOver(predecessor, successorInstance, now);
    REQUIRE(successor);
    reconnect(*successor, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    const HistoryCache* after = client.connection()->historyCache(1);
    REQUIRE(after);
    CHECK(after->key().hostInstanceID == successorInstance);
    CHECK(after->key().paneID == paneID);
    CHECK(after->key().epoch == before->key().epoch);
    CHECK(after->cachedRanges() == heldRanges);
    CHECK_EQ(successor->connection->counters().historyBytesSent, 0u);
}

// An event the predecessor wrote is not written again; one it never read,
// and one typed while the client was between hosts, are written once by the
// successor, in order.
TEST(inputSentAcrossAHandoverIsWrittenOnce)
{
    Host predecessor(predecessorInstance);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(predecessor, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(predecessor, client, now, 50'000);
    CHECK_STR_EQ(predecessor.pane.writer.bytes(), std::string("a"));
    // `b` reaches the host's socket after it stopped reading for the handover.
    REQUIRE(client.send(paneID, raw("b"), now).has_value());
    pumpFor(predecessor, client, now, 20'000, { .shouldCarryDown = true, .shouldCarryUp = false });
    sayHandingOver(predecessor, client, now);
    REQUIRE(client.send(paneID, raw("c"), now).has_value());
    auto successor = handOver(predecessor, successorInstance, now);
    REQUIRE(successor);
    reconnect(*successor, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].inputWritten == std::optional<std::uint64_t> { 1 });
    CHECK_STR_EQ(predecessor.pane.writer.bytes(), std::string("a"));
    CHECK_STR_EQ(successor->writer.bytes(), std::string("bc"));
    CHECK_EQ(successor->connection->counters().inputDuplicates, 0u);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK(client.inDoubt(paneID).empty());
}

// Many events, a handover between any two of them: each is written once.
TEST(manyEventsAcrossAHandoverAreEachWrittenOnce)
{
    for (int cut = 1; cut < 40; cut += 7) {
        Host predecessor(predecessorInstance);
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
        std::uint64_t now = 1;
        attachFresh(predecessor, client, now);
        std::string expected;
        for (int event = 0; event < cut; ++event) {
            const std::string key(1, static_cast<char>('A' + event % 26));
            expected += key;
            REQUIRE(client.send(paneID, raw(key), now).has_value());
            // Every other event's receipt is lost on the way back.
            pumpFor(predecessor, client, now, 1'000, { .shouldCarryDown = event % 2 == 0, .shouldCarryUp = true });
        }
        sayHandingOver(predecessor, client, now);
        for (int event = cut; event < 40; ++event) {
            const std::string key(1, static_cast<char>('a' + event % 26));
            expected += key;
            REQUIRE(client.send(paneID, raw(key), now).has_value());
        }
        auto successor = handOver(predecessor, successorInstance, now);
        REQUIRE(successor);
        reconnect(*successor, client, now);
        const std::string written = predecessor.pane.writer.bytes() + successor->writer.bytes();
        CHECK_MSG(written == expected, ("cut " + std::to_string(cut) + " wrote " + written).c_str());
        CHECK_EQ(client.pendingCount(paneID), 0u);
    }
}

// Every recorded stream, handed over at the first ground past its middle,
// keeps the oracle on the successor through to its end.
TEST(everyCorpusEntrySurvivesAHandoverAtGround)
{
    std::string error;
    const auto entries = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    REQUIRE_MSG(entries && !entries->empty(), error.c_str());
    for (const auto& entry : *entries) {
        Host predecessor(predecessorInstance);
        Ubiquitty::resize(predecessor.pane.terminal, entry.rows, entry.columns);
        predecessor.pane.source->noteTerminalChanged();
        predecessor.pane.source->publish(FrameClass::Structural, 1);
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
        std::uint64_t now = 1;
        attachFresh(predecessor, client, now);
        const auto reads = entry.ptyReads();
        std::unique_ptr<SuccessorHost> successor;
        for (std::size_t index = 0; index < reads.size(); ++index) {
            Ubiquitty::Terminal& terminal = successor ? *successor->terminal : predecessor.pane.terminal;
            PaneSource& source = successor ? *successor->source : *predecessor.pane.source;
            UbiquittyTest::feed(terminal, reads[index].bytes);
            discardReplies(terminal);
            source.noteTerminalChanged();
            source.publish(FrameClass::Settled, now);
            if (successor)
                pumpFor(*successor, client, now, 2'000);
            else
                pumpFor(predecessor, client, now, 2'000);
            if (!successor && index >= reads.size() / 2 && terminalHandoverBlockers(terminal).isEmpty()) {
                sayHandingOver(predecessor, client, now);
                successor = handOver(predecessor, successorInstance, now);
                REQUIRE_MSG(successor, entry.name.c_str());
                reconnect(*successor, client, now);
                REQUIRE_MSG(client.connection() && client.connection()->welcome(), entry.name.c_str());
                const auto& resumed = client.connection()->welcome()->resumed;
                CHECK_MSG(resumed.size() == 1 && resumed[0].outcome == ResumeOutcome::Diffed, entry.name.c_str());
            }
        }
        CHECK_MSG(successor != nullptr, (entry.name + " never reached ground past its middle").c_str());
        if (!successor)
            continue;
        pumpFor(*successor, client, now, 200'000);
        const std::string difference = disagreement(*successor->terminal, client);
        CHECK_MSG(difference.empty(), (entry.name + ": " + difference).c_str());
        CHECK_MSG(successor->connection->counters().checkpointsSent == 0, (entry.name + " needed a checkpoint after the handover").c_str());
    }
}

UBIQUITTY_TEST_MAIN()
