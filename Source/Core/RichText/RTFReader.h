// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/RTFReader.h — the RTF subset a rich text block accepts, read into a
// `Document` (Docs/rich-text.md, "The RTF subset").
//
// UTF-8 validation and one parsing pass, no recursion: group state is a fixed stack of
// `maxGroupDepth` entries, and the only allocations are the document's text,
// run and font vectors. Unknown mandatory controls and every exceeded bound refuse
// the whole body. Optional destinations still receive structural validation.

#include "Core/RichText/Document.h"

#include <cstdint>
#include <expected>
#include <span>

namespace Ubiquitty::RichText {

/// Why a body was refused.
enum class ReadError : std::uint8_t {
    /// The body does not open with `{\rtf1`.
    NotRTF,
    /// Groups, escapes, numeric parameters, or raw controls are malformed.
    Malformed,
    /// The body or a font-family name is not complete, shortest-form UTF-8.
    InvalidUTF8,
    /// A mandatory control or a legacy text-encoding control is unsupported.
    UnsupportedControl,
    /// The body is longer than ``maxBodyBytes``.
    BodyTooLong,
    /// Groups nest deeper than ``maxGroupDepth``.
    TooDeep,
    /// The text reaches ``maxCodePoints``.
    TooMuchText,
    /// The styles change more than ``maxRuns`` times.
    TooManyRuns,
    /// The font table has more than ``maxFonts`` entries.
    TooManyFonts,
    /// A font-family identifier exceeds ``maxFamilyNameBytes``.
    FamilyNameTooLong,
    /// The colour table has more than ``maxColors`` entries.
    TooManyColors,
    /// A paragraph's spacing or indent is outside its range.
    ParagraphOutOfRange,
    /// The bounded decoded document could not obtain its storage.
    AllocationRefused,
};

/// Reads an RTF body into a document.
///
/// The OSC profile uses UTF-8 exclusively. Exactly one complete root group
/// is required; only ASCII whitespace may follow its closing brace.
///
/// - Parameter body: The bytes after the code's second semicolon.
/// - Returns: The document, or why the body was refused.
[[nodiscard]] std::expected<Document, ReadError> readRTF(std::span<const std::uint8_t> body);

} // namespace Ubiquitty::RichText
