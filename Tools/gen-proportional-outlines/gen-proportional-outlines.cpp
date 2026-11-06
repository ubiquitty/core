// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tools/gen-proportional-outlines/gen-proportional-outlines.cpp — bake the
// outlines of the bitmap tier's proportional face, Noto Sans Regular and
// Bold, into Source/Core/Rendering/ProportionalOutlineData.{h,cpp}, so Core
// draws that face at any size the strikes do not hold (the 12 px and 24 px
// strikes of Tools/gen-proportional-strike) rather than magnifying a strike's
// pixels:
//
//   c++ -std=c++20 -O2 Tools/gen-proportional-outlines/gen-proportional-outlines.cpp \
//       $(pkg-config --cflags --libs freetype2) -lz -o build/gen-proportional-outlines \
//       && build/gen-proportional-outlines \
//       && clang-format -i Source/Core/Rendering/ProportionalOutlineData.{h,cpp}
//
// An outline is the font's own data, the same on every host, so unlike the
// strikes it needs no Linux container: no hinting or rasterisation runs here.
// The generated files are checked in so a clean build needs no font library.
//
// Font: Noto Sans Regular and Bold (Resources/fonts, SIL Open Font License
// 1.1, Resources/fonts/NotoSans-OFL.txt), the code points the strikes carry.
//
// The inflated blob is a stream of faces, each:
//   [u8 face 0=Regular 1=Bold][u16 LE unitsPerEm][u32 LE glyphCount]
//   glyphCount records: [u16 LE codePoint][u16 LE contourCount][u16 LE pointCount]
//                       contourCount × [u16 LE last point index of the contour]
//                       pointCount × [i16 LE x][i16 LE y] in font units, y up
//                       pointCount × [u8 1 on the curve, 0 a quadratic control]
// Records are sorted by code point.
#if __has_include(<ft2build.h>)

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

const std::pair<int, const char*> faces[] = { { 0, "NotoSans-Regular.ttf" }, { 1, "NotoSans-Bold.ttf" } };

// The strikes' code points (Tools/gen-proportional-strike), so a code point
// the strikes draw has an outline and one they do not falls back the same way.
const std::pair<unsigned, unsigned> ranges[] = { { 0x20, 0x17F }, { 0x370, 0x3FF }, { 0x400, 0x482 }, { 0x48A, 0x4FF },
    { 0x2000, 0x206F }, { 0x20A0, 0x20CF }, { 0x2190, 0x21FF }, { 0x2200, 0x22FF } };

void put16(std::vector<unsigned char>& out, unsigned value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void put32(std::vector<unsigned char>& out, unsigned value)
{
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

bool face(FT_Library library, int faceIndex, const std::string& path, std::vector<unsigned char>& stream, std::string& report)
{
    FT_Face fontFace = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &fontFace) != 0) {
        std::fprintf(stderr, "gen-proportional-outlines: cannot open %s\n", path.c_str());
        return false;
    }
    struct Glyph {
        unsigned codePoint;
        std::vector<unsigned> ends;
        std::vector<std::pair<int, int>> points;
        std::vector<unsigned char> isOnCurve;
    };
    std::vector<Glyph> glyphs;
    for (const auto& [low, high] : ranges) {
        for (unsigned codePoint = low; codePoint <= high; ++codePoint) {
            const unsigned glyphIndex = FT_Get_Char_Index(fontFace, codePoint);
            if (glyphIndex == 0)
                continue;
            if (FT_Load_Glyph(fontFace, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0 || fontFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
                continue;
            const FT_Outline& outline = fontFace->glyph->outline;
            Glyph glyph { codePoint, {}, {}, {} };
            for (int contour = 0; contour < outline.n_contours; ++contour)
                glyph.ends.push_back(static_cast<unsigned>(outline.contours[contour]));
            for (int point = 0; point < outline.n_points; ++point) {
                const char tag = FT_CURVE_TAG(outline.tags[point]);
                if (tag == FT_CURVE_TAG_CUBIC) {
                    std::fprintf(stderr, "gen-proportional-outlines: U+%04X has a cubic segment\n", codePoint);
                    return false;
                }
                glyph.points.emplace_back(static_cast<int>(outline.points[point].x), static_cast<int>(outline.points[point].y));
                glyph.isOnCurve.push_back(tag == FT_CURVE_TAG_ON ? 1 : 0);
            }
            glyphs.push_back(std::move(glyph));
        }
    }
    const std::size_t start = stream.size();
    stream.push_back(static_cast<unsigned char>(faceIndex));
    put16(stream, fontFace->units_per_EM);
    put32(stream, static_cast<unsigned>(glyphs.size()));
    for (const Glyph& glyph : glyphs) {
        put16(stream, glyph.codePoint);
        put16(stream, static_cast<unsigned>(glyph.ends.size()));
        put16(stream, static_cast<unsigned>(glyph.points.size()));
        for (const unsigned end : glyph.ends)
            put16(stream, end);
        for (const auto& [x, y] : glyph.points) {
            put16(stream, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(x))));
            put16(stream, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(y))));
        }
        stream.insert(stream.end(), glyph.isOnCurve.begin(), glyph.isOnCurve.end());
    }
    report = path.substr(path.rfind('/') + 1) + ": " + std::to_string(glyphs.size()) + " outlines, " + std::to_string(fontFace->units_per_EM)
        + " units per em, " + std::to_string(stream.size() - start) + " bytes";
    FT_Done_Face(fontFace);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        std::fprintf(stderr, "gen-proportional-outlines: FreeType did not initialise\n");
        return 1;
    }
    std::vector<unsigned char> stream;
    std::vector<std::string> reports;
    for (const auto& [faceIndex, name] : faces) {
        std::string report;
        if (!face(library, faceIndex, root + "/Resources/fonts/" + name, stream, report))
            return 1;
        reports.push_back(report);
    }
    uLongf compressedLength = compressBound(static_cast<uLong>(stream.size()));
    std::vector<unsigned char> compressed(compressedLength);
    if (compress2(compressed.data(), &compressedLength, stream.data(), static_cast<uLong>(stream.size()), 9) != Z_OK) {
        std::fprintf(stderr, "gen-proportional-outlines: zlib refused the stream\n");
        return 1;
    }
    compressed.resize(compressedLength);

    const std::string out = root + "/Source/Core/Rendering/";
    FILE* source = std::fopen((out + "ProportionalOutlineData.cpp").c_str(), "w");
    FILE* header = std::fopen((out + "ProportionalOutlineData.h").c_str(), "w");
    if (!source || !header) {
        std::fprintf(stderr, "gen-proportional-outlines: cannot write under %s\n", out.c_str());
        return 1;
    }
    std::fprintf(source,
        "// Copyright (C) 2026 Stanislav Yaglo\n"
        "// Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic)\n"
        "// SPDX-License-Identifier: Apache-2.0 AND OFL-1.1\n\n"
        "//\n"
        "// Source/Core/Rendering/ProportionalOutlineData.cpp — the outlines of Noto\n"
        "// Sans, Regular and Bold, the bitmap tier's proportional face, for the sizes\n"
        "// its strikes do not hold; generated by Tools/gen-proportional-outlines.\n"
        "// Do not edit.\n"
        "//\n"
        "// Font: Noto Sans (Google), SIL Open Font License 1.1 — Resources/fonts/NotoSans-OFL.txt.\n"
        "//\n");
    for (const std::string& report : reports)
        std::fprintf(source, "// %s\n", report.c_str());
    std::fprintf(source,
        "\n#include \"Core/Rendering/ProportionalOutlineData.h\"\n\n"
        "namespace Ubiquitty::Rendering {\n\n"
        "const unsigned int proportionalOutlineBlobLength = %zu;\n"
        "const unsigned int proportionalOutlineCompressedLength = %zu;\n"
        "const unsigned char proportionalOutlineCompressed[] = {\n",
        stream.size(), compressed.size());
    for (std::size_t i = 0; i < compressed.size(); i += 24) {
        std::fputs("    ", source);
        for (std::size_t j = i; j < std::min(compressed.size(), i + 24); ++j)
            std::fprintf(source, "0x%02x,%s", compressed[j], j + 1 < std::min(compressed.size(), i + 24) ? " " : "");
        std::fputs("\n", source);
    }
    std::fputs("};\n\n} // namespace Ubiquitty::Rendering\n", source);
    std::fclose(source);
    std::fputs(
        "// Copyright (C) 2026 Stanislav Yaglo\n"
        "// SPDX-License-Identifier: Apache-2.0\n\n"
        "#pragma once\n\n"
        "namespace Ubiquitty::Rendering {\n\n"
        "/// The outlines of the bitmap tier's proportional face, Noto Sans Regular\n"
        "/// and Bold, as a zlib stream; the layout is stated in\n"
        "/// Tools/gen-proportional-outlines/gen-proportional-outlines.cpp, which\n"
        "/// writes it.\n"
        "extern const unsigned int proportionalOutlineBlobLength;\n"
        "extern const unsigned int proportionalOutlineCompressedLength;\n"
        "extern const unsigned char proportionalOutlineCompressed[];\n\n"
        "} // namespace Ubiquitty::Rendering\n",
        header);
    std::fclose(header);
    for (const std::string& report : reports)
        std::printf("%s\n", report.c_str());
    std::printf("stream %zu bytes, compressed %zu bytes\n", stream.size(), compressed.size());
    FT_Done_FreeType(library);
    return 0;
}

#else

#include <cstdio>

int main()
{
    std::fputs("gen-proportional-outlines: the FreeType headers are not on this host\n", stderr);
    return 2;
}

#endif
