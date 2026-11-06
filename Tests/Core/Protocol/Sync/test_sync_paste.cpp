// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_paste — a paste on the upward bulk lane (Docs/sync.md §6.7):
// written once under the bracket and the paste normaliser whatever its pieces,
// never ahead of a key typed after it, never closing its bracket early, given
// up at a PasteAbort, and closed when its client is gone.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Input/PasteNormalize.h"
#include "Core/Keys/Keymap.h"
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
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

/// A program's input, as the encoder wrote it.
class RecordingSink final : public Ubiquitty::Runtime::ByteSink {
public:
    [[nodiscard]] WriteStatus writeAll(ID128, std::span<const std::uint8_t> bytes) override
    {
        if (refusals > 0) {
            --refusals;
            return WriteStatus::Refused;
        }
        written.append(bytes.begin(), bytes.end());
        if (clock)
            times.insert(times.end(), bytes.size(), *clock);
        return WriteStatus::Written;
    }

    /// When the last byte of the first `text` written was written, when the
    /// sink keeps a clock.
    [[nodiscard]] std::optional<std::uint64_t> timeOf(std::string_view text) const
    {
        const std::size_t at = written.find(text);
        if (at == std::string::npos || text.empty() || at + text.size() > times.size())
            return std::nullopt;
        return times[at + text.size() - 1];
    }

    std::string written;
    int refusals = 0;
    /// The time, read at each write, or null for none kept.
    const std::uint64_t* clock = nullptr;
    std::vector<std::uint64_t> times;
};

/// A pane whose input goes through the host's encoder, as the daemon's does.
struct EncodedPane {
    Ubiquitty::Terminal terminal;
    std::unique_ptr<PaneSource> source;
    RecordingSink sink;
    std::unique_ptr<Ubiquitty::Runtime::RemoteInputEncoder> encoder;

    /// Creates the pane, its program asking for bracketed paste or not.
    explicit EncodedPane(Ubiquitty::Input::BracketedPaste bracketed = Ubiquitty::Input::BracketedPaste::Enabled)
    {
        Ubiquitty::resize(terminal, 24, 80);
        source = std::make_unique<PaneSource>(terminal, ID128 { 9, 10 });
        UbiquittyTest::feed(terminal, bracketed == Ubiquitty::Input::BracketedPaste::Enabled ? "\x1b[?2004h$ " : "$ ");
        source->publish(FrameClass::Structural, 1);
        encoder = std::make_unique<Ubiquitty::Runtime::RemoteInputEncoder>(terminal, sink);
        source->setInputWriter(*encoder);
    }
};

constexpr ID128 client { 7, 8 };
constexpr std::string_view bracketOpen = "\x1b[200~";
constexpr std::string_view bracketClose = "\x1b[201~";

std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

PasteInput piece(std::string_view text, std::uint64_t offset, PasteEnd end = PasteEnd::More)
{
    return { .text = bytesOf(text), .offset = offset, .end = end };
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

std::string repeated(char letter, std::size_t count)
{
    return std::string(count, letter);
}

/// A connection over a narrow uplink, 1 Mbit/s each way and 50 ms long: the
/// client is handed the transport only when what it gave before has left, as
/// a socket's send buffer drains.
struct NarrowLink {
    static constexpr std::uint64_t microsecondsPerByte = 8;
    static constexpr std::uint64_t oneWayMicroseconds = 50'000;
    static constexpr std::uint64_t roundTripMicroseconds = 2 * oneWayMicroseconds;

    struct InFlight {
        std::uint64_t arrivesAt = 0;
        std::vector<std::uint8_t> bytes;
    };

    UbiquittyTest::PaneConnection& link;
    std::uint64_t upDrainedAt = 0;
    std::deque<InFlight> up {};
    std::deque<InFlight> down {};

    /// Runs both ends once and moves what the link carries by then.
    ///
    /// - Parameter now: The time.
    /// - Returns: Whether the client handed the uplink bytes.
    bool step(std::uint64_t now)
    {
        link.host.service(now);
        std::vector<std::uint8_t> stream;
        while (link.host.popStreamFrame(stream, now)) {
        }
        if (!stream.empty())
            down.push_back({ .arrivesAt = now + oneWayMicroseconds, .bytes = std::move(stream) });
        while (!down.empty() && down.front().arrivesAt <= now) {
            link.client.receiveStream(down.front().bytes, now);
            down.pop_front();
        }
        link.client.tick(now);
        bool isHanded = false;
        if (now >= upDrainedAt) {
            std::vector<std::uint8_t> output = link.client.takeStreamOutput();
            if (!output.empty()) {
                upDrainedAt = now + output.size() * microsecondsPerByte;
                up.push_back({ .arrivesAt = upDrainedAt + oneWayMicroseconds, .bytes = std::move(output) });
                isHanded = true;
            }
        }
        while (!up.empty() && up.front().arrivesAt <= now) {
            link.host.receiveStream(up.front().bytes, now);
            up.pop_front();
        }
        return isHanded;
    }
};

/// Pastes 8 MiB over the narrow link and lets it flow for a second, until a
/// piece has just left: the moment a key waits longest behind one.
///
/// - Parameters:
///   - link: The link.
///   - now: The time, moved on.
/// - Returns: Whether the paste was taken.
[[nodiscard]] bool pasteEightMebibytes(NarrowLink& link, std::uint64_t& now)
{
    if (!link.link.client.sendPaste(link.link.handle, repeated('m', std::size_t { 8 } << 20)))
        return false;
    for (now = 1'000;; now += 1'000) {
        const bool isHanded = link.step(now);
        if (now >= 1'000'000 && isHanded)
            return true;
    }
}

/// Ctrl with a letter, as a platform reports it.
KeyEvent controlKey(char letter)
{
    return { .key = 0, .modifiers = Ubiquitty::Keys::Keymap::keymapModifierCtrl, .kind = KeyEventKind::Press, .text = {}, .unshiftedCodePoint = static_cast<std::uint32_t>(letter), .shiftedCodePoint = static_cast<std::uint32_t>(letter), .baseLayoutCodePoint = 0, .wasModified = true };
}

} // namespace

// The paste's pieces arrive; the client gives it up and presses Ctrl-C. The
// program reads the bracket's end, then the key.
TEST(anAbortWritesTheEndMarkerThenTheKey)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(link.client.sendPaste(link.handle, repeated('a', 100 * 1024)).has_value());
    link.pump(300);
    CHECK(link.client.isPasting(link.handle));
    REQUIRE(link.client.abortPaste(link.handle).has_value());
    const std::array<std::uint8_t, 1> interrupt { 0x03 };
    REQUIRE(link.client.sendRaw(link.handle, interrupt).has_value());
    for (std::uint64_t now = 301; now < 320; ++now)
        link.pump(now);
    CHECK(pane.sink.written.ends_with(std::string(bracketClose) + "\x03"));
    CHECK(pane.sink.written.starts_with(bracketOpen));
    CHECK(pane.sink.written.size() < 100 * 1024);
    CHECK(link.host.state() == ConnectionState::Attached);
}

// 8 MiB of paste on a 1 Mbit/s uplink, then Ctrl-C: the interrupt reaches the
// program within two round trips, right after the bracket's end.
TEST(anInterruptAfterEightMebibytesArrivesWithinTwoRoundTrips)
{
    EncodedPane pane;
    std::uint64_t now = 0;
    pane.sink.clock = &now;
    UbiquittyTest::PaneConnection connection(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    NarrowLink link { .link = connection };
    REQUIRE(pasteEightMebibytes(link, now));
    REQUIRE(pane.sink.written.starts_with(bracketOpen));
    const std::uint64_t keyTime = now;
    const std::array<std::uint8_t, 1> interrupt { 0x03 };
    REQUIRE(connection.client.sendRaw(connection.handle, interrupt).has_value());
    for (; now < keyTime + 1'000'000; now += 1'000)
        (void)link.step(now);
    const std::optional<std::uint64_t> interruptAt = pane.sink.timeOf("\x03");
    REQUIRE(interruptAt.has_value());
    CHECK(*interruptAt - keyTime <= 2 * NarrowLink::roundTripMicroseconds);
    CHECK(pane.sink.written.ends_with(std::string(bracketClose) + "\x03"));
    CHECK(!connection.client.isPasting(connection.handle));
    CHECK(connection.host.state() == ConnectionState::Attached);
}

// A key that is not an interrupt waits behind the whole paste, which goes on.
TEST(aKeyThatIsNotAnInterruptWaitsBehindThePaste)
{
    EncodedPane pane;
    std::uint64_t now = 0;
    UbiquittyTest::PaneConnection connection(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    NarrowLink link { .link = connection };
    REQUIRE(pasteEightMebibytes(link, now));
    const std::array<std::uint8_t, 1> key { 'k' };
    REQUIRE(connection.client.sendRaw(connection.handle, key).has_value());
    const std::uint64_t keyTime = now;
    for (; now < keyTime + 1'000'000; now += 1'000)
        (void)link.step(now);
    CHECK(pane.sink.written.find('k') == std::string::npos);
    CHECK(pane.sink.written.find(bracketClose) == std::string::npos);
    CHECK(connection.client.isPasting(connection.handle));
}

// Ctrl-C typed as a key, not bytes, is the same interrupt: its legacy byte is
// one the line discipline signals on.
TEST(aControlKeyChordIsAnInterrupt)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(link.client.sendPaste(link.handle, repeated('a', 100 * 1024)).has_value());
    link.pump(300);
    const std::array<KeyEvent, 1> interrupt { controlKey('c') };
    REQUIRE(link.client.sendKeys(link.handle, interrupt).has_value());
    CHECK(!link.client.isPasting(link.handle));
    for (std::uint64_t now = 301; now < 320; ++now)
        link.pump(now);
    CHECK(pane.sink.written.ends_with(std::string(bracketClose) + "\x03"));
}

// The characters are the pane's: with VINTR moved to Ctrl-X, Ctrl-C is a key
// like any other and Ctrl-X the interrupt.
TEST(theInterruptCharactersAreThePanes)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    pane.source->noteEchoFacts({ .isSecureEntry = false, .isAtPrompt = false, .interruptCharacters = { .interrupt = 0x18, .quit = std::nullopt, .suspend = std::nullopt }, .foregroundGeneration = 0 });
    UbiquittyTest::feed(pane.terminal, "x");
    pane.source->publish(FrameClass::Settled, 300);
    for (std::uint64_t now = 20'000; now < 200'000; now += 20'000)
        link.pump(now);
    REQUIRE(link.client.interruptCharacters(link.handle).interrupt == std::optional<std::uint8_t> { 0x18 });
    REQUIRE(link.client.sendPaste(link.handle, repeated('a', 100 * 1024)).has_value());
    link.pump(200'000);
    const std::array<std::uint8_t, 1> control { 0x03 };
    REQUIRE(link.client.sendRaw(link.handle, control).has_value());
    CHECK(link.client.isPasting(link.handle));
    const std::array<std::uint8_t, 1> interrupt { 0x18 };
    REQUIRE(link.client.sendRaw(link.handle, interrupt).has_value());
    CHECK(!link.client.isPasting(link.handle));
}

// The legacy byte of a key chord: Ctrl with a letter or a backslash, the
// letter the standard layout's where the layout's is not Latin; nothing for a
// release or another modifier.
TEST(theLegacyControlCharacterOfAChord)
{
    CHECK(legacyControlCharacterOf(controlKey('c')) == std::optional<std::uint8_t> { 0x03 });
    CHECK(legacyControlCharacterOf(controlKey('Z')) == std::optional<std::uint8_t> { 0x1a });
    CHECK(legacyControlCharacterOf(controlKey('\\')) == std::optional<std::uint8_t> { 0x1c });
    KeyEvent cyrillic = controlKey('c');
    cyrillic.unshiftedCodePoint = 0x0441;
    CHECK(!legacyControlCharacterOf(cyrillic).has_value());
    cyrillic.baseLayoutCodePoint = 'c';
    CHECK(legacyControlCharacterOf(cyrillic) == std::optional<std::uint8_t> { 0x03 });
    KeyEvent locked = controlKey('c');
    locked.modifiers |= Ubiquitty::Keys::Keymap::keymapModifierCapsLock;
    CHECK(legacyControlCharacterOf(locked) == std::optional<std::uint8_t> { 0x03 });
    KeyEvent released = controlKey('c');
    released.kind = KeyEventKind::Release;
    CHECK(!legacyControlCharacterOf(released).has_value());
    KeyEvent alternate = controlKey('c');
    alternate.modifiers |= Ubiquitty::Keys::Keymap::keymapModifierAlternate;
    CHECK(!legacyControlCharacterOf(alternate).has_value());
    KeyEvent plain = controlKey('c');
    plain.modifiers = 0;
    CHECK(!legacyControlCharacterOf(plain).has_value());
    CHECK(!legacyControlCharacterOf(controlKey('1')).has_value());
}

// Through the attachment: the interrupt gives the paste up ahead of itself,
// and the program reads the bracket's end, then the interrupt.
TEST(anAttachmentAbortsThePasteAtAnInterrupt)
{
    using namespace UbiquittyTest::AttachmentLink;
    Host host({ 41, 42 });
    RecordingSink sink;
    Ubiquitty::Runtime::RemoteInputEncoder encoder(host.pane.terminal, sink);
    host.pane.source->setInputWriter(encoder);
    UbiquittyTest::feed(host.pane.terminal, "\x1b[?2004h");
    ClientAttachment attached(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, attached, now);
    REQUIRE(attached.send(paneID, PasteInput { .text = bytesOf(repeated('e', 100 * 1024)), .offset = 0, .end = PasteEnd::Final }, now).has_value());
    pump(host, attached, now);
    REQUIRE(sink.written.starts_with(bracketOpen));
    REQUIRE(attached.send(paneID, raw("\x03"), now).has_value());
    pumpFor(host, attached, now, 50'000);
    CHECK(sink.written.ends_with(std::string(bracketClose) + "\x03"));
    CHECK(sink.written.size() < 100 * 1024);
    CHECK_EQ(attached.pendingCount(paneID), 0u);
}

// An aborted paste is never sent again: a drop after the interrupt left
// resumes with the abort, not the paste's text, and the interrupt is written
// once, after the bracket's end.
TEST(aResumeDoesNotResendAnAbortedPaste)
{
    using namespace UbiquittyTest::AttachmentLink;
    Host host({ 41, 42 });
    RecordingSink sink;
    Ubiquitty::Runtime::RemoteInputEncoder encoder(host.pane.terminal, sink);
    host.pane.source->setInputWriter(encoder);
    UbiquittyTest::feed(host.pane.terminal, "\x1b[?2004h");
    ClientAttachment attached(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, attached, now);
    REQUIRE(attached.send(paneID, raw("x"), now).has_value());
    pumpFor(host, attached, now, 50'000);
    REQUIRE(attached.send(paneID, PasteInput { .text = bytesOf(repeated('f', 100 * 1024)), .offset = 0, .end = PasteEnd::Final }, now).has_value());
    pump(host, attached, now, { .shouldCarryDown = false, .shouldCarryUp = true });
    REQUIRE(sink.written.starts_with("x" + std::string(bracketOpen)));
    // The interrupt and its abort leave, and the path goes before either
    // arrives.
    REQUIRE(attached.send(paneID, raw("\x03"), now + 1).has_value());
    pump(host, attached, now + 1, { .shouldCarryDown = false, .shouldCarryUp = false });
    const std::size_t writtenAtDrop = sink.written.size();
    drop(host, attached, now + 2);
    now += 10;
    reconnect(host, attached, now);
    CHECK(sink.written.size() < writtenAtDrop + 1024);
    CHECK(sink.written.ends_with(std::string(bracketClose) + "\x03"));
    CHECK_EQ(std::ranges::count(sink.written, '\x03'), 1);
    CHECK_EQ(attached.pendingCount(paneID), 0u);
}

// A key typed while the paste's pieces are still coming is written after the
// whole paste.
TEST(aKeyTypedDuringAPasteFollowsIt)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::string text = repeated('p', 80 * 1024);
    REQUIRE(link.client.sendPaste(link.handle, text).has_value());
    link.pump(300);
    const std::array<std::uint8_t, 1> key { 'k' };
    REQUIRE(link.client.sendRaw(link.handle, key).has_value());
    for (std::uint64_t now = 301; now < 320; ++now)
        link.pump(now);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + text + std::string(bracketClose) + "k");
}

// The client's keystroke never waits behind the paste's pieces on the way up:
// a key typed after a piece left goes ahead of the next.
TEST(aKeystrokeIsNeverQueuedBehindPasteChunks)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(link.client.sendPaste(link.handle, repeated('q', 1 << 20)).has_value());
    (void)link.client.takeStreamOutput();
    const std::array<std::uint8_t, 1> key { 'k' };
    REQUIRE(link.client.sendRaw(link.handle, key).has_value());
    const std::vector<FrameKind> kinds = kindsOf(link.client.takeStreamOutput());
    REQUIRE(kinds.size() == 2);
    CHECK(kinds[0] == FrameKind::Raw);
    CHECK(kinds[1] == FrameKind::Paste);
}

// Pieces of a paste that come after its PasteAbort, as on a carrier whose
// lanes reorder, are dropped.
TEST(lateChunksOfAnAbortedPasteAreDiscarded)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 3, piece("first", 0), 1) == EnqueueOutcome::Queued);
    pane.source->abortPaste(client, 3, 2);
    CHECK(pane.source->queueInput(client, 3, piece("second", 5), 3) == EnqueueOutcome::Duplicate);
    CHECK(pane.source->queueInput(client, 3, piece("third", 11, PasteEnd::Final), 4) == EnqueueOutcome::Duplicate);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "first" + std::string(bracketClose));
    CHECK(pane.source->inputLedger().lastWritten(client) == std::optional<std::uint64_t> { 3 });
}

// While the program has not yet taken the abort's end, a late piece is still
// dropped: it never reopens the bracket behind the end.
TEST(aLatePieceBeforeTheEndIsTakenIsDiscarded)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 3, piece("first", 0), 1) == EnqueueOutcome::Queued);
    pane.sink.refusals = 1'000;
    pane.source->abortPaste(client, 3, 2);
    CHECK(pane.source->queueInput(client, 3, piece("second", 5), 3) == EnqueueOutcome::Duplicate);
    pane.sink.refusals = 0;
    pane.source->drainInput(4);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "first" + std::string(bracketClose));
}

// A PasteAbort that overtook its paste's first piece ends the paste before
// it starts.
TEST(anAbortAheadOfItsPasteEndsItBeforeItStarts)
{
    EncodedPane pane;
    pane.source->abortPaste(client, 4, 1);
    CHECK(pane.source->queueInput(client, 4, piece("late", 0), 2) == EnqueueOutcome::Duplicate);
    CHECK_STR_EQ(pane.sink.written, std::string());
    const std::array<std::uint8_t, 1> key { 'k' };
    CHECK(pane.source->queueInput(client, 5, RawInput { .bytes = { key.begin(), key.end() } }, 3) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string("k"));
}

// A PasteAbort naming another number than the paste in progress changes
// nothing.
TEST(aPasteAbortNamingAnotherSeqChangesNothing)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 3, piece("one ", 0), 1) == EnqueueOutcome::Queued);
    pane.source->abortPaste(client, 9, 2);
    pane.source->abortPaste(client, 2, 2);
    CHECK(pane.source->queueInput(client, 3, piece("two", 4, PasteEnd::Final), 3) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "one two" + std::string(bracketClose));
}

// A CR at one piece's end and an LF at the next's start are one CR.
TEST(aCRLFSplitAcrossChunksIsOneCR)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 1, piece("a\r", 0), 1) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(client, 1, piece("\nb", 2, PasteEnd::Final), 2) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "a\rb" + std::string(bracketClose));
}

// An end marker split across two pieces cannot close the bracket early: the
// only end the program reads is the paste's own.
TEST(aFenceSplitAcrossChunksIsNeutralised)
{
    for (std::size_t cut = 1; cut < 6; ++cut) {
        EncodedPane pane;
        const std::string text = std::string("x") + std::string(bracketClose) + "y";
        CHECK(pane.source->queueInput(client, 1, piece(std::string_view(text).substr(0, 1 + cut), 0), 1) == EnqueueOutcome::Queued);
        CHECK(pane.source->queueInput(client, 1, piece(std::string_view(text).substr(1 + cut), 1 + cut, PasteEnd::Final), 2) == EnqueueOutcome::Queued);
        CHECK(pane.sink.written.find(bracketClose) == pane.sink.written.size() - bracketClose.size());
    }
}

// The client cuts its pieces on scalar boundaries: a paste of three-byte
// characters is written whole, every piece taken.
TEST(aChunkCutNeverSplitsAScalar)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::string text;
    for (int index = 0; index < 30'000; ++index)
        text += "\xe2\x82\xac";
    REQUIRE(link.client.sendPaste(link.handle, text).has_value());
    for (std::uint64_t now = 300; now < 320; ++now)
        link.pump(now);
    CHECK(link.host.state() == ConnectionState::Attached);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + text + std::string(bracketClose));
}

// A piece over the cap, or a paste past sixteen mebibytes, ends the
// connection.
TEST(aChunkOverItsLimitCloses)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::vector<std::uint8_t> text(maxPasteChunkBytes + 1, 'a');
    std::vector<std::uint8_t> body;
    encode(body, Paste { .inputSeq = 1, .offset = 0, .state = PasteState::More, .text = text });
    link.host.receiveStream([&] {
        std::vector<std::uint8_t> frame;
        (void)appendFrame(frame, FrameKind::Paste, link.handle, body);
        return frame;
    }(),
        300);
    CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
}

TEST(aPasteOverSixteenMebibytesCloses)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::vector<std::uint8_t> text(16, 'a');
    std::vector<std::uint8_t> body;
    encode(body, Paste { .inputSeq = 1, .offset = maxPasteBytes - 8, .state = PasteState::Final, .text = text });
    std::vector<std::uint8_t> frame;
    (void)appendFrame(frame, FrameKind::Paste, link.handle, body);
    link.host.receiveStream(frame, 300);
    CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
}

// A piece that ends inside a character, or starts inside one, ends the
// connection: the host never writes half a character.
TEST(aPieceSplittingACharacterCloses)
{
    for (const std::string_view text : { std::string_view("ok\xe2\x82"), std::string_view("\x82\xacok") }) {
        EncodedPane pane;
        UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
        const std::vector<std::uint8_t> bytes = bytesOf(text);
        std::vector<std::uint8_t> body;
        encode(body, Paste { .inputSeq = 1, .offset = 0, .state = PasteState::More, .text = bytes });
        std::vector<std::uint8_t> frame;
        (void)appendFrame(frame, FrameKind::Paste, link.handle, body);
        link.host.receiveStream(frame, 300);
        CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
        CHECK_STR_EQ(pane.sink.written, std::string());
    }
}

// A piece that skips past what was taken is malformed; a piece that overlaps
// it, a resend, is trimmed to what is new.
TEST(aGapIsRefusedAndAnOverlapIsTrimmed)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 1, piece("abcd", 0), 1) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(client, 1, piece("zz", 6), 2) == EnqueueOutcome::Malformed);
    CHECK(pane.source->queueInput(client, 1, piece("abcdef", 0), 3) == EnqueueOutcome::Queued);
    CHECK(pane.source->queueInput(client, 1, piece("abc", 0), 4) == EnqueueOutcome::Duplicate);
    CHECK(pane.source->queueInput(client, 1, piece("efgh", 4, PasteEnd::Final), 5) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "abcdefgh" + std::string(bracketClose));
    // A first piece that does not start at the paste's start is a gap too.
    CHECK(pane.source->queueInput(client, 2, piece("late", 3), 6) == EnqueueOutcome::Malformed);
}

// A host sees the gap and ends the connection.
TEST(aGapClosesTheConnection)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::vector<std::uint8_t> text = bytesOf("later");
    std::vector<std::uint8_t> body;
    encode(body, Paste { .inputSeq = 1, .offset = 100, .state = PasteState::More, .text = text });
    std::vector<std::uint8_t> frame;
    (void)appendFrame(frame, FrameKind::Paste, link.handle, body);
    link.host.receiveStream(frame, 300);
    CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
}

// The client is gone mid-paste for good: the bracket it opened is closed,
// and what it had typed after the paste is written behind it.
TEST(anAbandonedPasteStillClosesItsBracket)
{
    EncodedPane pane;
    CHECK(pane.source->queueInput(client, 1, piece("half a pas", 0), 1) == EnqueueOutcome::Queued);
    const std::array<std::uint8_t, 1> key { 'k' };
    CHECK(pane.source->queueInput(client, 2, RawInput { .bytes = { key.begin(), key.end() } }, 2) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "half a pas");
    pane.source->abandonPaste(client, 3);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "half a pas" + std::string(bracketClose) + "k");
}

// Without bracketed paste the text is written as the normaliser leaves it,
// with no bracket to open or close.
TEST(aPasteWithoutBracketedPasteIsWrittenBare)
{
    EncodedPane pane(Ubiquitty::Input::BracketedPaste::Disabled);
    CHECK(pane.source->queueInput(client, 1, piece("a\r\nb\x03", 0, PasteEnd::Final), 1) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string("a\rb"));
}

// A piece the program refused is encoded again the same way: the bracket
// opens once and the normaliser's state is not spent.
TEST(aRefusedPieceIsWrittenOnceWhenTaken)
{
    EncodedPane pane;
    pane.sink.refusals = 1;
    CHECK(pane.source->queueInput(client, 1, piece("x\r", 0), 1) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string());
    pane.source->drainInput(2);
    CHECK(pane.source->queueInput(client, 1, piece("\ny", 2, PasteEnd::Final), 3) == EnqueueOutcome::Queued);
    CHECK_STR_EQ(pane.sink.written, std::string(bracketOpen) + "x\ry" + std::string(bracketClose));
}

// A paste cut by a drop is written once: the resume reports the client's
// count below it, the client sends it again from its start, and the host
// takes only what it had not.
TEST(aPasteCutByADropIsWrittenOnce)
{
    using namespace UbiquittyTest::AttachmentLink;
    Host host({ 41, 42 });
    RecordingSink sink;
    Ubiquitty::Runtime::RemoteInputEncoder encoder(host.pane.terminal, sink);
    host.pane.source->setInputWriter(encoder);
    UbiquittyTest::feed(host.pane.terminal, "\x1b[?2004h");
    ClientAttachment attached(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, attached, now);
    // A key first, so the host has a count for the client to resume from.
    REQUIRE(attached.send(paneID, raw("x"), now).has_value());
    pumpFor(host, attached, now, 50'000);
    const std::string text = repeated('d', 100 * 1024);
    REQUIRE(attached.send(paneID, PasteInput { .text = bytesOf(text), .offset = 0, .end = PasteEnd::Final }, now).has_value());
    // Two pieces reach the host; then the path goes.
    pump(host, attached, now, { .shouldCarryDown = false, .shouldCarryUp = true });
    pump(host, attached, now + 1, { .shouldCarryDown = false, .shouldCarryUp = true });
    REQUIRE(sink.written.size() > 1 + bracketOpen.size());
    REQUIRE(sink.written.size() < text.size());
    drop(host, attached, now + 2);
    now += 10;
    reconnect(host, attached, now);
    CHECK(sink.written == "x" + std::string(bracketOpen) + text + std::string(bracketClose));
    CHECK_EQ(attached.pendingCount(paneID), 0u);
}

// A Welcome carries the host's paste limits after the resume's fields, and
// they round-trip with or without a resume.
TEST(theWelcomeCarriesThePasteLimits)
{
    Welcome welcome;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    welcome.wireVersion = wireVersion;
    welcome.pasteLimits = PasteLimits { .chunkBytes = 32 * 1024, .pasteBytes = 16 << 20, .windowBytes = 256 * 1024 };
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    std::optional<Welcome> decoded = decodeWelcome(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->pasteLimits == welcome.pasteLimits);
    // A Welcome that ends inside its paste limits is malformed: no served
    // version states fewer than three.
    CHECK(!decodeWelcome(std::span { body }.first(body.size() - 4)).has_value());
    welcome.resumed.push_back({ .paneID = { 1, 2 }, .outcome = ResumeOutcome::Diffed, .inputWritten = 4 });
    body.clear();
    encode(body, welcome);
    decoded = decodeWelcome(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->pasteLimits == welcome.pasteLimits);
    CHECK_EQ(decoded->resumed.size(), 1u);
}

// A paste is typing: it takes the size from another driver.
TEST(aPasteTakesTheSize)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection first(*pane.source, { 31, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection second(*pane.source, { 32, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::array<std::uint8_t, 1> key { 'k' };
    REQUIRE(first.client.sendRaw(first.handle, key).has_value());
    first.pump(300);
    constexpr ID128 secondID { 32, 100 };
    REQUIRE(second.client.sendPaste(second.handle, "pasted").has_value());
    second.pump(301);
    CHECK(pane.source->geometry().drivingOwner() == std::optional { secondID });
}

// A program that reads nothing: its paste stays inside the window the host
// stated, the rest stays with the client, and the connection stands. When the
// program reads, the host's receipts open the window and the whole paste is
// written once, in order.
TEST(aPasteIntoAProgramThatReadsNothingStaysInsideTheWindow)
{
    EncodedPane pane;
    pane.sink.refusals = 1 << 30;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(link.client.welcome().has_value());
    REQUIRE(link.client.welcome()->pasteLimits.has_value());
    const std::size_t window = link.client.welcome()->pasteLimits->windowBytes;
    REQUIRE(window == measuredInputPolicy().pasteWindowBytes);
    const std::string text = repeated('w', std::size_t { 8 } << 20);
    REQUIRE(link.client.sendPaste(link.handle, text).has_value());
    std::uint64_t now = 300;
    std::size_t mostHeld = 0;
    for (int rounds = 0; rounds < 2'000; ++rounds) {
        link.pump(++now);
        mostHeld = std::max(mostHeld, pane.source->heldInputBytes());
    }
    // One piece past the window, and each piece's share of the bracket.
    CHECK(mostHeld >= window);
    CHECK(mostHeld <= window + 2 * clientPastePieceBytes);
    CHECK(pane.sink.written.empty());
    CHECK(link.client.isPasting(link.handle));
    CHECK(link.host.state() == ConnectionState::Attached);
    CHECK_EQ(link.host.counters().pasteReceiptsSent, 0u);

    pane.sink.refusals = 0;
    for (int rounds = 0; rounds < 8'000 && (link.client.isPasting(link.handle) || pane.source->hasPendingInput()); ++rounds) {
        pane.source->drainInput(++now);
        link.pump(now);
        mostHeld = std::max(mostHeld, pane.source->heldInputBytes());
    }
    CHECK(!link.client.isPasting(link.handle));
    CHECK(mostHeld <= window + 2 * clientPastePieceBytes);
    CHECK(link.host.counters().pasteReceiptsSent > 0);
    CHECK(pane.sink.written == std::string(bracketOpen) + text + std::string(bracketClose));
    CHECK(link.host.state() == ConnectionState::Attached);
}

// The window is the host's to state: a smaller one holds less.
TEST(theWindowIsTheOneTheHostStates)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = 32 * 1024;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    (void)link.addPane();
    UbiquittyTest::RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1 << 30);
    REQUIRE(link.client().welcome()->pasteLimits->windowBytes == 32u * 1024u);
    REQUIRE(link.client().sendPaste(link.handle(), repeated('s', std::size_t { 1 } << 20)).has_value());
    for (int rounds = 0; rounds < 400; ++rounds)
        link.step();
    CHECK(link.source(link.handle()).heldInputBytes() >= 32u * 1024u);
    CHECK(link.source(link.handle()).heldInputBytes() <= 32u * 1024u + 2 * clientPastePieceBytes);
    CHECK(link.host().state() == ConnectionState::Attached);
}

// A host that states no window is sent a paste as it comes, as a host below
// 15 is: nothing waits for a receipt that host never sends.
TEST(aHostThatStatesNoWindowIsSentThePasteAsItComes)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = 0;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    (void)link.addPane();
    UbiquittyTest::RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1 << 30);
    REQUIRE(link.client().welcome()->pasteLimits->windowBytes == 0u);
    REQUIRE(link.client().sendPaste(link.handle(), repeated('n', std::size_t { 1 } << 20)).has_value());
    for (int rounds = 0; rounds < 40; ++rounds)
        link.step();
    // Forty pieces left with no receipt, past where a window would stop.
    CHECK(link.source(link.handle()).heldInputBytes() > measuredInputPolicy().pasteWindowBytes);
}

// A paste a program is not reading holds no other pane's paste behind it.
TEST(aPasteWaitingOnItsWindowDoesNotHoldAnothersPaste)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = 32 * 1024;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    const std::uint32_t stalled = link.addPane();
    const std::uint32_t reading = link.addPane();
    UbiquittyTest::RecordingWriter stalledWriter;
    UbiquittyTest::RecordingWriter readingWriter;
    link.source(stalled).setInputWriter(stalledWriter);
    link.source(reading).setInputWriter(readingWriter);
    stalledWriter.refuseNext(1 << 30);
    REQUIRE(link.client().sendPaste(stalled, repeated('a', std::size_t { 1 } << 20)).has_value());
    REQUIRE(link.client().sendPaste(reading, repeated('b', 200 * 1024)).has_value());
    for (int rounds = 0; rounds < 400; ++rounds)
        link.step();
    CHECK(link.client().isPasting(stalled));
    CHECK(!link.client().isPasting(reading));
    CHECK(!link.source(reading).hasPendingInput());
    CHECK(link.source(stalled).heldInputBytes() <= 32u * 1024u + 2 * clientPastePieceBytes);
}

// A PasteReceipt round-trips, and one a client sends closes it: the kind
// travels downward only.
TEST(aPasteReceiptTravelsDownwardOnly)
{
    const PasteReceipt receipt { .inputSeq = 7, .bytesWritten = 123'456 };
    std::vector<std::uint8_t> body;
    encode(body, receipt);
    CHECK_EQ(body.size(), 16u);
    CHECK(decodePasteReceipt(body) == std::optional { receipt });
    CHECK(!decodePasteReceipt(std::span { body }.first(15)).has_value());

    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::PasteReceipt, link.handle, body));
    link.host.receiveStream(frame, 300);
    CHECK(link.host.state() == ConnectionState::Closed);
    CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
}

// A receipt for a pane the client has not bound, or one it cannot read,
// closes the client.
TEST(aPasteReceiptForAnUnboundPaneClosesTheClient)
{
    EncodedPane pane;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::vector<std::uint8_t> body;
    encode(body, PasteReceipt { .inputSeq = 1, .bytesWritten = 1 });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::PasteReceipt, 77, body));
    link.client.receiveStream(frame, 300);
    CHECK(link.client.state() == ConnectionState::Closed);

    EncodedPane other;
    UbiquittyTest::PaneConnection bound(*other.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::vector<std::uint8_t> shortFrame;
    REQUIRE(appendFrame(shortFrame, FrameKind::PasteReceipt, bound.handle, std::span { body }.first(15)));
    bound.client.receiveStream(shortFrame, 300);
    CHECK(bound.client.state() == ConnectionState::Closed);
}

// A receipt is sent when the program has taken more of the paste, at once,
// and not again until it takes more.
TEST(aReceiptIsSentOnceForEachAdvance)
{
    EncodedPane pane;
    pane.sink.refusals = 1 << 30;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    REQUIRE(link.client.sendPaste(link.handle, repeated('r', std::size_t { 2 } << 20)).has_value());
    std::uint64_t now = 300;
    for (int rounds = 0; rounds < 200; ++rounds)
        link.pump(++now);
    CHECK_EQ(link.host.counters().pasteReceiptsSent, 0u);
    // The program reads what the pane held, then nothing again.
    pane.sink.refusals = 0;
    pane.source->drainInput(++now);
    pane.sink.refusals = 1 << 30;
    CHECK(!pane.sink.written.empty());
    CHECK(link.host.nextServiceMicroseconds(now) == std::optional { now });
    for (int rounds = 0; rounds < 200; ++rounds)
        link.pump(++now);
    CHECK_EQ(link.host.counters().pasteReceiptsSent, 1u);
    CHECK(link.client.isPasting(link.handle));
}

// A client reads a receipt for the paste it names, on the pane it came on,
// as far as the client has sent and never backward.
TEST(aClientReadsAReceiptForItsPasteAndNeverBackward)
{
    constexpr std::uint32_t window = 32 * 1024;
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = window;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    (void)link.addPane();
    UbiquittyTest::RecordingWriter writer;
    link.source(link.handle()).setInputWriter(writer);
    writer.refuseNext(1 << 30);
    const std::optional<std::uint64_t> pasted = link.client().sendPaste(link.handle(), repeated('c', std::size_t { 1 } << 20));
    REQUIRE(pasted.has_value());
    const auto settle = [&] {
        for (int rounds = 0; rounds < 100; ++rounds)
            link.step();
        return link.source(link.handle()).heldInputBytes();
    };
    const auto receive = [&](std::uint64_t inputSeq, std::uint64_t bytesWritten) {
        std::vector<std::uint8_t> body;
        encode(body, PasteReceipt { .inputSeq = inputSeq, .bytesWritten = bytesWritten });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::PasteReceipt, link.handle(), body));
        link.client().receiveStream(frame, link.now());
    };
    const std::size_t stalled = settle();
    REQUIRE(stalled >= window);
    // Another paste's receipt opens nothing.
    receive(*pasted + 5, window);
    CHECK_EQ(settle(), stalled);
    // A receipt overtaken by a newer one takes nothing back.
    receive(*pasted, window);
    receive(*pasted, 0);
    const std::size_t opened = settle();
    CHECK(opened >= stalled + window - clientPastePieceBytes);
    CHECK(opened <= stalled + window + clientPastePieceBytes);
    // A receipt past what the client sent counts as all it sent.
    receive(*pasted, ~std::uint64_t { 0 });
    const std::size_t openedWhole = settle();
    CHECK(openedWhole >= opened + window);
    CHECK(openedWhole <= opened + 2 * window);
    CHECK(link.client().state() == ConnectionState::Attached);
}

// A pane unbound under a paste takes none of the rest, and the client is no
// longer pasting into it.
TEST(aPaneUnboundUnderAPasteTakesNoneOfTheRest)
{
    HostPolicy policy = UbiquittyTest::syncHostPolicy(0);
    policy.input.pasteWindowBytes = 32 * 1024;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    const std::uint32_t handle = link.addPane();
    UbiquittyTest::RecordingWriter writer;
    link.source(handle).setInputWriter(writer);
    writer.refuseNext(1 << 30);
    REQUIRE(link.client().sendPaste(handle, repeated('u', std::size_t { 1 } << 20)).has_value());
    for (int rounds = 0; rounds < 100; ++rounds)
        link.step();
    REQUIRE(link.client().isPasting(handle));
    link.host().unbindPane(handle, 0);
    for (int rounds = 0; rounds < 100; ++rounds)
        link.step();
    CHECK(!link.client().isPasting(handle));
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(link.client().state() == ConnectionState::Attached);
}

// Each client's paste is reported by its own progress: one whose paste the
// program has taken none of is told nothing of another's.
TEST(aPastesProgressIsItsOwnClients)
{
    constexpr ID128 other { 17, 18 };
    EncodedPane pane;
    CHECK(pane.source->queueInput(other, 1, piece("abcdef", 0), 1) == EnqueueOutcome::Queued);
    pane.sink.refusals = 1 << 30;
    CHECK(pane.source->queueInput(client, 4, piece("xy", 0), 2) == EnqueueOutcome::Queued);
    constexpr PaneInputQueue::PasteWritten taken { .inputSeq = 1, .bytesWritten = 6 };
    constexpr PaneInputQueue::PasteWritten waiting { .inputSeq = 4, .bytesWritten = 0 };
    CHECK(pane.source->pasteWritten(other) == std::optional { taken });
    CHECK(pane.source->pasteWritten(client) == std::optional { waiting });
    CHECK(!pane.source->pasteWritten(ID128 { 99, 99 }).has_value());
}

// A client that ignores the window it was stated is not held back and gains
// nothing by it: the pane takes its paste to the pane's bound, the piece past
// that closes the connection, and the program is written none of it.
TEST(aClientIgnoringItsWindowIsClosedAtThePanesBound)
{
    EncodedPane pane;
    pane.sink.refusals = 1 << 30;
    UbiquittyTest::PaneConnection link(*pane.source, client, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const std::vector<std::uint8_t> piece = bytesOf(repeated('g', clientPastePieceBytes));
    std::uint64_t offset = 0;
    std::size_t mostHeld = 0;
    for (int pieces = 0; pieces < 1'000 && link.host.state() == ConnectionState::Attached; ++pieces) {
        std::vector<std::uint8_t> body;
        encode(body, Paste { .inputSeq = 1, .offset = offset, .state = PasteState::More, .text = piece });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::Paste, link.handle, body));
        link.host.receiveStream(frame, 300 + offset);
        offset += piece.size();
        mostHeld = std::max(mostHeld, pane.source->heldInputBytes());
    }
    CHECK(link.host.state() == ConnectionState::Closed);
    CHECK(link.host.byeReason() == std::optional { ByeReason::Oversize });
    CHECK(mostHeld > measuredInputPolicy().pasteWindowBytes);
    CHECK(mostHeld <= maxHeldInputBytes);
    CHECK(pane.sink.written.empty());
}

// The window is no way past the host's authority: a viewer's paste, or one
// from a client without the Drive scope, takes no room in the pane, is sent
// no receipt and closes its connection.
TEST(aPasteWithoutAuthorityTakesNoRoomAndIsSentNoReceipt)
{
    for (const bool isViewer : { true, false }) {
        EncodedPane pane;
        UbiquittyTest::PaneConnection link(*pane.source, client, isViewer ? ClientKind::Viewer : ClientKind::Human, isViewer ? UbiquittyTest::DriveScope::Granted : UbiquittyTest::DriveScope::Withheld);
        const std::vector<std::uint8_t> text = bytesOf("rm -rf ~");
        std::vector<std::uint8_t> body;
        encode(body, Paste { .inputSeq = 1, .offset = 0, .state = PasteState::Final, .text = text });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::Paste, link.handle, body));
        link.host.receiveStream(frame, 300);
        link.pump(400);
        CHECK(link.host.state() == ConnectionState::Closed);
        CHECK(link.host.byeReason() == std::optional { ByeReason::Malformed });
        CHECK_EQ(pane.source->heldInputBytes(), 0u);
        CHECK(!pane.source->pasteWritten(client).has_value());
        CHECK_EQ(link.host.counters().pasteReceiptsSent, 0u);
        CHECK(pane.sink.written.empty());
    }
}

UBIQUITTY_TEST_MAIN()
