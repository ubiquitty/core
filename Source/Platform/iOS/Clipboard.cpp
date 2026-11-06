// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Clipboard.cpp — Ubiquitty::PAL's clipboard on iOS, as a seam the shell fills.
//
// UIPasteboard is Objective-C, and this frontend has none: the shell is Swift
// and stays Swift. So the PAL entry point lives here and the actual pasteboard
// lives in the shell, which installs itself once at startup — the same shape
// PAL.h already describes for the clock hook.
//
// Unset, a write is dropped rather than crashing. A terminal whose OSC 52 goes
// nowhere is a terminal with no clipboard, which is a true statement about a
// shell that has not installed one; a null call would be a false statement
// about the program.

#include "Platform/iOS/Clipboard.h"

#include "PAL/PAL.h"

#include <atomic>
#include <string>

namespace {

// The shell installs on the main thread at launch, and the terminal writes from
// whichever thread parsed the copy — OSC 52 arrives on the ingest thread. The
// atomic owns that synchronization, as `PAL::installContext` does for the
// context: install publishes with release and a write loads with acquire.
std::atomic<Ubiquitty::PAL::iOSClipboardWriter> installedWriter { nullptr };

} // namespace

namespace Ubiquitty::PAL {

void installIOSClipboardWriter(iOSClipboardWriter writer)
{
    installedWriter.store(writer, std::memory_order_release);
}

void clipboardSetText(std::string_view text)
{
    if (auto writer = installedWriter.load(std::memory_order_acquire)) {
        writer(text.data(), text.size());
    }
}

} // namespace Ubiquitty::PAL
