// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// Tests/Core/Rendering/test_boxglyph_golden.cpp — golden / cross-frontend parity test
// for the SHARED box-drawing geometry (Source/Core/Rendering/BoxDrawCommands.h).
//
// buildBoxGlyph is the single source of box-drawing geometry for every consumer:
// the coverage-atlas sink (BoxGlyphRaster.cpp) and the box-fill overlay sink
// (BoxFillEmit.cpp OverlayBoxDrawSink, shared by every frontend) only PAINT the
// device-space draw commands the builder emits. This test drives buildBoxGlyph
// over a representative codepoint set into a command-recording sink and asserts
// a stable, committed golden of the exact fillRect / stipple / triangle /
// capsule commands (name + rounded device coordinates + parameters). It locks
// that shared geometry: if the builder (or a refactor that claims to be
// byte-equivalent) ever changes what commands are emitted, this test fails — so
// a sink-mapping regression vs. this reference is caught at the geometry seam,
// before any consumer's paint code.
//
// To regenerate the golden after a geometry change, run:
//   ./Scripts/run-tests.sh test_boxglyph_golden
// with PRINT=1 set in the environment (the harness prints the digest); paste the
// new block into expectedGolden below and re-run to confirm it matches.
//

#include "Core/Rendering/BoxDrawCommands.h"
#include "Core/Rendering/BoxGlyphRaster.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Ubiquitty::Rendering::BoxDrawSink;
using Ubiquitty::Rendering::buildBoxGlyph;

// A device cell rect chosen so the integer snapping in buildBoxGlyph produces
// stable, human-readable coordinates. Non-square (W != H) on purpose so the
// per-axis arc/diagonal scaling and the light/heavy weight (round(H/12)) are
// exercised. Origin offset so edge-anchored fills (L/T/R/B) differ from interior
// rounded splits.
constexpr float cellL = 10.0f;
constexpr float cellT = 20.0f;
constexpr float cellR = 10.0f + 18.0f; // cell width 18
constexpr float cellB = 20.0f + 32.0f; // cell height 32

// Records every command as one digest line: NAME args... with floats rounded to
// 0.01 px so the golden is exact yet stable across libm rounding noise.
class RecordingSink final : public BoxDrawSink {
public:
    struct Arc {
        Ubiquitty::FloatPoint center;
        Ubiquitty::FloatSize radii;
    };
    void fillRect(float x0, float y0, float x1, float y1, float alpha) override { appendf("fillRect %s %s %s %s a=%s", f(x0).c_str(), f(y0).c_str(), f(x1).c_str(), f(y1).c_str(), f(alpha).c_str()); }
    void stipple(float x0, float y0, float x1, float y1, float density, Ubiquitty::Rendering::ShadePolarity polarity) override { appendf("stipple %s %s %s %s d=%s%s", f(x0).c_str(), f(y0).c_str(), f(x1).c_str(), f(y1).c_str(), f(density).c_str(), polarity == Ubiquitty::Rendering::ShadePolarity::Inverse ? " inverse" : ""); }
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, float alpha) override { appendf("triangle %s %s %s %s %s %s a=%s", f(ax).c_str(), f(ay).c_str(), f(bx).c_str(), f(by).c_str(), f(cx).c_str(), f(cy).c_str(), f(alpha).c_str()); }
    void capsule(float ax, float ay, float bx, float by, float half, std::optional<Ubiquitty::FloatRect> clip) override
    {
        appendf("capsule %s %s %s %s h=%s", f(ax).c_str(), f(ay).c_str(), f(bx).c_str(), f(by).c_str(), f(half).c_str());
        if (clip)
            appendf("clip %s %s %s %s", f(clip->origin.x).c_str(), f(clip->origin.y).c_str(), f(clip->size.width).c_str(), f(clip->size.height).c_str());
    }

    void ellipticalArc(Ubiquitty::FloatRect cell, Ubiquitty::FloatPoint center, Ubiquitty::FloatSize radii, Ubiquitty::FloatSize halfWidths) override
    {
        m_arcs.push_back({ center, radii });
        appendf("ellipse %s %s radii=%s,%s half=%s,%s clip=%s,%s,%s,%s", f(center.x).c_str(), f(center.y).c_str(),
            f(radii.width).c_str(), f(radii.height).c_str(), f(halfWidths.width).c_str(), f(halfWidths.height).c_str(),
            f(cell.origin.x).c_str(), f(cell.origin.y).c_str(), f(cell.size.width).c_str(), f(cell.size.height).c_str());
    }

    [[nodiscard]] const std::vector<std::string>& lines() const { return m_lines; }
    [[nodiscard]] const std::vector<Arc>& arcs() const { return m_arcs; }

private:
    std::vector<Arc> m_arcs;
    static std::string f(float v)
    {
        // Round to 0.01 and normalise -0 so the digest is reproducible.
        float r = std::round(v * 100.0f) / 100.0f;
        if (r == 0.0f)
            r = 0.0f;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", r);
        return { buf };
    }
    void appendf(const char* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        m_lines.emplace_back(buf);
    }
    std::vector<std::string> m_lines;
};

// Representative codepoint set spanning every BoxDrawing primitive family:
// full / half blocks, shades, quadrants, a sextant, octants (2×4 fills),
// corners / tees / cross, heavy + double corners, rounded corners, and diagonals.
struct Case {
    unsigned cp;
    const char* name;
};
static const Case cases[] = {
    { 0x2588, "FULL BLOCK" },
    { 0x2580, "UPPER HALF BLOCK" },
    { 0x2584, "LOWER HALF BLOCK" },
    { 0x2591, "LIGHT SHADE" },
    { 0x2592, "MEDIUM SHADE" },
    { 0x2593, "DARK SHADE" },
    { 0x2596, "QUADRANT LOWER LEFT" },
    { 0x259B, "QUADRANT UL+UR+LL" },
    { 0x1FB00, "SEXTANT-1" },
    // Octants (U+1CD00..1CDE5): 2×4 sub-cell fills via fillRect. U+1CD00 is
    // OCTANT-3 (one sub-rect); U+1CDE5 is OCTANT-2345678 (seven sub-rects, the
    // densest octant) — together they pin the 2×4 grid's row splits.
    { 0x1CD00, "OCTANT-3" },
    { 0x1CDE5, "OCTANT-2345678" },
    { 0x250C, "LIGHT DOWN AND RIGHT (corner)" },
    { 0x2510, "LIGHT DOWN AND LEFT (corner)" },
    { 0x2514, "LIGHT UP AND RIGHT (corner)" },
    { 0x2518, "LIGHT UP AND LEFT (corner)" },
    { 0x251C, "LIGHT VERTICAL AND RIGHT (tee)" },
    { 0x2524, "LIGHT VERTICAL AND LEFT (tee)" },
    { 0x252C, "LIGHT DOWN AND HORIZONTAL (tee)" },
    { 0x2534, "LIGHT UP AND HORIZONTAL (tee)" },
    { 0x253C, "LIGHT VERTICAL AND HORIZONTAL (cross)" },
    { 0x250F, "HEAVY DOWN AND RIGHT" },
    { 0x2513, "HEAVY DOWN AND LEFT" },
    { 0x2554, "DOUBLE DOWN AND RIGHT" },
    { 0x2557, "DOUBLE DOWN AND LEFT" },
    { 0x256D, "ARC DOWN AND RIGHT (rounded)" },
    { 0x256E, "ARC DOWN AND LEFT (rounded)" },
    { 0x2570, "ARC UP AND RIGHT (rounded)" },
    { 0x256F, "ARC UP AND LEFT (rounded)" },
    { 0x2571, "DIAGONAL UPPER RIGHT TO LOWER LEFT" },
    { 0x2572, "DIAGONAL UPPER LEFT TO LOWER RIGHT" },
    { 0x2573, "DIAGONAL CROSS" },
    // Legacy-computing wedges (U+1FB3C..1FB47) decompose to filled Triangles —
    // the only family that emits the `triangle` draw command, so they pin the
    // most complex sink mapping (per-row scan-convert). Several orientations.
    { 0x1FB3C, "WEDGE 1FB3C (triangle)" },
    { 0x1FB3E, "WEDGE 1FB3E (triangle)" },
    { 0x1FB47, "WEDGE 1FB47 (triangle)" },
};

// The committed golden: for each case, "U+XXXX count\n" then the recorded
// command lines. Generated by the PRINT=1 dump below and pinned here.
static const char* expectedGolden =
#include "test_boxglyph_golden.inc"
    ;

TEST(boxglyphGolden)
{

    std::string actual;
    char hdr[64];
    for (const Case& c : cases) {
        RecordingSink sink;
        const bool ok = buildBoxGlyph(c.cp, { { cellL, cellT }, { cellR - cellL, cellB - cellT } }, cellB - cellT, nullptr, sink);
        std::snprintf(hdr, sizeof(hdr), "U+%04X %s%d\n", c.cp, ok ? "ok " : "none ", static_cast<int>(sink.lines().size()));
        actual += hdr;
        for (const std::string& line : sink.lines()) {
            actual += "  ";
            actual += line;
            actual += "\n";
        }
    }

    if (std::getenv("PRINT")) {
        // Dump in a C string-literal-friendly form for pasting into the .inc.
        std::fputs("---- BEGIN GOLDEN DUMP ----\n", stderr);
        std::fputs(actual.c_str(), stderr);
        std::fputs("---- END GOLDEN DUMP ----\n", stderr);
    }

    if (actual != expectedGolden) {
        CHECK_MSG(false, "shared buildBoxGlyph geometry does not match the golden dump");
        std::printf("FAIL test_boxglyph_golden: shared buildBoxGlyph geometry "
                    "drifted from the committed golden.\n");
        // Show the first differing line to make the drift obvious.
        std::vector<std::string> ga, gb;
        auto split = [](const std::string& s, std::vector<std::string>& out) {
            std::string current;
            for (char ch : s) {
                if (ch == '\n') {
                    out.push_back(current);
                    current.clear();
                } else
                    current += ch;
            }
            if (!current.empty())
                out.push_back(current);
        };
        split(actual, ga);
        split(expectedGolden, gb);
        const size_t n = ga.size() < gb.size() ? ga.size() : gb.size();
        for (size_t i = 0; i < n; ++i) {
            if (ga[i] != gb[i]) {
                std::printf("  first diff at line %zu:\n    actual:   %s\n"
                            "    expected: %s\n",
                    i, ga[i].c_str(), gb[i].c_str());
                break;
            }
        }
        if (ga.size() != gb.size())
            std::printf("  line count: actual=%zu expected=%zu\n", ga.size(), gb.size());
        std::printf("  (re-run with PRINT=1 to dump the full actual digest)\n");
    }
}

TEST(roundedCornersKeepCircularCurvatureAcrossCellAspectRatios)
{
    for (float width : { 9.0f, 18.0f, 32.0f }) {
        for (float height : { 9.0f, 18.0f, 32.0f }) {
            for (unsigned codePoint : { 0x256D, 0x256E, 0x256F, 0x2570 }) {
                RecordingSink sink;
                CHECK(buildBoxGlyph(codePoint, { { 0, 0 }, { width, height } }, 12, nullptr, sink));
                CHECK(sink.arcs().size() == 1);
                const auto& arc = sink.arcs().front();
                CHECK(arc.radii.width > 0);
                CHECK(std::abs(arc.radii.width - arc.radii.height) < 0.001f);
            }
        }
    }
}

TEST(roundedCornersHaveAntialiasedEdgesAndMatchStraightBands)
{
    for (int width : { 9, 13, 18 }) {
        for (int height : { 18, 26, 32 }) {
            std::vector<std::uint8_t> vertical(width * height), horizontal(width * height), corner(width * height);
            CHECK(Ubiquitty::Rendering::rasterizeBoxGlyph(0x2502, { width, height }, { width, height }, height, vertical));
            CHECK(Ubiquitty::Rendering::rasterizeBoxGlyph(0x2500, { width, height }, { width, height }, height, horizontal));
            for (unsigned codePoint : { 0x256D, 0x256E, 0x256F, 0x2570 }) {
                CHECK(Ubiquitty::Rendering::rasterizeBoxGlyph(codePoint, { width, height }, { width, height }, height, corner));
                bool hasAntialiasing = false;
                for (auto coverage : corner) {
                    hasAntialiasing |= coverage > 0 && coverage < 255;
                }
                CHECK(hasAntialiasing);
                const bool down = codePoint == 0x256D || codePoint == 0x256E;
                const bool isRight = codePoint == 0x256D || codePoint == 0x2570;
                for (int x = 0; x < width; ++x) {
                    const int index = (down ? height - 1 : 0) * width + x;
                    CHECK(std::abs(int(corner[index]) - int(vertical[index])) < 2);
                }
                for (int y = 0; y < height; ++y) {
                    const int index = y * width + (isRight ? width - 1 : 0);
                    CHECK(std::abs(int(corner[index]) - int(horizontal[index])) < 2);
                }
            }
        }
    }
}

TEST(roundedCornersPreserveFractionalFontBands)
{
    constexpr int width = 16, height = 32;
    Ubiquitty::Rendering::BoxDrawing::BoxStrokeProfile profile;
    profile.lightVertical = { true, 0.4375f, 1.5f / width };
    profile.lightHorizontal = { true, 0.40625f, 2.25f / height };
    std::vector<std::uint8_t> corner(width * height);
    CHECK(Ubiquitty::Rendering::rasterizeBoxGlyph(0x256D, { width, height }, { width, height }, height, corner, &profile));
    float verticalMass = 0, verticalMoment = 0, horizontalMass = 0, horizontalMoment = 0;
    for (int x = 0; x < width; ++x) {
        const float coverage = corner[(height - 1) * width + x] / 255.0f;
        verticalMass += coverage;
        verticalMoment += coverage * (x + 0.5f);
    }
    for (int y = 0; y < height; ++y) {
        const float coverage = corner[y * width + width - 1] / 255.0f;
        horizontalMass += coverage;
        horizontalMoment += coverage * (y + 0.5f);
    }
    CHECK(std::abs(verticalMoment / verticalMass - 7.0f) < 0.1f);
    CHECK(std::abs(horizontalMoment / horizontalMass - 13.0f) < 0.1f);
    CHECK(std::abs(verticalMass - 1.5f) < 0.1f);
    CHECK(std::abs(horizontalMass - 2.25f) < 0.1f);
}

// Whether a join's two strokes actually MEET, which neither the golden nor a
// count of commands can answer.
//
// `strokeBand` is asymmetric about the centre — a 3px stroke over an 18x32 cell
// is `[cx-2, cx+1]` — so two stubs that each stop at the centre POINT leave the
// square `[cx-2, cx] x [cy-2, cy]` covered by nothing, and every corner and tee
// is drawn with a bite out of it. The golden recorded that geometry happily for
// as long as it existed, because a notch is exactly as pinnable as a corner.
//
// Sampled rather than reasoned about: the rects are asked whether they cover
// each pixel of the centre band, which is the same question the screen asks and
// the only one that cannot be satisfied by a second copy of the formula.
// Whether a join's strokes actually MEET, asked of the PIXELS the shipping
// rasterizer produces.
//
// `rasterizeBoxGlyph` is the sink the Linux frontend blits into its atlas, so
// this asks the question at the surface a reader sees rather than at the command
// list. A sink of this test's own would answer for capsule coverage by its
// bounding box, which is exactly where a join's ink is thinnest — the question
// would be decided by the approximation instead of by the geometry.
//
// The probe lines come from rasterizing a straight rule over the same cell, so
// nothing here restates the stroke band or the rail gap: a second copy of that
// arithmetic would agree with a broken copy for the reason the golden does.
static std::vector<std::uint8_t> rasterize(unsigned codePoint, int width, int height)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width) * height, 0);
    if (!Ubiquitty::Rendering::rasterizeBoxGlyph(codePoint, { width, height }, { width, height }, static_cast<float>(height), { coverage.data(), static_cast<std::size_t>(width) * static_cast<std::size_t>(height) }))
        coverage.assign(coverage.size(), 0);
    return coverage;
}

// The columns a vertical rule inks, and the rows a horizontal one does. A double
// rule reports two of each, a light rule one.
static std::vector<int> inkedColumns(const std::vector<std::uint8_t>& coverage, int width, int height)
{
    std::vector<int> columns;
    for (int x = 0; x < width; ++x) {
        int lit = 0;
        for (int y = 0; y < height; ++y)
            lit += coverage[static_cast<std::size_t>(y) * width + x] > 128 ? 1 : 0;
        // Two thirds of the height, so a corner's short leg is not mistaken for a
        // rule running the length of the cell.
        if (lit * 3 >= height * 2)
            columns.push_back(x);
    }
    return columns;
}

static std::vector<int> inkedRows(const std::vector<std::uint8_t>& coverage, int width, int height)
{
    std::vector<int> rows;
    for (int y = 0; y < height; ++y) {
        int lit = 0;
        for (int x = 0; x < width; ++x)
            lit += coverage[static_cast<std::size_t>(y) * width + x] > 128 ? 1 : 0;
        if (lit * 3 >= width * 2)
            rows.push_back(y);
    }
    return rows;
}

// Every crossing of a rule's columns with a rule's rows must be inked in the
// join. For a light join that is one point; for a double join it is four, and
// each is a square both rails can decline — cut a rail at the NEAR edge of what
// it crosses and its partner does the same for its own reason, leaving every
// quadrant of a `╬` with a hollow corner.
static void checkJoinIsSolid(unsigned codePoint, const std::vector<int>& columns,
    const std::vector<int>& rows, int width, int height)
{
    const std::vector<std::uint8_t> coverage = rasterize(codePoint, width, height);
    for (int x : columns) {
        for (int y : rows) {
            char message[176];
            std::snprintf(message, sizeof(message),
                "U+%04X: a rule runs down column %d and along row %d, and the join leaves "
                "their crossing empty",
                codePoint, x, y);
            CHECK_MSG(coverage[static_cast<std::size_t>(y) * width + x] > 128, message);
        }
    }
}

TEST(joinsMeetWhereTheirStrokesCross)
{
    // Odd on both axes so the cell has a true centre pixel, and non-square so a
    // rule that only lands correctly on one axis is caught.
    constexpr int width = 19;
    constexpr int height = 33;

    const std::vector<int> lightColumns = inkedColumns(rasterize(0x2502, width, height), width, height);
    const std::vector<int> lightRows = inkedRows(rasterize(0x2500, width, height), width, height);
    CHECK(!lightColumns.empty());
    CHECK(!lightRows.empty());
    // Light and heavy corners, tees and crosses. A corner exercises one quadrant
    // of the centre, a tee two and a cross all four, so a rule that held for the
    // cross alone would leave three corners notched.
    const unsigned lightJoins[] = {
        0x250C,
        0x2510,
        0x2514,
        0x2518,
        0x251C,
        0x2524,
        0x252C,
        0x2534,
        0x253C,
        0x250F,
        0x2513,
        0x2517,
        0x251B,
        0x2523,
        0x252B,
        0x2533,
        0x253B,
        0x254B,
    };
    for (unsigned codePoint : lightJoins)
        checkJoinIsSolid(codePoint, lightColumns, lightRows, width, height);

    // The double family reads its own rails: a double rule inks two columns, and
    // its joins must be solid at all four crossings.
    const std::vector<int> doubleColumns = inkedColumns(rasterize(0x2551, width, height), width, height);
    const std::vector<int> doubleRows = inkedRows(rasterize(0x2550, width, height), width, height);
    CHECK(!doubleColumns.empty());
    CHECK(!doubleRows.empty());
    const unsigned doubleJoins[] = {
        0x2554,
        0x2557,
        0x255A,
        0x255D,
        0x2560,
        0x2563,
        0x2566,
        0x2569,
        0x256C,
    };
    for (unsigned codePoint : doubleJoins)
        checkJoinIsSolid(codePoint, doubleColumns, doubleRows, width, height);
}

UBIQUITTY_TEST_MAIN()
