// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/DropFiles.h"

#include <string>

namespace Ubiquitty::PAL {

using DropFileBytes = std::span<const char>;
/// Distinguishes a data chunk from the entry's empty final marker.
enum class DropEntryCompletion : std::uint8_t {
    More,
    Complete
};

/// Materializes remote drag entries under an isolated private directory.
/// All methods, including destruction, run on the native file worker.
class ReceivedDropFiles {
public:
    explicit ReceivedDropFiles(std::string_view temporaryDirectory);
    explicit ReceivedDropFiles(const std::string& temporaryDirectory)
        : ReceivedDropFiles(std::string_view(temporaryDirectory))
    {
    }
    /// Removes staging files and closes their owned descriptors.
    ~ReceivedDropFiles();
    ReceivedDropFiles(const ReceivedDropFiles&) = delete;
    ReceivedDropFiles& operator=(const ReceivedDropFiles&) = delete;
    /// Transfers the staging tree and its exclusive cleanup responsibility.
    ReceivedDropFiles(ReceivedDropFiles&&) noexcept;
    ReceivedDropFiles& operator=(ReceivedDropFiles&&) noexcept;

    [[nodiscard]] DropFileError addRoot(std::string_view name);
    [[nodiscard]] DropFileError writeEntry(std::size_t root, std::int64_t parent, std::size_t child,
        std::int64_t kind, DropFileBytes, DropEntryCompletion);
    [[nodiscard]] bool isComplete(std::size_t root) const;
    /// Returns an empty path until every entry belonging to this root is complete.
    [[nodiscard]] std::string path(std::size_t root) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Ubiquitty::PAL
