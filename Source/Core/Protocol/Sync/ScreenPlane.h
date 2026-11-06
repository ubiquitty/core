// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/ScreenPlane.h — the host's publisher and the client's replica of one
// pane's screen plane: the checkpoint, updates, and the state digest that
// checks both (Docs/sync.md §6.2, §7.4).
//
// The host keeps, per pane, the dictionaries, both screens' rows as last
// encoded, and the state blocks of the frame it last captured. A row's cached
// record is reused only after a memcmp of the cells and row facts it was
// encoded from, so the row hash is a diff key and never what decides a row is
// unchanged. A frame is captured once and every attachment's update is cut
// from that capture, so one encoded row serves every attachment.
//
// A checkpoint is self-contained: it defines every dictionary entry of the
// epoch, and carries both screens, the modes, the palette, the cursor and
// every per-screen and character state `CheckpointLedger.h` classifies as
// carried. Of the history it carries only the address — the epoch and the
// head — since the lines travel on the history plane (HistoryPlane.h). An
// update brings one attachment from what it holds to the captured frame: the
// definitions it lacks, the state blocks whose bytes moved, and the rows of
// either screen whose hashes moved. Only a change no update can say — the
// geometry, the dictionary epoch, a proportional run's text — needs a
// checkpoint.
//
// Both kinds end with the state digest of what they produce. A client
// computes it from the bytes it received — the hash of each row record and
// block as it arrived, the hashes it already held for what did not travel —
// and applies nothing unless it matches. Apply is transactional: a body is
// decoded and checked whole before the terminal is touched.
//
// Body grammars:
//
//     Checkpoint := generation:u64 dictionaryEpoch:u32 rows:u16 columns:u16 activeScreen:u8
//                   modes:bytes palette:bytes cursor:bytes screenStates:bytes textState:bytes
//                   activity:bytes
//                   pens:[Pen] links:[(id:u16, key:utf8, uri:utf8)]
//                   clusters:[codePoints:[varint]] runTexts:[(id:varint, codePoints:[varint])]
//                   richTexts:[Definition]
//                   activeRows:RowRecord×rows inactiveRows:RowRecord×rows
//                   historyEpoch:u32 historyHead:u64
//                   parserGround:bytes (the REP record, which no update carries)
//                   stateDigest:u128
//     Update     := activeScreen:u8 rows:u16 columns:u16 dictionaryEpoch:u32 ops:[Op] stateDigest:u128
//     Op         := DefinePen(1) id:varint Pen
//                 | DefineLink(2) id:u16 key:utf8 uri:utf8
//                 | DefineCluster(3) id:varint codePoints:[varint]
//                 | Row(4) row:varint RowRecord                     the active screen
//                 | InactiveRow(5) row:varint RowRecord             the other screen
//                 | Block(6) block:u8 bytes                         a `StateBlock`, whole
//                 | DefineRichText(8) Definition                   a block's document
//                 | Spans(9) screen:u8 row:varint baseHash:u64 rowHash:u64
//                            count:varint (column:u16 cells:u16 RowRecord)×count
//                 | ScrollRect(10) screen:u8 top:u16 bottom:u16 delta:svarint
//
// Op 7 is unassigned: it carried history lines before the history plane did.
// A `screen` byte is 0 for the active screen and 1 for the other.
//
// `Spans` and `ScrollRect` say a row in terms of the row the client holds, so
// they go only in a chain update, and only to a client whose `Greet` names
// them among its capabilities. `Spans` rewrites cells of a row whose hash is
// `baseHash`: each span's record, decoded into `cells` cells, replaces the
// row's cells from `column`, and the row's hash becomes `rowHash`; its line
// attribute, flags and mark stay. `ScrollRect` moves the rows `top` to
// `bottom` of a screen by `delta`, row `r` taking row `r + delta`'s cells and
// hash; a row whose source falls outside the rectangle is revealed blank, with
// hash 0, and a later op writes it. A `ScrollRect` precedes every row op of
// its screen, at most one per screen, and no `Spans` names a row it moved.
//
// A checkpoint's links are the host's live link ring in ring order, so the
// client's ring evicts what the host's evicts. An update's ops come in that
// order: definitions, blocks, scrolls, rows. `Definition` and the `RichText`
// block's grammar are in `RichTextForm.h`; a document is defined once per
// attachment, before the block that places it.
//
// Each RowRecord inside a body travels as `bytes`, so its hash covers exactly
// the record. The state digest is XXH3-128 over
//
//     activeScreen:u8 rows:u16 columns:u16
//     rowHash × rows (active screen), rowHash × rows (inactive screen)
//     blockHash × 7 (in `StateBlock` order)
//
// The digest has no history term (Docs/sync.md §6.2): history is checked by
// each line's own hash and by what the client acknowledges.
//

#include "Base/Bits/OptionSet.h"
#include "Core/Pen.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/ReplicaStorage.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/TermPalette.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Ubiquitty {
struct Terminal;
/// Which of a terminal's two screens; defined in `Core/CellGrid.h`.
enum class ScreenMode : std::uint8_t;
}

namespace Ubiquitty::Protocol::Sync {

class WireReader;
class WireWriter;

/// Writes a live palette and its baseline as a checkpoint's `PaletteBlock`
/// codes them: the live one against the seeded palette, the baseline against
/// the live one, so a theme with no resets costs little.
///
/// - Parameters:
///   - writer: The writer.
///   - live: The live palette.
///   - baseline: The palette a reset returns to.
void putPaletteBlock(WireWriter& writer, const Ubiquitty::TermPalette& live, const Ubiquitty::TermPalette& baseline);

/// Reads what `putPaletteBlock` wrote, failing the reader on a bad delta.
///
/// - Parameters:
///   - reader: The reader.
///   - live: The live palette read.
///   - baseline: The baseline read.
void takePaletteBlock(WireReader& reader, Ubiquitty::TermPalette& live, Ubiquitty::TermPalette& baseline);

/// Why a body was not applied.
enum class ApplyOutcome : std::uint8_t {
    /// The body was applied.
    Applied,
    /// A length, count, id, range or sum was out of bounds; nothing was applied.
    Malformed,
    /// The body was well formed but builds a state whose digest differs from
    /// the one it carries; nothing was applied.
    DigestMismatch,
    /// A row update named a base the replica does not hold: another
    /// geometry, screen or dictionary epoch; nothing was applied.
    BaseMissing,
    /// A checkpoint's storage allowance or a fallible owner allocation was
    /// refused; nothing was applied.
    AllocationRefused,
};

/// An op of an update body.
enum class UpdateOp : std::uint8_t {
    DefinePen = 1,
    DefineLink = 2,
    DefineCluster = 3,
    Row = 4,
    InactiveRow = 5,
    Block = 6,
    DefineRichText = 8,
    Spans = 9,
    ScrollRect = 10,
    /// What the client's predictor reads of the pane: whether the program is
    /// reading a line at a prompt with echo on, whether another writer typed,
    /// and the characters that signal (Docs/sync.md §6.7). It travels when
    /// it changes, to a client that takes it, and is outside the digest.
    EchoContext = 11,
};

/// Every update op, in ascending wire order: the one list of the op numbers.
inline constexpr UpdateOp updateOps[] {
    UpdateOp::DefinePen,
    UpdateOp::DefineLink,
    UpdateOp::DefineCluster,
    UpdateOp::Row,
    UpdateOp::InactiveRow,
    UpdateOp::Block,
    UpdateOp::DefineRichText,
    UpdateOp::Spans,
    UpdateOp::ScrollRect,
    UpdateOp::EchoContext,
};

// Strictly ascending, so no two ops share a number: an op given another's
// number would be applied as that other op.
static_assert([] {
    for (std::size_t index = 1; index < std::size(updateOps); ++index) {
        if (std::to_underlying(updateOps[index - 1]) >= std::to_underlying(updateOps[index]))
            return false;
    }
    return true;
}(),
    "every update op has a number of its own");

/// What an `EchoContext` says of the pane.
enum class EchoFlag : std::uint8_t {
    /// The program turned echo off: a password is being typed.
    SecureEntry = 1 << 0,
    /// Another writer's input was written since the last frame.
    ForeignInput = 1 << 1,
    /// The shell reads a command line: OSC 133 B seen, C not yet.
    AtPrompt = 1 << 2,
};

/// The flags of an `EchoContext`.
using EchoFlags = Ubiquitty::Bits::OptionSet<EchoFlag>;

/// The characters that signal the program's foreground group, as its line
/// discipline sets them; one the program disabled is absent.
struct InterruptCharacters {
    std::optional<std::uint8_t> interrupt;
    std::optional<std::uint8_t> quit;
    std::optional<std::uint8_t> suspend;

    friend bool operator==(const InterruptCharacters&, const InterruptCharacters&) = default;

    /// Whether a byte is one of them.
    ///
    /// - Parameter byte: The byte.
    [[nodiscard]] bool contains(std::uint8_t byte) const { return interrupt == byte || quit == byte || suspend == byte; }

    /// What a new terminal's line discipline starts with: Ctrl-C, Ctrl-\ and
    /// Ctrl-Z. A client that holds no echo context for a pane matches these.
    [[nodiscard]] static constexpr InterruptCharacters terminalDefaults() { return { .interrupt = 0x03, .quit = 0x1c, .suspend = 0x1a }; }
};

/// What the host tells a client's predictor about a pane.
struct EchoContext {
    /// Moves whenever the program's modes change.
    std::uint32_t modesRevision = 0;
    /// Moves whenever the foreground program changes.
    std::uint32_t foregroundGeneration = 0;
    EchoFlags flags;
    InterruptCharacters interruptCharacters;
    /// VERASE from the PTY; absent when disabled.
    std::optional<std::uint8_t> eraseCharacter = 0x7f;

    friend bool operator==(const EchoContext&, const EchoContext&) = default;
};

/// An op a chain update may use only for a client that takes it.
enum class ChainOp : std::uint8_t {
    Spans = 1 << 0,
    ScrollRect = 1 << 1,
};

/// The optional ops an update may use.
using ChainOps = Ubiquitty::Bits::OptionSet<ChainOp>;

/// One screen's rows as the host last encoded them.
class RowEncodingCache {
public:
    /// Re-encodes every row whose cells or row facts differ from what was
    /// encoded, confirming the rest by memcmp.
    ///
    /// - Parameters:
    ///   - terminal: The terminal to read.
    ///   - screen: Which screen this cache holds.
    ///   - dictionaries: The pane's dictionaries; an epoch other than the
    ///     cache's re-encodes every row.
    /// - Returns: False when a row cannot be said exactly.
    [[nodiscard]] bool refresh(const Ubiquitty::Terminal& terminal, ScreenMode screen, PaneDictionaries& dictionaries);

    /// The encoded record of a row.
    [[nodiscard]] std::span<const std::uint8_t> record(int row) const { return m_rows[static_cast<std::size_t>(row)].record; }

    /// The hash of a row's record.
    [[nodiscard]] std::uint64_t hash(int row) const { return m_rows[static_cast<std::size_t>(row)].hash; }

    /// Whether a row names a proportional run's text, which only a checkpoint
    /// defines.
    [[nodiscard]] bool hasRunTextAnchor(int row) const { return m_rows[static_cast<std::size_t>(row)].hasRunTextAnchor; }

    /// How many rows the cache holds.
    [[nodiscard]] int rowCount() const { return static_cast<int>(m_rows.size()); }

    /// How many records the last `refresh` encoded rather than reused.
    [[nodiscard]] int encodedCount() const { return m_encodedCount; }

    /// The cells a row was encoded from.
    [[nodiscard]] std::span<const Cell> cells(int row) const { return m_rows[static_cast<std::size_t>(row)].cells; }

    /// The row facts a row was encoded from.
    [[nodiscard]] const RowAttributes& attributes(int row) const { return m_rows[static_cast<std::size_t>(row)].attributes; }

    /// The hash the row had before it was last re-encoded, with the cells and
    /// facts that gave it, or null when the row has been encoded once.
    [[nodiscard]] const std::uint64_t* previousHash(int row) const
    {
        const Row& entry = m_rows[static_cast<std::size_t>(row)];
        return entry.hasPrevious ? &entry.previousHash : nullptr;
    }

    /// The cells a row held before it was last re-encoded.
    [[nodiscard]] std::span<const Cell> previousCells(int row) const { return m_rows[static_cast<std::size_t>(row)].previousCells; }

    /// The row facts a row held before it was last re-encoded.
    [[nodiscard]] const RowAttributes& previousAttributes(int row) const { return m_rows[static_cast<std::size_t>(row)].previousAttributes; }

private:
    /// One row as last encoded, with the cells and row facts it was encoded
    /// from, and the encoding before it, from which a `Spans` op is cut.
    struct Row {
        std::vector<Cell> cells;
        RowAttributes attributes;
        std::vector<std::uint8_t> record;
        std::uint64_t hash = 0;
        bool hasRunTextAnchor = false;
        bool isValid = false;
        std::vector<Cell> previousCells;
        RowAttributes previousAttributes;
        std::uint64_t previousHash = 0;
        bool hasPrevious = false;
    };
    std::vector<Row> m_rows;
    std::vector<Cell> m_scratch;
    std::uint32_t m_epoch = 0;
    int m_columns = 0;
    int m_encodedCount = 0;
};

/// A piece of screen-wide state an update carries whole when its bytes move.
enum class StateBlock : std::uint8_t {
    /// Where the active screen's cursor is and how it is drawn.
    Cursor,
    /// Every DEC and ANSI mode, the input encodings and the report state.
    Modes,
    /// The live and baseline palettes and the dynamic colours.
    Palette,
    /// Each screen's margins, pen and saved cursor, and the inactive one's cursor.
    ScreenStates,
    /// The character sets, titles, tab stops, user-defined keys, text sizes
    /// and the status row.
    TextState,
    /// The OSC 3008 contexts and the OSC 7501 program status records, which
    /// a frontend shows beside the screen; carried here until a status-record
    /// plane carries them (Docs/sync.md §7.7).
    Activity,
    /// Where each screen's rich text blocks are; their documents travel as
    /// definitions (Sync/RichTextForm.h).
    RichText,
};

/// How many state blocks a frame has.
inline constexpr std::size_t stateBlockCount = 7;

/// The bytes one text-size slot occupies in the text-state block: scale, width,
/// the fraction's two terms, the two alignments, the proportional flag, the run
/// width, the claimed run width and the continuation flag.
///
/// The editors' clients walk the table to learn which specs are proportional,
/// and their generated wire tables carry these two; a slot of
/// another length puts every later slot's flag in the wrong byte.
inline constexpr std::size_t textSizeSpecWireBytes = 12;

/// Where a text-size slot's proportional flag sits within it.
inline constexpr std::size_t textSizeSpecProportionalOffset = 6;

/// The hash of each state block, in `StateBlock` order.
using StateBlockHashes = std::array<std::uint64_t, stateBlockCount>;

/// What one attachment holds of a pane's screen plane, as the host tracks it.
struct AttachmentBaseline {
    std::uint32_t dictionaryEpoch = 0;
    DictionaryMarks marks;
    bool isAlternate = false;
    int columns = 0;
    /// The hash of every row of each screen as the attachment holds it,
    /// primary first.
    std::array<std::vector<std::uint64_t>, 2> rowHashes;
    StateBlockHashes blockHashes {};
    /// The serials of the rich text documents the attachment holds, ascending.
    std::vector<std::uint32_t> richTextSerials;

    /// The active screen's row hashes.
    [[nodiscard]] const std::vector<std::uint64_t>& activeRowHashes() const { return rowHashes[isAlternate ? 1 : 0]; }
};

/// Whether an update could be built.
enum class UpdateOutcome : std::uint8_t {
    /// The update was built; it may hold no ops.
    Encoded,
    /// Only a checkpoint can bring the attachment up: the geometry or the
    /// dictionary epoch moved, or a row names what no update defines.
    NeedsCheckpoint,
};

/// One pane's screen-plane encoder, shared by every attachment.
class ScreenPublisher {
public:
    /// Dictionary identities and the exact captured frame they describe.
    struct Continuation {
        PaneDictionaries::Snapshot dictionaries;
        std::array<std::vector<std::uint64_t>, 2> rowHashes;
        StateBlockHashes blockHashes {};
        Ubiquitty::Screen::HistoryAddress history;
        int rows = 0;
        int columns = 0;
        bool isAlternate = false;
    };

    /// Copies the identity needed to continue a captured frame's dictionary epoch.
    /// - Returns: An owned continuation, or no value before a successful capture.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Rebuilds captured caches without renumbering dictionary entries.
    ///
    /// The supplied terminal must already hold the complete carried state.
    /// A mismatching frame leaves this publisher unchanged. Vector ownership
    /// is admitted by the containing handover reader before this call.
    /// - Parameters:
    ///   - terminal: Restored terminal, unchanged throughout reconstruction.
    ///   - continuation: Owned exact frame and dictionary identity.
    /// - Returns: Whether the same frame could be reconstructed.
    [[nodiscard]] bool restore(const Ubiquitty::Terminal& terminal, Continuation continuation);

    /// Captures the terminal as the frame every later update and checkpoint
    /// describes: both screens' rows, the state blocks and the history's
    /// address.
    ///
    /// - Parameter terminal: The pane's terminal.
    /// - Returns: False when the frame cannot be said exactly; nothing can
    ///   then bring an attachment up until a later capture succeeds.
    [[nodiscard]] bool capture(const Ubiquitty::Terminal& terminal);

    /// Encodes a checkpoint of the captured frame and records it as the
    /// attachment's baseline.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, holding the captured frame.
    ///   - generation: The generation the checkpoint is of.
    ///   - body: Receives the body.
    ///   - attachment: Becomes what the checkpoint gives its receiver.
    /// - Returns: False, with `body` unchanged, when no frame is captured.
    [[nodiscard]] bool checkpoint(const Ubiquitty::Terminal& terminal, std::uint64_t generation, std::vector<std::uint8_t>& body, AttachmentBaseline& attachment);

    /// Encodes the update that brings an attachment to the captured frame.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, holding the captured frame.
    ///   - attachment: What the attachment holds; advanced to what the update
    ///     gives it.
    ///   - body: Receives the body.
    ///   - chainOps: The optional ops the update may use: none for a
    ///     superset update, which must apply onto any generation the client
    ///     may hold, and for a client that takes none.
    ///   - echoContext: An echo context to carry, first among the ops, or
    ///     null.
    /// - Returns: Whether an update could say it.
    [[nodiscard]] UpdateOutcome rowUpdate(const Ubiquitty::Terminal& terminal, AttachmentBaseline& attachment, std::vector<std::uint8_t>& body, ChainOps chainOps = {}, const EchoContext* echoContext = nullptr);

    /// The rows, of both screens, the last encoded update carried.
    [[nodiscard]] std::size_t lastUpdateRows() const { return m_lastUpdateRows; }

    /// The `Spans` ops the last encoded update carried.
    [[nodiscard]] std::size_t lastUpdateSpans() const { return m_lastUpdateSpans; }

    /// The `ScrollRect` ops the last encoded update carried.
    [[nodiscard]] std::size_t lastUpdateScrollRects() const { return m_lastUpdateScrollRects; }

    /// Whether the last capture succeeded.
    [[nodiscard]] bool isCaptured() const { return m_isCaptured; }

    /// Whether the captured frame shows the alternate screen.
    [[nodiscard]] bool isAlternate() const { return m_isAlternate; }

    /// The captured frame's state-block hashes.
    [[nodiscard]] const StateBlockHashes& blockHashes() const { return m_blockHashes; }

    /// The pane's dictionaries.
    [[nodiscard]] const PaneDictionaries& dictionaries() const { return m_dictionaries; }

    /// Defines links that history lines name in the pane's dictionaries, so
    /// the next update defines them for every attachment: a line can leave
    /// the screen within one frame, before any update named its link.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal.
    ///   - ids: The host's link ids.
    void noteLinks(const Ubiquitty::Terminal& terminal, std::span<const std::uint16_t> ids);

    /// Where the history stood at the last capture.
    [[nodiscard]] const Ubiquitty::Screen::HistoryAddress& historyAddress() const { return m_historyAddress; }

    /// A screen's row cache, `0` primary and `1` alternate.
    [[nodiscard]] const RowEncodingCache& rows(int screen) const { return m_screens[static_cast<std::size_t>(screen)]; }

private:
    std::size_t m_lastUpdateRows = 0;
    std::size_t m_lastUpdateSpans = 0;
    std::size_t m_lastUpdateScrollRects = 0;

    /// Refreshes both screens' row caches and the state blocks, and defines
    /// every cluster the pool holds.
    [[nodiscard]] bool refreshScreens(const Ubiquitty::Terminal& terminal);

    PaneDictionaries m_dictionaries;
    std::array<RowEncodingCache, 2> m_screens;
    std::array<std::vector<std::uint8_t>, stateBlockCount> m_blocks;
    StateBlockHashes m_blockHashes {};
    bool m_isCaptured = false;
    bool m_isAlternate = false;
    int m_rows = 0;
    int m_columns = 0;
    Ubiquitty::Screen::HistoryAddress m_historyAddress;
};

/// What a client holds of a pane's screen plane between bodies.
struct ScreenReplica {
    std::uint64_t generation = 0;
    std::uint32_t dictionaryEpoch = 0;
    ReplicaStorage storage;
    [[nodiscard]] std::span<const Pen> pens() const { return storage.pens(); }
    [[nodiscard]] std::array<std::span<const std::uint64_t>, 2> rowHashes() const { return storage.rowHashes(); }
    /// The most pens the epoch may hold: those its checkpoint defined and a
    /// cap's worth more (`maxPensPerEpoch`). An update defining one past it
    /// is refused.
    std::size_t maxPens = std::numeric_limits<std::size_t>::max();
    StateBlockHashes blockHashes {};
    /// The history's epoch and head the last checkpoint named.
    std::uint32_t historyEpoch = 0;
    std::uint64_t historyHead = 0;
    /// The state digest of the generation held.
    Digest128 stateDigest;
    /// A terminal an update's blocks are decoded into before any is committed;
    /// kept between updates because the cursor block moves on most of them.
    std::shared_ptr<Ubiquitty::Terminal> scratch;
    /// The echo context the host last sent, absent before the first.
    std::optional<EchoContext> echoContext;
};

/// The digest of a screen as one end holds it: every row hash of both screens,
/// primary first, each screen's count before its hashes, then every state
/// block's hash (Docs/sync.md §6.9). A resuming client sends it for the
/// generation it acknowledged, and the host matches it against the baselines
/// it kept; the two ends compute it the same way from what they hold.
///
/// - Parameters:
///   - rowHashes: Each screen's row hashes, primary first.
///   - blockHashes: Each state block's hash.
/// - Precondition: Each screen has at most `maxRows` hashes.
/// - Returns: The digest.
[[nodiscard]] Digest128 screenHashesDigest(const std::array<std::span<const std::uint64_t>, 2>& rowHashes, const StateBlockHashes& blockHashes);

/// The digest of what the host sent an attachment.
///
/// - Parameter baseline: The baseline.
/// - Returns: `screenHashesDigest` of its rows and blocks.
[[nodiscard]] Digest128 baselineDigest(const AttachmentBaseline& baseline);

/// The digest of what a client holds of a pane.
///
/// - Parameter replica: The replica.
/// - Returns: `screenHashesDigest` of its rows and blocks.
[[nodiscard]] Digest128 replicaDigest(const ScreenReplica& replica);

/// Reads bounded geometry through the checkpoint decoder's header parser,
/// without allocating or validating the remaining body.
///
/// - Parameter body: A checkpoint's borrowed wire body.
/// - Returns: The cell bytes for both screens, or null for an invalid header.
[[nodiscard]] std::optional<std::size_t> checkpointCellStorageBytes(std::span<const std::uint8_t> body);

/// Validated checkpoint storage requests, independent of a destination model.
/// Every materialized array and string reserves its exact planned capacity.
/// Component fields overlap where stated; `preparationStorageCost` combines
/// the distinct owners rather than summing overlapping component counts.
struct CheckpointStoragePlan {
    int rows = 0;
    int columns = 0;
    std::size_t screenCellBytes = 0;
    std::size_t rowAttributeBytes = 0;
    std::size_t rowHashBytes = 0;
    /// The immutable replica header and exact initial pen/hash capacities;
    /// includes `rowHashBytes` rather than adding to it.
    std::size_t replicaStorageBytes = 0;
    std::size_t penCount = 0;
    std::size_t linkCount = 0;
    std::size_t linkTextBytes = 0;
    /// The complete private hyperlink ring moved into the Terminal at commit.
    std::size_t linkTableBytes = 0;
    std::size_t clusterCount = 0;
    /// The complete cluster pool's entry, index and bucket array capacities.
    std::size_t clusterStorageBytes = 0;
    std::size_t clusterCodePoints = 0;
    std::size_t runTextCount = 0;
    std::size_t runTextSlots = 0;
    std::size_t runTextCodePoints = 0;
    std::size_t richTextDefinitionCount = 0;
    std::size_t richTextCodePoints = 0;
    std::size_t richTextRuns = 0;
    std::size_t richTextFonts = 0;
    std::array<std::size_t, 2> richTextBlocks {};
    std::size_t placedRichTextCodePoints = 0;
    std::size_t placedRichTextRuns = 0;
    std::size_t placedRichTextFonts = 0;
    std::size_t placedRichTextRectangles = 0;
    std::size_t stateStringBytes = 0;
    std::size_t stateStringCount = 0;
    std::size_t activeContexts = 0;
    std::size_t statusRecords = 0;
    bool hasEndedContext = false;
    bool hasUserDefinedKeys = false;
    std::size_t userDefinedKeysBytes = 0;
    std::size_t alignedTabBytes = 0;
    std::size_t paletteStackBytes = 0;
    bool hasStatusRow = false;
    /// The state-only scratch Terminal. It owns neither a grid nor a parser
    /// buffer; validation leaves its input-sized owners empty.
    std::size_t validationScratchBytes = 0;
    /// The sum of exact requested capacities for private preparation owners
    /// and decoding temporaries, including nonempty strings' terminators.
    /// Summing owners whose lifetimes do not overlap makes this an upper bound
    /// rather than the minimum peak. Stack scratch and allocator bookkeeping,
    /// the borrowed body, and previously retained model owners are excluded.
    std::size_t preparationStorageCost = 0;

    bool operator==(const CheckpointStoragePlan&) const = default;
};

/// Validates the complete canonical grammar and state digest without creating
/// input-sized containers or decoded screen arrays. Fixed scratch model owners
/// allocate fallibly and are released before return; this is not zero-allocation
/// validation. All input-sized owners remain empty during this walk.
[[nodiscard]] std::expected<CheckpointStoragePlan, ApplyOutcome> checkpointStoragePlan(std::span<const std::uint8_t> body);

/// Limits a checkpoint's cell arrays and complete requested preparation storage.
///
/// Zero admits nothing. Storage cost includes the destination backing peak,
/// with every preparation container reserving exact capacities fallibly.
/// Allocator bookkeeping and owners retained outside this checkpoint are not
/// part of this per-checkpoint allowance.
struct CheckpointPreparationLimits {
    std::size_t maxDecodedScreenCellBytes = std::numeric_limits<std::size_t>::max();
    std::size_t maxStorageCost = std::numeric_limits<std::size_t>::max();
};

/// Adds the destination's exact old/new backing peak to preparation storage
/// with checked arithmetic, without reserving or changing the model.
///
/// - Parameters:
///   - plan: A successful `checkpointStoragePlan` result.
///   - destination: The terminal that would receive the checkpoint.
/// - Returns: Admission cost, or refusal for a missing grid or overflow.
[[nodiscard]] std::expected<std::size_t, ApplyOutcome> checkpointStorageCost(const CheckpointStoragePlan& plan, const Ubiquitty::Terminal& destination);

/// Owns a fully decoded and digest-checked checkpoint independently of its
/// wire body. Destination mutation is deferred until `commitCheckpoint`.
/// Every decoded cell, dictionary, text, activity, rich-text, and scratch owner
/// allocates fallibly at its planned exact capacity.
class PreparedCheckpoint {
public:
    /// Releases an uncommitted checkpoint without publishing its state.
    ~PreparedCheckpoint();
    PreparedCheckpoint(const PreparedCheckpoint&) = delete;
    PreparedCheckpoint& operator=(const PreparedCheckpoint&) = delete;
    /// Transfers the decoded owner, leaving its source empty.
    ///
    /// - Parameter other: The owner to consume.
    PreparedCheckpoint(PreparedCheckpoint&& other) noexcept;
    /// Replaces the decoded owner without committing the previous one.
    ///
    /// - Parameter other: The owner to consume.
    /// - Returns: This owner.
    PreparedCheckpoint& operator=(PreparedCheckpoint&& other) noexcept;

    /// Reports the validated screen generation.
    ///
    /// The owner must not have been moved or committed.
    /// - Returns: The checkpoint's generation.
    [[nodiscard]] std::uint64_t generation() const;
    /// The validated number of rows; requires an unconsumed owner.
    [[nodiscard]] int rows() const;
    /// The validated number of columns; requires an unconsumed owner.
    [[nodiscard]] int columns() const;
    /// Reports the validated history epoch.
    ///
    /// The owner must not have been moved or committed.
    /// - Returns: The checkpoint's history epoch.
    [[nodiscard]] std::uint32_t historyEpoch() const;
    /// Reports the validated history head.
    ///
    /// The owner must not have been moved or committed.
    /// - Returns: The first logical line beyond checkpoint history.
    [[nodiscard]] std::uint64_t historyHead() const;
    /// Reports the verified screen digest.
    ///
    /// The owner must not have been moved or committed.
    /// - Returns: The digest of the checkpoint's rows and state blocks.
    [[nodiscard]] Digest128 stateDigest() const;

private:
    struct Data;
    explicit PreparedCheckpoint(std::unique_ptr<Data>);
    std::unique_ptr<Data> m_data;

    friend std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t>);
    friend std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t>, std::size_t);
    friend std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t>, const Ubiquitty::Terminal&, CheckpointPreparationLimits);
    friend std::expected<CheckpointStoragePlan, ApplyOutcome> checkpointStoragePlan(std::span<const std::uint8_t>);
    friend ApplyOutcome reserveCheckpointDestination(Ubiquitty::Terminal&, const PreparedCheckpoint&);
    friend ApplyOutcome commitCheckpoint(Ubiquitty::Terminal&, PreparedCheckpoint, ScreenReplica&);
};

/// Decodes and validates one checkpoint without touching a destination.
///
/// - Parameter body: Borrowed only during this call.
/// - Returns: An independent owner or the validation/allocation refusal.
[[nodiscard]] std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body);

/// Admits both exact decoded screen-cell arrays before any preparation allocation.
///
/// The allowance covers only `2 * rows * columns * sizeof(Cell)`. It excludes
/// status-row storage, dictionaries, state blocks, scratch terminals, destination
/// grid backing and all history owners; it is not a complete preparation bound.
/// The geometry and body grammar are identical to the unrestricted overload.
/// - Parameters:
///   - body: Borrowed only during this call.
///   - maxDecodedScreenCellBytes: Combined capacity allowed for the two arrays.
/// - Returns: An independent owner or the validation/allocation refusal. An
///   insufficient allowance refuses before any preparation allocation; later
///   validation or allocation failure may discard privately allocated owners.
[[nodiscard]] std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body, std::size_t maxDecodedScreenCellBytes);

/// Validates and admits every preparation family plus destination backing
/// before materializing input-sized containers. Validation itself uses bounded
/// fixed scratch owners. A refused admission leaves destination state intact.
///
/// - Parameters:
///   - body: Borrowed only during this call.
///   - destination: The destination whose backing growth is admitted.
///   - limits: Independent cell-array and combined storage-cost allowances.
/// - Returns: A prepared owner or validation/allocation refusal. Commit checks
///   the destination backing cost again before requesting any growth.
[[nodiscard]] std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body, const Ubiquitty::Terminal& destination, CheckpointPreparationLimits limits);

/// Reserves admitted grid growth before a caller retires prior history.
///
/// The prepared owner is retained. Refusal leaves geometry and logical cells
/// intact; success changes capacity only, so history can still be preserved if
/// another caller-owned precondition subsequently refuses the checkpoint.
[[nodiscard]] ApplyOutcome reserveCheckpointDestination(Ubiquitty::Terminal& terminal, const PreparedCheckpoint& checkpoint);

/// Reserves destination grid capacity and consumes a prepared checkpoint.
///
/// - Parameters:
///   - terminal: The exact destination, unchanged if grid capacity is refused.
///   - checkpoint: The validated owner, consumed even on refusal.
///   - replica: Replaced with the checkpoint's original dictionary and hashes.
/// - Returns: Applied or allocation refusal. History retirement is the
///   caller's responsibility and is not invoked by this operation.
[[nodiscard]] ApplyOutcome commitCheckpoint(Ubiquitty::Terminal& terminal, PreparedCheckpoint checkpoint, ScreenReplica& replica);

/// Applies a checkpoint body to a passive terminal.
///
/// - Parameters:
///   - terminal: The client's terminal; untouched unless the result is `Applied`.
///   - body: The checkpoint body.
///   - replica: Replaced by what the checkpoint establishes when it applies.
/// - Returns: The outcome.
[[nodiscard]] ApplyOutcome applyCheckpoint(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica);

/// Applies a checkpoint after admitting its complete preparation and grid peak.
///
/// - Parameters:
///   - terminal: The destination; untouched unless the result is `Applied`.
///   - body: The checkpoint body, borrowed only during this call.
///   - replica: Replaced only after the bounded preparation and commit succeed.
///   - limits: Independent decoded-cell and complete checkpoint storage bounds.
/// - Returns: Applied, or validation/allocation refusal without publication.
[[nodiscard]] ApplyOutcome applyCheckpoint(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica, CheckpointPreparationLimits limits);

/// Draws one terminal's picture into another as a checkpoint of it, so the
/// destination shows exactly what the source does: how a client moves a
/// picture it holds into a terminal a frontend names.
///
/// - Parameters:
///   - source: The terminal the picture is in.
///   - destination: The terminal it is drawn into; its host answers it.
///   - limits: The client's bounds on the checkpoint's decoded cells.
/// - Returns: Whether the destination now holds the whole picture; on
///   refusal it is unchanged.
[[nodiscard]] bool copyPicture(const Ubiquitty::Terminal& source, Ubiquitty::Terminal& destination, CheckpointPreparationLimits limits);

/// Applies an update body to a passive terminal.
///
/// - Parameters:
///   - terminal: The client's terminal; untouched unless the result is `Applied`.
///   - body: The update body.
///   - replica: Advanced to what the update establishes when it applies.
///   - refusedRowHashes: When not null and the result is `DigestMismatch`,
///     receives the active screen's row hashes the update built, so the host
///     can name the first row its own disagree with.
/// - Returns: The outcome.
[[nodiscard]] ApplyOutcome applyRowUpdate(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica, std::vector<std::uint64_t>* refusedRowHashes = nullptr);

/// Writes a terminal's palette block: the bytes an update's `Block` op carries
/// for `StateBlock::Palette`, which a `PanePalette` frame carries alone to a
/// pane its client holds `Dormant` (Docs/sync.md §6.8).
///
/// - Parameters:
///   - terminal: The host's terminal.
///   - block: Cleared, then filled.
void encodePaletteState(const Ubiquitty::Terminal& terminal, std::vector<std::uint8_t>& block);

/// Applies a palette block to a client's terminal, as an update's `Block` op
/// does, and moves the replica's palette hash with it, and its state digest
/// where it holds the terminal's picture, so the replica still describes what
/// the terminal holds.
///
/// - Parameters:
///   - terminal: The client's terminal; untouched unless the result is `Applied`.
///   - block: The block's bytes.
///   - replica: The pane's replica.
/// - Returns: `Applied`, `Malformed` for bytes that are no palette block, or
///   `AllocationRefused` when the saved palettes' storage was refused.
[[nodiscard]] ApplyOutcome applyPaletteState(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> block, ScreenReplica& replica);

} // namespace Ubiquitty::Protocol::Sync
