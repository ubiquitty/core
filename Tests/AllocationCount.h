// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Counts the heap allocations a test binary makes through the global
// allocation functions, so a case can assert that a path makes none.
//
// A program defines its replacement allocation functions once, so the test's
// own translation unit replaces them, each `operator new` forwarding to
// `countedAllocation` and each `operator delete` to `std::free`. Every form a
// library may pair is replaced — scalar and array, throwing and nothrow, at the
// default alignment and over it — so each block the replaced `operator delete`
// frees came from the replaced `operator new`, under a sanitizer too.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace UbiquittyTest {

/// The allocations this process has made through the global allocation
/// functions, on any thread.
[[nodiscard]] inline std::atomic<std::size_t>& processAllocationCount()
{
    static std::atomic<std::size_t> count { 0 };
    return count;
}

/// Counts the allocations made while it is alive.
///
/// The count is process-wide, so a case reads it around work that runs on
/// its own thread with no other thread allocating.
class AllocationCount {
public:
    AllocationCount()
        : m_start(processAllocationCount().load(std::memory_order_acquire))
    {
    }

    /// The allocations made since this was created.
    [[nodiscard]] std::size_t made() const { return processAllocationCount().load(std::memory_order_acquire) - m_start; }

private:
    std::size_t m_start;
};

/// Takes one block from the system allocator and counts it.
///
/// - Parameter size: The bytes asked for.
/// - Returns: The block, or null when the system allocator has none.
[[nodiscard]] inline void* countedAllocation(std::size_t size) noexcept
{
    processAllocationCount().fetch_add(1, std::memory_order_acq_rel);
    return std::malloc(size ? size : 1);
}

/// Takes one block aligned past the default from the system allocator and
/// counts it.
///
/// - Parameters:
///   - size: The bytes asked for.
///   - alignment: The alignment asked for, a power of two.
/// - Returns: The block, or null when the system allocator has none.
[[nodiscard]] inline void* countedAlignedAllocation(std::size_t size, std::align_val_t alignment) noexcept
{
    processAllocationCount().fetch_add(1, std::memory_order_acq_rel);
    const auto bytes = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    void* block = nullptr;
    return posix_memalign(&block, bytes, size ? size : 1) == 0 ? block : nullptr;
}

} // namespace UbiquittyTest
