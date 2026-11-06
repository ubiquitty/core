// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Protocol/Control/JSONSchema.cpp — see JSONSchema.h.

#include "Core/Protocol/Control/JSONSchema.h"

#include "Core/Unicode/UTF8Repair.h"

#include <array>
#include <cmath>
#include <string_view>

namespace Ubiquitty::Protocol::Control {

namespace {

using Codec::JSON::Type;
using Codec::JSON::Value;

constexpr std::array<std::string_view, 15> supportedKeywords {
    "type",
    "format",
    "description",
    "properties",
    "required",
    "additionalProperties",
    "enum",
    "minimum",
    "maximum",
    "minLength",
    "maxLength",
    "minItems",
    "maxItems",
    "items",
    "default",
};

constexpr std::array<std::string_view, 7> supportedTypes { "object", "array", "string", "integer", "number", "boolean", "null" };

bool isSupported(std::string_view word, auto const& list)
{
    for (const std::string_view entry : list) {
        if (entry == word)
            return true;
    }
    return false;
}

std::string typeNameOf(const Value& value)
{
    switch (value.type()) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return "a boolean";
    case Type::Number:
        return "a number";
    case Type::String:
        return "a string";
    case Type::Array:
        return "an array";
    case Type::Object:
        return "an object";
    }
    return "a value";
}

bool matchesType(const Value& value, std::string_view type)
{
    if (type == "object")
        return value.isObject();
    if (type == "array")
        return value.isArray();
    if (type == "string")
        return value.isString();
    if (type == "boolean")
        return value.isBool();
    if (type == "null")
        return value.isNull();
    if (type == "number")
        return value.isNumber() && std::isfinite(value.asNumber());
    if (type == "integer")
        return value.isNumber() && std::isfinite(value.asNumber()) && std::trunc(value.asNumber()) == value.asNumber();
    return false;
}

bool sameValue(const Value& a, const Value& b)
{
    if (a.type() != b.type())
        return false;
    switch (a.type()) {
    case Type::Null:
        return true;
    case Type::Bool:
        return a.asBool() == b.asBool();
    case Type::Number:
        return a.asNumber() == b.asNumber();
    case Type::String:
        return a.asString() == b.asString();
    case Type::Array:
    case Type::Object:
        return Codec::JSON::serialize(a) == Codec::JSON::serialize(b);
    }
    return false;
}

std::string quoted(const std::string& text)
{
    return "'" + text + "'";
}

/// Escapes one JSON Pointer token (RFC 6901).
std::string pointerToken(const std::string& key)
{
    std::string out;
    for (const char character : key) {
        if (character == '~')
            out += "~0";
        else if (character == '/')
            out += "~1";
        else
            out += character;
    }
    return out;
}

std::optional<std::string> checkSchemaAt(const Value& schema, const std::string& path)
{
    if (!schema.isObject())
        return "the schema at " + quoted(path) + " is not an object";
    for (const auto& [keyword, value] : schema.members()) {
        if (!isSupported(keyword, supportedKeywords))
            return "the schema at " + quoted(path) + " uses '" + keyword + "', which the validator does not enforce";
        if (keyword == "type" && !(value.isString() && isSupported(value.asString(), supportedTypes)))
            return "the schema at " + quoted(path) + " names no type the validator knows";
        if (keyword == "format" && (!value.isString() || value.asString() != "utf8" || schema.stringOr("type") != "string"))
            return "the schema at " + quoted(path) + " requires format 'utf8' on a string";
        if (keyword == "additionalProperties" && !value.isBool())
            return "the schema at " + quoted(path) + " gives additionalProperties as other than a boolean";
        if ((keyword == "minimum" || keyword == "maximum" || keyword == "minLength" || keyword == "maxLength") && !value.isNumber())
            return "the schema at " + quoted(path) + " gives " + keyword + " as other than a number";
        if ((keyword == "minItems" || keyword == "maxItems") && (!matchesType(value, "integer") || value.asNumber() < 0))
            return "the schema at " + quoted(path) + " gives " + keyword + " as other than a nonnegative integer";
        if (keyword == "enum" && (!value.isArray() || value.items().empty()))
            return "the schema at " + quoted(path) + " gives an enum with no values";
        if (keyword == "required") {
            if (!value.isArray())
                return "the schema at " + quoted(path) + " gives required as other than an array";
            for (const Value& name : value.items()) {
                if (!name.isString())
                    return "the schema at " + quoted(path) + " requires a property that is not named by a string";
            }
        }
        if (keyword == "properties") {
            if (!value.isObject())
                return "the schema at " + quoted(path) + " gives properties as other than an object";
            for (const auto& [name, property] : value.members()) {
                if (auto problem = checkSchemaAt(property, path + "/properties/" + pointerToken(name)))
                    return problem;
            }
        }
        if (keyword == "items") {
            if (auto problem = checkSchemaAt(value, path + "/items"))
                return problem;
        }
    }
    // A required property the schema does not describe cannot be supplied
    // under additionalProperties false, so the method could never be called.
    if (const Value* required = schema.find("required")) {
        const Value* properties = schema.find("properties");
        for (const Value& name : required->items()) {
            if (!properties || !properties->find(name.asString()))
                return "the schema at " + quoted(path) + " requires '" + name.asString() + "' and does not describe it";
        }
    }
    return std::nullopt;
}

std::optional<SchemaViolation> validateAt(const Value& instance, const Value& schema, const std::string& path)
{
    const auto violation = [&](std::string message) {
        return SchemaViolation { path, std::move(message) };
    };
    const std::string where = path.empty() ? std::string("the parameters") : quoted(path.substr(1));

    if (const Value* type = schema.find("type"); type && !matchesType(instance, type->asString()))
        return violation(where + " must be " + (type->asString() == "integer" || type->asString() == "array" || type->asString() == "object" ? "an " : "a ") + type->asString() + ", not " + typeNameOf(instance) + ".");

    if (const Value* values = schema.find("enum")) {
        bool isListed = false;
        std::string listed;
        for (const Value& value : values->items()) {
            isListed = isListed || sameValue(instance, value);
            listed += (listed.empty() ? "" : ", ") + Codec::JSON::serialize(value);
        }
        if (!isListed)
            return violation(where + " must be one of " + listed + ".");
    }

    if (instance.isNumber()) {
        if (const Value* minimum = schema.find("minimum"); minimum && instance.asNumber() < minimum->asNumber())
            return violation(where + " must be at least " + Codec::JSON::serialize(*minimum) + ".");
        if (const Value* maximum = schema.find("maximum"); maximum && instance.asNumber() > maximum->asNumber())
            return violation(where + " must be at most " + Codec::JSON::serialize(*maximum) + ".");
    }

    if (instance.isString()) {
        const double length = static_cast<double>(instance.asString().size());
        if (const Value* minimum = schema.find("minLength"); minimum && length < minimum->asNumber())
            return violation(where + (minimum->asNumber() <= 1 ? std::string(" must not be empty.") : " must be at least " + Codec::JSON::serialize(*minimum) + " bytes long."));
        if (const Value* maximum = schema.find("maxLength"); maximum && length > maximum->asNumber())
            return violation(where + " must be at most " + Codec::JSON::serialize(*maximum) + " bytes long, and this one is " + std::to_string(instance.asString().size()) + ".");
        if (schema.stringOr("format") == "utf8" && Unicode::wellFormedUTF8PrefixLength(instance.asString()) != instance.asString().size())
            return violation(where + " is not valid UTF-8.");
    }

    if (instance.isArray()) {
        if (const Value* minimum = schema.find("minItems"); minimum && static_cast<double>(instance.items().size()) < minimum->asNumber())
            return violation(where + " must contain at least " + Codec::JSON::serialize(*minimum) + " items.");
        if (const Value* maximum = schema.find("maxItems"); maximum && static_cast<double>(instance.items().size()) > maximum->asNumber())
            return violation(where + " must contain at most " + Codec::JSON::serialize(*maximum) + " items.");
        if (const Value* items = schema.find("items")) {
            for (std::size_t index = 0; index < instance.items().size(); ++index) {
                if (auto broken = validateAt(instance.items()[index], *items, path + "/" + std::to_string(index)))
                    return broken;
            }
        }
    }

    if (instance.isObject()) {
        const Value* properties = schema.find("properties");
        if (const Value* required = schema.find("required")) {
            for (const Value& name : required->items()) {
                if (!instance.find(name.asString()))
                    return SchemaViolation { path + "/" + pointerToken(name.asString()), "'" + name.asString() + "' is required." };
            }
        }
        const Value* additional = schema.find("additionalProperties");
        const bool shouldRefuseUnknown = additional && additional->isBool() && !additional->asBool();
        for (const auto& [name, value] : instance.members()) {
            const Value* property = properties ? properties->find(name) : nullptr;
            if (!property) {
                if (shouldRefuseUnknown)
                    return SchemaViolation { path + "/" + pointerToken(name), "'" + name + "' is not a parameter this method takes." };
                continue;
            }
            if (auto broken = validateAt(value, *property, path + "/" + pointerToken(name)))
                return broken;
        }
        // A key given twice is two answers to one question; the reader would
        // take the first and the writer may have meant the second.
        for (std::size_t index = 0; index < instance.members().size(); ++index) {
            for (std::size_t later = index + 1; later < instance.members().size(); ++later) {
                if (instance.members()[index].first == instance.members()[later].first)
                    return SchemaViolation { path + "/" + pointerToken(instance.members()[index].first), "'" + instance.members()[index].first + "' is given more than once." };
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> checkSchema(const Value& schema)
{
    return checkSchemaAt(schema, "");
}

std::optional<SchemaViolation> validate(const Value& instance, const Value& schema)
{
    return validateAt(instance, schema, "");
}

} // namespace Ubiquitty::Protocol::Control
