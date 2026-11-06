// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// PaletteEngine.cpp — the ranking pipeline every frontend shares, so they all
// filter and order identically.
//

#include "Core/CommandPalette/PaletteEngine.h"

#include "Core/CommandPalette/FuzzyMatcher.h"

#include <algorithm>
#include <cstddef>

namespace Ubiquitty::CommandPalette {

namespace {

// Count code points (UTF-8 lead bytes) — the frontend's `title.count` for the
// Latin command text, used as the final ranking tiebreak.
int codePointCount(const std::string& text)
{
    int count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

// Bytes of one whitespace code point at `i`, or 0. Covers ASCII space/tab and
// the UTF-8 whitespace a macOS keyboard can type into the field (Option-Space
// NBSP U+00A0, the U+2000-200A quad family, U+202F, U+3000) — the frontends
// must not need their own trim for the ranking gate to agree with the engine.
std::size_t whitespaceLengthAt(std::string_view text, std::size_t i)
{
    const unsigned char first = static_cast<unsigned char>(text[i]);
    if (first == ' ' || first == '\t')
        return 1;
    const std::size_t remaining = text.size() - i;
    const unsigned char second = remaining > 1 ? static_cast<unsigned char>(text[i + 1]) : 0;
    const unsigned char third = remaining > 2 ? static_cast<unsigned char>(text[i + 2]) : 0;
    if (first == 0xC2 && second == 0xA0)
        return 2; // U+00A0 no-break space (Option-Space)
    if (first == 0xE2 && second == 0x80 && ((third >= 0x80 && third <= 0x8A) || third == 0xAF))
        return 3; // U+2000-200A spaces, U+202F narrow no-break space
    if (first == 0xE3 && second == 0x80 && third == 0x80)
        return 3; // U+3000 ideographic space
    return 0;
}

std::string_view trimWhitespace(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end) {
        const std::size_t length = whitespaceLengthAt(text, begin);
        if (length == 0)
            break;
        begin += length;
    }
    // Trailing side: scan forward from `begin` remembering the end of the last
    // non-whitespace run (a UTF-8 code point can't be recognized backward).
    std::size_t lastContent = begin;
    for (std::size_t i = begin; i < end;) {
        const std::size_t length = whitespaceLengthAt(text, i);
        if (length == 0) {
            ++i;
            lastContent = i;
        } else {
            i += length;
        }
    }
    end = lastContent > begin ? lastContent : begin;
    return text.substr(begin, end - begin);
}

char foldASCII(char character)
{
    if (character >= 'A' && character <= 'Z')
        return static_cast<char>(character + 32);
    return character;
}

// ASCII case-fold ordering; non-ASCII compares by raw UTF-8 byte. Localized
// collation is not used, because the blank-query order must be identical on
// every platform and stdlib, and locale tables would fork it.
bool caseInsensitiveLess(const std::string& lhs, const std::string& rhs)
{
    const std::size_t n = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char a = foldASCII(lhs[i]);
        const char b = foldASCII(rhs[i]);
        if (a != b)
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
    }
    return lhs.size() < rhs.size();
}

PaletteResult resultFor(const PaletteCommand& command, std::vector<int> matchedIndices)
{
    PaletteResult result;
    result.id = command.id;
    result.checked = false;
    result.matchedIndices = std::move(matchedIndices);
    return result;
}

// The strongest match for a command: a TITLE hit wins (its indices drive the
// highlight); otherwise the best KEYWORD hit, demoted below every title hit and
// carrying no title indices (nothing on the title to highlight). The query
// arrives pre-decoded (one decode+fold per rankedResults call, not one per
// candidate string).
template<class DecodedCommand> FuzzyMatch bestMatch(const std::vector<char32_t>& queryNeedles, const DecodedCommand& command)
{
    FuzzyMatch titleMatch = fuzzyMatchDecoded(queryNeedles, command.title);
    if (titleMatch.matched)
        return titleMatch;

    FuzzyMatch best;
    for (const std::vector<char32_t>& keyword : command.keywords) {
        FuzzyMatch match = fuzzyMatchDecoded(queryNeedles, keyword);
        if (!match.matched)
            continue;
        FuzzyMatch demoted;
        demoted.matched = true;
        demoted.tier = match.tier + 10;
        demoted.position = match.position;
        if (!best.matched || demoted.tier < best.tier)
            best = demoted;
    }
    return best;
}

} // namespace

Engine::ParsedInput Engine::interpretInput(std::string_view rawText, Scope currentScope, TerminalAvailability terminalAvailability)
{
    ParsedInput parsed;
    parsed.scope = currentScope;
    parsed.query = std::string(rawText);
    parsed.didSwitchScope = false;

    if (currentScope != Scope::Actions || rawText.empty())
        return parsed;

    std::optional<Scope> next = scopeForPrefix(rawText.front());
    if (!next)
        return parsed;
    if (requiresTerminal(*next) && terminalAvailability == TerminalAvailability::Unavailable)
        return parsed;

    parsed.scope = *next;
    parsed.query = std::string(rawText.substr(1)); // strip the prefix exactly once
    parsed.didSwitchScope = true;
    return parsed;
}

Engine::ParsedFindQuery Engine::parseFindQuery(std::string_view text)
{
    ParsedFindQuery parsed;
    if (text.size() >= 3 && text.starts_with("re ")) {
        parsed.isRegex = true;
        parsed.pattern = std::string(text.substr(3));
        return parsed;
    }
    // "\re " escapes the mini-language: a literal search for text starting
    // with "re " (otherwise unreachable).
    if (text.size() >= 4 && text.starts_with("\\re ")) {
        parsed.pattern = std::string(text.substr(1));
        return parsed;
    }
    parsed.pattern = std::string(text);
    return parsed;
}

void Engine::setActive(Scope scope, std::vector<PaletteCommand> commands)
{
    scope_ = scope;
    commands_ = std::move(commands);
    decodedCommands_.clear();
    decodedCommands_.reserve(commands_.size());
    for (const PaletteCommand& command : commands_) {
        DecodedCommand decoded;
        decoded.title = decodeCandidate(command.title);
        decoded.keywords.reserve(command.keywords.size());
        for (const std::string& keyword : command.keywords)
            decoded.keywords.push_back(decodeCandidate(keyword));
        decodedCommands_.push_back(std::move(decoded));
    }
}

void Engine::setUsageScores(UsageScoreMap scores) { usageScores_ = std::move(scores); }

double Engine::usageScoreFor(const std::string& id) const
{
    const auto found = usageScores_.find(id);
    return found == usageScores_.end() ? 0.0 : found->second;
}

std::vector<PaletteResult> Engine::rankedResults(std::string_view query) const
{
    const std::string_view trimmed = trimWhitespace(query);

    if (trimmed.empty()) {
        std::vector<const PaletteCommand*> ordered;
        ordered.reserve(commands_.size());
        for (const PaletteCommand& command : commands_)
            ordered.push_back(&command);
        if (!preservesOrder(scope_)) {
            // stable_sort here too: case-variant duplicate titles tie under
            // caseInsensitiveLess, and ties must keep registration order (the
            // same contract the ranked path pins below). Frecency floats
            // recently-used commands above the alphabet (score desc first).
            std::ranges::stable_sort(ordered, [this](const PaletteCommand* lhs, const PaletteCommand* rhs) {
                const double lhsScore = usageScoreFor(lhs->id);
                const double rhsScore = usageScoreFor(rhs->id);
                if (lhsScore != rhsScore)
                    return lhsScore > rhsScore;
                return caseInsensitiveLess(lhs->title, rhs->title);
            });
        }
        std::vector<PaletteResult> results;
        results.reserve(ordered.size());
        for (const PaletteCommand* command : ordered)
            results.push_back(resultFor(*command, {}));
        return results;
    }

    struct Scored {
        PaletteResult result;
        int tier;
        int position;
        double usage;
        int titleLength;
    };
    // One decode+fold of the query for the whole pass — bestMatch runs per
    // command and per keyword; re-decoding there dominated the allocations.
    const std::vector<char32_t> queryNeedles = decodeQuery(trimmed);
    std::vector<Scored> scored;
    scored.reserve(commands_.size());
    for (std::size_t index = 0; index < commands_.size(); ++index) {
        const PaletteCommand& command = commands_[index];
        FuzzyMatch match = bestMatch(queryNeedles, decodedCommands_[index]);
        if (!match.matched)
            continue;
        scored.push_back(Scored { resultFor(command, match.matchedIndices), match.tier, match.position, preservesOrder(scope_) ? 0.0 : usageScoreFor(command.id), codePointCount(command.title) });
    }
    // stable_sort: the frontend's Swift sorted(by:) is stable (guaranteed since
    // Swift 5), so rows that tie on every key keep their registration order.
    std::ranges::stable_sort(scored, [](const Scored& lhs, const Scored& rhs) {
        if (lhs.tier != rhs.tier)
            return lhs.tier < rhs.tier;
        if (lhs.position != rhs.position)
            return lhs.position < rhs.position;
        // Frecency breaks equal-quality ties (never beats match quality),
        // ahead of the shorter-title rule.
        if (lhs.usage != rhs.usage)
            return lhs.usage > rhs.usage;
        return lhs.titleLength < rhs.titleLength;
    });

    std::vector<PaletteResult> results;
    results.reserve(scored.size());
    for (Scored& entry : scored)
        results.push_back(std::move(entry.result));
    return results;
}

std::vector<PaletteResult> Engine::rankedOptions(std::string_view query, const std::vector<PaletteOption>& options, int current) const
{
    const std::string_view trimmed = trimWhitespace(query);

    auto optionResult = [&](int index, std::vector<int> matchedIndices) {
        PaletteResult result;
        result.id = std::to_string(index);
        result.checked = index == current;
        result.matchedIndices = std::move(matchedIndices);
        return result;
    };

    if (trimmed.empty()) {
        // Natural order (Off, Short, Long, Always …) — never alphabetized.
        std::vector<PaletteResult> results;
        results.reserve(options.size());
        for (int index = 0; index < static_cast<int>(options.size()); ++index)
            results.push_back(optionResult(index, {}));
        return results;
    }

    struct Scored {
        PaletteResult result;
        int tier;
        int position;
    };
    const std::vector<char32_t> queryNeedles = decodeQuery(trimmed);
    std::vector<Scored> scored;
    for (int index = 0; index < static_cast<int>(options.size()); ++index) {
        FuzzyMatch match = fuzzyMatchDecoded(queryNeedles, options[static_cast<std::size_t>(index)].label);
        if (!match.matched)
            continue;
        scored.push_back(Scored { optionResult(index, match.matchedIndices), match.tier, match.position });
    }
    std::ranges::stable_sort(scored, [](const Scored& lhs, const Scored& rhs) {
        if (lhs.tier != rhs.tier)
            return lhs.tier < rhs.tier;
        return lhs.position < rhs.position;
    });

    std::vector<PaletteResult> results;
    results.reserve(scored.size());
    for (Scored& entry : scored)
        results.push_back(std::move(entry.result));
    return results;
}

} // namespace Ubiquitty::CommandPalette
