// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/RichTextForm.h — how a pane's rich text blocks travel (Docs/sync.md
// §7.4, Docs/rich-text.md "Synchronization").
//
// A block is two things on the wire: its placement, in the `RichText` state
// block beside its screen's other state, and its document, defined once per
// attachment by serial. A serial names one placement for the life of the host
// terminal, so a document an attachment holds is never redefined, and an
// update that moves or removes blocks carries only their placements.
//
//     RichTextBlock := count:varint Placement×count
//     Placement     := screen:u8 serial:varint id:u16 row:svarint column:u16
//                      width:u16 height:u16 overflow:u8 alignment:u8
//     Definition    := serial:varint Document
//     Document      := text:[varint] runs:[(length:varint halfPoints:u16 font:u8
//                      color:u8 styles:u8 alignment:u8)] fonts:[(class:u8 name:utf8)]
//                      colors:[(red:u8 green:u8 blue:u8 automatic:u8)]
//
// The elected font owner returns native geometry, including any ellipsis
// presentation, to the host for measurement, capture and hit testing.

#include "Core/Protocol/Sync/Wire.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Shaping.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

enum class ApplyOutcome : std::uint8_t;

/// The bounded assembled measurement, including all 4096 grapheme positions.
inline constexpr std::size_t maxRichTextMeasurementBytes = 512 * 1024;
/// Catalog results have fixed fields, one byte count and two bytes per name length.
inline constexpr std::size_t maxRichTextFontCatalogBytes = 20 + RichText::maxFontCatalogPageFamilies * 2 + RichText::maxFontCatalogPageBytes;

/// Native measurements correlated with one outstanding pane request.
struct RichTextMeasureReply {
    std::uint32_t handle = 0;
    std::uint32_t token = 0;
    RichText::Measurement measurement;
};

/// Writes a bounded native measurement request.
///
/// - Parameters:
///   - writer: The message body.
///   - request: The document and shaping constraints.
///   - rendererRevision: The selected pane's revision, or zero to use the request's.
void writeMeasureRequest(WireWriter& writer, const RichText::MeasureRequest& request, std::uint64_t rendererRevision = 0);
/// Reads a bounded native measurement request.
///
/// - Parameters:
///   - reader: The message body.
///   - request: Receives the validated request.
/// - Returns: Whether the body satisfies the measurement limits.
[[nodiscard]] bool readMeasureRequest(WireReader& reader, RichText::MeasureRequest& request);
/// Writes font-derived geometry without terminal escape bytes.
///
/// - Parameters:
///   - writer: The message body.
///   - value: The measurement to serialize.
void writeMeasurement(WireWriter& writer, const RichText::Measurement& value);
/// Reads bounded font-derived geometry.
///
/// - Parameters:
///   - reader: The message body.
///   - value: Receives the measurement.
/// - Returns: Whether all fields and line ranges satisfy their bounds.
[[nodiscard]] bool readMeasurement(WireReader& reader, RichText::Measurement& value);

/// One document an update or a checkpoint defines.
struct RichTextDefinition {
    std::uint32_t serial = 0;
    RichText::Document document;
};

/// Complete validated document bytes and their input-sized element counts.
/// The bytes remain borrowed; no text or run vector is materialized.
struct DocumentWireView {
    std::span<const std::uint8_t> bytes;
    std::size_t codePoints = 0;
    std::size_t runs = 0;
    std::size_t fonts = 0;
};

/// Reads the canonical document grammar into bounded validation scratch.
/// The returned view borrows bytes already consumed by the reader.
[[nodiscard]] std::optional<DocumentWireView> readDocumentView(WireReader& reader);

/// One immutable definition used to validate checkpoint placements without copies.
struct RichTextDefinitionView {
    std::uint32_t serial = 0;
    DocumentWireView document;
};

/// The exact document elements copied into a validated placement set.
struct RichTextPlacementStorage {
    std::array<std::size_t, 2> blocks {};
    std::size_t codePoints = 0;
    std::size_t runs = 0;
    std::size_t fonts = 0;
    std::size_t visibleRectangles = 0;
};

/// Validates a checkpoint's placements through the canonical placement reader.
[[nodiscard]] std::optional<RichTextPlacementStorage> richTextPlacementStorage(std::span<const std::uint8_t> bytes,
    int rows, int columns, std::span<const RichTextDefinitionView> definitions);

/// Writes a document.
///
/// - Parameters:
///   - writer: The body being written.
///   - document: The document.
void writeDocument(WireWriter& writer, const RichText::Document& document);

/// Reads a document, refusing one the RTF reader could not have produced.
///
/// - Parameters:
///   - reader: The body being read.
///   - document: Receives the document.
/// - Returns: False, with the reader failed, on a malformed document.
[[nodiscard]] bool readDocument(WireReader& reader, RichText::Document& document);

/// Reads a document into exact-capacity fallible owners.
[[nodiscard]] std::expected<void, ApplyOutcome> tryReadDocument(WireReader& reader, RichText::Document& document);

/// Writes the placements of both screens' blocks.
///
/// - Parameters:
///   - writer: The state block being written.
///   - blocks: The terminal's blocks.
void writeRichTextBlock(WireWriter& writer, const RichText::BlockStore& blocks);

/// Rebuilds a block store from a `RichText` state block.
///
/// Each placement takes its document from `definitions` or, failing that,
/// from the block with its serial in `held`. Refused when a placement lies
/// outside the geometry, repeats a serial, names a document neither side
/// holds, or the store would exceed a cap.
///
/// - Parameters:
///   - bytes: The state block.
///   - rows: The screen's rows.
///   - columns: The screen's columns.
///   - definitions: The documents the same body defines.
///   - held: The blocks the receiver already holds.
///   - blocks: Receives the rebuilt store.
/// - Returns: Whether the block was well formed.
[[nodiscard]] bool readRichTextBlock(std::span<const std::uint8_t> bytes, int rows, int columns, std::span<const RichTextDefinition> definitions,
    const RichText::BlockStore& held, RichText::BlockStore& blocks);

/// Validates placements before reserving their exact block capacities, then
/// clones documents and visible rectangles through fallible owners.
[[nodiscard]] std::expected<void, ApplyOutcome> tryReadRichTextBlock(std::span<const std::uint8_t> bytes, int rows, int columns,
    std::span<const RichTextDefinition> definitions, const RichText::BlockStore& held, RichText::BlockStore& blocks);

/// The serials of every block a store holds, ascending.
///
/// - Parameter blocks: The store.
/// - Returns: The serials.
[[nodiscard]] std::vector<std::uint32_t> richTextSerials(const RichText::BlockStore& blocks);

} // namespace Ubiquitty::Protocol::Sync
