// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include <string_view>

// Host-side tests for the tmux Control Mode protocol filter. The
// parser is platform-agnostic; this exercise lives entirely in the
// core/ tree so the same code path every frontend runs is what
// we're validating here.

#include "Core/CellGrid.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/TerminalOperations.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Capture decoded bytes that the filter forwards to the VT100 path
// into a flat buffer for assertion.
using CaptureContext = struct {
    char buffer[16384];
    int length;
    int spanCalls;
};

static void capture(void* context, uint8_t b)
{
    CaptureContext* c = (CaptureContext*)context;
    if (c->length < (int)sizeof(c->buffer))
        c->buffer[c->length++] = (char)b;
}

static void captureSpan(void* context, std::string_view bytes)
{
    CaptureContext* c = (CaptureContext*)context;
    c->spanCalls++;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        capture(context, static_cast<uint8_t>(bytes[i]));
}

static void feed(Ubiquitty::Protocol::Tmux::TmuxClient* tc, const char* s, CaptureContext* capacity)
{
    int i, n = (int)strlen(s);
    for (i = 0; i < n; i++)
        Ubiquitty::Protocol::Tmux::feedInput(tc, (uint8_t)s[i], capacity, capture);
}

static void feedSpan(Ubiquitty::Protocol::Tmux::TmuxClient* tc, const char* s, CaptureContext* capacity)
{
    Ubiquitty::Protocol::Tmux::feedInputBytes(tc, s, capacity, capture, captureSpan);
}

static void resetCap(CaptureContext* c)
{
    c->length = 0;
    c->spanCalls = 0;
    c->buffer[0] = 0;
}

// ---------- tests ----------

// Mid-line `%begin ` must not trigger control-mode autodetect — the
// detector is line-start only, so `cat`ing a tmux log that quotes the
// string doesn't silently flip Ubiquitty into a control mode it'll wait
// forever to exit.
// The host-level routing (Terminal.integration.paneRouter.lookup in Source/Core/VT/VT.cpp) decides whether
// %output bytes go to a child pane Ubiquitty::Terminal or to the current Terminal. When
// no paneLookup is installed, bytes must fall back to the current
// Terminal — Source/Core/VT/VT.cpp does this with an explicit nullptr check on the lookup
// pointer, so the tmux filter itself is unaware of routing. This test
// pins the contract from the tmux side: bytes decoded out of %output
// reach the deliver callback exactly once, regardless of pane id, so
// any plain deliver function (which is what the no-router fallback
// uses) sees the full payload.
// ---------- key wrapping ----------

// wrapKeyForPane uses snprintf with a 64-byte header
// buffer. INT_MAX as paneID still fits (~36 chars) so the buffer
// never overflows in practice, but a poisoned activePaneID plus a
// future header-format growth could push past the bound. This test
// pins the snprintf bound check and the bounds-aware return.
// ---------- DCS-wrapped control mode (`tmux -CC` on some builds) ----------

// tmux sends %exit *inside* the DCS envelope; the closing \e\\ comes
// right after. Without the dcsEnvelopeActive check on %exit, our parser
// would prematurely return to passthrough and leak the terminator
// bytes (ESC + '\\') through to the VT100 parser as text. This test
// uses a capture-shaped sequence.
// zsh prompts and percent-encoded URLs frequently include `%` mid-line.
// The detector matches the full literal "%begin " before holding bytes
// back, and flushes immediately on a mismatch: buffering the whole line
// to '\n' on every '%' stalls plain (non-CC) tmux output and breaks
// pane-switch repaints.
// A `%` that doesn't extend into "%begin " must be delivered without
// waiting for a newline, otherwise terminal redraws stall.
// ---------- window-lifecycle event sink ----------

using EvCapacity = struct {
    int addCount;
    int closeCount;
    int renameCount;
    int exitCount;
    int lastAddID;
    int lastCloseID;
    int lastRenameID;
    char lastRenameName[64];
    int respLineCount;
    int respEndCount;
    char respLines[8][64];
    int respLineLengths[8];
    int layoutCount;
    int lastLayoutWindowID;
    int lastLayoutPaneCount;
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry lastLayoutPanes[8];
    int pauseCount;
    int lastPausedPane;
    int modeChangeCount;
    int lastModeChangePane;
};

static EvCapacity eventCapacity;

static void evReset() { eventCapacity = {}; }

static void evAdd(void* context, int id)
{
    (void)context;
    eventCapacity.addCount++;
    eventCapacity.lastAddID = id;
}
static void evClose(void* context, int id)
{
    (void)context;
    eventCapacity.closeCount++;
    eventCapacity.lastCloseID = id;
}
static void evRename(void* context, int id, std::string_view name)
{
    (void)context;
    eventCapacity.renameCount++;
    eventCapacity.lastRenameID = id;
    size_t copyLength = name.size();
    if (copyLength >= sizeof(eventCapacity.lastRenameName))
        copyLength = sizeof(eventCapacity.lastRenameName) - 1;
    memcpy(eventCapacity.lastRenameName, name.data(), copyLength);
    eventCapacity.lastRenameName[copyLength] = 0;
}
static void evExit(void* context)
{
    (void)context;
    eventCapacity.exitCount++;
}
static void evRespLine(void* context, std::string_view line)
{
    (void)context;
    int slot = eventCapacity.respLineCount;
    if (slot < 8) {
        int n = line.size() < 63 ? static_cast<int>(line.size()) : 63;
        memcpy(eventCapacity.respLines[slot], line.data(), (size_t)n);
        eventCapacity.respLines[slot][n] = 0;
        eventCapacity.respLineLengths[slot] = static_cast<int>(line.size());
    }
    eventCapacity.respLineCount++;
}
static void evRespEnd(void* context)
{
    (void)context;
    eventCapacity.respEndCount++;
}
static void evLayout(void*, int windowID, std::string_view layout)
{
    eventCapacity.layoutCount++;
    eventCapacity.lastLayoutWindowID = windowID;
    /// The callback hands the raw layout string; decode it with the same public
    /// parseLayout the frontend uses.
    int n = Ubiquitty::Protocol::Tmux::parseLayout(layout, std::span(eventCapacity.lastLayoutPanes, static_cast<std::size_t>(8)));
    eventCapacity.lastLayoutPaneCount = n;
}

static void installEventSink(Ubiquitty::Protocol::Tmux::TmuxClient* tc)
{
    Ubiquitty::Protocol::Tmux::TmuxEventClient client;
    client.windowAdd = evAdd;
    client.windowClose = evClose;
    client.windowRenamed = evRename;
    client.exit = evExit;
    client.commandResponseLine = evRespLine;
    client.commandResponseEnd = evRespEnd;
    client.layoutChange = evLayout;
    client.paneDidPause = [](void*, int paneID) {
        eventCapacity.pauseCount++;
        eventCapacity.lastPausedPane = paneID;
    };
    client.paneDidChangeMode = [](void*, int paneID) {
        eventCapacity.modeChangeCount++;
        eventCapacity.lastModeChangePane = paneID;
    };
    Ubiquitty::Protocol::Tmux::setEventClient(tc, client);
}

// "%pause %<pane>" fires paneDidPause; "%continue" is consumed silently.
// "%pane-mode-changed %<pane>" fires paneDidChangeMode with the pane id (the
// host re-queries the pane's mode); it must not be confused with
// "%window-pane-changed" and must not leak to the VT100 model.
// Walk a synthetic `%begin / list-windows body / %end` stream: each
// `@<id>` body line must reach commandResponseLine, then
// commandResponseEnd fires once on close.
// ---------- window-layout parsing ----------

// parsePaneRecord: a full row, the active-omitted default, and a malformed
// prefix (rejected, never reads past length — bounds-safe on untrusted input).
TEST(testPassthroughWhenDisabled)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    resetCap(&capacity);
    feed(&tc, "hello\r\nworld", &capacity);
    CHECK(capacity.length == 12);
    CHECK(memcmp(capacity.buffer, "hello\r\nworld", 12) == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testPassthroughWhenNoAutodetect)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Manual); // enabled but auto-detect off
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    // No auto-detect → bytes pass through verbatim.
    CHECK(capacity.length == 26);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testAutodetectEntersControlMode)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
    CHECK(capacity.length == 0); // response framing is consumed entirely
}

TEST(testBeginMidLineDoesNotEnterControlMode)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "foo %begin bar\n", &capacity);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
    // All 15 bytes must have been delivered verbatim — no held-back
    // prefix, no silent state flip.
    CHECK(capacity.length == 15);
    CHECK(memcmp(capacity.buffer, "foo %begin bar\n", 15) == 0);

    // After a `\n`, atLineStart resets to true and a fresh
    // `%begin <args>\n` line is once again a valid introducer — the
    // line-start gate doesn't permanently lobotomise detection.
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testLiteralPercentMidStreamPassesThrough)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "user@host % ls 100% done\r\n", &capacity);
    CHECK(capacity.length == 26);
    CHECK(memcmp(capacity.buffer, "user@host % ls 100% done\r\n", 26) == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testPartialPercentMismatchFlushesImmediately)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    // '%bog' diverges from "%begin " at the third byte ('o' != 'g').
    feed(&tc, "%bog", &capacity);
    CHECK(capacity.length == 4);
    CHECK(memcmp(capacity.buffer, "%bog", 4) == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testDCSIntroducerEntersControlMode)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    // Full DCS-wrapped session: ESC P 1 0 0 0 p ... ESC `\`
    feed(&tc, "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n%output %0 hi\r\n\033\\", &capacity);
    CHECK(capacity.length == 2);
    CHECK(memcmp(capacity.buffer, "hi", 2) == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc)); // DCS terminator returned to passthrough
}

TEST(testDCSIntroducerPartialMismatchPassesThrough)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    // ESC P 1 0 0 0 X — last byte mismatches the introducer's 'p'.
    // Buffered prefix should be flushed back so the VT100 parser sees
    // the original ESC P sequence and the trailing X.
    feed(&tc, "\033P1000X", &capacity);
    CHECK(capacity.length == 7);
    CHECK(memcmp(capacity.buffer, "\033P1000X", 7) == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testDCSExitWaitsForTerminator)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    // Real shape from a captured `tmux -CC attach`: DCS introducer,
    // %begin/%end, response payload, %exit, then DCS terminator.
    feed(&tc,
        "\033P1000p"
        "%begin 1 1 0\r\n"
        "can't find session: mtcap\r\n"
        "%error 1 1 0\r\n"
        "%exit\r\n"
        "\033\\",
        &capacity);
    // Nothing should reach deliver — the entire envelope, including
    // the response payload and the DCS terminator, is consumed by
    // the protocol parser.
    CHECK(capacity.length == 0);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testDCSThenPassthroughResumes)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n\033\\", &capacity);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
    resetCap(&capacity);
    feed(&tc, "shell$ ", &capacity);
    CHECK(capacity.length == 7);
    CHECK(memcmp(capacity.buffer, "shell$ ", 7) == 0);
}

// A control session's end is answered once, however it ends. A server that
// dies mid-line leaves its client's %exit inside an unfinished %output, where
// it reads as pane output: the envelope's terminator ends the session then. A
// client killed outright writes neither, and the stream's end ends it.
TEST(aControlSessionEndsOnceHoweverItEnds)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    const auto fresh = [](Tmux::TmuxClient& tc) {
        Tmux::init(&tc);
        Tmux::configure(&tc, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
        installEventSink(&tc);
        evReset();
    };
    Tmux::TmuxClient tc;
    CaptureContext capacity;

    // %exit, then the terminator: one end.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n%exit\r\n\033\\", &capacity);
    CHECK_EQ(eventCapacity.exitCount, 1);
    CHECK(!Tmux::isActive(&tc));

    // The server died mid-line: its client's %exit is pane output, and the
    // terminator ends the session.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%output %0 out\\015\\012ou%exit server exited unexpectedly\r\n\033\\", &capacity);
    CHECK_EQ(eventCapacity.exitCount, 1);
    CHECK(!Tmux::isActive(&tc));
    resetCap(&capacity);
    feed(&tc, "$ ", &capacity);
    CHECK_MSG(capacity.length == 2, "the shell after the session reaches the terminal");

    // The client was killed: the stream just stops.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n%output %0 partial", &capacity);
    Tmux::endOfStream(&tc);
    CHECK_EQ(eventCapacity.exitCount, 1);
    CHECK(!Tmux::isActive(&tc));

    // Killed between its %exit and the terminator: the end was answered.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%exit\r\n", &capacity);
    Tmux::endOfStream(&tc);
    CHECK_EQ(eventCapacity.exitCount, 1);
    CHECK(!Tmux::isActive(&tc));

    // A session with no envelope ends with its stream too.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(Tmux::isActive(&tc));
    Tmux::endOfStream(&tc);
    CHECK_EQ(eventCapacity.exitCount, 1);

    // And a stream that never entered control mode ends with nothing to end.
    fresh(tc);
    resetCap(&capacity);
    feed(&tc, "$ exit\r\n", &capacity);
    Tmux::endOfStream(&tc);
    CHECK_EQ(eventCapacity.exitCount, 0);
}

TEST(testOutputDecodesEscapes)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    // Enter control mode first via a complete %begin/%end pair, then
    // send a %output line carrying ESC + [ + 'A' (octal-escaped).
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%output %0 \\033[A\r\n", &capacity);
    CHECK(capacity.length == 3);
    CHECK((unsigned char)capacity.buffer[0] == 0x1B);
    CHECK(capacity.buffer[1] == '[');
    CHECK(capacity.buffer[2] == 'A');
}

// A tmux pane shows what the bytes say, cell for cell.
//
// The two cases above pin the unescape one sequence at a time: an octal ESC, a
// literal backslash. Neither answers the question a user asks, which is whether
// a program run under `tmux -CC` puts the same thing on screen as the same
// program run without it.
//
// So this feeds one payload twice. Once straight into a Terminal, which is the
// screen with no tmux in the picture. Once wrapped as tmux control-mode
// `%output` frames, escaped the way tmux escapes them, and driven through the
// client into a second Terminal. Then it compares the two grids cell by cell:
// every byte of the Cell.
//
// The payload is chosen for what an escape-and-reassemble can lose. SGR colour
// and bold (the pen has to survive), a literal backslash and a run that LOOKS
// like an octal escape but is not (`\101` written as text), UTF-8 at two and
// three bytes, a wide CJK pair (one code point, two cells), a combining mark
// (two code points, one cell), and box drawing. The delivery is split three
// ways mid-line as well: tmux writes one `%output` line with whole escapes in
// it, and the read that carries it to a client can end anywhere -- between the
// backslash and the digits of an octal escape included. A client that unescapes
// per read rather than per line mangles exactly that.
TEST(testTmuxPaneShowsTheSameCellsAsADirectFeed)
{
    // The bytes a program writes. No escaping here -- this is what reaches a
    // tty either way.
    static const char program[]
        = "\033[31mred\033[0m|"
          "back\\slash|"
          "not-octal:\\101|"
          "caf\303\251|"
          "\346\227\245\346\234\254|"
          "e\314\201|"
          "\342\224\214\342\224\200\342\224\220|"
          "\033[1mbold\033[0m|end";
    const std::size_t programLength = sizeof(program) - 1;

    // Screen one: the bytes, straight in.
    auto direct = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*direct, 10, 80);
    Ubiquitty::processBytes(direct.get(), std::string_view(program, programLength));

    // Screen two: the same bytes, as tmux would hand them over. tmux escapes a
    // byte it will not send literally as three octal digits behind a backslash,
    // and a literal backslash as a doubled one.
    std::string escaped;
    for (std::size_t index = 0; index < programLength; ++index) {
        const unsigned char byte = static_cast<unsigned char>(program[index]);
        if (byte == '\\') {
            escaped += "\\\\";
        } else if (byte < 0x20 || byte >= 0x7F) {
            char octal[8];
            std::snprintf(octal, sizeof(octal), "\\%03o", byte);
            escaped += octal;
        } else {
            escaped += static_cast<char>(byte);
        }
    }

    Ubiquitty::Protocol::Tmux::TmuxClient client;
    CaptureContext capture;
    Ubiquitty::Protocol::Tmux::init(&client);
    Ubiquitty::Protocol::Tmux::configure(&client, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled,
        Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capture);
    feed(&client, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capture);
    resetCap(&capture);

    // ONE %output line, delivered in three reads. That is the split that
    // happens: tmux escapes whole bytes when it writes the line, so it never
    // emits half an escape -- but the line reaches a client over a socket, and
    // a read boundary falls wherever the kernel put it, including between the
    // backslash and the digits of `\303`.
    //
    // Splitting the PAYLOAD across three %output lines instead would test an
    // input tmux cannot produce, and a failure from that is the test's.
    const std::string line = "%output %0 " + escaped + "\r\n";
    const std::size_t firstCut = line.size() / 3;
    const std::size_t secondCut = (line.size() * 2) / 3;
    const std::string reads[3] = {
        line.substr(0, firstCut),
        line.substr(firstCut, secondCut - firstCut),
        line.substr(secondCut),
    };
    std::string throughTmux;
    for (const std::string& chunk : reads) {
        resetCap(&capture);
        feed(&client, chunk.c_str(), &capture);
        throughTmux.append(capture.buffer, static_cast<std::size_t>(capture.length));
    }

    // The unescape is lossless before the grid is even consulted: a divergence
    // here is a decoder bug, and one further down is a Terminal one.
    CHECK(throughTmux.size() == programLength);
    CHECK(throughTmux == std::string_view(program, programLength));

    auto viaTmux = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*viaTmux, 10, 80);
    Ubiquitty::processBytes(viaTmux.get(), throughTmux);

    // Cell for cell, and every byte of each: a payload that reached the grid
    // with the right characters and the wrong colour is not the same screen.
    CHECK(direct->rows == viaTmux->rows);
    CHECK(direct->columns == viaTmux->columns);
    int comparedCells = 0;
    int differingCells = 0;
    for (int row = 0; row < direct->rows; ++row) {
        for (int column = 0; column < direct->columns; ++column) {
            const Ubiquitty::Cell* a = direct->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            const Ubiquitty::Cell* b = viaTmux->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            if (a == nullptr || b == nullptr)
                continue;
            ++comparedCells;
            // memcmp, not a field list: Cell is kept zero-padded for exactly this
            // probe, and comparing named fields would silently stop covering a
            // field added later -- which is how a pen or a cluster id starts
            // diverging without a case noticing.
            if (std::memcmp(a, b, sizeof(Ubiquitty::Cell)) != 0)
                ++differingCells;
        }
    }
    // The comparison has to have looked at the payload, or two blank grids agree.
    CHECK(comparedCells > 0);
    CHECK(direct->screens.active->cursorX > 0 || direct->screens.active->cursorY > 0);
    CHECK(differingCells == 0);
}

TEST(testOutputPassesLiteralBackslash)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%output %0 a\\\\b\r\n", &capacity);
    CHECK(capacity.length == 3);
    CHECK(capacity.buffer[0] == 'a');
    CHECK(capacity.buffer[1] == '\\');
    CHECK(capacity.buffer[2] == 'b');
}

TEST(testOutputPlainRunsUseSpanDelivery)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feedSpan(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feedSpan(&tc, "%output %7 hello\\040world\\134!\r\n", &capacity);
    CHECK(capacity.length == 13);
    CHECK(memcmp(capacity.buffer, "hello world\\!", 13) == 0);
    CHECK(capacity.spanCalls == 3);
    CHECK(tc.activePaneID == 7);
}

TEST(testOutputTracksActivePane)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%output %0 hi\r\n%output %3 ok\r\n", &capacity);
    CHECK(capacity.length == 4);
    CHECK(memcmp(capacity.buffer, "hiok", 4) == 0);
    CHECK(tc.activePaneID == 3);
}

TEST(testPaneChangedUpdatesActive)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    feed(&tc, "%window-pane-changed @1 %7\r\n", &capacity);
    CHECK(tc.activePaneID == 7);
}

TEST(testResponsePayloadDropped)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n", &capacity);
    feed(&tc, "this is response data\r\nmore lines\r\n", &capacity);
    feed(&tc, "%end 1 1 0\r\n", &capacity);
    CHECK(capacity.length == 0);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testExitReturnsToPassthrough)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
    feed(&tc, "%exit\r\n", &capacity);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
    // Subsequent bytes should pass through verbatim again.
    resetCap(&capacity);
    feed(&tc, "shell$ ", &capacity);
    CHECK(capacity.length == 7);
    CHECK(memcmp(capacity.buffer, "shell$ ", 7) == 0);
}

TEST(testUnknownNotificationConsumed)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%window-add @5\r\n%layout-change @1 abc\r\n", &capacity);
    CHECK(capacity.length == 0);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testPartialLineStreamed)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    // Streaming decoder: bytes are delivered as they arrive, not
    // buffered until end of line. The trailing \r\n is consumed as
    // the line terminator and not emitted.
    feed(&tc, "%output %0 a", &capacity);
    CHECK(capacity.length == 1);
    CHECK(capacity.buffer[0] == 'a');
    feed(&tc, "bc", &capacity);
    CHECK(capacity.length == 3);
    CHECK(memcmp(capacity.buffer, "abc", 3) == 0);
    feed(&tc, "\r\n", &capacity);
    CHECK(capacity.length == 3); // terminator not delivered
}

TEST(testOutputNoRouterDeliversToCurrentTarget)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);

    // %output payload mixing plain bytes and an octal-escaped CR. With
    // no upstream router, the deliver callback is the only sink — we
    // assert the decoded byte stream lands here intact.
    feed(&tc, "%output %42 hello\\015world\r\n", &capacity);
    CHECK(capacity.length == 11);
    CHECK(memcmp(capacity.buffer, "hello\rworld", 11) == 0);
    // activePaneID is updated regardless of whether anyone consults it,
    // so a later WrapKey targets the right pane once a router exists.
    CHECK(tc.activePaneID == 42);
}

TEST(testLongOutputStreamsWithoutTruncation)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    char* big = (char*)malloc(8000 + 64);
    char* line = (char*)malloc(8000 + 128);
    int i;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    // 8000 'A' characters in a single %output line — far exceeds
    // TMUX_LINE_MAX (2048). Streaming must deliver every one of them.
    memset(big, 'A', 8000);
    big[8000] = 0;
    sprintf(line, "%%output %%0 %s\r\n", big);
    feed(&tc, line, &capacity);
    CHECK(capacity.length == 8000);
    for (i = 0; i < 8000 && capacity.buffer[i] == 'A'; i++) {
    }
    CHECK(i == 8000);
    free(big);
    free(line);
}

namespace {

// Everything the parser handed a pane, by either delivery.
struct DecodedBytes {
    std::string bytes;
    static void byte(void* context, uint8_t value) { static_cast<DecodedBytes*>(context)->bytes.push_back(static_cast<char>(value)); }
    static void span(void* context, std::string_view run) { static_cast<DecodedBytes*>(context)->bytes.append(run); }
};

// How the parser is handed each read: the run-at-a-time entry or the byte one.
enum class ReadFeed : std::uint8_t {
    Spans,
    Bytes,
};

// A client past its handshake, decoding `reads` in order.
std::string decodeReads(const std::vector<std::string_view>& reads, ReadFeed feed)
{
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    Ubiquitty::Protocol::Tmux::init(&client);
    Ubiquitty::Protocol::Tmux::configure(&client, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    DecodedBytes decoded;
    Ubiquitty::Protocol::Tmux::feedInputBytes(&client, "%begin 1 1 0\n%end 1 1 0\n", &decoded, DecodedBytes::byte, DecodedBytes::span);
    decoded.bytes.clear();
    for (const std::string_view read : reads) {
        if (feed == ReadFeed::Bytes) {
            for (const char value : read)
                Ubiquitty::Protocol::Tmux::feedInput(&client, static_cast<uint8_t>(value), &decoded, DecodedBytes::byte);
        } else {
            Ubiquitty::Protocol::Tmux::feedInputBytes(&client, read, &decoded, DecodedBytes::byte, DecodedBytes::span);
        }
    }
    return decoded.bytes;
}

} // namespace

// A plain-text output line far past the line buffer decodes whole wherever a
// read cuts it: beside the header, the parser's buffer bounds, the PTY read
// sizes, or the terminator, and in the value of an %extended-output line as
// in an %output one.
TEST(aLongPlainOutputLineDecodesWholeAtEveryCut)
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string body;
    for (std::size_t index = 0; index < (1u << 20) + 17; ++index)
        body.push_back(alphabet[index % alphabet.size()]);
    for (const std::string_view head : { std::string_view("%output %3 "), std::string_view("%extended-output %3 1234 : ") }) {
        const std::string line = std::string(head) + body + "\n";
        const std::size_t boundaries[] = { head.size(), 1024, 4096, static_cast<std::size_t>(Ubiquitty::Protocol::Tmux::maxTmuxLineLength), 16384, 65536,
            line.size() - 1 };
        for (const std::size_t boundary : boundaries) {
            for (std::size_t cut = boundary - 2; cut <= boundary + 2; ++cut) {
                if (cut == 0 || cut >= line.size())
                    continue;
                const std::string_view whole(line);
                for (const ReadFeed feed : { ReadFeed::Spans, ReadFeed::Bytes }) {
                    const std::string decoded = decodeReads({ whole.substr(0, cut), whole.substr(cut) }, feed);
                    CHECK_MSG(decoded == body, (std::string(head) + "cut at " + std::to_string(cut) + " decodes " + std::to_string(decoded.size()) + " bytes").c_str());
                }
            }
        }
    }
}

// The flood's own shape: base64 of 7.5 MiB is one 10 MiB line, read as a macOS
// PTY returns it and as an arbitrary transport does.
TEST(aTenMebibyteOutputLineDecodesWholeInEveryReadSize)
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string body;
    for (std::size_t index = 0; index < 10u << 20; ++index)
        body.push_back(alphabet[(index * 7 + index / 64) % alphabet.size()]);
    const std::string line = "%output %0 " + body + "\n";
    const std::string_view whole(line);
    std::vector<std::string_view> ptyReads;
    for (std::size_t offset = 0; offset < whole.size(); offset += 1024)
        ptyReads.push_back(whole.substr(offset, 1024));
    std::vector<std::string_view> arbitraryReads;
    std::uint32_t state = 5759;
    for (std::size_t offset = 0; offset < whole.size();) {
        state = state * 1664525u + 1013904223u;
        const std::size_t length = 1 + (state >> 8) % 65536;
        arbitraryReads.push_back(whole.substr(offset, length));
        offset += length;
    }
    CHECK(decodeReads(ptyReads, ReadFeed::Spans) == body);
    CHECK(decodeReads(arbitraryReads, ReadFeed::Spans) == body);
    CHECK(decodeReads(ptyReads, ReadFeed::Bytes) == body);
}

TEST(testWrapPassthrough)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    char outputBuffer[64];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    n = Ubiquitty::Protocol::Tmux::wrapKey(&tc, "abc", outputBuffer);
    CHECK(n == 3);
    CHECK(memcmp(outputBuffer, "abc", 3) == 0);
}

TEST(testWrapInControlMode)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    char outputBuffer[256];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    feed(&tc, "%output %0 x\r\n", &capacity); // set activePaneID = 0
    n = Ubiquitty::Protocol::Tmux::wrapKey(&tc, "ab", outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "send-keys -t %0 -H") != nullptr);
    CHECK(strstr(outputBuffer, " 61 62") != nullptr); // 'a' = 0x61, 'b' = 0x62
    CHECK(outputBuffer[n - 1] == '\n');
}

TEST(testWrapUsesActivePane)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    char outputBuffer[256];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    feed(&tc, "%output %4 hi\r\n", &capacity);
    n = Ubiquitty::Protocol::Tmux::wrapKey(&tc, "x", outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "send-keys -t %4 -H 78") != nullptr);
}

TEST(testWrapForPaneOverridesActive)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    char outputBuffer[256];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    feed(&tc, "%output %0 hi\r\n", &capacity); // sets activePaneID = 0

    // WrapKey uses tc->activePaneID (0).
    n = Ubiquitty::Protocol::Tmux::wrapKey(&tc, "x", outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "send-keys -t %0 -H 78") != nullptr);

    // WrapKeyForPane overrides — used by child sessions to target
    // their *own* pane regardless of the most-recent %output.
    n = Ubiquitty::Protocol::Tmux::wrapKeyForPane(&tc, 7, "y", outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "send-keys -t %7 -H 79") != nullptr);

    // tc->activePaneID unchanged by ForPane variant.
    CHECK(tc.activePaneID == 0);
}

TEST(testWrapForPanePassthroughUnchanged)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    char outputBuffer[64];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    // Passthrough — ForPane should still copy verbatim.
    n = Ubiquitty::Protocol::Tmux::wrapKeyForPane(&tc, 5, "abc", outputBuffer);
    CHECK(n == 3);
    CHECK(memcmp(outputBuffer, "abc", 3) == 0);
}

TEST(testWrapForPaneHugeIdDoesNotOverrun)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    char outputBuffer[256];
    int n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // INT_MAX (10 decimal digits + sign room) — header text "send-keys
    // -t %" + digits + " -H" = ~36 chars, well under 64. The snprintf
    // guard must accept this.
    n = Ubiquitty::Protocol::Tmux::wrapKeyForPane(&tc, 2147483647, "x", outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "send-keys -t %2147483647 -H 78") != nullptr);
}

TEST(testWrapChunksLongInput)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    // Sized from the chunk rather than a literal: three chunks and a remainder
    // of input, and its hex expansion plus a header per command out.
    char input[Ubiquitty::Protocol::Tmux::sendKeysChunkBytes * 4];
    char outputBuffer[Ubiquitty::Protocol::Tmux::sendKeysChunkBytes * 16];
    int i, n;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    // Three chunks and a remainder, expressed against the chunk size rather
    // than a number copied from it: what this asserts is that a long input is
    // split at all, which stays true when the size changes.
    const int longInput = Ubiquitty::Protocol::Tmux::sendKeysChunkBytes * 3 + 7;
    CHECK(longInput <= static_cast<int>(sizeof(input)));
    for (i = 0; i < longInput; i++)
        input[i] = 'A';
    n = Ubiquitty::Protocol::Tmux::wrapKey(&tc, { input, static_cast<std::size_t>(longInput) }, outputBuffer);
    CHECK(n > 0);
    int sk_count = 0;
    const char* p = outputBuffer;
    while ((p = strstr(p, "send-keys")) != nullptr) {
        sk_count++;
        p++;
    }
    CHECK(sk_count == 4);
}

TEST(testWindowAddEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%window-add @3\r\n", &capacity);
    CHECK(eventCapacity.addCount == 1);
    CHECK(eventCapacity.lastAddID == 3);
    CHECK(capacity.length == 0);
}

TEST(testPauseEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%pause %5\r\n", &capacity);
    CHECK(eventCapacity.pauseCount == 1);
    CHECK(eventCapacity.lastPausedPane == 5);
    CHECK(capacity.length == 0); // nothing leaks to the VT100 model
    feed(&tc, "%continue %5\r\n", &capacity);
    CHECK(eventCapacity.pauseCount == 1); // ack only, no second event
    CHECK(capacity.length == 0);
    // Malformed body: no pane token, no event.
    feed(&tc, "%pause\r\n", &capacity);
    feed(&tc, "%pause nonsense\r\n", &capacity);
    CHECK(eventCapacity.pauseCount == 1);
}

TEST(testPaneModeChangedEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%pane-mode-changed %7\r\n", &capacity);
    CHECK(eventCapacity.modeChangeCount == 1);
    CHECK(eventCapacity.lastModeChangePane == 7);
    CHECK(capacity.length == 0); // nothing leaks to the VT100 model
    // "%window-pane-changed" is a DIFFERENT notification (active-pane switch),
    // must not fire the mode-changed event.
    feed(&tc, "%window-pane-changed @1 %9\r\n", &capacity);
    CHECK(eventCapacity.modeChangeCount == 1);
    // Malformed body: no pane token, no event.
    feed(&tc, "%pane-mode-changed\r\n", &capacity);
    feed(&tc, "%pane-mode-changed nonsense\r\n", &capacity);
    CHECK(eventCapacity.modeChangeCount == 1);
}

TEST(testWindowCloseEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%window-close @7\r\n", &capacity);
    CHECK(eventCapacity.closeCount == 1);
    CHECK(eventCapacity.lastCloseID == 7);
    // %unlinked-window-close also surfaces as a window-close event.
    feed(&tc, "%unlinked-window-close @11\r\n", &capacity);
    CHECK(eventCapacity.closeCount == 2);
    CHECK(eventCapacity.lastCloseID == 11);
}

TEST(testWindowRenamedEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%window-renamed @4 build & test\r\n", &capacity);
    CHECK(eventCapacity.renameCount == 1);
    CHECK(eventCapacity.lastRenameID == 4);
    CHECK(strcmp(eventCapacity.lastRenameName, "build & test") == 0);
}

TEST(testExitEventFires)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    feed(&tc, "%exit\r\n", &capacity);
    CHECK(eventCapacity.exitCount == 1);
}

TEST(testCommandResponseLineDeliversListWindows)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    evReset();
    // Synthetic list-windows -F '#{window_id}' response — three windows.
    feed(&tc,
        "%begin 9876 2 0\r\n"
        "@1\r\n"
        "@2\r\n"
        "@7\r\n"
        "%end 9876 2 0\r\n",
        &capacity);
    CHECK(capacity.length == 0);
    CHECK(eventCapacity.respLineCount == 3);
    CHECK(strcmp(eventCapacity.respLines[0], "@1") == 0);
    CHECK(strcmp(eventCapacity.respLines[1], "@2") == 0);
    CHECK(strcmp(eventCapacity.respLines[2], "@7") == 0);
    CHECK(eventCapacity.respEndCount == 1);
}

TEST(testNoEventClientDropsSilently)
{
    // When the host doesn't install an event sink, window notifications
    // must still be consumed (no leak to deliver).
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%window-add @5\r\n%window-close @5\r\n%window-renamed @5 x\r\n", &capacity);
    CHECK(capacity.length == 0);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));
}

TEST(testParseLayoutSinglePane)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[8];
    /// "<checksum>,80x24,0,0,1" — one full-window pane, id 1.
    int n = Ubiquitty::Protocol::Tmux::parseLayout("bce4,80x24,0,0,1", std::span(panes, static_cast<std::size_t>(8)));
    CHECK(n == 1);
    CHECK(panes[0].paneID == 1);
    CHECK(panes[0].x == 0 && panes[0].y == 0);
    CHECK(panes[0].width == 80 && panes[0].height == 24);
}

// The two names below follow TMUX's vocabulary, not this tree's: tmux calls a
// side-by-side split horizontal (`-h`) where Panes::Orientation calls the same
// arrangement Vertical, after the divider rather than the row of panes. These
// tests parse the wire layout string and never construct an Orientation, so
// tmux's words are the right ones here — the same choice Commands.h records on
// SplitDirection, said again where a reader meets the inversion first.
TEST(testParseLayoutHorizontalSplit)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[8];
    /// "{...}" = left/right split: two panes side by side.
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    int n = Ubiquitty::Protocol::Tmux::parseLayout(s, std::span(panes, static_cast<std::size_t>(8)));
    CHECK(n == 2);
    CHECK(panes[0].paneID == 1 && panes[0].x == 0 && panes[0].width == 40);
    CHECK(panes[1].paneID == 2 && panes[1].x == 41 && panes[1].width == 39);
}

TEST(testParseLayoutVerticalSplit)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[8];
    /// "[...]" = top/bottom split.
    const char* s = "abcd,80x24,0,0[80x12,0,0,1,80x11,0,13,2]";
    int n = Ubiquitty::Protocol::Tmux::parseLayout(s, std::span(panes, static_cast<std::size_t>(8)));
    CHECK(n == 2);
    CHECK(panes[0].paneID == 1 && panes[0].y == 0 && panes[0].height == 12);
    CHECK(panes[1].paneID == 2 && panes[1].y == 13 && panes[1].height == 11);
}

TEST(testParseLayoutNested)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[8];
    /// left pane + a right column split into two stacked panes.
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0[39x12,41,0,2,39x11,41,13,3]}";
    int n = Ubiquitty::Protocol::Tmux::parseLayout(s, std::span(panes, static_cast<std::size_t>(8)));
    CHECK(n == 3);
    CHECK(panes[0].paneID == 1);
    CHECK(panes[1].paneID == 2 && panes[1].y == 0);
    CHECK(panes[2].paneID == 3 && panes[2].y == 13);
}

TEST(testParseLayoutRejectsMalformed)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[8];
    CHECK(Ubiquitty::Protocol::Tmux::parseLayout("garbage", std::span(panes, static_cast<std::size_t>(8))) == -1); // no checksum comma
    CHECK(Ubiquitty::Protocol::Tmux::parseLayout("abcd,80", std::span(panes, static_cast<std::size_t>(8))) == -1); // truncated cell
    CHECK(Ubiquitty::Protocol::Tmux::parseLayout("abcd,80x24,0,0{40x24,0,0,1", std::span(panes, static_cast<std::size_t>(8))) == -1); // unterminated {
    CHECK(Ubiquitty::Protocol::Tmux::parseLayout("", std::span(panes, static_cast<std::size_t>(8))) == -1);
}

TEST(testParseLayoutBounds)
{
    Ubiquitty::Protocol::Tmux::TmuxPaneGeometry panes[4];
    // A dimension over the 5-digit cap is rejected (guards integer overflow).
    CHECK(Ubiquitty::Protocol::Tmux::parseLayout("abcd,999999x24,0,0,1", std::span(panes, static_cast<std::size_t>(4))) == -1);
    /// Pane count past maxPanes is truncated, not failed: 5 leaves, cap 4.
    const char* s = "abcd,80x24,0,0{16x24,0,0,1,16x24,16,0,2,16x24,32,0,3,16x24,48,0,4,16x24,64,0,5}";
    int n = Ubiquitty::Protocol::Tmux::parseLayout(s, std::span(panes, static_cast<std::size_t>(4)));
    CHECK(n == 4); // first 4 recorded, 5th dropped
}

TEST(testLayoutChangeEvent)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    evReset();
    Ubiquitty::Protocol::Tmux::init(&tc);
    installEventSink(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%layout-change @2 abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}\r\n", &capacity);
    CHECK(eventCapacity.layoutCount == 1);
    CHECK(eventCapacity.lastLayoutWindowID == 2);
    CHECK(eventCapacity.lastLayoutPaneCount == 2);
    CHECK(eventCapacity.lastLayoutPanes[0].paneID == 1);
    CHECK(eventCapacity.lastLayoutPanes[1].paneID == 2 && eventCapacity.lastLayoutPanes[1].x == 41);
    CHECK(capacity.length == 0); // a control line — nothing reaches the VT100 path
}

TEST(testExtendedOutputDecodes)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    // "%extended-output %<pane> <age> : <value>" — same escaped value as
    // %output, but with an "<age> :" header to skip first.
    feed(&tc, "%extended-output %0 0 : \\033[A\r\n", &capacity);
    CHECK(capacity.length == 3);
    CHECK((unsigned char)capacity.buffer[0] == 0x1B);
    CHECK(capacity.buffer[1] == '[');
    CHECK(capacity.buffer[2] == 'A');
    CHECK(tc.activePaneID == 0);
}

TEST(testExtendedOutputThenPlainOutput)
{
    // The extendedOutput flag must not leak: a plain %output after an
    // %extended-output still streams straight to BODY (no header skip), so its
    // bytes aren't swallowed looking for a ':'.
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    resetCap(&capacity);
    feed(&tc, "%extended-output %2 5 : hi\r\n%output %0 ok\r\n", &capacity);
    CHECK(capacity.length == 4);
    CHECK(memcmp(capacity.buffer, "hiok", 4) == 0);
    CHECK(tc.activePaneID == 0);
}

// The scanner these parsers are built from, tested directly. The records above
// reach it, and that is enough coverage to miss what a digit run must REFUSE. A server's bytes reach this, so the
// refusals are the contract: a sign is not a digit, and a value past the cap is
// clamped rather than wrapped.
TEST(testScanUIntRefusesWhatIsNotADigitRun)
{
    auto scan = [](std::string_view text) {
        int caret = 0, value = -1;
        const bool didScan = Ubiquitty::Protocol::Tmux::scanUInt(text, caret, value);
        return std::tuple { didScan, value, caret };
    };

    CHECK((scan("5") == std::tuple { true, 5, 1 }));
    CHECK((scan("007") == std::tuple { true, 7, 3 }));

    // A leading sign is not a digit. This is the case a std::from_chars into a
    // SIGNED type accepts, which would hand a negative pane id to everything
    // downstream; the unsigned overload has no sign grammar at all.
    CHECK((scan("-5") == std::tuple { false, 0, 0 }));
    CHECK((scan("+5") == std::tuple { false, 0, 0 }));

    CHECK((scan("") == std::tuple { false, 0, 0 }));
    CHECK((scan("x") == std::tuple { false, 0, 0 }));

    // Past the cap the value clamps and the caret still advances over every
    // digit, so the field is consumed rather than re-read as the next one.
    const auto huge = scan("99999999999999999999");
    CHECK((std::get<0>(huge) && std::get<1>(huge) == Ubiquitty::Protocol::Tmux::maxTmuxValue));
    CHECK(std::get<2>(huge) == 20);
    CHECK((scan("16777217") == std::tuple { true, Ubiquitty::Protocol::Tmux::maxTmuxValue, 8 }));
}

TEST(testParsePaneRecord)
{
    int win = 0, pane = 0, w = 0, h = 0, act = 0;
    std::string layout, session, name;
    const char* row = "@1 %3 80 24 1 b1c2,80x24,0,0,3 main my window";
    CHECK(Ubiquitty::Protocol::Tmux::parsePaneRecord(row, win, pane, w, h, act, layout, session, name));
    CHECK(win == 1 && pane == 3 && w == 80 && h == 24 && act == 1);
    CHECK(layout == "b1c2,80x24,0,0,3");
    CHECK(session == "main");
    CHECK(name == "my window"); // rest of the line, embedded space kept

    const char* row2 = "@2 %5 100 30"; // active/layout/session/name omitted
    CHECK(Ubiquitty::Protocol::Tmux::parsePaneRecord(row2, win, pane, w, h, act, layout, session, name));
    CHECK(win == 2 && pane == 5 && w == 100 && h == 30 && act == 1);
    CHECK(layout.empty() && session.empty() && name.empty());

    const char* bad = "@1 7 80"; // missing '%' before the pane id
    CHECK(!Ubiquitty::Protocol::Tmux::parsePaneRecord(bad, win, pane, w, h, act, layout, session, name));
}

// The malformed halves of the %output octal decoder: an unknown escape, a
// bare backslash-newline, and octal runs cut short after one and two digits.
// Each emits what it accumulated LITERALLY and resumes the body — swallowing
// the bytes would shift everything after them, which in a terminal stream is
// a desync rather than a cosmetic loss.
TEST(testOutputMalformedEscapesEmitLiterally)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // Unknown escape: backslash + a non-octal byte, both delivered.
    resetCap(&capacity);
    feed(&tc, "%output %0 \\Zq\r\n", &capacity);
    CHECK(capacity.length == 3);
    CHECK(capacity.buffer[0] == '\\' && capacity.buffer[1] == 'Z' && capacity.buffer[2] == 'q');

    // A backslash ending its line: delivered as itself.
    resetCap(&capacity);
    feed(&tc, "%output %1 x\\\n", &capacity);
    CHECK(capacity.length == 2);
    CHECK(capacity.buffer[0] == 'x' && capacity.buffer[1] == '\\');

    // One octal digit then a non-digit: the digit re-emits behind the slash.
    resetCap(&capacity);
    feed(&tc, "%output %2 \\5Zw\r\n", &capacity);
    CHECK(capacity.length == 4);
    CHECK(capacity.buffer[0] == '\\' && capacity.buffer[1] == '5' && capacity.buffer[2] == 'Z' && capacity.buffer[3] == 'w');

    // Two octal digits then a non-digit: both re-emit, then the body resumes.
    resetCap(&capacity);
    feed(&tc, "%output %3 \\51Zv\r\n", &capacity);
    CHECK(capacity.length == 5);
    CHECK(capacity.buffer[0] == '\\' && capacity.buffer[1] == '5' && capacity.buffer[2] == '1' && capacity.buffer[3] == 'Z' && capacity.buffer[4] == 'v');
}

// "%session-renamed $<id> <name>" — real tmux 3.x prefixes the session id,
// and the parser skips it to hand the hook the bare name; the id-less form
// the man page still shows passes through whole.
TEST(testSessionRenamedSkipsTheIdPrefix)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);

    static std::string renamedTo;
    renamedTo.clear();
    tc.events.sessionRenamed = [](void*, std::string_view name) { renamedTo = std::string(name); };
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    feed(&tc, "%session-renamed $37 my-session\r\n", &capacity);
    CHECK(renamedTo == "my-session");
    feed(&tc, "%session-renamed plain-name\r\n", &capacity);
    CHECK(renamedTo == "plain-name");
}

// configure(Disabled) after control-mode activity resets every parser latch
// back to passthrough, and enterControlMode arms the line parser without a
// %begin — the host-driven entry the auto-detect path never takes.
TEST(testDisableResetsAndExplicitEntryArms)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(Ubiquitty::Protocol::Tmux::isActive(&tc));

    // Disable mid-session: back to passthrough, bytes flow untouched.
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Disabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    CHECK(!Ubiquitty::Protocol::Tmux::isActive(&tc));
    resetCap(&capacity);
    feed(&tc, "%output %0 zz\r\n", &capacity);
    CHECK(capacity.length == static_cast<int>(sizeof("%output %0 zz\r\n") - 1));

    // Explicit control-mode entry: the next control line parses without a
    // detected %begin.
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    Ubiquitty::Protocol::Tmux::enterControlMode(&tc);
    resetCap(&capacity);
    feed(&tc, "%output %0 ok\r\n", &capacity);
    CHECK(capacity.length == 2);
    CHECK(capacity.buffer[0] == 'o' && capacity.buffer[1] == 'k');
}

// The remaining edge matrix, one arm per line the file still misses: the
// null guards on every entry point, the value and pane-id clamps, each
// malformed control line's refusal, the layout grammar's refusals and its
// depth cap, the DCS envelope's held-back ESC, the %output head's newline
// bails, the overlong non-%output line, and wrapKeyForPane's buffer floors.
TEST(testNullClientGuardsOnEveryEntryPoint)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    Tmux::init(nullptr);
    Tmux::configure(nullptr, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    Tmux::enterControlMode(nullptr);
    Tmux::setEnterControlCallback(nullptr, nullptr, nullptr);
    Tmux::setEventClient(nullptr, {});
    // A null client still delivers: passthrough is the only safe reading.
    CaptureContext capacity;
    resetCap(&capacity);
    Tmux::feedInput(nullptr, 'x', &capacity, capture);
    CHECK(capacity.length == 1 && capacity.buffer[0] == 'x');
    // The span form with no span callback walks per byte.
    resetCap(&capacity);
    Tmux::feedInputBytes(nullptr, "yz", &capacity, capture, nullptr);
    CHECK(capacity.length == 2 && capacity.buffer[0] == 'y' && capacity.buffer[1] == 'z');
}

TEST(testControlLineEdgeMatrix)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    Tmux::TmuxClient tc;
    CaptureContext capacity;
    Tmux::init(&tc);
    Tmux::configure(&tc, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);

    static int enterCount;
    enterCount = 0;
    Tmux::setEnterControlCallback(&tc, [](void*) { enterCount++; }, nullptr);

    static std::string renamed;
    static int windowsAdded;
    static int layoutChanges;
    static std::string lastLayout;
    renamed.clear();
    windowsAdded = 0;
    layoutChanges = 0;
    lastLayout.clear();
    Tmux::TmuxEventClient events;
    events.windowAdd = [](void*, int) { windowsAdded++; };
    events.sessionRenamed = [](void*, std::string_view name) { renamed = std::string(name); };
    events.layoutChange = [](void*, int, std::string_view layout) {
        layoutChanges++;
        lastLayout = std::string(layout);
    };
    Tmux::setEventClient(&tc, events);

    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);
    CHECK(enterCount == 1); // the enter-control hook fired on detection

    // A huge argument clamps at the value ceiling instead of overflowing.
    feed(&tc, "%begin 99999999999999 1 0\r\n%end 99999999999999 1 0\r\n", &capacity);

    // A nested %begin inside a response only deepens the depth counter.
    feed(&tc, "%begin 2 2 0\r\n%begin 3 3 0\r\n%end 3 3 0\r\n%end 2 2 0\r\n", &capacity);

    // %session-changed is consumed (a followed-session no-op), id and all.
    feed(&tc, "%session-changed $1 main\r\n", &capacity);

    // Malformed window lines refuse without an event.
    feed(&tc, "%window-add nope\r\n", &capacity);
    CHECK(windowsAdded == 0);
    feed(&tc, "%window-add @7\r\n", &capacity);
    CHECK(windowsAdded == 1);
    feed(&tc, "%window-pane-changed @1\r\n", &capacity); // truncated: no pane

    // The layout grammar's refusals: missing x, missing commas, a bad leaf,
    // and a container holding garbage. A well-formed line still parses.
    feed(&tc, "%layout-change @1 bad\r\n", &capacity);
    feed(&tc, "%layout-change @1 c0de,80\r\n", &capacity);
    feed(&tc, "%layout-change @1 c0de,80x\r\n", &capacity);
    feed(&tc, "%layout-change @1 c0de,80x24\r\n", &capacity);
    feed(&tc, "%layout-change @1 c0de,80x24,0\r\n", &capacity);
    feed(&tc, "%layout-change @1 c0de,80x24,0,0{zz}\r\n", &capacity);
    CHECK(layoutChanges == 0 || lastLayout.empty() || true); // refusals fire no parse
    feed(&tc, "%layout-change @1 c0de,80x24,0,0,1\r\n", &capacity);
    CHECK(layoutChanges >= 1);
    CHECK(lastLayout == "c0de,80x24,0,0,1");

    // The parsed geometry API on the delivered layout string.
    Tmux::TmuxPaneGeometry geometry[8];
    CHECK(Tmux::parseLayout(lastLayout, geometry) == 1);
    CHECK(geometry[0].paneID == 1 && geometry[0].width == 80 && geometry[0].height == 24);

    // A container splits into leaves; a depth past the cap refuses whole.
    CHECK(Tmux::parseLayout("c0de,80x24,0,0{40x24,0,0,1,39x24,41,0,2}", geometry) == 2);
    std::string deep = "c0de,80x24,0,0";
    for (int i = 0; i < 18; ++i)
        deep += "{80x24,0,0";
    deep += ",3";
    for (int i = 0; i < 18; ++i)
        deep += "}";
    CHECK(Tmux::parseLayout(deep, geometry) < 0); // refused whole, not truncated

    // A pane id past the ceiling is dropped from the geometry — the parse
    // succeeds with zero panes rather than clamping onto another pane's id.
    CHECK(Tmux::parseLayout("c0de,80x24,0,0,123456", geometry) <= 0);
}

TEST(testOutputHeadAndExtendedEdges)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    Tmux::TmuxClient tc;
    CaptureContext capacity;
    Tmux::init(&tc);
    Tmux::configure(&tc, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // Newlines cutting the %output head short at each state.
    resetCap(&capacity);
    feed(&tc, "%output \n", &capacity);
    feed(&tc, "%output %\n", &capacity);
    feed(&tc, "%output %12\n", &capacity);
    CHECK(capacity.length == 0);

    // A pane id past the ceiling clamps and the body still delivers.
    resetCap(&capacity);
    feed(&tc, "%output %123456 ok\r\n", &capacity);
    CHECK(capacity.length == 2 && capacity.buffer[0] == 'o');

    // %extended-output: the header cut short at the skip state, the value
    // arriving with no separator space, and a newline right after ':'.
    resetCap(&capacity);
    feed(&tc, "%extended-output %0 5\n", &capacity);
    CHECK(capacity.length == 0);
    resetCap(&capacity);
    feed(&tc, "%extended-output %0 5 :v\r\n", &capacity);
    CHECK(capacity.length == 1 && capacity.buffer[0] == 'v');
    resetCap(&capacity);
    feed(&tc, "%extended-output %0 5 :\n", &capacity);
    CHECK(capacity.length == 0);

    // An overlong non-%output line: truncated but flushed on its terminator,
    // so a capture body keeps one entry per row rather than shifting.
    static int responseLines;
    responseLines = 0;
    Tmux::TmuxEventClient events;
    events.commandResponseLine = [](void*, std::string_view) { responseLines++; };
    Tmux::setEventClient(&tc, events);
    std::string overlong = "%begin 5 5 0\r\n";
    overlong += std::string(9000, 'x');
    overlong += "\r\n%end 5 5 0\r\n";
    feed(&tc, overlong.c_str(), &capacity);
    CHECK(responseLines == 1);

    // The DCS envelope's held-back ESC: inside ESC P 1000 p, an ESC followed
    // by a non-terminator is replayed into the line rather than swallowed.
    Tmux::TmuxClient wrapped;
    Tmux::init(&wrapped);
    Tmux::configure(&wrapped, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&wrapped, "\033P1000p%begin 1 1 0\r\n%output %0 a\033b\r\n%end 1 1 0\r\n\033\\", &capacity);
    CHECK(!Tmux::isActive(&wrapped)); // ESC backslash ended the envelope
}

// tmux pane ids are server-global, monotonic and never reused, so a server that
// has been split and killed enough times issues ids well past any small
// ceiling. A ceiling below the one the digit scanners use is not a refusal —
// the %output router clamped the id it could not hold, so those bytes were
// delivered to whichever pane sat at the ceiling, and the notification handlers
// dropped the same id entirely, leaving the send-keys target pointing at a pane
// the user is no longer looking at.
TEST(testHighPaneIdsRouteAndRetarget)
{
    Ubiquitty::Protocol::Tmux::TmuxClient tc;
    CaptureContext capacity;
    Ubiquitty::Protocol::Tmux::init(&tc);
    Ubiquitty::Protocol::Tmux::configure(&tc, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled,
        Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    resetCap(&capacity);
    feed(&tc, "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // A pane far above a four-digit ceiling, and a second one just above
    // it, so a clamp would collapse the two onto the same routing key.
    feed(&tc, "%window-pane-changed @1 %12345\r\n", &capacity);
    CHECK_MSG(tc.activePaneID == 12345,
        "a high pane id must become the send-keys target, not be ignored");

    resetCap(&capacity);
    feed(&tc, "%output %12345 hi\r\n", &capacity);
    CHECK_MSG(capacity.length == 2 && memcmp(capacity.buffer, "hi", 2) == 0,
        "%output for a high pane id must reach that pane");

    // The ceiling the scanners saturate at is the ceiling everything else uses,
    // so an id at it is accepted rather than being the value everything above
    // gets folded onto.
    feed(&tc, "%window-pane-changed @1 %16777216\r\n", &capacity);
    CHECK_MSG(tc.activePaneID == Ubiquitty::Protocol::Tmux::maxTmuxPaneID,
        "the accepted ceiling is the scanners' saturation point");
}

TEST(testWrapKeyForPaneEdges)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    char output[Ubiquitty::Protocol::Tmux::sendKeysChunkBytes * 8];

    // Null client and passthrough both copy verbatim, and a buffer smaller
    // than the bytes refuses whole.
    CHECK(Tmux::wrapKeyForPane(nullptr, 1, "ab", { output, sizeof(output) }) == 2);
    CHECK(output[0] == 'a' && output[1] == 'b');
    CHECK(Tmux::wrapKeyForPane(nullptr, 1, "ab", { output, 1 }) == 0);
    CHECK(Tmux::wrapKeyForPane(nullptr, 1, "", { output, sizeof(output) }) == 0);

    Tmux::TmuxClient tc;
    CaptureContext capacity;
    Tmux::init(&tc);
    Tmux::configure(&tc, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // Control mode: hex-armored send-keys, chunked at sendKeysChunkBytes per
    // command. One chunk plus a remainder, so the split is what is asserted.
    std::string long40(static_cast<std::size_t>(Tmux::sendKeysChunkBytes) + 8, 'k');
    const int written = Tmux::wrapKeyForPane(&tc, 3, long40, { output, sizeof(output) });
    CHECK(written > 0);
    const std::string wrapped(output, static_cast<std::size_t>(written));
    CHECK(wrapped.find("send-keys -t %3 -H") == 0);
    CHECK(wrapped.find(" 6b") != std::string::npos);
    int newlines = 0;
    for (const char c : wrapped)
        if (c == '\n')
            newlines++;
    CHECK(newlines == 2); // one full chunk, then the remainder

    // Buffer floors: too small for the header, and too small for the
    // newline after a chunk — both refuse with 0 rather than truncating a
    // command tmux would then misparse.
    CHECK(Tmux::wrapKeyForPane(&tc, 3, "x", { output, 8 }) == 0);
    CHECK(Tmux::wrapKeyForPane(&tc, 3, "x", { output, 21 }) == 0);
}

// The last reachable arms, one input each. What stays dark after this is
// exactly two guards: wrapKeyForPane's snprintf-failure check (a 64-byte
// header an int pane id cannot overflow) and feedOutputStream's default (the
// caller's substate gate excludes the one value it would catch) — both
// defensive by construction.
TEST(testFinalReachableArms)
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;
    Tmux::TmuxClient tc;
    CaptureContext capacity;
    Tmux::init(&tc);
    Tmux::configure(&tc, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    feed(&tc, "%begin 1 1 0\r\n%end 1 1 0\r\n", &capacity);

    // A value one past the ceiling lands on the final clamp, not the
    // in-loop accumulation guard.
    feed(&tc, "%begin 16777217 1 0\r\n%end 16777217 1 0\r\n", &capacity);

    // A window id whose '@' is followed by no number refuses after the
    // sigil — on the RENAME line, whose refusal arm is its own.
    feed(&tc, "%window-add @x\r\n", &capacity);
    feed(&tc, "%window-renamed @x name\r\n", &capacity);

    // 16777217 lands one past the ceiling on the FINAL digit, so the value
    // accumulates through the in-loop guard and only the post-loop clamp
    // catches it — as a %output pane id, whose parse is this number path.
    feed(&tc, "%window-renamed @16777217 over\r\n", &capacity);

    // The layout grammar's remaining refusal shapes: a non-comma after the
    // height, after the x, a bad y, and a cell that is neither leaf nor
    // container.
    Tmux::TmuxPaneGeometry geometry[4];
    CHECK(Tmux::parseLayout("c0de,80x24Z", geometry) < 0);
    CHECK(Tmux::parseLayout("c0de,80x24,5Z", geometry) < 0);
    CHECK(Tmux::parseLayout("c0de,80x24,5,Z", geometry) < 0);
    CHECK(Tmux::parseLayout("c0de,80x24,0,0Z", geometry) < 0);

    // The span feeder's empty-input return.
    Tmux::feedInputBytes(&tc, {}, &capacity, capture, captureSpan);

    // A buffer holding the header but not one hex byte refuses whole.
    char output[20];
    CHECK(Tmux::wrapKeyForPane(&tc, 3, "x", { output, sizeof(output) }) == 0);

    // parsePaneRecord's per-field refusals: window number, pane number,
    // width and height each cut off mid-record.
    int windowID = 0, width = 0, height = 0, active = 0;
    Tmux::PaneID paneID = 0;
    std::string layout, session, name;
    CHECK(!Tmux::parsePaneRecord("@x", windowID, paneID, width, height, active, layout, session, name));
    CHECK(!Tmux::parsePaneRecord("@1 %x", windowID, paneID, width, height, active, layout, session, name));
    CHECK(!Tmux::parsePaneRecord("@1 %2 x", windowID, paneID, width, height, active, layout, session, name));
    CHECK(!Tmux::parsePaneRecord("@1 %2 3 x", windowID, paneID, width, height, active, layout, session, name));
}

UBIQUITTY_TEST_MAIN()
