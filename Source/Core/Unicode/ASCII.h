// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ASCII-only by design: locale-aware tolower() would misfold these tokens
// under Turkish-style locales.

#include <string>
#include <string_view>

namespace Ubiquitty::Unicode {

inline char toASCIILower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// True when a equals the NUL-terminated b, ignoring ASCII case.
inline bool equalIgnoringASCIICase(std::string_view a, const char* b)
{
    std::string_view::size_type i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (toASCIILower(a[i]) != toASCIILower(b[i]))
            return false;
    }
    return i == a.size() && b[i] == '\0';
}

inline std::string lowercasedASCII(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += toASCIILower(c);
    return out;
}

} // namespace Ubiquitty::Unicode
