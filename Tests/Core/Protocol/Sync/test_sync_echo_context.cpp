// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_echo_context — what a client's predictor reads of a pane
// (Docs/sync.md §6.7 "Echo context"): an update op that travels when it
// changes, to a client that takes it, outside the digest, naming secure
// entry, the shell's prompt, another writer's input and the characters that
// signal.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

PaneEchoFacts prompting()
{
    return { .isSecureEntry = false, .isAtPrompt = true, .interruptCharacters = { .interrupt = 0x03, .quit = 0x1c, .suspend = 0x1a }, .foregroundGeneration = 1 };
}

/// Notes facts, writes to the pane, and carries the frame to the client.
void publishWith(SyncLink& link, const PaneEchoFacts& facts, std::string_view bytes)
{
    link.source(link.handle()).noteEchoFacts(facts);
    link.write(bytes);
    link.stepUntilQuiet();
}

} // namespace

// The context reaches the client with the frame, and an unchanged context
// does not travel again.
TEST(echoContextTravelsOnlyWhenItChanges)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    publishWith(link, prompting(), "a");
    const std::optional<EchoContext> held = link.client().echoContext(link.handle());
    REQUIRE(held.has_value());
    CHECK(held->flags.contains(EchoFlag::AtPrompt));
    CHECK(!held->flags.contains(EchoFlag::SecureEntry));
    CHECK(held->interruptCharacters.interrupt == std::optional<std::uint8_t> { 0x03 });
    CHECK(held->interruptCharacters.contains(0x1a));
    const std::uint64_t sent = link.host().counters().echoContextsSent;
    publishWith(link, prompting(), "b");
    publishWith(link, prompting(), "c");
    CHECK_EQ(link.host().counters().echoContextsSent, sent);
    PaneEchoFacts password = prompting();
    password.isSecureEntry = true;
    publishWith(link, password, "d");
    CHECK_EQ(link.host().counters().echoContextsSent, sent + 1);
    CHECK(link.client().echoContext(link.handle())->flags.contains(EchoFlag::SecureEntry));
}

// The context is outside the digest: the same update carried with it and
// without it ends with the same state digest, and a replica applying either
// holds the same picture.
TEST(theDigestIgnoresEchoContext)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    UbiquittyTest::feed(terminal, "$ ");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    REQUIRE(publisher.capture(terminal) && publisher.checkpoint(terminal, 1, checkpoint, baseline));
    UbiquittyTest::feed(terminal, "ls");
    REQUIRE(publisher.capture(terminal));
    const EchoContext context { .modesRevision = 4, .foregroundGeneration = 2, .flags = EchoFlags { EchoFlag::AtPrompt }, .interruptCharacters = { .interrupt = 3, .quit = std::nullopt, .suspend = std::nullopt } };
    AttachmentBaseline first = baseline;
    AttachmentBaseline second = baseline;
    std::vector<std::uint8_t> without;
    std::vector<std::uint8_t> with;
    REQUIRE(publisher.rowUpdate(terminal, first, without) == UpdateOutcome::Encoded);
    REQUIRE(publisher.rowUpdate(terminal, second, with, {}, &context) == UpdateOutcome::Encoded);
    CHECK(with.size() > without.size());
    CHECK(std::vector<std::uint8_t>(with.end() - 16, with.end()) == std::vector<std::uint8_t>(without.end() - 16, without.end()));
    Ubiquitty::Terminal replicaTerminal;
    Ubiquitty::resize(replicaTerminal, 24, 80);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(replicaTerminal, checkpoint, replica) == ApplyOutcome::Applied);
    CHECK(applyRowUpdate(replicaTerminal, with, replica) == ApplyOutcome::Applied);
    CHECK(replica.echoContext == std::optional { context });
    CHECK(UbiquittyTest::firstDisagreement(terminal, replicaTerminal).empty());
}

// A client that names no echo context capability is sent none.
TEST(aClientWithoutTheCapabilityIsNeverSentIt)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.capabilities.set(ClientCapability::EchoContext, false);
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane();
    publishWith(link, prompting(), "a");
    CHECK(!link.client().echoContext(link.handle()).has_value());
    CHECK_EQ(link.host().counters().echoContextsSent, 0u);
}

// A checkpoint replaces what the client knew, and the host says the context
// again at once: the update after the checkpoint carries it though the
// program wrote nothing, so the client is without one for a frame.
TEST(aCheckpointIsFollowedByTheEchoContextWithoutOutput)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    publishWith(link, prompting(), "a");
    const std::uint64_t sent = link.host().counters().echoContextsSent;
    REQUIRE(sent >= 1);
    const std::uint64_t checkpoints = link.client().counters().checkpointsApplied;
    link.client().requestCheckpoint(link.handle());
    link.stepUntilQuiet();
    REQUIRE(link.client().counters().checkpointsApplied == checkpoints + 1);
    // The checkpoint replaced what the client knew: the context it held may
    // be from before a reconnect.
    CHECK(!link.client().echoContext(link.handle()).has_value());
    // The update follows within the host's update interval, with nothing
    // written to the pane: two frames at most.
    for (int frames = 0; frames < 2 && !link.client().echoContext(link.handle()); ++frames)
        link.step();
    const std::optional<EchoContext> held = link.client().echoContext(link.handle());
    REQUIRE(held.has_value());
    CHECK(held->flags.contains(EchoFlag::AtPrompt));
    CHECK_EQ(link.host().counters().echoContextsSent, sent + 1);
    CHECK(link.disagreement(link.handle()).empty());
}

// A client is told the context of a pane it has just attached to, before the
// program writes anything.
TEST(aFreshAttachIsToldTheEchoContextWithoutOutput)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    link.stepUntilQuiet();
    CHECK(link.client().echoContext(link.handle()).has_value());
    // Told once: a screen that stands still costs nothing more.
    const std::uint64_t updates = link.host().counters().chainUpdatesSent;
    link.stepUntilQuiet();
    for (int steps = 0; steps < 16; ++steps)
        link.step();
    CHECK_EQ(link.host().counters().chainUpdatesSent, updates);
}

// A client that takes none is owed none: its screen standing still, the host
// sends it nothing after the checkpoint.
TEST(aClientWithoutTheCapabilityCostsNoUpdateAfterACheckpoint)
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.capabilities.set(ClientCapability::EchoContext, false);
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, policy);
    (void)link.addPane();
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().chainUpdatesSent + link.host().counters().supersetUpdatesSent, 0u);
}

// On a link with datagrams, where a still-standing update would go as one, a
// changed context goes on the stream and arrives.
TEST(aChangedEchoContextCrossesOnTheStreamOfADatagramLink)
{
    SyncLink link(1'200, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    publishWith(link, prompting(), "a");
    publishWith(link, prompting(), "b");
    publishWith(link, prompting(), "c");
    REQUIRE(link.host().counters().supersetUpdatesSent >= 1);
    const std::uint64_t datagrams = link.host().counters().datagramsSent;
    PaneEchoFacts password = prompting();
    password.isSecureEntry = true;
    publishWith(link, password, "d");
    const std::optional<EchoContext> held = link.client().echoContext(link.handle());
    REQUIRE(held.has_value());
    CHECK(held->flags.contains(EchoFlag::SecureEntry));
    CHECK_EQ(link.host().counters().datagramsSent, datagrams);
}

// The modes revision moves when the program changes a mode.
TEST(theModesRevisionMovesWithTheModes)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    publishWith(link, prompting(), "a");
    const std::uint32_t before = link.client().echoContext(link.handle())->modesRevision;
    publishWith(link, prompting(), "\x1b[?1h");
    CHECK(link.client().echoContext(link.handle())->modesRevision != before);
}

// Another client's input written while the frame was drawn is foreign to
// this one; its own is not.
TEST(foreignInputNamesAnotherWriter)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection mine(*pane.source, { 41, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection theirs(*pane.source, { 42, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::array<std::uint8_t, 1> key { 'k' };
    REQUIRE(theirs.client.sendRaw(theirs.handle, key).has_value());
    theirs.pump(20'000);
    pane.source->noteEchoFacts(prompting());
    UbiquittyTest::feed(pane.terminal, "k");
    pane.source->publish(FrameClass::Settled, 40'000);
    for (std::uint64_t now = 60'000; now < 200'000; now += 20'000) {
        mine.pump(now);
        theirs.pump(now);
    }
    REQUIRE(mine.client.echoContext(mine.handle).has_value());
    REQUIRE(theirs.client.echoContext(theirs.handle).has_value());
    CHECK(mine.client.echoContext(mine.handle)->flags.contains(EchoFlag::ForeignInput));
    CHECK(!theirs.client.echoContext(theirs.handle)->flags.contains(EchoFlag::ForeignInput));
}

// The shell's prompt is the Terminal's: OSC 133 B to C.
TEST(atPromptFollowsTheShellsMarks)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    UbiquittyTest::feed(terminal, "\x1b]133;A\x07$ \x1b]133;B\x07");
    CHECK(terminal.isReadingCommand);
    UbiquittyTest::feed(terminal, "ls\r\n\x1b]133;C\x07");
    CHECK(!terminal.isReadingCommand);
    UbiquittyTest::feed(terminal, "\x1b]133;D;0\x07\x1b]133;A\x07$ \x1b]633;B\x07");
    CHECK(terminal.isReadingCommand);
    UbiquittyTest::feed(terminal, "\x1b]133;A\x07");
    CHECK(!terminal.isReadingCommand);
}

UBIQUITTY_TEST_MAIN()

TEST(echoContextCarriesChangedAndDisabledEraseCharacters)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    auto facts = prompting();
    for (const std::optional<std::uint8_t> erase : { std::optional<std::uint8_t> { 8 }, std::optional<std::uint8_t> {}, std::optional<std::uint8_t> { 0 }, std::optional<std::uint8_t> { 127 } }) {
        facts.eraseCharacter = erase;
        publishWith(link, facts, "x");
        const auto context = link.client().echoContext(link.handle());
        REQUIRE(context);
        CHECK(context->eraseCharacter == erase);
    }
}
