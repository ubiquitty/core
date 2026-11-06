// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// OSC/RichTextBlocks.h — OSC 7404, rich text blocks (Docs/rich-text.md): the
// keys a code carries and the handler that places, queries and deletes.

#include "Core/RichText/Blocks.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::OSC::RichTextBlocks {

/// What a code asks for.
enum class Action : std::uint8_t {
    Place,
    Query,
    Delete,
    Subscribe,
    Cancel,
    FontCatalog,
};

/// One code's parsed keys.
struct Request {
    Action action = Action::Place;
    /// The block's geometry and options; the query reads its width, height,
    /// overflow and alignment, and its id as the request id.
    RichText::Placement placement;
    /// Refuses a query or placement if its measurement authority has changed.
    std::optional<std::uint32_t> expectedTypographyEpoch;
    bool needsGeometry = false;
    bool wantsStatus = false;
    bool shouldSubscribe = false;
    /// Whether the code named `i`; a delete without one removes every block.
    bool hasID = false; // bool-ok: whether a key was present, read once by the delete
    RichText::FontCatalogQuery fontCatalog;
};

/// Parses a code's keys: colon-separated `key=value` pairs.
///
/// Strict, as OSC 66's keys are: an unknown key, a pair without `=`, an empty
/// or out-of-range value, a repeated key, a key the action does not take, or
/// a place or query without `w` and `h` refuses the whole code.
///
/// - Parameter keys: The bytes between the code's first two semicolons.
/// - Returns: The request, or nothing on refusal.
[[nodiscard]] std::optional<Request> parseKeys(std::string_view keys);

/// Handles one OSC 7404 code.
///
/// - Parameters:
///   - t: The terminal.
///   - payload: The bytes after `7404;`.
void handle(Ubiquitty::Terminal& t, std::string_view payload);

/// Selects whether a measurement reply includes grapheme caret packets.
enum class GeometryReport : std::uint8_t {
    Omit,
    Include
};

/// Publishes changed font metrics to the application's explicit subscription.
///
/// - Parameters:
///   - terminal: The terminal whose typography publication changed.
void typographyChanged(Ubiquitty::Terminal& terminal);

/// Retires application queries whose admission deadline has elapsed.
/// - Parameters:
///   - terminal: The terminal owning the queries.
///   - now: The current terminal clock, advanced even without PTY output.
void expireMeasurements(Ubiquitty::Terminal& terminal, Ticks now);

/// Stores a native result until its complete application reply can be published.
/// - Parameters:
///   - terminal: The terminal owning the query.
///   - token: The outstanding host measurement token.
///   - measurement: The validated geometry from the selected renderer.
/// - Returns: Whether a current unanswered application query accepted the result.
[[nodiscard]] bool completeMeasurement(Ubiquitty::Terminal& terminal, std::uint32_t token, RichText::Measurement measurement);

/// Publishes finished measurement outcomes and the newest typography notification.
/// Queue pressure retains each outcome for a later call without partial packets.
/// - Parameter terminal: The terminal whose program reads the reports.
void publishMeasurements(Ubiquitty::Terminal& terminal);

/// Emits validated measurement fields, including complete source line ends.
///
/// - Parameters:
///   - terminal: The program's terminal.
///   - id: The program's query identifier.
///   - measurement: Validated frontend metrics.
///   - geometry: Whether to emit native caret geometry before the final reply.
[[nodiscard]] bool answerMeasurement(Ubiquitty::Terminal& terminal, std::uint16_t id, const RichText::Measurement& measurement, GeometryReport geometry = GeometryReport::Omit);

} // namespace Ubiquitty::OSC::RichTextBlocks
