// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_signal — a key the program's terminal answers with a signal
// (Docs/sync.md §6.7): it is written at once, past the input the pane holds
// unwritten. Where the terminal discards its unread input, the pane's goes
// with it, whoever sent it, and each client is told of its own before any
// count passes it. Where the terminal keeps its input, or signals are off,
// nothing is dropped.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Keys/Keymap.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr ID128 first { 11, 0 };
constexpr ID128 second { 12, 0 };
constexpr std::uint8_t interruptByte = 0x03;
constexpr std::string_view bracketOpen = "\x1b[200~";
constexpr std::string_view bracketClose = "\x1b[201~";

std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

RawInput rawOf(std::string_view text)
{
    return { .bytes = bytesOf(text) };
}

/// A program's terminal, as the pane's input reaches it: one that reads or
/// does not, and that answers Ctrl-C as its modes say. A signal is taken
/// whether or not the program reads, as a terminal takes one past the input
/// it holds unread.
class TerminalSink final : public Ubiquitty::Runtime::ByteSink {
public:
    /// Takes bytes when the program reads, or when one of them signals.
    [[nodiscard]] WriteStatus writeAll(ID128 clientInstance, std::span<const std::uint8_t> bytes) override
    {
        if (!isReading && signalEffect(bytes) == SignalEffect::None)
            return WriteStatus::Refused;
        if (signalRefusals > 0 && signalEffect(bytes) != SignalEffect::None) {
            --signalRefusals;
            return WriteStatus::Refused;
        }
        written.append(bytes.begin(), bytes.end());
        writes.emplace_back(bytes.begin(), bytes.end());
        writers.push_back(clientInstance);
        return WriteStatus::Written;
    }

    /// Answers for Ctrl-C among the bytes as `effect` says.
    [[nodiscard]] SignalEffect signalEffect(std::span<const std::uint8_t> bytes) override
    {
        return std::ranges::find(bytes, interruptByte) == bytes.end() ? SignalEffect::None : effect;
    }

    std::string written;
    std::vector<std::string> writes;
    std::vector<ID128> writers;
    bool isReading = false;
    SignalEffect effect = SignalEffect::SignalsAndDiscards;
    /// How many writes holding a signal are refused first, as a stream whose
    /// own queue is full refuses one.
    int signalRefusals = 0;
};

/// A pane whose input goes through the host's encoder to a `TerminalSink`.
struct SignalPane {
    Ubiquitty::Terminal terminal;
    std::unique_ptr<PaneSource> source;
    TerminalSink sink;
    std::unique_ptr<Ubiquitty::Runtime::RemoteInputEncoder> encoder;

    /// Creates the pane, its program asking for bracketed paste, reading
    /// nothing.
    SignalPane()
    {
        Ubiquitty::resize(terminal, 24, 80);
        source = std::make_unique<PaneSource>(terminal, ID128 { 9, 10 });
        UbiquittyTest::feed(terminal, "\x1b[?2004h$ ");
        source->publish(FrameClass::Structural, 1);
        encoder = std::make_unique<Ubiquitty::Runtime::RemoteInputEncoder>(terminal, sink);
        source->setInputWriter(*encoder);
    }
};

/// Ctrl with a letter, as a platform reports it.
KeyEvent controlKey(char letter)
{
    return { .key = 0, .modifiers = Ubiquitty::Keys::Keymap::keymapModifierCtrl, .kind = KeyEventKind::Press, .text = {}, .unshiftedCodePoint = static_cast<std::uint32_t>(letter), .shiftedCodePoint = static_cast<std::uint32_t>(letter), .baseLayoutCodePoint = 0, .wasModified = true };
}

/// Sends Ctrl-C as a key.
bool pressInterrupt(UbiquittyTest::PaneConnection& connection)
{
    const std::array<KeyEvent, 1> keys { controlKey('c') };
    return connection.client.sendKeys(connection.handle, keys).has_value();
}

/// The frames a stream holds, by kind.
std::vector<FrameKind> kindsOf(std::span<const std::uint8_t> stream)
{
    std::vector<FrameKind> kinds;
    FrameDecoder decoder;
    decoder.append(stream);
    while (const std::optional<Frame> frame = decoder.next())
        kinds.push_back(frame->kind);
    return kinds;
}

/// Runs a host's service and takes what it sends, without delivering it.
std::vector<std::uint8_t> serviced(HostConnection& host, std::uint64_t now)
{
    host.service(now);
    std::vector<std::uint8_t> stream;
    while (host.popStreamFrame(stream, now)) {
    }
    return stream;
}

} // namespace

// Two people type into one pane whose program reads nothing. The second's
// Ctrl-C reaches the program at once and the first's queued line is dropped,
// as the terminal drops its unread input; the first is told, before any
// count passes the line, and the line is never written or taken again.
TEST(anInterruptDropsAnothersQueuedLineAndTellsThem)
{
    SignalPane pane;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::optional<std::uint64_t> line = a.client.sendRaw(a.handle, bytesOf("rm -rf build\r"));
    REQUIRE(line.has_value());
    a.pump(300);
    REQUIRE(pane.source->hasPendingInput());
    CHECK(pane.sink.written.empty());

    REQUIRE(pressInterrupt(b));
    b.pump(400);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    CHECK(pane.sink.writers == std::vector<ID128> { second });
    CHECK(!pane.source->hasPendingInput());
    CHECK_EQ(pane.source->heldInputBytes(), 0u);
    CHECK(pane.source->inputLedger().lastWritten(first) == line);
    CHECK(pane.source->queueInput(first, *line, rawOf("rm -rf build\r"), 401) == EnqueueOutcome::Duplicate);

    // The first is told of the run before anything that counts it written.
    CHECK(a.host.nextServiceMicroseconds(402) == std::optional<std::uint64_t> { 402 });
    const std::vector<std::uint8_t> down = serviced(a.host, 30'000);
    const std::vector<FrameKind> kinds = kindsOf(down);
    const auto told = std::ranges::find(kinds, FrameKind::InputDiscarded);
    REQUIRE(told != kinds.end());
    CHECK(std::ranges::find(kinds.begin(), told, FrameKind::Receipt) == told);
    CHECK(std::ranges::find(kinds.begin(), told, FrameKind::Update) == told);
    CHECK_EQ(a.client.receipt(a.handle).inputWritten, 0u);
    a.client.receiveStream(down, 30'000);
    const std::vector<InputDiscarded> runs = a.client.takeInputDiscards(a.handle);
    constexpr InputDiscarded whole { .inputWritten = 0, .throughSeq = 1 };
    CHECK(runs == std::vector<InputDiscarded> { whole });
    CHECK(a.client.takeInputDiscards(a.handle).empty());
    CHECK_EQ(a.host.counters().inputDiscardsSent, 1u);
    // Told once, and the one who pressed the key is told nothing.
    for (std::uint64_t now = 60'000; now < 200'000; now += 30'000) {
        a.pump(now);
        b.pump(now);
    }
    CHECK_EQ(a.host.counters().inputDiscardsSent, 1u);
    CHECK_EQ(b.host.counters().inputDiscardsSent, 0u);
    CHECK(b.client.takeInputDiscards(b.handle).empty());
    CHECK_EQ(a.client.receipt(a.handle).inputWritten, *line);
    CHECK_EQ(b.client.receipt(b.handle).inputWritten, 1u);

    // The program reads again: what is typed next is written, the line never.
    pane.sink.isReading = true;
    REQUIRE(a.client.sendRaw(a.handle, bytesOf("x")).has_value());
    a.pump(300'000);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03x"));
    CHECK(a.host.state() == ConnectionState::Attached);
    CHECK(b.host.state() == ConnectionState::Attached);
}

// With signals off the key is data: it waits its turn behind what was typed
// before it, and nothing is dropped.
TEST(aKeyThatDoesNotSignalDropsNothing)
{
    SignalPane pane;
    pane.sink.effect = SignalEffect::None;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(a.client.sendRaw(a.handle, bytesOf("iHello")).has_value());
    a.pump(300);
    REQUIRE(pressInterrupt(b));
    b.pump(400);
    CHECK(pane.sink.written.empty());
    CHECK(pane.source->hasPendingInput());
    pane.sink.isReading = true;
    pane.source->drainInput(500);
    CHECK_STR_EQ(pane.sink.written, std::string("iHello\x03"));
    for (std::uint64_t now = 30'000; now < 120'000; now += 30'000) {
        a.pump(now);
        b.pump(now);
    }
    CHECK_EQ(a.host.counters().inputDiscardsSent, 0u);
    CHECK(a.client.takeInputDiscards(a.handle).empty());
    CHECK(pane.source->inputLedger().discards(first).empty());
}

// A terminal that keeps its input at a signal (NOFLSH): the key still goes
// at once, ahead of what waits, and nothing is dropped. The key counts as
// written only once everything its client typed before it is.
TEST(aSignalThatKeepsTheInputGoesFirstAndDropsNothing)
{
    SignalPane pane;
    pane.sink.effect = SignalEffect::Signals;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(a.client.sendRaw(a.handle, bytesOf("one")).has_value());
    REQUIRE(b.client.sendRaw(b.handle, bytesOf("two")).has_value());
    a.pump(300);
    b.pump(300);
    const std::array<KeyEvent, 1> keys { controlKey('c') };
    const std::optional<std::uint64_t> key = a.client.sendKeys(a.handle, keys);
    REQUIRE(key.has_value());
    a.pump(400);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    CHECK(pane.source->hasPendingInput());
    // Its own earlier line still waits, so the client's count stays below it,
    // and the key sent again is not written again.
    CHECK(!pane.source->inputLedger().lastWritten(first).has_value());
    CHECK(pane.source->queueInput(first, *key, KeyInput { .events = { controlKey('c') } }, 401) == EnqueueOutcome::Duplicate);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    pane.sink.isReading = true;
    pane.source->drainInput(500);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03onetwo"));
    CHECK(pane.source->inputLedger().lastWritten(first) == key);
    CHECK(pane.source->inputLedger().discards(first).empty());
    CHECK(pane.source->inputLedger().discards(second).empty());
    for (std::uint64_t now = 30'000; now < 120'000; now += 30'000) {
        a.pump(now);
        b.pump(now);
    }
    CHECK_EQ(a.host.counters().inputDiscardsSent, 0u);
    CHECK_EQ(b.host.counters().inputDiscardsSent, 0u);
}

// Only a client that may drive causes the drop: a viewer's Ctrl-C, or one
// from a client without the Drive scope, closes its connection, writes
// nothing and leaves what waits where it is.
TEST(aClientThatMayNotDriveDropsNothing)
{
    for (const bool isViewer : { true, false }) {
        SignalPane pane;
        UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        UbiquittyTest::PaneConnection watcher(*pane.source, second, isViewer ? ClientKind::Viewer : ClientKind::Human, isViewer ? UbiquittyTest::DriveScope::Granted : UbiquittyTest::DriveScope::Withheld);
        REQUIRE(a.client.sendRaw(a.handle, bytesOf("make\r")).has_value());
        a.pump(300);
        const std::size_t held = pane.source->heldInputBytes();
        REQUIRE(held > 0);
        // The client sends regardless; the host is what refuses.
        (void)pressInterrupt(watcher);
        watcher.pump(400);
        CHECK(watcher.host.state() == ConnectionState::Closed);
        CHECK(watcher.host.byeReason() == std::optional { ByeReason::Malformed });
        CHECK(pane.sink.written.empty());
        CHECK_EQ(pane.source->heldInputBytes(), held);
        CHECK(pane.source->inputLedger().discards(first).empty());
        pane.sink.isReading = true;
        pane.source->drainInput(500);
        CHECK_STR_EQ(pane.sink.written, std::string("make\r"));
    }
}

// A paste is under way, its bracket open, when another client interrupts:
// the paste is given up, and the program reads the bracket's end and then
// the key in one write, so it is never left in paste mode. The paste's
// client is told, sends no more of it, and what it sends late is dropped.
TEST(anInterruptClosesAnOpenBracketInItsOwnWrite)
{
    SignalPane pane;
    pane.sink.isReading = true;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::optional<std::uint64_t> paste = a.client.sendPaste(a.handle, std::string(std::size_t { 1 } << 20, 'p'));
    REQUIRE(paste.has_value());
    a.pump(300);
    a.pump(301);
    REQUIRE(pane.sink.written.starts_with(bracketOpen));
    REQUIRE(a.client.isPasting(a.handle));
    pane.sink.isReading = false;
    a.pump(302);
    a.pump(303);
    REQUIRE(pane.source->hasPendingInput());
    const std::size_t writesBefore = pane.sink.writes.size();

    REQUIRE(pressInterrupt(b));
    b.pump(400);
    REQUIRE(pane.sink.writes.size() == writesBefore + 1);
    CHECK_STR_EQ(pane.sink.writes.back(), std::string(bracketClose) + "\x03");
    CHECK(!pane.source->hasPendingInput());
    CHECK(!pane.source->pasteWritten(first).has_value());
    for (std::uint64_t now = 30'000; now < 120'000; now += 30'000)
        a.pump(now);
    CHECK(!a.client.isPasting(a.handle));
    constexpr InputDiscarded whole { .inputWritten = 0, .throughSeq = 1 };
    CHECK(a.client.takeInputDiscards(a.handle) == std::vector<InputDiscarded> { whole });
    CHECK(pane.source->queueInput(first, *paste, PasteInput { .text = bytesOf("late"), .offset = std::size_t { 1 } << 20, .end = PasteEnd::Final }, 130'000) == EnqueueOutcome::Duplicate);
    // The bracket is closed once: a second interrupt writes the key alone.
    pane.sink.isReading = true;
    REQUIRE(pressInterrupt(b));
    b.pump(140'000);
    CHECK_STR_EQ(pane.sink.writes.back(), std::string("\x03"));
    CHECK_EQ(std::ranges::count(pane.sink.written, '\x03'), 2);
    CHECK(pane.sink.written.find("late") == std::string::npos);
    CHECK(a.host.state() == ConnectionState::Attached);
    // The paste that was given up left nothing behind: the next opens and
    // closes a bracket of its own.
    REQUIRE(a.client.sendPaste(a.handle, "again").has_value());
    for (std::uint64_t now = 150'000; now < 160'000; now += 1'000)
        a.pump(now);
    CHECK(pane.sink.written.ends_with(std::string("\x03") + std::string(bracketOpen) + "again" + std::string(bracketClose)));
}

// A program that asked for no bracketed paste has no bracket to close: the
// interrupt over its paste is written alone.
TEST(anInterruptOverABarePasteWritesNoEndMarker)
{
    SignalPane pane;
    UbiquittyTest::feed(pane.terminal, "\x1b[?2004l");
    pane.sink.isReading = true;
    CHECK(pane.source->queueInput(first, 1, PasteInput { .text = bytesOf("bare"), .offset = 0, .end = PasteEnd::More }, 1) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string("bare"));
    CHECK(pane.source->queueInput(second, 1, rawOf("\x03"), 2) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string("bare\x03"));
}

// An InputDiscarded ends the client's pastes it names and no other: not a
// paste on another pane, nor one numbered after the run, nor one before it.
TEST(aDiscardedRunEndsOnlyThePastesItNames)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = 32 * 1024;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    const std::uint32_t named = link.addPane();
    const std::uint32_t other = link.addPane();
    UbiquittyTest::RecordingWriter namedWriter;
    UbiquittyTest::RecordingWriter otherWriter;
    link.source(named).setInputWriter(namedWriter);
    link.source(other).setInputWriter(otherWriter);
    namedWriter.refuseNext(1 << 30);
    otherWriter.refuseNext(1 << 30);
    const std::string text(std::size_t { 1 } << 20, 'd');
    REQUIRE(link.client().sendPaste(named, text) == std::optional<std::uint64_t> { 1 });
    REQUIRE(link.client().sendPaste(named, text) == std::optional<std::uint64_t> { 2 });
    REQUIRE(link.client().sendPaste(named, text) == std::optional<std::uint64_t> { 3 });
    REQUIRE(link.client().sendPaste(other, text) == std::optional<std::uint64_t> { 1 });
    const auto receive = [&](std::uint64_t inputWritten, std::uint64_t throughSeq) {
        std::vector<std::uint8_t> body;
        encode(body, InputDiscarded { .inputWritten = inputWritten, .throughSeq = throughSeq });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::InputDiscarded, named, body));
        link.client().receiveStream(frame, link.now());
    };
    receive(0, 1);
    CHECK(link.client().isPasting(named));
    CHECK(link.client().isPasting(other));
    receive(2, 3);
    CHECK(link.client().isPasting(named));
    receive(1, 2);
    CHECK(!link.client().isPasting(named));
    CHECK(link.client().isPasting(other));
    CHECK(link.client().state() == ConnectionState::Attached);
}

// The interrupt's write is refused, the stream's own queue full: the bracket
// stays open and the key stays first, and the retry writes the end and the
// key once.
TEST(aRefusedInterruptStillClosesTheBracketWhenItIsTaken)
{
    SignalPane pane;
    pane.sink.isReading = true;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(a.client.sendPaste(a.handle, std::string(std::size_t { 1 } << 20, 'p')).has_value());
    a.pump(300);
    a.pump(301);
    REQUIRE(pane.sink.written.starts_with(bracketOpen));
    pane.sink.isReading = false;
    pane.sink.signalRefusals = 2;
    const std::size_t writesBefore = pane.sink.writes.size();
    REQUIRE(pressInterrupt(b));
    b.pump(400);
    CHECK_EQ(pane.sink.writes.size(), writesBefore);
    CHECK(pane.source->hasPendingInput());
    pane.source->drainInput(401);
    CHECK_EQ(pane.sink.writes.size(), writesBefore);
    pane.source->drainInput(402);
    REQUIRE(pane.sink.writes.size() == writesBefore + 1);
    CHECK_STR_EQ(pane.sink.writes.back(), std::string(bracketClose) + "\x03");
    CHECK(!pane.source->hasPendingInput());
    CHECK(pane.source->inputLedger().lastWritten(second) == std::optional<std::uint64_t> { 1 });
}

// A paste whose pieces so far were all written has nothing queued, and is
// in progress all the same: the interrupt ends it, its number is consumed,
// and its client is told.
TEST(anInterruptEndsAPasteWithNothingQueued)
{
    SignalPane pane;
    pane.sink.isReading = true;
    CHECK(pane.source->queueInput(first, 3, PasteInput { .text = bytesOf("head"), .offset = 0, .end = PasteEnd::More }, 1) == EnqueueOutcome::Queued);
    REQUIRE(!pane.source->hasPendingInput());
    REQUIRE(pane.source->pasteWritten(first).has_value());
    CHECK(pane.source->queueInput(second, 1, rawOf("\x03"), 2) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "head" + std::string(bracketClose) + "\x03");
    CHECK(!pane.source->pasteWritten(first).has_value());
    REQUIRE(pane.source->inputLedger().discards(first).size() == 1);
    CHECK_EQ(pane.source->inputLedger().discards(first).front().throughSeq, 3u);
    CHECK(pane.source->queueInput(first, 3, PasteInput { .text = bytesOf("tail"), .offset = 4, .end = PasteEnd::Final }, 3) == EnqueueOutcome::Duplicate);
    CHECK(pane.sink.written.find("tail") == std::string::npos);
}

// A terminal that keeps its input at a signal: the key goes at once though
// its own client's paste is under way, the bracket stays open, and the paste
// goes on to its end.
TEST(aSignalThatKeepsTheInputPassesItsOwnPasteAndLeavesItOpen)
{
    SignalPane pane;
    pane.sink.effect = SignalEffect::Signals;
    pane.sink.isReading = true;
    CHECK(pane.source->queueInput(first, 1, PasteInput { .text = bytesOf("head"), .offset = 0, .end = PasteEnd::More }, 1) == EnqueueOutcome::Queued);
    pane.sink.isReading = false;
    CHECK(pane.source->queueInput(first, 1, PasteInput { .text = bytesOf("body"), .offset = 4, .end = PasteEnd::More }, 2) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(first, 2, rawOf("\x03"), 3) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "head\x03");
    CHECK(pane.source->pasteWritten(first).has_value());
    pane.sink.isReading = true;
    CHECK(pane.source->queueInput(first, 1, PasteInput { .text = bytesOf("tail"), .offset = 8, .end = PasteEnd::Final }, 4) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "head\x03" + "bodytail" + std::string(bracketClose));
    CHECK(pane.source->inputLedger().lastWritten(first) == std::optional<std::uint64_t> { 2 });
    CHECK(pane.source->inputLedger().discards(first).empty());
}

// A signal the program has not taken yet stays first when another arrives
// and discards what waits: the second drops the line between them, never
// the first signal.
TEST(aSignalStillWaitingIsNotDroppedByTheNext)
{
    SignalPane pane;
    pane.sink.signalRefusals = 1 << 20;
    CHECK(pane.source->queueInput(first, 1, rawOf("\x1c\x03"), 1) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(first, 2, rawOf("typed"), 2) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(second, 1, rawOf("\x03"), 3) == EnqueueOutcome::Queued);
    CHECK(pane.sink.written.empty());
    pane.sink.signalRefusals = 0;
    pane.sink.isReading = true;
    pane.source->drainInput(4);
    CHECK_STR_EQ(pane.sink.written, std::string("\x1c\x03\x03"));
    REQUIRE(pane.source->inputLedger().discards(first).size() == 1);
    CHECK_EQ(pane.source->inputLedger().discards(first).front().inputWritten, 0u);
    CHECK_EQ(pane.source->inputLedger().discards(first).front().throughSeq, 2u);
    CHECK(pane.source->inputLedger().discards(second).empty());
}

// A paste none of which was written opened no bracket: the key is written
// alone.
TEST(anInterruptOverAPasteNotStartedWritesTheKeyAlone)
{
    SignalPane pane;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(a.client.sendPaste(a.handle, std::string(64 * 1024, 'p')).has_value());
    a.pump(300);
    a.pump(301);
    REQUIRE(pane.sink.written.empty());
    REQUIRE(pressInterrupt(b));
    b.pump(400);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    CHECK(!pane.source->hasPendingInput());
}

// 8 MiB pasted into a program that reads nothing, then Ctrl-C from the same
// person: the pane never holds more than the window and its slack, the key
// reaches the program on the pass it arrives, the paste is given up, and the
// client stays attached.
TEST(anInterruptAfterAnEightMebibytePasteIntoAProgramThatReadsNothing)
{
    SignalPane pane;
    UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::size_t window = measuredInputPolicy().pasteWindowBytes;
    REQUIRE(a.client.sendPaste(a.handle, std::string(std::size_t { 8 } << 20, 'p')).has_value());
    std::uint64_t now = 300;
    std::size_t mostHeld = 0;
    for (int rounds = 0; rounds < 400; ++rounds) {
        a.pump(++now);
        mostHeld = std::max(mostHeld, pane.source->heldInputBytes());
    }
    REQUIRE(mostHeld >= window);
    REQUIRE(a.client.isPasting(a.handle));
    REQUIRE(pane.sink.written.empty());

    REQUIRE(pressInterrupt(a));
    a.pump(++now);
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    for (int rounds = 0; rounds < 400; ++rounds) {
        a.pump(now += 1'000);
        mostHeld = std::max(mostHeld, pane.source->heldInputBytes());
    }
    CHECK(mostHeld <= window + 2 * clientPastePieceBytes);
    CHECK_EQ(pane.source->heldInputBytes(), 0u);
    CHECK(!a.client.isPasting(a.handle));
    CHECK_STR_EQ(pane.sink.written, std::string("\x03"));
    CHECK(a.host.state() == ConnectionState::Attached);
    CHECK(a.client.state() == ConnectionState::Attached);
    // The paste was the client's first event and the key its second.
    CHECK_EQ(a.client.receipt(a.handle).inputWritten, 2u);
}

// A client that binds a pane after a run of its instance's events was
// dropped is not told of it: the resume's count is what reports them.
TEST(aRunDroppedBeforeTheBindIsNotToldAgain)
{
    SignalPane pane;
    {
        UbiquittyTest::PaneConnection a(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        UbiquittyTest::PaneConnection b(*pane.source, second, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        REQUIRE(a.client.sendRaw(a.handle, bytesOf("lost")).has_value());
        a.pump(300);
        REQUIRE(pressInterrupt(b));
        b.pump(400);
        REQUIRE(pane.source->inputLedger().discards(first).size() == 1);
    }
    UbiquittyTest::PaneConnection again(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    for (std::uint64_t now = 30'000; now < 120'000; now += 30'000)
        again.pump(now);
    CHECK_EQ(again.host.counters().inputDiscardsSent, 0u);
    CHECK(again.client.takeInputDiscards(again.handle).empty());
}

// Signals a program is not taking are held to a bound of their own, past the
// pane's: an interrupt gets through a full pane, and a sender of nothing but
// interrupts is refused in the end.
TEST(aSignalIsTakenPastThePanesBoundAndSignalsAreBounded)
{
    PaneInputQueue queue;
    InputLedger ledger;
    std::uint64_t seq = 0;
    const std::string chunk(32 * 1024, 'f');
    while (queue.enqueue(ledger, first, ++seq, rawOf(chunk)) == EnqueueOutcome::Queued) {
    }
    REQUIRE(queue.heldBytes() + chunk.size() > maxHeldInputBytes);
    CHECK(queue.enqueueSignal(ledger, second, 1, rawOf("\x03"), SignalEffect::Signals, 1) == EnqueueOutcome::Queued);
    std::uint64_t signals = 1;
    while (queue.enqueueSignal(ledger, second, signals + 1, rawOf("\x03"), SignalEffect::Signals, 1) == EnqueueOutcome::Queued)
        ++signals;
    CHECK_EQ(signals, maxPendingSignalBytes);
    CHECK(queue.enqueueSignal(ledger, second, signals + 1, rawOf("\x03"), SignalEffect::SignalsAndDiscards, 1) == EnqueueOutcome::Refused);
}

// The ledger consumes a dropped run: nothing in it is admitted again, the
// instance's counts pass it in turn, and the run is remembered to be told.
TEST(theLedgerConsumesADroppedRun)
{
    InputLedger ledger;
    ledger.noteWritten(first, 2, 1'000);
    (void)ledger.takeRecentWriters();
    ledger.noteDiscarded(first, 5, 2'000);
    CHECK(ledger.admit(first, 5) == InputAdmission::Duplicate);
    CHECK(ledger.admit(first, 6) == InputAdmission::Write);
    CHECK(ledger.lastWritten(first) == std::optional<std::uint64_t> { 5 });
    // The program was written nothing: no frame is the instance's to settle.
    CHECK(ledger.takeRecentWriters().empty());
    REQUIRE(ledger.discards(first).size() == 1);
    CHECK_EQ(ledger.discards(first).front().inputWritten, 2u);
    CHECK_EQ(ledger.discards(first).front().throughSeq, 5u);
    CHECK_EQ(ledger.discards(first).front().serial, 1u);
    CHECK_EQ(ledger.discardSerial(), 1u);
    // A run at or below what the ledger holds records nothing.
    ledger.noteDiscarded(first, 5, 2'500);
    ledger.noteDiscarded(first, 3, 2'500);
    CHECK_EQ(ledger.discards(first).size(), 1u);
    // Settled in turn: no earlier than the write before it.
    CHECK_EQ(ledger.settledThrough(first), 0u);
    ledger.noteReadWouldBlock(1'500, 100);
    CHECK_EQ(ledger.settledThrough(first), 2u);
    ledger.noteReadWouldBlock(2'500, 100);
    CHECK_EQ(ledger.settledThrough(first), 5u);
    // An instance the ledger did not know is known by its dropped run.
    ledger.noteDiscarded(second, 4, 3'000);
    CHECK(ledger.admit(second, 4) == InputAdmission::Duplicate);
    CHECK_EQ(ledger.discards(second).front().inputWritten, 0u);
    CHECK_EQ(ledger.discards(second).front().serial, 2u);
    CHECK(ledger.discards(ID128 { 99, 99 }).empty());
}

// The runs remembered for an instance are bounded: past the bound the oldest
// two are told as one.
TEST(theRunsRememberedAreBounded)
{
    InputLedger ledger;
    for (std::uint64_t run = 1; run <= maxInputDiscardRecords + 1; ++run) {
        ledger.noteDiscarded(first, run * 10, run);
        ledger.noteWritten(first, run * 10 + 1, run);
    }
    const std::span<const InputDiscardRecord> runs = ledger.discards(first);
    REQUIRE(runs.size() == maxInputDiscardRecords);
    CHECK_EQ(runs.front().inputWritten, 0u);
    CHECK_EQ(runs.front().throughSeq, 20u);
    CHECK_EQ(runs.back().inputWritten, (maxInputDiscardRecords) * 10 + 1);
    CHECK_EQ(runs.back().throughSeq, (maxInputDiscardRecords + 1) * 10);
    CHECK_EQ(runs.back().serial, maxInputDiscardRecords + 1);
}

// An InputDiscarded round-trips; one whose run is empty is not one; the kind
// travels downward only, and one for a pane the client has not bound, or one
// it cannot read, closes the client.
TEST(anInputDiscardedTravelsDownwardOnly)
{
    constexpr InputDiscarded run { .inputWritten = 7, .throughSeq = 12 };
    std::vector<std::uint8_t> body;
    encode(body, run);
    CHECK_EQ(body.size(), 16u);
    CHECK(decodeInputDiscarded(body) == std::optional { run });
    CHECK(!decodeInputDiscarded(std::span { body }.first(15)).has_value());
    std::vector<std::uint8_t> empty;
    encode(empty, InputDiscarded { .inputWritten = 7, .throughSeq = 7 });
    CHECK(!decodeInputDiscarded(empty).has_value());
    std::vector<std::uint8_t> backward;
    encode(backward, InputDiscarded { .inputWritten = 7, .throughSeq = 6 });
    CHECK(!decodeInputDiscarded(backward).has_value());

    SignalPane pane;
    {
        UbiquittyTest::PaneConnection link(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::InputDiscarded, link.handle, body));
        link.host.receiveStream(frame, 300);
        CHECK(link.host.state() == ConnectionState::Closed);
        CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
    }
    {
        UbiquittyTest::PaneConnection link(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::InputDiscarded, 77, body));
        link.client.receiveStream(frame, 300);
        CHECK(link.client.state() == ConnectionState::Closed);
    }
    {
        UbiquittyTest::PaneConnection link(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::InputDiscarded, link.handle, empty));
        link.client.receiveStream(frame, 300);
        CHECK(link.client.state() == ConnectionState::Closed);
    }
}

// The runs a client holds untaken are bounded as the host's are: past the
// bound the oldest two are held as one.
TEST(theRunsAClientHoldsAreBounded)
{
    SignalPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, first, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    for (std::uint64_t run = 1; run <= maxInputDiscardRecords + 1; ++run) {
        std::vector<std::uint8_t> body;
        encode(body, InputDiscarded { .inputWritten = run * 10, .throughSeq = run * 10 + 5 });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::InputDiscarded, link.handle, body));
        link.client.receiveStream(frame, 300 + run);
    }
    REQUIRE(link.client.state() == ConnectionState::Attached);
    const std::vector<InputDiscarded> runs = link.client.takeInputDiscards(link.handle);
    REQUIRE(runs.size() == maxInputDiscardRecords);
    constexpr InputDiscarded merged { .inputWritten = 10, .throughSeq = 25 };
    CHECK(runs.front() == merged);
    CHECK_EQ(runs.back().throughSeq, (maxInputDiscardRecords + 1) * 10 + 5);
}

// What the encoder asks the terminal is the bytes the key encodes to under
// the live modes: Ctrl-C reported as an escape sequence is data, a paste's
// text signals nothing, and so does an event that encodes to nothing.
TEST(theEffectIsAskedOfTheBytesTheKeyEncodesTo)
{
    SignalPane pane;
    CHECK(pane.encoder->signalEffect(KeyInput { .events = { controlKey('c') } }) == SignalEffect::SignalsAndDiscards);
    CHECK(pane.encoder->signalEffect(RawInput { .bytes = { 'a', interruptByte } }) == SignalEffect::SignalsAndDiscards);
    CHECK(pane.encoder->signalEffect(KeyInput { .events = { controlKey('d') } }) == SignalEffect::None);
    CHECK(pane.encoder->signalEffect(PasteInput { .text = { interruptByte }, .offset = 0, .end = PasteEnd::Final }) == SignalEffect::None);
    CHECK(pane.encoder->signalEffect(FocusInput { .focus = Ubiquitty::Modes::FocusState::Focused }) == SignalEffect::None);
    // The program asked for every key as an escape sequence: Ctrl-C is one.
    UbiquittyTest::feed(pane.terminal, "\x1b[>8u");
    CHECK(pane.encoder->signalEffect(KeyInput { .events = { controlKey('c') } }) == SignalEffect::None);
    pane.sink.isReading = true;
    CHECK(pane.source->queueInput(first, 1, KeyInput { .events = { controlKey('c') } }, 1) == EnqueueOutcome::Queued);
    CHECK(pane.sink.written.find('\x03') == std::string::npos);
    CHECK(!pane.sink.written.empty());
}

// The attachment counts a dropped run's events as discarded and holds them
// no longer, whether the run arrives before the count that passes them or
// after it, and a run told twice counts once.
TEST(theAttachmentCountsADroppedRunOnceInEitherOrder)
{
    using namespace UbiquittyTest::AttachmentLink;
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    host.pane.writer.refuseNext(1 << 30);
    CHECK(client.send(paneID, raw("a"), now).has_value());
    CHECK(client.send(paneID, raw("b"), now).has_value());
    pumpFor(host, client, now, 50'000);
    REQUIRE(client.pendingCount(paneID) == 2);
    // The count that passes them arrives first: the events are held no
    // longer, and nothing says yet what became of them.
    REQUIRE(client.connection() && client.connection()->boundPanes().size() == 1);
    const std::uint32_t handle = client.connection()->boundPanes().front().first;
    const auto deliver = [&](FrameKind kind, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, handle, body));
        client.receiveStream(frame, now);
        client.tick(now);
    };
    std::vector<std::uint8_t> receipt;
    encode(receipt, Receipt { .inputWritten = 2, .inputSettled = 0 });
    deliver(FrameKind::Receipt, receipt);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK_EQ(client.inputStatus(paneID).discarded, 0u);
    std::vector<std::uint8_t> run;
    encode(run, InputDiscarded { .inputWritten = 0, .throughSeq = 2 });
    deliver(FrameKind::InputDiscarded, run);
    CHECK_EQ(client.inputStatus(paneID).discarded, 2u);
    deliver(FrameKind::InputDiscarded, run);
    CHECK_EQ(client.inputStatus(paneID).discarded, 2u);
    // The run first: the events it names go, and the ones after it stay.
    CHECK(client.send(paneID, raw("c"), now).has_value());
    CHECK(client.send(paneID, raw("d"), now).has_value());
    CHECK(client.send(paneID, raw("e"), now).has_value());
    REQUIRE(client.pendingCount(paneID) == 3);
    std::vector<std::uint8_t> later;
    encode(later, InputDiscarded { .inputWritten = 2, .throughSeq = 4 });
    deliver(FrameKind::InputDiscarded, later);
    CHECK_EQ(client.inputStatus(paneID).discarded, 4u);
    CHECK_EQ(client.pendingCount(paneID), 1u);
    // A run overlapping one counted counts only what is new.
    std::vector<std::uint8_t> overlapping;
    encode(overlapping, InputDiscarded { .inputWritten = 3, .throughSeq = 5 });
    deliver(FrameKind::InputDiscarded, overlapping);
    CHECK_EQ(client.inputStatus(paneID).discarded, 5u);
    CHECK_EQ(client.pendingCount(paneID), 0u);
    CHECK(!client.inputStatus(paneID).isAwaitingDecision());
}

UBIQUITTY_TEST_MAIN()
