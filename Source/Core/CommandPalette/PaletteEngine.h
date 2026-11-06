// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// PaletteEngine.h — the query → ranked-results pipeline, shared by every
// frontend. Holds the active scope's command set (fed on scope entry) and ranks
// it per query: an empty query shows all (intrinsic order for blocks / go-to,
// alphabetical for actions / settings); a non-empty query filters + ranks via
// the fuzzy matcher. It is also the one place a leading scope prefix is parsed
// and stripped — feeding a stripped query back through never strips again.
//

#include "Base/Compiler.h"
#include "Core/CommandPalette/PaletteCommand.h"
#include "Core/CommandPalette/PaletteScope.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Ubiquitty::CommandPalette {

// Whether a live terminal backs the terminal-only scopes (go-to / blocks /
// find) for this summon — gates whether their prefixes retarget the scope.
// A fixed closed set: marked closed so Swift/C++ interop imports it @frozen (an
// exhaustive Swift switch needs no @unknown default, and stays valid under the
// Swift 6 language mode).
enum class UBIQUITTY_CLOSED_ENUM TerminalAvailability {
    Unavailable,
    Available,
};

// Named specialization so the Swift/C++ interop importer can spell it (Swift
// can't name a bare std::unordered_map<K,V>). Command id -> decayed frecency.
using UsageScoreMap = std::unordered_map<std::string, double>;

class Engine {
public:
    // The result of interpreting raw field text: the scope it selects and the
    // query with any scope prefix stripped exactly once.
    struct ParsedInput {
        Scope scope = Scope::Actions;
        std::string query;
        bool didSwitchScope = false; // a leading prefix retargeted the scope
    };

    // Interpret raw field text. From the Actions scope, a leading prefix for an
    // AVAILABLE scope retargets the palette and is stripped once; otherwise the
    // text is the query for the current scope, unchanged. `terminalAvailability`
    // gates the terminal-only scopes. This is the SOLE prefix strip: feeding the
    // returned query back through never removes a second character.
    static ParsedInput interpretInput(std::string_view rawText, Scope currentScope, TerminalAvailability terminalAvailability);

    // The find scope's query mini-language, parsed here so every frontend
    // agrees: a leading "re " makes the remainder a regex pattern; a leading
    // "\\re " escapes that (a literal search for text starting "re ");
    // anything else is a literal needle.
    struct ParsedFindQuery {
        std::string pattern;
        bool isRegex = false;
    };
    static ParsedFindQuery parseFindQuery(std::string_view text);

    // Set the active scope and its command list (the fed-per-scope set).
    void setActive(Scope scope, std::vector<PaletteCommand> commands);

    // Frecency: per-summon usage scores keyed by command id (the frontend
    // computes them from its stored histories via frecencyScore). Sparse —
    // only used ids appear. Order-affecting only in the ranked scopes
    // (actions / settings): blank query floats scored ids first (by score);
    // a ranked query breaks (tier, position) ties by score before the
    // shorter-title rule. Scopes that preserve intrinsic order (go-to,
    // blocks) ignore it. Survives setActive.
    void setUsageScores(UsageScoreMap scores);

    [[nodiscard]] Scope scope() const { return scope_; }
    [[nodiscard]] const std::vector<PaletteCommand>& commands() const { return commands_; }

    // Rank the active command set for `query` (whitespace-trimmed internally).
    // Empty query → every row in the scope's blank-query order; else filtered +
    // ranked (tier, then earliest position, then shorter title). A title match
    // wins over a keyword match; a keyword-only hit carries no title indices.
    [[nodiscard]] std::vector<PaletteResult> rankedResults(std::string_view query) const;

    // Rank an options drill's value list for `query` (same fuzzy tiers, no
    // keyword demotion). Empty query → natural order. `current` marks the live
    // value (checked). Each result's id is the ORIGINAL option index as a
    // decimal string, so the frontend routes the apply by index.
    [[nodiscard]] std::vector<PaletteResult> rankedOptions(std::string_view query, const std::vector<PaletteOption>& options, int current) const;

private:
    [[nodiscard]] double usageScoreFor(const std::string& id) const;

    Scope scope_ = Scope::Actions;
    std::vector<PaletteCommand> commands_;
    /// One command's matchable text in the form the matcher scans.
    ///
    /// The strings do not change between keystrokes and the frontend re-ranks on each of them, so
    /// they are decoded and case-folded once when the set is installed rather than once per
    /// candidate per character typed. Parallel to ``commands_`` by index.
    struct DecodedCommand {
        /// The command's title, decoded and case-folded.
        std::vector<char32_t> title;
        /// Its keywords, each decoded and case-folded.
        std::vector<std::vector<char32_t>> keywords;
    };
    std::vector<DecodedCommand> decodedCommands_;
    UsageScoreMap usageScores_;
};

} // namespace Ubiquitty::CommandPalette
