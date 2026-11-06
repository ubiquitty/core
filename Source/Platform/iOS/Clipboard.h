// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The clipboard seam the iOS shell fills. See Clipboard.cpp for why the
// pasteboard itself lives in Swift.

#include <cstddef>

namespace Ubiquitty::PAL {

/// Receives text the terminal asked to place on the clipboard.
///
/// C linkage because the shell installs a Swift `@convention(c)` function, and
/// that is a C function however it is spelled here. Its parameters are a
/// pointer and a length for the same reason — the C ABI has no `string_view` —
/// and the text is not NUL-terminated, since a clipboard payload is bytes.
///
/// - Parameters:
///   - utf8: The text. Borrowed for the duration of the call.
///   - byteCount: How many bytes `utf8` holds.
extern "C" {
using iOSClipboardWriter = void (*)(const char* utf8, std::size_t byteCount);
}

/// Installs the shell's pasteboard. Call once, at startup.
///
/// - Parameter writer: The sink, or null to drop writes.
void installIOSClipboardWriter(iOSClipboardWriter writer);

} // namespace Ubiquitty::PAL
