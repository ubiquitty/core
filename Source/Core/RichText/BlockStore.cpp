// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/BlockStore.h"

#include <cassert>
#include <utility>

namespace Ubiquitty::RichText {

bool validVisibleRects(std::span<const VisibleRect> rectangles, std::uint16_t width, std::uint16_t height)
{
    if (rectangles.size() > maxVisibleRects)
        return false;
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
        const auto& rectangle = rectangles[index];
        if (!rectangle.width || !rectangle.height || rectangle.x >= width || rectangle.y >= height
            || rectangle.width > width - rectangle.x || rectangle.height > height - rectangle.y)
            return false;
        for (const auto& previous : rectangles.first(index)) {
            if (rectangle.x < previous.x + previous.width && previous.x < rectangle.x + rectangle.width
                && rectangle.y < previous.y + previous.height && previous.y < rectangle.y + rectangle.height)
                return false;
        }
    }
    return true;
}

int BlockStore::codePointCount() const
{
    std::size_t count = 0;
    for (const auto& screen : m_screens) {
        for (const Block& block : screen)
            count += block.document.text.size();
    }
    return static_cast<int>(count);
}

std::expected<BlockStore::Admission, StorageError> BlockStore::planAdmission(ScreenMode screen, const Block& block, std::span<const bool> leaving) const
{
    const auto target = index(screen);
    assert(leaving.empty() || leaving.size() == m_screens[target].size());
    const auto isDisplaced = [&](std::size_t heldScreen, std::size_t position) {
        return heldScreen == target && !leaving.empty() && leaving[position];
    };
    Admission result;
    const auto fits = [&] {
        std::size_t bytes = 0;
        const auto charge = [&](std::size_t amount) {
            if (amount > m_storageLimit - bytes)
                return false;
            bytes += amount;
            return true;
        };
        std::size_t remaining = 0;
        for (std::size_t heldScreen = 0; heldScreen < m_screens.size(); ++heldScreen) {
            const auto& blocks = m_screens[heldScreen];
            if (!charge(blocks.capacity() * sizeof(Block)))
                return false;
            for (std::size_t position = 0; position < blocks.size(); ++position) {
                const auto& held = blocks[position];
                if (isDisplaced(heldScreen, position) || (!heldScreen && held.row < result.historyBoundary))
                    continue;
                if (heldScreen == target)
                    ++remaining;
                if (!charge(held.document.storageBytes()) || !charge(held.visibleRects.allocationBytes()))
                    return false;
            }
        }
        if (remaining >= static_cast<std::size_t>(maxBlocksPerScreen))
            return false;
        result.screenCapacity = std::max(m_screens[target].capacity(), remaining + 1);
        return charge((result.screenCapacity - m_screens[target].capacity()) * sizeof(Block))
            && charge(block.document.storageBytes()) && charge(block.visibleRects.allocationBytes());
    };
    while (!fits()) {
        const Block* oldest = nullptr;
        for (std::size_t position = 0; position < m_screens[0].size(); ++position) {
            const auto& held = m_screens[0][position];
            if (isDisplaced(0, position) || held.row < result.historyBoundary || held.row + held.height > 0)
                continue;
            if (!oldest || held.row < oldest->row)
                oldest = &held;
        }
        if (!oldest)
            return std::unexpected(StorageError::OverCapacity);
        result.historyBoundary = oldest->row + oldest->height;
        // Retire whole documents and whole rows together. Another block may
        // span this boundary in a different column; a live row is never cut.
        bool didExtend;
        do {
            didExtend = false;
            for (std::size_t position = 0; position < m_screens[0].size(); ++position) {
                const auto& held = m_screens[0][position];
                if (isDisplaced(0, position) || held.row >= result.historyBoundary || held.row + held.height <= result.historyBoundary)
                    continue;
                if (held.row + held.height > 0)
                    return std::unexpected(StorageError::OverCapacity);
                result.historyBoundary = held.row + held.height;
                didExtend = true;
            }
        } while (didExtend);
    }
    return result;
}

const Block& BlockStore::add(ScreenMode screen, Block block)
{
    auto& blocks = m_screens[index(screen)];
    assert(blocks.size() < blocks.capacity());
    block.serial = m_nextSerial++;
    blocks.push_back(std::move(block));
    return blocks.back();
}

void BlockStore::restore(ScreenMode screen, Block block)
{
    if (block.serial >= m_nextSerial)
        m_nextSerial = block.serial + 1;
    m_screens[index(screen)].push_back(std::move(block));
}

std::expected<void, StorageError> BlockStore::tryReserve(ScreenMode screen, std::size_t count)
{
    if (count > static_cast<std::size_t>(maxBlocksPerScreen))
        return std::unexpected(StorageError::OverCapacity);
    if (!m_screens[index(screen)].tryReserve(count))
        return std::unexpected(StorageError::AllocationRefused);
    return {};
}

std::expected<void, StorageError> BlockStore::tryRestore(ScreenMode screen, Block block)
{
    auto& blocks = m_screens[index(screen)];
    const auto bytes = storageBytes();
    const auto slotBytes = blocks.size() == blocks.capacity() ? sizeof(Block) : 0;
    if (blocks.size() >= static_cast<std::size_t>(maxBlocksPerScreen) || bytes > m_storageLimit
        || slotBytes > m_storageLimit - bytes || block.document.storageBytes() > m_storageLimit - bytes - slotBytes
        || block.visibleRects.allocationBytes() > m_storageLimit - bytes - slotBytes - block.document.storageBytes())
        return std::unexpected(StorageError::OverCapacity);
    if (!blocks.tryReserve(blocks.size() + 1))
        return std::unexpected(StorageError::AllocationRefused);
    const auto serial = block.serial;
    if (!blocks.tryPushBack(std::move(block)))
        return std::unexpected(StorageError::AllocationRefused);
    if (serial >= m_nextSerial)
        m_nextSerial = serial + 1;
    return {};
}

std::size_t BlockStore::storageBytes() const noexcept
{
    std::size_t bytes = 0;
    for (const auto& screen : m_screens) {
        bytes += screen.capacity() * sizeof(Block);
        for (const auto& block : screen)
            bytes += block.visibleRects.capacity() * sizeof(VisibleRect) + block.document.storageBytes();
    }
    return bytes;
}

std::expected<void, StorageError> BlockStore::setStorageLimit(std::size_t bytes)
{
    if (bytes > maxRetainedBytes || bytes < storageBytes())
        return std::unexpected(StorageError::OverCapacity);
    m_storageLimit = bytes;
    return {};
}

void BlockStore::remove(ScreenMode screen, std::size_t position)
{
    auto& blocks = m_screens[index(screen)];
    assert(position < blocks.size());
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(position));
}

void BlockStore::clear()
{
    m_screens[0].clear();
    m_screens[1].clear();
}

} // namespace Ubiquitty::RichText
