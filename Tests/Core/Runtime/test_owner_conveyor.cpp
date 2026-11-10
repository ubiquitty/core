// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Runtime/RuntimeLink.h"

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// test_owner_conveyor.cpp — a frontend's commands reach its pane's owner whole
// and in order, and a post never waits: a full conveyor refuses it, and the
// frontend's overflow keeps it, in order, until the conveyor has room.

using namespace Ubiquitty::Runtime;

namespace {

/// A command whose arguments name its place in a sequence.
struct SequenceArguments {
    std::uint64_t sequence = 0;
};

OwnerCommand numbered(std::uint64_t sequence)
{
    return OwnerCommand::make(OwnerCommandKind::SetFocus, SequenceArguments { sequence });
}

/// A payload whose bytes follow from its sequence, so a torn or misplaced one
/// shows.
std::vector<std::byte> payloadFor(std::uint64_t sequence, std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::byte>((sequence * 131 + index) & 0xff);
    return bytes;
}

bool isPayloadFor(std::uint64_t sequence, std::span<const std::byte> payload, std::size_t size)
{
    if (payload.size() != size)
        return false;
    for (std::size_t index = 0; index < size; ++index) {
        if (payload[index] != static_cast<std::byte>((sequence * 131 + index) & 0xff))
            return false;
    }
    return true;
}

} // namespace

TEST(postedCommandsArriveInOrderWithTheirArguments)
{
    OwnerConveyor conveyor;
    for (std::uint64_t sequence = 1; sequence <= 10; ++sequence)
        REQUIRE(conveyor.post(numbered(sequence)));
    OwnerCommand command;
    std::span<const std::byte> payload;
    for (std::uint64_t sequence = 1; sequence <= 10; ++sequence) {
        REQUIRE(conveyor.pop(command, payload));
        CHECK_EQ(command.argumentsAs<SequenceArguments>().sequence, sequence);
        CHECK(payload.empty());
    }
    CHECK(!conveyor.pop(command, payload));
    CHECK(conveyor.isEmpty());
}

// A payload that would run past the arena's end starts again at its
// beginning, whole.
TEST(aPayloadArrivesWholeAcrossTheArenasEnd)
{
    OwnerConveyor conveyor;
    const std::size_t size = OwnerConveyor::maxPayloadBytes - 7;
    OwnerCommand command;
    std::span<const std::byte> payload;
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        const auto bytes = payloadFor(sequence, size);
        REQUIRE(conveyor.post(numbered(sequence), bytes));
        REQUIRE(conveyor.pop(command, payload));
        CHECK_MSG(isPayloadFor(sequence, payload, size), "a payload arrived torn or misplaced");
    }
    conveyor.finishService();
}

TEST(aFullRingRefusesThePostAndKeepsWhatItHolds)
{
    OwnerConveyor conveyor;
    for (std::uint64_t sequence = 1; sequence <= OwnerConveyor::commandCapacity; ++sequence)
        REQUIRE(conveyor.post(numbered(sequence)));
    CHECK_MSG(!conveyor.post(numbered(0)), "a full ring took one more");
    OwnerCommand command;
    std::span<const std::byte> payload;
    REQUIRE(conveyor.pop(command, payload));
    CHECK_EQ(command.argumentsAs<SequenceArguments>().sequence, 1u);
    CHECK_MSG(conveyor.post(numbered(OwnerConveyor::commandCapacity + 1)), "a pop made no room");
}

// The owner reads a popped payload in place until its next pop, so the
// frontend writes no payload over it before then.
TEST(aPayloadWaitsForTheOwnerToReleaseItsSpace)
{
    OwnerConveyor conveyor;
    const std::size_t size = OwnerConveyor::maxPayloadBytes;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence)
        REQUIRE(conveyor.post(numbered(sequence), payloadFor(sequence, size)));
    CHECK_MSG(!conveyor.post(numbered(5), payloadFor(5, size)), "a full arena took one more payload");
    OwnerCommand command;
    std::span<const std::byte> payload;
    REQUIRE(conveyor.pop(command, payload));
    CHECK_MSG(!conveyor.post(numbered(5), payloadFor(5, size)), "a payload the owner still reads was written over");
    CHECK(isPayloadFor(1, payload, size));
    conveyor.finishService();
    REQUIRE(conveyor.post(numbered(5), payloadFor(5, size)));
    for (std::uint64_t sequence = 2; sequence <= 5; ++sequence) {
        REQUIRE(conveyor.pop(command, payload));
        CHECK(isPayloadFor(sequence, payload, size));
    }
}

TEST(aPayloadLongerThanAQuarterOfTheArenaIsRefused)
{
    OwnerConveyor conveyor;
    CHECK(!conveyor.post(numbered(1), payloadFor(1, OwnerConveyor::maxPayloadBytes + 1)));
    CHECK(conveyor.isEmpty());
}

// One producer thread and one consumer thread, as a frontend and its owner:
// every command and payload arrives once, whole and in order.
TEST(anOwnerOnAnotherThreadSeesEveryCommandInOrder)
{
    OwnerConveyor conveyor;
    constexpr std::uint64_t count = 100'000;
    const auto sizeFor = [](std::uint64_t sequence) -> std::size_t { return sequence % 5 == 0 ? (sequence * 977) % 40'000 : 0; };
    std::uint64_t mismatches = 0;
    std::thread owner([&] {
        OwnerCommand command;
        std::span<const std::byte> payload;
        std::uint64_t expected = 1;
        while (expected <= count) {
            if (!conveyor.pop(command, payload)) {
                std::this_thread::yield();
                continue;
            }
            const std::uint64_t sequence = command.argumentsAs<SequenceArguments>().sequence;
            if (sequence != expected || !isPayloadFor(sequence, payload, sizeFor(sequence)))
                ++mismatches;
            ++expected;
        }
        conveyor.finishService();
    });
    for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
        const auto bytes = payloadFor(sequence, sizeFor(sequence));
        while (!conveyor.post(numbered(sequence), bytes))
            std::this_thread::yield();
    }
    owner.join();
    CHECK_EQ(mismatches, 0u);
    CHECK(conveyor.isEmpty());
}

// A conveyor with no room takes nothing more, and the overflow keeps what the
// frontend posts behind it, in order.
TEST(theOverflowKeepsOrderOnceTheRingIsFull)
{
    OwnerConveyor conveyor;
    ConveyorOverflow overflow;
    constexpr std::uint64_t count = OwnerConveyor::commandCapacity + 40;
    for (std::uint64_t sequence = 1; sequence <= count; ++sequence)
        REQUIRE(overflow.post(conveyor, numbered(sequence), payloadFor(sequence, sequence % 3 ? 0 : 100)));
    CHECK(overflow.isHolding());
    OwnerCommand command;
    std::span<const std::byte> payload;
    for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
        if (!conveyor.pop(command, payload)) {
            conveyor.finishService();
            (void)overflow.flush(conveyor);
            REQUIRE(conveyor.pop(command, payload));
        }
        CHECK_EQ(command.argumentsAs<SequenceArguments>().sequence, sequence);
        CHECK(isPayloadFor(sequence, payload, sequence % 3 ? 0 : 100));
    }
    CHECK(!overflow.isHolding());
}

TEST(theOverflowRefusesPastItsBound)
{
    OwnerConveyor conveyor;
    ConveyorOverflow overflow;
    std::uint64_t sequence = 0;
    while (overflow.post(conveyor, numbered(++sequence))) {
        if (sequence > OwnerConveyor::commandCapacity + ConveyorOverflow::maxCommands + 1)
            break;
    }
    CHECK_EQ(sequence, OwnerConveyor::commandCapacity + ConveyorOverflow::maxCommands + 1);
}

UBIQUITTY_TEST_MAIN()
