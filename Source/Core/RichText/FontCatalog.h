// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/RichText/Document.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace Ubiquitty::RichText {

struct TypographyFacts;

/// What the selected renderer can enumerate, rather than what its host has.
enum class FontCatalogScope : std::uint8_t {
    Unavailable,
    Exposed,
    System,
};

/// A provider may refuse access without pretending the system has no fonts.
enum class FontCatalogStatus : std::uint8_t {
    Ready,
    Unavailable,
    Denied,
    Stale,
    Invalid,
};

/// A page's count uses one wire octet; every OSC packet is below 32 KiB.
inline constexpr std::uint16_t maxFontCatalogPageFamilies = std::numeric_limits<std::uint8_t>::max();
inline constexpr std::size_t fontCatalogPacketBytes = 32 * 1024;
inline constexpr std::size_t maxFontCatalogHeaderBytes = std::string_view("\x1b]7404;a=f:i=65535:v=3:E=4294967295:F=2:b=4294967295:n=255:t=4294967295:s=unavailable;\x1b\\").size();
inline constexpr std::size_t maxFontCatalogPageBytes = (fontCatalogPacketBytes - 1 - maxFontCatalogHeaderBytes - (maxFontCatalogPageFamilies - 1)) / 2;

/// Whether a native name can be selected exactly in the rich-text profile.
[[nodiscard]] inline bool isFontCatalogFamily(std::string_view family)
{
    if (family.empty() || family.size() > maxFamilyNameBytes || family.front() == ' ' || family.back() == ' '
        || Unicode::wellFormedUTF8PrefixLength(family) != family.size())
        return false;
    for (std::size_t index = 0; index < family.size(); ++index) {
        const auto byte = static_cast<std::uint8_t>(family[index]);
        if (byte < 0x20 || byte == 0x7f
            || (byte == 0xc2 && index + 1 < family.size() && static_cast<std::uint8_t>(family[index + 1]) <= 0x9f))
            return false;
    }
    return true;
}

/// A native catalog is stable, sorted and unique for one renderer revision.
struct UBIQUITTY_ESCAPABLE FontCatalogQuery {
    std::uint32_t offset = 0;
    std::uint16_t count = maxFontCatalogPageFamilies;
    std::uint64_t rendererRevision = 0;
};

/// One complete family name, usable verbatim in the UTF-8 rich-text profile.
struct FontCatalogFamily {
    std::array<char, maxFamilyNameBytes> name {};
    std::uint8_t length = 0;

    [[nodiscard]] std::string_view familyName() const { return { name.data(), length }; }
    friend bool operator==(const FontCatalogFamily&, const FontCatalogFamily&) = default;
};

/// A provider page contains names, never application reply bytes.
struct FontCatalogPage {
    FontCatalogStatus status = FontCatalogStatus::Unavailable;
    FontCatalogScope scope = FontCatalogScope::Unavailable;
    std::uint32_t offset = 0;
    std::uint32_t total = 0;
    std::uint64_t rendererRevision = 0;
    FallibleVector<FontCatalogFamily> families;

    /// The number of complete family identifiers in this page.
    [[nodiscard]] std::size_t familyCount() const { return families.size(); }

    /// Copies one name across a native language boundary.
    /// - Parameter index: An index less than familyCount().
    [[nodiscard]] std::string familyNameAt(std::size_t index) const { return std::string(families[index].familyName()); }

    /// Appends one name without exceeding the page's bounded storage.
    /// - Parameter family: A nonempty UTF-8 family with no control characters.
    /// - Returns: False when invalid, full, or allocation was refused.
    [[nodiscard]] bool appendFamily(const std::string& family)
    {
        return appendFamilyView(family);
    }

    /// Appends a borrowed name without allocating a temporary string.
    [[nodiscard]] bool appendFamilyView(std::string_view family)
    {
        if (!isFontCatalogFamily(family) || families.size() >= maxFontCatalogPageFamilies)
            return false;
        std::size_t bytes = family.size();
        for (const auto& entry : families)
            bytes += entry.length;
        if (bytes > maxFontCatalogPageBytes)
            return false;
        FontCatalogFamily entry;
        entry.length = static_cast<std::uint8_t>(family.size());
        std::ranges::copy(family, entry.name.begin());
        return static_cast<bool>(families.tryPushBack(std::move(entry)));
    }
};

/// Validates provider output before it crosses either the Sync or OSC boundary.
[[nodiscard]] inline bool isFontCatalogPage(const FontCatalogQuery& query, const FontCatalogPage& page)
{
    if (!query.count || query.count > maxFontCatalogPageFamilies || !query.rendererRevision
        || page.rendererRevision != query.rendererRevision || page.offset != query.offset
        || page.status > FontCatalogStatus::Invalid || page.scope > FontCatalogScope::System)
        return false;
    if (page.status != FontCatalogStatus::Ready)
        return page.families.empty() && !page.total;
    if (page.scope == FontCatalogScope::Unavailable || page.offset > page.total
        || page.families.size() > query.count || page.families.size() > page.total - page.offset
        || (page.offset < page.total && page.families.empty()))
        return false;
    std::size_t bytes = 0;
    std::string_view previous;
    for (const auto& entry : page.families) {
        const auto family = entry.familyName();
        if (!isFontCatalogFamily(family) || (!previous.empty() && family <= previous))
            return false;
        bytes += family.size();
        if (bytes > maxFontCatalogPageBytes)
            return false;
        previous = family;
    }
    return true;
}

/// A synchronous borrowed service; it must not prompt or enter the UI thread.
/// Enumerate from a stable renderer snapshot prepared outside the model lock.
struct FontCatalogProvider {
    using Function = bool (*)(void*, const FontCatalogQuery*, const TypographyFacts*, FontCatalogPage*);
    Function function = nullptr;
    void* context = nullptr;
};

} // namespace Ubiquitty::RichText
