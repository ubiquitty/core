// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_remote_input — a remote client's keys, pointer events and focus,
// encoded on the host under the modes live when each is written
// (Docs/sync.md §6.7, §6.11). A mode the program sets between the client
// sending an event and the host writing it decides the bytes; a write the
// program refused is encoded again when it is retried. A paste written in
// pieces writes what the same paste written whole does.

#include "Core/Protocol/Sync/SyncLink.h"
#include "Harness.h"

#include "Core/Keys/KeyTranslation.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Runtime/RemoteInput.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using Ubiquitty::Runtime::ByteSink;
using Ubiquitty::Runtime::RemoteInputEncoder;
using UbiquittyTest::DriveScope;
using UbiquittyTest::OwnedPane;
using UbiquittyTest::PaneConnection;
using UbiquittyTest::SyncLink;

namespace {

/// A program's input: what it took, and a way to refuse writes.
class RecordingSink final : public ByteSink {
public:
    [[nodiscard]] WriteStatus writeAll(ID128, std::span<const std::uint8_t> bytes) override
    {
        if (m_refusals > 0) {
            --m_refusals;
            return WriteStatus::Refused;
        }
        m_bytes.append(bytes.begin(), bytes.end());
        return WriteStatus::Written;
    }

    void refuseNext(int count) { m_refusals = count; }
    [[nodiscard]] const std::string& bytes() const { return m_bytes; }

private:
    std::string m_bytes;
    int m_refusals = 0;
};

KeyEvent upArrow()
{
    Ubiquitty::Runtime::KeyPress press;
    press.virtualKey = Ubiquitty::Keys::VirtualKey::UpArrow;
    return Ubiquitty::Runtime::keyEventOf(press);
}

/// Writes one paste through a pane's encoder, whole or in pieces, and returns
/// what the program was written.
///
/// - Parameters:
///   - terminal: The pane's terminal, whose paste modes each write reads.
///   - text: The paste.
///   - cuts: Ascending offsets into the text at which a piece ends and the
///     next begins; none writes the paste as one final piece.
/// - Returns: Every byte the sink took.
std::string pastedBytes(const Ubiquitty::Terminal& terminal, std::string_view text, std::span<const std::size_t> cuts)
{
    constexpr ID128 pasting { 7, 8 };
    RecordingSink sink;
    RemoteInputEncoder encoder(terminal, sink);
    std::size_t start = 0;
    const auto writePiece = [&](std::size_t end, PasteEnd ending) {
        const std::string_view piece = text.substr(start, end - start);
        const InputIntent intent = PasteInput { .text = std::vector<std::uint8_t>(piece.begin(), piece.end()), .offset = static_cast<std::uint64_t>(start), .end = ending };
        CHECK(encoder.write(pasting, intent) == WriteStatus::Written);
        start = end;
    };
    for (const std::size_t cut : cuts)
        writePiece(cut, PasteEnd::More);
    writePiece(text.size(), PasteEnd::Final);
    return sink.bytes();
}

} // namespace

// The client sends Up; the program is not reading, so the write waits. The
// program then sets DECCKM, and the key it reads is the application form,
// because the host encodes it when it writes.
TEST(aKeyIsEncodedUnderTheModesLiveWhenItIsWritten)
{
    SyncLink link(0, 24);
    RecordingSink sink;
    RemoteInputEncoder encoder(link.hostTerminal(link.handle()), sink);
    link.source(link.handle()).setInputWriter(encoder);
    sink.refuseNext(1);
    const std::vector<KeyEvent> keys { upArrow() };
    CHECK(link.client().sendKeys(link.handle(), keys).has_value());
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string());
    link.write("\x1b[?1h");
    link.source(link.handle()).drainInput(link.now());
    CHECK_STR_EQ(sink.bytes(), std::string("\x1bOA"));
    // And with DECCKM reset, the next Up reads as the cursor form.
    link.write("\x1b[?1l");
    CHECK(link.client().sendKeys(link.handle(), keys).has_value());
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1bOA\x1b[A"));
}

// The pointer report takes the mouse encoding the program set by the write,
// not the one set when the client sent it.
TEST(aPointerIsEncodedByTheMouseModeLiveAtTheWrite)
{
    SyncLink link(0, 24);
    RecordingSink sink;
    RemoteInputEncoder encoder(link.hostTerminal(link.handle()), sink);
    link.source(link.handle()).setInputWriter(encoder);
    link.write("\x1b[?1000h");
    sink.refuseNext(1);
    const std::vector<PointerEvent> press { { .kind = PointerEventKind::Press, .button = 0, .modifiers = 0, .row = 4, .column = 9 } };
    CHECK(link.client().sendPointer(link.handle(), press).has_value());
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string());
    link.write("\x1b[?1006h");
    link.source(link.handle()).drainInput(link.now());
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[<0;10;5M"));
    // With no mouse mode at the write, the event writes nothing.
    link.write("\x1b[?1000l");
    CHECK(link.client().sendPointer(link.handle(), press).has_value());
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[<0;10;5M"));
}

// A focus change reaches the program only while it asked for focus reports.
TEST(aFocusEventIsWrittenOnlyUnderMode1004)
{
    SyncLink link(0, 24);
    RecordingSink sink;
    RemoteInputEncoder encoder(link.hostTerminal(link.handle()), sink);
    link.source(link.handle()).setInputWriter(encoder);
    CHECK(link.client().sendFocus(link.handle(), Ubiquitty::Modes::FocusState::Focused));
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string());
    CHECK(link.source(link.handle()).isAnyDriverFocused());
    link.write("\x1b[?1004h");
    CHECK(link.client().sendFocus(link.handle(), Ubiquitty::Modes::FocusState::Unfocused));
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[O"));
    CHECK(link.client().sendFocus(link.handle(), Ubiquitty::Modes::FocusState::Focused));
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[O\x1b[I"));
}

// Two drivers on one pane: the pane is focused while either is. The second
// losing the focus while the first holds it reports nothing.
TEST(aSecondDriverLosingFocusWritesNoFocusOutWhileTheFirstHoldsIt)
{
    OwnedPane pane;
    RecordingSink sink;
    RemoteInputEncoder encoder(pane.terminal, sink);
    pane.source->setInputWriter(encoder);
    UbiquittyTest::feed(pane.terminal, "\x1b[?1004h");
    PaneConnection first(*pane.source, { 7, 8 }, ClientKind::Human, DriveScope::Granted);
    PaneConnection second(*pane.source, { 11, 12 }, ClientKind::Human, DriveScope::Granted);
    CHECK(first.client.sendFocus(first.handle, Ubiquitty::Modes::FocusState::Focused));
    first.pump(300);
    CHECK(second.client.sendFocus(second.handle, Ubiquitty::Modes::FocusState::Focused));
    second.pump(300);
    CHECK(second.client.sendFocus(second.handle, Ubiquitty::Modes::FocusState::Unfocused));
    second.pump(301);
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[I"));
    // The first leaving takes the last focus away.
    CHECK(first.client.sendFocus(first.handle, Ubiquitty::Modes::FocusState::Unfocused));
    first.pump(302);
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[I\x1b[O"));
}

// A pane focused in a window that is not is not a focused driver; and a
// driver whose connection closes leaves the focus.
TEST(anUnfocusedWindowIsNotAFocusedDriver)
{
    SyncLink link(0, 24);
    RecordingSink sink;
    RemoteInputEncoder encoder(link.hostTerminal(link.handle()), sink);
    link.source(link.handle()).setInputWriter(encoder);
    link.write("\x1b[?1004h");
    link.client().setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    CHECK(link.client().sendFocus(link.handle(), Ubiquitty::Modes::FocusState::Focused));
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string());
    // The window gains the focus: now the driver is focused.
    link.client().setFocus(Ubiquitty::Modes::FocusState::Focused);
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[I"));
    // An unbind takes it away.
    link.unbind();
    CHECK_STR_EQ(sink.bytes(), std::string("\x1b[I\x1b[O"));
}

// Only a driver's keys and pointer reach the program; a viewer's input closes
// its connection and writes nothing.
TEST(aViewerMayNotSendKeysOrPointer)
{
    for (const FrameKind kind : { FrameKind::Keys, FrameKind::Pointer }) {
        OwnedPane pane;
        RecordingSink sink;
        RemoteInputEncoder encoder(pane.terminal, sink);
        pane.source->setInputWriter(encoder);
        UbiquittyTest::feed(pane.terminal, "\x1b[?1000h");
        PaneConnection viewer(*pane.source, { 7, 8 }, ClientKind::Viewer, DriveScope::Granted);
        const std::vector<KeyEvent> keys { upArrow() };
        const std::vector<PointerEvent> press { { .kind = PointerEventKind::Press, .button = 0, .modifiers = 0, .row = 1, .column = 1 } };
        if (kind == FrameKind::Keys)
            CHECK(viewer.client.sendKeys(viewer.handle, keys).has_value());
        else
            CHECK(viewer.client.sendPointer(viewer.handle, press).has_value());
        viewer.pump(300);
        CHECK(viewer.host.state() == ConnectionState::Closed);
        CHECK(viewer.host.byeReason() == ByeReason::Malformed);
        CHECK_STR_EQ(sink.bytes(), std::string());
    }
}

// The wire round trip of each input body, and its bounds: a count past the
// body is refused before any event is read, and so is a trailing byte.
TEST(everyInputBodyRoundTripsAndRefusesATrailingByte)
{
    KeyEvent key = upArrow();
    key.text = "é";
    key.modifiers = 5;
    key.kind = KeyEventKind::Repeat;
    std::vector<std::uint8_t> body;
    encode(body, Keys { .inputSeq = 3, .events = { key } });
    const std::optional<Keys> keys = decodeKeys(body);
    REQUIRE(keys.has_value());
    CHECK_EQ(keys->inputSeq, 3u);
    REQUIRE(keys->events.size() == 1);
    CHECK(keys->events[0] == key);
    body.push_back(0);
    CHECK(!decodeKeys(body));

    const PointerEvent event { .kind = PointerEventKind::Release, .button = 2, .modifiers = 16, .row = 7, .column = 300, .pixelX = 12, .pixelY = noPointerPixel };
    body.clear();
    encode(body, Pointer { .inputSeq = 9, .events = { event, event } });
    const std::optional<Pointer> pointer = decodePointer(body);
    REQUIRE(pointer.has_value());
    CHECK(pointer->events.size() == 2 && pointer->events[1] == event);
    body.push_back(0);
    CHECK(!decodePointer(body));

    body.clear();
    encode(body, Focus { .isFocused = true });
    CHECK(decodeFocus(body).has_value() && decodeFocus(body)->isFocused);
    body.push_back(0);
    CHECK(!decodeFocus(body));

    // No event at all, a number 0, or a key past the key table's range, is
    // no input frame.
    body.clear();
    encode(body, Keys { .inputSeq = 1, .events = {} });
    CHECK(!decodeKeys(body));
    body.clear();
    encode(body, Pointer { .inputSeq = 0, .events = { event } });
    CHECK(!decodePointer(body));
    key.key = 0x10000;
    body.clear();
    encode(body, Keys { .inputSeq = 1, .events = { key } });
    CHECK(!decodeKeys(body));
}

// A Keys whose count promises more events than its body could hold is
// refused before an event is read or a vector sized.
TEST(aKeysCountBeyondTheBodyIsRefusedBeforeAllocation)
{
    std::vector<std::uint8_t> body;
    encode(body, Keys { .inputSeq = 1, .events = { upArrow() } });
    // The count is the byte after the eight-byte number; claim the most a
    // frame may carry with one event's bytes behind it.
    body[8] = static_cast<std::uint8_t>(maxInputEventsPerFrame);
    CHECK(!decodeKeys(body));
    body[8] = static_cast<std::uint8_t>(maxInputEventsPerFrame + 1);
    CHECK(!decodeKeys(body));
}

TEST(keyPositionsAndSidedModifiersReachTheHostEncoder)
{
    SyncLink link(0, 24);
    RecordingSink sink;
    RemoteInputEncoder encoder(link.hostTerminal(link.handle()), sink);
    link.source(link.handle()).setInputWriter(encoder);
    link.write("\033[?81h");
    Ubiquitty::Runtime::KeyPress press;
    press.virtualKey = Ubiquitty::Keys::VirtualKey::KeyA;
    press.unshiftedCodePoint = 0x0444;
    press.modifiers = Ubiquitty::Keys::Keymap::keymapModifierShift | (2u << Ubiquitty::Keys::positionModifierShift);
    const std::vector<KeyEvent> keys { Ubiquitty::Runtime::keyEventOf(press) };
    CHECK(link.client().sendKeys(link.handle(), keys).has_value());
    link.pump();
    CHECK_STR_EQ(sink.bytes(), std::string("\033_:C0102\033\\"));
    // Set member by member, as the KeyPress above is: a designated list that
    // names two of the eight members is a missing-field-initializers error
    // under GCC and clang on Linux.
    KeyEvent control;
    control.modifiers = static_cast<std::uint16_t>(Ubiquitty::Keys::Keymap::keymapModifierCtrl | (8u << Ubiquitty::Keys::positionModifierShift));
    control.unshiftedCodePoint = 'c';
    CHECK(legacyControlCharacterOf(control) == 3);
}

// A paste the client sends in pieces, wherever they are cut, writes what the
// same paste sent whole writes. Under each combination of bracketed paste
// (2004), character quoting (2005) and literal newlines (2006), the bracket
// opens at the first piece and closes at the last, and a CR/LF pair, a fence
// in either spelling or a quoted control across a cut is written as it is
// written uncut. Pieces carry whole scalars, so cuts fall between them.
TEST(aPasteStreamedInAnyPiecesWritesWhatItWritesWhole)
{
    const std::array<std::string_view, 15> scalars { "a", "b", "\r", "\n", "\t", "\x1b", "[", "2", "0", "1", "~", "\x03", "\x7f", "\xc2\x9b", "\xe2\x82\xac" };
    const std::array<std::array<std::string_view, 6>, 2> fences { {
        { "\x1b", "[", "2", "0", "1", "~" },
        { "\xc2\x9b", "2", "0", "0", "~", "" },
    } };
    std::uint64_t state = 0x2545f4914f6cdd1dull;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (unsigned modes = 0; modes < 8; ++modes) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 24, 80);
        UbiquittyTest::feed(terminal, modes & 1 ? "\x1b[?2004h" : "\x1b[?2004l");
        UbiquittyTest::feed(terminal, modes & 2 ? "\x1b[?2005h" : "\x1b[?2005l");
        UbiquittyTest::feed(terminal, modes & 4 ? "\x1b[?2006h" : "\x1b[?2006l");
        REQUIRE(terminal.modes.bracketedPaste == ((modes & 1) != 0));
        REQUIRE(terminal.modes.shouldQuotePasteCharacters == ((modes & 2) != 0));
        REQUIRE(terminal.modes.shouldPreservePasteNewlines == ((modes & 4) != 0));
        for (int round = 0; round < 500; ++round) {
            std::string text;
            // Every offset a piece may end at: before and after each scalar.
            std::vector<std::size_t> boundaries { 0 };
            const auto append = [&](std::string_view scalar) {
                if (scalar.empty())
                    return;
                text += scalar;
                boundaries.push_back(text.size());
            };
            const std::size_t length = next() % 48;
            for (std::size_t index = 0; index < length; ++index) {
                if (next() % 8 == 0) {
                    for (const std::string_view scalar : fences[next() % fences.size()])
                        append(scalar);
                } else
                    append(scalars[next() % scalars.size()]);
            }
            std::vector<std::size_t> cuts;
            for (const std::size_t boundary : boundaries) {
                if (next() % 3 == 0)
                    cuts.push_back(boundary);
            }
            const std::string whole = pastedBytes(terminal, text, {});
            const std::string streamed = pastedBytes(terminal, text, cuts);
            CHECK_MSG(streamed == whole, ("modes " + std::to_string(modes) + ", round " + std::to_string(round) + ": " + std::to_string(cuts.size()) + " cuts wrote " + std::to_string(streamed.size()) + " bytes against " + std::to_string(whole.size())).c_str());
            if (streamed != whole)
                return;
            if (modes & 1) {
                CHECK(whole.starts_with("\x1b[200~"));
                CHECK(whole.ends_with("\x1b[201~"));
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
