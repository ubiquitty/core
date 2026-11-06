// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Sync/Records.h — what a host says about a pane beside its picture
// (Docs/sync.md §6.11, §7.7).
//
// Two records, each whole and each under a revision of its own:
//
// - `PaneFacts`: what a title, a tab and a listing are drawn from. The
//   program's title and its directory, the foreground chain, progress, the
//   indicator and tab colour and badge a program set, its user variables, how
//   the last command and the session ended, the pane's size and who set it,
//   when it last wrote and was last typed into, and the attention it raised
//   that nobody has seen.
// - `StatusRecords`: the status reports a program posted.
//
// A bound pane is sent its facts in every view state, ahead of its picture.
// Its status reports have one carrier per reader: a pane whose screen the
// reader receives has them in its screen, and any other bound pane is sent
// `StatusRecords`. A session tree's leaf carries the same two bodies, so one
// decoder reads a pane's facts wherever they come from. Every count and length is checked
// against its cap before an element is read, and a record a decoder refuses
// changes nothing its reader holds.
//
// A status record's bytes are the ones a checkpoint's activity block carries
// for the same record: one encoding of a status, in both places.
//

#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/OSC/TabStatus.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"
#include "PAL/RemoteTarget.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The most bytes of a process chain a record carries, with or without the
/// leaf's arguments.
inline constexpr std::size_t maxProcessChainBytes = 1024;
inline constexpr std::size_t maxProcessChainWithArgumentsBytes = 2048;
/// The most bytes of a program, shell or terminal name, and of a host name.
inline constexpr std::size_t maxProcessNameBytes = 255;
/// The most bytes of the home directory a title abbreviates against.
inline constexpr std::size_t maxHomeDirectoryBytes = 1024;
/// The most bytes of a status record's id, app, title and message, as the
/// OSC parser stores them.
inline constexpr std::size_t maxStatusIDBytes = 128;
inline constexpr std::size_t maxStatusAppBytes = 32;
inline constexpr std::size_t maxStatusTitleBytes = 192;
inline constexpr std::size_t maxStatusMessageBytes = 2048;
/// The fewest bytes a status record takes: four empty strings, a state, a
/// kind and a progress.
inline constexpr std::size_t minStatusRecordBytes = 7;

/// The program holding a pane's terminal.
struct ForegroundFacts {
    /// Its process id on the host; zero when none is known.
    std::uint32_t processID = 0;
    /// Its own name, without its ancestors.
    std::string name;
    /// Bumped each time another program takes the terminal.
    std::uint32_t generation = 0;

    friend bool operator==(const ForegroundFacts&, const ForegroundFacts&) = default;
};

/// How a pane's session ended, once it has.
struct SessionEndFacts {
    /// Nothing is running on the session.
    bool didEnd = false;
    /// The status below has been collected.
    bool hasStatus = false;
    /// A signal ended it, rather than its own exit.
    bool wasSignalled = false;
    /// The exit status, or the signal number when signalled.
    std::int32_t code = 0;

    friend bool operator==(const SessionEndFacts&, const SessionEndFacts&) = default;
};

/// A program's progress report (OSC 9;4).
struct ProgressFacts {
    /// One of `OSC::ProgressState`.
    std::uint8_t state = OSC::ProgressNone;
    /// The percentage, zero to a hundred.
    std::uint8_t value = 0;

    friend bool operator==(const ProgressFacts&, const ProgressFacts&) = default;
};

/// The indicator a program set (OSC 21337): a colour and the word for it.
struct IndicatorFacts {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    /// The label describing the colour; at most `OSC::tabStatusTextCapacity`.
    std::string label;

    friend bool operator==(const IndicatorFacts&, const IndicatorFacts&) = default;
};

/// A colour a program asked its tab to take.
struct TabColorFacts {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend bool operator==(const TabColorFacts&, const TabColorFacts&) = default;
};

/// One user variable a program set (the OSC 1337 user-variable escape).
struct UserVariableFact {
    std::string name;
    std::string value;

    friend bool operator==(const UserVariableFact&, const UserVariableFact&) = default;
};

/// The pixels of the geometry owner's cells and text area, which a program
/// asking for its size in pixels is answered from.
struct PanePixelGeometry {
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint16_t textAreaWidth = 0;
    std::uint16_t textAreaHeight = 0;

    friend bool operator==(const PanePixelGeometry&, const PanePixelGeometry&) = default;
};

/// When a pane last wrote and was last typed into, on the host's wall clock
/// in milliseconds since the Unix epoch; zero for never.
struct PaneActivityFacts {
    std::uint64_t lastOutputUnixMilliseconds = 0;
    std::uint64_t lastInputUnixMilliseconds = 0;

    friend bool operator==(const PaneActivityFacts&, const PaneActivityFacts&) = default;
};

/// The attention a program raised that no person has seen: a bell that rang,
/// and the newest notification posted.
struct AttentionFacts {
    bool hasUnseenBell = false;
    bool hasUnseenNotification = false;
    /// The newest unseen notification's words, each at most
    /// `OSC::attentionTextCapacity` bytes.
    std::string notificationTitle;
    std::string notificationBody;

    friend bool operator==(const AttentionFacts&, const AttentionFacts&) = default;
};

/// What a host says about one pane beside its picture.
struct PaneFacts {
    /// Moves when anything below but `activity` does.
    std::uint32_t revision = 0;

    /// What the program set as its title (OSC 0/2).
    std::string programTitle {};
    /// The pane's working directory as reported, not shortened.
    std::string currentDirectory {};
    /// The home directory `currentDirectory` is abbreviated against: the
    /// pane's user's, on the pane's host.
    std::string homeDirectory {};
    /// The document the program says it has open (OSC 6), as a URL.
    std::string currentFileURL {};
    /// The foreground chain as a title shows it, and the same with each
    /// program's arguments.
    std::string processChain {};
    std::string processChainWithArguments {};
    /// The session leader shell's name, without its login dash.
    std::string shellCommandName {};
    /// The terminal's name on its host ("ttys003").
    std::string terminalName {};
    /// The host the pane's session lives on.
    std::string hostName {};
    /// The pane's grid.
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    /// How the session ended, once it has.
    SessionEndFacts end {};

    /// The program holding the terminal.
    ForegroundFacts foreground {};
    /// Whether a shell, rather than a program it started, holds the terminal.
    bool isAtPrompt = false;
    /// Whether this pane has received an OSC 133 prompt marker.
    bool hasMarkedPrompt = false;
    /// The OSC 9;4 report.
    ProgressFacts progress {};
    /// The indicator a program set, absent when none is.
    std::optional<IndicatorFacts> indicator {};
    /// The colour a program asked its tab to take, absent when none is.
    std::optional<TabColorFacts> tabColor {};
    /// The badge a program set, as sent; empty for none.
    std::string badgeText {};
    /// The user variables a program set, whole, in slot order.
    std::vector<UserVariableFact> userVariables {};
    /// The last command's exit status, absent when none was reported.
    std::optional<std::int32_t> exitStatus {};

    /// The geometry owner's pixels.
    PanePixelGeometry pixels {};
    /// Who sets the pane's size.
    GeometryStatus geometry {};

    /// When the pane last wrote and was last typed into. A change to this
    /// alone does not move `revision`: it rides with whatever else moves.
    PaneActivityFacts activity {};
    /// When the pane's session was created, in seconds since the Unix epoch
    /// on the host's wall clock; zero when unknown.
    std::uint64_t createdUnixSeconds = 0;
    /// Whether the host adopted the session's terminal from another process
    /// rather than starting its program itself.
    bool isAdopted = false;
    /// What nobody has seen yet. It is here and not with the status reports
    /// because a pane's facts reach every reader, while a reader that
    /// receives the pane's screen has the reports with it.
    AttentionFacts attention {};

    friend bool operator==(const PaneFacts&, const PaneFacts&) = default;
};

/// The status a pane's programs posted, whole.
struct StatusRecords {
    /// Moves when anything below does.
    std::uint32_t revision = 0;
    /// The reports, least recently updated first, as the OSC parser keeps
    /// them; at most `OSC::programStatusLimit`.
    OSC::ProgramStatusStore store;

    friend bool operator==(const StatusRecords&, const StatusRecords&) = default;
};

/// What a host knows about a pane that its terminal does not hold: the
/// process facts it samples, the session's facts, and the clock.
struct PaneHostFacts {
    std::string homeDirectory;
    /// The foreground program's working directory as the host read it, which
    /// stands in while the program has reported none.
    std::string foregroundDirectory;
    std::string processChain;
    std::string processChainWithArguments;
    std::string shellCommandName;
    std::string terminalName;
    std::string hostName;
    /// Where the pane's foreground process leads: under a connection to
    /// another machine, `foregroundDirectory` is the client's own folder here.
    PAL::Process::RemoteKind foregroundLeadsTo = PAL::Process::RemoteKind::None;
    SessionEndFacts end;
    ForegroundFacts foreground;
    bool isAtPrompt = false;
    PanePixelGeometry pixels;
    PaneActivityFacts activity;
    std::uint64_t createdUnixSeconds = 0;
    bool isAdopted = false;

    friend bool operator==(const PaneHostFacts&, const PaneHostFacts&) = default;
};

/// Reads a pane's facts from its terminal and what its host knows.
///
/// The one reader of a terminal for this record: a title's inputs, progress,
/// the indicator, tab colour and badge, user variables in slot order, the
/// last command's exit status, and the attention nobody has seen. Every text is cut to its cap, so the result
/// always encodes. `revision` and `geometry` are left for the pane's source,
/// which owns both.
///
/// - Parameters:
///   - terminal: The pane's terminal on its host.
///   - host: What the host knows beside it.
/// - Returns: The facts, with `revision` zero.
[[nodiscard]] PaneFacts paneFactsOf(const Terminal& terminal, const PaneHostFacts& host);

/// Reads a pane's status records from its terminal.
///
/// - Parameter terminal: The pane's terminal on its host.
/// - Returns: The records, with `revision` zero.
[[nodiscard]] StatusRecords statusRecordsOf(const Terminal& terminal);

/// Cuts every text of a pane's facts to its cap and drops what a cap has no
/// room for, so the facts encode into one frame.
///
/// - Parameter facts: The facts as a host built them.
/// - Returns: The facts a frame can carry.
[[nodiscard]] PaneFacts boundedForWire(PaneFacts facts);

/// Drops the status records a frame cannot carry (one the OSC parser would
/// not have stored), so a frame always has room for the next record.
///
/// - Parameter records: The records as a host built them.
/// - Returns: The records a frame can carry.
[[nodiscard]] StatusRecords boundedForWire(StatusRecords records);

/// Reports whether two facts differ in anything a revision counts: all of
/// them but the revision itself and the activity stamps.
///
/// - Parameters:
///   - left: One record.
///   - right: Another.
/// - Returns: True when a client shown `left` must be sent `right`.
[[nodiscard]] bool differsBeyondActivity(const PaneFacts& left, const PaneFacts& right);

/// Appends the body of a `PaneFacts` frame.
///
/// - Parameters:
///   - output: The buffer to extend.
///   - facts: The facts, already bounded for the wire.
void encodePaneFacts(std::vector<std::uint8_t>& output, const PaneFacts& facts);

/// Reads a `PaneFacts` body.
///
/// - Parameter body: The frame's body.
/// - Returns: The facts, or nothing for a body cut short, carrying a trailing
///   byte, a count or length past its cap, an unknown enumerator, or the same
///   user variable twice.
[[nodiscard]] std::optional<PaneFacts> decodePaneFacts(std::span<const std::uint8_t> body);

/// Writes one status record as a checkpoint's activity block and a
/// `StatusRecords` frame both carry it.
///
/// - Parameters:
///   - writer: The writer to extend.
///   - record: The record.
void writeStatusRecord(WireWriter& writer, const OSC::ProgramStatus& record);

/// Reads one status record, refusing what the OSC parser would not have
/// stored.
///
/// - Parameter reader: The reader; failed when the record is refused.
/// - Returns: The record; meaningless once the reader has failed.
[[nodiscard]] OSC::ProgramStatus readStatusRecord(WireReader& reader);
/// Reads and validates the canonical record while borrowing its four text fields.
[[nodiscard]] OSC::ProgramStatusView readStatusRecordView(WireReader& reader);

/// One frame of a status set. A set too large for one frame travels as
/// several, back to back, each naming where its records begin.
struct StatusFragment {
    std::uint32_t revision = 0;
    /// How many records the whole set holds.
    std::uint32_t total = 0;
    /// The index in the set of this frame's first record.
    std::uint32_t first = 0;
    std::vector<OSC::ProgramStatus> records;

    friend bool operator==(const StatusFragment&, const StatusFragment&) = default;
};

/// Appends the body of one `StatusRecords` frame: as many records from
/// `first` on as one frame holds.
///
/// - Parameters:
///   - output: The buffer to extend.
///   - records: The whole set.
///   - first: The index of the first record to write.
/// - Returns: The index of the first record left unwritten; the set's size
///   when the frame finished it.
[[nodiscard]] std::size_t encodeStatusFragment(std::vector<std::uint8_t>& output, const StatusRecords& records, std::size_t first);

/// Reads one `StatusRecords` frame.
///
/// - Parameter body: The frame's body.
/// - Returns: The fragment, or nothing for a body cut short, carrying a
///   trailing byte, a count past its cap, records running past the set's
///   total, an unknown state, or the same id twice.
[[nodiscard]] std::optional<StatusFragment> decodeStatusFragment(std::span<const std::uint8_t> body);

/// A status set as its frames arrive. A set is whole once the frame that
/// reaches its total has been taken, and only then replaces the set held.
class StatusAssembly {
public:
    /// What taking a frame did.
    enum class Outcome : std::uint8_t {
        /// More frames of the set follow.
        Partial,
        /// The set is whole; `take` yields it.
        Whole,
        /// The frame does not continue the set in assembly and does not
        /// begin one: the stream is not a host's.
        Refused,
    };

    /// Takes one frame.
    ///
    /// - Parameter fragment: The frame, decoded.
    /// - Returns: What it did to the set in assembly.
    Outcome accept(StatusFragment fragment);

    /// Yields the whole set and empties the assembly.
    ///
    /// - Returns: The set.
    /// - Precondition: The last `accept` answered `Whole`.
    [[nodiscard]] StatusRecords take();

private:
    StatusRecords m_assembling;
    std::uint32_t m_total = 0;
    bool m_isOpen = false;
};

/// Writes the members of a terminal that a `PaneFacts` carries into a
/// client's replica: progress, the indicator, tab colour, badge, the open
/// document, the user variables, whole, and the unseen attention. A member that changes moves the
/// replica's own generation for it, which is what a frontend redraws on.
///
/// - Parameters:
///   - terminal: The client's replica of the pane.
///   - facts: The facts a host sent.
void applyRecorded(Terminal& terminal, const PaneFacts& facts);

} // namespace Ubiquitty::Protocol::Sync
