// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The host's event stream: one typed record per thing a person or a tool may
// want to wake on — a session starting or ending, a pane taking focus, a
// program's status changing, a command finishing, a bell, a title, a consent
// question and its answer.
//
// Every record carries a cursor, a number the log assigns in order and never
// repeats. A subscriber that remembers the last cursor it saw resumes from it
// and receives exactly the records after it, so a client that reconnects
// misses nothing the log still holds, and learns how many records it did miss
// when the log has already dropped them. The log is bounded: it keeps the most
// recent `capacity` records, and each record's text is cut to a fixed length,
// so no program can grow the host's memory by printing.

#include "Core/Codec/JSON.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// What happened. The raw values are the wire's and never renumbered.
enum class EventKind : std::uint8_t {
    /// The log dropped records the subscriber had not seen; `count` says how
    /// many, and `cursor` is the last one dropped. A count of zero marks where
    /// a stream that skipped the backlog begins. Never stored: a subscription
    /// synthesizes it.
    Gap = 0,
    /// A session started. It has no label yet: a label is given to a session
    /// that exists.
    SessionCreated = 1,
    /// A session ended; `text` says why.
    SessionEnded = 2,
    /// A client's view of a session took focus.
    PaneFocused = 3,
    /// A program's status record changed; `state` and `text` (its message)
    /// are the new record's, `detail` is the record's ID path.
    ProgramStatus = 4,
    /// A command finished; `exitStatus` is its status, `text` its command
    /// line, `detail` its directory.
    CommandFinished = 5,
    /// The session rang its bell.
    Bell = 6,
    /// The session's title changed; `text` is the new title.
    TitleChanged = 7,
    /// The session's foreground moved; `detail` is the foreground process and
    /// `isAtPrompt` says whether it is the shell.
    ForegroundChanged = 8,
    /// An agent asked to reach a session and a person was asked; `text` is the
    /// agent's label, `detail` the action.
    ConsentRequested = 9,
    /// The person allowed it.
    ConsentGranted = 10,
    /// The person refused, or the question timed out; `detail` says which.
    ConsentDenied = 11,
    /// The shell started running a command line (its OSC 133 `C` mark), so
    /// the next CommandFinished is that command's.
    CommandStarted = 12,
    /// An agent read screen content; `text` is its claimed label, `detail`
    /// its verified local process, and `count` the Unix time in milliseconds.
    ScreenRead = 13,
};

/// The largest raw value an `EventKind` takes.
inline constexpr std::uint8_t lastEventKind = 13;

/// The state an OSC 7501 record reports, as the event stream names it.
/// The raw values match `OSC::ProgramState`.
enum class EventProgramState : std::uint8_t {
    None = 0,
    Idle = 1,
    Working = 2,
    Done = 3,
    Blocked = 4,
    Error = 5,
};

/// The longest `text` or `detail` an event keeps, in bytes.
inline constexpr std::size_t maxEventTextLength = 1024;

/// The sentinel `exitStatus` for a command whose shell reported none.
inline constexpr std::int32_t noExitStatus = INT32_MIN;

/// One record of the event stream.
struct Event {
    /// The log's number for this record; strictly increasing, starting at 1.
    std::uint64_t cursor = 0;
    EventKind kind = EventKind::Gap;
    /// The session the event concerns; zero when none does.
    std::uint32_t sessionID = 0;
    /// The pane showing that session; zero when the host has not placed it.
    std::uint32_t paneID = 0;
    /// The command's exit status, or ``noExitStatus``.
    std::int32_t exitStatus = noExitStatus;
    EventProgramState state = EventProgramState::None;
    /// Whether the foreground is the shell; meaningful for ``EventKind/ForegroundChanged``.
    bool isAtPrompt = false;
    /// For a gap, how many records were dropped; for ScreenRead, Unix milliseconds.
    std::uint64_t count = 0;
    std::string text;
    std::string detail;

    friend bool operator==(const Event&, const Event&) = default;
};

/// Returns the word the stream writes a kind with, such as `command-finished`.
///
/// - Parameter kind: The kind to name.
/// - Returns: A lowercase, hyphenated word, never empty.
[[nodiscard]] std::string_view eventKindName(EventKind kind);

/// Returns the kind a raw wire value names.
///
/// Inline so the wire codec reads a kind without linking the log.
///
/// - Parameter raw: A byte from the wire.
/// - Returns: The kind, or nothing for a value no kind has.
[[nodiscard]] constexpr std::optional<EventKind> eventKindFromRaw(std::uint8_t raw)
{
    if (raw > lastEventKind)
        return std::nullopt;
    return static_cast<EventKind>(raw);
}

/// Returns the word a program state is written with: `idle`, `working`,
/// `done`, `blocked`, `error`, or `none`.
///
/// - Parameter state: The state to name.
/// - Returns: The word.
[[nodiscard]] std::string_view eventProgramStateName(EventProgramState state);

/// Writes an event as the JSON object `ubq events --json` prints.
///
/// Identities are written in their typed spelling, `session:3`; fields that
/// say nothing for the kind are omitted.
///
/// - Parameter event: The event to write.
/// - Returns: A JSON object.
[[nodiscard]] Codec::JSON::Value eventJSON(const Event& event);

/// The bounded, ordered log of events a host keeps for its subscribers.
class EventLog {
public:
    /// Exact ring placement and cursor high-water, independent of subscribers.
    struct Continuation {
        std::vector<Event> ring;
        std::size_t head = 0;
        std::size_t count = 0;
        std::uint64_t nextCursor = 1;
    };

    /// Copies the fixed-capacity ring without advancing any subscriber cursor.
    /// - Returns: All physical slots and original counters.
    [[nodiscard]] Continuation continuation() const { return { m_ring, m_head, m_count, m_nextCursor }; }

    /// Validates exact ring state without allocating or changing its owners.
    /// - Parameters:
    ///   - state: The predecessor's physical ring and counters.
    ///   - capacity: The independently admitted receiver capacity.
    /// - Returns: Whether every slot and counter is consistent.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state, std::size_t capacity);

    /// Restores a fresh log only after exact capacity and cursor validation.
    /// - Parameter state: Owned predecessor ring, consumed only on success.
    /// - Returns: Whether the receiver's independent capacity and all records match.
    [[nodiscard]] bool restoreContinuation(Continuation&& state);

    /// The most records a log keeps by default.
    static constexpr std::size_t defaultCapacity = 4096;

    /// Creates an empty log.
    ///
    /// - Parameter capacity: How many records it keeps; at least one.
    explicit EventLog(std::size_t capacity = defaultCapacity);

    /// Appends an event, assigning its cursor and cutting its text to
    /// ``maxEventTextLength``.
    ///
    /// - Parameter event: The event; its `cursor` is ignored.
    /// - Returns: The stored record, valid until the next append.
    const Event& append(Event event);

    /// What a subscriber resuming from a cursor is owed.
    struct Backlog {
        /// Records the log dropped before the subscriber saw them; zero when
        /// nothing was lost.
        std::uint64_t dropped = 0;
        /// The cursor just before the first record still held, which a gap
        /// event reports so the subscriber can resume after it.
        std::uint64_t gapCursor = 0;
        /// The records after the cursor, oldest first.
        std::vector<Event> events;
    };

    /// Returns a bounded run after a cursor without copying the rest of the log.
    ///
    /// - Parameter cursor: The last cursor the subscriber saw; zero for none.
    /// - Parameter maximum: The most records copied, independently of retention.
    /// - Returns: The records it is owed and how many it can no longer have.
    [[nodiscard]] Backlog since(std::uint64_t cursor, std::size_t maximum = std::numeric_limits<std::size_t>::max()) const;

    /// Returns the cursor of the newest record, or zero when the log has
    /// never held one.
    [[nodiscard]] std::uint64_t latestCursor() const { return m_nextCursor - 1; }

    /// Returns how many records the log holds.
    [[nodiscard]] std::size_t count() const { return m_count; }

private:
    [[nodiscard]] const Event& at(std::size_t ordinal) const;

    std::vector<Event> m_ring;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    std::uint64_t m_nextCursor = 1;
};

/// Holds one more record of a host's event stream for a reader that has not
/// taken the ones before it.
///
/// A reader past its bound gives up its oldest records, and says so as the
/// log does: they are folded into a gap at the front whose `count` is how many
/// records it stands for and whose `cursor` is the last of them. A gap the
/// host sent is folded in with its own count, so a loss the log reported is
/// never the record a full reader forgets.
///
/// - Parameters:
///   - held: The records not yet taken, oldest first.
///   - event: The record that arrived.
///   - capacity: The most records to hold; at least two.
void holdEvent(std::vector<Event>& held, Event event, std::size_t capacity);

} // namespace Ubiquitty::Session
