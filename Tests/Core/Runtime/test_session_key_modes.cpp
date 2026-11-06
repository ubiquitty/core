// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Runtime/TerminalSession.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <string>

TEST(sessionKeysObserveProtocolResetBeforeSnapshotPublication)
{
    using namespace Ubiquitty;
    const std::unique_ptr<Runtime::TerminalSession, decltype(&releaseTerminalSession)> session(
        Runtime::TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    const std::string enable = "\x1b[>7u";
    session->feedBytes(Runtime::ByteSpan(enable.data(), enable.size()));
    REQUIRE(session->readSnapshot().modes.kittyKeyboardFlags[0] == 7);

    Runtime::KeyPress press;
    press.unshiftedCodePoint = 'c';
    press.shiftedCodePoint = 'c';
    press.wasModified = true;
    press.modifiers = Keys::Keymap::keymapModifierCtrl;
    press.eventType = Keys::KeyEventType::Release;
    char buffer[64];
    const Keys::Keymap::EncodedKeyBytes output { buffer, sizeof(buffer) };
    CHECK(session->encodeKey(press, output).length > 0);

    session->lockForRead();
    processBytes(session->terminal(), std::string_view("\x1b[<u"));
    session->unlockForRead();
    REQUIRE(session->readSnapshot().modes.kittyKeyboardFlags[0] == 7);
    CHECK(session->encodeKey(press, output).length == 0);
    press.eventType = Keys::KeyEventType::Press;
    CHECK(session->encodeKey(press, output).length == 1);
    CHECK(buffer[0] == '\x03');
}

TEST(sessionKeysSuppressPasswordEchoBeforeSnapshotPublication)
{
    using namespace Ubiquitty;
    const std::unique_ptr<Runtime::TerminalSession, decltype(&releaseTerminalSession)> session(
        Runtime::TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    const std::string localEcho = "\x1b[12l";
    session->feedBytes(Runtime::ByteSpan(localEcho.data(), localEcho.size()));

    Runtime::KeyPress press;
    press.unshiftedCodePoint = 'x';
    press.shiftedCodePoint = 'x';
    press.eventType = Keys::KeyEventType::Press;
    char buffer[64];
    const Keys::Keymap::EncodedKeyBytes output { buffer, sizeof(buffer) };
    REQUIRE(session->encodeKey(press, output).shouldEchoLocally);
    session->lockForRead();
    session->terminal()->passwordPromptActive = true;
    session->unlockForRead();
    REQUIRE(!session->readSnapshot().modes.isPasswordPromptActive);
    const auto encoding = session->encodeKey(press, output);
    CHECK(encoding.length == 1);
    CHECK(buffer[0] == 'x');
    CHECK(!encoding.shouldEchoLocally);
}

UBIQUITTY_TEST_MAIN()

TEST(embeddedFeedPublishesStatusAndBellBeforeEventDelivery)
{
    using namespace Ubiquitty;
    const std::unique_ptr<Runtime::TerminalSession, decltype(&releaseTerminalSession)> session(
        Runtime::TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->bellMode = BellMode::Visual;
    session->terminal()->bellRaisesUrgency = true;
    (void)session->takeEventFlags();
    const std::string bytes = "\x1b]21337;indicator=#5f87ff;status=Waiting\x07\x07";
    session->feedBytes(Runtime::ByteSpan(bytes.data(), bytes.size()));
    const auto events = session->takeEventFlags();
    CHECK(events & (1u << static_cast<unsigned>(Runtime::EventKind::Bell)));
    CHECK(events & (1u << static_cast<unsigned>(Runtime::EventKind::TabStatusChanged)));
    CHECK(session->readSnapshot().tabStatusLabel() == "Waiting");
    CHECK(session->lastBellCue().mode == BellMode::Visual);
    CHECK(session->lastBellCue().shouldRaiseUrgency);
    const std::string clear = "\x1b]21337;indicator=;status=\x07";
    session->feedBytes(Runtime::ByteSpan(clear.data(), clear.size()));
    CHECK(session->takeEventFlags() & (1u << static_cast<unsigned>(Runtime::EventKind::TabStatusChanged)));
    CHECK(session->readSnapshot().tabStatusLabel().empty());
}

TEST(sessionPasteEventsPrecedeBracketsAndRetainReadPolicy)
{
    using namespace Ubiquitty;
    const std::unique_ptr<Runtime::TerminalSession, decltype(&releaseTerminalSession)> session(
        Runtime::TerminalSession::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    std::string input;
    session->setReplySink([](void* context, std::string_view bytes) {
        static_cast<std::string*>(context)->append(bytes);
    },
        &input);
    session->lockForRead();
    processBytes(session->terminal(), "\033[?2004;5522h");
    session->unlockForRead();
    CHECK(session->sendPasteNotification("text/plain image/png", ClipboardBuffer::Primary));
    CHECK(input == OSC::clipboardPasteNotification("text/plain image/png", ClipboardBuffer::Primary));
    input.clear();
    CHECK(session->sendPaste("clipboard contents").empty());
    CHECK(input == OSC::clipboardPasteNotification("text/plain", ClipboardBuffer::Clipboard));
    input.clear();
    session->terminal()->osc52ClipboardReadPolicy = OSC::ClipboardReadPolicy::Deny;
    CHECK(session->sendPasteNotification("image/png"));
    CHECK(input == "\033]5522;type=read:status=EPERM\033\\");
    input.clear();
    session->lockForRead();
    processBytes(session->terminal(), "\033[?5522l");
    session->unlockForRead();
    CHECK(!session->sendPasteNotification("text/plain"));
    CHECK(session->sendPaste("pasted") == "\033[200~pasted\033[201~");
    CHECK(input == "\033[200~pasted\033[201~");
    session->setReplySink(nullptr, nullptr);
}
