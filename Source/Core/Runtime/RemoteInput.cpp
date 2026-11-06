// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/RemoteInput.h"

#include "Core/Mouse/MouseEvent.h"
#include "Core/Runtime/KeyEncode.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <string>
#include <variant>
#include <vector>

namespace Ubiquitty::Runtime {

namespace {

namespace Sync = Protocol::Sync;

bool isEncoderContinuationValid(const RemoteInputEncoder::Continuation& state)
{
    if (state.pasteCount > state.pastes.size())
        return false;
    for (std::size_t index = 0; index < state.pasteCount; ++index) {
        const auto& paste = state.pastes[index];
        if (paste.clientInstance == Sync::ID128 {} || !Input::PasteNormalizer::isValidContinuation(paste.normalizer)
            || paste.lastWriteBytes > (Sync::maxPasteChunkBytes + 5) * (paste.normalizer.options.shouldQuoteCharacters ? 2 : 1) + 12
            || paste.isBracketOpen != (paste.normalizer.bracketedPaste == Input::BracketedPaste::Enabled)
            || (paste.didOpen && !paste.isBracketOpen) || paste.didClose != (paste.isBracketOpen && paste.hasEnded)
            || paste.lastWriteBytes < (paste.didOpen ? 6u : 0u) + (paste.didClose ? 6u : 0u))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.pastes[previous].authority == paste.authority && state.pastes[previous].clientInstance == paste.clientInstance)
                return false;
        }
    }
    return true;
}

/// Decodes UTF-8 the wire already checked into at most `output.size()` code
/// points, returning how many it wrote.
int decodeText(std::string_view text, std::span<std::uint32_t> output)
{
    int count = 0;
    std::size_t index = 0;
    while (index < text.size() && static_cast<std::size_t>(count) < output.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2
            : lead < 0xF0                                ? 3
                                                         : 4;
        std::uint32_t codePoint = length == 1 ? lead : length == 2 ? lead & 0x1F
            : length == 3                                          ? lead & 0x0F
                                                                   : lead & 0x07;
        for (int continuation = 1; continuation < length && index + static_cast<std::size_t>(continuation) < text.size(); ++continuation)
            codePoint = (codePoint << 6) | (static_cast<std::uint8_t>(text[index + static_cast<std::size_t>(continuation)]) & 0x3F);
        output[static_cast<std::size_t>(count++)] = codePoint;
        index += static_cast<std::size_t>(length);
    }
    return count;
}

Mouse::EventType mouseEventTypeOf(Sync::PointerEventKind kind)
{
    switch (kind) {
    case Sync::PointerEventKind::Press:
        return Mouse::EventType::Press;
    case Sync::PointerEventKind::Release:
        return Mouse::EventType::Release;
    case Sync::PointerEventKind::Motion:
        return Mouse::EventType::Motion;
    }
    return Mouse::EventType::Motion;
}

/// Appends the bytes an intent encodes to under the terminal's live modes.
struct Encoder {
    const Ubiquitty::Terminal& terminal;
    std::vector<std::uint8_t>& bytes;

    void append(std::span<const char> encoded) { bytes.insert(bytes.end(), encoded.begin(), encoded.end()); }

    void operator()(const Sync::RawInput& raw) { bytes.insert(bytes.end(), raw.bytes.begin(), raw.bytes.end()); }

    void operator()(const Sync::KeyInput& keys)
    {
        std::array<char, 128> buffer {};
        for (const Sync::KeyEvent& event : keys.events) {
            const KeyEncoding encoding = encodeKey(terminal, keyPressOf(event), buffer);
            append({ buffer.data(), static_cast<std::size_t>(encoding.length) });
        }
    }

    void operator()(const Sync::PointerInput& pointer)
    {
        std::array<char, 64> buffer {};
        for (const Sync::PointerEvent& event : pointer.events) {
            const int pixelX = event.pixelX == Sync::noPointerPixel ? -1 : event.pixelX;
            const int pixelY = event.pixelY == Sync::noPointerPixel ? -1 : event.pixelY;
            const int length = encodeMouseReport(terminal, mouseEventTypeOf(event.kind), event.button, event.row, event.column, event.modifiers, buffer, pixelX, pixelY);
            append({ buffer.data(), static_cast<std::size_t>(length) });
        }
    }

    void operator()(const Sync::FocusInput& focus)
    {
        if (!terminal.modes.focusReporting)
            return;
        append(std::string_view(focus.focus == Modes::FocusState::Focused ? "\x1b[I" : "\x1b[O"));
    }

    // A paste's pieces are the encoder's to write, with their state.
    void operator()(const Sync::PasteInput&) { }
};

} // namespace

bool RemoteInputEncoder::isValidContinuation(const Continuation& state)
{
    return isEncoderContinuationValid(state);
}

std::optional<RemoteInputEncoder::Continuation> RemoteInputEncoder::continuation() const
{
    Continuation state;
    if (m_pastes.size() > state.pastes.size())
        return std::nullopt;
    for (const auto& paste : m_pastes) {
        const auto normalizer = paste.normalizer.continuation();
        if (!normalizer)
            return std::nullopt;
        state.pastes[state.pasteCount++] = {
            .clientInstance = paste.clientInstance,
            .authority = paste.authority,
            .normalizer = *normalizer,
            .lastWriteBytes = paste.lastWriteBytes,
            .isBracketOpen = paste.isBracketOpen,
            .didOpen = paste.didOpen,
            .didClose = paste.didClose,
            .hasEnded = paste.hasEnded,
        };
    }
    return isEncoderContinuationValid(state) ? std::optional(state) : std::nullopt;
}

bool RemoteInputEncoder::restoreContinuation(const Continuation& state)
{
    if (!m_pastes.empty() || !isEncoderContinuationValid(state))
        return false;
    std::vector<PasteInProgress> pastes;
    pastes.reserve(state.pasteCount);
    for (std::size_t index = 0; index < state.pasteCount; ++index) {
        const auto& paste = state.pastes[index];
        Input::PasteNormalizer normalizer(paste.normalizer.bracketedPaste, paste.normalizer.options);
        if (!normalizer.restoreContinuation(paste.normalizer))
            return false;
        pastes.push_back({
            .clientInstance = paste.clientInstance,
            .authority = paste.authority,
            .isBracketOpen = paste.isBracketOpen,
            .normalizer = std::move(normalizer),
            .lastWriteBytes = static_cast<std::size_t>(paste.lastWriteBytes),
            .didOpen = paste.didOpen,
            .didClose = paste.didClose,
            .hasEnded = paste.hasEnded,
        });
    }
    m_pastes = std::move(pastes);
    return true;
}

Sync::WriteStatus RemoteInputEncoder::write(Sync::ID128 clientInstance, const Sync::InputIntent& intent)
{
    return writeOwned({}, clientInstance, intent);
}

Sync::WriteStatus RemoteInputEncoder::writeOwned(Sync::ID128 authority, Sync::ID128 clientInstance, const Sync::InputIntent& intent)
{
    std::erase_if(m_pastes, [&](const PasteInProgress& paste) { return paste.hasEnded && m_sink.pendingBytes(paste.authority) == 0; });
    if (const auto* piece = std::get_if<Sync::PasteInput>(&intent))
        return writePaste(authority, clientInstance, *piece);
    std::vector<std::uint8_t> bytes;
    std::visit(Encoder { .terminal = m_terminal, .bytes = bytes }, intent);
    if (bytes.empty())
        return Sync::WriteStatus::Written;
    return m_sink.writeOwned(authority, clientInstance, bytes);
}

Sync::SignalEffect RemoteInputEncoder::signalEffect(const Sync::InputIntent& intent)
{
    // A paste's piece encodes to nothing here: its text is the paste
    // writer's, and signals nothing.
    std::vector<std::uint8_t> bytes;
    std::visit(Encoder { .terminal = m_terminal, .bytes = bytes }, intent);
    return m_sink.signalEffect(bytes);
}

Sync::WriteStatus RemoteInputEncoder::writeSignal(Sync::ID128 clientInstance, const Sync::InputIntent& intent, std::span<const Sync::ID128> endedPastes)
{
    return writeSignalOwned({}, clientInstance, intent, endedPastes);
}

Sync::WriteStatus RemoteInputEncoder::writeSignalOwned(Sync::ID128 authority, Sync::ID128 clientInstance, const Sync::InputIntent& intent, std::span<const Sync::ID128> endedPastes)
{
    const auto isEnded = [&](const PasteInProgress& paste) { return std::ranges::find(endedPastes, paste.clientInstance) != endedPastes.end(); };
    std::vector<std::uint8_t> bytes;
    constexpr std::string_view bracketEnd = "\x1b[201~";
    for (const PasteInProgress& paste : m_pastes) {
        if (paste.isBracketOpen && isEnded(paste))
            bytes.insert(bytes.end(), bracketEnd.begin(), bracketEnd.end());
    }
    std::visit(Encoder { .terminal = m_terminal, .bytes = bytes }, intent);
    const Sync::WriteStatus status = bytes.empty() ? Sync::WriteStatus::Written : m_sink.writeOwned(authority, clientInstance, bytes);
    if (status == Sync::WriteStatus::Written)
        std::erase_if(m_pastes, isEnded);
    return status;
}

Sync::WriteStatus RemoteInputEncoder::writePaste(Sync::ID128 authority, Sync::ID128 clientInstance, const Sync::PasteInput& piece)
{
    std::erase_if(m_pastes, [&](const PasteInProgress& paste) { return paste.hasEnded && m_sink.pendingBytes(paste.authority) == 0; });
    const auto found = std::ranges::find_if(m_pastes, [&](const PasteInProgress& paste) { return paste.authority == authority && paste.clientInstance == clientInstance; });
    // An end with nothing written before it opened nothing to close.
    if (found == m_pastes.end() && piece.end == Sync::PasteEnd::Aborted)
        return Sync::WriteStatus::Written;
    const bool isBracketed = m_terminal.modes.bracketedPaste;
    PasteInProgress next = found != m_pastes.end()
        ? *found
        : PasteInProgress { .clientInstance = clientInstance, .authority = authority, .isBracketOpen = false, .normalizer = Input::PasteNormalizer(isBracketed ? Input::BracketedPaste::Enabled : Input::BracketedPaste::Disabled, { .shouldQuoteCharacters = m_terminal.modes.shouldQuotePasteCharacters, .shouldPreserveNewlines = m_terminal.modes.shouldPreservePasteNewlines }) };
    if (found == m_pastes.end() && m_pastes.size() >= Sync::maxLastWrittenEntries)
        return Sync::WriteStatus::Refused;
    next.didOpen = false;
    next.didClose = false;
    std::string bytes;
    if (found == m_pastes.end() && isBracketed) {
        bytes = "\x1b[200~";
        next.isBracketOpen = true;
        next.didOpen = true;
    }
    if (piece.end != Sync::PasteEnd::Aborted)
        bytes += next.normalizer.push({ reinterpret_cast<const char*>(piece.text.data()), piece.text.size() });
    if (piece.end == Sync::PasteEnd::Final)
        bytes += next.normalizer.finish();
    if (piece.end != Sync::PasteEnd::More && next.isBracketOpen) {
        bytes += "\x1b[201~";
        next.didClose = true;
    }
    const std::span<const std::uint8_t> encoded { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() };
    const Sync::WriteStatus status = encoded.empty() ? Sync::WriteStatus::Written : m_sink.writePasteOwned(authority, clientInstance, encoded);
    if (status != Sync::WriteStatus::Written)
        return status;
    next.lastWriteBytes = bytes.size();
    next.hasEnded = piece.end != Sync::PasteEnd::More;
    if (found != m_pastes.end()) {
        *found = std::move(next);
    } else {
        m_pastes.push_back(std::move(next));
    }
    return status;
}

void RemoteInputEncoder::withdraw(Sync::ID128 authority)
{
    constexpr std::string_view start = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";
    std::array<char, 12> repair {};
    std::size_t repairBytes = 0;
    const auto append = [&](std::string_view bytes) {
        std::ranges::copy(bytes, repair.begin() + static_cast<std::ptrdiff_t>(repairBytes));
        repairBytes += bytes.size();
    };
    const auto found = std::ranges::find(m_pastes, authority, &PasteInProgress::authority);
    if (found != m_pastes.end() && found->isBracketOpen) {
        const std::size_t pending = std::min(found->lastWriteBytes, m_sink.pendingBytes(authority));
        const std::size_t delivered = found->lastWriteBytes - pending;
        if (found->didOpen && delivered < start.size()) {
            if (delivered) {
                append(start.substr(delivered));
                append(end);
            }
        } else if (found->didClose) {
            const std::size_t closingAt = found->lastWriteBytes - end.size();
            append(end.substr(delivered > closingAt ? delivered - closingAt : 0));
        } else {
            append(end);
        }
    }
    m_sink.withdraw(authority, std::span(repair).first(repairBytes));
    std::erase_if(m_pastes, [&](const PasteInProgress& paste) { return paste.authority == authority; });
}

KeyPress keyPressOf(const Sync::KeyEvent& event)
{
    KeyPress press;
    press.virtualKey = static_cast<Keys::VirtualKey>(event.key);
    press.modifiers = event.modifiers;
    press.unshiftedCodePoint = event.unshiftedCodePoint;
    press.shiftedCodePoint = event.shiftedCodePoint;
    press.baseLayoutCodePoint = event.baseLayoutCodePoint;
    press.textLength = decodeText(event.text, press.text);
    press.eventType = event.kind == Sync::KeyEventKind::Release ? Keys::KeyEventType::Release : Keys::KeyEventType::Press;
    press.isRepeat = event.kind == Sync::KeyEventKind::Repeat;
    press.wasModified = event.wasModified;
    return press;
}

} // namespace Ubiquitty::Runtime
