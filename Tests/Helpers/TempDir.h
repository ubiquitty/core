// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace Ubiquitty::Test {

/// A scoped temporary directory.
///
/// Creates a unique directory via `mkdtemp` and removes it on destruction.
/// Move-only: a directory has one owner and its path is the single source
/// of truth, so a copy would leave two destructors racing to remove the
/// same path.
///
/// - Parameters:
///   - prefix: The `mkdtemp` template, which must end in `XXXXXX`.
/// - Returns: A valid directory when `mkdtemp` succeeds, otherwise an
///   invalid one.
class TempDir {
public:
    /// Creates a temporary directory.
    ///
    /// - Parameter prefix: The `mkdtemp` template.
    explicit TempDir(std::string_view prefix = "/tmp/ubiquitty-test-XXXXXX")
        : m_path(prefix)
    {
        // `mkdtemp` mutates its buffer, so a mutable copy is required.
        // `m_path` owns the buffer and the resulting path.
        if (::mkdtemp(m_path.data()) == nullptr)
            m_path.clear();
    }

    /// Removes the directory and everything under it.
    ///
    /// Best effort: the test owns everything under the directory, and a leak
    /// is a `/tmp` accumulation across agent worktrees. The error code is
    /// taken so a directory already gone cannot throw out of a destructor.
    ~TempDir()
    {
        if (m_path.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Moves a temporary directory.
    ///
    /// - Parameter other: The directory to move from.
    TempDir(TempDir&& other) noexcept
        : m_path(std::move(other.m_path))
    {
        other.m_path.clear();
    }

    /// Moves a temporary directory.
    ///
    /// - Parameter other: The directory to move from.
    /// - Returns: This directory.
    TempDir& operator=(TempDir&& other) noexcept
    {
        if (this != &other) {
            m_path = std::move(other.m_path);
            other.m_path.clear();
        }
        return *this;
    }

    /// Whether the directory was created.
    ///
    /// - Returns: True when `mkdtemp` succeeded.
    [[nodiscard]] bool valid() const { return !m_path.empty(); }

    /// The directory path.
    ///
    /// - Returns: The path, or empty when invalid.
    [[nodiscard]] const std::string& path() const { return m_path; }

    /// The directory path as a view.
    ///
    /// - Returns: The path view.
    [[nodiscard]] std::string_view view() const { return std::string_view(m_path); }

private:
    // `mkdtemp` requires a mutable NUL-terminated buffer; `std::string`
    // provides one via `data()` in C++17 and later.
    std::string m_path;
};

} // namespace Ubiquitty::Test
