// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Search — what a find request asks and what it finds.
//
// A hit names its first character by logical position and its extent in
// written cells of the same line, so it keeps naming the same text while the
// rows under it are renumbered, and resolves to an explicit failure once the
// text is gone. A match never crosses a hard line end, so one line holds it.

#include "Core/CaseSensitivity.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/Unicode/Normalize.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Ubiquitty::Search {

/// How a request's pattern reads.
enum class PatternForm : std::uint8_t {
    /// The pattern is text to find as written.
    Literal,
    /// The pattern is an RE2 expression.
    Regex,
};

/// An inclusive range of logical lines, attached to its boundary text.
struct Scope {
    Screen::LogicalPosition first;
    Screen::LogicalPosition last;

    friend bool operator==(const Scope&, const Scope&) = default;
};

/// A find request.
struct Request {
    std::string pattern;
    PatternForm form = PatternForm::Literal;
    CaseSensitivity sensitivity = CaseSensitivity::Insensitive;
    /// The `[unicode]` form both pattern and text are compared in; None
    /// compares in NFC, so canonically equivalent text always matches.
    Unicode::NormalizationForm normalization = Unicode::NormalizationForm::None;
    /// No value searches the whole buffer. A destroyed boundary empties the scope.
    std::optional<Scope> scope = std::nullopt;
    /// The most result labels a palette asks to read; zero requests none.
    std::size_t labelLimit = 0;

    friend bool operator==(const Request&, const Request&) = default;
};

/// One match: written cells `[start.cell, endCell)` of the line `start` names.
struct Hit {
    Screen::LogicalPosition start;
    std::uint32_t endCell = 0;
};

/// A palette row and the durable position its action reveals.
struct Label {
    Screen::LogicalPosition position;
    std::string text {};
    int absoluteRow = 0;
    int column = 0;
};

/// The most labels one publication exposes to a palette.
inline constexpr std::size_t maxStoredLabels = 200;

/// The most code points a label copies from its display row.
inline constexpr std::size_t maxLabelCodePoints = 1024;

/// Where a search stands, as last published.
struct Status {
    /// The request the status answers; zero before any.
    std::uint64_t generation = 0;
    /// The screen result publication, independent of query and focus changes.
    std::uint64_t screenGeneration = 0;
    /// Matches found so far; exact once `isComplete`, and still counting past
    /// what the hit store holds.
    std::int64_t foundCount = 0;
    /// The active hit's query-wide index, newest first; -1 with none.
    std::int64_t activeIndex = -1;
    /// Bumped on every change of the active hit.
    std::uint32_t activeEpoch = 0;
    /// Bumped when the active hit should be brought into view: a frontend
    /// scrolls only when this moves, so a scan finding more never yanks it.
    std::uint32_t revealEpoch = 0;
    /// The view offset that reveals the active hit, or -1 with none.
    std::int32_t revealViewOffset = -1;
    bool isComplete = false;
    bool isPatternInvalid = false;
    /// More matches were found than the hit store holds.
    bool isTruncated = false;

    friend bool operator==(const Status&, const Status&) = default;
};

/// The most hits one navigation window keeps, independent of the exact count.
inline constexpr std::size_t maxStoredHits = 16384;

/// Which way a step moves the active hit.
enum class StepDirection : std::uint8_t {
    /// Toward older text.
    Older,
    /// Toward newer text.
    Newer,
};

} // namespace Ubiquitty::Search
