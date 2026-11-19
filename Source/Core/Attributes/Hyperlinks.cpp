// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/LineStore.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace Ubiquitty::Attributes::Hyperlinks {

namespace {

/// The longest `id=` token a table stores, its terminator excluded.
constexpr std::size_t maxStoredKeyLength = static_cast<std::size_t>(Ubiquitty::VT::maxLinkKeyLength) - 1;
/// The longest URI a table stores, its terminator excluded.
constexpr std::size_t maxStoredURILength = static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength) - 1;

/// The bucket a hash starts its probe at.
///
/// The hash is FNV-1a folded to 32 bits, whose low bits alone are a poor
/// spread for a power-of-two table; a multiply carries the high bits down.
///
/// - Parameters:
///   - hash: The dedup hash.
///   - mask: The bucket count less one.
/// - Returns: The first bucket to probe.
[[nodiscard]] std::size_t bucketOf(std::uint32_t hash, std::size_t mask) noexcept
{
    return static_cast<std::size_t>((static_cast<std::uint64_t>(hash) * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

/// Whether an id is marked in a census.
///
/// - Parameters:
///   - referenced: One bit per id.
///   - id: The id.
/// - Returns: True when a cell or the open link names it.
[[nodiscard]] bool isMarked(std::span<const std::uint64_t, 1024> referenced, std::uint16_t id) noexcept
{
    return (referenced[id >> 6] >> (id & 63)) & 1u;
}

} // namespace

// MARK: - LinkTable

LinkEntryView LinkTable::entry(std::size_t index) const noexcept
{
    const Slot& slot = m_slots[index];
    return { slot.hash, slot.id, keyOf(slot), uriOf(slot) };
}

std::string_view LinkTable::keyOf(const Slot& slot) const noexcept
{
    return { m_text.get() + slot.textOffset, slot.keyLength };
}

std::string_view LinkTable::uriOf(const Slot& slot) const noexcept
{
    return { m_text.get() + slot.textOffset + slot.keyLength + 1, slot.uriLength };
}

bool LinkTable::holds(std::size_t index, std::string_view target, TargetField field) const noexcept
{
    const Slot& slot = m_slots[index];
    // A clipped copy is shorter than the target it was clipped from, so the
    // length test alone keeps an over-long target from matching its own
    // truncation, and a match compares the whole target.
    if (field == TargetField::Key) {
        return slot.keyLength != 0 && slot.keyLength == target.size()
            && std::memcmp(m_text.get() + slot.textOffset, target.data(), target.size()) == 0;
    }
    return slot.keyLength == 0 && slot.uriLength == target.size()
        && std::memcmp(m_text.get() + slot.textOffset + 1, target.data(), target.size()) == 0;
}

std::size_t LinkTable::find(std::string_view target, TargetField field, std::uint32_t hash) const noexcept
{
    if (!m_bucketCount)
        return notFound;
    const std::size_t mask = m_bucketCount - 1;
    // The index is never more than three quarters used, so an empty bucket
    // ends every probe.
    for (std::size_t at = bucketOf(hash, mask); m_buckets[at] != emptyBucket; at = (at + 1) & mask) {
        const std::uint32_t bucket = m_buckets[at];
        if (bucket == vacatedBucket)
            continue;
        const std::size_t index = bucket - 1;
        if (m_slots[index].hash == hash && holds(index, target, field))
            return index;
    }
    return notFound;
}

std::size_t LinkTable::bucketsFor(std::size_t entryCapacity) noexcept
{
    std::size_t buckets = 16;
    while (buckets < entryCapacity * 2)
        buckets <<= 1;
    return buckets;
}

std::size_t LinkTable::idSlotsFor(std::uint16_t largestID) noexcept
{
    std::size_t slots = 64;
    while (slots <= largestID)
        slots <<= 1;
    return slots;
}

std::size_t LinkTable::storageBytesFor(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept
{
    return entryCapacity * sizeof(Slot) + textCapacity + idSlotsFor(largestID) * sizeof(std::uint16_t)
        + bucketsFor(entryCapacity) * sizeof(std::uint32_t);
}

bool LinkTable::grow(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept
{
    if (entryCapacity > maxEntries || textCapacity > std::numeric_limits<std::uint32_t>::max())
        return false;
    // Every array this grow needs is admitted before any live one is touched,
    // so a refusal part way leaves the table exactly as it was.
    std::unique_ptr<Slot[]> slots;
    if (entryCapacity > m_capacity) {
        slots.reset(new (std::nothrow) Slot[entryCapacity]);
        if (!slots)
            return false;
    }
    std::unique_ptr<char[]> text;
    if (textCapacity > m_textCapacity) {
        text.reset(new (std::nothrow) char[textCapacity]);
        if (!text)
            return false;
    }
    const std::size_t idSlots = idSlotsFor(largestID);
    std::unique_ptr<std::uint16_t[]> slotOfID;
    if (idSlots > m_idSlots) {
        slotOfID.reset(new (std::nothrow) std::uint16_t[idSlots]);
        if (!slotOfID)
            return false;
    }
    const std::size_t bucketCount = bucketsFor(std::max(entryCapacity, m_capacity));
    std::unique_ptr<std::uint32_t[]> buckets;
    if (bucketCount > m_bucketCount) {
        buckets.reset(new (std::nothrow) std::uint32_t[bucketCount]);
        if (!buckets)
            return false;
    }

    if (slots) {
        if (m_count)
            std::memcpy(slots.get(), m_slots.get(), m_count * sizeof(Slot));
        m_slots = std::move(slots);
        m_capacity = entryCapacity;
    }
    if (text) {
        if (m_textUsed)
            std::memcpy(text.get(), m_text.get(), m_textUsed);
        m_text = std::move(text);
        m_textCapacity = textCapacity;
    }
    if (slotOfID) {
        if (m_idSlots)
            std::memcpy(slotOfID.get(), m_slotOfID.get(), m_idSlots * sizeof(std::uint16_t));
        std::fill(slotOfID.get() + m_idSlots, slotOfID.get() + idSlots, noSlot);
        m_slotOfID = std::move(slotOfID);
        m_idSlots = idSlots;
    }
    if (buckets) {
        m_buckets = std::move(buckets);
        m_bucketCount = bucketCount;
        reindex();
    }
    return true;
}

bool LinkTable::reserve(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept
{
    return grow(std::max(entryCapacity, m_capacity), std::max(textCapacity, m_textCapacity), largestID);
}

bool LinkTable::admit(std::size_t textNeeded, std::uint16_t id, std::size_t entryCapacity) noexcept
{
    if (entryCapacity > maxEntries)
        return false;
    // What assignments left behind is reclaimed before the buffer doubles
    // over it, once it is half of what the buffer holds.
    if (m_textUsed + textNeeded > m_textCapacity && (m_textUsed - m_textLive) * 2 >= m_textUsed && m_textUsed)
        packText();
    std::size_t textCapacity = m_textCapacity;
    if (m_textUsed + textNeeded > textCapacity)
        textCapacity = std::max({ textCapacity * 2, m_textUsed + textNeeded, std::size_t { 1024 } });
    std::size_t capacity = m_capacity;
    if (entryCapacity > capacity)
        capacity = std::min(maxEntries, std::max({ capacity * 2, entryCapacity, std::size_t { 16 } }));
    if (!grow(capacity, textCapacity, id))
        return false;
    // Vacated buckets lengthen every probe that crosses them; past three
    // quarters of the index in use, the index is rebuilt without them.
    if ((entryCapacity + m_vacatedBuckets) * 4 > m_bucketCount * 3)
        reindex();
    return true;
}

std::uint32_t LinkTable::storeText(std::string_view key, std::string_view uri) noexcept
{
    const std::size_t offset = m_textUsed;
    char* const out = m_text.get() + offset;
    if (!key.empty())
        std::memcpy(out, key.data(), key.size());
    out[key.size()] = 0;
    if (!uri.empty())
        std::memcpy(out + key.size() + 1, uri.data(), uri.size());
    out[key.size() + 1 + uri.size()] = 0;
    const std::size_t bytes = textBytesFor(key, uri);
    m_textUsed += bytes;
    m_textLive += bytes;
    return static_cast<std::uint32_t>(offset);
}

void LinkTable::indexInsert(std::uint32_t hash, std::size_t index) noexcept
{
    const std::size_t mask = m_bucketCount - 1;
    std::size_t at = bucketOf(hash, mask);
    while (m_buckets[at] != emptyBucket && m_buckets[at] != vacatedBucket)
        at = (at + 1) & mask;
    if (m_buckets[at] == vacatedBucket)
        --m_vacatedBuckets;
    m_buckets[at] = static_cast<std::uint32_t>(index + 1);
}

void LinkTable::indexErase(std::uint32_t hash, std::size_t index) noexcept
{
    const std::size_t mask = m_bucketCount - 1;
    for (std::size_t at = bucketOf(hash, mask); m_buckets[at] != emptyBucket; at = (at + 1) & mask) {
        if (m_buckets[at] == index + 1) {
            m_buckets[at] = vacatedBucket;
            ++m_vacatedBuckets;
            return;
        }
    }
}

void LinkTable::reindex() noexcept
{
    if (m_idSlots)
        std::fill(m_slotOfID.get(), m_slotOfID.get() + m_idSlots, noSlot);
    for (std::size_t index = 0; index < m_count; ++index)
        m_slotOfID[m_slots[index].id] = static_cast<std::uint16_t>(index);
    if (!m_bucketCount)
        return;
    std::fill(m_buckets.get(), m_buckets.get() + m_bucketCount, emptyBucket);
    m_vacatedBuckets = 0;
    for (std::size_t index = 0; index < m_count; ++index) {
        if (m_slots[index].findability == Findability::Findable)
            indexInsert(m_slots[index].hash, index);
    }
}

void LinkTable::packText() noexcept
{
    std::size_t live = 0;
    std::size_t end = 0;
    bool isInTextOrder = true;
    for (std::size_t index = 0; index < m_count; ++index) {
        const Slot& slot = m_slots[index];
        const std::size_t bytes = static_cast<std::size_t>(slot.keyLength) + slot.uriLength + 2;
        if (slot.textOffset < end)
            isInTextOrder = false;
        end = slot.textOffset + bytes;
        live += bytes;
    }
    m_textLive = live;
    if (isInTextOrder) {
        // Entries whose text runs in their own order pack in place: each moves
        // down onto bytes no earlier entry still needs.
        std::size_t write = 0;
        for (std::size_t index = 0; index < m_count; ++index) {
            Slot& slot = m_slots[index];
            const std::size_t bytes = static_cast<std::size_t>(slot.keyLength) + slot.uriLength + 2;
            if (slot.textOffset != write)
                std::memmove(m_text.get() + write, m_text.get() + slot.textOffset, bytes);
            slot.textOffset = static_cast<std::uint32_t>(write);
            write += bytes;
        }
        m_textUsed = write;
        return;
    }
    // An assignment wrote an older entry's text after a newer one's, which
    // only a replica's install does. Packing that in place would overwrite
    // text still to be moved, so it packs into a fresh buffer, and a refused
    // one leaves the text as it was: every entry still reads its own bytes.
    std::unique_ptr<char[]> packed(new (std::nothrow) char[std::max<std::size_t>(live, 1)]);
    if (!packed)
        return;
    std::size_t write = 0;
    for (std::size_t index = 0; index < m_count; ++index) {
        Slot& slot = m_slots[index];
        const std::size_t bytes = static_cast<std::size_t>(slot.keyLength) + slot.uriLength + 2;
        std::memcpy(packed.get() + write, m_text.get() + slot.textOffset, bytes);
        slot.textOffset = static_cast<std::uint32_t>(write);
        write += bytes;
    }
    m_text = std::move(packed);
    m_textCapacity = std::max<std::size_t>(live, 1);
    m_textUsed = write;
}

bool LinkTable::append(std::uint16_t id, std::uint32_t hash, std::string_view key, std::string_view uri, Findability findability) noexcept
{
    if (id == 0 || m_count >= maxEntries || indexOf(id) != notFound
        || key.size() > std::numeric_limits<std::uint16_t>::max() || uri.size() > std::numeric_limits<std::uint16_t>::max())
        return false;
    if (!admit(textBytesFor(key, uri), id, m_count + 1))
        return false;
    const std::size_t index = m_count;
    const std::uint32_t offset = storeText(key, uri);
    m_slots[index] = Slot { hash, offset, id, static_cast<std::uint16_t>(key.size()), static_cast<std::uint16_t>(uri.size()), findability };
    m_slotOfID[id] = static_cast<std::uint16_t>(index);
    if (findability == Findability::Findable)
        indexInsert(hash, index);
    ++m_count;
    return true;
}

bool LinkTable::assign(std::size_t index, std::uint32_t hash, std::string_view key, std::string_view uri) noexcept
{
    if (index >= m_count || key.size() > std::numeric_limits<std::uint16_t>::max() || uri.size() > std::numeric_limits<std::uint16_t>::max())
        return false;
    if (!admit(textBytesFor(key, uri), m_slots[index].id, m_count))
        return false;
    Slot& slot = m_slots[index];
    m_textLive -= static_cast<std::size_t>(slot.keyLength) + slot.uriLength + 2;
    if (slot.findability == Findability::Findable)
        indexErase(slot.hash, index);
    slot.textOffset = storeText(key, uri);
    slot.hash = hash;
    slot.keyLength = static_cast<std::uint16_t>(key.size());
    slot.uriLength = static_cast<std::uint16_t>(uri.size());
    slot.findability = Findability::Findable;
    indexInsert(hash, index);
    if (recentIndex == index)
        recentIndex = notFound;
    return true;
}

void LinkTable::retain(std::span<const std::uint64_t, 1024> referenced) noexcept
{
    std::size_t kept = 0;
    for (std::size_t index = 0; index < m_count; ++index) {
        const Slot slot = m_slots[index];
        if (isMarked(referenced, slot.id))
            m_slots[kept++] = slot;
    }
    if (kept == m_count)
        return;
    m_count = kept;
    packText();
    reindex();
    recentIndex = notFound;
}

void LinkTable::dropOldestUntil(std::size_t textBytes) noexcept
{
    std::size_t live = m_textLive;
    std::size_t first = 0;
    while (first < m_count && live > textBytes) {
        live -= static_cast<std::size_t>(m_slots[first].keyLength) + m_slots[first].uriLength + 2;
        ++first;
    }
    dropBefore(first);
}

void LinkTable::dropBefore(std::size_t first) noexcept
{
    if (!first && m_textUsed == m_textLive)
        return;
    if (first) {
        std::memmove(m_slots.get(), m_slots.get() + first, (m_count - first) * sizeof(Slot));
        m_count -= first;
    }
    packText();
    reindex();
    recentIndex = notFound;
}

// MARK: - The terminal's table

LinkTable* table(Ubiquitty::Terminal* t) { return t ? t->linkTable.get() : nullptr; }

const LinkTable* table(const Ubiquitty::Terminal* t) { return t ? t->linkTable.get() : nullptr; }

LinkTable& ensureTable(Ubiquitty::Terminal& t)
{
    if (!t.linkTable) [[unlikely]]
        t.linkTable = std::make_unique<LinkTable>();
    return *t.linkTable;
}

std::size_t count(const Ubiquitty::Terminal& t) { return t.linkTable ? t.linkTable->count() : 0; }

void reset(Ubiquitty::Terminal& t)
{
    t.linkTable.reset();
    t.currentLinkID = 0;
    t.linkNextID = 1;
    t.linkFreeIDs.clear();
    t.linkReclaimBackoffMints = 0;
}

/// Marks every id something can still hand back to a lookup.
///
/// That is every cell on both screens whole (the inactive one returns), every
/// retained scrollback line, and the link a further write would stamp next.
/// The table's own entries are not marked: an entry no cell names resolves for
/// nobody, and keeping it would keep its id from ever being minted again.
///
/// - Parameters:
///   - t: The terminal.
///   - referenced: One bit per id, cleared by the caller.
static void markReferencedIDs(const Ubiquitty::Terminal& t, std::span<std::uint64_t, 1024> referenced)
{
    const auto mark = [&referenced](std::uint16_t id) {
        referenced[id >> 6] |= (std::uint64_t { 1 } << (id & 63));
    };
    const auto markCells = [&mark](const Ubiquitty::Cell* cells, int count) {
        for (int i = 0; i < count; ++i) {
            if (const std::uint16_t id = cells[i].linkID(); id != 0)
                mark(id);
        }
    };
    mark(t.currentLinkID);
    if (t.grid) {
        for (int row = 0; row < t.grid->rows(); ++row) {
            const Ubiquitty::RowView primary = t.grid->rowCells(Ubiquitty::ScreenMode::Primary, Ubiquitty::Row(row));
            markCells(primary.data, primary.count);
            const Ubiquitty::RowView alternate = t.grid->rowCells(Ubiquitty::ScreenMode::Alternate, Ubiquitty::Row(row));
            markCells(alternate.data, alternate.count);
        }
    }
    if (t.lineStore) {
        for (int line = 0; line < t.lineStore->count(); ++line) {
            if (const Ubiquitty::StoredLine* stored = t.lineStore->at(line))
                markCells(stored->cells, stored->length);
        }
    }
}

/// Reclaim every id nothing can hand back to a lookup.
///
/// An id is what a cell carries in place of a URI, and a cell drawn long ago
/// still names an id `uri()` will look up when the user clicks it. So an id must
/// never mean two things. Recycling the counter at 0xFFFF made it mean two
/// things after 65535 mints — and minting is what OSC 8 does, with no cell
/// printed and nothing on screen to see. A program that opens 65535 links with
/// distinct targets and then opens one more would get that last target resolved
/// for a cell the user watched being drawn with a different one.
///
/// Exhaustion therefore never wraps the counter. What it does instead is take a
/// census: an id referenced by no cell on either screen, no scrollback line and
/// no open link cannot resolve for anyone, so its entry is dropped and the id
/// returns to the pool. Reusing it cannot make any EXISTING cell mean something
/// new. That is the whole of the invariant; the census is what proves it per id.
///
/// A counter that merely parks at 0xFFFF keeps the invariant too, at the price a
/// benchmark made visible: a window that has ever printed 65535 distinct targets
/// draws every later link as plain text for the rest of its life. A reclaim per
/// 65535 mints amortises to noise against the parse work those mints rode in on;
/// the barren-census backoff in mintID keeps the degenerate case (every id
/// genuinely live) from paying a walk per link.
///
/// - Parameter t: The terminal whose id space is spent.
static void reclaimSpentLinkIDs(Ubiquitty::Terminal& t)
{
    // One bit per id. Heap rather than stack: 8KB, reached once per 65535
    // mints, and the parse stack is not the place to test depth limits.
    std::vector<std::uint64_t> referenced(1024, 0);
    const std::span<std::uint64_t, 1024> bits(referenced.data(), 1024);
    markReferencedIDs(t, bits);
    if (LinkTable* links = table(&t))
        links->retain(bits);
    // Ids only, never 0; LIFO order is as good as any — no consumer reads
    // meaning into which free id a link receives. Every entry the table kept
    // is marked, so none of their ids is freed.
    for (std::uint32_t id = 1; id <= 0xFFFFu; ++id) {
        if (!(referenced[id >> 6] & (std::uint64_t { 1 } << (id & 63))))
            t.linkFreeIDs.push_back(static_cast<std::uint16_t>(id));
    }
    Diagnostics::diagLog(t, "link-ids-reclaimed", static_cast<int>(t.linkFreeIDs.size()));
}

/// Mints the next 1-based id, or 0 once the space is spent and the census
/// frees nothing.
///
/// - Parameter t: The terminal.
/// - Returns: The id.
static std::uint16_t mintID(Ubiquitty::Terminal& t)
{
    if (t.linkNextID != 0) {
        const std::uint16_t id = t.linkNextID;
        const std::uint16_t next = static_cast<std::uint16_t>(t.linkNextID + 1);
        if (next == 0) {
            // 0xFFFF was just handed out; 0 is the "no link" value and cannot
            // be minted, so the sequential space ends here and later mints go
            // through the census.
            Diagnostics::diagLog(t, "link-ids-exhausted", 0xFFFF);
        }
        t.linkNextID = next;
        return id;
    }
    if (t.linkFreeIDs.empty()) {
        if (t.linkReclaimBackoffMints > 0) {
            --t.linkReclaimBackoffMints;
            return 0;
        }
        reclaimSpentLinkIDs(t);
        if (t.linkFreeIDs.empty()) {
            // Every id is genuinely live. Walking again for the next link
            // would buy nothing until content leaves a screen or the
            // scrollback, so a batch of mints shares one failed walk.
            t.linkReclaimBackoffMints = 4096;
            return 0;
        }
    }
    const std::uint16_t id = t.linkFreeIDs.back();
    t.linkFreeIDs.pop_back();
    return id;
}

/// Brings a minting terminal's table back within `textBudgetBytes` before an
/// entry would take it past.
///
/// Entries no cell names go first. If the text the cells do name is still
/// over half the budget, the oldest go too, so the next walk is at least half
/// a budget of new links away. Their cells resolve to nothing, as every cell
/// did once a fixed ring recycled its slot; their ids stay out of the pool for
/// as long as the cells hold them, so none comes to mean another target.
///
/// - Parameters:
///   - t: The terminal.
///   - links: Its table.
///   - textNeeded: The text bytes the entry about to be appended stores.
static void keepWithinBudget(Ubiquitty::Terminal& t, LinkTable& links, std::size_t textNeeded)
{
    if (links.textBytes() + textNeeded <= textBudgetBytes) [[likely]]
        return;
    std::vector<std::uint64_t> referenced(1024, 0);
    const std::span<std::uint64_t, 1024> bits(referenced.data(), 1024);
    markReferencedIDs(t, bits);
    links.retain(bits);
    if (links.textBytes() + textNeeded > textBudgetBytes / 2)
        links.dropOldestUntil(textBudgetBytes / 2);
    Diagnostics::diagLog(t, "link-table-trimmed", static_cast<std::uint32_t>(links.count()));
}

const char* uri(const Ubiquitty::Terminal* t, uint16_t id)
{
    const LinkTable* links = table(t);
    if (!links)
        return nullptr;
    const std::size_t index = links->indexOf(id);
    return index == LinkTable::notFound ? nullptr : links->entry(index).uri.data();
}

bool isExplicitlyKeyed(const Ubiquitty::Terminal* t, uint16_t id)
{
    const LinkTable* links = table(t);
    if (!links)
        return false;
    const std::size_t index = links->indexOf(id);
    // `intern` stores the key for a keyed entry and none for one keyed by its
    // URI, so the key's presence is the whole answer.
    return index != LinkTable::notFound && !links->entry(index).key.empty();
}

std::uint32_t dedupHash(std::string_view target)
{
    // u64-chunk FNV-1a: eight bytes a multiply.
    const std::size_t length = target.size();
    std::uint64_t h = 1469598103934665603ull;
    std::size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, target.data() + i, 8);
        h ^= word;
        h *= 1099511628211ull;
    }
    if (i < length) {
        std::uint64_t word = 0;
        std::memcpy(&word, target.data() + i, length - i);
        h ^= word;
        h *= 1099511628211ull;
    }
    const std::uint32_t folded = static_cast<std::uint32_t>(h ^ (h >> 32));
    // 0 is kept out of the range, as an empty bucket's marker.
    return folded == 0 ? 1u : folded;
}

uint16_t intern(Ubiquitty::Terminal* t, std::string_view key, std::string_view uri)
{
    if (!t)
        return 0;

    // Empty URI closes the current link (OSC 8 ; ; ST).
    if (uri.empty()) {
        t->currentLinkID = 0;
        return 0;
    }

    // Dedup target is the id= key when present, else the URI.
    const bool hasKey = !key.empty();
    const TargetField field = hasKey ? TargetField::Key : TargetField::URI;
    const std::string_view target = hasKey ? key : uri;

    LinkTable* links = table(t);
    // The common re-open of the same target around every styled span is one
    // string compare against the entry the last intern resolved.
    if (links && links->recentIndex != LinkTable::notFound && links->holds(links->recentIndex, target, field)) {
        t->currentLinkID = links->entry(links->recentIndex).id;
        return t->currentLinkID;
    }
    const std::uint32_t hash = dedupHash(target);
    if (links) {
        // A hit reuses the existing id, so a multi-line link (same id= across
        // runs) shares one entry.
        if (const std::size_t found = links->find(target, field, hash); found != LinkTable::notFound) {
            links->recentIndex = found;
            t->currentLinkID = links->entry(found).id;
            return t->currentLinkID;
        }
    }

    // A spent id space means this link gets none, and is drawn as text.
    const std::uint16_t id = mintID(*t);
    if (id == 0) {
        t->currentLinkID = 0;
        return 0;
    }

    // After the mint, so a spent id space leaves no table behind.
    LinkTable& store = ensureTable(*t);
    const std::string_view storedKey = key.substr(0, std::min(key.size(), maxStoredKeyLength));
    const std::string_view storedURI = uri.substr(0, std::min(uri.size(), maxStoredURILength));
    keepWithinBudget(*t, store, LinkTable::textBytesFor(storedKey, storedURI));
    // A target clipped to fit never compares equal to itself, so it re-interns
    // rather than aliasing the entry it was clipped into, and is left out of
    // the dedup index a repeat of it would only probe further through.
    const std::size_t storedTargetLength = hasKey ? storedKey.size() : storedURI.size();
    const Findability findability = storedTargetLength == target.size() ? Findability::Findable : Findability::Unfindable;
    if (!store.append(id, hash, storedKey, storedURI, findability)) {
        // Storage refused: the minted id names nothing, and the census returns
        // it once nothing names it either.
        t->currentLinkID = 0;
        return 0;
    }
    store.recentIndex = findability == Findability::Findable ? store.count() - 1 : LinkTable::notFound;
    t->currentLinkID = id;
    return id;
}

bool install(Ubiquitty::Terminal& t, std::uint16_t id, std::string_view key, std::string_view uri)
{
    if (id == 0)
        return false;
    LinkTable& links = ensureTable(t);
    const std::string_view storedKey = key.substr(0, std::min(key.size(), maxStoredKeyLength));
    const std::string_view storedURI = uri.substr(0, std::min(uri.size(), maxStoredURILength));
    const std::uint32_t hash = dedupHash(storedKey.empty() ? storedURI : storedKey);
    if (const std::size_t index = links.indexOf(id); index != LinkTable::notFound)
        return links.assign(index, hash, storedKey, storedURI);
    if (links.textBytes() + LinkTable::textBytesFor(storedKey, storedURI) > textBudgetBytes)
        links.dropOldestUntil(textBudgetBytes / 2);
    return links.append(id, hash, storedKey, storedURI, Findability::Findable);
}

bool appendCarried(LinkTable& table, const LinkEntryView& entry)
{
    // A carried hash agrees with its stored target exactly when the target was
    // stored whole; a clipped one was hashed before clipping, and is left out
    // of the dedup index as `intern` left it.
    const std::string_view target = entry.key.empty() ? entry.uri : entry.key;
    const Findability findability = entry.hash == dedupHash(target) ? Findability::Findable : Findability::Unfindable;
    return table.append(entry.id, entry.hash, entry.key, entry.uri, findability);
}

} // namespace Ubiquitty::Attributes::Hyperlinks
