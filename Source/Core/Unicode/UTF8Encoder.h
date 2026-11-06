// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <string>

// Single-code-point UTF-8 encoder, the inverse of UTF8Decoder: one scalar in,
// its 1..4-byte form appended. A value that is not a Unicode scalar contributes
// no bytes, rather than an overlong, out-of-range or surrogate sequence.
//
// Surrogates are the half that is easy to miss: U+D800..U+DFFF are below
// U+10000, so a length-only range check routes them into the three-byte branch
// and emits WTF-8 — three bytes no UTF-8 reader will accept. They also make a
// caller counting UTF-16 units disagree with this encoder about how long the same
// value is, so a range measured in one, such as an accessibility row range, and
// applied in the other covers bytes that do not exist.

namespace Ubiquitty::Unicode {

// Append the UTF-8 encoding of one Unicode scalar `codePoint` to `out`.
// 1 byte for ASCII, 2 for U+0080..U+07FF, 3 for the rest of the BMP, 4 for
// the astral planes. A value that is not a scalar appends nothing.
inline void appendUTF8(std::string& outputBuffer, std::uint32_t codePoint)
{
    if (!isScalarValue(codePoint))
        return;
    if (codePoint < 0x80) {
        outputBuffer.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        outputBuffer.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        outputBuffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        outputBuffer.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        outputBuffer.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        outputBuffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        outputBuffer.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        outputBuffer.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        outputBuffer.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        outputBuffer.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Fixed-buffer form: writes the 1..4 byte form at out[*length], advancing
// *length. Precondition: at least 4 bytes of headroom. A value that is not a
// scalar writes nothing and leaves *length unchanged.
inline void appendUTF8(char* outputBuffer, std::size_t* length, std::uint32_t codePoint)
{
    if (!isScalarValue(codePoint))
        return;
    std::size_t n = *length;
    if (codePoint < 0x80) {
        outputBuffer[n++] = static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        outputBuffer[n++] = static_cast<char>(0xC0 | (codePoint >> 6));
        outputBuffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        outputBuffer[n++] = static_cast<char>(0xE0 | (codePoint >> 12));
        outputBuffer[n++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        outputBuffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0x10FFFF) {
        outputBuffer[n++] = static_cast<char>(0xF0 | (codePoint >> 18));
        outputBuffer[n++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        outputBuffer[n++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        outputBuffer[n++] = static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    *length = n;
}

} // namespace Ubiquitty::Unicode
