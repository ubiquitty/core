// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable detection of an in-band file-transfer handshake (ZMODEM sz/rz, and
// X/YMODEM + Kermit preambles) in the PTY output byte stream, so the frontend can
// auto-launch a transfer agent instead of dumping the binary frame to screen.
// A rolling matcher holds at most `maxPreambleScan` bytes of carry-over, so a
// handshake split across two read() bursts is still caught. No platform headers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Network::Transfer {

// The transfer protocol a detected preamble belongs to.
enum class Protocol : std::uint8_t {
    None = 0,
    ZModem, // rz/sz — ZDLE-framed; the de-facto auto-transfer
    XModem, // bare NAK/'C' poll → block 1 SOH/STX
    YModem, // XMODEM framing + a filename block-0
    Kermit, // SOH-marked packets, type 'S' send-init
};

// What the remote side is doing, so the frontend launches the matching agent.
//   Receive  — the remote is SENDING to us (sz / kermit send) → run `rz`
//   Send     — the remote is asking US to send (rz waiting) → run `sz <file>`
enum class Direction : std::uint8_t {
    Unknown = 0,
    Receive, // download: remote → us
    Send, // upload: us → remote
};

struct Detection {
    Protocol protocol = Protocol::None;
    Direction direction = Direction::Unknown;
    explicit operator bool() const { return protocol != Protocol::None; }
};

// Longest preamble we ever need to keep across a read boundary. The ZMODEM rz
// preamble ("rz\r" + the ZDLE header) plus a little slack; bounded so the carry
// buffer can never grow with the stream.
inline constexpr std::size_t maxPreambleScan = 64;

// ZMODEM control bytes (rzsz.h): ZPAD '*', ZDLE 0x18 (CAN), the "**\x18B"
// hex-header lead-in, and the frame-type byte that follows it.
inline constexpr char zModemZPAD = '*';
inline constexpr char zModemZDLE = 0x18; // Can
inline constexpr char zModemZBIN = 'A'; // binary header
inline constexpr char zModemZHEX = 'B'; // hex header
inline constexpr char zModemZBIN32 = 'C'; // binary header, CRC-32
inline constexpr char kermitSOH = 0x01; // Kermit packet frame start

// A streaming detector. Feed it the same bytes the VT parser ingests; keeps only
// a short tail. Single-threaded by construction: it rides the ingest thread
// (Docs/coding-conventions.md §5.2).
class Detector {
public:
    // Scan `bytes`; returns a truthy Detection on the FIRST burst that completes
    // a preamble, else a falsy one. Once it fires, the caller hands the PTY to a
    // transfer agent; reset() re-arms it for the next session.
    Detection feed(std::string_view bytes);

    /// The count of the firing burst's bytes that precede the preamble.
    ///
    /// Meaningful only after a truthy ``feed()``: the caller must parse no more
    /// of that burst than this says — from this offset on the burst is the
    /// transfer frame, and rendering it corrupts the screen. Zero when the
    /// preamble began in an earlier burst's carry, in which case the whole
    /// burst is frame. `-1` when ``feed()`` has not fired since the last
    /// ``reset()``.
    ///
    /// - Returns: Bytes of the firing burst that are safe to render, or `-1`.
    [[nodiscard]] int lastMatchOffsetInFeed() const { return m_lastMatchOffsetInFeed; }

    /// Re-arms the detector, forgetting any partial match.
    ///
    /// Call after a transfer finishes, so a preamble split across the handover
    /// cannot complete against bytes from before it.
    void reset()
    {
        m_carryLength = 0;
        m_lastMatchOffsetInFeed = -1;
    }

private:
    /// A classification with where in the window its preamble began.
    ///
    /// One value rather than a detection beside an out-parameter: the offset is
    /// meaningless without the detection and the detection incomplete without
    /// the offset, so the two travel together or a caller reads one stale.
    struct PreambleMatch {
        Detection detection;
        /// Where the preamble starts in the classified window; zero when
        /// `detection` is falsy.
        std::size_t windowOffset = 0;
    };

    /// Classifies a concatenated carry+burst window.
    ///
    /// - Parameter window: The carry+burst window to classify.
    /// - Returns: The match, whose detection is falsy when nothing completed.
    [[nodiscard]] static PreambleMatch classify(std::string_view window);

    /// Maps a match's window offset to how much of the firing burst is safe.
    ///
    /// The window is carry + burst: a match that began inside the carry
    /// predates the burst, so nothing of the burst is safe to render.
    ///
    /// - Parameter matchedWindowOffset: Where the preamble began in the window.
    /// - Returns: The count of leading burst bytes the model may keep.
    [[nodiscard]] int straddleOffsetInFeed(std::size_t matchedWindowOffset) const
    {
        if (matchedWindowOffset <= m_carryLength)
            return 0;
        return static_cast<int>(matchedWindowOffset - m_carryLength);
    }

    /// Keeps the tail a following burst may complete a candidate from.
    ///
    /// Holds what the joining form would have left: the last `maxPreambleScan`
    /// bytes of carry followed by burst, which is every byte a candidate
    /// straddling the boundary can begin in.
    ///
    /// - Parameter bytes: The burst just judged.
    void rememberTail(std::string_view bytes);

    // Both buffers are inline and fixed: this object rides the ingest thread and
    // is touched once per ring bite, so a heap indirection here is a pointer
    // chase on every byte the terminal receives. Bounded by the same constant
    // the carry is trimmed to, which is what makes the bound sound rather than
    // chosen.
    std::array<char, maxPreambleScan> m_carry {}; // bytes straddling the last burst
    std::array<char, 2 * maxPreambleScan> m_straddle {}; // carry + the prefix a carry candidate can reach
    std::uint8_t m_carryLength = 0;
    // Bytes of the last firing burst that precede the preamble (see
    // lastMatchOffsetInFeed); -1 while no burst has fired since reset().
    std::int32_t m_lastMatchOffsetInFeed = -1;
    // Only a burst that begins a candidate is joined, which normal output never
    // does, so this stays empty for the life of most sessions.
    std::string m_joined;
};

// Stateless one-shot: does `window` (a single contiguous buffer) contain a
// transfer preamble? Exposed for tests + callers that already have the full
// burst. The Detector wraps this with cross-burst carry handling.
Detection detectPreamble(std::string_view window);

// Human-readable protocol name for logs / the confirmation prompt.
std::string_view protocolName(Protocol protocol);

} // namespace Ubiquitty::Network::Transfer
