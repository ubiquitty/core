// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// ClipboardStore.cpp — see Core/OSC/ClipboardStore.h.
//

#include "Core/OSC/ClipboardStore.h"

#include <algorithm>
#include <utility>

namespace Ubiquitty::OSC {

namespace {

// Decode one UTF-8 scalar at `text[offset]`, advancing `offset` past it. An
// invalid lead or truncated sequence yields the raw byte and advances one: a
// preview is display text, and must not drop bytes it cannot interpret.
uint32_t nextScalar(std::string_view text, std::size_t& offset)
{
    const auto byteAt = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byteAt(offset);

    std::size_t width = 1;
    uint32_t scalar = lead;
    if ((lead & 0xE0) == 0xC0) {
        width = 2;
        scalar = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        width = 3;
        scalar = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        width = 4;
        scalar = lead & 0x07u;
    }

    if (width == 1 || offset + width > text.size()) {
        ++offset;
        return lead;
    }
    for (std::size_t i = 1; i < width; ++i) {
        if ((byteAt(offset + i) & 0xC0) != 0x80) { // truncated sequence
            ++offset;
            return lead;
        }
        scalar = (scalar << 6) | (byteAt(offset + i) & 0x3Fu);
    }
    offset += width;
    return scalar;
}

// The set collapsed by clipboardPreview.
bool isPreviewWhitespace(uint32_t scalar)
{
    switch (scalar) {
    case 0x09: // TAB
    case 0x0A: // LF
    case 0x0B: // VT
    case 0x0C: // FF
    case 0x0D: // CR
    case 0x20: // SPACE
    case 0x85: // NEL
    case 0xA0: // NBSP
    case 0x1680: // OGHAM SPACE MARK
    case 0x2028: // LINE SEPARATOR
    case 0x2029: // PARAGRAPH SEPARATOR
    case 0x202F: // Narrow no-break space
    case 0x205F: // MEDIUM MATHEMATICAL SPACE
    case 0x3000: // IDEOGRAPHIC SPACE
        return true;
    default:
        return scalar >= 0x2000 && scalar <= 0x200A; // EN QUAD … HAIR SPACE
    }
}

bool isRealBuffer(ClipboardBuffer buffer)
{
    const auto raw = static_cast<std::size_t>(buffer);
    return buffer != ClipboardBuffer::Clipboard
        && raw < static_cast<std::size_t>(ClipboardBuffer::Count);
}

} // namespace

std::string clipboardBufferLabel(ClipboardBuffer buffer)
{
    switch (buffer) {
    case ClipboardBuffer::Clipboard:
        return "clipboard";
    case ClipboardBuffer::Primary:
        return "primary";
    case ClipboardBuffer::Secondary:
        return "secondary";
    case ClipboardBuffer::Select:
        return "select";
    case ClipboardBuffer::Cut0:
    case ClipboardBuffer::Cut1:
    case ClipboardBuffer::Cut2:
    case ClipboardBuffer::Cut3:
    case ClipboardBuffer::Cut4:
    case ClipboardBuffer::Cut5:
    case ClipboardBuffer::Cut6:
    case ClipboardBuffer::Cut7: {
        const int n = static_cast<int>(buffer) - static_cast<int>(ClipboardBuffer::Cut0);
        return "cut " + std::to_string(n);
    }
    case ClipboardBuffer::Count:
        break;
    }
    return "buffer";
}

std::string clipboardPreview(std::string_view text)
{
    std::string collapsed;
    collapsed.reserve(std::min<std::size_t>(text.size(), maxClipboardPreviewScalars * 4));

    bool pendingSpace = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t start = offset;
        const uint32_t scalar = nextScalar(text, offset);
        if (isPreviewWhitespace(scalar)) {
            // Leading whitespace never emits, so this also trims the head; the
            // tail trims by never flushing a pending space at the end.
            pendingSpace = !collapsed.empty();
            continue;
        }
        if (pendingSpace) {
            collapsed.push_back(' ');
            pendingSpace = false;
        }
        collapsed.append(text.substr(start, offset - start));
    }

    if (Title::titleScalarLength(collapsed) <= maxClipboardPreviewScalars)
        return collapsed;
    return Title::titleHead(collapsed, maxClipboardPreviewScalars) + "…";
}

ClipboardStore::ClipboardStore(ClipboardStore&& other) noexcept
{
    std::lock_guard lock(other.m_lock);
    m_buffers = std::move(other.m_buffers);
    m_recent = std::move(other.m_recent);
}

void ClipboardStore::setBuffer(ClipboardBuffer buffer, std::string_view bytes)
{
    if (!isRealBuffer(buffer))
        return;
    const std::scoped_lock locked(m_lock);
    m_buffers[static_cast<std::size_t>(buffer)].assign(bytes);
}

std::string ClipboardStore::bufferBytes(ClipboardBuffer buffer) const
{
    if (!isRealBuffer(buffer))
        return {};
    const std::scoped_lock locked(m_lock);
    return m_buffers[static_cast<std::size_t>(buffer)];
}

void ClipboardStore::recordRecentCopy(std::string_view text)
{
    if (text.empty())
        return;
    const std::scoped_lock locked(m_lock);
    // A re-copy promotes rather than duplicates: the ring holds distinct values.
    const auto duplicate = std::ranges::find(m_recent, text);
    if (duplicate != m_recent.end())
        m_recent.erase(duplicate);
    m_recent.emplace_front(text);
    while (m_recent.size() > maxRecentClipboardCopies)
        m_recent.pop_back();
}

std::vector<std::string> ClipboardStore::recentCopies() const
{
    const std::scoped_lock locked(m_lock);
    return { m_recent.begin(), m_recent.end() };
}

std::vector<PasteEntry> ClipboardStore::pasteEntries(std::string_view systemClipboardText) const
{
    std::vector<PasteEntry> entries;

    if (!systemClipboardText.empty()) {
        entries.push_back({ PasteEntryKind::Buffer,
            static_cast<int>(ClipboardBuffer::Clipboard),
            clipboardBufferLabel(ClipboardBuffer::Clipboard),
            clipboardPreview(systemClipboardText) });
    }

    const std::scoped_lock locked(m_lock);
    for (std::size_t i = 0; i < m_buffers.size(); ++i) {
        const auto buffer = static_cast<ClipboardBuffer>(i);
        if (!isRealBuffer(buffer) || m_buffers[i].empty())
            continue;
        entries.push_back({ PasteEntryKind::Buffer, static_cast<int>(i),
            clipboardBufferLabel(buffer), clipboardPreview(m_buffers[i]) });
    }
    for (std::size_t i = 0; i < m_recent.size(); ++i) {
        entries.push_back({ PasteEntryKind::Recent, static_cast<int>(i),
            "recent copy", clipboardPreview(m_recent[i]) });
    }
    return entries;
}

std::string ClipboardStore::textForEntry(PasteEntryKind kind, int index,
    std::string_view systemClipboardText) const
{
    if (kind == PasteEntryKind::Recent) {
        const std::scoped_lock locked(m_lock);
        if (index < 0 || static_cast<std::size_t>(index) >= m_recent.size())
            return {};
        return m_recent[static_cast<std::size_t>(index)];
    }
    if (index == static_cast<int>(ClipboardBuffer::Clipboard))
        return std::string(systemClipboardText);
    if (index < 0 || index >= static_cast<int>(ClipboardBuffer::Count))
        return {};
    return bufferBytes(static_cast<ClipboardBuffer>(index));
}

} // namespace Ubiquitty::OSC
