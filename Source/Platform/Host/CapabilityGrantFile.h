// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The host's standing capability grants, <controlDir>/capability_grants: one
// `<capability>\t<origin>\t<identity>` line, in decimal, per grant a human
// answered "always" for, under the header GrantFileHeader.h describes. Only
// token-keyed identities are recorded: a path-keyed one names a binary an
// attacker can replace.
//
// Several daemon instances can share the file, so it follows the consent
// list's protocol (ConsentStore.h). Each intent applies to the current complete
// file under the nonblocking mutation lock and commits through a private
// temporary; no daemon writes its cache back, so an older cache cannot restore
// a grant another instance withdrew. A daemon learns of that withdrawal before
// its own cached copy authorizes anything (CapabilityGrantFile below).

#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/GrantFileHeader.h"
#include "Platform/Host/GrantFileReader.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// The standing capability grants one file holds.
inline constexpr std::size_t maxAlwaysAllowedCapabilityGrants = 256;
/// The maximum bytes in one line, including its line end.
inline constexpr std::size_t maxCapabilityGrantLineBytes = 4096;
/// The maximum bytes accepted from one file: the header and every grant.
inline constexpr std::size_t maxCapabilityGrantFileBytes = (maxAlwaysAllowedCapabilityGrants + 1) * maxCapabilityGrantLineBytes;

/// Whether an identity may hold a standing capability grant: a token key
/// with a digest, and nothing that could end or split its line.
///
/// - Parameter identity: The principal's grant key.
/// - Returns: Whether the file may record it.
[[nodiscard]] inline bool isStandingCapabilityIdentity(std::string_view identity)
{
    constexpr std::string_view tokenPrefix = "token:";
    constexpr std::string_view lineBytes("\t\r\n\0", 4);
    return identity.size() > tokenPrefix.size() && identity.starts_with(tokenPrefix) && identity.find_first_of(lineBytes) == std::string_view::npos;
}

/// Whether a key is one the file may record: known enumerators and a
/// standing identity.
///
/// - Parameter key: The grant.
/// - Returns: Whether it is recordable.
[[nodiscard]] inline bool isStandingCapabilityGrant(const CapabilityGrantKey& key)
{
    return key.capability <= Capability::None && key.origin <= CapabilityOrigin::Remote && isStandingCapabilityIdentity(key.identity);
}

/// Parses one grant line, line end removed.
///
/// Both numbers are plain decimal and must name a known enumerator.
///
/// - Parameter line: The line after the header.
/// - Returns: The grant, or nothing when the line is not one this build writes.
[[nodiscard]] inline std::optional<CapabilityGrantKey> parseCapabilityGrantLine(std::string_view line)
{
    const auto firstTab = line.find('\t');
    const auto secondTab = firstTab == std::string_view::npos ? std::string_view::npos : line.find('\t', firstTab + 1);
    if (secondTab == std::string_view::npos)
        return std::nullopt;
    const auto number = [](std::string_view field) -> std::optional<unsigned> {
        unsigned value = 0;
        const auto end = field.data() + field.size();
        const auto [stop, error] = std::from_chars(field.data(), end, value);
        if (field.empty() || error != std::errc {} || stop != end)
            return std::nullopt;
        return value;
    };
    const auto capability = number(line.substr(0, firstTab));
    const auto origin = number(line.substr(firstTab + 1, secondTab - firstTab - 1));
    if (!capability || !origin || *capability > static_cast<unsigned>(Capability::None) || *origin > static_cast<unsigned>(CapabilityOrigin::Remote))
        return std::nullopt;
    CapabilityGrantKey key { static_cast<Capability>(*capability), static_cast<CapabilityOrigin>(*origin), std::string(line.substr(secondTab + 1)) };
    if (!isStandingCapabilityGrant(key))
        return std::nullopt;
    return key;
}

/// A grant file held open as it was read, with its stamp.
///
/// The descriptor keeps the inode allocated, so no file renamed over this one
/// can reuse it: every replacement changes the inode at the path, however
/// coarse the filesystem's timestamps.
struct HeldGrantFile {
    /// The file, or none for an absent one.
    PAL::UnixFileDescriptor descriptor;
    /// The file as `fstat` described it, or the `ENOENT` of its absence.
    GrantFileStamp stamp;
};

/// Holds a regular file open from its own descriptor.
///
/// - Parameter descriptor: An open descriptor, which the caller keeps.
/// - Returns: A duplicate and its stamp, or nothing when either fails.
[[nodiscard]] inline std::optional<HeldGrantFile> holdGrantFile(int descriptor)
{
    PAL::UnixFileDescriptor held(::fcntl(descriptor, F_DUPFD_CLOEXEC, 0));
    struct stat information {};
    if (!held || ::fstat(held.get(), &information) != 0 || !S_ISREG(information.st_mode))
        return std::nullopt;
    return HeldGrantFile { std::move(held), grantFileStamp(information) };
}

/// The capability grants as one read found them.
struct CapabilityGrantFileContents {
    /// Whether every byte was read within the bounds and every line parsed.
    /// An absent or empty file is whole and holds no grant.
    bool isWhole = false;
    /// The operating system's open, examination, read, or close error, or zero.
    int readError = 0;
    /// Whether the header is another layout's, or a line is malformed or holds a NUL.
    bool hasUnreadableLayout = false;
    /// Every distinct grant in file order; empty on any failure.
    std::vector<CapabilityGrantKey> grants;
    /// When whole, the file the bytes came from; empty on any failure.
    std::optional<HeldGrantFile> held;
};

/// Reads one regular grant file without following links or waiting on a FIFO.
///
/// The file is examined before its bytes are read, so the stamp is never newer
/// than they are: an edit made in place during the read shows at the next
/// examination.
///
/// - Parameter path: The capability grant file.
/// - Returns: The complete grants, or a refusal with none.
[[nodiscard]] inline CapabilityGrantFileContents readCapabilityGrantFile(const std::string& path)
{
    CapabilityGrantFileContents contents;
    auto file = openGrantFile(path);
    if (!file) {
        contents.isWhole = file.error() == ENOENT;
        contents.readError = contents.isWhole ? 0 : file.error();
        if (contents.isWhole)
            contents.held = HeldGrantFile { .descriptor = {}, .stamp = { .error = ENOENT } };
        return contents;
    }
    auto held = holdGrantFile(::fileno(file->get()));
    if (!held) {
        contents.readError = errno ? errno : EIO;
        return contents;
    }
    std::vector<CapabilityGrantKey> grants;
    bool isFirstLine = true;
    bool isMalformed = false;
    const auto take = [&](std::string line) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (std::exchange(isFirstLine, false)) {
            isMalformed = !isGrantFileHeader(line, "capability-grants");
            return !isMalformed;
        }
        if (line.empty())
            return true;
        auto key = parseCapabilityGrantLine(line);
        if (!key) {
            isMalformed = true;
            return false;
        }
        if (std::ranges::find(grants, *key) == grants.end())
            grants.push_back(std::move(*key));
        return true;
    };
    const auto result = readGrantFileLines(file->get(),
        { .maxLineBytes = maxCapabilityGrantLineBytes, .maxFileBytes = maxCapabilityGrantFileBytes, .maxLines = maxAlwaysAllowedCapabilityGrants + 1 }, take);
    const bool didClose = std::fclose(file->release()) == 0;
    contents.hasUnreadableLayout = isMalformed || result.hasInvalidBytes;
    contents.readError = result.readError;
    if (!didClose && !contents.readError)
        contents.readError = errno ? errno : EIO;
    if (!result.isWhole || !didClose)
        return contents;
    contents.isWhole = true;
    contents.grants = std::move(grants);
    contents.held = std::move(held);
    return contents;
}

/// Writes a bounded grant snapshot through atomic replacement.
///
/// Grants the reader would refuse are refused before any file is touched.
///
/// - Parameters:
///   - path: The destination, whose ``TokenStoreLock`` the caller holds.
///   - grants: The grants to publish, in order.
///   - sync: The durability syscalls; the real ones when null.
/// - Returns: Changed on complete replacement; IOError with the file untouched.
[[nodiscard]] inline ConsentEdit writeCapabilityGrantFile(const std::string& path, std::span<const CapabilityGrantKey> grants, const TokenStoreSync* sync = nullptr)
{
    if (grants.size() > maxAlwaysAllowedCapabilityGrants)
        return ConsentEdit::IOError;
    std::string bytes = grantFileHeader("capability-grants");
    for (const auto& key : grants) {
        if (!isStandingCapabilityGrant(key))
            return ConsentEdit::IOError;
        const auto line = std::to_string(std::to_underlying(key.capability)) + '\t' + std::to_string(std::to_underlying(key.origin)) + '\t' + key.identity + '\n';
        if (line.size() > maxCapabilityGrantLineBytes)
            return ConsentEdit::IOError;
        bytes += line;
    }
    return commitGrantFileBytes(path, bytes, sync);
}

/// The one capability-grant intent applied under the file's mutation lock.
enum class CapabilityGrantOperation : std::uint8_t {
    /// Records the key, evicting the oldest grant at the bound.
    Grant,
    /// Withdraws exactly the key.
    Withdraw,
    /// Withdraws every grant held by the key's identity, whatever its
    /// capability and origin.
    WithdrawIdentity,
};

/// The committed result of one capability-grant intent.
struct CapabilityGrantEditResult {
    ConsentEdit outcome = ConsentEdit::IOError;
    /// The committed grants, including an unchanged snapshot; empty on failure.
    std::vector<CapabilityGrantKey> grants {};
    /// The file that holds exactly `grants`, opened under the lock; empty on
    /// failure or when the committed file could not be opened.
    std::optional<HeldGrantFile> held {};
};

/// Applies one intent to the current complete file under its mutation lock.
///
/// Unrelated grants are preserved, and the caller supplies no snapshot, so a
/// stale cache has nothing to restore. A file that cannot be read whole is
/// left as it is.
///
/// - Parameters:
///   - path: The capability grant file, whose lock is taken without waiting.
///   - operation: The single mutation intended by the caller.
///   - key: The grant, or for `CapabilityGrantOperation::WithdrawIdentity`
///     the identity whose grants are withdrawn.
///   - sync: The durability syscalls; the real ones when null.
/// - Returns: The committed outcome and snapshot, or a refusal with no grants.
[[nodiscard]] inline CapabilityGrantEditResult editCapabilityGrantFile(const std::string& path, CapabilityGrantOperation operation, const CapabilityGrantKey& key, const TokenStoreSync* sync = nullptr)
{
    TokenStoreLock lock(path);
    if (!lock.isLocked())
        return {};
    auto contents = readCapabilityGrantFile(path);
    if (contents.hasUnreadableLayout)
        return { .outcome = ConsentEdit::UnreadableLayout };
    if (!contents.isWhole)
        return {};
    auto& grants = contents.grants;
    bool didChange = false;
    switch (operation) {
    case CapabilityGrantOperation::Grant:
        if (!isStandingCapabilityGrant(key))
            return {};
        if (std::ranges::find(grants, key) != grants.end())
            break;
        if (grants.size() >= maxAlwaysAllowedCapabilityGrants)
            grants.erase(grants.begin(), grants.begin() + static_cast<std::ptrdiff_t>(grants.size() - maxAlwaysAllowedCapabilityGrants + 1));
        grants.push_back(key);
        didChange = true;
        break;
    case CapabilityGrantOperation::Withdraw:
        didChange = std::erase(grants, key) != 0;
        break;
    case CapabilityGrantOperation::WithdrawIdentity:
        if (key.identity.empty())
            return {};
        didChange = std::erase_if(grants, [&](const CapabilityGrantKey& grant) { return grant.identity == key.identity; }) != 0;
        break;
    }
    if (!didChange)
        return { .outcome = ConsentEdit::Unchanged, .grants = std::move(grants), .held = std::move(contents.held) };
    const auto outcome = writeCapabilityGrantFile(path, grants, sync);
    if (outcome != ConsentEdit::Changed)
        return { .outcome = outcome };
    // The lock is still held, so no cooperating writer has replaced the file
    // since the rename.
    auto committed = openGrantFile(path);
    return { .outcome = outcome, .grants = std::move(grants), .held = committed ? holdGrantFile(::fileno(committed->get())) : std::nullopt };
}

/// Keeps a grant store's Always grants equal to one capability grant file.
///
/// Bound, it is the store's `CapabilityGrantStore::StandingGrantSource`.
/// Before a cached Always grant authorizes, the store has it compare the entry
/// at its path with the file it last read whole, by device, inode, size, and
/// modification and change times, and reread the file when they differ. The
/// stamp comes from the descriptor the bytes were read through, which stays
/// open: writers replace the file by rename, so a replacement always shows a
/// new inode, and an edit in place moves the size or a time. An equal stamp
/// therefore means the cache holds the file's grants, including every
/// withdrawal. A file that cannot be examined or read whole leaves the store
/// with no Always grant and nothing held, so the next confirmation reads again.
///
/// The store holds this object's address: it neither copies nor moves, and
/// outlives every check the store answers.
class CapabilityGrantFile final {
public:
    CapabilityGrantFile() = default;
    CapabilityGrantFile(const CapabilityGrantFile&) = delete;
    CapabilityGrantFile& operator=(const CapabilityGrantFile&) = delete;

    /// The bound file, or empty before `bind`.
    [[nodiscard]] const std::string& path() const { return m_path; }

    /// Makes `path` the persisted record of `store`'s Always grants.
    ///
    /// No grant is read here; the next confirmation reads the file.
    ///
    /// - Parameters:
    ///   - path: The capability grant file.
    ///   - store: The store that consults this file from now on.
    void bind(std::string path, CapabilityGrantStore& store)
    {
        m_path = std::move(path);
        m_held.reset();
        store.setStandingGrantSource({ .context = this, .confirm = confirmThrough });
    }

    /// Forgets the file last read, so the next confirmation rereads it.
    void invalidate() { m_held.reset(); }

    /// Reads the file whole into `store`, or leaves it no Always grant.
    ///
    /// - Parameter store: The bound store.
    /// - Returns: The read, whose grants `store` now holds when it is whole.
    CapabilityGrantFileContents reload(CapabilityGrantStore& store)
    {
        auto contents = readCapabilityGrantFile(m_path);
        adopt(store, contents.grants, std::move(contents.held));
        return contents;
    }

    /// Rereads the file into `store` when it is not the file last read whole.
    ///
    /// - Parameter store: The bound store.
    void confirm(CapabilityGrantStore& store)
    {
        if (m_held && examineGrantFile(m_path) == m_held->stamp)
            return;
        (void)reload(store);
    }

    /// Applies one intent to the file and adopts the grants it committed.
    ///
    /// A refusal leaves `store` as it was, so an intent that did not commit
    /// is never acknowledged; the next confirmation decides what a file that
    /// could not be read grants.
    ///
    /// - Parameters:
    ///   - store: The bound store.
    ///   - operation: The single mutation intended.
    ///   - key: The grant, or the identity to withdraw.
    ///   - sync: The durability syscalls; the real ones when null.
    /// - Returns: How the edit landed.
    ConsentEdit edit(CapabilityGrantStore& store, CapabilityGrantOperation operation, const CapabilityGrantKey& key, const TokenStoreSync* sync = nullptr)
    {
        auto result = editCapabilityGrantFile(m_path, operation, key, sync);
        if (result.outcome == ConsentEdit::Changed || result.outcome == ConsentEdit::Unchanged)
            adopt(store, result.grants, std::move(result.held));
        return result.outcome;
    }

private:
    /// Confirms `store` through the bound file a standing-grant source names.
    ///
    /// - Parameters:
    ///   - context: The `CapabilityGrantFile` that bound `store`.
    ///   - store: The store asking.
    static void confirmThrough(void* context, CapabilityGrantStore& store) { static_cast<CapabilityGrantFile*>(context)->confirm(store); }

    /// Makes `grants` the store's Always grants and `held` the file they came from.
    ///
    /// - Parameters:
    ///   - store: The bound store.
    ///   - grants: The file's complete grants, or none.
    ///   - held: The file that holds exactly `grants`, or nothing to confirm against.
    void adopt(CapabilityGrantStore& store, std::span<const CapabilityGrantKey> grants, std::optional<HeldGrantFile>&& held)
    {
        store.adoptAlwaysGrants(grants);
        m_held = std::move(held);
    }

    std::string m_path;
    /// The file whose grants the store holds; empty when none was read whole.
    std::optional<HeldGrantFile> m_held;
};

} // namespace Ubiquitty::Host
