// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pastenormalize.cpp — host tests for the portable paste rewrite
// (Source/Core/Input/PasteNormalize.{h,cpp}).
//
// Pins the security-relevant Ubiquitty::Input::wrapPaste contract that both the macOS and Linux
// frontends route through:
//   - an embedded ESC[200~ / ESC[201~ fence marker is neutralised (its trailing
//     '~' dropped) so a hostile clipboard can neither open paste mode with no
//     close nor smuggle an early end-of-paste and inject the remainder as keys;
//   - this neutralisation happens even when bracketed-paste is Disabled;
//   - CR, LF, CRLF, and LFCR all collapse to a single CR;
//   - Enabled wraps the (neutralised) body in exactly one balanced
//     ESC[200~ … ESC[201~ fence; Disabled returns the neutralised body bare;
//   - Ubiquitty::Input::normalizePaste + Ubiquitty::Input::wrapPaste compose to the identical bytes (the byte-for-
//     byte cross-frontend parity guarantee), proving the macOS in-place path
//     and the Linux string path land on one result.

#include "Core/Input/PasteNormalize.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Input::BracketedPaste;
using Ubiquitty::Input::normalizePaste;
using Ubiquitty::Input::wrapPaste;

// Render a byte string with control bytes escaped so failures are legible.
static std::string escaped(std::string_view bytes)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const char raw : bytes) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            out += "\\x";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

#define CHECK_NORMALIZED_EQ(actual, expected, msg)                \
    do {                                                          \
        const std::string a_ = (actual);                          \
        const std::string e_ = (expected);                        \
        if (a_ != e_) {                                           \
            char ubqMessage[512];                                 \
            std::snprintf(ubqMessage, sizeof(ubqMessage),         \
                "%s\n  got:      \"%s\"\n  expected: \"%s\"",     \
                (msg), escaped(a_).c_str(), escaped(e_).c_str()); \
            CHECK_MSG(false, ubqMessage);                         \
            return;                                               \
        }                                                         \
    } while (0)

// Enabled mode wraps a plain payload in exactly one balanced fence.
TEST(testEnabledWrapsPlain) { CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("hi", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~hi\x1b[201~", "plain bracketed"); }

// Disabled mode passes a plain payload through unwrapped.
TEST(testDisabledPassesThrough) { CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("hi", Ubiquitty::Input::BracketedPaste::Disabled), "hi", "plain unwrapped"); }

// An embedded ESC[200~ (paste-start) inside the clipboard must be neutralised —
// otherwise it flips the receiver into paste mode with no matching close. The
// trailing '~' is dropped, leaving an inert "ESC[200".
TEST(testEmbeddedStartMarkerNeutralizedEnabled)
{
    // Payload smuggles a paste-start; the wrap fence must remain the only live
    // ESC[200~ in the output, and the embedded one must be defused.
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b[200~b", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~a\x1b[200b\x1b[201~", "embedded 200~ defused, fence intact");
}

// The 201~ (paste-end) smuggling vector: an embedded end marker is defused so
// the pasted text cannot close the fence early.
TEST(testEmbeddedEndMarkerNeutralizedEnabled) { CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b[201~b", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~a\x1b[201b\x1b[201~", "embedded 201~ defused, fence intact"); }

// Even with bracketed-paste Disabled (no wrap), a raw ESC[200~ in the payload
// would otherwise reach the terminal verbatim and open paste mode with no
// close. Neutralisation must not be gated on the wrap.
TEST(testEmbeddedMarkersNeutralizedWhenDisabled)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b[200~b", Ubiquitty::Input::BracketedPaste::Disabled), "a\x1b[200b", "embedded 200~ defused even unwrapped");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b[201~b", Ubiquitty::Input::BracketedPaste::Disabled), "a\x1b[201b", "embedded 201~ defused even unwrapped");
}

// Multiple embedded markers are each neutralised.
TEST(testC1FenceNeutralizedEnabled)
{
    // The same fence spelled with the single-byte C1 CSI (0x9B): a receiver
    // honouring 8-bit controls reads it as ESC[, so it neutralises the same
    // way — trailing '~' dropped, the rest inert.
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x9b"
                                                    "200~b",
                            Ubiquitty::Input::BracketedPaste::Enabled),
        "\x1b[200~a\x9b"
        "200b\x1b[201~",
        "C1 fence defused inside the fence");
}

TEST(testC1IntroducersDroppedWhenDisabled)
{
    // With bracketed paste off the bytes land as raw keys, and the C1 bytes
    // that introduce control sequences (DCS, SOS, CSI, OSC, PM, APC) are the
    // one-byte spellings of the injection the C0 strip guards. The rest of the
    // C1 range survives for legacy single-byte encodings.
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x90"
                                                    "b\x98"
                                                    "c\x9b"
                                                    "d\x9d"
                                                    "e\x9e"
                                                    "f\x9f"
                                                    "g",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        "abcdefg", "sequence introducers dropped raw");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("caf\x82 \x91"
                                                    "quote\x92",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        "caf\x82 \x91"
        "quote\x92",
        "non-introducer high bytes survive");
}

TEST(testMultipleEmbeddedMarkers) { CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("\x1b[200~\x1b[201~\x1b[200~", Ubiquitty::Input::BracketedPaste::Disabled), "\x1b[200\x1b[201\x1b[200", "every embedded marker defused"); }

// CR/LF normalisation: CR, LF, CRLF, and LFCR each collapse to a single CR, in
// both modes (the body inside the fence is normalised for Enabled).
TEST(testCRLFNormalizationDisabled)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\nb", Ubiquitty::Input::BracketedPaste::Disabled), "a\rb", "LF -> CR");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\rb", Ubiquitty::Input::BracketedPaste::Disabled), "a\rb", "CR -> CR");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\r\nb", Ubiquitty::Input::BracketedPaste::Disabled), "a\rb", "CRLF -> CR");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\n\rb", Ubiquitty::Input::BracketedPaste::Disabled), "a\rb", "LFCR -> CR");
}

TEST(testCRLFNormalizationEnabled)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\r\nb", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~a\rb\x1b[201~", "CRLF -> CR inside fence");
    // A run of distinct line endings collapses pairwise: CRLF then LFCR.
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("\r\n\n\r", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~\r\r\x1b[201~", "two line endings -> two CRs inside fence");
}

// Empty payload: Disabled is empty, Enabled is just the empty fence.
TEST(testEmptyPayload)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("", Ubiquitty::Input::BracketedPaste::Disabled), "", "empty unwrapped");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("", Ubiquitty::Input::BracketedPaste::Enabled), "\x1b[200~\x1b[201~", "empty fence");
}

// A truncated marker (no trailing '~', or cut off at the buffer end) is not a
// fence and must pass through untouched — only a complete ESC[20X~ is defused.
TEST(testIncompleteMarkerPassesThrough)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b[200", Ubiquitty::Input::BracketedPaste::Disabled), "a\x1b[200", "incomplete marker (no ~) untouched");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("\x1b[2", Ubiquitty::Input::BracketedPaste::Disabled), "\x1b[2", "short escape untouched");
}

// With bracketed-paste Disabled the body reaches the app as raw keystrokes, so
// dangerous C0 controls (and DEL) must be stripped to prevent paste-injection;
// TAB is preserved and CR/LF still collapse to CR.
TEST(testControlCharactersStrippedWhenDisabled)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x07"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        "ab", "BEL stripped");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x01\x02"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        "ab", "Ctrl-A/Ctrl-B stripped");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x7f"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        "ab", "DEL stripped");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\tb", Ubiquitty::Input::BracketedPaste::Disabled), "a\tb", "TAB preserved");
    // ESC is not stripped here — the fence-neutralisation layer owns it
    // (so an inert ESC[200/201 survives), and stripping it would break that.
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x1b"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Disabled),
        ("a\x1b"
         "b"),
        "bare ESC left to the fence layer");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("x\ny", Ubiquitty::Input::BracketedPaste::Disabled), "x\ry", "LF still collapses to CR");
}

// With bracketed-paste Enabled the fence makes the body inert data, so controls
// pass through literally (only an embedded fence marker is still defused).
TEST(testControlCharactersPreservedWhenEnabled)
{
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x07"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Enabled),
        "\x1b[200~a\x07"
        "b\x1b[201~",
        "BEL kept inside fence");
    CHECK_NORMALIZED_EQ(Ubiquitty::Input::wrapPaste("a\x01"
                                                    "b",
                            Ubiquitty::Input::BracketedPaste::Enabled),
        "\x1b[200~a\x01"
        "b\x1b[201~",
        "Ctrl-A kept inside fence");
}

// Cross-frontend parity: the macOS path runs Ubiquitty::Input::normalizePaste in place, then
// Ubiquitty::Input::wrapPaste; the Linux path runs Ubiquitty::Input::wrapPaste directly on the raw payload. Both
// must produce identical bytes for any input — Ubiquitty::Input::wrapPaste re-normalises
// idempotently so pre-normalising can't change the result.
TEST(testNormalizeThenWrapMatchesWrapOnly)
{
    const char* inputs[] = {
        "plain text",
        "a\r\nb\n\rc",
        "x\x1b[200~y\x1b[201~z",
        "\x1b[200~",
        "line1\nline2\nline3",
        ("ctl\x01\x07\x1b\x7f"
         "more\there"),
        "",
    };
    const Ubiquitty::Input::BracketedPaste modes[] = { Ubiquitty::Input::BracketedPaste::Disabled, Ubiquitty::Input::BracketedPaste::Enabled };
    for (const char* input : inputs) {
        for (const Ubiquitty::Input::BracketedPaste mode : modes) {
            const std::string raw(input);
            // macOS path: Ubiquitty::Input::normalizePaste rewrites in place first.
            std::string buffer(input);
            const long olen = buffer.empty() ? 0 : Ubiquitty::Input::normalizePaste(buffer.data(), static_cast<long>(buffer.size()), mode);
            const std::string macResult = Ubiquitty::Input::wrapPaste(std::string_view(buffer.data(), static_cast<size_t>(olen)), mode);
            // Linux path: Ubiquitty::Input::wrapPaste straight off the raw payload.
            const std::string linuxResult = Ubiquitty::Input::wrapPaste(raw, mode);
            CHECK_NORMALIZED_EQ(macResult, linuxResult, "macOS and Linux paths byte-identical");
        }
    }
}

// The count that decides whether a paste is held for confirmation. A shell
// runs one command per line, so the question this answers is how many commands
// a confirmed paste would run.
TEST(testPasteLineCount)
{
    using Ubiquitty::Input::pasteLineCount;

    CHECK(pasteLineCount("") == 0);
    CHECK(pasteLineCount("one") == 1);

    // A trailing break closes the last line rather than opening an empty one:
    // "echo hi\n" runs one command, not two.
    CHECK(pasteLineCount("one\n") == 1);
    CHECK(pasteLineCount("one\r") == 1);
    CHECK(pasteLineCount("one\r\n") == 1);

    CHECK(pasteLineCount("one\ntwo") == 2);
    CHECK(pasteLineCount("one\ntwo\n") == 2);

    // CRLF is one break. Counting the LF separately would double every count
    // from a Windows clipboard and hold pastes the threshold allows.
    CHECK(pasteLineCount("a\r\nb\r\nc") == 3);
    CHECK(pasteLineCount("a\nb\nc") == 3);
    CHECK(pasteLineCount("a\r\nb\r\nc") == pasteLineCount("a\nb\nc"));

    // A lone LF is a line: an empty first line still ends in a break the shell
    // acts on.
    CHECK(pasteLineCount("\n") == 1);
    CHECK(pasteLineCount("\n\n") == 2);

    // A break in the middle of the text is not a trailing one.
    CHECK(pasteLineCount("a\n\nb") == 3);
}

TEST(lineCountsMatchNormalizedNewlinePairs)
{
    using Ubiquitty::Input::pasteLineCount;
    for (const auto* input : { "a\n\rb", "a\n\r", "\n\r", "\r\n\r", "\n\r\n", "a\n\r\n\rb", "a\r\r\nb", "a\n\n\rb" }) {
        std::string normalized(input);
        normalized.resize(static_cast<std::size_t>(normalizePaste(normalized.data(), static_cast<long>(normalized.size()), BracketedPaste::Enabled)));
        CHECK(pasteLineCount(input) == pasteLineCount(normalized));
    }
    CHECK(pasteLineCount("a\n\rb") == 2);
    CHECK(pasteLineCount("\n\r\n") == 2);
    CHECK(pasteLineCount("\r\n\r") == 2);
}

namespace {

/// The whole buffer normalised at once.
std::string normalizedWhole(std::string_view text, BracketedPaste bracketed)
{
    std::string whole(text);
    whole.resize(static_cast<std::size_t>(normalizePaste(whole.data(), static_cast<long>(whole.size()), bracketed)));
    return whole;
}

/// The same buffer normalised in pieces cut at the given offsets.
std::string normalizedInPieces(std::string_view text, BracketedPaste bracketed, const std::vector<std::size_t>& cuts)
{
    Ubiquitty::Input::PasteNormalizer normalizer(bracketed);
    std::string out;
    std::size_t start = 0;
    for (const std::size_t cut : cuts) {
        out += normalizer.push(text.substr(start, cut - start));
        start = cut;
    }
    out += normalizer.push(text.substr(start));
    out += normalizer.finish();
    return out;
}

} // namespace

// A paste normalised in pieces, wherever they are cut, is the paste
// normalised whole: a CR/LF pair and a fence across a cut are one.
TEST(streamedNormalisationEqualsWholeBufferNormalisation)
{
    const std::string alphabet = std::string("ab\r\n\t\x1b[201~\x03\x7f") + "\x9b" + "\xe2\x82\xac";
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int round = 0; round < 4000; ++round) {
        std::string text;
        const std::size_t length = next() % 40;
        for (std::size_t index = 0; index < length; ++index) {
            // Whole fences now and then, so they straddle cuts.
            if (next() % 9 == 0)
                text += next() % 2 ? "\x1b[201~" : "\x9b"
                                                   "200~";
            else
                text.push_back(alphabet[next() % alphabet.size()]);
        }
        std::vector<std::size_t> cuts;
        for (std::size_t at = 1; at < text.size(); ++at) {
            if (next() % 3 == 0)
                cuts.push_back(at);
        }
        for (const BracketedPaste bracketed : { BracketedPaste::Enabled, BracketedPaste::Disabled }) {
            const std::string whole = normalizedWhole(text, bracketed);
            const std::string pieces = normalizedInPieces(text, bracketed, cuts);
            CHECK(pieces == whole);
            if (pieces != whole)
                return;
        }
    }
}

// The end-of-paste marker split across two pieces at every byte cannot close
// the bracket early, in either spelling, bracketed or not.
TEST(anEndMarkerSplitAtEveryOffsetIsNeutralised)
{
    for (const std::string_view marker : { std::string_view("\x1b[201~"), std::string_view("\x9b"
                                                                                           "201~") }) {
        const std::string text = std::string("before") + std::string(marker) + "after";
        for (std::size_t cut = 1; cut < text.size(); ++cut) {
            for (const BracketedPaste bracketed : { BracketedPaste::Enabled, BracketedPaste::Disabled }) {
                const std::string out = normalizedInPieces(text, bracketed, { cut });
                CHECK(out.find("\x1b[201~") == std::string::npos);
                CHECK(out.find("\x9b"
                               "201~")
                    == std::string::npos);
                CHECK(out == normalizedWhole(text, bracketed));
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()

TEST(readlinePasteQuotesBytesAndPreservesLiteralNewlines)
{
    using Ubiquitty::Input::PasteOptions;
    const PasteOptions quoted { .shouldQuoteCharacters = true };
    CHECK_NORMALIZED_EQ(wrapPaste("A\n\x03\xc3\xa9", BracketedPaste::Disabled, quoted),
        "\x16"
        "A\x16\r\x16\x03\x16\xc3\x16\xa9",
        "quote every byte after newline normalization");
    CHECK_NORMALIZED_EQ(wrapPaste("A\n", BracketedPaste::Enabled, quoted),
        "\x1b[200~\x16"
        "A\x16\r\x1b[201~",
        "generated fences are not quoted");
    const PasteOptions literal { .shouldPreserveNewlines = true };
    CHECK_NORMALIZED_EQ(wrapPaste("a\r\nb\n\rc", BracketedPaste::Disabled, literal),
        "a\r\nb\n\rc", "preserve both bytes of newline pairs");
    CHECK_NORMALIZED_EQ(wrapPaste("A\n", BracketedPaste::Disabled,
                            { .shouldQuoteCharacters = true, .shouldPreserveNewlines = true }),
        "\x16"
        "A\x16\n",
        "quote literal LF");
}

TEST(readlinePasteModesSurviveEveryChunkBoundary)
{
    const std::string text = "a\r\n\x1b[200~b\x1b[201~\n\r\x03\xc3\xa9";
    for (const auto bracketed : { BracketedPaste::Disabled, BracketedPaste::Enabled }) {
        for (const auto quotes : { false, true }) {
            for (const auto literal : { false, true }) {
                const Ubiquitty::Input::PasteOptions options { .shouldQuoteCharacters = quotes, .shouldPreserveNewlines = literal };
                const auto expected = wrapPaste(text, bracketed, options);
                for (std::size_t split = 0; split <= text.size(); ++split) {
                    Ubiquitty::Input::PasteNormalizer normalizer(bracketed, options);
                    std::string actual = bracketed == BracketedPaste::Enabled ? "\x1b[200~" : "";
                    actual += normalizer.push(std::string_view(text).substr(0, split));
                    const auto state = normalizer.continuation();
                    REQUIRE(state);
                    Ubiquitty::Input::PasteNormalizer restored(BracketedPaste::Disabled);
                    REQUIRE(restored.restoreContinuation(*state));
                    actual += restored.push(std::string_view(text).substr(split));
                    actual += restored.finish();
                    if (bracketed == BracketedPaste::Enabled)
                        actual += "\x1b[201~";
                    CHECK_NORMALIZED_EQ(actual, expected, "chunk boundary and restore preserve the paste modes");
                }
            }
        }
    }
}
