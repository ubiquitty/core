// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Typed identities for everything a person or an agent can address: sessions,
// windows, tabs, panes, and attached clients.
//
// An identity is a kind and a number, written `pane:12` on every outward
// surface so a pasted ID carries its own type. Numbers come from an
// `Allocator`, which never hands the same number out twice for its lifetime
// and refuses rather than wrapping: an ID a script saved can never come to name
// a different object. Because of that, a label kept for an ID that has since
// ended can never attach itself to something new.
//
// Labels are optional, mutable, and separate from identity. `resolveTarget` is
// the one place a target a person typed — an ID, a label, or `here` — becomes an
// identity, and it refuses an ambiguous label rather than picking one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Session::Identity {

/// What an identity names.
///
/// Declared from the outermost container to the innermost, which is the order
/// `here` falls back through when a target names no kind.
enum class ObjectKind : std::uint8_t {
    /// The windows, tabs and panes a person attaches to as one thing.
    Workspace,
    Session,
    Window,
    Tab,
    Pane,
    Client,
};

/// The number of `ObjectKind` enumerators.
inline constexpr std::size_t objectKindCount = 6;

/// Every kind, in declaration order.
inline constexpr std::array<ObjectKind, objectKindCount> allObjectKinds {
    ObjectKind::Workspace,
    ObjectKind::Session,
    ObjectKind::Window,
    ObjectKind::Tab,
    ObjectKind::Pane,
    ObjectKind::Client,
};

/// Returns the word a kind is written with: `workspace`, `session`, `window`,
/// `tab`, `pane`, or `client`.
///
/// - Parameter kind: The kind to name.
/// - Returns: The lowercase word, never empty.
[[nodiscard]] std::string_view kindName(ObjectKind kind);

/// Returns the kind a word names.
///
/// - Parameter word: A kind's word, exactly as ``kindName(_:)`` writes it.
/// - Returns: The kind, or nothing for a word that names none.
[[nodiscard]] std::optional<ObjectKind> kindNamed(std::string_view word);

/// One object's identity: what it is and the number it was minted with.
///
/// Number zero is never minted, so a zero identity names nothing.
struct ObjectID {
    ObjectKind kind = ObjectKind::Session;
    std::uint64_t number = 0;

    friend bool operator==(const ObjectID&, const ObjectID&) = default;

    /// Reports whether the identity names an object at all.
    ///
    /// - Returns: Whether the number is nonzero.
    [[nodiscard]] bool isValid() const { return number != 0; }
};

/// Writes an identity as `kind:number`, the one spelling every surface uses.
///
/// - Parameter id: The identity to write.
/// - Returns: For example `pane:12`.
[[nodiscard]] std::string format(ObjectID id);

/// Reads an identity written as `kind:number`.
///
/// Strict: the kind must be one ``kindName(_:)`` writes, the number decimal
/// digits with no sign, no leading zero, and no larger than 2^64 - 1, and
/// nothing may follow it.
///
/// - Parameter text: The text to read.
/// - Returns: The identity, or nothing when the text is not exactly one.
[[nodiscard]] std::optional<ObjectID> parseObjectID(std::string_view text);

/// Mints identities that are never reused for the allocator's lifetime.
///
/// Each kind counts on its own from one. A kind that reaches the ceiling stops
/// minting rather than wrapping, because a wrapped number would name a new
/// object with an ID something may still hold for an old one.
class Allocator {
public:
    /// Exact next-number state, including exhausted and no-longer-live identities.
    struct Continuation {
        std::array<std::uint64_t, objectKindCount> next {};
        std::uint64_t ceiling = UINT32_MAX;
        friend bool operator==(const Continuation&, const Continuation&) = default;
    };

    /// Copies the allocator's exact high-water marks without minting an identity.
    /// - Returns: All kind counters and the original policy ceiling.
    [[nodiscard]] Continuation continuation() const { return { m_next, m_ceiling }; }

    /// Restores a fresh allocator without advancing or normalizing any counter.
    /// - Parameter state: Exact counters with the same receiver-selected ceiling.
    /// - Returns: Whether the receiver was fresh and the policy matched.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    /// Creates an allocator whose numbers stay at or below `ceiling`.
    ///
    /// - Parameter ceiling: The largest number minted; the wire carries
    ///   identities as 32-bit numbers, which is the default.
    explicit Allocator(std::uint64_t ceiling = UINT32_MAX);

    /// Mints the next identity of a kind.
    ///
    /// - Parameter kind: The kind to mint.
    /// - Returns: A fresh identity, or nothing once the kind is exhausted.
    [[nodiscard]] std::optional<ObjectID> mint(ObjectKind kind);

    /// Advances a kind past an identity minted elsewhere and adopted here, so
    /// no later mint repeats it.
    ///
    /// - Parameter id: The adopted identity; a zero number changes nothing.
    void reserveThrough(ObjectID id);

    /// Returns the number the next mint of a kind would take.
    ///
    /// - Parameter kind: The kind to ask about.
    /// - Returns: The next number, which exceeds the ceiling once exhausted.
    [[nodiscard]] std::uint64_t nextNumber(ObjectKind kind) const { return m_next[std::to_underlying(kind)]; }

private:
    std::array<std::uint64_t, objectKindCount> m_next;
    std::uint64_t m_ceiling;
};

/// The longest label accepted, in bytes.
inline constexpr std::size_t maxLabelLength = 64;

/// Why a label was or was not stored.
enum class LabelVerdict : std::uint8_t {
    /// Stored, or cleared by an empty label.
    Accepted,
    /// Longer than ``maxLabelLength``.
    TooLong,
    /// Carries a control character, which no prompt can type back.
    ControlCharacter,
    /// Reads as an identity or a bare number, which a target would resolve as
    /// an identity first and so never reach the label.
    LooksLikeID,
    /// The word `here`, which a target always resolves to the caller.
    Reserved,
    /// The book holds as many labels as it may.
    Full,
};

/// Checks whether text may be used as a label.
///
/// An empty label is accepted: it is how a label is cleared.
///
/// - Parameter label: The proposed label, UTF-8.
/// - Returns: ``LabelVerdict/Accepted`` or the reason it is refused.
[[nodiscard]] LabelVerdict checkLabel(std::string_view label);

/// Returns the sentence a refused label is reported with.
///
/// - Parameter verdict: A verdict from ``checkLabel(_:)`` or ``LabelBook``.
/// - Returns: A sentence, empty for ``LabelVerdict/Accepted``.
[[nodiscard]] std::string_view labelVerdictSentence(LabelVerdict verdict);

/// The labels of live objects, keyed by identity.
///
/// An object of any kind is labelled the same way: a workspace, a session, a
/// window, a tab, a pane and a client share the one validation, the one
/// length bound and the one capacity.
///
/// Bounded: a book that is full refuses a new label instead of growing.
class LabelBook {
public:
    /// The most labels a book holds at once.
    static constexpr std::size_t capacity = 4096;

    /// Sets or clears an object's label.
    ///
    /// - Parameters:
    ///   - id: The object to label.
    ///   - label: The new label; empty clears it.
    /// - Returns: Whether it was stored, and why not when it was not.
    LabelVerdict set(ObjectID id, std::string_view label);

    /// Returns an object's label.
    ///
    /// - Parameter id: The object to look up.
    /// - Returns: The label, empty when it has none. Borrowed until the next
    ///   mutation.
    [[nodiscard]] std::string_view label(ObjectID id) const;

    /// Drops an object's label when the object ends.
    ///
    /// - Parameter id: The object that ended.
    void forget(ObjectID id);

    /// Drops the label of every object a predicate reports as ended.
    ///
    /// Identities are never reused, so a stale label is never wrong; pruning
    /// only returns its slot to a book that has filled.
    ///
    /// - Parameter isLive: Answers whether an identity still names an object.
    template<typename Predicate>
    void prune(Predicate isLive)
    {
        std::erase_if(m_entries, [&](const auto& entry) { return !isLive(entry.first); });
    }

    /// Returns how many labels the book holds.
    ///
    /// - Returns: The count, at most ``capacity``.
    [[nodiscard]] std::size_t count() const { return m_entries.size(); }

    /// Borrows every label in stored order, including labels awaiting pruning.
    /// - Returns: Exact entries, valid until the next mutation.
    [[nodiscard]] std::span<const std::pair<ObjectID, std::string>> records() const { return m_entries; }

private:
    std::vector<std::pair<ObjectID, std::string>> m_entries;
};

/// The most tags one object carries.
inline constexpr std::size_t maxTagsPerObject = 16;

/// Why an object's tags were or were not stored.
enum class TagVerdict : std::uint8_t {
    /// Stored, or cleared by no tags.
    Accepted,
    /// More than ``maxTagsPerObject``.
    TooMany,
    /// A tag with no name.
    Empty,
    /// A name longer than ``maxLabelLength``.
    TooLong,
    /// A name carrying a control character.
    ControlCharacter,
    /// The book holds tags for as many objects as it may.
    Full,
};

/// Returns the sentence a refused set of tags is reported with.
///
/// - Parameter verdict: A verdict from ``TagBook``.
/// - Returns: A sentence, empty for ``TagVerdict/Accepted``.
[[nodiscard]] std::string_view tagVerdictSentence(TagVerdict verdict);

/// The tags a person put on live objects, keyed by identity.
///
/// A tag is a marker, not a handle: it is drawn, and a listing can be searched
/// by it, but no target resolves through it, so a name may read as anything a
/// label may not. Which colour a name is drawn in is the reader's
/// (`Session::resolveManualTag`); the host keeps names only.
///
/// Bounded like ``LabelBook``: a book that is full refuses a new object
/// instead of growing.
class TagBook {
public:
    /// The most objects a book holds tags for at once.
    static constexpr std::size_t capacity = 4096;

    /// Replaces an object's tags.
    ///
    /// A name given more than once, compared without regard to ASCII case, is
    /// kept once under its first spelling. A refused set changes nothing.
    ///
    /// - Parameters:
    ///   - id: The object to tag.
    ///   - names: Its tags, in the order they are drawn; none clears them.
    /// - Returns: Whether they were stored, and why not when they were not.
    TagVerdict set(ObjectID id, std::span<const std::string> names);

    /// Returns an object's tags.
    ///
    /// - Parameter id: The object to look up.
    /// - Returns: Its tags in order, none when it has none. Borrowed until
    ///   the next mutation.
    [[nodiscard]] std::span<const std::string> tags(ObjectID id) const;

    /// Drops an object's tags when the object ends.
    ///
    /// - Parameter id: The object that ended.
    void forget(ObjectID id);

    /// Drops the tags of every object a predicate reports as ended.
    ///
    /// - Parameter isLive: Answers whether an identity still names an object.
    template<typename Predicate>
    void prune(Predicate isLive)
    {
        std::erase_if(m_entries, [&](const auto& entry) { return !isLive(entry.first); });
    }

    /// Returns how many objects the book holds tags for.
    ///
    /// - Returns: The count, at most ``capacity``.
    [[nodiscard]] std::size_t count() const { return m_entries.size(); }

    /// Borrows every object's tags without sorting or changing their spelling.
    /// - Returns: Exact entries, valid until the next mutation.
    [[nodiscard]] std::span<const std::pair<ObjectID, std::vector<std::string>>> records() const { return m_entries; }

private:
    std::vector<std::pair<ObjectID, std::vector<std::string>>> m_entries;
};

/// One object a target may resolve to.
struct Candidate {
    ObjectID id;
    /// Borrowed for the duration of the resolution; empty when unlabelled.
    std::string_view label;
};

/// How a target resolved.
enum class ResolutionOutcome : std::uint8_t {
    /// Exactly one object matched; `id` names it.
    Resolved,
    /// Nothing live matched.
    NotFound,
    /// A label matched more than one object; `candidates` lists them.
    Ambiguous,
    /// The target was `here`, and the caller is not inside an object of the
    /// kind wanted.
    NoContext,
};

/// The answer to one target.
struct Resolution {
    ResolutionOutcome outcome = ResolutionOutcome::NotFound;
    /// The object resolved to; zero unless ``ResolutionOutcome/Resolved``.
    ObjectID id;
    /// Every object an ambiguous label matched, in candidate order.
    std::vector<ObjectID> candidates;
    /// A sentence for the person who typed the target; empty when resolved.
    std::string message;
};

/// The word a target uses for the object the caller runs in.
inline constexpr std::string_view hereTarget = "here";

/// Resolves a target to one live object.
///
/// Tried in a fixed order, the first form that applies deciding:
/// 1. `here`: the identity in `here` of the wanted kind, or with no kind
///    wanted the innermost the caller has (pane, then tab, window, session,
///    workspace).
/// 2. An identity written `kind:number`.
/// 3. A bare number, when a kind is wanted: that kind's identity.
/// 4. A label, compared ignoring ASCII case, among the candidates of the
///    wanted kind. More than one match changes nothing and is reported with
///    every match named.
///
/// An identity, however it is reached, resolves only when it is among
/// `candidates`: a target never names an object that has ended.
///
/// - Parameters:
///   - target: What the person typed.
///   - candidates: Every live object the caller may address.
///   - kind: The kind wanted, or nothing to accept any.
///   - here: The identities the caller runs inside, from its environment.
/// - Returns: The resolution; its message is written for the person.
[[nodiscard]] Resolution resolveTarget(std::string_view target, std::span<const Candidate> candidates, std::optional<ObjectKind> kind, std::span<const ObjectID> here);

} // namespace Ubiquitty::Session::Identity
