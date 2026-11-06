// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Network/FileTransferDetect.h"
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace Ubiquitty::Network::Transfer {

namespace {

// Find the ZDLE header lead-in "**\x18" (ZPAD ZPAD ZDLE) and read the frame-type
// byte after it. Direction signal: the sender's `sz` leads with "rz\r" before its
// ZRQINIT (remote sends → we receive); a bare remote `rz` (ZRINIT) has no "rz\r".
/// What the walk below has decided so far.
struct WalkState {
    Detection kermit;
    std::size_t kermitOffset = 0; // where the held Kermit packet begins
    bool isZModemJudged = false; // bool-ok: the first lead has been read, or it has not
    bool isDone = false; // bool-ok: the walk has its answer
    Detection answer;
    std::size_t answerOffset = 0; // where the answer's preamble begins
};

/// Reads one candidate byte, advancing the state. Returns false when the walk is
/// over — either it has its answer or ZMODEM's header ran off the window's end.
bool readCandidate(std::string_view window, std::size_t at, WalkState& state)
{
    if (!state.isZModemJudged && window[at] == zModemZDLE && at >= 2
        && window[at - 1] == zModemZPAD && window[at - 2] == zModemZPAD) {
        state.isZModemJudged = true;
        const std::size_t formatIndex = at + 1;
        if (formatIndex >= window.size()) {
            // Header split across the boundary — wait for the next burst. ZMODEM has
            // no claim on this window, which leaves Kermit's still standing.
            state.isDone = true;
            state.answer = state.kermit;
            state.answerOffset = state.kermitOffset;
            return false;
        }
        const char format = window[formatIndex];
        if (format == zModemZBIN || format == zModemZHEX || format == zModemZBIN32) {
            Detection detection;
            detection.protocol = Protocol::ZModem;
            const std::string_view before = window.substr(0, at - 2);
            detection.direction = (before.find("rz\r") != std::string_view::npos
                                      || before.find("rz\n") != std::string_view::npos)
                ? Direction::Receive
                : Direction::Send;
            state.isDone = true;
            state.answer = detection;
            state.answerOffset = at - 2; // the first ZPAD opens the preamble
            return false;
        }
        // "**\x18" not followed by a real format byte — not ZMODEM, and no later
        // lead is looked at.
    }

    // Kermit send-init: a packet framed SOH (0x01) <len> <seq> 'S'. len and seq are
    // printable (char+32); the 4th byte is the packet type.
    if (!state.kermit && window[at] == kermitSOH && at + 3 < window.size()
        && static_cast<unsigned char>(window[at + 1]) >= 0x20 && static_cast<unsigned char>(window[at + 1]) <= 0x7E
        && static_cast<unsigned char>(window[at + 2]) >= 0x20 && static_cast<unsigned char>(window[at + 2]) <= 0x7E
        && window[at + 3] == 'S') {
        state.kermit.protocol = Protocol::Kermit;
        state.kermit.direction = Direction::Receive;
        state.kermitOffset = at;
        if (state.isZModemJudged) { // ZMODEM already declined this window
            state.isDone = true;
            state.answer = state.kermit;
            state.answerOffset = at;
            return false;
        }
    }
    return true;
}

// Find the ZDLE header lead-in "**\x18" (ZPAD ZPAD ZDLE) and read the frame-type
// byte after it. Direction signal: the sender's `sz` leads with "rz\r" before its
// ZRQINIT (remote sends → we receive); a bare remote `rz` (ZRINIT) has no "rz\r".
//
// Both protocols are decided in one walk, because both are hunting a rare byte —
// ZMODEM's lead ends in ZDLE and Kermit's packet opens with SOH — and one pass
// over the window costs what one search did, not what two did. ZMODEM outranks
// Kermit wherever each sits, so a Kermit packet met first is held until the first
// ZMODEM lead has been judged; and only that first lead is judged, a lead whose
// format byte is wrong ending ZMODEM's claim rather than deferring to the next.
// Whether a preamble could BEGIN anywhere in `bytes`.
//
// Every candidate the walk below reads starts on one of these two bytes, so a
// buffer holding neither begins nothing — which is what lets the carry be
// judged against a bounded prefix instead of the burst being copied to sit
// behind it. Answers only about starts: a candidate that began earlier and
// continues here is the caller's to carry.
bool couldPreambleBegin(std::string_view bytes)
{
    const unsigned char* const data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t size = bytes.size();
    std::size_t index = 0;

#if defined(__ARM_NEON)
    const uint8x16_t zdle = vdupq_n_u8(static_cast<std::uint8_t>(zModemZDLE));
    const uint8x16_t soh = vdupq_n_u8(static_cast<std::uint8_t>(kermitSOH));
    const auto matches = [&](const uint8x16_t chunk) {
        return vorrq_u8(vceqq_u8(chunk, zdle), vceqq_u8(chunk, soh));
    };
    // Four vectors per reduction: the pass only asks whether a sentinel is
    // there ANYWHERE, and every byte of ordinary output takes this path.
    //
    // The loops below locate the sentinel after this pass establishes its presence.
    for (; index + 64 <= size; index += 64) {
        const uint8x16_t hit = vorrq_u8(
            vorrq_u8(matches(vld1q_u8(data + index)), matches(vld1q_u8(data + index + 16))),
            vorrq_u8(matches(vld1q_u8(data + index + 32)), matches(vld1q_u8(data + index + 48))));
        if (vmaxvq_u8(hit) != 0)
            return true;
    }
    for (; index + 16 <= size; index += 16) {
        if (vmaxvq_u8(matches(vld1q_u8(data + index))) != 0)
            return true;
    }
#endif
    for (; index < size; ++index) {
        const unsigned char byte = data[index];
        if (byte == static_cast<unsigned char>(zModemZDLE) || byte == static_cast<unsigned char>(kermitSOH))
            return true;
    }
    return false;
}

struct PreambleWalkResult {
    Detection detection;
    std::size_t offset = 0;
};

PreambleWalkResult detectPreambleIn(std::string_view window)
{
    const unsigned char* const data = reinterpret_cast<const unsigned char*>(window.data());
    const std::size_t size = window.size();
    WalkState state;
    std::size_t index = 0;

#if defined(__ARM_NEON)
    const uint8x16_t zdle = vdupq_n_u8(static_cast<std::uint8_t>(zModemZDLE));
    const uint8x16_t soh = vdupq_n_u8(static_cast<std::uint8_t>(kermitSOH));
    for (; index + 16 <= size; index += 16) {
        const uint8x16_t chunk = vld1q_u8(data + index);
        const uint8x16_t hit = vorrq_u8(vceqq_u8(chunk, zdle), vceqq_u8(chunk, soh));
        if (vmaxvq_u8(hit) == 0)
            continue;
        // Every candidate this block holds is read here, so the next block is a
        // whole stride away however many it held.
        for (std::size_t offset = 0; offset < 16; ++offset) {
            const unsigned char byte = data[index + offset];
            if (byte != static_cast<unsigned char>(zModemZDLE) && byte != static_cast<unsigned char>(kermitSOH))
                continue;
            if (!readCandidate(window, index + offset, state))
                return { state.answer, state.answerOffset };
        }
    }
#endif
    for (; index < size; ++index) {
        const unsigned char byte = data[index];
        if (byte != static_cast<unsigned char>(zModemZDLE) && byte != static_cast<unsigned char>(kermitSOH))
            continue;
        if (!readCandidate(window, index, state))
            return { state.answer, state.answerOffset };
    }
    return { state.kermit, state.kermitOffset };
}

} // namespace

Detection detectPreamble(std::string_view window)
{
    // X/YMODEM have no distinctive sender-side opener (the receiver drives with
    // NAK/'C'); left to an explicit user action. None here keeps auto ZMODEM-only.
    return detectPreambleIn(window).detection;
}

void Detector::rememberTail(std::string_view bytes)
{
    // What the joining form would have left: the last `maxPreambleScan` bytes of
    // carry + burst.
    if (bytes.size() >= maxPreambleScan) {
        std::memcpy(m_carry.data(), bytes.data() + (bytes.size() - maxPreambleScan), maxPreambleScan);
        m_carryLength = static_cast<std::uint8_t>(maxPreambleScan);
        return;
    }
    const std::size_t total = m_carryLength + bytes.size();
    if (total <= maxPreambleScan) {
        std::memcpy(m_carry.data() + m_carryLength, bytes.data(), bytes.size());
        m_carryLength = static_cast<std::uint8_t>(total);
        return;
    }
    const std::size_t dropped = total - maxPreambleScan;
    std::memmove(m_carry.data(), m_carry.data() + dropped, m_carryLength - dropped);
    std::memcpy(m_carry.data() + (m_carryLength - dropped), bytes.data(), bytes.size());
    m_carryLength = static_cast<std::uint8_t>(maxPreambleScan);
}

Detection Detector::feed(std::string_view bytes)
{
    if (bytes.empty())
        return {};

    // A burst that begins no candidate cannot change what the joined window
    // answers beyond what a bounded prefix already settles: every candidate in
    // that window starts in the carry, and each reads at most `maxPreambleScan`
    // bytes forward. Scanning the carry against that prefix meets the same
    // candidates in the same order, which is what the answer depends on — the
    // walk holds a Kermit packet until the first ZMODEM lead is judged, and
    // judges only that lead.
    //
    // This is the path every byte of ordinary output takes, and it neither
    // copies the burst nor touches the heap.
    if (!couldPreambleBegin(bytes)) [[likely]] {
        if (m_carryLength != 0) [[unlikely]] {
            const std::size_t prefixBytes = bytes.size() < maxPreambleScan ? bytes.size() : maxPreambleScan;
            std::memcpy(m_straddle.data(), m_carry.data(), m_carryLength);
            std::memcpy(m_straddle.data() + m_carryLength, bytes.data(), prefixBytes);
            const PreambleMatch straddleMatch = classify({ m_straddle.data(), m_carryLength + prefixBytes });
            if (straddleMatch.detection) [[unlikely]] {
                m_lastMatchOffsetInFeed = straddleOffsetInFeed(straddleMatch.windowOffset);
                m_carryLength = 0; // fired — the caller takes over the stream
                return straddleMatch.detection;
            }
        }
        rememberTail(bytes);
        return {};
    }

    // A candidate begins here, so the whole window is judged at once: the walk's
    // answer depends on the order it meets candidates in, and only a window
    // holding all of them meets them in that order.
    m_joined.clear();
    m_joined.reserve(m_carryLength + bytes.size());
    m_joined.append(m_carry.data(), m_carryLength);
    m_joined.append(bytes.data(), bytes.size());

    const PreambleMatch match = classify(m_joined);
    if (match.detection) {
        m_lastMatchOffsetInFeed = straddleOffsetInFeed(match.windowOffset);
        m_carryLength = 0;
        return match.detection;
    }

    if (m_joined.size() > maxPreambleScan) {
        std::memcpy(m_carry.data(), m_joined.data() + (m_joined.size() - maxPreambleScan), maxPreambleScan);
        m_carryLength = static_cast<std::uint8_t>(maxPreambleScan);
    } else {
        std::memcpy(m_carry.data(), m_joined.data(), m_joined.size());
        m_carryLength = static_cast<std::uint8_t>(m_joined.size());
    }
    return {};
}

Detector::PreambleMatch Detector::classify(std::string_view window)
{
    const PreambleWalkResult walked = detectPreambleIn(window);
    return { walked.detection, walked.offset };
}

std::string_view protocolName(Protocol protocol)
{
    switch (protocol) {
    case Protocol::ZModem:
        return "ZMODEM";
    case Protocol::XModem:
        return "XMODEM";
    case Protocol::YModem:
        return "YMODEM";
    case Protocol::Kermit:
        return "Kermit";
    case Protocol::None:
        return "none";
    }
    // Outside the enumeration; see xtermButtonCode for why this is not a default.
    return "none";
}

} // namespace Ubiquitty::Network::Transfer
