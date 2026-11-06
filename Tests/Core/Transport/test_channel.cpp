// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_channel.cpp — the portable Transport::Channel backends.
//
// Exercises the real I/O paths headlessly: SubprocessChannel over `cat` (echo)
// and over a child that exits (drop), and LocalSocketChannel against an in-test
// AF_UNIX echo server. Asserts the observer contract — onConnect, onData round
// trip, onClose on a peer drop, and no onClose after a clean stop().

#include "Core/Transport/CarrierCapacity.h"
#include "Core/Transport/LocalSocketChannel.h"
#include "Core/Transport/PollChannel.h"
#include "PAL/Poll.h"
#include "PAL/Socket.h"

#include "Core/Transport/SubprocessChannel.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Ubiquitty::Transport;

namespace {

// Thread-safe event sink: the channel's I/O thread writes; the test thread
// waits. Mirrors how a real adapter would marshal, minus the UI hop.
class Collector final : public ChannelObserver {
public:
    void onConnect() override
    {
        std::scoped_lock lock(m_mutex);
        m_connected = true;
        m_cv.notify_all();
    }
    void onData(std::span<const std::uint8_t> data) override
    {
        std::scoped_lock lock(m_mutex);
        m_received.append(reinterpret_cast<const char*>(data.data()), data.size());
        m_cv.notify_all();
    }
    void onClose(const std::string& reason) override
    {
        std::scoped_lock lock(m_mutex);
        m_closed = true;
        m_closeReason = reason;
        m_cv.notify_all();
    }

    bool waitConnected(int ms)
    {
        return waitFor(ms, [&] { return m_connected; });
    }
    bool waitReceived(const std::string& want, int ms)
    {
        return waitFor(ms, [&] { return m_received.find(want) != std::string::npos; });
    }
    bool waitClosed(int ms)
    {
        return waitFor(ms, [&] { return m_closed; });
    }
    // What the channel has delivered, for a case that turns on how much rather
    // than on what: a suspended channel is one that delivers no more.
    std::size_t receivedBytes()
    {
        std::scoped_lock lock(m_mutex);
        return m_received.size();
    }
    std::string receivedString()
    {
        std::scoped_lock lock(m_mutex);
        return m_received;
    }
    bool waitReceivedBytes(std::size_t want, int ms)
    {
        return waitFor(ms, [&] { return m_received.size() >= want; });
    }
    bool sawClose()
    {
        std::scoped_lock lock(m_mutex);
        return m_closed;
    }
    std::string closeReason()
    {
        std::scoped_lock lock(m_mutex);
        return m_closeReason;
    }

private:
    template<class Pred> bool waitFor(int ms, Pred pred)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_connected = false;
    bool m_closed = false;
    std::string m_closeReason;
    std::string m_received;
};

Ubiquitty::Transport::Origin daemonOrigin(const char* host) { return { Ubiquitty::Transport::OriginKind::Daemon, host ? host : "" }; }

// ---- SubprocessChannel: echo via `cat` ----
TEST(test_subprocess_echo)
{
    SubprocessChannel channel("cat", daemonOrigin("local"));
    Collector collector;
    CHECK_MSG(channel.start(&collector), "subprocess start");
    CHECK_MSG(collector.waitConnected(2000), "subprocess onConnect");
    CHECK_MSG(channel.health() == Health::Connected, "subprocess health Connected");

    const char* msg = "ping-through-cat";
    channel.send({ reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) });
    CHECK_MSG(collector.waitReceived("ping-through-cat", 2000), "subprocess echo round trip");

    channel.stop();
    CHECK_MSG(!collector.sawClose(), "clean stop fires no onClose");
    CHECK_MSG(channel.health() == Health::Closed, "subprocess health Closed after stop");
}

TEST(subprocessDiagnosticsRemainBoundedAndSeparate)
{
    SubprocessChannel channel("printf protocol; i=0; while [ $i -lt 1000 ]; do printf 'diagnostic diagnostic diagnostic diagnostic diagnostic diagnostic diagnostic diagnostic\\n' >&2; i=$((i + 1)); done; printf 'Permission denied (publickey).' >&2", daemonOrigin("local"));
    Collector collector;
    CHECK(channel.start(&collector));
    CHECK(collector.waitClosed(5000));
    CHECK(collector.waitReceived("protocol", 100));
    CHECK(collector.closeReason().find("Permission denied") != std::string::npos);
    CHECK(collector.closeReason().size() <= 4200);
    channel.stop();
}

// ---- SubprocessChannel: a child that exits drops the channel ----
TEST(test_subprocess_drop)
{
    SubprocessChannel channel("exit 0", daemonOrigin("local"));
    Collector collector;
    CHECK_MSG(channel.start(&collector), "drop: start");
    CHECK_MSG(collector.waitClosed(2000), "drop: onClose on child exit");
    CHECK_MSG(channel.health() == Health::Closed || channel.health() == Health::Failed, "drop: terminal health");
    channel.stop();
}

// ---- LocalSocketChannel: round trip against an in-test AF_UNIX echo server ----
TEST(test_local_socket_echo)
{
    std::string path = "/tmp/mt-chan-test-" + std::to_string(::getpid()) + ".sock";
    ::unlink(path.c_str());

    const int listenFileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK_MSG(listenFileDescriptor >= 0, "socket: listen fileDescriptor");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    CHECK_MSG(::bind(listenFileDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "socket: bind");
    CHECK_MSG(::listen(listenFileDescriptor, 1) == 0, "socket: listen");

    std::atomic<bool> serverDone { false };
    std::thread server([listenFileDescriptor, &serverDone] {
        const int connection = ::accept(listenFileDescriptor, nullptr, nullptr);
        if (connection >= 0) {
            char buffer[4096];
            for (;;) {
                const ssize_t n = ::read(connection, buffer, sizeof(buffer));
                if (n <= 0)
                    break;
                ssize_t off = 0;
                while (off < n) {
                    const ssize_t w = ::write(connection, buffer + off, static_cast<std::size_t>(n - off));
                    if (w <= 0)
                        break;
                    off += w;
                }
            }
            ::close(connection);
        }
        serverDone.store(true);
    });

    {
        LocalSocketChannel channel(path, daemonOrigin("local"));
        Collector collector;
        CHECK_MSG(channel.start(&collector), "socket: start");
        CHECK_MSG(collector.waitConnected(2000), "socket: onConnect");
        const char* msg = "round-trip-over-unix";
        channel.send({ reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) });
        CHECK_MSG(collector.waitReceived("round-trip-over-unix", 2000), "socket: echo round trip");
        CHECK_MSG(!collector.sawClose(), "socket: no close mid-session");
        channel.stop(); // closes our end → the server read loop EOFs
    }

    server.join();
    CHECK_MSG(serverDone.load(), "socket: server drained");
    ::close(listenFileDescriptor);
    ::unlink(path.c_str());
}

// ---- LocalSocketChannel: a missing socket is an immediate setup failure ----
TEST(test_local_socket_connect_failure)
{
    LocalSocketChannel channel("/tmp/mt-chan-nonexistent.sock", daemonOrigin("local"));
    Collector collector;
    CHECK_MSG(!channel.start(&collector), "missing socket: start returns false");
    CHECK_MSG(channel.health() == Health::Failed, "missing socket: health Failed");
    CHECK_MSG(!collector.sawClose(), "missing socket: no onClose on local setup error");
}

// ---- A wedged peer (never reads stdin) overflows the outbound queue → clean drop ----
TEST(test_send_overflow)
{
    // `sleep 30` never reads its stdin, so our writes back up; a >cap send must
    // surface a clean onClose(Failed), not silently truncate the framed stream.
    SubprocessChannel channel("sleep 30", daemonOrigin("local"));
    Collector collector;
    CHECK_MSG(channel.start(&collector), "overflow: start");
    CHECK_MSG(collector.waitConnected(2000), "overflow: onConnect");

    std::vector<std::uint8_t> big(Ubiquitty::Transport::guaranteedOutboundFrameBytes + 1024u, 'x'); // past the floor the queue enforces
    channel.send(big);

    CHECK_MSG(collector.waitClosed(3000), "overflow: onClose fired");
    CHECK_MSG(collector.closeReason().find("too much unsent data") != std::string::npos, "overflow: reason names the overflow");
    CHECK_MSG(channel.health() == Health::Failed, "overflow: health Failed");
    channel.stop();
}

TEST(test_pending_outbound_bytes_reports_the_queue)
{
    // What the sender still holds. A caller draws a progress bar from it, so
    // "cannot say" (-1, the base class's answer) and "nothing left" must not
    // be the same value — a channel that keeps a queue answers from the queue.
    //
    // `sleep 30` never reads its stdin, so a payload past the pipe's buffer
    // stays queued for the life of the test rather than racing the drain.
    SubprocessChannel channel("sleep 30", daemonOrigin("local"));
    Collector collector;
    CHECK_MSG(channel.start(&collector), "pending: start");
    CHECK_MSG(collector.waitConnected(2000), "pending: onConnect");
    CHECK_MSG(channel.pendingOutboundBytes() == 0, "pending: nothing sent, nothing queued");

    const std::vector<std::uint8_t> payload(1024u * 1024u, 'x'); // past any pipe buffer, under the 4 MiB cap
    channel.send(payload);
    const std::int64_t pending = channel.pendingOutboundBytes();
    CHECK_MSG(pending > 0, "pending: a wedged peer leaves the payload queued");
    CHECK_MSG(pending <= static_cast<std::int64_t>(payload.size()), "pending: never more than was sent");
    channel.stop();
}

// ---- stop() called from inside an observer callback must not deadlock (self-join) ----
class StopOnClose final : public ChannelObserver {
public:
    explicit StopOnClose(Channel* channel)
        : m_channel(channel)
    {
    }
    void onConnect() override { }
    void onData(std::span<const std::uint8_t>) override { }
    void onClose(const std::string&) override
    {
        m_closed.store(true);
        m_channel->stop(); // re-entrant: runs on the I/O thread — must not self-join
    }
    bool closed() const { return m_closed.load(); }

private:
    Channel* m_channel;
    std::atomic<bool> m_closed { false };
};

TEST(test_stop_from_callback)
{
    SubprocessChannel channel("exit 0", daemonOrigin("local")); // drops immediately
    StopOnClose observer(&channel);
    CHECK_MSG(channel.start(&observer), "stop-from-callback: start");
    // The drop fires onClose on the I/O thread, which calls stop(). If the self-join
    // guard were missing this would deadlock; bound the wait so a hang fails loudly.
    for (int waited = 0; waited < 3000 && !observer.closed(); waited += 25)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK_MSG(observer.closed(), "stop-from-callback: onClose ran without deadlock");
    channel.stop(); // idempotent; reaps cleanly on the test thread
}

// A PollChannel over a socketpair, with the sender's view of the wake write end
// exposed so a test can occupy the send-versus-teardown window.
class SocketPairChannel final : public PollChannel {
public:
    SocketPairChannel()
        : PollChannel(daemonOrigin("local"))
    {
    }

    explicit SocketPairChannel(OverflowPolicy overflowPolicy)
        : PollChannel(daemonOrigin("local"), overflowPolicy)
    {
    }

    ~SocketPairChannel() override { stop(); }

    std::string description() const override { return "socketpair"; }

    using PollChannel::loadWaker;

    // The far end, for a test that needs to read the bytes the channel sent —
    // or to read them slowly, which is the only way to hold a flush open.
    int peer() const { return m_peer; }

protected:
    bool openTransport(std::unique_ptr<Ubiquitty::PAL::PollableByteStream>& readStream,
        std::unique_ptr<Ubiquitty::PAL::PollableByteStream>& writeStream, std::string& error) override
    {
        int pair[2] = { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
            error = "socketpair failed";
            return false;
        }
        // The far end stays open for the channel's lifetime, so the loop never
        // sees EOF and tears itself down mid-test.
        m_peer = pair[1];
        // Adopted rather than handed over as a descriptor: the double gets the
        // same non-blocking stream a real transport would, so it cannot pass the
        // channel something no PAL backend would ever produce.
        readStream = Ubiquitty::PAL::adoptByteStream(pair[0], Ubiquitty::PAL::ReadinessWaitStrategy::WaitFirst);
        if (!readStream) {
            error = "could not adopt the socketpair endpoint";
            return false;
        }
        writeStream = readStream->duplicateAsPollableStream();
        if (!writeStream) {
            error = "could not duplicate the socketpair endpoint";
            readStream.reset();
            return false;
        }
        return true;
    }

    void closeTransport() override
    {
        if (m_peer >= 0) {
            ::close(m_peer);
            m_peer = -1;
        }
    }

private:
    int m_peer = -1;
};

// The lifetime half: a sender that has taken its reference to the waker — the
// exact state send() is in between the validity test and the wake — must keep
// that waker live, and its descriptor NUMBER un-recycled, across a complete
// stop() including teardown. Without the reference teardown destroys the
// endpoint mid-flight and the kernel is free to hand the number to an unrelated
// open(), so the wake lands in a stranger's file; and destroying the receiving
// end turns the same wake into SIGPIPE, which takes the process down.
//
// Only the read handle is observable — Waker::wake() owns the sending end — so
// the send half is asserted by calling wake() itself, which is what send()
// actually does rather than an imitation of it. A fault is its failure mode, so
// reaching the line after it is that assertion.
TEST(test_wake_pipe_survives_stop)
{
    SocketPairChannel channel;
    Collector observer;
    CHECK_MSG(channel.start(&observer), "wake-lifetime: start");
    CHECK_MSG(observer.waitConnected(2000), "wake-lifetime: onConnect");

    std::shared_ptr<Ubiquitty::PAL::Waker> waker = channel.loadWaker();
    CHECK_MSG(waker && waker->handle().isValid(), "wake-lifetime: sender holds a live waker");
    if (!waker || !waker->handle().isValid())
        return;
    const int pinnedRead = static_cast<Ubiquitty::PAL::UnixFileDescriptor&>(waker->handle()).get();

    channel.stop(); // full teardown: joins the I/O thread, releases the channel's reference

    CHECK_MSG(waker->handle().isValid(), "wake-lifetime: waker still live after stop");
    CHECK_MSG(::fcntl(pinnedRead, F_GETFD) != -1, "wake-lifetime: read end still open after stop");

    // Make the kernel hand out descriptors; the pinned number may not come back.
    std::vector<int> opened;
    bool didHandOutPinned = false;
    for (int i = 0; i < 64; ++i) {
        const int fileDescriptor = ::open("/dev/null", O_RDONLY);
        if (fileDescriptor < 0)
            break;
        if (fileDescriptor == pinnedRead)
            didHandOutPinned = true;
        opened.push_back(fileDescriptor);
    }
    CHECK_MSG(!didHandOutPinned, "wake-lifetime: stop() did not release the descriptor number for reuse");

    waker->wake();

    for (int fileDescriptor : opened)
        ::close(fileDescriptor);

    // Dropping the last reference is what finally closes it — cleanup is
    // single-owner, so no teardown path can close it twice.
    waker.reset();
    CHECK_MSG(::fcntl(pinnedRead, F_GETFD) == -1 && errno == EBADF, "wake-lifetime: waker closes with the last reference");
}

// The race half: senders on several threads against a concurrent, repeated
// stop(). The assertion that matters here is ThreadSanitizer's — unsynchronized
// access to the wake notifier is a data race whether or not a given run corrupts
// anything. Reaching the end without a fault also exercises idempotent stop.
TEST(test_send_races_stop)
{
    for (int round = 0; round < 16; ++round) {
        SocketPairChannel channel;
        Collector observer;
        if (!channel.start(&observer))
            break;
        observer.waitConnected(2000);

        std::atomic<bool> go { false };
        std::vector<std::thread> senders;
        for (int i = 0; i < 4; ++i) {
            senders.emplace_back([&channel, &go] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                const std::uint8_t byte = 0x41;
                for (int n = 0; n < 256; ++n)
                    channel.send({ &byte, 1 });
            });
        }
        std::thread stopper([&channel, &go] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            channel.stop();
            channel.stop(); // idempotent, and concurrent with senders still in flight
        });

        go.store(true, std::memory_order_release);
        for (std::thread& sender : senders)
            sender.join();
        stopper.join();
    }
    CHECK_MSG(true, "send-races-stop: senders and repeated stop ran to completion without a fault");
}

} // namespace

// A slow peer holds a flush open, and while it is open the sender keeps
// sending. Two things must hold: every byte arrives exactly once and in order,
// and what the channel retains tracks the backlog rather than the total.
//
// A flush that advances an offset and reclaims nothing until the buffer drains
// completely lets a producer that never lets it drain — a long paste into a
// slow ssh link — grow the buffer by every byte ever sent, while the 4 MiB cap
// counts only the unsent part and never fires.
// A suspended reader waits rather than spins.
//
// Two things stop the bytes, and either alone would: the poll set drops its
// readable interest, and the read is guarded. Only one of them stops the CPU.
//
// A socket with data waiting is readable forever. A loop that keeps asking to
// be told about it and then declines to read gets told again immediately, on
// every iteration, for as long as the data sits there — a core at full tilt
// while the consumer it is waiting for tries to catch up, which is the opposite
// of pacing. Dropping the interest is what makes the wait a wait.
//
// Measured as process CPU across a wall interval, because that is the whole
// claim: the bytes not arriving is the other case's business.
TEST(test_a_suspended_reader_waits_rather_than_spins)
{
    SocketPairChannel channel;
    Collector observer;
    CHECK_MSG(channel.start(&observer), "spin: start");
    CHECK_MSG(observer.waitConnected(2000), "spin: onConnect");

    channel.suspendReading();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Data left waiting, which is what makes the socket readable and the poll
    // return. Without it a suspended channel would idle either way and the case
    // would prove nothing.
    const std::string waiting(2048, 'z');
    CHECK(::write(channel.peer(), waiting.data(), waiting.size()) == static_cast<ssize_t>(waiting.size()));

    const auto processCPUSeconds = [] {
        struct rusage usage {};
        ::getrusage(RUSAGE_SELF, &usage);
        return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6
            + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    };
    constexpr double wallSeconds = 1.0;
    const double before = processCPUSeconds();
    std::this_thread::sleep_for(std::chrono::duration<double>(wallSeconds));
    const double burned = processCPUSeconds() - before;

    // A spinning loop burns a whole core, so it reads at or above the wall
    // interval. The bar is a quarter of that: high enough that other threads in
    // this process cannot trip it, low enough that a spin cannot hide under it.
    CHECK_MSG(burned < wallSeconds / 4,
        "a suspended channel burned a core waiting — its poll still asks to be told about the data");

    CHECK_MSG(observer.receivedBytes() == 0, "a suspended channel delivered the data it was spinning on");
    channel.resumeReading();
    CHECK_MSG(observer.waitReceivedBytes(waiting.size(), 5000), "the data did not arrive after the resume");
    channel.stop();
}

// A consumer that stops reading stops the carrier.
//
// `onData` returns void, so a consumer cannot refuse what it is handed: it
// stages the chunk and the I/O thread goes straight back to reading more. A
// consumer slower than the transport therefore grows a queue that nothing
// bounds, and the flood pipeline's ring — which parks its reader when full —
// has no equivalent here.
//
// Suspending is how it says stop: the carrier goes unread, its receive buffer
// fills, and the peer's own writes block. What this holds the channel to is
// that the bytes stop arriving while it is suspended and start again after,
// with nothing lost in between — a carrier that dropped what it did not read
// would satisfy the first half and corrupt the stream.
TEST(test_a_suspended_reader_stops_the_bytes_and_resumes_them_whole)
{
    SocketPairChannel channel;
    Collector observer;
    CHECK_MSG(channel.start(&observer), "suspend: start");
    CHECK_MSG(observer.waitConnected(2000), "suspend: onConnect");

    // Enough to outlast one read, so a channel that ignored the suspend would
    // deliver visibly more than the little written before it.
    const std::string firstWrite(4096, 'a');
    CHECK(::write(channel.peer(), firstWrite.data(), firstWrite.size()) > 0);
    CHECK_MSG(observer.waitReceivedBytes(firstWrite.size(), 3000), "the channel has to deliver before it is suspended");

    channel.suspendReading();
    // Settled before the depth is read: the call cannot retract a read already
    // in flight on the I/O thread, so what it promises is that no read STARTS
    // after it — which is the steady state, and what a consumer pacing itself
    // is relying on.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const std::size_t deliveredWhenSuspended = observer.receivedBytes();

    // Written while suspended, and sized to fit the socket's own buffer. A
    // larger write blocks here — which is the mechanism working, the peer
    // stalled by a reader that stopped — but a test that blocks its own thread
    // proves it by hanging, so the write stays inside what the kernel holds.
    const std::string whileSuspended(2048, 'b');
    CHECK(::write(channel.peer(), whileSuspended.data(), whileSuspended.size())
        == static_cast<ssize_t>(whileSuspended.size()));

    // Long enough that an unsuspended channel would have drained it several
    // times over.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_MSG(observer.receivedBytes() == deliveredWhenSuspended,
        "a suspended channel delivered bytes anyway");

    channel.resumeReading();
    CHECK_MSG(observer.waitReceivedBytes(firstWrite.size() + whileSuspended.size(), 5000),
        "a resumed channel did not deliver what arrived while it was suspended");

    // And in order, with nothing dropped: the bytes written while suspended are
    // all there, after the ones written before.
    const std::string received = observer.receivedString();
    CHECK_MSG(received.size() == firstWrite.size() + whileSuspended.size(), "the resumed stream is the wrong length");
    CHECK_MSG(received.compare(0, firstWrite.size(), firstWrite) == 0, "the bytes before the suspend did not survive it");
    CHECK_MSG(received.compare(firstWrite.size(), whileSuspended.size(), whileSuspended) == 0,
        "the bytes written while suspended did not arrive whole");
    channel.stop();
}

// A channel over a byte stream survives a peer that stopped reading; one over
// frames does not, and should not.
//
// Framing is the whole difference. Bytes missing from the middle of a framed
// stream do not make a shorter frame — they desynchronise everything after, so
// nothing after the gap can be read and the channel cannot continue. A raw byte
// stream has no framing to desynchronise, and ending it takes down a terminal
// session over a child that stopped reading its input, where a real terminal
// answers by making the writer wait.
//
// The peer here never reads. Both channels fill their queue to the cap; only
// one of them is supposed to still be there afterwards.
TEST(test_a_wedged_peer_ends_a_framed_channel_and_spares_a_byte_stream)
{
    // Past the four-mebibyte outbound cap, in pieces, so the queue reaches it
    // while the peer reads nothing.
    constexpr std::size_t pieceBytes = 512u * 1024u;
    constexpr int pieceCount = 12;

    {
        SocketPairChannel framed;
        Collector observer;
        CHECK_MSG(framed.start(&observer), "framed: start");
        CHECK_MSG(observer.waitConnected(2000), "framed: onConnect");
        const std::vector<std::uint8_t> piece(pieceBytes, 0x5A);
        for (int index = 0; index < pieceCount; ++index)
            framed.send(piece);
        CHECK_MSG(observer.waitClosed(5000), "a framed channel whose queue overflows has to end");
        CHECK_MSG(framed.refusedBytes() == 0, "a channel that ends refuses nothing — it ends");
    }

    {
        SocketPairChannel byteStream { PollChannel::OverflowPolicy::RefuseWrite };
        Collector observer;
        CHECK_MSG(byteStream.start(&observer), "byte stream: start");
        CHECK_MSG(observer.waitConnected(2000), "byte stream: onConnect");
        const std::vector<std::uint8_t> piece(pieceBytes, 0x5A);
        for (int index = 0; index < pieceCount; ++index)
            byteStream.send(piece);

        // Long enough that the framed channel above would have closed by now.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline && byteStream.refusedBytes() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        CHECK_MSG(byteStream.refusedBytes() > 0, "the queue has to reach the cap for the refusal to be exercised");
        CHECK_MSG(!observer.sawClose(), "a byte-stream channel whose queue overflows must not end");
        CHECK_MSG(byteStream.health() != Health::Failed,
            "a byte-stream channel whose queue overflows must not report failure");

        // And it still works: the peer reads and the queue drains. A channel
        // that survived but stopped carrying would pass every check above and
        // be no better than one that ended.
        //
        // Only when it survived: draining a channel that already closed waits
        // out its own deadline for bytes nothing will send, which turns a
        // failure into a hang.
        if (observer.sawClose()) {
            byteStream.stop();
            return;
        }
        std::array<std::uint8_t, 65536> bite;
        std::size_t drained = 0;
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (drained < pieceBytes * 2 && std::chrono::steady_clock::now() < drainDeadline) {
            const ssize_t n = ::read(byteStream.peer(), bite.data(), bite.size());
            if (n > 0) {
                drained += static_cast<std::size_t>(n);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_MSG(drained > 0, "the peer read nothing, so the channel was not carrying before the refusal either");
    }
}

TEST(test_slow_peer_keeps_order_and_reclaims)
{
    SocketPairChannel channel;
    Collector observer;
    CHECK_MSG(channel.start(&observer), "slow-peer: start");
    CHECK_MSG(observer.waitConnected(2000), "slow-peer: onConnect");

    // A burst far larger than any socket buffer, so the first flush stops with
    // a remainder, followed by a trickle that keeps the queue from ever
    // reaching empty. That is the shape a long paste into a slow link takes,
    // and it is the only shape in which the delivered prefix accumulates: a
    // queue that drains completely resets it.
    constexpr std::size_t burstBytes = 2u * 1024u * 1024u;
    constexpr std::size_t trickleBytes = 1024;
    constexpr int trickleCount = 256;
    constexpr std::size_t totalBytes = burstBytes + trickleBytes * trickleCount;
    static_assert(totalBytes < 4u * 1024u * 1024u, "the burst must stay under the outbound cap");

    // Every byte carries the index of the 64 KiB block it belongs to, so the
    // reader can prove order rather than volume.
    constexpr std::size_t blockBytes = 64u * 1024u;
    const auto stampFor = [](std::size_t offset) { return static_cast<std::uint8_t>((offset / blockBytes) & 0xFF); };

    std::vector<std::uint8_t> burst(burstBytes);
    for (std::size_t offset = 0; offset < burstBytes; ++offset)
        burst[offset] = stampFor(offset);
    channel.send(burst);

    std::thread trickler([&] {
        for (int index = 0; index < trickleCount; ++index) {
            std::vector<std::uint8_t> chunk(trickleBytes);
            for (std::size_t offset = 0; offset < trickleBytes; ++offset)
                chunk[offset] = stampFor(burstBytes + static_cast<std::size_t>(index) * trickleBytes + offset);
            channel.send(chunk);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    // Read slowly, so the queue stays behind for the whole run.
    std::vector<std::uint8_t> received;
    received.reserve(totalBytes);
    std::size_t retainedNearEnd = 0;
    bool didSample = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::array<std::uint8_t, 8192> bite;
    while (received.size() < totalBytes && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(channel.peer(), bite.data(), bite.size());
        if (n > 0) {
            received.insert(received.end(), bite.data(), bite.data() + n);
            // Sampled once most of the stream is out, which is the only moment
            // the two forms differ: before that both hold everything, because
            // nothing has been written yet.
            if (!didSample && received.size() >= totalBytes * 3 / 4) {
                retainedNearEnd = Ubiquitty::Transport::OutboundRetentionAccess::retainedBytes(channel);
                didSample = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        break;
    }
    trickler.join();
    CHECK_MSG(received.size() == totalBytes, "slow-peer: every byte arrived");

    bool isInOrder = received.size() == totalBytes;
    for (std::size_t offset = 0; isInOrder && offset < received.size(); ++offset)
        isInOrder = received[offset] == stampFor(offset);
    CHECK_MSG(isInOrder, "slow-peer: the stream is in order, with nothing lost or repeated");

    // Three quarters delivered: a queue that reclaims its delivered prefix
    // holds about the last quarter, and one that does not holds all of it.
    CHECK_MSG(didSample, "slow-peer: retention was didSample");
    CHECK_MSG(retainedNearEnd < totalBytes / 2,
        "slow-peer: the buffer holds the backlog, not every byte ever sent");

    channel.stop();
}

UBIQUITTY_TEST_MAIN()
