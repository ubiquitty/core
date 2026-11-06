// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/HistoryCache.h — what a client keeps of one pane's history
// (Docs/sync.md §6.6).
//
// The cache holds logical lines of one epoch, keyed by the host instance,
// the pane and the epoch. Its invariants:
//
// - A line of another epoch is never stored.
// - `reset` empties the cache, and the epoch it names is the only one taken
//   afterwards; nothing below its `clearedThrough` is stored.
// - A head set below the lines held hides the lines at or above it. They stay
//   hidden with their hashes, and a head set above them again restores each
//   one only when no line of its number arrived in between.
// - The caps are the client's own, independent of the host's ring: a line
//   count and a byte count, and past either the oldest line goes first.
// - A line older than the host's oldest is kept and flagged beyond the host's
//   horizon; a gap never names one, since the host no longer has it.
// - `project` writes the newest contiguous run below the head into a
//   replica's scrollback, wrapped at the client's width: each client wraps
//   history at its own.
// - The open line, the first rows of the line straddling the host's seam, is
//   held apart, at most one. It is shown after the run, its last row
//   continuing onto the screen, while it is numbered the head and the run
//   reaches the head. The completed line of its number, a newer open line, a
//   withdrawal or a head past it removes it.
//
// The cache sits beside its owner, `ClientConnection`, until the pane link
// that will own it exists; the purge reaches the copies it does not own (a
// resume record, a disk cache) through a `HistoryPurgeSink`.
//

#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/Messages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// The bytes of one pane's history cache unless its owner says otherwise:
/// 10,000 lines at 200 columns of 32-byte cells, rounded up
/// (Docs/sync-measurements.md, "Memory admission").
inline constexpr std::size_t defaultHistoryCacheBytes = std::size_t { 64 } << 20;

/// Which history a cache holds.
struct HistoryKey {
    ID128 hostInstanceID;
    ID128 paneID;
    std::uint32_t epoch = 0;

    friend constexpr bool operator==(const HistoryKey&, const HistoryKey&) = default;
};

/// What `HistoryCache::insert` did.
enum class CacheInsert : std::uint8_t {
    /// The lines are held.
    Stored,
    /// They belong to an epoch other than the cache's; nothing was stored.
    WrongEpoch,
    /// One is below the epoch's `clearedThrough`; nothing was stored.
    BelowClearedThrough,
};

/// Whether external copies of a retired history epoch are confirmed gone.
enum class HistoryPurgeOutcome : std::uint8_t {
    Purged,
    Refused,
};

/// Canonical history bytes and the admitted decoded payload they describe.
struct HistorySnapshot {
    std::vector<std::uint8_t> bytes;
    std::size_t retainedBytes = 0;
};

/// Receives the purge a `HistoryReset` obliges, for copies of a pane's history
/// the cache does not own. Borrowed by whoever calls it, never owned. Calls
/// complete synchronously and must not mutate or destroy the calling client
/// or reenter the cache; refusal prevents the new epoch from committing.
class HistoryPurgeSink {
public:
    virtual ~HistoryPurgeSink() = default;

    /// Purges every line of a retired epoch.
    ///
    /// - Parameters:
    ///   - retired: The key the cache held before the reset.
    ///   - clearedThrough: Every line below it is gone.
    /// - Returns: Whether retirement is confirmed.
    [[nodiscard]] virtual HistoryPurgeOutcome purge(const HistoryKey& retired, std::uint64_t clearedThrough) = 0;
};

/// A client's cache of one pane's history.
class HistoryCache {
public:
    /// Creates an empty cache of the first epoch.
    ///
    /// - Parameters:
    ///   - hostInstanceID: The host the pane lives on.
    ///   - paneID: The pane.
    ///   - maxLines: The most lines held.
    ///   - maxBytes: The most line storage held (`retainedStorageBytes`).
    HistoryCache(ID128 hostInstanceID, ID128 paneID, std::size_t maxLines, std::size_t maxBytes = defaultHistoryCacheBytes);

    /// Stores lines of an epoch.
    ///
    /// - Parameters:
    ///   - epoch: The epoch they were sent under.
    ///   - lines: The lines, in any order.
    /// - Returns: `Stored`, or why nothing was stored.
    [[nodiscard]] CacheInsert insert(std::uint32_t epoch, std::span<const DecodedLine> lines);

    /// Takes the open line, replacing the one held; a line with no cells
    /// withdraws it.
    ///
    /// - Parameters:
    ///   - epoch: The epoch it was sent under.
    ///   - line: The line, flagged `Open`.
    /// - Returns: `Stored`, or why nothing was taken.
    [[nodiscard]] CacheInsert setOpenLine(std::uint32_t epoch, DecodedLine line);

    /// The open line, when it is numbered the head.
    [[nodiscard]] const DecodedLine* openLine() const { return m_openLine && m_openLine->number == m_head ? &*m_openLine : nullptr; }

    /// Purges the cache for a new epoch.
    ///
    /// - Parameters:
    ///   - reset: The reset.
    ///   - sink: Confirms retirement before the cache changes; may be null.
    /// - Returns: Purged, or refusal with the previous cache unchanged.
    [[nodiscard]] HistoryPurgeOutcome reset(const HistoryReset& reset, HistoryPurgeSink* sink = nullptr);

    /// Moves the head the host last reported. A head below the lines held
    /// hides the lines at or above it; one above restores hidden lines no
    /// newer line replaced.
    ///
    /// - Parameters:
    ///   - epoch: The epoch the head is counted in; another epoch's is ignored.
    ///   - head: The number the host's next line takes.
    void setHead(std::uint32_t epoch, std::uint64_t head);

    /// Records the host's oldest line; held lines below it are beyond the
    /// host's horizon.
    ///
    /// - Parameter oldest: The host's oldest line.
    void noteHostOldest(std::uint64_t oldest);

    /// The line of a number, or null when it is not held or is hidden.
    [[nodiscard]] const DecodedLine* line(std::uint64_t number) const;

    /// Whether a held line is older than the host's oldest.
    [[nodiscard]] bool isBeyondHostHorizon(std::uint64_t number) const;

    /// The runs of held lines, ascending.
    [[nodiscard]] std::vector<LineRange> cachedRanges() const;

    /// The parts of a range not held that the host can still send: below
    /// neither the epoch's `clearedThrough` nor the host's oldest.
    ///
    /// - Parameter range: The range.
    /// - Returns: The gaps, ascending.
    [[nodiscard]] std::vector<LineRange> gaps(LineRange range) const;

    /// The end of the newest run of held lines, or the epoch's
    /// `clearedThrough` when none is held: what the client acknowledges.
    [[nodiscard]] std::uint64_t contiguousHead() const;

    /// The lines held and not hidden.
    [[nodiscard]] std::size_t lineCount() const { return m_lines.size(); }

    /// The host, pane and epoch the cache holds.
    [[nodiscard]] const HistoryKey& key() const { return m_key; }

    /// Rekeys retained history after an authenticated Welcome names its exact
    /// predecessor. Content, epoch, horizons and projection remain unchanged.
    /// This is not an authentication check; the connection admits the Welcome.
    /// - Parameters:
    ///   - predecessor: The instance this cache must currently belong to.
    ///   - successor: The nonempty instance continuing that host's sessions.
    /// - Returns: False without mutation for an empty or mismatched identity.
    [[nodiscard]] bool rekeyForHandover(ID128 predecessor, ID128 successor);

    /// The head the host last reported.
    [[nodiscard]] std::uint64_t head() const { return m_head; }

    /// The retained line objects and payload sizes, excluding spare container
    /// capacity and allocator bookkeeping, kept as lines come and go.
    /// - Returns: Their summed storage, including hidden and open lines.
    [[nodiscard]] std::size_t retainedStorageBytes() const { return m_retainedBytes; }

    /// The byte cap the cache evicts to.
    [[nodiscard]] std::size_t maxBytes() const { return m_maxBytes; }

    /// Evicts the oldest lines, then the oldest hidden ones, until the cache
    /// holds at most `bytes`: what a replica owner with no room left asks of
    /// the pane that grew. The open line stays.
    ///
    /// - Parameter bytes: The storage to fit in.
    void trimToBytes(std::size_t bytes);

    /// The storage one line takes in `retainedStorageBytes`.
    ///
    /// - Parameter line: The line.
    /// - Returns: Its object and payload bytes, saturated at the largest size.
    [[nodiscard]] static std::size_t lineStorageBytes(const DecodedLine& line);

    /// Encodes the newest retained lines for an authenticated encrypted store.
    /// Hidden pullback lines are not advertised or persisted. Older lines are
    /// omitted when either budget is exhausted; a line is never split.
    ///
    /// - Parameters:
    ///   - maxBytes: The encoded snapshot budget, including identity metadata.
    ///   - maxRetainedBytes: The decoded line objects and payload budget.
    /// - Returns: A bounded snapshot, or no value if its header cannot fit or a line is invalid.
    [[nodiscard]] std::optional<HistorySnapshot> snapshot(std::size_t maxBytes, std::size_t maxRetainedBytes) const;

    /// Restores an authenticated snapshot only for this cache's exact host and
    /// pane. Invalid input leaves all current history unchanged. The caller
    /// retires any prior durable epoch before replacing a live cache.
    /// One independently bounded history body is decoded as scratch before
    /// its retained payload is admitted; this is not a peak allocator budget.
    ///
    /// - Parameters:
    ///   - bytes: The complete immutable snapshot.
    ///   - maxBytes: The encoded storage budget.
    ///   - maxRetainedBytes: The decoded line objects and payload budget.
    /// - Returns: Whether the complete snapshot was accepted.
    [[nodiscard]] bool restoreSnapshot(std::span<const std::uint8_t> bytes, std::size_t maxBytes, std::size_t maxRetainedBytes);

    /// Writes the newest contiguous run below the head into a replica's
    /// scrollback, each line wrapped at `columns`. Links and clusters are interned into the
    /// replica's own tables.
    ///
    /// When the replica holds the run as the last projection left it and only
    /// lines past it arrived since, those lines are appended; anything else —
    /// another width, a line changed or hidden below the end, a reset —
    /// replaces what the scrollback held.
    ///
    /// - Parameters:
    ///   - replica: The client's terminal for the pane.
    ///   - columns: The width to wrap at.
    void project(Ubiquitty::Terminal& replica, int columns);

    /// Records that the replica's scrollback no longer holds the last
    /// projection, so the next is whole.
    void invalidateProjection()
    {
        m_lowestChange = 0;
        m_projectedColumns = 0;
    }

private:
    /// Notes a change at or above a number, which the next projection must
    /// cover.
    ///
    /// - Parameter number: The lowest line changed.
    void noteChange(std::uint64_t number) { m_lowestChange = std::min(m_lowestChange, number); }

    /// Evicts the oldest lines past the line cap, then the oldest lines and
    /// hidden lines until the cache holds at most `bytes`.
    ///
    /// - Parameter bytes: The storage to fit in.
    void evict(std::size_t bytes);

    HistoryKey m_key;
    std::size_t m_maxLines;
    std::size_t m_maxBytes;
    std::size_t m_retainedBytes = 0;
    std::uint64_t m_clearedThrough = 0;
    std::uint64_t m_head = 0;
    std::uint64_t m_hostOldest = 0;
    std::map<std::uint64_t, DecodedLine> m_lines;
    std::map<std::uint64_t, DecodedLine> m_hidden;
    std::optional<DecodedLine> m_openLine;
    /// The run the last projection wrote, `[first, end)`, at `m_projectedColumns`.
    std::uint64_t m_projectedFirst = 0;
    std::uint64_t m_projectedEnd = 0;
    int m_projectedColumns = 0;
    /// The open line the last projection wrote after the run, by number and
    /// hash, and the rows it took.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> m_projectedOpenLine;
    int m_projectedOpenRows = 0;
    /// The lowest number changed since the last projection.
    std::uint64_t m_lowestChange = 0;
};

/// The bytes before each body of a history section: the number of its newest
/// line and its length.
inline constexpr std::size_t historySectionPrefixBytes = sizeof(std::uint64_t) + sizeof(std::uint32_t);

/// Seeds a terminal's scrollback from a history section: history bodies
/// (§7.5) back to back, each behind `newestLine:u64 length:u32` and listing
/// its newest line first, each continuing where the one before it ended.
///
/// The lines are wrapped into the scrollback at the terminal's width; their
/// links and clusters resolve through the ring and pool the terminal holds.
/// The section is untrusted: it is decoded whole, and refused before the
/// scrollback is touched when a body does not decode or does not continue
/// the one before it.
///
/// - Parameters:
///   - terminal: The terminal, its screen already in place.
///   - section: The section; empty for no history.
///   - olderLinesNote: Text for one line placed above the oldest line the
///     section carries, saying what is not there; empty for none.
/// - Returns: False, with the scrollback as it was, when the section is
///   refused.
[[nodiscard]] bool applyHistorySection(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> section, std::u32string_view olderLinesNote);

} // namespace Ubiquitty::Protocol::Sync
