// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Terminal.h"
#include "Harness.h"

//
// Tests/Core/Runtime/test_runtime.cpp — host unit test for the portable
// Ubiquitty::Runtime::TerminalRuntime.
//
// Links the real Core + Source/PAL/PAL.cpp (the portable, non-stream PAL)
// and supplies its OWN observable Ubiquitty::PAL::Stream below: the production
// stream lives in the frontend, and the shared test stubs implement neither
// streamWriteAll nor any reply-byte inspector. The double records written
// bytes, the last window size, and a settable pending-write flag so the test
// can assert the runtime forwards correctly.
//

#include "Core/Preferences/Preferences.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Runtime/DrainHostPacing.h"
#include "Core/Runtime/HostDirectoryClient.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Runtime/KeyEncode.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/SearchCounterText.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Runtime/WorkspaceSession.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionLocation.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/StatedPane.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/Themes.h"
#include "Core/Title/ProcessIcons.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

// --- observable PAL::Stream double ------------------------------------------
// The runtime drives PAL::Stream through its virtual methods, so the test
// SUBCLASSES it: StreamTestPipe records writes, replays a scripted inbound
// queue, and counts its own destruction (the RAII-ownership probe). Core +
// PAL.cpp leave the stream seam to the frontend, so openPTY() (named by launch())
// is a no-op; the tests inject pipes via adoptStream(), never the factory.

namespace Ubiquitty::PAL {

static int g_streamDestroyCount = 0; // ~StreamTestPipe bumps it — the ownership probe

class StreamTestPipe final : public Stream, private ProcessFacts {
public:
    std::string written; // every write()/writeAll() payload (replies + input)
    std::string toRead; // inbound queue: the test pushes, read() drains
    std::size_t readPosition = 0;
    int readCallCount = 0; // every read() call — observes the mid-burst backoff
    std::size_t maxReadBytes = std::numeric_limits<std::size_t>::max();
    bool eofPending = false; // set to simulate the peer closing
    int windowColumns = -1;
    int windowRows = -1;
    int windowXpixel = -1;
    int windowYpixel = -1;
    int treeNotifierInstallations = 0;
    int scriptedFileDescriptor = -1;
    int pendingWriteDrainCalls = 0;
    ProcessClosePolicy closePolicy = ProcessClosePolicy::Terminate;
    int duplicateCalls = 0;
    bool pendingWrite = false; // test-driven backpressure state
    std::size_t remainingWriteCapacity = std::numeric_limits<std::size_t>::max();
    std::optional<ProcessID> foregroundGroup; // scripted tcgetpgrp double (stale-mouse guard)
    bool foregroundShell = true; // scripted at-prompt double (stale-mouse guard)
    /// The directory the foreground process is sampled in. Empty unless a case
    /// scripts one, and an empty sample is one the drain's probe ignores.
    std::string foregroundDirectory;
    std::optional<LineDiscipline> discipline;
    int disciplineProbes = 0;
    std::optional<LineDiscipline> lineDiscipline() override
    {
        ++disciplineProbes;
        return discipline;
    }

    ~StreamTestPipe() override
    {
        if (m_releaseTreeContext) {
            m_releaseTreeContext(m_treeContext);
        }
        ++g_streamDestroyCount;
    }

    void setTreeChangeNotifier(bool (*notify)(void*), void* context, void (*releaseContext)(void*)) override
    {
        ++treeNotifierInstallations;
        m_treeNotify = notify;
        m_treeContext = context;
        m_releaseTreeContext = releaseContext;
    }

    bool notifyTreeEdge() { return m_treeNotify(m_treeContext); }

private:
    bool (*m_treeNotify)(void*) = nullptr;
    void* m_treeContext = nullptr;
    void (*m_releaseTreeContext)(void*) = nullptr;

public:
    int read(std::span<char> buffer) override
    {
        ++readCallCount;
        if (buffer.empty())
            return static_cast<int>(StreamStatus::Fatal);
        const std::size_t available = toRead.size() - readPosition;
        if (available == 0)
            return static_cast<int>(eofPending ? StreamStatus::EndOfFile : StreamStatus::Again);
        const std::size_t want = std::min({ buffer.size(), available, maxReadBytes });
        std::memcpy(buffer.data(), toRead.data() + readPosition, want);
        readPosition += want;
        return static_cast<int>(want);
    }
    int write(std::span<const char> bytes) override
    {
        written.append(bytes.data(), bytes.size());
        return static_cast<int>(bytes.size());
    }
    int writeAll(std::span<const char> bytes) override
    {
        if (bytes.size() > remainingWriteCapacity) {
            pendingWrite = true;
            return static_cast<int>(StreamStatus::Again);
        }
        remainingWriteCapacity -= bytes.size();
        written.append(bytes.data(), bytes.size());
        return 0; // PAL contract: 0 == fully written/queued
    }
    bool drainPendingWrite() override
    {
        ++pendingWriteDrainCalls;
        pendingWrite = false; // the double flushes in one shot
        return false; // nothing remains
    }
    [[nodiscard]] bool hasPendingWrite() const override { return pendingWrite; }
    void setWindowSize(int columns, int rows, int xPixels, int yPixels) override
    {
        windowColumns = columns;
        windowRows = rows;
        windowXpixel = xPixels;
        windowYpixel = yPixels;
    }
    /// How many times the runtime re-sent a size with the signal that makes a
    /// program read it again.
    int reassertCount = 0;
    /// How many of those re-sends would have signalled, the way a pseudo-terminal
    /// decides it: only while the group the re-send was decided for is still the
    /// one holding the terminal.
    int reassertSignalCount = 0;
    /// The group the last re-send named as the one it was decided for.
    ProcessID reassertExpectedGroup = 0;
    void reassertWindowSize(int columns, int rows, int xPixels, int yPixels, ProcessID expectedForegroundGroup) override
    {
        ++reassertCount;
        reassertExpectedGroup = expectedForegroundGroup;
        if (expectedForegroundGroup > 1 && expectedForegroundGroup == foregroundGroup.value_or(0))
            ++reassertSignalCount;
        setWindowSize(columns, rows, xPixels, yPixels);
    }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] int fileDescriptor() const override { return scriptedFileDescriptor; } // no polling occurs against scripted descriptors
    void setProcessClosePolicy(ProcessClosePolicy policy) override { closePolicy = policy; }
    std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        ++duplicateCalls;
        return nullptr;
    }
    ProcessFacts& process() override { return *this; }

    // ProcessFacts — scripted foreground facts; the rest is inert (the runtime
    // tests exercise no title/proc walk).
    /// The foreground process's name, empty unless a case scripts one.
    std::string scriptedForegroundName;
    /// A foreground that takes the terminal once a name is read: the edge
    /// landing between the probe of the group and its holder and the read of
    /// the name after them, which still answers for what stood before it.
    struct ForegroundMove {
        std::optional<ProcessID> group;
        bool isShell = true;
        std::string name;
    };
    std::vector<ForegroundMove> movesAsNamesAreRead;
    std::string foregroundName() override
    {
        std::string name = scriptedForegroundName;
        if (!movesAsNamesAreRead.empty()) {
            foregroundGroup = movesAsNamesAreRead.front().group;
            foregroundShell = movesAsNamesAreRead.front().isShell;
            scriptedForegroundName = movesAsNamesAreRead.front().name;
            movesAsNamesAreRead.erase(movesAsNamesAreRead.begin());
        }
        return name;
    }
    int foregroundShellCalls { 0 };
    bool foregroundIsShell() override
    {
        ++foregroundShellCalls;
        return foregroundShell;
    }
    int foregroundGroupCalls { 0 };
    /// How many of the next group reads land a process-tree edge right after
    /// the read, the moment an edge can fall between a probe's read and its
    /// clearing of the owed flag.
    int edgesAfterGroupRead { 0 };
    std::optional<ProcessID> foregroundProcessGroup() override
    {
        ++foregroundGroupCalls;
        const std::optional<ProcessID> group = foregroundGroup;
        if (edgesAfterGroupRead > 0) {
            --edgesAfterGroupRead;
            (void)notifyTreeEdge();
        }
        return group;
    }
    std::string foregroundCurrentDirectory() override { return foregroundDirectory; }
    std::string ttyName() override { return {}; }
    /// Whether the session's own child is alive, false unless a case scripts it.
    bool runningChild = false;
    bool hasRunningChild() override { return runningChild; }
    // Scripted subtree. Empty by default, so every case that does not set one
    // sees an inert walk.
    std::vector<PAL::Process::Record> scriptedRecords;
    int gatherProcessesCalls = 0;
    int gatherProcesses(std::span<PAL::Process::Record> out) override
    {
        ++gatherProcessesCalls;
        const int count = static_cast<int>(std::min(scriptedRecords.size(), out.size()));
        for (int index = 0; index < count; ++index)
            out[static_cast<std::size_t>(index)] = scriptedRecords[static_cast<std::size_t>(index)];
        return count;
    }
};

// Frontend-defined (not in PAL.cpp); drainAvailable resolves the bell with it. A
// fixed value is fine for the drain tests, which inject no bell; a case that
// ages a 2026 block pins the tick through the variable the shipping clock reads.
Ticks monotonicTicks()
{
    if (const char* pinned = std::getenv("UBIQUITTY_FAKE_TICKS"))
        return Ticks(static_cast<std::uint32_t>(std::strtoul(pinned, nullptr, 10)));
    return Ticks(0);
}

// drainAvailable's mid-burst refill probe waits on this clock. Advancing it
// by a microsecond per call keeps the spin loop finite under the stub.
static uint32_t s_fakeMicroseconds = 0;
uint32_t monotonicMicros()
{
    return ++s_fakeMicroseconds;
}

void clipboardSetText(std::string_view)
{
    CHECK_MSG(false, "input reporting without a selection must not copy to the clipboard");
}

} // namespace Ubiquitty::PAL

// --- event-sink double ------------------------------------------------------
namespace {

using namespace Ubiquitty::Runtime;

struct CountingSink : EventSink {
    int screenDirty = 0;
    int exited = 0;
    int bell = 0;
    int titleChanged = 0;
    int fileTransferDetected = 0;
    std::uint32_t lastFileTransferData0 = 0;
    int deployConfirmationPending = 0;
    int pastePushSettled = 0;
    std::uint32_t lastPastePushStatus = 0;
    int remoteStateChanged = 0;
    std::uint32_t lastRemoteState = 0;
    int remoteSessionEstablished = 0;
    std::uint32_t lastRemoteSessionID = 0;
    int consentRequestPending = 0;
    int presenceChanged = 0;
    int directoryChanged = 0;
    int workspaceChanged = 0;
    int processStateChanged = 0;
    int progressChanged = 0;
    int fontSwapRequested = 0;
    int windowRequestPending = 0;
    int notificationPosted = 0;
    int tabStatusChanged = 0;
    int dragDropPending = 0;
    void onRuntimeEvent(const Event& event) override
    {
        switch (event.kind) {
        case EventKind::ScreenDirty:
            ++screenDirty;
            break;
        case EventKind::Exited:
            ++exited;
            break;
        case EventKind::Bell:
            ++bell;
            break;
        case EventKind::TitleChanged:
            ++titleChanged;
            break;
        case EventKind::FileTransferDetected:
            ++fileTransferDetected;
            lastFileTransferData0 = event.data0;
            break;
        case EventKind::DeployConfirmationPending:
            ++deployConfirmationPending;
            break;
        case EventKind::PastePushSettled:
            ++pastePushSettled;
            lastPastePushStatus = event.data0;
            break;
        case EventKind::RemoteStateChanged:
            ++remoteStateChanged;
            lastRemoteState = event.data0;
            break;
        case EventKind::RemoteSessionEstablished:
            ++remoteSessionEstablished;
            lastRemoteSessionID = event.data0;
            break;
        case EventKind::ConsentRequestPending:
            ++consentRequestPending;
            break;
        case EventKind::PresenceChanged:
            ++presenceChanged;
            break;
        case EventKind::DirectoryChanged:
            ++directoryChanged;
            break;
        case EventKind::HostSessionListChanged:
        case EventKind::HostSessionTerminateSettled:
            // Raised by a session's directory errands, which no case in this
            // file runs; the recorder keeps the switch exhaustive and counts
            // nothing for them.
            break;
        case EventKind::WorkspaceChanged:
            ++workspaceChanged;
            break;
        case EventKind::ProcessStateChanged:
            ++processStateChanged;
            break;
        case EventKind::ProgressChanged:
            ++progressChanged;
            break;
        case EventKind::FontSwapRequested:
            ++fontSwapRequested;
            break;
        case EventKind::WindowRequestPending:
            ++windowRequestPending;
            break;
        case EventKind::NotificationPosted:
            ++notificationPosted;
            break;
        case EventKind::TabStatusChanged:
            ++tabStatusChanged;
            break;
        case EventKind::DragDropPending:
            ++dragDropPending;
            break;
        case EventKind::SearchChanged:
            break;
        case EventKind::TriggerMatched:
        case EventKind::DeployFailed:
        case EventKind::DeployNeedsManualInstall:
        case EventKind::RemoteFileArrived:
        case EventKind::HostStructureSettled:
        case EventKind::HostedWindowsChanged:
        case EventKind::LinkStateChanged:
        case EventKind::InputInDoubtChanged:
            // Raised by the session's trigger queue, by a remote session's
            // install, file fetches, link and input, and by a hosted-window
            // source, none of which a case in this file runs; the recorder
            // keeps the switch exhaustive and counts nothing for them.
            break;
        }
    }
};

// Counts the platform hooks drainAvailable() calls; verifies lock/unlock stay
// balanced (a leaked lock would deadlock the real os_unfair_lock).
struct CountingDrainHost : DrainHost {
    int locks = 0;
    int unlocks = 0;
    int frames = 0;
    void lockModel() override { ++locks; }
    void unlockModel() override { ++unlocks; }
    std::vector<SyncFrameReason> reasons;
    void onSyncFrameReadyLocked(SyncFrameReason reason) override
    {
        ++frames;
        reasons.push_back(reason);
    }
};

RuntimeInitParameters parameters80x24()
{
    RuntimeInitParameters p;
    p.rows = 24;
    p.columns = 80;
    p.allowReflowOnInit = false;
    return p;
}

template<std::size_t N> std::size_t literalLength(const char (&)[N]) { return N - 1; } // string-literal byte length

// The coherence rule's clock, held in the test's hand: a case moves it rather
// than sleeping, so a loaded runner cannot carry a hold past a deadline the
// case means to stay inside.
struct HandClock {
    std::uint64_t nowMicroseconds = 1'000'000;

    static std::uint64_t read(void* context) { return static_cast<HandClock*>(context)->nowMicroseconds; }

    void drive(TerminalRuntime& runtime) { runtime.setCoherenceClock(&HandClock::read, this); }
};

// Lets the read go quiet and asks for the frame the rule held for it, as the
// ingest loop's timer does: the clock moves on to each instant the rule names
// until it holds nothing, or until `limit` has passed.
void settleHeldFrame(TerminalRuntime& runtime, DrainHost& host, HandClock& clock, std::uint64_t limit = Ubiquitty::Runtime::readQuiescenceMicroseconds)
{
    const std::uint64_t end = clock.nowMicroseconds + limit;
    while (const std::optional<std::uint64_t> due = runtime.publishHeldFrameIfDue(host)) {
        if (clock.nowMicroseconds + *due > end)
            return;
        clock.nowMicroseconds += std::max<std::uint64_t>(*due, 1);
    }
}

// Construct a StreamTestPipe, hand the runtime ownership, and return a borrow so the
// test can script toRead / inspect written. Valid until rt closes or is destroyed.
Ubiquitty::PAL::StreamTestPipe* attachPipe(TerminalRuntime& rt)
{
    auto pipe = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    Ubiquitty::PAL::StreamTestPipe* borrow = pipe.get();
    auto adoption = rt.adoptStream(std::move(pipe));
    if (!adoption) {
        CHECK_MSG(false, "runtime refused the test helper's pipe");
        return nullptr;
    }
    return borrow;
}

} // namespace

namespace {
std::unique_ptr<Ubiquitty::Terminal> makeBlinkTerminal(int rows = 6, int columns = 20)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}
} // namespace

// The published snapshot is what every frontend clock reads. The flag in it
// cannot retract; the reading beside it can, and a clock that consults both
// stands down when the blinking stops.
TEST(aPublishedSnapshotRetractsTheBlinkFlagItCannotClear)
{
    auto t = makeBlinkTerminal();
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;

    Ubiquitty::processBytes(t.get(), "\x1b[5mblinking\x1b[0m");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.hasBlinkHint, "the flag is armed while it blinks");
    CHECK_MSG(snapshot.hasBlinkCellOnScreen, "and the reading agrees");

    Ubiquitty::processBytes(t.get(), "\x1b[H\x1b[2J");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.hasBlinkHint, "the flag cannot retract, by design");
    CHECK_MSG(!snapshot.hasBlinkCellOnScreen, "the reading does, which is what stands a clock down");

    // The control: a terminal that never blinked publishes both false, so the
    // case above is not passing because the reading is always false.
    auto plain = makeBlinkTerminal();
    Ubiquitty::processBytes(plain.get(), "plain text");
    Ubiquitty::Runtime::RuntimeSnapshot plainSnapshot;
    Ubiquitty::Runtime::captureSnapshotFromTerm(plain.get(), 0, plainSnapshot);
    CHECK_MSG(!plainSnapshot.hasBlinkHint && !plainSnapshot.hasBlinkCellOnScreen, "nothing blinked, nothing is published");

    plain.reset();
    t.reset();
}

TEST(aPublishedSnapshotCarriesTheActiveScreenCursorForViewportFollowing)
{
    auto terminal = makeBlinkTerminal(24, 80);
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;
    Ubiquitty::processBytes(terminal.get(), "\033[12;45H");
    Ubiquitty::Runtime::captureSnapshotFromTerm(terminal.get(), 0, snapshot);
    CHECK_EQ(snapshot.cursorRow, 11);
    CHECK_EQ(snapshot.cursorColumn, 44);
    Ubiquitty::processBytes(terminal.get(), "\033[?1049h\033[3;7H\033[?25l");
    Ubiquitty::Runtime::captureSnapshotFromTerm(terminal.get(), 0, snapshot);
    CHECK_EQ(snapshot.cursorRow, 2);
    CHECK_EQ(snapshot.cursorColumn, 6);
    CHECK(!snapshot.isCursorVisible);
    Ubiquitty::processBytes(terminal.get(), "\033[?1049l");
    Ubiquitty::Runtime::captureSnapshotFromTerm(terminal.get(), 0, snapshot);
    CHECK_EQ(snapshot.cursorRow, 11);
    CHECK_EQ(snapshot.cursorColumn, 44);
}

// DECSCNM swaps the frame's DEFAULT pair, and the published snapshot carries
// the effective ground so a renderer paints what the mode asks for without
// holding a second copy of the rule.
TEST(aPublishedSnapshotCarriesTheEffectiveGroundUnderReverseVideo)
{
    auto t = makeBlinkTerminal();
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;
    constexpr float fullChannel = 65535.0F;

    Ubiquitty::processBytes(t.get(), "plain");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    const float plainRed = static_cast<float>(t->palette.defaultBackground.red) / fullChannel;
    CHECK_MSG(snapshot.defaultBackgroundRed == plainRed, "unreversed, the ground is the palette's background");

    Ubiquitty::processBytes(t.get(), "\x1b[?5h");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    const float reversedRed = static_cast<float>(t->palette.defaultForeground.red) / fullChannel;
    CHECK_MSG(snapshot.defaultBackgroundRed == reversedRed, "DECSCNM publishes the swapped ground");
    CHECK_MSG(plainRed != reversedRed, "the two grounds differ, so the case can fail");

    Ubiquitty::processBytes(t.get(), "\x1b[?5l");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.defaultBackgroundRed == plainRed, "and the reset restores it");

    t.reset();
}

namespace {

/// Feeds bytes through the runtime's sync-clear pump until all are parsed, as
/// a drain does, publishing nothing on the way.
bool feedThroughSyncClears(TerminalRuntime& runtime, std::string_view bytes)
{
    while (!bytes.empty()) {
        const FeedResult fed = runtime.feedChunkUpToSyncClear({ bytes.data(), bytes.size() });
        if (fed.consumed <= 0)
            return false;
        bytes.remove_prefix(static_cast<std::size_t>(fed.consumed));
    }
    return true;
}

bool isCoherentBackground(const TerminalRuntime& runtime, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    const Ubiquitty::ColorRGBA background = runtime.link().coherentColors.read().colors.background;
    return background.red == red && background.green == green && background.blue == blue;
}

} // namespace

// The colours a host paints its chrome from are the frame's, which a
// synchronized update holds back: published outside an update and at each
// close, never from inside one, and not withheld from a program that opens
// the next update in the read that closed the last.
TEST(theCoherentColoursAreTheFramesAndNeverAnOpenUpdates)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());

    REQUIRE(feedThroughSyncClears(runtime, "\x1b]11;rgb:12/34/56\a"));
    runtime.republishSnapshot();
    CHECK_MSG(isCoherentBackground(runtime, 0x12, 0x34, 0x56), "a colour set outside an update publishes with the snapshot");

    REQUIRE(feedThroughSyncClears(runtime, "\x1b[?2026h\x1b]11;rgb:ab/01/cd\a"));
    runtime.republishSnapshot();
    CHECK_MSG(isCoherentBackground(runtime, 0x12, 0x34, 0x56), "a snapshot inside an update keeps the colours from before it");

    REQUIRE(feedThroughSyncClears(runtime, "\x1b]11;rgb:65/43/21\a\x1b[?2026l\x1b[?2026h\x1b]11;rgb:ab/01/cd\a"));
    CHECK_MSG(isCoherentBackground(runtime, 0x65, 0x43, 0x21), "the close publishes the colours of the frame it completes");
    runtime.republishSnapshot();
    CHECK_MSG(isCoherentBackground(runtime, 0x65, 0x43, 0x21), "the update opened after it publishes nothing over them");

    REQUIRE(feedThroughSyncClears(runtime, "\x1b]11;rgb:65/43/21\a\x1b[?2026l"));
    const std::uint64_t publications = runtime.link().coherentColors.version();
    REQUIRE(feedThroughSyncClears(runtime, "\x1b[?2026hAB\x1b[?2026l"));
    CHECK_MSG(runtime.link().coherentColors.version() == publications, "a close that moved no colour reads and publishes none");
}

// DECSET 7402's edge-to-edge frame is a fact the pointer maps through on the
// main thread, between drains, where the model lock is the wrong thing to
// take. Published resolved, from both the mode and a Color letterbox, because a
// frontend reading the mode alone would zero its insets under a Passthrough
// letterbox whose grid Core still draws padded. The mode itself rides beside
// it: a fitted image fills the drawable under either letterbox, and the pixel
// report measures the image's space.
TEST(aPublishedSnapshotCarriesTheEdgeToEdgeFrame)
{
    auto t = makeBlinkTerminal();
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(!snapshot.isEdgeToEdge, "a reset terminal is padded");
    CHECK_MSG(!snapshot.modes.isFullscreenContentActive, "and holds no fullscreen content");

    Ubiquitty::processBytes(t.get(), "\x1b]7402;fill=1a2b3c\x1b\\\x1b[?7402h");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.modes.isFullscreenContentActive, "the mode is published");
    CHECK_MSG(snapshot.isEdgeToEdge, "a Color letterbox sends the frame edge to edge");
    CHECK_MSG(snapshot.letterboxColor == 0x1a2b3cU, "with the fill the margin clears to");

    Ubiquitty::processBytes(t.get(), "\x1b]7402;fill=pass\x1b\\");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.modes.isFullscreenContentActive, "Passthrough keeps the mode");
    CHECK_MSG(!snapshot.isEdgeToEdge, "and the padded grid");

    Ubiquitty::processBytes(t.get(), "\x1b]7402;fill=1a2b3c\x1b\\\x1b[?7402l");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(!snapshot.modes.isFullscreenContentActive, "the reset clears the mode");
    CHECK_MSG(!snapshot.isEdgeToEdge, "and restores the padding");

    t.reset();
}

// OSC 22's stack is Core's, and its top is a fact a frontend must read without
// the model lock: a frontend that installs no WindowClient has no callback to hear
// it through. The SET bit rides beside the
// shape because an application that pushes "default" has asked for the arrow,
// which is not the same answer as having asked for nothing.
TEST(aPublishedSnapshotCarriesTheLiveOSC22PointerShape)
{
    auto t = makeBlinkTerminal();
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;

    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(!snapshot.isAppPointerShapeSet, "an untouched terminal has asked for no shape");
    CHECK_MSG(snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Default, "and the shape beside it reads Default");

    Ubiquitty::processBytes(t.get(), "\x1b]22;pointer\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.isAppPointerShapeSet && snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Pointer,
        "a SET publishes the named shape");

    Ubiquitty::processBytes(t.get(), "\x1b]22;>wait\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Wait, "a PUSH publishes the new top");

    Ubiquitty::processBytes(t.get(), "\x1b]22;<\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Pointer, "and a POP publishes what stood under it");

    Ubiquitty::processBytes(t.get(), "\x1b]22;\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(!snapshot.isAppPointerShapeSet && snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Default,
        "an empty SET retracts both, which is what gives the pointer back");

    // The control that separates the two fields: an explicit "default" is an
    // ASK, and a snapshot carrying only the shape could not say so.
    Ubiquitty::processBytes(t.get(), "\x1b]22;default\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.isAppPointerShapeSet && snapshot.appRequestedPointerShape == Ubiquitty::MousePointerShape::Default,
        "an explicit default is set, and reads Default");

    // Tek's graphic-input crosshair rides beside the shape, so the ladder off
    // the snapshot can rank it.
    Ubiquitty::processBytes(t.get(), "\x1b[?38h\x1b\x1a");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.isTekModeActive && snapshot.isTekGraphicInputActive, "Tek GIN is published beside the mode");

    t.reset();
}

TEST(anOSC94ReportIsPublishedAndRaisesOneEdge)
{
    auto t = makeBlinkTerminal();
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;

    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.progressState == Ubiquitty::OSC::ProgressNone, "an untouched terminal reports no progress");

    Ubiquitty::processBytes(t.get(), "\x1b]9;4;1;40\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.progressState == Ubiquitty::OSC::ProgressNormal, "a determinate report publishes its kind");
    CHECK_MSG(snapshot.progressValue == 40, "and the percent beside it");

    // The pair travels together: a state read apart from its value shows a
    // determinate fill at whatever an error left behind.
    Ubiquitty::processBytes(t.get(), "\x1b]9;4;2;0\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.progressState == Ubiquitty::OSC::ProgressError, "an error replaces the kind");
    CHECK_MSG(snapshot.progressValue == 0, "and the percent with it");

    Ubiquitty::processBytes(t.get(), "\x1b]9;4;0;0\x07");
    Ubiquitty::Runtime::captureSnapshotFromTerm(t.get(), 0, snapshot);
    CHECK_MSG(snapshot.progressState == Ubiquitty::OSC::ProgressNone, "and a clear retracts it");

    t.reset();
}

TEST(passwordDetectionReadsTheStreamInBothIngestPaths)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Runtime;
    for (const bool usesPipeline : { false, true }) {
        TerminalRuntime runtime;
        runtime.init(parameters80x24());
        auto* stream = attachPipe(runtime);
        REQUIRE(stream);
        auto* terminal = runtime.borrowTermForRender();
        REQUIRE(terminal);
        terminal->passwordDetect = true;
        stream->discipline = PAL::LineDiscipline {};
        CountingDrainHost host;
        const auto input = [&](std::string_view text) {
            if (usesPipeline) {
                CHECK_EQ(runtime.feedChunkUpToSyncClear({ text.data(), text.size() }).consumed, static_cast<int>(text.size()));
                // The pipeline reads the prompt where it closes a pass on a pause.
                CHECK_EQ(runtime.finishExternalDrainPass(host, static_cast<int>(text.size()), DrainPassEnd::WouldBlock).bytesDrained, static_cast<int>(text.size()));
            } else {
                stream->toRead.append(text);
                CHECK_EQ(runtime.drainAvailable(host).bytesDrained, static_cast<int>(text.size()));
            }
        };
        input("Password:");
        CHECK(!terminal->passwordPromptActive);
        CHECK_EQ(stream->disciplineProbes, 1);
        stream->discipline->isEchoEnabled = false;
        input("\r\nPIN> ");
        CHECK(terminal->passwordPromptActive);
        CHECK_EQ(stream->disciplineProbes, 2);
        stream->discipline->isEchoEnabled = true;
        input("\r\nPassword:");
        CHECK(!terminal->passwordPromptActive);
        CHECK_EQ(stream->disciplineProbes, 3);
    }
}

TEST(aReplicaRuntimeKeepsTheHostsPasswordVerdict)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    runtime.borrowTermForRender()->replyAuthority = Ubiquitty::ReplyAuthority::Elsewhere;
    runtime.borrowTermForRender()->passwordDetect = true;
    const std::string_view prompt = "Password:";
    runtime.feedHostBytes({ prompt.data(), prompt.size() });
    CHECK(!runtime.borrowTermForRender()->passwordPromptActive);
    runtime.feedChunkUpToSyncClear({ prompt.data(), prompt.size() });
    CHECK(!runtime.borrowTermForRender()->passwordPromptActive);
    runtime.borrowTermForRender()->passwordPromptActive = true;
    const std::string_view plain = "\r\n$ ";
    runtime.feedChunkUpToSyncClear({ plain.data(), plain.size() });
    CHECK(runtime.borrowTermForRender()->passwordPromptActive);
}

TEST(theFileTransferWatchBelongsToTheRuntimeThatSeesTheBytes)
{
    using namespace Ubiquitty::Runtime;

    // The watch is Core's, so a runtime with no frontend attached still keeps
    // it: a detector owned by one frontend is a feature only that frontend has,
    // and this asserts the arrangement rather than the arithmetic.
    CountingSink sink;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    runtime.setEventSink(&sink);

    // Unarmed by default: a session that never asks for the watch pays no
    // judgement on its bytes.
    CHECK(!runtime.isFileTransferWatchArmed());
    runtime.tapRawBytes(std::span<const char>("**\x18"
                                              "B00000000000000",
        18));
    CHECK(sink.fileTransferDetected == 0);

    runtime.armFileTransferWatch();
    CHECK(runtime.isFileTransferWatchArmed());
    CHECK(!runtime.detectedFileTransfer());

    // Ordinary output leaves it armed and silent.
    runtime.tapRawBytes(std::span<const char>("hello, world\r\n", 14));
    CHECK(sink.fileTransferDetected == 0);
    CHECK(runtime.isFileTransferWatchArmed());

    // A ZMODEM ZHEX preamble fires once, and disarms so a continuing frame
    // stream cannot fire it again under the frontend setting the transfer up.
    runtime.tapRawBytes(std::span<const char>("**\x18"
                                              "B00000000000000",
        18));
    CHECK(sink.fileTransferDetected == 1);
    CHECK(!runtime.isFileTransferWatchArmed());
    const Ubiquitty::Network::Transfer::Detection detection = runtime.detectedFileTransfer();
    CHECK(detection);
    CHECK(detection.protocol == Ubiquitty::Network::Transfer::Protocol::ZModem);
    // The event carries the same answer as a scalar: protocol low, direction above.
    CHECK((sink.lastFileTransferData0 & 0xFFu) == static_cast<std::uint32_t>(detection.protocol));
    CHECK((sink.lastFileTransferData0 >> 8) == static_cast<std::uint32_t>(detection.direction));

    runtime.tapRawBytes(std::span<const char>("**\x18"
                                              "B00000000000000",
        18));
    CHECK(sink.fileTransferDetected == 1); // still one: disarmed means disarmed

    // Arming again forgets the partial match and watches afresh, which is what
    // resuming after a transfer wants.
    runtime.armFileTransferWatch();
    CHECK(runtime.isFileTransferWatchArmed());
    CHECK(!runtime.detectedFileTransfer());
    runtime.tapRawBytes(std::span<const char>("**\x18"
                                              "B00000000000000",
        18));
    CHECK(sink.fileTransferDetected == 2);

    // Bytes arriving through a real read reach the watch too. The sequential
    // drain reads at one site and the flood pipeline at another, so a watch
    // wired to one of them sees a stream that depends on which path carried it —
    // which a test that only ever taps by hand cannot see.
    runtime.armFileTransferWatch();
    auto* readStream = attachPipe(runtime);
    REQUIRE(readStream != nullptr);
    readStream->toRead.append("**\x18"
                              "B00000000000000");
    CountingDrainHost readHost;
    (void)runtime.drainAvailable(readHost);
    CHECK(sink.fileTransferDetected == 3);
    CHECK(!runtime.isFileTransferWatchArmed());

    // The runtime holds a CheckedPtr to the sink, so it stops pointing at one
    // that is about to leave scope.
    runtime.setEventSink(nullptr);
}

TEST(theOutboundTapSeesInputAndReplies)
{
    using namespace Ubiquitty::Runtime;

    // The outbound tap must observe every byte written to the stream:
    // - sendInputBytes (user input)
    // - flushReplies (parser replies like DA1)
    // The tap sees only accepted bytes.
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);

    std::string tapped;
    auto tapFn = [](void* ctx, std::string_view bytes) {
        auto& s = *static_cast<std::string*>(ctx);
        s.append(bytes.data(), bytes.size());
    };

    std::string tappedBytes;
    runtime.setOutboundByteTap(tapFn, &tappedBytes);

    // 1) User input via sendInputBytes
    runtime.sendInputBytes(std::span<const char>("ls\r", 3));
    CHECK(stream->written == "ls\r");
    CHECK(tappedBytes == "ls\r");

    // 2) Parser reply: feed a DA1 query (\x1b[c) -> terminal queues reply \x1b[?1;2c
    // Feed the DA1 sequence through the parser
    stream->toRead.append("\x1b[c");
    CountingDrainHost host;
    (void)runtime.drainAvailable(host);

    // The reply should have been written and tapped
    CHECK(stream->written.size() > 3);
    CHECK(tappedBytes == stream->written); // tap sees exactly what went to the stream

    runtime.setEventSink(nullptr);
}

TEST(refusedBulkRepliesWaitForCapacityWithoutTruncationOrDuplicateTaps)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream);
    auto* terminal = runtime.borrowTermForRender();
    REQUIRE(terminal);
    std::string tapped;
    runtime.setOutboundByteTap([](void* context, std::string_view bytes) {
        static_cast<std::string*>(context)->append(bytes);
    },
        &tapped);
    Ubiquitty::processBytes(terminal, "\033[6n");
    const std::string small(terminal->outputBuffer.data(), terminal->outputLength);
    REQUIRE(!small.empty());
    const std::string bulk = "\033]72;t=r:m=1;" + std::string(10000, 'A') + "\033\\";
    REQUIRE(Ubiquitty::respondBulk(*terminal, Ubiquitty::Capability::InputModeReport, bulk));

    stream->remainingWriteCapacity = small.size();
    runtime.flushReplies();
    CHECK(stream->written == small);
    CHECK(tapped == small);
    CHECK(terminal->bulkOutput.length == static_cast<int>(bulk.size()));
    runtime.flushReplies();
    CHECK(stream->written == small);
    CHECK(terminal->bulkOutput.length == static_cast<int>(bulk.size()));

    stream->remainingWriteCapacity = bulk.size();
    runtime.drainPendingWrite();
    CountingDrainHost host;
    CHECK(runtime.drainAvailable(host).bytesDrained == 0);
    CHECK(stream->written == small + bulk);
    CHECK(tapped == stream->written);
    CHECK(terminal->bulkOutput.length == 0);
    runtime.flushReplies();
    CHECK(tapped == small + bulk);
}

TEST(aRefusedSmallReplyStaysAheadOfBulkOutput)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream);
    auto* terminal = runtime.borrowTermForRender();
    REQUIRE(terminal);
    Ubiquitty::processBytes(terminal, "\033[6n");
    const std::string small(terminal->outputBuffer.data(), terminal->outputLength);
    REQUIRE(!small.empty());
    constexpr std::string_view bulk = "\033]72;t=Q;test\033\\";
    REQUIRE(Ubiquitty::respondBulk(*terminal, Ubiquitty::Capability::InputModeReport, bulk));
    stream->remainingWriteCapacity = 0;
    runtime.flushReplies();
    CHECK(stream->written.empty());
    CHECK(terminal->outputLength == static_cast<int>(small.size()));
    CHECK(terminal->bulkOutput.length == static_cast<int>(bulk.size()));
    stream->remainingWriteCapacity = small.size() + bulk.size();
    runtime.flushReplies();
    CHECK(stream->written == small + std::string(bulk));
}

TEST(aThrottledDrainSkipsTheFullSnapshotItWillNotPublish)
{
    using namespace Ubiquitty::Runtime;

    Ubiquitty::PAL::s_fakeMicroseconds = 1;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);

    Ubiquitty::Terminal* terminal = runtime.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    terminal->modes.appCursorKeys = true;

    CountingDrainHost host;
    const std::uint64_t capturesBefore = runtime.fullSnapshotCaptureCountForTest();
    const std::uint64_t publishesBefore = runtime.link().reads.version();
    const DrainOutcome throttled = runtime.finishExternalDrainPass(host, 1, DrainPassEnd::TimeBudget);
    CHECK_EQ(runtime.fullSnapshotCaptureCountForTest(), capturesBefore);
    CHECK_EQ(runtime.link().reads.version(), publishesBefore);
    CHECK_MSG(throttled.modes.applicationCursorKeys,
        "the drain outcome still receives current input state without a full snapshot");
    CHECK_EQ(throttled.rows, 24);
    CHECK_EQ(throttled.columns, 80);

    // Advancing through the publication floor is the control: the same pass
    // captures and publishes when its result has a consumer.
    Ubiquitty::PAL::s_fakeMicroseconds = syncPublishMinIntervalFloorMicros;
    runtime.finishExternalDrainPass(host, 1, DrainPassEnd::TimeBudget);
    CHECK_EQ(runtime.fullSnapshotCaptureCountForTest(), capturesBefore + 1);
    CHECK_EQ(runtime.link().reads.version(), publishesBefore + 1);
}

TEST(aBurstEndPublishesTheFinalThrottledHistory)
{
    using namespace Ubiquitty::Runtime;
    for (const auto context : { TerminalRuntime::SettleContext::CaughtUp, TerminalRuntime::SettleContext::BurstEnd }) {
        Ubiquitty::PAL::s_fakeMicroseconds = 1;
        TerminalRuntime runtime;
        HandClock clock;
        clock.drive(runtime);
        runtime.init(parameters80x24());
        REQUIRE(attachPipe(runtime) != nullptr);
        auto* terminal = runtime.borrowTermForRender();
        REQUIRE(terminal != nullptr);
        const int publishedHistory = runtime.link().reads.read().scrollbackCount;
        for (int row = 0; row < 64; ++row) {
            Ubiquitty::processBytes(terminal, "benchmark output\r\n");
        }
        CountingDrainHost host;
        runtime.finishExternalDrainPass(host, 1, DrainPassEnd::TimeBudget);
        REQUIRE(terminal->scrollbackCount > publishedHistory);
        CHECK_EQ(runtime.link().reads.read().scrollbackCount, publishedHistory);
        const auto captures = runtime.fullSnapshotCaptureCountForTest();
        if (context == TerminalRuntime::SettleContext::BurstEnd) {
            // The frame is announced once the read has gone quiet; the history
            // is republished whether or not it is.
            clock.nowMicroseconds += readQuiescenceMicroseconds;
            host.lockModel();
            runtime.publishSettledFrameIfCoherent(host, context);
            host.unlockModel();
            CHECK_EQ(host.frames, 1);
        } else {
            const auto settled = runtime.drainAvailable(host);
            CHECK(settled.caughtUp);
            CHECK_EQ(settled.bytesDrained, 0);
        }
        CHECK_EQ(runtime.link().reads.read().scrollbackCount, terminal->scrollbackCount);
        CHECK_EQ(runtime.fullSnapshotCaptureCountForTest(), captures + 1);
        runtime.drainAvailable(host);
        CHECK_EQ(runtime.fullSnapshotCaptureCountForTest(), captures + 1);
    }
}

TEST(aFontRequestPublishesItsGenerationBeforeAThrottledDrainEvent)
{
    using namespace Ubiquitty::Runtime;

    Ubiquitty::PAL::s_fakeMicroseconds = 1;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    REQUIRE(attachPipe(runtime) != nullptr);
    Ubiquitty::Terminal* terminal = runtime.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    terminal->allowFontOperations = true;
    const std::uint64_t publishesBefore = runtime.link().reads.version();
    runtime.feedHostBytes(std::string_view("\x1b]50;Menlo:size=17\x07"));

    CountingDrainHost host;
    const DrainOutcome outcome = runtime.finishExternalDrainPass(host, 1, DrainPassEnd::TimeBudget);
    REQUIRE(!outcome.caughtUp && !outcome.reachedEnd);
    REQUIRE(outcome.fontRequestChanged);
    CHECK_EQ(runtime.link().reads.version(), publishesBefore + 1);
    CHECK_EQ(runtime.link().reads.read().fontRequestGeneration, terminal->fontRequests.generation());
    CHECK_EQ(terminal->fontRequests.generation(), 1u);
}

TEST(runtimeCases)
{

    using namespace Ubiquitty::Runtime;
    namespace PAL = Ubiquitty::PAL;

    // 1) init + baseline dimensions; second init is an idempotent no-op.
    {
        CountingSink sink;
        TerminalRuntime rt;
        rt.setEventSink(&sink);
        rt.init(parameters80x24());
        DimensionSnapshot d = rt.dimensions();
        CHECK(d.rows == 24);
        CHECK(d.columns == 80);
        CHECK(d.scrollbackCount == 0);
        CHECK(d.generation == 0);

        RuntimeInitParameters smaller;
        smaller.rows = 10;
        smaller.columns = 40;
        rt.init(smaller); // guarded: m_term already exists
        d = rt.dimensions();
        CHECK(d.rows == 24);
        CHECK(d.columns == 80);
    }

    // 2) a feed mutates the grid and fires exactly one ScreenDirty.
    {
        CountingSink sink;
        TerminalRuntime rt;
        rt.setEventSink(&sink);
        rt.init(parameters80x24());
        const char hello[] = "hello";
        FeedResult r = rt.feedChunkUpToSyncClear({ hello, literalLength(hello) });
        CHECK(r.consumed == 5);
        CHECK(!r.reachedSyncClear);
        CHECK(sink.screenDirty == 1);
        Ubiquitty::Terminal* term = rt.borrowTermForRender();
        CHECK(term != nullptr);
        CHECK(term->screens.active->cursorX == 5); // five glyphs advanced the cursor
    }

    // 3) no DECSET-2026 boundary -> the whole chunk is consumed.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        const char plain[] = "abc\r\ndef";
        FeedResult r = rt.feedChunkUpToSyncClear({ plain, literalLength(plain) });
        CHECK(r.consumed == static_cast<int>(literalLength(plain)));
        CHECK(!r.reachedSyncClear);
    }

    // 4) a DECSET-2026 clear stops the pump at (and including) the ESU; the
    //    remainder replays cleanly on the next call.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        const char seq[] = "\x1b[?2026hAB\x1b[?2026lCD"; // BSU .. ESU then "CD"
        const int total = literalLength(seq);
        FeedResult r = rt.feedChunkUpToSyncClear({ seq, static_cast<std::size_t>(total) });
        CHECK(r.reachedSyncClear);
        CHECK(r.consumed < total);
        CHECK(r.consumed == total - 2); // "CD" left for the next call
        FeedResult r2 = rt.feedChunkUpToSyncClear({ seq + r.consumed, static_cast<std::size_t>(total - r.consumed) });
        CHECK(r2.consumed == total - r.consumed);
        CHECK(!r2.reachedSyncClear);
    }

    // 4b) an ESU that is the last byte of the span still reports the frame:
    //     a TUI writes one frame per write(), so a bite often ends on it.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        const char seq[] = "\x1b[?2026hAB\x1b[?2026l";
        FeedResult r = rt.feedChunkUpToSyncClear({ seq, literalLength(seq) });
        CHECK(r.consumed == static_cast<int>(literalLength(seq)));
        CHECK_MSG(r.reachedSyncClear, "an ESU ending the span completes a frame");
        CHECK(rt.syncClearSeenCount() == 1u);
    }

    // 4c) the same with the BSU in an earlier span, so the ESU is met on the
    //     CSI fast lane rather than in the byte-at-a-time parser.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        const char open[] = "\x1b[?2026h";
        const char close[] = "AB\x1b[?2026l";
        (void)rt.feedChunkUpToSyncClear({ open, literalLength(open) });
        FeedResult r = rt.feedChunkUpToSyncClear({ close, literalLength(close) });
        CHECK(r.reachedSyncClear);
        CHECK(rt.syncClearSeenCount() == 1u);
    }

    // 4d) the pass that carried that ESU settles as a Boundary frame, as the
    //     sequential drain labels the same bytes, not as a saturated pass.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        REQUIRE(attachPipe(rt) != nullptr);
        CountingDrainHost host;
        const char seq[] = "\x1b[?2026hAB\x1b[?2026l";
        (void)rt.feedChunkUpToSyncClear({ seq, literalLength(seq) });
        rt.finishExternalDrainPass(host, TerminalRuntime::drainPassCapacityBytes, DrainPassEnd::ByteBudget);
        REQUIRE(!host.reasons.empty());
        CHECK(host.reasons.back() == SyncFrameReason::Boundary);
    }

    // 5) host-output reply path: feedChunk does not auto-flush; flushReplies
    //    drains the queued CPR to the stream.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        const char dsr[] = "\x1b[6n"; // DSR -> Cursor Position Report
        rt.feedChunkUpToSyncClear({ dsr, literalLength(dsr) });
        CHECK(s->written.empty()); // not flushed yet
        rt.flushReplies();
        CHECK(!s->written.empty()); // CPR now on the wire
        CHECK(static_cast<unsigned char>(s->written.front()) == 0x1b);
        CHECK(s->written.back() == 'R'); // CPR terminates with 'R'
        rt.close();
    }

    // 6) the local-echo path (feedHostBytes) auto-flushes replies.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        const char dsr5[] = "\x1b[5n"; // DSR device-status -> "\x1b[0n"
        rt.feedHostBytes({ dsr5, literalLength(dsr5) });
        CHECK(!s->written.empty()); // no manual flush needed
        CHECK(static_cast<unsigned char>(s->written.front()) == 0x1b);
        CHECK(s->written.back() == 'n');
        rt.close();
    }

    // 7) resizeGrid updates dimensions and bumps the generation each time.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        CHECK(rt.dimensions().generation == 0);
        rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
        DimensionSnapshot d = rt.dimensions();
        CHECK(d.rows == 30);
        CHECK(d.columns == 100);
        CHECK(d.generation == 1);
        rt.resizeGrid(24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
        CHECK(rt.dimensions().generation == 2);
    }

    // 7b) A resize flushes the DECSET 2048 in-band report it queued, without
    // waiting for an input-driven drain: a mode-2048 program ignores SIGWINCH
    // and would otherwise learn the size at the next keypress.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(rt);
        REQUIRE(pipe != nullptr);
        rt.feedHostBytes(std::span<const char>("\x1b[?2048h", literalLength("\x1b[?2048h")));
        pipe->written.clear();
        rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
        CHECK_MSG(pipe->written.find("\x1b[48;30;100;") != std::string::npos, "the in-band resize report reaches the child on the resize itself");
        pipe->written.clear();
        rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
        CHECK_MSG(pipe->written.empty(), "a dimension no-op queues no report, so nothing is flushed");
        rt.close();
    }

    // 8) setWindowSize forwards columns/rows/xpixel/ypixel to the stream.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        rt.setWindowSize(100, 30, 800, 600);
        // Adoption stamped the coalescing interval, so this size is held;
        // the deferred apply is what hands it to the stream.
        rt.applyPendingWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros() + 100000);
        CHECK(s->windowColumns == 100);
        CHECK(s->windowRows == 30);
        CHECK(s->windowXpixel == 800);
        CHECK(s->windowYpixel == 600);
        rt.close();
    }

    // 9) hasPendingWrite / drainPendingWrite forward to the stream.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        CHECK(!rt.hasPendingWrite());
        s->pendingWrite = true;
        CHECK(rt.hasPendingWrite());
        CHECK(!rt.drainPendingWrite()); // double drains fully -> nothing remains
        CHECK(!rt.hasPendingWrite());
        rt.close();
    }

    // 10) sendInputBytes writes straight to the stream (no model involvement).
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        const char in[] = "ls -la\n";
        rt.sendInputBytes({ in, literalLength(in) });
        CHECK(s->written == "ls -la\n");
        rt.close();
    }

    // 11) ownership is RAII: the runtime OWNS its stream, so both close() and the
    //     destructor destroy it exactly once (the non-owning attach path is gone).
    {
        const int destroyedBefore = PAL::g_streamDestroyCount;
        {
            TerminalRuntime rt;
            rt.init(parameters80x24());
            REQUIRE(attachPipe(rt) != nullptr);
            rt.close();
            CHECK(PAL::g_streamDestroyCount == destroyedBefore + 1); // close() destroyed it
        }
        CHECK(PAL::g_streamDestroyCount == destroyedBefore + 1); // dtor after close: no double-free

        {
            TerminalRuntime rt; // destroyed without an explicit close()
            rt.init(parameters80x24());
            REQUIRE(attachPipe(rt) != nullptr);
        }
        CHECK(PAL::g_streamDestroyCount == destroyedBefore + 2); // ~TerminalRuntime closed it
    }

    // 12) guards: feeding before init or after close is a safe no-op that
    //     still reports the chunk as consumed (so a caller loop terminates).
    {
        TerminalRuntime rt; // never initialised
        const char x[] = "x";
        FeedResult r = rt.feedChunkUpToSyncClear({ x, literalLength(x) });
        CHECK(r.consumed == 1);
        CHECK(!r.reachedSyncClear);
        CHECK(rt.borrowTermForRender() == nullptr);

        rt.init(parameters80x24());
        rt.close();
        FeedResult r2 = rt.feedChunkUpToSyncClear({ x, literalLength(x) }); // after close
        CHECK(r2.consumed == 1);
        CHECK(rt.borrowTermForRender() == nullptr);
    }

    // 13) close() detaches the sink: no events fire afterwards.
    {
        CountingSink sink;
        TerminalRuntime rt;
        rt.setEventSink(&sink);
        rt.init(parameters80x24());
        const char a[] = "a";
        rt.feedChunkUpToSyncClear({ a, literalLength(a) });
        CHECK(sink.screenDirty == 1);
        rt.close(); // clears m_sink (checked-ptr release)
        rt.init(parameters80x24()); // re-init, now sink-less
        rt.feedChunkUpToSyncClear({ a, literalLength(a) });
        CHECK(sink.screenDirty == 1); // still 1 — detached
    }

    // 14) the runtime must construct/destruct the Terminal (not calloc/free): ~Ubiquitty::Terminal has to
    //     run so a wired client's CheckedPtr referent count returns to zero.
    //     CanMakeCheckedPtr asserts on destruction while a CheckedPtr still
    //     references it — so if close() ever skips ~Terminal, `client` aborts at
    //     scope exit, as a window controller's dealloc does.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        Ubiquitty::Terminal* term = rt.borrowTermForRender();
        CHECK(term != nullptr);
        if (term) {
            Ubiquitty::WindowClient client; // CanMakeCheckedPtr-derived, concrete
            term->windowClient = &client; // CheckedPtr ref -> client count 1
            rt.close(); // ~Terminal -> ~CheckedPtr -> client count 0
            CHECK(rt.borrowTermForRender() == nullptr);
        } // ~client asserts count==0 here
    }

    // 15) drainAvailable: the portable ingest drain reads injected bytes, pumps
    //     them through Core, flushes the reply, and reports the outcome via the
    //     platform DrainHost (lock/unlock balanced). This is the reusable ingest
    //     logic the macOS/Linux/host frontends share — exercised here with no AppKit/Metal.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            s->toRead = "hi\x1b[6n"; // text + DSR cursor-position query
            CountingDrainHost host;
            DrainOutcome out = rt.drainAvailable(host);
            CHECK(out.bytesDrained == 6);
            CHECK(!out.reachedEnd);
            CHECK(out.columns == 80 && out.rows == 24);
            CHECK(host.locks == host.unlocks && host.locks >= 1); // balanced
            // Core's CPR reply was flushed to the stream by drainAvailable.
            CHECK(!s->written.empty());
            CHECK(static_cast<unsigned char>(s->written.front()) == 0x1b);
            CHECK(s->written.back() == 'R');
            // Nothing queued now → a clean Again no-op.
            DrainOutcome again = rt.drainAvailable(host);
            CHECK(again.bytesDrained == 0 && !again.reachedEnd);
            // Peer close → reachedEnd.
            s->eofPending = true;
            CHECK(rt.drainAvailable(host).reachedEnd);
        }
        rt.close();
    }

    // 16) drainAvailable surfaces the Terminal's mouse-tracking modes so the platform
    //     caches them and answers its hot per-event pointer/report queries without
    //     re-locking the model. The DECSET escapes are parsed in the same drain
    //     (under the lock), then read into the all-scalar DrainOutcome.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            CountingDrainHost host;
            s->toRead = "\x1b[?1000h"; // DECSET 1000: normal (no motion)
            DrainOutcome a = rt.drainAvailable(host);
            CHECK(a.mouseReportsActive);
            CHECK(!a.mouseMotionReportsActive);
            CHECK(!a.mouseAnyEventActive);
            CHECK(!a.mouseSGRPixelsActive);
            s->toRead += "\x1b[?1003h\x1b[?1016h"; // + any-event + SGR-pixel coords
            DrainOutcome b = rt.drainAvailable(host);
            CHECK(b.mouseReportsActive);
            CHECK(b.mouseMotionReportsActive); // any-event implies motion
            CHECK(b.mouseAnyEventActive);
            CHECK(b.mouseSGRPixelsActive);
            s->toRead += "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1016l"; // disable every mode
            DrainOutcome c = rt.drainAvailable(host);
            CHECK(!c.mouseReportsActive);
            CHECK(!c.mouseMotionReportsActive);
            CHECK(!c.mouseAnyEventActive);
            CHECK(!c.mouseSGRPixelsActive);
        }
        rt.close();
    }

    // 17) Stale-mouse guard: a foreground job (ssh) enables mouse tracking, dies
    //     without the DECRSTs (connection reset), and the shell prints a fresh
    //     prompt — the next drain notices the foreground group moved back to the
    //     shell and clears the tracking modes itself, so pointer moves stop
    //     typing junk at the prompt. Tracking the SHELL'S OWN group enabled, or a
    //     still-running job's, is left alone.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            CountingDrainHost host;
            // ssh (group 222) owns the tty when the remote side enables SGR
            // any-event tracking.
            s->foregroundShell = false;
            s->foregroundGroup = 222;
            s->toRead = "\x1b[?1003h\x1b[?1006h";
            CHECK(rt.drainAvailable(host).mouseReportsActive);
            // More output from the same foreground job: tracking stays on.
            s->toRead += "still alive";
            CHECK(rt.drainAvailable(host).mouseReportsActive);
            // The connection drops; the shell (group 111) is back at a prompt.
            s->foregroundShell = true;
            s->foregroundGroup = 111;
            s->toRead += "Connection reset by peer\r\n% ";
            DrainOutcome after = rt.drainAvailable(host);
            CHECK(!after.mouseReportsActive);
            CHECK(!after.mouseMotionReportsActive);
            // The encoding selection (SGR) survives — a re-enabling app finds it
            // still in effect, matching a plain DECRST of the tracking modes.
            // Re-enable from the prompt's own group: the guard records the shell
            // as the enabler and must not clear while it stays in the foreground.
            s->toRead += "\x1b[?1000h";
            CHECK(rt.drainAvailable(host).mouseReportsActive);
            s->toRead += "prompt redraw";
            CHECK(rt.drainAvailable(host).mouseReportsActive);
        }
        rt.close();
    }

    // 18) close() kills the child of a LOCAL (non-remote, non-host) session.
    //     A real PTY launch (not the injected pipe) — the child prints its own
    //     processID then execs a long sleep (same processID). close() -> ~PTYStream SIGHUPs +
    //     reaps it, so the processID must be gone afterwards. This is the engine under
    //     the macOS "close tab kills the shell" path (the confirmation is UI).
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        const bool launched = rt.launch(LaunchCommand::fromArguments({ "/bin/sh", "-c", "echo $$; exec sleep 60" }), Ubiquitty::Preferences::terminalIdentity(Ubiquitty::Preferences::values()));
        CHECK(launched);
        if (launched) {
            const int fileDescriptor = rt.streamFileDescriptor();
            CHECK(fileDescriptor >= 0);
            if (fileDescriptor >= 0)
                ::fcntl(fileDescriptor, F_SETFL, ::fcntl(fileDescriptor, F_GETFL, 0) | O_NONBLOCK);
            // Read the child's processID off the PTY master (bounded ~2s).
            // launch() runs a LOGIN shell, so the profile speaks first on any
            // machine whose /etc/profile or ~/.profile echoes anything (an
            // rbenv/nvm banner, a fortune, a motd). The processID is therefore
            // the first line that is all digits, not the first line.
            const auto firstNumericLine = [](const std::string& text) -> long {
                for (std::size_t start = 0; start < text.size();) {
                    const std::size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                        break; // a partial trailing line: wait for its newline
                    std::string line = text.substr(start, end - start);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (!line.empty() && line.find_first_not_of("0123456789") == std::string::npos)
                        return ::strtol(line.c_str(), nullptr, 10);
                    start = end + 1;
                }
                return 0;
            };
            std::string accumulated;
            long child = 0;
            for (int i = 0; i < 400 && child <= 0; ++i) {
                char buffer[128];
                const ssize_t n = ::read(fileDescriptor, buffer, sizeof(buffer));
                if (n > 0)
                    accumulated.append(buffer, static_cast<std::size_t>(n));
                else
                    ::usleep(5000);
                child = firstNumericLine(accumulated);
            }
            CHECK(child > 0);
            CHECK(::kill(static_cast<pid_t>(child), 0) == 0); // alive before close

            rt.close();

            bool gone = false;
            for (int i = 0; i < 400 && !gone; ++i) {
                if (::kill(static_cast<pid_t>(child), 0) != 0 && errno == ESRCH)
                    gone = true;
                else
                    ::usleep(5000);
            }
            CHECK(gone); // close() reaped/killed the local child
        }
    }

    // 19) Non-2026 drain-settle frame publish (the quick-refresh flicker guard):
    //     a pass that settles (EAGAIN) with NEW bytes publishes a complete frame
    //     for the render to promote once the read has gone quiet — but only when
    //     the parser is at ground. A settle that lands mid-escape-sequence (a
    //     redraw torn by a mid-burst EAGAIN: the clear arrived, the redraw's CSIs
    //     did not) must not publish a half-drawn grid; the next settle, once the
    //     rest arrives, publishes once.
    {
        TerminalRuntime rt;
        HandClock clock;
        clock.drive(rt);
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            CountingDrainHost host;
            // a) a coherent plain redraw that ends at ground → one publish.
            s->toRead = "\x1b[2J\x1b[Hhello world";
            DrainOutcome a = rt.drainAvailable(host);
            CHECK(a.caughtUp); // settled on EAGAIN, not a capacity yield
            settleHeldFrame(rt, host, clock);
            CHECK(host.frames == 1); // complete frame published for the render

            // b) the next chunk arrives split mid-CSI: the parser is left in the
            //    middle of "\x1b[7" (no final byte) → settle must not publish.
            const int framesBefore = host.frames;
            s->toRead += "\x1b[2J\x1b[7"; // clear + an unfinished CSI (mid-sequence)
            DrainOutcome b = rt.drainAvailable(host);
            CHECK(b.caughtUp);
            settleHeldFrame(rt, host, clock);
            CHECK(host.frames == framesBefore); // torn frame held back, not shown

            // c) the rest of the sequence arrives, returning the parser to ground
            //    → exactly one publish for the now-complete frame.
            s->toRead += "m done"; // completes "\x1b[7m" then prints text
            DrainOutcome c = rt.drainAvailable(host);
            CHECK(c.caughtUp);
            settleHeldFrame(rt, host, clock);
            CHECK(host.frames == framesBefore + 1);

            // d) a settle with no new bytes (idle wakeup) publishes nothing — the
            //    publish is gated on bytesDrained > 0, so an empty pass is silent.
            const int framesIdle = host.frames;
            DrainOutcome d = rt.drainAvailable(host);
            CHECK(d.bytesDrained == 0);
            CHECK(host.frames == framesIdle);
        }
        rt.close();
    }

    // 20) A 2026 (synchronized-output) app never publishes via the non-2026
    //     drain-settle path — it publishes at ESU through processBytesUpToSyncClear.
    //     A settle WHILE sync is held (BSU with no ESU) must publish nothing here.
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            CountingDrainHost host;
            s->toRead = "\x1b[?2026h\x1b[2Jpartial"; // BSU, then a mid-frame redraw, no ESU
            DrainOutcome a = rt.drainAvailable(host);
            CHECK(a.caughtUp);
            CHECK(host.frames == 0); // sync held → no non-2026 settle publish
        }
        rt.close();
    }

    // The gate is derived, not set: Core folds the two platform facts so every
    // ingest adapter shares one policy. The macOS bridge and the Swift ingest
    // drive's session each own a runtime — a fold living in either frontend
    // leaves the other runtime's default riding while hidden.
    {
        TerminalRuntime rt;
        CHECK(rt.drainRidesBurst()); // presumes presented, full-rate
        rt.setMachineThrottle(MachineThrottle::Throttled);
        CHECK(!rt.drainRidesBurst()); // throttle collapses even a presented pane
        rt.setMachineThrottle(MachineThrottle::FullRate);
        CHECK(rt.drainRidesBurst());
        rt.setPresentation(PanePresentation::Hidden);
        CHECK(!rt.drainRidesBurst()); // a hidden pane never rides
        rt.setMachineThrottle(MachineThrottle::Throttled);
        rt.setMachineThrottle(MachineThrottle::FullRate);
        CHECK(!rt.drainRidesBurst()); // clearing one fact does not forget the other
        rt.setPresentation(PanePresentation::Presented);
        CHECK(rt.drainRidesBurst());
    }

    // The ingest-drain ride-the-burst gate. A presented pane rides
    // mid-burst EAGAIN gaps (spin+yield retries) to keep throughput up; a
    // background/occluded/throttled pane (presentation Hidden) collapses
    // the budget to 0 and ends the pass on the FIRST gap, making far fewer
    // read() retries. Verify-by-revert: if drainAvailable ignored the flag, the
    // not-presented pass would ride too and readsNotPresented would blow past 2.
    {
        CountingSink sink;
        TerminalRuntime rt;
        rt.setEventSink(&sink);
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        if (s) {
            CountingDrainHost host;

            // Not presented: gate off -> one data read + one EAGAIN, no ride.
            rt.setPresentation(PanePresentation::Hidden);
            s->toRead = "hi";
            s->readPosition = 0;
            s->readCallCount = 0;
            rt.drainAvailable(host);
            const int readsNotPresented = s->readCallCount;

            // Presented (default): rides the burst -> many EAGAIN retry reads.
            rt.setPresentation(PanePresentation::Presented);
            s->toRead = "hi";
            s->readPosition = 0;
            s->readCallCount = 0;
            rt.drainAvailable(host);
            const int readsPresented = s->readCallCount;

            CHECK(readsNotPresented >= 1); // still drained the byte(s)
            CHECK(readsNotPresented <= 2); // data read + one EAGAIN, no ride
            CHECK(readsPresented > readsNotPresented + 5); // gate actually rode the burst
        }
        rt.close();
    }
}

// The confirming zero-byte pass after a burst publishes nothing: the pass that
// parsed the bytes already published, the byteless outcome is discarded by its
// caller, and a republish only churns every lock-free reader. A posted
// command's effects are the one thing a byteless pass must still publish —
// nothing else can change the model without bytes.
TEST(aConfirmingPassRepublishesNothing)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CountingDrainHost host;

    s->toRead = "hello";
    const DrainOutcome burst = rt.drainAvailable(host);
    CHECK(burst.bytesDrained == 5);
    const std::uint64_t versionAfterBurst = rt.link().reads.version();
    CHECK(versionAfterBurst >= 1);

    const DrainOutcome confirm = rt.drainAvailable(host);
    CHECK(confirm.bytesDrained == 0 && !confirm.reachedEnd);
    CHECK_MSG(rt.link().reads.version() == versionAfterBurst, "the confirming pass published nothing");

    // The control, in both directions: a posted command's effects reach the
    // published snapshot through a byteless pass, so the skip above cannot be
    // a pass that publishes nothing ever.
    Command focus;
    focus.kind = CommandKind::SetFocus;
    focus.flag = false;
    CHECK(rt.link().commands.post(focus));
    CHECK(rt.applyPendingCommands(host));
    const DrainOutcome afterCommand = rt.drainAvailable(host);
    CHECK(afterCommand.bytesDrained == 0);
    CHECK_MSG(rt.link().reads.version() > versionAfterBurst, "a command's effects publish through a byteless pass");

    rt.close();
}

namespace {
// Swallows the -CC filter's VT-bound bytes; the routing test asserts on the
// primary's stream, not the parse side.
void discardFilteredByte(void*, uint8_t) { }
} // namespace

/// A session with no host has nowhere to send a payload it cannot spell as
/// input, and must say so rather than answer as though a push were crossing.
///
/// The local answers are what the frontend branches on: a zero id means
/// nothing was sent, and a negative fraction means no bar may be drawn. A
/// session answering 0.0 instead would show a bar at the start of a transfer
/// that never began.
TEST(sessionWithoutAHostSendsNoPasteAndClaimsNoProgress)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    CHECK(!session->isRemoteAttached());

    const std::uint8_t png[] = { 0x89, 'P', 'N', 'G' };
    CHECK(session->pushPaste("image/png", Ubiquitty::Runtime::PayloadSpan(png)) == 0);
    CHECK(session->pastePushProgress() < 0);
    CHECK(session->lastPastePushPath().empty());

    // An empty payload is refused by the same answer, so a caller cannot tell
    // "nothing to send" from "nowhere to send it" — both mean no push.
    CHECK(session->pushPaste("image/png", Ubiquitty::Runtime::PayloadSpan()) == 0);

    releaseTerminalSession(session);
}

TEST(aPublishedRichTextCatalogRequestsOneReplacementFrame)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    CHECK(session->publishTypography(facts));
    CHECK(!session->fontCatalogBecameReady(1, 2));
    CHECK(terminal->richTextBlocks.tryReserve(Ubiquitty::ScreenMode::Primary, 1).has_value());
    Ubiquitty::RichText::Block block;
    block.serial = 1;
    block.width = 1;
    block.height = 1;
    terminal->richTextBlocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    const auto before = terminal->typography.epoch;
    CHECK(session->fontCatalogBecameReady(2, 3));
    CHECK_EQ(terminal->typography.epoch, before + 1);
    CHECK(!session->fontCatalogBecameReady(2, 3));
    CHECK_EQ(terminal->typography.epoch, before + 1);
    releaseTerminalSession(session);
}

/// An embedder's feed has no drain pass behind it, so the feed is where the
/// pause that follows its write is read: a prompt the bytes printed is flagged
/// when the call returns.
TEST(aStreamlessFeedSettlesThePasswordPrompt)
{
    const std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(
        TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->lockForRead();
    session->terminal()->passwordDetect = true;
    session->unlockForRead();
    const auto isPromptActive = [&] {
        session->lockForRead();
        const bool isActive = session->terminal()->passwordPromptActive;
        session->unlockForRead();
        return isActive;
    };

    constexpr std::string_view prompt = "$ sudo make install\r\nPassword: ";
    session->feedBytes({ prompt.data(), prompt.size() });
    CHECK_MSG(isPromptActive(), "the prompt the feed printed is detected when it returns");

    constexpr std::string_view answered = "\r\n$ ";
    session->feedBytes({ answered.data(), answered.size() });
    CHECK_MSG(!isPromptActive(), "the next feed's pause clears it");

    constexpr std::string_view framed = "\x1b[?2026h\x1b[?2026l\r\nPassword: ";
    session->feedBytes({ framed.data(), framed.size() });
    CHECK_MSG(isPromptActive(), "a feed split at a synchronized-update clear is read after its last chunk");
}

/// The streamed session reads the line discipline of its stream's process, and
/// reads it once per feed however many chunks the feed splits into.
TEST(aStreamedFeedSettlesThePasswordPromptOncePerFeed)
{
    const std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(
        TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto pipe = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* stream = pipe.get();
    stream->discipline = Ubiquitty::PAL::LineDiscipline {};
    REQUIRE(session->adoptStream(std::move(pipe)).has_value());
    session->lockForRead();
    session->terminal()->passwordDetect = true;
    session->unlockForRead();
    const auto isPromptActive = [&] {
        session->lockForRead();
        const bool isActive = session->terminal()->passwordPromptActive;
        session->unlockForRead();
        return isActive;
    };
    const auto feed = [&](std::string_view bytes) {
        const int before = stream->disciplineProbes;
        session->feedBytes({ bytes.data(), bytes.size() });
        return stream->disciplineProbes - before;
    };

    CHECK_EQ(feed("\r\nPassword: "), 1);
    CHECK_MSG(!isPromptActive(), "an echoing terminal is not a password prompt");

    stream->discipline->isEchoEnabled = false;
    CHECK_EQ(feed("\r\nPIN> "), 1);
    CHECK_MSG(isPromptActive(), "a canonical terminal with echo off is");

    stream->discipline->isEchoEnabled = true;
    CHECK_EQ(feed("\r\n$ \x1b[?2026h\x1b[?2026l\x1b[?2026h\x1b[?2026l\r\nPassword: "), 1);
    CHECK_MSG(!isPromptActive(), "a feed split into chunks is read once, by the stream's facts");

    std::string queries;
    for (int i = 0; i < 200; ++i)
        queries += "\x1b[6n";
    queries += "\r\nPIN> ";
    stream->discipline->isEchoEnabled = false;
    CHECK_EQ(feed(queries), 1);
    CHECK_MSG(isPromptActive(), "reply pressure inside a feed does not settle per chunk");
}

/// A session fed by its embedder, with no stream of its own, answers the host's
/// queries through the reply sink the embedder installs: an SSH carrier on iOS
/// feeds host bytes in and sends the replies out. Without one, the replies sit
/// in the terminal's small buffer and every later one is dropped.
TEST(aStreamlessSessionAnswersQueriesThroughItsReplySink)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    static std::string replies;
    replies.clear();
    session->setReplySink([](void*, std::string_view bytes) { replies.append(bytes); }, nullptr);

    constexpr std::string_view deviceAttributes = "\x1b[c";
    session->feedBytes({ deviceAttributes.data(), deviceAttributes.size() });
    CHECK_MSG(replies.starts_with("\x1b[?"), "the DA1 reply reached the sink");

    replies.clear();
    std::string queries;
    for (int i = 0; i < 40; ++i)
        queries += "\x1b[6n";
    session->feedBytes({ queries.data(), queries.size() });
    std::size_t reports = 0;
    for (std::size_t at = replies.find('R'); at != std::string::npos; at = replies.find('R', at + 1))
        ++reports;
    CHECK_MSG(reports == 40, "every cursor-position query was answered, none dropped");

    session->close();
    releaseTerminalSession(session);
}

/// Everything a streamless session writes to the host leaves through its sink,
/// not only the replies: the wheel and button reports Core's ladder encodes are
/// input the embedder's carrier owes the host. An SSH carrier on iOS dropped
/// them, so a swipe in tmux with the mouse on scrolled nothing.
TEST(nativeDragRepliesReachTheLocalStreamWithoutMoreProgramOutput)
{
    const std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(
        TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto pipe = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto& written = pipe->written;
    REQUIRE(session->adoptStream(std::move(pipe)).has_value());
    constexpr std::string_view registration = "\033]72;t=a:i=7;text/plain\033\\";
    session->feedBytes({ registration.data(), registration.size() });
    REQUIRE(written.empty());
    Ubiquitty::DragDrop::NativeEvent event;
    event.kind = Ubiquitty::DragDrop::EventKind::Enter;
    event.sessionID = session->nextNativeDragSession();
    event.operation = 1;
    constexpr std::string_view mime = "text/plain";
    REQUIRE(session->sendNativeDragEvent(event, { mime.data(), mime.size() }));
    CHECK(written.starts_with("\033]72;t=m:"));
    CHECK(written.find("i=7") != std::string::npos);
    CHECK(written.ends_with(";text/plain\033\\"));
}

TEST(aStreamlessSessionSendsLadderInputThroughItsSink)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    static std::string written;
    written.clear();
    CHECK_MSG(!session->hasHostInputSink(), "a session with no stream and no sink takes no input");
    session->setReplySink([](void*, std::string_view bytes) { written.append(bytes); }, nullptr);
    CHECK_MSG(session->hasHostInputSink(), "a sink is somewhere for input to go");

    constexpr std::string_view tracking = "\033[?1049h\033[?1000h\033[?1002h\033[?1006h";
    session->feedBytes({ tracking.data(), tracking.size() });
    written.clear();

    TerminalSession::ScrollFact swipe;
    swipe.isPrecise = true;
    swipe.isDirectTouch = true;
    swipe.deltaY = 16;
    swipe.row = 3;
    swipe.column = 5;
    Ubiquitty::Screen::ScrollTuning::Policy tuning;
    tuning.pointsPerLine = 16;
    CHECK(session->scrollFact(swipe, tuning).wasConsumed);
    CHECK_MSG(written == "\033[<64;6;4M", ("a swipe down the glass is a wheel-up report: " + written).c_str());

    written.clear();
    constexpr std::string_view keys = "ls";
    session->sendInput({ keys.data(), keys.size() });
    CHECK_MSG(written == "ls", "typed input takes the same way out, once");

    session->close();
    releaseTerminalSession(session);
}

TEST(aLocalPaneLinkPreservesEncodingAndAllowsSinkReentry)
{
    auto* session = TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    constexpr std::string_view modes = "\033[?1000h\033[?1006h\033[?2004h";
    session->feedBytes({ modes.data(), modes.size() });
    struct Capture {
        TerminalSession* session;
        std::string bytes;
        bool shouldReenter = true;
        bool didReadModel = false;
    } capture { .session = session, .bytes = {} };
    session->setReplySink([](void* context, std::string_view bytes) {
        auto& capture = *static_cast<Capture*>(context);
        capture.bytes.append(bytes);
        if (!capture.shouldReenter)
            return;
        capture.shouldReenter = false;
        std::array<char, 64> encoded {};
        // Takes the model lock again: delivery must be outside that lock.
        capture.didReadModel = capture.session->encodeKey({}, encoded).length == 0;
        capture.session->sendInput(std::string_view("!"));
    },
        &capture);

    Ubiquitty::Runtime::KeyPress key;
    key.unshiftedCodePoint = 'a';
    key.shiftedCodePoint = 'a';
    key.text[0] = 'a';
    key.textLength = 1;
    std::array<char, 64> encoded {};
    const auto accepted = session->sendKey(key, encoded);
    REQUIRE(accepted.length == 1);
    CHECK(accepted.wasAccepted);
    CHECK(capture.didReadModel);
    CHECK_STR_EQ(capture.bytes, "a!");
    key.eventType = Ubiquitty::Keys::KeyEventType::Release;
    const auto ignored = session->sendKey(key, encoded);
    CHECK_EQ(ignored.length, 0);
    CHECK(!ignored.wasAccepted);
    CHECK_STR_EQ(capture.bytes, "a!");

    CHECK_STR_EQ(session->sendPaste("b\nc"), "\033[200~b\rc\033[201~");
    session->sendInput(std::string_view("raw"));
    TerminalSession::PointerFact click;
    click.kind = TerminalSession::PointerFact::Kind::Press;
    click.button = Ubiquitty::Input::PointerButton::Left;
    click.row = 2;
    click.column = 3;
    CHECK(session->pointerFact(click, nullptr).wasConsumed);
    CHECK_STR_EQ(capture.bytes, "a!\033[200~b\rc\033[201~raw\033[<0;4;3M");

    constexpr std::string_view readline = "\033[?2005;2006h";
    session->feedBytes({ readline.data(), readline.size() });
    CHECK_STR_EQ(session->sendPaste("A\n"), "\033[200~\x16"
                                            "A\x16\n\033[201~");

    releaseTerminalSession(session);
}

TEST(pointerLockBreakoutClearsThePublishedModeAndNotifiesItsHostOnce)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* observed = stream.get();
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());
    constexpr std::string_view lock = "\x1b[?7403h";
    session->feedBytes({ lock.data(), lock.size() });
    REQUIRE(session->terminal()->modes.mouseRelativePointerLock);
    session->breakOutOfPointerLock();
    CHECK(!session->terminal()->modes.mouseRelativePointerLock);
    CHECK(!session->readSnapshot().mouseRelativePointerLockActive);
    CHECK(observed->written == "\x1b[?7403;2$y");
    session->breakOutOfPointerLock();
    CHECK(observed->written == "\x1b[?7403;2$y");
    session->close();
    releaseTerminalSession(session);
}

/// The settled event carries its status as the scalar the flag surface allows,
/// and the path beside it — a path is not a scalar, and the frontend needs
/// both to name the file it can now open.
TEST(sessionPublishesAPastePushStatusOnTheFlagSurface)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    // Nothing settled yet: the status is meaningful only when the bit is set,
    // so the resting value must not read as a landed push.
    CHECK(session->lastPastePushStatus() == 0);
    CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::PastePushSettled))) == 0);

    releaseTerminalSession(session);
}

/// A remote connection's state reaches the frontend as an event, and the
/// reason for a failure reaches it beside one.
///
/// The state machine runs in Core, and a frontend's own transport delegate is
/// not a channel the portable session path has, so without the event it would
/// show a frozen screen with nothing saying the host had gone. Both halves are
/// asserted because they are
/// different mechanisms — the event is a scalar on the flag surface, the
/// reason is a string read beside it.
TEST(sessionPublishesRemoteConnectionStateAndTheReasonAFailureCarries)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    // A session that never attached is Idle and carries no failure, so a
    // frontend reading these before any connection does not draw a state.
    CHECK(session->remoteState() == Ubiquitty::Session::State::Idle);
    CHECK(!session->shouldShowCommandAccessory());
    CHECK(session->lastRemoteFailure().empty());
    CHECK(session->remoteSessionID() == 0);
    constexpr std::uint32_t stateBit = 1u << static_cast<unsigned>(EventKind::RemoteStateChanged);
    CHECK((session->takeEventFlags() & stateBit) == 0);

    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Failed, "host closed the connection");
    CHECK((session->takeEventFlags() & stateBit) != 0);
    CHECK(session->lastRemoteFailure() == "host closed the connection");

    // A state that is not Failed clears the reason. Without this a reconnect
    // that succeeded would leave the last failure's words on a live session,
    // and a frontend showing them beside a working screen is worse than one
    // showing nothing.
    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Connected, "");
    CHECK(session->lastRemoteFailure().empty());
    CHECK((session->takeEventFlags() & stateBit) != 0);

    // The detail is kept only for Failed, so a caller that passes one with a
    // healthy state does not get it echoed back as a failure.
    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Reconnecting, "backing off");
    CHECK(session->lastRemoteFailure().empty());

    // Why the connection is reconnecting is read separately, so a log can say
    // whether the handshake timed out, the transport was missing or the path
    // moved, rather than only that it is reconnecting again.
    CHECK(session->lastRemoteTransitionDetail() == "backing off");
    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Connected, "");
    CHECK(session->lastRemoteTransitionDetail().empty());

    releaseTerminalSession(session);
}

TEST(sessionTitleGridUsesTheLaidOutGridUntilTheRemoteScreenArrives)
{
    using Ubiquitty::Runtime::TitleGrid;
    using Ubiquitty::Runtime::titleGridForScreen;

    constexpr TitleGrid laidOut { 42, 160 };
    constexpr TitleGrid model { 24, 80 };
    CHECK(titleGridForScreen(true, false, laidOut, model) == laidOut);
    CHECK(titleGridForScreen(true, true, laidOut, model) == model);
    CHECK(titleGridForScreen(false, false, laidOut, model) == model);
    CHECK(titleGridForScreen(true, false, {}, model) == model);

    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(model.rows, model.columns);
    CHECK(session->titleGrid(laidOut) == model);
    releaseTerminalSession(session);
}

namespace {

/// A host's file answer as a test states it.
struct RemoteFile {
    std::uint32_t requestID = 0;
    std::string path;
    std::vector<std::uint8_t> data;
};

/// Delivers a successful file answer to a session.
void deliver(Ubiquitty::Runtime::TerminalSession& session, const RemoteFile& file)
{
    session.noteRemoteFileForTest(file.requestID, Ubiquitty::Protocol::Control::FileReadStatus::OK, file.path, file.data);
}

} // namespace

/// A host's file answer is taken only for a file this session asked for.
///
/// A host is the other end of a connection and can put any frame on it, and
/// the frontend above writes an answer to this disk and hands it to whatever
/// opens that kind of file. An id nobody asked for is therefore a host
/// choosing what lands in a temporary directory here and what gets opened, so
/// the request ledger is the gate — the same kind of per-request completion
/// table every frontend keeps.
TEST(sessionTakesAHostsFileOnlyForARequestItMade)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    constexpr std::uint32_t arrivedBit = 1u << static_cast<unsigned>(EventKind::RemoteFileArrived);

    const RemoteFile unasked { 9, "/etc/shadow", { 'x' } };
    deliver(*session, unasked);
    CHECK(!session->hasRemoteFile());
    CHECK((session->takeEventFlags() & arrivedBit) == 0);

    // The answer to a request that was made is taken whole, and the event says
    // so. The path the ask named is kept beside it: a landing is named from
    // that and never from the path the host echoed, because the name carries
    // the extension deciding which handler opens the bytes.
    session->noteAskedRemoteFileForTest(3, "/home/me/deploy.sh");
    const RemoteFile asked { 3, "/srv/notes.txt", { 'h', 'i' } };
    deliver(*session, asked);
    CHECK(session->hasRemoteFile());
    CHECK((session->takeEventFlags() & arrivedBit) != 0);
    CHECK(session->remoteFileRequestID() == 3);
    CHECK(session->remoteFilePath() == "/srv/notes.txt");
    CHECK(session->remoteFileRequestedPath() == "/home/me/deploy.sh");
    CHECK(session->remoteFileByteCount() == 2);

    // One ask, one answer: a host repeating the id it was already answered on
    // would otherwise land the same click's file as many times as it liked.
    session->dropRemoteFile();
    deliver(*session, asked);
    CHECK(!session->hasRemoteFile());
    CHECK((session->takeEventFlags() & arrivedBit) == 0);

    // Two clicks before either answer crosses: both answers wait, oldest
    // first, each carrying the path its own ask named. The event is one
    // coalesced bit for the pair, so a frontend reading it takes answers until
    // there are none rather than one per event — with one taken per event the
    // second would land on a later click and show that reader the earlier
    // click's file.
    session->noteAskedRemoteFileForTest(11, "/srv/logs/build.log");
    session->noteAskedRemoteFileForTest(12, "/srv/notes.txt");
    const RemoteFile second { 12, "/srv/notes.txt", { 'b' } };
    const RemoteFile first { 11, "/srv/logs/build.log", { 'a' } };
    deliver(*session, first);
    deliver(*session, second);
    CHECK((session->takeEventFlags() & arrivedBit) != 0);
    CHECK(session->remoteFileRequestID() == 11);
    CHECK(session->remoteFileRequestedPath() == "/srv/logs/build.log");
    session->dropRemoteFile();
    CHECK(session->hasRemoteFile());
    CHECK(session->remoteFileRequestID() == 12);
    CHECK(session->remoteFileRequestedPath() == "/srv/notes.txt");
    session->dropRemoteFile();
    CHECK(!session->hasRemoteFile());
    CHECK(session->remoteFileRequestedPath().empty());

    // A line link's ask carries the line its landed copy opens at, as it
    // carries the path; an ask that named none carries none.
    session->noteAskedRemoteFileForTest(21, "/srv/main.swift", Ubiquitty::Session::FileLinkLine { 12, 5 });
    session->noteAskedRemoteFileForTest(22, "/srv/notes.txt");
    deliver(*session, RemoteFile { 21, "/srv/main.swift", { 'c' } });
    deliver(*session, RemoteFile { 22, "/srv/notes.txt", { 'd' } });
    CHECK(session->remoteFileRequestedPath() == "/srv/main.swift");
    CHECK(session->remoteFileRequestedLine().line == 12 && session->remoteFileRequestedLine().column == 5);
    session->dropRemoteFile();
    CHECK(session->remoteFileRequestedPath() == "/srv/notes.txt");
    CHECK(session->remoteFileRequestedLine().line == 0);
    session->dropRemoteFile();
    CHECK(session->remoteFileRequestedLine().line == 0);

    releaseTerminalSession(session);
}

/// An agent asking to attach reaches the frontend with who is asking and what
/// it wants, and the answer clears the question.
///
/// The portable session path had no route for this at all: the daemon holds
/// the agent for 30 seconds and then denies it, so the path failed closed but
/// nobody was ever asked and a legitimate agent could not be let in.
///
/// The action is asserted separately from the request because they are
/// separate authorisations — approving a terminate as though it were an attach
/// hands over the session, which is why the prompt reads the action rather
/// than assuming one.
TEST(sessionCarriesAConsentQuestionWithWhoIsAskingAndForWhat)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    constexpr std::uint32_t consentBit = 1u << static_cast<unsigned>(EventKind::ConsentRequestPending);
    CHECK(session->pendingConsentRequestID() == 0);
    CHECK(session->pendingConsentToken() == 0);
    CHECK((session->takeEventFlags() & consentBit) == 0);

    Ubiquitty::Protocol::Sync::ConsentQuestion request;
    request.requestID = 77;
    request.sessionID = 2;
    request.peerProcessID = 23;
    request.action = 1; // terminate — the one a prompt must not call an attach
    request.peerProcess = "/usr/local/bin/some-agent";
    request.agentLabel = "Backup Helper";
    session->noteConsentRequestForTest(request);

    CHECK((session->takeEventFlags() & consentBit) != 0);
    CHECK(session->pendingConsentRequestID() == 77);
    const auto first = session->pendingConsentToken();
    REQUIRE(first != 0);
    CHECK(session->pendingConsentAction() == 1);
    // Both halves survive, and separately: the verified process is what a
    // prompt may trust, the label is what the agent chose to call itself.
    CHECK(session->pendingConsentPeerProcess() == "/usr/local/bin/some-agent");
    CHECK(session->pendingConsentAgentLabel() == "Backup Helper");

    // Answering clears it. A session that kept the question would prompt again
    // on the next drain for something already decided.
    session->respondToConsent(first, 0);
    CHECK(session->pendingConsentRequestID() == 0);
    CHECK(session->pendingConsentToken() == 0);

    // A later question receives a distinct local identity.
    request.requestID = 78;
    request.action = 0;
    session->noteConsentRequestForTest(request);
    CHECK(session->pendingConsentRequestID() == 78);
    CHECK(session->pendingConsentAction() == 0);
    const auto second = session->pendingConsentToken();
    REQUIRE(second != 0 && second != first);

    (void)session->takeEventFlags();
    for (const auto& wrong : {
             Ubiquitty::Protocol::Sync::ConsentResolution { Ubiquitty::Protocol::Sync::ConsentKind::Terminate, 78, 2 },
             Ubiquitty::Protocol::Sync::ConsentResolution { Ubiquitty::Protocol::Sync::ConsentKind::Attach, 78, 3 },
             Ubiquitty::Protocol::Sync::ConsentResolution { Ubiquitty::Protocol::Sync::ConsentKind::Attach, 79, 2 },
         }) {
        session->noteConsentResolvedForTest(wrong);
        CHECK(session->pendingConsentToken() == second);
        CHECK((session->takeEventFlags() & consentBit) == 0);
    }

    // Answering a stale token leaves the live question standing, so a late reply
    // to a question already gone cannot silently drop the current one.
    session->respondToConsent(first, 0);
    CHECK(session->pendingConsentRequestID() == 78);

    // Neither a repeated question nor an unpresentable distinct question may
    // overwrite this presentation or ask the UI to show it again.
    (void)session->takeEventFlags();
    session->noteConsentRequestForTest(request);
    request.requestID = 79;
    request.action = 1;
    session->noteConsentRequestForTest(request);
    CHECK(session->pendingConsentToken() == second);
    CHECK(session->pendingConsentRequestID() == 78);
    CHECK(session->pendingConsentAction() == 0);
    CHECK((session->takeEventFlags() & consentBit) == 0);

    // Reconnect removes the question, and a restarted host reusing its request
    // number cannot make a still-open old sheet answer this new presentation.
    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Reconnecting, "lost");
    CHECK(session->pendingConsentToken() == 0);
    CHECK(session->pendingConsentRequestID() == 0);
    CHECK((session->takeEventFlags() & consentBit) != 0);
    request.requestID = 78;
    request.action = 0;
    session->noteConsentRequestForTest(request);
    const auto fresh = session->pendingConsentToken();
    REQUIRE(fresh != 0 && fresh != second);
    session->respondToConsent(second, 1);
    CHECK(session->pendingConsentToken() == fresh);
    (void)session->takeEventFlags();
    const Ubiquitty::Protocol::Sync::ConsentResolution exact { Ubiquitty::Protocol::Sync::ConsentKind::Attach, 78, 2 };
    session->noteConsentResolvedForTest(exact);
    CHECK(session->pendingConsentToken() == 0);
    CHECK((session->takeEventFlags() & consentBit) != 0);
    session->noteConsentResolvedForTest(exact);
    CHECK((session->takeEventFlags() & consentBit) == 0);
    session->respondToConsent(fresh, 0);
    CHECK(session->pendingConsentToken() == 0);

    releaseTerminalSession(session);
}

TEST(structureConsentQueuesExactQuestionsAndRefusesAStalePresentationToken)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    Ubiquitty::Protocol::Sync::StructureConsent request {
        .requestID = 1,
        .sessionID = 2,
        .workspaceID = 3,
        .revision = 1,
        .workspaceName = "work",
        .method = "window.close",
        .parameters = R"({"window":4})",
        .agentLabel = "helper",
        .peerProcess = "/usr/bin/tool",
        .peerProcessID = 7,
    };
    session->noteStructureConsentForTest(request);
    const std::uint64_t first = session->pendingStructureConsentToken();
    REQUIRE(first != 0);
    session->noteConsentResolvedForTest({ Ubiquitty::Protocol::Sync::ConsentKind::Structure, request.requestID, request.sessionID + 1 });
    CHECK_EQ(session->pendingStructureConsentToken(), first);
    CHECK_EQ(session->pendingConsentAction(), 3u);
    CHECK(session->pendingConsentGrantIsPerUse());
    CHECK(session->pendingStructureConsentDetail().contains("end every session"));
    request.requestID = 2;
    session->noteStructureConsentForTest(request);
    CHECK_EQ(session->pendingStructureConsentToken(), first);
    session->respondToConsent(first, 1);
    CHECK_EQ(session->pendingStructureConsentToken(), first);
    session->respondToStructureConsent(first, false);
    CHECK_EQ(session->pendingConsentRequestID(), 2u);
    const std::uint64_t second = session->pendingStructureConsentToken();
    CHECK(second != first);
    session->respondToStructureConsent(first, true);
    CHECK_EQ(session->pendingStructureConsentToken(), second);
    session->noteRemoteStateChangedForTest(Ubiquitty::Session::State::Reconnecting, "lost");
    CHECK_EQ(session->pendingStructureConsentToken(), 0u);
    // A restarted host reuses a request number; the old sheet cannot answer it.
    session->noteStructureConsentForTest(request);
    const std::uint64_t fresh = session->pendingStructureConsentToken();
    CHECK(fresh != second);
    session->respondToStructureConsent(second, true);
    CHECK_EQ(session->pendingStructureConsentToken(), fresh);
    request.requestID = 3;
    session->noteStructureConsentForTest(request);
    session->noteStructureConsentResolvedForTest(2);
    const std::uint64_t afterRetirement = session->pendingStructureConsentToken();
    CHECK(afterRetirement != fresh && afterRetirement != 0);
    session->respondToStructureConsent(fresh, true);
    CHECK_EQ(session->pendingStructureConsentToken(), afterRetirement);
    session->noteStructureConsentResolvedForTest(3);
    CHECK_EQ(session->pendingStructureConsentToken(), 0u);
    session->respondToStructureConsent(afterRetirement, true);
    CHECK_EQ(session->pendingConsentRequestID(), 0u);
    releaseTerminalSession(session);
}

TEST(retiringAQueuedQuestionDoesNotRepublishTheDisplayedConsent)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    constexpr std::uint32_t consentBit = 1u << static_cast<unsigned>(EventKind::ConsentRequestPending);
    Ubiquitty::Protocol::Sync::StructureConsent request {
        .requestID = 1,
        .sessionID = 2,
        .workspaceID = 3,
        .revision = 1,
        .workspaceName = "work",
        .method = "window.close",
        .parameters = R"({"window":4})",
        .agentLabel = {},
        .peerProcess = {},
    };
    session->noteStructureConsentForTest(request);
    const std::uint64_t shown = session->pendingStructureConsentToken();
    request.requestID = 2;
    session->noteStructureConsentForTest(request);
    (void)session->takeEventFlags();
    session->noteStructureConsentResolvedForTest(2);
    CHECK_EQ(session->pendingStructureConsentToken(), shown);
    CHECK((session->takeEventFlags() & consentBit) == 0);
    session->noteStructureConsentResolvedForTest(1);
    CHECK((session->takeEventFlags() & consentBit) != 0);
    CHECK_EQ(session->pendingStructureConsentToken(), 0u);
    Ubiquitty::Protocol::Sync::ConsentQuestion legacy;
    legacy.requestID = 77;
    session->noteConsentRequestForTest(legacy);
    request.requestID = 3;
    session->noteStructureConsentForTest(request);
    (void)session->takeEventFlags();
    session->noteStructureConsentResolvedForTest(3);
    CHECK_EQ(session->pendingConsentRequestID(), 77u);
    CHECK((session->takeEventFlags() & consentBit) == 0);
    releaseTerminalSession(session);
}

/// The roster of who is attached reaches the frontend, with the verified half
/// of each identity separate from the claimed one.
///
/// A session more than one client is on is one somebody else can see, and on
/// the portable path nothing said so. The self bit is what lets a frontend
/// leave itself out of that count without guessing which row it is.
///
/// Every accessor is asked past the end too, because a frontend redraws while
/// the roster changes under it: the host rebuilds the whole list on every
/// attach and detach, so a row that existed when the paint started can be gone
/// when it reads.
TEST(sessionCarriesTheRosterOfWhoElseIsAttached)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    constexpr std::uint32_t presenceBit = 1u << static_cast<unsigned>(EventKind::PresenceChanged);
    CHECK(session->presenceAttendeeCount() == 0);
    CHECK((session->takeEventFlags() & presenceBit) == 0);

    Ubiquitty::Session::PanePresenceSnapshot presence;
    presence.sessionID = 5;
    presence.attendees.push_back({ .clientID = 11,
        .peerProcessID = 900,
        .flags = Ubiquitty::Session::presenceHuman | Ubiquitty::Session::presenceSelf,
        .grantScope = 0,
        .label = "",
        .process = "/Applications/Ubiquitty.app" });
    presence.attendees.push_back({ .clientID = 12,
        .peerProcessID = 901,
        .flags = 0,
        .grantScope = Ubiquitty::Session::grantAlways,
        .label = "Backup Helper",
        .process = "/usr/local/bin/agent" });
    session->notePresenceForTest(presence);

    CHECK((session->takeEventFlags() & presenceBit) != 0);
    CHECK(session->presenceAttendeeCount() == 2);

    // The fold every frontend reads, and the reason the self bit exists: two
    // attendees, one of them this client, is ONE other. A count that included
    // the reader would put a "someone is watching" badge on every remote
    // session there has ever been.
    CHECK(session->otherAttendeeCount() == 1);
    CHECK(session->isSharedWithOthers());

    // Row 0 is this client: a frontend counting watchers subtracts itself by
    // reading the bit rather than by assuming a position.
    CHECK(session->isPresenceAttendeeSelf(0));
    CHECK(session->isPresenceAttendeeHuman(0));
    CHECK(session->presenceClientID(0) == 11);

    // Row 1 is an agent, and its two names stay apart.
    CHECK(!session->isPresenceAttendeeSelf(1));
    CHECK(!session->isPresenceAttendeeHuman(1));
    CHECK(session->presenceClientID(1) == 12);
    CHECK(session->presenceGrantScope(1) == Ubiquitty::Session::grantAlways);
    CHECK(session->presenceProcess(1) == "/usr/local/bin/agent");
    CHECK(session->presenceLabel(1) == "Backup Helper");

    // Past the end every accessor answers a resting value rather than reading
    // off the end of the vector.
    CHECK(session->presenceClientID(9) == 0);
    CHECK(!session->isPresenceAttendeeSelf(9));
    CHECK(!session->isPresenceAttendeeHuman(9));
    CHECK(session->presenceGrantScope(9) == 0);
    CHECK(session->presenceProcess(9).empty());
    CHECK(session->presenceLabel(9).empty());

    // A later roster replaces the earlier one whole. The host rebuilds it on
    // every change, so a session that merged would keep an attendee who left.
    Ubiquitty::Session::PanePresenceSnapshot alone;
    alone.sessionID = 5;
    alone.attendees.push_back({ .clientID = 11,
        .peerProcessID = 900,
        .flags = Ubiquitty::Session::presenceHuman | Ubiquitty::Session::presenceSelf,
        .grantScope = 0,
        .label = "",
        .process = "/Applications/Ubiquitty.app" });
    session->notePresenceForTest(alone);
    CHECK(session->presenceAttendeeCount() == 1);
    CHECK(session->presenceClientID(1) == 0);
    // Alone on the session: the one row left is this client's, so nobody else
    // is watching and the badge has nothing to say.
    CHECK(session->otherAttendeeCount() == 0);
    CHECK(!session->isSharedWithOthers());

    releaseTerminalSession(session);
}

TEST(clearReportedDirectoryKeepsSampledDirectoryAndPublishesEvent)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    session->lockForRead();
    auto* terminal = session->terminal();
    terminal->sampledDirectory = "/sampled/project";
    (void)Ubiquitty::setReportedDirectoryString(*terminal, "/reported/project");
    session->unlockForRead();
    CHECK(session->canClearReportedDirectory());
    CHECK(session->inspectorInfo().directory == "/reported/project");
    (void)session->takeEventFlags();
    CHECK(session->clearReportedDirectory());
    constexpr auto directoryChanged = 1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::DirectoryChanged);
    CHECK((session->takeEventFlags() & directoryChanged) != 0);
    CHECK(session->inspectorInfo().directory == "/sampled/project");
    session->lockForRead();
    CHECK(terminal->reportedDirectory.empty());
    CHECK(terminal->reportingProcessGroup == 0);
    CHECK(terminal->sampledDirectory == "/sampled/project");
    session->unlockForRead();
    CHECK(!session->canClearReportedDirectory());
    CHECK(!session->clearReportedDirectory());
    CHECK((session->takeEventFlags() & directoryChanged) == 0);
    releaseTerminalSession(session);
}

/// A host pane's facts arrive as whole records, each replacing the last, and
/// none of them touches the program's OSC title.
TEST(sessionTakesEachPaneFactsRecordWhole)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    session->lockForRead();
    session->terminal()->titleUTF8 = "Program title";
    session->unlockForRead();

    constexpr auto directoryChanged = 1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::DirectoryChanged);
    constexpr auto processChanged = 1u << static_cast<unsigned>(Ubiquitty::Runtime::EventKind::ProcessStateChanged);
    CHECK((session->takeEventFlags() & directoryChanged) == 0);
    Ubiquitty::Protocol::Sync::PaneFacts first;
    first.revision = 1;
    first.currentDirectory = "/srv/project";
    first.processChain = "-zsh";
    session->applyRemoteTitlePartsForTest(first);
    CHECK(session->titleForTest() == "Program title");
    CHECK(session->inspectorInfo().directory == "/srv/project");
    std::uint32_t flags = session->takeEventFlags();
    CHECK((flags & directoryChanged) != 0);
    CHECK((flags & processChanged) != 0);

    // A record whose process moved and whose directory did not raises only
    // the process.
    Ubiquitty::Protocol::Sync::PaneFacts moved = first;
    moved.revision = 2;
    moved.processChain = "vim ◂ -zsh";
    session->applyRemoteTitlePartsForTest(moved);
    CHECK(session->titleForTest() == "Program title");
    CHECK(session->inspectorInfo().directory == "/srv/project");
    flags = session->takeEventFlags();
    CHECK((flags & directoryChanged) == 0);
    CHECK((flags & processChanged) != 0);

    // The same record again raises nothing.
    session->applyRemoteTitlePartsForTest(moved);
    CHECK((session->takeEventFlags() & (directoryChanged | processChanged)) == 0);

    // A record is the host's whole answer: a field it leaves empty is empty,
    // not a part some earlier frame still owns.
    Ubiquitty::Protocol::Sync::PaneFacts cleared;
    cleared.revision = 3;
    session->applyRemoteTitlePartsForTest(cleared);
    CHECK(session->titleForTest() == "Program title");
    CHECK(session->inspectorInfo().directory.empty());
    CHECK((session->takeEventFlags() & directoryChanged) != 0);

    releaseTerminalSession(session);
}

TEST(remoteRecordsPublishProgressAndTabStatusBeforeNotifyingTheFrontend)
{
    using namespace Ubiquitty;
    auto* session = Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    (void)session->takeEventFlags();
    Protocol::Sync::PaneFacts facts;
    facts.progress = { .state = OSC::ProgressIndeterminate, .value = 0 };
    facts.badgeText = "building";
    session->lockForRead();
    Protocol::Sync::applyRecorded(*session->terminal(), facts);
    session->unlockForRead();
    // A separate frontend operation can publish the new snapshot first;
    // that must not consume the progress notification owed by the records.
    session->clearScrollbackSearch();
    session->noteRemoteScreenChangedForTest();
    constexpr auto progressChanged = 1u << static_cast<unsigned>(Runtime::EventKind::ProgressChanged);
    constexpr auto statusChanged = 1u << static_cast<unsigned>(Runtime::EventKind::TabStatusChanged);
    CHECK(session->readSnapshot().progressState == OSC::ProgressIndeterminate);
    CHECK((session->takeEventFlags() & (progressChanged | statusChanged)) == (progressChanged | statusChanged));

    // A later screen-only repaint does not repeat a records edge.
    session->noteRemoteScreenChangedForTest();
    CHECK((session->takeEventFlags() & (progressChanged | statusChanged)) == 0);

    facts.progress = {};
    facts.badgeText.clear();
    session->lockForRead();
    Protocol::Sync::applyRecorded(*session->terminal(), facts);
    session->unlockForRead();
    session->noteRemoteScreenChangedForTest();
    CHECK(session->readSnapshot().progressState == OSC::ProgressNone);
    CHECK((session->takeEventFlags() & (progressChanged | statusChanged)) == (progressChanged | statusChanged));
    releaseTerminalSession(session);
}

/// A routed session wraps its input in send-keys for its pane and rides the
/// -CC primary's stream instead of a local PTY; clearing the route silences
/// the primary. The wrap itself has its own tests — this is the session's
/// funnel: every input path goes through sendInput, so a routed session's
/// frontend needs no tmux awareness at all.
TEST(sessionRoutedInputWrapsForThePaneAndRidesThePrimaryStream)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    // The primary's control channel, write-capturing.
    Ubiquitty::PAL::StreamTestPipe primary;

    // A live -CC client — configured and past the handshake — so the wrap is
    // send-keys rather than the unattached client's verbatim passthrough.
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    Ubiquitty::Protocol::Tmux::init(&client);
    Ubiquitty::Protocol::Tmux::configure(&client, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    const char handshake[] = "%begin 1 1 0\r\n%end 1 1 0\r\n";
    for (const char* byte = handshake; *byte != '\0'; ++byte)
        Ubiquitty::Protocol::Tmux::feedInput(&client, static_cast<uint8_t>(*byte), nullptr, discardFilteredByte);

    session->routeInputViaTmux(&client, &primary, 7);
    const char keys[] = "ls\n";
    session->sendInput({ keys, 3 });
    CHECK(primary.written.find("send-keys -t %7") != std::string::npos);

    constexpr std::string_view application = "\033[?1049h\033[?1h";
    session->feedBytes({ application.data(), application.size() });
    TerminalSession::ScrollFact swipe;
    swipe.isPrecise = true;
    swipe.isDirectTouch = true;
    swipe.deltaY = -16;
    Ubiquitty::Screen::ScrollTuning::Policy tuning;
    tuning.pointsPerLine = 16;
    primary.written.clear();
    CHECK(session->scrollFact(swipe, tuning).wasConsumed);
    CHECK(primary.written.find("send-keys -t %7") != std::string::npos);
    CHECK(primary.written.find("1b 4f 42") != std::string::npos);

    // Semantic input uses the same local encoding, not a remote Input frame.
    constexpr std::string_view tracking = "\033[?1000h\033[?1006h";
    session->feedBytes({ tracking.data(), tracking.size() });
    primary.written.clear();
    Ubiquitty::Runtime::KeyPress key;
    key.unshiftedCodePoint = 'a';
    key.shiftedCodePoint = 'a';
    std::array<char, 64> encoded {};
    CHECK_EQ(session->sendKey(key, encoded).length, 1);
    CHECK(primary.written.find("send-keys -t %7 -H 61") != std::string::npos);
    primary.written.clear();
    CHECK_STR_EQ(session->sendPaste("b\nc"), "b\rc");
    CHECK(primary.written.find("send-keys -t %7 -H 62 0d 63") != std::string::npos);
    primary.written.clear();
    TerminalSession::PointerFact click;
    click.kind = TerminalSession::PointerFact::Kind::Press;
    click.button = Ubiquitty::Input::PointerButton::Left;
    click.row = 2;
    click.column = 3;
    CHECK(session->pointerFact(click, nullptr).wasConsumed);
    CHECK(primary.written.find("send-keys -t %7 -H 1b 5b 3c 30 3b 34 3b 33 4d") != std::string::npos);

    // Cleared, the primary hears nothing more from this session's input.
    session->routeInputViaTmux(nullptr, nullptr, 0);
    const std::size_t heard = primary.written.size();
    session->sendInput({ keys, 3 });
    CHECK(primary.written.size() == heard);

    session->close();
    releaseTerminalSession(session);
}

/// Broadcast asks one question of every pane -- can it take input -- and the
/// answer is the routes sendInput takes. A session with none of them drops
/// what it is sent, so a fan-out must neither target it nor count it in the
/// confirmation that names how many panes the keystrokes will reach.
TEST(aSessionTakesInputOnlyWhileARouteReachesAHost)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK_MSG(!session->hasHostInputSink(), "a model with no host drops input, as a passive mirror does");

    Ubiquitty::PAL::StreamTestPipe primary;
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    Ubiquitty::Protocol::Tmux::init(&client);
    session->routeInputViaTmux(&client, &primary, 7);
    CHECK_MSG(session->hasHostInputSink(), "a tmux pane takes input through the -CC primary");
    session->routeInputViaTmux(nullptr, nullptr, 0);
    CHECK_MSG(!session->hasHostInputSink(), "and takes none once that route is cleared");

    auto adoption = session->adoptStream(std::make_unique<Ubiquitty::PAL::StreamTestPipe>());
    REQUIRE(adoption.has_value());
    CHECK_MSG(session->hasHostInputSink(), "a host stream takes input directly");

    session->close();
    CHECK_MSG(!session->hasHostInputSink(), "and a closed session takes none");
    releaseTerminalSession(session);
}

/// The DEC locator's arm edge, at the session: the frontend says where a
/// pointer that has not moved stands, and a report that comes back -- a
/// DECEFR in the same drain armed a rectangle the pointer already stands
/// outside -- reaches the host rather than being dropped.
TEST(aSeededLocatorSendsTheReportItsRectangleOwes)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* observed = stream.get();
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    // DECELR in cells, then a one-cell rectangle at the origin.
    constexpr std::string_view arm = "\x1b[1;2'z\x1b[1;1;1;1'w";
    session->feedBytes({ arm.data(), arm.size() });
    REQUIRE(session->readSnapshot().locatorActive);

    Ubiquitty::Runtime::TerminalSession::PointerFact stationary;
    stationary.row = 2;
    stationary.column = 5;
    session->seedLocator(stationary);
    CHECK_MSG(observed->written == "\x1b[10;0;3;6;1&w", "the pointer stands outside the rectangle: Pe 10 at row 3, column 6");

    // The position stays for DECRQLP, which answers it rather than omitting it. A
    // reply to fed bytes waits in the queue for the next drain, so it is
    // flushed here, as that drain would.
    observed->written.clear();
    constexpr std::string_view request = "\x1b['|";
    session->feedBytes({ request.data(), request.size() });
    session->flushReplies();
    CHECK_MSG(observed->written == "\x1b[1;0;3;6;1&w", "DECRQLP answers the seeded position");

    session->close();
    releaseTerminalSession(session);
}

namespace {

/// A factory that takes an errand and never answers it.
///
/// `createChannel` keeps the completion rather than calling it, so the
/// directory the case drives stays in flight for as long as the case needs —
/// which is what "busy" means to the errand rule under test.
class StallingChannelFactory final : public Ubiquitty::Session::ChannelFactory {
public:
    void createChannel(Ubiquitty::Session::TransportMode, Done done) override
    {
        ++createCount;
        m_held = std::move(done);
    }

    int createCount = 0;

private:
    Done m_held;
};

// The session's platform loop. It discards each unit as it is posted rather
// than running it: the stalling factory hands the directory no carrier, so
// nothing these would run exists, and what is posted is timers holding weak
// references. It must exist though — a session refuses a loop that lacks an
// entry point.
void discardPostedWork(Ubiquitty::Runtime::PostedWorkToken token) { Ubiquitty::Runtime::discardPostedSessionWork(token); }
void discardDelayedWork(double, Ubiquitty::Runtime::PostedWorkToken token) { Ubiquitty::Runtime::discardPostedSessionWork(token); }

Ubiquitty::Runtime::PostedWorkLoop discardingLoop() { return { .runSoon = &discardPostedWork, .runAfter = &discardDelayedWork }; }

} // namespace

/// A host's answer reaches the frontend as one event, raised once, and the
/// tree it reported is what the session decides and draws on.
TEST(sessionPublishesEachListingAnswerOnce)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    constexpr std::uint32_t listBit = 1u << static_cast<unsigned>(EventKind::HostSessionListChanged);

    // Nothing asked, nothing answered: a frontend reading before the first
    // answer sees no tree rather than a stale one.
    CHECK(!session->hasHostAnsweredListing());
    CHECK((session->takeEventFlags() & listBit) == 0);

    session->noteHostSessionTreeForTest(Ubiquitty::Session::SessionTree {});
    session->noteHostListingAnsweredForTest();

    // Raised once: the bit is set by the answer and does not come back on the
    // next pass, which is what a frontend pumping on events depends on.
    CHECK((session->takeEventFlags() & listBit) != 0);
    CHECK((session->takeEventFlags() & listBit) == 0);
    // A host that answered it holds nothing is a new session, never a wait.
    CHECK(session->hasHostAnsweredListing());
    CHECK(session->decideHostAttach("answer-once.example", "", 0).plan == Ubiquitty::Session::AttachPlan::CreateNew);

    releaseTerminalSession(session);
}

/// A session keeps the tree a listing reported before it completed and hands out
/// its outline; an answer with no tree leaves none to draw.
TEST(sessionHoldsTheListedHostsTreeAndHandsOutItsOutline)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    CHECK(session->hostSessionTreeRevision() == 0);
    CHECK(session->hostOutline({}).rows.empty());
    CHECK(session->hostOutlineRows({}).count() == 0);

    // One workspace of one window, one tab and one pane: one row.
    const auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    const Ubiquitty::Session::Layout::Workspace layout(identities);
    const Ubiquitty::Session::Identity::LabelBook labels;
    UbiquittyTest::StatedPane pane;
    pane.title.programTitle = "make all";
    pane.paneID = { .low = 1, .high = 7 };
    pane.activity = { 90'000, 0 };
    const std::array sources { Ubiquitty::Session::TreeWorkspaceSource { .id = 9, .name = "work", .layout = &layout } };
    const Ubiquitty::Session::SessionTree tree = Ubiquitty::Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); });

    session->noteHostSessionTreeForTest(tree);
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeRevision() == 1);
    Ubiquitty::Runtime::HostOutlineRequest request;
    const Ubiquitty::App::SessionOutline outline = session->hostOutline(request);
    REQUIRE(outline.rows.size() == 1);
    CHECK(outline.rows[0].title == "work");
    CHECK(outline.rows[0].sinceOutput.isKnown);
    // The host's clock is kept running by this machine's, so the age is the
    // ten seconds the host stated or a moment more, never less.
    CHECK(outline.rows[0].sinceOutput.seconds >= 10);
    CHECK(outline.rows[0].sinceOutput.seconds < 70);
    const Ubiquitty::Runtime::HostOutlineRows rows = session->hostOutlineRows(request);
    CHECK(rows.count() == 1);
    CHECK(rows.title(0) == "work");
    // A search reaches the rows.
    request.query = "nothing has this";
    CHECK(session->hostOutline(request).rows.empty());
    // The session's own theme colours the rows unless the request names one.
    request.query.clear();
    const Ubiquitty::Themes::StatusColors own = session->statusColors();
    const auto slot = outline.rows[0].statusSlot;
    CHECK(Ubiquitty::Runtime::HostOutlineDetail::packed(outline.rows[0].statusColor) == Ubiquitty::Runtime::HostOutlineDetail::packed(own.color(slot)));
    request.themeIndex = 0;
    CHECK(Ubiquitty::Runtime::HostOutlineDetail::packed(session->hostOutline(request).rows.at(0).statusColor)
        == Ubiquitty::Runtime::HostOutlineDetail::packed(Ubiquitty::Themes::get(0)->statusColors().color(slot)));

    // A newer answer with a tree replaces it; one without leaves none.
    session->noteHostSessionTreeForTest(tree);
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeRevision() == 2);
    CHECK(session->hostOutline({}).rows.size() == 1);
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeRevision() == 3);
    CHECK(session->hostOutline({}).rows.empty());
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeRevision() == 3);

    releaseTerminalSession(session);
}

/// A session decides the connect to its listed host on the tree that host
/// answered with: nothing is read as an empty host before an answer, a name
/// the tree bears is attached to, and a host with workspaces and no name is
/// the person's to choose.
TEST(sessionDecidesTheConnectOnItsListedHostsTree)
{
    using Ubiquitty::Session::AttachPlan;
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    CHECK(!session->hasHostAnsweredListing());
    CHECK(session->decideHostAttach("tree-decide.example", "", 0).plan == AttachPlan::AwaitListing);
    CHECK(session->decideHostAttach("tree-decide.example", "work", 0).plan == AttachPlan::CreateNamed);

    const auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    const Ubiquitty::Session::Layout::Workspace layout(identities);
    const Ubiquitty::Session::Identity::LabelBook labels;
    UbiquittyTest::StatedPane pane;
    pane.sessionID = 41;
    pane.paneID = { .low = 41, .high = 7 };
    const std::array sources { Ubiquitty::Session::TreeWorkspaceSource { .id = 9, .name = "work", .layout = &layout } };
    const Ubiquitty::Session::SessionTree tree = Ubiquitty::Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); });
    session->noteHostSessionTreeForTest(tree);
    session->noteHostListingAnsweredForTest();

    CHECK(session->hasHostAnsweredListing());
    const Ubiquitty::Session::AttachDecision named = session->decideHostAttach("tree-decide.example", "work", 0);
    CHECK(named.plan == AttachPlan::AttachNamed);
    CHECK(named.workspaceID == 9);
    const Ubiquitty::Session::AttachDecision unnamed = session->decideHostAttach("tree-decide.example", "", 0);
    CHECK(unnamed.plan == AttachPlan::ChooseFromList);
    CHECK(unnamed.ask == Ubiquitty::Session::AttachAsk::WhichWorkspace);
    CHECK(unnamed.candidates.size() == 1 && unnamed.candidates[0].workspaceID == 9);

    // An answer that failed leaves no tree, which is a host that has not
    // answered rather than one that holds nothing.
    session->noteHostListingAnsweredForTest();
    CHECK(!session->hasHostAnsweredListing());
    CHECK(session->decideHostAttach("tree-decide.example", "", 0).plan == AttachPlan::AwaitListing);

    releaseTerminalSession(session);
}

/// A session asks its listed host for a change by a typed request, and each
/// ends as a numbered result behind one event: refused here against the tree
/// it holds, served, refused in the host's words, or unanswered.
TEST(sessionAsksItsListedHostForAChangeAndSaysHowEachEnded)
{
    using Ubiquitty::Runtime::StructureOperation;
    using Ubiquitty::Runtime::StructureOutcome;
    using Ubiquitty::Runtime::StructureRequest;
    using Ubiquitty::Session::Identity::ObjectID;
    using Ubiquitty::Session::Identity::ObjectKind;
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    constexpr std::uint32_t settledBit = 1u << static_cast<unsigned>(EventKind::HostStructureSettled);

    // No host listed: nothing is asked and nothing settles.
    CHECK_EQ(session->setHostLabel({ ObjectKind::Workspace, 9 }, "x"), 0u);
    CHECK((session->takeEventFlags() & settledBit) == 0);
    CHECK_EQ(session->lastHostStructureRequestID(), 0u);

    const auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    const Ubiquitty::Session::Layout::Workspace layout(identities);
    const Ubiquitty::Session::Identity::LabelBook labels;
    UbiquittyTest::StatedPane pane;
    pane.paneID = { .low = 1, .high = 7 };
    const std::array sources { Ubiquitty::Session::TreeWorkspaceSource { .id = 9, .name = "work", .layout = &layout } };
    Ubiquitty::Session::SessionTree tree = Ubiquitty::Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); });
    tree.workspaces[0].revision = 1;
    session->noteHostSessionTreeForTest(tree);
    const ObjectID tab { ObjectKind::Tab, std::to_underlying(tree.workspaces[0].windows[0].tabs[0].id) };
    (void)session->takeEventFlags();

    // Refused here, against the tree: a result and the event, in Core's words.
    const std::uint64_t absent = session->renameHostTab({ ObjectKind::Tab, 99'999 }, "logs");
    CHECK(absent != 0);
    CHECK((session->takeEventFlags() & settledBit) != 0);
    CHECK(session->lastHostStructureRequestID() == absent);
    CHECK(session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::RefusedHere));
    CHECK(session->lastHostStructureOperation() == std::to_underlying(StructureOperation::RenameTab));
    CHECK(session->lastHostStructureTargetKind() == std::to_underlying(ObjectKind::Tab) && session->lastHostStructureTargetNumber() == 99'999);
    CHECK_STR_EQ(session->lastHostStructureReason(), "tab:99999 is no longer on this host’s list.");
    // With no connection to send on, a sound request is refused here too.
    const std::uint64_t unsent = session->renameHostTab(tab, "logs");
    CHECK(session->lastHostStructureRequestID() == unsent && session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::RefusedHere));
    CHECK(!session->lastHostStructureReason().empty());
    // The entry a language seam calls takes the same request as scalars, and
    // an operation or a kind no build knows asks nothing.
    const Ubiquitty::Runtime::StructureTags none;
    const std::uint64_t scalar = session->requestHostStructureChange(std::to_underlying(StructureOperation::CloseWindow), std::to_underlying(ObjectKind::Window), 77'777, "", none, 0, 0, 0);
    CHECK(scalar > unsent && session->lastHostStructureRequestID() == scalar && session->lastHostStructureOperation() == std::to_underlying(StructureOperation::CloseWindow));
    CHECK_STR_EQ(session->lastHostStructureReason(), "window:77777 is no longer on this host’s list.");
    CHECK_EQ(session->requestHostStructureChange(0, std::to_underlying(ObjectKind::Tab), 1, "", none, 0, 0, 0), 0u);
    CHECK_EQ(session->requestHostStructureChange(7, std::to_underlying(ObjectKind::Tab), 1, "", none, 0, 0, 0), 0u);
    CHECK_EQ(session->requestHostStructureChange(std::to_underlying(StructureOperation::SetLabel), 6, 1, "", none, 0, 0, 0), 0u);
    CHECK(session->lastHostStructureRequestID() == scalar);
    (void)session->takeEventFlags();

    // A call the host answers: served, then refused in the host's words.
    StructureRequest rename;
    rename.operation = StructureOperation::RenameTab;
    rename.target = tab;
    rename.text = "logs";
    const std::string call = session->holdHostStructureCallForTest(rename);
    REQUIRE(call.find("\"method\":\"tab.rename\"") != std::string::npos);
    CHECK((session->takeEventFlags() & settledBit) == 0);
    const std::uint64_t held = scalar + 1;
    const std::string id = Ubiquitty::Runtime::StructureCalls::callID(held);
    CHECK(call.find(id) != std::string::npos);
    session->noteHostStructureAnswerForTest(R"({"jsonrpc":"2.0","id":")" + id + R"(","result":{"tab":"tab:1","name":"logs"}})");
    CHECK((session->takeEventFlags() & settledBit) != 0);
    CHECK(session->lastHostStructureRequestID() == held && session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::Served));
    CHECK(session->lastHostStructureReason().empty());
    CHECK(session->lastHostStructureResult().target == tab);

    const std::string again = session->holdHostStructureCallForTest(rename);
    session->noteHostStructureAnswerForTest(R"({"jsonrpc":"2.0","id":")" + Ubiquitty::Runtime::StructureCalls::callID(held + 1) + R"(","error":{"code":-32602,"message":"a tab's name is at most 4096 bytes, and this one is 5000"}})");
    CHECK(session->lastHostStructureRequestID() == held + 1 && session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::RefusedByHost));
    CHECK_STR_EQ(session->lastHostStructureReason(), "a tab's name is at most 4096 bytes, and this one is 5000");
    CHECK(!again.empty());

    // A carrier that drops under a call leaves it unanswered, and leaves the
    // tree the session holds alone.
    (void)session->holdHostStructureCallForTest(rename);
    const std::uint64_t treeBefore = session->hostSessionTreeRevision();
    session->noteHostStructureAnswerForTest("");
    CHECK(session->lastHostStructureRequestID() == held + 2 && session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::Unanswered));
    CHECK_STR_EQ(session->lastHostStructureReason(), "The host could not be reached.");
    CHECK_EQ(session->hostSessionTreeRevision(), treeBefore);
    CHECK_EQ(session->hostOutline({}).rows.size(), 1u);

    // A request that ends sessions is asked about first, in Core's words,
    // and sent only with the token of its own question.
    using Ubiquitty::Runtime::DestructiveRequest;
    const std::uint8_t closeIt = std::to_underlying(DestructiveRequest::CloseWindow);
    const std::uint8_t endIt = std::to_underlying(DestructiveRequest::TerminateWorkspace);
    const ObjectID window { ObjectKind::Window, std::to_underlying(tree.workspaces[0].windows[0].id) };
    const char* const unconfirmed = "This was not confirmed. Nothing is ended without asking first.";
    CHECK_EQ(session->askHostDestructiveConfirmation(0, std::to_underlying(ObjectKind::Window), window.number, "devbox"), 0u);
    CHECK_STR_EQ(session->hostDestructiveConfirmationRefusal(), "This is not a request this build asks about.");
    CHECK_EQ(session->askHostDestructiveConfirmation(closeIt, std::to_underlying(ObjectKind::Window), 77'777, "devbox"), 0u);
    CHECK_STR_EQ(session->hostDestructiveConfirmationRefusal(), "window:77777 is no longer on this host’s list.");
    CHECK(session->hostDestructiveConfirmationTitle().empty());
    const std::uint64_t closeToken = session->askHostDestructiveConfirmation(closeIt, std::to_underlying(ObjectKind::Window), window.number, "devbox");
    REQUIRE(closeToken != 0);
    CHECK_STR_EQ(session->hostDestructiveConfirmationTitle(), "Close this window on devbox?");
    CHECK(!session->hostDestructiveConfirmationBody().empty() && session->hostDestructiveConfirmationRefusal().empty());
    CHECK_STR_EQ(session->hostDestructiveConfirmationSafeButton(), "Keep Window");
    CHECK_STR_EQ(session->hostDestructiveConfirmationDestructiveButton(), "Close Window");
    CHECK_MSG(session->hostDestructiveConfirmationSessionCount() == 0, "no session draws into this tree's pane");
    (void)session->closeHostWindow(window, 0);
    CHECK_STR_EQ(session->lastHostStructureReason(), unconfirmed);
    (void)session->requestHostStructureChange(std::to_underlying(StructureOperation::CloseWindow), std::to_underlying(ObjectKind::Window), window.number, "", none, 0, 0, closeToken + 1);
    CHECK_STR_EQ(session->lastHostStructureReason(), unconfirmed);
    // With its token it passes the question and stops at the connection
    // this session does not have.
    (void)session->closeHostWindow(window, closeToken);
    CHECK(session->lastHostStructureOutcome() == std::to_underlying(StructureOutcome::RefusedHere) && session->lastHostStructureReason() != unconfirmed);

    CHECK_EQ(session->askHostDestructiveConfirmation(endIt, std::to_underlying(ObjectKind::Workspace), 9, "devbox"), 0u);
    CHECK_STR_EQ(session->hostDestructiveConfirmationRefusal(), "Nothing runs in this workspace, so there is nothing to terminate.");
    pane.sessionID = 5;
    session->noteHostSessionTreeForTest(Ubiquitty::Session::buildSessionTree({ .revision = 2, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); }));
    const std::uint64_t endToken = session->askHostDestructiveConfirmation(endIt, std::to_underlying(ObjectKind::Workspace), 9, "devbox");
    REQUIRE(endToken != 0);
    CHECK_EQ(session->hostDestructiveConfirmationSessionCount(), 1u);
    CHECK_STR_EQ(session->hostDestructiveConfirmationTitle(), "Terminate work on devbox?");
    CHECK_EQ(session->terminateHostSessions(closeIt, std::to_underlying(ObjectKind::Window), window.number, closeToken), 0u);
    CHECK_STR_EQ(session->lastHostTerminateRefusal(), "This is not a request this build sends.");
    // No connection to this host: said before the token is spent.
    CHECK_EQ(session->terminateHostSessions(endIt, std::to_underlying(ObjectKind::Workspace), 9, endToken), 0u);
    CHECK_STR_EQ(session->lastHostTerminateRefusal(), "This host is answering another request. Try again in a moment.");

    releaseTerminalSession(session);
}

/// A directory client's doors report a tree as a listing and a watch do: held
/// as of the listing, then followed, then left behind with the reason when the
/// watch loses its host, and followed again when it is back.
TEST(aDirectoryClientsDoorsReportATreeAsAListingAndAWatchDo)
{
    struct IdleDispatcher final : Ubiquitty::Session::Dispatcher {
        void post(Ubiquitty::Session::PostedWork) override { }
        void postDelayed(double, Ubiquitty::Session::PostedWork) override { }
    };
    struct NoChannels final : Ubiquitty::Session::ChannelFactory {
        void createChannel(Ubiquitty::Session::TransportMode, Done) override { }
    };
    using Client = Ubiquitty::Runtime::HostDirectoryClient;
    IdleDispatcher dispatcher;
    NoChannels channels;
    Client client { dispatcher, channels };
    int changes = 0;
    client.setChangeCallback([&changes] { ++changes; });

    const auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    const Ubiquitty::Session::Layout::Workspace layout(identities);
    const Ubiquitty::Session::Identity::LabelBook labels;
    UbiquittyTest::StatedPane pane;
    pane.title.programTitle = "make all";
    pane.paneID = { .low = 1, .high = 7 };
    const std::array sources { Ubiquitty::Session::TreeWorkspaceSource { .id = 9, .name = "work", .layout = &layout } };
    const Ubiquitty::Session::SessionTree tree = Ubiquitty::Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); });

    CHECK(client.snapshot().treeFreshness == Client::TreeFreshness::None);
    client.noteListedSessionTreeForTest(tree);
    Client::Snapshot held = client.snapshot();
    CHECK(held.tree && held.treeFreshness == Client::TreeFreshness::Listed);
    CHECK(held.phase == Client::ListingPhase::Listed && held.event == Client::Event::SessionList);
    CHECK_EQ(changes, 1);
    CHECK_EQ(client.outline({}).rows.size(), 1u);

    client.noteWatchedSessionTreeForTest(tree);
    held = client.snapshot();
    CHECK(held.tree && held.treeFreshness == Client::TreeFreshness::Live && held.event == Client::Event::SessionTree);
    CHECK(held.watchError.empty());
    CHECK_EQ(changes, 2);

    Ubiquitty::Session::DirectoryFailureReport dropped;
    dropped.detail = "connection reset";
    client.noteWatchInterruptedForTest(dropped);
    held = client.snapshot();
    CHECK_MSG(held.tree && held.treeFreshness == Client::TreeFreshness::Stale, "the tree is kept and is no longer followed");
    CHECK(held.event == Client::Event::WatchInterrupted);
    CHECK(held.watchError.find("connection reset") != std::string::npos);
    CHECK_EQ(client.outline({}).rows.size(), 1u);

    client.noteWatchedSessionTreeForTest(tree);
    held = client.snapshot();
    CHECK(held.treeFreshness == Client::TreeFreshness::Live && held.watchError.empty());
}

/// A settled terminate says which session it was about, beside how it went.
///
/// The outcome rides the event as a scalar and the session id is read beside
/// it, because an event carries one scalar and a picker holding several rows
/// cannot tell which of them an outcome applies to without both.
TEST(sessionSaysWhichSessionASettledTerminateWasAbout)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    constexpr std::uint32_t settledBit = 1u << static_cast<unsigned>(EventKind::HostSessionTerminateSettled);
    CHECK((session->takeEventFlags() & settledBit) == 0);
    CHECK(session->lastHostSessionTerminateSessionID() == 0);

    session->noteHostSessionTerminateSettledForTest(41, Ubiquitty::Session::TerminateOutcome::Denied);

    CHECK((session->takeEventFlags() & settledBit) != 0);
    CHECK(session->lastHostSessionTerminateSessionID() == 41);
    CHECK(session->lastHostSessionTerminateOutcome()
        == static_cast<std::uint8_t>(Ubiquitty::Session::TerminateOutcome::Denied));

    releaseTerminalSession(session);
}

/// A confirmed terminate of a workspace asks the host for each session in
/// turn, and one the host cannot be asked for settles as failed, so every
/// session the question named is answered for.
TEST(aConfirmedWorkspaceTerminateAnswersForEverySession)
{
    using Ubiquitty::Runtime::DestructiveRequest;
    using Ubiquitty::Session::Identity::ObjectKind;
    struct UnreachableChannelFactory final : Ubiquitty::Session::ChannelFactory {
        int dialled = 0;
        void createChannel(Ubiquitty::Session::TransportMode, Done done) override
        {
            ++dialled;
            done(nullptr, false);
        }
    };
    UnreachableChannelFactory factory;
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    (void)session->listHostSessions(reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory)), discardingLoop());
    REQUIRE(factory.dialled == 1);

    Ubiquitty::Session::SessionTree tree;
    Ubiquitty::Session::TreeWorkspace work;
    work.id = 9;
    work.name = "work";
    Ubiquitty::Session::TreeTab tab;
    for (const std::uint32_t sessionID : { 31u, 32u, 33u }) {
        Ubiquitty::Session::TreePane pane;
        pane.id = Ubiquitty::Session::Layout::PaneID { sessionID + 100 };
        pane.sessionID = sessionID;
        tab.panes.push_back(pane);
    }
    Ubiquitty::Session::TreeWindow window;
    window.tabs = { tab };
    work.windows = { window };
    tree.workspaces = { work };
    session->noteHostSessionTreeForTest(tree);
    (void)session->takeEventFlags();

    const std::uint8_t endIt = std::to_underlying(DestructiveRequest::TerminateWorkspace);
    const std::uint64_t token = session->askHostDestructiveConfirmation(endIt, std::to_underlying(ObjectKind::Workspace), 9, "devbox");
    REQUIRE(token != 0);
    CHECK_EQ(session->terminateHostSessions(endIt, std::to_underlying(ObjectKind::Workspace), 9, token + 1), 0u);
    CHECK_STR_EQ(session->lastHostTerminateRefusal(), "This was not confirmed. Nothing is ended without asking first.");
    CHECK_EQ(factory.dialled, 1);
    CHECK_EQ(session->terminateHostSessions(endIt, std::to_underlying(ObjectKind::Workspace), 9, token), 3u);
    CHECK(session->lastHostTerminateRefusal().empty());
    // Each was dialled for, in the tree's order, and the last to settle is
    // the last session.
    CHECK_EQ(factory.dialled, 4);
    CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::HostSessionTerminateSettled))) != 0);
    CHECK_EQ(session->lastHostSessionTerminateSessionID(), 33u);
    CHECK(session->lastHostSessionTerminateOutcome() == static_cast<std::uint8_t>(Ubiquitty::Session::TerminateOutcome::Failed));
    // Spent, and nothing is owed.
    CHECK_EQ(session->terminateHostSessions(endIt, std::to_underlying(ObjectKind::Workspace), 9, token), 0u);
    CHECK_STR_EQ(session->lastHostTerminateRefusal(), "This was not confirmed. Nothing is ended without asking first.");

    releaseTerminalSession(session);
}

/// A refresh while the directory is already asking is refused, not queued.
///
/// One channel per errand is the directory's whole shape, and a caller that
/// cannot see its request start has no way to tell a queued ask from a lost
/// one. The factory here takes the first errand and never answers it, which
/// is the state a slow or unreachable host puts the directory in.
TEST(aHostSessionRefreshIsRefusedWhileTheDirectoryIsBusy)
{
    StallingChannelFactory factory;
    const std::uintptr_t address
        = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));

    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    CHECK(session->listHostSessions(address, discardingLoop()));
    CHECK(factory.createCount == 1);

    // The second ask is refused rather than dialled again: it would be a
    // second channel for one errand.
    CHECK(!session->listHostSessions(address, discardingLoop()));
    CHECK(factory.createCount == 1);

    // A zero factory is refused before anything is built, so a frontend
    // passing an address it never obtained does not dereference it.
    CHECK(!session->listHostSessions(0, discardingLoop()));

    releaseTerminalSession(session);
}

/// A watch runs on a connection of its own: a listing that is out does not
/// stop one starting, a watch already out over the same factory is kept
/// rather than dialled again, and the listing keeps its own errand.
TEST(aHostWatchRunsBesideAListingAndIsNotDialledTwice)
{
    StallingChannelFactory factory;
    const std::uintptr_t address
        = reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(&factory));

    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    CHECK(session->listHostSessions(address, discardingLoop()));
    CHECK(factory.createCount == 1);
    CHECK(session->watchHostSessions(address, discardingLoop()));
    CHECK_MSG(factory.createCount == 2, "the watch did not dial a connection of its own beside the listing");
    CHECK(session->watchHostSessions(address, discardingLoop()));
    CHECK_MSG(factory.createCount == 2, "a second ask dialled a second watch");
    CHECK(!session->watchHostSessions(0, discardingLoop()));

    // Nothing reported yet, so nothing is followed.
    CHECK(session->hostSessionTreeFreshness() == Ubiquitty::Runtime::HostDirectoryClient::TreeFreshness::None);
    session->stopWatchingHostSessions();
    CHECK(session->hostSessionTreeFreshness() == Ubiquitty::Runtime::HostDirectoryClient::TreeFreshness::None);

    releaseTerminalSession(session);
}

/// Each tree a watch reports is published as it arrives and is the host's
/// word while the watch is connected; once the watch loses its host the
/// tree it left is stale, never live, and the change is published once.
TEST(aWatchedHostTreeIsLiveUntilTheWatchLosesItsHost)
{
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    using Freshness = Ubiquitty::Runtime::HostDirectoryClient::TreeFreshness;
    constexpr std::uint32_t listBit = 1u << static_cast<unsigned>(EventKind::HostSessionListChanged);
    (void)session->takeEventFlags();

    const auto identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    const Ubiquitty::Session::Layout::Workspace layout(identities);
    const Ubiquitty::Session::Identity::LabelBook labels;
    UbiquittyTest::StatedPane pane;
    pane.title.programTitle = "make all";
    pane.paneID = { .low = 1, .high = 7 };
    const std::array sources { Ubiquitty::Session::TreeWorkspaceSource { .id = 9, .name = "work", .layout = &layout } };
    const Ubiquitty::Session::SessionTree tree = Ubiquitty::Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels,
        [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID) { return pane.treeFacts(); });

    session->noteWatchedHostSessionTreeForTest(tree);
    CHECK_MSG((session->takeEventFlags() & listBit) != 0, "a watched tree was not published");
    CHECK(session->hostSessionTreeFreshness() == Freshness::Live);
    CHECK(session->hasHostAnsweredListing());
    CHECK(!session->hostOutline({}).rows.empty());

    session->noteWatchedHostSessionTreeForTest(tree);
    CHECK_MSG((session->takeEventFlags() & listBit) != 0, "the watch's second report was not published");
    CHECK(session->hostSessionTreeRevision() == 2);

    // An older empty listing can arrive after the watch's newer tree on
    // their separate connections. Neither its rows nor its completion owns
    // the live tree, even when the listing reported nothing at all.
    const Ubiquitty::Session::SessionTree oldListing;
    session->noteHostSessionTreeForTest(oldListing);
    session->noteHostListingAnsweredForTest();
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeFreshness() == Freshness::Live);
    CHECK(session->hostSessionTreeRevision() == 2);
    CHECK(!session->hostOutline({}).rows.empty());
    (void)session->takeEventFlags();

    session->noteHostWatchInterruptedForTest({});
    CHECK_MSG((session->takeEventFlags() & listBit) != 0, "losing the host was not published");
    CHECK_MSG(session->hostSessionTreeFreshness() == Freshness::Stale, "a tree nothing follows still reads as live");
    CHECK(!session->hostOutline({}).rows.empty());

    // Lost once is lost: a second drop changes nothing a reader draws.
    session->noteHostWatchInterruptedForTest({});
    CHECK((session->takeEventFlags() & listBit) == 0);

    // A listing's answer with no watch following is a listing's.
    session->noteHostSessionTreeForTest(tree);
    session->noteHostListingAnsweredForTest();
    CHECK(session->hostSessionTreeFreshness() == Freshness::Listed);

    releaseTerminalSession(session);
}

/// A burst of queries inside one drained chunk answers every one of them.
/// The reply buffer holds 256 bytes and a chunk parses under a single
/// processBytesUpToSyncClear call, so without the parser's reply-pressure
/// yield the burst overflows the buffer mid-call and the surplus replies
/// vanish — a host that probes (the 256-color palette walk, a DA loop)
/// blocks on answers that never come.
TEST(aQueryBurstInsideOneChunkAnswersEveryQuery)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(rt);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    constexpr int queryCount = 200; // ~40 reply bytes each: ~30x the buffer
    std::string burst;
    for (int i = 0; i < queryCount; ++i)
        burst += "\033[c"; // primary DA
    pipe->toRead = burst;
    const DrainOutcome out = rt.drainAvailable(host);
    CHECK_EQ(out.bytesDrained, static_cast<int>(burst.size()));
    CHECK_EQ(host.locks, host.unlocks);
    int replies = 0;
    for (std::size_t at = pipe->written.find("\033[?"); at != std::string::npos; at = pipe->written.find("\033[?", at + 1))
        ++replies;
    CHECK_EQ(replies, queryCount);
    rt.close();
}

// The DECUDK table rides beside the snapshot, and the runtime is what keeps the
// two in step: it publishes the table before any snapshot that counts it. A
// programmed key must resolve through the link alone, the way the session and
// the Swift arm encode every press.
TEST(aProgrammedKeyResolvesOffTheLinkTheRuntimePublishes)
{
    using namespace Ubiquitty::Runtime;

    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);

    // DECUDK, clearing nothing and locking nothing: F6 (DEC key 17) becomes "xyz".
    stream->toRead.append("\033P1;1|17/78797A\033\\");
    CountingDrainHost host;
    (void)runtime.drainAvailable(host);

    KeyPress press;
    press.virtualKey = Ubiquitty::Keys::VirtualKey::F6;
    press.eventType = Ubiquitty::Keys::KeyEventType::Press;
    char buffer[32] = { 0 };
    const KeyEncoding encoding = encodeKey(runtime.link(), press, Ubiquitty::Keys::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) });
    CHECK_MSG(encoding.length == 3 && std::memcmp(buffer, "xyz", 3) == 0,
        "the programmed string, not F6's default sequence");
}

TEST(saturatedPassPublishesCellsWhileTheNextEscapeSequenceIsIncomplete)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);
    const std::string prefix = "\x1b[2J\x1b[Hvisible";
    stream->toRead = prefix;
    stream->toRead.append(TerminalRuntime::drainPassCapacityBytes - prefix.size() - 3, '\0');
    stream->toRead += "\x1b[3";
    stream->toRead += "1mred";
    CountingDrainHost host;
    const auto outcome = runtime.drainAvailable(host);
    REQUIRE(outcome.bytesDrained == TerminalRuntime::drainPassCapacityBytes);
    REQUIRE(runtime.isDrainSaturated());
    CHECK(host.frames == 1);
    const auto settled = runtime.drainAvailable(host);
    REQUIRE(settled.caughtUp);
    settleHeldFrame(runtime, host, clock);
    CHECK(host.frames == 2);
}

TEST(saturatedPassNeverPublishesInsideSynchronizedOutput)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);
    const std::string prefix = "\x1b[?2026h\x1b[2Jpartial";
    stream->toRead = prefix;
    stream->toRead.append(TerminalRuntime::drainPassCapacityBytes - prefix.size() - 3, '\0');
    stream->toRead += "\x1b[3";
    stream->toRead += "1mred";
    CountingDrainHost host;
    const auto outcome = runtime.drainAvailable(host);
    REQUIRE(outcome.bytesDrained == TerminalRuntime::drainPassCapacityBytes);
    REQUIRE(runtime.isDrainSaturated());
    CHECK(host.frames == 0);
    runtime.drainAvailable(host);
    CHECK(host.frames == 0);
}

// A DrainOutcome carries modes to a frontend that caches them — the GTK shell —
// and resolves its keys against the link's table. The count in those modes is
// sampled fresh every pass, while a snapshot is published only when one is
// due, so the table has to go out on the passes that publish no snapshot too.
// A DECUDK program at the head of a burst longer than one pass is the case
// that separates the two.
TEST(aDrainOutcomeNeverCountsKeysTheLinkHasNotPublished)
{
    using namespace Ubiquitty::Runtime;

    // Well inside the sync-publish floor, so nothing but a settled stream could
    // make the snapshot due.
    Ubiquitty::PAL::s_fakeMicroseconds = 1;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);

    stream->toRead.append("\033P1;1|17/78797A\033\\");
    stream->toRead.append(std::string(static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes) + 4096, 'x'));
    CountingDrainHost host;
    const DrainOutcome outcome = runtime.drainAvailable(host);

    // The precondition, asserted: the pass yielded on capacity with input still
    // pending, so it took the branch that hands out modes without a snapshot.
    REQUIRE_MSG(!outcome.caughtUp && !outcome.reachedEnd && outcome.bytesDrained >= TerminalRuntime::drainPassCapacityBytes,
        "the pass must yield on capacity for the case to reach the branch it tests");
    REQUIRE_MSG(runtime.link().reads.read().modes.userDefinedKeyCount == 0,
        "and the snapshot must not have been due, or the case tests the other branch");
    CHECK(outcome.modes.userDefinedKeyCount == 1);
    CHECK_MSG(runtime.link().userDefinedKeys.read().count >= outcome.modes.userDefinedKeyCount,
        "the table the link publishes is at least as new as the count the outcome carries");
}

// The tmux render-only pane's Terminal is written past the drain, so its
// snapshot is republished by hand — and the same rule holds there: a DECUDK
// program on that Terminal reaches the link's table before the snapshot that
// counts it, or a key pressed in that pane resolves against nothing.
TEST(aRepublishedExternalTerminalPublishesItsTableFirst)
{
    using namespace Ubiquitty::Runtime;

    TerminalRuntime runtime;
    runtime.init(parameters80x24());

    auto external = makeBlinkTerminal(24, 80);
    Ubiquitty::processBytes(external.get(), "\033P1;1|17/78797A\033\\");
    runtime.republishSnapshotForTerm(external.get());

    CHECK(runtime.link().reads.read().modes.userDefinedKeyCount == 1);
    CHECK_MSG(runtime.link().userDefinedKeys.read().count == 1,
        "the external terminal's table rode ahead of its snapshot");

    // And a key pressed in that pane resolves through the link like any other.
    KeyPress press;
    press.virtualKey = Ubiquitty::Keys::VirtualKey::F6;
    press.eventType = Ubiquitty::Keys::KeyEventType::Press;
    char buffer[32] = { 0 };
    const KeyEncoding encoding = encodeKey(runtime.link(), press, Ubiquitty::Keys::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) });
    CHECK(encoding.length == 3 && std::memcmp(buffer, "xyz", 3) == 0);
}

// The reported defect: a pane kept open after `exit` went on calling itself
// "-zsh" and went on blinking a caret, both of which describe a process that
// had left. End-of-file is the moment that becomes knowable, and it is the
// moment Core must act on — there is no later walk, because the ingest loop
// ends with the stream.
TEST(aSessionThatReachesEndOfFileStopsNamingItsProcessAndStopsOfferingACaret)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* stream = attachPipe(runtime);
    REQUIRE(stream != nullptr);

    // A live session first, so the assertions below have something to move
    // from. Without this the test would pass against a runtime that never set
    // any of it in the first place.
    CountingDrainHost host;
    stream->toRead.append("hello");
    (void)runtime.drainAvailable(host);
    REQUIRE(!runtime.sessionEnd().didEnd);
    Ubiquitty::Terminal* term = runtime.borrowTermForRender();
    REQUIRE(term != nullptr);
    CHECK_MSG(term->modes.isCursorVisible, "a running session offers a caret");

    // Now the peer closes.
    stream->eofPending = true;
    const DrainOutcome outcome = runtime.drainAvailable(host);

    CHECK_MSG(outcome.reachedEnd, "the drain reports the end it was given");
    CHECK_MSG(runtime.sessionEnd().didEnd, "the end is latched, not merely passed through the outcome");
    CHECK_MSG(runtime.processChain().empty(), "the chain stops naming a process that has gone");
    CHECK_MSG(runtime.shellCommandName().empty(), "the shell segment goes with it, being the same process");
    CHECK_MSG(!term->modes.isCursorVisible, "the caret goes: there is nothing left to type into");
    // Said in the terminal rather than in the title, so it reads as the last
    // line of a session instead of as the name of a program.
    bool didSayItEnded = false;
    for (int row = 0; row < term->rows && !didSayItEnded; ++row) {
        std::string line;
        for (int column = 0; column < term->columns; ++column) {
            const Ubiquitty::Cell* cell = term->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            line.push_back(cell != nullptr && cell->codePoint != 0 ? static_cast<char>(cell->codePoint) : ' ');
        }
        didSayItEnded = line.find("[Process exited]") != std::string::npos;
    }
    CHECK_MSG(didSayItEnded, "the grid carries the notice that the session ended");
    // The push. Core must not wait for a poll to tell a frontend, because the
    // ingest loop returns as soon as this outcome is delivered.
    CHECK_MSG(outcome.processStateChanged, "the same delivery that reports the end pushes the title change");
}

// The subtree the title's chain was formatted from is kept, so a second reader
// answers from the same walk rather than running its own.
//
// Two readers each walking is not merely twice the cost: they sample at
// different instants, so an inspector listing the subtree can disagree with the
// title about what is running. And a reader with no walk of its own has no edge
// to wake it either, which is what leaves a timer in place.
TEST(hiddenProcessStateDefersDiscoveryUntilReveal)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    runtime.setForegroundObservation(ForegroundObservation::Unread);
    pipe->scriptedForegroundName = "vim";
    pipe->foregroundGroup = 4243;
    pipe->toRead = "hidden output";
    CountingDrainHost host;
    (void)runtime.drainAvailable(host);
    CHECK(pipe->readPosition == pipe->toRead.size());
    CHECK(pipe->gatherProcessesCalls == 0);
    CHECK(pipe->foregroundGroupCalls == 0);
    CHECK(!runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK(!pipe->notifyTreeEdge());
    CHECK(!runtime.hasPendingProcessTreeEdge());
    CHECK(runtime.walkFallbackMilliseconds() == 0);

    pipe->scriptedForegroundName = "less";
    pipe->foregroundGroup = 4244;
    Ubiquitty::PAL::Process::Record child;
    child.processID = 4244;
    child.name = "less";
    child.commandLine = "less notes.txt";
    child.isForeground = true;
    pipe->scriptedRecords = { child };
    runtime.setForegroundObservation(ForegroundObservation::ProcessState);
    CHECK(pipe->notifyTreeEdge());
    CHECK(runtime.hasPendingProcessTreeEdge());
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK(pipe->gatherProcessesCalls == 1);
    CHECK_STR_EQ(runtime.processRecords()[0].name, "less");
    CHECK(!runtime.hasPendingProcessTreeEdge());

    runtime.setForegroundObservation(ForegroundObservation::Unread);
    pipe->toRead += "more output";
    (void)runtime.drainAvailable(host);
    CHECK(pipe->gatherProcessesCalls == 1);
    runtime.setForegroundObservation(ForegroundObservation::ProcessState);
    (void)runtime.sampleProcessStateOffDrain(host).processStateChanged;
    CHECK(pipe->gatherProcessesCalls == 2);
}

// An edge that lands while a probe reads the foreground is not answered by that
// probe, so the probe must leave it owed: it cannot clear the flag after the
// read.
TEST(anEdgeLandingDuringTheOffDrainProbeStaysOwed)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    runtime.setForegroundObservation(ForegroundObservation::TitleAndDirectory);
    CountingDrainHost host;
    pipe->foregroundGroup = 100;
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK(!runtime.hasPendingProcessTreeEdge());

    pipe->foregroundGroup = 200;
    pipe->edgesAfterGroupRead = 1;
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(pipe->edgesAfterGroupRead == 0, "the seam landed its edge during the probe");
    CHECK_MSG(runtime.hasPendingProcessTreeEdge(), "the edge that landed after the read is still owed");
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(!runtime.hasPendingProcessTreeEdge(), "the next probe answers it");
}

TEST(anEdgeLandingDuringTheDrainProbeStaysOwed)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    runtime.setForegroundObservation(ForegroundObservation::TitleAndDirectory);
    CountingDrainHost host;
    pipe->foregroundGroup = 100;
    pipe->edgesAfterGroupRead = 1;
    pipe->toRead = "output";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->edgesAfterGroupRead == 0, "the seam landed its edge during the probe");
    CHECK_MSG(runtime.hasPendingProcessTreeEdge(), "the edge that landed after the read is still owed");
}

// A reader of the title and directory alone keeps both current without the
// walk: the probe of the terminal's holder stamps a title, retires it once its
// program exits and samples the directory; an edge is answered once; and the
// walk runs only once process state has a reader.
TEST(aTitleAndDirectoryReaderProbesTheHolderWithoutWalking)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    runtime.setForegroundObservation(ForegroundObservation::TitleAndDirectory);
    CountingDrainHost host;
    pipe->scriptedForegroundName = "vim";
    pipe->foregroundGroup = 100;
    pipe->foregroundDirectory = "/editing";
    pipe->toRead = "\x1b]2;Editor title\x07";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->foregroundGroupCalls > 0, "the drain names who holds the terminal");
    CHECK_STR_EQ(runtime.borrowTermForRender()->currentDirectory, "/editing");

    pipe->foregroundGroup = 200;
    pipe->foregroundDirectory = "/next";
    const DrainOutcome exited = runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(exited.titleChanged, "the title retires with its program");
    CHECK(exited.currentDirectoryChanged);
    CHECK_STR_EQ(runtime.borrowTermForRender()->currentDirectory, "/next");
    CHECK(!exited.processStateChanged);
    CHECK_MSG(pipe->gatherProcessesCalls == 0, "nothing reads the chain");
    CHECK(runtime.processRecords().empty());

    CHECK(!runtime.hasPendingProcessTreeEdge());
    CHECK(pipe->notifyTreeEdge());
    CHECK(runtime.hasPendingProcessTreeEdge());
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(!runtime.hasPendingProcessTreeEdge(), "the probe answers the edge without a walk");
    CHECK(pipe->gatherProcessesCalls == 0);

    pipe->scriptedForegroundName = "less";
    Ubiquitty::PAL::Process::Record child;
    child.processID = 200;
    child.name = "less";
    child.commandLine = "less notes.txt";
    child.isForeground = true;
    pipe->scriptedRecords = { child };
    runtime.setForegroundObservation(ForegroundObservation::ProcessState);
    CHECK_MSG(runtime.hasPendingProcessTreeEdge(), "a reader of process state is owed the walk");
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK(pipe->gatherProcessesCalls == 1);
    CHECK_STR_EQ(runtime.processRecords()[0].name, "less");

    runtime.setForegroundObservation(ForegroundObservation::Unread);
    const int probesBeforeUnread = pipe->foregroundGroupCalls;
    pipe->toRead += "unread output";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->foregroundGroupCalls == probesBeforeUnread, "an unread foreground is not probed");
    CHECK(!pipe->notifyTreeEdge());
}

TEST(refillGapsWithinOneForegroundGroupDoNotRepeatTheProcessWalk)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    Ubiquitty::PAL::Process::Record shell;
    shell.processID = 4242;
    shell.processGroupID = 4242;
    shell.name = "zsh";
    shell.isSessionLeader = true;
    shell.isForeground = true;
    auto child = shell;
    child.processID = 4243;
    child.parentProcessID = 4242;
    child.name = "writer";
    child.isSessionLeader = false;
    pipe->scriptedRecords = { shell, child };
    pipe->scriptedForegroundName = "writer";
    pipe->foregroundGroup = 4242;
    CountingDrainHost host;

    setenv("UBIQUITTY_FAKE_TICKS", "100", 1);
    pipe->toRead = "first";
    (void)runtime.drainAvailable(host);
    CHECK(pipe->gatherProcessesCalls == 1);
    for (int burst = 0; burst < 20; ++burst) {
        pipe->toRead += "x";
        CHECK(runtime.drainAvailable(host).caughtUp);
    }
    CHECK_MSG(pipe->gatherProcessesCalls == 1, "refill gaps do not imply a changed process tree");

    setenv("UBIQUITTY_FAKE_TICKS", "103", 1);
    pipe->toRead += "same group, later";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->gatherProcessesCalls == 1, "a foreground probe does not require another full-table walk");
    setenv("UBIQUITTY_FAKE_TICKS", "160", 1);
    pipe->toRead += "poll the unchanged foreground";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->gatherProcessesCalls == 2, "the fallback cadence still discovers changes within one group");

    pipe->foregroundGroup = 5000;
    pipe->toRead += "new foreground job";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->gatherProcessesCalls == 3, "a job transition is sampled without waiting for the budget");
    unsetenv("UBIQUITTY_FAKE_TICKS");
}

TEST(aFrontendSamplesQuietProcessFactsThroughTheSessionCache)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* pipe = stream.get();
    pipe->scriptedForegroundName = "writer";
    pipe->foregroundGroup = 4242;
    pipe->foregroundDirectory = "/sampled";
    Ubiquitty::PAL::Process::Record child;
    child.processID = 4242;
    child.processGroupID = 4242;
    child.name = "writer";
    child.commandLine = "writer --quiet";
    child.isSessionLeader = true;
    child.isForeground = true;
    pipe->scriptedRecords = { child };
    CHECK(session->adoptStream(std::move(stream)).has_value());
    (void)session->sampleProcessStateOffDrain();
    CHECK(session->processChain().find("writer") != std::string::npos);
    CHECK(session->currentDirectory() == "/sampled");
    const int walks = pipe->gatherProcessesCalls;
    for (int frame = 0; frame < 120; ++frame) {
        (void)session->processChain();
        (void)session->processChainWithArguments();
        (void)session->currentDirectory();
    }
    CHECK(pipe->gatherProcessesCalls == walks);
    pipe->foregroundGroup = 5000;
    pipe->foregroundDirectory = "/moved";
    (void)session->sampleProcessStateOffDrain();
    CHECK(session->currentDirectory() == "/moved");
    CHECK(pipe->gatherProcessesCalls == walks + 1);
    pipe->eofPending = true;
    (void)session->drain();
    const int endedWalks = pipe->gatherProcessesCalls;
    (void)session->sampleProcessStateOffDrain();
    CHECK(pipe->gatherProcessesCalls == endedWalks);
    CHECK(session->processChain().empty());
    releaseTerminalSession(session);
}
TEST(endOfFileDoesNotResampleAndRestoreTheExitedProcess)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    Ubiquitty::PAL::Process::Record child;
    child.processID = 4242;
    child.processGroupID = 4242;
    child.name = "editor";
    child.isSessionLeader = true;
    child.isForeground = true;
    pipe->scriptedRecords = { child };
    pipe->scriptedForegroundName = "editor";
    pipe->foregroundGroup = 4242;
    pipe->foregroundShell = false;
    pipe->toRead = "\x1b[?1000h";
    CountingDrainHost host;
    (void)runtime.drainAvailable(host);
    CHECK(!runtime.processChain().empty());
    const int walks = pipe->gatherProcessesCalls;

    // A process can still be present in the table after its last slave closes.
    pipe->eofPending = true;
    const auto outcome = runtime.drainAvailable(host);
    CHECK(outcome.reachedEnd);
    CHECK(outcome.processStateChanged);
    CHECK(runtime.processChain().empty());
    CHECK(runtime.shellCommandName().empty());
    CHECK(pipe->gatherProcessesCalls == walks);
}

TEST(theWalkThatBuildsTheChainKeepsWhatItFound)
{
    using namespace Ubiquitty::Runtime;
    namespace Process = Ubiquitty::PAL::Process;

    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);

    Process::Record shell;
    shell.processID = 4242;
    shell.name = "zsh";
    shell.commandLine = "-zsh";
    shell.isSessionLeader = true;
    shell.tty = "ttys012";
    Process::Record child;
    child.processID = 4243;
    child.parentProcessID = 4242;
    child.name = "vim";
    child.commandLine = "vim notes.txt";
    child.isForeground = true;
    child.tty = "ttys012";
    pipe->scriptedRecords = { shell, child };
    pipe->foregroundGroup = 4243;

    CountingDrainHost host;
    (void)runtime.sampleProcessStateOffDrain(host).processStateChanged;

    const auto records = runtime.processRecords();
    CHECK_MSG(records.size() == 2, "both processes the walk found are kept, not just the leaf");
    CHECK_STR_EQ(records[0].name, "zsh");
    CHECK_STR_EQ(records[1].name, "vim");
    // The arguments come with them: an inspector lists the command line, and a
    // reader that had to walk again to get it would be walking again.
    CHECK_STR_EQ(records[1].commandLine, "vim notes.txt");

    // The kept copy is the reader's own. The walk rewrites its vector from the
    // ingest thread, so a reader holding a reference into it would be reading
    // storage being replaced.
    const auto second = runtime.processRecords();
    CHECK(second.data() != records.data());

    // A walk that comes back empty leaves the subtree standing, for the same
    // reason it leaves the chain standing: a transient failure to read the
    // process table is not the session ending, and blanking on it would make an
    // inspector flicker between a list and nothing.
    pipe->scriptedRecords.clear();
    (void)runtime.sampleProcessStateOffDrain(host).processStateChanged;
    CHECK_MSG(runtime.processRecords().size() == 2, "an empty walk retires nothing");
}

// Quitting Midnight Commander hands the terminal back to the shell, and the
// shell's first act at its prompt is to run its prompt hooks -- several of them
// as foreground jobs that live for a millisecond. The drain that reads the
// start of the prompt samples the shell, sees mc gone and announces the move;
// the close button's marker is redrawn on that announcement, a hop later. Read
// live, that redraw can land inside a hook and find a job. The drain after the
// hook samples the shell again, which is no move at all, so nothing redraws the
// marker and it keeps saying a job is running at an idle prompt.
TEST(theRunningJobMarkerAnswersForTheSampleThatRedrawsIt)
{
    namespace Process = Ubiquitty::PAL::Process;
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* pipe = stream.get();
    pipe->runningChild = true;
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    Process::Record shell;
    shell.processID = 500;
    shell.processGroupID = 500;
    shell.name = "-zsh";
    shell.isSessionLeader = true;
    const auto atThePrompt = [&] {
        shell.isForeground = true;
        pipe->scriptedRecords = { shell };
        pipe->foregroundGroup = 500;
        pipe->foregroundShell = true;
        pipe->scriptedForegroundName = "zsh";
    };
    atThePrompt();
    pipe->toRead = "% ";
    (void)session->drain();
    CHECK_MSG(!session->needsRunningJobMarker(), "a shell at its prompt is not a running job");

    // mc holds the terminal, with the subshell it runs on a terminal of its own.
    Process::Record commander;
    commander.processID = 600;
    commander.parentProcessID = 500;
    commander.processGroupID = 600;
    commander.name = "mc";
    commander.isForeground = true;
    Process::Record subshell;
    subshell.processID = 601;
    subshell.parentProcessID = 600;
    subshell.processGroupID = 601;
    subshell.name = "zsh";
    shell.isForeground = false;
    pipe->scriptedRecords = { shell, commander, subshell };
    pipe->foregroundGroup = 600;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "mc";
    pipe->toRead += "\x1b[?1049h";
    CHECK(session->drain().processStateChanged);
    CHECK_MSG(session->needsRunningJobMarker(), "mc in the foreground is a running job");

    // mc leaves the alternate screen and exits; the shell takes the terminal
    // back and writes the start of its prompt.
    pipe->toRead += "\x1b[?1049l";
    (void)session->drain();
    atThePrompt();
    pipe->toRead += "\r \r";
    CHECK_MSG(session->drain().processStateChanged, "mc leaving is announced");

    // The marker's redraw for that announcement lands while a prompt hook
    // holds the terminal.
    pipe->foregroundGroup = 700;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "zsh";
    CHECK_MSG(!session->needsRunningJobMarker(), "the marker answers for the sample its redraw was announced by");

    // The hook ends and the prompt is written. The sample finds what the last
    // one found, so this is the answer the marker keeps.
    atThePrompt();
    pipe->toRead += "% ";
    CHECK(!session->drain().processStateChanged);
    CHECK_MSG(!session->needsRunningJobMarker(), "the prompt clears the marker");
    session->close();
    releaseTerminalSession(session);
}

// The marker reads whether the shell holds the terminal from the sample, so the
// sample must announce that fact moving even when no process came or went: a
// shell taking the terminal back from a group that already exited is exactly
// that.
TEST(theShellTakingTheTerminalBackIsAProcessStateMove)
{
    using namespace Ubiquitty::Runtime;
    namespace Process = Ubiquitty::PAL::Process;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    Process::Record shell;
    shell.processID = 500;
    shell.processGroupID = 500;
    shell.name = "-zsh";
    shell.isSessionLeader = true;
    pipe->scriptedRecords = { shell };

    // The job's group outlives its last process until the shell reclaims.
    pipe->foregroundGroup = 600;
    pipe->foregroundShell = false;
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK(!runtime.isSampledForegroundShell());

    pipe->foregroundGroup = 500;
    pipe->foregroundShell = true;
    CHECK_MSG(runtime.sampleProcessStateOffDrain(host).processStateChanged, "the shell reclaiming is announced");
    CHECK(runtime.isSampledForegroundShell());
    CHECK_MSG(!runtime.sampleProcessStateOffDrain(host).processStateChanged, "a sample that finds the same owner is quiet");
}

// A program's catalog key is decided by who holds the terminal and named by the
// foreground process, so a published name and holder must come from one
// foreground. The probe reads the group, then the holder and its name; a
// foreground that moves before the reading after it is read again until two
// readings find the same group, holder and name.
TEST(aSampleKeepsANameOnlyWithTheGroupAndHolderReadAroundIt)
{
    using namespace Ubiquitty::Runtime;
    using ForegroundMove = Ubiquitty::PAL::StreamTestPipe::ForegroundMove;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;

    pipe->foregroundGroup = 600;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "claude";
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName() == "claude");
    REQUIRE(!runtime.isSampledForegroundShell());

    // ^C: the probe still sees the program, and the shell has the terminal
    // back by the time the name is read.
    pipe->scriptedForegroundName = "bash";
    pipe->movesAsNamesAreRead = { ForegroundMove { .group = 500, .isShell = true, .name = "bash" } };
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "bash");
    CHECK_MSG(runtime.isSampledForegroundShell(), "the shell's name is published with the shell as its holder");
    CHECK_EQ(runtime.foregroundProcessGroup(), 500u);

    // A sample that finds the published name and holder reads them once.
    const int shellReads = pipe->foregroundShellCalls;
    CHECK_MSG(!runtime.sampleProcessStateOffDrain(host).processStateChanged, "the next sample finds the same foreground");
    CHECK_EQ(pipe->foregroundShellCalls - shellReads, 1);

    // The reverse edge: the probe finds the shell, and a program has the
    // terminal by the time the name is read.
    pipe->scriptedForegroundName = "codex";
    pipe->movesAsNamesAreRead = { ForegroundMove { .group = 700, .isShell = false, .name = "codex" } };
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "codex");
    CHECK_MSG(!runtime.isSampledForegroundShell(), "the program's name is published with the program as its holder");
    CHECK_EQ(runtime.foregroundProcessGroup(), 700u);

    // One job ends and the next begins between the probe and the name read:
    // both reads of the holder see a program, and the name is the shell's,
    // read in the moment between them. The group read after the name is the
    // second job's, so the name is read again.
    pipe->scriptedForegroundName = "claude";
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName() == "claude");
    pipe->foregroundGroup = 800;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "bash";
    pipe->movesAsNamesAreRead = { ForegroundMove { .group = 801, .isShell = false, .name = "claude" } };
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "claude");
    CHECK(!runtime.isSampledForegroundShell());
    CHECK_EQ(runtime.foregroundProcessGroup(), 801u);
}

// A program that execs another in its place moves the name alone, the group
// and the holder standing still. That is a process-state move: a reader hears
// the new name.
TEST(aNameThatMovesAloneIsAProcessStateMove)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;

    pipe->foregroundGroup = 600;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "make";
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName() == "make");

    pipe->scriptedForegroundName = "cc";
    CHECK_MSG(runtime.sampleProcessStateOffDrain(host).processStateChanged, "the new name is announced");
    CHECK_STR_EQ(runtime.foregroundProcessName(), "cc");
    CHECK(!runtime.isSampledForegroundShell());
    CHECK_MSG(!runtime.sampleProcessStateOffDrain(host).processStateChanged, "a sample that finds the same name is quiet");
}

// A program that execs in the foreground reads as having exec'd a moment before
// the kernel gives it its own name, and in that moment carries its parent's: a
// reading pairs a job's hold with the shell's name. The group and the holder
// stand still across it, so only the name tells it from the reading after it,
// and a reading is settled only when the next one finds the same name.
TEST(aNameTheNextReadingDoesNotFindIsReadAgain)
{
    using namespace Ubiquitty::Runtime;
    using ForegroundMove = Ubiquitty::PAL::StreamTestPipe::ForegroundMove;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;

    pipe->foregroundGroup = 500;
    pipe->foregroundShell = true;
    pipe->scriptedForegroundName = "bash";
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName() == "bash");

    pipe->foregroundGroup = 700;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "bash";
    pipe->movesAsNamesAreRead = { ForegroundMove { .group = 700, .isShell = false, .name = "sleep" } };
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "sleep");
    CHECK(!runtime.isSampledForegroundShell());
    CHECK_EQ(runtime.foregroundProcessGroup(), 700u);
}

// A reading whose name did not read has nothing to settle against: the member
// that named it left before its name was read. It is published unsettled, and
// the next sample reads it again rather than taking it as standing.
TEST(aReadingWithNoNameIsNotSettled)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;

    pipe->foregroundGroup = 700;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = {};
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName().empty());
    const int shellReads = pipe->foregroundShellCalls;
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_EQ(pipe->foregroundShellCalls - shellReads, 2);
}

// A foreground that moves on every read is read again a bounded number of
// times, and the reading it publishes unsettled is read again by the next
// sample even when that sample finds the same name and holder.
TEST(aForegroundStillMovingIsPublishedUnsettledAndReadAgain)
{
    using namespace Ubiquitty::Runtime;
    using ForegroundMove = Ubiquitty::PAL::StreamTestPipe::ForegroundMove;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;

    pipe->foregroundGroup = 600;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "claude";
    for (int move = 0; move < 8; ++move)
        pipe->movesAsNamesAreRead.push_back(ForegroundMove { .group = 601 + move, .isShell = false, .name = "claude" });
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "claude");
    CHECK_MSG(pipe->movesAsNamesAreRead.size() == 4, "one name read, then three more, and no further");

    // Still the same name and holder, but the reading was never settled: the
    // next sample reads the group and holder again after its name.
    pipe->movesAsNamesAreRead.clear();
    const int shellReads = pipe->foregroundShellCalls;
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_EQ(pipe->foregroundShellCalls - shellReads, 2);
    CHECK_EQ(runtime.foregroundProcessGroup(), 604u);
}

// A job still moving whose last reading named nothing is no news about its
// name: publishing it would leave a program with no name, which a reader
// resolves to the session's launch name. The name last published stands, and
// the next reading that has a name replaces it. The holder is not held: a job
// that took the terminal is a job whatever its name reads, so the pane is not
// at its prompt, the running-job marker is up, and the size is re-sent to it
// through its first second, the named sample after it included.
TEST(aMovingForegroundThatNamesNothingKeepsThePublishedNameAndTakesTheHolderRead)
{
    using namespace Ubiquitty::Runtime;
    using ForegroundMove = Ubiquitty::PAL::StreamTestPipe::ForegroundMove;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    runtime.setWindowSize(100, 30, 800, 600);

    pipe->foregroundGroup = 500;
    pipe->foregroundShell = true;
    pipe->scriptedForegroundName = "bash";
    (void)runtime.sampleProcessStateOffDrain(host);
    REQUIRE(runtime.foregroundProcessName() == "bash");
    REQUIRE(runtime.isSampledForegroundShell());
    CHECK_EQ(pipe->reassertCount, 0);

    // A job takes the terminal and every reading of it finds another group
    // with no name, past the rereads that would have settled it.
    pipe->foregroundGroup = 700;
    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = {};
    for (int move = 0; move < 8; ++move)
        pipe->movesAsNamesAreRead.push_back(ForegroundMove { .group = 701 + move, .isShell = false, .name = {} });
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "bash");
    CHECK_MSG(!runtime.isSampledForegroundShell(), "the job holds the terminal whatever its name reads");
    CHECK_MSG(pipe->reassertCount == 1, "the takeover is re-sent its size with the sample that saw it");
    const ForegroundProgram held = runtime.foregroundProgram(0);
    CHECK_STR_EQ(held.name, "bash");

    // The next reading has a name and publishes it with its holder; the group
    // it names is the one already recorded, and its window is not reopened.
    pipe->movesAsNamesAreRead.clear();
    pipe->foregroundGroup = 704;
    pipe->scriptedForegroundName = "sleep";
    CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
    CHECK_STR_EQ(runtime.foregroundProcessName(), "sleep");
    CHECK(!runtime.isSampledForegroundShell());
}

// A fork the shell made reads as the shell's until it execs. The job it
// becomes holds the same group, and it is the takeover: the window opens when
// the holder turns into a job, not only when the group changes.
TEST(aGroupReadAsTheShellsThenAsAJobIsATakeover)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    runtime.setWindowSize(100, 30, 800, 600);

    pipe->foregroundGroup = 500;
    pipe->foregroundShell = true;
    pipe->scriptedForegroundName = "bash";
    (void)runtime.sampleProcessStateOffDrain(host);

    pipe->foregroundGroup = 700;
    pipe->foregroundShell = true;
    pipe->scriptedForegroundName = "bash";
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(pipe->reassertCount == 0, "a fork that has not exec'd runs the shell's code");

    pipe->foregroundShell = false;
    pipe->scriptedForegroundName = "vim";
    (void)runtime.sampleProcessStateOffDrain(host);
    CHECK_MSG(pipe->reassertCount == 1, "the job it became is re-sent its size");
}

// A reported host names where the shell that sent it is, and outranks the walk
// while it stands, because it is the only source that sees past a nested ssh.
// So it must stand only while that shell holds the terminal: the drain probe
// that retires the same report's directory retires its host, and publishes the
// retirement where a window tag reads it.
TEST(aReportedHostDoesNotOutliveTheProgramThatReportedIt)
{
    using namespace Ubiquitty::Runtime;
    namespace Process = Ubiquitty::PAL::Process;

    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    // Every foreground program is sampled in the directory the shell reports,
    // as an ssh run from that directory is. Retiring the directory report then
    // leaves the pane's directory where it was and raises no edge of its own,
    // so the host's retirement must raise the edge that publishes it.
    pipe->foregroundDirectory = "/home/user";

    // The local shell reports where it is.
    pipe->foregroundGroup = 100;
    pipe->toRead = "\x1b]7;file://example-host/home/user\x07";
    const DrainOutcome reported = runtime.drainAvailable(host);
    CHECK(reported.currentDirectoryChanged);
    CHECK_STR_EQ(runtime.reportedHost(), "example-host");

    // More output from the same program leaves the report standing.
    pipe->toRead += "$ ";
    (void)runtime.drainAvailable(host);
    CHECK_STR_EQ(runtime.reportedHost(), "example-host");

    // ssh to a machine with no integration takes the terminal.
    Process::Record shell;
    shell.processID = 4242;
    shell.name = "zsh";
    shell.commandLine = "-zsh";
    shell.isSessionLeader = true;
    shell.tty = "ttys012";
    Process::Record client;
    client.processID = 4243;
    client.parentProcessID = 4242;
    client.name = "ssh";
    client.commandLine = "ssh build-1";
    client.isForeground = true;
    client.tty = "ttys012";
    pipe->scriptedRecords = { shell, client };
    pipe->foregroundGroup = 200;
    pipe->toRead += "Welcome to build-1\r\n$ ";
    const DrainOutcome connected = runtime.drainAvailable(host);
    CHECK_MSG(runtime.reportedHost().empty(), "a report does not outlive its program into another connection");
    CHECK_MSG(connected.currentDirectoryChanged, "the retirement goes out on the edge that carried the report");
    const Ubiquitty::Session::SessionLocation location = Ubiquitty::Session::resolveSessionLocation(
        { .target = runtime.remoteTarget(), .reportedHost = runtime.reportedHost() });
    CHECK_MSG(location.name == "build-1", "the session is where the ssh went, not where its shell reported from");

    // A shell on the far side reports, and the connection closes under it.
    pipe->toRead += "\x1b]7;file://build-1/home/user\x07";
    (void)runtime.drainAvailable(host);
    CHECK_STR_EQ(runtime.reportedHost(), "build-1");
    pipe->foregroundGroup = 300;
    pipe->toRead += "Connection to build-1 closed.\r\n$ ";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(runtime.reportedHost().empty(), "a remote shell's report leaves with that shell");
}

// An ssh is sampled in the directory it was run from, and with nothing
// reported that sample is the pane's directory. A title naming it puts a
// session on another machine in a folder here, and keeps it there however far
// the far side moves. The title names only what the far side reports, while a
// new tab still starts where the ssh was run.
TEST(aConnectionsTitleNamesOnlyWhatItsFarSideReports)
{
    namespace Process = Ubiquitty::PAL::Process;
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* pipe = stream.get();
    pipe->foregroundDirectory = "/Users/user/project";
    Process::Record shell;
    shell.processID = 4242;
    shell.processGroupID = 4242;
    shell.name = "zsh";
    shell.commandLine = "-zsh";
    shell.isSessionLeader = true;
    shell.isForeground = true;
    shell.tty = "ttys012";
    pipe->scriptedRecords = { shell };
    pipe->foregroundGroup = 4242;
    CHECK(session->adoptStream(std::move(stream)).has_value());

    // The local shell reports where it is, and the title names it.
    pipe->toRead = "\x1b]7;file://localhost/Users/user/project\x07$ ";
    (void)session->drain();
    CHECK_STR_EQ(session->titleDirectory(), "/Users/user/project");

    // ssh to a machine with no integration takes the terminal.
    Process::Record client;
    client.processID = 4243;
    client.parentProcessID = 4242;
    client.processGroupID = 4243;
    client.name = "ssh";
    client.commandLine = "ssh build-1";
    client.isForeground = true;
    client.tty = "ttys012";
    shell.isForeground = false;
    pipe->scriptedRecords = { shell, client };
    pipe->foregroundGroup = 4243;
    pipe->toRead += "Welcome to build-1\r\n$ ";
    (void)session->drain();
    CHECK_MSG(session->titleDirectory().empty(), "a title does not name the folder the ssh was run from");
    CHECK_MSG(session->currentDirectory() == "/Users/user/project", "a tab opened beside it still starts where the ssh was run");

    // A shell on the far side reports, and the title names that.
    pipe->toRead += "\x1b]7;file://build-1/home/user\x07";
    (void)session->drain();
    CHECK_STR_EQ(session->titleDirectory(), "/home/user");

    // The connection closes, and the local folder is named again.
    shell.isForeground = true;
    pipe->scriptedRecords = { shell };
    pipe->foregroundGroup = 4242;
    pipe->toRead += "Connection to build-1 closed.\r\n$ ";
    (void)session->drain();
    CHECK_STR_EQ(session->titleDirectory(), "/Users/user/project");

    // A known case: the far side reports the very path the client was run
    // from, as between two machines that keep a user's home at one path. The
    // pane's directory does not move, so no directory edge goes out, and a
    // title that waits for one leaves the directory out until it is woven
    // again. Read again, it names the report: the title shows nothing in
    // between, never the folder the session left.
    pipe->foregroundDirectory = "/home/user";
    pipe->toRead += "\x1b]7;file://localhost/home/user\x07$ ";
    (void)session->drain();
    CHECK_STR_EQ(session->titleDirectory(), "/home/user");
    shell.isForeground = false;
    pipe->scriptedRecords = { shell, client };
    pipe->foregroundGroup = 4243;
    pipe->toRead += "Welcome to build-1\r\n$ ";
    (void)session->drain();
    CHECK_MSG(session->titleDirectory().empty(), "a second connection's title does not name the folder it was run from");
    pipe->toRead += "\x1b]7;file://build-1/home/user\x07";
    (void)session->drain();
    CHECK_STR_EQ(session->titleDirectory(), "/home/user");
    releaseTerminalSession(session);
}

TEST(aQuietForegroundChangeRetiresReportedMetadata)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    pipe->foregroundGroup = 100;
    pipe->foregroundDirectory = "/local";
    pipe->toRead = "\x1b]7;file://remote/home/user\x07\x1b]2;Remote editor\x07";
    (void)runtime.drainAvailable(host);
    CHECK_STR_EQ(runtime.reportedHost(), "remote");
    pipe->foregroundGroup = 200;
    pipe->foregroundDirectory = "/next";
    const auto outcome = runtime.sampleProcessStateOffDrain(host);
    CHECK(outcome.currentDirectoryChanged);
    CHECK(outcome.titleChanged);
    CHECK(runtime.reportedHost().empty());
    CHECK_STR_EQ(runtime.currentDirectory(), "/next");
    const auto repeated = runtime.sampleProcessStateOffDrain(host);
    CHECK(!repeated.currentDirectoryChanged);
    CHECK(!repeated.titleChanged);
}

TEST(aQuietShellReturnReleasesOnlyTheDepartedApplicationsPointerCapture)
{
    for (const std::string_view mode : { "\x1b[?1000h", "\x1b[?7403h", "\x1b[?1000;7403h" }) {
        TerminalRuntime runtime;
        runtime.init(parameters80x24());
        auto* pipe = attachPipe(runtime);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        pipe->foregroundGroup = 222;
        pipe->foregroundShell = false;
        pipe->toRead = std::string(mode) + "\x1b[?1016;7402h";
        (void)runtime.drainAvailable(host);
        const auto isCaptured = [&] {
            const auto snapshot = runtime.link().reads.read();
            return snapshot.mouseReportsActive || snapshot.mouseRelativePointerLockActive;
        };
        CHECK(isCaptured());
        (void)runtime.sampleProcessStateOffDrain(host);
        CHECK(isCaptured());
        pipe->foregroundGroup.reset();
        (void)runtime.sampleProcessStateOffDrain(host);
        CHECK(isCaptured());
        pipe->foregroundGroup = 333;
        (void)runtime.sampleProcessStateOffDrain(host);
        CHECK(isCaptured());
        pipe->foregroundGroup = 111;
        pipe->foregroundShell = true;
        CHECK(runtime.sampleProcessStateOffDrain(host).processStateChanged);
        CHECK(!isCaptured());
        CHECK(runtime.borrowTermForRender()->modes.mouseSGRPixels);
        CHECK(runtime.borrowTermForRender()->modes.fullscreenContent);
        pipe->toRead += mode;
        (void)runtime.drainAvailable(host);
        CHECK(isCaptured());
        (void)runtime.sampleProcessStateOffDrain(host);
        CHECK(isCaptured());
    }
}

TEST(aNullStreamAdoptionRefusesWithoutChangingTheInstalledOwner)
{
    const int destroyedBefore = Ubiquitty::PAL::g_streamDestroyCount;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    runtime.setWindowSize(80, 24, 640, 480);

    std::unique_ptr<Ubiquitty::PAL::Stream> absent;
    auto emptyRefusal = runtime.adoptStream(std::move(absent));
    REQUIRE(!emptyRefusal.has_value());
    CHECK(emptyRefusal.error() == nullptr);
    CHECK(absent == nullptr);
    CHECK(runtime.stream() == nullptr);
    CHECK(runtime.windowSizeDeliveryCount() == 0);
    CHECK(!runtime.hasPendingWindowSize());
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);

    auto* installed = attachPipe(runtime);
    REQUIRE(installed != nullptr);
    REQUIRE(installed->treeNotifierInstallations == 1);
    runtime.setWindowSize(80, 24, 800, 600);
    REQUIRE(runtime.hasPendingWindowSize());
    const auto deliveriesBefore = runtime.windowSizeDeliveryCount();

    auto installedRefusal = runtime.adoptStream(nullptr);
    REQUIRE(!installedRefusal.has_value());
    CHECK(installedRefusal.error() == nullptr);
    CHECK(runtime.stream() == installed);
    CHECK(installed->treeNotifierInstallations == 1);
    CHECK(installed->windowXpixel == 640);
    CHECK(installed->windowYpixel == 480);
    CHECK(runtime.windowSizeDeliveryCount() == deliveriesBefore);
    CHECK(runtime.hasPendingWindowSize());
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);
    constexpr std::string_view input = "still installed";
    runtime.sendInputBytes({ input.data(), input.size() });
    CHECK(installed->written == input);

    runtime.close();
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore + 1);
}

TEST(aNonnullStreamAdoptionReplacesTheOwnerAndRunsItsInstallationHooks)
{
    const int destroyedBefore = Ubiquitty::PAL::g_streamDestroyCount;
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    runtime.setWindowSize(80, 24, 640, 480);
    auto* original = attachPipe(runtime);
    REQUIRE(original != nullptr);
    CHECK(original->treeNotifierInstallations == 1);
    runtime.setWindowSize(80, 24, 800, 600);
    REQUIRE(runtime.hasPendingWindowSize());
    const auto deliveriesBefore = runtime.windowSizeDeliveryCount();

    auto replacement = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    const auto* expectedOwner = replacement.get();
    auto adoption = runtime.adoptStream(std::move(replacement));
    REQUIRE(adoption.has_value());
    auto* installed = static_cast<Ubiquitty::PAL::StreamTestPipe*>(runtime.stream());
    REQUIRE(installed != nullptr);
    CHECK(installed == expectedOwner);
    CHECK(replacement == nullptr);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore + 1);
    CHECK(installed->treeNotifierInstallations == 1);
    CHECK(installed->windowColumns == 80);
    CHECK(installed->windowRows == 24);
    CHECK(installed->windowXpixel == 800);
    CHECK(installed->windowYpixel == 600);
    CHECK(runtime.windowSizeDeliveryCount() == deliveriesBefore + 1);
    CHECK(!runtime.hasPendingWindowSize());
    constexpr std::string_view input = "replacement installed";
    runtime.sendInputBytes({ input.data(), input.size() });
    CHECK(installed->written == input);

    runtime.close();
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore + 2);
    runtime.close();
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore + 2);
}

TEST(aStreamAdoptedIntoASizedRuntimeLearnsItsGrid)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CHECK_MSG(s->windowColumns == 100 && s->windowRows == 30, "a PTY knows the grid it was adopted into");
    rt.close();
}

TEST(aWindowSizePublishedBeforeTheStreamReachesIt)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    rt.setWindowSize(100, 30, 800, 600);
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CHECK_MSG(s->windowXpixel == 800 && s->windowYpixel == 600, "the pixel size published before the spawn is replayed");
    CHECK(s->windowColumns == 100);
    CHECK(s->windowRows == 30);
    rt.close();
}

TEST(aGridChangeReachesTheStreamAtOnceAndPixelsCoalesce)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    // Adoption handed the stream its grid and stamped the interval. Two grid
    // changes inside the interval both reach the stream at once: a program's
    // redraw must land for the grid it was told.
    rt.setWindowSize(101, 30, 800, 600);
    CHECK_MSG(s->windowColumns == 101 && s->windowRows == 30, "a grid change is never held");
    rt.setWindowSize(102, 31, 810, 606);
    CHECK_MSG(s->windowColumns == 102 && s->windowRows == 31, "nor the next one");
    CHECK(!rt.hasPendingWindowSize());
    // A change of pixels alone inside the interval is held, and the newest
    // lands when the interval has passed.
    rt.setWindowSize(102, 31, 820, 612);
    rt.setWindowSize(102, 31, 830, 618);
    CHECK_MSG(rt.hasPendingWindowSize(), "a pixel-only size inside the interval is held");
    CHECK_MSG(s->windowXpixel == 810, "and not yet on the stream");
    rt.applyPendingWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros());
    CHECK_MSG(rt.hasPendingWindowSize(), "not due yet");
    rt.applyPendingWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros() + 100000);
    CHECK_MSG(s->windowXpixel == 830 && s->windowYpixel == 618, "the newest pixels land when the interval has passed");
    CHECK(!rt.hasPendingWindowSize());
    // The same size again sends nothing.
    rt.setWindowSize(102, 31, 830, 618);
    CHECK(!rt.hasPendingWindowSize());
    rt.close();
}

// A program that takes the terminal from the shell hears its size again: once
// with the sample that saw it take over, then each quarter second through its
// first second, so a size change that landed before its SIGWINCH handler stood
// still reaches it. The shell is never re-sent its size, and the same program
// sampled again opens nothing.
TEST(aTakeoverHearsTheWindowSizeAgainThroughItsFirstSecond)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    runtime.setWindowSize(100, 30, 800, 600);

    pipe->foregroundGroup = 100;
    pipe->foregroundShell = true;
    pipe->toRead = "$ ";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->reassertCount == 0, "the shell at its prompt is not re-sent its size");

    pipe->foregroundGroup = 200;
    pipe->foregroundShell = false;
    pipe->toRead += "vim";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->reassertCount == 1, "the drain whose sample saw the takeover re-sends at once");
    CHECK_MSG(pipe->windowColumns == 100 && pipe->windowRows == 30 && pipe->windowXpixel == 800 && pipe->windowYpixel == 600,
        "the size re-sent is the one the frontend published");

    runtime.reassertWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros());
    CHECK_MSG(pipe->reassertCount == 1, "not again before the spacing");
    const std::optional<std::uint32_t> wait = runtime.windowSizeReassertDueInMillis(Ubiquitty::PAL::monotonicMicros());
    CHECK_MSG(wait.has_value() && *wait > 0 && *wait <= 250, "the ingest loop is told when to wake for the next one");

    for (int expected = 2; expected <= 4; ++expected) {
        Ubiquitty::PAL::s_fakeMicroseconds += 250'000;
        runtime.reassertWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros());
        CHECK_MSG(pipe->reassertCount == expected, "a re-send each quarter second inside the window");
    }
    Ubiquitty::PAL::s_fakeMicroseconds += 250'000;
    runtime.reassertWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros());
    CHECK_MSG(pipe->reassertCount == 4, "none once the second is over");
    CHECK_MSG(!runtime.windowSizeReassertDueInMillis(Ubiquitty::PAL::monotonicMicros()).has_value(), "and nothing left to wake for");

    pipe->toRead += "~";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->reassertCount == 4, "the same program sampled again opens nothing");

    pipe->foregroundGroup = 300;
    pipe->toRead += "htop";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->reassertCount == 5, "the next program opens a window of its own");
    runtime.close();
}

// A re-send names the group its window was opened for, and the transport
// signals only while that group still holds the terminal. A shell taking the
// terminal back inside the window is not woken in the takeover's place: the
// rule that allows the signal at all read the takeover's facts, and a bash
// prompt reprints its upper lines on every SIGWINCH it is sent. The size goes
// out either way.
TEST(aReassertNamesTheGroupItsWindowWasOpenedFor)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    Ubiquitty::PAL::StreamTestPipe* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    runtime.setWindowSize(100, 30, 800, 600);

    pipe->foregroundGroup = 100;
    pipe->foregroundShell = true;
    pipe->toRead = "$ ";
    (void)runtime.drainAvailable(host);

    pipe->foregroundGroup = 200;
    pipe->foregroundShell = false;
    pipe->toRead += "vim";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(pipe->reassertExpectedGroup == 200, "the re-send names the group whose takeover opened the window");
    CHECK_MSG(pipe->reassertCount == 1 && pipe->reassertSignalCount == 1, "and it reaches that group");

    // The shell has the terminal back, and no sample has said so yet: the
    // window is still open for the program that took it.
    pipe->foregroundGroup = 100;
    Ubiquitty::PAL::s_fakeMicroseconds += 250'000;
    runtime.reassertWindowSizeIfDue(Ubiquitty::PAL::monotonicMicros());
    CHECK_MSG(pipe->reassertCount == 2, "the size is re-sent on the schedule");
    CHECK_MSG(pipe->reassertExpectedGroup == 200, "still for the group the takeover named");
    CHECK_MSG(pipe->reassertSignalCount == 1, "and the group now holding the terminal is not signalled");
    runtime.close();
}

TEST(aWholeScreenEraseHoldsThePresentUntilTheRedrawSettles)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CountingDrainHost host;
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(Ubiquitty::PAL::monotonicMicros()), "nothing is held before an erase");
    s->toRead += "\x1b[2J";
    (void)rt.drainAvailable(host);
    const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(now), "the drain that erased the screen holds the present");
    CHECK(rt.eraseHoldMicrosRemaining(now) > 0);
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(now + 3000), "and keeps holding while the redraw can still be arriving");
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(now + 20000), "until the program's bytes have been quiet for the settle");
    CHECK(rt.eraseHoldMicrosRemaining(now + 20000) == 0);
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(now + 21000), "a released hold stays released");
    // A second erase arms it again.
    s->toRead += "\x1b[2J";
    (void)rt.drainAvailable(host);
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(Ubiquitty::PAL::monotonicMicros()), "the next erase re-arms the hold");
    rt.close();
}

// The flood pipeline parses through feedChunkUpToSyncClear and closes its
// passes through finishExternalDrainPass; the erase hold's settle clock must
// see those bytes too, or a hold armed by a pipelined pass reads as settled on
// its first query and the blank screen is presented.
TEST(aPipelinedEraseHoldsThePresentLikeASequentialOne)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    REQUIRE(attachPipe(rt) != nullptr);
    CountingDrainHost host;
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    const char erase[] = "\x1b[2J";
    (void)rt.feedChunkUpToSyncClear({ erase, literalLength(erase) });
    rt.finishExternalDrainPass(host, static_cast<int>(literalLength(erase)), DrainPassEnd::WouldBlock);
    const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(now), "the pipelined pass that erased the screen holds the present");
    CHECK(rt.eraseHoldMicrosRemaining(now) > 0);
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(now + 20000), "until the bytes have been quiet for the settle");
    rt.close();
}

// The drain stamps the erase hold when its pass ends, and a capture can take
// the model between the parse of an erase and that stamp: the capture's
// recheck under the lock arms the hold itself, or it would present the erased
// screen with nothing holding it back.
TEST(aCaptureBeforeTheErasingPassEndsArmsTheHold)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    REQUIRE(attachPipe(rt) != nullptr);
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    const char erase[] = "\x1b[2J";
    (void)rt.feedChunkUpToSyncClear({ erase, literalLength(erase) });
    const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(now), "the pass has not ended, so nothing has stamped the erase");
    Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    CHECK_MSG(rt.shouldWithholdPresentation(terminal, Ubiquitty::PAL::monotonicTicks(), now), "the capture's recheck holds the erased screen back");
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(now + 3000), "and the hold it armed stands for the redraw");
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(now + 20000), "until the bytes have been quiet for the settle");
    rt.close();
}

// The ingest thread stamps bytes while a presenter holds a `now` it read a
// moment earlier; a stamp after that `now` is no time at all, not seventy
// minutes of quiet that release the hold mid-redraw.
TEST(aByteStampedAfterThePresentersNowKeepsTheHold)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CountingDrainHost host;
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    const std::uint32_t presenterNow = Ubiquitty::PAL::monotonicMicros();
    s->toRead += "\x1b[2J";
    (void)rt.drainAvailable(host);
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(presenterNow), "an erase and bytes stamped after the presenter's now hold");
    CHECK(rt.eraseHoldMicrosRemaining(presenterNow) > 0);
    rt.close();
}

TEST(aContinuousResizePresentsBetweenDisplayRefreshes)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    REQUIRE(attachPipe(runtime) != nullptr);
    CountingDrainHost host;
    constexpr std::uint32_t startedAt = 1'000'000;
    constexpr std::uint32_t frameMicros = 8333;
    constexpr std::string_view repaint = "\x1b[2J\x1b[Hrepainted";
    for (int step = 0; step < 60; ++step) {
        const std::uint32_t stepAt = startedAt + static_cast<std::uint32_t>(step) * frameMicros;
        Ubiquitty::PAL::s_fakeMicroseconds = stepAt;
        runtime.resizeGrid(24, 80 + step % 2, Ubiquitty::Modes::ReflowMode::Fixed,
            Ubiquitty::Modes::ResizeUrgency::Interactive, Ubiquitty::Modes::AnchorMode::Top);
        Ubiquitty::PAL::s_fakeMicroseconds = stepAt + 1000;
        (void)runtime.feedChunkUpToSyncClear(repaint);
        runtime.finishExternalDrainPass(host, static_cast<int>(repaint.size()), DrainPassEnd::WouldBlock);
        CHECK_MSG(runtime.shouldWithholdPresentationAfterErase(stepAt + 2000), "a just-erased frame still waits for redraw bytes");
        CHECK_MSG(runtime.eraseHoldMicrosRemaining(stepAt + frameMicros) == 0, "the completed repaint is ready within a 120 Hz display frame");
        CHECK_MSG(!runtime.shouldWithholdPresentationAfterErase(stepAt + frameMicros), "continuous resizing must not starve presentation");
    }
    runtime.close();
}

// A live resize asks a full-screen program for a repaint on every step and
// waits for no quiet output: its capture holds an erase only while the redraw
// behind it is still arriving, and shows it as soon as the drain has caught up,
// where an ordinary present still waits out the settle. A pass that spent its
// time budget has not caught up.
TEST(aResizeCaptureHoldsAnEraseUntilTheDrainCatchesUpWithItsRedraw)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    REQUIRE(attachPipe(rt) != nullptr);
    CountingDrainHost host;
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
    CHECK_MSG(!rt.isEraseRedrawArriving(*terminal, now), "nothing is arriving before an erase");

    constexpr std::string_view erase = "\x1b[H\x1b[2Jfirst row";
    (void)rt.feedChunkUpToSyncClear(erase);
    CHECK_MSG(rt.isEraseRedrawArriving(*terminal, now), "a capture inside the pass that parsed the erase holds it");
    rt.finishExternalDrainPass(host, static_cast<int>(erase.size()), DrainPassEnd::TimeBudget);
    CHECK_MSG(rt.isEraseRedrawArriving(*terminal, now + 1000), "a pass that spent its time budget has not caught up with the redraw");

    constexpr std::string_view rest = "\r\nlast row";
    (void)rt.feedChunkUpToSyncClear(rest);
    rt.finishExternalDrainPass(host, static_cast<int>(rest.size()), DrainPassEnd::WouldBlock);
    now = Ubiquitty::PAL::monotonicMicros();
    CHECK_MSG(rt.shouldWithholdPresentationAfterErase(now + 1000), "an ordinary present still waits for the settle");
    CHECK_MSG(!rt.isEraseRedrawArriving(*terminal, now + 1000), "the resize capture shows the redraw once the drain has caught up");

    // The next erase is held again, until its own redraw has been parsed.
    (void)rt.feedChunkUpToSyncClear(erase);
    CHECK_MSG(rt.isEraseRedrawArriving(*terminal, Ubiquitty::PAL::monotonicMicros()), "the next erase arms the hold again");
    rt.close();
}

// The resize capture's hold lifts at the settle for a redraw that stops short of
// catching up, and at its own watchdog for bytes that never go quiet. The
// watchdog runs from the first ask that finds a redraw arriving, and a redraw
// parsed whole ends it, so a later erase is held on a deadline of its own.
TEST(aResizeCaptureHoldIsBoundedByTheSettleAndItsOwnWatchdog)
{
    constexpr std::string_view erase = "\x1b[H\x1b[2Jfirst rows";
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        REQUIRE(attachPipe(rt) != nullptr);
        CountingDrainHost host;
        Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
        Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
        REQUIRE(terminal != nullptr);
        (void)rt.feedChunkUpToSyncClear(erase);
        rt.finishExternalDrainPass(host, static_cast<int>(erase.size()), DrainPassEnd::TimeBudget);
        const std::uint32_t erasedAt = Ubiquitty::PAL::monotonicMicros();
        CHECK_MSG(rt.isEraseRedrawArriving(*terminal, erasedAt + 5000), "a redraw that stops short is held through the settle");
        CHECK_MSG(!rt.isEraseRedrawArriving(*terminal, erasedAt + 6000), "and shown once the bytes have been quiet for it");
        rt.close();
    }
    {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        REQUIRE(attachPipe(rt) != nullptr);
        CountingDrainHost host;
        Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
        Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
        REQUIRE(terminal != nullptr);
        (void)rt.feedChunkUpToSyncClear(erase);
        rt.finishExternalDrainPass(host, static_cast<int>(erase.size()), DrainPassEnd::TimeBudget);
        const std::uint32_t firstAsk = Ubiquitty::PAL::monotonicMicros();
        REQUIRE(rt.isEraseRedrawArriving(*terminal, firstAsk));
        // A byte every millisecond, so the settle never comes, from a drain
        // that never catches up. Each ask falls half a millisecond after its
        // step: this clock moves a microsecond on every read, and an ask on the
        // watchdog's own instant would be decided by how many reads came first.
        int heldThrough = 0;
        for (int milliseconds = 1; milliseconds <= 60; ++milliseconds) {
            const std::uint32_t stepAt = firstAsk + static_cast<std::uint32_t>(milliseconds) * 1000;
            Ubiquitty::PAL::s_fakeMicroseconds = stepAt;
            constexpr std::string_view more = "more";
            (void)rt.feedChunkUpToSyncClear(more);
            rt.finishExternalDrainPass(host, static_cast<int>(more.size()), DrainPassEnd::TimeBudget);
            if (!rt.isEraseRedrawArriving(*terminal, stepAt + 500))
                break;
            heldThrough = milliseconds;
        }
        CHECK_MSG(heldThrough == 49, "bytes that never go quiet are held until the watchdog, 50 ms from the first ask, and no longer");
        rt.close();
    }
    for (const bool wasAsked : { true, false }) {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        REQUIRE(attachPipe(rt) != nullptr);
        CountingDrainHost host;
        Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
        Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
        REQUIRE(terminal != nullptr);
        (void)rt.feedChunkUpToSyncClear(erase);
        if (wasAsked)
            CHECK(rt.isEraseRedrawArriving(*terminal, Ubiquitty::PAL::monotonicMicros()));
        rt.finishExternalDrainPass(host, static_cast<int>(erase.size()), DrainPassEnd::WouldBlock);
        Ubiquitty::PAL::s_fakeMicroseconds += 60'000;
        constexpr std::string_view second = "\x1b[H\x1b[2Jpartial";
        (void)rt.feedChunkUpToSyncClear(second);
        CHECK_MSG(rt.isEraseRedrawArriving(*terminal, Ubiquitty::PAL::monotonicMicros()), "a later erase is held mid-pass on a deadline of its own");
        rt.finishExternalDrainPass(host, static_cast<int>(second.size()), DrainPassEnd::TimeBudget);
        CHECK_MSG(rt.isEraseRedrawArriving(*terminal, Ubiquitty::PAL::monotonicMicros() + 1000), "and after a pass that ended on its budget");
        rt.close();
    }
}

// A redraw whose last bytes ended a pass on its time budget is whole once the
// drain finds nothing more to read, though that read parses nothing: the
// sequential drain's confirming empty read and the flood pipeline's empty pass
// both say so, as the stream's end does.
TEST(anEmptyCaughtUpPassOrTheStreamsEndFinishesARedraw)
{
    for (const int path : { 0, 1, 2 }) {
        TerminalRuntime rt;
        rt.init(parameters80x24());
        auto* pipe = attachPipe(rt);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
        Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
        REQUIRE(terminal != nullptr);
        constexpr std::string_view whole = "\x1b[H\x1b[2Jfirst row\r\nlast row";
        (void)rt.feedChunkUpToSyncClear(whole);
        rt.finishExternalDrainPass(host, static_cast<int>(whole.size()), DrainPassEnd::TimeBudget);
        const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
        CHECK_MSG(rt.isEraseRedrawArriving(*terminal, now + 1000), "a pass that ended on its budget leaves the redraw arriving");
        if (path == 0) {
            CHECK(rt.drainAvailable(host).caughtUp);
        } else if (path == 1) {
            CHECK(rt.finishExternalDrainPass(host, 0, DrainPassEnd::WouldBlock).caughtUp);
        } else {
            pipe->eofPending = true;
            CHECK(rt.drainAvailable(host).reachedEnd);
        }
        CHECK_MSG(!rt.isEraseRedrawArriving(*terminal, now + 1000), "the read that finds nothing more shows the redraw at once");
        rt.close();
    }
}

// Fed bytes arrive whole, so a feed that erases the screen itself carries its
// redraw whole. A feed that only echoes a keystroke, or that erases while the
// program's redraw is under way, leaves that redraw arriving.
TEST(onlyAFeedCarryingItsOwnEraseFinishesARedraw)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    REQUIRE(attachPipe(rt) != nullptr);
    CountingDrainHost host;
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    Ubiquitty::Terminal* terminal = rt.borrowTermForRender();
    REQUIRE(terminal != nullptr);
    const auto feed = [&](std::string_view bytes) {
        const std::uint32_t before = rt.eraseGeneration();
        (void)rt.feedChunkUpToSyncClear(bytes);
        rt.noteFeedParsed(before);
    };
    const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
    feed("\x1b[H\x1b[2Jfed screen");
    CHECK_MSG(!rt.isEraseRedrawArriving(*terminal, now + 1000), "a feed carrying its own erase is whole");

    constexpr std::string_view program = "\x1b[H\x1b[2Jprogram rows";
    (void)rt.feedChunkUpToSyncClear(program);
    rt.finishExternalDrainPass(host, static_cast<int>(program.size()), DrainPassEnd::TimeBudget);
    CHECK(rt.isEraseRedrawArriving(*terminal, now + 1000));
    feed("x");
    CHECK_MSG(rt.isEraseRedrawArriving(*terminal, now + 1000), "an echoed keystroke leaves the program's redraw arriving");
    feed("\x1b[2J");
    CHECK_MSG(rt.isEraseRedrawArriving(*terminal, now + 1000), "and so does an erase fed while that redraw is under way");
    rt.close();
}

TEST(repeatedErasesCannotRestartThePresentationWatchdog)
{
    for (const auto urgency : { Ubiquitty::Modes::ResizeUrgency::Settled, Ubiquitty::Modes::ResizeUrgency::Interactive }) {
        TerminalRuntime runtime;
        runtime.init(parameters80x24());
        REQUIRE(attachPipe(runtime) != nullptr);
        CountingDrainHost host;
        Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
        runtime.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Fixed, urgency, Ubiquitty::Modes::AnchorMode::Top);
        constexpr std::string_view repaint = "\x1b[2J\x1b[Hrepainted";
        for (std::uint32_t elapsed = 0; elapsed <= 52000; elapsed += 4000) {
            Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000 + elapsed;
            (void)runtime.feedChunkUpToSyncClear(repaint);
            runtime.finishExternalDrainPass(host, static_cast<int>(repaint.size()), DrainPassEnd::WouldBlock);
        }
        const std::uint32_t now = Ubiquitty::PAL::monotonicMicros();
        CHECK_MSG(runtime.eraseHoldMicrosRemaining(now) == 0, "repeated erases cannot move the watchdog deadline");
        CHECK_MSG(!runtime.shouldWithholdPresentationAfterErase(now), "a busy redraw releases within the ordinary 50 ms bound");
        (void)runtime.feedChunkUpToSyncClear(repaint);
        runtime.finishExternalDrainPass(host, static_cast<int>(repaint.size()), DrainPassEnd::WouldBlock);
        CHECK_MSG(runtime.shouldWithholdPresentationAfterErase(Ubiquitty::PAL::monotonicMicros()), "an erase after release arms a fresh hold");
        runtime.close();
    }
}

// A pipelined burst that stops exactly on a pass boundary leaves no partial
// pass to close, and the full pass before it marked the drain saturated: the
// render skips every tick while that holds. The pipeline reports the ring empty
// across a nap as a pass of zero bytes, which is the writer having paused.
TEST(aPipelinedPauseOnAPassBoundaryClearsSaturation)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    REQUIRE(attachPipe(rt) != nullptr);
    CountingDrainHost host;
    const std::string pass(static_cast<std::size_t>(TerminalRuntime::drainPassCapacityBytes), 'x');
    (void)rt.feedChunkUpToSyncClear({ pass.data(), pass.size() });
    rt.finishExternalDrainPass(host, TerminalRuntime::drainPassCapacityBytes, DrainPassEnd::ByteBudget);
    REQUIRE(rt.isDrainSaturated());
    rt.finishExternalDrainPass(host, 0, DrainPassEnd::WouldBlock);
    CHECK_MSG(!rt.isDrainSaturated(), "the pause after a full pass leaves the drain saturated");
    rt.close();
}

// A frontend that drains and paints on one thread paints after a drain only
// when the drain's own settle did not, and only when the program has not asked
// it to wait: an open 2026 block is the tear the mode exists to prevent, a
// frame the coherence rule holds is a screen not finished, and a second paint
// of a settled drain renders the whole screen twice.
TEST(aSingleThreadedFrontendPaintsADrainOnceAndNeverInsideABlock)
{
    // The loop a single-threaded frontend runs: drain, ask the rule again for
    // a frame it held once its wait is up, tell the runtime whether either
    // painted, and paint what the runtime says is owed.
    const auto drainAndPaint = [](TerminalRuntime& rt, CountingDrainHost& host, HandClock& clock) {
        const int framesBefore = host.frames;
        (void)rt.drainAvailable(host);
        settleHeldFrame(rt, host, clock);
        rt.noteDrainForOwnPaint(host.frames != framesBefore ? TerminalRuntime::SettlePaint::Painted : TerminalRuntime::SettlePaint::Owed);
        int paints = host.frames - framesBefore;
        if (rt.takeOwedPaint(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()))
            ++paints;
        return paints;
    };
    {
        TerminalRuntime rt;
        HandClock clock;
        clock.drive(rt);
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        CountingDrainHost host;
        s->toRead += "\x1b[?2026hHALF";
        CHECK_MSG(drainAndPaint(rt, host, clock) == 0, "a frame inside an open block was painted");
        const std::optional<std::uint64_t> due = rt.publishHeldFrameIfDue(host);
        CHECK_MSG(due.has_value() && *due > 0, "a withheld paint does not wake the loop when the hold expires");
        // The rule's wait wakes the loop, so the owed paint adds none of its own
        // while the rule holds the frame: a zero would spin the loop.
        CHECK(rt.owedPaintWaitMilliseconds(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()) == -1);
        s->toRead += "DONE\x1b[?2026l";
        CHECK_MSG(drainAndPaint(rt, host, clock) == 1, "the closed block was not painted exactly once");
        CHECK(rt.owedPaintWaitMilliseconds(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()) == -1);
        rt.close();
    }
    {
        TerminalRuntime rt;
        HandClock clock;
        clock.drive(rt);
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        CountingDrainHost host;
        s->toRead += "abc";
        CHECK_MSG(drainAndPaint(rt, host, clock) == 1, "a settled drain was painted twice, or not at all");
        rt.close();
    }
    {
        // A frame the rule holds is not painted as owed: it is painted when
        // the rule admits it, through the host.
        TerminalRuntime rt;
        HandClock clock;
        clock.drive(rt);
        rt.init(parameters80x24());
        auto* s = attachPipe(rt);
        REQUIRE(s != nullptr);
        CountingDrainHost host;
        s->toRead += "$ ";
        CHECK(drainAndPaint(rt, host, clock) == 1);
        clock.nowMicroseconds += 50'000;
        s->toRead += "\x1b[?25lhalf a repaint";
        const int framesBefore = host.frames;
        (void)rt.drainAvailable(host);
        rt.noteDrainForOwnPaint(host.frames != framesBefore ? TerminalRuntime::SettlePaint::Painted : TerminalRuntime::SettlePaint::Owed);
        CHECK_MSG(!rt.takeOwedPaint(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()),
            "a frame inside a cursor-hidden redraw was painted as owed");
        rt.close();
    }
    {
        // A drain whose settle refused the frame, with nothing withheld, is
        // painted by the frontend, once.
        TerminalRuntime rt;
        rt.init(parameters80x24());
        CountingDrainHost host;
        rt.noteDrainForOwnPaint(TerminalRuntime::SettlePaint::Owed);
        CHECK(rt.takeOwedPaint(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()));
        CHECK(!rt.takeOwedPaint(Ubiquitty::PAL::monotonicTicks(), Ubiquitty::PAL::monotonicMicros()));
        rt.close();
    }
}

TEST(aPartialEraseHoldsNothing)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CountingDrainHost host;
    s->toRead += "\x1b[Jsome text\x1b[K";
    (void)rt.drainAvailable(host);
    CHECK_MSG(!rt.shouldWithholdPresentationAfterErase(Ubiquitty::PAL::monotonicMicros()), "an erase to the end of the screen is a prompt's, not a redraw's");
    rt.close();
}

TEST(aModeTwentyFortyEightResizeReportReachesTheChildWithTheResize)
{
    TerminalRuntime rt;
    rt.init(parameters80x24());
    auto* s = attachPipe(rt);
    REQUIRE(s != nullptr);
    CountingDrainHost host;
    // Enabling the mode reports once at once; that report is the drain's.
    s->toRead += "\x1b[?2048h";
    (void)rt.drainAvailable(host);
    s->written.clear();
    rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Reflow);
    CHECK_MSG(s->written.find("\x1b[48;30;100;") != std::string::npos, "the report for the new grid is on the wire with the resize, not on the next drain");
    s->written.clear();
    rt.resizeGrid(30, 100, Ubiquitty::Modes::ReflowMode::Reflow);
    CHECK_MSG(s->written.empty(), "an unchanged grid reports nothing");
    rt.close();
}

// The generation an Inspector keys its process table on. The chain moving is
// not the same question: a reaped background job, a stopped process and a
// reordered walk all leave the chain standing while the table a reader is
// looking at is out of date.
TEST(processRecordsMoveOnlyWhenARecordChanges)
{
    using Ubiquitty::PAL::Process::Record;
    using Ubiquitty::Runtime::didProcessRecordsMove;

    Record shell;
    shell.processID = 100;
    shell.name = "zsh";
    shell.isSessionLeader = true;
    Record job;
    job.processID = 101;
    job.name = "sleep";

    const std::vector<Record> both { shell, job };
    CHECK_MSG(!didProcessRecordsMove(both, both), "identical walks move nothing");

    const std::vector<Record> reaped { shell };
    CHECK_MSG(didProcessRecordsMove(both, reaped), "a reaped job moves the table");

    std::vector<Record> stopped { shell, job };
    stopped[1].state = Ubiquitty::PAL::Process::State::Stopped;
    CHECK_MSG(didProcessRecordsMove(both, stopped), "a state flip moves it");

    const std::vector<Record> reordered { job, shell };
    CHECK_MSG(didProcessRecordsMove(both, reordered), "and so does the order the walk found them in");

    std::vector<Record> renamed { shell, job };
    renamed[1].commandLine = "sleep 4";
    CHECK_MSG(didProcessRecordsMove(both, renamed), "a command line that filled in moves it");
}

// A local pane's bells ring its own terminal's route: two runtimes in one
// process each report only the bells their own program rang.
TEST(aRuntimesBellsRingOnlyItsOwnTerminalsRoute)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime ringing;
    TerminalRuntime quiet;
    ringing.init(parameters80x24());
    quiet.init(parameters80x24());
    auto* ringingStream = attachPipe(ringing);
    auto* quietStream = attachPipe(quiet);
    REQUIRE(ringingStream != nullptr && quietStream != nullptr);
    ringingStream->toRead.append("\a\a\a");
    quietStream->toRead.append("x");
    CountingDrainHost host;
    CHECK_EQ(ringing.drainAvailable(host).bellsRung, 3u);
    CHECK_EQ(quiet.drainAvailable(host).bellsRung, 0u);
    CHECK_EQ(ringing.drainAvailable(host).bellsRung, 0u);
}

// A session counts its pane's bells from its own terminal's route, including
// bytes its embedder feeds outside a drain. A frontend plays per pane from
// this count.
TEST(aSessionCountsOnlyItsOwnPanesBells)
{
    auto* ringing = Ubiquitty::Runtime::TerminalSession::make();
    auto* quiet = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(ringing != nullptr && quiet != nullptr);
    ringing->start(24, 80);
    quiet->start(24, 80);
    constexpr std::string_view bells = "\a\a";
    ringing->feedBytes({ bells.data(), bells.size() });
    constexpr std::string_view text = "x";
    quiet->feedBytes({ text.data(), text.size() });
    CHECK_EQ(ringing->takeBellsRung(), 2u);
    CHECK_EQ(quiet->takeBellsRung(), 0u);
    CHECK_EQ(ringing->takeBellsRung(), 0u);
    ringing->close();
    quiet->close();
    releaseTerminalSession(ringing);
    releaseTerminalSession(quiet);
}

// A local pane times its commands by when their marks were read, on its own
// terminal's clock, and two panes each by their own reads.
TEST(twoRuntimesTimeTheirCommandsByWhenTheyReadTheirMarks)
{
    using namespace Ubiquitty::Runtime;
    TerminalRuntime left;
    TerminalRuntime right;
    left.init(parameters80x24());
    right.init(parameters80x24());
    auto* leftStream = attachPipe(left);
    auto* rightStream = attachPipe(right);
    REQUIRE(leftStream != nullptr && rightStream != nullptr);
    CountingDrainHost host;
    const auto readAt = [&host](TerminalRuntime& runtime, Ubiquitty::PAL::StreamTestPipe& stream, const char* ticks, const char* bytes) {
        ::setenv("UBIQUITTY_FAKE_TICKS", ticks, 1);
        stream.toRead.append(bytes);
        (void)runtime.drainAvailable(host);
    };
    readAt(left, *leftStream, "100", "\x1b]133;C\x07");
    readAt(right, *rightStream, "5000", "\x1b]133;C\x07");
    readAt(left, *leftStream, "160", "\x1b]133;D;0\x07");
    readAt(right, *rightStream, "5090", "\x1b]133;D;0\x07");
    ::unsetenv("UBIQUITTY_FAKE_TICKS");
    const auto lastDuration = [](TerminalRuntime& runtime) {
        const Ubiquitty::Terminal& term = *runtime.borrowTermForRender();
        return term.commandDurationTicksRing[(term.commandCompletedGeneration + 7) % 8];
    };
    CHECK_EQ(lastDuration(left), 60u);
    CHECK_EQ(lastDuration(right), 90u);
}

UBIQUITTY_TEST_MAIN()

/// A routed pane's session speaks for its tmux session: an intent synthesizes
/// a control command aimed at the session's own pane, closing the session
/// detaches or kills it, and a window resize reports the client's size — each
/// one line down the primary's channel, newline-terminated because tmux
/// dispatches on the newline. Unrouted, the same calls send nothing: a native
/// pane's frontend performs its own action.
TEST(sessionTmuxVerbsRideTheControlChannelForTheirOwnPane)
{
    using Ubiquitty::Protocol::Tmux::ActionIntent;
    using Ubiquitty::Protocol::Tmux::PaneContext;
    using Ubiquitty::Runtime::TerminalSession;
    TerminalSession* session = TerminalSession::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    Ubiquitty::PAL::StreamTestPipe primary;
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    Ubiquitty::Protocol::Tmux::init(&client);
    session->routeInputViaTmux(&client, &primary, 7);
    CHECK(session->tmuxPaneContext() == PaneContext::ControlMode);

    CHECK(session->performTmuxIntent(ActionIntent::SplitStacked, ""));
    CHECK_STR_EQ(primary.written, "split-window -v -t %7\n");
    primary.written.clear();
    CHECK(session->performTmuxIntent(ActionIntent::RenameWindow, "logs"));
    CHECK_STR_EQ(primary.written, "rename-window -t %7 \"logs\"\n");
    primary.written.clear();
    CHECK_MSG(session->tmuxIntentKeystrokes(ActionIntent::SplitStacked, "").empty(), "a -CC pane's verb is a command, never a chord to type");

    CHECK(session->requestTmuxWindowClose(3));
    CHECK_STR_EQ(primary.written, "kill-window -t @3\n");
    primary.written.clear();
    CHECK(!session->requestTmuxWindowClose(-1));
    CHECK(primary.written.empty());

    CHECK(session->requestTmuxSessionClose(TerminalSession::TmuxSessionClose::Detach));
    CHECK_STR_EQ(primary.written, "detach-client\n");
    primary.written.clear();
    CHECK(session->requestTmuxSessionClose(TerminalSession::TmuxSessionClose::Kill));
    CHECK_STR_EQ(primary.written, "kill-session\n");
    primary.written.clear();

    CHECK(session->reportTmuxClientSize(161, 47));
    CHECK_STR_EQ(primary.written, "refresh-client -C 161x47\n");
    primary.written.clear();
    CHECK_MSG(!session->reportTmuxClientSize(0, 47), "a grid with no cells is not a size");
    CHECK(primary.written.empty());

    session->routeInputViaTmux(nullptr, nullptr, 0);
    CHECK(session->tmuxPaneContext() == PaneContext::NativePane);
    CHECK(!session->performTmuxIntent(ActionIntent::NewWindow, ""));
    CHECK(!session->requestTmuxSessionClose(TerminalSession::TmuxSessionClose::Kill));
    CHECK(!session->requestTmuxWindowClose(3));
    CHECK(!session->reportTmuxClientSize(161, 47));
    CHECK_MSG(primary.written.empty(), "an unrouted session sends the primary nothing");

    session->close();
    releaseTerminalSession(session);
}

TEST(testPaneTransfersStayOnTheirOwningTmuxConnection)
{
    using Ubiquitty::Runtime::TerminalSession;
    using namespace Ubiquitty::Panes;
    auto* source = TerminalSession::make();
    auto* target = TerminalSession::make();
    CHECK(source && target);
    if (!source || !target) {
        return;
    }
    Ubiquitty::PAL::StreamTestPipe first;
    Ubiquitty::PAL::StreamTestPipe second;
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    Ubiquitty::Protocol::Tmux::init(&client);
    source->routeInputViaTmux(&client, &first, 7);
    target->routeInputViaTmux(&client, &second, 9);
    Tree tree(7);
    CHECK(!source->canTransferTo(*target));
    CHECK(source->requestTransferTo(*target, tree, Orientation::Vertical, InsertSide::First, TerminalSession::TransferScope::Pane, TerminalSession::TransferPlacement::Split) == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(first.written.empty() && second.written.empty());
    target->routeInputViaTmux(&client, &first, 9);
    CHECK(source->requestTransferTo(*target, tree, Orientation::Vertical, InsertSide::First, TerminalSession::TransferScope::Pane, TerminalSession::TransferPlacement::Split) == TerminalSession::PaneVerbOutcome::Handled);
    CHECK_STR_EQ(first.written, "select-window -t %9 ; run-shell -d 0 ; move-pane -d -h -b -l 50% -s %7 -t %9\nselect-pane -t %7\n");
    first.written.clear();
    CHECK(source->requestExtraction(TerminalSession::TransferScope::Pane, TerminalSession::ExtractionPlacement::Window) == TerminalSession::PaneVerbOutcome::Handled);
    CHECK_STR_EQ(first.written, "break-pane -s %7\n");
    source->routeInputViaTmux(nullptr, nullptr, 0);
    target->routeInputViaTmux(nullptr, nullptr, 0);
    releaseTerminalSession(source);
    releaseTerminalSession(target);
}

/// A full-screen `tmux attach` in the session's own PTY is driven by prefix
/// chords typed into that PTY, read off the live process table at the call. A
/// typed `tmux -CC` is not: its stdin is the control protocol, and a chord
/// written there is garbage to it.
TEST(sessionRawTmuxAttachTakesPrefixChordsAndATypedControlClientNone)
{
    using Ubiquitty::Protocol::Tmux::ActionIntent;
    using Ubiquitty::Protocol::Tmux::PaneContext;
    Ubiquitty::Runtime::TerminalSession* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* pty = stream.get();
    pty->scriptedForegroundName = "tmux";
    Ubiquitty::PAL::Process::Record attach;
    attach.name = "tmux";
    attach.commandLine = "tmux attach -t work";
    attach.isForeground = true;
    pty->scriptedRecords = { attach };
    auto adoption = session->adoptStream(std::move(stream));
    REQUIRE(adoption.has_value());

    CHECK(session->tmuxPaneContext() == PaneContext::RawTmuxAttach);
    CHECK(session->performTmuxIntent(ActionIntent::NewWindow, ""));
    CHECK_STR_EQ(pty->written, "\x02"
                               "c");
    pty->written.clear();
    // The same bytes handed back for a frontend to type on the reader's
    // behalf, so the chord takes the user-input route its side effects — the
    // dropped selection, the local echo, the broadcast fan-out — belong to.
    CHECK_STR_EQ(session->tmuxIntentKeystrokes(ActionIntent::NewWindow, ""), "\x02"
                                                                             "c");
    CHECK_MSG(pty->written.empty(), "asking for the chord sends nothing");

    pty->scriptedRecords[0].commandLine = "tmux -CC attach -t work";
    CHECK(session->tmuxPaneContext() == PaneContext::NativePane);
    CHECK(!session->performTmuxIntent(ActionIntent::NewWindow, ""));
    CHECK_MSG(session->tmuxIntentKeystrokes(ActionIntent::NewWindow, "").empty(), "and a typed control client has no chord to hand back");
    CHECK_MSG(pty->written.empty(), "a control-mode client's stdin takes no chord");

    pty->scriptedForegroundName = "vim";
    CHECK(session->tmuxPaneContext() == PaneContext::NativePane);

    session->close();
    releaseTerminalSession(session);
}

TEST(aMovedReportedHostRaisesTheProcessStateEdge)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    pipe->foregroundGroup = 100;
    pipe->toRead = "\x1b]7;file://ubq-tag-host/tmp\x07";
    CHECK(runtime.drainAvailable(host).processStateChanged);
    pipe->toRead += "\x1b]7;file://ubq-tag-host/tmp\x07";
    CHECK(!runtime.drainAvailable(host).processStateChanged);
    pipe->toRead += "\x1b]7;file://ubq-other-host/tmp\x07";
    CHECK(runtime.drainAvailable(host).processStateChanged);
    CHECK_STR_EQ(runtime.reportedHost(), "ubq-other-host");
}

// The local arms announce frames through publishSettledFrameIfCoherent, and so
// by the rule the host daemon publishes by: a cursor-hidden redraw holds the
// frame until the cursor is shown, a read that has not gone quiet holds it, and
// a program that hid the cursor and went quiet is shown at the deadline.
TEST(theLocalPublishFollowsTheCoherenceRule)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    pipe->toRead = "$ ";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    CHECK(runtime.lastFrameClass() == FrameClass::Structural);
    CHECK(!runtime.isFrameHeld());
    const std::size_t afterFirst = host.reasons.size();

    // Half a repaint, the cursor hidden for it: not announced, however long
    // the read has been quiet, while the bracket is younger than the deadline.
    clock.nowMicroseconds += 50'000;
    pipe->toRead += "\x1b[?25l\x1b[H\x1b[2Ktop half";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    CHECK(host.reasons.size() == afterFirst);
    CHECK(runtime.isFrameHeld());
    settleHeldFrame(runtime, host, clock, Ubiquitty::Runtime::coherenceDeadlineMicroseconds - 1);
    CHECK_MSG(host.reasons.size() == afterFirst, "a frame inside a cursor-hidden redraw was announced");
    CHECK(runtime.isFrameHeld());

    // The rest of the repaint shows the cursor: announced once the read is quiet.
    clock.nowMicroseconds += 1'000;
    pipe->toRead += "\x1b[2;1Hbottom half\x1b[?25h";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    CHECK(host.reasons.size() == afterFirst);
    settleHeldFrame(runtime, host, clock);
    REQUIRE(host.reasons.size() == afterFirst + 1);
    CHECK(host.reasons.back() == SyncFrameReason::BurstEnd);
    CHECK(runtime.lastFrameClass() == FrameClass::Settled);
    CHECK(!runtime.isFrameHeld());

    // Hidden, and nothing more: the deadline shows it, forced.
    clock.nowMicroseconds += 50'000;
    pipe->toRead += "\x1b[?25l\x1b[3;1Hspinner";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock, Ubiquitty::Runtime::coherenceDeadlineMicroseconds);
    CHECK(host.reasons.size() == afterFirst + 2);
    CHECK(runtime.lastFrameClass() == FrameClass::Forced);
    CHECK(runtime.frameCount(FrameClass::Forced) == 1);
    CHECK(!runtime.publishHeldFrameIfDue(host).has_value());
}

// A 2026 block that opens while the rule holds a frame, and is never closed, is
// expired by the rule's own timer once it outlives its budget. Nothing else
// asks: the frontend's withhold answers from the held flag without reaching
// the block, so a timer that only names the block's expiry, and never runs it,
// holds the pane until the program writes again. Both ways a block arrives are
// covered: drained from the program, and fed by an embedder past the rule.
TEST(aBlockOpenedUnderAHeldFrameExpiresOnTheRulesTimer)
{
    const auto expiresOnTheTimer = [](bool isFed) {
        TerminalRuntime runtime;
        HandClock clock;
        clock.drive(runtime);
        runtime.init(parameters80x24());
        auto* pipe = attachPipe(runtime);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        // The block's budget is counted on the 60 Hz tick, pinned here and
        // moved with the rule's clock, so no case waits on the wall.
        std::uint32_t ticks = 1'000;
        const auto pinTicks = [&ticks] { setenv("UBIQUITTY_FAKE_TICKS", std::to_string(ticks).c_str(), 1); };
        pinTicks();
        pipe->toRead = "$ ";
        REQUIRE(runtime.drainAvailable(host).caughtUp);
        settleHeldFrame(runtime, host, clock);
        REQUIRE(!runtime.isFrameHeld());

        clock.nowMicroseconds += 50'000;
        pipe->toRead += "redraw";
        REQUIRE(runtime.drainAvailable(host).caughtUp);
        REQUIRE(runtime.isFrameHeld());
        const char open[] = "\x1b[?2026h";
        if (isFed) {
            REQUIRE(runtime.feedChunkUpToSyncClear({ open, literalLength(open) }).consumed > 0);
        } else {
            pipe->toRead += open;
            REQUIRE(runtime.drainAvailable(host).caughtUp);
        }
        REQUIRE(runtime.isFrameHeld());
        const std::size_t before = host.reasons.size();

        // The ingest loop's timer: wait what the rule names, then ask again.
        // Bounded by asks, not by time: the budget is one second, and every
        // ask sleeps at least as long as the rule says.
        for (int ask = 0; ask < 8 && runtime.isFrameHeld(); ++ask) {
            const std::optional<std::uint64_t> due = runtime.publishHeldFrameIfDue(host);
            if (!due)
                break;
            clock.nowMicroseconds += *due;
            ticks += static_cast<std::uint32_t>((*due + 16'666) / 16'667);
            pinTicks();
        }
        unsetenv("UBIQUITTY_FAKE_TICKS");
        CHECK_MSG(!runtime.isFrameHeld(), isFed ? "a fed block never expired under a held frame" : "a drained block never expired under a held frame");
        CHECK(host.reasons.size() == before + 1);
        CHECK(!runtime.publishHeldFrameIfDue(host).has_value());
        runtime.close();
    };
    expiresOnTheTimer(false);
    expiresOnTheTimer(true);
}

// A 2026 frame whose close is the read's last byte is complete the moment the
// pass ends: a driver that never asks the rule again (a frontend draining on
// its own readiness, with no ingest loop) must still hear it then.
TEST(aSynchronizedFrameEndingTheReadIsAnnouncedAtThePassEnd)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    pipe->toRead = "$ ";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    const std::size_t before = host.reasons.size();
    clock.nowMicroseconds += 50'000;
    pipe->toRead += "\x1b[?2026h\x1b[Hframe\x1b[?2026l";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    REQUIRE(host.reasons.size() == before + 1);
    CHECK(host.reasons.back() == SyncFrameReason::Boundary);
    CHECK(runtime.lastFrameClass() == FrameClass::Boundary);
}

// A 2026 block closed in the middle of a read, inside a cursor-hidden redraw,
// is the bracket's to hold on the local runtime as on the daemon: the frame at
// the close is not taken, and the pass that follows is not counted twice.
TEST(aSynchronizedBlockClosedInsideACursorHiddenRedrawIsHeld)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    pipe->toRead = "$ ";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    const std::size_t before = host.reasons.size();
    clock.nowMicroseconds += 50'000;
    pipe->toRead += "\x1b[?25l\x1b[?2026h\x1b[Hpart\x1b[?2026l\x1b[2;1Hthe rest outside the block";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    CHECK_MSG(host.reasons.size() == before, "a frame inside the bracket was announced at the 2026 close");
    CHECK(runtime.isFrameHeld());
    clock.nowMicroseconds += 1'000;
    pipe->toRead += "\x1b[?25h";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    REQUIRE(host.reasons.size() == before + 1);
    CHECK(runtime.frameCount(FrameClass::Boundary) + runtime.frameCount(FrameClass::Settled) >= 1);
}

// A host that presents frames at an interval paces the rule to it: a frame due
// inside the interval waits for it and is announced then, rather than handed to
// a host that would throttle it away.
TEST(theRuleIsPacedToTheHostsFrameInterval)
{
    struct PacedHost final : CountingDrainHost {
        std::uint32_t frameIntervalMicroseconds() const override { return 8'000; }
    };
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    PacedHost host;
    pipe->toRead = "$ ";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    const std::size_t before = host.reasons.size();
    const std::uint64_t taken = clock.nowMicroseconds;
    clock.nowMicroseconds += 100;
    pipe->toRead += "x";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    const std::optional<std::uint64_t> due = runtime.publishHeldFrameIfDue(host);
    REQUIRE(due.has_value());
    CHECK(*due == taken + 8'000 - clock.nowMicroseconds);
    CHECK(host.reasons.size() == before);
    settleHeldFrame(runtime, host, clock, 8'000);
    CHECK(host.reasons.size() == before + 1);
    CHECK(clock.nowMicroseconds == taken + 8'000);
}

TEST(drainSaturationPublishesItsReasonAndClearsAtSettleOrEnd)
{
    for (const bool hasReachedEnd : { false, true }) {
        TerminalRuntime runtime;
        HandClock clock;
        clock.drive(runtime);
        runtime.init(parameters80x24());
        auto* pipe = attachPipe(runtime);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        pipe->toRead = "short";
        REQUIRE(runtime.drainAvailable(host).caughtUp);
        CHECK(!runtime.isDrainSaturated());
        // A settle waits for the read to go quiet, then is the burst's end.
        settleHeldFrame(runtime, host, clock);
        REQUIRE(!host.reasons.empty());
        CHECK(host.reasons.back() == SyncFrameReason::BurstEnd);
        pipe->toRead.append(TerminalRuntime::drainPassCapacityBytes, 'x');
        const auto saturated = runtime.drainAvailable(host);
        REQUIRE(saturated.bytesDrained == TerminalRuntime::drainPassCapacityBytes);
        CHECK(runtime.isDrainSaturated());
        CHECK(host.reasons.back() == SyncFrameReason::SaturatedPass);
        // Capacity ends the read loop before its next read can observe EOF.
        pipe->eofPending = hasReachedEnd;
        const auto final = runtime.drainAvailable(host);
        CHECK(final.reachedEnd == hasReachedEnd);
        CHECK(!runtime.isDrainSaturated());
    }
}

namespace {

// Parsing advances both clocks without sleeping. Only a completed lock bite
// spends time, so a read spanning several bites exposes an early-return bug.
struct TimedDrainHost final : CountingDrainHost {
    explicit TimedDrainHost(HandClock& clock)
        : clock(clock)
    {
    }
    HandClock& clock;
    std::uint32_t interval = 8'000;
    std::uint32_t biteMicroseconds = 0;
    TerminalRuntime* observedRuntime = nullptr;
    bool didUnlockHeldFrame = false;
    std::uint32_t frameIntervalMicroseconds() const override { return interval; }
    void observeClock(TerminalRuntime& runtime)
    {
        observedRuntime = &runtime;
        runtime.setCoherenceClock(
            [](void* context) -> std::uint64_t {
                auto& host = *static_cast<TimedDrainHost*>(context);
                CHECK_EQ(host.locks, host.unlocks + 1);
                return host.clock.nowMicroseconds;
            },
            this);
    }
    void unlockModel() override
    {
        CountingDrainHost::unlockModel();
        if (observedRuntime && observedRuntime->isFrameHeld())
            didUnlockHeldFrame = true;
        clock.nowMicroseconds += biteMicroseconds;
        Ubiquitty::PAL::s_fakeMicroseconds += biteMicroseconds;
    }
};

} // namespace

TEST(aTimedDrainPublishesContinuingInputWithoutClaimingQuiet)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    TimedDrainHost host(clock);
    host.observeClock(runtime);
    pipe->toRead = "ready";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    const int framesBefore = host.frames;
    clock.nowMicroseconds += host.interval;
    pipe->maxReadBytes = 1024;
    pipe->toRead.append(8 * 1024, 'a');
    host.biteMicroseconds = 2'000;
    host.didUnlockHeldFrame = false;
    const auto outcome = runtime.drainAvailable(host);
    CHECK(outcome.end == DrainPassEnd::TimeBudget);
    CHECK(!outcome.caughtUp && !outcome.reachedEnd);
    CHECK_EQ(outcome.bytesDrained, 4 * 1024);
    CHECK_EQ(pipe->toRead.size() - pipe->readPosition, 4 * 1024u);
    CHECK_EQ(host.frames, framesBefore + 1);
    CHECK(host.reasons.back() == SyncFrameReason::ContinuingPass);
    CHECK(runtime.lastFrameClass() == FrameClass::Flood);
    CHECK(!runtime.isDrainSaturated());
    CHECK_EQ(host.locks, host.unlocks);
    CHECK(host.didUnlockHeldFrame);
}

TEST(onlyAnElapsedPassBudgetReleasesAnIncompleteCommand)
{
    for (const auto end : { DrainPassEnd::OwnerFence, DrainPassEnd::None, DrainPassEnd::TimeBudget }) {
        TerminalRuntime runtime;
        HandClock clock;
        clock.drive(runtime);
        runtime.init(parameters80x24());
        auto* pipe = attachPipe(runtime);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        pipe->toRead = "ready";
        REQUIRE(runtime.drainAvailable(host).caughtUp);
        settleHeldFrame(runtime, host, clock);
        clock.nowMicroseconds += refreshIntervalMicroseconds;
        const int framesBefore = host.frames;
        constexpr std::string_view prefix = "painted\x1b[48;5;";
        host.lockModel();
        Ubiquitty::processBytes(runtime.borrowTermForRender(), prefix);
        host.unlockModel();
        const auto outcome = runtime.finishExternalDrainPass(host, static_cast<int>(prefix.size()), end);
        CHECK(!outcome.caughtUp && !outcome.reachedEnd);
        CHECK(!runtime.isDrainSaturated());
        const auto due = runtime.publishHeldFrameIfDue(host);
        if (end == DrainPassEnd::TimeBudget) {
            CHECK(!due);
            CHECK_EQ(host.frames, framesBefore + 1);
            CHECK(runtime.lastFrameClass() == FrameClass::Forced);
            CHECK(host.reasons.back() == SyncFrameReason::ContinuingPass);
        } else {
            CHECK(due == coherenceDeadlineMicroseconds);
            CHECK_EQ(host.frames, framesBefore);
            CHECK(runtime.isFrameHeld());
        }
        CHECK_EQ(host.locks, host.unlocks);
    }
}

// A flood that opens with a whole-screen erase, or with the alternate screen's
// clearing entry, does not go quiet behind it: outside a live resize every pass
// that spends its time budget is a TimeBudget pass, and the rule takes its
// committed grid mid-command once per frame interval for as long as the flood
// lasts, as it does for any other flood.
TEST(aFloodBehindAWholeScreenEraseKeepsItsTimeBudgetFrames)
{
    for (const std::string_view opening : { std::string_view("\x1b[H\x1b[2J"), std::string_view("\x1b[?1049h") }) {
        TerminalRuntime runtime;
        HandClock clock;
        clock.drive(runtime);
        runtime.init(parameters80x24());
        auto* pipe = attachPipe(runtime);
        REQUIRE(pipe != nullptr);
        CountingDrainHost host;
        pipe->toRead = "ready";
        REQUIRE(runtime.drainAvailable(host).caughtUp);
        settleHeldFrame(runtime, host, clock);
        const std::uint32_t erasesBefore = runtime.borrowTermForRender()->eraseDisplayGeneration;
        const int framesBefore = host.frames;
        constexpr int intervals = 12;
        for (int pass = 0; pass < intervals; ++pass) {
            clock.nowMicroseconds += refreshIntervalMicroseconds;
            std::string bytes = pass == 0 ? std::string(opening) : std::string("196m ");
            bytes += "flooded row\x1b[48;5;";
            host.lockModel();
            Ubiquitty::processBytes(runtime.borrowTermForRender(), bytes);
            host.unlockModel();
            const auto outcome = runtime.finishExternalDrainPass(host, static_cast<int>(bytes.size()), DrainPassEnd::TimeBudget);
            CHECK(!outcome.caughtUp && !outcome.reachedEnd);
            CHECK_MSG(host.frames == framesBefore + pass + 1, "each elapsed budget of the flood takes its frame");
            CHECK(runtime.lastFrameClass() == FrameClass::Forced);
            CHECK(host.reasons.back() == SyncFrameReason::ContinuingPass);
        }
        CHECK_MSG(runtime.borrowTermForRender()->eraseDisplayGeneration != erasesBefore, "the flood opened with a whole-screen erase");
        CHECK_EQ(host.locks, host.unlocks);
    }
}

// During a live resize the drain takes no frame of a redraw still arriving
// behind a whole-screen erase, at its budget or at ground, so the boundary
// route cannot present what the resize's capture holds back. The frame follows
// once the drain has caught up, and the gate ends with the resize.
TEST(aLiveResizeTakesNoDrainFrameOfARedrawStillArriving)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    CountingDrainHost host;
    Ubiquitty::PAL::s_fakeMicroseconds = 1'000'000;
    pipe->toRead = "ready";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    const auto parse = [&](std::string_view bytes, DrainPassEnd end) {
        (void)runtime.feedChunkUpToSyncClear(bytes);
        (void)runtime.finishExternalDrainPass(host, static_cast<int>(bytes.size()), end);
    };
    const int framesBefore = host.frames;

    runtime.setLiveResize(LiveResize::Active);
    clock.nowMicroseconds += refreshIntervalMicroseconds;
    parse("\x1b[H\x1b[2Jpainted\x1b[48;5;", DrainPassEnd::TimeBudget);
    CHECK_MSG(host.frames == framesBefore, "the budget takes no frame of the erased screen");
    CHECK(runtime.isFrameHeld());
    clock.nowMicroseconds += refreshIntervalMicroseconds;
    parse("196m more rows", DrainPassEnd::TimeBudget);
    CHECK_MSG(host.frames == framesBefore, "nor does a pass that ends at ground while the redraw is arriving");

    parse(" last row", DrainPassEnd::WouldBlock);
    settleHeldFrame(runtime, host, clock);
    CHECK_MSG(host.frames == framesBefore + 1, "the redraw is taken once the drain has caught up with it");

    runtime.setLiveResize(LiveResize::Idle);
    clock.nowMicroseconds += refreshIntervalMicroseconds;
    parse("\x1b[H\x1b[2Jagain\x1b[48;5;", DrainPassEnd::TimeBudget);
    CHECK_MSG(host.frames == framesBefore + 2, "with the resize over, the budget takes its frame again");
    CHECK(runtime.lastFrameClass() == FrameClass::Forced);
    CHECK_EQ(host.locks, host.unlocks);
}

TEST(aTimedReadRearmsTheDirtyEpochAfterAMidReadSynchronizedFrame)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    TimedDrainHost host(clock);
    host.observeClock(runtime);
    pipe->toRead = "ready";
    REQUIRE(runtime.drainAvailable(host).caughtUp);
    settleHeldFrame(runtime, host, clock);
    clock.nowMicroseconds += host.interval;
    const auto before = host.reasons.size();
    pipe->toRead += "\x1b[?2026hfirst\x1b[?2026lsecond";
    host.biteMicroseconds = host.interval;
    const auto outcome = runtime.drainAvailable(host);
    CHECK(outcome.end == DrainPassEnd::TimeBudget);
    REQUIRE(host.reasons.size() == before + 2);
    CHECK(host.reasons[before] == SyncFrameReason::Boundary);
    CHECK(host.reasons[before + 1] == SyncFrameReason::ContinuingPass);
    CHECK(!runtime.isFrameHeld());
    CHECK_EQ(host.locks, host.unlocks);
}

TEST(aTimedDrainFinishesItsReadBeforeReturningAndPreservesTheNextQuery)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    TimedDrainHost host(clock);
    host.observeClock(runtime);
    host.biteMicroseconds = host.interval;
    pipe->toRead.assign(drainReadBufferBytes, 'x');
    pipe->toRead += "\x1b[6n";
    const auto first = runtime.drainAvailable(host);
    CHECK(first.end == DrainPassEnd::TimeBudget);
    CHECK_EQ(first.bytesDrained, drainReadBufferBytes);
    CHECK_EQ(pipe->readPosition, static_cast<std::size_t>(drainReadBufferBytes));
    CHECK(pipe->written.empty());
    CHECK_EQ(runtime.borrowTermForRender()->screens.active->cursorX, 16);
    const auto second = runtime.drainAvailable(host);
    CHECK(second.end == DrainPassEnd::TimeBudget);
    CHECK_EQ(second.bytesDrained, 4);
    CHECK(pipe->written == "\x1b[24;17R");
    const auto quiet = runtime.drainAvailable(host);
    CHECK(quiet.caughtUp && quiet.end == DrainPassEnd::WouldBlock);
    CHECK_EQ(host.locks, host.unlocks);
}

TEST(anUnpacedDrainKeepsItsByteBudgetDespiteElapsedTime)
{
    TerminalRuntime runtime;
    HandClock clock;
    clock.drive(runtime);
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe != nullptr);
    TimedDrainHost host(clock);
    host.observeClock(runtime);
    host.interval = 0;
    host.biteMicroseconds = 8'000;
    pipe->toRead.assign(TerminalRuntime::drainPassCapacityBytes + 1, 'x');
    const auto outcome = runtime.drainAvailable(host);
    CHECK(outcome.end == DrainPassEnd::ByteBudget);
    CHECK_EQ(outcome.bytesDrained, TerminalRuntime::drainPassCapacityBytes);
    CHECK(runtime.isDrainSaturated());
    CHECK_EQ(pipe->toRead.size() - pipe->readPosition, 1u);
}

namespace {

// A measurer that answers every run as one glyph three cells wide and counts
// how often it is asked, so a session that lost it answers with the grid
// face and a count that never moves.
struct CountingMeasurer {
    int calls = 0;
    static int measure(void* context, const std::uint32_t*, int count, std::uint8_t, std::uint32_t, Ubiquitty::ProportionalShapedGlyph* glyphsOut, int capacity, float* cellAdvanceOut)
    {
        auto* self = static_cast<CountingMeasurer*>(context);
        ++self->calls;
        if (count <= 0 || capacity < 1)
            return 0;
        glyphsOut[0] = Ubiquitty::ProportionalShapedGlyph {};
        glyphsOut[0].advanceInPixels = 24.0f;
        if (cellAdvanceOut)
            *cellAdvanceOut = 8.0f;
        return 1;
    }
};

} // namespace

// A frontend installs its measurer when its renderer arrives, which can be
// before the session starts its model and before an adoption swaps it: the
// measurer belongs to the session, and every terminal the session comes to
// hold must answer through it, or a proportional run is laid out with the
// grid face and a drag inside it lands on whole cells.
TEST(theTextMeasurerReachesATerminalStartedOrAdoptedAfterItWasSet)
{
    const std::uint32_t text[] = { 'a', 'b', 'c' };

    CountingMeasurer beforeStart;
    const std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> session(
        TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->setTextMeasurer(CountingMeasurer::measure, &beforeStart);
    session->start(24, 80);
    CHECK_EQ(session->proportionalRunCells(Ubiquitty::Runtime::TerminalSession::CodePointSpan(text)), 3);
    CHECK_EQ(beforeStart.calls, 1);

    // An adopted terminal answers through the same measurer, and the one the
    // session left no longer reaches it.
    CountingMeasurer adopted;
    const std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)> host(
        TerminalSession::make(), releaseTerminalSession);
    REQUIRE(host != nullptr);
    host->setTextMeasurer(CountingMeasurer::measure, &adopted);
    Ubiquitty::Terminal external;
    hardReset(external);
    resize(external, 24, 80);
    external.drawsSizedText = true;
    external.drawsProportionalText = true;
    std::mutex externalLock;
    host->adoptExternalTerminal(&external, reinterpret_cast<std::uintptr_t>(&externalLock));
    CHECK_EQ(host->proportionalRunCells(Ubiquitty::Runtime::TerminalSession::CodePointSpan(text)), 3);
    CHECK_EQ(adopted.calls, 1);
    host->adoptExternalTerminal(nullptr, 0);
    CHECK(external.textMeasureClient.get() == nullptr);
}

// A working or blocked record ends when the shell has the terminal back. A
// shell with the integration says so with a prompt mark; one without marks
// nothing, and the record of a program that was interrupted would otherwise
// stand for as long as the pane does.
TEST(programActivityEndsWhenTheForegroundReturnsToAShellThatMarksNoPrompt)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe);
    CountingDrainHost host;
    const auto records = [&runtime] { return runtime.borrowTermForRender()->programStatus.records.size(); };

    // The foreground facts are probed on a tick budget, so each step moves the
    // tick past it.
    setenv("UBIQUITTY_FAKE_TICKS", "100", 1);
    pipe->foregroundShell = false;
    pipe->foregroundGroup = 222;
    pipe->toRead = "\x1b]7501;id=job:state=blocked:kind=question:msg=Q29udGludWU/\x1b\\";
    (void)runtime.drainAvailable(host);
    REQUIRE(runtime.borrowTermForRender());
    CHECK(records() == 1);

    setenv("UBIQUITTY_FAKE_TICKS", "200", 1);
    pipe->toRead += "still asking";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(records() == 1, "the record ended while its program still held the terminal");

    // Interrupted: the shell's group has the terminal and prints a bare prompt.
    setenv("UBIQUITTY_FAKE_TICKS", "300", 1);
    pipe->foregroundShell = true;
    pipe->foregroundGroup = 111;
    pipe->toRead += "^C\r\n$ ";
    (void)runtime.drainAvailable(host);
    unsetenv("UBIQUITTY_FAKE_TICKS");
    CHECK_MSG(records() == 0, "a blocked record outlived its program at a shell that marks no prompt");
    runtime.close();
}

// A local terminal's person looks at it when its frontend reports the pane
// focused: the bell and the notification it latched are seen then, and not
// when the pane loses the focus.
TEST(aFocusedPaneHasHadItsLatchedAttentionSeen)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe);
    CountingDrainHost host;
    const auto unseen = [&runtime] { return Ubiquitty::unseenAttention(runtime.borrowTermForRender()); };
    const auto focus = [&](bool isFocused) { // bool-ok: the command ring's own flag
        Command command;
        command.kind = CommandKind::SetFocus;
        command.flag = isFocused;
        CHECK(runtime.link().commands.post(command));
        CHECK(runtime.applyPendingCommands(host));
    };

    pipe->toRead = "\a\x1b]9;the build finished\a";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(unseen().hasBell && unseen().hasNotification, "premise: the bell and the notification were latched");

    focus(false);
    CHECK_MSG(unseen().hasBell && unseen().hasNotification, "losing the focus is nobody looking");

    focus(true);
    CHECK_MSG(!unseen().hasBell, "the focused pane's bell was seen");
    CHECK_MSG(!unseen().hasNotification && unseen().notificationBody.empty(), "the focused pane's notification was seen");

    // Still focused: what the program raises now is seen by the drain that
    // parsed it.
    pipe->toRead += "\a\x1b]9;the tests finished\a";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(!unseen().hasBell && !unseen().hasNotification, "a bell and a notification under the focus were seen as they arrived");

    focus(false);
    pipe->toRead += "\a\x1b]9;nobody is here\a";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(unseen().hasBell && unseen().hasNotification, "with the focus gone they latch again");
    runtime.close();
}

// A shell that marks its prompts ends a record at the mark and nowhere before
// it: a function of its own reports a state and then runs programs, and each
// one handing the terminal back is not the function's end.
TEST(programActivityOutlivesAProgramUnderAShellThatMarksItsPrompts)
{
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    auto* pipe = attachPipe(runtime);
    REQUIRE(pipe);
    CountingDrainHost host;
    const auto records = [&runtime] { return runtime.borrowTermForRender()->programStatus.records.size(); };

    setenv("UBIQUITTY_FAKE_TICKS", "100", 1);
    pipe->foregroundShell = true;
    pipe->foregroundGroup = 111;
    pipe->toRead = "\x1b]133;A\x07$ \x1b]7501;id=job:state=working\x1b\\";
    (void)runtime.drainAvailable(host);
    REQUIRE(runtime.borrowTermForRender());
    CHECK(records() == 1);

    // The function runs a program, which takes the terminal and gives it back.
    setenv("UBIQUITTY_FAKE_TICKS", "200", 1);
    pipe->foregroundShell = false;
    pipe->foregroundGroup = 222;
    pipe->toRead += "building";
    (void)runtime.drainAvailable(host);
    setenv("UBIQUITTY_FAKE_TICKS", "300", 1);
    pipe->foregroundShell = true;
    pipe->foregroundGroup = 111;
    pipe->toRead += " done\r\n";
    (void)runtime.drainAvailable(host);
    CHECK_MSG(records() == 1, "a program handing the terminal back ended its shell's record before the prompt");

    setenv("UBIQUITTY_FAKE_TICKS", "400", 1);
    pipe->toRead += "\x1b]133;A\x07$ ";
    (void)runtime.drainAvailable(host);
    unsetenv("UBIQUITTY_FAKE_TICKS");
    CHECK_MSG(records() == 0, "the record outlived the next prompt mark");
    runtime.close();
}

TEST(aBlockedProgramPublishesWhatItWaitsForAndNoOtherStatusDoes)
{
    const auto published = [](const char* report) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::processBytes(&terminal, report);
        Ubiquitty::Runtime::RuntimeSnapshot snapshot;
        Ubiquitty::Runtime::captureSnapshotFromTerm(&terminal, 0, snapshot);
        return snapshot.programBlockKind;
    };
    using Ubiquitty::OSC::ProgramBlockKind;
    CHECK_EQ(published("\033]7501;state=blocked:kind=question\a"), std::to_underlying(ProgramBlockKind::Question));
    CHECK_EQ(published("\033]7501;state=blocked:kind=auth\a"), std::to_underlying(ProgramBlockKind::Auth));
    CHECK_MSG(published("\033]7501;state=blocked\a") == std::to_underlying(ProgramBlockKind::None), "a blocked report that names no kind publishes none");
    CHECK_MSG(published("\033]7501;state=working:kind=permission\a") == 0, "a kind on a record that is not blocked is not published");
    CHECK_MSG(published("\033]7501;state=done\a") == 0, "nor is one on a record with none");
    CHECK_MSG(published("") == 0, "nor with no record at all");
}

TEST(aWorkingProgramPublishesItsPercentAndNoOtherStatusDoes)
{
    const auto published = [](const char* report) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::processBytes(&terminal, report);
        Ubiquitty::Runtime::RuntimeSnapshot snapshot;
        Ubiquitty::Runtime::captureSnapshotFromTerm(&terminal, 0, snapshot);
        return static_cast<int>(snapshot.programProgress);
    };
    CHECK_EQ(published("\033]7501;state=working:progress=40\a"), 40);
    CHECK_EQ(published("\033]7501;state=working:progress=0\a"), 0);
    CHECK_EQ(published("\033]7501;state=working:progress=100\a"), 100);
    CHECK_MSG(published("\033]7501;state=working\a") == -1, "working with no percent is progress of unknown length");
    CHECK_MSG(published("\033]7501;state=working:progress=101\a") == -1, "a percent past 100 is absent, as the protocol says");
    CHECK_MSG(published("\033]7501;state=blocked:progress=40\a") == -1, "a blocked program's percent waits with it; its symbol is what shows");
    CHECK_MSG(published("") == -1, "and with no record, none");
}

TEST(aStatusRecordPublishesTheProgramItsAppNames)
{
    const auto published = [](const char* report) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::processBytes(&terminal, report);
        Ubiquitty::Runtime::RuntimeSnapshot snapshot;
        Ubiquitty::Runtime::captureSnapshotFromTerm(&terminal, 0, snapshot);
        const auto* icon = Ubiquitty::Title::processIconAt(snapshot.reportedProcessIcon);
        return icon ? std::string(icon->key) : std::string();
    };
    CHECK_STR_EQ(published("\033]7501;state=working:app=claude-code\a"), "claude-code");
    CHECK_STR_EQ(published("\033]7501;id=s:state=idle:app=Codex\a\033]7501;id=s/t:state=blocked:kind=auth\a"), "codex");
    CHECK_MSG(published("\033]7501;state=working:app=build\a").empty(), "an app the catalog does not hold leaves it to the process");
    CHECK_MSG(published("").empty(), "and with no record, none");
}

TEST(programActivityPublishesTabStatusAndClearsAtPrompt)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::processBytes(&terminal, "\033]7501;state=blocked:kind=permission:app=build:msg=T0s=\a");
    Ubiquitty::Runtime::RuntimeSnapshot snapshot;
    Ubiquitty::Runtime::captureSnapshotFromTerm(&terminal, 0, snapshot);
    CHECK(snapshot.isTabStatusColorSet);
    CHECK_EQ(snapshot.programStatusKind, std::to_underlying(Ubiquitty::App::StatusKind::Blocked));
    CHECK_EQ(snapshot.programBlockKind, std::to_underlying(Ubiquitty::OSC::ProgramBlockKind::Permission));
    CHECK_STR_EQ(snapshot.tabStatusLabel(), "build: Approval needed — OK");
    const auto generation = terminal.tabStatusGeneration;
    Ubiquitty::processBytes(&terminal, "\033]133;A\a");
    CHECK(terminal.tabStatusGeneration > generation);
    Ubiquitty::Runtime::captureSnapshotFromTerm(&terminal, 0, snapshot);
    CHECK(!snapshot.isTabStatusColorSet);
    CHECK(snapshot.tabStatusLabel().empty());
    CHECK_EQ(snapshot.programStatusKind, 0);
}

namespace Ubiquitty::Runtime {

// Injects only the remembered ingest fact. No thread or endpoint is created;
// the stream double counts a forbidden restart before refusing its duplicate.
struct LocalAdoptionTestAccess {
    static void notePriorIngest(TerminalSession& session)
    {
        session.m_localAdoption->shouldResumeIngest = true;
    }
};

// Creates only layout and retained bindings, bypassing the production launch
// factory. The close methods under test are the actual workspace methods.
struct WorkspaceSessionTestAccess {
    static Session::Layout::SurfaceID addSurface(WorkspaceSession& workspace, TerminalSession& session)
    {
        const auto surface = workspace.m_workspace.addSurface();
        if (surface == Session::Layout::invalidSurface)
            return surface;
        const auto pane = workspace.m_workspace.focusedPane(surface);
        retainTerminalSession(&session);
        workspace.bind(surface, pane, &session);
        return surface;
    }

    static Session::Layout::PaneID split(WorkspaceSession& workspace, Session::Layout::SurfaceID surface, TerminalSession& session)
    {
        const auto pane = workspace.m_workspace.splitFocusedPane(surface, Panes::Orientation::Vertical);
        if (pane == Session::Layout::invalidPane)
            return pane;
        retainTerminalSession(&session);
        workspace.bind(surface, pane, &session);
        return pane;
    }

    static Session::Layout::TabID addTab(WorkspaceSession& workspace, Session::Layout::SurfaceID surface, TerminalSession& session)
    {
        const auto tab = workspace.m_workspace.addTab(surface);
        if (tab == Session::Layout::invalidTab)
            return tab;
        retainTerminalSession(&session);
        workspace.bind(surface, workspace.m_workspace.focusedPane(surface), &session);
        return tab;
    }

    static void notePendingRestart(WorkspaceSession& workspace, Session::Layout::PaneID pane)
    {
        for (auto& binding : workspace.m_bindings) {
            if (binding.pane == pane && binding.session)
                binding.isRestartPending = true;
        }
    }

    static bool isRestartPending(const WorkspaceSession& workspace, Session::Layout::PaneID pane)
    {
        for (const auto& binding : workspace.m_bindings) {
            if (binding.pane == pane && binding.session)
                return binding.isRestartPending;
        }
        return false;
    }
};

} // namespace Ubiquitty::Runtime

namespace {

using Ubiquitty::Session::AdoptClientAttempt;
using Ubiquitty::Session::AdoptDisposition;
using Ubiquitty::Session::ShareHandoffResult;
using Ubiquitty::Session::ShareOwnership;

ShareHandoffResult unresolvedLocalAdoption(const char*, std::span<const std::byte> body, int descriptor, const Ubiquitty::Session::ShareHandoffLimits& limits)
{
    CHECK(!body.empty());
    CHECK(descriptor == 42);
    CHECK(limits.maxRecordBytes > body.size());
    CHECK(TerminalSession::unresolvedLocalAdoptionCount() > 0);
    auto attempt = std::make_unique<AdoptClientAttempt>(Ubiquitty::Session::AdoptOperation { { 51, 61 }, 71 });
    const Ubiquitty::Session::AdoptOutcome prepared { attempt->operation(), AdoptDisposition::Prepared, { 81, { 81, 71 } }, {} };
    CHECK(attempt->observe(prepared).has_value());
    CHECK(attempt->beginCommit().has_value());
    return { .ownership = ShareOwnership::Unresolved, .adopted = {}, .attempt = std::move(attempt) };
}

ShareHandoffResult abortLocalAdoption(const char*, std::unique_ptr<AdoptClientAttempt> attempt, const Ubiquitty::Session::ShareHandoffLimits&)
{
    if (!attempt)
        return { .ownership = ShareOwnership::Unresolved };
    const auto accepted = attempt->observe({ attempt->operation(), AdoptDisposition::Aborted, attempt->target(), {} });
    CHECK(accepted.has_value());
    return { .ownership = accepted ? ShareOwnership::Local : ShareOwnership::Unresolved, .adopted = {}, .attempt = std::move(attempt) };
}

ShareHandoffResult malformedLocalOwnership(const char* socket, std::span<const std::byte> body, int descriptor, const Ubiquitty::Session::ShareHandoffLimits& limits)
{
    auto result = unresolvedLocalAdoption(socket, body, descriptor, limits);
    result.ownership = static_cast<ShareOwnership>(255);
    return result;
}

ShareHandoffResult noOwnershipProof(const char*, std::unique_ptr<AdoptClientAttempt> attempt, const Ubiquitty::Session::ShareHandoffLimits&)
{
    if (!attempt)
        return { .ownership = ShareOwnership::Unresolved };
    CHECK(attempt->observe({ attempt->operation(), AdoptDisposition::Unknown, {}, {} }).has_value());
    CHECK(attempt->observe({ attempt->operation(), AdoptDisposition::Prepared, attempt->target(), {} }).has_value());
    auto wrongDaemon = attempt->operation();
    ++wrongDaemon.daemonInstanceID;
    CHECK(!attempt->observe({ wrongDaemon, AdoptDisposition::Aborted, attempt->target(), {} }).has_value());
    CHECK(!attempt->mayResumeLocal());
    // Even an incorrectly classified platform refusal cannot relax the
    // attempt after a possible Commit without exact terminal evidence.
    return { .ownership = ShareOwnership::Local, .adopted = {}, .attempt = std::move(attempt) };
}

ShareHandoffResult commitLocalAdoption(const char*, std::unique_ptr<AdoptClientAttempt> attempt, const Ubiquitty::Session::ShareHandoffLimits&)
{
    if (!attempt)
        return { .ownership = ShareOwnership::Unresolved };
    const auto accepted = attempt->observe({ attempt->operation(), AdoptDisposition::Committed, attempt->target(), {} });
    CHECK(accepted.has_value());
    const Ubiquitty::Session::AdoptedSession adopted { attempt->target().sessionID, attempt->operation().daemonInstanceID };
    return { .ownership = accepted ? ShareOwnership::Committed : ShareOwnership::Unresolved, .adopted = adopted, .attempt = std::move(attempt) };
}

struct LocalAdoptionFixture {
    RetainedTerminalSession session { TerminalSession::make() };
    Ubiquitty::PAL::StreamTestPipe* pipe = nullptr;

    LocalAdoptionFixture()
    {
        if (!session)
            return;
        session->start(24, 80);
        auto stream = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
        stream->scriptedFileDescriptor = 42;
        stream->runningChild = true;
        auto* borrowed = stream.get();
        auto installed = session->adoptStream(std::move(stream));
        CHECK(installed.has_value());
        if (installed)
            pipe = borrowed;
    }

    ~LocalAdoptionFixture()
    {
        if (session) {
            if (session->hasUnresolvedLocalAdoption())
                (void)session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&abortLocalAdoption));
            session->close();
        }
    }

    [[nodiscard]] std::uint32_t begin()
    {
        return session->handOverToLocalDaemon("injected-no-socket", reinterpret_cast<std::uintptr_t>(&unresolvedLocalAdoption));
    }
};

} // namespace

TEST(anUnresolvedLocalAdoptionFreezesEveryLocalIOPathAndRefusesTeardown)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    REQUIRE(fixture.session->terminal());
    const auto destroyedBefore = Ubiquitty::PAL::g_streamDestroyCount;
    const auto dimensionsBefore = fixture.session->dimensions();
    const auto sizeBefore = fixture.pipe->windowColumns;
    CHECK(fixture.begin() == 0);
    REQUIRE(fixture.session->hasUnresolvedLocalAdoption());
    CHECK(!fixture.session->canClose());
    CHECK(!fixture.session->tryClose().has_value());
    fixture.session->close();
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);
    CHECK(fixture.session->stream() == nullptr);
    CHECK(fixture.session->fileDescriptor() == -1);
    CHECK(!fixture.session->startIngest());
    CHECK(!fixture.session->launchShellCommand("must not launch"));
    CHECK(!fixture.session->hasHostInputSink());
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::KeepRunning);

    auto replacement = std::make_unique<Ubiquitty::PAL::StreamTestPipe>();
    auto* const replacementBorrow = replacement.get();
    auto refused = fixture.session->adoptStream(std::move(replacement));
    REQUIRE(!refused.has_value());
    CHECK(refused.error().get() == replacementBorrow);
    CHECK(replacementBorrow->treeNotifierInstallations == 0);
    CHECK(replacementBorrow->windowColumns == -1);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);

    fixture.pipe->pendingWrite = true;
    constexpr std::string_view blockedInput = "blocked input";
    constexpr std::string_view blockedEcho = "blocked echo";
    fixture.session->sendInput({ blockedInput.data(), blockedInput.size() });
    fixture.session->feedBytes({ blockedEcho.data(), blockedEcho.size() });
    Ubiquitty::processBytes(fixture.session->terminal(), "\x1b[6n");
    const auto queuedReplies = fixture.session->terminal()->outputLength;
    REQUIRE(queuedReplies > 0);
    fixture.session->flushReplies();
    CHECK(fixture.session->terminal()->outputLength == queuedReplies);
    CHECK(!fixture.session->drainPendingWrite());
    CHECK(fixture.pipe->pendingWriteDrainCalls == 0);
    CHECK(fixture.pipe->pendingWrite);
    CHECK(fixture.pipe->written.empty());
    CHECK(fixture.session->drain().bytesDrained == 0);
    CHECK(fixture.pipe->readCallCount == 0);
    fixture.session->resizeGrid(40, 120, Ubiquitty::Modes::ReflowMode::Fixed);
    fixture.session->setWindowSize(120, 40, 1200, 800);
    CHECK(fixture.session->dimensions().rows == dimensionsBefore.rows);
    CHECK(fixture.session->dimensions().columns == dimensionsBefore.columns);
    CHECK(fixture.pipe->windowColumns == sizeBefore);

    CHECK(fixture.session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&noOwnershipProof)) == ShareOwnership::Unresolved);
    CHECK(fixture.session->hasUnresolvedLocalAdoption());
    CHECK(TerminalSession::takeResolvedLocalAdoption(fixture.session->localAdoptionEscrowID()) == nullptr);
    CHECK(fixture.session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&abortLocalAdoption)) == ShareOwnership::Local);
    CHECK(fixture.session->canClose());
    CHECK(fixture.session->stream() == fixture.pipe);
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::Terminate);
}

TEST(anEscrowRetainsTheMasterAfterTheViewReleasesItsOwnerUntilExplicitLocalTake)
{
    const int destroyedBefore = Ubiquitty::PAL::g_streamDestroyCount;
    std::uint64_t id = 0;
    Ubiquitty::PAL::StreamTestPipe* retainedPipe = nullptr;
    {
        LocalAdoptionFixture fixture;
        REQUIRE(fixture.pipe);
        CHECK(fixture.begin() == 0);
        REQUIRE(fixture.session->hasUnresolvedLocalAdoption());
        Ubiquitty::Runtime::LocalAdoptionTestAccess::notePriorIngest(*fixture.session);
        retainedPipe = fixture.pipe;
        id = fixture.session->localAdoptionEscrowID();
        REQUIRE(id != 0);
        fixture.session.reset();
    }
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);
    CHECK(TerminalSession::resolveLocalAdoptionEscrow(id, reinterpret_cast<std::uintptr_t>(&noOwnershipProof)) == ShareOwnership::Unresolved);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);
    CHECK(TerminalSession::resolveLocalAdoptionEscrow(id, reinterpret_cast<std::uintptr_t>(&abortLocalAdoption)) == ShareOwnership::Local);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore);
    CHECK(retainedPipe->duplicateCalls == 0);
    RetainedTerminalSession reopened { TerminalSession::takeResolvedLocalAdoption(id) };
    REQUIRE(reopened != nullptr);
    CHECK(reopened->canClose());
    CHECK(reopened->shouldResumeLocalAdoptionIngest());
    constexpr std::string_view input = "reopened";
    reopened->sendInput({ input.data(), input.size() });
    CHECK_STR_EQ(static_cast<Ubiquitty::PAL::StreamTestPipe*>(reopened->stream())->written, "reopened");
    reopened->close();
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyedBefore + 1);
    CHECK(TerminalSession::takeResolvedLocalAdoption(id) == nullptr);
}

// A shared window's panes join one connection only once each handoff is
// proven committed. A pane whose Commit is unresolved joins nothing and keeps
// its escrowed owner exactly; resolved as committed, it is shared with no
// client until a window's anchor or a connection of its own takes it.
TEST(anUnresolvedAdoptionJoinsNoSharedWindowAndKeepsItsEscrow)
{
    LocalAdoptionFixture anchor;
    LocalAdoptionFixture member;
    REQUIRE(anchor.pipe && member.pipe);
    CHECK(member.begin() == 0);
    REQUIRE(member.session->hasUnresolvedLocalAdoption());
    const std::uint64_t escrowID = member.session->localAdoptionEscrowID();
    const std::uint32_t unresolvedBefore = TerminalSession::unresolvedLocalAdoptionCount();
    REQUIRE(escrowID != 0);
    CHECK(member.session->joinSharedLocalWindow(*anchor.session, discardingLoop())
        == TerminalSession::SharedWindowJoin::Refused);
    CHECK(member.session->hasUnresolvedLocalAdoption());
    CHECK(!member.session->canClose());
    CHECK(!member.session->isRemoteAttached());
    CHECK_EQ(member.session->localAdoptionEscrowID(), escrowID);
    CHECK_EQ(TerminalSession::unresolvedLocalAdoptionCount(), unresolvedBefore);
    CHECK(!member.session->attachSharedLocalWindow(0, discardingLoop()));
    CHECK(member.session->hasUnresolvedLocalAdoption());

    CHECK(member.session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&commitLocalAdoption)) == ShareOwnership::Committed);
    CHECK(member.session->isSharedIntoLocalDaemon());
    CHECK(!member.session->isRemoteAttached());
    CHECK_MSG(member.session->joinSharedLocalWindow(*anchor.session, discardingLoop())
            == TerminalSession::SharedWindowJoin::Refused,
        "a committed pane joined a session that anchors no shared window");
    CHECK(member.session->isSharedIntoLocalDaemon());
    CHECK(!member.session->isRemoteAttached());
}

TEST(exactCommitPermitsCloseButNeverResumesTheOriginalLocalStream)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    CHECK(fixture.begin() == 0);
    REQUIRE(fixture.session->hasUnresolvedLocalAdoption());
    CHECK(fixture.session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&commitLocalAdoption)) == ShareOwnership::Committed);
    CHECK(!fixture.session->hasUnresolvedLocalAdoption());
    CHECK(fixture.session->canClose());
    CHECK(fixture.session->sharedSessionID() == 81);
    CHECK(fixture.session->remoteDaemonInstanceID() == 71);
    CHECK(fixture.session->stream() == nullptr);
    CHECK(!fixture.session->startIngest());
    constexpr std::string_view input = "still blocked";
    fixture.session->sendInput({ input.data(), input.size() });
    CHECK(fixture.pipe->written.empty());
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::KeepRunning);
}

TEST(pendingLocalInputRefusesAdoptionBeforeAnyOwnershipFreeze)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    fixture.pipe->pendingWrite = true;
    CHECK(fixture.begin() == 0);
    CHECK(!fixture.session->hasUnresolvedLocalAdoption());
    CHECK(fixture.session->localAdoptionEscrowID() == 0);
    CHECK(fixture.session->stream() == fixture.pipe);
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::Terminate);
    CHECK(fixture.pipe->pendingWriteDrainCalls == 0);
    CHECK(fixture.pipe->pendingWrite);
}

TEST(anInvalidPlatformOwnershipValueCannotResumeTheOriginalStream)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    CHECK(fixture.session->handOverToLocalDaemon("injected-no-socket", reinterpret_cast<std::uintptr_t>(&malformedLocalOwnership)) == 0);
    CHECK(fixture.session->hasUnresolvedLocalAdoption());
    CHECK(!fixture.session->canClose());
    CHECK(fixture.session->stream() == nullptr);
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::KeepRunning);
}

TEST(anUnresolvedAdoptionCannotRebindItsActiveModelOrInstallBorrowedInputRoutes)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    auto* const originalModel = fixture.session->terminal();
    auto* const originalLock = fixture.session->modelLockPointer();
    const auto referencesBefore = fixture.session->swiftReferences.load(std::memory_order_acquire);
    CHECK(fixture.begin() == 0);
    REQUIRE(fixture.session->hasUnresolvedLocalAdoption());
    CHECK(fixture.session->swiftReferences.load(std::memory_order_acquire) == referencesBefore + 1);

    Ubiquitty::Terminal external;
    Ubiquitty::resize(external, 3, 5, Ubiquitty::Modes::ReflowMode::Fixed);
    std::mutex externalLock;
    fixture.session->adoptExternalTerminal(&external, reinterpret_cast<std::uintptr_t>(&externalLock));
    CHECK(fixture.session->terminal() == originalModel);
    CHECK(fixture.session->modelLockPointer() == originalLock);
    CHECK(!external.windowClient.installed());
    CHECK(external.shellClient.get() == nullptr);
    CHECK(external.triggerClient.get() == nullptr);
    CHECK(external.textMeasureClient.get() == nullptr);
    CHECK(fixture.session->swiftReferences.load(std::memory_order_acquire) == referencesBefore + 1);

    Ubiquitty::Protocol::Tmux::TmuxClient client;
    fixture.session->routeInputViaTmux(&client, fixture.pipe, 7);
    CHECK(!fixture.session->hasHostInputSink());
    fixture.session->routeInputViaTmux(nullptr, nullptr, -1);
    fixture.session->endExternalAdoption();
}

TEST(localAdoptionEscrowExhaustionRefusesBeforeTouchingTheNextOwner)
{
    std::array<std::unique_ptr<LocalAdoptionFixture>, LocalAdoptionEscrow::capacity + 1> fixtures;
    for (std::size_t index = 0; index < LocalAdoptionEscrow::capacity; ++index) {
        fixtures[index] = std::make_unique<LocalAdoptionFixture>();
        REQUIRE(fixtures[index]->pipe);
        CHECK(fixtures[index]->begin() == 0);
        REQUIRE(fixtures[index]->session->hasUnresolvedLocalAdoption());
    }
    CHECK(TerminalSession::unresolvedLocalAdoptionCount() == LocalAdoptionEscrow::capacity);
    fixtures.back() = std::make_unique<LocalAdoptionFixture>();
    REQUIRE(fixtures.back()->pipe);
    CHECK(fixtures.back()->begin() == 0);
    CHECK(!fixtures.back()->session->hasUnresolvedLocalAdoption());
    CHECK(fixtures.back()->session->localAdoptionEscrowID() == 0);
    CHECK(fixtures.back()->session->stream() == fixtures.back()->pipe);
    CHECK(fixtures.back()->pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::Terminate);
    CHECK(TerminalSession::unresolvedLocalAdoptionCount() == LocalAdoptionEscrow::capacity);
}

TEST(unfinishedTerminalCaptureRestoresLocalOwnershipWithoutSending)
{
    LocalAdoptionFixture fixture;
    REQUIRE(fixture.pipe);
    constexpr std::string_view unfinished = "\x1b[";
    fixture.session->feedBytes({ unfinished.data(), unfinished.size() });
    CHECK(fixture.begin() == 0);
    CHECK(!fixture.session->hasUnresolvedLocalAdoption());
    CHECK(fixture.session->localAdoptionEscrowID() == 0);
    CHECK(fixture.session->stream() == fixture.pipe);
    CHECK(fixture.pipe->closePolicy == Ubiquitty::PAL::ProcessClosePolicy::Terminate);
    CHECK(fixture.pipe->readCallCount == 0);
}

namespace {

using WorkspaceHandle = std::unique_ptr<WorkspaceSession, decltype(&releaseWorkspaceSession)>;

[[nodiscard]] WorkspaceHandle makeMemoryWorkspace()
{
    return WorkspaceHandle { WorkspaceSession::make(WorkspaceSession::IngestPolicy::CallerDrained, DrawnText::SizedAndProportional), &releaseWorkspaceSession };
}

} // namespace

TEST(anUnresolvedPaneClosePreservesWorkspaceLayoutAndBindings)
{
    LocalAdoptionFixture frozen;
    LocalAdoptionFixture sibling;
    REQUIRE(frozen.pipe && sibling.pipe);
    auto workspace = makeMemoryWorkspace();
    REQUIRE(workspace);
    const auto surface = WorkspaceSessionTestAccess::addSurface(*workspace, *frozen.session);
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto pane = workspace->focusedPane(surface);
    const auto otherPane = WorkspaceSessionTestAccess::split(*workspace, surface, *sibling.session);
    REQUIRE(otherPane != Ubiquitty::Session::Layout::invalidPane);
    REQUIRE(workspace->focusPane(surface, pane));
    REQUIRE(workspace->setDividerRatio(surface, workspace->paneTree(surface)->root(), 0.37f));
    REQUIRE(workspace->toggleZoomFocused(surface) == TerminalSession::PaneVerbOutcome::LocalActionRequired);
    const auto tab = workspace->activeTab(surface);
    const auto root = workspace->paneTree(surface)->root();
    const auto references = frozen.session->swiftReferences.load();
    CHECK(frozen.begin() == 0);
    REQUIRE(!frozen.session->canClose());
    const auto retainedReferences = frozen.session->swiftReferences.load();
    CHECK(retainedReferences == references + 1);
    const auto destroyed = Ubiquitty::PAL::g_streamDestroyCount;

    const auto result = workspace->closePane(surface, pane);
    CHECK(result.outcome == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(result.removedPane == Ubiquitty::Session::Layout::invalidPane);
    CHECK(result.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    CHECK(workspace->paneCount(surface) == 2);
    CHECK(workspace->activeTab(surface) == tab);
    CHECK(workspace->focusedPane(surface) == pane);
    CHECK(workspace->zoomedPane(surface) == pane);
    REQUIRE(workspace->paneTree(surface));
    CHECK(workspace->paneTree(surface)->root() == root);
    CHECK(workspace->paneTree(surface)->node(root).ratio == 0.37f);
    CHECK(workspace->session(pane) == frozen.session.get());
    CHECK(workspace->session(otherPane) == sibling.session.get());
    CHECK(frozen.session->swiftReferences.load() == retainedReferences);
    CHECK(sibling.session->stream() == sibling.pipe);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyed);

    // An unrelated local pane still closes; this is not a workspace-wide lock.
    const auto otherResult = workspace->closePane(surface, otherPane);
    CHECK(otherResult.outcome == TerminalSession::PaneVerbOutcome::LocalActionRequired);
    CHECK(otherResult.removedPane == otherPane);
    CHECK(workspace->paneCount(surface) == 1);
    CHECK(workspace->session(pane) == frozen.session.get());
}

TEST(anUnresolvedBackgroundTabCloseDoesNotReleaseAnyOfItsSiblings)
{
    LocalAdoptionFixture first;
    LocalAdoptionFixture frozen;
    LocalAdoptionFixture otherTab;
    REQUIRE(first.pipe && frozen.pipe && otherTab.pipe);
    auto workspace = makeMemoryWorkspace();
    REQUIRE(workspace);
    const auto surface = WorkspaceSessionTestAccess::addSurface(*workspace, *first.session);
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto firstPane = workspace->focusedPane(surface);
    const auto tab = workspace->activeTab(surface);
    const auto frozenPane = WorkspaceSessionTestAccess::split(*workspace, surface, *frozen.session);
    REQUIRE(frozenPane != Ubiquitty::Session::Layout::invalidPane);
    const auto activeTab = WorkspaceSessionTestAccess::addTab(*workspace, surface, *otherTab.session);
    REQUIRE(activeTab != Ubiquitty::Session::Layout::invalidTab);
    const auto activePane = workspace->focusedPane(surface);
    CHECK(frozen.begin() == 0);
    REQUIRE(!frozen.session->canClose());
    const auto firstReferences = first.session->swiftReferences.load();
    const auto frozenReferences = frozen.session->swiftReferences.load();
    const auto destroyed = Ubiquitty::PAL::g_streamDestroyCount;

    const auto result = workspace->closeTab(surface, tab);
    CHECK(result.outcome == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(result.removedPane == Ubiquitty::Session::Layout::invalidPane);
    CHECK(result.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    CHECK(workspace->tabCount(surface) == 2);
    CHECK(workspace->activeTab(surface) == activeTab);
    CHECK(workspace->focusedPane(surface) == activePane);
    REQUIRE(workspace->paneTree(surface, tab));
    CHECK(workspace->paneTree(surface, tab)->paneCount() == 2);
    CHECK(workspace->session(firstPane) == first.session.get());
    CHECK(workspace->session(frozenPane) == frozen.session.get());
    CHECK(workspace->session(activePane) == otherTab.session.get());
    CHECK(first.session->swiftReferences.load() == firstReferences);
    CHECK(frozen.session->swiftReferences.load() == frozenReferences);
    CHECK(first.session->stream() == first.pipe);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyed);

    const auto unrelated = workspace->closePane(surface, activePane);
    CHECK(unrelated.outcome == TerminalSession::PaneVerbOutcome::LocalActionRequired);
    CHECK(unrelated.removedPane == activePane);
    CHECK(unrelated.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    CHECK(workspace->tabCount(surface) == 1);
    CHECK(workspace->activeTab(surface) == tab);
    CHECK(workspace->session(frozenPane) == frozen.session.get());
}

TEST(anUnresolvedSurfaceClosePreflightsEveryTabBeforeRemovingAnyOwner)
{
    LocalAdoptionFixture first;
    LocalAdoptionFixture frozen;
    LocalAdoptionFixture otherSurface;
    REQUIRE(first.pipe && frozen.pipe && otherSurface.pipe);
    auto workspace = makeMemoryWorkspace();
    REQUIRE(workspace);
    const auto surface = WorkspaceSessionTestAccess::addSurface(*workspace, *first.session);
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto firstPane = workspace->focusedPane(surface);
    const auto firstTab = workspace->activeTab(surface);
    const auto frozenTab = WorkspaceSessionTestAccess::addTab(*workspace, surface, *frozen.session);
    REQUIRE(frozenTab != Ubiquitty::Session::Layout::invalidTab);
    const auto frozenPane = workspace->focusedPane(surface);
    REQUIRE(workspace->setActiveTab(surface, firstTab));
    const auto unrelated = WorkspaceSessionTestAccess::addSurface(*workspace, *otherSurface.session);
    REQUIRE(unrelated != Ubiquitty::Session::Layout::invalidSurface);
    const auto otherPane = workspace->focusedPane(unrelated);
    const auto surfaces = workspace->surfaceCount();
    CHECK(frozen.begin() == 0);
    REQUIRE(!frozen.session->canClose());
    const auto firstReferences = first.session->swiftReferences.load();
    const auto frozenReferences = frozen.session->swiftReferences.load();
    const auto destroyed = Ubiquitty::PAL::g_streamDestroyCount;

    workspace->closeWindowSurface(surface);
    CHECK(workspace->surfaceCount() == surfaces);
    CHECK(workspace->activeSurface() == unrelated);
    CHECK(workspace->tabCount(surface) == 2);
    CHECK(workspace->tabAt(surface, 0) == firstTab);
    CHECK(workspace->tabAt(surface, 1) == frozenTab);
    CHECK(workspace->activeTab(surface) == firstTab);
    CHECK(workspace->focusedPane(surface) == firstPane);
    CHECK(workspace->session(firstPane) == first.session.get());
    CHECK(workspace->session(frozenPane) == frozen.session.get());
    CHECK(workspace->session(otherPane) == otherSurface.session.get());
    CHECK(first.session->swiftReferences.load() == firstReferences);
    CHECK(frozen.session->swiftReferences.load() == frozenReferences);
    CHECK(first.session->stream() == first.pipe);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyed);

    workspace->closeWindowSurface(unrelated);
    CHECK(workspace->surfaceCount() == surfaces - 1);
    CHECK(workspace->session(otherPane) == nullptr);
    CHECK(workspace->session(frozenPane) == frozen.session.get());
}

TEST(anUnresolvedLastPaneCannotEscalateToTabOrSurfaceRemoval)
{
    LocalAdoptionFixture frozen;
    REQUIRE(frozen.pipe);
    auto workspace = makeMemoryWorkspace();
    REQUIRE(workspace);
    const auto surface = WorkspaceSessionTestAccess::addSurface(*workspace, *frozen.session);
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto pane = workspace->focusedPane(surface);
    const auto tab = workspace->activeTab(surface);
    const auto surfaces = workspace->surfaceCount();
    CHECK(frozen.begin() == 0);
    REQUIRE(!frozen.session->canClose());

    const auto paneResult = workspace->closePane(surface, pane);
    CHECK(paneResult.outcome == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(paneResult.removedPane == Ubiquitty::Session::Layout::invalidPane);
    CHECK(paneResult.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    const auto tabResult = workspace->closeTab(surface, tab);
    CHECK(tabResult.outcome == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(tabResult.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    CHECK(workspace->surfaceCount() == surfaces);
    CHECK(workspace->activeSurface() == surface);
    CHECK(workspace->activeTab(surface) == tab);
    CHECK(workspace->focusedPane(surface) == pane);
    CHECK(workspace->session(pane) == frozen.session.get());

    REQUIRE(frozen.session->resolveLocalAdoption(reinterpret_cast<std::uintptr_t>(&abortLocalAdoption)) == ShareOwnership::Local);
    // An invalid pane must not be mistaken for this tab's last live pane.
    const auto invalid = workspace->closePane(surface, Ubiquitty::Session::Layout::invalidPane);
    CHECK(invalid.outcome == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(invalid.removedPane == Ubiquitty::Session::Layout::invalidPane);
    CHECK(invalid.removedSurface == Ubiquitty::Session::Layout::invalidSurface);
    CHECK(workspace->session(pane) == frozen.session.get());
    CHECK(workspace->surfaceCount() == surfaces);
    const auto closed = workspace->closePane(surface, pane);
    CHECK(closed.outcome == TerminalSession::PaneVerbOutcome::LocalActionRequired);
    CHECK(closed.removedPane == pane);
    CHECK(closed.removedSurface == surface);
    CHECK(workspace->surfaceCount() == surfaces - 1);
    CHECK(workspace->session(pane) == nullptr);
}

TEST(anUnresolvedPaneRestartPreservesItsOwnerAndPendingIntent)
{
    LocalAdoptionFixture frozen;
    REQUIRE(frozen.pipe);
    auto workspace = makeMemoryWorkspace();
    REQUIRE(workspace);
    const auto surface = WorkspaceSessionTestAccess::addSurface(*workspace, *frozen.session);
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto pane = workspace->focusedPane(surface);
    WorkspaceSessionTestAccess::notePendingRestart(*workspace, pane);
    REQUIRE(WorkspaceSessionTestAccess::isRestartPending(*workspace, pane));
    CHECK(frozen.begin() == 0);
    REQUIRE(!frozen.session->canClose());
    const auto references = frozen.session->swiftReferences.load();
    const auto destroyed = Ubiquitty::PAL::g_streamDestroyCount;

    CHECK(!workspace->restartEndedPane(pane));
    CHECK(WorkspaceSessionTestAccess::isRestartPending(*workspace, pane));
    CHECK(workspace->session(pane) == frozen.session.get());
    CHECK(frozen.session->swiftReferences.load() == references);
    CHECK(Ubiquitty::PAL::g_streamDestroyCount == destroyed);
}

TEST(runtimeServicesCRTDeadlineWithoutARead)
{
    ::setenv("UBIQUITTY_FAKE_TICKS", "100", 1);
    TerminalRuntime runtime;
    runtime.init(parameters80x24());
    CountingDrainHost host;
    const auto off = runtime.serviceDisplayIdle(host);
    CHECK(!off.remainingTicks);
    CHECK(!off.didChange);
    runtime.feedHostBytes(std::string_view("\033[?97h"));
    const auto first = runtime.serviceDisplayIdle(host);
    CHECK(first.remainingTicks == 108000);
    CHECK(!first.didChange);
    ::setenv("UBIQUITTY_FAKE_TICKS", "108100", 1);
    const auto due = runtime.serviceDisplayIdle(host);
    CHECK(due.didChange);
    CHECK(!due.remainingTicks);
    CHECK(host.frames == 1);
    CHECK(host.locks == host.unlocks);
    CHECK(runtime.borrowTermForRender()->screenBlanked);
    CHECK(runtime.frameCount(FrameClass::Settled) == 1);
    ::unsetenv("UBIQUITTY_FAKE_TICKS");
}

TEST(inspectorReplacesAnswerbackButCannotChangeAMirror)
{
    auto* session = Ubiquitty::Runtime::TerminalSession::make();
    REQUIRE(session);
    session->start(2, 8);
    CHECK(session->setInspectorAnswerback("visible"));
    CHECK(session->inspectorAnswerbackDisplay() == "visible");
    session->lockForRead();
    auto* terminal = session->terminal();
    Ubiquitty::processBytes(terminal, "\033[?101h");
    session->unlockForRead();
    CHECK(session->inspectorAnswerbackDisplay() == "concealed");
    CHECK(session->inspectorInfo().answerbackDisplay == "concealed");
    CHECK(session->setInspectorAnswerback("replacement"));
    CHECK(session->inspectorAnswerbackDisplay() == "replacement");
    Ubiquitty::MirrorClient mirror;
    session->lockForRead();
    terminal->mirrorClient = &mirror;
    session->unlockForRead();
    CHECK(!session->setInspectorAnswerback("forbidden"));
    CHECK(session->inspectorAnswerbackDisplay().empty());
    CHECK(!session->inspectorInfo().canConfigureAnswerback);
    session->lockForRead();
    CHECK(terminal->answerback == "replacement");
    terminal->mirrorClient = nullptr;
    session->unlockForRead();
    releaseTerminalSession(session);
}

TEST(aSessionResizeResolvesTheViewportBeforeTheFrontendPublishesIt)
{
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(4, 12);
    std::string text;
    for (int line = 0; line < 30; ++line)
        text += std::to_string(line) + ":" + std::string(1 + line % 5 * 11, 'x') + "\r\n";
    session->feedBytes({ text.data(), text.size() });
    Ubiquitty::Selection::Session selection;
    const int offset = session->readSnapshot().history.retainedRows - 5;
    session->noteViewportBeforeResize(&selection, offset);
    REQUIRE(selection.viewportAnchor);
    const auto anchor = *selection.viewportAnchor;
    session->resize(4, 7, TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Top);
    const int resized = session->selectionViewportAfterResize(&selection, offset);
    session->lockForRead();
    const auto place = Ubiquitty::Screen::resolvePosition(*session->terminal(), anchor);
    const int history = Ubiquitty::Screen::scrollbackCount(session->terminal());
    session->unlockForRead();
    REQUIRE(place);
    CHECK_EQ(resized, history - place->absoluteRow);
    session->observeSelectionViewport(&selection, resized - 1);
    CHECK(!selection.viewportAnchor);
    CHECK(!selection.usesLogicalViewport);
}

TEST(sessionSearchPublishesWithoutMoreOutputAndSurvivesSnapshotRefresh)
{
    std::atomic<unsigned> wakeCount { 0 };
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->setDrainSignal(&wakeCount, [](void* context) {
        static_cast<std::atomic<unsigned>*>(context)->fetch_add(1);
    });
    session->start(4, 30);
    const std::string text = "needle one\r\nneedle two\r\n";
    session->feedBytes({ text.data(), text.size() });
    (void)session->takeEventFlags();
    wakeCount.store(0);
    CHECK(session->setSearchQuery(Ubiquitty::Search::Request { .pattern = "needle" }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uint32_t events = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        events |= session->takeEventFlags();
        if (session->searchStatus().isComplete && (events & (1u << static_cast<unsigned>(EventKind::SearchChanged))) && wakeCount.load())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto found = session->searchStatus();
    CHECK(found.isComplete);
    CHECK_EQ(found.foundCount, 2);
    CHECK_EQ(found.activeIndex, 0);
    CHECK(found.revealViewOffset >= 0);
    CHECK(events & (1u << static_cast<unsigned>(EventKind::SearchChanged)));
    CHECK(wakeCount.load() > 0);
    CHECK(!session->setSearchQuery(Ubiquitty::Search::Request { .pattern = "needle" }));
    const std::string more = "unrelated output\r\n";
    session->feedBytes({ more.data(), more.size() });
    const auto refreshed = session->readSnapshot().search;
    CHECK_EQ(refreshed.generation, found.generation);
    CHECK_EQ(refreshed.foundCount, found.foundCount);
    CHECK_EQ(refreshed.revealEpoch, found.revealEpoch);
    (void)session->stepSearchMatch(TerminalSession::SearchStep::Forward);
    const auto stepped = session->searchStatus();
    CHECK_EQ(stepped.activeIndex, 1);
    CHECK(stepped.revealEpoch > found.revealEpoch);
    CHECK(session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::SearchChanged)));
    session->clearScrollbackSearch();
    CHECK(session->searchStatus().isComplete);
    CHECK_EQ(session->searchStatus().foundCount, 0);
    CHECK_EQ(session->searchStatus().revealViewOffset, -1);
    session->lockForRead();
    CHECK_EQ(Ubiquitty::Screen::ScrollSearch::matchCount(*session->terminal()), 0);
    CHECK_EQ(Ubiquitty::Screen::ScrollSearch::activeMatchEpoch(*session->terminal()), 0);
    session->unlockForRead();
}

TEST(unregisteringAnEventSignalWaitsForItsActiveProducer)
{
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    struct State {
        std::binary_semaphore entered { 0 };
        std::binary_semaphore mayReturn { 0 };
        std::atomic<unsigned> calls { 0 };
    } state;
    session->setDrainSignal(&state, [](void* context) {
        auto& state = *static_cast<State*>(context);
        ++state.calls;
        state.entered.release();
        state.mayReturn.acquire();
    });
    std::thread producer([&] { session->signalDrain(); });
    state.entered.acquire();
    std::binary_semaphore removing(0);
    std::binary_semaphore removed(0);
    std::thread remover([&] {
        removing.release();
        session->setDrainSignal(nullptr, nullptr);
        removed.release();
    });
    removing.acquire();
    CHECK(!removed.try_acquire_for(std::chrono::milliseconds(30)));
    state.mayReturn.release();
    producer.join();
    remover.join();
    (void)session->takeEventFlags();
    session->signalDrain();
    CHECK_EQ(state.calls.load(), 1u);
}

TEST(closingASessionDetachesItsQueuedSearchBeforeDisposingTheTerminal)
{
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(4, 30);
    const auto controller = session->searchController();
    REQUIRE(controller);
    CHECK(controller->setQuery({ .pattern = "needle" }));
    CHECK(controller->hasWork());
    session->close();
    CHECK(!controller->hasWork());
    CHECK(!session->searchController());
}

TEST(sessionPaletteSearchNamesItsScopeAndResolvesSavedDestinations)
{
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(4, 30);
    const std::string text = "needle one\r\nneedle two\r\nneedle outside\r\n";
    session->feedBytes({ text.data(), text.size() });
    CHECK(session->setSearchQuery("needle", TerminalSession::SearchPatternForm::Literal, Ubiquitty::CaseSensitivity::Insensitive, 0, 1, 1));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!session->searchStatus().isComplete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(session->searchStatus().isComplete);
    CHECK_EQ(session->searchStatus().foundCount, 2);
    const auto labels = session->searchLabels();
    REQUIRE(labels.size() == 1);
    CHECK(labels.front().text == "needle two");
    CHECK(session->searchPositionViewOffset(labels.front().position) >= 0);
    session->clearScrollbackSearch();
    CHECK(session->searchLabels().empty());
    CHECK(session->searchPositionViewOffset(labels.front().position) >= 0);
    const std::string erase = "\033[2J";
    session->feedBytes({ erase.data(), erase.size() });
    CHECK_EQ(session->searchPositionViewOffset(labels.front().position), -1);
}

TEST(sessionSearchReadsReloadedNormalizationForTheSameQuery)
{
    using Ubiquitty::Unicode::NormalizationForm;
    namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;
    const auto savedForm = ScrollSearch::normalizationForm();
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(3, 30);
    const std::string text = "cafe\xCC\x81 \xEF\xAC\x81le\r\nblank\r\nblank\r\ncafe\xCC\x81 \xEF\xAC\x81le";
    session->feedBytes({ text.data(), text.size() });
    ScrollSearch::setNormalizationForm(NormalizationForm::None);
    const auto canonical = session->searchScrollbackAndWait("caf\xC3\xA9", TerminalSession::SearchPatternForm::Literal, Ubiquitty::CaseSensitivity::Sensitive);
    CHECK(canonical.isComplete);
    CHECK_EQ(canonical.foundCount, 2);
    std::uint64_t generation = canonical.generation;
    for (const auto form : { NormalizationForm::None, NormalizationForm::NFC, NormalizationForm::NFKC, NormalizationForm::None }) {
        ScrollSearch::setNormalizationForm(form);
        CHECK(session->setSearchQuery("file", TerminalSession::SearchPatternForm::Literal, Ubiquitty::CaseSensitivity::Sensitive));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!session->searchStatus().isComplete && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto status = session->searchStatus();
        CHECK(status.isComplete);
        CHECK(!status.isPatternInvalid);
        CHECK(status.generation > generation);
        CHECK_EQ(status.foundCount, form == NormalizationForm::NFKC ? 2 : 0);
        generation = status.generation;
        const auto waited = session->searchScrollbackAndWait("file", TerminalSession::SearchPatternForm::Literal, Ubiquitty::CaseSensitivity::Sensitive);
        CHECK(waited.isComplete);
        CHECK_EQ(waited.foundCount, status.foundCount);
        generation = waited.generation;
    }
    ScrollSearch::setNormalizationForm(savedForm);
}

TEST(sessionSearchWaitRefreshesTextAndClearCancelsQueuedWork)
{
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(4, 30);
    const Ubiquitty::Search::Request request { .pattern = "arrived" };
    CHECK_EQ(session->searchScrollbackAndWait(request).foundCount, 0);
    const std::string text = "arrived\r\n";
    session->feedBytes({ text.data(), text.size() });
    CHECK_EQ(session->searchScrollbackAndWait(request).foundCount, 1);
    const auto completed = session->searchStatus();
    std::binary_semaphore modelHeld(0);
    std::binary_semaphore releaseModel(0);
    std::thread modelOwner([&] {
        session->lockForRead();
        modelHeld.release();
        releaseModel.acquire();
        session->unlockForRead();
    });
    modelHeld.acquire();
    CHECK(session->setSearchQuery(Ubiquitty::Search::Request { .pattern = "replacement" }));
    const auto pending = session->searchStatus();
    CHECK(pending.generation > completed.generation);
    CHECK(!pending.isComplete);
    CHECK_EQ(pending.foundCount, 0);
    releaseModel.release();
    modelOwner.join();
    session->clearScrollbackSearch();
    for (int index = 0; index < 20; ++index) {
        CHECK(session->setSearchQuery(request));
        session->clearScrollbackSearch();
        const auto cleared = session->searchStatus();
        CHECK(cleared.isComplete);
        CHECK_EQ(cleared.foundCount, 0);
        CHECK_EQ(cleared.activeIndex, -1);
    }
}

TEST(searchCounterDistinguishesProgressFromAnEmptyAnswer)
{
    CHECK_STR_EQ(searchCounterText(0, -1, false, false), "Searching…");
    CHECK_STR_EQ(searchCounterText(0, -1, false, true), "No matches");
    CHECK_STR_EQ(searchCounterText(3, 0, false, false), "1 of 3+");
    CHECK_STR_EQ(searchCounterText(3, 1, false, true), "2 of 3");
    CHECK_STR_EQ(searchCounterText(0, -1, true, true), "Invalid pattern");
}

TEST(sessionSearchRebindsWhenThePresentedTerminalChanges)
{
    auto external = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*external, 4, 30);
    Ubiquitty::processBytes(external.get(), "external text");
    std::mutex externalLock;
    RetainedTerminalSession session { TerminalSession::make() };
    REQUIRE(session);
    session->start(4, 30);
    const std::string local = "local text";
    session->feedBytes({ local.data(), local.size() });
    CHECK_EQ(session->searchScrollbackAndWait({ .pattern = "local" }).foundCount, 1);
    session->adoptExternalTerminal(external.get(), reinterpret_cast<std::uintptr_t>(&externalLock));
    CHECK_EQ(session->searchScrollbackAndWait({ .pattern = "external" }).foundCount, 1);
    CHECK_EQ(session->searchScrollbackAndWait({ .pattern = "local" }).foundCount, 0);
    CHECK(session->setSearchQuery(Ubiquitty::Search::Request { .pattern = "external" }));
    session->adoptExternalTerminal(nullptr, 0);
    external.reset();
    CHECK_EQ(session->searchScrollbackAndWait({ .pattern = "local" }).foundCount, 1);
}

TEST(sessionPublicationsRefreshCompletedSearchesInOwnedAndAdoptedModels)
{
    for (const bool isAdopted : { false, true }) {
        auto external = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*external, 4, 30);
        std::mutex externalLock;
        RetainedTerminalSession session { TerminalSession::make() };
        REQUIRE(session);
        session->start(4, 30);
        if (isAdopted)
            session->adoptExternalTerminal(external.get(), reinterpret_cast<std::uintptr_t>(&externalLock));
        const auto first = session->searchScrollbackAndWait({ .pattern = "arrived" });
        CHECK_EQ(first.foundCount, 0);
        (void)session->takeEventFlags();
        const std::string text = "arrived\r\n";
        if (isAdopted) {
            {
                const std::scoped_lock hold(externalLock);
                Ubiquitty::processBytes(external.get(), text);
            }
            session->republishSnapshot();
        } else
            session->feedBytes({ text.data(), text.size() });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint32_t events = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            events |= session->takeEventFlags();
            if (session->readSnapshot().search.foundCount == 1 && (events & (1u << static_cast<unsigned>(EventKind::SearchChanged))))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto next = session->readSnapshot().search;
        CHECK_EQ(next.generation, first.generation);
        CHECK_EQ(next.foundCount, 1);
        CHECK(next.isComplete);
        CHECK(events & (1u << static_cast<unsigned>(EventKind::SearchChanged)));
        session->clearScrollbackSearch();
        if (isAdopted)
            session->adoptExternalTerminal(nullptr, 0);
    }
}
