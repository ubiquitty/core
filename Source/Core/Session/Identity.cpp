// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/Identity.cpp — see Identity.h.

#include "Core/Session/Identity.h"

#include <algorithm>
#include <charconv>

namespace Ubiquitty::Session::Identity {

namespace {

constexpr std::array<std::string_view, objectKindCount> kindNames { "workspace", "session", "window", "tab", "pane", "client" };

/// The kinds `here` falls back through when a target names none, innermost
/// first. A client is not something a process runs inside.
constexpr std::array<ObjectKind, 5> containmentInnermostFirst { ObjectKind::Pane, ObjectKind::Tab, ObjectKind::Window, ObjectKind::Session, ObjectKind::Workspace };

/// Reads a decimal number with no sign and no leading zero.
std::optional<std::uint64_t> parseNumber(std::string_view digits)
{
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
        return std::nullopt;
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc() || end != digits.data() + digits.size())
        return std::nullopt;
    return number;
}

char foldASCII(char character)
{
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
}

bool equalIgnoringASCIICase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return foldASCII(x) == foldASCII(y); });
}

/// Joins identities as a reader lists them: `a`, `a and b`, `a, b, and c`.
std::string listIdentities(std::span<const ObjectID> identities)
{
    std::string out;
    for (std::size_t index = 0; index < identities.size(); ++index) {
        if (index > 0) {
            if (identities.size() > 2)
                out += ',';
            out += index + 1 == identities.size() ? " and " : " ";
        }
        out += format(identities[index]);
    }
    return out;
}

bool isLive(std::span<const Candidate> candidates, ObjectID id)
{
    return std::ranges::any_of(candidates, [&](const Candidate& candidate) { return candidate.id == id; });
}

Resolution resolved(ObjectID id)
{
    Resolution resolution;
    resolution.outcome = ResolutionOutcome::Resolved;
    resolution.id = id;
    return resolution;
}

Resolution refused(ResolutionOutcome outcome, std::string message)
{
    Resolution resolution;
    resolution.outcome = outcome;
    resolution.message = std::move(message);
    return resolution;
}

/// Resolves an identity the target spelled, or `here` reached.
Resolution resolveIdentity(ObjectID id, std::span<const Candidate> candidates, std::optional<ObjectKind> kind)
{
    if (kind && id.kind != *kind)
        return refused(ResolutionOutcome::NotFound, format(id) + " is a " + std::string(kindName(id.kind)) + ", and a " + std::string(kindName(*kind)) + " is wanted.");
    if (!isLive(candidates, id))
        return refused(ResolutionOutcome::NotFound, "There is no " + format(id) + ".");
    return resolved(id);
}

} // namespace

std::string_view kindName(ObjectKind kind)
{
    const auto index = static_cast<std::size_t>(std::to_underlying(kind));
    return index < kindNames.size() ? kindNames[index] : std::string_view("object");
}

std::optional<ObjectKind> kindNamed(std::string_view word)
{
    for (const ObjectKind kind : allObjectKinds) {
        if (kindName(kind) == word)
            return kind;
    }
    return std::nullopt;
}

std::string format(ObjectID id)
{
    std::string out(kindName(id.kind));
    out += ':';
    out += std::to_string(id.number);
    return out;
}

std::optional<ObjectID> parseObjectID(std::string_view text)
{
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos)
        return std::nullopt;
    const std::optional<ObjectKind> kind = kindNamed(text.substr(0, separator));
    const std::optional<std::uint64_t> number = parseNumber(text.substr(separator + 1));
    if (!kind || !number || *number == 0)
        return std::nullopt;
    return ObjectID { *kind, *number };
}

Allocator::Allocator(std::uint64_t ceiling)
    : m_ceiling(ceiling)
{
    m_next.fill(1);
}

bool Allocator::restoreContinuation(const Continuation& state)
{
    if (state.ceiling != m_ceiling || std::ranges::any_of(m_next, [](auto next) { return next != 1; }))
        return false;
    m_next = state.next;
    return true;
}

std::optional<ObjectID> Allocator::mint(ObjectKind kind)
{
    std::uint64_t& next = m_next[std::to_underlying(kind)];
    if (next == 0 || next > m_ceiling)
        return std::nullopt;
    const ObjectID id { kind, next };
    // At the ceiling the counter moves past it and stays there; at 2^64 - 1 it
    // reaches zero, which mint reads as exhausted too.
    ++next;
    return id;
}

void Allocator::reserveThrough(ObjectID id)
{
    std::uint64_t& next = m_next[std::to_underlying(id.kind)];
    if (id.number == 0 || next == 0 || id.number < next)
        return;
    next = id.number + 1;
}

LabelVerdict checkLabel(std::string_view label)
{
    if (label.size() > maxLabelLength)
        return LabelVerdict::TooLong;
    if (std::ranges::any_of(label, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20 || byte == 0x7F;
        }))
        return LabelVerdict::ControlCharacter;
    if (equalIgnoringASCIICase(label, hereTarget))
        return LabelVerdict::Reserved;
    if (parseObjectID(label) || (!label.empty() && std::ranges::all_of(label, [](char character) { return character >= '0' && character <= '9'; })))
        return LabelVerdict::LooksLikeID;
    return LabelVerdict::Accepted;
}

std::string_view labelVerdictSentence(LabelVerdict verdict)
{
    switch (verdict) {
    case LabelVerdict::Accepted:
        return {};
    case LabelVerdict::TooLong:
        return "A label is at most 64 bytes.";
    case LabelVerdict::ControlCharacter:
        return "A label cannot contain a control character.";
    case LabelVerdict::LooksLikeID:
        return "A label cannot read as an ID or a number, because a target would resolve it as one.";
    case LabelVerdict::Reserved:
        return "'here' always means the caller's own pane, so it cannot be a label.";
    case LabelVerdict::Full:
        return "Too many objects are labelled; clear a label first.";
    }
    return "The label was refused.";
}

LabelVerdict LabelBook::set(ObjectID id, std::string_view label)
{
    if (const LabelVerdict verdict = checkLabel(label); verdict != LabelVerdict::Accepted)
        return verdict;
    const auto existing = std::ranges::find_if(m_entries, [&](const auto& entry) { return entry.first == id; });
    if (label.empty()) {
        if (existing != m_entries.end())
            m_entries.erase(existing);
        return LabelVerdict::Accepted;
    }
    if (existing != m_entries.end()) {
        existing->second.assign(label);
        return LabelVerdict::Accepted;
    }
    if (m_entries.size() >= capacity)
        return LabelVerdict::Full;
    m_entries.emplace_back(id, std::string(label));
    return LabelVerdict::Accepted;
}

std::string_view LabelBook::label(ObjectID id) const
{
    const auto existing = std::ranges::find_if(m_entries, [&](const auto& entry) { return entry.first == id; });
    return existing == m_entries.end() ? std::string_view() : std::string_view(existing->second);
}

void LabelBook::forget(ObjectID id)
{
    std::erase_if(m_entries, [&](const auto& entry) { return entry.first == id; });
}

std::string_view tagVerdictSentence(TagVerdict verdict)
{
    switch (verdict) {
    case TagVerdict::Accepted:
        return {};
    case TagVerdict::TooMany:
        return "An object carries at most 16 tags.";
    case TagVerdict::Empty:
        return "A tag has a name.";
    case TagVerdict::TooLong:
        return "A tag is at most 64 bytes.";
    case TagVerdict::ControlCharacter:
        return "A tag cannot contain a control character.";
    case TagVerdict::Full:
        return "Too many objects are tagged; clear an object's tags first.";
    }
    return "The tags were refused.";
}

TagVerdict TagBook::set(ObjectID id, std::span<const std::string> names)
{
    std::vector<std::string> distinct;
    for (const std::string& name : names) {
        if (name.empty())
            return TagVerdict::Empty;
        if (name.size() > maxLabelLength)
            return TagVerdict::TooLong;
        if (std::ranges::any_of(name, [](char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte < 0x20 || byte == 0x7F;
            }))
            return TagVerdict::ControlCharacter;
        if (std::ranges::none_of(distinct, [&](const std::string& seen) { return equalIgnoringASCIICase(seen, name); }))
            distinct.push_back(name);
    }
    if (distinct.size() > maxTagsPerObject)
        return TagVerdict::TooMany;
    const auto existing = std::ranges::find_if(m_entries, [&](const auto& entry) { return entry.first == id; });
    if (distinct.empty()) {
        if (existing != m_entries.end())
            m_entries.erase(existing);
        return TagVerdict::Accepted;
    }
    if (existing != m_entries.end()) {
        existing->second = std::move(distinct);
        return TagVerdict::Accepted;
    }
    if (m_entries.size() >= capacity)
        return TagVerdict::Full;
    m_entries.emplace_back(id, std::move(distinct));
    return TagVerdict::Accepted;
}

std::span<const std::string> TagBook::tags(ObjectID id) const
{
    const auto existing = std::ranges::find_if(m_entries, [&](const auto& entry) { return entry.first == id; });
    return existing == m_entries.end() ? std::span<const std::string>() : std::span<const std::string>(existing->second);
}

void TagBook::forget(ObjectID id)
{
    std::erase_if(m_entries, [&](const auto& entry) { return entry.first == id; });
}

Resolution resolveTarget(std::string_view target, std::span<const Candidate> candidates, std::optional<ObjectKind> kind, std::span<const ObjectID> here)
{
    if (target.empty())
        return refused(ResolutionOutcome::NotFound, "The target is empty; name an ID, a label, or 'here'.");

    if (equalIgnoringASCIICase(target, hereTarget)) {
        const auto inside = [&](ObjectKind wanted) -> std::optional<ObjectID> {
            const auto found = std::ranges::find_if(here, [&](ObjectID id) { return id.kind == wanted && id.isValid(); });
            return found == here.end() ? std::nullopt : std::optional<ObjectID>(*found);
        };
        std::optional<ObjectID> id;
        if (kind) {
            id = inside(*kind);
        } else {
            for (const ObjectKind candidateKind : containmentInnermostFirst) {
                if ((id = inside(candidateKind)))
                    break;
            }
        }
        if (!id) {
            const std::string wanted = kind ? "a " + std::string(kindName(*kind)) : std::string("a pane");
            return refused(ResolutionOutcome::NoContext, "'here' needs " + wanted + " to run in, and this process was not started inside one.");
        }
        return resolveIdentity(*id, candidates, kind);
    }

    if (const std::optional<ObjectID> id = parseObjectID(target))
        return resolveIdentity(*id, candidates, kind);

    if (kind) {
        if (const std::optional<std::uint64_t> number = parseNumber(target); number && *number != 0)
            return resolveIdentity(ObjectID { *kind, *number }, candidates, kind);
    }

    std::vector<ObjectID> matches;
    for (const Candidate& candidate : candidates) {
        if (kind && candidate.id.kind != *kind)
            continue;
        if (!candidate.label.empty() && equalIgnoringASCIICase(candidate.label, target))
            matches.push_back(candidate.id);
    }
    if (matches.size() == 1)
        return resolved(matches.front());
    if (matches.empty()) {
        const std::string wanted = kind ? std::string(kindName(*kind)) : std::string("object");
        return refused(ResolutionOutcome::NotFound, "No " + wanted + " is labelled '" + std::string(target) + "'.");
    }
    Resolution resolution = refused(ResolutionOutcome::Ambiguous,
        "'" + std::string(target) + "' labels " + std::to_string(matches.size()) + " objects: " + listIdentities(matches) + ". Name one by its ID.");
    resolution.candidates = std::move(matches);
    return resolution;
}

} // namespace Ubiquitty::Session::Identity
