// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/VT/Constants.h"
#include "Forward.h"

#include <array>
#include <cstddef>
#include <string_view>

// OSC 8 hyperlinks: the per-Terminal URI registry.
//
// A cell cannot hold a URI, so it holds a 16-bit id into this table and the
// renderer resolves it back when it needs the target. Ids are interned by
// (key, uri) so a shell emitting the same link on every prompt does not grow the
// table, and the id space is small enough that minting has to handle exhaustion.

namespace Ubiquitty::Attributes::Hyperlinks {

/// The ring's per-slot storage and its intern cache.
///
/// 132 KB, so `Terminal` holds it by pointer and the first `intern` that mints
/// an id allocates it. `linkCount` stays on `Terminal` and remains the gate: it
/// is zero exactly when there is nothing to resolve, table or no table.
///
/// The cache lives here so it cannot outlive the ring it caches — an id it
/// names that no slot holds would resolve to nullptr.
struct LinkTable {
    // First member: the dedup sweep reads it as four cache lines from the head
    // of the allocation.
    std::array<std::uint32_t, Ubiquitty::VT::linkTableCapacity> hashes {};
    std::array<std::uint16_t, Ubiquitty::VT::linkTableCapacity> ids {};
    std::array<std::array<char, Ubiquitty::VT::maxLinkKeyLength>, Ubiquitty::VT::linkTableCapacity> keys {};
    std::array<std::array<char, Ubiquitty::VT::maxLinkURILength>, Ubiquitty::VT::linkTableCapacity> uris {};

    // One-slot MRU over the last interned dedup target: a repeat open costs one
    // string compare and no ring scan. 0 == empty; cleared when the ring
    // recycles a slot.
    std::array<char, Ubiquitty::VT::maxLinkURILength> mruTarget {};
    std::uint16_t mruID { 0 };
    bool isMRUTargetKeyed { false };
};

// Bulk is the reason it is behind a pointer; below this it would not be.
static_assert(sizeof(LinkTable) > 1024 * sizeof(void*),
    "the OSC 8 ring is held by pointer on the strength of its size");
static_assert(offsetof(LinkTable, hashes) == 0,
    "the dedup sweep starts at the allocation");

/// The terminal's ring, or nullptr when no link has been interned.
[[nodiscard]] LinkTable* table(Ubiquitty::Terminal* t);

/// The terminal's ring for reading, or nullptr when none was allocated.
[[nodiscard]] const LinkTable* table(const Ubiquitty::Terminal* t);

/// The terminal's ring, allocating it on first use. Only `intern` needs this.
[[nodiscard]] LinkTable& ensureTable(Ubiquitty::Terminal& t);

/// Releases the ring and the id state that means nothing without it.
///
/// Sound only where no cell can still name an id.
void reset(Ubiquitty::Terminal& t);

/// Resolve an id back to its URI, or nullptr when the id is 0 / unknown / retired.
[[nodiscard]] const char* uri(const Ubiquitty::Terminal* t, uint16_t id);

/// Whether the entry behind an id was interned under an explicit `id=` key.
///
/// The ring keys on the `id=` token when a link carries one and on the URI
/// otherwise, and a repeat of one URI with no token dedups into the first
/// one's entry just as a repeated token does. Two cells sharing an id are
/// therefore either two runs a program declared to be one link or two
/// emissions of one target, and the entry's key field is what separates
/// them.
///
/// - Parameters:
///   - t: The terminal holding the ring.
///   - id: The id a cell carries.
/// - Returns: True for an entry interned with `id=`. False for the 0 id, an id
///   no live entry holds, and an entry keyed by its URI.
[[nodiscard]] bool isExplicitlyKeyed(const Ubiquitty::Terminal* t, uint16_t id);

/// Hashes an OSC 8 dedup target the way the link ring keys it.
///
/// The ring stores this beside each entry so a lookup rejects a miss with an
/// integer compare. Every writer of `linkHashes` has to reach it through here:
/// a slot whose stored hash disagrees with what a lookup computes is a slot no
/// lookup can find, which reads as a link that will not dedup rather than as a
/// wrong answer.
///
/// Never returns 0, which the ring reserves to mean an empty slot.
///
/// - Parameter target: The dedup key when the sequence carried `id=`, else the URI.
/// - Returns: The slot key for `target`.
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
/// - Returns: The id, or 0 when the id space is spent or the target is empty.
uint16_t intern(Ubiquitty::Terminal* t, std::string_view key, std::string_view target);

} // namespace Ubiquitty::Attributes::Hyperlinks
