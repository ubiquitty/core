// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// ReplyBuilder — the fixed-frame writer every host-facing reply is assembled
// with.
//
// A reply is a handful of literal bytes around one or two decimal numbers, and
// reaching that through the printf family costs more than everything else
// answering the query does. Programs probe the terminal at startup to learn
// what they are running under, so every one of them waits on this path.
//
// The frame is fixed and the writer clamps: a reply that does not fit is
// reported truncated and the caller drops it, because half an escape sequence
// is not a shorter answer but a different one — the host reads whatever
// follows as part of it.

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace Ubiquitty {

/// Assembles one host-facing reply in a fixed frame.
///
/// Callers append the literal bytes and numbers of the answer and then send
/// ``span()``, but only while ``isSendable()`` holds: an answer that overruns
/// the frame is reported truncated rather than sent short, because the host
/// reads whatever follows a partial escape sequence as part of it.
class ReplyBuilder {
public:
    /// Appends one byte, or records truncation when the frame is full.
    ///
    /// - Parameter character: The byte to append.
    void put(char character)
    {
        if (m_length >= sizeof(m_bytes)) {
            m_isTruncated = true;
            return;
        }
        m_bytes[m_length] = character;
        ++m_length;
    }

    /// Appends every byte of `text`.
    ///
    /// The frame is measured once for the whole run rather than once per byte,
    /// and a run that does not fit is dropped whole. Callers send only while
    /// ``isSendable()`` holds, so bytes appended past the point of truncation
    /// are never read either way, and dropping the run keeps the bound off the
    /// copy.
    ///
    /// - Parameter text: The bytes to append.
    void put(std::string_view text)
    {
        const std::size_t count = text.size();
        if (!count) {
            return;
        }
        if (count > sizeof(m_bytes) - m_length) {
            m_isTruncated = true;
            return;
        }
        std::memcpy(m_bytes + m_length, text.data(), count);
        m_length += count;
    }

    /// Appends a literal, whose length the compiler knows.
    ///
    /// A reply body is mostly short fixed runs, and a run of known length is a
    /// store or two where the general form is a call.
    ///
    /// - Parameter text: The literal to append, its terminator excluded.
    template<std::size_t textSize>
    void put(const char (&text)[textSize])
    {
        static_assert(textSize > 0, "a literal carries its terminator");
        constexpr std::size_t count = textSize - 1;
        if (count > sizeof(m_bytes) - m_length) {
            m_isTruncated = true;
            return;
        }
        std::memcpy(m_bytes + m_length, text, count);
        m_length += count;
    }

    /// Appends `value` in decimal, with a leading minus when negative.
    ///
    /// - Parameter value: The number to render.
    void putDecimal(int value)
    {
        unsigned magnitude = static_cast<unsigned>(value);
        if (value < 0) {
            put('-');
            magnitude = static_cast<unsigned>(-(value + 1)) + 1;
        }
        char digits[std::numeric_limits<unsigned>::digits10 + 1];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0 && count < static_cast<int>(sizeof(digits)));
        while (count > 0) {
            put(digits[--count]);
        }
    }

    /// Appends `value` as exactly four lowercase hex digits.
    ///
    /// - Parameter value: The component to render.
    void putHex16(std::uint16_t value)
    {
        static constexpr char hexDigits[] = "0123456789abcdef";
        put(hexDigits[(value >> 12) & 0xF]);
        put(hexDigits[(value >> 8) & 0xF]);
        put(hexDigits[(value >> 4) & 0xF]);
        put(hexDigits[value & 0xF]);
    }

    /// Appends `;rgb:RRRR/GGGG/BBBB`, the colour body every colour reply carries.
    ///
    /// - Parameters:
    ///   - red: The red component.
    ///   - green: The green component.
    ///   - blue: The blue component.
    void putColor(std::uint16_t red, std::uint16_t green, std::uint16_t blue)
    {
        put(";rgb:");
        putHex16(red);
        put('/');
        putHex16(green);
        put('/');
        putHex16(blue);
    }

    /// Discards everything appended so far, and the truncation with it.
    ///
    /// A reply whose opening depends on whether the body could be assembled is
    /// written by opening optimistically and starting over on the answer that
    /// it could not — which keeps the body where it is going instead of
    /// assembling it somewhere else and copying it in.
    void reset()
    {
        m_length = 0;
        m_isTruncated = false;
    }

    /// Whether a byte was dropped because the frame was full.
    [[nodiscard]] bool isTruncated() const { return m_isTruncated; }

    /// How many bytes have been appended.
    [[nodiscard]] int length() const { return static_cast<int>(m_length); }

    /// Whether the reply is complete and worth sending.
    [[nodiscard]] bool isSendable() const { return !m_isTruncated && m_length > 0; }

    /// The bytes appended so far.
    [[nodiscard]] std::span<const char> span() const { return { m_bytes, m_length }; }

private:
    // Sized for the longest reply the terminal makes, the DECRQSS report of
    // DECPS's bounds at sixty-six bytes, with room to grow.
    char m_bytes[96];
    // Every append preserves m_length <= sizeof(m_bytes), so remaining-space
    // checks subtract without underflow and never add an unchecked input size.
    std::size_t m_length = 0;
    bool m_isTruncated = false;
};

} // namespace Ubiquitty
