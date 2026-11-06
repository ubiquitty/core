// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/OutputQueue.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace Ubiquitty::PAL {

OutputQueue::Unwritten::Unwritten(Unwritten&& other) noexcept
{
    *this = std::move(other);
}

OutputQueue::Unwritten& OutputQueue::Unwritten::operator=(Unwritten&& other) noexcept
{
    if (this != &other) {
        m_data = std::move(other.m_data);
        m_size = std::exchange(other.m_size, 0);
        m_runs = other.m_runs;
        m_runCount = std::exchange(other.m_runCount, 0);
    }
    return *this;
}

std::optional<OutputQueue::Unwritten> OutputQueue::Unwritten::copyOf(std::span<const char> bytes, std::span<const Run> runs)
{
    if (bytes.size() > maxPending || runs.size() > maxPendingRuns)
        return std::nullopt;
    std::size_t covered = 0;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const Run& run = runs[index];
        if (!run.bytes || run.bytes > bytes.size() - covered || (index && runs[index - 1].owner == run.owner))
            return std::nullopt;
        covered += run.bytes;
    }
    if (covered != bytes.size())
        return std::nullopt;
    Unwritten result;
    if (!bytes.empty()) {
        result.m_data.reset(new (std::nothrow) char[bytes.size()]);
        if (!result.m_data)
            return std::nullopt;
        std::memcpy(result.m_data.get(), bytes.data(), bytes.size());
    }
    result.m_size = bytes.size();
    std::ranges::copy(runs, result.m_runs.begin());
    result.m_runCount = runs.size();
    return result;
}

std::optional<OutputQueue::Unwritten> OutputQueue::copyUnwritten() const
{
    return Unwritten::copyOf({ m_data ? m_data + m_head : nullptr, pendingBytes() }, std::span(m_runs).first(m_runCount));
}

bool OutputQueue::restoreUnwritten(Unwritten& unwritten)
{
    if (hasPending())
        return false;
    delete[] m_data;
    m_data = unwritten.m_data.release();
    m_size = std::exchange(unwritten.m_size, 0);
    m_capacity = m_size;
    m_head = 0;
    m_runs = unwritten.m_runs;
    m_runCount = std::exchange(unwritten.m_runCount, 0);
    return true;
}

OutputQueue::~OutputQueue()
{
    delete[] m_data;
    m_data = nullptr;
}

int OutputQueue::submit(std::span<const char> bytes, OutputWriteCallback callback, void* context)
{
    if (bytes.empty())
        return 0;
    // Fast path: nothing queued — hand bytes straight to the sink, allocate/copy
    // only the part it can't take.
    if (!hasPending()) {
        int accepted = callback(context, bytes);
        if (accepted < 0)
            return accepted; // sink fatal
        if (static_cast<size_t>(accepted) >= bytes.size())
            return 0; // all accepted, nothing to queue
        if (!append(bytes.subspan(static_cast<size_t>(accepted))))
            return errQueueFull;
        return 0;
    }
    // A backlog exists: queue behind it (order!) then push as much as fits.
    if (!append(bytes))
        return errQueueFull;
    return flush(callback, context);
}

int OutputQueue::drain(OutputWriteCallback callback, void* context)
{
    if (!hasPending())
        return 0;
    return flush(callback, context);
}

int OutputQueue::reserveOwned(std::uint64_t owner, std::size_t bytes)
{
    if (!owner)
        return errInvalidOwner;
    return reserve(bytes, owner) ? 0 : errQueueFull;
}

int OutputQueue::submitOwned(std::uint64_t owner, std::span<const char> bytes, OutputWriteCallback callback, void* context)
{
    const int admission = reserveOwned(owner, bytes.size());
    if (admission < 0 || bytes.empty())
        return admission;
    // Admission above covers both the copy and its ownership run. There is
    // no sink call before it, so refusal has no partially delivered prefix.
    if (!append(bytes, owner))
        return errQueueFull;
    return flush(callback, context);
}

std::size_t OutputQueue::pendingBytes(std::uint64_t owner) const
{
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < m_runCount; ++index)
        if (m_runs[index].owner == owner)
            bytes += m_runs[index].bytes;
    return bytes;
}

std::size_t OutputQueue::discardPending(std::uint64_t owner)
{
    if (!owner)
        return 0;
    std::size_t read = m_head;
    std::size_t keptBytes = 0;
    std::size_t keptRuns = 0;
    std::size_t discarded = 0;
    for (std::size_t index = 0; index < m_runCount; ++index) {
        const Run run = m_runs[index];
        if (run.owner == owner) {
            discarded += run.bytes;
        } else {
            std::memmove(m_data + keptBytes, m_data + read, run.bytes);
            keptBytes += run.bytes;
            if (keptRuns && m_runs[keptRuns - 1].owner == run.owner)
                m_runs[keptRuns - 1].bytes += run.bytes;
            else
                m_runs[keptRuns++] = run;
        }
        read += run.bytes;
    }
    m_head = 0;
    m_size = keptBytes;
    m_runCount = keptRuns;
    return discarded;
}

bool OutputQueue::repairPending(std::uint64_t owner, std::span<const char> repair)
{
    if (!owner || repair.size() > 12)
        return false;
    std::array<Run, maxPendingRuns> runs {};
    std::size_t count = 0;
    std::size_t insertion = 0;
    std::size_t kept = 0;
    bool hasInserted = false;
    const auto append = [&](Run run) {
        if (!run.bytes)
            return true;
        if (count && runs[count - 1].owner == run.owner) {
            runs[count - 1].bytes += run.bytes;
            return true;
        }
        if (count == runs.size())
            return false;
        runs[count++] = run;
        return true;
    };
    for (std::size_t index = 0; index < m_runCount; ++index) {
        const Run run = m_runs[index];
        if (run.owner == owner) {
            if (!hasInserted) {
                insertion = kept;
                hasInserted = true;
                if (!append({ .owner = 0, .bytes = repair.size() }))
                    return false;
            }
        } else {
            if (!append(run))
                return false;
            kept += run.bytes;
        }
    }
    if (!hasInserted) {
        insertion = kept;
        if (!append({ .owner = 0, .bytes = repair.size() }))
            return false;
    }
    if (repair.size() > maxPending - kept)
        return false;
    const std::size_t finalBytes = kept + repair.size();
    const std::size_t growth = finalBytes > pendingBytes() ? finalBytes - pendingBytes() : 0;
    if (!reserve(growth, m_runCount ? m_runs[m_runCount - 1].owner : 0))
        return false;
    (void)discardPending(owner);
    if (!repair.empty()) {
        std::memmove(m_data + insertion + repair.size(), m_data + insertion, kept - insertion);
        std::memcpy(m_data + insertion, repair.data(), repair.size());
    }
    m_size = finalBytes;
    m_runs = runs;
    m_runCount = count;
    return true;
}

void OutputQueue::consumeRuns(std::size_t bytes)
{
    std::size_t consumed = 0;
    while (consumed < m_runCount && bytes >= m_runs[consumed].bytes) {
        bytes -= m_runs[consumed].bytes;
        ++consumed;
    }
    if (consumed < m_runCount)
        m_runs[consumed].bytes -= bytes;
    if (consumed)
        std::move(m_runs.begin() + static_cast<std::ptrdiff_t>(consumed), m_runs.begin() + static_cast<std::ptrdiff_t>(m_runCount), m_runs.begin());
    m_runCount -= consumed;
}

int OutputQueue::flush(OutputWriteCallback callback, void* context)
{
    while (m_head < m_size) {
        int n = callback(context, std::span<const char>(m_data + m_head, m_size - m_head));
        if (n < 0)
            return n; // sink fatal — keep the queue intact
        if (n == 0)
            break; // sink full now — flush the rest later
        m_head += static_cast<size_t>(n);
        consumeRuns(static_cast<std::size_t>(n));
    }
    if (m_head >= m_size) { // fully drained — reset to the front
        m_head = 0;
        m_size = 0;
    }
    return 0;
}

bool OutputQueue::reserve(std::size_t add, std::uint64_t owner)
{
    if (add == 0)
        return true;
    const size_t live = m_size - m_head; // undelivered bytes to preserve
    if (add > maxPending - live || (m_runCount == maxPendingRuns && m_runs[m_runCount - 1].owner != owner))
        return false;
    // Reclaim the delivered prefix by shifting live bytes to the front; this
    // keeps the buffer from growing without bound under sustained backpressure.
    if (m_head > 0) {
        if (live > 0)
            std::memmove(m_data, m_data + m_head, live);
        m_size = live;
        m_head = 0;
    }
    // Bounded: refuse rather than grow past the cap (wedged sink + giant paste
    // must surface a controlled error, not crash the process).
    if (add > maxPending - m_size)
        return false;
    if (m_size + add > m_capacity) {
        size_t newCapacity = m_capacity ? m_capacity * 2 : 256;
        while (newCapacity < m_size + add)
            newCapacity *= 2;
        if (newCapacity > maxPending)
            newCapacity = maxPending; // m_size+add fits (checked above); clamp growth
        // No-throw: OutputQueue.cpp is built -fno-exceptions, so a throwing
        // new[] would std::terminate on OOM instead of degrading.
        char* grown = new (std::nothrow) char[newCapacity];
        if (!grown)
            return false;
        if (m_size > 0)
            std::memcpy(grown, m_data, m_size);
        delete[] m_data;
        m_data = grown;
        m_capacity = newCapacity;
    }
    return true;
}

bool OutputQueue::append(std::span<const char> bytes, std::uint64_t owner)
{
    if (bytes.empty())
        return true;
    if (!reserve(bytes.size(), owner))
        return false;
    std::memcpy(m_data + m_size, bytes.data(), bytes.size());
    m_size += bytes.size();
    if (m_runCount && m_runs[m_runCount - 1].owner == owner)
        m_runs[m_runCount - 1].bytes += bytes.size();
    else
        m_runs[m_runCount++] = { .owner = owner, .bytes = bytes.size() };
    return true;
}

}
// namespace Ubiquitty::PAL
