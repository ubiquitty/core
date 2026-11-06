// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Clipboard/ReadStage.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace Ubiquitty::Clipboard {

namespace {

// Caseless ASCII equality — MIME names match caselessly on the seam.
[[nodiscard]] bool equalsCaseless(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

} // namespace

int Snapshot::typeList(std::span<char> out) const
{
    std::size_t length = 0;
    for (const SnapshotItem& item : m_items) {
        const std::size_t needed = item.mime.size() + (length > 0 ? 1 : 0);
        if (length + needed > out.size())
            break; // past the seam's bound: the list names what fits
        if (length > 0)
            out[length++] = ' ';
        std::memcpy(out.data() + length, item.mime.data(), item.mime.size());
        length += item.mime.size();
    }
    return static_cast<int>(length);
}

const SnapshotItem* Snapshot::find(std::string_view mime) const
{
    // Parameters are part of the name on the wire but not of the match: the
    // capture stores bare types, and a requester naming a character set still
    // means the type in front of it.
    const std::size_t semicolon = mime.find(';');
    const std::string_view bare = mime.substr(0, semicolon == std::string_view::npos ? mime.size() : semicolon);
    for (const SnapshotItem& item : m_items) {
        if (equalsCaseless(item.mime, bare))
            return &item;
    }
    return nullptr;
}

int Snapshot::item(std::string_view mime, std::span<char> out) const
{
    const SnapshotItem* found = find(mime);
    if (found == nullptr)
        return -1;
    const std::size_t n = std::min(found->bytes.size(), out.size());
    if (n > 0)
        std::memcpy(out.data(), found->bytes.data(), n);
    return static_cast<int>(n);
}

void Snapshot::add(std::string mime, std::vector<char> bytes)
{
    m_items.push_back({ std::move(mime), std::move(bytes) });
}

void ReadStage::stageBytes(std::span<const char> bytes)
{
    std::lock_guard lock(m_lock);
    m_bytes.assign(bytes.begin(), bytes.end());
    m_hasBytes = true; // staged even when empty: a reply is owed
}

void ReadStage::stageTyped(Snapshot&& snapshot)
{
    std::lock_guard lock(m_lock);
    m_hasTyped = !snapshot.isEmpty();
    m_typed = std::move(snapshot);
}

int ReadStage::serveBytes(std::span<char> out)
{
    std::lock_guard lock(m_lock);
    if (!m_hasBytes) {
        return -1; // nothing staged (read refused) — Core sends no reply
    }
    const std::size_t n = std::min(m_bytes.size(), out.size());
    if (n > 0)
        std::memcpy(out.data(), m_bytes.data(), n);
    m_hasBytes = false; // consume this one-shot staging
    m_bytes.clear();
    // 0 (staged but empty capture) has Core frame an empty reply, not silence.
    return static_cast<int>(n);
}

int ReadStage::serveTypes(std::span<char> out)
{
    std::lock_guard lock(m_lock);
    const int n = m_hasTyped ? m_typed.typeList(out) : -1;
    return n;
}

int ReadStage::serveItem(std::string_view mime, std::span<char> out)
{
    std::lock_guard lock(m_lock);
    const int n = m_hasTyped ? m_typed.item(mime, out) : -1;
    return n;
}

} // namespace Ubiquitty::Clipboard
