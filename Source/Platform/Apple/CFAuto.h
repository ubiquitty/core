// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFError.h>

#include <memory>
#include <type_traits>

namespace Ubiquitty::Apple {

/// Releases an owned CoreFoundation reference.
///
/// `std::unique_ptr` invokes the deleter only for a non-null stored pointer.
struct CFDeleter {
    /// Releases the non-null reference passed by its owner.
    ///
    /// - Parameter object: The owned reference.
    void operator()(CFTypeRef object) const noexcept
    {
        ::CFRelease(object);
    }
};

/// Owns a reference returned by a CoreFoundation Create or Copy function.
///
/// Adopts the existing +1 reference without retaining it. Move transfers
/// ownership; destruction and reset release the owned reference.
///
/// - Parameter T: The framework pointer type, such as `CFErrorRef`.
template<typename T>
    requires std::is_pointer_v<T>
using UniqueCF = std::unique_ptr<std::remove_pointer_t<T>, CFDeleter>;

/// Owns a CoreFoundation error reference.
using UniqueCFError = UniqueCF<CFErrorRef>;

} // namespace Ubiquitty::Apple
