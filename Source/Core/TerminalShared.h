// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Forward.h"
#include "PAL/Capability.h"
#include <span>
#include <string_view>

// The terminal-wide helpers that sibling Core modules call.
//
// None of the three is a parser concern by contents: `respond` writes to the
// Terminal's output buffer, `screenIndex` reads which screen is showing, and
// `decimalAppendSaturated` is arithmetic. They are declared here rather than
// under VT/, because OSC and Kitty reaching into VT/ for them makes
// Core/OSC <-> Core/VT and Core/Kitty <-> Core/VT cycles.
//
// Nor does a module re-declare them. A re-declaration is not a cheap include,
// it is a second source of truth: the copies must match the definition
// exactly, nothing checks that they do, a signature change surfaces at link
// time in whichever module was not edited, and the copies' doc comments drift
// apart.
//
// So the seam is a header, and it carries the least that keeps those names one
// source of truth: Forward.h and the capability enum, nothing else — anything
// heavier and the modules that include it pay for the parser they avoided.
// Adding a name here says a module outside VT may call it, which is a different
// claim from VT/Internal.h's "the VT TUs share this" — declarations only needed
// inside VT belong there.

namespace Ubiquitty {

/// What becomes of a reply the terminal would send its program.
enum class ReplyDisposition : uint8_t {
    /// This terminal writes it.
    Answer,
    /// The capability policy refused it, so nothing is written.
    Refuse,
    /// Another terminal answers for this one, so nothing is written here.
    AnsweredElsewhere,
};

/// Decides what becomes of one reply.
///
/// The question `respond` and `respondBulk` ask before a byte is queued, and
/// the only place it is answered: who answers — this terminal, nobody, or the
/// terminal that is authoritative for it — and whether the policy lets the
/// disclosure happen at all. A terminal answered elsewhere never consults its
/// policy, so a replica raises no consent prompt for a query its host answers.
///
/// - Parameters:
///   - t: The terminal asked.
///   - capability: The capability the reply discloses.
/// - Returns: The disposition.
[[nodiscard]] ReplyDisposition replyDisposition(Ubiquitty::Terminal& t, Ubiquitty::Capability capability);

/// Queues a reply to the host.
///
/// The single chokepoint for every host-facing reply, because a reply
/// exfiltrates host state: the bytes are gated on `capability` at the terminal's
/// current origin and never queued unless the decision is Allow. They append to
/// `Terminal::outputBuffer`, or follow any queued bulk bytes to preserve reply
/// order. The host drains both on its next event-loop tick.
/// A reply that would overrun that buffer is dropped whole rather than
/// truncated — half a control sequence is worse than none.
///
/// - Parameters:
///   - t: The terminal whose output buffer receives the reply.
///   - capability: The capability the reply discloses, checked against the
///     terminal's current origin.
///   - s: The NUL-terminated reply bytes. Under S8C1T each 7-bit C1 escape in
///     them is collapsed to its single 8-bit byte on the way out.
void respond(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, const char* s);

/// Sends a reply whose bytes are counted rather than terminated.
///
/// Same gate, same buffer, same S8C1T collapse as the string form, which
/// delegates here. It exists because a reply is not always text: the Tek GIN
/// report carries the byte of the key the user pressed, and a user can press the
/// one that reads as a terminator. Measuring the reply rather than scanning it
/// for a NUL is what keeps that report six bytes long whichever key it names.
///
/// - Parameters:
///   - t: The terminal whose output buffer receives the reply.
///   - capability: The capability the reply discloses, checked against the
///     terminal's current origin.
///   - bytes: The reply, exactly as many bytes as it has.
void respond(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, std::span<const char> bytes);

/// Queues a reply too large for the fixed output buffer.
///
/// Same capability gate and S8C1T collapse as ``respond``, but the bytes land
/// in the growable bulk channel, bounded by `maxBulkOutputBytes`. A refusal
/// reports rather than dropping silently, because a bulk reply is composed of
/// many packets and only the composer can take a partial composition back —
/// the caller truncates to where it began and answers with a small error the
/// fixed buffer can carry.
///
/// - Parameters:
///   - t: The terminal whose bulk channel receives the reply.
///   - capability: The capability the reply discloses, checked against the
///     terminal's current origin. A denial suppresses the bytes and reports
///     success: the policy answered, and there is nothing to take back.
///   - bytes: The reply, exactly as many bytes as it has.
/// - Returns: Whether the reply was handled — queued, or suppressed by
///   policy. False only when the channel is full.
[[nodiscard]] bool respondBulk(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, std::span<const char> bytes);

/// The string terminator an OSC reply ends with.
///
/// BEL while the terminal answers an OSC request that ended with BEL, ST
/// otherwise, so a reply ends the way its request did (xterm do_osc). A reply
/// to anything but an OSC request ends with ST.
///
/// - Parameter t: The terminal answering.
/// - Returns: `BEL` or `ESC \\`, in static storage.
[[nodiscard]] std::string_view oscReplyTerminator(const Ubiquitty::Terminal& t);

/// Rings the terminal's bell.
///
/// The one producer every bell goes through — the C0 BEL in each grammar, the
/// margin bell and the Tektronix bell — so the terminal's own `EffectRoute`
/// hears every one and no other terminal's route hears any. Also sets
/// `bellPending`, the state a frontend resolves on its drain.
///
/// - Parameter t: The terminal whose program rang.
void ringBell(Ubiquitty::Terminal& t);

/// Returns which screen buffer is showing, in the encoding DSR and DECRQM report.
///
/// - Parameter t: The terminal to inspect.
/// - Returns: 0 for the primary screen, 1 for the alternate.
[[nodiscard]] int screenIndex(const Ubiquitty::Terminal& t);

/// Appends one decimal digit to an accumulating parameter, saturating.
///
/// A host can send arbitrarily many digits, so the parameter parsers cannot
/// assume the running value stays in range; saturating at a declared ceiling is
/// what keeps an overlong parameter from wrapping into a small one.
///
/// - Parameters:
///   - current: The value accumulated so far.
///   - digit: The next digit, 0 through 9.
///   - limit: The ceiling to saturate at.
/// - Returns: `current * 10 + digit`, or `limit` if that would exceed it.
inline constexpr int decimalAppendSaturated(int current, int digit, int limit)
{
    if (current > (limit - digit) / 10)
        return limit;
    return current * 10 + digit;
}

} // namespace Ubiquitty
