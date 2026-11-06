// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Wire.h — the primitives every Sync body is written in
// (Docs/sync.md §7.1).
//
// - `varint` is unsigned LEB128, at most ten bytes; an overlong encoding, a
//   tenth byte carrying more than the top bit, or a value past the reader's
//   bound is refused.
// - `svarint` is zigzag, then LEB128.
// - Fixed-width integers are little-endian, written field by field.
// - `bytes` is a varint length, then the bytes; `utf8` is `bytes` that must be
//   valid UTF-8 with no surrogate and nothing past U+10FFFF.
// - A list starts with a varint count, checked before any element is read.
//
// The reader latches its first failure and reads zeros after it, so a decoder
// walks a whole body and asks `ok()` once; nothing a reader returns is trusted
// until then. Every length and count it hands back has already been checked
// against the bytes that remain, so no allocation sized from the wire can
// exceed the body that announced it.
//

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The bytes `WireWriter::varint` takes for a value: one per seven bits.
[[nodiscard]] constexpr std::size_t varintBytes(std::uint64_t value)
{
    std::size_t bytes = 1;
    for (; value >= 0x80; value >>= 7)
        ++bytes;
    return bytes;
}

/// The bytes `WireWriter::utf8` takes for a text: its length, then the text.
[[nodiscard]] constexpr std::size_t utf8Bytes(std::string_view text) { return varintBytes(text.size()) + text.size(); }

/// Appends Sync primitives to a byte vector.
class WireWriter {
public:
    /// Creates a writer appending to `output`.
    ///
    /// - Parameter output: The buffer the writer appends to; it must outlive the writer.
    explicit WireWriter(std::vector<std::uint8_t>& output)
        : m_output(output)
    {
    }

    /// Appends one byte.
    ///
    /// - Parameter value: The byte.
    void u8(std::uint8_t value) { m_output.push_back(value); }

    /// Appends a little-endian 16-bit integer.
    ///
    /// - Parameter value: The integer.
    void u16(std::uint16_t value) { fixed(value, 2); }

    /// Appends a little-endian 32-bit integer.
    ///
    /// - Parameter value: The integer.
    void u32(std::uint32_t value) { fixed(value, 4); }

    /// Appends a little-endian 64-bit integer.
    ///
    /// - Parameter value: The integer.
    void u64(std::uint64_t value) { fixed(value, 8); }

    /// Appends an unsigned LEB128 integer.
    ///
    /// - Parameter value: The integer.
    void varint(std::uint64_t value)
    {
        while (value >= 0x80) {
            m_output.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        m_output.push_back(static_cast<std::uint8_t>(value));
    }

    /// Appends a zigzag-encoded signed LEB128 integer.
    ///
    /// - Parameter value: The integer.
    void svarint(std::int64_t value)
    {
        const auto bits = static_cast<std::uint64_t>(value);
        varint((bits << 1) ^ (value < 0 ? ~std::uint64_t { 0 } : 0));
    }

    /// Appends a varint length and the bytes.
    ///
    /// - Parameter value: The bytes.
    void bytes(std::span<const std::uint8_t> value)
    {
        varint(value.size());
        m_output.insert(m_output.end(), value.begin(), value.end());
    }

    /// Appends a varint length and the bytes of `value`.
    ///
    /// - Parameter value: The text, which must be valid UTF-8: the reader
    ///   refuses anything else.
    void utf8(std::string_view value)
    {
        varint(value.size());
        m_output.insert(m_output.end(), value.begin(), value.end());
    }

    /// Appends bytes with no length; the reader must know their extent.
    ///
    /// - Parameter value: The bytes.
    void raw(std::span<const std::uint8_t> value) { m_output.insert(m_output.end(), value.begin(), value.end()); }

    /// The buffer's size, for a caller recording where a section starts.
    [[nodiscard]] std::size_t size() const { return m_output.size(); }

    /// The buffer written so far.
    [[nodiscard]] std::span<const std::uint8_t> written() const { return m_output; }

private:
    /// Appends the low `width` bytes of a value, least significant first.
    ///
    /// - Parameters:
    ///   - value: The integer.
    ///   - width: How many bytes it takes.
    void fixed(std::uint64_t value, int width)
    {
        for (int i = 0; i < width; ++i)
            m_output.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }

    std::vector<std::uint8_t>& m_output;
};

/// Reads Sync primitives from a bounded span.
class WireReader {
public:
    /// Creates a reader over `bytes`, which must outlive it.
    explicit WireReader(std::span<const std::uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    /// Whether every read so far stayed in bounds and was well formed.
    [[nodiscard]] bool ok() const { return m_isOK; }

    /// Latches a failure a caller found in a value the reader returned.
    void fail() { m_isOK = false; }

    /// Whether every byte has been consumed.
    [[nodiscard]] bool isAtEnd() const { return m_offset == m_bytes.size(); }

    /// The offset of the next byte.
    [[nodiscard]] std::size_t offset() const { return m_offset; }

    /// The bytes not yet consumed.
    [[nodiscard]] std::size_t remaining() const { return m_bytes.size() - m_offset; }

    /// The bytes between two offsets this reader has passed.
    ///
    /// - Precondition: `begin <= end <= offset()`.
    [[nodiscard]] std::span<const std::uint8_t> consumed(std::size_t begin, std::size_t end) const { return m_bytes.subspan(begin, end - begin); }

    /// Reads one byte.
    [[nodiscard]] std::uint8_t u8() { return static_cast<std::uint8_t>(fixed(1)); }

    /// Reads a little-endian 16-bit integer.
    [[nodiscard]] std::uint16_t u16() { return static_cast<std::uint16_t>(fixed(2)); }

    /// Reads a little-endian 32-bit integer.
    [[nodiscard]] std::uint32_t u32() { return static_cast<std::uint32_t>(fixed(4)); }

    /// Reads a little-endian 64-bit integer.
    [[nodiscard]] std::uint64_t u64() { return fixed(8); }

    /// Reads an unsigned LEB128 integer no greater than `maximum`.
    ///
    /// Inlined unconditionally, with `count`, `bytes` and `take` below, as
    /// WireFormat.h's reader inlines its primitives: a read through a call
    /// keeps the cursor in memory for the whole decode.
    ///
    /// - Parameter maximum: The largest value the field may carry.
    /// - Returns: The value, or 0 once the reader has failed.
    [[nodiscard]] [[gnu::always_inline]] inline std::uint64_t varint(std::uint64_t maximum = ~std::uint64_t { 0 })
    {
        std::uint64_t value = 0;
        for (int index = 0; index < 10; ++index) {
            if (!m_isOK || m_offset >= m_bytes.size())
                return failed();
            const std::uint8_t byte = m_bytes[m_offset++];
            // The tenth byte holds bit 63 alone.
            if (index == 9 && byte > 1)
                return failed();
            value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * index);
            if ((byte & 0x80) == 0) {
                // A trailing zero group is a longer spelling of a shorter value.
                if (index > 0 && byte == 0)
                    return failed();
                if (value > maximum)
                    return failed();
                return value;
            }
        }
        return failed();
    }

    /// Reads a zigzag-encoded signed LEB128 integer.
    [[nodiscard]] std::int64_t svarint()
    {
        const std::uint64_t bits = varint();
        return static_cast<std::int64_t>(bits >> 1) ^ -static_cast<std::int64_t>(bits & 1);
    }

    /// Reads a list count whose elements take at least `minElementBytes`
    /// each, so the count can never promise more elements than the body holds.
    ///
    /// - Parameters:
    ///   - maximum: The structural ceiling on the count.
    ///   - minElementBytes: The fewest bytes one element can be encoded in.
    /// - Returns: The count, or 0 once the reader has failed.
    [[nodiscard]] [[gnu::always_inline]] inline std::size_t count(std::size_t maximum, std::size_t minElementBytes = 1)
    {
        const std::uint64_t value = varint(maximum);
        if (minElementBytes > 0 && value > remaining() / minElementBytes) {
            (void)failed();
            return 0;
        }
        return static_cast<std::size_t>(value);
    }

    /// Reads a varint length and that many bytes.
    ///
    /// - Parameter maximum: The longest the field may be.
    /// - Returns: A view into the reader's span, empty once the reader has failed.
    [[nodiscard]] [[gnu::always_inline]] inline std::span<const std::uint8_t> bytes(std::size_t maximum)
    {
        const std::size_t length = count(maximum);
        return take(length);
    }

    /// Reads exactly `length` bytes with no prefix.
    [[nodiscard]] [[gnu::always_inline]] inline std::span<const std::uint8_t> take(std::size_t length)
    {
        if (!m_isOK || length > remaining()) {
            failed();
            return {};
        }
        const std::span<const std::uint8_t> view = m_bytes.subspan(m_offset, length);
        m_offset += length;
        return view;
    }

    /// Reads a `utf8` field, refusing anything that is not valid UTF-8.
    ///
    /// - Parameter maximum: The longest the field may be, in bytes.
    /// - Returns: A view into the reader's span, empty once the reader has failed.
    [[nodiscard]] std::string_view utf8(std::size_t maximum);

private:
    /// Latches the failure.
    ///
    /// - Returns: 0, the value every read returns once the reader has failed.
    std::uint64_t failed()
    {
        m_isOK = false;
        return 0;
    }

    /// Reads a little-endian integer of `width` bytes.
    ///
    /// - Parameter width: How many bytes it takes.
    /// - Returns: The integer, or 0 when the bytes run out.
    std::uint64_t fixed(int width)
    {
        if (!m_isOK || remaining() < static_cast<std::size_t>(width))
            return failed();
        std::uint64_t value = 0;
        for (int i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(m_bytes[m_offset + static_cast<std::size_t>(i)]) << (8 * i);
        m_offset += static_cast<std::size_t>(width);
        return value;
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_offset = 0;
    bool m_isOK = true;
};

/// The largest Unicode scalar value.
inline constexpr std::uint32_t maxScalarValue = 0x10FFFF;

/// Whether a code point is a Unicode scalar value, the only kind UTF-8 carries.
[[nodiscard]] constexpr bool isScalarValue(std::uint32_t codePoint)
{
    return codePoint <= maxScalarValue && (codePoint < 0xD800 || codePoint > 0xDFFF);
}

/// Appends the UTF-8 form of a scalar value.
///
/// - Parameters:
///   - output: The buffer appended to.
///   - codePoint: The value, which must satisfy `isScalarValue`.
inline void appendUTF8(std::vector<std::uint8_t>& output, std::uint32_t codePoint)
{
    if (codePoint < 0x80) {
        output.push_back(static_cast<std::uint8_t>(codePoint));
    } else if (codePoint < 0x800) {
        output.push_back(static_cast<std::uint8_t>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        output.push_back(static_cast<std::uint8_t>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<std::uint8_t>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<std::uint8_t>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<std::uint8_t>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<std::uint8_t>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F)));
    }
}

/// Decodes the next scalar value of valid UTF-8.
///
/// Header-inline: a row decoder calls it once per code point.
///
/// - Parameters:
///   - text: The bytes, validated by `WireReader::utf8`.
///   - offset: The index of the sequence's lead byte, advanced past it.
/// - Returns: The scalar value.
/// - Precondition: `offset < text.size()` and `text` is valid UTF-8.
[[nodiscard]] inline std::uint32_t nextScalarValue(std::string_view text, std::size_t& offset)
{
    const auto byteAt = [&text](std::size_t index) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[index])); };
    const std::uint32_t lead = byteAt(offset);
    if (lead < 0x80) {
        offset += 1;
        return lead;
    }
    if (lead < 0xE0) {
        const std::uint32_t value = ((lead & 0x1F) << 6) | (byteAt(offset + 1) & 0x3F);
        offset += 2;
        return value;
    }
    if (lead < 0xF0) {
        const std::uint32_t value = ((lead & 0x0F) << 12) | ((byteAt(offset + 1) & 0x3F) << 6) | (byteAt(offset + 2) & 0x3F);
        offset += 3;
        return value;
    }
    const std::uint32_t value = ((lead & 0x07) << 18) | ((byteAt(offset + 1) & 0x3F) << 12) | ((byteAt(offset + 2) & 0x3F) << 6) | (byteAt(offset + 3) & 0x3F);
    offset += 4;
    return value;
}

/// Whether bytes are valid UTF-8: shortest form, no surrogate, nothing past U+10FFFF.
[[nodiscard]] inline bool isValidUTF8(std::span<const std::uint8_t> bytes)
{
    std::size_t index = 0;
    const std::size_t size = bytes.size();
    while (index < size) {
        const std::uint8_t lead = bytes[index];
        if (lead < 0x80) {
            ++index;
            continue;
        }
        int length = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            value = lead & 0x1F;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            value = lead & 0x0F;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            value = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (size - index < static_cast<std::size_t>(length))
            return false;
        for (int continuation = 1; continuation < length; ++continuation) {
            const std::uint8_t byte = bytes[index + static_cast<std::size_t>(continuation)];
            if ((byte & 0xC0) != 0x80)
                return false;
            value = (value << 6) | (byte & 0x3F);
        }
        if (value < minimum || !isScalarValue(value))
            return false;
        index += static_cast<std::size_t>(length);
    }
    return true;
}

inline std::string_view WireReader::utf8(std::size_t maximum)
{
    const std::span<const std::uint8_t> view = bytes(maximum);
    if (!isValidUTF8(view)) {
        fail();
        return {};
    }
    // The span and the view share storage; char and uint8_t alias by rule.
    return { reinterpret_cast<const char*>(view.data()), view.size() };
}

} // namespace Ubiquitty::Protocol::Sync
