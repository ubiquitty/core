// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_trigger_actions — what a matched string is allowed to become.
//
// A trigger matches attacker-controllable terminal output, so these are the
// injection boundaries: argv construction for run_command and the URL/body for
// a webhook.

#include "Harness.h"

#include "Core/Triggers/Actions.h"

#include <string>

namespace {

using Ubiquitty::Triggers::buildCommandArgv;
using Ubiquitty::Triggers::buildWebhookBody;
using Ubiquitty::Triggers::webhookSchemeAllowed;

// ---- argv construction --------------------------------------------------

TEST(testTemplateTokenisesOnWhitespace)
{
    const auto argv = buildCommandArgv("notify-send   title\tbody\nmore", "");
    CHECK_MSG(argv.size() == 4, "runs of whitespace collapse");
    CHECK_MSG(argv[0] == "notify-send", "argv[0] is the program");
    CHECK_MSG(argv[1] == "title", "tabs split");
    CHECK_MSG(argv[2] == "body", "newlines split");
    CHECK_MSG(argv[3] == "more", "and the last word survives");
}

TEST(testEmptyTemplateYieldsNothing)
{
    CHECK_MSG(buildCommandArgv("", "match").empty(), "an empty template spawns nothing");
    CHECK_MSG(buildCommandArgv("   \t\n ", "match").empty(), "an all-whitespace template spawns nothing");
}

TEST(testSubstitutionIsOneArgument)
{
    const auto argv = buildCommandArgv("echo %s", "hello world");
    CHECK_MSG(argv.size() == 2, "the match is a single argv element");
    CHECK_MSG(argv[1] == "hello world", "and keeps its space");
}

// The property the whole design exists for: a match is DATA. It is never
// re-tokenised, so shell metacharacters in terminal output cannot become
// commands — there is no shell, and execve takes argv elements verbatim.
TEST(testMatchIsNeverSplitOrInterpreted)
{
    const char* hostile[] = {
        "; rm -rf /",
        "$(whoami)",
        "`id`",
        "a | tee /tmp/x",
        "a && b",
        "one\ntwo",
        "a\tb",
        "'quoted'",
        "\"double\"",
        "$HOME",
        "a\\;b",
    };

    for (const char* match : hostile) {
        const auto argv = buildCommandArgv("run %s", match);
        CHECK_MSG(argv.size() == 2, "a hostile match stays exactly one argument");
        CHECK_MSG(argv[1] == match, "and is passed through byte for byte");
    }
}

// A match the spawned program would read as an option, not data: with no shell
// to quote it, a `%s` that begins with '-' is argv injection, so the command
// runs nothing. Template words that begin with '-' are the author's and stay.
TEST(testOptionLikeMatchNeutralisesTheCommand)
{
    CHECK_MSG(buildCommandArgv("run %s", "-rf").empty(), "a match that is an option runs nothing");
    CHECK_MSG(buildCommandArgv("run %s", "--output=/etc/x").empty(), "a long-option match runs nothing");
    CHECK_MSG(buildCommandArgv("run %s", "-").empty(), "a lone dash runs nothing");
    const auto literal = buildCommandArgv("grep -n %s", "pattern");
    CHECK_MSG(literal.size() == 3 && literal[1] == "-n" && literal[2] == "pattern",
        "a template's own -flag is kept, and a safe match still runs");
    const auto safe = buildCommandArgv("run %s", "file-1");
    CHECK_MSG(safe.size() == 2 && safe[1] == "file-1", "a dash inside the match is fine");
}

// A match that looks like the substitution token must not recurse.
TEST(testMatchContainingTheTokenIsNotReExpanded)
{
    const auto argv = buildCommandArgv("run %s", "%s %% %s");
    CHECK_MSG(argv.size() == 2, "the match is still one argument");
    CHECK_MSG(argv[1] == "%s %% %s", "and its tokens are not expanded");
}

TEST(testLiteralPercent)
{
    const auto argv = buildCommandArgv("printf %% done", "ignored");
    CHECK_MSG(argv.size() == 3, "the escape is one word");
    CHECK_MSG(argv[1] == "%", "%% collapses to a literal percent");
    CHECK_MSG(argv[2] == "done", "and the rest follows");
}

// Nothing forces a template to use the match, and a program name alone is a
// valid command.
TEST(testTemplateWithoutSubstitution)
{
    const auto argv = buildCommandArgv("true", "match");
    CHECK_MSG(argv.size() == 1, "a bare program is valid");
    CHECK_MSG(argv[0] == "true", "and is argv[0]");
}

TEST(testRepeatedSubstitution)
{
    const auto argv = buildCommandArgv("cmp %s %s", "same");
    CHECK_MSG(argv.size() == 3, "every %s expands");
    CHECK_MSG(argv[1] == "same" && argv[2] == "same", "each to the whole match");
}

// Only an exact "%s" word substitutes; a word merely containing it is literal,
// so no partial expansion can smuggle the match into a larger argument.
TEST(testOnlyAWholeWordSubstitutes)
{
    const auto argv = buildCommandArgv("run x%s %sy a%sb", "M");
    CHECK_MSG(argv.size() == 4, "three literal words follow the program");
    CHECK_MSG(argv[1] == "x%s", "a prefix makes it literal");
    CHECK_MSG(argv[2] == "%sy", "so does a suffix");
    CHECK_MSG(argv[3] == "a%sb", "and so does being embedded");
}

TEST(testEmptyMatchStillProducesAnArgument)
{
    const auto argv = buildCommandArgv("echo %s", "");
    CHECK_MSG(argv.size() == 2, "an empty match is still an argument");
    CHECK_MSG(argv[1].empty(), "an empty one");
}

// ---- webhook scheme --------------------------------------------------------

TEST(testOnlyHTTPSchemesAreAllowed)
{
    CHECK_MSG(webhookSchemeAllowed("http://example.test/hook"), "http is allowed");
    CHECK_MSG(webhookSchemeAllowed("https://example.test/hook"), "https is allowed");
    CHECK_MSG(webhookSchemeAllowed("HTTPS://example.test/hook"), "the scheme is case-insensitive");
    CHECK_MSG(webhookSchemeAllowed("HtTp://example.test"), "in any mixture");
}

// A webhook must not double as a local opener or a way to reach another handler.
TEST(testNonHTTPSchemesAreRefused)
{
    const char* refused[] = {
        "file:///etc/passwd",
        "ftp://example.test",
        "ws://example.test",
        "javascript:alert(1)",
        "data:text/plain,x",
        "ubiquitty://open",
        "mailto:someone@example.test",
        "httpx://example.test",
        "shttp://example.test",
        "",
        "example.test/hook",
        "http",
        "http:",
        "https:",
        "://example.test",
    };

    for (const char* url : refused)
        CHECK_MSG(!webhookSchemeAllowed(url), "a non-http(s) URL is refused");
}

// ---- webhook body -------------------------------------------------------

TEST(testBodyCarriesTheMatchAsAValue)
{
    CHECK_MSG(buildWebhookBody("hello") == "{\"match\":\"hello\"}", "the match is a JSON value");
    CHECK_MSG(buildWebhookBody("") == "{\"match\":\"\"}", "an empty match is an empty value");
}

// The match is attacker-controlled, so it must not be able to close the string,
// add a field, or otherwise restructure the object.
TEST(testBodyEscapesHostileMatches)
{
    CHECK_MSG(buildWebhookBody("a\"b") == "{\"match\":\"a\\\"b\"}", "a quote is escaped");
    CHECK_MSG(buildWebhookBody("a\\b") == "{\"match\":\"a\\\\b\"}", "a backslash is escaped");
    CHECK_MSG(buildWebhookBody("\",\"evil\":\"1") == "{\"match\":\"\\\",\\\"evil\\\":\\\"1\"}",
        "a break-out attempt cannot add a field");
    CHECK_MSG(buildWebhookBody("a\nb") == "{\"match\":\"a\\nb\"}", "a newline cannot forge a header");
    CHECK_MSG(buildWebhookBody("a\rb") == "{\"match\":\"a\\rb\"}", "and neither can a carriage return");
    CHECK_MSG(buildWebhookBody("a\tb") == "{\"match\":\"a\\tb\"}", "a tab is escaped");
}

// Every C0 control must be escaped: JSON forbids them raw in a string, and a
// server rejecting the body would silently drop the notification.
TEST(testBodyEscapesEveryControlCharacter)
{
    for (int byte = 0x00; byte < 0x20; ++byte) {
        const std::string match(1, static_cast<char>(byte));
        const std::string body = buildWebhookBody(match);
        CHECK_MSG(body.find(static_cast<char>(byte)) == std::string::npos,
            "no raw control byte survives into the body");
        CHECK_MSG(body.size() > match.size() + 12, "it was replaced by an escape");
    }
}

// UTF-8 is valid in a JSON string and must not be mangled.
TEST(testBodyPassesUTF8Through)
{
    CHECK_MSG(buildWebhookBody("héllo 😀") == "{\"match\":\"héllo 😀\"}", "UTF-8 passes through");
}

// DEL is not a C0 control and JSON does not require escaping it.
TEST(testBodyLeavesDeleteAlone)
{
    CHECK_MSG(buildWebhookBody("\x7F") == "{\"match\":\"\x7F\"}", "DEL is not escaped");
}

} // namespace

UBIQUITTY_TEST_MAIN()
