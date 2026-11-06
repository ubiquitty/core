// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// How the process facts behind a title learn that they have changed.
//
// The segments divide by who reports them. The OSC title and the reported
// directory are PUSHED: a program writes OSC 0/2 or OSC 7 and the bytes carry
// the change, so the drain that parses them raises an event. The
// process-derived segments — the foreground chain, the foreground name, the
// session-leader shell — come from walking the process table, and the question
// is what makes that walk run.
//
// Where the kernel announces a fork, exec or exit, the edge is the whole
// signal: every way a title can change either raises an edge,
// arrives as output, or is a keystroke, and each of those already wakes the
// ingest loop. A wall-clock walk beside them is redundant, and it is not free —
// walking the table four times a second costs most of what an idle terminal
// spends.
//
// Where the kernel announces nothing, that redundancy is the whole mechanism.
// A platform with no process-event facility has to notice a change by looking,
// so Core paces the walk itself, on the ingest thread, and delivers the result
// as the same `processStateChanged` push. A frontend never learns which case it
// is in and never runs a timer either way.
//
// So none of this is platform code. Which case a session is in is a platform
// fact, and `PAL::ProcessFacts::deliversTreeEdges` is where it is asked; what
// to do about either case is a decision, and decisions are Core's.

#include <cstdint>

namespace Ubiquitty::Title {

/// The cadence for a session whose tree does not announce itself.
///
/// Slow on purpose: it is a fallback for a fact that changes rarely, not a
/// sampling rate. A second is Terminal.app's own background cadence, and the
/// cost of missing a change for that long is a title a moment stale, against a
/// walk of the machine's process table every time it fires.
inline constexpr std::uint32_t walkFallbackMilliseconds = 1000;

/// The least time between two walks driven by a kernel process-tree edge.
///
/// An edge is a push, so it needs no cadence of its own — but a build script
/// forking steadily would raise one continuously, and a walk per fork is a cost
/// nobody asked for. The floor bounds that. It DEFERS rather than drops: an
/// edge arriving inside the floor still causes a walk, at the floor's end. A
/// dropped edge leaves a title naming a process that has already been
/// replaced, since login exec's its way to the shell in one burst.
inline constexpr std::uint32_t edgeFloorMilliseconds = 100;

/// How still the process tree must be before an edge-driven walk answers it.
///
/// A leading rate limit is the wrong shape here. What a title wants is where
/// the tree SETTLES, not each transition through it: a shell loop running
/// `sleep` twenty times a second forks and exits twenty times a second, and
/// answering every edge walks the table ten times a second to describe a tree
/// that is never still — and puts `sleep` in the title half the time it looks.
///
/// So the walk waits for quiet instead. Each edge pushes the answer out; the
/// walk runs once the tree has held still this long.
inline constexpr std::uint32_t edgeSettleMilliseconds = 120;

/// The longest an edge may be deferred while the tree keeps moving.
///
/// Without it a permanently churning tree would never be walked at all, and a
/// title would freeze on whatever it last saw. With it a storm costs a bounded
/// two walks a second instead of ten, and a tree that settles is still answered
/// within `edgeSettleMilliseconds`.
inline constexpr std::uint32_t edgeMaximumDeferMilliseconds = 500;

} // namespace Ubiquitty::Title
