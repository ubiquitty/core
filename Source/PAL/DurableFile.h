// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <span>

namespace Ubiquitty::PAL {

/// Whether replacement occurred and both persistence barriers succeeded.
enum class DurableFileResult {
    NotCommitted,
    DurabilityUnknown,
    Durable,
};

/// Atomically replaces an application-owned file and checks its persistence
/// barriers. The parent must already exist durably; this function does not
/// create directories. The caller serializes operations on this destination.
/// Unsupported platforms or protection requirements refuse without writing.
///
/// - Parameters:
///   - path: An absolute trusted path, never a peer-supplied destination.
///   - buffer: Complete bytes to store in a new owner-only file.
/// - Returns: NotCommitted before rename, DurabilityUnknown if rename happened
///   but the directory flush failed, or Durable after both flushes. Unknown
///   must never be treated as safe persistence or as an unchanged old file.
[[nodiscard]] DurableFileResult storageWriteDurable(const char* path, std::span<const std::byte> buffer);

/// Appends bytes to an application-owned log, creating it owner-only.
///
/// A file this call creates is mode 0600 whatever the process's file-creation
/// mask; an existing file keeps its mode. A symbolic link at `path` is refused.
///
/// - Parameters:
///   - path: A trusted path, never a peer-supplied destination.
///   - bytes: The record to append, written with one call where it fits.
/// - Returns: True when every byte was written.
[[nodiscard]] bool storageAppendOwnerOnly(const char* path, std::span<const std::byte> bytes);

} // namespace Ubiquitty::PAL
