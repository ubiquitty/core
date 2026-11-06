// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Core/Screen/CommandBlocks.h — the OSC 133 command history as a list.
//
// rowMark, resolveCommandRegion and absoluteRowText each answer a question
// about one row. A command palette wants the whole history, and a frontend that
// walks those primitives itself has to settle the same six things: which way to
// scan, how many blocks to keep, how a command spanning several rows becomes
// one label, what happens to the whitespace between them, what an executed
// prompt with no text means, and how many rows the alternate screen addresses.
// Six answers a second frontend would otherwise guess at, which is why the walk
// lives here and not above the seam.
//
// Rows are ABSOLUTE throughout, in the space Addressing.h defines:
// [0, scrollbackCount) is history, the rest is the live screen.

#include <string>
#include <vector>

namespace Ubiquitty {

/// The terminal model a walk reads.
///
/// Declared rather than included so this header stays free of the model's
/// include graph and imports into a frontend language on its own.
struct Terminal;

namespace Screen {

/// The number of blocks a palette-sized enumeration stops at.
///
/// A long session holds thousands of prompts, and both the list and the fuzzy
/// ranking over it are paid per block on every keystroke. The bound belongs to
/// the model so that two frontends offering "the command blocks" offer the
/// same ones.
constexpr int maxCommandBlocks = 300;

/// How many output rows one command block's text is read at most.
///
/// A bound rather than a limit anyone should reach: a command that printed more
/// than this has output nobody is reading in a palette row, and the alternative
/// is a read proportional to the scrollback, which a user-configurable million
/// lines makes unbounded.
constexpr int maxCommandOutputRows = 5000;

/// How many rows one command block's label is read from at most.
///
/// The label spans the prompt row through the row before output begins, and that
/// span is delimited by two marks the program prints. A program that emits
/// `OSC 133;A`, then text, then `OSC 133;C` decides how far apart they are, so
/// the span is whatever it says — up to the whole scrollback, which the user may
/// set to a million lines.
///
/// The sibling bound already exists for the output half (``maxCommandOutputRows``)
/// and the same reasoning applies here with less room: this is one palette row's
/// title, and the fuzzy ranking re-reads it on every keystroke. Sixty-four rows
/// covers a prompt's information lines plus a command wrapped several times over
/// at any usable width; past that the text is not a command line, whatever the
/// marks claim.
constexpr int maxCommandTitleRows = 64;

/// One executed OSC 133 command: what was typed, where it sits, how it ended.
struct CommandBlock {
    /// The typed command line as a single-line label.
    ///
    /// A command occupies every row from its prompt row through the row before
    /// its output begins, which is more than one whenever the prompt prints an
    /// information line above the command or the command wraps at the right
    /// margin. Those rows arrive joined, with every run of whitespace collapsed
    /// to a single space and the ends trimmed, so the label does not carry the
    /// column count that happened to be in force when the command ran. Never
    /// empty — a prompt whose rows hold no text is not enumerated at all.
    std::string command;

    /// The absolute row carrying the prompt's `osc133MarkCmdExecuted` mark.
    ///
    /// The identity of the block: what a caller scrolls to, and what
    /// `resolveCommandRegion` re-derives the rest of the region from.
    int promptRow = -1;

    /// Whether the command reported a non-zero exit status.
    ///
    /// How a failure reads — a glyph, a colour, a badge — is the frontend's
    /// choice, so the model reports the fact and names no presentation.
    bool didFail = false;
};

/// A block list under a name a frontend language can spell.
///
/// Swift/C++ interop cannot name a bare `std::vector<T>`, so the instantiation
/// carries an alias and the vector imports as a collection.
using CommandBlockList = std::vector<CommandBlock>;

/// Enumerates the executed OSC 133 command blocks, newest first.
///
/// Scans absolute rows downwards from the last, keeping every prompt row whose
/// mark carries `osc133MarkCmdExecuted`. A prompt whose command rows hold no
/// text is dropped rather than listed as a blank entry, and a dropped prompt
/// does not count against `maxBlocks` — a shell that marks a prompt it never
/// printed cannot push real commands off the end of the list.
///
/// While the alternate screen is up, history counts as zero, which bounds the
/// walk at the visible row count instead of at history plus the visible rows.
/// Row addressing keeps its own history regardless, so the rows inside that
/// bound resolve into the OLDEST retained lines rather than onto the alternate
/// screen: a session holding history enumerates its earliest commands there.
///
/// - Parameters:
///   - t: The terminal to read. A null terminal enumerates nothing.
///   - maxBlocks: How many blocks to return, counted from the newest. Zero or
///     less enumerates nothing; `maxCommandBlocks` is the palette's bound.
/// - Returns: The blocks, newest first, at most `maxBlocks` of them.
[[nodiscard]] CommandBlockList commandBlocks(const Ubiquitty::Terminal* t, int maxBlocks);

/// An inclusive absolute-row span, and whether there was one to report.
struct CommandOutputRows {
    /// The first absolute row of the span.
    ///
    /// One past the last row of the typed command line, so it is the row the
    /// command's output starts on.
    int from = -1;

    /// The last absolute row of the span.
    ///
    /// The end of the buffer: output belongs to the newest command until
    /// another prompt claims it, and none has.
    int to = -1;

    /// Whether a command block was found to report a span for.
    ///
    /// False means no shell integration has marked an executed prompt, which is
    /// a different answer from an empty span and the two must not be conflated:
    /// a frontend hides its scope control on the first and shows an empty
    /// result on the second.
    bool isFound = false;
};

/// Locates the output rows of the newest executed OSC 133 command block.
///
/// Only the newest block is considered: the first executed prompt found
/// scanning upwards decides the answer, and the scan stops there. An older
/// block's output is not the newest one's, so a second candidate would answer a
/// different question.
///
/// A found span always satisfies `from <= to`. A command occupying the buffer's
/// final row has produced no output rows, and that reports as not-found rather
/// than as a pair describing rows backwards — a caller building a range out of
/// an inverted pair does not get an empty range, it gets a trap.
///
/// - Parameter t: The terminal to read. A null terminal reports nothing found.
/// - Returns: The output span of the newest block, or a value whose `isFound`
///   is false when no executed prompt is marked or the block has yet to print.
[[nodiscard]] CommandOutputRows lastCommandOutputRows(const Ubiquitty::Terminal* t);

/// The text a command block printed, rows joined by newlines.
///
/// The output runs from the row after the command to the row before the next
/// executed prompt, or to the end of the buffer when none follows. Trailing
/// blank rows are dropped: a block whose output ended mid-screen would otherwise
/// carry the blank remainder of the buffer with it.
///
/// - Parameters:
///   - t: The terminal to read. A null terminal yields an empty string.
///   - absoluteRow: A row inside the block, in the same absolute space
///     `CommandBlock::promptRow` reports.
///   - maxRows: How many rows to read at most, so a runaway command cannot make
///     this proportional to the scrollback.
/// - Returns: The block's output, or empty when `absoluteRow` names no block.
[[nodiscard]] std::string commandBlockOutput(const Ubiquitty::Terminal* t, int absoluteRow, int maxRows);

} // namespace Screen
} // namespace Ubiquitty
