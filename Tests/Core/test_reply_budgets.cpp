// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Budgets on the work one request can demand.
//
// An XTGETTCAP carries as many capability names as the sender joins with
// semicolons, and each is answered with its own DCS frame; the answers must
// arrive whole — a frame torn by reply pressure reads as garbage keys to the
// host waiting on it. A trigger line is matched by every configured rule, and
// the matcher work a hostile line can demand is the sum over the rules, so the
// step budget spans the line rather than being reborn per rule.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Engine.h"

#include <string>
#include <vector>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.outputLength = 0;
    return t;
}

std::string hexOf(std::string_view text)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string hex;
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0xF]);
    }
    return hex;
}

std::string drainEverything(Ubiquitty::Terminal& t)
{
    std::string all;
    char block[4096];
    for (;;) {
        const int got = drain(t, block);
        if (got <= 0)
            break;
        all.append(block, static_cast<std::size_t>(got));
    }
    return all;
}

// Every DCS in `bytes` must open with one of the two XTGETTCAP forms and close
// with ST before the next opens: a tear is an opening whose frame never ends.
int wellFormedTerminfoFrames(const std::string& bytes)
{
    int frames = 0;
    std::size_t at = 0;
    while ((at = bytes.find("\x1bP", at)) != std::string::npos) {
        const std::size_t close = bytes.find("\x1b\\", at + 2);
        if (close == std::string::npos)
            return -1; // torn: an opening with no ST
        const std::size_t nextOpen = bytes.find("\x1bP", at + 2);
        if (nextOpen != std::string::npos && nextOpen < close)
            return -1; // torn: a frame opened inside another
        ++frames;
        at = close + 2;
    }
    return frames;
}

} // namespace

TEST(aMultiTokenQueryAnswersEveryTokenWhole)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1bP+q" + hexOf("cols") + ";" + hexOf("colors") + ";" + hexOf("nonsense") + "\x1b\\");
    const std::string replies = drainEverything(t);

    // One frame answers the request, as xterm answers it: the two hits joined
    // by `;`, and nothing past the `;` before the miss that ends the walk.
    CHECK(wellFormedTerminfoFrames(replies) == 1);
    CHECK_CONTAINS(replies.c_str(), ("\x1bP1+r" + hexOf("cols") + "=").c_str());
    CHECK_CONTAINS(replies.c_str(), (";" + hexOf("colors") + "=").c_str());
    CHECK(replies.find(hexOf("nonsense")) == std::string::npos);
}

TEST(aBurstOfTokensArrivesUntornAndUndropped)
{
    // Two hundred tokens in one request outgrow the small reply buffer many
    // times over. Every answer must still arrive, in one whole frame: the
    // undersized path dropped whole pieces of frames it had already opened.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    std::string query = "\x1bP+q";
    const std::string cols = hexOf("cols");
    for (int i = 0; i < 200; ++i) {
        if (i > 0)
            query += ";";
        query += cols;
    }
    query += "\x1b\\";
    feed(t, query);

    const std::string replies = drainEverything(t);
    CHECK(wellFormedTerminfoFrames(replies) == 1);
    std::size_t answers = 0;
    for (std::size_t at = replies.find(cols + "="); at != std::string::npos; at = replies.find(cols + "=", at + 1))
        ++answers;
    CHECK(answers == 200);
}

TEST(theMatcherBudgetSpansTheLine)
{
    // Sixty-four rules whose pattern collapses against a line of near-misses:
    // each would spend the whole step budget alone, so a budget reborn per rule
    // multiplies by the rule count on the ingest thread. The pool spans the
    // line, the exhaustion is surfaced, and the call returns.
    Ubiquitty::Triggers::Engine engine;
    for (int i = 0; i < 64; ++i)
        CHECK(engine.addRule("a*a*a*a*a*a*a*x", Ubiquitty::Triggers::Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));

    const std::string hostile(60, 'a'); // no 'x': every split is explored and none succeeds
    Ubiquitty::Triggers::Matched matches[8];
    bool didExhaustBudget = false;
    const int found = engine.evaluateLine(hostile, matches, &didExhaustBudget);

    CHECK(found == 0);
    CHECK_MSG(didExhaustBudget, "the shared pool must report exhaustion rather than miss silently");
}

TEST(ordinaryRulesShareTheLinePoolWithoutNoticing)
{
    // A plain rule's match costs steps proportional to the line, so several of
    // them spend a sliver of the pool: all match, and the flag stays down.
    Ubiquitty::Triggers::Engine engine;
    CHECK(engine.addRule("error", Ubiquitty::Triggers::Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(engine.addRule("warning", Ubiquitty::Triggers::Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(engine.addRule("[0-9]+", Ubiquitty::Triggers::Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));

    Ubiquitty::Triggers::Matched matches[8];
    bool didExhaustBudget = false;
    const int found = engine.evaluateLine("error 42 with a warning attached", matches, &didExhaustBudget);

    CHECK(found == 3);
    CHECK(!didExhaustBudget);
}

UBIQUITTY_TEST_MAIN()
