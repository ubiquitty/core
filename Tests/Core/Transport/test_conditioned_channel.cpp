// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_conditioned_channel.cpp — the impairing Channel decorator.
//
// The conditioner is an instrument, so what it claims about a network has to be true before any
// session result measured through it means anything. A session result depends on all three
// properties below, and each is asserted here rather than assumed:
//
//   Order. Latency and jitter DELAY a reliable stream; they never permute it. A conditioner that
//   reordered would desynchronise the frame decoder and read as a protocol bug no carrier can cause.
//
//   Loss is datagram-only. A reliable stream is retransmitted underneath, so loss there is latency,
//   not absence. Dropping stream bytes would model a corruption neither TCP nor a QUIC stream can
//   deliver.
//
//   A blackout is not a close. It holds both directions and reports nothing, which is the half-open
//   link — the case a keep-alive exists to notice and the one `onClose` never fires for. A cut is
//   the reported drop, and the two must not be confusable.
//
// The clock is manual: a conditioner asserted against sleep measures the machine's load.
//
// Exit 0 on success.

#include "Core/Transport/ConditionedChannel.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Ubiquitty::Transport;

namespace {

// A LinkScheduler the case steps by hand. `advanceTo` runs everything due at or before the new now,
// in (deadline, arrival) order — the same order the real scheduler runs them in.
class ManualLinkScheduler final : public LinkScheduler {
public:
    [[nodiscard]] std::uint64_t nowMilliseconds() const override { return m_now; }

    void scheduleAt(std::uint64_t deadlineMilliseconds, std::function<void()> work) override
    {
        m_queue.emplace(Key { deadlineMilliseconds, m_nextSequence++ }, std::move(work));
    }

    void advanceTo(std::uint64_t nowMilliseconds)
    {
        m_now = nowMilliseconds;
        // Re-checked each turn: work may schedule more work, and a release due at this same instant
        // has to run in this same advance rather than wait for the next one.
        while (!m_queue.empty() && m_queue.begin()->first.first <= m_now) {
            auto work = std::move(m_queue.begin()->second);
            m_queue.erase(m_queue.begin());
            work();
        }
    }

    [[nodiscard]] std::size_t pending() const { return m_queue.size(); }

private:
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    std::map<Key, std::function<void()>> m_queue;
    std::uint64_t m_now = 0;
    std::uint64_t m_nextSequence = 0;
};

// The carrier under the conditioner. Records what reached it and lets the case fire inbound events
// as the I/O thread would.
class FakeInner final : public Channel {
public:
    bool start(ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }
    void send(std::span<const std::uint8_t> data) override
    {
        sent.emplace_back(data.begin(), data.end());
    }
    void stop() override
    {
        std::unique_lock<std::mutex> lock(m_stopMutex);
        didStop = true;
        m_hasStopEntered = true;
        m_stopSignal.notify_all();
        m_stopSignal.wait(lock, [this] { return !m_shouldHoldStop; });
    }
    [[nodiscard]] Origin origin() const override { return Origin { OriginKind::Daemon, "" }; }
    [[nodiscard]] Health health() const override { return m_health; }
    [[nodiscard]] std::string description() const override { return "fake"; }

    void fireConnect()
    {
        m_health = Health::Connected;
        if (m_observer)
            m_observer->onConnect();
    }
    void fireData(const std::vector<std::uint8_t>& bytes)
    {
        if (m_observer)
            m_observer->onData(bytes);
    }
    void fireDatagram(const std::vector<std::uint8_t>& bytes)
    {
        if (m_observer)
            m_observer->onDatagram(bytes);
    }
    void fireClose(const std::string& reason)
    {
        m_health = Health::Closed;
        if (m_observer)
            m_observer->onClose(reason);
    }

    void holdStop()
    {
        std::lock_guard<std::mutex> guard(m_stopMutex);
        m_shouldHoldStop = true;
        m_hasStopEntered = false;
    }

    [[nodiscard]] bool waitForStop(int milliseconds)
    {
        std::unique_lock<std::mutex> lock(m_stopMutex);
        return m_stopSignal.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] {
            return m_hasStopEntered;
        });
    }

    void allowStop()
    {
        {
            std::lock_guard<std::mutex> guard(m_stopMutex);
            m_shouldHoldStop = false;
        }
        m_stopSignal.notify_all();
    }

    std::vector<std::vector<std::uint8_t>> sent;
    bool didStop = false;

private:
    ChannelObserver* m_observer = nullptr;
    Health m_health = Health::Connecting;
    std::mutex m_stopMutex;
    std::condition_variable m_stopSignal;
    bool m_shouldHoldStop = false;
    bool m_hasStopEntered = false;
};

// Records what the session saw, in arrival order.
class RecordingObserver final : public ChannelObserver {
public:
    void onConnect() override { ++connects; }
    void onData(std::span<const std::uint8_t> data) override
    {
        received.emplace_back(data.begin(), data.end());
    }
    void onDatagram(std::span<const std::uint8_t> data) override
    {
        datagrams.emplace_back(data.begin(), data.end());
    }
    void onClose(const std::string& reason) override
    {
        ++closes;
        lastCloseReason = reason;
    }

    std::vector<std::vector<std::uint8_t>> received;
    std::vector<std::vector<std::uint8_t>> datagrams;
    int connects = 0;
    int closes = 0;
    std::string lastCloseReason;
};

// Parks one chosen callback in client code. Every cross-thread observation goes through the
// condition variable, so the race case is also a valid ThreadSanitizer case.
enum class BlockedCallback {
    Connect,
    Data,
};

class BlockingObserver final : public ChannelObserver {
public:
    explicit BlockingObserver(BlockedCallback callback)
        : m_blockedCallback(callback)
    {
    }

    void onConnect() override
    {
        if (m_blockedCallback == BlockedCallback::Connect)
            enterCallback();
    }
    void onData(std::span<const std::uint8_t>) override
    {
        if (m_blockedCallback == BlockedCallback::Data)
            enterCallback();
    }
    void onClose(const std::string&) override { }

    [[nodiscard]] bool waitForCallback(int milliseconds)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_signal.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] {
            return m_hasCallbackEntered;
        });
    }

    void allowCallback()
    {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_mayReturn = true;
        }
        m_signal.notify_all();
    }

    [[nodiscard]] bool callbackReturned()
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_hasCallbackReturned;
    }

    [[nodiscard]] int callbacks()
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_callbacks;
    }

private:
    void enterCallback()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_callbacks;
        m_hasCallbackEntered = true;
        m_signal.notify_all();
        m_signal.wait(lock, [this] { return m_mayReturn; });
        m_hasCallbackReturned = true;
        m_signal.notify_all();
    }

    BlockedCallback m_blockedCallback;
    std::mutex m_mutex;
    std::condition_variable m_signal;
    int m_callbacks = 0;
    bool m_hasCallbackEntered = false;
    bool m_hasCallbackReturned = false;
    bool m_mayReturn = false;
};

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) { return { values }; }

// One chunk carrying a single ordinal, so a permutation is visible in the assertion.
std::vector<std::uint8_t> ordinal(std::uint8_t value) { return { value }; }

} // namespace

TEST(clearConditionsPassEverythingStraightThrough)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    ConditionedChannel channel(std::move(inner), &scheduler);
    RecordingObserver observer;

    CHECK(channel.start(&observer));
    innerRaw->fireConnect();
    CHECK_EQ(observer.connects, 1);

    innerRaw->fireData(bytes({ 'a', 'b' }));
    // Even at zero latency a release is scheduled, so the clock still has to turn.
    scheduler.advanceTo(0);
    CHECK_EQ(static_cast<int>(observer.received.size()), 1);
    CHECK_EQ(static_cast<int>(observer.received[0].size()), 2);

    const std::uint8_t out[] = { 'x' };
    channel.send(out);
    scheduler.advanceTo(0);
    CHECK_EQ(static_cast<int>(innerRaw->sent.size()), 1);
}

TEST(latencyHoldsDeliveryUntilItsDeadline)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    LinkConditions conditions;
    conditions.latencyMilliseconds = 50;
    ConditionedChannel channel(std::move(inner), &scheduler, conditions);
    RecordingObserver observer;
    CHECK(channel.start(&observer));

    innerRaw->fireData(bytes({ 'a' }));
    scheduler.advanceTo(49);
    CHECK_MSG(observer.received.empty(), "delivered before the one-way delay elapsed");
    scheduler.advanceTo(50);
    CHECK_EQ(static_cast<int>(observer.received.size()), 1);
}

TEST(jitterDelaysTheStreamButNeverReordersIt)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    LinkConditions conditions;
    conditions.latencyMilliseconds = 10;
    conditions.jitterMilliseconds = 40; // four times the base delay: a naive draw WILL invert a pair
    ConditionedChannel channel(std::move(inner), &scheduler, conditions);
    RecordingObserver observer;
    CHECK(channel.start(&observer));

    constexpr int chunkCount = 64;
    for (int index = 0; index < chunkCount; ++index)
        innerRaw->fireData(ordinal(static_cast<std::uint8_t>(index)));

    scheduler.advanceTo(100000);
    CHECK_EQ(static_cast<int>(observer.received.size()), chunkCount);
    for (int index = 0; index < chunkCount; ++index)
        CHECK_EQ(static_cast<int>(observer.received[index][0]), index);
}

TEST(bandwidthSpreadsABurstRatherThanDroppingIt)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    LinkConditions conditions;
    conditions.bytesPerSecond = 1000; // one byte per millisecond
    ConditionedChannel channel(std::move(inner), &scheduler, conditions);
    RecordingObserver observer;
    CHECK(channel.start(&observer));

    // Three 100-byte chunks: 100ms of serialization each, so the third lands at 300ms.
    for (int index = 0; index < 3; ++index)
        innerRaw->fireData(std::vector<std::uint8_t>(100, static_cast<std::uint8_t>(index)));

    scheduler.advanceTo(100);
    CHECK_EQ(static_cast<int>(observer.received.size()), 1);
    scheduler.advanceTo(299);
    CHECK_EQ(static_cast<int>(observer.received.size()), 2);
    scheduler.advanceTo(300);
    CHECK_EQ(static_cast<int>(observer.received.size()), 3);
    CHECK_MSG(observer.closes == 0, "a metered link dropped the session instead of pacing it");
}

TEST(blackoutHoldsBothDirectionsAndReportsNothing)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    ConditionedChannel channel(std::move(inner), &scheduler);
    RecordingObserver observer;
    CHECK(channel.start(&observer));
    innerRaw->fireConnect();

    channel.beginBlackout();
    innerRaw->fireData(bytes({ 'a' }));
    const std::uint8_t out[] = { 'x' };
    channel.send(out);

    scheduler.advanceTo(10000);
    CHECK_MSG(observer.received.empty(), "a blackout delivered inbound bytes");
    CHECK_MSG(innerRaw->sent.empty(), "a blackout delivered outbound bytes");
    // The half-open link: nothing is reported, so only a liveness deadline can notice.
    CHECK_EQ(observer.closes, 0);
    CHECK_MSG(channel.innerIsOpen(), "a blackout closed the carrier");

    channel.endBlackout();
    scheduler.advanceTo(20000);
    CHECK_EQ(static_cast<int>(observer.received.size()), 1);
    CHECK_EQ(static_cast<int>(innerRaw->sent.size()), 1);
}

TEST(aCutIsReportedAndDiscardsWhatTheBlackoutHeld)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    ConditionedChannel channel(std::move(inner), &scheduler);
    RecordingObserver observer;
    CHECK(channel.start(&observer));
    innerRaw->fireConnect();

    channel.beginBlackout();
    innerRaw->fireData(bytes({ 'a' }));
    channel.cut("link reset");

    CHECK_EQ(observer.closes, 1);
    CHECK_MSG(observer.lastCloseReason == "link reset", "the cut's reason did not reach the session");
    CHECK_MSG(!channel.innerIsOpen(), "a cut left the carrier reading open");

    // A reset does not deliver what was in flight, and nothing follows a close.
    scheduler.advanceTo(10000);
    CHECK_MSG(observer.received.empty(), "a cut delivered bytes the blackout was holding");
    CHECK_EQ(observer.closes, 1);
}

TEST(lossDiscardsDatagramsAndNeverTheStream)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    LinkConditions conditions;
    conditions.datagramLossPercent = 100; // total, so the assertion is exact rather than statistical
    ConditionedChannel channel(std::move(inner), &scheduler, conditions);
    RecordingObserver observer;
    CHECK(channel.start(&observer));

    for (int index = 0; index < 8; ++index)
        innerRaw->fireDatagram(ordinal(static_cast<std::uint8_t>(index)));
    for (int index = 0; index < 8; ++index)
        innerRaw->fireData(ordinal(static_cast<std::uint8_t>(index)));
    scheduler.advanceTo(10000);

    CHECK_MSG(observer.datagrams.empty(), "a 100%-loss link delivered a datagram");
    CHECK_EQ(static_cast<int>(channel.datagramsDropped()), 8);
    // The reliable stream is retransmitted underneath; loss must not touch it.
    CHECK_EQ(static_cast<int>(observer.received.size()), 8);
    for (int index = 0; index < 8; ++index)
        CHECK_EQ(static_cast<int>(observer.received[index][0]), index);
}

TEST(noCallbackSurvivesStop)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    LinkConditions conditions;
    conditions.latencyMilliseconds = 50;
    ConditionedChannel channel(std::move(inner), &scheduler, conditions);
    RecordingObserver observer;
    CHECK(channel.start(&observer));

    innerRaw->fireData(bytes({ 'a' }));
    channel.stop();
    scheduler.advanceTo(10000);

    CHECK_MSG(observer.received.empty(), "a release scheduled before stop() fired after it");
    CHECK(innerRaw->didStop);
}

TEST(stopWaitsForAReleaseAlreadyInsideTheObserver)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    innerRaw->holdStop();
    ConditionedChannel channel(std::move(inner), &scheduler);
    BlockingObserver observer(BlockedCallback::Data);
    CHECK(channel.start(&observer));

    // One callback is parked in client code; the rest make the stopped generation's pending work
    // non-trivial and must all be rejected when the scheduler resumes its walk.
    constexpr int scheduledReleaseCount = 128;
    for (int index = 0; index < scheduledReleaseCount; ++index)
        innerRaw->fireData(ordinal(static_cast<std::uint8_t>(index)));

    std::thread schedulerThread([&scheduler] { scheduler.advanceTo(1000); });
    const bool hasCallbackEntered = observer.waitForCallback(2000);

    std::promise<void> stopReturnedPromise;
    std::future<void> stopReturned = stopReturnedPromise.get_future();
    std::thread stopThread([&channel, &stopReturnedPromise] {
        channel.stop();
        stopReturnedPromise.set_value();
    });

    // Holding the fake carrier's stop pins the stopping thread after it closed admission but before
    // it reaches the callback drain. Once released, the only thing allowed to keep stop() inside is
    // the observer callback parked above.
    const bool hasStopEntered = innerRaw->waitForStop(2000);
    innerRaw->allowStop();
    const bool hasReturnedWhileCallbackWasBlocked
        = stopReturned.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;

    observer.allowCallback();
    const bool hasStopFinished = stopReturned.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    schedulerThread.join();
    stopThread.join();

    CHECK_MSG(hasCallbackEntered, "the scheduled release never entered the observer");
    CHECK_MSG(hasStopEntered, "the concurrent stop never reached the inner carrier");
    CHECK_MSG(!hasReturnedWhileCallbackWasBlocked, "stop returned while client callback code was still running");
    CHECK_MSG(hasStopFinished, "stop did not return after the callback drained");
    CHECK_MSG(observer.callbackReturned(), "the admitted callback did not finish before stop returned");
    CHECK_EQ(observer.callbacks(), 1);

    scheduler.advanceTo(2000);
    CHECK_EQ(observer.callbacks(), 1);
}

TEST(stopWaitsForAStateCallbackAlreadyInsideTheObserver)
{
    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    innerRaw->holdStop();
    ConditionedChannel channel(std::move(inner), &scheduler);
    BlockingObserver observer(BlockedCallback::Connect);
    CHECK(channel.start(&observer));

    std::thread callbackThread([innerRaw] { innerRaw->fireConnect(); });
    const bool hasCallbackEntered = observer.waitForCallback(2000);

    std::promise<void> stopReturnedPromise;
    std::future<void> stopReturned = stopReturnedPromise.get_future();
    std::thread stopThread([&channel, &stopReturnedPromise] {
        channel.stop();
        stopReturnedPromise.set_value();
    });

    const bool hasStopEntered = innerRaw->waitForStop(2000);
    innerRaw->allowStop();
    const bool hasReturnedWhileCallbackWasBlocked
        = stopReturned.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;

    observer.allowCallback();
    const bool hasStopFinished = stopReturned.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    callbackThread.join();
    stopThread.join();

    CHECK_MSG(hasCallbackEntered, "the connect event never entered the observer");
    CHECK_MSG(hasStopEntered, "the concurrent stop never reached the inner carrier");
    CHECK_MSG(!hasReturnedWhileCallbackWasBlocked, "stop returned while a state callback was still running");
    CHECK_MSG(hasStopFinished, "stop did not return after the state callback drained");
    CHECK_MSG(observer.callbackReturned(), "the state callback did not finish before stop returned");
    CHECK_EQ(observer.callbacks(), 1);
}

TEST(aStoppedGenerationCannotReachAReplacementObserver)
{
    ManualLinkScheduler scheduler;
    LinkConditions conditions;
    conditions.latencyMilliseconds = 50;

    auto retiredInner = std::make_unique<FakeInner>();
    FakeInner* retiredInnerRaw = retiredInner.get();
    ConditionedChannel retired(std::move(retiredInner), &scheduler, conditions);
    auto retiredObserver = std::make_unique<RecordingObserver>();
    CHECK(retired.start(retiredObserver.get()));
    retiredInnerRaw->fireData(bytes({ 'o' }));
    retired.stop();

    RecordingObserver replacementObserver;
    CHECK_MSG(!retired.start(&replacementObserver), "a stopped channel accepted a replacement observer");
    retiredObserver.reset();

    // Reconnect owns a new ConditionedChannel and therefore a new State generation. Advancing one
    // shared scheduler runs the retired release and the new one at the same deadline; only the new
    // generation may reach the replacement observer.
    auto replacementInner = std::make_unique<FakeInner>();
    FakeInner* replacementInnerRaw = replacementInner.get();
    ConditionedChannel replacement(std::move(replacementInner), &scheduler, conditions);
    CHECK(replacement.start(&replacementObserver));
    replacementInnerRaw->fireData(bytes({ 'n' }));
    scheduler.advanceTo(50);

    CHECK_EQ(static_cast<int>(replacementObserver.received.size()), 1);
    CHECK_EQ(static_cast<int>(replacementObserver.received[0][0]), static_cast<int>('n'));
}

TEST(aReleaseOutlivingTheChannelDeliversToNothing)
{
    // The scheduler outlives the channel, which is the real teardown order: work queued against a
    // deadline is still in the queue when the session drops its carrier.
    ManualLinkScheduler scheduler;
    RecordingObserver observer;
    {
        auto inner = std::make_unique<FakeInner>();
        FakeInner* innerRaw = inner.get();
        LinkConditions conditions;
        conditions.latencyMilliseconds = 50;
        ConditionedChannel channel(std::move(inner), &scheduler, conditions);
        CHECK(channel.start(&observer));
        innerRaw->fireData(bytes({ 'a' }));
        CHECK_EQ(static_cast<int>(scheduler.pending()), 1);
    }
    scheduler.advanceTo(10000); // must not touch the destroyed channel
    CHECK_MSG(observer.received.empty(), "a release fired into a destroyed channel");
}

TEST(anObserverMaySendFromInsideACallback)
{
    // The carrier contract every other Channel keeps: no lock is held while an observer runs, so a
    // session that answers a frame by writing one does not deadlock against its own conditioner.
    class SendingObserver final : public ChannelObserver {
    public:
        void onConnect() override { }
        void onData(std::span<const std::uint8_t>) override
        {
            ++received;
            if (channel != nullptr && received == 1) {
                const std::uint8_t reply[] = { 'r' };
                channel->send(reply);
            }
        }
        void onClose(const std::string&) override { }
        ConditionedChannel* channel = nullptr;
        int received = 0;
    };

    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    ConditionedChannel channel(std::move(inner), &scheduler);
    SendingObserver observer;
    observer.channel = &channel;
    CHECK(channel.start(&observer));

    innerRaw->fireData(bytes({ 'a' }));
    scheduler.advanceTo(1);
    CHECK_EQ(observer.received, 1);
    scheduler.advanceTo(2);
    CHECK_MSG(innerRaw->sent.size() == 1, "the reply sent from inside onData never reached the carrier");
}

TEST(anObserverMayStopFromInsideACallback)
{
    // The other half of the same contract, and the one that closes the gate: nothing may follow.
    class StoppingObserver final : public ChannelObserver {
    public:
        void onConnect() override { }
        void onData(std::span<const std::uint8_t>) override
        {
            ++received;
            if (channel != nullptr)
                channel->stop();
        }
        void onClose(const std::string&) override { }
        ConditionedChannel* channel = nullptr;
        int received = 0;
    };

    ManualLinkScheduler scheduler;
    auto inner = std::make_unique<FakeInner>();
    FakeInner* innerRaw = inner.get();
    ConditionedChannel channel(std::move(inner), &scheduler);
    StoppingObserver observer;
    observer.channel = &channel;
    CHECK(channel.start(&observer));

    innerRaw->fireData(bytes({ 'a' }));
    innerRaw->fireData(bytes({ 'b' }));
    scheduler.advanceTo(10);
    CHECK_EQ(observer.received, 1);
    CHECK(innerRaw->didStop);

    // The gate the in-callback stop closed stays closed.
    innerRaw->fireData(bytes({ 'c' }));
    scheduler.advanceTo(20);
    CHECK_EQ(observer.received, 1);
}

UBIQUITTY_TEST_MAIN()
