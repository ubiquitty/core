// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_paletteengine.cpp — pins Core/CommandPalette/PaletteEngine against the
// macOS BeamModel pipeline: the scope-prefix parse/strip (including the `:`
// double-strip as a NEGATIVE test), empty-query-shows-all ordering (alphabetical
// vs intrinsic), scoped filtering + fuzzy ranking, keyword demotion, and the
// options drill. Covers the palette logic on the host.

#include "Core/CommandPalette/PaletteEngine.h"
#include "Core/CommandPalette/PaletteFrecency.h"

#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::CommandPalette::CommandKind;
using Ubiquitty::CommandPalette::Engine;
using Ubiquitty::CommandPalette::PaletteCommand;
using Ubiquitty::CommandPalette::PaletteResult;
using Ubiquitty::CommandPalette::Scope;
using Ubiquitty::CommandPalette::TerminalAvailability;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

PaletteCommand action(const char* id, const char* title)
{
    PaletteCommand command;
    command.id = id;
    command.title = title;
    command.kind = CommandKind::Action;
    return command;
}

std::vector<std::string> ids(const std::vector<PaletteResult>& results)
{
    std::vector<std::string> out;
    for (const PaletteResult& result : results)
        out.push_back(result.id);
    return out;
}

} // namespace

int main()
{
    // ---- interpretInput: prefix parse / strip -----------------------------

    // A leading prefix in the Actions scope retargets + strips exactly one char.
    {
        Engine::ParsedInput parsed = Engine::interpretInput(":ls", Scope::Actions, TerminalAvailability::Available);
        check(parsed.didSwitchScope && parsed.scope == Scope::GoTo && parsed.query == "ls", ":ls from Actions → GoTo scope, query 'ls'");
    }

    // The `:` double-strip NEGATIVE test: feeding the stripped query back through
    // (the guarded refresh path) must not strip a second character.
    {
        Engine::ParsedInput first = Engine::interpretInput(":ls", Scope::Actions, TerminalAvailability::Available);
        Engine::ParsedInput again = Engine::interpretInput(first.query, first.scope, TerminalAvailability::Available);
        check(!again.didSwitchScope && again.query == "ls", ":ls does not double-strip to 's'");
    }

    // A bare prefix enters the scope with an empty query and stays put on reentry.
    {
        Engine::ParsedInput enter = Engine::interpretInput(":", Scope::Actions, TerminalAvailability::Available);
        check(enter.didSwitchScope && enter.scope == Scope::GoTo && enter.query.empty(), "':' enters GoTo with empty query");
        Engine::ParsedInput stay = Engine::interpretInput("", Scope::GoTo, TerminalAvailability::Available);
        check(!stay.didSwitchScope && stay.scope == Scope::GoTo && stay.query.empty(), "empty text in GoTo stays, no strip");
    }

    // A prefix char is LITERAL once inside a sub-scope (never re-parsed).
    {
        Engine::ParsedInput parsed = Engine::interpretInput(":x", Scope::GoTo, TerminalAvailability::Available);
        check(!parsed.didSwitchScope && parsed.scope == Scope::GoTo && parsed.query == ":x", "':x' in GoTo keeps the colon literal");
    }

    // Terminal-only scopes are gated on availability.
    {
        Engine::ParsedInput blocked = Engine::interpretInput("@blk", Scope::Actions, TerminalAvailability::Unavailable);
        check(!blocked.didSwitchScope && blocked.scope == Scope::Actions && blocked.query == "@blk", "'@' is inert with no terminal");
        Engine::ParsedInput allowed = Engine::interpretInput("@blk", Scope::Actions, TerminalAvailability::Available);
        check(allowed.didSwitchScope && allowed.scope == Scope::Blocks && allowed.query == "blk", "'@' enters Blocks with a terminal");
        Engine::ParsedInput settings = Engine::interpretInput("#col", Scope::Actions, TerminalAvailability::Unavailable);
        check(settings.didSwitchScope && settings.scope == Scope::Settings && settings.query == "col", "'#' enters Settings even with no terminal");
    }

    // Plain text carries through unchanged.
    {
        Engine::ParsedInput parsed = Engine::interpretInput("new", Scope::Actions, TerminalAvailability::Available);
        check(!parsed.didSwitchScope && parsed.scope == Scope::Actions && parsed.query == "new", "plain text is not a scope switch");
    }

    // ---- rankedResults: empty-query ordering ------------------------------

    Engine engine;
    engine.setActive(Scope::Actions, { action("close", "Close Tab"), action("newtab", "New Tab"), action("newwin", "New Window"), action("split", "Split Vertically") });

    // Actions: a blank query shows all rows, alphabetically.
    {
        std::vector<std::string> order = ids(engine.rankedResults(""));
        std::vector<std::string> want { "close", "newtab", "newwin", "split" };
        check(order == want, "empty Actions query is alphabetical");
    }

    // Blocks / go-to preserve the fed (intrinsic) order for a blank query.
    {
        Engine ordered;
        ordered.setActive(Scope::GoTo, { action("t2", "Zeta"), action("t0", "Alpha"), action("t1", "Mid") });
        std::vector<std::string> order = ids(ordered.rankedResults(""));
        std::vector<std::string> want { "t2", "t0", "t1" };
        check(order == want, "empty GoTo query preserves intrinsic order");
    }

    // ---- rankedResults: filtering + ranking -------------------------------

    // 'new' matches both New rows (tier-1 prefix); shorter title wins the tie.
    {
        std::vector<std::string> order = ids(engine.rankedResults("new"));
        std::vector<std::string> want { "newtab", "newwin" };
        check(order == want, "'new' ranks New Tab before New Window (shorter title)");
    }

    // A non-matching query yields nothing.
    check(engine.rankedResults("zzz").empty(), "no match → empty result set");

    // Whitespace is trimmed before matching — including the UTF-8 whitespace a
    // macOS keyboard types (Option-Space NBSP), so frontends need no own trim.
    check(ids(engine.rankedResults("  new  ")) == ids(engine.rankedResults("new")), "surrounding whitespace is trimmed");
    check(ids(engine.rankedResults("\xC2\xA0new\xC2\xA0")) == ids(engine.rankedResults("new")), "NBSP (U+00A0) is trimmed");
    check(ids(engine.rankedResults("\xE3\x80\x80new")) == ids(engine.rankedResults("new")), "ideographic space (U+3000) is trimmed");

    // A keyword-only hit surfaces the command but highlights nothing on the title.
    {
        PaletteCommand settings = action("settings", "Values…");
        settings.keywords = { "preferences" };
        Engine keyworded;
        keyworded.setActive(Scope::Actions, { action("newtab", "New Tab"), settings });
        std::vector<PaletteResult> results = keyworded.rankedResults("preferences");
        check(results.size() == 1 && results[0].id == "settings" && results[0].matchedIndices.empty(), "keyword hit surfaces the command with no title highlight");
    }

    // ---- rankedOptions ----------------------------------------------------

    std::vector<Ubiquitty::CommandPalette::PaletteOption> options { { "Off" }, { "Short" }, { "Long" }, { "Always" } };

    // Blank query: natural order, current value checked.
    {
        std::vector<PaletteResult> results = engine.rankedOptions("", options, 2);
        check(results.size() == 4 && results[0].id == "0" && results[2].id == "2" && results[2].checked && !results[0].checked, "empty options query keeps natural order + checks current");
    }

    // 'l' ranks Long (tier-1 prefix) above Always (tier-3 substring).
    {
        std::vector<std::string> order = ids(engine.rankedOptions("l", options, 2));
        std::vector<std::string> want { "2", "3" };
        check(order == want, "'l' ranks Long (prefix) above Always (substring)");
    }

    // ---- stable ordering on ties (matches Swift's stable sorted(by:)) -----

    // Two options tie on every sort key (tier 1, position 0): registration order
    // is preserved, never swapped.
    {
        std::vector<Ubiquitty::CommandPalette::PaletteOption> tie { { "On" }, { "Off" } };
        std::vector<std::string> order = ids(engine.rankedOptions("o", tie, 0));
        std::vector<std::string> want { "0", "1" };
        check(order == want, "tied options keep registration order (stable sort)");
    }

    // Two commands tie fully (tier 1 prefix, position 0, title length 4): the
    // earlier-registered one stays first.
    {
        Engine ties;
        ties.setActive(Scope::Actions, { action("first", "Copy"), action("second", "Cont") });
        std::vector<std::string> order = ids(ties.rankedResults("co"));
        std::vector<std::string> want { "first", "second" };
        check(order == want, "tied commands keep registration order (stable sort)");
    }

    // The BLANK-query alphabetical path must be stable too: case-variant
    // duplicate titles tie under the fold and keep registration order.
    {
        Engine ties;
        ties.setActive(Scope::Actions, { action("upper", "Copy"), action("lower", "copy"), action("mixed", "COPY") });
        std::vector<std::string> order = ids(ties.rankedResults(""));
        std::vector<std::string> want { "upper", "lower", "mixed" };
        check(order == want, "blank-query case-variant ties keep registration order");
    }

    // Keyword demotion is ABSOLUTE: the weakest title hit (tier 4) outranks the
    // strongest keyword hit (tier 1 + 10).
    {
        PaletteCommand keyworded = action("bykeyword", "Values…");
        keyworded.keywords = { "nt" }; // tier-1 keyword hit for "nt"
        Engine mixed;
        mixed.setActive(Scope::Actions, { keyworded, action("bytitle", "New Tab") });
        std::vector<std::string> order = ids(mixed.rankedResults("nt"));
        std::vector<std::string> want { "bytitle", "bykeyword" }; // tier 4 < tier 11
        check(order == want, "tier-4 title hit outranks tier-1 keyword hit");
    }

    // ---- find mini-language ------------------------------------------------

    {
        Engine::ParsedFindQuery regex = Engine::parseFindQuery("re foo.*");
        check(regex.isRegex && regex.pattern == "foo.*", "'re ' prefix = regex");
        Engine::ParsedFindQuery literal = Engine::parseFindQuery("read me");
        check(!literal.isRegex && literal.pattern == "read me", "'re' without the space stays literal");
        Engine::ParsedFindQuery escaped = Engine::parseFindQuery("\\re boot");
        check(!escaped.isRegex && escaped.pattern == "re boot", "backslash escapes the mini-language for a literal 're ' search");
    }

    // ---- frecency ----------------------------------------------------------

    // Decay math: a use now counts 1.0; one half-life old counts 0.5; a
    // future-dated stamp (clock skew) clamps to now; empty history is 0.
    {
        using Ubiquitty::CommandPalette::frecencyHalfLifeSeconds;
        using Ubiquitty::CommandPalette::frecencyScore;
        using Ubiquitty::CommandPalette::UsageHistory;
        const std::int64_t now = 1'000'000'000;
        const std::int64_t halfLife = static_cast<std::int64_t>(frecencyHalfLifeSeconds);
        UsageHistory history { "id", { now, now - halfLife, now + 60 } };
        const double score = frecencyScore(history, now);
        check(score > 2.49 && score < 2.51, "decay: now + half-life-old + future = 2.5");
        check(frecencyScore(UsageHistory { "id", {} }, now) == 0.0, "empty history scores 0");
    }

    // Blank query: a used command floats above the alphabet; the rest keep
    // alphabetical order.
    {
        Engine used;
        used.setActive(Scope::Actions, { action("close", "Close Tab"), action("newtab", "New Tab"), action("split", "Split Vertically") });
        used.setUsageScores({ { "split", 2.0 }, { "newtab", 1.0 } });
        std::vector<std::string> order = ids(used.rankedResults(""));
        std::vector<std::string> want { "split", "newtab", "close" };
        check(order == want, "blank query floats used commands by score");
    }

    // Ranked query: frecency breaks a full (tier, position) tie ahead of the
    // shorter-title rule, but never beats a better tier.
    {
        Engine used;
        used.setActive(Scope::Actions, { action("short", "New Tab"), action("long", "New Terminal") });
        used.setUsageScores({ { "long", 5.0 } });
        std::vector<std::string> order = ids(used.rankedResults("new"));
        std::vector<std::string> want { "long", "short" }; // tie broken by usage
        check(order == want, "frecency outranks shorter-title on a tie");

        used.setUsageScores({ { "long", 5.0 }, { "short", 0.0 } });
        order = ids(used.rankedResults("terminal"));
        check(order == std::vector<std::string> { "long" }, "frecency never resurrects a non-match");
    }

    // Intrinsic-order scopes ignore frecency entirely.
    {
        Engine ordered;
        ordered.setActive(Scope::GoTo, { action("t2", "Zeta"), action("t0", "Alpha"), action("t1", "Mid") });
        ordered.setUsageScores({ { "t1", 9.0 } });
        std::vector<std::string> order = ids(ordered.rankedResults(""));
        std::vector<std::string> want { "t2", "t0", "t1" };
        check(order == want, "go-to keeps intrinsic order regardless of usage");
    }

    if (g_failures == 0)
        std::printf("test_paletteengine: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
