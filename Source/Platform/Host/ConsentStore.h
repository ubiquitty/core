// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The host's standing agent-attach consent list, <controlDir>/consent: one
// grant key per line under the header GrantFileHeader.h describes. A line is a
// principal a human has answered "always" for. The daemon reads the list at
// start and on an authenticated local reload (or operator SIGHUP), and rewrites
// it whole when a human answers; the token CLI
// edits it from outside the daemon — `token issue --allow-always` records the
// grant for the token it mints, `token revoke` withdraws the grant with the
// token — and requests a daemon-local reload. Only token-keyed lines come
// through here: a path-keyed grant names a binary an attacker can replace,
// which is why the daemon refuses to persist one.
//
// The daemon and token CLI share one nonblocking mutation lock. Each intent
// applies to the current complete list and commits through a private temporary
// file, so an older daemon cache cannot restore a withdrawn key.

#include "Platform/Host/GrantFileHeader.h"
#include "Platform/Host/GrantFileReader.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// The grant key the daemon derives for a client that authenticated with a token.
///
/// - Parameter tokenDigestHex: The token's SHA-256, as the token store records it.
/// - Returns: The line that grants that token standing consent.
inline std::string tokenGrantKey(std::string_view tokenDigestHex)
{
    return "token:" + std::string(tokenDigestHex);
}

/// How an edit of a standing grant file landed: the consent list here, or the
/// capability grants in CapabilityGrantFile.h.
enum class ConsentEdit : std::uint8_t {
    /// The key was added or removed.
    Changed,
    /// The key was already present, or already absent; the file is untouched.
    Unchanged,
    /// The file could not be locked, read, or rewritten; it is untouched.
    IOError,
    /// The file is not in this build's layout; it is untouched.
    UnreadableLayout,
};

/// The consent list as it sits on disk, entries uninterpreted.
struct ConsentList {
    /// Whether a regular list stream opened.
    bool isPresent = false;
    /// Whether every byte was read within the bounds and in the known layout.
    /// An absent or empty file is whole and holds no grant.
    bool isWhole = false;
    /// The operating system's open, read, or close error, or zero.
    int readError = 0;
    /// Whether it opens with this build's header.
    bool hasHeader = false;
    /// Whether its header is unsupported or its text contains a NUL.
    bool hasUnreadableLayout = false;
    /// Whether a line exceeds the ceiling a well-formed list never reaches.
    bool hasOverlongLine = false;
    /// Every line after the header, line ends removed; empty on any failure.
    std::vector<std::string> entries;
};

/// The maximum bytes in one logical line, including its line end.
inline constexpr std::size_t maxConsentListLineBytes = 64 * 1024;
/// The maximum bytes accepted from one consent file.
inline constexpr std::size_t maxConsentListBytes = 1024 * 1024;
/// The maximum entries retained, including blank lines.
inline constexpr std::size_t maxConsentListEntries = 4096;
/// The standing token grants the daemon can keep active.
inline constexpr std::size_t maxStandingConsentGrants = 256;

/// The one standing-consent intent applied under the file's mutation lock.
enum class StandingConsentOperation : std::uint8_t {
    Add,
    Remove,
};

/// The committed result of one standing-consent intent.
struct StandingConsentEditResult {
    ConsentEdit outcome = ConsentEdit::IOError;
    /// The committed snapshot, including an unchanged one; empty on failure.
    std::vector<std::string> entries {};
    /// Token grants removed by the committed intent, including cap eviction.
    std::vector<std::string> withdrawnGrants {};
};

/// Reads a list from a borrowed stream, publishing entries only on success.
///
/// Counted bytes preserve embedded NULs for rejection. A logical line spans
/// read chunks; clean EOF may end the final line without a line terminator.
///
/// - Parameter file: A non-null stream, which the caller keeps open.
/// - Returns: The complete entries or a refusal with no entries.
[[nodiscard]] inline ConsentList readConsentList(FILE* file)
{
    ConsentList list;
    list.isPresent = true;
    std::vector<std::string> entries;
    bool isFirstLine = true;
    const auto take = [&](std::string entry) {
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
            entry.pop_back();
        if (isFirstLine) {
            isFirstLine = false;
            list.hasHeader = isGrantFileHeader(entry, "consent");
            list.hasUnreadableLayout = !list.hasHeader;
            return list.hasHeader;
        }
        entries.push_back(std::move(entry));
        return true;
    };
    const auto result = readGrantFileLines(file,
        { .maxLineBytes = maxConsentListLineBytes, .maxFileBytes = maxConsentListBytes, .maxLines = maxConsentListEntries + 1 }, take);
    list.hasUnreadableLayout = list.hasUnreadableLayout || result.hasInvalidBytes;
    list.hasOverlongLine = result.hasOverlongLine;
    list.readError = result.readError;
    if (!result.isWhole)
        return list;
    list.entries = std::move(entries);
    list.isWhole = true;
    return list;
}

/// Reads one regular consent file without following links or waiting on a FIFO.
///
/// - Parameter path: The list file.
/// - Returns: The complete list; an absent file is whole and holds no entry.
[[nodiscard]] inline ConsentList readConsentList(const std::string& path)
{
    ConsentList list;
    auto file = openGrantFile(path);
    if (!file) {
        list.isWhole = file.error() == ENOENT;
        list.readError = list.isWhole ? 0 : file.error();
        return list;
    }
    list = readConsentList(file->get());
    if (std::fclose(file->release()) != 0) {
        list.isWhole = false;
        list.readError = errno ? errno : EIO;
        list.entries.clear();
    }
    return list;
}

/// Publishes the whole bytes of a standing grant file.
///
/// The bytes replace the file through ``replaceStoreFile``: a unique private
/// temporary, synced before the rename.
///
/// - Parameters:
///   - path: The destination, whose ``TokenStoreLock`` the caller holds.
///   - bytes: The complete file, header first.
///   - sync: The durability syscalls; the real ones when null.
/// - Returns: Changed once the bytes replaced the file, even where the
///   filesystem cannot prove them durable; IOError with the file untouched.
[[nodiscard]] inline ConsentEdit commitGrantFileBytes(const std::string& path, std::string_view bytes, const TokenStoreSync* sync = nullptr)
{
    const auto outcome = replaceStoreFile(path, [bytes](FILE* file) { return std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(); }, sync);
    return outcome == TokenWriteOutcome::IOError ? ConsentEdit::IOError : ConsentEdit::Changed;
}

/// Writes a bounded consent snapshot through atomic replacement.
///
/// Entries that cannot fit the reader's bounds are refused before any file is
/// touched. A final unterminated line can use the full logical-line bound.
/// The caller holds ``TokenStoreLock`` for the destination until replacement.
///
/// - Parameters:
///   - path: The destination whose complete current snapshot the caller read.
///   - entries: The entries to publish, with their line ends removed.
/// - Returns: Changed on complete replacement; IOError with the file untouched.
[[nodiscard]] inline ConsentEdit writeStandingConsentList(const std::string& path, std::span<const std::string> entries)
{
    if (entries.size() > maxConsentListEntries)
        return ConsentEdit::IOError;
    std::string bytes = grantFileHeader("consent");
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (entry.size() > maxConsentListLineBytes || entry.find('\0') != std::string::npos
            || entry.find('\n') != std::string::npos || entry.ends_with('\r'))
            return ConsentEdit::IOError;
        const bool hasLineEnd = entry.size() < maxConsentListLineBytes;
        if (!hasLineEnd && index + 1 < entries.size())
            return ConsentEdit::IOError;
        const auto lineBytes = entry.size() + (hasLineEnd ? std::size_t { 1 } : std::size_t { 0 });
        if (lineBytes > maxConsentListBytes - bytes.size())
            return ConsentEdit::IOError;
        bytes.append(entry);
        if (hasLineEnd)
            bytes.push_back('\n');
    }
    return commitGrantFileBytes(path, bytes);
}

/// Applies one intent to the current complete list under its mutation lock.
///
/// Adding at the active-grant bound evicts the oldest token entries. Other
/// entries are preserved, and a stale caller supplies no snapshot to restore.
///
/// - Parameters:
///   - path: The consent file whose lock is taken without waiting.
///   - grantKey: The exact key to add or remove.
///   - operation: The single mutation intended by the caller.
/// - Returns: The committed outcome and snapshot, or a refusal with no entries.
[[nodiscard]] inline StandingConsentEditResult editStandingConsentList(const std::string& path, const std::string& grantKey, StandingConsentOperation operation)
{
    TokenStoreLock lock(path);
    if (!lock.isLocked())
        return {};
    auto list = readConsentList(path);
    if (list.hasUnreadableLayout)
        return { .outcome = ConsentEdit::UnreadableLayout, .entries = {} };
    if (!list.isWhole)
        return {};
    bool didChange = false;
    std::vector<std::string> withdrawn;
    if (operation == StandingConsentOperation::Remove) {
        didChange = std::erase(list.entries, grantKey) != 0;
        if (didChange && grantKey.starts_with("token:"))
            withdrawn.push_back(grantKey);
    } else {
        if (!grantKey.starts_with("token:") || grantKey.size() > maxConsentListLineBytes)
            return {};
        std::size_t grants = 0;
        for (const auto& entry : list.entries)
            if (entry.starts_with("token:"))
                ++grants;
        const auto present = std::ranges::find(list.entries, grantKey);
        if (present != list.entries.end() && grants <= maxStandingConsentGrants)
            return { .outcome = ConsentEdit::Unchanged, .entries = std::move(list.entries) };
        grants -= std::erase(list.entries, grantKey);
        while (grants >= maxStandingConsentGrants) {
            const auto oldest = std::ranges::find_if(list.entries, [](const auto& entry) { return entry.starts_with("token:"); });
            withdrawn.push_back(*oldest);
            list.entries.erase(oldest);
            --grants;
        }
        if (list.entries.size() == maxConsentListEntries)
            return {};
        list.entries.push_back(grantKey);
        didChange = true;
    }
    if (!didChange)
        return { .outcome = ConsentEdit::Unchanged, .entries = std::move(list.entries) };
    const auto outcome = writeStandingConsentList(path, list.entries);
    if (outcome != ConsentEdit::Changed)
        return { .outcome = outcome, .entries = {} };
    std::erase_if(withdrawn, [&](const auto& identity) { return std::ranges::find(list.entries, identity) != list.entries.end(); });
    return { .outcome = outcome, .entries = std::move(list.entries), .withdrawnGrants = std::move(withdrawn) };
}

/// Records `grantKey` in the list at `path`, creating the list when absent.
///
/// The key follows existing entries, with a line separator even when the last
/// entry had none. A list that does not exist yet gets the header first.
///
/// - Parameters:
///   - path: The list file.
///   - grantKey: The key to record, as ``tokenGrantKey`` spells it.
/// - Returns: How the edit landed.
[[nodiscard]] inline ConsentEdit addStandingConsent(const std::string& path, const std::string& grantKey)
{
    return editStandingConsentList(path, grantKey, StandingConsentOperation::Add).outcome;
}

/// Withdraws `grantKey` from the list at `path`.
///
/// The list is rewritten whole through a temporary file and a rename, so a
/// crash mid-write leaves the old list rather than a truncated one. The
/// rewrite carries the header, as the daemon's own save does.
///
/// - Parameters:
///   - path: The list file.
///   - grantKey: The key to withdraw.
/// - Returns: How the edit landed; ``Unchanged`` when the key was not there.
[[nodiscard]] inline ConsentEdit removeStandingConsent(const std::string& path, const std::string& grantKey)
{
    return editStandingConsentList(path, grantKey, StandingConsentOperation::Remove).outcome;
}

} // namespace Ubiquitty::Host
