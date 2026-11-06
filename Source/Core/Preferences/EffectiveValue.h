// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"

#include <optional>
#include <string>

// One preference addressed by its key, read and written without knowing which
// field it is.
//
// A settings row needs four facts about a key: `writeDescriptors` gives its
// section, name and serializer, `Schema::rangeFor` gives its bounds,
// `vocabularyFor` gives its tokens, and this gives what the key reads as right
// now. `Effective::` resolves each key through an accessor with its own name and
// its own return type, which a frontend building a row per key cannot ask.
//
// A frontend that resolves a key itself spells the default a second time, with
// nothing comparing the two: its interop can answer `follow_system` off while
// Core answers on.

namespace Ubiquitty::Preferences {

/// One preference's live value, tagged with how it is written.
///
/// The tag travels with the value rather than being fetched beside it. A caller
/// that pairs a value with a descriptor it looked up separately can pair the
/// wrong one, and this makes that unrepresentable instead of merely unlikely.
///
/// One slot is meaningful per kind — `boolean` for `Bool` and
/// `LegacyBoolOrToken`, `integer` for `Int`, `number` for `Double`, `text` for
/// `String` and `EnumToken`. The others hold their zero and mean nothing.
struct Value {
    SerializeKind kind = SerializeKind::Bool;
    bool boolean = false; // bool-ok: a value slot named for its kind, not a flag steering anything
    long long integer = 0;
    double number = 0.0;
    std::string text;

    /// A value in the boolean slot.
    ///
    /// These four exist so no caller writes a designated initializer that names
    /// some members and leaves the rest — GCC refuses that under
    /// `-Werror=missing-field-initializers` where clang accepts it, so the
    /// partial form compiles on one arm and not the other. They also put the
    /// slot and the kind in one place, which is the pairing a caller can
    /// otherwise get wrong.
    ///
    /// - Parameters:
    ///   - kind: The key's serializer.
    ///   - value: The resolved value.
    /// - Returns: A `Value` whose other slots hold their zero.
    [[nodiscard]] static Value ofBool(SerializeKind kind, bool value) // bool-ok: the payload being carried, not a flag steering the call
    {
        Value result;
        result.kind = kind;
        result.boolean = value;
        return result;
    }

    /// A value in the integer slot. See ``ofBool`` for why these exist.
    ///
    /// - Parameters:
    ///   - kind: The key's serializer.
    ///   - value: The resolved value.
    /// - Returns: A `Value` whose other slots hold their zero.
    [[nodiscard]] static Value ofInteger(SerializeKind kind, long long value)
    {
        Value result;
        result.kind = kind;
        result.integer = value;
        return result;
    }

    /// A value in the number slot. See ``ofBool`` for why these exist.
    ///
    /// - Parameters:
    ///   - kind: The key's serializer.
    ///   - value: The resolved value.
    /// - Returns: A `Value` whose other slots hold their zero.
    [[nodiscard]] static Value ofNumber(SerializeKind kind, double value)
    {
        Value result;
        result.kind = kind;
        result.number = value;
        return result;
    }

    /// A value in the text slot. See ``ofBool`` for why these exist.
    ///
    /// - Parameters:
    ///   - kind: The key's serializer.
    ///   - value: The resolved value.
    /// - Returns: A `Value` whose other slots hold their zero.
    [[nodiscard]] static Value ofText(SerializeKind kind, std::string value)
    {
        Value result;
        result.kind = kind;
        result.text = std::move(value);
        return result;
    }
};

/// What a preference reads as, resolved the way the app resolves it.
///
/// Where an `Effective::` accessor exists this defers to it rather than reading
/// the field, so the two cannot disagree: `blur_radius` falls back through the
/// theme, `titleless_padding_top` clamps, and a caller
/// asking by key gets the same answer as a caller asking by name. Where no
/// accessor exists the field is read against its canonical default.
///
/// - Parameters:
///   - key: The preference to resolve.
///   - values: The preferences to resolve it against, normally ``values()``.
/// - Returns: Its live value, or nothing when `key` names no enumerator — which
///   only a value cast in from outside the enumeration can do.
[[nodiscard]] std::optional<Value> effectiveValue(Key key, const Values& values);

/// Writes a preference back to the file a person edits.
///
/// `descriptorFor`, `serializeWrite` and `writeKeyValue` behind one call,
/// because a frontend assembling those three itself is a fourth place for the
/// section, the name and the serializer to drift apart — and there were already
/// three.
///
/// - Parameters:
///   - key: The preference to write.
///   - value: Its new value. Its `kind` must match the key's serializer.
///   - path: The file to write, normally the configuration path.
/// - Returns: Whether the file was written. False when `key` names no
///   enumerator, when `value.kind` disagrees with the descriptor, or when the
///   write itself failed.
/// Writes a preference into an in-memory configuration body.
///
/// The same edit as the path overload for a caller that holds the bytes rather
/// than a name. PAL keys its preference storage by blob, and a backend is free
/// not to publish where a blob lives — the browser has no file at all, and the
/// POSIX backend keeps its `blobPath` private — so a frontend can reach the
/// text through `preferencesLoad` and still have nothing to pass as `path`.
///
/// - Parameters:
///   - key: The preference to write.
///   - value: Its new value. Its `kind` must match the key's serializer.
///   - body: The current text, empty for a store with nothing in it yet.
/// - Returns: The new text, or nothing when `key` names no enumerator or
///   `value.kind` disagrees with the descriptor — the same two refusals the
///   path overload answers false for, kept distinguishable from a body that
///   legitimately came back unchanged.
///
/// Named apart from the path overload rather than overloading it: both would
/// take a `std::string` and every call passing one would be ambiguous.
[[nodiscard]] std::optional<std::string> writeValueIntoBody(Key key, const Value& value, std::string body);

[[nodiscard]] bool writeValue(Key key, const Value& value, const std::string& path);

/// Changes one key by rewriting the live document and configuring from it.
///
/// The settings-surface path. The edit is the by-key writer's: an existing
/// assignment is rewritten in place keeping its indent and trailing comment, a
/// commented-out one is uncommented, a missing key is appended inside its
/// section, a missing section is appended whole. What the rewrite does not
/// keep: the replaced value's own spelling (a `12` written back as a double is
/// `12.0`, a literal string becomes a basic string), CRLF line endings (the
/// whole document comes back LF), a file without a final newline (one is
/// added), and a second assignment of the same key (only the first is
/// rewritten). A key set under a dotted header (`[profile.font]`) is
/// not matched; the write lands in the plain section.
///
/// - Parameters:
///   - key: The preference to change.
///   - value: Its new value, in the key's serializer kind.
/// - Returns: The new document for the frontend to persist, or nothing when
///   `value.kind` disagrees with the key — in which case nothing changed.
std::optional<std::string> configureKey(Key key, const Value& value);

/// ``configureKey``, then the new document stored where the platform keeps it
/// — the PAL configuration blob, on a desktop the file a person edits — as the
/// app's own edit (SelfWriteGuard.h), so a watcher does not reload it.
///
/// What a settings row calls: the live values, every subscriber and the stored
/// document move together, from one edit.
///
/// - Parameters:
///   - key: The preference to change.
///   - value: Its new value, in the key's serializer kind.
/// - Returns: The new document, or nothing when `value.kind` disagrees with
///   the key — in which case nothing changed and nothing was written.
std::optional<std::string> configureOwnKey(Key key, const Value& value);

} // namespace Ubiquitty::Preferences
