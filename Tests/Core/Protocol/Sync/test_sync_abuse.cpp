// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_abuse.cpp — abuse/stress tests for the Sync wire protocol
//
// These cases hammer the protocol from the angles a real user or a hostile
// peer can reach, as scripted sequences rather than libFuzzer's random corpus:
//   * Cmd+K / clearBuffer fast-paths (single-frame storms, scrollback vs
//     preserve-input, alternate-screen interactions, image eviction)
//   * Fast resizing (drag storms, 0/huge dims, interactive deferral, settle)
//   * Scrolling (newline storms, history turnover)
//   * Theme / palette (Facts storms, checkpoint palette, OSC 10/11/4)
//   * Transport framing (split header, interleaved types, over-long, retained bytes)
//   * Capability-gated frames (title, bell, clipboard, paste, file, workspace)
//
// Each case is a deterministic abuse script that would be invisible to a
// single-frame round-trip test: the bug class is stateful — a checkpoint that
// self-round-trips but desynchronizes after N rapid mutations, or a resize
// that is valid in isolation but leaves pendingHistoryRewrap set for the next
// checkpoint.

#include "Core/CellGrid.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;
namespace Sync = Ubiquitty::Protocol::Sync;

template<typename Message>
static void appendMessage(std::vector<std::uint8_t>& wire, Sync::FrameKind kind, std::uint32_t channel, const Message& message)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, message);
    CHECK(Sync::appendFrame(wire, kind, channel, body));
}

static std::string callText(std::string_view method, std::string id, Codec::JSON::Value parameters)
{
    using Codec::JSON::Value;
    auto call = Value::object();
    call.set("jsonrpc", Value::string("2.0"));
    call.set("id", Value::string(std::move(id)));
    call.set("method", Value::string(std::string(method)));
    call.set("params", std::move(parameters));
    return Codec::JSON::serialize(call);
}

static std::string captureCall(std::uint32_t index)
{
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("pane", Value::number(1));
    parameters.set("source", Value::string("rows"));
    parameters.set("start", Value::number(20 + index % 100));
    parameters.set("end", Value::number(130 + index % 40));
    return callText("pane.capture", "capture." + std::to_string(index), std::move(parameters));
}

static std::string clearCall(std::string_view command, std::uint32_t index)
{
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("pane", Value::number(1));
    parameters.set("command", Value::string(std::string(command)));
    return callText("pane.clear", "clear." + std::to_string(index), std::move(parameters));
}

static Sync::FrameHeaderDecision admitHeader(std::span<const std::uint8_t> bytes)
{
    Sync::FrameHeader header;
    Sync::EnvelopeError error = Sync::EnvelopeError::None;
    switch (Sync::readFrameHeader(bytes, header, error)) {
    case Sync::FrameHeaderState::NeedsMore:
        return {};
    case Sync::FrameHeaderState::Failed:
        return { .state = Sync::FrameHeaderDecision::State::Refused };
    case Sync::FrameHeaderState::Complete:
        return { .state = Sync::FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
    }
    return { .state = Sync::FrameHeaderDecision::State::Refused };
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::unique_ptr<Terminal> makeHost(int rows = 24, int columns = 80)
{
    auto term = std::make_unique<Terminal>();
    resize(*term, rows, columns);
    return term;
}

static std::unique_ptr<Terminal> makeClient(int rows = 24, int columns = 80)
{
    auto term = std::make_unique<Terminal>();
    resize(*term, rows, columns);
    return term;
}

// A Sync checkpoint body of `host`, the form a screen travels in.
static std::vector<uint8_t> checkpointPayload(Terminal& host, uint64_t generation = 1)
{
    std::vector<uint8_t> body = UbiquittyTest::checkpointBody(host, generation);
    CHECK(!body.empty());
    return body;
}

static bool applyCheckpointPayload(Terminal& client, const std::vector<uint8_t>& body)
{
    Protocol::Sync::ScreenReplica replica;
    return Protocol::Sync::applyCheckpoint(client, body, replica) == Protocol::Sync::ApplyOutcome::Applied;
}

// The history beside a checkpoint: a checkpoint names the history's address
// and carries no line (Docs/sync.md §6.6), so the lines travel on the history
// plane and the client wraps them into its scrollback at its own width.
static bool applyHistory(Terminal& host, Terminal& client)
{
    Protocol::Sync::HistoryPublisher publisher;
    (void)publisher.capture(host);
    const Screen::HistoryAddress address = publisher.address();
    Protocol::Sync::HistoryCache cache({}, {}, 1'000'000);
    if (address.head > address.oldest) {
        std::vector<uint8_t> body;
        if (publisher.writeBody(host, { address.oldest, address.head }, Protocol::Sync::HistoryOrder::Ascending, std::size_t { 64 } << 20, body) == 0)
            return false;
        const Protocol::Sync::HistoryBodyLimits limits { .maxLines = Protocol::Sync::historyBodyLimits.maxLines, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() };
        std::vector<Protocol::Sync::DecodedLine> lines;
        if (!Protocol::Sync::decodeHistoryBody(body, address.oldest, Protocol::Sync::HistoryOrder::Ascending, limits, lines)
            || cache.insert(cache.key().epoch, lines) != Protocol::Sync::CacheInsert::Stored)
            return false;
    }
    cache.setHead(cache.key().epoch, address.head);
    if (publisher.openLine()) {
        std::vector<uint8_t> body;
        std::vector<Protocol::Sync::DecodedLine> lines;
        if (!publisher.writeOpenLine(host, std::size_t { 64 } << 20, body)
            || !Protocol::Sync::decodeHistoryBody(body, address.head, Protocol::Sync::HistoryOrder::Ascending, { .maxLines = 1, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() }, lines)
            || cache.setOpenLine(cache.key().epoch, std::move(lines.front())) != Protocol::Sync::CacheInsert::Stored)
            return false;
    }
    cache.project(client, client.columns);
    return true;
}

static void checkSane(const Terminal& t)
{
    // Minimal renderer invariants; deeper checks live in fuzz_sync's checkTermSanity.
    CHECK(t.rows > 0 && t.rows <= maxRows);
    CHECK(t.columns > 0 && t.columns <= maxColumns);
    CHECK(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
    CHECK(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < t.columns);
    CHECK(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < t.rows);
}

// Compare what the renderer would read: grid + marks + lineAttributes + cursor + title + palette.
static bool renderEqual(Terminal& a, Terminal& b)
{
    if (a.rows != b.rows || a.columns != b.columns)
        return false;
    for (int r = 0; r < a.rows; ++r) {
        for (int c = 0; c < a.columns; ++c) {
            const Cell* ca = Screen::visibleCell(&a, 0, r, c);
            const Cell* cb = Screen::visibleCell(&b, 0, r, c);
            if (!ca || !cb)
                return false;
            if (ca->codePoint != cb->codePoint)
                return false;
            if (std::memcmp(&ca->pen, &cb->pen, sizeof(ca->pen)) != 0)
                return false;
        }
        uint8_t ma = a.screens.active ? a.screens.active->mark[r] : 0;
        uint8_t mb = b.screens.active ? b.screens.active->mark[r] : 0;
        if (ma != mb)
            return false;
        uint8_t la = a.screens.active ? a.screens.active->lineAttribute[r] : 0;
        uint8_t lb = b.screens.active ? b.screens.active->lineAttribute[r] : 0;
        if (la != lb)
            return false;
    }
    if (a.screens.active && b.screens.active) {
        if (a.screens.active->cursorX != b.screens.active->cursorX)
            return false;
        if (a.screens.active->cursorY != b.screens.active->cursorY)
            return false;
    }
    if (a.cursorStyle != b.cursorStyle)
        return false;
    if (a.modes.isCursorVisible != b.modes.isCursorVisible)
        return false;
    if (a.titleUTF8 != b.titleUTF8)
        return false;
    // Palette is not compared here; callers compare it explicitly when relevant.
    return true;
}

// ---------------------------------------------------------------------------
// 1. Cmd+K / clearBuffer abuse
// ---------------------------------------------------------------------------

// Cmd+K (clearBufferPreserveInput) is the heaviest single-frame mutation the
// user can trigger from the keyboard: it clears scrollback, blanks every row,
// restores one row's prefix into row 0, moves the cursor, drops images and
// marks, and marks every row dirty. A storm of them must never desynchronize
// the passive client and must stay checkpoint-applicable even when the cursor
// column is 0, at the right edge, or out of range.
TEST(testSyncAbuseClearPreserveRapid)
{
    auto host = makeHost();
    auto client = makeClient();
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host)));

    // Seed the screen so preserve-input has something to preserve.
    processBytes(host.get(), "hello world\r\nsecond line\r\nthird");
    host->screens.active->cursorY = 2;
    host->screens.active->cursorX = 5;
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 2)));
    CHECK(renderEqual(*host, *client));

    for (int i = 0; i < 1000; ++i) {
        // Vary cursor column within valid range 0..columns-1. Using columns itself
        // as a cursor (wrapPending edge) is allowed on the host, so we stay
        // inside the renderable range.
        host->screens.active->cursorX = (i * 37) % host->columns;
        host->screens.active->cursorY = i % host->rows;
        clearBufferPreserveInput(host.get());
        auto body = checkpointPayload(*host, 10 + i);
        CHECK(applyCheckpointPayload(*client, body));
        checkSane(*client);
        CHECK(renderEqual(*host, *client));
        // Cursor must land on row 0 after every Cmd+K, column = preserved length.
        CHECK(client->screens.active->cursorY == 0);
        CHECK(client->screens.active->cursorX >= 0 && client->screens.active->cursorX < client->columns);
        CHECK(Screen::scrollbackCount(host.get()) == 0);
    }
}

// clearBuffer (no preserve) vs preserve-input: the two must produce different
// wire states and different scrollback behaviour under a storm. Alternating
// them is the abuse shape that leaves a client's history out of step with the
// host's when one path clears it and the other does not.
TEST(testSyncAbuseClearVsPreserveAlternate)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host)));

    // Generate scrollback first.
    for (int i = 0; i < 200; ++i)
        processBytes(host.get(), "scroll line that will age into history\r\n");
    CHECK(Screen::scrollbackCount(host.get()) > 0);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 2)));
    CHECK(applyHistory(*host, *client));
    CHECK(Screen::scrollbackCount(client.get()) == Screen::scrollbackCount(host.get()));

    for (int i = 0; i < 200; ++i) {
        if (i % 2 == 0) {
            clearBuffer(host.get());
            CHECK(Screen::scrollbackCount(host.get()) == 0);
        } else {
            host->screens.active->cursorX = 4;
            host->screens.active->cursorY = host->rows - 1;
            clearBufferPreserveInput(host.get());
            CHECK(Screen::scrollbackCount(host.get()) == 0);
            CHECK(host->screens.active->cursorY == 0);
        }
        auto body = checkpointPayload(*host, 100 + i);
        CHECK(applyCheckpointPayload(*client, body));
        checkSane(*client);
    }
}

// Cmd+K on the alternate screen must not touch the primary's scrollback or
// grid. The checkpoint carries both screens; a storm that flips screens and
// clears must round-trip both.
TEST(testSyncAbuseClearOnAlternateScreen)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);

    // Fill primary with scrollback.
    for (int i = 0; i < 50; ++i)
        processBytes(host.get(), "primary\r\n");

    // Enter alternate via DECSET 1049.
    processBytes(host.get(), "\033[?1049h");
    CHECK(host->screens.active == &host->screens.alternate);
    processBytes(host.get(), "alt content");
    auto body1 = checkpointPayload(*host, 1);
    CHECK(applyCheckpointPayload(*client, body1));
    CHECK(client->screens.active == &client->screens.alternate);
    CHECK(renderEqual(*host, *client));

    for (int i = 0; i < 100; ++i) {
        host->screens.active->cursorX = i % host->columns;
        clearBufferPreserveInput(host.get());
        auto body = checkpointPayload(*host, 10 + i);
        CHECK(applyCheckpointPayload(*client, body));
        CHECK(client->screens.active == &client->screens.alternate);
        checkSane(*client);
    }

    // Exit alternate; scrollback was cleared by the alternate-screen Cmd+K
    // storm (clearBufferPreserveInput clears lineStore regardless of screen), so
    // primary history is now gone — the important invariant is that the screen
    // *contents* round-trip and the host didn't corrupt its grid.
    processBytes(host.get(), "\033[?1049l");
    CHECK(host->screens.active == &host->screens.primary);
    auto body2 = checkpointPayload(*host, 999);
    CHECK(applyCheckpointPayload(*client, body2));
    CHECK(client->screens.active == &client->screens.primary);
    CHECK(renderEqual(*host, *client));
}

// ---------------------------------------------------------------------------
// 2. Fast resizing abuse
// ---------------------------------------------------------------------------

// A window drag delivers dozens of RESIZEs per second. The host answers each
// with a checkpoint at the new rows/columns. The client must apply every one
// without OOB, and the host must not assert on pendingHistoryRewrapColumns.
TEST(testSyncFixedResizeDoesNotManufactureMirrorHistory)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    for (int row = 0; row < 20; ++row) {
        processBytes(host.get(), "incremental redraw paragraph with a right-edge fragment 1234\r\n");
    }
    CHECK(Screen::scrollbackCount(host.get()) == 0);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));
    CHECK(Screen::scrollbackCount(client.get()) == 0);
    resize(*host, 24, 45, Modes::ReflowMode::Fixed);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 2)));
    CHECK(renderEqual(*host, *client));
    CHECK(Screen::scrollbackCount(client.get()) == Screen::scrollbackCount(host.get()));
}

TEST(testSyncAbuseFastResizeStorm)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));

    // Interactive drag: defer history rewrap.
    for (int i = 0; i < 300; ++i) {
        int cols = 20 + (i * 7) % 100; // 20..119
        int rows = 10 + (i * 11) % 40; // 10..49
        resize(*host, rows, cols, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        // Host must settle before encoding a checkpoint of its history.
        settleResize(host.get());
        auto body = checkpointPayload(*host, 10 + i);
        CHECK(applyCheckpointPayload(*client, body));
        checkSane(*client);
        CHECK(host->rows == client->rows && host->columns == client->columns);
        CHECK(renderEqual(*host, *client));
    }
}

// Resize to 0, 1, huge, and back-to-back identical sizes. 0 is clamped to 1;
// huge is clamped to maxRows/maxColumns; identical must be a no-op but still
// produce a checkpoint the client accepts. The grid stays modest to keep the
// case fast.
TEST(testSyncAbuseResizeEdges)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host)));

    const int bigRows = std::min(maxRows, 300);
    const int bigCols = std::min(maxColumns, 300);
    struct Case {
        int rows;
        int cols;
    } cases[] = {
        { 0, 0 },
        { 1, 1 },
        { bigRows, bigCols },
        { bigRows + 100, bigCols + 100 },
        { 24, 80 },
        { 24, 80 }, // identical
        { -5, -5 }, // negative: clamped
        { 500, 10 },
        { 10, 500 },
    };
    uint64_t seq = 10;
    for (auto& c : cases) {
        resize(*host, c.rows, c.cols);
        settleResize(host.get());
        CHECK(host->rows >= 1 && host->rows <= maxRows);
        CHECK(host->columns >= 1 && host->columns <= maxColumns);
        auto body = checkpointPayload(*host, seq++);
        CHECK(applyCheckpointPayload(*client, body));
        checkSane(*client);
        CHECK(host->rows == client->rows && host->columns == client->columns);
    }
}

// The interactive deferral must be idempotent: settling twice, settling when
// nothing is pending, and interleaving resize+clear+resize must not leave the
// host in a state a checkpoint of its history could not be taken from.
TEST(testSyncAbuseResizeSettleIdempotence)
{
    auto host = makeHost(24, 80);
    // Generate scrollback so rewrap has work to do.
    for (int i = 0; i < 100; ++i)
        processBytes(host.get(), "wrap line with some content to reflow\r\n");

    for (int i = 0; i < 50; ++i) {
        resize(*host, 30 + i % 10, 60 + i % 20, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        // No settle yet: pendingHistoryRewrapColumns should be nonzero when scrollback exists.
        settleResize(host.get());
        settleResize(host.get()); // second is a no-op
        settleResize(host.get());
        auto body = checkpointPayload(*host, 100 + i);
        auto client = makeClient(host->rows, host->columns);
        CHECK(applyCheckpointPayload(*client, body));
        // Interleave a clear while pending would have been set.
        clearBuffer(host.get());
        resize(*host, 24, 80, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
        settleResize(host.get());
        auto body2 = checkpointPayload(*host, 1000 + i);
        auto client2 = makeClient(host->rows, host->columns);
        CHECK(applyCheckpointPayload(*client2, body2));
    }
}

// Capture requests keep their exact IDs and row selectors across a burst of
// direct canonical Calls, without a second fixed-layout request grammar.
TEST(testSyncAbuseClientReadRegionFrameStorm)
{
    std::vector<uint8_t> wire;
    for (std::uint32_t i = 0; i < 500; ++i)
        appendMessage(wire, Sync::FrameKind::Call, 0, Sync::Call { captureCall(i) });
    Sync::FrameDecoder dec;
    dec.append(wire);
    int seen = 0;
    while (const auto frame = dec.next()) {
        CHECK(frame->kind == Sync::FrameKind::Call);
        const auto call = Sync::decodeCall(frame->body);
        REQUIRE(call);
        const auto request = Sync::decodeCallRequest(call->text);
        CHECK(request.outcome == Protocol::Control::RequestOutcome::Dispatch);
        CHECK(request.method == "pane.capture");
        CHECK(request.id.isString() && request.id.asString() == "capture." + std::to_string(seen));
        CHECK(request.params.numberOr("start") == 20 + seen % 100);
        CHECK(request.params.numberOr("end") == 130 + seen % 40);
        ++seen;
    }
    CHECK(seen == 500);
    CHECK(dec.error() == Sync::EnvelopeError::None);
    CHECK(dec.bufferedBytes() == 0);
}

// ---------------------------------------------------------------------------
// 3. Scrolling abuse
// ---------------------------------------------------------------------------

// Newline storm: feed 10k lines, checkpoint every N lines, and verify the
// client stays sane and row marks / line attributes travel correctly.
TEST(testSyncAbuseScrollNewlineStorm)
{
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));

    for (int i = 0; i < 10000; ++i) {
        char line[64];
        std::snprintf(line, sizeof(line), "line %05d with some text to scroll\r\n", i);
        processBytes(host.get(), line);
        if (i % 50 == 0) {
            auto body = checkpointPayload(*host, 10 + i);
            CHECK(applyCheckpointPayload(*client, body));
            checkSane(*client);
            CHECK(host->rows == client->rows && host->columns == client->columns);
            CHECK(renderEqual(*host, *client));
        }
    }
    auto body = checkpointPayload(*host, 99999);
    CHECK(applyCheckpointPayload(*client, body));
    checkSane(*client);
}

// Scrollback turnover: at capacity, every new line evicts the oldest. The
// checkpoint carries the turned-over ring, and the client holds exactly what
// the host's ring holds rather than growing past it.
TEST(testSyncAbuseScrollbackTurnover)
{
    auto host = makeHost(24, 80);
    setScrollbackLimit(host.get(), 100); // small so turnover happens fast
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));

    // Fill past capacity: turnover in one tick.
    for (int i = 0; i < 500; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "overflow %03d\r\n", i);
        processBytes(host.get(), line);
    }
    CHECK(Screen::scrollbackCount(host.get()) <= 100);
    auto body = checkpointPayload(*host, 2);
    CHECK(applyCheckpointPayload(*client, body));
    CHECK(applyHistory(*host, *client));
    checkSane(*client);
    CHECK_EQ(Screen::scrollbackCount(client.get()), Screen::scrollbackCount(host.get()));
}

// ---------------------------------------------------------------------------
// 4. Theme / palette abuse
// ---------------------------------------------------------------------------

// Palette Facts storm: client pushes a new palette per frame (theme switch).
// Host decode must accept every one, and the checkpoint's palette must survive.
TEST(testSyncAbusePaletteFactsStorm)
{
    std::vector<uint8_t> wire;
    for (int i = 0; i < 500; ++i) {
        Protocol::Sync::ThemePalette theme;
        theme.live.indexed[1] = { static_cast<uint8_t>(i), static_cast<uint8_t>(i * 3), static_cast<uint8_t>(i * 7) };
        theme.live.defaultBackground = { static_cast<uint8_t>(i / 2), 0, 0 };
        theme.baseline = theme.live;
        Protocol::Sync::Facts facts;
        facts.themePalette = theme;
        std::vector<uint8_t> body;
        Protocol::Sync::encode(body, facts);
        CHECK(Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Facts, 0, body));
    }
    Protocol::Sync::FrameDecoder decoder;
    decoder.append(wire);
    int seen = 0;
    while (const auto frame = decoder.next()) {
        CHECK(frame->kind == Protocol::Sync::FrameKind::Facts);
        const auto facts = Protocol::Sync::decodeFacts(frame->body);
        REQUIRE(facts && facts->themePalette);
        CHECK(facts->themePalette->live.indexed[1].red == static_cast<uint8_t>(seen));
        ++seen;
    }
    CHECK(seen == 500);

    // Checkpoint palette round-trip: host sets palette, checkpoint carries it, client gets it.
    auto host = makeHost();
    auto client = makeClient();
    host->palette.indexed[5] = { 0x11, 0x22, 0x33 };
    host->palette.defaultForeground = { 0xAA, 0xBB, 0xCC };
    host->paletteBaseline = host->palette;
    auto body = checkpointPayload(*host, 1);
    CHECK(applyCheckpointPayload(*client, body));
    CHECK(client->palette.indexed[5].red == 0x11);
    CHECK(client->palette.defaultForeground.red == 0xAA);
    CHECK(client->paletteBaseline.indexed[5].red == 0x11);
}

// Palette via OSC 4/10/11 must also travel through the checkpoint. Abuse by
// spamming OSC colour changes and checkpointing.
TEST(testSyncAbuseThemeOSCStorm)
{
    auto host = makeHost();
    auto client = makeClient();
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));

    for (int i = 0; i < 200; ++i) {
        char seq[64];
        // OSC 4 ; index ; rgb:RR/GG/BB
        std::snprintf(seq, sizeof(seq), "\033]4;%d;rgb:%02x/%02x/%02x\007", i % 16, (i * 7) % 256, (i * 13) % 256, (i * 29) % 256);
        processBytes(host.get(), seq);
        // OSC 10/11 change default fg/bg
        std::snprintf(seq, sizeof(seq), "\033]10;rgb:%02x/%02x/%02x\007", (i * 3) % 256, (i * 5) % 256, (i * 11) % 256);
        processBytes(host.get(), seq);
        if (i % 10 == 0) {
            auto body = checkpointPayload(*host, 10 + i);
            CHECK(applyCheckpointPayload(*client, body));
            checkSane(*client);
            // Value equality covers every palette field without comparing padding.
            CHECK(host->palette == client->palette);
        }
    }
}

// Checkpoint baseline vs live palette: both halves travel in Facts and in
// the checkpoint. A bug that wrote palette twice and never wrote baseline would
// still pass a palette-only check.
TEST(testSyncAbusePaletteBaselineRoundTrips)
{
    auto host = makeHost();
    auto client = makeClient();
    host->palette.indexed[2] = { 1, 2, 3 };
    host->paletteBaseline.indexed[2] = { 4, 5, 6 };
    host->palette.defaultBackground = { 10, 20, 30 };
    host->paletteBaseline.defaultBackground = { 40, 50, 60 };
    auto body = checkpointPayload(*host, 1);
    CHECK(applyCheckpointPayload(*client, body));
    CHECK(client->palette.indexed[2].red == 1 && client->palette.indexed[2].green == 2);
    CHECK(client->paletteBaseline.indexed[2].red == 4 && client->paletteBaseline.indexed[2].green == 5);
    CHECK(client->palette.defaultBackground.red == 10);
    CHECK(client->paletteBaseline.defaultBackground.red == 40);

    // Both halves are independently carried by the attachment's Facts.
    Protocol::Sync::Facts facts;
    facts.themePalette = { host->palette, host->paletteBaseline };
    std::vector<uint8_t> wire;
    Protocol::Sync::encode(wire, facts);
    const auto decoded = Protocol::Sync::decodeFacts(wire);
    REQUIRE(decoded && decoded->themePalette);
    CHECK(decoded->themePalette->live.indexed[2].red == 1);
    CHECK(decoded->themePalette->baseline.indexed[2].red == 4);
}

// ---------------------------------------------------------------------------
// 5. Transport / framing abuse
// ---------------------------------------------------------------------------

// A single byte split per feed must reassemble regardless of where the split
// falls (mid-header, mid-payload, between frames). Abuse by feeding every
// possible split point.
TEST(testSyncAbuseFrameByteAtATimeAndInterleavedTypes)
{
    std::vector<uint8_t> wire;
    for (int i = 0; i < 20; ++i) {
        appendMessage(wire, Sync::FrameKind::Call, 0, Sync::Call { captureCall(i) });
        appendMessage(wire, Sync::FrameKind::Probe, 0, Sync::Probe { .time = static_cast<std::uint64_t>(i) });
        appendMessage(wire, Sync::FrameKind::Notify, 0, Sync::Notify { .kind = Sync::NotifyKind::PasteReady, .record = { 'h', 'i' } });
        Protocol::Sync::Facts facts;
        facts.themePalette.emplace();
        facts.themePalette->live.indexed[0] = { static_cast<uint8_t>(i), 0, 0 };
        facts.themePalette->baseline = facts.themePalette->live;
        std::vector<uint8_t> body;
        Protocol::Sync::encode(body, facts);
        CHECK(Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Facts, 0, body));
    }
    // Feed one byte at a time.
    Sync::FrameDecoder dec;
    int frames = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
        dec.append(std::span(wire).subspan(i, 1));
        while (dec.next())
            ++frames;
    }
    CHECK(frames == 80); // 20 * 4
    CHECK(dec.error() == Sync::EnvelopeError::None);
    CHECK(dec.bufferedBytes() == 0);

    // Same wire but fed in random-like chunks (3,7,1,13...).
    Sync::FrameDecoder dec2;
    size_t off = 0;
    int chunk = 3;
    frames = 0;
    while (off < wire.size()) {
        size_t take = std::min<size_t>(chunk, wire.size() - off);
        dec2.append(std::span(wire).subspan(off, take));
        off += take;
        while (dec2.next())
            ++frames;
        chunk = (chunk * 7 + 3) % 17 + 1;
    }
    CHECK(frames == 80);
    CHECK(dec2.error() == Sync::EnvelopeError::None);
}

// Over-long payload must fault the decoder and stay faulted, even if later
// bytes would form a valid frame. The fault is a security gate: a hostile
// length can't exhaust memory, and a decoder that "recovers" would resync on
// attacker-controlled bytes.
TEST(testSyncAbuseOverlongFaultLatch)
{
    std::vector<std::uint8_t> over;
    Sync::WireWriter writer(over);
    writer.u8(std::to_underlying(Sync::FrameKind::Notify));
    writer.varint(0);
    writer.varint(Sync::maxFrameBodyBytes + 1);
    std::vector<std::uint8_t> good;
    appendMessage(good, Sync::FrameKind::Call, 0, Sync::Call { captureCall(1) });
    over.insert(over.end(), good.begin(), good.end());

    Sync::FrameDecoder decoder;
    decoder.append(over);
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::Oversize);
    decoder.append(good);
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::Oversize);
}

// retainedBytes / releaseRetained: a big frame pins capacity even after it is
// consumed. A peer that streams huge frames must not pin unbounded memory
// on the receiver after the frame is done. releaseRetained must drop it, but
// must not drop a partial frame in flight.
TEST(testSyncAbuseRetainedBytesAccounting)
{
    // The canonical grammar replaces one 4 MiB frame with bounded frames.
    // Process over 4 MiB without retaining more than one exact body.
    const std::vector<std::uint8_t> body(Sync::maxFrameBodyBytes, 'x');
    std::vector<std::uint8_t> wire;
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, body));
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto receive = [&](auto, auto bytes) {
        CHECK(std::ranges::equal(bytes, body));
        CHECK_EQ(ingress.retainedBytes(), body.size());
        ++delivered;
        return Sync::FrameDelivery::Continue;
    };
    for (unsigned index = 0; index < 65; ++index) {
        CHECK(ingress.consume(wire, admitHeader, receive).state == Sync::FrameIngress::State::Accepting);
        CHECK_EQ(ingress.pendingBytes(), 0u);
        CHECK_EQ(ingress.retainedBytes(), body.size());
    }
    CHECK_EQ(delivered, 65u);
    ingress.releaseRetained();
    CHECK_EQ(ingress.retainedBytes(), 0u);

    const auto prefix = wire.size() - body.size() + 4096;
    CHECK(ingress.consume(std::span(wire).first(prefix), admitHeader, receive).state == Sync::FrameIngress::State::Accepting);
    const auto pending = ingress.pendingBytes();
    ingress.releaseRetained();
    CHECK_EQ(ingress.pendingBytes(), pending);
    CHECK_EQ(ingress.retainedBytes(), body.size());
    CHECK_EQ(delivered, 65u);
    CHECK(ingress.consume(std::span(wire).subspan(prefix), admitHeader, receive).state == Sync::FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 66u);
}

// Unknown canonical kinds close the reader. Neither the unknown body's bytes
// nor valid frames after it may be treated as a resynchronization point.
TEST(testSyncAbuseUnknownTypeInterleaved)
{
    std::vector<std::uint8_t> wire;
    appendMessage(wire, Sync::FrameKind::Probe, 0, Sync::Probe { .time = 1 });
    wire.push_back(0xFE);
    wire.insert(wire.end(), { 0, 2, 'A', 'B' });
    appendMessage(wire, Sync::FrameKind::Probe, 0, Sync::Probe { .time = 2 });
    appendMessage(wire, Sync::FrameKind::Call, 0, Sync::Call { captureCall(1) });

    Sync::FrameDecoder decoder;
    decoder.append(wire);
    const auto first = decoder.next();
    REQUIRE(first && first->kind == Sync::FrameKind::Probe);
    const auto probe = Sync::decodeProbe(first->body);
    CHECK(probe && probe->time == 1);
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::UnknownKind);
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::UnknownKind);
}

// ---------------------------------------------------------------------------
// 6. Mixed clear / opaque text / empty-frame storms
// ---------------------------------------------------------------------------

// Canonical clear Calls, pane records and Bell effect packets stay ordered and
// decode exactly once across three hundred interleaved rounds.
TEST(testSyncAbuseClearTitleBellStorm)
{
    constexpr std::array<std::string_view, 3> commands { "buffer", "scrollback", "screen" };
    const auto bell = Sync::encodeEffectBody(Sync::BellEffect {});
    REQUIRE(bell);
    std::vector<std::uint8_t> wire;
    for (std::uint32_t index = 0; index < 300; ++index) {
        appendMessage(wire, Sync::FrameKind::Call, 0, Sync::Call { clearCall(commands[index % commands.size()], index) });
        Sync::PaneFacts facts;
        facts.revision = index + 1;
        facts.programTitle = "title " + std::to_string(index);
        std::vector<std::uint8_t> body;
        Sync::encodePaneFacts(body, facts);
        REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::PaneFacts, 1, body));
        const auto record = Sync::encodeEffect({ .paneID = { 1, 17 }, .generation = 1, .sequence = index + 1, .primary = { 3, 4 }, .kind = Sync::EffectKind::Bell, .payload = bell->view() });
        REQUIRE(record);
        const auto packet = Sync::encodeEffectPacket({ 1, 17 }, 1, index + 1, 0, record->view());
        REQUIRE(packet);
        REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Effect, 1, packet->view()));
    }
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    unsigned clears = 0, titles = 0, bells = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind == Sync::FrameKind::Call) {
            const auto call = Sync::decodeCall(frame->body);
            REQUIRE(call);
            const auto request = Sync::decodeCallRequest(call->text);
            CHECK(request.outcome == Protocol::Control::RequestOutcome::Dispatch);
            CHECK(request.method == "pane.clear");
            CHECK(request.params.stringOr("command") == commands[clears % commands.size()]);
            ++clears;
        } else if (frame->kind == Sync::FrameKind::PaneFacts) {
            const auto facts = Sync::decodePaneFacts(frame->body);
            REQUIRE(facts);
            CHECK(facts->programTitle == "title " + std::to_string(titles));
            CHECK_EQ(facts->revision, titles + 1);
            ++titles;
        } else if (frame->kind == Sync::FrameKind::Effect) {
            const auto packet = Sync::decodeEffectPacket({ 1, 17 }, frame->body);
            REQUIRE(packet && packet->record);
            CHECK(packet->record->kind == Sync::EffectKind::Bell);
            CHECK_EQ(packet->record->sequence, bells + 1);
            const auto effect = Sync::decodeEffectBody(packet->record->kind, packet->record->payload);
            REQUIRE(effect);
            CHECK(std::holds_alternative<Sync::BellEffect>(*effect));
            ++bells;
        } else
            CHECK(false);
    }
    CHECK(clears == 300 && titles == 300 && bells == 300);
    CHECK(decoder.error() == Sync::EnvelopeError::None);
}

// ---------------------------------------------------------------------------
// 7. Delta convergence under abuse
// ---------------------------------------------------------------------------

// Refusal appends no partial header or body and preserves previously queued
// frames, so no caller can mistake a short result for an admitted frame.
TEST(testSyncAbuseAnOversizedBodyComesBackEmpty)
{
    const std::vector<std::uint8_t> body(Sync::maxFrameBodyBytes + 1);
    std::vector<std::uint8_t> wire;
    CHECK(!Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, body));
    CHECK_MSG(wire.empty(), "an oversized canonical frame appends neither a header nor a partial body");
    appendMessage(wire, Sync::FrameKind::Probe, 0, Sync::Probe { .time = 1 });
    const auto before = wire;
    CHECK(!Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, body));
    CHECK(wire == before);
}

// The style byte is a DECSCUSR code, and DECSCUSR stores only 0...6: Ps 7 is
// the configured shape, as 0 is, and a Ps past 7 changes nothing. A peer byte
// outside 0...6 is one no parser produces, so the
// checkpoint carrying it is refused and the client keeps the style it had; a
// DECRQSS reply never echoes a Ps no terminal can send.
TEST(testSyncAbuseCursorStyleFromThePeerIsADECSCUSRCode)
{
    {
        auto host = makeHost(4, 10);
        host->cursorStyle = static_cast<Cursor::Style>(200);
        const std::vector<uint8_t> body = checkpointPayload(*host);
        auto client = makeClient(4, 10);
        const Cursor::Style before = client->cursorStyle;
        CHECK_MSG(!applyCheckpointPayload(*client, body), "a checkpoint carrying a style no parser writes is refused");
        CHECK_MSG(client->cursorStyle == before, "a refused checkpoint leaves the client's style as it was");
    }
    {
        auto client = makeClient(4, 10);
        processBytes(client.get(), "\x1b[2 q");
        const Cursor::Style steadyBlock = client->cursorStyle;
        CHECK_MSG(steadyBlock == Cursor::Style::SteadyBlock, "control: DECSCUSR 2 stores its own code");
        processBytes(client.get(), "\x1b[200 q");
        CHECK_MSG(client->cursorStyle == steadyBlock, "control: DECSCUSR 200 writes no style at all");
    }
}

// ---------------------------------------------------------------------------
// 9. Clipboard abuse
// ---------------------------------------------------------------------------

TEST(testSyncAbuseRetiredClipboardHeadersAreRefused)
{
    // Neither an empty old frame nor a large declared body may reach dispatch.
    for (const std::uint8_t raw : { 0x13, 0x19, 0x9D, 0xA2 }) {
        for (const std::uint32_t length : { 0u, static_cast<std::uint32_t>(maxPayloadLength) }) {
            const std::array<std::uint8_t, 5> header { raw, static_cast<std::uint8_t>(length),
                static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24) };
            for (std::size_t split = 0; split <= header.size(); ++split) {
                SyncClientCore client(nullptr, nullptr);
                client.setClock([] { return 1; });
                client.setClientInstance({ 3, 4 });
                std::vector<std::uint8_t> greeting;
                client.buildHello(HandshakeRequest {}, greeting);
                REQUIRE(!greeting.empty());
                const bool first = client.feed(std::as_bytes(std::span { header }.first(split)));
                if (first)
                    CHECK(!client.feed(std::as_bytes(std::span { header }.subspan(split))));
                CHECK(client.faulted());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 10. HELLO abuse
// ---------------------------------------------------------------------------

TEST(testSyncAbuseHelloTruncation)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.clientInstance = { 3, 4 };
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    greeting.request.agentLabel = "abuse";
    greeting.credential = std::vector<std::uint8_t> { 't', 'o', 'k' };
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(!body.empty());
    for (std::size_t cut = 0; cut < body.size(); ++cut)
        CHECK(!Sync::decodeGreet(std::span(body).first(cut)));
    const auto complete = Sync::decodeGreet(body);
    REQUIRE(complete);
    CHECK(complete->request.agentLabel == "abuse");
    CHECK(complete->credential == greeting.credential);

    // Find the target byte by changing just one valid required field, not a
    // hard-coded offset through the build and identity prefixes.
    greeting.request.target = Sync::AdmissionTarget::Workspace;
    std::vector<std::uint8_t> workspace;
    Sync::encode(workspace, greeting);
    REQUIRE(workspace.size() == body.size());
    unsigned changed = 0;
    for (std::size_t offset = 0; offset < body.size(); ++offset) {
        if (body[offset] != workspace[offset]) {
            body[offset] = 0xFF;
            ++changed;
        }
    }
    REQUIRE(changed == 1);
    CHECK(!Sync::decodeGreet(body));
}

// The registered pane.clear Call names every command exactly; its parameter
// schema rejects missing, unknown and extra fields before any terminal edit.
TEST(testSyncAbuseClearBufferFrame)
{
    namespace Control = Ubiquitty::Protocol::Control;
    const auto* method = Control::findSyncMethod("pane.clear");
    REQUIRE(method);
    bool isJSON = false;
    const auto schema = Codec::JSON::parse(std::string(method->parametersSchema), isJSON);
    REQUIRE(isJSON);
    for (const auto command : { "buffer", "scrollback", "screen", "previous-mark" }) {
        std::vector<uint8_t> wire;
        appendMessage(wire, Sync::FrameKind::Call, 0, Sync::Call { clearCall(command, 1) });
        Sync::FrameDecoder decoder;
        decoder.append(wire);
        const auto frame = decoder.next();
        REQUIRE(frame && frame->kind == Sync::FrameKind::Call);
        const auto call = Sync::decodeCall(frame->body);
        REQUIRE(call);
        const auto request = Sync::decodeCallRequest(call->text);
        CHECK(request.outcome == Control::RequestOutcome::Dispatch);
        CHECK(request.method == "pane.clear" && request.params.stringOr("command") == command);
        CHECK(!Control::validate(request.params, schema));
    }
    for (const char* invalid : { R"({"pane":1})", R"({"pane":1,"command":"unknown"})", R"({"pane":1,"command":4})", R"({"pane":1,"command":"buffer","extra":true})" }) {
        const auto parameters = Codec::JSON::parse(invalid, isJSON);
        REQUIRE(isJSON);
        CHECK(Control::validate(parameters, schema));
    }

    // Host-side effect: clearBufferPreserveInput preserves the prompt at row 0.
    auto host = makeHost(24, 80);
    auto client = makeClient(24, 80);
    processBytes(host.get(), "prompt$ input\r");
    // Move cursor to after "prompt$ input" (simulate shell at prompt)
    host->screens.active->cursorY = 0;
    host->screens.active->cursorX = 14;
    // Checkpoint to the client so both have the same prompt at row 0.
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));
    CHECK(renderEqual(*host, *client));
    // Host receives ClearBuffer (like Daemon would) — do preserve directly.
    clearBufferPreserveInput(host.get());
    CHECK(host->screens.active->cursorY == 0 && host->screens.active->cursorX == 14);
    const Cell* c = Screen::visibleCell(host.get(), 0, 0, 0);
    CHECK(c && c->codePoint == 'p');
    // The checkpoint must carry the preserved row to the client without blanking.
    auto body = checkpointPayload(*host, 2);
    CHECK(applyCheckpointPayload(*client, body));
    CHECK(renderEqual(*host, *client));
    const Cell* cc = Screen::visibleCell(client.get(), 0, 0, 0);
    CHECK(cc && cc->codePoint == 'p');
    CHECK(client->screens.active->cursorX == 14);
}

// A narrowing rewraps the ring into more rows without appending any line, and
// the client wraps the same logical lines at the new width: it holds the new
// width's rows, never the old width's history under the new width.
TEST(testSyncAbuseScrollbackReflowNarrow)
{
    auto host = makeHost(24, 80);
    setScrollbackLimit(host.get(), 200);
    // Long lines that will wrap differently at 40 vs 80: 100 chars.
    std::string longLine(100, 'a');
    longLine += "\r\n";
    for (int i = 0; i < 20; ++i)
        processBytes(host.get(), longLine);
    const int count80 = Screen::scrollbackCount(host.get());
    CHECK(count80 > 0);
    auto client = makeClient(24, 80);
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 1)));
    CHECK(applyHistory(*host, *client));
    CHECK(Screen::scrollbackCount(client.get()) == Screen::scrollbackCount(host.get()));
    // Narrow to 40 — should reflow to more lines.
    resize(*host, 24, 40);
    settleResize(host.get());
    const int count40 = Screen::scrollbackCount(host.get());
    CHECK(count40 > count80); // more wrapped lines at narrower width
    CHECK(applyCheckpointPayload(*client, checkpointPayload(*host, 2)));
    CHECK(applyHistory(*host, *client));
    CHECK(Screen::scrollbackCount(client.get()) == Screen::scrollbackCount(host.get()));
}

UBIQUITTY_TEST_MAIN()
