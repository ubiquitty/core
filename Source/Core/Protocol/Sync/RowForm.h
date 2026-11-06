// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/RowForm.h — the canonical row form and the dictionaries it names
// (Docs/sync.md §6.2, §7.4).
//
// A row travels as a *row record*: its line attribute, its flags, its OSC 133
// mark, and its cells as UTF-8 text plus pen runs. Pens, links and long
// clusters are named through per-pane dictionaries whose entries travel once,
// ahead of the first record that uses them.
//
//     RowRecord   := lineAttribute:u8  flags:u8 {Wrapped}  mark:u8  RowForm
//     RowForm     := extent:varint
//                    text:utf8
//                    runs:[(units:varint, pen:varint)]
//                    links:[(units:varint, link:varint)]
//                    clusters:[(gap:varint, codePoints:varint)]
//                    clusterRefs:[(gap:varint, cluster:varint)]
//                    wideKept:[gap:varint]
//
// Its rules:
//
// - Cells at and past `extent` are the canonical blank (`defaultBlankCell`),
//   so trailing blanks cost nothing.
// - The cells before `extent` are grouped into *units*. A unit is one cell,
//   except a lead whose pen carries `Wide` and whose next cell is exactly the
//   continuation the Core writes for it (`derivedContinuation`): that pair is
//   one unit, and the continuation never travels. A wide lead whose partner is
//   anything else is listed in `wideKept`, and its partner travels as a unit
//   of its own.
// - `text` holds each unit's lead cell: its base code point, then its inline
//   combining marks. An unwritten cell (code point 0) is U+0000, so a cell the
//   program never touched stays distinct from a space it wrote.
// - A unit is one code point unless `clusters` lists it with its count.
// - `runs` and `links` are run-length over units and each covers every unit;
//   an empty `links` means no unit carries a link. A pen is a dictionary id; a
//   link is the host's OSC 8 id, which the link dictionary defines.
// - `clusterRefs` gives the cluster field of a unit naming a cluster-pool or
//   proportional-run entry, by the host's id.
// - Gaps count units since the previous entry of the same list, or since the
//   row's start for the first.
//
// Every cell is reproduced byte for byte: a decoder fills exactly the cells
// the encoder read. The encoder refuses a row it cannot say exactly, which is
// a cell whose code point is not a Unicode scalar value.
//
// Spelling identity in units rather than cells is what keeps a wide character
// one text entry and one run entry: the lead and its continuation carry
// different pens, so counting them as cells would split every CJK row into
// alternating runs.
//
// The codec that writes and reads the form is `Core/Screen/RowForm.h`; the
// scrollback store folds its cold blocks with the same codec, naming pens in
// a table of the block's own.
//

#include "Core/Cell.h"
#include "Core/Pen.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/RowForm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// The bytes a pen takes on the wire. The underline word carries its colour
/// index in bits 0–8, its semantic role in bits 9–10, and in bits 11 and 12
/// whether the foreground and background are the DECAC normal text colour
/// standing in for the default; bits 13–15 are zero.
inline constexpr std::size_t penWireBytes = 20;

/// Encodes a pen into fixed storage without allocating.
/// - Parameter pen: The pen to encode.
/// - Returns: Its twenty canonical wire bytes.
[[nodiscard]] std::array<std::uint8_t, penWireBytes> encodePen(const Pen& pen);

/// Writes a pen as its twenty wire bytes.
///
/// - Parameters:
///   - writer: The body being written.
///   - pen: The pen.
void writePen(WireWriter& writer, const Pen& pen);

/// Reads a pen's twenty wire bytes.
[[nodiscard]] Pen readPen(WireReader& reader);

/// The row-level facts a row record carries beside its cells.
struct RowAttributes {
    /// DECDWL/DECDHL (`lineAttributeNormal` and siblings).
    std::uint8_t lineAttribute = 0;
    /// Whether the row soft-wraps into the next.
    bool isWrapped = false;
    /// The row's OSC 133 mark bits.
    std::uint8_t mark = 0;

    friend constexpr bool operator==(const RowAttributes&, const RowAttributes&) = default;
};

/// One OSC 8 link as the link dictionary defines it.
struct LinkDefinition {
    /// The host's id, which cells carry.
    std::uint16_t id = 0;
    /// The `id=` key the link was interned under, empty when it carried none.
    std::string key;
    /// The target.
    std::string uri;

    friend bool operator==(const LinkDefinition&, const LinkDefinition&) = default;
};

/// Reads the live definition of a host link id from a terminal's ring.
///
/// - Returns: The definition, or `nullopt` when no live slot holds the id.
[[nodiscard]] std::optional<LinkDefinition> linkDefinition(const Ubiquitty::Terminal& terminal, std::uint16_t id);

/// How far an attachment has been sent each dictionary.
struct DictionaryMarks {
    std::size_t pens = 0;
    std::size_t links = 0;
    std::size_t clusters = 0;

    friend constexpr bool operator==(const DictionaryMarks&, const DictionaryMarks&) = default;
};

/// The most pens updates define in one dictionary epoch past what its last
/// checkpoint defined (Docs/sync.md §7.9); a client refuses one past it.
inline constexpr std::size_t maxPensPerEpoch = 65'535;

/// The most link definitions updates make in one epoch past its last
/// checkpoint's (Docs/sync.md §7.9).
inline constexpr std::size_t maxLinksPerEpoch = 4'096;

/// The most long clusters updates define in one epoch past its last
/// checkpoint's (Docs/sync.md §7.9).
inline constexpr std::size_t maxClustersPerEpoch = 4'096;

/// The dictionaries one pane's rows are encoded against, shared by every
/// attachment to the pane.
///
/// Entries are append-only within an epoch, so an attachment's position in
/// each is one number (`DictionaryMarks`) and every entry past it is owed
/// before the first record that names it. Pens are numbered by this
/// dictionary. Links and long clusters keep the host's own ids, because a
/// cell carries those ids and a client's cell must equal the host's byte for
/// byte: the link dictionary is the sequence of (id, key, URI) definitions the
/// host has used, a later one replacing an earlier one with the same id, and
/// the cluster dictionary is the terminal's cluster pool, which is
/// append-only below its id ceiling.
class PaneDictionaries {
public:
    /// Portable dictionary content, without implementation-specific lookup maps.
    struct Snapshot {
        std::uint32_t epoch = 0;
        DictionaryMarks checkpointed;
        std::vector<Pen> pens;
        std::vector<LinkDefinition> links;
        std::vector<std::uint64_t> clusterHashes;
    };

    /// Copies the dictionary identities and their checkpoint accounting.
    /// - Returns: An owned snapshot of the current epoch.
    [[nodiscard]] Snapshot snapshot() const;

    /// Rebuilds lookup maps from admitted dictionary content.
    ///
    /// The containing handover reader bounds the owned vectors before calling.
    /// This is an in-memory reconstruction operation, not a wire decoder.
    /// - Parameter snapshot: Owned ordered definitions, consumed on success.
    /// - Returns: The dictionary, or no value for inconsistent IDs or marks.
    [[nodiscard]] static std::optional<PaneDictionaries> restored(Snapshot&& snapshot);

    /// The epoch the current entries belong to.
    [[nodiscard]] std::uint32_t epoch() const { return m_epoch; }

    /// Drops every entry and starts the next epoch.
    void roll();

    /// Records that a checkpoint defined every entry held: what updates define
    /// past this is what the per-epoch caps count.
    void noteCheckpointed();

    /// Whether updates have defined more since the last checkpoint than an
    /// epoch may hold: `maxPensPerEpoch` pens, `maxLinksPerEpoch` links or
    /// `maxClustersPerEpoch` clusters. A dictionary past its cap rolls the
    /// epoch, and the clients take the next frame as a checkpoint.
    [[nodiscard]] bool isPastEpochLimits() const;

    /// Returns the id of a pen, defining it when it is new.
    [[nodiscard]] std::uint32_t penID(const Pen& pen);

    /// Every pen defined this epoch, by id.
    [[nodiscard]] std::span<const Pen> pens() const { return m_pens; }

    /// Records that a row names a host link id, defining it when it is new or
    /// its slot now holds another target.
    ///
    /// An id no live slot holds is a link the host's ring retired; its cells
    /// resolve to no target on the host, and an undefined id resolves to none
    /// on the client, so it travels with no definition.
    ///
    /// - Parameters:
    ///   - terminal: The terminal whose ring holds the link.
    ///   - id: The id a cell carries.
    void noteLink(const Ubiquitty::Terminal& terminal, std::uint16_t id);

    /// Every link definition this epoch, in the order they were made.
    [[nodiscard]] std::span<const LinkDefinition> links() const { return m_links; }

    /// Records that a row names a cluster-pool id, checking that the pool
    /// still holds what this epoch has defined under it.
    ///
    /// - Returns: False when the pool can no longer be mirrored by appending:
    ///   it recycled past its id ceiling, or was cleared and regrew with other
    ///   content under ids already sent. Either needs a new epoch.
    [[nodiscard]] bool noteCluster(const Ubiquitty::Terminal& terminal, std::uint16_t id);

    /// How many pool entries this epoch has defined.
    [[nodiscard]] std::size_t clusterCount() const { return m_clusterHashes.size(); }

    /// Defines every pool entry the epoch has not yet defined.
    ///
    /// - Returns: False when the pool's ids are not its slots plus one, which
    ///   an append cannot mirror.
    [[nodiscard]] bool takeClusters(const Ubiquitty::Terminal& terminal);

private:
    /// A pen's wire bytes, which identify it independently of its layout.
    struct PenKey {
        std::array<std::uint8_t, penWireBytes> bytes {};
        friend bool operator==(const PenKey&, const PenKey&) = default;
    };
    /// Hashes a pen key for the id map.
    struct PenKeyHash {
        std::size_t operator()(const PenKey& key) const;
    };

    std::uint32_t m_epoch = 0;
    /// The entries held when a checkpoint last defined them all.
    DictionaryMarks m_checkpointed;
    std::vector<Pen> m_pens;
    std::unordered_map<PenKey, std::uint32_t, PenKeyHash> m_penIDs;
    std::vector<LinkDefinition> m_links;
    std::unordered_map<std::uint16_t, std::size_t> m_latestLink;
    std::vector<std::uint64_t> m_clusterHashes;
};

/// Whether a row's receiver holds the text of every proportional run.
///
/// A checkpoint defines all of them; an update defines none, so a row naming
/// one can only travel in a checkpoint.
enum class RunTextDefinitions : std::uint8_t {
    Absent,
    Present,
};

/// What a row record names that must already be defined where it lands.
///
/// Link ids are not among them: an id the receiver does not hold resolves to
/// no target, which is what a retired id resolves to on the host.
struct RowReferences {
    /// Whether each cluster-field id resolves, to a pool entry or a run text.
    std::vector<bool> clusters;
};

/// Encodes one row record.
///
/// - Parameters:
///   - terminal: The terminal the cells belong to, for link and cluster content.
///   - cells: The row's cells, one per column of its width.
///   - attributes: The row's line attribute, wrap and mark.
///   - dictionaries: The pane's dictionaries; pens and links the row names are
///     defined in it as a side effect.
///   - runTexts: Whether the receiver holds every proportional run's text, as
///     a checkpoint's does; when it does not, a row naming one is refused.
///   - output: The record is appended here.
/// - Returns: False, with `output` unchanged, when the row cannot be said
///   exactly; see the file comment.
[[nodiscard]] bool encodeRowRecord(const Ubiquitty::Terminal& terminal, std::span<const Cell> cells, const RowAttributes& attributes,
    PaneDictionaries& dictionaries, RunTextDefinitions runTexts, std::vector<std::uint8_t>& output);

/// Decodes one row record into a row of `cells.size()` cells.
///
/// Reads the whole record and refuses it, leaving `cells` in an unspecified
/// state, when any count, length, id or sum is out of bounds: the extent past
/// the width, text that does not split into the units the runs cover, a pen id
/// past `pens`, or a cluster id `references` does not resolve.
///
/// - Parameters:
///   - reader: Positioned at the record.
///   - pens: The pens defined so far, by id.
///   - references: What resolves on the receiver.
///   - cells: The row to fill, one cell per column.
///   - attributes: Receives the record's row-level facts.
/// - Returns: Whether the record was well formed; the reader is failed otherwise.
[[nodiscard]] bool decodeRowRecord(WireReader& reader, std::span<const Pen> pens, const RowReferences& references,
    std::span<Cell> cells, RowAttributes& attributes);

/// Uses the canonical row walk with borrowed pen lookup and reference policy.
/// `pens` supplies size(), empty() and indexed Pen values; neither lookup owns data.
template<typename Pens, typename Resolves>
[[nodiscard]] bool decodeRowRecordWithNames(WireReader& reader, const Pens& pens, Resolves&& resolves,
    std::span<Cell> cells, RowAttributes& attributes)
{
    attributes.lineAttribute = reader.u8();
    const std::uint8_t flags = reader.u8();
    attributes.isWrapped = (flags & 1) != 0;
    attributes.mark = reader.u8();
    if (flags > 1)
        reader.fail();
    return Screen::readRowForm(reader, pens, std::forward<Resolves>(resolves), cells);
}

} // namespace Ubiquitty::Protocol::Sync
