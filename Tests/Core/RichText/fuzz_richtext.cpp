// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/RichText/fuzz_richtext.cpp — libFuzzer harness for the RTF reader.
//
// An RTF body is untrusted: it arrives in an OSC 7404 payload straight off the
// terminal byte stream. The reader walks attacker-shaped groups, control-word
// parameters, `\u` surrogates and fallbacks, and the font and colour tables.
// A body the reader accepts must satisfy every invariant the model and the
// wire rely on, so the harness checks them on each accepted document rather
// than only surviving the parse.

#include "Core/RichText/RTFReader.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace {

/// Ends the process on a broken invariant in every configuration, so the
/// replayed corpus asserts in the release builds that compile `assert` out.
void require(bool condition) // bool-ok: the predicate an assertion asserts
{
    if (!condition)
        __builtin_trap();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace Ubiquitty::RichText;
    const auto document = readRTF(std::span(data, size));
    if (!document)
        return 0;
    require(isWellFormed(*document));
    require(size <= maxBodyBytes);
    require(document->text.size() <= static_cast<std::size_t>(maxCodePoints));
    require(document->runs.size() <= static_cast<std::size_t>(maxRuns));
    require(document->fontCount() <= maxFonts);
    require(document->colorCount <= maxColors);
    for (const std::uint32_t codePoint : document->text)
        require(codePoint != 0 && Ubiquitty::Unicode::isScalarValue(codePoint));
    // Runs tile the text in order, each at least one code point long, and no
    // two neighbours carry the same style.
    std::uint32_t next = 0;
    for (std::size_t index = 0; index < document->runs.size(); ++index) {
        const Run& run = document->runs[index];
        require(run.start == next);
        require(run.length >= 1);
        require(run.halfPoints >= minHalfPoints && run.halfPoints <= maxHalfPoints);
        require(run.font == noFontEntry || run.font < document->fontCount());
        require(run.color == 0 || (run.color < document->colorCount && !document->colors[run.color].isAutomatic));
        require((run.styles & ~allStyleBits) == 0);
        if (index) {
            Run previous = document->runs[index - 1];
            previous.start = run.start;
            previous.length = run.length;
            require(!(previous == run));
        }
        next = run.start + run.length;
    }
    require(next == document->text.size());
    for (const FontEntry& font : document->fontTable())
        require(font.nameLength <= maxFamilyNameBytes);
    return 0;
}
