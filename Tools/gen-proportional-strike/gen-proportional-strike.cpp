// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tools/gen-proportional-strike/gen-proportional-strike.cpp — bake Noto Sans
// into the bitmap tier's proportional face, Source/Core/Rendering/
// ProportionalStrikeData.{h,cpp}. Run in the Linux container, where FreeType
// and HarfBuzz are the ones the Linux arm renders with:
//
//   ./Scripts/run-linux.sh sh -c 'c++ -std=c++20 -O2 \
//       Tools/gen-proportional-strike/gen-proportional-strike.cpp \
//       $(pkg-config --cflags --libs freetype2 harfbuzz) -lz \
//       -o build/gen-proportional-strike && build/gen-proportional-strike'
//
// The generated files are checked in so a clean build needs no font library
// and no font at run time, the way Unifont is baked by Scripts/gen-unifont.py.
//
// Font: Noto Sans Regular and Bold (Resources/fonts, SIL Open Font License
// 1.1, Resources/fonts/NotoSans-OFL.txt), at 12 px for bitmap-tier scale 1 and
// 24 px for scale 2, each on Unifont's cell: 16 rows per scale with the
// baseline 13.5 rows down, floored. Scales past two upscale the 24 px strike
// as Unifont's own glyphs upscale.
//
// What the strike records is what the Linux arm's FreeType rasterizer draws for
// a proportional run: advances and kerning pairs
// from HarfBuzz over the FreeType face in 1/64 px, and a raster of each glyph
// at every subpixel phase Core places at, light-hinted with the phase's
// fraction folded into the outline before rasterising. A run drawn from it
// carries a fractional pen and picks the raster for the pen's phase, so the
// bitmap tier's text is FreeType's at that size, kerned as HarfBuzz kerns it,
// and the same bytes on every arm.
//
// The inflated blob is a stream of strikes, each:
//   [u8 face 0=Regular 1=Bold][u8 scale][u8 phases][u8 overhang]
//   [u32 LE glyphCount][u32 LE kernCount]
//   glyphCount records: [u16 LE codePoint][u16 LE advance in 1/64 px]
//                       then `phases` rasters, each
//                       [i8 left][i8 top][u8 width][u8 height][width * height coverage]
//                       where top counts rows from the baseline up to the
//                       raster's first row, as FreeType's bitmap_top does.
//   kernCount records:  [u16 LE first][u16 LE second][i16 LE kern in 1/64 px]
// `overhang` is the rows the face reaches past Unifont's cell above or below,
// the larger of the two; a strip slot grows by it on both sides.
// Records are sorted by code point; kern pairs by (first, second).
#if __has_include(<ft2build.h>) && __has_include(<hb.h>)

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include <hb-ft.h>
#include <hb.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int emPerScale = 12;
constexpr int boxRowsPerScale = 16;
constexpr double baselineRowsPerScale = 13.5;
constexpr int phases = 8; // Core/Rendering/SubpixelPhase.h subpixelPhaseCount
const int scales[] = { 1, 2 };
const std::pair<int, const char*> faces[] = { { 0, "NotoSans-Regular.ttf" }, { 1, "NotoSans-Bold.ttf" } };

// The code points the strike carries: Latin with its first extension, Greek,
// Cyrillic and the punctuation, currency, arrow and mathematical blocks a
// label reaches for. A code point outside these draws in the grid face, as
// every arm's fallback does. The Cyrillic combining marks are left out: a
// mark has no advance of its own and its ink reaches rows no letter does.
const std::pair<unsigned, unsigned> ranges[] = { { 0x20, 0x17F }, { 0x370, 0x3FF }, { 0x400, 0x482 }, { 0x48A, 0x4FF },
    { 0x2000, 0x206F }, { 0x20A0, 0x20CF }, { 0x2190, 0x21FF }, { 0x2200, 0x22FF } };

// Kerning pairs are taken among the letters and punctuation a label kerns:
// printable ASCII, the Latin-1 letters, Greek and Cyrillic.
std::vector<unsigned> kernSet()
{
    std::vector<unsigned> set;
    for (unsigned c = 0x21; c < 0x7F; ++c)
        set.push_back(c);
    for (unsigned c = 0xC0; c < 0x100; ++c)
        if (c != 0xD7 && c != 0xF7)
            set.push_back(c);
    for (unsigned c = 0x391; c <= 0x3A9; ++c)
        set.push_back(c);
    for (unsigned c = 0x3B1; c <= 0x3C9; ++c)
        set.push_back(c);
    for (unsigned c = 0x410; c <= 0x44F; ++c)
        set.push_back(c);
    return set;
}

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

// One HarfBuzz shape of `text` at the face's current size; the positions.
std::vector<hb_glyph_position_t> shape(hb_font_t* font, const std::vector<unsigned>& text, unsigned* glyphCountOut)
{
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf32(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<hb_glyph_position_t> out(positions, positions + count);
    hb_buffer_destroy(buffer);
    *glyphCountOut = count;
    return out;
}

struct Raster {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> coverage;
};

// The glyph at one subpixel phase, as the Linux arm draws it: the fraction
// goes into the outline before rasterising and hinting is light.
Raster raster(FT_Face face, unsigned glyphIndex, int phase)
{
    Raster out;
    FT_Vector delta { static_cast<FT_Pos>(phase * 64 / phases), 0 };
    FT_Set_Transform(face, nullptr, &delta);
    const bool isLoaded = FT_Load_Glyph(face, glyphIndex, FT_LOAD_TARGET_LIGHT) == 0
        && (face->glyph->format == FT_GLYPH_FORMAT_BITMAP || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0);
    FT_Set_Transform(face, nullptr, nullptr);
    if (!isLoaded)
        return out;
    const FT_GlyphSlot slot = face->glyph;
    out.left = slot->bitmap_left;
    out.top = slot->bitmap_top;
    out.width = static_cast<int>(slot->bitmap.width);
    out.height = static_cast<int>(slot->bitmap.rows);
    out.coverage.reserve(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    for (int y = 0; y < out.height; ++y) {
        const unsigned char* row = slot->bitmap.buffer + static_cast<long>(y) * slot->bitmap.pitch;
        out.coverage.insert(out.coverage.end(), row, row + out.width);
    }
    return out;
}

struct StrikeReport {
    std::string line;
};

bool strike(FT_Library library, int faceIndex, const std::string& path, int scale, std::vector<unsigned char>& stream, StrikeReport& report)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0) {
        std::fprintf(stderr, "gen-proportional-strike: cannot open %s\n", path.c_str());
        return false;
    }
    const int em = emPerScale * scale;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(em));
    // The shaper over the face exactly as FreeTypeRasterizer builds its
    // proportional shaper, so the advances are the Linux arm's.
    hb_font_t* font = hb_ft_font_create_referenced(face);
    const int box = boxRowsPerScale * scale;
    const int baselineRows = static_cast<int>(baselineRowsPerScale * scale);

    struct Glyph {
        unsigned codePoint;
        unsigned advance;
        std::vector<Raster> rasters;
    };
    std::vector<Glyph> glyphs;
    int overhang = 0;
    for (const auto& [low, high] : ranges) {
        for (unsigned codePoint = low; codePoint <= high; ++codePoint) {
            const unsigned glyphIndex = FT_Get_Char_Index(face, codePoint);
            if (glyphIndex == 0)
                continue; // the face has no glyph; the fallback draws it
            unsigned count = 0;
            const std::vector<hb_glyph_position_t> positions = shape(font, { codePoint }, &count);
            if (count != 1 || positions[0].x_advance < 0 || positions[0].x_advance > 0xFFFF)
                continue;
            Glyph glyph { codePoint, static_cast<unsigned>(positions[0].x_advance), {} };
            for (int phase = 0; phase < phases; ++phase) {
                Raster r = raster(face, glyphIndex, phase);
                if (r.width > 255 || r.height > 255 || r.left < -128 || r.left > 127 || r.top < -128 || r.top > 127) {
                    std::fprintf(stderr, "gen-proportional-strike: U+%04X does not fit the record at scale %d\n", codePoint, scale);
                    return false;
                }
                if (r.height > 0) {
                    overhang = std::max(overhang, r.top - baselineRows);
                    overhang = std::max(overhang, (baselineRows - r.top + r.height) - box);
                }
                glyph.rasters.push_back(std::move(r));
            }
            glyphs.push_back(std::move(glyph));
        }
    }

    struct Kern {
        unsigned first;
        unsigned second;
        int kern;
    };
    std::vector<Kern> kerns;
    const std::vector<unsigned> set = kernSet();
    std::vector<int> alone(set.size(), -1);
    for (std::size_t i = 0; i < set.size(); ++i) {
        unsigned count = 0;
        const std::vector<hb_glyph_position_t> positions = shape(font, { set[i] }, &count);
        if (count == 1)
            alone[i] = positions[0].x_advance;
    }
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (alone[i] < 0)
            continue;
        for (std::size_t j = 0; j < set.size(); ++j) {
            if (alone[j] < 0)
                continue;
            unsigned count = 0;
            const std::vector<hb_glyph_position_t> positions = shape(font, { set[i], set[j] }, &count);
            // A pair the shaper joins into one glyph (fi, fl) has no kern to
            // record: the strike draws by code point and the fallback keeps
            // the two letters apart.
            if (count != 2)
                continue;
            const int kern = positions[0].x_advance - alone[i];
            if (kern != 0 && kern >= -32768 && kern <= 32767)
                kerns.push_back({ set[i], set[j], kern });
        }
    }

    const std::size_t start = stream.size();
    stream.push_back(static_cast<unsigned char>(faceIndex));
    stream.push_back(static_cast<unsigned char>(scale));
    stream.push_back(static_cast<unsigned char>(phases));
    stream.push_back(static_cast<unsigned char>(overhang));
    put32(stream, static_cast<unsigned>(glyphs.size()));
    put32(stream, static_cast<unsigned>(kerns.size()));
    for (const Glyph& glyph : glyphs) {
        put16(stream, glyph.codePoint);
        put16(stream, glyph.advance);
        for (const Raster& r : glyph.rasters) {
            stream.push_back(static_cast<unsigned char>(static_cast<signed char>(r.left)));
            stream.push_back(static_cast<unsigned char>(static_cast<signed char>(r.top)));
            stream.push_back(static_cast<unsigned char>(r.width));
            stream.push_back(static_cast<unsigned char>(r.height));
            stream.insert(stream.end(), r.coverage.begin(), r.coverage.end());
        }
    }
    for (const Kern& kern : kerns) {
        put16(stream, kern.first);
        put16(stream, kern.second);
        put16(stream, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(kern.kern))));
    }
    report.line = path.substr(path.rfind('/') + 1) + " x" + std::to_string(scale) + ": " + std::to_string(glyphs.size()) + " glyphs, "
        + std::to_string(kerns.size()) + " kern pairs, overhang " + std::to_string(overhang) + ", " + std::to_string(stream.size() - start) + " bytes";
    hb_font_destroy(font);
    FT_Done_Face(face);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        std::fprintf(stderr, "gen-proportional-strike: FreeType did not initialise\n");
        return 1;
    }
    std::vector<unsigned char> stream;
    std::vector<StrikeReport> reports;
    for (const auto& [faceIndex, name] : faces) {
        for (const int scale : scales) {
            StrikeReport report;
            if (!strike(library, faceIndex, root + "/Resources/fonts/" + name, scale, stream, report))
                return 1;
            reports.push_back(report);
        }
    }
    uLongf compressedLength = compressBound(static_cast<uLong>(stream.size()));
    std::vector<unsigned char> compressed(compressedLength);
    if (compress2(compressed.data(), &compressedLength, stream.data(), static_cast<uLong>(stream.size()), 9) != Z_OK) {
        std::fprintf(stderr, "gen-proportional-strike: zlib refused the stream\n");
        return 1;
    }
    compressed.resize(compressedLength);

    const std::string out = root + "/Source/Core/Rendering/";
    FILE* source = std::fopen((out + "ProportionalStrikeData.cpp").c_str(), "w");
    FILE* header = std::fopen((out + "ProportionalStrikeData.h").c_str(), "w");
    if (!source || !header) {
        std::fprintf(stderr, "gen-proportional-strike: cannot write under %s\n", out.c_str());
        return 1;
    }
    std::fprintf(source,
        "// Copyright (C) 2026 Stanislav Yaglo\n"
        "// Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic)\n"
        "// SPDX-License-Identifier: Apache-2.0 AND OFL-1.1\n\n"
        "//\n"
        "// Source/Core/Rendering/ProportionalStrikeData.cpp — Noto Sans, Regular and\n"
        "// Bold, pre-rendered at 12 px and 24 px into the bitmap tier's proportional\n"
        "// face; generated by Tools/gen-proportional-strike. Do not edit.\n"
        "//\n"
        "// Font: Noto Sans (Google), SIL Open Font License 1.1 — Resources/fonts/NotoSans-OFL.txt.\n"
        "//\n");
    for (const StrikeReport& report : reports)
        std::fprintf(source, "// %s\n", report.line.c_str());
    std::fprintf(source,
        "\n#include \"Core/Rendering/ProportionalStrikeData.h\"\n\n"
        "namespace Ubiquitty::Rendering {\n\n"
        "const unsigned int proportionalStrikeBlobLength = %zu;\n"
        "const unsigned int proportionalStrikeCompressedLength = %zu;\n"
        "const unsigned char proportionalStrikeCompressed[] = {\n",
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
        "/// The bitmap tier's proportional face, Noto Sans pre-rendered at two\n"
        "/// scales and every subpixel phase, as a zlib stream; the layout is stated\n"
        "/// in Tools/gen-proportional-strike/gen-proportional-strike.cpp, which\n"
        "/// writes it.\n"
        "extern const unsigned int proportionalStrikeBlobLength;\n"
        "extern const unsigned int proportionalStrikeCompressedLength;\n"
        "extern const unsigned char proportionalStrikeCompressed[];\n\n"
        "} // namespace Ubiquitty::Rendering\n",
        header);
    std::fclose(header);
    for (const StrikeReport& report : reports)
        std::printf("%s\n", report.line.c_str());
    std::printf("stream %zu bytes, compressed %zu bytes\n", stream.size(), compressed.size());
    FT_Done_FreeType(library);
    return 0;
}

#else

#include <cstdio>

int main()
{
    std::fputs("gen-proportional-strike: FreeType and HarfBuzz headers are not on this host; run it in the Linux container (see the file's header)\n", stderr);
    return 2;
}

#endif
