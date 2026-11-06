// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The subset of JSON Schema the method registry writes its parameter and
// result schemas in, and the validator every request is held to before it has
// any effect.
//
// Supported keywords: `type` (one of object, array, string, integer, number,
// boolean, null), `format`, `description`, `properties`, `required`,
// `additionalProperties` (a boolean only), `enum`, `minimum`, `maximum`,
// `minLength`, `maxLength`, `minItems`, `maxItems`, `items`, and `default`, which documents and
// constrains nothing. A schema using any other keyword is
// refused by ``checkSchema``: a keyword the validator does not read would be a
// constraint the schema advertises and nothing enforces.
//
// `format: "utf8"` requires a string to contain well-formed UTF-8. Other
// formats are refused. Strings without a format may carry raw input bytes.
// Lengths count bytes, which is what every bound downstream measures.

#include "Core/Codec/JSON.h"

#include <optional>
#include <string>

namespace Ubiquitty::Protocol::Control {

/// Where a value broke its schema, and how.
struct SchemaViolation {
    /// A JSON Pointer to the offending value, `""` for the document itself, for
    /// example `/text`.
    std::string path;
    /// A sentence naming the rule broken.
    std::string message;
};

/// Checks that a schema uses only the keywords this validator enforces.
///
/// - Parameter schema: A schema document.
/// - Returns: A sentence naming the first unsupported or malformed keyword, or
///   nothing when the schema is one the validator reads whole.
[[nodiscard]] std::optional<std::string> checkSchema(const Codec::JSON::Value& schema);

/// Validates a value against a schema.
///
/// The schema is trusted: it is one ``checkSchema`` accepted.
///
/// - Parameters:
///   - instance: The value to check, typically a request's parameters.
///   - schema: The schema it must satisfy.
/// - Returns: The first violation found, or nothing when the value conforms.
[[nodiscard]] std::optional<SchemaViolation> validate(const Codec::JSON::Value& instance, const Codec::JSON::Value& schema);

} // namespace Ubiquitty::Protocol::Control
