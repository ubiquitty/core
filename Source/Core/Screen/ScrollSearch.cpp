// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// ScrollSearch.cpp — see ScrollSearch.h.
//
// RE2-backed matcher. compile() builds one immutable RE2 program per
// pattern (shared by every copy of the CompiledRegex handle); scanners
// hand it one logical line at a time, so
// the never-cross-a-hard-line-end guarantee is structural — the subject
// handed to RE2 contains no '\n' — and '^' / '$' anchor per line
// without (?m). RE2 is linear-time (no backtracking): a hostile pattern
// costs O(text), so no adversarial-pattern step budget is needed.
//
// Option choices, each pinned for the reason given — changing one changes a
// guarantee stated above:
//   EncodingUTF8     — the haystack is every row's text as UTF-8, each
//                      cell's cluster normalized as a unit (see matchingForm),
//                      and every byte maps to its origin cell, so a match of
//                      any script maps back to the cells it covers. Case
//                      folding is Unicode's, so a case-insensitive search
//                      finds Cyrillic, Greek and accented Latin as well as
//                      ASCII.
//   log_errors=false — patterns are user input; a typo in the find field
//                      must not spew to stderr.
//   max_mem=8<<20    — bounds the compiled-program + DFA memory for a
//                      hostile pattern (compile fails instead of ballooning).
//   never_capture    — group syntax works but nothing is captured; the API
//                      only ever reports whole-match spans.
//   longest_match stays OFF: leftmost-first greedy quantifiers (Perl
//   semantics), the behavior every find field on this platform teaches.
//

#include "Core/Screen/ScrollSearch.h"
#include "Core/CellPen.h"
#include "Core/LineStore.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/Unicode/Normalize.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "re2/re2.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {

// The compiled RE2 program behind a CompiledRegex handle. Defined here (the
// header only forward-declares it) so re2 headers stay out of every
// consumer's include graph.
struct ScrollSearchProgram {
    RE2 pattern;
    ScrollSearchProgram(const std::string& source, const RE2::Options& options)
        : pattern(source, options)
    {
    }
};

namespace Screen::ScrollSearch {

// The person's [unicode] normalization form, which copy applies as it is.
// Set once by the frontend at config load; a torn read can only misclassify
// one in-flight search, which the next search corrects.
static Ubiquitty::Unicode::NormalizationForm s_normalizationForm = Ubiquitty::Unicode::NormalizationForm::None;

// The form search compares text in: the configured one, or NFC when none is
// configured. Search always treats canonically equivalent text as equal, so a
// needle typed precomposed finds text a program wrote decomposed (macOS file
// names, for one); copy keeps the text exactly as written unless a form is set.
static Ubiquitty::Unicode::NormalizationForm matchingForm(Ubiquitty::Unicode::NormalizationForm configured)
{
    return configured == Ubiquitty::Unicode::NormalizationForm::None ? Ubiquitty::Unicode::NormalizationForm::NFC : configured;
}

void setNormalizationForm(Ubiquitty::Unicode::NormalizationForm form) { s_normalizationForm = form; }

Ubiquitty::Unicode::NormalizationForm matchingNormalizationForm(Ubiquitty::Unicode::NormalizationForm configured) { return matchingForm(configured); }

Ubiquitty::Unicode::NormalizationForm normalizationForm() { return s_normalizationForm; }

// Mirrors RE2::QuoteMeta: backslash-escape every byte outside [A-Za-z0-9_]
// below 0x80 — legal RE2 syntax for any ASCII punctuation byte, and the
// escape turns it into a literal. Bytes >= 0x80 (UTF-8 lead/continuation)
// ride through raw so multi-byte text stays a literal byte run.
std::string escapeLiteral(std::string_view literal)
{
    std::string out;
    out.reserve(literal.size());
    for (char c : literal) {
        const auto b = static_cast<uint8_t>(c);
        if (!b) {
            // A backslash-NUL escape is not valid syntax; spell it out.
            out.append("\\x00");
            continue;
        }
        const bool plain = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b >= 0x80;
        if (!plain)
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// ----------- compile ----------------------------------------------

static void setError(std::span<char> output, const char* msg)
{
    char* const buffer = output.data();
    const int capacity = static_cast<int>(output.size());
    if (!buffer || capacity <= 0)
        return;
    int n = static_cast<int>(std::strlen(msg));
    if (n > capacity - 1)
        n = capacity - 1;
    std::memcpy(buffer, msg, n);
    buffer[n] = 0;
}

// Compiles a pattern for a normalization form, or answers why it did not.
static std::shared_ptr<ScrollSearchProgram> compileProgram(std::string_view pattern, CaseSensitivity caseSensitivity, Ubiquitty::Unicode::NormalizationForm form, std::string* error)
{
    // Byte-length cap on the raw pattern (pre-normalization, so the limit a
    // user sees never moves with the configured form). RE2's max_mem is the
    // real memory backstop; this cap keeps a small, predictable ceiling with
    // a stable one-line error message.
    if (pattern.size() >= static_cast<std::size_t>(scrollSearchMaxPattern)) {
        *error = "pattern too long";
        return nullptr;
    }
    // Normalize the pattern to the matching form so a precomposed/decomposed
    // needle matches the equally-normalized haystack. Normalization never
    // touches ASCII, so regex metacharacters are untouched; only literal
    // non-ASCII text is rewritten.
    const std::string source = Ubiquitty::Unicode::normalizeUTF8(std::string(pattern), matchingForm(form));

    RE2::Options options;
    options.set_log_errors(false); // user input: no stderr spew on a typo
    options.set_max_mem(8 << 20); // bounded program/DFA memory for hostile patterns
    options.set_encoding(RE2::Options::EncodingUTF8); // whole characters, Unicode case folding (see file-top essay)
    options.set_case_sensitive(caseSensitivity == CaseSensitivity::Sensitive);
    options.set_never_capture(true); // groups group; only whole-match spans are reported
    auto program = std::make_shared<ScrollSearchProgram>(source, options);
    if (!program->pattern.ok()) {
        *error = program->pattern.error();
        return nullptr;
    }
    return program;
}

bool compile(const char* pattern, CaseSensitivity caseSensitivity, CompiledRegex* out, std::span<char> output)
{
    if (!pattern || !out) {
        setError(output, "null pattern or output");
        return false;
    }
    out->program.reset();
    std::string error;
    out->program = compileProgram(pattern, caseSensitivity, s_normalizationForm, &error);
    if (!out->program) {
        setError(output, error.c_str());
        return false;
    }
    return true;
}

std::optional<CompiledRegex> compilePattern(std::string_view pattern, CaseSensitivity caseSensitivity, Ubiquitty::Unicode::NormalizationForm form, std::string& error)
{
    auto program = compileProgram(pattern, caseSensitivity, form, &error);
    if (!program)
        return std::nullopt;
    return CompiledRegex { .program = std::move(program) };
}

// ----------- per-cell text and per-line matching ------------------

static bool namesTerminalStore(const Cell& cell)
{
    return !Ubiquitty::isTextContinuation(cell) && (cell.clusterID() != 0 || cell.pen.extendedAttributes.textSizeSpecID() != 0);
}

std::optional<std::string_view> cellSearchText(const Ubiquitty::Terminal* terminal, const Cell* cell, Ubiquitty::Unicode::NormalizationForm form, CellTextScratch& scratch)
{
    if (cell && Ubiquitty::isTextContinuation(*cell))
        return std::string_view {};
    if (cell && !terminal && namesTerminalStore(*cell))
        return std::nullopt;
    // A proportional run's continuation carries only the run's claim; the
    // run's text rides its anchor.
    if (cell && terminal && Ubiquitty::isProportionalRunContinuation(terminal, *cell))
        return std::string_view {};
    std::span<const uint32_t> text = cell && terminal ? Ubiquitty::runTextCodePoints(terminal, *cell) : std::span<const uint32_t> {};
    if (text.empty() && (!cell || (cell->clusterID() == 0 && cell->combiningLength() == 0))) {
        // The common cell: one code point. ASCII reads the same in every form.
        const uint32_t codePoint = cell ? cell->codePoint : uint32_t { ' ' };
        if (codePoint < 0x80) {
            const bool isPrintable = codePoint >= 0x20 && codePoint < 0x7F;
            scratch.utf8.assign(1, static_cast<char>(isPrintable ? codePoint : ' '));
            return std::string_view(scratch.utf8);
        }
    }
    uint32_t cluster[16];
    if (text.empty() && cell) {
        const int count = terminal ? Ubiquitty::cellClusterCodePoints(terminal, cell, cluster, static_cast<int>(sizeof(cluster) / sizeof(cluster[0])))
                                   : Ubiquitty::cellInlineClusterCodePoints(*cell, cluster, static_cast<int>(sizeof(cluster) / sizeof(cluster[0])));
        text = { cluster, static_cast<std::size_t>(count > 0 ? count : 0) };
    }
    return codePointSearchText(text, form, scratch);
}

std::string_view codePointSearchText(std::span<const std::uint32_t> text, Ubiquitty::Unicode::NormalizationForm form, CellTextScratch& scratch)
{
    scratch.codePoints.assign(text.begin(), text.end());
    Ubiquitty::Unicode::normalizeCodePoints(scratch.codePoints, form);
    scratch.utf8.clear();
    for (const uint32_t codePoint : scratch.codePoints)
        Ubiquitty::Unicode::appendUTF8(scratch.utf8, (codePoint < 0x20 || codePoint == 0x7F) ? uint32_t { ' ' } : codePoint);
    if (scratch.utf8.empty())
        scratch.utf8 = " ";
    return std::string_view(scratch.utf8);
}

void forEachLineMatch(const CompiledRegex& compiled, std::string_view line, LineMatchVisitor visit, void* context)
{
    if (!compiled.program || !compiled.program->pattern.ok() || !visit)
        return;
    const RE2& pattern = compiled.program->pattern;
    const re2::StringPiece subject(line.data(), line.size());
    std::size_t scanPosition = 0;
    while (scanPosition <= subject.size()) {
        re2::StringPiece hit;
        if (!pattern.Match(subject, scanPosition, subject.size(), RE2::UNANCHORED, &hit, 1))
            return;
        const auto start = static_cast<std::size_t>(hit.data() - subject.data());
        if (!visit(context, start, hit.size()))
            return;
        scanPosition = start + std::max<std::size_t>(hit.size(), 1);
    }
}

// ----------- public API -------------------------------------------

// ----------- history search ------------------------------------------

// Appends one history cell's text and its index in the protocol line body.
static void appendHistoryCell(const Ubiquitty::Terminal& t, const Cell& cell, std::uint32_t cellIndex, Ubiquitty::Unicode::NormalizationForm form, std::string& bytes, std::vector<std::uint32_t>& cells, CellTextScratch& scratch)
{
    const std::string_view cellText = cellSearchText(&t, &cell, form, scratch).value_or(std::string_view {});
    for (const char byte : cellText) {
        bytes.push_back(byte);
        cells.push_back(cellIndex);
    }
}

std::optional<HistorySearchResult> searchHistory(const Ubiquitty::Terminal& terminal, std::string_view pattern, HistoryLineRange range, CaseSensitivity caseSensitivity,
    Ubiquitty::Unicode::NormalizationForm form, std::size_t maxMatches)
{
    std::string error;
    const std::shared_ptr<ScrollSearchProgram> program = compileProgram(pattern, caseSensitivity, form, &error);
    if (!program)
        return std::nullopt;
    CellTextScratch scratch;
    const HistoryAddress address = historyAddress(terminal);
    HistorySearchResult result;
    result.epoch = address.epoch;
    const std::uint64_t first = std::max({ range.from, address.oldest, address.clearedThrough });
    const std::uint64_t end = std::min(range.to, address.head);
    std::string bytes;
    std::vector<std::uint32_t> cells;
    for (std::uint64_t number = first; number < end; ++number) {
        const std::optional<Ubiquitty::LineStore::LogicalLineRows> rows = logicalLineRows(terminal, number);
        if (!rows)
            continue;
        // The line's rows joined as the history plane joins them, so a cell
        // index here is one in the line a client holds.
        bytes.clear();
        cells.clear();
        std::uint32_t cellIndex = 0;
        for (int index = rows->firstIndex; index < rows->firstIndex + rows->rowCount; ++index) {
            const ScrollLine* row = scrollbackLine(&terminal, index);
            if (!row)
                break;
            const int length = writtenLength(*row);
            for (int column = 0; column < length; ++column, ++cellIndex)
                appendHistoryCell(terminal, row->cells[column], cellIndex, matchingForm(form), bytes, cells, scratch);
        }
        const re2::StringPiece line(bytes.data(), bytes.size());
        std::size_t scanPosition = 0;
        while (scanPosition <= line.size()) {
            re2::StringPiece hit;
            if (!program->pattern.Match(line, scanPosition, line.size(), RE2::UNANCHORED, &hit, 1))
                break;
            const auto start = static_cast<std::size_t>(hit.data() - line.data());
            // A zero-length hit covers no cell.
            if (!hit.empty()) {
                if (result.matches.size() == maxMatches) {
                    result.isTruncated = true;
                    return result;
                }
                result.matches.push_back({ .line = number, .startCell = cells[start], .endCell = cells[start + hit.size() - 1] + 1 });
            }
            scanPosition = start + std::max<std::size_t>(hit.size(), 1);
        }
    }
    return result;
}

} // namespace Screen::ScrollSearch

} // namespace Ubiquitty
