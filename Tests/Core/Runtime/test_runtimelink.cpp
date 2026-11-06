// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// Tests/Core/Runtime/test_runtimelink.cpp — concurrency unit tests for the portable
// Ubiquitty::Runtime cross-thread primitives (SnapshotPublisher / CommandRing).
// Header-only seam: links nothing but libc++ + pthreads.
//
// Each test runs a real writer/reader thread pair and asserts the invariant a
// data race would violate: SnapshotPublisher must never hand a
// reader a TORN value (fields from two different publishes); CommandRing must
// preserve FIFO order with no loss and report full instead of dropping.
//

#include "Core/Runtime/RuntimeLink.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace Ubiquitty::Runtime;

// A payload whose every field equals the same counter, including an array member
// (the case an in-place seqlock would tear and corrupt mid-keystroke — ModeSnapshot
// has std::array fields, so this mirrors the real risk).
struct TearProbe {
    std::uint64_t a = 0;
    std::array<std::uint64_t, 8> arr {};
    std::uint64_t b = 0;
};

static constexpr std::uint64_t snapshotIters = 2'000'000;
static constexpr std::uint64_t ringIters = 1'000'000;

TEST(testSnapshotPublisherPreservesDefaultsAndPartialWords)
{
    struct Payload {
        std::array<unsigned char, 11> bytes { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5 };
    };
    SnapshotPublisher<Payload> publisher;
    CHECK(publisher.read().bytes == Payload {}.bytes);
    CHECK_EQ(publisher.version(), 0u);
    Payload next;
    next.bytes.fill(0xA5);
    publisher.publish(next);
    CHECK(publisher.read().bytes == next.bytes);
    CHECK_EQ(publisher.version(), 1u);
}

TEST(testSnapshotPublisherSupportsConcurrentReaders)
{
    SnapshotPublisher<TearProbe> publisher;
    std::atomic<unsigned> ready { 0 };
    std::atomic<bool> stop { false };
    std::atomic<unsigned> failures { 0 };
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) {
        reader = std::thread([&] {
            std::uint64_t previous = 0;
            ready.fetch_add(1, std::memory_order_release);
            do {
                const auto value = publisher.read();
                bool isCoherent = value.a >= previous && value.a == value.b;
                for (const auto element : value.arr) {
                    isCoherent &= element == value.a;
                }
                if (!isCoherent) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                previous = value.a;
            } while (!stop.load(std::memory_order_relaxed));
        });
    }
    while (ready.load(std::memory_order_acquire) != readers.size()) {
        std::this_thread::yield();
    }
    for (std::uint64_t index = 1; index <= 200'000; ++index) {
        TearProbe value;
        value.a = index;
        value.b = index;
        value.arr.fill(index);
        publisher.publish(value);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK_EQ(failures.load(), 0u);
    CHECK_EQ(publisher.read().a, 200'000u);
}

// (R) The reader must never observe a snapshot whose fields disagree.
TEST(testSnapshotPublisherTearFree)
{
    SnapshotPublisher<TearProbe> publisher;
    std::atomic<bool> stop { false };
    std::atomic<std::uint64_t> torn { 0 };
    std::atomic<std::uint64_t> reads { 0 };

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const TearProbe v = publisher.read();
            bool coherent = (v.b == v.a);
            for (std::uint64_t x : v.arr)
                if (x != v.a)
                    coherent = false;
            if (!coherent)
                torn.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (std::uint64_t i = 1; i <= snapshotIters; ++i) {
        TearProbe v;
        v.a = i;
        v.b = i;
        v.arr.fill(i);
        publisher.publish(v);
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    CHECK(torn.load() == 0); // never torn
    CHECK(reads.load() > 0); // the reader actually ran
    CHECK(publisher.version() == snapshotIters); // one version per publish, monotonic
}

// (W) FIFO order, full-not-drop, empty/pop semantics — single-thread checks first.
TEST(testCommandRingSemantics)
{
    CommandRing<int, 8> ring;
    int out = -1;
    CHECK(!ring.pop(out)); // empty
    CHECK(ring.empty());
    for (int i = 0; i < 8; ++i)
        CHECK(ring.post(i)); // fills to capacity
    CHECK(!ring.post(99)); // full -> false (never drops silently)
    CHECK(!ring.empty());
    for (int i = 0; i < 8; ++i) {
        CHECK(ring.pop(out));
        CHECK(out == i); // FIFO
    }
    CHECK(!ring.pop(out)); // drained
    CHECK(ring.empty());
}

// (W) Concurrent SPSC: every produced value is consumed exactly once, in order.
TEST(testCommandRingSpscConcurrent)
{
    CommandRing<std::uint64_t, 1024> ring;
    std::atomic<bool> done { false };
    std::uint64_t expected = 0;
    std::uint64_t consumed = 0;
    bool orderOk = true;

    std::thread consumer([&] {
        std::uint64_t v = 0;
        while (!done.load(std::memory_order_acquire) || !ring.empty()) {
            if (ring.pop(v)) {
                if (v != expected)
                    orderOk = false;
                ++expected;
                ++consumed;
            }
        }
    });

    for (std::uint64_t i = 0; i < ringIters; ++i)
        while (!ring.post(i)) {
        } // spin on backpressure (consumer drains) — never drop
    done.store(true, std::memory_order_release);
    consumer.join();

    CHECK(orderOk); // strict FIFO across the thread boundary
    CHECK(consumed == ringIters); // no loss, no duplication
}

UBIQUITTY_TEST_MAIN()
