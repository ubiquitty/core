// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Triggers/Triggers.h"

#include <string_view>

#include <array>
#include <optional>
#include <span>
#include <tuple>

#include "Base/Types.h"
#include "Core/CaseSensitivity.h"

// Triggers — portable smart-selection + line-trigger rule engine. Two rule tables consulted from Core so
// every frontend shares one matcher:
//   1. Ordered {regex, action} TRIGGER rules against a completed line. Rule order is the priority: every
//      matching rule contributes its action, in order.
//   2. A characterClass / smart-selection table: named token-class regexes the word-expansion path uses to
//      decide how far a selection grows around a clicked column (e.g. a whole path or URL as one token).
// Regex: a dependency-free subset (literals, '.', '*+?', '[...]' with ranges/negation, '^'/'$', '\'
// escapes; no alternation/grouping/back-references). Self-contained so the engine links without the
// VT/Terminal stack or the RE2 archive.

namespace Ubiquitty::Triggers {

constexpr int maxPatternLength = 128; // matches ScrollSearch's pattern ceiling
constexpr int maxRuleCount = 64; // ordered trigger rules
constexpr int maxClassCount = 32; // smart-selection token classes
constexpr int maxMatched = maxRuleCount; // matches reportable per line
constexpr int maxArgumentLength = 256; // per-rule action argument (title, command, …)

// Backtracking steps one pattern may spend on one line before the matcher gives
// up and reports no match.
//
// The matcher explores every way to split a subject between adjacent
// quantifiers. Measured on this machine, `a*a*a*a*a*a*a*x` against 60 bytes runs
// 20.8 s, and 8 quantifiers against 40 bytes runs 6.3 s — while the same
// patterns with a discriminating literal between them ( `.*a.*b.*c.*d` ) finish
// in under a millisecond. The pattern is the user's own config; the subject is a
// committed line of terminal output, so whoever can print picks the input that
// makes an ordinary pattern behave like a hostile one.
//
// A million steps is roughly 4 ms of the pathological shape and some four orders
// of magnitude above what a pattern spends when it is not being made to
// backtrack, so a legitimate rule never reaches it.
constexpr long long maxMatchSteps = 1000000;

// What a matched trigger asks the frontend to do. The engine only classifies + reports; the
// frontend executes.
enum class Action : uint8_t {
    None = 0,
    Notify, // raise a desktop notification (arg = optional title/text)
    Copy, // copy the match (or whole line) to the clipboard
    SetTitle, // set the window/tab title (arg = title template)
    Highlight, // paint a highlight over the matched span
    RunCommand, // run a command (arg = command line) — gated, spawned by the frontend
    Webhook // POST the match to a URL (arg = URL) — gated, sent by the frontend
};

// One ordered trigger rule.
struct Rule {
    Action action;
    char arg[maxArgumentLength]; // action payload (e.g. title text, command); may be empty
};

// One reported match from evaluateLine.
struct Matched {
    int ruleIndex; // index into the engine's rule list
    Action action;
    const char* arg; // points into the rule's arg buffer (engine-owned)
    int column0; // first matched byte column on the line
    int column1; // last matched byte column (inclusive)
};

// A smart-selection token class: a named regex the expansion path uses to
// recognize a contiguous token (path, URL, identifier, …).
struct CharacterClass {
    char name[32];
};

// One compiled regex program (token array). Sized to the pattern ceiling so the engine is allocation-free.
struct Program {
    /// What a trigger token matches against.
    enum class TokenOperation : uint8_t {
        End = 0,
        Literal,
        Any,
        Class,
        ClassNegated,
        AnchorBOL,
        AnchorEOL
    };
    /// How many times a trigger token may repeat.
    enum class TokenQuantifier : uint8_t {
        One = 0,
        Star,
        Plus,
        Question
    };
    struct Token {
        TokenOperation op;
        TokenQuantifier quant;
        // 256 bits, one per byte value, because classContains indexes it with a
        // byte from the subject line and classSet with a byte from the pattern.
        // At 16 it covered 0x00-0x7F and both sides ran past the end for anything
        // above: the matcher read the next token's bytes for every non-ASCII byte
        // in terminal output, so a rule holding a character class returned a match
        // decision made from adjacent compiled pattern data.
        std::array<uint8_t, 32> classBitmap;
        uint8_t literal;
    } tokens[maxPatternLength];

    // classSet and classContains index classBitmap with a whole byte, so the
    // bitmap has to hold one bit per byte value. Narrowing it puts both sides out
    // of bounds for every byte above the new ceiling, and the read side takes its
    // byte from the line being matched — which is terminal output, so nothing
    // about the pattern keeps the index in range.
    static_assert(std::tuple_size_v<decltype(Token::classBitmap)> * 8 == 256,
        "classBitmap must hold one bit per byte value (see classContains)");
    int tokenCount;
    bool caseInsensitive;
    bool anchorBol;
    bool isEndOfLineAnchored;
    bool valid;
};

// The engine: an ordered rule table + a smart-selection class table. Fixed-size, allocation-free, copyable.
// Build with addRule / addCharacterClass, then feed completed lines through evaluateLine.
class Engine {
public:
    Engine();

    /// Borrows the complete ordered programs; patterns are not retained as text.
    struct Continuation {
        std::span<const Rule> rules;
        std::span<const Program> rulePrograms;
        std::span<const CharacterClass> classes;
        std::span<const Program> classPrograms;
    };

    /// Borrows programs while this engine remains unchanged.
    /// - Returns: Matching rule/program and class/program spans in evaluation order.
    [[nodiscard]] Continuation continuation() const
    {
        return { { m_rules, static_cast<std::size_t>(m_ruleCount) }, { m_rulePrograms, static_cast<std::size_t>(m_ruleCount) },
            { m_classes, static_cast<std::size_t>(m_classCount) }, { m_classPrograms, static_cast<std::size_t>(m_classCount) } };
    }

    /// Checks the bounded instruction stream accepted by the current compiler.
    /// - Parameter program: A field-wise decoded program, never raw object bytes.
    /// - Returns: Whether matching must encounter its terminal instruction within the fixed table.
    [[nodiscard]] static bool isRestorable(const Program& program);

    /// Appends one validated compiled rule without recompiling or executing it.
    /// - Parameters:
    ///   - rule: Its terminated argument and known action.
    ///   - program: The exact compiled matcher.
    /// - Returns: False without mutation on invalid input or a full table.
    [[nodiscard]] bool restoreRule(const Rule& rule, const Program& program);

    /// Appends one validated smart-selection class in registration order.
    /// - Parameters:
    ///   - characterClass: Its terminated name.
    ///   - program: The exact compiled matcher.
    /// - Returns: False without mutation on invalid input or a full table.
    [[nodiscard]] bool restoreCharacterClass(const CharacterClass& characterClass, const Program& program);

    // Append a trigger rule. On a bad pattern or full table the rule is rejected (false). `arg` is copied
    // (truncated to maxArgumentLength-1); may be nullptr/empty.
    bool addRule(const char* pattern, Action, const char* arg, Ubiquitty::CaseSensitivity caseSensitivity);

    // Append a smart-selection token class. False on a bad pattern or full table.
    bool addCharacterClass(const char* name, const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity);

    [[nodiscard]] int ruleCount() const { return m_ruleCount; }
    [[nodiscard]] int characterClassCount() const { return m_classCount; }

    /// Evaluates every rule against one completed line.
    ///
    /// A rule contributes at most its leftmost match, so the result holds one
    /// entry per matching rule rather than one per occurrence, in rule order.
    ///
    /// - Parameters:
    ///   - utf8: The line's bytes, with non-printables already substituted by
    ///     the caller.
    ///   - out: Receives the matches. Its size is the cap — evaluation stops
    ///     when it is full, and the remaining rules are not reported.
    /// - Returns: How many entries were written.
    [[nodiscard]] int evaluateLine(std::string_view utf8, std::span<Matched> out, bool* didExhaustBudget = nullptr) const;

    /// Grows a selection around a column to the widest span one character class matches.
    ///
    /// Classes are tried in registration order and the first covering `column`
    /// wins, so the order rules were added in decides the answer where two
    /// classes overlap.
    ///
    /// - Parameters:
    ///   - utf8: The line's bytes.
    ///   - column: The column the selection must cover.
    ///   - column0: Receives the first column of the span.
    ///   - column1: Receives the last column of the span.
    /// - Returns: True when a class covered `column`; the span is untouched
    ///   otherwise, so a caller's existing word walk can run.
    [[nodiscard]] std::optional<MatchSpan> expandSelection(std::string_view utf8, int column) const;

private:
    Rule m_rules[maxRuleCount];
    Program m_rulePrograms[maxRuleCount];
    int m_ruleCount;

    CharacterClass m_classes[maxClassCount];
    Program m_classPrograms[maxClassCount];
    int m_classCount;
};

// Compile `pattern` into `out` (true on success; on failure `out` is marked invalid). Exposed for tests
// and callers wanting the matcher without the Engine table.
bool compile(const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity, Program* out);

/// Finds the leftmost match of a compiled program in a line.
///
/// The search is bounded by ``maxMatchSteps``. A pattern whose quantifiers sit
/// next to each other explores every way to split the subject between them, so a
/// line chosen to defeat it can cost seconds; the budget converts that into no
/// match. `didExhaustBudget` reports which of the two a "no match" was, since the
/// caller cannot otherwise tell a line that did not match from one that was not
/// searched to the end.
///
/// - Parameters:
///   - s: The bytes to search.
///   - didExhaustBudget: Set to true when the step budget ran out; may be null.
/// - Returns: The match, or nothing when the program does not match.
std::optional<MatchSpan> matchSpan(const Program&, std::string_view s, bool* didExhaustBudget = nullptr, long long* sharedSteps = nullptr);

} // namespace Ubiquitty::Triggers
