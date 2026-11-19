// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/VT/Constants.h"
#include "Forward.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// OSC 8 hyperlinks: the per-Terminal URI registry.
//
// A cell cannot hold a URI, so it holds a 16-bit id into this table and the
// renderer resolves it back when it needs the target. Ids are interned by
// (key, uri) so a shell emitting the same link on every prompt does not grow the
// table, and the id space is small enough that minting has to handle exhaustion.

namespace Ubiquitty::Attributes::Hyperlinks {

/// Which field of an entry a dedup target is compared against.
enum class TargetField : std::uint8_t {
    /// The OSC 8 `id=` token, for a link that carried one.
    Key,
    /// The URI, for a link that carried no token.
    URI,
};

/// Whether an appended entry can be found again by its dedup target.
enum class Findability : std::uint8_t {
    /// The stored target is the whole target, so a repeat of it finds this entry.
    Findable,
    /// The stored target was clipped to fit and cannot equal any target, so
    /// indexing it would only lengthen the probe a repeat of it walks.
    Unfindable,
};

/// One live link as the table hands it out.
struct LinkEntryView {
    /// The dedup hash of the target the entry was interned under.
    std::uint32_t hash = 0;
    /// The id cells carry for this link.
    std::uint16_t id = 0;
    /// The `id=` token, or empty for a link keyed by its URI.
    std::string_view key;
    /// The URI. Its storage is NUL-terminated.
    std::string_view uri;
};

/// The terminal's links, oldest first, each held for as long as a cell names it.
///
/// A cell names its link by a 16-bit id, and the id is all a click or a
/// renderer has to go on. An entry therefore stays while any cell may still
/// carry its id: the census `intern` takes drops only entries no cell on either
/// screen or in the scrollback names. The table grows with what it holds. A
/// fixed ring recycled its oldest slot while a screen full of links (one per
/// file in a directory listing) still showed it, and those cells were drawn as
/// links that resolved to nothing and opened nothing.
///
/// Storage is allocated nothrow and admitted before it is used, so a refused
/// grow leaves the table as it was and the caller decides what that means.
class LinkTable {
public:
    /// One entry per id, and ids are 16-bit with 0 reserved.
    static constexpr std::size_t maxEntries = 0xFFFF;
    /// What `indexOf` and `find` return when nothing matches.
    static constexpr std::size_t notFound = static_cast<std::size_t>(-1);

    LinkTable() = default;
    LinkTable(const LinkTable&) = delete;
    LinkTable& operator=(const LinkTable&) = delete;

    /// The number of links held.
    ///
    /// - Returns: The entry count.
    [[nodiscard]] std::size_t count() const noexcept { return m_count; }

    /// The entry at a position, oldest first.
    ///
    /// - Parameter index: A position below `count()`.
    /// - Returns: The entry's view, valid until the table next changes.
    [[nodiscard]] LinkEntryView entry(std::size_t index) const noexcept;

    /// The position of the entry holding an id.
    ///
    /// - Parameter id: The id a cell carries.
    /// - Returns: The position, or `notFound` for 0 and for an id no entry holds.
    [[nodiscard]] std::size_t indexOf(std::uint16_t id) const noexcept
    {
        if (id >= m_idSlots)
            return notFound;
        const std::uint16_t slot = m_slotOfID[id];
        return slot == noSlot ? notFound : slot;
    }

    /// The position of the entry interned under a dedup target.
    ///
    /// - Parameters:
    ///   - target: The `id=` token or the URI, as the link was interned.
    ///   - field: Which of the entry's fields `target` is compared with.
    ///   - hash: `dedupHash(target)`.
    /// - Returns: The position, or `notFound`.
    [[nodiscard]] std::size_t find(std::string_view target, TargetField field, std::uint32_t hash) const noexcept;

    /// Whether the entry at a position was interned under a target.
    ///
    /// - Parameters:
    ///   - index: A position below `count()`.
    ///   - target: The `id=` token or the URI.
    ///   - field: Which of the entry's fields `target` is compared with.
    /// - Returns: True when the entry's field holds exactly `target`.
    [[nodiscard]] bool holds(std::size_t index, std::string_view target, TargetField field) const noexcept;

    /// The bytes of link text stored, including text an `assign` left behind.
    ///
    /// - Returns: The stored text's size.
    [[nodiscard]] std::size_t textBytes() const noexcept { return m_textUsed; }

    /// Admits storage for at least the given contents before any is added.
    ///
    /// - Parameters:
    ///   - entryCapacity: The entries to hold.
    ///   - textCapacity: The text bytes to hold, two terminators an entry included.
    ///   - largestID: The largest id an entry will carry.
    /// - Returns: False when the storage is refused; the table is then unchanged.
    [[nodiscard]] bool reserve(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept;

    /// The bytes `reserve` allocates for given contents, excluding the table object.
    ///
    /// - Parameters:
    ///   - entryCapacity: The entries to hold.
    ///   - textCapacity: The text bytes to hold.
    ///   - largestID: The largest id an entry will carry.
    /// - Returns: The allocation's size.
    [[nodiscard]] static std::size_t storageBytesFor(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept;

    /// The text bytes one entry stores.
    ///
    /// - Parameters:
    ///   - key: The stored `id=` token.
    ///   - uri: The stored URI.
    /// - Returns: Both strings and their terminators.
    [[nodiscard]] static std::size_t textBytesFor(std::string_view key, std::string_view uri) noexcept
    {
        return key.size() + uri.size() + 2;
    }

    /// Adds a link as the newest entry.
    ///
    /// - Parameters:
    ///   - id: An id no entry holds.
    ///   - hash: The dedup hash of the target the link was interned under.
    ///   - key: The `id=` token, already clipped to fit.
    ///   - uri: The URI, already clipped to fit.
    ///   - findability: Whether a repeat of the target should find the entry.
    /// - Returns: False when storage is refused, the id is 0 or already held,
    ///   or the table is full; the table is then unchanged.
    [[nodiscard]] bool append(std::uint16_t id, std::uint32_t hash, std::string_view key, std::string_view uri, Findability findability) noexcept;

    /// Replaces what the entry at a position holds, keeping its id and place.
    ///
    /// - Parameters:
    ///   - index: A position below `count()`.
    ///   - hash: The new target's dedup hash.
    ///   - key: The new `id=` token.
    ///   - uri: The new URI.
    /// - Returns: False when storage is refused; the entry is then unchanged.
    [[nodiscard]] bool assign(std::size_t index, std::uint32_t hash, std::string_view key, std::string_view uri) noexcept;

    /// Keeps only the entries whose ids are marked, in their order, and packs
    /// their text. Allocates nothing.
    ///
    /// - Parameter referenced: One bit per id, 1024 words.
    void retain(std::span<const std::uint64_t, 1024> referenced) noexcept;

    /// Drops the oldest entries until the text is at most a size. Allocates nothing.
    ///
    /// - Parameter textBytes: The stored text to come down to.
    void dropOldestUntil(std::size_t textBytes) noexcept;

    /// The position of the entry the last intern resolved, or `notFound`.
    ///
    /// The common re-open of the same target around every styled span is one
    /// string compare against it, with no hash and no probe. Reset whenever the
    /// table moves or rewrites an entry.
    std::size_t recentIndex = notFound;

private:
    /// One entry's fixed part; its strings live in the text buffer.
    struct Slot {
        /// The dedup hash of the target.
        std::uint32_t hash;
        /// Where the key begins in the text buffer; the URI follows its terminator.
        std::uint32_t textOffset;
        /// The id cells carry.
        std::uint16_t id;
        /// The key's length.
        std::uint16_t keyLength;
        /// The URI's length.
        std::uint16_t uriLength;
        /// Whether the entry is in the dedup index.
        Findability findability;
    };

    /// An index bucket no entry has used.
    static constexpr std::uint32_t emptyBucket = 0;
    /// An index bucket whose entry moved or left; probes continue past it.
    static constexpr std::uint32_t vacatedBucket = 0xFFFFFFFFu;
    /// An id with no entry in the id index.
    static constexpr std::uint16_t noSlot = 0xFFFF;

    /// The bucket count that keeps an index of a given size at most half full.
    ///
    /// - Parameter entryCapacity: The entries the index must hold.
    /// - Returns: A power of two.
    [[nodiscard]] static std::size_t bucketsFor(std::size_t entryCapacity) noexcept;

    /// The id-index length that covers an id.
    ///
    /// - Parameter largestID: The largest id to cover.
    /// - Returns: A power of two above `largestID`.
    [[nodiscard]] static std::size_t idSlotsFor(std::uint16_t largestID) noexcept;

    /// The stored key of an entry.
    ///
    /// - Parameter slot: The entry.
    /// - Returns: A view into the text buffer.
    [[nodiscard]] std::string_view keyOf(const Slot& slot) const noexcept;

    /// The stored URI of an entry.
    ///
    /// - Parameter slot: The entry.
    /// - Returns: A view into the text buffer, NUL-terminated.
    [[nodiscard]] std::string_view uriOf(const Slot& slot) const noexcept;

    /// Grows the arrays so they hold at least the given contents, admitting
    /// every new array before any is moved.
    ///
    /// - Parameters:
    ///   - entryCapacity: The entries to hold.
    ///   - textCapacity: The text bytes to hold.
    ///   - largestID: The largest id to cover.
    /// - Returns: False when storage is refused; the table is then unchanged.
    [[nodiscard]] bool grow(std::size_t entryCapacity, std::size_t textCapacity, std::uint16_t largestID) noexcept;

    /// Makes room for one more entry's text and id, packing the text first
    /// when what assignments left behind is enough.
    ///
    /// - Parameters:
    ///   - textNeeded: The text bytes the entry adds.
    ///   - id: The entry's id.
    ///   - entryCapacity: The entries the table must hold afterwards.
    /// - Returns: False when storage is refused; the table is then unchanged.
    [[nodiscard]] bool admit(std::size_t textNeeded, std::uint16_t id, std::size_t entryCapacity) noexcept;

    /// Copies an entry's strings to the end of the text buffer, which `admit`
    /// has already sized.
    ///
    /// - Parameters:
    ///   - key: The key.
    ///   - uri: The URI.
    /// - Returns: The key's offset.
    std::uint32_t storeText(std::string_view key, std::string_view uri) noexcept;

    /// Puts an entry into the dedup index.
    ///
    /// - Parameters:
    ///   - hash: The entry's hash.
    ///   - index: The entry's position.
    void indexInsert(std::uint32_t hash, std::size_t index) noexcept;

    /// Takes an entry out of the dedup index, leaving a vacated bucket.
    ///
    /// - Parameters:
    ///   - hash: The hash it was indexed under.
    ///   - index: The entry's position.
    void indexErase(std::uint32_t hash, std::size_t index) noexcept;

    /// Rebuilds both indexes from the entries.
    void reindex() noexcept;

    /// Packs the text so it holds only what the entries name.
    void packText() noexcept;

    /// Moves the entries from a position on to the front, then reindexes and
    /// packs the text.
    ///
    /// - Parameter first: The first entry to keep.
    void dropBefore(std::size_t first) noexcept;

    /// The entries, oldest first.
    std::unique_ptr<Slot[]> m_slots;
    /// Entries held.
    std::size_t m_count = 0;
    /// Entries `m_slots` has room for.
    std::size_t m_capacity = 0;
    /// Every entry's key and URI, each NUL-terminated.
    std::unique_ptr<char[]> m_text;
    /// Bytes of `m_text` written.
    std::size_t m_textUsed = 0;
    /// Bytes of `m_text` the entries name.
    std::size_t m_textLive = 0;
    /// Bytes `m_text` has room for.
    std::size_t m_textCapacity = 0;
    /// Entry position by id, `noSlot` for none.
    std::unique_ptr<std::uint16_t[]> m_slotOfID;
    /// Ids `m_slotOfID` covers.
    std::size_t m_idSlots = 0;
    /// Dedup index: entry position + 1 by hash, or a sentinel.
    std::unique_ptr<std::uint32_t[]> m_buckets;
    /// Buckets in `m_buckets`, a power of two.
    std::size_t m_bucketCount = 0;
    /// Buckets `indexErase` left vacated.
    std::size_t m_vacatedBuckets = 0;
};

/// The link text a terminal keeps before it drops entries to stay within it.
///
/// Thousands of ordinary links fit many times over; the bound is for a stream
/// of long distinct targets, which would otherwise hold up to 65535 of them at
/// 2 KB each. Past it, links no cell names go first, then the oldest.
inline constexpr std::size_t textBudgetBytes = std::size_t { 4 } << 20;

/// The terminal's table, or nullptr when no link has been interned.
///
/// - Parameter t: The terminal.
/// - Returns: The table.
[[nodiscard]] LinkTable* table(Ubiquitty::Terminal* t);

/// The terminal's table for reading, or nullptr when none was allocated.
///
/// - Parameter t: The terminal.
/// - Returns: The table.
[[nodiscard]] const LinkTable* table(const Ubiquitty::Terminal* t);

/// The terminal's table, allocating it on first use.
///
/// - Parameter t: The terminal.
/// - Returns: The table.
[[nodiscard]] LinkTable& ensureTable(Ubiquitty::Terminal& t);

/// The number of links a terminal holds.
///
/// - Parameter t: The terminal.
/// - Returns: 0 when it has no table.
[[nodiscard]] std::size_t count(const Ubiquitty::Terminal& t);

/// Releases the table and the id state that means nothing without it.
///
/// Sound only where no cell can still name an id.
///
/// - Parameter t: The terminal.
void reset(Ubiquitty::Terminal& t);

/// Resolve an id back to its URI, or nullptr when the id is 0 / unknown / retired.
///
/// - Parameters:
///   - t: The terminal holding the table.
///   - id: The id a cell carries.
/// - Returns: The NUL-terminated URI, valid until the table next changes.
[[nodiscard]] const char* uri(const Ubiquitty::Terminal* t, uint16_t id);

/// Whether the entry behind an id was interned under an explicit `id=` key.
///
/// The table keys on the `id=` token when a link carries one and on the URI
/// otherwise, and a repeat of one URI with no token dedups into the first
/// one's entry just as a repeated token does. Two cells sharing an id are
/// therefore either two runs a program declared to be one link or two
/// emissions of one target, and the entry's key field is what separates
/// them.
///
/// - Parameters:
///   - t: The terminal holding the table.
///   - id: The id a cell carries.
/// - Returns: True for an entry interned with `id=`. False for the 0 id, an id
///   no live entry holds, and an entry keyed by its URI.
[[nodiscard]] bool isExplicitlyKeyed(const Ubiquitty::Terminal* t, uint16_t id);

/// Hashes an OSC 8 dedup target the way the table keys it.
///
/// The table stores this beside each entry so a lookup rejects a miss with an
/// integer compare. Every writer of an entry has to reach it through here: an
/// entry whose stored hash disagrees with what a lookup computes is one no
/// lookup can find, which reads as a link that will not dedup rather than as a
/// wrong answer.
///
/// Never returns 0.
///
/// - Parameter target: The dedup key when the sequence carried `id=`, else the URI.
/// - Returns: The key for `target`.
[[nodiscard]] std::uint32_t dedupHash(std::string_view target);

/// Interns a hyperlink target and returns the id a cell stores for it.
///
/// Repeat calls with the same pair return the same id, so a shell emitting one
/// link on every prompt costs one table entry. An empty target closes the
/// current link rather than interning anything.
///
/// - Parameters:
///   - t: The terminal whose table is being interned into.
///   - key: The OSC 8 `id=` token, or empty when the link carries none. When
///     present it is what the dedup keys on, so two runs of a multi-line link
///     share one entry even though their targets are compared byte for byte.
///   - target: The URI.
/// - Returns: The id, or 0 when the id space is spent, storage is refused or
///   the target is empty.
uint16_t intern(Ubiquitty::Terminal* t, std::string_view key, std::string_view target);

/// Installs a link under an id another terminal minted, as a replica does.
///
/// Over the entry holding the id when there is one, else as the newest entry.
/// A replica takes no census of its own, so past `textBudgetBytes` it drops
/// its oldest entries.
///
/// - Parameters:
///   - t: The replica terminal.
///   - id: The id the minting terminal gave the link.
///   - key: The `id=` token, or empty.
///   - uri: The URI.
/// - Returns: False when storage is refused; the table is then unchanged.
bool install(Ubiquitty::Terminal& t, std::uint16_t id, std::string_view key, std::string_view uri);

/// Adds one carried entry, as a handover or checkpoint rebuilds a table.
///
/// - Parameters:
///   - table: The table being rebuilt, already reserved.
///   - entry: The entry as it was carried.
/// - Returns: False when storage is refused or the id is 0 or held twice.
[[nodiscard]] bool appendCarried(LinkTable& table, const LinkEntryView& entry);

} // namespace Ubiquitty::Attributes::Hyperlinks
