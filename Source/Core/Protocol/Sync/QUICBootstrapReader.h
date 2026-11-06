// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Reads the daemon's `--quic-bootstrap` line off a carrier that delivers it in
// pieces. parseQUICBootstrapLine judges one complete line; this accumulates
// until there is one.
//
// A frontend that runs the bootstrap as a subprocess can read to end-of-file and
// parse once. A frontend whose carrier is an SSH exec channel cannot: bytes
// arrive at no boundary in particular, so the line can straddle two deliveries
// and the reader has to hold the tail. Same refusal rules either way, decided
// here rather than per frontend.
//
// The bytes are remote output, so they are attacker-reachable: the buffer is
// capped and a remote that never emits a line is refused rather than allowed to
// grow it.

#include "Core/Protocol/Sync/QUICBootstrap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace Ubiquitty::Protocol::Sync {

/// Maximum bytes in one bootstrap output line, excluding its newline.
///
/// A bootstrap line is well under 200 bytes; the slack absorbs a login banner
/// that reached stdout, and the cap refuses a remote that streams instead.
inline constexpr std::size_t maxQUICBootstrapBufferSize = 8192;

/// The bytes one feed() call carries.
///
/// A named alias rather than the span written out, because Swift imports it and
/// the bare type it stands for. `std::span`'s second parameter defaults to
/// `dynamic_extent`, Swift will not infer it, and `std.span<UInt8>` is rejected
/// as "too few type parameters" — a fully specialised alias is spellable from
/// both languages while remaining the same type as the span it names, so no
/// C++ caller changes.
///
/// A Swift caller also needs `import CxxStdlib`, or the importer refuses the
/// partial specialisation before it reaches this.
using QUICBootstrapBytes = std::span<const std::uint8_t>;

/// Accumulates a carrier's output until it holds one complete bootstrap line.
///
/// Feed each slice as it arrives and read the returned state; the decision is
/// terminal once it leaves `NeedMoreData`, so a caller acts on the first answer
/// and a remote that keeps talking cannot revise it.
class QUICBootstrapReader {
public:
    /// Where the read stands after the most recent input.
    enum class State : std::uint8_t {
        // No complete, well-formed line yet. Keep feeding.
        NeedMoreData = 0,
        // A line parsed with all three fields; `info()` is usable.
        Ready = 1,
        // No usable line will arrive; `refusalReason()` says why. Terminal —
        // the caller must fall back rather than dial QUIC unpinned.
        Refused = 2,
        // The bootstrap was pinned to a daemon that is gone
        // (`Session::instanceGoneSentinel`); `goneSocketName()` names it.
        // Terminal, and neither a retry nor the same pin's stream reaches it.
        InstanceGone = 3,
    };

    /// Accumulates a slice of the carrier's output.
    ///
    /// Scans only completed lines, so a partial tail is held rather than
    /// half-parsed. Once the state is `Ready` or `Refused` further input is
    /// ignored, which keeps a chatty remote from unsettling a decided read.
    ///
    /// - Parameter data: One slice of the bootstrap command's standard output.
    /// - Returns: The state after consuming the slice.
    State feed(QUICBootstrapBytes data);

    /// Reports that the carrier closed, so no further bytes will arrive.
    ///
    /// Parses a final unterminated line if one is buffered — a command that
    /// exits without a trailing newline still produced its answer. Anything
    /// else becomes a refusal, because an absent line is not a passable one.
    ///
    /// - Returns: The final state, `Ready` or `Refused`.
    State finish();

    /// Returns the state as it stands.
    ///
    /// - Returns: The current state.
    [[nodiscard]] State state() const { return m_state; }

    /// Returns the decoded dial information.
    ///
    /// Only meaningful once the state is `Ready`; the port is zero otherwise.
    ///
    /// - Returns: The bootstrap information.
    [[nodiscard]] const QUICBootstrapInfo& info() const { return m_info; }

    /// Returns why no usable line will arrive.
    ///
    /// - Returns: The refusal reason, empty unless the state is `Refused`.
    [[nodiscard]] const std::string& refusalReason() const { return m_refusalReason; }

    /// Returns the socket of the pinned daemon the bootstrap reported gone.
    ///
    /// - Returns: The socket name, empty unless the state is `InstanceGone`.
    [[nodiscard]] const std::string& goneSocketName() const { return m_goneSocketName; }

private:
    /// Judges one complete line.
    ///
    /// Leaves the state unchanged for a line that names no bootstrap field — a
    /// login banner is noise, not an answer — and refuses one that names a
    /// field but did not parse, so a half-supplied line is never skipped in the
    /// hope that a better one follows.
    ///
    /// - Parameter line: One line, without its terminator.
    void consumeLine(std::string_view line);

    State m_state = State::NeedMoreData;
    std::array<char, maxQUICBootstrapBufferSize> m_buffer {};
    std::size_t m_bufferSize = 0;
    QUICBootstrapInfo m_info;
    std::string m_refusalReason;
    std::string m_goneSocketName;
};

} // namespace Ubiquitty::Protocol::Sync

namespace Ubiquitty::SyncBridge {

// The bootstrap reader under a name Swift can say.
//
// `Protocol` is a Swift keyword in a member position and backticks do not
// rescue it: `Ubiquitty.\`Protocol\`.Sync.X` is still parsed as the metatype
// syntax and rejected with "cannot use 'Protocol' with non-protocol type". The
// namespace cannot be renamed — it is the wire protocol's, and every C++ caller
// spells it — so the two types a frontend needs get a second home here.
//
// Aliases, not wrappers. Same types, no conversion, nothing to keep in step.
// Only a Swift caller needs these; C++ keeps using the real names.
using QUICBootstrapReader = Ubiquitty::Protocol::Sync::QUICBootstrapReader;
using QUICBootstrapBytes = Ubiquitty::Protocol::Sync::QUICBootstrapBytes;
using QUICBootstrapInfo = Ubiquitty::Protocol::Sync::QUICBootstrapInfo;

/// The dial information, returned by value.
///
/// `QUICBootstrapReader::info()` returns a const reference, and Swift does not
/// import a reference-returning accessor on a non-trivial type: it cannot know
/// the referent outlives the call. Copying is inexpensive here — the struct is
/// a port and two short byte strings, read once per connection.
///
/// - Parameter reader: A reader whose state is Ready.
/// - Returns: A copy of the decoded bootstrap information.
inline QUICBootstrapInfo bootstrapInfo(const QUICBootstrapReader& reader)
{
    return reader.info();
}

/// Why no usable line will arrive, returned by value. As above.
///
/// - Parameter reader: A reader whose state is Refused.
/// - Returns: A copy of the refusal reason, empty in any other state.
inline std::string bootstrapRefusal(const QUICBootstrapReader& reader)
{
    return reader.refusalReason();
}

/// The socket of the pinned daemon the bootstrap reported gone, returned by
/// value. As above.
///
/// - Parameter reader: A reader whose state is InstanceGone.
/// - Returns: A copy of the socket name, empty in any other state.
inline std::string bootstrapGoneSocketName(const QUICBootstrapReader& reader)
{
    return reader.goneSocketName();
}

} // namespace Ubiquitty::SyncBridge
