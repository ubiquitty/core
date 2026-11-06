// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A minimal, exception-free JSON value + parser + serializer, just enough for the
// MCP server's JSON-RPC framing. Parses the subset MCP sends and serializes compact
// JSON. Backed by vendored yyjson (the nesting-depth DoS cap lives in JSON.cpp).
// No exceptions: parse() reports failure via an out `ok` flag.

#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Codec::JSON {

/// Which JSON value a node holds.
enum class Type {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class Value {
public:
    Value() = default;

    /// A JSON null.
    ///
    /// - Returns: A value whose type is Null.
    static Value null() { return {}; }
    static Value boolean(bool b)
    {
        Value v;
        v.m_type = Type::Bool;
        v.m_bool = b;
        return v;
    }
    static Value number(double n)
    {
        Value v;
        v.m_type = Type::Number;
        v.m_number = n;
        return v;
    }
    static Value string(std::string s)
    {
        Value v;
        v.m_type = Type::String;
        v.m_string = std::move(s);
        return v;
    }
    static Value array()
    {
        Value v;
        v.m_type = Type::Array;
        return v;
    }
    static Value object()
    {
        Value v;
        v.m_type = Type::Object;
        return v;
    }

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool isObject() const { return m_type == Type::Object; }
    [[nodiscard]] bool isArray() const { return m_type == Type::Array; }
    [[nodiscard]] bool isString() const { return m_type == Type::String; }
    [[nodiscard]] bool isNumber() const { return m_type == Type::Number; }
    [[nodiscard]] bool isBool() const { return m_type == Type::Bool; }
    [[nodiscard]] bool isNull() const { return m_type == Type::Null; }

    [[nodiscard]] const std::string& asString() const { return m_string; }
    [[nodiscard]] double asNumber() const { return m_number; }
    [[nodiscard]] bool asBool() const { return m_bool; }

    // Builders (object/array).
    void set(std::string key, Value value) { m_members.emplace_back(std::move(key), std::move(value)); }
    void push(Value value) { m_items.push_back(std::move(value)); }

    // Object lookup; nullptr when absent or not an object.
    [[nodiscard]] const Value* find(const std::string& key) const
    {
        if (m_type != Type::Object)
            return nullptr;
        for (const auto& member : m_members)
            if (member.first == key)
                return &member.second;
        return nullptr;
    }
    [[nodiscard]] std::string stringOr(const std::string& key, const std::string& fallback = std::string()) const
    {
        const Value* v = find(key);
        return (v && v->isString()) ? v->m_string : fallback;
    }
    [[nodiscard]] double numberOr(const std::string& key, double fallback = 0.0) const
    {
        const Value* v = find(key);
        return (v && v->isNumber()) ? v->m_number : fallback;
    }
    // `fallback` is the value returned when the key is absent — the same
    // parameter numberOr takes as a double. An enum would name the two values
    // of the result type rather than a mode the call is in.
    [[nodiscard]] bool boolOr(const std::string& key, bool fallback = false) const // bool-ok: a returned value, not a mode
    {
        const Value* v = find(key);
        return (v && v->isBool()) ? v->m_bool : fallback;
    }

    [[nodiscard]] const std::vector<Value>& items() const { return m_items; }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }

#if UBIQUITTY_ENABLE_TEST_HOOKS
    /// Builds a value carrying a type none of the factories above produces.
    ///
    /// The writer switches over every `Type` and the factories are the only
    /// producers, so what it does with one outside that set is unreachable
    /// through the interface — and worth stating anyway, because it is what an
    /// enumerator added without a matching case would meet.
    ///
    /// - Parameter type: The type to stamp on an otherwise empty value.
    /// - Returns: A value whose type is `type` and which holds nothing else.
    static Value withTypeForTest(Type type)
    {
        Value v;
        v.m_type = type;
        return v;
    }
#endif

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items; // array
    std::vector<std::pair<std::string, Value>> m_members; // object (insertion order)
};

// Parse exactly one JSON value (leading/trailing whitespace allowed). On
// malformed input sets ok=false and returns null.
Value parse(const std::string& text, bool& ok);

// Compact serialization (no insignificant whitespace), valid JSON.
std::string serialize(const Value&);

#if UBIQUITTY_ENABLE_TEST_HOOKS
namespace ForTest {

/// Parses with explicit reader flags rather than the strict defaults.
///
/// The conversion from the parsed document into a `Value` refuses any node type
/// it does not handle. The default flags produce only the six JSON types, so
/// that arm carries a reader configured differently — a number read as a raw
/// string, say — and there is no other way to reach it.
///
/// - Parameters:
///   - text: The document to read.
///   - readFlags: The underlying reader's flags.
///   - ok: Set to whether the parse and the conversion both succeeded.
/// - Returns: The converted value, or null when either step failed.
Value parseWithReadFlags(const std::string& text, unsigned readFlags, bool& ok);

} // namespace ForTest
#endif

} // namespace Ubiquitty::Codec::JSON
