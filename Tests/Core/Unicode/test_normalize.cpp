// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Unicode normalization (NFC/NFD/NFKC/NFKD) per UAX #15. Pins token parsing,
// base+combining compose/decompose, canonical mark ordering, algorithmic Hangul,
// compatibility (ligature/circled-digit) folding, and byte-for-byte passthrough
// of invalid UTF-8.

#include "Harness.h"

#include "Core/Unicode/Normalize.h"
#include "Core/Unicode/UTF8Encoder.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::Unicode::appendUTF8;
using Ubiquitty::Unicode::NormalizationForm;
using Ubiquitty::Unicode::normalizationFormFromName;
using Ubiquitty::Unicode::normalizationFormToName;
using Ubiquitty::Unicode::normalizeUTF8;

// Build a UTF-8 string from a code-point list.
static std::string utf8(std::initializer_list<std::uint32_t> cps)
{
    std::string s;
    for (std::uint32_t c : cps)
        appendUTF8(s, c);
    return s;
}

#define CHECK_FORM(input, form, expected)                                                   \
    do {                                                                                    \
        std::string got = normalizeUTF8((input), (form));                                   \
        if (got != (expected)) {                                                            \
            std::printf("FAIL %s:%d normalize(%s): mismatch\n", __FILE__, __LINE__, #form); \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

TEST(normalizeCases)
{

    // ---- token parsing --------------------------------------------------
    CHECK(normalizationFormFromName("none") == NormalizationForm::None);
    CHECK(normalizationFormFromName("nfc") == NormalizationForm::NFC);
    CHECK(normalizationFormFromName("NFD") == NormalizationForm::NFD);
    CHECK(normalizationFormFromName("nf-kc") == NormalizationForm::NFKC); // separator-insensitive
    CHECK(normalizationFormFromName("NFKD") == NormalizationForm::NFKD);
    CHECK(!normalizationFormFromName("bogus").has_value()); // unknown returns nullopt
    CHECK(std::string(normalizationFormToName(NormalizationForm::NFC)) == "nfc");
    CHECK(std::string(normalizationFormToName(NormalizationForm::None)) == "none");

    // ---- None is an identity no-op --------------------------------------
    const std::string decomposedE = utf8({ 0x0065, 0x0301 }); // e + COMBINING ACUTE
    CHECK_FORM(decomposedE, NormalizationForm::None, decomposedE);
    CHECK_FORM("plain ascii", NormalizationForm::NFC, std::string("plain ascii"));

    // ---- base + combining mark: NFC composes, NFD decomposes ------------
    const std::string precomposedE = utf8({ 0x00E9 }); // é (LATIN SMALL LETTER E WITH ACUTE)
    CHECK_FORM(decomposedE, NormalizationForm::NFC, precomposedE); // e + ´  -> é
    CHECK_FORM(precomposedE, NormalizationForm::NFC, precomposedE); // idempotent
    CHECK_FORM(precomposedE, NormalizationForm::NFD, decomposedE); // é -> e + ´
    CHECK_FORM(decomposedE, NormalizationForm::NFD, decomposedE); // idempotent

    // A word: "café" precomposed vs decomposed converge under each form.
    const std::string cafePre = utf8({ 'c', 'a', 'f', 0x00E9 });
    const std::string cafeDecomp = utf8({ 'c', 'a', 'f', 0x0065, 0x0301 });
    CHECK_FORM(cafeDecomp, NormalizationForm::NFC, cafePre);
    CHECK_FORM(cafePre, NormalizationForm::NFD, cafeDecomp);

    // ---- canonical ordering of multiple combining marks ----------------
    // U+1E0B U+0323 (canonical) — combining marks reorder by combining class.
    // q + COMBINING DOT BELOW (ccc 220) + COMBINING DOT ABOVE (ccc 230):
    // NFD canonical order keeps 220 before 230 regardless of input order.
    const std::string marksInOrder = utf8({ 'q', 0x0323, 0x0307 });
    const std::string marksReversed = utf8({ 'q', 0x0307, 0x0323 });
    CHECK_FORM(marksReversed, NormalizationForm::NFD, marksInOrder);
    CHECK_FORM(marksInOrder, NormalizationForm::NFD, marksInOrder);

    // ---- Hangul (algorithmic) ------------------------------------------
    // U+AC00 (가) = U+1100 (ᄀ) + U+1161 (ᅡ).
    const std::string hangulComposed = utf8({ 0xAC00 });
    const std::string hangulDecomposed = utf8({ 0x1100, 0x1161 });
    CHECK_FORM(hangulComposed, NormalizationForm::NFD, hangulDecomposed);
    CHECK_FORM(hangulDecomposed, NormalizationForm::NFC, hangulComposed);
    // LVT syllable U+AC01 (각) = L U+1100 + V U+1161 + T U+11A8.
    CHECK_FORM(utf8({ 0xAC01 }), NormalizationForm::NFD, utf8({ 0x1100, 0x1161, 0x11A8 }));
    CHECK_FORM(utf8({ 0x1100, 0x1161, 0x11A8 }), NormalizationForm::NFC, utf8({ 0xAC01 }));

    // ---- compatibility forms (NFKC / NFKD) -----------------------------
    // U+FB01 (ﬁ LATIN SMALL LIGATURE FI) -> "fi" under compatibility.
    CHECK_FORM(utf8({ 0xFB01 }), NormalizationForm::NFKC, std::string("fi"));
    CHECK_FORM(utf8({ 0xFB01 }), NormalizationForm::NFKD, std::string("fi"));
    // U+FB01 has no canonical decomposition: NFC / NFD leave it unchanged.
    CHECK_FORM(utf8({ 0xFB01 }), NormalizationForm::NFC, utf8({ 0xFB01 }));
    CHECK_FORM(utf8({ 0xFB01 }), NormalizationForm::NFD, utf8({ 0xFB01 }));
    // U+2460 (① circled digit one) -> "1" under compatibility only.
    CHECK_FORM(utf8({ 0x2460 }), NormalizationForm::NFKC, std::string("1"));
    CHECK_FORM(utf8({ 0x2460 }), NormalizationForm::NFKD, std::string("1"));
    // Nested compatibility: U+0385 (GREEK DIALYTIKA TONOS) NFKD = 0020 0308 0301.
    CHECK_FORM(utf8({ 0x0385 }), NormalizationForm::NFKD, utf8({ 0x0020, 0x0308, 0x0301 }));

    // ---- invalid UTF-8 survives byte-for-byte --------------------------
    // A lone 0xFF (never valid UTF-8) is passed through unchanged so search
    // text is never corrupted on bad input.
    std::string bad;
    bad.push_back('a');
    bad.push_back(static_cast<char>(0xFF));
    bad.push_back('b');
    CHECK(normalizeUTF8(bad, NormalizationForm::NFC).size() == 3);
    CHECK(normalizeUTF8(bad, NormalizationForm::NFC)[1] == static_cast<char>(0xFF));
}

UBIQUITTY_TEST_MAIN()
