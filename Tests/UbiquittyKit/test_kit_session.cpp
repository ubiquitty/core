// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "PAL/Socket.h"
#include "ScreenPlaneFixture.h"

// test_kit_session.cpp — the UbiquittyKit::Session facade, LOCAL origin,
// end-to-end through the real carrier: a socketpair stands in for the PTY
// master (the channel polls the fd exactly as it would a PTY), the test holds
// the far end as "the child". Proves the byte path (fd -> poll loop ->
// dispatcher -> Terminal grid -> onFrame), the input path (sendInput -> fd),
// the health transitions, and the exit path (peer close -> onExit).

#include "UbiquittyKit/UbiquittyKit.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Transport/Channel.h"
#include "Core/Transport/DelegatedChannel.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

UBIQUITTY_KIT_REQUIRE(1, 0, 0);

namespace {

// A thread-safe session-thread stand-in: the channel's I/O thread posts here;
// the test drains on the main thread (the "session thread").
class QueueDispatcher final : public Ubiquitty::Session::Dispatcher {
public:
    void post(Ubiquitty::Session::PostedWork work) override
    {
        {
            std::scoped_lock lock(m_mutex);
            m_queue.push_back(std::move(work));
        }
        m_signal.notify_one();
    }
    void postDelayed(double, Ubiquitty::Session::PostedWork work) override { post(std::move(work)); }

    // Run queued work until `done` is true or the deadline passes.
    bool drainUntil(const std::function<bool()>& done, double seconds = 5.0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!done()) {
            Ubiquitty::Session::PostedWork work;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_queue.empty() && m_signal.wait_until(lock, deadline) == std::cv_status::timeout && m_queue.empty())
                    return done();
                if (m_queue.empty())
                    continue;
                work = std::move(m_queue.front());
                m_queue.pop_front();
            }
            std::move(work)();
        }
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_signal;
    std::deque<Ubiquitty::Session::PostedWork> m_queue;
};

// A Stream over one end of a socketpair — the "PTY master" the facade wraps.
// The channel only polls fileDescriptor(); read/write ride the fd, so the byte
// methods are bare ::read/::write with no buffering or short-write retry of
// their own — what the case observes is what the kernel did.
class SocketPairStream final : public Ubiquitty::PAL::Stream {
public:
    explicit SocketPairStream(int fileDescriptor)
        : m_fileDescriptor(fileDescriptor)
    {
    }
    ~SocketPairStream() override
    {
        if (m_fileDescriptor >= 0)
            close(m_fileDescriptor);
    }

    int read(std::span<char> buffer) override { return static_cast<int>(::read(m_fileDescriptor, buffer.data(), buffer.size())); }
    int write(std::span<const char> bytes) override { return static_cast<int>(::write(m_fileDescriptor, bytes.data(), bytes.size())); }
    int writeAll(std::span<const char> bytes) override { return write(bytes); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    void setWindowSize(int columns, int rows, int, int) override
    {
        lastColumns = columns;
        lastRows = rows;
    }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] int fileDescriptor() const override { return m_fileDescriptor; }
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> duplicateAsPollableStream() override
    {
        const int duplicate = dup(m_fileDescriptor);
        if (duplicate < 0)
            return nullptr;
        return Ubiquitty::PAL::adoptByteStream(duplicate, Ubiquitty::PAL::ReadinessWaitStrategy::ProbeBeforeWait);
    }
    Ubiquitty::PAL::ProcessFacts& process() override { return m_facts; }

    int lastColumns = 0;
    int lastRows = 0;

private:
    class NoFacts final : public Ubiquitty::PAL::ProcessFacts {
    public:
        std::string foregroundName() override { return {}; }
        bool foregroundIsShell() override { return false; }
        std::optional<Ubiquitty::PAL::ProcessID> foregroundProcessGroup() override { return std::nullopt; }
        std::string foregroundCurrentDirectory() override { return {}; }
        std::string ttyName() override { return {}; }
        bool hasRunningChild() override { return false; }
        int gatherProcesses(std::span<Ubiquitty::PAL::Process::Record>) override { return 0; }
    };
    int m_fileDescriptor = -1;
    NoFacts m_facts;
};

class RecordingObserver final : public UbiquittyKit::Session::Observer {
public:
    void onFrame() override { frames++; }
    void onTitle(std::string_view utf8) override { lastTitle.assign(utf8); }
    void onBell() override { bells++; }
    void onExit() override { didExit = true; }
    void onHealthChanged(Ubiquitty::Session::Health health, const std::string&) override { lastHealth = health; }
    void onInputInDoubt() override { inputInDoubtReports++; }
    void onConfiguration(const Ubiquitty::Rendering::RenderConfiguration& configuration) override
    {
        configurations++;
        lastConfiguration = configuration;
    }

    int frames = 0;
    int inputInDoubtReports = 0;
    int configurations = 0;
    Ubiquitty::Rendering::RenderConfiguration lastConfiguration;
    int bells = 0;
    std::string lastTitle;
    bool didExit = false;
    Ubiquitty::Session::Health lastHealth = Ubiquitty::Session::Health::Connecting;
};

// Read a Terminal row as printable ASCII, trimmed at the first blank.
std::string rowText(Ubiquitty::Terminal& terminal, int row)
{
    std::string out;
    for (int column = 0; column < terminal.columns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
        const std::uint32_t codePoint = cell ? cell->codePoint : 0;
        if (codePoint < 33 || codePoint > 126)
            break;
        out.push_back(static_cast<char>(codePoint));
    }
    return out;
}

} // namespace

// ---- Remote origin: the facade over RemoteSessionCore, driven by fakes -----
namespace {
using FrameLog = std::vector<std::vector<std::uint8_t>>;
namespace Sync = Ubiquitty::Protocol::Sync;

constexpr std::uint64_t remoteFixtureDaemonInstanceID = 17;

class FakeRemoteChannel final : public Ubiquitty::Transport::Channel {
public:
    FakeRemoteChannel(Ubiquitty::Transport::Origin origin, FrameLog* sink)
        : m_origin(std::move(origin))
        , m_sink(sink)
    {
    }
    bool start(Ubiquitty::Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }
    void send(std::span<const std::uint8_t> data) override
    {
        if (m_sink)
            m_sink->emplace_back(data.begin(), data.end());
    }
    void stop() override { }
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const override { return m_origin; }
    [[nodiscard]] Ubiquitty::Transport::Health health() const override { return Ubiquitty::Transport::Health::Connected; }
    [[nodiscard]] std::string description() const override { return "fake-remote"; }

    void fireConnect()
    {
        if (m_observer)
            m_observer->onConnect();
    }
    void fireData(const std::vector<std::uint8_t>& bytes)
    {
        if (m_observer)
            m_observer->onData(bytes);
    }
    void fireClose(const std::string& reason)
    {
        if (m_observer)
            m_observer->onClose(reason);
    }

private:
    Ubiquitty::Transport::Origin m_origin;
    FrameLog* m_sink = nullptr;
    Ubiquitty::Transport::ChannelObserver* m_observer = nullptr;
};

class FakeRemoteFactory final : public Ubiquitty::Session::ChannelFactory {
public:
    void createChannel(Ubiquitty::Session::TransportMode, Done done) override
    {
        ++createCount;
        auto channel = std::make_unique<FakeRemoteChannel>(Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, std::string() }, &sentLog);
        last = channel.get();
        done(std::move(channel), false);
    }
    int createCount = 0;
    FakeRemoteChannel* last = nullptr;
    FrameLog sentLog;
};

std::vector<std::uint8_t> welcomeFrame(UbiquittyTest::HostScreen& host, const FrameLog& sent, std::uint32_t sessionID)
{
    host.receive(sent);
    std::vector<std::uint8_t> out;
    CHECK(host.plane().connection().popStreamFrame(out, host.plane().now()).has_value());
    Sync::EnvelopeError error {};
    const auto frame = Sync::decodeDatagram(out, error);
    const auto welcome = frame && frame->kind == Sync::FrameKind::Welcome && !frame->channel ? Sync::decodeWelcome(frame->body) : std::nullopt;
    CHECK(welcome && welcome->selection.target == Sync::AdmissionTarget::Session
        && welcome->selection.sessionID == sessionID
        && welcome->selection.daemonInstanceID == remoteFixtureDaemonInstanceID);
    // Only the real Welcome leaves now. The already queued PaneBind remains
    // on this same host connection until the client has buffered its input.
    return out;
}
std::vector<std::uint8_t> bellFrame(UbiquittyTest::HostScreen& host, const FrameLog& sent)
{
    host.receive(sent);
    CHECK(host.plane().source().appendEffect(Ubiquitty::Protocol::Sync::BellEffect {}, host.plane().connection().clientInstance()));
    return host.flush();
}
std::vector<std::uint8_t> exitFrame(UbiquittyTest::HostScreen& host)
{
    host.plane().connection().finish();
    return host.flush();
}

void runRemoteOriginChecks()
{
    // A: the happy path — Welcome, frame forwards, typed host Bye.
    {
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        QueueDispatcher dispatcher;
        RecordingObserver observer;
        FakeRemoteFactory factory;
        auto session = UbiquittyKit::Session::createRemote(*terminal, dispatcher, factory, observer, 0);
        CHECK(session->origin().transport == Ubiquitty::Transport::OriginKind::Daemon);
        CHECK(session->origin().isRemote());
        CHECK(session->health() == Ubiquitty::Session::Health::Connecting);

        session->start(80, 24);
        CHECK(dispatcher.drainUntil([&] { return factory.createCount == 1; }));
        factory.last->fireConnect();
        CHECK(dispatcher.drainUntil([&] { return !factory.sentLog.empty(); })); // Greet went out
        UbiquittyTest::HostScreen host(4242, remoteFixtureDaemonInstanceID);
        factory.last->fireData(welcomeFrame(host, factory.sentLog, 4242));
        CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Connected; }));
        CHECK(session->health() == Ubiquitty::Session::Health::Connected);

        const std::size_t framesBeforeInput = factory.sentLog.size();
        // Input is the pane's, and waits in the core for the host to bind
        // the pane: nothing of it is written yet.
        session->sendInput("x");
        CHECK(factory.sentLog.size() == framesBeforeInput);
        session->setSize(100, 30);
        CHECK(factory.sentLog.size() == framesBeforeInput + 1); // Window Facts can leave before the pane's Viewport.

        host.receive(factory.sentLog);
        const int framesBeforeCheckpoint = observer.frames;
        factory.last->fireData(host.publish());
        CHECK(dispatcher.drainUntil([&] { return observer.frames > framesBeforeCheckpoint; }));
        host.receive(factory.sentLog);
        const auto viewports = host.plane().connection().takeViewportReports();
        CHECK(std::ranges::any_of(viewports, [](const auto& report) {
            return report.viewport.columns == 100 && report.viewport.rows == 30;
        }));

        // The buffered input reached the fixture after the bind. Publish its
        // picture too, so an owed EchoContext cannot supersede the checkpoint
        // generation between its ACK and the effect below.
        const int framesBeforeInputPicture = observer.frames;
        factory.last->fireData(host.publish());
        CHECK(dispatcher.drainUntil([&] { return observer.frames > framesBeforeInputPicture; }));

        // Both pictures really applied above. This case tests typed facade
        // routing, not normal-client ACK timing: acknowledge the last picture
        // in the fixture. Timer/pacing is covered by the protocol fixtures.
        Sync::Ack ack;
        ack.panes.push_back({ .handle = host.plane().handle(), .generation = host.plane().source().generation() });
        std::vector<std::uint8_t> ackBody;
        Sync::encode(ackBody, ack);
        std::vector<std::uint8_t> ackFrame;
        CHECK(Sync::appendFrame(ackFrame, Sync::FrameKind::Ack, 0, ackBody));
        host.plane().connection().receiveStream(ackFrame, host.plane().now());

        factory.last->fireData(bellFrame(host, factory.sentLog));
        CHECK(dispatcher.drainUntil([&] { return observer.bells > 0; })); // onBell forward
        factory.last->fireData(exitFrame(host));
        CHECK(dispatcher.drainUntil([&] { return observer.didExit; })); // onExit forward
    }
    // B: the drop path — Reconnecting, then stop() is terminal.
    {
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        QueueDispatcher dispatcher;
        RecordingObserver observer;
        FakeRemoteFactory factory;
        auto session = UbiquittyKit::Session::createRemote(*terminal, dispatcher, factory, observer, 0);
        session->start(80, 24);
        CHECK(dispatcher.drainUntil([&] { return factory.createCount == 1; }));
        factory.last->fireConnect();
        CHECK(dispatcher.drainUntil([&] { return !factory.sentLog.empty(); }));
        UbiquittyTest::HostScreen host(7, remoteFixtureDaemonInstanceID);
        factory.last->fireData(welcomeFrame(host, factory.sentLog, 7));
        CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Connected; }));
        factory.last->fireClose("dropped");
        CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Reconnecting; }));
        session->stop();
        CHECK(session->health() == Ubiquitty::Session::Health::Failed);
    }
    // C: typing a reconnect cannot account for is in doubt, the embedder is
    // told, and nothing goes again until it answers.
    {
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        QueueDispatcher dispatcher;
        RecordingObserver observer;
        FakeRemoteFactory factory;
        auto session = UbiquittyKit::Session::createRemote(*terminal, dispatcher, factory, observer, 0);
        session->start(80, 24);
        CHECK(dispatcher.drainUntil([&] { return factory.createCount == 1; }));
        factory.last->fireConnect();
        CHECK(dispatcher.drainUntil([&] { return !factory.sentLog.empty(); }));
        UbiquittyTest::HostScreen first(7, remoteFixtureDaemonInstanceID);
        factory.last->fireData(welcomeFrame(first, factory.sentLog, 7));
        CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Connected; }));
        first.receive(factory.sentLog);
        const int framesBeforeCheckpoint = observer.frames;
        factory.last->fireData(first.publish());
        CHECK(dispatcher.drainUntil([&] { return observer.frames > framesBeforeCheckpoint; }));
        CHECK(session->inputInDoubtNotice().message.empty());
        // `x` goes on the carrier, which dies before the host reads it.
        session->sendInput("x");
        const std::size_t firstConnectionFrames = factory.sentLog.size();
        factory.last->fireClose("dropped");
        CHECK(dispatcher.drainUntil([&] { return factory.createCount == 2; }));
        factory.last->fireConnect();
        CHECK(dispatcher.drainUntil([&] { return factory.sentLog.size() > firstConnectionFrames; }));
        UbiquittyTest::HostScreen second(7, remoteFixtureDaemonInstanceID);
        second.passOver(firstConnectionFrames);
        factory.last->fireData(welcomeFrame(second, factory.sentLog, 7));
        CHECK(dispatcher.drainUntil([&] { return observer.inputInDoubtReports > 0; }));
        const Ubiquitty::Session::InputInDoubtNotice notice = session->inputInDoubtNotice();
        CHECK_STR_EQ(notice.message, "Some of what you typed may not have reached the host");
        CHECK_STR_EQ(notice.resendButton, "Send Again");
        session->discardInputInDoubt();
        CHECK(session->inputInDoubtNotice().message.empty());
        CHECK(observer.inputInDoubtReports >= 2);
        session->stop();
    }
}
} // namespace

TEST(kitSession)
{

    int pair[2] = { -1, -1 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const int childEnd = pair[1];

    auto terminalOwner = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::Terminal& terminal = *terminalOwner;
    QueueDispatcher dispatcher;
    RecordingObserver observer;

    auto stream = std::make_unique<SocketPairStream>(pair[0]);
    SocketPairStream* streamPointer = stream.get();
    auto session = UbiquittyKit::Session::createLocal(terminal, dispatcher, observer, std::move(stream));

    CHECK(session->origin().transport == Ubiquitty::Transport::OriginKind::Local);
    CHECK(!session->origin().isRemote());

    // Pre-start input is a no-op: nothing arrives at the far end.
    session->sendInput("early");
    char preStartBuffer[8] = {};
    CHECK(::recv(childEnd, preStartBuffer, sizeof preStartBuffer, MSG_DONTWAIT) < 0);

    session->start(80, 24);
    CHECK(terminal.columns == 80 && terminal.rows == 24);
    CHECK(streamPointer->lastColumns == 80 && streamPointer->lastRows == 24);
    CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Connected; }));
    CHECK(session->health() == Ubiquitty::Session::Health::Connected);

    // Child -> screen: bytes written at the far end land in the grid and fire
    // the frame hook.
    const char* banner = "kit-hello";
    CHECK(::write(childEnd, banner, std::strlen(banner)) == static_cast<ssize_t>(std::strlen(banner)));
    CHECK(dispatcher.drainUntil([&] { return observer.frames > 0; }));
    CHECK(rowText(terminal, 0) == "kit-hello");

    // Input -> child: sendInput arrives at the far end.
    session->sendInput("ls\r");
    char inputBuffer[16] = {};
    ssize_t inputLength = 0;
    for (int attempt = 0; attempt < 50 && inputLength <= 0; ++attempt) {
        inputLength = ::recv(childEnd, inputBuffer, sizeof inputBuffer, MSG_DONTWAIT);
        if (inputLength <= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(inputLength == 3 && std::memcmp(inputBuffer, "ls\r", 3) == 0);

    // Resize reaches both the grid and the "PTY".
    session->setSize(100, 30);
    CHECK(terminal.columns == 100 && terminal.rows == 30);
    CHECK(streamPointer->lastColumns == 100 && streamPointer->lastRows == 30);

    // Title + bell reach the same hooks as a remote origin (origin parity):
    // an OSC 2 title and a BEL through the byte stream.
    const char* titleAndBell = "\x1b]2;kit-title\x07\x07";
    CHECK(::write(childEnd, titleAndBell, std::strlen(titleAndBell)) > 0);
    CHECK(dispatcher.drainUntil([&] { return observer.bells > 0; }));
    CHECK(observer.lastTitle == "kit-title");

    // A query the child sends gets its answer back. The reply buffer is pull
    // model — whoever feeds the bytes owes the delivery — so a session that
    // parses without draining leaves the child waiting on an answer that was
    // written and never sent, which reads to the application as a hang rather
    // than a missing feature. DA1 is the probe every program sends at startup.
    const char* deviceAttributes = "\x1b[c";
    CHECK(::write(childEnd, deviceAttributes, std::strlen(deviceAttributes)) > 0);
    char replyBuffer[64] = {};
    ssize_t replyLength = 0;
    for (int attempt = 0; attempt < 50 && replyLength <= 0; ++attempt) {
        dispatcher.drainUntil([] { return false; }, 0.02);
        replyLength = ::recv(childEnd, replyBuffer, sizeof replyBuffer - 1, MSG_DONTWAIT);
        if (replyLength <= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_MSG(replyLength > 0, "the terminal's DA1 answer never reached the child");
    if (replyLength > 0) {
        const std::string reply(replyBuffer, static_cast<std::size_t>(replyLength));
        CHECK_MSG(reply.find("\x1b[?") != std::string::npos, "the answer was not a device-attributes report");
    }

    // A frontend's other half: a key event becomes bytes. The encoding set is
    // published by the model umbrella, and the Kit is a facade over that model
    // rather than a second surface — an embedder holding UbiquittyKit.h alone,
    // as this file does, reaches key encoding without a second include.
    Ubiquitty::Keys::KeyEvent up {};
    up.virtualKey = Ubiquitty::Keys::VirtualKey::UpArrow;
    up.keyCode = Ubiquitty::Keys::functionalKeyCodePoint(up.virtualKey);
    up.eventType = Ubiquitty::Keys::KeyEventType::Press;
    const Ubiquitty::Kitty::KeyFlagSet noKittyFlags {};
    char encodedKey[32];
    const int encodedLength = Ubiquitty::Keys::Keymap::encodeKeyEvent(up,
        Ubiquitty::Modes::CursorKeyMode::Normal, noKittyFlags, 0, 0, 0, 0, 0, encodedKey);
    CHECK(encodedLength == 3);
    CHECK(std::string(encodedKey, static_cast<std::size_t>(encodedLength)) == "\x1b[A");

    // Peer close = the child didExit: health falls, onExit fires.
    close(childEnd);
    CHECK(dispatcher.drainUntil([&] { return observer.didExit; }));
    CHECK(session->health() == Ubiquitty::Session::Health::Failed);

    session->stop();
    session.reset();
    terminalOwner.reset();

    // Stop-before-drain: work queued by the I/O thread must not publish
    // health or mutate the Terminal once stop() ran.
    {
        int stopPair[2] = { -1, -1 };
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, stopPair) == 0);
        auto stopTerminal = std::make_unique<Ubiquitty::Terminal>();
        QueueDispatcher stopDispatcher;
        RecordingObserver stopObserver;
        auto stopSession = UbiquittyKit::Session::createLocal(*stopTerminal, stopDispatcher, stopObserver, std::make_unique<SocketPairStream>(stopPair[0]));
        stopSession->start(80, 24);
        CHECK(::write(stopPair[1], "late-bytes", 10) == 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the I/O thread enqueue
        stopSession->stop(); // Before draining
        stopDispatcher.drainUntil([] { return false; }, 0.3);
        CHECK(stopObserver.frames == 0); // nothing published after stop
        CHECK(rowText(*stopTerminal, 0).empty()); // Terminal untouched
        close(stopPair[1]);
    }

    // destroy-before-drain: queued work outliving the Session runs harmlessly.
    {
        int destroyPair[2] = { -1, -1 };
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, destroyPair) == 0);
        auto destroyTerminal = std::make_unique<Ubiquitty::Terminal>();
        QueueDispatcher destroyDispatcher;
        RecordingObserver destroyObserver;
        auto destroySession = UbiquittyKit::Session::createLocal(*destroyTerminal, destroyDispatcher, destroyObserver, std::make_unique<SocketPairStream>(destroyPair[0]));
        destroySession->start(80, 24);
        CHECK(::write(destroyPair[1], "orphan-bytes", 12) == 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the I/O thread enqueue
        destroySession.reset(); // destroy without draining
        destroyDispatcher.drainUntil([] { return false; }, 0.3); // queued weak refs fail to lock
        CHECK(destroyObserver.frames == 0);
        close(destroyPair[1]);
    }
}

// The fake remote path is independently runnable without the real-fd case above.
TEST(kitRemoteSessionUsesTypedPaneEffects)
{
    runRemoteOriginChecks();
}

// The facade over an embedder-driven carrier — every fact the LOCAL case pins,
// re-pinned where the bytes cross a Driver instead of a file descriptor. This
// is the surface the origin() generalization can damage: the facade believes
// whatever the carrier stamps rather than hardcoding Local, so a session
// whose carrier says SSH must read as remote to every badge without the
// facade growing a case for it — and the reply pump, resize hook and teardown
// must reach the Driver where the local path reaches the PTY.
namespace {
struct DirectDriver {
    std::string sent;
    int stops = 0;
    static bool start(void* context)
    {
        (void)context;
        return true;
    }
    static void send(void* context, const std::uint8_t* bytes, std::size_t count)
    {
        static_cast<DirectDriver*>(context)->sent.append(reinterpret_cast<const char*>(bytes), count);
    }
    static void stop(void* context) { ++static_cast<DirectDriver*>(context)->stops; }

    Ubiquitty::Transport::DelegatedChannel::Driver driver()
    {
        return Ubiquitty::Transport::DelegatedChannel::Driver {
            .context = this,
            .start = &start,
            .send = &send,
            .stop = &stop,
        };
    }
};
} // namespace

TEST(kitSessionOverADelegatedCarrier)
{
    auto terminalOwner = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::Terminal& terminal = *terminalOwner;
    QueueDispatcher dispatcher;
    RecordingObserver observer;
    DirectDriver carrier;

    auto channel = std::make_unique<Ubiquitty::Transport::DelegatedChannel>(
        carrier.driver(),
        Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::SSH, "testhost" },
        "ssh testhost");
    Ubiquitty::Transport::DelegatedChannel* channelPointer = channel.get();

    int resizedColumns = 0;
    int resizedRows = 0;
    auto session = UbiquittyKit::Session::createDirect(terminal, dispatcher, observer, std::move(channel),
        [&](int columns, int rows) {
            resizedColumns = columns;
            resizedRows = rows;
        });

    // The carrier's own stamp, not a hardcoded origin: an SSH-stamped direct
    // session reads as remote, where the facade once answered Local for
    // every channel it held.
    CHECK(session->origin().transport == Ubiquitty::Transport::OriginKind::SSH);
    CHECK(session->origin().isRemote());

    session->start(80, 24);
    CHECK(terminal.columns == 80 && terminal.rows == 24);
    channelPointer->notifyConnect();
    CHECK(dispatcher.drainUntil([&] { return observer.lastHealth == Ubiquitty::Session::Health::Connected; }));

    // Carrier -> screen: delivered bytes land in the grid and fire the frame
    // hook, the same route the local case pins for a PTY.
    const char* banner = "direct-hello";
    channelPointer->deliver({ reinterpret_cast<const std::uint8_t*>(banner), std::strlen(banner) });
    CHECK(dispatcher.drainUntil([&] { return observer.frames > 0; }));
    CHECK(rowText(terminal, 0) == "direct-hello");

    // Input -> carrier: sendInput reaches the Driver's send.
    session->sendInput("ls\r");
    CHECK(dispatcher.drainUntil([&] { return carrier.sent.find("ls\r") != std::string::npos; }));

    // A query's answer leaves through the Driver: DA1 in, the reply
    // pump owes the carrier the report. This is the reply-flush the LocalState
    // generalization must not strand — a parse without a delivery reads to
    // the far application as a hang.
    carrier.sent.clear();
    const char* deviceAttributes = "\x1b[c";
    channelPointer->deliver({ reinterpret_cast<const std::uint8_t*>(deviceAttributes), std::strlen(deviceAttributes) });
    CHECK(dispatcher.drainUntil([&] { return carrier.sent.find("\x1b[?") != std::string::npos; }));

    // Resize reaches the grid and the embedder's hook, which is the Driver
    // world's setWindowSize.
    session->setSize(100, 30);
    CHECK(terminal.columns == 100 && terminal.rows == 30);
    CHECK(resizedColumns == 100 && resizedRows == 30);

    // Teardown quiesces the carrier exactly once.
    session->stop();
    CHECK_EQ(carrier.stops, 1);
}

// A Kit session follows the one configuration door. The embedder hands Core a
// document; a second document, configured while two sessions are live, reaches
// both — their terminals re-seeded and their observers handed the renderer's
// configuration — through that one call, and a session that has ended hears
// nothing.
TEST(kitSessionsFollowTheConfigurationDocument)
{
    namespace Preferences = Ubiquitty::Preferences;
    CHECK(Preferences::configure("[cursor]\nshape = \"underline\"\n[underline]\nthickness = 2.0\n", Preferences::ParseMode::Strict).isApplied);

    QueueDispatcher dispatcher;
    Ubiquitty::Terminal terminals[2];
    RecordingObserver observers[2];
    std::unique_ptr<UbiquittyKit::Session> sessions[2];
    for (int index = 0; index < 2; ++index) {
        Ubiquitty::hardReset(terminals[index]);
        Ubiquitty::resize(terminals[index], 24, 80);
        (void)Ubiquitty::Runtime::seedTerminalConfiguration(terminals[index], Preferences::values());
        CHECK(terminals[index].cursorShapePref == static_cast<std::uint8_t>(Preferences::CursorShape::Underline));
        sessions[index] = UbiquittyKit::Session::createDirect(terminals[index], dispatcher, observers[index],
            std::make_unique<FakeRemoteChannel>(Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Local, std::string() }, nullptr),
            nullptr);
        sessions[index]->start(80, 24);
    }

    const Preferences::ConfigureResult reload = Preferences::configure("[cursor]\n"
                                                                       "shape = \"block\"\n"
                                                                       "blink = false\n"
                                                                       "[underline]\n"
                                                                       "thickness = 3.0\n"
                                                                       "[terminal]\n"
                                                                       "ambiguous_width = 2\n",
        Preferences::ParseMode::Strict);
    CHECK(reload.isApplied);
    CHECK(dispatcher.drainUntil([&] { return observers[0].configurations == 1 && observers[1].configurations == 1; }));
    for (int index = 0; index < 2; ++index) {
        CHECK_EQ(observers[index].configurations, 1);
        CHECK_MSG(observers[index].lastConfiguration.underlineThicknessScale == 3.0, "the renderer is handed the new document's fold");
        CHECK_MSG(terminals[index].cursorShapePref == static_cast<std::uint8_t>(Preferences::CursorShape::Block), "the terminal is re-seeded");
        CHECK(!terminals[index].cursorBlinkPref);
        CHECK(terminals[index].ambiguousWidth == Ubiquitty::Unicode::Width::ambiguousWidthFromCells(2));
        CHECK_MSG(observers[index].frames > 0, "a frame follows the change");
    }

    // An ended session is unsubscribed: the next document reaches the other
    // one only.
    sessions[1].reset();
    CHECK(Preferences::configure("", Preferences::ParseMode::Strict).isApplied);
    CHECK(dispatcher.drainUntil([&] { return observers[0].configurations == 2; }));
    (void)dispatcher.drainUntil([] { return false; }, 0.1);
    CHECK_EQ(observers[1].configurations, 1);
    CHECK_MSG(observers[0].lastConfiguration.underlineThicknessScale == 1.0, "an empty document is the default table");
    sessions[0].reset();
}

UBIQUITTY_TEST_MAIN()
