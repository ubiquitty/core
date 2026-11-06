// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::DragDrop {

inline constexpr std::size_t maxPacketPayloadBytes = 4096;
inline constexpr std::size_t maxMetadataBytes = 256;
inline constexpr std::size_t maxMIMETypes = 128;
inline constexpr std::size_t maxMIMEBytes = 255;
inline constexpr std::size_t maxMIMEListBytes = maxMIMETypes * (maxMIMEBytes + 1);
inline constexpr std::size_t maxPreparedDataBytes = 64 * 1024 * 1024;
inline constexpr std::size_t maxPendingReads = 64;
inline constexpr std::size_t maxRemoteRoots = 1024;
inline constexpr std::size_t maxDirectoryHandles = 128;
inline constexpr std::size_t maxImages = 16;
inline constexpr std::size_t maxImageBytes = 16 * 1024 * 1024;

/// OSC 72 command types retain the protocol's case-sensitive spelling.
enum class Type : char {
    Accept = 'a',
    StopAccepting = 'A',
    Move = 'm',
    Drop = 'M',
    Read = 'r',
    ReadError = 'R',
    Offer = 'o',
    Prepare = 'p',
    Present = 'P',
    Event = 'e',
    Error = 'E',
    File = 'k',
    Query = 'q',
    QueryResponse = 'Q',
};

/// A protocol refusal, independent of the host's errno values.
enum class Error : std::uint8_t {
    Invalid,
    TooLarge,
    NoMemory,
    NotPermitted,
    NotFound,
    TooManyFiles,
    InputOutput,
    Unknown,
};

/// Metadata values fit the union of signed and unsigned 32-bit integers.
/// An absent field is distinct from an explicitly transmitted zero.
struct Metadata {
    Type type = Type::Accept;
    std::optional<std::int64_t> x;
    std::optional<std::int64_t> y;
    std::optional<std::int64_t> pixelX;
    std::optional<std::int64_t> pixelY;
    std::optional<std::uint32_t> identity;
    std::optional<std::uint32_t> operation;
    bool hasType = false;
    bool hasMore = false;
    bool hasMoreChunks = false;
};

/// Views borrow the OSC accumulator until its next mutation.
struct PacketView {
    Metadata metadata {};
    std::string_view payload {};
};

/// Parses one OSC 72 body after its numeric selector and separator.
/// - Parameter body: Metadata and optional payload, without OSC or ST.
/// - Returns: Validated metadata and a payload of at most 4096 encoded bytes.
[[nodiscard]] std::expected<PacketView, Error> parsePacket(std::string_view body);

/// Frames one packet with the protocol's seven-bit OSC and ST delimiters.
/// - Parameters:
///   - metadata: Fields to include; absent fields remain absent.
///   - payload: At most 4096 encoded bytes.
///   - output: Receives a complete frame or remains unusable on refusal.
/// - Returns: The complete frame length, or a refusal.
[[nodiscard]] std::expected<std::size_t, Error> framePacket(const Metadata& metadata, std::string_view payload, std::span<char> output);

/// Returns the POSIX symbolic spelling used in OSC 72 error payloads.
[[nodiscard]] std::string_view errorName(Error);

/// Validates the space-separated MIME list, including the empty registration.
[[nodiscard]] std::expected<std::size_t, Error> countMIMETypes(std::string_view);

/// Borrows a MIME type by its zero-based position in a validated list.
[[nodiscard]] std::optional<std::string_view> mimeAt(std::string_view, std::size_t);

} // namespace Ubiquitty::DragDrop
