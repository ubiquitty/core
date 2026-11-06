// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>

// The per-OSC / per-DCS / per-APC sequence gate (security). One value per
// Terminal, keyed on the OSC Ps code, on stable DCS sub-protocol ids, and on an
// APC's first byte (`G` introduces kitty graphics). Policy (deny wins; a
// non-empty allowlist is exclusive): in deny → blocked; else allow non-empty and
// not in it → blocked; else allowed. Both sets empty (the default) passes
// everything. The gate runs in addition to the finer OSC-52/color/font gates,
// never instead. A blocked sequence is parsed and consumed cleanly (no hook, no
// response).
//
// A plain value rather than a pointer to shared state: two Terminals in one
// process share zero policy, and a session carries its gate wherever the
// Terminal goes — across a hard reset and across a re-home into the daemon.
// Fixed-capacity arrays, so the copy is a memcpy and dispatch consults it with
// a linear scan over a tiny (commonly empty) set.

namespace Ubiquitty::OSC {

/// A DCS sub-protocol, as a config list or the wire names it.
///
/// A DCS is keyed by (intermediate, final) and mapped here to a small integer.
/// The values are the config and wire contract and never change.
enum class DCSKind : uint8_t {
    Sixel = 0, // DCS … q   (intermediate 0, final 'q')
    DECUDK = 1, // DCS … | (intermediate 0, final '|')
    DECRQSS = 2, // DCS $ q
    XTGETTCAP = 3, // DCS + q
    Tmux = 4, // tmux control-mode passthrough (table miss)
    DECDLD = 5, // DCS … { — downloadable soft character set (DRCS)
    DECRSTS = 6, // DCS $ p — host-supplied terminal state (DECTSR's inverse)
    Sample = 7, // DCS + s — a sample download for DECPS's sample voice
};

/// The highest DCSKind value; a wire byte above it names no sub-protocol.
constexpr uint8_t lastDCSKindValue = static_cast<uint8_t>(DCSKind::Sample);

/// How many OSC codes an allow or deny list holds. Generous: there are ~40 known OSC codes.
constexpr int oscListCapacity = 64;
/// How many DCS kinds an allow or deny list holds; there are eight DCS kinds.
constexpr int dcsListCapacity = 8;
/// How many APC introducers an allow or deny list holds.
constexpr int apcListCapacity = 16;

/// Whether a byte can name an APC in a list: printable ASCII other than space.
///
/// An APC is keyed by the first byte of its payload. The lists, the config and
/// the wire name only these, so a byte outside the range names no APC.
[[nodiscard]] constexpr bool isAPCIntroducer(uint8_t byte)
{
    return byte >= 0x21 && byte <= 0x7E;
}

/// One terminal's OSC, DCS and APC allow and deny lists, each with its count.
///
/// An empty list is absent. Deny beats allow, and a non-empty allow list is
/// exclusive. Installed whole through the `set*List` functions in
/// `Core/OSC/Dispatch.h`, which clamp a count to the list's capacity.
///
/// An over-capacity DENY list cannot be honored by truncation: the codes past
/// the cap would be permitted, which is the opposite of what naming them
/// asked for. So a truncated deny closes its whole dimension instead — the
/// overflow flags below — and `oscAllowed` / `dcsAllowed` / `apcAllowed` deny everything while
/// it stands, allow list included (an allowed code may itself sit in the
/// dropped tail). A fitting reinstall clears it. Sessions keep running under
/// the closed gate, loudly (the truncation is logged where it happens), until
/// the list is fixed and reloaded; running them under a weaker policy than
/// configured would be silent exfiltration.
struct SequenceGate {
    std::array<int, oscListCapacity> oscAllow {};
    int oscAllowCount = 0;
    std::array<int, oscListCapacity> oscDeny {};
    int oscDenyCount = 0;
    std::array<DCSKind, dcsListCapacity> dcsAllow {};
    int dcsAllowCount = 0;
    std::array<DCSKind, dcsListCapacity> dcsDeny {};
    int dcsDenyCount = 0;
    std::array<uint8_t, apcListCapacity> apcAllow {};
    int apcAllowCount = 0;
    std::array<uint8_t, apcListCapacity> apcDeny {};
    int apcDenyCount = 0;
    /// An OSC deny list was installed truncated: deny every OSC code.
    bool hasOSCDenyOverflow = false;
    /// A DCS deny list was installed truncated: deny every DCS kind.
    bool hasDCSDenyOverflow = false;
    /// An APC deny list was installed truncated: deny every APC.
    bool hasAPCDenyOverflow = false;
};

} // namespace Ubiquitty::OSC
