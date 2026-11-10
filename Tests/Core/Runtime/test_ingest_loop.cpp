// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SoleModelThread.h"

// test_ingest_loop.cpp — the portable ingest loop and the session that owns
// it (Core/Runtime/IngestLoop.h through Core/Runtime/TerminalSession.h),
// driven over a real PTY, and the flood pipeline over a prefilled transport:
// the parked wait and its waker, the coalesced delivery, the pipeline's
// reader/ring/parser, input backpressure, the posted-command paths and the
// stop/restart pair. The loop's tuning constants are the Metal arm's measured
// numbers; nothing here asserts timing beyond one coarse ceiling (a held
// frame announced within 100 ms, where the defect it pins takes 500), only
// that bytes arrive, events coalesce, and stops end the thread.

#include "Core/Runtime/HostedWindows.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/PostedWorkTable.h"
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
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

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
// The scheduling band the calling thread runs in, as the platform names it:
// the QoS class on Darwin, the nice value on Linux.
int currentThreadBand()
{
#if defined(__APPLE__)
    return static_cast<int>(qos_class_self());
#elif defined(__linux__)
    return ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
#else
    return 0;
#endif
}

// The threads that read a transport, and the bands they read in.
struct ReadBands {
    std::mutex lock;
    std::set<std::thread::id> threads;
    std::set<int> bands;

    void noteRead()
    {
        std::lock_guard guard(lock);
        threads.insert(std::this_thread::get_id());
        bands.insert(currentThreadBand());
    }
};

struct ReadShape {
    // Most bytes a read returns while the slab is still landing; 0 for no cap.
    int quantumBytes = 0;
    // How long after the previous read returned the slab has landed; zero
    // keeps the quantum on every read.
    std::chrono::microseconds landing { 0 };
    // Most bytes a read returns once the slab has landed; 0 for no cap.
    int slabBytes = 0;
    // Every this many bytes the writer pauses, and the next `pauseReads` reads
    // find nothing; 0 for a writer that never pauses.
    int pauseEveryBytes = 0;
    int pauseReads = 0;
};

class PrefilledEndpoint final : public PollableByteStream {
public:
    explicit PrefilledEndpoint(UnixFileDescriptor descriptor, ReadShape shape = {}, ReadBands* readBands = nullptr)
        : m_descriptor(std::move(descriptor))
        , m_shape(shape)
        , m_readBands(readBands)
    {
    }

    int read(std::span<char> buffer) override
    {
        std::size_t length = buffer.size();
        if (m_shape.pauseEveryBytes > 0) {
            if (m_pauseReadsLeft > 0) {
                --m_pauseReadsLeft;
                return std::to_underlying(StreamStatus::Again);
            }
            length = std::min(length, static_cast<std::size_t>(m_shape.pauseEveryBytes - m_bytesSincePause));
        }
        if (m_shape.quantumBytes > 0) {
            const bool hasLanded = m_shape.landing.count() > 0 && std::chrono::steady_clock::now() - m_lastReturn >= m_shape.landing;
            if (!hasLanded)
                length = std::min(length, static_cast<std::size_t>(m_shape.quantumBytes));
            else if (m_shape.slabBytes > 0)
                length = std::min(length, static_cast<std::size_t>(m_shape.slabBytes));
        }
        const ssize_t count = ::read(m_descriptor.get(), buffer.data(), length);
        m_lastReturn = std::chrono::steady_clock::now();
        if (count > 0 && m_readBands)
            m_readBands->noteRead();
        if (count > 0 && m_shape.pauseEveryBytes > 0) {
            m_bytesSincePause += static_cast<int>(count);
            if (m_bytesSincePause >= m_shape.pauseEveryBytes) {
                m_bytesSincePause = 0;
                m_pauseReadsLeft = m_shape.pauseReads;
            }
        }
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
        return std::make_unique<PrefilledEndpoint>(std::move(duplicate), m_shape, m_readBands);
    }

private:
    UnixFileDescriptor m_descriptor;
    ReadShape m_shape;
    ReadBands* m_readBands;
    std::chrono::steady_clock::time_point m_lastReturn {};
    int m_bytesSincePause = 0;
    int m_pauseReadsLeft = 0;
};

// A session transport whose whole stream is on disk before ingest starts.
class PrefilledStream final : public Stream, private ProcessFacts {
public:
    explicit PrefilledStream(UnixFileDescriptor descriptor, ReadShape shape = {}, ReadBands* readBands = nullptr)
        : m_rawDescriptor(descriptor.get())
        , m_endpoint(std::move(descriptor), shape, readBands)
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
    ProcessGroupPresence programPresence = ProcessGroupPresence::Unknown;
    /// A terminal handoff performed after the next foreground probe read its group.
    std::atomic<ProcessID> groupAfterNextProbe { 0 };
    std::atomic<ProcessID> groupReadBeforeHandoff { 0 };
    std::atomic<unsigned> acknowledgedGroupReads { 0 };
    std::atomic<ProcessID> groupBeforeProbeHandoff { 0 };
    std::atomic<unsigned> probeHandoffs { 0 };

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
        if (groupAfterNextProbe.load(std::memory_order_acquire) != 0) {
            groupReadBeforeHandoff.store(group, std::memory_order_relaxed);
            acknowledgedGroupReads.fetch_add(1, std::memory_order_release);
            const ProcessID nextGroup = groupAfterNextProbe.exchange(0, std::memory_order_acq_rel);
            if (nextGroup != 0) {
                groupBeforeProbeHandoff.store(foregroundGroup.exchange(nextGroup, std::memory_order_acq_rel), std::memory_order_relaxed);
                probeHandoffs.fetch_add(1, std::memory_order_release);
            }
        }
        if (group == 0)
            return std::nullopt;
        return group;
    }
    ProcessGroupPresence processGroupPresence(ProcessID group) override
    {
        return group == shellGroup ? ProcessGroupPresence::Present : programPresence;
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
// transport positioned at its start, whose reads note their threads' bands in
// `readBands` when one is given.
bool adoptPrefilledStream(TerminalSession& session, std::string_view bytes, Ubiquitty::PAL::ReadBands* readBands = nullptr)
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
    auto adoption = session.adoptStream(std::make_unique<Ubiquitty::PAL::PrefilledStream>(Ubiquitty::PAL::UnixFileDescriptor(descriptor), Ubiquitty::PAL::ReadShape {}, readBands));
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

int foregroundGroupOf(TerminalSession& session)
{
    session.lockForRead();
    const auto* terminal = session.terminal();
    const int group = terminal ? terminal->foregroundProcessGroup : 0;
    session.unlockForRead();
    return group;
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

TEST(testAQuietShellHandoffAfterTheExitProbeRetiresTheProgramTitle)
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
        constexpr std::string_view setTitle = "\x1b]2;program title\x07";
        REQUIRE(::write(writeEnd.get(), setTitle.data(), setTitle.size()) == static_cast<ssize_t>(setTitle.size()));
        const DrainOutcome initialDrain = session.drain();
        REQUIRE(initialDrain.bytesDrained == static_cast<int>(setTitle.size()));
        REQUIRE(titleOf(session) == "program title");
        // Finish the initial sample before the loop starts; no bytes or
        // second tree edge follow the one exit below.
        (void)session.sampleProcessStateOffDrain();
        // Direct drains report through their returned outcome; only ingest
        // deliveries increment this counter. Establish its baseline anyway.
        REQUIRE(session.takeDrainedBytes() == 0);
        transport.programPresence = Ubiquitty::PAL::ProcessGroupPresence::Gone;
        transport.groupAfterNextProbe.store(shellGroup, std::memory_order_release);
        REQUIRE(session.startIngest());
        const bool didRaiseExit = transport.raiseTreeEdge();
        bool didRetire = false;
        bool didConverge = false;
        for (int waited = 0; waited < 2000 && !(didRetire && didConverge); ++waited) {
            didRetire = titleOf(session).empty();
            didConverge = foregroundGroupOf(session) == shellGroup;
            ::usleep(1000);
        }
        session.stopIngest();

        CHECK_MSG(didRaiseExit, level);
        CHECK_MSG(transport.acknowledgedGroupReads.load(std::memory_order_acquire) > 0, level);
        CHECK_MSG(transport.groupReadBeforeHandoff.load(std::memory_order_relaxed) == programGroup, level);
        CHECK_MSG(transport.probeHandoffs.load(std::memory_order_acquire) == 1, level);
        CHECK_MSG(transport.groupBeforeProbeHandoff.load(std::memory_order_relaxed) == programGroup, level);
        CHECK_MSG(transport.foregroundGroup.load(std::memory_order_relaxed) == shellGroup, level);
        const std::uint64_t drainedBytes = session.takeDrainedBytes();
        CHECK_MSG(drainedBytes == 0, "the handoff produced no output to trigger another foreground sample");
        const std::string failure = std::string(level) + ": title survived one exit edge; group reads="
            + std::to_string(transport.acknowledgedGroupReads.load(std::memory_order_relaxed))
            + ", read group=" + std::to_string(transport.groupReadBeforeHandoff.load(std::memory_order_relaxed))
            + ", previous group=" + std::to_string(transport.groupBeforeProbeHandoff.load(std::memory_order_relaxed))
            + ", current group=" + std::to_string(transport.foregroundGroup.load(std::memory_order_relaxed))
            + ", handoffs=" + std::to_string(transport.probeHandoffs.load(std::memory_order_relaxed))
            + ", drained bytes=" + std::to_string(drainedBytes);
        CHECK_MSG(didRetire, failure.c_str());
        CHECK_MSG(didConverge, "the debt-only probe discovers the actual shell without another edge or output");
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
    ownModelOnTestThread();
    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    runtime.init(parameters);

    const std::array<std::string, 2> arguments { "/bin/sleep", "30" };
    auto stream = Ubiquitty::PAL::openPTY(arguments);
    REQUIRE(stream != nullptr);
    const bool hasTreeEdges = stream->process().deliversTreeEdges();
    REQUIRE(runtime.prepareAdoption());
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

// The flood reader hands every refill on to the parser, so a reader ranked
// below the loop holds the writer back on a loaded machine by its own
// scheduling delay: one ranked with a machine's busy loops cut a flood to a
// tenth. Every thread that reads the transport, the loop's and the reader's,
// reads in one band.
TEST(testEveryThreadThatReadsAFloodReadsInOneBand)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    Ubiquitty::PAL::ReadBands readBands;
    REQUIRE(adoptPrefilledStream(session, floodStream(), &readBands));
    REQUIRE(session.startIngest());

    const SessionWatch watch = watchUntilExit(session, 30000);
    session.stopIngest();
    CHECK_MSG(watch.exited, "the flood ends and the end reaches the flags");
    CHECK_MSG(session.floodBurstCount() >= 1, "the burst rode the pipeline, so its reader read the transport");
    std::lock_guard guard(readBands.lock);
    CHECK_MSG(readBands.threads.size() >= 2, "the loop's thread and the flood reader both read");
    CHECK_EQ(readBands.bands.size(), 1u);
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
    void lockModel() override UBIQUITTY_ACQUIRES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.lock(); }
    void unlockModel() override UBIQUITTY_RELEASES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.unlock(); }
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
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
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
        void lockModel() override UBIQUITTY_ACQUIRES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.lock(); }
        void unlockModel() override UBIQUITTY_RELEASES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.unlock(); }
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
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
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
    CHECK_EQ(releases.load(std::memory_order_acquire), 0);
    clock.store(coherenceDeadlineMicroseconds + 1, std::memory_order_relaxed);
    for (int waited = 0; waited < 2000 && releases.load(std::memory_order_acquire) == 0; ++waited)
        ::usleep(1000);
    CHECK_MSG(releases.load(std::memory_order_acquire) == 1, "a paused timed pass must release its held frame while the reader remains alive");
    CHECK(!runtime.isFrameHeld());
    CHECK_MSG(!runtime.isDrainSaturated(), "the pause ends the saturation the timed pass set");
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
    void lockModel() override UBIQUITTY_ACQUIRES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.lock(); }
    void unlockModel() override UBIQUITTY_RELEASES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.unlock(); }
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
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
    auto adoption = runtime.adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    AnnouncementCountingDrainHost host;
    IngestLoop loop(runtime, host);
    loop.setDeliverHook([](void*, const DrainOutcome&, std::uint32_t) { }, nullptr);
    // Every outcome after the first is held for two seconds: far enough from
    // the rule's two-refresh deadline that a busy runner's scheduling delay
    // cannot be mistaken for the delivery's wait.
    loop.setDeliverMinInterval(2.0);
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
    CHECK_MSG(waited < 1000, ("the held frame was announced " + std::to_string(waited) + " ms after its write").c_str());

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
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
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

namespace {

// A session reading its program through a pipe, its ingest loop running: the
// write end is the program's.
struct LoopSession {
    SessionHandle handle;
    Ubiquitty::PAL::UnixFileDescriptor writeEnd;
    bool isRunning = false;

    LoopSession()
    {
        int pipeDescriptors[2] = { -1, -1 };
        if (::pipe(pipeDescriptors) != 0)
            return;
        Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
        writeEnd = Ubiquitty::PAL::UnixFileDescriptor(pipeDescriptors[1]);
        if (::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) != 0)
            return;
        handle.session->start(24, 80);
        if (!handle.session->adoptStream(std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd))).has_value())
            return;
        isRunning = handle.session->startIngest();
    }

    ~LoopSession() { handle.session->stopIngest(); }

    [[nodiscard]] TerminalSession& session() const { return *handle.session; }

    // Writes `bytes` as the program, and waits up to two seconds for the loop
    // to drain them.
    bool write(std::string_view bytes) const
    {
        if (::write(writeEnd.get(), bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()))
            return false;
        std::uint64_t drained = 0;
        for (int waited = 0; waited < 2000 && drained < bytes.size(); ++waited) {
            drained += handle.session->takeDrainedBytes();
            if (drained < bytes.size())
                ::usleep(1000);
        }
        return drained >= bytes.size();
    }
};

// Runs `call` while another thread holds the session's model lock, and says
// whether it returned before that thread let go.
template<typename Call>
bool returnsWhileTheModelIsHeld(TerminalSession& session, Call call)
{
    std::binary_semaphore isHeld(0);
    std::binary_semaphore didReturn(0);
    bool didReturnWhileHeld = false;
    std::thread holder([&] {
        session.lockForRead();
        isHeld.release();
        didReturnWhileHeld = didReturn.try_acquire_for(std::chrono::seconds(2));
        session.unlockForRead();
    });
    isHeld.acquire();
    call();
    didReturn.release();
    holder.join();
    return didReturnWhileHeld;
}

KeyPress characterPress(char character)
{
    KeyPress press;
    press.unshiftedCodePoint = static_cast<unsigned char>(character);
    press.shiftedCodePoint = static_cast<unsigned char>(character);
    press.text[0] = static_cast<unsigned char>(character);
    press.textLength = 1;
    return press;
}

} // namespace

// A key on a pane the loop drains encodes in the modes the program left,
// with the model held by another thread: the loop published them at the
// release that ended the bite, before any snapshot.
TEST(testAKeyEncodesWithoutTheModelInTheModesTheLoopReleased)
{
    LoopSession loop;
    REQUIRE(loop.isRunning);
    REQUIRE(loop.write("\033[?1h"));
    KeyPress up;
    up.virtualKey = Ubiquitty::Keys::VirtualKey::UpArrow;
    std::array<char, 64> encoded {};
    KeyEncoding encoding;
    CHECK(returnsWhileTheModelIsHeld(loop.session(), [&] { encoding = loop.session().sendKey(up, encoded); }));
    REQUIRE(encoding.length == 3);
    CHECK_STR_EQ(std::string(encoded.data(), 3), "\033OA");
}

// A title and a directory a program reported are read from what the loop
// published, with the model held by another thread.
TEST(testATitleAndDirectoryAreReadWithoutTheModel)
{
    LoopSession loop;
    REQUIRE(loop.isRunning);
    REQUIRE(loop.write("\033]2;a published title\033\\\033]7;file://host/tmp/published-directory\033\\"));
    std::string title;
    std::string directory;
    bool didAnswer = false;
    for (int waited = 0; waited < 2000; ++waited) {
        CHECK(returnsWhileTheModelIsHeld(loop.session(), [&] { didAnswer = loop.session().readPublishedTitleTexts(title, directory); }));
        if (didAnswer && title == "a published title" && directory.find("/tmp/published-directory") != std::string::npos)
            break;
        ::usleep(1000);
    }
    CHECK(didAnswer);
    CHECK_STR_EQ(title, "a published title");
    CHECK(directory.find("/tmp/published-directory") != std::string::npos);
}

// A banner a program posts is taken from the queue without the model, with the
// model held by another thread, and a repeat of the banner standing at the
// tail still folds into it.
TEST(testANotificationIsTakenWithoutTheModel)
{
    LoopSession loop;
    REQUIRE(loop.isRunning);
    REQUIRE(loop.session().installNotificationQueue());
    REQUIRE(loop.write("\033]9;same banner\033\\\033]9;same banner\033\\"));
    TerminalSession::Notification first;
    for (int waited = 0; waited < 2000 && !first.isPresent; ++waited) {
        CHECK(returnsWhileTheModelIsHeld(loop.session(), [&] { first = loop.session().takeNotification(); }));
        if (!first.isPresent)
            ::usleep(1000);
    }
    REQUIRE(first.isPresent);
    CHECK_STR_EQ(first.title, "same banner");
    bool hasMore = true;
    CHECK(returnsWhileTheModelIsHeld(loop.session(), [&] { hasMore = loop.session().hasPendingNotification(); }));
    CHECK(!hasMore);
}

// A text longer than the record keeps is cut on a character boundary, and the
// record says the directory was cut.
TEST(testTheTitleRecordCutsOnACharacterBoundary)
{
    Ubiquitty::Runtime::TitleFacts facts;
    const std::string twoByte = [] {
        std::string text;
        for (int index = 0; index < 3000; ++index)
            text += "\xC3\xA9";
        return text;
    }();
    facts.assign("short", twoByte);
    CHECK_STR_EQ(std::string(facts.titleView()), "short");
    CHECK(facts.isDirectoryCut);
    CHECK(facts.directoryView().size() <= Ubiquitty::Runtime::TitleFacts::maxBytes);
    CHECK(facts.directoryView().size() % 2 == 0);
    CHECK(facts.holds("short", twoByte));
    CHECK(!facts.holds("other", twoByte));
}

// A key's activity is posted to the loop rather than written under the lock:
// the key returns while another thread holds the model, and the loop stamps
// the cursor's activity tick once it may.
TEST(testAKeyPostsItsActivityToTheLoop)
{
    LoopSession loop;
    REQUIRE(loop.isRunning);
    setenv("UBIQUITTY_FAKE_TICKS", "4321", 1);
    std::array<char, 64> encoded {};
    CHECK(returnsWhileTheModelIsHeld(loop.session(), [&] { static_cast<void>(loop.session().sendKey(characterPress('x'), encoded)); }));
    unsetenv("UBIQUITTY_FAKE_TICKS");
    std::uint32_t activityTick = 0;
    for (int waited = 0; waited < 2000 && activityTick != 4321; ++waited) {
        loop.session().lockForRead();
        if (const Ubiquitty::Terminal* terminal = loop.session().terminal())
            activityTick = terminal->lastActivityTick;
        loop.session().unlockForRead();
        if (activityTick != 4321)
            ::usleep(1000);
    }
    CHECK_EQ(activityTick, 4321u);
}

// A key typed while the loop carries a flood is applied at the end of a pass,
// with most of the flood still unread: the pipeline runs no idle service, so
// the pass is the only place a busy pane's typing reaches its cursor.
TEST(testAKeyTypedIntoAFloodIsAppliedBeforeTheFloodEnds)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);
    const std::string flood = floodStream();
    REQUIRE(adoptPrefilledStream(session, flood));
    REQUIRE(session.startIngest());
    setenv("UBIQUITTY_FAKE_TICKS", "4321", 1);
    std::array<char, 64> encoded {};
    static_cast<void>(session.sendKey(characterPress('x'), encoded));
    unsetenv("UBIQUITTY_FAKE_TICKS");
    std::uint64_t drained = 0;
    bool isApplied = false;
    for (int waited = 0; waited < 30000 && !isApplied; ++waited) {
        drained += session.takeDrainedBytes();
        session.lockForRead();
        if (const Ubiquitty::Terminal* terminal = session.terminal())
            isApplied = terminal->lastActivityTick == 4321;
        session.unlockForRead();
        if (!isApplied)
            ::usleep(1000);
    }
    CHECK(isApplied);
    CHECK_MSG(drained < flood.size() / 2, std::format("the key was applied after {} of {} bytes", drained, flood.size()).c_str());
    session.stopIngest();
}

// A key on a screen the CRT saver blanked wakes it through the loop, with no
// output behind it: the loop applies the posted input, publishes the screen
// shown and tells the frontend to draw.
TEST(testAKeyWakesACRTSaverBlankThroughTheLoop)
{
    LoopSession loop;
    REQUIRE(loop.isRunning);
    TerminalSession& session = loop.session();
    session.lockForWrite();
    if (Ubiquitty::Terminal* terminal = session.terminal()) {
        // Saving stays on, so the loop's own idle service keeps the blank.
        terminal->modes.shouldSaveCRT = true;
        terminal->isCRTBlanked = true;
        terminal->screenBlanked = true;
    }
    session.unlockAfterWrite();
    REQUIRE(session.readSnapshot().modes.isScreenBlanked);
    // The control: with no input, the loop's idle service keeps the blank.
    for (int waited = 0; waited < 300; ++waited)
        ::usleep(1000);
    REQUIRE_MSG(session.readSnapshot().modes.isScreenBlanked, "the screen stays blanked with no input");
    static_cast<void>(session.takeEventFlags());
    std::array<char, 64> encoded {};
    static_cast<void>(session.sendKey(characterPress('x'), encoded));
    constexpr std::uint32_t screenDirtyBit = 1u << static_cast<unsigned>(EventKind::ScreenDirty);
    std::uint32_t flags = 0;
    for (int waited = 0; waited < 2000 && session.readSnapshot().modes.isScreenBlanked; ++waited) {
        flags |= session.takeEventFlags();
        ::usleep(1000);
    }
    flags |= session.takeEventFlags();
    CHECK_MSG(!session.readSnapshot().modes.isScreenBlanked, "the key woke the screen");
    CHECK_MSG(flags & screenDirtyBit, "the frontend was told to draw the screen the key woke");
}

namespace {

// The frontend loop a hosting source posts its work to, which the case's own
// thread runs between its checks.
struct CaseLoop {
    static std::mutex& lock()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::vector<PostedWorkToken>& tokens()
    {
        static std::vector<PostedWorkToken> pending;
        return pending;
    }

    static void post(PostedWorkToken token)
    {
        const std::scoped_lock hold(lock());
        tokens().push_back(token);
    }

    static void postAfter(double, PostedWorkToken token) { post(token); }

    static void pump()
    {
        std::vector<PostedWorkToken> due;
        {
            const std::scoped_lock hold(lock());
            due.swap(tokens());
        }
        for (const PostedWorkToken token : due)
            static_cast<void>(runPostedSessionWork(token));
    }

    static void discard()
    {
        std::vector<PostedWorkToken> left;
        {
            const std::scoped_lock hold(lock());
            left.swap(tokens());
        }
        for (const PostedWorkToken token : left)
            static_cast<void>(discardPostedSessionWork(token));
    }
};

// What a tmux server says to a control client that attaches to one window of
// one pane, and the pane's first output: the attach's empty answer, which the
// client enters control mode on, the pane listing it asks for, and a %output
// line. Echo is off so the client's commands do not come back as text.
constexpr std::string_view controlModeProgram = "stty -echo; "
                                                "printf '%%begin 1 1 0\\n%%end 1 1 0\\n"
                                                "%%begin 2 2 1\\n@1 %%1 80 24 1 ef01,80x24,0,0,1 work editor\\n%%end 2 2 1\\n"
                                                "%%output %%1 hosted-pane-content\\\\015\\\\012\\n'; "
                                                "exec sleep 30";

} // namespace

// A tmux pane drawn through a view with no tmux server: a program speaking the
// control protocol, launched as a frontend launches a local `tmux -CC`, hosts
// one pane, and a view adopts it through the hosting source, reads the pane's
// output, and reads its frame clock while the pane's model is held elsewhere.
TEST(testAPaneAControlModeProgramHostsIsAdoptedThroughItsSource)
{
    TerminalSession::enableHostedWindows(Ubiquitty::Runtime::tmuxControlHostFactoryAddress(), { .runSoon = &CaseLoop::post, .runAfter = &CaseLoop::postAfter });
    SessionHandle handle;
    TerminalSession& host = *handle.session;
    host.start(24, 80);
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", std::string(controlModeProgram) });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    command.setTmuxControlHost("");
    REQUIRE(host.launch(command));
    REQUIRE(host.startIngest());
    const std::uintptr_t source = host.hostedWindowSourceAddress();
    REQUIRE(source != 0);
    bool isPaneLive = false;
    for (int waited = 0; waited < 5000 && !isPaneLive; waited += 5) {
        CaseLoop::pump();
        isPaneLive = host.hostedPaneFacts(1).terminalAddress != 0;
        if (!isPaneLive)
            ::usleep(5000);
    }
    REQUIRE(isPaneLive);

    TerminalSession* view = TerminalSession::make();
    REQUIRE(view != nullptr);
    view->start(24, 80);
    REQUIRE(view->adoptHostedPane(source, 1));
    bool hasContent = false;
    for (int waited = 0; waited < 5000 && !hasContent; waited += 5) {
        CaseLoop::pump();
        hasContent = gridContains(*view, "hosted-pane-content");
        if (!hasContent)
            ::usleep(5000);
    }
    CHECK_MSG(hasContent, "the view reads the output the program sent its pane");
    CHECK(returnsWhileTheModelIsHeld(*view, [&] { static_cast<void>(view->frameClockFactsNow()); }));
    view->endExternalAdoption();
    view->close();
    releaseTerminalSession(view);
    host.stopIngest();
    host.endHostedWindows();
    CaseLoop::discard();
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
    void lockModel() override UBIQUITTY_ACQUIRES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        m_lock.lock();
        std::this_thread::sleep_for(modelLockHold);
        m_clockMicroseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::microseconds(modelLockHold).count()), std::memory_order_relaxed);
    }
    void unlockModel() override UBIQUITTY_RELEASES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.unlock(); }
    void onSyncFrameReadyLocked(SyncFrameReason reason) override UBIQUITTY_REQUIRES(Ubiquitty::modelLockRole)
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
    if (!loopEndpoint || !floodEndpoint || !prepareAdoptionBeforeIngest(runtime) || !runtime.adoptStream(std::move(stream)).has_value())
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
    outcome.didReachTail = withModelAfterIngest(runtime, [&](const Ubiquitty::Terminal* terminal) {
        if (!terminal || !terminal->grid)
            return false;
        for (std::size_t column = 0; column < tail.size(); ++column) {
            const auto* cell = terminal->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(static_cast<int>(column)));
            if (!cell || cell->codePoint != static_cast<std::uint32_t>(tail[column]))
                return false;
        }
        return true;
    });
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

// A writer the machine starves refills the tty after a pass's refill band has
// run out, so every pass ends at a would-block while the writer stays far
// ahead of the parser. Its passes are one flood once they drain a pass's
// capacity with the loop waiting less in all than the reader's quiet budget,
// and the flood rides the reader.
TEST(testPassesThatEachEndAtAWouldBlockHandAFastWriterToTheReader)
{
    // 64 KB between pauses, each one would-block longer than a pass's refill
    // band re-reads: it ends the pass that meets it, and the next pass finds
    // the writer's next 64 KB.
    std::string flood;
    constexpr std::string_view line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    while (flood.size() < 3 * static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes))
        flood += line;
    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/ubiquitty-paused-XXXXXX";
    Ubiquitty::PAL::UnixFileDescriptor file(::mkstemp(path.data()));
    REQUIRE(static_cast<bool>(file));
    ::unlink(path.c_str());
    REQUIRE(::write(file.get(), flood.data(), flood.size()) == static_cast<ssize_t>(flood.size()));
    REQUIRE(::lseek(file.get(), 0, SEEK_SET) == 0);

    TerminalRuntime runtime;
    RuntimeInitParameters parameters;
    parameters.rows = 24;
    parameters.columns = 80;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto stream = std::make_unique<Ubiquitty::PAL::PrefilledStream>(std::move(file), Ubiquitty::PAL::ReadShape { .pauseEveryBytes = 64 * 1024, .pauseReads = drainQuietSpinBudget + 1 });
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint = stream->duplicateAsPollableStream();
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> floodEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr && floodEndpoint != nullptr);
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
    REQUIRE(runtime.adoptStream(std::move(stream)).has_value());

    LockOnlyDrainHost host;
    IngestLoop loop(runtime, host);
    std::atomic<std::uint64_t> deliveredBytes { 0 };
    loop.setDeliverHook(
        [](void* context, const DrainOutcome& outcome, std::uint32_t) {
            static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(static_cast<std::uint64_t>(outcome.bytesDrained), std::memory_order_relaxed);
        },
        &deliveredBytes);
    (void)loop.prepare();
    loop.adoptFloodEndpoint(std::move(floodEndpoint));
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });
    for (int waited = 0; waited < 20000 && deliveredBytes.load(std::memory_order_relaxed) < flood.size(); waited += 5)
        ::usleep(5 * 1000);
    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    runtime.close();
    CHECK_EQ(deliveredBytes.load(std::memory_order_relaxed), static_cast<std::uint64_t>(flood.size()));
    CHECK_MSG(loop.floodBurstCount() >= 1, "passes that each ended at a would-block handed the writer to the reader");
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

// A writer the machine starves keeps a flood's reader alive while it arrives
// in slices a nap apart, so every pass the reader closes ends at a
// would-block. The presentation budget runs across those closes: a pass that
// ends after the frame stayed held for the interval is offered to the rule
// and captured where it is admitted, and frames keep reaching the screen at
// the interval rather than only when the rule's clock lands between bites.
TEST(testAStarvedWritersFloodKeepsTakingFramesAtTheInterval)
{
    class StarvedWriterDrainHost final : public DrainHost {
    public:
        void lockModel() override UBIQUITTY_ACQUIRES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.lock(); }
        void unlockModel() override UBIQUITTY_RELEASES(Ubiquitty::modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS { m_lock.unlock(); }
        void onSyncFrameReadyLocked(SyncFrameReason reason) override
        {
            if (isCounting.load(std::memory_order_relaxed) && reason == SyncFrameReason::ContinuingPass)
                capturedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        std::uint32_t frameIntervalMicroseconds() const override { return 8'000; }

        std::atomic<bool> isCounting { false };
        std::atomic<int> capturedFrames { 0 };

    private:
        std::mutex m_lock;
    };

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
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> floodEndpoint = stream->duplicateAsPollableStream();
    REQUIRE(loopEndpoint != nullptr && floodEndpoint != nullptr);
    REQUIRE(prepareAdoptionBeforeIngest(runtime));
    REQUIRE(runtime.adoptStream(std::move(stream)).has_value());

    StarvedWriterDrainHost host;
    IngestLoop loop(runtime, host);
    std::atomic<std::uint64_t> deliveredBytes { 0 };
    loop.setDeliverHook(
        [](void* context, const DrainOutcome& outcome, std::uint32_t) {
            static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(static_cast<std::uint64_t>(outcome.bytesDrained), std::memory_order_relaxed);
        },
        &deliveredBytes);
    (void)loop.prepare();
    loop.adoptFloodEndpoint(std::move(floodEndpoint));
    std::jthread ingest([&loop, &loopEndpoint] { loop.run(loopEndpoint->handle()); });

    // A writer ahead of the parser fills a pass, which hands the burst to the
    // reader.
    std::string burst;
    constexpr std::string_view line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    while (burst.size() < 2 * static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes))
        burst += line;
    std::size_t written = 0;
    while (written < burst.size()) {
        const ssize_t count = ::write(writeEnd.get(), burst.data() + written, burst.size() - written);
        REQUIRE(count > 0);
        written += static_cast<std::size_t>(count);
    }

    // Then it is starved: a slice of truecolour cells every millisecond and a
    // half, far inside the reader's quiet budget and the read's quiescence,
    // and past a consumer nap, so once the backlog is parsed every pass
    // closes at a would-block. Each slice ends inside a sequence, so the
    // rule's clock alone releases the frame only at its deadline.
    std::string slice;
    while (slice.size() < 250)
        slice += "\x1b[38;2;111;222;123mx";
    slice.resize(250);
    REQUIRE(slice.back() != 'x');
    const auto dripStart = std::chrono::steady_clock::now();
    const auto elapsed = [&dripStart] { return std::chrono::steady_clock::now() - dripStart; };
    std::uint64_t napsBeforeEnd = 0;
    while (elapsed() < std::chrono::milliseconds(600)) {
        REQUIRE(::write(writeEnd.get(), slice.data(), slice.size()) == static_cast<ssize_t>(slice.size()));
        ::usleep(1500);
        if (!host.isCounting.load(std::memory_order_relaxed) && elapsed() >= std::chrono::milliseconds(200))
            host.isCounting.store(true, std::memory_order_relaxed);
        if (!napsBeforeEnd && elapsed() >= std::chrono::milliseconds(550))
            napsBeforeEnd = loop.floodNapCount();
    }
    host.isCounting.store(false, std::memory_order_relaxed);
    const std::uint64_t bursts = loop.floodBurstCount();
    const std::uint64_t napsAtEnd = loop.floodNapCount();
    loop.requestStop();
    loop.raiseWake();
    ingest.join();
    runtime.close();
    CHECK_MSG(bursts == 1 && napsAtEnd > napsBeforeEnd, "the starved writer stayed inside the reader's burst to the end");
    // About 45 in the 400 ms counted at the 8 ms interval; the deadline alone
    // gives about 12.
    CHECK_MSG(host.capturedFrames.load(std::memory_order_relaxed) >= 20,
        "a starved writer's flood took a captured frame about every interval, not only at the rule's deadline");
}

namespace {

// A sequential ingest loop over a pipe the test writes into, with a live
// resize in progress, for the erase hold's quiet rule.
struct ResizeRedrawLoop {
    Ubiquitty::PAL::UnixFileDescriptor writeEnd;
    TerminalRuntime runtime;
    LockOnlyDrainHost host;
    std::unique_ptr<Ubiquitty::PAL::PollableByteStream> loopEndpoint;
    std::unique_ptr<IngestLoop> loop;
    std::jthread ingest;

    bool start()
    {
        int pipeDescriptors[2] = { -1, -1 };
        if (::pipe(pipeDescriptors) != 0)
            return false;
        Ubiquitty::PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
        writeEnd = Ubiquitty::PAL::UnixFileDescriptor(pipeDescriptors[1]);
        if (::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK) != 0)
            return false;
        RuntimeInitParameters parameters;
        parameters.rows = 24;
        parameters.columns = 80;
        parameters.allowReflowOnInit = false;
        runtime.init(parameters);
        runtime.setLiveResize(LiveResize::Active);
        auto stream = std::make_unique<Ubiquitty::PAL::PipeStream>(std::move(readEnd));
        loopEndpoint = stream->duplicateAsPollableStream();
        if (!loopEndpoint || !runtime.adoptStream(std::move(stream)).has_value())
            return false;
        loop = std::make_unique<IngestLoop>(runtime, host);
        (void)loop->prepare();
        ingest = std::jthread([this] { loop->run(loopEndpoint->handle()); });
        return true;
    }

    void write(std::string_view bytes) const
    {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(writeEnd.get(), bytes.data() + written, bytes.size() - written);
            if (count <= 0)
                return;
            written += static_cast<std::size_t>(count);
        }
    }

    // The newest whole-screen erase's generation, read before a test writes one.
    std::uint32_t eraseGeneration()
    {
        host.lockModel();
        const std::uint32_t generation = runtime.eraseGeneration();
        host.unlockModel();
        return generation;
    }

    // Until the ingest thread parses an erase the test wrote, the model still
    // shows the screen before it, and the redraw the runtime reports on is the
    // previous erase's.
    struct RedrawState {
        bool isEraseParsed = false;
        bool isWhole = false;
    };

    RedrawState redrawState(std::uint32_t generationBefore)
    {
        host.lockModel();
        RedrawState state;
        state.isEraseParsed = runtime.eraseGeneration() != generationBefore;
        state.isWhole = state.isEraseParsed && runtime.isEraseRedrawWhole();
        host.unlockModel();
        return state;
    }

    // Microseconds until the redraw behind an erase written after
    // `generationBefore` is reported whole, or -1 past `budget`.
    long microsecondsUntilWhole(std::uint32_t generationBefore, std::chrono::microseconds budget)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < budget) {
            if (redrawState(generationBefore).isWhole)
                return static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            ::usleep(100);
        }
        return -1;
    }

    void stop()
    {
        loop->requestStop();
        loop->raiseWake();
        ingest.join();
        runtime.close();
    }
};

std::string repaintRows(int firstRow, int rowCount, char fill)
{
    std::string rows;
    for (int row = firstRow; row < firstRow + rowCount; ++row)
        rows += "\x1b[" + std::to_string(row + 1) + ";1H" + std::string(79, fill);
    return rows;
}

void spinFor(std::chrono::microseconds span)
{
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < span) {
    }
}

} // namespace

// A program repaints the screen in slices a writer's refill apart. The loop
// reports the redraw whole only once the transport has stayed empty for the
// quiet interval after the last byte it drained, so while each slice's write
// ends within the interval of the previous write's start, the redraw stays
// arriving from the moment its erase is parsed. A writer the machine
// deschedules for longer than that between two slices is the limit the stall
// test pins, and its attempt is not counted. After the last slice the redraw
// is whole within the hold's watchdog.
TEST(testARedrawIsWholeOnlyOnceTheTransportStaysQuietAfterIt)
{
    ResizeRedrawLoop harness;
    REQUIRE(harness.start());
    const auto quiet = std::chrono::microseconds(TerminalRuntime::eraseRedrawQuietMicroseconds);
    int countedChecks = 0;
    bool wasWholeInACountedAttempt = false;
    long slowestUntilWhole = 0;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint32_t generationBefore = harness.eraseGeneration();
        auto previousWriteStart = std::chrono::steady_clock::now();
        harness.write("\x1b[H\x1b[2J" + repaintRows(0, 6, 'a'));
        auto widestSpan = std::chrono::steady_clock::duration::zero();
        int parsedChecks = 0;
        bool wasWhole = false;
        for (int slice = 1; slice < 4; ++slice) {
            spinFor(std::chrono::microseconds(200));
            const ResizeRedrawLoop::RedrawState state = harness.redrawState(generationBefore);
            parsedChecks += state.isEraseParsed;
            wasWhole = wasWhole || state.isWhole;
            const auto writeStart = std::chrono::steady_clock::now();
            harness.write(repaintRows(slice * 6, 6, static_cast<char>('a' + slice)));
            widestSpan = std::max(widestSpan, std::chrono::steady_clock::now() - previousWriteStart);
            previousWriteStart = writeStart;
        }
        if (widestSpan < quiet - std::chrono::microseconds(10)) {
            countedChecks += parsedChecks;
            wasWholeInACountedAttempt = wasWholeInACountedAttempt || wasWhole;
        }
        const long untilWhole = harness.microsecondsUntilWhole(generationBefore, std::chrono::milliseconds(200));
        slowestUntilWhole = untilWhole < 0 ? -1 : std::max(slowestUntilWhole, untilWhole);
        if (untilWhole < 0)
            break;
    }
    harness.stop();
    CHECK_MSG(countedChecks > 0, "some attempt's slices all came within the quiet interval, and its erase was parsed before a check");
    CHECK_MSG(!wasWholeInACountedAttempt, "slices that keep coming within the quiet interval keep the redraw arriving");
    CHECK_MSG(slowestUntilWhole >= 0, "every redraw is reported whole once the writer stops");
    CHECK_MSG(slowestUntilWhole < 50'000, "and within the hold's watchdog");
}

// A live resize asks for a repaint on every step: each step's redraw is
// reported whole a quiet interval and a wake-up after its last byte, so the
// hold lifts within the step's own frame and the resize keeps presenting at the
// display's rate. A step whose wake-up the machine delays is still whole
// within the hold's watchdog.
TEST(testEveryResizeStepsRedrawIsWholeWithinItsFrame)
{
    ResizeRedrawLoop harness;
    REQUIRE(harness.start());
    std::vector<long> untilWhole;
    for (int step = 0; step < 8; ++step) {
        const std::uint32_t generationBefore = harness.eraseGeneration();
        const auto stepStart = std::chrono::steady_clock::now();
        harness.write("\x1b[H\x1b[2J" + repaintRows(0, 24, static_cast<char>('a' + step)));
        untilWhole.push_back(harness.microsecondsUntilWhole(generationBefore, std::chrono::milliseconds(50)));
        const auto elapsed = std::chrono::steady_clock::now() - stepStart;
        if (elapsed < std::chrono::milliseconds(16))
            std::this_thread::sleep_for(std::chrono::milliseconds(16) - elapsed);
    }
    harness.stop();
    CHECK_MSG(std::ranges::none_of(untilWhole, [](long micros) { return micros < 0; }), "every step's redraw is whole within the hold's watchdog");
    std::ranges::sort(untilWhole);
    CHECK_MSG(untilWhole[untilWhole.size() / 2] < 16'000, "and the median step's within its frame");
}

// The quiet interval's lower bound, pinned: a writer the machine deschedules
// mid-redraw for longer than the interval has the redraw reported whole
// during the stall, on every resize step, and the rest of each repaint then
// reaches the display as it arrives.
TEST(testAWriterStalledLongerThanTheQuietIntervalHasItsRedrawCalledWhole)
{
    ResizeRedrawLoop harness;
    REQUIRE(harness.start());
    const auto stall = std::chrono::microseconds(TerminalRuntime::eraseRedrawQuietMicroseconds) + std::chrono::milliseconds(3);
    int stepsWholeMidRedraw = 0;
    for (int step = 0; step < 4; ++step) {
        const std::uint32_t generationBefore = harness.eraseGeneration();
        const auto stepStart = std::chrono::steady_clock::now();
        harness.write("\x1b[H\x1b[2J" + repaintRows(0, 12, static_cast<char>('a' + step)));
        stepsWholeMidRedraw += harness.microsecondsUntilWhole(generationBefore, std::chrono::milliseconds(200)) >= 0;
        const auto stalled = std::chrono::steady_clock::now() - stepStart;
        if (stalled < stall)
            std::this_thread::sleep_for(stall - stalled);
        harness.write(repaintRows(12, 12, static_cast<char>('a' + step)));
        const auto elapsed = std::chrono::steady_clock::now() - stepStart;
        if (elapsed < std::chrono::milliseconds(16))
            std::this_thread::sleep_for(std::chrono::milliseconds(16) - elapsed);
    }
    harness.stop();
    CHECK_MSG(stepsWholeMidRedraw == 4, "a stall longer than the quiet interval is taken for the end of the redraw");
}

UBIQUITTY_TEST_MAIN()
