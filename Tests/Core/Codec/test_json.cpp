// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_json — the JSON value tree, its parser, and its writer.
//
// Every control-socket reply and every agent/MCP response is built by the
// serializer here, and every request arriving on either surface is read by the
// parser. Both sit on a trust boundary: the parser's input comes from whoever
// connected, and the serializer's output is handed to whoever is listening.
//
// The writer's escaping is the part that matters — a quote or a backslash
// reaching the wire unescaped ends the string
// early and hands the reader the rest as structure, which is how a value
// becomes a message.

#include "Core/Codec/JSON.h"

#include "ThirdParty/yyjson/yyjson.h"

#include <cmath>
#include <cstdio>
#include <string>

using Ubiquitty::Codec::JSON::parse;
using Ubiquitty::Codec::JSON::serialize;
using Ubiquitty::Codec::JSON::Type;
using Ubiquitty::Codec::JSON::Value;

namespace {

/// Parses `text`, requiring success.
Value parsed(const char* text)
{
    bool ok = false;
    Value v = parse(std::string(text), ok);
    CHECK_MSG(ok, text);
    return v;
}

/// Whether `text` is refused.
bool refused(const char* text)
{
    bool ok = false;
    const Value v = parse(std::string(text), ok);
    // A refusal answers null as well as clearing the flag, so a caller reading
    // only the value cannot act on rejected input.
    return !ok && v.isNull();
}

} // namespace

TEST(testEveryTypeSurvivesARoundTrip)
{
    // A value written out and read back is the same value. The two directions
    // are separate code, so one can drift without the other — and the writer is
    // what a client sees.
    for (const char* text : { "null", "true", "false", "0", "-1", "42", "3.5",
             "\"\"", "\"plain\"", "[]", "{}", "[1,2,3]", "{\"a\":1}",
             "{\"a\":[1,{\"b\":null}],\"c\":\"d\"}" }) {
        const Value value = parsed(text);
        const std::string written = serialize(value);
        CHECK_STR_EQ(written.c_str(), text);

        bool ok = false;
        const Value again = parse(written, ok);
        CHECK_MSG(ok, text);
        CHECK_MSG(serialize(again) == written, text);
    }
}

TEST(testTheWriterEscapesEveryCharacterThatWouldEndTheString)
{
    // The two that change what the reader sees as structure. Without these the
    // remainder of the value is read as JSON rather than as text, which is how
    // a string a remote program chose becomes a message.
    CHECK_STR_EQ(serialize(Value::string("a\"b")).c_str(), "\"a\\\"b\"");
    CHECK_STR_EQ(serialize(Value::string("a\\b")).c_str(), "\"a\\\\b\"");

    // The five named control escapes, each its own label.
    CHECK_STR_EQ(serialize(Value::string("a\bb")).c_str(), "\"a\\bb\"");
    CHECK_STR_EQ(serialize(Value::string("a\fb")).c_str(), "\"a\\fb\"");
    CHECK_STR_EQ(serialize(Value::string("a\nb")).c_str(), "\"a\\nb\"");
    CHECK_STR_EQ(serialize(Value::string("a\rb")).c_str(), "\"a\\rb\"");
    CHECK_STR_EQ(serialize(Value::string("a\tb")).c_str(), "\"a\\tb\"");

    // Every other control byte takes the \u form, including the one JSON has no
    // name for and the NUL a std::string can hold.
    CHECK_STR_EQ(serialize(Value::string("a\x01"
                                         "b"))
                     .c_str(),
        "\"a\\u0001b\"");
    CHECK_STR_EQ(serialize(Value::string("a\x1F"
                                         "b"))
                     .c_str(),
        "\"a\\u001fb\"");
    CHECK_STR_EQ(serialize(Value::string(std::string("a\0b", 3))).c_str(), "\"a\\u0000b\"");

    // 0x20 is the first byte that passes through, so the boundary is asserted
    // rather than assumed from the two sides of it.
    CHECK_STR_EQ(serialize(Value::string(" ")).c_str(), "\" \"");

    // Bytes above ASCII are not escaped: the output is UTF-8 and a reader
    // decodes it as such. Escaping them would still be valid JSON, so this is
    // asserted rather than left to whichever the writer happens to do.
    CHECK_STR_EQ(serialize(Value::string("\xC3\xA9")).c_str(), "\"\xC3\xA9\""); // é
    CHECK_STR_EQ(serialize(Value::string("\xF0\x9F\x98\x80")).c_str(), "\"\xF0\x9F\x98\x80\"");

    // And an escaped string reads back as the bytes it stood for, so the
    // escaping above is reversible rather than merely well-formed.
    for (const char* awkward : { "a\"b", "a\\b", "a\nb", "a\tb", "\x01\x1F", "\xC3\xA9" }) {
        const std::string written = serialize(Value::string(awkward));
        bool ok = false;
        const Value back = parse(written, ok);
        CHECK_MSG(ok, awkward);
        CHECK_MSG(back.isString() && back.asString() == awkward, awkward);
    }
}

// A string a program chose may hold bytes that are no UTF-8. What the writer
// produces is a JSON document all the same: every reader decodes it, and the
// bytes that were no text read back as U+FFFD.
TEST(testTheWriterNeverEmitsBytesThatAreNotUTF8)
{
    CHECK_STR_EQ(serialize(Value::string("a\xFF"
                                         "b"))
                     .c_str(),
        "\"a\xEF\xBF\xBD"
        "b\"");
    // A sequence cut short is one replacement; a surrogate and an overlong
    // form are refused a byte at a time.
    CHECK_STR_EQ(serialize(Value::string("\xE2\x82")).c_str(), "\"\xEF\xBF\xBD\"");
    CHECK_STR_EQ(serialize(Value::string("\xED\xA0\x80")).c_str(), "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"");
    CHECK_STR_EQ(serialize(Value::string("\xC0\xAF")).c_str(), "\"\xEF\xBF\xBD\xEF\xBF\xBD\"");
    // An ill-formed byte beside characters the writer escapes: each is
    // handled, in order.
    CHECK_STR_EQ(serialize(Value::string("\"\xFF\n\x01")).c_str(), "\"\\\"\xEF\xBF\xBD\\n\\u0001\"");
    // A key is text too.
    Value object = Value::object();
    object.set("k\xFF", Value::string("v\xFE"));
    const std::string written = serialize(object);
    CHECK_STR_EQ(written.c_str(), "{\"k\xEF\xBF\xBD\":\"v\xEF\xBF\xBD\"}");
    bool ok = false;
    const Value back = parse(written, ok);
    REQUIRE(ok);
    CHECK_STR_EQ(back.stringOr("k\xEF\xBF\xBD"), "v\xEF\xBF\xBD");
    // Every byte, alone and behind a lead: the document always parses.
    for (int first = 0; first < 256; ++first) {
        for (int second = 0; second < 256; ++second) {
            const std::string text { 'x', static_cast<char>(first), static_cast<char>(second), 'y' };
            bool isRead = false; // bool-ok: the parser's out flag
            const Value read = parse(serialize(Value::string(text)), isRead);
            if (!isRead || !read.isString()) {
                CHECK_MSG(false, "a written string did not parse");
                return;
            }
        }
    }
}

TEST(testAKeyIsEscapedTheSameWayAValueIs)
{
    // Object keys go through the same writer, and a key is as likely to carry a
    // quote as a value is — it comes from the same place.
    Value object = Value::object();
    object.set("a\"b", Value::number(1));
    object.set("c\nd", Value::string("e\\f"));
    CHECK_STR_EQ(serialize(object).c_str(), "{\"a\\\"b\":1,\"c\\nd\":\"e\\\\f\"}");

    bool ok = false;
    const Value back = parse(serialize(object), ok);
    CHECK(ok);
    CHECK(back.find("a\"b") != nullptr);
    CHECK_STR_EQ(back.stringOr("c\nd").c_str(), "e\\f");
}

TEST(testANumberIsWrittenAsTheKindItIs)
{
    // A JSON-RPC id round-trips as an integer or the peer stops matching
    // replies to requests, so an integral value is written without a fraction.
    CHECK_STR_EQ(serialize(Value::number(0)).c_str(), "0");
    CHECK_STR_EQ(serialize(Value::number(42)).c_str(), "42");
    CHECK_STR_EQ(serialize(Value::number(-42)).c_str(), "-42");
    CHECK_STR_EQ(serialize(Value::number(-0.0)).c_str(), "0");

    // A value with a fraction keeps it, and one large enough to lose precision
    // as an integer is written in the general form instead.
    CHECK_STR_EQ(serialize(Value::number(0.5)).c_str(), "0.5");
    CHECK_STR_EQ(serialize(Value::number(-2.25)).c_str(), "-2.25");
    // Past the integral cutoff the general form takes over. It still reads as
    // an integer here — the two spellings agree at this magnitude — and only
    // becomes an exponent once the digits run out.
    CHECK_STR_EQ(serialize(Value::number(1e15)).c_str(), "1000000000000000");
    CHECK_STR_EQ(serialize(Value::number(-1e15)).c_str(), "-1000000000000000");
    CHECK(serialize(Value::number(1e30)).find('e') != std::string::npos);

    // Just inside the integral cutoff is still an integer, so the boundary is
    // measured from both sides.
    CHECK_STR_EQ(serialize(Value::number(999999999999999.0)).c_str(), "999999999999999");

    // The general form asks for seventeen significant digits, which is what a
    // double needs to survive a round trip and more than a value like this
    // reads as. The output is exact rather than tidy: a peer parses it back to
    // the same double, and a human reading the wire sees the representation.
    CHECK_STR_EQ(serialize(Value::number(0.1)).c_str(), "0.10000000000000001");
    CHECK_EQ(parsed(serialize(Value::number(0.1)).c_str()).asNumber(), 0.1);

    // Neither infinity nor NaN is a JSON number. They cannot arrive from the
    // parser, but a caller building a reply from a computed value can hand one
    // over — so what the writer does with them is worth stating. It writes the
    // C spelling, which no JSON reader accepts: the reply is refused by the
    // peer rather than silently read as something else.
    const std::string infinite = serialize(Value::number(HUGE_VAL));
    const std::string notANumber = serialize(Value::number(std::nan("")));
    CHECK(!infinite.empty());
    CHECK(!notANumber.empty());
    bool ok = true;
    parse(infinite, ok);
    CHECK_MSG(!ok, "an infinity does not survive as JSON");
    ok = true;
    parse(notANumber, ok);
    CHECK_MSG(!ok, "a NaN does not survive as JSON");
}

TEST(testAnEmptyContainerIsNotACommaAndNeitherIsASingleEntry)
{
    // The separator is emitted before every element but the first, so an empty
    // container and a one-element one are the two shapes that would produce a
    // stray comma.
    CHECK_STR_EQ(serialize(Value::array()).c_str(), "[]");
    CHECK_STR_EQ(serialize(Value::object()).c_str(), "{}");

    Value one = Value::array();
    one.push(Value::number(1));
    CHECK_STR_EQ(serialize(one).c_str(), "[1]");

    Value two = Value::array();
    two.push(Value::number(1));
    two.push(Value::null());
    CHECK_STR_EQ(serialize(two).c_str(), "[1,null]");

    Value oneMember = Value::object();
    oneMember.set("a", Value::boolean(true));
    CHECK_STR_EQ(serialize(oneMember).c_str(), "{\"a\":true}");

    Value twoMembers = Value::object();
    twoMembers.set("a", Value::boolean(true));
    twoMembers.set("b", Value::boolean(false));
    CHECK_STR_EQ(serialize(twoMembers).c_str(), "{\"a\":true,\"b\":false}");

    // A default-built value is null, so a caller that forgot to fill one in
    // writes a well-formed reply rather than nothing.
    CHECK_STR_EQ(serialize(Value()).c_str(), "null");
    CHECK_STR_EQ(serialize(Value::null()).c_str(), "null");
}

TEST(testTheReaderRefusesWhatIsNotOneJSONValue)
{
    // Every refusal here is input a connected peer can send, and the contract is
    // that a refusal is reported rather than guessed around.
    CHECK(refused(""));
    CHECK(refused("   "));
    CHECK(refused("{"));
    CHECK(refused("["));
    CHECK(refused("}"));
    CHECK(refused("{\"a\":}"));
    CHECK(refused("{\"a\" 1}"));
    CHECK(refused("{a:1}")); // an unquoted key is JavaScript, not JSON
    CHECK(refused("[1,]")); // a trailing comma likewise
    CHECK(refused("{'a':1}")); // single quotes likewise
    CHECK(refused("nul"));
    CHECK(refused("tru"));
    CHECK(refused("01")); // a leading zero is not a JSON number
    CHECK(refused("+1"));
    CHECK(refused(".5"));
    CHECK(refused("1."));
    CHECK(refused("\"unterminated"));
    CHECK(refused("NaN"));
    CHECK(refused("Infinity"));

    // Two values in one message is not one value, which is what keeps a second
    // request from riding along inside the first.
    CHECK(refused("{} {}"));
    CHECK(refused("1 2"));
    CHECK(refused("null null"));

    // Whitespace around a single value is fine, so the refusals above are about
    // the content rather than about anything unusual failing.
    CHECK(parsed("  {\"a\":1}  \n").isObject());
    CHECK(parsed("\t\r\n 42 ").isNumber());
}

TEST(testTheReaderKeepsMemberOrderAndEveryDuplicate)
{
    // A reply is built by walking members in order, so the order the message
    // stated has to survive the read.
    const Value object = parsed("{\"z\":1,\"a\":2,\"m\":3}");
    const auto& members = object.members();
    CHECK(members.size() == 3);
    CHECK_STR_EQ(members[0].first.c_str(), "z");
    CHECK_STR_EQ(members[1].first.c_str(), "a");
    CHECK_STR_EQ(members[2].first.c_str(), "m");
    CHECK_STR_EQ(serialize(object).c_str(), "{\"z\":1,\"a\":2,\"m\":3}");

    // A duplicate key is kept rather than collapsed, and lookup answers the
    // first. Which one wins decides what a message means when a peer sends two,
    // so it is asserted rather than left to the container.
    const Value duplicated = parsed("{\"a\":1,\"a\":2}");
    CHECK(duplicated.members().size() == 2);
    CHECK_EQ(duplicated.numberOr("a"), 1.0);
    CHECK_STR_EQ(serialize(duplicated).c_str(), "{\"a\":1,\"a\":2}");
}

TEST(testAccessorsAnswerTheFallbackRatherThanTheWrongType)
{
    const Value object = parsed("{\"s\":\"text\",\"n\":7,\"b\":true,\"z\":null,"
                                "\"arr\":[1],\"obj\":{\"k\":1}}");

    CHECK_STR_EQ(object.stringOr("s").c_str(), "text");
    CHECK_EQ(object.numberOr("n"), 7.0);
    CHECK(object.boolOr("b"));

    // A key of the wrong type is the same answer as a key that is absent: the
    // caller keeps its default rather than reading a number out of a string.
    CHECK_STR_EQ(object.stringOr("n", "fallback").c_str(), "fallback");
    CHECK_EQ(object.numberOr("s", -1.0), -1.0);
    CHECK(!object.boolOr("s", false));
    CHECK(object.boolOr("s", true));
    CHECK_STR_EQ(object.stringOr("absent", "fallback").c_str(), "fallback");
    CHECK_EQ(object.numberOr("absent", -1.0), -1.0);
    CHECK(object.boolOr("absent", true));

    // find answers a pointer for what is there and nothing for what is not.
    CHECK(object.find("s") != nullptr);
    CHECK(object.find("absent") == nullptr);
    CHECK(object.find("z") != nullptr && object.find("z")->isNull());

    // A lookup on something that is not an object answers nothing rather than
    // searching an empty member list by accident.
    CHECK(parsed("[1,2]").find("a") == nullptr);
    CHECK(parsed("42").find("a") == nullptr);
    CHECK_STR_EQ(parsed("42").stringOr("a", "fallback").c_str(), "fallback");

    // Every predicate agrees with the type it names, over one value of each.
    CHECK(object.find("s")->isString() && object.find("s")->type() == Type::String);
    CHECK(object.find("n")->isNumber() && object.find("n")->type() == Type::Number);
    CHECK(object.find("b")->isBool() && object.find("b")->type() == Type::Bool);
    CHECK(object.find("z")->isNull() && object.find("z")->type() == Type::Null);
    CHECK(object.find("arr")->isArray() && object.find("arr")->type() == Type::Array);
    CHECK(object.find("obj")->isObject() && object.find("obj")->type() == Type::Object);
    CHECK(object.find("arr")->items().size() == 1);
    CHECK(object.find("obj")->members().size() == 1);
}

TEST(testAStringReadsBackAsTheBytesItsEscapesNamed)
{
    // The reader's side of the escaping. A message carrying these is ordinary:
    // a path with a backslash, a log line with a newline, a name with an accent.
    CHECK_STR_EQ(parsed("\"a\\\"b\"").asString().c_str(), "a\"b");
    CHECK_STR_EQ(parsed("\"a\\\\b\"").asString().c_str(), "a\\b");
    CHECK_STR_EQ(parsed("\"a\\/b\"").asString().c_str(), "a/b");
    CHECK_STR_EQ(parsed("\"a\\bb\"").asString().c_str(), "a\bb");
    CHECK_STR_EQ(parsed("\"a\\fb\"").asString().c_str(), "a\fb");
    CHECK_STR_EQ(parsed("\"a\\nb\"").asString().c_str(), "a\nb");
    CHECK_STR_EQ(parsed("\"a\\rb\"").asString().c_str(), "a\rb");
    CHECK_STR_EQ(parsed("\"a\\tb\"").asString().c_str(), "a\tb");
    CHECK_STR_EQ(parsed("\"\\u0041\"").asString().c_str(), "A");
    CHECK_STR_EQ(parsed("\"\\u00e9\"").asString().c_str(), "\xC3\xA9");

    // A surrogate pair is one character, and the pieces are not characters on
    // their own — so the pair becomes four UTF-8 bytes and a lone half is
    // refused rather than encoded as itself.
    CHECK_STR_EQ(parsed("\"\\ud83d\\ude00\"").asString().c_str(), "\xF0\x9F\x98\x80");
    CHECK(refused("\"\\ud800\""));
    CHECK(refused("\"\\u00g1\""));
    CHECK(refused("\"\\q\""));

    // A raw control byte inside a string is not JSON, which is what stops a
    // newline in a message body from ending the value early.
    CHECK(refused("\"a\nb\""));
}

TEST(testNestingPastTheMaximumIsRefusedAndLegitimateNestingIsNot)
{
    // The maximum exists because converting the parsed document into the value tree
    // recurses, and the input comes off a socket. Both sides of it, so a bound
    // lowered to nothing would fail here rather than quietly refusing real
    // messages.
    std::string deep;
    for (int i = 0; i < 200; ++i)
        deep += '[';
    for (int i = 0; i < 200; ++i)
        deep += ']';
    bool ok = true;
    parse(deep, ok);
    CHECK_MSG(!ok, "nesting past the maximum is refused");

    std::string deepObject;
    for (int i = 0; i < 200; ++i)
        deepObject += "{\"a\":";
    deepObject += "1";
    for (int i = 0; i < 200; ++i)
        deepObject += '}';
    ok = true;
    parse(deepObject, ok);
    CHECK_MSG(!ok, "the bound covers objects as well as arrays");

    // A message nested as deeply as any real one still parses.
    std::string shallow;
    for (int i = 0; i < 32; ++i)
        shallow += '[';
    for (int i = 0; i < 32; ++i)
        shallow += ']';
    ok = false;
    const Value value = parse(shallow, ok);
    CHECK_MSG(ok, "ordinary nesting still parses");
    CHECK(value.isArray());
}

TEST(testANodeTypeTheConverterDoesNotHandleIsRefused)
{
    // The reader can be configured to hand back a number as a raw string —
    // which is what a caller wanting big integers past a double's range would
    // reach for — and the converter has no case for that node type. It refuses
    // rather than dropping the value or reading the union as something else,
    // and this is the only way to reach that arm: the strict flags the shipped
    // parser uses produce only the six JSON types.
    bool ok = true;
    Value value = Ubiquitty::Codec::JSON::ForTest::parseWithReadFlags(
        "12345678901234567890123", YYJSON_READ_NUMBER_AS_RAW, ok);
    CHECK_MSG(!ok, "a raw node is refused rather than converted");
    CHECK(value.isNull());

    // Nested, so the refusal comes back through the recursion rather than only
    // from the root.
    ok = true;
    value = Ubiquitty::Codec::JSON::ForTest::parseWithReadFlags(
        "[1]", YYJSON_READ_NUMBER_AS_RAW, ok);
    CHECK_MSG(!ok, "a raw node inside an array is refused too");
    ok = true;
    value = Ubiquitty::Codec::JSON::ForTest::parseWithReadFlags(
        "{\"a\":1}", YYJSON_READ_NUMBER_AS_RAW, ok);
    CHECK_MSG(!ok, "and inside an object");

    // The control: the same seam with the flags the shipped parser uses reads
    // the same documents, so the refusals above are about the node type rather
    // than about this entry point refusing everything.
    ok = false;
    value = Ubiquitty::Codec::JSON::ForTest::parseWithReadFlags(
        "{\"a\":1}", YYJSON_READ_NOFLAG, ok);
    CHECK(ok);
    CHECK_EQ(value.numberOr("a"), 1.0);

    // And a document with no number is unaffected by the flag, so what the flag
    // changes is the number rather than the parse.
    ok = false;
    value = Ubiquitty::Codec::JSON::ForTest::parseWithReadFlags(
        "{\"a\":\"text\"}", YYJSON_READ_NUMBER_AS_RAW, ok);
    CHECK(ok);
    CHECK_STR_EQ(value.stringOr("a").c_str(), "text");
}

TEST(testATypeTheWriterHasNoCaseForContributesNothing)
{
    // Every factory stamps one of the six types and the writer switches over
    // all six, so this is unreachable through the interface. It is asserted
    // because of what it says about the next enumerator: a `Type` added without
    // a matching case writes nothing at all, and nothing is not a JSON value.
    const Value unknown = Value::withTypeForTest(static_cast<Type>(7));
    CHECK_MSG(serialize(unknown).empty(), "an unhandled type writes nothing at all");

    // Which inside a container is the part that bites: the separator is still
    // emitted, so the reply is malformed rather than merely missing a member.
    // A reader refuses the whole message, not the one value.
    Value array = Value::array();
    array.push(Value::number(1));
    array.push(unknown);
    array.push(Value::number(2));
    const std::string written = serialize(array);
    CHECK_STR_EQ(written.c_str(), "[1,,2]");
    bool ok = true;
    parse(written, ok);
    CHECK_MSG(!ok, "and no reader accepts it");

    // The control: the same shape with a value the writer knows is well-formed,
    // so the refusal above is about the unhandled type.
    Value known = Value::array();
    known.push(Value::number(1));
    known.push(Value::null());
    known.push(Value::number(2));
    CHECK_STR_EQ(serialize(known).c_str(), "[1,null,2]");
}

UBIQUITTY_TEST_MAIN()
