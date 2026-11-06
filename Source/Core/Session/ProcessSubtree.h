// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Session/ProcessSubtree.h — the pure, host-testable half of the process
// subtree gather. The platform enumerates every process into a flat
// RawNode list (pid/ppid/pgid/uid); this layer walks each node's
// parent-PID chain back toward the session leader and admits only the nodes
// whose chain reaches it, stamping isForeground / isSessionLeader on the
// admitted records. Admission is strictly by ancestry to `leader`, never a
// global tty/pgid, so two live sessions never bleed into one another.
//
// What stays platform: the per-pid resolution of name / commandLine / user
// (KERN_PROCARGS2, libproc, getpwuid on macOS; /proc on Linux) — those fill
// the PAL::Process::Record strings after this classifier admits a pid. The
// classifier itself touches no OS header, so it is exercised against scripted
// PPID lists in the host test suite.
//
// Exception/rtti-free (WebKit style); the only state is the caller-owned
// arrays passed in.

#include "PAL/Process.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::Session::Process {

// One raw, pre-classification process the platform enumerated. The platform
// fills the four scalars from its process table (proc_bsdshortinfo /
// /proc/<pid>/stat); the classifier reads them. Process ids are unsigned to
// match PAL::Process::Record; 0 marks an empty / unusable slot the classifier skips.
/// One raw, pre-classification process as the platform enumerated it.
struct RawNode {
    uint32_t processID;
    uint32_t parentProcessID;
    uint32_t processGroupID;
    uint32_t userID;
};

// True when `node` belongs to the subtree rooted at `leader`: it is the leader,
// or its parent-PID chain (walked through `nodes`) reaches the leader within
// `maxHops` hops. A chain that climbs past pid 1, hits a pid absent from
// `nodes`, or exceeds `maxHops` is rejected (returns false). Pure over the flat
// list — no OS lookup — so the ancestry policy is host-tested directly.
/// Whether `node` belongs to the subtree rooted at `leader`.
///
/// True when it is the leader, or its parent-PID chain — walked through `nodes`
/// — reaches the leader within `maxHops`. A chain that climbs past pid 1, hits a
/// pid absent from `nodes`, or exceeds `maxHops` is rejected.
///
/// Pure over the flat list with no OS lookup, so the ancestry policy is
/// host-tested directly rather than against a live process table.
///
/// - Parameters:
///   - nodes: The flat process list to walk.
///   - count: How many entries `nodes` holds.
///   - node: The process to test.
///   - leader: The session leader's process id.
///   - maxHops: Ancestry chain bound, guarding a cycle in a hostile table.
/// - Returns: `true` when `node` is in the subtree.
[[nodiscard]] bool processInSubtree(const RawNode* nodes, int count, const RawNode& node, uint32_t leader, int maxHops = 64);

// Classify `nodes[0..count)` against the session rooted at `leader`: write one
// PAL::Process::Record into `out` for each node in the subtree (the leader plus every
// descendant whose PPID chain reaches it), up to out.size() records, and return
// the count written. Each emitted record carries:
//   processID / parentProcessID / processGroupID  — copied from the raw node
//   isForeground      — processGroupID == foregroundGroup (and foregroundGroup > 0)
//   isSessionLeader   — processID == leader
//   tty               — every record stamped with `tty`
// The name / commandLine / user string buffers are left EMPTY (zeroed) for the
// platform to fill per admitted pid; only the ancestry + foreground/leader
// policy lives here. Returns 0 when out is empty or leader is 0.
/// Classifies a raw process list against one session and fills the records.
///
/// Writes one `PAL::Process::Record` per node in the subtree — the leader plus
/// every descendant whose PPID chain reaches it — up to `outputBuffer.size()`.
/// Each record carries the ids copied from the raw node, `isForeground` from
/// the process-group match, `isSessionLeader`, and the given `tty`.
///
/// The name, command-line and user string buffers are left ZEROED for the
/// platform to fill per admitted pid. Only the ancestry and foreground policy
/// lives here, which is what keeps this half testable without a process table.
///
/// - Parameters:
///   - nodes: The flat process list.
///   - count: How many entries `nodes` holds.
///   - leader: The session leader's process id; 0 yields no records.
///   - foregroundGroup: The foreground process-group id, or 0 for none.
///   - tty: The replica tty basename stamped onto every record.
///   - outputBuffer: Destination records; an empty span yields 0.
/// - Returns: How many records were written.
int gatherSubtree(const RawNode* nodes, int count, uint32_t leader, uint32_t foregroundGroup, std::string_view tty, std::span<PAL::Process::Record> outputBuffer);

} // namespace Ubiquitty::Session::Process
