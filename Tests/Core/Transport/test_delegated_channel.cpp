// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_delegated_channel.cpp — the embedder-driven Transport::Channel.
//
// The Driver here stands in for the frontend's own carrier (a Swift SSH client
// on iOS): the test drives it directly, so the cases assert the seam's contract
// rather than any transport's behaviour. What matters is the ordering: no
// observer callback survives stop(), an observer may send from inside a
// callback, and a Driver that fails to start fires nothing at all.

#include "Core/Transport/DelegatedChannel.h"
#include "Core/Transport/InboundBackpressure.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Ubiquitty::Transport;

namespace {

Origin sshOrigin(const char* host)
{
    return Origin { OriginKind::SSH, std::string(host) };
}

// A Driver whose behaviour each case sets up: it records what the channel asked
// of it, and lets the case decide whether start succeeds.
struct FakeCarrier {
    bool shouldStartSucceed = true;
    bool didStart = false;
    bool didStop = false;
    bool didRelease = false;
    int columns = 0;
    int rows = 0;
    std::string sent;

    static bool start(void* context)
    {
        auto* self = static_cast<FakeCarrier*>(context);
        self->didStart = true;
        return self->shouldStartSucceed;
    }
    static void send(void* context, const std::uint8_t* bytes, std::size_t count)
    {
        auto* self = static_cast<FakeCarrier*>(context);
        self->sent.append(reinterpret_cast<const char*>(bytes), count);
    }
    static void stop(void* context) { static_cast<FakeCarrier*>(context)->didStop = true; }
    static void setWindowSize(void* context, int columns, int rows)
    {
        auto* self = static_cast<FakeCarrier*>(context);
        self->columns = columns;
        self->rows = rows;
    }
    static void release(void* context) { static_cast<FakeCarrier*>(context)->didRelease = true; }

    // The carrier is stack-owned by the case, so release only records the call.
    DelegatedChannel::Driver driver()
    {
        // Designated rather than positional: several Driver entries share one
        // signature, so a positional list survives a member reorder by calling
        // the wrong carrier hook instead of failing to compile.
        return DelegatedChannel::Driver {
            .context = this,
            .start = &start,
            .send = &send,
            .stop = &stop,
            .setWindowSize = &setWindowSize,
            .release = &release,
        };
    }
};

class Collector final : public ChannelObserver {
public:
    void onConnect() override { ++connects; }
    void onData(std::span<const std::uint8_t> data) override
    {
        received.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
    void onClose(const std::string& reason) override
    {
        ++closes;
        closeReason = reason;
    }

    int connects = 0;
    int closes = 0;
    std::string received;
    std::string closeReason;
};

// An observer that answers onConnect with a send, the way the Sync client
// answers with its HELLO.
class ReplyingCollector final : public ChannelObserver {
public:
    explicit ReplyingCollector(DelegatedChannel& channel)
        : m_channel(channel)
    {
    }
    void onConnect() override
    {
        const char* hello = "HELLO";
        m_channel.send({ reinterpret_cast<const std::uint8_t*>(hello), std::strlen(hello) });
    }
    void onData(std::span<const std::uint8_t> data) override
    {
        // Echo back from inside the callback: the other re-entrant direction.
        m_channel.send(data);
    }
    void onClose(const std::string&) override { }

private:
    DelegatedChannel& m_channel;
};

} // namespace

// ---- The happy path: connect, both directions, clean stop ----
TEST(test_delegated_round_trip)
{
    FakeCarrier carrier;
    {
        DelegatedChannel channel(carrier.driver(), sshOrigin("build.example"), "ssh build.example");
        Collector collector;

        CHECK_MSG(channel.health() == Health::Idle, "idle before start");
        CHECK_MSG(channel.start(&collector), "start");
        CHECK_MSG(carrier.didStart, "driver start called");
        CHECK_MSG(channel.health() == Health::Connecting, "connecting until the carrier answers");

        channel.notifyConnect();
        CHECK_MSG(collector.connects == 1, "onConnect delivered");
        CHECK_MSG(channel.health() == Health::Connected, "health Connected");

        const char* outbound = "ls -l\n";
        channel.send({ reinterpret_cast<const std::uint8_t*>(outbound), std::strlen(outbound) });
        CHECK_MSG(carrier.sent == "ls -l\n", "outbound reached the driver");

        const char* inbound = "total 0\n";
        channel.deliver({ reinterpret_cast<const std::uint8_t*>(inbound), std::strlen(inbound) });
        CHECK_MSG(collector.received == "total 0\n", "inbound reached the observer");

        channel.setWindowSize(120, 40);
        CHECK_MSG(carrier.columns == 120 && carrier.rows == 40, "resize reached the driver");

        channel.stop();
        CHECK_MSG(carrier.didStop, "driver stop called");
        CHECK_MSG(collector.closes == 0, "clean stop fires no onClose");
        CHECK_MSG(channel.health() == Health::Closed, "health Closed after stop");
    }
    CHECK_MSG(carrier.didRelease, "driver context released on destruction");
}

// ---- The origin is the carrier's, so the Kit facade need not branch ----
TEST(test_delegated_origin_is_the_carriers)
{
    FakeCarrier carrier;
    DelegatedChannel channel(carrier.driver(), sshOrigin("build.example"), "ssh build.example");
    CHECK_MSG(channel.origin().transport == OriginKind::SSH, "origin kind SSH");
    CHECK_MSG(channel.origin().host == "build.example", "origin host");
    CHECK_MSG(channel.origin().isRemote(), "an ssh carrier is remote");
    CHECK_MSG(channel.description() == "ssh build.example", "description");
}

// ---- Nothing reaches the observer once the channel has stopped ----
TEST(test_delegated_no_callback_after_stop)
{
    FakeCarrier carrier;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    Collector collector;
    CHECK_MSG(channel.start(&collector), "start");
    channel.notifyConnect();
    channel.stop();

    // A carrier still draining on its own thread: every one of these is dropped
    // rather than reaching an observer the caller is now free to destroy.
    channel.notifyConnect();
    const char* late = "late bytes";
    channel.deliver({ reinterpret_cast<const std::uint8_t*>(late), std::strlen(late) });
    channel.notifyClose(Health::Failed, "late close");

    CHECK_MSG(collector.connects == 1, "no second onConnect after stop");
    CHECK_MSG(collector.received.empty(), "no onData after stop");
    CHECK_MSG(collector.closes == 0, "no onClose after stop");

    // Outbound is equally inert; the driver saw only its stop.
    channel.send({ reinterpret_cast<const std::uint8_t*>(late), std::strlen(late) });
    CHECK_MSG(carrier.sent.empty(), "no send after stop");
}

// ---- A failed start fires no callback, per the Channel contract ----
TEST(test_delegated_start_failure_is_silent)
{
    FakeCarrier carrier;
    carrier.shouldStartSucceed = false;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    Collector collector;

    CHECK_MSG(!channel.start(&collector), "start reports the setup failure");
    CHECK_MSG(channel.health() == Health::Failed, "health Failed");
    CHECK_MSG(collector.connects == 0 && collector.closes == 0, "a failed start fires nothing");

    // The observer was dropped, so a carrier that calls back anyway is inert.
    channel.notifyClose(Health::Failed, "spurious");
    CHECK_MSG(collector.closes == 0, "no onClose after a failed start");
}

// ---- A Driver missing start is a setup failure, not a crash ----
TEST(test_delegated_driver_without_start)
{
    DelegatedChannel channel(DelegatedChannel::Driver {}, sshOrigin("host"), "ssh host");
    Collector collector;
    CHECK_MSG(!channel.start(&collector), "a Driver with no start cannot start");
    CHECK_MSG(channel.health() == Health::Failed, "health Failed");
}

// ---- start() is idempotent; a second call does not re-enter the Driver ----
TEST(test_delegated_start_is_idempotent)
{
    FakeCarrier carrier;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    Collector first;
    Collector second;
    CHECK_MSG(channel.start(&first), "first start");
    carrier.didStart = false;
    CHECK_MSG(channel.start(&second), "second start is a no-op that succeeds");
    CHECK_MSG(!carrier.didStart, "the driver is not didStart twice");

    channel.notifyConnect();
    CHECK_MSG(first.connects == 1, "the first observer keeps the channel");
    CHECK_MSG(second.connects == 0, "the second observer never bound");
}

// ---- notifyClose reports the carrier's own verdict, not an inferred one ----
TEST(test_delegated_close_outcome_is_the_carriers)
{
    {
        FakeCarrier carrier;
        DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
        Collector collector;
        channel.start(&collector);
        channel.notifyConnect();
        channel.notifyClose(Health::Closed, "peer EOF");
        CHECK_MSG(collector.closes == 1, "onClose delivered");
        CHECK_MSG(collector.closeReason == "peer EOF", "close reason");
        CHECK_MSG(channel.health() == Health::Closed, "a clean EOF from a Connected carrier is Closed");
    }
    {
        // The same prior state, the opposite verdict: proof the outcome is not
        // inferred from health.
        FakeCarrier carrier;
        DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
        Collector collector;
        channel.start(&collector);
        channel.notifyConnect();
        channel.notifyClose(Health::Failed, "connection reset");
        CHECK_MSG(channel.health() == Health::Failed, "an I/O error from a Connected carrier is Failed");
    }
}

// ---- Only one onClose, however many times the carrier reports it ----
TEST(test_delegated_close_is_delivered_once)
{
    FakeCarrier carrier;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    Collector collector;
    channel.start(&collector);
    channel.notifyConnect();
    channel.notifyClose(Health::Failed, "first");
    channel.notifyClose(Health::Failed, "second");
    CHECK_MSG(collector.closes == 1, "onClose is delivered once");
    CHECK_MSG(collector.closeReason == "first", "the first reason is the one reported");
}

// ---- An observer may send from inside a callback ----
//
// The Sync client sends its HELLO from onConnect, so a lock held across the
// callback would deadlock against send(). This case would hang, not fail, if
// that regressed.
TEST(test_delegated_observer_may_send_from_a_callback)
{
    FakeCarrier carrier;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    ReplyingCollector collector(channel);

    CHECK_MSG(channel.start(&collector), "start");
    channel.notifyConnect();
    CHECK_MSG(carrier.sent == "HELLO", "onConnect sent through the channel");

    const char* inbound = "echo";
    channel.deliver({ reinterpret_cast<const std::uint8_t*>(inbound), std::strlen(inbound) });
    CHECK_MSG(carrier.sent == "HELLOecho", "onData sent through the channel");
}

// ---- The Driver's thread may deliver while the owner stops ----
TEST(test_delegated_deliver_races_stop)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        FakeCarrier carrier;
        DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
        Collector collector;
        channel.start(&collector);
        channel.notifyConnect();

        std::atomic<bool> running { true };
        std::thread carrierThread([&channel, &running] {
            const char* bytes = "x";
            while (running.load(std::memory_order_acquire))
                channel.deliver({ reinterpret_cast<const std::uint8_t*>(bytes), 1 });
        });

        channel.stop();
        // Read once stop() has returned: the last in-flight deliver released
        // the channel's mutex before stop() could take it, so this is the final
        // value if the contract holds.
        const std::size_t deliveredByStop = collector.received.size();

        running.store(false, std::memory_order_release);
        carrierThread.join();

        CHECK_MSG(collector.received.size() == deliveredByStop, "no onData after stop() returned");
        CHECK_MSG(channel.health() == Health::Closed, "health Closed after a raced stop");
        CHECK_MSG(collector.closes == 0, "a raced stop still fires no onClose");
    }
}

// A carrier that pauses: the three contract entries the Channel grew after the
// seam was first written. What matters is the direction of each default — a
// Driver that cannot pause is inert (never wrongly paused), and a Driver that
// cannot count is unmeasured (never wrongly finished).
struct PacingCarrier : FakeCarrier {
    int suspends = 0;
    int resumes = 0;
    std::int64_t queuedAnswer = -1;

    static void suspendReading(void* context) { ++static_cast<PacingCarrier*>(context)->suspends; }
    static void resumeReading(void* context) { ++static_cast<PacingCarrier*>(context)->resumes; }
    static std::int64_t pendingOutboundBytes(void* context)
    {
        return static_cast<PacingCarrier*>(context)->queuedAnswer;
    }

    DelegatedChannel::Driver pacingDriver()
    {
        DelegatedChannel::Driver base = driver();
        base.context = this;
        base.suspendReading = &suspendReading;
        base.resumeReading = &resumeReading;
        base.pendingOutboundBytes = &pendingOutboundBytes;
        return base;
    }
};

TEST(theBackpressureEntriesReachTheDriver)
{
    PacingCarrier carrier;
    Collector collector;
    DelegatedChannel channel(carrier.pacingDriver(), sshOrigin("host"), "ssh host");
    CHECK(channel.start(&collector));

    channel.suspendReading();
    channel.resumeReading();
    CHECK_EQ(carrier.suspends, 1);
    CHECK_EQ(carrier.resumes, 1);

    carrier.queuedAnswer = 7;
    CHECK_EQ(channel.pendingOutboundBytes(), 7);
    // A carrier that answers negative is unmeasured, and unmeasured is -1
    // exactly — never a stray negative a progress bar might arithmetic on.
    carrier.queuedAnswer = -42;
    CHECK_EQ(channel.pendingOutboundBytes(), -1);

    // After stop the carrier is quiescing; nothing may reach it.
    channel.stop();
    channel.suspendReading();
    channel.resumeReading();
    CHECK_EQ(carrier.suspends, 1);
    CHECK_EQ(carrier.resumes, 1);
    CHECK_EQ(channel.pendingOutboundBytes(), -1);
}

TEST(aDriverWithoutPacingEntriesIsInertAndUnmeasured)
{
    FakeCarrier carrier;
    Collector collector;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    CHECK(channel.start(&collector));
    // The base contract's defaults, reached through the null entries: a no-op
    // pause and an unmeasured queue, with nothing to crash into.
    channel.suspendReading();
    channel.resumeReading();
    CHECK_EQ(channel.pendingOutboundBytes(), -1);
    channel.stop();
}

TEST(inboundBackpressurePacesADelegatedCarrier)
{
    // The merged behaviour, end to end: the consumer-side policy that landed
    // after this seam was first written must reach the embedder's carrier
    // through it, or a slow delegated session buffers without bound while the
    // policy believes it is pacing.
    PacingCarrier carrier;
    Collector collector;
    DelegatedChannel channel(carrier.pacingDriver(), sshOrigin("host"), "ssh host");
    CHECK(channel.start(&collector));

    InboundBackpressure pacing;
    pacing.applyTo(&channel, InboundBackpressure::suspendAboveBytes + 1);
    CHECK_MSG(pacing.isSuspended(), "the policy crossed its high mark");
    CHECK_EQ(carrier.suspends, 1);

    pacing.applyTo(&channel, InboundBackpressure::resumeAtOrBelowBytes);
    CHECK_MSG(!pacing.isSuspended(), "the policy fell to its low mark");
    CHECK_EQ(carrier.resumes, 1);

    channel.stop();
}

TEST(anObserverMayStopTheChannelFromInsideItsOwnCallback)
{
    // The drain in stop() waits for in-flight callbacks — except the caller's
    // own, or an observer that stops the channel on a protocol error would
    // wait for itself forever. A regression here is not a wrong value but a
    // hang, so the test's existence is the assertion: it completing at all is
    // what it checks, plus that the stop it made from inside the callback
    // held (nothing delivered after it).
    struct StoppingObserver final : ChannelObserver {
        DelegatedChannel* channel = nullptr;
        int received = 0;
        void onConnect() override { }
        void onData(std::span<const std::uint8_t>) override
        {
            ++received;
            channel->stop();
        }
        void onClose(const std::string&) override { }
    };

    FakeCarrier carrier;
    StoppingObserver observer;
    DelegatedChannel channel(carrier.driver(), sshOrigin("host"), "ssh host");
    observer.channel = &channel;
    CHECK(channel.start(&observer));

    const std::uint8_t byte = 'x';
    channel.deliver({ &byte, 1 });
    CHECK_EQ(observer.received, 1);
    CHECK(carrier.didStop);
    CHECK_MSG(channel.health() == Health::Closed, "the in-callback stop held");

    // The gate the in-callback stop closed stays closed.
    channel.deliver({ &byte, 1 });
    CHECK_EQ(observer.received, 1);
}

UBIQUITTY_TEST_MAIN()
