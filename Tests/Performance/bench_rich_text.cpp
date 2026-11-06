// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_rich_text.cpp — the rich text block's measured paths (Docs/rich-text.md,
// "Performance"), emitting one JSON object via BenchHarness:
//
//   rich_text_layout_40x6        laying a heading and a paragraph out in a 40x6
//                                block, from the RTF reader's document
//   rich_text_hit_test_10000     ten thousand pointer positions resolved to a
//                                character over that layout
//   rich_text_fit_query_2kib     the fit query over a 2 KiB body: read, laid
//                                out and answered
//
// Pure CPU against a synthetic typography table: no renderer, no fonts.
//
//   bench_rich_text [--platform NAME] [--commit SHA] [--iters N]

#include "BenchHarness.h"

#include "Core/RichText/Layout.h"
#include "Core/RichText/RTFReader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty::RichText;
using namespace Ubiquitty::Bench;

TypographyFacts benchFacts()
{
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.faceCount = 4;
    for (std::uint8_t index = 0; index < 4; ++index) {
        FaceMetrics& face = facts.faces[index];
        face.styles = index;
        face.ascent = 1900;
        face.descent = 500;
        face.lineHeight = 2400;
        face.averageAdvance = 1000;
        for (std::size_t advance = 0; advance < tabledCodePointCount; ++advance)
            face.advances[advance] = static_cast<std::uint16_t>(700 + (advance * 37) % 600);
    }
    return facts;
}

Document read(const std::string& body)
{
    auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    return document ? std::move(*document) : Document {};
}

std::string headingAndParagraph()
{
    return "{\\rtf1{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\colortbl;\\red0\\green102\\blue204;}\\f0\\fs36\\b\\cf1 Release notes\\par"
           "\\fs24\\b0\\cf0 The build now signs every artefact and verifies the chain before upload, so a release that "
           "reaches a mirror is the release the pipeline produced and nothing else.}";
}

std::string twoKibibyteBody()
{
    std::string body = "{\\rtf1\\fs24 ";
    const std::string sentence = "A paragraph of prose with \\b bold\\b0 , \\i italic\\i0  and plain words that wraps. ";
    while (body.size() + sentence.size() < 2048 - 1)
        body += sentence;
    body += "}";
    return body;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace Ubiquitty::Bench;
    using namespace Ubiquitty::RichText;
    const char* platform = "";
    const char* commit = "";
    int iters = 21;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "bench_rich_text: unrecognised argument \"%s\"\n", argv[i]);
            std::fprintf(stderr, "  usage: --platform NAME --commit SHA --iters N\n");
            return 2;
        }
    }

    const TypographyFacts facts = benchFacts();
    const Document heading = read(headingAndParagraph());
    std::vector<Result> results;

    const Timing layout = measure(5, iters, [&] {
        Layout laid = Layout::lay(heading, 40, 6, Overflow::Wrap, Alignment::Left, &facts);
        doNotOptimize(laid);
    });
    results.push_back({ "rich_text_layout_40x6", "ns", layout.medianSeconds * 1e9, layout.p95Seconds * 1e9 });

    const Layout laid = Layout::lay(heading, 40, 6, Overflow::Wrap, Alignment::Left, &facts);
    const std::uint32_t width = laid.blockWidth();
    const std::uint32_t height = laid.blockHeight();
    const Timing hits = measure(5, iters, [&] {
        std::uint32_t sum = 0;
        for (std::uint32_t point = 0; point < 10000; ++point) {
            const std::int64_t x = static_cast<std::int64_t>((point * 7919u) % width);
            const std::int64_t y = static_cast<std::int64_t>((point * 104729u) % height);
            sum += laid.characterAt(x, y);
        }
        doNotOptimize(sum);
    });
    results.push_back({ "rich_text_hit_test_10000", "ns", hits.medianSeconds * 1e9, hits.p95Seconds * 1e9 });

    const std::string body = twoKibibyteBody();
    const Timing fit = measure(5, iters, [&] {
        auto document = readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
        if (document) {
            Layout measured = Layout::lay(*document, 80, 24, Overflow::Wrap, Alignment::Left, &facts);
            doNotOptimize(measured);
        }
    });
    results.push_back({ "rich_text_fit_query_2kib", "ns", fit.medianSeconds * 1e9, fit.p95Seconds * 1e9 });

    emitJSON(results, platform, commit);
    return 0;
}
