// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_taskqueue.cpp — host-side coverage for the TaskQueue abstraction
// (Source/PAL/TaskQueue.cpp).
//
// submit() runs the worker inline and queues the result; drain() pulls
// completed jobs FIFO on the calling thread, preserving submit order.

#include "PAL/TaskQueue.h"
#include <chrono>
#include <cstdio>
#include <thread>

struct CountJob {
    int callCount;
    int result;
    int input;
};

static void doubleWorker(void* p)
{
    CountJob* j = static_cast<CountJob*>(p);
    j->callCount++;
    j->result = j->input * 2;
}

enum {
    maxCompletions = 32
};
static int g_completions[maxCompletions];
static int g_completionCount = 0;
static void g_resetCompletions() { g_completionCount = 0; }
static void recordCompletion(void* p)
{
    if (g_completionCount < maxCompletions)
        g_completions[g_completionCount++] = static_cast<CountJob*>(p)->result;
}

// The worker runs asynchronously; drain in a loop until `expected` completions
// have arrived (or give up after a generous bound).
static int drainUntil(Ubiquitty::TaskQueue& q, int expected)
{
    int total = 0;
    for (int spins = 0; total < expected && spins < 5000; ++spins) {
        total += q.drain(recordCompletion);
        if (total < expected)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return total;
}

TEST(testConstructEmpty)
{
    Ubiquitty::TaskQueue q;
    CHECK(q.pendingCount() == 0);
    CHECK(q.drain(recordCompletion) == 0);
}

TEST(testSubmitRunsWorker)
{
    Ubiquitty::TaskQueue q;
    CountJob job = { 0, -1, 7 };
    CHECK(q.submit(doubleWorker, &job));
    CHECK(q.pendingCount() == 1); // inflight until drained
    g_resetCompletions();
    CHECK(drainUntil(q, 1) == 1);
    CHECK(job.callCount == 1);
    CHECK(job.result == 14);
    CHECK(q.pendingCount() == 0);
}

TEST(testDrainFifoOrder)
{
    Ubiquitty::TaskQueue q;
    CountJob jobs[5];
    for (int i = 0; i < 5; ++i) {
        jobs[i] = { 0, -1, i + 1 };
        CHECK(q.submit(doubleWorker, &jobs[i]));
    }
    CHECK(q.pendingCount() == 5);
    g_resetCompletions();
    CHECK(drainUntil(q, 5) == 5);
    CHECK(q.pendingCount() == 0);
    CHECK(g_completionCount == 5);
    for (int i = 0; i < 5; ++i) // single worker → FIFO: 2,4,6,8,10
        CHECK(g_completions[i] == (i + 1) * 2);
}

TEST(testDrainEmptyQueue)
{
    Ubiquitty::TaskQueue q;
    g_resetCompletions();
    CHECK(q.drain(recordCompletion) == 0);
    CHECK(g_completionCount == 0);
}

TEST(testMultipleSubmitDrainCycles)
{
    Ubiquitty::TaskQueue q;
    for (int cycle = 0; cycle < 3; ++cycle) {
        CountJob a = { 0, -1, cycle * 10 + 1 };
        CountJob b = { 0, -1, cycle * 10 + 2 };
        CHECK(q.submit(doubleWorker, &a));
        CHECK(q.submit(doubleWorker, &b));
        g_resetCompletions();
        CHECK(drainUntil(q, 2) == 2);
        CHECK(g_completionCount == 2);
        CHECK(g_completions[0] == a.result);
        CHECK(g_completions[1] == b.result);
        CHECK(q.pendingCount() == 0);
    }
}

UBIQUITTY_TEST_MAIN()
