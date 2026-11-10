// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_flood_start — how soon the drain captures a frame once a flood begins.
//
// A writer thread replays the sgr_churn shape, every cell under its own
// truecolour SGR redrawn in place, into a pipe the session's ingest loop
// reads, and the case waits for the first frame the drain captures. The clock
// starts at the writer's first write: the stream is built before then, so the
// time measured is the drain's alone, from the bytes being offered to a frame
// a frontend can present. Both geometries run, since 80x24 and 200x50 take
// different scroll paths, with and without a selection standing, which the
// drain resolves into each frame it captures.

#include "Core/Rendering/FrameCapture.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace Ubiquitty::PAL {

// The read end of a pipe the case's writer fills, read nonblocking: an empty
// pipe answers Again and readiness is the pipe's own, as a pty's is.
class FloodPipeEndpoint final : public PollableByteStream {
public:
    explicit FloodPipeEndpoint(UnixFileDescriptor descriptor)
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
        return std::make_unique<FloodPipeEndpoint>(std::move(duplicate));
    }

private:
    UnixFileDescriptor m_descriptor;
};

// A session transport over that endpoint. What the session writes back is
// accepted and dropped.
class FloodPipeStream final : public Stream, private ProcessFacts {
public:
    explicit FloodPipeStream(UnixFileDescriptor descriptor)
        : m_rawDescriptor(descriptor.get())
        , m_endpoint(std::move(descriptor))
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
    FloodPipeEndpoint m_endpoint;
};

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty;
using Clock = std::chrono::steady_clock;
using Rendering::FrameCapture;
using Runtime::TerminalSession;
using namespace std::chrono_literals;

// A taken frame with its release bound in.
using TakenFrame = std::unique_ptr<FrameCapture, decltype([](FrameCapture* frame) { releaseFrameCapture(frame); })>;

constexpr double refreshSeconds = 1.0 / 60.0;
constexpr Clock::duration refreshInterval = std::chrono::microseconds(16'667);

// Whether a sanitizer instruments this build, Core compiled with it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool isCoreInstrumented = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
constexpr bool isCoreInstrumented = true;
#else
constexpr bool isCoreInstrumented = false;
#endif
#else
constexpr bool isCoreInstrumented = false;
#endif

// The longest from a flood's first write to the first frame the drain
// captures of it: six refresh intervals. An unoptimized build captures inside
// one, so the bound is for a loaded machine, not a slow build; a sanitized
// Core parses several times slower, and its bound is three times as long.
constexpr Clock::duration firstCaptureBound = (isCoreInstrumented ? 18 : 6) * refreshInterval;
// How long a case waits for a capture before it reports none.
constexpr Clock::duration captureDeadline = 2s;

// One frame of a program that redraws every row in place, each cell under a
// truecolour SGR, ending at the home position.
std::string sgrChurnFrame(int rows, int columns, int frame)
{
    std::string bytes = "\x1b[H";
    for (int row = 0; row < rows; ++row) {
        if (row > 0)
            bytes += "\r\n";
        for (int column = 0; column < columns; ++column) {
            const int shade = (frame * 7 + row * 3 + column) & 0xff;
            bytes += std::format("\x1b[38;2;{};{};{}m{}", shade, 255 - shade, (shade * 5) & 0xff, static_cast<char>('a' + (frame + column) % 26));
        }
    }
    bytes += "\x1b[m\x1b[H";
    return bytes;
}

std::string sgrChurnRecording(int rows, int columns)
{
    std::string bytes;
    for (int frame = 1; frame <= 16; ++frame)
        bytes += sgrChurnFrame(rows, columns, frame);
    return bytes;
}

enum class StandingSelection : std::uint8_t {
    Absent,
    Present,
};

struct FloodStart {
    bool didStart = false;
    // From the writer's first write to the first capture, if one came.
    std::optional<Clock::duration> untilFirstCapture;
};

FloodStart measureFloodStart(int rows, int columns, StandingSelection standingSelection)
{
    FloodStart start;
    int pipeDescriptors[2] = { -1, -1 };
    if (::pipe(pipeDescriptors) != 0)
        return start;
    PAL::UnixFileDescriptor readEnd(pipeDescriptors[0]);
    PAL::UnixFileDescriptor writeEnd(pipeDescriptors[1]);
    // Both ends nonblocking: the writer retries a full pipe until the case
    // stops it, so it never waits on a pipe nobody reads.
    for (const int descriptor : { readEnd.get(), writeEnd.get() }) {
        if (::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK) != 0)
            return start;
    }

    TerminalSession* session = TerminalSession::make();
    if (!session)
        return start;
    struct SessionRelease {
        TerminalSession* session;
        ~SessionRelease()
        {
            session->close();
            releaseTerminalSession(session);
        }
    } release { session };
    session->start(rows, columns);
    if (!session->adoptStream(std::make_unique<PAL::FloodPipeStream>(std::move(readEnd))).has_value())
        return start;
    session->setSyncPublishRefreshSeconds(refreshSeconds);
    Runtime::SyncFrameCaptureGeometry geometry;
    geometry.metrics.cellWidth = 8.0f;
    geometry.metrics.cellHeight = 16.0f;
    geometry.metrics.baselineFromTop = 13.0f;
    geometry.metrics.underlineThickness = 1.0f;
    geometry.metrics.underlineDrop = 1.0f;
    geometry.metrics.strikeThickness = 1.0f;
    geometry.metrics.xHeight = 10.0f;
    geometry.glyphSlot = { 32, 8, 16, 6, { 1, 1 } };
    geometry.viewportSizeInPixels = { 8.0f * static_cast<float>(columns), 16.0f * static_cast<float>(rows) };
    session->setSyncFrameCaptureGeometry(geometry);
    session->setSyncFramePresentation(Runtime::PanePresentation::Presented);
    if (!session->startIngest())
        return start;
    struct IngestStop {
        TerminalSession* session;
        ~IngestStop() { session->stopIngest(); }
    } ingestStop { session };

    // The screen the flood starts over, parsed whole, and the frame the drain
    // took of it handed back, so the next capture is the flood's.
    const std::string prologue = sgrChurnFrame(rows, columns, 0);
    std::size_t prologueWritten = 0;
    std::uint64_t drained = 0;
    for (int waited = 0; waited < 2000 && drained < prologue.size(); ++waited) {
        if (prologueWritten < prologue.size()) {
            const ssize_t count = ::write(writeEnd.get(), prologue.data() + prologueWritten, prologue.size() - prologueWritten);
            if (count > 0)
                prologueWritten += static_cast<std::size_t>(count);
        }
        drained += session->takeDrainedBytes();
        std::this_thread::sleep_for(1ms);
    }
    if (drained < prologue.size())
        return start;
    std::this_thread::sleep_for(100ms);
    static_cast<void>(TakenFrame { session->takeSyncFrame() });

    Selection::Session selection;
    Selection::init(selection);
    if (standingSelection == StandingSelection::Present) {
        session->lockForRead();
        if (Terminal* terminal = session->terminal()) {
            Selection::beginGesture(selection, *terminal, rows / 3, 2, 1, 0, {});
            Selection::dragGesture(selection, *terminal, rows / 3, columns / 2, 0);
            Selection::endGesture(selection);
        }
        session->unlockForRead();
        if (!Selection::hasSelection(selection))
            return start;
        session->publishSelectionForCaptures(&selection);
    }
    start.didStart = true;

    const std::string recording = sgrChurnRecording(rows, columns);
    std::atomic<bool> isWriterStopping { false };
    std::atomic<std::int64_t> firstWriteMicroseconds { -1 };
    std::jthread writer([&] {
        PAL::raiseCurrentThreadToInteractivePriority();
        firstWriteMicroseconds.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(),
            std::memory_order_release);
        while (!isWriterStopping.load(std::memory_order_relaxed)) {
            std::size_t offset = 0;
            while (offset < recording.size() && !isWriterStopping.load(std::memory_order_relaxed)) {
                const ssize_t count = ::write(writeEnd.get(), recording.data() + offset, recording.size() - offset);
                if (count > 0)
                    offset += static_cast<std::size_t>(count);
                else if (count < 0 && (errno == EAGAIN || errno == EINTR))
                    std::this_thread::sleep_for(100us);
                else
                    return;
            }
        }
    });

    const Clock::time_point waitedFrom = Clock::now();
    while (Clock::now() - waitedFrom < captureDeadline) {
        if (const TakenFrame frame { session->takeSyncFrame() }) {
            const Clock::time_point capturedAt = Clock::now();
            const Clock::time_point firstWriteAt { std::chrono::microseconds(firstWriteMicroseconds.load(std::memory_order_acquire)) };
            start.untilFirstCapture = capturedAt - firstWriteAt;
            break;
        }
        std::this_thread::sleep_for(500us);
    }
    isWriterStopping.store(true, std::memory_order_relaxed);
    writer.join();
    return start;
}

double millisecondsOf(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

void checkFloodStart(int rows, int columns, StandingSelection standingSelection)
{
    const FloodStart start = measureFloodStart(rows, columns, standingSelection);
    const char* const selection = standingSelection == StandingSelection::Present ? "with a selection standing" : "with no selection";
    REQUIRE_MSG(start.didStart, std::format("{}x{} {}: the session did not start", columns, rows, selection).c_str());
    REQUIRE_MSG(start.untilFirstCapture.has_value(),
        std::format("{}x{} {}: the drain captured no frame in {:.0f} ms of flood", columns, rows, selection, millisecondsOf(captureDeadline)).c_str());
    CHECK_MSG(*start.untilFirstCapture <= firstCaptureBound,
        std::format("{}x{} {}: the first frame was captured {:.1f} ms after the flood's first write, against {:.1f} ms", columns, rows,
            selection, millisecondsOf(*start.untilFirstCapture), millisecondsOf(firstCaptureBound))
            .c_str());
}

} // namespace

TEST(aFloodAtEightyColumnsIsCapturedSoonAfterItsFirstWrite)
{
    checkFloodStart(24, 80, StandingSelection::Absent);
    checkFloodStart(24, 80, StandingSelection::Present);
}

TEST(aFloodAtTwoHundredColumnsIsCapturedSoonAfterItsFirstWrite)
{
    checkFloodStart(50, 200, StandingSelection::Absent);
    checkFloodStart(50, 200, StandingSelection::Present);
}

UBIQUITTY_TEST_MAIN()
