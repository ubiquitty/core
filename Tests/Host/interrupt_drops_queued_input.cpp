// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/interrupt_drops_queued_input.cpp — the daemon reads what a
// signal character does from the pane's line discipline (Docs/sync.md §6.7).
//
// Two clients drive one pane whose program reads nothing. The first types a
// line, the second presses Ctrl-C. With signals on and the input discarded at
// one, the line is dropped, the first client is told, and the program is
// written the key alone. With the input kept (NOFLSH) the key is written
// first and the line after it. With signals off the key is data, written in
// its turn, and the echo context names no interrupt character. A pane with
// no line discipline to read, a pipe, treats the key as data.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Keys/Keymap.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// The line the first client types: longer than a key costs the queue, so
/// the bytes held tell the line from the key.
std::string typedLine()
{
    return "rm -rf " + std::string(96, 'b') + "\r";
}

/// What one pane's program was written, and what its first client was told.
struct Outcome {
    /// What the program was written once it read again.
    std::string written;
    /// The pane's unwritten input, in bytes, after the key arrived.
    std::size_t heldAfterKey = 0;
    /// The `InputDiscarded` frames the first client was sent.
    std::size_t discardsTold = 0;
    /// The first client's count of its input written, as the host reported.
    std::uint64_t firstInputWritten = 0;
    /// Whether the pane's echo context names an interrupt character.
    bool hasInterruptCharacter = false;
    /// Whether the first client holds an echo context for the pane at all.
    bool hasEchoContext = false;
    bool areBothAttached = false;
};

/// Types a line from one client and Ctrl-C from another into a program that
/// reads nothing, then lets the program read.
///
/// - Parameters:
///   - discipline: The line discipline the PTY reports, or none.
///   - signalByte: The byte the second client sends in place of the Ctrl-C
///     key, or none for the key.
/// - Returns: What happened.
Outcome run(std::optional<Ubiquitty::PAL::LineDiscipline> discipline, std::optional<std::uint8_t> signalByte = std::nullopt)
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    pty.facts.discipline = discipline;
    pty.acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const auto attach = [&](const char* token, UbiquittyTest::ScreenClient& client) {
        const std::size_t index = Screen::attach(d, session.id);
        Screen::grantScope(d, index, driving);
        Screen::setTokenID(d, index, token);
        Screen::markInteractiveUser(d, index);
        Screen::greet(d, index, client);
        return index;
    };
    UbiquittyTest::ScreenClient first(24, 80);
    UbiquittyTest::ScreenClient second(24, 80, Sync::ID128 { .low = 3, .high = 4 });
    const std::size_t firstIndex = attach("first", first);
    const std::size_t secondIndex = attach("second", second);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const auto deliver = [&] {
        for (int rounds = 0; rounds < 8; ++rounds) {
            Screen::deliver(d, firstIndex, first);
            Screen::deliver(d, secondIndex, second);
        }
    };
    deliver();

    const std::string line = typedLine();
    CHECK(first.sendRaw({ reinterpret_cast<const std::uint8_t*>(line.data()), line.size() }).has_value());
    deliver();
    CHECK_MSG(session.screenSource->hasPendingInput(), "premise: the line was written to a program that reads nothing");
    const std::array<Sync::KeyEvent, 1> interrupt { Sync::KeyEvent { .key = 0, .modifiers = Ubiquitty::Keys::Keymap::keymapModifierCtrl, .kind = Sync::KeyEventKind::Press, .text = {}, .unshiftedCodePoint = 'c', .shiftedCodePoint = 'c', .baseLayoutCodePoint = 0, .wasModified = true } };
    if (signalByte)
        CHECK(second.sendRaw({ &*signalByte, 1 }).has_value());
    else
        CHECK(second.sendKeys(interrupt).has_value());
    deliver();
    Outcome outcome;
    outcome.heldAfterKey = session.screenSource->heldInputBytes();

    pty.allowWrites();
    session.screenSource->drainInput(Screen::now());
    // A frame carries the echo context, and the receipt delay passes, so a
    // count with no update to carry it is sent.
    Ubiquitty::processBytes(session.term.get(), "*");
    session.isFrameDue = true;
    for (int rounds = 0; rounds < 40; ++rounds) {
        if (session.isFrameDue)
            Screen::publish(d, session);
        deliver();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    outcome.written = pty.written;
    outcome.discardsTold = first.count(Sync::FrameKind::InputDiscarded);
    outcome.firstInputWritten = first.connection().receipt(1).inputWritten;
    const std::optional<Sync::EchoContext> echo = first.connection().echoContext(1);
    outcome.hasEchoContext = echo.has_value();
    outcome.hasInterruptCharacter = echo && echo->interruptCharacters.interrupt.has_value();
    outcome.areBothAttached = Screen::connection(d, firstIndex) && Screen::connection(d, firstIndex)->state() == Sync::ConnectionState::Attached
        && Screen::connection(d, secondIndex) && Screen::connection(d, secondIndex)->state() == Sync::ConnectionState::Attached;
    return outcome;
}

/// Text with its control characters spelled out, for a failure's message.
std::string visible(const std::string& text)
{
    std::string shown;
    for (const char byte : text) {
        if (static_cast<unsigned char>(byte) < 0x20) {
            shown += '^';
            shown += static_cast<char>(byte + 0x40);
        } else {
            shown += byte;
        }
    }
    return shown;
}

/// What a terminal does at a signal character.
enum class AtSignal : std::uint8_t {
    /// Signals are off: the character is data.
    Nothing,
    /// It signals and keeps the input it holds unread (NOFLSH).
    SignalsKeepingInput,
    /// It signals and discards the input it holds unread.
    SignalsDiscardingInput,
};

/// A line discipline with the default characters.
///
/// - Parameter atSignal: What it does at one of them.
/// - Returns: The discipline.
Ubiquitty::PAL::LineDiscipline disciplineWith(AtSignal atSignal)
{
    return { .isEchoEnabled = true, .isCanonical = true, .isSignalEnabled = atSignal != AtSignal::Nothing, .isInputDiscardedOnSignal = atSignal != AtSignal::SignalsKeepingInput, .interruptCharacter = 0x03, .quitCharacter = 0x1c, .suspendCharacter = 0x1a };
}

} // namespace

int main()
{
    const std::string line = typedLine();

    const Outcome discarding = run(disciplineWith(AtSignal::SignalsDiscardingInput));
    CHECK_MSG(discarding.written == "\x03", ("a terminal that discards its input at a signal was written \"" + visible(discarding.written) + "\"").c_str());
    CHECK_MSG(discarding.heldAfterKey < line.size(), "the line stayed queued behind an interrupt that discards the input");
    CHECK_MSG(discarding.discardsTold == 1, ("the first client was told of its dropped line " + std::to_string(discarding.discardsTold) + " times").c_str());
    CHECK_MSG(discarding.firstInputWritten == 1, "the first client's count did not move past its dropped line");
    CHECK_MSG(discarding.hasInterruptCharacter, "the echo context names no interrupt character with signals on");
    CHECK_MSG(discarding.areBothAttached, "a client was closed");

    // The quit and suspend characters signal as the interrupt does, and a
    // byte that is none of them is data.
    for (const std::uint8_t byte : { std::uint8_t { 0x1c }, std::uint8_t { 0x1a } }) {
        const Outcome other = run(disciplineWith(AtSignal::SignalsDiscardingInput), byte);
        CHECK_MSG(other.written == std::string(1, static_cast<char>(byte)), ("a signal character other than the interrupt left \"" + visible(other.written) + "\"").c_str());
        CHECK_MSG(other.discardsTold == 1, "the first client was not told of a line a quit or suspend dropped");
    }
    const Outcome plain = run(disciplineWith(AtSignal::SignalsDiscardingInput), std::uint8_t { 0x04 });
    CHECK_MSG(plain.written == line + "\x04", ("a byte that signals nothing left \"" + visible(plain.written) + "\"").c_str());
    CHECK_MSG(plain.discardsTold == 0, "a client was told of a discard a plain byte made");

    const Outcome keeping = run(disciplineWith(AtSignal::SignalsKeepingInput));
    CHECK_MSG(keeping.written == "\x03" + line, ("a terminal that keeps its input at a signal was written \"" + visible(keeping.written) + "\"").c_str());
    CHECK_MSG(keeping.heldAfterKey >= line.size(), "the line was dropped under NOFLSH");
    CHECK_MSG(keeping.discardsTold == 0, "a client was told of a discard under NOFLSH");
    CHECK_MSG(keeping.areBothAttached, "a client was closed");

    const Outcome data = run(disciplineWith(AtSignal::Nothing));
    CHECK_MSG(data.written == line + "\x03", ("a terminal with signals off was written \"" + visible(data.written) + "\"").c_str());
    CHECK_MSG(data.discardsTold == 0, "a client was told of a discard with signals off");
    CHECK_MSG(data.hasEchoContext && !data.hasInterruptCharacter, "the echo context is absent, or names an interrupt character with signals off");

    const Outcome pipe = run(std::nullopt);
    CHECK_MSG(pipe.written == line + "\x03", ("a pane with no line discipline was written \"" + visible(pipe.written) + "\"").c_str());
    CHECK_MSG(pipe.discardsTold == 0, "a client was told of a discard on a pane with no line discipline");

    if (failures == 0)
        std::printf("interrupt_drops_queued_input: an interrupt drops the pane's queued input where the terminal discards its own and tells its sender, goes first and drops nothing under NOFLSH, and is data with signals off\n");
    return failures == 0 ? 0 : 1;
}
