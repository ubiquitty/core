// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace Ubiquitty::Host {

/// An owned grant-file stream whose descriptor was verified as regular.
using GrantFile = std::unique_ptr<FILE, int (*)(FILE*)>;

/// Opens one regular grant file without following links or waiting on a FIFO.
///
/// - Parameter path: The file to open for reading.
/// - Returns: The owned stream, or the operating system's error.
[[nodiscard]] inline std::expected<GrantFile, int> openGrantFile(const std::string& path)
{
    PAL::UnixFileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!descriptor)
        return std::unexpected(errno ? errno : EIO);
    struct stat information {};
    if (::fstat(descriptor.get(), &information) != 0)
        return std::unexpected(errno ? errno : EIO);
    if (!S_ISREG(information.st_mode))
        return std::unexpected(EINVAL);
    GrantFile file(::fdopen(descriptor.get(), "rb"), &std::fclose);
    if (!file)
        return std::unexpected(errno ? errno : EIO);
    (void)descriptor.release();
    return std::expected<GrantFile, int>(std::move(file));
}

/// One file's identity, size, and times as `stat` describes them.
///
/// Every grant-file writer renames a new file over the old one, so an equal
/// stamp names the same bytes; an edit made in place still moves the size or
/// a time.
struct GrantFileStamp {
    /// The `errno` of a failed examination, or 0.
    int error = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::int64_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0;
    bool operator==(const GrantFileStamp&) const = default;
};

/// Describes a file that `stat`, `lstat`, or `fstat` examined.
///
/// - Parameter information: What the examination found.
/// - Returns: Its device, inode, size, and modification and change times.
[[nodiscard]] inline GrantFileStamp grantFileStamp(const struct stat& information)
{
    GrantFileStamp stamp;
    stamp.device = static_cast<std::uint64_t>(information.st_dev);
    stamp.inode = static_cast<std::uint64_t>(information.st_ino);
    stamp.size = static_cast<std::int64_t>(information.st_size);
#if defined(__APPLE__)
    stamp.modifiedSeconds = information.st_mtimespec.tv_sec;
    stamp.modifiedNanoseconds = information.st_mtimespec.tv_nsec;
    stamp.changedSeconds = information.st_ctimespec.tv_sec;
    stamp.changedNanoseconds = information.st_ctimespec.tv_nsec;
#else
    stamp.modifiedSeconds = information.st_mtim.tv_sec;
    stamp.modifiedNanoseconds = information.st_mtim.tv_nsec;
    stamp.changedSeconds = information.st_ctim.tv_sec;
    stamp.changedNanoseconds = information.st_ctim.tv_nsec;
#endif
    return stamp;
}

/// Examines the entry at `path` itself, without following a final link.
///
/// - Parameter path: The grant file.
/// - Returns: Its stamp, or one carrying only the `errno` of a failed `lstat`.
[[nodiscard]] inline GrantFileStamp examineGrantFile(const std::string& path)
{
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0)
        return { .error = errno ? errno : EIO };
    return grantFileStamp(information);
}

/// The work and storage accepted from one grant-file stream.
struct GrantFileReadLimits {
    /// Bytes in one logical line, including its line end.
    std::size_t maxLineBytes;
    /// Bytes accepted across the file.
    std::size_t maxFileBytes = std::numeric_limits<std::size_t>::max();
    /// Logical lines accepted, including headers and blank lines.
    std::size_t maxLines = std::numeric_limits<std::size_t>::max();
};

/// Whether every byte of a grant-file stream was accepted.
struct GrantFileReadResult {
    bool isWhole = false;
    bool hasInvalidBytes = false;
    bool hasOverlongLine = false;
    /// The operating system's read error, or zero.
    int readError = 0;
};

/// Reads counted logical lines from a borrowed stream.
///
/// NULs and I/O errors refuse the stream without completing its pending line.
/// An overlong line is discarded whole; later lines may still be listed, but
/// the result withholds authority for the entire stream. Clean EOF can end a
/// final line without a terminator. The caller owns publication of the lines.
///
/// - Parameters:
///   - file: A non-null stream that remains owned by the caller.
///   - limits: The accepted line, byte, and line-count bounds.
///   - takeLine: Accepts one complete line, returning false to refuse the stream.
/// - Returns: Whether the complete stream was accepted, with any I/O error.
template<typename TakeLine>
[[nodiscard]] GrantFileReadResult readGrantFileLines(FILE* file, const GrantFileReadLimits& limits, const TakeLine& takeLine)
{
    assert(file);
    GrantFileReadResult result;
    std::string line;
    bool isDropping = false;
    std::size_t lines = 0;
    const auto finishLine = [&] {
        if (lines == limits.maxLines)
            return false;
        ++lines;
        const bool isAccepted = isDropping || takeLine(std::move(line));
        line.clear();
        isDropping = false;
        return isAccepted;
    };
    std::array<char, 1024> chunk {};
    std::size_t totalBytes = 0;
    for (;;) {
        errno = 0;
        const auto count = std::fread(chunk.data(), 1, chunk.size(), file);
        if (std::ferror(file)) {
            result.readError = errno ? errno : EIO;
            return result;
        }
        if (!count)
            break;
        if (count > limits.maxFileBytes - totalBytes)
            return result;
        totalBytes += count;
        for (std::size_t index = 0; index < count; ++index) {
            const char byte = chunk[index];
            if (!byte) {
                result.hasInvalidBytes = true;
                return result;
            }
            if (!isDropping) {
                if (line.size() == limits.maxLineBytes) {
                    result.hasOverlongLine = true;
                    isDropping = true;
                    line.clear();
                } else
                    line.push_back(byte);
            }
            if (byte == '\n' && !finishLine())
                return result;
        }
    }
    if ((!line.empty() || isDropping) && !finishLine())
        return result;
    result.isWhole = !result.hasOverlongLine;
    return result;
}

} // namespace Ubiquitty::Host
