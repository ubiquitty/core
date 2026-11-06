// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Ubiquitty::PAL {

using DropFileBuffer = std::span<char>;

/// Classifies file failures without exposing platform errno values.
enum class DropFileError : std::uint8_t {
    None,
    Invalid,
    NotFound,
    NotPermitted,
    NoMemory,
    TooManyFiles,
    InputOutput
};

/// Identifies a bounded read and the file, link or directory it represents.
struct DropFileRead {
    std::uint64_t token = 0;
    /// Zero for a regular file, one for a symlink, otherwise a directory handle.
    std::int64_t kind = 0;
    DropFileError error = DropFileError::None;
};

/// Describes bytes written into the caller's buffer, with an explicit EOF.
struct DropFileChunk {
    std::size_t size = 0;
    DropFileError error = DropFileError::None;
    bool isComplete = false;
};

/// Descriptor-scoped access to paths supplied by an actual native drop.
/// Calls are serialized on the native file worker, outside the model/UI locks.
class DropFiles {
public:
    /// Creates an empty set of native file grants.
    DropFiles();
    /// Closes all granted paths, reads and directory handles.
    ~DropFiles();
    DropFiles(const DropFiles&) = delete;
    DropFiles& operator=(const DropFiles&) = delete;
    /// Transfers file grants without duplicating their descriptors.
    DropFiles(DropFiles&&) noexcept;
    DropFiles& operator=(DropFiles&&) noexcept;

    /// Pins the parent of one native dropped path. Indices are one-based.
    [[nodiscard]] DropFileError addPath(std::string_view);
    [[nodiscard]] DropFileRead readRoot(std::size_t index);
    [[nodiscard]] DropFileRead readChild(std::int64_t directory, std::size_t index);
    [[nodiscard]] DropFileChunk read(std::uint64_t token, DropFileBuffer);
    /// Closes a directory and invalidates its handle for future requests.
    void closeDirectory(std::int64_t);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Ubiquitty::PAL
