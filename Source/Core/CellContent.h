// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"

// Bounds and sentinels for what a cell can hold. A cell carries one base
// codepoint inline; anything longer spills whole into the cluster pool, which is
// why the pool bounds sit beside the sentinel marking a wide character's second
// column.

// Continuation-half sentinel: a wide-cell pair stores the glyph in the lead
// cell (Attribute::Wide) and this in the cont cell (Attribute::WideContinuation).
// Writers + idempotency probes reference this so the contract isn't tied to 0.

namespace Ubiquitty {
constexpr uint32_t wideContinuationSentinelCodePoint = 0;

/// Columns a stored wide run occupies: a lead plus its continuations.
///
/// This is the width the grid's editing controls are WRITTEN for, not the width
/// its producers happen to emit. Each one reaches a partner at an offset spelled
/// from this constant — a lead one column left of its continuation, a
/// continuation one column right of its lead — so a run of another width leaves
/// one half standing while clearing the other.
///
/// ``Screen::multicellRunAt`` answers the covering-run question for a run of
/// any width, and the write path's invariant machinery — the orphan scrub, the
/// range widening, the overlap breaking, the selection walks — derives each
/// run's extent from its continuation chain rather than from this constant.
/// This is the WIDE CHARACTER's claim alone: what ``claimedRunColumns``
/// answers for a pen whose only run mark is `Attribute::Wide`, and what the
/// wide-character producer writes. A producer of another width carries its own
/// claim; nothing else in the write path is sized by this.
constexpr int wideRunColumns = 2;

/// Columns from a wide run's lead to its continuation.
constexpr int wideContinuationOffset = wideRunColumns - 1;

// Grapheme-cluster overflow pool — full codePoint sequences for clusters too
// long for a Cell's inline combining buffer (long ZWJ emoji families). An
// overflowing cell interns its whole sequence and stores a 1-based 15-bit id
// (cellClusterID); cells that fit inline keep clusterID 0.
//
/// The pool grows to hold every distinct cluster shown; interning dedups by
/// content. An id names one sequence for the pool's whole life and is never
/// reused, so a cell reads back the cluster it was written with or, for an id
/// the pool doesn't hold, its own base plus inline mark — never another
/// cluster's text. At the 15-bit ceiling of 32767 entries a new sequence is
/// refused and that new cell keeps only its base and inline mark; both are
/// pinned by Tests/Core/test_cell_cluster.cpp.
///
/// cellMaxCombining is 1, so a base with two marks already interns.
/// clusterPoolMaxEntries bounds growth and id space so the id fits 15 bits.
constexpr int clusterPoolMaxEntries = 32767;
constexpr int clusterPoolMaximumCodePoints = 16;

/// The most code points one proportional run's text holds: the OSC 66 payload
/// bound, since a code point takes at least one byte. The run-text store holds
/// a run of this length as one entry, and a reader that wants a whole run sizes
/// its buffer by it.
constexpr int runTextMaximumCodePoints = 4096;

} // namespace Ubiquitty
