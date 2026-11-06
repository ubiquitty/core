// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace Ubiquitty::PAL {

/// Why a bounded regular-file read produced no owner.
enum class FileReadError {
    Unavailable,
    TooLarge,
    AllocationFailed,
    Changed,
};

/// An exact file snapshot, never a partially filled successful result.
struct BoundedFileBytes {
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size = 0;

    /// Borrows the complete file bytes.
    ///
    /// - Returns: Bytes valid while this owner remains alive and unchanged.
    [[nodiscard]] std::span<const std::uint8_t> view() const { return { bytes.get(), size }; }
};

/// Reads one regular file without following its final symlink. Size admission
/// precedes allocation and reading; the open descriptor pins the file across
/// path replacement. A concurrent length change is refused, not truncated.
/// Same-length mutation still requires the caller's authenticated validation.
///
/// - Parameters:
///   - path: A trusted, NUL-terminated application storage path.
///   - maxBytes: The largest admitted file, including any envelope overhead.
/// - Returns: An exact fallible owner, or a refusal with no partial output.
[[nodiscard]] std::expected<BoundedFileBytes, FileReadError> storageReadBounded(const char* path, std::size_t maxBytes);

} // namespace Ubiquitty::PAL
