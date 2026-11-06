// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_input — a pane's input on the host (Docs/sync.md §6.7): each
// client's events written once and in order, what was written remembered per
// client instance for the pane's lifetime, and reported back on every update
// or, with none to carry it, on a standalone `Receipt`.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::DriveScope;
using UbiquittyTest::PaneConnection;
using UbiquittyTest::RecordingWriter;
using UbiquittyTest::SyncLink;

namespace {

std::span<const std::uint8_t> bytesOf(std::string_view text)
{
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

/// Whether a terminal's first row holds `text`.
bool firstRowHolds(const Ubiquitty::Terminal& terminal, std::string_view text)
{
    std::string row;
    for (int column = 0; column < terminal.columns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, 0, column);
        row.push_back(cell ? static_cast<char>(cell->codePoint & 0x7F) : ' ');
    }
    return row.find(text) != std::string::npos;
}

} // namespace

// A client resends after a drop what it cannot know was written. The host
// writes only what its ledger has not seen for that instance, so a second
// connection continues the first one's numbering on the pane.
TEST(aRawAtOrBelowLastWrittenIsNotWrittenAgain)
{
    UbiquittyTest::OwnedPane pane;
    {
        PaneConnection first(*pane.source, { 7, 8 }, ClientKind::Human, DriveScope::Granted);
        CHECK(first.client.sendRaw(first.handle, bytesOf("a")) == std::optional<std::uint64_t> { 1 });
        first.pump(300);
    }
    PaneConnection second(*pane.source, { 7, 8 }, ClientKind::Human, DriveScope::Granted);
    // The new connection numbers from 1 again, as a client that lost its
    // count would; the host writes only what is new.
    CHECK(second.client.sendRaw(second.handle, bytesOf("a")) == std::optional<std::uint64_t> { 1 });
    CHECK(second.client.sendRaw(second.handle, bytesOf("b")) == std::optional<std::uint64_t> { 2 });
    second.pump(400);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("ab"));
    CHECK(pane.source->inputLedger().lastWritten({ 7, 8 }) == std::optional<std::uint64_t> { 2 });
}

// An instance the ledger forgot reads absent, never 0: a 0 would tell its
// client that nothing it sent was written.
TEST(anEvictedInstanceReadsAbsentNeverZero)
{
    InputLedger ledger;
    for (std::uint64_t instance = 1; instance <= maxLastWrittenEntries + 1; ++instance)
        ledger.noteWritten({ instance, 0 }, 5, instance);
    CHECK(!ledger.lastWritten({ 1, 0 }).has_value());
    CHECK(ledger.lastWritten({ 2, 0 }) == std::optional<std::uint64_t> { 5 });
    CHECK(ledger.lastWritten({ maxLastWrittenEntries + 1, 0 }) == std::optional<std::uint64_t> { 5 });
    CHECK(!ledger.lastWritten({ 99, 99 }).has_value());
    // A write refreshes an instance, so the least recently written goes.
    ledger.noteWritten({ 2, 0 }, 6, 100);
    ledger.noteWritten({ 200, 0 }, 1, 101);
    CHECK(ledger.lastWritten({ 2, 0 }) == std::optional<std::uint64_t> { 6 });
    CHECK(!ledger.lastWritten({ 3, 0 }).has_value());
}

// Two clients drive one pane; each update tells each only of its own input.
TEST(anUpdateCarriesTheClientsOwnInputWritten)
{
    UbiquittyTest::OwnedPane pane;
    PaneConnection first(*pane.source, { 7, 8 }, ClientKind::Human, DriveScope::Granted);
    PaneConnection second(*pane.source, { 11, 12 }, ClientKind::Human, DriveScope::Granted);
    (void)first.client.sendRaw(first.handle, bytesOf("a"));
    (void)first.client.sendRaw(first.handle, bytesOf("b"));
    (void)first.client.sendRaw(first.handle, bytesOf("c"));
    first.pump(300);
    (void)second.client.sendRaw(second.handle, bytesOf("x"));
    second.pump(300);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("abcx"));
    UbiquittyTest::feed(pane.terminal, "abcx");
    pane.source->publish(FrameClass::Settled, 301);
    // The update leaves once the pane's rate allows, inside the receipt delay.
    for (std::uint64_t at = 302; at < 20'000 && first.host.counters().chainUpdatesSent == 0; at += 1'000) {
        first.pump(at);
        second.pump(at);
    }
    CHECK_EQ(first.host.counters().chainUpdatesSent, 1u);
    CHECK_EQ(first.client.receipt(first.handle).inputWritten, 3u);
    CHECK_EQ(second.client.receipt(second.handle).inputWritten, 1u);
    // Updates carried both; neither owed a standalone receipt.
    CHECK_EQ(first.host.counters().receiptsSent, 0u);
    CHECK_EQ(second.host.counters().receiptsSent, 0u);
}

// Input the client sent after a call waits until the owner has served the
// call: a call can retire the pane the input names (workspace.closePane), and
// the stream's order is the client's. What follows no call is written at once.
TEST(inputSentAfterACallIsWrittenOnlyOnceTheCallIsServed)
{
    SyncLink link(0, 24);
    link.settleAttach();
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    REQUIRE(link.client().call(R"({"jsonrpc":"2.0","id":1,"method":"host.describe","params":{}})"));
    CHECK(link.client().sendRaw(link.handle(), bytesOf("q")).has_value());
    link.pump();
    CHECK_STR_EQ(writer.bytes(), std::string());
    std::string writtenWhileServing = "unserved";
    serveCallsDeferred(link.host(), [&](std::uint32_t, const Ubiquitty::Protocol::Control::DecodedRequest& request) -> std::optional<Ubiquitty::Codec::JSON::Value> {
        writtenWhileServing = writer.bytes();
        return Ubiquitty::Protocol::Control::rpcResult(request.idOrNull(), Ubiquitty::Codec::JSON::Value::object());
    });
    CHECK_STR_EQ(writtenWhileServing, std::string());
    CHECK_STR_EQ(writer.bytes(), std::string("q"));
    CHECK(link.client().sendRaw(link.handle(), bytesOf("r")).has_value());
    link.pump();
    CHECK_STR_EQ(writer.bytes(), std::string("qr"));
}

// Input whose program printed nothing is reported by one standalone Receipt
// once the receipt delay passes, and not before.
TEST(aWriteWithNoUpdateWithinTheReceiptDelayGetsOneStandaloneReceipt)
{
    SyncLink link(0, 24);
    // The update that ends the attach would carry the receipt.
    link.settleAttach();
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    const std::uint64_t sentAt = link.now();
    CHECK(link.client().sendRaw(link.handle(), bytesOf("q")).has_value());
    link.pump();
    CHECK_STR_EQ(writer.bytes(), std::string("q"));
    const std::uint64_t delay = measuredInputPolicy().receiptDelayMicroseconds;
    while (link.now() < sentAt + delay - UbiquittyTest::syncFrameInterval) {
        link.step();
        CHECK_EQ(link.host().counters().receiptsSent, 0u);
    }
    for (int steps = 0; steps < 8; ++steps)
        link.step();
    CHECK_EQ(link.host().counters().receiptsSent, 1u);
    CHECK_EQ(link.client().receipt(link.handle()).inputWritten, 1u);
    // Nothing more is owed.
    for (int steps = 0; steps < 8; ++steps)
        link.step();
    CHECK_EQ(link.host().counters().receiptsSent, 1u);
    // The host's next deadline included the receipt's.
    CHECK(link.host().nextServiceMicroseconds(link.now()).has_value());
}

// A program not reading leaves the write at the head of the queue; it is
// written once, in order, when the program reads again.
TEST(aRefusedWriteStaysQueuedAndIsWrittenOnce)
{
    SyncLink link(0, 24);
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1'000);
    (void)link.client().sendRaw(link.handle(), bytesOf("1"));
    (void)link.client().sendRaw(link.handle(), bytesOf("2"));
    link.pump();
    CHECK_STR_EQ(writer.bytes(), std::string());
    CHECK(link.source(link.handle()).hasPendingInput());
    writer.refuseNext(0);
    link.source(link.handle()).drainInput(link.now());
    CHECK_STR_EQ(writer.bytes(), std::string("12"));
    link.source(link.handle()).drainInput(link.now());
    CHECK_STR_EQ(writer.bytes(), std::string("12"));
    CHECK(!link.source(link.handle()).hasPendingInput());
}

// What someone typed is counted apart from what the queue writes on its own
// account: a host that stamps "typed at" reads the count, and a pointer moving
// over a pane or a window gaining focus is nobody typing.
TEST(typedInputIsCountedAndPointerAndFocusAreNot)
{
    SyncLink link(0, 24);
    RecordingWriter writer;
    PaneSource& source = link.source(link.handle());
    source.setInputWriter(writer);
    CHECK(source.typedInputWrites() == 0);

    (void)link.client().sendRaw(link.handle(), bytesOf("a"));
    link.pump();
    CHECK_MSG(source.typedInputWrites() == 1, "raw bytes are typed");
    const std::array<KeyEvent, 1> key { KeyEvent {} };
    (void)link.client().sendKeys(link.handle(), key);
    link.pump();
    CHECK_MSG(source.typedInputWrites() == 2, "keys are typed");
    CHECK(link.client().sendPaste(link.handle(), "pasted").has_value());
    link.pump();
    CHECK_MSG(source.typedInputWrites() == 3, "a paste is typed, once for the whole paste");

    const std::uint64_t typed = source.typedInputWrites();
    const std::array<PointerEvent, 1> motion { PointerEvent {} };
    (void)link.client().sendPointer(link.handle(), motion);
    (void)link.client().sendFocus(link.handle(), Ubiquitty::Modes::FocusState::Focused);
    link.pump();
    CHECK_MSG(source.typedInputWrites() == typed, "pointer motion and a focus change are not typing");

    // Refused, nothing was typed into the program yet; written later, it was.
    writer.refuseNext(1'000);
    (void)link.client().sendRaw(link.handle(), bytesOf("b"));
    link.pump();
    CHECK(source.typedInputWrites() == typed);
    writer.refuseNext(0);
    source.drainInput(link.now());
    CHECK(source.typedInputWrites() == typed + 1);
}

// A pane takes input a program is not reading up to its bound, and an event
// past the bound closes the connection rather than being dropped. Its
// sender is never held back unread: an interrupt behind it would not be seen.
TEST(aPaneTakesInputToItsBoundAndAnEventPastItCloses)
{
    SyncLink link(0, 24);
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1'000'000);
    const std::string chunk(32 * 1024, 'p');
    std::size_t sent = 0;
    for (; sent < maxPendingInputBytes; sent += chunk.size())
        (void)link.client().sendRaw(link.handle(), bytesOf(chunk));
    link.pump();
    CHECK(link.source(link.handle()).heldInputBytes() >= maxPendingInputBytes);
    CHECK(link.host().state() == ConnectionState::Attached);
    for (; sent + chunk.size() <= maxHeldInputBytes; sent += chunk.size())
        (void)link.client().sendRaw(link.handle(), bytesOf(chunk));
    link.pump();
    CHECK(link.host().state() == ConnectionState::Attached);
    (void)link.client().sendRaw(link.handle(), bytesOf("x"));
    link.pump();
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::Oversize);
}

// A Raw naming a handle the host never bound closes the connection.
TEST(aRawOnAnUnboundHandleCloses)
{
    SyncLink link(0, 24);
    std::vector<std::uint8_t> body;
    encode(body, Raw { .inputSeq = 1, .bytes = bytesOf("x") });
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Raw, 77, body));
    link.host().receiveStream(frame, link.now());
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::UnboundHandle);
}

// Input travels only upward: a Raw from the host closes the client.
TEST(aRawFromTheHostIsMalformed)
{
    SyncLink link(0, 24);
    std::vector<std::uint8_t> body;
    encode(body, Raw { .inputSeq = 1, .bytes = bytesOf("x") });
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Raw, link.handle(), body));
    link.client().receiveStream(frame, link.now());
    CHECK(link.client().state() == ConnectionState::Closed);
    CHECK(link.client().byeReason() == ByeReason::Malformed);
    // And a Raw numbered 0 is no event at all.
    body.clear();
    encode(body, Raw { .inputSeq = 0, .bytes = bytesOf("x") });
    CHECK(!decodeRaw(body).has_value());
}

// A Raw with no bytes is no event: the decoder refuses it, a host closes the
// sender that frames one as malformed, and a client asked to send one sends
// nothing.
TEST(anEmptyRawIsNoEvent)
{
    std::vector<std::uint8_t> body;
    encode(body, Raw { .inputSeq = 1, .bytes = {} });
    CHECK(!decodeRaw(body).has_value());

    SyncLink framed(0, 24);
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, FrameKind::Raw, framed.handle(), body));
    framed.host().receiveStream(frame, framed.now());
    CHECK(framed.host().state() == ConnectionState::Closed);
    CHECK(framed.host().byeReason() == ByeReason::Malformed);

    SyncLink quiet(0, 24);
    CHECK(!quiet.client().sendRaw(quiet.handle(), {}).has_value());
    quiet.pump();
    CHECK(quiet.host().state() == ConnectionState::Attached);
    CHECK_EQ(quiet.source(quiet.handle()).heldInputBytes(), 0u);
}

// The bound on what a pane holds is in events as well as bytes: a sender
// whose events cost next to nothing cannot hold the pane behind thousands of
// them while the program reads none.
TEST(aPaneHoldsABoundedNumberOfEventsHoweverSmall)
{
    SyncLink link(0, 24);
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1'000'000);
    for (std::size_t sent = 0; sent < maxHeldInputEvents; ++sent)
        (void)link.client().sendRaw(link.handle(), bytesOf("x"));
    for (int pass = 0; pass < 64 && link.source(link.handle()).heldInputBytes() < maxHeldInputEvents; ++pass)
        link.pump();
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK_EQ(link.source(link.handle()).heldInputBytes(), maxHeldInputEvents);
    (void)link.client().sendRaw(link.handle(), bytesOf("x"));
    link.pump();
    CHECK(link.host().state() == ConnectionState::Closed);
    CHECK(link.host().byeReason() == ByeReason::Oversize);
}

// Only a driver with the Drive scope writes to the pane: input from a viewer
// or a client without that scope closes the connection and writes nothing.
TEST(aViewerMayNotSendRaw)
{
    for (const bool isViewer : { true, false }) {
        UbiquittyTest::OwnedPane pane;
        PaneConnection connection(*pane.source, { 7, 8 }, isViewer ? ClientKind::Viewer : ClientKind::Human, isViewer ? DriveScope::Granted : DriveScope::Withheld);
        // The client sends regardless; the host is what refuses.
        (void)connection.client.sendRaw(connection.handle, bytesOf("rm -rf ~\r"));
        connection.pump(300);
        CHECK(connection.host.state() == ConnectionState::Closed);
        CHECK(connection.host.byeReason() == ByeReason::Malformed);
        CHECK_STR_EQ(pane.writer.bytes(), std::string());
    }
}

// A program echoes 30 ms after it reads a key; every frame the client holds
// that reports the key settled holds its echo, because a write settles only
// once the program had E to echo it before a read that found nothing.
TEST(aFrameWithSettledInputHoldsItsEcho)
{
    for (int round = 0; round < 3; ++round) {
        SyncLink link(0, 24, 80, "");
        RecordingWriter writer;
        link.source(link.handle()).setInputWriter(writer);
        for (int step = 0; step < round * 3; ++step)
            link.step();
        CHECK(link.client().sendRaw(link.handle(), bytesOf("Z")).has_value());
        link.pump();
        const std::uint64_t writtenAt = link.now();
        bool hasEchoed = false;
        bool hasSettled = false;
        for (int step = 0; step < 40; ++step) {
            // The program writes its echo once 30 ms have passed since the
            // write; each step reads it, and a step with nothing to read is a
            // read that found nothing, begun at the step's start.
            const std::uint64_t readBegan = link.now();
            if (!hasEchoed && readBegan >= writtenAt + 30'000) {
                link.write("Z");
                hasEchoed = true;
            } else {
                link.source(link.handle()).noteReadWouldBlock(readBegan);
            }
            link.step();
            const Ubiquitty::Terminal* replica = link.client().paneTerminal(link.handle());
            REQUIRE(replica != nullptr);
            if (link.client().receipt(link.handle()).inputSettled >= 1) {
                hasSettled = true;
                CHECK_MSG(firstRowHolds(*replica, "Z"), "a frame reported the key settled without its echo");
            }
        }
        CHECK(hasSettled);
    }
}

// A write settled after the terminal moved on from the frame the client was
// sent is not reported by a standalone Receipt; the update that carries the
// newer picture reports it.
TEST(aStandaloneReceiptNeverSettlesPastTheSentGeneration)
{
    SyncLink link(0, 24, 80, "");
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    CHECK(link.client().sendRaw(link.handle(), bytesOf("Z")).has_value());
    link.pump();
    // The echo lands in the terminal, but no frame of it has been published.
    link.writeUnpublished("Z");
    const std::uint64_t echoSettle = measuredInputPolicy().echoSettleMicroseconds;
    link.advance(echoSettle + 1'000);
    link.source(link.handle()).noteReadWouldBlock(link.now());
    for (int step = 0; step < 4; ++step)
        link.step();
    CHECK(link.host().counters().receiptsSent >= 1);
    CHECK_EQ(link.client().receipt(link.handle()).inputWritten, 1u);
    CHECK_EQ(link.client().receipt(link.handle()).inputSettled, 0u);
    // The frame that holds the echo reports it settled.
    link.publish();
    for (int step = 0; step < 4; ++step)
        link.step();
    CHECK_EQ(link.client().receipt(link.handle()).inputSettled, 1u);
    CHECK(firstRowHolds(*link.client().paneTerminal(link.handle()), "Z"));
}

// The read that settles a write is one that began at least E after it; a
// read that began earlier settles nothing however late it is noted, so the
// time compared is the read's start, never its end.
TEST(aWouldBlockReadBegunBeforeWritePlusENeverSettles)
{
    InputLedger ledger;
    ledger.noteWritten({ 7, 8 }, 1, 100'000);
    ledger.noteReadWouldBlock(149'999, 50'000);
    CHECK_EQ(ledger.settledThrough({ 7, 8 }), 0u);
    CHECK(ledger.nextSettleMicroseconds(50'000) == std::optional<std::uint64_t> { 150'000 });
    ledger.noteReadWouldBlock(150'000, 50'000);
    CHECK_EQ(ledger.settledThrough({ 7, 8 }), 1u);
    CHECK(!ledger.nextSettleMicroseconds(50'000).has_value());
}

// Settled follows written and never passes it, whatever reads arrive.
TEST(settledNeverPassesWritten)
{
    InputLedger ledger;
    std::uint64_t now = 0;
    std::uint64_t seq = 0;
    for (int index = 0; index < 1'000; ++index) {
        now += 1'000 + static_cast<std::uint64_t>((index * 7'919) % 40'000);
        if (index % 3 != 2)
            ledger.noteWritten({ 7, 8 }, ++seq, now);
        if (index % 5 == 0)
            ledger.noteReadWouldBlock(now - static_cast<std::uint64_t>((index * 104'729) % 60'000), 50'000);
        CHECK(ledger.settledThrough({ 7, 8 }) <= ledger.lastWritten({ 7, 8 }).value_or(0));
    }
    // More than the ledger keeps unsettled merge, and every one still settles.
    ledger.noteReadWouldBlock(now + 50'000, 50'000);
    CHECK_EQ(ledger.settledThrough({ 7, 8 }), seq);
}

// A program that echoes nothing has its write settled by the timer: the
// client is told once, by a standalone Receipt, and then nothing more.
TEST(aQuietPaneSettlesByTheTimerAndSendsOneStandaloneReceipt)
{
    SyncLink link(0, 24);
    RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    CHECK(link.client().sendRaw(link.handle(), bytesOf("q")).has_value());
    link.pump();
    const std::optional<std::uint64_t> due = link.source(link.handle()).nextSettleMicroseconds();
    REQUIRE(due.has_value());
    while (link.now() < *due)
        link.step();
    // The owner finds the program quiet at the deadline.
    link.source(link.handle()).noteReadWouldBlock(link.now());
    const std::uint64_t receiptsBefore = link.host().counters().receiptsSent;
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK_EQ(link.host().counters().receiptsSent, receiptsBefore + 1);
    CHECK_EQ(link.client().receipt(link.handle()).inputSettled, 1u);
    CHECK(!link.source(link.handle()).nextSettleMicroseconds().has_value());
}

UBIQUITTY_TEST_MAIN()
