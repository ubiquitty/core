// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/RichText/Blocks.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <cstddef>
#include <new>
#include <string_view>
#include <utility>

namespace {

struct AllocationRefusal {
    int remaining = -1;
    bool didRefuse = false;

    static AllocationRefusal& singleton()
    {
        static AllocationRefusal state;
        return state;
    }

    bool shouldRefuse()
    {
        if (remaining < 0)
            return false;
        if (remaining-- > 0)
            return false;
        didRefuse = true;
        return true;
    }
};

Ubiquitty::RichText::Document document(std::string_view body)
{
    auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    CHECK(parsed);
    return parsed ? std::move(*parsed) : Ubiquitty::RichText::Document {};
}

} // namespace

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (AllocationRefusal::singleton().shouldRefuse())
        return nullptr;
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (AllocationRefusal::singleton().shouldRefuse())
        return nullptr;
    return ::operator new[](size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(refusedTextAndStyleAllocationsReturnAParseError)
{
    constexpr std::string_view source = "{\\rtf1 normal\\b bold}";
    for (int allocation = 0; allocation < 2; ++allocation) {
        auto& refusal = AllocationRefusal::singleton();
        refusal = { .remaining = allocation };
        auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
        refusal.remaining = -1;
        CHECK(refusal.didRefuse);
        CHECK(!parsed && parsed.error() == Ubiquitty::RichText::ReadError::AllocationRefused);
    }
    auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    CHECK(parsed);
}

TEST(refusedFontTableGrowthReturnsAParseError)
{
    // The source reserves text once, then grows the font table for each entry.
    constexpr std::string_view source = R"({\rtf1{\fonttbl{\f0 First;}{\f1 Second;}}})";
    for (int allocation = 1; allocation <= 2; ++allocation) {
        auto& refusal = AllocationRefusal::singleton();
        refusal = { .remaining = allocation };
        auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
        refusal.remaining = -1;
        CHECK(refusal.didRefuse);
        CHECK(!parsed && parsed.error() == Ubiquitty::RichText::ReadError::AllocationRefused);
    }
    const auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(parsed);
    CHECK(parsed->fontCount() == 2);
    CHECK(parsed->fonts[0].familyName() == "First");
    CHECK(parsed->fonts[1].familyName() == "Second");
}

TEST(refusedFontTableParsingKeepsThePreviousPlacementAndCursor)
{
    using namespace Ubiquitty::RichText;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 6, 30);
    REQUIRE(placeBlock(terminal, { .id = 4, .width = 10, .height = 1, .cursor = CursorPolicy::Stay }, document("{\\rtf1 original}")));
    const auto held = terminal.richTextBlocks;
    constexpr std::string_view replacement = "\x1b]7404;w=10:h=1:i=4;{\\rtf1{\\fonttbl{\\f0 Replacement;}}new}\x1b\\";
    auto& refusal = AllocationRefusal::singleton();
    refusal = { .remaining = 1 };
    UbiquittyTest::feed(terminal, replacement);
    refusal.remaining = -1;
    CHECK(refusal.didRefuse);
    CHECK(terminal.richTextBlocks == held);
    CHECK(terminal.screens.active->cursorX == 0);
    CHECK(terminal.screens.active->cursorY == 0);
    UbiquittyTest::feed(terminal, replacement);
    const auto blocks = terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks.front().document.fontCount() == 1);
    CHECK(blocks.front().document.fonts[0].familyName() == "Replacement");
}

TEST(refusedRectangleStagingKeepsReplacedContentAndHistory)
{
    using namespace Ubiquitty::RichText;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 6, 30);
    UbiquittyTest::feed(terminal, "old history\r\n\x1b[6;1H\n\x1b[1;1H");
    const Placement original { .id = 4, .width = 10, .height = 1, .cursor = CursorPolicy::Stay };
    REQUIRE(placeBlock(terminal, original, document("{\\rtf1 original}")));
    const auto held = terminal.richTextBlocks;
    const auto history = terminal.scrollbackCount;
    auto replacement = document("{\\rtf1 replacement}");
    Placement placement { .id = 4, .width = 10, .height = 1 };
    REQUIRE(placement.visibleRects.tryPushBack({ 0, 0, 10, 1 }));
    auto& refusal = AllocationRefusal::singleton();
    refusal = { .remaining = 0 };
    const auto placed = placeBlock(terminal, placement, std::move(replacement));
    refusal.remaining = -1;
    CHECK(refusal.didRefuse);
    CHECK(!placed && placed.error() == PlacementError::Limit);
    CHECK(terminal.richTextBlocks == held);
    CHECK_EQ(terminal.scrollbackCount, history);
    CHECK_EQ(terminal.screens.active->cursorX, 0);
    CHECK_EQ(terminal.screens.active->cursorY, 0);
    CHECK(placeBlock(terminal, placement, document("{\\rtf1 replacement}")));
}

TEST(refusedBlockReservationKeepsTheGridAndSerial)
{
    using namespace Ubiquitty::RichText;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 6, 30);
    REQUIRE(placeBlock(terminal, { .id = 1, .width = 10, .height = 1 }, document("{\\rtf1 original}")));
    UbiquittyTest::feed(terminal, "preserved\r");
    const auto held = terminal.richTextBlocks;
    auto incoming = document("{\\rtf1 incoming}");
    const Placement placement { .id = 2, .width = 10, .height = 1 };
    auto& refusal = AllocationRefusal::singleton();
    refusal = { .remaining = 0 };
    const auto placed = placeBlock(terminal, placement, std::move(incoming));
    refusal.remaining = -1;
    CHECK(refusal.didRefuse);
    CHECK(!placed && placed.error() == PlacementError::Limit);
    CHECK(terminal.richTextBlocks == held);
    CHECK_EQ(UbiquittyTest::cell(terminal, 1, 0)->codePoint, std::uint32_t { 'p' });
    CHECK_EQ(terminal.screens.active->cursorY, 1);
    CHECK(placeBlock(terminal, placement, document("{\\rtf1 incoming}")));
    CHECK_EQ(terminal.richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 2 });
}

UBIQUITTY_TEST_MAIN()
