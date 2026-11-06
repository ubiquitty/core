// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sound is the one grid-escaping effect that reaches a person who is not
// looking at the screen, and the people beside them, so it is gated like the
// clipboard and the window rather than being the one host escape a byte stream
// could reach for free. These cover the gate, and the slot bookkeeping a
// program needs to keep a chime across a session it reattached to.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Sound.h"
#include "PAL/TerminalClients.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

/// Answers one decision for the sound capability and allows everything else,
/// which is the shape a real policy has: it refuses a named effect rather than
/// every question, and the protocol acknowledgement below is not the effect.
class FixedPolicy final : public CapabilityPolicy {
public:
    explicit FixedPolicy(CapabilityDecision decision)
        : m_decision(decision)
    {
    }

    CapabilityDecision check(Capability capability, CapabilityOrigin, std::string_view) override
    {
        m_asked.push_back(capability);
        return capability == Capability::PlaySound ? m_decision : CapabilityDecision::Allow;
    }

    [[nodiscard]] bool wasAsked(Capability capability) const
    {
        for (const Capability seen : m_asked)
            if (seen == capability)
                return true;
        return false;
    }

    std::vector<Capability> m_asked;

private:
    CapabilityDecision m_decision;
};

// A sixty-byte mono 44.1 kHz WAV: eight frames, enough to decode and land in a
// slot without making the test a fixture file.
constexpr std::string_view tinyWavBase64 = "UklGRjQAAABXQVZFZm10IBAAAAABAAEARKwAAIhYAQACABAAZGF0YRAAAAAAAEAfwOCgD2Dw6AMY/AAA";

std::string download(std::string_view parameters)
{
    return std::string("\033P") + std::string(parameters) + "+s" + std::string(tinyWavBase64) + "\033\\";
}

int queuedSounds(Ubiquitty::Terminal& t)
{
    return t.pendingSound ? t.pendingSound->count : 0;
}

}

/// A denied program makes no sound, and nothing reaches the windows attached to
/// the session either — the request is dropped before it is queued for them.
TEST(testADeniedProgramCannotPlay)
{
    // The policy outlives the terminal: teardown still asks the gate, so one
    // declared after it would be gone by the time the question arrives.
    FixedPolicy deny(CapabilityDecision::Deny);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.capabilityPolicy = &deny;

    feed(t, "\033[7;8;25,~");
    CHECK(deny.wasAsked(Capability::PlaySound));
    CHECK(queuedSounds(t) == 0);
}

/// The same request lands when the policy allows it, so the gate is what
/// decided and not a parse that broke.
TEST(testAnAllowedProgramPlays)
{
    // The policy outlives the terminal: teardown still asks the gate, so one
    // declared after it would be gone by the time the question arrives.
    FixedPolicy allow(CapabilityDecision::Allow);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.capabilityPolicy = &allow;

    feed(t, "\033[7;8;25,~");
    CHECK(allow.wasAsked(Capability::PlaySound));
    CHECK(queuedSounds(t) == 1);
}

/// Silence is not a privilege. A program whose grant lapsed mid-phrase must
/// still be able to stop the channel it started, so volume 0 is ungated.
TEST(testStoppingSoundIsNotGated)
{
    // The policy outlives the terminal: teardown still asks the gate, so one
    // declared after it would be gone by the time the question arrives.
    FixedPolicy deny(CapabilityDecision::Deny);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.capabilityPolicy = &deny;

    feed(t, "\033[0;0;0,~"); // volume 0 on channel 0: stop everything
    CHECK(queuedSounds(t) == 1);
}

/// Prompt parks rather than allows: Core cannot block for an answer, so the
/// pass makes no sound and the frontend re-drives the sequence after a grant.
TEST(testAPromptedProgramMakesNoSoundThisPass)
{
    // The policy outlives the terminal: teardown still asks the gate, so one
    // declared after it would be gone by the time the question arrives.
    FixedPolicy prompt(CapabilityDecision::Prompt);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.capabilityPolicy = &prompt;

    feed(t, "\033[7;8;25,~");
    CHECK(queuedSounds(t) == 0);
}

/// A download is the capability's other half — the bytes a later request makes
/// audible — so it asks the same question.
TEST(testADeniedProgramCannotDownloadASample)
{
    // The policy outlives the terminal: teardown still asks the gate, so one
    // declared after it would be gone by the time the question arrives.
    FixedPolicy deny(CapabilityDecision::Deny);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.capabilityPolicy = &deny;

    feed(t, download("0;0;4660"));
    CHECK(deny.wasAsked(Capability::PlaySound));
    CHECK(!t.sampleBank || t.sampleBank->loadedMask() == 0u);
    // Refusal is answered rather than silent: the caller asked which slot it
    // got, and slot 0 is "none" instead of a wait that never ends.
    char outputBuffer[64];
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P0;0;4660+s\033\\") != std::string_view::npos);
}

/// Slot 0 asks the bank to choose, because the numbers are global to the
/// terminal and two programs picking their own overwrote each other silently.
TEST(testADownloadCanAskTheBankForASlot)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];

    feed(t, download("0;0;4660"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P1;0;4660+s\033\\") != std::string_view::npos);
    CHECK(t.sampleBank->slotForTag(4660) == 1);

    // A second program asking gets a different slot rather than the first one's.
    feed(t, download("0;0;1"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P2;0;1+s\033\\") != std::string_view::npos);
    CHECK(t.sampleBank->slotForTag(4660) == 1);
    CHECK(t.sampleBank->slotForTag(1) == 2);
}

/// Re-uploading a tag returns to the slot it already holds, so a program that
/// could not tell whether its chime survived does not spend a second slot
/// finding out.
TEST(testATaggedReuploadKeepsItsSlot)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];

    feed(t, download("0;0;77"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(t.sampleBank->slotForTag(77) == 1);

    feed(t, download("0;0;77"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P1;0;77+s\033\\") != std::string_view::npos);
    CHECK(t.sampleBank->loadedMask() == 1u); // still one slot spent
}

/// An empty payload gives a slot back, the way a zero-length DECUDK value
/// clears a key.
TEST(testAnEmptyDownloadClearsTheSlot)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];

    feed(t, download("3;0;0"));
    CHECK(t.sampleBank->loadedMask() == (1u << 2));

    feed(t, "\033P3;0;0+s\033\\");
    CHECK(t.sampleBank->loadedMask() == 0u);
    (void)drain(t, outputBuffer);

    // By tag, for a program that never learned which slot it was given.
    feed(t, download("0;0;9"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(t.sampleBank->slotForTag(9) == 1);
    feed(t, "\033P0;0;9+s\033\\");
    CHECK(t.sampleBank->slotForTag(9) == 0);
    CHECK(t.sampleBank->loadedMask() == 0u);
}

/// A full bank refuses rather than evicting a sample another program is still
/// playing.
TEST(testAFullBankRefusesRatherThanEvicting)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    for (int i = 0; i < Ubiquitty::VT::sampleSlotCount; ++i) {
        feed(t, download("0;0;0"));
        (void)drain(t, outputBuffer);
    }
    CHECK(t.sampleBank->firstFreeSlot() == 0);

    feed(t, download("0;0;4242"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P0;0;4242+s\033\\") != std::string_view::npos);
}

/// The whole reconnect story, in the order a program lives it: ask, upload,
/// return to a session that outlived the program, find the bank empty, upload
/// again. This is the case the tag exists for — a build finishing in a session
/// the program has reattached to.
TEST(testAProgramKeepsItsChimeAcrossAReattach)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[96];

    // First run: nothing of ours is here, so upload and remember the slot.
    feed(t, "\033[?7411;4660n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033[?7411;4660;0n") != std::string_view::npos);
    feed(t, download("0;0;4660"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P1;0;4660+s\033\\") != std::string_view::npos);

    // Still there while the session lives: the build finishes, we play it.
    feed(t, "\033[?7411;4660n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033[?7411;4660;1n") != std::string_view::npos);

    // RIS is what a fresh shell in that session does, and it empties the bank
    // without telling anyone — the silence this whole mechanism is against.
    feed(t, "\033c");
    feed(t, "\033[?7411;4660n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033[?7411;4660;0n") != std::string_view::npos);

    // So the program uploads again, and is told where it landed.
    feed(t, download("0;0;4660"));
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer).find("\033P1;0;4660+s\033\\") != std::string_view::npos);
    CHECK(t.sampleBank->slotForTag(4660) == 1);
}

/// A caller that names its own slot and no tag sees exactly the bytes it always
/// saw: the acknowledgement answers a question, and it did not ask one.
TEST(testAnUnaskedDownloadIsNotAcknowledged)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    feed(t, download("5;0"));
    CHECK(t.sampleBank->loadedMask() == (1u << 4));
    CHECK(drain(t, outputBuffer) == 0);
}

TEST(testParsedSampleSurvivesABankOverwriteBeforePublication)
{
    FixedPolicy allow(CapabilityDecision::Allow);
    Terminal terminal;
    hardReset(terminal);
    terminal.capabilityPolicy = &allow;
    auto& bank = ensureSampleBank(terminal);
    bank.slots[0] = { -4, 8, 16 };
    bank.serial = 11;
    feed(terminal, "\033[5:0:6;8;1,~");
    REQUIRE(terminal.pendingSound && terminal.pendingSound->front());
    bank.slots[0] = { 99 };
    bank.serial = 12;
    const auto* pending = terminal.pendingSound->front();
    CHECK_EQ(pending->sampleSerial, 11u);
    CHECK_EQ(pending->request.sampleFrames, 3u);
    CHECK_EQ(pending->request.samplePCM[0], -4);
    CHECK_EQ(pending->request.samplePCM[2], 16);
}

TEST(testRISInTheSameReadPreservesAcceptedSoundThenAnOrderedStop)
{
    FixedPolicy allow(CapabilityDecision::Allow);
    Terminal terminal;
    hardReset(terminal);
    terminal.capabilityPolicy = &allow;
    auto& bank = ensureSampleBank(terminal);
    bank.slots[0] = { -4, 8, 16 };
    bank.serial = 11;
    feed(terminal, "\033[5:0:6;8;1,~\033c");
    CHECK(!terminal.sampleBank);
    REQUIRE(terminal.pendingSound && terminal.pendingSound->front());
    const auto* first = terminal.pendingSound->front();
    CHECK_EQ(first->sampleSerial, 11u);
    CHECK_EQ(first->request.sampleFrames, 3u);
    CHECK_EQ(first->request.samplePCM[0], -4);
    terminal.pendingSound->pop();
    REQUIRE(terminal.pendingSound->front());
    CHECK_EQ(terminal.pendingSound->front()->request.volume, 0u);
    CHECK_EQ(terminal.pendingSound->front()->request.channel, 0u);
    terminal.pendingSound->pop();
    CHECK(!terminal.pendingSound->front());
    CHECK_EQ(terminal.pendingSound->pcmBytes, 0u);
}

TEST(testFullQueueAndRepeatedRISRetainEverySoundAndOneStop)
{
    FixedPolicy allow(CapabilityDecision::Allow);
    Terminal terminal;
    hardReset(terminal);
    terminal.capabilityPolicy = &allow;
    auto& bank = ensureSampleBank(terminal);
    bank.slots[0] = { 42 };
    for (uint32_t serial = 1; serial <= VT::PendingSound::depth; ++serial) {
        bank.serial = serial;
        feed(terminal, "\033[5:0:6;8;1,~");
    }
    feed(terminal, "\033c\033c");
    REQUIRE(terminal.pendingSound && terminal.pendingSound->front());
    auto& pending = *terminal.pendingSound;
    // No room for a subsequent operation before the deferred stop is ordered.
    feed(terminal, "\033[7;8;25,~");
    CHECK_EQ(pending.count, VT::PendingSound::depth);
    for (uint32_t serial = 1; serial <= VT::PendingSound::depth; ++serial) {
        REQUIRE(pending.front());
        CHECK_EQ(pending.front()->sampleSerial, serial);
        CHECK_EQ(pending.front()->request.samplePCM[0], 42);
        pending.pop();
    }
    REQUIRE(pending.front());
    CHECK_EQ(pending.front()->request.volume, 0u);
    CHECK_EQ(pending.front()->request.channel, 0u);
    pending.pop();
    CHECK(!pending.front());
    CHECK_EQ(pending.pcmBytes, 0u);
}

TEST(testPostResetSoundFollowsTheDeferredResetStop)
{
    FixedPolicy allow(CapabilityDecision::Allow);
    Terminal terminal;
    hardReset(terminal);
    terminal.capabilityPolicy = &allow;
    for (int index = 0; index < VT::PendingSound::depth; ++index)
        feed(terminal, "\033[5;8;25,~");
    feed(terminal, "\033c");
    REQUIRE(terminal.pendingSound && terminal.pendingSound->front());
    auto& pending = *terminal.pendingSound;
    pending.pop();
    // One vacant slot is reserved for the reset stop, not a later sound.
    feed(terminal, "\033[7;8;25,~");
    CHECK_EQ(pending.count, VT::PendingSound::depth - 1);
    pending.pop();
    feed(terminal, "\033[7;8;25,~");
    for (int index = 0; index < VT::PendingSound::depth - 2; ++index) {
        REQUIRE(pending.front());
        CHECK_EQ(pending.front()->request.volume, 5u);
        pending.pop();
    }
    REQUIRE(pending.front());
    CHECK_EQ(pending.front()->request.volume, 0u);
    pending.pop();
    REQUIRE(pending.front());
    CHECK_EQ(pending.front()->request.volume, 7u);
    pending.pop();
    CHECK(!pending.front());
}

UBIQUITTY_TEST_MAIN()
