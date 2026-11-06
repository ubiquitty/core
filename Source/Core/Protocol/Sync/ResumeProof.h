// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sync's resume proof (Docs/sync.md §6.9 "Resume without SSH"): how a client
// continues an attachment over a new QUIC connection
// without sending its resume secret and without another SSH login.
//
// The client proves it holds the secret the last `Welcome` issued by a MAC
// keyed with it over this connection's TLS exporter, the attachment and the
// host instance it held:
//
//   HMAC-SHA256(resumeSecret, tlsExporter ‖ attachmentID ‖ hostInstanceID)
//
// The exporter is this connection's alone, so a proof read off one connection
// is worth nothing on another, and proving costs no round trip. The table
// admits a proof only for an attachment that is Live or Lost (`admitProof`),
// and the connection it admits may do one thing: resume that attachment, its
// `Greet` carrying no secret. It creates no principal, pane or shell.

#pragma once

#include "Core/Protocol/Sync/Messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// What a client presents in place of a bootstrap token to continue an
/// attachment.
struct ResumeProof {
    ID128 attachmentID;
    std::array<std::uint8_t, 32> mac {};

    friend bool operator==(const ResumeProof&, const ResumeProof&) = default;
};

/// A proof's bytes in the preamble: the attachment's identity, then the MAC.
inline constexpr std::size_t resumeProofBytes = 16 + 32;

/// Computes a proof's MAC. Both ends compute it the same way: an identity is
/// its 16 wire bytes, low half first, each little-endian.
///
/// - Parameters:
///   - secret: The resume secret the last `Welcome` issued.
///   - exporter: This connection's TLS keying-material exporter.
///   - attachmentID: The attachment.
///   - heldHostInstanceID: The host instance the client held the attachment
///     of.
/// - Returns: The MAC.
[[nodiscard]] std::array<std::uint8_t, 32> resumeProofMAC(ID128 secret, std::span<const std::uint8_t, 32> exporter, ID128 attachmentID, ID128 heldHostInstanceID);

/// Appends a proof as it travels in the preamble.
///
/// - Parameters:
///   - output: The buffer to append to.
///   - proof: The proof.
void encodeResumePreamble(std::vector<std::uint8_t>& output, const ResumeProof& proof);

/// Reads a proof from the preamble.
///
/// - Parameter bytes: Exactly a proof's bytes.
/// - Returns: The proof, or `nullopt` for any other length.
[[nodiscard]] std::optional<ResumeProof> decodeResumePreamble(std::span<const std::uint8_t> bytes);

/// What a QUIC relay stream leads with, before any frame: how the connection
/// is authenticated.
enum class PreambleKind : std::uint8_t {
    /// A one-time bootstrap token, minted over SSH: `[1][length:u8][token]`.
    Token = 1,
    /// A resume proof for an attachment the client holds: `[2][proof]`.
    Resume = 2,
};

/// The longest bootstrap token a preamble carries.
inline constexpr std::size_t maxPreambleTokenBytes = 64;

/// The longest preamble: the most a host holds of a connection that has
/// authenticated nothing.
inline constexpr std::size_t maxPreambleBytes = 1 + std::max(1 + maxPreambleTokenBytes, resumeProofBytes);

/// The TLS exporter label both ends derive a resume proof's 32 bytes of
/// keying material under, with no context.
inline constexpr std::string_view resumeExporterLabel = "EXPORTER-Ubiquitty-Sync-Resume";

/// A bootstrap token as a preamble carries it.
struct TokenPreamble {
    std::string token;

    friend bool operator==(const TokenPreamble&, const TokenPreamble&) = default;
};

/// A preamble, read whole.
using Preamble = std::variant<TokenPreamble, ResumeProof>;

/// Appends a preamble: its kind, then its body.
///
/// - Parameters:
///   - output: The buffer to append to.
///   - preamble: The preamble; a token of 1 to `maxPreambleTokenBytes` bytes.
void encodePreamble(std::vector<std::uint8_t>& output, const Preamble& preamble);

/// The preamble a client leads a connection with to continue an attachment:
/// the resume kind, then the proof for this connection.
///
/// - Parameters:
///   - secret: The resume secret the attachment's last `Welcome` issued.
///   - exporter: This connection's keying material, exported under
///     `resumeExporterLabel`.
///   - attachmentID: The attachment.
///   - heldHostInstanceID: The host instance the attachment was held of.
/// - Returns: The preamble's bytes.
[[nodiscard]] std::vector<std::uint8_t> resumePreamble(ID128 secret, std::span<const std::uint8_t, 32> exporter, ID128 attachmentID, ID128 heldHostInstanceID);

/// Reads a preamble from a stream's first bytes, holding no more of them than
/// a preamble is long.
class PreambleReader {
public:
    /// How far the preamble has been read.
    enum class Status : std::uint8_t {
        /// More bytes are needed.
        Incomplete,
        /// The preamble is whole; `take` returns it.
        Complete,
        /// The bytes are no preamble: an unknown kind, or a token of no
        /// length or one too long.
        Malformed,
    };

    /// Takes the stream's next bytes.
    ///
    /// - Parameter bytes: The bytes.
    /// - Returns: How many of them were the preamble's. The rest, the
    ///   stream's first frames, are the caller's; none are taken once the
    ///   preamble is whole or malformed.
    std::size_t feed(std::span<const std::uint8_t> bytes);

    /// How far the preamble has been read.
    [[nodiscard]] Status status() const { return m_status; }

    /// Takes the preamble read whole, releasing what was held.
    ///
    /// - Returns: The preamble, or `nullopt` unless the status is `Complete`.
    [[nodiscard]] std::optional<Preamble> take();

    /// The bytes held, at most `maxPreambleBytes`.
    [[nodiscard]] std::size_t heldBytes() const { return m_bytes.size(); }

private:
    /// The preamble's whole length as far as the bytes held say: the bytes
    /// needed to know more while the kind or the token's length is unread.
    ///
    /// - Returns: The length, or `nullopt` for bytes that are no preamble.
    [[nodiscard]] std::optional<std::size_t> wantedBytes() const;

    std::vector<std::uint8_t> m_bytes;
    Status m_status = Status::Incomplete;
};

} // namespace Ubiquitty::Protocol::Sync
