// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_ingest_loop.cpp — the portable ingest loop and the session that owns
// it (Core/Runtime/IngestLoop.h through Core/Runtime/TerminalSession.h),
// driven over a real PTY, and the flood pipeline over a prefilled transport:
// the parked wait and its waker, the coalesced delivery, the pipeline's
// reader/ring/parser, input backpressure, the posted-command paths and the
// stop/restart pair. The loop's tuning constants are the Metal arm's measured
// numbers; nothing here asserts timing beyond one coarse ceiling (a held
// frame announced within 100 ms, where the defect it pins takes 500), only
// that bytes arrive, events coalesce, and stops end the thread.

#include "Core/Runtime/TerminalSession.h"

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/Title/WalkPacing.h"
#include "Core/VT/ParseState.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

using namespace Ubiquitty::Runtime;

namespace Ubiquitty::PAL {

// A pollable endpoint over one owned regular-file descriptor. A regular file
// answers every read with bytes until end-of-file — never Again — so a pass
// over one is capacity-limited by construction, whatever else the machine is
// running: the flood pipeline's election needs no writer to keep up.
//
// The shape caps what one read returns, as a pty does: a macOS pty answers
// every read with at most 1024 bytes however long the reader waits, where a
// Linux pty read taken too soon catches the line discipline mid-copy and a
// read taken once the slab has landed takes all of it.
struct ReadShape {
    // Most bytes a read returns while the slab is still landing; 0 for no cap.
    int quantumBytes = 0;
    // How long after the previous read returned the slab has landed; zero
    // keeps the quantum on every read.
    std::chrono::microseconds landing { 0 };
    // Most bytes a read returns once the slab has landed; 0 for no cap.
    int slabBytes = 0;
};

class PrefilledEndpoint final : public PollableByteStream {
public:
    explicit PrefilledEndpoint(UnixFileDescriptor descriptor, ReadShape shape = {})
        : m_descriptor(std::move(descriptor))
        , m_shape(shape)
    {
    }

    int read(std::span<char> buffer) override
    {
        std::size_t length = buffer.size();
        if (m_shape.quantumBytes > 0) {
            const bool hasLanded = m_shape.landing.count() > 0 && std::chrono::steady_clock::now() - m_lastReturn >= m_shape.landing;
            if (!hasLanded)
                length = std::min(length, static_cast<std::size_t>(m_shape.quantumBytes));
            else if (m_shape.slabBytes > 0)
                length = std::min(length, static_cast<std::size_t>(m_shape.slabBytes));
        }
        const ssize_t count = ::read(m_descriptor.get(), buffer.data(), length);
        m_lastReturn = std::chrono::steady_clock::now();
        if (count > 0)
            return static_cast<int>(count);
        if (count == 0)
            return std::to_underlying(StreamStatus::EndOfFile);
        return std::to_underlying(StreamStatus::Fatal);
    }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] FileDescriptor& handle() override { return m_descriptor; }

    // Duplicates share the file offset, so the loop's endpoint, the reader's
    // endpoint and the Stream all consume one sequence, as the PTY's do.
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        UnixFileDescriptor duplicate(::dup(m_descriptor.get()));
        if (!duplicate)
            return nullptr;
        return std::make_unique<PrefilledEndpoint>(std::move(duplicate), m_shape);
    }

private:
    UnixFileDescriptor m_descriptor;
    ReadShape m_shape;
    std::chrono::steady_clock::time_point m_lastReturn {};
};

// A session transport whose whole stream is on disk before ingest starts.
class PrefilledStream final : public Stream, private ProcessFacts {
public:
    explicit PrefilledStream(UnixFileDescriptor descriptor, ReadShape shape = {})
        : m_rawDescriptor(descriptor.get())
        , m_endpoint(std::move(descriptor), shape)
    {
    }

    int read(std::span<char> buffer) override { return m_endpoint.read(buffer); }
    int write(std::span<const char> bytes) override { return m_endpoint.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_endpoint.writeAll(bytes); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int fileDescriptor() const override { return m_rawDescriptor; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override { return m_endpoint.duplicateAsPollableStream(); }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return false; }
    std::optional<ProcessID> foregroundProcessGroup() override { return {}; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }

private:
    int m_rawDescriptor;
    PrefilledEndpoint m_endpoint;
};

// A transport whose writer has paused rather than gone: a prefilled file
// read to its end answers Again, and readiness is asked of a pipe holding one
// byte nobody reads, so it always answers readable and the flood reader never
// reaches its quiet budget. The burst stays open for as long as the test runs.
class PausedEndpoint final : public PollableByteStream {
public:
    PausedEndpoint(UnixFileDescriptor file, UnixFileDescriptor readiness)
        : m_file(std::move(file))
        , m_readiness(std::move(readiness))
    {
    }

    int read(std::span<char> buffer) override
    {
        const ssize_t count = ::read(m_file.get(), buffer.data(), buffer.size());
        if (count > 0)
            return static_cast<int>(count);
        if (count == 0)
            return std::to_underlying(StreamStatus::Again);
        return std::to_underlying(StreamStatus::Fatal);
    }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] FileDescriptor& handle() override { return m_readiness; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        UnixFileDescriptor file(::dup(m_file.get()));
        UnixFileDescriptor readiness(::dup(m_readiness.get()));
        if (!file || !readiness)
            return nullptr;
        return std::make_unique<PausedEndpoint>(std::move(file), std::move(readiness));
    }

private:
    UnixFileDescriptor m_file;
    UnixFileDescriptor m_readiness;
};

class PausedStream final : public Stream, private ProcessFacts {
public:
    PausedStream(UnixFileDescriptor file, UnixFileDescriptor readiness)
        : m_rawReadiness(readiness.get())
        , m_endpoint(std::move(file), std::move(readiness))
    {
    }

    int read(std::span<char> buffer) override { return m_endpoint.read(buffer); }
    int write(std::span<const char> bytes) override { return m_endpoint.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_endpoint.writeAll(bytes); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int fileDescriptor() const override { return m_rawReadiness; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override { return m_endpoint.duplicateAsPollableStream(); }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return false; }
    std::optional<ProcessID> foregroundProcessGroup() override { return {}; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }

private:
    int m_rawReadiness;
    PausedEndpoint m_endpoint;
};

// A pollable endpoint over the read end of a pipe the test writes into: a
// writer that pauses between writes, read nonblocking, so an empty pipe
// answers Again and readiness is the pipe's own.
class PipeEndpoint final : public PollableByteStream {
public:
    explicit PipeEndpoint(UnixFileDescriptor descriptor)
        : m_descriptor(std::move(descriptor))
    {
    }

    int read(std::span<char> buffer) override
    {
        const ssize_t count = ::read(m_descriptor.get(), buffer.data(), buffer.size());
        if (count > 0)
            return static_cast<int>(count);
        if (count == 0)
            return std::to_underlying(StreamStatus::EndOfFile);
        return errno == EAGAIN ? std::to_underlying(StreamStatus::Again) : std::to_underlying(StreamStatus::Fatal);
    }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] FileDescriptor& handle() override { return m_descriptor; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        UnixFileDescriptor duplicate(::dup(m_descriptor.get()));
        if (!duplicate)
            return nullptr;
        return std::make_unique<PipeEndpoint>(std::move(duplicate));
    }

private:
    UnixFileDescriptor m_descriptor;
};

// A session transport over a PipeEndpoint. Its foreground is the group a case
// sets, which the case may move while the loop runs, and its tree announces
// the edges the case raises once a case says it does.
class PipeStream final : public Stream, private ProcessFacts {
public:
    explicit PipeStream(UnixFileDescriptor descriptor)
        : m_rawDescriptor(descriptor.get())
        , m_endpoint(std::move(descriptor))
    {
    }

    ~PipeStream() override
    {
        if (m_releaseTreeContext)
            m_releaseTreeContext(m_treeContext);
    }

    /// The group holding the terminal; zero resolves none.
    std::atomic<ProcessID> foregroundGroup { 0 };
    /// The shell's group, which holds the terminal at a prompt; zero for none.
    ProcessID shellGroup = 0;
    /// Whether the tree announces its edges.
    bool isTreeTracked = false;

    /// Raises a fork, exec or exit edge, as the tracker's thread does.
    ///
    /// - Returns: Whether the session reads its foreground.
    bool raiseTreeEdge() { return m_treeNotify != nullptr && m_treeNotify(m_treeContext); }

    int read(std::span<char> buffer) override { return m_endpoint.read(buffer); }
    int write(std::span<const char> bytes) override { return m_endpoint.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_endpoint.writeAll(bytes); }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    [[nodiscard]] int lastError() const override { return 0; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int fileDescriptor() const override { return m_rawDescriptor; }
    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override { return m_endpoint.duplicateAsPollableStream(); }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return shellGroup != 0 && foregroundGroup.load(std::memory_order_relaxed) == shellGroup; }
    std::optional<ProcessID> foregroundProcessGroup() override
    {
        const ProcessID group = foregroundGroup.load(std::memory_order_relaxed);
        if (group == 0)
            return std::nullopt;
        return group;
    }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }
    void setTreeChangeNotifier(bool (*notify)(void*), void* context, void (*releaseContext)(void*)) override
    {
        if (m_releaseTreeContext)
            m_releaseTreeContext(m_treeContext);
        m_treeNotify = notify;
        m_treeContext = context;
        m_releaseTreeContext = releaseContext;
    }
    bool deliversTreeEdges() override { return isTreeTracked; }

private:
    int m_rawDescriptor;
    PipeEndpoint m_endpoint;
    bool (*m_treeNotify)(void*) = nullptr;
    void* m_treeContext = nullptr;
    void (*m_releaseTreeContext)(void*) = nullptr;
};

} // namespace Ubiquitty::PAL

namespace {

// Writes `bytes` to an unlinked temporary file and hands the session a
// transport positioned at its start.
bool adoptPrefilledStream(TerminalSession& session, std::string_view bytes)
{
    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/ubiquitty-flood-XXXXXX";
    const int descriptor = ::mkstemp(path.data());
    if (descriptor < 0)
        return false;
    ::unlink(path.c_str());
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count <= 0) {
            ::close(descriptor);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::lseek(descriptor, 0, SEEK_SET) != 0) {
        ::close(descriptor);
        return false;
    }
    auto adoption = session.adoptStream(std::make_unique<Ubiquitty::PAL::PrefilledStream>(Ubiquitty::PAL::UnixFileDescriptor(descriptor)));
    return adoption.has_value();
}

// The flood the pipeline cases share: past the drain pass capacity many
// times over, with a tail marker that only an in-order parse lands.
std::string floodStream()
{
    std::string flood;
    constexpr std::string_view line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    while (flood.size() < 6000000)
        flood += line;
    flood += "\nFLOOD-END\n";
    return flood;
}

constexpr std::uint32_t screenDirtyBit = 1u << 0;
constexpr std::uint32_t exitedBit = 1u << 1;

// Polls the session until the exited bit lands or the budget runs out,
// accumulating flags and drained bytes.
struct SessionWatch {
    std::uint32_t flags = 0;
    std::uint64_t drainedBytes = 0;
    bool exited = false;
};
SessionWatch watchUntilExit(TerminalSession& session, int budgetMilliseconds)
{
    SessionWatch watch;
    for (int waited = 0; waited < budgetMilliseconds; waited += 10) {
        watch.flags |= session.takeEventFlags();
        watch.drainedBytes += session.takeDrainedBytes();
        if (watch.flags & exitedBit) {
            watch.exited = true;
            break;
        }
        ::usleep(10 * 1000);
    }
    // The exit delivery may have raced the last take; sweep once more.
    watch.flags |= session.takeEventFlags();
    watch.drainedBytes += session.takeDrainedBytes();
    watch.exited = watch.exited || (watch.flags & exitedBit);
    return watch;
}

// Whether any visible row's text contains `needle`.
bool gridContains(TerminalSession& session, const std::string& needle)
{
    session.lockForRead();
    Ubiquitty::Terminal* term = session.terminal();
    bool found = false;
    if (term) {
        for (int row = 0; row < term->rows && !found; ++row) {
            std::string line;
            for (int column = 0; column < term->columns; ++column) {
                const Ubiquitty::Cell* cell = term->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
                if (cell && cell->codePoint >= 0x20 && cell->codePoint < 0x7F)
                    line.push_back(static_cast<char>(cell->codePoint));
            }
            found = line.find(needle) != std::string::npos;
        }
    }
    session.unlockForRead();
    return found;
}

struct SessionHandle {
    TerminalSession* session;
    explicit SessionHandle()
        : session(TerminalSession::make())
    {
    }
    ~SessionHandle()
    {
        session->close();
        releaseTerminalSession(session);
    }
};

} // namespace

TEST(testAnEmptyReadDeliversProcessStateChanges)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    const std::array<std::string, 2> arguments { "/bin/sleep", "40" };
    auto stream = Ubiquitty::PAL::openPTY(arguments);
    REQUIRE(stream != nullptr);
    bool isChildReady = false;
    for (int waited = 0; waited < 3000 && !isChildReady; waited += 10) {
        isChildReady = stream->process().foregroundName() == "sleep";
        ::usleep(10000);
    }
    REQUIRE(isChildReady);
    auto adoption = session.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    session.setForegroundObservation(ForegroundObservation::Unread);
    (void)session.takeEventFlags();
    REQUIRE(session.startIngest());
    session.setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);

    constexpr std::uint32_t processBit = 1u << static_cast<unsigned>(EventKind::ProcessStateChanged);
    ::usleep(650000);
    CHECK(session.processChain().empty());
    CHECK(!(session.takeEventFlags() & processBit));
    session.setForegroundObservation(ForegroundObservation::ProcessState);
    std::uint32_t flags = 0;
    for (int waited = 0; waited < 1500 && !(flags & processBit); waited += 10) {
        flags |= session.takeEventFlags();
        ::usleep(10000);
    }
    CHECK_MSG(session.processChain() == "-sleep", ("the empty read discovers the running process: " + session.processChain()).c_str());
    CHECK_MSG(flags & processBit, "the same empty read delivers the changed process state");
    CHECK(session.takeDrainedBytes() == 0);
}

namespace {

// The model's title, read under the session's lock.
std::string titleOf(TerminalSession& session)
{
    session.lockForRead();
    const Ubiquitty::Terminal* term = session.terminal();
    std::string title = term != nullptr ? term->titleUTF8.copyToString() : std::string();
    session.unlockForRead();
    return title;
}

} // namespace

// A program that set a title exits, and its shell takes the terminal back a
// moment after the exit edge, writing nothing and raising no edge of its own.
// The edge wakes the loop out of its wait, and the loop answers it once the
// tree has held still, so the probe reads the shell and the title leaves with
// its program, at either level that reads the foreground.
TEST(testAnExitEdgeIsAnsweredOnceTheShellHasTheTerminalBack)
{
    constexpr Ubiquitty::PAL::ProcessID shellGroup = 100;
    constexpr Ubiquitty::PAL::ProcessID programGroup = 200;
    for (const ForegroundObservation observation : { ForegroundObservation::TitleAndDirectory, ForegroundObservation::ProcessState }) {
        const char* const level = observation == ForegroundObservation::TitleAndDirectory ? "TitleAndDirectory" : "ProcessState";
        int pipeDescriptors[2] = { -1, -1 };
        REQUIRE(::pipe(pipeDescriptors) == 0);
        Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
        Ubiquitty::PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
        REQUIRE(::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) == 0);
        auto stream = std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd));
        stream->isTreeTracked = true;
        stream->shellGroup = shellGroup;
        stream->foregroundGroup = programGroup;
        Ubiquitty::PAL::PipeStream& transport = *stream;

        SessionHandle handle;
        TerminalSession& session = *handle.session;
        session.start(24, 80);
        REQUIRE(session.adoptStream(std::move(stream)).has_value());
        session.setForegroundObservation(observation);
        REQUIRE(session.startIngest());

        constexpr std::string_view setTitle = "\x1b]2;program title\x07";
        REQUIRE(::write(writeEnd.get(), setTitle.data(), setTitle.size()) == static_cast<ssize_t>(setTitle.size()));
        for (int waited = 0; waited < 2000 && titleOf(session) != "program title"; ++waited)
            ::usleep(1000);
        REQUIRE(titleOf(session) == "program title");
        // Past the floor and the settle of the edge the session starts owing,
        // so the exit edge is the one the loop answers.
        ::usleep((Ubiquitty::Title::edgeFloorMilliseconds + Ubiquitty::Title::edgeSettleMilliseconds) * 2000);

        CHECK_MSG(transport.raiseTreeEdge(), level);
        ::usleep(Ubiquitty::Title::edgeSettleMilliseconds * 1000 / 6);
        transport.foregroundGroup = shellGroup;
        bool isRetired = false;
        for (int waited = 0; waited < 2000 && !isRetired; ++waited) {
            isRetired = titleOf(session).empty();
            ::usleep(1000);
        }
        CHECK_MSG(isRetired, (std::string(level) + ": the exit edge was answered before the shell took the terminal back, and the title outlived its program").c_str());
        session.stopIngest();
    }
}

TEST(testIngestLoopDrainsALiveShell)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    // No transport yet: the loop refuses to start rather than parking on
    // nothing.
    CHECK(!session.startIngest());

    CHECK(session.launchShellCommand("printf 'ingest-marker\\n'"));
    CHECK(session.fileDescriptor() >= 0);

    // A command posted before the loop runs applies inline under the lock.
    session.setDesiredFocus(Ubiquitty::Modes::FocusState::Focused);

    CHECK(session.startIngest());
    CHECK(!session.startIngest()); // one loop per session

    const SessionWatch watch = watchUntilExit(session, 5000);
    CHECK_MSG(watch.exited, "the shell's exit reaches the flags as Exited");
    CHECK(watch.flags & screenDirtyBit);
    CHECK(watch.drainedBytes >= 13); // the marker and its newline, at least
    CHECK(gridContains(session, "ingest-marker"));

    session.stopIngest(); // idempotent over an already-ended loop
}

TEST(testReportedDirectorySuppressesQuietDirectorySample)
{
    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    runtime.init(parameters);

    const std::array<std::string, 2> arguments { "/bin/sleep", "30" };
    auto stream = Ubiquitty::PAL::openPTY(arguments);
    REQUIRE(stream != nullptr);
    const bool hasTreeEdges = stream->process().deliversTreeEdges();
    REQUIRE(runtime.adoptStream(std::move(stream)).has_value());
    if (!hasTreeEdges) {
        CHECK_EQ(runtime.directorySampleMilliseconds(), 0);
        return;
    }
    CHECK_EQ(runtime.directorySampleMilliseconds(), Ubiquitty::Title::walkFallbackMilliseconds);

    constexpr std::string_view report = "\x1b]7;file:///tmp\x07";
    const FeedResult result = runtime.feedChunkUpToSyncClear({ report.data(), report.size() });
    CHECK_EQ(result.consumed, report.size());
    const Ubiquitty::Terminal* terminal = runtime.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    REQUIRE(!terminal->reportedDirectory.empty());
    CHECK_EQ(runtime.directorySampleMilliseconds(), 0);

    Ubiquitty::setReportedDirectoryString(*runtime.borrowTermForRender(), {});
    CHECK_EQ(runtime.directorySampleMilliseconds(), Ubiquitty::Title::walkFallbackMilliseconds);
}

// The first visible row whose text contains `needle`, or -1.
int rowContaining(TerminalSession& session, const std::string& needle)
{
    session.lockForRead();
    Ubiquitty::Terminal* term = session.terminal();
    int found = -1;
    if (term) {
        for (int row = 0; row < term->rows && found < 0; ++row) {
            std::string line;
            for (int column = 0; column < term->columns; ++column) {
                const Ubiquitty::Cell* cell = term->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
                if (cell && cell->codePoint >= 0x20 && cell->codePoint < 0x7F)
                    line.push_back(static_cast<char>(cell->codePoint));
            }
            if (line.find(needle) != std::string::npos)
                found = row;
        }
    }
    session.unlockForRead();
    return found;
}

// A launch that holds for an opening screen starts its child at once — the
// child runs and writes — but the loop leaves that output unread until the
// screen is written, and the screen then lands ahead of it.
TEST(testOpeningHoldWritesTheScreenAheadOfTheChildsFirstOutput)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", "printf 'prompt-marker\\r\\n'; sleep 3" });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    command.setOpeningScreen(OpeningScreen::Held);
    REQUIRE(session.launch(command));
    REQUIRE(session.startIngest());

    // Long enough for the child to have written its line many times over.
    ::usleep(500000);
    CHECK(session.isOpeningHoldActive());
    CHECK_MSG(!gridContains(session, "prompt-marker"), "the child's output waits behind the hold");
    CHECK(session.takeDrainedBytes() == 0);

    const std::string screen = "welcome-marker\r\n";
    session.finishOpeningScreen(ByteSpan(screen.data(), screen.size()));
    CHECK(!session.isOpeningHoldActive());
    bool didArrive = false;
    for (int waited = 0; waited < 3000 && !didArrive; waited += 10) {
        didArrive = gridContains(session, "prompt-marker");
        ::usleep(10000);
    }
    CHECK_MSG(didArrive, "releasing the hold lets the child's output through");
    const int screenRow = rowContaining(session, "welcome-marker");
    const int promptRow = rowContaining(session, "prompt-marker");
    CHECK_EQ(screenRow, 0);
    CHECK_MSG(promptRow > screenRow, "the opening screen precedes the child's first output");
    session.stopIngest();
}

// A launch that held nothing ignores an opening screen: bytes dropped into a
// live stream would land wherever its cursor is.
TEST(testOpeningScreenWithoutAHoldIsIgnored)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    CHECK(session.launchShellCommand("sleep 1"));
    CHECK(!session.isOpeningHoldActive());
    const std::string screen = "stray-screen";
    session.finishOpeningScreen(ByteSpan(screen.data(), screen.size()));
    CHECK(!gridContains(session, "stray-screen"));
}

TEST(testIngestFloodPipelineCarriesABurst)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    // A burst past the drain pass capacity (1 MiB): capacity-limited passes
    // hand the pipeline the flood — reader thread, ring, bites — and the tail
    // marker proves the parsed stream stayed in order through the handoff and
    // the residue drain. Prefilled rather than piped from a child: a pass is
    // capacity-limited only while the writer stays ahead of its quiet budget,
    // which a child process cannot promise on a loaded machine.
    REQUIRE(adoptPrefilledStream(session, floodStream()));
    REQUIRE(session.startIngest());

    const SessionWatch watch = watchUntilExit(session, 30000);
    CHECK_MSG(watch.exited, "the flood ends and the end reaches the flags");
    CHECK_MSG(watch.drainedBytes >= 6000000, "every burst byte was drained through the loop");
    CHECK_MSG(session.floodBurstCount() >= 1, "the pipelined reader carried the burst, not the sequential fallback");
    CHECK(gridContains(session, "FLOOD-END"));
}

TEST(testFloodPipelineFeedsTheRawByteTap)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    // The tap is the ZMODEM detector's seam: it must observe the whole
    // stream, or a handshake that lands inside a pipelined burst is missed.
    // Fn-pointer tap, so the counter is a static the lambda can reach.
    static std::atomic<std::uint64_t> tappedBytes { 0 };
    tappedBytes.store(0);
    session.setRawByteTap(
        [](void*, std::string_view bytes) { tappedBytes.fetch_add(bytes.size(), std::memory_order_relaxed); },
        nullptr);

    REQUIRE(adoptPrefilledStream(session, floodStream()));
    REQUIRE(session.startIngest());

    const SessionWatch watch = watchUntilExit(session, 30000);
    CHECK_MSG(watch.exited, "the flood ends and the end reaches the flags");
    CHECK_MSG(watch.drainedBytes >= 6000000, "every burst byte was drained through the loop");
    CHECK_MSG(session.floodBurstCount() >= 1, "the burst went through the pipeline, so the tap crossed the handoff");
    CHECK_MSG(tappedBytes.load() >= 6000000, "the tap observed every drained byte, pipelined bites included");
}

TEST(testZModemDetectionHoldsIngestThenReleases)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    // The child prints the handshake to its stdout — the only path the ingest
    // loop reads; writing the master-side input would go to the shell's
    // stdin, where `sleep` never reads it. The shell's printf escapes the
    // control byte \030 as octal so the -A hex-escape ambiguity is avoided.
    // The frame tail that the hold drops is never re-sent here; instead the
    // second phase (post-sleep) stands in for the sender's retransmission
    // and must land only after the hold lifts.
    CHECK(session.launchShellCommand("printf 'before-marker\\r\\nrz\\r**\\030Aframe-stuff'; sleep 2; printf 'after-release\\r\\n'"));
    session.armFileTransferWatch();
    CHECK(session.startIngest());

    constexpr std::uint32_t transferBit = 1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::FileTransferDetected);
    std::uint32_t flags = 0;
    bool didDetect = false;
    for (int waited = 0; waited < 5000; waited += 10) {
        flags |= session.takeEventFlags();
        if (flags & transferBit) {
            didDetect = true;
            break;
        }
        ::usleep(10 * 1000);
    }
    CHECK_MSG(didDetect, "the preamble fires the FileTransferDetected event through the loop");
    // The detection is raised from the byte tap, which sees a chunk before the
    // pass that parses it and dirties the screen (TerminalRuntime::tapRawBytes
    // runs ahead of IngestLoop::deliver), so the two flags can land in separate
    // batches. Under a sanitized build on a loaded box they did, and the dirty
    // bit was asserted the instant the transfer bit arrived. It is waited for,
    // on the same budget, not assumed to have travelled with the detection.
    for (int waited = 0; !(flags & screenDirtyBit) && waited < 5000; waited += 10) {
        ::usleep(10 * 1000);
        flags |= session.takeEventFlags();
    }
    CHECK_MSG(flags & screenDirtyBit, "the pre-frame text parsed and dirtied the screen");

    const Ubiquitty::Network::Transfer::Detection detection = session.detectedFileTransfer();
    CHECK(detection);
    CHECK(detection.protocol == Ubiquitty::Network::Transfer::Protocol::ZModem);
    CHECK(detection.direction == Ubiquitty::Network::Transfer::Direction::Receive);
    CHECK(!session.isFileTransferWatchArmed()); // one shot: the frontend takes over
    CHECK(session.isFileTransferHoldActive()); // ingest gates held until answered

    // The preamble's own text parsed ("before-marker", the "rz\r" lead-in);
    // everything from the **\x18 lead on stayed out of the model.
    CHECK(gridContains(session, "before-marker"));
    CHECK(!gridContains(session, "frame-stuff"));

    // Answer the detection the way every frontend path does: re-arm. Arming
    // lifts the hold — the one release point a decline, a missing agent and a
    // finished transfer all funnel through — so a frontend cannot resume the
    // watch and leave the stream parked.
    session.armFileTransferWatch();
    CHECK(!session.isFileTransferHoldActive());
    CHECK(session.isFileTransferWatchArmed());
    bool didResume = false;
    for (int waited = 0; waited < 5000; waited += 10) {
        if (gridContains(session, "after-release")) {
            didResume = true;
            break;
        }
        ::usleep(10 * 1000);
    }
    CHECK_MSG(didResume, "releasing the hold resumes the stream through the same loop");

    // The bare release is for a caller that resumes without re-arming, and it
    // is idempotent: lifting a hold nobody set changes nothing.
    session.releaseFileTransferHold();
    CHECK(!session.isFileTransferHoldActive());
    CHECK(session.isFileTransferWatchArmed()); // the re-arm above still stands

    session.stopIngest();
}

TEST(testThrottledPaneKeepsAFloodOnTheSequentialPasses)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    // A throttled machine must not busy-wait a core: the flood pipeline's
    // reader spins a µs probe band per refill gap, so the drain gate that
    // collapses the sequential budgets must keep the pipeline out too. The
    // bytes still all arrive — at the parked, poll-paced rate.
    session.setMachineThrottle(MachineThrottle::Throttled);
    CHECK(session.launchShellCommand(
        "yes 0123456789abcdefghijklmnopqrstuvwxyz | head -c 3000000; printf '\\nFLOOD-END\\n'"));
    CHECK(session.startIngest());

    const SessionWatch watch = watchUntilExit(session, 30000);
    CHECK_MSG(watch.exited, "the flood ends and the end reaches the flags");
    CHECK_MSG(watch.drainedBytes >= 3000000, "a throttled pane still drains the whole flood");
    CHECK_EQ(session.floodBurstCount(), 0u);
    CHECK(gridContains(session, "FLOOD-END"));
}

// A model lock and nothing else: the pipeline case below watches deliveries,
// not frames.
class LockOnlyDrainHost final : public DrainHost {
public:
    void lockModel() override { m_lock.lock(); }
    void unlockModel() override { m_lock.unlock(); }
    void onSyncFrameReadyLocked(SyncFrameReason) override { }

private:
    std::mutex m_lock;
};

// A pipelined burst whose writer pauses exactly on a pass boundary leaves no
// partial pass to close. The empty pass the pipeline closes instead clears the
// drain's saturation and delivers what the throttle held, while the burst is
// still open: here it never ends, so nothing waits on its teardown.
TEST(testAPipelinedPauseOnAPassBoundaryDeliversBeforeTheBurstEnds)
{
    constexpr std::size_t burstBytes = 3 * static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes);
    std::string burst;
    constexpr std::string_view line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    while (burst.size() < burstBytes)
        burst += line;
    burst.resize(burstBytes);

    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/ubiquitty-paused-XXXXXX";
    Ubiquitty::PAL::UnixFileDescriptor file(::mkstemp(path.data()));
    REQUIRE(static_cast<bool>(file));
    ::unlink(path.c_str());
    REQUIRE(::write(file.get(), burst.data(), burst.size()) == static_cast<ssize_t>(burst.size()));
    REQUIRE(::lseek(file.get(), 0, SEEK_SET) == 0);
    int readinessPipe[2] = { -1, -1 };
    REQUIRE(::pipe(readinessPipe) == 0);
    Ubiquitty::PAL::UnixFileDescriptor readinessRead(readinessPipe[0]);
    Ubiquitty::PAL::UnixFileDescriptor readinessWrite(readinessPipe[1]);
    REQUIRE(::write(readinessWrite.get(), "r", 1) == 1);

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto stream = std::make_unique<Ubiquitty::PAL::PausedStream>(std::move(file), Ubiquitty::PAL::UnixFileDescriptor(::dup(readinessRead.get())));
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint = stream->duplicateAsPollableStream();
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> floodEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr && floodEndpoint != nullptr);
    auto adoption = runtime.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    LockOnlyDrainHost host;
    IngestLoop loop(runtime, host);
    std::atomic<std::uint64_t> deliveredBytes { 0 };
    loop.setDeliverHook(
        [](void* context, const DrainOutcome& outcome, std::uint32_t) {
            static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(static_cast<std::uint64_t>(outcome.bytesDrained), std::memory_order_relaxed);
        },
        &deliveredBytes);
    // Every outcome after the first is held by the throttle, so only the
    // empty pass can deliver the burst's bytes before it ends.
    loop.setDeliverMinInterval(60.0);
    (void)loop.prepare();
    loop.adoptFloodEndpoint(std::move(floodEndpoint));
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });

    bool isDelivered = false;
    for (int waited = 0; waited < 10000 && !isDelivered; waited += 5) {
        isDelivered = deliveredBytes.load(std::memory_order_relaxed) == burstBytes && !runtime.isDrainSaturated();
        if (!isDelivered)
            ::usleep(5 * 1000);
    }
    CHECK_MSG(loop.floodBurstCount() == 1, "the burst rode the pipeline and is still open");
    CHECK_MSG(isDelivered, "the burst's last pass waited for the burst to end");
    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    runtime.close();
}

TEST(testATimedPipelinePauseReleasesAHeldFrameBeforeTheBurstEnds)
{
    class TimedPausedDrainHost final : public DrainHost {
    public:
        void lockModel() override { m_lock.lock(); }
        void unlockModel() override { m_lock.unlock(); }
        void onSyncFrameReadyLocked(SyncFrameReason reason) override
        {
            if (reason == SyncFrameReason::SaturatedPass)
                m_isPaced = true;
        }
        std::uint32_t frameIntervalMicroseconds() const override { return 8'000; }
        std::uint32_t drainPassIntervalMicroseconds() const override { return m_isPaced ? 1 : 0; }

    private:
        std::mutex m_lock;
        bool m_isPaced = false;
    };

    // One byte-capped pass elects the reader; one full ring bite ends with
    // an unfinished CSI. The writer then remains paused without ending the
    // reader, so only the held-frame timer can release the refused capture.
    const std::size_t burstBytes = TerminalRuntime::drainPassCapacityBytes + floodBiteCapacityBytes;
    std::string burst(burstBytes - 3, 'x');
    burst += "\a\x1b["; // The bell delivers the timed outcome without waiting for the delivery throttle.
    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/ubiquitty-timed-pause-XXXXXX";
    Ubiquitty::PAL::UnixFileDescriptor file(::mkstemp(path.data()));
    REQUIRE(static_cast<bool>(file));
    ::unlink(path.c_str());
    REQUIRE(::write(file.get(), burst.data(), burst.size()) == static_cast<ssize_t>(burst.size()));
    REQUIRE(::lseek(file.get(), 0, SEEK_SET) == 0);
    int readinessPipe[2] = { -1, -1 };
    REQUIRE(::pipe(readinessPipe) == 0);
    Ubiquitty::PAL::UnixFileDescriptor readinessRead(readinessPipe[0]);
    Ubiquitty::PAL::UnixFileDescriptor readinessWrite(readinessPipe[1]);
    REQUIRE(::write(readinessWrite.get(), "r", 1) == 1);

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    std::atomic<std::uint64_t> clock { 0 };
    runtime.setCoherenceClock([](void* context) { return static_cast<std::atomic<std::uint64_t>*>(context)->load(std::memory_order_relaxed); }, &clock);
    auto stream = std::make_unique<Ubiquitty::PAL::PausedStream>(std::move(file), Ubiquitty::PAL::UnixFileDescriptor(::dup(readinessRead.get())));
    auto loopEndpoint = stream->duplicateAsPollableStream();
    auto floodEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr && floodEndpoint != nullptr);
    REQUIRE(runtime.adoptStream(std::move(stream)).has_value());

    TimedPausedDrainHost host;
    IngestLoop loop(runtime, host);
    std::atomic<bool> didFinishTimedPass { false };
    std::atomic<int> releases { 0 };
    loop.setDeliverHook(
        [](void* context, const DrainOutcome& outcome, std::uint32_t) {
            if (outcome.end == DrainPassEnd::TimeBudget)
                static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
        },
        &didFinishTimedPass);
    loop.setReleaseHook([](void* context) { static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_release); }, &releases);
    (void)loop.prepare();
    loop.adoptFloodEndpoint(std::move(floodEndpoint));
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });
    for (int waited = 0; waited < 2000 && !didFinishTimedPass.load(std::memory_order_acquire); ++waited)
        ::usleep(1000);
    CHECK(didFinishTimedPass.load(std::memory_order_acquire));
    CHECK(runtime.isFrameHeld());
    CHECK(!runtime.isDrainSaturated());
    CHECK_EQ(releases.load(std::memory_order_acquire), 0);
    clock.store(coherenceDeadlineMicroseconds + 1, std::memory_order_relaxed);
    for (int waited = 0; waited < 2000 && releases.load(std::memory_order_acquire) == 0; ++waited)
        ::usleep(1000);
    CHECK_MSG(releases.load(std::memory_order_acquire) == 1, "a paused timed pass must release its held frame while the reader remains alive");
    CHECK(!runtime.isFrameHeld());
    CHECK_EQ(loop.floodBurstCount(), 1u);
    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    CHECK(runtime.lastFrameClass() == FrameClass::Forced);
    runtime.close();
}

// A model lock, and the announcements the loop made, counted.
class AnnouncementCountingDrainHost final : public DrainHost {
public:
    void lockModel() override { m_lock.lock(); }
    void unlockModel() override { m_lock.unlock(); }
    void onSyncFrameReadyLocked(SyncFrameReason) override { announcements.fetch_add(1, std::memory_order_relaxed); }

    std::atomic<int> announcements { 0 };

private:
    std::mutex m_lock;
};

// A frame the coherence rule holds for the read to go quiet is asked again on
// the rule's schedule even while the coalesced delivery is holding an outcome:
// the delivery's longer wait must not stand in for the rule's shorter one, or
// a prompt printed just after output reaches the screen a delivery interval
// late.
TEST(testAHeldFrameIsAnnouncedOnTheRulesScheduleWhileADeliveryIsHeld)
{
    int pipeDescriptors[2] = { -1, -1 };
    REQUIRE(::pipe(pipeDescriptors) == 0);
    Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
    Ubiquitty::PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
    REQUIRE(::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) == 0);

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto stream = std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd));
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr);
    auto adoption = runtime.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    AnnouncementCountingDrainHost host;
    IngestLoop loop(runtime, host);
    loop.setDeliverHook([](void*, const DrainOutcome&, std::uint32_t) { }, nullptr);
    // Every outcome after the first is held for half a second.
    loop.setDeliverMinInterval(0.5);
    (void)loop.prepare();
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });

    const auto waitForAnnouncements = [&host](int count) {
        for (int waited = 0; waited < 2000 && host.announcements.load(std::memory_order_relaxed) < count; ++waited)
            ::usleep(1000);
        return host.announcements.load(std::memory_order_relaxed) >= count;
    };
    REQUIRE(::write(writeEnd.get(), "output\r\n", 8) == 8);
    CHECK(waitForAnnouncements(1));
    ::usleep(5000);
    const auto written = std::chrono::steady_clock::now();
    REQUIRE(::write(writeEnd.get(), "$ ", 2) == 2);
    CHECK(waitForAnnouncements(2));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - written).count();
    CHECK_MSG(waited < 100, ("the held frame was announced " + std::to_string(waited) + " ms after its write").c_str());

    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    runtime.close();
}

// A loop that stops while the coherence rule holds a frame leaves nobody to
// ask for it again; a render that waits on the rule would wait forever. The
// held frame is released, and announced, on the way out.
TEST(testALoopThatStopsReleasesTheFrameTheRuleHolds)
{
    int pipeDescriptors[2] = { -1, -1 };
    REQUIRE(::pipe(pipeDescriptors) == 0);
    Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
    Ubiquitty::PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
    REQUIRE(::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) == 0);

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto stream = std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd));
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr);
    auto adoption = runtime.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    AnnouncementCountingDrainHost host;
    IngestLoop loop(runtime, host);
    (void)loop.prepare();
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });

    // An open 2026 block: held for its whole budget, a second.
    REQUIRE(::write(writeEnd.get(), "\x1b[?2026hhalf a frame", 20) == 20);
    for (int waited = 0; waited < 2000 && !runtime.isFrameHeld(); ++waited)
        ::usleep(500);
    REQUIRE(runtime.isFrameHeld());
    const int announcedWhileHeld = host.announcements.load(std::memory_order_relaxed);

    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    CHECK_MSG(!runtime.isFrameHeld(), "a stopped loop left the frame held with nobody to release it");
    CHECK(host.announcements.load(std::memory_order_relaxed) == announcedWhileHeld + 1);
    CHECK(runtime.lastFrameClass() == FrameClass::Forced);
    runtime.close();
}

// A burst the coherence rule holds for its read to go quiet reaches the
// frontend on the burst's own delivery, while the rule still holds it: the draw
// that delivery asks for is refused. The rule lets go on the loop's clock, with
// no pass and so no delivery behind the release, and a program that wrote once
// and waits sends no byte that would ask again. The release itself has to tell
// the frontend, or the screen keeps showing the frame from before the burst.
TEST(testAFrameTheRuleReleasesWakesTheFrontend)
{
    int pipeDescriptors[2] = { -1, -1 };
    REQUIRE(::pipe(pipeDescriptors) == 0);
    Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
    Ubiquitty::PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
    REQUIRE(::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) == 0);

    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    auto adoption = session.adoptStream(std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd)));
    REQUIRE(adoption.has_value());

    // The frontend's side of the signal: it takes the pending flags on every
    // wake, and a wake that finds the frame still held draws nothing.
    struct Frontend {
        TerminalSession* session = nullptr;
        std::atomic<int> wakes { 0 };
        std::atomic<int> wakesAfterRelease { 0 };
        std::atomic<std::uint32_t> flagsAfterRelease { 0 };
    } frontend;
    frontend.session = &session;
    session.setDrainSignal(&frontend, [](void* context) {
        auto* state = static_cast<Frontend*>(context);
        const std::uint32_t flags = state->session->takeEventFlags();
        if (!state->session->isFrameHeld()) {
            state->flagsAfterRelease.fetch_or(flags, std::memory_order_relaxed);
            state->wakesAfterRelease.fetch_add(1, std::memory_order_relaxed);
        }
        state->wakes.fetch_add(1, std::memory_order_relaxed);
    });
    (void)session.takeEventFlags();
    REQUIRE(session.startIngest());

    REQUIRE(::write(writeEnd.get(), "first burst\r\n", 13) == 13);
    for (int waited = 0; waited < 2000 && frontend.wakesAfterRelease.load(std::memory_order_relaxed) == 0; ++waited)
        ::usleep(1000);
    CHECK(frontend.wakes.load(std::memory_order_relaxed) > 0);
    CHECK(!session.isFrameHeld());
    CHECK_MSG(frontend.wakesAfterRelease.load(std::memory_order_relaxed) > 0,
        "the rule released the burst and nobody told the frontend");
    constexpr std::uint32_t screenDirtyBit = 1u << static_cast<unsigned>(EventKind::ScreenDirty);
    CHECK_MSG(frontend.flagsAfterRelease.load(std::memory_order_relaxed) & screenDirtyBit,
        "the release's wake carries no screen to draw");

    session.stopIngest();
    session.setDrainSignal(nullptr, nullptr);
}

TEST(testIngestInputCommandsAndRestart)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    CHECK(session.launchShellCommand("cat"));
    CHECK(session.startIngest());

    // Input crosses to the child and its echo comes back through the loop.
    const std::string line = "echo-through-cat\r";
    session.sendInput({ line.data(), line.size() });
    bool echoed = false;
    for (int waited = 0; waited < 5000 && !echoed; waited += 10) {
        (void)session.takeEventFlags();
        echoed = gridContains(session, "echo-through-cat");
        if (!echoed)
            ::usleep(10 * 1000);
    }
    CHECK_MSG(echoed, "input reached the child and its echo reached the grid");

    // A paste bigger than the kernel's tty buffers rides the pending-write
    // queue; the wake gets the parked loop to drain it. Lines stay short of
    // the canonical line limit.
    std::string paste;
    for (int i = 0; i < 3000; ++i)
        paste += std::string(60, 'p') + "\r";
    session.sendInput({ paste.data(), paste.size() });

    // A command posted against the live loop takes the ring-and-wake path.
    session.setDesiredFocus(Ubiquitty::Modes::FocusState::Unfocused);

    // Unbracketed on purpose: the model-lock bracket around the grid
    // mutation is the session's own, and a caller-side lock here deadlocks
    // against it. The unbracketed call against the live flood is what an
    // unlocked resize fails — a rowMetaAt out-of-range abort when the drain
    // parses into a grid mid-reallocation.
    session.resize(30, 100, Ubiquitty::Runtime::TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Top);
    bool resized = false;
    for (int waited = 0; waited < 5000 && !resized; waited += 10) {
        resized = session.dimensions().rows == 30 && session.dimensions().columns == 100;
        if (!resized)
            ::usleep(10 * 1000);
    }
    CHECK_MSG(resized, "the grid resized under the live loop");

    // The paste crosses in lockstep with the child: the canonical tty input
    // queue holds about a kilobyte, so each refill waits for cat to be
    // scheduled and to echo. The wall time is the machine's; what the loop
    // owns is that the echo never stops advancing, so the budget is on
    // silence, with a ceiling only against a hang.
    std::uint64_t drained = 0;
    int silentMilliseconds = 0;
    for (int waited = 0; waited < 120000 && drained < paste.size() && silentMilliseconds < 2000; waited += 20) {
        const std::uint64_t step = session.takeDrainedBytes();
        drained += step;
        silentMilliseconds = step > 0 ? 0 : silentMilliseconds + 20;
        if (drained < paste.size())
            ::usleep(20 * 1000);
    }
    CHECK_MSG(drained >= paste.size(), "the queued paste drained and its echo returned");

    // Stop parts the parked loop through the waker; restart rearms it.
    session.stopIngest();
    CHECK(session.startIngest());
    session.wakeIngest();
    session.stopIngest();
}

namespace {

// When the slow host starts pacing its frames, and with them its passes.
enum class FramePacing {
    // Never: every pass ends at a would-block or its byte budget.
    Never,
    // From the capture of the first pass that filled its byte budget.
    AfterCapacity,
    // From the first pass, as a presented pane is paced.
    FromTheStart,
};

// A model lock held a millisecond per bite: the parser falls behind a reader
// pulling a file, so the ring keeps the backlog under which the reader's
// short-read wait applies.
//
// The host also counts that millisecond on a clock of its own, which moves
// only when the lock is taken, and the coherence rule reads it: a frame's age
// against the rule's deadline is the number of holds behind it, whatever the
// build's parse speed. The loop times its passes on the real clock, which the
// sleeps alone carry past the frame interval within one full ring bite.
class SlowParseDrainHost final : public DrainHost {
public:
    explicit SlowParseDrainHost(FramePacing pacing = FramePacing::Never)
        : m_shouldPaceAfterCapacity(pacing == FramePacing::AfterCapacity)
        , m_interval(pacing == FramePacing::FromTheStart ? 8'000 : 0)
    {
    }
    void lockModel() override
    {
        m_lock.lock();
        std::this_thread::sleep_for(modelLockHold);
        m_clockMicroseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::microseconds(modelLockHold).count()), std::memory_order_relaxed);
    }
    void unlockModel() override { m_lock.unlock(); }
    void onSyncFrameReadyLocked(SyncFrameReason reason) override
    {
        if (reason == SyncFrameReason::SaturatedPass && m_shouldPaceAfterCapacity)
            m_interval = 8'000;
        if (reason == SyncFrameReason::ContinuingPass) {
            ++continuingFrames;
            // Called under the model lock the drain holds, so the parser and
            // the frame just taken can be read here.
            const Ubiquitty::Terminal* terminal = runtime ? runtime->borrowTermForRender() : nullptr;
            if (terminal && loop && loop->floodBurstCount() > 0 && !Ubiquitty::VT::isParserAtGround(terminal)
                && runtime->lastFrameLagMicroseconds() < coherenceDeadlineMicroseconds)
                ++budgetedMidSequenceFrames;
        }
    }
    std::uint32_t frameIntervalMicroseconds() const override { return m_interval; }

    /// Reads the host's clock, the coherence rule's in these floods.
    ///
    /// - Parameter context: The host.
    /// - Returns: One `modelLockHold`, in microseconds, for every time the
    ///   model lock was taken.
    static std::uint64_t readCoherenceClock(void* context)
    {
        return static_cast<const SlowParseDrainHost*>(context)->m_clockMicroseconds.load(std::memory_order_relaxed);
    }

    int continuingFrames = 0;
    // Frames the reader's timed passes took with the parser inside a
    // sequence, before the coherence deadline could have released them.
    int budgetedMidSequenceFrames = 0;
    TerminalRuntime* runtime = nullptr;
    const IngestLoop* loop = nullptr;

private:
    static constexpr std::chrono::milliseconds modelLockHold { 1 };

    std::mutex m_lock;
    bool m_shouldPaceAfterCapacity;
    std::uint32_t m_interval;
    std::atomic<std::uint64_t> m_clockMicroseconds { 0 };
};

struct ShapedFloodOutcome {
    std::uint64_t deliveredBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t bursts = 0;
    std::uint64_t accumulateWaits = 0;
    int continuingFrames = 0;
    int budgetedMidSequenceFrames = 0;
    bool didReachTail = false;
};

// What a shaped flood carries.
enum class FloodContent {
    // Plain lines: a pass almost always ends at ground.
    Lines,
    // A truecolour SGR before every cell: a pass almost always ends inside one.
    DenseSGR,
};

// Floods a loop over a prefilled transport read in `shape`, behind a slow
// parser, and reports what the flood reader did.
ShapedFloodOutcome runShapedFlood(Ubiquitty::PAL::ReadShape shape, FramePacing pacing = FramePacing::Never,
    FloodContent content = FloodContent::Lines)
{
    ShapedFloodOutcome outcome;
    // Past the drain pass capacity, so the flood rides the pipeline, and
    // small enough that the slow parser finishes it in a few hundred ms.
    std::string flood;
    constexpr std::string_view line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    constexpr std::string_view cell = "\x1b[38;2;111;222;123mx";
    while (flood.size() < 3 * static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes)) {
        if (content == FloodContent::Lines) {
            flood += line;
            continue;
        }
        for (int column = 0; column < 79; ++column)
            flood += cell;
        flood += "\r\n";
    }
    constexpr std::string_view tail = "timed-end";
    flood += "\x1b[H";
    flood += tail;
    outcome.totalBytes = flood.size();
    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/ubiquitty-shaped-XXXXXX";
    Ubiquitty::PAL::UnixFileDescriptor file(::mkstemp(path.data()));
    if (!file)
        return outcome;
    ::unlink(path.c_str());
    if (::write(file.get(), flood.data(), flood.size()) != static_cast<ssize_t>(flood.size()) || ::lseek(file.get(), 0, SEEK_SET) != 0)
        return outcome;

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto stream = std::make_unique<Ubiquitty::PAL::PrefilledStream>(std::move(file), shape);
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint = stream->duplicateAsPollableStream();
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> floodEndpoint = stream->duplicateAsPollableStream();
    if (!loopEndpoint || !floodEndpoint || !runtime.adoptStream(std::move(stream)).has_value())
        return outcome;

    SlowParseDrainHost host(pacing);
    runtime.setCoherenceClock(&SlowParseDrainHost::readCoherenceClock, &host);
    IngestLoop loop(runtime, host);
    host.runtime = &runtime;
    host.loop = &loop;
    std::atomic<std::uint64_t> deliveredBytes { 0 };
    loop.setDeliverHook(
        [](void* context, const DrainOutcome& drained, std::uint32_t) {
            static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(static_cast<std::uint64_t>(drained.bytesDrained), std::memory_order_relaxed);
        },
        &deliveredBytes);
    (void)loop.prepare();
    loop.adoptFloodEndpoint(std::move(floodEndpoint));
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });
    for (int waited = 0; waited < 20000 && deliveredBytes.load(std::memory_order_relaxed) < outcome.totalBytes; waited += 5)
        ::usleep(5 * 1000);
    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    if (const auto* terminal = runtime.borrowTermForRender(); terminal && terminal->grid) {
        outcome.didReachTail = true;
        for (std::size_t column = 0; column < tail.size(); ++column) {
            const auto* cell = terminal->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(static_cast<int>(column)));
            if (!cell || cell->codePoint != static_cast<std::uint32_t>(tail[column]))
                outcome.didReachTail = false;
        }
    }
    outcome.continuingFrames = host.continuingFrames;
    outcome.budgetedMidSequenceFrames = host.budgetedMidSequenceFrames;
    runtime.close();
    outcome.deliveredBytes = deliveredBytes.load(std::memory_order_relaxed);
    outcome.bursts = loop.floodBurstCount();
    outcome.accumulateWaits = loop.floodAccumulateWaitCount();
    return outcome;
}

} // namespace

// A transport with a fixed read quantum (a macOS pty: 1024 bytes a read
// however long the reader waits) gains nothing from the short-read wait, and
// a wait per read held a flood to quantum / wait, about a quarter of the
// rate without it. One wait per burst finds that out; the rest are refused.
TEST(testAFixedReadQuantumTakesOneAccumulateWaitPerBurst)
{
    const ShapedFloodOutcome outcome = runShapedFlood({ .quantumBytes = 1024 });
    CHECK_EQ(outcome.deliveredBytes, outcome.totalBytes);
    CHECK_MSG(outcome.bursts >= 1, "the flood rode the pipeline, so the reader's wait was in play");
    CHECK_MSG(outcome.accumulateWaits >= 1, "the backlog under a short read asked for the wait");
    CHECK_MSG(outcome.accumulateWaits <= outcome.bursts, "a wait that did not grow the next read ends the burst's waits");
}

// Where a wait lets the slab land (a Linux pty), the next read grows and the
// reader keeps waiting after short reads for the whole burst.
TEST(testASlabThatLandsKeepsTheAccumulateWait)
{
    const ShapedFloodOutcome outcome = runShapedFlood({ .quantumBytes = 512, .landing = std::chrono::microseconds(30), .slabBytes = 16 * 1024 });
    CHECK_EQ(outcome.deliveredBytes, outcome.totalBytes);
    CHECK_MSG(outcome.bursts >= 1, "the flood rode the pipeline, so the reader's wait was in play");
    CHECK_MSG(outcome.accumulateWaits > outcome.bursts, "a wait that grew the next read is taken again");
}

TEST(testTimedPipelinePassesKeepTheReaderAndEveryUnreadByte)
{
    // The first byte-capped pass elects the reader. Pacing begins only after
    // that capture, so subsequent timed passes exercise the active ring.
    const ShapedFloodOutcome outcome = runShapedFlood({}, FramePacing::AfterCapacity);
    CHECK_EQ(outcome.deliveredBytes, outcome.totalBytes);
    CHECK_EQ(outcome.bursts, 1u);
    CHECK(outcome.continuingFrames > 0);
    CHECK(outcome.didReachTail);
}

// A presented pane times its passes from the first one. Behind a parser that
// retires a 4 KiB bite a millisecond, no pass can fill its byte budget inside
// the 8 ms interval, so a loop that elected the reader only on a full pass
// kept the whole flood on the sequential reads, every byte of it waiting on
// the parse that read it. A pass that spent its interval still reading is the
// same flood, and the reader takes the burst.
TEST(testATimedPassHandsAFloodToTheReader)
{
    const ShapedFloodOutcome outcome = runShapedFlood({}, FramePacing::FromTheStart);
    CHECK_EQ(outcome.deliveredBytes, outcome.totalBytes);
    CHECK_MSG(outcome.bursts >= 1, "a pass that ran out its frame interval with the writer ahead elects the flood reader");
    CHECK(outcome.continuingFrames > 0);
    CHECK(outcome.didReachTail);
}

// The reader's passes end at the same frame interval, and a pass that ends
// there takes the frame the model holds even inside a sequence, as the
// sequential pass that handed the burst over does. A flood that puts an SGR
// before every cell ends almost every pass inside one, so frames reach the
// screen at the pass cadence rather than at the coherence deadline.
TEST(testTheReadersTimedPassesTakeFramesInsideASequence)
{
    const ShapedFloodOutcome outcome = runShapedFlood({}, FramePacing::FromTheStart, FloodContent::DenseSGR);
    CHECK_EQ(outcome.deliveredBytes, outcome.totalBytes);
    CHECK_MSG(outcome.bursts >= 1, "the timed pass elected the flood reader");
    CHECK_MSG(outcome.budgetedMidSequenceFrames > 0, "a timed pass of the reader took its frame inside a sequence, before the deadline");
    CHECK(outcome.didReachTail);
}

UBIQUITTY_TEST_MAIN()
