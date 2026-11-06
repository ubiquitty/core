// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The multi-buffer clipboard behind OSC 52: c=CLIPBOARD, p=PRIMARY,
// q=SECONDARY, s=select, 0-7=cut buffers.
//
// Holds every buffer except Clipboard, which belongs to the host's system
// pasteboard so ⌘C/⌘V (and the X11/Wayland equivalents) keep working. Nothing
// here touches a pasteboard; the host's live clipboard text is an argument.
//
// Budgets are counted in UTF-8 scalars and cuts land on scalar boundaries.

#include "Base/SwiftInterop.h"
#include "Core/Title/Truncate.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::OSC {

// Bound the recent-copy ring and each preview so a picker can never grow without
// limit from one enormous clipboard.
inline constexpr std::size_t maxRecentClipboardCopies = 8;
inline constexpr std::size_t maxClipboardPreviewScalars = 80;

// The kind of a paste-picker entry. Buffer = an OSC 52 selection buffer (index
// is the ClipboardBuffer raw value); Recent = an entry from the user-copy ring.
/// Which store a paste-picker row came from.
enum class PasteEntryKind : uint8_t {
    Buffer = 0,
    Recent = 1,
};

/// One row of the paste picker.
///
/// `preview` is single-line and length-capped; `label` is human text —
/// "clipboard", "cut 3", "recent copy".
struct PasteEntry {
    PasteEntryKind kind {};
    int index {};
    std::string label;
    std::string preview;
};

// Cut buffers are labelled by their OSC 52 digit: `Cut0 + n` reads "cut n".
[[nodiscard]] std::string clipboardBufferLabel(ClipboardBuffer buffer);

// Collapse every whitespace run to a single space, trim both ends, and elide
// anything past maxClipboardPreviewScalars scalars, so a multi-line or oversized
// value renders as one tidy row. An elided result is that many scalars plus a
// trailing U+2026 ellipsis; a fitting one is returned untouched.
//
// The collapsed set is ASCII TAB/LF/VT/FF/CR/SPACE plus the Unicode separators
// that would otherwise break a single-line row: U+0085 NEL, U+00A0 NBSP,
// U+1680, U+2000–U+200A, U+2028 LS, U+2029 PS, U+202F, U+205F, U+3000.
[[nodiscard]] std::string clipboardPreview(std::string_view text);

// Any process-wide singleton is the frontend's, not this class's.
//
// Thread-safe: OSC 52 writes arrive on the terminal's read thread while the
// picker reads on the UI thread.
/// The multi-buffer clipboard OSC 52 writes into.
///
/// Any process-wide singleton is the frontend's, not this class's. Thread-safe:
/// OSC 52 writes arrive on the terminal's read thread while the picker reads on
/// the UI thread.
class UBIQUITTY_NONCOPYABLE ClipboardStore {
public:
    ClipboardStore() = default;
    ClipboardStore(const ClipboardStore&) = delete;
    ClipboardStore& operator=(const ClipboardStore&) = delete;

    /// Transfers the stored payloads while retaining an independent mutex.
    ClipboardStore(ClipboardStore&&) noexcept;

    /// Stores bytes into one non-`Clipboard` buffer.
    ///
    /// `Clipboard` is a no-op here and empty from ``bufferBytes``: the host
    /// pasteboard owns it, so a copy kept here would be the one that goes
    /// stale. Empty bytes clear the buffer.
    ///
    /// - Parameters:
    ///   - buffer: The OSC 52 buffer to write.
    ///   - bytes: The text to store, or empty to clear.
    void setBuffer(ClipboardBuffer buffer, std::string_view bytes);
    [[nodiscard]] std::string bufferBytes(ClipboardBuffer buffer) const;

    /// Pushes a user copy onto the bounded ring, most-recent first.
    ///
    /// Empty text is ignored, and a duplicate MOVES to the front rather than
    /// appearing twice — a ring that repeated an entry would spend its bound on
    /// one value.
    ///
    /// - Parameter text: The copied text.
    void recordRecentCopy(std::string_view text);
    [[nodiscard]] std::vector<std::string> recentCopies() const;

    // One entry per NON-empty item, ordered system clipboard, then internal
    // buffers in enum order, then recent copies. `systemClipboardText` is the
    // host's live pasteboard text, empty when it holds none.
    [[nodiscard]] std::vector<PasteEntry> pasteEntries(std::string_view systemClipboardText) const;

    // The full text to paste for a picker entry, or empty when that entry is
    // gone. Same systemClipboardText contract as pasteEntries.
    [[nodiscard]] std::string textForEntry(PasteEntryKind kind, int index,
        std::string_view systemClipboardText) const;

private:
    mutable std::mutex m_lock;
    std::array<std::string, static_cast<std::size_t>(ClipboardBuffer::Count)> m_buffers;
    std::deque<std::string> m_recent;
};

} // namespace Ubiquitty::OSC
