// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Codec/JSON.cpp — see JSON.h. Backed by vendored yyjson, which
// parses iteratively. Converting the yyjson document into the JSON::Value tree is
// recursive, so the nesting-depth DoS cap lives in convertValue(): input nested
// past maxDepth is rejected via ok=false before the conversion recurses deep.
// Object member order and duplicate keys are preserved as they appear.
//
// Serialization: compact output, integral finite numbers below 1e15 printed
// as integers (JSON-RPC ids round-trip as "42", not "42.0").
//

#include "Core/Codec/JSON.h"

#include "Core/Unicode/UTF8Repair.h"

#include "ThirdParty/yyjson/yyjson.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace Ubiquitty::Codec::JSON {

namespace {

// Nesting-depth DoS cap (a value at nesting level `depth` is rejected when
// depth >= 128).
// Far beyond any real JSON-RPC message, far below the stack-frame ceiling of
// the recursive yyjson -> Value conversion below.
constexpr int maxDepth = 128;

// Convert a parsed yyjson node into a JSON::Value. Returns false (and leaves
// `out` null) when nesting exceeds maxDepth — the depth check runs before any
// recursion, so conversion never descends more than maxDepth frames.
bool convertValue(yyjson_val* node, Value& out, int depth)
{
    if (depth >= maxDepth)
        return false;
    switch (yyjson_get_type(node)) {
    case YYJSON_TYPE_NULL:
        out = Value::null();
        return true;
    case YYJSON_TYPE_BOOL:
        out = Value::boolean(yyjson_get_bool(node));
        return true;
    case YYJSON_TYPE_NUM:
        // All numeric subtypes (sint/uint/real) collapse to double — Value
        // carries one number representation and the serializer re-derives
        // integer formatting from the value itself.
        out = Value::number(yyjson_get_num(node));
        return true;
    case YYJSON_TYPE_STR:
        out = Value::string(std::string(yyjson_get_str(node), yyjson_get_len(node)));
        return true;
    case YYJSON_TYPE_ARR: {
        Value array = Value::array();
        yyjson_arr_iter iterator = yyjson_arr_iter_with(node);
        while (yyjson_val* item = yyjson_arr_iter_next(&iterator)) {
            Value element;
            if (!convertValue(item, element, depth + 1))
                return false;
            array.push(std::move(element));
        }
        out = std::move(array);
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        // yyjson_obj_iter visits every member in document order, including
        // duplicate keys — every member is appended, and find() resolves
        // duplicates to the first occurrence.
        Value object = Value::object();
        yyjson_obj_iter iterator = yyjson_obj_iter_with(node);
        while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
            yyjson_val* memberValue = yyjson_obj_iter_get_val(key);
            Value member;
            if (!convertValue(memberValue, member, depth + 1))
                return false;
            object.set(std::string(yyjson_get_str(key), yyjson_get_len(key)), std::move(member));
        }
        out = std::move(object);
        return true;
    }
    default:
        return false;
    }
}

void serializeString(const std::string& text, std::string& out)
{
    // A string a caller built from a program's bytes may hold some that are
    // no UTF-8, and one such byte makes the whole document undecodable. What
    // is written is always text: an ill-formed sequence is written as U+FFFD.
    // Text, the usual case, is written from where it is.
    std::string repaired;
    if (Unicode::wellFormedUTF8PrefixLength(text) != text.size()) {
        repaired = text;
        (void)Unicode::repairUTF8(repaired);
    }
    const std::string& s = repaired.empty() ? text : repaired;
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                std::array<char, 8> escapeBuffer;
                std::snprintf(escapeBuffer.data(), escapeBuffer.size(), "\\u%04x", c);
                out += escapeBuffer.data();
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void serializeValue(const Value& value, std::string& out);

void serializeNumber(double n, std::string& out)
{
    char buffer[32];
    if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15)
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(n));
    else
        std::snprintf(buffer, sizeof(buffer), "%.17g", n);
    out += buffer;
}

void serializeArray(const Value& value, std::string& out)
{
    out.push_back('[');
    const auto& items = value.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i)
            out.push_back(',');
        serializeValue(items[i], out);
    }
    out.push_back(']');
}

void serializeObject(const Value& value, std::string& out)
{
    out.push_back('{');
    const auto& members = value.members();
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i)
            out.push_back(',');
        serializeString(members[i].first, out);
        out.push_back(':');
        serializeValue(members[i].second, out);
    }
    out.push_back('}');
}

void serializeValue(const Value& value, std::string& out)
{
    switch (value.type()) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += value.asBool() ? "true" : "false";
        break;
    case Type::Number:
        serializeNumber(value.asNumber(), out);
        break;
    case Type::String:
        serializeString(value.asString(), out);
        break;
    case Type::Array:
        serializeArray(value, out);
        break;
    case Type::Object:
        serializeObject(value, out);
        break;
    }
}

} // namespace

namespace {

Value readAndConvert(const std::string& text, yyjson_read_flag flags, bool& ok)
{
    ok = false;
    yyjson_doc* document = yyjson_read(text.data(), text.size(), flags);
    if (!document)
        return Value::null();
    Value value;
    ok = convertValue(yyjson_doc_get_root(document), value, 0);
    yyjson_doc_free(document);
    if (!ok)
        return Value::null();
    return value;
}

} // namespace

Value parse(const std::string& text, bool& ok)
{
    // Default flags: strict RFC 8259 (whole input must be one JSON value;
    // leading/trailing whitespace allowed). yyjson never throws and parses
    // iteratively, so hostile nesting cannot crash the read itself.
    return readAndConvert(text, YYJSON_READ_NOFLAG, ok);
}

#if UBIQUITTY_ENABLE_TEST_HOOKS
namespace ForTest {

Value parseWithReadFlags(const std::string& text, unsigned readFlags, bool& ok)
{
    return readAndConvert(text, static_cast<yyjson_read_flag>(readFlags), ok);
}

} // namespace ForTest
#endif

std::string serialize(const Value& value)
{
    std::string out;
    serializeValue(value, out);
    return out;
}

} // namespace Ubiquitty::Codec::JSON
