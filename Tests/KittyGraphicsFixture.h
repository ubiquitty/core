// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// KittyGraphicsFixture — the scene helpers a kitty graphics case needs.
//
// A case in this protocol is an APC block carrying base64, so a file testing it
// needs an encoder, a block wrapper and something that produces a real image.
// Those are the same four functions each time, and a second copy of `makePNG`
// is a second answer to "what does a valid PNG look like" — the copies drift,
// and the one that drifts is the one whose case stops being about the protocol.
//
// `encodePNG` is the tree's own encoder, so a case built here is refused or
// accepted for the reason the protocol gives rather than for a fixture's idea of
// the format.

#pragma once

#include "Core/Rendering/PNGEncoder.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace UbiquittyTest::Kitty {

/// Feeds bytes to the terminal as a host would.
///
/// - Parameters:
///   - t: The terminal.
///   - bytes: What arrives on the wire.
inline void feed(Ubiquitty::Terminal& t, const std::string& bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

/// Base64 as the kitty payload carries it.
///
/// - Parameter bytes: The payload.
/// - Returns: Its base64 encoding, padded.
inline std::string base64(std::span<const std::uint8_t> bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t single = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += alphabet[(single >> 18) & 0x3F];
        out += alphabet[(single >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t pair = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += alphabet[(pair >> 18) & 0x3F];
        out += alphabet[(pair >> 12) & 0x3F];
        out += alphabet[(pair >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

/// One APC graphics block.
///
/// - Parameters:
///   - keys: The control keys, comma separated.
///   - payload: The body, already encoded.
/// - Returns: The block including its introducer and terminator.
inline std::string apc(const std::string& keys, const std::string& payload)
{
    return "\033_G" + keys + ";" + payload + "\033\\";
}

/// A real PNG, every pixel opaque and distinct enough that a decoder returning
/// the wrong buffer shows up in the bytes.
///
/// - Parameters:
///   - width: Columns of pixels.
///   - height: Rows of pixels.
/// - Returns: The encoded file.
inline std::vector<std::uint8_t> makePNG(int width, int height)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = static_cast<std::uint8_t>((i / 4) & 0xFF);
        rgba[i + 1] = 0x40;
        rgba[i + 2] = 0x80;
        rgba[i + 3] = 0xFF;
    }
    return Ubiquitty::Rendering::encodePNG(rgba, { width, height });
}

/// Raw RGBA bytes for a direct (f=32) transmit.
///
/// - Parameters:
///   - width: Columns of pixels.
///   - height: Rows of pixels.
/// - Returns: `width * height * 4` bytes.
inline std::vector<std::uint8_t> makeRGBA(int width, int height)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<std::uint8_t>(i & 0xFF);
    return rgba;
}

} // namespace UbiquittyTest::Kitty
