// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Input.h — a pane's input on the host: what each client wrote, in its
// own order, written once (Docs/sync.md §6.7).
//
// The invariants:
//
// - A client numbers its input to a pane from 1. The host writes `s` only if
//   it is above everything that client has had written or queued there, so a
//   client that resends after a drop never has an event written twice.
// - What was written last is kept per client instance for the pane's
//   lifetime, not a connection's: a second connection from the same instance
//   continues the same numbering. The ledger keeps the 64 instances written
//   most recently (§7.9). An instance it evicted reads absent, never 0: a 0
//   would tell a client that nothing it sent was written, and it would send
//   everything again.
// - Input is written in arrival order. A write the program does not take now
//   stays at the head of the queue and is written once when it does, never
//   dropped and never reordered.
// - The queue is bounded per client instance. An instance past its own share
//   is refused and its sender closed; another instance's input never counts
//   against it, so one sender's flood closes nobody else.
// - A write `s` settles once it was written at least `E` before a read of the
//   program's output began and found nothing to read: by then the program has
//   had `E` to echo it, and the read took everything it wrote. The read's
//   start, never its end, is the time compared, so a read that took long
//   cannot settle a write it began before. Settled never passes written.
//

#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The client instances a pane's ledger remembers (Docs/sync.md §7.9).
inline constexpr std::size_t maxLastWrittenEntries = 64;

/// The unsettled writes a ledger keeps per instance. Past it the two oldest
/// merge into one with the later number and the later time, which settles
/// later and never earlier.
inline constexpr std::size_t maxUnsettledWrites = 64;

/// The paste a client may have sent beyond what the host reported written:
/// the window a `Welcome` states. A client keeps a paste inside it, so a
/// program that reads nothing holds this much of one and no more.
/// The default window holds 256 KiB while the program catches up.
inline constexpr std::size_t maxPendingInputBytes = std::size_t { 256 } << 10;

/// The most one client instance's input a pane holds unwritten: its share.
/// It is the window, and as much again for what is typed around a paste. An
/// event past it is refused and its sender closed; another instance's input
/// never counts against it. A sender is never held back by not being read: an
/// interrupt behind unread input would never be seen.
inline constexpr std::size_t maxHeldInputBytes = 2 * maxPendingInputBytes;

/// The most events one instance's share holds unwritten, whatever they weigh.
/// The byte bound prices an event by what it writes, and the smallest event
/// writes one byte, so without this a sender could hold the pane behind half a
/// million entries that cost it almost nothing and cost the host an entry
/// each. An event past it is refused like one past the byte bound. Signals are
/// not counted: an interrupt is taken past every bound, within its own.
inline constexpr std::size_t maxHeldInputEvents = 4096;

/// The instances whose input a pane holds at once: as many as its ledger
/// remembers, which is every client the host attaches. The pane's bound is
/// the sum of their shares.
inline constexpr std::size_t maxInputInstances = maxLastWrittenEntries;

/// The most one connection holds unwritten across all of its panes: two
/// shares, so a person pasting into two busy panes at once fits, and the host's
/// whole input is bounded by its connections rather than by its connections
/// times its panes. A sender past it is closed like one past its share.
inline constexpr std::size_t maxConnectionHeldInputBytes = 2 * maxHeldInputBytes;

/// The signals a pane holds unwritten, in bytes. A signal is taken past the
/// pane's bound, so an interrupt always gets through; one past this is
/// refused, as nothing but a sender ignoring a program that is not taking
/// them holds that many.
inline constexpr std::size_t maxPendingSignalBytes = std::size_t { 4 } << 10;

/// The room past a share for what the host adds to it rather than its sender:
/// the end of a paste it gave up. The pane's own focus report, which a newer
/// one replaces while it waits, fits in the same room.
inline constexpr std::size_t maxInputShareSlack = 64;

/// The most a pane holds unwritten at all: every instance's share with its
/// slack, and the signals' own bound.
inline constexpr std::size_t maxPaneHeldInputBytes = maxInputInstances * (maxHeldInputBytes + maxInputShareSlack) + maxPendingSignalBytes;

/// The runs of discarded input a pane remembers for one client instance, to
/// tell it of each. Past it the oldest two are told as one run, which names
/// what was written between them as discarded too.
inline constexpr std::size_t maxInputDiscardRecords = 8;

/// Bytes a client sends as they are: an agent's input, written verbatim.
struct RawInput {
    std::vector<std::uint8_t> bytes;
};

/// Keys a client typed, encoded when they are written.
struct KeyInput {
    std::vector<KeyEvent> events;
};

/// Pointer events a client routed to the program, encoded when they are
/// written.
struct PointerInput {
    std::vector<PointerEvent> events;
};

/// A change of whether any driver of the pane holds the focus: a focus report
/// under DECSET 1004 when it is written.
struct FocusInput {
    Ubiquitty::Modes::FocusState focus = Ubiquitty::Modes::FocusState::Unfocused;
};

/// How a piece of a paste ends it.
enum class PasteEnd : std::uint8_t {
    /// More of the paste follows.
    More,
    /// The paste is whole.
    Final,
    /// The client gave the paste up: nothing more of it is written, and an
    /// open bracket is closed.
    Aborted,
};

/// A piece of a paste: one event, numbered once, whose text arrives in
/// pieces on the bulk lane (Docs/sync.md §6.7). The host writes it under
/// bracketed paste and the paste normaliser when it writes it.
struct PasteInput {
    /// UTF-8, whole scalars.
    std::vector<std::uint8_t> text;
    /// The paste's bytes before this piece.
    std::uint64_t offset = 0;
    PasteEnd end = PasteEnd::More;
};

/// One event of input, as a client meant it.
using InputIntent = std::variant<RawInput, KeyInput, PointerInput, FocusInput, PasteInput>;

/// What an input event costs the queue.
///
/// - Parameter intent: The event.
/// - Returns: Its bytes.
[[nodiscard]] std::size_t intentBytes(const InputIntent& intent);

/// The byte a key chord is in a terminal's legacy encoding when that byte is
/// a control character a line discipline can act on: Ctrl with a letter, or
/// Ctrl-\. The letter is the layout's when Latin, else the standard layout's
/// at the key's position, as the key encoder reads it.
///
/// - Parameter event: The key.
/// - Returns: The byte, or `nullopt` for a release, another chord, or a key
///   held with any modifier but Ctrl and the locks.
[[nodiscard]] std::optional<std::uint8_t> legacyControlCharacterOf(const KeyEvent& event);

/// How far one client instance's input was settled.
struct SettledMark {
    ID128 clientInstance;
    std::uint64_t settledSeq = 0;
};

/// A run of one client instance's events a pane dropped unwritten: a signal
/// character discarded them with the input the terminal held unread.
struct InputDiscardRecord {
    /// The instance's newest event written or dropped before the run.
    std::uint64_t inputWritten = 0;
    /// The newest event of the run.
    std::uint64_t throughSeq = 0;
    /// The record's place among every discard the ledger noted, ascending
    /// from 1.
    std::uint64_t serial = 0;
};

/// Whether a numbered event is new to a pane.
enum class InputAdmission : std::uint8_t {
    /// Nothing at or above it was written for its instance.
    Write,
    /// It was written already, or it is queued.
    Duplicate,
};

/// What was written last to one pane, per client instance, for the pane's
/// lifetime.
class InputLedger {
public:
    /// Owns the bounded replay and settlement ledger in its physical order.
    struct Continuation {
        /// Retains one unsettled event and its original host timestamp.
        struct Write {
            std::uint64_t inputSeq = 0;
            std::uint64_t writtenAt = 0;
        };
        /// Retains one client instance's ordered receipts and eviction rank.
        struct Entry {
            ID128 clientInstance;
            std::uint64_t inputSeq = 0;
            std::uint64_t writtenAt = 0;
            std::uint64_t settledSeq = 0;
            std::uint64_t order = 0;
            std::array<Write, maxUnsettledWrites> unsettled {};
            std::array<InputDiscardRecord, maxInputDiscardRecords> discards {};
            std::uint8_t unsettledCount = 0;
            std::uint8_t discardCount = 0;
        };
        std::array<Entry, maxLastWrittenEntries> entries {};
        std::array<ID128, maxLastWrittenEntries> recentWriters {};
        std::uint64_t writes = 0;
        std::uint64_t discardSerial = 0;
        std::uint8_t capacity = 0;
        std::uint8_t entryCount = 0;
        std::uint8_t recentWriterCount = 0;
    };

    /// Copies exact counters, receipt order and absolute settlement times.
    /// - Returns: Owned state, or nothing if this ledger exceeds the continuation bounds.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Replaces a ledger only after validating its complete continuation.
    ///
    /// The destination capacity must match. Candidate vectors retain the ledger's
    /// bounded STL allocation contract; this is not a fallible allocation API.
    /// - Parameter state: Field-wise decoded state, including the original clock values.
    /// - Returns: False without mutation for invalid state or a different capacity.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    /// Creates an empty ledger.
    ///
    /// - Parameter capacity: The instances it remembers, the least recently
    ///   written forgotten first.
    explicit InputLedger(std::size_t capacity = maxLastWrittenEntries);

    /// Whether an event is above everything written for its instance.
    ///
    /// - Parameters:
    ///   - clientInstance: The client that numbered it.
    ///   - inputSeq: Its number.
    /// - Returns: `Write` when it is.
    [[nodiscard]] InputAdmission admit(ID128 clientInstance, std::uint64_t inputSeq) const;

    /// Records an event written.
    ///
    /// - Parameters:
    ///   - clientInstance: The client that numbered it.
    ///   - inputSeq: Its number.
    ///   - now: When it was written.
    void noteWritten(ID128 clientInstance, std::uint64_t inputSeq, std::uint64_t now);

    /// Records an instance's events up to a number as dropped unwritten.
    /// They are consumed: nothing at or below the number is admitted again,
    /// and they settle as a write made now would, so the instance's counts
    /// move past them. The run is remembered for the instance to be told of.
    ///
    /// - Parameters:
    ///   - clientInstance: The client that numbered them.
    ///   - throughSeq: The newest of them; at or below what the ledger holds
    ///     for the instance, nothing is recorded.
    ///   - now: When they were dropped.
    void noteDiscarded(ID128 clientInstance, std::uint64_t throughSeq, std::uint64_t now);

    /// The runs of an instance's events dropped unwritten, oldest first.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The runs remembered, none for an instance not remembered.
    [[nodiscard]] std::span<const InputDiscardRecord> discards(ID128 clientInstance) const;

    /// The serial of the newest discard noted for any instance, 0 before the
    /// first.
    [[nodiscard]] std::uint64_t discardSerial() const { return m_discards; }

    /// The newest event written for an instance, or dropped unwritten.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: Its number, or `nullopt` when nothing was written for it or
    ///   it was forgotten; never 0 for either.
    [[nodiscard]] std::optional<std::uint64_t> lastWritten(ID128 clientInstance) const;

    /// When the newest event of an instance was written.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The time, or `nullopt` when nothing is remembered for it.
    [[nodiscard]] std::optional<std::uint64_t> lastWrittenAt(ID128 clientInstance) const;

    /// Settles every write made at least `echoSettle` before a read of the
    /// program's output that began at `readBegan` and found nothing.
    ///
    /// - Parameters:
    ///   - readBegan: When the read that found nothing began.
    ///   - echoSettleMicroseconds: The echo timer `E`.
    void noteReadWouldBlock(std::uint64_t readBegan, std::uint64_t echoSettleMicroseconds);

    /// The newest event of an instance known settled.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: Its number, 0 when none is.
    [[nodiscard]] std::uint64_t settledThrough(ID128 clientInstance) const;

    /// When the oldest unsettled write may first settle.
    ///
    /// - Parameter echoSettleMicroseconds: The echo timer `E`.
    /// - Returns: Its write time plus `E`, saturated at `UINT64_MAX`, or
    ///   `nullopt` when every write is settled.
    [[nodiscard]] std::optional<std::uint64_t> nextSettleMicroseconds(std::uint64_t echoSettleMicroseconds) const;

    /// Takes the instances written since the last call: whose input reached
    /// the program while the frame being published was drawn.
    ///
    /// - Returns: The instances, each once.
    [[nodiscard]] std::vector<ID128> takeRecentWriters() { return std::exchange(m_recentWriters, {}); }

    /// Copies each instance's settled mark, reusing `marks`' storage.
    ///
    /// - Parameter marks: Replaced with one mark per instance remembered.
    void copySettled(std::vector<SettledMark>& marks) const;

private:
    /// A write not yet settled.
    struct Unsettled {
        std::uint64_t inputSeq = 0;
        std::uint64_t writtenAt = 0;
    };

    /// One instance's newest write.
    struct Entry {
        ID128 clientInstance;
        std::uint64_t inputSeq = 0;
        std::uint64_t writtenAt = 0;
        std::uint64_t settledSeq = 0;
        /// Writes not yet settled, ascending.
        std::vector<Unsettled> unsettled;
        /// The ledger's write count when it was last written: the least is
        /// the least recently written.
        std::uint64_t order = 0;
        /// Runs dropped unwritten, oldest first, at most
        /// `maxInputDiscardRecords`.
        std::vector<InputDiscardRecord> discards;
    };

    [[nodiscard]] const Entry* find(ID128 clientInstance) const;

    /// The entry of an instance, made when it has none, the least recently
    /// written forgotten for it.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The entry.
    Entry& entry(ID128 clientInstance);

    std::vector<Entry> m_entries;
    /// The instances written since `takeRecentWriters`, at most `m_capacity`.
    std::vector<ID128> m_recentWriters;
    std::size_t m_capacity;
    std::uint64_t m_writes = 0;
    std::uint64_t m_discards = 0;
};

/// Whether a write reached the program.
enum class WriteStatus : std::uint8_t {
    /// It was written whole.
    Written,
    /// The program is not reading now; nothing of it was written.
    Refused,
    /// This event's attachment has lost authority; other writers remain live.
    Withdrawn,
    /// The program is gone; nothing more will be written.
    Closed,
};

/// What the program's terminal does with an event beyond handing the program
/// its bytes.
enum class SignalEffect : std::uint8_t {
    /// Nothing: the event is data.
    None,
    /// It signals the foreground group, and the input waiting unread stays.
    Signals,
    /// It signals the foreground group and discards the input waiting unread.
    SignalsAndDiscards,
};

/// Writes a pane's input to its program. The owner of the pane supplies it.
class InputWriter {
public:
    virtual ~InputWriter() = default;

    /// What the program's terminal would do with an event written now, under
    /// its modes as they stand.
    ///
    /// - Parameter intent: The event.
    /// - Returns: Its effect; `None` from a writer with no terminal to ask.
    [[nodiscard]] virtual SignalEffect signalEffect(const InputIntent& intent)
    {
        (void)intent;
        return SignalEffect::None;
    }

    /// Writes an event that signals. The brackets of the pastes it ended are
    /// closed first, in the same write, so the program is not left in paste
    /// mode and reads the end before the key.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose event it is.
    ///   - intent: The event.
    ///   - endedPastes: The clients whose pastes in progress the event ended.
    /// - Returns: Whether it was written, refused or the program is gone.
    [[nodiscard]] virtual WriteStatus writeSignal(ID128 clientInstance, const InputIntent& intent, std::span<const ID128> endedPastes)
    {
        (void)endedPastes;
        return write(clientInstance, intent);
    }

    /// Writes one event.
    ///
    /// - Parameters:
    ///   - clientInstance: The client whose event it is.
    ///   - intent: The event.
    /// - Returns: Whether it was written, refused or the program is gone.
    [[nodiscard]] virtual WriteStatus write(ID128 clientInstance, const InputIntent& intent) = 0;

    /// Writes an event under the attachment that admitted it.
    /// - Parameters:
    ///   - authority: Server-issued attachment identity; zero for system input.
    ///   - clientInstance: The replay-numbering identity.
    ///   - intent: The event.
    /// - Returns: Admission, backpressure, withdrawal, or closure.
    [[nodiscard]] virtual WriteStatus writeOwned(ID128 authority, ID128 clientInstance, const InputIntent& intent)
    {
        (void)authority;
        return write(clientInstance, intent);
    }

    /// Writes a signaling event under its original attachment.
    /// - Parameters:
    ///   - authority: The admitting attachment.
    ///   - clientInstance: The replay identity.
    ///   - intent: The signaling event.
    ///   - endedPastes: Paste identities ended by the signal.
    /// - Returns: The write outcome.
    [[nodiscard]] virtual WriteStatus writeSignalOwned(ID128 authority, ID128 clientInstance, const InputIntent& intent, std::span<const ID128> endedPastes)
    {
        (void)authority;
        return writeSignal(clientInstance, intent, endedPastes);
    }

    /// Removes the attachment's undelivered sink bytes and closes its paste.
    /// - Parameter authority: The exact retired attachment, never a client identity.
    virtual void withdraw(ID128 authority) { (void)authority; }
};

/// What the queue did with an event.
enum class EnqueueOutcome : std::uint8_t {
    /// It waits to be written.
    Queued,
    /// It was written or queued already; it is dropped.
    Duplicate,
    /// The queue is past its bound, or the program is gone; it is dropped.
    Refused,
    /// A piece of a paste that skips past what was taken of it: no client
    /// sends it, and its connection ends.
    Malformed,
};

/// A pane's input waiting to be written, in arrival order.
class PaneInputQueue {
public:
    /// Queues an event that is new to the pane.
    ///
    /// - Parameters:
    ///   - ledger: What was written to the pane.
    ///   - clientInstance: The client that numbered it.
    ///   - inputSeq: Its number.
    ///   - intent: The event.
    ///   - authority: The admitting server attachment, separate from replay identity.
    ///   - room: The most its sender may add across all of its panes.
    /// - Returns: Whether it was queued.
    [[nodiscard]] EnqueueOutcome enqueue(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, ID128 authority = {}, std::size_t room = SIZE_MAX);

    /// Queues an event the program's terminal answers with a signal, ahead of
    /// everything that waits: it is written first, as the terminal takes a
    /// signal character past the input it holds unread. Where the terminal
    /// discards that input (`SignalsAndDiscards`), what waits here unwritten
    /// is discarded with it, whoever sent it: each instance's dropped events
    /// are recorded in the ledger as consumed, and every paste in progress is
    /// ended, its bracket closed in the event's own write. Where it keeps the
    /// input (`Signals`), nothing is dropped, and the event is counted
    /// written once everything its instance numbered before it is.
    ///
    /// The pane's bound does not refuse it: an interrupt always gets through.
    /// Signals themselves are held to `maxPendingSignalBytes`.
    ///
    /// - Parameters:
    ///   - ledger: What was written to the pane.
    ///   - clientInstance: The client that numbered it.
    ///   - inputSeq: Its number.
    ///   - intent: The event.
    ///   - effect: What the terminal does with it; not `None`.
    ///   - now: The time.
    ///   - authority: The admitting server attachment.
    /// - Returns: Whether it was queued.
    [[nodiscard]] EnqueueOutcome enqueueSignal(InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, SignalEffect effect, std::uint64_t now, ID128 authority = {});

    /// Gives up an instance's paste in progress, as its `PasteAbort` asks:
    /// the pieces not yet written are dropped, and an end goes where the
    /// first of them stood, so the open bracket closes before anything the
    /// client typed after. A paste of another number is left as it is.
    ///
    /// - Parameters:
    ///   - ledger: What was written to the pane.
    ///   - clientInstance: The client.
    ///   - inputSeq: The paste's number.
    ///   - authority: The attachment authorized to cancel this paste.
    /// - Returns: Whether a paste was given up.
    bool abortPaste(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, ID128 authority = {});

    /// Retires only queued events admitted by one attachment.
    /// Already admitted sink prefixes remain written. System events and
    /// neighboring attachments remain queued in their original order.
    /// - Parameters:
    ///   - ledger: Replay and delivery accounting.
    ///   - authority: The retired server attachment; zero is not withdrawable.
    void withdraw(InputLedger& ledger, ID128 authority);

    /// Gives up whatever paste an instance has in progress: its attachment
    /// is gone, and no piece of it will come. The bracket it opened is still
    /// closed, so the program is not left in paste mode.
    ///
    /// - Parameter clientInstance: The client.
    void abandonPaste(ID128 clientInstance);

    /// Queues an event no client numbered, behind everything queued: a
    /// focus report, which no client resends and the ledger never counts. A
    /// report that waits unwritten is replaced where it stands, so the program
    /// is told the pane's focus as it is, and a client that turns its focus
    /// over and over holds one entry, not one for each turn.
    ///
    /// - Parameter intent: The event.
    /// - Returns: Whether it was queued.
    [[nodiscard]] EnqueueOutcome enqueueUnnumbered(InputIntent intent);

    /// Writes what the program takes, from the head, recording each write.
    /// A refused write stays at the head; a closed program drops the rest.
    ///
    /// - Parameters:
    ///   - writer: Where the input goes.
    ///   - ledger: Where each write is recorded.
    ///   - now: The time.
    /// - Returns: Whether anything was written.
    bool drain(InputWriter& writer, InputLedger& ledger, std::uint64_t now);

    /// The bytes held unwritten.
    [[nodiscard]] std::size_t heldBytes() const { return m_bytes; }

    /// The bytes one instance's share holds unwritten, signals aside.
    ///
    /// - Parameter clientInstance: The client.
    [[nodiscard]] std::size_t heldBytes(ID128 clientInstance) const;

    /// How much of an instance's unfinished paste was written.
    struct PasteWritten {
        std::uint64_t inputSeq = 0;
        std::uint64_t bytesWritten = 0;

        friend bool operator==(const PasteWritten&, const PasteWritten&) = default;
    };

    /// How much of an instance's paste in progress the program was written,
    /// which its `PasteReceipt` reports.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The paste and its bytes written, or `nullopt` when the
    ///   instance has no paste in progress.
    [[nodiscard]] std::optional<PasteWritten> pasteWritten(ID128 clientInstance) const;

    /// The bytes taken of an instance's paste in progress, or `nullopt` when
    /// it has none.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - inputSeq: The paste's number.
    [[nodiscard]] std::optional<std::uint64_t> pasteTaken(ID128 clientInstance, std::uint64_t inputSeq) const;

    /// Whether nothing waits.
    [[nodiscard]] bool isEmpty() const { return m_pending.empty(); }

    /// How many typed events have been written: raw bytes, keys, and each
    /// paste once at its end. Pointer events and focus reports are not typing
    /// and are not counted, nor is anything still queued.
    [[nodiscard]] std::uint64_t typedWrites() const { return m_typedWrites; }

    /// One event waiting to be written.
    struct Pending {
        ID128 clientInstance;
        ID128 authority;
        std::uint64_t inputSeq = 0;
        InputIntent intent;
        /// Whether the event signals: it stands at the head and is written
        /// with `InputWriter::writeSignal`.
        bool isSignal = false;
        /// The clients whose pastes in progress a signal ended.
        std::vector<ID128> endedPastes;
    };

    /// A signal written ahead of events its instance numbered before it, not
    /// yet counted written.
    struct Passed {
        ID128 clientInstance;
        std::uint64_t inputSeq = 0;
        std::uint64_t writtenAt = 0;
    };

    /// An instance's paste, from its first piece taken until its end is
    /// written.
    struct PasteProgress {
        ID128 clientInstance;
        ID128 authority;
        std::uint64_t inputSeq = 0;
        std::uint64_t bytesTaken = 0;
        /// The bytes of it written to the program.
        std::uint64_t bytesWritten = 0;
        /// Whether its end, final or aborted, was taken.
        bool hasEnded = false;
    };

    /// The most entries a continuation carries: every ordinary event the
    /// pane's shares may hold and the end of a paste each gave up, a focus
    /// report for every byte of the slack, and a signal for every byte of the
    /// signal bound, which is the most signals that bound admits.
    static constexpr std::size_t maxContinuationIntents = maxInputInstances * (maxHeldInputEvents + 1) + maxInputShareSlack + maxPendingSignalBytes;

    /// Owns unsubmitted intent order, paste progress and overtaking receipts.
    /// Accepted stream bytes and encoder delimiter progress are separate owners.
    struct Continuation {
        std::vector<Pending> pending;
        std::vector<PasteProgress> pastes;
        std::vector<Passed> passed;
        std::uint64_t heldBytes = 0;
        std::uint64_t typedWrites = 0;
        bool isProgramGone = false;
    };

    /// Copies bounded unsubmitted state without consuming or encoding input.
    /// - Parameter ledger: The exact ledger from the same frozen pane.
    /// - Returns: Independent owners, or refusal without changing this queue.
    [[nodiscard]] std::optional<Continuation> continuation(const InputLedger& ledger) const;

    /// Validates lengths, paste partitions and replay identity before adoption.
    /// - Parameters:
    ///   - state: Bounded continuation whose vector sizes are checked first.
    ///   - ledger: The exact restored input ledger.
    /// - Returns: Whether the state can continue without replaying settled input.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state, const InputLedger& ledger);

    /// Consumes a complete continuation into a pristine queue without writing.
    /// Candidate container allocation retains the queue's bounded STL contract.
    /// - Parameters:
    ///   - state: Owned intents, consumed only after complete validation.
    ///   - ledger: The exact restored input ledger, never mutated by import.
    /// - Returns: False without changing either owner when admission refuses.
    [[nodiscard]] bool restoreContinuation(Continuation&& state, const InputLedger& ledger);

private:
    /// Queues a piece of a paste: what overlaps what was taken is trimmed, a
    /// gap is malformed.
    ///
    /// - Parameters:
    ///   - ledger: What was written to the pane.
    ///   - clientInstance: The client.
    ///   - inputSeq: The paste's number.
    ///   - piece: The piece.
    ///   - authority: The server attachment that admitted the piece.
    ///   - room: The most its sender may add across all of its panes.
    /// - Returns: What became of it.
    [[nodiscard]] EnqueueOutcome enqueuePaste(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, PasteInput piece, ID128 authority, std::size_t room);

    /// Whether an entry waits behind its instance's unfinished paste.
    ///
    /// - Parameter pending: The entry.
    [[nodiscard]] bool isHeldBehindPaste(const Pending& pending) const;

    /// Ends a paste in progress: drops its unwritten pieces and queues its
    /// end where the first of them stood.
    ///
    /// - Parameter progress: The paste.
    void endPaste(PasteProgress& progress);

    /// Whether an event is one its instance has queued, or numbered below
    /// one it has queued or had written ahead of its turn: a resend.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - inputSeq: The event's number.
    [[nodiscard]] bool isHeld(ID128 clientInstance, std::uint64_t inputSeq) const;

    /// One instance's input held unwritten, which its share bounds.
    struct Share {
        ID128 clientInstance;
        /// What its ordinary events write; signals are bounded on their own.
        std::size_t bytes = 0;
        std::size_t events = 0;
        /// Its entries of any kind, signals included: the share goes with the
        /// last of them.
        std::size_t entries = 0;
        /// The highest number among its entries, which tells a resend.
        std::uint64_t highestSeq = 0;
    };

    /// An instance's share, or null when it holds nothing.
    ///
    /// - Parameter clientInstance: The client.
    [[nodiscard]] const Share* share(ID128 clientInstance) const;

    /// Whether an instance's share takes an ordinary event of so many bytes.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - bytes: What the event writes.
    ///   - room: The most its sender may add across all of its panes.
    [[nodiscard]] bool admits(ID128 clientInstance, std::size_t bytes, std::size_t room) const;

    /// Queues an entry at a place, charging its instance's share.
    ///
    /// - Parameters:
    ///   - place: Where it stands.
    ///   - pending: The entry.
    void insert(std::deque<Pending>::iterator place, Pending&& pending);

    /// Removes the entry at an index, releasing its share.
    ///
    /// - Parameter index: The entry's place.
    void eraseAt(std::size_t index);

    /// Counts every share again from what waits, after entries were removed
    /// in bulk.
    void recountShares();

    /// Counts written each signal that passed its instance's earlier events
    /// once none of them waits.
    ///
    /// - Parameter ledger: Where the writes are recorded.
    void settlePassed(InputLedger& ledger);

    std::deque<Pending> m_pending;
    std::vector<PasteProgress> m_pastes;
    std::vector<Passed> m_passed;
    std::vector<Share> m_shares;
    std::size_t m_bytes = 0;
    std::size_t m_signalBytes = 0;
    std::uint64_t m_typedWrites = 0;
    bool m_isProgramGone = false;
};

} // namespace Ubiquitty::Protocol::Sync
