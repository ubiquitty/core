// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/DurableFile.h"

namespace Ubiquitty::Platform {

/// Runs the commit ordering against one already-created temporary file.
/// Operations owns its descriptors and removes an uncommitted temporary file
/// on destruction. Its write method handles interrupted syscalls and reports
/// zero on refusal; every flush must report failure rather than downgrade it.
///
/// - Parameters:
///   - operations: The scoped file owner and its filesystem operations.
///   - bytes: The entire replacement content.
/// - Returns: The exact replacement and persistence state.
template<typename Operations>
[[nodiscard]] PAL::DurableFileResult commitDurableFile(Operations& operations, std::span<const std::byte> bytes)
{
    while (!bytes.empty()) {
        const auto count = operations.write(bytes);
        if (!count || count > bytes.size())
            return PAL::DurableFileResult::NotCommitted;
        bytes = bytes.subspan(count);
    }
    if (!operations.flushFile() || !operations.closeFile() || !operations.replace())
        return PAL::DurableFileResult::NotCommitted;
    if (!operations.flushDirectory())
        return PAL::DurableFileResult::DurabilityUnknown;
    return PAL::DurableFileResult::Durable;
}

} // namespace Ubiquitty::Platform
