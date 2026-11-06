// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Triggers.cpp — see Triggers.h. Self-contained regex matcher plus the ordered rule + smart-selection tables.

#include "Core/Triggers/Engine.h"
#include <cstring>

namespace Ubiquitty::Triggers {

using Token = Program::Token;

namespace {

uint8_t foldByte(uint8_t c, bool ci)
{
    if (!ci)
        return c;
    if (c >= 'A' && c <= 'Z')
        return static_cast<uint8_t>(c + ('a' - 'A'));
    return c;
}

void classSet(Token* t, uint8_t c, bool ci)
{
    uint8_t cf = foldByte(c, ci);
    t->classBitmap[cf / 8] |= static_cast<uint8_t>(1 << (cf % 8));
    if (ci && c >= 'a' && c <= 'z') {
        uint8_t up = static_cast<uint8_t>(c - ('a' - 'A'));
        t->classBitmap[up / 8] |= static_cast<uint8_t>(1 << (up % 8));
    } else if (ci && c >= 'A' && c <= 'Z') {
        uint8_t lo = static_cast<uint8_t>(c + ('a' - 'A'));
        t->classBitmap[lo / 8] |= static_cast<uint8_t>(1 << (lo % 8));
    }
}

bool classContains(const Token* t, uint8_t c) { return (t->classBitmap[c / 8] & (1 << (c % 8))) != 0; }

// Does token `tokenIndex` match a single byte? Anchors / END never do.
bool tokenMatchByte(const Program* re, int tokenIndex, uint8_t c)
{
    const Token* token = &re->tokens[tokenIndex];
    uint8_t cf = foldByte(c, re->caseInsensitive);
    switch (token->op) {
    case Program::TokenOperation::Any:
        return true;
    case Program::TokenOperation::Literal:
        return cf == token->literal;
    case Program::TokenOperation::Class:
        return classContains(token, cf);
    case Program::TokenOperation::ClassNegated:
        return !classContains(token, cf);
    default:
        return false;
    }
}

// Recursive backtracking matcher. Bounded input + program depth keeps the
// stack budget small. On success writes the absolute end position.
//
// `steps` bounds the search. Backtracking explores every way to split the subject
// between adjacent quantifiers, so a pattern like `a*a*a*a*a*a*a*x` against a line
// of 60 `a`s takes 20.8 s measured here — and the line comes from terminal output,
// which anything that can print chooses. The pattern is the user's own config, so
// this is not a hostile regex; it is an ordinary one meeting a subject picked to
// be worst-case for it.
//
// Exhausting the budget reports no match. That is a real loss — a pattern that
// would have matched after more work does not fire — but it is the loss that
// keeps the parse path moving, and maxMatchSteps is set far above what any
// pattern needs when it is not being made to backtrack.
bool matchHere(const Program* re, int tokenIndex, std::span<const uint8_t> row, int pos, int* matchLength, long long* steps)
{
    if (--*steps < 0)
        return false;
    while (re->tokens[tokenIndex].op != Program::TokenOperation::End) {
        const Token* token = &re->tokens[tokenIndex];
        if (token->op == Program::TokenOperation::AnchorEOL) {
            if (pos == static_cast<int>(row.size())) {
                tokenIndex++;
                continue;
            }
            return false;
        }
        if (token->quant == Program::TokenQuantifier::One) {
            if (pos < static_cast<int>(row.size()) && tokenMatchByte(re, tokenIndex, row[static_cast<std::size_t>(pos)])) {
                pos++;
                tokenIndex++;
                continue;
            }
            return false;
        }
        if (token->quant == Program::TokenQuantifier::Question) {
            if (pos < static_cast<int>(row.size()) && tokenMatchByte(re, tokenIndex, row[static_cast<std::size_t>(pos)])) {
                int sub = 0;
                if (matchHere(re, tokenIndex + 1, row, pos + 1, &sub, steps)) {
                    *matchLength = sub;
                    return true;
                }
            }
            int sub = 0;
            if (matchHere(re, tokenIndex + 1, row, pos, &sub, steps)) {
                *matchLength = sub;
                return true;
            }
            return false;
        }
        if (token->quant == Program::TokenQuantifier::Star || token->quant == Program::TokenQuantifier::Plus) {
            int min = (token->quant == Program::TokenQuantifier::Plus) ? 1 : 0;
            int p = pos;
            int taken = 0;
            while (p < static_cast<int>(row.size()) && tokenMatchByte(re, tokenIndex, row[static_cast<std::size_t>(p)])) {
                p++;
                taken++;
            }
            while (taken >= min) {
                int sub = 0;
                if (matchHere(re, tokenIndex + 1, row, p, &sub, steps)) {
                    *matchLength = sub;
                    return true;
                }
                taken--;
                p--;
            }
            return false;
        }
        return false;
    }
    *matchLength = pos;
    return true;
}

bool matchAt(const Program* re, std::span<const uint8_t> row, int startPosition, int* outputLength, long long* steps)
{
    int endPosition = 0;
    if (matchHere(re, 0, row, startPosition, &endPosition, steps)) {
        *outputLength = endPosition - startPosition;
        return true;
    }
    return false;
}

} // namespace

bool compile(const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity, Program* out)
{
    const bool isCaseInsensitive = (caseSensitivity == Ubiquitty::CaseSensitivity::Insensitive);
    if (!out)
        return false;
    std::memset(out, 0, sizeof(*out));
    if (!pattern)
        return false;
    out->caseInsensitive = isCaseInsensitive;

    const char* p = pattern;
    int n = 0;
    while (*p) {
        if (n >= maxPatternLength - 1)
            return false; // pattern too long
        Token* token = &out->tokens[n];
        std::memset(token, 0, sizeof(*token));
        token->quant = Program::TokenQuantifier::One;
        char c = *p++;
        if (c == '^') {
            if (n == 0) {
                out->anchorBol = true;
                continue;
            }
            token->op = Program::TokenOperation::Literal;
            token->literal = foldByte(static_cast<uint8_t>(c), isCaseInsensitive);
        } else if (c == '$') {
            if (*p == 0) {
                out->isEndOfLineAnchored = true;
                continue;
            }
            token->op = Program::TokenOperation::Literal;
            token->literal = foldByte(static_cast<uint8_t>(c), isCaseInsensitive);
        } else if (c == '.') {
            token->op = Program::TokenOperation::Any;
        } else if (c == '[') {
            bool neg = false;
            if (*p == '^') {
                neg = true;
                p++;
            }
            if (*p == 0)
                return false; // unterminated class
            token->op = neg ? Program::TokenOperation::ClassNegated : Program::TokenOperation::Class;
            bool first = true;
            while (*p && (first || *p != ']')) {
                first = false;
                char lo = *p++;
                if (lo == '\\' && *p)
                    lo = *p++;
                if (*p == '-' && p[1] && p[1] != ']') {
                    p++;
                    char hi = *p++;
                    if (hi == '\\' && *p)
                        hi = *p++;
                    uint8_t a = static_cast<uint8_t>(lo), b = static_cast<uint8_t>(hi);
                    if (a > b) {
                        uint8_t tmp = a;
                        a = b;
                        b = tmp;
                    }
                    for (uint8_t k = a;; k++) {
                        classSet(token, k, isCaseInsensitive);
                        if (k == b)
                            break;
                    }
                } else {
                    classSet(token, static_cast<uint8_t>(lo), isCaseInsensitive);
                }
            }
            if (*p != ']')
                return false; // unterminated class
            p++;
        } else if (c == '\\' && *p) {
            token->op = Program::TokenOperation::Literal;
            token->literal = foldByte(static_cast<uint8_t>(*p++), isCaseInsensitive);
        } else if (c == '*' || c == '+' || c == '?') {
            return false; // quantifier with no atom
        } else {
            token->op = Program::TokenOperation::Literal;
            token->literal = foldByte(static_cast<uint8_t>(c), isCaseInsensitive);
        }
        if (*p == '*') {
            token->quant = Program::TokenQuantifier::Star;
            p++;
        } else if (*p == '+') {
            token->quant = Program::TokenQuantifier::Plus;
            p++;
        } else if (*p == '?') {
            token->quant = Program::TokenQuantifier::Question;
            p++;
        }
        n++;
    }
    out->tokens[n].op = Program::TokenOperation::End;
    out->tokenCount = n;
    out->valid = true;
    return true;
}

std::optional<MatchSpan> matchSpan(const Program& prog, std::string_view s, bool* didExhaustBudget, long long* sharedSteps)
{
    if (didExhaustBudget)
        *didExhaustBudget = false;
    if (!prog.valid || s.data() == nullptr)
        return std::nullopt;
    const std::span<const uint8_t> row { reinterpret_cast<const uint8_t*>(s.data()), s.size() };
    const int length = static_cast<int>(s.size());
    int maxStartIndex = prog.anchorBol ? 1 : length + 1;
    // One budget for the whole line, not one per start position: the search a
    // long line invites is the sum over its starts, so a per-start budget would
    // bound each attempt and none of the total. A caller evaluating several
    // rules against one line passes its own pool for the same reason one rank
    // up — the work a line can demand is the sum over the rules, and a fresh
    // budget per rule bounds each and none of the total.
    long long ownSteps = maxMatchSteps;
    long long& steps = sharedSteps ? *sharedSteps : ownSteps;
    for (int start = 0; start < maxStartIndex; start++) {
        int matchLength = 0;
        if (!matchAt(&prog, row, start, &matchLength, &steps))
            continue;
        if (prog.isEndOfLineAnchored && start + matchLength != length)
            continue;
        const int span = (matchLength > 0) ? matchLength : 1;
        return MatchSpan { start, start + span - 1 };
    }
    if (didExhaustBudget && steps < 0)
        *didExhaustBudget = true;
    return std::nullopt;
}

// ----------- Engine ------------------------------------------------

Engine::Engine()
    : m_ruleCount(0)
    , m_classCount(0)
{
    std::memset(m_rules, 0, sizeof(m_rules));
    std::memset(m_rulePrograms, 0, sizeof(m_rulePrograms));
    std::memset(m_classes, 0, sizeof(m_classes));
    std::memset(m_classPrograms, 0, sizeof(m_classPrograms));
}

bool Engine::isRestorable(const Program& program)
{
    if (!program.valid || program.tokenCount < 0 || program.tokenCount >= maxPatternLength
        || program.tokens[program.tokenCount].op != Program::TokenOperation::End)
        return false;
    for (int index = 0; index < program.tokenCount; ++index) {
        const auto& token = program.tokens[index];
        if (token.op < Program::TokenOperation::Literal || token.op > Program::TokenOperation::ClassNegated
            || token.quant > Program::TokenQuantifier::Question)
            return false;
    }
    return true;
}

bool Engine::restoreRule(const Rule& rule, const Program& program)
{
    if (m_ruleCount == maxRuleCount || rule.action > Action::Webhook
        || !std::memchr(rule.arg, 0, sizeof(rule.arg)) || !isRestorable(program))
        return false;
    m_rules[m_ruleCount] = rule;
    m_rulePrograms[m_ruleCount++] = program;
    return true;
}

bool Engine::restoreCharacterClass(const CharacterClass& characterClass, const Program& program)
{
    if (m_classCount == maxClassCount || !std::memchr(characterClass.name, 0, sizeof(characterClass.name)) || !isRestorable(program))
        return false;
    m_classes[m_classCount] = characterClass;
    m_classPrograms[m_classCount++] = program;
    return true;
}

static void copyArgument(char* dst, int capacity, const char* src)
{
    if (!src) {
        dst[0] = 0;
        return;
    }
    int n = static_cast<int>(std::strlen(src));
    if (n > capacity - 1)
        n = capacity - 1;
    std::memcpy(dst, src, static_cast<size_t>(n));
    dst[n] = 0;
}

bool Engine::addRule(const char* pattern, Action action, const char* arg, Ubiquitty::CaseSensitivity caseSensitivity)
{
    if (m_ruleCount >= maxRuleCount)
        return false;
    Program prog;
    if (!compile(pattern, caseSensitivity, &prog))
        return false;
    m_rulePrograms[m_ruleCount] = prog;
    m_rules[m_ruleCount].action = action;
    copyArgument(m_rules[m_ruleCount].arg, maxArgumentLength, arg);
    m_ruleCount++;
    return true;
}

bool Engine::addCharacterClass(const char* name, const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity)
{
    if (m_classCount >= maxClassCount)
        return false;
    Program prog;
    if (!compile(pattern, caseSensitivity, &prog))
        return false;
    m_classPrograms[m_classCount] = prog;
    copyArgument(m_classes[m_classCount].name, static_cast<int>(sizeof(m_classes[m_classCount].name)), name);
    m_classCount++;
    return true;
}

int Engine::evaluateLine(std::string_view utf8, std::span<Matched> out, bool* didExhaustBudget) const
{
    if (didExhaustBudget)
        *didExhaustBudget = false;
    if (utf8.data() == nullptr || out.empty())
        return 0;
    const int outputCapacity = static_cast<int>(out.size());
    int written = 0;
    // The pool spans the rules: this runs on the ingest thread once per
    // committed line, and a budget reborn per rule multiplies by the rule
    // count exactly where the line is hostile.
    long long lineSteps = maxMatchSteps;
    for (int r = 0; r < m_ruleCount && written < outputCapacity && lineSteps > 0; r++) {
        bool didRuleExhaustBudget = false;
        const std::optional<MatchSpan> hit = matchSpan(m_rulePrograms[r], utf8, &didRuleExhaustBudget, &lineSteps);
        if (didRuleExhaustBudget && didExhaustBudget)
            *didExhaustBudget = true;
        if (!hit)
            continue;
        Matched* m = &out[written++];
        m->ruleIndex = r;
        m->action = m_rules[r].action;
        m->arg = m_rules[r].arg;
        m->column0 = hit->begin;
        m->column1 = hit->end;
    }
    return written;
}

std::optional<MatchSpan> Engine::expandSelection(std::string_view utf8, int column) const
{
    const int length = static_cast<int>(utf8.size());
    if (utf8.data() == nullptr || length <= 0 || column < 0 || column >= length)
        return std::nullopt;
    const std::span<const uint8_t> row { reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size() };
    for (int c = 0; c < m_classCount; c++) {
        const Program* prog = &m_classPrograms[c];
        if (!prog->valid)
            continue;
        // Find a run this class matches that covers `column`. Anchored classes
        // are honored; otherwise every start is a candidate.
        int maxStartIndex = prog->anchorBol ? 1 : length;
        long long steps = maxMatchSteps; // per class per line, as in matchSpan
        for (int start = 0; start < maxStartIndex; start++) {
            int matchLength = 0;
            if (!matchAt(prog, row, start, &matchLength, &steps))
                continue;
            if (matchLength <= 0)
                continue;
            int last = start + matchLength - 1;
            if (prog->isEndOfLineAnchored && start + matchLength != length)
                continue;
            if (column >= start && column <= last)
                return MatchSpan { start, last };
        }
    }
    return std::nullopt;
}

} // namespace Ubiquitty::Triggers
