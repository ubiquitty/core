// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// PasteNormalize.cpp — portable paste rewrite (CR/LF normalisation,
// bracketed-paste fence neutralisation). PAL-level abstraction shared by every
// frontend; declared in Core/Input/PasteNormalize.h.
//

#include "Core/Input/PasteNormalize.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace Ubiquitty::Input {

namespace {

constexpr bool isNewlinePair(unsigned char first, unsigned char second)
{
    return (first == '\r' && second == '\n') || (first == '\n' && second == '\r');
}

long normalizePasteBody(char* buffer, long length, BracketedPaste bracketedPaste, PasteOptions options)
{
    if (!buffer || length <= 0)
        return 0;
    long outputLength = 0;
    for (long i = 0; i < length;) {
        unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (c == '\r' || c == '\n') {
            if (options.shouldPreserveNewlines) {
                buffer[outputLength++] = static_cast<char>(c);
                ++i;
                continue;
            }
            buffer[outputLength++] = '\r';
            if (i + 1 < length) {
                unsigned char n = static_cast<unsigned char>(buffer[i + 1]);
                if (isNewlinePair(c, n)) {
                    i += 2;
                    continue;
                }
            }
            i += 1;
            continue;
        }
        // A pasted fence cannot open or close the receiving application's paste.
        if (c == 0x1B && i + 5 < length && buffer[i + 1] == '[' && buffer[i + 2] == '2' && buffer[i + 3] == '0' && (buffer[i + 4] == '0' || buffer[i + 4] == '1') && buffer[i + 5] == '~') {
            buffer[outputLength++] = 0x1B;
            buffer[outputLength++] = '[';
            buffer[outputLength++] = '2';
            buffer[outputLength++] = '0';
            buffer[outputLength++] = buffer[i + 4]; // the '0' or '1'
            i += 6; // drop the trailing '~'
            continue;
        }
        if (c == 0x9B && i + 4 < length && buffer[i + 1] == '2' && buffer[i + 2] == '0' && (buffer[i + 3] == '0' || buffer[i + 3] == '1') && buffer[i + 4] == '~') {
            buffer[outputLength++] = static_cast<char>(0x9B);
            buffer[outputLength++] = '2';
            buffer[outputLength++] = '0';
            buffer[outputLength++] = buffer[i + 3]; // the '0' or '1'
            i += 5; // drop the trailing '~'
            continue;
        }
        // Unquoted, unfenced controls would arrive as executable input.
        if (bracketedPaste == BracketedPaste::Disabled && !options.shouldQuoteCharacters && ((c < 0x20 && c != '\t' && c != 0x1b) || c == 0x7F)) {
            i += 1;
            continue;
        }
        if (bracketedPaste == BracketedPaste::Disabled && !options.shouldQuoteCharacters && (c == 0x90 || c == 0x98 || c == 0x9B || c == 0x9D || c == 0x9E || c == 0x9F)) {
            i += 1;
            continue;
        }
        buffer[outputLength++] = static_cast<char>(c);
        i += 1;
    }
    return outputLength;
}

std::string encodePasteBody(std::string body, BracketedPaste bracketedPaste, PasteOptions options)
{
    body.resize(static_cast<std::size_t>(normalizePasteBody(body.data(), static_cast<long>(body.size()), bracketedPaste, options)));
    if (!options.shouldQuoteCharacters)
        return body;
    if (body.size() > body.max_size() / 2)
        return {};
    const auto length = body.size();
    body.resize(length * 2);
    for (std::size_t index = length; index > 0; --index) {
        body[2 * index - 1] = body[index - 1];
        body[2 * index - 2] = '\x16';
    }
    return body;
}

} // namespace

long normalizePaste(char* buffer, long length, BracketedPaste bracketedPaste)
{
    return normalizePasteBody(buffer, length, bracketedPaste, {});
}

std::string wrapPaste(std::string_view payload, BracketedPaste bracketedPaste, PasteOptions options)
{
    auto body = encodePasteBody(std::string(payload), bracketedPaste, options);
    if (bracketedPaste == BracketedPaste::Enabled)
        return "\x1b[200~" + body + "\x1b[201~";
    return body;
}

namespace {

/// Whether bytes are the start of a bracketed-paste fence, in either
/// spelling, short of its end: the bytes after them decide whether it is one.
bool isFencePrefix(std::string_view bytes)
{
    constexpr std::string_view sevenBit { "\x1b[200~" };
    constexpr std::string_view eightBit { "\x9b"
                                          "200~" };
    const auto prefixOf = [&](std::string_view fence) {
        // The fifth byte is '0' or '1', the marker's last digit.
        if (bytes.size() >= fence.size())
            return false;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const bool isDigit = index == fence.size() - 2;
            if (isDigit ? (bytes[index] != '0' && bytes[index] != '1') : bytes[index] != fence[index])
                return false;
        }
        return true;
    };
    return !bytes.empty() && (prefixOf(sevenBit) || prefixOf(eightBit));
}

} // namespace

std::optional<PasteNormalizer::Continuation> PasteNormalizer::continuation() const
{
    if (m_held.size() > Continuation().held.size())
        return std::nullopt;
    Continuation state { .bracketedPaste = m_bracketedPaste, .options = m_options, .heldBytes = static_cast<std::uint8_t>(m_held.size()) };
    std::ranges::copy(m_held, state.held.begin());
    return isValidContinuation(state) ? std::optional(state) : std::nullopt;
}

bool PasteNormalizer::isValidContinuation(const Continuation& state)
{
    if ((state.bracketedPaste != BracketedPaste::Disabled && state.bracketedPaste != BracketedPaste::Enabled)
        || state.heldBytes > state.held.size())
        return false;
    const std::string_view held(state.held.data(), state.heldBytes);
    return held.empty() || held == "\r" || held == "\n" || isFencePrefix(held);
}

bool PasteNormalizer::restoreContinuation(const Continuation& state)
{
    if (!isValidContinuation(state))
        return false;
    std::string held(state.held.data(), state.heldBytes);
    m_held = std::move(held);
    m_bracketedPaste = state.bracketedPaste;
    m_options = state.options;
    return true;
}

std::string PasteNormalizer::push(std::string_view piece)
{
    std::string buffer = std::move(m_held);
    buffer.append(piece);
    m_held.clear();
    // What a later byte can change waits: a trailing CR or LF, which may pair,
    // or the start of a fence the next piece completes or breaks.
    std::size_t keep = buffer.size();
    // A trailing CR or LF waits only when it is single: CR LF and LF CR pair
    // greedily from the start of their run, and a last byte already paired
    // is settled.
    std::size_t run = buffer.size();
    while (run > 0 && (buffer[run - 1] == '\r' || buffer[run - 1] == '\n'))
        --run;
    bool isLastSingle = false;
    for (std::size_t index = run; index < buffer.size();) {
        const bool isPair = index + 1 < buffer.size() && isNewlinePair(buffer[index], buffer[index + 1]);
        isLastSingle = !isPair && index + 1 == buffer.size();
        index += isPair ? 2 : 1;
    }
    if (isLastSingle && !m_options.shouldPreserveNewlines)
        keep = buffer.size() - 1;
    for (std::size_t start = buffer.size() > 5 ? buffer.size() - 5 : 0; start < buffer.size(); ++start) {
        if (isFencePrefix(std::string_view(buffer).substr(start))) {
            keep = std::min(keep, start);
            break;
        }
    }
    m_held = buffer.substr(keep);
    buffer.resize(keep);
    return encodePasteBody(std::move(buffer), m_bracketedPaste, m_options);
}

std::string PasteNormalizer::finish()
{
    std::string held = std::move(m_held);
    m_held.clear();
    return encodePasteBody(std::move(held), m_bracketedPaste, m_options);
}

int pasteLineCount(std::string_view text)
{
    if (text.empty())
        return 0;
    int breaks = 0;
    bool isEndingOnBreak = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            ++breaks;
            if (i + 1 < text.size() && isNewlinePair(c, text[i + 1]))
                ++i;
            isEndingOnBreak = i + 1 == text.size();
        } else {
            isEndingOnBreak = false;
        }
    }
    return breaks + (isEndingOnBreak ? 0 : 1);
}

std::string shellEscape(std::string_view text)
{
    // Single-quote everything: inside '…' the shell interprets nothing, so the
    // only character needing care is the single quote itself, which can't appear
    // inside a single-quoted string. Close the quote, emit an escaped literal
    // quote (\'), reopen — the canonical '\'' idiom. An empty input becomes ''.
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('\'');
    for (char ch : text) {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

} // namespace Ubiquitty::Input
