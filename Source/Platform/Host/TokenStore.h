// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The host's capability-token store. Tokens are persisted HASHED (never the
// secret): one record per line under <controlDir>/tokens, 0600:
//     <sha256hex> <scopebits-decimal> <expiry-unix-or-0> <label>
// The daemon verifies a presented token by hashing it and matching the digest;
// the issuance CLI is the only place a plaintext token exists (printed once).

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/TokenRecord.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/GrantFileReader.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty {
namespace Host {

using Protocol::Sync::parseTokenLine;
using Protocol::Sync::tokenExpiry;
using Protocol::Sync::TokenExpiry;
using Protocol::Sync::TokenRecord;

/// Holds the token store's process-wide mutation lock until destruction.
/// Readers observe complete rewrites; competing writers retry without blocking the reactor.
class TokenStoreLock final {
public:
    explicit TokenStoreLock(const std::string& path)
        : m_descriptor(path.empty() ? -1 : ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600))
    {
        struct stat information {};
        if (!m_descriptor || ::fstat(m_descriptor.get(), &information) != 0
            || !S_ISREG(information.st_mode) || information.st_uid != ::geteuid()
            || (information.st_mode & 077) || ::flock(m_descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
            m_descriptor.reset();
        }
    }

    /// Reports whether this owner may read, modify, and rewrite the store.
    [[nodiscard]] bool isLocked() const { return static_cast<bool>(m_descriptor); }

private:
    PAL::UnixFileDescriptor m_descriptor;
};

/// Opens a file that only its owner may ever read.
///
/// The mode argument to `open` applies at creation, so the file is never
/// briefly group- or world-readable the way create-then-`fchmod` leaves it —
/// a window a co-located user can win, and this file holds token digests. The
/// `fchmod` is kept for the file that already exists, whose mode `open` does
/// not touch.
///
/// - Parameters:
///   - path: The file to open.
///   - mode: An `fopen` mode string; the matching `open` flags are derived
///     from its first character, so "a" appends and "w" truncates.
/// - Returns: The stream, or nullptr with `errno` set.
inline FILE* openOwnerOnly(const std::string& path, const char* mode)
{
    const int flags = (mode[0] == 'a') ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
    // A file that does not exist yet is created private, and the mode argument
    // applies at creation — so unlike a create-then-chmod there is no moment
    // when it is readable to anyone else. Nothing tightens it afterwards,
    // which is what lets a test observe that the creation is what made it
    // private.
    int descriptor = ::open(path.c_str(), flags | O_EXCL, 0600);
    if (descriptor < 0) {
        if (errno != EEXIST)
            return nullptr;
        descriptor = ::open(path.c_str(), flags, 0600);
        if (descriptor < 0)
            return nullptr;
        // An existing file keeps whatever mode it was created with, which
        // need not be 0600.
        ::fchmod(descriptor, 0600);
    }
    FILE* file = ::fdopen(descriptor, mode);
    if (!file)
        ::close(descriptor);
    return file;
}

struct TokenVerdict {
    bool ok = false;
    Protocol::Sync::ByeReason reason = Protocol::Sync::ByeReason::TokenInvalid; // valid only when !ok
    Protocol::Sync::ScopeSet scope {}; // the granted scope when ok
    std::string tokenID; // the digest — a stable, non-spoofable principal key
    // The record's expiry when ok, 0 when it never expires. A token binds the
    // connection it authenticated for as long as it is valid, so the deadline
    // travels with the verdict rather than staying behind in the store.
    std::uint64_t expiryUnix = 0;
};

/// Whether one store line, line end removed, is blank, a comment, or a record
/// in the form `writeTokens` writes: a 64-digit lowercase hexadecimal digest,
/// a decimal scope mask that fits a byte, a decimal expiry that fits 64 bits,
/// and a label that may be empty.
[[nodiscard]] inline bool isWellFormedTokenLine(std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);
    if (line.empty() || line.front() == '#')
        return true;
    const auto field = [&line] {
        const std::size_t space = line.find(' ');
        const std::string_view value = line.substr(0, space);
        line = space == std::string_view::npos ? std::string_view {} : line.substr(space + 1);
        return value;
    };
    const std::string_view digest = field();
    const std::string_view scope = field();
    const std::string_view expiry = field();
    if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return false;
    unsigned scopeMask = 0;
    const auto scopeEnd = scope.data() + scope.size();
    const auto [scopeStop, scopeError] = std::from_chars(scope.data(), scopeEnd, scopeMask);
    if (scope.empty() || scopeError != std::errc {} || scopeStop != scopeEnd || scopeMask > 0xff)
        return false;
    std::uint64_t expiryUnix = 0;
    const auto expiryEnd = expiry.data() + expiry.size();
    const auto [expiryStop, expiryError] = std::from_chars(expiry.data(), expiryEnd, expiryUnix);
    return !expiry.empty() && expiryError == std::errc {} && expiryStop == expiryEnd;
}

/// What one read of the store found.
struct TokenStoreContents {
    /// The records, in file order.
    std::vector<TokenRecord> records;
    /// Whether the store was read to its end and every line in it is
    /// well formed. An absent store is whole and holds no record.
    ///
    /// Authority read from a store that is not whole is no authority: a line
    /// the reader could not take may be the revocation it was meant to see.
    bool isWhole = false;
};

/// Reads the store, reporting whether it read all of it.
///
/// - Parameter path: The store.
/// - Returns: The records, and whether nothing in the store went unread.
[[nodiscard]] inline TokenStoreContents readTokenStore(const std::string& path)
{
    TokenStoreContents contents;
    auto file = openGrantFile(path);
    if (!file) {
        contents.isWhole = file.error() == ENOENT;
        return contents;
    }
    constexpr std::size_t maxLineBytes = 64 * 1024;
    bool isWhole = true;
    const auto take = [&](std::string line) {
        isWhole = isWhole && isWellFormedTokenLine(line);
        parseTokenLine(std::move(line), contents.records);
        return true;
    };
    const auto result = readGrantFileLines(file->get(), { .maxLineBytes = maxLineBytes }, take);
    const bool didClose = std::fclose(file->release()) == 0;
    contents.isWhole = isWhole && result.isWhole && didClose;
    return contents;
}

/// Reads the records the store holds, whether or not it could be read whole.
///
/// For listing only. Authorization and mutations read ``readTokenStore`` and
/// refuse a store that is not whole.
///
/// - Parameter path: The store.
/// - Returns: The records it could read, in file order.
inline std::vector<TokenRecord> loadTokens(const std::string& path)
{
    return readTokenStore(path).records;
}

// Rewrite the store with exactly `records`, 0600, via a temp file + rename so
// a crash mid-write can't truncate it. Used by `token revoke`.
//
// How the rewrite landed, because a security property needs more than worked
// or did not: only `Durable` proves the revoked record stays revoked across
// a crash. `IOError` promises nothing (retry); `UnsupportedDurability` names
// a filesystem that cannot fsync, where durability is unprovable rather than
// failed — the caller reports that distinctly instead of believing either way.
/// How a store rewrite landed.
enum class TokenWriteOutcome : std::uint8_t {
    Durable,
    IOError,
    UnsupportedDurability,
};

/// The directory a store path's rename commits in: `"tokens"` lives in `"."`,
/// `"/tokens"` in `"/"`, anything else in its dirname. Pure, so every shape
/// is unit-tested where the syscall behind it cannot be.
[[nodiscard]] inline std::string tokenParentDirectory(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

/// The two durability syscalls a rewrite performs, injectable so tests fail
/// them on demand: a test cannot crash the kernel's page cache, but it can
/// hand back EIO or EINVAL and assert the outcome each maps to.
struct TokenStoreSync {
    /// Syncs an open file descriptor; `::fsync` unless injected.
    int (*syncFile)(int descriptor);
    /// Opens, syncs and closes a directory path; likewise.
    int (*syncParent)(const char* parentPath);
};

/// The real syscalls behind ``TokenStoreSync``.
[[nodiscard]] inline int syncTokenFile(int descriptor) { return ::fsync(descriptor); }
[[nodiscard]] inline int syncTokenParent(const char* parentPath)
{
    const int directoryDescriptor = ::open(parentPath, O_RDONLY | O_DIRECTORY);
    if (directoryDescriptor < 0)
        return -1;
    const int result = ::fsync(directoryDescriptor);
    const int savedErrno = errno;
    ::close(directoryDescriptor);
    if (result != 0)
        errno = savedErrno;
    return result;
}

/// Replaces a store file durably with what `writeContents` writes (see
/// ``TokenWriteOutcome``).
///
/// The contents go to a temporary beside `path` that is created exclusively,
/// under a unique name, with mode 0600, and synced before the rename that
/// publishes it. A temporary that is not published is removed. The caller holds
/// the store's ``TokenStoreLock`` when another writer may edit the same file.
///
/// - Parameters:
///   - path: The store file.
///   - writeContents: Writes the complete contents to the stream it is
///     given, answering whether every write succeeded.
///   - sync: The durability syscalls; the real ones when null, injected
///     failures under test.
/// - Returns: How the replacement landed; only ``Durable`` proves it, and
///   ``IOError`` leaves `path` as it was.
template<typename WriteContents>
[[nodiscard]] TokenWriteOutcome replaceStoreFile(const std::string& path, const WriteContents& writeContents, const TokenStoreSync* sync = nullptr)
{
    std::string temporary = path + ".tmp.XXXXXX";
    PAL::UnixFileDescriptor descriptor(::mkstemp(temporary.data()));
    if (!descriptor)
        return TokenWriteOutcome::IOError;
    /// Removes an unpublished temporary file at scope exit.
    struct TemporaryPathCleanup {
        const std::string& path;
        ~TemporaryPathCleanup()
        {
            if (!path.empty())
                ::unlink(path.c_str());
        }
    } cleanup { temporary };
    if (::fcntl(descriptor.get(), F_SETFD, FD_CLOEXEC) != 0 || ::fchmod(descriptor.get(), 0600) != 0)
        return TokenWriteOutcome::IOError;
    const auto closeFile = [](FILE* stream) { std::fclose(stream); };
    std::unique_ptr<FILE, decltype(closeFile)> file(::fdopen(descriptor.get(), "w"), closeFile);
    if (!file)
        return TokenWriteOutcome::IOError;
    (void)descriptor.release();
    const bool didWriteContents = writeContents(file.get());
    // Flush userspace buffers, then force the file's bytes to stable storage
    // ahead of the rename that makes them the store — and ahead of the close,
    // because syncing needs the open descriptor: otherwise the rename can
    // commit while the revoked-away bytes are still only in cache.
    // An earlier formatted write can fail with no buffered bytes left for
    // fflush to reject, so its result and the stream's sticky error count too.
    bool isTemporaryComplete = std::fflush(file.get()) == 0 && didWriteContents && std::ferror(file.get()) == 0;
    const int fileDescriptor = ::fileno(file.get());
    // A filesystem without fsync reports EINVAL, which marks the rewrite
    // isUnproven rather than failed: the bytes below still land, and landing
    // them is what a revoke needs now — only a crash could replay the old
    // store, and refusing the rewrite would leave the revoked token valid
    // with certainty instead of risk. Any other sync error fails outright.
    bool isUnproven = false;
    if (isTemporaryComplete) {
        const int synced = (sync == nullptr) ? ::fsync(fileDescriptor) : sync->syncFile(fileDescriptor);
        if (synced != 0) {
            if (errno == EINVAL) {
                isUnproven = true;
            } else {
                isTemporaryComplete = false;
            }
        }
    }
    // fclose can report a deferred write error, so its answer counts too — and
    // it runs on every path, since a descriptor left open here leaks.
    const bool closed = std::fclose(file.release()) == 0;
    if (!isTemporaryComplete || !closed)
        return TokenWriteOutcome::IOError;
    if (::rename(temporary.c_str(), path.c_str()) != 0)
        return TokenWriteOutcome::IOError;
    temporary.clear();
    // The rename itself is a directory mutation: sync the parent so a crash
    // cannot lose the rename while keeping both files. An unprovable parent
    // marks rather than fails — the rewrite already landed, and rolling it
    // back would revalidate the revoked record, which is the worst outcome.
    const std::string parent = tokenParentDirectory(path);
    const int parentSynced = (sync == nullptr) ? syncTokenParent(parent.c_str()) : sync->syncParent(parent.c_str());
    if (parentSynced != 0)
        isUnproven = true;
    return isUnproven ? TokenWriteOutcome::UnsupportedDurability : TokenWriteOutcome::Durable;
}

/// Rewrites the store durably (see ``TokenWriteOutcome``).
///
/// - Parameters:
///   - path: The store file.
///   - records: Exactly what it must hold after.
///   - sync: The durability syscalls; the real ones when null, injected
///     failures under test.
/// - Returns: How the rewrite landed; only ``Durable`` proves it.
[[nodiscard]] inline TokenWriteOutcome writeTokens(const std::string& path,
    const std::vector<TokenRecord>& records, const TokenStoreSync* sync = nullptr)
{
    const auto writeRecords = [&records](FILE* file) {
        for (const auto& record : records) {
            if (std::fprintf(file, "%s %u %llu %s\n", record.hashHex.c_str(), static_cast<unsigned>(record.scope.toRaw()), static_cast<unsigned long long>(record.expiryUnix), record.label.c_str()) < 0)
                return false;
        }
        return true;
    };
    return replaceStoreFile(path, writeRecords, sync);
}

// Constant-time compare of two equal-length hex digests (no early-out on the
// first differing byte), so the verify path never leaks a match position via
// timing.
inline bool digestsEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

// Verify `token` against the store. A missing token is reported distinctly so
// the HELLO gate can fail-closed only when require_token is on.
inline TokenVerdict verifyToken(const std::string& path, const std::string& token, std::uint64_t nowUnix)
{
    TokenVerdict verdict;
    if (token.empty()) {
        verdict.reason = Protocol::Sync::ByeReason::TokenMissing;
        return verdict;
    }
    // No store holds a token this long, so none is hashed to find that out.
    if (token.size() > Protocol::Sync::maxAuthTokenLength) {
        verdict.reason = Protocol::Sync::ByeReason::TokenInvalid;
        return verdict;
    }
    const std::string hash = Crypto::sha256Hex(token);
    const TokenStoreContents store = readTokenStore(path);
    // A store read in part proves nothing about the token: its record, or
    // the edit that removed it, may be in the part that went unread.
    if (!store.isWhole) {
        verdict.reason = Protocol::Sync::ByeReason::TokenInvalid;
        verdict.tokenID = hash;
        return verdict;
    }
    for (const auto& record : store.records) {
        if (!digestsEqual(record.hashHex, hash))
            continue;
        verdict.tokenID = hash;
        if (tokenExpiry(record, nowUnix) == TokenExpiry::Expired) {
            verdict.reason = Protocol::Sync::ByeReason::TokenExpired;
            return verdict;
        }
        verdict.ok = true; // reason is ignored when ok (stays at its default)
        verdict.scope = record.scope;
        verdict.expiryUnix = record.expiryUnix;
        return verdict;
    }
    verdict.reason = Protocol::Sync::ByeReason::TokenInvalid; // not found (never issued, or revoked)
    verdict.tokenID = hash;
    return verdict;
}

} // namespace Host
} // namespace Ubiquitty
