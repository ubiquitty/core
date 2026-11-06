// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_reply_disposition.cpp — every reply passes one decision about who answers.
//
// A program's queries (DA, DSR, DECRQSS, the OSC colour queries, XTGETTCAP)
// are answered from terminal state. Where two terminals parse the same bytes —
// a host and a replica of it — exactly one may answer, or the program reads two
// replies to one question. So every reply passes through replyDisposition, and a
// terminal whose replies are answered elsewhere writes none of them. Each query
// family is driven here against both kinds of terminal, so a reply path that
// bypasses the decision shows up as a family that still answers.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include "Core/TerminalShared.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

using UbiquittyTest::feed;

namespace {

struct Query {
    const char* name;
    std::string_view bytes;
};

constexpr Query queries[] = {
    { "DA", "\x1b[c" },
    { "DSR cursor position", "\x1b[6n" },
    { "DSR status", "\x1b[5n" },
    { "DECRQSS SGR", "\x1bP$qm\x1b\\" },
    { "OSC 10 query", "\x1b]10;?\x1b\\" },
    { "OSC 4 query", "\x1b]4;1;?\x1b\\" },
    { "XTGETTCAP", "\x1bP+q544e\x1b\\" },
    { "XTVERSION", "\x1b[>q" },
};

std::string replyTo(Ubiquitty::Terminal& terminal, std::string_view query)
{
    feed(terminal, query);
    std::string reply;
    std::array<char, 4096> buffer;
    for (int n; (n = Ubiquitty::drainOutput(&terminal, { buffer.data(), buffer.size() })) > 0;)
        reply.append(buffer.data(), static_cast<std::size_t>(n));
    return reply;
}

std::unique_ptr<Ubiquitty::Terminal> makeTerminal(Ubiquitty::ReplyAuthority authority)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 10, 40);
    terminal->allowTerminalCapabilityOperations = true;
    terminal->replyAuthority = authority;
    return terminal;
}

} // namespace

TEST(aTerminalThatAnswersRepliesToEveryQueryFamily)
{
    for (const Query& query : queries) {
        auto terminal = makeTerminal(Ubiquitty::ReplyAuthority::ThisTerminal);
        CHECK_MSG(!replyTo(*terminal, query.bytes).empty(), query.name);
    }
}

TEST(aTerminalAnsweredElsewhereRepliesToNone)
{
    for (const Query& query : queries) {
        auto terminal = makeTerminal(Ubiquitty::ReplyAuthority::Elsewhere);
        const std::string reply = replyTo(*terminal, query.bytes);
        CHECK_MSG(reply.empty(), (std::string(query.name) + " answered: " + reply).c_str());
    }
}

TEST(theDispositionNamesWhoAnswers)
{
    auto answering = makeTerminal(Ubiquitty::ReplyAuthority::ThisTerminal);
    auto replica = makeTerminal(Ubiquitty::ReplyAuthority::Elsewhere);
    CHECK(Ubiquitty::replyDisposition(*answering, Ubiquitty::Capability::DeviceAttributesRead) == Ubiquitty::ReplyDisposition::Answer);
    CHECK(Ubiquitty::replyDisposition(*replica, Ubiquitty::Capability::DeviceAttributesRead) == Ubiquitty::ReplyDisposition::AnsweredElsewhere);
}

// RIS wipes the program's state, not who answers for the terminal: a program
// that resets a replica must not make it start answering.
TEST(aHardResetKeepsTheReplyAuthority)
{
    auto replica = makeTerminal(Ubiquitty::ReplyAuthority::Elsewhere);
    CHECK(replyTo(*replica, "\033c\x1b[c")
            .empty());
}

UBIQUITTY_TEST_MAIN()
