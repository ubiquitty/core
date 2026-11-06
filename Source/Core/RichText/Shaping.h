// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/TypographyFacts.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Ubiquitty::RichText {

/// A native shaper receives complete styled text, including the context needed
/// for substitution, fallback, bidi ordering and language-aware line breaking.
struct UBIQUITTY_ESCAPABLE ShapeRequest {
    std::uint16_t width = 1;
    std::uint16_t height = 1;
    Overflow overflow = Overflow::Wrap;
    Alignment alignment = Alignment::Left;
    LineMode lineMode = LineMode::Natural;
    /// The content insets, in points: lines wrap at the width they leave.
    Insets padding {};
    /// A logical scalar anchor whose containing line starts a retained viewport.
    std::uint16_t viewportAnchor = 0;
};

/// A shaped line in logical source order; positions use 1/4096 point.
struct UBIQUITTY_ESCAPABLE ShapedLine {
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    std::uint32_t top = 0;
    std::uint32_t height = 0;
    std::uint32_t baseline = 0;
    std::uint32_t origin = 0;
    std::uint32_t width = 0;
};

/// One selectable grapheme, with the two carets supplied by the shaper.
/// Leading can be to the right of trailing in a right-to-left run.
struct UBIQUITTY_ESCAPABLE ShapedSpan {
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    std::uint32_t line = 0;
    std::int32_t leading = 0;
    std::int32_t trailing = 0;
};

/// Portable geometry, shared by fit replies, hit testing and drawing.
struct ShapedText {
    std::vector<ShapedLine> lines;
    std::vector<ShapedSpan> spans;
    /// Native geometry of a visible source prefix followed by U+2026. The
    /// original geometry still describes the complete source for measurement.
    /// Only one level is allowed; elisionEnd excludes the synthetic ellipsis.
    std::shared_ptr<ShapedText> elision {};
    std::uint32_t elisionEnd = 0;

    /// Appends a line in logical source order across the Swift interop boundary.
    ///
    /// - Parameter line: The native line geometry in 1/4096 point.
    void appendLine(ShapedLine line) { lines.push_back(line); }
    /// Appends a selectable grapheme across the Swift interop boundary.
    ///
    /// - Parameter span: The logical range and its visual caret edges.
    void appendSpan(ShapedSpan span) { spans.push_back(span); }
};

/// A borrowed frontend service. It must not enter the UI thread: callers hold
/// the terminal model lock. A null function means shaping is unavailable.
struct Shaper {
    using Function = bool (*)(void*, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText*);
    Function function = nullptr;
    void* context = nullptr;
};

/// An application measurement sent to the attachment that owns the fonts.
struct MeasureRequest {
    std::uint32_t token = 0;
    std::uint16_t id = 0;
    ShapeRequest shape;
    Document document;
    bool needsGeometry = false;
    /// The exact native renderer publication requested across Sync.
    std::uint64_t rendererRevision = 1;
    /// Catalog pages share measurement IDs, admission, deadlines and cancellation.
    std::optional<FontCatalogQuery> fontCatalog {};
};

/// Font-derived geometry returned to the application. No raw terminal bytes
/// from an attachment are forwarded to the program.
struct Measurement {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t shown = 0;
    std::uint32_t capacity = 0;
    std::uint32_t lineCapacity = 0;
    std::uint32_t epoch = 0;
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint16_t baseline = 0;
    std::uint16_t fontSize = 0;
    std::uint32_t lineHeight = 0;
    std::uint32_t lineBaseline = 0;
    /// The narrowest width, in cells, that breaks no word: the widest run of
    /// text between two break opportunities.
    std::uint32_t minWidth = 0;
    bool isFitting = false;
    bool isExact = false;
    bool isShaped = false;
    std::vector<std::uint32_t> lineEnds;
    ShapedText geometry;
    /// The native renderer publication that produced this result.
    std::uint64_t rendererRevision = 1;
    std::optional<FontCatalogPage> fontCatalog {};
};

inline constexpr std::size_t maxPendingMeasurements = 32;
/// An application's native query has one deadline, including queueing time.
inline constexpr std::uint32_t maxMeasurementWaitMilliseconds = 2000;
inline constexpr std::uint32_t maxMeasurementWaitTicks = Ticks::fromMilliseconds(maxMeasurementWaitMilliseconds).count;

/// The terminal outcome of a measurement that produced no geometry.
enum class MeasurementFailure : std::uint8_t {
    None,
    Timeout,
    Cancelled,
    Duplicate,
    Stale
};

/// A bounded measurement awaiting the font-owning attachment until its deadline.
struct PendingMeasurement {
    MeasureRequest request;
    std::uint32_t recipient = 0;
    std::uint64_t expiresAt = 0;
    std::uint32_t blockSerial = 0;
    std::uint32_t typographyEpoch = 0;
    bool wantsStatus = false;
    /// The terminal clock at admission; retries never restart this clock.
    Ticks admittedAt {};
    /// A completed result waits here until its atomic reply fits the output queue.
    std::optional<Measurement> completion {};
    MeasurementFailure failure = MeasurementFailure::None;
};

/// Returns the unspent part of an application's measurement deadline.
/// - Parameters:
///   - pending: The admitted application query.
///   - now: The terminal's current tick clock.
/// - Returns: Microseconds remaining, or zero once the deadline has passed.
[[nodiscard]] inline std::uint64_t measurementWaitMicroseconds(const PendingMeasurement& pending, Ticks now)
{
    const auto elapsed = now.since(pending.admittedAt);
    return elapsed >= maxMeasurementWaitTicks ? 0 : static_cast<std::uint64_t>(maxMeasurementWaitTicks - elapsed) * 1'000'000 / 60;
}

} // namespace Ubiquitty::RichText
