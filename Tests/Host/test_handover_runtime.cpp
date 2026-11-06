// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "PAL/OutputQueue.h"
#include "Platform/Host/HandoverRuntime.h"

#include <algorithm>
#include <limits>
#include <memory>

using namespace Ubiquitty;
using namespace Ubiquitty::Runtime;
namespace Sync = Ubiquitty::Protocol::Sync;
using UbiquittyTest::feed;

namespace {

constexpr Host::HandoverRuntimeLimits limits { 3373, 64 };
constexpr Sync::ID128 authority { 11, 31 };
constexpr Sync::ID128 client { 13, 37 };

Host::HandoverRuntime example()
{
    Host::HandoverRuntime state;
    auto& frame = state.coherence;
    frame.tuning = { 34567, 2345, 12345, true, false };
    frame.dirtySinceMicroseconds = 0x1020304050607080;
    frame.lastPassEndMicroseconds = 0x2131415161718191;
    frame.lastFrameMicroseconds = 0x32425262728292a2;
    frame.bracketOpenedMicroseconds = 0x435363738393a3b3;
    frame.bracketHideCount = 0x12345678;
    frame.cursorHideCountAtLastFrame = 0x23456789;
    frame.syncCloseCountAtLastFrame = 0x3456789a;
    frame.syncCloseCountAtLastPass = 0x456789ab;
    frame.frameRows = 27;
    frame.frameColumns = 113;
    frame.bracket = FrameCoherence::Bracket::Persistent;
    frame.lastPassEnd = PassEnd::Capacity;
    frame.isDirty = true;
    frame.hasTakenFrame = true;
    frame.hasPassEnded = true;
    frame.wasCursorVisibleAtLastFrame = false;
    frame.wasComposingAtLastPass = true;
    frame.didExpireSinceLastFrame = true;
    state.input.pasteCount = 2;
    auto& paste = state.input.pastes[0];
    paste.clientInstance = client;
    paste.authority = authority;
    paste.normalizer.bracketedPaste = Input::BracketedPaste::Enabled;
    paste.normalizer.options = { .shouldQuoteCharacters = true, .shouldPreserveNewlines = true };
    paste.normalizer.held = { '\x1b', '[', '2', '0', '1' };
    paste.normalizer.heldBytes = 5;
    paste.lastWriteBytes = 17;
    paste.isBracketOpen = true;
    paste.didOpen = true;
    auto& ended = state.input.pastes[1];
    ended.clientInstance = client;
    ended.authority = { 17, 41 };
    ended.normalizer.bracketedPaste = Input::BracketedPaste::Enabled;
    ended.lastWriteBytes = 6;
    ended.isBracketOpen = true;
    ended.didClose = true;
    ended.hasEnded = true;
    return state;
}

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}

Sync::PasteInput piece(std::string_view bytes, Sync::PasteEnd end = Sync::PasteEnd::More)
{
    return { .text = { bytes.begin(), bytes.end() }, .end = end };
}

/// Actual accepted-byte ownership, independent of the runtime metadata codec.
struct QueuedSink final : ByteSink {
    PAL::OutputQueue queue;
    std::string delivered;
    std::size_t allowance = 0;
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
        return queue.submit({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, accept, this) == 0 ? Sync::WriteStatus::Written : Sync::WriteStatus::Refused;
    }
    Sync::WriteStatus writeOwned(Sync::ID128 owner, Sync::ID128, std::span<const std::uint8_t> bytes) override
    {
        ++calls;
        return queue.submitOwned(owner.low, { reinterpret_cast<const char*>(bytes.data()), bytes.size() }, accept, this) == 0 ? Sync::WriteStatus::Written : Sync::WriteStatus::Refused;
    }
    std::size_t pendingBytes(Sync::ID128 owner) const override { return queue.pendingBytes(owner.low); }
    void withdraw(Sync::ID128 owner, std::span<const char> repair) override
    {
        ++calls;
        CHECK(queue.repairPending(owner.low, repair));
    }
    void finish()
    {
        allowance = PAL::OutputQueue::maxPending;
        CHECK_EQ(queue.drain(accept, this), 0);
    }
};

#if defined(__clang__) && __has_builtin(__builtin_dump_struct)
struct MemberNames {
    std::vector<std::string_view> names;
    template<typename... Arguments> void operator()(const char*, Arguments... arguments) { take(arguments...); }
    void take(...) { }
    template<typename Value> void take(const char* indent, const char*, const char* name, Value)
    {
        if (std::string_view(indent) == "  ")
            names.emplace_back(name);
    }
    void take(const char* indent, const char*, const char* name)
    {
        if (std::string_view(indent) == "  ")
            names.emplace_back(name);
    }
};

template<typename T>
void checkMembers(std::initializer_list<std::string_view> expected)
{
    T value {};
    MemberNames observed;
    __builtin_dump_struct(&value, observed);
    CHECK(std::equal(observed.names.begin(), observed.names.end(), expected.begin(), expected.end()));
}
#endif

} // namespace

TEST(runtimeSectionNamesEveryContinuationMemberIncludingPaddingHoles)
{
#if defined(__clang__) && __has_builtin(__builtin_dump_struct)
    checkMembers<CoherenceTuning>({ "deadlineMicroseconds", "quiescenceMicroseconds", "frameIntervalMicroseconds", "isPaced", "isPersistentBracketEnabled" });
    checkMembers<FrameCoherence::Continuation>({ "tuning", "dirtySinceMicroseconds", "lastPassEndMicroseconds", "lastFrameMicroseconds", "bracketOpenedMicroseconds", "bracketHideCount", "cursorHideCountAtLastFrame", "syncCloseCountAtLastFrame", "syncCloseCountAtLastPass", "frameRows", "frameColumns", "bracket", "lastPassEnd", "isDirty", "hasTakenFrame", "hasPassEnded", "wasCursorVisibleAtLastFrame", "wasComposingAtLastPass", "didExpireSinceLastFrame" });
    checkMembers<RemoteInputEncoder::Continuation>({ "pastes", "pasteCount" });
    checkMembers<RemoteInputEncoder::Continuation::Paste>({ "clientInstance", "authority", "normalizer", "lastWriteBytes", "isBracketOpen", "didOpen", "didClose", "hasEnded" });
    checkMembers<Input::PasteNormalizer::Continuation>({ "bracketedPaste", "options", "held", "heldBytes" });
#else
    std::fprintf(stderr, "SKIPPED member enumeration: compiler lacks __builtin_dump_struct.\n");
#endif
}

TEST(runtimeSectionPreservesEveryClockCounterFlagAndPasteOwner)
{
    const auto state = example();
    const auto bytes = Host::encodeHandoverRuntime(state, limits);
    REQUIRE(bytes);
    CHECK_EQ(bytes->size(), 211u);
    const auto decoded = Host::decodeHandoverRuntime(*bytes, limits);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(Host::encodeHandoverRuntime(*decoded, limits) == bytes);
    CHECK(state == example());
    CHECK(decoded->input.pastes[1].hasEnded && decoded->input.pastes[1].didClose);
}

TEST(runtimeSectionPreservesEveryPendingPassReason)
{
    for (const auto reason : { PassEnd::TimeBudget, PassEnd::Pending }) {
        auto state = example();
        state.coherence.lastPassEnd = reason;
        const auto bytes = Host::encodeHandoverRuntime(state, limits);
        REQUIRE(bytes);
        const auto decoded = Host::decodeHandoverRuntime(*bytes, limits);
        REQUIRE(decoded);
        CHECK(*decoded == state);
    }
}

TEST(runtimeSectionPinsVersionAndFieldBytePositions)
{
    Host::HandoverRuntime state;
    state.coherence.tuning = { 0, 0, 0, false, false };
    const auto empty = Host::encodeHandoverRuntime(state, limits);
    REQUIRE(empty);
    REQUIRE(empty->size() == 109);
    std::array<std::uint8_t, 93> expected {};
    expected[0] = 2;
    expected[89] = 1; // wasCursorVisibleAtLastFrame, not a serialized C++ bool.
    CHECK(std::equal(expected.begin(), expected.end(), empty->begin()));
    const auto bytes = Host::encodeHandoverRuntime(example(), limits);
    REQUIRE(bytes);
    constexpr std::array<std::uint8_t, 8> dirty { 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10 };
    CHECK(std::equal(dirty.begin(), dirty.end(), bytes->begin() + 28));
    CHECK_EQ((*bytes)[76], 27u);
    CHECK_EQ((*bytes)[80], 113u);
    CHECK_EQ((*bytes)[84], 2u);
    CHECK_EQ((*bytes)[85], 1u);
    CHECK_EQ((*bytes)[92], 2u);
    CHECK_EQ((*bytes)[93], 13u); // First client's low word.
    CHECK_EQ((*bytes)[109], 11u); // First authority's low word.
    CHECK_EQ((*bytes)[125], 7u); // Brackets, quoting and literal newlines.
    CHECK_EQ((*bytes)[126], 5u); // Held length, then all five exact bytes.
    CHECK((*bytes)[127] == 0x1b && (*bytes)[131] == '1');
    CHECK_EQ((*bytes)[132], 17u);
    CHECK((*bytes)[140] == 1 && (*bytes)[141] == 1 && (*bytes)[142] == 0 && (*bytes)[143] == 0);
}

TEST(runtimeSectionChecksIndependentByteAndPasteLimits)
{
    Host::HandoverRuntime state;
    state.input.pasteCount = 64;
    for (std::size_t index = 0; index < 64; ++index)
        state.input.pastes[index].clientInstance = { index + 1, 7 };
    auto encoded = Host::encodeHandoverRuntime(state, limits);
    REQUIRE(encoded);
    CHECK_EQ(encoded->size(), limits.maxSectionBytes);
    REQUIRE(Host::decodeHandoverRuntime(*encoded, limits));
    auto fewer = limits;
    --fewer.maxPastes;
    CHECK(Host::encodeHandoverRuntime(state, fewer).error() == Host::HandoverRuntimeError::OverCapacity);
    CHECK(Host::decodeHandoverRuntime(*encoded, fewer).error() == Host::HandoverRuntimeError::OverCapacity);
    auto shorter = limits;
    --shorter.maxSectionBytes;
    CHECK(Host::encodeHandoverRuntime(state, shorter).error() == Host::HandoverRuntimeError::OverCapacity);
    CHECK(Host::decodeHandoverRuntime(*encoded, shorter).error() == Host::HandoverRuntimeError::OverCapacity);
    state.input.pasteCount = 65;
    CHECK(Host::encodeHandoverRuntime(state, limits).error() == Host::HandoverRuntimeError::OverCapacity);
    (*encoded)[92] = 65;
    CHECK(Host::decodeHandoverRuntime(*encoded, limits).error() == Host::HandoverRuntimeError::OverCapacity);
}

TEST(runtimeSectionRefusesEveryTruncationTrailingByteVersionAndCorruption)
{
    const auto encoded = Host::encodeHandoverRuntime(example(), limits);
    REQUIRE(encoded);
    for (std::size_t size = 0; size < encoded->size(); ++size)
        CHECK(!Host::decodeHandoverRuntime(std::span(*encoded).first(size), limits));
    auto invalid = *encoded;
    invalid.push_back(0);
    CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::Malformed);
    invalid = *encoded;
    invalid[0] = 3;
    CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::NewerVersion);
    invalid[0] = 0;
    CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::Malformed);
    invalid = *encoded;
    invalid[28] ^= 1;
    CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::DigestMismatch);
}

TEST(runtimeSectionRejectsNonBooleanBitsAndMalformedMetadataWithValidDigest)
{
    const auto encoded = Host::encodeHandoverRuntime(example(), limits);
    REQUIRE(encoded);
    for (const std::size_t offset : { 26, 27, 86, 87, 88, 89, 90, 91, 140, 141, 142, 143 }) {
        auto invalid = *encoded;
        invalid[offset] = 2;
        redigest(invalid);
        CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::Malformed);
    }
    const auto refuse = [&](std::size_t offset, std::uint8_t value) {
        auto invalid = *encoded;
        invalid[offset] = value;
        redigest(invalid);
        const auto decoded = Host::decodeHandoverRuntime(invalid, limits);
        CHECK(!decoded);
        if (!decoded)
            CHECK(decoded.error() == Host::HandoverRuntimeError::Malformed);
    };
    refuse(84, 3); // Bracket.
    refuse(85, 4); // First invalid PassEnd.
    refuse(85, 0xff);
    refuse(76, 0); // Taken frame with zero rows.
    refuse(125, 8); // Unknown paste option.
    refuse(126, 6); // Normalizer length.
    refuse(127, 'x'); // Not a held newline or incomplete fence.
    refuse(132, 1); // Last write cannot contain its six-byte opener.
    auto invalid = *encoded;
    std::copy_n(invalid.begin() + 93, 32, invalid.begin() + 144);
    redigest(invalid);
    CHECK(Host::decodeHandoverRuntime(invalid, limits).error() == Host::HandoverRuntimeError::Malformed);
}

TEST(runtimeSectionUsesTheOwnersSemanticValidatorsBeforeEncoding)
{
    const auto valid = example();
    const auto refuse = [&](auto change) {
        auto state = valid;
        change(state);
        CHECK(!FrameCoherence::isValidContinuation(state.coherence) || !RemoteInputEncoder::isValidContinuation(state.input));
        CHECK(Host::encodeHandoverRuntime(state, limits).error() == Host::HandoverRuntimeError::Malformed);
    };
    refuse([](auto& state) { state.coherence.dirtySinceMicroseconds = std::numeric_limits<std::uint64_t>::max(); });
    refuse([](auto& state) { state.coherence.hasPassEnded = false; });
    refuse([](auto& state) { state.coherence.frameColumns = -1; });
    refuse([](auto& state) { state.input.pastes[0].clientInstance = {}; });
    refuse([](auto& state) { state.input.pastes[1].didClose = false; });
    refuse([](auto& state) { state.input.pastes[1].lastWriteBytes = std::numeric_limits<std::uint64_t>::max(); });
}

TEST(runtimeSectionRestoresTheActualFrameDecisionWithoutRebasingItsClock)
{
    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, 6, 20);
    for (const auto sequence : { std::string_view("hello"), std::string_view("\x1b[?25lhidden"), std::string_view("\x1b[?2026hheld\x1b[?2026l") }) {
        FrameCoherence source;
        source.noteFrameTaken(*terminal, 1000);
        feed(*terminal, sequence);
        source.noteDrainPass(*terminal, 2000, PassEnd::WouldBlock);
        const auto continuation = source.continuation();
        REQUIRE(continuation);
        Host::HandoverRuntime state;
        state.coherence = *continuation;
        const auto encoded = Host::encodeHandoverRuntime(state, limits);
        REQUIRE(encoded);
        const auto decoded = Host::decodeHandoverRuntime(*encoded, limits);
        REQUIRE(decoded);
        FrameCoherence restored;
        REQUIRE(restored.restoreContinuation(decoded->coherence));
        for (const auto now : { 4000u, 17666u, 17667u, 35334u, 40000u }) {
            const auto ticks = Ticks::fromMilliseconds(now / 1000);
            CHECK(source.microsecondsUntilAsk(*terminal, now, ticks) == restored.microsecondsUntilAsk(*terminal, now, ticks));
            const auto expected = source.admit(*terminal, now, ticks, ReadReadiness::Quiet);
            const auto actual = restored.admit(*terminal, now, ticks, ReadReadiness::Quiet);
            CHECK(actual.isAdmitted == expected.isAdmitted);
            CHECK(actual.frameClass == expected.frameClass);
            CHECK(restored.continuation() == source.continuation());
        }
        feed(*terminal, "\x1b[?25h");
    }
}

TEST(runtimeSectionPreservesEverySplitPasteFenceAndNewlinePair)
{
    for (const bool isBracketed : { false, true }) {
        for (const auto text : { std::string_view("a\r\nb\n\rc"), std::string_view("\x1b[200~x\x1b[201~") }) {
            for (std::size_t split = 0; split <= text.size(); ++split) {
                auto terminal = std::make_unique<Terminal>();
                terminal->modes.bracketedPaste = isBracketed;
                QueuedSink before;
                RemoteInputEncoder source(*terminal, before);
                REQUIRE(source.writeOwned(authority, client, piece(text.substr(0, split))) == Sync::WriteStatus::Written);
                const auto continuation = source.continuation();
                REQUIRE(continuation);
                Host::HandoverRuntime state;
                state.input = *continuation;
                const auto encoded = Host::encodeHandoverRuntime(state, limits);
                REQUIRE(encoded);
                const auto decoded = Host::decodeHandoverRuntime(*encoded, limits);
                REQUIRE(decoded);
                auto pending = before.queue.copyUnwritten();
                REQUIRE(pending);
                QueuedSink after;
                REQUIRE(after.queue.restoreUnwritten(*pending));
                RemoteInputEncoder restored(*terminal, after);
                REQUIRE(restored.restoreContinuation(decoded->input));
                CHECK_EQ(after.calls, 0);
                terminal->modes.bracketedPaste = !isBracketed; // Original paste mode survives.
                const auto tail = piece(text.substr(split), Sync::PasteEnd::Final);
                REQUIRE(source.writeOwned(authority, client, tail) == Sync::WriteStatus::Written);
                REQUIRE(restored.writeOwned(authority, client, tail) == Sync::WriteStatus::Written);
                before.finish();
                after.finish();
                CHECK_STR_EQ(after.delivered, before.delivered);
                CHECK_STR_EQ(after.delivered, Input::wrapPaste(text, isBracketed ? Input::BracketedPaste::Enabled : Input::BracketedPaste::Disabled));
            }
        }
    }
}

TEST(runtimeSectionPreservesEndedUndrainedDelimiterRepairAndUnrelatedBytes)
{
    for (std::size_t delivered = 0; delivered <= 15; ++delivered) {
        auto terminal = std::make_unique<Terminal>();
        terminal->modes.bracketedPaste = true;
        QueuedSink before;
        before.allowance = delivered;
        RemoteInputEncoder source(*terminal, before);
        REQUIRE(source.writeOwned(authority, client, piece("abc", Sync::PasteEnd::Final)) == Sync::WriteStatus::Written);
        REQUIRE(before.queue.submit(std::string_view("reply"), QueuedSink::accept, &before) == 0);
        Host::HandoverRuntime state;
        const auto continuation = source.continuation();
        REQUIRE(continuation && continuation->pasteCount == 1);
        REQUIRE(continuation->pastes[0].hasEnded);
        state.input = *continuation;
        const auto encoded = Host::encodeHandoverRuntime(state, limits);
        REQUIRE(encoded);
        const auto decoded = Host::decodeHandoverRuntime(*encoded, limits);
        REQUIRE(decoded);
        auto pending = before.queue.copyUnwritten();
        REQUIRE(pending);
        QueuedSink after;
        after.delivered = before.delivered;
        REQUIRE(after.queue.restoreUnwritten(*pending));
        RemoteInputEncoder restored(*terminal, after);
        REQUIRE(restored.restoreContinuation(decoded->input));
        CHECK_EQ(after.calls, 0);
        source.withdraw(authority);
        restored.withdraw(authority);
        before.finish();
        after.finish();
        CHECK_STR_EQ(after.delivered, before.delivered);
        CHECK(restored.continuation() == source.continuation());
        const auto reply = after.delivered.find("reply");
        CHECK(reply != std::string::npos && reply == after.delivered.rfind("reply"));
    }
}

UBIQUITTY_TEST_MAIN()

// A host reads only the runtime section it writes; an earlier version is
// refused rather than read in another layout.
TEST(anEarlierRuntimeSectionVersionIsRefused)
{
    auto state = example();
    for (auto& paste : state.input.pastes)
        paste.normalizer.options = {};
    auto bytes = Host::encodeHandoverRuntime(state, limits);
    REQUIRE(bytes);
    (*bytes)[0] = 1;
    redigest(*bytes);
    const auto refused = Host::decodeHandoverRuntime(*bytes, limits);
    REQUIRE(!refused);
    CHECK(refused.error() == Host::HandoverRuntimeError::Malformed);
}
