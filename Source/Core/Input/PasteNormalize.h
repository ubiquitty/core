// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable paste rewrite: CR/LF normalisation and unconditional neutralisation of
// any embedded bracketed-paste fence (ESC[200~ / ESC[201~) so a paste can never
// toggle the target's paste mode. Implemented in Core/Input/PasteNormalize.cpp.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Input {

/// Whether the receiving terminal enabled bracketed-paste mode (DECSET 2004).
enum class BracketedPaste {
    Disabled,
    Enabled,
};

/// Readline's character quoting (2005) and literal newline (2006) modes.
struct PasteOptions {
    bool shouldQuoteCharacters = false;
    bool shouldPreserveNewlines = false;
    bool operator==(const PasteOptions&) const = default;
};

/// Rewrites a paste buffer in place and returns its new length.
///
/// The result is never longer than the input. `bracketedPaste` selects the same
/// C0/DEL sanitisation ``wrapPaste`` applies — controls barring TAB and ESC,
/// and DEL, stripped only when Disabled — and tells the caller whether to wrap.
///
/// Fence neutralisation is always on and matches ``wrapPaste``, so this
/// followed by ``wrapPaste`` yields the same bytes as ``wrapPaste`` alone. Two
/// spellings of that sanitisation would let a payload survive one path and not
/// the other.
///
/// - Parameters:
///   - buffer: The bytes to rewrite, modified in place.
///   - length: How many bytes `buffer` holds.
///   - bracketedPaste: Whether the target enabled DECSET 2004.
/// - Returns: The new length, at most `length`.
long normalizePaste(char* buffer, long length, BracketedPaste bracketedPaste);

/// Returns the bytes to transmit for a paste.
///
/// Embedded paste fences are neutralised. Unless literal newlines are enabled,
/// CR, LF, CRLF and LFCR become one CR. Character quoting prefixes each payload
/// byte with Ctrl-V; otherwise unfenced C0 controls except TAB/ESC and DEL are
/// stripped. Generated brackets are never quoted.
///
/// - Parameters:
///   - payload: The clipboard text.
///   - bracketedPaste: Whether to surround it with paste brackets.
///   - options: Readline quoting and newline modes.
/// - Returns: The bytes to write to the program.
std::string wrapPaste(std::string_view payload, BracketedPaste bracketedPaste, PasteOptions options = {});

/// Normalises a paste that arrives in pieces, as `normalizePaste` does a
/// whole one: the pieces' output, joined, is the whole buffer's.
///
/// A CR or LF at a piece's end waits for the next byte, which may pair with
/// it, and a piece ending in what may be the start of a bracketed-paste
/// fence waits until the fence is complete or cannot be, so a fence split
/// across pieces is neutralised as one inside a piece is. What waits is at
/// most five bytes.
class PasteNormalizer final {
public:
    /// The exact undecided suffix, not yet normalized or submitted to a sink.
    struct Continuation {
        BracketedPaste bracketedPaste = BracketedPaste::Disabled;
        PasteOptions options;
        std::array<char, 5> held {};
        std::uint8_t heldBytes = 0;
        bool operator==(const Continuation&) const = default;
    };

    /// Copies the bounded normalization state without finishing the paste.
    /// - Returns: Exact state, or nothing if the suffix invariant is broken.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Checks that the suffix can still be awaiting a following input byte.
    /// - Parameter state: The portable candidate.
    /// - Returns: Whether its mode and exact prefix are valid.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state);

    /// Restores the original mode and suffix without emitting any bytes.
    /// - Parameter state: The portable candidate.
    /// - Returns: True on replacement; refusal leaves this normalizer unchanged.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    /// Creates a normalizer for one paste.
    ///
    /// - Parameters:
    ///   - bracketedPaste: Whether the target enabled DECSET 2004.
    ///   - options: The readline modes held for this paste.
    explicit PasteNormalizer(BracketedPaste bracketedPaste, PasteOptions options = {})
        : m_bracketedPaste(bracketedPaste)
        , m_options(options)
    {
    }

    /// Normalises the next piece.
    ///
    /// - Parameter piece: The paste's next bytes.
    /// - Returns: The bytes now known, which may lag the piece by the few it
    ///   holds back.
    [[nodiscard]] std::string push(std::string_view piece);

    /// Ends the paste.
    ///
    /// - Returns: The bytes held back, normalised.
    [[nodiscard]] std::string finish();

private:
    BracketedPaste m_bracketedPaste;
    PasteOptions m_options;
    std::string m_held;
};

/// How many lines a paste would type, for the multi-line confirmation.
///
/// CRLF is one break, not two: the LF directly after a CR is the tail of a pair
/// already counted. Text ending on a break has no trailing empty line, so
/// `"a\n"` is one line rather than two — a shell would run one command.
///
/// The count decides whether a paste is held for confirmation, so a second
/// spelling of the rule is a second answer to "is this dangerous".
///
/// - Parameter text: The clipboard text, before normalisation.
/// - Returns: The number of lines, zero for empty text.
int pasteLineCount(std::string_view text);

/// Escapes text so it pastes as a SINGLE shell argument.
///
/// POSIX single-quote scheme: wrap in `'…'` and rewrite every embedded single
/// quote as `'\''`, so no metacharacter is interpreted. Empty input yields `''`
/// rather than nothing, which would drop the argument instead of passing an
/// empty one.
///
/// The caller still routes the result through ``wrapPaste``: this handles shell
/// syntax, not CR/LF or fences.
///
/// - Parameter text: The text to escape.
/// - Returns: A single quoted shell word.
std::string shellEscape(std::string_view text);

} // namespace Ubiquitty::Input
