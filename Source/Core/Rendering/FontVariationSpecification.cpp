// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/FontVariationSpecification.h"

#include <cctype>
#include <cmath>
#include <cstdint>

namespace Ubiquitty::Rendering {

namespace {

// The fixed-point scale the grammar's values live on. Four fractional digits
// cover every published axis scale (weights are integers, slants tenths),
// and a fixed scale is what keeps parse and serialize locale-independent:
// printf/strtod spell the decimal point per locale, and a config file must
// not change meaning with the environment.
constexpr std::int64_t valueScale = 10000;

// Parses a signed decimal with at most four fractional digits into the
// fixed-point scale. Returns false for anything else — exponents, second
// points, trailing letters — so a malformed value skips its token rather
// than landing an axis on a surprise.
bool parseScaledDecimal(const std::string& text, std::int64_t& scaled)
{
    if (text.empty())
        return false;
    std::size_t i = 0;
    bool isNegative = false;
    if (text[i] == '+' || text[i] == '-') {
        isNegative = text[i] == '-';
        ++i;
    }
    if (i == text.size())
        return false;
    std::int64_t integral = 0;
    bool hasIntegralDigit = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        integral = integral * 10 + (text[i] - '0');
        if (integral > 100000)
            return false;
        hasIntegralDigit = true;
        ++i;
    }
    std::int64_t fraction = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::int64_t digits = 0;
        std::int64_t place = valueScale;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            place /= 10;
            if (place == 0)
                return false;
            fraction += (text[i] - '0') * place;
            ++digits;
            ++i;
        }
        if (digits == 0)
            return false;
    } else if (!hasIntegralDigit) {
        return false;
    }
    if (i != text.size())
        return false;
    scaled = integral * valueScale + fraction;
    if (isNegative)
        scaled = -scaled;
    return true;
}

// A usable axis tag: exactly four ASCII graphic characters, case preserved —
// registered axes are lower-case by specification and custom axes upper-case,
// so folding case would alias a font's `GRAD` with a nonexistent `grad`.
bool isUsableAxisTag(const std::string& tag)
{
    if (tag.size() != 4)
        return false;
    for (char ch : tag)
        if (!std::isgraph(static_cast<unsigned char>(ch)) || ch == '=')
            return false;
    return true;
}

} // namespace

FontVariationSettings parseFontVariationSpecification(const std::string& specification)
{
    FontVariationSettings out;

    auto upsert = [&](const std::string& tag, double value) {
        for (auto& axis : out)
            if (axis.tag == tag) {
                axis.value = value; // last token wins
                return;
            }
        out.push_back({ tag, value });
    };

    auto handleToken = [&](const std::string& token) {
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos)
            return;
        const std::string tag = token.substr(0, eq);
        if (!isUsableAxisTag(tag))
            return;
        std::int64_t scaled = 0;
        if (!parseScaledDecimal(token.substr(eq + 1), scaled))
            return;
        upsert(tag, static_cast<double>(scaled) / valueScale);
    };

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

std::string serializeFontVariationSettings(const FontVariationSettings& settings)
{
    std::string out;
    for (const auto& axis : settings) {
        if (!out.empty())
            out += ' ';
        out += axis.tag;
        out += '=';
        std::int64_t scaled = static_cast<std::int64_t>(std::llround(axis.value * valueScale));
        if (scaled < 0) {
            out += '-';
            scaled = -scaled;
        }
        out += std::to_string(scaled / valueScale);
        std::int64_t fraction = scaled % valueScale;
        if (fraction != 0) {
            std::string digits = std::to_string(valueScale + fraction).substr(1);
            while (digits.size() > 1 && digits.back() == '0')
                digits.pop_back();
            out += '.';
            out += digits;
        }
    }
    return out;
}

FontVariationSettings mergeFontVariationSettings(const FontVariationSettings& derived, const FontVariationSettings& user)
{
    FontVariationSettings merged = derived;
    for (const auto& axis : user) {
        bool didReplace = false;
        for (auto& kept : merged)
            if (kept.tag == axis.tag) {
                kept.value = axis.value;
                didReplace = true;
                break;
            }
        if (!didReplace)
            merged.push_back(axis);
    }
    return merged;
}

} // namespace Ubiquitty::Rendering
