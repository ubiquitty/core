// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

/// Supplied by the embedder, in Swift: file protection classes are a
/// Foundation contract. Returns an owned descriptor,
/// or -1 with errno set.
extern "C" int ubiquittyCreateCompleteProtectedFile(int directory, const char* name);

namespace Ubiquitty::Platform {

/// Exclusively creates an empty Complete-protected file in a pinned private
/// directory. No payload is written before protection and identity agree.
///
/// - Parameters:
///   - directory: Borrowed descriptor for the application-owned parent.
///   - name: A fresh unpredictable leaf, without separators or dot components.
/// - Returns: The protected writable owner, or an invalid descriptor. An
///   existing leaf, including a symlink, is never replaced or removed.
[[nodiscard]] inline PAL::UnixFileDescriptor createCompleteProtectedFile(int directory, const char* name)
{
    return PAL::UnixFileDescriptor(ubiquittyCreateCompleteProtectedFile(directory, name));
}

} // namespace Ubiquitty::Platform
