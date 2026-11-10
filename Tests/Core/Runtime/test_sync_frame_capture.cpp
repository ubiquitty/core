// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_frame_capture — the session's boundary-capture path, end to end:
// geometry registered ahead, host bytes drained through the paced host, the
// publish hook capturing at each DECSET-2026 clear, and the retained frame a
// drive takes across the seam.
//
// The slot's index arithmetic is test_sync_frame_slot's; this file drives the
// real composition around it — the one the Swift drive runs — so the capture,
// the coalescing and the handoff are covered by a Core test rather than only
// by a frontend wiring them.

#include "Harness.h"
#include "SoleModelThread.h"
#include "StrictAtlas.h"
#include "TerminalFixture.h"

#include "Core/Modes/SyncOutput.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/OverlaySpanGather.h"
#include "Core/RichText/BlockStore.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/SelectionOverCapture.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Tek/Colors.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/ParseState.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// --- frontend-defined PAL hooks --------------------------------------------
// The paced host throttles publishes against this clock, so the test owns it:
// a frame is published only when the test has advanced past the interval
// floor, which makes every publish-or-suppress assertion deterministic
// instead of racing a real clock.

namespace Ubiquitty::PAL {

static std::uint32_t s_testMicroseconds = 0;
static std::uint32_t s_testTicks = 0;

Ticks monotonicTicks() { return Ticks(s_testTicks); }
uint32_t monotonicMicros() { return ++s_testMicroseconds; }

// The launch factory is the frontend's; every session here adopts a scripted
// stream instead, so opening a PTY is a refusal rather than a capability.
std::unique_ptr<Stream> openPTY(std::span<const std::string>, const PTYLaunchOptions&) { return nullptr; }

// An observable stream double on the test_runtime recipe: read() replays a
// scripted queue, and the descriptor stays -1 so the drain reads directly
// rather than polling.
class ScriptedStream final : public Stream, private ProcessFacts {
public:
    std::string toRead;
    std::size_t readPosition = 0;
    std::string written;
    std::uint32_t readMicroseconds = 0;

    int read(std::span<char> buffer) override
    {
        if (buffer.empty())
            return static_cast<int>(StreamStatus::Fatal);
        const std::size_t available = toRead.size() - readPosition;
        if (available == 0)
            return static_cast<int>(StreamStatus::Again);
        const std::size_t want = std::min(buffer.size(), available);
        std::ranges::copy(std::span(toRead).subspan(readPosition, want), buffer.begin());
        readPosition += want;
        s_testMicroseconds += readMicroseconds;
        return static_cast<int>(want);
    }
    int write(std::span<const char> bytes) override
    {
        written.append(bytes.data(), bytes.size());
        return static_cast<int>(bytes.size());
    }
    int writeAll(std::span<const char> bytes) override
    {
        written.append(bytes.data(), bytes.size());
        return 0;
    }
    bool drainPendingWrite() override { return false; }
    [[nodiscard]] bool hasPendingWrite() const override { return false; }
    void setWindowSize(int, int, int, int) override { }
    [[nodiscard]] int lastError() const override { return 0; }
    [[nodiscard]] int fileDescriptor() const override { return -1; }
    ProcessFacts& process() override { return *this; }

    std::string foregroundName() override { return {}; }
    bool foregroundIsShell() override { return true; }
    std::optional<ProcessID> foregroundProcessGroup() override { return {}; }
    std::string foregroundCurrentDirectory() override { return {}; }
    std::string ttyName() override { return {}; }
    bool hasRunningChild() override { return false; }
    int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }
};

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;
using Ubiquitty::Runtime::SyncFrameCaptureGeometry;
using Ubiquitty::Runtime::TerminalSession;
using namespace std::string_view_literals;

// A taken frame with its release bound in, so an assertion that stops a case
// early cannot strand the reference count.
using TakenFrame = std::unique_ptr<FrameCapture, decltype([](FrameCapture* frame) { releaseFrameCapture(frame); })>;

// The interval floor is 1s/240 in host micros; one advance past it guarantees
// the next completed boundary publishes.
void openPublishWindow() { Ubiquitty::PAL::s_testMicroseconds += 5'000; }

// A session over a scripted stream, plus the borrow the cases feed bytes
// through. The session owns the stream; the borrow is valid until close().
struct SessionFixture {
    TerminalSession* session = nullptr;
    Ubiquitty::PAL::ScriptedStream* stream = nullptr;
    bool isReady = false;
    std::unique_ptr<Ubiquitty::PAL::Stream> refusedStream;
    // The coherence rule reads the same faked clock the publish window does,
    // so opening the window moves the rule's pacing on with it.
    static std::uint64_t readCoherenceClock(void*) { return 1'000'000 + Ubiquitty::PAL::s_testMicroseconds; }

    SessionFixture(int rows, int columns)
    {
        session = TerminalSession::make();
        if (!session)
            return;
        session->setCoherenceClock(&SessionFixture::readCoherenceClock, this);
        session->start(rows, columns);
        auto scripted = std::make_unique<Ubiquitty::PAL::ScriptedStream>();
        auto* const borrowed = scripted.get();
        auto adoption = session->adoptStream(std::move(scripted));
        if (!adoption) {
            refusedStream = std::move(adoption.error());
            return;
        }
        stream = borrowed;
        isReady = true;
    }
    ~SessionFixture()
    {
        if (session) {
            session->close();
            releaseTerminalSession(session);
        }
    }

    // Queues one complete sync frame around `body` and drains it, with the
    // publish window opened so the boundary is captured, not throttled.
    void drainSyncFrame(std::string_view body)
    {
        stream->toRead += std::format("\x1b[?2026h{}\x1b[?2026l", body);
        openPublishWindow();
        (void)session->drain();
    }
};

SyncFrameCaptureGeometry testGeometry()
{
    SyncFrameCaptureGeometry geometry;
    geometry.metrics.cellWidth = 8.0f;
    geometry.metrics.cellHeight = 16.0f;
    geometry.metrics.baselineFromTop = 13.0f;
    geometry.metrics.underlineThickness = 1.0f;
    geometry.metrics.underlineDrop = 1.0f;
    geometry.metrics.strikeThickness = 1.0f;
    geometry.metrics.xHeight = 10.0f;
    geometry.glyphSlot = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
        /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
    geometry.viewportSizeInPixels = { 640.0f, 384.0f };
    geometry.style.uniformDefaultBackground = { 0.1f, 0.2f, 0.3f, 1.0f };
    return geometry;
}

// The permissive atlas from the instance-builder tests: every code point gets
// deterministic UVs, so a build over captured text produces countable cells.
struct FakeAtlas : UbiquittyTest::StrictAtlas {
    Ubiquitty::Rendering::GlyphSlotCache monoCache {
        1024, Ubiquitty::Rendering::recommendedHashBuckets(1024), 32
    };
    // Every phase bucket the build minted a mono slot for, in order. The
    // instance stream cannot answer this: the phase is part of the slot's KEY,
    // and two builds that phase the same text differently still mint their
    // slots in the same order, so the UVs read identical either way.
    std::vector<std::uint32_t> monoPhases;
    Ubiquitty::Rendering::MonoAtlasBinding bindMonoAtlas()
    {
        Ubiquitty::Rendering::MonoAtlasBinding binding;
        binding.cache = &monoCache;
        binding.geometry = { 32, 8, 16, 6, { 1, 1 } };
        binding.slotCapacity = 1024;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t phase, int)
    {
        monoPhases.push_back(phase);
        return true;
    }

    int nextSlot = 0;
    void beginFrame() { }
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t)
    {
        return assign(codePoint);
    }
    AtlasGlyph wideGlyph(std::uint32_t codePoint, FontFace) { return assign(codePoint); }
    AtlasGlyph doubleGlyph(std::uint32_t codePoint, FontFace, DoubleRegion) { return assign(codePoint); }
    AtlasGlyph assign(std::uint32_t codePoint)
    {
        if (codePoint == 0x20u)
            return {};
        const float u = static_cast<float>(nextSlot++) * 0.01f;
        return { { u, u }, { u + 0.02f, u + 0.02f }, true };
    }
};

// Builds `capture` with a fresh deterministic atlas and returns the filled
// cell-instance stream: two builds of one frame are byte-identical, so
// equality of two returns is the self-containment probe, with no dependence
// on the instance layout.
std::vector<CellInstance> builtCells(const FrameCapture& capture, const FrameCapture::ViewTierOverrides* view = nullptr)
{
    FakeAtlas atlas;
    const std::size_t capacity = static_cast<std::size_t>(capture.rows()) * capture.columns();
    std::vector<CellInstance> cells(capacity);
    std::vector<DecorationInstance> decorations(capacity * 8);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);
    const InstanceCounts counts = view ? capture.build(backend, buffers, *view) : capture.build(backend, buffers);
    cells.resize(std::min(counts.cells, cells.size()));
    return cells;
}

// The phase buckets a build minted mono slots for, in order.
std::vector<std::uint32_t> builtPhases(const FrameCapture& capture)
{
    FakeAtlas atlas;
    const std::size_t capacity = static_cast<std::size_t>(capture.rows()) * capture.columns();
    std::vector<CellInstance> cells(capacity);
    std::vector<DecorationInstance> decorations(capacity * 8);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    (void)capture.build(UbiquittyTest::glyphBackendFor(atlas), buffers);
    return atlas.monoPhases;
}

bool sameInstanceBytes(const std::vector<CellInstance>& a, const std::vector<CellInstance>& b)
{
    return a.size() == b.size()
        && std::ranges::equal(std::as_bytes(std::span(a)), std::as_bytes(std::span(b)));
}

} // namespace

TEST(hiddenRendererSkipsBoundaryCaptures)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("visible");
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Hidden);
    CHECK(!TakenFrame(fixture.session->takeSyncFrame()));
    fixture.drainSyncFrame("hidden");
    CHECK(!TakenFrame(fixture.session->takeSyncFrame()));
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("still hidden");
    CHECK(!TakenFrame(fixture.session->takeSyncFrame()));
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Presented);
    fixture.drainSyncFrame("revealed");
    CHECK(TakenFrame(fixture.session->takeSyncFrame()));
}

// A view registers its geometry, a presentation edge and the hovered run while
// a drain holds the model lock, and none of them waits for it: each is
// published, and the next capture takes it in under the lock it holds.
TEST(aViewRegistersWithoutWaitingForTheDrain)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("first");
    const TakenFrame first(fixture.session->takeSyncFrame());
    REQUIRE(first);
    const std::uint64_t firstGeneration = first->viewGeneration();

    fixture.session->lockForRead();
    auto registering = std::async(std::launch::async, [&fixture] {
        fixture.session->setSyncFrameCaptureGeometry(testGeometry());
        fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Hidden);
        fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Presented);
        fixture.session->setSyncFrameHoverLink({});
    });
    const bool didReturnWhileHeld = registering.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    fixture.session->unlockForRead();
    registering.get();
    CHECK_MSG(didReturnWhileHeld, "the registrations returned while the drain held the model");

    fixture.drainSyncFrame("second");
    const TakenFrame second(fixture.session->takeSyncFrame());
    REQUIRE(second);
    CHECK_EQ(second->viewGeneration(), firstGeneration + 1);
}

TEST(timedDrainsRequireAnArmedPresentedCapture)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.stream->readMicroseconds = 10'000;
    const auto drainText = [&](std::string_view text) {
        fixture.stream->toRead += text;
        return fixture.session->drain();
    };
    CHECK(drainText("unarmed").end == Ubiquitty::Runtime::DrainPassEnd::WouldBlock);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    REQUIRE(fixture.session->isSyncCaptureArmed());
    CHECK(drainText("presented").end == Ubiquitty::Runtime::DrainPassEnd::TimeBudget);
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Hidden);
    CHECK(drainText("hidden").end == Ubiquitty::Runtime::DrainPassEnd::WouldBlock);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    REQUIRE(fixture.session->isSyncCaptureArmed());
    CHECK(drainText("still hidden").end == Ubiquitty::Runtime::DrainPassEnd::WouldBlock);
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Presented);
    const auto revealed = drainText("revealed");
    CHECK(revealed.end == Ubiquitty::Runtime::DrainPassEnd::TimeBudget);
    CHECK(!revealed.caughtUp);
}

TEST(timedPartialCommandsCaptureOwnedCellsAndCompleteWithoutChangingTheStream)
{
    struct SplitCommand {
        std::string_view prefix;
        std::string_view suffix;
    };
    constexpr std::array commands {
        SplitCommand { "\x1b[48;5;", "196m " },
        SplitCommand { "\x1b]4;1;rgb:12/34/", "56\x1b\\\x1b[41m " },
        SplitCommand { "\x1bP$q", "m\x1b\\" },
        SplitCommand { "\x1b_Ga=T,f=32,s=1,v=1,i=73,q=2;AAAA", "AA==\x1b\\" },
        SplitCommand { "\xe2\x82", "\xac" },
    };
    for (const auto& command : commands) {
        SessionFixture fixture(6, 20);
        SessionFixture control(6, 20);
        REQUIRE(fixture.isReady && control.isReady);
        fixture.session->setSyncFrameCaptureGeometry(testGeometry());
        fixture.drainSyncFrame("ready");
        REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
        control.drainSyncFrame("ready");
        static_cast<void>(fixture.session->takeSyncFrameReady());

        const std::string prefix = std::string("\x1b[Hpainted") + std::string(command.prefix);
        fixture.stream->readMicroseconds = 10'000;
        fixture.stream->toRead += prefix;
        const auto outcome = fixture.session->drain();
        CHECK(outcome.end == Ubiquitty::Runtime::DrainPassEnd::TimeBudget);
        CHECK(!outcome.caughtUp && !outcome.reachedEnd);
        CHECK(fixture.session->isDrainSaturated());
        CHECK(!fixture.session->shouldWithholdFrame(Ticks(0)));
        CHECK(fixture.session->takeSyncFrameReady());
        const TakenFrame frame { fixture.session->takeSyncFrame() };
        CHECK(frame != nullptr);
        std::vector<CellInstance> capturedCells;
        if (frame) {
            CHECK_EQ(frame->cursorColumn(), 7);
            capturedCells = builtCells(*frame);
        }

        fixture.stream->toRead += command.suffix;
        fixture.stream->toRead += "\x1b[6n";
        static_cast<void>(fixture.session->drain());
        control.stream->toRead += prefix + std::string(command.suffix) + "\x1b[6n";
        static_cast<void>(control.session->drain());
        const Terminal& actual = withSessionModel(*fixture.session, [](Terminal& terminal) -> Terminal& { return terminal; });
        const Terminal& expected = withSessionModel(*control.session, [](Terminal& terminal) -> Terminal& { return terminal; });
        CHECK(actual.parser.parseState == expected.parser.parseState);
        CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
        CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
        CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
        CHECK(std::memcmp(&actual.screens.active->pen, &expected.screens.active->pen, sizeof(Pen)) == 0);
        CHECK(actual.palette == expected.palette);
        for (int row = 0; row < actual.rows; ++row) {
            for (int column = 0; column < actual.columns; ++column)
                CHECK(std::memcmp(UbiquittyTest::cell(actual, row, column), UbiquittyTest::cell(expected, row, column), sizeof(Cell)) == 0);
        }
        CHECK(fixture.stream->written == control.stream->written);
        CHECK_EQ(Image::placementCount(&actual), Image::placementCount(&expected));
        if (frame)
            CHECK(sameInstanceBytes(capturedCells, builtCells(*frame)));
    }
}

TEST(aPersistentHiddenCursorAllowsEveryTimedPartialCommandCapture)
{
    SessionFixture fixture(6, 20);
    SessionFixture control(6, 20);
    REQUIRE(fixture.isReady && control.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.session->setSyncPublishRefreshSeconds(1.0 / 120.0);
    fixture.drainSyncFrame("ready");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    control.drainSyncFrame("ready");

    // A 120 Hz display gives the capture producer a half-frame budget, clamped
    // to 1s/240. The scripted read advances both clocks without sleeping.
    fixture.stream->readMicroseconds = 1'000'000 / 240;
    const std::string prefix = "\x1b[?25l\x1b[Hpainted\x1b[48;5;";
    std::string whole = prefix;
    const auto capturesBeforeHide = fixture.session->syncFramePublicationCount();
    fixture.stream->toRead += prefix;
    CHECK(fixture.session->drain().end == Runtime::DrainPassEnd::TimeBudget);
    CHECK(fixture.session->isFrameHeld());
    CHECK_EQ(fixture.session->syncFramePublicationCount(), capturesBeforeHide);
    CHECK(!TakenFrame(fixture.session->takeSyncFrame()));

    // The cursor stays hidden throughout; only its first redraw bracket ages.
    PAL::s_testMicroseconds += static_cast<std::uint32_t>(Runtime::coherenceDeadlineMicroseconds);
    constexpr std::array chunks {
        "196mA\x1b[48;5;"sv,
        "197mB\x1b[48;5;"sv,
        "198mC\x1b[48;5;"sv,
        "199mD\x1b[48;5;"sv,
        "200mE\x1b[48;5;"sv,
        "201mF\x1b[48;5;"sv,
        "202mG\x1b[48;5;"sv,
        "203mH\x1b[48;5;"sv,
    };
    TakenFrame retained;
    std::vector<CellInstance> retainedCells;
    auto expectedCaptures = capturesBeforeHide;
    for (const auto chunk : chunks) {
        whole += chunk;
        fixture.stream->toRead += chunk;
        const auto outcome = fixture.session->drain();
        CHECK(outcome.end == Runtime::DrainPassEnd::TimeBudget);
        CHECK(!outcome.caughtUp && !outcome.reachedEnd);
        CHECK(fixture.session->isDrainSaturated());
        CHECK(!fixture.session->isFrameHeld());
        CHECK(!withSessionModel(*fixture.session, [](Terminal& terminal) { return VT::isParserAtGround(&terminal); }));
        ++expectedCaptures;
        CHECK_EQ(fixture.session->syncFramePublicationCount(), expectedCaptures);
        TakenFrame frame { fixture.session->takeSyncFrame() };
        REQUIRE(frame != nullptr);
        CHECK(!TakenFrame(fixture.session->takeSyncFrame()));
        if (!retained) {
            retainedCells = builtCells(*frame);
            retained = std::move(frame);
        }
        CHECK(sameInstanceBytes(retainedCells, builtCells(*retained)));
    }

    constexpr std::string_view suffix = "204mZ\x1b[6n";
    fixture.stream->toRead += suffix;
    CHECK(fixture.session->drain().end == Runtime::DrainPassEnd::TimeBudget);
    ++expectedCaptures;
    CHECK_EQ(fixture.session->syncFramePublicationCount(), expectedCaptures);
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    whole += suffix;
    control.stream->toRead += whole;
    static_cast<void>(control.session->drain());
    const Terminal& actual = withSessionModel(*fixture.session, [](Terminal& terminal) -> Terminal& { return terminal; });
    const Terminal& expected = withSessionModel(*control.session, [](Terminal& terminal) -> Terminal& { return terminal; });
    CHECK(VT::isParserAtGround(&actual));
    CHECK(actual.parser.parseState == expected.parser.parseState);
    CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
    CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
    CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
    CHECK(actual.modes.isCursorVisible == expected.modes.isCursorVisible);
    CHECK_EQ(actual.cursorHideCount, expected.cursorHideCount);
    CHECK(std::memcmp(&actual.screens.active->pen, &expected.screens.active->pen, sizeof(Pen)) == 0);
    CHECK(actual.palette == expected.palette);
    for (int row = 0; row < actual.rows; ++row) {
        for (int column = 0; column < actual.columns; ++column)
            CHECK(std::memcmp(UbiquittyTest::cell(actual, row, column), UbiquittyTest::cell(expected, row, column), sizeof(Cell)) == 0);
    }
    CHECK(fixture.stream->written == control.stream->written);
    CHECK(sameInstanceBytes(retainedCells, builtCells(*retained)));
}

TEST(aBoundaryBeforeArmingRaisesTheFlagButHandsOverNoFrame)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.drainSyncFrame("still frame");
    // The flag is the Metal arm's surface and rises regardless; the capture
    // slot stays empty until geometry arms it.
    CHECK(fx.session->takeSyncFrameReady());
    CHECK(fx.session->takeSyncFrame() == nullptr);
}

TEST(anArmedBoundaryCapturesTheFrameTheClearCompleted)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("hello");

    CHECK(fx.session->takeSyncFrameReady());
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);
    CHECK_EQ(frame->rows(), 24);
    CHECK_EQ(frame->columns(), 80);
    // The cursor sits after the text the boundary completed: content, not
    // just geometry, crossed into the capture.
    CHECK_EQ(frame->cursorRow(), 0);
    CHECK_EQ(frame->cursorColumn(), 5);
}

TEST(cursorMotionPolicyStaysWithTheCapturedFrame)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    withSessionModel(*fx.session, [](Terminal& terminal) {
        terminal.cursorMotionModePref = CursorMotionMode::Long;
        terminal.cursorMotionDurationMilliseconds = 180;
    });
    fx.drainSyncFrame("motion");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);
    withSessionModel(*fx.session, [](Terminal& terminal) {
        terminal.cursorMotionModePref = CursorMotionMode::Off;
        terminal.cursorMotionDurationMilliseconds = 0;
    });
    CHECK(frame->cursorMotionMode() == CursorMotionMode::Long);
    CHECK_EQ(frame->cursorMotionDurationMilliseconds(), 180u);
}

// The optimization the whole split exists for: ordinary output settling is
// announced (the flag rises so the render wakes) but not captured into the sync
// slot, so the Swift render takes it down its own damage-cheap live route rather
// than the full-rebuild boundary route. A plain drain must therefore leave the
// slot empty.
TEST(aPlainSettleAnnouncesButDoesNotCapture)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.stream->toRead += "plain output, no synchronized-update block at all";
    openPublishWindow();
    (void)fx.session->drain();
    // Announced once the read has gone quiet, by the frontend's own re-ask:
    // a drain alone leaves the frame held for quiet.
    REQUIRE(fx.session->isFrameHeld());
    const std::optional<std::uint64_t> due = fx.session->publishHeldFrameIfDue();
    REQUIRE(due.has_value());
    Ubiquitty::PAL::s_testMicroseconds += static_cast<std::uint32_t>(*due);
    (void)fx.session->publishHeldFrameIfDue();
    CHECK(!fx.session->isFrameHeld());
    // The re-ask also names the CRT saver's deadline. With the saver off,
    // nothing is left to wait for.
    withSessionModel(*fx.session, [](Terminal& terminal) { terminal.modes.shouldSaveCRT = false; });
    CHECK(!fx.session->publishHeldFrameIfDue().has_value());
    CHECK(fx.session->takeSyncFrameReady()); // announced: the render still wakes
    CHECK(fx.session->takeSyncFrame() == nullptr); // but nothing was captured
}

// The held-frame timer is a quiet block's watchdog. The block it expires is
// published under the lock that expired it, or the frontend's lock-free
// withhold goes on reading the block as open.
TEST(aBlockTheHeldFrameTimerExpiresIsPublishedClosed)
{
    struct RestoreTicks {
        std::uint32_t previous = Ubiquitty::PAL::s_testTicks;
        ~RestoreTicks() { Ubiquitty::PAL::s_testTicks = previous; }
    } restoreTicks;
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    withSessionModel(*fixture.session, [](Terminal& terminal) { terminal.modes.shouldSaveCRT = false; });
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("before");
    static_cast<void>(TakenFrame(fixture.session->takeSyncFrame()));

    Ubiquitty::PAL::s_testTicks = 100;
    fixture.stream->toRead += "\x1b[?2026h\x1b[Hafter";
    REQUIRE(fixture.session->drain().caughtUp);
    REQUIRE(withSessionModel(*fixture.session, [](Terminal& terminal) { return terminal.modes.syncOutput; }));
    REQUIRE(fixture.session->isFrameHeld());
    CHECK(fixture.session->readSnapshot().syncHoldActive);

    Ubiquitty::PAL::s_testTicks = 161;
    Ubiquitty::PAL::s_testMicroseconds += 1'100'000;
    static_cast<void>(fixture.session->publishHeldFrameIfDue());
    CHECK(!withSessionModel(*fixture.session, [](Terminal& terminal) { return terminal.modes.syncOutput; }));
    CHECK(!fixture.session->readSnapshot().syncHoldActive);
    CHECK(!fixture.session->shouldWithholdFrame(Ticks(161)));
}

namespace {

/// Restores the faked tick clock when a case that ages a block ends.
struct RestoreTestTicks {
    std::uint32_t previous = Ubiquitty::PAL::s_testTicks;
    ~RestoreTestTicks() { Ubiquitty::PAL::s_testTicks = previous; }
};

/// Whether the published colours carry the background a case's OSC 11 set.
bool isPublishedBackgroundSet(TerminalSession& session)
{
    const Ubiquitty::ColorRGBA background = session.readCoherentColors().colors.background;
    return background.red == 0x00 && background.green == 0x77 && background.blue == 0x77;
}

} // namespace

// The drain that opens a block stamps it as it admits the pass's frame. The
// stamp is published with the block, so the budget a frontend reads without
// the model runs from the drain's tick, not from a later ask.
TEST(aBlockTheDrainStampsIsPublishedWithItsStamp)
{
    const RestoreTestTicks restoreTicks;
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    withSessionModel(*fixture.session, [](Terminal& terminal) { terminal.modes.shouldSaveCRT = false; });
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("before");
    static_cast<void>(TakenFrame(fixture.session->takeSyncFrame()));

    Ubiquitty::PAL::s_testTicks = 100;
    fixture.stream->toRead += "\x1b[?2026h\x1b[Hafter";
    REQUIRE(fixture.session->drain().caughtUp);
    const std::uint32_t stamp = withSessionModel(*fixture.session, [](Terminal& terminal) { return terminal.modes.syncOutputTouchTicks; });
    REQUIRE(stamp == 100u);
    CHECK(fixture.session->readSnapshot().syncHoldActive);
    CHECK_EQ(fixture.session->readSnapshot().syncHoldTouchTicks, stamp);
    CHECK_EQ(fixture.session->presentationHoldTicksRemaining(Ticks(130)), Ubiquitty::Modes::SyncOutput::maxHoldTicks - 30 + 1);
}

// A flood inside a block the program never closes runs the block's budget out
// on a drain pass. That pass publishes the block closed, colours and all, so
// the delivery it makes carries them whether or not the flood goes on.
TEST(aPassThatExpiresABlockPublishesItsColors)
{
    const RestoreTestTicks restoreTicks;
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    withSessionModel(*fixture.session, [](Terminal& terminal) { terminal.modes.shouldSaveCRT = false; });
    const std::uint64_t epochBefore = fixture.session->readCoherentColors().paletteEpoch;

    Ubiquitty::PAL::s_testTicks = 100;
    fixture.stream->toRead += "\x1b[?2026h\x1b]11;rgb:00/77/77\x07\x1b[Hflood";
    REQUIRE(fixture.session->drain().bytesDrained > 0);
    REQUIRE(withSessionModel(*fixture.session, [](Terminal& terminal) { return terminal.modes.syncOutput; }));
    CHECK(fixture.session->readSnapshot().syncHoldActive);
    CHECK_EQ(fixture.session->readCoherentColors().paletteEpoch, epochBefore);

    Ubiquitty::PAL::s_testTicks = 161;
    Ubiquitty::PAL::s_testMicroseconds += 1'100'000;
    fixture.stream->toRead += "\x1b[Hmore flood";
    CHECK(fixture.session->drain().bytesDrained > 0);
    REQUIRE(!withSessionModel(*fixture.session, [](Terminal& terminal) { return terminal.modes.syncOutput; }));
    CHECK(!fixture.session->readSnapshot().syncHoldActive);
    CHECK_NE(fixture.session->readCoherentColors().paletteEpoch, epochBefore);
    CHECK(isPublishedBackgroundSet(*fixture.session));
}

// The held-frame timer expires a block a quiet program left open and releases
// the frame it held, which the ingest loop announces. The colours the block
// set are published with that frame.
TEST(aBlockTheHeldFrameTimerExpiresReleasesItsFrameWithItsColors)
{
    const RestoreTestTicks restoreTicks;
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    withSessionModel(*fixture.session, [](Terminal& terminal) { terminal.modes.shouldSaveCRT = false; });
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("before");
    static_cast<void>(TakenFrame(fixture.session->takeSyncFrame()));
    const std::uint64_t epochBefore = fixture.session->readCoherentColors().paletteEpoch;

    Ubiquitty::PAL::s_testTicks = 100;
    fixture.stream->toRead += "\x1b[?2026h\x1b]11;rgb:00/77/77\x07\x1b[Hafter";
    REQUIRE(fixture.session->drain().caughtUp);
    REQUIRE(fixture.session->isFrameHeld());
    CHECK_EQ(fixture.session->readCoherentColors().paletteEpoch, epochBefore);

    Ubiquitty::PAL::s_testTicks = 161;
    Ubiquitty::PAL::s_testMicroseconds += 1'100'000;
    static_cast<void>(fixture.session->publishHeldFrameIfDue());
    CHECK(!fixture.session->isFrameHeld());
    CHECK(!fixture.session->readSnapshot().syncHoldActive);
    CHECK_NE(fixture.session->readCoherentColors().paletteEpoch, epochBefore);
    CHECK(isPublishedBackgroundSet(*fixture.session));
}

TEST(aContinuingUnsaturatedRedrawRetainsItsAdmittedFrame)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("initial");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    static_cast<void>(fixture.session->takeSyncFrameReady());

    // Every short pass ends at EAGAIN, but the writer returns before the
    // quiet interval. The pacing deadline admits a frame while it is active.
    for (int pass = 0; pass < 10; ++pass) {
        Ubiquitty::PAL::s_testMicroseconds += 1'000;
        fixture.stream->toRead += "\x1b[Hredraw";
        const auto outcome = fixture.session->drain();
        REQUIRE(outcome.caughtUp);
        REQUIRE(outcome.bytesDrained < Ubiquitty::Runtime::TerminalRuntime::drainPassCapacityBytes);
    }
    REQUIRE(fixture.session->takeSyncFrameReady());

    // The next pass starts before the display tick. Its incomplete model
    // must not hide the already-admitted frame from that tick.
    fixture.stream->toRead += "\x1b[Hnext\x1b[";
    static_cast<void>(fixture.session->drain());
    REQUIRE(fixture.session->isFrameHeld());
    REQUIRE(fixture.session->shouldWithholdFrame(Ticks(0)));
    const TakenFrame frame { fixture.session->takeSyncFrame() };
    REQUIRE(frame != nullptr);
    CHECK_EQ(frame->cursorColumn(), 6);
    const auto cells = builtCells(*frame);
    CHECK(!cells.empty());
    fixture.stream->toRead += "1;1Hdifferent";
    static_cast<void>(fixture.session->drain());
    CHECK(sameInstanceBytes(cells, builtCells(*frame)));
}

// The counterpart, and the regression guard: a 2026 frame
// whose ESU is the last byte of the read completes through the settle path (the
// mid-chunk boundary gate never fires), yet it is a real synchronized frame and
// must be captured — or the terminal shows nothing until the next keystroke.
TEST(anEsuTerminatedFrameSettlesButIsStillCaptured)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    // No trailing bytes after the ESU: the clear ends the read.
    fx.stream->toRead += "\x1b[?2026hZ\x1b[?2026l";
    openPublishWindow();
    (void)fx.session->drain();
    CHECK(fx.session->takeSyncFrameReady());
    const TakenFrame frame { fx.session->takeSyncFrame() };
    REQUIRE(frame != nullptr); // the frame the ESU completed was captured
    CHECK_EQ(frame->cursorColumn(), 1); // after the one cell the block drew
}

TEST(aSecondBoundaryRecordsADeltaOfTheRowsItChanged)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
        CHECK(first->isFullFrame()); // no baseline yet
    }
    fx.drainSyncFrame("\x1b[4;1Hb"); // one row, row 3
    const TakenFrame second { fx.session->takeSyncFrame() };
    REQUIRE(second != nullptr);
    CHECK(second->recordKind() == FrameRecordKind::Delta);
    CHECK(second->baseSequence() == 1);
    CHECK(second->hasCapturedDirtyRows());
    CHECK(second->isRowDirty(3));
    CHECK(!second->isRowDirty(10));
    CHECK(!second->isFullFrame());
}

TEST(aSupersededBoundaryCarriesItsRowsIntoTheNextCapture)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[2;1Hx"); // row 1, published but never taken
    fx.drainSyncFrame("\x1b[6;1Hy"); // row 5, supersedes it in the slot
    const TakenFrame newest { fx.session->takeSyncFrame() };
    REQUIRE(newest != nullptr);
    CHECK(newest->recordKind() == FrameRecordKind::Delta);
    CHECK(newest->isRowDirty(1)); // the superseded frame's row rides along
    CHECK(newest->isRowDirty(5));
    CHECK(!newest->isRowDirty(12));
    // Taken: nothing is owed, so the next Delta names only its own row. The
    // row the cursor vacated is not among them: the cursor is the tier over
    // the grid, never in it, so the row it leaves holds the cell it always
    // held, beside the `y` or not.
    fx.drainSyncFrame("\x1b[9;1Hz");
    const TakenFrame next { fx.session->takeSyncFrame() };
    REQUIRE(next != nullptr);
    CHECK(next->recordKind() == FrameRecordKind::Delta);
    CHECK(next->isRowDirty(8));
    CHECK_MSG(!next->isRowDirty(5), "a cursor that left beside ink owes no row either");
    CHECK(!next->isRowDirty(1));
    CHECK(!next->isRowDirty(12));
}

TEST(aCursorTheTierDrawsOwesNoRowWhenItLeaves)
{
    // The same rule with nothing beside the cursor: the tier re-emits the
    // cell at the tail of the stream, so the row it leaves holds the plain
    // cell it always held.
    //
    // Without this the case above passes against a plan that marks the vacated
    // row unconditionally, which is the behaviour the tier exists to avoid.
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    // The text lands at the row's head and the cursor parks far from it, so
    // the eight cells around the cursor are blank.
    fx.drainSyncFrame("\x1b[6;1Hy\x1b[6;40H");
    {
        const TakenFrame mid { fx.session->takeSyncFrame() };
        REQUIRE(mid != nullptr);
        CHECK(mid->isRowDirty(5));
    }
    fx.drainSyncFrame("\x1b[9;1Hz\x1b[9;40H");
    const TakenFrame next { fx.session->takeSyncFrame() };
    REQUIRE(next != nullptr);
    CHECK(next->recordKind() == FrameRecordKind::Delta);
    CHECK(next->isRowDirty(8));
    CHECK_MSG(!next->isRowDirty(5), "a cursor the tier drew leaves the row it stood in unchanged");
}

TEST(anIdenticalRewriteOwesNoRow)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("abc");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    // The same cells rewritten in place: the shadow reads no change, the
    // cursor lands where it was.
    fx.drainSyncFrame("\x1b[1;1Habc");
    const TakenFrame same { fx.session->takeSyncFrame() };
    REQUIRE(same != nullptr);
    CHECK(same->recordKind() == FrameRecordKind::Delta);
    CHECK(!same->hasCapturedDirtyRows());
    CHECK(!same->hasDirtyRowsInMask());
    CHECK(same->capturedDirtyRowCount() == 0);
}

TEST(aPaletteWriteMakesTheNextBoundaryFull)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[4;1Hb");
    {
        const TakenFrame second { fx.session->takeSyncFrame() };
        REQUIRE(second != nullptr);
        CHECK(second->recordKind() == FrameRecordKind::Delta);
    }
    // OSC 4 recolours palette entry 1: every cell painted with it changes
    // without a dirty row, so the epoch refuses the lineage.
    fx.drainSyncFrame("\x1b]4;1;rgb:ff/00/00\x1b\\c");
    {
        const TakenFrame recoloured { fx.session->takeSyncFrame() };
        REQUIRE(recoloured != nullptr);
        CHECK(recoloured->isFullFrame());
    }
    fx.drainSyncFrame("\x1b[6;1Hd");
    const TakenFrame next { fx.session->takeSyncFrame() };
    REQUIRE(next != nullptr);
    CHECK(next->recordKind() == FrameRecordKind::Delta);
    CHECK(next->isRowDirty(5));
    CHECK(!next->isRowDirty(12));
}

TEST(aReverseVideoFlipMakesTheNextBoundaryFull)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[?5h");
    {
        const TakenFrame reversed { fx.session->takeSyncFrame() };
        REQUIRE(reversed != nullptr);
        CHECK(reversed->isFullFrame());
    }
    fx.drainSyncFrame("\x1b[4;1Hb");
    {
        const TakenFrame second { fx.session->takeSyncFrame() };
        REQUIRE(second != nullptr);
        CHECK(second->recordKind() == FrameRecordKind::Delta);
    }
    fx.drainSyncFrame("\x1b[?5l");
    const TakenFrame restored { fx.session->takeSyncFrame() };
    REQUIRE(restored != nullptr);
    CHECK(restored->isFullFrame());
}

TEST(aGeometryChangeStartsANewLineage)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("b");
    {
        const TakenFrame delta { fx.session->takeSyncFrame() };
        REQUIRE(delta != nullptr);
        CHECK(delta->recordKind() == FrameRecordKind::Delta);
    }
    SyncFrameCaptureGeometry moved = testGeometry();
    moved.originInPixels.x = 9.0f;
    fx.session->setSyncFrameCaptureGeometry(moved);
    fx.drainSyncFrame("c");
    const TakenFrame full { fx.session->takeSyncFrame() };
    REQUIRE(full != nullptr);
    CHECK(full->recordKind() == FrameRecordKind::Full);
    CHECK(full->baseSequence() == 0);
}

// A registration that changes only the style starts a new lineage too: the
// frame after it is Full, at the drain's boundary and at the view's ask alike,
// so no row keeps pixels drawn in the old style.
TEST(aStyleOnlyRegistrationStartsANewLineage)
{
    for (const bool isServedAtTheAsk : { false, true }) {
        SessionFixture fx(24, 80);
        REQUIRE(fx.isReady);
        SyncFrameCaptureGeometry geometry = testGeometry();
        fx.session->setSyncFrameCaptureGeometry(geometry);
        fx.drainSyncFrame("a");
        REQUIRE(TakenFrame(fx.session->takeSyncFrame()));
        fx.drainSyncFrame("\x1b[2;1Hb");
        {
            const TakenFrame delta { fx.session->takeSyncFrame() };
            REQUIRE(delta != nullptr);
            REQUIRE(delta->recordKind() == FrameRecordKind::Delta);
        }
        geometry.style.uniformDefaultBackground = { 0.9f, 0.8f, 0.7f, 1.0f };
        fx.session->setSyncFrameCaptureGeometry(geometry);
        TakenFrame next;
        Runtime::FrameSupply supply;
        if (isServedAtTheAsk) {
            next.reset(fx.session->takeFrame({}, supply));
        } else {
            fx.drainSyncFrame("\x1b[3;1Hc");
            next.reset(fx.session->takeSyncFrame());
        }
        REQUIRE(next != nullptr);
        CHECK_MSG(next->recordKind() == FrameRecordKind::Full, isServedAtTheAsk ? "served at the ask" : "at the drain's boundary");
        CHECK(next->baseSequence() == 0);
        CHECK(next->dirtyRowCount() == next->rows());
        CHECK(next->uniforms().defaultBackground.x == 0.9f);
    }
}

TEST(boundaryRecordsCarryMonotonicIdentityAndEpochs)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    const TakenFrame first { fx.session->takeSyncFrame() };
    REQUIRE(first != nullptr);
    CHECK(first->sequence() == 1);
    CHECK(first->baseSequence() == 0);
    CHECK(first->recordKind() == FrameRecordKind::Full);
    CHECK(first->isFullFrame());
    CHECK(first->dirtyRowCount() == first->rows());
    CHECK(first->isRowDirty(0));
    const std::uint64_t firstModelGeneration = first->modelGeneration();
    const std::uint64_t firstPaletteGeneration = first->paletteGeneration();
    const std::uint64_t firstViewGeneration = first->viewGeneration();

    fx.drainSyncFrame("b\x1b]4;1;#123456\x1b\\");
    const TakenFrame second { fx.session->takeSyncFrame() };
    REQUIRE(second != nullptr);
    CHECK(second->sequence() == 2);
    CHECK(second->modelGeneration() > firstModelGeneration);
    CHECK(second->paletteGeneration() > firstPaletteGeneration);
    CHECK(second->viewGeneration() == firstViewGeneration);

    SyncFrameCaptureGeometry moved = testGeometry();
    moved.originInPixels.x = 9.0f;
    fx.session->setSyncFrameCaptureGeometry(moved);
    fx.drainSyncFrame("c");
    const TakenFrame third { fx.session->takeSyncFrame() };
    REQUIRE(third != nullptr);
    CHECK(third->sequence() == 3);
    CHECK(third->viewGeneration() == firstViewGeneration + 1);
    CHECK(third->recordHeader().captureTicks == third->blinkTicks());
    CHECK(fx.session->syncFrameCaptureAttemptCount() == 3);
    CHECK(fx.session->syncFramePublicationCount() == 3);
    CHECK(fx.session->syncFrameNoSlotCount() == 0);
}

TEST(aDeltaRecordOwnsDamageAndMalformedDamagePromotesToFull)
{
    ownModelOnTestThread();
    SessionFixture fx(4, 8);
    REQUIRE(fx.isReady);
    FrameCapture* capture = FrameCapture::make();
    REQUIRE(capture != nullptr);
    const TakenFrame owned { capture };
    Terminal* terminal = fx.session->terminal();
    REQUIRE(terminal != nullptr);
    TerminalSnapshotOptions options;
    REQUIRE(capture->capture(*terminal, options, testGeometry().metrics, testGeometry().glyphSlot));

    std::array<std::uint8_t, 4> dirtyRows { 0, 1, 0, 1 };
    CapturedRowRefresh refresh;
    refresh.canReuseRows = true;
    refresh.generationAfterDrain = 17;
    refresh.rowDirty = dirtyRows;
    FrameDamage damage;
    damage.canReuse = true;
    damage.generationAfterDrain = 17;
    FrameRecordHeader header;
    header.sequence = 7;
    header.baseSequence = 6;
    header.modelGeneration = 17;
    header.paletteGeneration = 5;
    header.viewGeneration = 3;
    header.captureTicks = 11;
    header.kind = FrameRecordKind::Delta;
    options.blinkTicks = header.captureTicks;
    REQUIRE(capture->captureRecord(*terminal, options, testGeometry().metrics,
        testGeometry().glyphSlot, header, damage, refresh));
    dirtyRows.fill(0);

    CHECK(capture->recordKind() == FrameRecordKind::Delta);
    CHECK(capture->sequence() == 7);
    CHECK(capture->baseSequence() == 6);
    CHECK(capture->modelGeneration() == 17);
    CHECK(capture->paletteGeneration() == 5);
    CHECK(capture->viewGeneration() == 3);
    CHECK(!capture->isRowDirty(0));
    CHECK(capture->isRowDirty(1));
    CHECK(!capture->isRowDirty(2));
    CHECK(capture->isRowDirty(3));

    std::array<std::uint8_t, 3> shortDamage { 0, 1, 0 };
    refresh.rowDirty = shortDamage;
    header.sequence = 8;
    header.baseSequence = 7;
    REQUIRE(capture->captureRecord(*terminal, options, testGeometry().metrics,
        testGeometry().glyphSlot, header, damage, refresh));
    CHECK(capture->isFullFrame());
    CHECK(capture->baseSequence() == 0);
    CHECK(!capture->frameDamage().canReuse);
    CHECK(capture->dirtyRowCount() == capture->rows());
    for (int row = 0; row < capture->rows(); ++row) {
        CHECK(capture->isRowDirty(row));
    }
}

TEST(takingACapturedFrameDoesNotWaitForTheModelLock)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("lock-free");

    fx.session->lockForRead();
    auto take = std::async(std::launch::async, [&] { return fx.session->takeSyncFrame(); });
    const bool didCompleteWhileLocked = take.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    fx.session->unlockForRead();

    const TakenFrame frame { take.get() };
    CHECK_MSG(didCompleteWhileLocked, "the Swift frame claim must not wait for the terminal model lock");
    CHECK(frame != nullptr);
}

TEST(aTakenFrameIsSelfContainedWhileTheTerminalAdvances)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("hello");
    const TakenFrame held { fx.session->takeSyncFrame() };
    CHECK(held != nullptr);
    const std::vector<CellInstance> before = builtCells(*held);

    // The terminal moves on; the held frame must not.
    fx.drainSyncFrame(" world and more text");
    CHECK_EQ(held->cursorColumn(), 5);
    CHECK(sameInstanceBytes(before, builtCells(*held)));
}

TEST(boundariesBetweenTakesCoalesceAndTheTakeSeesTheNewest)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("first");
    fx.drainSyncFrame("second-longer");

    CHECK_EQ(fx.session->syncFrameOverwriteCount(), 1);
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);
    // Both bodies ran; the capture is the grid as the second clear left it.
    CHECK_EQ(frame->cursorColumn(), static_cast<int>("firstsecond-longer"sv.size()));
    CHECK_EQ(fx.session->syncFramePromoteCount(), 1);
}

TEST(aThrottledBoundaryPublishesNothingUntilTheWindowReopens)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("shown");
    CHECK(fx.session->takeSyncFrameReady());

    // A second frame inside the publish interval: the clear completes, the
    // hook stays quiet, and no capture replaces the pending none.
    fx.stream->toRead += "\x1b[?2026hquiet\x1b[?2026l";
    (void)fx.session->drain();
    CHECK(!fx.session->takeSyncFrameReady());

    // The window reopens on the following boundary, which captures the grid as it
    // stands then — both bodies included, nothing lost to the throttle.
    fx.drainSyncFrame("!");
    CHECK(fx.session->takeSyncFrameReady());
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);
    CHECK_EQ(frame->cursorColumn(), static_cast<int>("shownquiet!"sv.size()));
}

// A saturated drain publishes once per window, so a tick that took the newest
// capture inside that window owes nothing until the next one; a tick that comes
// a window or more after it would leave the glass waiting on a missed publish.
TEST(aSaturatedTickSkipsTheLiveReadOnlyWithinAWindowOfTheNewestCapture)
{
    SessionFixture fixture(6, 20);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("ready");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    fixture.session->setSyncPublishRefreshSeconds(1.0 / 60.0);
    constexpr std::uint32_t publishWindowMicroseconds = 1'000'000 / 120;
    // A capture younger than a window, but no saturation: the tick reads live.
    CHECK(!fixture.session->isDrainSaturated());
    CHECK(!fixture.session->shouldSkipLiveReadForSaturation());

    // Each scripted read outlasts the pass budget, so every pass ends on time
    // with the writer ahead and publishes its frame.
    fixture.stream->readMicroseconds = 10'000;
    const auto floodPass = [&](std::string_view bytes) {
        const auto publicationsBefore = fixture.session->syncFramePublicationCount();
        fixture.stream->toRead += bytes;
        CHECK(fixture.session->drain().end == Runtime::DrainPassEnd::TimeBudget);
        REQUIRE(fixture.session->isDrainSaturated());
        REQUIRE(fixture.session->syncFramePublicationCount() == publicationsBefore + 1);
        REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    };

    floodPass("\x1b[Hfirst");
    CHECK(fixture.session->shouldSkipLiveReadForSaturation());
    PAL::s_testMicroseconds += publishWindowMicroseconds - 100;
    CHECK(fixture.session->shouldSkipLiveReadForSaturation());
    PAL::s_testMicroseconds += 100;
    CHECK(!fixture.session->shouldSkipLiveReadForSaturation());

    // The age is the newest capture's, not the first one's.
    floodPass("\x1b[Hsecond");
    CHECK(fixture.session->shouldSkipLiveReadForSaturation());
    PAL::s_testMicroseconds += publishWindowMicroseconds;
    CHECK(!fixture.session->shouldSkipLiveReadForSaturation());
}

TEST(consecutiveTakesAlternateBuffersSoTheHeldFrameSurvivesTheNextCapture)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("aa");
    const TakenFrame first { fx.session->takeSyncFrame() };
    CHECK(first != nullptr);

    fx.drainSyncFrame("bb");
    const TakenFrame second { fx.session->takeSyncFrame() };
    CHECK(second != nullptr);
    // Different buffers: the drive's held frame is never the capture target,
    // so taking the next one hands over the opposite buffer.
    CHECK(first != second);
    CHECK_EQ(first->cursorColumn(), 2);
    CHECK_EQ(second->cursorColumn(), 4);
}

TEST(retainedFramesAreNeverRecycledAndAFullExchangeSkipsWithoutWaiting)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    TakenFrame first { fx.session->takeSyncFrame() };
    REQUIRE(first != nullptr);
    const std::vector<CellInstance> firstBytes = builtCells(*first);

    fx.drainSyncFrame("b");
    TakenFrame second { fx.session->takeSyncFrame() };
    REQUIRE(second != nullptr);
    const std::vector<CellInstance> secondBytes = builtCells(*second);

    fx.drainSyncFrame("c");
    TakenFrame third { fx.session->takeSyncFrame() };
    REQUIRE(third != nullptr);
    const std::vector<CellInstance> thirdBytes = builtCells(*third);

    fx.drainSyncFrame("d");
    CHECK(fx.session->takeSyncFrame() == nullptr);
    CHECK(fx.session->syncFrameCaptureAttemptCount() == 4);
    CHECK(fx.session->syncFramePublicationCount() == 3);
    CHECK(fx.session->syncFrameNoSlotCount() == 1);
    CHECK(sameInstanceBytes(firstBytes, builtCells(*first)));
    CHECK(sameInstanceBytes(secondBytes, builtCells(*second)));
    CHECK(sameInstanceBytes(thirdBytes, builtCells(*third)));

    first.reset();
    fx.drainSyncFrame("e");
    const TakenFrame resumed { fx.session->takeSyncFrame() };
    REQUIRE(resumed != nullptr);
    CHECK(resumed->sequence() == 4);
    CHECK_EQ(resumed->cursorColumn(), 5);
    CHECK(fx.session->syncFramePublicationCount() == 4);
}

TEST(aChangedBackgroundDiscardsTheQueuedStyle)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    auto geometry = testGeometry();
    fixture.session->setSyncFrameCaptureGeometry(geometry);
    fixture.drainSyncFrame("old background");
    geometry.style.uniformDefaultBackground = { 0.9f, 0.8f, 0.7f, 1.0f };
    fixture.session->setSyncFrameCaptureGeometry(geometry);
    const TakenFrame obsolete { fixture.session->takeSyncFrame() };
    CHECK(!obsolete);
    fixture.drainSyncFrame("new background");
    const TakenFrame current { fixture.session->takeSyncFrame() };
    REQUIRE(current != nullptr);
    CHECK(current->uniforms().defaultBackground.x == 0.9f);
    CHECK(current->uniforms().defaultBackground.z == 0.7f);
}

TEST(theRegisteredPlacementAndStyleReachTheBoundaryFrame)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    // The per-index opacity is the palette's own: the boundary capture reads
    // it off the model's palette under the lock, as the live capture does,
    // so there is no registration for a caller to scribble over. A slot
    // dimmed in the palette draws dimmed on the boundary frame.
    REQUIRE(withSessionModel(*fx.session, [](Terminal& terminal) {
        terminal.palette.indexedAlpha[7] = 0x80;
        return true;
    }));

    fx.drainSyncFrame("\x1b[37mstyled");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    REQUIRE(frame != nullptr);
    const std::vector<CellInstance> cells = builtCells(*frame);
    REQUIRE(!cells.empty());
    CHECK_MSG(cells[0].foreground.w > 0.49f && cells[0].foreground.w < 0.51f, "the dimmed slot's alpha reached the boundary frame's cell");
    const FrameUniforms uniforms = frame->uniforms();
    // The placement scalars crossed: a zero viewport is the decline the first
    // consumer hit, so the frame must report the registered surface.
    CHECK(uniforms.viewportSizeInPixels.x == 640.0f);
    CHECK(uniforms.viewportSizeInPixels.y == 384.0f);
    // The style crossed: the uniforms carry the registered default background.
    CHECK(uniforms.defaultBackground.x == 0.1f);
    CHECK(uniforms.defaultBackground.z == 0.3f);
}

TEST(theViewTierBuildDrawsTheCallersPresentTimeSelection)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("selected text");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);

    // A base build and a selection-overridden build over one capture:
    // the tier arrives at build time, and forcing the selected glyphs' ink
    // changes the built cells while the grid content stays the frame's.
    const std::vector<CellInstance> base = builtCells(*frame);
    const SelectionRowSpan span { /*visibleRow=*/0, /*column0=*/0, /*column1=*/8 };
    FrameCapture::ViewTierOverrides view;
    view.selectionSpans = &span;
    view.selectionSpanCount = 1;
    view.selectionColor = { 0.2f, 0.4f, 0.9f, 0.9f };
    view.hasSelectionForegroundOverride = true;
    view.selectionForeground = { 1.0f, 0.0f, 0.0f, 1.0f };
    const std::vector<CellInstance> selected = builtCells(*frame, &view);
    CHECK_EQ(selected.size(), base.size());
    CHECK(!sameInstanceBytes(base, selected));
    // The overrides do not stick to the capture: a plain rebuild matches the
    // original.
    CHECK(sameInstanceBytes(base, builtCells(*frame)));
}

TEST(theRegisteredRasterFactsSteerTheBoundaryFramesGlyphPhases)
{
    // The pen must land between buckets for a rounding rule to have anything
    // to decide: at a whole-pixel advance every column's fraction is zero,
    // floor and nearest agree, and a case built on it passes with the rule
    // deleted. At 8.3 px the third column's fraction is 0.9 — bucket 7 under
    // floor, bucket 0 of the next pixel under nearest.
    const auto phasedGeometry = [](bool isCoreTextRaster) {
        SyncFrameCaptureGeometry geometry = testGeometry();
        geometry.metrics.cellWidth = 8.3f;
        geometry.raster.subpixelPhaseCount = 8;
        geometry.raster.isCoreTextRaster = isCoreTextRaster;
        return geometry;
    };
    const auto phasesUnder = [&](bool isCoreTextRaster) {
        SessionFixture fx(24, 80);
        if (!fx.isReady) {
            CHECK_MSG(false, "frame fixture refused its stream");
            return std::vector<std::uint32_t> {};
        }
        fx.session->setSyncFrameCaptureGeometry(phasedGeometry(isCoreTextRaster));
        fx.drainSyncFrame("phases");
        const TakenFrame frame { fx.session->takeSyncFrame() };
        CHECK(frame != nullptr);
        return frame ? builtPhases(*frame) : std::vector<std::uint32_t> {};
    };

    // Same session, same bytes, same registration but for the one fact: the
    // boundary route reads the registration through Core's shared rule, so a
    // sheet that floors its pens phases these glyphs where the live route
    // phases them and not one bucket over. Left unread, both builds would be
    // the portable one and the text would shift sideways on every frame this
    // route produced — which, while a reader types, is every other frame.
    const std::vector<std::uint32_t> portable = phasesUnder(false);
    const std::vector<std::uint32_t> coreText = phasesUnder(true);
    CHECK(!portable.empty());
    CHECK_EQ(portable.size(), coreText.size());
    CHECK(portable != coreText);
}

TEST(theHoveredRunReachesTheBoundaryFrameAndSurvivesAReRegistration)
{
    // A link on the first row, and two colours far enough apart that a build
    // taking the resting tint cannot read the same as one taking the hover
    // tint — the comparison below is over instance bytes, so a near miss
    // between the two would leave it passing on either.
    const auto linkedGeometry = [] {
        SyncFrameCaptureGeometry geometry = testGeometry();
        geometry.shouldUnderlineLinks = true;
        geometry.linkColor = { 0.0f, 0.3f, 0.9f, 1.0f };
        geometry.linkHoverColor = { 1.0f, 0.4f, 0.0f, 1.0f };
        return geometry;
    };
    const std::string body = "\x1b]8;;https://example.invalid/one\x1b\\LINK\x1b]8;;\x1b\\ tail";

    const auto cellsUnder = [&](std::optional<Ubiquitty::Rendering::FrameHoverLink> hover, bool shouldReRegister) {
        SessionFixture fx(24, 80);
        if (!fx.isReady) {
            CHECK_MSG(false, "frame fixture refused its stream");
            return std::vector<CellInstance> {};
        }
        fx.session->setSyncFrameCaptureGeometry(linkedGeometry());
        if (hover)
            fx.session->setSyncFrameHoverLink(*hover);
        // A resize re-registers the placement. The hover is held beside that
        // registration rather than inside it, so it has to survive one — a
        // reader whose pointer rests on a link while the window changes shape
        // keeps the emphasis.
        if (shouldReRegister)
            fx.session->setSyncFrameCaptureGeometry(linkedGeometry());
        fx.drainSyncFrame(body);
        const TakenFrame frame { fx.session->takeSyncFrame() };
        CHECK(frame != nullptr);
        return frame ? builtCells(*frame) : std::vector<CellInstance> {};
    };

    Ubiquitty::Rendering::FrameHoverLink onTheLink;
    onTheLink.isActive = true;
    onTheLink.rowAbsoluteFirst = 0;
    onTheLink.rowAbsoluteLast = 0;
    onTheLink.columnFirst = 0;
    onTheLink.columnLast = 3;

    const std::vector<CellInstance> resting = cellsUnder(std::nullopt, false);
    CHECK(!resting.empty());

    // The registration reaches the build: without it the boundary route drew
    // every link resting, so a program redrawing under synchronized output
    // flickered the emphasis on and off under a motionless pointer.
    const std::vector<CellInstance> hovered = cellsUnder(onTheLink, false);
    CHECK_EQ(hovered.size(), resting.size());
    CHECK(!sameInstanceBytes(resting, hovered));

    // And it is the hovered CELLS that moved, not the registration merely
    // perturbing the build: the same call naming a row this frame does not
    // show reads exactly like no hover at all. Without this arm the case above
    // passes for a build that tinted the whole screen.
    Ubiquitty::Rendering::FrameHoverLink offScreen = onTheLink;
    offScreen.rowAbsoluteFirst = 10000;
    offScreen.rowAbsoluteLast = 10000;
    CHECK(sameInstanceBytes(resting, cellsUnder(offScreen, false)));

    // Held beside the placement, so re-registering the placement leaves it.
    CHECK(sameInstanceBytes(hovered, cellsUnder(onTheLink, true)));
}

TEST(aResizeMakesTheNextBoundaryFull)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[4;1Hb");
    {
        const TakenFrame second { fx.session->takeSyncFrame() };
        REQUIRE(second != nullptr);
        CHECK(second->recordKind() == FrameRecordKind::Delta);
    }
    // The grid the retained target holds is gone with its dimensions.
    fx.session->resize(20, 70, TerminalSession::ResizeGesture::Settled, Ubiquitty::Modes::AnchorMode::Top);
    fx.drainSyncFrame("\x1b[5;1Hc");
    {
        const TakenFrame resized { fx.session->takeSyncFrame() };
        REQUIRE(resized != nullptr);
        CHECK(resized->isFullFrame());
        CHECK(resized->rows() == 20);
    }
    fx.drainSyncFrame("\x1b[6;1Hd");
    const TakenFrame next { fx.session->takeSyncFrame() };
    REQUIRE(next != nullptr);
    CHECK(next->recordKind() == FrameRecordKind::Delta);
    CHECK(next->isRowDirty(5));
    CHECK(!next->isRowDirty(12));
}

TEST(aBlockThatScrollsKeepsTheLineageCoherent)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    // Thirty line feeds on a 24-row screen: the block scrolls the grid, so a
    // Delta must carry the shift or the frame must be Full — a Delta that
    // marked only the row the byte landed on would leave every scrolled
    // row where it was.
    fx.drainSyncFrame(std::string(30, '\n') + "z");
    const TakenFrame scrolled { fx.session->takeSyncFrame() };
    REQUIRE(scrolled != nullptr);
    CHECK(scrolled->isFullFrame() || scrolled->scrollDelta() != 0);
    if (!scrolled->isFullFrame()) {
        CHECK(scrolled->isRowDirty(23));
    }
}

TEST(aLiveFramePlannedBetweenBoundariesAdvancesTheSharedBaseline)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    // Plain output between two blocks reaches the screen through the live
    // route: its plan consumes the rows it will paint, against the baseline
    // the boundary captures share.
    fx.stream->toRead += "\x1b[8;1Hlive";
    (void)fx.session->drain();
    {
        const ModelLockHold hold(*fx.session->modelLockPointer());
        Ubiquitty::Terminal* term = fx.session->terminal();
        REQUIRE(term != nullptr);
        // The live request names the target the boundary captures record
        // against: the same cell height and texture the geometry holds.
        Ubiquitty::Rendering::RetainedFrameRequest request;
        request.rows = 24;
        request.columns = 80;
        request.cellHeightInPixels = 16;
        request.textureHeightInPixels = 384;
        request.isTargetMatching = true;
        std::array<std::uint8_t, 24> rowDirty {};
        const Ubiquitty::Rendering::RetainedFramePlan plan = fx.session->planRetainedFrame(*term, request, Ubiquitty::Rendering::RowDirtySpan(rowDirty));
        CHECK(plan.shouldLoadRetainedTarget);
        CHECK(rowDirty[7] != 0);
    }
    // The next boundary owes only its own row: the live row was presented
    // by the frame planned above, and the lineage is one.
    fx.drainSyncFrame("\x1b[12;1Hb");
    const TakenFrame next { fx.session->takeSyncFrame() };
    REQUIRE(next != nullptr);
    CHECK(next->recordKind() == FrameRecordKind::Delta);
    CHECK(next->isRowDirty(11));
    CHECK(!next->isRowDirty(7));
}

// A superseded boundary hands its rows to the next capture, but a scroll is
// not rows: the target never performed the superseded capture's blit, so every
// row the scroll moved stands where the last presented frame left it. The
// capture that supersedes it must repaint them all.
TEST(aSupersededScrollingBoundaryLeavesTheNextCaptureNoRowToKeep)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    std::string screen = "\x1b[H";
    for (int row = 1; row <= 24; ++row)
        screen += std::format("\x1b[{};1Hline {}", row, row);
    fx.drainSyncFrame(screen);
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[24;1H\r\nscrolled\r\nagain"); // scrolls two rows, never taken
    fx.drainSyncFrame("\x1b[5;1Hedit"); // supersedes it in the slot
    const TakenFrame newest { fx.session->takeSyncFrame() };
    REQUIRE(newest != nullptr);
    bool isEveryRowRepainted = newest->isFullFrame();
    if (!isEveryRowRepainted) {
        isEveryRowRepainted = true;
        for (int row = 0; row < 22; ++row)
            isEveryRowRepainted = isEveryRowRepainted && newest->isRowDirty(row);
    }
    CHECK_MSG(isEveryRowRepainted, "every row the superseded scroll moved is repainted by the capture that replaced it");
}

// A boundary capture advances the baseline the live route plans against, and
// the drive does not always take it: a frame with a selection on screen takes
// the live route with the capture still in the slot. The live frame then
// paints onto a target that never received the capture's rows, so those rows
// are its to paint — and the capture, older than the frame that superseded
// it, must not reach the glass after it.
TEST(aLiveFrameOverAnUntakenBoundaryPaintsItsRowsAndRetiresIt)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    // Published and left in the slot, as a frame drawn under a selection
    // leaves it.
    fx.drainSyncFrame("\x1b[5;1Hboundary");
    fx.stream->toRead += "\x1b[8;1Hlive";
    (void)fx.session->drain();
    {
        const ModelLockHold hold(*fx.session->modelLockPointer());
        Ubiquitty::Terminal* term = fx.session->terminal();
        REQUIRE(term != nullptr);
        Ubiquitty::Rendering::RetainedFrameRequest request;
        request.rows = 24;
        request.columns = 80;
        request.cellHeightInPixels = 16;
        request.textureHeightInPixels = 384;
        request.isTargetMatching = true;
        std::array<std::uint8_t, 24> rowDirty {};
        const Ubiquitty::Rendering::RetainedFramePlan plan = fx.session->planRetainedFrame(*term, request, Ubiquitty::Rendering::RowDirtySpan(rowDirty));
        CHECK(rowDirty[7] != 0);
        CHECK_MSG(!plan.shouldLoadRetainedTarget || rowDirty[4] != 0,
            "the untaken boundary's row is painted by the live frame that supersedes it");
    }
    const TakenFrame stale { fx.session->takeSyncFrame() };
    CHECK_MSG(stale == nullptr, "a boundary older than the live frame on the glass is not handed out after it");
}

UBIQUITTY_TEST_MAIN()

TEST(boundaryCaptureOwnsImagePixelsAndSquareGeometryAfterDeletion)
{
    for (const float scale : { 1.0f, 1.5f, 2.0f, 3.0f }) {
        SessionFixture fx(24, 80);
        REQUIRE(fx.isReady);
        auto geometry = testGeometry();
        geometry.metrics.deviceScale = scale;
        fx.session->setSyncFrameCaptureGeometry(geometry);
        fx.drainSyncFrame("\x1b_Ga=T,f=32,s=1,v=1,i=42,q=2;/////w==\x1b\\");
        const TakenFrame capture { fx.session->takeSyncFrame() };
        REQUIRE(capture != nullptr);
        REQUIRE(capture->imageQuads().size() == 1);
        const auto quad = capture->imageQuads()[0];
        // Natural protocol dimensions are device pixels, not points. The
        // retained frame uses the same units as the live image resolver.
        CHECK(quad.maxXInPixels - quad.minXInPixels == 1.0f);
        CHECK(quad.maxYInPixels - quad.minYInPixels == 1.0f);
        fx.drainSyncFrame("\x1b_Ga=d,d=I,i=42,q=2\x1b\\");
        const TakenFrame deleted { fx.session->takeSyncFrame() };
        REQUIRE(deleted != nullptr);
        CHECK(deleted->imageQuads().size() == 0);
        CHECK(capture->imageQuads().size() == 1);
        bool found = false;
        for (const auto& image : capture->images()) {
            if (image.used && image.imageID == 42) {
                REQUIRE(image.pixels != nullptr);
                CHECK(image.pixels[0] == 255);
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST(resizingDiscardsAQueuedFrameWithThePreviousViewport)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("before resize");
    auto geometry = testGeometry();
    geometry.viewportSizeInPixels.width = 1280.0f;
    fx.session->setSyncFrameCaptureGeometry(geometry);
    const TakenFrame stale { fx.session->takeSyncFrame() };
    CHECK(stale == nullptr);
    fx.drainSyncFrame("after resize");
    const TakenFrame current { fx.session->takeSyncFrame() };
    REQUIRE(current != nullptr);
    CHECK(current->isFullFrame());
    CHECK(current->uniforms().viewportSizeInPixels.x == 1280.0f);
}

TEST(originChangeDiscardsQueuedFrameWithoutResizingViewport)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    auto geometry = testGeometry();
    geometry.originInPixels.y = -6.0f;
    fx.session->setSyncFrameCaptureGeometry(geometry);
    fx.drainSyncFrame("during resize");
    geometry.originInPixels.y = 0.0f;
    fx.session->setSyncFrameCaptureGeometry(geometry);
    const TakenFrame stale { fx.session->takeSyncFrame() };
    CHECK(stale == nullptr);
    fx.drainSyncFrame("after resize");
    const TakenFrame current { fx.session->takeSyncFrame() };
    REQUIRE(current != nullptr);
    CHECK(current->isFullFrame());
}

TEST(fontPreparationIsBoundedDeduplicatedAndStyleAware)
{
    SessionFixture fx(4, 20);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("ASCII \xe4\xb8\xad\xe4\xb8\xad\x1b[1m\xe4\xb8\xad\x1b[0;3m\xe4\xb8\xad"
                      "\xe2\x96\x91\xf0\x9f\xac\x80\xf4\x8e\xbb\xae\x1b[8m\xce\xbb");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    REQUIRE(frame != nullptr);
    std::vector<std::uint64_t> prepared(FrameCapture::fontPreparationWordCount);
    CHECK(frame->takeFontPreparationKeys(prepared, 0).empty());
    const auto first = frame->takeFontPreparationKeys(prepared, 1);
    REQUIRE(first.size() == 1);
    CHECK(first[0] == 0x4e2d);
    const auto remaining = frame->takeFontPreparationKeys(prepared, 8);
    REQUIRE(remaining.size() == 2);
    CHECK(remaining[0] == (0x4e2d | (std::uint64_t { 1 } << 32)));
    CHECK(remaining[1] == (0x4e2d | (std::uint64_t { 2 } << 32)));
    CHECK(frame->takeFontPreparationKeys(prepared, 8).empty());
    CHECK(frame->hasCapturedDirtyRows());
}

// The boundary route carries the rows' OSC 133 marks, not only the mark face's
// geometry. The live route reads them off the Terminal it captures and the
// snapshot arm covers that; this route assembles its options from a
// registration, and a registration that carried the face without the marks
// would draw a frame whose gutters are empty beside a command that ran.
TEST(theRegisteredMarkFaceDrawsBracketsBesideACommandThatRan)
{
    SessionFixture fx(6, 10);
    REQUIRE(fx.isReady);
    SyncFrameCaptureGeometry geometry = testGeometry();
    // A 80x96 grid inset in a 120x96 surface: 20px of gutter each side, which
    // is the room resolvePromptMarkGeometry asks for.
    geometry.originInPixels = { 20.0f, 0.0f };
    geometry.viewportSizeInPixels = { 120.0f, 96.0f };
    geometry.cellHeightPoints = 16.0;
    geometry.promptMarkFontSize = 12.0;
    geometry.promptMarkFontAscent = 12.0f;
    geometry.promptMarkFontDescent = 4.0f;
    geometry.promptMarkAtlasScale = 1.0f;
    fx.session->setSyncFrameCaptureGeometry(geometry);
    fx.drainSyncFrame("\x1b]133;A\x07$ one\r\n\x1b]133;C\x07one\r\n\x1b]133;D;0\x07");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    REQUIRE(frame != nullptr);

    FakeAtlas atlas;
    const std::size_t capacity = static_cast<std::size_t>(frame->rows()) * frame->columns();
    std::vector<CellInstance> cells(capacity);
    std::vector<DecorationInstance> decorations(capacity * 8);
    std::vector<OverlayInstance> overlays(256);
    InstanceBuffers buffers;
    buffers.cells = cells.data();
    buffers.cellCapacity = cells.size();
    buffers.decorations = decorations.data();
    buffers.decorationCapacity = decorations.size();
    buffers.overlays = overlays.data();
    buffers.overlayCapacity = overlays.size();
    const InstanceCounts counts = frame->build(UbiquittyTest::glyphBackendFor(atlas), buffers, BuildScope::Whole);
    REQUIRE(counts.overlays <= overlays.size());
    // A bracket is a spine and two stubs in each gutter, and the prompt row is
    // the only row whose command ran: three quads left of the grid and three
    // right of it, all of them on row 0.
    int leftOfGrid = 0;
    int rightOfGrid = 0;
    for (std::size_t index = 0; index < counts.overlays; ++index) {
        const OverlayInstance& quad = overlays[index];
        if (quad.topLeftInPixels.y < 0.0f || quad.bottomRightInPixels.y > 16.0f)
            continue;
        if (quad.bottomRightInPixels.x <= 20.0f)
            ++leftOfGrid;
        else if (quad.topLeftInPixels.x >= 100.0f)
            ++rightOfGrid;
    }
    // Zero on either side is the defect this covers: a registration carrying the
    // face without the marks draws empty gutters beside a command that ran. The
    // count is the bracket's own shape — a spine and two stubs — so reshaping the
    // bracket moves it, and the two checks separate that from a route drawing
    // nothing at all.
    CHECK_MSG(leftOfGrid > 0 && rightOfGrid > 0,
        "the registered route drew no bracket beside a command that ran");
    CHECK_MSG(leftOfGrid == 3 && rightOfGrid == 3,
        "a bracket is a spine and two stubs in each gutter — this drew a different shape");
}

namespace {

// What a frame draws of a selection: its spans and where it begins and ends.
struct DrawnSelection {
    std::vector<SelectionRowSpan> spans;
    Selection::ViewportEnds ends;
};

bool operator==(const DrawnSelection& left, const DrawnSelection& right)
{
    const Selection::ViewportEnds& a = left.ends;
    const Selection::ViewportEnds& b = right.ends;
    const bool isSameEnds = a.isVisible == b.isVisible
        && (!a.isVisible
            || (a.startRow == b.startRow && a.startColumn == b.startColumn && a.endRow == b.endRow && a.endColumn == b.endColumn));
    return left.spans == right.spans && isSameEnds;
}

bool isSameState(const SelectionState& left, const SelectionState& right)
{
    return left.active == right.active && left.anchorRow == right.anchorRow && left.anchorColumn == right.anchorColumn
        && left.headRow == right.headRow && left.headColumn == right.headColumn && left.mode == right.mode
        && left.isAnchoredByCommand == right.isAnchoredByCommand && left.startSubColumn == right.startSubColumn
        && left.endSubColumn == right.endSubColumn;
}

// What the live read draws at this moment for a view whose session only the
// live read has kept, as the drive's live route keeps the view's: it
// synchronizes `reference` as it draws.
DrawnSelection liveRead(Selection::Session& reference, Terminal& terminal, int viewOffset)
{
    DrawnSelection drawn;
    drawn.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
    const int count = Selection::viewportSpansAndEnds(reference, terminal, viewOffset, drawn.spans, drawn.ends);
    drawn.spans.resize(static_cast<std::size_t>(count));
    return drawn;
}

// A frame's selection as the view drew it.
struct DrawnFrame {
    DrawnSelection drawn;
    Selection::CaptureSpans answer;
};

// A view's selection over a terminal, handed to the frames a drain captures
// and drawn from them: the two halves a session runs, the drain's under its
// lock and the view's on its own thread, without a session between them.
struct SelectionAtCaptureFixture {
    Terminal terminal;
    // The view's session: drawn from captures, and brought up to them.
    Selection::Session selection;
    Selection::PublishedSelection handedOff;
    // The drain's copy of the latest hand-off.
    Selection::PublishedSelection atCapture;
    Selection::CapturedSelection captured;
    std::array<RichText::BlockSelection, 2 * RichText::maxBlocksPerScreen> blocks {};

    SelectionAtCaptureFixture(int rows, int columns, int scrollbackLimit)
    {
        hardReset(terminal);
        resize(terminal, rows, columns);
        terminal.scrollbackLimit = scrollbackLimit;
        Selection::init(selection);
    }

    void feed(std::string_view bytes) { UbiquittyTest::feed(terminal, bytes); }
    [[nodiscard]] int history() const { return Screen::scrollbackCount(&terminal); }

    void drag(int fromRow, int fromColumn, int toRow, int toColumn, int clickCount = 1, Selection::GestureStyle style = {}, int viewOffset = 0)
    {
        Selection::beginGesture(selection, terminal, fromRow, fromColumn, clickCount, viewOffset, style);
        Selection::dragGesture(selection, terminal, toRow, toColumn, viewOffset);
        Selection::endGesture(selection);
    }

    // The view hands its selection over; the drain reads it at its next
    // capture.
    void handOff()
    {
        if (Selection::prepareHandOff(selection, handedOff))
            atCapture = handedOff;
    }

    // The drain captures a frame scrolled back `viewOffset` rows.
    void capture(int viewOffset) { static_cast<void>(Selection::resolveForCapture(atCapture, terminal, viewOffset, captured, blocks)); }

    // The view draws a frame's selection.
    DrawnFrame draw(const Selection::CapturedSelection& frame)
    {
        DrawnFrame drawn;
        drawn.drawn.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
        drawn.answer = Selection::takeCapturedSelection(selection, handedOff, frame, drawn.drawn.spans, drawn.drawn.ends);
        drawn.drawn.spans.resize(static_cast<std::size_t>(drawn.answer.count));
        return drawn;
    }
    DrawnFrame draw() { return draw(captured); }
};

} // namespace

// The selection a capture resolves is the one the live read draws over the
// terminal the frame froze: by character, word, line and rectangle, with an end
// on a wide glyph's second half, past the text, and scrolled back over the
// history. The view takes the resolved session back, as the live read leaves
// the session it draws.
TEST(aSelectionResolvedAtACaptureIsWhatTheLiveReadDraws)
{
    struct Gesture {
        const char* name;
        int fromRow;
        int fromColumn;
        int toRow;
        int toColumn;
        int clickCount;
        bool isRectangular;
    };
    constexpr std::array gestures {
        Gesture { "character", 0, 3, 2, 6, 1, false },
        Gesture { "backwards", 3, 4, 1, 2, 1, false },
        Gesture { "word", 0, 7, 2, 1, 2, false },
        Gesture { "line", 1, 4, 3, 0, 3, false },
        Gesture { "rectangle", 0, 2, 3, 8, 1, true },
        Gesture { "wide second half", 1, 6, 1, 9, 1, false },
        Gesture { "past the text", 2, 3, 3, 18, 1, false },
    };
    for (const Gesture& gesture : gestures) {
        SelectionAtCaptureFixture fixture(6, 20, 100);
        fixture.feed("alpha beta gamma\r\nwide \xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97 here\r\nthird line text\r\nfour\r\nfifth line");
        Selection::GestureStyle style;
        style.isRectangular = gesture.isRectangular;
        fixture.drag(gesture.fromRow, gesture.fromColumn, gesture.toRow, gesture.toColumn, gesture.clickCount, style);
        REQUIRE_MSG(Selection::hasSelection(fixture.selection), gesture.name);
        Selection::Session reference = fixture.selection;
        fixture.handOff();
        fixture.capture(0);
        const DrawnFrame frame = fixture.draw();
        CHECK_MSG(!frame.drawn.spans.empty(), gesture.name);
        CHECK_MSG(frame.drawn == liveRead(reference, fixture.terminal, 0), gesture.name);
        CHECK_MSG(frame.answer.didAdopt, gesture.name);
        CHECK_MSG(isSameState(fixture.selection.state, reference.state), gesture.name);
    }

    // Scrolled back: a selection made in the history, drawn at three offsets.
    SelectionAtCaptureFixture fixture(5, 20, 100);
    for (int line = 0; line < 12; ++line)
        fixture.feed(std::format("line {:02} of text\r\n", line));
    const int history = fixture.history();
    REQUIRE(history >= 6);
    fixture.drag(1, 2, 3, 9, 1, {}, history);
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    for (const int viewOffset : { history, history - 2, 0 }) {
        fixture.capture(viewOffset);
        const DrawnFrame frame = fixture.draw();
        CHECK(frame.drawn == liveRead(reference, fixture.terminal, viewOffset));
        CHECK(frame.answer.didAdopt);
    }
}

// Output that scrolls a selection into the history, and then evicts it, moves
// it at each capture where the live read would draw it then, and the frame
// whose capture finds its first line evicted draws none and leaves the view
// without it, the moment the live read drops it. The view never synchronizes
// its session against the terminal in between, as under a flood it never
// takes the lock.
TEST(aSelectionOutputScrollsIsResolvedAtEachCaptureAsTheLiveReadDoes)
{
    SelectionAtCaptureFixture fixture(4, 20, 6);
    for (int line = 0; line < 4; ++line)
        fixture.feed(std::format("line {:02} of text\r\n", line));
    fixture.drag(0, 5, 1, 9);
    REQUIRE(Selection::hasSelection(fixture.selection));
    Selection::Session reference = fixture.selection;
    fixture.handOff();

    int drawnAfterEviction = 0;
    bool wasLost = false;
    for (int line = 4; line < 20 && !wasLost; ++line) {
        const std::int64_t appendedBefore = Screen::scrollbackTotalAppended(&fixture.terminal);
        fixture.feed(std::format("line {:02} of text\r\n", line));
        REQUIRE(Screen::scrollbackTotalAppended(&fixture.terminal) > appendedBefore);
        for (const int viewOffset : { fixture.history(), 0 }) {
            const bool hadSelection = Selection::hasSelection(reference);
            fixture.capture(viewOffset);
            const DrawnFrame frame = fixture.draw();
            const DrawnSelection live = liveRead(reference, fixture.terminal, viewOffset);
            CHECK(frame.drawn == live);
            CHECK(frame.answer.didAdopt);
            CHECK(Selection::hasSelection(fixture.selection) == Selection::hasSelection(reference));
            CHECK(isSameState(fixture.selection.state, reference.state));
            if (hadSelection && !Selection::hasSelection(reference)) {
                CHECK(frame.answer.outcome == Selection::CaptureOutcome::Lost);
                CHECK(frame.drawn.spans.empty());
                wasLost = true;
            }
            const bool hasEvicted = Screen::scrollbackTotalAppended(&fixture.terminal) > fixture.history();
            drawnAfterEviction += hasEvicted && !frame.drawn.spans.empty();
        }
    }
    CHECK_MSG(drawnAfterEviction > 0, "a selection the history evicted rows above was drawn over a frame");
    CHECK_MSG(wasLost, "the flood evicted the selection's first line");
}

// A frame shows the selection as it stood when it was captured, against the
// terminal it froze, however late it is drawn. A view that already took a
// later frame's resolved session back keeps it.
TEST(aFrameShowsTheSelectionAsItStoodWhenItWasCaptured)
{
    SelectionAtCaptureFixture fixture(4, 20, 6);
    for (int line = 0; line < 7; ++line)
        fixture.feed(std::format("line {:02} of text\r\n", line));
    fixture.drag(0, 3, 1, 11);
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    fixture.feed("line 07 of text\r\nline 08 of text\r\n");
    REQUIRE(fixture.history() == fixture.terminal.scrollbackLimit);
    fixture.capture(2);
    const Selection::CapturedSelection older = fixture.captured;
    const DrawnSelection liveThen = liveRead(reference, fixture.terminal, 2);
    REQUIRE(!liveThen.spans.empty());

    fixture.feed("line 09 of text\r\n");
    fixture.capture(3);
    const DrawnFrame newer = fixture.draw();
    CHECK(newer.answer.didAdopt);
    CHECK(newer.answer.outcome == Selection::CaptureOutcome::Moved);
    CHECK(!newer.drawn.spans.empty());
    CHECK(newer.drawn == liveRead(reference, fixture.terminal, 3));
    REQUIRE(Selection::hasSelection(fixture.selection));
    REQUIRE(fixture.selection.state.anchorRow != older.session.state.anchorRow);

    const DrawnFrame late = fixture.draw(older);
    CHECK(late.drawn == liveThen);
    CHECK(!late.answer.didAdopt);
    CHECK(isSameState(fixture.selection.state, reference.state));
}

// Output moves the view's own position every delivery, which the drain never
// reads: a view whose position moved since the hand-off still takes the
// resolved selection back, and keeps the position it moved to.
TEST(aViewWhosePositionOutputMovedStillTakesTheResolvedSelectionBack)
{
    SelectionAtCaptureFixture fixture(4, 20, 6);
    for (int line = 0; line < 7; ++line)
        fixture.feed(std::format("line {:02} of text\r\n", line));
    fixture.drag(0, 3, 1, 11);
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    fixture.feed("line 07 of text\r\n");
    static_cast<void>(Selection::viewportAfterOutput(fixture.selection, 0, Input::OutputSnapPolicy::Indeterminate, Screen::historyFacts(&fixture.terminal)));
    REQUIRE(fixture.selection.hasViewportObservation);
    const Screen::HistoryFacts observed = fixture.selection.viewportObservedHistory;
    fixture.capture(1);
    const DrawnFrame frame = fixture.draw();
    CHECK(frame.answer.didAdopt);
    CHECK(frame.drawn == liveRead(reference, fixture.terminal, 1));
    CHECK(isSameState(fixture.selection.state, reference.state));
    CHECK(fixture.selection.hasViewportObservation);
    CHECK(fixture.selection.viewportObservedHistory.appendedRows == observed.appendedRows);
    CHECK(fixture.selection.viewportObservedHistory.retainedRows == observed.retainedRows);
}

// Output that erases the selected text leaves the live read nothing to draw.
// The frame captured after it draws nothing either, and the view's selection
// is gone with it.
TEST(aSelectionOutputShearedAwayIsNeverDrawnOverACapture)
{
    SelectionAtCaptureFixture fixture(6, 20, 100);
    fixture.feed("keep\r\nselect me\r\nand me\r\n");
    fixture.drag(1, 0, 2, 5);
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    fixture.capture(0);
    const DrawnFrame before = fixture.draw();
    CHECK(!before.drawn.spans.empty());
    CHECK(before.drawn == liveRead(reference, fixture.terminal, 0));

    fixture.feed("\x1b[2;1H\x1b[2Kreplaced\x1b[4;1H");
    fixture.capture(0);
    const DrawnFrame sheared = fixture.draw();
    const DrawnSelection live = liveRead(reference, fixture.terminal, 0);
    CHECK_MSG(live.spans.empty(), "the live read drops a selection whose line was erased");
    CHECK(sheared.drawn == live);
    CHECK(sheared.answer.outcome == Selection::CaptureOutcome::Lost);
    CHECK(sheared.answer.didAdopt);
    CHECK(!Selection::hasSelection(fixture.selection));
}

// A selection erased on the screen and scrolled into the history before any
// capture resolved it is drawn only where the live read draws it: nowhere,
// and the view loses it as the live read does.
TEST(reviewProbeShearedThenScrolledIntoHistory)
{
    SelectionAtCaptureFixture fixture(6, 20, 100);
    fixture.feed("keep\r\nselect me\r\nand me\r\n");
    fixture.drag(1, 0, 2, 5);
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    fixture.feed("\x1b[2;1H\x1b[2Kreplaced\x1b[3;1H\x1b[2Kother\x1b[6;1H");
    for (int line = 0; line < 8; ++line)
        fixture.feed(std::format("\r\nscroll {:02}", line));
    fixture.capture(fixture.history());
    const DrawnFrame frame = fixture.draw();
    const DrawnSelection live = liveRead(reference, fixture.terminal, fixture.history());
    CHECK(live.spans.empty());
    CHECK_MSG(frame.drawn == live, "a selection sheared on screen and then scrolled into history is drawn only where the live read draws it");
    CHECK(frame.answer.outcome == Selection::CaptureOutcome::Lost);
    CHECK(Selection::hasSelection(fixture.selection) == Selection::hasSelection(reference));
}

// A frame captured before the view moved its selection shows the selection
// the capture resolved, never the old bounds under the new selection's name:
// the view keeps the selection it moved to, and the next capture, which
// resolves the moved one, draws what the live read draws for it.
TEST(reviewProbeSelectionChangedSinceBasis)
{
    SelectionAtCaptureFixture fixture(6, 20, 100);
    fixture.feed("alpha beta gamma\r\nsecond line\r\nthird");
    fixture.drag(0, 0, 0, 4);
    Selection::Session first = fixture.selection;
    fixture.handOff();
    fixture.capture(0);
    const DrawnSelection firstLive = liveRead(first, fixture.terminal, 0);
    REQUIRE(!firstLive.spans.empty());

    fixture.drag(1, 0, 1, 5);
    const SelectionState moved = fixture.selection.state;
    Selection::Session second = fixture.selection;
    const DrawnFrame stale = fixture.draw();
    CHECK_MSG(stale.drawn == firstLive, "the frame shows the selection its capture resolved");
    CHECK_MSG(!stale.answer.didAdopt, "a frame resolved for the selection before the view moved it never replaces the moved one");
    CHECK(isSameState(fixture.selection.state, moved));

    fixture.handOff();
    fixture.capture(0);
    const DrawnFrame fresh = fixture.draw();
    const DrawnSelection secondLive = liveRead(second, fixture.terminal, 0);
    REQUIRE(!secondLive.spans.empty());
    CHECK(secondLive != firstLive);
    CHECK(fresh.drawn == secondLive);
    CHECK(fresh.answer.didAdopt);
}

// The view moves its selection and hands the move over before it draws a
// frame the drain captured for the hand-off before: the frame shows the
// selection it resolved, and the view keeps the one it moved to.
TEST(aFrameResolvedForAnEarlierHandOffNeverReplacesTheMovedSelection)
{
    SelectionAtCaptureFixture fixture(6, 20, 100);
    fixture.feed("alpha beta gamma\r\nsecond line\r\nthird");
    fixture.drag(0, 0, 0, 4);
    fixture.handOff();
    fixture.capture(0);
    const Selection::CapturedSelection earlier = fixture.captured;
    fixture.drag(1, 0, 1, 5);
    const SelectionState moved = fixture.selection.state;
    fixture.handOff();
    const DrawnFrame frame = fixture.draw(earlier);
    CHECK(!frame.drawn.spans.empty());
    CHECK(!frame.answer.didAdopt);
    CHECK(isSameState(fixture.selection.state, moved));
}

// An offset past the history shows the oldest rows, and on the alternate
// screen any offset shows the program's screen: a capture resolves the
// selection at the offset its rows are shown at, as the live read at that
// offset draws it.
TEST(aCaptureResolvesTheSelectionAtTheOffsetItsRowsAreShownAt)
{
    std::string lines;
    for (int line = 0; line < 12; ++line)
        lines += std::format("line {:02} of text\r\n", line);

    SelectionAtCaptureFixture pastTheHistory(5, 20, 100);
    pastTheHistory.feed(lines);
    const int history = pastTheHistory.history();
    pastTheHistory.drag(1, 2, 3, 9, 1, {}, history);
    Selection::Session reference = pastTheHistory.selection;
    pastTheHistory.handOff();
    pastTheHistory.capture(history + 4);
    const DrawnFrame oldest = pastTheHistory.draw();
    CHECK(!oldest.drawn.spans.empty());
    CHECK(oldest.drawn == liveRead(reference, pastTheHistory.terminal, history));

    SelectionAtCaptureFixture alternate(5, 20, 100);
    alternate.feed(lines);
    alternate.feed("\x1b[?1049h\x1b[Hprogram row one\r\nprogram row two\r\nprogram row three");
    REQUIRE(alternate.terminal.screens.isUsingAlternateScreen);
    REQUIRE(alternate.history() > 3);
    alternate.drag(0, 2, 2, 9);
    Selection::Session alternateReference = alternate.selection;
    alternate.handOff();
    alternate.capture(3);
    const DrawnFrame program = alternate.draw();
    CHECK(!program.drawn.spans.empty());
    CHECK(program.drawn == liveRead(alternateReference, alternate.terminal, 0));
}

// A press that has selected nothing yet is carried through output as the live
// read carries it, and is gone where the live read lets it go: once the row
// it was pressed on is evicted, no drag continues from it.
TEST(anOpenGestureWithNothingSelectedIsCarriedAsTheLiveReadCarriesIt)
{
    SelectionAtCaptureFixture fixture(4, 20, 2);
    for (int line = 0; line < 4; ++line)
        fixture.feed(std::format("line {:02} of text\r\n", line));
    Selection::beginGesture(fixture.selection, fixture.terminal, 1, 3, 1, 0, {});
    REQUIRE(Selection::isGestureOpen(fixture.selection));
    REQUIRE(!Selection::hasSelection(fixture.selection));
    Selection::Session reference = fixture.selection;
    fixture.handOff();
    bool wasClosed = false;
    for (int line = 4; line < 12 && !wasClosed; ++line) {
        fixture.feed(std::format("line {:02} of text\r\n", line));
        fixture.capture(0);
        const DrawnFrame frame = fixture.draw();
        CHECK(liveRead(reference, fixture.terminal, 0).spans.empty());
        CHECK(frame.drawn.spans.empty());
        CHECK(frame.answer.didAdopt);
        CHECK(Selection::isGestureOpen(fixture.selection) == Selection::isGestureOpen(reference));
        CHECK_EQ(fixture.selection.gestureAnchorAbsoluteRow, reference.gestureAnchorAbsoluteRow);
        wasClosed = !Selection::isGestureOpen(reference);
    }
    CHECK_MSG(wasClosed, "the flood evicted the row the press was on");
}

// Sized text resolves only through its run, which the cells alone do not
// answer: the capture asks the terminal the frame freezes, as the live read
// does, so the frame draws what the live read draws, down to the pieces that
// light a sized block's other rows when the selection holds one of its cells.
TEST(reviewProbeSizedTextAsksForTheLiveRead)
{
    struct Drag {
        int fromRow;
        int fromColumn;
        int toRow;
        int toColumn;
    };
    for (const Drag drag : { Drag { 0, 0, 3, 4 }, Drag { 1, 0, 1, 1 } }) {
        SelectionAtCaptureFixture fixture(8, 20, 100);
        fixture.terminal.drawsSizedText = true;
        fixture.terminal.drawsProportionalText = true;
        fixture.feed("plain\r\n\033]66;s=2;AB\007\r\n\r\nafter");
        fixture.drag(drag.fromRow, drag.fromColumn, drag.toRow, drag.toColumn);
        Selection::Session reference = fixture.selection;
        fixture.handOff();
        fixture.capture(0);
        const DrawnFrame frame = fixture.draw();
        const DrawnSelection live = liveRead(reference, fixture.terminal, 0);
        REQUIRE(!live.spans.empty());
        CHECK(frame.drawn == live);
        if (drag.fromRow == drag.toRow) {
            const bool hasBlockPiece = std::ranges::any_of(live.spans, [&](const SelectionRowSpan& span) { return span.visibleRow > live.ends.endRow; });
            CHECK_MSG(hasBlockPiece, "the live read lights the sized block's lower row, which no cell of the selection is on");
        }
    }
}

// Through output that moves the terminal's rows beneath a selection — the
// alternate screen entered and left inside a flood, the history cleared and
// filled again, a resize, a wrapped line selected whole in the history — the
// frame each capture draws is the live read's.
TEST(aSelectionResolvedAtEachCaptureFollowsOutputAsTheLiveReadDoes)
{
    struct Step {
        std::string bytes;
        int rows = 0;
        int columns = 0;
    };
    struct Scenario {
        const char* name;
        int rows;
        int columns;
        std::string setup;
        int fromRow;
        int fromColumn;
        int toRow;
        int toColumn;
        int clickCount;
        bool isInHistory;
        std::vector<Step> steps;
    };
    std::string lines;
    for (int line = 0; line < 12; ++line)
        lines += std::format("line {:02} of text\r\n", line);
    std::string refill;
    for (int line = 0; line < 20; ++line)
        refill += std::format("new  {:02} of text\r\n", line);
    const std::array scenarios {
        Scenario { "alternate round trip", 4, 20, lines, 0, 2, 1, 9, 1, false,
            { { "\033[?1049hALT SCREEN\r\nstuff\033[?1049lline 12 of text\r\nline 13 of text\r\n" } } },
        Scenario { "history cleared and filled again", 4, 20, lines, 0, 2, 1, 9, 1, false,
            { { "line 12 of text\r\nline 13 of text\r\nline 14 of text\r\nline 15 of text\r\n" }, { "\033[3J" }, { refill } } },
        Scenario { "resize", 4, 20, lines, 0, 2, 1, 9, 1, false,
            { { "line 12 of text\r\n" }, { "", 6, 12 }, { "more\r\n" } } },
        Scenario { "wrapped line selected whole in the history", 4, 10,
            "short\r\n0123456789abcdefghijklmnopqrst\r\nx\r\nl00\r\nl01\r\nl02\r\nl03\r\nl04\r\nl05\r\n", 2, 3, 2, 3, 3, true,
            { { "l06\r\n" }, { "l07\r\nl08\r\n" } } },
    };
    for (const Scenario& scenario : scenarios) {
        SelectionAtCaptureFixture fixture(scenario.rows, scenario.columns, 50);
        fixture.feed(scenario.setup);
        const int viewOffset = scenario.isInHistory ? fixture.history() : 0;
        fixture.drag(scenario.fromRow, scenario.fromColumn, scenario.toRow, scenario.toColumn, scenario.clickCount, {}, viewOffset);
        REQUIRE_MSG(Selection::hasSelection(fixture.selection), scenario.name);
        Selection::Session reference = fixture.selection;
        fixture.handOff();
        int drawnFrames = 0;
        for (const Step& step : scenario.steps) {
            if (step.rows > 0)
                resize(fixture.terminal, step.rows, step.columns);
            fixture.feed(step.bytes);
            for (const int offset : { fixture.history(), fixture.history() / 2, 0 }) {
                fixture.capture(offset);
                const DrawnFrame frame = fixture.draw();
                CHECK_MSG(frame.drawn == liveRead(reference, fixture.terminal, offset), scenario.name);
                CHECK_MSG(Selection::hasSelection(fixture.selection) == Selection::hasSelection(reference), scenario.name);
                drawnFrames += !frame.drawn.spans.empty();
            }
        }
        CHECK_MSG(drawnFrames > 0 || !Selection::hasSelection(reference), scenario.name);
    }
}

namespace {

// The drive's frame tick as it runs on the frontend's thread, while the test's
// thread holds the model lock the way a drain in the middle of a bite holds
// it: the frame asked of the session, which hands the selection over, takes
// the newest boundary frame and draws its selection.
struct FrontendTick {
    bool didFinishUnderTheLock = false;
    bool didTakeFrame = false;
    int frameViewOffset = 0;
    DrawnSelection drawn;
    Selection::CaptureSpans answer;
};

FrontendTick tickWhileTheDrainHoldsTheLock(TerminalSession& session, Selection::Session& selection)
{
    const Runtime::RuntimeSnapshot grid = session.readSnapshot();
    session.lockForRead();
    auto tick = std::async(std::launch::async, [&session, &selection, &grid] {
        FrontendTick result;
        result.drawn.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(grid.rows, grid.columns)));
        Runtime::FrameSupply supply;
        const TakenFrame frame { session.takeFrame({ .selection = &selection, .selectionSpans = result.drawn.spans }, supply) };
        if (!frame)
            return result;
        result.didTakeFrame = supply.route == Runtime::FrameRoute::Captured;
        result.frameViewOffset = frame->viewOffset();
        result.answer = supply.selection;
        result.drawn.ends = supply.selectionEnds;
        result.drawn.spans.resize(static_cast<std::size_t>(result.answer.count));
        return result;
    });
    const bool didFinish = tick.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    session.unlockForRead();
    FrontendTick result = tick.get();
    result.didFinishUnderTheLock = didFinish;
    return result;
}

// A pane flooded faster than its drain catches up, so each pass ends on its
// time budget and publishes the frame it leaves, through the paced host.
struct FloodedPane {
    SessionFixture fixture;
    Terminal* terminal = nullptr;
    Selection::Session selection;

    FloodedPane(int rows, int columns)
        : fixture(rows, columns)
    {
        REQUIRE(fixture.isReady);
        terminal = fixture.session->terminal();
        REQUIRE(terminal != nullptr);
        Selection::init(selection);
        fixture.session->setSyncFrameCaptureGeometry(testGeometry());
        fixture.drainSyncFrame("ready");
        REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
        fixture.session->setSyncPublishRefreshSeconds(1.0 / 60.0);
    }

    // One pass of the flood: the frame it captures is the terminal as the
    // pass leaves it.
    void floodPass(std::string_view bytes)
    {
        fixture.stream->readMicroseconds = 10'000;
        const auto publicationsBefore = fixture.session->syncFramePublicationCount();
        fixture.stream->toRead += bytes;
        CHECK(fixture.session->drain().end == Runtime::DrainPassEnd::TimeBudget);
        REQUIRE(fixture.session->syncFramePublicationCount() == publicationsBefore + 1);
    }

    // One synchronized update, captured at its end.
    void synchronizedFrame(std::string_view bytes)
    {
        fixture.stream->readMicroseconds = 0;
        Ubiquitty::PAL::s_testMicroseconds += 10'000;
        const auto publicationsBefore = fixture.session->syncFramePublicationCount();
        fixture.drainSyncFrame(bytes);
        REQUIRE(fixture.session->syncFramePublicationCount() > publicationsBefore);
    }

    void select(int fromRow, int fromColumn, int toRow, int toColumn, int viewOffset = 0)
    {
        Selection::beginGesture(selection, *terminal, fromRow, fromColumn, 1, viewOffset, {});
        Selection::dragGesture(selection, *terminal, toRow, toColumn, viewOffset);
        Selection::endGesture(selection);
        REQUIRE(Selection::hasSelection(selection));
        // The gesture hands the selection over as the drive's pointer path
        // does, before the next capture.
        fixture.session->publishSelectionForCaptures(&selection);
    }

    // The frame tick under the drain's lock beside the live read of the
    // moment its frame was captured, which is the terminal as it stands: the
    // tick resolves without waiting, presents a frame, and draws on it what
    // the live read draws, and the view's selection is the live read's.
    FrontendTick drawsWhatTheLiveReadDraws(Selection::Session& reference, int viewOffset, const char* what)
    {
        FrontendTick tick = tickWhileTheDrainHoldsTheLock(*fixture.session, selection);
        CHECK_MSG(tick.didFinishUnderTheLock, what);
        CHECK_MSG(tick.didTakeFrame, what);
        const DrawnSelection live = liveRead(reference, *terminal, viewOffset);
        CHECK_MSG(tick.drawn == live, what);
        CHECK_MSG(Selection::hasSelection(selection) == Selection::hasSelection(reference), what);
        CHECK_MSG(isSameState(selection.state, reference.state), what);
        return tick;
    }
};

// A full-screen program's frame, every row erased and rewritten in place.
std::string redrawnInPlace(int rows, int frame)
{
    std::string bytes;
    for (int row = 0; row < rows; ++row)
        bytes += std::format("\x1b[{};1H\x1b[2Kframe {:03} row {:02} redrawn in place", row + 1, frame, row);
    return bytes;
}

} // namespace

// A selection on the alternate screen of a program that redraws every row in
// place, under a flood the drain never catches up with: every frame the drain
// captures presents with the selection the live read draws, and no tick
// waits for the model lock. A scroll of the program's region carries the
// selection with its rows, as the live read carries it.
TEST(aSelectionOnTheAlternateScreenIsDrawnOnEveryFrameOfAnInPlaceRedraw)
{
    FloodedPane pane(24, 80);
    pane.floodPass("\x1b[?1049h" + redrawnInPlace(24, 0));
    REQUIRE(pane.terminal->screens.isUsingAlternateScreen);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(5, 2, 7, 20);
    Selection::Session reference = pane.selection;

    for (int frame = 1; frame <= 24; ++frame) {
        pane.floodPass(redrawnInPlace(24, frame));
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, 0, "an in-place redraw");
        CHECK_MSG(!tick.drawn.spans.empty(), "the redraw keeps the selection on its rows");
        CHECK(tick.answer.outcome == Selection::CaptureOutcome::Kept);
    }
    pane.floodPass("\x1b[1;24r\x1b[24;1H\n\x1b[r" + redrawnInPlace(3, 25));
    const FrontendTick scrolled = pane.drawsWhatTheLiveReadDraws(reference, 0, "a region scroll");
    CHECK_MSG(!scrolled.drawn.spans.empty(), "the scroll carries the selection with its rows");
    CHECK(scrolled.answer.outcome == Selection::CaptureOutcome::Moved);
    for (int frame = 26; frame <= 30; ++frame) {
        pane.floodPass(redrawnInPlace(24, frame));
        CHECK(!pane.drawsWhatTheLiveReadDraws(reference, 0, "an in-place redraw after the scroll").drawn.spans.empty());
    }
}

// A selection on a row outside the scrolling region, under synchronized
// updates that scroll everything below it: every update presents with the
// selection the live read draws, and no tick waits for the model lock.
TEST(aSelectionOutsideTheScrollRegionIsDrawnOnEveryFrameOfAFlood)
{
    FloodedPane pane(24, 80);
    pane.synchronizedFrame("\x1b[Hprotected marker row\x1b[2;24r\x1b[24;1H");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(0, 0, 0, 8);
    Selection::Session reference = pane.selection;

    int line = 0;
    for (int frame = 0; frame < 30; ++frame) {
        std::string lines;
        for (int count = 0; count < 7; ++count)
            lines += std::format("\r\nflood line {:05} below the protected row", line++);
        if (frame % 2 == 0)
            pane.synchronizedFrame(lines);
        else
            pane.floodPass(lines);
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, 0, "a scroll below the row");
        CHECK_MSG(!tick.drawn.spans.empty(), "the selection outside the region stands");
    }
}

// A selection on the screen that output scrolls into the history and out of
// it: every frame presents with the selection the live read draws, and the
// frame whose capture finds its first line evicted is the one the live read
// drops it at.
TEST(aSelectionScrolledByOutputIsDrawnOnEveryFrameUntilTheLiveReadDropsIt)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 20;
    pane.floodPass("first\r\nsecond selected line\r\nthird selected line\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(1, 0, 2, 12);
    Selection::Session reference = pane.selection;

    int drawn = 0;
    int line = 0;
    for (int frame = 0; frame < 20 && Selection::hasSelection(reference); ++frame) {
        std::string lines;
        for (int count = 0; count < 3; ++count)
            lines += std::format("scrolling line {:04}\r\n", line++);
        pane.floodPass(lines);
        drawn += !pane.drawsWhatTheLiveReadDraws(reference, 0, "scrolling output").drawn.spans.empty();
    }
    CHECK_MSG(drawn > 0, "frames drew the selection while it scrolled");
    CHECK_MSG(!Selection::hasSelection(reference), "the flood evicted the selection's first line");
    CHECK(!Selection::hasSelection(pane.selection));
}

// A view scrolled back over a selection in the history while output arrives
// below it: every frame presents with the selection the live read draws at
// the view's offset.
TEST(aSelectionInAViewScrolledBackIsDrawnOnEveryFrame)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 200;
    std::string history;
    for (int line = 0; line < 30; ++line)
        history += std::format("history line {:03}\r\n", line);
    pane.floodPass(history);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    constexpr int viewOffset = 10;
    SyncFrameCaptureGeometry geometry = testGeometry();
    geometry.viewOffset = viewOffset;
    pane.fixture.session->setSyncFrameCaptureGeometry(geometry);
    pane.select(3, 0, 5, 10, viewOffset);
    Selection::Session reference = pane.selection;

    int drawn = 0;
    for (int frame = 0; frame < 12; ++frame) {
        pane.floodPass(std::format("arriving below {:03}\r\n", frame));
        drawn += !pane.drawsWhatTheLiveReadDraws(reference, viewOffset, "a view scrolled back").drawn.spans.empty();
    }
    CHECK_MSG(drawn > 0, "the view drew the selection in the history");
}

// A view scrolled back holds its rows while output arrives, and the frames
// the drain captures for it hold the same rows: registered once, each frame
// stands where the view stands, with the selection on the rows it held.
TEST(aViewScrolledBackHoldsItsRowsInTheFramesTheDrainCaptures)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 200;
    std::string history;
    for (int line = 0; line < 30; ++line)
        history += std::format("history line {:03}\r\n", line);
    pane.floodPass(history);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    constexpr int viewOffset = 10;
    const Screen::HistoryFacts registeredHistory = Screen::historyFacts(pane.terminal);
    SyncFrameCaptureGeometry geometry = testGeometry();
    geometry.viewOffset = viewOffset;
    geometry.viewHistory = registeredHistory;
    geometry.isViewOffsetHeld = true;
    pane.fixture.session->setSyncFrameCaptureGeometry(geometry);
    pane.select(3, 0, 5, 10, viewOffset);
    Selection::Session reference = pane.selection;

    std::optional<DrawnSelection> firstDrawn;
    for (int frame = 0; frame < 12; ++frame) {
        pane.floodPass(std::format("arriving below {:03}\r\n", frame));
        const int heldOffset = Selection::heldViewOffset(viewOffset, registeredHistory, Screen::historyFacts(pane.terminal));
        REQUIRE(heldOffset == viewOffset + frame + 1);
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, heldOffset, "a view holding its rows");
        CHECK(tick.frameViewOffset == heldOffset);
        CHECK(!tick.drawn.spans.empty());
        if (!firstDrawn)
            firstDrawn = tick.drawn;
        CHECK_MSG(tick.drawn == *firstDrawn, "the held rows keep the selection where the view shows it");
    }
}

// A view reads the model under the lock after the drain appended history and
// before its own transition took it: the read stands where the frames the
// drain captures stand, held over the appended rows rather than at the view's
// own offset, and draws the selection on the rows they draw it on.
TEST(aLiveReadBeforeTheViewTakesOutputShowsTheRowsTheDrainCaptures)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 200;
    std::string history;
    for (int line = 0; line < 30; ++line)
        history += std::format("history line {:03}\r\n", line);
    pane.floodPass(history);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    constexpr int viewOffset = 10;
    static_cast<void>(Selection::viewportAfterOutput(pane.selection, viewOffset, Input::OutputSnapPolicy::Indeterminate, Screen::historyFacts(pane.terminal)));
    REQUIRE(Selection::isViewOffsetHeldByHistory(pane.selection));
    SyncFrameCaptureGeometry geometry = testGeometry();
    geometry.viewOffset = viewOffset;
    geometry.viewHistory = pane.selection.viewportObservedHistory;
    geometry.isViewOffsetHeld = true;
    pane.fixture.session->setSyncFrameCaptureGeometry(geometry);
    pane.select(3, 0, 5, 10, viewOffset);
    Selection::Session reference = pane.selection;

    for (int frame = 0; frame < 12; ++frame) {
        pane.floodPass(std::format("arriving below {:03}\r\n", frame));
        const int liveOffset = Selection::frameViewOffset(pane.selection, *pane.terminal, viewOffset);
        CHECK(liveOffset == viewOffset + frame + 1);
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, liveOffset, "a live read before the view takes output");
        CHECK(tick.frameViewOffset == liveOffset);
        CHECK_MSG(!tick.drawn.spans.empty(), "the selection stands on the held rows");
    }
}

// A frame stands where its view stands: held over the rows appended since a
// view held by history measured its offset, at its own offset for a view
// measured against nothing, against another epoch, or keeping a logical
// anchor, and in every case never past the history nor on the alternate
// screen, which has none.
TEST(aFrameViewOffsetHoldsOnlyAViewHeldByHistory)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 6, 20);
    terminal.scrollbackLimit = 40;
    int line = 0;
    const auto feedLines = [&](int count) {
        for (int fed = 0; fed < count; ++fed)
            UbiquittyTest::feed(terminal, std::format("line {:03}\r\n", line++));
    };
    const auto retainedRows = [&] { return Screen::historyFacts(&terminal).retainedRows; };
    const auto measure = [&](Selection::Session& view, int viewOffset) {
        static_cast<void>(Selection::viewportAfterOutput(view, viewOffset, Input::OutputSnapPolicy::Indeterminate, Screen::historyFacts(&terminal)));
    };
    feedLines(20);
    Selection::Session view;
    Selection::init(view);
    feedLines(3);
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == 4, "a view measured against nothing stands at its own offset");

    measure(view, 4);
    CHECK(Selection::frameViewOffset(view, terminal, 4) == 4);
    feedLines(3);
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == 7, "the rows appended since the view measured move it");
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 0) == 0, "a view at the tail follows it");
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 1000) == retainedRows(), "no frame stands past the history");

    resize(terminal, 7, 20);
    feedLines(2);
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == 4, "rows counted in another epoch move nothing");

    measure(view, 4);
    feedLines(100);
    REQUIRE(retainedRows() < 4 + 100);
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == retainedRows(), "a view whose rows were evicted stands at the oldest");

    UbiquittyTest::feed(terminal, "\x1b[?1049h");
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == 0, "the alternate screen has no history");
    UbiquittyTest::feed(terminal, "\x1b[?1049l");

    Selection::observeViewport(view, terminal, 4);
    feedLines(3);
    CHECK_MSG(Selection::frameViewOffset(view, terminal, 4) == 4, "a view keeping a logical anchor stands at its own offset");
}

// The offset a reader that missed every transition computes for a view that
// held its rows is the offset the view reached, transition by transition:
// growing history, history at its limit evicting the rows above, and the
// view pinned at the oldest row once its own rows are gone.
TEST(aHeldViewOffsetIsTheOffsetAViewThatHeldItsRowsReached)
{
    for (const int chunk : { 1, 3, 7 }) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 6, 20);
        terminal.scrollbackLimit = 40;
        Selection::Session view;
        Selection::init(view);
        for (int line = 0; line < 20; ++line)
            UbiquittyTest::feed(terminal, std::format("before {:03}\r\n", line));
        int viewOffset = 8;
        static_cast<void>(Selection::viewportAfterOutput(view, viewOffset, Input::OutputSnapPolicy::Indeterminate, Screen::historyFacts(&terminal)));
        const Screen::HistoryFacts measuredAgainst = view.viewportObservedHistory;
        int line = 0;
        bool wasPinned = false;
        while (line < 120) {
            for (int count = 0; count < chunk; ++count)
                UbiquittyTest::feed(terminal, std::format("after {:03}\r\n", line++));
            const Screen::HistoryFacts now = Screen::historyFacts(&terminal);
            const Input::ViewportAfterOutput after = Selection::viewportAfterOutput(view, viewOffset, Input::OutputSnapPolicy::Indeterminate, now);
            REQUIRE(after.hold == Input::ViewportHold::Hold);
            viewOffset = static_cast<int>(after.viewOffset);
            CHECK_EQ(Selection::heldViewOffset(8, measuredAgainst, now), viewOffset);
            wasPinned = wasPinned || viewOffset == now.retainedRows;
        }
        CHECK_MSG(wasPinned, "the view reached the oldest retained row");
    }
}

// A view's offset is held from the history its last output transition read:
// a view that has read none, or keeps a logical anchor, is not held by it.
TEST(aViewOffsetIsHeldByHistoryOnceMeasuredAgainstIt)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 6, 20);
    terminal.scrollbackLimit = 40;
    for (int line = 0; line < 20; ++line)
        UbiquittyTest::feed(terminal, std::format("before {:03}\r\n", line));
    Selection::Session view;
    Selection::init(view);
    CHECK(!Selection::isViewOffsetHeldByHistory(view));
    static_cast<void>(Selection::viewportAfterOutput(view, 4, Input::OutputSnapPolicy::Indeterminate, Screen::historyFacts(&terminal)));
    CHECK(Selection::isViewOffsetHeldByHistory(view));
    Selection::observeViewport(view, terminal, 4);
    CHECK(!Selection::isViewOffsetHeldByHistory(view));
}

// Two flooded panes, each with its own selection, one on the alternate screen
// redrawn in place and one scrolled by output: every frame of each presents
// with its own selection as its live read draws it.
TEST(aSelectionInEachOfTwoPanesIsDrawnOnEveryFrame)
{
    FloodedPane left(24, 80);
    FloodedPane right(12, 40);
    left.floodPass("\x1b[?1049h" + redrawnInPlace(24, 0));
    REQUIRE(TakenFrame(left.fixture.session->takeSyncFrame()));
    right.floodPass("one\r\ntwo selected\r\nthree\r\n");
    REQUIRE(TakenFrame(right.fixture.session->takeSyncFrame()));
    left.select(10, 0, 10, 30);
    right.select(1, 0, 1, 11);
    Selection::Session leftReference = left.selection;
    Selection::Session rightReference = right.selection;

    int rightDrawn = 0;
    for (int frame = 1; frame <= 12; ++frame) {
        left.floodPass(redrawnInPlace(24, frame));
        right.floodPass(std::format("right pane line {:03}\r\n", frame));
        CHECK(!left.drawsWhatTheLiveReadDraws(leftReference, 0, "the left pane").drawn.spans.empty());
        rightDrawn += !right.drawsWhatTheLiveReadDraws(rightReference, 0, "the right pane").drawn.spans.empty();
    }
    CHECK(Selection::hasSelection(left.selection));
    CHECK_MSG(rightDrawn > 0, "the right pane drew its selection until output scrolled it off the screen");
    CHECK(Selection::hasSelection(right.selection));
}

// The frame the drain captured is drawn with its selection while the drain
// holds the model lock: nothing on the way waits for it.
TEST(aSelectionResolvesOverATakenFrameWithoutTheModelLock)
{
    FloodedPane pane(24, 80);
    pane.synchronizedFrame("selected text\r\nand more of it");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(0, 2, 1, 6);
    Selection::Session reference = pane.selection;
    pane.synchronizedFrame("\x1b[3;1Hthird line");
    CHECK(!pane.drawsWhatTheLiveReadDraws(reference, 0, "a synchronized update").drawn.spans.empty());
}

// The view moves its selection during a flood, and the next tick hands the
// move over before it takes the frame the drain captured for the selection
// before: that frame never replaces the moved selection.
TEST(aTickThatHandsOverAMovedSelectionKeepsItOverAnEarlierFrame)
{
    FloodedPane pane(12, 40);
    pane.floodPass("first line here\r\nsecond line here\r\nthird line\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(0, 0, 0, 4);
    pane.floodPass("\x1b[5;1Hunder");
    Selection::beginGesture(pane.selection, *pane.terminal, 1, 0, 1, 0, {});
    Selection::dragGesture(pane.selection, *pane.terminal, 1, 6, 0);
    Selection::endGesture(pane.selection);
    const SelectionState moved = pane.selection.state;
    const FrontendTick tick = tickWhileTheDrainHoldsTheLock(*pane.fixture.session, pane.selection);
    CHECK(tick.didTakeFrame);
    CHECK(!tick.answer.didAdopt);
    CHECK(isSameState(pane.selection.state, moved));
}

// A view held back over the history shows the program's screen once the
// program enters the alternate screen, which has no history to hold: each
// frame the drain captures for it stands at the tail, with the selection the
// live read draws there.
TEST(aViewHeldBackIsShownTheAlternateScreenAtTheTail)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 200;
    std::string history;
    for (int line = 0; line < 30; ++line)
        history += std::format("history line {:03}\r\n", line);
    pane.floodPass(history);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    SyncFrameCaptureGeometry geometry = testGeometry();
    geometry.viewOffset = 10;
    geometry.viewHistory = Screen::historyFacts(pane.terminal);
    geometry.isViewOffsetHeld = true;
    pane.fixture.session->setSyncFrameCaptureGeometry(geometry);
    pane.floodPass("\x1b[?1049h" + redrawnInPlace(12, 0));
    REQUIRE(pane.terminal->screens.isUsingAlternateScreen);
    REQUIRE(Screen::scrollbackCount(pane.terminal) > geometry.viewOffset);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(5, 2, 7, 20);
    Selection::Session reference = pane.selection;

    for (int frame = 1; frame <= 6; ++frame) {
        pane.floodPass(redrawnInPlace(12, frame));
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, 0, "the alternate screen under a held view");
        CHECK(tick.frameViewOffset == 0);
        CHECK(!tick.drawn.spans.empty());
    }
}

// The live route hands Core the offset the view holds, as it stands, where the
// boundary capture resolves it first. Over the alternate screen under a view
// held back the two routes draw one frame: the press and the drag land on the
// rows the frame shows, the frames stand at one offset over one set of rows
// with the cursor on one row, the selection draws alike, and the live route
// plans against the baseline the boundary capture left rather than reading a
// different offset as a reason to rebuild whole.
TEST(theLiveAndBoundaryRoutesDrawOneFrameOverTheAlternateScreenUnderAHeldView)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 200;
    std::string history;
    for (int line = 0; line < 30; ++line)
        history += std::format("history line {:03}\r\n", line);
    pane.floodPass(history);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    SyncFrameCaptureGeometry geometry = testGeometry();
    geometry.viewOffset = 10;
    geometry.viewHistory = Screen::historyFacts(pane.terminal);
    geometry.isViewOffsetHeld = true;
    pane.fixture.session->setSyncFrameCaptureGeometry(geometry);
    const std::string cursorHome = "\x1b[9;5H";
    pane.floodPass("\x1b[?1049h" + redrawnInPlace(12, 0) + cursorHome);
    REQUIRE(pane.terminal->screens.isUsingAlternateScreen);
    REQUIRE(Screen::scrollbackCount(pane.terminal) > geometry.viewOffset);
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));

    Selection::Session atTail;
    Selection::init(atTail);
    Selection::beginGesture(atTail, *pane.terminal, 5, 2, 1, 0, {});
    Selection::dragGesture(atTail, *pane.terminal, 7, 20, 0);
    Selection::endGesture(atTail);
    pane.select(5, 2, 7, 20, geometry.viewOffset);
    CHECK_MSG(Selection::selectedText(pane.selection, *pane.terminal) == Selection::selectedText(atTail, *pane.terminal),
        "a drag at the held offset selected other rows than the frame shows");
    Selection::Session reference = pane.selection;

    pane.floodPass(redrawnInPlace(12, 1) + cursorHome);
    const TakenFrame boundary { pane.fixture.session->takeSyncFrame() };
    REQUIRE(boundary);
    REQUIRE(boundary->cursorRow() == 8);
    const TakenFrame live { FrameCapture::make() };
    REQUIRE(live);
    pane.fixture.session->lockForRead();
    RetainedFrameRequest request;
    request.rows = pane.terminal->rows;
    request.columns = pane.terminal->columns;
    request.viewOffset = geometry.viewOffset;
    request.isTargetMatching = true;
    request.isGridCacheEnabled = true;
    std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(request.rows), 0);
    const RetainedFramePlan plan = pane.fixture.session->planRetainedFrame(*pane.terminal, request, std::span { rowDirty });
    TerminalSnapshotOptions options;
    options.viewOffset = geometry.viewOffset;
    options.drawCursor = geometry.drawCursor;
    options.isFocused = geometry.isFocused;
    options.viewportSizeInPixels = geometry.viewportSizeInPixels;
    options.style = geometry.style;
    const bool didCaptureLive = live->capture(*pane.terminal, options, geometry.metrics, geometry.glyphSlot, plan.refresh, {});
    pane.fixture.session->unlockForRead();
    REQUIRE(didCaptureLive);
    CHECK_MSG(plan.damage.canReuse, "the live route rebuilt a frame the boundary capture had already drawn");
    CHECK_EQ(plan.refresh.viewOffset, boundary->viewOffset());
    CHECK_EQ(plan.refresh.firstAbsoluteRow, boundary->firstAbsoluteRow());
    CHECK_EQ(live->viewOffset(), boundary->viewOffset());
    CHECK_EQ(live->firstAbsoluteRow(), boundary->firstAbsoluteRow());
    CHECK_EQ(live->cursorRow(), boundary->cursorRow());

    pane.floodPass(redrawnInPlace(12, 2) + cursorHome);
    const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, geometry.viewOffset, "the live route's selection under a held view");
    CHECK(tick.frameViewOffset == 0);
    CHECK(!tick.drawn.spans.empty());
}

// Each delivery's output transition, which the view runs on its own thread
// beside the frames it takes, moves only the view's position over a standing
// selection, and no hand-off carries that: every frame the drain captured
// before a delivery still brings the view's selection up to what output did
// to it, down to the frame that loses it.
TEST(aDeliveryLeavesAStandingSelectionToTheFramesTheDrainCaptures)
{
    FloodedPane pane(12, 40);
    pane.terminal->scrollbackLimit = 20;
    pane.floodPass("first\r\nsecond selected line\r\nthird selected line\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(1, 0, 2, 12);
    Selection::Session reference = pane.selection;

    int line = 0;
    for (int frame = 0; frame < 20 && Selection::hasSelection(reference); ++frame) {
        std::string lines;
        for (int count = 0; count < 3; ++count)
            lines += std::format("scrolling line {:04}\r\n", line++);
        pane.floodPass(lines);
        const Selection::OutputUpdate delivery = pane.fixture.session->updateSelectionAfterOutput(&pane.selection, 0, Screen::historyFacts(pane.terminal));
        CHECK(!delivery.didChangeSelection);
        const FrontendTick tick = pane.drawsWhatTheLiveReadDraws(reference, 0, "a frame taken after a delivery");
        CHECK(tick.answer.didAdopt);
    }
    CHECK_MSG(!Selection::hasSelection(reference), "the flood evicted the selection's first line");
    CHECK(!Selection::hasSelection(pane.selection));
}

// The rows a selection behind the text leaves or changes are rebuilt from the
// held capture beside its own damage, and a held selection owes none.
TEST(aRowASelectionLeavesIsRebuiltFromTheHeldCapture)
{
    SessionFixture fx(24, 80);
    REQUIRE(fx.isReady);
    fx.session->setSyncFrameCaptureGeometry(testGeometry());
    fx.drainSyncFrame("a");
    {
        const TakenFrame first { fx.session->takeSyncFrame() };
        REQUIRE(first != nullptr);
    }
    fx.drainSyncFrame("\x1b[4;1Hb");
    const TakenFrame delta { fx.session->takeSyncFrame() };
    REQUIRE(delta != nullptr);
    REQUIRE(delta->recordKind() == FrameRecordKind::Delta);
    REQUIRE(!delta->isRowDirty(0));

    const SelectionRowSpan held[] = { { /*visibleRow=*/0, /*column0=*/0, /*column1=*/4 } };
    const Vec4 color { 0.2f, 0.4f, 0.9f, 1.0f };
    std::vector<std::uint8_t> repaint(static_cast<std::size_t>(delta->rows()), 0);
    markSelectionRepaintRows(held, color, held, color, delta->frameDamage(), repaint);
    CHECK_MSG(std::ranges::none_of(repaint, [](std::uint8_t row) { return row != 0; }), "a held selection owes no row");
    markSelectionRepaintRows(held, color, {}, color, delta->frameDamage(), repaint);
    CHECK(repaint[0] != 0);
    CHECK(std::ranges::count_if(repaint, [](std::uint8_t row) { return row != 0; }) == 1);

    FrameCapture::ViewTierOverrides damaged;
    FrameCapture::ViewTierOverrides repainted;
    repainted.overlayDirtyRows = repaint.data();
    repainted.overlayDirtyRowCount = static_cast<int>(repaint.size());
    const std::size_t damagedCells = builtCells(*delta, &damaged).size();
    const std::size_t repaintedCells = builtCells(*delta, &repainted).size();
    CHECK_MSG(repaintedCells > damagedCells, "the row the selection left was rebuilt beside the capture's damage");

    // A scroll carries the held selection with its rows: both places owe.
    FrameDamage scrolled;
    scrolled.scrollTop = 0;
    scrolled.scrollBottom = 23;
    scrolled.scrollDelta = 1;
    const SelectionRowSpan lower[] = { { /*visibleRow=*/5, /*column0=*/0, /*column1=*/4 } };
    std::ranges::fill(repaint, std::uint8_t { 0 });
    markSelectionRepaintRows(lower, color, lower, color, scrolled, repaint);
    CHECK(repaint[5] != 0);
    CHECK(repaint[4] != 0);
}

namespace {

// One tick's question, with a span buffer sized for the session's grid.
struct FrameAsk {
    Runtime::FrameSupply supply;
    TakenFrame frame;
    std::vector<SelectionRowSpan> spans;
};

FrameAsk askForFrame(TerminalSession& session, Runtime::FrameDemand demand)
{
    FrameAsk ask;
    const Runtime::RuntimeSnapshot grid = session.readSnapshot();
    ask.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(grid.rows, grid.columns)));
    demand.selectionSpans = ask.spans;
    ask.frame = TakenFrame(session.takeFrame(demand, ask.supply));
    ask.spans.resize(static_cast<std::size_t>(ask.supply.selection.count));
    return ask;
}

} // namespace

// A capture says whether it draws a status row, so a view reads it from the
// frame it presents rather than from the model: none before the program asks
// for the indicator line, one after.
TEST(aCaptureSaysWhetherItDrawsAStatusRow)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("no status line");
    {
        const TakenFrame plain { fixture.session->takeSyncFrame() };
        REQUIRE(plain != nullptr);
        CHECK(!plain->hasStatusRow());
    }
    fixture.drainSyncFrame("\x1b[1$~indicator");
    const TakenFrame indicated { fixture.session->takeSyncFrame() };
    REQUIRE(indicated != nullptr);
    CHECK(indicated->hasStatusRow());
}

// A capture waiting in the slot is the tick's frame, with the selection the
// drain resolved for it: the spans the live read draws over the terminal the
// frame froze.
TEST(aTickTakesTheNewestCaptureWithTheSelectionResolvedForIt)
{
    FloodedPane pane(24, 80);
    pane.floodPass("first line\r\nsecond line selected\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(1, 0, 1, 10);
    Selection::Session reference = pane.selection;
    pane.floodPass("third line\r\n");

    const FrameAsk ask = askForFrame(*pane.fixture.session, { .selection = &pane.selection });
    REQUIRE(ask.supply.route == Runtime::FrameRoute::Captured);
    CHECK(ask.supply.decline == Runtime::FrameDecline::None);
    REQUIRE(ask.frame != nullptr);
    const DrawnSelection live = liveRead(reference, *pane.terminal, 0);
    CHECK(!live.spans.empty());
    CHECK(ask.spans == live.spans);
    CHECK(ask.supply.selectionEnds.isVisible == live.ends.isVisible);
    CHECK_EQ(ask.supply.selectionEnds.startRow, live.ends.startRow);

    // The slot is drained: the next tick of this pane, which no loop drains,
    // is served a capture its own ask made.
    const FrameAsk next = askForFrame(*pane.fixture.session, { .selection = &pane.selection });
    REQUIRE(next.supply.route == Runtime::FrameRoute::Captured);
    REQUIRE(next.frame != nullptr);
    CHECK(next.frame->sequence() > ask.frame->sequence());
}

// A buffer with no room for every span of the frame's selection, as one sized
// from a grid read before the program widened the screen has none, receives
// the spans it has room for and learns how many the frame has. Taking the
// held frame again into a buffer that size writes them all, and adopts
// nothing the first take did not.
TEST(aBufferTooSmallForTheFramesSpansLearnsHowManyItNeeds)
{
    FloodedPane pane(24, 80);
    pane.floodPass("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(0, 0, 5, 2);
    Selection::Session reference = pane.selection;
    pane.floodPass("seven\r\n");
    const DrawnSelection live = liveRead(reference, *pane.terminal, 0);
    REQUIRE(live.spans.size() > 2);

    std::vector<SelectionRowSpan> room(2);
    Runtime::FrameSupply supply;
    const TakenFrame frame { pane.fixture.session->takeFrame({ .selection = &pane.selection, .selectionSpans = room }, supply) };
    REQUIRE(supply.route == Runtime::FrameRoute::Captured);
    REQUIRE(frame != nullptr);
    CHECK_EQ(supply.selection.count, 2);
    CHECK_EQ(supply.selection.needed, static_cast<int>(live.spans.size()));
    CHECK(std::ranges::equal(room, std::span(live.spans).first(2)));
    CHECK(supply.selection.didAdopt);
    const Selection::Session adopted = pane.selection;

    std::vector<SelectionRowSpan> whole(static_cast<std::size_t>(supply.selection.needed));
    Selection::ViewportEnds ends;
    const Selection::CaptureSpans refill = pane.fixture.session->selectionOverCapture(&pane.selection, *frame, whole, ends);
    CHECK_EQ(refill.count, supply.selection.needed);
    CHECK_EQ(refill.needed, supply.selection.needed);
    CHECK(whole == live.spans);
    CHECK_EQ(ends.startRow, supply.selectionEnds.startRow);
    CHECK(!refill.didAdopt);
    CHECK(isSameState(pane.selection.state, adopted.state));
}

// A view that moves its selection after the drain captured a frame draws,
// over that frame, the selection it holds rather than the one the frame
// resolved: what the live read draws over the terminal the frame froze, from
// takeFrame itself and from a refill. A view that moved nothing is not ahead
// of its frame, and draws the frame's.
TEST(aViewThatMovedItsSelectionAfterTheCaptureDrawsItsOwnOverTheFrame)
{
    FloodedPane pane(24, 80);
    pane.floodPass("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(0, 0, 1, 2);
    pane.floodPass("seven\r\n");

    // Unmoved: the frame's resolution is the view's.
    {
        std::vector<SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(24, 80)));
        Runtime::FrameSupply supply;
        const TakenFrame frame { pane.fixture.session->takeFrame({ .selection = &pane.selection, .selectionSpans = spans }, supply) };
        REQUIRE(supply.route == Runtime::FrameRoute::Captured);
        CHECK(!supply.selection.isViewAhead);
    }

    pane.select(2, 1, 4, 3);
    pane.floodPass("eight\r\n");
    // The view moves again after this capture: the frame resolved the
    // previous hand-off.
    pane.select(3, 0, 5, 1);
    Selection::Session reference = pane.selection;
    const DrawnSelection live = liveRead(reference, *pane.terminal, 0);
    REQUIRE(!live.spans.empty());

    std::vector<SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(24, 80)));
    Runtime::FrameSupply supply;
    const TakenFrame frame { pane.fixture.session->takeFrame({ .selection = &pane.selection, .selectionSpans = spans }, supply) };
    REQUIRE(supply.route == Runtime::FrameRoute::Captured);
    REQUIRE(frame != nullptr);
    CHECK(supply.selection.isViewAhead);
    CHECK(!supply.selection.didAdopt);
    spans.resize(static_cast<std::size_t>(supply.selection.count));
    CHECK(spans == live.spans);

    // The frame's own resolution is where the selection stood.
    DrawnSelection framesOwn;
    framesOwn.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(24, 80)));
    const Selection::CaptureSpans frames = pane.fixture.session->selectionOverCapture(&pane.selection, *frame, framesOwn.spans, framesOwn.ends);
    framesOwn.spans.resize(static_cast<std::size_t>(frames.count));
    CHECK(frames.isViewAhead);
    CHECK(framesOwn.spans != live.spans);

    const Selection::Session before = pane.selection;
    DrawnSelection own;
    own.spans.resize(static_cast<std::size_t>(Selection::viewportSpanCapacity(24, 80)));
    const Selection::CaptureSpans answer = pane.fixture.session->viewSelectionOverCapture(&pane.selection, *frame, own.spans, own.ends);
    own.spans.resize(static_cast<std::size_t>(answer.count));
    CHECK(answer.isViewAhead);
    CHECK_EQ(answer.needed, answer.count);
    CHECK(own == live);
    CHECK(isSameState(pane.selection.state, before.state));
}

// A view whose last frame drew search matches reads the model, saturated or
// not, and the capture stays in the slot for the tick that stops drawing them.
TEST(aViewKeepingSearchLiveReadsTheModelAndLeavesTheCaptureWaiting)
{
    FloodedPane pane(6, 20);
    pane.floodPass("\x1b[Hsearched");
    REQUIRE(pane.fixture.session->shouldSkipLiveReadForSaturation());
    const FrameAsk search = askForFrame(*pane.fixture.session, { .keepsSearchLive = true });
    CHECK(search.supply.route == Runtime::FrameRoute::LiveRead);
    CHECK(search.frame == nullptr);

    const FrameAsk plain = askForFrame(*pane.fixture.session, {});
    CHECK(plain.supply.route == Runtime::FrameRoute::Captured);
    CHECK(plain.frame != nullptr);
}

// With no capture waiting, a frame the coherence rule holds is declined for
// the rule, and a view being resized does not wait for quiet output: the
// asking thread, which drives this pane, captures the model the rule still
// holds.
TEST(aFrameTheRuleHoldsIsDeclinedUnlessTheViewIsResizing)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("ready");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    fixture.stream->toRead += "plain output the rule holds for quiet";
    openPublishWindow();
    static_cast<void>(fixture.session->drain());
    REQUIRE(fixture.session->isFrameHeld());
    REQUIRE(!fixture.session->isDrainSaturated());

    const FrameAsk held = askForFrame(*fixture.session, {});
    CHECK(held.supply.route == Runtime::FrameRoute::Declined);
    CHECK(held.supply.decline == Runtime::FrameDecline::HeldByCoherence);
    CHECK_MSG(held.supply.askAgainTicks == 0, "the rule's release wakes the view, so it waits out no budget");
    CHECK(held.frame == nullptr);
    CHECK(fixture.session->shouldWithholdFrame(Ticks(0)));

    const FrameAsk resizing = askForFrame(*fixture.session, { .isResizing = true });
    CHECK_MSG(resizing.supply.route == Runtime::FrameRoute::Captured, "the resize is served the model the rule still holds");
    CHECK(resizing.frame != nullptr);
    CHECK(fixture.session->isFrameHeld());
    fixture.session->lockForRead();
    CHECK(fixture.session->liveReadDecline(*fixture.session->terminal(), { .isResizing = true }).decline == Runtime::FrameDecline::None);
    fixture.session->unlockForRead();
}

// A view that stops resizing asks by the coherence rule again: its demand is
// held while the rule holds the frame.
TEST(aDemandRaisedAfterTheResizeIsHeldForTheRule)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("ready");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    fixture.stream->toRead += "plain output the rule holds for quiet";
    openPublishWindow();
    static_cast<void>(fixture.session->drain());
    REQUIRE(fixture.session->isFrameHeld());

    REQUIRE(askForFrame(*fixture.session, { .isResizing = true }).supply.route == Runtime::FrameRoute::Captured);
    CHECK(askForFrame(*fixture.session, {}).supply.decline == Runtime::FrameDecline::HeldByCoherence);
    CHECK_MSG(!TakenFrame(fixture.session->takeSyncFrame()), "the newest demand's rule held the frame");
}

// A view revealed over a lane that has never captured, with no output to
// carry a frame, is served its first frame at its ask, by the thread that
// drives the pane. A pane its ingest loop drains is served at the loop's turn
// instead (test_ingest_loop).
TEST(aRevealedLaneThatNeverCapturedIsServedAtTheAsk)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Hidden);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.session->setSyncFramePresentation(Ubiquitty::Runtime::PanePresentation::Presented);
    REQUIRE(!fixture.session->isFrameHeld());

    const FrameAsk served = askForFrame(*fixture.session, {});
    CHECK(served.supply.route == Runtime::FrameRoute::Captured);
    CHECK(served.frame != nullptr);
}

namespace {

// Lets the rule release a frame it holds for quiet, as the loop's watchdog
// does once the output has paused.
void releaseFrameHeldForQuiet(TerminalSession& session)
{
    if (const std::optional<std::uint64_t> due = session.publishHeldFrameIfDue())
        PAL::s_testMicroseconds += static_cast<std::uint32_t>(*due);
    static_cast<void>(session.publishHeldFrameIfDue());
}

} // namespace

// Tek mode on, then GS, a dark move to (2000,1000) and a vector to (100,200).
constexpr std::string_view tekEnterAndDraw = "\x1b[?38h\x1d\x27\x7a\x2f\x54\x21\x72\x20\x59";
// GS, a dark move to (100,200), and a vector back to (2000,1000).
constexpr std::string_view tekSecondVector = "\x1d\x21\x72\x20\x59\x27\x7a\x2f\x54";

// A Tek frame's capture carries what the tube displays and the host's colour
// requests, so a view draws the tube without the model; a capture of the grid
// carries no tube.
TEST(aTekFramesCaptureCarriesTheTube)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("grid");
    {
        const TakenFrame grid { fixture.session->takeSyncFrame() };
        REQUIRE(grid != nullptr);
        CHECK(!grid->isTekFrame());
        CHECK(grid->tekPrimitives().empty());
    }

    fixture.stream->toRead += "\x1b]16;#161616\x1b\\";
    fixture.stream->toRead += tekEnterAndDraw;
    static_cast<void>(fixture.session->drain());
    releaseFrameHeldForQuiet(*fixture.session);
    REQUIRE(!fixture.session->isFrameHeld());
    static_cast<void>(TakenFrame(fixture.session->takeSyncFrame()));
    const FrameAsk asked = askForFrame(*fixture.session, {});
    REQUIRE(asked.supply.route == Runtime::FrameRoute::Captured);
    REQUIRE(asked.frame != nullptr);
    CHECK(asked.frame->isTekFrame());
    const Tek::PrimitiveSpan primitives = asked.frame->tekPrimitives();
    REQUIRE(primitives.size() == 1u);
    CHECK(primitives[0].kind == Tek::PrimitiveKind::Line);
    CHECK(primitives[0].x0 == 2000 && primitives[0].y0 == 1000);
    CHECK(primitives[0].x1 == 100 && primitives[0].y1 == 200);
    CHECK_EQ(asked.frame->tekScreenWidth(), Tek::tekGridWidth);
    const Tek::HostColorRequests requests = asked.frame->tekHostColorRequests();
    CHECK(requests.isBackgroundSet);
    const Tek::TekColor background = Tek::tekBackground(requests, Tek::TekColor { 0, 0, 0 }, Tek::ColorPreference {});
    CHECK_EQ(background.red, 0x1616);
}

// The frontend's ingest-idle commit is the owner's work: asked without the
// model lock, it retires the capture of the uncommitted tube, and the frame
// the view takes next shows the committed one. Before the commit, the tube
// displays the frame committed last, not the strokes drawn since.
TEST(aTekCommitIsAppliedByTheOwnerBeforeTheViewsNextFrame)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    TerminalSession& session = *fixture.session;
    session.setSyncFrameCaptureGeometry(testGeometry());
    fixture.stream->toRead += tekEnterAndDraw;
    static_cast<void>(session.drain());
    releaseFrameHeldForQuiet(session);
    REQUIRE(!session.isFrameHeld());
    static_cast<void>(TakenFrame(session.takeSyncFrame()));
    session.requestTekFrameCommit();
    {
        const FrameAsk committed = askForFrame(session, {});
        REQUIRE(committed.frame != nullptr);
        CHECK_EQ(committed.frame->tekPrimitives().size(), 1u);
    }

    fixture.stream->toRead += tekSecondVector;
    static_cast<void>(session.drain());
    releaseFrameHeldForQuiet(session);
    REQUIRE(!session.isFrameHeld());
    static_cast<void>(TakenFrame(session.takeSyncFrame()));
    {
        const FrameAsk before = askForFrame(session, {});
        REQUIRE(before.frame != nullptr);
        CHECK_MSG(before.frame->tekPrimitives().size() == 1u, "the tube displays the frame committed last");
    }

    session.lockForRead();
    auto request = std::async(std::launch::async, [&session] { session.requestTekFrameCommit(); });
    const bool didFinish = request.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    session.unlockForRead();
    request.get();
    CHECK_MSG(didFinish, "the request returned while the drain held the model lock");
    const FrameAsk after = askForFrame(session, {});
    REQUIRE(after.supply.route == Runtime::FrameRoute::Captured);
    REQUIRE(after.frame != nullptr);
    CHECK_EQ(after.frame->tekPrimitives().size(), 2u);
}

// A resizing view's demand is held while the redraw behind a whole-screen
// erase is still arriving: the serve finds it so, the view is told when to ask
// again, and the ask after the program's bytes went quiet captures the
// redraw.
TEST(aResizeIsHeldWhileAnErasesRedrawIsArriving)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("ready");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    fixture.stream->toRead += "\x1b[H\x1b[2Jfirst row";
    static_cast<void>(fixture.session->drain());
    static_cast<void>(TakenFrame(fixture.session->takeSyncFrame()));

    const FrameAsk arriving = askForFrame(*fixture.session, { .isResizing = true });
    CHECK(arriving.supply.route == Runtime::FrameRoute::Declined);
    CHECK(arriving.supply.decline == Runtime::FrameDecline::RedrawArriving);
    CHECK(arriving.supply.askAgainTicks >= 1);
    CHECK_MSG(!TakenFrame(fixture.session->takeSyncFrame()), "the serve kept the erased screen off the glass");

    PAL::s_testMicroseconds += Runtime::eraseHoldSettleMicros;
    const FrameAsk served = askForFrame(*fixture.session, { .isResizing = true });
    CHECK_MSG(served.supply.route == Runtime::FrameRoute::Captured, "the ask after the bytes went quiet shows the redraw");
}

// An open synchronized update holds a saturated drain's frame for the block,
// not for the rule, and holds the live read under its lock, resizing or not.
TEST(anOpenSynchronizedUpdateHoldsTheTickAndTheLiveRead)
{
    FloodedPane pane(6, 20);
    pane.fixture.stream->readMicroseconds = 10'000;
    pane.fixture.stream->toRead += "\x1b[?2026h\x1b[Hinside the block";
    static_cast<void>(pane.fixture.session->drain());
    REQUIRE(pane.terminal->modes.syncOutput);
    REQUIRE(pane.fixture.session->isDrainSaturated());
    REQUIRE(pane.fixture.session->readSnapshot().syncHoldActive);
    static_cast<void>(TakenFrame(pane.fixture.session->takeSyncFrame()));

    const FrameAsk held = askForFrame(*pane.fixture.session, {});
    CHECK(held.supply.route == Runtime::FrameRoute::Declined);
    CHECK(held.supply.decline == Runtime::FrameDecline::HeldByPresentation);

    const FrameAsk resizing = askForFrame(*pane.fixture.session, { .isResizing = true });
    REQUIRE(resizing.supply.route != Runtime::FrameRoute::Captured);
    CHECK(resizing.supply.decline == Runtime::FrameDecline::HeldByPresentation);
    pane.fixture.session->lockForRead();
    CHECK(pane.fixture.session->liveReadDecline(*pane.terminal, { .isResizing = true }).decline == Runtime::FrameDecline::BlockOpenUnderLock);
    CHECK(pane.fixture.session->liveReadDecline(*pane.terminal, {}).decline == Runtime::FrameDecline::BlockOpenUnderLock);
    pane.fixture.session->unlockForRead();
}

// A tick an open synchronized update holds says when the update's budget runs
// out, from the stamp its first check put on it, and so does the live read
// under its lock, resizing or not: the view asks again then, on no timer of
// its own.
TEST(aHeldTickSaysWhenTheUpdatesBudgetRunsOut)
{
    FloodedPane pane(6, 20);
    // A clock past its first tick, where a stamp is told from none.
    Ubiquitty::PAL::s_testMicroseconds += 10'000'000;
    pane.fixture.stream->readMicroseconds = 10'000;
    pane.fixture.stream->toRead += "\x1b[?2026h\x1b[Hinside the block";
    static_cast<void>(pane.fixture.session->drain());
    REQUIRE(pane.terminal->modes.syncOutput);
    REQUIRE(pane.fixture.session->isDrainSaturated());
    static_cast<void>(TakenFrame(pane.fixture.session->takeSyncFrame()));

    const FrameAsk first = askForFrame(*pane.fixture.session, { .nowTicks = PAL::monotonicTicks() });
    REQUIRE(first.supply.decline == Runtime::FrameDecline::HeldByPresentation);
    CHECK(first.supply.askAgainTicks >= 1);
    // The test's thread is the drain's, and no drain runs between its passes.
    const std::uint32_t stamp = pane.terminal->modes.syncOutputTouchTicks;
    REQUIRE(stamp != 0);
    constexpr std::uint32_t elapsed = 10;
    constexpr std::uint32_t budget = Modes::SyncOutput::maxHoldTicks - elapsed + 1;
    const Ticks later(stamp + elapsed);

    const FrameAsk held = askForFrame(*pane.fixture.session, { .nowTicks = later });
    CHECK(held.supply.decline == Runtime::FrameDecline::HeldByPresentation);
    CHECK_EQ(held.supply.askAgainTicks, budget);
    const FrameAsk heldResizing = askForFrame(*pane.fixture.session, { .nowTicks = later, .isResizing = true });
    CHECK(heldResizing.supply.decline == Runtime::FrameDecline::HeldByPresentation);
    CHECK_EQ(heldResizing.supply.askAgainTicks, budget);

    pane.fixture.session->lockForRead();
    const Runtime::LiveReadAnswer underLock = pane.fixture.session->liveReadDecline(*pane.terminal, { .nowTicks = later });
    const Runtime::LiveReadAnswer resizing = pane.fixture.session->liveReadDecline(*pane.terminal, { .nowTicks = later, .isResizing = true });
    pane.fixture.session->unlockForRead();
    CHECK(underLock.decline == Runtime::FrameDecline::BlockOpenUnderLock);
    CHECK_EQ(underLock.askAgainTicks, budget);
    CHECK(resizing.decline == Runtime::FrameDecline::BlockOpenUnderLock);
    CHECK_EQ(resizing.askAgainTicks, budget);
}

// A saturated drain's tick that finds the slot empty inside the publish window,
// while the drain holds the model on its own thread, awaits the next capture;
// past the window, a pane no ingest loop drains reads the model, as a view
// drawing search matches does.
TEST(aSaturatedTickInsideThePublishWindowAwaitsTheNextCapture)
{
    FloodedPane pane(6, 20);
    constexpr std::uint32_t publishWindowMicroseconds = 1'000'000 / 120;
    pane.floodPass("\x1b[Hfirst");
    REQUIRE(askForFrame(*pane.fixture.session, {}).supply.route == Runtime::FrameRoute::Captured);

    struct Asks {
        Runtime::FrameSupply awaiting;
        Runtime::FrameRoute searching = Runtime::FrameRoute::Declined;
        Runtime::FrameRoute late = Runtime::FrameRoute::Declined;
    };
    TerminalSession& session = *pane.fixture.session;
    session.lockForRead();
    auto asking = std::async(std::launch::async, [&session] {
        Asks asks;
        asks.awaiting = askForFrame(session, {}).supply;
        asks.searching = askForFrame(session, { .keepsSearchLive = true }).supply.route;
        PAL::s_testMicroseconds += publishWindowMicroseconds;
        asks.late = askForFrame(session, {}).supply.route;
        return asks;
    });
    const bool didFinish = asking.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    session.unlockForRead();
    const Asks asks = asking.get();
    REQUIRE(didFinish);
    CHECK(asks.awaiting.route == Runtime::FrameRoute::Declined);
    CHECK(asks.awaiting.decline == Runtime::FrameDecline::AwaitingCapture);
    CHECK_MSG(asks.awaiting.askAgainTicks == 0, "the capture's publication wakes the view, so it waits out no budget");
    CHECK(asks.searching == Runtime::FrameRoute::LiveRead);
    CHECK(asks.late == Runtime::FrameRoute::LiveRead);
}

// A view that asks for a frame over a screen nothing changes, as one whose
// selection or hover moved does, of a pane no ingest loop drains, is served at
// its ask: no frame the rule admits will come to carry it, and no loop takes a
// turn, so the asking thread captures the model as the rule last admitted it.
TEST(aPaneNoLoopDrainsIsServedAtTheAsk)
{
    SessionFixture fixture(24, 80);
    REQUIRE(fixture.isReady);
    fixture.session->setSyncFrameCaptureGeometry(testGeometry());
    fixture.drainSyncFrame("shown");
    REQUIRE(TakenFrame(fixture.session->takeSyncFrame()));
    REQUIRE(!fixture.session->isFrameHeld());
    CHECK_MSG(!fixture.session->serviceFrontend(), "a turn with nothing asked takes no work");
    CHECK(!TakenFrame(fixture.session->takeSyncFrame()));

    const FrameAsk served = askForFrame(*fixture.session, {});
    CHECK(served.supply.route == Runtime::FrameRoute::Captured);
    CHECK(served.frame != nullptr);
}

// A pane an embedder feeds on its own thread, with no transport and no ingest
// loop, is drawn: the view registers its geometry after the feed, and its
// first ask is served on the asking thread.
TEST(aFedPaneIsServedItsFrameAtTheAsk)
{
    TerminalSession* session = TerminalSession::make();
    REQUIRE(session != nullptr);
    session->setCoherenceClock(&SessionFixture::readCoherenceClock, nullptr);
    session->start(24, 80);
    constexpr std::string_view text = "fed text";
    session->feedBytes({ text.data(), text.size() });
    PAL::s_testMicroseconds += 1'000'000;
    session->setSyncFrameCaptureGeometry(testGeometry());

    const FrameAsk asked = askForFrame(*session, {});
    CHECK_MSG(asked.supply.route == Runtime::FrameRoute::Captured, "the fed pane's first frame is its view's capture");
    CHECK(asked.frame != nullptr);
    session->close();
    releaseTerminalSession(session);
}

// Every answer short of an open block's is reached while the drain holds the
// model lock: the take, the selection over it, the rule's hold and the
// saturation window are published state.
TEST(takingAFrameNeverWaitsForTheModelLockOutsideAnOpenBlock)
{
    FloodedPane pane(24, 80);
    pane.floodPass("before\r\nselected text\r\n");
    REQUIRE(TakenFrame(pane.fixture.session->takeSyncFrame()));
    pane.select(1, 0, 1, 8);
    pane.floodPass("after\r\n");

    TerminalSession& session = *pane.fixture.session;
    session.lockForRead();
    auto ticks = std::async(std::launch::async, [&session, &pane] {
        std::vector<Runtime::FrameRoute> routes;
        for (int tick = 0; tick < 3; ++tick)
            routes.push_back(askForFrame(session, { .selection = &pane.selection }).supply.route);
        routes.push_back(askForFrame(session, { .isResizing = true }).supply.route);
        return routes;
    });
    const bool didFinish = ticks.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    session.unlockForRead();
    const std::vector<Runtime::FrameRoute> routes = ticks.get();
    CHECK_MSG(didFinish, "every tick answered while the drain held the model lock");
    REQUIRE(routes.size() == 4u);
    CHECK(routes[0] == Runtime::FrameRoute::Captured);
    CHECK_MSG(routes[3] != Runtime::FrameRoute::Captured, "a resizing ask whose model another thread holds is answered without serving it");
}
