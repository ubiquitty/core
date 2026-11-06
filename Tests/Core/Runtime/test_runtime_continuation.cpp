// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/RemoteInput.h"
#include "PAL/OutputQueue.h"

#include <algorithm>
#include <limits>
#include <memory>

using namespace Ubiquitty::Runtime;
namespace Sync = Ubiquitty::Protocol::Sync;
using Ubiquitty::Input::BracketedPaste;
using Ubiquitty::Input::PasteNormalizer;
using UbiquittyTest::feed;

namespace {

constexpr Sync::ID128 firstAuthority { 1, 10 };
constexpr Sync::ID128 secondAuthority { 2, 10 };
constexpr Sync::ID128 client { 3, 20 };

Sync::PasteInput piece(std::string_view bytes, Sync::PasteEnd end = Sync::PasteEnd::More)
{
    return { .text = { bytes.begin(), bytes.end() }, .end = end };
}

struct FramePair {
    std::unique_ptr<Ubiquitty::Terminal> first = std::make_unique<Ubiquitty::Terminal>();
    std::unique_ptr<Ubiquitty::Terminal> second = std::make_unique<Ubiquitty::Terminal>();
    FrameCoherence source;
    FrameCoherence restored;

    explicit FramePair(CoherenceTuning tuning = {})
        : source(tuning)
        , restored(tuning)
    {
        Ubiquitty::resize(*first, 6, 20);
        Ubiquitty::resize(*second, 6, 20);
        source.noteFrameTaken(*first, 1000);
    }

    void pass(std::string_view bytes, std::uint64_t now, PassEnd end = PassEnd::WouldBlock)
    {
        feed(*first, bytes);
        feed(*second, bytes);
        source.noteDrainPass(*first, now, end);
    }

    bool restore()
    {
        const auto state = source.continuation();
        return state && restored.restoreContinuation(*state);
    }

    FrameAdmission compare(std::uint64_t now, ReadReadiness readiness = ReadReadiness::Quiet)
    {
        const auto ticks = Ubiquitty::Ticks::fromMilliseconds(now / 1000);
        CHECK(source.microsecondsUntilAsk(*first, now, ticks) == restored.microsecondsUntilAsk(*second, now, ticks));
        const auto expected = source.admit(*first, now, ticks, readiness);
        const auto actual = restored.admit(*second, now, ticks, readiness);
        CHECK_EQ(actual.isAdmitted, expected.isAdmitted);
        CHECK(actual.frameClass == expected.frameClass);
        CHECK(source.continuation() == restored.continuation());
        return actual;
    }
};

/// Uses the actual owner-aware queue, with a bounded controllable sink prefix.
struct QueuedSink final : ByteSink {
    Ubiquitty::PAL::OutputQueue queue;
    std::string delivered;
    std::size_t allowance = 0;
    bool shouldRefuse = false;
    int calls = 0;

    static int accept(void* context, std::span<const char> bytes)
    {
        auto& sink = *static_cast<QueuedSink*>(context);
        const auto count = std::min(bytes.size(), sink.allowance);
        sink.delivered.append(bytes.data(), count);
        sink.allowance -= count;
        return static_cast<int>(count);
    }

    Sync::WriteStatus writeAll(Sync::ID128, std::span<const std::uint8_t> bytes) override
    {
        ++calls;
        if (shouldRefuse)
            return Sync::WriteStatus::Refused;
        return queue.submit({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, accept, this) == 0 ? Sync::WriteStatus::Written : Sync::WriteStatus::Refused;
    }

    Sync::WriteStatus writeOwned(Sync::ID128 authority, Sync::ID128, std::span<const std::uint8_t> bytes) override
    {
        ++calls;
        if (shouldRefuse)
            return Sync::WriteStatus::Refused;
        return queue.submitOwned(authority.low, { reinterpret_cast<const char*>(bytes.data()), bytes.size() }, accept, this) == 0 ? Sync::WriteStatus::Written : Sync::WriteStatus::Refused;
    }

    std::size_t pendingBytes(Sync::ID128 authority) const override { return queue.pendingBytes(authority.low); }

    void withdraw(Sync::ID128 authority, std::span<const char> repair) override
    {
        ++calls;
        CHECK(queue.repairPending(authority.low, repair));
    }

    void finish()
    {
        allowance = Ubiquitty::PAL::OutputQueue::maxPending;
        CHECK_EQ(queue.drain(accept, this), 0);
    }
};

} // namespace

TEST(coherenceContinuationPreservesPacingAndTheOriginalDirtyClock)
{
    FramePair pair;
    pair.pass("hello", 2000);
    REQUIRE(pair.restore());
    CHECK(!pair.compare(4000).isAdmitted);
    CHECK(!pair.compare(1000 + refreshIntervalMicroseconds - 1).isAdmitted);
    const auto frame = pair.compare(1000 + refreshIntervalMicroseconds);
    CHECK(frame.isAdmitted && frame.frameClass == FrameClass::Settled);
}

TEST(coherenceContinuationKeepsHoldingAndPersistentBracketPhases)
{
    FramePair pair({ .isPaced = false });
    pair.pass("\x1b[?25lhidden", 10'000);
    REQUIRE(pair.restore());
    CHECK(!pair.compare(10'000 + coherenceDeadlineMicroseconds - 1).isAdmitted);
    const auto expired = pair.compare(10'000 + coherenceDeadlineMicroseconds);
    CHECK(expired.isAdmitted && expired.frameClass == FrameClass::Forced);
    pair.source.noteFrameTaken(*pair.first, 50'000);
    REQUIRE(pair.restore());
    pair.pass("next", 51'000);
    pair.restored.noteDrainPass(*pair.second, 51'000, PassEnd::WouldBlock);
    const auto next = pair.compare(53'000);
    CHECK(next.isAdmitted && next.frameClass == FrameClass::Settled);
}

TEST(coherenceContinuationPreservesBoundaryAndCapacityClasses)
{
    FramePair pair({ .isPaced = false });
    pair.pass("\x1b[?2026hblocked", 10'000);
    REQUIRE(pair.restore());
    CHECK(!pair.compare(11'000).isAdmitted);
    pair.pass("\x1b[?2026l", 12'000);
    pair.restored.noteDrainPass(*pair.second, 12'000, PassEnd::WouldBlock);
    const auto boundary = pair.compare(12'000, ReadReadiness::Pending);
    CHECK(boundary.isAdmitted && boundary.frameClass == FrameClass::Boundary);
    pair.source.noteFrameTaken(*pair.first, 12'000);
    pair.pass("continuous", 13'000, PassEnd::Capacity);
    REQUIRE(pair.restore());
    const auto flood = pair.compare(13'000, ReadReadiness::Pending);
    CHECK(flood.isAdmitted && flood.frameClass == FrameClass::Flood);
}

TEST(coherenceContinuationRejectsInvalidStateWithoutAgingTheReceiver)
{
    FramePair pair;
    pair.pass("held", 2000);
    const auto state = pair.source.continuation();
    REQUIRE(state);
    const auto before = pair.restored.continuation();
    REQUIRE(before);
    const auto refuse = [&](auto change) {
        auto invalid = *state;
        change(invalid);
        CHECK(!pair.restored.restoreContinuation(invalid));
        CHECK(pair.restored.continuation() == before);
    };
    refuse([](auto& value) { value.bracket = static_cast<FrameCoherence::Bracket>(3); });
    refuse([](auto& value) { value.lastPassEnd = static_cast<PassEnd>(4); });
    refuse([](auto& value) { value.frameRows = 0; });
    refuse([](auto& value) { value.hasTakenFrame = false; });
    refuse([](auto& value) { ++value.tuning.quiescenceMicroseconds; });
    refuse([](auto& value) { value.lastFrameMicroseconds = std::numeric_limits<std::uint64_t>::max(); });
    CHECK(pair.source.continuation() == state);
}

TEST(coherenceContinuationPreservesPendingInput)
{
    FramePair pair({ .isPaced = false });
    pair.pass("pending", 100'000, PassEnd::Pending);
    REQUIRE(pair.restore());
    CHECK(!pair.compare(100'000 + readQuiescenceMicroseconds, ReadReadiness::Pending).isAdmitted);
    const auto flood = pair.compare(100'000 + refreshIntervalMicroseconds, ReadReadiness::Pending);
    CHECK(flood.isAdmitted && flood.frameClass == FrameClass::Flood);
}

TEST(coherenceContinuationPreservesTheBudgetOfAnIncompleteCommand)
{
    for (const auto reason : { PassEnd::TimeBudget, PassEnd::Pending }) {
        FramePair pair({ .isPaced = false });
        pair.pass("painted\x1b[48;5;", 100'000, reason);
        REQUIRE(pair.restore());
        const auto admission = pair.compare(100'000, ReadReadiness::Pending);
        CHECK(admission.isAdmitted == (reason == PassEnd::TimeBudget));
        if (admission.isAdmitted)
            CHECK(admission.frameClass == FrameClass::Forced);
    }
}

TEST(normalizerContinuationKeepsEverySplitFenceAndNewlinePair)
{
    for (const auto mode : { BracketedPaste::Disabled, BracketedPaste::Enabled }) {
        for (const std::string& bytes : { std::string("a\r\nb\n\rc"), std::string("\x1b[200~x\x1b[201~"), std::string("\x9b"
                                                                                                                      "200~z") }) {
            for (std::size_t split = 0; split <= bytes.size(); ++split) {
                PasteNormalizer source(mode);
                const auto prefix = source.push(std::string_view(bytes).substr(0, split));
                const auto state = source.continuation();
                REQUIRE(state);
                PasteNormalizer restored(BracketedPaste::Disabled);
                REQUIRE(restored.restoreContinuation(*state));
                CHECK(restored.continuation() == state);
                const auto suffix = std::string_view(bytes).substr(split);
                CHECK_STR_EQ(restored.push(suffix), source.push(suffix));
                CHECK_STR_EQ(restored.finish(), source.finish());
                (void)prefix;
            }
        }
    }
}

TEST(normalizerContinuationRefusesCompletedOrUnrelatedHeldBytes)
{
    PasteNormalizer source(BracketedPaste::Enabled);
    CHECK_STR_EQ(source.push("x\x1b[20"), std::string("x"));
    const auto before = source.continuation();
    REQUIRE(before);
    auto invalid = *before;
    invalid.heldBytes = 6;
    CHECK(!source.restoreContinuation(invalid));
    invalid = *before;
    invalid.held[0] = 'x';
    CHECK(!source.restoreContinuation(invalid));
    invalid = *before;
    invalid.bracketedPaste = static_cast<BracketedPaste>(2);
    CHECK(!source.restoreContinuation(invalid));
    CHECK(source.continuation() == before);
}

TEST(remoteInputContinuationPreservesHeldPasteBytesAndTransactionalRetry)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    terminal->modes.bracketedPaste = true;
    QueuedSink sourceSink;
    RemoteInputEncoder source(*terminal, sourceSink);
    CHECK(source.writeOwned(firstAuthority, client, piece("hello\x1b[20")) == Sync::WriteStatus::Written);
    const auto state = source.continuation();
    REQUIRE(state);
    auto unwritten = sourceSink.queue.copyUnwritten();
    REQUIRE(unwritten);
    QueuedSink restoredSink;
    REQUIRE(restoredSink.queue.restoreUnwritten(*unwritten));
    RemoteInputEncoder restored(*terminal, restoredSink);
    REQUIRE(restored.restoreContinuation(*state));
    CHECK_EQ(restoredSink.calls, 0);
    terminal->modes.bracketedPaste = false;
    const auto tail = piece("1~\r\nend", Sync::PasteEnd::Final);
    restoredSink.shouldRefuse = true;
    CHECK(restored.writeOwned(firstAuthority, client, tail) == Sync::WriteStatus::Refused);
    CHECK(restored.continuation() == state);
    restoredSink.shouldRefuse = false;
    CHECK(restored.writeOwned(firstAuthority, client, tail) == Sync::WriteStatus::Written);
    CHECK(source.writeOwned(firstAuthority, client, tail) == Sync::WriteStatus::Written);
    restoredSink.finish();
    sourceSink.finish();
    CHECK_STR_EQ(restoredSink.delivered, sourceSink.delivered);
    CHECK_STR_EQ(restoredSink.delivered, std::string("\x1b[200~hello\x1b[201\rend\x1b[201~"));
}

TEST(remoteInputContinuationRepairsEveryPartiallyDeliveredDelimiter)
{
    for (const auto end : { Sync::PasteEnd::More, Sync::PasteEnd::Final }) {
        for (std::size_t delivered = 0; delivered <= 15; ++delivered) {
            auto terminal = std::make_unique<Ubiquitty::Terminal>();
            terminal->modes.bracketedPaste = true;
            QueuedSink sourceSink;
            sourceSink.allowance = delivered;
            RemoteInputEncoder source(*terminal, sourceSink);
            CHECK(source.writeOwned(firstAuthority, client, piece("abc", end)) == Sync::WriteStatus::Written);
            CHECK(sourceSink.queue.submit(std::string_view("reply"), QueuedSink::accept, &sourceSink) == 0);
            const auto state = source.continuation();
            REQUIRE(state);
            auto unwritten = sourceSink.queue.copyUnwritten();
            REQUIRE(unwritten);
            QueuedSink restoredSink;
            restoredSink.delivered = sourceSink.delivered;
            REQUIRE(restoredSink.queue.restoreUnwritten(*unwritten));
            RemoteInputEncoder restored(*terminal, restoredSink);
            REQUIRE(restored.restoreContinuation(*state));
            source.withdraw(firstAuthority);
            restored.withdraw(firstAuthority);
            sourceSink.finish();
            restoredSink.finish();
            CHECK_STR_EQ(restoredSink.delivered, sourceSink.delivered);
            CHECK(restored.continuation() == source.continuation());
            const auto reply = restoredSink.delivered.find("reply");
            CHECK(reply != std::string::npos && reply == restoredSink.delivered.rfind("reply"));
        }
    }
}

TEST(remoteInputContinuationKeepsDistinctAttachmentOwnersForOneClient)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    terminal->modes.bracketedPaste = true;
    QueuedSink sink;
    RemoteInputEncoder source(*terminal, sink);
    CHECK(source.writeOwned(firstAuthority, client, piece("first")) == Sync::WriteStatus::Written);
    CHECK(source.writeOwned(secondAuthority, client, piece("second\r")) == Sync::WriteStatus::Written);
    const auto state = source.continuation();
    REQUIRE(state);
    CHECK_EQ(state->pasteCount, 2u);
    auto unwritten = sink.queue.copyUnwritten();
    REQUIRE(unwritten);
    QueuedSink nextSink;
    REQUIRE(nextSink.queue.restoreUnwritten(*unwritten));
    RemoteInputEncoder restored(*terminal, nextSink);
    REQUIRE(restored.restoreContinuation(*state));
    restored.withdraw(firstAuthority);
    CHECK(restored.writeOwned(secondAuthority, client, piece("\nend", Sync::PasteEnd::Final)) == Sync::WriteStatus::Written);
    nextSink.finish();
    CHECK_STR_EQ(nextSink.delivered, std::string("\x1b[200~second\rend\x1b[201~"));
}

TEST(remoteInputContinuationRejectsMalformedOrOccupiedCandidatesWithoutSinkCalls)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    terminal->modes.bracketedPaste = true;
    QueuedSink sourceSink;
    RemoteInputEncoder source(*terminal, sourceSink);
    CHECK(source.writeOwned(firstAuthority, client, piece("data")) == Sync::WriteStatus::Written);
    const auto state = source.continuation();
    REQUIRE(state);
    QueuedSink targetSink;
    RemoteInputEncoder target(*terminal, targetSink);
    const auto before = target.continuation();
    const auto refuse = [&](auto change) {
        auto invalid = *state;
        change(invalid);
        CHECK(!target.restoreContinuation(invalid));
        CHECK(target.continuation() == before);
        CHECK_EQ(targetSink.calls, 0);
    };
    refuse([](auto& value) { value.pasteCount = 65; });
    refuse([](auto& value) { value.pasteCount = 2; value.pastes[1] = value.pastes[0]; });
    refuse([](auto& value) { value.pastes[0].clientInstance = {}; });
    refuse([](auto& value) { value.pastes[0].didClose = true; });
    refuse([](auto& value) { value.pastes[0].hasEnded = true; });
    refuse([](auto& value) { value.pastes[0].lastWriteBytes = 1; });
    refuse([](auto& value) { value.pastes[0].lastWriteBytes = std::numeric_limits<std::uint64_t>::max(); });
    refuse([](auto& value) { value.pastes[0].normalizer.heldBytes = 6; });
    REQUIRE(target.restoreContinuation(*state));
    CHECK(!target.restoreContinuation(*state));
    CHECK(target.continuation() == state);
    CHECK_EQ(targetSink.calls, 0);
}

TEST(remoteInputContinuationRetainsTheExactOwnerLimitAndNextRetirement)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    QueuedSink sourceSink;
    RemoteInputEncoder source(*terminal, sourceSink);
    for (std::uint64_t index = 1; index <= Sync::maxLastWrittenEntries; ++index)
        CHECK(source.writeOwned({ index, 10 }, { index, 20 }, piece("x")) == Sync::WriteStatus::Written);
    const auto state = source.continuation();
    REQUIRE(state);
    CHECK_EQ(state->pasteCount, Sync::maxLastWrittenEntries);
    QueuedSink targetSink;
    auto unwritten = sourceSink.queue.copyUnwritten();
    REQUIRE(unwritten && targetSink.queue.restoreUnwritten(*unwritten));
    RemoteInputEncoder target(*terminal, targetSink);
    REQUIRE(target.restoreContinuation(*state));
    CHECK(target.writeOwned({ 65, 10 }, { 65, 20 }, piece("new")) == Sync::WriteStatus::Refused);
    CHECK_EQ(targetSink.calls, 0);
    target.withdraw(firstAuthority);
    CHECK(target.writeOwned({ 65, 10 }, { 65, 20 }, piece("new")) == Sync::WriteStatus::Written);
    const auto after = target.continuation();
    REQUIRE(after);
    CHECK_EQ(after->pasteCount, Sync::maxLastWrittenEntries);
    CHECK(after->pastes[0].authority == secondAuthority);
    CHECK(after->pastes[63].authority == (Sync::ID128 { 65, 10 }));
}

TEST(coherenceContinuationKeepsExpiryAcrossADeferredFrameAndGeometryChanges)
{
    FramePair pair;
    pair.pass("\x1b[?25lheld", 2000);
    const auto expiryTime = 2000 + coherenceDeadlineMicroseconds;
    const auto expired = pair.source.admit(*pair.first, expiryTime, Ubiquitty::Ticks(), ReadReadiness::Quiet);
    CHECK(expired.isAdmitted && expired.frameClass == FrameClass::Forced);
    // Asking is not taking: an unavailable downstream publisher leaves the
    // original dirty clock and expiry cause intact.
    REQUIRE(pair.restore());
    const auto after = pair.compare(expiryTime + 1000);
    CHECK(after.isAdmitted && after.frameClass == FrameClass::Forced);
    pair.source.noteFrameTaken(*pair.first, expiryTime + 1000);
    Ubiquitty::resize(*pair.first, 8, 30);
    Ubiquitty::resize(*pair.second, 8, 30);
    pair.source.noteHostChange(expiryTime + 2000);
    REQUIRE(pair.restore());
    const auto resized = pair.compare(expiryTime + 1000 + refreshIntervalMicroseconds);
    CHECK(resized.isAdmitted && resized.frameClass == FrameClass::Structural);
}

UBIQUITTY_TEST_MAIN()

TEST(readlinePasteContinuationRetainsModesAcrossRetryAndRestore)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    feed(*terminal, "\033[?2004;2005;2006h");
    QueuedSink sourceSink;
    RemoteInputEncoder source(*terminal, sourceSink);
    REQUIRE(source.writeOwned(firstAuthority, client, piece("a\r")) == Sync::WriteStatus::Written);
    const auto state = source.continuation();
    REQUIRE(state);
    auto pending = sourceSink.queue.copyUnwritten();
    REQUIRE(pending);
    QueuedSink restoredSink;
    REQUIRE(restoredSink.queue.restoreUnwritten(*pending));
    RemoteInputEncoder restored(*terminal, restoredSink);
    REQUIRE(restored.restoreContinuation(*state));
    feed(*terminal, "\033[?2004;2005;2006l");
    const auto tail = piece("\nb", Sync::PasteEnd::Final);
    restoredSink.shouldRefuse = true;
    CHECK(restored.writeOwned(firstAuthority, client, tail) == Sync::WriteStatus::Refused);
    CHECK(restored.continuation() == state);
    restoredSink.shouldRefuse = false;
    REQUIRE(restored.writeOwned(firstAuthority, client, tail) == Sync::WriteStatus::Written);
    restoredSink.finish();
    CHECK_STR_EQ(restoredSink.delivered, "\033[200~\x16"
                                         "a\x16\r\x16\n\x16"
                                         "b\033[201~");
}
