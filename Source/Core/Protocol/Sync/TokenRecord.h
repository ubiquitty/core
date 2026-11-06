// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The on-disk shape of a capability token, one record per line:
//
//     <sha256hex> <scopebits-decimal> <expiry-unix-or-0> <label>
//
// Tokens are persisted hashed, never as the secret. The label is last and may
// contain spaces; fields are separated by runs of spaces, so a record stays
// readable however it was written.
//
// The format lives here rather than beside the daemon that writes it, because
// the settings surfaces that list tokens cannot include Platform/Host. Reading
// the file and rewriting the store stay with the daemon; whether a record has
// lapsed does not, because the authorization gate and every surface that shows
// a token as spent have to answer that identically.

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// One capability token as persisted on disk.
struct TokenRecord {
    /// The token's SHA-256 digest, 64 lowercase hexadecimal characters.
    std::string hashHex;
    /// The capabilities the token grants.
    ScopeSet scope {};
    /// Expiry as a Unix timestamp, or `0` when the token never expires.
    std::uint64_t expiryUnix = 0;
    /// The user-supplied label, which may contain spaces.
    std::string label;
};

/// Parses one already-assembled line and appends the record it names.
///
/// Blanks, comments, and lines whose digest is not 64 characters are skipped,
/// so a damaged line costs its own record and nothing else.
///
/// - Parameters:
///   - entry: One line from the store, with or without its line terminator.
///   - records: Receives the parsed record when the line names a valid one.
inline void parseTokenLine(std::string entry, std::vector<TokenRecord>& records)
{
    while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
        entry.pop_back();
    if (entry.empty() || entry[0] == '#')
        return;
    /// Whether a field ends at the next space or runs to end of line.
    enum class FieldExtent : std::uint8_t {
        Word,
        ToEndOfLine
    };

    TokenRecord record;
    std::size_t p = 0;
    auto field = [&](FieldExtent extent) -> std::string {
        while (p < entry.size() && entry[p] == ' ')
            ++p;
        std::size_t start = p;
        if (extent == FieldExtent::ToEndOfLine)
            p = entry.size();
        else
            while (p < entry.size() && entry[p] != ' ')
                ++p;
        return entry.substr(start, p - start);
    };
    record.hashHex = field(FieldExtent::Word);
    const std::string scopeText = field(FieldExtent::Word);
    const std::string expiryText = field(FieldExtent::Word);
    record.label = field(FieldExtent::ToEndOfLine);
    if (record.hashHex.size() != 64)
        return; // malformed digest
    record.scope = ScopeSet::fromRaw(static_cast<uint8_t>(std::strtoul(scopeText.c_str(), nullptr, 10)));
    record.expiryUnix = std::strtoull(expiryText.c_str(), nullptr, 10);
    records.push_back(std::move(record));
}

/// Whether a token has lapsed.
enum class TokenExpiry : std::uint8_t {
    /// The record carries no expiry, so the token never lapses.
    Never,
    /// The record carries an expiry that has not been reached.
    Active,
    /// The record's expiry has been reached.
    Expired,
};

/// Returns whether `record` has lapsed as of `nowUnix`.
///
/// A token lapses *at* its expiry second rather than after it. The
/// authorization gate, the revocation sweep, and the listings all read that
/// boundary from here, so a token can never be honored by one and shown as
/// spent by another.
///
/// - Parameters:
///   - record: The record to classify.
///   - nowUnix: The moment to classify against, as a Unix timestamp.
/// - Returns: `Never` when the record carries no expiry, `Expired` once
///   `nowUnix` has reached it, and `Active` until then.
constexpr TokenExpiry tokenExpiry(const TokenRecord& record, std::uint64_t nowUnix)
{
    if (record.expiryUnix == 0)
        return TokenExpiry::Never;
    return nowUnix >= record.expiryUnix ? TokenExpiry::Expired : TokenExpiry::Active;
}

/// Returns the word a token listing shows for `expiry`.
///
/// - Parameter expiry: The state to name.
/// - Returns: `never`, `active`, or `expired`.
constexpr const char* tokenExpiryName(TokenExpiry expiry)
{
    switch (expiry) {
    case TokenExpiry::Never:
        return "never";
    case TokenExpiry::Active:
        return "active";
    case TokenExpiry::Expired:
        return "expired";
    }
    return "never";
}

} // namespace Ubiquitty::Protocol::Sync
