// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_json_depth — the agent JSON parser's recursion-depth guard.
//
// JSON::parse drives a mutually-recursive descent (parseValue ->
// parseObject/parseArray -> parseValue), one native stack frame per nesting
// level. The parser is fed untrusted MCP messages straight off a client's
// stdin (AgentMCP processLine -> handleMessage -> JSON::parse), pre-validation,
// so a deeply nested "[[[[..." or "{\"a\":{\"a\":{..." would exhaust the stack
// and hard-crash (SIGSEGV) under -fno-exceptions — a remote DoS.
//
// Parser::maxDepth caps recursion: input nested past the cap is rejected via
// the ordinary ok=false path with no crash. These cases pin both halves of the
// contract: hostile depth is refused cleanly, and legitimately-nested messages
// still parse.

#include "Core/Codec/JSON.h"
#include <cstdio>
#include <string>

using Ubiquitty::Codec::JSON::parse;
using Ubiquitty::Codec::JSON::Value;

// A hostile payload of `count` nested arrays: "[[[[...". Never closed — but the
// guard must reject it long before the parser cares about the missing ']'s, so
// the open prefix alone is the attack.
// The braced form selects std::string's initializer_list<char> constructor —
// two characters — where this wants the (count, char) fill. The narrowing
// from size_t to char is what makes it a compile error rather than a string
// of the wrong length.
// NOLINTNEXTLINE(modernize-return-braced-init-list)
static std::string nestedArrays(int count) { return std::string(static_cast<std::size_t>(count), '['); }

// A hostile payload of `count` nested objects: {"a":{"a":{"a":... — the other
// mutually-recursive descent path (parseObject), exercising the same guard.
static std::string nestedObjects(int count)
{
    std::string out;
    out.reserve(static_cast<std::size_t>(count) * 5);
    for (int i = 0; i < count; ++i)
        out += "{\"a\":";
    return out;
}

// (a) Hostile depth — ~200000 levels on each recursive path. Must be rejected
// (ok==false) and must not crash; reaching this assertion at all proves the
// stack survived. Without the depth guard this input segfaults instead.
//
// (The returned Value is unspecified — the parser hands back whatever partial
// container it had built when the guard tripped — so only ok matters here: a
// false ok is the caller's signal to discard the message entirely.)
TEST(testHostileDepthRejected)
{
    const int hostileDepth = 200000;

    {
        bool ok = true;
        parse(nestedArrays(hostileDepth), ok);
        CHECK(!ok); // rejected, not parsed (and we got here = no crash)
    }
    {
        bool ok = true;
        parse(nestedObjects(hostileDepth), ok);
        CHECK(!ok);
    }
}

// (b) Legitimately-nested message — ~20 deep, fully balanced/valid. The cap
// (128) sits far above any real JSON-RPC message, so this must parse cleanly.
// Guards against a regression that sets the cap too low and breaks real input.
TEST(testLegitNestingAccepted)
{
    const int realDepth = 20;

    // 20 nested arrays wrapping a number: [[[...[42]...]]]
    {
        std::string text = nestedArrays(realDepth) + "42" + std::string(static_cast<std::size_t>(realDepth), ']');
        bool ok = false;
        Value v = parse(text, ok);
        CHECK(ok); // the cap does not break real messages
        CHECK(v.isArray());
    }

    // 20 nested objects bottoming out in a string: {"a":{"a":...{"a":"deep"}...}}
    {
        std::string text = nestedObjects(realDepth) + "\"deep\"" + std::string(static_cast<std::size_t>(realDepth), '}');
        bool ok = false;
        Value v = parse(text, ok);
        CHECK(ok);
        CHECK(v.isObject());
        // Walk down to confirm it built the full structure, not a truncated stub.
        const Value* cursor = &v;
        int seen = 0;
        while (cursor && cursor->isObject()) {
            const Value* next = cursor->find("a");
            if (!next)
                break;
            cursor = next;
            ++seen;
        }
        CHECK(seen == realDepth);
        CHECK(cursor && cursor->isString() && cursor->asString() == "deep");
    }
}

UBIQUITTY_TEST_MAIN()
