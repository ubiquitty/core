// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Ubiquitty {

/// A heap byte buffer that grows toward a limit and refuses to pass it.
///
/// The three quantities a growable buffer needs — the allocation, how much of it
/// holds payload, and how much of it exists — are only meaningful together. As
/// three separate parameters they can be passed in the wrong order, advanced in
/// one place and not another, or cleared by two call sites that disagree about
/// which of them to clear.
///
/// The limit is an argument to ``append`` rather than a member. It is a
/// property of the payload being accumulated rather than of the storage, and one
/// buffer serves payloads with different limits.
///
/// Growth doubles from 1024 and stops at the limit, so a payload approaching
/// the limit allocates a bounded number of times rather than once per append.
/// The extra byte every capacity carries is for a terminator a reader may append
/// without a further check.
struct ByteBuffer {
    char* data {};
    int length {};
    int capacity {};

    /// Releases the storage and returns to the empty state.
    ///
    /// Clears all three fields together. Clearing the length without the capacity
    /// leaves an allocation the buffer no longer describes, which is the shape
    /// two hand-written teardowns of this buffer disagreed about.
    void reset()
    {
        std::free(data);
        data = nullptr;
        length = 0;
        capacity = 0;
    }

    /// Empties the buffer, releasing its storage when the allocation is larger
    /// than `maxRetainedCapacity`.
    ///
    /// Growth only ever doubles, so a buffer that held one large payload keeps
    /// that allocation for as long as its owner lives unless something gives
    /// it back. An allocation within the bound is kept for the next payload.
    ///
    /// - Parameter maxRetainedCapacity: The largest allocation kept.
    void clearReleasingAbove(int maxRetainedCapacity)
    {
        if (capacity > maxRetainedCapacity) {
            reset();
            return;
        }
        length = 0;
    }

    /// Appends bytes, refusing rather than growing past `limit`.
    ///
    /// - Parameters:
    ///   - bytes: The payload to append.
    ///   - limit: The limit on total length. A request that would pass it
    ///     appends nothing and leaves the buffer as it was.
    /// - Returns: `false` when the append would exceed `limit` or when the
    ///   allocation fails. The first is the decompression-bomb refusal and the
    ///   second is exhaustion; a caller that must tell them apart has to compare
    ///   `length` against `limit` itself.
    [[nodiscard]] bool append(std::string_view bytes, int limit)
    {
        // Refused before it is narrowed, because the narrowing is what makes the
        // rest of this function read backwards. `bytes.size()` is unsigned, and a
        // view of two gibibytes or more truncates to a negative `int`: then
        // `limit - byteCount` grows, so the overflow test below reports room;
        // `need` comes out smaller than `length` rather than larger; the copy is
        // skipped by its own `> 0` guard, which looks like the refusal working;
        // and the terminator writes at `data[need]`, a negative offset from the
        // allocation. Every step reads like a bounds check doing its job.
        //
        // Compared where the size is still unsigned, so `byteCount` is
        // non-negative and no greater than `limit` by construction. A negative
        // limit is refused rather than cast, since widening one is the same
        // defect pointed the other way.
        if (limit < 0 || bytes.size() > static_cast<std::size_t>(limit))
            return false;
        const int byteCount = static_cast<int>(bytes.size());
        if (length > limit - byteCount)
            return false;
        const int need = length + byteCount;
        if (need + 1 > capacity) {
            int newCapacity = (capacity > 0) ? capacity : 1024;
            while (newCapacity < need + 1) {
                if (newCapacity > limit / 2) {
                    newCapacity = limit + 1;
                    break;
                }
                newCapacity *= 2;
            }
            if (newCapacity > limit + 1)
                newCapacity = limit + 1;
#if UBIQUITTY_ENABLE_TEST_HOOKS
            // Test seam: the refusal below is the only branch here no input can
            // reach. Growth stops as soon as capacity covers the append, so a
            // caller cannot ask for an allocation large enough to fail — the
            // allocator has to be made to decline. Production never sets this.
            char* const grown = s_shouldFailNextGrowForTest
                ? nullptr
                : static_cast<char*>(std::realloc(data, static_cast<std::size_t>(newCapacity)));
#else
            char* const grown = static_cast<char*>(std::realloc(data, static_cast<std::size_t>(newCapacity)));
#endif
            if (!grown)
                return false;
            data = grown;
            capacity = newCapacity;
        }
        // Guarded rather than unconditional: an empty `std::string_view` may
        // carry a null pointer, and memcpy from null is undefined even for zero
        // bytes.
        if (byteCount > 0)
            std::memcpy(data + length, bytes.data(), static_cast<std::size_t>(byteCount));
        length = need;
        // The terminator every capacity reserves room for. Callers read this
        // buffer as a C string, so it is part of the contract rather than a
        // convenience.
        data[length] = 0;
        return true;
    }

#if UBIQUITTY_ENABLE_TEST_HOOKS
    /// Makes the next growth attempt behave as though the allocator declined.
    ///
    /// The refusal it reaches is what stops `append` writing into an allocation
    /// that did not grow, and no payload can provoke it: the growth loop asks
    /// only for what the append needs.
    static inline bool s_shouldFailNextGrowForTest = false;
#endif
};

} // namespace Ubiquitty
