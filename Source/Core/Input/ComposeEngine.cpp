// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Input/ComposeEngine.h"

namespace Ubiquitty::Input {

// Built-in compose table. Each entry is a key-run (1-4 codepoints, NUL-padded)
// and the codepoint it resolves to. Mirrors the common subset of X11's
// en_US.UTF-8 Compose file: accented Latin letters (Compose <accent> <letter>),
// punctuation, currency, fractions, and a few symbols. Order is irrelevant;
// lookup is a linear prefix scan (the table is small and a compose is rare).
namespace {
struct ComposeEntry {
    uint32_t keys[ComposeEngine::maxSequenceLength];
    uint32_t result;
};

// Helper macros keep the table terse and the NUL-padding consistent.
#define C2(a, b, r) { { (a), (b), 0, 0 }, (r) }
#define C3(a, b, c, r) { { (a), (b), (c), 0 }, (r) }

constexpr ComposeEntry composeTable[] = {
    C2('a', 't', '@'),
    C2('(', '(', '['),
    C2(')', ')', ']'),
    C2('(', '-', '{'),
    C2('-', ')', '}'),
    C2('/', '/', '\\'),
    C2('!', '^', '|'),
    C2('~', ' ', '~'),
    // --- Acute accent: Compose ' <vowel> ---
    C2('\'', 'a', 0x00E1),
    C2('\'', 'e', 0x00E9),
    C2('\'', 'i', 0x00ED),
    C2('\'', 'o', 0x00F3),
    C2('\'', 'u', 0x00FA),
    C2('\'', 'y', 0x00FD),
    C2('\'', 'A', 0x00C1),
    C2('\'', 'E', 0x00C9),
    C2('\'', 'I', 0x00CD),
    C2('\'', 'O', 0x00D3),
    C2('\'', 'U', 0x00DA),
    C2('\'', 'Y', 0x00DD),
    // --- Grave accent: Compose ` <vowel> ---
    C2('`', 'a', 0x00E0),
    C2('`', 'e', 0x00E8),
    C2('`', 'i', 0x00EC),
    C2('`', 'o', 0x00F2),
    C2('`', 'u', 0x00F9),
    C2('`', 'A', 0x00C0),
    C2('`', 'E', 0x00C8),
    C2('`', 'I', 0x00CC),
    C2('`', 'O', 0x00D2),
    C2('`', 'U', 0x00D9),
    // --- Circumflex: Compose ^ <vowel> ---
    C2('^', 'a', 0x00E2),
    C2('^', 'e', 0x00EA),
    C2('^', 'i', 0x00EE),
    C2('^', 'o', 0x00F4),
    C2('^', 'u', 0x00FB),
    C2('^', 'A', 0x00C2),
    C2('^', 'E', 0x00CA),
    C2('^', 'I', 0x00CE),
    C2('^', 'O', 0x00D4),
    C2('^', 'U', 0x00DB),
    // --- Diaeresis / umlaut: Compose " <vowel> ---
    C2('"', 'a', 0x00E4),
    C2('"', 'e', 0x00EB),
    C2('"', 'i', 0x00EF),
    C2('"', 'o', 0x00F6),
    C2('"', 'u', 0x00FC),
    C2('"', 'y', 0x00FF),
    C2('"', 'A', 0x00C4),
    C2('"', 'E', 0x00CB),
    C2('"', 'I', 0x00CF),
    C2('"', 'O', 0x00D6),
    C2('"', 'U', 0x00DC),
    // --- Tilde: Compose ~ <letter> ---
    C2('~', 'a', 0x00E3),
    C2('~', 'o', 0x00F5),
    C2('~', 'n', 0x00F1),
    C2('~', 'A', 0x00C3),
    C2('~', 'O', 0x00D5),
    C2('~', 'N', 0x00D1),
    // --- Cedilla: Compose , c ---
    C2(',', 'c', 0x00E7),
    C2(',', 'C', 0x00C7),
    // --- Ring: Compose o a / o A ---
    C2('o', 'a', 0x00E5),
    C2('o', 'A', 0x00C5),
    // --- Ligatures ---
    C2('a', 'e', 0x00E6),
    C2('A', 'E', 0x00C6),
    C2('o', 'e', 0x0153),
    C2('O', 'E', 0x0152),
    C2('s', 's', 0x00DF), // sharp s
    // --- Slashed o ---
    C2('/', 'o', 0x00F8),
    C2('/', 'O', 0x00D8),
    // --- Punctuation / dashes / quotes ---
    C3('-', '-', '-', 0x2014), // em dash
    C3('-', '-', '.', 0x2013), // en dash (X11 alias)
    C2('<', '<', 0x00AB), // «
    C2('>', '>', 0x00BB), // »
    C3('.', '.', '.', 0x2026), // ellipsis
    C2('!', '!', 0x00A1), // ¡
    C2('?', '?', 0x00BF), // ¿
    // --- Currency / symbols ---
    C2('c', '=', 0x20AC),
    C2('=', 'e', 0x20AC),
    C2('=', 'E', 0x20AC), // €
    C2('c', '/', 0x00A2),
    C2('c', '|', 0x00A2), // ¢
    C2('l', '-', 0x00A3),
    C2('L', '-', 0x00A3), // £
    C2('y', '=', 0x00A5),
    C2('Y', '=', 0x00A5), // ¥
    C2('c', 'o', 0x00A9),
    C2('C', 'O', 0x00A9),
    C3('(', 'c', ')', 0x00A9), // ©
    C2('r', 'o', 0x00AE),
    C3('(', 'r', ')', 0x00AE), // ®
    C3('(', 'p', ')', 0x2117), // ℗ (X11 alias for sound recording copyright)
    C2('+', '-', 0x00B1), // ±
    C2('x', 'x', 0x00D7), // × (multiply)
    C2(':', '-', 0x00F7), // ÷
    C2('o', 'o', 0x00B0), // ° degree
    C2('-', '>', 0x2192),
    C2('<', '-', 0x2190), // → ←
    C2('<', '3', 0x2665), // ♥
    // --- Fractions ---
    C3('1', '/', '2', 0x00BD), // 1/2
    C3('1', '/', '4', 0x00BC), // 1/4
    C3('3', '/', '4', 0x00BE), // 3/4
    // --- Trademark / section / ordinals ---
    C2('t', 'm', 0x2122), // ™
    C2('p', '!', 0x00B6), // ¶ pilcrow
    C2('s', 'o', 0x00BA), // º masculine ordinal
    C2('a', '_', 0x00AA), // ª feminine ordinal
};

#undef C1
#undef C2
#undef C3

// A run of `len` codepoints matches `entry` exactly when the first `len` keys
// equal the buffer and the entry's next slot is the NUL terminator (so a
// 2-key run doesn't spuriously match a 3-key entry's prefix).
bool runMatchesEntry(const uint32_t* run, int len, const ComposeEntry& e)
{
    for (int i = 0; i < len; ++i) {
        if (e.keys[i] != run[i])
            return false;
    }
    return len < ComposeEngine::maxSequenceLength ? e.keys[len] == 0 : true;
}

// True when `run` (length `len`) is a strict prefix of `entry` — the entry has
// the same first `len` keys and at least one more.
bool runIsPrefixOfEntry(const uint32_t* run, int len, const ComposeEntry& e)
{
    if (len >= ComposeEngine::maxSequenceLength)
        return false;
    if (e.keys[len] == 0)
        return false; // entry ends at `len` — exact match, not a strict prefix
    for (int i = 0; i < len; ++i) {
        if (e.keys[i] != run[i])
            return false;
    }
    return true;
}

template<int N> bool scanExact(const ComposeEntry (&table)[N], const uint32_t* run, int len, uint32_t& outValue)
{
    for (int i = 0; i < N; ++i) {
        if (runMatchesEntry(run, len, table[i])) {
            outValue = table[i].result;
            return true;
        }
    }
    return false;
}

template<int N> bool scanPrefix(const ComposeEntry (&table)[N], const uint32_t* run, int len)
{
    for (int i = 0; i < N; ++i) {
        if (runIsPrefixOfEntry(run, len, table[i]))
            return true;
    }
    return false;
}
} // namespace

void ComposeEngine::begin()
{
    m_active = true;
    m_length = 0;
    m_result = 0;
}

void ComposeEngine::reset()
{
    m_active = false;
    m_length = 0;
    m_result = 0;
}

ComposeResult ComposeEngine::feed(uint32_t codePoint)
{
    if (!m_active)
        return ComposeResult::Inactive;

    if (m_length >= maxSequenceLength) {
        // Run already at the cap with no match — can't extend further.
        reset();
        return ComposeResult::Invalid;
    }
    m_buffer[m_length++] = codePoint;

    // Exact match wins immediately.
    uint32_t resolved = 0;
    if (scanExact(composeTable, m_buffer, m_length, resolved)) {
        m_result = resolved;
        m_active = false;
        m_length = 0;
        return ComposeResult::Composed;
    }

    // No exact match — is the run still a viable prefix of some longer entry?
    if (scanPrefix(composeTable, m_buffer, m_length))
        return ComposeResult::Pending;

    // Dead end.
    reset();
    return ComposeResult::Invalid;
}

} // namespace Ubiquitty::Input
