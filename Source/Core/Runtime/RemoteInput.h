// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Runtime/RemoteInput.h — a remote client's input, encoded on the host under
// the terminal's modes at the moment it is written (Docs/sync.md §6.7).
//
// A client sends what the user did: keys, pointer events the pointer ladder
// routed to the program, focus. The bytes a program reads depend on modes the
// program sets — DECCKM, the mouse encodings, focus reporting — and a mode can
// change between the client sending an event and the host writing it. So the
// host encodes an event at the write, never when it arrives, and a write the
// program refused is encoded again when it is retried.
//
// The encoding lives here and not in Sync: Runtime already reaches the wire,
// and the wire does not reach the key encoder.
//

#include "Core/Input/PasteNormalize.h"
#include "Core/Input/PointerLadder.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Runtime/KeyPress.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Runtime {

/// Where a pane's encoded input goes: the program's input, whole or not at all.
class ByteSink {
public:
    virtual ~ByteSink() = default;

    /// Writes bytes to the program.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose input they are, whose authority
    ///     the program's resulting output is held to.
    ///   - bytes: The bytes, never empty.
    /// - Returns: Whether they were written whole, refused whole, or the
    ///   program is gone.
    [[nodiscard]] virtual Protocol::Sync::WriteStatus writeAll(Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes) = 0;

    /// Writes bytes with the exact attachment that admitted them.
    /// - Parameters:
    ///   - authority: Server attachment identity, or zero for system input.
    ///   - clientInstance: Replay identity.
    ///   - bytes: Encoded bytes.
    /// - Returns: The sink outcome.
    [[nodiscard]] virtual Protocol::Sync::WriteStatus writeOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes)
    {
        (void)authority;
        return writeAll(clientInstance, bytes);
    }

    /// Writes a paste chunk on a distinct owned lane from signal bypasses.
    /// - Parameters:
    ///   - authority: Original attachment identity.
    ///   - clientInstance: Replay identity.
    ///   - bytes: Exact encoded chunk, including any delimiters.
    /// - Returns: The sink outcome.
    [[nodiscard]] virtual Protocol::Sync::WriteStatus writePasteOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes)
    {
        return writeOwned(authority, clientInstance, bytes);
    }

    /// Counts an attachment's paste bytes still queued below the encoder.
    /// - Parameter authority: The original attachment.
    /// - Returns: Undelivered bytes; synchronous sinks retain none.
    [[nodiscard]] virtual std::size_t pendingBytes(Protocol::Sync::ID128 authority) const
    {
        (void)authority;
        return 0;
    }

    /// Cancels an attachment's user-space suffix and emits only delimiter repair.
    /// - Parameters:
    ///   - authority: The withdrawn attachment.
    ///   - repair: Host-generated paste delimiters, never revoked payload.
    virtual void withdraw(Protocol::Sync::ID128 authority, std::span<const char> repair)
    {
        (void)authority;
        (void)repair;
    }

    /// What the program's terminal would do with bytes written now: whether
    /// one of them is a character its line discipline answers with a signal,
    /// and whether taking it discards the input waiting unread.
    ///
    /// - Parameter bytes: The bytes; none signal nothing.
    /// - Returns: The effect; `None` from a sink with no terminal to ask.
    [[nodiscard]] virtual Protocol::Sync::SignalEffect signalEffect(std::span<const std::uint8_t> bytes)
    {
        (void)bytes;
        return Protocol::Sync::SignalEffect::None;
    }
};

/// Encodes a pane's remote input under its terminal's modes as each event is
/// written.
class RemoteInputEncoder final : public Protocol::Sync::InputWriter {
public:
    /// Exact paste encoder state, separate from the sink's accepted byte queue.
    struct Continuation {
        /// One original attachment's paste and the last accepted piece's repair data.
        struct Paste {
            Protocol::Sync::ID128 clientInstance;
            Protocol::Sync::ID128 authority;
            Input::PasteNormalizer::Continuation normalizer;
            std::uint64_t lastWriteBytes = 0;
            bool isBracketOpen = false;
            bool didOpen = false;
            bool didClose = false;
            bool hasEnded = false;
            bool operator==(const Paste&) const = default;
        };
        std::array<Paste, Protocol::Sync::maxLastWrittenEntries> pastes {};
        std::uint8_t pasteCount = 0;
        bool operator==(const Continuation&) const = default;
    };

    /// Copies paste order, held normalization bytes and exact withdrawal state.
    /// - Returns: Bounded state, or nothing if an owner cannot be represented.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Checks paste identities, normalization and withdrawal state without a sink.
    /// - Parameter state: The bounded active prefix to validate.
    /// - Returns: Whether the candidate is internally consistent.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state);

    /// Installs a candidate on a fresh encoder without writing or querying its sink.
    /// The caller restores the matching terminal modes, input queue and accepted
    /// sink suffix in the same transaction before allowing input or withdrawal.
    /// Terminal and sink references remain those supplied to this encoder.
    /// - Parameter state: The exact candidate, including ended but undrained pastes.
    /// - Returns: True on success; malformed or occupied refusal changes nothing.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    /// Creates the encoder of a pane.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, whose modes each write reads; it must
    ///     outlive the encoder.
    ///   - sink: Where the bytes go; it must outlive the encoder.
    RemoteInputEncoder(const Ubiquitty::Terminal& terminal, ByteSink& sink)
        : m_terminal(terminal)
        , m_sink(sink)
    {
    }

    /// Encodes one event and writes it: keys through the key encoder, pointer
    /// events through the mouse encoder, a focus change as `CSI I` or `CSI O`
    /// under DECSET 1004. An event that encodes to nothing under the live
    /// modes — a focus change with focus reporting off, a pointer event with no
    /// mouse mode — is written as nothing.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose event it is.
    ///   - intent: The event.
    /// - Returns: What the sink said, or `Written` for nothing to write.
    [[nodiscard]] Protocol::Sync::WriteStatus write(Protocol::Sync::ID128 clientInstance, const Protocol::Sync::InputIntent& intent) override;

    /// Encodes an event under its original attachment's authority.
    /// - Parameters:
    ///   - authority: Server attachment identity.
    ///   - clientInstance: Replay identity.
    ///   - intent: The event.
    /// - Returns: The sink's outcome.
    [[nodiscard]] Protocol::Sync::WriteStatus writeOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, const Protocol::Sync::InputIntent& intent) override;

    /// Cancels the exact attachment, repairing any delivered paste delimiter.
    /// - Parameter authority: The retired attachment.
    void withdraw(Protocol::Sync::ID128 authority) override;

    /// Asks the sink what the event's bytes, encoded under the live modes,
    /// would do at the program's terminal. A paste's text signals nothing, and
    /// neither does an event that encodes to nothing.
    ///
    /// - Parameter intent: The event.
    /// - Returns: The effect.
    [[nodiscard]] Protocol::Sync::SignalEffect signalEffect(const Protocol::Sync::InputIntent& intent) override;

    /// Writes an event that signals behind the end markers of the pastes it
    /// ended, as one write: each such paste whose bracket is open is closed.
    /// The pastes' state moves only when the write is taken.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose event it is.
    ///   - intent: The event.
    ///   - endedPastes: The clients whose pastes in progress the event ended.
    /// - Returns: What the sink said, or `Written` for nothing to write.
    [[nodiscard]] Protocol::Sync::WriteStatus writeSignal(Protocol::Sync::ID128 clientInstance, const Protocol::Sync::InputIntent& intent, std::span<const Protocol::Sync::ID128> endedPastes) override;

    /// Writes a signaling event under its original attachment.
    /// - Parameters:
    ///   - authority: Server attachment identity.
    ///   - clientInstance: Replay identity.
    ///   - intent: The event.
    ///   - endedPastes: Replay identities whose pastes the signal ends.
    /// - Returns: The sink's outcome.
    [[nodiscard]] Protocol::Sync::WriteStatus writeSignalOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, const Protocol::Sync::InputIntent& intent, std::span<const Protocol::Sync::ID128> endedPastes) override;

private:
    /// A client's paste being written: whether its bracket is open, and the
    /// normaliser that carries CR/LF pairs and fences across its pieces.
    struct PasteInProgress {
        Protocol::Sync::ID128 clientInstance;
        Protocol::Sync::ID128 authority;
        bool isBracketOpen = false;
        Input::PasteNormalizer normalizer;
        std::size_t lastWriteBytes = 0;
        bool didOpen = false;
        bool didClose = false;
        bool hasEnded = false;
    };

    /// Writes a piece of a paste. The bracket opens at the first piece when
    /// DECSET 2004 is live then, and closes at the end, final or aborted,
    /// only if it opened. The state moves only when the write is taken, so
    /// a refused piece is encoded again the same way.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose paste it is.
    ///   - authority: The original server attachment.
    ///   - piece: The piece.
    /// - Returns: What the sink said.
    [[nodiscard]] Protocol::Sync::WriteStatus writePaste(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, const Protocol::Sync::PasteInput& piece);

    const Ubiquitty::Terminal& m_terminal;
    ByteSink& m_sink;
    std::vector<PasteInProgress> m_pastes;
};

/// What a client sends for a key press: the facts the host's encoder reads.
///
/// - Parameter press: The press, as the frontend reported it.
/// - Returns: The key event.
[[nodiscard]] inline Protocol::Sync::KeyEvent keyEventOf(const KeyPress& press)
{
    Protocol::Sync::KeyEvent event;
    event.key = static_cast<std::uint32_t>(press.virtualKey);
    event.modifiers = static_cast<std::uint16_t>(press.modifiers);
    event.kind = press.eventType == Keys::KeyEventType::Release           ? Protocol::Sync::KeyEventKind::Release
        : press.isRepeat || press.eventType == Keys::KeyEventType::Repeat ? Protocol::Sync::KeyEventKind::Repeat
                                                                          : Protocol::Sync::KeyEventKind::Press;
    for (int index = 0; index < press.textLength && index < Keys::maxKeyEventTextLength; ++index)
        Unicode::appendUTF8(event.text, press.text[index]);
    event.unshiftedCodePoint = press.unshiftedCodePoint;
    event.shiftedCodePoint = press.shiftedCodePoint;
    event.baseLayoutCodePoint = press.baseLayoutCodePoint;
    event.wasModified = press.wasModified;
    return event;
}

/// What a client sends for a pointer event its ladder reported to the
/// program: the facts the ladder encoded its own report from.
///
/// - Parameter reported: Those facts.
/// - Returns: The pointer event.
[[nodiscard]] inline Protocol::Sync::PointerEvent pointerEventOf(const Input::PointerRoute::Reported& reported)
{
    const auto saturated = [](int value) { return static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFE)); };
    Protocol::Sync::PointerEventKind kind = Protocol::Sync::PointerEventKind::Motion;
    if (reported.type == Mouse::EventType::Press)
        kind = Protocol::Sync::PointerEventKind::Press;
    else if (reported.type == Mouse::EventType::Release)
        kind = Protocol::Sync::PointerEventKind::Release;
    return { .kind = kind,
        .button = static_cast<std::uint8_t>(reported.buttonCode),
        .modifiers = static_cast<std::uint16_t>(reported.modifiers),
        .row = saturated(reported.row),
        .column = saturated(reported.column),
        .pixelX = reported.pixelX < 0 ? Protocol::Sync::noPointerPixel : saturated(reported.pixelX),
        .pixelY = reported.pixelY < 0 ? Protocol::Sync::noPointerPixel : saturated(reported.pixelY) };
}

/// The key press a host encodes from a client's key event.
///
/// - Parameter event: The event.
/// - Returns: The press.
[[nodiscard]] KeyPress keyPressOf(const Protocol::Sync::KeyEvent& event);

} // namespace Ubiquitty::Runtime
