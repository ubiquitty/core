// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/FontFeatureSpecification.h"

#include <array>
#include <cctype>
#include <cstdio>

namespace Ubiquitty::Rendering {

namespace {

// Lower-case an ASCII string in place; non-ASCII bytes pass through unchanged.
void asciiLowerInPlace(std::string& s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

// Canonicalize a bare tag (no sign/value prefix already stripped): trim, lower-
// case, zero-pad the `ssN` stylistic-set shorthand to `ssNN`. Returns "" when
// the result is not a usable 4-char ASCII-graphic OpenType tag.
std::string canonicalTag(std::string tag)
{
    asciiLowerInPlace(tag);
    // `ssN`/`ssNN` stylistic-set shorthand -> zero-padded `ssNN`.
    if (tag.size() == 3 && tag[0] == 's' && tag[1] == 's' && std::isdigit(static_cast<unsigned char>(tag[2])))
        tag = std::string("ss0") + tag[2];
    if (tag.size() != 4)
        return {};
    for (char ch : tag)
        if (!std::isgraph(static_cast<unsigned char>(ch)))
            return {};
    return tag;
}

} // namespace

FontFeatureSettings parseFontFeatureSpecification(const std::string& specification)
{
    FontFeatureSettings out;

    auto upsert = [&](const std::string& tag, int value) {
        for (auto& feature : out)
            if (feature.tag == tag) {
                feature.value = value; // last token wins
                return;
            }
        out.push_back({ tag, value });
    };

    auto handleToken = [&](std::string token) {
        // Trim ASCII whitespace.
        size_t begin = token.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return;
        size_t end = token.find_last_not_of(" \t\r\n");
        token = token.substr(begin, end - begin + 1);

        int value = 1;
        // Leading sign: '+' enables, '-' disables.
        if (token.front() == '+') {
            token.erase(token.begin());
        } else if (token.front() == '-') {
            value = 0;
            token.erase(token.begin());
        }

        // `tag=N` explicit value (only meaningful for an enabled tag).
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            const std::string num = token.substr(eq + 1);
            token = token.substr(0, eq);
            // Parse a non-negative integer; reject if empty or non-numeric.
            if (num.empty())
                return;
            int parsed = 0;
            for (char ch : num) {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                    return;
                parsed = parsed * 10 + (ch - '0');
            }
            // An explicit `=N` overrides the value unless the token was disabled
            // (`-tag=N` stays off — disabling wins over an explicit value).
            if (value != 0)
                value = parsed;
        }

        std::string tag = canonicalTag(token);
        if (tag.empty())
            return;
        upsert(tag, value);
    };

    // Split on commas and ASCII whitespace; both are token separators.
    std::string token;
    for (char ch : specification) {
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!token.empty()) {
                handleToken(token);
                token.clear();
            }
        } else {
            token.push_back(ch);
        }
    }
    if (!token.empty())
        handleToken(token);

    return out;
}

std::string serializeFontFeatureSettings(const FontFeatureSettings& settings)
{
    std::string out;
    for (const auto& s : settings) {
        if (!out.empty())
            out += ' ';
        if (s.value == 0)
            out += '-';
        out += s.tag;
        if (s.value > 1)
            out += '=' + std::to_string(s.value);
    }
    return out;
}

std::string canonicalFontFeatureSpecification(const std::string& specification) { return serializeFontFeatureSettings(parseFontFeatureSpecification(specification)); }

std::vector<std::string> ligatureSuppressionFeatureTags() { return { "liga", "clig", "calt" }; }

FontFeatureSettings resolveFontFeatures(Ligatures ligatures, const std::string& specification)
{
    FontFeatureSettings resolved;
    if (ligatures == Ligatures::Disabled) {
        for (const std::string& tag : ligatureSuppressionFeatureTags())
            resolved.push_back({ tag, 0 });
    }
    // Appended rather than merged: a duplicate tag is what expresses the
    // override, and collapsing it here would hand the frontend a list where the
    // reader's explicit value had already lost to the toggle.
    const FontFeatureSettings requested = parseFontFeatureSpecification(specification);
    resolved.insert(resolved.end(), requested.begin(), requested.end());
    return resolved;
}

std::vector<std::string> ligatureCandidateFeatureTags()
{
    std::vector<std::string> tags { "calt", "liga", "dlig" };
    for (int i = 1; i <= 20; ++i) {
        std::array<char, 5> tag {};
        std::snprintf(tag.data(), tag.size(), "ss%02d", i);
        tags.emplace_back(tag.data());
    }
    return tags;
}

} // namespace Ubiquitty::Rendering
