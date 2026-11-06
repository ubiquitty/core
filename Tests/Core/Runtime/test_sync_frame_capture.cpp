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
#include "StrictAtlas.h"
#include "TerminalFixture.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Runtime/TerminalSession.h"
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

Ticks monotonicTicks() { return Ticks(0); }
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
        CHECK(!fixture.session->isDrainSaturated());
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
        const auto& actual = *fixture.session->terminal();
        const auto& expected = *control.session->terminal();
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
        CHECK(!fixture.session->isDrainSaturated());
        CHECK(!fixture.session->isFrameHeld());
        CHECK(!VT::isParserAtGround(fixture.session->terminal()));
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
    const auto& actual = *fixture.session->terminal();
    const auto& expected = *control.session->terminal();
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
    fx.session->terminal()->cursorMotionModePref = CursorMotionMode::Long;
    fx.session->terminal()->cursorMotionDurationMilliseconds = 180;
    fx.drainSyncFrame("motion");
    const TakenFrame frame { fx.session->takeSyncFrame() };
    CHECK(frame != nullptr);
    fx.session->terminal()->cursorMotionModePref = CursorMotionMode::Off;
    fx.session->terminal()->cursorMotionDurationMilliseconds = 0;
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
    fx.session->terminal()->modes.shouldSaveCRT = false;
    CHECK(!fx.session->publishHeldFrameIfDue().has_value());
    CHECK(fx.session->takeSyncFrameReady()); // announced: the render still wakes
    CHECK(fx.session->takeSyncFrame() == nullptr); // but nothing was captured
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
    Terminal* terminal = fx.session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->palette.indexedAlpha[7] = 0x80;

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
        fx.session->lockForRead();
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
        fx.session->unlockForRead();
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
        fx.session->lockForRead();
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
        fx.session->unlockForRead();
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
