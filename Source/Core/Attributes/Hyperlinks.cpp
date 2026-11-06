// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/LineStore.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace Ubiquitty::Attributes::Hyperlinks {

LinkTable* table(Ubiquitty::Terminal* t) { return t ? t->linkTable.get() : nullptr; }

const LinkTable* table(const Ubiquitty::Terminal* t) { return t ? t->linkTable.get() : nullptr; }

LinkTable& ensureTable(Ubiquitty::Terminal& t)
{
    if (!t.linkTable) [[unlikely]]
        t.linkTable = std::make_unique<LinkTable>();
    return *t.linkTable;
}

void reset(Ubiquitty::Terminal& t)
{
    t.linkTable.reset();
    t.currentLinkID = 0;
    t.linkNextID = 1;
    t.linkCount = 0;
    t.linkHead = 0;
    t.linkFreeIDs.clear();
    t.linkReclaimBackoffMints = 0;
}

// Mint the next 1-based id, or 0 once the space is spent.
//
// An id is what a cell carries in place of a URI, and cells outlive the table:
// the ring holds 64 entries while the scrollback holds a million rows, so a cell
// drawn long ago still names an id `uri()` will look up when the user clicks it.
// That lookup answers with whatever entry currently holds that number.
//
// So an id must never mean two things. Recycling the counter at 0xFFFF made it
// mean two things after 65535 mints — and minting is what OSC 8 does, with no
// cell printed and nothing on screen to see. A program that opens 65535 links
// with distinct targets and then opens one more gets that last target resolved
// for a cell the user watched being drawn with a different one. The text says one
// destination and the click goes to another.
//
// Exhaustion therefore never wraps the counter. What it does instead is take a
// census: an id referenced by no cell on either screen, no scrollback line, no
// ring entry and no open link cannot resolve for anyone — the cells that carried
// it are gone, and its ring entry (the only thing a click consults) has been
// recycled — so reusing it cannot make any EXISTING cell mean something new.
// That is the whole of the invariant; the census is what proves it per id.
//
// A counter that merely parks at 0xFFFF keeps the invariant too, at the price a
// benchmark made visible: a window that has ever printed 65535 distinct targets
// draws every later link as plain text for the rest of its life. A reclaim per
// 65535 mints amortises to noise against the parse work those mints rode in on;
// the barren-census backoff in mintID keeps the degenerate case (every id
// genuinely live) from paying a walk per link.
static void reclaimSpentLinkIDs(Ubiquitty::Terminal& t)
{
    // One bit per id. Heap rather than stack: 8KB, reached once per 65535
    // mints, and the parse stack is not the place to test depth limits.
    std::vector<std::uint64_t> referenced(1024, 0);
    const auto mark = [&referenced](uint16_t id) {
        referenced[id >> 6] |= (std::uint64_t { 1 } << (id & 63));
    };
    const auto markCells = [&mark](const Ubiquitty::Cell* cells, int count) {
        for (int i = 0; i < count; ++i) {
            if (const uint16_t id = cells[i].linkID(); id != 0)
                mark(id);
        }
    };

    // The population is everything that can hand an id back to a lookup:
    // both screens whole (the inactive one returns), every retained
    // scrollback line, the ring (a dedup hit re-issues a ring id, so a ring
    // entry with no cells yet must keep its id), and the link a further
    // write would stamp next.
    mark(t.currentLinkID);
    if (const LinkTable* links = table(&t)) {
        for (int i = 0; i < t.linkCount; i++)
            mark(links->ids[(t.linkHead + i) % Ubiquitty::VT::linkTableCapacity]);
    }
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
            const Ubiquitty::StoredLine* stored = t.lineStore->at(line);
            if (stored)
                markCells(stored->cells, stored->length);
        }
    }

    // Ids only, never 0; LIFO order is as good as any — no consumer reads
    // meaning into which free id a link receives.
    for (std::uint32_t id = 1; id <= 0xFFFFu; ++id) {
        if (!(referenced[id >> 6] & (std::uint64_t { 1 } << (id & 63))))
            t.linkFreeIDs.push_back(static_cast<uint16_t>(id));
    }
    Diagnostics::diagLog(t, "link-ids-reclaimed", static_cast<int>(t.linkFreeIDs.size()));
}

static uint16_t mintID(Ubiquitty::Terminal& t)
{
    if (t.linkNextID != 0) {
        uint16_t id = t.linkNextID;
        uint16_t next = static_cast<uint16_t>(t.linkNextID + 1);
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
    const uint16_t id = t.linkFreeIDs.back();
    t.linkFreeIDs.pop_back();
    return id;
}

const char* uri(const Ubiquitty::Terminal* t, uint16_t id)
{
    if (!t || id == 0)
        return nullptr;
    const LinkTable* links = table(t);
    if (!links)
        return nullptr;
    for (int i = 0; i < t->linkCount; i++) {
        int slot = (t->linkHead + i) % Ubiquitty::VT::linkTableCapacity;
        if (links->ids[slot] == id)
            return links->uris[slot].data();
    }
    return nullptr;
}

bool isExplicitlyKeyed(const Ubiquitty::Terminal* t, uint16_t id)
{
    if (!t || id == 0)
        return false;
    const LinkTable* links = table(t);
    if (!links)
        return false;
    for (int i = 0; i < t->linkCount; i++) {
        int slot = (t->linkHead + i) % Ubiquitty::VT::linkTableCapacity;
        if (links->ids[slot] == id) {
            // `intern` writes the key field for a keyed entry and empties it for
            // one keyed by its URI, so the first byte is the whole answer.
            return links->keys[slot][0] != 0;
        }
    }
    return false;
}

// Compare a measured target against one of the ring's NUL-terminated copies.
//
// The measured side's length is already known, so the stored side is read as a
// block of exactly that many bytes rather than a byte at a time. Stopping at
// the first difference sounds cheaper and is not for the targets this ring
// actually holds: links off one host share their scheme, host and directory, so
// the bytes that tell two apart are the last few and a byte-at-a-time walk pays
// for the whole common prefix. A block compare reads sixteen at a step.
//
// The field's capacity is a template parameter so the bound is the array's own.
// A target that cannot fit cannot equal anything stored here — `storeClipped`
// keeps a copy to capacity - 1 bytes and a NUL — so that case answers false
// before any read, which is what keeps the compare inside the field.
template<std::size_t Capacity>
[[nodiscard]] static bool equalsStoredString(std::string_view target, const std::array<char, Capacity>& stored)
{
    if (target.size() >= Capacity)
        return false;
    // The stored copy ends where the measured target does, or they are two
    // different strings one of which begins with the other.
    return std::memcmp(stored.data(), target.data(), target.size()) == 0 && stored[target.size()] == 0;
}

// Copy a target into one of the ring's fixed fields, NUL-terminated and clipped
// to fit. An over-long target is stored truncated and never compares equal to
// itself, so it re-interns rather than aliasing the entry it was clipped into.
static void storeClipped(std::string_view target, char* destination, std::size_t capacity)
{
    const std::size_t length = std::min(target.size(), capacity - 1);
    if (length > 0)
        std::memcpy(destination, target.data(), length);
    destination[length] = 0;
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
    // 0 marks an empty slot, so no target may claim it.
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

    const bool haveKey = !key.empty();
    const std::string_view target = haveKey ? key : uri;

    // Dedup target is the id= key when present, else the URI. Both arms need the
    // hash, so it is taken once.
    const uint32_t wantHash = dedupHash(target);

    if (LinkTable* links = table(t)) {
        // One-slot MRU: the common re-open of the same target around every styled
        // span is one string compare (no hash, no ring scan). Keyed on the raw
        // target plus which field it dedups by (equal-byte key vs URI differ).
        if (links->mruID != 0 && haveKey == links->isMRUTargetKeyed
            && equalsStoredString(target, links->mruTarget)) {
            t->currentLinkID = links->mruID;
            return links->mruID;
        }

        // A hit reuses the existing id, so a multi-line link (same id= across
        // runs) shares one slot. Every slot is swept rather than the live ones in
        // ring order: an empty slot holds hash 0 and no target hashes to 0, so
        // the sweep needs no bounds and compiles to one compare a slot with no
        // modulo and no early exit.
        static_assert(Ubiquitty::VT::linkTableCapacity <= 64, "the match mask holds one bit a slot");
        std::uint64_t matches = 0;
        for (int slot = 0; slot < Ubiquitty::VT::linkTableCapacity; ++slot)
            matches |= static_cast<std::uint64_t>(links->hashes[slot] == wantHash) << slot;
        for (; matches; matches &= matches - 1) {
            const int slot = __builtin_ctzll(matches);
            bool match;
            if (haveKey)
                match = (links->keys[slot][0] != 0 && equalsStoredString(key, links->keys[slot]));
            else
                match = (links->keys[slot][0] == 0 && equalsStoredString(uri, links->uris[slot]));
            if (match) {
                t->currentLinkID = links->ids[slot];
                // A match means the ring's copy compared equal to the whole target,
                // so the target fit its field — an over-long one never matches the
                // truncated copy and reaches the insert below instead, where the fit
                // does have to be checked. Caching here needs no second test, which
                // keeps a strlen off the hit path.
                storeClipped(target, links->mruTarget.data(), links->mruTarget.size());
                links->isMRUTargetKeyed = haveKey;
                links->mruID = t->currentLinkID;
                return t->currentLinkID;
            }
        }
    }

    // Mint before touching the ring. A spent id space means this link gets none,
    // and evicting the oldest entry to store one that no cell can reference would
    // retire a working link to make room for an unusable one.
    const uint16_t id = mintID(*t);
    if (id == 0) {
        t->currentLinkID = 0;
        return 0;
    }

    // After the mint, so a spent id space leaves no table behind.
    LinkTable& links = ensureTable(*t);

    // Append a fresh entry. When the ring is full, recycle the oldest
    // slot (advance head); its id retires and any lingering cells that
    // referenced it resolve to nullptr.
    int slot;
    if (t->linkCount < Ubiquitty::VT::linkTableCapacity) {
        slot = (t->linkHead + t->linkCount) % Ubiquitty::VT::linkTableCapacity;
        t->linkCount++;
    } else {
        slot = t->linkHead;
        t->linkHead = (t->linkHead + 1) % Ubiquitty::VT::linkTableCapacity;
        // The recycled slot may be the MRU's — drop it rather than track.
        links.mruID = 0;
    }

    links.ids[slot] = id;
    links.hashes[slot] = wantHash;

    storeClipped(uri, links.uris[slot].data(), Ubiquitty::VT::maxLinkURILength);

    if (haveKey)
        storeClipped(key, links.keys[slot].data(), Ubiquitty::VT::maxLinkKeyLength);
    else
        links.keys[slot][0] = 0;

    // Same fit rule as the scan-hit refresh (see above), and the length it asks
    // for arrives with the target instead of being scanned for.
    if (target.size() < static_cast<std::size_t>(haveKey ? Ubiquitty::VT::maxLinkKeyLength : Ubiquitty::VT::maxLinkURILength)) {
        storeClipped(target, links.mruTarget.data(), links.mruTarget.size());
        links.isMRUTargetKeyed = haveKey;
        links.mruID = id;
    }

    t->currentLinkID = id;
    return id;
}

} // namespace Ubiquitty::Attributes::Hyperlinks
