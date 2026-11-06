// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The portable platform-font inventory seam, plus the fake inventory harness.
// Header-only and platform-free.

#include "Harness.h"

#include "Core/Rendering/Font/FontInventory.h"

#include <cstdio>

using namespace Ubiquitty::Rendering::Font;

namespace {

constexpr FontHandle primaryHandle { 1, 1 };
constexpr FontHandle japaneseHanHandle { 2, 1 };
constexpr FontHandle symbolHandle { 3, 1 };
constexpr FontHandle genericHandle { 4, 1 };
constexpr FontHandle lastResortHandle { 5, 1 };

class FakeInventory final : public FontInventory {
public:
    [[nodiscard]] std::uint32_t catalogGeneration() const override { return m_generation; }
    void bumpCatalogGeneration() override { ++m_generation; }

    FontHandle primaryFont(FontStyle, std::uint8_t) override { return primaryHandle; }

    FontHandle validateFamily(const FontFamilyRequest& request) override
    {
        if (request.family.empty())
            return {};
        if (same(request.family, "Hiragino Sans"))
            return japaneseHanHandle;
        if (same(request.family, "Symbols Nerd Font Mono"))
            return symbolHandle;
        if (same(request.family, "LastResort"))
            return lastResortHandle;
        return {};
    }

    GlyphProbeResult glyphForCodePoint(FontHandle font, std::uint32_t codePoint) override
    {
        if (font == primaryHandle && codePoint == 'A')
            return { font, { 65 }, true, false };
        if (font == japaneseHanHandle && codePoint == 0x6771)
            return { font, { 1200 }, true, false };
        if (font == symbolHandle && codePoint == 0xE0B0)
            return { font, { 300 }, true, false };
        if (font == lastResortHandle)
            return { font, { 1 }, true, true };
        return { font, { 0 }, false, false };
    }

    GlyphProbeResult glyphForCluster(FontHandle font, std::span<const std::uint32_t> codePoints, Presentation) override
    {
        return codePoints.empty() ? GlyphProbeResult {} : glyphForCodePoint(font, codePoints.front());
    }

    CascadeResult cascadeForCodePoint(const CascadeRequest& request) override
    {
        ++singleCascadeCalls;
        if (request.codePoints.empty())
            return {};
        if (request.codePoints[0] == 0x6771)
            return { japaneseHanHandle, { 1200 }, true, false };
        if (request.codePoints[0] == 0x10FFFF)
            return { lastResortHandle, { 1 }, true, true };
        return { genericHandle, { 900 }, true, false };
    }

    CascadeResult cascadeForRun(const CascadeRequest& request) override
    {
        ++runCascadeCalls;
        if (request.shapedRun && request.codePoints.size() > 1)
            return { genericHandle, { 901 }, true, false };
        return cascadeForCodePoint(request);
    }

    [[nodiscard]] bool isLastResort(FontHandle font) const override { return font == lastResortHandle; }

    [[nodiscard]] std::string_view familyName(FontHandle font) const override
    {
        if (font == primaryHandle)
            return "JetBrains Mono";
        if (font == japaneseHanHandle)
            return "Hiragino Sans";
        if (font == symbolHandle)
            return "Symbols Nerd Font Mono";
        if (font == genericHandle)
            return "Generic Fallback";
        if (font == lastResortHandle)
            return "LastResort";
        return {};
    }

    [[nodiscard]] std::string_view postScriptName(FontHandle font) const override
    {
        if (font == japaneseHanHandle)
            return "HiraginoSans-W3";
        return familyName(font);
    }

    int singleCascadeCalls { 0 };
    int runCascadeCalls { 0 };

private:
    static bool same(std::string_view left, const char* right) { return left == right; }

    std::uint32_t m_generation { 1 };
};

} // namespace

TEST(testFakeInventoryBasics)
{
    FakeInventory inventory;
    CHECK(inventory.catalogGeneration() == 1);
    inventory.bumpCatalogGeneration();
    CHECK(inventory.catalogGeneration() == 2);

    FontHandle primary = inventory.primaryFont(FontStyle::Regular, 0);
    CHECK(primary.isValid());
    CHECK(inventory.glyphForCodePoint(primary, 'A').present);
    CHECK(!inventory.glyphForCodePoint(primary, 0x6771).present);
}

TEST(testCustomFamilyAndLastResort)
{
    FakeInventory inventory;
    FontHandle han = inventory.validateFamily({ "Hiragino Sans", FontStyle::Regular, 0 });
    CHECK(han.isValid());
    CHECK(inventory.glyphForCodePoint(han, 0x6771).glyph.value == 1200);
    CHECK(!inventory.validateFamily({ "Missing Family", FontStyle::Regular, 0 }).isValid());

    FontHandle lastResort = inventory.validateFamily({ "LastResort", FontStyle::Regular, 0 });
    CHECK(lastResort.isValid());
    CHECK(inventory.isLastResort(lastResort));
    CHECK(inventory.glyphForCodePoint(lastResort, 0x6771).lastResort);
}

TEST(testCascadeCounters)
{
    FakeInventory inventory;
    std::uint32_t han[] { 0x6771 };
    CascadeRequest request;
    request.baseFont = inventory.primaryFont(FontStyle::Regular, 0);
    request.codePoints = std::span<const std::uint32_t>(han, 1);

    CascadeResult result = inventory.cascadeForCodePoint(request);
    CHECK(result.present);
    CHECK(result.font == japaneseHanHandle);
    CHECK(inventory.singleCascadeCalls == 1);

    std::uint32_t arabicRun[] { 0x0633, 0x0644, 0x0627 };
    request.codePoints = std::span<const std::uint32_t>(arabicRun, 3);
    request.shapedRun = true;
    result = inventory.cascadeForRun(request);
    CHECK(result.present);
    CHECK(result.font == genericHandle);
    CHECK(inventory.runCascadeCalls == 1);
}

UBIQUITTY_TEST_MAIN()
